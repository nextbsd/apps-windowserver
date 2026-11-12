/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Mach request dispatch (runs on the main thread).
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

static void
reply_send(struct ws_mach_item *item, ws_reply_t *r, size_t size)
{
	mach_msg_header_t *req = &item->msg.head;
	mach_msg_return_t kr;

	r->head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(req->msgh_bits), 0);
	r->head.msgh_size = (mach_msg_size_t)size;
	r->head.msgh_remote_port = req->msgh_remote_port;
	r->head.msgh_local_port = MACH_PORT_NULL;
	r->head.msgh_id = req->msgh_id + 100;
	r->version = WS_PROTOCOL_VERSION;
	kr = mach_msg(&r->head, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
	    r->head.msgh_size, 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS) {
		ws_debug("reply to 0x%x failed (0x%x)", req->msgh_id, kr);
		mach_port_deallocate(mach_task_self(), req->msgh_remote_port);
	}
	req->msgh_remote_port = MACH_PORT_NULL;
}

static void
reply_status(struct ws_mach_item *item, uint32_t seq, int status)
{
	ws_reply_t r;

	memset(&r, 0, sizeof(r));
	r.seq = seq;
	r.status = status;
	reply_send(item, &r, offsetof(ws_reply_t, u));
}

static uint64_t
random_token(void)
{
	uint64_t t;

	arc4random_buf(&t, sizeof(t));
	return t != 0 ? t : 1;
}

static void
handle_hello(struct ws_server *srv, struct ws_mach_item *item)
{
	ws_req_hello_t *m = (ws_req_hello_t *)(void *)item->msg.buf;
	struct ws_client *c, *app = NULL, *target;
	ws_reply_t r;

	if (item->size < WS_HELLO_MIN_SIZE) {
		reply_status(item, m->hdr.seq, EINVAL);
		return;
	}
	if (m->flags & WS_CONNECT_DOCK) {
		/* The Dock is a tool connection of its own kind; it sees
		 * every app's windows, so only the console user gets it
		 * (dock.c). */
		if (m->flags & (WS_CONNECT_CONTROL | WS_CONNECT_JOIN)) {
			reply_status(item, m->hdr.seq, EINVAL);
			return;
		}
		if (!dock_may_list(srv, item->sender_euid)) {
			ws_warn("pid %d (uid %d): Dock connection refused",
			    (int)item->sender_pid, (int)item->sender_euid);
			reply_status(item, m->hdr.seq, EPERM);
			return;
		}
	}
	if (m->flags & WS_CONNECT_JOIN) {
		/* The optional tail must be there, and a tool can't join. */
		if (item->size < sizeof(*m) || (m->flags & WS_CONNECT_CONTROL)) {
			reply_status(item, m->hdr.seq, EINVAL);
			return;
		}
		target = client_lookup(srv, m->join_client);
		if (target == NULL || target->doomed ||
		    (target->flags & WS_CONNECT_TOOL) ||
		    timingsafe_bcmp(&target->token, &m->join_token,
		    sizeof(target->token)) != 0) {
			ws_warn("pid %d: join of client %u refused",
			    (int)item->sender_pid, m->join_client);
			reply_status(item, m->hdr.seq, EPERM);
			return;
		}
		app = target->app;
	}
	c = calloc(1, sizeof(*c));
	if (c == NULL) {
		reply_status(item, m->hdr.seq, ENOMEM);
		return;
	}
	c->src = SRC_CLIENT;
	c->id = srv->next_client_id++;
	c->token = random_token();
	c->pid = item->sender_pid;
	c->uid = item->sender_euid;
	c->flags = m->flags & (WS_CONNECT_CONTROL | WS_CONNECT_XKB |
	    WS_CONNECT_DOCK);
	c->app = app != NULL ? app : c;
	c->sock = -1;
	c->hello_usec = ws_now_usec();
	c->outq_tail = &c->outq;
	c->cursor_kind = WS_CURSOR_ARROW;
	c->menu_bar = -1;
	m->app_name[sizeof(m->app_name) - 1] = '\0';
	strlcpy(c->app_name, m->app_name[0] ? m->app_name : "unnamed",
	    sizeof(c->app_name));
	c->next = srv->clients;
	srv->clients = c;

	memset(&r, 0, sizeof(r));
	r.seq = m->hdr.seq;
	r.status = 0;
	r.u.hello.client_id = c->id;
	r.u.hello.server_flags = (srv->headless ? WS_SERVER_HEADLESS : 0) |
	    WS_SERVER_FEATURES;
	r.u.hello.token = c->token;
	strlcpy(r.u.hello.socket_path, srv->socket_path,
	    sizeof(r.u.hello.socket_path));
	r.u.hello.app_id = c->app->id;
	reply_send(item, &r, sizeof(r));
	if (c->app != c)
		ws_info("hello from %s (pid %d) -> client %u, joining "
		    "app %u (%s)", c->app_name, (int)c->pid, c->id,
		    c->app->id, c->app->app_name);
	else
		ws_debug("hello from %s (pid %d) -> client %u", c->app_name,
		    (int)c->pid, c->id);
}

