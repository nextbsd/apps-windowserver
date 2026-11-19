#!/bin/sh
# Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless test of several displays ("Displays" in WSProtocol.h): a
# display plugged in goes to the right of the main one, the desktop is
# the rectangle around them at the largest scale, the usable area stays
# the main display's, displays move (snapped against each other, the
# desktop's corner following), the main display changes, one mirrors
# another, a change made with confirm is kept, reverted, or goes back by
# itself, windows of a display that goes away move to the main display
# and go back when it comes back, closing the lid turns the built-in
# panel off while another display is on, and each set of displays keeps
# its arrangement. Runs wstest (four windows, see tools/wstest.c) and
# wsctl.
#
#   sh tests/displays-test.sh [BUILD_DIR]
#
# Needs no privileges and doesn't touch the console or the GPU.

B=${1:-build}
export WS_SERVICE_NAME="org.nextbsd.windowserver.dtest.$$"
export WS_BackgroundColor=202020
LOG="/tmp/ws-dtest-$$"
SOCK="$LOG/ws.sock"
mkdir -p "$LOG" || exit 1
# a store of its own: arrangements are kept per set of displays
export WS_DisplayStore="$LOG/displays.plist"
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

# displays: wsctl displays, saved in $LOG/displays.
displays() {
	"$B/wsctl" displays >"$LOG/displays" 2>&1
}

# field ID KEY: display ID's KEY= value.
field() {
	awk -v id="$1" -v k="$2=" '$1 == id { for (i = 1; i <= NF; i++)
	    if (index($i, k) == 1) { print substr($i, length(k) + 1) } }' \
	    "$LOG/displays"
}

