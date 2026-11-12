/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The Mach service. NextBSD's kqueue has no EVFILT_MACHPORT, so a
 * dedicated thread receives requests and hands them to the main loop
 * through a queue and a pipe. All window state is touched only by the
 * main thread.
 *
 * Registration: bootstrap_check_in() works both when launchd starts
 * the server for its MachServices entry and when the server is started
 * by hand. In the hand-started case launchd-842 creates the service on
 * the fly in the caller's own bootstrap (the per-user launchd of that
 * login), so only processes of the same user can find it. Starting the
 * server through `launchctl load` puts the name in the system bootstrap,
 * where every user's apps can see it (see ws-session).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/event.h>

#include <servers/bootstrap.h>

#include "server.h"

const char *bootstrap_strerror(kern_return_t);

static void *
mach_thread_main(void *arg)
{
	struct ws_server *srv = arg;
	struct ws_mach_item *item = NULL;
	mach_msg_audit_trailer_t *tr;
	mach_msg_return_t kr;
	bool was_empty;
	size_t off;

	for (;;) {
		if (item == NULL) {
			item = malloc(sizeof(*item));
			if (item == NULL) {
				sleep(1);
				continue;
			}
		}
		memset(&item->msg.head, 0, sizeof(item->msg.head));
		kr = mach_msg(&item->msg.head,
		    MACH_RCV_MSG |
		    MACH_RCV_TRAILER_TYPE(MACH_MSG_TRAILER_FORMAT_0) |
		    MACH_RCV_TRAILER_ELEMENTS(MACH_RCV_TRAILER_AUDIT),
		    0, sizeof(item->msg.buf), srv->service_port,
		    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (kr != MACH_MSG_SUCCESS) {
			if (kr == MACH_RCV_INVALID_NAME ||
			    kr == MACH_RCV_PORT_DIED) {
				ws_err("mach: service port gone (0x%x)", kr);
				break;
			}
			ws_warn("mach: receive failed (0x%x)", kr);
			continue;
		}
		item->size = item->msg.head.msgh_size;
		item->sender_pid = -1;
		item->sender_euid = (uid_t)-1;
		off = (item->size + 3) & ~(size_t)3;
		if (off + sizeof(mach_msg_audit_trailer_t) <= sizeof(item->msg.buf)) {
			tr = (mach_msg_audit_trailer_t *)(void *)
			    (item->msg.buf + off);
			if (tr->msgh_trailer_size >=
			    sizeof(mach_msg_audit_trailer_t)) {
				item->sender_pid = (pid_t)tr->msgh_audit.val[5];
				item->sender_euid = (uid_t)tr->msgh_audit.val[1];
			}
		}
		item->next = NULL;
		pthread_mutex_lock(&srv->mach_lock);
		was_empty = srv->mach_head == NULL;
		*srv->mach_tail = item;
		srv->mach_tail = &item->next;
		pthread_mutex_unlock(&srv->mach_lock);
		if (was_empty)
			(void)write(srv->mach_pipe[1], "m", 1);
		item = NULL;
	}
	free(item);
	return NULL;
}

int
mach_ipc_start(struct ws_server *srv)
{
	mach_port_limits_t limits;
	kern_return_t kr;
	int flags;

	srv->mach_head = NULL;
	srv->mach_tail = &srv->mach_head;
	pthread_mutex_init(&srv->mach_lock, NULL);
	if (pipe2(srv->mach_pipe, O_CLOEXEC) < 0) {
		ws_err("pipe: %s", strerror(errno));
		return -1;
	}
	flags = fcntl(srv->mach_pipe[0], F_GETFL);
	fcntl(srv->mach_pipe[0], F_SETFL, flags | O_NONBLOCK);

	kr = bootstrap_check_in(bootstrap_port, srv->cfg.service_name,
	    &srv->service_port);
	if (kr != KERN_SUCCESS) {
		ws_err("bootstrap_check_in(%s): %s (%d)", srv->cfg.service_name,
		    bootstrap_strerror(kr), kr);
		return -1;
	}
	ws_info("Mach service %s checked in", srv->cfg.service_name);

	/* Let bursts queue up in the kernel rather than block senders. */
	limits.mpl_qlimit = 1024;
	kr = mach_port_set_attributes(mach_task_self(), srv->service_port,
	    MACH_PORT_LIMITS_INFO, (mach_port_info_t)&limits,
	    MACH_PORT_LIMITS_INFO_COUNT);
	if (kr != KERN_SUCCESS)
		ws_debug("mach: could not raise queue limit (%d)", kr);

	if (pthread_create(&srv->mach_thread, NULL, mach_thread_main, srv) != 0) {
		ws_err("pthread_create failed");
		return -1;
	}
	ws_kq_add_fd(srv, srv->mach_pipe[0], EVFILT_READ,
	    (void *)(uintptr_t)SRC_MACH);
	return 0;
}

/* Main thread: handle everything the receive thread queued. */
void
mach_ipc_drain(struct ws_server *srv)
{
	struct ws_mach_item *list, *item;
	char buf[64];

	while (read(srv->mach_pipe[0], buf, sizeof(buf)) > 0)
		;
	pthread_mutex_lock(&srv->mach_lock);
	list = srv->mach_head;
	srv->mach_head = NULL;
	srv->mach_tail = &srv->mach_head;
	pthread_mutex_unlock(&srv->mach_lock);

	while ((item = list) != NULL) {
		list = item->next;
		request_handle(srv, item);
		free(item);
	}
}
