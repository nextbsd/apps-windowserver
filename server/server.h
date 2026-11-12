/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * WindowServer internal declarations.
 */
#ifndef _WS_SERVER_H_
#define _WS_SERVER_H_

#include <sys/types.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include <pixman.h>
#include <mach/mach.h>
#include <xkbcommon/xkbcommon.h>

#include <WindowServer/WSTypes.h>
#include <WindowServer/WSProtocol.h>

/* ---- logging (log.c) ---- */

enum { WS_LOG_ERROR, WS_LOG_WARN, WS_LOG_INFO, WS_LOG_DEBUG };
extern int ws_log_level;
void	ws_log(int level, const char *fmt, ...)
	    __attribute__((format(printf, 2, 3)));
#define ws_err(...)	ws_log(WS_LOG_ERROR, __VA_ARGS__)
#define ws_warn(...)	ws_log(WS_LOG_WARN, __VA_ARGS__)
#define ws_info(...)	ws_log(WS_LOG_INFO, __VA_ARGS__)
#define ws_debug(...)	ws_log(WS_LOG_DEBUG, __VA_ARGS__)
uint64_t ws_now_usec(void);

/* Optional features this server has (WS_SERVER_* in hello and welcome). */
#define WS_SERVER_FEATURES	(WS_SERVER_JOIN | WS_SERVER_XKB | \
				 WS_SERVER_XRGB | WS_SERVER_ORDERED | \
				 WS_SERVER_MINIMIZE | WS_SERVER_DOCK | \
				 WS_SERVER_SCROLL_PHASES | WS_SERVER_OVERVIEW | \
				 WS_SERVER_DOCK_EDGE | WS_SERVER_MINIMIZE_WHERE | \
				 WS_SERVER_USABLE_AREA | WS_SERVER_SCREEN_CHANGES | \
				 WS_SERVER_SETTINGS | WS_SERVER_LATIN_SHORTCUTS | \
				 WS_SERVER_POINTER_GRAB | WS_SERVER_POINTER_WARP | \
				 WS_SERVER_WINDOW_CURSOR | WS_SERVER_INPUT_REGION | \
				 WS_SERVER_COMMIT_FRAME | WS_SERVER_WINDOW_IDS | \
				 WS_SERVER_MENU_BAR | WS_SERVER_ACTIVATE_RAISE | \
				 WS_SERVER_DOCK_NOT_ACTIVE | WS_SERVER_TILING)

/* The standard cursors with a built-in image (cursor.c): kinds 0 to
   WS_CURSOR_NOT_ALLOWED. */
#define WS_NSTD_CURSORS		(WS_CURSOR_NOT_ALLOWED + 1)

/* Connections that aren't apps: wsctl and the Dock's window list. */
#define WS_CONNECT_TOOL		(WS_CONNECT_CONTROL | WS_CONNECT_DOCK)

/* ---- configuration (config.c) ---- */

struct ws_config {
	char	keyboard_rules[32];
	char	keyboard_model[32];
	char	keyboard_layout[64];
	char	keyboard_variant[64];
	char	keyboard_options[128];
	char	keyboard_layout_shortcut[16]; /* None, ControlSpace, AltShift,
					   WinSpace, CapsLock */
	int	key_repeat_delay;	/* ms before the first repeat */
	int	key_repeat_interval;	/* ms between repeats */
	bool	click_through;		/* activating click also acts */
	bool	raise_on_click;
	bool	ignore_mux_devices;	/* skip kbdmux/sysmouse */
	bool	tap_to_click;
	bool	tap_to_drag;		/* tap, then drag (no drag lock) */
	bool	three_finger_drag;	/* moves the three-finger swipes
					   to four fingers */
	char	secondary_click[32];	/* TwoFingers, BottomRightCorner, Off */
	double	pointer_speed;		/* libinput accel speed, -1 to 1 */
	double	trackpad_speed;		/* touchpads; NAN = pointer_speed */
	double	mouse_speed;		/* other pointers; NAN = pointer_speed */
	char	mouse_accel_profile[16]; /* adaptive or flat (not touchpads) */
	bool	left_handed;		/* buttons swapped */
	double	scroll_speed;		/* scroll distance multiplier */
	bool	momentum_scrolling;
	bool	natural_scrolling;
	bool	allow_terminate;	/* Ctrl+Alt+Backspace quits */
	bool	hardware_cursor;
	uint32_t background;		/* 0xRRGGBB */
	char	drm_device[64];		/* "" = first card with a display */
	char	socket_path[WS_MAX_SOCKET_PATH];
	char	service_name[128];
	char	user[32];		/* account to drop to */
	char	group[32];
	char	snapshot_path[256];	/* SIGUSR1 writes a PPM here */
	int	minimize_tile_size;	/* minimized-window tiles, pixels */
	char	minimize_tile_image[256]; /* "" = plain dark rounded tile */
	bool	minimize_into_dock;	/* minimize requests that don't say
					   where go into the Dock */
	bool	start_key_overview;	/* a Start tap opens the overview */
	bool	reduce_motion;		/* no animations */
	/* window tiling (tiling.c) */
	char	window_mode[16];	/* stacking or tiling */
	char	tiling_layout[16];	/* dwindle or master */
	int	tiling_gaps_inner;	/* pixels between tiles */
	int	tiling_gaps_outer;	/* pixels around them */
	double	tiling_master_ratio;	/* master's share, 0.1 to 0.9 */
	int	tiling_border;		/* active-window border, 0 = none */
	uint32_t tiling_border_color;	/* 0xRRGGBB */
	char	tiling_modifier[48];	/* "Command+Option" */
};

