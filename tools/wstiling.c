/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * wstiling: a WindowServer test client for window tiling
 * (WS_SERVER_TILING). It connects as two apps whose windows act like
 * GNUstep's: each one draws itself in its colour at whatever size
 * WindowServer gives it (WS_EVENT_WINDOW_FRAME, then a new buffer and a
 * commit carrying the frame), and moves with the pointer while a button
 * pressed in it is held, as a title bar drag does. It prints every
 * event, one per line, like wstest:
 *
 *   Tiler  T1  (100,100) 400x300  red
 *   Tiler  T2  (200,150) 400x300  green
 *   Tiler  T3  (300,200) 400x300  blue
 *   Tiler  P1  (900, 60) 200x150  yellow, a panel (WS_WINDOW_PANEL)
 *   Other  O1  (500,400) 400x300  cyan
 *   Other  N1  (950,550) 250x200  orange, never tiled (WS_WINDOW_NO_TILE)
 *
 * Keys typed into Tiler:
 *
 *   n  a new window, T4 (grey)         x  close the key window
 *   m  minimize the key window         h  hide T2, u  show it again
 *   b  show M1 (purple), which is 900x700 at least
 *   k  show S1 (brown), which takes 1000x700 whatever it is given
 *   c  show C1 (lilac), which sizes itself in steps of 50 pixels
 *   o  show X1 (white), which only moves when WindowServer gives it a
 *      new frame and keeps drawing at its own size, as an X server's
 *      windows do; it draws again every 200 ms
 *
 * On WS_EVENT_WINDOW_RESTORE it orders the window in again.
 *
 * wstiling -e starts with one window, W1 in Tiler, as a user opening
 * Terminal windows one by one, and takes these keys in either app:
 *
 *   w  a new window in Tiler, W2 to W8 (made key, as a new window is)
 *   v  a new window in Other, V1 to V3 (Other activates itself)
 *   p  a panel, Q1 (yellow), shown but not made key
 *   x  close the key window
 *   g  grab the pointer for outside presses (a popup menu)
 *   G  end the grab
 *
 * tests/tiling-test.sh drives it with wsctl.
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
	uint32_t	key;		/* its key window */
};

enum { PLAIN, STUBBORN, STEPS, MOVER };

struct win {
	const char	*name;
	struct app	*app;
	uint32_t	id;
	ws_buffer_t	*buf;
	uint32_t	color;
	int		kind;
	ws_rect_t	frame;
	/* a drag: where the pointer and the window were at the press */
	int		dragging;
	double		press_x;
	double		press_y;
	ws_rect_t	press_frame;
};

enum { TILER, OTHER, NAPPS };

#define MAXWINS	16

static struct app apps[NAPPS] = { { "Tiler", NULL, 0 }, { "Other", NULL, 0 } };
static struct win wins[MAXWINS];
static int nwins;
static int empty;		/* -e */
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

static const char *
win_name(uint32_t id)
{
	struct win *w = win_by_id(id);

	return id == 0 ? "-" : w != NULL ? w->name : "?";
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
	ws_connection_t *c = w->app->conn;
	ws_buffer_t *b;

	if (f->width <= 0 || f->height <= 0)
		return;
	b = ws_buffer_create(c, (uint32_t)f->width, (uint32_t)f->height, 1);
	if (b == NULL) {
		fprintf(stderr, "wstiling: buffer for %s: %s\n", w->name,
		    strerror(errno));
		return;
	}
	fill(b, w->color);
	ws_window_attach(c, w->id, b);
	ws_window_commit_frame(c, w->id, 0, f);
	if (w->buf != NULL)
		ws_buffer_destroy(c, w->buf);
	w->buf = b;
	w->frame = *f;
}

