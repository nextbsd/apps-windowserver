/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pointer routing: hover, implicit grab while a button is held, click
 * activation and click counting. Tiles of minimized windows are the
 * server's own: they get no events, and tile.c handles their clicks.
 *
 * A connection may also grab the pointer (WS_MSG_POINTER_GRAB, see
 * "Pointer grabs" in WSProtocol.h): with WS_GRAB_POINTER every event
 * goes to it, hover only ever names one of its windows, and a press
 * activates nothing. Pointer tracking (WS_MSG_POINTER_TRACK) sends the
 * active app's tracking connections motion over anything else too.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include "server.h"

#define DOUBLE_CLICK_USEC	500000u
#define DOUBLE_CLICK_DIST	4.0

/*
 * A pointer event about window w to its owner, or, with w NULL, to
 * connection `to` about none of its windows (hdr.window 0, screen
 * position).
 */
static void
send_pointer_to(struct ws_server *srv, struct ws_client *to,
    struct ws_window *w, uint16_t type, uint32_t button, uint32_t click_count)
{
	ws_event_t ev;

	if (w != NULL) {
		if (w->tile_for != NULL)
			return;
		to = w->owner;
	}
	if (to == NULL || to->dead)
		return;
	event_init(srv, &ev, type, sizeof(ev.u.pointer), w != NULL ? w->id : 0);
	ev.u.pointer.x = srv->ptr_x - (w != NULL ? w->frame.x : 0);
	ev.u.pointer.y = srv->ptr_y - (w != NULL ? w->frame.y : 0);
	ev.u.pointer.screen_x = srv->ptr_x;
	ev.u.pointer.screen_y = srv->ptr_y;
	ev.u.pointer.button = button;
	ev.u.pointer.click_count = click_count;
	ev.u.pointer.buttons = srv->buttons;
	ev.u.pointer.modifiers = srv->modifiers;
	client_send(srv, to, &ev, -1);
}

static void
send_pointer(struct ws_server *srv, struct ws_window *w, uint16_t type,
    uint32_t button, uint32_t click_count)
{
	if (w != NULL)
		send_pointer_to(srv, NULL, w, type, button, click_count);
}

/* With WS_GRAB_POINTER: everything goes to the grabbing connection. */
static bool
grabbed(struct ws_server *srv)
{
	return srv->pgrab != NULL && srv->pgrab_mode == WS_GRAB_POINTER;
}

/*
 * Motion with hdr.window 0 to the active app's tracking connections
 * (WS_MSG_POINTER_TRACK) that aren't getting this motion already: the
 * pointer is over none of their windows.
 */
static void
send_tracking(struct ws_server *srv, struct ws_window *target)
{
	struct ws_client *c;

	if (srv->active == NULL && srv->dock_front == NULL)
		return;
	for (c = srv->clients; c != NULL; c = c->next)
		if (c->pointer_track && !c->dead && app_has_focus(srv, c->app) &&
		    (target == NULL || target->tile_for != NULL ||
		    target->owner != c))
			send_pointer_to(srv, c, NULL, WS_EVENT_MOUSE_MOVED, 0, 0);
}

/*
 * Recompute the window under the pointer (not while a button is held,
 * unless the pointer is grabbed: then it's the window under it if that
 * is one of the grabbing connection's, else none).
 */
void
pointer_recheck(struct ws_server *srv)
{
	struct ws_window *w;

	if (srv->grab != NULL && !grabbed(srv))
		return;
	/* While a drag lasts nobody hovers (drag.c). */
	if (drag_active(srv))
		return;
	/* While the overview is open, no window is under the pointer. */
	if (overview_active(srv))
		w = NULL;
	else if (grabbed(srv))
		w = window_at_owned(srv, srv->pgrab, srv->ptr_x, srv->ptr_y);
	else
		w = window_at(srv, srv->ptr_x, srv->ptr_y);
	if (w == srv->hover)
		return;
	if (srv->hover != NULL)
		send_pointer(srv, srv->hover, WS_EVENT_MOUSE_EXITED, 0, 0);
	srv->hover = w;
	if (w != NULL)
		send_pointer(srv, w, WS_EVENT_MOUSE_ENTERED, 0, 0);
	cursor_update(srv);
}

void
pointer_window_gone(struct ws_server *srv, struct ws_window *w)
{
	if (srv->grab == w) {
		srv->grab = NULL;
		srv->grab_suppressed = false;
	}
	if (srv->hover == w)
		srv->hover = NULL;
	pointer_recheck(srv);
}

/*
 * End c's pointer grab, if it holds it. notify: the server ends it, not
 * c (WS_EVENT_POINTER_UNGRABBED).
 */
