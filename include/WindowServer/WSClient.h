/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * WSClient.h - libWindowServerClient, the C client library for
 * WindowServer. It hides the Mach and socket details of WSProtocol.h.
 *
 * Usage outline:
 *
 *	ws_connection_t *c = ws_connect("TextEdit", 0);
 *	uint32_t win = ws_window_create(c, &frame, WS_LEVEL_NORMAL, 0, "Untitled");
 *	ws_buffer_t *b = ws_buffer_create(c, frame.width, frame.height, 1);
 *	... draw into ws_buffer_data(b) (premultiplied ARGB8888) ...
 *	ws_window_attach(c, win, b);
 *	ws_window_commit(c, win, 0);
 *	ws_window_order(c, win, WS_ORDER_ABOVE, 0);
 *	poll ws_connection_fd(c) for POLLIN, then call ws_dispatch(c, handler, ctx)
 *
 * A connection is not thread-safe; use it from one thread at a time.
 * Calls that wait for the server (ws_connect, ws_snapshot,
 * ws_list_windows) may read events off the socket and keep them in an
 * internal queue. After such a call, run ws_dispatch() even if the fd
 * isn't readable (ws_pending() tells you whether anything is queued).
 *
 * Unless noted, functions return 0 on success and -1 with errno set.
 */
#ifndef _WINDOWSERVER_WSCLIENT_H_
#define _WINDOWSERVER_WSCLIENT_H_

#include <stddef.h>
#include <stdint.h>
#include <WindowServer/WSTypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ws_connection ws_connection_t;
typedef struct ws_buffer ws_buffer_t;

typedef void (*ws_event_handler_t)(ws_connection_t *conn,
    const ws_event_t *event, void *context);

/*
 * Connect to WindowServer. The service name comes from the
 * WS_SERVICE_NAME environment variable, or WS_SERVICE_NAME by default.
 * flags: WS_CONNECT_* . Returns NULL with errno set on failure.
 *
 * With WS_CONNECT_XKB the connection also gets WS_EVENT_KEYMAP,
 * WS_EVENT_MODIFIERS and WS_EVENT_KEYS_HELD (WSTypes.h). A keymap event
 * carries a descriptor in u.keymap.fd that the handler must close.
 */
ws_connection_t *ws_connect(const char *app_name, uint32_t flags);
ws_connection_t *ws_connect_service(const char *service, const char *app_name,
    uint32_t flags);
void		 ws_disconnect(ws_connection_t *conn);

/*
 * Connect as part of another connection's app (WS_CONNECT_JOIN in
 * WSProtocol.h): WindowServer then activates both together, and they
 * share one key window. app_client_id and app_token are that
 * connection's ws_client_id() and ws_client_token(), handed to this
 * process by the app (XLibre.app passes them to the X server it
 * starts). Fails with EPERM if they're wrong, and with ENOTSUP if the
 * server can't join apps.
 */
ws_connection_t *ws_connect_join(const char *app_name, uint32_t flags,
    uint32_t app_client_id, uint64_t app_token);

/* File descriptor to poll for POLLIN; events are read by ws_dispatch. */
int		 ws_connection_fd(ws_connection_t *conn);
uint32_t	 ws_client_id(ws_connection_t *conn);
/*
 * The connection's secret token. Anyone holding it with the client id
 * can join this connection's app (ws_connect_join), so hand it only to
 * a helper process of your own, never on a command line other users
 * can read.
 */
uint64_t	 ws_client_token(ws_connection_t *conn);
/* Client id of the app this connection belongs to: its own id, or the
 * app it joined. 0 if the server predates joining. */
uint32_t	 ws_app_id(ws_connection_t *conn);
uint32_t	 ws_server_flags(ws_connection_t *conn);
const ws_screen_info_t *ws_main_screen(ws_connection_t *conn);
/*
 * The main screen's usable area (see "The screen and its usable area"
 * in WSProtocol.h): the screen minus the Dock's strip, as of the last
 * event dispatched. Both follow WS_EVENT_SCREEN_CHANGED and
 * WS_EVENT_USABLE_AREA. Servers without WS_SERVER_USABLE_AREA: fills
 * in the whole screen and fails with ENOTSUP.
 */
