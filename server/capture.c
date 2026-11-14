/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Screen capture ("Screen capture" in WSProtocol.h): streams of the
 * composited screen or of one window, in shared-memory frame buffers,
 * damage-driven, and the Screen Recording consent the user gives
 * through the consent agent (ScreenCaptureAgent), with the indicator
 * border while anything records.
 *
 * Screen streams copy from the shadow image right after each repaint,
 * only what changed since each buffer was filled last (every buffer
 * keeps its own stale region), with the indicator border and, unless
 * the stream wants it, the software cursor drawn out first
 * (compositor_capture_overlays). Window streams copy the window's
 * committed buffer.
 */

#include <sys/types.h>
#include <sys/mman.h>

#include <errno.h>
#include <math.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

#define WS_CAPTURE_MAX_SIZE	16384

/* ---- processes ---- */

/* The key of a program: its executable's path, its end if it's long. */
static void
make_key(const char *path, char *key)
{
	size_t n = strlen(path);

	if (n < WS_CAPTURE_KEY_MAX)
		strlcpy(key, path, WS_CAPTURE_KEY_MAX);
	else
		strlcpy(key, path + n - (WS_CAPTURE_KEY_MAX - 1),
		    WS_CAPTURE_KEY_MAX);
}

static const char *
base_name(const char *path)
{
	const char *p = strrchr(path, '/');

	return p != NULL ? p + 1 : path;
}

/* ---- streams ---- */

static struct ws_capture *
capture_lookup(struct ws_server *srv, uint32_t id)
{
	struct ws_capture *s;

	for (s = srv->captures; s != NULL; s = s->next)
		if (s->id == id)
			return s;
	return NULL;
}

static void
buf_free(struct ws_capture_buf *b)
{
	if (b->image != NULL)
		pixman_image_unref(b->image);
	if (b->map != NULL)
		munmap(b->map, b->size);
	if (b->fd >= 0)
		close(b->fd);
	b->image = NULL;
	b->map = NULL;
	b->fd = -1;
	b->size = 0;
	b->busy = false;
	b->sent = false;
	pixman_region32_clear(&b->stale);
	memset(&b->cursor, 0, sizeof(b->cursor));
}

static void
capture_free(struct ws_capture *s)
{
	int i;

	for (i = 0; i < WS_CAPTURE_BUFFERS; i++) {
		buf_free(&s->buf[i]);
		pixman_region32_fini(&s->buf[i].stale);
	}
	pixman_region32_fini(&s->damage);
	free(s);
}

static void
send_state(struct ws_server *srv, struct ws_capture *s, int error)
{
	ws_event_t ev;

	event_init(srv, &ev, WS_EVENT_CAPTURE_STATE, sizeof(ev.u.capture_state),
	    s->window);
	ev.u.capture_state.stream = s->id;
	ev.u.capture_state.state = s->state;
	ev.u.capture_state.error = error;
	client_send(srv, s->owner, &ev, -1);
}

/* Streams that count for the indicator, and those waiting. */
static void
capture_counts(struct ws_server *srv, uint32_t *running, uint32_t *pending)
{
	struct ws_capture *s;

	*running = *pending = 0;
	for (s = srv->captures; s != NULL; s = s->next) {
		if (s->state == WS_CAPTURE_RUNNING && s->indicator &&
		    (s->flags & WS_CAPTURE_ONESHOT) == 0)
			(*running)++;
		else if (s->state == WS_CAPTURE_PENDING)
			(*pending)++;
	}
}

void
capture_indicator_rects(struct ws_server *srv, ws_rect_t r[4])
{
	int32_t w = (int32_t)srv->width, h = (int32_t)srv->height;
	int32_t b = WS_CAPTURE_BORDER;

	r[0] = (ws_rect_t){ 0, 0, w, b };		/* top */
	r[1] = (ws_rect_t){ 0, h - b, w, b };		/* bottom */
	r[2] = (ws_rect_t){ 0, b, b, h - 2 * b };	/* left */
	r[3] = (ws_rect_t){ w - b, b, b, h - 2 * b };	/* right */
}

/* The streams changed: the border, and the agent's view of them. */
static void
capture_changed(struct ws_server *srv)
{
	uint32_t running, pending;
	ws_rect_t r[4];
	bool on;
	int i;

	capture_counts(srv, &running, &pending);
	on = running > 0;
	if (on != srv->capture_indicator) {
		srv->capture_indicator = on;
		capture_indicator_rects(srv, r);
		for (i = 0; i < 4; i++)
			damage_rect(srv, &r[i]);
		ws_info("capture: indicator %s", on ? "on" : "off");
	}
	srv->capture_activity_dirty = true;
}

/* Take stream s out of the list and free it. */
static void
capture_remove(struct ws_server *srv, struct ws_capture *s)
{
	struct ws_capture **pp;

	for (pp = &srv->captures; *pp != NULL; pp = &(*pp)->next)
		if (*pp == s) {
			*pp = s->next;
			break;
		}
	capture_free(s);
	capture_changed(srv);
}

/* End stream s with `state` (DENIED or STOPPED), telling its owner. */
static void
capture_end(struct ws_server *srv, struct ws_capture *s, uint32_t state,
    int error)
{
	s->state = state;
	send_state(srv, s, error);
	if (state == WS_CAPTURE_DENIED)
		ws_info("capture: stream %u for %s (pid %d) refused: %s", s->id,
		    s->name, (int)s->pid, strerror(error));
	else
		ws_info("capture: stream %u for %s (pid %d) ended%s%s", s->id,
		    s->name, (int)s->pid, error != 0 ? ": " : "",
		    error != 0 ? strerror(error) : "");
	capture_remove(srv, s);
}

