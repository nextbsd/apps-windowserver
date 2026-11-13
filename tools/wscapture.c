/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * wscapture: a test client for screen capture ("Screen capture" in
 * WSProtocol.h). tests/capture-test.sh drives it.
 *
 *   wscapture stream [-w WINDOW] [-c] [-s WxH] [-f FPS] [-t SECONDS]
 *                    [-p X,Y]... [-o FILE.ppm] [-b|-B PID] [-n NAME]
 *	asks for a stream of the screen (or of WINDOW; -c with the pointer,
 *	-s at that size, -f at most FPS frames a second) and prints its
 *	events, one per line, with the colour at each X,Y of every frame.
 *	-o writes each frame to FILE.ppm. -b asks on behalf of process PID
 *	(named NAME), from a second connection that joined the first's
 *	app, as an X server does; -B the same from the app's own
 *	connection, which the server refuses. Ends after SECONDS (default
 *	30) or when the stream does.
 *   wscapture agent allow|deny|none [-t SECONDS]
 *	is a consent agent (WS_CONNECT_CONSENT) that answers every request
 *	as told (none: never), and prints what it hears.
 */

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <WindowServer/WindowServer.h>

#define MAX_PROBES	8

static int probes[MAX_PROBES][2];
static int nprobes;
static const char *outfile;
static uint32_t stream;
static int done;
static const char *policy;

static void
usage(void)
{
	fprintf(stderr,
	    "usage: wscapture stream [-w WINDOW] [-c] [-s WxH] [-f FPS] "
	    "[-t SECONDS]\n"
	    "                        [-p X,Y]... [-o FILE.ppm] [-b|-B PID] "
	    "[-n NAME]\n"
	    "       wscapture agent allow|deny|none [-t SECONDS]\n");
	exit(2);
}

static const char *
state_name(uint32_t state)
{
	switch (state) {
	case WS_CAPTURE_PENDING:
		return "pending";
	case WS_CAPTURE_RUNNING:
		return "running";
	case WS_CAPTURE_DENIED:
		return "denied";
	case WS_CAPTURE_STOPPED:
		return "stopped";
	default:
		return "unknown";
	}
}

static const char *
error_name(int e)
{
	switch (e) {
	case 0:
		return "0";
	case EPERM:
		return "EPERM";
	case ETIMEDOUT:
		return "ETIMEDOUT";
	case ECANCELED:
		return "ECANCELED";
	case ENOENT:
		return "ENOENT";
	case ENOMEM:
		return "ENOMEM";
	case EBUSY:
		return "EBUSY";
	default:
		return strerror(e);
	}
}

static void
write_ppm(const void *data, uint32_t stride, uint32_t w, uint32_t h)
{
	unsigned char *row;
	uint32_t x, y;
	FILE *f;

	f = fopen(outfile, "wb");
	if (f == NULL)
		return;
	row = malloc((size_t)w * 3);
	if (row == NULL) {
		fclose(f);
		return;
	}
	fprintf(f, "P6\n%u %u\n255\n", w, h);
	for (y = 0; y < h; y++) {
		const uint32_t *px = (const uint32_t *)(const void *)
		    ((const char *)data + (size_t)y * stride);

		for (x = 0; x < w; x++) {
			row[x * 3] = (unsigned char)(px[x] >> 16);
			row[x * 3 + 1] = (unsigned char)(px[x] >> 8);
			row[x * 3 + 2] = (unsigned char)px[x];
		}
		fwrite(row, 3, w, f);
	}
	free(row);
	fclose(f);
}

static uint32_t buf_w, buf_h;

