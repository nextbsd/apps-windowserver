/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * CPU compositor (pixman). Windows are composited bottom to top into a
 * shadow image, but only inside the damaged region. The output backend
 * then copies the same region to the screen (DRM dumb buffer) or does
 * nothing (headless: the shadow is the screen).
 *
 * HiDPI (display.c): positions are points, the shadow is the backing
 * store, scale pixels per point. Images are drawn at their own pixels
 * per point (shadow_image); one of another scale is scaled. When the
 * backing store isn't the panel's size, the damaged part is scaled
 * into srv->panel too, which is what the output shows (srv->out).
 */

#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

static void draw_cursor(struct ws_server *srv);

/* Make the shadow (and the panel image) for the current sizes. */
static int
images_make(struct ws_server *srv)
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

int
compositor_init(struct ws_server *srv)
{
	if (images_make(srv) < 0)
		return -1;
	pixman_region32_init(&srv->damage);
	damage_all(srv);
	return 0;
}

/* The points, scale or panel changed (display.c): new images, all
   damaged. */
int
compositor_resize(struct ws_server *srv)
{
	if (images_make(srv) < 0)
		return -1;
	pixman_region32_clear(&srv->damage);
	damage_all(srv);
	return 0;
}

/* ---- drawing in points ---- */

/* The panel's pixel for a point (hardware cursor, captures). */
double
panel_x(struct ws_server *srv, double x)
{
	return x * srv->panel_width / (srv->width ? srv->width : 1);
}

double
panel_y(struct ws_server *srv, double y)
{
	return y * srv->panel_height / (srv->height ? srv->height : 1);
}

/*
 * Draw w x h points of src, an image with src_scale pixels per point,
 * with its top-left corner at x, y. Another scale than the screen's is
 * scaled: up by whole steps pixel for pixel (a 1x X window stays
 * crisp, only bigger), otherwise bilinear.
 */
void
shadow_image(struct ws_server *srv, pixman_op_t op, pixman_image_t *src,
    uint32_t src_scale, pixman_image_t *mask, int32_t x, int32_t y,
    int32_t w, int32_t h)
{
	int32_t s = (int32_t)srv->scale;
	pixman_transform_t t;

	if (w <= 0 || h <= 0)
		return;
	if (src_scale == 0)
		src_scale = 1;
	if ((int32_t)src_scale == s) {
		pixman_image_composite32(op, src, mask, srv->shadow, 0, 0, 0, 0,
		    x * s, y * s, w * s, h * s);
		return;
	}
	pixman_transform_init_scale(&t,
	    pixman_double_to_fixed((double)src_scale / s),
	    pixman_double_to_fixed((double)src_scale / s));
	pixman_image_set_transform(src, &t);
	pixman_image_set_filter(src, s % (int32_t)src_scale == 0 ?
	    PIXMAN_FILTER_NEAREST : PIXMAN_FILTER_BILINEAR, NULL, 0);
	pixman_image_composite32(op, src, mask, srv->shadow, 0, 0, 0, 0,
	    x * s, y * s, w * s, h * s);
	pixman_image_set_transform(src, NULL);
	pixman_image_set_filter(src, PIXMAN_FILTER_NEAREST, NULL, 0);
}

/* Draw sw x sh pixels of src into r (points), bilinear, with a mask. */
void
shadow_scaled(struct ws_server *srv, pixman_image_t *src, int sw, int sh,
    ws_rect_t r, pixman_image_t *mask)
{
	int32_t s = (int32_t)srv->scale;
	pixman_transform_t tr;

	if (r.width <= 0 || r.height <= 0 || sw <= 0 || sh <= 0)
		return;
	pixman_transform_init_scale(&tr,
	    pixman_double_to_fixed((double)sw / (r.width * s)),
	    pixman_double_to_fixed((double)sh / (r.height * s)));
	pixman_image_set_transform(src, &tr);
	pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);
	pixman_image_set_repeat(src, PIXMAN_REPEAT_PAD);
	pixman_image_composite32(PIXMAN_OP_OVER, src, mask, srv->shadow, 0, 0,
	    0, 0, r.x * s, r.y * s, r.width * s, r.height * s);
	pixman_image_set_transform(src, NULL);
	pixman_image_set_filter(src, PIXMAN_FILTER_NEAREST, NULL, 0);
	pixman_image_set_repeat(src, PIXMAN_REPEAT_NONE);
}