/* The source's size: the screen's, or the window's frame. */
/* A window's pixels per point. */
static uint32_t
win_scale(struct ws_window *w)
{
	return w->buffer != NULL && w->buffer->scale > 0 ? w->buffer->scale :
	    1;
}

static bool
source_size(struct ws_server *srv, struct ws_capture *s, uint32_t *w,
    uint32_t *h)
{
	struct ws_window *win;

	/* Sources are pixels: the panel's, or the window buffer's (HiDPI,
	   "HiDPI" in WSProtocol.h). */
	if (s->window == 0) {
		*w = srv->panel_width;
		*h = srv->panel_height;
		return true;
	}
	win = window_lookup(srv, s->window);
	if (win == NULL || win->frame.width <= 0 || win->frame.height <= 0)
		return false;
	*w = (uint32_t)win->frame.width * win_scale(win);
	*h = (uint32_t)win->frame.height * win_scale(win);
	return true;
}

/* Everything of the source is new to every buffer. */
static void
capture_damage_all(struct ws_capture *s)
{
	int i;

	pixman_region32_union_rect(&s->damage, &s->damage, 0, 0,
	    s->src_width, s->src_height);
	for (i = 0; i < WS_CAPTURE_BUFFERS; i++)
		pixman_region32_union_rect(&s->buf[i].stale, &s->buf[i].stale,
		    0, 0, s->src_width, s->src_height);
	s->full = true;
}

static void
capture_damage(struct ws_capture *s, const pixman_region32_t *r)
{
	int i;

	pixman_region32_union(&s->damage, &s->damage,
	    (pixman_region32_t *)(uintptr_t)r);
	for (i = 0; i < WS_CAPTURE_BUFFERS; i++)
		pixman_region32_union(&s->buf[i].stale, &s->buf[i].stale,
		    (pixman_region32_t *)(uintptr_t)r);
}

static void
capture_damage_rect(struct ws_capture *s, const ws_rect_t *r)
{
	pixman_region32_t reg;

	if (r->width <= 0 || r->height <= 0)
		return;
	pixman_region32_init_rect(&reg, r->x, r->y, (unsigned)r->width,
	    (unsigned)r->height);
	capture_damage(s, &reg);
	pixman_region32_fini(&reg);
}

/*
 * New buffers for the source's size (src_w x src_h): the old ones go,
 * the client keeps its own mappings of them until it hears of the new
 * generation.
 */
static int
capture_alloc(struct ws_capture *s, uint32_t src_w, uint32_t src_h)
{
	uint32_t w = s->req_width ? s->req_width : src_w;
	uint32_t h = s->req_height ? s->req_height : src_h;
	uint32_t stride = w * 4;
	struct ws_capture_buf *b;
	int i, n;

	n = (s->flags & WS_CAPTURE_ONESHOT) ? 1 : WS_CAPTURE_BUFFERS;
	for (i = 0; i < WS_CAPTURE_BUFFERS; i++)
		buf_free(&s->buf[i]);
	s->generation++;
	s->src_width = src_w;
	s->src_height = src_h;
	s->width = w;
	s->height = h;
	for (i = 0; i < n; i++) {
		b = &s->buf[i];
		b->size = (size_t)stride * h;
		b->fd = memfd_create("WindowServer-capture", MFD_CLOEXEC |
		    MFD_ALLOW_SEALING);
		if (b->fd < 0 || ftruncate(b->fd, (off_t)b->size) < 0)
			goto fail;
		/* The client may not shrink it under us. */
		(void)fcntl(b->fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);
		b->map = mmap(NULL, b->size, PROT_READ | PROT_WRITE, MAP_SHARED,
		    b->fd, 0);
		if (b->map == MAP_FAILED) {
			b->map = NULL;
			goto fail;
		}
		b->image = pixman_image_create_bits(PIXMAN_x8r8g8b8, (int)w,
		    (int)h, b->map, (int)stride);
		if (b->image == NULL)
			goto fail;
	}
	capture_damage_all(s);
	return 0;
fail:
	for (i = 0; i < WS_CAPTURE_BUFFERS; i++)
		buf_free(&s->buf[i]);
	s->src_width = s->src_height = 0;
	return -1;
}

static bool
scaled(const struct ws_capture *s)
{
	return s->width != s->src_width || s->height != s->src_height;
}

/* A source rectangle in output coordinates, rounded outwards. */
static void
scale_box(const struct ws_capture *s, const pixman_box32_t *in,
    ws_rect_t *out)
{
	int64_t x1, y1, x2, y2;

	if (!scaled(s)) {
		out->x = in->x1;
		out->y = in->y1;
		out->width = in->x2 - in->x1;
		out->height = in->y2 - in->y1;
		return;
	}
	x1 = (int64_t)in->x1 * s->width / s->src_width;
	y1 = (int64_t)in->y1 * s->height / s->src_height;
	x2 = ((int64_t)in->x2 * s->width + s->src_width - 1) / s->src_width;
	y2 = ((int64_t)in->y2 * s->height + s->src_height - 1) / s->src_height;
	/* bilinear filtering reaches a pixel further */
	x1 = x1 > 0 ? x1 - 1 : 0;
	y1 = y1 > 0 ? y1 - 1 : 0;
	x2 = x2 < s->width ? x2 + 1 : s->width;
	y2 = y2 < s->height ? y2 + 1 : s->height;
	out->x = (int32_t)x1;
	out->y = (int32_t)y1;
	out->width = (int32_t)(x2 - x1);
	out->height = (int32_t)(y2 - y1);
}

