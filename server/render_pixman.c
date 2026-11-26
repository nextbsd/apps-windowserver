/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The CPU renderer (pixman). The desktop is the shadow image, the
 * backing store. Each display shows its part of it (display.c): when
 * that part is the panel's size, the display's view is that part of the
 * shadow itself; otherwise the damaged part is scaled into the display's
 * own panel image at the end of each frame, and the view is that.
 * Outputs read the views; captures and snapshots read the main
 * display's (srv->out).
 */

#include <stdlib.h>
#include <string.h>

#include "server.h"

static void
views_free(struct ws_server *srv)
{
	struct ws_display *d;
	unsigned i;

	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (d->view != NULL && d->view != d->panel_image)
			pixman_image_unref(d->view);
		if (d->panel_image != NULL)
			pixman_image_unref(d->panel_image);
		d->view = d->panel_image = NULL;
	}
	srv->out = NULL;
}

/* d's view: its part of the shadow as is, or a panel image to scale it
   into. */
static int
view_make(struct ws_server *srv, struct ws_display *d)
{
	uint32_t *bits = pixman_image_get_data(srv->shadow);
	int stride = pixman_image_get_stride(srv->shadow);

	if (d->back.width == d->dest.width &&
	    d->back.height == d->dest.height &&
	    d->dest.width == (int32_t)d->panel_width &&
	    d->dest.height == (int32_t)d->panel_height) {
		d->view = pixman_image_create_bits(PIXMAN_x8r8g8b8,
		    d->back.width, d->back.height,
		    (uint32_t *)((uint8_t *)bits + (size_t)d->back.y * stride +
		    (size_t)d->back.x * 4), stride);
		return d->view != NULL ? 0 : -1;
	}
	/* zeroed: a mirror's bars stay black */
	d->panel_image = pixman_image_create_bits(PIXMAN_x8r8g8b8,
	    (int)d->panel_width, (int)d->panel_height, NULL, 0);
	if (d->panel_image == NULL) {
		ws_err("can't allocate a %ux%u panel image", d->panel_width,
		    d->panel_height);
		return -1;
	}
	d->view = d->panel_image;
	return 0;
}

/* Make the shadow, and each display's view, for the current sizes. */
static int
pr_init(struct ws_server *srv)
{
	pixman_image_t *shadow;
	unsigned i;

	shadow = pixman_image_create_bits(PIXMAN_x8r8g8b8,
	    (int)srv->back_width, (int)srv->back_height, NULL, 0);
	if (shadow == NULL) {
		ws_err("can't allocate a %ux%u shadow image", srv->back_width,
		    srv->back_height);
		return -1;
	}
	views_free(srv);
	if (srv->shadow != NULL)
		pixman_image_unref(srv->shadow);
	srv->shadow = shadow;
	for (i = 0; i < srv->ndisplays; i++)
		if (srv->displays[i]->on && view_make(srv,
		    srv->displays[i]) < 0) {
			views_free(srv);
			return -1;
		}
	srv->out = srv->main != NULL ? srv->main->view : NULL;
	return 0;
}

static void
pr_fini(struct ws_server *srv)
{
	views_free(srv);
	if (srv->shadow != NULL)
		pixman_image_unref(srv->shadow);
	srv->shadow = NULL;
}

/* d is unplugged: its view goes with it (display.c frees them). */
static void
pr_display_gone(struct ws_server *srv, struct ws_display *d)
{
	if (srv->out == d->view)
		srv->out = NULL;
}

static void
pr_begin(struct ws_server *srv, pixman_region32_t *back)
{
	pixman_image_set_clip_region32(srv->shadow, back);
}

static void
pr_draw(struct ws_server *srv, const struct ws_draw *d)
{
	pixman_image_t *mask = NULL;
	pixman_color_t a;
	pixman_transform_t t;
	bool scaled = d->fx != 1.0 || d->fy != 1.0;

	if (d->w <= 0 || d->h <= 0)
		return;
	if (d->opacity < 1.0) {
		a.red = a.green = a.blue = 0;
		a.alpha = (uint16_t)(d->opacity * 0xffff);
		mask = pixman_image_create_solid_fill(&a);
	}
	if (scaled) {
		pixman_transform_init_scale(&t, pixman_double_to_fixed(d->fx),
		    pixman_double_to_fixed(d->fy));
		pixman_image_set_transform(d->src, &t);
		pixman_image_set_filter(d->src, d->smooth ?
		    PIXMAN_FILTER_BILINEAR : PIXMAN_FILTER_NEAREST, NULL, 0);
	}
	if (d->pad)
		pixman_image_set_repeat(d->src, PIXMAN_REPEAT_PAD);
	pixman_image_composite32(d->blend ? PIXMAN_OP_OVER : PIXMAN_OP_SRC,
	    d->src, mask, srv->shadow, 0, 0, 0, 0, d->x, d->y, d->w, d->h);
	if (scaled) {
		pixman_image_set_transform(d->src, NULL);
		pixman_image_set_filter(d->src, PIXMAN_FILTER_NEAREST, NULL, 0);
	}
	if (d->pad)
		pixman_image_set_repeat(d->src, PIXMAN_REPEAT_NONE);
	if (mask != NULL)
		pixman_image_unref(mask);
}

