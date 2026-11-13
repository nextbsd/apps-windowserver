#!/bin/sh
# Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless test of windows kept clear of the Dock (apps-windowserver#42):
# a window opened under the Dock placed beside it; the Dock moved from
# the right to the top (windows along the top edge, a torn-off menu and
# the panel pushed down, a too-tall window shrunk), a new window opened
# under it, the user moving a pushed window, the Dock moved to the bottom
# (windows along the bottom pushed up, the others given their frames
# back, the moved one not) and back to the right (every frame back); a
# window the user drags partly under the Dock left there; a Mac menu bar
# coming and going; the screen shrinking and growing again; and a
# floating window in tiling mode. Desktop-level and minimized windows
# never move, and the owners hear each frame. Runs wsclear (see
# tools/wsclear.c) and drives it with wsctl.
#
#   sh tests/dock-clear-test.sh [BUILD_DIR]
#
# Needs no privileges and doesn't touch the console or the GPU.

B=${1:-build}
export WS_SERVICE_NAME="org.nextbsd.windowserver.ctest.$$"
export WS_BackgroundColor=202020
LOG="/tmp/ws-ctest-$$"
SOCK="$LOG/ws.sock"
mkdir -p "$LOG" || exit 1
FAILS=0
SPID=
CPID=
DPID=

fail() {
	echo "FAIL: $*"
	FAILS=$((FAILS + 1))
}

pass() {
	echo "ok:   $*"
}

cleanup() {
	[ -n "$DPID" ] && kill "$DPID" 2>/dev/null
	[ -n "$CPID" ] && kill "$CPID" 2>/dev/null
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

# settle: let wsclear answer what the server sent (new frames, redraws).
settle() {
	"$B/wsctl" sync
	sleep 0.4
	"$B/wsctl" sync
}

# frame NAME: NAME's frame from wsctl list.
frame() {
	"$B/wsctl" list | awk -v t="\"$1\"" '$NF == t {
	    for (i = 1; i <= NF; i++) if ($i ~ /^frame=/) {
	    sub("frame=", "", $i); print $i } }'
}

# expect NAME X,Y,WxH WHY
expect() {
	got=$(frame "$1")
	if [ "$got" = "$2" ]; then
		pass "$1 at $2 ($3)"
	else
		fail "$1 at '$got', expected $2 ($3)"
	fi
}

expect_pixel() {
	got=$("$B/wsctl" pixel "$1" "$2" | awk '{print $2}')
	if [ "$got" = "$3" ]; then
		pass "pixel $1,$2 is $3 ($4)"
	else
		fail "pixel $1,$2 is $got, expected $3 ($4)"
	fi
}

M=0
mark() {
	settle
	M=$(wc -l <"$LOG/wsclear.log")
}

seen() {
	awk -v from="$1" -v to="$2" 'NR > from && NR <= to' \
	    "$LOG/wsclear.log" | grep -q -E -- "$3"
}

check() {
	if seen "$2" "$3" "$4"; then pass "$1"; else fail "$1"; fi
}

check_not() {
	if seen "$2" "$3" "$4"; then fail "$1"; else pass "$1"; fi
}

# dock EDGE THICKNESS: a Dock on EDGE. The new Dock reports before the
# old one goes, so the usable area goes straight from one edge to the
# other, as when the Dock moves.
dock() {
	"$B/wsctl" dock edge "$1" "$2" 600 >"$LOG/dock-$1.log" 2>&1 &
	new=$!
	wait_for "$LOG/dock-$1.log" "^dock " 5 ||
	    { sleep 1; }
	if [ -n "$DPID" ]; then
		kill "$DPID"
		wait "$DPID" 2>/dev/null
	fi
	DPID=$new
	settle
}

"$B/WindowServer" --headless 1280x800 --socket "$SOCK" --config /dev/null -v \
    >"$LOG/server.log" 2>&1 &
SPID=$!
wait_for "$LOG/server.log" "ready:" 10 || { fail "server didn't start"; cat "$LOG/server.log"; exit 1; }
pass "server started (pid $SPID, service $WS_SERVICE_NAME)"

dock right 80
"$B/wsctl" screen | grep -q 'usable 0,0 1200x800' &&
    pass "a Dock on the right: usable 0,0 1200x800" ||
    { fail "usable area"; "$B/wsctl" screen; }

"$B/wsclear" >"$LOG/wsclear.log" 2>&1 &
CPID=$!
wait_for "$LOG/wsclear.log" "^ready" 10 || { fail "wsclear didn't get ready"; cat "$LOG/wsclear.log"; exit 1; }
pass "wsclear connected and created its windows"
mark

# ---- a window opened under the Dock ----
expect R1 900,300,300x200 "opened under the right Dock: placed beside it"
check "R1's app heard its frame" 0 "$M" \
    'type=window-frame win=R1 frame=900,300,300x200'
expect A1 100,0,400x300 "clear of the Dock: where its app put it"
expect D0 0,0,1280x800 "the desktop window covers the screen"
"$B/wsctl" snapshot "$LOG/dock-clear-1-right.ppm" >/dev/null

# ---- the Dock moves to the top ----
P0=$M
dock top 60
mark
P1=$M
"$B/wsctl" screen | grep -q 'usable 0,60 1280x740' &&
    pass "a Dock on the top: usable 0,60 1280x740" ||
    { fail "usable area"; "$B/wsctl" screen; }
