/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The display mode and HiDPI ("HiDPI" in WSProtocol.h), the way macOS
 * does Retina displays.
 *
 * The screen is laid out in points, width x height: the "looks like"
 * size. The compositor draws into a backing store of scale times that
 * many pixels (srv->shadow), and when the backing store isn't the size
 * of the panel, scales each frame to the panel (srv->panel), as macOS's
 * scaled modes do: "looks like 2048 x 1152" on a 3840 x 2160 panel is
 * drawn at 4096 x 2304 and shown at 3840 x 2160. The default for a
 * HiDPI display is exactly 2x (3840 x 2160 looks like 1920 x 1080).
 *
 * Each display gets a list of "looks like" choices and a default, from
 * its size in pixels and, when the EDID's physical size can be trusted,
 * its pixel density. The DisplayScale setting forces an integer scale;
 * otherwise the user's choice for this display (DisplayLooksLike), kept
 * per display in the display store for every user, wins over the
 * default.
 */

#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "server.h"

/* "Looks like" widths offered between half the panel and all of it. */
static const uint32_t mode_widths[] = {
	1280, 1440, 1680, 1920, 2048, 2560, 3008, 3840, 4096, 5120,
};

/* The largest backing store, pixels on a side. */
#define BACK_MAX	8192

/* ---- EDID ---- */

void
display_set_edid(struct ws_server *srv, const uint8_t *e, size_t len)
{
	static const uint8_t magic[8] = { 0, 0xff, 0xff, 0xff, 0xff, 0xff,
	    0xff, 0 };
	char vendor[4], name[14];
	uint32_t product, serial;
	size_t i, j;

	if (e == NULL || len < 128 || memcmp(e, magic, 8) != 0)
		return;
	vendor[0] = (char)('@' + ((e[8] >> 2) & 0x1f));
	vendor[1] = (char)('@' + (((e[8] & 3) << 3) | (e[9] >> 5)));
	vendor[2] = (char)('@' + (e[9] & 0x1f));
	vendor[3] = '\0';
	product = (uint32_t)e[10] | (uint32_t)e[11] << 8;
	serial = (uint32_t)e[12] | (uint32_t)e[13] << 8 |
	    (uint32_t)e[14] << 16 | (uint32_t)e[15] << 24;
	snprintf(srv->display_id, sizeof(srv->display_id), "%s-%04x-%08x",
	    vendor, product, serial);
	/* the monitor name descriptor */
	for (i = 54; i + 18 <= 126; i += 18) {
		if (e[i] != 0 || e[i + 1] != 0 || e[i + 3] != 0xfc)
			continue;
		for (j = 0; j < 13 && e[i + 5 + j] != '\n'; j++)
			name[j] = isprint(e[i + 5 + j]) ? (char)e[i + 5 + j] :
			    ' ';
		name[j] = '\0';
		while (j > 0 && name[j - 1] == ' ')
			name[--j] = '\0';
		if (j > 0)
			strlcpy(srv->display_name, name,
			    sizeof(srv->display_name));
	}
	/* QEMU (RHT) and VirtualBox (VBX) make their sizes up. */
	if (strcmp(vendor, "RHT") == 0 || strcmp(vendor, "VBX") == 0 ||
	    strcmp(vendor, "QEM") == 0)
		srv->edid_trusted = false;
	else
		srv->edid_trusted = true;
	ws_info("display %s (%s)%s", srv->display_id,
	    srv->display_name[0] ? srv->display_name : "no name",
	    srv->edid_trusted ? "" : ", a virtual machine's: its size is "
	    "made up");
}

/* Is the physical size believable for the panel's pixels? */
static bool
size_trusted(struct ws_server *srv)
{
	double ppi, pa, ma;

	if (!srv->edid_trusted || srv->mm_width < 100 ||
	    srv->mm_height < 60 || srv->mm_width > 1600 ||
	    srv->mm_height > 1000 || srv->panel_width == 0)
		return false;
	ppi = srv->panel_width * 25.4 / srv->mm_width;
	if (ppi < 50 || ppi > 600)
		return false;
	/* square pixels: the shapes agree (projectors and TVs that report
	   an aspect ratio instead don't) */
	pa = (double)srv->panel_width / srv->panel_height;
	ma = (double)srv->mm_width / srv->mm_height;
	return fabs(pa / ma - 1.0) < 0.15;
}

