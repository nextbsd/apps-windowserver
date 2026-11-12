#!/bin/sh
# Copyright (c) 2026, Joseph Maloney
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless end-to-end test: starts WindowServer without DRM, VT or
# libinput, runs wstest (two apps and a joined connection, four
# windows), then uses wsctl to check the composited screen and drive
# synthetic input, including minimizing a window to a tile and
# restoring it, and the Dock's window list (wsctl dock), with minimized
# windows handed to the Dock instead of tiles (MinimizeIntoDock, and
# per request with WS_MINIMIZE_INTO_DOCK or WS_MINIMIZE_BOTTOM_ROW), and
# the screen edge the Dock reports (WS_MSG_DOCK_SET_EDGE), touchpad
# scroll phases and momentum scrolling, and the window overview (Exposé).
#
#   sh tests/headless-test.sh [BUILD_DIR]
#
# Needs no privileges and doesn't touch the console or the GPU.

B=${1:-build}
export WS_SERVICE_NAME="org.nextbsd.windowserver.test.$$"
export WS_SnapshotPath="/tmp/ws-test-$$-signal.ppm"
# Not black, so a window drawn with the wrong alpha would show.
export WS_BackgroundColor=202020
# Tiles until a Dock connects, then the Dock shows minimized windows.
export WS_MinimizeIntoDock=true
LOG="/tmp/ws-test-$$"
SOCK="$LOG/ws.sock"
mkdir -p "$LOG" || exit 1
FAILS=0
SPID=
TPID=
DPID=
EPID=
WPID=

fail() {
	echo "FAIL: $*"
	FAILS=$((FAILS + 1))
}

pass() {
	echo "ok:   $*"
}

