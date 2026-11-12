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

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <WindowServer/WSProtocol.h>
#include <WindowServer/WSClient.h>

#define WS_API __attribute__((visibility("default")))

#define RPC_TIMEOUT_MS		10000
#define WELCOME_TIMEOUT_MS	5000

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
	ws_screen_info_t screen;
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
	kr = mach_msg(&h->head, MACH_SEND_MSG, h->head.msgh_size, 0,
	    MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
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
	kr = mach_msg(&h->head, MACH_SEND_MSG, h->head.msgh_size, 0,
	    MACH_PORT_NULL, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
		errno = kr_errno(kr);
		return -1;
	}
	for (;;) {
		memset(&rb, 0, sizeof(rb));
		kr = mach_msg(&rb.r.head, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0,
		    sizeof(rb), c->reply, RPC_TIMEOUT_MS, MACH_PORT_NULL);
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

static void
note_event(ws_connection_t *c, const ws_event_t *ev)
{
	if (ev->hdr.type == WS_EVENT_SCREEN_CHANGED &&
	    ev->u.screen.screen_id == c->screen.screen_id)
		c->screen = ev->u.screen;
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

	req_init(c, &m.hdr, WS_MSG_WINDOW_CREATE, sizeof(m), 0);
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
	return send_int(c, WS_MSG_WINDOW_COMMIT, window, (int32_t)flags);
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
ws_deactivate(ws_connection_t *c)
{
	ws_req_header_t h;

	req_init(c, &h, WS_MSG_DEACTIVATE, sizeof(h), 0);
	return send_oneway(c, &h);
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
	};

	if (type < WS_EVENT_MAX && names[type] != NULL)
		return names[type];
	return "unknown";
}
