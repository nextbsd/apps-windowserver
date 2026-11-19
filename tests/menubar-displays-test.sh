#!/bin/sh
# Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless test of the menu bar on every display (apps-windowserver#100,
# "The menu bar" in WSProtocol.h), as on macOS: each display of the
# desktop has a strip that its usable area leaves out; the display with
# focus shows the live bar and the status items, the others a dimmed
# copy that WindowServer composites from the same buffers; a press on a
# copy moves the live bar there in the same frame and is the bar's (or
# the item's) press; the bar follows the key window and clicks; a
# mirror shows its source's strip; hotplug, a monitor on either side,
# the lid, the Dock on each edge, window tiling, a HiDPI monitor, and
# menu style switches (the Mac style to one without a bar and back)
# leave every strip right, with no bar across two displays, no copy
# left behind, and no strip ever showing the desktop.
#
# Runs wsmenubar (tools/wsmenubar.c) and drives it with wsctl, reading
# each display's own pixels (wsctl pixel -d).
#
#   sh tests/menubar-displays-test.sh [BUILD_DIR]
#
# Needs no privileges and doesn't touch the console or the GPU.

B=${1:-build}
export WS_SERVICE_NAME="org.nextbsd.windowserver.mbdtest.$$"
export WS_BackgroundColor=202020
LOG="/tmp/ws-mbdtest-$$"
SOCK="$LOG/ws.sock"
mkdir -p "$LOG" || exit 1
export WS_DisplayStore="$LOG/displays.plist"
FAILS=0
SPID=
PIDS=
DOCK=

fail() {
	echo "FAIL: $*"
	FAILS=$((FAILS + 1))
}

pass() {
	echo "ok:   $*"
}

cleanup() {
	for p in $PIDS $DOCK; do
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

# pixel [-d ID] X Y: the colour there, on the main display or display ID.
pixel() {
	if [ "$1" = -d ]; then
		"$B/wsctl" pixel -d "$2" "$3" "$4" | awk '{print $2}'
	else
		"$B/wsctl" pixel "$1" "$2" | awk '{print $2}'
	fi
}

# near A B: colours within 2 per channel (blending rounds).
near() {
	awk -v a="$1" -v b="$2" 'function h(s, i) { return ("0x" substr(s,
	    i, 2)) + 0 } BEGIN { if (length(a) != 6) exit 1
	    for (i = 1; i <= 5; i += 2) { d = h(a, i) - h(b, i)
	    if (d < -2 || d > 2) exit 1 } exit 0 }'
}

# expect_pixel [-d ID] X Y COLOUR WHY
expect_pixel() {
	if [ "$1" = -d ]; then
		got=$(pixel -d "$2" "$3" "$4")
		where="display $2 pixel $3,$4"
		shift 4
	else
		got=$(pixel "$1" "$2")
		where="pixel $1,$2"
		shift 2
	fi
	if near "$got" "$1"; then
		pass "$where is $1 ($2)"
	else
		fail "$where is $got, expected $1 ($2)"
	fi
}

# winfo TITLE FIELD: a field of TITLE's line in wsctl list.
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

displays() {
	"$B/wsctl" displays >"$LOG/displays" 2>&1
}

field() {
	awk -v id="$1" -v k="$2=" '$1 == id { for (i = 1; i <= NF; i++)
	    if (index($i, k) == 1) { print substr($i, length(k) + 1) } }' \
	    "$LOG/displays"
}

expect_field() {
	displays
	got=$(field "$1" "$2")
	if [ "$got" = "$3" ]; then
		pass "display $1 $2=$3 ($4)"
	else
		fail "display $1 $2 is '$got', expected $3 ($4)"
		cat "$LOG/displays"
	fi
}

expect_usable() {
	got=$("$B/wsctl" screen | awk '$1 == "usable" { print $2 " " $3 }')
	if [ "$got" = "$1" ]; then
		pass "usable area $1 ($2)"
	else
		fail "usable area is '$got', expected '$1' ($2)"
	fi
}

screen_field() {
	"$B/wsctl" screen | awk -v k="$1" '$1 == k { print $2 }'
}

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

