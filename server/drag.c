/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Drag and drop between apps ("Drag and drop" in WSProtocol.h), the way
 * Darwin's window server mediates drags. The source app puts the data
 * on a named pasteboard (NSDragPboard in gpbs) and hands the drag to
 * WindowServer with WS_MSG_DRAG_BEGIN. From then on the server moves
 * the drag image with the pointer, above every window and the Dock,
 * finds the window under the pointer that takes drags
 * (WS_WINDOW_ACCEPTS_DRAGS), tells its owner the drag entered, moved,
 * left or was dropped (WS_EVENT_DRAG), shows the cursor for the
 * operation the owner says it would take (WS_MSG_DRAG_STATUS), and
 * tells the source how it ended (WS_EVENT_DRAG_END), after sliding the
 * image back if nobody took it.
 *
 * There is one drag at a time, like there is one pointer. While it
 * lasts, pointer motion goes to nobody; the release of the dragging
 * button still goes to the source's window, before the end event.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include "server.h"

#define SLIDE_USEC		200000	/* the image sliding back */
#define SLIDE_STEP_MS		16
#define DRAG_IMAGE_OPACITY	0.8	/* as Darwin's translucent drag images */

bool
drag_active(struct ws_server *srv)
{
	return srv->drag.session != 0;
}

/* ---- events ---- */

static void
send_drag(struct ws_server *srv, struct ws_window *w, uint32_t phase)
{
	struct ws_drag *d = &srv->drag;
	ws_event_t ev;

	if (w == NULL || w->owner == NULL || w->owner->dead)
		return;
	event_init(srv, &ev, WS_EVENT_DRAG, sizeof(ev.u.drag), w->id);
	ev.u.drag.session = d->session;
	ev.u.drag.phase = phase;
	ev.u.drag.x = srv->ptr_x - w->frame.x;
	ev.u.drag.y = srv->ptr_y - w->frame.y;
	ev.u.drag.screen_x = srv->ptr_x;
	ev.u.drag.screen_y = srv->ptr_y;
	ev.u.drag.operations = d->ops;
	ev.u.drag.modifiers = srv->modifiers;
	ev.u.drag.source_client = d->source != NULL ? d->source->id : 0;
	ev.u.drag.source_app = d->source != NULL ? d->source->app->id : 0;
	strlcpy(ev.u.drag.pasteboard, d->pasteboard,
	    sizeof(ev.u.drag.pasteboard));
	client_send(srv, w->owner, &ev, -1);
	if (phase == WS_DRAG_ENTER || phase == WS_DRAG_UPDATE)
		d->awaiting = true;
}

/* The source hears what is under the drag when that changes. */
static void
tell_source(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;
	uint32_t target = d->target != NULL ? d->target->id : 0;
	ws_event_t ev;

	if (d->source == NULL || d->source->dead)
		return;
	if (target == d->told_target && d->target_op == d->told_op &&
	    d->ops == d->told_ops)
		return;
	d->told_target = target;
	d->told_op = d->target_op;
	d->told_ops = d->ops;
	event_init(srv, &ev, WS_EVENT_DRAG_STATUS, sizeof(ev.u.drag_status),
	    0);
	ev.u.drag_status.session = d->session;
	ev.u.drag_status.target = target;
	ev.u.drag_status.target_app = d->target != NULL ?
	    d->target->owner->app->id : 0;
	ev.u.drag_status.operation = d->target_op;
	ev.u.drag_status.operations = d->ops;
	ev.u.drag_status.screen_x = srv->ptr_x;
	ev.u.drag_status.screen_y = srv->ptr_y;
	client_send(srv, d->source, &ev, -1);
}

/* ---- the image ---- */

static void
image_place(struct ws_server *srv, double x, double y)
{
	struct ws_drag *d = &srv->drag;

	damage_rect(srv, &d->image_rect);
	if (d->image == NULL) {
		memset(&d->image_rect, 0, sizeof(d->image_rect));
		return;
	}
	d->image_rect.x = (int32_t)lround(x);
	d->image_rect.y = (int32_t)lround(y);
	d->image_rect.width = pixman_image_get_width(d->image) /
	    (int32_t)d->image_scale;
	d->image_rect.height = pixman_image_get_height(d->image) /
	    (int32_t)d->image_scale;
	damage_rect(srv, &d->image_rect);
}

static void
image_follow(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;

	image_place(srv, floor(srv->ptr_x) - d->hot_x,
	    floor(srv->ptr_y) - d->hot_y);
}

