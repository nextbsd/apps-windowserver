/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Displays ("Displays" in WSProtocol.h) and their modes, the way macOS
 * does them.
 *
 * Each connected display shows a rectangle of one desktop, its frame,
 * in points. The desktop (srv->width x srv->height) is the smallest
 * rectangle around them all, with its corner at 0, 0, and the compositor
 * draws it into one backing store of scale times that many pixels: the
 * largest scale any display needs. Each display then shows its part of
 * the backing store (d->back), scaled to its panel when the sizes
 * differ, as macOS's scaled modes are: "looks like 2048 x 1280" on a
 * 3840 x 2400 panel is drawn at 4096 x 2560 and shown at 3840 x 2400,
 * and a 1x monitor beside a 2x panel is drawn at 2x and shown at half.
 *
 * HiDPI: each display gets a list of "looks like" choices and a default
 * from its size in pixels and, when the EDID's physical size can be
 * trusted, its pixel density (exactly 2x for HiDPI displays: 3840 x 2160
 * looks like 1920 x 1080). The DisplayScale setting forces an integer
 * scale; otherwise the user's choice for the display, kept per display
 * in the display store, wins over the default.
 *
 * The arrangement: the built-in panel is the main display (the menu bar
 * and the Dock), and a display plugged in goes to the right of the
 * others, tops aligned with the main one. The places, the main display
 * and mirroring are kept in the store per set of displays (by EDID), so
 * the same set comes back as it was. A display moved by hand is snapped
 * against the others, touching and never overlapping.
 *
 * Windows of a display that goes away move to the main display, and go
 * back when it comes back, unless they've moved since.
 *
 * Display sleep: powerd (pmconfigd) runs the display idle timer, as on
 * Apple silicon Macs, and publishes its decision as notify(3)
 * "com.apple.iokit.hid.displayStatus", state 1 while the display is on
 * and 0 once it should be off. The server checks it every couple of
 * seconds (notify_check: notify_register_file_descriptor doesn't work
 * yet, nextbsd-userland#617) and turns every display off (DPMS) when it
 * becomes 0, and on again when it becomes 1 or as soon as input comes,
 * without waiting for powerd. Nothing is drawn while they are off.
 */

#include <sys/stat.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "server.h"

/* "Looks like" widths offered between half the panel and all of it. */
static const uint32_t mode_widths[] = {
	1280, 1440, 1680, 1920, 2048, 2560, 3008, 3840, 4096, 5120,
};

/* The largest backing store of one display, pixels on a side. */
#define BACK_MAX	8192

/* How much of a window's corner stays on a display (window.c's). */
#define KEEP_X		40
#define KEEP_Y		24

/* Night Shift fades in and out over this long at a schedule's ends. */
#define NIGHT_FADE_SEC	60

static void relayout(struct ws_server *srv, bool force);

/* ---- the list ---- */

struct ws_display *
display_add(struct ws_server *srv)
{
	struct ws_display *d;

	if (srv->ndisplays >= WS_MAX_DISPLAYS)
		return NULL;
	d = calloc(1, sizeof(*d));
	if (d == NULL)
		return NULL;
	d->id = ++srv->next_display_id;
	d->on = true;
	d->gamma_r = d->gamma_g = d->gamma_b = 1.0;
	srv->displays[srv->ndisplays++] = d;
	return d;
}

/* d is unplugged: the output and renderer have let it go already. */
void
display_remove(struct ws_server *srv, struct ws_display *d)
{
	unsigned i, j;

	for (i = j = 0; i < srv->ndisplays; i++)
		if (srv->displays[i] != d)
			srv->displays[j++] = srv->displays[i];
	srv->ndisplays = j;
	for (i = 0; i < srv->ndisplays; i++)
		if (srv->displays[i]->mirror_of == d->id)
			srv->displays[i]->mirror_of = 0;
	if (srv->main == d)
		srv->main = NULL;
	if (d->panel_image != NULL)
		pixman_image_unref(d->panel_image);
	if (d->view != NULL && d->view != d->panel_image)
		pixman_image_unref(d->view);
	free(d);
}

struct ws_display *
display_lookup(struct ws_server *srv, uint32_t id)
{
	unsigned i;

	for (i = 0; i < srv->ndisplays; i++)
		if (srv->displays[i]->id == id)
			return srv->displays[i];
	return NULL;
}

/* Is d part of the desktop's arrangement (on, and not a mirror)? */
bool
display_shows(const struct ws_display *d)
{
	return d->on && d->mirror_of == 0;
}

/* ---- EDID ---- */

void
display_set_edid(struct ws_display *d, const uint8_t *e, size_t len)
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
	snprintf(d->edid_id, sizeof(d->edid_id), "%s-%04x-%08x",
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
			strlcpy(d->name, name, sizeof(d->name));
	}
	/* QEMU (RHT) and VirtualBox (VBX) make their sizes up. */
	d->edid_trusted = !(strcmp(vendor, "RHT") == 0 ||
	    strcmp(vendor, "VBX") == 0 || strcmp(vendor, "QEM") == 0);
	ws_info("display %s (%s)%s", d->edid_id,
	    d->name[0] ? d->name : "no name",
	    d->edid_trusted ? "" : ", a virtual machine's: its size is "
	    "made up");
}

/* Is the physical size believable for the panel's pixels? */
static bool
size_trusted(const struct ws_display *d)
{
	double ppi, pa, ma;

	if (!d->edid_trusted || d->mm_width < 100 || d->mm_height < 60 ||
	    d->mm_width > 1600 || d->mm_height > 1000 || d->panel_width == 0)
		return false;
	ppi = d->panel_width * 25.4 / d->mm_width;
	if (ppi < 50 || ppi > 600)
		return false;
	/* square pixels: the shapes agree (projectors and TVs that report
	   an aspect ratio instead don't) */
	pa = (double)d->panel_width / d->panel_height;
	ma = (double)d->mm_width / d->mm_height;
	return fabs(pa / ma - 1.0) < 0.15;
}

/* ---- the "looks like" choices ---- */

static uint32_t
height_for(const struct ws_display *d, uint32_t w)
{
	uint32_t h = (uint32_t)lround((double)w * d->panel_height /
	    d->panel_width);

	return h & ~1u;
}

static void
add_mode(struct ws_display *d, uint32_t w, uint32_t h)
{
	unsigned i;

	if (w == 0 || h == 0 || d->nmodes >= WS_MAX_DISPLAY_MODES)
		return;
	for (i = 0; i < d->nmodes; i++)
		if (d->modes[i].w == w)
			return;
	d->modes[d->nmodes].w = w;
	d->modes[d->nmodes].h = h;
	d->nmodes++;
}

/* Can this panel run at 2x? Half of it must still be a usable screen. */
static bool
hidpi_capable(const struct ws_display *d)
{
	return d->panel_width >= 2560 && d->panel_height >= 1400;
}

