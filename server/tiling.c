/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Window tiling ("Window tiling" in WSProtocol.h, apps-windowserver#20).
 *
 * While WindowMode is "tiling", the ordinary windows of every app are
 * laid out side by side in the usable area (screen.c), which leaves the
 * Dock's strip and the menu bar strip free. Their owners hear the new
 * frames as WS_EVENT_WINDOW_FRAME, as for any frame the server sets.
 * Everything else (menus, panels, alerts, desktop windows, app icons,
 * the Dock, windows that can't be resized and windows the user floated)
 * floats above the tiled windows: window.c sorts tiled windows below the
 * floating ones on their level (w->tiled). As on Hyprland, a window's
 * minimum size doesn't keep it from tiling: it gets its tile all the
 * same, and one whose owner keeps answering with a larger frame is cut
 * to its tile.
 *
 * The windows tiled are kept in a tiling space: an ordered list of
 * slots (master: each with its weight in the stack) and, for dwindle, a
 * binary tree whose leaves are the slots' windows and whose other nodes
 * each split their area in two, along its longer side, so that swapping
 * two windows swaps only the windows. A new window splits one tile
 * (TilingNewWindowSplits: the largest, the one under the pointer or the
 * focused one); a window that goes gives its tile back to its sibling,
 * as on Hyprland. With TilingFocusFollowsMouse the tile under the
 * pointer has the focus. There is one space, for the screen. Workspaces (apps-windowserver#13) will give each one its
 * own: everything here takes the space from tiling_space(), the one
 * place that would pick the workspace's.
 *
 * Changes only mark the layout dirty; tiling_flush() at the end of each
 * pass of the event loop works out which windows tile and lays them out
 * once, before the next frame is composited.
 */

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "server.h"

#define MIN_HANDLE	8	/* a split takes presses this wide at least */
#define RATIO_MIN	0.1	/* no side of a split smaller than this */
#define RATIO_MAX	0.9
#define WEIGHT_MIN	0.1	/* master: stack weights */
#define WEIGHT_MAX	10.0
#define RESIZE_STEP	0.05	/* a resize key: 5% of the area */
#define INSIST_LIMIT	3	/* larger frames in a row: it floats */
#define DRAWN_WAIT_USEC	1000000	/* how long an owner has to draw at the
				   size of its new tile */
#define OPENED_USEC	300000	/* a window that just joined keeps the
				   focus this long, whatever the pointer
				   does */

/* Window flags that make a window float in tiling mode. */
#define FLOATING_FLAGS	(WS_WINDOW_NO_TILE | WS_WINDOW_FIXED_SIZE | \
			 WS_WINDOW_PANEL | WS_WINDOW_ICON | WS_WINDOW_NO_KEY | \
			 WS_WINDOW_IGNORES_MOUSE)

static bool
rect_eq(const ws_rect_t *a, const ws_rect_t *b)
{
	return a->x == b->x && a->y == b->y && a->width == b->width &&
	    a->height == b->height;
}

static bool
in_rect(const ws_rect_t *r, double x, double y)
{
	return x >= r->x && y >= r->y && x < (double)r->x + r->width &&
	    y < (double)r->y + r->height;
}

static double
clampd(double v, double lo, double hi)
{
	if (!(v >= lo))		/* NAN too */
		return lo;
	return v > hi ? hi : v;
}

/* The space the screen's windows are tiled in (one per workspace, later). */
static struct ws_tiling_space *
tiling_space(struct ws_server *srv)
{
	return &srv->tiling.space;
}

static int
gap_inner(struct ws_server *srv)
{
	return srv->cfg.tiling_gaps_inner < 0 ? 0 :
	    srv->cfg.tiling_gaps_inner > 64 ? 64 : srv->cfg.tiling_gaps_inner;
}

static int
gap_outer(struct ws_server *srv)
{
	return srv->cfg.tiling_gaps_outer < 0 ? 0 :
	    srv->cfg.tiling_gaps_outer > 128 ? 128 : srv->cfg.tiling_gaps_outer;
}

/* TilingModifier: "Command+Option", "Control,Option"... never Shift,
   which the swap keys add. mods may be NULL (just checking). */
bool
tiling_modifier_parse(const char *s, uint32_t *mods)
{
	char buf[48], *p, *tok;
	uint32_t m = 0;

	if (strlcpy(buf, s, sizeof(buf)) >= sizeof(buf))
		return false;
	p = buf;
	while ((tok = strsep(&p, "+, ")) != NULL) {
		if (*tok == '\0')
			continue;
		if (strcasecmp(tok, "Command") == 0 ||
		    strcasecmp(tok, "Cmd") == 0 || strcasecmp(tok, "Super") == 0)
			m |= WS_MOD_COMMAND;
		else if (strcasecmp(tok, "Option") == 0 ||
		    strcasecmp(tok, "Alt") == 0)
			m |= WS_MOD_ALT;
		else if (strcasecmp(tok, "Control") == 0 ||
		    strcasecmp(tok, "Ctrl") == 0)
			m |= WS_MOD_CONTROL;
		else
			return false;
	}
	if (m == 0)
		return false;
	if (mods != NULL)
		*mods = m;
	return true;
}

/* ---- which windows tile ---- */

static bool
tileable(struct ws_server *srv, const struct ws_window *w)
{
	const struct ws_client *c = w->owner;

	return w->visible && !w->minimized && w->tile_for == NULL &&
	    w->level == WS_LEVEL_NORMAL && (w->flags & FLOATING_FLAGS) == 0 &&
	    !w->tiling_float &&
	    w->frame.width > 0 && w->frame.height > 0 &&
	    !c->dead && !c->doomed && (c->flags & WS_CONNECT_TOOL) == 0 &&
	    !dock_owns_app(srv, c->app);
}

/*
 * w's frame cut to its tile: the frame its owner keeps asking for
 * (w->tiling_asked) at the tile's corner, no larger than the tile.
 */
static ws_rect_t
clipped(const struct ws_window *w)
{
	const ws_rect_t *g = &w->tiling_given;
	ws_rect_t r = { g->x, g->y, w->tiling_asked.width,
	    w->tiling_asked.height };

	if (r.width > g->width)
		r.width = g->width;
	if (r.height > g->height)
		r.height = g->height;
	return r;
}

/* ---- dwindle's tree ---- */

/* The leaf of w under n, or NULL. */
static struct ws_tiling_node *
node_leaf(struct ws_tiling_node *n, const struct ws_window *w)
{
	struct ws_tiling_node *l;

	if (n == NULL || w == NULL)
		return NULL;
	if (n->child[0] == NULL)
		return n->w == w ? n : NULL;
	if ((l = node_leaf(n->child[0], w)) != NULL)
		return l;
	return node_leaf(n->child[1], w);
}

static void
node_free(struct ws_tiling_node *n)
{
	if (n == NULL)
		return;
	node_free(n->child[0]);
	node_free(n->child[1]);
	free(n);
}

/*
 * w joins the tree: it splits leaf `at` (its tile), taking the second
 * side, right or below. With no `at`, it splits the last leaf, or takes
 * the whole area if it is the first.
 */
static int
tree_insert(struct ws_tiling_space *sp, struct ws_tiling_node *at,
    struct ws_window *w)
{
	struct ws_tiling_node *leaf, *n;

	if ((leaf = calloc(1, sizeof(*leaf))) == NULL)
		return -1;
	leaf->w = w;
	if (sp->root == NULL) {
		sp->root = leaf;
		return 0;
	}
	if (at == NULL)
		for (at = sp->root; at->child[1] != NULL; at = at->child[1])
			;
	if ((n = calloc(1, sizeof(*n))) == NULL) {
		free(leaf);
		return -1;
	}
	n->parent = at->parent;
	if (at->parent == NULL)
		sp->root = n;
	else
		at->parent->child[at->parent->child[1] == at] = n;
	n->child[0] = at;
	n->child[1] = leaf;
	n->ratio = 0.5;
	at->parent = leaf->parent = n;
	return 0;
}

/* w leaves the tree: its sibling takes the area they shared. */
static void
tree_remove(struct ws_tiling_space *sp, const struct ws_window *w)
{
	struct ws_tiling_node *leaf = node_leaf(sp->root, w), *p, *sib;

	if (leaf == NULL)
		return;
	p = leaf->parent;
	if (p == NULL) {
		sp->root = NULL;
		free(leaf);
		return;
	}
	sib = p->child[p->child[0] == leaf];
	sib->parent = p->parent;
	if (p->parent == NULL)
		sp->root = sib;
	else
		p->parent->child[p->parent->child[1] == p] = sib;
	free(leaf);
	free(p);
}

/* ---- slots ---- */

static int
slot_of(const struct ws_tiling_space *sp, const struct ws_window *w)
{
	unsigned i;

	if (w == NULL)
		return -1;
	for (i = 0; i < sp->nslots; i++)
		if (sp->slots[i].w == w)
			return (int)i;
	return -1;
}

static int
slot_insert(struct ws_tiling_space *sp, unsigned at, struct ws_window *w)
{
	struct ws_tiling_slot *slots;
	struct ws_tiling_split *splits;
	unsigned cap;

	if (sp->nslots == sp->cap) {
		cap = sp->cap != 0 ? sp->cap * 2 : 8;
		slots = reallocarray(sp->slots, cap, sizeof(*slots));
		if (slots == NULL)
			return -1;
		sp->slots = slots;
		splits = reallocarray(sp->splits, cap, sizeof(*splits));
		if (splits == NULL)
			return -1;
		sp->splits = splits;
		sp->cap = cap;
	}
	if (at > sp->nslots)
		at = sp->nslots;
	memmove(&sp->slots[at + 1], &sp->slots[at],
	    (sp->nslots - at) * sizeof(sp->slots[0]));
	memset(&sp->slots[at], 0, sizeof(sp->slots[at]));
	sp->slots[at].w = w;
	sp->slots[at].weight = 1.0;
	sp->nslots++;
	return 0;
}

static void
slot_remove(struct ws_tiling_space *sp, unsigned i)
{
	tree_remove(sp, sp->slots[i].w);
	memmove(&sp->slots[i], &sp->slots[i + 1],
	    (sp->nslots - i - 1) * sizeof(sp->slots[0]));
	sp->nslots--;
	sp->nsplits = 0;	/* stale until the next layout */
}

/*
 * w joins the layout at the end of the slots, splitting leaf `split` in
 * dwindle's tree: it remembers the frame it had.
 */
static void
enter(struct ws_server *srv, struct ws_window *w, struct ws_tiling_node *split)
{
	struct ws_tiling_space *sp = tiling_space(srv);

	if (slot_insert(sp, sp->nslots, w) < 0 ||
	    tree_insert(sp, split, w) < 0) {
		if (slot_of(sp, w) >= 0)
			slot_remove(sp, (unsigned)slot_of(sp, w));
		ws_warn("tiling: no memory for window %u", w->id);
		return;
	}
	if (!w->has_saved_frame) {
		w->saved_frame = w->frame;
		w->has_saved_frame = true;
	}
	w->tiled = true;
	/* A window shown just now may still settle its frame; one that was
	   on screen when tiling started has its frame already, the one it
	   gets back, whatever its owner does on hearing of the tile (a
	   GNUstep app also changing its menu style for tiling). */
	w->tiling_fresh = !srv->tiling.starting;
	if (w->tiling_fresh)
		srv->tiling.opened_usec = ws_now_usec();
	w->tiling_insist = 0;
	w->tiling_drawn = 0;
	w->tiling_own_size = false;
	w->tiling_short = false;
	w->tiling_clipped = false;
	memset(&w->tiling_given, 0, sizeof(w->tiling_given));
	window_restack(srv, w);
	ws_debug("tiling: window %u (%s) tiled", w->id, w->owner->app_name);
}

/*
 * Slot i's window leaves the layout. restore: it floats now and gets
 * back its frame from before tiling, if it still has it.
 */
static void
leave(struct ws_server *srv, unsigned i, bool restore)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_window *w = sp->slots[i].w;

	slot_remove(sp, i);
	w->tiled = false;
	w->tiling_clipped = false;
	window_restack(srv, w);
	if (srv->tiling.moving == w)
		srv->tiling.moving = NULL;
	if (restore && w->has_saved_frame) {
		w->has_saved_frame = false;
		if (!rect_eq(&w->frame, &w->saved_frame))
			window_server_frame(srv, w, &w->saved_frame);
	}
	ws_debug("tiling: window %u (%s) floats", w->id, w->owner->app_name);
}

/* The window the keys act on: the active app's key window. */
static struct ws_window *
key_window(struct ws_server *srv)
{
	return srv->active != NULL ? srv->active->key_window : NULL;
}

/* The slot with the focus: the key window's, else the last one key. */
static int
focused_slot(struct ws_server *srv)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	int i = slot_of(sp, key_window(srv));

	return i >= 0 ? i : slot_of(sp, srv->tiling.focus);
}

