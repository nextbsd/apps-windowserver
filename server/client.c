/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Clients: the per-client Unix socket (events out, buffer fds in),
 * client lifetime and window buffers.
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ucred.h>
#include <sys/un.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

#define AUTH_TIMEOUT_USEC	(10 * 1000000u)
#define OUTQ_DROP_MOTION	256
#define OUTQ_MAX		4096

/* ---- listening socket ---- */

static int
make_socket_dir(struct ws_server *srv, const char *dir)
{
	struct stat st;

	if (mkdir(dir, 0755) == 0) {
		srv->socket_dir_created = true;
		strlcpy(srv->socket_dir, dir, sizeof(srv->socket_dir));
		return 0;
	}
	if (errno != EEXIST) {
		ws_err("mkdir %s: %s", dir, strerror(errno));
		return -1;
	}
	if (lstat(dir, &st) < 0 || !S_ISDIR(st.st_mode) ||
	    st.st_uid != geteuid()) {
		ws_err("%s exists but isn't a directory owned by uid %d", dir,
		    (int)geteuid());
		return -1;
	}
	return 0;
}

/* Something is accepting connections on path: another server's socket. */
static bool
socket_in_use(const char *path)
{
	struct sockaddr_un sun;
	int fd, in_use;

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return false;
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, path, sizeof(sun.sun_path));
	in_use = connect(fd, (struct sockaddr *)&sun, sizeof(sun)) == 0;
	close(fd);
	return in_use;
}

int
socket_listen(struct ws_server *srv)
{
	struct sockaddr_un sun;
	struct stat st;
	char dir[WS_MAX_SOCKET_PATH];
	int fd;

	if (srv->cfg.socket_path[0] != '\0') {
		strlcpy(srv->socket_path, srv->cfg.socket_path,
		    sizeof(srv->socket_path));
	} else {
		if (geteuid() == 0)
			strlcpy(dir, "/var/run/WindowServer", sizeof(dir));
		else
			snprintf(dir, sizeof(dir), "/tmp/WindowServer-%d",
			    (int)geteuid());
		if (make_socket_dir(srv, dir) < 0)
			return -1;
		/*
		 * A server under another service name (a headless test server
		 * beside the real one) gets its own socket, so neither replaces
		 * or removes the other's. Clients are told the path over Mach.
		 */
		if (strcmp(srv->cfg.service_name, WS_SERVICE_NAME) == 0)
			snprintf(srv->socket_path, sizeof(srv->socket_path),
			    "%s/ws.sock", dir);
		else
			snprintf(srv->socket_path, sizeof(srv->socket_path),
			    "%s/%s.sock", dir, srv->cfg.service_name);
	}
	if (lstat(srv->socket_path, &st) == 0) {
		if (!S_ISSOCK(st.st_mode)) {
			ws_err("%s exists and isn't a socket", srv->socket_path);
			return -1;
		}
		/* Only a stale socket is replaced, never a live server's. */
		if (socket_in_use(srv->socket_path)) {
			ws_err("another WindowServer is listening on %s",
			    srv->socket_path);
			return -1;
		}
		unlink(srv->socket_path);
	}

	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		ws_err("socket: %s", strerror(errno));
		return -1;
	}
	memset(&sun, 0, sizeof(sun));
	sun.sun_family = AF_UNIX;
	strlcpy(sun.sun_path, srv->socket_path, sizeof(sun.sun_path));
	if (bind(fd, (struct sockaddr *)&sun, sizeof(sun)) < 0) {
		ws_err("bind %s: %s", srv->socket_path, strerror(errno));
		close(fd);
		return -1;
	}
	/* Anyone may connect; a connection is useless without the token
	 * handed out over Mach. */
	chmod(srv->socket_path, 0666);
	if (listen(fd, 64) < 0) {
		ws_err("listen: %s", strerror(errno));
		close(fd);
		unlink(srv->socket_path);
		return -1;
	}
	srv->listen_fd = fd;
	ws_kq_add_fd(srv, fd, EVFILT_READ, (void *)(uintptr_t)SRC_LISTEN);
	ws_info("client socket %s", srv->socket_path);
	return 0;
}

