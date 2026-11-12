/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The window overview (Exposé / Mission Control). Every window of every
 * app, or only the active app's (App Exposé), laid out without overlap
 * as live scaled-down previews, grouped by app with the app's icon under
 * each group, over the dimmed desktop. Windows animate from their
 * places into the overview and back (not with ReduceMotion).
 *
 * It opens on a tap of the Start key on its own, Control+Up, the
 * Mission Control key or a three-finger swipe up (keyboard.c, input.c);
 * Control+Down or a three-finger swipe down show App Exposé. While it
 * is open, WindowServer takes the pointer and the keyboard: hovering or
 * the arrow keys select a window, a click or Return picks it (its app
 * is activated with it in front, or a minimized one is restored), and
 * Escape, a click on empty space or another Start tap close it.
 *
 * Which windows: the normal and floating levels (documents and
 * panels), visible or minimized, but not app icons or miniwindows.
 * Desktop-level windows (InnerSpace) stay where they are, dimmed;
 * everything else (menus, the Dock, tiles) fades out. WindowServer
 * draws no text, so there are no titles. A workspace strip waits for
 * workspaces (#13).
 */

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

#define MARGIN		40	/* around the overview */
#define MARGIN_TOP	48	/* room for workspaces later */
#define GROUP_GAP	32	/* between app groups */
#define WINDOW_GAP	20	/* between windows of a group */
#define ICON_SIZE	48	/* the app icon under a group */
#define ICON_GAP	10
#define LABEL		(ICON_SIZE + ICON_GAP)
#define HIGHLIGHT	3	/* the selection's border */
#define HIGHLIGHT_GAP	4
#define DIM		0.55	/* how dark the desktop gets */
#define ANIM_USEC	250000
#define ANIM_TICK_MS	16
#define SWIPE_DISTANCE	60.0	/* touchpad units before a swipe acts */

/* The selection's border: a light blue, premultiplied. */
static const pixman_color_t highlight = { 0x6e00, 0x9600, 0xe600, 0xe600 };

bool
overview_active(struct ws_server *srv)
{
	return srv->ov.state == WS_OVERVIEW_STATE_OPENING ||
	    srv->ov.state == WS_OVERVIEW_STATE_OPEN;
}

static double
ease(double t)
{
	double u = 1.0 - t;

	return 1.0 - u * u * u;		/* ease-out cubic */
}

/* ---- which windows ---- */

static bool
eligible(struct ws_server *srv, struct ws_window *w)
{
	if (w->tile_for != NULL || w->buffer == NULL ||
	    w->frame.width <= 0 || w->frame.height <= 0 ||
	    (w->owner->flags & WS_CONNECT_TOOL) || w->owner->dead ||
	    w->level < WS_LEVEL_NORMAL || w->level > WS_LEVEL_FLOATING ||
	    (w->flags & WS_WINDOW_ICON) || (!w->visible && !w->minimized))
		return false;
	if (srv->ov.mode == WS_OVERVIEW_APP &&
	    w->owner->app->id != srv->ov.app_id)
		return false;
	return true;
}

static int
item_index(struct ws_server *srv, uint32_t window)
{
	unsigned i;

	for (i = 0; i < srv->ov.nitems; i++)
		if (srv->ov.items[i].window == window)
			return (int)i;
	return -1;
}

/* The part of w's buffer its frame shows, in buffer pixels. */
static void
buffer_extent(struct ws_window *w, int *cw, int *ch)
{
	struct ws_buffer *b = w->buffer;

	*cw = (int)b->width;
	*ch = (int)b->height;
	if (*cw > w->frame.width * (int)b->scale)
		*cw = w->frame.width * (int)b->scale;
	if (*ch > w->frame.height * (int)b->scale)
		*ch = w->frame.height * (int)b->scale;
}

/* ---- layout ---- */

struct group {
	unsigned	first;		/* index of its first item */
	unsigned	n;
	unsigned	cols;
	double		scale;		/* best for this group */
};

/* The largest scale at which n windows fit c columns of a cw x ch cell. */
static double
group_scale(struct ws_server *srv, struct ws_overview_item *it, unsigned n,
    unsigned c, double cw, double ch)
{
	unsigned r = (n + c - 1) / c, i;
	double sw = (cw - (c - 1) * WINDOW_GAP) / c;
	double sh = (ch - (r - 1) * WINDOW_GAP) / r;
	double s = 1.0;
	struct ws_window *w;

	if (sw <= 0 || sh <= 0)
		return 0;
	for (i = 0; i < n; i++) {
		w = window_lookup(srv, it[i].window);
		if (w == NULL)
			continue;
		s = fmin(s, fmin(sw / w->frame.width, sh / w->frame.height));
	}
	return s;
}

/*
 * Place the items (already grouped, groups in order): the app groups
 * in a grid of cells, as many columns as give the windows the largest
 * common scale (never above 1), and each group's windows in rows inside
 * its cell, centred, with its app icon under them.
 */
static void
layout(struct ws_server *srv)
{
	struct ws_overview *ov = &srv->ov;
	struct group *g;
	struct ws_window *w;
	double aw, ah, cw = 0, ch = 0, best = -1, s, gs, scale = 0;
	unsigned ng = 0, gc, gr, i, j, c, k, bestgc = 1, row, rows;
	int x, y, bx, by, bw, bh, cx0, cy0, m;

	if (ov->nitems == 0)
		return;
	g = calloc(ov->nitems, sizeof(*g));
	if (g == NULL)
		return;
	for (i = 0; i < ov->nitems; i++) {
		if (i == 0 || ov->items[i].group != ov->items[i - 1].group) {
			g[ng].first = i;
			ng++;
		}
		g[ng - 1].n++;
	}
	/* Inside the usable area: the Dock stays clear. */
	aw = (double)srv->usable.width - 2 * MARGIN;
	ah = (double)srv->usable.height - MARGIN_TOP - MARGIN;

	/* How many group columns. */
	for (gc = 1; gc <= ng; gc++) {
		gr = (ng + gc - 1) / gc;
		cw = (aw - (gc - 1) * GROUP_GAP) / gc;
		ch = (ah - (gr - 1) * GROUP_GAP) / gr - LABEL;
		if (cw <= 0 || ch <= 0)
			continue;
		s = 1.0;
		for (i = 0; i < ng; i++) {
			gs = 0;
			for (c = 1; c <= g[i].n; c++)
				gs = fmax(gs, group_scale(srv,
				    &ov->items[g[i].first], g[i].n, c, cw, ch));
			s = fmin(s, gs);
		}
		if (s > best + 1e-9) {
			best = s;
			bestgc = gc;
		}
	}
	gc = bestgc;
	gr = (ng + gc - 1) / gc;
	cw = (aw - (gc - 1) * GROUP_GAP) / gc;
	ch = (ah - (gr - 1) * GROUP_GAP) / gr - LABEL;
	scale = best > 0 ? best : 0.05;

	for (i = 0; i < ng; i++) {
		struct ws_overview_item *it = &ov->items[g[i].first];
		int rowh[64] = { 0 }, roww[64] = { 0 };

		/* Fewest columns that still fit at the common scale. */
		for (c = 1; c <= g[i].n; c++)
			if (group_scale(srv, it, g[i].n, c, cw, ch) >=
			    scale - 1e-9)
				break;
		if (c > g[i].n)
			c = g[i].n;
		rows = (g[i].n + c - 1) / c;
		if (rows > 64)
			rows = 64;
		for (j = 0; j < g[i].n; j++) {
			w = window_lookup(srv, it[j].window);
			it[j].to.width = w != NULL ?
			    (int32_t)lround(w->frame.width * scale) : 1;
			it[j].to.height = w != NULL ?
			    (int32_t)lround(w->frame.height * scale) : 1;
			if (it[j].to.width < 1)
				it[j].to.width = 1;
			if (it[j].to.height < 1)
				it[j].to.height = 1;
			row = j / c < rows ? j / c : rows - 1;
			roww[row] += it[j].to.width + (j % c ? WINDOW_GAP : 0);
			if (it[j].to.height > rowh[row])
				rowh[row] = it[j].to.height;
		}
		bw = bh = 0;
		for (row = 0; row < rows; row++) {
			if (roww[row] > bw)
				bw = roww[row];
			bh += rowh[row] + (row ? WINDOW_GAP : 0);
		}
		/* The group's cell; a short last row of groups is centred. */
		k = i / gc;
		m = (int)(k == gr - 1 ? ng - k * gc : gc);
		cx0 = srv->usable.x + MARGIN + (int)lround((aw - (m * cw +
		    (m - 1) * GROUP_GAP)) / 2 + (i % gc) * (cw + GROUP_GAP));
		cy0 = srv->usable.y + MARGIN_TOP +
		    (int)lround(k * (ch + LABEL + GROUP_GAP));
		bx = cx0 + (int)lround((cw - bw) / 2);
		by = cy0 + (int)lround((ch - bh) / 2);
		y = by;
		for (row = 0; row < rows; row++) {
			x = bx + (bw - roww[row]) / 2;
			for (j = row * c; j < g[i].n && j < (row + 1) * c; j++) {
				it[j].to.x = x;
				it[j].to.y = y + (rowh[row] - it[j].to.height) / 2;
				x += it[j].to.width + WINDOW_GAP;
			}
			y += rowh[row] + WINDOW_GAP;
		}
		memset(&it[0].icon, 0, sizeof(it[0].icon));
		for (j = 1; j < g[i].n; j++)
			memset(&it[j].icon, 0, sizeof(it[j].icon));
		it[0].icon.x = bx + (bw - ICON_SIZE) / 2;
		it[0].icon.y = by + bh + ICON_GAP;
		it[0].icon.width = ICON_SIZE;
		it[0].icon.height = ICON_SIZE;
	}
	for (i = 0; i < ov->nitems; i++)
		ov->items[i].cache_dirty = true;
	free(g);
}

static void
items_free(struct ws_overview *ov)
{
	unsigned i;

	for (i = 0; i < ov->nitems; i++)
		if (ov->items[i].cache != NULL)
			pixman_image_unref(ov->items[i].cache);
	free(ov->items);
	ov->items = NULL;
	ov->nitems = 0;
}

/* The app icon shown for window w's group, or NULL. */
static pixman_image_t *
app_icon(struct ws_window *w)
{
	return w->owner->app_icon != NULL ? w->owner->app_icon :
	    w->owner->app->app_icon;
}

/*
 * Collect the windows again and lay them out. Groups: the active app
 * first, then the others in the order of their frontmost window; in a
 * group, front to back; minimized windows after the visible ones.
 */
static void
rebuild(struct ws_server *srv)
{
	struct ws_overview *ov = &srv->ov;
	struct ws_window **list, *w, *t;
	struct ws_overview_item *items;
	uint32_t sel = 0, *apps;
	unsigned n = 0, na = 0, i, j, k;

	if (ov->selected >= 0 && (unsigned)ov->selected < ov->nitems)
		sel = ov->items[ov->selected].window;
	items_free(ov);
	ov->selected = -1;
	for (w = srv->windows; w != NULL; w = w->next)
		n++;
	if (n == 0)
		return;
	list = calloc(n, sizeof(*list));
	apps = calloc(n + 1, sizeof(*apps));
	items = calloc(n, sizeof(*items));
	if (list == NULL || apps == NULL || items == NULL) {
		free(list);
		free(apps);
		free(items);
		return;
	}
	/* Front to back: the stack (a tile stands for its window), then
	 * minimized windows the Dock shows. */
	n = 0;
	for (t = srv->stack_top; t != NULL; t = t->below) {
		w = t->tile_for != NULL ? t->tile_for : t;
		if (eligible(srv, w))
			list[n++] = w;
	}
	for (w = srv->windows; w != NULL; w = w->next)
		if (w->minimized && w->tile == NULL && eligible(srv, w))
			list[n++] = w;
	if (srv->active != NULL)
		apps[na++] = srv->active->id;
	for (i = 0; i < n; i++) {
		for (j = 0; j < na; j++)
			if (apps[j] == list[i]->owner->app->id)
				break;
		if (j == na)
			apps[na++] = list[i]->owner->app->id;
	}
	k = 0;
	for (j = 0; j < na; j++) {
		/* visible windows first, then minimized ones */
		for (i = 0; i < n; i++)
			if (list[i]->owner->app->id == apps[j] &&
			    !list[i]->minimized)
				items[k++].window = list[i]->id;
		for (i = 0; i < n; i++)
			if (list[i]->owner->app->id == apps[j] &&
			    list[i]->minimized)
				items[k++].window = list[i]->id;
	}
	/* Number the groups that have windows. */
	for (i = 0, j = 0; i < k; i++) {
		w = window_lookup(srv, items[i].window);
		items[i].app_id = w->owner->app->id;
		if (i > 0 && items[i].app_id != items[i - 1].app_id)
			j++;
		items[i].group = j;
	}
	ov->items = items;
	ov->nitems = k;
	free(list);
	free(apps);
	layout(srv);
	if (sel != 0)
		ov->selected = item_index(srv, sel);
	if (ov->selected < 0 && ov->nitems > 0) {
		/* the active app's key window, else the first */
		if (srv->active != NULL && srv->active->key_window != NULL)
			ov->selected = item_index(srv,
			    srv->active->key_window->id);
		if (ov->selected < 0)
			ov->selected = 0;
	}
}

/* ---- state ---- */

static void
notify_docks(struct ws_server *srv)
{
	struct ws_client *c;
	ws_event_t ev;

	for (c = srv->clients; c != NULL; c = c->next) {
		if (c->dead || c->doomed || (c->flags & WS_CONNECT_DOCK) == 0)
			continue;
		event_init(srv, &ev, WS_EVENT_OVERVIEW_STATE,
		    sizeof(ev.u.overview), 0);
		ev.u.overview.state = (uint32_t)srv->ov.state;
		ev.u.overview.mode = (uint32_t)srv->ov.mode;
		ev.u.overview.count = srv->ov.nitems;
		ev.u.overview.app_id = srv->ov.app_id;
		client_send(srv, c, &ev, -1);
	}
}

static void
finish(struct ws_server *srv)
{
	struct ws_overview *ov = &srv->ov;

	ws_timer_cancel(srv, TIMER_OVERVIEW);
	damage_all(srv);
	if (ov->state == WS_OVERVIEW_STATE_OPENING) {
		ov->t = 1.0;
		ov->state = WS_OVERVIEW_STATE_OPEN;
		return;
	}
	ov->t = 0.0;
	ov->state = WS_OVERVIEW_STATE_CLOSED;
	items_free(ov);
	ov->selected = -1;
	ws_debug("overview closed");
	pointer_recheck(srv);
	cursor_update(srv);
	notify_docks(srv);
}

static void
animate(struct ws_server *srv, int state)
{
	struct ws_overview *ov = &srv->ov;

	ov->state = state;
	ov->t_start = ov->t;
	ov->anim_start = ws_now_usec();
	damage_all(srv);
	if (srv->cfg.reduce_motion) {
		finish(srv);
		return;
	}
	ws_timer_set(srv, TIMER_OVERVIEW, ANIM_TICK_MS, false);
}

/* TIMER_OVERVIEW: the next animation frame. */
void
overview_anim_fire(struct ws_server *srv)
{
	struct ws_overview *ov = &srv->ov;
	double d = (double)(ws_now_usec() - ov->anim_start) / ANIM_USEC;

	damage_all(srv);
	if (ov->state == WS_OVERVIEW_STATE_OPENING) {
		ov->t = fmin(1.0, ov->t_start + d);
		if (ov->t >= 1.0)
			finish(srv);
	} else if (ov->state == WS_OVERVIEW_STATE_CLOSING) {
		ov->t = fmax(0.0, ov->t_start - d);
		if (ov->t <= 0.0)
			finish(srv);
	} else
		ws_timer_cancel(srv, TIMER_OVERVIEW);
}

void
overview_open(struct ws_server *srv, int mode)
{
	struct ws_overview *ov = &srv->ov;
	struct ws_client *app = srv->active;
	struct ws_window *w;
	bool was_closed = ov->state == WS_OVERVIEW_STATE_CLOSED;

	/* Not while a drag lasts (drag.c). */
	if (drag_active(srv))
		return;
	if (mode == WS_OVERVIEW_APP && app == NULL) {
		/* no active app: the app of the frontmost window */
		for (w = srv->stack_top; w != NULL; w = w->below)
			if (w->tile_for == NULL && w->level >= WS_LEVEL_NORMAL &&
			    w->level <= WS_LEVEL_FLOATING &&
			    (w->owner->flags & WS_CONNECT_TOOL) == 0) {
				app = w->owner->app;
				break;
			}
		if (app == NULL)
			return;
	}
	if (overview_active(srv) && ov->mode == mode &&
	    (mode != WS_OVERVIEW_APP || ov->app_id == app->id))
		return;
	/* The overview takes the pointer: a grab ends. */
	pointer_ungrab(srv, srv->pgrab, true);
	ov->mode = mode;
	ov->app_id = mode == WS_OVERVIEW_APP ? app->id : 0;
	ov->selected = -1;
	rebuild(srv);
	ov->dirty = false;
	ws_debug("overview %s (%u windows)", mode == WS_OVERVIEW_APP ?
	    "of one app" : "of every window", ov->nitems);
	if (overview_active(srv)) {
		damage_all(srv);	/* only the mode changed */
		return;
	}
	if (srv->hover != NULL) {
		ws_event_t ev;
		struct ws_window *h = srv->hover;

		srv->hover = NULL;
		if (h->tile_for == NULL) {
			event_init(srv, &ev, WS_EVENT_MOUSE_EXITED,
			    sizeof(ev.u.pointer), h->id);
			ev.u.pointer.x = srv->ptr_x - h->frame.x;
			ev.u.pointer.y = srv->ptr_y - h->frame.y;
			ev.u.pointer.screen_x = srv->ptr_x;
			ev.u.pointer.screen_y = srv->ptr_y;
			ev.u.pointer.buttons = srv->buttons;
			ev.u.pointer.modifiers = srv->modifiers;
			client_send(srv, h->owner, &ev, -1);
		}
	}
	srv->start_tap = 0;
	scroll_touch(srv);
	animate(srv, WS_OVERVIEW_STATE_OPENING);
	cursor_update(srv);
	if (was_closed)
		notify_docks(srv);
}

void
overview_close(struct ws_server *srv)
{
	if (!overview_active(srv))
		return;
	animate(srv, WS_OVERVIEW_STATE_CLOSING);
	if (srv->ov.state == WS_OVERVIEW_STATE_CLOSING)
		pointer_recheck(srv);
}

/* The keyboard and gesture triggers: open, switch mode, or close. */
void
overview_toggle(struct ws_server *srv, int mode)
{
	if (overview_active(srv) && srv->ov.mode == mode)
		overview_close(srv);
	else
		overview_open(srv, mode);
}

/* Once per pass of the event loop: windows changed, lay out again. */
void
overview_flush(struct ws_server *srv)
{
	if (srv->ov.dirty && overview_active(srv)) {
		rebuild(srv);
		damage_all(srv);
	}
	srv->ov.dirty = false;
}

void
overview_fini(struct ws_server *srv)
{
	items_free(&srv->ov);
}

/* ---- drawing ---- */

/* r grown by d on every side */
static ws_rect_t
grow(ws_rect_t r, int d)
{
	r.x -= d;
	r.y -= d;
	r.width += 2 * d;
	r.height += 2 * d;
	return r;
}

static void
damage_item(struct ws_server *srv, int i)
{
	ws_rect_t r;

	if (i < 0 || (unsigned)i >= srv->ov.nitems)
		return;
	r = grow(srv->ov.items[i].to, HIGHLIGHT + HIGHLIGHT_GAP);
	damage_rect(srv, &r);
}

/* Window w's place on the screen when the overview is closed. */
static ws_rect_t
home_rect(struct ws_server *srv, struct ws_window *w)
{
	ws_rect_t r = w->frame;
	int cw, ch;

	if (w->minimized) {
		if (w->tile != NULL)
			return w->tile->frame;
		/* in the Dock: from the middle of the Dock's edge (the
		   right one if it hasn't said) */
		r.width = r.height = 64;
		r.x = ((int32_t)srv->width - 64) / 2;
		r.y = ((int32_t)srv->height - 64) / 2;
		switch (srv->dock_edge) {
		case WS_DOCK_EDGE_LEFT:
			r.x = 0;
			break;
		case WS_DOCK_EDGE_TOP:
			r.y = 0;
			break;
		case WS_DOCK_EDGE_BOTTOM:
			r.y = (int32_t)srv->height - 64;
			break;
		default:
			r.x = (int32_t)srv->width - 64;
			break;
		}
		return r;
	}
	buffer_extent(w, &cw, &ch);
	r.width = cw / (int)w->buffer->scale;
	r.height = ch / (int)w->buffer->scale;
	return r;
}

/* A view of w's buffer pixels without the compositor's transform. */
static pixman_image_t *
buffer_view(struct ws_window *w)
{
	struct ws_buffer *b = w->buffer;

	return pixman_image_create_bits(b->format == WS_FORMAT_XRGB8888 ?
	    PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8, (int)b->width, (int)b->height,
	    pixman_image_get_data(b->image), (int)b->stride);
}

/* Draw sw x sh of src into r on the screen, bilinear, with a mask. */
static void
draw_scaled(struct ws_server *srv, pixman_image_t *src, int sw, int sh,
    ws_rect_t r, pixman_image_t *mask)
{
	pixman_transform_t tr;

	if (r.width <= 0 || r.height <= 0 || sw <= 0 || sh <= 0)
		return;
	pixman_transform_init_scale(&tr,
	    pixman_double_to_fixed((double)sw / r.width),
	    pixman_double_to_fixed((double)sh / r.height));
	pixman_image_set_transform(src, &tr);
	pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);
	pixman_image_set_repeat(src, PIXMAN_REPEAT_PAD);
	pixman_image_composite32(PIXMAN_OP_OVER, src, mask, srv->shadow, 0, 0,
	    0, 0, r.x, r.y, r.width, r.height);
	pixman_image_set_transform(src, NULL);
	pixman_image_set_filter(src, PIXMAN_FILTER_NEAREST, NULL, 0);
	pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
}