static void split_two(const ws_rect_t *r, bool vertical, int gap,
    double ratio, ws_rect_t *first, ws_rect_t *second, int32_t *pos);

/* Leaf l's tile, if it is under n with area r, as compute() lays it out. */
static bool
leaf_rect(struct ws_server *srv, const struct ws_tiling_node *n,
    const ws_rect_t *r, const struct ws_tiling_node *l, ws_rect_t *out)
{
	ws_rect_t a, b;
	int32_t pos;

	if (n->child[0] == NULL) {
		if (n != l)
			return false;
		*out = *r;
		return true;
	}
	split_two(r, r->width >= r->height, gap_inner(srv),
	    clampd(n->ratio, RATIO_MIN, RATIO_MAX), &a, &b, &pos);
	return leaf_rect(srv, n->child[0], &a, l, out) ||
	    leaf_rect(srv, n->child[1], &b, l, out);
}

/*
 * The leaf a new window splits, for TilingNewWindowSplits "largest":
 * the largest tile (within 2%), the leftmost, then topmost, of equal
 * ones. Two windows sit side by side, the third splits the left one,
 * the fourth the right one, and so on: the screen fills evenly.
 */
static struct ws_tiling_node *
largest_leaf(struct ws_server *srv)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_tiling_node *l, *best = NULL;
	ws_rect_t r, br = { 0, 0, 0, 0 };
	int64_t a, ba = 0, big;
	unsigned i;

	for (i = 0; i < sp->nslots; i++) {
		if ((l = node_leaf(sp->root, sp->slots[i].w)) == NULL)
			continue;
		if (!leaf_rect(srv, sp->root, &sp->area, l, &r))
			continue;
		a = (int64_t)r.width * r.height;
		big = a > ba ? a : ba;
		if (best == NULL || (a - ba) * 50 > big ||
		    ((ba - a) * 50 <= big && (r.x < br.x ||
		    (r.x == br.x && r.y < br.y)))) {
			best = l;
			br = r;
			ba = a;
		}
	}
	return best;
}

/* The tiled window under the pointer, or NULL (a gap, the Dock, a
   window that floats). */
static struct ws_window *
tiled_at_pointer(struct ws_server *srv)
{
	struct ws_window *w = window_at(srv, srv->ptr_x, srv->ptr_y);

	return w != NULL && w->tiled && w->tile_for == NULL ? w : NULL;
}

/*
 * The leaf the next new window splits (dwindle's tree; master keeps it
 * too, for when the layout changes). `prev` is the window that joined
 * just before it in this pass, if any.
 */
static struct ws_tiling_node *
split_target(struct ws_server *srv, struct ws_window *prev)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_tiling *t = &srv->tiling;
	struct ws_window *w, *k = key_window(srv);
	struct ws_tiling_node *l;
	unsigned i;

	switch (t->splits) {
	case SPLITS_POINTER:
		/* the tile under the pointer, if it is over one (not over
		   the Dock, a panel, a menu) */
		w = window_at(srv, srv->ptr_x, srv->ptr_y);
		if (w != NULL && w->tiled &&
		    (l = node_leaf(sp->root, w)) != NULL)
			return l;
		if (w == NULL || (!w->tiled && tileable(srv, w)))
			for (i = 0; i < sp->nslots; i++)
				if (in_rect(&sp->slots[i].tile, srv->ptr_x,
				    srv->ptr_y))
					return node_leaf(sp->root,
					    sp->slots[i].w);
		/* FALLTHROUGH */
	case SPLITS_FOCUSED:
		/* the focused tile; windows that join together split the
		   one before them, the way they would one by one */
		if ((l = node_leaf(sp->root, prev)) != NULL ||
		    (l = node_leaf(sp->root, t->focus)) != NULL ||
		    (k != NULL && (l = node_leaf(sp->root, k)) != NULL))
			return l;
		return NULL;
	default:
		return largest_leaf(srv);
	}
}

