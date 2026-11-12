/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Configuration: built-in defaults, then a flat XML property list
 * (/System/Library/Preferences/org.nextbsd.WindowServer.plist), then
 * WS_<Key> environment variables, then the command line. That is the
 * system's layer; the console user's values go on top of it while the
 * server runs (settings.c).
 *
 * The plist reader only understands a single top-level <dict> of
 * <key> followed by <string>, <integer>, <real>, <true/> or <false/>,
 * which is all this file needs.
 */

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

/* C_OPTREAL: a real that may be unset (NAN, "" as text). */
enum ctype { C_STR, C_INT, C_BOOL, C_COLOR, C_REAL, C_OPTREAL };

struct ckey {
	const char	*name;
	enum ctype	type;
	size_t		offset;
	size_t		size;
	unsigned	flags;		/* CONFIG_* (server.h) */
};

#define S(n, f, fl) { n, C_STR, offsetof(struct ws_config, f), \
		  sizeof(((struct ws_config *)0)->f), fl }
#define I(n, f, fl) { n, C_INT, offsetof(struct ws_config, f), sizeof(int), fl }
#define B(n, f, fl) { n, C_BOOL, offsetof(struct ws_config, f), sizeof(bool), \
		  fl }
#define R(n, f, fl) { n, C_REAL, offsetof(struct ws_config, f), \
		  sizeof(double), fl }
#define O(n, f, fl) { n, C_OPTREAL, offsetof(struct ws_config, f), \
		  sizeof(double), fl }

/*
 * Every key, with what a live change to it needs (0: the server's own,
 * read at start only; settings.c refuses it with EPERM).
 */
static const struct ckey keys[] = {
	S("KeyboardRules", keyboard_rules, CONFIG_KEYMAP),
	S("KeyboardModel", keyboard_model, CONFIG_KEYMAP),
	S("KeyboardLayout", keyboard_layout, CONFIG_KEYMAP),
	S("KeyboardVariant", keyboard_variant, CONFIG_KEYMAP),
	S("KeyboardOptions", keyboard_options, CONFIG_KEYMAP),
	S("KeyboardLayoutShortcut", keyboard_layout_shortcut, CONFIG_KEYMAP),
	I("KeyRepeatDelay", key_repeat_delay, CONFIG_REPEAT),
	I("KeyRepeatInterval", key_repeat_interval, CONFIG_REPEAT),
	B("ClickThrough", click_through, CONFIG_LIVE),
	B("RaiseOnClick", raise_on_click, CONFIG_LIVE),
	B("IgnoreMuxDevices", ignore_mux_devices, 0),
	B("TapToClick", tap_to_click, CONFIG_DEVICE),
	B("TapToDrag", tap_to_drag, CONFIG_DEVICE),
	B("ThreeFingerDrag", three_finger_drag, CONFIG_DEVICE),
	S("SecondaryClick", secondary_click, CONFIG_DEVICE),
	R("PointerSpeed", pointer_speed, CONFIG_DEVICE),
	O("TrackpadSpeed", trackpad_speed, CONFIG_DEVICE),
	O("MouseSpeed", mouse_speed, CONFIG_DEVICE),
	S("MouseAccelProfile", mouse_accel_profile, CONFIG_DEVICE),
	B("LeftHanded", left_handed, CONFIG_DEVICE),
	R("ScrollSpeed", scroll_speed, CONFIG_LIVE),
	B("MomentumScrolling", momentum_scrolling, CONFIG_LIVE),
	B("NaturalScrolling", natural_scrolling, CONFIG_LIVE),
	B("AllowTerminate", allow_terminate, 0),
	B("HardwareCursor", hardware_cursor, 0),
	{ "BackgroundColor", C_COLOR, offsetof(struct ws_config, background),
	  sizeof(uint32_t), 0 },
	S("DRMDevice", drm_device, 0),
	S("SocketPath", socket_path, 0),
	S("ServiceName", service_name, 0),
	S("User", user, 0),
	S("Group", group, 0),
	S("SnapshotPath", snapshot_path, 0),
	I("MinimizeTileSize", minimize_tile_size, 0),
	S("MinimizeTileImage", minimize_tile_image, 0),
	/* per user through the minimize request (NBMinimizeStyle) for now;
	   apps-windowserver#27 */
	B("MinimizeIntoDock", minimize_into_dock, 0),
	B("StartKeyShowsOverview", start_key_overview, CONFIG_LIVE),
	B("ReduceMotion", reduce_motion, CONFIG_LIVE),
	/* window tiling (tiling.c, "Window tiling" in WSProtocol.h) */
	S("WindowMode", window_mode, CONFIG_TILING),
	S("TilingLayout", tiling_layout, CONFIG_TILING),
	I("TilingGapsInner", tiling_gaps_inner, CONFIG_TILING),
	I("TilingGapsOuter", tiling_gaps_outer, CONFIG_TILING),
	R("TilingMasterRatio", tiling_master_ratio, CONFIG_TILING),
	I("TilingBorder", tiling_border, CONFIG_TILING),
	{ "TilingBorderColor", C_COLOR, offsetof(struct ws_config,
	  tiling_border_color), sizeof(uint32_t), CONFIG_TILING },
	S("TilingModifier", tiling_modifier, CONFIG_TILING),
	S("TilingNewWindowSplits", tiling_new_window_splits, CONFIG_TILING),
	B("TilingFocusFollowsMouse", tiling_focus_follows_mouse, CONFIG_TILING),
};

