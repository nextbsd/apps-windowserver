/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * wsctl: inspect and drive WindowServer from the command line.
 * Snapshots and synthetic input are allowed when the server runs
 * headless, or for root and the server's own user. `wsctl dock ...`
 * connects as a Dock (WS_CONNECT_DOCK) to try the Dock's requests.
 * `wsctl settings` and `wsctl layout` change settings while the server
 * runs, which only the console user may do. `wsctl menubar` connects as
 * an app with a Mac-style menu bar.
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <WindowServer/WindowServer.h>

static void
usage(void)
{
	fprintf(stderr,
	    "usage: wsctl list\n"
	    "       wsctl snapshot FILE.ppm\n"
	    "       wsctl pixel X Y [X Y ...]\n"
	    "       wsctl move X Y\n"
	    "       wsctl click X Y [left|right|middle]\n"
	    "       wsctl down|up [left|right|middle]\n"
	    "       wsctl key NAME          (e.g. a, Return, shift+a, ctrl+q)\n"
	    "       wsctl keydown|keyup NAME\n"
	    "       wsctl scroll DX DY [finger]\n"
	    "       wsctl flick DX DY [STEPS]  (two-finger scroll, then lift)\n"
	    "       wsctl hold              (fingers rest on the touchpad)\n"
	    "       wsctl swipe FINGERS DX DY [STEPS]  (touchpad swipe)\n"
	    "       wsctl overview [all|app|close|status]\n"
	    "       wsctl screen            (size and usable area)\n"
	    "       wsctl screen size WxH   (headless: change the screen size)\n"
	    "       wsctl screen watch [SECONDS] (print screen and usable-area\n"
	    "                                changes)\n"
	    "       wsctl sync\n"
	    "       wsctl menubar HEIGHT [SECONDS] [inactive]\n"
	    "                                (an app with a menu bar HEIGHT\n"
	    "                                pixels high, active unless\n"
	    "                                inactive; stay SECONDS)\n"
	    "       wsctl dock list [-a]     (every window, with ids; -a: hidden too)\n"
	    "       wsctl dock activate APP_ID [WINDOW]\n"
	    "       wsctl dock restore WINDOW\n"
	    "       wsctl dock preview WINDOW SIZE FILE.ppm\n"
	    "       wsctl dock watch [SECONDS] (stay the Dock, print notices)\n"
	    "       wsctl dock edge left|right|top|bottom|none THICKNESS [SECONDS]\n"
	    "                                (report a Dock edge, stay SECONDS)\n"
	    "       wsctl settings get [KEY]   (values in effect, and where from)\n"
	    "       wsctl settings set KEY VALUE [KEY VALUE ...]\n"
	    "       wsctl settings unset KEY [KEY ...] (back to the system's)\n"
	    "       wsctl settings reload [FILE] (the user's org.nextbsd.WindowServer\n"
	    "                                defaults replace the session's)\n"
	    "       wsctl settings reset     (drop the session's values)\n"
	    "       wsctl layout list\n"
	    "       wsctl layout set N|NAME  (make a layout of the list active)\n"
	    "       wsctl layout watch [SECONDS] (print layout and settings changes)\n"
	    "       wsctl devices           (input devices)\n"
	    "       wsctl tiling [status]   (the layout: tiled windows, in order)\n"
	    "       wsctl tiling on|off     (WindowMode tiling or stacking)\n"
	    "       wsctl tiling layout dwindle|master\n"
	    "       wsctl tiling gaps INNER [OUTER]\n"
	    "       wsctl tiling border PIXELS [RRGGBB]\n"
	    "       wsctl tiling focus|swap left|right|up|down\n"
	    "       wsctl tiling grow|shrink (the focused tile)\n"
	    "       wsctl tiling float|tile|toggle [WINDOW] (the key window\n"
	    "                                if none is given)\n");
	exit(2);
}

static uint32_t
button(const char *s)
{
	if (s == NULL || strcmp(s, "left") == 0)
		return WS_BUTTON_LEFT;
	if (strcmp(s, "right") == 0)
		return WS_BUTTON_RIGHT;
	if (strcmp(s, "middle") == 0)
		return WS_BUTTON_MIDDLE;
	return (uint32_t)strtoul(s, NULL, 0);
}

static void
print_window(const ws_event_window_info_t *w, void *ctx)
{
	(void)ctx;
	printf("%-3u %-8s pid=%-6d level=%-5d frame=%d,%d,%dx%d%s%s%s%s \"%s\"\n",
	    w->stack_index, w->app_name, w->pid, w->level, w->frame.x,
	    w->frame.y, w->frame.width, w->frame.height,
	    (w->flags & WS_WINDOW_INFO_MINIMIZED) ? " minimized" : "",
	    (w->flags & WS_WINDOW_INFO_ACTIVE) ? " active" : "",
	    (w->flags & WS_WINDOW_INFO_KEY) ? " key" : "",
	    (w->flags & WS_WINDOW_INFO_TILED) ? " tiled" : "", w->title);
}

static void
print_dock_window(const ws_event_window_info_t *w, void *ctx)
{
	printf("%sid=%u app=%u pid=%d %s level=%d frame=%d,%d,%dx%d%s%s%s%s%s "
	    "\"%s\"\n", ctx != NULL ? (const char *)ctx : "", w->window,
	    w->app_id,
	    w->pid, w->app_name, w->level, w->frame.x, w->frame.y,
	    w->frame.width, w->frame.height,
	    (w->flags & WS_WINDOW_INFO_VISIBLE) ? " visible" : "",
	    (w->flags & WS_WINDOW_INFO_MINIMIZED) ? " minimized" : "",
	    (w->flags & WS_WINDOW_INFO_IN_DOCK) ? " in-dock" : "",
	    (w->flags & WS_WINDOW_INFO_ACTIVE) ? " active" : "",
	    (w->flags & WS_WINDOW_INFO_KEY) ? " key" : "", w->title);
}

