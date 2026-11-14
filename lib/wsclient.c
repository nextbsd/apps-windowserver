/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * libWindowServerClient: connects to WindowServer over its Mach service
 * and the per-client Unix socket (see WSProtocol.h).
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <WindowServer/WSProtocol.h>
#include <WindowServer/WSClient.h>

#define WS_API __attribute__((visibility("default")))

#define RPC_TIMEOUT_MS		10000
#define WELCOME_TIMEOUT_MS	5000

/* A capture stream's frame buffer, mapped (WS_EVENT_CAPTURE_BUFFER). */
struct ws_capture_map {
	uint32_t	stream;
	uint32_t	index;
	uint32_t	generation;
	uint32_t	stride;
	void		*data;
	size_t		size;
	int		held;		/* a frame in it is the client's */
	int		gone;		/* superseded or the stream ended:
					   unmap when released */
	struct ws_capture_map *next;
};

struct ws_connection {
	mach_port_t	server;
	mach_port_t	reply;
	uint32_t	client_id;
	uint64_t	token;
	uint32_t	app_id;		/* from the hello reply, 0 if unknown */
	uint32_t	seq;
	uint32_t	server_flags;
	int		sock;
	int		dead;
	uint32_t	next_buffer_id;
	/* window ids reserved for ws_window_create_nowait */
	uint32_t	wid_next;
	uint32_t	wid_end;
	ws_screen_info_t screen;
	ws_rect_t	usable;		/* WS_SERVER_USABLE_AREA, else the
					   whole screen */
	uint32_t	menu_bar;	/* WS_SERVER_MENU_BAR, else 0 */
	/* events read while waiting for something else */
	ws_event_t	*queue;
	size_t		qhead;
	size_t		qlen;
	size_t		qcap;
	/* bytes read from the stream socket, not yet a whole event */
	uint8_t		rbuf[4096];
	size_t		rlen;
	/* descriptors received, in order, not yet claimed by an event */
	int		fds[8];
	int		nfds;
	/* capture streams' buffers */
	struct ws_capture_map *maps;
};

struct ws_buffer {
	uint32_t	id;
	uint32_t	width;
	uint32_t	height;
	uint32_t	stride;
	void		*data;
	size_t		size;
};

/* ---- Mach ---- */

static void
req_init(ws_connection_t *c, ws_req_header_t *h, mach_msg_id_t id,
    size_t size, uint32_t window)
{
	memset(h, 0, sizeof(*h));
	h->head.msgh_size = (mach_msg_size_t)size;
	h->head.msgh_remote_port = c->server;
	h->head.msgh_id = id;
	h->version = WS_PROTOCOL_VERSION;
	h->client_id = c->client_id;
	h->token = c->token;
	h->seq = ++c->seq;
	h->window = window;
}

static int
kr_errno(kern_return_t kr)
{
	switch (kr) {
	case MACH_SEND_INVALID_DEST:
	case MACH_RCV_PORT_DIED:
		return EPIPE;
	case MACH_SEND_TIMED_OUT:
	case MACH_RCV_TIMED_OUT:
		return ETIMEDOUT;
	case MACH_SEND_INTERRUPTED:
	case MACH_RCV_INTERRUPTED:
		return EINTR;
	default:
		return EIO;
	}
}

/*
 * mach_msg, restarted when a signal interrupts it, as Darwin's mach_msg()
 * does for callers that don't ask for MACH_SEND_INTERRUPT or
 * MACH_RCV_INTERRUPT. NextBSD's returns MACH_*_INTERRUPTED instead, so
 * a signal arriving during a request (seen in the Dock while it starts)
 * failed it with EINTR, a window creation included.
 */
static kern_return_t
msg_send(ws_req_header_t *h)
{
	kern_return_t kr;

	do
		kr = mach_msg(&h->head, MACH_SEND_MSG, h->head.msgh_size, 0,
		    MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	while (kr == MACH_SEND_INTERRUPTED);
	return kr;
}

static int
send_oneway(ws_connection_t *c, ws_req_header_t *h)
{
	kern_return_t kr;

	if (c->dead) {
		errno = EPIPE;
		return -1;
	}
	h->head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	h->head.msgh_local_port = MACH_PORT_NULL;
	kr = msg_send(h);
	if (kr != MACH_MSG_SUCCESS) {
		errno = kr_errno(kr);
		return -1;
	}
	return 0;
}

static int
send_rpc(ws_connection_t *c, ws_req_header_t *h, ws_reply_t *out)
{
	union {
		ws_reply_t	r;
		uint8_t		buf[sizeof(ws_reply_t) +
				    sizeof(mach_msg_max_trailer_t)];
	} rb;
	kern_return_t kr;
	uint32_t seq = h->seq;

	if (c->dead) {
		errno = EPIPE;
		return -1;
	}
	h->head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
	    MACH_MSG_TYPE_MAKE_SEND_ONCE);
	h->head.msgh_local_port = c->reply;
	kr = msg_send(h);
	if (kr != MACH_MSG_SUCCESS) {
		errno = kr_errno(kr);
		return -1;
	}
	for (;;) {
		memset(&rb, 0, sizeof(rb));
		kr = mach_msg(&rb.r.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
		    sizeof(rb), c->reply, RPC_TIMEOUT_MS, MACH_PORT_NULL);
		if (kr == MACH_RCV_INTERRUPTED)
			continue;	/* a signal: wait again (msg_send) */
		if (kr != MACH_MSG_SUCCESS) {
			errno = kr_errno(kr);
			return -1;
		}
		if (rb.r.head.msgh_size < offsetof(ws_reply_t, u) ||
		    rb.r.seq != seq)
			continue;	/* late reply to an earlier timeout */
		break;
	}
	if (out != NULL)
		memcpy(out, &rb.r, sizeof(*out));
	if (rb.r.status != 0) {
		errno = rb.r.status;
		return -1;
	}
	return 0;
}

static int
send_int(ws_connection_t *c, mach_msg_id_t id, uint32_t window, int32_t value)
{
	ws_req_int_t m;

	req_init(c, &m.hdr, id, sizeof(m), window);
	m.value = value;
	m.reserved = 0;
	return send_oneway(c, &m.hdr);
}

/* ---- socket ---- */

static int
queue_push(ws_connection_t *c, const ws_event_t *ev)
{
	if (c->qlen == c->qcap) {
		size_t ncap = c->qcap ? c->qcap * 2 : 32;
		ws_event_t *nq = malloc(ncap * sizeof(*nq));
		size_t i;

		if (nq == NULL)
			return -1;
		for (i = 0; i < c->qlen; i++)
			nq[i] = c->queue[(c->qhead + i) % c->qcap];
		free(c->queue);
		c->queue = nq;
		c->qcap = ncap;
		c->qhead = 0;
	}
	c->queue[(c->qhead + c->qlen) % c->qcap] = *ev;
	c->qlen++;
	return 0;
}

static int
queue_pop(ws_connection_t *c, ws_event_t *ev)
{
	if (c->qlen == 0)
		return 0;
	*ev = c->queue[c->qhead];
	c->qhead = (c->qhead + 1) % c->qcap;
	c->qlen--;
	return 1;
}

static int
fd_take(ws_connection_t *c)
{
	int fd;

	if (c->nfds == 0)
		return -1;
	fd = c->fds[0];
	memmove(c->fds, c->fds + 1, (size_t)(c->nfds - 1) * sizeof(int));
	c->nfds--;
	return fd;
}

static void
screen_rect(const ws_screen_info_t *s, ws_rect_t *r)
{
	r->x = s->x;
	r->y = s->y;
	r->width = (int32_t)s->width;
	r->height = (int32_t)s->height;
}

/* Take one whole event out of rbuf. Returns 1, 0 if incomplete, -1 if bad. */
static int
parse_event(ws_connection_t *c, ws_event_t *ev)
{
	ws_event_header_t hdr;

	if (c->rlen < sizeof(hdr))
		return 0;
	memcpy(&hdr, c->rbuf, sizeof(hdr));
	if (hdr.size < sizeof(hdr) || hdr.size > sizeof(*ev)) {
		c->dead = 1;
		errno = EPROTO;
		return -1;
	}
	if (c->rlen < hdr.size)
		return 0;
	memset(ev, 0, sizeof(*ev));
	memcpy(ev, c->rbuf, hdr.size);
	c->rlen -= hdr.size;
	memmove(c->rbuf, c->rbuf + hdr.size, c->rlen);
	/* Only snapshots, previews and keymaps carry a descriptor; it
	 * arrived with their bytes. */
	if (ev->hdr.type == WS_EVENT_SNAPSHOT)
		ev->u.snapshot.fd = fd_take(c);
	else if (ev->hdr.type == WS_EVENT_PREVIEW)
		ev->u.preview.fd = fd_take(c);
	else if (ev->hdr.type == WS_EVENT_KEYMAP)
		ev->u.keymap.fd = fd_take(c);
	else if (ev->hdr.type == WS_EVENT_CAPTURE_BUFFER)
		ev->u.capture_buffer.fd = fd_take(c);
	return 1;
}

/* Close the descriptor an event carries, for events nobody handles. */
static void
event_close_fd(const ws_event_t *ev)
{
	if (ev->hdr.type == WS_EVENT_SNAPSHOT && ev->u.snapshot.fd >= 0)
		close(ev->u.snapshot.fd);
	else if (ev->hdr.type == WS_EVENT_PREVIEW && ev->u.preview.fd >= 0)
		close(ev->u.preview.fd);
	else if (ev->hdr.type == WS_EVENT_KEYMAP && ev->u.keymap.fd >= 0)
		close(ev->u.keymap.fd);
	else if (ev->hdr.type == WS_EVENT_CAPTURE_BUFFER &&
	    ev->u.capture_buffer.fd >= 0)
		close(ev->u.capture_buffer.fd);
}

/* Returns 1 with an event, 0 if nothing is ready (nonblocking), -1 on error. */
static int
read_event(ws_connection_t *c, ws_event_t *ev)
{
	union {
		struct cmsghdr	hdr;
		char		buf[CMSG_SPACE(sizeof(int) * 8)];
	} cm;
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmsg;
	ssize_t n;
	size_t i, nfd;
	int r, fd;

	for (;;) {
		r = parse_event(c, ev);
		if (r != 0)
			return r;
		if (c->dead) {
			errno = EPIPE;
			return -1;
		}
		memset(&msg, 0, sizeof(msg));
		iov.iov_base = c->rbuf + c->rlen;
		iov.iov_len = sizeof(c->rbuf) - c->rlen;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cm.buf;
		msg.msg_controllen = sizeof(cm.buf);
		do {
			n = recvmsg(c->sock, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		} while (n < 0 && errno == EINTR);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return 0;
			c->dead = 1;
			return -1;
		}
		if (n == 0) {
			c->dead = 1;
			errno = EPIPE;
			return -1;
		}
		c->rlen += (size_t)n;
		for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
		    cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_level != SOL_SOCKET ||
			    cmsg->cmsg_type != SCM_RIGHTS)
				continue;
			nfd = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			for (i = 0; i < nfd; i++) {
				memcpy(&fd, (char *)CMSG_DATA(cmsg) +
				    i * sizeof(int), sizeof(int));
				if (c->nfds < (int)(sizeof(c->fds) /
				    sizeof(c->fds[0])))
					c->fds[c->nfds++] = fd;
				else
					close(fd);
			}
		}
	}
}

