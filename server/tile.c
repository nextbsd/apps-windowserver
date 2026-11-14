/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Tiles for minimized windows (WS_MSG_WINDOW_MINIMIZE). A tile is a
 * live, scaled-down picture of the window's current buffer on a dark
 * rounded tile (or the MinimizeTileImage picture), with the app's icon
 * in its bottom-right corner, and no text. It is redrawn whenever the
 * window commits a new frame, so a window that keeps drawing while
 * minimized stays live. window.c places tiles and keeps them in the
 * stack; this file draws them and handles their clicks: one click
 * selects a tile, a drag moves it and a double-click asks the app to
 * restore the window (WS_EVENT_WINDOW_RESTORE).
 */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

#define TILE_SIZE_MIN	32
#define TILE_SIZE_MAX	256
#define TILE_IMAGE_MAX	1024	/* MinimizeTileImage, pixels square */
#define DRAG_THRESHOLD	4.0	/* pixels before a press becomes a drag */

/* The plain tile: a dark grey face with a lighter rim, so it shows on
 * the default black background. Premultiplied, 16 bits per channel. */
static const pixman_color_t tile_rim = { 0x5c5c, 0x5c5c, 0x5c5c, 0xffff };
static const pixman_color_t tile_face = { 0x3030, 0x3030, 0x3030, 0xffff };
static const pixman_color_t tile_highlight = { 0x3000, 0x3000, 0x3000,
    0x3000 };

/* ---- shapes and pictures ---- */

/* An a8 mask of a size x size square with corners of radius r. */
static pixman_image_t *
round_mask(int size, double r)
{
	pixman_image_t *img;
	uint8_t *px;
	int x, y, sx, sy, stride, hits;
	double fx, fy, dx, dy;

	img = pixman_image_create_bits(PIXMAN_a8, size, size, NULL, 0);
	if (img == NULL)
		return NULL;
	px = (uint8_t *)pixman_image_get_data(img);
	stride = pixman_image_get_stride(img);
	for (y = 0; y < size; y++)
		for (x = 0; x < size; x++) {
			/* 4x4 samples per pixel for smooth corners */
			hits = 0;
			for (sy = 0; sy < 4; sy++)
				for (sx = 0; sx < 4; sx++) {
					fx = x + (sx + 0.5) / 4;
					fy = y + (sy + 0.5) / 4;
					dx = fx < r ? r - fx :
					    fx > size - r ? fx - (size - r) : 0;
					dy = fy < r ? r - fy :
					    fy > size - r ? fy - (size - r) : 0;
					if (dx * dx + dy * dy <= r * r)
						hits++;
				}
			px[y * stride + x] = (uint8_t)(hits * 255 / 16);
		}
	return img;
}

static void
fill_masked(pixman_image_t *dst, const pixman_color_t *color,
    pixman_image_t *mask, int x, int y)
{
	pixman_image_t *solid;

	solid = pixman_image_create_solid_fill(color);
	if (solid == NULL)
		return;
	pixman_image_composite32(PIXMAN_OP_OVER, solid, mask, dst, 0, 0, 0, 0,
	    x, y, pixman_image_get_width(mask),
	    pixman_image_get_height(mask));
	pixman_image_unref(solid);
}

/*
 * Draw sw x sh pixels of src scaled into dw x dh at dx, dy. Shrinking
 * uses a box filter over every source pixel, so a big window turns
 * into a clean thumbnail rather than a few sampled pixels. (The Dock's
 * previews use it too, dock.c.)
 */
void
tile_scale(pixman_image_t *src, pixman_image_t *dst, int dx, int dy,
    int dw, int dh, int sw, int sh)
{
	pixman_transform_t tr;
	pixman_fixed_t *params;
	double fx = (double)sw / dw, fy = (double)sh / dh;
	int n;

	pixman_transform_init_scale(&tr, pixman_double_to_fixed(fx),
	    pixman_double_to_fixed(fy));
	pixman_image_set_transform(src, &tr);
	pixman_image_set_repeat(src, PIXMAN_REPEAT_PAD);
	params = NULL;
	if (fx > 1.0 || fy > 1.0)
		params = pixman_filter_create_separable_convolution(&n,
		    pixman_double_to_fixed(fx > 1.0 ? fx : 1.0),
		    pixman_double_to_fixed(fy > 1.0 ? fy : 1.0),
		    PIXMAN_KERNEL_BOX, PIXMAN_KERNEL_BOX,
		    PIXMAN_KERNEL_BOX, PIXMAN_KERNEL_BOX, 2, 2);
	if (params != NULL) {
		pixman_image_set_filter(src, PIXMAN_FILTER_SEPARABLE_CONVOLUTION,
		    params, n);
		free(params);
	} else
		pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);
	pixman_image_composite32(PIXMAN_OP_OVER, src, NULL, dst, 0, 0, 0, 0,
	    dx, dy, dw, dh);
	pixman_image_set_transform(src, NULL);
	pixman_image_set_filter(src, PIXMAN_FILTER_NEAREST, NULL, 0);
	pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
}