static void
scale_region(const struct ws_capture *s, pixman_region32_t *in,
    pixman_region32_t *out)
{
	pixman_box32_t *box;
	ws_rect_t r;
	int i, n;

	pixman_region32_init(out);
	box = pixman_region32_rectangles(in, &n);
	for (i = 0; i < n; i++) {
		scale_box(s, &box[i], &r);
		pixman_region32_union_rect(out, out, r.x, r.y,
		    (unsigned)r.width, (unsigned)r.height);
	}
}

/* Copy `region` (source coordinates) of src, at src_x, src_y of src,
   into buffer b, scaled to the output. */
static void
copy_region(struct ws_capture *s, struct ws_capture_buf *b,
    pixman_image_t *src, int src_x, int src_y, pixman_region32_t *region)
{
	pixman_region32_t clip;
	pixman_transform_t t;

	scale_region(s, region, &clip);
	pixman_image_set_clip_region32(b->image, &clip);
	if (!scaled(s)) {
		pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, b->image,
		    src_x, src_y, 0, 0, 0, 0, (int)s->width, (int)s->height);
	} else {
		pixman_transform_init_scale(&t,
		    pixman_double_to_fixed((double)s->src_width / s->width),
		    pixman_double_to_fixed((double)s->src_height / s->height));
		pixman_transform_translate(&t, NULL,
		    pixman_int_to_fixed(src_x), pixman_int_to_fixed(src_y));
		pixman_image_set_transform(src, &t);
		pixman_image_set_filter(src, PIXMAN_FILTER_BILINEAR, NULL, 0);
		pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, b->image,
		    0, 0, 0, 0, 0, 0, (int)s->width, (int)s->height);
		pixman_image_set_transform(src, NULL);
		pixman_image_set_filter(src, PIXMAN_FILTER_NEAREST, NULL, 0);
	}
	pixman_image_set_clip_region32(b->image, NULL);
	pixman_region32_fini(&clip);
}

/* Draw the pointer into b at r (source coordinates). */
static void
draw_cursor(struct ws_server *srv, struct ws_capture *s,
    struct ws_capture_buf *b, const ws_rect_t *r)
{
	pixman_transform_t t;
	pixman_box32_t box;
	ws_rect_t o;

	if (!scaled(s)) {
		pixman_image_composite32(PIXMAN_OP_OVER, srv->cursor_image,
		    NULL, b->image, 0, 0, 0, 0, r->x, r->y, r->width,
		    r->height);
		return;
	}
	box.x1 = r->x;
	box.y1 = r->y;
	box.x2 = r->x + r->width;
	box.y2 = r->y + r->height;
	scale_box(s, &box, &o);
	pixman_transform_init_scale(&t,
	    pixman_double_to_fixed((double)s->src_width / s->width),
	    pixman_double_to_fixed((double)s->src_height / s->height));
	pixman_image_set_transform(srv->cursor_image, &t);
	pixman_image_set_filter(srv->cursor_image, PIXMAN_FILTER_BILINEAR,
	    NULL, 0);
	pixman_image_composite32(PIXMAN_OP_OVER, srv->cursor_image, NULL,
	    b->image, 0, 0, 0, 0,
	    (int)((int64_t)r->x * s->width / s->src_width),
	    (int)((int64_t)r->y * s->height / s->src_height), o.width,
	    o.height);
	pixman_image_set_transform(srv->cursor_image, NULL);
	pixman_image_set_filter(srv->cursor_image, PIXMAN_FILTER_NEAREST,
	    NULL, 0);
}

/* Where the pointer shows in s's source, 0x0 = nowhere. */
static void
cursor_in_source(struct ws_server *srv, struct ws_capture *s, ws_rect_t *r)
{
	struct ws_window *w;

	uint32_t is = srv->cursor_image_scale ? srv->cursor_image_scale : 1;
	double k;

	memset(r, 0, sizeof(*r));
	if (!srv->cursor_visible || srv->cursor_image == NULL)
		return;
	/* in source pixels; the image is drawn at its own size */
	r->width = pixman_image_get_width(srv->cursor_image);
	r->height = pixman_image_get_height(srv->cursor_image);
	if (s->window != 0) {
		w = window_lookup(srv, s->window);
		if (w == NULL || !w->visible || window_at(srv, srv->ptr_x,
		    srv->ptr_y) != w) {
			memset(r, 0, sizeof(*r));
			return;
		}
		k = win_scale(w);
		r->x = (int32_t)floor((srv->ptr_x - w->frame.x -
		    srv->cursor_hot_x) * k);
		r->y = (int32_t)floor((srv->ptr_y - w->frame.y -
		    srv->cursor_hot_y) * k);
	} else {
		r->x = (int32_t)floor(panel_x(srv, srv->ptr_x -
		    srv->cursor_hot_x));
		r->y = (int32_t)floor(panel_y(srv, srv->ptr_y -
		    srv->cursor_hot_y));
	}
	(void)is;
}