static int
wait_readable(ws_connection_t *c, int timeout_ms)
{
	struct pollfd pfd;
	int r;

	pfd.fd = c->sock;
	pfd.events = POLLIN;
	pfd.revents = 0;
	do {
		r = poll(&pfd, 1, timeout_ms);
	} while (r < 0 && errno == EINTR);
	return r;
}

/* Wait for an event of `type`, queueing everything else. */
static int
wait_for_event(ws_connection_t *c, uint16_t type, ws_event_t *out,
    int timeout_ms)
{
	ws_event_t ev;
	int r;

	for (;;) {
		r = read_event(c, &ev);
		if (r < 0)
			return -1;
		if (r == 0) {
			r = wait_readable(c, timeout_ms);
			if (r < 0)
				return -1;
			if (r == 0) {
				errno = ETIMEDOUT;
				return -1;
			}
			continue;
		}
		if (ev.hdr.type == type) {
			*out = ev;
			return 0;
		}
		if (ev.hdr.type == WS_EVENT_ERROR && type == WS_EVENT_SNAPSHOT &&
		    ev.u.error.request == WS_MSG_SNAPSHOT) {
			errno = ev.u.error.code ? ev.u.error.code : EIO;
			return -1;
		}
		if (queue_push(c, &ev) < 0)
			return -1;
	}
}

static int
sock_send(ws_connection_t *c, const void *data, size_t len, int fd)
{
	union {
		struct cmsghdr	hdr;
		char		buf[CMSG_SPACE(sizeof(int))];
	} cm;
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;

	memset(&msg, 0, sizeof(msg));
	iov.iov_base = (void *)(uintptr_t)data;
	iov.iov_len = len;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	if (fd >= 0) {
		struct cmsghdr *cmsg;

		memset(&cm, 0, sizeof(cm));
		msg.msg_control = cm.buf;
		msg.msg_controllen = sizeof(cm.buf);
		cmsg = CMSG_FIRSTHDR(&msg);
		cmsg->cmsg_level = SOL_SOCKET;
		cmsg->cmsg_type = SCM_RIGHTS;
		cmsg->cmsg_len = CMSG_LEN(sizeof(int));
		memcpy(CMSG_DATA(cmsg), &fd, sizeof(int));
	}
	for (;;) {
		n = sendmsg(c->sock, &msg, MSG_NOSIGNAL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if ((size_t)n >= iov.iov_len)
			return 0;
		/* partial write: the rest goes without the descriptor */
		iov.iov_base = (char *)iov.iov_base + n;
		iov.iov_len -= (size_t)n;
		msg.msg_control = NULL;
		msg.msg_controllen = 0;
	}
}

/* ---- connection ---- */

static ws_connection_t *
connect_impl(const char *service, const char *app_name, uint32_t flags,
    uint32_t join_client, uint64_t join_token)
{
	ws_connection_t *c;
	ws_req_hello_t hello;
	ws_reply_t reply;
	ws_sock_auth_t auth;
	struct sockaddr_un sun;
	ws_event_t ev;
	kern_return_t kr;
	int saved;

	if (service == NULL)
		service = WS_SERVICE_NAME;
	c = calloc(1, sizeof(*c));
	if (c == NULL)
		return NULL;
	c->sock = -1;
	c->next_buffer_id = 1;

	kr = bootstrap_look_up(bootstrap_port, service, &c->server);
	if (kr != KERN_SUCCESS) {
		free(c);
		errno = ENOENT;
		return NULL;
	}
	kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE,
	    &c->reply);
	if (kr != KERN_SUCCESS) {
		mach_port_deallocate(mach_task_self(), c->server);
		free(c);
		errno = ENOMEM;
		return NULL;
	}

	req_init(c, &hello.hdr, WS_MSG_HELLO, sizeof(hello), 0);
	hello.flags = flags;
	hello.reserved = 0;
	memset(hello.app_name, 0, sizeof(hello.app_name));
	memset(hello.bundle_id, 0, sizeof(hello.bundle_id));
	if (app_name != NULL)
		strlcpy(hello.app_name, app_name, sizeof(hello.app_name));
	hello.join_client = join_client;
	hello.reserved2 = 0;
	hello.join_token = join_token;
	if (send_rpc(c, &hello.hdr, &reply) < 0)
		goto fail;
	c->client_id = reply.u.hello.client_id;
	c->token = reply.u.hello.token;
	c->server_flags = reply.u.hello.server_flags;
	c->app_id = reply.u.hello.app_id;
	if ((flags & WS_CONNECT_JOIN) && c->app_id == 0) {
		/* An older server ignored the join: this would be an app
		 * of its own. Say goodbye rather than pretend. */
		ws_req_header_t h;

		req_init(c, &h, WS_MSG_GOODBYE, sizeof(h), 0);
		(void)send_oneway(c, &h);
		errno = ENOTSUP;
		goto fail;
	}

	c->sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (c->sock < 0)
		goto fail;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	memcpy(sun.sun_path, reply.u.hello.socket_path,
	    sizeof(sun.sun_path) - 1);
	if (connect(c->sock, (struct sockaddr *)&sun, sizeof(sun)) < 0)
		goto fail;
	memset(&auth, 0, sizeof(auth));
	auth.hdr.type = WS_SOCK_AUTH;
	auth.hdr.size = sizeof(auth);
	auth.client_id = c->client_id;
	auth.version = WS_PROTOCOL_VERSION;
	auth.token = c->token;
	if (sock_send(c, &auth, sizeof(auth), -1) < 0)
		goto fail;
	if (wait_for_event(c, WS_EVENT_WELCOME, &ev, WELCOME_TIMEOUT_MS) < 0)
		goto fail;
	c->screen = ev.u.welcome.screen;
	c->server_flags = ev.u.welcome.server_flags;
	if (c->server_flags & WS_SERVER_USABLE_AREA)
		c->usable = ev.u.welcome.usable;
	else
		screen_rect(&c->screen, &c->usable);
	if (c->server_flags & WS_SERVER_MENU_BAR)
		c->menu_bar = ev.u.welcome.menu_bar;
	return c;

fail:
	saved = errno;
	if (c->sock >= 0)
		close(c->sock);
	mach_port_destruct(mach_task_self(), c->reply, 0, 0);
	mach_port_deallocate(mach_task_self(), c->server);
	free(c->queue);
	free(c);
	errno = saved;
	return NULL;
}

static const char *
default_service(void)
{
	const char *service = getenv(WS_SERVICE_NAME_ENV);

	if (service == NULL || *service == '\0')
		service = WS_SERVICE_NAME;
	return service;
}

WS_API ws_connection_t *
ws_connect_service(const char *service, const char *app_name, uint32_t flags)
{
	return connect_impl(service, app_name, flags & ~WS_CONNECT_JOIN, 0, 0);
}

WS_API ws_connection_t *
ws_connect(const char *app_name, uint32_t flags)
{
	return ws_connect_service(default_service(), app_name, flags);
}

WS_API ws_connection_t *
ws_connect_join(const char *app_name, uint32_t flags, uint32_t app_client_id,
    uint64_t app_token)
{
	if (app_client_id == 0 || (flags & WS_CONNECT_CONTROL)) {
		errno = EINVAL;
		return NULL;
	}
	return connect_impl(default_service(), app_name,
	    flags | WS_CONNECT_JOIN, app_client_id, app_token);
}

WS_API void
ws_disconnect(ws_connection_t *c)
{
	ws_req_header_t h;
	ws_event_t ev;

	if (c == NULL)
		return;
	if (!c->dead) {
		req_init(c, &h, WS_MSG_GOODBYE, sizeof(h), 0);
		(void)send_oneway(c, &h);
	}
	while (queue_pop(c, &ev))
		event_close_fd(&ev);
	while (c->nfds > 0)
		close(fd_take(c));
	while (c->maps != NULL) {
		struct ws_capture_map *m = c->maps;

		c->maps = m->next;
		munmap(m->data, m->size);
		free(m);
	}
	if (c->sock >= 0)
		close(c->sock);
	mach_port_destruct(mach_task_self(), c->reply, 0, 0);
	mach_port_deallocate(mach_task_self(), c->server);
	free(c->queue);
	free(c);
}

WS_API int
ws_connection_fd(ws_connection_t *c)
{
	return c->sock;
}

WS_API uint32_t
ws_client_id(ws_connection_t *c)
{
	return c->client_id;
}

WS_API uint64_t
ws_client_token(ws_connection_t *c)
{
	return c->token;
}

WS_API uint32_t
ws_app_id(ws_connection_t *c)
{
	return c->app_id;
}

WS_API uint32_t
ws_server_flags(ws_connection_t *c)
{
	return c->server_flags;
}

WS_API const ws_screen_info_t *
ws_main_screen(ws_connection_t *c)
{
	return &c->screen;
}