/* ---- the "looks like" choices ---- */

static uint32_t
height_for(struct ws_server *srv, uint32_t w)
{
	uint32_t h = (uint32_t)lround((double)w * srv->panel_height /
	    srv->panel_width);

	return h & ~1u;
}

static void
add_mode(struct ws_server *srv, uint32_t w, uint32_t h)
{
	unsigned i;

	if (w == 0 || h == 0 || srv->nmodes >= WS_MAX_DISPLAY_MODES)
		return;
	for (i = 0; i < srv->nmodes; i++)
		if (srv->modes[i].w == w)
			return;
	srv->modes[srv->nmodes].w = w;
	srv->modes[srv->nmodes].h = h;
	srv->nmodes++;
}

/* Can this panel run at 2x? Half of it must still be a usable screen. */
static bool
hidpi_capable(struct ws_server *srv)
{
	return srv->panel_width >= 2560 && srv->panel_height >= 1400;
}

static void
modes_compute(struct ws_server *srv)
{
	uint32_t pw = srv->panel_width, ph = srv->panel_height;
	double target = 0, ppi = 0, best;
	bool hidpi, trusted = size_trusted(srv);
	unsigned i;

	srv->nmodes = 0;
	srv->mode_default = 0;
	if (hidpi_capable(srv)) {
		add_mode(srv, pw / 2, ph / 2);
		for (i = 0; i < sizeof(mode_widths) / sizeof(mode_widths[0]);
		    i++)
			if (mode_widths[i] > pw / 2 && mode_widths[i] < pw)
				add_mode(srv, mode_widths[i],
				    height_for(srv, mode_widths[i]));
	}
	add_mode(srv, pw, ph);		/* native, 1x */

	if (trusted) {
		ppi = pw * 25.4 / srv->mm_width;
		hidpi = hidpi_capable(srv) && ppi >= 125;
	} else {
		/* no real size (virtual machines): by the pixels alone,
		   a 4K screen or larger is HiDPI */
		hidpi = pw >= 3840 && ph >= 2160;
	}
	/* As macOS: exactly 2x, a 3840 x 2160 display looking like
	   1920 x 1080; the scaled sizes are the user's to pick. */
	target = pw / 2.0;
	if (!hidpi) {
		srv->mode_default = srv->nmodes - 1;
	} else {
		if (target < pw / 2.0)
			target = pw / 2.0;
		best = 1e9;
		for (i = 0; i + 1 < srv->nmodes; i++)
			if (fabs(srv->modes[i].w - target) < best) {
				best = fabs(srv->modes[i].w - target);
				srv->mode_default = i;
			}
	}
	ws_info("display %ux%u, %u x %u mm%s%s: default looks like %ux%u",
	    pw, ph, srv->mm_width, srv->mm_height,
	    trusted ? "" : " (not trusted)",
	    srv->screen_internal ? ", built in" : "",
	    srv->modes[srv->mode_default].w, srv->modes[srv->mode_default].h);
	(void)ppi;
}

bool
display_scale_ok(const char *v)
{
	return strcasecmp(v, "Auto") == 0 || strcmp(v, "1") == 0 ||
	    strcmp(v, "2") == 0 || strcmp(v, "3") == 0;
}

static bool
parse_wxh(const char *v, uint32_t *w, uint32_t *h)
{
	unsigned a, b;
	char extra;

	if (sscanf(v, "%ux%u%c", &a, &b, &extra) != 2)
		return false;
	*w = a;
	*h = b;
	return true;
}

/* A "looks like" size this display can show: from 640 x 400 points up
   to the panel, and never more than BACK_MAX backing pixels. */
static bool
size_ok(struct ws_server *srv, uint32_t w, uint32_t h)
{
	return w >= 640 && h >= 400 && w <= srv->panel_width &&
	    h <= srv->panel_height &&
	    (w == srv->panel_width || (w * 2 <= BACK_MAX && h * 2 <= BACK_MAX));
}