static bool
may_inject(struct ws_server *srv, struct ws_mach_item *item)
{
	return srv->headless || item->sender_euid == 0 ||
	    item->sender_euid == geteuid();
}

static void
handle_inject(struct ws_server *srv, struct ws_client *c,
    const ws_req_inject_t *m)
{
	int err;
	char name[sizeof(m->key_name)];

	switch (m->kind) {
	case WS_INJECT_MOTION:
		pointer_motion(srv, m->x, m->y);
		break;
	case WS_INJECT_BUTTON:
		pointer_button(srv, m->button, m->pressed != 0);
		break;
	case WS_INJECT_KEY:
	case WS_INJECT_KEY_TAP:
		memcpy(name, m->key_name, sizeof(name));
		name[sizeof(name) - 1] = '\0';
		if (name[0] == '\0') {
			if (m->kind == WS_INJECT_KEY_TAP) {
				keyboard_key(srv, m->keycode, true);
				keyboard_key(srv, m->keycode, false);
			} else
				keyboard_key(srv, m->keycode, m->pressed != 0);
			break;
		}
		err = keyboard_inject(srv, name,
		    m->kind == WS_INJECT_KEY_TAP ? 2 : (m->pressed ? 1 : 0));
		if (err != 0)
			client_error(srv, c, WS_MSG_INJECT, 0, err,
			    "unknown key name");
		break;
	case WS_INJECT_SCROLL:
		pointer_scroll(srv, m->dx, m->dy, 0, 0, WS_SCROLL_CONTINUOUS);
		break;
	case WS_INJECT_SCROLL_FINGER:
		scroll_finger(srv, m->dx, m->dy, m->dx == 0 && m->dy == 0,
		    ws_now_usec());
		break;
	case WS_INJECT_HOLD:
		if (m->pressed)
			scroll_touch(srv);
		break;
	case WS_INJECT_SWIPE:
		overview_swipe(srv, (int)m->pressed, m->button, m->dx, m->dy);
		break;
	case WS_INJECT_SCREEN_SIZE:
		if (m->x < 0 || m->y < 0 || m->x > 65535 || m->y > 65535)
			err = EINVAL;
		else
			err = screen_resize(srv, (uint32_t)m->x, (uint32_t)m->y);
		if (err != 0)
			client_error(srv, c, WS_MSG_INJECT, 0, err,
			    err == ENOTSUP ? "only a headless screen can "
			    "change size" : "bad screen size");
		break;
	default:
		client_error(srv, c, WS_MSG_INJECT, 0, EINVAL,
		    "unknown inject kind");
		break;
	}
}

static void
handle_snapshot(struct ws_server *srv, struct ws_client *c,
    struct ws_mach_item *item, uint32_t seq)
{
	ws_event_t ev;
	uint32_t stride;
	int fd;

	if (!may_inject(srv, item)) {
		reply_status(item, seq, EPERM);
		return;
	}
	fd = snapshot_memfd(srv, &stride);
	if (fd < 0) {
		reply_status(item, seq, errno);
		return;
	}
	reply_status(item, seq, 0);
	event_init(srv, &ev, WS_EVENT_SNAPSHOT, sizeof(ev.u.snapshot), 0);
	ev.u.snapshot.width = srv->width;
	ev.u.snapshot.height = srv->height;
	ev.u.snapshot.stride = stride;
	ev.u.snapshot.format = WS_FORMAT_XRGB8888;
	ev.u.snapshot.fd = -1;
	client_send(srv, c, &ev, fd);
	close(fd);
}

