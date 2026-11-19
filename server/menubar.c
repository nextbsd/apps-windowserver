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
 * Status items are packed right to left from the right edge of the
 * display with focus, by priority (larger further right), then by age
 * (older further right). Their total width goes out with the usable
 * area, so apps' bars stop their menus short of it.
 *
 * Every display has a strip (apps-windowserver#100), as on macOS. The
 * live bar and the status items, the real windows, are on the display
 * with focus: the one the key window went to, or the last click on a
 * window or the desktop. Every other display's strip shows a dimmed
 * copy of them, composited here from the same buffers, so apps draw
 * once. A press on a copy makes its display the one with focus: the
 * bar and the items move there, in the same frame as the copies
 * change, and the press goes to the window now under the pointer, so
 * its menu opens on that display. A mirror shows its source's strip;
 * a bar never spans two displays.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

#define BAR_DEFAULT_COLOR	0xf2f2f2

/* How much of the bar a display without focus shows: macOS dims it to
   about half. */
#define BAR_COPY_OPACITY	0.5

static void focus_set(struct ws_server *srv, struct ws_display *d,
    const char *why);

bool
menubar_managed(const struct ws_window *w)
{
	return w != NULL && (w->menu_bar_window || w->status_item);
}

/* The display with focus, where the live bar and the status items
   are: the one chosen last, while it's part of the desktop, else the
   main display. */
struct ws_display *
menubar_focus_display(struct ws_server *srv)
{
	struct ws_display *d = display_lookup(srv, srv->bar_display);

	return d != NULL && display_shows(d) ? d : srv->main;
}

/* Display d's strip height: the menu bar strip's, at most a quarter of
   d; none on a mirror or a display that's off. */
int32_t
menubar_strip_height(struct ws_server *srv, const struct ws_display *d)
{
	uint32_t h = srv->menu_bar;

	if (d == NULL || !display_shows(d))
		return 0;
	if (h > (uint32_t)d->frame.height / 4)
		h = (uint32_t)d->frame.height / 4;
	return (int32_t)h;
}

/* The strip's height on the display with focus. */
static int32_t
strip_height(struct ws_server *srv)
{
	struct ws_display *f = menubar_focus_display(srv);

	return f != NULL ? menubar_strip_height(srv, f) :
	    (int32_t)srv->usable_menu_bar;
}

/* The strip along the top of the display with focus, `height` tall. */
static ws_rect_t
strip_rect(struct ws_server *srv, int32_t height)
{
	struct ws_display *f = menubar_focus_display(srv);
	ws_rect_t m = f != NULL ? f->frame : display_main_frame(srv);

	m.height = height;
	return m;
}

/* Damage every display's strip, `height` tall. */
static void
strips_damage(struct ws_server *srv, int32_t height)
{
	ws_rect_t r;
	unsigned i;

	if (height <= 0)
		return;
	for (i = 0; i < srv->ndisplays; i++) {
		if (!display_shows(srv->displays[i]))
			continue;
		r = srv->displays[i]->frame;
		if (r.height > height)
			r.height = height;
		damage_rect(srv, &r);
	}
}

/* Is d a display that shows a copy of the bar: part of the desktop,
   with a strip, and not the one with focus? */
static bool
shows_copy(struct ws_server *srv, const struct ws_display *d,
    const struct ws_display *focus)
{
	return d != focus && display_shows(d) &&
	    menubar_strip_height(srv, d) > 0;
}

/* Where w (the live bar or a status item) is copied to on display d:
   the bar at d's left end, an item as far in from d's right end as it
   is from the right end of the display with focus. */
static ws_rect_t
copy_rect(const struct ws_window *w, const struct ws_display *d,
    const struct ws_display *focus)
{
	ws_rect_t r = w->frame;

	if (w->status_item)
		r.x = d->frame.x + d->frame.width -
		    (focus->frame.x + focus->frame.width - w->frame.x);
	else
		r.x = d->frame.x + (w->frame.x - focus->frame.x);
	r.y = d->frame.y + (w->frame.y - focus->frame.y);
	return r;
}

/* w's buffer changed: damage its copies. */
static void
copies_damage(struct ws_server *srv, struct ws_window *w)
{
	struct ws_display *f = menubar_focus_display(srv), *d;
	ws_rect_t r;
	unsigned i;

	if (f == NULL || srv->ndisplays < 2 || !w->visible)
		return;
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!shows_copy(srv, d, f))
			continue;
		r = copy_rect(w, d, f);
		damage_rect(srv, &r);
	}
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
	if (p != srv->bar_color) {
		ws_debug("menu bar colour #%06x (window %u)", p, w->id);
		/* the strips under the copies are that colour */
		strips_damage(srv, srv->bar_strips_height);
	}
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
		/* the strip along the top of the display with focus */
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

