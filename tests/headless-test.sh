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

# serial APP TYPE [WIN]: serial of the last matching event in lines
# FROM+1..TO of wstest's log (wstest also logs the activations it did
# itself while starting up).
FROM=0
TO=999999
serial() {
	awk -v app="app=$1" -v type="type=$2" -v win="${3:+win=$3}" \
	    -v from="$FROM" -v to="$TO" '
		NR > from && NR <= to && $1 == "event" && $3 == app &&
		    $4 == type && (win == "" || $5 == win) {
			sub("serial=", "", $2); s = $2
		}
		END { print s }' "$LOG/wstest.log"
}

# mark: wait for wstest to print what the server sent so far; MARKS is
# then the number of lines in its log.
MARKS=0
mark() {
	"$B/wsctl" sync
	sleep 0.5
	MARKS=$(wc -l <"$LOG/wstest.log")
}

# in_part FROM TO PATTERN: PATTERN (ERE) in lines FROM+1..TO.
in_part() {
	awk -v from="$1" -v to="$2" 'NR > from && NR <= to' "$LOG/wstest.log" |
	    grep -q -E -- "$3"
}

"$B/WindowServer" --headless 1280x800 --socket "$SOCK" --config /dev/null -v \
    >"$LOG/server.log" 2>&1 &
SPID=$!
wait_for "$LOG/server.log" "ready:" 10 || { fail "server didn't start"; cat "$LOG/server.log"; exit 1; }
pass "server started (pid $SPID, service $WS_SERVICE_NAME)"

"$B/wstest" >"$LOG/wstest.log" 2>&1 &
TPID=$!
wait_for "$LOG/wstest.log" "^ready" 10 || { fail "wstest didn't get ready"; cat "$LOG/wstest.log"; exit 1; }
pass "wstest connected two apps and a joined connection, and created four windows"
"$B/wsctl" list
grep -q 'join refused app=Mallory EPERM' "$LOG/wstest.log" &&
    pass "join with a wrong token refused (EPERM)" || fail "join with a wrong token"
grep -q 'connected app=Gamma client=3 app=1$' "$LOG/wstest.log" &&
    pass "Gamma (client 3) joined Alpha's app (client 1)" || fail "Gamma didn't join Alpha"

# Stacking: W2 (Alpha, main-menu level) over everything; W3 (Beta,
# created last) over W1 (Alpha) within the normal level.
expect_pixel 350 300 3050c0 "W3 over W1, same level, newer on top"
expect_pixel 150 150 30a040 "W2 at main-menu level"
expect_pixel 320 300 30a040 "W2 over W1 and W3"
expect_pixel 700 600 3050c0 "W3 only"
expect_pixel 120 450 c03030 "W1 only"
expect_pixel 1000 700 000000 "desktop background"

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
mark
P1=$MARKS

# Joined app: a click in Gamma's W4 while Alpha is active only moves the
# key window (Alpha and Gamma are one app), and keys follow it.
"$B/wsctl" click 1000 200 || fail "wsctl click W4"
"$B/wsctl" key a || fail "wsctl key a (W4)"
"$B/wsctl" key cmd+q || fail "wsctl key cmd+q"
"$B/wsctl" key shift+b || fail "wsctl key shift+b"
"$B/wsctl" key Caps_Lock && "$B/wsctl" key Caps_Lock || fail "wsctl key Caps_Lock"
mark
P2=$MARKS
# Beta takes over while Shift is held, then a click in W4 brings the
# whole app back, and Gamma hears that Shift is still down.
"$B/wsctl" keydown Shift_L || fail "wsctl keydown Shift_L"
"$B/wsctl" click 700 600 || fail "wsctl click W3"
mark
P3=$MARKS
"$B/wsctl" click 1000 200 || fail "wsctl click W4 again"
"$B/wsctl" list >"$LOG/list3.txt"
cat "$LOG/list3.txt"
"$B/wsctl" keyup Shift_L || fail "wsctl keyup Shift_L"
mark
P4=$MARKS

kill "$TPID"
wait "$TPID" 2>/dev/null
TPID=

# Event order across the two apps (serials are server-wide).
TO=$P1
gamma_act=$(serial Gamma app-activated)
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
if [ -n "$gamma_act" ] && [ "$deact" -lt "$gamma_act" ]; then
	pass "the joined connection (Gamma) was activated with Alpha"
else
	fail "Gamma wasn't activated with Alpha"
fi
TO=999999
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

# Joined app (P1..P2): the key window moved within the app, no
# activation change, and keys (Command too) went to W4's connection.
in_part "$P1" "$P2" 'app=Alpha type=key-window-lost win=W1' &&
    in_part "$P1" "$P2" 'app=Gamma type=key-window-gained win=W4' &&
    pass "click in the joined connection's window moved the key window" ||
    fail "key window didn't move to W4"
