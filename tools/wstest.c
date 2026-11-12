/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * wstest: a WindowServer test client. It connects as two apps,
 * "Alpha" and "Beta", plus a third connection, "Gamma", that joins
 * Alpha's app with Alpha's client id and token (as an X server joins
 * XLibre.app's). It creates four windows and prints every event it
 * receives, one per line:
 *
 *   Alpha  W1  normal level      (100,100) 500x400  red
 *   Alpha  W2  main-menu level   ( 50, 50) 300x300  green  (never key)
 *   Beta   W3  normal level      (300,250) 500x400  blue
 *   Gamma  W4  normal level      (900,100) 300x200  yellow, XRGB8888
 *                                                      with alpha byte 0
 *
 * Gamma connects with WS_CONNECT_XKB, so it also gets the keymap, XKB
 * modifier state and held keys. Alpha activates first, then Beta, so
 * Beta ends up active with W3 as its key window. tests/headless-test.sh
 * drives it with wsctl.
 *
 * Alpha tries to list every app's windows, which only tools and the
 * Dock may do.
 *
 * Beta also gives the server a magenta app icon and acts like an app
 * that minimizes: typing "m" minimizes its key window, "c" redraws W3
 * in green (minimized or not), and on WS_EVENT_WINDOW_RESTORE it
 * orders the window in again and makes it key.
 */

#include <sys/mman.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <WindowServer/WindowServer.h>

struct app {
	const char	*name;
	ws_connection_t	*conn;
};

struct win {
	const char	*name;
	struct app	*app;
	uint32_t	id;
	ws_buffer_t	*buf;
};

#define NAPPS	3
#define NWINS	4

static struct app apps[NAPPS] = {
	{ "Alpha", NULL }, { "Beta", NULL }, { "Gamma", NULL },
};
static struct win wins[NWINS];
static volatile sig_atomic_t quit;
static uint32_t beta_key;	/* Beta's key window, for "m" */

static const char *
win_name(uint32_t id)
{
	size_t i;

	if (id == 0)
		return "-";
	for (i = 0; i < NWINS; i++)
		if (wins[i].id == id)
			return wins[i].name;
	return "?";
}

static void
fill(ws_buffer_t *b, uint32_t argb, uint32_t border)
{
	uint32_t *px = ws_buffer_data(b);
	uint32_t w = ws_buffer_width(b), h = ws_buffer_height(b);
	uint32_t stride = ws_buffer_stride(b) / 4, x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			px[y * stride + x] = (x < 2 || y < 2 || x >= w - 2 ||
			    y >= h - 2) ? border : argb;
}

/* Beta, the app that minimizes (see the top of the file). */
static void
beta_act(const ws_event_t *ev)
{
	ws_connection_t *c = apps[1].conn;
	ws_rect_t r = { 0, 0, 500, 400 };
	ws_buffer_t *b;

	if (ev->hdr.type == WS_EVENT_WINDOW_RESTORE) {
		ws_window_order(c, ev->hdr.window, WS_ORDER_ABOVE, 0);
		ws_window_make_key(c, ev->hdr.window);
		printf("restored %s\n", win_name(ev->hdr.window));
		return;
	}
	if (ev->hdr.type != WS_EVENT_KEY_DOWN)
		return;
	if (strcmp(ev->u.key.text, "m") == 0 && beta_key != 0) {
		printf("minimizing %s\n", win_name(beta_key));
		ws_window_minimize(c, beta_key);
	} else if (strcmp(ev->u.key.text, "c") == 0) {
		b = ws_buffer_create(c, 500, 400, 1);
		if (b == NULL)
			return;
		fill(b, 0xff20a050, 0xff105028);
		ws_window_attach(c, wins[2].id, b);
		ws_window_damage(c, wins[2].id, &r, 1);
		ws_window_commit(c, wins[2].id, 0);
		printf("redrew W3 green\n");
	}
}

/* Check a WS_EVENT_KEYMAP's memfd, then close it. */
static void
keymap_detail(const ws_event_keymap_t *km, char *buf, size_t len)
{
	const char *text = "unreadable";
	void *p = MAP_FAILED;
	int seals = -1;

	if (km->fd >= 0 && km->size > 0) {
		seals = fcntl(km->fd, F_GET_SEALS);
		p = mmap(NULL, km->size, PROT_READ, MAP_PRIVATE, km->fd, 0);
	}
	if (p != MAP_FAILED) {
		if (((const char *)p)[km->size - 1] != '\0')
			text = "unterminated";
		else if (strncmp(p, "xkb_keymap", 10) == 0)
			text = "xkb_keymap";
		else
			text = "other";
		munmap(p, km->size);
	}
	snprintf(buf, len, "format=%u size=%u delay=%u interval=%u text=%s "
	    "sealed=%s", km->format, km->size, km->repeat_delay,
	    km->repeat_interval, text,
	    seals >= 0 && (seals & F_SEAL_WRITE) ? "yes" : "no");
	if (km->fd >= 0)
		close(km->fd);
}

static void
print_event(ws_connection_t *conn, const ws_event_t *ev, void *ctx)
{
	struct app *app = ctx;
	char detail[200] = "";
	size_t off;
	uint32_t i;

	(void)conn;
	switch (ev->hdr.type) {
	case WS_EVENT_APP_ACTIVATED:
	case WS_EVENT_APP_DEACTIVATED:
		snprintf(detail, sizeof(detail), "other=%u",
		    ev->u.app.other_client);
		break;
	case WS_EVENT_MOUSE_DOWN:
	case WS_EVENT_MOUSE_UP:
	case WS_EVENT_MOUSE_MOVED:
	case WS_EVENT_MOUSE_DRAGGED:
	case WS_EVENT_MOUSE_ENTERED:
	case WS_EVENT_MOUSE_EXITED:
		snprintf(detail, sizeof(detail),
		    "x=%.0f y=%.0f button=%u clicks=%u mods=0x%x",
		    ev->u.pointer.x, ev->u.pointer.y, ev->u.pointer.button,
		    ev->u.pointer.click_count, ev->u.pointer.modifiers);
		break;
	case WS_EVENT_SCROLL:
		snprintf(detail, sizeof(detail), "dx=%.2f dy=%.2f source=%u",
		    ev->u.scroll.dx, ev->u.scroll.dy, ev->u.scroll.source);
		break;
	case WS_EVENT_KEY_DOWN:
	case WS_EVENT_KEY_UP:
		snprintf(detail, sizeof(detail),
		    "keycode=%u keysym=0x%x text=\"%s\" unmod=\"%s\" mods=0x%x%s",
		    ev->u.key.keycode, ev->u.key.keysym, ev->u.key.text,
		    ev->u.key.text_unmodified, ev->u.key.modifiers,
		    (ev->u.key.flags & WS_KEY_REPEAT) ? " repeat" : "");
		break;
	case WS_EVENT_FLAGS_CHANGED:
		snprintf(detail, sizeof(detail), "mods=0x%x",
		    ev->u.flags.modifiers);
		break;
	case WS_EVENT_BUFFER_RELEASE:
		snprintf(detail, sizeof(detail), "buffer=%u",
		    ev->u.buffer.buffer_id);
		break;
	case WS_EVENT_ERROR:
		snprintf(detail, sizeof(detail), "request=0x%x code=%d %s",
		    ev->u.error.request, ev->u.error.code, ev->u.error.message);
		break;
	case WS_EVENT_KEYMAP:
		keymap_detail(&ev->u.keymap, detail, sizeof(detail));
		break;
	case WS_EVENT_MODIFIERS:
		snprintf(detail, sizeof(detail),
		    "depressed=0x%x latched=0x%x locked=0x%x group=%u mods=0x%x",
		    ev->u.modifiers.depressed, ev->u.modifiers.latched,
		    ev->u.modifiers.locked, ev->u.modifiers.group,
		    ev->u.modifiers.modifiers);
		break;
	case WS_EVENT_WINDOW_ORDERED:
		snprintf(detail, sizeof(detail), "op=%s below=%s above=%s "
		    "level=%d", ev->u.ordered.op == WS_ORDER_OUT ? "out" :
		    "above", win_name(ev->u.ordered.below),
		    win_name(ev->u.ordered.above), ev->u.ordered.level);
		break;
	case WS_EVENT_KEY_WINDOW_GAINED:
		if (app == &apps[1])
			beta_key = ev->hdr.window;
		break;
	case WS_EVENT_KEY_WINDOW_LOST:
		if (app == &apps[1] && beta_key == ev->hdr.window)
			beta_key = 0;
		break;
	case WS_EVENT_KEYS_HELD:
		off = (size_t)snprintf(detail, sizeof(detail), "count=%u keys=",
		    ev->u.keys_held.count);
		for (i = 0; i < ev->u.keys_held.count && i < WS_MAX_KEYS_HELD &&
		    off < sizeof(detail); i++)
			off += (size_t)snprintf(detail + off, sizeof(detail) - off,
			    "%s%u", i ? "," : "", ev->u.keys_held.keycodes[i]);
		break;
	default:
		break;
	}
	printf("event serial=%u app=%s type=%s win=%s %s\n", ev->hdr.serial,
	    app->name, ws_event_name(ev->hdr.type), win_name(ev->hdr.window),
	    detail);
	fflush(stdout);
	if (app == &apps[1])
		beta_act(ev);
}

static int
make_window(struct win *w, struct app *app, const char *name, int x, int y,
    int width, int height, int32_t level, uint32_t flags, uint32_t color,
    uint32_t format)
{
	ws_rect_t frame = { x, y, width, height };

	w->name = name;
	w->app = app;
	w->id = ws_window_create(app->conn, &frame, level, flags, name);
	if (w->id == 0) {
		fprintf(stderr, "wstest: create %s: %s\n", name, strerror(errno));
		return -1;
	}
	w->buf = ws_buffer_create_format(app->conn, (uint32_t)width,
	    (uint32_t)height, 1, format);
	if (w->buf == NULL) {
		fprintf(stderr, "wstest: buffer for %s: %s\n", name,
		    strerror(errno));
		return -1;
	}
	fill(w->buf, color, (color & 0xff000000) | ((color >> 1) & 0x7f7f7f));
	if (ws_window_attach(app->conn, w->id, w->buf) < 0 ||
	    ws_window_commit(app->conn, w->id, 0) < 0 ||
	    ws_window_order(app->conn, w->id, WS_ORDER_ABOVE, 0) < 0) {
		fprintf(stderr, "wstest: show %s: %s\n", name, strerror(errno));
		return -1;
	}
	printf("window %s id=%u app=%s level=%d frame=%d,%d,%dx%d\n", name,
	    w->id, app->name, level, x, y, width, height);
	return 0;
}

static void
on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

int
main(int argc, char **argv)
{
	struct pollfd pfd[NAPPS];
	ws_connection_t *bad;
	int ch, i, timeout = 0, n;
	time_t end;

	while ((ch = getopt(argc, argv, "t:")) != -1) {
		switch (ch) {
		case 't':
			timeout = atoi(optarg);
			break;
		default:
			fprintf(stderr, "usage: wstest [-t seconds]\n");
			return 2;
		}
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	for (i = 0; i < 2; i++) {
		apps[i].conn = ws_connect(apps[i].name, 0);
		if (apps[i].conn == NULL) {
			fprintf(stderr, "wstest: connect %s: %s\n", apps[i].name,
			    strerror(errno));
			return 1;
		}
		printf("connected app=%s client=%u screen=%ux%u\n", apps[i].name,
		    ws_client_id(apps[i].conn),
		    ws_main_screen(apps[i].conn)->width,
		    ws_main_screen(apps[i].conn)->height);
	}

	/* Joining needs the right token. */
	bad = ws_connect_join("Mallory", 0, ws_client_id(apps[0].conn),
	    ws_client_token(apps[0].conn) ^ 1);
	if (bad != NULL) {
		printf("join with a wrong token accepted\n");
		ws_disconnect(bad);
	} else
		printf("join refused app=Mallory %s\n",
		    errno == EPERM ? "EPERM" : strerror(errno));
	apps[2].conn = ws_connect_join(apps[2].name, WS_CONNECT_XKB,
	    ws_client_id(apps[0].conn), ws_client_token(apps[0].conn));
	if (apps[2].conn == NULL) {
		fprintf(stderr, "wstest: join %s: %s\n", apps[2].name,
		    strerror(errno));
		return 1;
	}
	printf("connected app=%s client=%u app=%u\n", apps[2].name,
	    ws_client_id(apps[2].conn), ws_app_id(apps[2].conn));

	if (make_window(&wins[0], &apps[0], "W1", 100, 100, 500, 400,
	    WS_LEVEL_NORMAL, 0, 0xffc03030, WS_FORMAT_ARGB8888) < 0 ||
	    make_window(&wins[1], &apps[0], "W2", 50, 50, 300, 300,
	    WS_LEVEL_MAIN_MENU, WS_WINDOW_NO_KEY, 0xff30a040,
	    WS_FORMAT_ARGB8888) < 0)
		return 1;
	ws_activate(apps[0].conn);
	ws_window_make_key(apps[0].conn, wins[0].id);
	ws_sync(apps[0].conn);
	if (make_window(&wins[3], &apps[2], "W4", 900, 100, 300, 200,
	    WS_LEVEL_NORMAL, 0, 0x00c0c030, WS_FORMAT_XRGB8888) < 0)
		return 1;
	ws_sync(apps[2].conn);

	if (make_window(&wins[2], &apps[1], "W3", 300, 250, 500, 400,
	    WS_LEVEL_NORMAL, 0, 0xff3050c0, WS_FORMAT_ARGB8888) < 0)
		return 1;
	ws_activate(apps[1].conn);
	ws_window_make_key(apps[1].conn, wins[2].id);

	/* Swap W3 to a second buffer: the first one comes back with
	 * buffer-release, and the commit asks for frame-done. */
	{
		ws_buffer_t *b2 = ws_buffer_create(apps[1].conn, 500, 400, 1);
		ws_rect_t r = { 0, 0, 500, 400 };

		if (b2 == NULL) {
			perror("wstest: second buffer");
			return 1;
		}
		fill(b2, 0xff3050c0, 0xff182860);
		ws_window_attach(apps[1].conn, wins[2].id, b2);
		ws_window_damage(apps[1].conn, wins[2].id, &r, 1);
		ws_window_commit(apps[1].conn, wins[2].id, WS_COMMIT_FRAME_DONE);
	}
	/* Beta's app icon, for the tiles of its minimized windows. */
	if (ws_server_flags(apps[1].conn) & WS_SERVER_MINIMIZE) {
		static uint32_t icon[16 * 16];

		for (i = 0; i < 16 * 16; i++)
			icon[i] = 0xffff00ff;
		if (ws_set_app_icon(apps[1].conn, 16, 16, icon, 16 * 4) < 0)
			perror("wstest: app icon");
	}
	/* Only wsctl and the Dock may see every app's windows. */
	if (ws_list_windows(apps[0].conn, NULL, NULL) < 0)
		printf("list refused app=Alpha %s\n",
		    errno == EPERM ? "EPERM" : strerror(errno));
	else
		printf("list allowed app=Alpha\n");
	for (i = 0; i < NAPPS; i++)
		ws_sync(apps[i].conn);
	printf("ready\n");

	end = timeout > 0 ? time(NULL) + timeout : 0;
	while (!quit && (end == 0 || time(NULL) < end)) {
		for (i = 0; i < NAPPS; i++) {
			pfd[i].fd = ws_connection_fd(apps[i].conn);
			pfd[i].events = POLLIN;
			pfd[i].revents = 0;
		}
		n = poll(pfd, NAPPS, 200);
		if (n < 0 && errno != EINTR)
			break;
		for (i = 0; i < NAPPS; i++) {
			if (ws_pending(apps[i].conn) ||
			    (pfd[i].revents & (POLLIN | POLLHUP))) {
				if (ws_dispatch(apps[i].conn, print_event,
				    &apps[i]) < 0) {
					printf("server closed the connection\n");
					quit = 1;
				}
			}
		}
	}
	for (i = NAPPS; i > 0; i--)
		ws_disconnect(apps[i - 1].conn);
	printf("done\n");
	return 0;
}