static struct win *
make_window(struct app *app, const char *name, int x, int y, int width,
    int height, uint32_t flags, uint32_t color, int kind, int show)
{
	ws_rect_t frame = { x, y, width, height };
	struct win *w;

	if (nwins == MAXWINS)
		return NULL;
	w = &wins[nwins++];
	memset(w, 0, sizeof(*w));
	w->name = name;
	w->app = app;
	w->color = color;
	w->kind = kind;
	w->id = ws_window_create(app->conn, &frame, WS_LEVEL_NORMAL, flags,
	    name);
	if (w->id == 0) {
		fprintf(stderr, "wstiling: create %s: %s\n", name,
		    strerror(errno));
		nwins--;
		return NULL;
	}
	redraw(w, &frame);
	if (show)
		ws_window_order(app->conn, w->id, WS_ORDER_ABOVE, 0);
	printf("window %s id=%u app=%s frame=%d,%d,%dx%d\n", name, w->id,
	    app->name, x, y, width, height);
	return w;
}

static void
show(struct win *w)
{
	if (w == NULL)
		return;
	ws_window_order(w->app->conn, w->id, WS_ORDER_ABOVE, 0);
	ws_window_make_key(w->app->conn, w->id);
	printf("shown %s\n", w->name);
}

static void
tiler_key(const char *text)
{
	ws_connection_t *c = apps[TILER].conn;
	struct win *w = win_by_id(apps[TILER].key), *m;

	if (strcmp(text, "n") == 0 && win_by_name("T4") == NULL)
		show(make_window(&apps[TILER], "T4", 150, 120, 300, 250, 0,
		    0xffa0a0a0, PLAIN, 0));
	else if (strcmp(text, "x") == 0 && w != NULL) {
		printf("closed %s\n", w->name);
		ws_window_destroy(c, w->id);
		w->id = 0;
		apps[TILER].key = 0;
	} else if (strcmp(text, "m") == 0 && w != NULL) {
		ws_window_minimize(c, w->id);
		printf("minimized %s\n", w->name);
	} else if (strcmp(text, "h") == 0 && (m = win_by_name("T2")) != NULL) {
		ws_window_order(c, m->id, WS_ORDER_OUT, 0);
		printf("hid T2\n");
	} else if (strcmp(text, "u") == 0 && (m = win_by_name("T2")) != NULL)
		show(m);
	else if (strcmp(text, "b") == 0 && win_by_name("M1") == NULL) {
		m = make_window(&apps[TILER], "M1", 200, 50, 900, 700, 0,
		    0xff8030c0, PLAIN, 0);
		if (m != NULL) {
			ws_window_set_size_limits(c, m->id, 900, 700, 0, 0);
			show(m);
		}
	} else if (strcmp(text, "k") == 0 && win_by_name("S1") == NULL)
		show(make_window(&apps[TILER], "S1", 100, 50, 1000, 700, 0,
		    0xff806040, STUBBORN, 0));
	else if (strcmp(text, "c") == 0 && win_by_name("C1") == NULL)
		show(make_window(&apps[TILER], "C1", 400, 300, 300, 200, 0,
		    0xffb0a0e0, STEPS, 0));
	else if (strcmp(text, "o") == 0 && win_by_name("X1") == NULL)
		show(make_window(&apps[TILER], "X1", 60, 500, 250, 150, 0,
		    0xffffffff, MOVER, 0));
}

/* -e: the next of names[] not shown yet, as a new window of app. */
static void
open_next(struct app *app, const char *const *names, int n, uint32_t color)
{
	int i;

	for (i = 0; i < n; i++)
		if (win_by_name(names[i]) == NULL) {
			show(make_window(app, names[i], 150 + 30 * i,
			    120 + 20 * i, 500, 350, 0, color + 0x101010 * i,
			    PLAIN, 0));
			return;
		}
}

/* -e: a key typed into either app. */
static void
empty_key(struct app *app, const char *text)
{
	static const char *const tnames[] = { "W2", "W3", "W4", "W5", "W6",
	    "W7", "W8" };
	static const char *const onames[] = { "V1", "V2", "V3" };
	struct win *w = win_by_id(app->key);

	if (strcmp(text, "w") == 0)
		open_next(&apps[TILER], tnames, 7, 0xff304060);
	else if (strcmp(text, "v") == 0) {
		/* the other app opens a window, as the user asked it to */
		ws_activate(apps[OTHER].conn);
		open_next(&apps[OTHER], onames, 3, 0xff603040);
	}
	else if (strcmp(text, "p") == 0 && win_by_name("Q1") == NULL)
		make_window(app, "Q1", 560, 300, 160, 120, WS_WINDOW_PANEL,
		    0xffc0c030, PLAIN, 1);
	else if (strcmp(text, "x") == 0 && w != NULL) {
		printf("closed %s\n", w->name);
		ws_window_destroy(app->conn, w->id);
		w->id = 0;
		app->key = 0;
	} else if (strcmp(text, "g") == 0)
		printf("grab app=%s %d\n", app->name,
		    ws_pointer_grab(app->conn, WS_GRAB_OUTSIDE));
	else if (strcmp(text, "G") == 0)
		printf("ungrab app=%s %d\n", app->name,
		    ws_pointer_ungrab(app->conn));
}

