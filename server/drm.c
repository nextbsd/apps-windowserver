/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM/KMS output: legacy modesetting with two dumb buffers and page
 * flips, plus the hardware cursor plane when the driver has one.
 *
 * With the pixman renderer the screen is a shadow image in system
 * memory; present() copies only damaged rectangles into the back buffer
 * (each dumb buffer remembers what it missed), then flips. Reading from
 * dumb buffers, which may be write-combined, is avoided. A renderer with
 * its own scanout buffers (GLES on GBM) hands present() a framebuffer
 * to flip to instead.
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <xf86drm.h>
#include <xf86drmMode.h>

#include "server.h"

struct drm_fb {
	uint32_t	handle;
	uint32_t	fb_id;
	uint32_t	pitch;
	uint64_t	size;
	void		*map;
	pixman_image_t	*image;
	pixman_region32_t missing;	/* damage not yet copied here */
};

struct drm_output {
	int		fd;
	char		path[64];
	uint32_t	connector_id;
	uint32_t	crtc_id;
	drmModeModeInfo	mode;
	drmModeCrtc	*saved_crtc;
	struct drm_fb	fb[2];
	int		front;
	bool		flip_ok;
	bool		mode_set;
	/* hardware cursor */
	uint32_t	cursor_handle;
	uint32_t	cursor_w;
	uint32_t	cursor_h;
	uint64_t	cursor_size;
	void		*cursor_map;
	bool		cursor_ok;
	bool		cursor_shown;
	int		cursor_hot_x;
	int		cursor_hot_y;
};

static struct drm_output drm;

/* ---- device selection ---- */

/* DisplayPanelMode: "Preferred", or "WxH" / "WxH@Hz" naming one of the
   connector's modes. */
static const char *panel_mode_want;

static void
pick_mode(drmModeConnector *conn, struct drm_output *out)
{
	unsigned w = 0, h = 0, hz = 0;
	int j, n;

	out->mode = conn->modes[0];
	for (j = 0; j < conn->count_modes; j++)
		if (conn->modes[j].type & DRM_MODE_TYPE_PREFERRED) {
			out->mode = conn->modes[j];
			break;
		}
	if (panel_mode_want == NULL ||
	    strcasecmp(panel_mode_want, "Preferred") == 0)
		return;
	n = sscanf(panel_mode_want, "%ux%u@%u", &w, &h, &hz);
	if (n < 2) {
		ws_warn("DisplayPanelMode %s: not WxH[@Hz]", panel_mode_want);
		return;
	}
	for (j = 0; j < conn->count_modes; j++)
		if (conn->modes[j].hdisplay == w &&
		    conn->modes[j].vdisplay == h &&
		    (n < 3 || conn->modes[j].vrefresh == hz)) {
			out->mode = conn->modes[j];
			return;
		}
	ws_warn("DisplayPanelMode %s: the display has no such mode; using "
	    "%ux%u", panel_mode_want, out->mode.hdisplay, out->mode.vdisplay);
}

static bool
pick_connector(int fd, struct drm_output *out)
{
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;
	uint32_t crtc = 0;
	int i, j, k;
	bool found = false;

	res = drmModeGetResources(fd);
	if (res == NULL)
		return false;
	for (i = 0; i < res->count_connectors && !found; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (conn == NULL)
			continue;
		if (conn->connection != DRM_MODE_CONNECTED ||
		    conn->count_modes == 0) {
			drmModeFreeConnector(conn);
			continue;
		}
		/* DisplayPanelMode, else the preferred mode, else the
		   first one */
		pick_mode(conn, out);
		/* the CRTC already driving it, else any compatible one */
		if (conn->encoder_id != 0 &&
		    (enc = drmModeGetEncoder(fd, conn->encoder_id)) != NULL) {
			crtc = enc->crtc_id;
			drmModeFreeEncoder(enc);
		}
		for (j = 0; crtc == 0 && j < conn->count_encoders; j++) {
			enc = drmModeGetEncoder(fd, conn->encoders[j]);
			if (enc == NULL)
				continue;
			for (k = 0; k < res->count_crtcs; k++)
				if (enc->possible_crtcs & (1u << k)) {
					crtc = res->crtcs[k];
					break;
				}
			drmModeFreeEncoder(enc);
		}
		if (crtc != 0) {
			out->connector_id = conn->connector_id;
			out->crtc_id = crtc;
			found = true;
		}
		drmModeFreeConnector(conn);
	}
	drmModeFreeResources(res);
	return found;
}

