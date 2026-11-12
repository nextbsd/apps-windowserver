/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * wsdrag: a WindowServer test client for drag and drop (WS_SERVER_DRAG,
 * "Drag and drop" in WSProtocol.h). It connects as two apps and prints
 * every event they get, one per line, like wspointer:
 *
 *   Alpha  A1  (100,100) 300x200  red     drags start here; takes
 *                                          drags, answers Move
 *   Alpha  A2  (100,400) 300x200  orange  takes drags, answers Move
 *   Beta   B1  (600,100) 300x200  green   takes drags, answers Copy
 *   Beta   B2  (600,400) 300x200  blue    takes drags, answers Link
 *   Beta   B3  (950,100) 250x200  grey    doesn't take drags
 *   Beta   B4  (950,400) 250x200  purple  takes drags, answers none
 *
 * A press in A1 starts a drag at once (no drag threshold): a 40x40
 * magenta image held at its middle, Move, Copy, Link and Generic onto
 * Alpha's windows, Copy, Link and Generic onto Beta's, sliding back if
 * nobody takes it, NSFilenamesPboardType and NSStringPboardType on
 * NSDragPboard. Keys typed into Alpha:
 *
 *   b  try to start a drag with no button held (expect EPERM)
 *   c  cancel the drag under way
 *
 * SIGUSR1: Beta tries to start a drag (expect EPERM: not its button).
 * tests/drag-test.sh drives it with wsctl.
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
	int		app;
	uint32_t	id;
	ws_buffer_t	*buf;
	uint32_t	answer;		/* operation it takes, of those offered */
};

enum { ALPHA, BETA, NAPPS };
enum { A1, A2, B1, B2, B3, B4, NWINS };

static struct app apps[NAPPS] = { { "Alpha", NULL }, { "Beta", NULL } };
static struct win wins[NWINS];
static volatile sig_atomic_t quit, usr1;
static uint32_t session;	/* Alpha's drag under way */