/* WS_MSG_DOCK_PREVIEW: answer, then send the picture on the socket. */
static void
handle_preview(struct ws_server *srv, struct ws_client *c,
    struct ws_mach_item *item, uint32_t seq, uint32_t window, int32_t max)
{
	struct ws_window *w = window_lookup(srv, window);
	uint32_t width, height, stride;
	ws_event_t ev;
	int fd;

	if (w == NULL) {
		reply_status(item, seq, ENOENT);
		return;
	}
	fd = dock_preview_memfd(srv, w, max, &width, &height, &stride);
	if (fd < 0) {
		reply_status(item, seq, errno);
		return;
	}
	reply_status(item, seq, 0);
	event_init(srv, &ev, WS_EVENT_PREVIEW, sizeof(ev.u.preview), w->id);
	ev.u.preview.width = width;
	ev.u.preview.height = height;
	ev.u.preview.stride = stride;
	ev.u.preview.format = WS_FORMAT_ARGB8888;
	ev.u.preview.fd = -1;
	client_send(srv, c, &ev, fd);
	close(fd);
}

/* WS_MSG_LIST_WINDOWS: tools only, and only for the console user. */
static void
handle_list(struct ws_server *srv, struct ws_client *c,
    struct ws_mach_item *item, uint32_t options)
{
	ws_event_t ev;

	if ((c->flags & WS_CONNECT_TOOL) == 0 ||
	    !dock_may_list(srv, item->sender_euid)) {
		client_error(srv, c, WS_MSG_LIST_WINDOWS, 0, EPERM,
		    "window list not allowed");
		event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
		client_send(srv, c, &ev, -1);
		return;
	}
	list_windows(srv, c, options);
}

static bool
is_dock_request(mach_msg_id_t id)
{
	return id == WS_MSG_DOCK_ACTIVATE || id == WS_MSG_DOCK_RESTORE ||
	    id == WS_MSG_DOCK_PREVIEW || id == WS_MSG_DOCK_SHOW_MINIMIZED ||
	    id == WS_MSG_DOCK_SET_EDGE;
}

static size_t
min_size(mach_msg_id_t id)
{
	switch (id) {
	case WS_MSG_WINDOW_CREATE:
		return WS_WINDOW_CREATE_MIN_SIZE;
	case WS_MSG_WINDOW_SET_FRAME:
		return sizeof(ws_req_frame_t);
	case WS_MSG_WINDOW_SET_LEVEL:
	case WS_MSG_WINDOW_SET_FLAGS:
	case WS_MSG_WINDOW_ATTACH:
	case WS_MSG_WINDOW_COMMIT:
	case WS_MSG_BUFFER_DESTROY:
	case WS_MSG_CURSOR_VISIBLE:
	case WS_MSG_ACTIVATE:
	case WS_MSG_SNAPSHOT:
	case WS_MSG_SET_APP_ICON:
	case WS_MSG_DOCK_ACTIVATE:
	case WS_MSG_DOCK_PREVIEW:
	case WS_MSG_DOCK_SHOW_MINIMIZED:
	case WS_MSG_OVERVIEW:
	case WS_MSG_SET_KEYBOARD_LAYOUT:
	case WS_MSG_POINTER_GRAB:
	case WS_MSG_POINTER_TRACK:
	case WS_MSG_RESERVE_WINDOW_IDS:
		return sizeof(ws_req_int_t);
	case WS_MSG_WARP_POINTER:
		return sizeof(ws_req_point_t);
	case WS_MSG_WINDOW_SET_CURSOR:
		return sizeof(ws_req_cursor_t);
	case WS_MSG_WINDOW_SET_INPUT_REGION:
		return offsetof(ws_req_region_t, rects);
	case WS_MSG_SET_SETTINGS:
		return offsetof(ws_req_settings_t, data);
	case WS_MSG_WINDOW_ORDER:
		return sizeof(ws_req_order_t);
	case WS_MSG_WINDOW_SET_TITLE:
		return sizeof(ws_req_title_t);
	case WS_MSG_WINDOW_DAMAGE:
		return offsetof(ws_req_damage_t, rects);
	case WS_MSG_SET_CURSOR:
		return sizeof(ws_req_cursor_t);
	case WS_MSG_INJECT:
		return sizeof(ws_req_inject_t);
	case WS_MSG_DOCK_SET_EDGE:
		return sizeof(ws_req_dock_edge_t);
	default:
		return sizeof(ws_req_header_t);
	}
}