/* The preview at the item's place in the open overview, box-filtered
 * like the tiles, redrawn when the window commits. */
static pixman_image_t *
item_cache(struct ws_overview_item *it, struct ws_window *w)
{
	pixman_image_t *src;
	int cw, ch;

	if (it->cache != NULL && !it->cache_dirty)
		return it->cache;
	if (it->cache == NULL ||
	    pixman_image_get_width(it->cache) != it->to.width ||
	    pixman_image_get_height(it->cache) != it->to.height) {
		if (it->cache != NULL)
			pixman_image_unref(it->cache);
		it->cache = pixman_image_create_bits(PIXMAN_a8r8g8b8,
		    it->to.width, it->to.height, NULL, 0);
		if (it->cache == NULL)
			return NULL;
	}
	memset(pixman_image_get_data(it->cache), 0,
	    (size_t)pixman_image_get_stride(it->cache) * (size_t)it->to.height);
	src = buffer_view(w);
	if (src != NULL) {
		buffer_extent(w, &cw, &ch);
		tile_scale(src, it->cache, 0, 0, it->to.width, it->to.height,
		    cw, ch);
		pixman_image_unref(src);
	}
	it->cache_dirty = false;
	return it->cache;
}

static void
fill_rect(struct ws_server *srv, const pixman_color_t *color, int x, int y,
    int w, int h)
{
	pixman_image_t *solid = pixman_image_create_solid_fill(color);

	if (solid == NULL)
		return;
	pixman_image_composite32(PIXMAN_OP_OVER, solid, NULL, srv->shadow,
	    0, 0, 0, 0, x, y, w, h);
	pixman_image_unref(solid);
}

