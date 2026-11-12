/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * WSTypes.h - constants and event structures shared by WindowServer and
 * its clients. This header has no Mach or system dependencies beyond
 * <stdint.h>, so it can be included by any client, including the LGPL
 * GNUstep backend.
 *
 * Coordinates: screen and window coordinates are in pixels with the
 * origin at the top-left corner and y growing downwards. Window-local
 * coordinates are relative to the window frame's top-left corner.
 * (GNUstep's bottom-left origin is converted by the backend.)
 */
#ifndef _WINDOWSERVER_WSTYPES_H_
#define _WINDOWSERVER_WSTYPES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped whenever a wire structure changes incompatibly. */
#define WS_PROTOCOL_VERSION	1

/* Mach service name, registered with the bootstrap server. */
#define WS_SERVICE_NAME		"org.nextbsd.windowserver"
/* Environment variable that overrides the service name (development). */
#define WS_SERVICE_NAME_ENV	"WS_SERVICE_NAME"

/*
 * Window levels. Client levels use the same numbers as libs-gui's
 * NSWindowLevel values (Headers/AppKit/NSWindow.h), so the GNUstep
 * backend can pass them through unchanged.
 */
#define WS_LEVEL_DESKTOP	(-1000)	/* NSDesktopWindowLevel */
#define WS_LEVEL_NORMAL		0	/* NSNormalWindowLevel */
#define WS_LEVEL_FLOATING	2	/* NSFloatingWindowLevel */
#define WS_LEVEL_SUBMENU	3	/* NSSubmenuWindowLevel */
#define WS_LEVEL_TORN_OFF_MENU	3	/* NSTornOffMenuWindowLevel */
#define WS_LEVEL_MAIN_MENU	20	/* NSMainMenuWindowLevel */
#define WS_LEVEL_STATUS		21	/* NSStatusWindowLevel */
#define WS_LEVEL_DOCK		21	/* NSDockWindowLevel (deprecated) */
#define WS_LEVEL_MODAL_PANEL	100	/* NSModalPanelWindowLevel */
#define WS_LEVEL_POPUP_MENU	101	/* NSPopUpMenuWindowLevel */
#define WS_LEVEL_SCREEN_SAVER	1000	/* NSScreenSaverWindowLevel */
/* Highest level a client may ask for; higher values are clamped. */
#define WS_LEVEL_CLIENT_MAX	1999
/* Server-only tiers above every client window. */
#define WS_LEVEL_SWITCHER	2000	/* Command-Tab switcher */
#define WS_LEVEL_SHIELD		3000	/* lock / login shield */
#define WS_LEVEL_CURSOR		4000	/* software cursor */

/* Ordering operations, same values as NSWindowOrderingMode. */
#define WS_ORDER_OUT		0
#define WS_ORDER_ABOVE		1
#define WS_ORDER_BELOW		(-1)

/* Window flags (ws_window_create / ws_window_set_flags). */
#define WS_WINDOW_NO_KEY	0x0001	/* never becomes the key window */
#define WS_WINDOW_NO_ACTIVATE	0x0002	/* clicks don't activate the app */
#define WS_WINDOW_OPAQUE	0x0004	/* buffer alpha can be ignored */
#define WS_WINDOW_IGNORES_MOUSE	0x0008	/* pointer events pass through */
#define WS_WINDOW_ICON		0x0010	/* app icon or miniwindow: the server
					   finds it a free place (see window.c) */
/* Window tiling (servers with WS_SERVER_TILING; "Window tiling" in
   WSProtocol.h): windows with any of these float in tiling mode. */
#define WS_WINDOW_NO_TILE	0x0020	/* the app never wants it tiled */
#define WS_WINDOW_FIXED_SIZE	0x0040	/* the user can't resize it */
#define WS_WINDOW_PANEL		0x0080	/* a panel, alert or sheet */
/* Drag and drop (servers with WS_SERVER_DRAG; "Drag and drop" in
   WSProtocol.h): the window takes drags, so WindowServer sends it
   WS_EVENT_DRAG while a drag is over it. */
#define WS_WINDOW_ACCEPTS_DRAGS	0x0100

/*
 * Pixel formats. Window buffers can be either (XRGB8888 only on servers
 * with WS_SERVER_XRGB); snapshots are XRGB8888. An XRGB8888 buffer's
 * top byte is ignored and the window is composited opaque, as an X
 * server's depth-24 windows need.
 */
#define WS_FORMAT_ARGB8888	1	/* 32 bpp, premultiplied, native endian */
#define WS_FORMAT_XRGB8888	2	/* 32 bpp, top byte unused, native endian */

/* Modifier flags. */
#define WS_MOD_SHIFT		0x0001
#define WS_MOD_CONTROL		0x0002
#define WS_MOD_ALT		0x0004	/* Option / Alternate */
#define WS_MOD_COMMAND		0x0008	/* Super / Logo key */
#define WS_MOD_CAPSLOCK		0x0010
#define WS_MOD_NUMLOCK		0x0020

/* Mouse buttons (event.pointer.button); masks are 1 << button. */
#define WS_BUTTON_LEFT		0
#define WS_BUTTON_RIGHT		1
#define WS_BUTTON_MIDDLE	2

/* Scroll sources. */
#define WS_SCROLL_WHEEL		0	/* detents; v120 fields are set */
#define WS_SCROLL_FINGER	1	/* touchpad, precise */
#define WS_SCROLL_CONTINUOUS	2	/* trackpoint/button scrolling */

/*
 * Scroll phases (ws_event_scroll_t.phase and .momentum_phase, servers
 * with WS_SERVER_SCROLL_PHASES), the same values as NSEventPhase, as on
 * macOS. Touchpad scrolls (WS_SCROLL_FINGER) come as one BEGAN event,
 * CHANGED events, then an ENDED event with no delta when the fingers
 * lift. If they were moving, momentum scrolling follows: events with
 * phase NONE and momentum_phase BEGAN, CHANGED..., then ENDED (no
 * delta) when it has slowed down or a finger touches the touchpad
 * again. Wheel and other scrolls have both phases NONE.
 */
#define WS_SCROLL_PHASE_NONE		0
#define WS_SCROLL_PHASE_BEGAN		0x01
#define WS_SCROLL_PHASE_STATIONARY	0x02
#define WS_SCROLL_PHASE_CHANGED		0x04
#define WS_SCROLL_PHASE_ENDED		0x08
#define WS_SCROLL_PHASE_CANCELLED	0x10
#define WS_SCROLL_PHASE_MAY_BEGIN	0x20

/* Key event flags. */
#define WS_KEY_REPEAT		0x0001	/* generated by server key repeat */

/* Standard cursors (ws_set_cursor). */
#define WS_CURSOR_ARROW		0
#define WS_CURSOR_IBEAM		1
#define WS_CURSOR_POINTING_HAND	2
#define WS_CURSOR_CLOSED_HAND	3
#define WS_CURSOR_OPEN_HAND	4
#define WS_CURSOR_CROSSHAIR	5
#define WS_CURSOR_RESIZE_LR	6
#define WS_CURSOR_RESIZE_UD	7
#define WS_CURSOR_NOT_ALLOWED	8
/* Servers with WS_SERVER_DRAG, which shows them during a drag. */
#define WS_CURSOR_DRAG_COPY	9	/* arrow with a green plus */
#define WS_CURSOR_DRAG_LINK	10	/* arrow with a link arrow */
#define WS_CURSOR_CUSTOM	100	/* image from a client buffer */
/* A window's cursor only (WS_MSG_WINDOW_SET_CURSOR, servers with
   WS_SERVER_WINDOW_CURSOR): the window shows its connection's cursor
   again. */
#define WS_CURSOR_INHERIT	101

/* Commit flags. */
#define WS_COMMIT_FRAME_DONE	0x0001	/* send WS_EVENT_FRAME_DONE */
#define WS_COMMIT_SET_FRAME	0x0002	/* the commit carries the window's
					   new frame (ws_req_commit_t,
					   servers with WS_SERVER_COMMIT_FRAME) */

/* Pointer grabs (WS_MSG_POINTER_GRAB, servers with WS_SERVER_POINTER_GRAB;
   see "Pointer grabs" in WSProtocol.h). */
#define WS_GRAB_POINTER		1	/* every pointer event comes to the
					   grabbing connection */
#define WS_GRAB_OUTSIDE		2	/* events go where they would; a press
					   outside the connection's windows
					   sends WS_EVENT_POINTER_OUTSIDE and
					   ends the grab (popup menus) */

/* Input regions (WS_MSG_WINDOW_SET_INPUT_REGION). */
#define WS_REGION_APPEND	0x0001	/* add to the region sent so far */
#define WS_REGION_RESET		0x0002	/* the whole window again (rects
					   are ignored) */