/* Drawn above every window, the overview and the Dock, below the cursor. */
void
drag_composite(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;

	if (d->session == 0 || d->image == NULL || d->image_rect.width <= 0)
		return;
	shadow_image(srv, true, d->image, d->image_scale, DRAG_IMAGE_OPACITY,
	    d->image_rect.x, d->image_rect.y, d->image_rect.width,
	    d->image_rect.height);
}

/* A copy of the client's buffer, so the client may destroy it. */
static int
image_copy(struct ws_buffer *b, pixman_image_t **out)
{
	pixman_image_t *img, *src;
	int w = (int)b->width, h = (int)b->height;	/* all its pixels */

	*out = NULL;
	if (w <= 0 || h <= 0 || w / (int)b->scale > WS_MAX_DRAG_IMAGE ||
	    h / (int)b->scale > WS_MAX_DRAG_IMAGE)
		return EINVAL;
	img = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, NULL, 0);
	src = pixman_image_create_bits(b->format == WS_FORMAT_XRGB8888 ?
	    PIXMAN_x8r8g8b8 : PIXMAN_a8r8g8b8, (int)b->width, (int)b->height,
	    pixman_image_get_data(b->image), (int)b->stride);
	if (img == NULL || src == NULL) {
		if (img != NULL)
			pixman_image_unref(img);
		if (src != NULL)
			pixman_image_unref(src);
		return ENOMEM;
	}
	pixman_image_composite32(PIXMAN_OP_SRC, src, NULL, img, 0, 0, 0, 0,
	    0, 0, w, h);
	pixman_image_unref(src);
	*out = img;
	return 0;
}

/* ---- targets ---- */

/* The operations a drag offers window w: the modifiers narrow them. */
static uint32_t
ops_for(struct ws_server *srv, struct ws_window *w)
{
	struct ws_drag *d = &srv->drag;
	uint32_t ops, mods = srv->modifiers;

	if (w == NULL)
		return 0;
	ops = d->source != NULL && w->owner->app == d->source->app ?
	    d->ops_local : d->ops_external;
	if (d->flags & WS_DRAG_IGNORE_MODIFIERS)
		return ops;
	/* GSDragView's -_updateOperationMask: */
	if (mods & WS_MOD_CONTROL)
		ops &= WS_DRAG_OP_LINK;
	else if (mods & WS_MOD_ALT)
		ops &= WS_DRAG_OP_COPY;
	else if (mods & WS_MOD_COMMAND)
		ops &= WS_DRAG_OP_GENERIC;
	return ops;
}

/* The window under the pointer that takes drags, or NULL. */
static struct ws_window *
target_at(struct ws_server *srv)
{
	struct ws_window *w = window_at(srv, srv->ptr_x, srv->ptr_y);

	if (w == NULL || w->tile_for != NULL || !w->visible ||
	    (w->flags & WS_WINDOW_ACCEPTS_DRAGS) == 0 || w->owner == NULL ||
	    w->owner->dead || (w->owner->flags & WS_CONNECT_TOOL))
		return NULL;
	return w;
}

/*
 * Find the window under the drag again and tell it, and the one the
 * drag left. moved: the pointer moved (an UPDATE even if nothing else
 * changed).
 */
static void
retarget(struct ws_server *srv, bool moved)
{
	struct ws_drag *d = &srv->drag;
	struct ws_window *w = target_at(srv);
	uint32_t ops = ops_for(srv, w);

	if (w != d->target) {
		if (d->target != NULL)
			send_drag(srv, d->target, WS_DRAG_EXIT);
		d->target = w;
		d->target_op = 0;
		d->awaiting = false;
		d->ops = ops;
		if (w != NULL)
			send_drag(srv, w, WS_DRAG_ENTER);
	} else if (w != NULL && (moved || ops != d->ops)) {
		d->ops = ops;
		d->target_op &= ops;
		send_drag(srv, w, WS_DRAG_UPDATE);
	} else {
		d->ops = ops;
	}
	cursor_update(srv);
	tell_source(srv);
}

/* ---- the end ---- */

