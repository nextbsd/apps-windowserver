/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * wsclear: a WindowServer test client for windows kept clear of the
 * Dock (apps-windowserver#42). It connects as one app, "Clear", whose
 * windows act like GNUstep's: each one draws itself in its colour at
 * whatever frame WindowServer gives it (WS_EVENT_WINDOW_FRAME, then a
 * new buffer and a commit carrying the frame). On a 1280x800 screen:
 *
 *   A1  (100,  0) 400x300  red, along the top edge
 *   B1  (600,450) 400x350  green, along the bottom edge
 *   L1  (  0,300) 300x200  blue, along the left edge
 *   R1  (980,300) 300x200  cyan, along the right edge
 *   T1  (700,  5) 250x790  orange, nearly as tall as the screen
 *   U1  (400, 20) 250x150  purple, the one the user moves
 *   M1  ( 20, 10) 150x200  yellow, a torn-off menu
 *                          (WS_LEVEL_TORN_OFF_MENU)
 *   P1  (900, 10) 200x150  pink, a panel (WS_WINDOW_PANEL)
 *   D0  (  0,  0) 1280x800 dark grey, desktop level (InnerSpace)
 *   Z1  (950,  0) 200x150  white, minimized at once
 *
 * Keys typed into it:
 *
 *   u  the user moves U1 to 420,200 (as a title bar drag ends)
 *   d  the user drags R1 to 1100,300, partly under a right Dock
 *   n  a new window, N1 (grey), at 300,0 300x200
 *
 * It prints every event, one per line, like wstiling;
 * tests/dock-clear-test.sh drives it with wsctl.
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <WindowServer/WindowServer.h>

struct win {
	const char	*name;
	uint32_t	id;
	ws_buffer_t	*buf;
	uint32_t	color;
	ws_rect_t	frame;
};

#define MAXWINS	16

static ws_connection_t *conn;
static struct win wins[MAXWINS];
static int nwins;
static volatile sig_atomic_t quit;

static struct win *
win_by_id(uint32_t id)
{
	int i;

	for (i = 0; i < nwins; i++)
		if (wins[i].id == id && id != 0)
			return &wins[i];
	return NULL;
}

static struct win *
win_by_name(const char *name)
{
	int i;

	for (i = 0; i < nwins; i++)
		if (strcmp(wins[i].name, name) == 0)
			return &wins[i];
	return NULL;
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

/* Draw w at frame f: a new buffer of its size, committed with the frame. */
static void
redraw(struct win *w, const ws_rect_t *f)
{
	ws_buffer_t *b;

	if (f->width <= 0 || f->height <= 0)
		return;
	b = ws_buffer_create(conn, (uint32_t)f->width, (uint32_t)f->height, 1);
	if (b == NULL) {
		fprintf(stderr, "wsclear: buffer for %s: %s\n", w->name,
		    strerror(errno));
		return;
	}
	fill(b, w->color);
	ws_window_attach(conn, w->id, b);
	ws_window_commit_frame(conn, w->id, 0, f);
	if (w->buf != NULL)
		ws_buffer_destroy(conn, w->buf);
	w->buf = b;
	w->frame = *f;
}

static struct win *
make_window(const char *name, int x, int y, int width, int height,
    int32_t level, uint32_t flags, uint32_t color)
{
	ws_rect_t frame = { x, y, width, height };
	struct win *w;

	if (nwins == MAXWINS)
		return NULL;
	w = &wins[nwins++];
	memset(w, 0, sizeof(*w));
	w->name = name;
	w->color = color;
	w->id = ws_window_create(conn, &frame, level, flags, name);
	if (w->id == 0) {
		fprintf(stderr, "wsclear: create %s: %s\n", name,
		    strerror(errno));
		nwins--;
		return NULL;
	}
	redraw(w, &frame);
	ws_window_order(conn, w->id, WS_ORDER_ABOVE, 0);
	printf("window %s id=%u frame=%d,%d,%dx%d\n", name, w->id, x, y,
	    width, height);
	return w;
}

/* The user moved w to x, y: its app sets the frame, as GNUstep does at
 * the end of a title bar drag. */
static void
user_move(const char *name, int32_t x, int32_t y)
{
	struct win *w = win_by_name(name);
	ws_rect_t r;

	if (w == NULL)
		return;
	r = w->frame;
	r.x = x;
	r.y = y;
	w->frame = r;
	ws_window_set_frame(conn, w->id, &r);
	printf("moved %s to %d,%d\n", name, x, y);
}

static void
print_event(ws_connection_t *c, const ws_event_t *ev, void *ctx)
{
	struct win *w = win_by_id(ev->hdr.window);
	char detail[200] = "";

	(void)c;
	(void)ctx;
	switch (ev->hdr.type) {
	case WS_EVENT_WINDOW_FRAME:
		snprintf(detail, sizeof(detail), "frame=%d,%d,%dx%d%s",
		    ev->u.frame.frame.x, ev->u.frame.frame.y,
		    ev->u.frame.frame.width, ev->u.frame.frame.height,
		    (ev->u.frame.flags & WS_FRAME_TILED) ? " tiled" : "");
		break;
	case WS_EVENT_KEY_DOWN:
		snprintf(detail, sizeof(detail), "text=\"%s\"",
		    ev->u.key.text);
		break;
	case WS_EVENT_ERROR:
		snprintf(detail, sizeof(detail), "request=0x%x code=%d %s",
		    ev->u.error.request, ev->u.error.code, ev->u.error.message);
		break;
	default:
		return;		/* only what the test reads */
	}
	printf("event type=%s win=%s %s\n", ws_event_name(ev->hdr.type),
	    w != NULL ? w->name : "-", detail);

	switch (ev->hdr.type) {
	case WS_EVENT_WINDOW_FRAME:
		if (w != NULL)
			redraw(w, &ev->u.frame.frame);
		break;
	case WS_EVENT_KEY_DOWN:
		if (strcmp(ev->u.key.text, "u") == 0)
			user_move("U1", 420, 200);
		else if (strcmp(ev->u.key.text, "d") == 0)
			user_move("R1", 1100, 300);
		else if (strcmp(ev->u.key.text, "n") == 0 &&
		    win_by_name("N1") == NULL)
			make_window("N1", 300, 0, 300, 200, WS_LEVEL_NORMAL, 0,
			    0xff808080);
		break;
	default:
		break;
	}
}

static void
on_signal(int sig)
{
	(void)sig;
	quit = 1;
}

int
main(void)
{
	struct pollfd pfd;
	struct win *z, *a;
	int n;

	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	conn = ws_connect("Clear", 0);
	if (conn == NULL) {
		fprintf(stderr, "wsclear: connect: %s\n", strerror(errno));
		return 1;
	}
	printf("connected client=%u\n", ws_client_id(conn));
	if (make_window("D0", 0, 0, 1280, 800, WS_LEVEL_DESKTOP,
	    WS_WINDOW_NO_KEY, 0xff383838) == NULL ||
	    make_window("B1", 600, 450, 400, 350, WS_LEVEL_NORMAL, 0,
	    0xff30a040) == NULL ||
	    make_window("L1", 0, 300, 300, 200, WS_LEVEL_NORMAL, 0,
	    0xff3050c0) == NULL ||
	    make_window("R1", 980, 300, 300, 200, WS_LEVEL_NORMAL, 0,
	    0xff30c0c0) == NULL ||
	    make_window("T1", 700, 5, 250, 790, WS_LEVEL_NORMAL, 0,
	    0xffe08020) == NULL ||
	    make_window("U1", 400, 20, 250, 150, WS_LEVEL_NORMAL, 0,
	    0xff8030c0) == NULL ||
	    make_window("M1", 20, 10, 150, 200, WS_LEVEL_TORN_OFF_MENU,
	    WS_WINDOW_NO_KEY, 0xffc0c030) == NULL ||
	    make_window("P1", 900, 10, 200, 150, WS_LEVEL_NORMAL,
	    WS_WINDOW_PANEL, 0xffe0a0c0) == NULL ||
	    (z = make_window("Z1", 950, 0, 200, 150, WS_LEVEL_NORMAL, 0,
	    0xffffffff)) == NULL ||
	    (a = make_window("A1", 100, 0, 400, 300, WS_LEVEL_NORMAL, 0,
	    0xffc03030)) == NULL)
		return 1;
	ws_window_minimize(conn, z->id);
	ws_activate(conn);
	ws_window_make_key(conn, a->id);
	ws_sync(conn);
	printf("ready\n");

	while (!quit) {
		pfd.fd = ws_connection_fd(conn);
		pfd.events = POLLIN;
		pfd.revents = 0;
		n = poll(&pfd, 1, 200);
		if (n < 0 && errno != EINTR)
			break;
		if (ws_pending(conn) || (pfd.revents & (POLLIN | POLLHUP))) {
			if (ws_dispatch(conn, print_event, NULL) < 0) {
				printf("server closed the connection\n");
				quit = 1;
			}
		}
	}
	ws_disconnect(conn);
	printf("done\n");
	return 0;
}