cleanup() {
	[ -n "$DPID" ] && kill "$DPID" 2>/dev/null
	[ -n "$EPID" ] && kill "$EPID" 2>/dev/null
	[ -n "$WPID" ] && kill "$WPID" 2>/dev/null
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

# ppm_pixel FILE X Y: the colour at X,Y of a binary PPM wsctl wrote
# ("P6\nW H\n255\n").
ppm_pixel() {
	_w=$(head -c 32 "$1" | awk 'NR == 2 { print $1 }')
	_hl=$(head -c 32 "$1" | awk 'NR <= 3 { n += length($0) + 1 } END { print n }')
	od -An -tx1 -j $((_hl + ($3 * _w + $2) * 3)) -N3 "$1" | tr -d ' \n'
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
expect_pixel 1000 700 202020 "desktop background"
# W4 is XRGB8888 with alpha byte 0: opaque, without WS_WINDOW_OPAQUE.
expect_pixel 1000 200 c0c030 "XRGB8888 window W4 opaque"
expect_pixel 901 101 606018 "XRGB8888 window W4 border"
mark
P0=$MARKS

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

# Minimizing (WS_SERVER_MINIMIZE). Beta's W3 becomes a 64x64 tile in
# the bottom-left corner: a dark rounded tile (face 303030) with W3
# (500x400) shrunk to 48x38 at 8,13 inside it, and Beta's magenta icon
# (24x24) in its bottom-right corner, at 39,39.
TILE_Y=736
"$B/wsctl" click 700 600 || fail "wsctl click W3 (Beta)"
"$B/wsctl" key m || fail "wsctl key m"
mark
P5=$MARKS
in_part "$P4" "$P5" '^minimizing W3' && in_part "$P4" "$P5" 'app=Beta type=key-window-lost win=W3' &&
    pass "minimizing W3 took its key window away" || fail "minimize W3"
# (Pixels are checked away from the pointer: the software cursor is
# drawn at it.)
expect_pixel 690 590 202020 "minimized W3 is off the screen"
expect_pixel 4 $((TILE_Y + 24)) 303030 "tile face"
expect_pixel 20 $((TILE_Y + 24)) 3050c0 "tile shows W3's picture"
expect_pixel 56 $((TILE_Y + 56)) ff00ff "tile has Beta's icon in its corner"
expect_pixel 70 $((TILE_Y + 24)) 202020 "tile is 64 pixels wide"
"$B/wsctl" list >"$LOG/list5.txt"
cat "$LOG/list5.txt"
grep '"W3"$' "$LOG/list5.txt" | grep -q ' minimized ' &&
    pass "wsctl list shows W3 minimized" || fail "W3 not listed as minimized"
# Live: Beta redraws W3 while it's minimized, and the tile follows.
"$B/wsctl" key c || fail "wsctl key c"
mark
expect_pixel 20 $((TILE_Y + 24)) 20a050 "tile updated on W3's commit"
# One click selects the tile (lighter face) and goes to no client.
"$B/wsctl" click 4 $((TILE_Y + 24)) || fail "wsctl click tile"
got=$("$B/wsctl" pixel 3 $((TILE_Y + 19)) | awk '{print $2}')
[ "$got" != 303030 ] && pass "clicked tile selected ($got)" || fail "tile not selected"
# Drag it by 200,-100.
"$B/wsctl" move 20 $((TILE_Y + 24)) && "$B/wsctl" down &&
    "$B/wsctl" move 120 $((TILE_Y - 26)) && "$B/wsctl" move 220 $((TILE_Y - 76)) &&
    "$B/wsctl" up || fail "wsctl drag tile"
expect_pixel 20 $((TILE_Y + 24)) 202020 "tile dragged away"
expect_pixel 215 $((TILE_Y - 81)) 20a050 "tile dragged to its new place"
# Activate Alpha, then double-click the tile: Beta is activated and
# told to restore W3, which it does by ordering it in.
"$B/wsctl" click 150 450 || fail "wsctl click W1"
mark
P6=$MARKS
"$B/wsctl" click 220 $((TILE_Y - 76)) && "$B/wsctl" click 220 $((TILE_Y - 76)) ||
    fail "wsctl double-click tile"
mark
P7=$MARKS
in_part "$P6" "$P7" 'app=Beta type=app-activated' &&
    in_part "$P6" "$P7" 'app=Beta type=window-restore win=W3' &&
    pass "double-click on the tile activated Beta and sent window-restore" ||
    fail "no window-restore after a double-click"
in_part "$P5" "$P7" 'app=Beta type=mouse-' &&
    fail "clicks on the tile reached Beta" || pass "clicks on the tile went to no client"
expect_pixel 690 590 20a050 "W3 restored, with what it drew while minimized"
expect_pixel 215 $((TILE_Y - 81)) 202020 "tile gone after the restore"
"$B/wsctl" list | grep '"W3"$' | grep -q ' active key ' &&
    pass "W3 is key again" || fail "W3 not key after restoring"

# The Dock (WS_SERVER_DOCK). Apps can't list every app's windows;
# a Dock connection can, with ids.
grep -q 'list refused app=Alpha EPERM' "$LOG/wstest.log" &&
    pass "an app can't list other apps' windows (EPERM)" || fail "an app listed every window"
"$B/wsctl" dock list >"$LOG/dock-list.txt" || fail "wsctl dock list"
cat "$LOG/dock-list.txt"
W3ID=$(awk '/"W3"$/ { sub("id=", "", $1); print $1 }' "$LOG/dock-list.txt")
ALPHA=$(awk '/"W1"$/ { sub("app=", "", $2); print $2 }' "$LOG/dock-list.txt")
grep '"W4"$' "$LOG/dock-list.txt" | grep -q " app=$ALPHA " &&
    pass "dock list: W4 (Gamma) is in Alpha's app ($ALPHA)" || fail "dock list app ids"
grep '"W3"$' "$LOG/dock-list.txt" | grep -q ' visible active key ' &&
    pass "dock list: W3 visible, active and key" || fail "dock list flags"
"$B/wsctl" dock list -a | grep -q '"W3"$' && pass "dock list -a" || fail "dock list -a"
# While a Dock is connected, minimized windows go into it: no tile.
"$B/wsctl" dock watch 60 >"$LOG/dock-watch.log" 2>&1 &
DPID=$!
wait_for "$LOG/dock-watch.log" "^state flags=0x1 minimized-windows" 5 &&
    pass "the Dock was handed the minimized windows" || fail "no dock-state"
"$B/wsctl" key m || fail "wsctl key m (with a Dock)"
mark
expect_pixel 690 590 202020 "W3 minimized again"
expect_pixel 4 $((TILE_Y + 24)) 202020 "no tile while the Dock shows minimized windows"
wait_for "$LOG/dock-watch.log" "id=$W3ID .* minimized in-dock" 5 &&
    pass "the Dock heard the list change: W3 minimized, in the Dock" ||
    fail "no list-changed with W3 in the Dock"
"$B/wsctl" key c || fail "wsctl key c (with a Dock)"
wait_for "$LOG/dock-watch.log" "^preview-changed window=$W3ID" 5 &&
    pass "W3's commit while minimized: preview-changed" || fail "no preview-changed"
"$B/wsctl" dock preview "$W3ID" 100 "$LOG/preview.ppm" | grep -q ': 100x80$' &&
    pass "preview of W3 scaled to 100x80" || fail "preview size"
got=$(ppm_pixel "$LOG/preview.ppm" 50 40)
[ "$got" = 20a050 ] && pass "preview shows W3's last frame ($got)" || fail "preview pixel is $got, expected 20a050"
mark
P8=$MARKS
"$B/wsctl" dock activate "$ALPHA" || fail "wsctl dock activate"
mark
P9=$MARKS
in_part "$P8" "$P9" 'app=Alpha type=app-activated' &&
    in_part "$P8" "$P9" 'app=Beta type=app-deactivated' &&
    pass "the Dock activated Alpha" || fail "dock activate"
"$B/wsctl" dock restore "$W3ID" || fail "wsctl dock restore"
mark
P10=$MARKS
in_part "$P9" "$P10" 'app=Beta type=app-activated' &&
    in_part "$P9" "$P10" 'app=Beta type=window-restore win=W3' &&
    pass "the Dock's restore activated Beta and sent window-restore" || fail "dock restore"
expect_pixel 690 590 20a050 "W3 restored from the Dock"
# The Dock goes away while W3 is minimized: its tile comes back.
"$B/wsctl" key m || fail "wsctl key m (before the Dock quits)"
mark
expect_pixel 4 $((TILE_Y + 24)) 202020 "still no tile"
kill "$DPID"
wait "$DPID" 2>/dev/null
DPID=
"$B/wsctl" sync
expect_pixel 4 $((TILE_Y + 24)) 303030 "tile back once the Dock is gone"
expect_pixel 20 $((TILE_Y + 24)) 20a050 "the tile shows W3"
grep -q '^error' "$LOG/dock-watch.log" && fail "the Dock got errors"

# Where a window goes (WS_SERVER_MINIMIZE_WHERE): each minimize request
# may say, whatever MinimizeIntoDock (on here) says. Typing "r" in Beta
# minimizes onto the bottom row, "d" into the Dock; with W3 already
# minimized, it moves there.
"$B/wsctl" key r || fail "wsctl key r (W3 minimized, no Dock)"
mark
expect_pixel 4 $((TILE_Y + 24)) 303030 "W3 onto the bottom row: still its tile"
"$B/wsctl" dock watch 60 >"$LOG/dock-where.log" 2>&1 &
DPID=$!
wait_for "$LOG/dock-where.log" "^state flags=0x1 minimized-windows" 5 ||
    fail "no dock-state for the second Dock"
"$B/wsctl" sync
expect_pixel 4 $((TILE_Y + 24)) 303030 "a Dock connected: W3 keeps its bottom-row tile"
"$B/wsctl" dock list | grep '"W3"$' | grep ' minimized ' | grep -qv 'in-dock' &&
    pass "dock list: W3 minimized, not in the Dock" || fail "W3 listed in the Dock"
"$B/wsctl" key d || fail "wsctl key d (W3 minimized, with a Dock)"
mark
expect_pixel 4 $((TILE_Y + 24)) 202020 "W3 moved from its tile into the Dock"
wait_for "$LOG/dock-where.log" "id=$W3ID .* minimized in-dock" 5 &&
    pass "the Dock heard W3 move into it" || fail "W3 not listed in the Dock"
"$B/wsctl" dock restore "$W3ID" || fail "wsctl dock restore (where)"
mark
expect_pixel 690 590 20a050 "W3 restored from the Dock"
"$B/wsctl" key r || fail "wsctl key r (with a Dock)"
mark
expect_pixel 690 590 202020 "W3 minimized onto the bottom row"
expect_pixel 4 $((TILE_Y + 24)) 303030 "onto the bottom row with a Dock connected: a tile"
"$B/wsctl" dock list | grep '"W3"$' | grep ' minimized ' | grep -qv 'in-dock' &&
    pass "dock list: W3 on the bottom row, not in the Dock" ||
    fail "a bottom-row window went into the Dock"
"$B/wsctl" dock restore "$W3ID" || fail "wsctl dock restore (bottom row)"
mark
expect_pixel 4 $((TILE_Y + 24)) 202020 "tile gone after the restore"
"$B/wsctl" key d || fail "wsctl key d (with a Dock)"
mark
expect_pixel 690 590 202020 "W3 minimized into the Dock"
expect_pixel 4 $((TILE_Y + 24)) 202020 "into the Dock: no tile"
kill "$DPID"
wait "$DPID" 2>/dev/null
DPID=
"$B/wsctl" sync
expect_pixel 4 $((TILE_Y + 24)) 303030 "the Dock gone: W3 back onto the bottom row"
grep -q '^error' "$LOG/dock-where.log" && fail "the second Dock got errors"
# With no Dock connected, minimizing into the Dock gives a tile.
"$B/wsctl" dock restore "$W3ID" || fail "wsctl dock restore (no Dock)"
mark
expect_pixel 690 590 20a050 "W3 restored"
"$B/wsctl" key d || fail "wsctl key d (no Dock)"
mark
expect_pixel 690 590 202020 "W3 minimized into the Dock with no Dock"
expect_pixel 4 $((TILE_Y + 24)) 303030 "no Dock: onto the bottom row instead"
expect_pixel 20 $((TILE_Y + 24)) 20a050 "the tile shows W3"

# The Dock's edge (WS_SERVER_DOCK_EDGE): a Dock says which screen edge
# it keeps and how thick it is; every Dock connection hears it and
# `wsctl dock list` shows it. The newest report counts while its Dock is
# connected; when that Dock goes, the one before it counts again.
# dock_edge_is EDGE THICKNESS: within two seconds
dock_edge_is() {
	i=0
	while [ $i -lt 20 ]; do
		"$B/wsctl" dock list | grep -q "^dock edge=$1 thickness=$2 " &&
		    return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}
dock_edge_is none 0 && pass "dock list: no Dock edge yet" ||
    fail "dock list: an edge before any Dock said so"
"$B/wsctl" dock watch 60 >"$LOG/dock-edge-watch.log" 2>&1 &
WPID=$!
wait_for "$LOG/dock-edge-watch.log" "^watching" 5 || fail "no dock watch"
"$B/wsctl" dock edge right 84 60 >"$LOG/dock-edge.log" 2>&1 &
EPID=$!
wait_for "$LOG/dock-edge.log" "^edge=right thickness=84 " 5 &&
    pass "the Dock reported the right edge, 84 pixels" ||
    fail "no dock-state with the right edge"
dock_edge_is right 84 && pass "dock list shows the Dock's edge" ||
    fail "dock list doesn't show the right edge"
wait_for "$LOG/dock-edge-watch.log" "^state .* edge=right thickness=84$" 5 &&
    pass "another Dock connection heard the edge" ||
    fail "the watching Dock didn't hear the edge"
"$B/wsctl" dock edge bottom 90 | grep -q "^edge=bottom thickness=90 " &&
    pass "a newer report counts (bottom, 90)" || fail "dock edge bottom 90"
dock_edge_is right 84 && pass "back to the right edge when that Dock went" ||
    fail "the edge didn't fall back to the right"
"$B/wsctl" dock edge top 100000 2>/dev/null &&
    fail "a Dock thicker than half the screen was accepted" ||
    pass "a Dock thicker than half the screen is refused"
dock_edge_is right 84 && pass "a refused report changes nothing" ||
    fail "a refused report changed the edge"
kill "$EPID"
wait "$EPID" 2>/dev/null
EPID=
dock_edge_is none 0 && pass "no edge once the Dock is gone" ||
    fail "the edge stayed after the Dock went"
# (after the right edge: its first state, at the welcome, said none too)
i=0
until sed -n '/edge=right/,$p' "$LOG/dock-edge-watch.log" |
    grep -q '^state .* edge=none thickness=0$' || [ $i -ge 50 ]; do
	sleep 0.1
	i=$((i + 1))
done
[ $i -lt 50 ] &&
    pass "the watching Dock heard the edge go" ||
    fail "the watching Dock didn't hear the edge go"
kill "$WPID"
wait "$WPID" 2>/dev/null
WPID=
grep -q '^error' "$LOG/dock-edge-watch.log" && fail "the watching Dock got errors"

# Touchpad scrolling (WS_SERVER_SCROLL_PHASES): a two-finger flick over
# W1 (where nothing covers it) is began, changed..., ended when the
# fingers lift, then momentum began, changed... and ended.
"$B/wsctl" move 150 450 || fail "wsctl move (scroll)"
mark
PS0=$MARKS
"$B/wsctl" flick 0 400 8 || fail "wsctl flick"
sleep 3
mark
PS1=$MARKS
awk -v from="$PS0" -v to="$PS1" 'NR > from && NR <= to && / type=scroll /' \
    "$LOG/wstest.log" >"$LOG/flick.txt"
sed -n '1,3p;$p' "$LOG/flick.txt"
phases=$(awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^(phase|momentum)=/) \
    printf "%s ", $i; print "" }' "$LOG/flick.txt" | uniq | tr '\n' '|')
echo "phases: $phases"
[ "$phases" = "phase=began momentum=none |phase=changed momentum=none |phase=ended momentum=none |phase=none momentum=began |phase=none momentum=changed |phase=none momentum=ended |" ] &&
    pass "flick: began, changed, ended, then momentum began, changed, ended" ||
    fail "flick phases"
grep -q 'app=Alpha type=scroll win=W1 dx=0.00 dy=50.00 source=1 phase=began' "$LOG/flick.txt" &&
    pass "finger scroll delivered to W1 with its delta" || fail "finger scroll delta"
grep 'phase=ended' "$LOG/flick.txt" | grep -q 'dx=0.00 dy=0.00 ' &&
    grep 'momentum=ended' "$LOG/flick.txt" | grep -q 'dx=0.00 dy=0.00 ' &&
    pass "ended events carry no delta" || fail "ended events with a delta"
n=$(grep -c 'momentum=changed' "$LOG/flick.txt")
total=$(awk '/momentum=(began|changed)/ { sub("dy=", "", $7); t += $7 } END { printf "%d", t }' "$LOG/flick.txt")
first=$(awk '/momentum=began/ { sub("dy=", "", $7); print $7 }' "$LOG/flick.txt")
last=$(awk '/momentum=changed/ { sub("dy=", "", $7); d = $7 } END { print d }' "$LOG/flick.txt")
echo "momentum: $n steps, $total pixels, first step $first, last step $last"
[ "$n" -ge 20 ] && [ "$total" -gt 200 ] &&
    awk -v a="$first" -v b="$last" 'BEGIN { exit !(a > 0 && b > 0 && b < a) }' &&
    pass "momentum scrolls on in the same direction and slows down" ||
    fail "momentum scrolling"
# A finger touching down again (libinput's hold gesture) stops it.
"$B/wsctl" flick 0 400 8 && "$B/wsctl" hold || fail "wsctl flick + hold"
sleep 1
mark
PS2=$MARKS
awk -v from="$PS1" -v to="$PS2" 'NR > from && NR <= to && / type=scroll /' \
    "$LOG/wstest.log" >"$LOG/hold.txt"
n=$(grep -c 'momentum=changed' "$LOG/hold.txt")
tail -1 "$LOG/hold.txt" | grep -q 'momentum=ended' && [ "$n" -lt 10 ] &&
    pass "hold stopped momentum at once ($n steps)" || fail "hold didn't stop momentum ($n steps)"
# Fingers that rest before lifting: no momentum.
"$B/wsctl" scroll 0 30 finger && sleep 0.3 && "$B/wsctl" scroll 0 0 finger ||
    fail "wsctl scroll finger"
sleep 0.5
mark
PS3=$MARKS
in_part "$PS2" "$PS3" 'type=scroll .*phase=ended' &&
    ! in_part "$PS2" "$PS3" 'momentum=began' &&
    pass "no momentum after the fingers rested" || fail "momentum after resting fingers"
# Wheel and other scrolls have no phases.
grep -q 'app=Alpha type=scroll win=W1 dx=0.00 dy=15.00 source=2 phase=none momentum=none' "$LOG/wstest.log" &&
    pass "a non-touchpad scroll has no phases" || fail "phases on a non-touchpad scroll"

# The window overview (WS_SERVER_OVERVIEW). W3 is minimized (its tile
# is at the bottom left); Beta is the active app.
# ov_wait STATE: wait for the overview to be open or closed.
ov_wait() {
	i=0
	while [ $i -lt 30 ]; do
		"$B/wsctl" overview status >"$LOG/ov.txt" 2>&1
		grep -q "^state=$1 " "$LOG/ov.txt" && return 0
		sleep 0.1
		i=$((i + 1))
	done
	cat "$LOG/ov.txt"
	return 1
}
# ov_item WIN FIELD: an item's field (frame, group, icon) from ov.txt.
ov_item() {
	awk -v w="window=$1" -v f="$2=" '$2 == w { for (i = 3; i <= NF; i++)
	    if (index($i, f) == 1) { sub(f, "", $i); print $i } }' "$LOG/ov.txt"
}
# ov_center WIN: the centre of its preview, "X Y".
ov_center() {
	ov_item "$1" frame | awk -F'[,x]' '{ printf "%d %d\n", $1 + $3 / 2, $2 + $4 / 2 }'
}
W1ID=$(awk '/"W1"$/ { sub("id=", "", $1); print $1 }' "$LOG/dock-list.txt")
W4ID=$(awk '/"W4"$/ { sub("id=", "", $1); print $1 }' "$LOG/dock-list.txt")
W2ID=$(awk '/"W2"$/ { sub("id=", "", $1); print $1 }' "$LOG/dock-list.txt")
mark
PO0=$MARKS
"$B/wsctl" key Super_L || fail "wsctl key Super_L"
"$B/wsctl" overview status | grep -q '^state=opening ' &&
    pass "a Start tap opens the overview, animated" || fail "no opening animation"
