/*-
 * Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * ScreenCaptureAgent: asks the user whether an app may record the
 * screen, as macOS's tccd and UserNotificationCenter do for Screen
 * Recording. WindowServer draws no text, so it hands each capture that
 * needs consent to this agent (a WS_CONNECT_CONSENT connection), which
 * answers from the user's stored answers, or shows a prompt and stores
 * the answer. While anything records, a small panel says who, with a
 * Stop button; WindowServer draws the orange border itself.
 *
 * The answers: the org.nextbsd.ScreenCapture defaults domain
 * (~/Library/Preferences/org.nextbsd.ScreenCapture.plist), Apps, by
 * the app's key (its executable's path). `wsctl capture revoke KEY`
 * reaches the agent through WindowServer and makes it forget one;
 * `defaults delete org.nextbsd.ScreenCapture` forgets them all.
 */

#import "SCAgent.h"

#include <errno.h>
#include <string.h>

NSDictionary *
SCStoredAnswers(void)
{
	NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
	NSDictionary *dom, *apps;

	[d synchronize];
	dom = [d persistentDomainForName: SC_DOMAIN];
	apps = [dom objectForKey: SC_APPS];
	return [apps isKindOfClass: [NSDictionary class]] ? apps : @{};
}

static void
storeAnswers(NSDictionary *apps)
{
	NSUserDefaults *d = [NSUserDefaults standardUserDefaults];
	NSMutableDictionary *dom;

	dom = [[d persistentDomainForName: SC_DOMAIN] mutableCopy];
	if (dom == nil)
		dom = [NSMutableDictionary dictionary];
	[dom setObject: apps forKey: SC_APPS];
	[d setPersistentDomain: dom forName: SC_DOMAIN];
	[d synchronize];
}

static NSTextField *
label(NSRect r, NSString *s, NSFont *font)
{
	NSTextField *t = [[NSTextField alloc] initWithFrame: r];

	[t setStringValue: s];
	[t setEditable: NO];
	[t setSelectable: NO];
	[t setBezeled: NO];
	[t setBordered: NO];
	[t setDrawsBackground: NO];
	[t setFont: font];
	return t;
}

@interface SCAgent () <RunLoopEvents>
@end

@implementation SCAgent

- (id)init
{
	if ((self = [super init]) != nil) {
		queue = [NSMutableArray array];
		recordingKeys = [NSMutableArray array];
	}
	return self;
}

/* Connect once the app is up: windows ordered in before that don't
   show. Another agent connected already, or no WindowServer: quit. */
- (void)applicationDidFinishLaunching: (NSNotification *)n
{
	if (![self connect])
		[NSApp terminate: nil];
}

- (BOOL)connect
{
	NSRunLoop *rl = [NSRunLoop currentRunLoop];
	NSArray *modes = @[NSDefaultRunLoopMode, NSModalPanelRunLoopMode,
	    NSEventTrackingRunLoopMode];
	int fd;

	conn = ws_connect("ScreenCaptureAgent", WS_CONNECT_CONSENT);
	if (conn == NULL) {
		/* EBUSY: another agent answers already */
		NSLog(@"ScreenCaptureAgent: can't be WindowServer's consent "
		    "agent: %s", strerror(errno));
		return NO;
	}
	fd = ws_connection_fd(conn);
	for (NSString *m in modes)
		[rl addEvent: (void *)(uintptr_t)fd type: ET_RDESC
		     watcher: self forMode: m];
	/* the hello may have queued events already */
	[self performSelector: @selector(dispatch) withObject: nil
		   afterDelay: 0];
	NSLog(@"ScreenCaptureAgent: WindowServer's consent agent (client %u)",
	    ws_client_id(conn));
	return YES;
}

/* ---- WindowServer's events ---- */

static void
handler(ws_connection_t *c, const ws_event_t *ev, void *ctx)
{
	SCAgent *self = (__bridge SCAgent *)ctx;

	(void)c;
	[self handleEvent: ev];
}

- (void)dispatch
{
	if (ws_dispatch(conn, handler, (__bridge void *)self) < 0 &&
	    errno != EAGAIN) {
		NSLog(@"ScreenCaptureAgent: WindowServer went away");
		[[NSRunLoop currentRunLoop] removeEvent:
		    (void *)(uintptr_t)ws_connection_fd(conn) type: ET_RDESC
		    forMode: NSDefaultRunLoopMode all: YES];
		[NSApp terminate: nil];
	}
}

- (void)receivedEvent: (void *)data type: (RunLoopEventType)type
		extra: (void *)extra forMode: (NSString *)mode
{
	[self dispatch];
}

- (void)handleEvent: (const ws_event_t *)ev
{
	switch (ev->hdr.type) {
	case WS_EVENT_CAPTURE_CONSENT:
		[self consentRequested: &ev->u.capture_consent];
		break;
	case WS_EVENT_CAPTURE_ACTIVITY:
		if (!refreshQueued) {
			refreshQueued = YES;
			[self performSelector: @selector(refreshIndicator)
				   withObject: nil afterDelay: 0];
		}
		break;
	case WS_EVENT_CAPTURE_REVOKED:
		[self forget: ev->u.capture_revoked.key
			 all: (ev->u.capture_revoked.flags &
			    WS_CAPTURE_REVOKE_ALL) != 0];
		break;
	default:
		break;
	}
}