static int
open_card(const char *path, struct drm_output *out)
{
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (!pick_connector(fd, out)) {
		close(fd);
		return -1;
	}
	strlcpy(out->path, path, sizeof(out->path));
	return fd;
}

/* ---- dumb buffers ---- */

static int
fb_create(int fd, struct drm_fb *fb, uint32_t w, uint32_t h)
{
	uint64_t offset;

	memset(fb, 0, sizeof(*fb));
	pixman_region32_init(&fb->missing);
	if (drmModeCreateDumbBuffer(fd, w, h, 32, 0, &fb->handle, &fb->pitch,
	    &fb->size) != 0)
		return -1;
	if (drmModeAddFB(fd, w, h, 24, 32, fb->pitch, fb->handle,
	    &fb->fb_id) != 0)
		return -1;
	if (drmModeMapDumbBuffer(fd, fb->handle, &offset) != 0)
		return -1;
	fb->map = mmap(NULL, fb->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
	    (off_t)offset);
	if (fb->map == MAP_FAILED) {
		fb->map = NULL;
		return -1;
	}
	fb->image = pixman_image_create_bits(PIXMAN_x8r8g8b8, (int)w, (int)h,
	    fb->map, (int)fb->pitch);
	return fb->image != NULL ? 0 : -1;
}

static void
fb_destroy(int fd, struct drm_fb *fb)
{
	if (fb->image != NULL)
		pixman_image_unref(fb->image);
	if (fb->map != NULL)
		munmap(fb->map, fb->size);
	if (fb->fb_id != 0)
		drmModeRmFB(fd, fb->fb_id);
	if (fb->handle != 0)
		drmModeDestroyDumbBuffer(fd, fb->handle);
	pixman_region32_fini(&fb->missing);
	memset(fb, 0, sizeof(*fb));
}

static void
fb_copy(struct ws_server *srv, struct drm_fb *fb)
{
	pixman_box32_t *ext;

	if (srv->out == NULL || !pixman_region32_not_empty(&fb->missing))
		return;
	pixman_image_set_clip_region32(fb->image, &fb->missing);
	ext = pixman_region32_extents(&fb->missing);
	pixman_image_composite32(PIXMAN_OP_SRC, srv->out, NULL, fb->image,
	    ext->x1, ext->y1, 0, 0, ext->x1, ext->y1, ext->x2 - ext->x1,
	    ext->y2 - ext->y1);
	pixman_image_set_clip_region32(fb->image, NULL);
	pixman_region32_clear(&fb->missing);
}

static int
modeset(struct ws_server *srv)
{
	struct drm_fb *fb = &drm.fb[drm.front];
	int r;

	pixman_region32_union_rect(&fb->missing, &fb->missing, 0, 0,
	    srv->panel_width, srv->panel_height);
	fb_copy(srv, fb);
	r = drmModeSetCrtc(drm.fd, drm.crtc_id, fb->fb_id, 0, 0,
	    &drm.connector_id, 1, &drm.mode);
	if (r != 0) {
		ws_err("drmModeSetCrtc: %s", strerror(-r));
		return -1;
	}
	drm.mode_set = true;
	return 0;
}

/* ---- page flips ---- */

