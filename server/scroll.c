/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Touchpad scrolling as on macOS: scroll phases and momentum.
 *
 * A two-finger scroll is one BEGAN event, CHANGED events while the
 * fingers move, and an ENDED event with no delta when they lift
 * (libinput's zero-delta "scroll stop"). All of a scroll goes to the
 * window it started over. If the fingers were still moving when they
 * lifted, momentum scrolling (MomentumScrolling) carries on from their
 * speed with friction: momentum BEGAN, CHANGED... and ENDED events,
 * every 16 ms, until it has slowed down (about 1.5 s from a fast
 * flick) or a finger touches the touchpad again: a new scroll, pointer
 * motion, a click or libinput's hold gesture (fingers resting).
 * Phases use the NSEventPhase values (WS_SCROLL_PHASE_*), so the
 * GNUstep backend passes them straight through.
 */

#include <math.h>
#include <string.h>

#include "server.h"

#define MOMENTUM_TICK_MS	16
#define MOMENTUM_TAU		0.325	/* s: speed falls to 1/e */
#define MOMENTUM_MIN_SPEED	150.0	/* px/s at lift-off to start */
#define MOMENTUM_STOP_SPEED	20.0	/* px/s: stop below this */
#define MOMENTUM_MAX_USEC	2500000	/* never longer than this */
#define LIFT_MAX_USEC		100000	/* fingers rested longer: no
					   momentum */
#define VELOCITY_WINDOW_USEC	100000	/* samples used for the speed */
#define SAMPLE_DT_MIN		0.004	/* s, clamps for one sample's */
#define SAMPLE_DT_MAX		0.050	/* time step */

static struct ws_window *
scroll_window(struct ws_server *srv)
{
	return window_lookup(srv, srv->scroll.window);
}

static void
momentum_stop(struct ws_server *srv, bool send)
{
	struct ws_scroll_state *s = &srv->scroll;

	if (!s->momentum)
		return;
	ws_timer_cancel(srv, TIMER_MOMENTUM);
	s->momentum = false;
	if (send && s->momentum_began)
		pointer_scroll_send(srv, scroll_window(srv), 0, 0, 0, 0,
		    WS_SCROLL_FINGER, WS_SCROLL_PHASE_NONE,
		    WS_SCROLL_PHASE_ENDED);
	s->momentum_began = false;
	ws_debug("momentum scrolling stopped");
}

/* A finger touched the touchpad again, or other input came. */
void
scroll_touch(struct ws_server *srv)
{
	momentum_stop(srv, true);
}

/* The fingers' speed when they lifted, from the last samples. */
static void
lift_velocity(struct ws_scroll_state *s, double *vx, double *vy)
{
	double sx = 0, sy = 0, st = 0;
	uint64_t last;
	unsigned i, k;

	*vx = *vy = 0;
	if (s->nsamples == 0)
		return;
	k = (s->next_sample + WS_SCROLL_SAMPLES - 1) % WS_SCROLL_SAMPLES;
	last = s->samples[k].usec;
	for (i = 0; i < s->nsamples; i++) {
		k = (s->next_sample + WS_SCROLL_SAMPLES - 1 - i) %
		    WS_SCROLL_SAMPLES;
		if (last - s->samples[k].usec > VELOCITY_WINDOW_USEC)
			break;
		sx += s->samples[k].dx;
		sy += s->samples[k].dy;
		st += s->samples[k].dt;
	}
	if (st > 0) {
		*vx = sx / st;
		*vy = sy / st;
	}
}

static void
momentum_start(struct ws_server *srv, uint64_t lift_usec)
{
	struct ws_scroll_state *s = &srv->scroll;
	double vx, vy;

	if (!srv->cfg.momentum_scrolling || s->nsamples == 0 ||
	    lift_usec - s->last_usec > LIFT_MAX_USEC)
		return;
	lift_velocity(s, &vx, &vy);
	if (hypot(vx, vy) < MOMENTUM_MIN_SPEED)
		return;
	s->vx = vx;
	s->vy = vy;
	s->momentum = true;
	s->momentum_began = false;
	s->momentum_start = ws_now_usec();
	s->last_usec = s->momentum_start - MOMENTUM_TICK_MS * 1000;
	ws_timer_set(srv, TIMER_MOMENTUM, MOMENTUM_TICK_MS, false);
	ws_debug("momentum scrolling from %.0f,%.0f px/s", vx, vy);
	/* The first step (momentum BEGAN) right away, so that a finger
	 * touching down at once still gets an ENDED. */
	scroll_momentum_fire(srv);
}

/*
 * A two-finger scroll from libinput (or wsctl): dx, dy in pixels, or
 * stop when the fingers lifted. usec is the event's time.
 */
void
scroll_finger(struct ws_server *srv, double dx, double dy, bool stop,
    uint64_t usec)
{
	struct ws_scroll_state *s = &srv->scroll;
	struct ws_window *w;
	uint32_t phase;
	double dt;

	if (stop) {
		if (!s->fingers)
			return;
		s->fingers = false;
		pointer_scroll_send(srv, scroll_window(srv), 0, 0, 0, 0,
		    WS_SCROLL_FINGER, WS_SCROLL_PHASE_ENDED,
		    WS_SCROLL_PHASE_NONE);
		if (scroll_window(srv) != NULL)
			momentum_start(srv, usec);
		return;
	}
	momentum_stop(srv, true);
	if (!s->fingers) {
		w = pointer_scroll_target(srv);
		s->fingers = true;
		s->window = w != NULL ? w->id : 0;
		s->nsamples = 0;
		s->next_sample = 0;
		dt = MOMENTUM_TICK_MS / 1000.0;
		phase = WS_SCROLL_PHASE_BEGAN;
	} else {
		w = scroll_window(srv);
		dt = (double)(int64_t)(usec - s->last_usec) / 1e6;
		phase = WS_SCROLL_PHASE_CHANGED;
	}
	if (dt < SAMPLE_DT_MIN)
		dt = SAMPLE_DT_MIN;
	if (dt > SAMPLE_DT_MAX)
		dt = SAMPLE_DT_MAX;
	s->samples[s->next_sample].usec = usec;
	s->samples[s->next_sample].dt = dt;
	s->samples[s->next_sample].dx = dx;
	s->samples[s->next_sample].dy = dy;
	s->next_sample = (s->next_sample + 1) % WS_SCROLL_SAMPLES;
	if (s->nsamples < WS_SCROLL_SAMPLES)
		s->nsamples++;
	s->last_usec = usec;
	pointer_scroll_send(srv, w, dx, dy, 0, 0, WS_SCROLL_FINGER, phase,
	    WS_SCROLL_PHASE_NONE);
}

/* TIMER_MOMENTUM: one step of momentum scrolling. */
void
scroll_momentum_fire(struct ws_server *srv)
{
	struct ws_scroll_state *s = &srv->scroll;
	struct ws_window *w;
	uint64_t now = ws_now_usec();
	double dt, decay;

	if (!s->momentum)
		return;
	w = scroll_window(srv);
	if (w == NULL) {
		momentum_stop(srv, false);
		return;
	}
	dt = (double)(now - s->last_usec) / 1e6;
	if (dt > SAMPLE_DT_MAX)
		dt = SAMPLE_DT_MAX;
	s->last_usec = now;
	decay = exp(-dt / MOMENTUM_TAU);
	s->vx *= decay;
	s->vy *= decay;
	if (hypot(s->vx, s->vy) < MOMENTUM_STOP_SPEED ||
	    now - s->momentum_start > MOMENTUM_MAX_USEC) {
		momentum_stop(srv, true);
		return;
	}
	pointer_scroll_send(srv, w, s->vx * dt, s->vy * dt, 0, 0,
	    WS_SCROLL_FINGER, WS_SCROLL_PHASE_NONE,
	    s->momentum_began ? WS_SCROLL_PHASE_CHANGED :
	    WS_SCROLL_PHASE_BEGAN);
	s->momentum_began = true;
}

/* w is going away: whatever scrolling it had ends without events. */
void
scroll_window_gone(struct ws_server *srv, struct ws_window *w)
{
	if (srv->scroll.window != w->id)
		return;
	momentum_stop(srv, false);
	srv->scroll.fingers = false;
	srv->scroll.window = 0;
}
