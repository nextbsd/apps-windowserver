/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * WindowServer: start-up, the kqueue event loop and shutdown.
 *
 *   WindowServer [--headless WIDTHxHEIGHT] [options]
 *
 * The event loop is single-threaded. The only other thread receives
 * Mach messages and hands them over through a pipe (see mach.c).
 */

#include <sys/types.h>
#include <sys/event.h>

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

#define DEFAULT_CONFIG	"/System/Library/Preferences/org.nextbsd.WindowServer.plist"

static struct ws_server server;

void
ws_kq_add_fd(struct ws_server *srv, int fd, int filter, void *udata)
{
	struct kevent kev;

	EV_SET(&kev, (uintptr_t)fd, (short)filter, EV_ADD | EV_ENABLE, 0, 0,
	    udata);
	if (kevent(srv->kq, &kev, 1, NULL, 0, NULL) < 0)
		ws_warn("kevent add fd %d: %s", fd, strerror(errno));
}

void
ws_kq_del_fd(struct ws_server *srv, int fd, int filter)
{
	struct kevent kev;

	EV_SET(&kev, (uintptr_t)fd, (short)filter, EV_DELETE, 0, 0, NULL);
	(void)kevent(srv->kq, &kev, 1, NULL, 0, NULL);
}

void
ws_timer_set(struct ws_server *srv, int ident, int ms, bool oneshot)
{
	struct kevent kev;

	if (ms < 1)
		ms = 1;
	EV_SET(&kev, (uintptr_t)ident, EVFILT_TIMER,
	    EV_ADD | EV_ENABLE | (oneshot ? EV_ONESHOT : 0), 0, ms, NULL);
	if (kevent(srv->kq, &kev, 1, NULL, 0, NULL) < 0)
		ws_warn("kevent timer: %s", strerror(errno));
}