static void
draw_highlight(struct ws_server *srv, ws_rect_t r, double a)
{
	pixman_color_t c = highlight;
	int t = HIGHLIGHT;

	c.red = (uint16_t)(c.red * a);
	c.green = (uint16_t)(c.green * a);
	c.blue = (uint16_t)(c.blue * a);
	c.alpha = (uint16_t)(c.alpha * a);
	r = grow(r, HIGHLIGHT_GAP + t);
	fill_rect(srv, &c, r.x, r.y, r.width, t);
	fill_rect(srv, &c, r.x, r.y + r.height - t, r.width, t);
	fill_rect(srv, &c, r.x, r.y + t, t, r.height - 2 * t);
	fill_rect(srv, &c, r.x + r.width - t, r.y + t, t, r.height - 2 * t);
}

/* Stack position for drawing order: minimized windows lowest. */
static int
z_of(struct ws_server *srv, struct ws_window *w)
{
	struct ws_window *s;
	int z = 0;

	if (!w->visible)
		return -1;
	for (s = srv->stack_bottom; s != NULL && s != w; s = s->above)
		z++;
	return z;
}

/* Is w (or the window of tile w) shown by the overview? */
static bool
is_item(struct ws_server *srv, struct ws_window *w)
{
	if (w->tile_for != NULL)
		w = w->tile_for;
	return item_index(srv, w->id) >= 0;
}

