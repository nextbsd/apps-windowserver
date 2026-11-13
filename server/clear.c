/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Keeping windows clear of the Dock and the menu bar strip
 * (apps-windowserver#42): the stacking-mode counterpart of tiling
 * (tiling.c), using the same usable area (screen.c).
 *
 * When the usable area changes (the Dock moves to another edge or gets
 * thicker, a Mac menu bar appears, the screen changes), each window that
 * was inside it on a side that now comes in is pushed back inside on
 * that side, moving as little as possible, and shrunk only if it's
 * larger than the usable area (never below its minimum size, never if
 * it's fixed-size). A window's top edge is always kept inside, so its
 * title bar can be reached. A window that was already past the old edge
 * on a side, where the user dragged it partly under the Dock or off the
 * screen, is left alone on that side.
 *
 * A pushed window remembers its frame from before (clear_saved) and the
 * usable area it had then. On each later change it's placed from that
 * frame again, so once the Dock moves away it gets it back, unless its
 * frame changed meanwhile other than by its owner settling the frame it
 * was given (the user moved or resized it, or tiling took it): then it
 * forgets, and counts as any other window.
 *
 * A window shown for the first time that's on the screen but under the
 * Dock or the menu bar strip is placed inside the usable area the same
 * way. After that, dragging a window partly under the Dock is the
 * user's choice: nothing moves it until the usable area changes again.
 *
 * Owners hear the new frames as WS_EVENT_WINDOW_FRAME
 * (window_server_frame), as for any frame the server sets, so GNUstep
 * and X11 (Xnextbsd) windows alike follow. Tiled windows (tiling lays
 * them out), minimized and hidden windows, the Dock's windows,
 * desktop-level windows (InnerSpace), app icons, pop-up menus and
 * tooltips, the Mac menu bar itself, and tools' windows are left alone.
 */

#include "server.h"

/* An owner's frames this soon after a push are it settling the frame it
 * was given (a terminal keeping to whole character cells), not the user
 * moving the window. */
#define SETTLE_USEC	1000000

static bool
rect_eq(const ws_rect_t *a, const ws_rect_t *b)
{
	return a->x == b->x && a->y == b->y && a->width == b->width &&
	    a->height == b->height;
}

static bool
rect_overlaps(const ws_rect_t *a, const ws_rect_t *b)
{
	return a->x < b->x + b->width && b->x < a->x + a->width &&
	    a->y < b->y + b->height && b->y < a->y + a->height;
}

/*
 * May w be pushed? `shown`: it's being shown for the first time, when
 * submenus (on the same level as torn-off menus) are left where their
 * menu opens them.
 */
static bool
clear_candidate(struct ws_server *srv, const struct ws_window *w, bool shown)
{
	const struct ws_client *c = w->owner;
	ws_rect_t scr = { 0, 0, (int32_t)srv->width, (int32_t)srv->height };

	if (!w->visible || w->minimized || w->tiled || w->tile_for != NULL ||
	    w->frame.width <= 0 || w->frame.height <= 0)
		return false;
	if ((w->flags & (WS_WINDOW_ICON | WS_WINDOW_IGNORES_MOUSE)) != 0)
		return false;
	/* ordinary windows, torn-off menus, the NeXT main menu, panels;
	   not desktop windows, the Dock's level, pop-up menus and above */
	if (w->level < WS_LEVEL_NORMAL || w->level > WS_LEVEL_MODAL_PANEL ||
	    w->level == WS_LEVEL_STATUS)
		return false;
	if (shown && w->level == WS_LEVEL_SUBMENU)
		return false;
	if (c->dead || c->doomed || (c->flags & WS_CONNECT_TOOL) != 0 ||
	    dock_owns_app(srv, c->app))
		return false;
	/* an app's Mac-style menu bar sits in the strip it reserves */
	if (w->level == WS_LEVEL_MAIN_MENU && c->app->menu_bar > 0)
		return false;
	/* the user is dragging it */
	if (srv->buttons != 0 && srv->grab == w)
		return false;
	/* windows kept off the screen (an app's offscreen helpers) */
	return rect_overlaps(&w->frame, &scr);
}

/*
 * Fit one axis: the window at *pos, *len, inside lo..hi, on the sides
 * where it was inside old_lo..old_hi (`keep_lo` always on the low side,
 * the top edge). min: the smallest length; can_shrink: false for
 * fixed-size windows.
 */
static void
fit_axis(int32_t *pos, int32_t *len, int32_t lo, int32_t hi, int32_t old_lo,
    int32_t old_hi, bool keep_lo, int32_t min, bool can_shrink)
{
	bool low = keep_lo || *pos >= old_lo;
	bool high = *pos + *len <= old_hi;

	if (low && high) {
		if (*len > hi - lo) {
			if (can_shrink)
				*len = hi - lo > min ? hi - lo : min;
			*pos = lo;
			return;
		}
		if (*pos + *len > hi)
			*pos = hi - *len;
		if (*pos < lo)
			*pos = lo;
	} else if (low) {
		if (*pos < lo)
			*pos = lo;
	} else if (high) {
		if (*pos + *len > hi)
			*pos = hi - *len;
	}
}

/* frame fitted into the usable area u, from where it was inside old. */
static ws_rect_t
fit(const struct ws_window *w, const ws_rect_t *frame, const ws_rect_t *old,
    const ws_rect_t *u)
{
	ws_rect_t r = *frame;
	bool shrink = (w->flags & WS_WINDOW_FIXED_SIZE) == 0;

	if (u->width <= 0 || u->height <= 0)
		return r;
	fit_axis(&r.x, &r.width, u->x, u->x + u->width, old->x,
	    old->x + old->width, false, (int32_t)w->min_width, shrink);
	fit_axis(&r.y, &r.height, u->y, u->y + u->height, old->y,
	    old->y + old->height, true, (int32_t)w->min_height, shrink);
	return r;
}

static void
push(struct ws_server *srv, struct ws_window *w, const ws_rect_t *r)
{
	ws_debug("window %u (%s) kept clear: %d,%d %dx%d -> %d,%d %dx%d",
	    w->id, w->owner->app_name, w->frame.x, w->frame.y, w->frame.width,
	    w->frame.height, r->x, r->y, r->width, r->height);
	window_server_frame(srv, w, r);
	w->clear_given = w->frame;
	w->clear_usec = ws_now_usec();
}

/*
 * The usable area changed from *old to srv->usable: push the windows
 * out from under the Dock and the menu bar strip, and give the windows
 * pushed before back their frames where they fit again.
 */
void
windows_keep_clear(struct ws_server *srv, const ws_rect_t *old)
{
	const ws_rect_t *u = &srv->usable;
	struct ws_window *w;
	ws_rect_t r;

	for (w = srv->windows; w != NULL; w = w->next) {
		if (w->clear_pushed && !rect_eq(&w->frame, &w->clear_given)) {
			/* moved or resized since: its frame now counts */
			w->clear_pushed = false;
			ws_debug("window %u: moved since it was pushed, "
			    "its frame from before forgotten", w->id);
		}
		if (!clear_candidate(srv, w, false)) {
			if (w->tiled)
				w->clear_pushed = false;
			continue;
		}
		if (w->clear_pushed) {
			r = fit(w, &w->clear_saved, &w->clear_usable, u);
			if (rect_eq(&r, &w->clear_saved)) {
				w->clear_pushed = false;
				ws_debug("window %u (%s) back to %d,%d %dx%d",
				    w->id, w->owner->app_name, r.x, r.y,
				    r.width, r.height);
				if (!rect_eq(&r, &w->frame))
					window_server_frame(srv, w, &r);
			} else if (!rect_eq(&r, &w->frame))
				push(srv, w, &r);
			continue;
		}
		r = fit(w, &w->frame, old, u);
		if (rect_eq(&r, &w->frame))
			continue;
		w->clear_saved = w->frame;
		w->clear_usable = *old;
		w->clear_pushed = true;
		push(srv, w, &r);
	}
}

/*
 * w is being shown for the first time: if it's on the screen but under
 * the Dock or the menu bar strip, place it inside the usable area. It
 * remembers no frame: there's none to give back.
 */
void
window_place_clear(struct ws_server *srv, struct ws_window *w)
{
	ws_rect_t scr = { 0, 0, (int32_t)srv->width, (int32_t)srv->height };
	ws_rect_t r;

	if (w->clear_shown)
		return;
	w->clear_shown = true;
	if (!clear_candidate(srv, w, true))
		return;
	r = fit(w, &w->frame, &scr, &srv->usable);
	if (rect_eq(&r, &w->frame))
		return;
	ws_debug("window %u (%s) opened under the Dock: %d,%d %dx%d -> "
	    "%d,%d %dx%d", w->id, w->owner->app_name, w->frame.x, w->frame.y,
	    w->frame.width, w->frame.height, r.x, r.y, r.width, r.height);
	window_server_frame(srv, w, &r);
}

/*
 * w's owner gave it a frame. Just after a push that's it settling the
 * frame it was given, which still counts as the pushed one.
 */
void
window_clear_owner_frame(struct ws_server *srv, struct ws_window *w)
{
	(void)srv;
	if (w->clear_pushed && ws_now_usec() - w->clear_usec < SETTLE_USEC)
		w->clear_given = w->frame;
}
