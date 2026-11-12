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
	WS_MSG_ACTIVATE,		/* ws_req_int_t: WS_ACTIVATE_* (servers
					   with WS_SERVER_ACTIVATE_RAISE),
					   see "Activation" */
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
	/* servers with WS_SERVER_POINTER_GRAB, see "Pointer grabs" */
	WS_MSG_POINTER_GRAB,		/* sync or one-way, ws_req_int_t:
					   WS_GRAB_* */
	WS_MSG_POINTER_UNGRAB,
	/* servers with WS_SERVER_POINTER_WARP, see "Pointer warp and query" */
	WS_MSG_WARP_POINTER,		/* ws_req_point_t */
	WS_MSG_QUERY_POINTER,		/* sync: u.pointer */
	WS_MSG_POINTER_TRACK,		/* ws_req_int_t: 1 = on, 0 = off */
	/* servers with WS_SERVER_WINDOW_CURSOR */
	WS_MSG_WINDOW_SET_CURSOR,	/* ws_req_cursor_t, hdr.window */
	/* servers with WS_SERVER_INPUT_REGION */
	WS_MSG_WINDOW_SET_INPUT_REGION,	/* ws_req_region_t */
	/* servers with WS_SERVER_WINDOW_IDS */
	WS_MSG_RESERVE_WINDOW_IDS,	/* sync, ws_req_int_t: how many;
					   u.window_ids */
	/* app connections (servers with WS_SERVER_MENU_BAR), see "The menu
	   bar strip" */
	WS_MSG_SET_MENU_BAR,		/* ws_req_int_t: height in pixels,
					   0 = no menu bar */
	/* servers with WS_SERVER_TILING, see "Window tiling" */
	WS_MSG_WINDOW_SET_SIZE_LIMITS,	/* ws_req_size_limits_t */
	WS_MSG_TILING,			/* tools and the Dock, ws_req_tiling_t;
					   answers on the socket */
	/* servers with WS_SERVER_DRAG, see "Drag and drop" */
	WS_MSG_DRAG_BEGIN,		/* sync, ws_req_drag_begin_t;
					   u.drag.session */
	WS_MSG_DRAG_STATUS,		/* ws_req_drag_status_t, hdr.window */
	WS_MSG_DRAG_CANCEL,		/* ws_req_int_t: session */
	WS_MSG_DRAG_QUERY,		/* tools; answers on the socket */
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
	/* optional tail (servers with WS_SERVER_WINDOW_IDS), see "Window
	   ids chosen by the client" */
	uint32_t	window_id;	/* 0 = the server picks */
	uint32_t	reserved;
} ws_req_window_create_t;

/* A create request from before the optional tail. */
#define WS_WINDOW_CREATE_MIN_SIZE offsetof(ws_req_window_create_t, window_id)

typedef struct ws_req_frame {
	ws_req_header_t	hdr;
	ws_rect_t	frame;
} ws_req_frame_t;

typedef struct ws_req_int {		/* SET_LEVEL, SET_FLAGS, COMMIT,
					 * ATTACH, BUFFER_DESTROY,
					 * CURSOR_VISIBLE, ACTIVATE,
					 * SNAPSHOT, POINTER_GRAB,
					 * POINTER_TRACK,
					 * RESERVE_WINDOW_IDS */
	ws_req_header_t	hdr;
	int32_t		value;
	uint32_t	reserved;
} ws_req_int_t;

/*
 * WS_MSG_WINDOW_COMMIT with WS_COMMIT_SET_FRAME (servers with
 * WS_SERVER_COMMIT_FRAME): the window takes `frame` and the attached
 * buffer in one step, so a resize never shows the new frame with the
 * old buffer or the other way round. Without the flag a commit is a
 * plain ws_req_int_t (flags in `value`). Older servers read only
 * `value` and ignore the frame, so check for the flag first (the
 * client library falls back to WS_MSG_WINDOW_SET_FRAME, then commit).
 */