ov_wait open && pass "the overview opened" || fail "overview didn't open"
cat "$LOG/ov.txt"
grep -q '^state=open mode=all count=3 ' "$LOG/ov.txt" &&
    pass "every app's windows: W1, W4 and minimized W3" || fail "overview items"
[ -z "$(ov_item "$W2ID" frame)" ] && pass "W2 (main-menu level) left out" || fail "W2 in the overview"
ov_item "$W3ID" group | grep -qx 0 && ov_item "$W1ID" group | grep -qx 1 &&
    ov_item "$W4ID" group | grep -qx 1 &&
    pass "grouped by app, the active app (Beta) first" || fail "overview groups"
grep "window=$W3ID " "$LOG/ov.txt" | grep -q ' selected minimized$' &&
    pass "W3 shown as minimized, and selected" || fail "W3's item"
ov_item "$W3ID" icon | grep -q 'x48$' && ov_item "$W1ID" icon | grep -qx '0,0,0x0' &&
    pass "Beta's group has its icon; Alpha (no icon) none" || fail "group icons"
awk -F'[ =,x]+' '/^item/ { for (i = 1; i <= NF; i++) if ($i == "frame") {
	    x[n] = $(i + 1); y[n] = $(i + 2); w[n] = $(i + 3); h[n] = $(i + 4); n++ } }
    END { bad = 0
	for (i = 0; i < n; i++) {
		if (x[i] < 0 || y[i] < 0 || x[i] + w[i] > 1280 || y[i] + h[i] > 800) bad = 1
		for (j = i + 1; j < n; j++)
			if (x[i] < x[j] + w[j] && x[j] < x[i] + w[i] &&
			    y[i] < y[j] + h[j] && y[j] < y[i] + h[i]) bad = 1
	}
	exit bad }' "$LOG/ov.txt" &&
    pass "previews on screen and not overlapping" || fail "overview layout overlaps"
