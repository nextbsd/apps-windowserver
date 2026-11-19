#!/bin/sh
# Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless test of the menu bar (apps-windowserver#97, "The menu bar" in
# WSProtocol.h): apps' bar windows on WS_LEVEL_MENU_BAR, shown for the
# active app only and swapped on activation with no frame in between
# (even while the incoming app is stopped), the outgoing bar held for an
# app with no bar yet, then the empty bar in the last bar's colour
# (never the desktop), and status items on WS_LEVEL_STATUS_ITEM packed
# right to left by priority and age, repacked as they change, their
# width sent to every connection, hidden with the strip. Runs wsmenubar
# (tools/wsmenubar.c) and drives it with wsctl.
#
#   sh tests/menubar-test.sh [BUILD_DIR]
#
# Needs no privileges and doesn't touch the console or the GPU.

B=${1:-build}
export WS_SERVICE_NAME="org.nextbsd.windowserver.mbtest.$$"
export WS_BackgroundColor=202020
LOG="/tmp/ws-mbtest-$$"
SOCK="$LOG/ws.sock"
mkdir -p "$LOG" || exit 1
FAILS=0
SPID=
PIDS=

fail() {
	echo "FAIL: $*"
	FAILS=$((FAILS + 1))
}

pass() {
	echo "ok:   $*"
}

cleanup() {
	for p in $PIDS; do
		kill -CONT "$p" 2>/dev/null
		kill "$p" 2>/dev/null
	done
	[ -n "$SPID" ] && kill "$SPID" 2>/dev/null
	wait 2>/dev/null
}
trap cleanup EXIT INT TERM

wait_for() {
	i=0
	while [ $i -lt $(($3 * 10)) ]; do
		grep -q "$2" "$1" 2>/dev/null && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

settle() {
	"$B/wsctl" sync
	sleep 0.3
	"$B/wsctl" sync
}

pixel() {
	"$B/wsctl" pixel "$1" "$2" | awk '{print $2}'
}

expect_pixel() {
	got=$(pixel "$1" "$2")
	if [ "$got" = "$3" ]; then
		pass "pixel $1,$2 is $3 ($4)"
	else
		fail "pixel $1,$2 is $got, expected $3 ($4)"
	fi
}

# winfo TITLE FIELD: a field of TITLE's line in wsctl list
# (level, frame), or nothing while it isn't on screen.
winfo() {
	"$B/wsctl" list | awk -v t="\"$1\"" -v f="$2" '$NF == t {
	    for (i = 1; i <= NF; i++) if (index($i, f "=") == 1) {
	    sub(f "=", "", $i); print $i } }'
}

expect_frame() {
	got=$(winfo "$1" frame)
	if [ "$got" = "$2" ]; then
		pass "$1 at $2 ($3)"
	else
		fail "$1 at '$got', expected $2 ($3)"
	fi
}

screen_field() {
	"$B/wsctl" screen | awk -v k="$1" '$1 == k { print $2 }'
}

# start NAME ARGS...: a wsmenubar; its pid in $LAST, its client in $CLIENT.
start() {
	name=$1
	shift
	"$B/wsmenubar" "$@" >"$LOG/$name.log" 2>&1 &
	LAST=$!
	PIDS="$PIDS $LAST"
	wait_for "$LOG/$name.log" "^client " 5 ||
	    { fail "$name didn't connect"; cat "$LOG/$name.log"; }
	CLIENT=$(awk '$1 == "client" { print $2; exit }' "$LOG/$name.log")
}

ready() {
	wait_for "$LOG/$1.log" "^ready" 10 ||
	    { fail "$1 didn't get ready"; cat "$LOG/$1.log"; }
	settle
}

"$B/WindowServer" --headless 1280x800 --socket "$SOCK" --config /dev/null -v \
    >"$LOG/server.log" 2>&1 &
SPID=$!
wait_for "$LOG/server.log" "ready:" 10 || { fail "server didn't start"; cat "$LOG/server.log"; exit 1; }
pass "server started (pid $SPID, service $WS_SERVICE_NAME)"

