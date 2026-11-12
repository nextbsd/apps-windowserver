/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * CPU compositor (pixman). Windows are composited bottom to top into a
 * shadow image, but only inside the damaged region. The output backend
 * then copies the same region to the screen (DRM dumb buffer) or does
 * nothing (headless: the shadow is the screen).
 */

#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

int
compositor_init(struct ws_server *srv)
{
	srv->shadow = pixman_image_create_bits(PIXMAN_x8r8g8b8,
	    (int)srv->width, (int)srv->height, NULL, 0);
	if (srv->shadow == NULL) {
		ws_err("can't allocate a %ux%u shadow image", srv->width,
		    srv->height);
		return -1;
	}
	pixman_region32_init(&srv->damage);
	damage_all(srv);
	return 0;
}

/* The screen is now width x height: a new shadow image, all damaged. */
int
compositor_resize(struct ws_server *srv, uint32_t width, uint32_t height)
{
	pixman_image_t *img;

	img = pixman_image_create_bits(PIXMAN_x8r8g8b8, (int)width,
	    (int)height, NULL, 0);
	if (img == NULL) {
		ws_err("can't allocate a %ux%u shadow image", width, height);
		return -1;
	}
	if (srv->shadow != NULL)
		pixman_image_unref(srv->shadow);
	srv->shadow = img;
	srv->width = width;
	srv->height = height;
	pixman_region32_clear(&srv->damage);
	damage_all(srv);
	return 0;
}

void
compositor_fini(struct ws_server *srv)
{
	pixman_region32_fini(&srv->damage);
	if (srv->shadow != NULL)
		pixman_image_unref(srv->shadow);
	srv->shadow = NULL;
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
			pixman_image_composite32(PIXMAN_OP_OVER,
			    w->tile_image, mask, srv->shadow, 0, 0, 0,
			    0, w->frame.x, w->frame.y, w->frame.width,
			    w->frame.height);
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
	pixman_image_composite32(
	    mask == NULL && ((w->flags & WS_WINDOW_OPAQUE) ||
	    b->format == WS_FORMAT_XRGB8888) ? PIXMAN_OP_SRC :
	    PIXMAN_OP_OVER,
	    src, mask, srv->shadow, 0, 0, 0, 0,
	    w->frame.x, w->frame.y, bw, bh);
out:
	if (mask != NULL)
		pixman_image_unref(mask);
}

static void
composite_region(struct ws_server *srv, pixman_region32_t *region)
{
	pixman_color_t bg;
	pixman_image_t *fill;
	pixman_box32_t *ext;
	struct ws_window *w;
	uint32_t c = srv->cfg.background;

	pixman_image_set_clip_region32(srv->shadow, region);
	ext = pixman_region32_extents(region);

	bg.red = (uint16_t)(((c >> 16) & 0xff) * 0x101);
	bg.green = (uint16_t)(((c >> 8) & 0xff) * 0x101);
	bg.blue = (uint16_t)((c & 0xff) * 0x101);
	bg.alpha = 0xffff;
	fill = pixman_image_create_solid_fill(&bg);
	pixman_image_composite32(PIXMAN_OP_SRC, fill, NULL, srv->shadow,
	    0, 0, 0, 0, ext->x1, ext->y1, ext->x2 - ext->x1, ext->y2 - ext->y1);
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

	if (!srv->cursor_hw && srv->cursor_visible && srv->cursor_image != NULL)
		pixman_image_composite32(PIXMAN_OP_OVER, srv->cursor_image,
		    NULL, srv->shadow, 0, 0, 0, 0,
		    srv->cursor_rect.x, srv->cursor_rect.y,
		    srv->cursor_rect.width, srv->cursor_rect.height);

	pixman_image_set_clip_region32(srv->shadow, NULL);
}

/* Recomposite the damaged region and hand it to the output. */
void
compositor_repaint(struct ws_server *srv)
{
	pixman_region32_t region;
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
	composite_region(srv, &region);
	for (w = srv->windows; w != NULL; w = w->next)
		if (w->frame_requested) {
			w->frame_requested = false;
			w->frame_inflight = true;
		}
	async = srv->output->present(srv, &region);
	pixman_region32_fini(&region);
	if (async)
		srv->present_pending = true;
	else
		compositor_present_done(srv);
}

/* ---- snapshots ---- */

/* A memfd holding the current screen as XRGB8888. */
int
snapshot_memfd(struct ws_server *srv, uint32_t *stride_out)
{
	uint32_t stride = srv->width * 4;
	size_t size = (size_t)stride * srv->height;
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
	dst = pixman_image_create_bits(PIXMAN_x8r8g8b8, (int)srv->width,
	    (int)srv->height, p, (int)stride);
	if (dst != NULL) {
		pixman_image_composite32(PIXMAN_OP_SRC, srv->shadow, NULL, dst,
		    0, 0, 0, 0, 0, 0, srv->width, srv->height);
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
	row = malloc((size_t)srv->width * 3);
	if (row == NULL) {
		fclose(f);
		return -1;
	}
	px = pixman_image_get_data(srv->shadow);
	stride = (uint32_t)pixman_image_get_stride(srv->shadow) / 4;
	fprintf(f, "P6\n%u %u\n255\n", srv->width, srv->height);
	for (y = 0; y < srv->height; y++) {
		for (x = 0; x < srv->width; x++) {
			uint32_t p = px[y * stride + x];

			row[x * 3] = (unsigned char)(p >> 16);
			row[x * 3 + 1] = (unsigned char)(p >> 8);
			row[x * 3 + 2] = (unsigned char)p;
		}
		fwrite(row, 3, srv->width, f);
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