/* Do we draw the pointer into s's frames ourselves? */
static bool
draws_cursor(struct ws_server *srv, struct ws_capture *s)
{
	return (s->flags & WS_CAPTURE_CURSOR) && (s->window != 0 ||
	    !compositor_cursor_in_shadow(srv));
}

/* ---- frames ---- */

static void
send_buffer(struct ws_server *srv, struct ws_capture *s, int i)
{
	ws_event_t ev;

	event_init(srv, &ev, WS_EVENT_CAPTURE_BUFFER,
	    sizeof(ev.u.capture_buffer), s->window);
	ev.u.capture_buffer.stream = s->id;
	ev.u.capture_buffer.index = (uint32_t)i;
	ev.u.capture_buffer.generation = s->generation;
	ev.u.capture_buffer.width = s->width;
	ev.u.capture_buffer.height = s->height;
	ev.u.capture_buffer.stride = s->width * 4;
	ev.u.capture_buffer.format = WS_FORMAT_XRGB8888;
	ev.u.capture_buffer.fd = -1;
	client_send(srv, s->owner, &ev, s->buf[i].fd);
	s->buf[i].sent = true;
}

static void
send_frame(struct ws_server *srv, struct ws_capture *s, int i)
{
	pixman_region32_t out;
	pixman_box32_t *box;
	ws_event_t ev;
	int j, n;

	event_init(srv, &ev, WS_EVENT_CAPTURE_FRAME,
	    sizeof(ev.u.capture_frame), s->window);
	ev.u.capture_frame.stream = s->id;
	ev.u.capture_frame.index = (uint32_t)i;
	ev.u.capture_frame.generation = s->generation;
	ev.u.capture_frame.seq = ++s->seq;
	ev.u.capture_frame.flags = s->full ? WS_CAPTURE_FRAME_FULL : 0;
	pixman_region32_intersect_rect(&s->damage, &s->damage, 0, 0,
	    s->src_width, s->src_height);
	scale_region(s, &s->damage, &out);
	box = pixman_region32_rectangles(&out, &n);
	if (n > WS_CAPTURE_MAX_DAMAGE) {
		box = pixman_region32_extents(&out);
		n = 1;
	}
	for (j = 0; j < n; j++) {
		ev.u.capture_frame.damage[j].x = box[j].x1;
		ev.u.capture_frame.damage[j].y = box[j].y1;
		ev.u.capture_frame.damage[j].width = box[j].x2 - box[j].x1;
		ev.u.capture_frame.damage[j].height = box[j].y2 - box[j].y1;
	}
	ev.u.capture_frame.ndamage = (uint32_t)n;
	pixman_region32_fini(&out);
	client_send(srv, s->owner, &ev, -1);
}

/* Fill buffer i of s with what changed since it was filled last. */
static void
capture_fill(struct ws_server *srv, struct ws_capture *s, int i)
{
	struct ws_capture_buf *b = &s->buf[i];
	struct ws_window *w;
	pixman_region32_t region;
	ws_rect_t cur;
	bool drawn = draws_cursor(srv, s);

	pixman_region32_init(&region);
	pixman_region32_copy(&region, &b->stale);
	/* where this buffer shows the pointer we drew: draw over it */
	if (b->cursor.width > 0)
		pixman_region32_union_rect(&region, &region, b->cursor.x,
		    b->cursor.y, (unsigned)b->cursor.width,
		    (unsigned)b->cursor.height);
	cursor_in_source(srv, s, &cur);
	if (drawn && cur.width > 0)
		pixman_region32_union_rect(&region, &region, cur.x, cur.y,
		    (unsigned)cur.width, (unsigned)cur.height);
	pixman_region32_intersect_rect(&region, &region, 0, 0, s->src_width,
	    s->src_height);
	if (s->window == 0) {
		/* the shadow without the border, and without the pointer
		   unless the stream wants it (then it's in the shadow, or
		   we draw it below) */
		compositor_capture_overlays(srv, true,
		    (s->flags & WS_CAPTURE_CURSOR) == 0);
		if (srv->out != NULL)	/* a renderer that reads back (T8) */
			copy_region(s, b, srv->out, 0, 0, &region);
		compositor_capture_overlays(srv, false,
		    (s->flags & WS_CAPTURE_CURSOR) == 0);
	} else {
		w = window_lookup(srv, s->window);
		if (w != NULL && w->buffer != NULL) {
			pixman_region32_intersect_rect(&region, &region, 0, 0,
			    w->buffer->width, w->buffer->height);
			copy_region(s, b, w->buffer->image, 0, 0, &region);
		}
	}
	memset(&b->cursor, 0, sizeof(b->cursor));
	if (drawn && cur.width > 0) {
		draw_cursor(srv, s, b, &cur);
		b->cursor = cur;
	}
	pixman_region32_fini(&region);
	pixman_region32_clear(&b->stale);
}