static void
pr_fill(struct ws_server *srv, const pixman_color_t *color,
    const pixman_box32_t *boxes, int n)
{
	pixman_image_t *solid;
	pixman_box32_t b;
	int i;

	if (color->alpha == 0xffff) {
		/* fill_boxes keeps to the clip but not to the image */
		for (i = 0; i < n; i++) {
			b = boxes[i];
			if (b.x1 < 0)
				b.x1 = 0;
			if (b.y1 < 0)
				b.y1 = 0;
			if (b.x2 > (int32_t)srv->back_width)
				b.x2 = (int32_t)srv->back_width;
			if (b.y2 > (int32_t)srv->back_height)
				b.y2 = (int32_t)srv->back_height;
			if (b.x2 > b.x1 && b.y2 > b.y1)
				pixman_image_fill_boxes(PIXMAN_OP_SRC,
				    srv->shadow, color, 1, &b);
		}
		return;
	}
	solid = pixman_image_create_solid_fill(color);
	if (solid == NULL)
		return;
	for (i = 0; i < n; i++)
		pixman_image_composite32(PIXMAN_OP_OVER, solid, NULL,
		    srv->shadow, 0, 0, 0, 0, boxes[i].x1, boxes[i].y1,
		    boxes[i].x2 - boxes[i].x1, boxes[i].y2 - boxes[i].y1);
	pixman_image_unref(solid);
}

/* One box blur pass along a line of n pixels, `step` words apart in
   `in`, into `out` (contiguous): each the average of the 2r+1 around it,
   the ends repeated. */
static void
box_line(const uint32_t *in, uint32_t *out, int n, int step, int r)
{
	uint32_t sr = 0, sg = 0, sb = 0, p;
	int i, j, div = 2 * r + 1;

	for (j = -r; j <= r; j++) {
		p = in[(j < 0 ? 0 : j >= n ? n - 1 : j) * step];
		sr += (p >> 16) & 0xff;
		sg += (p >> 8) & 0xff;
		sb += p & 0xff;
	}
	for (i = 0; i < n; i++) {
		out[i] = 0xff000000u | (sr / div) << 16 | (sg / div) << 8 |
		    (sb / div);
		p = in[(i + r + 1 >= n ? n - 1 : i + r + 1) * step];
		sr += (p >> 16) & 0xff;
		sg += (p >> 8) & 0xff;
		sb += p & 0xff;
		p = in[(i - r < 0 ? 0 : i - r) * step];
		sr -= (p >> 16) & 0xff;
		sg -= (p >> 8) & 0xff;
		sb -= p & 0xff;
	}
}

/*
 * Blur the shadow under d's window: copy what is there, with `radius`
 * around it, blur the copy (three box passes each way, close to a
 * gaussian) and lay it back over the shadow through the window's alpha,
 * so it shows only where the window does (not in its rounded corners).
 */
static void
pr_blur(struct ws_server *srv, const struct ws_draw *d, int radius)
{
	int32_t x1 = d->x - radius, y1 = d->y - radius;
	int32_t x2 = d->x + d->w + radius, y2 = d->y + d->h + radius;
	int32_t w, h, i, pass, r = radius / 3 > 0 ? radius / 3 : 1;
	pixman_image_t *copy;
	uint32_t *bits, *tmp;
	pixman_transform_t t;
	bool scaled = d->fx != 1.0 || d->fy != 1.0;

	if (x1 < 0)
		x1 = 0;
	if (y1 < 0)
		y1 = 0;
	if (x2 > (int32_t)srv->back_width)
		x2 = (int32_t)srv->back_width;
	if (y2 > (int32_t)srv->back_height)
		y2 = (int32_t)srv->back_height;
	w = x2 - x1;
	h = y2 - y1;
	if (w <= 0 || h <= 0)
		return;
	copy = pixman_image_create_bits(PIXMAN_x8r8g8b8, w, h, NULL, 0);
	tmp = malloc((size_t)(w > h ? w : h) * 4);
	if (copy == NULL || tmp == NULL) {
		if (copy != NULL)
			pixman_image_unref(copy);
		free(tmp);
		return;
	}
	pixman_image_composite32(PIXMAN_OP_SRC, srv->shadow, NULL, copy,
	    x1, y1, 0, 0, 0, 0, w, h);
	bits = pixman_image_get_data(copy);
	for (pass = 0; pass < 3; pass++) {
		for (i = 0; i < h; i++) {
			box_line(bits + (size_t)i * w, tmp, w, 1, r);
			memcpy(bits + (size_t)i * w, tmp, (size_t)w * 4);
		}
		for (i = 0; i < w; i++) {
			int32_t j;

			box_line(bits + i, tmp, h, w, r);
			for (j = 0; j < h; j++)
				bits[(size_t)j * w + i] = tmp[j];
		}
	}
	free(tmp);
	/* back over the shadow where the window is, through its alpha */
	if (scaled) {
		pixman_transform_init_scale(&t, pixman_double_to_fixed(d->fx),
		    pixman_double_to_fixed(d->fy));
		pixman_image_set_transform(d->src, &t);
		pixman_image_set_filter(d->src, d->smooth ?
		    PIXMAN_FILTER_BILINEAR : PIXMAN_FILTER_NEAREST, NULL, 0);
	}
	pixman_image_composite32(PIXMAN_OP_OVER, copy, d->src, srv->shadow,
	    d->x - x1, d->y - y1, 0, 0, d->x, d->y, d->w, d->h);
	if (scaled) {
		pixman_image_set_transform(d->src, NULL);
		pixman_image_set_filter(d->src, PIXMAN_FILTER_NEAREST, NULL, 0);
	}
	pixman_image_unref(copy);
}