/* What a live change to a key needs (config.c's key table). */
#define CONFIG_LIVE	0x01	/* read on every event: nothing */
#define CONFIG_DEVICE	0x02	/* applied to every input device again */
#define CONFIG_KEYMAP	0x04	/* the keymap is compiled again */
#define CONFIG_REPEAT	0x08	/* XKB clients hear the repeat values */
#define CONFIG_TILING	0x10	/* window tiling follows (tiling.c) */
#define CONFIG_SETTABLE	(CONFIG_LIVE | CONFIG_DEVICE | CONFIG_KEYMAP | \
			 CONFIG_REPEAT | CONFIG_TILING)

#define CONFIG_MAX_KEYS	64

void	config_defaults(struct ws_config *cfg);
int	config_load(struct ws_config *cfg, const char *path);
void	config_env(struct ws_config *cfg);
int	config_nkeys(void);
int	config_find(const char *name);
const char *config_key_name(int i);
unsigned config_key_flags(int i);
int	config_key_type(int i);
int	config_set_text(struct ws_config *cfg, int i, const char *value);
bool	config_get_text(const struct ws_config *cfg, int i, char *buf,
	    size_t len);
bool	config_equal(const struct ws_config *a, const struct ws_config *b,
	    int i);
void	config_copy(struct ws_config *dst, const struct ws_config *src, int i);

/* ---- objects ---- */

struct ws_server;
struct ws_client;

struct ws_buffer {
	uint32_t	id;
	struct ws_client *owner;
	int		refs;		/* client handle + attachments */
	bool		client_ref;	/* client hasn't destroyed it */
	void		*map;
	size_t		map_size;
	uint32_t	width;
	uint32_t	height;
	uint32_t	stride;
	uint32_t	scale;
	uint32_t	format;		/* WS_FORMAT_* */
	pixman_image_t	*image;
	struct ws_buffer *next;		/* owner's list */
};

struct ws_window {
	uint32_t	id;
	struct ws_client *owner;
	int32_t		level;
	uint32_t	flags;
	ws_rect_t	frame;
	char		title[WS_MAX_TITLE];
	bool		visible;
	struct ws_buffer *buffer;	/* committed */
	struct ws_buffer *pending;	/* attached, not yet committed */
	bool		pending_attach;
	pixman_region32_t pending_damage; /* window-local */
	/* WS_MSG_WINDOW_SET_CURSOR: wins over the owner's cursor */
	bool		cursor_set;
	uint32_t	cursor_kind;
	struct ws_buffer *cursor_buffer;	/* ref'd, WS_CURSOR_CUSTOM */
	int32_t		cursor_hot_x;
	int32_t		cursor_hot_y;
	/* WS_MSG_WINDOW_SET_INPUT_REGION: where it takes pointer events */
	bool		has_input_region;	/* else the whole frame */
	pixman_region32_t input_region;		/* window-local */
	bool		frame_requested;	/* commit asked for FRAME_DONE */
	bool		frame_inflight;		/* composited, waiting for present */
	/* minimizing (tile.c) */
	bool		minimized;	/* WS_MSG_WINDOW_MINIMIZE, until the
					   owner orders it in or out */
	bool		preview_dirty;	/* minimized: tell the Dock (dock.c) */
	bool		into_dock;	/* minimized into the Dock (shown
					   there while one is connected),
					   not onto the bottom row */
	struct ws_window *tile;		/* minimized: its tile in the stack,
					   NULL while the Dock shows it */
	struct ws_window *tile_for;	/* a tile: the window it shows. Tiles
					   are only in the stack, never in
					   srv->windows, and get no events. */
	pixman_image_t	*tile_image;	/* a tile: what it shows */
	bool		tile_dirty;	/* a tile: redraw tile_image */
	bool		tile_moved;	/* a tile the user dragged: it stays
					   where it was put */
	/* window tiling (tiling.c); not to be confused with the tiles of
	   minimized windows above */
	bool		tiled;		/* laid out by tiling: below the
					   windows that float on its level */
	bool		tiling_float;	/* the user floated it */
	bool		tiling_refused;	/* too big for its tile: it floats */
	unsigned	tiling_insist;	/* owner's other frames in a row */
	unsigned	tiling_drawn;	/* commits in a row of a buffer
					   another size than the frame */
	bool		tiling_own_size; /* it only draws at its own size */
	bool		tiling_follows;	/* it has drawn at a tile's size */
	uint64_t	tiling_given_usec; /* when tiling_given changed */
	bool		tiling_short;	/* its owner took a smaller frame
					   than tiling_given: it keeps it */
	bool		tiling_fresh;	/* tiled, no frame from its owner yet */
	bool		has_saved_frame;
	ws_rect_t	saved_frame;	/* its frame before it was tiled */
	ws_rect_t	tiling_given;	/* the tile frame it was last given */
	uint32_t	min_width;	/* WS_MSG_WINDOW_SET_SIZE_LIMITS, */
	uint32_t	min_height;	/* 0 = none */
	uint32_t	max_width;
	uint32_t	max_height;
	struct ws_window *above;	/* visible stack, bottom -> top */
	struct ws_window *below;
	struct ws_window *next;		/* all windows */
};