int		 ws_usable_area(ws_connection_t *conn, ws_rect_t *area);
/*
 * The height of the menu bar strip along the top of the main screen
 * (see "The menu bar strip" in WSProtocol.h), as of the last event
 * dispatched: a Dock on the top edge sits directly below it. 0 when
 * there is none, or from servers without WS_SERVER_MENU_BAR.
 */
uint32_t	 ws_menu_bar_height(ws_connection_t *conn);

/*
 * Read and dispatch all available events without blocking. Returns
 * the number of events dispatched, or -1 when the connection is gone
 * (errno EPIPE) or broken.
 */
int		 ws_dispatch(ws_connection_t *conn, ws_event_handler_t handler,
    void *context);
/* Wait up to timeout_ms (-1 = forever) for one event. Returns 1 with
 * *event filled in, 0 on timeout, -1 on error. */
int		 ws_next_event(ws_connection_t *conn, ws_event_t *event,
    int timeout_ms);
/* Nonzero if events are queued inside the library. */
int		 ws_pending(ws_connection_t *conn);
/* Round trip: returns after the server has handled earlier requests. */
int		 ws_sync(ws_connection_t *conn);

/* Windows. ws_window_create returns the window id, 0 on failure. New
 * windows are ordered out; order them in with ws_window_order. */
uint32_t	 ws_window_create(ws_connection_t *conn, const ws_rect_t *frame,
    int32_t level, uint32_t flags, const char *title);
int		 ws_window_destroy(ws_connection_t *conn, uint32_t window);
int		 ws_window_set_frame(ws_connection_t *conn, uint32_t window,
    const ws_rect_t *frame);
int		 ws_window_set_level(ws_connection_t *conn, uint32_t window,
    int32_t level);
int		 ws_window_order(ws_connection_t *conn, uint32_t window,
    int op, uint32_t relative_to);
int		 ws_window_set_title(ws_connection_t *conn, uint32_t window,
    const char *title);
int		 ws_window_set_flags(ws_connection_t *conn, uint32_t window,
    uint32_t flags);
int		 ws_window_make_key(ws_connection_t *conn, uint32_t window);
/*
 * The smallest and largest frame the window takes (NSWindow's minSize
 * and maxSize), 0 for no limit: window tiling floats a window whose
 * minimum doesn't fit its tile. Servers with WS_SERVER_TILING (others
 * fail with ENOTSUP).
 */
int		 ws_window_set_size_limits(ws_connection_t *conn,
    uint32_t window, uint32_t min_width, uint32_t min_height,
    uint32_t max_width, uint32_t max_height);
/*
 * ws_window_create without the round trip, on servers with
 * WS_SERVER_WINDOW_IDS: the id comes from a block the library reserves
 * (one round trip per 256 windows), and the request goes one-way, so a
 * failure arrives later as WS_EVENT_ERROR about that id. On older
 * servers it is ws_window_create.
 */
uint32_t	 ws_window_create_nowait(ws_connection_t *conn,
    const ws_rect_t *frame, int32_t level, uint32_t flags, const char *title);
/*
 * The part of the window that takes pointer events (servers with
 * WS_SERVER_INPUT_REGION, else ENOTSUP), window-local rectangles: X's
 * input shape. count 0 makes the whole window let the pointer through;
 * rects NULL makes all of it take events again.
 */
int		 ws_window_set_input_region(ws_connection_t *conn,
    uint32_t window, const ws_rect_t *rects, unsigned count);

/*
 * Buffers: a memfd of width*height premultiplied ARGB8888 pixels,
 * mapped into the client and handed to the server once. scale is the
 * number of buffer pixels per point (1 for now).
 */
ws_buffer_t	*ws_buffer_create(ws_connection_t *conn, uint32_t width,
    uint32_t height, uint32_t scale);