/*
 * Work out which windows tile: those that no longer may leave (floating
 * again where they were, if they're still shown), new ones join, the
 * key window first, then the frontmost. Each new one splits a tile
 * (split_target) and comes at the end of the slots (master: the end of
 * the stack).
 */
static void
sync_windows(struct ws_server *srv)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_window *w, *key = key_window(srv), **add, *prev = NULL;
	unsigned i, n = 0, nadd = 0;

	for (i = sp->nslots; i-- > 0;) {
		w = sp->slots[i].w;
		if (tileable(srv, w))
			continue;
		if (!w->visible || w->minimized) {
			/* hidden, minimized: it comes back when shown; if
			   it had the focus, the tile under the pointer
			   takes it */
			if (w == srv->tiling.focus)
				srv->tiling.refocus = true;
			slot_remove(sp, i);
			w->tiled = false;
			continue;
		}
		leave(srv, i, true);
	}
	for (w = srv->stack_top; w != NULL; w = w->below)
		n++;
	if (n == 0 || (add = calloc(n, sizeof(*add))) == NULL)
		return;
	if (key != NULL && !key->tiled && tileable(srv, key))
		add[nadd++] = key;
	for (w = srv->stack_top; w != NULL; w = w->below)
		if (w != key && !w->tiled && tileable(srv, w))
			add[nadd++] = w;
	for (i = 0; i < nadd; i++) {
		enter(srv, add[i], split_target(srv, prev));
		prev = add[i];
	}
	free(add);
}

/* ---- layout ---- */

/*
 * Split r along its width (vertical: a vertical line) or height, `ratio`
 * of it, less the gap, to the first side. *pos is where it ends.
 */
static void
split_two(const ws_rect_t *r, bool vertical, int gap, double ratio,
    ws_rect_t *first, ws_rect_t *second, int32_t *pos)
{
	int32_t len = vertical ? r->width : r->height;
	int32_t avail = len - gap, a;

	if (avail < 2)
		avail = 2;
	a = (int32_t)lround(avail * ratio);
	if (a < 1)
		a = 1;
	if (a > avail - 1)
		a = avail - 1;
	*first = *second = *r;
	if (vertical) {
		first->width = a;
		second->x = r->x + a + gap;
		second->width = r->width - a - gap;
		*pos = r->x + a;
	} else {
		first->height = a;
		second->y = r->y + a + gap;
		second->height = r->height - a - gap;
		*pos = r->y + a;
	}
	if (second->width < 1)
		second->width = 1;
	if (second->height < 1)
		second->height = 1;
}

static void
add_split(struct ws_server *srv, int kind, struct ws_tiling_node *node,
    unsigned slot, bool vertical, const ws_rect_t *area, int32_t pos)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_tiling_split *s;
	int gap = gap_inner(srv), wide = gap > MIN_HANDLE ? gap : MIN_HANDLE;
	int32_t at = pos + gap / 2 - wide / 2;

	if (sp->nsplits >= sp->cap)
		return;
	s = &sp->splits[sp->nsplits++];
	s->kind = kind;
	s->node = node;
	s->slot = slot;
	s->vertical = vertical;
	s->area = *area;
	s->pos = pos;
	if (vertical) {
		s->handle.x = at;
		s->handle.y = area->y;
		s->handle.width = wide;
		s->handle.height = area->height;
	} else {
		s->handle.x = area->x;
		s->handle.y = at;
		s->handle.width = area->width;
		s->handle.height = wide;
	}
}

/*
 * Dwindle: lay out node n in r. Each leaf's slot moves to *next, so the
 * slots end up in the tree's order (left or top first).
 */
static void
compute_node(struct ws_server *srv, struct ws_tiling_node *n,
    const ws_rect_t *r, unsigned *next)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_tiling_slot tmp;
	ws_rect_t a, b;
	int32_t pos;
	bool vertical;
	int i;

	if (n->child[0] == NULL) {
		if ((i = slot_of(sp, n->w)) < 0)
			return;
		if ((unsigned)i != *next) {
			tmp = sp->slots[i];
			sp->slots[i] = sp->slots[*next];
			sp->slots[*next] = tmp;
		}
		sp->slots[(*next)++].tile = *r;
		return;
	}
	vertical = r->width >= r->height;
	split_two(r, vertical, gap_inner(srv), clampd(n->ratio, RATIO_MIN,
	    RATIO_MAX), &a, &b, &pos);
	add_split(srv, SPLIT_RATIO, n, 0, vertical, r, pos);
	compute_node(srv, n->child[0], &a, next);
	compute_node(srv, n->child[1], &b, next);
}

/* Work out every slot's tile and the splits between them. */
static void
compute(struct ws_server *srv)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	int gap = gap_inner(srv);
	ws_rect_t r = sp->area, first, rest;
	unsigned i, n = sp->nslots;
	int32_t pos, start, len, at, l;
	double total;
	bool vertical;

	sp->nsplits = 0;
	if (n == 0)
		return;
	if (n == 1) {
		sp->slots[0].tile = r;
		return;
	}
	if (srv->tiling.layout == WS_TILING_DWINDLE) {
		i = 0;
		if (sp->root != NULL)
			compute_node(srv, sp->root, &r, &i);
		return;
	}
	/* master and stack: the master beside (or above) the stack */
	vertical = r.width >= r.height;
	split_two(&r, vertical, gap, clampd(sp->master_ratio, RATIO_MIN,
	    RATIO_MAX), &first, &rest, &pos);
	sp->slots[0].tile = first;
	add_split(srv, SPLIT_MASTER, NULL, 0, vertical, &r, pos);
	/* the stack runs the other way */
	start = vertical ? rest.y : rest.x;
	len = (vertical ? rest.height : rest.width) - gap * (int32_t)(n - 2);
	for (i = 1, total = 0; i < n; i++)
		total += clampd(sp->slots[i].weight, WEIGHT_MIN, WEIGHT_MAX);
	for (i = 1, at = start; i < n; i++) {
		if (i == n - 1)
			l = start + (vertical ? rest.height : rest.width) - at;
		else
			l = (int32_t)lround(len * clampd(sp->slots[i].weight,
			    WEIGHT_MIN, WEIGHT_MAX) / total);
		if (l < 1)
			l = 1;
		first = rest;
		if (vertical) {
			first.y = at;
			first.height = l;
		} else {
			first.x = at;
			first.width = l;
		}
		sp->slots[i].tile = first;
		if (i < n - 1)
			add_split(srv, SPLIT_STACK, NULL, i, !vertical, &rest,
			    at + l);
		at += l + gap;
	}
}

/* The tiles as they are, for squeezed() after a split moves (NULL if
   there's no memory: then nothing stops the split). */
static ws_rect_t *
tiles_take(const struct ws_tiling_space *sp)
{
	ws_rect_t *old;
	unsigned i;

	if (sp->nslots == 0 ||
	    (old = calloc(sp->nslots, sizeof(*old))) == NULL)
		return NULL;
	for (i = 0; i < sp->nslots; i++)
		old[i] = sp->slots[i].tile;
	return old;
}

/*
 * Did the tiles shrink a window below its minimum size, or one already
 * below it further? Layouts don't keep windows to their minimum size,
 * but the user's resizing stops there.
 */
static bool
squeezed(const struct ws_tiling_space *sp, const ws_rect_t *old)
{
	const ws_rect_t *t;
	const struct ws_window *w;
	unsigned i;

	if (old == NULL)
		return false;
	for (i = 0; i < sp->nslots; i++) {
		t = &sp->slots[i].tile;
		w = sp->slots[i].w;
		if ((t->width < old[i].width &&
		    (int64_t)t->width < w->min_width) ||
		    (t->height < old[i].height &&
		    (int64_t)t->height < w->min_height))
			return true;
	}
	return false;
}

