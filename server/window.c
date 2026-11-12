/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The window model: the visible stack (strictly by level, then by
 * order within a level), key windows and the single active app, and
 * the tiles that stand in for minimized windows (drawn by tile.c).
 * Every change the Dock's window list shows calls dock_list_changed().
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

/* ---- damage ---- */

void
damage_rect(struct ws_server *srv, const ws_rect_t *r)
{
	if (r->width <= 0 || r->height <= 0)
		return;
	pixman_region32_union_rect(&srv->damage, &srv->damage, r->x, r->y,
	    (unsigned)r->width, (unsigned)r->height);
}

void
damage_all(struct ws_server *srv)
{
	pixman_region32_union_rect(&srv->damage, &srv->damage, 0, 0,
	    srv->width, srv->height);
}

/* ---- lookup ---- */

struct ws_window *
window_lookup(struct ws_server *srv, uint32_t id)
{
	struct ws_window *w;

	if (id == 0)
		return NULL;
	for (w = srv->windows; w != NULL; w = w->next)
		if (w->id == id)
			return w;
	return NULL;
}

static bool
rect_contains(const ws_rect_t *r, double x, double y)
{
	return x >= r->x && y >= r->y && x < (double)r->x + r->width &&
	    y < (double)r->y + r->height;
}

/* w takes pointer events at (x, y): in its frame and its input region. */
static bool
window_hit(const struct ws_window *w, double x, double y)
{
	if (w->flags & WS_WINDOW_IGNORES_MOUSE)
		return false;
	if (!rect_contains(&w->frame, x, y))
		return false;
	if (!w->has_input_region)
		return true;
	return pixman_region32_contains_point(
	    (pixman_region32_t *)&w->input_region,
	    (int)floor(x) - w->frame.x, (int)floor(y) - w->frame.y, NULL) != 0;
}

/* Topmost visible window under (x, y) that takes pointer events. */
struct ws_window *
window_at(struct ws_server *srv, double x, double y)
{
	struct ws_window *w;

	for (w = srv->stack_top; w != NULL; w = w->below)
		if (window_hit(w, x, y))
			return w;
	return NULL;
}

/*
 * The window under (x, y) if it is one of connection c's, else NULL
 * (another connection's window, a tile or the desktop is there): what
 * a pointer grab or query (WS_MSG_POINTER_GRAB, _QUERY_POINTER) calls
 * c's window under the pointer.
 */
struct ws_window *
window_at_owned(struct ws_server *srv, struct ws_client *c, double x, double y)
{
	struct ws_window *w = window_at(srv, x, y);

	return w != NULL && w->owner == c && w->tile_for == NULL ? w : NULL;
}

/*
 * WS_MSG_WINDOW_SET_INPUT_REGION: replace the input region with rects,
 * or add them (WS_REGION_APPEND), or go back to the whole window
 * (WS_REGION_RESET). It isn't clipped: window_hit() checks the frame
 * first, so the region fits whatever size the window has (an X server
 * sends the new shape before the resize).
 */
void
window_set_input_region(struct ws_server *srv, struct ws_window *w,
    const ws_rect_t *rects, unsigned n, uint32_t flags)
{
	unsigned i;

	if (flags & WS_REGION_RESET) {
		w->has_input_region = false;
		pixman_region32_clear(&w->input_region);
		pointer_recheck(srv);
		return;
	}
	if ((flags & WS_REGION_APPEND) == 0 || !w->has_input_region)
		pixman_region32_clear(&w->input_region);
	w->has_input_region = true;
	for (i = 0; i < n; i++) {
		if (rects[i].width <= 0 || rects[i].height <= 0)
			continue;
		pixman_region32_union_rect(&w->input_region, &w->input_region,
		    rects[i].x, rects[i].y, (unsigned)rects[i].width,
		    (unsigned)rects[i].height);
	}
	pointer_recheck(srv);
}

/* ---- stack ---- */

static void
stack_unlink(struct ws_server *srv, struct ws_window *w)
{
	if (w->below != NULL)
		w->below->above = w->above;
	else if (srv->stack_bottom == w)
		srv->stack_bottom = w->above;
	if (w->above != NULL)
		w->above->below = w->below;
	else if (srv->stack_top == w)
		srv->stack_top = w->below;
	w->above = w->below = NULL;
}

/* Insert w directly above `at` (NULL: at the very bottom). */
static void
stack_insert_above(struct ws_server *srv, struct ws_window *w,
    struct ws_window *at)
{
	if (at == NULL) {
		w->below = NULL;
		w->above = srv->stack_bottom;
		if (srv->stack_bottom != NULL)
			srv->stack_bottom->below = w;
		srv->stack_bottom = w;
		if (srv->stack_top == NULL)
			srv->stack_top = w;
		return;
	}
	w->below = at;
	w->above = at->above;
	if (at->above != NULL)
		at->above->below = w;
	else
		srv->stack_top = w;
	at->above = w;
}