/* The compositor's whole job while the overview is on screen. */
void
overview_composite(struct ws_server *srv, pixman_region32_t *region)
{
	struct ws_overview *ov = &srv->ov;
	pixman_box32_t *ext = pixman_region32_extents(region);
	pixman_image_t *mask = NULL, *src, *icon;
	pixman_color_t dim = { 0, 0, 0, 0 };
	struct ws_window *w;
	ws_rect_t from, r, to;
	double e = ease(ov->t);
	unsigned i, j, *order;
	int *z, cw, ch, tmp;

	/* The desktop stays, dimmed; the rest fades out. */
	for (w = srv->stack_bottom; w != NULL; w = w->above)
		if (w->tile_for == NULL && w->level < WS_LEVEL_NORMAL)
			composite_window(srv, w, ext, 1.0);
	dim.alpha = (uint16_t)(DIM * e * 0xffff);
	fill_rect(srv, &dim, ext->x1, ext->y1, ext->x2 - ext->x1,
	    ext->y2 - ext->y1);
	for (w = srv->stack_bottom; w != NULL; w = w->above)
		if ((w->tile_for != NULL || w->level >= WS_LEVEL_NORMAL) &&
		    !is_item(srv, w))
			composite_window(srv, w, ext, 1.0 - e);

	if (ov->nitems == 0)
		return;
	order = calloc(ov->nitems, sizeof(*order));
	z = calloc(ov->nitems, sizeof(*z));
	if (order == NULL || z == NULL) {
		free(order);
		free(z);
		return;
	}
	for (i = 0; i < ov->nitems; i++) {
		order[i] = i;
		w = window_lookup(srv, ov->items[i].window);
		z[i] = w != NULL ? z_of(srv, w) : -2;
	}
	for (i = 1; i < ov->nitems; i++)	/* few: insertion sort */
		for (j = i; j > 0 && z[order[j - 1]] > z[order[j]]; j--) {
			tmp = (int)order[j];
			order[j] = order[j - 1];
			order[j - 1] = (unsigned)tmp;
		}

	if (e < 1.0) {
		pixman_color_t a = { 0, 0, 0, (uint16_t)(e * 0xffff) };

		mask = pixman_image_create_solid_fill(&a);
	}
	for (j = 0; j < ov->nitems; j++) {
		struct ws_overview_item *it = &ov->items[order[j]];

		w = window_lookup(srv, it->window);
		if (w == NULL || w->buffer == NULL)
			continue;
		to = it->to;
		if (ov->state == WS_OVERVIEW_STATE_OPEN) {
			src = item_cache(it, w);
			if (src != NULL)
				pixman_image_composite32(PIXMAN_OP_OVER, src,
				    NULL, srv->shadow, 0, 0, 0, 0, to.x, to.y,
				    to.width, to.height);
		} else {
			from = home_rect(srv, w);
			r.x = (int32_t)lround(from.x + (to.x - from.x) * e);
			r.y = (int32_t)lround(from.y + (to.y - from.y) * e);
			r.width = (int32_t)lround(from.width +
			    (to.width - from.width) * e);
			r.height = (int32_t)lround(from.height +
			    (to.height - from.height) * e);
			src = buffer_view(w);
			if (src != NULL) {
				buffer_extent(w, &cw, &ch);
				/* minimized windows fade in from their tiles */
				draw_scaled(srv, src, cw, ch, r,
				    w->minimized ? mask : NULL);
				pixman_image_unref(src);
			}
			to = r;
		}
		if ((int)order[j] == ov->selected)
			draw_highlight(srv, to, e);
		if (it->icon.width > 0 && (icon = app_icon(w)) != NULL)
			draw_scaled(srv, icon, pixman_image_get_width(icon),
			    pixman_image_get_height(icon), it->icon, mask);
	}
	if (mask != NULL)
		pixman_image_unref(mask);
	free(order);
	free(z);
}

