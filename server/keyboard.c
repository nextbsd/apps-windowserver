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
#include <strings.h>
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

/* The grp: option KeyboardLayoutShortcut stands for, or NULL. */
static const char *
shortcut_option(const char *name)
{
	static const struct {
		const char *name;
		const char *option;
	} map[] = {
		{ "ControlSpace", "grp:ctrl_space_toggle" },
		{ "AltShift", "grp:alt_shift_toggle" },
		{ "WinSpace", "grp:win_space_toggle" },
		{ "CapsLock", "grp:caps_toggle" },
	};
	size_t i;

	for (i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (strcasecmp(name, map[i].name) == 0)
			return map[i].option;
	return NULL;
}

/* Is name a KeyboardLayoutShortcut value? */
bool
keyboard_shortcut_valid(const char *name)
{
	return name[0] == '\0' || strcasecmp(name, "None") == 0 ||
	    shortcut_option(name) != NULL;
}

/* Compile the keymap cfg describes. */
static struct xkb_keymap *
keymap_compile(struct ws_server *srv, const struct ws_config *cfg)
{
	struct xkb_rule_names names;
	struct xkb_keymap *km;
	char options[sizeof(cfg->keyboard_options) + 32];
	const char *grp = shortcut_option(cfg->keyboard_layout_shortcut);

	strlcpy(options, cfg->keyboard_options, sizeof(options));
	if (grp != NULL) {
		if (options[0] != '\0')
			strlcat(options, ",", sizeof(options));
		strlcat(options, grp, sizeof(options));
	}
	memset(&names, 0, sizeof(names));
	names.rules = cfg->keyboard_rules;
	names.model = cfg->keyboard_model;
	names.layout = cfg->keyboard_layout;
	names.variant = cfg->keyboard_variant[0] ? cfg->keyboard_variant : NULL;
	names.options = options[0] ? options : NULL;
	km = xkb_keymap_new_from_names(srv->xkb_ctx, &names,
	    XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (km == NULL)
		ws_warn("can't compile keymap (rules %s model %s layout %s "
		    "variant %s options %s)", names.rules, names.model,
		    names.layout, names.variant ? names.variant : "",
		    names.options ? names.options : "");
	else
		ws_info("keymap: rules %s, model %s, layout %s%s%s%s%s",
		    names.rules, names.model, names.layout,
		    names.variant ? ", variant " : "",
		    names.variant ? names.variant : "",
		    names.options ? ", options " : "",
		    names.options ? names.options : "");
	return km;
}

/* Split a comma-separated list into WS_MAX_KEYBOARD_LAYOUTS slots. */
static void
split_list(const char *list, char out[][32])
{
	const char *p = list, *e;
	size_t i, n;

	for (i = 0; i < WS_MAX_KEYBOARD_LAYOUTS; i++) {
		out[i][0] = '\0';
		if (p == NULL)
			continue;
		e = strchr(p, ',');
		n = e != NULL ? (size_t)(e - p) : strlen(p);
		if (n >= 32)
			n = 31;
		memcpy(out[i], p, n);
		out[i][n] = '\0';
		p = e != NULL ? e + 1 : NULL;
	}
}

/* Remember the layout list of the keymap just compiled. */
static void
layouts_note(struct ws_server *srv)
{
	xkb_layout_index_t n = xkb_keymap_num_layouts(srv->xkb_keymap);

	srv->nlayouts = n < WS_MAX_KEYBOARD_LAYOUTS ? n :
	    WS_MAX_KEYBOARD_LAYOUTS;
	split_list(srv->cfg.keyboard_layout, srv->layouts);
	split_list(srv->cfg.keyboard_variant, srv->variants);
	srv->layout_active = xkb_state_serialize_layout(srv->xkb_state,
	    XKB_STATE_LAYOUT_EFFECTIVE);
}

/*
 * Command shortcuts, as on macOS: while the active layout isn't Latin
 * (Greek, Russian...), a key pressed with Command held is resolved
 * against the first Latin layout in the list, so Command+C still
 * copies. A layout is Latin when most of its 26 letter keys type a to
 * z at level 1, which takes in QWERTZ, AZERTY and Dvorak.
 */
static const uint32_t letter_keys[] = {
	KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
	KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
	KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
};

static bool
layout_is_latin(struct xkb_keymap *km, xkb_layout_index_t layout)
{
	const xkb_keysym_t *syms;
	uint32_t u;
	size_t i;
	int latin = 0;

	for (i = 0; i < sizeof(letter_keys) / sizeof(letter_keys[0]); i++) {
		if (xkb_keymap_key_get_syms_by_level(km,
		    letter_keys[i] + XKB_OFFSET, layout, 0, &syms) < 1)
			continue;
		u = xkb_keysym_to_utf32(syms[0]);
		if (u >= 'a' && u <= 'z')
			latin++;
	}
	return latin >= 20;
}

static void
latin_fini(struct ws_server *srv)
{
	if (srv->latin_state != NULL)
		xkb_state_unref(srv->latin_state);
	if (srv->latin_keymap != NULL)
		xkb_keymap_unref(srv->latin_keymap);
	srv->latin_state = NULL;
	srv->latin_keymap = NULL;
	srv->layouts_latin = 0;
	srv->latin_layout = 0;
}

/* Find the layout Command shortcuts use for the keymap just compiled. */
static void
latin_setup(struct ws_server *srv)
{
	struct xkb_rule_names names;
	struct xkb_keymap *km = NULL;
	xkb_layout_index_t i, n = xkb_keymap_num_layouts(srv->xkb_keymap);
	uint32_t first = (uint32_t)-1;

	latin_fini(srv);
	for (i = 0; i < n && i < 32; i++)
		if (layout_is_latin(srv->xkb_keymap, i)) {
			srv->layouts_latin |= 1U << i;
			if (first == (uint32_t)-1)
				first = i;
		}
	if (srv->layouts_latin == (n >= 32 ? 0xffffffffU : (1U << n) - 1))
		return;		/* every layout is Latin: nothing to do */
	if (first != (uint32_t)-1) {
		km = xkb_keymap_ref(srv->xkb_keymap);
		srv->latin_layout = first;
	} else {
		memset(&names, 0, sizeof(names));
		names.rules = srv->cfg.keyboard_rules;
		names.model = srv->cfg.keyboard_model;
		names.layout = "us";
		km = xkb_keymap_new_from_names(srv->xkb_ctx, &names,
		    XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (km == NULL) {
			ws_warn("can't compile a us keymap for Command "
			    "shortcuts");
			return;
		}
		srv->latin_layout = 0;
	}
	srv->latin_state = xkb_state_new(km);
	if (srv->latin_state == NULL) {
		xkb_keymap_unref(km);
		return;
	}
	srv->latin_keymap = km;
	ws_info("Command shortcuts: layout %s",
	    km == srv->xkb_keymap && first < WS_MAX_KEYBOARD_LAYOUTS ?
	    srv->layouts[first] : "us");
}

/* A modifier mask of xkb_keymap in latin_keymap's mod indices. */
static xkb_mod_mask_t
latin_mods(struct ws_server *srv, xkb_mod_mask_t mask)
{
	xkb_mod_index_t i, j, n;
	xkb_mod_mask_t out = 0;

	if (srv->latin_keymap == srv->xkb_keymap)
		return mask;
	n = xkb_keymap_num_mods(srv->xkb_keymap);
	for (i = 0; i < n && i < 32; i++) {
		if ((mask & (1U << i)) == 0)
			continue;
		j = xkb_keymap_mod_get_index(srv->latin_keymap,
		    xkb_keymap_mod_get_name(srv->xkb_keymap, i));
		if (j != XKB_MOD_INVALID && j < 32)
			out |= 1U << j;
	}
	return out;
}

int
keyboard_init(struct ws_server *srv)
{
	srv->xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (srv->xkb_ctx == NULL) {
		ws_err("xkb_context_new failed");
		return -1;
	}
	srv->xkb_keymap = keymap_compile(srv, &srv->cfg);
	if (srv->xkb_keymap == NULL)
		return -1;
	srv->xkb_state = xkb_state_new(srv->xkb_keymap);
	if (srv->xkb_state == NULL)
		return -1;
	if (keymap_export(srv) < 0)
		ws_warn("can't export the keymap; WS_CONNECT_XKB clients "
		    "won't get WS_EVENT_KEYMAP");
	layouts_note(srv);
	latin_setup(srv);
	return 0;
}

/* Would cfg's keyboard settings compile? (settings.c, before a change) */
int
keyboard_check(struct ws_server *srv, const struct ws_config *cfg)
{
	struct xkb_keymap *km;

	if (srv->xkb_ctx == NULL)
		return -1;
	km = keymap_compile(srv, cfg);
	if (km == NULL)
		return -1;
	xkb_keymap_unref(km);
	return 0;
}

void
keyboard_fini(struct ws_server *srv)
{
	latin_fini(srv);
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
	/* While the user works in the Dock, a Dock popup that took the key
	   window (a text field) gets the keys; the rest go on to the
	   active app. */
	if (srv->dock_front != NULL && !srv->dock_front->dead &&
	    (w = srv->dock_front->key_window) != NULL && w->visible) {
		*wp = w;
		return w->owner;
	}
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

static void	send_flags(struct ws_server *srv);

/* ---- layouts ---- */

static void
layout_event(struct ws_server *srv, ws_event_t *ev, uint32_t index)
{
	const char *name;

	event_init(srv, ev, WS_EVENT_KEYBOARD_LAYOUT,
	    sizeof(ev->u.keyboard_layout), 0);
	ev->u.keyboard_layout.index = index;
	ev->u.keyboard_layout.count = srv->nlayouts;
	ev->u.keyboard_layout.active = srv->layout_active;
	ev->u.keyboard_layout.flags = index == srv->layout_active ?
	    WS_KEYBOARD_LAYOUT_ACTIVE : 0;
	if (index < WS_MAX_KEYBOARD_LAYOUTS) {
		strlcpy(ev->u.keyboard_layout.layout, srv->layouts[index],
		    sizeof(ev->u.keyboard_layout.layout));
		strlcpy(ev->u.keyboard_layout.variant, srv->variants[index],
		    sizeof(ev->u.keyboard_layout.variant));
	}
	name = xkb_keymap_layout_get_name(srv->xkb_keymap, index);
	strlcpy(ev->u.keyboard_layout.name, name != NULL ? name : "",
	    sizeof(ev->u.keyboard_layout.name));
}

/* Tools and the Dock (its Input menu) hear the active layout. */
static void
layout_announce(struct ws_server *srv)
{
	ws_event_t ev;

	layout_event(srv, &ev, srv->layout_active);
	ev.u.keyboard_layout.flags |= WS_KEYBOARD_LAYOUT_CHANGED;
	settings_tools_notify(srv, &ev);
}

/* After the XKB state changed: did the active layout? */
static void
layout_check(struct ws_server *srv)
{
	uint32_t g = xkb_state_serialize_layout(srv->xkb_state,
	    XKB_STATE_LAYOUT_EFFECTIVE);

	if (g == srv->layout_active)
		return;
	srv->layout_active = g;
	ws_info("keyboard layout %u (%s) is active", g,
	    g < WS_MAX_KEYBOARD_LAYOUTS ? srv->layouts[g] : "?");
	layout_announce(srv);
}

/* WS_MSG_LIST_KEYBOARD_LAYOUTS */
void
keyboard_list_layouts(struct ws_server *srv, struct ws_client *c)
{
	ws_event_t ev;
	uint32_t i;

	for (i = 0; i < srv->nlayouts; i++) {
		layout_event(srv, &ev, i);
		client_send(srv, c, &ev, -1);
	}
	event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
	client_send(srv, c, &ev, -1);
}

/* The modifier state changed outside a key event: tell whoever gets
 * key events, as a key event would. */
static void
state_changed(struct ws_server *srv)
{
	struct ws_client *c;
	struct ws_window *w;
	uint32_t old_mods = srv->modifiers;

	srv->modifiers = current_modifiers(srv);
	if (srv->modifiers != old_mods)
		send_flags(srv);
	if ((c = key_target(srv, &w)) != NULL && (c->flags & WS_CONNECT_XKB))
		send_modifiers(srv, c, w != NULL ? w->id : 0);
	layout_check(srv);
}

/* WS_MSG_SET_KEYBOARD_LAYOUT: make layout `index` the active one. */
int
keyboard_set_layout(struct ws_server *srv, uint32_t index)
{
	if (srv->xkb_state == NULL || index >= srv->nlayouts)
		return EINVAL;
	xkb_state_update_mask(srv->xkb_state,
	    xkb_state_serialize_mods(srv->xkb_state, XKB_STATE_MODS_DEPRESSED),
	    xkb_state_serialize_mods(srv->xkb_state, XKB_STATE_MODS_LATCHED),
	    xkb_state_serialize_mods(srv->xkb_state, XKB_STATE_MODS_LOCKED),
	    xkb_state_serialize_layout(srv->xkb_state,
	    XKB_STATE_LAYOUT_DEPRESSED),
	    xkb_state_serialize_layout(srv->xkb_state, XKB_STATE_LAYOUT_LATCHED),
	    index);
	state_changed(srv);
	return 0;
}

/* KeyRepeatDelay or KeyRepeatInterval changed. */
void
keyboard_repeat_changed(struct ws_server *srv)
{
	struct ws_client *c;

	repeat_stop(srv);
	for (c = srv->clients; c != NULL; c = c->next)
		if ((c->flags & WS_CONNECT_XKB) && c->sock >= 0)
			send_keymap(srv, c);
}

/* A locked modifier's mask in keymap km, if km has it. */
static xkb_mod_mask_t
locked_mod(struct ws_server *srv, struct xkb_keymap *km, const char *name)
{
	xkb_mod_index_t i;

	if (xkb_state_mod_name_is_active(srv->xkb_state, name,
	    XKB_STATE_MODS_LOCKED) <= 0)
		return 0;
	i = xkb_keymap_mod_get_index(km, name);
	return i == XKB_MOD_INVALID ? 0 : (xkb_mod_mask_t)1 << i;
}

/*
 * The keyboard settings in srv->cfg changed: compile the keymap again
 * and switch to it. Keys held stay held, Caps Lock and Num Lock stay
 * locked, and the active layout stays active if it's still in the list.
 * Every key event from now on carries keysyms from the new keymap;
 * WS_CONNECT_XKB connections get it (WS_EVENT_KEYMAP), then the state
 * (WS_EVENT_MODIFIERS).
 */
int
keyboard_reload(struct ws_server *srv)
{
	char active[32], names[WS_MAX_KEYBOARD_LAYOUTS][32];
	struct xkb_keymap *km;
	struct xkb_state *st;
	xkb_mod_mask_t locked;
	struct ws_client *c;
	struct ws_window *w;
	uint32_t group = 0, i;

	if (srv->xkb_ctx == NULL)
		return -1;
	km = keymap_compile(srv, &srv->cfg);
	if (km == NULL)
		return -1;
	st = xkb_state_new(km);
	if (st == NULL) {
		xkb_keymap_unref(km);
		return -1;
	}
	strlcpy(active, srv->layout_active < WS_MAX_KEYBOARD_LAYOUTS ?
	    srv->layouts[srv->layout_active] : "", sizeof(active));
	split_list(srv->cfg.keyboard_layout, names);
	for (i = 0; i < WS_MAX_KEYBOARD_LAYOUTS &&
	    i < xkb_keymap_num_layouts(km); i++)
		if (active[0] != '\0' && strcmp(names[i], active) == 0) {
			group = i;
			break;
		}
	locked = locked_mod(srv, km, XKB_MOD_NAME_CAPS) |
	    locked_mod(srv, km, XKB_MOD_NAME_NUM);
	for (i = 0; i < srv->nkeys_held; i++)
		xkb_state_update_key(st, srv->keys_held[i], XKB_KEY_DOWN);
	xkb_state_update_mask(st,
	    xkb_state_serialize_mods(st, XKB_STATE_MODS_DEPRESSED),
	    xkb_state_serialize_mods(st, XKB_STATE_MODS_LATCHED), locked,
	    xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_DEPRESSED),
	    xkb_state_serialize_layout(st, XKB_STATE_LAYOUT_LATCHED), group);

	repeat_stop(srv);
	xkb_state_unref(srv->xkb_state);
	xkb_keymap_unref(srv->xkb_keymap);
	srv->xkb_state = st;
	srv->xkb_keymap = km;
	if (keymap_export(srv) < 0)
		ws_warn("can't export the new keymap");
	srv->layout_active = (uint32_t)-1;	/* announce it below */
	layouts_note(srv);
	srv->layout_active = (uint32_t)-1;
	latin_setup(srv);
	for (c = srv->clients; c != NULL; c = c->next) {
		if ((c->flags & WS_CONNECT_XKB) == 0 || c->sock < 0)
			continue;
		send_keymap(srv, c);
		if (c != key_target(srv, &w))
			send_modifiers(srv, c, 0);
	}
	state_changed(srv);
	return 0;
}

static bool
is_modifier_keysym(xkb_keysym_t sym)
{
	return (sym >= XKB_KEY_Shift_L && sym <= XKB_KEY_Hyper_R) ||
	    (sym >= XKB_KEY_ISO_Lock && sym <= XKB_KEY_ISO_Level5_Lock) ||
	    sym == XKB_KEY_Mode_switch || sym == XKB_KEY_Num_Lock;
}

static int
equiv_key_index(struct ws_server *srv, xkb_keycode_t kc)
{
	unsigned i;

	for (i = 0; i < srv->nequiv_keys; i++)
		if (srv->equiv_keys[i] == kc)
			return (int)i;
	return -1;
}

/*
 * Command shortcuts belong to the app's menus. When the key window is a
 * joined connection's (an xterm's frame, owned by the X server XLibre.app
 * started), a key pressed with Command held goes to the app's own
 * connection, without a window, so its main menu sees it; the key's
 * repeats and release follow it there. Command with a modifier key
 * alone (Command-Shift) still goes to the key window's owner.
 */
static struct ws_client *
equiv_target(struct ws_server *srv, struct ws_client *c, xkb_keycode_t kc,
    bool pressed, bool repeat, xkb_keysym_t sym)
{
	struct ws_client *a = srv->active;
	int i = equiv_key_index(srv, kc);

	if (i >= 0) {
		if (!pressed) {
			srv->nequiv_keys--;
			memmove(&srv->equiv_keys[i], &srv->equiv_keys[i + 1],
			    (srv->nequiv_keys - (unsigned)i) *
			    sizeof(srv->equiv_keys[0]));
		}
		return a != NULL && !a->dead ? a : NULL;
	}
	if (!pressed || repeat || a == NULL || c == a || c->app != a ||
	    (srv->modifiers & WS_MOD_COMMAND) == 0 || is_modifier_keysym(sym) ||
	    srv->nequiv_keys >= WS_MAX_KEYS_HELD)
		return NULL;
	srv->equiv_keys[srv->nequiv_keys++] = kc;
	return a;
}

/*
 * Fill k's keysym and text from the Latin layout, with the modifiers
 * in effect, when Command is held and the active layout isn't Latin.
 * Returns false (k untouched) otherwise. X servers (WS_CONNECT_XKB)
 * read keycodes with their own keymap, so they keep the plain event.
 */
static bool
latin_key(struct ws_server *srv, struct ws_client *c, xkb_keycode_t kc,
    xkb_keysym_t sym, ws_event_key_t *k)
{
	const xkb_keysym_t *syms;
	xkb_layout_index_t layout;
	xkb_keysym_t lsym;

	if ((srv->modifiers & WS_MOD_COMMAND) == 0 ||
	    srv->latin_state == NULL || (c->flags & WS_CONNECT_XKB) != 0 ||
	    is_modifier_keysym(sym))
		return false;
	layout = xkb_state_key_get_layout(srv->xkb_state, kc);
	if (layout == XKB_LAYOUT_INVALID || (layout < 32 &&
	    (srv->layouts_latin & (1U << layout)) != 0))
		return false;
	xkb_state_update_mask(srv->latin_state,
	    latin_mods(srv, xkb_state_serialize_mods(srv->xkb_state,
	    XKB_STATE_MODS_DEPRESSED)),
	    latin_mods(srv, xkb_state_serialize_mods(srv->xkb_state,
	    XKB_STATE_MODS_LATCHED)),
	    latin_mods(srv, xkb_state_serialize_mods(srv->xkb_state,
	    XKB_STATE_MODS_LOCKED)), 0, 0, srv->latin_layout);
	lsym = xkb_state_key_get_one_sym(srv->latin_state, kc);
	if (lsym == XKB_KEY_NoSymbol)
		return false;
	k->keysym = lsym;
	xkb_state_key_get_utf8(srv->latin_state, kc, k->text, sizeof(k->text));
	k->text_unmodified[0] = '\0';
	if (xkb_keymap_key_get_syms_by_level(srv->latin_keymap, kc,
	    srv->latin_layout, 0, &syms) > 0)
		xkb_keysym_to_utf8(syms[0], k->text_unmodified,
		    sizeof(k->text_unmodified));
	return true;
}

static void
deliver_key(struct ws_server *srv, uint32_t code, bool pressed, bool repeat,
    xkb_keysym_t sym)
{
	struct ws_client *c, *a;
	struct ws_window *w;
	const xkb_keysym_t *syms;
	xkb_keycode_t kc = code + XKB_OFFSET;
	xkb_layout_index_t layout;
	ws_event_t ev;
	int n;

	c = key_target(srv, &w);
	if (c == NULL)
		return;
	if ((a = equiv_target(srv, c, kc, pressed, repeat, sym)) != NULL) {
		c = a;
		w = NULL;
	}
	event_init(srv, &ev, pressed ? WS_EVENT_KEY_DOWN : WS_EVENT_KEY_UP,
	    sizeof(ev.u.key), w != NULL ? w->id : 0);
	ev.u.key.keycode = kc;
	ev.u.key.keysym = sym;
	ev.u.key.modifiers = srv->modifiers;
	ev.u.key.flags = repeat ? WS_KEY_REPEAT : 0;
	if (latin_key(srv, c, kc, sym, &ev.u.key)) {
		client_send(srv, c, &ev, -1);
		return;
	}
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
	/* Escape cancels a drag (drag.c). */
	if (drag_key(srv, sym))
		return true;
	/* The window overview: the Mission Control key, Control+Up (every
	 * window) and Control+Down (App Exposé), as on macOS. */
	if (code == KEY_SCALE || sym == XKB_KEY_XF86LaunchA) {
		overview_toggle(srv, WS_OVERVIEW_ALL);
		return true;
	}
	if ((sym == XKB_KEY_Up || sym == XKB_KEY_Down) &&
	    (srv->modifiers & (WS_MOD_SHIFT | WS_MOD_CONTROL | WS_MOD_ALT |
	    WS_MOD_COMMAND)) == WS_MOD_CONTROL) {
		overview_toggle(srv, sym == XKB_KEY_Up ? WS_OVERVIEW_ALL :
		    WS_OVERVIEW_APP);
		return true;
	}
	if (overview_active(srv))
		return overview_key(srv, code, sym);
	if (tiling_key(srv, sym))
		return true;
	if (sym == XKB_KEY_BackSpace && srv->cfg.allow_terminate &&
	    (srv->modifiers & (WS_MOD_CONTROL | WS_MOD_ALT)) ==
	    (WS_MOD_CONTROL | WS_MOD_ALT)) {
		ws_info("Ctrl+Alt+Backspace: quitting");
		ws_quit(srv);
		return true;
	}
	return false;
}

/* Remember (pressed) or forget a key whose press the server took;
 * returns whether it was one. */
static bool
server_key_held(struct ws_server *srv, uint32_t code, bool pressed)
{
	unsigned i;

	for (i = 0; i < srv->nserver_keys; i++)
		if (srv->server_keys[i] == code)
			break;
	if (pressed) {
		if (i == srv->nserver_keys && i < WS_MAX_KEYS_HELD)
			srv->server_keys[srv->nserver_keys++] = code;
		return true;
	}
	if (i == srv->nserver_keys)
		return false;
	srv->nserver_keys--;
	memmove(&srv->server_keys[i], &srv->server_keys[i + 1],
	    (srv->nserver_keys - i) * sizeof(srv->server_keys[0]));
	return true;
}

/*
 * The Start key (Super, which is also Command) tapped on its own opens
 * or closes the overview: pressed with no other key or button down,
 * and released before any other key, click or gesture. Command
 * shortcuts are untouched, and the key itself still reaches the app.
 */
static bool
start_tap(struct ws_server *srv, uint32_t code, bool pressed)
{
	bool start = code == KEY_LEFTMETA || code == KEY_RIGHTMETA;

	if (pressed) {
		srv->start_tap = start && srv->nkeys_held == 0 &&
		    srv->buttons == 0 ? code : 0;
		return false;
	}
	if (code != srv->start_tap || code == 0)
		return false;
	srv->start_tap = 0;
	return srv->cfg.start_key_overview;
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
	bool consumed, tapped;

	if (srv->xkb_state == NULL)
		return;
	sym = xkb_state_key_get_one_sym(srv->xkb_state, kc);
	tapped = start_tap(srv, code, pressed);
	/* A key the server took on its way down is its own on the way up. */
	if (!pressed && server_key_held(srv, code, false))
		consumed = true;
	else {
		consumed = server_key(srv, code, pressed, sym);
		if (consumed && pressed)
			(void)server_key_held(srv, code, true);
	}
	if (!consumed)
		deliver_key(srv, code, pressed, false, sym);

	old_mods = srv->modifiers;
	changed = xkb_state_update_key(srv->xkb_state, kc,
	    pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
	keys_held_update(srv, kc, pressed);
	srv->modifiers = current_modifiers(srv);
	if (srv->modifiers != old_mods) {
		send_flags(srv);
		drag_modifiers_changed(srv);
	}
	if ((changed & (XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED |
	    XKB_STATE_MODS_LOCKED | XKB_STATE_LAYOUT_EFFECTIVE)) != 0 &&
	    (c = key_target(srv, &w)) != NULL && (c->flags & WS_CONNECT_XKB))
		send_modifiers(srv, c, w != NULL ? w->id : 0);
	if (changed & XKB_STATE_LAYOUT_EFFECTIVE)
		layout_check(srv);

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
	if (tapped)
		overview_toggle(srv, WS_OVERVIEW_ALL);
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
