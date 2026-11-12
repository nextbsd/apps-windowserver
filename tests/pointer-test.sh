#!/bin/sh
# Copyright (c) 2026, Joseph Maloney
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless test of what X servers need from WindowServer
# (apps-windowserver#8 and #9): window ids chosen by the client, a
# cursor per window, input regions, pointer grabs (all events, and
# popup grabs that hear about an outside press), pointer warp, query and
# tracking, and a frame carried in a commit. Runs wspointer (two apps,
# four windows, see tools/wspointer.c) and drives it with wsctl.
#
#   sh tests/pointer-test.sh [BUILD_DIR]
#
# Needs no privileges and doesn't touch the console or the GPU.

B=${1:-build}
export WS_SERVICE_NAME="org.nextbsd.windowserver.ptest.$$"
export WS_BackgroundColor=202020
LOG="/tmp/ws-ptest-$$"
SOCK="$LOG/ws.sock"
mkdir -p "$LOG" || exit 1
FAILS=0
SPID=
PPID_=

fail() {
	echo "FAIL: $*"
	FAILS=$((FAILS + 1))
}

pass() {
	echo "ok:   $*"
}

cleanup() {
	[ -n "$PPID_" ] && kill "$PPID_" 2>/dev/null
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

expect_pixel() {
	got=$("$B/wsctl" pixel "$1" "$2" | awk '{print $2}')
	if [ "$got" = "$3" ]; then
		pass "pixel $1,$2 is $3 ($4)"
	else
		fail "pixel $1,$2 is $got, expected $3 ($4)"
	fi
}

# mark: wait for wspointer to print what the server sent so far; M is
# then the number of lines in its log.
M=0
mark() {
	"$B/wsctl" sync
	sleep 0.5
	M=$(wc -l <"$LOG/wspointer.log")
}

# seen FROM TO PATTERN: PATTERN (ERE) in lines FROM+1..TO of the log.
seen() {
	awk -v from="$1" -v to="$2" 'NR > from && NR <= to' \
	    "$LOG/wspointer.log" | grep -q -E -- "$3"
}

# check WHAT FROM TO PATTERN: PATTERN must be there.
check() {
	if seen "$2" "$3" "$4"; then pass "$1"; else fail "$1"; fi
}

# check_not WHAT FROM TO PATTERN: PATTERN must not be there.
check_not() {
	if seen "$2" "$3" "$4"; then fail "$1"; else pass "$1"; fi
}

"$B/WindowServer" --headless 1280x800 --socket "$SOCK" --config /dev/null -v \
    >"$LOG/server.log" 2>&1 &
SPID=$!
wait_for "$LOG/server.log" "ready:" 10 || { fail "server didn't start"; cat "$LOG/server.log"; exit 1; }
pass "server started (pid $SPID, service $WS_SERVICE_NAME)"

"$B/wspointer" >"$LOG/wspointer.log" 2>&1 &
PPID_=$!
wait_for "$LOG/wspointer.log" "^ready" 10 || { fail "wspointer didn't get ready"; cat "$LOG/wspointer.log"; exit 1; }
pass "wspointer connected two apps and created four windows"
"$B/wsctl" list
OTHER=$(awk '/^connected app=Other/ { sub("client=", "", $3); print $3 }' "$LOG/wspointer.log")

# Window ids chosen by the client: Xeno's windows came without a round
# trip each, from a block of ids the server reserved, and show.
check "E1, E2 and E3 created without a round trip each" 0 99999 \
    '^window E3 id=[1-9][0-9]* app=Xeno nowait=yes'
E1=$(awk '/^window E1/ { sub("id=", "", $3); print $3 }' "$LOG/wspointer.log")
E3=$(awk '/^window E3/ { sub("id=", "", $3); print $3 }' "$LOG/wspointer.log")
[ "$E3" -eq $((E1 + 2)) ] && pass "ids taken in order from the block ($E1..$E3)" ||
    fail "ids $E1..$E3 not from one block"
expect_pixel 120 120 c03030 "E1 shows"
expect_pixel 650 150 30a040 "E2 shows"
expect_pixel 950 550 c0c030 "E3 shows, above O1"
expect_pixel 1150 700 3050c0 "O1 shows"
mark
P0=$M
check_not "no errors so far" 0 "$P0" 'type=error'

# A cursor per window: E1's magenta square the moment the pointer is
# over it, the connection's arrow over E2 (its white fill at 1,3 from
# the hot spot), whoever is active.
"$B/wsctl" move 300 300
expect_pixel 301 303 ff00ff "E1's own cursor over E1"
"$B/wsctl" move 650 150
expect_pixel 651 153 ffffff "the arrow over E2"
"$B/wsctl" move 300 300
expect_pixel 301 303 ff00ff "E1's cursor again on entering E1"

# Input regions: E3 takes pointer events in its left half only. A click
# in its right half goes to what is below: the desktop, then O1.
"$B/wsctl" click 870 550
mark
P1=$M
check_not "a click in E3's right half went to no app (desktop below)" "$P0" "$P1" \
    'type=mouse-down'
"$B/wsctl" click 950 550
mark
P2=$M
check "a click in E3's right half over O1 went to O1" "$P1" "$P2" \
    'app=Other type=mouse-down win=O1 x=50 y=100'
check_not "...and not to E3" "$P1" "$P2" 'app=Xeno type=mouse-down'
"$B/wsctl" click 770 550
mark
P3=$M
check "a click in E3's left half went to E3 and activated Xeno" "$P2" "$P3" \
    'app=Xeno type=mouse-down win=E3 x=70 y=50'
check "" "$P2" "$P3" 'app=Xeno type=app-activated'

# Pointer tracking: while Xeno is active it hears motion over O1, with
# no window.
"$B/wsctl" move 1150 700
mark
P4=$M
check "tracking: Xeno hears motion over O1 (window 0, screen position)" "$P3" "$P4" \
    'app=Xeno type=mouse-moved win=- x=1150 y=700 screen=1150,700'
check "O1 hears it too" "$P3" "$P4" 'app=Other type=mouse-moved win=O1'

# Pointer grab: everything goes to Xeno, nothing activates.
"$B/wsctl" key g
mark
P5=$M
check "Xeno grabbed the pointer" "$P4" "$P5" '^grab ok app=Xeno mode=pointer'
check "the grab took O1's hover away" "$P4" "$P5" 'app=Other type=mouse-exited win=O1'
"$B/wsctl" move 1100 650
"$B/wsctl" click 1100 650
kill -USR1 $PPID_
mark
P6=$M
check "grabbed: motion over O1 went to Xeno (window 0)" "$P5" "$P6" \
    'app=Xeno type=mouse-moved win=- x=1100 y=650'
check "grabbed: a press over O1 sent Xeno pointer-outside" "$P5" "$P6" \
    'app=Xeno type=pointer-outside win=- x=1100 y=650 screen=1100,650 button=0'
check "...and the press itself, with no window" "$P5" "$P6" \
    'app=Xeno type=mouse-down win=- x=1100 y=650'
check "" "$P5" "$P6" 'app=Xeno type=mouse-up win=- x=1100 y=650'
check_not "grabbed: O1 got no pointer events" "$P5" "$P6" 'app=Other type=mouse-'
check_not "grabbed: the click activated nothing" "$P5" "$P6" 'type=app-(de)?activated'
check "another app can't grab (EPERM: not active)" "$P5" "$P6" \
    '^grab refused app=Other EPERM'
"$B/wsctl" move 650 150
"$B/wsctl" click 650 150
mark
P7=$M
check "grabbed: entering E2 is an enter about E2" "$P6" "$P7" \
    'app=Xeno type=mouse-entered win=E2 x=50 y=50'
check "grabbed: a click in E2 is about E2" "$P6" "$P7" \
    'app=Xeno type=mouse-down win=E2 x=50 y=50'
check_not "grabbed: no pointer-outside over Xeno's own window" "$P6" "$P7" \
    'type=pointer-outside'
"$B/wsctl" move 1100 650
"$B/wsctl" key u
"$B/wsctl" click 1100 650
mark
P8=$M
check "after the ungrab O1 gets its click, and Other is activated" "$P7" "$P8" \
    'app=Other type=mouse-down win=O1'
check "" "$P7" "$P8" 'app=Other type=app-activated'
check_not "after the ungrab Xeno gets no pointer-outside" "$P7" "$P8" \
    'type=pointer-outside'

# The server ends a grab when the overview opens, and when another app
# is activated (here by the Dock).
"$B/wsctl" click 120 120
"$B/wsctl" key g
"$B/wsctl" overview all >/dev/null
"$B/wsctl" overview close >/dev/null
mark
P9=$M
check "the overview ended Xeno's grab" "$P8" "$P9" \
    'app=Xeno type=pointer-ungrabbed'
"$B/wsctl" key g
"$B/wsctl" dock activate "$OTHER" >/dev/null
mark
P10=$M
check "activating Other ended Xeno's grab" "$P9" "$P10" \
    'app=Xeno type=pointer-ungrabbed'
check "" "$P9" "$P10" 'app=Other type=app-activated'

# Popup grab (WS_GRAB_OUTSIDE): clicks in Xeno's windows go on as usual;
# the first one outside sends pointer-outside, ends the grab and goes
# ahead (O1 gets it, Other is activated).
"$B/wsctl" click 120 120
"$B/wsctl" key o
"$B/wsctl" click 650 150
mark
P11=$M
check "popup grab taken" "$P10" "$P11" '^grab ok app=Xeno mode=outside'
check "popup grab: a click in E2 is an ordinary click" "$P10" "$P11" \
    'app=Xeno type=mouse-down win=E2'
check_not "...with no pointer-outside" "$P10" "$P11" 'type=pointer-outside'
"$B/wsctl" click 1100 650
mark
P12=$M
check "popup grab: a click on O1 sent Xeno pointer-outside" "$P11" "$P12" \
    'app=Xeno type=pointer-outside win=- x=1100 y=650'
check "...and then went to O1, activating Other" "$P11" "$P12" \
    'app=Other type=mouse-down win=O1'
check "" "$P11" "$P12" 'app=Other type=app-activated'

# Warp and query.
"$B/wsctl" click 120 120
"$B/wsctl" key w
"$B/wsctl" key q
mark
P13=$M
check "warp: Xeno heard the motion to 300,200 in E1" "$P12" "$P13" \
    'app=Xeno type=mouse-moved win=E1 x=300 y=200 screen=400,300'
check "query after the warp: 400,300 over E1" "$P12" "$P13" \
    '^query app=Xeno x=400 y=300 buttons=0x0 win=E1'
expect_pixel 401 303 ff00ff "the cursor moved with the warp (E1's cursor)"
# Over O1 (Xeno still active): no warp, but the query answers.
"$B/wsctl" move 1100 650
kill -USR2 $PPID_
mark
P14=$M
check "no warp while the pointer is over another app's window (EPERM)" "$P13" "$P14" \
    'app=Xeno type=error win=- request=0x[0-9a-f]+ code=1 '
check "query while active, over O1: no window of Xeno's" "$P13" "$P14" \
    '^query app=Xeno x=1100 y=650 buttons=0x0 win=-'
# Other active: no warp, no query, no tracking.
"$B/wsctl" click 1100 650
mark
P14=$M
"$B/wsctl" move 1000 200
kill -USR2 $PPID_
mark
P15=$M
check "query refused while another app is active (EPERM)" "$P14" "$P15" \
    '^query refused app=Xeno EPERM'
check_not "tracking stops while Xeno isn't active" "$P14" "$P15" \
    'app=Xeno type=mouse-moved win=-'
expect_pixel 50 50 202020 "the refused warp didn't move the pointer there"

# A frame carried in the commit: E1 becomes 500x350 and orange at once.
"$B/wsctl" click 120 120
"$B/wsctl" key r
mark
P16=$M
check "E1 resized in one commit" "$P15" "$P16" '^resized E1'
expect_pixel 590 440 e0a020 "E1's new corner, drawn with the new buffer"
expect_pixel 130 130 e0a020 "E1's new buffer everywhere"
"$B/wsctl" list | grep '"E1"$' | grep -q 'frame=100,100,500x350' &&
    pass "wsctl list shows E1's new frame" || fail "E1's frame not 500x350"

# The input region again: back to the whole window.
"$B/wsctl" click 770 550
"$B/wsctl" key s
"$B/wsctl" click 870 550
mark
P17=$M
check "E3 takes the click in its right half after the reset" "$P16" "$P17" \
    'app=Xeno type=mouse-down win=E3 x=170 y=50'

check_not "no unexpected errors" "$P15" "$P17" 'type=error'

kill "$SPID"
wait "$SPID" 2>/dev/null
SPID=
grep -q "exiting" "$LOG/server.log" && pass "server shut down cleanly" || fail "server shutdown"

echo "logs: $LOG"
if [ $FAILS -ne 0 ]; then
	echo "$FAILS check(s) failed"
	exit 1
fi
echo "all checks passed"