/* DisplayDefaultLooksLike: Default or WxH (checked against the display
   when it's used). */
bool
display_default_ok(const char *v)
{
	uint32_t w, h;

	return v[0] == '\0' || strcasecmp(v, "Default") == 0 ||
	    parse_wxh(v, &w, &h);
}

bool
display_looks_like_ok(struct ws_server *srv, const char *v)
{
	uint32_t w, h;

	if (strcasecmp(v, "Default") == 0)
		return true;
	return parse_wxh(v, &w, &h) && size_ok(srv, w, h);
}

/* ---- the display store ---- */

/* Read the store: chosen_w/h for display_id, if it's in it. */
static void
store_load(struct ws_server *srv)
{
	char *buf, key[64], val[64];
	const char *p, *k, *e;
	size_t n;
	long len;
	FILE *f;

	srv->chosen_w = srv->chosen_h = 0;
	if (srv->display_id[0] == '\0')
		return;
	f = fopen(srv->cfg.display_store, "r");
	if (f == NULL)
		return;
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
	    len > 256 * 1024 || fseek(f, 0, SEEK_SET) != 0 ||
	    (buf = malloc((size_t)len + 1)) == NULL) {
		fclose(f);
		return;
	}
	n = fread(buf, 1, (size_t)len, f);
	buf[n] = '\0';
	fclose(f);
	for (p = buf; (k = strstr(p, "<key>")) != NULL; p = e) {
		k += 5;
		e = strstr(k, "</key>");
		if (e == NULL || (size_t)(e - k) >= sizeof(key))
			break;
		memcpy(key, k, (size_t)(e - k));
		key[e - k] = '\0';
		k = strstr(e, "<string>");
		if (k == NULL)
			break;
		k += 8;
		e = strstr(k, "</string>");
		if (e == NULL || (size_t)(e - k) >= sizeof(val))
			break;
		memcpy(val, k, (size_t)(e - k));
		val[e - k] = '\0';
		if (strcmp(key, srv->display_id) == 0)
			(void)parse_wxh(val, &srv->chosen_w, &srv->chosen_h);
	}
	free(buf);
}

/* /Library/Preferences may not exist yet. */
static void
store_mkdir(struct ws_server *srv)
{
	char dir[sizeof(srv->cfg.display_store)], *slash;

	strlcpy(dir, srv->cfg.display_store, sizeof(dir));
	slash = strrchr(dir, '/');
	if (slash == NULL || slash == dir)
		return;
	*slash = '\0';
	if (mkdir(dir, 0755) < 0 && errno != EEXIST)
		ws_warn("mkdir %s: %s", dir, strerror(errno));
}

/* Write the store again with display_id's choice changed (w 0: gone). */
static int
store_save(struct ws_server *srv, uint32_t w, uint32_t h)
{
	char *buf = NULL, *out, key[64], val[64];
	const char *p, *k, *e;
	size_t n = 0, cap;
	long len = 0;
	FILE *f;

	if (srv->cfg.display_store[0] == '\0')
		return 0;	/* headless tests: kept in memory only */
	f = fopen(srv->cfg.display_store, "r");
	if (f != NULL) {
		if (fseek(f, 0, SEEK_END) == 0 && (len = ftell(f)) >= 0 &&
		    len <= 256 * 1024 && fseek(f, 0, SEEK_SET) == 0 &&
		    (buf = malloc((size_t)len + 1)) != NULL) {
			n = fread(buf, 1, (size_t)len, f);
			buf[n] = '\0';
		}
		fclose(f);
	}
	cap = n + 4096;
	out = malloc(cap);
	if (out == NULL) {
		free(buf);
		return ENOMEM;
	}
	out[0] = '\0';
	strlcat(out, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<!-- WindowServer's display modes: each display's \"looks "
	    "like\" size, by EDID vendor-product-serial. Written by "
	    "WindowServer (Displays.app, wsctl screen looks-like). -->\n"
	    "<plist version=\"1.0\">\n<dict>\n", cap);
	for (p = buf; p != NULL && (k = strstr(p, "<key>")) != NULL; p = e) {
		k += 5;
		e = strstr(k, "</key>");
		if (e == NULL || (size_t)(e - k) >= sizeof(key))
			break;
		memcpy(key, k, (size_t)(e - k));
		key[e - k] = '\0';
		k = strstr(e, "<string>");
		if (k == NULL)
			break;
		k += 8;
		e = strstr(k, "</string>");
		if (e == NULL || (size_t)(e - k) >= sizeof(val))
			break;
		memcpy(val, k, (size_t)(e - k));
		val[e - k] = '\0';
		if (strcmp(key, srv->display_id) == 0)
			continue;
		snprintf(out + strlen(out), cap - strlen(out),
		    "\t<key>%s</key>\n\t<string>%s</string>\n", key, val);
	}
	if (w != 0)
		snprintf(out + strlen(out), cap - strlen(out),
		    "\t<key>%s</key>\n\t<string>%ux%u</string>\n",
		    srv->display_id, w, h);
	strlcat(out, "</dict>\n</plist>\n", cap);
	free(buf);
	store_mkdir(srv);
	f = fopen(srv->cfg.display_store, "w");
	if (f == NULL) {
		ws_warn("can't write %s: %s", srv->cfg.display_store,
		    strerror(errno));
		free(out);
		return errno;
	}
	fputs(out, f);
	free(out);
	if (fclose(f) != 0)
		return errno;
	return 0;
}