# dock EDGE: a Dock on EDGE, 64 thick, for the rest of the test (or
# until the next call); "none" for no Dock.
dock() {
	[ -n "$DOCK" ] && { kill "$DOCK" 2>/dev/null; wait "$DOCK" 2>/dev/null; }
	DOCK=
	if [ "$1" != none ]; then
		"$B/wsctl" dock edge "$1" 64 600 >"$LOG/dock.log" 2>&1 &
		DOCK=$!
	fi
	sleep 0.3
	settle
}

# no_gap ID X: the strip of display ID (0: the main one) at X never
# shows the desktop while COMMAND runs ten times, sampled after each.
no_gap() {
	id=$1
	x=$2
	shift 2
	gaps=0
	n=0
	while [ $n -lt 10 ]; do
		"$@" >/dev/null 2>&1
		if [ "$id" = 0 ]; then
			got=$(pixel "$x" 10)
		else
			got=$(pixel -d "$id" "$x" 10)
		fi
		[ "$got" = 202020 ] && gaps=$((gaps + 1))
		n=$((n + 1))
	done
	[ $gaps = 0 ] && pass "10 x '$*': display $id's strip never shows the desktop" ||
	    fail "'$*': display $id's strip showed the desktop $gaps times"
}

# Colours: A's bar is d0d0d0 with c03030 titles at its left, the clock
# 404040. A copy is the bar at half over the bar's colour.
DIM_TITLE=c88080	# c03030 at 0.5 over d0d0d0
DIM_CLOCK=888888	# 404040 at 0.5 over d0d0d0
BODY=d0d0d0
DESK=202020

"$B/WindowServer" --headless 1280x800 --socket "$SOCK" --config /dev/null -v \
    >"$LOG/server.log" 2>&1 &
SPID=$!
wait_for "$LOG/server.log" "ready:" 10 || { fail "server didn't start"; cat "$LOG/server.log"; exit 1; }
pass "server started (pid $SPID, service $WS_SERVICE_NAME)"
displays
BUILTIN=$(awk 'NR == 1 { print $1 }' "$LOG/displays")

# ---- a monitor, no menu bar yet (the NeXT style) ----
"$B/wsctl" displays plug 1920x1080 0x0 TST-0002-00000001 || fail "plug"
settle
displays
EXT=$(awk '$4 == "TST-0002-00000001" { print $1 }' "$LOG/displays")
[ -n "$EXT" ] && pass "the monitor is display $EXT" || fail "no monitor"
expect_field "$EXT" frame "1280,0,1920x1080" "to the right"
expect_field "$EXT" usable "1280,0,1920x1080" "no strip without a Mac menu bar"
expect_pixel -d "$EXT" 50 10 $DESK "no strip drawn on the monitor"

# ---- the Mac style: a strip on each display ----
start A bar A d0d0d0 c03030
APID=$LAST
A=$CLIENT
ready A
start clock item clock 80 2000 404040
CLOCK=$LAST
ready clock
expect_usable "0,24 1280x776" "the built-in display leaves its strip out"
expect_field "$EXT" usable "1280,24,1920x1056" "the monitor leaves its own strip out"
expect_frame A-bar 0,0,1280x24 "the live bar on the main display, as wide as it"
expect_frame clock 1200,0,80x24 "the clock at the main display's right end"
expect_pixel 50 10 c03030 "the live bar's titles"
expect_pixel 1230 10 404040 "the live clock"
expect_pixel -d "$EXT" 50 10 $DIM_TITLE "the monitor: the bar's titles, dimmed"
expect_pixel -d "$EXT" 1000 10 $BODY "the monitor: the bar's body"
expect_pixel -d "$EXT" 1900 10 $DIM_CLOCK "the monitor: the clock, dimmed, at its right end"
expect_pixel -d "$EXT" 1800 10 $BODY "the monitor: the bar's colour where no item is"
expect_pixel -d "$EXT" 50 40 $DESK "the monitor below its strip: the desktop"

# a status item's buffer changes: its copy follows (apps draw once)
kill -USR1 "$CLOCK"
wait_for "$LOG/clock.log" "^widened" 5
settle
expect_frame clock 1120,0,160x24 "the clock twice as wide"
expect_pixel -d "$EXT" 1780 10 $DIM_CLOCK "its copy twice as wide too"

