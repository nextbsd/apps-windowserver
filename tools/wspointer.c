/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * wspointer: a WindowServer test client for pointer grabs, warps and
 * queries, per-window cursors, input regions, frames carried in a
 * commit and window ids chosen by the client (WS_SERVER_POINTER_GRAB,
 * _POINTER_WARP, _WINDOW_CURSOR, _INPUT_REGION, _COMMIT_FRAME and
 * _WINDOW_IDS). It connects as two apps and prints every event they
 * get, one per line, like wstest:
 *
 *   Other  O1  normal level      (900,450) 300x300  blue
 *   Xeno   E1  normal level      (100,100) 400x300  red, magenta 8x8
 *                                                    cursor of its own
 *   Xeno   E2  popup-menu level  (600,100) 150x150  green (never key,
 *                                                    never activates)
 *   Xeno   E3  normal level      (700,500) 300x150  yellow, taking
 *                                                    pointer events in
 *                                                    its left half only
 *
 * Xeno creates its windows without a round trip each
 * (ws_window_create_nowait) and tracks the pointer while it is active
 * (ws_pointer_track). Keys typed into Xeno:
 *
 *   g  grab the pointer (WS_GRAB_POINTER)    o  popup grab (WS_GRAB_OUTSIDE)
 *   u  ungrab                                w  warp to 300,200 in E1
 *   q  query the pointer                     r  resize E1 to 500x350,
 *   s  E3 takes pointer events everywhere       orange, in one commit
 *      again
 *
 * SIGUSR1: Other tries to grab the pointer. SIGUSR2: Xeno warps to
 * 50,50 on the screen and queries the pointer. tests/pointer-test.sh
 * drives it with wsctl.
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <WindowServer/WindowServer.h>

struct app {
	const char	*name;
	ws_connection_t	*conn;
};

struct win {
	const char	*name;
	uint32_t	id;
	ws_buffer_t	*buf;
};

enum { OTHER, XENO, NAPPS };
enum { O1, E1, E2, E3, NWINS };

static struct app apps[NAPPS] = { { "Other", NULL }, { "Xeno", NULL } };
static struct win wins[NWINS];
static volatile sig_atomic_t quit, usr1, usr2;

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
fill(ws_buffer_t *b, uint32_t argb)
{
	uint32_t *px = ws_buffer_data(b);
	uint32_t w = ws_buffer_width(b), h = ws_buffer_height(b);
	uint32_t stride = ws_buffer_stride(b) / 4, x, y;

	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++)
			px[y * stride + x] = argb;
}

static int
make_window(struct win *w, struct app *app, const char *name, int x, int y,
    int width, int height, int32_t level, uint32_t flags, uint32_t color,
    int nowait)
{
	ws_rect_t frame = { x, y, width, height };

	w->name = name;
	w->id = nowait ?
	    ws_window_create_nowait(app->conn, &frame, level, flags, name) :
	    ws_window_create(app->conn, &frame, level, flags, name);
	if (w->id == 0) {
		fprintf(stderr, "wspointer: create %s: %s\n", name,
		    strerror(errno));
		return -1;
	}
	w->buf = ws_buffer_create(app->conn, (uint32_t)width,
	    (uint32_t)height, 1);
	if (w->buf == NULL) {
		fprintf(stderr, "wspointer: buffer for %s: %s\n", name,
		    strerror(errno));
		return -1;
	}
	fill(w->buf, color);
	if (ws_window_attach(app->conn, w->id, w->buf) < 0 ||
	    ws_window_commit(app->conn, w->id, 0) < 0 ||
	    ws_window_order(app->conn, w->id, WS_ORDER_ABOVE, 0) < 0) {
		fprintf(stderr, "wspointer: show %s: %s\n", name,
		    strerror(errno));
		return -1;
	}
	printf("window %s id=%u app=%s nowait=%s frame=%d,%d,%dx%d\n", name,
	    w->id, app->name, nowait ? "yes" : "no", x, y, width, height);
	return 0;
}