/* Scale `region` (d's panel pixels) of d's part of the shadow into its
   panel image. */
static void
resample(struct ws_server *srv, struct ws_display *d,
    pixman_region32_t *region)
{
	double fx = (double)d->back.width / d->dest.width;
	double fy = (double)d->back.height / d->dest.height;
	pixman_fixed_t *params = NULL;
	pixman_transform_t t;
	pixman_box32_t *ext;
	pixman_image_t *src;
	int n, stride = pixman_image_get_stride(srv->shadow);

	if (!pixman_region32_not_empty(region))
		return;
	/* d's part of the shadow on its own, so the transform stays small
	   (pixman's are 16.16) */
	src = pixman_image_create_bits(PIXMAN_x8r8g8b8, d->back.width,
	    d->back.height, (uint32_t *)((uint8_t *)pixman_image_get_data(
	    srv->shadow) + (size_t)d->back.y * stride + (size_t)d->back.x * 4),
	    stride);
	if (src == NULL)
		return;
	ext = pixman_region32_extents(region);
	/* panel pixel (px, py) samples it at (px - dest.x) * fx */
	pixman_transform_init_scale(&t, pixman_double_to_fixed(fx),
	    pixman_double_to_fixed(fy));
	pixman_transform_translate(&t, NULL,
	    pixman_double_to_fixed(-d->dest.x * fx),
	    pixman_double_to_fixed(-d->dest.y * fy));
	pixman_image_set_transform(src, &t);
	if (fx > 1.5 || fy > 1.5)
		params = pixman_filter_create_separable_convolution(&n,
		    pixman_double_to_fixed(fx), pixman_double_to_fixed(fy),
		    PIXMAN_KERNEL_LINEAR, PIXMAN_KERNEL_LINEAR,
		    PIXMAN_KERNEL_BOX, PIXMAN_KERNEL_BOX, 1, 1);
	if (params != NULL) {
		pixman_image_set_filter(src,
		    PIXMAN_FILTER_SEPARABLE_CONVOLUTION, params, n);
		free(params);
	} else
		pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);
	pixman_image_set_repeat(src, PIXMAN_REPEAT_PAD);
	pixman_image_set_clip_region32(d->panel_image, region);
	pixman_region32_intersect_rect(region, region, d->dest.x, d->dest.y,
	    (unsigned)d->dest.width, (unsigned)d->dest.height);
	pixman_image_set_clip_region32(d->panel_image, region);
	pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, d->panel_image,
	    ext->x1, ext->y1, 0, 0, ext->x1, ext->y1, ext->x2 - ext->x1,
	    ext->y2 - ext->y1);
	pixman_image_set_clip_region32(d->panel_image, NULL);
	pixman_image_unref(src);
}

static void
pr_end(struct ws_server *srv, pixman_region32_t *back,
    pixman_region32_t *out)
{
	pixman_region32_t panel;
	struct ws_display *d;
	unsigned i;

	pixman_image_set_clip_region32(srv->shadow, NULL);
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!d->on || d->panel_image == NULL)
			continue;
		display_back_to_panel(d, back, &panel);
		resample(srv, d, &panel);
		pixman_region32_fini(&panel);
	}
	pixman_region32_init(out);
	pixman_region32_copy(out, back);
}

const struct ws_renderer_ops pixman_renderer_ops = {
	.name = "pixman",
	.init = pr_init,
	.fini = pr_fini,
	.display_gone = pr_display_gone,
	.begin = pr_begin,
	.draw = pr_draw,
	.fill = pr_fill,
	.blur = pr_blur,
	.end = pr_end,
};