static void
modes_compute(struct ws_display *d)
{
	uint32_t pw = d->panel_width, ph = d->panel_height;
	double target, best;
	bool hidpi, trusted = size_trusted(d);
	unsigned i;

	d->nmodes = 0;
	d->mode_default = 0;
	if (hidpi_capable(d)) {
		add_mode(d, pw / 2, ph / 2);
		for (i = 0; i < sizeof(mode_widths) / sizeof(mode_widths[0]);
		    i++)
			if (mode_widths[i] > pw / 2 && mode_widths[i] < pw)
				add_mode(d, mode_widths[i],
				    height_for(d, mode_widths[i]));
	}
	add_mode(d, pw, ph);		/* native, 1x */

	if (trusted)
		hidpi = hidpi_capable(d) && pw * 25.4 / d->mm_width >= 125;
	else
		/* no real size (virtual machines): by the pixels alone,
		   a 4K screen or larger is HiDPI */
		hidpi = pw >= 3840 && ph >= 2160;
	/* As macOS: exactly 2x, a 3840 x 2160 display looking like
	   1920 x 1080; the scaled sizes are the user's to pick. */
	target = pw / 2.0;
	if (!hidpi) {
		d->mode_default = d->nmodes - 1;
	} else {
		best = 1e9;
		for (i = 0; i + 1 < d->nmodes; i++)
			if (fabs(d->modes[i].w - target) < best) {
				best = fabs(d->modes[i].w - target);
				d->mode_default = i;
			}
	}
	ws_info("display %s %ux%u, %u x %u mm%s%s: default looks like %ux%u",
	    d->connector, pw, ph, d->mm_width, d->mm_height,
	    trusted ? "" : " (not trusted)", d->builtin ? ", built in" : "",
	    d->modes[d->mode_default].w, d->modes[d->mode_default].h);
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
size_ok(const struct ws_display *d, uint32_t w, uint32_t h)
{
	return w >= 640 && h >= 400 && w <= d->panel_width &&
	    h <= d->panel_height &&
	    (w == d->panel_width || (w * 2 <= BACK_MAX && h * 2 <= BACK_MAX));
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

/* DisplayLooksLike (the settings): the main display's. */
bool
display_looks_like_ok(struct ws_server *srv, const char *v)
{
	uint32_t w, h;

	if (strcasecmp(v, "Default") == 0)
		return true;
	return srv->main != NULL && parse_wxh(v, &w, &h) &&
	    size_ok(srv->main, w, h);
}

/* ---- the display store ---- */

/*
 * The store is a plist of strings, read and written whole:
 *   <EDID id>			"WxH", the "looks like" choice
 *   <EDID id>/mode		"WxH@mHz", the mode chosen
 *   set:<EDID id>+<EDID id>...	the arrangement of that set:
 *				"main=<id>;<id>=x,y;<id>=x,y;<id>>mirrored-id"
 */
struct store {
	unsigned	n;
	char		(*key)[160];
	char		(*val)[512];
};

static void
store_free(struct store *st)
{
	free(st->key);
	free(st->val);
	memset(st, 0, sizeof(*st));
}

/* Unescape the plist's &amp; &lt; &gt; in place. */
static void
xml_unescape(char *s)
{
	char *o = s;

	while (*s != '\0') {
		if (strncmp(s, "&amp;", 5) == 0) {
			*o++ = '&';
			s += 5;
		} else if (strncmp(s, "&lt;", 4) == 0) {
			*o++ = '<';
			s += 4;
		} else if (strncmp(s, "&gt;", 4) == 0) {
			*o++ = '>';
			s += 4;
		} else
			*o++ = *s++;
	}
	*o = '\0';
}

static void
xml_escape(const char *s, char *out, size_t len)
{
	size_t o = 0;

	for (; *s != '\0' && o + 6 < len; s++) {
		if (*s == '&') {
			memcpy(out + o, "&amp;", 5);
			o += 5;
		} else if (*s == '<') {
			memcpy(out + o, "&lt;", 4);
			o += 4;
		} else if (*s == '>') {
			memcpy(out + o, "&gt;", 4);
			o += 4;
		} else
			out[o++] = *s;
	}
	out[o] = '\0';
}

static void
store_load(struct ws_server *srv, struct store *st)
{
	char *buf, key[160], val[512];
	const char *p, *k, *e;
	size_t n;
	long len;
	FILE *f;

	memset(st, 0, sizeof(*st));
	if (srv->cfg.display_store[0] == '\0')
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
		xml_unescape(key);
		xml_unescape(val);
		if ((st->n & 15) == 0) {
			void *nk = realloc(st->key, (st->n + 16) *
			    sizeof(*st->key));
			void *nv = nk == NULL ? NULL : realloc(st->val,
			    (st->n + 16) * sizeof(*st->val));

			if (nk != NULL)
				st->key = nk;
			if (nv == NULL)
				break;
			st->val = nv;
		}
		strlcpy(st->key[st->n], key, sizeof(st->key[0]));
		strlcpy(st->val[st->n], val, sizeof(st->val[0]));
		st->n++;
	}
	free(buf);
}

static const char *
store_get(const struct store *st, const char *key)
{
	unsigned i;

	for (i = 0; i < st->n; i++)
		if (strcmp(st->key[i], key) == 0)
			return st->val[i];
	return NULL;
}

/* /Local/Library/Preferences may not exist yet: make the store's
   directories, as mkdir -p. */
static void
store_mkdir(struct ws_server *srv)
{
	char dir[sizeof(srv->cfg.display_store)], *p;

	strlcpy(dir, srv->cfg.display_store, sizeof(dir));
	p = strrchr(dir, '/');
	if (p == NULL || p == dir)
		return;
	*p = '\0';
	for (p = dir + 1;; p++) {
		if (*p != '/' && *p != '\0')
			continue;
		char c = *p;

		*p = '\0';
		if (mkdir(dir, 0755) < 0 && errno != EEXIST) {
			ws_warn("mkdir %s: %s", dir, strerror(errno));
			return;
		}
		if (c == '\0')
			return;
		*p = c;
	}
}

/* Write the store again with `key` set to `val` (NULL: gone). */
static int
store_put(struct ws_server *srv, const char *key, const char *val)
{
	struct store st;
	char k[400], v[1100];
	unsigned i;
	FILE *f;
	int e = 0;

	if (srv->cfg.display_store[0] == '\0')
		return 0;	/* headless tests: kept in memory only */
	store_load(srv, &st);
	store_mkdir(srv);
	f = fopen(srv->cfg.display_store, "w");
	if (f == NULL) {
		e = errno;
		ws_warn("can't write %s: %s", srv->cfg.display_store,
		    strerror(e));
		store_free(&st);
		return e;
	}
	fputs("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
	    "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
	    "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
	    "<!-- WindowServer's displays: each display's \"looks like\" "
	    "size (by EDID vendor-product-serial) and mode (id/mode), and "
	    "each set of displays' arrangement (set:id+id). Written by "
	    "WindowServer (Displays.app, wsctl display). -->\n"
	    "<plist version=\"1.0\">\n<dict>\n", f);
	for (i = 0; i < st.n; i++) {
		if (strcmp(st.key[i], key) == 0)
			continue;
		xml_escape(st.key[i], k, sizeof(k));
		xml_escape(st.val[i], v, sizeof(v));
		fprintf(f, "\t<key>%s</key>\n\t<string>%s</string>\n", k, v);
	}
	if (val != NULL) {
		xml_escape(key, k, sizeof(k));
		xml_escape(val, v, sizeof(v));
		fprintf(f, "\t<key>%s</key>\n\t<string>%s</string>\n", k, v);
	}
	fputs("</dict>\n</plist>\n", f);
	store_free(&st);
	if (fclose(f) != 0)
		return errno;
	return 0;
}

/* Until the shipped org.nextbsd.WindowServer.plist was fixed, the store
   was /Library/Preferences/org.nextbsd.windowserver.displays.plist, the
   macOS path. It belongs in /Local/Library/Preferences: it is this
   machine's arrangement, not something NextBSD ships. When the store
   doesn't exist yet and the old file does, start the store as a copy of
   it, so an arrangement made before the move is kept. The old file is
   left as it is. */
#define DISPLAY_STORE_OLD \
	"/Library/Preferences/org.nextbsd.windowserver.displays.plist"

static void
store_migrate(struct ws_server *srv)
{
	char buf[8192];
	FILE *in, *out;
	size_t n;
	int ok = 1;

	if (srv->cfg.display_store[0] == '\0' ||
	    strcmp(srv->cfg.display_store, DISPLAY_STORE_OLD) == 0 ||
	    access(srv->cfg.display_store, F_OK) == 0)
		return;
	in = fopen(DISPLAY_STORE_OLD, "r");
	if (in == NULL)
		return;
	store_mkdir(srv);
	out = fopen(srv->cfg.display_store, "w");
	if (out == NULL) {
		ws_warn("can't write %s: %s", srv->cfg.display_store,
		    strerror(errno));
		fclose(in);
		return;
	}
	while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
		if (fwrite(buf, 1, n, out) != n) {
			ok = 0;
			break;
		}
	if (ferror(in))
		ok = 0;
	fclose(in);
	if (fclose(out) != 0)
		ok = 0;
	chmod(srv->cfg.display_store, 0644);
	if (ok)
		ws_info("display arrangement copied from %s to %s",
		    DISPLAY_STORE_OLD, srv->cfg.display_store);
	else
		ws_warn("copying %s to %s failed", DISPLAY_STORE_OLD,
		    srv->cfg.display_store);
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
	store_migrate(srv);
	if (access(srv->cfg.display_store, F_OK) != 0) {
		f = fopen(srv->cfg.display_store, "w");
		if (f == NULL)
			return;
		fclose(f);
		chmod(srv->cfg.display_store, 0644);
		(void)store_put(srv, "", NULL);
	}
	if (chown(srv->cfg.display_store, uid, gid) < 0)
		ws_warn("chown %s: %s", srv->cfg.display_store,
		    strerror(errno));
}

/* A display's own choices from the store: its "looks like" size and its
   mode. */
static void
choices_load(struct ws_server *srv, struct ws_display *d)
{
	struct store st;
	const char *v;
	char key[64];
	unsigned w, h, mhz;

	d->chosen_w = d->chosen_h = 0;
	d->want_w = d->want_h = d->want_mhz = 0;
	if (d->edid_id[0] == '\0')
		return;
	store_load(srv, &st);
	if ((v = store_get(&st, d->edid_id)) != NULL)
		(void)parse_wxh(v, &d->chosen_w, &d->chosen_h);
	snprintf(key, sizeof(key), "%s/mode", d->edid_id);
	if ((v = store_get(&st, key)) != NULL &&
	    sscanf(v, "%ux%u@%u", &w, &h, &mhz) == 3) {
		d->want_w = w;
		d->want_h = h;
		d->want_mhz = mhz;
	}
	store_free(&st);
}

/* ---- each display's size ---- */

/* The size in points and the scale d would run at on its own. */
static void
effective(struct ws_server *srv, struct ws_display *d, uint32_t *w,
    uint32_t *h, uint32_t *scale)
{
	const char *ds = srv->cfg.display_scale;
	uint32_t pw = d->panel_width, ph = d->panel_height, s;

	if (ds[0] >= '1' && ds[0] <= '3' && ds[1] == '\0') {
		s = (uint32_t)(ds[0] - '0');
		*w = pw / s;
		*h = ph / s;
		*scale = s;
		return;
	}
	if (d->chosen_w != 0 && size_ok(d, d->chosen_w, d->chosen_h)) {
		*w = d->chosen_w;
		*h = d->chosen_h;
	} else if (parse_wxh(srv->cfg.display_default_looks_like, w, h) &&
	    size_ok(d, *w, *h)) {
		/* DisplayDefaultLooksLike: the system's choice for displays
		   the user hasn't picked a size for */
	} else {
		*w = d->modes[d->mode_default].w;
		*h = d->modes[d->mode_default].h;
	}
	if (*w == pw && *h == ph)
		*scale = 1;
	else if (*w * 3 == pw && *h * 3 == ph)
		*scale = 3;
	else
		*scale = 2;
}

/* ---- the arrangement ---- */

static bool
overlaps(const ws_rect_t *a, const ws_rect_t *b)
{
	return a->x < b->x + b->width && b->x < a->x + a->width &&
	    a->y < b->y + b->height && b->y < a->y + a->height;
}

static int32_t
clamp32(int32_t v, int32_t lo, int32_t hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/*
 * Put d as near (x, y) as it can go touching one of the displays placed
 * before it (placed[0..n)) without overlapping any.
 */
static void
snap(struct ws_display *d, struct ws_display **placed, unsigned n, int32_t x,
    int32_t y)
{
	ws_rect_t r = d->frame, best = d->frame, o;
	double bd = 1e18, dist;
	unsigned i, j, k;

	if (n == 0) {
		d->frame.x = x;
		d->frame.y = y;
		return;
	}
	for (i = 0; i < n; i++) {
		o = placed[i]->frame;
		for (k = 0; k < 4; k++) {
			r.x = x;
			r.y = y;
			switch (k) {
			case 0:		/* right of o */
				r.x = o.x + o.width;
				r.y = clamp32(y, o.y - r.height + 1,
				    o.y + o.height - 1);
				break;
			case 1:		/* left of o */
				r.x = o.x - r.width;
				r.y = clamp32(y, o.y - r.height + 1,
				    o.y + o.height - 1);
				break;
			case 2:		/* below o */
				r.y = o.y + o.height;
				r.x = clamp32(x, o.x - r.width + 1,
				    o.x + o.width - 1);
				break;
			default:	/* above o */
				r.y = o.y - r.height;
				r.x = clamp32(x, o.x - r.width + 1,
				    o.x + o.width - 1);
				break;
			}
			for (j = 0; j < n; j++)
				if (overlaps(&r, &placed[j]->frame))
					break;
			if (j < n)
				continue;
			dist = (double)(r.x - x) * (r.x - x) +
			    (double)(r.y - y) * (r.y - y);
			if (dist < bd) {
				bd = dist;
				best = r;
			}
		}
	}
	d->frame = best;
}

/* The set's key in the store: its EDID ids, sorted. */
static void
set_key(struct ws_server *srv, char *buf, size_t len)
{
	const char *ids[WS_MAX_DISPLAYS], *t;
	unsigned i, j, n = 0;

	for (i = 0; i < srv->ndisplays; i++)
		ids[n++] = srv->displays[i]->edid_id;
	for (i = 1; i < n; i++)
		for (j = i; j > 0 && strcmp(ids[j - 1], ids[j]) > 0; j--) {
			t = ids[j];
			ids[j] = ids[j - 1];
			ids[j - 1] = t;
		}
	strlcpy(buf, "set:", len);
	for (i = 0; i < n; i++) {
		if (i > 0)
			strlcat(buf, "+", len);
		strlcat(buf, ids[i], len);
	}
}

static struct ws_display *
by_edid(struct ws_server *srv, const char *id)
{
	unsigned i;

	for (i = 0; i < srv->ndisplays; i++)
		if (strcmp(srv->displays[i]->edid_id, id) == 0)
			return srv->displays[i];
	return NULL;
}

/* The arrangement as the store keeps it. */
static void
arrangement_text(struct ws_server *srv, char *buf, size_t len)
{
	struct ws_display *d, *m;
	unsigned i;

	buf[0] = '\0';
	if (srv->main != NULL)
		snprintf(buf, len, "main=%s", srv->main->edid_id);
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (d->mirror_of != 0 &&
		    (m = display_lookup(srv, d->mirror_of)) != NULL)
			snprintf(buf + strlen(buf), len - strlen(buf),
			    ";%s>%s", m->edid_id, d->edid_id);
		else
			snprintf(buf + strlen(buf), len - strlen(buf),
			    ";%s=%d,%d", d->edid_id, d->frame.x, d->frame.y);
	}
}

/*
 * Read the set's arrangement: places into want_x/y, the main display,
 * mirrors. False when the store has none for this set.
 */
static bool
arrangement_load(struct ws_server *srv, int32_t *wx, int32_t *wy,
    bool *placed, struct ws_display **main)
{
	struct store st;
	struct ws_display *d, *s;
	char key[sizeof(srv->arrangement)], text[512], *p, *tok, *eq;
	const char *v;
	unsigned i;
	int x, y;

	set_key(srv, key, sizeof(key));
	store_load(srv, &st);
	v = store_get(&st, key);
	if (v == NULL) {
		store_free(&st);
		return false;
	}
	strlcpy(text, v, sizeof(text));
	store_free(&st);
	for (p = text; (tok = strsep(&p, ";")) != NULL;) {
		if (strncmp(tok, "main=", 5) == 0) {
			if ((d = by_edid(srv, tok + 5)) != NULL)
				*main = d;
		} else if ((eq = strchr(tok, '>')) != NULL) {
			*eq = '\0';
			s = by_edid(srv, tok);
			d = by_edid(srv, eq + 1);
			if (s != NULL && d != NULL && s != d)
				d->mirror_of = s->id;
		} else if ((eq = strchr(tok, '=')) != NULL) {
			*eq = '\0';
			d = by_edid(srv, tok);
			if (d == NULL || sscanf(eq + 1, "%d,%d", &x, &y) != 2)
				continue;
			for (i = 0; i < srv->ndisplays; i++)
				if (srv->displays[i] == d) {
					wx[i] = x;
					wy[i] = y;
					placed[i] = true;
				}
		}
	}
	return true;
}

static void
arrangement_save(struct ws_server *srv)
{
	char key[sizeof(srv->arrangement)], text[512];

	set_key(srv, key, sizeof(key));
	arrangement_text(srv, text, sizeof(text));
	(void)store_put(srv, key, text);
}

/* Where d's backing pixels go on its panel: all of it, or for a mirror of
   another shape, the largest rectangle of the source's shape. */
static void
dest_compute(struct ws_display *d, const ws_rect_t *src)
{
	double sa = (double)src->width / src->height;
	double pa = (double)d->panel_width / d->panel_height;
	int32_t w = (int32_t)d->panel_width, h = (int32_t)d->panel_height;

	if (d->mirror_of != 0 && fabs(sa / pa - 1.0) > 0.01) {
		if (sa > pa)
			h = (int32_t)lround(w / sa);
		else
			w = (int32_t)lround(h * sa);
	}
	d->dest.x = ((int32_t)d->panel_width - w) / 2;
	d->dest.y = ((int32_t)d->panel_height - h) / 2;
	d->dest.width = w;
	d->dest.height = h;
}

/* ---- the desktop's geometry ---- */

ws_rect_t
display_main_frame(struct ws_server *srv)
{
	ws_rect_t r = { 0, 0, (int32_t)srv->width, (int32_t)srv->height };

	return srv->main != NULL ? srv->main->frame : r;
}

/*
 * The display the Dock goes on for an edge, as on macOS: left or right,
 * the display furthest that way (its outer edge, never one between two
 * displays); top or bottom, the main display. Ties go to the main
 * display, then to the order WS_MSG_LIST_DISPLAYS gives, as the Dock
 * picks its screen.
 */
struct ws_display *
display_dock(struct ws_server *srv, uint32_t edge)
{
	struct ws_display *d, *best = srv->main;
	unsigned i;

	if (best == NULL || (edge != WS_DOCK_EDGE_LEFT &&
	    edge != WS_DOCK_EDGE_RIGHT))
		return best;
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (d == srv->main || !display_shows(d))
			continue;
		if (edge == WS_DOCK_EDGE_LEFT ? d->frame.x < best->frame.x :
		    d->frame.x + d->frame.width >
		    best->frame.x + best->frame.width)
			best = d;
	}
	return best;
}