typedef struct ws_req_commit {
	ws_req_header_t	hdr;
	int32_t		value;		/* WS_COMMIT_* */
	uint32_t	reserved;
	ws_rect_t	frame;		/* WS_COMMIT_SET_FRAME */
} ws_req_commit_t;

/* WS_MSG_WARP_POINTER */
typedef struct ws_req_point {
	ws_req_header_t	hdr;		/* hdr.window: x, y are local to this
					   window of the sender; 0 = screen */
	double		x;
	double		y;
} ws_req_point_t;

/*
 * WS_MSG_WINDOW_SET_INPUT_REGION (servers with WS_SERVER_INPUT_REGION):
 * the part of the window that takes pointer events, as rectangles in
 * window-local coordinates (X's input shape). Elsewhere the pointer
 * passes through to what is below, as with WS_WINDOW_IGNORES_MOUSE.
 * What is drawn doesn't change: the buffer's alpha gives the window
 * its visible shape. A request without WS_REGION_APPEND replaces the
 * region (count 0: the window takes no pointer events at all), one
 * with it adds its rectangles, so a region of more than
 * WS_MAX_REGION_RECTS rectangles goes in several requests;
 * WS_REGION_RESET makes the whole window take them again, the default.
 * Being window-local, the region moves with the window; the parts of it
 * outside the frame do nothing, whatever size the window has then, so
 * it may be sent before a resize. It applies at once, not at the next
 * commit.
 */
#define WS_MAX_REGION_RECTS	56

typedef struct ws_req_region {
	ws_req_header_t	hdr;
	uint32_t	count;
	uint32_t	flags;		/* WS_REGION_* */
	ws_rect_t	rects[WS_MAX_REGION_RECTS];
} ws_req_region_t;

/*
 * Pointer grabs (servers with WS_SERVER_POINTER_GRAB), for menus that
 * open on a click and stay open after the release (click-release-click
 * menus in X, GTK and Qt), where the implicit grab while a button is
 * held isn't enough.
 *
 * WS_MSG_POINTER_GRAB (ws_req_int_t, `value` WS_GRAB_*), sent with a
 * reply port to learn the outcome or one-way (errors then come as
 * WS_EVENT_ERROR). Only a connection of the active app may grab
 * (EPERM otherwise), and only one connection holds the grab: a grab
 * by another connection fails with EBUSY. A connection that holds it
 * may send it again to change the mode. hdr.window is ignored.
 *
 *   WS_GRAB_POINTER: every pointer event comes to the grabbing
 *	connection, wherever the pointer is, until it sends
 *	WS_MSG_POINTER_UNGRAB. Over one of its own windows the events
 *	are about that window as usual, MOUSE_ENTERED and _EXITED
 *	included; over anything else (another app's window, a tile, the
 *	desktop) hdr.window is 0 and x, y are the screen position. A
 *	press activates nothing, raises nothing and makes nothing key,
 *	and no other connection sees the pointer. A press outside the
 *	connection's windows first sends WS_EVENT_POINTER_OUTSIDE, then
 *	comes as WS_EVENT_MOUSE_DOWN with hdr.window 0.
 *   WS_GRAB_OUTSIDE: events go where they would without it, but the
 *	first press outside the connection's windows sends it
 *	WS_EVENT_POINTER_OUTSIDE and ends the grab, then goes ahead as
 *	usual (it may activate another app). For popups that just need
 *	to know when to close, like Wayland's xdg_popup.
 *
 * The cursor while grabbed is the grabbing connection's: its window's
 * over its own windows, its connection cursor elsewhere.
 *
 * The server ends a grab itself, and sends WS_EVENT_POINTER_UNGRABBED,
 * when the app stops being active (Command-Tab, a Dock activation) or
 * the overview opens, so a client that never lets go can't keep the
 * pointer from the rest of the desktop. A grab also ends when the
 * connection goes away. WS_MSG_POINTER_UNGRAB from a connection
 * without the grab does nothing.
 */