static void
session_clear(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;

	ws_timer_cancel(srv, TIMER_DRAG);
	damage_rect(srv, &d->image_rect);
	memset(&d->image_rect, 0, sizeof(d->image_rect));
	if (d->image != NULL)
		pixman_image_unref(d->image);
	d->image = NULL;
	d->session = 0;
	d->source = NULL;
	d->target = NULL;
	d->target_op = 0;
	d->awaiting = false;
	d->released = false;
	d->sliding = false;
	d->types_len = 0;
	d->ntypes = 0;
	cursor_update(srv);
	pointer_recheck(srv);
}

static void
send_end(struct ws_server *srv, uint32_t op, uint32_t flags,
    uint32_t target)
{
	struct ws_drag *d = &srv->drag;
	ws_event_t ev;

	ws_debug("drag %u ended: operation 0x%x flags 0x%x window %u",
	    d->session, op, flags, target);
	if (d->source == NULL || d->source->dead)
		return;
	event_init(srv, &ev, WS_EVENT_DRAG_END, sizeof(ev.u.drag_end), 0);
	ev.u.drag_end.session = d->session;
	ev.u.drag_end.operation = op;
	ev.u.drag_end.flags = flags;
	ev.u.drag_end.target = target;
	ev.u.drag_end.screen_x = srv->ptr_x;
	ev.u.drag_end.screen_y = srv->ptr_y;
	client_send(srv, d->source, &ev, -1);
}

/*
 * No window takes the drag: the window under it hears it left, the
 * image slides back if the source asked for that, then the source
 * hears it ended.
 */
static void
end_nothing(struct ws_server *srv, uint32_t flags)
{
	struct ws_drag *d = &srv->drag;

	if (d->target != NULL)
		send_drag(srv, d->target, WS_DRAG_EXIT);
	d->target = NULL;
	d->target_op = 0;
	d->awaiting = false;
	if ((d->flags & WS_DRAG_SLIDE_BACK) && d->image != NULL &&
	    !srv->cfg.reduce_motion && d->source != NULL &&
	    (d->image_rect.x != d->slide_x || d->image_rect.y != d->slide_y)) {
		d->sliding = true;
		d->end_flags = flags | WS_DRAG_END_SLID_BACK;
		d->slide_from_x = d->image_rect.x;
		d->slide_from_y = d->image_rect.y;
		d->slide_start = ws_now_usec();
		cursor_update(srv);
		ws_timer_set(srv, TIMER_DRAG, SLIDE_STEP_MS, false);
		return;
	}
	send_end(srv, WS_DRAG_OP_NONE, flags, 0);
	session_clear(srv);
}

/* The button is up and the target has answered (or never will). */
static void
decide(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;
	struct ws_window *w = d->target;
	uint32_t op = d->target_op & d->ops;

	ws_timer_cancel(srv, TIMER_DRAG);
	if (w != NULL && w->visible && op != WS_DRAG_OP_NONE) {
		/* The image goes before the drop, so the target redraws
		   under nothing of ours. */
		damage_rect(srv, &d->image_rect);
		memset(&d->image_rect, 0, sizeof(d->image_rect));
		send_drag(srv, w, WS_DRAG_DROP);
		d->awaiting = false;
		send_end(srv, op, WS_DRAG_END_DROPPED, w->id);
		session_clear(srv);
		return;
	}
	end_nothing(srv, 0);
}

static void
release(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;

	d->released = true;
	d->release_usec = ws_now_usec();
	if (d->target != NULL && d->awaiting) {
		/* Give the target a moment to answer the last ENTER or
		   UPDATE. */
		ws_timer_set(srv, TIMER_DRAG, WS_DRAG_STATUS_WAIT_MS, true);
		return;
	}
	decide(srv);
}

/* Escape, WS_MSG_DRAG_CANCEL, or the target went away during a drop. */
static void
cancel(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;

	if (d->session == 0 || d->sliding)
		return;
	ws_debug("drag %u cancelled", d->session);
	/* The button is still held: its release is nobody's. */
	if (!d->released && srv->buttons != 0)
		d->swallow_release = true;
	end_nothing(srv, WS_DRAG_END_CANCELLED);
}

void
drag_timer_fire(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;
	double t, x, y;

	if (d->session == 0) {
		ws_timer_cancel(srv, TIMER_DRAG);
		return;
	}
	if (d->sliding) {
		t = (double)(ws_now_usec() - d->slide_start) / SLIDE_USEC;
		if (t >= 1.0) {
			image_place(srv, d->slide_x, d->slide_y);
			send_end(srv, WS_DRAG_OP_NONE, d->end_flags, 0);
			session_clear(srv);
			return;
		}
		/* ease out */
		t = 1.0 - (1.0 - t) * (1.0 - t);
		x = d->slide_from_x + (d->slide_x - d->slide_from_x) * t;
		y = d->slide_from_y + (d->slide_y - d->slide_from_y) * t;
		image_place(srv, x, y);
		return;
	}
	if (d->released) {
		ws_debug("drag %u: window %u didn't answer in time", d->session,
		    d->target != NULL ? d->target->id : 0);
		decide(srv);
	}
}