# ---- a press on the copy: the live bar moves there and gets it ----
"$B/wsctl" click 1330 10
settle
expect_frame A-bar 1280,0,1920x24 "the live bar on the monitor, as wide as it"
expect_frame clock 3040,0,160x24 "the clock at the monitor's right end"
tail -n 3 "$LOG/A.log" | grep -q "mouse-down bar" &&
    pass "the press went to the live bar" ||
    { fail "the bar didn't get the press"; tail -5 "$LOG/A.log"; }
awk '/frame bar 1280,0,1920x24/ { f = NR } /mouse-down bar/ { m = NR }
    END { exit !(f && m && f < m) }' "$LOG/A.log" &&
    pass "the bar heard its new frame before the press" ||
    fail "frame and press out of order"
expect_pixel -d "$EXT" 50 10 c03030 "the monitor: the live bar"
expect_pixel 50 10 $DIM_TITLE "the built-in display: the dimmed copy"
expect_pixel 1250 10 $DIM_CLOCK "the built-in display: the clock's copy"
expect_usable "0,24 1280x776" "the usable areas don't move with focus"
expect_field "$EXT" usable "1280,24,1920x1056" "nor the monitor's"

# a status item's copy: its owner gets the press, on that display
"$B/wsctl" click 1250 10
settle
expect_frame clock 1120,0,160x24 "the clock back on the built-in display"
tail -n 3 "$LOG/clock.log" | grep -q "mouse-down item" &&
    pass "the press on the clock's copy went to the clock" ||
    { fail "the clock didn't get the press"; tail -5 "$LOG/clock.log"; }
expect_frame A-bar 0,0,1280x24 "the bar came with it"
"$B/wsctl" click 600 600		# the desktop: the item's popup ends
settle

# switching back and forth never leaves either strip on the desktop
no_gap "$EXT" 50 "$B/wsctl" click 1330 10
no_gap 0 50 "$B/wsctl" click 50 10
no_gap "$EXT" 1000 "$B/wsctl" click 2000 500
no_gap 0 1000 "$B/wsctl" click 600 600
gaps=0
for n in 1 2 3 4 5; do
	"$B/wsctl" click 1330 10 >/dev/null
	[ "$(pixel 50 10)" = $DESK ] && gaps=$((gaps + 1))
	[ "$(pixel -d "$EXT" 50 10)" = $DESK ] && gaps=$((gaps + 1))
	"$B/wsctl" click 50 10 >/dev/null
	[ "$(pixel 50 10)" = $DESK ] && gaps=$((gaps + 1))
	[ "$(pixel -d "$EXT" 50 10)" = $DESK ] && gaps=$((gaps + 1))
done
[ $gaps = 0 ] && pass "10 focus moves: no strip ever shows the desktop" ||
    fail "$gaps samples showed the desktop in a strip"

# ---- a click on the desktop or a window gives its display focus ----
"$B/wsctl" click 2000 500
settle
expect_frame A-bar 1280,0,1920x24 "a click on the monitor's desktop: the bar there"
"$B/wsctl" click 350 350		# A's document
settle
expect_frame A-bar 0,0,1280x24 "a click on A's window: the bar on its display"

# ---- the bar follows the key window ----
export WSMENUBAR_DOC=2000,400
start D bar D e0e0e0 3050c0
unset WSMENUBAR_DOC
D=$CLIENT
ready D
expect_frame D-bar 1280,0,1920x24 "D's key window is on the monitor: its bar there"
expect_pixel -d "$EXT" 50 10 3050c0 "the monitor: D's live bar"
expect_pixel 50 10 989090 "the built-in display: D's bar dimmed (3050c0 over e0e0e0)"
"$B/wsctl" dock activate "$A" >/dev/null
settle
expect_frame A-bar 0,0,1280x24 "A activated: its key window and bar on the built-in display"
expect_pixel -d "$EXT" 50 10 $DIM_TITLE "the monitor: A's bar dimmed"
no_gap "$EXT" 50 "$B/wsctl" dock activate "$D"
no_gap 0 50 "$B/wsctl" dock activate "$A"

# ---- a window opened under the monitor's strip goes below it ----
export WSMENUBAR_DOC=1600,0
start E plain E 30c030
unset WSMENUBAR_DOC
ready E
expect_frame E-doc 1600,24,200x150 "placed below the monitor's strip"
"$B/wsctl" dock activate "$A" >/dev/null
settle