/*
 * Give w its tile. A maximum size smaller than the tile centres it. An
 * owner that took a smaller frame for this same tile keeps that size,
 * at the tile's corner. A window the user is dragging stays with the
 * pointer.
 */
static void
place(struct ws_server *srv, struct ws_window *w, const ws_rect_t *tile)
{
	ws_rect_t f = *tile;

	if (w->max_width > 0 && f.width > (int64_t)w->max_width) {
		f.x += (f.width - (int32_t)w->max_width) / 2;
		f.width = (int32_t)w->max_width;
	}
	if (w->max_height > 0 && f.height > (int64_t)w->max_height) {
		f.y += (f.height - (int32_t)w->max_height) / 2;
		f.height = (int32_t)w->max_height;
	}
	if (!rect_eq(&f, &w->tiling_given)) {
		w->tiling_given = f;
		w->tiling_given_usec = ws_now_usec();
		w->tiling_short = false;
		w->tiling_clipped = false;
		w->tiling_insist = 0;
	}
	if (w->tiling_clipped) {
		/* it keeps asking for a larger frame: cut to its tile */
		f = clipped(w);
		if (srv->tiling.moving != w && !rect_eq(&f, &w->frame))
			window_set_frame(srv, w, &f);
		return;
	}
	if (w->tiling_own_size && w->buffer != NULL && w->buffer->scale > 0) {
		/* an owner that only moves its windows: the size it draws,
		   cut to the tile */
		f.width = (int32_t)(w->buffer->width / w->buffer->scale);
		f.height = (int32_t)(w->buffer->height / w->buffer->scale);
		if (f.width > tile->width)
			f.width = tile->width;
		if (f.height > tile->height)
			f.height = tile->height;
	} else if (w->tiling_short && w->frame.width <= f.width &&
	    w->frame.height <= f.height) {
		f.width = w->frame.width;
		f.height = w->frame.height;
	}
	if (srv->tiling.moving == w || rect_eq(&f, &w->frame))
		return;
	window_server_frame(srv, w, &f);
}

/*
 * Lay the tiled windows out in the usable area. Every one gets its tile,
 * whatever its minimum size (as on Hyprland): two windows always sit
 * side by side.
 */
static void
layout(struct ws_server *srv)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_tiling *t = &srv->tiling;
	unsigned i;

	compute(srv);
	for (i = 0; i < sp->nslots; i++)
		place(srv, sp->slots[i].w, &sp->slots[i].tile);
	if (t->hover_split >= (int)sp->nsplits)
		t->hover_split = -1;
	if (t->drag_split >= (int)sp->nsplits)
		t->drag_split = -1;
}

/* ---- the border round the key window ---- */

static void
border_update(struct ws_server *srv)
{
	struct ws_tiling *t = &srv->tiling;
	struct ws_window *k = key_window(srv);
	int bw = srv->cfg.tiling_border;
	ws_rect_t r = { 0, 0, 0, 0 };

	if (bw > 16)
		bw = 16;
	if (t->on && bw > 0 && k != NULL && k->tiled && k->visible &&
	    !overview_active(srv)) {
		r.x = k->frame.x - bw;
		r.y = k->frame.y - bw;
		r.width = k->frame.width + 2 * bw;
		r.height = k->frame.height + 2 * bw;
	} else
		k = NULL;
	if (k == t->border && rect_eq(&r, &t->border_rect))
		return;
	if (t->border != NULL)
		damage_rect(srv, &t->border_rect);
	t->border = k;
	t->border_rect = r;
	if (k != NULL)
		damage_rect(srv, &r);
}

/* Draw the border round w (compositor.c, just before w itself). */
void
tiling_draw_border(struct ws_server *srv, struct ws_window *w)
{
	const ws_rect_t *f = &w->frame;
	uint32_t c = srv->cfg.tiling_border_color;
	int32_t bw = srv->cfg.tiling_border > 16 ? 16 : srv->cfg.tiling_border;
	pixman_box32_t b[4];
	pixman_color_t col;

	if (bw <= 0)
		return;
	col.red = (uint16_t)(((c >> 16) & 0xff) * 0x101);
	col.green = (uint16_t)(((c >> 8) & 0xff) * 0x101);
	col.blue = (uint16_t)((c & 0xff) * 0x101);
	col.alpha = 0xffff;
	b[0] = (pixman_box32_t){ f->x - bw, f->y - bw, f->x + f->width + bw,
	    f->y };
	b[1] = (pixman_box32_t){ f->x - bw, f->y + f->height,
	    f->x + f->width + bw, f->y + f->height + bw };
	b[2] = (pixman_box32_t){ f->x - bw, f->y, f->x, f->y + f->height };
	b[3] = (pixman_box32_t){ f->x + f->width, f->y, f->x + f->width + bw,
	    f->y + f->height };
	for (int i = 0; i < 4; i++) {	/* points to backing pixels */
		b[i].x1 *= (int32_t)srv->scale;
		b[i].y1 *= (int32_t)srv->scale;
		b[i].x2 *= (int32_t)srv->scale;
		b[i].y2 *= (int32_t)srv->scale;
	}
	pixman_image_fill_boxes(PIXMAN_OP_SRC, srv->shadow, &col, 4, b);
}

/* ---- the event loop ---- */

/* Something tiling depends on changed: lay out again at the end of this
   pass. */
void
tiling_changed(struct ws_server *srv)
{
	srv->tiling.dirty = true;
}

/* The area tiles go in: the usable area less the outer gaps. */
static void
area_update(struct ws_server *srv)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	int go = gap_outer(srv);

	sp->area = srv->usable;
	if (sp->area.width > 4 * go && sp->area.height > 4 * go) {
		sp->area.x += go;
		sp->area.y += go;
		sp->area.width -= 2 * go;
		sp->area.height -= 2 * go;
	}
}

/* If the layout is dirty, work it out now. */
static void
settle(struct ws_server *srv)
{
	struct ws_tiling *t = &srv->tiling;

	if (!t->dirty)
		return;
	t->dirty = false;
	if (!t->on)
		return;
	area_update(srv);
	sync_windows(srv);
	t->starting = false;
	layout(srv);
}

/*
 * May the pointer move the focus now (TilingFocusFollowsMouse)? Not
 * while a button is held (a click, a window or a split dragged), a drag
 * and drop lasts, a menu or popup holds the pointer grab, or the
 * overview is open, nor away from a panel or alert of the active app
 * that has the focus (floating, often over the tiles).
 */
static bool
follows_mouse(struct ws_server *srv)
{
	struct ws_tiling *t = &srv->tiling;
	struct ws_window *k = key_window(srv);

	return t->on && srv->cfg.tiling_focus_follows_mouse &&
	    srv->buttons == 0 && srv->grab == NULL && srv->pgrab == NULL &&
	    !drag_active(srv) && !overview_active(srv) && t->drag_split < 0 &&
	    t->moving == NULL && (k == NULL || k->tiled || !k->visible ||
	    ((k->flags & WS_WINDOW_PANEL) == 0 && k->level <= WS_LEVEL_NORMAL));
}

/* Tiled window w takes the focus from the pointer: its app is made
   active with w key. Tiles don't overlap: it isn't raised. */
static void
pointer_focus(struct ws_server *srv, struct ws_window *w)
{
	struct ws_client *a = w->owner->app;

	srv->tiling.focus = w;
	if (srv->active == a && srv->dock_front == NULL && a->key_window == w)
		return;
	ws_debug("tiling: window %u under the pointer takes the focus", w->id);
	app_activate(srv, a, w);
}

/*
 * The pointer moved (pointer.c), with no button held: the tiled window
 * under it takes the focus, as on Hyprland (follow_mouse = 1). Over the
 * Dock, a window that floats or a gap, the focus stays. A window that
 * just joined keeps the focus a moment, so it doesn't lose it to the
 * pointer the user hasn't moved yet.
 */
