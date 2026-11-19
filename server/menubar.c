/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The menu bar, the way macOS builds it (apps-windowserver#97, "The menu
 * bar" in WSProtocol.h): nobody draws the whole bar. Each app draws its
 * own bar window, and the right end is a row of status items, small
 * windows from any process. WindowServer only gives them their levels
 * and places, and swaps the apps' bars when the active app changes, so
 * the strip reads as one surface.
 *
 * The bar window. An app names it with WS_MSG_SET_MENU_BAR (hdr.window).
 * From then on the server shows it while its app is active and hides it
 * otherwise, whatever the app orders: GNUstep orders its menu out when
 * the app deactivates and in when it activates again, and those come
 * from two processes at two moments, which left the strip empty (the
 * desktop showing through) for a frame or more on every switch. Here
 * both happen in menubar_update(), in one pass, before anything is
 * composited: the outgoing bar leaves the stack and the incoming one
 * enters it in the same frame.
 *
 * An incoming app with nothing to show yet (launching, busy, or an app
 * that never names a bar) leaves the outgoing bar where it is for
 * WS_MENU_BAR_HOLD_MS; after that the strip gets an empty bar, drawn
 * here in the colour of the last bar shown, never the desktop.
 *
 * Status items are packed right to left from the main display's right
 * edge, by priority (larger further right), then by age (older further
 * right). Their total width goes out with the usable area, so apps'
 * bars stop their menus short of it.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

#define BAR_DEFAULT_COLOR	0xf2f2f2

bool
menubar_managed(const struct ws_window *w)
{
	return w != NULL && (w->menu_bar_window || w->status_item);
}

/* The strip's height on the main display: the menu bar strip's. */
static int32_t
strip_height(struct ws_server *srv)
{
	return (int32_t)srv->usable_menu_bar;
}

static ws_rect_t
strip_rect(struct ws_server *srv, int32_t height)
{
	ws_rect_t m = display_main_frame(srv);

	m.height = height;
	return m;
}

static bool
rect_same(const ws_rect_t *a, const ws_rect_t *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

/* The colour at the right end of bar w's buffer: the theme's bar. */
static void
sample_color(struct ws_server *srv, struct ws_window *w)
{
	struct ws_buffer *b = w->buffer;
	const uint32_t *px;
	uint32_t x, y, p;

	if (b == NULL || b->image == NULL || b->width < 2 || b->height < 2)
		return;		/* a GPU buffer: keep the last colour */
	px = pixman_image_get_data(b->image);
	if (px == NULL)
		return;
	x = b->width - 2;
	y = b->height / 2;
	p = px[(size_t)y * (b->stride / 4) + x];
	if (b->format != WS_FORMAT_XRGB8888 && (p >> 24) < 0xf0)
		return;		/* see-through there: not the bar's colour */
	p &= 0xffffff;
	if (p != srv->bar_color)
		ws_debug("menu bar colour #%06x (window %u)", p, w->id);
	srv->bar_color = p;
}

/* ---- the bar window ---- */

/* w stops being the app's bar: back to what its owner asked for. */
static void
bar_release(struct ws_server *srv, struct ws_window *w)
{
	if (w == NULL || !w->menu_bar_window)
		return;
	if (srv->bar_shown == w) {
		sample_color(srv, w);
		srv->bar_shown = NULL;
	}
	window_stack_show(srv, w, false);
	w->menu_bar_window = false;
	if (w->owner->app->bar_window == w)
		w->owner->app->bar_window = NULL;
	w->level = w->client_level;
	if (w->client_visible)
		window_order(srv, w, WS_ORDER_ABOVE, NULL);
	ws_debug("window %u is no longer a menu bar", w->id);
}

/* Make w a window the server shows itself, keeping what its owner
   asked for. */
static void
take_over(struct ws_server *srv, struct ws_window *w, int32_t level)
{
	if (menubar_managed(w))
		return;
	w->client_visible = w->visible;
	w->client_level = w->level;
	/* not on screen until the server says so */
	window_stack_show(srv, w, false);
	w->level = level;
}

/*
 * WS_MSG_SET_MENU_BAR with a window: w (NULL: the app's current one) is
 * app a's bar, `height` points tall; 0 ends it. Returns 0 or an errno.
 */
int
menubar_set_window(struct ws_server *srv, struct ws_client *a,
    struct ws_window *w, uint32_t height)
{
	ws_rect_t r;
	int err;

	if (height > (uint32_t)display_main_frame(srv).height / 4)
		return EINVAL;
	if (height == 0) {
		bar_release(srv, a->bar_window);
		err = menu_bar_set(srv, a, 0);
		menubar_update(srv);
		return err;
	}
	if (w == NULL)
		w = a->bar_window;
	if (a->bar_window != NULL && a->bar_window != w)
		bar_release(srv, a->bar_window);
	if (w != NULL && w->status_item)
		return EINVAL;
	if (w != NULL && !w->menu_bar_window) {
		take_over(srv, w, WS_LEVEL_MENU_BAR);
		w->menu_bar_window = true;
		a->bar_window = w;
		ws_debug("window %u is app %u's (%s) menu bar", w->id, a->id,
		    a->app_name);
	}
	err = menu_bar_set(srv, a, height);
	if (w != NULL) {
		/* the strip along the main display's top */
		r = strip_rect(srv, (int32_t)height);
		if (!rect_same(&r, &w->frame))
			window_server_frame(srv, w, &r);
		if (w->buffer != NULL)
			sample_color(srv, w);
	}
	menubar_update(srv);
	return err;
}

/* ---- status items ---- */

int
menubar_status_item(struct ws_server *srv, struct ws_client *c,
    struct ws_window *w, uint32_t width, int32_t priority)
{
	(void)c;
	if (width == 0) {
		if (!w->status_item)
			return 0;
		window_stack_show(srv, w, false);
		w->status_item = false;
		w->level = w->client_level;
		if (w->client_visible)
			window_order(srv, w, WS_ORDER_ABOVE, NULL);
		ws_debug("window %u is no longer a status item", w->id);
		menubar_update(srv);
		return 0;
	}
	if (width > (uint32_t)display_main_frame(srv).width / 2)
		return EINVAL;
	if (w->menu_bar_window)
		return EINVAL;
	if (!w->status_item) {
		take_over(srv, w, WS_LEVEL_STATUS_ITEM);
		w->status_item = true;
		w->si_serial = ++srv->si_next_serial;
		ws_debug("window %u (%s) is a status item, %u points, "
		    "priority %d", w->id, w->owner->app_name, width, priority);
	}
	w->si_width = width;
	w->si_priority = priority;
	menubar_update(srv);
	return 0;
}

static int
si_compare(const void *pa, const void *pb)
{
	const struct ws_window *a = *(struct ws_window *const *)pa;
	const struct ws_window *b = *(struct ws_window *const *)pb;

	if (a->si_priority != b->si_priority)
		return a->si_priority > b->si_priority ? -1 : 1;
	return a->si_serial < b->si_serial ? -1 :
	    a->si_serial > b->si_serial ? 1 : 0;
}

/* Place the status items right to left; returns the width they take. */
static uint32_t
status_items_pack(struct ws_server *srv, int32_t height)
{
	struct ws_window *w, **list;
	ws_rect_t m = display_main_frame(srv), r;
	size_t n = 0, i;
	int32_t right = m.x + m.width, x = right;

	for (w = srv->windows; w != NULL; w = w->next)
		if (w->status_item)
			n++;
	if (n == 0)
		return 0;
	list = calloc(n, sizeof(*list));
	if (list == NULL)
		return srv->status_items_width;
	n = 0;
	for (w = srv->windows; w != NULL; w = w->next) {
		if (!w->status_item)
			continue;
		if (height <= 0 || !w->client_visible || w->owner->dead) {
			window_stack_show(srv, w, false);
			continue;
		}
		list[n++] = w;
	}
	qsort(list, n, sizeof(*list), si_compare);
	for (i = 0; i < n; i++) {
		w = list[i];
		x -= (int32_t)w->si_width;
		r = (ws_rect_t){ x, m.y, (int32_t)w->si_width, height };
		if (x < m.x) {
			window_stack_show(srv, w, false);	/* no room */
			x += (int32_t)w->si_width;
			continue;
		}
		if (!rect_same(&r, &w->frame))
			window_server_frame(srv, w, &r);
		window_stack_show(srv, w, true);
	}
	free(list);
	return (uint32_t)(right - x);
}

/* ---- which bar shows ---- */

void
menubar_update(struct ws_server *srv)
{
	struct ws_client *a = srv->active;
	struct ws_window *want = NULL, *old = srv->bar_shown;
	int32_t h = strip_height(srv);
	uint64_t now = ws_now_usec();
	uint32_t width;
	bool fallback;
	ws_rect_t r;

	if (h > 0) {
		if (a != NULL && !a->dead && a->bar_window != NULL &&
		    a->bar_window->buffer != NULL)
			want = a->bar_window;
		else if (old != NULL && !old->owner->dead) {
			/* the incoming app has no bar yet: the outgoing one
			   stays a little */
			if (srv->bar_hold_until == 0) {
				srv->bar_hold_until = now +
				    WS_MENU_BAR_HOLD_MS * 1000ULL;
				ws_timer_set(srv, TIMER_MENU_BAR,
				    WS_MENU_BAR_HOLD_MS, true);
				ws_debug("menu bar: app %u has none to show "
				    "yet, keeping window %u", a != NULL ?
				    a->id : 0, old->id);
			}
			if (now < srv->bar_hold_until)
				want = old;
		}
	}
	/* holding: the outgoing bar stays for an app with none yet */
	if (srv->bar_hold_until != 0 && !(want != NULL && want == old &&
	    (a == NULL || want != a->bar_window))) {
		srv->bar_hold_until = 0;
		ws_timer_cancel(srv, TIMER_MENU_BAR);
	}
	if (want != old) {
		/* both in this pass: the same composited frame */
		if (old != NULL) {
			sample_color(srv, old);
			window_stack_show(srv, old, false);
		}
		if (want != NULL) {
			r = strip_rect(srv, want->frame.height > 0 ?
			    want->frame.height : h);
			if (r.x != want->frame.x || r.y != want->frame.y) {
				r.height = want->frame.height;
				window_server_frame(srv, want, &r);
			}
			window_stack_show(srv, want, true);
		}
		srv->bar_shown = want;
		ws_debug("menu bar: window %u -> %u", old != NULL ? old->id : 0,
		    want != NULL ? want->id : 0);
	}

	/* the empty bar where no app's bar is */
	fallback = h > 0 && want == NULL;
	r = strip_rect(srv, h);
	if (fallback != srv->bar_fallback ||
	    (fallback && !rect_same(&r, &srv->bar_fallback_rect))) {
		if (srv->bar_fallback)
			damage_rect(srv, &srv->bar_fallback_rect);
		srv->bar_fallback = fallback;
		srv->bar_fallback_rect = r;
		if (fallback) {
			damage_rect(srv, &r);
			ws_debug("menu bar: empty bar #%06x", srv->bar_color);
		}
	}

	width = status_items_pack(srv, h);
	if (width != srv->status_items_width) {
		srv->status_items_width = width;
		ws_debug("status items: %u points", width);
		screen_send_usable(srv);
	}
}

void
menubar_timer(struct ws_server *srv)
{
	/* the hold is over: menubar_update() sees it */
	menubar_update(srv);
}

/* ---- events ---- */

/* The owner ordered w (its bar or a status item): kept for later. */
void
menubar_client_order(struct ws_server *srv, struct ws_window *w, int op)
{
	bool v = op != WS_ORDER_OUT;

	if (w->client_visible == v)
		return;
	w->client_visible = v;
	if (w->status_item)
		menubar_update(srv);
}

void
menubar_client_level(struct ws_server *srv, struct ws_window *w,
    int32_t level)
{
	(void)srv;
	w->client_level = level;
}

void
menubar_committed(struct ws_server *srv, struct ws_window *w)
{
	struct ws_client *a = w->owner->app;

	if (srv->bar_shown == w)
		return;
	/* the active app's bar has something to show now */
	if (a == srv->active && a->bar_window == w)
		menubar_update(srv);
}

void
menubar_window_gone(struct ws_server *srv, struct ws_window *w)
{
	if (w->menu_bar_window) {
		w->client_visible = false;
		bar_release(srv, w);
		menubar_update(srv);
	} else if (w->status_item) {
		window_stack_show(srv, w, false);
		w->status_item = false;
		menubar_update(srv);
	}
}

void
menubar_client_gone(struct ws_server *srv, struct ws_client *c)
{
	if (srv->status_front == c)
		srv->status_front = NULL;
}

/* A press on w (NULL: the desktop): a status item's app may use the
   pointer until the next press elsewhere. */
void
menubar_press(struct ws_server *srv, struct ws_window *w)
{
	if (w != NULL && w->status_item)
		srv->status_front = w->owner->app;
	else if (w == NULL || w->owner->app != srv->status_front)
		srv->status_front = NULL;
}

bool
menubar_may_point(struct ws_server *srv, struct ws_client *a)
{
	return app_has_focus(srv, a) ||
	    (a != NULL && a == srv->status_front);
}

/* Draw the empty bar where `region` (points) meets it. */
void
menubar_composite(struct ws_server *srv, pixman_region32_t *region)
{
	pixman_region32_t r;
	pixman_box32_t *b, *bb;
	pixman_color_t c;
	int32_t s = (int32_t)srv->scale;
	uint32_t col = srv->bar_color != 0 ? srv->bar_color :
	    BAR_DEFAULT_COLOR;
	int i, n;

	if (!srv->bar_fallback)
		return;
	pixman_region32_init(&r);
	pixman_region32_intersect_rect(&r, region, srv->bar_fallback_rect.x,
	    srv->bar_fallback_rect.y,
	    (unsigned)srv->bar_fallback_rect.width,
	    (unsigned)srv->bar_fallback_rect.height);
	b = pixman_region32_rectangles(&r, &n);
	if (n > 0 && (bb = calloc((size_t)n, sizeof(*bb))) != NULL) {
		for (i = 0; i < n; i++)
			bb[i] = (pixman_box32_t){ b[i].x1 * s, b[i].y1 * s,
			    b[i].x2 * s, b[i].y2 * s };
		c.red = (uint16_t)(((col >> 16) & 0xff) * 0x101);
		c.green = (uint16_t)(((col >> 8) & 0xff) * 0x101);
		c.blue = (uint16_t)((col & 0xff) * 0x101);
		c.alpha = 0xffff;
		shadow_fill_boxes(srv, &c, bb, n);
		free(bb);
	}
	pixman_region32_fini(&r);
}
