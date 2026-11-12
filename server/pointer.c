/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pointer routing: hover, implicit grab while a button is held, click
 * activation and click counting. Tiles of minimized windows are the
 * server's own: they get no events, and tile.c handles their clicks.
 */

#include <math.h>
#include <string.h>

#include "server.h"

#define DOUBLE_CLICK_USEC	500000u
#define DOUBLE_CLICK_DIST	4.0

static void
send_pointer(struct ws_server *srv, struct ws_window *w, uint16_t type,
    uint32_t button, uint32_t click_count)
{
	ws_event_t ev;

	if (w == NULL || w->tile_for != NULL)
		return;
	event_init(srv, &ev, type, sizeof(ev.u.pointer), w->id);
	ev.u.pointer.x = srv->ptr_x - w->frame.x;
	ev.u.pointer.y = srv->ptr_y - w->frame.y;
	ev.u.pointer.screen_x = srv->ptr_x;
	ev.u.pointer.screen_y = srv->ptr_y;
	ev.u.pointer.button = button;
	ev.u.pointer.click_count = click_count;
	ev.u.pointer.buttons = srv->buttons;
	ev.u.pointer.modifiers = srv->modifiers;
	client_send(srv, w->owner, &ev, -1);
}

/* Recompute the window under the pointer (not while a button is held). */
void
pointer_recheck(struct ws_server *srv)
{
	struct ws_window *w;

	if (srv->grab != NULL)
		return;
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
	srv->ptr_x = x;
	srv->ptr_y = y;
	cursor_moved(srv);
	if (srv->grab != NULL) {
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
	if (srv->hover != NULL)
		send_pointer(srv, srv->hover, WS_EVENT_MOUSE_MOVED, 0, 0);
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
	mask = 1u << button;
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
				if (c != srv->active &&
				    (w->flags & WS_WINDOW_NO_ACTIVATE) == 0 &&
				    (c->flags & WS_CONNECT_CONTROL) == 0) {
					app_activate(srv, c,
					    (w->flags & WS_WINDOW_NO_KEY) ? NULL : w);
					activated = true;
				} else if (c == srv->active) {
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
		srv->grab = NULL;
		srv->grab_suppressed = false;
		srv->tile_dragging = false;
		pointer_recheck(srv);
	}
}

void
pointer_scroll(struct ws_server *srv, double dx, double dy, int32_t v120_x,
    int32_t v120_y, uint32_t source)
{
	struct ws_window *w;
	ws_event_t ev;

	w = srv->grab != NULL ? srv->grab : window_at(srv, srv->ptr_x,
	    srv->ptr_y);
	if (w == NULL || w->tile_for != NULL)
		return;
	if (srv->cfg.natural_scrolling) {
		dx = -dx;
		dy = -dy;
		v120_x = -v120_x;
		v120_y = -v120_y;
	}
	event_init(srv, &ev, WS_EVENT_SCROLL, sizeof(ev.u.scroll), w->id);
	ev.u.scroll.x = srv->ptr_x - w->frame.x;
	ev.u.scroll.y = srv->ptr_y - w->frame.y;
	ev.u.scroll.dx = dx;
	ev.u.scroll.dy = dy;
	ev.u.scroll.v120_x = v120_x;
	ev.u.scroll.v120_y = v120_y;
	ev.u.scroll.source = source;
	ev.u.scroll.modifiers = srv->modifiers;
	client_send(srv, w->owner, &ev, -1);
}
