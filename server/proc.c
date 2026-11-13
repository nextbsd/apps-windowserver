/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * What the kernel says about a process (capture.c names programs by
 * their executables). Apart from server.h: <sys/user.h> brings in
 * <vm/vm.h>, whose boolean_t isn't Mach's.
 */

#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/user.h>

#include <string.h>

int	proc_path(pid_t pid, char *buf, size_t len);
int	proc_euid(pid_t pid, uid_t *uid);

/* The executable of `pid`. 0, or -1 with errno set. */
int
proc_path(pid_t pid, char *buf, size_t len)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PATHNAME, (int)pid };
	size_t n = len;

	if (pid <= 0 || sysctl(mib, 4, buf, &n, NULL, 0) < 0 || n == 0 ||
	    buf[0] == '\0')
		return -1;
	buf[len - 1] = '\0';
	return 0;
}

/* The effective uid of `pid`. 0, or -1. */
int
proc_euid(pid_t pid, uid_t *uid)
{
	int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };
	struct kinfo_proc kp;
	size_t n = sizeof(kp);

	if (pid <= 0 || sysctl(mib, 4, &kp, &n, NULL, 0) < 0 ||
	    n < sizeof(kp))
		return -1;
	*uid = kp.ki_uid;
	return 0;
}