/* The display showing point x, y (not a mirror), or NULL. */
struct ws_display *
display_at(struct ws_server *srv, double x, double y)
{
	struct ws_display *d;
	unsigned i;

	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (display_shows(d) && x >= d->frame.x && y >= d->frame.y &&
		    x < d->frame.x + d->frame.width &&
		    y < d->frame.y + d->frame.height)
			return d;
	}
	return NULL;
}

/* Keep x, y on a display: the nearest point of the nearest one. */
void
display_clamp(struct ws_server *srv, double *x, double *y)
{
	struct ws_display *d;
	double bx = *x, by = *y, bd = INFINITY, cx, cy, dist;
	unsigned i;
	bool any = false;

	if (display_at(srv, *x, *y) != NULL)
		return;
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!display_shows(d))
			continue;
		any = true;
		cx = fmin(fmax(*x, d->frame.x), d->frame.x + d->frame.width - 1);
		cy = fmin(fmax(*y, d->frame.y),
		    d->frame.y + d->frame.height - 1);
		dist = (cx - *x) * (cx - *x) + (cy - *y) * (cy - *y);
		if (dist < bd) {
			bd = dist;
			bx = cx;
			by = cy;
		}
	}
	if (!any) {
		bx = fmin(fmax(*x, 0), srv->width > 0 ? srv->width - 1 : 0);
		by = fmin(fmax(*y, 0), srv->height > 0 ? srv->height - 1 : 0);
	}
	*x = bx;
	*y = by;
}

/* Backing pixels of the desktop as d's panel pixels, rounded out (and a
   pixel more when scaled, for the filter). */
void
display_back_to_panel(const struct ws_display *d, pixman_region32_t *in,
    pixman_region32_t *out)
{
	int64_t bw = d->back.width, bh = d->back.height;
	int64_t pw = d->dest.width, ph = d->dest.height;
	int64_t x1, y1, x2, y2, bx1, by1, bx2, by2;
	bool same = bw == pw && bh == ph;
	pixman_box32_t *b;
	int i, n;

	pixman_region32_init(out);
	if (bw <= 0 || bh <= 0 || pw <= 0 || ph <= 0)
		return;
	b = pixman_region32_rectangles(in, &n);
	for (i = 0; i < n; i++) {
		bx1 = b[i].x1 > d->back.x ? b[i].x1 : d->back.x;
		by1 = b[i].y1 > d->back.y ? b[i].y1 : d->back.y;
		bx2 = b[i].x2 < d->back.x + bw ? b[i].x2 : d->back.x + bw;
		by2 = b[i].y2 < d->back.y + bh ? b[i].y2 : d->back.y + bh;
		if (bx2 <= bx1 || by2 <= by1)
			continue;
		bx1 -= d->back.x;
		bx2 -= d->back.x;
		by1 -= d->back.y;
		by2 -= d->back.y;
		if (same) {
			x1 = bx1;
			y1 = by1;
			x2 = bx2;
			y2 = by2;
		} else {
			x1 = bx1 * pw / bw - 1;
			y1 = by1 * ph / bh - 1;
			x2 = (bx2 * pw + bw - 1) / bw + 1;
			y2 = (by2 * ph + bh - 1) / bh + 1;
			if (x1 < 0)
				x1 = 0;
			if (y1 < 0)
				y1 = 0;
			if (x2 > pw)
				x2 = pw;
			if (y2 > ph)
				y2 = ph;
		}
		if (x2 > x1 && y2 > y1)
			pixman_region32_union_rect(out, out,
			    (int)(x1 + d->dest.x), (int)(y1 + d->dest.y),
			    (unsigned)(x2 - x1), (unsigned)(y2 - y1));
	}
}

/* A panel pixel of d as a point of the desktop. */
void
display_panel_to_points(const struct ws_display *d, double px, double py,
    double *x, double *y)
{
	ws_rect_t f = d->frame;

	*x = f.x + (px - d->dest.x) * f.width / (d->dest.width ? d->dest.width :
	    1);
	*y = f.y + (py - d->dest.y) * f.height / (d->dest.height ?
	    d->dest.height : 1);
}

/* ---- windows when displays come and go ---- */