/*
 * Place w in the stack. Levels always win: `rel` is honoured only if it
 * is visible and on the same level; otherwise w goes to the top (ABOVE)
 * or bottom (BELOW) of its level.
 */
static void
stack_place(struct ws_server *srv, struct ws_window *w, int op,
    struct ws_window *rel)
{
	struct ws_window *at;

	if (rel != NULL && (!rel->visible || rel == w || rel->level != w->level))
		rel = NULL;
	if (rel != NULL) {
		stack_insert_above(srv, w, op == WS_ORDER_ABOVE ? rel : rel->below);
		return;
	}
	if (op == WS_ORDER_ABOVE) {
		/* above the highest window whose level is <= ours */
		for (at = srv->stack_top; at != NULL; at = at->below)
			if (at->level <= w->level)
				break;
		stack_insert_above(srv, w, at);
	} else {
		/* below the lowest window whose level is >= ours */
		for (at = srv->stack_bottom; at != NULL; at = at->above)
			if (at->level >= w->level)
				break;
		stack_insert_above(srv, w, at != NULL ? at->below : srv->stack_top);
	}
}

/* ---- key window and activation ---- */

static bool
can_be_key(const struct ws_window *w)
{
	return w != NULL && w->visible && (w->flags & WS_WINDOW_NO_KEY) == 0;
}

static void
send_simple(struct ws_server *srv, struct ws_client *c, uint16_t type,
    uint32_t window, uint32_t other)
{
	ws_event_t ev;

	event_init(srv, &ev, type, sizeof(ev.u.app), window);
	ev.u.app.other_client = other;
	client_send(srv, c, &ev, -1);
}

/* Send an app event to every connection of app `a`. */
static void
send_app(struct ws_server *srv, struct ws_client *a, uint16_t type,
    uint32_t other)
{
	struct ws_client *m;

	for (m = srv->clients; m != NULL; m = m->next)
		if (!m->dead && m->app == a)
			send_simple(srv, m, type, 0, other);
}

/*
 * Set app a's key window (a window of any of its connections). Focus
 * events go out only for the active app, each to the window's owner.
 */
static void
set_key_window(struct ws_server *srv, struct ws_client *a, struct ws_window *w)
{
	struct ws_window *old = a->key_window;

	if (old == w)
		return;
	a->key_window = w;
	dock_list_changed(srv);
	if (srv->active != a)
		return;
	if (old != NULL)
		send_simple(srv, old->owner, WS_EVENT_KEY_WINDOW_LOST, old->id, 0);
	if (w != NULL)
		send_simple(srv, w->owner, WS_EVENT_KEY_WINDOW_GAINED, w->id, 0);
	keyboard_focus_update(srv);
}

/*
 * Make app c (a client->app) the active app. The old app first loses
 * its key window and all its connections are told it was deactivated;
 * then all of c's connections are told it was activated and `key` (or
 * the remembered key window) becomes key.
 */
void
app_activate(struct ws_server *srv, struct ws_client *c, struct ws_window *key)
{
	struct ws_client *old = srv->active;

	if (c != NULL && (c->dead || (c->flags & WS_CONNECT_TOOL)))
		return;
	/* Another app takes over: a pointer grab of the old one ends. */
	if (srv->pgrab != NULL && srv->pgrab->app != c)
		pointer_ungrab(srv, srv->pgrab, true);
	if (old == c) {
		if (c != NULL && key != NULL)
			set_key_window(srv, c, key);
		return;
	}
	if (old != NULL) {
		if (old->key_window != NULL)
			send_simple(srv, old->key_window->owner,
			    WS_EVENT_KEY_WINDOW_LOST, old->key_window->id, 0);
		send_app(srv, old, WS_EVENT_APP_DEACTIVATED,
		    c != NULL ? c->id : 0);
	}
	srv->active = c;
	dock_list_changed(srv);
	if (c == NULL) {
		ws_debug("no active app");
		keyboard_focus_update(srv);
		return;
	}
	if (key != NULL)
		c->key_window = key;
	if (c->key_window != NULL && !can_be_key(c->key_window))
		c->key_window = NULL;
	ws_debug("active app: %u (%s)", c->id, c->app_name);
	send_app(srv, c, WS_EVENT_APP_ACTIVATED, old != NULL ? old->id : 0);
	if (c->key_window != NULL)
		send_simple(srv, c->key_window->owner,
		    WS_EVENT_KEY_WINDOW_GAINED, c->key_window->id, 0);
	keyboard_focus_update(srv);
}

/*
 * App c gives up being active (it asked, quit, or crashed). The app
 * owning the topmost normal-or-higher window of another app takes over.
 */
