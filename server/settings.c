/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Settings that change while the server runs ("Settings" in
 * WSProtocol.h, README "Settings"). Two layers:
 *
 *   srv->base     the system's: built-in defaults, the system plist,
 *                 WS_<Key> and the command line (config.c), read at
 *                 start, on SIGHUP and on WS_SETTINGS_RELOAD_SYSTEM;
 *   srv->session  the console user's values for the keys marked in
 *                 srv->session_set, from WS_MSG_SET_SETTINGS.
 *
 * srv->cfg, what the rest of the server reads, is the base with the
 * session's values on top. The session's values belong to the console
 * user who was logged in when they were set (srv->session_uid); when
 * that changes, they're dropped.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "server.h"

/* The system's layer, as at start. */
void
config_load_system(struct ws_server *srv, struct ws_config *cfg)
{
	config_defaults(cfg);
	if (config_load(cfg, srv->config_path) < 0)
		ws_warn("can't read %s: %s", srv->config_path, strerror(errno));
	config_env(cfg);
	if (srv->opt_socket != NULL)
		strlcpy(cfg->socket_path, srv->opt_socket,
		    sizeof(cfg->socket_path));
	if (srv->opt_service != NULL)
		strlcpy(cfg->service_name, srv->opt_service,
		    sizeof(cfg->service_name));
}

void
settings_init(struct ws_server *srv)
{
	config_load_system(srv, &srv->base);
	srv->cfg = srv->base;
	memset(&srv->session, 0, sizeof(srv->session));
	memset(srv->session_set, 0, sizeof(srv->session_set));
	srv->session_uid = (uid_t)-1;
}

/* Send ev to every tool and Dock connection. */
void
settings_tools_notify(struct ws_server *srv, ws_event_t *ev)
{
	struct ws_client *c;
	uint32_t serial = ev->hdr.serial;

	for (c = srv->clients; c != NULL; c = c->next)
		if ((c->flags & WS_CONNECT_TOOL) && c->sock >= 0) {
			ev->hdr.serial = serial;
			client_send(srv, c, ev, -1);
		}
}

static void
compose(const struct ws_config *base, const struct ws_config *session,
    const bool *set, struct ws_config *out)
{
	int i;

	*out = *base;
	for (i = 0; i < config_nkeys(); i++)
		if (set[i])
			config_copy(out, session, i);
}

/*
 * Make `next` the configuration in effect and do what each change
 * needs. The server's own keys keep the values they started with.
 */
static void
apply(struct ws_server *srv, const struct ws_config *next)
{
	struct ws_config old = srv->cfg;
	unsigned need = 0, f;
	ws_event_t ev;
	int i;

	srv->cfg = *next;
	for (i = 0; i < config_nkeys(); i++) {
		if (config_equal(&old, next, i))
			continue;
		f = config_key_flags(i);
		if (f == 0) {
			ws_info("%s changed; it takes effect when WindowServer "
			    "starts again", config_key_name(i));
			config_copy(&srv->cfg, &old, i);
			continue;
		}
		need |= f;
	}
	if (need == 0)
		return;
	if (need & CONFIG_KEYMAP) {
		if (keyboard_reload(srv) < 0) {
			/* Keep the keymap there is, and say so in GET. */
			ws_warn("keeping the keymap in use");
			for (i = 0; i < config_nkeys(); i++)
				if (config_key_flags(i) & CONFIG_KEYMAP)
					config_copy(&srv->cfg, &old, i);
			if (need & CONFIG_REPEAT)
				keyboard_repeat_changed(srv);
		}
	} else if (need & CONFIG_REPEAT)
		keyboard_repeat_changed(srv);
	if (need & CONFIG_DEVICE)
		input_reconfigure(srv);
	if (need & CONFIG_TILING)
		tiling_config_changed(srv, &old);
	if (need & CONFIG_DISPLAY)
		(void)display_apply(srv, false);
	event_init(srv, &ev, WS_EVENT_SETTINGS_CHANGED, 0, 0);
	settings_tools_notify(srv, &ev);
}

int
settings_reload_system(struct ws_server *srv)
{
	struct ws_config next;

	config_load_system(srv, &srv->base);
	compose(&srv->base, &srv->session, srv->session_set, &next);
	apply(srv, &next);
	return 0;
}

/*
 * Every few seconds: if the console user changed (a loginwindow session
 * ended or another began), the session's values go, so the login panel
 * gets the system's settings and the next user starts from them.
 */