/* ---- requests ---- */

/* The types a begin request names, comma-separated, for the state. */
static void
types_summary(const struct ws_drag *d, char *out, size_t len)
{
	size_t o = 0, i;

	out[0] = '\0';
	for (i = 0; i < d->types_len && o + 1 < len; i++) {
		char ch = d->types[i];

		if (ch == '\0') {
			if (i + 1 == d->types_len)
				break;
			ch = ',';
		}
		out[o++] = ch;
	}
	out[o] = '\0';
}

static void
reply_session(struct ws_mach_item *item, uint32_t seq, int status,
    uint32_t session)
{
	mach_msg_header_t *req = &item->msg.head;
	ws_reply_t r;
	mach_msg_return_t kr;

	if (req->msgh_remote_port == MACH_PORT_NULL)
		return;
	memset(&r, 0, sizeof(r));
	r.seq = seq;
	r.status = status;
	r.u.drag.session = session;
	r.head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(req->msgh_bits),
	    0);
	r.head.msgh_size = (mach_msg_size_t)sizeof(r);
	r.head.msgh_remote_port = req->msgh_remote_port;
	r.head.msgh_local_port = MACH_PORT_NULL;
	r.head.msgh_id = req->msgh_id + 100;
	r.version = WS_PROTOCOL_VERSION;
	kr = mach_msg(&r.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT,
	    r.head.msgh_size, 0, MACH_PORT_NULL, 100, MACH_PORT_NULL);
	if (kr != MACH_MSG_SUCCESS)
		mach_port_deallocate(mach_task_self(), req->msgh_remote_port);
	req->msgh_remote_port = MACH_PORT_NULL;
}

/* WS_MSG_DRAG_BEGIN: from the app the dragging button went down in. */
void
drag_begin(struct ws_server *srv, struct ws_client *c,
    struct ws_mach_item *item)
{
	const ws_req_drag_begin_t *m = (const void *)item->msg.buf;
	struct ws_drag *d = &srv->drag;
	struct ws_buffer *b = NULL;
	pixman_image_t *img = NULL;
	size_t tlen;
	bool late = false;
	int err;
	uint32_t i;

	if ((c->flags & WS_CONNECT_TOOL) || overview_active(srv)) {
		reply_session(item, m->hdr.seq, EPERM, 0);
		return;
	}
	if (d->session != 0) {
		reply_session(item, m->hdr.seq, EBUSY, 0);
		return;
	}
	if (srv->buttons != 0) {
		/* the button went down in one of the app's windows */
		if (srv->grab == NULL || srv->grab->tile_for != NULL ||
		    srv->grab->owner->app != c->app || srv->pgrab != NULL) {
			reply_session(item, m->hdr.seq, EPERM, 0);
			return;
		}
	} else if (d->late_app == c->app->id &&
	    ws_now_usec() - d->late_usec < WS_DRAG_LATE_USEC) {
		/* a quick flick: dropped as soon as it starts */
		late = true;
	} else {
		reply_session(item, m->hdr.seq, EPERM, 0);
		return;
	}
	tlen = item->size - offsetof(ws_req_drag_begin_t, types);
	if (m->types_len < tlen)
		tlen = m->types_len;
	if (tlen > sizeof(d->types) || (tlen > 0 && m->types[tlen - 1] != '\0')) {
		reply_session(item, m->hdr.seq, EINVAL, 0);
		return;
	}
	if (m->buffer_id != 0) {
		b = client_buffer(c, m->buffer_id);
		if (b == NULL) {
			reply_session(item, m->hdr.seq, ENOENT, 0);
			return;
		}
		err = image_copy(b, &img);
		d->image_scale = b->scale;
		if (err != 0) {
			reply_session(item, m->hdr.seq, err, 0);
			return;
		}
	}