void
pointer_ungrab(struct ws_server *srv, struct ws_client *c, bool notify)
{
	ws_event_t ev;
	bool was_grabbed;

	if (c == NULL || srv->pgrab != c)
		return;
	was_grabbed = grabbed(srv);
	srv->pgrab = NULL;
	srv->pgrab_mode = 0;
	ws_debug("client %u's pointer grab ended%s", c->id,
	    notify ? " by the server" : "");
	if (notify && !c->dead) {
		event_init(srv, &ev, WS_EVENT_POINTER_UNGRABBED, 0, 0);
		client_send(srv, c, &ev, -1);
	}
	if (!was_grabbed)
		return;
	/* Buttons still held: their release goes where the implicit grab
	 * would have it, the window under the pointer now. */
	if (srv->buttons != 0) {
		srv->grab = srv->hover;
		srv->grab_suppressed = false;
	}
	pointer_recheck(srv);
	cursor_update(srv);
}

/* WS_MSG_POINTER_GRAB: only the active app, one connection at a time. */
int
pointer_grab(struct ws_server *srv, struct ws_client *c, uint32_t mode)
{
	if (mode != WS_GRAB_POINTER && mode != WS_GRAB_OUTSIDE)
		return EINVAL;
	if ((c->flags & WS_CONNECT_TOOL) || !app_has_focus(srv, c->app) ||
	    overview_active(srv))
		return EPERM;
	if (srv->pgrab != NULL && srv->pgrab != c)
		return EBUSY;
	srv->pgrab = c;
	srv->pgrab_mode = mode;
	ws_debug("client %u grabbed the pointer (%s)", c->id,
	    mode == WS_GRAB_POINTER ? "all events" : "outside presses");
	if (mode == WS_GRAB_POINTER) {
		/* From now on only c's windows are under the pointer. */
		if (srv->hover != NULL && srv->hover->owner != c) {
			send_pointer(srv, srv->hover, WS_EVENT_MOUSE_EXITED, 0, 0);
			srv->hover = NULL;
		}
		pointer_recheck(srv);
		cursor_update(srv);
	}
	return 0;
}

/*
 * WS_MSG_WARP_POINTER: the active app, while the pointer is over one of
 * its windows (or a button pressed in one is held), or the connection
 * holding the pointer grab. (x, y) is local to w, or the screen's.
 */
int
pointer_warp(struct ws_server *srv, struct ws_client *c, struct ws_window *w,
    double x, double y)
{
	struct ws_window *t = srv->grab != NULL ? srv->grab : srv->hover;

	if ((c->flags & WS_CONNECT_TOOL) || !app_has_focus(srv, c->app) ||
	    overview_active(srv))
		return EPERM;
	if (srv->pgrab != c && (t == NULL || t->tile_for != NULL ||
	    t->owner->app != c->app))
		return EPERM;
	if (!isfinite(x) || !isfinite(y))
		return EINVAL;
	if (w != NULL) {
		x += w->frame.x;
		y += w->frame.y;
	}
	pointer_motion(srv, x, y);
	return 0;
}

/* WS_MSG_QUERY_POINTER: the active app, or a connection under the pointer. */
int
pointer_query(struct ws_server *srv, struct ws_client *c,
    ws_pointer_info_t *info)
{
	struct ws_window *w;

	if (c->flags & WS_CONNECT_TOOL)
		return EPERM;
	w = overview_active(srv) ? NULL :
	    window_at_owned(srv, c, srv->ptr_x, srv->ptr_y);
	if (!app_has_focus(srv, c->app) && w == NULL)
		return EPERM;
	memset(info, 0, sizeof(*info));
	info->x = srv->ptr_x;
	info->y = srv->ptr_y;
	info->buttons = srv->buttons;
	info->modifiers = srv->modifiers;
	info->window = w != NULL ? w->id : 0;
	return 0;
}

void
pointer_motion(struct ws_server *srv, double x, double y)
{
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;
	if (x > srv->width - 1)
		x = srv->width - 1;
	if (y > srv->height - 1)
		y = srv->height - 1;
	/* A finger on the touchpad again: momentum scrolling stops. */
	scroll_touch(srv);
	srv->ptr_x = x;
	srv->ptr_y = y;
	cursor_moved(srv);
	/* A drag moves its image and finds the window under it (drag.c). */
	if (drag_motion(srv))
		return;
	if (overview_active(srv) && srv->grab == NULL) {
		overview_motion(srv);
		return;
	}
	if (grabbed(srv)) {
		pointer_recheck(srv);
		send_pointer_to(srv, srv->pgrab, srv->hover, srv->buttons != 0 ?
		    WS_EVENT_MOUSE_DRAGGED : WS_EVENT_MOUSE_MOVED, 0, 0);
		return;
	}
	/* Dragging a split between tiles, or over one (tiling.c). */
	if (tiling_motion(srv))
		return;
	if (srv->grab != NULL) {
		send_tracking(srv, srv->grab);
		if (srv->grab->tile_for != NULL) {
			if (srv->buttons & (1u << WS_BUTTON_LEFT))
				tile_drag(srv, srv->grab);
			return;
		}
		if (!srv->grab_suppressed)
			send_pointer(srv, srv->grab, WS_EVENT_MOUSE_DRAGGED, 0, 0);
		return;
	}
	pointer_recheck(srv);
	/* Tiling: the tile under the pointer may take the focus. */
	tiling_pointer_moved(srv);
	if (srv->hover != NULL)
		send_pointer(srv, srv->hover, WS_EVENT_MOUSE_MOVED, 0, 0);
	send_tracking(srv, srv->hover);
}