/*
 * The same with a pixel format: WS_FORMAT_ARGB8888 (what
 * ws_buffer_create makes) or WS_FORMAT_XRGB8888, whose top byte is
 * ignored: the window is opaque (servers with WS_SERVER_XRGB; older ones
 * answer with a WS_EVENT_ERROR).
 */
ws_buffer_t	*ws_buffer_create_format(ws_connection_t *conn, uint32_t width,
    uint32_t height, uint32_t scale, uint32_t format);
void		 ws_buffer_destroy(ws_connection_t *conn, ws_buffer_t *buffer);
void		*ws_buffer_data(ws_buffer_t *buffer);
uint32_t	 ws_buffer_stride(ws_buffer_t *buffer);
uint32_t	 ws_buffer_width(ws_buffer_t *buffer);
uint32_t	 ws_buffer_height(ws_buffer_t *buffer);
uint32_t	 ws_buffer_id(ws_buffer_t *buffer);

/*
 * Attach a buffer (NULL detaches), add damage in window-local
 * coordinates, then commit. A commit with no damage after a new
 * attach repaints the whole window. The previous buffer is handed
 * back with WS_EVENT_BUFFER_RELEASE.
 */
int		 ws_window_attach(ws_connection_t *conn, uint32_t window,
    ws_buffer_t *buffer);
int		 ws_window_damage(ws_connection_t *conn, uint32_t window,
    const ws_rect_t *rects, unsigned count);
int		 ws_window_commit(ws_connection_t *conn, uint32_t window,
    uint32_t flags);
/*
 * Commit with a new frame, so a resize shows its new size and its new
 * buffer together (servers with WS_SERVER_COMMIT_FRAME; on older ones
 * it sets the frame, then commits).
 */
int		 ws_window_commit_frame(ws_connection_t *conn, uint32_t window,
    uint32_t flags, const ws_rect_t *frame);

/*
 * Minimizing, on servers with WS_SERVER_MINIMIZE (ws_server_flags()).
 * ws_window_minimize() replaces the window with a live tile; the
 * server sends WS_EVENT_WINDOW_RESTORE when the user double-clicks it,
 * and ordering the window in (ws_window_order) restores it.
 * ws_set_app_icon() gives the icon shown on the tiles: width x height
 * premultiplied ARGB8888 pixels, rows `stride` bytes apart, at most
 * WS_MAX_APP_ICON square. NULL removes it. Older servers ignore both,
 * so check for WS_SERVER_MINIMIZE first.
 */
int		 ws_window_minimize(ws_connection_t *conn, uint32_t window);
/*
 * ws_window_minimize_where() also says where the window goes:
 * WS_MINIMIZE_INTO_DOCK (the Dock, or the bottom row while no Dock is
 * connected), WS_MINIMIZE_BOTTOM_ROW, or WS_MINIMIZE_DEFAULT (the
 * server's MinimizeIntoDock, as ws_window_minimize()). On servers
 * without WS_SERVER_MINIMIZE_WHERE it's ws_window_minimize().
 */
int		 ws_window_minimize_where(ws_connection_t *conn, uint32_t window,
    uint32_t where);
int		 ws_set_app_icon(ws_connection_t *conn, uint32_t width,
    uint32_t height, const void *argb, uint32_t stride);

/* Cursor: a standard kind, or WS_CURSOR_CUSTOM with a buffer. */
int		 ws_set_cursor(ws_connection_t *conn, uint32_t kind,
    ws_buffer_t *image, int32_t hot_x, int32_t hot_y);
int		 ws_set_cursor_visible(ws_connection_t *conn, int visible);
/*
 * A cursor for one window, shown over it instead of the connection's
 * (servers with WS_SERVER_WINDOW_CURSOR, else ENOTSUP).
 * WS_CURSOR_INHERIT goes back to the connection's. The server keeps its
 * own reference to the image, so it may be destroyed right after.
 */
int		 ws_window_set_cursor(ws_connection_t *conn, uint32_t window,
    uint32_t kind, ws_buffer_t *image, int32_t hot_x, int32_t hot_y);