/* Connection flags (ws_connect). */
#define WS_CONNECT_CONTROL	0x0001	/* tool connection (wsctl): not an app */
#define WS_CONNECT_JOIN		0x0002	/* join another connection's app
					   (ws_connect_join, WSProtocol.h) */
#define WS_CONNECT_XKB		0x0004	/* also send WS_EVENT_KEYMAP,
					   WS_EVENT_MODIFIERS and
					   WS_EVENT_KEYS_HELD */
#define WS_CONNECT_DOCK		0x0008	/* the Dock: every app's windows,
					   change notices, previews (see
					   "The Dock" in WSProtocol.h);
					   not an app */

/* Server flags in WS_EVENT_WELCOME. */
#define WS_SERVER_HEADLESS	0x0001
#define WS_SERVER_HW_CURSOR	0x0002
#define WS_SERVER_JOIN		0x0004	/* accepts WS_CONNECT_JOIN */
#define WS_SERVER_XKB		0x0008	/* accepts WS_CONNECT_XKB */
#define WS_SERVER_XRGB		0x0010	/* accepts WS_FORMAT_XRGB8888 buffers */
#define WS_SERVER_ORDERED	0x0020	/* sends WS_EVENT_WINDOW_ORDERED */
#define WS_SERVER_MINIMIZE	0x0040	/* minimizes windows itself:
					   WS_MSG_WINDOW_MINIMIZE,
					   WS_MSG_SET_APP_ICON and
					   WS_EVENT_WINDOW_RESTORE */
#define WS_SERVER_DOCK		0x0080	/* accepts WS_CONNECT_DOCK */
#define WS_SERVER_SCROLL_PHASES	0x0100	/* scroll phases and momentum
					   scrolling (WS_SCROLL_PHASE_*) */
#define WS_SERVER_OVERVIEW	0x0200	/* the window overview (Exposé),
					   WS_MSG_OVERVIEW */
#define WS_SERVER_DOCK_EDGE	0x0400	/* the Dock's edge: WS_MSG_DOCK_SET_EDGE
					   and the edge in WS_EVENT_DOCK_STATE */
#define WS_SERVER_MINIMIZE_WHERE 0x0800	/* WS_MSG_WINDOW_MINIMIZE may say
					   where the window goes
					   (WS_MINIMIZE_*) */
#define WS_SERVER_USABLE_AREA	0x1000	/* the usable area: in the welcome
					   and WS_EVENT_USABLE_AREA */
#define WS_SERVER_SCREEN_CHANGES 0x2000	/* sends WS_EVENT_SCREEN_CHANGED
					   when the screen's size or mode
					   changes */
#define WS_SERVER_SETTINGS	0x4000	/* settings change while it runs:
					   WS_MSG_SET_SETTINGS and _GET_,
					   keyboard layouts and input
					   devices ("Settings" in
					   WSProtocol.h) */
#define WS_SERVER_LATIN_SHORTCUTS 0x8000 /* while a layout that isn't
					   Latin (gr, ru) is active, keys
					   pressed with Command held carry
					   the keysym and text of the first
					   Latin layout in the list, or us
					   ("Command shortcuts" in
					   WSProtocol.h) */
#define WS_SERVER_POINTER_GRAB	0x00010000 /* WS_MSG_POINTER_GRAB and
					   _UNGRAB, WS_EVENT_POINTER_OUTSIDE
					   and _UNGRABBED ("Pointer grabs" in
					   WSProtocol.h) */