/*
 * Pointer warp and query (servers with WS_SERVER_POINTER_WARP).
 *
 * WS_MSG_WARP_POINTER (ws_req_point_t) moves the pointer, for X's
 * XWarpPointer. Only the active app may warp, and only while the
 * pointer is over one of its windows (or a button pressed in one is
 * held) or while the sender holds a pointer grab; other warps fail
 * with EPERM (WS_EVENT_ERROR). The position is clamped to the screen;
 * the pointer moves there as if the user moved it (enter and exit
 * events, motion).
 *
 * WS_MSG_QUERY_POINTER (sync): the reply's u.pointer
 * (ws_pointer_info_t) says where the pointer is, the buttons and
 * modifiers held, and the sender's own window under it. Only for the
 * active app, or a connection with a window under the pointer
 * (EPERM otherwise).
 *
 * WS_MSG_POINTER_TRACK (ws_req_int_t, 1 on, 0 off): while its app is
 * active, the connection also gets WS_EVENT_MOUSE_MOVED with
 * hdr.window 0 and the screen position when the pointer moves over
 * anything that isn't one of its windows, the way XQueryPointer lets an
 * X client (xeyes) follow the pointer over the whole screen.
 */

/*
 * Window ids chosen by the client (servers with WS_SERVER_WINDOW_IDS).
 * Creating a window is a round trip, because the server picks its id.
 * Instead, a connection may reserve a block of ids once,
 * WS_MSG_RESERVE_WINDOW_IDS (sync, ws_req_int_t: how many, 1 to
 * WS_MAX_WINDOW_IDS), and then name the id of each new window itself
 * in WS_MSG_WINDOW_CREATE's window_id, sent one-way; errors come back
 * as WS_EVENT_ERROR. The reply's u.window_ids gives the block: ids
 * base to base + count - 1, never used for another window. Each id
 * names one window: ids are taken in increasing order, so one that is
 * lower than or equal to an id already used, or outside the block,
 * fails with EINVAL. A new reservation replaces the block. Ids the
 * server picks and reserved ids come from the same space, so ids stay
 * unique across connections.
 */
#define WS_MAX_WINDOW_IDS	4096

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

/*
 * WS_MSG_SET_CURSOR: the connection's cursor, shown while the pointer is
 * over its windows. WS_MSG_WINDOW_SET_CURSOR (servers with
 * WS_SERVER_WINDOW_CURSOR, hdr.window) gives one window a cursor of its
 * own, which wins over the connection's over that window, so the right
 * cursor shows the moment the pointer enters it. WS_CURSOR_INHERIT
 * takes it away again. Several windows may share one buffer; the
 * server keeps its own reference until the cursor changes or the
 * window is destroyed, so the client may destroy the buffer right
 * after. Hiding the cursor (WS_MSG_CURSOR_VISIBLE) applies to the
 * connection's windows whatever their cursors.
 */
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
 *     "The screen and its usable area"), and window tiling keeps
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
 * changes. It is the screen minus the menu bar strip along the top
 * (WS_MSG_SET_MENU_BAR, see "The menu bar strip") and the strip of the
 * edge the Dock keeps (WS_MSG_DOCK_SET_EDGE, see "The Dock"), the
 * screen itself while neither has been said. A top Dock's strip starts
 * below the menu bar's. Apps use it as NSScreen's visibleFrame. The
 * welcome and the event also carry the menu bar's height (`menu_bar`),
 * so a top Dock knows how far down to sit. WindowServer puts
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
 * Activation. WS_MSG_ACTIVATE (ws_req_int_t) makes the sender's app the
 * active app; WS_MSG_DEACTIVATE gives it up. On its own, activating
 * moves no window: an app that activates itself orders its own windows
 * as it likes. That leaves out the windows of connections that joined
 * the app (an X server's, for XLibre.app), which the app can't order.
 * With WS_ACTIVATE_RAISE in `value` (servers with
 * WS_SERVER_ACTIVATE_RAISE) the server first brings every window of
 * the app on normal and higher levels, the joined connections'
 * included, to the front of its level, keeping their order, as a Dock
 * activation does; their owners hear WS_EVENT_WINDOW_ORDERED. Older
 * servers ignore `value` and just activate.
 *
 * The Dock's app (servers with WS_SERVER_DOCK_NOT_ACTIVE) never becomes
 * the active app, as on Darwin: its app is the one whose process also
 * holds a WS_CONNECT_DOCK connection. A click in one of its windows, or
 * its own WS_MSG_ACTIVATE, leaves the active app active, menus, menu
 * bar strip, key window and Command keys included; that app hears
 * nothing. The Dock's app instead hears WS_EVENT_APP_ACTIVATED (so
 * that a GNUstep Dock can pop up menus, grab the pointer for them and
 * show its panels as before) and, as soon as anything else is
 * activated or clicked, WS_EVENT_APP_DEACTIVATED. Meanwhile it may
 * grab, warp and query the pointer like the active app, and keys go
 * to its key window while it has a visible one (a Dock popup with a
 * text field, made key with WS_MSG_WINDOW_MAKE_KEY or a click), and
 * to the active app otherwise. Its first click isn't swallowed when
 * ClickThrough is off. If the Dock's app was active before its Dock
 * connection came, the next app takes over then. Older servers make
 * the Dock's app active like any other.
 */

