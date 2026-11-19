/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * DRM/KMS output: legacy modesetting, a CRTC per connected display with
 * two dumb buffers and page flips, plus the hardware cursor plane when
 * the driver has one.
 *
 * With the pixman renderer each display shows its view (display.c): its
 * part of the shadow image, or that part scaled to its panel. present()
 * copies only damaged rectangles into the back buffer (each dumb buffer
 * remembers what it missed), then flips. Reading from dumb buffers,
 * which may be write-combined, is avoided. A renderer with its own
 * scanout buffers (GLES on GBM) hands present() a framebuffer per
 * display to flip to instead.
 *
 * Displays come and go (E40 T1): connector hotplug reaches userland as a
 * udev event of the drm subsystem (libudev is backed by IOKit on
 * NextBSD, which hears the kernel's connector events), and every couple
 * of seconds the connectors' last known states are checked as well, in
 * case an event was missed. Either way, the connectors are probed again
 * and display.c lays the desktop out anew.
 *
 * WS_FLIP_STATS=1 logs, every 5 seconds of flipping, how evenly the
 * flips reached the screen (flip_stats), which is how smooth a window
 * drag looks: the GPU-priority measure of #74.
 */

#include <sys/types.h>
#include <sys/event.h>
#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <libudev.h>
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

/* A display's CRTC, buffers and cursor (d->output). */
struct drm_output {
	uint32_t	connector_id;
	int		connection;	/* DRM_MODE_CONNECTED... last seen */
	uint32_t	crtc_id;
	uint32_t	crtc_index;
	drmModeModeInfo	mode;
	drmModeCrtc	*saved_crtc;
	struct drm_fb	fb[2];
	int		front;
	bool		flip_ok;
	bool		mode_set;
	uint32_t	gamma_size;
	uint32_t	dpms_prop;	/* the connector's DPMS property, 0 =
					   none */
	/* hardware cursor */
	uint32_t	cursor_handle;
	uint64_t	cursor_size;
	void		*cursor_map;
	bool		cursor_ok;
	bool		cursor_shown;
	int		cursor_hot_x;	/* panel pixels */
	int		cursor_hot_y;
};

static struct {
	int		fd;
	char		path[64];
	uint32_t	crtcs_used;	/* by index */
	uint32_t	cursor_w;
	uint32_t	cursor_h;
	bool		cursor_cap;
	struct udev_monitor *monitor;
	/* every connector's state as last probed */
	struct { uint32_t id; int connection; } seen[32];
	unsigned nseen;
	/* the hardware cursor's image, its scale and hot spot (points) */
	pixman_image_t	*cursor_img;
	uint32_t	cursor_scale;
	double		cursor_hx;
	double		cursor_hy;
	bool		cursor_on;
} drm = { .fd = -1 };

#define OUT(d)	((struct drm_output *)(d)->output)

/* ---- connectors ---- */

static const char *
connector_type_name(uint32_t t)
{
	switch (t) {
	case DRM_MODE_CONNECTOR_VGA:		return "VGA";
	case DRM_MODE_CONNECTOR_DVII:		return "DVI-I";
	case DRM_MODE_CONNECTOR_DVID:		return "DVI-D";
	case DRM_MODE_CONNECTOR_DVIA:		return "DVI-A";
	case DRM_MODE_CONNECTOR_LVDS:		return "LVDS";
	case DRM_MODE_CONNECTOR_DisplayPort:	return "DP";
	case DRM_MODE_CONNECTOR_HDMIA:		return "HDMI-A";
	case DRM_MODE_CONNECTOR_HDMIB:		return "HDMI-B";
	case DRM_MODE_CONNECTOR_eDP:		return "eDP";
	case DRM_MODE_CONNECTOR_VIRTUAL:	return "Virtual";
	case DRM_MODE_CONNECTOR_DSI:		return "DSI";
	case DRM_MODE_CONNECTOR_DPI:		return "DPI";
	default:				return "Unknown";
	}
}

static struct ws_display *
display_of_connector(struct ws_server *srv, uint32_t id)
{
	unsigned i;

	for (i = 0; i < srv->ndisplays; i++)
		if (srv->displays[i]->output != NULL &&
		    OUT(srv->displays[i])->connector_id == id)
			return srv->displays[i];
	return NULL;
}

/* The connector's EDID into d (display.c reads it). */
static void
read_edid(int fd, uint32_t connector_id, struct ws_display *d)
{
	drmModeObjectProperties *props;
	drmModePropertyRes *pr;
	drmModePropertyBlobRes *blob;
	uint32_t k;

	props = drmModeObjectGetProperties(fd, connector_id,
	    DRM_MODE_OBJECT_CONNECTOR);
	for (k = 0; props != NULL && k < props->count_props; k++) {
		pr = drmModeGetProperty(fd, props->props[k]);
		if (pr == NULL)
			continue;
		if (strcmp(pr->name, "EDID") == 0 &&
		    props->prop_values[k] != 0 &&
		    (blob = drmModeGetPropertyBlob(fd,
		    (uint32_t)props->prop_values[k])) != NULL) {
			display_set_edid(d, blob->data, blob->length);
			drmModeFreePropertyBlob(blob);
		}
		drmModeFreeProperty(pr);
	}
	if (props != NULL)
		drmModeFreeObjectProperties(props);
}

static uint32_t
mode_mhz(const drmModeModeInfo *m)
{
	uint64_t num, den;

	/* the exact rate, as clock / total pixels, when the timings say */
	if (m->htotal != 0 && m->vtotal != 0) {
		num = (uint64_t)m->clock * 1000000ULL;
		den = (uint64_t)m->htotal * m->vtotal;
		if (m->flags & DRM_MODE_FLAG_INTERLACE)
			num *= 2;
		if (m->flags & DRM_MODE_FLAG_DBLSCAN)
			den *= 2;
		if (m->vscan > 1)
			den *= m->vscan;
		return (uint32_t)((num + den / 2) / den);
	}
	return m->vrefresh * 1000;
}

/* d's modes from the connector. */
static void
read_modes(drmModeConnector *conn, struct ws_display *d)
{
	int j;

	d->npanel = 0;
	for (j = 0; j < conn->count_modes && d->npanel < WS_MAX_PANEL_MODES;
	    j++) {
		struct ws_panel_mode *pm = &d->panel[d->npanel++];

		pm->w = conn->modes[j].hdisplay;
		pm->h = conn->modes[j].vdisplay;
		pm->mhz = mode_mhz(&conn->modes[j]);
		pm->preferred = (conn->modes[j].type &
		    DRM_MODE_TYPE_PREFERRED) != 0;
		pm->interlaced = (conn->modes[j].flags &
		    DRM_MODE_FLAG_INTERLACE) != 0;
	}
}

/* A new display for a connected connector. */
static struct ws_display *
display_for(struct ws_server *srv, drmModeConnector *conn)
{
	struct ws_display *d;
	struct drm_output *o;

	d = display_add(srv);
	if (d == NULL)
		return NULL;
	o = calloc(1, sizeof(*o));
	if (o == NULL) {
		display_remove(srv, d);
		return NULL;
	}
	pixman_region32_init(&o->fb[0].missing);
	pixman_region32_init(&o->fb[1].missing);
	o->connector_id = conn->connector_id;
	o->connection = conn->connection;
	d->output = o;
	snprintf(d->connector, sizeof(d->connector), "%s-%u",
	    connector_type_name(conn->connector_type),
	    conn->connector_type_id);
	d->builtin = conn->connector_type == DRM_MODE_CONNECTOR_eDP ||
	    conn->connector_type == DRM_MODE_CONNECTOR_LVDS ||
	    conn->connector_type == DRM_MODE_CONNECTOR_DSI;
	d->mm_width = conn->mmWidth;
	d->mm_height = conn->mmHeight;
	read_edid(drm.fd, conn->connector_id, d);
	{
		drmModeObjectProperties *props;
		drmModePropertyRes *pr;
		uint32_t k;

		props = drmModeObjectGetProperties(drm.fd, conn->connector_id,
		    DRM_MODE_OBJECT_CONNECTOR);
		for (k = 0; props != NULL && k < props->count_props; k++) {
			pr = drmModeGetProperty(drm.fd, props->props[k]);
			if (pr == NULL)
				continue;
			if (strcmp(pr->name, "DPMS") == 0)
				o->dpms_prop = pr->prop_id;
			drmModeFreeProperty(pr);
		}
		if (props != NULL)
			drmModeFreeObjectProperties(props);
	}
	if (d->edid_id[0] == '\0')
		snprintf(d->edid_id, sizeof(d->edid_id), "unknown-%s",
		    d->connector);
	read_modes(conn, d);
	ws_info("display %s connected: %u modes%s", d->connector, d->npanel,
	    d->builtin ? ", built in" : "");
	return d;
}

/*
 * Probe the connectors: a display for each newly connected one, and
 * displays whose connector went away removed. Returns whether anything
 * changed. probe: ask the driver to detect again (slow on some), else
 * take what it last knew.
 */
static bool
probe(struct ws_server *srv, bool force_probe)
{
	drmModeRes *res;
	drmModeConnector *conn;
	struct ws_display *d;
	bool seen[WS_MAX_DISPLAYS], changed = false;
	unsigned i;
	int c;

	res = drmModeGetResources(drm.fd);
	if (res == NULL)
		return false;
	memset(seen, 0, sizeof(seen));
	drm.nseen = 0;
	for (c = 0; c < res->count_connectors; c++) {
		conn = force_probe ? drmModeGetConnector(drm.fd,
		    res->connectors[c]) : drmModeGetConnectorCurrent(drm.fd,
		    res->connectors[c]);
		if (conn == NULL)
			continue;
		if (drm.nseen < sizeof(drm.seen) / sizeof(drm.seen[0])) {
			drm.seen[drm.nseen].id = conn->connector_id;
			drm.seen[drm.nseen].connection = conn->connection;
			drm.nseen++;
		}
		d = display_of_connector(srv, conn->connector_id);
		if (conn->connection == DRM_MODE_CONNECTED &&
		    conn->count_modes == 0 && !force_probe) {
			/* connected, but its modes aren't known yet */
			drmModeFreeConnector(conn);
			conn = drmModeGetConnector(drm.fd, res->connectors[c]);
			if (conn == NULL)
				continue;
		}
		if (conn->connection == DRM_MODE_CONNECTED &&
		    conn->count_modes > 0) {
			if (d == NULL) {
				d = display_for(srv, conn);
				changed = changed || d != NULL;
			} else if (force_probe) {
				read_modes(conn, d);
			}
		}
		if (d != NULL && conn->connection == DRM_MODE_CONNECTED &&
		    conn->count_modes > 0)
			for (i = 0; i < srv->ndisplays; i++)
				if (srv->displays[i] == d)
					seen[i] = true;
		drmModeFreeConnector(conn);
	}
	drmModeFreeResources(res);
	/* the ones that went */
	for (i = srv->ndisplays; i-- > 0;) {
		d = srv->displays[i];
		if (seen[i] || d->output == NULL)
			continue;
		ws_info("display %s disconnected", d->connector);
		if (d->on && srv->running)
			drm_output_ops.display_off(srv, d);
		if (srv->renderer != NULL && srv->renderer->display_gone !=
		    NULL)
			srv->renderer->display_gone(srv, d);
		pixman_region32_fini(&OUT(d)->fb[0].missing);
		pixman_region32_fini(&OUT(d)->fb[1].missing);
		free(d->output);
		d->output = NULL;
		display_remove(srv, d);
		changed = true;
	}
	return changed;
}

/* A connector changed (hotplug): probe, and lay the desktop out again. */
static void
hotplug(struct ws_server *srv)
{
	if (drm.fd < 0)
		return;
	if (!probe(srv, true))
		return;
	ws_info("displays changed: %u connected", srv->ndisplays);
	(void)display_apply(srv, true);
}

void
drm_hotplug_readable(struct ws_server *srv)
{
	struct udev_device *dev;
	const char *hp;
	bool any = false;

	if (drm.monitor == NULL)
		return;
	while ((dev = udev_monitor_receive_device(drm.monitor)) != NULL) {
		hp = udev_device_get_property_value(dev, "HOTPLUG");
		if (hp != NULL && strcmp(hp, "1") == 0)
			any = true;
		udev_device_unref(dev);
	}
	if (any)
		hotplug(srv);
}

/*
 * Every couple of seconds: has a connector changed state without an
 * event? Only what the driver last knew is asked (no probe), so this is
 * cheap; hotplug() probes when it has.
 */
void
drm_housekeeping(struct ws_server *srv)
{
	drmModeRes *res;
	drmModeConnector *conn;
	bool changed = false;
	unsigned k;
	int c;

	if (drm.fd < 0)
		return;
	res = drmModeGetResources(drm.fd);
	if (res == NULL)
		return;
	for (c = 0; c < res->count_connectors && !changed; c++) {
		conn = drmModeGetConnectorCurrent(drm.fd, res->connectors[c]);
		if (conn == NULL)
			continue;
		for (k = 0; k < drm.nseen; k++)
			if (drm.seen[k].id == conn->connector_id)
				break;
		if (k == drm.nseen ||
		    drm.seen[k].connection != (int)conn->connection)
			changed = true;
		drmModeFreeConnector(conn);
	}
	drmModeFreeResources(res);
	if (changed)
		hotplug(srv);
}

/* ---- dumb buffers ---- */

static int
fb_create(int fd, struct drm_fb *fb, uint32_t w, uint32_t h)
{
	uint64_t offset;

	pixman_region32_fini(&fb->missing);
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
	memset(fb->map, 0, fb->size);	/* black until the first frame */
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
	pixman_region32_clear(&fb->missing);
	fb->image = NULL;
	fb->map = NULL;
	fb->fb_id = fb->handle = 0;
}

static void
fb_copy(struct ws_display *d, struct drm_fb *fb)
{
	pixman_box32_t *ext;

	if (d->view == NULL || fb->image == NULL ||
	    !pixman_region32_not_empty(&fb->missing))
		return;
	pixman_image_set_clip_region32(fb->image, &fb->missing);
	ext = pixman_region32_extents(&fb->missing);
	pixman_image_composite32(PIXMAN_OP_SRC, d->view, NULL, fb->image,
	    ext->x1, ext->y1, 0, 0, ext->x1, ext->y1, ext->x2 - ext->x1,
	    ext->y2 - ext->y1);
	pixman_image_set_clip_region32(fb->image, NULL);
	pixman_region32_clear(&fb->missing);
}

/* Show the front dumb buffer at d's mode, all of d's view in it (copy),
   or as it is (black after drm_display_on). */
static int
modeset(struct ws_server *srv, struct ws_display *d, bool copy)
{
	struct drm_output *o = OUT(d);
	struct drm_fb *fb = &o->fb[o->front];
	int r;

	(void)srv;
	if (copy) {
		pixman_region32_union_rect(&fb->missing, &fb->missing, 0, 0,
		    d->panel_width, d->panel_height);
		fb_copy(d, fb);
	}
	r = drmModeSetCrtc(drm.fd, o->crtc_id, fb->fb_id, 0, 0,
	    &o->connector_id, 1, &o->mode);
	if (r != 0) {
		ws_err("display %s: drmModeSetCrtc: %s", d->connector,
		    strerror(-r));
		return -1;
	}
	o->mode_set = true;
	return 0;
}

/* ---- turning displays on and off (display.c) ---- */

/* A CRTC for the connector: the one driving it already, else a free one
   it can use. */
static bool
pick_crtc(struct drm_output *o)
{
	drmModeRes *res;
	drmModeConnector *conn;
	drmModeEncoder *enc;
	uint32_t crtc = 0, index = 0;
	int j, k;

	res = drmModeGetResources(drm.fd);
	conn = drmModeGetConnector(drm.fd, o->connector_id);
	if (res == NULL || conn == NULL) {
		if (res != NULL)
			drmModeFreeResources(res);
		if (conn != NULL)
			drmModeFreeConnector(conn);
		return false;
	}
	if (conn->encoder_id != 0 &&
	    (enc = drmModeGetEncoder(drm.fd, conn->encoder_id)) != NULL) {
		for (k = 0; k < res->count_crtcs; k++)
			if (res->crtcs[k] == enc->crtc_id &&
			    (drm.crtcs_used & (1u << k)) == 0) {
				crtc = enc->crtc_id;
				index = (uint32_t)k;
			}
		drmModeFreeEncoder(enc);
	}
	for (j = 0; crtc == 0 && j < conn->count_encoders; j++) {
		enc = drmModeGetEncoder(drm.fd, conn->encoders[j]);
		if (enc == NULL)
			continue;
		for (k = 0; k < res->count_crtcs; k++)
			if ((enc->possible_crtcs & (1u << k)) &&
			    (drm.crtcs_used & (1u << k)) == 0) {
				crtc = res->crtcs[k];
				index = (uint32_t)k;
				break;
			}
		drmModeFreeEncoder(enc);
	}
	drmModeFreeConnector(conn);
	drmModeFreeResources(res);
	if (crtc == 0)
		return false;
	o->crtc_id = crtc;
	o->crtc_index = index;
	drm.crtcs_used |= 1u << index;
	return true;
}

/* The DRM mode for d's panel size and rate. */
static bool
find_mode(struct ws_display *d, drmModeModeInfo *out)
{
	drmModeConnector *conn;
	int j, best = -1;

	conn = drmModeGetConnectorCurrent(drm.fd, OUT(d)->connector_id);
	if (conn == NULL)
		return false;
	for (j = 0; j < conn->count_modes; j++) {
		if (conn->modes[j].hdisplay != d->panel_width ||
		    conn->modes[j].vdisplay != d->panel_height)
			continue;
		if (mode_mhz(&conn->modes[j]) == d->refresh_mhz) {
			best = j;
			break;
		}
		if (best < 0 || (conn->modes[j].type &
		    DRM_MODE_TYPE_PREFERRED))
			best = j;
	}
	if (best >= 0)
		*out = conn->modes[best];
	drmModeFreeConnector(conn);
	return best >= 0;
}

static void
cursor_buffer_free(struct drm_output *o)
{
	if (o->cursor_map != NULL)
		munmap(o->cursor_map, o->cursor_size);
	if (o->cursor_handle != 0)
		drmModeDestroyDumbBuffer(drm.fd, o->cursor_handle);
	o->cursor_map = NULL;
	o->cursor_handle = 0;
	o->cursor_ok = false;
}

static void
cursor_buffer_make(struct drm_output *o)
{
	uint64_t offset;
	uint32_t pitch;

	if (!drm.cursor_cap || o->cursor_handle != 0)
		return;
	if (drmModeCreateDumbBuffer(drm.fd, drm.cursor_w, drm.cursor_h, 32, 0,
	    &o->cursor_handle, &pitch, &o->cursor_size) != 0)
		return;
	if (pitch != drm.cursor_w * 4 ||
	    drmModeMapDumbBuffer(drm.fd, o->cursor_handle, &offset) != 0) {
		cursor_buffer_free(o);
		return;
	}
	o->cursor_map = mmap(NULL, o->cursor_size, PROT_READ | PROT_WRITE,
	    MAP_SHARED, drm.fd, (off_t)offset);
	if (o->cursor_map == MAP_FAILED) {
		o->cursor_map = NULL;
		cursor_buffer_free(o);
		return;
	}
	o->cursor_ok = true;
}

static void
drm_display_off(struct ws_server *srv, struct ws_display *d)
{
	struct drm_output *o = OUT(d);

	(void)srv;
	if (o == NULL || o->crtc_id == 0)
		return;
	if (o->cursor_shown)
		drmModeSetCursor(drm.fd, o->crtc_id, 0, 0, 0);
	o->cursor_shown = false;
	if (o->saved_crtc != NULL && srv->running) {
		/* off: no framebuffer, no connector */
		drmModeSetCrtc(drm.fd, o->crtc_id, 0, 0, 0, NULL, 0, NULL);
	}
	if (o->saved_crtc != NULL)
		drmModeFreeCrtc(o->saved_crtc);
	o->saved_crtc = NULL;
	fb_destroy(drm.fd, &o->fb[0]);
	fb_destroy(drm.fd, &o->fb[1]);
	cursor_buffer_free(o);
	drm.crtcs_used &= ~(1u << o->crtc_index);
	o->crtc_id = 0;
	o->mode_set = false;
	d->flip_pending = false;
}

/*
 * Turn d on at its panel mode: a CRTC, two black dumb buffers of the
 * mode's size, and a modeset showing one. The renderer draws the next
 * frame, which the output flips to.
 */
static int
drm_display_on(struct ws_server *srv, struct ws_display *d)
{
	struct drm_output *o = OUT(d);
	drmModeModeInfo mode;

	if (o == NULL)
		return -1;
	if (!find_mode(d, &mode)) {
		ws_err("display %s: no %ux%u mode", d->connector,
		    d->panel_width, d->panel_height);
		return -1;
	}
	if (o->crtc_id != 0 && o->mode_set &&
	    memcmp(&mode, &o->mode, sizeof(mode)) == 0 &&
	    o->fb[0].image != NULL)
		return 0;	/* as it is */
	if (o->crtc_id == 0) {
		if (!pick_crtc(o)) {
			ws_warn("display %s: no CRTC free for it",
			    d->connector);
			return -1;
		}
		o->saved_crtc = drmModeGetCrtc(drm.fd, o->crtc_id);
	}
	/* while the renderer's buffers may still be on screen: they go
	   after this shows the new buffers */
	d->flip_pending = false;
	o->mode = mode;
	if (fb_create(drm.fd, &o->fb[0], d->panel_width,
	    d->panel_height) < 0 || fb_create(drm.fd, &o->fb[1],
	    d->panel_width, d->panel_height) < 0) {
		ws_err("display %s: can't create dumb buffers", d->connector);
		drm_display_off(srv, d);
		return -1;
	}
	o->front = 0;
	o->flip_ok = true;
	o->gamma_size = 0;
	{
		drmModeCrtc *c = drmModeGetCrtc(drm.fd, o->crtc_id);

		if (c != NULL) {
			o->gamma_size = (uint32_t)c->gamma_size;
			drmModeFreeCrtc(c);
		}
	}
	d->gamma_r = d->gamma_g = d->gamma_b = 1.0;	/* set again */
	if (srv->cfg.hardware_cursor)
		cursor_buffer_make(o);
	if (srv->running && modeset(srv, d, false) < 0) {
		drm_display_off(srv, d);
		return -1;
	}
	ws_info("display %s: %ux%u@%u.%02u on crtc %u%s", d->connector,
	    d->panel_width, d->panel_height, d->refresh_mhz / 1000,
	    d->refresh_mhz % 1000 / 10, o->crtc_id,
	    o->cursor_ok ? ", hardware cursor" : "");
	return 0;
}

/* ---- page flips ---- */

/*
 * WS_FLIP_STATS: the gaps between flips, from the kernel's vblank
 * timestamps. A gap over 1.5 refresh periods is a missed vblank ("late");
 * one over 250 ms is the screen sitting idle, not a slow frame, and isn't
 * counted.
 */
static struct {
	int		on;		/* -1: not read yet */
	uint64_t	last, start;	/* usec */
	uint64_t	sum, max;	/* usec */
	unsigned long	n, late;
} flips_seen = { .on = -1 };

static void
flip_stats(struct ws_server *srv, unsigned int sec, unsigned int usec)
{
	uint64_t t, gap, period;

	if (flips_seen.on < 0)
		flips_seen.on = getenv("WS_FLIP_STATS") != NULL;
	if (!flips_seen.on)
		return;
	t = sec != 0 || usec != 0 ? (uint64_t)sec * 1000000 + usec :
	    ws_now_usec();
	if (flips_seen.last != 0 && t > flips_seen.last &&
	    (gap = t - flips_seen.last) < 250000) {
		if (flips_seen.n == 0)
			flips_seen.start = flips_seen.last;
		flips_seen.n++;
		flips_seen.sum += gap;
		if (gap > flips_seen.max)
			flips_seen.max = gap;
		period = srv->refresh_mhz != 0 ?
		    1000000000ULL / srv->refresh_mhz : 16667;
		if (gap * 2 > period * 3)
			flips_seen.late++;
	}
	flips_seen.last = t;
	if (flips_seen.n != 0 && t - flips_seen.start >= 5000000) {
		ws_info("flips: %lu in %.1f s (%.1f/s), gap mean %.1f ms, "
		    "max %.1f ms, %lu late (over 1.5 vblanks)", flips_seen.n,
		    (double)(t - flips_seen.start) / 1e6,
		    flips_seen.n * 1e6 / (double)(t - flips_seen.start),
		    (double)flips_seen.sum / flips_seen.n / 1000.0,
		    (double)flips_seen.max / 1000.0, flips_seen.late);
		flips_seen.n = flips_seen.late = 0;
		flips_seen.sum = flips_seen.max = 0;
	}
}

static struct ws_server *flip_srv;

/* The frame is presented once every display it went to has flipped. */
static void
frame_maybe_done(struct ws_server *srv)
{
	unsigned i;

	for (i = 0; i < srv->ndisplays; i++)
		if (srv->displays[i]->flip_pending)
			return;
	if (srv->present_pending)
		compositor_present_done(srv);
}

static void
flip_handler(int fd, unsigned int seq, unsigned int sec, unsigned int usec,
    void *data)
{
	struct ws_server *srv = flip_srv;
	uint32_t id = (uint32_t)(uintptr_t)data;
	struct ws_display *d;

	(void)fd;
	(void)seq;
	d = display_lookup(srv, id);
	if (d == NULL || !d->flip_pending)
		return;		/* unplugged meanwhile */
	if (d == srv->main)
		flip_stats(srv, sec, usec);
	d->flip_pending = false;
	if (srv->renderer->presented != NULL)
		srv->renderer->presented(srv, d);
	else
		OUT(d)->front ^= 1;
	frame_maybe_done(srv);
}

void
drm_readable(struct ws_server *srv)
{
	drmEventContext ctx;

	memset(&ctx, 0, sizeof(ctx));
	ctx.version = 2;
	ctx.page_flip_handler = flip_handler;
	flip_srv = srv;
	drmHandleEvent(drm.fd, &ctx);
}

/* Show framebuffer fb_id on d: a flip if the driver can (true: one is
   pending), else SetCrtc. */
static bool
show(struct ws_server *srv, struct ws_display *d, uint32_t fb_id)
{
	struct drm_output *o = OUT(d);
	int r;

	if (o->flip_ok && o->mode_set) {
		r = drmModePageFlip(drm.fd, o->crtc_id, fb_id,
		    DRM_MODE_PAGE_FLIP_EVENT, (void *)(uintptr_t)d->id);
		if (r == 0) {
			d->flip_pending = true;
			return true;	/* flip_handler finishes it */
		}
		ws_warn("display %s: drmModePageFlip: %s; using SetCrtc",
		    d->connector, strerror(-r));
		o->flip_ok = false;
	}
	r = drmModeSetCrtc(drm.fd, o->crtc_id, fb_id, 0, 0,
	    &o->connector_id, 1, &o->mode);
	if (r != 0)
		ws_warn("display %s: drmModeSetCrtc: %s", d->connector,
		    strerror(-r));
	else {
		o->mode_set = true;
		if (srv->renderer->presented != NULL)
			srv->renderer->presented(srv, d);
		else
			o->front ^= 1;
	}
	return false;
}

/* Wait (a little) for the flips in flight to finish. */
static void
drm_wait_present(struct ws_server *srv)
{
	struct pollfd p = { .fd = drm.fd, .events = POLLIN };
	int i;

	for (i = 0; i < 10 && srv->present_pending; i++)
		if (poll(&p, 1, 100) > 0)
			drm_readable(srv);
}

/* Show every display's whole view from a dumb buffer (renderer switch). */
static void
drm_take_screen(struct ws_server *srv)
{
	struct ws_display *d;
	unsigned i;

	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!d->on || d->output == NULL || OUT(d)->crtc_id == 0)
			continue;
		OUT(d)->flip_ok = true;
		(void)modeset(srv, d, true);
	}
}

