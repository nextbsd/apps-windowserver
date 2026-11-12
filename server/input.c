/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Input from libinput's udev backend on seat0. On NextBSD, libudev is
 * backed by IOKit, so devices come from the IORegistry.
 *
 * Double keys: with both kbdmux0 and the per-device keyboards enabled
 * (kern.evdev.rcpt_mask), every key arrives twice. libinput's seat key
 * count catches that (the second press of a key that is already down
 * on the seat is ignored), and IgnoreMuxDevices drops kbdmux/sysmouse
 * entirely if per-device keyboards are configured.
 */

#include <sys/types.h>
#include <sys/event.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <dev/evdev/input-event-codes.h>
#include <libinput.h>
#include <libudev.h>

#include "server.h"

#define DEV_IGNORED	((void *)(uintptr_t)1)
#define DEV_TOUCHPAD	((void *)(uintptr_t)2)	/* taps: a touchpad */

static int
open_restricted(const char *path, int flags, void *data)
{
	int fd;

	(void)data;
	fd = open(path, flags | O_CLOEXEC);
	if (fd < 0) {
		ws_warn("can't open %s: %s", path, strerror(errno));
		return -errno;
	}
	return fd;
}

static void
close_restricted(int fd, void *data)
{
	(void)data;
	close(fd);
}

static const struct libinput_interface iface = {
	.open_restricted = open_restricted,
	.close_restricted = close_restricted,
};

static void
log_handler(struct libinput *li, enum libinput_log_priority prio,
    const char *fmt, va_list ap)
{
	char buf[512];
	size_t n;

	(void)li;
	vsnprintf(buf, sizeof(buf), fmt, ap);
	n = strlen(buf);
	if (n > 0 && buf[n - 1] == '\n')
		buf[n - 1] = '\0';
	ws_log(prio >= LIBINPUT_LOG_PRIORITY_ERROR ? WS_LOG_WARN :
	    prio >= LIBINPUT_LOG_PRIORITY_INFO ? WS_LOG_INFO : WS_LOG_DEBUG,
	    "libinput: %s", buf);
}

int
input_open(struct ws_server *srv)
{
	srv->udev = udev_new();
	if (srv->udev == NULL) {
		ws_err("udev_new failed");
		return -1;
	}
	srv->libinput = libinput_udev_create_context(&iface, srv, srv->udev);
	if (srv->libinput == NULL) {
		ws_err("libinput_udev_create_context failed");
		return -1;
	}
	libinput_log_set_handler(srv->libinput, log_handler);
	libinput_log_set_priority(srv->libinput,
	    ws_log_level >= WS_LOG_DEBUG ? LIBINPUT_LOG_PRIORITY_DEBUG :
	    LIBINPUT_LOG_PRIORITY_INFO);
	if (libinput_udev_assign_seat(srv->libinput, "seat0") != 0) {
		ws_err("libinput: can't assign seat0");
		return -1;
	}
	ws_kq_add_fd(srv, libinput_get_fd(srv->libinput), EVFILT_READ,
	    (void *)(uintptr_t)SRC_INPUT);
	/* Handle the initial DEVICE_ADDED events now. */
	input_readable(srv);
	return 0;
}

static bool
is_mux_device(const char *name)
{
	return strstr(name, "kbdmux") != NULL ||
	    strstr(name, "sysmouse") != NULL ||
	    strstr(name, "System keyboard multiplexer") != NULL ||
	    strstr(name, "System mouse") != NULL;
}

static double
clamp_speed(double speed)
{
	return speed < -1.0 ? -1.0 : speed > 1.0 ? 1.0 : speed;
}

/*
 * Touchpad and pointer settings (README, "Touchpad" and "Mouse"):
 * speed (TrackpadSpeed or MouseSpeed, else PointerSpeed), the mouse's
 * acceleration profile, left-handed buttons, tapping, tap and drag,
 * secondary click and three-finger drag. Run when a device arrives and
 * again for every device when a setting changes (settings.c).
 */
