/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * wsmenubar: a WindowServer test client for the menu bar
 * (apps-windowserver#97, "The menu bar" in WSProtocol.h). One process
 * is one app:
 *
 *   wsmenubar bar NAME BODY TITLE [DELAY_MS]
 *	An app with a Mac menu bar: a window along the top in BODY
 *	(0xRRGGBB) with its menu titles, a block TITLE wide 120 points at
 *	its left end, and a document window in TITLE at 300,300. It
 *	activates, and names the bar (WS_MSG_SET_MENU_BAR with the window)
 *	DELAY_MS later (a slow launch). Like GNUstep, it orders its bar
 *	out when it is deactivated and in when it is activated.
 *   wsmenubar oldbar NAME BODY TITLE
 *	The same app as a client from before WS_SERVER_STATUS_ITEMS: it
 *	reports the strip (WS_MSG_SET_MENU_BAR without a window) and
 *	keeps its bar on WS_LEVEL_MAIN_MENU, ordering it in and out
 *	itself, for comparing app switches.
 *   wsmenubar plain NAME COLOR
 *	An app with no menu bar (an X11 app): a document window only.
 *   wsmenubar item NAME WIDTH PRIORITY COLOR
 *	A status item: a window in COLOR, WIDTH points wide, at PRIORITY.
 *	SIGUSR1 doubles its width; SIGUSR2 orders it out.
 *
 * It prints the events the test reads, one per line, and "ready" once
 * set up; tests/menubar-test.sh drives it with wsctl.
 */

#include <errno.h>
#include <stdbool.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <WindowServer/WindowServer.h>

#define BAR_HEIGHT	24

struct win {
	uint32_t	id;
	ws_buffer_t	*buf;
	uint32_t	body;
	uint32_t	title;		/* the bar's left block, 0 = none */
	ws_rect_t	frame;
};

static ws_connection_t *conn;
static struct win bar, doc, item;
static volatile sig_atomic_t quit, widen, hide;
static bool old_style;
static uint32_t item_width;
static int32_t item_priority;

static void
draw(struct win *w, const ws_rect_t *f)
{
	ws_buffer_t *b;
	uint32_t *px, stride, x, y, bw, bh;

	if (f->width <= 0 || f->height <= 0)
		return;
	b = ws_buffer_create(conn, (uint32_t)f->width, (uint32_t)f->height, 1);
	if (b == NULL) {
		fprintf(stderr, "wsmenubar: buffer: %s\n", strerror(errno));
		return;
	}
	px = ws_buffer_data(b);
	bw = ws_buffer_width(b);
	bh = ws_buffer_height(b);
	stride = ws_buffer_stride(b) / 4;
	for (y = 0; y < bh; y++)
		for (x = 0; x < bw; x++)
			px[y * stride + x] = 0xff000000 |
			    (w->title != 0 && x < 120 ? w->title : w->body);
	ws_window_attach(conn, w->id, b);
	ws_window_commit_frame(conn, w->id, 0, f);
	if (w->buf != NULL)
		ws_buffer_destroy(conn, w->buf);
	w->buf = b;
	w->frame = *f;
}

static uint32_t
make(struct win *w, int x, int y, int width, int height, int32_t level,
    uint32_t flags, const char *title)
{
	ws_rect_t f = { x, y, width, height };

	w->id = ws_window_create(conn, &f, level, flags, title);
	if (w->id == 0) {
		fprintf(stderr, "wsmenubar: window: %s\n", strerror(errno));
		exit(1);
	}
	draw(w, &f);
	return w->id;
}

static struct win *
win_by_id(uint32_t id)
{
	if (id == 0)
		return NULL;
	if (id == bar.id)
		return &bar;
	if (id == doc.id)
		return &doc;
	if (id == item.id)
		return &item;
	return NULL;
}

static const char *
win_name(struct win *w)
{
	return w == &bar ? "bar" : w == &doc ? "doc" : w == &item ? "item" :
	    "-";
}

static void
on_event(ws_connection_t *c, const ws_event_t *ev, void *ctx)
{
	struct win *w = win_by_id(ev->hdr.window);

	(void)c;
	(void)ctx;
	switch (ev->hdr.type) {
	case WS_EVENT_WINDOW_FRAME:
		printf("frame %s %d,%d,%dx%d\n", win_name(w),
		    ev->u.frame.frame.x, ev->u.frame.frame.y,
		    ev->u.frame.frame.width, ev->u.frame.frame.height);
		if (w != NULL)
			draw(w, &ev->u.frame.frame);
		break;
	case WS_EVENT_APP_ACTIVATED:
		printf("activated\n");
		if (bar.id != 0)	/* GNUstep orders its menu in */
			ws_window_order(conn, bar.id, WS_ORDER_ABOVE, 0);
		break;
	case WS_EVENT_APP_DEACTIVATED:
		printf("deactivated\n");
		if (bar.id != 0)	/* and out (hidesOnDeactivate) */
			ws_window_order(conn, bar.id, WS_ORDER_OUT, 0);
		break;
	case WS_EVENT_USABLE_AREA:
		printf("usable menubar=%u statusitems=%u\n",
		    ev->u.usable_area.menu_bar, ev->u.usable_area.status_items);
		break;
	case WS_EVENT_MOUSE_DOWN:
		printf("mouse-down %s\n", win_name(w));
		/* a status item pops up its menu: it may grab the pointer */
		if (w == &item)
			printf("grab %s\n", ws_pointer_grab(conn,
			    WS_GRAB_OUTSIDE) == 0 ? "ok" : strerror(errno));
		break;
	case WS_EVENT_ERROR:
		printf("error request=0x%x code=%d %s\n", ev->u.error.request,
		    ev->u.error.code, ev->u.error.message);
		break;
	default:
		break;
	}
}

static void
on_signal(int sig)
{
	if (sig == SIGUSR1)
		widen = 1;
	else if (sig == SIGUSR2)
		hide = 1;
	else
		quit = 1;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: wsmenubar bar NAME BODY TITLE [DELAY_MS]\n"
	    "       wsmenubar oldbar NAME BODY TITLE\n"
	    "       wsmenubar plain NAME COLOR\n"
	    "       wsmenubar item NAME WIDTH PRIORITY COLOR\n");
	exit(2);
}