WS_API int
ws_usable_area(ws_connection_t *c, ws_rect_t *area)
{
	*area = c->usable;
	if ((c->server_flags & WS_SERVER_USABLE_AREA) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	return 0;
}

WS_API uint32_t
ws_menu_bar_height(ws_connection_t *c)
{
	return c->menu_bar;
}

WS_API int
ws_pending(ws_connection_t *c)
{
	ws_event_header_t hdr;

	if (c->qlen != 0)
		return 1;
	if (c->rlen < sizeof(hdr))
		return 0;
	memcpy(&hdr, c->rbuf, sizeof(hdr));
	return c->rlen >= hdr.size;
}

static void capture_note(ws_connection_t *c, ws_event_t *ev);

static void
note_event(ws_connection_t *c, ws_event_t *ev)
{
	if (ev->hdr.type >= WS_EVENT_CAPTURE_STATE &&
	    ev->hdr.type <= WS_EVENT_CAPTURE_FRAME)
		capture_note(c, ev);
	if (ev->hdr.type == WS_EVENT_SCREEN_CHANGED &&
	    ev->u.screen.screen_id == c->screen.screen_id) {
		c->screen = ev->u.screen;
		/* until the usable area that follows */
		if ((c->server_flags & WS_SERVER_USABLE_AREA) == 0)
			screen_rect(&c->screen, &c->usable);
	}
	if (ev->hdr.type == WS_EVENT_USABLE_AREA &&
	    ev->u.usable_area.screen_id == c->screen.screen_id) {
		c->usable = ev->u.usable_area.usable;
		c->menu_bar = ev->u.usable_area.menu_bar;
	}
}

WS_API int
ws_dispatch(ws_connection_t *c, ws_event_handler_t handler, void *ctx)
{
	ws_event_t ev;
	int count = 0, r;

	for (;;) {
		if (!queue_pop(c, &ev)) {
			r = read_event(c, &ev);
			if (r < 0)
				return count ? count : -1;
			if (r == 0)
				return count;
		}
		note_event(c, &ev);
		if (handler != NULL)
			handler(c, &ev, ctx);
		else
			event_close_fd(&ev);
		count++;
	}
}

WS_API int
ws_next_event(ws_connection_t *c, ws_event_t *ev, int timeout_ms)
{
	int r;

	if (queue_pop(c, ev)) {
		note_event(c, ev);
		return 1;
	}
	for (;;) {
		r = read_event(c, ev);
		if (r != 0) {
			if (r > 0)
				note_event(c, ev);
			return r;
		}
		r = wait_readable(c, timeout_ms);
		if (r <= 0)
			return r;
	}
}

WS_API int
ws_sync(ws_connection_t *c)
{
	ws_req_header_t h;

	req_init(c, &h, WS_MSG_SYNC, sizeof(h), 0);
	return send_rpc(c, &h, NULL);
}

/* ---- windows ---- */

WS_API uint32_t
ws_window_create(ws_connection_t *c, const ws_rect_t *frame, int32_t level,
    uint32_t flags, const char *title)
{
	ws_req_window_create_t m;
	ws_reply_t reply;

	/* Without the optional tail: the server picks the id. */
	req_init(c, &m.hdr, WS_MSG_WINDOW_CREATE, WS_WINDOW_CREATE_MIN_SIZE, 0);
	m.frame = *frame;
	m.level = level;
	m.flags = flags;
	memset(m.title, 0, sizeof(m.title));
	if (title != NULL)
		strlcpy(m.title, title, sizeof(m.title));
	if (send_rpc(c, &m.hdr, &reply) < 0)
		return 0;
	return reply.u.create.window;
}

/* How many window ids ws_window_create_nowait reserves at a time. */
#define WINDOW_ID_BLOCK		256

WS_API uint32_t
ws_window_create_nowait(ws_connection_t *c, const ws_rect_t *frame,
    int32_t level, uint32_t flags, const char *title)
{
	ws_req_window_create_t m;
	ws_req_int_t r;
	ws_reply_t reply;

	if ((c->server_flags & WS_SERVER_WINDOW_IDS) == 0)
		return ws_window_create(c, frame, level, flags, title);
	if (c->wid_next == c->wid_end) {
		/* One round trip per block of ids. */
		req_init(c, &r.hdr, WS_MSG_RESERVE_WINDOW_IDS, sizeof(r), 0);
		r.value = WINDOW_ID_BLOCK;
		r.reserved = 0;
		if (send_rpc(c, &r.hdr, &reply) < 0)
			return 0;
		if (reply.u.window_ids.base == 0 ||
		    reply.u.window_ids.count == 0) {
			errno = EPROTO;
			return 0;
		}
		c->wid_next = reply.u.window_ids.base;
		c->wid_end = reply.u.window_ids.base + reply.u.window_ids.count;
	}
	req_init(c, &m.hdr, WS_MSG_WINDOW_CREATE, sizeof(m), 0);
	m.frame = *frame;
	m.level = level;
	m.flags = flags;
	memset(m.title, 0, sizeof(m.title));
	if (title != NULL)
		strlcpy(m.title, title, sizeof(m.title));
	m.window_id = c->wid_next;
	m.reserved = 0;
	if (send_oneway(c, &m.hdr) < 0)
		return 0;
	return c->wid_next++;
}

WS_API int
ws_window_destroy(ws_connection_t *c, uint32_t window)
{
	ws_req_header_t h;

	req_init(c, &h, WS_MSG_WINDOW_DESTROY, sizeof(h), window);
	return send_oneway(c, &h);
}

WS_API int
ws_window_set_frame(ws_connection_t *c, uint32_t window, const ws_rect_t *frame)
{
	ws_req_frame_t m;

	req_init(c, &m.hdr, WS_MSG_WINDOW_SET_FRAME, sizeof(m), window);
	m.frame = *frame;
	return send_oneway(c, &m.hdr);
}

WS_API int
ws_window_set_level(ws_connection_t *c, uint32_t window, int32_t level)
{
	return send_int(c, WS_MSG_WINDOW_SET_LEVEL, window, level);
}

WS_API int
ws_window_order(ws_connection_t *c, uint32_t window, int op,
    uint32_t relative_to)
{
	ws_req_order_t m;

	req_init(c, &m.hdr, WS_MSG_WINDOW_ORDER, sizeof(m), window);
	m.op = op;
	m.relative_to = relative_to;
	return send_oneway(c, &m.hdr);
}

WS_API int
ws_window_set_title(ws_connection_t *c, uint32_t window, const char *title)
{
	ws_req_title_t m;

	req_init(c, &m.hdr, WS_MSG_WINDOW_SET_TITLE, sizeof(m), window);
	memset(m.title, 0, sizeof(m.title));
	if (title != NULL)
		strlcpy(m.title, title, sizeof(m.title));
	return send_oneway(c, &m.hdr);
}

WS_API int
ws_window_set_flags(ws_connection_t *c, uint32_t window, uint32_t flags)
{
	return send_int(c, WS_MSG_WINDOW_SET_FLAGS, window, (int32_t)flags);
}

WS_API int
ws_window_make_key(ws_connection_t *c, uint32_t window)
{
	ws_req_header_t h;

	req_init(c, &h, WS_MSG_WINDOW_MAKE_KEY, sizeof(h), window);
	return send_oneway(c, &h);
}

/* ---- buffers ---- */

WS_API ws_buffer_t *
ws_buffer_create_format(ws_connection_t *c, uint32_t width, uint32_t height,
    uint32_t scale, uint32_t format)
{
	ws_buffer_t *b;
	ws_sock_buffer_t m;
	int fd, saved;

	if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
	    (format != WS_FORMAT_ARGB8888 && format != WS_FORMAT_XRGB8888)) {
		errno = EINVAL;
		return NULL;
	}
	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return NULL;
	b->id = c->next_buffer_id++;
	b->width = width;
	b->height = height;
	b->stride = width * 4;
	b->size = (size_t)b->stride * height;

	fd = memfd_create("WindowServer-buffer", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0)
		goto fail;
	if (ftruncate(fd, (off_t)b->size) < 0 ||
	    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) < 0)
		goto fail_fd;
	b->data = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->data == MAP_FAILED)
		goto fail_fd;

	memset(&m, 0, sizeof(m));
	m.hdr.type = WS_SOCK_BUFFER_CREATE;
	m.hdr.size = sizeof(m);
	m.buffer_id = b->id;
	m.width = width;
	m.height = height;
	m.stride = b->stride;
	m.format = format;
	m.scale = scale ? scale : 1;
	m.offset = 0;
	if (sock_send(c, &m, sizeof(m), fd) < 0) {
		saved = errno;
		munmap(b->data, b->size);
		errno = saved;
		goto fail_fd;
	}
	close(fd);
	return b;

fail_fd:
	saved = errno;
	close(fd);
	errno = saved;
fail:
	saved = errno;
	free(b);
	errno = saved;
	return NULL;
}