/* Read one header token (a number or a word) of a Netpbm file. */
static int
pnm_token(FILE *f, char *buf, size_t len)
{
	size_t n = 0;
	int ch;

	for (;;) {
		ch = getc(f);
		if (ch == '#') {
			while (ch != '\n' && ch != EOF)
				ch = getc(f);
		}
		if (ch == EOF)
			return -1;
		if (ch != ' ' && ch != '\t' && ch != '\n' && ch != '\r')
			break;
	}
	while (ch != EOF && ch != ' ' && ch != '\t' && ch != '\n' &&
	    ch != '\r') {
		if (n + 1 < len)
			buf[n++] = (char)ch;
		ch = getc(f);
	}
	buf[n] = '\0';
	return 0;
}

/*
 * Load MinimizeTileImage: a binary Netpbm picture, PAM (P7, RGB or
 * RGB_ALPHA with straight alpha) or PPM (P6), 8 bits per channel. The
 * server reads no other formats, so it needs no image libraries;
 * `pamtopnm`, `pngtopam -alphapam` or `sips` make these from PNGs.
 */
static pixman_image_t *
load_tile_image(const char *path)
{
	char tok[32], magic[4];
	pixman_image_t *img = NULL;
	unsigned char px[4];
	uint32_t *row;
	char key;
	long width = 0, height = 0, depth = 3, maxval = 0;
	int x, y, stride, a;
	FILE *f;

	f = fopen(path, "rb");
	if (f == NULL) {
		ws_warn("MinimizeTileImage %s: %s", path, strerror(errno));
		return NULL;
	}
	if (pnm_token(f, magic, sizeof(magic)) < 0)
		goto bad;
	if (strcmp(magic, "P6") == 0) {
		if (pnm_token(f, tok, sizeof(tok)) < 0)
			goto bad;
		width = strtol(tok, NULL, 10);
		if (pnm_token(f, tok, sizeof(tok)) < 0)
			goto bad;
		height = strtol(tok, NULL, 10);
		if (pnm_token(f, tok, sizeof(tok)) < 0)
			goto bad;
		maxval = strtol(tok, NULL, 10);
	} else if (strcmp(magic, "P7") == 0) {
		for (;;) {
			if (pnm_token(f, tok, sizeof(tok)) < 0)
				goto bad;
			if (strcmp(tok, "ENDHDR") == 0)
				break;
			if (strcmp(tok, "TUPLTYPE") == 0) {
				if (pnm_token(f, tok, sizeof(tok)) < 0)
					goto bad;
				continue;
			}
			if (strcmp(tok, "WIDTH") != 0 &&
			    strcmp(tok, "HEIGHT") != 0 &&
			    strcmp(tok, "DEPTH") != 0 &&
			    strcmp(tok, "MAXVAL") != 0)
				goto bad;
			key = tok[0];
			if (pnm_token(f, tok, sizeof(tok)) < 0)
				goto bad;
			switch (key) {
			case 'W': width = strtol(tok, NULL, 10); break;
			case 'H': height = strtol(tok, NULL, 10); break;
			case 'D': depth = strtol(tok, NULL, 10); break;
			default: maxval = strtol(tok, NULL, 10); break;
			}
		}
	} else
		goto bad;
	if (width < 1 || height < 1 || width > TILE_IMAGE_MAX ||
	    height > TILE_IMAGE_MAX || maxval != 255 ||
	    (depth != 3 && depth != 4))
		goto bad;
	img = pixman_image_create_bits(PIXMAN_a8r8g8b8, (int)width,
	    (int)height, NULL, 0);
	if (img == NULL)
		goto bad;
	stride = pixman_image_get_stride(img) / 4;
	for (y = 0; y < height; y++) {
		row = pixman_image_get_data(img) + y * stride;
		for (x = 0; x < width; x++) {
			if (fread(px, 1, (size_t)depth, f) != (size_t)depth)
				goto bad;
			a = depth == 4 ? px[3] : 255;
			/* pixman wants premultiplied alpha */
			row[x] = (uint32_t)a << 24 |
			    (uint32_t)(px[0] * a / 255) << 16 |
			    (uint32_t)(px[1] * a / 255) << 8 |
			    (uint32_t)(px[2] * a / 255);
		}
	}
	fclose(f);
	ws_info("tile picture %s (%ldx%ld)", path, width, height);
	return img;
bad:
	ws_warn("MinimizeTileImage %s: not an 8-bit binary PAM or PPM "
	    "picture of at most %dx%d", path, TILE_IMAGE_MAX, TILE_IMAGE_MAX);
	if (img != NULL)
		pixman_image_unref(img);
	fclose(f);
	return NULL;
}

