/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The CPU renderer (pixman). The screen is the shadow image, the
 * backing store; when that isn't the panel's size, the damaged part is
 * scaled into srv->panel at the end of each frame, and srv->out is
 * whichever of the two the panel shows. Outputs, captures and snapshots
 * read srv->out.
 */

#include <stdlib.h>

#include "server.h"

/* Make the shadow (and the panel image) for the current sizes. */
static int
pr_init(struct ws_server *srv)
{
	pixman_image_t *shadow, *panel = NULL;

	shadow = pixman_image_create_bits(PIXMAN_x8r8g8b8,
	    (int)srv->back_width, (int)srv->back_height, NULL, 0);
	if (shadow == NULL) {
		ws_err("can't allocate a %ux%u shadow image", srv->back_width,
		    srv->back_height);
		return -1;
	}
	if (srv->back_width != srv->panel_width ||
	    srv->back_height != srv->panel_height) {
		panel = pixman_image_create_bits(PIXMAN_x8r8g8b8,
		    (int)srv->panel_width, (int)srv->panel_height, NULL, 0);
		if (panel == NULL) {
			ws_err("can't allocate a %ux%u panel image",
			    srv->panel_width, srv->panel_height);
			pixman_image_unref(shadow);
			return -1;
		}
	}
	if (srv->shadow != NULL)
		pixman_image_unref(srv->shadow);
	if (srv->panel != NULL)
		pixman_image_unref(srv->panel);
	srv->shadow = shadow;
	srv->panel = panel;
	srv->out = panel != NULL ? panel : shadow;
	return 0;
}

static void
pr_fini(struct ws_server *srv)
{
	if (srv->shadow != NULL)
		pixman_image_unref(srv->shadow);
	if (srv->panel != NULL)
		pixman_image_unref(srv->panel);
	srv->shadow = srv->panel = srv->out = NULL;
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

/* Backing pixels as panel pixels, rounded out (and a pixel more for the
   filter). */
static void
region_to_panel(struct ws_server *srv, pixman_region32_t *in,
    pixman_region32_t *out)
{
	int64_t bw = srv->back_width, bh = srv->back_height;
	int64_t pw = srv->panel_width, ph = srv->panel_height;
	int64_t x1, y1, x2, y2;
	pixman_box32_t *b;
	int i, n;

	pixman_region32_init(out);
	b = pixman_region32_rectangles(in, &n);
	for (i = 0; i < n; i++) {
		x1 = b[i].x1 * pw / bw - 1;
		y1 = b[i].y1 * ph / bh - 1;
		x2 = (b[i].x2 * pw + bw - 1) / bw + 1;
		y2 = (b[i].y2 * ph + bh - 1) / bh + 1;
		if (x1 < 0)
			x1 = 0;
		if (y1 < 0)
			y1 = 0;
		if (x2 > pw)
			x2 = pw;
		if (y2 > ph)
			y2 = ph;
		if (x2 > x1 && y2 > y1)
			pixman_region32_union_rect(out, out, (int)x1, (int)y1,
			    (unsigned)(x2 - x1), (unsigned)(y2 - y1));
	}
}

/* Scale `region` (panel pixels) of the shadow into the panel image. */
static void
resample(struct ws_server *srv, pixman_region32_t *region)
{
	double fx = (double)srv->back_width / srv->panel_width;
	double fy = (double)srv->back_height / srv->panel_height;
	pixman_fixed_t *params = NULL;
	pixman_transform_t t;
	pixman_box32_t *ext;
	int n;

	if (!pixman_region32_not_empty(region))
		return;
	ext = pixman_region32_extents(region);
	pixman_transform_init_scale(&t, pixman_double_to_fixed(fx),
	    pixman_double_to_fixed(fy));
	pixman_image_set_transform(srv->shadow, &t);
	if (fx > 1.5 || fy > 1.5)
		params = pixman_filter_create_separable_convolution(&n,
		    pixman_double_to_fixed(fx), pixman_double_to_fixed(fy),
		    PIXMAN_KERNEL_LINEAR, PIXMAN_KERNEL_LINEAR,
		    PIXMAN_KERNEL_BOX, PIXMAN_KERNEL_BOX, 1, 1);
	if (params != NULL) {
		pixman_image_set_filter(srv->shadow,
		    PIXMAN_FILTER_SEPARABLE_CONVOLUTION, params, n);
		free(params);
	} else
		pixman_image_set_filter(srv->shadow, PIXMAN_FILTER_BILINEAR,
		    NULL, 0);
	pixman_image_set_repeat(srv->shadow, PIXMAN_REPEAT_PAD);
	pixman_image_set_clip_region32(srv->panel, region);
	pixman_image_composite32(PIXMAN_OP_SRC, srv->shadow, NULL, srv->panel,
	    ext->x1, ext->y1, 0, 0, ext->x1, ext->y1, ext->x2 - ext->x1,
	    ext->y2 - ext->y1);
	pixman_image_set_clip_region32(srv->panel, NULL);
	pixman_image_set_transform(srv->shadow, NULL);
	pixman_image_set_filter(srv->shadow, PIXMAN_FILTER_NEAREST, NULL, 0);
	pixman_image_set_repeat(srv->shadow, PIXMAN_REPEAT_NONE);
}

static void
pr_end(struct ws_server *srv, pixman_region32_t *back,
    pixman_region32_t *out)
{
	pixman_image_set_clip_region32(srv->shadow, NULL);
	if (srv->panel != NULL) {
		region_to_panel(srv, back, out);
		resample(srv, out);
	} else {
		pixman_region32_init(out);
		pixman_region32_copy(out, back);
	}
}

const struct ws_renderer_ops pixman_renderer_ops = {
	.name = "pixman",
	.init = pr_init,
	.fini = pr_fini,
	.begin = pr_begin,
	.draw = pr_draw,
	.fill = pr_fill,
	.end = pr_end,
};