/*
 * Pointer grabs (servers with WS_SERVER_POINTER_GRAB, else ENOTSUP; see
 * "Pointer grabs" in WSProtocol.h). ws_pointer_grab waits for the
 * answer: EPERM unless this connection's app is active, EBUSY if
 * another connection holds the grab. mode WS_GRAB_POINTER sends every
 * pointer event here (hdr.window 0 over anything that isn't one of
 * this connection's windows) and activates nothing; WS_GRAB_OUTSIDE
 * only sends WS_EVENT_POINTER_OUTSIDE for the first press outside its
 * windows, and ends there. The server ends a grab itself with
 * WS_EVENT_POINTER_UNGRABBED when the app is deactivated or the
 * overview opens.
 */
int		 ws_pointer_grab(ws_connection_t *conn, uint32_t mode);
int		 ws_pointer_ungrab(ws_connection_t *conn);
/*
 * Pointer warp and query (servers with WS_SERVER_POINTER_WARP, else
 * ENOTSUP; see "Pointer warp and query" in WSProtocol.h).
 *   ws_warp_pointer	move the pointer to x, y, local to `window` (one
 *			of this connection's) or the screen's (window 0);
 *			only while this app is active and the pointer is
 *			over one of its windows, or this connection holds
 *			the grab (else WS_EVENT_ERROR, EPERM)
 *   ws_query_pointer	where the pointer is; for the active app, or
 *			while the pointer is over one of this
 *			connection's windows (EPERM otherwise)
 *   ws_pointer_track	while this app is active, also get
 *			WS_EVENT_MOUSE_MOVED with hdr.window 0 over
 *			anything that isn't one of its windows
 */
int		 ws_warp_pointer(ws_connection_t *conn, uint32_t window,
    double x, double y);
int		 ws_query_pointer(ws_connection_t *conn, ws_pointer_info_t *info);
int		 ws_pointer_track(ws_connection_t *conn, int on);

/* Activation: make this app the active app, or give it up.
 * ws_activate_flags takes WS_ACTIVATE_* (WS_ACTIVATE_RAISE: servers with
 * WS_SERVER_ACTIVATE_RAISE; older ones just activate). */
int		 ws_activate(ws_connection_t *conn);
int		 ws_activate_flags(ws_connection_t *conn, uint32_t flags);
int		 ws_deactivate(ws_connection_t *conn);

/*
 * The Mac menu style: this app's menu bar along the top of the screen
 * is `height` pixels, 0 when it shows none. The active app's sets the
 * menu bar strip that comes off the usable area (see "The menu bar
 * strip" in WSProtocol.h). Servers with WS_SERVER_MENU_BAR.
 */
int		 ws_set_menu_bar(ws_connection_t *conn, uint32_t height);

/*
 * Tool requests (wsctl). ws_snapshot returns the composited screen
 * as a mapped image; release it with ws_snapshot_free.
 */
typedef struct ws_snapshot {
	uint32_t	width;
	uint32_t	height;
	uint32_t	stride;
	uint32_t	format;		/* WS_FORMAT_XRGB8888 */
	void		*pixels;
	size_t		map_size;
} ws_snapshot_t;

int		 ws_snapshot(ws_connection_t *conn, ws_snapshot_t *out);
void		 ws_snapshot_free(ws_snapshot_t *snap);
/*
 * Calls fn once per window, bottom to top, then returns. Only
 * WS_CONNECT_CONTROL and WS_CONNECT_DOCK connections may list (EPERM
 * otherwise). ws_list_windows_flags takes WS_LIST_* options
 * (WS_LIST_ORDERED_OUT: windows that aren't shown too).
 */
int		 ws_list_windows(ws_connection_t *conn,
    void (*fn)(const ws_event_window_info_t *info, void *ctx), void *ctx);
int		 ws_list_windows_flags(ws_connection_t *conn, uint32_t options,
    void (*fn)(const ws_event_window_info_t *info, void *ctx), void *ctx);
