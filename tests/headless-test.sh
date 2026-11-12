#!/bin/sh
# Copyright (c) 2026, Joseph Maloney
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless end-to-end test: starts WindowServer without DRM, VT or
# libinput, runs wstest (two apps, three windows), then uses wsctl to
# check the composited screen and drive synthetic input.
#
#   sh tests/headless-test.sh [BUILD_DIR]
#
# Needs no privileges and doesn't touch the console or the GPU.

B=${1:-build}
export WS_SERVICE_NAME="org.nextbsd.windowserver.test.$$"
export WS_SnapshotPath="/tmp/ws-test-$$-signal.ppm"
LOG="/tmp/ws-test-$$"
SOCK="$LOG/ws.sock"
mkdir -p "$LOG" || exit 1
FAILS=0
SPID=
TPID=

fail() {
	echo "FAIL: $*"
	FAILS=$((FAILS + 1))
}

pass() {
	echo "ok:   $*"
}

cleanup() {
	[ -n "$TPID" ] && kill "$TPID" 2>/dev/null
	[ -n "$SPID" ] && kill "$SPID" 2>/dev/null
	wait 2>/dev/null
}
trap cleanup EXIT INT TERM

# wait_for FILE PATTERN SECONDS
wait_for() {
	i=0
	while [ $i -lt $(($3 * 10)) ]; do
		grep -q "$2" "$1" 2>/dev/null && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

# expect_pixel X Y RRGGBB WHAT
expect_pixel() {
	got=$("$B/wsctl" pixel "$1" "$2" | awk '{print $2}')
	if [ "$got" = "$3" ]; then
		pass "pixel $1,$2 is $3 ($4)"
	else
		fail "pixel $1,$2 is $got, expected $3 ($4)"
	fi
}

# serial APP TYPE [WIN]: serial of the last matching event (wstest
# also logs the activations it did itself while starting up)
serial() {
	awk -v app="app=$1" -v type="type=$2" -v win="${3:+win=$3}" '
		$1 == "event" && $3 == app && $4 == type &&
		    (win == "" || $5 == win) {
			sub("serial=", "", $2); s = $2
		}
		END { print s }' "$LOG/wstest.log"
}

"$B/WindowServer" --headless 1280x800 --socket "$SOCK" -v \
    >"$LOG/server.log" 2>&1 &
SPID=$!
wait_for "$LOG/server.log" "ready:" 10 || { fail "server didn't start"; cat "$LOG/server.log"; exit 1; }
pass "server started (pid $SPID, service $WS_SERVICE_NAME)"

"$B/wstest" >"$LOG/wstest.log" 2>&1 &
TPID=$!
wait_for "$LOG/wstest.log" "^ready" 10 || { fail "wstest didn't get ready"; cat "$LOG/wstest.log"; exit 1; }
pass "wstest connected two apps and created three windows"
"$B/wsctl" list

# Stacking: W2 (Alpha, main-menu level) over everything; W3 (Beta,
# created last) over W1 (Alpha) within the normal level.
expect_pixel 350 300 3050c0 "W3 over W1, same level, newer on top"
expect_pixel 150 150 30a040 "W2 at main-menu level"
expect_pixel 320 300 30a040 "W2 over W1 and W3"
expect_pixel 700 600 3050c0 "W3 only"
expect_pixel 120 450 c03030 "W1 only"
expect_pixel 1000 700 4a5a6a "desktop background"

# Click into Alpha's W1 where nothing covers it: Alpha becomes active.
"$B/wsctl" click 150 450 || fail "wsctl click"
"$B/wsctl" key a || fail "wsctl key a"
"$B/wsctl" keydown b && sleep 1 && "$B/wsctl" keyup b || fail "wsctl key b"
"$B/wsctl" scroll 0 15 || fail "wsctl scroll"
expect_pixel 350 300 c03030 "W1 raised above W3 by the click"
"$B/wsctl" list

"$B/wsctl" snapshot "$LOG/screen.ppm" >/dev/null &&
    head -c 2 "$LOG/screen.ppm" | grep -q P6 &&
    pass "wsctl snapshot wrote a PPM" || fail "wsctl snapshot"
kill -USR1 "$SPID"
wait_for "$LOG/server.log" "snapshot written" 5 &&
    [ -s "$WS_SnapshotPath" ] && pass "SIGUSR1 snapshot" || fail "SIGUSR1 snapshot"

sleep 0.5
kill "$TPID"
wait "$TPID" 2>/dev/null
TPID=

# Event order across the two apps (serials are server-wide).
lost=$(serial Beta key-window-lost W3)
deact=$(serial Beta app-deactivated)
act=$(serial Alpha app-activated)
gained=$(serial Alpha key-window-gained W1)
down=$(serial Alpha mouse-down W1)
up=$(serial Alpha mouse-up W1)
echo "serials: Beta key-lost=$lost deactivated=$deact; Alpha activated=$act key-gained=$gained mouse-down=$down mouse-up=$up"
if [ -n "$lost" ] && [ -n "$up" ] && [ "$lost" -lt "$deact" ] &&
    [ "$deact" -lt "$act" ] && [ "$act" -lt "$gained" ] &&
    [ "$gained" -lt "$down" ] && [ "$down" -lt "$up" ]; then
	pass "deactivate (Beta) before activate (Alpha), then key focus, then the click"
else
	fail "activation order"
fi
grep -q 'app=Alpha type=mouse-down win=W1 x=50 y=350 button=0 clicks=1' "$LOG/wstest.log" &&
    pass "click delivered with window-local coordinates" || fail "mouse-down coordinates"
grep -q 'app=Alpha type=key-down win=W1 keycode=38 keysym=0x61 text="a"' "$LOG/wstest.log" &&
    pass "key a reached Alpha's key window W1" || fail "key a not delivered to W1"
if grep -q -E 'app=Beta type=key-(down|up)' "$LOG/wstest.log"; then
	fail "Beta (inactive) received key events"
else
	pass "Beta (inactive) received no key events"
fi
grep -q 'app=Alpha type=key-down win=W1 .*text="b".* repeat' "$LOG/wstest.log" &&
    pass "server key repeat" || fail "no key repeat for b"
grep -q 'app=Alpha type=scroll win=W1' "$LOG/wstest.log" &&
    pass "scroll delivered" || fail "scroll"
grep -q 'app=Beta type=buffer-release win=W3 buffer=1' "$LOG/wstest.log" &&
    pass "old buffer released after attaching a new one" || fail "buffer-release"
grep -q 'app=Beta type=frame-done win=W3' "$LOG/wstest.log" &&
    pass "frame-done after commit" || fail "frame-done"
if grep -q 'type=error' "$LOG/wstest.log"; then
	fail "server reported errors to wstest"
fi

kill "$SPID"
wait "$SPID" 2>/dev/null
SPID=
grep -q "exiting" "$LOG/server.log" && pass "server shut down cleanly" || fail "server shutdown"
[ -e "$SOCK" ] && fail "socket left behind"

echo "logs: $LOG"
if [ $FAILS -ne 0 ]; then
	echo "$FAILS check(s) failed"
	exit 1
fi
echo "all checks passed"
