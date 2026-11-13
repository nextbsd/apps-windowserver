/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ScreenCaptureAgent under launchd: its LaunchAgent declares the Mach
 * service WS_CAPTURE_AGENT_SERVICE, and the client library sends an
 * empty message to it when WindowServer has no consent agent to ask,
 * which makes launchd start the agent, as Darwin starts its agents on
 * demand. The agent checks the service in and throws those messages
 * away. Apart from the Objective-C files, whose headers don't mix with
 * Mach's.
 */

#include <pthread.h>
#include <string.h>

#include <mach/mach.h>
#include <servers/bootstrap.h>

#include <WindowServer/WSTypes.h>

#include "SCLaunchd.h"

static mach_port_t service = MACH_PORT_NULL;

static void *
drain(void *arg)
{
	union {
		mach_msg_header_t h;
		uint8_t		buf[1024];
	} m;

	(void)arg;
	for (;;) {
		memset(&m.h, 0, sizeof(m.h));
		/* (a message too large for the buffer is thrown away by
		   the kernel) */
		if (mach_msg(&m.h, MACH_RCV_MSG, 0, sizeof(m), service,
		    MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL) == MACH_MSG_SUCCESS)
			mach_msg_destroy(&m.h);
	}
	return NULL;
}

int
SCLaunchdCheckIn(void)
{
	pthread_t t;

	if (bootstrap_check_in(bootstrap_port, WS_CAPTURE_AGENT_SERVICE,
	    &service) != KERN_SUCCESS)
		return -1;
	if (pthread_create(&t, NULL, drain, NULL) != 0)
		return -1;
	pthread_detach(t);
	return 0;
}