	memset(d->types, 0, sizeof(d->types));
	memcpy(d->types, m->types, tlen);
	d->types_len = (uint32_t)tlen;
	for (d->ntypes = 0, i = 0; i < tlen; i++)
		if (d->types[i] == '\0')
			d->ntypes++;
	memcpy(d->pasteboard, m->pasteboard, sizeof(d->pasteboard));
	d->pasteboard[sizeof(d->pasteboard) - 1] = '\0';
	if (++d->next_session == 0)
		d->next_session = 1;
	d->session = d->next_session;
	d->sessions++;
	d->source = c;
	d->source_window = m->hdr.window;
	d->button = WS_BUTTON_LEFT;
	for (i = 0; i < 32; i++)
		if (srv->buttons & (1u << i)) {
			d->button = i;
			break;
		}
	d->ops_local = m->ops_local;
	d->ops_external = m->ops_external;
	d->flags = m->flags;
	d->image = img;
	d->hot_x = m->hot_x;
	d->hot_y = m->hot_y;
	d->slide_x = m->slide_x;
	d->slide_y = m->slide_y;
	d->target = NULL;
	d->target_op = 0;
	d->awaiting = false;
	d->released = false;
	d->sliding = false;
	d->swallow_release = false;
	d->told_target = d->told_op = d->told_ops = (uint32_t)-1;
	memset(&d->image_rect, 0, sizeof(d->image_rect));
	reply_session(item, m->hdr.seq, 0, d->session);
	ws_debug("drag %u from client %u (%s): %u types on %s, ops 0x%x/0x%x%s",
	    d->session, c->id, c->app_name, d->ntypes, d->pasteboard,
	    d->ops_local, d->ops_external, late ? ", released already" : "");

	/* Nobody hovers while the drag lasts. */
	if (srv->hover != NULL && srv->hover != srv->grab) {
		ws_event_t ev;

		event_init(srv, &ev, WS_EVENT_MOUSE_EXITED,
		    sizeof(ev.u.pointer), srv->hover->id);
		ev.u.pointer.x = srv->ptr_x - srv->hover->frame.x;
		ev.u.pointer.y = srv->ptr_y - srv->hover->frame.y;
		ev.u.pointer.screen_x = srv->ptr_x;
		ev.u.pointer.screen_y = srv->ptr_y;
		ev.u.pointer.modifiers = srv->modifiers;
		client_send(srv, srv->hover->owner, &ev, -1);
		srv->hover = NULL;
	}
	image_follow(srv);
	retarget(srv, false);
	if (late)
		release(srv);
}

/* WS_MSG_DRAG_STATUS: what the window under the drag would take. */
void
drag_status(struct ws_server *srv, struct ws_client *c,
    const ws_req_drag_status_t *m)
{
	struct ws_drag *d = &srv->drag;

	if (d->session == 0 || m->session != d->session || d->sliding)
		return;
	if (m->flags & WS_DRAG_STATUS_FINISHED)
		return;
	if (d->target == NULL || d->target->id != m->hdr.window ||
	    d->target->owner != c)
		return;
	d->target_op = m->operation & d->ops;
	d->awaiting = false;
	if (d->released) {
		decide(srv);
		return;
	}
	cursor_update(srv);
	tell_source(srv);
}

/* WS_MSG_DRAG_CANCEL: the source gives up its drag. */
void
drag_cancel_request(struct ws_server *srv, struct ws_client *c,
    uint32_t session)
{
	struct ws_drag *d = &srv->drag;

	if (d->session == 0 || d->session != session || d->source != c) {
		client_error(srv, c, WS_MSG_DRAG_CANCEL, 0, ENOENT,
		    "no such drag");
		return;
	}
	cancel(srv);
}

/* WS_MSG_DRAG_QUERY (tools): the drag under way. */
void
drag_query(struct ws_server *srv, struct ws_client *c)
{
	struct ws_drag *d = &srv->drag;
	ws_event_t ev;

	event_init(srv, &ev, WS_EVENT_DRAG_STATE, sizeof(ev.u.drag_state), 0);
	ev.u.drag_state.sessions = d->sessions;
	if (d->session != 0) {
		ev.u.drag_state.session = d->session;
		ev.u.drag_state.source_client = d->source != NULL ?
		    d->source->id : 0;
		ev.u.drag_state.target = d->target != NULL ? d->target->id : 0;
		ev.u.drag_state.operation = d->target_op;
		ev.u.drag_state.operations = d->ops;
		ev.u.drag_state.flags = (d->released ? WS_DRAG_STATE_RELEASED :
		    0) | (d->sliding ? WS_DRAG_STATE_SLIDING : 0);
		ev.u.drag_state.image = d->image_rect;
		types_summary(d, ev.u.drag_state.types,
		    sizeof(ev.u.drag_state.types));
	}
	client_send(srv, c, &ev, -1);
}