static bool
drm_present(struct ws_server *srv, pixman_region32_t *damage)
{
	struct ws_display *d;
	struct drm_output *o;
	struct drm_fb *back;
	pixman_region32_t panel;
	uint32_t fb_id;
	bool async = false;
	unsigned i;

	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		o = OUT(d);
		if (!d->on || o == NULL || o->crtc_id == 0 || d->flip_pending)
			continue;
		if (srv->renderer->scanout != NULL) {
			if (srv->renderer->scanout(srv, d, &fb_id) &&
			    show(srv, d, fb_id))
				async = true;
			continue;
		}
		display_back_to_panel(d, damage, &panel);
		if (!pixman_region32_not_empty(&panel)) {
			pixman_region32_fini(&panel);
			continue;
		}
		pixman_region32_union(&o->fb[0].missing, &o->fb[0].missing,
		    &panel);
		pixman_region32_union(&o->fb[1].missing, &o->fb[1].missing,
		    &panel);
		pixman_region32_fini(&panel);
		back = &o->fb[o->front ^ 1];
		fb_copy(d, back);
		if (show(srv, d, back->fb_id))
			async = true;
	}
	return async;
}

/* ---- hardware cursor ---- */

/* Panel pixels per point on d. */
static double
display_ppp(const struct ws_display *d)
{
	return d->frame.width > 0 ? (double)d->dest.width / d->frame.width :
	    1.0;
}