WS_API int
ws_dmabuf_formats(ws_connection_t *c, ws_dmabuf_formats_t *out)
{
	ws_req_header_t h;
	ws_reply_t reply;
	uint32_t i;

	if ((c->server_flags & WS_SERVER_DMABUF) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	req_init(c, &h, WS_MSG_DMABUF_FORMATS, sizeof(h), 0);
	if (send_rpc(c, &h, &reply) < 0)
		return -1;
	memset(out, 0, sizeof(*out));
	memcpy(out->render_node, reply.u.dmabuf.render_node,
	    sizeof(out->render_node));
	out->render_node[sizeof(out->render_node) - 1] = '\0';
	out->count = reply.u.dmabuf.count;
	if (out->count > WS_MAX_DMABUF_FORMATS)
		out->count = WS_MAX_DMABUF_FORMATS;
	for (i = 0; i < out->count; i++) {
		out->formats[i].format = reply.u.dmabuf.formats[i].format;
		out->formats[i].modifier = reply.u.dmabuf.formats[i].modifier;
	}
	return 0;
}

WS_API ws_buffer_t *
ws_buffer_create_dmabuf(ws_connection_t *c, uint32_t width, uint32_t height,
    uint32_t scale, uint32_t format, int fd, uint32_t offset, uint32_t stride,
    uint64_t modifier)
{
	ws_sock_buffer_dmabuf_t m;
	ws_buffer_t *b;
	int saved;

	if (width == 0 || height == 0 || width > 16384 || height > 16384 ||
	    fd < 0 || (format != WS_FORMAT_ARGB8888 &&
	    format != WS_FORMAT_XRGB8888)) {
		errno = EINVAL;
		return NULL;
	}
	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return NULL;
	b->id = c->next_buffer_id++;
	b->width = width;
	b->height = height;
	b->stride = stride;
	memset(&m, 0, sizeof(m));
	m.hdr.type = WS_SOCK_BUFFER_CREATE_DMABUF;
	m.hdr.size = sizeof(m);
	m.buffer_id = b->id;
	m.width = width;
	m.height = height;
	m.format = format;
	m.scale = scale ? scale : 1;
	m.stride = stride;
	m.offset = offset;
	m.modifier = modifier;
	if (sock_send(c, &m, sizeof(m), fd) < 0) {
		saved = errno;
		free(b);
		errno = saved;
		return NULL;
	}
	return b;
}

WS_API ws_buffer_t *
ws_buffer_create(ws_connection_t *c, uint32_t width, uint32_t height,
    uint32_t scale)
{
	return ws_buffer_create_format(c, width, height, scale,
	    WS_FORMAT_ARGB8888);
}

WS_API void
ws_buffer_destroy(ws_connection_t *c, ws_buffer_t *b)
{
	if (b == NULL)
		return;
	(void)send_int(c, WS_MSG_BUFFER_DESTROY, 0, (int32_t)b->id);
	if (b->data != NULL)	/* not a GPU buffer */
		munmap(b->data, b->size);
	free(b);
}

WS_API void *
ws_buffer_data(ws_buffer_t *b)
{
	return b->data;
}

WS_API uint32_t
ws_buffer_stride(ws_buffer_t *b)
{
	return b->stride;
}

WS_API uint32_t
ws_buffer_width(ws_buffer_t *b)
{
	return b->width;
}

WS_API uint32_t
ws_buffer_height(ws_buffer_t *b)
{
	return b->height;
}

WS_API uint32_t
ws_buffer_id(ws_buffer_t *b)
{
	return b->id;
}

WS_API int
ws_window_attach(ws_connection_t *c, uint32_t window, ws_buffer_t *b)
{
	return send_int(c, WS_MSG_WINDOW_ATTACH, window,
	    b != NULL ? (int32_t)b->id : 0);
}

WS_API int
ws_window_damage(ws_connection_t *c, uint32_t window, const ws_rect_t *rects,
    unsigned count)
{
	ws_req_damage_t m;
	unsigned n;

	do {
		n = count > WS_MAX_DAMAGE_RECTS ? WS_MAX_DAMAGE_RECTS : count;
		req_init(c, &m.hdr, WS_MSG_WINDOW_DAMAGE,
		    offsetof(ws_req_damage_t, rects) + n * sizeof(ws_rect_t),
		    window);
		m.count = n;
		m.reserved = 0;
		memcpy(m.rects, rects, n * sizeof(ws_rect_t));
		if (send_oneway(c, &m.hdr) < 0)
			return -1;
		rects += n;
		count -= n;
	} while (count > 0);
	return 0;
}

WS_API int
ws_window_commit(ws_connection_t *c, uint32_t window, uint32_t flags)
{
	return send_int(c, WS_MSG_WINDOW_COMMIT, window,
	    (int32_t)(flags & ~(uint32_t)WS_COMMIT_SET_FRAME));
}

WS_API int
ws_window_commit_frame(ws_connection_t *c, uint32_t window, uint32_t flags,
    const ws_rect_t *frame)
{
	ws_req_commit_t m;

	if (frame == NULL)
		return ws_window_commit(c, window, flags);
	if ((c->server_flags & WS_SERVER_COMMIT_FRAME) == 0) {
		/* Older servers: the frame first, as before. */
		if (ws_window_set_frame(c, window, frame) < 0)
			return -1;
		return ws_window_commit(c, window, flags);
	}
	req_init(c, &m.hdr, WS_MSG_WINDOW_COMMIT, sizeof(m), window);
	m.value = (int32_t)(flags | WS_COMMIT_SET_FRAME);
	m.reserved = 0;
	m.frame = *frame;
	return send_oneway(c, &m.hdr);
}

WS_API int
ws_window_set_size_limits(ws_connection_t *c, uint32_t window,
    uint32_t min_width, uint32_t min_height, uint32_t max_width,
    uint32_t max_height)
{
	ws_req_size_limits_t m;

	if ((c->server_flags & WS_SERVER_TILING) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	req_init(c, &m.hdr, WS_MSG_WINDOW_SET_SIZE_LIMITS, sizeof(m), window);
	m.min_width = min_width;
	m.min_height = min_height;
	m.max_width = max_width;
	m.max_height = max_height;
	return send_oneway(c, &m.hdr);
}

WS_API int
ws_window_set_input_region(ws_connection_t *c, uint32_t window,
    const ws_rect_t *rects, unsigned count)
{
	ws_req_region_t m;
	uint32_t flags = 0;
	unsigned n;

	if ((c->server_flags & WS_SERVER_INPUT_REGION) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	if (rects == NULL) {
		req_init(c, &m.hdr, WS_MSG_WINDOW_SET_INPUT_REGION,
		    offsetof(ws_req_region_t, rects), window);
		m.count = 0;
		m.flags = WS_REGION_RESET;
		return send_oneway(c, &m.hdr);
	}
	do {
		n = count > WS_MAX_REGION_RECTS ? WS_MAX_REGION_RECTS : count;
		req_init(c, &m.hdr, WS_MSG_WINDOW_SET_INPUT_REGION,
		    offsetof(ws_req_region_t, rects) + n * sizeof(ws_rect_t),
		    window);
		m.count = n;
		m.flags = flags;
		memcpy(m.rects, rects, n * sizeof(ws_rect_t));
		if (send_oneway(c, &m.hdr) < 0)
			return -1;
		rects += n;
		count -= n;
		flags = WS_REGION_APPEND;
	} while (count > 0);
	return 0;
}

/* ---- minimizing ---- */

WS_API int
ws_window_minimize(ws_connection_t *c, uint32_t window)
{
	ws_req_header_t h;

	req_init(c, &h, WS_MSG_WINDOW_MINIMIZE, sizeof(h), window);
	return send_oneway(c, &h);
}

WS_API int
ws_window_minimize_where(ws_connection_t *c, uint32_t window, uint32_t where)
{
	/* A bare header for older servers and the server's default. */
	if (where == WS_MINIMIZE_DEFAULT ||
	    (c->server_flags & WS_SERVER_MINIMIZE_WHERE) == 0)
		return ws_window_minimize(c, window);
	return send_int(c, WS_MSG_WINDOW_MINIMIZE, window, (int32_t)where);
}

WS_API int
ws_set_app_icon(ws_connection_t *c, uint32_t width, uint32_t height,
    const void *argb, uint32_t stride)
{
	ws_buffer_t *b;
	uint32_t y;
	int r, saved;

	if (argb == NULL)
		return send_int(c, WS_MSG_SET_APP_ICON, 0, 0);
	if (width == 0 || height == 0 || width > WS_MAX_APP_ICON ||
	    height > WS_MAX_APP_ICON || stride < width * 4) {
		errno = EINVAL;
		return -1;
	}
	/* The server copies the pixels when it gets the request, and the
	 * buffer message goes first on the socket, so the buffer can go
	 * right after. */
	b = ws_buffer_create(c, width, height, 1);
	if (b == NULL)
		return -1;
	for (y = 0; y < height; y++)
		memcpy((char *)b->data + (size_t)y * b->stride,
		    (const char *)argb + (size_t)y * stride, (size_t)width * 4);
	r = send_int(c, WS_MSG_SET_APP_ICON, 0, (int32_t)b->id);
	saved = errno;
	ws_buffer_destroy(c, b);
	errno = saved;
	return r;
}

/* ---- cursor and activation ---- */

WS_API int
ws_set_cursor(ws_connection_t *c, uint32_t kind, ws_buffer_t *image,
    int32_t hot_x, int32_t hot_y)
{
	ws_req_cursor_t m;

	req_init(c, &m.hdr, WS_MSG_SET_CURSOR, sizeof(m), 0);
	m.kind = kind;
	m.buffer_id = image != NULL ? image->id : 0;
	m.hot_x = hot_x;
	m.hot_y = hot_y;
	return send_oneway(c, &m.hdr);
}

WS_API int
ws_window_set_cursor(ws_connection_t *c, uint32_t window, uint32_t kind,
    ws_buffer_t *image, int32_t hot_x, int32_t hot_y)
{
	ws_req_cursor_t m;

	if ((c->server_flags & WS_SERVER_WINDOW_CURSOR) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	req_init(c, &m.hdr, WS_MSG_WINDOW_SET_CURSOR, sizeof(m), window);
	m.kind = kind;
	m.buffer_id = image != NULL ? image->id : 0;
	m.hot_x = hot_x;
	m.hot_y = hot_y;
	return send_oneway(c, &m.hdr);
}

/* ---- pointer grabs, warp and query ---- */

WS_API int
ws_pointer_grab(ws_connection_t *c, uint32_t mode)
{
	ws_req_int_t m;

	if ((c->server_flags & WS_SERVER_POINTER_GRAB) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	req_init(c, &m.hdr, WS_MSG_POINTER_GRAB, sizeof(m), 0);
	m.value = (int32_t)mode;
	m.reserved = 0;
	return send_rpc(c, &m.hdr, NULL);
}

WS_API int
ws_pointer_ungrab(ws_connection_t *c)
{
	ws_req_header_t h;

	if ((c->server_flags & WS_SERVER_POINTER_GRAB) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	req_init(c, &h, WS_MSG_POINTER_UNGRAB, sizeof(h), 0);
	return send_oneway(c, &h);
}

WS_API int
ws_warp_pointer(ws_connection_t *c, uint32_t window, double x, double y)
{
	ws_req_point_t m;

	if ((c->server_flags & WS_SERVER_POINTER_WARP) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	req_init(c, &m.hdr, WS_MSG_WARP_POINTER, sizeof(m), window);
	m.x = x;
	m.y = y;
	return send_oneway(c, &m.hdr);
}

WS_API int
ws_query_pointer(ws_connection_t *c, ws_pointer_info_t *info)
{
	ws_req_header_t h;
	ws_reply_t reply;

	if ((c->server_flags & WS_SERVER_POINTER_WARP) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	req_init(c, &h, WS_MSG_QUERY_POINTER, sizeof(h), 0);
	if (send_rpc(c, &h, &reply) < 0)
		return -1;
	if (info != NULL)
		*info = reply.u.pointer;
	return 0;
}

WS_API int
ws_pointer_track(ws_connection_t *c, int on)
{
	if ((c->server_flags & WS_SERVER_POINTER_WARP) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	return send_int(c, WS_MSG_POINTER_TRACK, 0, on ? 1 : 0);
}

WS_API int
ws_set_cursor_visible(ws_connection_t *c, int visible)
{
	return send_int(c, WS_MSG_CURSOR_VISIBLE, 0, visible ? 1 : 0);
}

WS_API int
ws_activate(ws_connection_t *c)
{
	return send_int(c, WS_MSG_ACTIVATE, 0, 0);
}

WS_API int
ws_activate_flags(ws_connection_t *c, uint32_t flags)
{
	return send_int(c, WS_MSG_ACTIVATE, 0, (int32_t)flags);
}

WS_API int
ws_set_menu_bar(ws_connection_t *c, uint32_t height)
{
	return send_int(c, WS_MSG_SET_MENU_BAR, 0, height);
}

WS_API int
ws_deactivate(ws_connection_t *c)
{
	ws_req_header_t h;

	req_init(c, &h, WS_MSG_DEACTIVATE, sizeof(h), 0);
	return send_oneway(c, &h);
}

/* ---- drag and drop (WS_SERVER_DRAG) ---- */

static int
need_drag(ws_connection_t *c)
{
	if ((c->server_flags & WS_SERVER_DRAG) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	return 0;
}

WS_API int
ws_drag_begin(ws_connection_t *c, const ws_drag_params_t *p,
    uint32_t *session)
{
	ws_req_drag_begin_t m;
	ws_reply_t r;
	size_t len = 0, n;
	unsigned i;

	if (need_drag(c) < 0)
		return -1;
	if (p == NULL) {
		errno = EINVAL;
		return -1;
	}
	req_init(c, &m.hdr, WS_MSG_DRAG_BEGIN, sizeof(m), p->window);
	m.buffer_id = p->image != NULL ? p->image->id : 0;
	m.flags = p->flags;
	m.hot_x = p->hot_x;
	m.hot_y = p->hot_y;
	m.ops_local = p->ops_local;
	m.ops_external = p->ops_external;
	m.slide_x = p->slide_x;
	m.slide_y = p->slide_y;
	memset(m.pasteboard, 0, sizeof(m.pasteboard));
	if (p->pasteboard != NULL)
		strlcpy(m.pasteboard, p->pasteboard, sizeof(m.pasteboard));
	memset(m.types, 0, sizeof(m.types));
	m.ntypes = 0;
	for (i = 0; i < p->ntypes && p->types != NULL; i++) {
		n = strlen(p->types[i]) + 1;
		if (len + n > sizeof(m.types))
			break;		/* the rest don't fit */
		memcpy(m.types + len, p->types[i], n);
		len += n;
		m.ntypes++;
	}
	m.types_len = (uint32_t)len;
	/* Only the bytes used go. */
	m.hdr.head.msgh_size = (mach_msg_size_t)
	    ((offsetof(ws_req_drag_begin_t, types) + len + 3) & ~(size_t)3);
	memset(&r, 0, sizeof(r));
	if (send_rpc(c, &m.hdr, &r) < 0)
		return -1;
	if (session != NULL)
		*session = r.u.drag.session;
	return 0;
}

WS_API int
ws_drag_status(ws_connection_t *c, uint32_t session, uint32_t window,
    uint32_t operation, uint32_t flags)
{
	ws_req_drag_status_t m;

	if (need_drag(c) < 0)
		return -1;
	req_init(c, &m.hdr, WS_MSG_DRAG_STATUS, sizeof(m), window);
	m.session = session;
	m.operation = operation;
	m.flags = flags;
	m.reserved = 0;
	return send_oneway(c, &m.hdr);
}

WS_API int
ws_drag_cancel(ws_connection_t *c, uint32_t session)
{
	if (need_drag(c) < 0)
		return -1;
	return send_int(c, WS_MSG_DRAG_CANCEL, 0, (int32_t)session);
}

WS_API int
ws_drag_query(ws_connection_t *c, ws_event_drag_state_t *state)
{
	ws_req_header_t h;
	ws_event_t ev;
	int r;

	if (need_drag(c) < 0)
		return -1;
	req_init(c, &h, WS_MSG_DRAG_QUERY, sizeof(h), 0);
	if (send_oneway(c, &h) < 0)
		return -1;
	for (;;) {
		r = read_event(c, &ev);
		if (r < 0)
			return -1;
		if (r == 0) {
			r = wait_readable(c, RPC_TIMEOUT_MS);
			if (r <= 0) {
				if (r == 0)
					errno = ETIMEDOUT;
				return -1;
			}
			continue;
		}
		if (ev.hdr.type == WS_EVENT_DRAG_STATE) {
			if (state != NULL)
				*state = ev.u.drag_state;
			return 0;
		}
		if (ev.hdr.type == WS_EVENT_ERROR &&
		    ev.u.error.request == WS_MSG_DRAG_QUERY) {
			errno = ev.u.error.code ? ev.u.error.code : EIO;
			return -1;
		}
		if (queue_push(c, &ev) < 0)
			return -1;
	}
}

/* ---- tool requests ---- */

/* Map the image a snapshot or preview event carries, and close its fd. */
static int
map_image(const ws_event_snapshot_t *s, ws_snapshot_t *out)
{
	size_t size;
	void *p;

	if (s->fd < 0) {
		errno = EIO;
		return -1;
	}
	size = (size_t)s->stride * s->height;
	p = mmap(NULL, size, PROT_READ, MAP_SHARED, s->fd, 0);
	close(s->fd);
	if (p == MAP_FAILED)
		return -1;
	out->width = s->width;
	out->height = s->height;
	out->stride = s->stride;
	out->format = s->format;
	out->pixels = p;
	out->map_size = size;
	return 0;
}

WS_API int
ws_snapshot(ws_connection_t *c, ws_snapshot_t *out)
{
	ws_req_int_t m;
	ws_event_t ev;

	req_init(c, &m.hdr, WS_MSG_SNAPSHOT, sizeof(m), 0);
	m.value = 0;
	m.reserved = 0;
	if (send_rpc(c, &m.hdr, NULL) < 0)
		return -1;
	if (wait_for_event(c, WS_EVENT_SNAPSHOT, &ev, RPC_TIMEOUT_MS) < 0)
		return -1;
	return map_image(&ev.u.snapshot, out);
}

WS_API void
ws_snapshot_free(ws_snapshot_t *s)
{
	if (s->pixels != NULL)
		munmap(s->pixels, s->map_size);
	s->pixels = NULL;
}

WS_API int
ws_list_windows(ws_connection_t *c,
    void (*fn)(const ws_event_window_info_t *, void *), void *ctx)
{
	return ws_list_windows_flags(c, 0, fn, ctx);
}

WS_API int
ws_list_windows_flags(ws_connection_t *c, uint32_t options,
    void (*fn)(const ws_event_window_info_t *, void *), void *ctx)
{
	ws_req_header_t h;
	ws_req_int_t m;
	ws_event_t ev;
	int r, err = 0;

	if (options == 0) {
		/* the request older servers know */
		req_init(c, &h, WS_MSG_LIST_WINDOWS, sizeof(h), 0);
		r = send_oneway(c, &h);
	} else {
		req_init(c, &m.hdr, WS_MSG_LIST_WINDOWS, sizeof(m), 0);
		m.value = (int32_t)options;
		m.reserved = 0;
		r = send_oneway(c, &m.hdr);
	}
	if (r < 0)
		return -1;
	for (;;) {
		r = read_event(c, &ev);
		if (r < 0)
			return -1;
		if (r == 0) {
			r = wait_readable(c, RPC_TIMEOUT_MS);
			if (r <= 0) {
				if (r == 0)
					errno = ETIMEDOUT;
				return -1;
			}
			continue;
		}
		if (ev.hdr.type == WS_EVENT_LIST_END) {
			if (err != 0) {
				errno = err;
				return -1;
			}
			return 0;
		}
		if (ev.hdr.type == WS_EVENT_WINDOW_INFO) {
			if (fn != NULL)
				fn(&ev.u.window_info, ctx);
			continue;
		}
		if (ev.hdr.type == WS_EVENT_ERROR &&
		    ev.u.error.request == WS_MSG_LIST_WINDOWS) {
			err = ev.u.error.code ? ev.u.error.code : EIO;
			continue;
		}
		if (queue_push(c, &ev) < 0)
			return -1;
	}
}

/* ---- the Dock ---- */

WS_API int
ws_dock_activate(ws_connection_t *c, uint32_t app_id, uint32_t window)
{
	return send_int(c, WS_MSG_DOCK_ACTIVATE, window, (int32_t)app_id);
}

WS_API int
ws_dock_show_minimized(ws_connection_t *c, int on)
{
	return send_int(c, WS_MSG_DOCK_SHOW_MINIMIZED, 0, on ? 1 : 0);
}

WS_API int
ws_dock_set_edge(ws_connection_t *c, uint32_t edge, uint32_t thickness)
{
	ws_req_dock_edge_t m;

	req_init(c, &m.hdr, WS_MSG_DOCK_SET_EDGE, sizeof(m), 0);
	m.edge = edge;
	m.thickness = thickness;
	return send_oneway(c, &m.hdr);
}

WS_API int
ws_dock_restore(ws_connection_t *c, uint32_t window)
{
	ws_req_header_t h;

	req_init(c, &h, WS_MSG_DOCK_RESTORE, sizeof(h), window);
	return send_oneway(c, &h);
}

WS_API int
ws_dock_preview(ws_connection_t *c, uint32_t window, uint32_t max_size,
    ws_snapshot_t *out)
{
	ws_req_int_t m;
	ws_event_t ev;

	req_init(c, &m.hdr, WS_MSG_DOCK_PREVIEW, sizeof(m), window);
	m.value = (int32_t)max_size;
	m.reserved = 0;
	if (send_rpc(c, &m.hdr, NULL) < 0)
		return -1;
	for (;;) {
		if (wait_for_event(c, WS_EVENT_PREVIEW, &ev, RPC_TIMEOUT_MS) < 0)
			return -1;
		if (ev.hdr.window == window)
			return map_image(&ev.u.preview, out);
		/* an answer to an earlier, timed-out request */
		event_close_fd(&ev);
	}
}

static int
inject(ws_connection_t *c, uint32_t kind, uint32_t pressed, double x,
    double y, uint32_t button, const char *key)
{
	ws_req_inject_t m;

	req_init(c, &m.hdr, WS_MSG_INJECT, sizeof(m), 0);
	m.kind = kind;
	m.pressed = pressed;
	m.x = m.dx = x;
	m.y = m.dy = y;
	m.button = button;
	m.keycode = 0;
	memset(m.key_name, 0, sizeof(m.key_name));
	if (key != NULL)
		strlcpy(m.key_name, key, sizeof(m.key_name));
	if (send_oneway(c, &m.hdr) < 0)
		return -1;
	/* Make injected input synchronous so callers can sequence it. */
	return ws_sync(c);
}

WS_API int
ws_inject_motion(ws_connection_t *c, double x, double y)
{
	return inject(c, WS_INJECT_MOTION, 0, x, y, 0, NULL);
}

WS_API int
ws_inject_button(ws_connection_t *c, uint32_t button, int pressed)
{
	return inject(c, WS_INJECT_BUTTON, pressed ? 1 : 0, 0, 0, button, NULL);
}

WS_API int
ws_inject_key(ws_connection_t *c, const char *key_name, int pressed)
{
	return inject(c, WS_INJECT_KEY, pressed ? 1 : 0, 0, 0, 0, key_name);
}

WS_API int
ws_inject_key_tap(ws_connection_t *c, const char *key_name)
{
	return inject(c, WS_INJECT_KEY_TAP, 1, 0, 0, 0, key_name);
}

WS_API int
ws_inject_scroll(ws_connection_t *c, double dx, double dy)
{
	return inject(c, WS_INJECT_SCROLL, 0, dx, dy, 0, NULL);
}

WS_API int
ws_inject_scroll_finger(ws_connection_t *c, double dx, double dy)
{
	return inject(c, WS_INJECT_SCROLL_FINGER, 0, dx, dy, 0, NULL);
}

WS_API int
ws_inject_hold(ws_connection_t *c, int down)
{
	return inject(c, WS_INJECT_HOLD, down ? 1 : 0, 0, 0, 0, NULL);
}

WS_API int
ws_inject_swipe(ws_connection_t *c, uint32_t fingers, int phase, double dx,
    double dy)
{
	return inject(c, WS_INJECT_SWIPE, (uint32_t)phase, dx, dy, fingers,
	    NULL);
}

WS_API int
ws_inject_screen_size(ws_connection_t *c, uint32_t width, uint32_t height)
{
	return inject(c, WS_INJECT_SCREEN_SIZE, 0, (double)width,
	    (double)height, 0, NULL);
}

WS_API int
ws_overview(ws_connection_t *c, uint32_t request, ws_event_overview_t *state,
    void (*fn)(const ws_event_overview_item_t *, void *), void *ctx)
{
	ws_req_int_t m;
	ws_event_t ev;
	int r, err = 0;

	req_init(c, &m.hdr, WS_MSG_OVERVIEW, sizeof(m), 0);
	m.value = (int32_t)request;
	m.reserved = 0;
	if (send_oneway(c, &m.hdr) < 0)
		return -1;
	for (;;) {
		r = read_event(c, &ev);
		if (r < 0)
			return -1;
		if (r == 0) {
			r = wait_readable(c, RPC_TIMEOUT_MS);
			if (r <= 0) {
				if (r == 0)
					errno = ETIMEDOUT;
				return -1;
			}
			continue;
		}
		if (ev.hdr.type == WS_EVENT_LIST_END) {
			if (err != 0) {
				errno = err;
				return -1;
			}
			return 0;
		}
		if (ev.hdr.type == WS_EVENT_OVERVIEW_STATE) {
			if (state != NULL)
				*state = ev.u.overview;
			continue;
		}
		if (ev.hdr.type == WS_EVENT_OVERVIEW_ITEM) {
			if (fn != NULL)
				fn(&ev.u.overview_item, ctx);
			continue;
		}
		if (ev.hdr.type == WS_EVENT_ERROR &&
		    ev.u.error.request == WS_MSG_OVERVIEW) {
			err = ev.u.error.code ? ev.u.error.code : EIO;
			continue;
		}
		if (queue_push(c, &ev) < 0)
			return -1;
	}
}

WS_API int
ws_tiling(ws_connection_t *c, uint32_t op, uint32_t window, int32_t arg,
    ws_event_tiling_t *state,
    void (*fn)(const ws_event_tiling_item_t *, void *), void *ctx)
{
	ws_req_tiling_t m;
	ws_event_t ev;
	int r, err = 0;

	if ((c->server_flags & WS_SERVER_TILING) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	req_init(c, &m.hdr, WS_MSG_TILING, sizeof(m), window);
	m.op = op;
	m.arg = arg;
	if (send_oneway(c, &m.hdr) < 0)
		return -1;
	for (;;) {
		r = read_event(c, &ev);
		if (r < 0)
			return -1;
		if (r == 0) {
			r = wait_readable(c, RPC_TIMEOUT_MS);
			if (r <= 0) {
				if (r == 0)
					errno = ETIMEDOUT;
				return -1;
			}
			continue;
		}
		if (ev.hdr.type == WS_EVENT_LIST_END) {
			if (err != 0) {
				errno = err;
				return -1;
			}
			return 0;
		}
		if (ev.hdr.type == WS_EVENT_TILING_STATE) {
			if (state != NULL)
				*state = ev.u.tiling;
			continue;
		}
		if (ev.hdr.type == WS_EVENT_TILING_ITEM) {
			if (fn != NULL)
				fn(&ev.u.tiling_item, ctx);
			continue;
		}
		if (ev.hdr.type == WS_EVENT_ERROR &&
		    ev.u.error.request == WS_MSG_TILING) {
			err = ev.u.error.code ? ev.u.error.code : EIO;
			continue;
		}
		if (queue_push(c, &ev) < 0)
			return -1;
	}
}

WS_API const char *
ws_event_name(uint16_t type)
{
	static const char *const names[WS_EVENT_MAX] = {
		[WS_EVENT_WELCOME] = "welcome",
		[WS_EVENT_SCREEN_CHANGED] = "screen-changed",
		[WS_EVENT_APP_ACTIVATED] = "app-activated",
		[WS_EVENT_APP_DEACTIVATED] = "app-deactivated",
		[WS_EVENT_KEY_WINDOW_GAINED] = "key-window-gained",
		[WS_EVENT_KEY_WINDOW_LOST] = "key-window-lost",
		[WS_EVENT_WINDOW_FRAME] = "window-frame",
		[WS_EVENT_BUFFER_RELEASE] = "buffer-release",
		[WS_EVENT_FRAME_DONE] = "frame-done",
		[WS_EVENT_MOUSE_DOWN] = "mouse-down",
		[WS_EVENT_MOUSE_UP] = "mouse-up",
		[WS_EVENT_MOUSE_MOVED] = "mouse-moved",
		[WS_EVENT_MOUSE_DRAGGED] = "mouse-dragged",
		[WS_EVENT_MOUSE_ENTERED] = "mouse-entered",
		[WS_EVENT_MOUSE_EXITED] = "mouse-exited",
		[WS_EVENT_SCROLL] = "scroll",
		[WS_EVENT_KEY_DOWN] = "key-down",
		[WS_EVENT_KEY_UP] = "key-up",
		[WS_EVENT_FLAGS_CHANGED] = "flags-changed",
		[WS_EVENT_ERROR] = "error",
		[WS_EVENT_SNAPSHOT] = "snapshot",
		[WS_EVENT_WINDOW_INFO] = "window-info",
		[WS_EVENT_LIST_END] = "list-end",
		[WS_EVENT_KEYMAP] = "keymap",
		[WS_EVENT_MODIFIERS] = "modifiers",
		[WS_EVENT_KEYS_HELD] = "keys-held",
		[WS_EVENT_WINDOW_ORDERED] = "window-ordered",
		[WS_EVENT_WINDOW_RESTORE] = "window-restore",
		[WS_EVENT_DOCK_STATE] = "dock-state",
		[WS_EVENT_WINDOW_LIST_CHANGED] = "window-list-changed",
		[WS_EVENT_PREVIEW_CHANGED] = "preview-changed",
		[WS_EVENT_PREVIEW] = "preview",
		[WS_EVENT_OVERVIEW_STATE] = "overview-state",
		[WS_EVENT_OVERVIEW_ITEM] = "overview-item",
		[WS_EVENT_USABLE_AREA] = "usable-area",
		[WS_EVENT_SETTING] = "setting",
		[WS_EVENT_SETTINGS_CHANGED] = "settings-changed",
		[WS_EVENT_KEYBOARD_LAYOUT] = "keyboard-layout",
		[WS_EVENT_INPUT_DEVICE] = "input-device",
		[WS_EVENT_POINTER_OUTSIDE] = "pointer-outside",
		[WS_EVENT_POINTER_UNGRABBED] = "pointer-ungrabbed",
		[WS_EVENT_TILING_STATE] = "tiling-state",
		[WS_EVENT_TILING_ITEM] = "tiling-item",
		[WS_EVENT_DRAG] = "drag",
		[WS_EVENT_DRAG_STATUS] = "drag-status",
		[WS_EVENT_DRAG_END] = "drag-end",
		[WS_EVENT_DRAG_STATE] = "drag-state",
		[WS_EVENT_CAPTURE_STATE] = "capture-state",
		[WS_EVENT_CAPTURE_BUFFER] = "capture-buffer",
		[WS_EVENT_CAPTURE_FRAME] = "capture-frame",
		[WS_EVENT_CAPTURE_CONSENT] = "capture-consent",
		[WS_EVENT_CAPTURE_ACTIVITY] = "capture-activity",
		[WS_EVENT_CAPTURE_INFO] = "capture-info",
		[WS_EVENT_CAPTURE_REVOKED] = "capture-revoked",
	};

	if (type < WS_EVENT_MAX && names[type] != NULL)
		return names[type];
	return "unknown";
}

/* ---- settings (WS_SERVER_SETTINGS) ---- */

static int
need_settings(ws_connection_t *c)
{
	if ((c->server_flags & WS_SERVER_SETTINGS) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	return 0;
}

/*
 * Send a request that's answered on the socket with `type` events and
 * WS_EVENT_LIST_END; fn gets each. skip(ev) tells events of that type
 * that aren't part of the answer.
 */
static int
list_request(ws_connection_t *c, mach_msg_id_t id, uint16_t type,
    int (*skip)(const ws_event_t *), void (*fn)(const ws_event_t *, void *),
    void *ctx)
{
	ws_req_header_t h;
	ws_event_t ev;
	int r, err = 0;

	if (need_settings(c) < 0)
		return -1;
	req_init(c, &h, id, sizeof(h), 0);
	if (send_oneway(c, &h) < 0)
		return -1;
	for (;;) {
		r = read_event(c, &ev);
		if (r < 0)
			return -1;
		if (r == 0) {
			r = wait_readable(c, RPC_TIMEOUT_MS);
			if (r <= 0) {
				if (r == 0)
					errno = ETIMEDOUT;
				return -1;
			}
			continue;
		}
		if (ev.hdr.type == WS_EVENT_LIST_END) {
			if (err != 0) {
				errno = err;
				return -1;
			}
			return 0;
		}
		if (ev.hdr.type == type && (skip == NULL || !skip(&ev))) {
			fn(&ev, ctx);
			continue;
		}
		if (ev.hdr.type == WS_EVENT_ERROR &&
		    ev.u.error.request == (uint32_t)id) {
			err = ev.u.error.code ? ev.u.error.code : EIO;
			continue;
		}
		if (queue_push(c, &ev) < 0)
			return -1;
	}
}

static int
send_settings(ws_connection_t *c, ws_req_settings_t *m, uint32_t flags,
    size_t len, char *failed_key, size_t failed_len)
{
	ws_reply_t r;
	int ret;

	req_init(c, &m->hdr, WS_MSG_SET_SETTINGS,
	    offsetof(ws_req_settings_t, data) + ((len + 3) & ~(size_t)3), 0);
	m->flags = flags;
	m->length = (uint32_t)len;
	memset(&r, 0, sizeof(r));
	ret = send_rpc(c, &m->hdr, &r);
	if (failed_key != NULL && failed_len > 0 && r.u.settings.key[0] != '\0'
	    && failed_key[0] == '\0') {
		r.u.settings.key[sizeof(r.u.settings.key) - 1] = '\0';
		strlcpy(failed_key, r.u.settings.key, failed_len);
	}
	return ret;
}

WS_API int
ws_set_settings(ws_connection_t *c, uint32_t flags, const ws_setting_t *list,
    unsigned count, char *failed_key, size_t failed_len)
{
	ws_req_settings_t m;
	size_t len = 0, need, klen, vlen;
	unsigned i;
	int sent = 0;

	if (need_settings(c) < 0)
		return -1;
	if (failed_key != NULL && failed_len > 0)
		failed_key[0] = '\0';
	memset(&m, 0, sizeof(m));
	for (i = 0; i < count; i++) {
		const char *v = list[i].value != NULL ? list[i].value : "";

		klen = strlen(list[i].key);
		vlen = strlen(v);
		need = 1 + klen + 1 + vlen + 1;
		if (need > WS_MAX_SETTINGS_DATA || list[i].type <= 0 ||
		    list[i].type > 0xff) {
			if (failed_key != NULL && failed_len > 0)
				strlcpy(failed_key, list[i].key, failed_len);
			errno = EINVAL;
			return -1;
		}
		if (len + need > WS_MAX_SETTINGS_DATA) {
			/* Full: send what there is; only the first part
			   replaces or reloads. */
			if (send_settings(c, &m, flags, len, failed_key,
			    failed_len) < 0)
				return -1;
			flags &= ~(uint32_t)(WS_SETTINGS_REPLACE |
			    WS_SETTINGS_RELOAD_SYSTEM);
			memset(m.data, 0, sizeof(m.data));
			len = 0;
			sent = 1;
		}
		m.data[len] = (char)list[i].type;
		memcpy(m.data + len + 1, list[i].key, klen + 1);
		memcpy(m.data + len + 1 + klen + 1, v, vlen + 1);
		len += need;
	}
	if (len > 0 || !sent)
		return send_settings(c, &m, flags, len, failed_key, failed_len);
	return 0;
}

WS_API int
ws_set_setting(ws_connection_t *c, const char *key, int type,
    const char *value)
{
	ws_setting_t s;

	s.key = key;
	s.type = type;
	s.value = value;
	return ws_set_settings(c, 0, &s, 1, NULL, 0);
}

struct settings_cb {
	void	(*fn)(const ws_event_setting_t *, void *);
	void	*ctx;
};

static void
setting_cb(const ws_event_t *ev, void *arg)
{
	struct settings_cb *cb = arg;
	ws_event_setting_t s = ev->u.setting;

	s.key[sizeof(s.key) - 1] = '\0';
	s.value[sizeof(s.value) - 1] = '\0';
	cb->fn(&s, cb->ctx);
}

WS_API int
ws_get_settings(ws_connection_t *c,
    void (*fn)(const ws_event_setting_t *, void *), void *ctx)
{
	struct settings_cb cb = { fn, ctx };

	return list_request(c, WS_MSG_GET_SETTINGS, WS_EVENT_SETTING, NULL,
	    setting_cb, &cb);
}

WS_API int
ws_set_keyboard_layout(ws_connection_t *c, uint32_t index)
{
	ws_req_int_t m;

	if (need_settings(c) < 0)
		return -1;
	req_init(c, &m.hdr, WS_MSG_SET_KEYBOARD_LAYOUT, sizeof(m), 0);
	m.value = (int32_t)index;
	m.reserved = 0;
	return send_rpc(c, &m.hdr, NULL);
}

struct layout_cb {
	void	(*fn)(const ws_event_keyboard_layout_t *, void *);
	void	*ctx;
};

static int
layout_announcement(const ws_event_t *ev)
{
	return (ev->u.keyboard_layout.flags & WS_KEYBOARD_LAYOUT_CHANGED) != 0;
}

static void
layout_cb(const ws_event_t *ev, void *arg)
{
	struct layout_cb *cb = arg;

	cb->fn(&ev->u.keyboard_layout, cb->ctx);
}

WS_API int
ws_list_keyboard_layouts(ws_connection_t *c,
    void (*fn)(const ws_event_keyboard_layout_t *, void *), void *ctx)
{
	struct layout_cb cb = { fn, ctx };

	return list_request(c, WS_MSG_LIST_KEYBOARD_LAYOUTS,
	    WS_EVENT_KEYBOARD_LAYOUT, layout_announcement, layout_cb, &cb);
}

struct device_cb {
	void	(*fn)(const ws_event_input_device_t *, void *);
	void	*ctx;
};

static void
device_cb(const ws_event_t *ev, void *arg)
{
	struct device_cb *cb = arg;

	cb->fn(&ev->u.input_device, cb->ctx);
}

WS_API int
ws_list_input_devices(ws_connection_t *c,
    void (*fn)(const ws_event_input_device_t *, void *), void *ctx)
{
	struct device_cb cb = { fn, ctx };

	return list_request(c, WS_MSG_LIST_INPUT_DEVICES,
	    WS_EVENT_INPUT_DEVICE, NULL, device_cb, &cb);
}

/* ---- the user's settings file ---- */

WS_API int
ws_user_settings_path(char *buf, size_t len)
{
	const char *home = getenv("HOME");
	const char *dir = getenv("GNUSTEP_USER_DEFAULTS_DIR");
	int n;

	if (dir == NULL || *dir == '\0')
		dir = "Library/Preferences";	/* NextBSD's GNUstep.conf */
	if (*dir == '/')
		n = snprintf(buf, len, "%s/%s.plist", dir, WS_SETTINGS_DOMAIN);
	else {
		if (home == NULL || *home == '\0') {
			errno = ENOENT;
			return -1;
		}
		n = snprintf(buf, len, "%s/%s/%s.plist", home, dir,
		    WS_SETTINGS_DOMAIN);
	}
	if (n < 0 || (size_t)n >= len) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}

/* Copy the text up to </tag>, decoding XML's five entities. */
static const char *
xml_text(const char *p, const char *tag, char *out, size_t outlen)
{
	static const struct {
		const char *ent;
		char	ch;
	} ents[] = { { "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' },
	    { "&quot;", '"' }, { "&apos;", '\'' } };
	char close_tag[32];
	const char *e;
	size_t o = 0, i, n;

	snprintf(close_tag, sizeof(close_tag), "</%s>", tag);
	e = strstr(p, close_tag);
	if (e == NULL)
		return NULL;
	while (p < e && o + 1 < outlen) {
		if (*p == '&') {
			for (i = 0; i < sizeof(ents) / sizeof(ents[0]); i++) {
				n = strlen(ents[i].ent);
				if (strncmp(p, ents[i].ent, n) == 0)
					break;
			}
			if (i < sizeof(ents) / sizeof(ents[0])) {
				out[o++] = ents[i].ch;
				p += strlen(ents[i].ent);
				continue;
			}
		}
		out[o++] = *p++;
	}
	out[o] = '\0';
	return e + strlen(close_tag);
}

#define MAX_FILE_SETTINGS	64

WS_API int
ws_settings_load_file(ws_connection_t *c, const char *path, uint32_t flags,
    char *failed_key, size_t failed_len)
{
	struct {
		char	keys[MAX_FILE_SETTINGS][WS_MAX_SETTING_KEY];
		char	values[MAX_FILE_SETTINGS][256];
		ws_setting_t list[MAX_FILE_SETTINGS];
	} *st;
	unsigned n = 0;
	char key[128];
	const char *p;
	char *buf = NULL;
	FILE *f;
	long len;
	int type, r;

	if (need_settings(c) < 0)
		return -1;
	f = fopen(path, "r");
	if (f == NULL && errno != ENOENT)
		return -1;
	st = calloc(1, sizeof(*st));
	if (st == NULL) {
		if (f != NULL)
			fclose(f);
		return -1;
	}
	if (f != NULL) {
		if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
		    len > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0 ||
		    (buf = malloc((size_t)len + 1)) == NULL ||
		    fread(buf, 1, (size_t)len, f) != (size_t)len) {
			free(buf);
			free(st);
			fclose(f);
			errno = EIO;
			return -1;
		}
		buf[len] = '\0';
		fclose(f);
	}
	/* A flat <dict> of <key> and <string>, <integer>, <real>, <true/>
	   or <false/>, as NSUserDefaults writes it; other values (arrays,
	   dictionaries) aren't WindowServer settings and are skipped. */
	for (p = buf; p != NULL && (p = strstr(p, "<key>")) != NULL &&
	    n < MAX_FILE_SETTINGS;) {
		p = xml_text(p + 5, "key", key, sizeof(key));
		if (p == NULL)
			break;
		while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
			p++;
		st->values[n][0] = '\0';
		type = WS_SETTING_STRING;
		if (strncmp(p, "<true/>", 7) == 0) {
			strlcpy(st->values[n], "true", sizeof(st->values[n]));
			type = WS_SETTING_BOOLEAN;
		} else if (strncmp(p, "<false/>", 8) == 0) {
			strlcpy(st->values[n], "false", sizeof(st->values[n]));
			type = WS_SETTING_BOOLEAN;
		} else if (strncmp(p, "<string/>", 9) == 0) {
			;
		} else if (strncmp(p, "<string>", 8) == 0) {
			p = xml_text(p + 8, "string", st->values[n],
			    sizeof(st->values[n]));
		} else if (strncmp(p, "<integer>", 9) == 0) {
			type = WS_SETTING_INTEGER;
			p = xml_text(p + 9, "integer", st->values[n],
			    sizeof(st->values[n]));
		} else if (strncmp(p, "<real>", 6) == 0) {
			type = WS_SETTING_REAL;
			p = xml_text(p + 6, "real", st->values[n],
			    sizeof(st->values[n]));
		} else
			type = 0;
		if (p == NULL)
			break;
		if (type == 0 || strlen(key) >= WS_MAX_SETTING_KEY)
			continue;
		strlcpy(st->keys[n], key, sizeof(st->keys[n]));
		st->list[n].key = st->keys[n];
		st->list[n].type = type;
		st->list[n].value = st->values[n];
		n++;
	}
	free(buf);
	r = ws_set_settings(c, flags, st->list, n, failed_key, failed_len);
	free(st);
	return r;
}

/* ---- screen capture (WS_SERVER_CAPTURE) ---- */

static int
need_capture(ws_connection_t *c)
{
	if ((c->server_flags & WS_SERVER_CAPTURE) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	return 0;
}

static void
map_free(ws_connection_t *c, struct ws_capture_map *m)
{
	struct ws_capture_map **pp;

	for (pp = &c->maps; *pp != NULL; pp = &(*pp)->next)
		if (*pp == m) {
			*pp = m->next;
			break;
		}
	munmap(m->data, m->size);
	free(m);
}

static struct ws_capture_map *
map_find(ws_connection_t *c, uint32_t stream, uint32_t index,
    uint32_t generation)
{
	struct ws_capture_map *m;

	for (m = c->maps; m != NULL; m = m->next)
		if (m->stream == stream && m->index == index &&
		    m->generation == generation)
			return m;
	return NULL;
}

/* Buffers of `stream` (older than `generation`, or all if 0) are done
   with: unmap them now, or once the client releases them. */
static void
maps_retire(ws_connection_t *c, uint32_t stream, uint32_t generation)
{
	struct ws_capture_map *m, *next;

	for (m = c->maps; m != NULL; m = next) {
		next = m->next;
		if (m->stream != stream ||
		    (generation != 0 && m->generation >= generation))
			continue;
		if (m->held)
			m->gone = 1;
		else
			map_free(c, m);
	}
}

/* An event of a capture stream is handed to the client: keep the
   library's mappings in step. */
static void
capture_note(ws_connection_t *c, ws_event_t *ev)
{
	ws_event_capture_buffer_t *b = &ev->u.capture_buffer;
	struct ws_capture_map *m;
	void *p;

	switch (ev->hdr.type) {
	case WS_EVENT_CAPTURE_BUFFER:
		if (b->fd < 0)
			break;
		maps_retire(c, b->stream, b->generation);
		m = calloc(1, sizeof(*m));
		p = MAP_FAILED;
		if (m != NULL && b->stride >= b->width * 4 && b->height > 0)
			p = mmap(NULL, (size_t)b->stride * b->height, PROT_READ,
			    MAP_SHARED, b->fd, 0);
		close(b->fd);
		b->fd = -1;
		if (p == MAP_FAILED) {
			free(m);
			break;
		}
		m->stream = b->stream;
		m->index = b->index;
		m->generation = b->generation;
		m->stride = b->stride;
		m->data = p;
		m->size = (size_t)b->stride * b->height;
		m->next = c->maps;
		c->maps = m;
		break;
	case WS_EVENT_CAPTURE_FRAME:
		m = map_find(c, ev->u.capture_frame.stream,
		    ev->u.capture_frame.index, ev->u.capture_frame.generation);
		if (m != NULL)
			m->held = 1;
		break;
	case WS_EVENT_CAPTURE_STATE:
		if (ev->u.capture_state.state == WS_CAPTURE_DENIED ||
		    ev->u.capture_state.state == WS_CAPTURE_STOPPED)
			maps_retire(c, ev->u.capture_state.stream, 0);
		break;
	}
}

/*
 * Start the consent agent: launchd starts it on demand when its
 * LaunchAgent is loaded (a message to its Mach service), else run it.
 * Its own process, in its own session, with no descriptors of ours.
 */
static void
start_agent(void)
{
	const char *path = getenv(WS_CAPTURE_AGENT_ENV);
	const char *home = getenv("HOME");
	char log[PATH_MAX];
	mach_msg_header_t h;
	mach_port_t port;
	kern_return_t kr;
	pid_t pid;
	int fd, lfd;

	if (path != NULL && strcmp(path, "none") == 0)
		return;
	if (path == NULL || *path == '\0') {
		if (bootstrap_look_up(bootstrap_port, WS_CAPTURE_AGENT_SERVICE,
		    &port) == KERN_SUCCESS) {
			memset(&h, 0, sizeof(h));
			h.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
			h.msgh_size = sizeof(h);
			h.msgh_remote_port = port;
			h.msgh_id = 0;
			kr = mach_msg(&h, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
			    sizeof(h), 0, MACH_PORT_NULL, 0, MACH_PORT_NULL);
			mach_port_deallocate(mach_task_self(), port);
			if (kr == MACH_MSG_SUCCESS)
				return;
			/* a stale name: start it ourselves */
		}
		path = WS_CAPTURE_AGENT_PATH;
	}
	if (access(path, X_OK) < 0)
		return;
	log[0] = '\0';
	if (home != NULL && *home == '/')
		snprintf(log, sizeof(log), "%s/Library/Logs/ScreenCaptureAgent.log",
		    home);
	pid = fork();
	if (pid == 0) {
		(void)setsid();
		if (fork() != 0)
			_exit(0);
		fd = open("/dev/null", O_RDWR);
		lfd = log[0] ? open(log, O_WRONLY | O_CREAT | O_APPEND, 0600) : -1;
		if (fd >= 0) {
			dup2(fd, 0);
			dup2(lfd >= 0 ? lfd : fd, 1);
			dup2(lfd >= 0 ? lfd : fd, 2);
		}
		closefrom(3);
		execl(path, path, (char *)NULL);
		_exit(127);
	}
	if (pid > 0)
		while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
			;
}

WS_API uint32_t
ws_capture_start_for(ws_connection_t *c, uint32_t window, uint32_t flags,
    uint32_t width, uint32_t height, uint32_t max_fps, int32_t pid,
    const char *name, uint32_t *reply_flags)
{
	ws_req_capture_t m;
	ws_reply_t r;

	if (need_capture(c) < 0)
		return 0;
	req_init(c, &m.hdr, WS_MSG_CAPTURE_START, sizeof(m), window);
	m.flags = flags;
	m.max_fps = max_fps;
	m.width = width;
	m.height = height;
	m.for_pid = pid;
	m.reserved = 0;
	memset(m.for_name, 0, sizeof(m.for_name));
	if (name != NULL)
		strlcpy(m.for_name, name, sizeof(m.for_name));
	if (send_rpc(c, &m.hdr, &r) < 0)
		return 0;
	if (reply_flags != NULL)
		*reply_flags = r.u.capture.flags;
	if (r.u.capture.flags & WS_CAPTURE_NO_AGENT)
		start_agent();
	return r.u.capture.stream;
}

WS_API uint32_t
ws_capture_start(ws_connection_t *c, uint32_t window, uint32_t flags,
    uint32_t width, uint32_t height, uint32_t max_fps, uint32_t *reply_flags)
{
	return ws_capture_start_for(c, window, flags, width, height, max_fps,
	    0, NULL, reply_flags);
}

WS_API int
ws_capture_stop(ws_connection_t *c, uint32_t stream)
{
	if (need_capture(c) < 0)
		return -1;
	return send_int(c, WS_MSG_CAPTURE_STOP, 0, (int32_t)stream);
}

WS_API const void *
ws_capture_frame_data(ws_connection_t *c, const ws_event_capture_frame_t *f,
    uint32_t *stride)
{
	struct ws_capture_map *m;

	m = map_find(c, f->stream, f->index, f->generation);
	if (m == NULL) {
		errno = ENOENT;
		return NULL;
	}
	if (stride != NULL)
		*stride = m->stride;
	return m->data;
}

WS_API int
ws_capture_release(ws_connection_t *c, const ws_event_capture_frame_t *f)
{
	ws_req_capture_release_t m;
	struct ws_capture_map *map;

	map = map_find(c, f->stream, f->index, f->generation);
	if (map != NULL) {
		map->held = 0;
		if (map->gone) {
			map_free(c, map);
			return 0;	/* the server has forgotten it */
		}
	}
	req_init(c, &m.hdr, WS_MSG_CAPTURE_RELEASE, sizeof(m), 0);
	m.stream = f->stream;
	m.index = f->index;
	m.generation = f->generation;
	m.reserved = 0;
	return send_oneway(c, &m.hdr);
}

WS_API int
ws_capture_image(ws_connection_t *c, uint32_t window, uint32_t flags,
    int timeout_ms, ws_snapshot_t *out)
{
	struct ws_capture_map *map;
	ws_event_t ev;
	uint32_t stream;
	uint64_t deadline;
	struct timespec ts;
	int r, left;

	memset(out, 0, sizeof(*out));
	stream = ws_capture_start(c, window, flags | WS_CAPTURE_ONESHOT, 0, 0,
	    0, NULL);
	if (stream == 0)
		return -1;
	if (timeout_ms < 0)
		timeout_ms = (WS_CAPTURE_CONSENT_TIMEOUT + 10) * 1000;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	deadline = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000 +
	    (uint64_t)timeout_ms;
	for (;;) {
		r = read_event(c, &ev);
		if (r < 0)
			return -1;
		if (r == 0) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			left = (int)((int64_t)deadline -
			    (int64_t)((uint64_t)ts.tv_sec * 1000 +
			    (uint64_t)ts.tv_nsec / 1000000));
			if (left <= 0 || (r = wait_readable(c, left)) == 0) {
				(void)ws_capture_stop(c, stream);
				errno = ETIMEDOUT;
				return -1;
			}
			if (r < 0)
				return -1;
			continue;
		}
		if (ev.hdr.type < WS_EVENT_CAPTURE_STATE ||
		    ev.hdr.type > WS_EVENT_CAPTURE_FRAME ||
		    ev.u.capture_state.stream != stream) {
			if (queue_push(c, &ev) < 0)
				return -1;
			continue;
		}
		capture_note(c, &ev);
		if (ev.hdr.type == WS_EVENT_CAPTURE_STATE &&
		    (ev.u.capture_state.state == WS_CAPTURE_DENIED ||
		    ev.u.capture_state.state == WS_CAPTURE_STOPPED)) {
			errno = ev.u.capture_state.error ?
			    ev.u.capture_state.error : EIO;
			return -1;
		}
		if (ev.hdr.type != WS_EVENT_CAPTURE_FRAME)
			continue;
		map = map_find(c, stream, ev.u.capture_frame.index,
		    ev.u.capture_frame.generation);
		if (map == NULL) {
			errno = EIO;
			return -1;
		}
		/* the picture is the caller's now (ws_snapshot_free) */
		out->stride = map->stride;
		out->width = map->stride / 4;
		out->height = (uint32_t)(map->size / map->stride);
		out->format = WS_FORMAT_XRGB8888;
		out->pixels = map->data;
		out->map_size = map->size;
		{
			struct ws_capture_map **pp;

			for (pp = &c->maps; *pp != NULL; pp = &(*pp)->next)
				if (*pp == map) {
					*pp = map->next;
					break;
				}
			free(map);
		}
		return 0;
	}
}

WS_API int
ws_capture_consent(ws_connection_t *c, uint32_t request, uint32_t decision)
{
	ws_req_capture_consent_t m;

	if (need_capture(c) < 0)
		return -1;
	req_init(c, &m.hdr, WS_MSG_CAPTURE_CONSENT, sizeof(m), 0);
	m.request = request;
	m.decision = decision;
	return send_oneway(c, &m.hdr);
}

WS_API int
ws_capture_list(ws_connection_t *c,
    void (*fn)(const ws_event_capture_info_t *, void *), void *ctx)
{
	ws_req_header_t h;
	ws_event_t ev;
	int r, err = 0;

	if (need_capture(c) < 0)
		return -1;
	req_init(c, &h, WS_MSG_CAPTURE_LIST, sizeof(h), 0);
	if (send_oneway(c, &h) < 0)
		return -1;
	for (;;) {
		r = read_event(c, &ev);
		if (r < 0)
			return -1;
		if (r == 0) {
			r = wait_readable(c, RPC_TIMEOUT_MS);
			if (r <= 0) {
				if (r == 0)
					errno = ETIMEDOUT;
				return -1;
			}
			continue;
		}
		if (ev.hdr.type == WS_EVENT_LIST_END) {
			if (err != 0) {
				errno = err;
				return -1;
			}
			return 0;
		}
		if (ev.hdr.type == WS_EVENT_CAPTURE_INFO) {
			if (fn != NULL)
				fn(&ev.u.capture_info, ctx);
			continue;
		}
		if (ev.hdr.type == WS_EVENT_ERROR &&
		    ev.u.error.request == WS_MSG_CAPTURE_LIST) {
			err = ev.u.error.code ? ev.u.error.code : EIO;
			continue;
		}
		if (queue_push(c, &ev) < 0)
			return -1;
	}
}

WS_API int
ws_capture_revoke(ws_connection_t *c, uint32_t stream, uint32_t flags,
    const char *key)
{
	ws_req_capture_revoke_t m;

	if (need_capture(c) < 0)
		return -1;
	req_init(c, &m.hdr, WS_MSG_CAPTURE_REVOKE, sizeof(m), 0);
	m.stream = stream;
	m.flags = flags;
	memset(m.key, 0, sizeof(m.key));
	if (key != NULL)
		strlcpy(m.key, key, sizeof(m.key));
	return send_oneway(c, &m.hdr);
}