expect_pixel 5 5 0e0e0e "the desktop dimmed"
set -- $(ov_center "$W3ID")
expect_pixel "$1" "$2" 20a050 "minimized W3's live preview"
set -- $(ov_center "$W1ID")
expect_pixel "$1" "$2" c03030 "W1's preview"
"$B/wsctl" snapshot "$LOG/overview.ppm" >/dev/null
# Keys and the pointer are the overview's while it's open.
"$B/wsctl" key a || fail "wsctl key a (overview)"
"$B/wsctl" key Right || fail "wsctl key Right"
"$B/wsctl" overview status >"$LOG/ov.txt"
SEL=$(awk -F'selected=' '/^state=/ { split($2, a, " "); print a[1] }' "$LOG/ov.txt")
[ "$SEL" = "$W1ID" ] || [ "$SEL" = "$W4ID" ] &&
    pass "Right moved the selection to Alpha's group ($SEL)" || fail "arrow keys (selected $SEL)"
"$B/wsctl" key Return || fail "wsctl key Return"
ov_wait closed && pass "Return closed the overview" || fail "Return didn't close it"
mark
PO1=$MARKS
in_part "$PO0" "$PO1" 'type=key-(down|up) .*keycode=38 ' &&
    fail "a key reached an app through the overview" ||
    pass "keys went to no app while the overview was open"