struct ws_outmsg {
	struct ws_outmsg *next;
	size_t		len;
	size_t		off;		/* bytes already sent */
	int		fd;		/* sent with the first byte */
	uint8_t		data[];
};

struct ws_client {
	int		src;		/* SRC_CLIENT, must be first */
	uint32_t	id;
	uint64_t	token;
	pid_t		pid;
	uid_t		uid;
	uint32_t	flags;		/* WS_CONNECT_* */
	/*
	 * The app this connection belongs to: itself, or the connection
	 * whose app it joined (WS_CONNECT_JOIN). srv->active and the key
	 * window are kept on that one.
	 */
	struct ws_client *app;
	char		app_name[WS_MAX_APP_NAME];
	pixman_image_t	*app_icon;	/* WS_MSG_SET_APP_ICON, or NULL */
	int		sock;		/* -1 until WS_SOCK_AUTH */
	uint64_t	hello_usec;
	bool		dead;
	bool		doomed;		/* destroy at the end of this loop pass */
	bool		dock_wants_minimized; /* WS_MSG_DOCK_SHOW_MINIMIZED */
	uint32_t	dock_edge;	/* WS_MSG_DOCK_SET_EDGE: WS_DOCK_EDGE_* */
	uint32_t	dock_thickness;
	uint64_t	dock_edge_serial; /* when it said so (newest wins) */
	/* WS_MSG_SET_MENU_BAR, on c->app only: the bar's height, or -1
	   if the app never said */
	int32_t		menu_bar;
	struct ws_buffer *buffers;
	struct ws_window *key_window;	/* app's key window (kept while
					   inactive); set on c->app only, and
					   may belong to another connection
					   of the app */
	/* cursor this app wants while the pointer is over its windows */
	uint32_t	cursor_kind;
	struct ws_buffer *cursor_buffer;
	int32_t		cursor_hot_x;
	int32_t		cursor_hot_y;
	bool		cursor_hidden;
	bool		pointer_track;	/* WS_MSG_POINTER_TRACK */
	/* WS_MSG_RESERVE_WINDOW_IDS: ids it may name, from wid_next on */
	uint32_t	wid_next;
	uint32_t	wid_end;	/* first id past the block */
	/* stream input not yet a whole message, and received fds */
	uint8_t		rbuf[512];
	size_t		rlen;
	int		fds[8];
	int		nfds;
	/* events the socket couldn't take yet */
	struct ws_outmsg *outq;
	struct ws_outmsg **outq_tail;
	size_t		outq_len;
	struct ws_client *next;
};

/* A socket connection that hasn't sent WS_SOCK_AUTH yet. */
struct ws_conn {
	int		src;		/* SRC_CONN, must be first */
	int		fd;
	uint64_t	accepted_usec;
	bool		dead;
	struct ws_conn	*next;
};

/* kqueue udata tags */
enum ws_src_type {
	SRC_MACH = 1,
	SRC_LISTEN,
	SRC_CONN,
	SRC_CLIENT,
	SRC_INPUT,
	SRC_DRM,
	SRC_TTY,
};

enum {
	TIMER_REPEAT = 1,
	TIMER_HOUSEKEEPING,
	TIMER_MOMENTUM,
	TIMER_OVERVIEW,
};

/* The window overview (overview.c). */
struct ws_overview_item {
	uint32_t	window;
	uint32_t	app_id;
	unsigned	group;
	ws_rect_t	to;		/* place in the overview */
	ws_rect_t	icon;		/* its group's icon (first item only) */
	pixman_image_t	*cache;		/* the preview at `to`, box-filtered */
	bool		cache_dirty;
};

struct ws_overview {
	int		state;		/* WS_OVERVIEW_STATE_* */
	int		mode;		/* WS_OVERVIEW_ALL or _APP */
	uint32_t	app_id;		/* App Exposé's app */
	struct ws_overview_item *items;
	unsigned	nitems;
	int		selected;	/* item index, -1 = none */
	bool		dirty;		/* windows changed: lay out again */
	double		t;		/* 0 closed .. 1 open */
	double		t_start;	/* t when the animation started */
	uint64_t	anim_start;
	uint32_t	buttons;	/* pointer buttons pressed while open */
	/* touchpad swipe in progress */
	bool		swiping;
	bool		swipe_fired;
	double		swipe_dx;
	double		swipe_dy;
};

/* Window tiling (tiling.c). */
struct ws_tiling_slot {
	struct ws_window *w;
	double		ratio;		/* dwindle: share of the area left
					   that this slot takes */
	double		weight;		/* master: share of the stack */
	ws_rect_t	tile;		/* where it goes, gaps left out */
};

enum { SPLIT_RATIO, SPLIT_MASTER, SPLIT_STACK };

struct ws_tiling_split {
	int		kind;		/* SPLIT_* */
	unsigned	slot;		/* RATIO: the slot whose ratio it is;
					   STACK: the slot before the line */
	bool		vertical;	/* a vertical line: sides left, right */
	ws_rect_t	area;		/* both sides and the gap */
	int32_t		pos;		/* where the first side ends */
	ws_rect_t	handle;		/* where a press takes it */
};