void
app_deactivate(struct ws_server *srv, struct ws_client *c)
{
	struct ws_window *w;
	struct ws_client *next = NULL;

	if (srv->active != c)
		return;
	for (w = srv->stack_top; w != NULL; w = w->below) {
		if (w->tile_for != NULL)
			continue;	/* a minimized window's tile */
		if (w->owner->app != c && !w->owner->dead &&
		    (w->owner->flags & WS_CONNECT_TOOL) == 0 &&
		    w->level >= WS_LEVEL_NORMAL && w->level <= WS_LEVEL_CLIENT_MAX) {
			next = w->owner->app;
			break;
		}
	}
	if (c->dead) {
		/* no events to a dead client */
		srv->active = NULL;
		dock_list_changed(srv);
		if (next != NULL)
			app_activate(srv, next, NULL);
		return;
	}
	/* With next == NULL this still tells c, and leaves no active app. */
	app_activate(srv, next, NULL);
}

/*
 * Connection c is going away and its windows are gone. If it joined an
 * app, it just leaves it. If other connections joined c's app, the
 * oldest of them carries the app on, with its key window and its place
 * as the active app, so the app doesn't deactivate.
 */
void
app_member_gone(struct ws_server *srv, struct ws_client *c)
{
	struct ws_client *m, *heir = NULL;

	if (c->app != c) {
		c->app = c;
		return;
	}
	/* srv->clients is newest first: the last match is the oldest. */
	for (m = srv->clients; m != NULL; m = m->next)
		if (m != c && !m->dead && m->app == c)
			heir = m;
	if (heir == NULL)
		return;
	for (m = srv->clients; m != NULL; m = m->next)
		if (m != c && !m->dead && m->app == c)
			m->app = heir;
	heir->key_window = c->key_window;
	c->key_window = NULL;
	if (srv->active == c)
		srv->active = heir;
	ws_info("client %u (%s) carries on app %u", heir->id, heir->app_name,
	    c->id);
}

void
window_make_key(struct ws_server *srv, struct ws_window *w)
{
	if (w->flags & WS_WINDOW_NO_KEY)
		return;
	set_key_window(srv, w->owner->app, w);
}

/* ---- create / destroy ---- */

static void window_unminimize(struct ws_server *srv, struct ws_window *w);

static int32_t
clamp_level(int32_t level)
{
	if (level > WS_LEVEL_CLIENT_MAX)
		return WS_LEVEL_CLIENT_MAX;
	if (level < WS_LEVEL_DESKTOP)
		return WS_LEVEL_DESKTOP;
	return level;
}

/*
 * WS_MSG_RESERVE_WINDOW_IDS: a block of `count` ids for c to name its
 * new windows with, from the same space as the server's own ids.
 */
int
window_reserve_ids(struct ws_server *srv, struct ws_client *c, uint32_t count,
    uint32_t *base)
{
	if (count == 0 || count > WS_MAX_WINDOW_IDS)
		return EINVAL;
	if (srv->next_window_id > UINT32_MAX - count)
		return ENOSPC;
	*base = srv->next_window_id;
	srv->next_window_id += count;
	c->wid_next = *base;
	c->wid_end = *base + count;
	return 0;
}

/* May c create a window with id `id` (from its block, not used yet)? */
int
window_create_check_id(struct ws_server *srv, struct ws_client *c, uint32_t id)
{
	(void)srv;
	if (id == 0 || id < c->wid_next || id >= c->wid_end)
		return EINVAL;
	return 0;
}

/* id: one window_create_check_id() accepted, or 0 for the next free one. */
struct ws_window *
window_create(struct ws_server *srv, struct ws_client *c, const ws_rect_t *frame,
    int32_t level, uint32_t flags, const char *title, uint32_t id)
{
	struct ws_window *w;

	w = calloc(1, sizeof(*w));
	if (w == NULL)
		return NULL;
	if (id != 0) {
		w->id = id;
		c->wid_next = id + 1;
	} else
		w->id = srv->next_window_id++;
	w->owner = c;
	w->level = clamp_level(level);
	w->flags = flags;
	w->frame = *frame;
	if (w->frame.width < 0)
		w->frame.width = 0;
	if (w->frame.height < 0)
		w->frame.height = 0;
	strlcpy(w->title, title, sizeof(w->title));
	pixman_region32_init(&w->pending_damage);
	pixman_region32_init(&w->input_region);
	w->next = srv->windows;
	srv->windows = w;
	dock_list_changed(srv);
	ws_debug("window %u created for %s level %d (%d,%d %dx%d)", w->id,
	    c->app_name, w->level, w->frame.x, w->frame.y, w->frame.width,
	    w->frame.height);
	return w;
}

static void
window_release_buffer(struct ws_server *srv, struct ws_window *w,
    struct ws_buffer *b)
{
	ws_event_t ev;