# did CONNECTOR: the id of the display on CONNECTOR.
did() {
	awk -v c="$1" '$2 == c { print $1 }' "$LOG/displays"
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

expect_flag() {
	displays
	if awk -v id="$1" '$1 == id' "$LOG/displays" | grep -q -w -- "$2"; then
		pass "display $1 is $2 ($3)"
	else
		fail "display $1 isn't $2 ($3)"
		cat "$LOG/displays"
	fi
}

expect_count() {
	displays
	got=$(wc -l <"$LOG/displays" | tr -d ' ')
	if [ "$got" = "$1" ]; then
		pass "$1 display(s) ($2)"
	else
		fail "$got displays, expected $1 ($2)"
		cat "$LOG/displays"
	fi
}

expect_screen() {
	got=$("$B/wsctl" screen | awk '$1 == "screen" { print $2 }')
	if [ "$got" = "$1" ]; then
		pass "desktop $1 ($2)"
	else
		fail "desktop is $got, expected $1 ($2)"
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

# frame TITLE: the window's frame from wsctl list.
frame() {
	"$B/wsctl" list | awk -v t="\"$1\"" '$NF == t {
	    for (i = 1; i <= NF; i++) if ($i ~ /^frame=/) {
	    sub("frame=", "", $i); print $i } }'
}

expect_frame() {
	settle
	got=$(frame "$1")
	if [ "$got" = "$2" ]; then
		pass "$1 at $2 ($3)"
	else
		fail "$1 at '$got', expected $2 ($3)"
		"$B/wsctl" list
	fi
}

# The built-in display: 1280 x 800, 1x.
"$B/WindowServer" --headless 1280x800 --socket "$SOCK" --config /dev/null -v \
    >"$LOG/server.log" 2>&1 &
SPID=$!
wait_for "$LOG/server.log" "ready:" 10 || { fail "server didn't start"; cat "$LOG/server.log"; exit 1; }
pass "server started (pid $SPID)"

"$B/wstest" >"$LOG/wstest.log" 2>&1 &
TPID=$!
wait_for "$LOG/wstest.log" "W4" 10 || { fail "wstest didn't start"; exit 1; }
settle
W1=$(frame W1)
pass "W1 starts at $W1"

# ---- one display ----
expect_count 1 "the headless display"
displays
BUILTIN=$(did headless)
expect_flag "$BUILTIN" main "the only display is main"
expect_flag "$BUILTIN" builtin "the headless display counts as built in"
expect_field "$BUILTIN" frame "0,0,1280x800" "it is the desktop"
expect_screen 1280x800 "one display"

# ---- plug in a 4K monitor: 2x, to the right ----
"$B/wsctl" displays plug 3840x2160 600x340 TST-0001-00000001 || fail "plug"
settle
expect_count 2 "a monitor plugged in"
displays
EXT=$(awk '$4 == "TST-0001-00000001" { print $1 }' "$LOG/displays")
[ -n "$EXT" ] && pass "the monitor is display $EXT" || fail "no monitor in the list"
expect_field "$EXT" frame "1280,0,1920x1080" "right of the built-in one, looking like 1920x1080"
expect_flag "$EXT" hidpi "a 4K monitor of 600 mm is HiDPI"
expect_field "$EXT" scale 2 "the desktop is drawn at 2x for it"
expect_field "$BUILTIN" scale 2 "every display reports the desktop's scale"
expect_screen 3200x1080 "the desktop spans both"
expect_usable "0,0 1280x800" "the usable area stays the built-in display's"
expect_field "$EXT" usable "1280,0,1920x1080" "another display's usable area is all of it"
expect_frame W1 "$W1" "plugging in moved no window"
grep -q "WS_EVENT_DISPLAYS_CHANGED\|displays-changed" "$LOG/wstest.log" &&
    pass "clients heard the displays change" ||
    fail "no displays-changed event reached wstest"

# modes: looks-like sizes, then the panel's
"$B/wsctl" displays modes "$EXT" >"$LOG/modes"
grep -q "^looks-like 1920x1080 default current" "$LOG/modes" &&
    pass "the monitor's default looks like 1920x1080" ||
    { fail "no default 1920x1080"; cat "$LOG/modes"; }
grep -q "^looks-like 3840x2160" "$LOG/modes" &&
    pass "its native size is offered" || fail "no 3840x2160 looks-like"

# ---- the pointer can go there ----
"$B/wsctl" move 2500 900
settle
"$B/wsctl" move 5000 2000
settle
pass "pointer moved past the desktop's edge (kept on a display)"

# ---- move the monitor to the left of the built-in display ----
"$B/wsctl" displays "$EXT" origin -1920,0 || fail "origin"
settle
expect_field "$EXT" frame "0,0,1920x1080" "the monitor on the left; the desktop's corner is its"
expect_field "$BUILTIN" frame "1920,0,1280x800" "the built-in display moved right with the corner"
W1X=$(echo "$W1" | cut -d, -f1)
W1REST=$(echo "$W1" | cut -d, -f2-)
expect_frame W1 "$((W1X + 1920)),$W1REST" "windows moved with their display"
expect_usable "1920,0 1280x800" "the usable area followed the main display"

# snapping: asked for a place overlapping the built-in display (at
# 1920,0 now), it goes to the nearest place touching it: its left side
"$B/wsctl" displays "$EXT" origin 300,100 || fail "origin overlap"
settle
expect_field "$EXT" frame "0,100,1920x1080" "an overlapping place snapped to the built-in display's left side"
expect_field "$BUILTIN" frame "1920,0,1280x800" "the built-in display stayed"
# back to its right: x 3200 now
"$B/wsctl" displays "$EXT" origin 3200,0 || fail "origin back"
settle
expect_field "$BUILTIN" frame "0,0,1280x800" "back on the left"
expect_frame W1 "$W1" "windows back where they were"

# ---- the Dock, as on macOS: a right Dock goes to the outer edge of the
# rightmost display, a bottom one stays on the main display ----
"$B/wsctl" dock edge right 64 30 >/dev/null 2>&1 &
DOCKPID=$!
settle
sleep 0.5
expect_usable "0,0 1280x800" "a right Dock isn't on the main display with one to its right"
expect_field "$EXT" usable "1280,0,1856x1080" "the Dock's strip comes off the rightmost display"
kill "$DOCKPID" 2>/dev/null; wait "$DOCKPID" 2>/dev/null
settle
"$B/wsctl" dock edge bottom 64 30 >/dev/null 2>&1 &
DOCKPID=$!
settle
sleep 0.5
expect_usable "0,0 1280x736" "a bottom Dock is on the main display"
expect_field "$EXT" usable "1280,0,1920x1080" "the other display keeps all of itself"
kill "$DOCKPID" 2>/dev/null; wait "$DOCKPID" 2>/dev/null
settle
expect_usable "0,0 1280x800" "no Dock: all of the main display"

# ---- main display ----
"$B/wsctl" displays "$EXT" main || fail "main"
settle
expect_flag "$EXT" main "the monitor is main now"
expect_usable "1280,0 1920x1080" "the usable area is the new main display's"
"$B/wsctl" displays "$BUILTIN" main || fail "main back"
settle
expect_flag "$BUILTIN" main "the built-in display is main again"

# ---- looks like, with confirm: keep ----
"$B/wsctl" displays "$EXT" looks-like 2560x1440 confirm || fail "looks-like confirm"
settle
expect_field "$EXT" frame "1280,0,2560x1440" "looks like 2560x1440"
expect_flag "$EXT" "pending=[0-9]*ms" "the change is pending"
expect_screen 3840x1440 "the desktop grew"
"$B/wsctl" displays keep || fail "keep"
settle
displays
grep -q pending "$LOG/displays" && fail "still pending after keep" ||
    pass "kept: nothing pending"
expect_field "$EXT" frame "1280,0,2560x1440" "kept 2560x1440"

# ---- revert ----
"$B/wsctl" displays "$EXT" looks-like 3840x2160 confirm || fail "looks-like 2"
settle
expect_field "$EXT" frame "1280,0,3840x2160" "looks like 3840x2160 (pending)"
"$B/wsctl" displays revert || fail "revert"
settle
expect_field "$EXT" frame "1280,0,2560x1440" "reverted to 2560x1440"

# ---- goes back by itself ----
"$B/wsctl" displays "$EXT" looks-like default confirm || fail "looks-like default"
settle
expect_field "$EXT" frame "1280,0,1920x1080" "the default again (pending)"
echo "      (waiting for the 15 s revert)"
sleep 16
settle
expect_field "$EXT" frame "1280,0,2560x1440" "unkept, it went back by itself"
"$B/wsctl" displays "$EXT" looks-like default || fail "default"
settle
expect_field "$EXT" frame "1280,0,1920x1080" "the default, without confirm"

# ---- mirroring ----
"$B/wsctl" displays "$EXT" mirror "$BUILTIN" || fail "mirror"
settle
expect_flag "$EXT" "mirror-of=$BUILTIN" "the monitor mirrors the built-in display"
expect_field "$EXT" frame "0,0,1280x800" "a mirror shows its source's part"
expect_screen 1280x800 "the desktop is the built-in display's alone"
# optimize for the monitor: the built-in display mirrors it instead
"$B/wsctl" displays "$BUILTIN" mirror "$EXT" || fail "swap mirror"
settle
expect_flag "$BUILTIN" "mirror-of=$EXT" "the two swapped: the built-in display mirrors the monitor"
expect_field "$EXT" frame "0,0,1920x1080" "the monitor shows its own part, filling it"
expect_flag "$EXT" main "the source is the main display"
expect_screen 1920x1080 "the desktop is the monitor's shape"
# and back
"$B/wsctl" displays "$EXT" mirror "$BUILTIN" || fail "swap back"
settle
expect_flag "$EXT" "mirror-of=$BUILTIN" "swapped back"
expect_screen 1280x800 "the desktop is the built-in display's again"
"$B/wsctl" displays "$EXT" mirror off || fail "mirror off"
settle
expect_field "$BUILTIN" frame "0,0,1280x800" "the built-in display back on the left after mirroring"
expect_field "$EXT" frame "1280,0,1920x1080" "the monitor back on its right"
# optimized for the monitor, then mirroring off: places as before
"$B/wsctl" displays "$EXT" mirror "$BUILTIN" || fail "mirror again"
settle
"$B/wsctl" displays "$BUILTIN" mirror "$EXT" || fail "optimize for the monitor"
settle
"$B/wsctl" displays "$BUILTIN" mirror off || fail "mirror off after optimize"
settle
expect_field "$BUILTIN" frame "0,0,1280x800" "after optimizing for the monitor, the built-in display is back on the left"
expect_field "$EXT" frame "1280,0,1920x1080" "and the monitor on its right"
expect_flag "$BUILTIN" main "the main display from before mirroring is main again"
expect_field "$EXT" frame "1280,0,1920x1080" "extending again, back where it was"
expect_screen 3200x1080 "the desktop spans both again"

# ---- the lid ----
"$B/wsctl" displays lid closed || fail "lid closed"
settle
expect_count 1 "the lid closed: only the monitor is on"
expect_flag "$EXT" main "the monitor is main while the lid is closed"
expect_frame W1 "$W1" "W1 moved onto the monitor"
"$B/wsctl" displays lid open || fail "lid open"
settle
expect_count 2 "the lid opened: both are on"
expect_flag "$BUILTIN" main "the built-in display is main again"
expect_field "$BUILTIN" frame "0,0,1280x800" "in its place"
expect_frame W1 "$W1" "W1 went back to the built-in display"

# ---- unplug the display the windows are on ----
"$B/wsctl" displays unplug "$BUILTIN" || fail "unplug built-in"
settle
expect_count 1 "the built-in display unplugged"
expect_flag "$EXT" main "the monitor is main"
expect_field "$EXT" frame "0,0,1920x1080" "it is the desktop"
expect_screen 1920x1080 "the desktop is the monitor"
expect_frame W1 "$W1" "W1 moved onto the main display, at the same place"
# and back: the same display (its EDID) plugs in again
"$B/wsctl" displays plug 1280x800 0x0 headless-1280x800 || fail "replug"
settle
expect_count 2 "the built-in display back"
displays
BUILTIN=$(awk '$4 == "headless-1280x800" { print $1 }' "$LOG/displays")
expect_field "$BUILTIN" frame "0,0,1280x800" "it came back where it was (the set's arrangement)"
expect_frame W1 "$W1" "W1 is on it again"

# ---- unplug the monitor ----
"$B/wsctl" displays unplug "$EXT" || fail "unplug monitor"
settle
expect_count 1 "the monitor unplugged"
expect_screen 1280x800 "the desktop is the built-in display again"
expect_frame W1 "$W1" "windows stayed"

# ---- the set's arrangement is kept: the monitor plugged in again on the
# left, then unplugged and plugged back ----
"$B/wsctl" displays plug 3840x2160 600x340 TST-0001-00000001 || fail "plug again"
settle
displays
EXT=$(awk '$4 == "TST-0001-00000001" { print $1 }' "$LOG/displays")
"$B/wsctl" displays "$EXT" origin -1920,0 || fail "origin left again"
settle
"$B/wsctl" displays unplug "$EXT" || fail "unplug again"
settle
"$B/wsctl" displays plug 3840x2160 600x340 TST-0001-00000001 || fail "plug a third time"
settle
displays
EXT=$(awk '$4 == "TST-0001-00000001" { print $1 }' "$LOG/displays")
expect_field "$EXT" frame "0,0,1920x1080" "the monitor came back on the left"

# ---- Night Shift settings ----
"$B/wsctl" settings set NightShift On NightShiftTemperature 2700 &&
    pass "Night Shift on at 2700 K" || fail "Night Shift settings refused"
"$B/wsctl" settings set NightShiftSchedule 21:30-06:45 &&
    pass "a schedule" || fail "schedule refused"
"$B/wsctl" settings set NightShiftTemperature 900 2>/dev/null &&
    fail "900 K accepted" || pass "900 K refused"
"$B/wsctl" settings set NightShiftSchedule tonight 2>/dev/null &&
    fail "a bad schedule accepted" || pass "a bad schedule refused"

# ---- errors ----
"$B/wsctl" displays 999 main 2>/dev/null && fail "an unknown display took main" ||
    pass "an unknown display is refused"
"$B/wsctl" displays "$EXT" looks-like 7000x5000 2>/dev/null &&
    fail "a size too big accepted" || pass "a size the display can't show is refused"
"$B/wsctl" displays keep 2>/dev/null && fail "keep with nothing pending" ||
    pass "keep with nothing pending is refused"

kill -0 "$SPID" 2>/dev/null && pass "the server is still running" ||
    fail "the server died"

if [ "$FAILS" -ne 0 ]; then
	echo "displays test: $FAILS failed (logs in $LOG)"
	exit 1
fi
echo "displays test: all passed"
rm -rf "$LOG"
exit 0