/*
 * The menu bar strip (servers with WS_SERVER_MENU_BAR). In the Mac menu
 * style (NSMacintoshInterfaceStyle) each GNUstep app draws its own main
 * menu as a bar along the top of the screen. The strip it takes comes
 * off the usable area, so new windows, zoomed windows, tiles, the
 * overview and window tiling stay below it, and a Dock on the top edge
 * sits directly below it (its WS_MSG_DOCK_SET_EDGE thickness is
 * measured from the bar's bottom edge).
 *
 * WS_MSG_SET_MENU_BAR (ws_req_int_t), from app connections (a joined
 * connection speaks for its app; tools and Docks get EPERM): `value` is
 * the bar's height in pixels, 0 when the app shows no bar (the NeXT or
 * in-window menu style). More than a quarter of the screen's height
 * fails with EINVAL. The server keeps each app's newest report.
 *
 * Only the active app sets the strip: there is no separate menu-bar
 * process to trust, every app draws its own bar, and the active app's
 * bar is the one on top. When an app becomes active its report, if it
 * made one, sets the strip at once. An active app that never reported
 * (an X11 app, a tool-like client, an app with no main menu) leaves the
 * strip as the last active app that did report set it, so the strip
 * doesn't come and go with the focus; that app may still change or
 * clear it while inactive (it hears the style change after the user
 * moved on). When that app goes away and the active app hasn't
 * reported, there is no strip. A report from any other inactive app is
 * kept for when it becomes active.
 */

