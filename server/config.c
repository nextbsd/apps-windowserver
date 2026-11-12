/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Configuration: built-in defaults, then a flat XML property list
 * (/System/Library/Preferences/org.nextbsd.WindowServer.plist), then
 * WS_<Key> environment variables, then the command line.
 *
 * The plist reader only understands a single top-level <dict> of
 * <key> followed by <string>, <integer>, <real>, <true/> or <false/>,
 * which is all this file needs.
 */

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

enum ctype { C_STR, C_INT, C_BOOL, C_COLOR };

struct ckey {
	const char	*name;
	enum ctype	type;
	size_t		offset;
	size_t		size;
};

#define S(n, f)	{ n, C_STR, offsetof(struct ws_config, f), \
		  sizeof(((struct ws_config *)0)->f) }
#define I(n, f)	{ n, C_INT, offsetof(struct ws_config, f), sizeof(int) }
#define B(n, f)	{ n, C_BOOL, offsetof(struct ws_config, f), sizeof(bool) }

static const struct ckey keys[] = {
	S("KeyboardRules", keyboard_rules),
	S("KeyboardModel", keyboard_model),
	S("KeyboardLayout", keyboard_layout),
	S("KeyboardVariant", keyboard_variant),
	S("KeyboardOptions", keyboard_options),
	I("KeyRepeatDelay", key_repeat_delay),
	I("KeyRepeatInterval", key_repeat_interval),
	B("ClickThrough", click_through),
	B("RaiseOnClick", raise_on_click),
	B("IgnoreMuxDevices", ignore_mux_devices),
	B("TapToClick", tap_to_click),
	B("NaturalScrolling", natural_scrolling),
	B("AllowTerminate", allow_terminate),
	B("HardwareCursor", hardware_cursor),
	{ "BackgroundColor", C_COLOR, offsetof(struct ws_config, background),
	  sizeof(uint32_t) },
	S("DRMDevice", drm_device),
	S("SocketPath", socket_path),
	S("ServiceName", service_name),
	S("User", user),
	S("Group", group),
	S("SnapshotPath", snapshot_path),
	I("MinimizeTileSize", minimize_tile_size),
	S("MinimizeTileImage", minimize_tile_image),
	B("MinimizeIntoDock", minimize_into_dock),
};

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
	}
	return -1;
}

static const struct ckey *
config_key(const char *name)
{
	size_t i;

	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (strcmp(keys[i].name, name) == 0)
			return &keys[i];
	return NULL;
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

int
config_load(struct ws_config *cfg, const char *path)
{
	char key[128], value[256];
	const struct ckey *k;
	const char *p;
	char *buf;
	FILE *f;
	long len;

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
		if (strncmp(p, "<true/>", 7) == 0) {
			strlcpy(value, "true", sizeof(value));
			p += 7;
		} else if (strncmp(p, "<false/>", 8) == 0) {
			strlcpy(value, "false", sizeof(value));
			p += 8;
		} else if (strncmp(p, "<string>", 8) == 0) {
			p = xml_text(p + 8, "string", value, sizeof(value));
		} else if (strncmp(p, "<integer>", 9) == 0) {
			p = xml_text(p + 9, "integer", value, sizeof(value));
		} else if (strncmp(p, "<real>", 6) == 0) {
			p = xml_text(p + 6, "real", value, sizeof(value));
		} else {
			ws_warn("%s: unsupported value type for key %s", path,
			    key);
			continue;
		}
		if (p == NULL)
			break;
		k = config_key(key);
		if (k == NULL) {
			ws_warn("%s: unknown key %s", path, key);
			continue;
		}
		if (config_set(cfg, k, value) < 0)
			ws_warn("%s: bad value for %s: %s", path, key, value);
	}
	free(buf);
	return 0;
}

void
config_env(struct ws_config *cfg)
{
	char name[160];
	const char *v;
	size_t i;

	for (i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
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