SELNAME=W1
[ "$SEL" = "$W4ID" ] && SELNAME=W4
in_part "$PO0" "$PO1" 'app=Alpha type=app-activated' &&
    in_part "$PO0" "$PO1" "type=key-window-gained win=$SELNAME" &&
    pass "Return activated Alpha with $SELNAME key" || fail "Return didn't activate the chosen window"
"$B/wsctl" list | grep "\"$SELNAME\"\$" | grep -q ' active key ' &&
    pass "$SELNAME is the active key window" || fail "$SELNAME not key after the overview"
# Control+Up opens it; a click on W3's preview restores W3.
"$B/wsctl" key ctrl+Up || fail "wsctl key ctrl+Up"
ov_wait open && pass "Control+Up opened the overview" || fail "Control+Up"
set -- $(ov_center "$W3ID")
"$B/wsctl" click "$1" "$2" || fail "wsctl click W3's preview"
ov_wait closed && pass "a click on a preview closed the overview" || fail "click didn't close it"
mark
PO2=$MARKS
in_part "$PO1" "$PO2" 'app=Beta type=window-restore win=W3' &&
    pass "clicking minimized W3's preview restored it" || fail "no window-restore from the overview"
in_part "$PO1" "$PO2" 'type=mouse-(down|up|dragged)' &&
    fail "the click reached an app" || pass "the click went to no app"