/* The display showing most of r, or NULL when none shows any. */
static struct ws_display *
display_of(struct ws_server *srv, const ws_rect_t *r)
{
	struct ws_display *d, *best = NULL;
	int64_t area, most = 0;
	int32_t x1, y1, x2, y2;
	unsigned i;

	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!display_shows(d))
			continue;
		x1 = r->x > d->frame.x ? r->x : d->frame.x;
		y1 = r->y > d->frame.y ? r->y : d->frame.y;
		x2 = r->x + r->width < d->frame.x + d->frame.width ?
		    r->x + r->width : d->frame.x + d->frame.width;
		y2 = r->y + r->height < d->frame.y + d->frame.height ?
		    r->y + r->height : d->frame.y + d->frame.height;
		if (x2 <= x1 || y2 <= y1)
			continue;
		area = (int64_t)(x2 - x1) * (y2 - y1);
		if (area > most) {
			most = area;
			best = d;
		}
	}
	return best;
}

static bool
window_kept(const struct ws_window *w)
{
	/* Windows that aren't shown (an app's offscreen helpers) stay
	   where their app put them; the server's own tiles are placed by
	   tile.c. */
	return (w->visible || w->minimized) && w->tile_for == NULL &&
	    w->frame.width > 0 && w->frame.height > 0;
}

struct old_display {
	char		edid_id[48];
	ws_rect_t	frame;
};

/*
 * The displays changed from `old` (n of them, their frames before) to
 * now. Windows of a display that went away move to the main display, at
 * the same place relative to its corner as far as they fit, and
 * remember where they were; windows whose display came back go back
 * there if they haven't moved since. Then every shown window keeps its
 * corner on some display.
 */
static void
windows_follow(struct ws_server *srv, const struct old_display *old,
    unsigned nold)
{
	struct ws_display *d, *m = srv->main;
	struct ws_window *w;
	ws_rect_t r, u;
	unsigned i;

	if (m == NULL)
		return;
	u = srv->usable.width > 0 ? srv->usable : m->frame;
	for (w = srv->windows; w != NULL; w = w->next) {
		if (!window_kept(w))
			continue;
		r = w->frame;
		/* back to its own display? */
		if (w->home_display[0] != '\0') {
			d = by_edid(srv, w->home_display);
			if (d != NULL && display_shows(d) &&
			    memcmp(&w->moved_to, &w->frame,
			    sizeof(r)) == 0) {
				r = w->home_frame;
				r.x += d->frame.x;
				r.y += d->frame.y;
				w->home_display[0] = '\0';
				ws_debug("window %u back on %s", w->id,
				    d->connector);
				window_server_frame(srv, w, &r);
				continue;
			}
			if (memcmp(&w->moved_to, &w->frame, sizeof(r)) != 0)
				w->home_display[0] = '\0';
		}
		if (display_of(srv, &r) != NULL)
			continue;
		/* its display went away (or the arrangement left it off
		   every display) */
		for (i = 0; i < nold; i++)
			if (r.x < old[i].frame.x + old[i].frame.width &&
			    r.x + r.width > old[i].frame.x &&
			    r.y < old[i].frame.y + old[i].frame.height &&
			    r.y + r.height > old[i].frame.y)
				break;
		if (i < nold && old[i].edid_id[0] != '\0' &&
		    ((d = by_edid(srv, old[i].edid_id)) == NULL ||
		    !display_shows(d))) {
			strlcpy(w->home_display, old[i].edid_id,
			    sizeof(w->home_display));
			w->home_frame = r;
			w->home_frame.x -= old[i].frame.x;
			w->home_frame.y -= old[i].frame.y;
			r.x = u.x + w->home_frame.x;
			r.y = u.y + w->home_frame.y;
		}
		r.x = clamp32(r.x, u.x, u.x + u.width - r.width);
		r.y = clamp32(r.y, u.y, u.y + u.height - r.height);
		if (r.x < u.x)
			r.x = u.x;
		if (r.y < u.y)
			r.y = u.y;
		w->moved_to = r;
		ws_debug("window %u moved onto the main display: %d,%d -> "
		    "%d,%d", w->id, w->frame.x, w->frame.y, r.x, r.y);
		window_server_frame(srv, w, &r);
		if (w->home_display[0] != '\0')
			w->moved_to = w->frame;
	}
	/* a corner on some display for every other window */
	for (w = srv->windows; w != NULL; w = w->next) {
		ws_rect_t corner;
		double cx, cy;

		if (!window_kept(w))
			continue;
		r = w->frame;
		corner = (ws_rect_t){ r.x, r.y, r.width < KEEP_X ? r.width :
		    KEEP_X, r.height < KEEP_Y ? r.height : KEEP_Y };
		if (display_of(srv, &corner) != NULL)
			continue;
		cx = r.x;
		cy = r.y;
		display_clamp(srv, &cx, &cy);
		d = display_at(srv, cx, cy);
		if (d == NULL)
			continue;
		r.x = clamp32((int32_t)cx, d->frame.x,
		    d->frame.x + d->frame.width - corner.width);
		r.y = clamp32((int32_t)cy, d->frame.y,
		    d->frame.y + d->frame.height - corner.height);
		if (r.x != w->frame.x || r.y != w->frame.y)
			window_server_frame(srv, w, &r);
	}
}

/* Everything in the desktop moves by dx, dy (its corner moved). */
static void
desktop_shift(struct ws_server *srv, int32_t dx, int32_t dy)
{
	struct ws_window *w;
	ws_rect_t r;

	for (w = srv->windows; w != NULL; w = w->next) {
		if (w->frame.width <= 0 && w->frame.height <= 0)
			continue;
		r = w->frame;
		r.x += dx;
		r.y += dy;
		if (w->home_display[0] != '\0') {
			w->moved_to.x += dx;
			w->moved_to.y += dy;
		}
		window_server_frame(srv, w, &r);
	}
	srv->ptr_x += dx;
	srv->ptr_y += dy;
}

/* ---- telling the clients ---- */

static void
info_fill(struct ws_server *srv, const struct ws_display *d,
    ws_display_info_t *di)
{
	memset(di, 0, sizeof(*di));
	di->display_id = d->id;
	if (d == srv->main)
		di->flags |= WS_DISPLAY_MAIN;
	if (d->builtin)
		di->flags |= WS_DISPLAY_BUILTIN;
	if (d->mirror_of != 0)
		di->flags |= WS_DISPLAY_MIRROR;
	if (d->modes[d->mode_default].w * 2 <= d->panel_width + 1 &&
	    d->nmodes > 1)
		di->flags |= WS_DISPLAY_HIDPI;
	if (srv->display_pending) {
		uint64_t now = ws_now_usec();

		di->flags |= WS_DISPLAY_PENDING;
		di->revert_ms = srv->display_pending_until > now ?
		    (uint32_t)((srv->display_pending_until - now) / 1000) : 0;
	}
	di->frame = d->frame;
	screen_usable_of(srv, d, &di->usable);
	di->scale = srv->scale ? srv->scale : 1;
	di->panel_width = d->panel_width;
	di->panel_height = d->panel_height;
	di->refresh_mhz = d->refresh_mhz;
	di->mm_width = d->mm_width;
	di->mm_height = d->mm_height;
	di->mirror_of = d->mirror_of;
	strlcpy(di->name, d->name[0] != '\0' ? d->name : d->builtin ?
	    "Built-in Display" : "Display", sizeof(di->name));
	strlcpy(di->edid_id, d->edid_id, sizeof(di->edid_id));
	strlcpy(di->connector, d->connector, sizeof(di->connector));
}

void
display_changed_notify(struct ws_server *srv)
{
	struct ws_client *c;
	ws_event_t ev;

	for (c = srv->clients; c != NULL; c = c->next) {
		event_init(srv, &ev, WS_EVENT_DISPLAYS_CHANGED, 0, 0);
		client_send(srv, c, &ev, -1);
	}
}

/* WS_MSG_LIST_DISPLAYS: the displays that are on, the main one first. */
void
display_list(struct ws_server *srv, struct ws_client *c)
{
	struct ws_display *d;
	ws_event_t ev;
	unsigned i;
	int pass;

	for (pass = 0; pass < 2; pass++)
		for (i = 0; i < srv->ndisplays; i++) {
			d = srv->displays[i];
			if (!d->on || (pass == 0) != (d == srv->main))
				continue;
			event_init(srv, &ev, WS_EVENT_DISPLAY,
			    sizeof(ev.u.display), 0);
			info_fill(srv, d, &ev.u.display);
			client_send(srv, c, &ev, -1);
		}
	event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
	client_send(srv, c, &ev, -1);
}

/* WS_MSG_LIST_DISPLAY_MODES: "looks like" sizes, then modes, largest
   first. */
void
display_list_modes(struct ws_server *srv, struct ws_client *c, uint32_t id)
{
	struct ws_display *d = display_lookup(srv, id);
	uint32_t w, h, s;
	unsigned i, j, order[WS_MAX_PANEL_MODES], t;
	ws_event_t ev;

	if (d == NULL) {
		client_error(srv, c, WS_MSG_LIST_DISPLAY_MODES, 0, ENOENT,
		    "no such display");
		event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
		client_send(srv, c, &ev, -1);
		return;
	}
	effective(srv, d, &w, &h, &s);
	for (i = 0; i < d->nmodes; i++) {
		event_init(srv, &ev, WS_EVENT_DISPLAY_MODE,
		    sizeof(ev.u.display_mode), 0);
		ev.u.display_mode.display_id = d->id;
		ev.u.display_mode.kind = WS_DISPLAY_MODE_LOOKS_LIKE;
		ev.u.display_mode.width = d->modes[i].w;
		ev.u.display_mode.height = d->modes[i].h;
		if (i == d->mode_default)
			ev.u.display_mode.flags |= WS_DISPLAY_MODE_DEFAULT;
		if (d->modes[i].w == w && d->modes[i].h == h) {
			ev.u.display_mode.flags |= WS_DISPLAY_MODE_CURRENT;
			if (d->chosen_w != 0)
				ev.u.display_mode.flags |=
				    WS_DISPLAY_MODE_CHOSEN;
		}
		client_send(srv, c, &ev, -1);
	}
	for (i = 0; i < d->npanel; i++)
		order[i] = i;
	for (i = 1; i < d->npanel; i++)
		for (j = i; j > 0; j--) {
			const struct ws_panel_mode *a = &d->panel[order[j - 1]];
			const struct ws_panel_mode *b = &d->panel[order[j]];
			uint64_t pa = (uint64_t)a->w * a->h, pb =
			    (uint64_t)b->w * b->h;

			if (pa > pb || (pa == pb && a->mhz >= b->mhz))
				break;
			t = order[j];
			order[j] = order[j - 1];
			order[j - 1] = t;
		}
	for (i = 0; i < d->npanel; i++) {
		const struct ws_panel_mode *pm = &d->panel[order[i]];

		/* the same size and rate twice (other timings): once */
		for (j = 0; j < i; j++)
			if (d->panel[order[j]].w == pm->w &&
			    d->panel[order[j]].h == pm->h &&
			    d->panel[order[j]].mhz / 1000 == pm->mhz / 1000 &&
			    d->panel[order[j]].interlaced == pm->interlaced)
				break;
		if (j < i)
			continue;
		event_init(srv, &ev, WS_EVENT_DISPLAY_MODE,
		    sizeof(ev.u.display_mode), 0);
		ev.u.display_mode.display_id = d->id;
		ev.u.display_mode.kind = WS_DISPLAY_MODE_PANEL;
		ev.u.display_mode.width = pm->w;
		ev.u.display_mode.height = pm->h;
		ev.u.display_mode.refresh_mhz = pm->mhz;
		if (pm->preferred)
			ev.u.display_mode.flags |= WS_DISPLAY_MODE_DEFAULT;
		if (pm->interlaced)
			ev.u.display_mode.flags |= WS_DISPLAY_MODE_INTERLACED;
		if (pm->w == d->panel_width && pm->h == d->panel_height &&
		    pm->mhz / 1000 == d->refresh_mhz / 1000)
			ev.u.display_mode.flags |= WS_DISPLAY_MODE_CURRENT;
		client_send(srv, c, &ev, -1);
	}
	event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
	client_send(srv, c, &ev, -1);
}