static void
query(void)
{
	ws_pointer_info_t pi;

	if (ws_query_pointer(apps[XENO].conn, &pi) < 0) {
		printf("query refused app=Xeno %s\n",
		    errno == EPERM ? "EPERM" : strerror(errno));
		return;
	}
	printf("query app=Xeno x=%.0f y=%.0f buttons=0x%x win=%s\n", pi.x,
	    pi.y, pi.buttons, win_name(pi.window));
}

static void
grab(struct app *app, uint32_t mode)
{
	if (ws_pointer_grab(app->conn, mode) == 0)
		printf("grab ok app=%s mode=%s\n", app->name,
		    mode == WS_GRAB_POINTER ? "pointer" : "outside");
	else
		printf("grab refused app=%s %s\n", app->name,
		    errno == EPERM ? "EPERM" : errno == EBUSY ? "EBUSY" :
		    strerror(errno));
}

/* E1 becomes 500x350 and orange: new frame and new buffer together. */
static void
resize_e1(void)
{
	ws_connection_t *c = apps[XENO].conn;
	ws_rect_t frame = { 100, 100, 500, 350 };
	ws_buffer_t *b;

	b = ws_buffer_create(c, 500, 350, 1);
	if (b == NULL) {
		perror("wspointer: E1's new buffer");
		return;
	}
	fill(b, 0xffe0a020);
	ws_window_attach(c, wins[E1].id, b);
	ws_window_commit_frame(c, wins[E1].id, 0, &frame);
	ws_buffer_destroy(c, wins[E1].buf);
	wins[E1].buf = b;
	printf("resized E1 to 500x350 in one commit\n");
}

static void
xeno_key(const char *text)
{
	ws_connection_t *c = apps[XENO].conn;

	if (strcmp(text, "g") == 0)
		grab(&apps[XENO], WS_GRAB_POINTER);
	else if (strcmp(text, "o") == 0)
		grab(&apps[XENO], WS_GRAB_OUTSIDE);
	else if (strcmp(text, "u") == 0) {
		ws_pointer_ungrab(c);
		printf("ungrabbed app=Xeno\n");
	} else if (strcmp(text, "w") == 0) {
		ws_warp_pointer(c, wins[E1].id, 300, 200);
		printf("warped app=Xeno to E1 300,200\n");
	} else if (strcmp(text, "q") == 0)
		query();
	else if (strcmp(text, "r") == 0)
		resize_e1();
	else if (strcmp(text, "s") == 0) {
		ws_window_set_input_region(c, wins[E3].id, NULL, 0);
		printf("E3 takes pointer events everywhere\n");
	}
}

static void
print_event(ws_connection_t *conn, const ws_event_t *ev, void *ctx)
{
	struct app *app = ctx;
	char detail[200] = "";

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
	case WS_EVENT_POINTER_OUTSIDE:
		snprintf(detail, sizeof(detail),
		    "x=%.0f y=%.0f screen=%.0f,%.0f button=%u buttons=0x%x",
		    ev->u.pointer.x, ev->u.pointer.y, ev->u.pointer.screen_x,
		    ev->u.pointer.screen_y, ev->u.pointer.button,
		    ev->u.pointer.buttons);
		break;
	case WS_EVENT_SCROLL:
		snprintf(detail, sizeof(detail), "x=%.0f y=%.0f dy=%.0f",
		    ev->u.scroll.x, ev->u.scroll.y, ev->u.scroll.dy);
		break;
	case WS_EVENT_KEY_DOWN:
	case WS_EVENT_KEY_UP:
		snprintf(detail, sizeof(detail), "text=\"%s\"",
		    ev->u.key.text);
		break;
	case WS_EVENT_ERROR:
		snprintf(detail, sizeof(detail), "request=0x%x code=%d %s",
		    ev->u.error.request, ev->u.error.code, ev->u.error.message);
		break;
	default:
		break;
	}
	printf("event serial=%u app=%s type=%s win=%s %s\n", ev->hdr.serial,
	    app->name, ws_event_name(ev->hdr.type), win_name(ev->hdr.window),
	    detail);
	if (app == &apps[XENO] && ev->hdr.type == WS_EVENT_KEY_DOWN)
		xeno_key(ev->u.key.text);
}