static struct win *
win_by_id(uint32_t id)
{
	size_t i;

	for (i = 0; i < NWINS; i++)
		if (wins[i].id == id && id != 0)
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

static int
make_window(int idx, int app, const char *name, int x, int y, int width,
    int height, uint32_t flags, uint32_t color, uint32_t answer)
{
	struct win *w = &wins[idx];
	ws_connection_t *c = apps[app].conn;
	ws_rect_t frame = { x, y, width, height };

	w->name = name;
	w->app = app;
	w->answer = answer;
	w->id = ws_window_create(c, &frame, WS_LEVEL_NORMAL, flags, name);
	if (w->id == 0) {
		fprintf(stderr, "wsdrag: create %s: %s\n", name,
		    strerror(errno));
		return -1;
	}
	w->buf = ws_buffer_create(c, (uint32_t)width, (uint32_t)height, 1);
	if (w->buf == NULL) {
		fprintf(stderr, "wsdrag: buffer for %s: %s\n", name,
		    strerror(errno));
		return -1;
	}
	fill(w->buf, color);
	if (ws_window_attach(c, w->id, w->buf) < 0 ||
	    ws_window_commit(c, w->id, 0) < 0 ||
	    ws_window_order(c, w->id, WS_ORDER_ABOVE, 0) < 0) {
		fprintf(stderr, "wsdrag: show %s: %s\n", name,
		    strerror(errno));
		return -1;
	}
	printf("window %s id=%u app=%s frame=%d,%d,%dx%d\n", name, w->id,
	    apps[app].name, x, y, width, height);
	return 0;
}

static const char *
err_name(int e)
{
	return e == EPERM ? "EPERM" : e == EBUSY ? "EBUSY" :
	    e == EINVAL ? "EINVAL" : e == ENOENT ? "ENOENT" : strerror(e);
}

/* Start a drag from app a, the pointer at screen sx, sy. */
static void
begin(int a, uint32_t window, double sx, double sy)
{
	static const char *const types[] = { "NSFilenamesPboardType",
	    "NSStringPboardType" };
	ws_connection_t *c = apps[a].conn;
	ws_drag_params_t p;
	ws_buffer_t *img;
	uint32_t s = 0;
	int r;

	img = ws_buffer_create(c, 40, 40, 1);
	if (img == NULL) {
		perror("wsdrag: drag image");
		return;
	}
	fill(img, 0xffff00ff);
	memset(&p, 0, sizeof(p));
	p.window = window;
	p.image = img;
	p.hot_x = 20;
	p.hot_y = 20;
	p.ops_local = WS_DRAG_OP_MOVE | WS_DRAG_OP_COPY | WS_DRAG_OP_LINK |
	    WS_DRAG_OP_GENERIC;
	p.ops_external = WS_DRAG_OP_COPY | WS_DRAG_OP_LINK | WS_DRAG_OP_GENERIC;
	p.flags = WS_DRAG_SLIDE_BACK;
	p.slide_x = (int32_t)sx - 20;
	p.slide_y = (int32_t)sy - 20;
	p.pasteboard = "NSDragPboard";
	p.types = types;
	p.ntypes = 2;
	r = ws_drag_begin(c, &p, &s);
	ws_buffer_destroy(c, img);	/* the server keeps a copy */
	if (r < 0) {
		printf("drag-begin app=%s refused %s\n", apps[a].name,
		    err_name(errno));
		return;
	}
	if (a == ALPHA)
		session = s;
	printf("drag-begin app=%s ok session=%u\n", apps[a].name, s);
}

static void
print_event(ws_connection_t *conn, const ws_event_t *ev, void *ctx)
{
	struct app *app = ctx;
	struct win *w;
	char detail[200] = "";
	uint32_t op;

	switch (ev->hdr.type) {
	case WS_EVENT_MOUSE_DOWN:
	case WS_EVENT_MOUSE_UP:
	case WS_EVENT_MOUSE_DRAGGED:
	case WS_EVENT_MOUSE_ENTERED:
	case WS_EVENT_MOUSE_EXITED:
		snprintf(detail, sizeof(detail),
		    "x=%.0f y=%.0f screen=%.0f,%.0f button=%u buttons=0x%x",
		    ev->u.pointer.x, ev->u.pointer.y, ev->u.pointer.screen_x,
		    ev->u.pointer.screen_y, ev->u.pointer.button,
		    ev->u.pointer.buttons);
		break;
	case WS_EVENT_MOUSE_MOVED:
		return;		/* too many */
	case WS_EVENT_KEY_DOWN:
		snprintf(detail, sizeof(detail), "text=\"%s\"",
		    ev->u.key.text);
		break;
	case WS_EVENT_DRAG:
		snprintf(detail, sizeof(detail),
		    "phase=%s session=%u x=%.0f y=%.0f ops=0x%x src=%u "
		    "pb=%s",
		    ev->u.drag.phase == WS_DRAG_ENTER ? "enter" :
		    ev->u.drag.phase == WS_DRAG_UPDATE ? "update" :
		    ev->u.drag.phase == WS_DRAG_EXIT ? "exit" :
		    ev->u.drag.phase == WS_DRAG_DROP ? "drop" : "?",
		    ev->u.drag.session, ev->u.drag.x, ev->u.drag.y,
		    ev->u.drag.operations, ev->u.drag.source_app,
		    ev->u.drag.pasteboard);
		break;
	case WS_EVENT_DRAG_STATUS:
		snprintf(detail, sizeof(detail),
		    "session=%u target=%s op=0x%x ops=0x%x",
		    ev->u.drag_status.session,
		    win_name(ev->u.drag_status.target),
		    ev->u.drag_status.operation, ev->u.drag_status.operations);
		break;
	case WS_EVENT_DRAG_END:
		snprintf(detail, sizeof(detail),
		    "session=%u op=0x%x target=%s%s%s%s",
		    ev->u.drag_end.session, ev->u.drag_end.operation,
		    win_name(ev->u.drag_end.target),
		    (ev->u.drag_end.flags & WS_DRAG_END_DROPPED) ?
		    " dropped" : "",
		    (ev->u.drag_end.flags & WS_DRAG_END_CANCELLED) ?
		    " cancelled" : "",
		    (ev->u.drag_end.flags & WS_DRAG_END_SLID_BACK) ?
		    " slid-back" : "");
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

	switch (ev->hdr.type) {
	case WS_EVENT_MOUSE_DOWN:
		if (app == &apps[ALPHA] && ev->hdr.window == wins[A1].id &&
		    ev->u.pointer.button == WS_BUTTON_LEFT)
			begin(ALPHA, wins[A1].id, ev->u.pointer.screen_x,
			    ev->u.pointer.screen_y);
		break;
	case WS_EVENT_KEY_DOWN:
		if (app != &apps[ALPHA])
			break;
		if (strcmp(ev->u.key.text, "b") == 0)
			begin(ALPHA, 0, 500, 300);
		else if (strcmp(ev->u.key.text, "c") == 0) {
			ws_drag_cancel(conn, session);
			printf("drag-cancel app=Alpha session=%u\n", session);
		}
		break;
	case WS_EVENT_DRAG:
		w = win_by_id(ev->hdr.window);
		if (w == NULL || (ev->u.drag.phase != WS_DRAG_ENTER &&
		    ev->u.drag.phase != WS_DRAG_UPDATE))
			break;
		op = w->answer & ev->u.drag.operations;
		ws_drag_status(conn, ev->u.drag.session, w->id, op, 0);
		if (ev->u.drag.phase == WS_DRAG_ENTER)
			printf("drag-answer app=%s win=%s op=0x%x\n", app->name,
			    w->name, op);
		break;
	case WS_EVENT_DRAG_END:
		session = 0;
		break;
	default:
		break;
	}
}

static void
on_signal(int sig)
{
	if (sig == SIGUSR1)
		usr1 = 1;
	else
		quit = 1;
}

int
main(void)
{
	struct pollfd pfd[NAPPS];
	int i, n;

	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGUSR1, on_signal);

	for (i = 0; i < NAPPS; i++) {
		apps[i].conn = ws_connect(apps[i].name, 0);
		if (apps[i].conn == NULL) {
			fprintf(stderr, "wsdrag: connect %s: %s\n",
			    apps[i].name, strerror(errno));
			return 1;
		}
		printf("connected app=%s client=%u\n", apps[i].name,
		    ws_client_id(apps[i].conn));
	}
	if ((ws_server_flags(apps[ALPHA].conn) & WS_SERVER_DRAG) == 0) {
		fprintf(stderr, "wsdrag: the server can't drag and drop "
		    "(flags 0x%x)\n", ws_server_flags(apps[ALPHA].conn));
		return 1;
	}
	if (make_window(A1, ALPHA, "A1", 100, 100, 300, 200,
	    WS_WINDOW_ACCEPTS_DRAGS, 0xffc03030, WS_DRAG_OP_MOVE) < 0 ||
	    make_window(A2, ALPHA, "A2", 100, 400, 300, 200,
	    WS_WINDOW_ACCEPTS_DRAGS, 0xffe08020, WS_DRAG_OP_MOVE) < 0 ||
	    make_window(B1, BETA, "B1", 600, 100, 300, 200,
	    WS_WINDOW_ACCEPTS_DRAGS, 0xff30a040, WS_DRAG_OP_COPY) < 0 ||
	    make_window(B2, BETA, "B2", 600, 400, 300, 200,
	    WS_WINDOW_ACCEPTS_DRAGS, 0xff3050c0, WS_DRAG_OP_LINK) < 0 ||
	    make_window(B3, BETA, "B3", 950, 100, 250, 200, 0, 0xff808080,
	    0) < 0 ||
	    make_window(B4, BETA, "B4", 950, 400, 250, 200,
	    WS_WINDOW_ACCEPTS_DRAGS, 0xff8030a0, WS_DRAG_OP_NONE) < 0)
		return 1;
	for (i = 0; i < NAPPS; i++)
		ws_sync(apps[i].conn);
	printf("ready\n");

	while (!quit) {
		if (usr1) {
			usr1 = 0;
			begin(BETA, wins[B1].id, 700, 200);
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