#define NKEYS	(sizeof(keys) / sizeof(keys[0]))

void
config_defaults(struct ws_config *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	strlcpy(cfg->keyboard_rules, "evdev", sizeof(cfg->keyboard_rules));
	strlcpy(cfg->keyboard_model, "pc105", sizeof(cfg->keyboard_model));
	strlcpy(cfg->keyboard_layout, "us", sizeof(cfg->keyboard_layout));
	cfg->key_repeat_delay = 500;
	cfg->key_repeat_interval = 33;
	cfg->click_through = true;
	cfg->raise_on_click = true;
	cfg->ignore_mux_devices = false;
	cfg->tap_to_click = true;
	cfg->tap_to_drag = true;
	cfg->three_finger_drag = false;
	strlcpy(cfg->secondary_click, "TwoFingers",
	    sizeof(cfg->secondary_click));
	cfg->pointer_speed = 0.0;
	cfg->trackpad_speed = NAN;	/* PointerSpeed */
	cfg->mouse_speed = NAN;
	strlcpy(cfg->mouse_accel_profile, "adaptive",
	    sizeof(cfg->mouse_accel_profile));
	cfg->left_handed = false;
	strlcpy(cfg->keyboard_layout_shortcut, "None",
	    sizeof(cfg->keyboard_layout_shortcut));
	cfg->scroll_speed = 1.0;
	cfg->momentum_scrolling = true;
	cfg->natural_scrolling = false;
	cfg->allow_terminate = true;
	cfg->hardware_cursor = true;
	/* Black where no window is. Desktop pictures come from a desktop-level
	   window (InnerSpace, or a Dock/Workspace desktop), as on macOS. */
	cfg->background = 0x000000;
	strlcpy(cfg->service_name, WS_SERVICE_NAME, sizeof(cfg->service_name));
	strlcpy(cfg->user, "_windowserver", sizeof(cfg->user));
	strlcpy(cfg->group, "video", sizeof(cfg->group));
	strlcpy(cfg->snapshot_path, "/tmp/WindowServer-snapshot.ppm",
	    sizeof(cfg->snapshot_path));
	/* The size of a GNUstep app icon, so tiles line up with icons. */
	cfg->minimize_tile_size = 64;
	cfg->start_key_overview = true;
	cfg->reduce_motion = false;
	/* Windows stack until the user (Workspace Preferences) or the
	   theme picks tiling. */
	strlcpy(cfg->window_mode, "stacking", sizeof(cfg->window_mode));
	strlcpy(cfg->tiling_layout, "dwindle", sizeof(cfg->tiling_layout));
	cfg->tiling_gaps_inner = 8;
	cfg->tiling_gaps_outer = 8;
	cfg->tiling_master_ratio = 0.55;
	cfg->tiling_border = 2;
	cfg->tiling_border_color = 0x4a7fc0;
	strlcpy(cfg->tiling_modifier, "Command+Option",
	    sizeof(cfg->tiling_modifier));
	strlcpy(cfg->tiling_new_window_splits, "largest",
	    sizeof(cfg->tiling_new_window_splits));
	cfg->tiling_focus_follows_mouse = true;
}