	if (b == NULL)
		return;
	if (b->client_ref && b->owner != NULL) {
		event_init(srv, &ev, WS_EVENT_BUFFER_RELEASE,
		    sizeof(ev.u.buffer), w->id);
		ev.u.buffer.buffer_id = b->id;
		client_send(srv, b->owner, &ev, -1);
	}
	buffer_unref(srv, b);
}

void
window_destroy(struct ws_server *srv, struct ws_window *w)
{
	struct ws_window **pp;
	struct ws_client *c = w->owner;

	window_unminimize(srv, w);
	if (w->visible)
		window_order(srv, w, WS_ORDER_OUT, NULL);
	if (c->app->key_window == w)
		set_key_window(srv, c->app, NULL);
	pointer_window_gone(srv, w);
	scroll_window_gone(srv, w);
	if (w->pending_attach)
		buffer_unref(srv, w->pending);
	if (!c->dead)
		window_release_buffer(srv, w, w->buffer);
	else
		buffer_unref(srv, w->buffer);
	pixman_region32_fini(&w->pending_damage);
	pixman_region32_fini(&w->input_region);
	if (w->cursor_buffer != NULL) {
		cursor_buffer_gone(srv, w->cursor_buffer);
		buffer_unref(srv, w->cursor_buffer);
		w->cursor_buffer = NULL;
	}
	for (pp = &srv->windows; *pp != NULL; pp = &(*pp)->next)
		if (*pp == w) {
			*pp = w->next;
			break;
		}
	dock_list_changed(srv);
	ws_debug("window %u destroyed", w->id);
	free(w);
}

/* ---- ordering, level, frame ---- */

/* ---- icon placement ---- */

static bool
rects_overlap(const ws_rect_t *a, const ws_rect_t *b)
{
	return a->x < b->x + b->width && b->x < a->x + a->width &&
	    a->y < b->y + b->height && b->y < a->y + a->height;
}

/* Another visible icon window covers r. */
static bool
icon_overlaps(struct ws_server *srv, const struct ws_window *w,
    const ws_rect_t *r)
{
	struct ws_window *o;

	for (o = srv->stack_bottom; o != NULL; o = o->above)
		if (o != w && (o->flags & WS_WINDOW_ICON) != 0 &&
		    rects_overlap(r, &o->frame))
			return (true);
	return (false);
}

/*
 * App icons and miniwindows ask for a place only where their app thinks
 * one is free, and every app thinks the bottom-left corner is. So when
 * one is shown over another, it moves to the first free place along the
 * bottom of the usable area (the screen minus the Dock's strip, see
 * screen.c), left to right, then the row above, as a window manager does
 * for X11 apps, and its app hears where with WS_EVENT_WINDOW_FRAME. One
 * that's already clear, or that's been put somewhere by a GSIconManager
 * (a Dock), stays where it was asked to be. Dragging one onto another
 * later is the user's choice and is left alone. The server's own tiles
 * also move when they're not inside the usable area (under the Dock).
 */
static bool
rect_inside(const ws_rect_t *outer, const ws_rect_t *r)
{
	return r->x >= outer->x && r->y >= outer->y &&
	    r->x + r->width <= outer->x + outer->width &&
	    r->y + r->height <= outer->y + outer->height;
}

static void
place_icon(struct ws_server *srv, struct ws_window *w)
{
	ws_rect_t r = w->frame, u = srv->usable;
	ws_event_t ev;
	int32_t cols, rows, i;

	if (r.width <= 0 || r.height <= 0)
		return;
	if (!icon_overlaps(srv, w, &r) &&
	    (w->tile_for == NULL || rect_inside(&u, &r)))
		return;
	if (u.width <= 0 || u.height <= 0) {
		u.x = u.y = 0;
		u.width = (int32_t)srv->width;
		u.height = (int32_t)srv->height;
	}
	cols = u.width / r.width;
	rows = u.height / r.height;
	for (i = 0; i < cols * rows; i++) {
		r.x = u.x + (i % cols) * r.width;
		r.y = u.y + u.height - (i / cols + 1) * r.height;
		if (!icon_overlaps(srv, w, &r))
			break;
	}
	if (i == cols * rows)
		return;		/* the screen is full of icons: leave it */
	w->frame = r;
	if (w->tile_for != NULL)
		return;		/* the server's own tile */
	event_init(srv, &ev, WS_EVENT_WINDOW_FRAME, sizeof(ev.u.frame), w->id);
	ev.u.frame.frame = r;
	client_send(srv, w->owner, &ev, -1);
}

void
window_order(struct ws_server *srv, struct ws_window *w, int op,
    struct ws_window *rel)
{
	struct ws_client *c = w->owner;

