/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The screen's usable area and screen changes (see "The screen and its
 * usable area" in WSProtocol.h).
 *
 * The usable area is the main display minus the menu bar strip along
 * its top (the Mac menu style) and the strip the Dock keeps on its edge:
 * what NSScreen's visibleFrame is on macOS. Every other display of the
 * desktop has a menu bar strip of its own too (apps-windowserver#100),
 * and the Dock's strip when the Dock is on it: their usable areas go
 * out in the display list (ws_display_info_t.usable). Every connection gets it in the
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

static bool rects_equal(const ws_rect_t *a, const ws_rect_t *b);

/* The Dock's strip off r, its display's frame (or what's left of it).
   Never more than half of it. */
static void
dock_strip(struct ws_server *srv, ws_rect_t *r)
{
	uint32_t t = srv->dock_thickness;

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

/*
 * The usable area for the current screen, menu bar and Dock: the main
 * display's. The Dock's strip comes off it only when the Dock is on it
 * (display_dock); a left or right Dock with a display beyond that edge
 * is on that display instead.
 */
void
screen_usable_compute(struct ws_server *srv, ws_rect_t *r)
{
	struct ws_display *dd = display_dock(srv, srv->dock_edge);
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
	 * screen may have shrunk since. */
	if (dd == NULL || dd == srv->main)
		dock_strip(srv, r);
}

/*
 * A display that isn't the main one: its frame minus its own menu bar
 * strip (every display that's part of the desktop has one, as on
 * macOS; a mirror shows its source's) and the Dock's strip when the
 * Dock is on it.
 */
static void
other_usable_compute(struct ws_server *srv, const struct ws_display *d,
    ws_rect_t *r)
{
	struct ws_display *dd = display_dock(srv, srv->dock_edge);
	int32_t mb;

	*r = d->frame;
	if (!display_shows(d))
		return;
	mb = menubar_strip_height(srv, d);
	r->y += mb;
	r->height -= mb;
	if (dd == d)
		dock_strip(srv, r);
}

/* Any display's usable area (ws_display_info_t.usable). */
void
screen_usable_of(struct ws_server *srv, const struct ws_display *d,
    ws_rect_t *r)
{
	if (d == srv->main && srv->usable.width > 0)
		*r = srv->usable;
	else
		other_usable_compute(srv, d, r);
}

/*
 * The displays other than the main one: work their usable areas out
 * again (their menu bar strips, and the Dock when it is on one of
 * them), keep the windows on each clear of its strips, and if any
 * changed, tell everyone the displays changed (their usable areas are
 * in the display list).
 */
static void
dock_area_update(struct ws_server *srv)
{
	struct ws_display *d, *dd = display_dock(srv, srv->dock_edge);
	ws_rect_t r, old;
	uint32_t old_id = srv->dock_display;
	bool changed = false;
	unsigned i;

	/* the Dock's display, for the log and window_place_clear() */
	srv->dock_display = dd != NULL && dd != srv->main ? dd->id : 0;
	srv->dock_usable = (ws_rect_t){ 0, 0, 0, 0 };
	if (srv->dock_display != 0) {
		r = dd->frame;
		dock_strip(srv, &r);
		srv->dock_usable = r;
		if (old_id != srv->dock_display)
			ws_info("the Dock is on display %s", dd->connector);
	}
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (d == srv->main) {
			d->usable = srv->usable;
			continue;
		}
		other_usable_compute(srv, d, &r);
		old = d->usable.width > 0 ? d->usable : d->frame;
		if (d->usable.width > 0 && rects_equal(&r, &d->usable))
			continue;
		d->usable = r;
		changed = true;
		ws_info("display %s: usable area %d,%d %dx%d", d->connector,
		    r.x, r.y, r.width, r.height);
		if (display_shows(d)) {
			/* a display that moved: what was inside it before
			   counts from its new place */
			if (old.x < d->frame.x || old.y < d->frame.y ||
			    old.x + old.width > d->frame.x + d->frame.width ||
			    old.y + old.height > d->frame.y + d->frame.height)
				old = d->frame;
			windows_keep_clear(srv, &d->frame, &old, &r);
		}
	}
	if (changed && srv->started)
		display_changed_notify(srv);
}

static bool
rects_equal(const ws_rect_t *a, const ws_rect_t *b)
{
	return a->x == b->x && a->y == b->y && a->width == b->width &&
	    a->height == b->height;
}

/* Tell every connection the usable area (and the menu bar's height and
   the status items' width with it). */
void
screen_send_usable(struct ws_server *srv)
{
	struct ws_client *c;
	ws_event_t ev;

	for (c = srv->clients; c != NULL; c = c->next) {
		event_init(srv, &ev, WS_EVENT_USABLE_AREA,
		    sizeof(ev.u.usable_area), 0);
		ev.u.usable_area.screen_id = 1;
		ev.u.usable_area.usable = srv->usable;
		ev.u.usable_area.menu_bar = srv->usable_menu_bar;
		ev.u.usable_area.status_items = srv->status_items_width;
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
	screen_send_usable(srv);
	window_tiles_place(srv);
	tiling_changed(srv);		/* tiling lays out again */
	if (srv->main != NULL) {
		ws_rect_t m = display_main_frame(srv);

		/* the windows that float move */
		windows_keep_clear(srv, &m, &old, &srv->usable);
	}
	srv->ov.dirty = true;		/* the overview lays out again */
	menubar_update(srv);		/* the bar and the status items */
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
	if (!rects_equal(&r, &srv->usable) ||
	    menu_bar_height(srv) != srv->usable_menu_bar)
		usable_set(srv, &r);
	dock_area_update(srv);
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
	dock_area_update(srv);
	menubar_displays_changed(srv);	/* every display's strip */
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
	dock_area_update(srv);
	menubar_displays_changed(srv);	/* every display's strip */
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