/* ---- the mode in effect ---- */

/* The panel mode d runs at: the user's, else the preferred one. */
static void
panel_mode_pick(struct ws_server *srv, struct ws_display *d)
{
	const struct ws_panel_mode *pm = NULL;
	unsigned i, w = 0, h = 0, hz = 0;
	int n;

	if (d->npanel == 0)
		return;		/* headless: the panel is what it is */
	for (i = 0; i < d->npanel; i++)
		if (d->panel[i].preferred) {
			pm = &d->panel[i];
			break;
		}
	if (pm == NULL)
		pm = &d->panel[0];
	if (d->want_w != 0) {
		for (i = 0; i < d->npanel; i++)
			if (d->panel[i].w == d->want_w &&
			    d->panel[i].h == d->want_h &&
			    (d->want_mhz == 0 ||
			    d->panel[i].mhz / 1000 == d->want_mhz / 1000)) {
				pm = &d->panel[i];
				break;
			}
	} else if (d == srv->main || srv->ndisplays == 1) {
		/* DisplayPanelMode: the system's mode for the main display */
		const char *v = srv->cfg.display_panel_mode;

		if (v[0] != '\0' && strcasecmp(v, "Preferred") != 0) {
			n = sscanf(v, "%ux%u@%u", &w, &h, &hz);
			for (i = 0; n >= 2 && i < d->npanel; i++)
				if (d->panel[i].w == w && d->panel[i].h == h &&
				    (n < 3 || d->panel[i].mhz / 1000 == hz)) {
					pm = &d->panel[i];
					break;
				}
		}
	}
	d->panel_width = pm->w;
	d->panel_height = pm->h;
	d->refresh_mhz = pm->mhz;
}

/*
 * Work out the whole layout from the displays, their choices and the
 * store, run at it, and tell everyone what changed. force: make the
 * compositor's images again even if the sizes stay (a display came,
 * went or changed mode).
 */
static void
relayout(struct ws_server *srv, bool force)
{
	struct old_display old[WS_MAX_DISPLAYS];
	struct ws_display *d, *m, *placed[WS_MAX_DISPLAYS], *src, *anchor;
	struct ws_display *stored_main = NULL;
	uint32_t ow = srv->width, oh = srv->height, os = srv->scale, w, h, s;
	uint32_t opw[WS_MAX_DISPLAYS], oph[WS_MAX_DISPLAYS];
	bool was_on[WS_MAX_DISPLAYS], have[WS_MAX_DISPLAYS];
	int32_t wx[WS_MAX_DISPLAYS], wy[WS_MAX_DISPLAYS], minx, miny, maxx,
	    maxy, rx;
	bool known, back_on, all_on, any_external = false;
	bool changed_panels = force;
	char key[sizeof(srv->arrangement)];
	unsigned i, j, n, nold = 0;

	/* what there was */
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		was_on[i] = d->on && d->frame.width > 0;
		opw[i] = d->panel_width;
		oph[i] = d->panel_height;
		if (was_on[i] && nold < WS_MAX_DISPLAYS) {
			strlcpy(old[nold].edid_id, d->edid_id,
			    sizeof(old[nold].edid_id));
			old[nold].frame = d->frame;
			nold++;
		}
		if (!d->builtin)
			any_external = true;
	}

	/* which are on: all but a closed lid's panel, when there's
	   another */
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		d->on = !(d->builtin && srv->lid_closed && any_external);
		panel_mode_pick(srv, d);
		if (d->panel_width != opw[i] || d->panel_height != oph[i] ||
		    d->on != was_on[i])
			changed_panels = true;
		if (d->on) {
			modes_compute(d);
			effective(srv, d, &w, &h, &d->own_scale);
			d->frame.width = (int32_t)w;
			d->frame.height = (int32_t)h;
		} else {
			d->frame.width = d->frame.height = 0;
		}
	}

	/*
	 * A new set of displays, or one of the set back on (the lid
	 * opened): the set's stored arrangement, if any, for the ones
	 * without a place; the others keep theirs.
	 */
	set_key(srv, key, sizeof(key));
	known = strcmp(key, srv->arrangement) == 0;
	back_on = false;
	for (i = 0; i < srv->ndisplays; i++)
		if (srv->displays[i]->on && !was_on[i])
			back_on = true;
	memset(have, 0, sizeof(have));
	if (!known || back_on) {
		if (!known)
			for (i = 0; i < srv->ndisplays; i++)
				srv->displays[i]->mirror_of = 0;
		if (!arrangement_load(srv, wx, wy, have, &stored_main))
			memset(have, 0, sizeof(have));
		strlcpy(srv->arrangement, key, sizeof(srv->arrangement));
	}
	if (known) {
		for (i = 0; i < srv->ndisplays; i++) {
			d = srv->displays[i];
			if (was_on[i] && !back_on) {
				wx[i] = d->frame.x;
				wy[i] = d->frame.y;
				have[i] = true;
			}
		}
	}

	/* the main display: the stored one, else the one there was, else
	   the built-in panel, else the first */
	m = stored_main;
	if (m == NULL || !m->on || m->mirror_of != 0)
		m = srv->main;
	if (m == NULL || !m->on || m->mirror_of != 0 ||
	    display_lookup(srv, m->id) == NULL) {
		m = NULL;
		for (i = 0; i < srv->ndisplays && m == NULL; i++)
			if (srv->displays[i]->on && srv->displays[i]->builtin &&
			    srv->displays[i]->mirror_of == 0)
				m = srv->displays[i];
		for (i = 0; i < srv->ndisplays && m == NULL; i++)
			if (display_shows(srv->displays[i]))
				m = srv->displays[i];
	}
	/* mirrors of displays that are off show their own part again */
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		src = d->mirror_of ? display_lookup(srv, d->mirror_of) : NULL;
		if (src == NULL || !src->on || src->mirror_of != 0 || !d->on)
			d->mirror_of = 0;
	}
	srv->main = m;

	/*
	 * Places, as macOS arranges them whichever display is main: the
	 * built-in panel first (else the main display), at its place, then
	 * the others with places, snapped; new ones go to the right, tops
	 * aligned with the first.
	 */
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (d->place_afresh) {
			have[i] = false;
			d->place_afresh = false;
		}
	}
	anchor = m;
	for (i = 0; i < srv->ndisplays; i++)
		if (display_shows(srv->displays[i]) &&
		    srv->displays[i]->builtin)
			anchor = srv->displays[i];
	n = 0;
	if (anchor != NULL) {
		for (i = 0; i < srv->ndisplays; i++)
			if (srv->displays[i] == anchor)
				break;
		anchor->frame.x = have[i] ? wx[i] : 0;
		anchor->frame.y = have[i] ? wy[i] : 0;
		placed[n++] = anchor;
	}
	for (j = 0; j < 2; j++)
		for (i = 0; i < srv->ndisplays; i++) {
			d = srv->displays[i];
			if (!display_shows(d) || d == anchor ||
			    (j == 0) != have[i])
				continue;
			if (have[i]) {
				snap(d, placed, n, wx[i], wy[i]);
			} else {
				for (rx = anchor != NULL ? anchor->frame.x : 0,
				    s = 0; s < n; s++)
					if (placed[s]->frame.x +
					    placed[s]->frame.width > rx)
						rx = placed[s]->frame.x +
						    placed[s]->frame.width;
				snap(d, placed, n, rx, anchor != NULL ?
				    anchor->frame.y : 0);
			}
			placed[n++] = d;
		}
	/* mirrors show their source's part */
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (d->on && d->mirror_of != 0 &&
		    (src = display_lookup(srv, d->mirror_of)) != NULL)
			d->frame = src->frame;
	}

	/* the desktop: around them all, its corner at 0, 0 */
	minx = miny = INT32_MAX;
	maxx = maxy = INT32_MIN;
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!display_shows(d))
			continue;
		if (d->frame.x < minx)
			minx = d->frame.x;
		if (d->frame.y < miny)
			miny = d->frame.y;
		if (d->frame.x + d->frame.width > maxx)
			maxx = d->frame.x + d->frame.width;
		if (d->frame.y + d->frame.height > maxy)
			maxy = d->frame.y + d->frame.height;
	}
	if (m == NULL) {
		/* nothing to show (every display unplugged): keep the
		   desktop as it was until one comes */
		ws_info("no display is on");
		srv->display_active = false;
		display_changed_notify(srv);
		return;
	}
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (d->on) {
			d->frame.x -= minx;
			d->frame.y -= miny;
		}
	}
	for (i = 0; i < nold; i++) {
		old[i].frame.x -= minx;
		old[i].frame.y -= miny;
	}

	/* the scale: the largest any display needs */
	srv->scale = 1;
	for (i = 0; i < srv->ndisplays; i++)
		if (srv->displays[i]->on &&
		    srv->displays[i]->own_scale > srv->scale)
			srv->scale = srv->displays[i]->own_scale;
	srv->width = (uint32_t)(maxx - minx);
	srv->height = (uint32_t)(maxy - miny);
	srv->back_width = srv->width * srv->scale;
	srv->back_height = srv->height * srv->scale;
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!d->on)
			continue;
		d->back = (ws_rect_t){ d->frame.x * (int32_t)srv->scale,
		    d->frame.y * (int32_t)srv->scale,
		    d->frame.width * (int32_t)srv->scale,
		    d->frame.height * (int32_t)srv->scale };
		dest_compute(d, &d->frame);
	}
	srv->panel_width = m->panel_width;
	srv->panel_height = m->panel_height;
	srv->refresh_mhz = m->refresh_mhz;
	srv->mm_width = m->mm_width;
	srv->mm_height = m->mm_height;
	snprintf(srv->screen_name, sizeof(srv->screen_name), "%s",
	    m->connector);
	if (!srv->started) {
		/* at start (display_init): the compositor, the outputs and
		   the screen come after */
		for (i = 0; i < srv->ndisplays; i++)
			if (srv->displays[i]->on)
				ws_info("display %s%s: looks like %dx%d at "
				    "%d,%d", srv->displays[i]->connector,
				    srv->displays[i] == m ? " (main)" : "",
				    srv->displays[i]->frame.width,
				    srv->displays[i]->frame.height,
				    srv->displays[i]->frame.x,
				    srv->displays[i]->frame.y);
		ws_info("desktop %ux%u at %ux (%ux%u backing)", srv->width,
		    srv->height, srv->scale, srv->back_width,
		    srv->back_height);
		return;
	}

	/* the outputs follow, then the compositor */
	if (srv->output != NULL && srv->output->display_on != NULL)
		for (i = 0; i < srv->ndisplays; i++) {
			d = srv->displays[i];
			if (d->on && (!was_on[i] || d->panel_width != opw[i] ||
			    d->panel_height != oph[i] || force)) {
				if (srv->output->display_on(srv, d) < 0) {
					ws_warn("display %s: can't turn it on",
					    d->connector);
					d->on = false;
				}
			} else if (!d->on && was_on[i])
				srv->output->display_off(srv, d);
		}
	if (changed_panels || srv->width != ow || srv->height != oh ||
	    srv->scale != os) {
		if (compositor_resize(srv) < 0)
			ws_err("displays: no images for %ux%u at %ux",
			    srv->width, srv->height, srv->scale);
	}
	srv->display_active = srv->output != NULL;
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!d->on)
			continue;
		ws_info("display %s%s: looks like %dx%d at %d,%d (%ux%u@%u.%02u "
		    "panel)%s%s", d->connector, d == m ? " (main)" : "",
		    d->frame.width, d->frame.height, d->frame.x, d->frame.y,
		    d->panel_width, d->panel_height, d->refresh_mhz / 1000,
		    d->refresh_mhz % 1000 / 10, d->mirror_of ? ", mirroring" :
		    "", d->builtin ? ", built in" : "");
	}
	ws_info("desktop %ux%u at %ux (%ux%u backing), %u display%s",
	    srv->width, srv->height, srv->scale, srv->back_width,
	    srv->back_height, srv->ndisplays, srv->ndisplays == 1 ? "" : "s");

	/* tell everyone: the displays, then the screen, then the windows */
	if (srv->scale != os) {
		(void)cursor_rescale(srv);
		(void)tile_rescale(srv);
	}
	if (overview_active(srv))
		overview_close(srv);
	display_changed_notify(srv);
	if (minx != 0 || miny != 0) {
		/* the desktop's corner moved: everything in it with it */
		desktop_shift(srv, -minx, -miny);
	}
	if (srv->width != ow || srv->height != oh || srv->scale != os)
		screen_changed(srv);
	else
		screen_displays_changed(srv);
	/* windows of displays that went away first (they remember them),
	   then the old screen's rule: a window that fitted still fits */
	windows_follow(srv, old, nold);
	windows_keep_on_screen(srv, ow, oh);
	display_night_shift(srv);
	/* a display turned on while the others sleep sleeps too */
	if (srv->displays_asleep && srv->output != NULL &&
	    srv->output->blank != NULL)
		for (i = 0; i < srv->ndisplays; i++)
			if (srv->displays[i]->on)
				srv->output->blank(srv, srv->displays[i],
				    true);
	/* the set comes back as it is now, unless part of it is off */
	all_on = true;
	for (i = 0; i < srv->ndisplays; i++)
		if (!srv->displays[i]->on)
			all_on = false;
	if (all_on && srv->ndisplays > 1)
		arrangement_save(srv);
}