/* Place the status items right to left; returns the width they take.
   *changed: whether any moved, came or went. */
static uint32_t
status_items_pack(struct ws_server *srv, int32_t height, bool *changed)
{
	struct ws_window *w, **list;
	ws_rect_t m = strip_rect(srv, height), r;
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
			if (w->visible)
				*changed = true;
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
			if (w->visible)
				*changed = true;
			window_stack_show(srv, w, false);	/* no room */
			x += (int32_t)w->si_width;
			continue;
		}
		if (!rect_same(&r, &w->frame)) {
			window_server_frame(srv, w, &r);
			*changed = true;
		}
		if (!w->visible)
			*changed = true;
		window_stack_show(srv, w, true);
	}
	free(list);
	return (uint32_t)(right - x);
}

/* ---- which bar shows ---- */

/* The display with focus, last drawn (menubar_update). */
static uint32_t focus_drawn;

void
menubar_update(struct ws_server *srv)
{
	struct ws_client *a = srv->active;
	struct ws_window *want = NULL, *old = srv->bar_shown;
	struct ws_display *focus;
	int32_t h, old_h = srv->bar_strips_height;
	uint64_t now = ws_now_usec();
	uint32_t width;
	bool fallback, changed = false;
	ws_rect_t r;

	/* the key window went to another display: the bar follows it */
	menubar_key_changed(srv);
	focus = menubar_focus_display(srv);
	h = strip_height(srv);
	if ((focus != NULL ? focus->id : 0) != focus_drawn) {
		focus_drawn = focus != NULL ? focus->id : 0;
		changed = true;
		ws_debug("menu bar: display %s has focus", focus != NULL ?
		    focus->connector : "none");
	}

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
		if (want != NULL)
			window_stack_show(srv, want, true);
		srv->bar_shown = want;
		changed = true;
		ws_debug("menu bar: window %u -> %u", old != NULL ? old->id : 0,
		    want != NULL ? want->id : 0);
	}
	if (want != NULL) {
		/* along the top of the display with focus, as wide as it:
		   never across two */
		r = strip_rect(srv, want->frame.height > 0 ?
		    want->frame.height : h);
		if (!rect_same(&r, &want->frame)) {
			window_server_frame(srv, want, &r);
			changed = true;
		}
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

	width = status_items_pack(srv, h, &changed);
	if (width != srv->status_items_width) {
		srv->status_items_width = width;
		ws_debug("status items: %u points", width);
		screen_send_usable(srv);
	}

	/* the copies on the other displays follow, in the same frame */
	if (h != old_h) {
		srv->bar_strips_height = h;
		changed = true;
	}
	if (changed)
		strips_damage(srv, h > old_h ? h : old_h);
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

	/* its copies on the other displays show the new buffer */
	if (srv->bar_shown == w || w->status_item)
		copies_damage(srv, w);
	if (srv->bar_shown == w || w->status_item)
		return;
	/* the active app's bar has something to show now */
	if (a == srv->active && a->bar_window == w)
		menubar_update(srv);
}

void
menubar_window_gone(struct ws_server *srv, struct ws_window *w)
{
	if (srv->bar_key == w)
		srv->bar_key = NULL;
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
	/* A press on the desktop or an app's window gives its display
	   focus, as on macOS; not one on the Dock, the bar or an item, a
	   menu, or a window that doesn't activate its app. */
	if (w == NULL || (!menubar_managed(w) &&
	    w->level < WS_LEVEL_MAIN_MENU &&
	    (w->flags & WS_WINDOW_NO_ACTIVATE) == 0 &&
	    !dock_owns_app(srv, w->owner->app)))
		focus_set(srv, display_at(srv, srv->ptr_x, srv->ptr_y),
		    "a press there");
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

/* Fill r (points) where `region` meets it with the bar's colour. */
static void
strip_fill(struct ws_server *srv, pixman_region32_t *region,
    const ws_rect_t *rect)
{
	pixman_region32_t r;
	pixman_box32_t *b, *bb;
	pixman_color_t c;
	int32_t s = (int32_t)srv->scale;
	uint32_t col = srv->bar_color != 0 ? srv->bar_color :
	    BAR_DEFAULT_COLOR;
	int i, n;

	if (rect->width <= 0 || rect->height <= 0)
		return;
	pixman_region32_init(&r);
	pixman_region32_intersect_rect(&r, region, rect->x, rect->y,
	    (unsigned)rect->width, (unsigned)rect->height);
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

/*
 * Draw the strips where `region` (points) meets them, in the bar's
 * colour: the empty bar on the display with focus while no app's bar
 * is there, and under the copies on the others.
 */
void
menubar_composite(struct ws_server *srv, pixman_region32_t *region)
{
	struct ws_display *f = menubar_focus_display(srv), *d;
	ws_rect_t r;
	unsigned i;

	if (srv->bar_fallback)
		strip_fill(srv, region, &srv->bar_fallback_rect);
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!shows_copy(srv, d, f))
			continue;
		r = d->frame;
		r.height = menubar_strip_height(srv, d);
		strip_fill(srv, region, &r);
	}
}