void
tiling_pointer_moved(struct ws_server *srv)
{
	struct ws_window *w;

	if (!follows_mouse(srv) ||
	    ws_now_usec() - srv->tiling.opened_usec < OPENED_USEC)
		return;
	if ((w = tiled_at_pointer(srv)) != NULL)
		pointer_focus(srv, w);
}

void
tiling_flush(struct ws_server *srv)
{
	struct ws_tiling *t = &srv->tiling;
	struct ws_window *k;

	settle(srv);
	/* The focused tile went: the one under the pointer takes the
	   focus (its sibling, which took its space, if the pointer was
	   over it). */
	if (t->refocus) {
		t->refocus = false;
		if (follows_mouse(srv)) {
			pointer_recheck(srv);
			if ((k = tiled_at_pointer(srv)) != NULL)
				pointer_focus(srv, k);
		}
	}
	k = key_window(srv);
	if (k != NULL && k->tiled)
		t->focus = k;
	border_update(srv);
}

/* w is going away. */
void
tiling_window_gone(struct ws_server *srv, struct ws_window *w)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_tiling *t = &srv->tiling;
	int i = slot_of(sp, w);

	if (i >= 0) {
		slot_remove(sp, (unsigned)i);
		w->tiled = false;
		t->dirty = true;
	}
	if (t->focus == w) {
		t->focus = NULL;
		t->refocus = i >= 0;
	}
	if (t->moving == w)
		t->moving = NULL;
	if (t->border == w) {
		damage_rect(srv, &t->border_rect);
		t->border = NULL;
	}
}

/*
 * w keeps asking for a frame larger than its tile (an NSWindow's
 * -setFrame: keeps to its minSize): it stays in its tile, cut to it. Its
 * owner hears the size it asked for at the tile's corner, so what it
 * draws lines up with the tile, and draws all of it; the screen and the
 * pointer only show the part in the tile.
 */
static void
cut_to_tile(struct ws_server *srv, struct ws_window *w)
{
	const ws_rect_t *g = &w->tiling_given;
	ws_rect_t f = w->frame, c;

	if (f.x != g->x || f.y != g->y) {
		f.x = g->x;
		f.y = g->y;
		window_server_frame(srv, w, &f);
	}
	w->tiling_asked = f;
	c = clipped(w);
	window_set_frame(srv, w, &c);
}

/*
 * w's owner gave it a frame (WS_MSG_WINDOW_SET_FRAME or a commit with
 * one). While the user drags it by a button pressed in it, it follows
 * (tiling_grab_end puts it in place). Otherwise the frame it was given
 * wins, except that a smaller one stays (an owner that sizes in steps,
 * a terminal's character cells) and one that keeps asking for a larger
 * one is cut to its tile.
 */
void
tiling_client_frame(struct ws_server *srv, struct ws_window *w)
{
	struct ws_tiling *t = &srv->tiling;
	const ws_rect_t *g = &w->tiling_given;

	if (!t->on || !w->tiled)
		return;
	if (srv->buttons != 0 && srv->grab == w && srv->pgrab == NULL &&
	    t->drag_split < 0) {
		if (t->moving != w)
			ws_debug("tiling: the user drags window %u", w->id);
		t->moving = w;
		return;
	}
	if (t->moving == w || g->width <= 0)
		return;
	if (rect_eq(&w->frame, g)) {
		w->tiling_fresh = false;
		w->tiling_insist = 0;
		w->tiling_clipped = false;
		return;
	}
	if (w->tiling_clipped) {
		if (w->frame.width > g->width || w->frame.height > g->height) {
			cut_to_tile(srv, w);	/* still larger */
			return;
		}
		w->tiling_clipped = false;	/* it fits now */
	}
	/* A window shown just now still settling its frame (a GNUstep
	   window adds its title bar): that is the frame it floats with. */
	if (w->tiling_fresh) {
		w->tiling_fresh = false;
		if (w->has_saved_frame)
			w->saved_frame = w->frame;
	}
	if (w->tiling_short && w->frame.x == g->x && w->frame.y == g->y &&
	    w->frame.width <= g->width && w->frame.height <= g->height)
		return;		/* the smaller size it kept */
	ws_debug("tiling: window %u's owner gave it %d,%d %dx%d (tile %d,%d "
	    "%dx%d)", w->id, w->frame.x, w->frame.y, w->frame.width,
	    w->frame.height, g->x, g->y, g->width, g->height);
	/*
	 * Another frame: one the owner sent before it heard of the tile (a
	 * new window's first frame), or its own idea. It gets its tile's
	 * frame again, a few times; an owner that keeps answering with
	 * another one means it.
	 */
	t->dirty = true;
	if (++w->tiling_insist < INSIST_LIMIT)
		return;
	if (w->frame.width <= g->width && w->frame.height <= g->height) {
		/* smaller: it keeps that size, at the tile's corner */
		w->tiling_short = true;
		return;
	}
	ws_info("tiling: window %u (%s) keeps taking %dx%d, larger than its "
	    "%dx%d tile: it is cut to its tile", w->id, w->owner->app_name,
	    w->frame.width, w->frame.height, g->width, g->height);
	w->tiling_clipped = true;
	w->tiling_insist = 0;
	cut_to_tile(srv, w);
}

/*
 * w committed a buffer. An owner that hears its new frame but only
 * moves the window keeps drawing at its old size (an X server, which
 * moves its windows when WindowServer does but leaves their size to X
 * clients): after a few such commits the window takes the size it
 * draws, at its tile's corner, cut to the tile if that is larger.
 */
void
tiling_committed(struct ws_server *srv, struct ws_window *w)
{
	struct ws_tiling *t = &srv->tiling;
	struct ws_buffer *b = w->buffer;
	const ws_rect_t *g = &w->tiling_given;
	ws_rect_t r;
	int32_t bw, bh, cw, ch;

	if (!t->on || !w->tiled || w->tiling_clipped || b == NULL ||
	    b->scale == 0 || t->moving == w || g->width <= 0)
		return;
	bw = (int32_t)(b->width / b->scale);
	bh = (int32_t)(b->height / b->scale);
	cw = bw > g->width ? g->width : bw;
	ch = bh > g->height ? g->height : bh;
	if (bw == g->width && bh == g->height) {
		w->tiling_follows = true;
		w->tiling_own_size = false;
	}
	if ((bw == w->frame.width && bh == w->frame.height) ||
	    (w->tiling_own_size && cw == w->frame.width &&
	    ch == w->frame.height)) {
		w->tiling_drawn = 0;
		return;
	}
	/* An owner that resizes (GNUstep) may draw a few frames at the old
	   size first: only one that never did, a while after its tile
	   changed, counts. */
	if (w->tiling_follows ||
	    ws_now_usec() - w->tiling_given_usec < DRAWN_WAIT_USEC ||
	    ++w->tiling_drawn < INSIST_LIMIT)
		return;
	w->tiling_drawn = 0;
	t->dirty = true;
	if (bw <= g->width && bh <= g->height) {
		ws_debug("tiling: window %u draws %dx%d in its %dx%d tile: it "
		    "keeps that size", w->id, bw, bh, g->width, g->height);
		w->tiling_own_size = true;
		r = w->frame;
		r.width = bw;
		r.height = bh;
		window_server_frame(srv, w, &r);
		return;
	}
	ws_info("tiling: window %u (%s) draws %dx%d, larger than its %dx%d "
	    "tile: it is cut to its tile", w->id, w->owner->app_name, bw, bh,
	    g->width, g->height);
	w->tiling_own_size = true;
	r = w->frame;
	r.width = cw;
	r.height = ch;
	window_server_frame(srv, w, &r);
}

/* WS_MSG_WINDOW_SET_SIZE_LIMITS. */
void
tiling_set_size_limits(struct ws_server *srv, struct ws_window *w,
    const ws_req_size_limits_t *m)
{
	if (w->min_width == m->min_width && w->min_height == m->min_height &&
	    w->max_width == m->max_width && w->max_height == m->max_height)
		return;
	w->min_width = m->min_width;
	w->min_height = m->min_height;
	w->max_width = m->max_width;
	w->max_height = m->max_height;
	ws_debug("window %u: %ux%u at least, %ux%u at most (0: no limit)",
	    w->id, w->min_width, w->min_height, w->max_width, w->max_height);
	srv->tiling.dirty = true;	/* a maximum size centres it */
}