/*
 * Window tiling (servers with WS_SERVER_TILING). Windows stack, as
 * always, until the WindowMode setting says "tiling": then WindowServer
 * lays out the ordinary windows of every app side by side in the usable
 * area (see "The screen and its usable area": the Dock's strip and the
 * menu bar strip stay free), with TilingGapsInner pixels between them
 * and TilingGapsOuter around them, and lays them out again whenever one
 * is shown, hidden, minimized, restored or closed, or the usable area
 * changes (the Dock moved). The mode is the screen's (one layout per
 * screen now; one per workspace once there are workspaces). Owners hear
 * their windows' new frames as WS_EVENT_WINDOW_FRAME, as for any frame
 * the server sets, with WS_FRAME_TILED, and draw them at the new size.
 * Leaving tiling mode (or floating a window) gives every window back the
 * frame it had before it was tiled, without WS_FRAME_TILED, so an owner
 * that saves window frames can tell tiles from the frames to keep.
 *
 * Layouts (TilingLayout): "dwindle" gives the first window part of the
 * area and the next one part of what is left, splitting the longer side
 * each time; "master" gives the first window the master area (left, or
 * top on a tall screen) and stacks the others beside it. New windows
 * come after the window that had the focus (dwindle) or at the end of
 * the stack (master).
 *
 * What tiles: visible windows on the normal level, of app connections
 * (not the Dock's app), that take pointer events and can be key, with
 * none of WS_WINDOW_NO_TILE (the app's choice), WS_WINDOW_FIXED_SIZE
 * (the user can't resize it) and WS_WINDOW_PANEL (panels, alerts,
 * sheets), and that fit their tile. Everything else floats above the
 * tiled windows, where its owner puts it: menus, torn-off menus,
 * panels, desktop-level windows, app icons, the Dock. A window floats
 * too when the user floats it, when its minimum size
 * (WS_MSG_WINDOW_SET_SIZE_LIMITS) is larger than its tile, or when its
 * owner keeps giving it a larger frame than its tile (three times in a
 * row). An owner that answers with a smaller frame keeps that size, at
 * the tile's top-left corner; a maximum size smaller than the tile
 * centres the window in it.
 *
 * The stack: on the normal level, tiled windows are always below the
 * windows that float, whatever their owners order. Clicking a tiled
 * window raises it among the tiled ones.
 *
 * WS_MSG_WINDOW_SET_SIZE_LIMITS (ws_req_size_limits_t, hdr.window): the
 * smallest and largest frame the window takes, 0 for no limit, as
 * NSWindow's minSize and maxSize. Only tiling reads them.
 *
 * Keys, while tiling is on (TilingModifier, "Command+Option" by
 * default, is any of Command, Option, Control; never Shift):
 *	modifier+arrow			focus the tile that way
 *	modifier+Shift+arrow		swap places with it
 *	modifier+Minus, modifier+Equal	shrink, grow the focused tile
 *	modifier+Shift+Space		float the key window, or tile it again
 * Otherwise the keys go to the app as usual.
 *
 * The pointer: a press in the gap between two tiles (TilingGapsInner,
 * or 4 pixels either side of the line without a gap) takes the split,
 * and dragging moves it. When the user drags a tiled window by its
 * title bar (its owner moves the window while a button pressed in it
 * is held), it follows; let go over another tile and the two swap
 * places, anywhere else and it goes back. If the owner resized the
 * window that way (a resize bar), the splits along the edges it moved
 * follow.
 *
 * With TilingBorder > 0 the active app's key window, if tiled, gets a
 * border of that many pixels in TilingBorderColor, outside its frame.
 *
 * In tiling mode a minimized window always goes into the Dock (a tile
 * on the bottom row would cover the tiled windows), or, while no Dock
 * shows minimized windows, onto the bottom row as before.
 *
 * WS_MSG_TILING (ws_req_tiling_t), from WS_CONNECT_CONTROL and
 * WS_CONNECT_DOCK connections with the same user check as the window
 * list: `op` WS_TILING_FOCUS, _SWAP, _RESIZE and _FLOAT do what the keys
 * do (FLOAT on hdr.window, or the key window when it is 0), and every
 * op, WS_TILING_QUERY included, is answered on the socket with
 * WS_EVENT_TILING_STATE, WS_EVENT_TILING_ITEMs and WS_EVENT_LIST_END
 * (WSTypes.h). Turning tiling on and off and choosing the layout are
 * settings (WS_MSG_SET_SETTINGS), so Workspace Preferences switches the
 * mode live with one request, and the session's value persists in the
 * user's org.nextbsd.WindowServer defaults. Window lists mark tiled
 * windows with WS_WINDOW_INFO_TILED.
 */