in_part "$PO1" "$PO2" 'app=Alpha type=mouse-exited' &&
    pass "the window under the pointer heard it leave when the overview opened" ||
    fail "no mouse-exited when the overview opened"
expect_pixel 690 590 20a050 "W3 back on screen"
# App Exposé: Control+Down shows only the active app's (Beta's) windows.
"$B/wsctl" key ctrl+Down || fail "wsctl key ctrl+Down"
ov_wait open || fail "Control+Down"
grep -q '^state=open mode=app count=1 ' "$LOG/ov.txt" && grep -q "window=$W3ID " "$LOG/ov.txt" &&
    pass "Control+Down: App Exposé with Beta's W3 only" || fail "App Exposé"
"$B/wsctl" snapshot "$LOG/app-expose.ppm" >/dev/null
"$B/wsctl" key Escape || fail "wsctl key Escape"
ov_wait closed && pass "Escape closed it" || fail "Escape"
# Command shortcuts don't open it, nor does Start held over a click.
"$B/wsctl" key cmd+q || fail "wsctl key cmd+q"
"$B/wsctl" keydown Super_L && "$B/wsctl" click 150 450 && "$B/wsctl" keyup Super_L ||
    fail "Start held over a click"
sleep 0.3
"$B/wsctl" overview status | grep -q '^state=closed ' &&
    pass "Command+Q and Start with a click don't open it" || fail "the overview opened on a shortcut"