static int
write_ppm(const ws_snapshot_t *s, const char *path)
{
	unsigned char *row;
	uint32_t x, y;
	FILE *f;

	f = fopen(path, "wb");
	if (f == NULL)
		return -1;
	row = malloc((size_t)s->width * 3);
	if (row == NULL) {
		fclose(f);
		return -1;
	}
	fprintf(f, "P6\n%u %u\n255\n", s->width, s->height);
	for (y = 0; y < s->height; y++) {
		const uint32_t *px = (const uint32_t *)(const void *)
		    ((const char *)s->pixels + (size_t)y * s->stride);

		for (x = 0; x < s->width; x++) {
			row[x * 3] = (unsigned char)(px[x] >> 16);
			row[x * 3 + 1] = (unsigned char)(px[x] >> 8);
			row[x * 3 + 2] = (unsigned char)px[x];
		}
		fwrite(row, 3, s->width, f);
	}
	free(row);
	return fclose(f);
}

static const char *const ov_states[] = { "closed", "opening", "open",
    "closing" };

static void
print_overview_item(const ws_event_overview_item_t *it, void *ctx)
{
	(void)ctx;
	printf("item window=%u app=%u group=%u frame=%d,%d,%dx%d "
	    "icon=%d,%d,%dx%d%s%s\n", it->window, it->app_id, it->group,
	    it->frame.x, it->frame.y, it->frame.width, it->frame.height,
	    it->icon.x, it->icon.y, it->icon.width, it->icon.height,
	    (it->flags & WS_OVERVIEW_ITEM_SELECTED) ? " selected" : "",
	    (it->flags & WS_OVERVIEW_ITEM_MINIMIZED) ? " minimized" : "");
}

/*
 * wsctl overview [all|app|close|status]: open, close or just look, then
 * print the state and the windows shown. Opening and closing wait for
 * the animation to end (status doesn't).
 */
static int
overview_cmd(ws_connection_t *c, const char *what)
{
	ws_event_overview_t st;
	struct timespec ts = { 0, 20 * 1000 * 1000 };
	uint32_t req = WS_OVERVIEW_QUERY;
	int r, i;

	if ((ws_server_flags(c) & WS_SERVER_OVERVIEW) == 0) {
		fprintf(stderr, "wsctl: this server has no overview\n");
		errno = ENOTSUP;
		return -1;
	}
	if (what == NULL || strcmp(what, "all") == 0)
		req = WS_OVERVIEW_ALL;
	else if (strcmp(what, "app") == 0)
		req = WS_OVERVIEW_APP;
	else if (strcmp(what, "close") == 0)
		req = WS_OVERVIEW_CLOSE;
	else if (strcmp(what, "status") == 0)
		req = WS_OVERVIEW_QUERY;
	else
		usage();
	memset(&st, 0, sizeof(st));
	r = ws_overview(c, req, &st, NULL, NULL);
	for (i = 0; r == 0 && req != WS_OVERVIEW_QUERY && i < 100 &&
	    (st.state == WS_OVERVIEW_STATE_OPENING ||
	    st.state == WS_OVERVIEW_STATE_CLOSING); i++) {
		nanosleep(&ts, NULL);
		r = ws_overview(c, WS_OVERVIEW_QUERY, &st, NULL, NULL);
	}
	if (r != 0)
		return r;
	r = ws_overview(c, WS_OVERVIEW_QUERY, &st, print_overview_item, NULL);
	if (r == 0)
		printf("state=%s mode=%s count=%u selected=%u app=%u\n",
		    st.state < 4 ? ov_states[st.state] : "?",
		    st.mode == WS_OVERVIEW_APP ? "app" : "all", st.count,
		    st.selected, st.app_id);
	return r;
}

static volatile sig_atomic_t stop;


static void
on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

/* ---- wsctl screen ---- */

static void
print_usable(const ws_rect_t *r)
{
	printf("usable %d,%d %dx%d\n", r->x, r->y, r->width, r->height);
}

static void
print_screen_event(const ws_event_t *ev)
{
	switch (ev->hdr.type) {
	case WS_EVENT_SCREEN_CHANGED:
		printf("screen-changed %ux%u\n", ev->u.screen.width,
		    ev->u.screen.height);
		break;
	case WS_EVENT_USABLE_AREA:
		printf("usable-area ");
		print_usable(&ev->u.usable_area.usable);
		break;
	case WS_EVENT_ERROR:
		fprintf(stderr, "wsctl: server: %s\n", ev->u.error.message);
		break;
	default:
		break;
	}
}

/*
 * wsctl menubar: connect as an app, become active (unless "inactive"),
 * say the app's menu bar is `height` pixels, and print the usable area
 * once the server confirms the strip (or at once for an inactive app,
 * whose report doesn't count yet). Then stay `seconds`, printing each
 * usable-area change.
 */