static void
stream_event(ws_connection_t *c, const ws_event_t *ev, void *ctx)
{
	const ws_event_capture_frame_t *f;
	const void *data;
	uint32_t stride, i;
	char line[512];
	size_t n;

	(void)ctx;
	switch (ev->hdr.type) {
	case WS_EVENT_CAPTURE_STATE:
		printf("state stream=%u %s error=%s\n", ev->u.capture_state.stream,
		    state_name(ev->u.capture_state.state),
		    error_name(ev->u.capture_state.error));
		if (ev->u.capture_state.state == WS_CAPTURE_DENIED ||
		    ev->u.capture_state.state == WS_CAPTURE_STOPPED)
			done = 1;
		break;
	case WS_EVENT_CAPTURE_BUFFER:
		buf_w = ev->u.capture_buffer.width;
		buf_h = ev->u.capture_buffer.height;
		printf("buffer stream=%u index=%u gen=%u %ux%u stride=%u "
		    "mapped=%s\n", ev->u.capture_buffer.stream,
		    ev->u.capture_buffer.index, ev->u.capture_buffer.generation,
		    buf_w, buf_h, ev->u.capture_buffer.stride,
		    ev->u.capture_buffer.fd < 0 ? "yes" : "no");
		break;
	case WS_EVENT_CAPTURE_FRAME:
		f = &ev->u.capture_frame;
		n = (size_t)snprintf(line, sizeof(line),
		    "frame stream=%u seq=%u index=%u gen=%u%s damage=",
		    f->stream, f->seq, f->index, f->generation,
		    (f->flags & WS_CAPTURE_FRAME_FULL) ? " full" : "");
		for (i = 0; i < f->ndamage && n < sizeof(line); i++)
			n += (size_t)snprintf(line + n, sizeof(line) - n,
			    "%s%d,%d,%dx%d", i ? ";" : "", f->damage[i].x,
			    f->damage[i].y, f->damage[i].width,
			    f->damage[i].height);
		printf("%s\n", line);
		data = ws_capture_frame_data(c, f, &stride);
		if (data == NULL) {
			printf("frame seq=%u has no data\n", f->seq);
			break;
		}
		for (i = 0; i < (uint32_t)nprobes; i++) {
			uint32_t x = (uint32_t)probes[i][0];
			uint32_t y = (uint32_t)probes[i][1];
			const uint32_t *row;

			if (x >= buf_w || y >= buf_h) {
				printf("pixel seq=%u %u,%u outside\n", f->seq,
				    x, y);
				continue;
			}
			row = (const uint32_t *)(const void *)
			    ((const char *)data + (size_t)y * stride);
			printf("pixel seq=%u %u,%u %06x\n", f->seq, x, y,
			    row[x] & 0xffffff);
		}
		if (outfile != NULL)
			write_ppm(data, stride, buf_w, buf_h);
		ws_capture_release(c, f);
		break;
	case WS_EVENT_ERROR:
		printf("error request=0x%x code=%s %s\n", ev->u.error.request,
		    error_name(ev->u.error.code), ev->u.error.message);
		break;
	default:
		break;
	}
	fflush(stdout);
}

static void
agent_event(ws_connection_t *c, const ws_event_t *ev, void *ctx)
{
	const ws_event_capture_consent_t *q;

	(void)ctx;
	switch (ev->hdr.type) {
	case WS_EVENT_CAPTURE_CONSENT:
		q = &ev->u.capture_consent;
		printf("consent request=%u pid=%d uid=%u name=\"%s\" key=%s "
		    "window=%u%s%s helper=\"%s\" helper_pid=%d\n", q->request,
		    q->pid, q->uid, q->name, q->key, q->window,
		    (q->flags & WS_CONSENT_WINDOW) ? " window" : "",
		    (q->flags & WS_CONSENT_ON_BEHALF) ? " on-behalf" : "",
		    q->helper_name, q->helper_pid);
		if (strcmp(policy, "allow") == 0)
			ws_capture_consent(c, q->request, WS_CONSENT_ALLOW);
		else if (strcmp(policy, "deny") == 0)
			ws_capture_consent(c, q->request, WS_CONSENT_DENY);
		break;
	case WS_EVENT_CAPTURE_ACTIVITY:
		printf("activity running=%u pending=%u\n",
		    ev->u.capture_activity.running,
		    ev->u.capture_activity.pending);
		break;
	case WS_EVENT_CAPTURE_REVOKED:
		printf("revoked%s key=%s\n",
		    (ev->u.capture_revoked.flags & WS_CAPTURE_REVOKE_ALL) ?
		    " all" : "", ev->u.capture_revoked.key);
		break;
	default:
		break;
	}
	fflush(stdout);
}