	/* Ordering a minimized window in restores it; ordering it out
	 * (its app is hiding) just takes the tile away. */
	window_unminimize(srv, w);
	if (op == WS_ORDER_OUT) {
		if (!w->visible)
			return;
		stack_unlink(srv, w);
		w->visible = false;
		dock_list_changed(srv);
		damage_rect(srv, &w->frame);
		if (c->app->key_window == w)
			set_key_window(srv, c->app, NULL);
		pointer_window_gone(srv, w);
		return;
	}
	if (w->visible) {
		if (rel == NULL && op == WS_ORDER_ABOVE &&
		    (w->above == NULL || w->above->level > w->level))
			return;		/* already on top of its level */
		if (rel == NULL && op == WS_ORDER_BELOW &&
		    (w->below == NULL || w->below->level < w->level))
			return;
		stack_unlink(srv, w);
	} else if ((w->flags & WS_WINDOW_ICON) != 0)
		place_icon(srv, w);
	w->visible = true;
	stack_place(srv, w, op, rel);
	damage_rect(srv, &w->frame);
	pointer_recheck(srv);
	dock_list_changed(srv);
}

/* Tell w's owner where the server put w (WS_EVENT_WINDOW_ORDERED). */
static void
send_ordered(struct ws_server *srv, struct ws_window *w)
{
	struct ws_window *o;
	ws_event_t ev;

	event_init(srv, &ev, WS_EVENT_WINDOW_ORDERED, sizeof(ev.u.ordered),
	    w->id);
	ev.u.ordered.op = w->visible ? WS_ORDER_ABOVE : WS_ORDER_OUT;
	ev.u.ordered.level = w->level;
	if (w->visible) {
		for (o = w->below; o != NULL && (o->owner != w->owner ||
		    o->tile_for != NULL); o = o->below)
			;
		ev.u.ordered.below = o != NULL ? o->id : 0;
		for (o = w->above; o != NULL && (o->owner != w->owner ||
		    o->tile_for != NULL); o = o->above)
			;
		ev.u.ordered.above = o != NULL ? o->id : 0;
	}
	client_send(srv, w->owner, &ev, -1);
}

/*
 * window_order() for the server's own reasons, not the owner's request:
 * if w's place changed, the owner hears about it.
 */
void
window_order_by_server(struct ws_server *srv, struct ws_window *w, int op,
    struct ws_window *rel)
{
	struct ws_window *below = w->below, *above = w->above;
	bool visible = w->visible;

	window_order(srv, w, op, rel);
	if (w->visible == visible &&
	    (!visible || (w->below == below && w->above == above)))
		return;
	send_ordered(srv, w);
}

void
window_set_level(struct ws_server *srv, struct ws_window *w, int32_t level)
{
	level = clamp_level(level);
	if (level == w->level)
		return;
	w->level = level;
	dock_list_changed(srv);
	if (w->visible) {
		stack_unlink(srv, w);
		stack_place(srv, w, WS_ORDER_ABOVE, NULL);
		damage_rect(srv, &w->frame);
		pointer_recheck(srv);
	}
}

void
window_set_frame(struct ws_server *srv, struct ws_window *w,
    const ws_rect_t *frame)
{
	if (memcmp(&w->frame, frame, sizeof(*frame)) == 0)
		return;
	if (w->visible)
		damage_rect(srv, &w->frame);
	w->frame = *frame;
	if (w->frame.width < 0)
		w->frame.width = 0;
	if (w->frame.height < 0)
		w->frame.height = 0;
	if (w->visible) {
		damage_rect(srv, &w->frame);
		pointer_recheck(srv);
	}
	srv->ov.dirty = true;	/* the overview lays out again */
	if (w->tile != NULL) {
		w->tile->tile_dirty = true;
		damage_rect(srv, &w->tile->frame);
	} else if (w->minimized)
		dock_preview_changed(srv, w);
}

/* ---- buffers and commits ---- */

void
window_attach(struct ws_server *srv, struct ws_window *w, struct ws_buffer *b)
{
	if (w->pending_attach)
		buffer_unref(srv, w->pending);
	if (b != NULL)
		b->refs++;
	w->pending = b;
	w->pending_attach = true;
}

void
window_damage(struct ws_server *srv, struct ws_window *w,
    const ws_rect_t *rects, unsigned n)
{
	unsigned i;

	(void)srv;
	for (i = 0; i < n; i++) {
		if (rects[i].width <= 0 || rects[i].height <= 0)
			continue;
		pixman_region32_union_rect(&w->pending_damage,
		    &w->pending_damage, rects[i].x, rects[i].y,
		    (unsigned)rects[i].width, (unsigned)rects[i].height);
	}
}

/*
 * Commit what was attached and damaged. With WS_COMMIT_SET_FRAME the
 * window takes `frame` in the same step (WS_SERVER_COMMIT_FRAME), so the
 * screen never shows the new frame with the old buffer: nothing is
 * composited between the two.
 */