void
socket_cleanup(struct ws_server *srv)
{
	if (srv->listen_fd >= 0) {
		close(srv->listen_fd);
		srv->listen_fd = -1;
		unlink(srv->socket_path);
	}
	if (srv->socket_dir_created)
		rmdir(srv->socket_dir);
}

void
socket_accept(struct ws_server *srv)
{
	struct ws_conn *conn;
	int fd;

	for (;;) {
		fd = accept4(srv->listen_fd, NULL, NULL,
		    SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (fd < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != EINTR && errno != ECONNABORTED)
				ws_warn("accept: %s", strerror(errno));
			if (errno == EINTR || errno == ECONNABORTED)
				continue;
			return;
		}
		conn = calloc(1, sizeof(*conn));
		if (conn == NULL) {
			close(fd);
			continue;
		}
		conn->src = SRC_CONN;
		conn->fd = fd;
		conn->accepted_usec = ws_now_usec();
		conn->next = srv->conns;
		srv->conns = conn;
		ws_kq_add_fd(srv, fd, EVFILT_READ, conn);
	}
}

static void
conn_close(struct ws_conn *conn)
{
	if (conn->fd >= 0)
		close(conn->fd);
	conn->fd = -1;
	conn->dead = true;
}

void
screen_info(struct ws_server *srv, ws_screen_info_t *si)
{
	memset(si, 0, sizeof(*si));
	si->screen_id = 1;
	si->width = srv->width;
	si->height = srv->height;
	si->scale = 1;
	si->refresh_mhz = srv->refresh_mhz;
	si->mm_width = srv->mm_width;
	si->mm_height = srv->mm_height;
	strlcpy(si->name, srv->screen_name, sizeof(si->name));
}

void
conn_readable(struct ws_server *srv, struct ws_conn *conn)
{
	ws_sock_auth_t auth;
	struct ws_client *c;
	struct xucred xuc;
	socklen_t len;
	ws_event_t ev;
	ssize_t n;
	int sz;

	if (conn->dead)
		return;
	/* Wait until the whole AUTH message is there, then take it. */
	n = recv(conn->fd, &auth, sizeof(auth), MSG_DONTWAIT | MSG_PEEK);
	if (n < 0 && (errno == EAGAIN || errno == EINTR))
		return;
	if (n > 0 && n < (ssize_t)sizeof(auth))
		return;
	if (n == (ssize_t)sizeof(auth))
		n = recv(conn->fd, &auth, sizeof(auth), MSG_DONTWAIT);
	if (n != (ssize_t)sizeof(auth) || auth.hdr.type != WS_SOCK_AUTH ||
	    auth.hdr.size != sizeof(auth)) {
		conn_close(conn);
		return;
	}
	c = client_lookup(srv, auth.client_id);
	if (c == NULL || c->token != auth.token || c->sock >= 0) {
		ws_warn("socket auth failed for client %u", auth.client_id);
		conn_close(conn);
		return;
	}
	len = sizeof(xuc);
	memset(&xuc, 0, sizeof(xuc));
	if (getsockopt(conn->fd, 0 /* SOL_LOCAL */, LOCAL_PEERCRED, &xuc,
	    &len) == 0 && xuc.cr_version == XUCRED_VERSION) {
		if (xuc.cr_pid != 0 && c->pid > 0 && xuc.cr_pid != c->pid) {
			ws_warn("client %u: socket pid %d != Mach pid %d",
			    c->id, (int)xuc.cr_pid, (int)c->pid);
			conn_close(conn);
			return;
		}
	}

	c->sock = conn->fd;
	conn->fd = -1;
	conn->dead = true;
	sz = 256 * 1024;
	setsockopt(c->sock, SOL_SOCKET, SO_SNDBUF, &sz, sizeof(sz));
	ws_kq_add_fd(srv, c->sock, EVFILT_READ, c);

	event_init(srv, &ev, WS_EVENT_WELCOME, sizeof(ev.u.welcome), 0);
	ev.u.welcome.client_id = c->id;
	ev.u.welcome.protocol_version = WS_PROTOCOL_VERSION;
	ev.u.welcome.server_flags = (srv->headless ? WS_SERVER_HEADLESS : 0) |
	    (srv->cursor_hw ? WS_SERVER_HW_CURSOR : 0) | WS_SERVER_FEATURES;
	ev.u.welcome.screen_count = 1;
	screen_info(srv, &ev.u.welcome.screen);
	ev.u.welcome.usable = srv->usable;
	ev.u.welcome.menu_bar = srv->usable_menu_bar;
	client_send(srv, c, &ev, -1);
	if (c->flags & WS_CONNECT_XKB)
		keyboard_client_ready(srv, c);
	if (c->flags & WS_CONNECT_DOCK)
		dock_client_ready(srv, c);
	ws_info("client %u (%s, pid %d) connected", c->id, c->app_name,
	    (int)c->pid);
}

