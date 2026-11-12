/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The pointer cursor: the built-in images for the standard kinds,
 * per-connection and per-window
 * cursors (a window's wins over its connection's), and the
 * choice between the display's hardware cursor plane and a software
 * cursor composited in the server's top tier.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "server.h"

/*
 * The standard cursors, drawn for NextBSD: '#' black, '.' white, ' '
 * transparent. Solid shapes are white with a black outline; thin ones
 * (I-beam, crosshair, not-allowed) are black with a white halo, so each
 * shows on dark and light backgrounds alike.
 */

/* 12x19 arrow. Hot spot 0,0. */
static const char *const arrow[] = {
	"#           ",
	"##          ",
	"#.#         ",
	"#..#        ",
	"#...#       ",
	"#....#      ",
	"#.....#     ",
	"#......#    ",
	"#.......#   ",
	"#........#  ",
	"#.........# ",
	"#......#####",
	"#...#..#    ",
	"#..##..#    ",
	"#.#  #..#   ",
	"##   #..#   ",
	"#     #..#  ",
	"      #..#  ",
	"       ##   ",
};

/* 9x18 I-beam. Hot spot 4,8, mid-stem. */
static const char *const ibeam[] = {
	" ....... ",
	".#######.",
	" ...#... ",
	"   .#.   ",
	"   .#.   ",
	"   .#.   ",
	"   .#.   ",
	"   .#.   ",
	"   .#.   ",
	"   .#.   ",
	"   .#.   ",
	"   .#.   ",
	"   .#.   ",
	"   .#.   ",
	"   .#.   ",
	" ...#... ",
	".#######.",
	" ....... ",
};

/* 18x20 pointing hand. Hot spot 6,0, the fingertip. */
static const char *const pointing_hand[] = {
	"      ##          ",
	"     #..#         ",
	"     #..#         ",
	"     #..#         ",
	"     #..#         ",
	"     #..###       ",
	"     #..#..###    ",
	"     #..#..#..#   ",
	"     #..#..#..### ",
	"  ## #..#..#..#..#",
	" #..##..#..#..#..#",
	" #...#........#..#",
	" #...............#",
	"  #..............#",
	"  #..............#",
	"   #.............#",
	"   #............# ",
	"    #...........# ",
	"    #...........# ",
	"    ############# ",
};

/* 17x13 closed hand (grabbing). Hot spot 9,5, mid-palm. */
static const char *const closed_hand[] = {
	"    ## ## ##     ",
	"   #..#..#..###  ",
	"   #..#..#..#..# ",
	"  ##..#..#..#..# ",
	" #..#...........#",
	" #..............#",
	"  #.............#",
	"   #............#",
	"   #............#",
	"    #..........# ",
	"     #.........# ",
	"     #.........# ",
	"     ########### ",
};

/* 17x17 open hand. Hot spot 9,9, mid-palm. */
static const char *const open_hand[] = {
	"        ##       ",
	"     ###..###    ",
	"    #..#..#..#   ",
	"    #..#..#..### ",
	"    #..#..#..#..#",
	"    #..#..#..#..#",
	" ## #..#..#..#..#",
	"#..##..#..#..#..#",
	"#...#..#..#..#..#",
	" #..............#",
	"  #.............#",
	"   #............#",
	"    #...........#",
	"    #...........#",
	"     #.........# ",
	"     #.........# ",
	"     ########### ",
};

/* 19x19 crosshair. Hot spot 9,9, the centre dot. */
static const char *const crosshair[] = {
	"        .#.        ",
	"        .#.        ",
	"        .#.        ",
	"        .#.        ",
	"        .#.        ",
	"        .#.        ",
	"        .#.        ",
	"        ...        ",
	"...................",
	"#######..#..#######",
	"...................",
	"        ...        ",
	"        .#.        ",
	"        .#.        ",
	"        .#.        ",
	"        .#.        ",
	"        .#.        ",
	"        .#.        ",
	"        .#.        ",
};

/* 25x11 left-right resize. Hot spot 12,5, the centre. */
static const char *const resize_lr[] = {
	"     #             #     ",
	"    ##             ##    ",
	"   #.#             #.#   ",
	"  #..###############..#  ",
	" #.....................# ",
	"#.......................#",
	" #.....................# ",
	"  #..###############..#  ",
	"   #.#             #.#   ",
	"    ##             ##    ",
	"     #             #     ",
};