static void
capture_flush_one(struct ws_server *srv, struct ws_capture *s,
    uint64_t now, int *wait_ms)
{
	uint32_t w, h;
	ws_rect_t cur;
	int i;

	if (!source_size(srv, s, &w, &h)) {
		/* a window without a frame yet; one that went away was
		   ended by capture_window_gone */
		return;
	}
	if (s->window == 0 && !srv->display_active)
		return;
	if (w != s->src_width || h != s->src_height) {
		if (capture_alloc(s, w, h) < 0) {
			ws_warn("capture: no memory for stream %u's %ux%u "
			    "frames", s->id, s->req_width ? s->req_width : w,
			    s->req_height ? s->req_height : h);
			capture_end(srv, s, WS_CAPTURE_STOPPED, ENOMEM);
			return;
		}
	}
	if (s->flags & WS_CAPTURE_CURSOR) {
		cursor_in_source(srv, s, &cur);
		if (memcmp(&cur, &s->cursor_rect, sizeof(cur)) != 0 ||
		    (cur.width > 0 && s->cursor_image != srv->cursor_image)) {
			capture_damage_rect(s, &s->cursor_rect);
			capture_damage_rect(s, &cur);
			s->cursor_rect = cur;
			s->cursor_image = srv->cursor_image;
		}
	}
	pixman_region32_intersect_rect(&s->damage, &s->damage, 0, 0,
	    s->src_width, s->src_height);
	if (!pixman_region32_not_empty(&s->damage))
		return;
	if (s->interval_usec != 0 && s->last_usec != 0 &&
	    now - s->last_usec < s->interval_usec) {
		int ms = (int)((s->interval_usec - (now - s->last_usec) +
		    999) / 1000);

		if (*wait_ms < 0 || ms < *wait_ms)
			*wait_ms = ms;
		return;
	}
	for (i = 0; i < WS_CAPTURE_BUFFERS; i++)
		if (s->buf[i].image != NULL && !s->buf[i].busy)
			break;
	if (i == WS_CAPTURE_BUFFERS)
		return;			/* waiting for a release */
	capture_fill(srv, s, i);
	if (!s->buf[i].sent)
		send_buffer(srv, s, i);
	send_frame(srv, s, i);
	s->buf[i].busy = true;
	pixman_region32_clear(&s->damage);
	s->full = false;
	s->last_usec = now;
	if (s->flags & WS_CAPTURE_ONESHOT)
		capture_end(srv, s, WS_CAPTURE_STOPPED, 0);
}

/* Send the frames that are due; called after every repaint. */
void
capture_flush(struct ws_server *srv)
{
	struct ws_capture *s, *next;
	uint64_t now = ws_now_usec();
	int wait_ms = -1;

	for (s = srv->captures; s != NULL; s = next) {
		next = s->next;
		if (s->state == WS_CAPTURE_RUNNING && !s->owner->dead &&
		    !s->owner->doomed)
			capture_flush_one(srv, s, now, &wait_ms);
	}
	if (wait_ms >= 0)
		ws_timer_set(srv, TIMER_CAPTURE, wait_ms, true);
	if (srv->capture_activity_dirty) {
		srv->capture_activity_dirty = false;
		if (srv->capture_agent != NULL) {
			ws_event_t ev;
			uint32_t running, pending;

			capture_counts(srv, &running, &pending);
			event_init(srv, &ev, WS_EVENT_CAPTURE_ACTIVITY,
			    sizeof(ev.u.capture_activity), 0);
			ev.u.capture_activity.running = running;
			ev.u.capture_activity.pending = pending;
			client_send(srv, srv->capture_agent, &ev, -1);
		}
	}
}

/* What the compositor drew (screen coordinates), after a repaint. */
void
capture_screen_damage(struct ws_server *srv, const pixman_region32_t *region)
{
	struct ws_capture *s;

	for (s = srv->captures; s != NULL; s = s->next)
		if (s->window == 0 && s->state == WS_CAPTURE_RUNNING &&
		    s->src_width != 0)
			capture_damage(s, region);
}

/* Window w committed `damage` (window-local), NULL for all of it. */
void
capture_window_committed(struct ws_server *srv, struct ws_window *w,
    const pixman_region32_t *damage)
{
	struct ws_capture *s;

	for (s = srv->captures; s != NULL; s = s->next) {
		if (s->window != w->id || s->state != WS_CAPTURE_RUNNING ||
		    s->src_width == 0)
			continue;
		if (damage == NULL) {
			capture_damage_all(s);
		} else if (win_scale(w) == 1) {
			capture_damage(s, damage);
		} else {
			/* window-local points to buffer pixels */
			pixman_region32_t px;
			pixman_box32_t *b;
			int i, n, k = (int)win_scale(w);

			pixman_region32_init(&px);
			b = pixman_region32_rectangles(
			    (pixman_region32_t *)(uintptr_t)damage, &n);
			for (i = 0; i < n; i++)
				pixman_region32_union_rect(&px, &px,
				    b[i].x1 * k, b[i].y1 * k,
				    (unsigned)((b[i].x2 - b[i].x1) * k),
				    (unsigned)((b[i].y2 - b[i].y1) * k));
			capture_damage(s, &px);
			pixman_region32_fini(&px);
		}
	}
}

void
capture_window_gone(struct ws_server *srv, struct ws_window *w)
{
	struct ws_capture *s, *next;

	for (s = srv->captures; s != NULL; s = next) {
		next = s->next;
		if (s->window == w->id)
			capture_end(srv, s, s->state == WS_CAPTURE_PENDING ?
			    WS_CAPTURE_DENIED : WS_CAPTURE_STOPPED, ENOENT);
	}
}

/* ---- consent ---- */