int
main(int argc, char **argv)
{
	struct pollfd pfd;
	int delay = 0, n;
	bool named = false;
	uint64_t start;
	struct timespec ts;
	char bar_title[80], doc_title[80];

	if (argc < 4)
		usage();
	setvbuf(stdout, NULL, _IOLBF, 0);
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGUSR1, on_signal);
	signal(SIGUSR2, on_signal);

	conn = ws_connect(argv[2], 0);
	if (conn == NULL) {
		fprintf(stderr, "wsmenubar: connect: %s\n", strerror(errno));
		return 1;
	}
	if ((ws_server_flags(conn) & WS_SERVER_STATUS_ITEMS) == 0) {
		fprintf(stderr, "wsmenubar: no WS_SERVER_STATUS_ITEMS\n");
		return 1;
	}
	printf("client %u\n", ws_client_id(conn));
	snprintf(bar_title, sizeof(bar_title), "%s-bar", argv[2]);
	snprintf(doc_title, sizeof(doc_title), "%s-doc", argv[2]);
	old_style = strcmp(argv[1], "oldbar") == 0 && argc == 5;
	if ((strcmp(argv[1], "bar") == 0 && (argc == 5 || argc == 6)) ||
	    old_style) {
		bar.body = (uint32_t)strtoul(argv[3], NULL, 16);
		bar.title = (uint32_t)strtoul(argv[4], NULL, 16);
		doc.body = bar.title;
		delay = argc == 6 ? atoi(argv[5]) : 0;
		make(&doc, 300, 300, 200, 150, WS_LEVEL_NORMAL, 0, doc_title);
		ws_window_order(conn, doc.id, WS_ORDER_ABOVE, 0);
		/* (reported first, or keeping clear of the strip would push
		   the bar below it) */
		if (old_style)
			ws_set_menu_bar(conn, BAR_HEIGHT);
		make(&bar, 0, 0, 1280, BAR_HEIGHT, WS_LEVEL_MAIN_MENU,
		    WS_WINDOW_NO_KEY, bar_title);
		ws_window_order(conn, bar.id, WS_ORDER_ABOVE, 0);
		ws_activate(conn);
		ws_window_make_key(conn, doc.id);
		printf("bar window %u\n", bar.id);
	} else if (strcmp(argv[1], "plain") == 0 && argc == 4) {
		doc.body = (uint32_t)strtoul(argv[3], NULL, 16);
		make(&doc, 300, 300, 200, 150, WS_LEVEL_NORMAL, 0, doc_title);
		ws_window_order(conn, doc.id, WS_ORDER_ABOVE, 0);
		ws_activate(conn);
		ws_window_make_key(conn, doc.id);
		named = true;
	} else if (strcmp(argv[1], "item") == 0 && argc == 6) {
		item_width = (uint32_t)strtoul(argv[3], NULL, 0);
		item_priority = (int32_t)strtol(argv[4], NULL, 0);
		item.body = (uint32_t)strtoul(argv[5], NULL, 16);
		make(&item, 0, 0, (int)item_width, BAR_HEIGHT, WS_LEVEL_NORMAL,
		    WS_WINDOW_NO_KEY, argv[2]);
		if (ws_status_item(conn, item.id, item_width,
		    item_priority) < 0) {
			fprintf(stderr, "wsmenubar: status item: %s\n",
			    strerror(errno));
			return 1;
		}
		ws_window_order(conn, item.id, WS_ORDER_ABOVE, 0);
		printf("item window %u\n", item.id);
		named = true;
	} else
		usage();
	clock_gettime(CLOCK_MONOTONIC, &ts);
	start = (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
	if (named) {
		ws_sync(conn);
		printf("ready\n");
	}

	while (!quit) {
		if (!named) {
			clock_gettime(CLOCK_MONOTONIC, &ts);
			if ((uint64_t)ts.tv_sec * 1000 +
			    (uint64_t)ts.tv_nsec / 1000000 >=
			    start + (uint64_t)delay) {
				if (old_style)
					ws_set_menu_bar(conn, BAR_HEIGHT);
				else
					ws_set_menu_bar_window(conn, bar.id,
					    BAR_HEIGHT);
				ws_sync(conn);
				named = true;
				printf("ready\n");
			}
		}
		if (widen) {
			widen = 0;
			item_width *= 2;
			ws_status_item(conn, item.id, item_width,
			    item_priority);
			ws_sync(conn);
			printf("widened %u\n", item_width);
		}
		if (hide) {
			hide = 0;
			ws_window_order(conn, item.id, WS_ORDER_OUT, 0);
			ws_sync(conn);
			printf("hidden\n");
		}
		pfd.fd = ws_connection_fd(conn);
		pfd.events = POLLIN;
		pfd.revents = 0;
		n = poll(&pfd, 1, named ? 200 : 10);
		if (n < 0 && errno != EINTR)
			break;
		if (ws_pending(conn) || (pfd.revents & (POLLIN | POLLHUP))) {
			if (ws_dispatch(conn, on_event, NULL) < 0) {
				printf("server closed the connection\n");
				break;
			}
		}
	}
	ws_disconnect(conn);
	return 0;
}
