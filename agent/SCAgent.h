/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ScreenCaptureAgent: WindowServer's Screen Recording consent agent
 * ("Screen capture" in WSProtocol.h).
 */
#import <AppKit/AppKit.h>

#include <WindowServer/WSClient.h>

/* Where the user's answers are kept: the org.nextbsd.ScreenCapture
   defaults domain, key Apps, a dictionary from an app's key (its
   executable's path) to { Allowed = YES|NO; Name; Date; }. */
#define SC_DOMAIN	@"org.nextbsd.ScreenCapture"
#define SC_APPS		@"Apps"

@interface SCAgent : NSObject
{
	ws_connection_t *conn;
	NSMutableArray *queue;		/* requests waiting for the prompt */
	NSDictionary *asking;		/* the request the prompt shows */
	NSPanel *prompt;
	NSPanel *indicator;
	NSTextField *indicatorText;
	NSMutableArray *recordingKeys;	/* keys the indicator's Stop stops */
	BOOL refreshQueued;
}
- (BOOL)connect;
- (void)applicationDidFinishLaunching: (NSNotification *)n;
@end

/* The answers stored, for --list. */
NSDictionary *SCStoredAnswers(void);