static void
flip_handler(int fd, unsigned int seq, unsigned int sec, unsigned int usec,
    void *data)
{
	struct ws_server *srv = data;

	(void)fd;
	(void)seq;
	(void)sec;
	(void)usec;
	if (srv->renderer->presented != NULL)
		srv->renderer->presented(srv);
	else
		drm.front ^= 1;
	compositor_present_done(srv);
}

void
drm_readable(struct ws_server *srv)
{
	drmEventContext ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.version = 2;
	ctx.page_flip_handler = flip_handler;
	(void)srv;
	drmHandleEvent(drm.fd, &ctx);
}

/* Show framebuffer fb_id: a flip if the driver can, else SetCrtc. */
static bool
show(struct ws_server *srv, uint32_t fb_id)
{
	int r;

	if (drm.flip_ok) {
		r = drmModePageFlip(drm.fd, drm.crtc_id, fb_id,
		    DRM_MODE_PAGE_FLIP_EVENT, srv);
		if (r == 0)
			return true;	/* flip_handler finishes the frame */
		ws_warn("drmModePageFlip: %s; using SetCrtc", strerror(-r));
		drm.flip_ok = false;
	}
	r = drmModeSetCrtc(drm.fd, drm.crtc_id, fb_id, 0, 0,
	    &drm.connector_id, 1, &drm.mode);
	if (r != 0)
		ws_warn("drmModeSetCrtc: %s", strerror(-r));
	else if (srv->renderer->presented != NULL)
		srv->renderer->presented(srv);
	else
		drm.front ^= 1;
	return false;
}

static bool
drm_present(struct ws_server *srv, pixman_region32_t *damage)
{
	struct drm_fb *back = &drm.fb[drm.front ^ 1];
	uint32_t fb_id;

	if (srv->renderer->scanout != NULL)
		return srv->renderer->scanout(srv, &fb_id) &&
		    show(srv, fb_id);
	pixman_region32_union(&drm.fb[0].missing, &drm.fb[0].missing, damage);
	pixman_region32_union(&drm.fb[1].missing, &drm.fb[1].missing, damage);
	fb_copy(srv, back);
	return show(srv, back->fb_id);
}

/* ---- hardware cursor ---- */

static void
cursor_setup(void)
{
	uint64_t w = 64, h = 64, offset;
	uint32_t pitch;

	drmGetCap(drm.fd, DRM_CAP_CURSOR_WIDTH, &w);
	drmGetCap(drm.fd, DRM_CAP_CURSOR_HEIGHT, &h);
	if (w == 0 || h == 0 || w > 256 || h > 256)
		w = h = 64;
	drm.cursor_w = (uint32_t)w;
	drm.cursor_h = (uint32_t)h;
	if (drmModeCreateDumbBuffer(drm.fd, drm.cursor_w, drm.cursor_h, 32, 0,
	    &drm.cursor_handle, &pitch, &drm.cursor_size) != 0)
		return;
	if (pitch != drm.cursor_w * 4 ||
	    drmModeMapDumbBuffer(drm.fd, drm.cursor_handle, &offset) != 0)
		goto fail;
	drm.cursor_map = mmap(NULL, drm.cursor_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, drm.fd, (off_t)offset);
	if (drm.cursor_map == MAP_FAILED) {
		drm.cursor_map = NULL;
		goto fail;
	}
	drm.cursor_ok = true;
	return;
fail:
	drmModeDestroyDumbBuffer(drm.fd, drm.cursor_handle);
	drm.cursor_handle = 0;
}