# The Mission Control key opens it, another Start tap closes it, and
# so does a click on empty space.
"$B/wsctl" key XF86LaunchA || fail "wsctl key XF86LaunchA"
ov_wait open && pass "the Mission Control key opened it" || fail "Mission Control key"
"$B/wsctl" key Super_L || fail "wsctl key Super_L (close)"
ov_wait closed && pass "another Start tap closed it" || fail "Start tap didn't close it"
"$B/wsctl" overview all | grep -q '^state=open ' || fail "wsctl overview all"
"$B/wsctl" click 5 5 || fail "wsctl click (empty)"
ov_wait closed && pass "a click on empty space closed it" || fail "click on empty space"
# Three-finger swipes: up opens, down closes; down from closed is App
# Exposé; four fingers do nothing (ThreeFingerDrag is off).
"$B/wsctl" swipe 3 0 -200 || fail "wsctl swipe up"
ov_wait open && grep -q 'mode=all' "$LOG/ov.txt" &&
    pass "three-finger swipe up opened it" || fail "swipe up"
"$B/wsctl" swipe 3 0 200 || fail "wsctl swipe down"
ov_wait closed && pass "three-finger swipe down closed it" || fail "swipe down"
"$B/wsctl" swipe 4 0 -200 && "$B/wsctl" swipe 3 300 -100 || fail "wsctl swipe (ignored)"
sleep 0.3
"$B/wsctl" overview status | grep -q '^state=closed ' &&
    pass "four fingers and sideways swipes leave it alone" || fail "a swipe opened it"