# ---- the monitor on the left ----
"$B/wsctl" displays "$EXT" origin -1920,0 || fail "origin left"
settle
expect_field "$EXT" frame "0,0,1920x1080" "the monitor on the left"
expect_field "$BUILTIN" frame "1920,0,1280x800" "the built-in display right of it"
expect_usable "1920,24 1280x776" "the main display's usable area"
expect_field "$EXT" usable "0,24,1920x1056" "the monitor's"
expect_frame A-bar 1920,0,1280x24 "the live bar on the main display"
expect_pixel -d "$EXT" 50 10 $DIM_TITLE "the copy on the left monitor"
"$B/wsctl" click 50 10
settle
expect_frame A-bar 0,0,1920x24 "a press on its copy: the bar on the left monitor"
expect_pixel 50 10 $DIM_TITLE "the built-in display shows the copy"

# ---- the Dock on each edge ----
dock left
expect_field "$EXT" usable "64,24,1856x1056" "a left Dock is on the left monitor, below its strip"
expect_usable "1920,24 1280x776" "the built-in display keeps its strip only"
dock bottom
expect_usable "1920,24 1280x712" "a bottom Dock on the main display"
expect_field "$EXT" usable "0,24,1920x1056" "the monitor: its strip only"
dock right
expect_usable "1920,24 1216x776" "a right Dock on the built-in display (the rightmost)"
expect_field "$EXT" usable "0,24,1920x1056" "the monitor: its strip only"
dock none
"$B/wsctl" displays "$EXT" origin 3200,0 || fail "origin right"
settle
dock right
expect_field "$EXT" usable "1280,24,1856x1056" "a right Dock on the right monitor, below its strip"
expect_usable "0,24 1280x776" "the main display: its strip only"
dock left
expect_usable "64,24 1216x776" "a left Dock on the built-in display"
dock bottom
expect_usable "0,24 1280x712" "a bottom Dock"
dock none

# ---- the main display changes ----
"$B/wsctl" click 600 600
settle
"$B/wsctl" displays "$EXT" main || fail "main"
settle
expect_usable "1280,24 1920x1056" "the monitor is the main display: its usable area"
expect_field "$BUILTIN" usable "0,24,1280x776" "the built-in display keeps its strip"
expect_frame A-bar 0,0,1280x24 "focus stays on the built-in display"
"$B/wsctl" displays "$BUILTIN" main || fail "main back"
settle

# ---- tiling ----
"$B/wsctl" settings set WindowMode tiling >/dev/null || fail "tiling"
settle
expect_usable "0,24 1280x776" "tiling: the usable area"
expect_field "$EXT" usable "1280,24,1920x1056" "tiling: the monitor's"
expect_pixel -d "$EXT" 50 10 $DIM_TITLE "tiling: the copy"
"$B/wsctl" click 1330 10
settle
expect_frame A-bar 1280,0,1920x24 "tiling: a press on the copy moves the bar"
"$B/wsctl" settings set WindowMode stacking >/dev/null || fail "stacking"
settle
expect_frame A-bar 1280,0,1920x24 "back to stacking: the bar stays"
expect_pixel 50 10 $DIM_TITLE "and the copy"

# ---- mirroring ----
"$B/wsctl" displays "$EXT" mirror "$BUILTIN" || fail "mirror"
settle
expect_frame A-bar 0,0,1280x24 "mirrored: the bar back on the source"
expect_frame clock 1120,0,160x24 "and the clock"
expect_field "$EXT" usable "0,0,1280x800" "a mirror has no strip of its own"
expect_usable "0,24 1280x776" "the source's usable area"
expect_pixel 50 10 c03030 "the source shows the live bar, not a copy"
# the mirror shows the source's part, 1280x800 in 1728x1080 at 96,0
expect_pixel -d "$EXT" 200 13 c03030 "the mirror shows the live bar"
expect_pixel -d "$EXT" 1500 13 $BODY "the bar's body on the mirror"
"$B/wsctl" click 50 10
settle
expect_frame A-bar 0,0,1280x24 "a press on the mirrored bar: the bar stays"
"$B/wsctl" displays "$EXT" mirror off || fail "mirror off"
settle
expect_field "$EXT" usable "1280,24,1920x1056" "mirroring off: the monitor's strip again"
expect_pixel -d "$EXT" 50 10 $DIM_TITLE "and its copy"