void
ws_timer_cancel(struct ws_server *srv, int ident)
{
	struct kevent kev;

	EV_SET(&kev, (uintptr_t)ident, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
	(void)kevent(srv->kq, &kev, 1, NULL, 0, NULL);
}

void
ws_quit(struct ws_server *srv)
{
	srv->running = false;
}

static void
usage(void)
{
	fprintf(stderr,
	    "usage: WindowServer [--headless WIDTHxHEIGHT] [--config FILE]\n"
	    "                    [--socket PATH] [--service NAME]\n"
	    "                    [--no-drop] [-v]\n");
	exit(2);
}

static void
handle_signal(struct ws_server *srv, int sig)
{
	switch (sig) {
	case SIGUSR1:
		if (snapshot_write_ppm(srv, srv->cfg.snapshot_path) == 0)
			ws_info("snapshot written to %s", srv->cfg.snapshot_path);
		else
			ws_warn("snapshot to %s failed: %s",
			    srv->cfg.snapshot_path, strerror(errno));
		break;
	case SIGUSR2:
		vt_signal(srv);
		break;
	default:
		ws_info("signal %d: shutting down", sig);
		ws_quit(srv);
		break;
	}
}

static void
handle_kevent(struct ws_server *srv, struct kevent *kev)
{
	uintptr_t tag = (uintptr_t)kev->udata;
	int type;

	if (kev->filter == EVFILT_SIGNAL) {
		handle_signal(srv, (int)kev->ident);
		return;
	}
	if (kev->filter == EVFILT_TIMER) {
		if (kev->ident == TIMER_REPEAT)
			keyboard_repeat_fire(srv);
		else if (kev->ident == TIMER_HOUSEKEEPING)
			clients_housekeeping(srv);
		return;
	}
	type = tag < 64 ? (int)tag : *(int *)kev->udata;
	switch (type) {
	case SRC_MACH:
		mach_ipc_drain(srv);
		break;
	case SRC_LISTEN:
		socket_accept(srv);
		break;
	case SRC_CONN:
		conn_readable(srv, kev->udata);
		break;
	case SRC_CLIENT: {
		struct ws_client *c = kev->udata;

		if (c->dead)
			break;
		if (kev->filter == EVFILT_WRITE)
			client_writable(srv, c);
		else
			client_readable(srv, c);
		break;
	}
	case SRC_INPUT:
		input_readable(srv);
		break;
	case SRC_DRM:
		drm_readable(srv);
		break;
	case SRC_TTY:
		vt_drain(srv);
		break;
	default:
		break;
	}
}

static int
parse_size(const char *s, uint32_t *w, uint32_t *h)
{
	unsigned a, b;

	if (sscanf(s, "%ux%u", &a, &b) != 2 || a < 64 || b < 64 ||
	    a > 16384 || b > 16384)
		return -1;
	*w = a;
	*h = b;
	return 0;
}

int
main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "headless", required_argument, NULL, 'H' },
		{ "config", required_argument, NULL, 'c' },
		{ "socket", required_argument, NULL, 's' },
		{ "service", required_argument, NULL, 'S' },
		{ "no-drop", no_argument, NULL, 'n' },
		{ "verbose", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	static const int sigs[] = { SIGINT, SIGTERM, SIGHUP, SIGUSR1, SIGUSR2 };
	struct ws_server *srv = &server;
	const char *config_path = DEFAULT_CONFIG;
	const char *opt_socket = NULL, *opt_service = NULL;
	struct kevent kev, events[64];
	bool no_drop = false;
	int ch, n, i, status = 1;
	size_t s;

	memset(srv, 0, sizeof(*srv));
	srv->listen_fd = -1;
	srv->tty_fd = -1;
	srv->next_client_id = 1;
	srv->next_window_id = 1;

	while ((ch = getopt_long(argc, argv, "vh", longopts, NULL)) != -1) {
		switch (ch) {
		case 'H':
			if (parse_size(optarg, &srv->width, &srv->height) < 0)
				usage();
			srv->headless = true;
			break;
		case 'c':
			config_path = optarg;
			break;
		case 's':
			opt_socket = optarg;
			break;
		case 'S':
			opt_service = optarg;
			break;
		case 'n':
			no_drop = true;
			break;
		case 'v':
			ws_log_level = WS_LOG_DEBUG;
			break;
		default:
			usage();
		}
	}

	config_defaults(&srv->cfg);
	if (config_load(&srv->cfg, config_path) < 0)
		ws_warn("can't read %s: %s", config_path, strerror(errno));
	config_env(&srv->cfg);
	if (opt_socket != NULL)
		strlcpy(srv->cfg.socket_path, opt_socket,
		    sizeof(srv->cfg.socket_path));
	if (opt_service != NULL)
		strlcpy(srv->cfg.service_name, opt_service,
		    sizeof(srv->cfg.service_name));

	srv->kq = kqueue();
	if (srv->kq < 0) {
		ws_err("kqueue: %s", strerror(errno));
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	for (s = 0; s < sizeof(sigs) / sizeof(sigs[0]); s++) {
		signal(sigs[s], SIG_IGN);	/* kqueue still sees them */
		EV_SET(&kev, (uintptr_t)sigs[s], EVFILT_SIGNAL, EV_ADD, 0, 0,
		    NULL);
		kevent(srv->kq, &kev, 1, NULL, 0, NULL);
	}

	ws_info("starting (%s mode)", srv->headless ? "headless" : "DRM");
	if (mach_ipc_start(srv) < 0 || socket_listen(srv) < 0)
		goto out;

	if (srv->headless) {
		srv->output = &headless_output_ops;
		srv->refresh_mhz = 60000;
		strlcpy(srv->screen_name, "headless", sizeof(srv->screen_name));
	} else {
		/* Devices first, while still root. */
		vt_install_crash_handlers(srv);
		if (vt_open(srv) < 0 || drm_open(srv) < 0)
			goto out;
		if (input_open(srv) < 0)
			ws_warn("no input devices; continuing without input");
	}
	if (keyboard_init(srv) < 0)
		goto out;
	if (!no_drop && privs_drop(srv) < 0)
		goto out;

	if (compositor_init(srv) < 0)
		goto out;
	srv->display_active = true;
	srv->ptr_x = srv->width / 2;
	srv->ptr_y = srv->height / 2;
	if (cursor_init(srv) < 0)
		goto out;
	if (!srv->headless && drm_start(srv) < 0)
		goto out;
	ws_timer_set(srv, TIMER_HOUSEKEEPING, 2000, false);
	ws_info("ready: %ux%u, service %s", srv->width, srv->height,
	    srv->cfg.service_name);

	srv->running = true;
	while (srv->running) {
		compositor_repaint(srv);
		n = kevent(srv->kq, NULL, 0, events,
		    (int)(sizeof(events) / sizeof(events[0])), NULL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			ws_err("kevent: %s", strerror(errno));
			break;
		}
		for (i = 0; i < n; i++)
			handle_kevent(srv, &events[i]);
		clients_reap(srv);
	}
	status = 0;

out:
	{
		struct ws_client *c;

		for (c = srv->clients; c != NULL; c = c->next)
			client_destroy(srv, c);
		clients_reap(srv);
	}
	cursor_fini(srv);
	if (srv->output != NULL && srv->output->shutdown != NULL)
		srv->output->shutdown(srv);
	vt_close(srv);
	input_close(srv);
	keyboard_fini(srv);
	compositor_fini(srv);
	socket_cleanup(srv);
	ws_info("exiting");
	return status;
}