/* ---- the user's actions ---- */

/* The slot next to `from` in direction dir (WS_TILING_DIR_*), or -1. */
static int
neighbour(const struct ws_tiling_space *sp, int from, int dir)
{
	const ws_rect_t *a = &sp->slots[from].tile, *b;
	int64_t score, best_score = 0;
	int32_t dist, lo1, hi1, lo2, hi2, gap, cdiff;
	int best = -1;
	unsigned i;

	for (i = 0; i < sp->nslots; i++) {
		if ((int)i == from)
			continue;
		b = &sp->slots[i].tile;
		switch (dir) {
		case WS_TILING_DIR_LEFT:
			dist = a->x - (b->x + b->width);
			break;
		case WS_TILING_DIR_RIGHT:
			dist = b->x - (a->x + a->width);
			break;
		case WS_TILING_DIR_UP:
			dist = a->y - (b->y + b->height);
			break;
		case WS_TILING_DIR_DOWN:
			dist = b->y - (a->y + a->height);
			break;
		default:
			return -1;
		}
		if (dist < 0)
			continue;
		if (dir == WS_TILING_DIR_LEFT || dir == WS_TILING_DIR_RIGHT) {
			lo1 = a->y;
			hi1 = a->y + a->height;
			lo2 = b->y;
			hi2 = b->y + b->height;
		} else {
			lo1 = a->x;
			hi1 = a->x + a->width;
			lo2 = b->x;
			hi2 = b->x + b->width;
		}
		/* side by side first, then the nearest, then the most
		   in line */
		gap = (lo1 > lo2 ? lo1 : lo2) - (hi1 < hi2 ? hi1 : hi2);
		if (gap < 0)
			gap = 0;
		cdiff = abs((lo1 + hi1) / 2 - (lo2 + hi2) / 2);
		score = (int64_t)gap * 10000000000LL +
		    (int64_t)dist * 100000 + cdiff;
		if (best < 0 || score < best_score) {
			best = (int)i;
			best_score = score;
		}
	}
	return best;
}

/* Give w the focus: activate its app with w key, and raise it. */
static void
focus_window(struct ws_server *srv, struct ws_window *w)
{
	app_activate(srv, w->owner->app, w);
	window_order_by_server(srv, w, WS_ORDER_ABOVE, NULL);
	srv->tiling.focus = w;
}

static int
op_focus(struct ws_server *srv, int dir)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	int i = focused_slot(srv), j;

	if (sp->nslots == 0)
		return ENOENT;
	if (i < 0) {
		focus_window(srv, sp->slots[0].w);
		return 0;
	}
	j = neighbour(sp, i, dir);
	if (j < 0)
		return ENOENT;
	focus_window(srv, sp->slots[j].w);
	return 0;
}

static void
swap_slots(struct ws_server *srv, unsigned i, unsigned j)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_window *w = sp->slots[i].w;
	struct ws_tiling_node *a = node_leaf(sp->root, w),
	    *b = node_leaf(sp->root, sp->slots[j].w);

	if (a != NULL && b != NULL) {
		a->w = sp->slots[j].w;
		b->w = w;
	}
	sp->slots[i].w = sp->slots[j].w;
	sp->slots[j].w = w;
	srv->tiling.dirty = true;
}

static int
op_swap(struct ws_server *srv, int dir)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	int i = focused_slot(srv), j;

	if (i < 0)
		return ENOENT;
	j = neighbour(sp, i, dir);
	if (j < 0)
		return ENOENT;
	swap_slots(srv, (unsigned)i, (unsigned)j);
	return 0;
}

/*
 * The values a split's move may change, to put back when the result
 * squeezes a window below its minimum size.
 */
struct split_backup {
	double	master_ratio;
	double	weight[2];
	unsigned slot;
	struct ws_tiling_node *node;	/* dwindle: the node split */
	double	ratio;
};

static void
backup_take(struct ws_tiling_space *sp, unsigned slot,
    struct ws_tiling_node *node, struct split_backup *b)
{
	unsigned k;

	b->master_ratio = sp->master_ratio;
	b->slot = slot;
	b->node = node;
	b->ratio = node != NULL ? node->ratio : 0.5;
	for (k = 0; k < 2; k++)
		b->weight[k] = slot + k < sp->nslots ?
		    sp->slots[slot + k].weight : 1.0;
}

static void
backup_put(struct ws_tiling_space *sp, const struct split_backup *b)
{
	unsigned k;

	sp->master_ratio = b->master_ratio;
	if (b->node != NULL)
		b->node->ratio = b->ratio;
	for (k = 0; k < 2; k++)
		if (b->slot + k < sp->nslots)
			sp->slots[b->slot + k].weight = b->weight[k];
}

/* Move split s (as it was laid out) so its first side ends at pos. */
static void
split_move(struct ws_server *srv, const struct ws_tiling_split *s,
    int32_t pos)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	int gap = gap_inner(srv);
	int32_t start = s->vertical ? s->area.x : s->area.y;
	int32_t len = (s->vertical ? s->area.width : s->area.height) - gap;
	const ws_rect_t *a, *b;
	double r, total, na, wsum;

	if (len < 2)
		return;
	switch (s->kind) {
	case SPLIT_RATIO:
	case SPLIT_MASTER:
		r = clampd((double)(pos - start) / len, RATIO_MIN, RATIO_MAX);
		if (s->kind == SPLIT_MASTER)
			sp->master_ratio = r;
		else if (s->node != NULL)
			s->node->ratio = r;
		break;
	case SPLIT_STACK:
		if (s->slot + 1 >= sp->nslots)
			return;
		a = &sp->slots[s->slot].tile;
		b = &sp->slots[s->slot + 1].tile;
		total = s->vertical ? a->width + b->width :
		    a->height + b->height;
		if (total < 2)
			return;
		na = clampd(pos - (s->vertical ? a->x : a->y), total * 0.1,
		    total * 0.9);
		wsum = clampd(sp->slots[s->slot].weight, WEIGHT_MIN,
		    WEIGHT_MAX) + clampd(sp->slots[s->slot + 1].weight,
		    WEIGHT_MIN, WEIGHT_MAX);
		sp->slots[s->slot].weight = wsum * na / total;
		sp->slots[s->slot + 1].weight = wsum - wsum * na / total;
		break;
	}
}

/* Grow (step > 0) or shrink the focused tile. */
static int
op_resize(struct ws_server *srv, int step)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	int i = focused_slot(srv), n = (int)sp->nslots;
	double d = step > 0 ? RESIZE_STEP : -RESIZE_STEP;
	struct split_backup b;
	struct ws_tiling_slot *s;
	struct ws_tiling_node *l, *p;
	ws_rect_t *old;

	if (i < 0 || n < 2 || step == 0)
		return ENOENT;
	if (srv->tiling.layout == WS_TILING_DWINDLE) {
		/* the split that makes this tile: its side grows */
		l = node_leaf(sp->root, sp->slots[i].w);
		if (l == NULL || (p = l->parent) == NULL)
			return ENOENT;
		backup_take(sp, 0, p, &b);
		p->ratio = clampd(p->ratio + (p->child[0] == l ? d : -d),
		    RATIO_MIN, RATIO_MAX);
	} else {
		backup_take(sp, i > 0 ? (unsigned)i - 1 : 0, NULL, &b);
		if (i == 0 || n == 2) {
			sp->master_ratio = clampd(sp->master_ratio +
			    (i == 0 ? d : -d), RATIO_MIN, RATIO_MAX);
		} else {
			s = &sp->slots[i];
			s->weight = clampd(s->weight * (step > 0 ? 1.25 : 0.8),
			    WEIGHT_MIN, WEIGHT_MAX);
		}
	}
	old = tiles_take(sp);
	compute(srv);
	if (squeezed(sp, old)) {
		backup_put(sp, &b);
		compute(srv);
	}
	free(old);
	srv->tiling.dirty = true;
	return 0;
}