static void
on_signal(int sig)
{
	(void)sig;
	done = 1;
}

/* Dispatch events until `done` or SECONDS pass. */
static void
run(ws_connection_t *c, ws_event_handler_t fn, int seconds)
{
	struct pollfd pfd;
	time_t end = time(NULL) + seconds;

	pfd.fd = ws_connection_fd(c);
	pfd.events = POLLIN;
	while (!done && time(NULL) < end) {
		if (ws_dispatch(c, fn, NULL) < 0) {
			printf("connection lost\n");
			break;
		}
		if (done)
			break;
		if (ws_pending(c))
			continue;
		(void)poll(&pfd, 1, 200);
	}
}

int
main(int argc, char **argv)
{
	ws_connection_t *c, *app = NULL;
	uint32_t window = 0, flags = 0, width = 0, height = 0, fps = 0;
	uint32_t rflags = 0;
	int seconds = 30, ch, pid = 0, join = 0;
	const char *name = NULL, *mode;

	if (argc < 2)
		usage();
	mode = argv[1];
	if (strcmp(mode, "agent") == 0) {
		if (argc < 3)
			usage();
		policy = argv[2];
		optind = 3;
	} else if (strcmp(mode, "stream") == 0) {
		optind = 2;
	} else {
		usage();
	}
	while ((ch = getopt(argc, argv, "w:cs:f:t:p:o:b:B:n:")) != -1) {
		switch (ch) {
		case 'w':
			window = (uint32_t)strtoul(optarg, NULL, 0);
			break;
		case 'c':
			flags |= WS_CAPTURE_CURSOR;
			break;
		case 's':
			if (sscanf(optarg, "%ux%u", &width, &height) != 2)
				usage();
			break;
		case 'f':
			fps = (uint32_t)atoi(optarg);
			break;
		case 't':
			seconds = atoi(optarg);
			break;
		case 'p':
			if (nprobes == MAX_PROBES || sscanf(optarg, "%d,%d",
			    &probes[nprobes][0], &probes[nprobes][1]) != 2)
				usage();
			nprobes++;
			break;
		case 'o':
			outfile = optarg;
			break;
		case 'b':
		case 'B':
			pid = atoi(optarg);
			join = ch == 'b';
			break;
		case 'n':
			name = optarg;
			break;
		default:
			usage();
		}
	}
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);
	setvbuf(stdout, NULL, _IOLBF, 0);

	if (policy != NULL) {
		c = ws_connect("wscapture-agent", WS_CONNECT_CONSENT);
		if (c == NULL) {
			printf("agent refused: %s\n", error_name(errno));
			return 1;
		}
		printf("agent ready client=%u policy=%s\n", ws_client_id(c),
		    policy);
		run(c, agent_event, seconds);
		ws_disconnect(c);
		return 0;
	}

	c = ws_connect("wscapture", 0);
	if (c == NULL) {
		printf("can't connect: %s\n", strerror(errno));
		return 1;
	}
	if (join) {
		/* a helper of this app, as an X server is XLibre.app's */
		app = c;
		c = ws_connect_join("wscapture-helper", 0, ws_client_id(app),
		    ws_client_token(app));
		if (c == NULL) {
			printf("can't join: %s\n", strerror(errno));
			return 1;
		}
	}
	stream = ws_capture_start_for(c, window, flags, width, height, fps,
	    pid, name, &rflags);
	if (stream == 0) {
		printf("start failed error=%s\n", error_name(errno));
		return 1;
	}
	printf("started stream=%u%s%s%s\n", stream,
	    (rflags & WS_CAPTURE_ASKING) ? " asking" : "",
	    (rflags & WS_CAPTURE_NO_AGENT) ? " no-agent" : "",
	    (rflags & WS_CAPTURE_EXEMPT) ? " exempt" : "");
	run(c, stream_event, seconds);
	if (!done)
		ws_capture_stop(c, stream);
	ws_disconnect(c);
	if (app != NULL)
		ws_disconnect(app);
	printf("done\n");
	return 0;
}
