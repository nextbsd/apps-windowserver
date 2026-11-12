/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Logging to stderr. Under launchd, StandardErrorPath collects it.
 */

#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "server.h"

int ws_log_level = WS_LOG_INFO;

uint64_t
ws_now_usec(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

void
ws_log(int level, const char *fmt, ...)
{
	static const char *const tags[] = { "error", "warning", "info", "debug" };
	char stamp[32];
	struct tm tm;
	time_t now;
	va_list ap;

	if (level > ws_log_level)
		return;
	now = time(NULL);
	localtime_r(&now, &tm);
	strftime(stamp, sizeof(stamp), "%H:%M:%S", &tm);
	fprintf(stderr, "%s WindowServer[%d] %s: ", stamp, (int)getpid(),
	    tags[level]);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	fflush(stderr);
}
