/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The pointer cursor: the built-in arrow, per-app cursors, and the
 * choice between the display's hardware cursor plane and a software
 * cursor composited in the server's top tier.
 */

#include <stdlib.h>
#include <string.h>

#include "server.h"

/* 12x19 arrow: '#' outline, '.' fill, ' ' transparent. Hot spot 0,0. */
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

static pixman_image_t *
make_arrow(void)
{
	const int w = 12, h = (int)(sizeof(arrow) / sizeof(arrow[0]));
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
			char ch = arrow[y][x];

			px[y * stride + x] = ch == '#' ? 0xff000000 :
			    ch == '.' ? 0xffffffff : 0x00000000;
		}
	return img;
}

int
cursor_init(struct ws_server *srv)
{
	srv->cursor_default = make_arrow();
	if (srv->cursor_default == NULL)
		return -1;
	srv->cursor_default_hot_x = 0;
	srv->cursor_default_hot_y = 0;
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
	if (srv->cursor_default != NULL)
		pixman_image_unref(srv->cursor_default);
	srv->cursor_default = NULL;
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

/* Pick the cursor for the window under the pointer and show it. */
void
cursor_update(struct ws_server *srv)
{
	struct ws_window *w = srv->grab != NULL ? srv->grab : srv->hover;
	/* Tiles are the server's: they get the default arrow. */
	struct ws_client *c = w != NULL && w->tile_for == NULL ? w->owner :
	    NULL;
	pixman_image_t *img = srv->cursor_default;
	int hx = srv->cursor_default_hot_x, hy = srv->cursor_default_hot_y;
	bool visible = true, hw = false;

	if (srv->cursor_default == NULL)
		return;
	if (c != NULL && !c->dead) {
		if (c->cursor_hidden)
			visible = false;
		if (c->cursor_kind == WS_CURSOR_CUSTOM &&
		    c->cursor_buffer != NULL) {
			img = c->cursor_buffer->image;
			hx = c->cursor_hot_x;
			hy = c->cursor_hot_y;
		}
		/* TODO: built-in images for the other standard cursors. */
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
