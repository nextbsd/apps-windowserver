/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The Dock's window list (WS_CONNECT_DOCK, "The Dock" in WSProtocol.h):
 * who may have it, change notices, activating and restoring other apps'
 * windows, previews of minimized windows, and handing windows minimized
 * into the Dock over to it instead of drawing tiles (WS_MINIMIZE_INTO_DOCK
 * or MinimizeIntoDock).
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

/* Where loginwindow keeps a record of each graphical session,
 * <uid>.session (apps-loginwindow, LWSession.m). */
#define LW_RUN_DIR	"/var/run/loginwindow"

#define PREVIEW_MIN	16

/* ---- trust ---- */

/*
 * Does `uid` have a loginwindow session? *any tells whether any user
 * has one. Only root-owned records in a root-owned directory nobody
 * else can write count; if the directory can't be trusted, nobody is
 * the console user, but *any is set so that nobody gets in through the
 * no-session rule either.
 */
static bool
console_user(uid_t uid, bool *any)
{
	struct dirent *de;
	struct stat st;
	unsigned long v;
	bool found = false;
	size_t n;
	char *end;
	DIR *d;

	*any = false;
	d = opendir(LW_RUN_DIR);
	if (d == NULL)
		return false;
	if (fstat(dirfd(d), &st) < 0 || st.st_uid != 0 ||
	    (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
		ws_warn("%s isn't root's alone; no console user", LW_RUN_DIR);
		*any = true;
		closedir(d);
		return false;
	}
	while ((de = readdir(d)) != NULL) {
		n = strlen(de->d_name);
		if (n <= 8 || strcmp(de->d_name + n - 8, ".session") != 0)
			continue;
		errno = 0;
		v = strtoul(de->d_name, &end, 10);
		if (errno != 0 || end != de->d_name + n - 8)
			continue;
		if (fstatat(dirfd(d), de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
		    !S_ISREG(st.st_mode) || st.st_uid != 0)
			continue;
		*any = true;
		if ((uid_t)v == uid)
			found = true;
	}
	closedir(d);
	return found;
}

/*
 * May a process running as `uid` see every app's windows (the Dock role
 * and the window list)? Root, the server's own user and the console
 * user may. Without a loginwindow session (a server started by hand)
 * and on a headless server, anyone may, as for the rest of the protocol.
 */
bool
dock_may_list(struct ws_server *srv, uid_t uid)
{
	bool any;

	if (srv->headless || uid == 0 || uid == geteuid())
		return true;
	if (uid == (uid_t)-1)
		return false;	/* no audit trailer */
	if (console_user(uid, &any))
		return true;
	return !any;
}

/* ---- the Dock that shows minimized windows ---- */

static bool
dock_usable(const struct ws_client *c)
{
	return c != NULL && !c->dead && !c->doomed && c->sock >= 0 &&
	    (c->flags & WS_CONNECT_DOCK) != 0;
}

/* Minimized window w goes to the Dock instead of getting a tile: it was
 * minimized into the Dock, and a Dock that shows minimized windows is
 * connected. */
bool
dock_shows_minimized(struct ws_server *srv, const struct ws_window *w)
{
	return w->into_dock && dock_usable(srv->dock);
}

static void
send_state(struct ws_server *srv, struct ws_client *c)
{
	ws_event_t ev;

	event_init(srv, &ev, WS_EVENT_DOCK_STATE, sizeof(ev.u.dock_state), 0);
	ev.u.dock_state.flags = (c == srv->dock && dock_usable(c)) ?
	    WS_DOCK_MINIMIZED_WINDOWS : 0;
	ev.u.dock_state.edge = srv->dock_edge;
	ev.u.dock_state.thickness = srv->dock_thickness;
	ev.u.dock_state.edge_client = srv->dock_edge_client;
	client_send(srv, c, &ev, -1);
}

/* ---- the Dock's edge ---- */

static const char *
edge_name(uint32_t edge)
{
	static const char *names[] = { "none", "left", "right", "top",
	    "bottom" };

	return edge <= WS_DOCK_EDGE_BOTTOM ? names[edge] : "?";
}

/*
 * The Dock's edge is the newest report of a Dock connection still
 * connected (other than `gone`). If it changed, every Dock connection
 * hears it.
 */
static void
edge_update(struct ws_server *srv, struct ws_client *gone)
{
	struct ws_client *c, *best = NULL;

	for (c = srv->clients; c != NULL; c = c->next)
		if (c != gone && dock_usable(c) &&
		    c->dock_edge != WS_DOCK_EDGE_NONE &&
		    (best == NULL || c->dock_edge_serial > best->dock_edge_serial))
			best = c;
	if (best == NULL ? srv->dock_edge_client == 0 :
	    (srv->dock_edge_client == best->id &&
	    srv->dock_edge_serial == best->dock_edge_serial))
		return;
	srv->dock_edge = best != NULL ? best->dock_edge : WS_DOCK_EDGE_NONE;
	srv->dock_thickness = best != NULL ? best->dock_thickness : 0;
	srv->dock_edge_client = best != NULL ? best->id : 0;
	srv->dock_edge_serial = best != NULL ? best->dock_edge_serial : 0;
	if (best != NULL)
		ws_info("the Dock (client %u) keeps the %s edge, %u pixels",
		    best->id, edge_name(srv->dock_edge), srv->dock_thickness);
	else
		ws_info("no Dock keeps a screen edge");
	/* Tiling (apps-windowserver#20) will lay out again here. */
	for (c = srv->clients; c != NULL; c = c->next)
		if (c != gone && dock_usable(c))
			send_state(srv, c);
}

/* WS_MSG_DOCK_SET_EDGE: Dock c is on `edge`, `thickness` pixels thick.
 * Returns 0 or an errno. */
int
dock_set_edge(struct ws_server *srv, struct ws_client *c, uint32_t edge,
    uint32_t thickness)
{
	static uint64_t serial;
	uint32_t limit;

	if (edge > WS_DOCK_EDGE_BOTTOM)
		return EINVAL;
	limit = (edge == WS_DOCK_EDGE_LEFT || edge == WS_DOCK_EDGE_RIGHT) ?
	    srv->width : srv->height;
	if (edge == WS_DOCK_EDGE_NONE)
		thickness = 0;
	else if (thickness > limit / 2)
		return EINVAL;
	if (c->dock_edge == edge && c->dock_thickness == thickness)
		return 0;
	c->dock_edge = edge;
	c->dock_thickness = thickness;
	c->dock_edge_serial = ++serial;
	edge_update(srv, NULL);
	return 0;
}

/* Tiles follow who shows minimized windows: none for the windows the
 * Dock shows, one for every other minimized window. */
static void
dock_tiles_follow(struct ws_server *srv)
{
	struct ws_window *w;

	for (w = srv->windows; w != NULL; w = w->next) {
		if (!w->minimized)
			continue;
		if (dock_shows_minimized(srv, w))
			window_tile_remove(srv, w);
		else
			window_tile_add(srv, w);
	}
}

/* A Dock connection got its welcome: tell it the state. */
void
dock_client_ready(struct ws_server *srv, struct ws_client *c)
{
	if (dock_usable(c))
		send_state(srv, c);
}

/* srv->dock gives up showing minimized windows (it went away, or said
 * so): the oldest other Dock that can show them takes over, or the
 * server draws tiles again. */
static void
dock_hand_on(struct ws_server *srv, struct ws_client *c)
{
	struct ws_client *o, *next = NULL;

	/* srv->clients is newest first: the last match is the oldest. */
	for (o = srv->clients; o != NULL; o = o->next)
		if (o != c && dock_usable(o) && o->dock_wants_minimized)
			next = o;
	srv->dock = next;
	dock_tiles_follow(srv);
	if (next != NULL) {
		ws_info("client %u (pid %d) shows minimized windows now",
		    next->id, (int)next->pid);
		send_state(srv, next);
	} else
		ws_info("no Dock shows minimized windows; they get tiles");
}

/* WS_MSG_DOCK_SHOW_MINIMIZED: Dock c can (or no longer) show minimized
 * windows. The first one that can, does. */
void
dock_show_minimized(struct ws_server *srv, struct ws_client *c, bool on)
{
	if (c->dock_wants_minimized == on)
		return;
	c->dock_wants_minimized = on;
	if (on && !dock_usable(srv->dock)) {
		srv->dock = c;
		dock_tiles_follow(srv);
		ws_info("client %u (%s, pid %d) is the Dock; windows "
		    "minimized into the Dock go into it", c->id, c->app_name,
		    (int)c->pid);
		send_state(srv, c);
	} else if (!on && srv->dock == c) {
		dock_hand_on(srv, c);
		send_state(srv, c);
	}
}

/* Dock connection c is going away. */
void
dock_client_gone(struct ws_server *srv, struct ws_client *c)
{
	dock_list_changed(srv);
	if (srv->dock == c)
		dock_hand_on(srv, c);
	if (srv->dock_edge_client == c->id)
		edge_update(srv, c);
}

/* ---- notices ---- */

void
dock_list_changed(struct ws_server *srv)
{
	srv->dock_list_dirty = true;
	srv->ov.dirty = true;	/* the overview lays out again */
}

void
dock_preview_changed(struct ws_server *srv, struct ws_window *w)
{
	w->preview_dirty = true;
	srv->dock_previews_dirty = true;
}

static void
send_docks(struct ws_server *srv, uint16_t type, uint32_t window)
{
	struct ws_client *c;
	ws_event_t ev;

	for (c = srv->clients; c != NULL; c = c->next) {
		if (!dock_usable(c))
			continue;
		event_init(srv, &ev, type, 0, window);
		client_send(srv, c, &ev, -1);
	}
}

/* Send what changed in this pass of the event loop. */
void
dock_flush(struct ws_server *srv)
{
	struct ws_window *w;

	if (srv->dock_list_dirty) {
		srv->dock_list_dirty = false;
		send_docks(srv, WS_EVENT_WINDOW_LIST_CHANGED, 0);
	}
	if (srv->dock_previews_dirty) {
		srv->dock_previews_dirty = false;
		for (w = srv->windows; w != NULL; w = w->next) {
			if (!w->preview_dirty)
				continue;
			w->preview_dirty = false;
			if (w->minimized && w->tile == NULL)
				send_docks(srv, WS_EVENT_PREVIEW_CHANGED, w->id);
		}
	}
}

/* ---- requests ---- */

/*
 * Bring app a's windows on normal and higher levels to the front of
 * their levels, keeping their order, as activating an app from the
 * Dock does on macOS. Its owners hear about it (WS_EVENT_WINDOW_ORDERED).
 */
static void
raise_app(struct ws_server *srv, struct ws_client *a)
{
	struct ws_window *w, **list;
	size_t n = 0, i;

	for (w = srv->stack_bottom; w != NULL; w = w->above)
		n++;
	if (n == 0 || (list = calloc(n, sizeof(*list))) == NULL)
		return;
	n = 0;
	for (w = srv->stack_bottom; w != NULL; w = w->above)
		if (w->tile_for == NULL && w->owner->app == a &&
		    w->level >= WS_LEVEL_NORMAL &&
		    (w->flags & WS_WINDOW_ICON) == 0)
			list[n++] = w;
	for (i = 0; i < n; i++)
		window_order_by_server(srv, list[i], WS_ORDER_ABOVE, NULL);
	free(list);
}

/*
 * WS_MSG_DOCK_ACTIVATE: activate the app of `window` with that window
 * key and in front (or ask its owner to restore it, if it's minimized),
 * or with window 0 the app `app` names.
 */
void
dock_activate(struct ws_server *srv, struct ws_client *c, uint32_t window,
    uint32_t app)
{
	struct ws_window *w = NULL;
	struct ws_client *a;

	if (window != 0) {
		w = window_lookup(srv, window);
		if (w == NULL) {
			client_error(srv, c, WS_MSG_DOCK_ACTIVATE, window,
			    ENOENT, "no such window");
			return;
		}
		if (w->minimized) {
			window_request_restore(srv, w);
			return;
		}
		a = w->owner->app;
	} else {
		a = client_lookup(srv, app);
		if (a == NULL || (a->flags & WS_CONNECT_TOOL)) {
			client_error(srv, c, WS_MSG_DOCK_ACTIVATE, 0, ENOENT,
			    "no such app");
			return;
		}
		a = a->app;
	}
	ws_debug("the Dock activates app %u (%s), window %u", a->id,
	    a->app_name, window);
	if (w != NULL && w->visible) {
		app_bring_forward(srv, w);
		return;
	}
	raise_app(srv, a);
	app_activate(srv, a, NULL);
}

/*
 * Bring visible window w and the rest of its app's windows forward, w
 * in front, and activate the app with w key (the Dock, the overview).
 */
void
app_bring_forward(struct ws_server *srv, struct ws_window *w)
{
	struct ws_client *a = w->owner->app;

	raise_app(srv, a);
	window_order_by_server(srv, w, WS_ORDER_ABOVE, NULL);
	app_activate(srv, a, (w->flags & WS_WINDOW_NO_KEY) ? NULL : w);
}

/* WS_MSG_DOCK_RESTORE: ask the owner of a minimized window to restore it. */
void
dock_restore(struct ws_server *srv, struct ws_client *c, uint32_t window)
{
	struct ws_window *w = window_lookup(srv, window);

	if (w == NULL || !w->minimized) {
		client_error(srv, c, WS_MSG_DOCK_RESTORE, window,
		    w == NULL ? ENOENT : EINVAL,
		    w == NULL ? "no such window" : "window isn't minimized");
		return;
	}
	ws_debug("the Dock restores window %u", w->id);
	window_request_restore(srv, w);
}

/*
 * A memfd with w's picture: as much of its buffer as its frame shows,
 * scaled down to at most `max` pixels on the longer side and never
 * enlarged, premultiplied ARGB8888, sealed against changes.
 */
int
dock_preview_memfd(struct ws_server *srv, struct ws_window *w, int max,
    uint32_t *width, uint32_t *height, uint32_t *stride)
{
	struct ws_buffer *b = w->buffer;
	pixman_image_t *src, *dst;
	int cw, ch, dw, dh, fd, saved;
	size_t size;
	double f;
	void *p;

	(void)srv;
	if (b == NULL || w->frame.width <= 0 || w->frame.height <= 0) {
		errno = ENOENT;
		return -1;
	}
	if (max <= 0 || max > WS_MAX_PREVIEW)
		max = WS_MAX_PREVIEW;
	if (max < PREVIEW_MIN)
		max = PREVIEW_MIN;
	cw = (int)b->width;
	ch = (int)b->height;
	if (cw > w->frame.width * (int)b->scale)
		cw = w->frame.width * (int)b->scale;
	if (ch > w->frame.height * (int)b->scale)
		ch = w->frame.height * (int)b->scale;
	f = fmin((double)max / cw, (double)max / ch);
	if (f > 1.0)
		f = 1.0;
	dw = (int)lround(cw * f);
	dh = (int)lround(ch * f);
	if (dw < 1)
		dw = 1;
	if (dh < 1)
		dh = 1;
	*width = (uint32_t)dw;
	*height = (uint32_t)dh;
	*stride = (uint32_t)dw * 4;
	size = (size_t)*stride * (size_t)dh;

	fd = memfd_create("WindowServer-preview", MFD_CLOEXEC |
	    MFD_ALLOW_SEALING);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) < 0)
		goto fail;
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED)
		goto fail;
	dst = pixman_image_create_bits(PIXMAN_a8r8g8b8, dw, dh, p,
	    (int)*stride);
	src = pixman_image_create_bits(b->format == WS_FORMAT_XRGB8888 ?
	    PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8, (int)b->width, (int)b->height,
	    pixman_image_get_data(b->image), (int)b->stride);
	if (dst != NULL && src != NULL)
		tile_scale(src, dst, 0, 0, dw, dh, cw, ch);
	if (src != NULL)
		pixman_image_unref(src);
	if (dst != NULL)
		pixman_image_unref(dst);
	munmap(p, size);
	if (dst == NULL || src == NULL) {
		errno = ENOMEM;
		goto fail;
	}
	(void)fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW |
	    F_SEAL_WRITE | F_SEAL_SEAL);
	return fd;
fail:
	saved = errno;
	close(fd);
	errno = saved;
	return -1;
}