static void
ask_agent(struct ws_server *srv, struct ws_capture *s)
{
	ws_event_t ev;

	if (srv->capture_agent == NULL || srv->capture_agent->sock < 0)
		return;
	event_init(srv, &ev, WS_EVENT_CAPTURE_CONSENT,
	    sizeof(ev.u.capture_consent), 0);
	ev.u.capture_consent.request = s->request;
	ev.u.capture_consent.pid = (int32_t)s->pid;
	ev.u.capture_consent.uid = (uint32_t)s->uid;
	ev.u.capture_consent.window = s->window;
	ev.u.capture_consent.flags = (s->window != 0 ? WS_CONSENT_WINDOW : 0) |
	    (s->on_behalf ? WS_CONSENT_ON_BEHALF : 0);
	ev.u.capture_consent.helper_pid = (int32_t)s->helper_pid;
	strlcpy(ev.u.capture_consent.name, s->name,
	    sizeof(ev.u.capture_consent.name));
	strlcpy(ev.u.capture_consent.helper_name, s->helper_name,
	    sizeof(ev.u.capture_consent.helper_name));
	strlcpy(ev.u.capture_consent.key, s->key,
	    sizeof(ev.u.capture_consent.key));
	client_send(srv, srv->capture_agent, &ev, -1);
	s->asked = true;
	ws_debug("capture: asked the consent agent about stream %u (%s, %s)",
	    s->id, s->name, s->key);
}

static void
capture_run(struct ws_server *srv, struct ws_capture *s)
{
	s->state = WS_CAPTURE_RUNNING;
	s->src_width = s->src_height = 0;	/* buffers at the next flush */
	send_state(srv, s, 0);
	capture_changed(srv);
}

/* May a process running as uid capture (with consent)? */
static bool
may_ask(struct ws_server *srv, uid_t uid)
{
	return srv->headless || dock_may_list(srv, uid);
}

/*
 * WS_MSG_CAPTURE_START: make the stream. Returns 0 with *out and the
 * reply's flags, or an errno value. request.c answers, then calls
 * capture_begin(), so the stream's events follow the reply.
 */
int
capture_start(struct ws_server *srv, struct ws_client *c,
    ws_req_capture_t *m, struct ws_capture **out, uint32_t *reply_flags)
{
	struct ws_window *w = NULL;
	struct ws_capture *s;
	char path[1024];
	uid_t uid = c->uid;
	pid_t pid = c->pid;
	bool root, own = false;
	int err = 0, i;

	m->for_name[sizeof(m->for_name) - 1] = '\0';
	if (c->flags & WS_CONNECT_CONSENT)
		err = EPERM;
	else if (m->width > WS_CAPTURE_MAX_SIZE ||
	    m->height > WS_CAPTURE_MAX_SIZE ||
	    (m->width == 0) != (m->height == 0) ||
	    m->max_fps > WS_CAPTURE_MAX_FPS ||
	    (m->flags & ~(uint32_t)(WS_CAPTURE_CURSOR | WS_CAPTURE_ONESHOT)) != 0)
		err = EINVAL;
	else if (m->hdr.window != 0 &&
	    ((w = window_lookup(srv, m->hdr.window)) == NULL ||
	    w->tile_for != NULL))
		err = ENOENT;
	if (err == 0 && m->for_pid != 0) {
		/* a helper: only a joined connection, for its user's
		   processes */
		pid = (pid_t)m->for_pid;
		if (c->app == c || proc_euid(pid, &uid) < 0 ||
		    (uid != c->uid && c->uid != 0))
			err = EPERM;
	}
	root = c->uid == 0 && (m->for_pid == 0 || uid == 0);
	if (err == 0 && w != NULL && m->for_pid == 0 &&
	    w->owner->app == c->app)
		own = true;
	if (err == 0 && !root && !own &&
	    !(srv->run_uid != 0 && c->uid == srv->run_uid) &&
	    !may_ask(srv, uid))
		err = EPERM;
	if (err != 0) {
		ws_info("capture: refused %s (pid %d)%s: %s", c->app_name,
		    (int)c->pid, m->for_pid != 0 ? " for another process" :
		    "", strerror(err));
		return err;
	}

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return ENOMEM;
	for (i = 0; i < WS_CAPTURE_BUFFERS; i++) {
		s->buf[i].fd = -1;
		pixman_region32_init(&s->buf[i].stale);
	}
	pixman_region32_init(&s->damage);
	s->id = ++srv->next_capture_id;
	if (s->id == 0)
		s->id = ++srv->next_capture_id;
	s->owner = c;
	s->window = m->hdr.window;
	s->flags = m->flags;
	s->req_width = m->width;
	s->req_height = m->height;
	s->interval_usec = m->max_fps != 0 ? 1000000 / m->max_fps : 0;
	s->pid = pid;
	s->uid = uid;
	s->on_behalf = m->for_pid != 0;
	s->helper_pid = s->on_behalf ? c->pid : 0;
	strlcpy(s->helper_name, c->app->app_name, sizeof(s->helper_name));
	if (proc_path(pid, path, sizeof(path)) == 0)
		make_key(path, s->key);
	else
		snprintf(s->key, sizeof(s->key), "app:%s", s->on_behalf &&
		    m->for_name[0] ? m->for_name : c->app->app_name);
	if (s->on_behalf && m->for_name[0] != '\0')
		strlcpy(s->name, m->for_name, sizeof(s->name));
	else if (!s->on_behalf && strcmp(c->app->app_name, "unnamed") != 0)
		strlcpy(s->name, c->app->app_name, sizeof(s->name));
	else
		strlcpy(s->name, strncmp(s->key, "app:", 4) == 0 ?
		    s->key + 4 : base_name(s->key), sizeof(s->name));
	s->exempt = root || own || (srv->run_uid != 0 &&
	    c->uid == srv->run_uid);
	s->indicator = !own;
	s->next = srv->captures;
	srv->captures = s;

	if (s->exempt) {
		*reply_flags = WS_CAPTURE_EXEMPT;
		if (!own)
			ws_info("capture: stream %u of the %s for %s (pid %d, "
			    "uid %d) is exempt from consent", s->id,
			    s->window ? "window" : "screen", s->name,
			    (int)s->pid, (int)s->uid);
	} else {
		s->state = WS_CAPTURE_PENDING;
		s->request = ++srv->next_consent_request;
		s->pending_usec = ws_now_usec();
		*reply_flags = WS_CAPTURE_ASKING;
		if (srv->capture_agent == NULL)
			*reply_flags |= WS_CAPTURE_NO_AGENT;
		ws_info("capture: stream %u of the %s for %s (pid %d%s%s) "
		    "waits for consent%s", s->id,
		    s->window ? "window" : "screen", s->name, (int)s->pid,
		    s->on_behalf ? ", through " : "",
		    s->on_behalf ? s->helper_name : "",
		    srv->capture_agent == NULL ? " (no agent yet)" : "");
	}
	*out = s;
	return 0;
}