/* Load the cursor image into d's buffer, scaled to d's pixels per point. */
static bool
cursor_load(struct ws_display *d)
{
	struct drm_output *o = OUT(d);
	pixman_image_t *dst;
	pixman_transform_t t;
	double f = display_ppp(d) / (drm.cursor_scale ? drm.cursor_scale : 1);
	int w = pixman_image_get_width(drm.cursor_img);
	int h = pixman_image_get_height(drm.cursor_img);
	int dw = (int)lround(w * f), dh = (int)lround(h * f), r;

	if (!o->cursor_ok || dw > (int)drm.cursor_w || dh > (int)drm.cursor_h ||
	    dw <= 0 || dh <= 0)
		return false;
	memset(o->cursor_map, 0, o->cursor_size);
	dst = pixman_image_create_bits(PIXMAN_a8r8g8b8, (int)drm.cursor_w,
	    (int)drm.cursor_h, o->cursor_map, (int)drm.cursor_w * 4);
	if (dst == NULL)
		return false;
	if (fabs(f - 1.0) > 1e-6) {
		pixman_transform_init_scale(&t, pixman_double_to_fixed(1.0 / f),
		    pixman_double_to_fixed(1.0 / f));
		pixman_image_set_transform(drm.cursor_img, &t);
		pixman_image_set_filter(drm.cursor_img,
		    PIXMAN_FILTER_BILINEAR, NULL, 0);
	}
	pixman_image_composite32(PIXMAN_OP_SRC, drm.cursor_img, NULL, dst, 0,
	    0, 0, 0, 0, 0, dw, dh);
	pixman_image_set_transform(drm.cursor_img, NULL);
	pixman_image_set_filter(drm.cursor_img, PIXMAN_FILTER_NEAREST, NULL,
	    0);
	pixman_image_unref(dst);
	o->cursor_hot_x = (int)lround(drm.cursor_hx * display_ppp(d));
	o->cursor_hot_y = (int)lround(drm.cursor_hy * display_ppp(d));
	r = drmModeSetCursor2(drm.fd, o->crtc_id, o->cursor_handle,
	    drm.cursor_w, drm.cursor_h, o->cursor_hot_x, o->cursor_hot_y);
	if (r != 0)
		r = drmModeSetCursor(drm.fd, o->crtc_id, o->cursor_handle,
		    drm.cursor_w, drm.cursor_h);
	if (r != 0) {
		ws_warn("display %s: hardware cursor unavailable (%s)",
		    d->connector, strerror(-r));
		o->cursor_ok = false;
		return false;
	}
	o->cursor_shown = true;
	return true;
}