int
tile_init(struct ws_server *srv)
{
	int size = srv->cfg.minimize_tile_size;

	if (size < TILE_SIZE_MIN)
		size = TILE_SIZE_MIN;
	if (size > TILE_SIZE_MAX)
		size = TILE_SIZE_MAX;
	srv->cfg.minimize_tile_size = size;
	if (tile_rescale(srv) < 0)
		return -1;
	if (srv->cfg.minimize_tile_image[0] != '\0')
		srv->tile_background =
		    load_tile_image(srv->cfg.minimize_tile_image);
	return 0;
}

/* The masks at the screen's scale; every tile is drawn again. */
int
tile_rescale(struct ws_server *srv)
{
	int k = srv->scale ? (int)srv->scale : 1;
	int size = srv->cfg.minimize_tile_size * k;
	double r = size / 8.0;
	struct ws_window *t;

	if (srv->tile_mask_outer != NULL)
		pixman_image_unref(srv->tile_mask_outer);
	if (srv->tile_mask_inner != NULL)
		pixman_image_unref(srv->tile_mask_inner);
	srv->tile_mask_outer = round_mask(size, r);
	srv->tile_mask_inner = round_mask(size - 2 * k, r - k);
	if (srv->tile_mask_outer == NULL || srv->tile_mask_inner == NULL)
		return -1;
	for (t = srv->stack_bottom; t != NULL; t = t->above)
		if (t->tile_for != NULL)
			t->tile_dirty = true;
	return 0;
}

void
tile_fini(struct ws_server *srv)
{
	if (srv->tile_mask_outer != NULL)
		pixman_image_unref(srv->tile_mask_outer);
	if (srv->tile_mask_inner != NULL)
		pixman_image_unref(srv->tile_mask_inner);
	if (srv->tile_background != NULL)
		pixman_image_unref(srv->tile_background);
	srv->tile_mask_outer = srv->tile_mask_inner = NULL;
	srv->tile_background = NULL;
}

void
tile_free(struct ws_window *t)
{
	if (t->tile_image != NULL)
		pixman_image_unref(t->tile_image);
	pixman_region32_fini(&t->pending_damage);
	free(t);
}

/* ---- drawing ---- */

static void
tile_render(struct ws_server *srv, struct ws_window *t)
{
	struct ws_window *w = t->tile_for;
	struct ws_buffer *b = w->buffer;
	pixman_image_t *icon, *src;
	/* drawn in backing pixels, sharp on HiDPI */
	int k = (int)srv->scale, s = t->frame.width * k;
	int pad, inner, cw, ch, dw, dh, bs;
	double f;

	t->tile_dirty = false;
	if (t->tile_image != NULL && pixman_image_get_width(t->tile_image) != s) {
		pixman_image_unref(t->tile_image);
		t->tile_image = NULL;
	}
	if (t->tile_image == NULL) {
		t->tile_image = pixman_image_create_bits(PIXMAN_a8r8g8b8, s, s,
		    NULL, 0);
		if (t->tile_image == NULL)
			return;
	}
	memset(pixman_image_get_data(t->tile_image), 0,
	    (size_t)pixman_image_get_stride(t->tile_image) * (size_t)s);

	if (srv->tile_background != NULL)
		tile_scale(srv->tile_background, t->tile_image, 0, 0, s, s,
		    pixman_image_get_width(srv->tile_background),
		    pixman_image_get_height(srv->tile_background));
	else {
		fill_masked(t->tile_image, &tile_rim, srv->tile_mask_outer, 0, 0);
		fill_masked(t->tile_image, &tile_face, srv->tile_mask_inner,
		    k, k);
	}
	if (srv->tile_selected == t)
		fill_masked(t->tile_image, &tile_highlight,
		    srv->tile_mask_outer, 0, 0);

	/* The window, as much of it as its frame shows, fitted inside the
	 * tile's margin and never enlarged. */
	pad = s / 8;
	inner = s - 2 * pad;
	if (b != NULL && w->frame.width > 0 && w->frame.height > 0) {
		cw = (int)b->width;
		ch = (int)b->height;
		if (cw > w->frame.width * (int)b->scale)
			cw = w->frame.width * (int)b->scale;
		if (ch > w->frame.height * (int)b->scale)
			ch = w->frame.height * (int)b->scale;
		f = fmin((double)inner / cw, (double)inner / ch);
		if (f > (double)k / b->scale)
			f = (double)k / b->scale;	/* never enlarged */
		dw = (int)lround(cw * f);
		dh = (int)lround(ch * f);
		if (dw < 1)
			dw = 1;
		if (dh < 1)
			dh = 1;
		/* A view of the buffer's pixels without the compositor's
		 * own transform. */
		src = buffer_view_image(srv, b);
		if (src != NULL) {
			tile_scale(src, t->tile_image, (s - dw) / 2,
			    (s - dh) / 2, dw, dh, cw, ch);
			pixman_image_unref(src);
		}
	}

	/* The app's icon, in the bottom-right corner. */
	icon = t->owner->app_icon != NULL ? t->owner->app_icon :
	    t->owner->app->app_icon;
	if (icon != NULL) {
		bs = s * 3 / 8;
		tile_scale(icon, t->tile_image, s - bs - k, s - bs - k,
		    bs, bs, pixman_image_get_width(icon),
		    pixman_image_get_height(icon));
	}
}