void
settings_housekeeping(struct ws_server *srv)
{
	struct ws_config next;
	bool any = false;
	uid_t now;
	int i;

	for (i = 0; i < config_nkeys(); i++)
		any |= srv->session_set[i];
	if (!any)
		return;
	now = console_uid();
	if (now == srv->session_uid)
		return;
	ws_info("console user changed (%d -> %d): back to the system's "
	    "settings", (int)srv->session_uid, (int)now);
	memset(srv->session_set, 0, sizeof(srv->session_set));
	srv->session_uid = (uid_t)-1;
	compose(&srv->base, &srv->session, srv->session_set, &next);
	apply(srv, &next);
}

/* ---- WS_MSG_SET_SETTINGS ---- */

static bool
count_ok(const char *list, unsigned max)
{
	unsigned n = 1;

	for (; *list != '\0'; list++)
		if (*list == ',')
			n++;
	return n <= max;
}

static bool
in_range(double v, double lo, double hi, bool unset_ok)
{
	if (isnan(v))
		return unset_ok;
	return v >= lo && v <= hi;
}

/* Is the value of key i in cfg acceptable? */
static bool
value_ok(const struct ws_config *cfg, const char *key)
{
	if (strcmp(key, "KeyboardLayout") == 0)
		return cfg->keyboard_layout[0] != '\0' &&
		    count_ok(cfg->keyboard_layout, WS_MAX_KEYBOARD_LAYOUTS);
	if (strcmp(key, "KeyboardVariant") == 0)
		return count_ok(cfg->keyboard_variant, WS_MAX_KEYBOARD_LAYOUTS);
	if (strcmp(key, "KeyboardRules") == 0)
		return cfg->keyboard_rules[0] != '\0' &&
		    strchr(cfg->keyboard_rules, '/') == NULL;
	if (strcmp(key, "KeyboardLayoutShortcut") == 0)
		return keyboard_shortcut_valid(cfg->keyboard_layout_shortcut);
	if (strcmp(key, "KeyRepeatDelay") == 0)
		return cfg->key_repeat_delay >= 1 &&
		    cfg->key_repeat_delay <= 10000;
	if (strcmp(key, "KeyRepeatInterval") == 0)
		return cfg->key_repeat_interval >= 0 &&
		    cfg->key_repeat_interval <= 10000;
	if (strcmp(key, "SecondaryClick") == 0)
		return strcasecmp(cfg->secondary_click, "TwoFingers") == 0 ||
		    strcasecmp(cfg->secondary_click, "BottomRightCorner") == 0 ||
		    strcasecmp(cfg->secondary_click, "Off") == 0;
	if (strcmp(key, "MouseAccelProfile") == 0)
		return strcasecmp(cfg->mouse_accel_profile, "adaptive") == 0 ||
		    strcasecmp(cfg->mouse_accel_profile, "flat") == 0;
	if (strcmp(key, "PointerSpeed") == 0)
		return in_range(cfg->pointer_speed, -1, 1, false);
	if (strcmp(key, "TrackpadSpeed") == 0)
		return in_range(cfg->trackpad_speed, -1, 1, true);
	if (strcmp(key, "MouseSpeed") == 0)
		return in_range(cfg->mouse_speed, -1, 1, true);
	if (strcmp(key, "ScrollSpeed") == 0)
		return in_range(cfg->scroll_speed, 0.05, 20, false);
	if (strcmp(key, "WindowMode") == 0)
		return strcasecmp(cfg->window_mode, "stacking") == 0 ||
		    strcasecmp(cfg->window_mode, "tiling") == 0;
	if (strcmp(key, "TilingLayout") == 0)
		return strcasecmp(cfg->tiling_layout, "dwindle") == 0 ||
		    strcasecmp(cfg->tiling_layout, "master") == 0;
	if (strcmp(key, "TilingGapsInner") == 0)
		return cfg->tiling_gaps_inner >= 0 &&
		    cfg->tiling_gaps_inner <= 64;
	if (strcmp(key, "TilingGapsOuter") == 0)
		return cfg->tiling_gaps_outer >= 0 &&
		    cfg->tiling_gaps_outer <= 128;
	if (strcmp(key, "TilingMasterRatio") == 0)
		return in_range(cfg->tiling_master_ratio, 0.1, 0.9, false);
	if (strcmp(key, "TilingBorder") == 0)
		return cfg->tiling_border >= 0 && cfg->tiling_border <= 16;
	if (strcmp(key, "TilingModifier") == 0)
		return tiling_modifier_parse(cfg->tiling_modifier, NULL);
	if (strcmp(key, "DisplayScale") == 0)
		return display_scale_ok(cfg->display_scale);
	if (strcmp(key, "DisplayDefaultLooksLike") == 0)
		return display_default_ok(cfg->display_default_looks_like);
	if (strcmp(key, "TilingNewWindowSplits") == 0)
		return strcasecmp(cfg->tiling_new_window_splits, "largest") == 0 ||
		    strcasecmp(cfg->tiling_new_window_splits, "pointer") == 0 ||
		    strcasecmp(cfg->tiling_new_window_splits, "focused") == 0;
	return true;
}

