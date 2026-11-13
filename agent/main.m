/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ScreenCaptureAgent [-launchd]	be WindowServer's consent agent
 * ScreenCaptureAgent --list		print the answers the user gave
 *
 * Started on demand: by launchd through its LaunchAgent (-launchd: it
 * checks in the Mach service the client library sends a message to),
 * or by the client library itself when no LaunchAgent is loaded. It
 * exits when another agent is already connected, and when WindowServer
 * goes away.
 */

#import "SCAgent.h"

#include <signal.h>
#include <string.h>

#include "SCLaunchd.h"

int
main(int argc, const char *argv[])
{
	@autoreleasepool {
		SCAgent *agent;
		int i;

		signal(SIGPIPE, SIG_IGN);
		for (i = 1; i < argc; i++) {
			if (strcmp(argv[i], "--list") == 0) {
				NSDictionary *apps = SCStoredAnswers();

				for (NSString *k in [[apps allKeys]
				    sortedArrayUsingSelector:
				    @selector(compare:)]) {
					NSDictionary *a = [apps objectForKey: k];

					printf("%s %s \"%s\" %s\n",
					    [[a objectForKey: @"Allowed"]
					    boolValue] ? "allowed" : "refused",
					    [k UTF8String], [[a objectForKey:
					    @"Name"] UTF8String], [[a
					    objectForKey: @"Date"] UTF8String]);
				}
				return 0;
			}
			if (strcmp(argv[i], "-launchd") == 0)
				(void)SCLaunchdCheckIn();
		}
		/* an agent, not an app: no app icon */
		[[NSUserDefaults standardUserDefaults] registerDefaults:
		    @{ @"GSSuppressAppIcon": @YES }];
		[NSApplication sharedApplication];
		agent = [[SCAgent alloc] init];
		[NSApp setDelegate: (id)agent];
		[NSApp run];
	}
	return 0;
}