/*
 * The windows tiled in one area. There is one, for the screen; once
 * there are workspaces (apps-windowserver#13) each gets its own.
 */
struct ws_tiling_space {
	struct ws_tiling_slot *slots;	/* in layout order */
	unsigned	nslots;
	unsigned	cap;
	struct ws_tiling_split *splits;	/* cap of them */
	unsigned	nsplits;
	double		master_ratio;
	ws_rect_t	area;		/* usable area minus the outer gaps */
};

struct ws_tiling {
	bool		on;		/* WindowMode is tiling */
	int		layout;		/* WS_TILING_DWINDLE or _MASTER */
	uint32_t	modifier;	/* WS_MOD_* of TilingModifier */
	bool		dirty;		/* lay out again before the next frame */
	struct ws_tiling_space space;
	struct ws_window *focus;	/* tiled window that had the focus */
	struct ws_window *border;	/* has the border drawn round it */
	ws_rect_t	border_rect;
	int		drag_split;	/* split being dragged, -1 = none */
	int		hover_split;	/* split under the pointer, -1 = none */
	struct ws_window *moving;	/* tiled window the user drags */
};

/* Touchpad scrolling in progress (scroll.c). */
#define WS_SCROLL_SAMPLES	8

struct ws_scroll_state {
	bool		fingers;	/* a finger scroll is under way */
	bool		momentum;	/* momentum scrolling is under way */
	bool		momentum_began;	/* its BEGAN event went out */
	uint32_t	window;		/* where this scroll goes */
	uint64_t	last_usec;	/* last finger scroll or momentum step */
	double		vx;		/* momentum velocity, pixels/s */
	double		vy;
	uint64_t	momentum_start;
	/* recent finger deltas, for the velocity at lift-off */
	unsigned	nsamples;
	unsigned	next_sample;
	struct {
		uint64_t usec;
		double	dt;		/* seconds since the sample before */
		double	dx;
		double	dy;
	} samples[WS_SCROLL_SAMPLES];
};

/* ---- output backends ---- */

struct ws_output_ops {
	const char *name;
	/* Push the damaged part of srv->shadow to the screen. Returns true
	 * if the frame is presented asynchronously (flip pending). */
	bool	(*present)(struct ws_server *srv, pixman_region32_t *damage);
	/* Hardware cursor: return false to fall back to software. */
	bool	(*cursor_set)(struct ws_server *srv, pixman_image_t *img,
		    int hot_x, int hot_y);
	void	(*cursor_move)(struct ws_server *srv, int x, int y);
	void	(*cursor_hide)(struct ws_server *srv);
	void	(*shutdown)(struct ws_server *srv);
};

struct ws_mach_item {
	struct ws_mach_item *next;
	size_t		size;
	pid_t		sender_pid;
	uid_t		sender_euid;
	union {
		mach_msg_header_t head;
		uint8_t		buf[WS_MAX_REQUEST_SIZE +
				    sizeof(mach_msg_max_trailer_t)];
	} msg;
};

/* An input device libinput reported (input.c). */
struct ws_input_device {
	struct libinput_device *dev;	/* ref'd */
	uint32_t	flags;		/* WS_INPUT_* */
	struct ws_input_device *next;
};

struct ws_server {
	struct ws_config cfg;		/* in effect: base + session */
	/* settings layers (settings.c) */
	struct ws_config base;		/* the system's */
	struct ws_config session;	/* the console user's values ... */
	bool		session_set[CONFIG_MAX_KEYS]; /* ... for these keys */
	uid_t		session_uid;	/* console user they belong to,
					   -1 = nobody logged in */
	const char	*config_path;	/* the system plist */
	const char	*opt_socket;	/* command line */
	const char	*opt_service;
	bool		headless;
	bool		running;
	int		kq;

	/* screen */
	uint32_t	width;
	uint32_t	height;
	uint32_t	refresh_mhz;
	uint32_t	mm_width;
	uint32_t	mm_height;
	char		screen_name[32];
	pixman_image_t	*shadow;	/* composited screen (x8r8g8b8) */
	pixman_region32_t damage;	/* screen damage not yet composited */
	const struct ws_output_ops *output;
	void		*output_data;
	bool		present_pending;	/* waiting for page flip */
	bool		display_active;		/* we own the VT and DRM master */

	/* Mach */
	mach_port_t	service_port;
	pthread_t	mach_thread;
	pthread_mutex_t	mach_lock;
	struct ws_mach_item *mach_head;
	struct ws_mach_item **mach_tail;
	int		mach_pipe[2];

	/* socket */
	int		listen_fd;
	char		socket_path[WS_MAX_SOCKET_PATH];
	char		socket_dir[WS_MAX_SOCKET_PATH];
	bool		socket_dir_created;
	struct ws_conn	*conns;