static int
config_set(struct ws_config *cfg, const struct ckey *k, const char *value)
{
	char *p = (char *)cfg + k->offset;
	char *end;
	long v;

	switch (k->type) {
	case C_STR:
		strlcpy(p, value, k->size);
		return 0;
	case C_INT:
		errno = 0;
		v = strtol(value, &end, 0);
		if (errno != 0 || *end != '\0')
			return -1;
		*(int *)(void *)p = (int)v;
		return 0;
	case C_BOOL:
		if (strcasecmp(value, "true") == 0 ||
		    strcasecmp(value, "yes") == 0 || strcmp(value, "1") == 0)
			*(bool *)p = true;
		else if (strcasecmp(value, "false") == 0 ||
		    strcasecmp(value, "no") == 0 || strcmp(value, "0") == 0)
			*(bool *)p = false;
		else
			return -1;
		return 0;
	case C_COLOR:
		if (*value == '#')
			value++;
		errno = 0;
		v = strtol(value, &end, 16);
		if (errno != 0 || *end != '\0')
			return -1;
		*(uint32_t *)(void *)p = (uint32_t)v & 0xffffff;
		return 0;
	case C_OPTREAL:
		if (*value == '\0') {
			*(double *)(void *)p = NAN;
			return 0;
		}
		/* FALLTHROUGH */
	case C_REAL: {
		double d;

		errno = 0;
		d = strtod(value, &end);
		if (errno != 0 || *end != '\0' || end == value || !isfinite(d))
			return -1;
		*(double *)(void *)p = d;
		return 0;
	}
	}
	return -1;
}

static const struct ckey *
config_key(const char *name)
{
	size_t i;

	for (i = 0; i < NKEYS; i++)
		if (strcmp(keys[i].name, name) == 0)
			return &keys[i];
	return NULL;
}

/* ---- the key table, for settings.c ---- */

int
config_nkeys(void)
{
	return (int)NKEYS;
}

int
config_find(const char *name)
{
	const struct ckey *k = config_key(name);

	return k != NULL ? (int)(k - keys) : -1;
}

const char *
config_key_name(int i)
{
	return keys[i].name;
}

unsigned
config_key_flags(int i)
{
	return keys[i].flags;
}

/* The key's type as WS_SETTING_* (WSTypes.h). */
int
config_key_type(int i)
{
	switch (keys[i].type) {
	case C_INT:
		return WS_SETTING_INTEGER;
	case C_BOOL:
		return WS_SETTING_BOOLEAN;
	case C_REAL:
	case C_OPTREAL:
		return WS_SETTING_REAL;
	default:
		return WS_SETTING_STRING;
	}
}

_Static_assert(NKEYS <= CONFIG_MAX_KEYS, "raise CONFIG_MAX_KEYS");

/* Set key i from text; a string too long for it is refused. */
int
config_set_text(struct ws_config *cfg, int i, const char *value)
{
	if (keys[i].type == C_STR && strlen(value) >= keys[i].size)
		return -1;
	return config_set(cfg, &keys[i], value);
}

/* The value as text; returns false if it didn't fit. */
bool
config_get_text(const struct ws_config *cfg, int i, char *buf, size_t len)
{
	const char *p = (const char *)cfg + keys[i].offset;
	double d;
	int n;

	switch (keys[i].type) {
	case C_STR:
		return strlcpy(buf, p, len) < len;
	case C_INT:
		n = snprintf(buf, len, "%d", *(const int *)(const void *)p);
		break;
	case C_BOOL:
		n = snprintf(buf, len, "%s", *(const bool *)p ? "true" :
		    "false");
		break;
	case C_COLOR:
		n = snprintf(buf, len, "%06x",
		    *(const uint32_t *)(const void *)p);
		break;
	case C_OPTREAL:
	case C_REAL:
		d = *(const double *)(const void *)p;
		n = isnan(d) ? snprintf(buf, len, "%s", "") :
		    snprintf(buf, len, "%g", d);
		break;
	default:
		n = -1;
		break;
	}
	return n >= 0 && (size_t)n < len;
}

bool
config_equal(const struct ws_config *a, const struct ws_config *b, int i)
{
	const char *pa = (const char *)a + keys[i].offset;
	const char *pb = (const char *)b + keys[i].offset;
	double da, db;

	switch (keys[i].type) {
	case C_STR:
		return strcmp(pa, pb) == 0;
	case C_OPTREAL:
	case C_REAL:
		da = *(const double *)(const void *)pa;
		db = *(const double *)(const void *)pb;
		return (isnan(da) && isnan(db)) || da == db;
	default:
		return memcmp(pa, pb, keys[i].size) == 0;
	}
}