/* Window w drew a new frame: its preview follows. */
void
overview_window_committed(struct ws_server *srv, struct ws_window *w)
{
	int i;

	if (srv->ov.state == WS_OVERVIEW_STATE_CLOSED ||
	    (i = item_index(srv, w->id)) < 0)
		return;
	srv->ov.items[i].cache_dirty = true;
	damage_item(srv, i);
}

/* ---- input ---- */

static int
item_at(struct ws_server *srv, double x, double y)
{
	ws_rect_t r;
	unsigned i;

	for (i = 0; i < srv->ov.nitems; i++) {
		r = grow(srv->ov.items[i].to, HIGHLIGHT_GAP);
		if (x >= r.x && y >= r.y && x < r.x + r.width &&
		    y < r.y + r.height)
			return (int)i;
	}
	return -1;
}

static void
select_item(struct ws_server *srv, int i)
{
	if (i == srv->ov.selected)
		return;
	damage_item(srv, srv->ov.selected);
	srv->ov.selected = i;
	damage_item(srv, i);
}

/* The pointer moved: the window under it is selected. */
void
overview_motion(struct ws_server *srv)
{
	int i = item_at(srv, srv->ptr_x, srv->ptr_y);

	if (i >= 0)
		select_item(srv, i);
}

/* Pick item i: bring its window forward (or restore it), and close. */
static void
choose(struct ws_server *srv, int i)
{
	struct ws_window *w = NULL;

	if (i >= 0 && (unsigned)i < srv->ov.nitems)
		w = window_lookup(srv, srv->ov.items[i].window);
	if (w != NULL) {
		ws_debug("overview: window %u chosen", w->id);
		if (w->minimized)
			window_request_restore(srv, w);
		else if (w->visible)
			app_bring_forward(srv, w);
	}
	overview_close(srv);
}