	/* objects */
	struct ws_client *clients;
	struct ws_window *windows;
	struct ws_window *stack_bottom;
	struct ws_window *stack_top;
	uint32_t	next_client_id;
	uint32_t	next_window_id;
	uint32_t	event_serial;
	struct ws_client *active;	/* an app (client->app), or NULL */
	/*
	 * The Dock's app while the user works in it (a click on a tile or
	 * in a Dock popup, or it activated itself), or NULL. The Dock never
	 * becomes the active app, as on Darwin: srv->active keeps its menus
	 * and Command keys, and the Dock's app hears WS_EVENT_APP_ACTIVATED
	 * and _DEACTIVATED for this instead. Keys go to its key window, if
	 * it has a visible one (a Dock popup with a text field).
	 */
	struct ws_client *dock_front;

	/* pointer */
	double		ptr_x;
	double		ptr_y;
	uint32_t	buttons;	/* mask */
	struct ws_window *grab;		/* implicit grab while buttons held */
	bool		grab_suppressed;	/* activating click swallowed */
	/* WS_MSG_POINTER_GRAB: the connection holding it, or NULL */
	struct ws_client *pgrab;
	uint32_t	pgrab_mode;	/* WS_GRAB_* */
	struct ws_window *hover;
	uint64_t	last_click_usec;
	uint32_t	last_click_button;
	double		last_click_x;
	double		last_click_y;
	uint32_t	click_count;
	/* minimized-window tiles (tile.c) */
	struct ws_window *tile_selected;
	bool		tile_dragging;
	double		tile_grab_x;	/* pointer offset in the grabbed tile */
	double		tile_grab_y;
	/* touchpad scrolling (scroll.c) */
	struct ws_scroll_state scroll;
	/* the window overview (overview.c) */
	struct ws_overview ov;
	pixman_image_t	*tile_background; /* MinimizeTileImage, or NULL */
	pixman_image_t	*tile_mask_outer; /* rounded tile shapes (a8) */
	pixman_image_t	*tile_mask_inner;
	/* the Dock (dock.c) */
	struct ws_client *dock;		/* shows minimized windows, or NULL */
	bool		dock_list_dirty;	/* send WINDOW_LIST_CHANGED */
	bool		dock_previews_dirty;	/* some window->preview_dirty */
	/*
	 * The screen edge the Dock keeps (WS_MSG_DOCK_SET_EDGE): the newest
	 * report of a Dock connection still connected. Window tiling
	 * (apps-windowserver#20) keeps this strip of the screen free.
	 */
	uint32_t	dock_edge;		/* WS_DOCK_EDGE_* */
	uint32_t	dock_thickness;		/* pixels from that edge */
	uint32_t	dock_edge_client;	/* client id, 0 = none */
	uint64_t	dock_edge_serial;
	/*
	 * The menu bar strip along the top (screen.c, WS_MSG_SET_MENU_BAR):
	 * its height, and the app whose report set it (NULL = none).
	 */
	uint32_t	menu_bar;
	struct ws_client *menu_bar_app;
	/* The usable area (screen.c): the screen minus the menu bar's and
	   the Dock's strips, and the menu bar height last sent with it. */
	ws_rect_t	usable;
	uint32_t	usable_menu_bar;
	/* window tiling (tiling.c) */
	struct ws_tiling tiling;

	/* cursor */
	pixman_image_t	*cursor_std[WS_NSTD_CURSORS]; /* built-in kinds */
	pixman_image_t	*cursor_image;	/* current, not owned */
	int		cursor_hot_x;
	int		cursor_hot_y;
	bool		cursor_visible;
	bool		cursor_hw;	/* hardware plane in use */
	ws_rect_t	cursor_rect;	/* software cursor on screen */

	/* keyboard (keyboard.c) */
	struct xkb_context *xkb_ctx;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;
	uint32_t	modifiers;
	uint32_t	repeat_key;	/* evdev code, 0 = none */
	bool		repeat_started;
	int		keymap_fd;	/* sealed memfd with the keymap, or -1 */
	uint32_t	keymap_size;	/* bytes, NUL included */
	uint32_t	keys_held[WS_MAX_KEYS_HELD];	/* XKB keycodes */
	unsigned	nkeys_held;
	struct ws_client *kbd_focus;	/* gets key events (not ref'd) */
	/*
	 * Keys whose press, with Command held, went to the active app's own
	 * connection instead of a joined connection's key window: their
	 * repeats and releases go there too.
	 */
	uint32_t	equiv_keys[WS_MAX_KEYS_HELD];	/* XKB keycodes */
	unsigned	nequiv_keys;
	/* keys whose press the server took: their release is its too */
	uint32_t	server_keys[WS_MAX_KEYS_HELD];	/* evdev codes */
	unsigned	nserver_keys;
	uint32_t	start_tap;	/* Start key down on its own, 0 = no */
	uint32_t	layout_active;	/* the active layout (XKB group) */
	uint32_t	nlayouts;
	char		layouts[WS_MAX_KEYBOARD_LAYOUTS][32];	/* "us", "gr" */
	char		variants[WS_MAX_KEYBOARD_LAYOUTS][32];
	/*
	 * Command shortcuts: while a layout that isn't Latin is active, a
	 * key pressed with Command held takes its keysym and text from
	 * the first Latin layout in the list, or a "us" keymap compiled
	 * for this when there is none (latin_keymap).
	 */
	uint32_t	layouts_latin;	/* bit i: layout i is Latin */
	uint32_t	latin_layout;	/* its index in latin_keymap */
	struct xkb_keymap *latin_keymap;	/* xkb_keymap, or "us" */
	struct xkb_state *latin_state;

