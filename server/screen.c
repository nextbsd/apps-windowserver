/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The screen's usable area and screen changes (see "The screen and its
 * usable area" in WSProtocol.h).
 *
 * The usable area is the main display minus the menu bar strip along
 * its top (the Mac menu style) and the strip the Dock keeps on its edge:
 * what NSScreen's visibleFrame is on macOS. Every connection gets it in the
 * welcome and WS_EVENT_USABLE_AREA when it changes, and the server's
 * own things (minimized-window tiles, the overview) stay inside it, and
 * windows are pushed out from under the Dock and the menu bar strip when
 * it changes (clear.c).
 */

#include <errno.h>
#include <string.h>

#include "server.h"

/* The menu bar strip's height, at most a quarter of the main display. */
static uint32_t
menu_bar_height(struct ws_server *srv)
{
	uint32_t h = (uint32_t)display_main_frame(srv).height;

	return srv->menu_bar > h / 4 ? h / 4 : srv->menu_bar;
}

/* The usable area for the current screen, menu bar and Dock. */
void
screen_usable_compute(struct ws_server *srv, ws_rect_t *r)
{
	uint32_t t = srv->dock_thickness;
	uint32_t mb;

	*r = display_main_frame(srv);

	/*
	 * The menu bar strip along the top comes off first: a top Dock sits
	 * directly below it, and its thickness counts from the bar down.
	 * Checked against the screen when the app said so; the screen may
	 * have shrunk since. Never more than a quarter of it.
	 */
	mb = menu_bar_height(srv);
	r->y += (int32_t)mb;
	r->height -= (int32_t)mb;

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
		ev.u.usable_area.menu_bar = srv->usable_menu_bar;
		client_send(srv, c, &ev, -1);
	}
}

static void
usable_set(struct ws_server *srv, const ws_rect_t *r)
{
	ws_rect_t old = srv->usable;

	srv->usable = *r;
	srv->usable_menu_bar = menu_bar_height(srv);
	ws_info("usable area %d,%d %dx%d, menu bar %u", r->x, r->y, r->width,
	    r->height, srv->usable_menu_bar);
	send_usable(srv);
	window_tiles_place(srv);
	tiling_changed(srv);		/* tiling lays out again */
	windows_keep_clear(srv, &old);	/* the windows that float move */
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
	if (rects_equal(&r, &srv->usable) &&
	    menu_bar_height(srv) == srv->usable_menu_bar)
		return;
	usable_set(srv, &r);
}

/* ---- the menu bar strip ---- */

/*
 * Work out which app's report sets the menu bar strip (see "The menu bar
 * strip" in WSProtocol.h): the active app's, if it has said; else the
 * app that set it last, while it's still there; else none. If the strip
 * changed, the usable area follows.
 */
void
menu_bar_update(struct ws_server *srv)
{
	struct ws_client *a = srv->active, *owner = srv->menu_bar_app;
	uint32_t height = 0;

	if (a != NULL && !a->dead && a->menu_bar >= 0)
		owner = a;
	else if (owner != NULL && (owner->dead || owner->menu_bar < 0))
		owner = NULL;
	if (owner != NULL)
		height = (uint32_t)owner->menu_bar;
	srv->menu_bar_app = owner;
	if (height == srv->menu_bar)
		return;
	srv->menu_bar = height;
	if (owner != NULL)
		ws_info("menu bar strip %u pixels (app %u, %s)", height,
		    owner->id, owner->app_name);
	else
		ws_info("no menu bar strip");
	screen_usable_update(srv);
}

/* WS_MSG_SET_MENU_BAR from app c (a client->app): its bar is `height`
 * pixels, 0 for none. Returns 0 or an errno. */
int
menu_bar_set(struct ws_server *srv, struct ws_client *c, uint32_t height)
{
	if (height > (uint32_t)display_main_frame(srv).height / 4)
		return EINVAL;
	c->menu_bar = (int32_t)height;
	menu_bar_update(srv);
	return 0;
}

/*
 * App c is going away. If another connection carries the app on
 * (`heir`, see app_member_gone), the app's report goes with it;
 * otherwise a strip c set goes away, unless the active app has said.
 */
void
menu_bar_app_gone(struct ws_server *srv, struct ws_client *c,
    struct ws_client *heir)
{
	if (heir != NULL) {
		heir->menu_bar = c->menu_bar;
		if (srv->menu_bar_app == c)
			srv->menu_bar_app = heir;
		return;
	}
	if (srv->menu_bar_app == c)
		srv->menu_bar_app = NULL;
	menu_bar_update(srv);
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
	display_clamp(srv, &srv->ptr_x, &srv->ptr_y);
	damage_all(srv);
	cursor_update(srv);
	cursor_moved(srv);
	pointer_recheck(srv);
	dock_list_changed(srv);
}

/*
 * The displays changed but the desktop kept its size and scale (one
 * moved, a mirror came or went, the main display changed): the usable
 * area may have, and everything is drawn again.
 */
void
screen_displays_changed(struct ws_server *srv)
{
	ws_rect_t r;

	screen_usable_compute(srv, &r);
	if (memcmp(&r, &srv->usable, sizeof(r)) != 0 ||
	    menu_bar_height(srv) != srv->usable_menu_bar)
		usable_set(srv, &r);
	display_clamp(srv, &srv->ptr_x, &srv->ptr_y);
	damage_all(srv);
	cursor_update(srv);
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
	struct ws_display *d = srv->main;
	uint32_t ow, oh;
	int e;

	if (!srv->headless || d == NULL)
		return ENOTSUP;
	if (width < 64 || height < 64 || width > 16384 || height > 16384)
		return EINVAL;
	ow = d->panel_width;
	oh = d->panel_height;
	if (width == ow && height == oh)
		return 0;
	/* the main display's pixels; the points follow its mode */
	d->panel_width = width;
	d->panel_height = height;
	e = display_apply(srv, true);
	if (e != 0) {
		d->panel_width = ow;
		d->panel_height = oh;
		(void)display_apply(srv, true);
		return e;
	}
	ws_info("screen changed: %ux%u -> %ux%u pixels", ow, oh, width,
	    height);
	return 0;
}