/* A button went down while the overview is open. */
void
overview_button(struct ws_server *srv, uint32_t button)
{
	if (button != WS_BUTTON_LEFT)
		return;
	choose(srv, item_at(srv, srv->ptr_x, srv->ptr_y));
}

/* The nearest window from the selected one in direction dx, dy. */
static void
move_selection(struct ws_server *srv, int dx, int dy)
{
	struct ws_overview *ov = &srv->ov;
	double cx, cy, ox, oy, along, across, score, best = 1e18;
	ws_rect_t *r;
	unsigned i;
	int found = -1;

	if (ov->nitems == 0)
		return;
	if (ov->selected < 0) {
		select_item(srv, 0);
		return;
	}
	r = &ov->items[ov->selected].to;
	cx = r->x + r->width / 2.0;
	cy = r->y + r->height / 2.0;
	for (i = 0; i < ov->nitems; i++) {
		if ((int)i == ov->selected)
			continue;
		r = &ov->items[i].to;
		ox = r->x + r->width / 2.0 - cx;
		oy = r->y + r->height / 2.0 - cy;
		along = ox * dx + oy * dy;
		across = fabs(ox * dy - oy * dx);
		if (along <= 0)
			continue;
		score = along + 2 * across;
		if (score < best) {
			best = score;
			found = (int)i;
		}
	}
	if (found >= 0)
		select_item(srv, found);
}