static void
drm_cursor_move(struct ws_server *srv, double x, double y)
{
	struct ws_display *d, *at;
	struct drm_output *o;
	double px, py;
	unsigned i;

	if (!drm.cursor_on)
		return;
	at = display_at(srv, x, y);
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		o = OUT(d);
		if (!d->on || o == NULL || o->crtc_id == 0)
			continue;
		/* shown where the pointer is, and on that display's
		   mirrors */
		if (at == NULL || (d != at && d->mirror_of != at->id)) {
			if (o->cursor_shown) {
				drmModeSetCursor(drm.fd, o->crtc_id, 0, 0, 0);
				o->cursor_shown = false;
			}
			continue;
		}
		if (!o->cursor_shown && !cursor_load(d))
			continue;
		px = d->dest.x + (x - d->frame.x) * display_ppp(d);
		py = d->dest.y + (y - d->frame.y) * display_ppp(d);
		drmModeMoveCursor(drm.fd, o->crtc_id,
		    (int)floor(px) - o->cursor_hot_x,
		    (int)floor(py) - o->cursor_hot_y);
	}
}

/*
 * The cursor image (scale pixels per point, hot spot in points). True if
 * every display can show it on its cursor plane, scaled to its pixels.
 */
static bool
drm_cursor_set(struct ws_server *srv, pixman_image_t *img, uint32_t scale,
    double hot_x, double hot_y)
{
	struct ws_display *d;
	struct drm_output *o;
	double f;
	unsigned i;

	if (!drm.cursor_cap)
		return false;
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		o = OUT(d);
		if (!d->on || o == NULL)
			continue;
		f = display_ppp(d) / (scale ? scale : 1);
		if (!o->cursor_ok ||
		    lround(pixman_image_get_width(img) * f) >
		    (long)drm.cursor_w ||
		    lround(pixman_image_get_height(img) * f) >
		    (long)drm.cursor_h)
			return false;
	}
	if (drm.cursor_img != NULL)
		pixman_image_unref(drm.cursor_img);
	drm.cursor_img = pixman_image_ref(img);
	drm.cursor_scale = scale;
	drm.cursor_hx = hot_x;
	drm.cursor_hy = hot_y;
	drm.cursor_on = true;
	/* loaded again on each display where it shows */
	for (i = 0; i < srv->ndisplays; i++) {
		o = OUT(srv->displays[i]);
		if (o != NULL)
			o->cursor_shown = false;
	}
	drm_cursor_move(srv, srv->ptr_x, srv->ptr_y);
	return true;
}