/*
 * Does an entry of `type` fit key i? Its own type does, an integer does
 * for a real, and a string does for anything (then parsed), since
 * `defaults write` stores plain strings.
 */
static bool
type_ok(int i, int type)
{
	int want = config_key_type(i);

	return type == want || type == WS_SETTING_STRING ||
	    (want == WS_SETTING_REAL && type == WS_SETTING_INTEGER);
}

static void
reply_key(struct ws_mach_item *item, uint32_t seq, int status,
    const char *key)
{
	mach_msg_header_t *req = &item->msg.head;
	mach_msg_return_t kr;
	ws_reply_t r;

	memset(&r, 0, sizeof(r));
	r.seq = seq;
	r.status = status;
	if (key != NULL)
		strlcpy(r.u.settings.key, key, sizeof(r.u.settings.key));
	r.head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(req->msgh_bits),
	    0);
	r.head.msgh_size = sizeof(r);
	r.head.msgh_remote_port = req->msgh_remote_port;
	r.head.msgh_local_port = MACH_PORT_NULL;
	r.head.msgh_id = req->msgh_id + 100;
	r.version = WS_PROTOCOL_VERSION;
	if (req->msgh_remote_port == MACH_PORT_NULL)
		return;
	kr = mach_msg(&r.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
	    r.head.msgh_size, 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS)
		mach_port_deallocate(mach_task_self(), req->msgh_remote_port);
	req->msgh_remote_port = MACH_PORT_NULL;
}

/*
 * WS_MSG_SET_SETTINGS: check every entry, then change the session's
 * values all at once and apply them. Replies itself.
 */
void
settings_set(struct ws_server *srv, struct ws_client *c,
    struct ws_mach_item *item)
{
	ws_req_settings_t *m = (ws_req_settings_t *)(void *)item->msg.buf;
	struct ws_config base, session, next;
	bool set[CONFIG_MAX_KEYS], skip, any;
	char bad[64] = "", looks[32] = "";
	const char *p, *end, *key, *value;
	int i, type, n = 0;

	(void)c;
	if (!dock_may_list(srv, item->sender_euid)) {
		ws_warn("pid %d (uid %d): settings change refused",
		    (int)item->sender_pid, (int)item->sender_euid);
		reply_key(item, m->hdr.seq, EPERM, NULL);
		return;
	}
	if (item->size < offsetof(ws_req_settings_t, data) ||
	    m->length > WS_MAX_SETTINGS_DATA ||
	    item->size < offsetof(ws_req_settings_t, data) + m->length) {
		reply_key(item, m->hdr.seq, EINVAL, NULL);
		return;
	}
	skip = (m->flags & WS_SETTINGS_SKIP_INVALID) != 0;
	if (m->flags & WS_SETTINGS_RELOAD_SYSTEM)
		config_load_system(srv, &base);
	else
		base = srv->base;
	session = srv->session;
	memcpy(set, srv->session_set, sizeof(set));
	if (m->flags & WS_SETTINGS_REPLACE)
		memset(set, 0, sizeof(set));

	p = m->data;
	end = m->data + m->length;
	while (p < end) {
		struct ws_config before = session;
		int e = 0;

		type = (unsigned char)*p++;
		key = p;
		p = memchr(p, '\0', (size_t)(end - p));
		if (p == NULL) {
			reply_key(item, m->hdr.seq, EINVAL, NULL);
			return;
		}
		value = ++p;
		p = p < end ? memchr(p, '\0', (size_t)(end - p)) : NULL;
		if (p == NULL) {
			reply_key(item, m->hdr.seq, EINVAL, key);
			return;
		}
		p++;
		i = config_find(key);
		if (i < 0 && strcmp(key, "DisplayLooksLike") == 0) {
			/* Not a session value: kept per display for every
			   user (display.c), applied after the reply. */
			if (type == WS_SETTING_DEFAULT)
				strlcpy(looks, "Default", sizeof(looks));
			else if (display_looks_like_ok(srv, value))
				strlcpy(looks, value, sizeof(looks));
			else
				e = EINVAL;
			if (e == 0) {
				n++;
				continue;
			}
		} else if (i < 0)
			e = EINVAL;
		else if ((config_key_flags(i) & CONFIG_SETTABLE) == 0)
			e = EPERM;
		else if (type == WS_SETTING_DEFAULT)
			set[i] = false;
		else if (!type_ok(i, type) ||
		    config_set_text(&session, i, value) < 0 ||
		    !value_ok(&session, key))
			e = EINVAL;
		else
			set[i] = true;
		if (e == 0) {
			n++;
			continue;
		}
		session = before;
		if (bad[0] == '\0')
			strlcpy(bad, key, sizeof(bad));
		if (!skip) {
			ws_info("settings: %s refused (%s)", key, strerror(e));
			reply_key(item, m->hdr.seq, e, key);
			return;
		}
		ws_info("settings: leaving out %s (%s)", key, strerror(e));
	}