static bool
is_modifier(xkb_keysym_t sym)
{
	return (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) ||
	    (sym >= XKB_KEY_ISO_Lock && sym <= XKB_KEY_ISO_Level5_Lock) ||
	    sym == XKB_KEY_Mode_switch || sym == XKB_KEY_Num_Lock;
}

/*
 * A key went down while the overview is open. Returns false for
 * modifier keys, which still go to the app as usual; every other key
 * is the overview's.
 */
bool
overview_key(struct ws_server *srv, uint32_t code, xkb_keysym_t sym)
{
	int n = (int)srv->ov.nitems;

	(void)code;
	if (is_modifier(sym))
		return false;
	switch (sym) {
	case XKB_KEY_Escape:
		overview_close(srv);
		break;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
	case XKB_KEY_space:
		choose(srv, srv->ov.selected);
		break;
	case XKB_KEY_Left:
	case XKB_KEY_KP_Left:
		move_selection(srv, -1, 0);
		break;
	case XKB_KEY_Right:
	case XKB_KEY_KP_Right:
		move_selection(srv, 1, 0);
		break;
	case XKB_KEY_Up:
	case XKB_KEY_KP_Up:
		move_selection(srv, 0, -1);
		break;
	case XKB_KEY_Down:
	case XKB_KEY_KP_Down:
		move_selection(srv, 0, 1);
		break;
	case XKB_KEY_Tab:
		if (n > 0)
			select_item(srv, (srv->ov.selected + 1) % n);
		break;
	case XKB_KEY_ISO_Left_Tab:
		if (n > 0)
			select_item(srv, (srv->ov.selected + n - 1) % n);
		break;
	default:
		break;
	}
	return true;
}