/* ---- events out ---- */

void
event_init(struct ws_server *srv, ws_event_t *ev, uint16_t type,
    size_t payload, uint32_t window)
{
	memset(ev, 0, sizeof(*ev));
	ev->hdr.type = type;
	ev->hdr.size = (uint16_t)(sizeof(ev->hdr) + payload);
	ev->hdr.serial = ++srv->event_serial;
	ev->hdr.time_usec = ws_now_usec();
	ev->hdr.window = window;
}

static ssize_t
sock_sendmsg(int sock, const void *data, size_t len, int fd)
{
	union {
		struct cmsghdr	hdr;
		char		buf[CMSG_SPACE(sizeof(int))];
	} cm;
	struct msghdr msg;
	struct iovec iov;

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
	return sendmsg(sock, &msg, MSG_DONTWAIT | MSG_NOSIGNAL);
}

static void
outq_push(struct ws_server *srv, struct ws_client *c, const void *data,
    size_t len, int fd)
{
	struct ws_outmsg *m;

	m = malloc(sizeof(*m) + len);
	if (m == NULL) {
		c->doomed = true;
		return;
	}
	m->next = NULL;
	m->len = len;
	m->off = 0;
	m->fd = fd >= 0 ? dup(fd) : -1;
	memcpy(m->data, data, len);
	if (c->outq == NULL) {
		c->outq_tail = &c->outq;
		ws_kq_add_fd(srv, c->sock, EVFILT_WRITE, c);
	}
	*c->outq_tail = m;
	c->outq_tail = &m->next;
	c->outq_len++;
}

/* Sends ev (and optionally fd, which stays owned by the caller). */
void
client_send(struct ws_server *srv, struct ws_client *c, ws_event_t *ev, int fd)
{
	ssize_t n;

	if (c == NULL || c->dead || c->doomed || c->sock < 0)
		return;
	if (c->outq != NULL) {
		if (c->outq_len >= OUTQ_MAX) {
			ws_warn("client %u (%s) isn't reading events; "
			    "disconnecting", c->id, c->app_name);
			c->doomed = true;
			return;
		}
		if (c->outq_len >= OUTQ_DROP_MOTION &&
		    (ev->hdr.type == WS_EVENT_MOUSE_MOVED ||
		    ev->hdr.type == WS_EVENT_MOUSE_DRAGGED))
			return;
		outq_push(srv, c, ev, ev->hdr.size, fd);
		return;
	}
	n = sock_sendmsg(c->sock, ev, ev->hdr.size, fd);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ENOBUFS) {
			outq_push(srv, c, ev, ev->hdr.size, fd);
			return;
		}
		ws_debug("client %u: send: %s", c->id, strerror(errno));
		c->doomed = true;
		return;
	}
	if ((size_t)n < ev->hdr.size)	/* the fd went with the first part */
		outq_push(srv, c, (uint8_t *)ev + n, ev->hdr.size - (size_t)n, -1);
}