/* Root, before dropping privileges: the store belongs to the server's
   account, so it can write it later. */
void
display_store_chown(struct ws_server *srv, uid_t uid, gid_t gid)
{
	FILE *f;

	if (srv->cfg.display_store[0] == '\0')
		return;
	store_mkdir(srv);
	if (access(srv->cfg.display_store, F_OK) != 0) {
		f = fopen(srv->cfg.display_store, "w");
		if (f == NULL)
			return;
		fclose(f);
		chmod(srv->cfg.display_store, 0644);
		(void)store_save(srv, 0, 0);
	}
	if (chown(srv->cfg.display_store, uid, gid) < 0)
		ws_warn("chown %s: %s", srv->cfg.display_store,
		    strerror(errno));
}

/* ---- the mode in effect ---- */

/* The size in points and the scale to run at now. */
static void
effective(struct ws_server *srv, uint32_t *w, uint32_t *h, uint32_t *scale)
{
	const char *ds = srv->cfg.display_scale;
	uint32_t pw = srv->panel_width, ph = srv->panel_height, s;

	if (ds[0] >= '1' && ds[0] <= '3' && ds[1] == '\0') {
		s = (uint32_t)(ds[0] - '0');
		*w = pw / s;
		*h = ph / s;
		*scale = s;
		return;
	}
	if (srv->chosen_w != 0 && size_ok(srv, srv->chosen_w, srv->chosen_h)) {
		*w = srv->chosen_w;
		*h = srv->chosen_h;
	} else if (parse_wxh(srv->cfg.display_default_looks_like, w, h) &&
	    size_ok(srv, *w, *h)) {
		/* DisplayDefaultLooksLike: the system's choice for displays
		   the user hasn't picked a size for */
	} else {
		*w = srv->modes[srv->mode_default].w;
		*h = srv->modes[srv->mode_default].h;
	}
	if (*w == pw && *h == ph)
		*scale = 1;
	else if (*w * 3 == pw && *h * 3 == ph)
		*scale = 3;
	else
		*scale = 2;
}

/* At start, once the panel's size (and EDID) are known. */
void
display_init(struct ws_server *srv)
{
	if (srv->display_id[0] == '\0')
		snprintf(srv->display_id, sizeof(srv->display_id), "%s-%ux%u",
		    srv->headless ? "headless" : "unknown",
		    srv->panel_width, srv->panel_height);
	if (srv->display_name[0] == '\0')
		strlcpy(srv->display_name, srv->headless ? "Headless" :
		    "Display", sizeof(srv->display_name));
	modes_compute(srv);
	store_load(srv);
	effective(srv, &srv->width, &srv->height, &srv->scale);
	srv->back_width = srv->width * srv->scale;
	srv->back_height = srv->height * srv->scale;
	ws_info("screen looks like %ux%u at %ux (%ux%u backing, %ux%u panel)",
	    srv->width, srv->height, srv->scale, srv->back_width,
	    srv->back_height, srv->panel_width, srv->panel_height);
}