/* ---- applying ---- */

/* At start, once the displays are known. */
void
display_init(struct ws_server *srv)
{
	struct ws_display *d;
	unsigned i;

	/* Also here: a server that stays root never calls
	   display_store_chown(). */
	store_migrate(srv);
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (d->edid_id[0] == '\0')
			snprintf(d->edid_id, sizeof(d->edid_id), "%s-%ux%u",
			    srv->headless ? "headless" : "unknown",
			    d->panel_width, d->panel_height);
		choices_load(srv, d);
	}
	srv->display_active = false;
	relayout(srv, true);
}

/*
 * A setting or a choice changed, or the displays did (force: a display
 * came, went or changed its panel): run at the layout it makes.
 * Returns 0 or an errno.
 */
int
display_apply(struct ws_server *srv, bool force)
{
	relayout(srv, force);
	return srv->main != NULL ? 0 : ENXIO;
}

/* DisplayLooksLike: the user's choice for the main display, or
   "Default". */
int
display_set_looks_like(struct ws_server *srv, const char *v)
{
	struct ws_display *d = srv->main;
	uint32_t w = 0, h = 0;
	char buf[32];
	int e;

	if (d == NULL)
		return ENXIO;
	if (strcasecmp(v, "Default") != 0 && (!parse_wxh(v, &w, &h) ||
	    !size_ok(d, w, h)))
		return EINVAL;
	if (w == d->modes[d->mode_default].w &&
	    h == d->modes[d->mode_default].h)
		w = h = 0;	/* the default: keep following it */
	d->chosen_w = w;
	d->chosen_h = h;
	snprintf(buf, sizeof(buf), "%ux%u", w, h);
	e = store_put(srv, d->edid_id, w ? buf : NULL);
	ws_info("display %s: looks like %s%s", d->edid_id,
	    w ? v : "the default", e ? " (not saved)" : "");
	return display_apply(srv, false);
}

/* ---- WS_MSG_DISPLAY_CONFIGURE ---- */

static bool
any_mirror(struct ws_server *srv)
{
	unsigned i;

	for (i = 0; i < srv->ndisplays; i++)
		if (srv->displays[i]->mirror_of != 0)
			return (true);
	return (false);
}

/*
 * Mirroring begins: remember the arrangement, as places relative to
 * one another, and the main display, so turning it off gives them back
 * (macOS does); the desktop's corner moves while mirrored, the source
 * becomes the main display, and swaps of the source ("Optimize for")
 * don't change what's remembered.
 */
static void
unmirrored_save(struct ws_server *srv)
{
	struct ws_display *d;
	unsigned i;

	srv->nunmirrored = 0;
	strlcpy(srv->unmirrored_main, srv->main != NULL ?
	    srv->main->edid_id : "", sizeof(srv->unmirrored_main));
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!display_shows(d))
			continue;
		strlcpy(srv->unmirrored[srv->nunmirrored].edid_id, d->edid_id,
		    sizeof(srv->unmirrored[0].edid_id));
		srv->unmirrored[srv->nunmirrored].x = d->frame.x;
		srv->unmirrored[srv->nunmirrored].y = d->frame.y;
		srv->nunmirrored++;
	}
}

/*
 * d stopped mirroring: its place from before. When none mirrors any
 * more, every display goes back to its place, offset so the displays
 * that stayed put stay put, and the main display from before is main
 * again. A display with no place is placed anew.
 */
static void
unmirrored_restore(struct ws_server *srv, struct ws_display *d)
{
	struct ws_display *o, *ref = NULL;
	int32_t dx = 0, dy = 0;
	unsigned i, k;
	bool all = !any_mirror(srv);

	/* the offset: from a display that kept showing its own part */
	for (i = 0; i < srv->ndisplays && ref == NULL; i++) {
		o = srv->displays[i];
		if (o == d || !display_shows(o))
			continue;
		for (k = 0; k < srv->nunmirrored; k++)
			if (strcmp(srv->unmirrored[k].edid_id,
			    o->edid_id) == 0) {
				ref = o;
				dx = o->frame.x - srv->unmirrored[k].x;
				dy = o->frame.y - srv->unmirrored[k].y;
				break;
			}
	}
	for (i = 0; i < srv->ndisplays; i++) {
		o = srv->displays[i];
		if (!all && o != d)
			continue;
		if (!display_shows(o) && o != d)
			continue;
		for (k = 0; k < srv->nunmirrored; k++)
			if (strcmp(srv->unmirrored[k].edid_id,
			    o->edid_id) == 0)
				break;
		if (k == srv->nunmirrored) {
			if (o == d)
				o->place_afresh = true;
			continue;
		}
		o->frame.x = srv->unmirrored[k].x + dx;
		o->frame.y = srv->unmirrored[k].y + dy;
		if (all && srv->unmirrored_main[0] != '\0' &&
		    strcmp(o->edid_id, srv->unmirrored_main) == 0)
			srv->main = o;
	}
	if (all) {
		srv->nunmirrored = 0;
		srv->unmirrored_main[0] = '\0';
	}
}

/*
 * The state a confirmed change goes back to: every display's choices,
 * the arrangement, the main display and mirrors, as text.
 */
static char *
state_save(struct ws_server *srv)
{
	struct ws_display *d;
	char *buf;
	size_t len = 2048;
	unsigned i;

	buf = malloc(len);
	if (buf == NULL)
		return NULL;
	arrangement_text(srv, buf, len);
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		snprintf(buf + strlen(buf), len - strlen(buf),
		    "|%s:%u,%u,%u,%u,%u", d->edid_id, d->chosen_w,
		    d->chosen_h, d->want_w, d->want_h, d->want_mhz);
	}
	return buf;
}

