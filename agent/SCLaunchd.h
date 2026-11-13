/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 */
#ifndef SC_LAUNCHD_H
#define SC_LAUNCHD_H

/* Check in WS_CAPTURE_AGENT_SERVICE (started by launchd) and drain it. */
int	SCLaunchdCheckIn(void);

#endif