/* 11x25 up-down resize. Hot spot 5,12, the centre. */
static const char *const resize_ud[] = {
	"     #     ",
	"    #.#    ",
	"   #...#   ",
	"  #.....#  ",
	" #.......# ",
	"####...####",
	"   #...#   ",
	"   #...#   ",
	"   #...#   ",
	"   #...#   ",
	"   #...#   ",
	"   #...#   ",
	"   #...#   ",
	"   #...#   ",
	"   #...#   ",
	"   #...#   ",
	"   #...#   ",
	"   #...#   ",
	"   #...#   ",
	"####...####",
	" #.......# ",
	"  #.....#  ",
	"   #...#   ",
	"    #.#    ",
	"     #     ",
};

/* 20x20 operation not allowed. Hot spot 10,10, the centre. */
static const char *const not_allowed[] = {
	"        ....        ",
	"     ....##....     ",
	"    ..########..    ",
	"   ..##########..   ",
	"  ..###......###..  ",
	" ..####..   ...##.. ",
	" .######..    .###. ",
	" .##..###..   ..##. ",
	"..##...###..   .##..",
	".###. ..###..  .###.",
	".###.  ..###.. .###.",
	"..##.   ..###...##..",
	" .##..   ..###..##. ",
	" .###.    ..######. ",
	" ..##...   ..####.. ",
	"  ..###......###..  ",
	"   ..##########..   ",
	"    ..########..    ",
	"     ....##....     ",
	"        ....        ",
};

#define STD(rows, hx, hy) \
	{ rows, (int)(sizeof(rows) / sizeof(rows[0])), hx, hy }

static const struct std_cursor {
	const char *const *rows;
	int	h;
	int	hot_x;
	int	hot_y;
} std_cursors[WS_NSTD_CURSORS] = {
	[WS_CURSOR_ARROW] =		STD(arrow, 0, 0),
	[WS_CURSOR_IBEAM] =		STD(ibeam, 4, 8),
	[WS_CURSOR_POINTING_HAND] =	STD(pointing_hand, 6, 0),
	[WS_CURSOR_CLOSED_HAND] =	STD(closed_hand, 9, 5),
	[WS_CURSOR_OPEN_HAND] =		STD(open_hand, 9, 9),
	[WS_CURSOR_CROSSHAIR] =		STD(crosshair, 9, 9),
	[WS_CURSOR_RESIZE_LR] =		STD(resize_lr, 12, 5),
	[WS_CURSOR_RESIZE_UD] =		STD(resize_ud, 5, 12),
	[WS_CURSOR_NOT_ALLOWED] =	STD(not_allowed, 10, 10),
};

static pixman_image_t *
make_image(const char *const *rows, int h)
{
	const int w = (int)strlen(rows[0]);
	pixman_image_t *img;
	uint32_t *px;
	int x, y, stride;

	img = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, NULL, 0);
	if (img == NULL)
		return NULL;
	px = pixman_image_get_data(img);
	stride = pixman_image_get_stride(img) / 4;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			char ch = rows[y][x];

			px[y * stride + x] = ch == '#' ? 0xff000000 :
			    ch == '.' ? 0xffffffff : 0x00000000;
		}
	return img;
}

int
cursor_init(struct ws_server *srv)
{
	int i;

	for (i = 0; i < WS_NSTD_CURSORS; i++) {
		srv->cursor_std[i] = make_image(std_cursors[i].rows,
		    std_cursors[i].h);
		if (srv->cursor_std[i] == NULL)
			return -1;
	}
	srv->cursor_visible = true;
	srv->cursor_hw = false;
	memset(&srv->cursor_rect, 0, sizeof(srv->cursor_rect));
	cursor_update(srv);
	return 0;
}

void
cursor_fini(struct ws_server *srv)
{
	if (srv->output != NULL && srv->output->cursor_hide != NULL)
		srv->output->cursor_hide(srv);
	int i;

	for (i = 0; i < WS_NSTD_CURSORS; i++) {
		if (srv->cursor_std[i] != NULL)
			pixman_image_unref(srv->cursor_std[i]);
		srv->cursor_std[i] = NULL;
	}
	srv->cursor_image = NULL;
}

static void
sw_cursor_damage(struct ws_server *srv)
{
	damage_rect(srv, &srv->cursor_rect);
	memset(&srv->cursor_rect, 0, sizeof(srv->cursor_rect));
	if (!srv->cursor_hw && srv->cursor_visible && srv->cursor_image != NULL) {
		srv->cursor_rect.x = (int32_t)srv->ptr_x - srv->cursor_hot_x;
		srv->cursor_rect.y = (int32_t)srv->ptr_y - srv->cursor_hot_y;
		srv->cursor_rect.width =
		    pixman_image_get_width(srv->cursor_image);
		srv->cursor_rect.height =
		    pixman_image_get_height(srv->cursor_image);
		damage_rect(srv, &srv->cursor_rect);
	}
}