/* After the reply: the stream's first state. */
void
capture_begin(struct ws_server *srv, struct ws_capture *s)
{
	if (s->exempt) {
		capture_run(srv, s);
	} else {
		send_state(srv, s, 0);
		ask_agent(srv, s);
		capture_changed(srv);
	}
}

/* WS_MSG_CAPTURE_STOP */
void
capture_stop_request(struct ws_server *srv, struct ws_client *c,
    uint32_t stream)
{
	struct ws_capture *s, *next;

	for (s = srv->captures; s != NULL; s = next) {
		next = s->next;
		if (s->owner == c && (stream == 0 || s->id == stream))
			capture_end(srv, s, WS_CAPTURE_STOPPED, 0);
	}
}

/* WS_MSG_CAPTURE_RELEASE */
void
capture_release(struct ws_server *srv, struct ws_client *c,
    const ws_req_capture_release_t *m)
{
	struct ws_capture *s = capture_lookup(srv, m->stream);

	(void)srv;
	if (s == NULL || s->owner != c || m->index >= WS_CAPTURE_BUFFERS ||
	    m->generation != s->generation)
		return;		/* an old generation's: already gone */
	s->buf[m->index].busy = false;
}

/* WS_MSG_CAPTURE_CONSENT, from the consent agent */
void
capture_consent(struct ws_server *srv, struct ws_client *c,
    const ws_req_capture_consent_t *m)
{
	struct ws_capture *s;

	if (c != srv->capture_agent) {
		client_error(srv, c, WS_MSG_CAPTURE_CONSENT, 0, EPERM,
		    "not the consent agent");
		return;
	}
	for (s = srv->captures; s != NULL; s = s->next)
		if (s->state == WS_CAPTURE_PENDING && s->request == m->request)
			break;
	if (s == NULL)
		return;		/* it went away meanwhile */
	if (m->decision == WS_CONSENT_ALLOW) {
		ws_info("capture: the user allowed %s (%s): stream %u runs",
		    s->name, s->key, s->id);
		capture_run(srv, s);
	} else {
		capture_end(srv, s, WS_CAPTURE_DENIED, EPERM);
	}
}

/* WS_MSG_CAPTURE_LIST */
void
capture_list(struct ws_server *srv, struct ws_client *c)
{
	struct ws_capture *s;
	ws_event_t ev;

	for (s = srv->captures; s != NULL; s = s->next) {
		event_init(srv, &ev, WS_EVENT_CAPTURE_INFO,
		    sizeof(ev.u.capture_info), s->window);
		ev.u.capture_info.stream = s->id;
		ev.u.capture_info.client = s->owner->id;
		ev.u.capture_info.pid = (int32_t)s->pid;
		ev.u.capture_info.window = s->window;
		ev.u.capture_info.state = s->state;
		ev.u.capture_info.flags = (s->exempt ? WS_CAPTURE_INFO_EXEMPT :
		    0) | (s->on_behalf ? WS_CAPTURE_INFO_ON_BEHALF : 0) |
		    (s->indicator && (s->flags & WS_CAPTURE_ONESHOT) == 0 ?
		    WS_CAPTURE_INFO_INDICATOR : 0);
		ev.u.capture_info.width = s->width;
		ev.u.capture_info.height = s->height;
		ev.u.capture_info.frames = s->seq;
		ev.u.capture_info.capture_flags = s->flags;
		strlcpy(ev.u.capture_info.name, s->name,
		    sizeof(ev.u.capture_info.name));
		strlcpy(ev.u.capture_info.key, s->key,
		    sizeof(ev.u.capture_info.key));
		client_send(srv, c, &ev, -1);
	}
	event_init(srv, &ev, WS_EVENT_LIST_END, 0, 0);
	client_send(srv, c, &ev, -1);
}

static void
send_revoked(struct ws_server *srv, uint32_t flags, const char *key)
{
	ws_event_t ev;

	event_init(srv, &ev, WS_EVENT_CAPTURE_REVOKED,
	    sizeof(ev.u.capture_revoked), 0);
	ev.u.capture_revoked.flags = flags;
	strlcpy(ev.u.capture_revoked.key, key, sizeof(ev.u.capture_revoked.key));
	client_send(srv, srv->capture_agent, &ev, -1);
}