static int
menubar_main(int argc, char **argv)
{
	ws_connection_t *c;
	uint32_t height;
	int seconds, inactive, confirmed = 0, r;
	time_t end;
	ws_event_t ev;
	ws_rect_t u;

	if (argc < 3 || argc > 5)
		usage();
	height = (uint32_t)strtoul(argv[2], NULL, 0);
	seconds = argc >= 4 ? atoi(argv[3]) : 0;
	inactive = argc == 5 && strcmp(argv[4], "inactive") == 0;
	if (argc == 5 && !inactive)
		usage();
	c = ws_connect("menubar", 0);
	if (c == NULL) {
		fprintf(stderr, "wsctl: can't connect to WindowServer: %s\n",
		    strerror(errno));
		return 1;
	}
	if ((ws_server_flags(c) & WS_SERVER_MENU_BAR) == 0) {
		fprintf(stderr, "wsctl menubar: %s\n", strerror(ENOTSUP));
		ws_disconnect(c);
		return 1;
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("client %u\n", ws_client_id(c));
	if ((!inactive && ws_activate(c) < 0) || ws_set_menu_bar(c, height) < 0 ||
	    ws_sync(c) < 0) {
		fprintf(stderr, "wsctl menubar: %s\n", strerror(errno));
		ws_disconnect(c);
		return 1;
	}
	/* Two seconds for the strip (an inactive app: what's queued). */
	end = time(NULL) + (inactive ? 0 : 2);
	confirmed = !inactive && ws_menu_bar_height(c) == height;
	while (!confirmed && time(NULL) <= end) {
		r = ws_next_event(c, &ev, 200);
		if (r < 0)
			break;
		if (r == 0)
			continue;
		if (ev.hdr.type == WS_EVENT_ERROR) {
			fprintf(stderr, "wsctl: server: %s\n",
			    ev.u.error.message);
			ws_disconnect(c);
			return 1;
		}
		if (!inactive && ev.hdr.type == WS_EVENT_USABLE_AREA &&
		    ev.u.usable_area.menu_bar == height)
			confirmed = 1;
	}
	if (!inactive && !confirmed) {
		fprintf(stderr, "wsctl menubar: %s\n", strerror(ETIMEDOUT));
		ws_disconnect(c);
		return 1;
	}
	(void)ws_usable_area(c, &u);
	printf("menubar %u ", ws_menu_bar_height(c));
	print_usable(&u);
	end = time(NULL) + seconds;
	while (!stop && seconds > 0 && time(NULL) < end) {
		r = ws_next_event(c, &ev, 200);
		if (r < 0)
			break;
		if (r > 0 && ev.hdr.type == WS_EVENT_USABLE_AREA) {
			printf("menubar %u ", ev.u.usable_area.menu_bar);
			print_usable(&ev.u.usable_area.usable);
		} else if (r > 0 && ev.hdr.type == WS_EVENT_ERROR)
			fprintf(stderr, "wsctl: server: %s\n",
			    ev.u.error.message);
	}
	ws_disconnect(c);
	return 0;
}

/*
 * wsctl screen: the screen and its usable area. With "size WxH" the
 * (headless) screen changes size first, and this waits for the
 * screen-changed and usable-area events that confirm it. With "watch",
 * stays connected printing those events.
 */
static int
screen_cmd(ws_connection_t *c, int argc, char **argv)
{
	const ws_screen_info_t *s;
	unsigned w, h;
	time_t end;
	ws_event_t ev;
	ws_rect_t u;
	int r, got = 0;

	if ((ws_server_flags(c) & WS_SERVER_USABLE_AREA) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	if (argc == 2) {
		s = ws_main_screen(c);
		(void)ws_usable_area(c, &u);
		printf("screen %ux%u\n", s->width, s->height);
		print_usable(&u);
		if (ws_server_flags(c) & WS_SERVER_MENU_BAR)
			printf("menubar %u\n", ws_menu_bar_height(c));
		return 0;
	}
	if (strcmp(argv[2], "size") == 0 && argc == 4) {
		if (sscanf(argv[3], "%ux%u", &w, &h) != 2)
			usage();
		if (ws_inject_screen_size(c, w, h) < 0 || ws_sync(c) < 0)
			return -1;
		s = ws_main_screen(c);
		if (s->width == w && s->height == h)
			got = 2;	/* already that size: nothing comes */
		end = time(NULL) + 3;
		while (got < 2 && time(NULL) <= end) {
			r = ws_next_event(c, &ev, 200);
			if (r < 0)
				return -1;
			if (r == 0)
				continue;
			if (ev.hdr.type == WS_EVENT_ERROR) {
				print_screen_event(&ev);
				errno = ev.u.error.code ? ev.u.error.code : EIO;
				return -1;
			}
			print_screen_event(&ev);
			if (ev.hdr.type == WS_EVENT_SCREEN_CHANGED ||
			    (got == 1 && ev.hdr.type == WS_EVENT_USABLE_AREA))
				got++;
		}
		if (got < 2) {
			errno = ETIMEDOUT;
			return -1;
		}
		return 0;
	}
	if (strcmp(argv[2], "watch") == 0 && argc <= 4) {
		end = argc == 4 ? time(NULL) + atoi(argv[3]) : 0;
		signal(SIGTERM, on_signal);
		signal(SIGINT, on_signal);
		setvbuf(stdout, NULL, _IOLBF, 0);
		(void)ws_usable_area(c, &u);
		printf("watching ");
		print_usable(&u);
		while (!stop && (end == 0 || time(NULL) < end)) {
			r = ws_next_event(c, &ev, 200);
			if (r < 0)
				return -1;
			if (r > 0)
				print_screen_event(&ev);
		}
		return 0;
	}
	usage();
	return -1;
}

static const char *const dock_edges[] = { "none", "left", "right", "top",
    "bottom" };

static const char *
dock_edge_name(uint32_t edge)
{
	return edge <= WS_DOCK_EDGE_BOTTOM ? dock_edges[edge] : "?";
}

/* The Dock's edge as WS_EVENT_DOCK_STATE gives it. */
static void
print_dock_edge(const char *prefix, const ws_event_dock_state_t *s)
{
	printf("%sedge=%s thickness=%u client=%u\n", prefix,
	    dock_edge_name(s->edge), s->thickness, s->edge_client);
}

static void
print_dock_event(const ws_event_t *ev)
{
	switch (ev->hdr.type) {
	case WS_EVENT_DOCK_STATE:
		printf("state flags=0x%x%s edge=%s thickness=%u\n",
		    ev->u.dock_state.flags,
		    (ev->u.dock_state.flags & WS_DOCK_MINIMIZED_WINDOWS) ?
		    " minimized-windows" : "",
		    dock_edge_name(ev->u.dock_state.edge),
		    ev->u.dock_state.thickness);
		break;
	case WS_EVENT_PREVIEW_CHANGED:
		printf("preview-changed window=%u\n", ev->hdr.window);
		break;
	case WS_EVENT_ERROR:
		printf("error request=0x%x code=%d %s\n", ev->u.error.request,
		    ev->u.error.code, ev->u.error.message);
		break;
	default:
		break;
	}
}

/* wsctl dock watch: stay connected as the Dock and print what comes. */
static int
dock_watch(ws_connection_t *c, int seconds)
{
	time_t end = seconds > 0 ? time(NULL) + seconds : 0;
	struct pollfd pfd;
	ws_event_t ev;
	int changed, r;

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (ws_dock_show_minimized(c, 1) < 0)
		return -1;
	printf("watching as client %u\n", ws_client_id(c));
	while (!stop && (end == 0 || time(NULL) < end)) {
		changed = 0;
		while ((r = ws_next_event(c, &ev, 0)) > 0) {
			if (ev.hdr.type == WS_EVENT_WINDOW_LIST_CHANGED)
				changed = 1;
			else
				print_dock_event(&ev);
		}
		if (r < 0) {
			printf("server closed the connection\n");
			return -1;
		}
		if (changed) {
			printf("list-changed\n");
			if (ws_list_windows_flags(c, 0, print_dock_window,
			    "  ") < 0)
				printf("list failed: %s\n", strerror(errno));
			printf("end\n");
			continue;	/* the list may have queued events */
		}
		pfd.fd = ws_connection_fd(c);
		pfd.events = POLLIN;
		pfd.revents = 0;
		(void)poll(&pfd, 1, 200);
	}
	return 0;
}

/* wsctl dock edge: report an edge, print the state that confirms it (or
 * the server's error), then stay connected, as a Dock that keeps the
 * edge, for `seconds`, printing each change. */
static int
dock_edge(ws_connection_t *c, const char *name, const char *thickness,
    int seconds)
{
	time_t end;
	uint32_t edge, px;
	ws_event_t ev;
	int confirmed = 0, r;

	for (edge = 0; edge <= WS_DOCK_EDGE_BOTTOM; edge++)
		if (strcmp(name, dock_edges[edge]) == 0)
			break;
	if (edge > WS_DOCK_EDGE_BOTTOM)
		usage();
	px = edge == WS_DOCK_EDGE_NONE ? 0 : (uint32_t)strtoul(thickness, NULL, 0);
	if ((ws_server_flags(c) & WS_SERVER_DOCK_EDGE) == 0) {
		errno = ENOTSUP;
		return -1;
	}
	if (ws_dock_set_edge(c, edge, px) < 0)
		return -1;
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	setvbuf(stdout, NULL, _IOLBF, 0);
	/* Two seconds for the state naming this connection (none: any). */
	end = time(NULL) + 2;
	while (!confirmed && time(NULL) <= end) {
		r = ws_next_event(c, &ev, 200);
		if (r < 0)
			return -1;
		if (r == 0)
			continue;
		if (ev.hdr.type == WS_EVENT_ERROR) {
			fprintf(stderr, "wsctl: server: %s\n", ev.u.error.message);
			errno = ev.u.error.code ? ev.u.error.code : EIO;
			return -1;
		}
		if (ev.hdr.type == WS_EVENT_DOCK_STATE &&
		    (edge == WS_DOCK_EDGE_NONE ||
		    (ev.u.dock_state.edge_client == ws_client_id(c) &&
		    ev.u.dock_state.edge == edge &&
		    ev.u.dock_state.thickness == px))) {
			print_dock_edge("", &ev.u.dock_state);
			confirmed = 1;
		}
	}
	if (!confirmed) {
		errno = ETIMEDOUT;
		return -1;
	}
	end = time(NULL) + seconds;
	while (!stop && seconds > 0 && time(NULL) < end) {
		r = ws_next_event(c, &ev, 200);
		if (r < 0)
			return -1;
		if (r > 0 && ev.hdr.type == WS_EVENT_DOCK_STATE)
			print_dock_edge("", &ev.u.dock_state);
	}
	return 0;
}

static int
dock_main(int argc, char **argv)
{
	ws_connection_t *c;
	ws_snapshot_t snap;
	const char *cmd;
	ws_event_t ev;
	int r = -1;

	if (argc < 2)
		usage();
	cmd = argv[1];
	c = ws_connect("wsctl-dock", WS_CONNECT_DOCK);
	if (c == NULL) {
		fprintf(stderr, "wsctl: can't connect as the Dock: %s\n",
		    strerror(errno));
		return 1;
	}
	if ((ws_server_flags(c) & WS_SERVER_DOCK) == 0) {
		fprintf(stderr, "wsctl: this server has no Dock role\n");
		ws_disconnect(c);
		return 1;
	}
	if (strcmp(cmd, "list") == 0 && argc <= 3) {
		r = ws_list_windows_flags(c, argc == 3 &&
		    strcmp(argv[2], "-a") == 0 ? WS_LIST_ORDERED_OUT : 0,
		    print_dock_window, NULL);
		/* The Dock's edge: the state that came after the welcome
		 * (queued while the list was read). */
		while (r == 0 && ws_next_event(c, &ev, 0) > 0)
			if (ev.hdr.type == WS_EVENT_DOCK_STATE &&
			    (ws_server_flags(c) & WS_SERVER_DOCK_EDGE) != 0)
				print_dock_edge("dock ", &ev.u.dock_state);
			else if (ev.hdr.type == WS_EVENT_ERROR) {
				fprintf(stderr, "wsctl: server: %s\n",
				    ev.u.error.message);
				r = -1;
			}
	} else if (strcmp(cmd, "activate") == 0 && (argc == 3 || argc == 4)) {
		r = ws_dock_activate(c, (uint32_t)strtoul(argv[2], NULL, 0),
		    argc == 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0);
		if (r == 0)
			r = ws_sync(c);
	} else if (strcmp(cmd, "restore") == 0 && argc == 3) {
		r = ws_dock_restore(c, (uint32_t)strtoul(argv[2], NULL, 0));
		if (r == 0)
			r = ws_sync(c);
	} else if (strcmp(cmd, "preview") == 0 && argc == 5) {
		r = ws_dock_preview(c, (uint32_t)strtoul(argv[2], NULL, 0),
		    (uint32_t)strtoul(argv[3], NULL, 0), &snap);
		if (r == 0) {
			/* premultiplied: as it looks on black */
			r = write_ppm(&snap, argv[4]);
			printf("%s: %ux%u\n", argv[4], snap.width, snap.height);
			ws_snapshot_free(&snap);
		}
	} else if (strcmp(cmd, "watch") == 0 && argc <= 3) {
		r = dock_watch(c, argc == 3 ? atoi(argv[2]) : 0);
	} else if (strcmp(cmd, "edge") == 0 && (argc == 4 || argc == 5)) {
		r = dock_edge(c, argv[2], argv[3], argc == 5 ? atoi(argv[4]) : 0);
	} else {
		ws_disconnect(c);
		usage();
	}
	if (r != 0)
		fprintf(stderr, "wsctl dock %s: %s\n", cmd, strerror(errno));
	while (ws_next_event(c, &ev, 0) > 0)
		if (ev.hdr.type == WS_EVENT_ERROR) {
			fprintf(stderr, "wsctl: server: %s\n",
			    ev.u.error.message);
			r = -1;
		}
	ws_disconnect(c);
	return r == 0 ? 0 : 1;
}

/* ---- tiling ---- */

static int settings_fail(const char *what, const char *key);

static void
print_tiling_item(const ws_event_tiling_item_t *it, void *ctx)
{
	(void)ctx;
	if (it->flags & WS_TILING_ITEM_TILED)
		printf("tile %u window=%u app=%u tile=%d,%d,%dx%d "
		    "frame=%d,%d,%dx%d%s\n", it->index, it->window, it->app_id,
		    it->tile.x, it->tile.y, it->tile.width, it->tile.height,
		    it->frame.x, it->frame.y, it->frame.width, it->frame.height,
		    (it->flags & WS_TILING_ITEM_FOCUSED) ? " focused" : "");
	else
		printf("float window=%u app=%u frame=%d,%d,%dx%d%s%s\n",
		    it->window, it->app_id, it->frame.x, it->frame.y,
		    it->frame.width, it->frame.height,
		    (it->flags & WS_TILING_ITEM_FLOATED) ? " floated" : "",
		    (it->flags & WS_TILING_ITEM_REFUSED) ? " too-big" : "");
}

static int
tiling_dir(const char *s)
{
	if (strcmp(s, "left") == 0)
		return WS_TILING_DIR_LEFT;
	if (strcmp(s, "right") == 0)
		return WS_TILING_DIR_RIGHT;
	if (strcmp(s, "up") == 0)
		return WS_TILING_DIR_UP;
	if (strcmp(s, "down") == 0)
		return WS_TILING_DIR_DOWN;
	usage();
	return 0;
}

static int
tiling_cmd(ws_connection_t *c, int argc, char **argv)
{
	static const char *const mods[] = { "", "Shift+", "Control+",
	    "Option+", "Command+" };
	const char *what = argc >= 3 ? argv[2] : "status";
	ws_setting_t set[2];
	ws_event_tiling_t st;
	char key[64] = "";
	uint32_t op = WS_TILING_QUERY, win = 0;
	int32_t arg = 0;
	unsigned n = 0, i;
	int r;

	if (strcmp(what, "on") == 0 || strcmp(what, "off") == 0) {
		set[n++] = (ws_setting_t){ "WindowMode", WS_SETTING_STRING,
		    what[1] == 'n' ? "tiling" : "stacking" };
	} else if (strcmp(what, "layout") == 0 && argc == 4) {
		set[n++] = (ws_setting_t){ "TilingLayout", WS_SETTING_STRING,
		    argv[3] };
	} else if (strcmp(what, "gaps") == 0 && (argc == 4 || argc == 5)) {
		set[n++] = (ws_setting_t){ "TilingGapsInner",
		    WS_SETTING_STRING, argv[3] };
		set[n++] = (ws_setting_t){ "TilingGapsOuter",
		    WS_SETTING_STRING, argv[argc == 5 ? 4 : 3] };
	} else if (strcmp(what, "border") == 0 && (argc == 4 || argc == 5)) {
		set[n++] = (ws_setting_t){ "TilingBorder", WS_SETTING_STRING,
		    argv[3] };
		if (argc == 5)
			set[n++] = (ws_setting_t){ "TilingBorderColor",
			    WS_SETTING_STRING, argv[4] };
	} else if ((strcmp(what, "focus") == 0 || strcmp(what, "swap") == 0) &&
	    argc == 4) {
		op = what[0] == 'f' ? WS_TILING_FOCUS : WS_TILING_SWAP;
		arg = tiling_dir(argv[3]);
	} else if ((strcmp(what, "grow") == 0 || strcmp(what, "shrink") == 0) &&
	    argc == 3) {
		op = WS_TILING_RESIZE;
		arg = what[0] == 'g' ? 1 : -1;
	} else if ((strcmp(what, "float") == 0 || strcmp(what, "tile") == 0 ||
	    strcmp(what, "toggle") == 0) && argc <= 4) {
		op = WS_TILING_FLOAT;
		arg = what[0] == 'f' ? 1 : what[1] == 'i' ? 0 : 2;
		win = argc == 4 ? (uint32_t)strtoul(argv[3], NULL, 0) : 0;
	} else if (strcmp(what, "status") != 0 || argc > 3)
		usage();
	if (n > 0) {
		if (ws_set_settings(c, 0, set, n, key, sizeof(key)) < 0)
			return settings_fail("set", key);
		return 0;
	}
	memset(&st, 0, sizeof(st));
	if (op == WS_TILING_QUERY) {
		r = ws_tiling(c, op, win, arg, &st, NULL, NULL);
		if (r != 0)
			return r;
		printf("mode=%s layout=%s tiled=%u focused=%u "
		    "area=%d,%d,%dx%d gaps=%u,%u border=%u modifier=",
		    st.mode == WS_TILING_TILING ? "tiling" : "stacking",
		    st.layout == WS_TILING_MASTER ? "master" : "dwindle",
		    st.count, st.focused, st.area.x, st.area.y, st.area.width,
		    st.area.height, st.gaps_inner, st.gaps_outer, st.border);
		for (i = 4; i > 0; i--)
			if (st.modifier & (1u << (i - 1)))
				printf("%s", mods[i]);
		printf("arrows\n");
		fflush(stdout);
	}
	return ws_tiling(c, op, win, arg, NULL, print_tiling_item, NULL);
}

/* ---- settings ---- */

static const char *const sources[] = { "built-in", "system", "session" };

static void
print_setting(const ws_event_setting_t *st, void *ctx)
{
	const char *only = ctx;

	if (only != NULL && strcmp(only, st->key) != 0)
		return;
	if (only != NULL) {
		printf("%s\n", st->value);
		return;
	}
	printf("%-22s = %s%s (%s%s)\n", st->key, st->value,
	    (st->flags & WS_SETTING_TRUNCATED) ? "..." : "",
	    st->source <= WS_SETTING_SOURCE_SESSION ? sources[st->source] : "?",
	    (st->flags & WS_SETTING_SETTABLE) ? "" : ", server only");
}

static int
settings_fail(const char *what, const char *key)
{
	int e = errno;

	if (key[0] != '\0')
		fprintf(stderr, "wsctl settings %s: %s: %s\n", what, key,
		    strerror(e));
	else
		fprintf(stderr, "wsctl settings %s: %s\n", what, strerror(e));
	return 2;
}

static int
settings_cmd(ws_connection_t *c, int argc, char **argv)
{
	ws_setting_t list[32];
	char path[1024], key[64] = "";
	struct timespec ts = { 0, 200 * 1000 * 1000 };
	int i, n, tries;

	if (argc < 3)
		usage();
	if (strcmp(argv[2], "get") == 0 && argc <= 4)
		return ws_get_settings(c, print_setting,
		    argc == 4 ? argv[3] : NULL);
	if (strcmp(argv[2], "set") == 0 && argc >= 5 && argc % 2 == 1) {
		for (i = 3, n = 0; i + 1 < argc && n < 32; i += 2, n++) {
			list[n].key = argv[i];
			list[n].type = WS_SETTING_STRING;
			list[n].value = argv[i + 1];
		}
		if (ws_set_settings(c, 0, list, (unsigned)n, key,
		    sizeof(key)) < 0)
			return settings_fail("set", key);
		return 0;
	}
	if (strcmp(argv[2], "unset") == 0 && argc >= 4) {
		for (i = 3, n = 0; i < argc && n < 32; i++, n++) {
			list[n].key = argv[i];
			list[n].type = WS_SETTING_DEFAULT;
			list[n].value = "";
		}
		if (ws_set_settings(c, 0, list, (unsigned)n, key,
		    sizeof(key)) < 0)
			return settings_fail("unset", key);
		return 0;
	}
	if (strcmp(argv[2], "reset") == 0 && argc == 3) {
		if (ws_set_settings(c, WS_SETTINGS_REPLACE, NULL, 0, key,
		    sizeof(key)) < 0)
			return settings_fail("reset", key);
		return 0;
	}
	if (strcmp(argv[2], "reload") == 0 && argc <= 4) {
		if (argc == 4)
			strlcpy(path, argv[3], sizeof(path));
		else if (ws_user_settings_path(path, sizeof(path)) < 0)
			return settings_fail("reload", "");
		/*
		 * At login, loginwindow writes its session record just as
		 * the session starts: until WindowServer sees it, this user
		 * isn't the console user yet. Try for a few seconds.
		 */
		for (tries = 0;; tries++) {
			key[0] = '\0';
			if (ws_settings_load_file(c, path, WS_SETTINGS_REPLACE |
			    WS_SETTINGS_SKIP_INVALID, key, sizeof(key)) == 0)
				break;
			if (errno != EPERM || tries >= 15)
				return settings_fail("reload", "");
			nanosleep(&ts, NULL);
		}
		if (key[0] != '\0')
			fprintf(stderr, "wsctl settings reload: %s: left out "
			    "%s (and maybe more; see the server's log)\n",
			    path, key);
		return 0;
	}
	usage();
	return -1;
}

static void
print_layout(const ws_event_keyboard_layout_t *l, void *ctx)
{
	(void)ctx;
	printf("%u %s%s%s \"%s\"%s\n", l->index, l->layout,
	    l->variant[0] ? "(" : "", l->variant[0] ? l->variant : "",
	    l->name, (l->flags & WS_KEYBOARD_LAYOUT_ACTIVE) ? " active" : "");
}

struct layout_find {
	const char	*name;
	int		index;
};

static void
find_layout(const ws_event_keyboard_layout_t *l, void *ctx)
{
	struct layout_find *f = ctx;

	if (f->index < 0 && strcmp(l->layout, f->name) == 0)
		f->index = (int)l->index;
}

static int
layout_cmd(ws_connection_t *c, int argc, char **argv)
{
	struct layout_find f;
	ws_event_t ev;
	time_t end;
	char *e;
	int r;

	if (argc < 3)
		usage();
	if (strcmp(argv[2], "list") == 0 && argc == 3)
		return ws_list_keyboard_layouts(c, print_layout, NULL);
	if (strcmp(argv[2], "set") == 0 && argc == 4) {
		f.name = argv[3];
		f.index = (int)strtol(argv[3], &e, 10);
		if (*e != '\0' || e == argv[3]) {
			f.index = -1;
			if (ws_list_keyboard_layouts(c, find_layout, &f) < 0)
				return -1;
			if (f.index < 0) {
				errno = ENOENT;
				return -1;
			}
		}
		return ws_set_keyboard_layout(c, (uint32_t)f.index);
	}
	if (strcmp(argv[2], "watch") == 0 && argc <= 4) {
		end = argc == 4 ? time(NULL) + atoi(argv[3]) : 0;
		signal(SIGTERM, on_signal);
		signal(SIGINT, on_signal);
		setvbuf(stdout, NULL, _IOLBF, 0);
		printf("watching\n");
		while (!stop && (end == 0 || time(NULL) < end)) {
			r = ws_next_event(c, &ev, 200);
			if (r < 0)
				return -1;
			if (r == 0)
				continue;
			if (ev.hdr.type == WS_EVENT_KEYBOARD_LAYOUT) {
				printf("layout ");
				print_layout(&ev.u.keyboard_layout, NULL);
			} else if (ev.hdr.type == WS_EVENT_SETTINGS_CHANGED)
				printf("settings-changed\n");
		}
		return 0;
	}
	usage();
	return -1;
}

static void
print_device(const ws_event_input_device_t *d, void *ctx)
{
	(void)ctx;
	printf("%-8s %s%s%s%s%s \"%s\"\n", d->sysname,
	    (d->flags & WS_INPUT_KEYBOARD) ? " keyboard" : "",
	    (d->flags & WS_INPUT_POINTER) ? " pointer" : "",
	    (d->flags & WS_INPUT_TOUCHPAD) ? " touchpad" : "",
	    (d->flags & WS_INPUT_MOUSE) ? " mouse" : "",
	    (d->flags & WS_INPUT_TOUCH) ? " touch" : "", d->name);
}

int
main(int argc, char **argv)
{
	ws_connection_t *c;
	ws_snapshot_t snap;
	const char *cmd;
	int r = 0, i;

	if (argc < 2)
		usage();
	cmd = argv[1];
	if (strcmp(cmd, "dock") == 0)
		return dock_main(argc - 1, argv + 1);
	if (strcmp(cmd, "menubar") == 0)
		return menubar_main(argc, argv);
	c = ws_connect("wsctl", WS_CONNECT_CONTROL);
	if (c == NULL) {
		fprintf(stderr, "wsctl: can't connect to WindowServer: %s\n",
		    strerror(errno));
		return 1;
	}

	if (strcmp(cmd, "list") == 0) {
		r = ws_list_windows(c, print_window, NULL);
	} else if (strcmp(cmd, "snapshot") == 0 && argc == 3) {
		r = ws_snapshot(c, &snap);
		if (r == 0) {
			r = write_ppm(&snap, argv[2]);
			printf("%s: %ux%u\n", argv[2], snap.width, snap.height);
			ws_snapshot_free(&snap);
		}
	} else if (strcmp(cmd, "pixel") == 0 && argc >= 4 && argc % 2 == 0) {
		r = ws_snapshot(c, &snap);
		for (i = 2; r == 0 && i + 1 < argc; i += 2) {
			uint32_t x = (uint32_t)atoi(argv[i]);
			uint32_t y = (uint32_t)atoi(argv[i + 1]);
			const uint32_t *row;

			if (x >= snap.width || y >= snap.height) {
				printf("%u,%u outside\n", x, y);
				continue;
			}
			row = (const uint32_t *)(const void *)
			    ((const char *)snap.pixels + (size_t)y * snap.stride);
			printf("%u,%u %06x\n", x, y, row[x] & 0xffffff);
		}
		if (r == 0)
			ws_snapshot_free(&snap);
	} else if (strcmp(cmd, "move") == 0 && argc == 4) {
		r = ws_inject_motion(c, atof(argv[2]), atof(argv[3]));
	} else if (strcmp(cmd, "click") == 0 && (argc == 4 || argc == 5)) {
		uint32_t b = button(argc == 5 ? argv[4] : NULL);

		r = ws_inject_motion(c, atof(argv[2]), atof(argv[3]));
		if (r == 0)
			r = ws_inject_button(c, b, 1);
		if (r == 0)
			r = ws_inject_button(c, b, 0);
	} else if ((strcmp(cmd, "down") == 0 || strcmp(cmd, "up") == 0) &&
	    argc <= 3) {
		r = ws_inject_button(c, button(argc == 3 ? argv[2] : NULL),
		    strcmp(cmd, "down") == 0);
	} else if (strcmp(cmd, "key") == 0 && argc == 3) {
		r = ws_inject_key_tap(c, argv[2]);
	} else if ((strcmp(cmd, "keydown") == 0 ||
	    strcmp(cmd, "keyup") == 0) && argc == 3) {
		r = ws_inject_key(c, argv[2], strcmp(cmd, "keydown") == 0);
	} else if (strcmp(cmd, "scroll") == 0 && argc == 4) {
		r = ws_inject_scroll(c, atof(argv[2]), atof(argv[3]));
	} else if (strcmp(cmd, "scroll") == 0 && argc == 5 &&
	    strcmp(argv[4], "finger") == 0) {
		r = ws_inject_scroll_finger(c, atof(argv[2]), atof(argv[3]));
	} else if (strcmp(cmd, "flick") == 0 && (argc == 4 || argc == 5)) {
		/* DX, DY in STEPS finger scrolls 10 ms apart, then the lift:
		 * fast enough for momentum scrolling. */
		int steps = argc == 5 ? atoi(argv[4]) : 8;
		struct timespec ts = { 0, 10 * 1000 * 1000 };

		if (steps < 1)
			steps = 1;
		for (i = 0; r == 0 && i < steps; i++) {
			r = ws_inject_scroll_finger(c, atof(argv[2]) / steps,
			    atof(argv[3]) / steps);
			nanosleep(&ts, NULL);
		}
		if (r == 0)
			r = ws_inject_scroll_finger(c, 0, 0);
	} else if (strcmp(cmd, "swipe") == 0 && (argc == 5 || argc == 6)) {
		/* begin, STEPS updates 10 ms apart, end */
		int steps = argc == 6 ? atoi(argv[5]) : 8;
		uint32_t fingers = (uint32_t)atoi(argv[2]);
		struct timespec ts = { 0, 10 * 1000 * 1000 };

		if (steps < 1)
			steps = 1;
		r = ws_inject_swipe(c, fingers, WS_GESTURE_BEGIN, 0, 0);
		for (i = 0; r == 0 && i < steps; i++) {
			r = ws_inject_swipe(c, fingers, WS_GESTURE_UPDATE,
			    atof(argv[3]) / steps, atof(argv[4]) / steps);
			nanosleep(&ts, NULL);
		}
		if (r == 0)
			r = ws_inject_swipe(c, fingers, WS_GESTURE_END, 0, 0);
	} else if (strcmp(cmd, "screen") == 0) {
		r = screen_cmd(c, argc, argv);
	} else if (strcmp(cmd, "overview") == 0 && argc <= 3) {
		r = overview_cmd(c, argc == 3 ? argv[2] : NULL);
	} else if (strcmp(cmd, "hold") == 0 && argc == 2) {
		r = ws_inject_hold(c, 1);
		if (r == 0)
			r = ws_inject_hold(c, 0);
	} else if (strcmp(cmd, "sync") == 0) {
		r = ws_sync(c);
	} else if (strcmp(cmd, "settings") == 0) {
		r = settings_cmd(c, argc, argv);
		if (r == 2) {		/* it said what failed */
			ws_disconnect(c);
			return 1;
		}
	} else if (strcmp(cmd, "layout") == 0) {
		r = layout_cmd(c, argc, argv);
	} else if (strcmp(cmd, "tiling") == 0) {
		r = tiling_cmd(c, argc, argv);
		if (r == 2) {		/* it said what failed */
			ws_disconnect(c);
			return 1;
		}
	} else if (strcmp(cmd, "devices") == 0 && argc == 2) {
		r = ws_list_input_devices(c, print_device, NULL);
	} else {
		ws_disconnect(c);
		usage();
	}
	if (r != 0)
		fprintf(stderr, "wsctl %s: %s\n", cmd, strerror(errno));
	/* Report errors the server sent back (bad key names etc.). */
	{
		ws_event_t ev;

		while (ws_next_event(c, &ev, 0) > 0)
			if (ev.hdr.type == WS_EVENT_ERROR) {
				fprintf(stderr, "wsctl: server: %s\n",
				    ev.u.error.message);
				r = -1;
			}
	}
	ws_disconnect(c);
	return r == 0 ? 0 : 1;
}