void
config_copy(struct ws_config *dst, const struct ws_config *src, int i)
{
	memcpy((char *)dst + keys[i].offset,
	    (const char *)src + keys[i].offset, keys[i].size);
}

/* Copy the text between <tag> and </tag> starting at p; return the end. */
static const char *
xml_text(const char *p, const char *tag, char *out, size_t outlen)
{
	char close[32];
	const char *e;
	size_t n;

	snprintf(close, sizeof(close), "</%s>", tag);
	e = strstr(p, close);
	if (e == NULL)
		return NULL;
	n = (size_t)(e - p);
	if (n >= outlen)
		n = outlen - 1;
	memcpy(out, p, n);
	out[n] = '\0';
	return e + strlen(close);
}

/*
 * Calls fn(key, value, type, ctx) for each <key> of a flat plist, with
 * the value as text and its type (WS_SETTING_*, 0 for anything else).
 * Returns -1 if the file can't be read; a missing file is empty.
 */
static int
plist_each(const char *path, void (*fn)(const char *, const char *, int,
    void *), void *ctx)
{
	char key[128], value[256];
	const char *p;
	char *buf;
	FILE *f;
	long len;
	int type;

	f = fopen(path, "r");
	if (f == NULL)
		return errno == ENOENT ? 0 : -1;
	if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
	    len > 1024 * 1024 || fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return -1;
	}
	buf = malloc((size_t)len + 1);
	if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
		free(buf);
		fclose(f);
		return -1;
	}
	buf[len] = '\0';
	fclose(f);

	p = buf;
	while ((p = strstr(p, "<key>")) != NULL) {
		p = xml_text(p + 5, "key", key, sizeof(key));
		if (p == NULL)
			break;
		while (isspace((unsigned char)*p))
			p++;
		value[0] = '\0';
		type = WS_SETTING_STRING;
		if (strncmp(p, "<true/>", 7) == 0) {
			strlcpy(value, "true", sizeof(value));
			type = WS_SETTING_BOOLEAN;
			p += 7;
		} else if (strncmp(p, "<false/>", 8) == 0) {
			strlcpy(value, "false", sizeof(value));
			type = WS_SETTING_BOOLEAN;
			p += 8;
		} else if (strncmp(p, "<string/>", 9) == 0) {
			p += 9;
		} else if (strncmp(p, "<string>", 8) == 0) {
			p = xml_text(p + 8, "string", value, sizeof(value));
		} else if (strncmp(p, "<integer>", 9) == 0) {
			type = WS_SETTING_INTEGER;
			p = xml_text(p + 9, "integer", value, sizeof(value));
		} else if (strncmp(p, "<real>", 6) == 0) {
			type = WS_SETTING_REAL;
			p = xml_text(p + 6, "real", value, sizeof(value));
		} else
			type = 0;
		if (p == NULL)
			break;
		fn(key, value, type, ctx);
	}
	free(buf);
	return 0;
}

struct load_ctx {
	struct ws_config *cfg;
	const char	*path;
};

static void
load_one(const char *key, const char *value, int type, void *arg)
{
	struct load_ctx *lc = arg;
	const struct ckey *k;

	if (type == 0) {
		ws_warn("%s: unsupported value type for key %s", lc->path,
		    key);
		return;
	}
	k = config_key(key);
	if (k == NULL) {
		ws_warn("%s: unknown key %s", lc->path, key);
		return;
	}
	if (config_set(lc->cfg, k, value) < 0)
		ws_warn("%s: bad value for %s: %s", lc->path, key, value);
}

int
config_load(struct ws_config *cfg, const char *path)
{
	struct load_ctx lc = { cfg, path };

	return plist_each(path, load_one, &lc);
}

void
config_env(struct ws_config *cfg)
{
	char name[160];
	const char *v;
	size_t i;

	for (i = 0; i < NKEYS; i++) {
		snprintf(name, sizeof(name), "WS_%s", keys[i].name);
		v = getenv(name);
		if (v != NULL && config_set(cfg, &keys[i], v) < 0)
			ws_warn("bad value in %s: %s", name, v);
	}
	/* The client-side override also applies to the server. */
	v = getenv(WS_SERVICE_NAME_ENV);
	if (v != NULL && *v != '\0')
		strlcpy(cfg->service_name, v, sizeof(cfg->service_name));
}