/* Fill w x h points at x, y with a colour (OVER). */
void
shadow_fill(struct ws_server *srv, const pixman_color_t *color, int32_t x,
    int32_t y, int32_t w, int32_t h)
{
	int32_t s = (int32_t)srv->scale;
	pixman_image_t *solid;

	if (w <= 0 || h <= 0)
		return;
	solid = pixman_image_create_solid_fill(color);
	if (solid == NULL)
		return;
	pixman_image_composite32(color->alpha == 0xffff ? PIXMAN_OP_SRC :
	    PIXMAN_OP_OVER, solid, NULL, srv->shadow, 0, 0, 0, 0, x * s, y * s,
	    w * s, h * s);
	pixman_image_unref(solid);
}

/* A region in points as backing pixels. */
static void
region_to_back(struct ws_server *srv, pixman_region32_t *in,
    pixman_region32_t *out)
{
	int32_t s = (int32_t)srv->scale;
	pixman_box32_t *b;
	int i, n;

	pixman_region32_init(out);
	b = pixman_region32_rectangles(in, &n);
	for (i = 0; i < n; i++)
		pixman_region32_union_rect(out, out, b[i].x1 * s, b[i].y1 * s,
		    (unsigned)((b[i].x2 - b[i].x1) * s),
		    (unsigned)((b[i].y2 - b[i].y1) * s));
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

void
compositor_fini(struct ws_server *srv)
{
	pixman_region32_fini(&srv->damage);
	if (srv->shadow != NULL)
		pixman_image_unref(srv->shadow);
	if (srv->panel != NULL)
		pixman_image_unref(srv->panel);
	srv->shadow = srv->panel = srv->out = NULL;
}

/* The software cursor, at the pointer's backing pixel. */
static void
draw_cursor(struct ws_server *srv)
{
	int32_t s = (int32_t)srv->scale;
	uint32_t is = srv->cursor_image_scale ? srv->cursor_image_scale : 1;
	int32_t x = (int32_t)floor(srv->ptr_x * s) - srv->cursor_hot_x * s;
	int32_t y = (int32_t)floor(srv->ptr_y * s) - srv->cursor_hot_y * s;
	int32_t w = pixman_image_get_width(srv->cursor_image);
	int32_t h = pixman_image_get_height(srv->cursor_image);
	pixman_transform_t t;

	if ((int32_t)is == s) {
		pixman_image_composite32(PIXMAN_OP_OVER, srv->cursor_image,
		    NULL, srv->shadow, 0, 0, 0, 0, x, y, w, h);
		return;
	}
	pixman_transform_init_scale(&t, pixman_double_to_fixed((double)is / s),
	    pixman_double_to_fixed((double)is / s));
	pixman_image_set_transform(srv->cursor_image, &t);
	pixman_image_set_filter(srv->cursor_image, s % (int32_t)is == 0 ?
	    PIXMAN_FILTER_NEAREST : PIXMAN_FILTER_BILINEAR, NULL, 0);
	pixman_image_composite32(PIXMAN_OP_OVER, srv->cursor_image, NULL,
	    srv->shadow, 0, 0, 0, 0, x, y, w * s / (int32_t)is,
	    h * s / (int32_t)is);
	pixman_image_set_transform(srv->cursor_image, NULL);
	pixman_image_set_filter(srv->cursor_image, PIXMAN_FILTER_NEAREST,
	    NULL, 0);
}

static void
send_frame_done(struct ws_server *srv, struct ws_window *w)
{
	ws_event_t ev;

	event_init(srv, &ev, WS_EVENT_FRAME_DONE, 0, w->id);
	client_send(srv, w->owner, &ev, -1);
}

/* The frame is on screen: answer WS_COMMIT_FRAME_DONE requests. */
void
compositor_present_done(struct ws_server *srv)
{
	struct ws_window *w;

	srv->present_pending = false;
	for (w = srv->windows; w != NULL; w = w->next) {
		if (w->frame_inflight) {
			w->frame_inflight = false;
			send_frame_done(srv, w);
		}
	}
}

/*
 * Draw window w (or a minimized window's tile) at its place, where it
 * meets ext, with the given opacity (the overview fades windows).
 */
void
composite_window(struct ws_server *srv, struct ws_window *w,
    const pixman_box32_t *ext, double opacity)
{
	struct ws_buffer *b = w->buffer;
	pixman_image_t *src, *mask = NULL;
	pixman_color_t a;
	int bw, bh;

	if (opacity <= 0.0)
		return;
	if (opacity < 1.0) {
		a.red = a.green = a.blue = 0;
		a.alpha = (uint16_t)(opacity * 0xffff);
		mask = pixman_image_create_solid_fill(&a);
	}
	if (w->tile_for != NULL) {
		/* a minimized window's tile (tile.c) */
		if (w->tile_image != NULL)
			shadow_image(srv, PIXMAN_OP_OVER, w->tile_image,
			    srv->scale, mask, w->frame.x, w->frame.y,
			    w->frame.width, w->frame.height);
		goto out;
	}
	if (b == NULL || w->frame.width <= 0 || w->frame.height <= 0)
		goto out;
	bw = (int)(b->width / b->scale);
	bh = (int)(b->height / b->scale);
	if (bw > w->frame.width)
		bw = w->frame.width;
	if (bh > w->frame.height)
		bh = w->frame.height;
	if (w->frame.x >= ext->x2 || w->frame.y >= ext->y2 ||
	    w->frame.x + bw <= ext->x1 || w->frame.y + bh <= ext->y1)
		goto out;
	src = b->image;
	shadow_image(srv,
	    mask == NULL && ((w->flags & WS_WINDOW_OPAQUE) ||
	    b->format == WS_FORMAT_XRGB8888) ? PIXMAN_OP_SRC :
	    PIXMAN_OP_OVER,
	    src, b->scale, mask, w->frame.x, w->frame.y, bw, bh);
out:
	if (mask != NULL)
		pixman_image_unref(mask);
}

/* Composite `region` (points) into the shadow; `back` gets it in
   backing pixels. */
static void
composite_region(struct ws_server *srv, pixman_region32_t *region,
    pixman_region32_t *back)
{
	pixman_color_t bg;
	pixman_image_t *fill;
	pixman_box32_t *ext, *bext;
	struct ws_window *w;
	uint32_t c = srv->cfg.background;

	region_to_back(srv, region, back);
	pixman_image_set_clip_region32(srv->shadow, back);
	ext = pixman_region32_extents(region);
	bext = pixman_region32_extents(back);

	bg.red = (uint16_t)(((c >> 16) & 0xff) * 0x101);
	bg.green = (uint16_t)(((c >> 8) & 0xff) * 0x101);
	bg.blue = (uint16_t)((c & 0xff) * 0x101);
	bg.alpha = 0xffff;
	fill = pixman_image_create_solid_fill(&bg);
	pixman_image_composite32(PIXMAN_OP_SRC, fill, NULL, srv->shadow,
	    0, 0, 0, 0, bext->x1, bext->y1, bext->x2 - bext->x1,
	    bext->y2 - bext->y1);
	pixman_image_unref(fill);

	if (srv->ov.state != WS_OVERVIEW_STATE_CLOSED) {
		/* the window overview (overview.c) */
		overview_composite(srv, region);
	} else {
		/* TODO: skip windows hidden behind opaque windows above
		 * them. */
		for (w = srv->stack_bottom; w != NULL; w = w->above) {
			if (w == srv->tiling.border)
				tiling_draw_border(srv, w);
			composite_window(srv, w, ext, 1.0);
		}
	}

	/* a drag's image, above everything but the cursor (drag.c) */
	drag_composite(srv);

	/* the capture indicator (capture.c), never in captured frames */
	if (srv->capture_indicator && !srv->capture_hide_overlays) {
		pixman_color_t ic;
		ws_rect_t r[4];
		int i;
		uint32_t col = WS_CAPTURE_BORDER_COLOR;

		ic.red = (uint16_t)(((col >> 16) & 0xff) * 0x101);
		ic.green = (uint16_t)(((col >> 8) & 0xff) * 0x101);
		ic.blue = (uint16_t)((col & 0xff) * 0x101);
		ic.alpha = 0xffff;
		capture_indicator_rects(srv, r);
		for (i = 0; i < 4; i++)
			shadow_fill(srv, &ic, r[i].x, r[i].y, r[i].width,
			    r[i].height);
	}

	if (!srv->cursor_hw && srv->cursor_visible && srv->cursor_image != NULL &&
	    !srv->capture_hide_cursor)
		draw_cursor(srv);

	pixman_image_set_clip_region32(srv->shadow, NULL);
}

/* Composite `region` (points), and scale it to the panel if the panel
   is another size; `out` gets what changed in srv->out's pixels. */
static void
repaint_region(struct ws_server *srv, pixman_region32_t *region,
    pixman_region32_t *out)
{
	pixman_region32_t back;

	composite_region(srv, region, &back);
	if (srv->panel != NULL) {
		region_to_panel(srv, &back, out);
		resample(srv, out);
		pixman_region32_fini(&back);
	} else
		*out = back;
}

/* Recomposite the damaged region and hand it to the output. */
void
compositor_repaint(struct ws_server *srv)
{
	pixman_region32_t region, out;
	struct ws_window *w;
	bool async;

	if (!srv->display_active || srv->present_pending)
		return;
	pixman_region32_intersect_rect(&srv->damage, &srv->damage, 0, 0,
	    srv->width, srv->height);
	if (!pixman_region32_not_empty(&srv->damage)) {
		/* Nothing changed, but don't leave frame requests hanging. */
		for (w = srv->windows; w != NULL; w = w->next)
			if (w->frame_requested) {
				w->frame_requested = false;
				send_frame_done(srv, w);
			}
		return;
	}
	pixman_region32_init(&region);
	pixman_region32_copy(&region, &srv->damage);
	pixman_region32_clear(&srv->damage);

	tiles_render(srv);
	repaint_region(srv, &region, &out);
	capture_screen_damage(srv, &out);
	for (w = srv->windows; w != NULL; w = w->next)
		if (w->frame_requested) {
			w->frame_requested = false;
			w->frame_inflight = true;
		}
	async = srv->output->present(srv, &out);
	pixman_region32_fini(&out);
	pixman_region32_fini(&region);
	if (async)
		srv->present_pending = true;
	else
		compositor_present_done(srv);
}

/* ---- captures (capture.c) ---- */

/* Is the pointer drawn into the shadow (a software cursor)? */
bool
compositor_cursor_in_shadow(struct ws_server *srv)
{
	return !srv->cursor_hw && srv->cursor_visible &&
	    srv->cursor_image != NULL && srv->cursor_rect.width > 0;
}

/*
 * Captured frames leave out the capture indicator, and the software
 * cursor unless the stream wants it. Draw the parts of the shadow they
 * cover again without them (hide), copy, then with them (!hide): the
 * shadow ends up as it was, and nothing is presented in between.
 */
void
compositor_capture_overlays(struct ws_server *srv, bool hide, bool cursor)
{
	pixman_region32_t reg, out;
	ws_rect_t r[5];
	int i, n = 0;

	if (srv->capture_indicator) {
		capture_indicator_rects(srv, r);
		n = 4;
	}
	if (cursor && compositor_cursor_in_shadow(srv))
		r[n++] = srv->cursor_rect;
	srv->capture_hide_overlays = hide;
	srv->capture_hide_cursor = hide && cursor;
	for (i = 0; i < n; i++) {
		if (r[i].width <= 0 || r[i].height <= 0)
			continue;
		pixman_region32_init_rect(&reg, r[i].x, r[i].y,
		    (unsigned)r[i].width, (unsigned)r[i].height);
		pixman_region32_intersect_rect(&reg, &reg, 0, 0, srv->width,
		    srv->height);
		if (pixman_region32_not_empty(&reg)) {
			repaint_region(srv, &reg, &out);
			pixman_region32_fini(&out);
		}
		pixman_region32_fini(&reg);
	}
}

/* ---- snapshots ---- */

/* A memfd holding the current screen as XRGB8888. */
int
snapshot_memfd(struct ws_server *srv, uint32_t *stride_out)
{
	uint32_t stride = srv->panel_width * 4;
	size_t size = (size_t)stride * srv->panel_height;
	pixman_image_t *dst;
	void *p;
	int fd;

	compositor_repaint(srv);
	fd = memfd_create("WindowServer-snapshot", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		close(fd);
		return -1;
	}
	dst = pixman_image_create_bits(PIXMAN_x8r8g8b8, (int)srv->panel_width,
	    (int)srv->panel_height, p, (int)stride);
	if (dst != NULL) {
		pixman_image_composite32(PIXMAN_OP_SRC, srv->out, NULL, dst,
		    0, 0, 0, 0, 0, 0, srv->panel_width, srv->panel_height);
		pixman_image_unref(dst);
	}
	munmap(p, size);
	*stride_out = stride;
	return fd;
}

int
snapshot_write_ppm(struct ws_server *srv, const char *path)
{
	const uint32_t *px;
	unsigned char *row;
	uint32_t x, y, stride;
	FILE *f;

	compositor_repaint(srv);
	f = fopen(path, "wb");
	if (f == NULL)
		return -1;
	row = malloc((size_t)srv->panel_width * 3);
	if (row == NULL) {
		fclose(f);
		return -1;
	}
	px = pixman_image_get_data(srv->out);
	stride = (uint32_t)pixman_image_get_stride(srv->out) / 4;
	fprintf(f, "P6\n%u %u\n255\n", srv->panel_width, srv->panel_height);
	for (y = 0; y < srv->panel_height; y++) {
		for (x = 0; x < srv->panel_width; x++) {
			uint32_t p = px[y * stride + x];

			row[x * 3] = (unsigned char)(p >> 16);
			row[x * 3 + 1] = (unsigned char)(p >> 8);
			row[x * 3 + 2] = (unsigned char)p;
		}
		fwrite(row, 3, srv->panel_width, f);
	}
	free(row);
	return fclose(f);
}

/* ---- headless output: the shadow image is the screen ---- */

static bool
headless_present(struct ws_server *srv, pixman_region32_t *damage)
{
	(void)srv;
	(void)damage;
	return false;
}

const struct ws_output_ops headless_output_ops = {
	.name = "headless",
	.present = headless_present,
};
