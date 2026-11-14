/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The compositor. Windows are composited bottom to top, but only inside
 * the damaged region, by the renderer (srv->renderer: render_pixman.c
 * today). Windows hidden there under opaque windows are left out, and
 * the background is only filled where no opaque window is. The output
 * backend then puts the same region on the screen (DRM) or does nothing
 * (headless).
 *
 * HiDPI (display.c): positions are points, the backing store is scale
 * pixels per point. Images are drawn at their own pixels per point
 * (shadow_image); one of another scale is scaled. When the backing
 * store isn't the panel's size, the renderer scales the damaged part to
 * the panel at the end of the frame.
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

int
compositor_init(struct ws_server *srv)
{
	if (srv->renderer == NULL)
		srv->renderer = &pixman_renderer_ops;
	if (srv->renderer->init(srv) < 0)
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
	if (srv->renderer->init(srv) < 0)
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
shadow_image(struct ws_server *srv, bool blend, pixman_image_t *src,
    uint32_t src_scale, double opacity, int32_t x, int32_t y, int32_t w,
    int32_t h)
{
	int32_t s = (int32_t)srv->scale;
	struct ws_draw d = { .src = src, .x = x * s, .y = y * s, .w = w * s,
	    .h = h * s, .fx = 1.0, .fy = 1.0, .blend = blend,
	    .opacity = opacity };

	if (w <= 0 || h <= 0)
		return;
	if (src_scale == 0)
		src_scale = 1;
	if ((int32_t)src_scale != s) {
		d.fx = d.fy = (double)src_scale / s;
		d.smooth = s % (int32_t)src_scale != 0;
	}
	srv->renderer->draw(srv, &d);
}

/* Draw sw x sh pixels of src into r (points), bilinear, with an
   opacity. */
void
shadow_scaled(struct ws_server *srv, pixman_image_t *src, int sw, int sh,
    ws_rect_t r, double opacity)
{
	int32_t s = (int32_t)srv->scale;
	struct ws_draw d = { .src = src, .x = r.x * s, .y = r.y * s,
	    .w = r.width * s, .h = r.height * s, .smooth = true, .pad = true,
	    .blend = true, .opacity = opacity };

	if (r.width <= 0 || r.height <= 0 || sw <= 0 || sh <= 0)
		return;
	d.fx = (double)sw / (r.width * s);
	d.fy = (double)sh / (r.height * s);
	srv->renderer->draw(srv, &d);
}

/* Fill w x h points at x, y with a colour (OVER). */
void
shadow_fill(struct ws_server *srv, const pixman_color_t *color, int32_t x,
    int32_t y, int32_t w, int32_t h)
{
	int32_t s = (int32_t)srv->scale;
	pixman_box32_t b = { x * s, y * s, (x + w) * s, (y + h) * s };

	if (w <= 0 || h <= 0)
		return;
	srv->renderer->fill(srv, color, &b, 1);
}

/* Fill boxes in backing pixels with a colour. */
void
shadow_fill_boxes(struct ws_server *srv, const pixman_color_t *color,
    const pixman_box32_t *boxes, int n)
{
	srv->renderer->fill(srv, color, boxes, n);
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

void
compositor_fini(struct ws_server *srv)
{
	pixman_region32_fini(&srv->damage);
	if (srv->renderer != NULL)
		srv->renderer->fini(srv);
}

/* The software cursor, at the pointer's backing pixel. */
static void
draw_cursor(struct ws_server *srv)
{
	int32_t s = (int32_t)srv->scale;
	uint32_t is = srv->cursor_image_scale ? srv->cursor_image_scale : 1;
	struct ws_draw d = { .src = srv->cursor_image,
	    .x = (int32_t)floor(srv->ptr_x * s) - srv->cursor_hot_x * s,
	    .y = (int32_t)floor(srv->ptr_y * s) - srv->cursor_hot_y * s,
	    .w = pixman_image_get_width(srv->cursor_image),
	    .h = pixman_image_get_height(srv->cursor_image),
	    .fx = 1.0, .fy = 1.0, .blend = true, .opacity = 1.0 };

	if ((int32_t)is != s) {
		d.fx = d.fy = (double)is / s;
		d.smooth = s % (int32_t)is != 0;
		d.w = d.w * s / (int32_t)is;
		d.h = d.h * s / (int32_t)is;
	}
	srv->renderer->draw(srv, &d);
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
	int bw, bh;

	if (opacity <= 0.0)
		return;
	if (w->tile_for != NULL) {
		/* a minimized window's tile (tile.c) */
		if (w->tile_image != NULL)
			shadow_image(srv, true, w->tile_image, srv->scale,
			    opacity, w->frame.x, w->frame.y, w->frame.width,
			    w->frame.height);
		return;
	}
	if (b == NULL || w->frame.width <= 0 || w->frame.height <= 0)
		return;
	bw = (int)(b->width / b->scale);
	bh = (int)(b->height / b->scale);
	if (bw > w->frame.width)
		bw = w->frame.width;
	if (bh > w->frame.height)
		bh = w->frame.height;
	if (w->frame.x >= ext->x2 || w->frame.y >= ext->y2 ||
	    w->frame.x + bw <= ext->x1 || w->frame.y + bh <= ext->y1)
		return;
	shadow_image(srv, opacity < 1.0 || ((w->flags & WS_WINDOW_OPAQUE) == 0 &&
	    b->format != WS_FORMAT_XRGB8888), b->image, b->scale, opacity,
	    w->frame.x, w->frame.y, bw, bh);
}

/*
 * Does w hide what is below its frame? Only when it is drawn without
 * blending (as composite_window does at opacity 1) over all of its
 * frame: a smaller buffer leaves the rest showing what is below.
 */
static bool
window_opaque(struct ws_window *w)
{
	struct ws_buffer *b = w->buffer;

	if (!w->visible || w->tile_for != NULL || b == NULL ||
	    w->frame.width <= 0 || w->frame.height <= 0)
		return false;
	if ((w->flags & WS_WINDOW_OPAQUE) == 0 &&
	    b->format != WS_FORMAT_XRGB8888)
		return false;
	return (int)(b->width / b->scale) >= w->frame.width &&
	    (int)(b->height / b->scale) >= w->frame.height;
}

/*
 * Walk the stack top-down and mark the windows not to draw in `region`
 * (points): those it doesn't meet, and those the opaque windows above
 * cover there. `bg` gets what no opaque window covers.
 */
static void
occlude(struct ws_server *srv, pixman_region32_t *region,
    pixman_region32_t *bg)
{
	pixman_region32_t vis;
	pixman_box32_t f;
	struct ws_window *w;
	bool hidden;

	pixman_region32_init(bg);
	pixman_region32_copy(bg, region);
	pixman_region32_init(&vis);
	for (w = srv->stack_top; w != NULL; w = w->below) {
		f = (pixman_box32_t){ w->frame.x, w->frame.y,
		    w->frame.x + w->frame.width, w->frame.y + w->frame.height };
		w->culled = true;
		if (w->frame.width <= 0 || w->frame.height <= 0 ||
		    pixman_region32_contains_rectangle(region, &f) ==
		    PIXMAN_REGION_OUT)
			continue;
		pixman_region32_intersect_rect(&vis, bg, f.x1, f.y1,
		    (unsigned)w->frame.width, (unsigned)w->frame.height);
		hidden = !pixman_region32_not_empty(&vis);
		if (hidden != w->occluded)
			ws_debug(hidden ? "window %u (%s) not drawn: covered" :
			    "window %u (%s) drawn again", w->id, w->title);
		w->occluded = hidden;
		w->culled = hidden;
		if (!hidden && window_opaque(w))
			pixman_region32_subtract(bg, bg, &vis);
	}
	pixman_region32_fini(&vis);
}

/* Composite `region` (points) and finish the frame; `out` gets what
   changed in panel pixels. */
static void
repaint_region(struct ws_server *srv, pixman_region32_t *region,
    pixman_region32_t *out)
{
	pixman_region32_t back, uncov, uncov_back;
	pixman_color_t bg;
	pixman_box32_t *ext, *bext, *b;
	struct ws_window *w;
	uint32_t c = srv->cfg.background;
	int n;

	region_to_back(srv, region, &back);
	srv->renderer->begin(srv, &back);
	ext = pixman_region32_extents(region);
	bext = pixman_region32_extents(&back);

	bg.red = (uint16_t)(((c >> 16) & 0xff) * 0x101);
	bg.green = (uint16_t)(((c >> 8) & 0xff) * 0x101);
	bg.blue = (uint16_t)((c & 0xff) * 0x101);
	bg.alpha = 0xffff;

	if (srv->ov.state != WS_OVERVIEW_STATE_CLOSED) {
		/* the window overview (overview.c) */
		srv->renderer->fill(srv, &bg, bext, 1);
		overview_composite(srv, region);
	} else {
		occlude(srv, region, &uncov);
		region_to_back(srv, &uncov, &uncov_back);
		b = pixman_region32_rectangles(&uncov_back, &n);
		if (n > 0)
			srv->renderer->fill(srv, &bg, b, n);
		pixman_region32_fini(&uncov_back);
		pixman_region32_fini(&uncov);
		for (w = srv->stack_bottom; w != NULL; w = w->above) {
			/* the border is outside the frame: drawn anyway */
			if (w == srv->tiling.border)
				tiling_draw_border(srv, w);
			if (!w->culled)
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

	srv->renderer->end(srv, &back, out);
	pixman_region32_fini(&back);
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