/* Requests that need a window owned by the sender. */
static bool
needs_window(mach_msg_id_t id)
{
	switch (id) {
	case WS_MSG_WINDOW_DESTROY:
	case WS_MSG_WINDOW_SET_FRAME:
	case WS_MSG_WINDOW_SET_LEVEL:
	case WS_MSG_WINDOW_ORDER:
	case WS_MSG_WINDOW_SET_TITLE:
	case WS_MSG_WINDOW_SET_FLAGS:
	case WS_MSG_WINDOW_ATTACH:
	case WS_MSG_WINDOW_DAMAGE:
	case WS_MSG_WINDOW_COMMIT:
	case WS_MSG_WINDOW_MAKE_KEY:
	case WS_MSG_WINDOW_MINIMIZE:
	case WS_MSG_WINDOW_SET_CURSOR:
	case WS_MSG_WINDOW_SET_INPUT_REGION:
		return true;
	default:
		return false;
	}
}

void
request_handle(struct ws_server *srv, struct ws_mach_item *item)
{
	mach_msg_header_t *head = &item->msg.head;
	ws_req_header_t *h = (ws_req_header_t *)(void *)item->msg.buf;
	mach_msg_id_t id = head->msgh_id;
	bool want_reply = head->msgh_remote_port != MACH_PORT_NULL;
	struct ws_client *c;
	struct ws_window *w = NULL;
	struct ws_buffer *b;
	int32_t value;

	if (item->size < sizeof(*h) || h->version != WS_PROTOCOL_VERSION ||
	    id <= WS_MSG_BASE || id >= WS_MSG_LAST) {
		ws_debug("bad request 0x%x (size %zu)", id, item->size);
		if (want_reply)
			reply_status(item, item->size >= sizeof(*h) ? h->seq : 0,
			    EPROTO);
		return;
	}
	if (id == WS_MSG_HELLO) {
		handle_hello(srv, item);
		return;
	}
	c = client_lookup(srv, h->client_id);
	if (c == NULL || c->token != h->token ||
	    (item->sender_pid > 0 && c->pid > 0 && item->sender_pid != c->pid)) {
		if (want_reply)
			reply_status(item, h->seq, EPERM);
		return;
	}
	/* Socket messages sent before this request come first. */
	if (c->sock >= 0)
		client_readable(srv, c);
	if (c->dead || c->doomed) {
		if (want_reply)
			reply_status(item, h->seq, EPIPE);
		return;
	}
	if (item->size < min_size(id)) {
		client_error(srv, c, (uint32_t)id, h->window, EINVAL,
		    "short request");
		if (want_reply)
			reply_status(item, h->seq, EINVAL);
		return;
	}
	if (is_dock_request(id) && (c->flags & WS_CONNECT_DOCK) == 0) {
		client_error(srv, c, (uint32_t)id, h->window, EPERM,
		    "not a Dock connection");
		if (want_reply)
			reply_status(item, h->seq, EPERM);
		return;
	}
	if (needs_window(id)) {
		w = window_lookup(srv, h->window);
		if (w == NULL || w->owner != c) {
			client_error(srv, c, (uint32_t)id, h->window, ENOENT,
			    "no such window");
			if (want_reply)
				reply_status(item, h->seq, ENOENT);
			return;
		}
	}
	/* Requests shorter than ws_req_int_t (an old LIST_WINDOWS) have
	 * no value. */
	value = item->size >= sizeof(ws_req_int_t) ?
	    ((ws_req_int_t *)(void *)h)->value : 0;

	switch (id) {
	case WS_MSG_GOODBYE:
		ws_info("client %u (%s) said goodbye", c->id, c->app_name);
		client_destroy(srv, c);
		break;
	case WS_MSG_SYNC:
		break;
	case WS_MSG_WINDOW_CREATE: {
		ws_req_window_create_t *m = (void *)h;
		uint32_t wid = item->size >= sizeof(*m) ? m->window_id : 0;
		ws_reply_t r;
		int err = 0;

		/* One-way creates name their id (WS_SERVER_WINDOW_IDS) and
		 * hear about failures as WS_EVENT_ERROR. */
		if (c->flags & WS_CONNECT_TOOL)
			err = EPERM;
		else if (wid != 0)
			err = window_create_check_id(srv, c, wid);
		else if (!want_reply)
			err = EINVAL;
		if (err == 0) {
			m->title[sizeof(m->title) - 1] = '\0';
			w = window_create(srv, c, &m->frame, m->level, m->flags,
			    m->title, wid);
			if (w == NULL)
				err = ENOMEM;
		}
		if (err != 0) {
			if (want_reply)
				reply_status(item, h->seq, err);
			else
				client_error(srv, c, (uint32_t)id, wid, err,
				    err == EINVAL ? "bad window id" :
				    "window not created");
			return;
		}
		if (!want_reply)
			return;
		memset(&r, 0, sizeof(r));
		r.seq = h->seq;
		r.u.create.window = w->id;
		reply_send(item, &r, sizeof(r));
		return;
	}
	case WS_MSG_WINDOW_DESTROY:
		window_destroy(srv, w);
		break;
	case WS_MSG_WINDOW_SET_FRAME:
		window_set_frame(srv, w, &((ws_req_frame_t *)(void *)h)->frame);
		break;
	case WS_MSG_WINDOW_SET_LEVEL:
		window_set_level(srv, w, value);
		break;
	case WS_MSG_WINDOW_ORDER: {
		ws_req_order_t *m = (void *)h;
		int op = m->op > 0 ? WS_ORDER_ABOVE :
		    m->op < 0 ? WS_ORDER_BELOW : WS_ORDER_OUT;

		window_order(srv, w, op, window_lookup(srv, m->relative_to));
		break;
	}
	case WS_MSG_WINDOW_SET_TITLE: {
		ws_req_title_t *m = (void *)h;

		m->title[sizeof(m->title) - 1] = '\0';
		if (strcmp(w->title, m->title) != 0) {
			strlcpy(w->title, m->title, sizeof(w->title));
			dock_list_changed(srv);
		}
		break;
	}
	case WS_MSG_WINDOW_SET_FLAGS:
		if (w->flags != (uint32_t)value)
			dock_list_changed(srv);
		w->flags = (uint32_t)value;
		if (w->visible)
			damage_rect(srv, &w->frame);
		pointer_recheck(srv);
		break;
	case WS_MSG_WINDOW_ATTACH:
		b = NULL;
		if (value != 0 && (b = client_buffer(c, (uint32_t)value)) == NULL) {
			client_error(srv, c, (uint32_t)id, w->id, ENOENT,
			    "no such buffer");
			break;
		}
		window_attach(srv, w, b);
		break;
	case WS_MSG_WINDOW_DAMAGE: {
		ws_req_damage_t *m = (void *)h;
		unsigned n = m->count;

		if (n > WS_MAX_DAMAGE_RECTS ||
		    item->size < offsetof(ws_req_damage_t, rects) +
		    n * sizeof(ws_rect_t)) {
			client_error(srv, c, (uint32_t)id, w->id, EINVAL,
			    "bad damage count");
			break;
		}
		window_damage(srv, w, m->rects, n);
		break;
	}
	case WS_MSG_WINDOW_COMMIT: {
		ws_req_commit_t *m = (void *)h;

		if ((value & WS_COMMIT_SET_FRAME) && item->size < sizeof(*m)) {
			client_error(srv, c, (uint32_t)id, w->id, EINVAL,
			    "commit without its frame");
			value &= ~WS_COMMIT_SET_FRAME;
		}
		window_commit(srv, w, (uint32_t)value,
		    (value & WS_COMMIT_SET_FRAME) ? &m->frame : NULL);
		break;
	}
	case WS_MSG_WINDOW_MAKE_KEY:
		window_make_key(srv, w);
		break;
	case WS_MSG_WINDOW_MINIMIZE:
		window_minimize(srv, w, value);
		break;
	case WS_MSG_SET_APP_ICON: {
		int err;

		b = NULL;
		if (value != 0 && (b = client_buffer(c, (uint32_t)value)) == NULL) {
			client_error(srv, c, (uint32_t)id, 0, ENOENT,
			    "no such icon buffer");
			break;
		}
		err = tile_set_app_icon(srv, c, b);
		if (err != 0)
			client_error(srv, c, (uint32_t)id, 0, err,
			    "bad app icon");
		break;
	}
	case WS_MSG_BUFFER_DESTROY: {
		struct ws_buffer **pp;

		for (pp = &c->buffers; *pp != NULL; pp = &(*pp)->next)
			if ((*pp)->id == (uint32_t)value)
				break;
		if ((b = *pp) == NULL) {
			client_error(srv, c, (uint32_t)id, 0, ENOENT,
			    "no such buffer");
			break;
		}
		*pp = b->next;
		b->client_ref = false;
		buffer_unref(srv, b);
		break;
	}
	case WS_MSG_SET_CURSOR: {
		ws_req_cursor_t *m = (void *)h;
		struct ws_buffer *old = c->cursor_buffer;

		b = NULL;
		if (m->kind == WS_CURSOR_CUSTOM) {
			b = client_buffer(c, m->buffer_id);
			if (b == NULL) {
				client_error(srv, c, (uint32_t)id, 0, ENOENT,
				    "no such cursor buffer");
				break;
			}
			b->refs++;
		}
		c->cursor_kind = m->kind;
		c->cursor_buffer = b;
		c->cursor_hot_x = m->hot_x;
		c->cursor_hot_y = m->hot_y;
		if (old != NULL) {
			cursor_buffer_gone(srv, old);
			buffer_unref(srv, old);
		}
		cursor_update(srv);
		break;
	}
	case WS_MSG_CURSOR_VISIBLE:
		c->cursor_hidden = value == 0;
		cursor_update(srv);
		break;
	case WS_MSG_WINDOW_SET_CURSOR: {
		ws_req_cursor_t *m = (void *)h;
		int err;

		b = NULL;
		if (m->kind == WS_CURSOR_CUSTOM &&
		    (b = client_buffer(c, m->buffer_id)) == NULL) {
			client_error(srv, c, (uint32_t)id, w->id, ENOENT,
			    "no such cursor buffer");
			break;
		}
		err = cursor_window_set(srv, w, m->kind, b, m->hot_x, m->hot_y);
		if (err != 0)
			client_error(srv, c, (uint32_t)id, w->id, err,
			    "bad cursor");
		break;
	}
	case WS_MSG_WINDOW_SET_INPUT_REGION: {
		ws_req_region_t *m = (void *)h;
		unsigned n = (m->flags & WS_REGION_RESET) ? 0 : m->count;

		if (n > WS_MAX_REGION_RECTS ||
		    item->size < offsetof(ws_req_region_t, rects) +
		    n * sizeof(ws_rect_t)) {
			client_error(srv, c, (uint32_t)id, w->id, EINVAL,
			    "bad input region count");
			break;
		}
		window_set_input_region(srv, w, m->rects, n, m->flags);
		break;
	}
	case WS_MSG_POINTER_GRAB: {
		int err = pointer_grab(srv, c, (uint32_t)value);

		if (want_reply)
			reply_status(item, h->seq, err);
		else if (err != 0)
			client_error(srv, c, (uint32_t)id, 0, err,
			    err == EINVAL ? "unknown grab mode" :
			    err == EBUSY ? "another connection holds the grab" :
			    "only the active app may grab");
		return;
	}
	case WS_MSG_POINTER_UNGRAB:
		pointer_ungrab(srv, c, false);
		break;
	case WS_MSG_WARP_POINTER: {
		ws_req_point_t *m = (void *)h;
		int err;

		w = NULL;
		if (h->window != 0) {
			w = window_lookup(srv, h->window);
			if (w == NULL || w->owner != c) {
				client_error(srv, c, (uint32_t)id, h->window,
				    ENOENT, "no such window");
				break;
			}
		}
		err = pointer_warp(srv, c, w, m->x, m->y);
		if (err != 0)
			client_error(srv, c, (uint32_t)id, h->window, err,
			    "warp not allowed");
		break;
	}
	case WS_MSG_QUERY_POINTER: {
		ws_reply_t r;
		int err;

		memset(&r, 0, sizeof(r));
		err = pointer_query(srv, c, &r.u.pointer);
		if (!want_reply)
			return;
		if (err != 0) {
			reply_status(item, h->seq, err);
			return;
		}
		r.seq = h->seq;
		reply_send(item, &r, sizeof(r));
		return;
	}
	case WS_MSG_POINTER_TRACK:
		c->pointer_track = value != 0;
		break;
	case WS_MSG_RESERVE_WINDOW_IDS: {
		ws_reply_t r;
		uint32_t base = 0;
		int err;

		err = (c->flags & WS_CONNECT_TOOL) ? EPERM :
		    window_reserve_ids(srv, c, (uint32_t)value, &base);
		if (!want_reply) {
			if (err != 0)
				client_error(srv, c, (uint32_t)id, 0, err,
				    "no window ids reserved");
			return;
		}
		if (err != 0) {
			reply_status(item, h->seq, err);
			return;
		}
		memset(&r, 0, sizeof(r));
		r.seq = h->seq;
		r.u.window_ids.base = base;
		r.u.window_ids.count = (uint32_t)value;
		reply_send(item, &r, sizeof(r));
		return;
	}
	case WS_MSG_ACTIVATE:
		/* TODO: focus-stealing policy (only on user action). */
		app_activate(srv, c->app, NULL);
		break;
	case WS_MSG_DEACTIVATE:
		app_deactivate(srv, c->app);
		break;
	case WS_MSG_SET_MENU_BAR: {
		int err;

		if (c->flags & WS_CONNECT_TOOL) {
			client_error(srv, c, (uint32_t)id, 0, EPERM,
			    "only apps have a menu bar");
			break;
		}
		err = menu_bar_set(srv, c->app, (uint32_t)value);
		if (err != 0)
			client_error(srv, c, (uint32_t)id, 0, err,
			    "bad menu bar height");
		break;
	}
	case WS_MSG_SNAPSHOT:
		handle_snapshot(srv, c, item, h->seq);
		return;
	case WS_MSG_LIST_WINDOWS:
		handle_list(srv, c, item, (uint32_t)value);
		break;
	case WS_MSG_DOCK_ACTIVATE:
		dock_activate(srv, c, h->window, (uint32_t)value);
		break;
	case WS_MSG_DOCK_RESTORE:
		dock_restore(srv, c, h->window);
		break;
	case WS_MSG_DOCK_SHOW_MINIMIZED:
		dock_show_minimized(srv, c, value != 0);
		break;
	case WS_MSG_DOCK_SET_EDGE: {
		ws_req_dock_edge_t *m = (void *)h;

		if (dock_set_edge(srv, c, m->edge, m->thickness) != 0)
			client_error(srv, c, (uint32_t)id, 0, EINVAL,
			    "bad Dock edge or thickness");
		break;
	}
	case WS_MSG_DOCK_PREVIEW:
		handle_preview(srv, c, item, h->seq, h->window, value);
		return;
	case WS_MSG_OVERVIEW:
		/* tools and the Dock, for the console user */
		if ((c->flags & WS_CONNECT_TOOL) == 0 ||
		    !dock_may_list(srv, item->sender_euid)) {
			ws_event_t ev;

			client_error(srv, c, (uint32_t)id, 0, EPERM,
			    "overview not allowed");
			event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
			client_send(srv, c, &ev, -1);
			break;
		}
		switch (value) {
		case WS_OVERVIEW_CLOSE:
			overview_close(srv);
			break;
		case WS_OVERVIEW_ALL:
		case WS_OVERVIEW_APP:
			overview_open(srv, value);
			break;
		case WS_OVERVIEW_QUERY:
			break;
		default:
			client_error(srv, c, (uint32_t)id, 0, EINVAL,
			    "unknown overview request");
			break;
		}
		overview_report(srv, c);
		break;
	case WS_MSG_SET_SETTINGS:
		settings_set(srv, c, item);
		return;
	case WS_MSG_GET_SETTINGS:
		settings_get(srv, c);
		break;
	case WS_MSG_SET_KEYBOARD_LAYOUT: {
		int err;

		/* the console user's, as for the settings */
		if (!dock_may_list(srv, item->sender_euid))
			err = EPERM;
		else
			err = keyboard_set_layout(srv, (uint32_t)value);
		if (want_reply)
			reply_status(item, h->seq, err);
		else if (err != 0)
			client_error(srv, c, (uint32_t)id, 0, err,
			    err == EPERM ? "layout change not allowed" :
			    "no such keyboard layout");
		return;
	}
	case WS_MSG_LIST_KEYBOARD_LAYOUTS:
		keyboard_list_layouts(srv, c);
		break;
	case WS_MSG_LIST_INPUT_DEVICES:
		input_list(srv, c);
		break;
	case WS_MSG_INJECT:
		if (!may_inject(srv, item)) {
			client_error(srv, c, (uint32_t)id, 0, EPERM,
			    "synthetic input not allowed");
			break;
		}
		handle_inject(srv, c, (ws_req_inject_t *)(void *)h);
		break;
	default:
		break;
	}
	if (want_reply)
		reply_status(item, h->seq, 0);
}