typedef struct ws_req_size_limits {	/* WINDOW_SET_SIZE_LIMITS */
	ws_req_header_t	hdr;
	uint32_t	min_width;	/* 0 = no limit */
	uint32_t	min_height;
	uint32_t	max_width;
	uint32_t	max_height;
} ws_req_size_limits_t;

typedef struct ws_req_tiling {		/* TILING */
	ws_req_header_t	hdr;		/* hdr.window: WS_TILING_FLOAT's window,
					   0 = the key window */
	uint32_t	op;		/* WS_TILING_* */
	int32_t		arg;
} ws_req_tiling_t;

/*
 * Drag and drop (servers with WS_SERVER_DRAG), the way Darwin's window
 * server mediates drags: the source app puts the data on a named
 * pasteboard (NSDragPboard in GNUstep's pasteboard server, gpbs) and
 * hands WindowServer the drag; WindowServer moves the image with the
 * pointer, above every window and the Dock, asks the window under the
 * pointer what it would do, shows the cursor for the answer, and tells
 * both sides how it ended. Only data that's on the pasteboard crosses
 * between the apps.
 *
 * Who takes drags: windows with WS_WINDOW_ACCEPTS_DRAGS (the GNUstep
 * backend sets it on windows that have views registered for dragged
 * types), of app connections, where the pointer hits them (input
 * regions count). A window above that doesn't take drags hides the
 * ones below it, as on macOS. The source's own windows take its drags
 * too: a drag inside one app goes through WindowServer like any other.
 *
 * WS_MSG_DRAG_BEGIN (sync, ws_req_drag_begin_t) starts a drag. The
 * sender must be the app a button went down in, while that button is
 * held, or, so a quick flick isn't lost, the app the last button went
 * up in, within WS_DRAG_LATE_USEC of the release: that drag is then
 * dropped where the button went up as soon as it starts. Otherwise
 * EPERM; EBUSY while another drag is under way. The reply's
 * u.drag.session names the drag.
 *   buffer_id		the drag image (premultiplied ARGB8888, at most
 *			WS_MAX_DRAG_IMAGE pixels on a side), copied by the
 *			server, so the client may destroy it right away; 0
 *			for none
 *   hot_x, hot_y	the pointer's position in the image
 *   ops_local		WS_DRAG_OP_* allowed onto the source app's own windows
 *   ops_external	and onto other apps' windows
 *   flags		WS_DRAG_SLIDE_BACK, WS_DRAG_IGNORE_MODIFIERS
 *   slide_x, slide_y	where the image's top-left corner slides back to
 *   pasteboard		the pasteboard holding the data, NUL-terminated
 *   types		the types on it (NSPasteboard type names), each
 *			NUL-terminated, ntypes of them in types_len bytes;
 *			kept for the drag state and for bridges to other
 *			protocols (XDND)
 * While the drag lasts no app gets pointer motion; the button's release
 * still goes to the source's window, before WS_EVENT_DRAG_END. The
 * operations offered to a window are ops_local or ops_external, as
 * GNUstep's -draggingSourceOperationMaskForLocal: says, narrowed by
 * the modifiers held unless WS_DRAG_IGNORE_MODIFIERS: Control to
 * WS_DRAG_OP_LINK, Option to _COPY, Command to _GENERIC (libs-gui's
 * GSDragView rules).
 *
 * The window under the pointer gets WS_EVENT_DRAG: WS_DRAG_ENTER when
 * the drag comes over it, WS_DRAG_UPDATE when it moves or the
 * operations change, WS_DRAG_EXIT when it leaves or ends without a drop
 * on it. Its owner answers ENTER and UPDATE with WS_MSG_DRAG_STATUS
 * (one-way, ws_req_drag_status_t, hdr.window the window), the operation
 * it would take, WS_DRAG_OP_NONE if none; statuses for an old session
 * or a window the drag has left are ignored. The cursor follows the
 * answer: WS_CURSOR_DRAG_COPY for _COPY and _GENERIC, _DRAG_LINK for
 * _LINK, the arrow otherwise; the source hears WS_EVENT_DRAG_STATUS.
 *
 * When the button goes up over a window whose last answer, of the
 * operations offered, isn't WS_DRAG_OP_NONE, that window gets
 * WS_DRAG_DROP and reads the pasteboard; the image goes away, and the
 * source gets WS_EVENT_DRAG_END with WS_DRAG_END_DROPPED and the
 * operation. If the window hasn't answered the last ENTER or UPDATE
 * yet, the server waits for it up to WS_DRAG_STATUS_WAIT_MS first.
 * Otherwise the window gets WS_DRAG_EXIT, the image slides back if the
 * drag asked for that, and the source gets WS_EVENT_DRAG_END with
 * WS_DRAG_OP_NONE. The source isn't kept waiting for the drop to be
 * handled, so it can answer the target's requests for data it promised
 * on the pasteboard. A target may send WS_MSG_DRAG_STATUS with
 * WS_DRAG_STATUS_FINISHED after a drop; the server takes it as done.
 *
 * Escape (the key goes to nobody) and WS_MSG_DRAG_CANCEL from the source
 * cancel the drag: WS_DRAG_EXIT, slide back, WS_EVENT_DRAG_END with
 * WS_DRAG_END_CANCELLED. If the source goes away the target gets
 * WS_DRAG_EXIT. While a drag lasts the overview doesn't open.
 *
 * WS_MSG_DRAG_QUERY (tool connections, the console user): answered on
 * the socket with WS_EVENT_DRAG_STATE.
 *
 * X11 windows (an X server joined with WS_CONNECT_JOIN) take drags only
 * if the X server sets WS_WINDOW_ACCEPTS_DRAGS and speaks this protocol
 * for its clients: a bridge to XDND, not done yet.
 */