[ "$(screen_field statusitems)" = 0 ] &&
    pass "the welcome: no status items" || fail "statusitems at the start"

# ---- the bar window ----
start A bar A d0d0d0 c03030
APID=$LAST
A=$CLIENT
ready A
expect_pixel 50 10 c03030 "A's bar, its menu titles"
expect_pixel 1000 10 d0d0d0 "A's bar, its body"
[ "$(winfo A-bar level)" = 24 ] && pass "A's bar on WS_LEVEL_MENU_BAR" ||
    fail "A's bar on level '$(winfo A-bar level)'"
expect_frame A-bar 0,0,1280x24 "the strip along the main display's top"
[ "$(screen_field menubar)" = 24 ] && pass "the strip is 24 points" ||
    fail "strip $(screen_field menubar)"

start B bar B e0e0e0 30a040
BPID=$LAST
B_=$CLIENT
ready B
expect_pixel 50 10 30a040 "B active: B's bar"
[ -z "$(winfo A-bar frame)" ] && pass "A's bar off screen while B is active" ||
    fail "A's bar still on screen"

# The swap is the server's: A is stopped (busy), yet its bar comes back
# the moment it is activated, and B's ordering its bar out (as GNUstep
# does on deactivation) changes nothing.
kill -STOP "$APID"
"$B/wsctl" dock activate "$A" >/dev/null
expect_pixel 50 10 c03030 "A activated while stopped: its bar at once"
[ -z "$(winfo B-bar frame)" ] && pass "B's bar off screen in the same step" ||
    fail "B's bar still on screen"
kill -CONT "$APID"
settle
expect_pixel 50 10 c03030 "A's ordering in its bar changes nothing"
"$B/wsctl" dock activate "$B_" >/dev/null
expect_pixel 50 10 30a040 "B again"
settle
expect_pixel 50 10 30a040 "A ordering its bar out (deactivated) changes nothing"

# A pass of every frame between: switch back and forth and look at the
# strip after each request; it never shows the desktop.
gaps=0
for n in 1 2 3 4 5 6; do
	"$B/wsctl" dock activate "$A" >/dev/null
	[ "$(pixel 1000 10)" = 202020 ] && gaps=$((gaps + 1))
	"$B/wsctl" dock activate "$B_" >/dev/null
	[ "$(pixel 1000 10)" = 202020 ] && gaps=$((gaps + 1))
done
[ $gaps = 0 ] && pass "12 switches, the strip never shows the desktop" ||
    fail "$gaps switches showed the desktop in the strip"

# ---- an app with no bar yet: the hold, then the empty bar ----
start C bar C c0c0f0 3050c0 1800
CPID=$LAST
sleep 0.15
expect_pixel 50 10 30a040 "C launching: B's bar stays"
sleep 0.8
expect_pixel 50 10 e0e0e0 "after the hold: the empty bar in B's colour"
expect_pixel 1000 10 e0e0e0 "the empty bar is full width"
ready C
expect_pixel 50 10 3050c0 "C named its bar: C's bar"
[ "$(winfo C-bar level)" = 24 ] && pass "C's bar on WS_LEVEL_MENU_BAR" ||
    fail "C's bar level"

start P plain P 806020
PPID_=$LAST
ready P
sleep 0.7
expect_pixel 50 10 c0c0f0 "an app with no bar: the empty bar in C's colour"
[ "$(screen_field menubar)" = 24 ] && pass "the strip stays for it" ||
    fail "strip $(screen_field menubar)"
C=$(awk '$1 == "client" { print $2; exit }' "$LOG/C.log")
"$B/wsctl" dock activate "$C" >/dev/null
expect_pixel 50 10 3050c0 "back to C: its bar at once"

