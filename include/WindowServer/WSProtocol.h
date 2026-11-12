/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * WSProtocol.h - WindowServer wire protocol, version WS_PROTOCOL_VERSION.
 *
 * Applications normally use libWindowServerClient (WSClient.h), which
 * hides everything in this file. It is public so the protocol is
 * documented in one place and other implementations can follow it.
 *
 * Two channels per client:
 *
 *  1. Mach (control). The client looks up WS_SERVICE_NAME through the
 *     bootstrap server and sends requests to it. Every request starts
 *     with ws_req_header_t. Requests that need an answer carry a
 *     send-once reply port in msgh_local_port (MACH_MSG_TYPE_MAKE_SEND_ONCE)
 *     and get a ws_reply_t back; all other requests are one-way. The
 *     server reads the sender's pid from the Mach audit trailer.
 *
 *  2. A Unix domain stream socket. NextBSD's Mach can't carry file
 *     descriptors or shared memory yet, so window buffers (memfd)
 *     travel here with SCM_RIGHTS, and the server sends its events
 *     (ws_event_t, WSTypes.h) here, which gives clients a plain file
 *     descriptor to poll. Every message starts with its own size
 *     (ws_sock_header_t.size, ws_event_header_t.size), so readers split
 *     the byte stream themselves. (SOCK_SEQPACKET would do that for us,
 *     but on NextBSD it doesn't keep message boundaries.) A message
 *     that carries a descriptor sends it with its first byte; readers
 *     queue received descriptors and take them in order.
 *
 * Connection setup:
 *   - Mach WS_MSG_HELLO -> reply with client_id, a random 64-bit token
 *     and the socket path.
 *   - connect() to the socket path and send WS_SOCK_AUTH {client_id,
 *     token}. The server also checks the peer pid (LOCAL_PEERCRED)
 *     against the pid from the HELLO audit trailer.
 *   - The server answers with WS_EVENT_WELCOME on the socket.
 *
 * Ordering rule: before the server handles a Mach request from a
 * client, it reads everything already queued on that client's socket.
 * A client therefore sends WS_SOCK_BUFFER_CREATE on the socket and can
 * then immediately refer to the buffer in a Mach request.
 */
#ifndef _WINDOWSERVER_WSPROTOCOL_H_
#define _WINDOWSERVER_WSPROTOCOL_H_

#include <stddef.h>
#include <stdint.h>
#include <mach/message.h>
#include <WindowServer/WSTypes.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mach message ids ('WS' << 16 | n). */
#define WS_MSG_BASE		0x57530000
enum {
	WS_MSG_HELLO = WS_MSG_BASE + 1,	/* sync */
	WS_MSG_GOODBYE,
	WS_MSG_SYNC,			/* sync: round trip */
	WS_MSG_WINDOW_CREATE,		/* sync */
	WS_MSG_WINDOW_DESTROY,
	WS_MSG_WINDOW_SET_FRAME,
	WS_MSG_WINDOW_SET_LEVEL,
	WS_MSG_WINDOW_ORDER,
	WS_MSG_WINDOW_SET_TITLE,
	WS_MSG_WINDOW_SET_FLAGS,
	WS_MSG_WINDOW_ATTACH,
	WS_MSG_WINDOW_DAMAGE,
	WS_MSG_WINDOW_COMMIT,
	WS_MSG_WINDOW_MAKE_KEY,
	WS_MSG_BUFFER_DESTROY,
	WS_MSG_SET_CURSOR,
	WS_MSG_CURSOR_VISIBLE,
	WS_MSG_ACTIVATE,
	WS_MSG_DEACTIVATE,
	WS_MSG_SNAPSHOT,		/* sync; image follows on the socket */
	WS_MSG_LIST_WINDOWS,		/* answers on the socket */
	WS_MSG_INJECT,			/* synthetic input (see below) */
	/* added after protocol 1 shipped (servers with WS_SERVER_MINIMIZE) */
	WS_MSG_WINDOW_MINIMIZE,		/* see "Minimizing" below; header,
					   or ws_req_int_t: WS_MINIMIZE_* */
	WS_MSG_SET_APP_ICON,		/* ws_req_int_t: buffer id, 0 = none */
	/* WS_CONNECT_DOCK only (servers with WS_SERVER_DOCK), see "The Dock" */
	WS_MSG_DOCK_ACTIVATE,		/* ws_req_int_t: app id, or hdr.window */
	WS_MSG_DOCK_RESTORE,		/* hdr.window */
	WS_MSG_DOCK_PREVIEW,		/* sync, ws_req_int_t: largest side;
					   image follows on the socket */
	WS_MSG_DOCK_SHOW_MINIMIZED,	/* ws_req_int_t: 1 = this Dock shows
					   minimized windows, 0 = no longer */
	/* tools and the Dock (servers with WS_SERVER_OVERVIEW) */
	WS_MSG_OVERVIEW,		/* ws_req_int_t: WS_OVERVIEW_* */
	/* WS_CONNECT_DOCK only (servers with WS_SERVER_DOCK_EDGE) */
	WS_MSG_DOCK_SET_EDGE,		/* ws_req_dock_edge_t */
	/* servers with WS_SERVER_SETTINGS, see "Settings" */
	WS_MSG_SET_SETTINGS,		/* sync, ws_req_settings_t */
	WS_MSG_GET_SETTINGS,		/* answers on the socket */
	WS_MSG_SET_KEYBOARD_LAYOUT,	/* sync, ws_req_int_t: layout index */
	WS_MSG_LIST_KEYBOARD_LAYOUTS,	/* answers on the socket */
	WS_MSG_LIST_INPUT_DEVICES,	/* answers on the socket */
	WS_MSG_LAST
};

#define WS_MAX_DAMAGE_RECTS	16	/* more: send several messages */
#define WS_MAX_TITLE		256
#define WS_MAX_APP_NAME		64
#define WS_MAX_SOCKET_PATH	104	/* sizeof(sun_path) */

typedef struct ws_req_header {
	mach_msg_header_t head;
	uint32_t	version;	/* WS_PROTOCOL_VERSION */
	uint32_t	client_id;	/* 0 for HELLO */
	uint64_t	token;		/* from the HELLO reply */
	uint32_t	seq;		/* echoed in the reply */
	uint32_t	window;		/* target window, if any */
} ws_req_header_t;

/*
 * Joining an app (WS_CONNECT_JOIN). Normally every connection is an app
 * of its own. A connection that sets WS_CONNECT_JOIN and names another
 * connection's client id and token in join_client and join_token
 * becomes part of that connection's app instead (a helper process such
 * as an X server started by XLibre.app). All connections of an app are
 * activated and deactivated together and share one key window, which
 * may belong to any of them; key events go to the connection that owns
 * the key window. Only a process that was given the token can join.
 * A wrong id or token fails the hello with EPERM. If the app's first
 * connection goes away, the oldest remaining one carries the app on.
 *
 * join_client and join_token were added after protocol 1 shipped:
 * older clients send a hello without them, which servers accept, and
 * older servers ignore them and leave the reply's app_id 0, which the
 * client library reports as ENOTSUP.
 */
/*
 * Command shortcuts (WS_SERVER_LATIN_SHORTCUTS). Key events carry the
 * keysym and text of the active layout, except that while the active
 * layout isn't Latin (gr, ru...), a key pressed with Command held
 * carries the keysym, text and text_unmodified of the first Latin
 * layout in the KeyboardLayout list, or of us when there is none, as
 * on macOS: Command+C copies while Greek is active. The modifiers in
 * effect still apply (Command+Shift+Z gives "Z"). Keys without Command,
 * with Option or Control alone, and every key of a WS_CONNECT_XKB
 * connection (which reads keycodes with its own keymap) keep the active
 * layout. Older servers send the active layout's in every case.
 */
typedef struct ws_req_hello {
	ws_req_header_t	hdr;
	uint32_t	flags;		/* WS_CONNECT_* */
	uint32_t	reserved;
	char		app_name[WS_MAX_APP_NAME];
	char		bundle_id[128];
	/* optional tail */
	uint32_t	join_client;	/* WS_CONNECT_JOIN: client id */
	uint32_t	reserved2;
	uint64_t	join_token;	/* WS_CONNECT_JOIN: its token */
} ws_req_hello_t;

/* A hello from before the optional tail. */
#define WS_HELLO_MIN_SIZE	offsetof(ws_req_hello_t, join_client)

typedef struct ws_req_window_create {
	ws_req_header_t	hdr;
	ws_rect_t	frame;
	int32_t		level;
	uint32_t	flags;		/* WS_WINDOW_* */
	char		title[128];
} ws_req_window_create_t;

typedef struct ws_req_frame {
	ws_req_header_t	hdr;
	ws_rect_t	frame;
} ws_req_frame_t;

typedef struct ws_req_int {		/* SET_LEVEL, SET_FLAGS, COMMIT,
					 * ATTACH, BUFFER_DESTROY,
					 * CURSOR_VISIBLE, ACTIVATE,
					 * SNAPSHOT */
	ws_req_header_t	hdr;
	int32_t		value;
	uint32_t	reserved;
} ws_req_int_t;

typedef struct ws_req_dock_edge {	/* DOCK_SET_EDGE */
	ws_req_header_t	hdr;
	uint32_t	edge;		/* WS_DOCK_EDGE_* */
	uint32_t	thickness;	/* pixels, at most half the screen */
} ws_req_dock_edge_t;

typedef struct ws_req_order {
	ws_req_header_t	hdr;
	int32_t		op;		/* WS_ORDER_* */
	uint32_t	relative_to;	/* window id, 0 = whole level */
} ws_req_order_t;

typedef struct ws_req_title {
	ws_req_header_t	hdr;
	char		title[WS_MAX_TITLE];
} ws_req_title_t;

typedef struct ws_req_damage {
	ws_req_header_t	hdr;
	uint32_t	count;
	uint32_t	reserved;
	ws_rect_t	rects[WS_MAX_DAMAGE_RECTS];	/* window-local */
} ws_req_damage_t;

typedef struct ws_req_cursor {
	ws_req_header_t	hdr;
	uint32_t	kind;		/* WS_CURSOR_* */
	uint32_t	buffer_id;	/* for WS_CURSOR_CUSTOM */
	int32_t		hot_x;
	int32_t		hot_y;
} ws_req_cursor_t;

/*
 * Minimizing (servers with WS_SERVER_MINIMIZE). WS_MSG_WINDOW_MINIMIZE
 * (a bare ws_req_header_t naming the window) orders the window out and
 * puts a tile for it in the bottom row of the screen instead: a live,
 * scaled-down picture of the window's buffer, redrawn on every commit,
 * with the app's icon in a corner. The window stays minimized until its
 * owner orders it in again (WS_ORDER_ABOVE or _BELOW), which removes the
 * tile and shows the window, or orders it out, which only removes the
 * tile, or destroys it. A double-click on the tile activates the app
 * and sends the owner WS_EVENT_WINDOW_RESTORE; the owner then orders
 * the window in.
 *
 * Where the window goes (servers with WS_SERVER_MINIMIZE_WHERE): the
 * request may instead be a ws_req_int_t whose value is WS_MINIMIZE_*
 * (WSTypes.h), so that each app's minimize follows its user's setting
 * (NextBSD's NBMinimizeStyle). WS_MINIMIZE_INTO_DOCK hands the window to the Dock
 * (see "The Dock" below) while a Dock that shows minimized windows is
 * connected, and gives it a tile on the bottom row otherwise; it moves
 * into the Dock when one connects and back to the bottom row when the
 * Dock goes. WS_MINIMIZE_BOTTOM_ROW always gives it a tile, even with
 * a Dock connected. WS_MINIMIZE_DEFAULT, a bare header and an unknown
 * value all mean what the server's MinimizeIntoDock default says: into
 * the Dock if it's on, the bottom row if not. Minimizing a window
 * that's already minimized moves it to where the new request says.
 * Older servers ignore the value, so check for the flag first.
 *
 * WS_MSG_SET_APP_ICON (ws_req_int_t) gives the connection's app icon
 * for the tiles: `value` names a buffer the client sent before
 * (premultiplied ARGB8888, at most WS_MAX_APP_ICON pixels square),
 * which the server copies, so the client may destroy it right away.
 * 0 removes the icon. Tiles of a connection that has none use the icon
 * of the app it joined, if any.
 */
#define WS_MAX_APP_ICON		512

/*
 * The Dock (servers with WS_SERVER_DOCK). Apps see only their own
 * windows. The Dock needs every app's, the way Darwin's Dock uses
 * CoreGraphics' private window list, so it connects with
 * WS_CONNECT_DOCK. A Dock connection is a tool connection like
 * WS_CONNECT_CONTROL, not an app: it has no windows and is never
 * activated (the Dock's own windows come from its ordinary app
 * connection, its GNUstep backend's).
 *
 * Who gets it: the hello fails with EPERM unless the sender (its euid
 * from the Mach audit trailer) is root, the server's own user, or the
 * console user, i.e. the user of a loginwindow session: a root-owned
 * /var/run/loginwindow/<uid>.session record. When no loginwindow
 * session exists (a server started by hand with ws-session) and on a
 * headless server, any user is allowed, as for the rest of the
 * protocol. WS_CONNECT_DOCK can't be combined with WS_CONNECT_CONTROL
 * or WS_CONNECT_JOIN (EINVAL). The same check guards WS_MSG_LIST_WINDOWS,
 * which only WS_CONNECT_CONTROL and WS_CONNECT_DOCK connections may send
 * (others get a WS_EVENT_ERROR with EPERM, then WS_EVENT_LIST_END).
 *
 * The trust boundary is the user: the list carries other apps' window
 * titles, and those apps all belong to the console user, so another
 * user (an SSH login on the same machine) can't read them. The console
 * user's own processes are trusted, as they are on macOS (where any
 * process of the user could read CGWindowListCopyWindowInfo's titles
 * before 10.15); WindowServer doesn't tell the Dock binary apart from
 * other programs of the same user.
 *
 * What a Dock connection gets and may send:
 *   - WS_MSG_LIST_WINDOWS, optionally as a ws_req_int_t whose value is
 *     WS_LIST_* (WS_LIST_ORDERED_OUT: windows that aren't shown too):
 *     a WS_EVENT_WINDOW_INFO per window, then WS_EVENT_LIST_END.
 *   - WS_EVENT_WINDOW_LIST_CHANGED whenever the list changed: a window
 *     was created, destroyed, ordered in or out, restacked, minimized,
 *     restored, retitled or got a new level or flags, the active app or
 *     a key window changed, or a client went away. At most one per pass
 *     of the server's event loop, so the Dock lists again rather than
 *     polling. A frame change alone sends none (dragging a window would
 *     flood the Dock); the list has the frames current when it's sent.
 *   - WS_MSG_DOCK_ACTIVATE (ws_req_int_t): with hdr.window, activates
 *     that window's app with the window as its key window, raising it,
 *     or, if it's minimized, asking its owner to restore it as below;
 *     with hdr.window 0, activates the app `value` names (an app id or
 *     any client id of the app). Errors come back as WS_EVENT_ERROR.
 *   - WS_MSG_DOCK_RESTORE (hdr.window, a minimized window): activates
 *     the app and sends the window's owner WS_EVENT_WINDOW_RESTORE, as
 *     a double-click on its tile does. The app does the restoring.
 *   - WS_MSG_DOCK_PREVIEW (sync, ws_req_int_t, hdr.window): the reply
 *     carries the status (ENOENT: no such window or nothing drawn yet);
 *     then WS_EVENT_PREVIEW (ws_event_snapshot_t, about hdr.window)
 *     arrives on the socket with a memfd holding the window's picture,
 *     as much of its buffer as its frame shows, scaled down so neither
 *     side is longer than `value` pixels (16 to WS_MAX_PREVIEW; 0 means
 *     WS_MAX_PREVIEW) and never enlarged, as premultiplied ARGB8888.
 *     It has no tile or icon: the Dock draws those.
 *   - WS_MSG_DOCK_SHOW_MINIMIZED (ws_req_int_t, value 1): this Dock can
 *     show minimized windows (a Dock that only reads the list, such as
 *     `wsctl dock list`, doesn't send it). "Minimize into the Dock" is
 *     chosen per window: WS_MINIMIZE_INTO_DOCK on the minimize request
 *     (see "Minimizing" above), or, for a request that doesn't say,
 *     the WindowServer default MinimizeIntoDock (off by default). The
 *     oldest Dock connection that sent this gets
 *     WS_DOCK_MINIMIZED_WINDOWS in WS_EVENT_DOCK_STATE: WindowServer
 *     draws no tiles for windows minimized into the Dock, takes away
 *     the ones it drew for them, and lists them with
 *     WS_WINDOW_INFO_IN_DOCK; the Dock shows them with previews and
 *     restores them with WS_MSG_DOCK_RESTORE. Windows minimized onto
 *     the bottom row keep their tiles and aren't the Dock's to show.
 *     Each time a window in the Dock commits a frame, the Dock gets
 *     WS_EVENT_PREVIEW_CHANGED for it (at most one per window per pass).
 *     When that Dock goes away or sends value 0, the next such Dock
 *     connection takes over, or else the server puts tiles back for the
 *     windows still minimized.
 *   - WS_MSG_DOCK_SET_EDGE (ws_req_dock_edge_t, servers with
 *     WS_SERVER_DOCK_EDGE): the screen edge this Dock sits on
 *     (WS_DOCK_EDGE_*) and how many pixels it takes from it, measured
 *     from the edge. That strip is left out of the usable area (see
 *     "The screen and its usable area"), and window tiling will keep
 *     it free. The newest
 *     report from a Dock connection still connected counts; when that
 *     Dock goes away or reports WS_DOCK_EDGE_NONE, the newest other
 *     Dock's report does, or none. An edge above WS_DOCK_EDGE_BOTTOM or
 *     a thickness over half the screen fails with EINVAL.
 *   - WS_EVENT_DOCK_STATE (WSTypes.h) after the welcome and whenever
 *     its flags or the Dock's edge change.
 */
#define WS_MAX_PREVIEW		512

/*
 * The overview (servers with WS_SERVER_OVERVIEW). WindowServer opens it
 * itself: a tap of the Start (Command) key on its own, Control+Up, the
 * Mission Control key (KEY_SCALE) or a three-finger swipe up show every
 * window; Control+Down or a three-finger swipe down show the active
 * app's (App Exposé). (With ThreeFingerDrag on, the swipes take four
 * fingers.) While it's open no app gets pointer or key events:
 * clicking a window or pressing Return on the selected one activates
 * its app with that window key and in front (a minimized one is
 * restored as from its tile), and Escape, a click on empty space or
 * another Start tap closes it. Apps don't change.
 *
 * WS_MSG_OVERVIEW (ws_req_int_t), from WS_CONNECT_CONTROL and
 * WS_CONNECT_DOCK connections only, with the same user check as the
 * window list: `value` WS_OVERVIEW_ALL or _APP opens it (or switches
 * mode), WS_OVERVIEW_CLOSE closes it, and every value, WS_OVERVIEW_QUERY
 * included, is answered on the socket with WS_EVENT_OVERVIEW_STATE, a
 * WS_EVENT_OVERVIEW_ITEM for each window shown, and WS_EVENT_LIST_END.
 * Dock connections also get WS_EVENT_OVERVIEW_STATE whenever it starts
 * opening and when it has closed.
 */

/*
 * The screen and its usable area.
 *
 * Usable area (servers with WS_SERVER_USABLE_AREA): the welcome carries
 * it after the screen (ws_event_welcome_t.usable), and every connection
 * gets WS_EVENT_USABLE_AREA (ws_event_usable_area_t) whenever it
 * changes. It is the screen minus the strip of the edge the Dock keeps
 * (WS_MSG_DOCK_SET_EDGE, see "The Dock"), the screen itself while no
 * Dock has said; later it will also leave out a menu bar strip along
 * the top. Apps use it as NSScreen's visibleFrame. WindowServer puts
 * its own things inside it: minimized-window tiles go along its bottom
 * edge from its left edge (just above a bottom Dock, right of a left
 * one), and are placed again when it changes; tiles the user dragged
 * stay, moved inside it only if they'd be outside. The overview lays
 * the windows out inside it, and a window minimized into the Dock
 * comes from the Dock's edge.
 *
 * Screen changes (servers with WS_SERVER_SCREEN_CHANGES): when the
 * screen's size or mode changes, every connection gets
 * WS_EVENT_SCREEN_CHANGED with the new ws_screen_info_t, then
 * WS_EVENT_USABLE_AREA. Window frames keep their screen coordinates
 * (top-left origin), except that a window that was on the screen and
 * no longer fits is moved back onto it, and one that was partly off is
 * moved so that at least its top-left part shows; their owners get
 * WS_EVENT_WINDOW_FRAME, after the two events above. Tiles are placed
 * again. A headless server changes size on WS_INJECT_SCREEN_SIZE
 * (`wsctl screen size WxH`), for tests; a DRM server has a single mode
 * for now, so it sends the event once mode changes come (hotplug,
 * Displays).
 */

/*
 * Settings (servers with WS_SERVER_SETTINGS). WindowServer's settings
 * (README, "Settings") change while it runs, in layers:
 *
 *   1. the system's: built-in defaults, the system plist
 *      (/System/Library/Preferences/org.nextbsd.WindowServer.plist),
 *      WS_<Key> environment variables and the command line, read at
 *      start and again on SIGHUP or WS_SETTINGS_RELOAD_SYSTEM;
 *   2. the console user's session: values sent with WS_MSG_SET_SETTINGS,
 *      normally the user's org.nextbsd.WindowServer defaults, sent once
 *      at login (wsctl settings reload, from loginwindow's session
 *      script), then each change the settings apps make.
 *
 * The session's values are dropped when the console user changes (a
 * loginwindow session ends or another one starts), so the login panel
 * always has the system's settings.
 *
 * WS_MSG_SET_SETTINGS (sync, ws_req_settings_t) changes the session's
 * values. Who may: the same check as the Dock role (see "The Dock"):
 * root, the server's own user, or the console user; anyone on a
 * headless server or when nobody is logged in through loginwindow.
 * Others get EPERM. `data` holds `length` bytes of entries, each
 *
 *	one type byte (WS_SETTING_*), the key, NUL, the value as text, NUL
 *
 * using the key names of the system plist. A value of the key's own type
 * fits, an integer fits a real, and a string fits any key and is parsed
 * (as `defaults write` stores strings). WS_SETTING_DEFAULT drops the
 * session's value of the key, so the system's applies again. The
 * request is checked as a whole before anything changes: a key only
 * the server may set (DRMDevice, User, Group, SocketPath,
 * AllowTerminate... WS_SETTING_SETTABLE in the answer to
 * WS_MSG_GET_SETTINGS says which aren't) fails it with EPERM; an
 * unknown key, a value of the wrong type or out of range, or a keyboard
 * layout XKB can't compile with EINVAL. The reply then names the key
 * in u.settings.key. Flags:
 *   WS_SETTINGS_REPLACE	drop every session value first: the entries
 *				are the whole set (a login, a reload)
 *   WS_SETTINGS_RELOAD_SYSTEM	read the system's layer again first
 *   WS_SETTINGS_SKIP_INVALID	leave out entries that would fail instead
 *				of failing (a user's defaults may hold keys
 *				this server doesn't take); the reply still
 *				names the first one left out, with status 0
 * Larger sets go in several requests; only the first says REPLACE.
 *
 * What a change does:
 *   - ScrollSpeed, NaturalScrolling, MomentumScrolling, ClickThrough,
 *     RaiseOnClick, ReduceMotion, StartKeyShowsOverview: used from the
 *     next event on.
 *   - PointerSpeed, TrackpadSpeed, MouseSpeed, MouseAccelProfile,
 *     LeftHanded, TapToClick, TapToDrag, ThreeFingerDrag,
 *     SecondaryClick: applied again to every input device.
 *   - KeyboardRules, KeyboardModel, KeyboardLayout, KeyboardVariant,
 *     KeyboardOptions, KeyboardLayoutShortcut: the keymap is compiled
 *     again. Key events of every connection carry keysyms and text from
 *     the new keymap. WS_CONNECT_XKB connections get WS_EVENT_KEYMAP
 *     again, then WS_EVENT_MODIFIERS with the new state. Keys
 *     held stay held, Caps Lock and Num Lock stay locked, and the
 *     active layout stays active if it's still in the list.
 *   - KeyRepeatDelay, KeyRepeatInterval: WS_CONNECT_XKB connections
 *     get WS_EVENT_KEYMAP again, which carries them.
 * Tool and Dock connections get WS_EVENT_SETTINGS_CHANGED after any
 * change in effect, from any layer.
 *
 * WS_MSG_GET_SETTINGS (any connection): a WS_EVENT_SETTING for every
 * key with the value in effect and where it comes from, then
 * WS_EVENT_LIST_END.
 *
 * Keyboard layouts. KeyboardLayout is a list ("us,gr", variants to
 * match in KeyboardVariant, ",polytonic"), at most
 * WS_MAX_KEYBOARD_LAYOUTS. KeyboardLayoutShortcut picks the key that
 * switches to the next one: None (the default), ControlSpace, AltShift,
 * WinSpace or CapsLock; a grp: option in KeyboardOptions works too.
 * WS_MSG_SET_KEYBOARD_LAYOUT (sync, ws_req_int_t, same check as
 * WS_MSG_SET_SETTINGS) makes layout `value` active (EINVAL past the
 * end). WS_MSG_LIST_KEYBOARD_LAYOUTS (any connection) answers with a
 * WS_EVENT_KEYBOARD_LAYOUT per layout, then WS_EVENT_LIST_END. Tool and
 * Dock connections (the Dock's Input menu) get a WS_EVENT_KEYBOARD_LAYOUT
 * naming the active layout whenever it changes: the request, the
 * shortcut, or a new layout list.
 *
 * WS_MSG_LIST_INPUT_DEVICES (any connection): a WS_EVENT_INPUT_DEVICE
 * per input device, then WS_EVENT_LIST_END, so Trackpad and Mouse
 * settings can say there is none. A headless server has none.
 */
#define WS_MAX_SETTINGS_DATA	896

/* WS_SETTINGS_* flags are in WSTypes.h. */

typedef struct ws_req_settings {	/* SET_SETTINGS */
	ws_req_header_t	hdr;
	uint32_t	flags;		/* WS_SETTINGS_* */
	uint32_t	length;		/* bytes used in data */
	char		data[WS_MAX_SETTINGS_DATA];
} ws_req_settings_t;

/*
 * Synthetic input. Accepted when the server runs headless, or from a
 * process running as root or as the server's own user.
 */
typedef struct ws_req_inject {
	ws_req_header_t	hdr;
	uint32_t	kind;		/* WS_INJECT_* */
	uint32_t	pressed;
	double		x;
	double		y;
	double		dx;
	double		dy;
	uint32_t	button;
	uint32_t	keycode;	/* evdev code, used if key_name is "" */
	char		key_name[32];	/* XKB keysym name, e.g. "a",
					 * "Return", "shift+a", "ctrl+q" */
} ws_req_inject_t;

typedef struct ws_reply {
	mach_msg_header_t head;
	uint32_t	version;
	uint32_t	seq;
	int32_t		status;		/* 0 or an errno value */
	uint32_t	reserved;
	union {
		struct {
			uint32_t client_id;
			uint32_t server_flags;
			uint64_t token;
			char	 socket_path[WS_MAX_SOCKET_PATH];
			uint32_t app_id;	/* client id of the app's first
						   connection; 0 from servers
						   without WS_SERVER_JOIN */
			uint32_t reserved;
		} hello;
		struct {
			uint32_t window;
			uint32_t reserved;
		} create;
		struct {
			char	 key[64];	/* SET_SETTINGS: the key that
						   failed, or was left out */
		} settings;
		uint8_t	raw[128];
	} u;
} ws_reply_t;

/* Largest request; servers size their receive buffer from this. */
#define WS_MAX_REQUEST_SIZE	1024

/* Client -> server socket messages. */
enum {
	WS_SOCK_AUTH = 1,
	WS_SOCK_BUFFER_CREATE = 2,	/* carries one fd (SCM_RIGHTS) */
};

typedef struct ws_sock_header {
	uint32_t	type;
	uint32_t	size;
} ws_sock_header_t;

typedef struct ws_sock_auth {
	ws_sock_header_t hdr;
	uint32_t	client_id;
	uint32_t	version;
	uint64_t	token;
} ws_sock_auth_t;

/*
 * A window buffer. The fd must be a memfd (or other shared memory fd)
 * of at least stride * height bytes, sealed with F_SEAL_SHRINK so the
 * server can't be made to fault on a truncated mapping.
 */
typedef struct ws_sock_buffer {
	ws_sock_header_t hdr;
	uint32_t	buffer_id;	/* chosen by the client, nonzero */
	uint32_t	width;
	uint32_t	height;
	uint32_t	stride;		/* bytes per row */
	uint32_t	format;		/* WS_FORMAT_ARGB8888 or _XRGB8888 */
	uint32_t	scale;		/* 1 = one buffer pixel per point */
	uint64_t	offset;		/* start of the pixels in the fd */
} ws_sock_buffer_t;

#ifdef __cplusplus
}
#endif

#endif /* !_WINDOWSERVER_WSPROTOCOL_H_ */