in_part "$P1" "$P2" 'type=app-(de)?activated' &&
    fail "click in W4 re-activated a connection of the same app" ||
    pass "click in W4 didn't make Gamma a separate app"
in_part "$P1" "$P2" 'app=Gamma type=key-down win=W4 keycode=38 ' &&
    pass "key a went to W4's connection (Gamma)" || fail "key a not delivered to W4"
in_part "$P1" "$P2" 'app=Gamma type=key-down win=W4 keycode=24 .*mods=0x8' &&
    pass "Command+q went to the key window's connection" || fail "Command+q not delivered to W4"
in_part "$P1" "$P2" 'app=Alpha type=key-(down|up)' &&
    fail "Alpha got keys while W4 was key" || pass "Alpha got no keys while W4 was key"
# P2..P3: Beta's click deactivated both connections of the app.
in_part "$P2" "$P3" 'app=Alpha type=app-deactivated' &&
    in_part "$P2" "$P3" 'app=Gamma type=app-deactivated' &&
    in_part "$P2" "$P3" 'app=Gamma type=key-window-lost win=W4' &&
    pass "Beta's activation deactivated Alpha and Gamma" || fail "app deactivation with a joined connection"
# P3..P4: a click in W4 activated the app (both connections) with W4 key.
in_part "$P3" "$P4" 'app=Alpha type=app-activated' &&
    in_part "$P3" "$P4" 'app=Gamma type=app-activated' &&
    in_part "$P3" "$P4" 'app=Gamma type=key-window-gained win=W4' &&
    pass "click in W4 activated Alpha's app with W4 as key window" || fail "activation through the joined connection"
grep -q '"W4"$' "$LOG/list3.txt" && grep '"W4"$' "$LOG/list3.txt" | grep -q ' active key ' &&
    grep '"W1"$' "$LOG/list3.txt" | grep -q ' active ' &&
    pass "wsctl list: W4 key, W1 active (one app)" || fail "wsctl list after the join"

# XKB state (Gamma connected with WS_CONNECT_XKB; the others didn't).
grep -q 'app=Gamma type=keymap win=- format=1 size=[1-9][0-9]* delay=500 interval=33 text=xkb_keymap sealed=yes' "$LOG/wstest.log" &&
    pass "keymap memfd (sealed, xkb_keymap text) and repeat settings at welcome" ||
    fail "no usable WS_EVENT_KEYMAP"
grep -q -E 'app=(Alpha|Beta) type=(keymap|modifiers|keys-held)' "$LOG/wstest.log" &&
    fail "a client without WS_CONNECT_XKB got XKB events" ||
    pass "clients without WS_CONNECT_XKB got no XKB events"
in_part "$P1" "$P2" 'app=Gamma type=keys-held win=W4 count=0 keys=$' &&
    pass "keys-held (none) when W4's connection started getting keys" ||
    fail "no keys-held when the key window moved to W4"
in_part "$P1" "$P2" 'app=Gamma type=modifiers win=W4 depressed=0x1 latched=0x0 locked=0x0 group=0 mods=0x1' &&
    pass "modifiers: Shift depressed (XKB mask 0x1)" || fail "modifiers for Shift"
in_part "$P1" "$P2" 'app=Gamma type=modifiers win=W4 depressed=0x0 latched=0x0 locked=0x2 ' &&
    pass "modifiers: Caps Lock locked (XKB mask 0x2)" || fail "modifiers for Caps Lock"
in_part "$P3" "$P4" 'app=Gamma type=keys-held win=W4 count=1 keys=50$' &&
    in_part "$P3" "$P4" 'app=Gamma type=modifiers win=W4 depressed=0x1 ' &&
    pass "on activation Gamma heard Shift_L (50) still held, with its modifiers" ||
    fail "held keys on activation"
in_part "$P3" "$P4" 'app=Gamma type=key-up win=W4 keycode=50 ' &&
    in_part "$P3" "$P4" 'app=Gamma type=modifiers win=W4 depressed=0x0 ' &&
    pass "Shift_L's release reached Gamma and cleared the modifier" ||
    fail "release of a held key after activation"

kill "$SPID"
wait "$SPID" 2>/dev/null
SPID=
grep -q "exiting" "$LOG/server.log" && pass "server shut down cleanly" || fail "server shutdown"
[ -e "$SOCK" ] && fail "socket left behind"
grep -q "VT [0-9]" "$LOG/server.log" && fail "headless server touched a VT"

echo "logs: $LOG"
if [ $FAILS -ne 0 ]; then
	echo "$FAILS check(s) failed"
	exit 1
fi
echo "all checks passed"