/* how: 1 float w, 0 tile it again, 2 toggle. */
static int
op_float(struct ws_server *srv, struct ws_window *w, int how)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	bool floating = !w->tiled;
	int i;

	if (how == 2)
		how = floating ? 0 : 1;
	if (how == 1) {
		if ((i = slot_of(sp, w)) < 0)
			return 0;
		w->tiling_float = true;
		leave(srv, (unsigned)i, true);
		window_order_by_server(srv, w, WS_ORDER_ABOVE, NULL);
	} else {
		if (!w->tiling_float)
			return w->tiled ? 0 : EINVAL;	/* it can't tile */
		w->tiling_float = false;
		w->tiling_insist = 0;
	}
	srv->tiling.dirty = true;
	return 0;
}

/*
 * Keys the server takes in tiling mode (see "Window tiling" in
 * WSProtocol.h). Returns true if it took the key.
 */
bool
tiling_key(struct ws_server *srv, xkb_keysym_t sym)
{
	struct ws_tiling *t = &srv->tiling;
	uint32_t mods = srv->modifiers & (WS_MOD_SHIFT | WS_MOD_CONTROL |
	    WS_MOD_ALT | WS_MOD_COMMAND);
	bool shift = (mods & WS_MOD_SHIFT) != 0;
	struct ws_window *k;
	int dir = 0;

	if (!t->on || t->modifier == 0 || (mods & ~WS_MOD_SHIFT) != t->modifier)
		return false;
	switch (sym) {
	case XKB_KEY_Left:
	case XKB_KEY_KP_Left:
		dir = WS_TILING_DIR_LEFT;
		break;
	case XKB_KEY_Right:
	case XKB_KEY_KP_Right:
		dir = WS_TILING_DIR_RIGHT;
		break;
	case XKB_KEY_Up:
	case XKB_KEY_KP_Up:
		dir = WS_TILING_DIR_UP;
		break;
	case XKB_KEY_Down:
	case XKB_KEY_KP_Down:
		dir = WS_TILING_DIR_DOWN;
		break;
	default:
		break;
	}
	settle(srv);
	if (dir != 0) {
		if (shift)
			(void)op_swap(srv, dir);
		else
			(void)op_focus(srv, dir);
		return true;
	}
	if (!shift && (sym == XKB_KEY_minus || sym == XKB_KEY_KP_Subtract)) {
		(void)op_resize(srv, -1);
		return true;
	}
	if (!shift && (sym == XKB_KEY_equal || sym == XKB_KEY_plus ||
	    sym == XKB_KEY_KP_Add)) {
		(void)op_resize(srv, 1);
		return true;
	}
	if (shift && sym == XKB_KEY_space) {
		if ((k = key_window(srv)) != NULL)
			(void)op_float(srv, k, 2);
		return true;
	}
	return false;
}

/* ---- the pointer: splits and dragged windows ---- */

/* The split under the pointer that a press would take, or -1. Not
   where a floating window is on top. */
static int
split_at_pointer(struct ws_server *srv)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_window *w;
	unsigned i;

	if (!srv->tiling.on || srv->tiling.dirty || sp->nsplits == 0 ||
	    overview_active(srv) || srv->pgrab != NULL)
		return -1;
	for (i = 0; i < sp->nsplits; i++)
		if (in_rect(&sp->splits[i].handle, srv->ptr_x, srv->ptr_y))
			break;
	if (i == sp->nsplits)
		return -1;
	w = window_at(srv, srv->ptr_x, srv->ptr_y);
	if (w != NULL && !w->tiled && w->level >= WS_LEVEL_NORMAL)
		return -1;
	return (int)i;
}

bool
tiling_press(struct ws_server *srv, uint32_t button)
{
	struct ws_tiling *t = &srv->tiling;
	int i;

	if (button != WS_BUTTON_LEFT || (i = split_at_pointer(srv)) < 0)
		return false;
	t->drag_split = i;
	t->hover_split = i;
	srv->buttons |= 1u << button;
	ws_debug("tiling: the user drags split %d", i);
	cursor_update(srv);
	return true;
}

bool
tiling_release(struct ws_server *srv, uint32_t button)
{
	struct ws_tiling *t = &srv->tiling;

	if (t->drag_split < 0 || button != WS_BUTTON_LEFT ||
	    (srv->buttons & (1u << button)) == 0)
		return false;
	srv->buttons &= ~(1u << button);
	t->drag_split = -1;
	t->hover_split = split_at_pointer(srv);
	pointer_recheck(srv);
	cursor_update(srv);
	return true;
}

/* The user drags split t->drag_split to the pointer. */
static void
split_drag(struct ws_server *srv)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_tiling *t = &srv->tiling;
	struct ws_tiling_split s = sp->splits[t->drag_split];
	struct split_backup b;
	ws_rect_t *old;
	int32_t pos;

	pos = (int32_t)lround((s.vertical ? srv->ptr_x : srv->ptr_y) -
	    gap_inner(srv) / 2.0);
	backup_take(sp, s.slot, s.node, &b);
	split_move(srv, &s, pos);
	old = tiles_take(sp);
	compute(srv);
	if (squeezed(sp, old)) {
		/* a window's minimum size stops it here */
		backup_put(sp, &b);
		compute(srv);
		free(old);
		return;
	}
	free(old);
	t->dirty = true;
}

/* Pointer motion: a split being dragged takes it (true); otherwise
   note the split under the pointer, for the cursor. */
bool
tiling_motion(struct ws_server *srv)
{
	struct ws_tiling *t = &srv->tiling;
	int h;

	if (t->drag_split >= 0) {
		if (t->drag_split < (int)tiling_space(srv)->nsplits)
			split_drag(srv);
		return true;
	}
	h = srv->buttons == 0 && srv->grab == NULL ? split_at_pointer(srv) :
	    -1;
	if (h != t->hover_split) {
		t->hover_split = h;
		cursor_update(srv);
	}
	return false;
}

/* The cursor tiling wants (a split under the pointer), or -1. */
int
tiling_cursor(struct ws_server *srv)
{
	struct ws_tiling *t = &srv->tiling;
	struct ws_tiling_space *sp = tiling_space(srv);
	int i = t->drag_split >= 0 ? t->drag_split : t->hover_split;

	if (!t->on || i < 0 || i >= (int)sp->nsplits)
		return -1;
	return sp->splits[i].vertical ? WS_CURSOR_RESIZE_LR :
	    WS_CURSOR_RESIZE_UD;
}

/*
 * The user resized tiled window w with a button pressed in it (its
 * resize bar): the splits along the edges it moved follow.
 */
static void
edges_follow(struct ws_server *srv, struct ws_window *w)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	const ws_rect_t g = w->tiling_given, f = w->frame;
	int gap = gap_inner(srv);
	struct {
		bool	vertical;
		bool	first;		/* w is the split's first side */
		int32_t	was;
		int32_t	now;
	} e[4] = {
		{ true, true, g.x + g.width, f.x + f.width },
		{ true, false, g.x, f.x },
		{ false, true, g.y + g.height, f.y + f.height },
		{ false, false, g.y, f.y },
	};
	const struct ws_tiling_split *s;
	unsigned k, i;
	int at;

	for (k = 0; k < 4; k++) {
		if (e[k].was == e[k].now || (at = slot_of(sp, w)) < 0)
			continue;
		for (i = 0; i < sp->nsplits; i++) {
			s = &sp->splits[i];
			if (s->vertical != e[k].vertical ||
			    (e[k].first ? s->pos : s->pos + gap) != e[k].was ||
			    !in_rect(&s->area, sp->slots[at].tile.x,
			    sp->slots[at].tile.y))
				continue;
			split_move(srv, s, e[k].first ? e[k].now :
			    e[k].now - gap);
			compute(srv);
			break;
		}
	}
}

/*
 * The buttons pressed in w were released. If the user dragged tiled
 * window w around, it goes to its tile again, swapping places with the
 * tile it was dropped on; if the user resized it, the splits follow.
 */
