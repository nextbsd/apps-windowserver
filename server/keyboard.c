/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Keyboard: xkbcommon keymap and state, server-side key repeat
 * (libinput doesn't repeat), and routing to the key window of the
 * active app.
 */

#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <dev/evdev/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

#include "server.h"

#define XKB_OFFSET	8	/* XKB keycode = evdev code + 8 */

/*
 * Put the compiled keymap in a sealed memfd for WS_EVENT_KEYMAP. Every
 * client gets the same file, so it is sealed against writes and size
 * changes.
 */
static int
keymap_export(struct ws_server *srv)
{
	char *str;
	size_t len, off;
	ssize_t n;
	int fd;

	str = xkb_keymap_get_as_string(srv->xkb_keymap,
	    XKB_KEYMAP_FORMAT_TEXT_V1);
	if (str == NULL)
		return -1;
	len = strlen(str) + 1;
	fd = memfd_create("WindowServer-keymap", MFD_CLOEXEC | MFD_ALLOW_SEALING);
	if (fd < 0) {
		free(str);
		return -1;
	}
	for (off = 0; off < len; off += (size_t)n) {
		n = write(fd, str + off, len - off);
		if (n < 0 && errno == EINTR) {
			n = 0;
			continue;
		}
		if (n <= 0) {
			free(str);
			close(fd);
			return -1;
		}
	}
	free(str);
	if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW |
	    F_SEAL_WRITE | F_SEAL_SEAL) < 0) {
		ws_warn("can't seal the keymap memfd: %s", strerror(errno));
		close(fd);
		return -1;
	}
	(void)lseek(fd, 0, SEEK_SET);
	if (srv->keymap_fd >= 0)
		close(srv->keymap_fd);
	srv->keymap_fd = fd;
	srv->keymap_size = (uint32_t)len;
	return 0;
}