static void
configure_device(struct ws_server *srv, struct libinput_device *dev)
{
	const char *name = libinput_device_get_name(dev);
	const char *sc = srv->cfg.secondary_click;
	bool touchpad = libinput_device_config_tap_get_finger_count(dev) > 0;
	uint32_t methods;
	double speed;

	if (libinput_device_config_accel_is_available(dev)) {
		speed = touchpad ? srv->cfg.trackpad_speed :
		    srv->cfg.mouse_speed;
		if (isnan(speed))
			speed = srv->cfg.pointer_speed;
		libinput_device_config_accel_set_speed(dev, clamp_speed(speed));
		/* Mouse acceleration on (adaptive) or off (flat); touchpads
		   keep libinput's own profile. */
		if (!touchpad) {
			enum libinput_config_accel_profile prof =
			    strcasecmp(srv->cfg.mouse_accel_profile, "flat") == 0 ?
			    LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT :
			    LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE;

			if (libinput_device_config_accel_get_profiles(dev) & prof)
				libinput_device_config_accel_set_profile(dev,
				    prof);
		}
	}
	if (libinput_device_config_left_handed_is_available(dev))
		libinput_device_config_left_handed_set(dev,
		    srv->cfg.left_handed ? 1 : 0);
	if (!touchpad)
		return;
	libinput_device_set_user_data(dev, DEV_TOUCHPAD);
	libinput_device_config_tap_set_enabled(dev, srv->cfg.tap_to_click ?
	    LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	/* One finger left, two right, three middle. */
	libinput_device_config_tap_set_button_map(dev,
	    LIBINPUT_CONFIG_TAP_MAP_LRM);
	libinput_device_config_tap_set_drag_enabled(dev, srv->cfg.tap_to_drag ?
	    LIBINPUT_CONFIG_DRAG_ENABLED : LIBINPUT_CONFIG_DRAG_DISABLED);
	libinput_device_config_tap_set_drag_lock_enabled(dev,
	    LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);

	/* A click: with two fingers, in the bottom-right corner, or
	 * always a left click (Off, and see map_touchpad_button()). */
	methods = libinput_device_config_click_get_methods(dev);
	if (strcasecmp(sc, "TwoFingers") == 0) {
		if (methods & LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER) {
			libinput_device_config_click_set_method(dev,
			    LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER);
			libinput_device_config_click_set_clickfinger_button_map(
			    dev, LIBINPUT_CONFIG_CLICKFINGER_MAP_LRM);
		}
	} else if (strcasecmp(sc, "BottomRightCorner") == 0) {
		if (methods & LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS)
			libinput_device_config_click_set_method(dev,
			    LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS);
	} else if (strcasecmp(sc, "Off") == 0) {
		if (methods != 0)
			libinput_device_config_click_set_method(dev,
			    LIBINPUT_CONFIG_CLICK_METHOD_NONE);
	} else
		ws_warn("SecondaryClick: unknown value %s (TwoFingers, "
		    "BottomRightCorner or Off)", sc);

	/* Three-finger drag (macOS's accessibility option). libinput then
	 * reports swipes with four fingers, not three. */
	if (libinput_device_config_3fg_drag_get_finger_count(dev) >= 3)
		libinput_device_config_3fg_drag_set_enabled(dev,
		    srv->cfg.three_finger_drag ?
		    LIBINPUT_CONFIG_3FG_DRAG_ENABLED_3FG :
		    LIBINPUT_CONFIG_3FG_DRAG_DISABLED);
	ws_info("input: %s: tap %s, tap-and-drag %s, secondary click %s, "
	    "three-finger drag %s", name, srv->cfg.tap_to_click ? "on" : "off",
	    srv->cfg.tap_to_drag ? "on" : "off", sc,
	    srv->cfg.three_finger_drag ? "on" : "off");
}

/* A pointer or touchpad setting changed: apply it to every device. */
void
input_reconfigure(struct ws_server *srv)
{
	struct ws_input_device *d;

	for (d = srv->input_devices; d != NULL; d = d->next)
		configure_device(srv, d->dev);
}

/* WS_MSG_LIST_INPUT_DEVICES */
void
input_list(struct ws_server *srv, struct ws_client *c)
{
	struct ws_input_device *d;
	ws_event_t ev;

	for (d = srv->input_devices; d != NULL; d = d->next) {
		event_init(srv, &ev, WS_EVENT_INPUT_DEVICE,
		    sizeof(ev.u.input_device), 0);
		ev.u.input_device.flags = d->flags;
		strlcpy(ev.u.input_device.name, libinput_device_get_name(d->dev),
		    sizeof(ev.u.input_device.name));
		strlcpy(ev.u.input_device.sysname,
		    libinput_device_get_sysname(d->dev),
		    sizeof(ev.u.input_device.sysname));
		client_send(srv, c, &ev, -1);
	}
	event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
	client_send(srv, c, &ev, -1);
}

static void
device_track(struct ws_server *srv, struct libinput_device *dev)
{
	struct ws_input_device *d = calloc(1, sizeof(*d));

	if (d == NULL)
		return;
	d->dev = libinput_device_ref(dev);
	if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_KEYBOARD))
		d->flags |= WS_INPUT_KEYBOARD;
	if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_POINTER)) {
		d->flags |= WS_INPUT_POINTER;
		d->flags |= libinput_device_config_tap_get_finger_count(dev) > 0 ?
		    WS_INPUT_TOUCHPAD : WS_INPUT_MOUSE;
	}
	if (libinput_device_has_capability(dev, LIBINPUT_DEVICE_CAP_TOUCH))
		d->flags |= WS_INPUT_TOUCH;
	d->next = srv->input_devices;
	srv->input_devices = d;
}