static void
on_signal(int sig)
{
	if (sig == SIGUSR1)
		usr1 = 1;
	else if (sig == SIGUSR2)
		usr2 = 1;
	else
		quit = 1;
}

int
main(void)
{
	static const uint32_t needed = WS_SERVER_POINTER_GRAB |
	    WS_SERVER_POINTER_WARP | WS_SERVER_WINDOW_CURSOR |
	    WS_SERVER_INPUT_REGION | WS_SERVER_COMMIT_FRAME |
	    WS_SERVER_WINDOW_IDS;
	ws_rect_t left = { 0, 0, 150, 150 };
	struct pollfd pfd[NAPPS];
	ws_buffer_t *cursor;
	int i, n;

	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGUSR1, on_signal);
	signal(SIGUSR2, on_signal);

	for (i = 0; i < NAPPS; i++) {
		apps[i].conn = ws_connect(apps[i].name, 0);
		if (apps[i].conn == NULL) {
			fprintf(stderr, "wspointer: connect %s: %s\n",
			    apps[i].name, strerror(errno));
			return 1;
		}
		printf("connected app=%s client=%u\n", apps[i].name,
		    ws_client_id(apps[i].conn));
	}
	if ((ws_server_flags(apps[XENO].conn) & needed) != needed) {
		fprintf(stderr, "wspointer: the server lacks features "
		    "(flags 0x%x)\n", ws_server_flags(apps[XENO].conn));
		return 1;
	}
	if (make_window(&wins[O1], &apps[OTHER], "O1", 900, 450, 300, 300,
	    WS_LEVEL_NORMAL, 0, 0xff3050c0, 0) < 0 ||
	    make_window(&wins[E1], &apps[XENO], "E1", 100, 100, 400, 300,
	    WS_LEVEL_NORMAL, 0, 0xffc03030, 1) < 0 ||
	    make_window(&wins[E2], &apps[XENO], "E2", 600, 100, 150, 150,
	    WS_LEVEL_POPUP_MENU, WS_WINDOW_NO_KEY | WS_WINDOW_NO_ACTIVATE,
	    0xff30a040, 1) < 0 ||
	    make_window(&wins[E3], &apps[XENO], "E3", 700, 500, 300, 150,
	    WS_LEVEL_NORMAL, 0, 0xffc0c030, 1) < 0)
		return 1;

	/* E1's own cursor: an 8x8 magenta square, hot spot at its corner. */
	cursor = ws_buffer_create(apps[XENO].conn, 8, 8, 1);
	if (cursor == NULL) {
		perror("wspointer: cursor buffer");
		return 1;
	}
	fill(cursor, 0xffff00ff);
	if (ws_window_set_cursor(apps[XENO].conn, wins[E1].id,
	    WS_CURSOR_CUSTOM, cursor, 0, 0) < 0)
		perror("wspointer: E1's cursor");
	ws_buffer_destroy(apps[XENO].conn, cursor);	/* the server keeps it */
	if (ws_window_set_input_region(apps[XENO].conn, wins[E3].id, &left,
	    1) < 0)
		perror("wspointer: E3's input region");
	if (ws_pointer_track(apps[XENO].conn, 1) < 0)
		perror("wspointer: tracking");
	for (i = 0; i < NAPPS; i++)
		ws_sync(apps[i].conn);
	printf("ready\n");

	while (!quit) {
		if (usr1) {
			usr1 = 0;
			grab(&apps[OTHER], WS_GRAB_POINTER);
		}
		if (usr2) {
			usr2 = 0;
			ws_warp_pointer(apps[XENO].conn, 0, 50, 50);
			printf("warped app=Xeno to 50,50\n");
			query();
		}
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