/*
 * Pick the cursor for the window under the pointer and show it. While a
 * connection holds a pointer grab, it's that connection's: its window's
 * over its own windows (hover is only ever one of those then), its own
 * elsewhere.
 */
void
cursor_update(struct ws_server *srv)
{
	struct ws_window *w = srv->pgrab != NULL || srv->grab == NULL ?
	    srv->hover : srv->grab;
	/* Tiles are the server's: they get the default arrow. */
	struct ws_client *c = w != NULL && w->tile_for == NULL ? w->owner :
	    srv->pgrab;
	uint32_t kind = WS_CURSOR_ARROW;
	struct ws_buffer *buf = NULL;
	pixman_image_t *img;
	int hx = 0, hy = 0;
	bool visible = true, hw = false;

	if (srv->cursor_std[WS_CURSOR_ARROW] == NULL)
		return;
	if (c != NULL && !c->dead) {
		if (c->cursor_hidden)
			visible = false;
		if (w != NULL && w->owner == c && w->cursor_set) {
			kind = w->cursor_kind;
			buf = w->cursor_buffer;
			hx = w->cursor_hot_x;
			hy = w->cursor_hot_y;
		} else {
			kind = c->cursor_kind;
			buf = c->cursor_buffer;
			hx = c->cursor_hot_x;
			hy = c->cursor_hot_y;
		}
	}
	if (kind == WS_CURSOR_CUSTOM && buf != NULL) {
		img = buf->image;
	} else {
		/* A kind this server doesn't know gets the arrow. */
		if (kind >= WS_NSTD_CURSORS)
			kind = WS_CURSOR_ARROW;
		img = srv->cursor_std[kind];
		hx = std_cursors[kind].hot_x;
		hy = std_cursors[kind].hot_y;
	}
	if (img == srv->cursor_image && hx == srv->cursor_hot_x &&
	    hy == srv->cursor_hot_y && visible == srv->cursor_visible)
		return;
	srv->cursor_image = img;
	srv->cursor_hot_x = hx;
	srv->cursor_hot_y = hy;
	srv->cursor_visible = visible;

	if (srv->display_active && srv->cfg.hardware_cursor &&
	    srv->output != NULL && srv->output->cursor_set != NULL) {
		if (!visible) {
			srv->output->cursor_hide(srv);
			hw = true;
		} else if (srv->output->cursor_set(srv, img, hx, hy)) {
			srv->output->cursor_move(srv, (int)srv->ptr_x,
			    (int)srv->ptr_y);
			hw = true;
		}
	}
	if (!hw && srv->cursor_hw && srv->output->cursor_hide != NULL)
		srv->output->cursor_hide(srv);
	srv->cursor_hw = hw;
	sw_cursor_damage(srv);
}

void
cursor_moved(struct ws_server *srv)
{
	if (srv->cursor_hw) {
		if (srv->display_active && srv->cursor_visible)
			srv->output->cursor_move(srv, (int)srv->ptr_x,
			    (int)srv->ptr_y);
		return;
	}
	sw_cursor_damage(srv);
}

void
cursor_buffer_gone(struct ws_server *srv, struct ws_buffer *b)
{
	if (srv->cursor_image == b->image) {
		srv->cursor_image = NULL;	/* force a reload */
		srv->cursor_hot_x = -1;
	}
}

/*
 * WS_MSG_WINDOW_SET_CURSOR: w's own cursor, a standard kind or a custom
 * image (b, which the window keeps a reference to), or WS_CURSOR_INHERIT
 * for its connection's again.
 */
int
cursor_window_set(struct ws_server *srv, struct ws_window *w, uint32_t kind,
    struct ws_buffer *b, int32_t hot_x, int32_t hot_y)
{
	struct ws_buffer *old = w->cursor_buffer;

	if (kind == WS_CURSOR_CUSTOM && b == NULL)
		return EINVAL;
	if (kind != WS_CURSOR_CUSTOM)
		b = NULL;
	if (b != NULL)
		b->refs++;
	w->cursor_set = kind != WS_CURSOR_INHERIT;
	w->cursor_kind = kind;
	w->cursor_buffer = b;
	w->cursor_hot_x = hot_x;
	w->cursor_hot_y = hot_y;
	if (old != NULL) {
		cursor_buffer_gone(srv, old);
		buffer_unref(srv, old);
	}
	cursor_update(srv);
	return 0;
}