static bool
drm_cursor_set(struct ws_server *srv, pixman_image_t *img, int hot_x,
    int hot_y)
{
	pixman_image_t *dst;
	int r;

	(void)srv;
	if (!drm.cursor_ok ||
	    pixman_image_get_width(img) > (int)drm.cursor_w ||
	    pixman_image_get_height(img) > (int)drm.cursor_h)
		return false;
	memset(drm.cursor_map, 0, drm.cursor_size);
	dst = pixman_image_create_bits(PIXMAN_a8r8g8b8, (int)drm.cursor_w,
	    (int)drm.cursor_h, drm.cursor_map, (int)drm.cursor_w * 4);
	if (dst == NULL)
		return false;
	pixman_image_composite32(PIXMAN_OP_SRC, img, NULL, dst, 0, 0, 0, 0,
	    0, 0, pixman_image_get_width(img), pixman_image_get_height(img));
	pixman_image_unref(dst);
	r = drmModeSetCursor2(drm.fd, drm.crtc_id, drm.cursor_handle,
	    drm.cursor_w, drm.cursor_h, hot_x, hot_y);
	if (r != 0)
		r = drmModeSetCursor(drm.fd, drm.crtc_id, drm.cursor_handle,
		    drm.cursor_w, drm.cursor_h);
	if (r != 0) {
		ws_warn("hardware cursor unavailable (%s); using software",
		    strerror(-r));
		drm.cursor_ok = false;
		return false;
	}
	drm.cursor_hot_x = hot_x;
	drm.cursor_hot_y = hot_y;
	drm.cursor_shown = true;
	return true;
}

static void
drm_cursor_move(struct ws_server *srv, int x, int y)
{
	(void)srv;
	if (drm.cursor_shown)
		drmModeMoveCursor(drm.fd, drm.crtc_id, x - drm.cursor_hot_x,
		    y - drm.cursor_hot_y);
}

static void
drm_cursor_hide(struct ws_server *srv)
{
	(void)srv;
	if (drm.cursor_shown)
		drmModeSetCursor(drm.fd, drm.crtc_id, 0, 0, 0);
	drm.cursor_shown = false;
}

/* ---- shutdown: the console's CRTC back, DRM master dropped ---- */

static void
drm_shutdown(struct ws_server *srv)
{
	(void)srv;
	if (drm.fd < 0)
		return;
	drm_cursor_hide(srv);
	if (drm.saved_crtc != NULL) {
		drmModeSetCrtc(drm.fd, drm.saved_crtc->crtc_id,
		    drm.saved_crtc->buffer_id, drm.saved_crtc->x,
		    drm.saved_crtc->y, &drm.connector_id, 1,
		    &drm.saved_crtc->mode);
		drmModeFreeCrtc(drm.saved_crtc);
		drm.saved_crtc = NULL;
	}
	fb_destroy(drm.fd, &drm.fb[0]);
	fb_destroy(drm.fd, &drm.fb[1]);
	if (drm.cursor_map != NULL)
		munmap(drm.cursor_map, drm.cursor_size);
	if (drm.cursor_handle != 0)
		drmModeDestroyDumbBuffer(drm.fd, drm.cursor_handle);
	drmDropMaster(drm.fd);
	close(drm.fd);
	drm.fd = -1;
}

const struct ws_output_ops drm_output_ops = {
	.name = "drm",
	.present = drm_present,
	.cursor_set = drm_cursor_set,
	.cursor_move = drm_cursor_move,
	.cursor_hide = drm_cursor_hide,
	.shutdown = drm_shutdown,
};

/*
 * Open the card that has a connected display (or cfg.drm_device),
 * become DRM master and set the preferred mode. Called as root,
 * before privileges are dropped; the same process keeps master.
 *
 * TODO: wait for the GPU kext's IOKit match before probing
 * (nextbsd/nextbsd#390), and connector hotplug through IOKit.
 */