static void
drm_cursor_hide(struct ws_server *srv)
{
	struct drm_output *o;
	unsigned i;

	drm.cursor_on = false;
	for (i = 0; i < srv->ndisplays; i++) {
		o = OUT(srv->displays[i]);
		if (o != NULL && o->cursor_shown && o->crtc_id != 0)
			drmModeSetCursor(drm.fd, o->crtc_id, 0, 0, 0);
		if (o != NULL)
			o->cursor_shown = false;
	}
}

/* ---- display sleep ---- */

static void
drm_blank(struct ws_server *srv, struct ws_display *d, bool off)
{
	struct drm_output *o = OUT(d);
	int r;

	(void)srv;
	if (o == NULL || o->crtc_id == 0 || o->dpms_prop == 0)
		return;
	r = drmModeConnectorSetProperty(drm.fd, o->connector_id,
	    o->dpms_prop, off ? DRM_MODE_DPMS_OFF : DRM_MODE_DPMS_ON);
	if (r != 0)
		ws_warn("display %s: DPMS %s: %s", d->connector,
		    off ? "off" : "on", strerror(-r));
	/* a flip asked for just before won't finish while it's off */
	if (off)
		d->flip_pending = false;
}

/* ---- Night Shift ---- */

static void
drm_gamma(struct ws_server *srv, struct ws_display *d, double r, double g,
    double b)
{
	struct drm_output *o = OUT(d);
	uint16_t *red, *green, *blue;
	uint32_t i, n;
	double v;
	int e;

	(void)srv;
	if (o == NULL || o->crtc_id == 0 || o->gamma_size < 2)
		return;
	n = o->gamma_size;
	red = malloc(n * sizeof(uint16_t) * 3);
	if (red == NULL)
		return;
	green = red + n;
	blue = green + n;
	for (i = 0; i < n; i++) {
		v = (double)i / (n - 1) * 65535.0;
		red[i] = (uint16_t)lround(v * r);
		green[i] = (uint16_t)lround(v * g);
		blue[i] = (uint16_t)lround(v * b);
	}
	e = drmModeCrtcSetGamma(drm.fd, o->crtc_id, n, red, green, blue);
	if (e != 0)
		ws_warn("display %s: drmModeCrtcSetGamma: %s", d->connector,
		    strerror(-e));
	free(red);
}