/*
 * The mode may have changed (a setting, the user's choice, a new panel
 * size): run at it. force: the panel changed, so the compositor's
 * images are made again even if the points stay. Returns 0 or an errno.
 */
int
display_apply(struct ws_server *srv, bool force)
{
	uint32_t ow = srv->width, oh = srv->height, os = srv->scale;
	uint32_t w, h, s;

	if (force)
		modes_compute(srv);
	effective(srv, &w, &h, &s);
	if (!force && w == ow && h == oh && s == os)
		return 0;
	srv->width = w;
	srv->height = h;
	srv->scale = s;
	srv->back_width = w * s;
	srv->back_height = h * s;
	if (compositor_resize(srv) < 0) {
		srv->width = ow;
		srv->height = oh;
		srv->scale = os;
		srv->back_width = ow * os;
		srv->back_height = oh * os;
		(void)compositor_resize(srv);
		return ENOMEM;
	}
	ws_info("screen looks like %ux%u at %ux (%ux%u backing, %ux%u panel)",
	    w, h, s, srv->back_width, srv->back_height, srv->panel_width,
	    srv->panel_height);
	if (s != os) {
		(void)cursor_rescale(srv);
		(void)tile_rescale(srv);
	}
	if (overview_active(srv))
		overview_close(srv);
	screen_changed(srv);
	windows_keep_on_screen(srv, ow, oh);
	return 0;
}

/* DisplayLooksLike: the user's choice for this display, or "Default". */
int
display_set_looks_like(struct ws_server *srv, const char *v)
{
	uint32_t w = 0, h = 0;
	int e;

	if (strcasecmp(v, "Default") != 0 && (!parse_wxh(v, &w, &h) ||
	    !size_ok(srv, w, h)))
		return EINVAL;
	if (w == srv->modes[srv->mode_default].w &&
	    h == srv->modes[srv->mode_default].h)
		w = h = 0;	/* the default: keep following it */
	srv->chosen_w = w;
	srv->chosen_h = h;
	e = store_save(srv, w, h);
	ws_info("display %s: looks like %s%s", srv->display_id,
	    w ? v : "the default", e ? " (not saved)" : "");
	return display_apply(srv, false);
}

/* The read-only display entries of WS_MSG_GET_SETTINGS, by index; -1
   past the last. */
int
display_info(struct ws_server *srv, unsigned i, char *key, size_t klen,
    char *value, size_t vlen)
{
	unsigned j;

	value[0] = '\0';
	switch (i) {
	case 0:
		strlcpy(key, "DisplayID", klen);
		strlcpy(value, srv->display_id, vlen);
		break;
	case 1:
		strlcpy(key, "DisplayName", klen);
		strlcpy(value, srv->display_name, vlen);
		break;
	case 2:
		strlcpy(key, "DisplayPanel", klen);
		snprintf(value, vlen, "%ux%u", srv->panel_width,
		    srv->panel_height);
		break;
	case 3:
		strlcpy(key, "DisplayModes", klen);
		for (j = 0; j < srv->nmodes; j++)
			snprintf(value + strlen(value), vlen - strlen(value),
			    "%s%ux%u", j ? "," : "", srv->modes[j].w,
			    srv->modes[j].h);
		break;
	case 4:
		strlcpy(key, "DisplayDefault", klen);
		snprintf(value, vlen, "%ux%u", srv->modes[srv->mode_default].w,
		    srv->modes[srv->mode_default].h);
		break;
	case 5:
		strlcpy(key, "DisplayCurrent", klen);
		snprintf(value, vlen, "%ux%u@%u", srv->width, srv->height,
		    srv->scale);
		break;
	case 6:
		strlcpy(key, "DisplayLooksLike", klen);
		if (srv->chosen_w != 0)
			snprintf(value, vlen, "%ux%u", srv->chosen_w,
			    srv->chosen_h);
		else
			strlcpy(value, "Default", vlen);
		break;
	default:
		return -1;
	}
	return 0;
}