	compose(&base, &session, set, &next);
	for (i = 0, any = false; i < config_nkeys(); i++)
		if ((config_key_flags(i) & CONFIG_KEYMAP) &&
		    !config_equal(&srv->cfg, &next, i))
			any = true;
	if (any && keyboard_check(srv, &next) < 0) {
		if (!skip) {
			reply_key(item, m->hdr.seq, EINVAL, "KeyboardLayout");
			return;
		}
		/* Keep the session's keyboard values as they were. */
		for (i = 0; i < config_nkeys(); i++)
			if (config_key_flags(i) & CONFIG_KEYMAP) {
				set[i] = srv->session_set[i];
				config_copy(&session, &srv->session, i);
			}
		if (bad[0] == '\0')
			strlcpy(bad, "KeyboardLayout", sizeof(bad));
		compose(&base, &session, set, &next);
	}

	srv->base = base;
	srv->session = session;
	memcpy(srv->session_set, set, sizeof(set));
	srv->session_uid = console_uid();
	ws_info("settings: %d value%s from pid %d (uid %d)%s", n,
	    n == 1 ? "" : "s", (int)item->sender_pid, (int)item->sender_euid,
	    (m->flags & WS_SETTINGS_REPLACE) ? ", replacing the session's" : "");
	/* Reply first, so the sender isn't kept waiting for the keymap. */
	reply_key(item, m->hdr.seq, 0, bad[0] ? bad : NULL);
	apply(srv, &next);
	if (looks[0] != '\0')
		(void)display_set_looks_like(srv, looks);
}

/* ---- WS_MSG_GET_SETTINGS ---- */

void
settings_get(struct ws_server *srv, struct ws_client *c)
{
	struct ws_config builtin;
	ws_event_t ev;
	int i;

	config_defaults(&builtin);
	for (i = 0; i < config_nkeys(); i++) {
		event_init(srv, &ev, WS_EVENT_SETTING, sizeof(ev.u.setting), 0);
		strlcpy(ev.u.setting.key, config_key_name(i),
		    sizeof(ev.u.setting.key));
		ev.u.setting.type = (uint32_t)config_key_type(i);
		ev.u.setting.source = srv->session_set[i] ?
		    WS_SETTING_SOURCE_SESSION :
		    config_equal(&srv->base, &builtin, i) ?
		    WS_SETTING_SOURCE_BUILTIN : WS_SETTING_SOURCE_SYSTEM;
		ev.u.setting.flags =
		    (config_key_flags(i) & CONFIG_SETTABLE) ?
		    WS_SETTING_SETTABLE : 0;
		if (!config_get_text(&srv->cfg, i, ev.u.setting.value,
		    sizeof(ev.u.setting.value)))
			ev.u.setting.flags |= WS_SETTING_TRUNCATED;
		client_send(srv, c, &ev, -1);
	}
	/* What the display offers and runs at (display.c), read-only
	   but DisplayLooksLike. */
	for (i = 0;; i++) {
		event_init(srv, &ev, WS_EVENT_SETTING, sizeof(ev.u.setting), 0);
		if (display_info(srv, (unsigned)i, ev.u.setting.key,
		    sizeof(ev.u.setting.key), ev.u.setting.value,
		    sizeof(ev.u.setting.value)) < 0)
			break;
		ev.u.setting.type = WS_SETTING_STRING;
		ev.u.setting.source = WS_SETTING_SOURCE_SYSTEM;
		ev.u.setting.flags = strcmp(ev.u.setting.key,
		    "DisplayLooksLike") == 0 ? WS_SETTING_SETTABLE : 0;
		client_send(srv, c, &ev, -1);
	}
	event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
	client_send(srv, c, &ev, -1);
}