void
window_commit(struct ws_server *srv, struct ws_window *w, uint32_t flags,
    const ws_rect_t *frame)
{
	pixman_region32_t r;
	bool full = false;

	if ((flags & WS_COMMIT_SET_FRAME) && frame != NULL)
		window_set_frame(srv, w, frame);
	if (w->pending_attach) {
		struct ws_buffer *old = w->buffer;

		w->buffer = w->pending;	/* reference moves */
		w->pending = NULL;
		w->pending_attach = false;
		if (old != w->buffer) {
			full = !pixman_region32_not_empty(&w->pending_damage);
			window_release_buffer(srv, w, old);
		} else {
			buffer_unref(srv, old);	/* re-attach of the same one */
		}
	}
	if (w->visible) {
		if (full) {
			damage_rect(srv, &w->frame);
		} else {
			pixman_region32_init(&r);
			pixman_region32_copy(&r, &w->pending_damage);
			pixman_region32_intersect_rect(&r, &r, 0, 0,
			    (unsigned)w->frame.width, (unsigned)w->frame.height);
			pixman_region32_translate(&r, w->frame.x, w->frame.y);
			pixman_region32_union(&srv->damage, &srv->damage, &r);
			pixman_region32_fini(&r);
		}
	}
	overview_window_committed(srv, w);
	if (w->tile != NULL) {
		/* The tile shows the new frame. */
		w->tile->tile_dirty = true;
		damage_rect(srv, &w->tile->frame);
	} else if (w->minimized)
		dock_preview_changed(srv, w);	/* the Dock's preview does */
	pixman_region32_clear(&w->pending_damage);
	if (flags & WS_COMMIT_FRAME_DONE)
		w->frame_requested = true;
}

/* ---- window list (wsctl, the Dock) ---- */

static void
list_one(struct ws_server *srv, struct ws_client *to, struct ws_window *lw,
    uint32_t flags, uint32_t idx)
{
	ws_event_t ev;

	event_init(srv, &ev, WS_EVENT_WINDOW_INFO, sizeof(ev.u.window_info),
	    lw->id);
	ev.u.window_info.client_id = lw->owner->id;
	ev.u.window_info.pid = lw->owner->pid;
	ev.u.window_info.level = lw->level;
	if (lw->owner->app->key_window == lw)
		flags |= WS_WINDOW_INFO_KEY;
	if (srv->active == lw->owner->app)
		flags |= WS_WINDOW_INFO_ACTIVE;
	else if (to->flags & WS_CONNECT_CONTROL)
		flags &= ~WS_WINDOW_INFO_KEY;	/* as wsctl always showed it */
	ev.u.window_info.flags = flags;
	ev.u.window_info.frame = lw->frame;
	ev.u.window_info.stack_index = idx;
	strlcpy(ev.u.window_info.app_name, lw->owner->app_name,
	    sizeof(ev.u.window_info.app_name));
	strlcpy(ev.u.window_info.title, lw->title,
	    sizeof(ev.u.window_info.title));
	ev.u.window_info.window = lw->id;
	ev.u.window_info.app_id = lw->owner->app->id;
	ev.u.window_info.window_flags = lw->flags;
	client_send(srv, to, &ev, -1);
}

/*
 * The stack, bottom to top, with each tile standing for its minimized
 * window; then minimized windows the Dock shows (no tile), then, with
 * WS_LIST_ORDERED_OUT, the windows that aren't shown.
 */
void
list_windows(struct ws_server *srv, struct ws_client *to, uint32_t options)
{
	struct ws_window *w;
	ws_event_t ev;
	uint32_t idx = 0;

	for (w = srv->stack_bottom; w != NULL; w = w->above, idx++) {
		if (w->tile_for != NULL)
			list_one(srv, to, w->tile_for, WS_WINDOW_INFO_MINIMIZED,
			    idx);
		else
			list_one(srv, to, w, WS_WINDOW_INFO_VISIBLE, idx);
	}
	for (w = srv->windows; w != NULL; w = w->next)
		if (w->minimized && w->tile == NULL)
			list_one(srv, to, w, WS_WINDOW_INFO_MINIMIZED |
			    WS_WINDOW_INFO_IN_DOCK, WS_STACK_INDEX_NONE);
	if (options & WS_LIST_ORDERED_OUT)
		for (w = srv->windows; w != NULL; w = w->next)
			if (!w->visible && !w->minimized)
				list_one(srv, to, w, 0, WS_STACK_INDEX_NONE);
	event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
	client_send(srv, to, &ev, -1);
}

/* ---- minimizing ---- */

/*
 * Minimize w: order it out and, unless the Dock shows it (dock.c), put
 * a tile in its place. `where` is WS_MINIMIZE_*: into the Dock, onto the
 * bottom row, or (WS_MINIMIZE_DEFAULT, a bare request, anything else)
 * what MinimizeIntoDock says. A window that's already minimized moves to
 * where the new request says.
 */
