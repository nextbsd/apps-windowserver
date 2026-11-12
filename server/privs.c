/*-
 * Copyright (c) 2026, Joseph Maloney
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Privilege drop. Started as root, WindowServer opens the DRM card,
 * the VT (through /dev/ttyv0) and the input devices first, then becomes
 * the _windowserver user in group video. The process keeps its pid,
 * so drm-kmod still lets it drop and set DRM master on VT switches.
 * Input devices that appear later need a devfs rule that lets the
 * _windowserver user open them (a NextBSD packaging task).
 */

#include <sys/types.h>

#include <errno.h>
#include <grp.h>
#include <pwd.h>
#include <string.h>
#include <unistd.h>

#include "server.h"

int
privs_drop(struct ws_server *srv)
{
	struct passwd *pw;
	struct group *gr;
	gid_t groups[2];
	int ngroups = 0;

	if (geteuid() != 0)
		return 0;
	pw = getpwnam(srv->cfg.user);
	if (pw == NULL) {
		ws_warn("user %s doesn't exist; staying root", srv->cfg.user);
		return 0;
	}
	gr = getgrnam(srv->cfg.group);
	if (gr != NULL)
		groups[ngroups++] = gr->gr_gid;
	else
		ws_warn("group %s doesn't exist", srv->cfg.group);
	if (ngroups == 0 || groups[0] != pw->pw_gid)
		groups[ngroups++] = pw->pw_gid;

	if (srv->socket_dir_created &&
	    chown(srv->socket_dir, pw->pw_uid, groups[0]) < 0)
		ws_warn("chown %s: %s", srv->socket_dir, strerror(errno));
	if (setgroups(ngroups, groups) < 0 || setgid(groups[0]) < 0 ||
	    setuid(pw->pw_uid) < 0) {
		ws_err("dropping to %s: %s", srv->cfg.user, strerror(errno));
		return -1;
	}
	if (setuid(0) == 0) {
		ws_err("privilege drop didn't stick");
		return -1;
	}
	srv->run_uid = pw->pw_uid;
	srv->run_gid = groups[0];
	ws_info("running as %s (uid %d, gid %d)", srv->cfg.user,
	    (int)pw->pw_uid, (int)groups[0]);
	return 0;
}