/* Redraw the tiles that changed; the compositor calls this first. */
void
tiles_render(struct ws_server *srv)
{
	struct ws_window *t;

	for (t = srv->stack_bottom; t != NULL; t = t->above)
		if (t->tile_for != NULL && t->tile_dirty)
			tile_render(srv, t);
}

static void
tile_touch(struct ws_server *srv, struct ws_window *t)
{
	t->tile_dirty = true;
	damage_rect(srv, &t->frame);
}

/* Redraw every tile of c's app (its icon changed). */
void
tiles_redraw_app(struct ws_server *srv, struct ws_client *c)
{
	struct ws_window *t;

	for (t = srv->stack_bottom; t != NULL; t = t->above)
		if (t->tile_for != NULL && t->owner->app == c->app)
			tile_touch(srv, t);
}

int
tile_set_app_icon(struct ws_server *srv, struct ws_client *c,
    struct ws_buffer *b)
{
	pixman_image_t *img = NULL, *src;

	if (b != NULL) {
		if (b->width > WS_MAX_APP_ICON || b->height > WS_MAX_APP_ICON)
			return EINVAL;
		img = pixman_image_create_bits(PIXMAN_a8r8g8b8, (int)b->width,
		    (int)b->height, NULL, 0);
		src = buffer_view_image(srv, b);
		if (img == NULL || src == NULL) {
			if (img != NULL)
				pixman_image_unref(img);
			if (src != NULL)
				pixman_image_unref(src);
			return ENOMEM;
		}
		pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, img, 0, 0,
		    0, 0, 0, 0, b->width, b->height);
		pixman_image_unref(src);
	}
	if (c->app_icon != NULL)
		pixman_image_unref(c->app_icon);
	c->app_icon = img;
	ws_debug("client %u: app icon %ux%u", c->id, b ? b->width : 0,
	    b ? b->height : 0);
	tiles_redraw_app(srv, c);
	return 0;
}

/* ---- clicks ---- */

void
tile_select(struct ws_server *srv, struct ws_window *t)
{
	if (srv->tile_selected == t)
		return;
	if (srv->tile_selected != NULL)
		tile_touch(srv, srv->tile_selected);
	srv->tile_selected = t;
	if (t != NULL)
		tile_touch(srv, t);
}

/*
 * A button went down on tile t (the pointer grab is on it). A left
 * click selects it and brings it to the front of the icons; the second
 * click of a double-click activates the app and asks it to restore the
 * window. The tile stays until the app orders the window in.
 */
void
tile_press(struct ws_server *srv, struct ws_window *t, uint32_t button,
    uint32_t count)
{
	struct ws_window *w = t->tile_for;

	if (button != WS_BUTTON_LEFT)
		return;
	tile_select(srv, t);
	window_tile_raise(srv, t);
	srv->tile_dragging = false;
	srv->tile_grab_x = srv->ptr_x - t->frame.x;
	srv->tile_grab_y = srv->ptr_y - t->frame.y;
	if (count != 2)
		return;
	ws_debug("window %u: tile double-clicked", w->id);
	window_request_restore(srv, w);
}

/* The pointer moved with the left button held on tile t. */
void
tile_drag(struct ws_server *srv, struct ws_window *t)
{
	if (!srv->tile_dragging) {
		if (fabs(srv->ptr_x - srv->last_click_x) < DRAG_THRESHOLD &&
		    fabs(srv->ptr_y - srv->last_click_y) < DRAG_THRESHOLD)
			return;
		srv->tile_dragging = true;
	}
	window_tile_move(srv, t, (int32_t)lround(srv->ptr_x - srv->tile_grab_x),
	    (int32_t)lround(srv->ptr_y - srv->tile_grab_y));
}