void
client_writable(struct ws_server *srv, struct ws_client *c)
{
	struct ws_outmsg *m;
	ssize_t n;

	if (c->dead)
		return;
	while ((m = c->outq) != NULL) {
		n = sock_sendmsg(c->sock, m->data + m->off, m->len - m->off,
		    m->fd);
		if (n < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK ||
			    errno == ENOBUFS)
				return;
			client_destroy(srv, c);
			return;
		}
		if (m->fd >= 0) {
			close(m->fd);
			m->fd = -1;
		}
		m->off += (size_t)n;
		if (m->off < m->len)
			return;
		c->outq = m->next;
		c->outq_len--;
		if (m->fd >= 0)
			close(m->fd);
		free(m);
	}
	c->outq_tail = &c->outq;
	ws_kq_del_fd(srv, c->sock, EVFILT_WRITE);
}

void
client_error(struct ws_server *srv, struct ws_client *c, uint32_t request,
    uint32_t window, int code, const char *msg)
{
	ws_event_t ev;

	ws_debug("client %u: request 0x%x window %u: %s", c->id, request,
	    window, msg);
	event_init(srv, &ev, WS_EVENT_ERROR, sizeof(ev.u.error), window);
	ev.u.error.request = request;
	ev.u.error.code = code;
	strlcpy(ev.u.error.message, msg, sizeof(ev.u.error.message));
	client_send(srv, c, &ev, -1);
}

/* ---- buffers ---- */

struct ws_buffer *
client_buffer(struct ws_client *c, uint32_t id)
{
	struct ws_buffer *b;

	for (b = c->buffers; b != NULL; b = b->next)
		if (b->id == id)
			return b;
	return NULL;
}

void
buffer_unref(struct ws_server *srv, struct ws_buffer *b)
{
	(void)srv;
	if (b == NULL || --b->refs > 0)
		return;
	if (b->image != NULL)
		pixman_image_unref(b->image);
	if (b->map != NULL && b->map != MAP_FAILED)
		munmap(b->map, b->map_size);
	free(b);
}

