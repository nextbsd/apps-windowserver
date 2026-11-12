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
int		 ws_set_app_icon(ws_connection_t *conn, uint32_t width,
    uint32_t height, const void *argb, uint32_t stride);

/* Cursor: a standard kind, or WS_CURSOR_CUSTOM with a buffer. */
int		 ws_set_cursor(ws_connection_t *conn, uint32_t kind,
    ws_buffer_t *image, int32_t hot_x, int32_t hot_y);
int		 ws_set_cursor_visible(ws_connection_t *conn, int visible);

/* Activation: make this app the active app, or give it up. */
int		 ws_activate(ws_connection_t *conn);
int		 ws_deactivate(ws_connection_t *conn);

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
 *			say this Dock can show minimized windows; with
 *			MinimizeIntoDock on, WindowServer then hands them
 *			over (WS_DOCK_MINIMIZED_WINDOWS) instead of drawing
 *			tiles
 */
int		 ws_dock_activate(ws_connection_t *conn, uint32_t app_id,
    uint32_t window);
int		 ws_dock_restore(ws_connection_t *conn, uint32_t window);
int		 ws_dock_show_minimized(ws_connection_t *conn, int on);
int		 ws_dock_preview(ws_connection_t *conn, uint32_t window,
    uint32_t max_size, ws_snapshot_t *out);

/* Names for event types and helpers, for logging. */
const char	*ws_event_name(uint16_t type);

#ifdef __cplusplus
}
#endif

#endif /* !_WINDOWSERVER_WSCLIENT_H_ */