static void
device_untrack(struct ws_server *srv, struct libinput_device *dev)
{
	struct ws_input_device **dp, *d;

	for (dp = &srv->input_devices; (d = *dp) != NULL; dp = &d->next)
		if (d->dev == dev) {
			*dp = d->next;
			libinput_device_unref(d->dev);
			free(d);
			return;
		}
}

static void
device_added(struct ws_server *srv, struct libinput_device *dev)
{
	const char *name = libinput_device_get_name(dev);

	if (srv->cfg.ignore_mux_devices && is_mux_device(name)) {
		libinput_device_set_user_data(dev, DEV_IGNORED);
		ws_info("input: ignoring %s (IgnoreMuxDevices)", name);
		return;
	}
	ws_info("input: %s (%s)%s%s%s", name,
	    libinput_device_get_sysname(dev),
	    libinput_device_has_capability(dev,
	    LIBINPUT_DEVICE_CAP_KEYBOARD) ? " keyboard" : "",
	    libinput_device_has_capability(dev,
	    LIBINPUT_DEVICE_CAP_POINTER) ? " pointer" : "",
	    libinput_device_has_capability(dev,
	    LIBINPUT_DEVICE_CAP_TOUCH) ? " touch" : "");
	device_track(srv, dev);
	configure_device(srv, dev);
}

static uint32_t
map_button(uint32_t code)
{
	switch (code) {
	case BTN_LEFT:
		return WS_BUTTON_LEFT;
	case BTN_RIGHT:
		return WS_BUTTON_RIGHT;
	case BTN_MIDDLE:
		return WS_BUTTON_MIDDLE;
	default:
		return code >= BTN_LEFT && code < BTN_LEFT + 29 ?
		    code - BTN_LEFT : 31;
	}
}

static void
handle_scroll(struct ws_server *srv, struct libinput_event_pointer *p,
    uint32_t source)
{
	enum libinput_pointer_axis v = LIBINPUT_POINTER_AXIS_SCROLL_VERTICAL;
	enum libinput_pointer_axis h = LIBINPUT_POINTER_AXIS_SCROLL_HORIZONTAL;
	double dx = 0, dy = 0;
	int32_t vx = 0, vy = 0;

	if (libinput_event_pointer_has_axis(p, h)) {
		dx = libinput_event_pointer_get_scroll_value(p, h);
		if (source == WS_SCROLL_WHEEL)
			vx = (int32_t)libinput_event_pointer_get_scroll_value_v120(p, h);
	}
	if (libinput_event_pointer_has_axis(p, v)) {
		dy = libinput_event_pointer_get_scroll_value(p, v);
		if (source == WS_SCROLL_WHEEL)
			vy = (int32_t)libinput_event_pointer_get_scroll_value_v120(p, v);
	}
	if (source == WS_SCROLL_FINGER) {
		/* Zero on every axis it names: the fingers lifted. */
		scroll_finger(srv, dx, dy, dx == 0 && dy == 0,
		    libinput_event_pointer_get_time_usec(p));
		return;
	}
	if (dx != 0 || dy != 0 || vx != 0 || vy != 0)
		pointer_scroll(srv, dx, dy, vx, vy, source);
}