int		 ws_inject_motion(ws_connection_t *conn, double x, double y);
int		 ws_inject_button(ws_connection_t *conn, uint32_t button,
    int pressed);
int		 ws_inject_key(ws_connection_t *conn, const char *key_name,
    int pressed);
int		 ws_inject_key_tap(ws_connection_t *conn, const char *key_name);
int		 ws_inject_scroll(ws_connection_t *conn, double dx, double dy);
/*
 * Touchpad input, on servers with WS_SERVER_SCROLL_PHASES: a
 * two-finger scroll by dx, dy pixels (0, 0: the fingers lift, which
 * may start momentum scrolling), and fingers resting on the touchpad
 * (libinput's hold gesture), which stops it.
 */
int		 ws_inject_scroll_finger(ws_connection_t *conn, double dx,
    double dy);
int		 ws_inject_hold(ws_connection_t *conn, int down);
/* A touchpad swipe step (WS_GESTURE_*), on servers with WS_SERVER_OVERVIEW. */
int		 ws_inject_swipe(ws_connection_t *conn, uint32_t fingers,
    int phase, double dx, double dy);
/* Headless servers with WS_SERVER_SCREEN_CHANGES: the screen becomes
 * width x height, as a mode change would make it (tests). */
int		 ws_inject_screen_size(ws_connection_t *conn, uint32_t width,
    uint32_t height);

/*
 * The window overview (servers with WS_SERVER_OVERVIEW; tool and Dock
 * connections only). request is WS_OVERVIEW_ALL or _APP (open), _CLOSE
 * or _QUERY; then *state (if not NULL) gets the state and fn is called
 * for each window shown.
 */
int		 ws_overview(ws_connection_t *conn, uint32_t request,
    ws_event_overview_t *state,
    void (*fn)(const ws_event_overview_item_t *item, void *ctx), void *ctx);

/*
 * Window tiling (servers with WS_SERVER_TILING; tool and Dock
 * connections only; see "Window tiling" in WSProtocol.h). op is
 * WS_TILING_QUERY, _FOCUS or _SWAP (arg WS_TILING_DIR_*), _RESIZE (arg
 * > 0 grows, < 0 shrinks the focused tile) or _FLOAT (window, 0 for the
 * key window; arg 1 float, 0 tile again, 2 toggle); then *state (if not
 * NULL) gets the state and fn is called for each tiled window, in layout
 * order, and each window the user floated or that didn't fit. Turning
 * tiling on and off and the layout are settings (WindowMode,
 * TilingLayout...: ws_set_setting).
 */
int		 ws_tiling(ws_connection_t *conn, uint32_t op, uint32_t window,
    int32_t arg, ws_event_tiling_t *state,
    void (*fn)(const ws_event_tiling_item_t *item, void *ctx), void *ctx);

/*
 * The Dock (WS_CONNECT_DOCK on servers with WS_SERVER_DOCK; see "The
 * Dock" in WSProtocol.h). A Dock connection lists every app's windows
 * (ws_list_windows_flags), gets WS_EVENT_WINDOW_LIST_CHANGED when that
 * list changes and WS_EVENT_DOCK_STATE, and may:
 *   ws_dock_activate	activate app `app_id` (window 0), or the app of
 *			`window` with that window key and in front; a
 *			minimized window is restored instead
 *   ws_dock_restore	ask the owner of a minimized window to restore it
 *   ws_dock_preview	a scaled picture of a window, at most max_size
 *			pixels on its longer side, premultiplied
 *			ARGB8888; release it with ws_snapshot_free
 *   ws_dock_show_minimized
 *			say this Dock can show minimized windows;
 *			WindowServer then hands it the ones minimized into
 *			the Dock (WS_DOCK_MINIMIZED_WINDOWS) instead of
 *			drawing tiles for them
 *   ws_dock_set_edge	say which screen edge this Dock is on
 *			(WS_DOCK_EDGE_*) and how thick it is, so tiling
 *			keeps it free; servers with WS_SERVER_DOCK_EDGE
 */