static void
buffer_create(struct ws_server *srv, struct ws_client *c,
    const ws_sock_buffer_t *m, int fd)
{
	struct ws_buffer *b;
	struct stat st;
	uint64_t need;
	int seals;

	if (m->buffer_id == 0 || client_buffer(c, m->buffer_id) != NULL) {
		client_error(srv, c, WS_SOCK_BUFFER_CREATE, 0, EEXIST,
		    "bad or duplicate buffer id");
		return;
	}
	if ((m->format != WS_FORMAT_ARGB8888 &&
	    m->format != WS_FORMAT_XRGB8888) || m->width == 0 ||
	    m->height == 0 || m->width > 16384 || m->height > 16384 ||
	    m->stride < m->width * 4 || (m->stride & 3) != 0 ||
	    (m->offset & 3) != 0) {
		client_error(srv, c, WS_SOCK_BUFFER_CREATE, 0, EINVAL,
		    "bad buffer geometry or format");
		return;
	}
	need = m->offset + (uint64_t)m->stride * m->height;
	if (fstat(fd, &st) < 0 || (uint64_t)st.st_size < need) {
		client_error(srv, c, WS_SOCK_BUFFER_CREATE, 0, EINVAL,
		    "buffer fd too small");
		return;
	}
	seals = fcntl(fd, F_GET_SEALS);
	if (seals < 0 || (seals & F_SEAL_SHRINK) == 0) {
		client_error(srv, c, WS_SOCK_BUFFER_CREATE, 0, EPERM,
		    "buffer fd must be sealed with F_SEAL_SHRINK");
		return;
	}
	b = calloc(1, sizeof(*b));
	if (b == NULL)
		return;
	b->map_size = (size_t)need;
	b->map = mmap(NULL, b->map_size, PROT_READ, MAP_SHARED, fd, 0);
	if (b->map == MAP_FAILED) {
		client_error(srv, c, WS_SOCK_BUFFER_CREATE, 0, errno,
		    "mmap failed");
		free(b);
		return;
	}
	b->id = m->buffer_id;
	b->owner = c;
	b->width = m->width;
	b->height = m->height;
	b->stride = m->stride;
	b->scale = m->scale ? m->scale : 1;
	b->format = m->format;
	/* x8r8g8b8 makes pixman treat every pixel as opaque. */
	b->image = pixman_image_create_bits(m->format == WS_FORMAT_XRGB8888 ?
	    PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8, (int)m->width,
	    (int)m->height, (uint32_t *)(void *)((char *)b->map + m->offset),
	    (int)m->stride);
	if (b->image == NULL) {
		munmap(b->map, b->map_size);
		free(b);
		return;
	}
	if (b->scale != 1) {
		pixman_transform_t t;

		pixman_transform_init_scale(&t,
		    pixman_int_to_fixed((int)b->scale),
		    pixman_int_to_fixed((int)b->scale));
		pixman_image_set_transform(b->image, &t);
		pixman_image_set_filter(b->image, PIXMAN_FILTER_BILINEAR,
		    NULL, 0);
	}
	b->refs = 1;
	b->client_ref = true;
	b->next = c->buffers;
	c->buffers = b;
	ws_debug("client %u: buffer %u %ux%u", c->id, b->id, b->width,
	    b->height);
}

/* ---- client socket input ---- */

static int
client_fd_take(struct ws_client *c)
{
	int fd;

	if (c->nfds == 0)
		return -1;
	fd = c->fds[0];
	memmove(c->fds, c->fds + 1, (size_t)(c->nfds - 1) * sizeof(int));
	c->nfds--;
	return fd;
}

/* Handle every whole message in c->rbuf. */
static void
client_parse(struct ws_server *srv, struct ws_client *c)
{
	ws_sock_header_t hdr;
	ws_sock_buffer_t bm;
	int fd;

	while (!c->dead && c->rlen >= sizeof(hdr)) {
		memcpy(&hdr, c->rbuf, sizeof(hdr));
		if (hdr.size < sizeof(hdr) || hdr.size > sizeof(c->rbuf)) {
			ws_warn("client %u: bad socket message", c->id);
			c->doomed = true;
			return;
		}
		if (c->rlen < hdr.size)
			return;
		if (hdr.type == WS_SOCK_BUFFER_CREATE &&
		    hdr.size == sizeof(bm)) {
			memcpy(&bm, c->rbuf, sizeof(bm));
			fd = client_fd_take(c);
			if (fd < 0)
				client_error(srv, c, hdr.type, 0, EBADF,
				    "buffer message without a descriptor");
			else {
				buffer_create(srv, c, &bm, fd);
				close(fd);
			}
		} else {
			client_error(srv, c, hdr.type, 0, EINVAL,
			    "bad socket message");
		}
		c->rlen -= hdr.size;
		memmove(c->rbuf, c->rbuf + hdr.size, c->rlen);
	}
}