/* ---- shutdown: the console's CRTCs back, DRM master dropped ---- */

static void
drm_shutdown(struct ws_server *srv)
{
	struct ws_display *d;
	struct drm_output *o;
	unsigned i;

	if (drm.fd < 0)
		return;
	drm_cursor_hide(srv);
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		o = OUT(d);
		if (o == NULL)
			continue;
		if (o->saved_crtc != NULL) {
			drmModeSetCrtc(drm.fd, o->saved_crtc->crtc_id,
			    o->saved_crtc->buffer_id, o->saved_crtc->x,
			    o->saved_crtc->y, &o->connector_id, 1,
			    &o->saved_crtc->mode);
			drmModeFreeCrtc(o->saved_crtc);
			o->saved_crtc = NULL;
		}
		if (o->gamma_size >= 2)
			drm_gamma(srv, d, 1.0, 1.0, 1.0);
		fb_destroy(drm.fd, &o->fb[0]);
		fb_destroy(drm.fd, &o->fb[1]);
		cursor_buffer_free(o);
	}
	if (drm.cursor_img != NULL)
		pixman_image_unref(drm.cursor_img);
	drm.cursor_img = NULL;
	if (drm.monitor != NULL)
		udev_monitor_unref(drm.monitor);
	drm.monitor = NULL;
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
	.wait_present = drm_wait_present,
	.take_screen = drm_take_screen,
	.display_on = drm_display_on,
	.display_off = drm_display_off,
	.blank = drm_blank,
	.gamma = drm_gamma,
};