expect A1 100,60,400x300 "pushed down below the Dock, as little as it takes"
expect U1 400,60,250x150 "pushed down too"
expect M1 20,60,150x200 "the torn-off menu pushed down"
expect P1 900,60,200x150 "the panel pushed down"
expect T1 700,60,250x740 "too tall for what's left: shrunk to fit"
expect B1 600,450,400x350 "clear of the Dock: not moved"
expect L1 0,300,300x200 "not moved"
expect R1 900,300,300x200 "not moved"
expect D0 0,0,1280x800 "the desktop window never moves"
"$B/wsctl" list | grep '"Z1"$' | grep -q 'frame=950,0,200x150' &&
    pass "the minimized Z1 isn't moved" ||
    fail "Z1 moved: $("$B/wsctl" list | grep '"Z1"$')"
check "A1's app heard its new frame" "$P0" "$P1" \
    'type=window-frame win=A1 frame=100,60,400x300'
check "T1's app heard its new size" "$P0" "$P1" \
    'type=window-frame win=T1 frame=700,60,250x740'
check_not "B1's app heard nothing" "$P0" "$P1" 'type=window-frame win=B1'
check_not "D0's app heard nothing" "$P0" "$P1" 'type=window-frame win=D0'
expect_pixel 300 61 c03030 "A1 drawn below the Dock"
"$B/wsctl" snapshot "$LOG/dock-clear-2-top.ppm" >/dev/null

# ---- a new window opens under it ----
"$B/wsctl" key n
mark
expect N1 300,60,300x200 "a new window asked for 300,0: placed below the Dock"

# ---- the user moves a pushed window ----
sleep 1.2	# past the time an owner has to settle its frame
"$B/wsctl" key u
mark
expect U1 420,200,250x150 "the user moved U1"

# ---- the Dock moves to the bottom ----
P2=$M
dock bottom 70
mark
P3=$M
expect A1 100,0,400x300 "the top is free again: A1 back where it was"
expect M1 20,10,150x200 "the torn-off menu back"
expect P1 900,10,200x150 "the panel back"
expect T1 700,0,250x730 "T1 from its old frame, shrunk to fit above the Dock"
expect B1 600,380,400x350 "pushed up above the Dock"
expect U1 420,200,250x150 "the user moved U1 meanwhile: it stays"
expect N1 300,60,300x200 "N1 was placed, not pushed: it stays"
check "B1's app heard its new frame" "$P2" "$P3" \
    'type=window-frame win=B1 frame=600,380,400x350'
check_not "U1's app heard nothing" "$P2" "$P3" 'type=window-frame win=U1'
"$B/wsctl" snapshot "$LOG/dock-clear-3-bottom.ppm" >/dev/null

# ---- back to the right ----
dock right 80
mark
expect A1 100,0,400x300 "A1 where it was"
expect B1 600,450,400x350 "B1 back down"
expect T1 700,5,250x790 "T1 back to its full height"
expect R1 900,300,300x200 "R1 where it was placed"
expect U1 420,200,250x150 "U1 where the user put it"
"$B/wsctl" snapshot "$LOG/dock-clear-4-back.ppm" >/dev/null

# ---- partly under the Dock is the user's choice ----
"$B/wsctl" key d
mark
expect R1 1100,300,300x200 "the user dragged R1 partly under the Dock: left there"

# ---- the Mac menu bar ----
P4=$M
"$B/wsctl" menubar 24 3 >"$LOG/menubar.log" 2>&1 &
MPID=$!
wait_for "$LOG/menubar.log" "menubar 24" 5 || fail "wsctl menubar"
mark
expect A1 100,24,400x300 "a Mac menu bar: A1 pushed below it"
expect M1 20,24,150x200 "the torn-off menu below it"
expect T1 700,24,250x776 "T1 below it, shrunk"
expect R1 1100,300,300x200 "R1 still where the user left it"
expect D0 0,0,1280x800 "the desktop window doesn't move"
"$B/wsctl" snapshot "$LOG/dock-clear-5-menubar.ppm" >/dev/null
wait "$MPID" 2>/dev/null
mark
expect A1 100,0,400x300 "the menu bar gone: A1 back"
expect T1 700,5,250x790 "T1 back"

# ---- the screen shrinks and grows again ----
"$B/wsctl" screen size 1024x700 >/dev/null || fail "wsctl screen size"
mark
expect B1 544,350,400x350 "a smaller screen: B1 pushed left of the Dock and up"
expect T1 694,0,250x700 "T1 moved left of the Dock and shrunk to the smaller screen"
"$B/wsctl" screen size 1280x800 >/dev/null || fail "wsctl screen size"
mark
expect B1 600,450,400x350 "the screen back: B1 where it was"
expect T1 700,5,250x790 "T1 where it was"

# ---- tiling: the floating windows ----
"$B/wsctl" tiling on || fail "wsctl tiling on"
settle
"$B/wsctl" list | grep '"A1"$' | grep -q ' tiled ' &&
    pass "tiling on: A1 tiled" || fail "A1 not tiled"
dock top 60
mark
expect P1 900,60,200x150 "tiling: the floating panel pushed below the Dock"
expect M1 20,60,150x200 "tiling: the torn-off menu too"
"$B/wsctl" tiling | grep -q 'area=8,68,1264x724' &&
    pass "the tiles keep clear of the Dock (tiling)" ||
    { fail "tiling area"; "$B/wsctl" tiling; }
"$B/wsctl" snapshot "$LOG/dock-clear-6-tiling.ppm" >/dev/null
dock right 80
mark
expect P1 900,10,200x150 "tiling: the panel back"
"$B/wsctl" tiling off || fail "wsctl tiling off"
settle

grep -q "kept clear" "$LOG/server.log" &&
    pass "the server logged the windows it moved" ||
    fail "nothing logged"

echo
if [ "$FAILS" -eq 0 ]; then
	echo "all dock-clear tests passed (logs in $LOG)"
	exit 0
fi
echo "$FAILS dock-clear test(s) failed (logs in $LOG)"
exit 1