/* ---- input ---- */

/* The pointer moved. Returns true if the drag took the motion. */
bool
drag_motion(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;

	if (d->session == 0)
		return false;
	if (d->sliding || d->released)
		return true;
	image_follow(srv);
	retarget(srv, true);
	return true;
}

/*
 * A button went down or up. Returns true if the drag took it: presses
 * while a drag lasts do nothing, and the release of a cancelled drag's
 * button goes to nobody. The release of the dragging button goes on to
 * the source's window as usual, then ends the drag (drag_note_release).
 */
bool
drag_button(struct ws_server *srv, uint32_t button, bool pressed)
{
	struct ws_drag *d = &srv->drag;

	if (d->swallow_release && !pressed &&
	    (srv->buttons & (1u << button)) != 0) {
		srv->buttons &= ~(1u << button);
		if (srv->buttons == 0) {
			d->swallow_release = false;
			srv->grab = NULL;
			srv->grab_suppressed = false;
			srv->tile_dragging = false;
			pointer_recheck(srv);
		}
		return true;
	}
	return d->session != 0 && pressed;
}

/* After pointer.c delivered the release of a button. */
void
drag_note_release(struct ws_server *srv, struct ws_window *w)
{
	struct ws_drag *d = &srv->drag;

	if (d->session != 0 && !d->released && !d->sliding) {
		if (srv->buttons == 0)
			release(srv);
		return;
	}
	if (w != NULL && w->tile_for == NULL && w->owner != NULL) {
		d->late_app = w->owner->app->id;
		d->late_usec = ws_now_usec();
	}
}

/* Escape cancels the drag. Returns true if the key was the drag's. */
bool
drag_key(struct ws_server *srv, xkb_keysym_t sym)
{
	if (srv->drag.session == 0)
		return false;
	if (sym == XKB_KEY_Escape) {
		cancel(srv);
		return true;
	}
	return false;
}

/* Control, Option or Command changed: the operations offered may have. */
void
drag_modifiers_changed(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;

	if (d->session == 0 || d->sliding || d->released)
		return;
	retarget(srv, false);
}

/*
 * The cursor while a drag lasts, or -1: the copy or link cursor for
 * the operation the window under it takes, the arrow otherwise
 * (nothing takes it, or a move), as GSDragView's -_setCursor picks.
 */
int
drag_cursor(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;

	if (d->session == 0)
		return -1;
	if (d->sliding || d->target == NULL)
		return WS_CURSOR_ARROW;
	switch (d->target_op & d->ops) {
	case WS_DRAG_OP_COPY:
	case WS_DRAG_OP_GENERIC:
		return WS_CURSOR_DRAG_COPY;
	case WS_DRAG_OP_LINK:
		return WS_CURSOR_DRAG_LINK;
	default:
		return WS_CURSOR_ARROW;
	}
}

/* ---- things going away ---- */

void
drag_window_gone(struct ws_server *srv, struct ws_window *w)
{
	struct ws_drag *d = &srv->drag;

	if (d->session == 0 || d->target != w)
		return;
	d->target = NULL;
	d->target_op = 0;
	d->awaiting = false;
	if (d->released) {
		decide(srv);
		return;
	}
	if (!d->sliding)
		retarget(srv, false);
}

void
drag_client_gone(struct ws_server *srv, struct ws_client *c)
{
	struct ws_drag *d = &srv->drag;

	if (d->session == 0)
		return;
	if (d->target != NULL && d->target->owner == c) {
		d->target = NULL;
		d->target_op = 0;
		d->awaiting = false;
		if (d->released) {
			decide(srv);
			return;
		}
	}
	if (d->source == c) {
		ws_debug("drag %u: the source went away", d->session);
		if (d->target != NULL)
			send_drag(srv, d->target, WS_DRAG_EXIT);
		d->source = NULL;
		if (!d->released && srv->buttons != 0)
			d->swallow_release = true;
		session_clear(srv);
	}
}

void
drag_fini(struct ws_server *srv)
{
	struct ws_drag *d = &srv->drag;

	if (d->image != NULL)
		pixman_image_unref(d->image);
	d->image = NULL;
	d->session = 0;
}