static uint32_t
click_count(struct ws_server *srv, uint32_t button)
{
	uint64_t now = ws_now_usec();

	if (button == srv->last_click_button &&
	    now - srv->last_click_usec < DOUBLE_CLICK_USEC &&
	    fabs(srv->ptr_x - srv->last_click_x) <= DOUBLE_CLICK_DIST &&
	    fabs(srv->ptr_y - srv->last_click_y) <= DOUBLE_CLICK_DIST)
		srv->click_count++;
	else
		srv->click_count = 1;
	srv->last_click_usec = now;
	srv->last_click_button = button;
	srv->last_click_x = srv->ptr_x;
	srv->last_click_y = srv->ptr_y;
	return srv->click_count;
}

/*
 * The first button press picks the target window and starts an
 * implicit grab. A click in an inactive app's window activates that
 * app (deactivate to the old app first, then activate), raises the
 * window, makes it key, and then delivers the click unless
 * ClickThrough is off.
 */
void
pointer_button(struct ws_server *srv, uint32_t button, bool pressed)
{
	struct ws_window *w;
	struct ws_client *c;
	uint32_t mask, count;
	bool activated = false;

	if (button >= 32)
		return;
	scroll_touch(srv);
	srv->start_tap = 0;	/* a click: Start isn't tapped on its own */
	/* Presses during a drag, and a cancelled drag's release (drag.c). */
	if (drag_button(srv, button, pressed))
		return;
	mask = 1u << button;
	/* The overview takes the buttons pressed while it is open, up to
	 * their release (even if it closed meanwhile). */
	if (pressed && overview_active(srv) && (srv->buttons & mask) == 0) {
		if ((srv->ov.buttons & mask) == 0) {
			srv->ov.buttons |= mask;
			overview_button(srv, button);
		}
		return;
	}
	if (!pressed && (srv->ov.buttons & mask)) {
		srv->ov.buttons &= ~mask;
		return;
	}
	/* A popup grab hears about a press outside its windows, and ends. */
	if (pressed && srv->buttons == 0 && srv->pgrab != NULL &&
	    srv->pgrab_mode == WS_GRAB_OUTSIDE && !overview_active(srv) &&
	    window_at_owned(srv, srv->pgrab, srv->ptr_x, srv->ptr_y) == NULL) {
		c = srv->pgrab;
		pointer_ungrab(srv, c, false);
		send_pointer_to(srv, c, NULL, WS_EVENT_POINTER_OUTSIDE, button, 0);
	}
	if (grabbed(srv)) {
		/* No activation, raising or tiles: the grab gets it all. */
		if (pressed) {
			if (srv->buttons & mask)
				return;
			pointer_recheck(srv);
			if (srv->hover == NULL)
				send_pointer_to(srv, srv->pgrab, NULL,
				    WS_EVENT_POINTER_OUTSIDE, button, 0);
			srv->buttons |= mask;
			count = click_count(srv, button);
			send_pointer_to(srv, srv->pgrab, srv->hover,
			    WS_EVENT_MOUSE_DOWN, button, count);
			return;
		}
		if ((srv->buttons & mask) == 0)
			return;
		srv->buttons &= ~mask;
		send_pointer_to(srv, srv->pgrab, srv->hover, WS_EVENT_MOUSE_UP,
		    button, srv->click_count);
		if (srv->buttons == 0) {
			srv->grab = NULL;
			srv->grab_suppressed = false;
			srv->tile_dragging = false;
		}
		return;
	}
	/* A press on a split between tiles takes it, up to its release. */
	if (pressed ? srv->buttons == 0 && tiling_press(srv, button) :
	    tiling_release(srv, button))
		return;
	if (pressed) {
		if (srv->buttons & mask)
			return;
		if (srv->buttons == 0) {
			pointer_recheck(srv);
			w = window_at(srv, srv->ptr_x, srv->ptr_y);
			srv->grab = w;
			srv->grab_suppressed = false;
			if (w == NULL || w->tile_for == NULL)
				tile_select(srv, NULL);
			if (w != NULL && w->tile_for == NULL) {
				c = w->owner->app;
				/* A click outside the Dock while the user
				   worked in it gives input back. */
				if (srv->dock_front != NULL &&
				    c != srv->dock_front &&
				    (w->flags & WS_WINDOW_NO_ACTIVATE) == 0 &&
				    (c->flags & WS_CONNECT_TOOL) == 0)
					app_dock_front_end(srv, true);
				if (!app_has_focus(srv, c) &&
				    (w->flags & WS_WINDOW_NO_ACTIVATE) == 0 &&
				    (c->flags & WS_CONNECT_TOOL) == 0) {
					app_activate(srv, c,
					    (w->flags & WS_WINDOW_NO_KEY) ? NULL : w);
					/* The Dock's app isn't made
					   active: its click goes through. */
					activated = srv->active == c;
				} else if (app_has_focus(srv, c)) {
					window_make_key(srv, w);
				}
				if (srv->cfg.raise_on_click)
					window_order_by_server(srv, w,
					    WS_ORDER_ABOVE, NULL);
				if (activated && !srv->cfg.click_through)
					srv->grab_suppressed = true;
			}
		}
		srv->buttons |= mask;
		count = click_count(srv, button);
		if (srv->grab != NULL && srv->grab->tile_for != NULL) {
			tile_press(srv, srv->grab, button, count);
			return;
		}
		if (srv->grab != NULL && !srv->grab_suppressed)
			send_pointer(srv, srv->grab, WS_EVENT_MOUSE_DOWN, button,
			    count);
		return;
	}
	if ((srv->buttons & mask) == 0)
		return;
	srv->buttons &= ~mask;
	if (srv->grab != NULL && !srv->grab_suppressed)
		send_pointer(srv, srv->grab, WS_EVENT_MOUSE_UP, button,
		    srv->click_count);
	if (srv->buttons == 0) {
		w = srv->grab;
		/* A tiled window the user dragged goes to its place. */
		if (w != NULL)
			tiling_grab_end(srv, w);
		srv->grab = NULL;
		srv->grab_suppressed = false;
		srv->tile_dragging = false;
		/* A drag ends with the release (drag.c). */
		drag_note_release(srv, w);
		pointer_recheck(srv);
	}
}