/* ---- consent ---- */

- (void)answer: (NSDictionary *)req allow: (BOOL)allow
{
	ws_capture_consent(conn, [[req objectForKey: @"request"] unsignedIntValue],
	    allow ? WS_CONSENT_ALLOW : WS_CONSENT_DENY);
}

- (void)consentRequested: (const ws_event_capture_consent_t *)q
{
	NSString *key = [NSString stringWithUTF8String: q->key];
	NSDictionary *stored = [SCStoredAnswers() objectForKey: key];
	NSDictionary *req;

	req = @{
		@"request": @(q->request),
		@"key": key,
		@"name": [NSString stringWithUTF8String: q->name],
		@"helper": [NSString stringWithUTF8String: q->helper_name],
		@"onBehalf": @((q->flags & WS_CONSENT_ON_BEHALF) != 0),
		@"window": @((q->flags & WS_CONSENT_WINDOW) != 0),
	};
	if ([stored isKindOfClass: [NSDictionary class]] &&
	    [stored objectForKey: @"Allowed"] != nil) {
		BOOL allowed = [[stored objectForKey: @"Allowed"] boolValue];

		NSLog(@"ScreenCaptureAgent: %@ (%@): %@, as the user said "
		    "before", [req objectForKey: @"name"], key,
		    allowed ? @"allowed" : @"refused");
		[self answer: req allow: allowed];
		return;
	}
	/* the prompt already asks about this app: same answer */
	if (asking != nil && [[asking objectForKey: @"key"] isEqual: key]) {
		[queue addObject: req];
		return;
	}
	[queue addObject: req];
	if (asking == nil)
		[self askNext];
}

- (void)askNext
{
	NSDictionary *req;
	NSString *name, *title, *info;
	NSRect vis, fr;
	NSView *v;
	NSImageView *iv;
	NSButton *allow, *deny;

	if ([queue count] == 0) {
		asking = nil;
		return;
	}
	req = [queue objectAtIndex: 0];
	asking = req;
	name = [req objectForKey: @"name"];
	title = [NSString stringWithFormat: [[req objectForKey: @"window"]
	    boolValue] ? @"“%@” would like to record a window." :
	    @"“%@” would like to record this screen.", name];
	info = @"Screen recording lets it see everything on the screen, "
	    @"including other apps' windows, until you stop it or take the "
	    @"permission back (wsctl capture revoke).";
	if ([[req objectForKey: @"onBehalf"] boolValue])
		info = [NSString stringWithFormat: @"It asks through %@. %@",
		    [req objectForKey: @"helper"], info];

	fr = NSMakeRect(0, 0, 460, 190);
	if (prompt == nil) {
		prompt = [[NSPanel alloc] initWithContentRect: fr
		    styleMask: NSTitledWindowMask backing: NSBackingStoreBuffered
		    defer: NO];
		[prompt setTitle: @"Screen Recording"];
		[prompt setLevel: NSModalPanelWindowLevel];
		[prompt setHidesOnDeactivate: NO];
		[prompt setReleasedWhenClosed: NO];
	}
	v = [[NSView alloc] initWithFrame: fr];
	iv = [[NSImageView alloc] initWithFrame: NSMakeRect(20, 106, 64, 64)];
	[iv setImage: [NSApp applicationIconImage]];
	[v addSubview: iv];
	[v addSubview: label(NSMakeRect(100, 128, 340, 42), title,
	    [NSFont boldSystemFontOfSize: 13])];
	[v addSubview: label(NSMakeRect(100, 66, 340, 58), info,
	    [NSFont systemFontOfSize: 11])];
	[v addSubview: label(NSMakeRect(100, 46, 340, 16),
	    [req objectForKey: @"key"], [NSFont systemFontOfSize: 9])];
	deny = [[NSButton alloc] initWithFrame: NSMakeRect(230, 12, 104, 28)];
	[deny setTitle: @"Don't Allow"];
	[deny setTarget: self];
	[deny setAction: @selector(deny:)];
	[deny setKeyEquivalent: @"\r"];
	[v addSubview: deny];
	allow = [[NSButton alloc] initWithFrame: NSMakeRect(342, 12, 98, 28)];
	[allow setTitle: @"Allow"];
	[allow setTarget: self];
	[allow setAction: @selector(allow:)];
	[v addSubview: allow];
	[prompt setContentView: v];
	vis = [[NSScreen mainScreen] visibleFrame];
	[prompt setFrameOrigin: NSMakePoint(NSMidX(vis) - fr.size.width / 2,
	    NSMidY(vis) - fr.size.height / 2 + vis.size.height / 6)];
	[NSApp activateIgnoringOtherApps: YES];
	[prompt makeKeyAndOrderFront: nil];
	NSLog(@"ScreenCaptureAgent: asking about %@ (%@)", name,
	    [req objectForKey: @"key"]);
}

