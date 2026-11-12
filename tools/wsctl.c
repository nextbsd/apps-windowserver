/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * wsctl: inspect and drive WindowServer from the command line.
 * Snapshots and synthetic input are allowed when the server runs
 * headless, or for root and the server's own user. `wsctl dock ...`
 * connects as a Dock (WS_CONNECT_DOCK) to try the Dock's requests.
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
	    "       wsctl sync\n"
	    "       wsctl dock list [-a]     (every window, with ids; -a: hidden too)\n"
	    "       wsctl dock activate APP_ID [WINDOW]\n"
	    "       wsctl dock restore WINDOW\n"
	    "       wsctl dock preview WINDOW SIZE FILE.ppm\n"
	    "       wsctl dock watch [SECONDS] (stay the Dock, print notices)\n");
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
	printf("%-3u %-8s pid=%-6d level=%-5d frame=%d,%d,%dx%d%s%s%s \"%s\"\n",
	    w->stack_index, w->app_name, w->pid, w->level, w->frame.x,
	    w->frame.y, w->frame.width, w->frame.height,
	    (w->flags & WS_WINDOW_INFO_MINIMIZED) ? " minimized" : "",
	    (w->flags & WS_WINDOW_INFO_ACTIVE) ? " active" : "",
	    (w->flags & WS_WINDOW_INFO_KEY) ? " key" : "", w->title);
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

static volatile sig_atomic_t stop;

static void
on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static void
print_dock_event(const ws_event_t *ev)
{
	switch (ev->hdr.type) {
	case WS_EVENT_DOCK_STATE:
		printf("state flags=0x%x%s\n", ev->u.dock_state.flags,
		    (ev->u.dock_state.flags & WS_DOCK_MINIMIZED_WINDOWS) ?
		    " minimized-windows" : "");
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
	} else if (strcmp(cmd, "hold") == 0 && argc == 2) {
		r = ws_inject_hold(c, 1);
		if (r == 0)
			r = ws_inject_hold(c, 0);
	} else if (strcmp(cmd, "sync") == 0) {
		r = ws_sync(c);
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