void
window_minimize(struct ws_server *srv, struct ws_window *w, int32_t where)
{
	bool into_dock;

	if (where == WS_MINIMIZE_INTO_DOCK)
		into_dock = true;
	else if (where == WS_MINIMIZE_BOTTOM_ROW)
		into_dock = false;
	else
		into_dock = srv->cfg.minimize_into_dock;
	if (w->minimized) {
		if (w->into_dock == into_dock)
			return;
		w->into_dock = into_dock;
		if (dock_shows_minimized(srv, w))
			window_tile_remove(srv, w);
		else
			window_tile_add(srv, w);
		dock_list_changed(srv);
		return;
	}
	if (w->visible)
		window_order(srv, w, WS_ORDER_OUT, NULL);
	w->minimized = true;
	w->into_dock = into_dock;
	dock_list_changed(srv);
	if (dock_shows_minimized(srv, w)) {
		ws_debug("window %u minimized into the Dock", w->id);
		return;
	}
	if (into_dock)
		ws_debug("window %u minimized into the Dock; no Dock, so "
		    "onto the bottom row", w->id);
	window_tile_add(srv, w);
}

/*
 * Give minimized window w its tile, at the icon level, in the first
 * free place along the bottom of the usable area (the same placement as
 * app icons). The tile is the server's own: it is in the stack but not in
 * srv->windows, so clients can't name it, and tile.c draws it from w's
 * buffer.
 */
void
window_tile_add(struct ws_server *srv, struct ws_window *w)
{
	struct ws_window *t;
	int32_t size = srv->cfg.minimize_tile_size;

	if (w->tile != NULL || !w->minimized)
		return;
	t = calloc(1, sizeof(*t));
	if (t == NULL) {
		client_error(srv, w->owner, WS_MSG_WINDOW_MINIMIZE, w->id,
		    ENOMEM, "no memory for a tile");
		return;
	}
	t->owner = w->owner;
	t->tile_for = w;
	t->level = WS_LEVEL_DOCK;
	t->flags = WS_WINDOW_ICON | WS_WINDOW_NO_KEY | WS_WINDOW_NO_ACTIVATE;
	/* the bottom-left corner of the usable area: just above a bottom
	   Dock, right of a left one */
	t->frame.x = srv->usable.x;
	t->frame.y = srv->usable.y + srv->usable.height - size;
	t->frame.width = size;
	t->frame.height = size;
	pixman_region32_init(&t->pending_damage);
	w->tile = t;
	place_icon(srv, t);
	t->visible = true;
	t->tile_dirty = true;
	stack_place(srv, t, WS_ORDER_ABOVE, NULL);
	damage_rect(srv, &t->frame);
	pointer_recheck(srv);
	dock_list_changed(srv);
	ws_debug("window %u minimized, tile at %d,%d", w->id, t->frame.x,
	    t->frame.y);
}

/* Take w's tile away, if it has one (restored, hidden, destroyed, or
 * handed to the Dock). */
void
window_tile_remove(struct ws_server *srv, struct ws_window *w)
{
	struct ws_window *t = w->tile;

	if (t == NULL)
		return;
	w->tile = NULL;
	stack_unlink(srv, t);
	t->visible = false;
	damage_rect(srv, &t->frame);
	if (srv->tile_selected == t)
		srv->tile_selected = NULL;
	pointer_window_gone(srv, t);
	dock_list_changed(srv);
	ws_debug("window %u: tile removed", w->id);
	tile_free(t);
}

/* w is no longer minimized: its owner ordered it in or out, or it's
 * going away. */
static void
window_unminimize(struct ws_server *srv, struct ws_window *w)
{
	if (!w->minimized)
		return;
	w->minimized = false;
	w->preview_dirty = false;
	window_tile_remove(srv, w);
	dock_list_changed(srv);
}

/*
 * Ask the owner of minimized window w to restore it (a double-click on
 * its tile, or the Dock): activate the app and send
 * WS_EVENT_WINDOW_RESTORE. The window stays minimized until the owner
 * orders it in.
 */
void
window_request_restore(struct ws_server *srv, struct ws_window *w)
{
	ws_event_t ev;

	app_activate(srv, w->owner->app, NULL);
	event_init(srv, &ev, WS_EVENT_WINDOW_RESTORE, 0, w->id);
	client_send(srv, w->owner, &ev, -1);
}