static void
handle_event(struct ws_server *srv, struct libinput_event *ev)
{
	struct libinput_device *dev = libinput_event_get_device(ev);
	struct libinput_event_pointer *p;
	struct libinput_event_keyboard *k;
	struct libinput_event_gesture *g;
	enum libinput_event_type type;
	uint32_t key, count;
	bool pressed;

	if (libinput_event_get_type(ev) == LIBINPUT_EVENT_DEVICE_ADDED) {
		device_added(srv, dev);
		return;
	}
	if (libinput_device_get_user_data(dev) == DEV_IGNORED)
		return;

	switch (libinput_event_get_type(ev)) {
	case LIBINPUT_EVENT_DEVICE_REMOVED:
		ws_info("input: %s removed", libinput_device_get_name(dev));
		device_untrack(srv, dev);
		break;
	case LIBINPUT_EVENT_KEYBOARD_KEY:
		k = libinput_event_get_keyboard_event(ev);
		key = libinput_event_keyboard_get_key(k);
		pressed = libinput_event_keyboard_get_key_state(k) ==
		    LIBINPUT_KEY_STATE_PRESSED;
		count = libinput_event_keyboard_get_seat_key_count(k);
		/* Only the first press / last release on the seat counts. */
		if ((pressed && count != 1) || (!pressed && count != 0))
			break;
		keyboard_key(srv, key, pressed);
		break;
	case LIBINPUT_EVENT_POINTER_MOTION:
		p = libinput_event_get_pointer_event(ev);
		pointer_motion(srv,
		    srv->ptr_x + libinput_event_pointer_get_dx(p),
		    srv->ptr_y + libinput_event_pointer_get_dy(p));
		break;
	case LIBINPUT_EVENT_POINTER_MOTION_ABSOLUTE:
		p = libinput_event_get_pointer_event(ev);
		pointer_motion(srv,
		    libinput_event_pointer_get_absolute_x_transformed(p,
		    srv->width),
		    libinput_event_pointer_get_absolute_y_transformed(p,
		    srv->height));
		break;
	case LIBINPUT_EVENT_POINTER_BUTTON:
		p = libinput_event_get_pointer_event(ev);
		count = libinput_event_pointer_get_seat_button_count(p);
		pressed = libinput_event_pointer_get_button_state(p) ==
		    LIBINPUT_BUTTON_STATE_PRESSED;
		if ((pressed && count != 1) || (!pressed && count != 0))
			break;
		key = libinput_event_pointer_get_button(p);
		/* SecondaryClick Off: a two-finger tap is a left click too. */
		if (key == BTN_RIGHT &&
		    libinput_device_get_user_data(dev) == DEV_TOUCHPAD &&
		    strcasecmp(srv->cfg.secondary_click, "Off") == 0)
			key = BTN_LEFT;
		pointer_button(srv, map_button(key), pressed);
		break;
	case LIBINPUT_EVENT_POINTER_SCROLL_WHEEL:
		handle_scroll(srv, libinput_event_get_pointer_event(ev),
		    WS_SCROLL_WHEEL);
		break;
	case LIBINPUT_EVENT_POINTER_SCROLL_FINGER:
		handle_scroll(srv, libinput_event_get_pointer_event(ev),
		    WS_SCROLL_FINGER);
		break;
	case LIBINPUT_EVENT_POINTER_SCROLL_CONTINUOUS:
		handle_scroll(srv, libinput_event_get_pointer_event(ev),
		    WS_SCROLL_CONTINUOUS);
		break;
	case LIBINPUT_EVENT_GESTURE_HOLD_BEGIN:
		/* Fingers resting on the touchpad stop momentum scrolling. */
		scroll_touch(srv);
		break;
	case LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN:
	case LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE:
	case LIBINPUT_EVENT_GESTURE_SWIPE_END:
		/* Three-finger swipes up and down: the overview. */
		g = libinput_event_get_gesture_event(ev);
		type = libinput_event_get_type(ev);
		overview_swipe(srv,
		    type == LIBINPUT_EVENT_GESTURE_SWIPE_BEGIN ?
		    WS_GESTURE_BEGIN :
		    type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE ?
		    WS_GESTURE_UPDATE :
		    libinput_event_gesture_get_cancelled(g) ?
		    WS_GESTURE_CANCEL : WS_GESTURE_END,
		    (unsigned)libinput_event_gesture_get_finger_count(g),
		    type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE ?
		    libinput_event_gesture_get_dx_unaccelerated(g) : 0,
		    type == LIBINPUT_EVENT_GESTURE_SWIPE_UPDATE ?
		    libinput_event_gesture_get_dy_unaccelerated(g) : 0);
		break;
	default:
		/* LIBINPUT_EVENT_POINTER_AXIS is the deprecated duplicate of
		 * the SCROLL_* events. Touch, tablet, other gestures and
		 * switches: TODO. */
		break;
	}
}

void
input_readable(struct ws_server *srv)
{
	struct libinput_event *ev;

	if (srv->libinput == NULL)
		return;
	if (libinput_dispatch(srv->libinput) != 0)
		ws_warn("libinput_dispatch failed");
	while ((ev = libinput_get_event(srv->libinput)) != NULL) {
		handle_event(srv, ev);
		libinput_event_destroy(ev);
	}
}

void
input_close(struct ws_server *srv)
{
	struct ws_input_device *d;

	while ((d = srv->input_devices) != NULL) {
		srv->input_devices = d->next;
		libinput_device_unref(d->dev);
		free(d);
	}
	if (srv->libinput != NULL)
		libinput_unref(srv->libinput);
	if (srv->udev != NULL)
		udev_unref(srv->udev);
	srv->libinput = NULL;
	srv->udev = NULL;
}