int		 ws_dock_activate(ws_connection_t *conn, uint32_t app_id,
    uint32_t window);
int		 ws_dock_restore(ws_connection_t *conn, uint32_t window);
int		 ws_dock_show_minimized(ws_connection_t *conn, int on);
int		 ws_dock_set_edge(ws_connection_t *conn, uint32_t edge,
    uint32_t thickness);
int		 ws_dock_preview(ws_connection_t *conn, uint32_t window,
    uint32_t max_size, ws_snapshot_t *out);

/*
 * Settings, on servers with WS_SERVER_SETTINGS (others fail with
 * ENOTSUP); see "Settings" in WSProtocol.h. Changing them is for the
 * console user (EPERM otherwise).
 *
 *   ws_set_settings	change the session's values: count entries of
 *			key, WS_SETTING_* type and value as text (a string
 *			is parsed for any key; WS_SETTING_DEFAULT drops
 *			the session's value). flags: WS_SETTINGS_*. If a
 *			key is refused, failed_key (may be NULL) gets its
 *			name; with WS_SETTINGS_SKIP_INVALID the call
 *			succeeds and failed_key names the first key left
 *			out ("" if none). Long lists go in several
 *			requests.
 *   ws_set_setting	one key
 *   ws_get_settings	fn gets every key's value in effect
 *   ws_settings_load_file
 *			send a flat plist of settings (a user's
 *			org.nextbsd.WindowServer defaults) as one set:
 *			flags WS_SETTINGS_REPLACE | WS_SETTINGS_SKIP_INVALID
 *			is what a login does. A missing file is an
 *			empty set.
 *   ws_user_settings_path
 *			that file for this user: $HOME/Library/Preferences/
 *			org.nextbsd.WindowServer.plist (the directory
 *			follows GNUSTEP_USER_DEFAULTS_DIR when it's set)
 *   ws_set_keyboard_layout
 *			make layout `index` of the KeyboardLayout list
 *			active (EINVAL past its end)
 *   ws_list_keyboard_layouts
 *			fn gets each layout in the list
 *			(WS_KEYBOARD_LAYOUT_ACTIVE marks the active one).
 *			Tool and Dock connections also get a
 *			WS_EVENT_KEYBOARD_LAYOUT with
 *			WS_KEYBOARD_LAYOUT_CHANGED whenever the active
 *			layout changes, and WS_EVENT_SETTINGS_CHANGED
 *			when settings do.
 *   ws_list_input_devices
 *			fn gets each input device (WS_INPUT_TOUCHPAD,
 *			WS_INPUT_MOUSE...)
 */
#define WS_SETTINGS_DOMAIN	"org.nextbsd.WindowServer"

typedef struct ws_setting {
	const char	*key;
	int		type;		/* WS_SETTING_* */
	const char	*value;
} ws_setting_t;

int		 ws_set_settings(ws_connection_t *conn, uint32_t flags,
    const ws_setting_t *settings, unsigned count, char *failed_key,
    size_t failed_len);
int		 ws_set_setting(ws_connection_t *conn, const char *key, int type,
    const char *value);
int		 ws_get_settings(ws_connection_t *conn,
    void (*fn)(const ws_event_setting_t *setting, void *ctx), void *ctx);
int		 ws_settings_load_file(ws_connection_t *conn, const char *path,
    uint32_t flags, char *failed_key, size_t failed_len);
int		 ws_user_settings_path(char *buf, size_t len);
int		 ws_set_keyboard_layout(ws_connection_t *conn, uint32_t index);
int		 ws_list_keyboard_layouts(ws_connection_t *conn,
    void (*fn)(const ws_event_keyboard_layout_t *layout, void *ctx),
    void *ctx);
int		 ws_list_input_devices(ws_connection_t *conn,
    void (*fn)(const ws_event_input_device_t *device, void *ctx), void *ctx);

/* Names for event types and helpers, for logging. */
const char	*ws_event_name(uint16_t type);

#ifdef __cplusplus
}
#endif

#endif /* !_WINDOWSERVER_WSCLIENT_H_ */