/* WindowServer gave w a frame: draw it at that size, as GNUstep does. */
static void
new_frame(struct win *w, const ws_rect_t *f)
{
	ws_rect_t r = *f;

	if (w->kind == MOVER) {
		w->frame.x = f->x;	/* it moves, nothing else */
		w->frame.y = f->y;
		return;
	}
	if (w->kind == STUBBORN) {
		r.width = 1000;
		r.height = 700;
	} else if (w->kind == STEPS) {
		r.width -= r.width % 50;
		r.height -= r.height % 50;
	}
	redraw(w, &r);
}

static void
print_event(ws_connection_t *conn, const ws_event_t *ev, void *ctx)
{
	struct app *app = ctx;
	struct win *w = win_by_id(ev->hdr.window);
	char detail[200] = "";
	ws_rect_t r;

	(void)conn;
	switch (ev->hdr.type) {
	case WS_EVENT_WINDOW_FRAME:
		snprintf(detail, sizeof(detail), "frame=%d,%d,%dx%d%s",
		    ev->u.frame.frame.x, ev->u.frame.frame.y,
		    ev->u.frame.frame.width, ev->u.frame.frame.height,
		    (ev->u.frame.flags & WS_FRAME_TILED) ? " tiled" : "");
		break;
	case WS_EVENT_MOUSE_DOWN:
	case WS_EVENT_MOUSE_UP:
	case WS_EVENT_MOUSE_DRAGGED:
		snprintf(detail, sizeof(detail), "x=%.0f y=%.0f screen=%.0f,%.0f",
		    ev->u.pointer.x, ev->u.pointer.y, ev->u.pointer.screen_x,
		    ev->u.pointer.screen_y);
		break;
	case WS_EVENT_KEY_DOWN:
	case WS_EVENT_KEY_UP:
		snprintf(detail, sizeof(detail), "sym=0x%x text=\"%s\" mods=0x%x",
		    ev->u.key.keysym, ev->u.key.text, ev->u.key.modifiers);
		break;
	case WS_EVENT_ERROR:
		snprintf(detail, sizeof(detail), "request=0x%x code=%d %s",
		    ev->u.error.request, ev->u.error.code, ev->u.error.message);
		break;
	case WS_EVENT_MOUSE_MOVED:
	case WS_EVENT_MOUSE_ENTERED:
	case WS_EVENT_MOUSE_EXITED:
	case WS_EVENT_FRAME_DONE:
	case WS_EVENT_BUFFER_RELEASE:
	case WS_EVENT_USABLE_AREA:
	case WS_EVENT_FLAGS_CHANGED:
		return;		/* too many to be useful */
	default:
		break;
	}
	printf("event app=%s type=%s win=%s %s\n", app->name,
	    ws_event_name(ev->hdr.type), win_name(ev->hdr.window), detail);

	switch (ev->hdr.type) {
	case WS_EVENT_WINDOW_FRAME:
		if (w != NULL)
			new_frame(w, &ev->u.frame.frame);
		break;
	case WS_EVENT_KEY_WINDOW_GAINED:
		app->key = ev->hdr.window;
		break;
	case WS_EVENT_WINDOW_RESTORE:
		if (w != NULL)
			show(w);
		break;
	case WS_EVENT_MOUSE_DOWN:
		if (w != NULL && ev->u.pointer.button == WS_BUTTON_LEFT) {
			w->dragging = 1;
			w->press_x = ev->u.pointer.screen_x;
			w->press_y = ev->u.pointer.screen_y;
			w->press_frame = w->frame;
		}
		break;
	case WS_EVENT_MOUSE_DRAGGED:
		/* a title bar drag: the app moves its window itself */
		if (w != NULL && w->dragging) {
			r = w->press_frame;
			r.x += (int32_t)(ev->u.pointer.screen_x - w->press_x);
			r.y += (int32_t)(ev->u.pointer.screen_y - w->press_y);
			w->frame = r;
			ws_window_set_frame(w->app->conn, w->id, &r);
		}
		break;
	case WS_EVENT_MOUSE_UP:
		if (w != NULL)
			w->dragging = 0;
		break;
	case WS_EVENT_KEY_DOWN:
		if (empty)
			empty_key(app, ev->u.key.text);
		else if (app == &apps[TILER])
			tiler_key(ev->u.key.text);
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
main(int argc, char **argv)
{
	struct pollfd pfd[NAPPS];
	int i, n;

	empty = argc == 2 && strcmp(argv[1], "-e") == 0;
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	for (i = 0; i < NAPPS; i++) {
		apps[i].conn = ws_connect(apps[i].name, 0);
		if (apps[i].conn == NULL) {
			fprintf(stderr, "wstiling: connect %s: %s\n",
			    apps[i].name, strerror(errno));
			return 1;
		}
		printf("connected app=%s client=%u\n", apps[i].name,
		    ws_client_id(apps[i].conn));
	}
	if ((ws_server_flags(apps[TILER].conn) & WS_SERVER_TILING) == 0) {
		fprintf(stderr, "wstiling: the server can't tile (flags 0x%x)\n",
		    ws_server_flags(apps[TILER].conn));
		return 1;
	}
	if (empty) {
		if (make_window(&apps[TILER], "W1", 120, 100, 500, 350, 0,
		    0xff304060, PLAIN, 1) == NULL)
			return 1;
		ws_activate(apps[TILER].conn);
		ws_window_make_key(apps[TILER].conn, win_by_name("W1")->id);
	} else if (make_window(&apps[OTHER], "O1", 500, 400, 400, 300, 0,
	    0xff30c0c0, PLAIN, 1) == NULL ||
	    make_window(&apps[OTHER], "N1", 950, 550, 250, 200,
	    WS_WINDOW_NO_TILE, 0xffe08020, PLAIN, 1) == NULL ||
	    make_window(&apps[TILER], "T1", 100, 100, 400, 300, 0, 0xffc03030,
	    PLAIN, 1) == NULL ||
	    make_window(&apps[TILER], "T2", 200, 150, 400, 300, 0, 0xff30a040,
	    PLAIN, 1) == NULL ||
	    make_window(&apps[TILER], "T3", 300, 200, 400, 300, 0, 0xff3050c0,
	    PLAIN, 1) == NULL ||
	    make_window(&apps[TILER], "P1", 900, 60, 200, 150, WS_WINDOW_PANEL,
	    0xffc0c030, PLAIN, 1) == NULL)
		return 1;
	if (!empty) {
		ws_activate(apps[TILER].conn);
		ws_window_make_key(apps[TILER].conn, win_by_name("T3")->id);
	}
	for (i = 0; i < NAPPS; i++)
		ws_sync(apps[i].conn);
	printf("ready\n");

	while (!quit) {
		for (i = 0; i < NAPPS; i++) {
			pfd[i].fd = ws_connection_fd(apps[i].conn);
			pfd[i].events = POLLIN;
			pfd[i].revents = 0;
		}
		n = poll(pfd, NAPPS, 200);
		if (n < 0 && errno != EINTR)
			break;
		/* X1 draws again, at its own size, with no frame */
		for (i = 0; i < nwins; i++)
			if (wins[i].kind == MOVER && wins[i].id != 0) {
				ws_rect_t all = { 0, 0, 250, 150 };

				ws_window_attach(wins[i].app->conn, wins[i].id,
				    wins[i].buf);
				ws_window_damage(wins[i].app->conn, wins[i].id,
				    &all, 1);
				ws_window_commit(wins[i].app->conn, wins[i].id,
				    0);
			}
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