int
drm_open(struct ws_server *srv)
{
	char path[64];
	uint64_t cap = 0;
	int i;

	memset(&drm, 0, sizeof(drm));
	drm.fd = -1;
	panel_mode_want = srv->cfg.display_panel_mode;
	if (srv->cfg.drm_device[0] != '\0') {
		drm.fd = open_card(srv->cfg.drm_device, &drm);
	} else {
		for (i = 0; i < 16 && drm.fd < 0; i++) {
			snprintf(path, sizeof(path), "/dev/dri/card%d", i);
			drm.fd = open_card(path, &drm);
		}
	}
	if (drm.fd < 0) {
		ws_err("no DRM device with a connected display");
		return -1;
	}
	if (drmSetMaster(drm.fd) != 0 && !drmIsMaster(drm.fd)) {
		ws_err("%s: can't become DRM master (is Xorg running?)",
		    drm.path);
		close(drm.fd);
		drm.fd = -1;
		return -1;
	}
	if (drmGetCap(drm.fd, DRM_CAP_DUMB_BUFFER, &cap) != 0 || cap == 0) {
		ws_err("%s: no dumb buffer support", drm.path);
		goto fail;
	}
	srv->panel_width = drm.mode.hdisplay;
	srv->panel_height = drm.mode.vdisplay;
	srv->refresh_mhz = drm.mode.vrefresh * 1000;
	snprintf(srv->screen_name, sizeof(srv->screen_name), "%s", drm.mode.name);
	{
		drmModeConnector *conn = drmModeGetConnector(drm.fd,
		    drm.connector_id);
		drmModeObjectProperties *props;
		drmModePropertyRes *pr;
		drmModePropertyBlobRes *blob;
		uint32_t k;

		if (conn != NULL) {
			srv->mm_width = conn->mmWidth;
			srv->mm_height = conn->mmHeight;
			srv->screen_internal =
			    conn->connector_type == DRM_MODE_CONNECTOR_eDP ||
			    conn->connector_type == DRM_MODE_CONNECTOR_LVDS ||
			    conn->connector_type == DRM_MODE_CONNECTOR_DSI;
			drmModeFreeConnector(conn);
		}
		/* the EDID: which display it is, and if its size is real
		   (display.c) */
		props = drmModeObjectGetProperties(drm.fd, drm.connector_id,
		    DRM_MODE_OBJECT_CONNECTOR);
		for (k = 0; props != NULL && k < props->count_props; k++) {
			pr = drmModeGetProperty(drm.fd, props->props[k]);
			if (pr == NULL)
				continue;
			if (strcmp(pr->name, "EDID") == 0 &&
			    props->prop_values[k] != 0 &&
			    (blob = drmModeGetPropertyBlob(drm.fd,
			    (uint32_t)props->prop_values[k])) != NULL) {
				display_set_edid(srv, blob->data, blob->length);
				drmModeFreePropertyBlob(blob);
			}
			drmModeFreeProperty(pr);
		}
		if (props != NULL)
			drmModeFreeObjectProperties(props);
	}
	drm.saved_crtc = drmModeGetCrtc(drm.fd, drm.crtc_id);
	if (fb_create(drm.fd, &drm.fb[0], srv->panel_width,
	    srv->panel_height) < 0 ||
	    fb_create(drm.fd, &drm.fb[1], srv->panel_width,
	    srv->panel_height) < 0) {
		ws_err("%s: can't create dumb buffers", drm.path);
		goto fail;
	}
	drm.flip_ok = true;
	if (srv->cfg.hardware_cursor)
		cursor_setup();
#ifdef WS_HAVE_GLES
	/* As root: EGL opens the render node (render_gles.c). */
	if (gles_open(srv, drm.fd))
		srv->renderer = &gles_renderer_ops;
#endif
	srv->output = &drm_output_ops;
	ws_info("%s: %ux%u@%u connector %u crtc %u%s", drm.path,
	    srv->panel_width, srv->panel_height, drm.mode.vrefresh, drm.connector_id, drm.crtc_id,
	    drm.cursor_ok ? ", hardware cursor" : "");
	return drm.fd;
fail:
	drm_shutdown(srv);
	return -1;
}

/* The first modeset happens once the compositor has a shadow image. */
int
drm_start(struct ws_server *srv)
{
	ws_kq_add_fd(srv, drm.fd, EVFILT_READ, (void *)(uintptr_t)SRC_DRM);
	return modeset(srv);
}