# ---- status items ----
start clock item clock 80 2000 404040
CLOCK=$LAST
ready clock
start wifi item wifi 30 1000 806000
WIFI=$LAST
ready wifi
start app item app 40 0 600060
ready app
start bt item bt 30 1000 008060
BT=$LAST
ready bt
expect_frame clock 1200,0,80x24 "the clock rightmost"
expect_frame wifi 1170,0,30x24 "a system item left of it"
expect_frame bt 1140,0,30x24 "a newer system item left of the older"
expect_frame app 1100,0,40x24 "an app's item left of the system ones"
[ "$(winfo clock level)" = 25 ] && pass "items on WS_LEVEL_STATUS_ITEM" ||
    fail "item level $(winfo clock level)"
expect_pixel 1230 10 404040 "the clock drawn over the bar"
expect_pixel 1120 10 600060 "the app's item"
expect_pixel 1000 10 c0c0f0 "C's bar left of the items"
[ "$(screen_field statusitems)" = 180 ] && pass "statusitems 180 in the welcome" ||
    fail "statusitems $(screen_field statusitems)"
grep -q "statusitems=180" "$LOG/C.log" &&
    pass "the active app heard the width (WS_EVENT_USABLE_AREA)" ||
    fail "C didn't hear statusitems=180"
grep -q "frame item 1200,0,80x24" "$LOG/clock.log" &&
    pass "the clock's owner heard its frame" || fail "clock frame event"

kill -USR1 "$WIFI"
wait_for "$LOG/wifi.log" "^widened" 5
settle
expect_frame wifi 1140,0,60x24 "Wi-Fi twice as wide"
expect_frame bt 1110,0,30x24 "repacked"
expect_frame app 1070,0,40x24 "repacked"
kill -USR2 "$BT"
wait_for "$LOG/bt.log" "^hidden" 5
settle
[ -z "$(winfo bt frame)" ] && pass "an item ordered out takes no place" ||
    fail "bt still placed"
expect_frame app 1100,0,40x24 "closed up"
kill "$CLOCK"
settle
expect_frame wifi 1220,0,60x24 "the clock gone: Wi-Fi rightmost"
[ "$(screen_field statusitems)" = 100 ] && pass "statusitems 100" ||
    fail "statusitems $(screen_field statusitems)"

# A press on a status item: the active app stays, and the item's app may
# grab the pointer for its menu.
"$B/wsctl" click 1240 10 >/dev/null
settle
grep -q "mouse-down item" "$LOG/wifi.log" && pass "the item got the press" ||
    fail "no press on the item"
grep -q "^grab ok" "$LOG/wifi.log" &&
    pass "the item's app may grab the pointer for its menu" ||
    fail "the item's app can't grab: $(grep '^grab' "$LOG/wifi.log")"
"$B/wsctl" list | grep '"C-doc"$' | grep -q ' active' &&
    pass "C stays active" || fail "the press activated another app"
expect_pixel 50 10 3050c0 "C's bar stays"

# ---- no strip: no items ----
kill "$APID" "$BPID" "$CPID" "$PPID_"
settle
sleep 0.7
"$B/wsctl" sync
[ "$(screen_field menubar)" = 0 ] && pass "no app with a bar: no strip" ||
    fail "strip $(screen_field menubar)"
[ -z "$(winfo wifi frame)" ] && pass "status items hidden without a strip" ||
    fail "wifi on screen without a strip"
[ "$(screen_field statusitems)" = 0 ] && pass "statusitems 0" ||
    fail "statusitems $(screen_field statusitems)"
"$B/wsctl" move 640 600
expect_pixel 1240 10 202020 "the desktop: no bar, no items"

for f in "$LOG"/*.log; do
	[ "$f" = "$LOG/server.log" ] && continue
	grep -q "^error" "$f" && { fail "$(basename "$f" .log) got an error"; grep "^error" "$f"; }
done

if [ $FAILS -eq 0 ]; then
	echo "menubar-test: all passed"
	rm -rf "$LOG"
	exit 0
fi
echo "menubar-test: $FAILS failed (logs in $LOG)"
exit 1