static void
state_restore(struct ws_server *srv, const char *saved)
{
	struct ws_display *d;
	char *text, *p, *tok, *colon;
	unsigned cw, ch, ww, wh, wm, i;
	char key[64], val[32];
	int32_t wx[WS_MAX_DISPLAYS], wy[WS_MAX_DISPLAYS];
	bool have[WS_MAX_DISPLAYS];
	struct ws_display *m = NULL;

	text = strdup(saved);
	if (text == NULL)
		return;
	p = strchr(text, '|');
	if (p != NULL)
		*p++ = '\0';
	/* the choices */
	while (p != NULL && (tok = strsep(&p, "|")) != NULL) {
		colon = strrchr(tok, ':');
		if (colon == NULL)
			continue;
		*colon = '\0';
		if ((d = by_edid(srv, tok)) == NULL ||
		    sscanf(colon + 1, "%u,%u,%u,%u,%u", &cw, &ch, &ww, &wh,
		    &wm) != 5)
			continue;
		d->chosen_w = cw;
		d->chosen_h = ch;
		d->want_w = ww;
		d->want_h = wh;
		d->want_mhz = wm;
		snprintf(val, sizeof(val), "%ux%u", cw, ch);
		(void)store_put(srv, d->edid_id, cw ? val : NULL);
		snprintf(key, sizeof(key), "%s/mode", d->edid_id);
		snprintf(val, sizeof(val), "%ux%u@%u", ww, wh, wm);
		(void)store_put(srv, key, ww ? val : NULL);
	}
	/* the arrangement */
	set_key(srv, key, sizeof(key));
	(void)store_put(srv, key, text[0] != '\0' ? text : NULL);
	memset(have, 0, sizeof(have));
	for (i = 0; i < srv->ndisplays; i++)
		srv->displays[i]->mirror_of = 0;
	if (arrangement_load(srv, wx, wy, have, &m)) {
		for (i = 0; i < srv->ndisplays; i++)
			if (have[i]) {
				srv->displays[i]->frame.x = wx[i];
				srv->displays[i]->frame.y = wy[i];
			}
		if (m != NULL)
			srv->main = m;
	}
	free(text);
}

static void
pending_end(struct ws_server *srv)
{
	srv->display_pending = false;
	free(srv->display_saved);
	srv->display_saved = NULL;
	ws_timer_cancel(srv, TIMER_DISPLAY_REVERT);
}

/* WS_DISPLAY_REVERT, or the time ran out. */
static void
pending_revert(struct ws_server *srv)
{
	char *saved = srv->display_saved;

	if (!srv->display_pending || saved == NULL)
		return;
	srv->display_saved = NULL;
	pending_end(srv);
	ws_info("displays: going back to the settings before");
	state_restore(srv, saved);
	free(saved);
	relayout(srv, true);
}

void
display_revert_fire(struct ws_server *srv)
{
	if (srv->display_pending && ws_now_usec() + 50000 >=
	    srv->display_pending_until)
		pending_revert(srv);
}

/* Does d have this mode (w x h, at mhz or any when 0)? The highest
   refresh of that size in *mhz_out. */
static bool
panel_has(const struct ws_display *d, uint32_t w, uint32_t h, uint32_t mhz,
    uint32_t *mhz_out)
{
	unsigned i;
	bool found = false;

	*mhz_out = 0;
	for (i = 0; i < d->npanel; i++)
		if (d->panel[i].w == w && d->panel[i].h == h &&
		    (mhz == 0 || d->panel[i].mhz / 1000 == mhz / 1000)) {
			if (!found || d->panel[i].mhz > *mhz_out)
				*mhz_out = d->panel[i].mhz;
			found = true;
		}
	return found;
}

int
display_configure(struct ws_server *srv, const ws_req_display_t *m)
{
	struct ws_display *d, *src;
	char key[64], val[32], *saved = NULL;
	uint32_t mhz;
	bool force = false;
	unsigned i;

	if (m->op == WS_DISPLAY_KEEP) {
		if (!srv->display_pending)
			return ENOENT;
		ws_info("displays: keeping the new settings");
		pending_end(srv);
		display_changed_notify(srv);
		return 0;
	}
	if (m->op == WS_DISPLAY_REVERT) {
		if (!srv->display_pending)
			return ENOENT;
		pending_revert(srv);
		return 0;
	}
	d = display_lookup(srv, m->display_id);
	if (d == NULL || !d->on)
		return ENOENT;
	if ((m->flags & WS_DISPLAY_CONFIRM) && !srv->display_pending) {
		/* (while one is pending, this joins it: reverting undoes
		   both) */
		saved = state_save(srv);
		if (saved == NULL)
			return ENOMEM;
	}
	switch (m->op) {
	case WS_DISPLAY_SET_LOOKS_LIKE:
		if (m->width == 0 && m->height == 0) {
			d->chosen_w = d->chosen_h = 0;
		} else if (!size_ok(d, m->width, m->height)) {
			free(saved);
			return EINVAL;
		} else if (m->width == d->modes[d->mode_default].w &&
		    m->height == d->modes[d->mode_default].h) {
			d->chosen_w = d->chosen_h = 0;
		} else {
			d->chosen_w = m->width;
			d->chosen_h = m->height;
		}
		snprintf(val, sizeof(val), "%ux%u", d->chosen_w, d->chosen_h);
		(void)store_put(srv, d->edid_id, d->chosen_w ? val : NULL);
		ws_info("display %s: looks like %s", d->connector,
		    d->chosen_w ? val : "the default");
		break;
	case WS_DISPLAY_SET_MODE:
		if (m->width == 0 && m->height == 0) {
			d->want_w = d->want_h = d->want_mhz = 0;
		} else if (d->npanel == 0 || !panel_has(d, m->width,
		    m->height, m->refresh_mhz, &mhz)) {
			free(saved);
			return EINVAL;
		} else {
			d->want_w = m->width;
			d->want_h = m->height;
			d->want_mhz = mhz;
		}
		/* a "looks like" size of the old mode may not fit the new
		   one */
		d->chosen_w = d->chosen_h = 0;
		(void)store_put(srv, d->edid_id, NULL);
		snprintf(key, sizeof(key), "%s/mode", d->edid_id);
		snprintf(val, sizeof(val), "%ux%u@%u", d->want_w, d->want_h,
		    d->want_mhz);
		(void)store_put(srv, key, d->want_w ? val : NULL);
		ws_info("display %s: mode %s", d->connector,
		    d->want_w ? val : "preferred");
		force = true;
		break;
	case WS_DISPLAY_SET_ORIGIN: {
		struct ws_display *placed[WS_MAX_DISPLAYS];
		unsigned n = 0;

		if (d->mirror_of != 0) {
			free(saved);
			return EINVAL;
		}
		for (i = 0; i < srv->ndisplays; i++)
			if (srv->displays[i] != d &&
			    display_shows(srv->displays[i]))
				placed[n++] = srv->displays[i];
		snap(d, placed, n, m->x, m->y);
		ws_info("display %s: placed at %d,%d", d->connector,
		    d->frame.x, d->frame.y);
		break;
	}
	case WS_DISPLAY_SET_MAIN:
		if (d->mirror_of != 0) {
			free(saved);
			return EINVAL;
		}
		srv->main = d;
		ws_info("display %s: the main display", d->connector);
		break;
	case WS_DISPLAY_SET_MIRROR:
		if (m->arg == 0) {
			if (d->mirror_of != 0) {
				/* back where it was before mirroring, else
				   placed as a new display */
				d->mirror_of = 0;
				unmirrored_restore(srv, d);
			}
		} else {
			src = display_lookup(srv, m->arg);
			if (src == NULL || src == d || !src->on ||
			    (src->mirror_of != 0 && src->mirror_of != d->id)) {
				free(saved);
				return EINVAL;
			}
			/*
			 * src mirrored d: they trade places, src showing its
			 * own part ("Optimize for" in Displays: the source
			 * fills its panel, the mirrors fit its shape)
			 */
			if (src->mirror_of == d->id)
				src->mirror_of = 0;
			else if (!any_mirror(srv))
				unmirrored_save(srv);	/* mirroring begins */
			/* what mirrored d mirrors src now */
			for (i = 0; i < srv->ndisplays; i++)
				if (srv->displays[i]->mirror_of == d->id)
					srv->displays[i]->mirror_of = src->id;
			d->mirror_of = src->id;
			if (srv->main == d)
				srv->main = src;
		}
		ws_info("display %s: %s", d->connector, d->mirror_of ?
		    "mirrors" : "extends the desktop");
		force = true;
		break;
	default:
		free(saved);
		return EINVAL;
	}
	if (saved != NULL || (srv->display_pending &&
	    (m->flags & WS_DISPLAY_CONFIRM))) {
		if (saved != NULL)
			srv->display_saved = saved;
		srv->display_pending = true;
		srv->display_pending_until = ws_now_usec() +
		    (uint64_t)WS_DISPLAY_REVERT_MS * 1000;
		ws_timer_set(srv, TIMER_DISPLAY_REVERT, WS_DISPLAY_REVERT_MS,
		    true);
	}
	relayout(srv, force);
	arrangement_save(srv);
	return 0;
}

/* ---- the settings' read-only entries (the main display's) ---- */

int
display_info(struct ws_server *srv, unsigned i, char *key, size_t klen,
    char *value, size_t vlen)
{
	struct ws_display *d = srv->main;
	unsigned j;

	value[0] = '\0';
	if (d == NULL)
		return -1;
	switch (i) {
	case 0:
		strlcpy(key, "DisplayID", klen);
		strlcpy(value, d->edid_id, vlen);
		break;
	case 1:
		strlcpy(key, "DisplayName", klen);
		strlcpy(value, d->name[0] ? d->name : srv->headless ?
		    "Headless" : "Display", vlen);
		break;
	case 2:
		strlcpy(key, "DisplayPanel", klen);
		snprintf(value, vlen, "%ux%u", d->panel_width,
		    d->panel_height);
		break;
	case 3:
		strlcpy(key, "DisplayModes", klen);
		for (j = 0; j < d->nmodes; j++)
			snprintf(value + strlen(value), vlen - strlen(value),
			    "%s%ux%u", j ? "," : "", d->modes[j].w,
			    d->modes[j].h);
		break;
	case 4:
		strlcpy(key, "DisplayDefault", klen);
		snprintf(value, vlen, "%ux%u", d->modes[d->mode_default].w,
		    d->modes[d->mode_default].h);
		break;
	case 5:
		strlcpy(key, "DisplayCurrent", klen);
		snprintf(value, vlen, "%ux%u@%u", d->frame.width,
		    d->frame.height, srv->scale);
		break;
	case 6:
		strlcpy(key, "DisplayLooksLike", klen);
		if (d->chosen_w != 0)
			snprintf(value, vlen, "%ux%u", d->chosen_w,
			    d->chosen_h);
		else
			strlcpy(value, "Default", vlen);
		break;
	default:
		return -1;
	}
	return 0;
}