/*
 * The window a scroll at the pointer goes to, or NULL (with the pointer
 * grabbed, NULL sends it to the grabbing connection about no window).
 */
struct ws_window *
pointer_scroll_target(struct ws_server *srv)
{
	struct ws_window *w;

	if (grabbed(srv))
		return window_at_owned(srv, srv->pgrab, srv->ptr_x, srv->ptr_y);
	w = srv->grab != NULL ? srv->grab : window_at(srv, srv->ptr_x,
	    srv->ptr_y);
	if (w == NULL || w->tile_for != NULL)
		return NULL;
	return w;
}

/*
 * Send a scroll event to w. ScrollSpeed scales the pixel deltas (not
 * the wheel's detents) and NaturalScrolling turns them around.
 */
void
pointer_scroll_send(struct ws_server *srv, struct ws_window *w, double dx,
    double dy, int32_t v120_x, int32_t v120_y, uint32_t source,
    uint32_t phase, uint32_t momentum_phase)
{
	struct ws_client *to = w != NULL ? w->owner :
	    grabbed(srv) ? srv->pgrab : NULL;
	ws_event_t ev;

	if (to == NULL || to->dead || overview_active(srv))
		return;
	dx *= srv->cfg.scroll_speed;
	dy *= srv->cfg.scroll_speed;
	if (srv->cfg.natural_scrolling) {
		dx = -dx;
		dy = -dy;
		v120_x = -v120_x;
		v120_y = -v120_y;
	}
	event_init(srv, &ev, WS_EVENT_SCROLL, sizeof(ev.u.scroll),
	    w != NULL ? w->id : 0);
	ev.u.scroll.x = srv->ptr_x - (w != NULL ? w->frame.x : 0);
	ev.u.scroll.y = srv->ptr_y - (w != NULL ? w->frame.y : 0);
	ev.u.scroll.dx = dx;
	ev.u.scroll.dy = dy;
	ev.u.scroll.v120_x = v120_x;
	ev.u.scroll.v120_y = v120_y;
	ev.u.scroll.source = source;
	ev.u.scroll.modifiers = srv->modifiers;
	ev.u.scroll.phase = phase;
	ev.u.scroll.momentum_phase = momentum_phase;
	client_send(srv, to, &ev, -1);
}

/* A wheel or other scroll without phases (touchpad scrolls: scroll.c). */
void
pointer_scroll(struct ws_server *srv, double dx, double dy, int32_t v120_x,
    int32_t v120_y, uint32_t source)
{
	scroll_touch(srv);
	pointer_scroll_send(srv, pointer_scroll_target(srv), dx, dy, v120_x,
	    v120_y, source, WS_SCROLL_PHASE_NONE, WS_SCROLL_PHASE_NONE);
}
