/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The window model: the visible stack (strictly by level, then by
 * order within a level), key windows and the single active app.
 */

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

/* Topmost visible window under (x, y) that takes pointer events. */
struct ws_window *
window_at(struct ws_server *srv, double x, double y)
{
	struct ws_window *w;

	for (w = srv->stack_top; w != NULL; w = w->below) {
		if (w->flags & WS_WINDOW_IGNORES_MOUSE)
			continue;
		if (rect_contains(&w->frame, x, y))
			return w;
	}
	return NULL;
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

/* Set an app's key window. Focus events go out only for the active app. */
static void
set_key_window(struct ws_server *srv, struct ws_client *c, struct ws_window *w)
{
	struct ws_window *old = c->key_window;

	if (old == w)
		return;
	c->key_window = w;
	if (srv->active != c)
		return;
	if (old != NULL)
		send_simple(srv, c, WS_EVENT_KEY_WINDOW_LOST, old->id, 0);
	if (w != NULL)
		send_simple(srv, c, WS_EVENT_KEY_WINDOW_GAINED, w->id, 0);
}

/*
 * Make c the active app. The old app first loses its key window and is
 * told it was deactivated; then c is activated and gets `key` (or its
 * remembered key window) as key window.
 */
void
app_activate(struct ws_server *srv, struct ws_client *c, struct ws_window *key)
{
	struct ws_client *old = srv->active;

	if (c != NULL && (c->dead || (c->flags & WS_CONNECT_CONTROL)))
		return;
	if (old == c) {
		if (c != NULL && key != NULL)
			set_key_window(srv, c, key);
		return;
	}
	if (old != NULL) {
		if (old->key_window != NULL)
			send_simple(srv, old, WS_EVENT_KEY_WINDOW_LOST,
			    old->key_window->id, 0);
		send_simple(srv, old, WS_EVENT_APP_DEACTIVATED, 0,
		    c != NULL ? c->id : 0);
	}
	srv->active = c;
	if (c == NULL) {
		ws_debug("no active app");
		return;
	}
	if (key != NULL)
		c->key_window = key;
	if (c->key_window != NULL && !can_be_key(c->key_window))
		c->key_window = NULL;
	ws_debug("active app: %u (%s)", c->id, c->app_name);
	send_simple(srv, c, WS_EVENT_APP_ACTIVATED, 0, old != NULL ? old->id : 0);
	if (c->key_window != NULL)
		send_simple(srv, c, WS_EVENT_KEY_WINDOW_GAINED,
		    c->key_window->id, 0);
}

/*
 * c gives up being active (it asked, quit, or crashed). The owner of
 * the topmost normal-or-higher window of another app takes over.
 */
void
app_deactivate(struct ws_server *srv, struct ws_client *c)
{
	struct ws_window *w;
	struct ws_client *next = NULL;

	if (srv->active != c)
		return;
	for (w = srv->stack_top; w != NULL; w = w->below) {
		if (w->owner != c && w->owner != NULL && !w->owner->dead &&
		    (w->owner->flags & WS_CONNECT_CONTROL) == 0 &&
		    w->level >= WS_LEVEL_NORMAL && w->level <= WS_LEVEL_CLIENT_MAX) {
			next = w->owner;
			break;
		}
	}
	if (c->dead) {
		/* no events to a dead client */
		srv->active = NULL;
		if (next != NULL)
			app_activate(srv, next, NULL);
		return;
	}
	/* With next == NULL this still tells c, and leaves no active app. */
	app_activate(srv, next, NULL);
}

void
window_make_key(struct ws_server *srv, struct ws_window *w)
{
	if (w->flags & WS_WINDOW_NO_KEY)
		return;
	set_key_window(srv, w->owner, w);
}

/* ---- create / destroy ---- */

static int32_t
clamp_level(int32_t level)
{
	if (level > WS_LEVEL_CLIENT_MAX)
		return WS_LEVEL_CLIENT_MAX;
	if (level < WS_LEVEL_DESKTOP)
		return WS_LEVEL_DESKTOP;
	return level;
}

struct ws_window *
window_create(struct ws_server *srv, struct ws_client *c, const ws_rect_t *frame,
    int32_t level, uint32_t flags, const char *title)
{
	struct ws_window *w;

	w = calloc(1, sizeof(*w));
	if (w == NULL)
		return NULL;
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
	w->next = srv->windows;
	srv->windows = w;
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

	if (w->visible)
		window_order(srv, w, WS_ORDER_OUT, NULL);
	if (c->key_window == w)
		set_key_window(srv, c, NULL);
	pointer_window_gone(srv, w);
	if (w->pending_attach)
		buffer_unref(srv, w->pending);
	if (!c->dead)
		window_release_buffer(srv, w, w->buffer);
	else
		buffer_unref(srv, w->buffer);
	pixman_region32_fini(&w->pending_damage);
	for (pp = &srv->windows; *pp != NULL; pp = &(*pp)->next)
		if (*pp == w) {
			*pp = w->next;
			break;
		}
	ws_debug("window %u destroyed", w->id);
	free(w);
}

/* ---- ordering, level, frame ---- */

void
window_order(struct ws_server *srv, struct ws_window *w, int op,
    struct ws_window *rel)
{
	struct ws_client *c = w->owner;

	if (op == WS_ORDER_OUT) {
		if (!w->visible)
			return;
		stack_unlink(srv, w);
		w->visible = false;
		damage_rect(srv, &w->frame);
		if (c->key_window == w)
			set_key_window(srv, c, NULL);
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
	}
	w->visible = true;
	stack_place(srv, w, op, rel);
	damage_rect(srv, &w->frame);
	pointer_recheck(srv);
}

void
window_set_level(struct ws_server *srv, struct ws_window *w, int32_t level)
{
	level = clamp_level(level);
	if (level == w->level)
		return;
	w->level = level;
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

void
window_commit(struct ws_server *srv, struct ws_window *w, uint32_t flags)
{
	pixman_region32_t r;
	bool full = false;

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
	pixman_region32_clear(&w->pending_damage);
	if (flags & WS_COMMIT_FRAME_DONE)
		w->frame_requested = true;
}

/* ---- window list (wsctl) ---- */

void
list_windows(struct ws_server *srv, struct ws_client *to)
{
	struct ws_window *w;
	ws_event_t ev;
	uint32_t idx = 0;

	for (w = srv->stack_bottom; w != NULL; w = w->above, idx++) {
		event_init(srv, &ev, WS_EVENT_WINDOW_INFO,
		    sizeof(ev.u.window_info), w->id);
		ev.u.window_info.client_id = w->owner->id;
		ev.u.window_info.pid = w->owner->pid;
		ev.u.window_info.level = w->level;
		ev.u.window_info.flags = WS_WINDOW_INFO_VISIBLE;
		if (srv->active == w->owner) {
			ev.u.window_info.flags |= WS_WINDOW_INFO_ACTIVE;
			if (w->owner->key_window == w)
				ev.u.window_info.flags |= WS_WINDOW_INFO_KEY;
		}
		ev.u.window_info.frame = w->frame;
		ev.u.window_info.stack_index = idx;
		strlcpy(ev.u.window_info.app_name, w->owner->app_name,
		    sizeof(ev.u.window_info.app_name));
		strlcpy(ev.u.window_info.title, w->title,
		    sizeof(ev.u.window_info.title));
		client_send(srv, to, &ev, -1);
	}
	event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
	client_send(srv, to, &ev, -1);
}