	/* input (input.c) */
	struct libinput	*libinput;
	struct udev	*udev;
	struct ws_input_device *input_devices;

	/* VT (vt.c) */
	int		tty_fd;
	int		vt_num;		/* the VT that was active at start */
	bool		vt_setup;

	/* privileges */
	uid_t		run_uid;
	gid_t		run_gid;
};

/* ---- main.c ---- */
void	ws_kq_add_fd(struct ws_server *srv, int fd, int filter, void *udata);
void	ws_kq_del_fd(struct ws_server *srv, int fd, int filter);
void	ws_timer_set(struct ws_server *srv, int ident, int ms, bool oneshot);
void	ws_timer_cancel(struct ws_server *srv, int ident);
void	ws_quit(struct ws_server *srv);

/* ---- mach.c ---- */
int	mach_ipc_start(struct ws_server *srv);
void	mach_ipc_drain(struct ws_server *srv);

/* ---- client.c ---- */
int	socket_listen(struct ws_server *srv);
void	socket_cleanup(struct ws_server *srv);
void	socket_accept(struct ws_server *srv);
void	conn_readable(struct ws_server *srv, struct ws_conn *conn);
void	client_readable(struct ws_server *srv, struct ws_client *c);
void	client_writable(struct ws_server *srv, struct ws_client *c);
void	client_destroy(struct ws_server *srv, struct ws_client *c);
void	clients_reap(struct ws_server *srv);
void	clients_housekeeping(struct ws_server *srv);
struct ws_client *client_lookup(struct ws_server *srv, uint32_t id);
struct ws_buffer *client_buffer(struct ws_client *c, uint32_t id);
void	buffer_unref(struct ws_server *srv, struct ws_buffer *b);
void	client_send(struct ws_server *srv, struct ws_client *c,
	    ws_event_t *ev, int fd);
void	client_error(struct ws_server *srv, struct ws_client *c,
	    uint32_t request, uint32_t window, int code, const char *msg);
void	event_init(struct ws_server *srv, ws_event_t *ev, uint16_t type,
	    size_t payload, uint32_t window);
void	screen_info(struct ws_server *srv, ws_screen_info_t *si);

/* ---- screen.c ---- */
void	screen_usable_compute(struct ws_server *srv, ws_rect_t *r);
void	screen_usable_update(struct ws_server *srv);
int	menu_bar_set(struct ws_server *srv, struct ws_client *c,
	    uint32_t height);
void	menu_bar_update(struct ws_server *srv);
void	menu_bar_app_gone(struct ws_server *srv, struct ws_client *c,
	    struct ws_client *heir);
int	screen_resize(struct ws_server *srv, uint32_t width, uint32_t height);
void	screen_changed(struct ws_server *srv);

/* ---- request.c ---- */
void	request_handle(struct ws_server *srv, struct ws_mach_item *item);

/* ---- window.c ---- */
struct ws_window *window_create(struct ws_server *srv, struct ws_client *c,
	    const ws_rect_t *frame, int32_t level, uint32_t flags,
	    const char *title, uint32_t id);
void	window_destroy(struct ws_server *srv, struct ws_window *w);
struct ws_window *window_lookup(struct ws_server *srv, uint32_t id);
void	window_order(struct ws_server *srv, struct ws_window *w, int op,
	    struct ws_window *rel);
void	window_order_by_server(struct ws_server *srv, struct ws_window *w,
	    int op, struct ws_window *rel);
void	window_set_level(struct ws_server *srv, struct ws_window *w,
	    int32_t level);
void	window_set_frame(struct ws_server *srv, struct ws_window *w,
	    const ws_rect_t *frame);
void	window_attach(struct ws_server *srv, struct ws_window *w,
	    struct ws_buffer *b);
void	window_damage(struct ws_server *srv, struct ws_window *w,
	    const ws_rect_t *rects, unsigned n);
void	window_commit(struct ws_server *srv, struct ws_window *w,
	    uint32_t flags, const ws_rect_t *frame);
void	window_make_key(struct ws_server *srv, struct ws_window *w);
struct ws_window *window_at(struct ws_server *srv, double x, double y);
struct ws_window *window_at_owned(struct ws_server *srv, struct ws_client *c,
	    double x, double y);
void	window_set_input_region(struct ws_server *srv, struct ws_window *w,
	    const ws_rect_t *rects, unsigned n, uint32_t flags);
int	window_reserve_ids(struct ws_server *srv, struct ws_client *c,
	    uint32_t count, uint32_t *base);
int	window_create_check_id(struct ws_server *srv, struct ws_client *c,
	    uint32_t id);
void	app_activate(struct ws_server *srv, struct ws_client *c,
	    struct ws_window *key);
void	app_deactivate(struct ws_server *srv, struct ws_client *c);
void	app_dock_front_end(struct ws_server *srv, bool tell);
bool	app_has_focus(struct ws_server *srv, struct ws_client *c);
void	app_member_gone(struct ws_server *srv, struct ws_client *c);
void	damage_rect(struct ws_server *srv, const ws_rect_t *r);
void	damage_all(struct ws_server *srv);
void	list_windows(struct ws_server *srv, struct ws_client *to,
	    uint32_t options);