#define WS_SERVER_POINTER_WARP	0x00020000 /* WS_MSG_WARP_POINTER,
					   WS_MSG_QUERY_POINTER and
					   WS_MSG_POINTER_TRACK ("Pointer
					   warp and query") */
#define WS_SERVER_WINDOW_CURSOR	0x00040000 /* WS_MSG_WINDOW_SET_CURSOR: a
					   cursor per window */
#define WS_SERVER_INPUT_REGION	0x00080000 /* WS_MSG_WINDOW_SET_INPUT_REGION
					   (shaped windows) */
#define WS_SERVER_COMMIT_FRAME	0x00100000 /* WS_COMMIT_SET_FRAME: a frame
					   carried in the commit */
#define WS_SERVER_WINDOW_IDS	0x00200000 /* WS_MSG_RESERVE_WINDOW_IDS and
					   a one-way WS_MSG_WINDOW_CREATE
					   naming its own id ("Window ids
					   chosen by the client") */
#define WS_SERVER_MENU_BAR	0x00400000 /* WS_MSG_SET_MENU_BAR: the active
					   app's menu bar strip along the top
					   comes off the usable area ("The
					   menu bar strip" in WSProtocol.h) */
#define WS_SERVER_ACTIVATE_RAISE 0x00800000 /* WS_MSG_ACTIVATE with
					   WS_ACTIVATE_RAISE brings the app's
					   windows forward ("Activation" in
					   WSProtocol.h) */
#define WS_SERVER_DOCK_NOT_ACTIVE 0x01000000 /* the Dock's app never becomes
					   the active app; the one before
					   stays active, menus included
					   ("Activation" in WSProtocol.h) */
#define WS_SERVER_TILING	0x02000000 /* window tiling: the WindowMode and
						   Tiling* settings, WS_WINDOW_NO_TILE,
						   _FIXED_SIZE and _PANEL,
						   WS_MSG_WINDOW_SET_SIZE_LIMITS and
						   WS_MSG_TILING ("Window tiling" in
						   WSProtocol.h) */
#define WS_SERVER_DRAG		0x04000000 /* drag and drop between apps:
						   WS_MSG_DRAG_*, WS_EVENT_DRAG*,
						   WS_WINDOW_ACCEPTS_DRAGS ("Drag
						   and drop" in WSProtocol.h) */

/* WS_MSG_ACTIVATE flags (ws_activate_flags). */
#define WS_ACTIVATE_RAISE	0x0001	/* also bring the app's windows,
					   joined connections' included, to
					   the front */

/* Where a minimized window goes ("Minimizing" in WSProtocol.h). */
#define WS_MINIMIZE_DEFAULT	0	/* the server's MinimizeIntoDock */
#define WS_MINIMIZE_BOTTOM_ROW	1	/* a tile on the bottom row */
#define WS_MINIMIZE_INTO_DOCK	2	/* the Dock, or a tile without one */

/* Synthetic input kinds (wsctl, headless testing). */
#define WS_INJECT_MOTION	1	/* absolute: x, y */
#define WS_INJECT_BUTTON	2	/* button, pressed */
#define WS_INJECT_KEY		3	/* key_name or keycode, pressed */
#define WS_INJECT_KEY_TAP	4	/* key_name: press + release */
#define WS_INJECT_SCROLL	5	/* dx, dy */
/* added after protocol 1 shipped (WS_SERVER_SCROLL_PHASES) */
#define WS_INJECT_SCROLL_FINGER	6	/* dx, dy as a two-finger touchpad
					   scroll; 0, 0: the fingers lift */
#define WS_INJECT_HOLD		7	/* fingers rest on the touchpad
					   (pressed 1) or lift (0) */
/* added after protocol 1 shipped (WS_SERVER_OVERVIEW) */
#define WS_INJECT_SWIPE		8	/* a touchpad swipe: button = fingers,
					   pressed = WS_GESTURE_*, dx, dy */
/* added after protocol 1 shipped (WS_SERVER_SCREEN_CHANGES) */
#define WS_INJECT_SCREEN_SIZE	9	/* headless servers only: the screen
					   becomes x by y pixels (tests) */

/* Gesture phases (WS_INJECT_SWIPE). */
#define WS_GESTURE_BEGIN	1
#define WS_GESTURE_UPDATE	2
#define WS_GESTURE_END		3
#define WS_GESTURE_CANCEL	4

typedef struct ws_rect {
	int32_t		x;
	int32_t		y;
	int32_t		width;
	int32_t		height;
} ws_rect_t;

typedef struct ws_screen_info {
	uint32_t	screen_id;
	int32_t		x;
	int32_t		y;
	uint32_t	width;
	uint32_t	height;
	uint32_t	scale;		/* 1 for now */
	uint32_t	refresh_mhz;	/* 60000 = 60 Hz, 0 if unknown */
	uint32_t	mm_width;
	uint32_t	mm_height;
	uint32_t	reserved;
	char		name[32];
} ws_screen_info_t;

/* Server -> client event types. */
enum {
	WS_EVENT_WELCOME = 1,
	WS_EVENT_SCREEN_CHANGED,	/* ws_screen_info_t; sent by servers
					   with WS_SERVER_SCREEN_CHANGES */
	WS_EVENT_APP_ACTIVATED,
	WS_EVENT_APP_DEACTIVATED,
	WS_EVENT_KEY_WINDOW_GAINED,
	WS_EVENT_KEY_WINDOW_LOST,
	WS_EVENT_WINDOW_FRAME,
	WS_EVENT_BUFFER_RELEASE,
	WS_EVENT_FRAME_DONE,
	WS_EVENT_MOUSE_DOWN,
	WS_EVENT_MOUSE_UP,
	WS_EVENT_MOUSE_MOVED,
	WS_EVENT_MOUSE_DRAGGED,
	WS_EVENT_MOUSE_ENTERED,
	WS_EVENT_MOUSE_EXITED,
	WS_EVENT_SCROLL,
	WS_EVENT_KEY_DOWN,
	WS_EVENT_KEY_UP,
	WS_EVENT_FLAGS_CHANGED,
	WS_EVENT_ERROR,
	WS_EVENT_SNAPSHOT,
	WS_EVENT_WINDOW_INFO,
	WS_EVENT_LIST_END,
	/* added after protocol 1 shipped; clients ignore unknown types */
	WS_EVENT_KEYMAP,		/* WS_CONNECT_XKB only */
	WS_EVENT_MODIFIERS,		/* WS_CONNECT_XKB only */
	WS_EVENT_KEYS_HELD,		/* WS_CONNECT_XKB only */
	WS_EVENT_WINDOW_ORDERED,
	WS_EVENT_WINDOW_RESTORE,	/* WS_SERVER_MINIMIZE; no payload */
	/* WS_CONNECT_DOCK only (WS_SERVER_DOCK) */
	WS_EVENT_DOCK_STATE,		/* ws_event_dock_state_t */
	WS_EVENT_WINDOW_LIST_CHANGED,	/* no payload: list again */
	WS_EVENT_PREVIEW_CHANGED,	/* about hdr.window; no payload */
	WS_EVENT_PREVIEW,		/* ws_event_snapshot_t, with a memfd */
	/* WS_SERVER_OVERVIEW: to tools that ask (WS_MSG_OVERVIEW) and to
	   Dock connections when the overview opens and closes */
	WS_EVENT_OVERVIEW_STATE,	/* ws_event_overview_t */
	WS_EVENT_OVERVIEW_ITEM,		/* ws_event_overview_item_t */
	/* WS_SERVER_USABLE_AREA: to every connection */
	WS_EVENT_USABLE_AREA,		/* ws_event_usable_area_t */
	/* WS_SERVER_SETTINGS ("Settings" in WSProtocol.h) */
	WS_EVENT_SETTING,		/* ws_event_setting_t, answers
					   WS_MSG_GET_SETTINGS */
	WS_EVENT_SETTINGS_CHANGED,	/* tools and the Dock; no payload:
					   get them again */
	WS_EVENT_KEYBOARD_LAYOUT,	/* ws_event_keyboard_layout_t: to tools
					   and the Dock when the active layout
					   changes, and answering
					   WS_MSG_LIST_KEYBOARD_LAYOUTS */
	WS_EVENT_INPUT_DEVICE,		/* ws_event_input_device_t, answers
					   WS_MSG_LIST_INPUT_DEVICES */
	/* WS_SERVER_POINTER_GRAB ("Pointer grabs" in WSProtocol.h) */
	WS_EVENT_POINTER_OUTSIDE,	/* ws_event_pointer_t (screen position
					   and button); hdr.window 0 */
	WS_EVENT_POINTER_UNGRABBED,	/* no payload: the server ended this
					   connection's grab */
	/* WS_SERVER_TILING: answers WS_MSG_TILING ("Window tiling" in
	   WSProtocol.h) */
	WS_EVENT_TILING_STATE,		/* ws_event_tiling_t */
	WS_EVENT_TILING_ITEM,		/* ws_event_tiling_item_t */
	/* WS_SERVER_DRAG ("Drag and drop" in WSProtocol.h) */
	WS_EVENT_DRAG,			/* ws_event_drag_t: to the window a
					   drag is over */
	WS_EVENT_DRAG_STATUS,		/* ws_event_drag_status_t: to the
					   drag's source */
	WS_EVENT_DRAG_END,		/* ws_event_drag_end_t: to the drag's
					   source */
	WS_EVENT_DRAG_STATE,		/* ws_event_drag_state_t, answers
					   WS_MSG_DRAG_QUERY */
	WS_EVENT_MAX
};

/*
 * Every event starts with this header. `serial` is a server-wide
 * counter, so the order of events delivered to different clients can
 * be compared. `window` is 0 when the event isn't about a window.
 */
typedef struct ws_event_header {
	uint16_t	type;
	uint16_t	size;		/* bytes, header included */
	uint32_t	serial;
	uint64_t	time_usec;	/* CLOCK_MONOTONIC */
	uint32_t	window;
	uint32_t	reserved;
} ws_event_header_t;

typedef struct ws_event_welcome {
	uint32_t	client_id;
	uint32_t	protocol_version;
	uint32_t	server_flags;
	uint32_t	screen_count;
	ws_screen_info_t screen;
	/* added after protocol 1 shipped (WS_SERVER_USABLE_AREA); older
	   servers send the welcome without it, and it reads as 0 */
	ws_rect_t	usable;		/* see ws_event_usable_area_t */
	/* added after protocol 1 shipped (WS_SERVER_MENU_BAR); read as 0
	   from older servers */
	uint32_t	menu_bar;	/* see ws_event_usable_area_t */
	uint32_t	reserved2;
} ws_event_welcome_t;

/*
 * The usable area (servers with WS_SERVER_USABLE_AREA): the part of
 * the screen that windows should use, the screen minus the menu bar
 * strip along the top (WS_MSG_SET_MENU_BAR) and the strip the Dock
 * keeps on its edge (WS_MSG_DOCK_SET_EDGE); a top Dock's strip is
 * below the menu bar's. It is what NSScreen's visibleFrame is on macOS:
 * new windows are centred and cascaded inside it, zooming fills it,
 * and WindowServer puts its own things (minimized-window tiles, the
 * overview) there. Every connection gets it in the welcome and then
 * WS_EVENT_USABLE_AREA whenever it changes: the Dock moved to another
 * edge, got thicker or thinner, connected or went away, the menu bar
 * strip changed, or the screen changed (after WS_EVENT_SCREEN_CHANGED).
 * Screen coordinates. `menu_bar` is the height of the menu bar strip
 * (0 for none), which a top Dock sits directly below.
 */
typedef struct ws_event_usable_area {
	uint32_t	screen_id;
	uint32_t	reserved;
	ws_rect_t	usable;
	/* added after protocol 1 shipped (WS_SERVER_MENU_BAR); older
	   servers send the event without it, and it reads as 0 */
	uint32_t	menu_bar;	/* pixels the menu bar takes from the
					   top of the screen */
	uint32_t	reserved2;
} ws_event_usable_area_t;

typedef struct ws_event_app {
	uint32_t	other_client;	/* the app on the other side, or 0 */
	uint32_t	reserved;
} ws_event_app_t;

typedef struct ws_event_frame {
	ws_rect_t	frame;
	/* added after protocol 1 shipped (WS_SERVER_TILING); older servers
	   send the event without them, and they read as 0 */
	uint32_t	flags;		/* WS_FRAME_* */
	uint32_t	reserved;
} ws_event_frame_t;

/*
 * WS_EVENT_WINDOW_FRAME flags. WS_FRAME_TILED: the frame is the window's
 * tile (see "Window tiling" in WSProtocol.h), not one the owner or the
 * user chose. An owner that saves its windows' frames (GNUstep's frame
 * autosave) keeps the frame from before tiling instead; a frame without
 * it (leaving tiling gives the old frame back) is an ordinary one.
 */
#define WS_FRAME_TILED		0x0001

typedef struct ws_event_buffer {
	uint32_t	buffer_id;
	uint32_t	reserved;
} ws_event_buffer_t;

/*
 * Pointer events. hdr.window is the window the pointer is over (or the
 * one a held button was pressed in), and x, y are local to it. With a
 * pointer grab or pointer tracking ("Pointer grabs" and "Pointer warp
 * and query" in WSProtocol.h) a connection also gets pointer events
 * while the pointer is over none of its windows: hdr.window is then 0
 * and x, y are the screen position, like screen_x, screen_y.
 */
typedef struct ws_event_pointer {
	double		x;		/* window-local (screen if hdr.window
					   is 0) */
	double		y;
	double		screen_x;
	double		screen_y;
	uint32_t	button;		/* WS_BUTTON_* for down/up */
	uint32_t	click_count;
	uint32_t	buttons;	/* mask of buttons held */
	uint32_t	modifiers;
} ws_event_pointer_t;

/*
 * Where the pointer is (WS_MSG_QUERY_POINTER, servers with
 * WS_SERVER_POINTER_WARP), in the reply's u.pointer.
 */
typedef struct ws_pointer_info {
	double		x;		/* screen position */
	double		y;
	uint32_t	buttons;	/* mask of buttons held */
	uint32_t	modifiers;	/* WS_MOD_* */
	uint32_t	window;		/* the asking connection's window under
					   the pointer, 0 = none of its own */
	uint32_t	reserved;
} ws_pointer_info_t;

typedef struct ws_event_scroll {
	double		x;		/* window-local pointer position */
	double		y;
	double		dx;		/* pixels, positive = right */
	double		dy;		/* pixels, positive = down */
	int32_t		v120_x;		/* wheel detents * 120 (wheel only) */
	int32_t		v120_y;
	uint32_t	source;		/* WS_SCROLL_* */
	uint32_t	modifiers;
	/* added after protocol 1 shipped (WS_SERVER_SCROLL_PHASES); older
	   servers send the event without them, and they read as 0 */
	uint32_t	phase;		/* WS_SCROLL_PHASE_*: the fingers */
	uint32_t	momentum_phase;	/* WS_SCROLL_PHASE_*: momentum after
					   they lifted */
} ws_event_scroll_t;

typedef struct ws_event_key {
	uint32_t	keycode;	/* XKB keycode = evdev code + 8 */
	uint32_t	keysym;		/* XKB keysym with modifiers applied */
	uint32_t	modifiers;	/* WS_MOD_* in effect */
	uint32_t	flags;		/* WS_KEY_REPEAT */
	char		text[16];	/* UTF-8 with modifiers, NUL-terminated */
	char		text_unmodified[16]; /* UTF-8 of the level-1 symbol */
} ws_event_key_t;

typedef struct ws_event_flags {
	uint32_t	modifiers;
	uint32_t	reserved;
} ws_event_flags_t;

typedef struct ws_event_error {
	uint32_t	request;	/* Mach message id that failed */
	int32_t		code;		/* errno value */
	char		message[64];
} ws_event_error_t;

typedef struct ws_event_snapshot {
	uint32_t	width;
	uint32_t	height;
	uint32_t	stride;
	uint32_t	format;		/* WS_FORMAT_XRGB8888 */
	int32_t		fd;		/* filled in by the client library */
	uint32_t	reserved;
} ws_event_snapshot_t;

/*
 * XKB keyboard state, for clients that connect with WS_CONNECT_XKB (an
 * X server, which runs its own XKB). Others get only the WS_MOD_* bits
 * in key and pointer events and WS_EVENT_FLAGS_CHANGED.
 *
 * WS_EVENT_KEYMAP comes right after WS_EVENT_WELCOME, and again if the
 * server's keymap changes. It carries a read-only, sealed memfd holding
 * the compiled keymap (xkb_keymap_get_as_string(), XKB_KEYMAP_FORMAT_
 * TEXT_V1) as a NUL-terminated string of `size` bytes. mmap it
 * (PROT_READ, MAP_PRIVATE) rather than read() it: every client gets the
 * same open file. The client must close the fd.
 */
#define WS_KEYMAP_XKB_V1	1

typedef struct ws_event_keymap {
	uint32_t	format;		/* WS_KEYMAP_XKB_V1 */
	uint32_t	size;		/* bytes, NUL included */
	uint32_t	repeat_delay;	/* ms before the first repeat */
	uint32_t	repeat_interval; /* ms between repeats, 0 = off */
	int32_t		fd;		/* filled in by the client library */
	uint32_t	reserved;
} ws_event_keymap_t;

/*
 * WS_EVENT_MODIFIERS: the XKB modifier and layout state, as
 * xkb_state_serialize_mods() and xkb_state_serialize_layout() give it,
 * like Wayland's wl_keyboard.modifiers. Sent to the connection that
 * gets key events when it starts getting them (after WS_EVENT_KEYS_HELD)
 * and whenever the state changes while it does, after the key event
 * that changed it (or a layout switch, WS_MSG_SET_KEYBOARD_LAYOUT), and
 * to every WS_CONNECT_XKB connection after a new WS_EVENT_KEYMAP.
 */
typedef struct ws_event_modifiers {
	uint32_t	depressed;	/* XKB_STATE_MODS_DEPRESSED */
	uint32_t	latched;	/* XKB_STATE_MODS_LATCHED */
	uint32_t	locked;		/* XKB_STATE_MODS_LOCKED */
	uint32_t	group;		/* XKB_STATE_LAYOUT_EFFECTIVE */
	uint32_t	modifiers;	/* WS_MOD_* */
	uint32_t	reserved;
} ws_event_modifiers_t;

/*
 * WS_EVENT_KEYS_HELD: the keys held down when a connection starts
 * getting key events (its app is activated, or the key window moves to
 * it). Their key-ups come to it later. Sent with count 0 too.
 */
#define WS_MAX_KEYS_HELD	32

typedef struct ws_event_keys_held {
	uint32_t	count;
	uint32_t	reserved;
	uint32_t	keycodes[WS_MAX_KEYS_HELD]; /* XKB keycodes */
} ws_event_keys_held_t;

/*
 * WS_EVENT_WINDOW_ORDERED (about hdr.window): the server moved one of
 * the client's windows in the stack, or ordered it out, on its own
 * (RaiseOnClick, for now), so a client that keeps its own copy of the
 * stacking order, such as an X server, can follow. A client's own
 * ordering, level and destroy requests never cause one. The new place
 * is given by the client's own visible windows next to it, on any
 * level.
 */
typedef struct ws_event_ordered {
	int32_t		op;		/* WS_ORDER_ABOVE: in the stack at a new
					   place; WS_ORDER_OUT: ordered out */
	uint32_t	below;		/* the client's nearest visible window
					   below it, 0 = none (or ordered out) */
	uint32_t	above;		/* its nearest visible window above it,
					   0 = none (or ordered out) */
	int32_t		level;
} ws_event_ordered_t;

/*
 * WS_EVENT_WINDOW_RESTORE (about hdr.window, no payload): the user
 * double-clicked the tile of a window the client minimized
 * (WS_MSG_WINDOW_MINIMIZE, WSProtocol.h). The server has activated the
 * app; the client restores the window by ordering it in, which removes
 * the tile.
 */

/*
 * Window list entry (wsctl list, the Dock). A minimized window is listed where its tile is in the stack, with
 * WS_WINDOW_INFO_MINIMIZED instead of WS_WINDOW_INFO_VISIBLE, and the
 * frame of the window itself. A minimized window the Dock shows has no
 * tile (WS_WINDOW_INFO_IN_DOCK) and is listed after the stack, and so
 * are ordered-out windows when the list asked for them
 * (WS_LIST_ORDERED_OUT); those have neither VISIBLE nor MINIMIZED, and
 * stack_index WS_STACK_INDEX_NONE.
 */
#define WS_WINDOW_INFO_VISIBLE	0x0001
#define WS_WINDOW_INFO_KEY	0x0002	/* key window of the app; with
					   ACTIVE, the key window on screen */
#define WS_WINDOW_INFO_ACTIVE	0x0004	/* owner is the active app */
#define WS_WINDOW_INFO_MINIMIZED 0x0008	/* minimized */
#define WS_WINDOW_INFO_IN_DOCK	0x0010	/* minimized, and the Dock shows it
					   instead of a tile */
#define WS_WINDOW_INFO_TILED	0x0020	/* tiled (WS_SERVER_TILING) */

#define WS_STACK_INDEX_NONE	0xffffffffu

typedef struct ws_event_window_info {
	uint32_t	client_id;
	int32_t		pid;
	int32_t		level;
	uint32_t	flags;		/* WS_WINDOW_INFO_* */
	ws_rect_t	frame;
	uint32_t	stack_index;	/* 0 = bottom of the visible stack */
	uint32_t	reserved;
	char		app_name[32];
	char		title[64];
	/* added after protocol 1 shipped (WS_SERVER_DOCK); older servers
	   send the event without them, and they read as 0 */
	uint32_t	window;		/* its id, as in hdr.window */
	uint32_t	app_id;		/* client id of the owner's app */
	uint32_t	window_flags;	/* WS_WINDOW_* the owner set */
	uint32_t	reserved2;
} ws_event_window_info_t;

/* Options for the window list (ws_list_windows_flags). */
#define WS_LIST_ORDERED_OUT	0x0001	/* also windows that aren't shown */

/*
 * WS_EVENT_DOCK_STATE: sent to a WS_CONNECT_DOCK connection right after
 * the welcome, and again when it changes. With
 * WS_DOCK_MINIMIZED_WINDOWS this connection is the Dock that windows
 * minimized into the Dock go to: the server draws no tiles for those,
 * and this connection shows them (WS_WINDOW_INFO_IN_DOCK), with
 * previews from WS_MSG_DOCK_PREVIEW. Windows minimized onto the bottom
 * row keep their tiles. Only one Dock connection, one that sent
 * WS_MSG_DOCK_SHOW_MINIMIZED, has it at a time.
 */
#define WS_DOCK_MINIMIZED_WINDOWS 0x0001

/*
 * The screen edge the Dock keeps (servers with WS_SERVER_DOCK_EDGE):
 * a Dock connection reports it with WS_MSG_DOCK_SET_EDGE, and every
 * Dock connection gets it in WS_EVENT_DOCK_STATE, after the welcome and
 * whenever it changes. Window tiling keeps that strip of the screen free.
 */
#define WS_DOCK_EDGE_NONE	0	/* no Dock has said */
#define WS_DOCK_EDGE_LEFT	1
#define WS_DOCK_EDGE_RIGHT	2
#define WS_DOCK_EDGE_TOP	3
#define WS_DOCK_EDGE_BOTTOM	4

typedef struct ws_event_dock_state {
	uint32_t	flags;		/* WS_DOCK_* */
	uint32_t	reserved;
	/* added after protocol 1 shipped (WS_SERVER_DOCK_EDGE); older
	   servers send the event without them, and they read as 0 */
	uint32_t	edge;		/* WS_DOCK_EDGE_* */
	uint32_t	thickness;	/* pixels the Dock takes from that edge */
	uint32_t	edge_client;	/* the Dock connection that said so */
	uint32_t	reserved2;
} ws_event_dock_state_t;

/*
 * The window overview (Exposé / Mission Control, servers with
 * WS_SERVER_OVERVIEW). WindowServer shows every window of every app,
 * or only one app's (App Exposé), as live scaled-down previews grouped
 * by app with the app's icon under each group, over the dimmed
 * desktop. See "The overview" in WSProtocol.h.
 */
#define WS_OVERVIEW_CLOSE	0	/* WS_MSG_OVERVIEW values */
#define WS_OVERVIEW_ALL		1	/* every app's windows */
#define WS_OVERVIEW_APP		2	/* the active app's (App Exposé) */
#define WS_OVERVIEW_QUERY	3	/* state and items, nothing changes */

#define WS_OVERVIEW_STATE_CLOSED  0
#define WS_OVERVIEW_STATE_OPENING 1	/* animating in */
#define WS_OVERVIEW_STATE_OPEN	  2
#define WS_OVERVIEW_STATE_CLOSING 3	/* animating out */

typedef struct ws_event_overview {
	uint32_t	state;		/* WS_OVERVIEW_STATE_* */
	uint32_t	mode;		/* WS_OVERVIEW_ALL or _APP */
	uint32_t	count;		/* windows shown */
	uint32_t	selected;	/* the selected window, 0 = none */
	uint32_t	app_id;		/* App Exposé: the app shown */
	uint32_t	reserved;
} ws_event_overview_t;

#define WS_OVERVIEW_ITEM_SELECTED  0x0001
#define WS_OVERVIEW_ITEM_MINIMIZED 0x0002

typedef struct ws_event_overview_item {
	uint32_t	window;		/* as in hdr.window */
	uint32_t	app_id;		/* client id of the window's app */
	ws_rect_t	frame;		/* where its preview is, open */
	ws_rect_t	icon;		/* its group's app icon, 0x0 = none */
	uint32_t	flags;		/* WS_OVERVIEW_ITEM_* */
	uint32_t	group;		/* 0 = the first app group */
} ws_event_overview_item_t;

/*
 * Settings (servers with WS_SERVER_SETTINGS; see "Settings" in
 * WSProtocol.h). Every setting has a type, and its value travels as
 * text: "true"/"false" for booleans, decimal numbers in the C locale.
 */
#define WS_SETTING_STRING	's'
#define WS_SETTING_INTEGER	'i'
#define WS_SETTING_BOOLEAN	'b'
#define WS_SETTING_REAL		'r'	/* integers are accepted too */
#define WS_SETTING_DEFAULT	'd'	/* WS_MSG_SET_SETTINGS only: drop the
					   session's value, back to the
					   system's (value is "") */

/* WS_MSG_SET_SETTINGS flags ("Settings" in WSProtocol.h). */
#define WS_SETTINGS_REPLACE	0x0001	/* drop the session's values first */
#define WS_SETTINGS_RELOAD_SYSTEM 0x0002 /* read the system's layer again */
#define WS_SETTINGS_SKIP_INVALID 0x0004	/* leave out refused entries */

/* Where the value in effect comes from (ws_event_setting_t.source). */
#define WS_SETTING_SOURCE_BUILTIN 0	/* compiled-in default */
#define WS_SETTING_SOURCE_SYSTEM  1	/* the system plist, WS_<Key> or the
					   command line */
#define WS_SETTING_SOURCE_SESSION 2	/* the console user's session */

/* ws_event_setting_t.flags */
#define WS_SETTING_SETTABLE	0x0001	/* WS_MSG_SET_SETTINGS may change it;
					   others are the server's own */
#define WS_SETTING_TRUNCATED	0x0002	/* value didn't fit */

#define WS_MAX_SETTING_KEY	32
#define WS_MAX_SETTING_VALUE	176

typedef struct ws_event_setting {
	char		key[WS_MAX_SETTING_KEY];
	uint32_t	type;		/* WS_SETTING_* */
	uint32_t	source;		/* WS_SETTING_SOURCE_* */
	uint32_t	flags;		/* WS_SETTING_SETTABLE, _TRUNCATED */
	uint32_t	reserved;
	char		value[WS_MAX_SETTING_VALUE];
} ws_event_setting_t;

/*
 * Keyboard layouts (WS_SERVER_SETTINGS): the KeyboardLayout setting is
 * a list ("us,gr"), at most WS_MAX_KEYBOARD_LAYOUTS (XKB's group limit).
 * WS_EVENT_KEYBOARD_LAYOUT names one of them: the active one when it
 * says the layout changed, each in turn when it answers
 * WS_MSG_LIST_KEYBOARD_LAYOUTS.
 */
#define WS_MAX_KEYBOARD_LAYOUTS	4

#define WS_KEYBOARD_LAYOUT_ACTIVE 0x0001	/* this is the active layout */
#define WS_KEYBOARD_LAYOUT_CHANGED 0x0002	/* says the active layout changed
						   (not part of a list) */

typedef struct ws_event_keyboard_layout {
	uint32_t	index;		/* 0-based, into the layout list */
	uint32_t	count;		/* layouts in the list */
	uint32_t	active;		/* index of the active layout */
	uint32_t	flags;		/* WS_KEYBOARD_LAYOUT_ACTIVE */
	char		layout[32];	/* the XKB layout, e.g. "gr" */
	char		variant[32];	/* its variant, "" = the default */
	char		name[64];	/* XKB's description, e.g. "Greek" */
} ws_event_keyboard_layout_t;

/* Input devices (WS_MSG_LIST_INPUT_DEVICES), ws_event_input_device_t.flags. */
#define WS_INPUT_KEYBOARD	0x0001
#define WS_INPUT_POINTER	0x0002	/* any pointing device */
#define WS_INPUT_TOUCHPAD	0x0004	/* a pointer that taps: a touchpad */
#define WS_INPUT_MOUSE		0x0008	/* a pointer that isn't a touchpad */
#define WS_INPUT_TOUCH		0x0010	/* a touchscreen */

typedef struct ws_event_input_device {
	uint32_t	flags;		/* WS_INPUT_* */
	uint32_t	reserved;
	char		name[64];
	char		sysname[32];	/* e.g. "event3" */
} ws_event_input_device_t;

/*
 * Window tiling (servers with WS_SERVER_TILING; see "Window tiling" in
 * WSProtocol.h). WS_MSG_TILING is answered with WS_EVENT_TILING_STATE,
 * a WS_EVENT_TILING_ITEM per tiled window (in layout order) and per
 * window that floats in tiling mode only because the user floated it or
 * it wouldn't fit its tile, then WS_EVENT_LIST_END.
 */
#define WS_TILING_STACKING	0	/* ws_event_tiling_t.mode */
#define WS_TILING_TILING	1

#define WS_TILING_DWINDLE	0	/* ws_event_tiling_t.layout */
#define WS_TILING_MASTER	1	/* master and stack */

/* WS_MSG_TILING operations (ws_req_tiling_t.op) */
#define WS_TILING_QUERY		0	/* state and items, nothing changes */
#define WS_TILING_FOCUS		1	/* arg WS_TILING_DIR_*: focus the
					   neighbour tile */
#define WS_TILING_SWAP		2	/* arg WS_TILING_DIR_*: swap places
					   with the neighbour tile */
#define WS_TILING_RESIZE	3	/* arg > 0 grows the tile, < 0 shrinks
					   it, by 5% of the area per step */
#define WS_TILING_FLOAT		4	/* arg 1 floats the window, 0 tiles it
					   again, 2 toggles */

#define WS_TILING_DIR_LEFT	1
#define WS_TILING_DIR_RIGHT	2
#define WS_TILING_DIR_UP	3
#define WS_TILING_DIR_DOWN	4

typedef struct ws_event_tiling {
	uint32_t	mode;		/* WS_TILING_STACKING or _TILING */
	uint32_t	layout;		/* WS_TILING_DWINDLE or _MASTER */
	uint32_t	count;		/* windows tiled */
	uint32_t	focused;	/* the tiled window that has the focus,
					   0 = none */
	ws_rect_t	area;		/* where tiles go: the usable area
					   minus the outer gaps */
	uint32_t	gaps_inner;	/* pixels between tiles */
	uint32_t	gaps_outer;	/* pixels around them */
	uint32_t	border;		/* active-window border, pixels */
	uint32_t	modifier;	/* WS_MOD_* the tiling keys take */
} ws_event_tiling_t;

#define WS_TILING_ITEM_TILED	0x0001
#define WS_TILING_ITEM_FLOATED	0x0002	/* the user floated it */
#define WS_TILING_ITEM_CUT	0x0004	/* tiled, but its owner keeps asking
					   for a larger frame: cut to its
					   tile */
#define WS_TILING_ITEM_FOCUSED	0x0008

typedef struct ws_event_tiling_item {
	uint32_t	window;		/* as in hdr.window */
	uint32_t	app_id;
	uint32_t	flags;		/* WS_TILING_ITEM_* */
	uint32_t	index;		/* place in the layout, 0 first */
	ws_rect_t	tile;		/* its tile (tiled windows) */
	ws_rect_t	frame;		/* its frame */
} ws_event_tiling_item_t;

/*
 * Drag and drop (servers with WS_SERVER_DRAG; see "Drag and drop" in
 * WSProtocol.h). Operations are the NSDragOperation values, so the
 * GNUstep backend passes them through unchanged.
 */
#define WS_DRAG_OP_NONE		0
#define WS_DRAG_OP_COPY		0x0001
#define WS_DRAG_OP_LINK		0x0002
#define WS_DRAG_OP_GENERIC	0x0004
#define WS_DRAG_OP_PRIVATE	0x0008
#define WS_DRAG_OP_MOVE		0x0010
#define WS_DRAG_OP_DELETE	0x0020
#define WS_DRAG_OP_EVERY	0xffffffffu

/* WS_MSG_DRAG_BEGIN flags (ws_drag_params_t.flags) */
#define WS_DRAG_SLIDE_BACK	0x0001	/* a drag nobody takes slides its
					   image back to where it started */
#define WS_DRAG_IGNORE_MODIFIERS 0x0002	/* Control, Option and Command don't
					   narrow the operations */

/* ws_event_drag_t.phase */
#define WS_DRAG_ENTER		1	/* the drag came over the window */
#define WS_DRAG_UPDATE		2	/* it moved, or its operations changed */
#define WS_DRAG_EXIT		3	/* it left, or ended without a drop */
#define WS_DRAG_DROP		4	/* it was dropped: read the pasteboard */

/* WS_MSG_DRAG_STATUS flags */
#define WS_DRAG_STATUS_FINISHED	0x0001	/* the drop is handled (informational) */

/* ws_event_drag_end_t.flags */
#define WS_DRAG_END_DROPPED	0x0001	/* dropped on a window that took it */
#define WS_DRAG_END_CANCELLED	0x0002	/* Escape, WS_MSG_DRAG_CANCEL, or the
					   window under it went away */
#define WS_DRAG_END_SLID_BACK	0x0004	/* the image slid back first */

#define WS_MAX_DRAG_IMAGE	512	/* pixels on a side */
#define WS_MAX_DRAG_PASTEBOARD	64	/* bytes, NUL included */
#define WS_MAX_DRAG_TYPES	512	/* bytes of NUL-separated type names */

/*
 * WS_EVENT_DRAG (about hdr.window, a window of this connection with
 * WS_WINDOW_ACCEPTS_DRAGS): a drag came over it, moved over it, left
 * it or was dropped on it. Answer ENTER and UPDATE with WS_MSG_DRAG_STATUS
 * naming the operation the window would take, WS_DRAG_OP_NONE if none.
 */
typedef struct ws_event_drag {
	uint32_t	session;	/* the drag, for WS_MSG_DRAG_STATUS */
	uint32_t	phase;		/* WS_DRAG_* */
	double		x;		/* window-local pointer position */
	double		y;
	double		screen_x;
	double		screen_y;
	uint32_t	operations;	/* WS_DRAG_OP_* the source allows here,
					   narrowed by the modifiers held */
	uint32_t	modifiers;	/* WS_MOD_* */
	uint32_t	source_client;	/* the source's connection */
	uint32_t	source_app;	/* and its app */
	char		pasteboard[WS_MAX_DRAG_PASTEBOARD]; /* where the data
					   is, "NSDragPboard" for GNUstep */
} ws_event_drag_t;

/*
 * WS_EVENT_DRAG_STATUS: to the source, whenever the window under the
 * drag or the operation it takes changes. The server shows the cursor
 * for it itself.
 */
typedef struct ws_event_drag_status {
	uint32_t	session;
	uint32_t	target;		/* window under the drag that takes
					   drags, 0 = none */
	uint32_t	target_app;	/* its app's id */
	uint32_t	operation;	/* WS_DRAG_OP_* it takes, of operations */
	uint32_t	operations;	/* what the source offers it now */
	uint32_t	reserved;
	double		screen_x;
	double		screen_y;
} ws_event_drag_status_t;

/*
 * WS_EVENT_DRAG_END: to the source, once: the drag is over. A drop
 * comes with the operation the window took (then the window has been
 * sent WS_DRAG_DROP), anything else with WS_DRAG_OP_NONE, after the
 * image slid back if the drag asked for that.
 */
typedef struct ws_event_drag_end {
	uint32_t	session;
	uint32_t	operation;	/* WS_DRAG_OP_* taken, 0 = none */
	uint32_t	flags;		/* WS_DRAG_END_* */
	uint32_t	target;		/* the window it was dropped on, or 0 */
	double		screen_x;	/* pointer at the end */
	double		screen_y;
} ws_event_drag_end_t;

/* WS_EVENT_DRAG_STATE (WS_MSG_DRAG_QUERY): the drag under way, if any. */
#define WS_DRAG_STATE_RELEASED	0x0001	/* button up, waiting for the target */
#define WS_DRAG_STATE_SLIDING	0x0002	/* the image is sliding back */

typedef struct ws_event_drag_state {
	uint32_t	session;	/* 0 = no drag */
	uint32_t	source_client;
	uint32_t	target;		/* window under it that takes drags */
	uint32_t	operation;	/* what that window takes */
	uint32_t	operations;	/* what the source offers it */
	uint32_t	flags;		/* WS_DRAG_STATE_* */
	ws_rect_t	image;		/* where the drag image is, 0x0 = none */
	uint32_t	sessions;	/* drags since the server started */
	uint32_t	reserved;
	char		types[152];	/* the types offered, comma-separated
					   (cut short if long) */
} ws_event_drag_state_t;

typedef struct ws_event {
	ws_event_header_t hdr;
	union {
		ws_event_welcome_t	welcome;
		ws_screen_info_t	screen;
		ws_event_app_t		app;
		ws_event_frame_t	frame;
		ws_event_buffer_t	buffer;
		ws_event_pointer_t	pointer;
		ws_event_scroll_t	scroll;
		ws_event_key_t		key;
		ws_event_flags_t	flags;
		ws_event_error_t	error;
		ws_event_snapshot_t	snapshot;
		ws_event_window_info_t	window_info;
		ws_event_keymap_t	keymap;
		ws_event_modifiers_t	modifiers;
		ws_event_keys_held_t	keys_held;
		ws_event_ordered_t	ordered;
		ws_event_dock_state_t	dock_state;
		ws_event_snapshot_t	preview;	/* WS_EVENT_PREVIEW */
		ws_event_overview_t	overview;
		ws_event_overview_item_t overview_item;
		ws_event_usable_area_t	usable_area;
		ws_event_setting_t	setting;
		ws_event_keyboard_layout_t keyboard_layout;
		ws_event_input_device_t	input_device;
		ws_event_tiling_t	tiling;
		ws_event_tiling_item_t	tiling_item;
		ws_event_drag_t		drag;
		ws_event_drag_status_t	drag_status;
		ws_event_drag_end_t	drag_end;
		ws_event_drag_state_t	drag_state;
		uint8_t			raw[232];
	} u;
} ws_event_t;

#ifdef __cplusplus
}
#endif

#endif /* !_WINDOWSERVER_WSTYPES_H_ */