/* ---- Night Shift ---- */

static bool
parse_hhmm(const char *s, int *min)
{
	int h, m;

	if (sscanf(s, "%d:%d", &h, &m) != 2 || h < 0 || h > 23 || m < 0 ||
	    m > 59)
		return false;
	*min = h * 60 + m;
	return true;
}

static bool
parse_schedule(const char *v, int *from, int *to)
{
	char a[8], b[8];

	return sscanf(v, "%7[0-9:]-%7[0-9:]", a, b) == 2 &&
	    parse_hhmm(a, from) && parse_hhmm(b, to);
}

bool
display_night_shift_ok(const struct ws_config *cfg, const char *key)
{
	int a, b;

	if (strcmp(key, "NightShift") == 0)
		return strcasecmp(cfg->night_shift, "Off") == 0 ||
		    strcasecmp(cfg->night_shift, "On") == 0 ||
		    strcasecmp(cfg->night_shift, "Schedule") == 0;
	if (strcmp(key, "NightShiftSchedule") == 0)
		return parse_schedule(cfg->night_shift_schedule, &a, &b);
	if (strcmp(key, "NightShiftTemperature") == 0)
		return cfg->night_shift_temperature >= 1900 &&
		    cfg->night_shift_temperature <= 6500;
	return true;
}

/* How warm now: 0 (none) to 1 (NightShiftTemperature). */
static double
night_strength(struct ws_server *srv)
{
	const struct ws_config *cfg = &srv->cfg;
	int from, to, now, in, len, left;
	struct tm tm;
	time_t t;
	double sec;

	if (strcasecmp(cfg->night_shift, "On") == 0)
		return 1.0;
	if (strcasecmp(cfg->night_shift, "Schedule") != 0 ||
	    !parse_schedule(cfg->night_shift_schedule, &from, &to) ||
	    from == to)
		return 0.0;
	t = time(NULL);
	localtime_r(&t, &tm);
	sec = tm.tm_sec;
	now = tm.tm_hour * 60 + tm.tm_min;
	len = (to - from + 1440) % 1440;
	in = (now - from + 1440) % 1440;
	if (in >= len)
		return 0.0;
	/* fade in over the first minute, out over the last */
	left = len - in;
	if (in == 0)
		return sec / NIGHT_FADE_SEC;
	if (left == 1)
		return 1.0 - sec / NIGHT_FADE_SEC;
	return 1.0;
}

/* The colour of a black body at k kelvin, as display gains (6500 K is
   1, 1, 1). Tanner Helland's fit. */
static void
kelvin_rgb(double k, double *r, double *g, double *b)
{
	double t = k / 100.0;

	if (t <= 66)
		*r = 1.0;
	else
		*r = fmin(1.0, 1.29293618606 * pow(t - 60, -0.1332047592));
	if (t <= 66)
		*g = fmin(1.0, fmax(0.0, (99.4708025861 * log(t) -
		    161.1195681661) / 255.0));
	else
		*g = fmin(1.0, 1.12989086089 * pow(t - 60, -0.0755148492));
	if (t >= 66)
		*b = 1.0;
	else if (t <= 19)
		*b = 0.0;
	else
		*b = fmin(1.0, fmax(0.0, (138.5177312231 * log(t - 10) -
		    305.0447927307) / 255.0));
}

/* Put Night Shift's warmth for now on every display (when it changed). */
void
display_night_shift(struct ws_server *srv)
{
	struct ws_display *d;
	double s, k, r, g, b, r0, g0, b0;
	unsigned i;

	if (srv->output == NULL || srv->output->gamma == NULL)
		return;
	s = night_strength(srv);
	k = 6500.0 - s * (6500.0 - srv->cfg.night_shift_temperature);
	kelvin_rgb(k, &r, &g, &b);
	kelvin_rgb(6500.0, &r0, &g0, &b0);
	r /= r0;
	g /= g0;
	b /= b0;
	for (i = 0; i < srv->ndisplays; i++) {
		d = srv->displays[i];
		if (!d->on || (fabs(d->gamma_r - r) < 0.002 &&
		    fabs(d->gamma_g - g) < 0.002 &&
		    fabs(d->gamma_b - b) < 0.002))
			continue;
		srv->output->gamma(srv, d, r, g, b);
		if (fabs(d->gamma_r - r) > 0.05 || fabs(d->gamma_b - b) > 0.05)
			ws_info("display %s: Night Shift %.0f K", d->connector,
			    k);
		d->gamma_r = r;
		d->gamma_g = g;
		d->gamma_b = b;
	}
}

/* ---- display sleep ---- */

#define DISPLAY_STATUS_KEY	"com.apple.iokit.hid.displayStatus"

/*
 * libnotify (-lnotify). The installed <notify.h> includes <os/base.h>,
 * which isn't installed, so the three calls used are declared here, as
 * notify.h declares them.
 */
#define NOTIFY_STATUS_OK	0
uint32_t notify_register_check(const char *, int *);
uint32_t notify_check(int, int *);
uint32_t notify_get_state(int, uint64_t *);

/* Turn every display off (asleep) or on again. */
void
display_sleep(struct ws_server *srv, bool asleep)
{
	unsigned i;

	if (asleep == srv->displays_asleep)
		return;
	if (asleep && srv->present_pending && srv->output != NULL &&
	    srv->output->wait_present != NULL)
		srv->output->wait_present(srv);
	srv->displays_asleep = asleep;
	ws_info("displays %s", asleep ? "asleep" : "awake");
	if (srv->output != NULL && srv->output->blank != NULL)
		for (i = 0; i < srv->ndisplays; i++)
			if (srv->displays[i]->on)
				srv->output->blank(srv, srv->displays[i],
				    asleep);
	/* a frame whose flips won't come now is done */
	if (asleep && srv->present_pending)
		compositor_present_done(srv);
	if (!asleep) {
		damage_all(srv);
		cursor_moved(srv);
	}
}

/* Input while the displays are off wakes them at once. */
void
display_wake_input(struct ws_server *srv)
{
	if (srv->displays_asleep)
		display_sleep(srv, false);
}

/* powerd's display state: follow it when it changes. */
static void
display_status_check(struct ws_server *srv)
{
	uint64_t state = 1;
	int changed = 0;

	if (srv->headless)
		return;
	if (srv->display_status_token == -2 &&
	    notify_register_check(DISPLAY_STATUS_KEY,
	    &srv->display_status_token) != NOTIFY_STATUS_OK) {
		srv->display_status_token = -1;
		ws_warn("notify(3) %s: can't register; the displays never "
		    "sleep", DISPLAY_STATUS_KEY);
	}
	if (srv->display_status_token < 0 ||
	    notify_check(srv->display_status_token, &changed) !=
	    NOTIFY_STATUS_OK || !changed ||
	    notify_get_state(srv->display_status_token, &state) !=
	    NOTIFY_STATUS_OK)
		return;
	/* only a change counts: input may have woken the displays while
	   powerd still says 0 */
	if ((int)(state != 0) == srv->display_status_seen)
		return;
	srv->display_status_seen = state != 0;
	display_sleep(srv, state == 0);
}

/* Every couple of seconds (main.c): Night Shift's schedule, and whether
   the displays should sleep. */
void
display_housekeeping(struct ws_server *srv)
{
	display_night_shift(srv);
	display_status_check(srv);
}

/* ---- the lid ---- */

void
display_lid(struct ws_server *srv, bool closed)
{
	unsigned i;
	bool external = false;

	if (closed == srv->lid_closed)
		return;
	srv->lid_closed = closed;
	for (i = 0; i < srv->ndisplays; i++)
		if (!srv->displays[i]->builtin)
			external = true;
	ws_info("lid %s%s", closed ? "closed" : "opened",
	    external ? "" : " (no other display: the panel stays on)");
	if (external)
		relayout(srv, true);
}

/* ---- headless tests (WS_INJECT_DISPLAY, WS_INJECT_LID) ---- */

int
display_inject(struct ws_server *srv, const ws_req_inject_t *m)
{
	struct ws_display *d;
	uint32_t w = (uint32_t)m->x, h = (uint32_t)m->y;

	if (!srv->headless)
		return ENOTSUP;
	if (m->kind == WS_INJECT_LID) {
		display_lid(srv, m->pressed != 0);
		return 0;
	}
	if (!m->pressed) {
		d = display_lookup(srv, m->button);
		if (d == NULL)
			return ENOENT;
		if (srv->ndisplays == 1)
			return EBUSY;	/* a headless server keeps one */
		if (d->on && srv->output->display_off != NULL)
			srv->output->display_off(srv, d);
		display_remove(srv, d);
		relayout(srv, true);
		return 0;
	}
	if (w < 64 || h < 64 || w > 16384 || h > 16384)
		return EINVAL;
	d = display_add(srv);
	if (d == NULL)
		return ENOSPC;
	d->panel_width = w;
	d->panel_height = h;
	d->refresh_mhz = 60000;
	d->mm_width = (uint32_t)m->dx;
	d->mm_height = (uint32_t)m->dy;
	d->edid_trusted = d->mm_width != 0;
	snprintf(d->connector, sizeof(d->connector), "HEADLESS-%u", d->id);
	if (m->key_name[0] != '\0')
		strlcpy(d->edid_id, m->key_name, sizeof(d->edid_id));
	else
		snprintf(d->edid_id, sizeof(d->edid_id), "headless-%u-%ux%u",
		    d->id, w, h);
	choices_load(srv, d);
	ws_info("display %s plugged in: %ux%u (%s)", d->connector, w, h,
	    d->edid_id);
	relayout(srv, true);
	return 0;
}