"$B/wsctl" swipe 3 0 200 || fail "wsctl swipe down (closed)"
ov_wait open && grep -q 'mode=app' "$LOG/ov.txt" &&
    pass "three-finger swipe down opened App Exposé" || fail "swipe down to App Exposé"
"$B/wsctl" overview close | grep -q '^state=closed ' &&
    pass "wsctl overview close" || fail "wsctl overview close"

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
if in_part 0 "$P4" 'app=Beta type=key-(down|up)'; then
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
# activation change, keys went to W4's connection, and Command shortcuts
# (press and release) to the app's own connection, for its menus.
in_part "$P1" "$P2" 'app=Alpha type=key-window-lost win=W1' &&
    in_part "$P1" "$P2" 'app=Gamma type=key-window-gained win=W4' &&
    pass "click in the joined connection's window moved the key window" ||
    fail "key window didn't move to W4"
in_part "$P1" "$P2" 'type=app-(de)?activated' &&
    fail "click in W4 re-activated a connection of the same app" ||
    pass "click in W4 didn't make Gamma a separate app"
in_part "$P1" "$P2" 'app=Gamma type=key-down win=W4 keycode=38 ' &&
    pass "key a went to W4's connection (Gamma)" || fail "key a not delivered to W4"
in_part "$P1" "$P2" 'app=Alpha type=key-down win=- keycode=24 .*mods=0x8' &&
    in_part "$P1" "$P2" 'app=Alpha type=key-up win=- keycode=24 ' &&
    pass "Command+q went to the app's own connection (Alpha)" ||
    fail "Command+q not delivered to Alpha"
in_part "$P1" "$P2" 'app=Gamma type=key-(down|up) win=W4 keycode=24 ' &&
    fail "Gamma got Command+q" || pass "Gamma didn't get Command+q"
in_part "$P1" "$P2" 'app=Alpha type=key-(down|up) win=- keycode=38 ' &&
    fail "Alpha got key a while W4 was key" || pass "Alpha got only the Command shortcut"
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

# Restack notices: only for the server's own raises (RaiseOnClick), to
# the window's owner, with its place among the owner's own windows.
in_part 0 "$P0" 'type=window-ordered' &&
    fail "window-ordered for a client's own ordering requests" ||
    pass "no window-ordered for the clients' own requests"
in_part "$P0" "$P1" 'app=Alpha type=window-ordered win=W1 op=above below=- above=W2 level=0' &&
    pass "raising W1 on click told Alpha (below none of its windows, under W2)" ||
    fail "no window-ordered for W1"
in_part "$P0" "$P1" 'app=(Beta|Gamma) type=window-ordered' &&
    fail "window-ordered went to a client whose window didn't move" ||
    pass "window-ordered only to the raised window's owner"
in_part "$P1" "$P2" 'app=Gamma type=window-ordered win=W4 op=above below=- above=- level=0' &&
    pass "raising W4 told its owner, the joined connection" || fail "no window-ordered for W4"
in_part "$P2" "$P3" 'app=Beta type=window-ordered win=W3 op=above ' &&
    pass "raising W3 told Beta" || fail "no window-ordered for W3"

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