/*
 * A touchpad swipe (libinput's gesture events, or wsctl swipe). With
 * three fingers (four with ThreeFingerDrag): up shows every window, or
 * turns App Exposé into the full overview; down shows App Exposé, or
 * closes the overview. It acts once per swipe, as soon as the fingers
 * have moved far enough mostly up or down. Sideways swipes are left
 * for workspaces (#13).
 */
void
overview_swipe(struct ws_server *srv, int phase, unsigned fingers,
    double dx, double dy)
{
	struct ws_overview *ov = &srv->ov;
	unsigned want = srv->cfg.three_finger_drag ? 4 : 3;

	switch (phase) {
	case WS_GESTURE_BEGIN:
		srv->start_tap = 0;
		scroll_touch(srv);
		ov->swiping = fingers == want;
		ov->swipe_fired = false;
		ov->swipe_dx = ov->swipe_dy = 0;
		return;
	case WS_GESTURE_UPDATE:
		if (!ov->swiping || ov->swipe_fired)
			return;
		ov->swipe_dx += dx;
		ov->swipe_dy += dy;
		if (fabs(ov->swipe_dy) < SWIPE_DISTANCE ||
		    fabs(ov->swipe_dy) < 1.5 * fabs(ov->swipe_dx))
			return;
		ov->swipe_fired = true;
		if (ov->swipe_dy < 0) {
			if (!overview_active(srv) ||
			    ov->mode == WS_OVERVIEW_APP)
				overview_open(srv, WS_OVERVIEW_ALL);
		} else if (overview_active(srv))
			overview_close(srv);
		else
			overview_open(srv, WS_OVERVIEW_APP);
		return;
	default:
		ov->swiping = false;
		return;
	}
}

/* WS_MSG_OVERVIEW: the state, each item, then WS_EVENT_LIST_END. */
void
overview_report(struct ws_server *srv, struct ws_client *to)
{
	struct ws_overview *ov = &srv->ov;
	ws_event_t ev;
	unsigned i;

	event_init(srv, &ev, WS_EVENT_OVERVIEW_STATE, sizeof(ev.u.overview),
	    0);
	ev.u.overview.state = (uint32_t)ov->state;
	ev.u.overview.mode = (uint32_t)ov->mode;
	ev.u.overview.count = ov->nitems;
	ev.u.overview.selected = ov->selected >= 0 &&
	    (unsigned)ov->selected < ov->nitems ?
	    ov->items[ov->selected].window : 0;
	ev.u.overview.app_id = ov->app_id;
	client_send(srv, to, &ev, -1);
	for (i = 0; i < ov->nitems; i++) {
		struct ws_window *w = window_lookup(srv, ov->items[i].window);

		event_init(srv, &ev, WS_EVENT_OVERVIEW_ITEM,
		    sizeof(ev.u.overview_item), ov->items[i].window);
		ev.u.overview_item.window = ov->items[i].window;
		ev.u.overview_item.app_id = ov->items[i].app_id;
		ev.u.overview_item.frame = ov->items[i].to;
		if (w != NULL && app_icon(w) != NULL)
			ev.u.overview_item.icon = ov->items[i].icon;
		ev.u.overview_item.flags =
		    ((int)i == ov->selected ? WS_OVERVIEW_ITEM_SELECTED : 0) |
		    (w != NULL && w->minimized ? WS_OVERVIEW_ITEM_MINIMIZED : 0);
		ev.u.overview_item.group = ov->items[i].group;
		client_send(srv, to, &ev, -1);
	}
	event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
	client_send(srv, to, &ev, -1);
}
