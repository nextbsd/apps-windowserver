/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The screen's usable area and screen changes (see "The screen and its
 * usable area" in WSProtocol.h).
 *
 * The usable area is the screen minus the strip the Dock keeps on its
 * edge: what NSScreen's visibleFrame is on macOS. Every connection gets
 * it in the welcome and WS_EVENT_USABLE_AREA when it changes, and the
 * server's own things (minimized-window tiles, the overview) stay
 * inside it.
 */

#include <errno.h>
#include <string.h>

#include "server.h"

/* The usable area for the current screen and Dock. */
void
screen_usable_compute(struct ws_server *srv, ws_rect_t *r)
{
	uint32_t t = srv->dock_thickness;

	r->x = 0;
	r->y = 0;
	r->width = (int32_t)srv->width;
	r->height = (int32_t)srv->height;

	/*
	 * A menu bar strip along the top (the Mac menu style, E36) will be
	 * taken off here, before the Dock: a top Dock then sits below it.
	 */

	/* The Dock was checked against the screen when it said so; the
	 * screen may have shrunk since. Never more than half of it. */
	switch (srv->dock_edge) {
	case WS_DOCK_EDGE_LEFT:
	case WS_DOCK_EDGE_RIGHT:
		if (t > (uint32_t)r->width / 2)
			t = (uint32_t)r->width / 2;
		r->width -= (int32_t)t;
		if (srv->dock_edge == WS_DOCK_EDGE_LEFT)
			r->x += (int32_t)t;
		break;
	case WS_DOCK_EDGE_TOP:
	case WS_DOCK_EDGE_BOTTOM:
		if (t > (uint32_t)r->height / 2)
			t = (uint32_t)r->height / 2;
		r->height -= (int32_t)t;
		if (srv->dock_edge == WS_DOCK_EDGE_TOP)
			r->y += (int32_t)t;
		break;
	default:
		break;
	}
}

static bool
rects_equal(const ws_rect_t *a, const ws_rect_t *b)
{
	return a->x == b->x && a->y == b->y && a->width == b->width &&
	    a->height == b->height;
}

/* Tell every connection the usable area. */
static void
send_usable(struct ws_server *srv)
{
	struct ws_client *c;
	ws_event_t ev;

	for (c = srv->clients; c != NULL; c = c->next) {
		event_init(srv, &ev, WS_EVENT_USABLE_AREA,
		    sizeof(ev.u.usable_area), 0);
		ev.u.usable_area.screen_id = 1;
		ev.u.usable_area.usable = srv->usable;
		client_send(srv, c, &ev, -1);
	}
}

static void
usable_set(struct ws_server *srv, const ws_rect_t *r)
{
	srv->usable = *r;
	ws_info("usable area %d,%d %dx%d", r->x, r->y, r->width, r->height);
	send_usable(srv);
	window_tiles_place(srv);
	srv->ov.dirty = true;		/* the overview lays out again */
}

/*
 * The Dock's edge or thickness changed, or the Dock connected or went
 * away: work the usable area out again, and if it changed, tell every
 * connection and put the tiles inside it.
 */
void
screen_usable_update(struct ws_server *srv)
{
	ws_rect_t r;

	screen_usable_compute(srv, &r);
	if (rects_equal(&r, &srv->usable))
		return;
	usable_set(srv, &r);
}

/*
 * The screen's size or mode changed (srv->width, ->height and the
 * shadow image already follow it): tell every connection, then the new
 * usable area, and place the tiles again. The caller then keeps the
 * windows on the screen (windows_keep_on_screen), so their owners hear
 * about the screen before their windows move.
 */
void
screen_changed(struct ws_server *srv)
{
	struct ws_client *c;
	ws_event_t ev;
	ws_rect_t r;

	for (c = srv->clients; c != NULL; c = c->next) {
		event_init(srv, &ev, WS_EVENT_SCREEN_CHANGED,
		    sizeof(ev.u.screen), 0);
		screen_info(srv, &ev.u.screen);
		client_send(srv, c, &ev, -1);
	}
	screen_usable_compute(srv, &r);
	usable_set(srv, &r);
	if (srv->ptr_x > srv->width - 1)
		srv->ptr_x = srv->width - 1;
	if (srv->ptr_y > srv->height - 1)
		srv->ptr_y = srv->height - 1;
	damage_all(srv);
	cursor_moved(srv);
	pointer_recheck(srv);
	dock_list_changed(srv);
}

/*
 * Change the screen's size: headless servers only (WS_INJECT_SCREEN_SIZE,
 * for tests). Returns 0 or an errno.
 */
int
screen_resize(struct ws_server *srv, uint32_t width, uint32_t height)
{
	uint32_t ow = srv->width, oh = srv->height;

	if (!srv->headless)
		return ENOTSUP;
	if (width < 64 || height < 64 || width > 16384 || height > 16384)
		return EINVAL;
	if (width == srv->width && height == srv->height)
		return 0;
	if (compositor_resize(srv, width, height) < 0)
		return ENOMEM;
	ws_info("screen changed: %ux%u -> %ux%u", ow, oh, width, height);
	screen_changed(srv);
	windows_keep_on_screen(srv, ow, oh);
	return 0;
}