void
tiling_grab_end(struct ws_server *srv, struct ws_window *w)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_tiling *t = &srv->tiling;
	unsigned i;
	int at;

	if (t->moving != w)
		return;
	t->moving = NULL;
	t->dirty = true;
	if (!t->on || (at = slot_of(sp, w)) < 0)
		return;
	if (w->frame.width != w->tiling_given.width ||
	    w->frame.height != w->tiling_given.height) {
		ws_debug("tiling: window %u resized by the user", w->id);
		edges_follow(srv, w);
		return;
	}
	for (i = 0; i < sp->nslots; i++)
		if ((int)i != at && in_rect(&sp->slots[i].tile, srv->ptr_x,
		    srv->ptr_y)) {
			ws_debug("tiling: window %u dropped on window %u: "
			    "they swap", w->id, sp->slots[i].w->id);
			swap_slots(srv, (unsigned)at, i);
			break;
		}
}

/* ---- settings ---- */

static void
read_config(struct ws_server *srv)
{
	struct ws_tiling *t = &srv->tiling;

	t->layout = strcasecmp(srv->cfg.tiling_layout, "master") == 0 ?
	    WS_TILING_MASTER : WS_TILING_DWINDLE;
	if (!tiling_modifier_parse(srv->cfg.tiling_modifier, &t->modifier))
		t->modifier = WS_MOD_COMMAND | WS_MOD_ALT;
	t->splits = strcasecmp(srv->cfg.tiling_new_window_splits,
	    "pointer") == 0 ? SPLITS_POINTER :
	    strcasecmp(srv->cfg.tiling_new_window_splits, "focused") == 0 ?
	    SPLITS_FOCUSED : SPLITS_LARGEST;
}

static bool
mode_tiling(const struct ws_config *cfg)
{
	return strcasecmp(cfg->window_mode, "tiling") == 0;
}

/* Back to stacking: every window floats, with its frame from before. */
static void
tiling_off(struct ws_server *srv)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_tiling *t = &srv->tiling;
	struct ws_window *w;

	t->on = false;
	while (sp->nslots > 0)
		leave(srv, sp->nslots - 1, true);
	for (w = srv->windows; w != NULL; w = w->next) {
		if (w->has_saved_frame) {
			w->has_saved_frame = false;
			if (!rect_eq(&w->frame, &w->saved_frame))
				window_server_frame(srv, w, &w->saved_frame);
		}
		w->tiling_float = false;
		w->tiling_clipped = false;
		w->tiling_insist = 0;
		w->tiling_short = false;
	}
	t->drag_split = t->hover_split = -1;
	t->moving = NULL;
	t->focus = NULL;
	t->refocus = false;
	cursor_update(srv);
}

void
tiling_config_changed(struct ws_server *srv, const struct ws_config *old)
{
	struct ws_tiling *t = &srv->tiling;
	bool on = mode_tiling(&srv->cfg);

	read_config(srv);
	if (srv->cfg.tiling_master_ratio != old->tiling_master_ratio)
		tiling_space(srv)->master_ratio = clampd(
		    srv->cfg.tiling_master_ratio, RATIO_MIN, RATIO_MAX);
	if (on != t->on)
		ws_info("tiling: %s", on ? "on" : "off, windows stack");
	if (on && !t->on) {
		t->on = true;
		t->starting = true;
		t->focus = NULL;
	} else if (!on && t->on)
		tiling_off(srv);
	/* the border's width or colour may have changed */
	if (t->border != NULL)
		damage_rect(srv, &t->border_rect);
	t->border = NULL;
	t->dirty = true;
}

void
tiling_init(struct ws_server *srv)
{
	struct ws_tiling *t = &srv->tiling;

	memset(t, 0, sizeof(*t));
	t->drag_split = t->hover_split = -1;
	t->space.master_ratio = clampd(srv->cfg.tiling_master_ratio, RATIO_MIN,
	    RATIO_MAX);
	read_config(srv);
	t->on = mode_tiling(&srv->cfg);
	t->dirty = true;
	if (t->on)
		ws_info("tiling: on");
}

void
tiling_fini(struct ws_server *srv)
{
	struct ws_tiling_space *sp = tiling_space(srv);

	node_free(sp->root);
	sp->root = NULL;
	free(sp->slots);
	free(sp->splits);
	sp->slots = NULL;
	sp->splits = NULL;
	sp->nslots = sp->cap = sp->nsplits = 0;
}

/* ---- WS_MSG_TILING ---- */

static void
report_item(struct ws_server *srv, struct ws_client *to, struct ws_window *w,
    uint32_t flags, uint32_t index, const ws_rect_t *tile)
{
	ws_event_t ev;

	event_init(srv, &ev, WS_EVENT_TILING_ITEM, sizeof(ev.u.tiling_item),
	    w->id);
	ev.u.tiling_item.window = w->id;
	ev.u.tiling_item.app_id = w->owner->app->id;
	ev.u.tiling_item.flags = flags;
	if (w == key_window(srv) && w->tiled)
		ev.u.tiling_item.flags |= WS_TILING_ITEM_FOCUSED;
	ev.u.tiling_item.index = index;
	if (tile != NULL)
		ev.u.tiling_item.tile = *tile;
	ev.u.tiling_item.frame = w->frame;
	client_send(srv, to, &ev, -1);
}

static void
report(struct ws_server *srv, struct ws_client *to)
{
	struct ws_tiling_space *sp = tiling_space(srv);
	struct ws_tiling *t = &srv->tiling;
	struct ws_window *w, *k = key_window(srv);
	ws_event_t ev;
	unsigned i;

	event_init(srv, &ev, WS_EVENT_TILING_STATE, sizeof(ev.u.tiling), 0);
	ev.u.tiling.mode = t->on ? WS_TILING_TILING : WS_TILING_STACKING;
	ev.u.tiling.layout = (uint32_t)t->layout;
	ev.u.tiling.count = sp->nslots;
	ev.u.tiling.focused = k != NULL && k->tiled ? k->id : 0;
	ev.u.tiling.area = sp->area;
	ev.u.tiling.gaps_inner = (uint32_t)gap_inner(srv);
	ev.u.tiling.gaps_outer = (uint32_t)gap_outer(srv);
	ev.u.tiling.border = (uint32_t)(srv->cfg.tiling_border > 0 ?
	    srv->cfg.tiling_border : 0);
	ev.u.tiling.modifier = t->modifier;
	client_send(srv, to, &ev, -1);
	for (i = 0; i < sp->nslots; i++)
		report_item(srv, to, sp->slots[i].w, WS_TILING_ITEM_TILED |
		    (sp->slots[i].w->tiling_clipped ? WS_TILING_ITEM_CUT : 0),
		    i, &sp->slots[i].tile);
	if (t->on)
		for (w = srv->windows; w != NULL; w = w->next)
			if (!w->tiled && w->visible && w->tiling_float)
				report_item(srv, to, w,
				    WS_TILING_ITEM_FLOATED, UINT32_MAX, NULL);
	event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
	client_send(srv, to, &ev, -1);
}

/* WS_MSG_TILING from a tool or the Dock: do `op`, then report. */
void
tiling_request(struct ws_server *srv, struct ws_client *c, uint32_t op,
    uint32_t window, int32_t arg)
{
	struct ws_window *w;
	int err = 0;

	settle(srv);
	if (op != WS_TILING_QUERY && !srv->tiling.on)
		err = ENOTSUP;
	else switch (op) {
	case WS_TILING_QUERY:
		break;
	case WS_TILING_FOCUS:
		err = op_focus(srv, arg);
		break;
	case WS_TILING_SWAP:
		err = op_swap(srv, arg);
		break;
	case WS_TILING_RESIZE:
		err = op_resize(srv, arg);
		break;
	case WS_TILING_FLOAT:
		w = window != 0 ? window_lookup(srv, window) : key_window(srv);
		if (w == NULL)
			err = ENOENT;
		else if (arg < 0 || arg > 2)
			err = EINVAL;
		else
			err = op_float(srv, w, arg);
		break;
	default:
		err = EINVAL;
		break;
	}
	if (err != 0)
		client_error(srv, c, WS_MSG_TILING, window, err,
		    err == ENOTSUP ? "windows aren't tiled" :
		    err == ENOENT ? "no such window or tile" :
		    "bad tiling request");
	settle(srv);
	report(srv, c);
}