/* ---- opening ---- */

/* Does the card have a connected display? */
static bool
card_connected(int fd)
{
	drmModeRes *res;
	drmModeConnector *conn;
	bool found = false;
	int i;

	res = drmModeGetResources(fd);
	if (res == NULL)
		return false;
	for (i = 0; i < res->count_connectors && !found; i++) {
		conn = drmModeGetConnector(fd, res->connectors[i]);
		if (conn == NULL)
			continue;
		found = conn->connection == DRM_MODE_CONNECTED &&
		    conn->count_modes > 0;
		drmModeFreeConnector(conn);
	}
	drmModeFreeResources(res);
	return found;
}

static int
open_card(const char *path)
{
	int fd;

	fd = open(path, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return -1;
	if (!card_connected(fd)) {
		close(fd);
		return -1;
	}
	strlcpy(drm.path, path, sizeof(drm.path));
	return fd;
}

/*
 * Open the card that has a connected display (or cfg.drm_device),
 * become DRM master, and make a display for each connected connector.
 * Called as root, before privileges are dropped; the same process keeps
 * master. The displays are turned on by drm_start, once the desktop is
 * laid out (display.c).
 *
 * TODO: wait for the GPU kext's IOKit match before probing
 * (nextbsd/nextbsd#390).
 */
int
drm_open(struct ws_server *srv)
{
	char path[64];
	uint64_t cap = 0, w = 64, h = 64;
	int i;

	drm.fd = -1;
	if (srv->cfg.drm_device[0] != '\0') {
		drm.fd = open_card(srv->cfg.drm_device);
	} else {
		for (i = 0; i < 16 && drm.fd < 0; i++) {
			snprintf(path, sizeof(path), "/dev/dri/card%d", i);
			drm.fd = open_card(path);
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
	if (srv->cfg.hardware_cursor) {
		drmGetCap(drm.fd, DRM_CAP_CURSOR_WIDTH, &w);
		drmGetCap(drm.fd, DRM_CAP_CURSOR_HEIGHT, &h);
		if (w == 0 || h == 0 || w > 256 || h > 256)
			w = h = 64;
		drm.cursor_w = (uint32_t)w;
		drm.cursor_h = (uint32_t)h;
		drm.cursor_cap = true;
	}
	(void)probe(srv, true);
	if (srv->ndisplays == 0) {
		ws_err("%s: no connected display", drm.path);
		goto fail;
	}
#ifdef WS_HAVE_GLES
	/* As root: EGL opens the render node (render_gles.c). */
	if (gles_open(srv, drm.fd))
		srv->renderer = &gles_renderer_ops;
#endif
	srv->output = &drm_output_ops;
	ws_info("%s: %u display%s connected", drm.path, srv->ndisplays,
	    srv->ndisplays == 1 ? "" : "s");
	return drm.fd;
fail:
	drm_shutdown(srv);
	return -1;
}

/*
 * The first modesets happen once the compositor has images: every
 * display that is on gets its CRTC, then hotplug events are listened
 * for.
 */
int
drm_start(struct ws_server *srv)
{
	struct ws_display *d;
	unsigned i, n = 0;

	ws_kq_add_fd(srv, drm.fd, EVFILT_READ, (void *)(uintptr_t)SRC_DRM);
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!d->on)
			continue;
		if (drm_display_on(srv, d) < 0 || modeset(srv, d, true) < 0) {
			d->on = false;
			continue;
		}
		n++;
	}
	if (n == 0)
		return -1;
	if (srv->udev == NULL)
		srv->udev = udev_new();
	if (srv->udev != NULL &&
	    (drm.monitor = udev_monitor_new_from_netlink(srv->udev,
	    "udev")) != NULL) {
		udev_monitor_filter_add_match_subsystem_devtype(drm.monitor,
		    "drm", NULL);
		if (udev_monitor_enable_receiving(drm.monitor) == 0 &&
		    udev_monitor_get_fd(drm.monitor) >= 0)
			ws_kq_add_fd(srv, udev_monitor_get_fd(drm.monitor),
			    EVFILT_READ, (void *)(uintptr_t)SRC_HOTPLUG);
		else {
			udev_monitor_unref(drm.monitor);
			drm.monitor = NULL;
		}
	}
	if (drm.monitor == NULL)
		ws_warn("no udev monitor: display hotplug is checked every "
		    "couple of seconds");
	if (n < srv->ndisplays)
		(void)display_apply(srv, true);	/* without the ones that
						   failed */
	return 0;
}