#define WS_DRAG_LATE_USEC	500000	/* a begin after the release */
#define WS_DRAG_STATUS_WAIT_MS	250	/* a drop waits this long for the
					   target's answer */

typedef struct ws_req_drag_begin {	/* DRAG_BEGIN */
	ws_req_header_t	hdr;		/* hdr.window: the source's window the
					   drag starts in, or 0 */
	uint32_t	buffer_id;	/* drag image, 0 = none */
	uint32_t	flags;		/* WS_DRAG_* */
	int32_t		hot_x;		/* pointer position in the image */
	int32_t		hot_y;
	uint32_t	ops_local;	/* WS_DRAG_OP_* onto the source app */
	uint32_t	ops_external;	/* onto other apps */
	int32_t		slide_x;	/* screen position the image's top-left */
	int32_t		slide_y;	/* corner slides back to */
	uint32_t	ntypes;
	uint32_t	types_len;	/* bytes used in types */
	char		pasteboard[WS_MAX_DRAG_PASTEBOARD];
	char		types[WS_MAX_DRAG_TYPES];
} ws_req_drag_begin_t;

typedef struct ws_req_drag_status {	/* DRAG_STATUS */
	ws_req_header_t	hdr;		/* hdr.window: the window answering */
	uint32_t	session;
	uint32_t	operation;	/* WS_DRAG_OP_* it takes */
	uint32_t	flags;		/* WS_DRAG_STATUS_* */
	uint32_t	reserved;
} ws_req_drag_status_t;

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
 *   - WindowMode, TilingLayout, TilingGapsInner, TilingGapsOuter,
 *     TilingMasterRatio, TilingBorder, TilingBorderColor,
 *     TilingModifier (servers with WS_SERVER_TILING): the windows are
 *     tiled, laid out again, or given back their frames from before
 *     tiling (see "Window tiling").
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
		ws_pointer_info_t pointer;	/* QUERY_POINTER */
		struct {
			uint32_t base;		/* RESERVE_WINDOW_IDS */
			uint32_t count;
		} window_ids;
		struct {
			uint32_t session;	/* DRAG_BEGIN */
			uint32_t reserved;
		} drag;
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