int
keyboard_init(struct ws_server *srv)
{
	struct xkb_rule_names names;

	srv->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (srv->xkb_ctx == NULL) {
		ws_err("xkb_context_new failed");
		return -1;
	}
	memset(&names, 0, sizeof(names));
	names.rules = srv->cfg.keyboard_rules;
	names.model = srv->cfg.keyboard_model;
	names.layout = srv->cfg.keyboard_layout;
	names.variant = srv->cfg.keyboard_variant[0] ?
	    srv->cfg.keyboard_variant : NULL;
	names.options = srv->cfg.keyboard_options[0] ?
	    srv->cfg.keyboard_options : NULL;
	srv->xkb_keymap = xkb_keymap_new_from_names(srv->xkb_ctx, &names,
	    XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (srv->xkb_keymap == NULL) {
		ws_err("can't compile keymap (rules %s model %s layout %s)",
		    names.rules, names.model, names.layout);
		return -1;
	}
	srv->xkb_state = xkb_state_new(srv->xkb_keymap);
	if (srv->xkb_state == NULL)
		return -1;
	if (keymap_export(srv) < 0)
		ws_warn("can't export the keymap; WS_CONNECT_XKB clients "
		    "won't get WS_EVENT_KEYMAP");
	ws_info("keymap: rules %s, model %s, layout %s%s%s",
	    names.rules, names.model, names.layout,
	    names.variant ? ", variant " : "", names.variant ? names.variant : "");
	return 0;
}

void
keyboard_fini(struct ws_server *srv)
{
	if (srv->xkb_state != NULL)
		xkb_state_unref(srv->xkb_state);
	if (srv->xkb_keymap != NULL)
		xkb_keymap_unref(srv->xkb_keymap);
	if (srv->xkb_ctx != NULL)
		xkb_context_unref(srv->xkb_ctx);
	srv->xkb_state = NULL;
	srv->xkb_keymap = NULL;
	srv->xkb_ctx = NULL;
	if (srv->keymap_fd >= 0)
		close(srv->keymap_fd);
	srv->keymap_fd = -1;
}

static uint32_t
current_modifiers(struct ws_server *srv)
{
	static const struct {
		const char *name;
		uint32_t flag;
	} map[] = {
		{ XKB_MOD_NAME_SHIFT, WS_MOD_SHIFT },
		{ XKB_MOD_NAME_CTRL, WS_MOD_CONTROL },
		{ XKB_MOD_NAME_ALT, WS_MOD_ALT },
		{ XKB_MOD_NAME_LOGO, WS_MOD_COMMAND },
		{ XKB_MOD_NAME_CAPS, WS_MOD_CAPSLOCK },
		{ XKB_MOD_NAME_NUM, WS_MOD_NUMLOCK },
	};
	uint32_t mods = 0;
	size_t i;

	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (xkb_state_mod_name_is_active(srv->xkb_state, map[i].name,
		    XKB_STATE_MODS_EFFECTIVE) > 0)
			mods |= map[i].flag;
	return mods;
}

static void
repeat_stop(struct ws_server *srv)
{
	if (srv->repeat_key != 0) {
		ws_timer_cancel(srv, TIMER_REPEAT);
		srv->repeat_key = 0;
		srv->repeat_started = false;
	}
}

/*
 * Where key events go: the owner of the active app's key window (which
 * may be a connection that joined the app), else the app itself.
 */
static struct ws_client *
key_target(struct ws_server *srv, struct ws_window **wp)
{
	struct ws_client *a = srv->active;
	struct ws_window *w;

	*wp = NULL;
	if (a == NULL)
		return NULL;
	w = a->key_window;
	if (w != NULL && !w->visible)
		w = NULL;
	*wp = w;
	return w != NULL ? w->owner : a;
}

/* ---- XKB state for WS_CONNECT_XKB clients ---- */

static void
send_keymap(struct ws_server *srv, struct ws_client *c)
{
	ws_event_t ev;

	if (srv->keymap_fd < 0)
		return;
	event_init(srv, &ev, WS_EVENT_KEYMAP, sizeof(ev.u.keymap), 0);
	ev.u.keymap.format = WS_KEYMAP_XKB_V1;
	ev.u.keymap.size = srv->keymap_size;
	ev.u.keymap.repeat_delay = (uint32_t)srv->cfg.key_repeat_delay;
	ev.u.keymap.repeat_interval = srv->cfg.key_repeat_interval > 0 ?
	    (uint32_t)srv->cfg.key_repeat_interval : 0;
	ev.u.keymap.fd = -1;
	client_send(srv, c, &ev, srv->keymap_fd);
}

static void
send_modifiers(struct ws_server *srv, struct ws_client *c, uint32_t window)
{
	ws_event_t ev;

	event_init(srv, &ev, WS_EVENT_MODIFIERS, sizeof(ev.u.modifiers),
	    window);
	ev.u.modifiers.depressed = xkb_state_serialize_mods(srv->xkb_state,
	    XKB_STATE_MODS_DEPRESSED);
	ev.u.modifiers.latched = xkb_state_serialize_mods(srv->xkb_state,
	    XKB_STATE_MODS_LATCHED);
	ev.u.modifiers.locked = xkb_state_serialize_mods(srv->xkb_state,
	    XKB_STATE_MODS_LOCKED);
	ev.u.modifiers.group = xkb_state_serialize_layout(srv->xkb_state,
	    XKB_STATE_LAYOUT_EFFECTIVE);
	ev.u.modifiers.modifiers = srv->modifiers;
	client_send(srv, c, &ev, -1);
}

static void
send_keys_held(struct ws_server *srv, struct ws_client *c, uint32_t window)
{
	ws_event_t ev;
	unsigned i;

	event_init(srv, &ev, WS_EVENT_KEYS_HELD,
	    offsetof(ws_event_keys_held_t, keycodes) +
	    srv->nkeys_held * sizeof(uint32_t), window);
	ev.u.keys_held.count = srv->nkeys_held;
	for (i = 0; i < srv->nkeys_held; i++)
		ev.u.keys_held.keycodes[i] = srv->keys_held[i];
	client_send(srv, c, &ev, -1);
}

static void
keys_held_update(struct ws_server *srv, xkb_keycode_t kc, bool pressed)
{
	unsigned i;

	for (i = 0; i < srv->nkeys_held; i++)
		if (srv->keys_held[i] == kc)
			break;
	if (pressed) {
		if (i == srv->nkeys_held && i < WS_MAX_KEYS_HELD)
			srv->keys_held[srv->nkeys_held++] = kc;
	} else if (i < srv->nkeys_held) {
		srv->nkeys_held--;
		memmove(&srv->keys_held[i], &srv->keys_held[i + 1],
		    (srv->nkeys_held - i) * sizeof(srv->keys_held[0]));
	}
}

/* A WS_CONNECT_XKB client got its welcome. */
void
keyboard_client_ready(struct ws_server *srv, struct ws_client *c)
{
	struct ws_window *w;

	send_keymap(srv, c);
	if (srv->kbd_focus == c && srv->xkb_state != NULL) {
		(void)key_target(srv, &w);
		send_keys_held(srv, c, w != NULL ? w->id : 0);
		send_modifiers(srv, c, w != NULL ? w->id : 0);
	}
}

/*
 * The connection that gets key events may have changed (activation, key
 * window, a client gone). A new WS_CONNECT_XKB one hears which keys are
 * held and the modifier state, so nothing sticks.
 */
void
keyboard_focus_update(struct ws_server *srv)
{
	struct ws_client *to;
	struct ws_window *w;

	to = key_target(srv, &w);
	if (to != NULL && to->dead)
		to = NULL;
	if (to == srv->kbd_focus)
		return;
	srv->kbd_focus = to;
	if (to == NULL || (to->flags & WS_CONNECT_XKB) == 0 ||
	    srv->xkb_state == NULL)
		return;
	send_keys_held(srv, to, w != NULL ? w->id : 0);
	send_modifiers(srv, to, w != NULL ? w->id : 0);
}

static void
deliver_key(struct ws_server *srv, uint32_t code, bool pressed, bool repeat,
    xkb_keysym_t sym)
{
	struct ws_client *c;
	struct ws_window *w;
	const xkb_keysym_t *syms;
	xkb_keycode_t kc = code + XKB_OFFSET;
	xkb_layout_index_t layout;
	ws_event_t ev;
	int n;

	c = key_target(srv, &w);
	if (c == NULL)
		return;
	event_init(srv, &ev, pressed ? WS_EVENT_KEY_DOWN : WS_EVENT_KEY_UP,
	    sizeof(ev.u.key), w != NULL ? w->id : 0);
	ev.u.key.keycode = kc;
	ev.u.key.keysym = sym;
	ev.u.key.modifiers = srv->modifiers;
	ev.u.key.flags = repeat ? WS_KEY_REPEAT : 0;
	xkb_state_key_get_utf8(srv->xkb_state, kc, ev.u.key.text,
	    sizeof(ev.u.key.text));
	layout = xkb_state_key_get_layout(srv->xkb_state, kc);
	if (layout == XKB_LAYOUT_INVALID)
		layout = 0;
	n = xkb_keymap_key_get_syms_by_level(srv->xkb_keymap, kc, layout, 0,
	    &syms);
	if (n > 0)
		xkb_keysym_to_utf8(syms[0], ev.u.key.text_unmodified,
		    sizeof(ev.u.key.text_unmodified));
	client_send(srv, c, &ev, -1);
}

static void
send_flags(struct ws_server *srv)
{
	ws_event_t ev;
	struct ws_window *w;
	struct ws_client *c = key_target(srv, &w);

	if (c == NULL)
		return;
	event_init(srv, &ev, WS_EVENT_FLAGS_CHANGED, sizeof(ev.u.flags),
	    w != NULL ? w->id : 0);
	ev.u.flags.modifiers = srv->modifiers;
	client_send(srv, c, &ev, -1);
}

/* Keys the server handles itself. Returns true if consumed. */
static bool
server_key(struct ws_server *srv, uint32_t code, bool pressed, xkb_keysym_t sym)
{
	switch (code) {
	case KEY_POWER:
	case KEY_SLEEP:
	case KEY_SUSPEND:
	case KEY_WAKEUP:
		/* Later these go to loginwindow / IOPM; never into text. */
		if (pressed)
			ws_info("power key %u pressed (not delivered)", code);
		return true;
	default:
		break;
	}
	if (!pressed)
		return false;
	if (sym == XKB_KEY_BackSpace && srv->cfg.allow_terminate &&
	    (srv->modifiers & (WS_MOD_CONTROL | WS_MOD_ALT)) ==
	    (WS_MOD_CONTROL | WS_MOD_ALT)) {
		ws_info("Ctrl+Alt+Backspace: quitting");
		ws_quit(srv);
		return true;
	}
	return false;
}

void
keyboard_key(struct ws_server *srv, uint32_t code, bool pressed)
{
	xkb_keycode_t kc = code + XKB_OFFSET;
	enum xkb_state_component changed;
	struct ws_client *c;
	struct ws_window *w;
	xkb_keysym_t sym;
	uint32_t old_mods;
	bool consumed;

	if (srv->xkb_state == NULL)
		return;
	sym = xkb_state_key_get_one_sym(srv->xkb_state, kc);
	consumed = server_key(srv, code, pressed, sym);
	if (!consumed)
		deliver_key(srv, code, pressed, false, sym);

	old_mods = srv->modifiers;
	changed = xkb_state_update_key(srv->xkb_state, kc,
	    pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
	keys_held_update(srv, kc, pressed);
	srv->modifiers = current_modifiers(srv);
	if (srv->modifiers != old_mods)
		send_flags(srv);
	if ((changed & (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED |
	    XKB_STATE_MODS_LOCKED | XKB_STATE_LAYOUT_EFFECTIVE)) != 0 &&
	    (c = key_target(srv, &w)) != NULL && (c->flags & WS_CONNECT_XKB))
		send_modifiers(srv, c, w != NULL ? w->id : 0);

	if (pressed) {
		repeat_stop(srv);
		if (!consumed && srv->cfg.key_repeat_interval > 0 &&
		    xkb_keymap_key_repeats(srv->xkb_keymap, kc)) {
			srv->repeat_key = code;
			srv->repeat_started = false;
			ws_timer_set(srv, TIMER_REPEAT,
			    srv->cfg.key_repeat_delay, true);
		}
	} else if (code == srv->repeat_key) {
		repeat_stop(srv);
	}
}

void
keyboard_repeat_fire(struct ws_server *srv)
{
	xkb_keycode_t kc;

	if (srv->repeat_key == 0)
		return;
	kc = srv->repeat_key + XKB_OFFSET;
	deliver_key(srv, srv->repeat_key, true, true,
	    xkb_state_key_get_one_sym(srv->xkb_state, kc));
	if (!srv->repeat_started) {
		srv->repeat_started = true;
		ws_timer_set(srv, TIMER_REPEAT, srv->cfg.key_repeat_interval,
		    false);
	}
}

/* ---- synthetic keys (wsctl key NAME) ---- */

static int
find_keycode(struct ws_server *srv, xkb_keysym_t want, uint32_t *code,
    bool *shift)
{
	xkb_keycode_t kc, min, max;
	xkb_level_index_t lvl, nlvl;
	const xkb_keysym_t *syms;
	int n, i;

	min = xkb_keymap_min_keycode(srv->xkb_keymap);
	max = xkb_keymap_max_keycode(srv->xkb_keymap);
	for (lvl = 0; lvl < 2; lvl++) {
		for (kc = min; kc <= max; kc++) {
			if (kc < XKB_OFFSET)
				continue;
			nlvl = xkb_keymap_num_levels_for_key(srv->xkb_keymap,
			    kc, 0);
			if (lvl >= nlvl)
				continue;
			n = xkb_keymap_key_get_syms_by_level(srv->xkb_keymap,
			    kc, 0, lvl, &syms);
			for (i = 0; i < n; i++) {
				if (syms[i] == want) {
					*code = kc - XKB_OFFSET;
					*shift = lvl == 1;
					return 0;
				}
			}
		}
	}
	return -1;
}

/*
 * name: "a", "A", "Return", "shift+a", "ctrl+alt+F2" ...
 * mode: 1 press, 0 release, 2 press and release.
 */
int
keyboard_inject(struct ws_server *srv, const char *name, int mode)
{
	static const struct {
		const char *name;
		uint32_t code;
	} mods[] = {
		{ "shift", KEY_LEFTSHIFT },
		{ "ctrl", KEY_LEFTCTRL },
		{ "control", KEY_LEFTCTRL },
		{ "alt", KEY_LEFTALT },
		{ "option", KEY_LEFTALT },
		{ "cmd", KEY_LEFTMETA },
		{ "command", KEY_LEFTMETA },
		{ "super", KEY_LEFTMETA },
	};
	uint32_t held[8], code;
	char buf[64], *p, *tok, *last;
	xkb_keysym_t sym;
	size_t nheld = 0, i;
	bool shift;

	strlcpy(buf, name, sizeof(buf));
	/* everything before the last '+' names modifiers */
	last = strrchr(buf, '+');
	if (last != NULL && last[1] == '\0')
		last = NULL;		/* "+" itself */
	if (last != NULL) {
		*last = '\0';
		p = buf;
		while ((tok = strsep(&p, "+")) != NULL && nheld < 8) {
			for (i = 0; i < sizeof(mods) / sizeof(mods[0]); i++)
				if (strcasecmp(tok, mods[i].name) == 0)
					break;
			if (i == sizeof(mods) / sizeof(mods[0]))
				return EINVAL;
			held[nheld++] = mods[i].code;
		}
		name = last + 1;
	} else {
		name = buf;
	}
	sym = xkb_keysym_from_name(name, XKB_KEYSYM_NO_FLAGS);
	if (sym == XKB_KEY_NoSymbol)
		sym = xkb_keysym_from_name(name, XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym == XKB_KEY_NoSymbol || find_keycode(srv, sym, &code, &shift) < 0)
		return ENOENT;
	if (shift && nheld < 8)
		held[nheld++] = KEY_LEFTSHIFT;

	if (mode != 0)
		for (i = 0; i < nheld; i++)
			keyboard_key(srv, held[i], true);
	if (mode != 0)
		keyboard_key(srv, code, true);
	if (mode != 1)
		keyboard_key(srv, code, false);
	if (mode != 1)
		for (i = nheld; i > 0; i--)
			keyboard_key(srv, held[i - 1], false);
	return 0;
}