/* WS_MSG_CAPTURE_REVOKE */
void
capture_revoke(struct ws_server *srv, struct ws_client *c,
    const ws_req_capture_revoke_t *m)
{
	struct ws_capture *s, *next;
	char key[WS_CAPTURE_KEY_MAX];
	bool all = (m->flags & WS_CAPTURE_REVOKE_ALL) != 0;
	unsigned n = 0;

	memcpy(key, m->key, sizeof(key));
	key[sizeof(key) - 1] = '\0';
	if (m->stream == 0 && !all && key[0] == '\0') {
		client_error(srv, c, WS_MSG_CAPTURE_REVOKE, 0, EINVAL,
		    "no stream or key");
		return;
	}
	for (s = srv->captures; s != NULL; s = next) {
		next = s->next;
		if (m->stream != 0 ? s->id == m->stream :
		    (all || strcmp(s->key, key) == 0)) {
			if (m->stream == 0 && s->exempt && !all)
				continue;
			n++;
			capture_end(srv, s, s->state == WS_CAPTURE_PENDING ?
			    WS_CAPTURE_DENIED : WS_CAPTURE_STOPPED, ECANCELED);
		}
	}
	ws_info("capture: %s (pid %d) stopped %u stream(s)%s%s%s", c->app_name,
	    (int)c->pid, n, (m->flags & WS_CAPTURE_REVOKE_FORGET) ?
	    " and revoked " : "", (m->flags & WS_CAPTURE_REVOKE_FORGET) ?
	    (all ? "every app's permission" : key) : "", "");
	if ((m->flags & WS_CAPTURE_REVOKE_FORGET) == 0 || m->stream != 0 ||
	    c == srv->capture_agent)
		return;
	if (srv->capture_agent != NULL && srv->capture_agent->sock >= 0) {
		send_revoked(srv, m->flags, key);
	} else if (srv->nrevoked < WS_CAPTURE_MAX_REVOKED) {
		/* for the agent that connects next */
		srv->revoked_flags[srv->nrevoked] = m->flags;
		strlcpy(srv->revoked[srv->nrevoked], key, WS_CAPTURE_KEY_MAX);
		srv->nrevoked++;
	} else {
		client_error(srv, c, WS_MSG_CAPTURE_REVOKE, 0, EAGAIN,
		    "too many revocations waiting for the consent agent");
	}
}

/* ---- the consent agent ---- */

/* May a hello with WS_CONNECT_CONSENT from uid go ahead? 0 or errno. */
int
capture_agent_hello(struct ws_server *srv, uid_t uid)
{
	if (!dock_may_list(srv, uid))
		return EPERM;
	if (srv->capture_agent != NULL && !srv->capture_agent->dead &&
	    !srv->capture_agent->doomed)
		return EBUSY;
	return 0;
}

/* A connection's socket is up: the agent gets what waited for it. */
void
capture_client_ready(struct ws_server *srv, struct ws_client *c)
{
	struct ws_capture *s;
	unsigned i;

	if ((c->flags & WS_CONNECT_CONSENT) == 0)
		return;
	srv->capture_agent = c;
	ws_info("capture: consent agent %s (pid %d) connected", c->app_name,
	    (int)c->pid);
	for (i = 0; i < srv->nrevoked; i++)
		send_revoked(srv, srv->revoked_flags[i], srv->revoked[i]);
	srv->nrevoked = 0;
	/* oldest first */
	for (;;) {
		struct ws_capture *oldest = NULL;

		for (s = srv->captures; s != NULL; s = s->next)
			if (s->state == WS_CAPTURE_PENDING && !s->asked &&
			    (oldest == NULL || s->request < oldest->request))
				oldest = s;
		if (oldest == NULL)
			break;
		ask_agent(srv, oldest);
	}
	srv->capture_activity_dirty = true;
}

void
capture_client_gone(struct ws_server *srv, struct ws_client *c)
{
	struct ws_capture *s, *next;
	bool changed = false;

	if (srv->capture_agent == c) {
		srv->capture_agent = NULL;
		ws_info("capture: the consent agent went away");
		/* the next agent asks again */
		for (s = srv->captures; s != NULL; s = s->next)
			s->asked = false;
	}
	for (s = srv->captures; s != NULL; s = next) {
		next = s->next;
		if (s->owner != c)
			continue;
		ws_info("capture: stream %u for %s ends with its client",
		    s->id, s->name);
		{
			struct ws_capture **pp;

			for (pp = &srv->captures; *pp != NULL; pp = &(*pp)->next)
				if (*pp == s) {
					*pp = s->next;
					break;
				}
		}
		capture_free(s);
		changed = true;
	}
	if (changed)
		capture_changed(srv);
}

/* Every 2 s: nobody answers forever. */
void
capture_housekeeping(struct ws_server *srv)
{
	struct ws_capture *s, *next;
	uint64_t now = ws_now_usec();

	for (s = srv->captures; s != NULL; s = next) {
		next = s->next;
		if (s->state == WS_CAPTURE_PENDING &&
		    now - s->pending_usec >
		    (uint64_t)WS_CAPTURE_CONSENT_TIMEOUT * 1000000)
			capture_end(srv, s, WS_CAPTURE_DENIED, ETIMEDOUT);
	}
}

void
capture_fini(struct ws_server *srv)
{
	struct ws_capture *s;

	while ((s = srv->captures) != NULL) {
		srv->captures = s->next;
		capture_free(s);
	}
	ws_timer_cancel(srv, TIMER_CAPTURE);
}