void
client_readable(struct ws_server *srv, struct ws_client *c)
{
	union {
		struct cmsghdr	hdr;
		char		buf[CMSG_SPACE(sizeof(int) * 8)];
	} cm;
	struct cmsghdr *cmsg;
	struct msghdr msg;
	struct iovec iov;
	ssize_t n;
	size_t i, nfd;
	int fd;

	while (!c->dead && !c->doomed && c->sock >= 0) {
		memset(&msg, 0, sizeof(msg));
		iov.iov_base = c->rbuf + c->rlen;
		iov.iov_len = sizeof(c->rbuf) - c->rlen;
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = cm.buf;
		msg.msg_controllen = sizeof(cm.buf);
		n = recvmsg(c->sock, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return;
			client_destroy(srv, c);
			return;
		}
		if (n == 0) {
			ws_info("client %u (%s) disconnected", c->id,
			    c->app_name);
			client_destroy(srv, c);
			return;
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
		client_parse(srv, c);
	}
}

/* ---- lifetime ---- */

struct ws_client *
client_lookup(struct ws_server *srv, uint32_t id)
{
	struct ws_client *c;

	for (c = srv->clients; c != NULL; c = c->next)
		if (c->id == id && !c->dead)
			return c;
	return NULL;
}

void
client_destroy(struct ws_server *srv, struct ws_client *c)
{
	struct ws_window *w, *next;
	struct ws_buffer *b;
	struct ws_outmsg *m;

	if (c->dead)
		return;
	pointer_ungrab(srv, c, false);
	c->dead = true;
	if (srv->kbd_focus == c)
		srv->kbd_focus = NULL;
	ws_debug("destroying client %u (%s)", c->id, c->app_name);

	for (w = srv->windows; w != NULL; w = next) {
		next = w->next;
		if (w->owner == c)
			window_destroy(srv, w);
	}
	app_member_gone(srv, c);
	c->key_window = NULL;
	if (c->flags & WS_CONNECT_DOCK)
		dock_client_gone(srv, c);
	if (srv->active == c || srv->dock_front == c)
		app_deactivate(srv, c);
	/* After the deactivation: the next active app's bar may count. */
	if (srv->menu_bar_app == c)
		menu_bar_app_gone(srv, c, NULL);
	keyboard_focus_update(srv);

	if (c->app_icon != NULL) {
		pixman_image_unref(c->app_icon);
		c->app_icon = NULL;
	}
	if (c->cursor_buffer != NULL) {
		cursor_buffer_gone(srv, c->cursor_buffer);
		buffer_unref(srv, c->cursor_buffer);
		c->cursor_buffer = NULL;
	}
	while ((b = c->buffers) != NULL) {
		c->buffers = b->next;
		b->client_ref = false;
		b->owner = NULL;
		buffer_unref(srv, b);
	}
	while ((m = c->outq) != NULL) {
		c->outq = m->next;
		if (m->fd >= 0)
			close(m->fd);
		free(m);
	}
	c->outq_len = 0;
	while (c->nfds > 0)
		close(client_fd_take(c));
	if (c->sock >= 0) {
		close(c->sock);
		c->sock = -1;
	}
	cursor_update(srv);
}

/* Free dead clients and connections; called between kevent batches. */
void
clients_reap(struct ws_server *srv)
{
	struct ws_client **cp, *c;
	struct ws_conn **np, *n;

	for (c = srv->clients; c != NULL; c = c->next)
		if (c->doomed && !c->dead)
			client_destroy(srv, c);

	for (cp = &srv->clients; (c = *cp) != NULL;) {
		if (c->dead) {
			*cp = c->next;
			free(c);
		} else
			cp = &c->next;
	}
	for (np = &srv->conns; (n = *np) != NULL;) {
		if (n->dead) {
			*np = n->next;
			free(n);
		} else
			np = &n->next;
	}
}

void
clients_housekeeping(struct ws_server *srv)
{
	uint64_t now = ws_now_usec();
	struct ws_client *c;
	struct ws_conn *n;

	for (c = srv->clients; c != NULL; c = c->next)
		if (!c->dead && c->sock < 0 &&
		    now - c->hello_usec > AUTH_TIMEOUT_USEC) {
			ws_info("client %u (%s) never connected its socket",
			    c->id, c->app_name);
			client_destroy(srv, c);
		}
	for (n = srv->conns; n != NULL; n = n->next)
		if (!n->dead && now - n->accepted_usec > AUTH_TIMEOUT_USEC)
			conn_close(n);
}