- (void)decide: (BOOL)allow
{
	NSString *key = [asking objectForKey: @"key"];
	NSMutableDictionary *apps = [SCStoredAnswers() mutableCopy];
	NSMutableArray *rest = [NSMutableArray array];

	[apps setObject: @{ @"Allowed": @(allow),
			    @"Name": [asking objectForKey: @"name"],
			    @"Date": [[NSDate date] description] }
		 forKey: key];
	storeAnswers(apps);
	NSLog(@"ScreenCaptureAgent: the user %@ %@ (%@)",
	    allow ? @"allowed" : @"refused", [asking objectForKey: @"name"],
	    key);
	for (NSDictionary *r in queue) {
		if ([[r objectForKey: @"key"] isEqual: key])
			[self answer: r allow: allow];
		else
			[rest addObject: r];
	}
	queue = rest;
	[prompt orderOut: nil];
	asking = nil;
	[self askNext];
}

- (void)allow: (id)sender
{
	[self decide: YES];
}

- (void)deny: (id)sender
{
	[self decide: NO];
}

- (void)forget: (const char *)ckey all: (BOOL)all
{
	NSMutableDictionary *apps = [SCStoredAnswers() mutableCopy];

	if (all)
		[apps removeAllObjects];
	else
		[apps removeObjectForKey: [NSString stringWithUTF8String: ckey]];
	storeAnswers(apps);
	NSLog(@"ScreenCaptureAgent: forgot %s", all ? "every answer" : ckey);
}

/* ---- the indicator: who records, and Stop ---- */

static void
listed(const ws_event_capture_info_t *i, void *ctx)
{
	NSMutableArray *a = (__bridge NSMutableArray *)ctx;

	if (i->state == WS_CAPTURE_RUNNING &&
	    (i->flags & WS_CAPTURE_INFO_INDICATOR))
		[a addObject: @{ @"name": [NSString stringWithUTF8String:
		    i->name], @"key": [NSString stringWithUTF8String:
		    i->key], @"stream": @(i->stream) }];
}

- (void)refreshIndicator
{
	NSMutableArray *streams = [NSMutableArray array];
	NSMutableArray *names = [NSMutableArray array];
	NSRect vis, fr = NSMakeRect(0, 0, 300, 34);
	NSButton *stop;
	NSView *v;

	refreshQueued = NO;
	if (ws_capture_list(conn, listed, (__bridge void *)streams) < 0)
		return;
	/* the list may have queued other events */
	[self performSelector: @selector(dispatch) withObject: nil
		   afterDelay: 0];
	[recordingKeys removeAllObjects];
	for (NSDictionary *s in streams) {
		if (![names containsObject: [s objectForKey: @"name"]])
			[names addObject: [s objectForKey: @"name"]];
		[recordingKeys addObject: s];
	}
	if ([streams count] == 0) {
		[indicator orderOut: nil];
		return;
	}
	if (indicator == nil) {
		indicator = [[NSPanel alloc] initWithContentRect: fr
		    styleMask: NSBorderlessWindowMask
		    backing: NSBackingStoreBuffered defer: NO];
		[indicator setLevel: NSStatusWindowLevel];
		[indicator setHidesOnDeactivate: NO];
		[indicator setReleasedWhenClosed: NO];
		[indicator setBackgroundColor: [NSColor colorWithCalibratedWhite:
		    0.15 alpha: 1.0]];
		v = [[NSView alloc] initWithFrame: fr];
		indicatorText = label(NSMakeRect(10, 8, 214, 17), @"",
		    [NSFont boldSystemFontOfSize: 11]);
		[indicatorText setTextColor: [NSColor colorWithCalibratedRed: 1.0
		    green: 0.58 blue: 0.0 alpha: 1.0]];
		[v addSubview: indicatorText];
		stop = [[NSButton alloc] initWithFrame:
		    NSMakeRect(230, 4, 62, 26)];
		[stop setTitle: @"Stop"];
		[stop setTarget: self];
		[stop setAction: @selector(stopRecording:)];
		[v addSubview: stop];
		[indicator setContentView: v];
	}
	[indicatorText setStringValue: [NSString stringWithFormat:
	    @"● Recording: %@", [names componentsJoinedByString: @", "]]];
	vis = [[NSScreen mainScreen] visibleFrame];
	[indicator setFrameOrigin: NSMakePoint(NSMaxX(vis) - fr.size.width - 8,
	    NSMaxY(vis) - fr.size.height - 8)];
	[indicator orderFront: nil];
}

- (void)stopRecording: (id)sender
{
	for (NSDictionary *s in recordingKeys) {
		NSLog(@"ScreenCaptureAgent: the user stopped %@'s recording",
		    [s objectForKey: @"name"]);
		ws_capture_revoke(conn, [[s objectForKey: @"stream"]
		    unsignedIntValue], 0, NULL);
	}
	[indicator orderOut: nil];
}

@end