void	window_minimize(struct ws_server *srv, struct ws_window *w,
	    int32_t where);
void	window_tile_add(struct ws_server *srv, struct ws_window *w);
void	window_tile_remove(struct ws_server *srv, struct ws_window *w);
void	window_request_restore(struct ws_server *srv, struct ws_window *w);
void	window_tile_move(struct ws_server *srv, struct ws_window *t,
	    int32_t x, int32_t y);
void	window_tile_raise(struct ws_server *srv, struct ws_window *t);
void	window_tiles_place(struct ws_server *srv);
void	windows_keep_on_screen(struct ws_server *srv, uint32_t old_width,
	    uint32_t old_height);
void	window_restack(struct ws_server *srv, struct ws_window *w);
void	window_server_frame(struct ws_server *srv, struct ws_window *w,
	    const ws_rect_t *frame);

/* ---- tiling.c ---- */
void	tiling_init(struct ws_server *srv);
void	tiling_fini(struct ws_server *srv);
void	tiling_config_changed(struct ws_server *srv,
	    const struct ws_config *old);
bool	tiling_modifier_parse(const char *s, uint32_t *mods);
void	tiling_changed(struct ws_server *srv);
void	tiling_window_gone(struct ws_server *srv, struct ws_window *w);
void	tiling_client_frame(struct ws_server *srv, struct ws_window *w);
void	tiling_committed(struct ws_server *srv, struct ws_window *w);
void	tiling_set_size_limits(struct ws_server *srv, struct ws_window *w,
	    const ws_req_size_limits_t *m);
void	tiling_flush(struct ws_server *srv);
bool	tiling_key(struct ws_server *srv, xkb_keysym_t sym);
bool	tiling_press(struct ws_server *srv, uint32_t button);
bool	tiling_release(struct ws_server *srv, uint32_t button);
bool	tiling_motion(struct ws_server *srv);
void	tiling_grab_end(struct ws_server *srv, struct ws_window *w);
int	tiling_cursor(struct ws_server *srv);
void	tiling_draw_border(struct ws_server *srv, struct ws_window *w);
void	tiling_request(struct ws_server *srv, struct ws_client *c,
	    uint32_t op, uint32_t window, int32_t arg);

/* ---- tile.c ---- */
int	tile_init(struct ws_server *srv);
void	tile_fini(struct ws_server *srv);
void	tile_free(struct ws_window *t);
void	tiles_render(struct ws_server *srv);
void	tiles_redraw_app(struct ws_server *srv, struct ws_client *c);
void	tile_select(struct ws_server *srv, struct ws_window *t);
void	tile_press(struct ws_server *srv, struct ws_window *t, uint32_t button,
	    uint32_t count);
void	tile_drag(struct ws_server *srv, struct ws_window *t);
int	tile_set_app_icon(struct ws_server *srv, struct ws_client *c,
	    struct ws_buffer *b);

/* ---- dock.c ---- */
bool	dock_may_list(struct ws_server *srv, uid_t uid);
bool	dock_owns_app(struct ws_server *srv, const struct ws_client *a);
uid_t	console_uid(void);
bool	dock_shows_minimized(struct ws_server *srv,
	    const struct ws_window *w);
void	dock_list_changed(struct ws_server *srv);
void	dock_preview_changed(struct ws_server *srv, struct ws_window *w);
void	dock_client_ready(struct ws_server *srv, struct ws_client *c);
void	dock_client_gone(struct ws_server *srv, struct ws_client *c);
void	dock_flush(struct ws_server *srv);
void	dock_show_minimized(struct ws_server *srv, struct ws_client *c,
	    bool on);
int	dock_set_edge(struct ws_server *srv, struct ws_client *c,
	    uint32_t edge, uint32_t thickness);
void	raise_app(struct ws_server *srv, struct ws_client *a);
void	dock_activate(struct ws_server *srv, struct ws_client *c,
	    uint32_t window, uint32_t app);
void	dock_restore(struct ws_server *srv, struct ws_client *c,
	    uint32_t window);
int	dock_preview_memfd(struct ws_server *srv, struct ws_window *w,
	    int max, uint32_t *width, uint32_t *height, uint32_t *stride);

/* ---- tile.c (previews) ---- */
void	tile_scale(pixman_image_t *src, pixman_image_t *dst, int dx, int dy,
	    int dw, int dh, int sw, int sh);

/* ---- overview.c ---- */
bool	overview_active(struct ws_server *srv);
void	overview_open(struct ws_server *srv, int mode);
void	overview_close(struct ws_server *srv);
void	overview_toggle(struct ws_server *srv, int mode);
void	overview_anim_fire(struct ws_server *srv);
void	overview_flush(struct ws_server *srv);
void	overview_composite(struct ws_server *srv, pixman_region32_t *region);
void	overview_window_committed(struct ws_server *srv, struct ws_window *w);
void	overview_motion(struct ws_server *srv);
void	overview_button(struct ws_server *srv, uint32_t button);
bool	overview_key(struct ws_server *srv, uint32_t code, xkb_keysym_t sym);
void	overview_swipe(struct ws_server *srv, int phase, unsigned fingers,
	    double dx, double dy);