/* Move tile t to x, y, kept inside the usable area. */
static void
tile_move_inside(struct ws_server *srv, struct ws_window *t, int32_t x,
    int32_t y)
{
	const ws_rect_t *u = &srv->usable;

	if (x > u->x + u->width - t->frame.width)
		x = u->x + u->width - t->frame.width;
	if (y > u->y + u->height - t->frame.height)
		y = u->y + u->height - t->frame.height;
	if (x < u->x)
		x = u->x;
	if (y < u->y)
		y = u->y;
	if (x == t->frame.x && y == t->frame.y)
		return;
	damage_rect(srv, &t->frame);
	t->frame.x = x;
	t->frame.y = y;
	damage_rect(srv, &t->frame);
}

/* The user dragged tile t to x, y: it stays there from now on, inside
 * the usable area. */
void
window_tile_move(struct ws_server *srv, struct ws_window *t, int32_t x,
    int32_t y)
{
	t->tile_moved = true;
	tile_move_inside(srv, t, x, y);
}

/*
 * The usable area changed (the Dock moved, or the screen changed): put
 * the tiles inside it again. Tiles the user dragged stay where they are
 * if they still fit, or move just inside it. The others go along its
 * bottom again, in stacking order, out of the way first so each one
 * only sees those placed before it.
 */
void
window_tiles_place(struct ws_server *srv)
{
	struct ws_window *t;

	for (t = srv->stack_bottom; t != NULL; t = t->above)
		if (t->tile_for != NULL && t->tile_moved)
			tile_move_inside(srv, t, t->frame.x, t->frame.y);
	for (t = srv->stack_bottom; t != NULL; t = t->above)
		if (t->tile_for != NULL && !t->tile_moved) {
			damage_rect(srv, &t->frame);
			t->frame.x = t->frame.y = INT32_MIN / 2;
		}
	for (t = srv->stack_bottom; t != NULL; t = t->above)
		if (t->tile_for != NULL && !t->tile_moved) {
			t->frame.x = srv->usable.x;
			t->frame.y = srv->usable.y + srv->usable.height -
			    t->frame.height;
			place_icon(srv, t);
			damage_rect(srv, &t->frame);
			ws_debug("window %u: tile at %d,%d", t->tile_for->id,
			    t->frame.x, t->frame.y);
		}
	pointer_recheck(srv);
}

/* Window edges that must stay on the screen when a window was partly
 * off it already. */
#define KEEP_VISIBLE_X	32
#define KEEP_VISIBLE_Y	24

/* v within lo..hi; lo wins if hi < lo. */
static int32_t
clamp32(int32_t v, int32_t lo, int32_t hi)
{
	if (v > hi)
		v = hi;
	if (v < lo)
		v = lo;
	return v;
}

/*
 * The screen changed from old_width x old_height to its current size:
 * a shown or minimized window that was on the old screen and no longer
 * fits on the new one moves back onto it (its top-left corner first if
 * it's bigger than the screen); one that was partly off already keeps at
 * least its top-left part on the screen. Their owners hear where with
 * WS_EVENT_WINDOW_FRAME.
 */
void
windows_keep_on_screen(struct ws_server *srv, uint32_t old_width,
    uint32_t old_height)
{
	ws_rect_t old = { 0, 0, (int32_t)old_width, (int32_t)old_height };
	ws_rect_t scr = { 0, 0, (int32_t)srv->width, (int32_t)srv->height };
	struct ws_window *w;
	ws_event_t ev;
	ws_rect_t r;

	for (w = srv->windows; w != NULL; w = w->next) {
		r = w->frame;
		/* Windows that aren't shown (an app's offscreen helpers)
		   stay where their app put them. */
		if ((!w->visible && !w->minimized) || r.width <= 0 ||
		    r.height <= 0 || rect_inside(&scr, &r))
			continue;
		if (rect_inside(&old, &r)) {
			r.x = clamp32(r.x, 0, scr.width - r.width);
			r.y = clamp32(r.y, 0, scr.height - r.height);
		} else {
			r.x = clamp32(r.x, KEEP_VISIBLE_X - r.width,
			    scr.width - KEEP_VISIBLE_X);
			r.y = clamp32(r.y, r.y < 0 ? r.y : 0,
			    scr.height - KEEP_VISIBLE_Y);
		}
		if (r.x == w->frame.x && r.y == w->frame.y)
			continue;
		ws_debug("window %u kept on the screen: %d,%d -> %d,%d",
		    w->id, w->frame.x, w->frame.y, r.x, r.y);
		window_set_frame(srv, w, &r);
		event_init(srv, &ev, WS_EVENT_WINDOW_FRAME, sizeof(ev.u.frame),
		    w->id);
		ev.u.frame.frame = r;
		client_send(srv, w->owner, &ev, -1);
	}
}

/* Bring tile t to the top of its level. */
void
window_tile_raise(struct ws_server *srv, struct ws_window *t)
{
	if (t->above == NULL || t->above->level > t->level)
		return;
	stack_unlink(srv, t);
	stack_place(srv, t, WS_ORDER_ABOVE, NULL);
	damage_rect(srv, &t->frame);
}