# ---- hotplug ----
"$B/wsctl" click 1330 10
settle
expect_frame A-bar 1280,0,1920x24 "the bar on the monitor"
"$B/wsctl" displays unplug "$EXT" || fail "unplug"
settle
expect_frame A-bar 0,0,1280x24 "unplugged: the bar back on the built-in display"
expect_frame clock 1120,0,160x24 "and the clock"
expect_pixel 50 10 c03030 "the live bar, not a copy"
expect_usable "0,24 1280x776" "the built-in display's usable area"
"$B/wsctl" displays plug 1920x1080 0x0 TST-0002-00000001 || fail "replug"
settle
displays
EXT=$(awk '$4 == "TST-0002-00000001" { print $1 }' "$LOG/displays")
expect_field "$EXT" usable "1280,24,1920x1056" "plugged in again: its strip"
expect_pixel -d "$EXT" 50 10 $DIM_TITLE "and the copy"
expect_frame A-bar 0,0,1280x24 "the live bar stays on the built-in display"

# ---- the lid ----
"$B/wsctl" displays lid closed || fail "lid closed"
settle
displays
expect_frame A-bar 0,0,1920x24 "lid closed: the bar on the monitor (the desktop's corner now)"
expect_pixel 50 10 c03030 "the monitor (main now) shows the live bar"
"$B/wsctl" displays lid open || fail "lid open"
settle
expect_field "$BUILTIN" usable "0,24,1280x776" "lid open: the built-in display's strip"
expect_frame A-bar 1280,0,1920x24 "the bar stays where focus is"
"$B/wsctl" click 600 600
settle
expect_frame A-bar 0,0,1280x24 "a click on the built-in display: the bar there"

# ---- a menu style switch: the Mac style and back ----
kill -HUP "$APID"
wait_for "$LOG/A.log" "^style next" 5
settle
expect_usable "0,0 1280x800" "no bar: no strip on the built-in display"
expect_field "$EXT" usable "1280,0,1920x1080" "nor on the monitor"
[ -z "$(winfo clock frame)" ] && pass "no strip: the items aren't shown" ||
    fail "the clock still shows"
expect_pixel 50 10 $DESK "the built-in display's strip is gone"
expect_pixel -d "$EXT" 50 10 $DESK "no copy is left on the monitor"
expect_pixel -d "$EXT" 1900 10 $DESK "nor of the clock"
kill -HUP "$APID"
wait_for "$LOG/A.log" "^style mac" 5
settle
expect_usable "0,24 1280x776" "the Mac style again: the strip"
expect_field "$EXT" usable "1280,24,1920x1056" "on the monitor too"
expect_pixel -d "$EXT" 50 10 $DIM_TITLE "and its copy"
expect_frame clock 1120,0,160x24 "the clock back"

# ---- a HiDPI monitor ----
"$B/wsctl" displays unplug "$EXT" || fail "unplug"
"$B/wsctl" displays plug 3840x2160 600x340 TST-0003-00000001 || fail "plug 4K"
settle
displays
K=$(awk '$4 == "TST-0003-00000001" { print $1 }' "$LOG/displays")
expect_field "$K" usable "1280,24,1920x1056" "the 4K monitor (2x): its strip in points"
expect_pixel -d "$K" 100 20 $DIM_TITLE "its copy, at 2x"
expect_pixel -d "$K" 3800 20 $DIM_CLOCK "the clock's copy at its right end"
"$B/wsctl" click 1330 10
settle
expect_frame A-bar 1280,0,1920x24 "a press on it: the bar there"
expect_pixel -d "$K" 100 20 c03030 "the live bar at 2x"

# ---- the bar's app quits: the strips go ----
kill "$APID"
for p in $PIDS; do kill "$p" 2>/dev/null; done
sleep 0.5
settle
expect_usable "0,0 1280x800" "no app with a bar: no strip"
expect_field "$K" usable "1280,0,1920x1080" "nor on the monitor"
expect_pixel -d "$K" 100 20 $DESK "no copy left"

grep -q "AddressSanitizer\|assertion" "$LOG/server.log" &&
    fail "the server log has errors"
kill -0 "$SPID" 2>/dev/null && pass "server still running" ||
    fail "server died"

echo
if [ $FAILS -eq 0 ]; then
	echo "all menu-bar-on-every-display tests passed"
	rm -rf "$LOG"
	exit 0
fi
echo "$FAILS failure(s); logs in $LOG"
exit 1