void	overview_report(struct ws_server *srv, struct ws_client *to);
void	overview_fini(struct ws_server *srv);
void	app_bring_forward(struct ws_server *srv, struct ws_window *w);

/* ---- pointer.c ---- */
void	pointer_motion(struct ws_server *srv, double x, double y);
void	pointer_button(struct ws_server *srv, uint32_t button, bool pressed);
int	pointer_grab(struct ws_server *srv, struct ws_client *c,
	    uint32_t mode);
void	pointer_ungrab(struct ws_server *srv, struct ws_client *c,
	    bool notify);
int	pointer_warp(struct ws_server *srv, struct ws_client *c,
	    struct ws_window *w, double x, double y);
int	pointer_query(struct ws_server *srv, struct ws_client *c,
	    ws_pointer_info_t *info);
void	pointer_scroll(struct ws_server *srv, double dx, double dy,
	    int32_t v120_x, int32_t v120_y, uint32_t source);
struct ws_window *pointer_scroll_target(struct ws_server *srv);
void	pointer_scroll_send(struct ws_server *srv, struct ws_window *w,
	    double dx, double dy, int32_t v120_x, int32_t v120_y,
	    uint32_t source, uint32_t phase, uint32_t momentum_phase);

/* ---- scroll.c ---- */
void	scroll_finger(struct ws_server *srv, double dx, double dy,
	    bool stop, uint64_t usec);
void	scroll_touch(struct ws_server *srv);
void	scroll_momentum_fire(struct ws_server *srv);
void	scroll_window_gone(struct ws_server *srv, struct ws_window *w);
void	pointer_window_gone(struct ws_server *srv, struct ws_window *w);
void	pointer_recheck(struct ws_server *srv);

/* ---- settings.c ---- */
void	settings_init(struct ws_server *srv);
int	settings_reload_system(struct ws_server *srv);
void	settings_housekeeping(struct ws_server *srv);
void	settings_set(struct ws_server *srv, struct ws_client *c,
	    struct ws_mach_item *item);
void	settings_get(struct ws_server *srv, struct ws_client *c);
void	settings_tools_notify(struct ws_server *srv, ws_event_t *ev);
void	config_load_system(struct ws_server *srv, struct ws_config *cfg);

/* ---- keyboard.c ---- */
int	keyboard_init(struct ws_server *srv);
int	keyboard_check(struct ws_server *srv, const struct ws_config *cfg);
bool	keyboard_shortcut_valid(const char *name);
int	keyboard_reload(struct ws_server *srv);
void	keyboard_repeat_changed(struct ws_server *srv);
int	keyboard_set_layout(struct ws_server *srv, uint32_t index);
void	keyboard_list_layouts(struct ws_server *srv, struct ws_client *c);
void	keyboard_fini(struct ws_server *srv);
void	keyboard_key(struct ws_server *srv, uint32_t evdev_code, bool pressed);
void	keyboard_repeat_fire(struct ws_server *srv);
int	keyboard_inject(struct ws_server *srv, const char *name, int mode);
void	keyboard_client_ready(struct ws_server *srv, struct ws_client *c);
void	keyboard_focus_update(struct ws_server *srv);

/* ---- cursor.c ---- */
int	cursor_init(struct ws_server *srv);
void	cursor_fini(struct ws_server *srv);
void	cursor_update(struct ws_server *srv);
void	cursor_moved(struct ws_server *srv);
void	cursor_buffer_gone(struct ws_server *srv, struct ws_buffer *b);
int	cursor_window_set(struct ws_server *srv, struct ws_window *w,
	    uint32_t kind, struct ws_buffer *b, int32_t hot_x, int32_t hot_y);

/* ---- compositor.c ---- */
int	compositor_init(struct ws_server *srv);
int	compositor_resize(struct ws_server *srv, uint32_t width,
	    uint32_t height);
void	compositor_fini(struct ws_server *srv);
void	compositor_repaint(struct ws_server *srv);
void	composite_window(struct ws_server *srv, struct ws_window *w,
	    const pixman_box32_t *ext, double opacity);
void	compositor_present_done(struct ws_server *srv);
int	snapshot_memfd(struct ws_server *srv, uint32_t *stride);
int	snapshot_write_ppm(struct ws_server *srv, const char *path);

/* ---- headless output (compositor.c) ---- */
extern const struct ws_output_ops headless_output_ops;

/* ---- drm.c ---- */
int	drm_open(struct ws_server *srv);
int	drm_start(struct ws_server *srv);
void	drm_readable(struct ws_server *srv);
extern const struct ws_output_ops drm_output_ops;

/* ---- vt.c ---- */
int	vt_open(struct ws_server *srv);
void	vt_close(struct ws_server *srv);
void	vt_signal(struct ws_server *srv);
void	vt_drain(struct ws_server *srv);
void	vt_install_crash_handlers(struct ws_server *srv);

/* ---- input.c ---- */
int	input_open(struct ws_server *srv);
void	input_readable(struct ws_server *srv);
void	input_reconfigure(struct ws_server *srv);
void	input_list(struct ws_server *srv, struct ws_client *c);
void	input_close(struct ws_server *srv);

/* ---- privs.c ---- */
int	privs_drop(struct ws_server *srv);

#endif /* !_WS_SERVER_H_ */