/*
 * The dimmed copies of the live bar and the status items on the
 * displays without focus, where `region` meets their strips. Drawn
 * just above the status items' level, as the windows they copy are.
 */
void
menubar_composite_copies(struct ws_server *srv, pixman_region32_t *region)
{
	struct ws_display *f = menubar_focus_display(srv), *d;
	struct ws_window *w;
	pixman_box32_t box;
	ws_rect_t s, r;
	unsigned i;

	if (f == NULL || srv->ndisplays < 2)
		return;
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!shows_copy(srv, d, f))
			continue;
		s = d->frame;
		s.height = menubar_strip_height(srv, d);
		box = (pixman_box32_t){ s.x, s.y, s.x + s.width,
		    s.y + s.height };
		if (pixman_region32_contains_rectangle(region, &box) ==
		    PIXMAN_REGION_OUT)
			continue;
		/* the bar, then the items above it, as they're stacked */
		for (w = srv->stack_bottom; w != NULL; w = w->above) {
			if (w != srv->bar_shown && !w->status_item)
				continue;
			r = copy_rect(w, d, f);
			if (r.x < s.x || r.y < s.y)
				continue;	/* no room on d */
			composite_window_copy(srv, w, r.x, r.y,
			    s.x + s.width - r.x, s.y + s.height - r.y,
			    BAR_COPY_OPACITY);
		}
	}
}

/* Is x, y on a copy of the bar (a strip of a display without focus)? */
static struct ws_display *
copy_at(struct ws_server *srv, double x, double y)
{
	struct ws_display *f = menubar_focus_display(srv), *d;

	if (f == NULL || srv->bar_strips_height <= 0)
		return NULL;
	d = display_at(srv, x, y);
	if (d == NULL || !shows_copy(srv, d, f) ||
	    y >= d->frame.y + menubar_strip_height(srv, d))
		return NULL;
	return d;
}

/* Does a copy of the bar cover x, y? Only windows above the status
   items' level are over it (window_at). */
bool
menubar_covers(struct ws_server *srv, double x, double y)
{
	return copy_at(srv, x, y) != NULL;
}

/* d has focus now: the live bar and the items move there. */
static void
focus_set(struct ws_server *srv, struct ws_display *d, const char *why)
{
	if (d == NULL || !display_shows(d) ||
	    d == menubar_focus_display(srv)) {
		if (d != NULL && display_shows(d))
			srv->bar_display = d->id;
		return;
	}
	srv->bar_display = d->id;
	ws_debug("menu bar: to display %s (%s)", d->connector, why);
	menubar_update(srv);
}

/*
 * A press is starting at the pointer: if it's on a copy of the bar,
 * that display gets focus, and the live bar and the items move under
 * the pointer before the press goes to whatever is there. Returns
 * whether it did.
 */
bool
menubar_copy_press(struct ws_server *srv)
{
	struct ws_display *d = copy_at(srv, srv->ptr_x, srv->ptr_y);

	if (d == NULL)
		return false;
	/* a window above the items' level (a menu, a modal panel) is
	   over the copy there: window_at() gives no other */
	if (window_at(srv, srv->ptr_x, srv->ptr_y) != NULL)
		return false;
	focus_set(srv, d, "a press on its copy");
	return true;
}

/* The active app's key window went to another display: the bar
   follows it there (called from menubar_update()). */
void
menubar_key_changed(struct ws_server *srv)
{
	struct ws_window *k = srv->active != NULL ?
	    srv->active->key_window : NULL;
	struct ws_display *d;
	ws_rect_t f;
	double cx, cy;

	if (k == srv->bar_key)
		return;
	srv->bar_key = k;
	if (k == NULL || !k->visible || k->frame.width <= 0 ||
	    k->frame.height <= 0)
		return;
	/* where most of it is: its middle, or its top-left corner */
	f = k->frame;
	cx = f.x + f.width / 2.0;
	cy = f.y + f.height / 2.0;
	d = display_at(srv, cx, cy);
	if (d == NULL)
		d = display_at(srv, f.x, f.y);
	if (d != NULL && display_shows(d))
		srv->bar_display = d->id;
}

/* The displays changed: a display with focus that went away or became
   a mirror gives it back to the main display. */
void
menubar_displays_changed(struct ws_server *srv)
{
	struct ws_display *d = display_lookup(srv, srv->bar_display);

	if (d == NULL || !display_shows(d))
		srv->bar_display = srv->main != NULL ? srv->main->id : 0;
	menubar_update(srv);
	strips_damage(srv, srv->bar_strips_height);
}
