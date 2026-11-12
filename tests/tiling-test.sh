#!/bin/sh
# Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless test of window tiling (apps-windowserver#20): WindowMode
# switched live, the dwindle and master layouts, gaps, the Dock's strip,
# the active-window border, windows that float (panels, NO_TILE), a
# minimum size larger than the tile (tiled all the same), an owner that
# insists on a larger frame (cut to its tile), the keys (focus, swap, resize, float), dragging a split and
# dragging a window onto another tile, windows hidden, minimized and
# closed, a screen change, and every window's frame given back when
# tiling goes off. Then, with windows opened one by one (wstiling -e):
# where new windows go (TilingNewWindowSplits: the largest tile, so the
# screen fills evenly, or the tile under the pointer), focus following
# the pointer (TilingFocusFollowsMouse) and when it doesn't (a button
# held, a popup's pointer grab, over a panel), and a closed window's
# tile given to its sibling, the focus to the tile under the pointer.
# Runs wstiling (two apps, see tools/wstiling.c) and drives it with
# wsctl.
#
#   sh tests/tiling-test.sh [BUILD_DIR]
#
# Needs no privileges and doesn't touch the console or the GPU.

B=${1:-build}
export WS_SERVICE_NAME="org.nextbsd.windowserver.ttest.$$"
export WS_BackgroundColor=202020
LOG="/tmp/ws-ttest-$$"
SOCK="$LOG/ws.sock"
mkdir -p "$LOG" || exit 1
FAILS=0
SPID=
TPID=
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

# settle: let wstiling answer what the server sent (new frames, redraws).
settle() {
	"$B/wsctl" sync
	sleep 0.4
	"$B/wsctl" sync
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
	M=$(wc -l <"$LOG/wstiling.log")
}

seen() {
	awk -v from="$1" -v to="$2" 'NR > from && NR <= to' \
	    "$LOG/wstiling.log" | grep -q -E -- "$3"
}

check() {
	if seen "$2" "$3" "$4"; then pass "$1"; else fail "$1"; fi
}

check_not() {
	if seen "$2" "$3" "$4"; then fail "$1"; else pass "$1"; fi
}

# id NAME: wstiling's window id for NAME.
id() {
	awk -v n="$1" '$1 == "window" && $2 == n { sub("id=", "", $3); print $3 }' \
	    "$LOG/wstiling.log" | tail -1
}

# status: wsctl tiling, saved in $LOG/status.
status() {
	"$B/wsctl" tiling >"$LOG/status" 2>&1
}

# tile NAME: NAME's tile as x,y,wxh (empty if it isn't tiled).
tile() {
	awk -v w="window=$(id "$1")" '$1 == "tile" && $3 == w {
	    sub("tile=", "", $5); print $5 }' "$LOG/status"
}

# frame NAME: NAME's frame from wsctl list.
frame() {
	"$B/wsctl" list | awk -v t="\"$1\"" '$NF == t {
	    for (i = 1; i <= NF; i++) if ($i ~ /^frame=/) {
	    sub("frame=", "", $i); print $i } }'
}

# slot NAME: NAME's place in the layout.
slot() {
	awk -v w="window=$(id "$1")" '$1 == "tile" && $3 == w { print $2 }' \
	    "$LOG/status"
}

expect_tile() {
	status
	got=$(tile "$1")
	if [ "$got" = "$2" ]; then
		pass "$1's tile is $2 ($3)"
	else
		fail "$1's tile is '$got', expected $2 ($3)"
	fi
}

expect_status() {
	status
	if grep -q -E -- "$1" "$LOG/status"; then
		pass "$2"
	else
		fail "$2"
		cat "$LOG/status"
	fi
}

expect_status_not() {
	status
	if grep -q -E -- "$1" "$LOG/status"; then
		fail "$2"
		cat "$LOG/status"
	else
		pass "$2"
	fi
}

# corner X,Y,WxH: a point 20 pixels in from its top-left corner, clear
# of the windows that float (P1 at the top, N1 at the bottom right).
corner() {
	echo "$1" | awk -F'[,x]' '{ printf "%d %d\n", $1 + 20, $2 + 20 }'
}

# slot_field N FIELD: tile N's window id (3) or tile (5) or frame (6).
slot_field() {
	awk -v n="$1" -v f="$2" '$1 == "tile" && $2 == n {
	    sub("^[a-z]*=", "", $f); print $f }' "$LOG/status"
}

# click_on NAME: click NAME's tile (NAME's app becomes active).
click_on() {
	status
	set -- $(corner "$(tile "$1")")
	"$B/wsctl" click "$1" "$2"
	settle
}

"$B/WindowServer" --headless 1280x800 --socket "$SOCK" --config /dev/null -v \
    >"$LOG/server.log" 2>&1 &
SPID=$!
wait_for "$LOG/server.log" "ready:" 10 || { fail "server didn't start"; cat "$LOG/server.log"; exit 1; }
pass "server started (pid $SPID, service $WS_SERVICE_NAME)"

"$B/wstiling" >"$LOG/wstiling.log" 2>&1 &
TPID=$!
wait_for "$LOG/wstiling.log" "^ready" 10 || { fail "wstiling didn't get ready"; cat "$LOG/wstiling.log"; exit 1; }
pass "wstiling connected two apps and created six windows"
settle

# ---- stacking, the default ----
expect_status '^mode=stacking layout=dwindle tiled=0 .*gaps=8,8 border=2 modifier=Command\+Option\+arrows' \
    "windows stack by default; dwindle, 8-pixel gaps, Command+Option"
[ "$(frame T1)" = "100,100,400x300" ] && pass "T1 is where its app put it" ||
    fail "T1 at $(frame T1)"
[ "$("$B/wsctl" settings get TilingNewWindowSplits)" = largest ] &&
    pass "new windows split the largest tile by default" ||
    fail "TilingNewWindowSplits is $("$B/wsctl" settings get TilingNewWindowSplits)"
"$B/wsctl" settings get TilingFocusFollowsMouse | grep -q -i -E '^(true|yes|1)$' &&
    pass "focus follows the mouse by default" ||
    fail "TilingFocusFollowsMouse is $("$B/wsctl" settings get TilingFocusFollowsMouse)"
"$B/wsctl" settings set TilingNewWindowSplits spiral 2>/dev/null &&
    fail "TilingNewWindowSplits spiral accepted" ||
    pass "TilingNewWindowSplits spiral refused"
# Up to "tiling off": classic dwindle, each window splitting the focused
# one, and focus only by click or key.
"$B/wsctl" settings set TilingNewWindowSplits focused \
    TilingFocusFollowsMouse false >/dev/null ||
    fail "settings set TilingNewWindowSplits focused"
mark
P0=$M
"$B/wsctl" key cmd+alt+Right
mark
P1=$M
check "stacking: Command+Option+Right goes to the app" "$P0" "$P1" \
    'app=Tiler type=key-down win=T3 .*sym=0xff53'

# ---- tiling on: dwindle ----
"$B/wsctl" tiling on || fail "wsctl tiling on"
settle
expect_status '^mode=tiling layout=dwindle tiled=4 focused='"$(id T3)"' area=8,8,1264x784' \
    "tiling on: four windows tiled in 8,8 1264x784, T3 focused"
expect_tile T3 8,8,628x784 "the key window first, left half"
expect_tile T2 644,8,628x388 "then the frontmost, splitting what is left"
expect_tile T1 644,404,310x388 ""
expect_tile O1 962,404,310x388 "the other app's window too"
[ -z "$(tile P1)" ] && pass "the panel P1 isn't tiled" || fail "P1 tiled"
[ -z "$(tile N1)" ] && pass "N1 (WS_WINDOW_NO_TILE) isn't tiled" || fail "N1 tiled"
[ "$(frame T3)" = "8,8,628x784" ] && pass "T3 took its tile's frame" ||
    fail "T3's frame is $(frame T3)"
"$B/wsctl" list | grep '"T3"$' | grep -q ' tiled ' &&
    pass "wsctl list marks T3 tiled" || fail "T3 not marked tiled"
"$B/wsctl" list | grep '"P1"$' | grep -q ' tiled ' &&
    fail "P1 marked tiled" || pass "P1 isn't marked tiled"
mark
P2=$M
check "T3's app heard its new frame, marked as a tile" "$P1" "$P2" \
    'app=Tiler type=window-frame win=T3 frame=8,8,628x784 tiled'
check "O1's app too" "$P1" "$P2" \
    'app=Other type=window-frame win=O1 frame=962,404,310x388'
expect_pixel 322 400 3050c0 "T3 redrawn at its new size"
expect_pixel 700 300 30a040 "T2 in its tile"
expect_pixel 797 598 c03030 "T1 in its tile"
expect_pixel 1111 500 30c0c0 "O1 in its tile"
expect_pixel 1000 135 c0c030 "the panel floats above the tiles"
expect_pixel 1075 650 e08020 "N1 floats above the tiles"
expect_pixel 7 400 4a7fc0 "the border round the key window"
expect_pixel 640 300 202020 "the gap between tiles"
expect_pixel 3 3 202020 "the outer gap"
"$B/wsctl" snapshot "$LOG/tiling-dwindle.ppm" >/dev/null

# The stack: tiled windows stay below the floating ones.
"$B/wsctl" click 1111 500
settle
expect_pixel 1075 650 e08020 "clicking O1 raises it among the tiles, not over N1"
expect_pixel 1000 135 c0c030 "...nor over the panel"
"$B/wsctl" click 322 400
settle

# ---- the keys ----
mark
P3=$M
"$B/wsctl" key cmd+alt+Right
settle
expect_status "focused=$(id T2) " "Command+Option+Right: T2 has the focus"
mark
P4=$M
check_not "tiling: the key didn't go to the app" "$P3" "$P4" 'type=key-down.*sym=0xff53'
"$B/wsctl" key cmd+alt+Down
settle
expect_status "focused=$(id T1) " "Command+Option+Down: T1 (below T2)"
"$B/wsctl" key cmd+alt+Right
settle
expect_status "focused=$(id O1) " "Command+Option+Right: O1, the other app"
expect_pixel 961 450 4a7fc0 "the border moved to O1"
"$B/wsctl" key cmd+alt+Left
"$B/wsctl" key cmd+alt+Up
"$B/wsctl" key cmd+alt+Left
settle
expect_status "focused=$(id T3) " "back to T3"
"$B/wsctl" key cmd+alt+shift+Right
settle
expect_tile T3 644,8,628x388 "Command+Option+Shift+Right: T3 and T2 swapped"
expect_tile T2 8,8,628x784 ""
expect_pixel 200 600 30a040 "T2 drawn on the left"
"$B/wsctl" key cmd+alt+shift+Left
settle
expect_tile T3 8,8,628x784 "and back"
"$B/wsctl" key cmd+alt+equal
settle
expect_tile T3 8,8,691x784 "Command+Option+Equal: T3 grows to 55%"
"$B/wsctl" key cmd+alt+minus
"$B/wsctl" key cmd+alt+minus
settle
expect_tile T3 8,8,565x784 "Command+Option+Minus twice: 45%"
"$B/wsctl" tiling grow >/dev/null
settle
expect_tile T3 8,8,628x784 "wsctl tiling grow: 50% again"

# Float and tile again.
"$B/wsctl" key cmd+alt+shift+space
settle
expect_status '^mode=tiling .*tiled=3 ' "Command+Option+Shift+Space: T3 floats"
expect_status "^float window=$(id T3) .* floated" "...listed as floated"
[ "$(frame T3)" = "300,200,400x300" ] &&
    pass "T3 has its frame from before tiling again" ||
    fail "T3's frame is $(frame T3)"
expect_tile T2 8,8,628x784 "the others take the space"
expect_pixel 500 350 3050c0 "the floating T3 is above the tiles"
"$B/wsctl" snapshot "$LOG/tiling-float.ppm" >/dev/null
"$B/wsctl" key cmd+alt+shift+space
settle
expect_status '^mode=tiling .*tiled=4 ' "pressed again: T3 tiled again"
"$B/wsctl" tiling float "$(id T1)" >/dev/null
settle
expect_status "^float window=$(id T1) .* floated" "wsctl tiling float WINDOW"
"$B/wsctl" tiling tile "$(id T1)" >/dev/null
settle
expect_status '^mode=tiling .*tiled=4 ' "wsctl tiling tile WINDOW"
"$B/wsctl" tiling float "$(id P1)" >/dev/null
"$B/wsctl" tiling tile "$(id P1)" >/dev/null 2>&1 &&
    fail "a panel can be tiled" || pass "a panel can't be tiled (EINVAL)"

# ---- dragging ----
status
FIRST=$(slot_field 0 3)
W0=$(slot_field 0 5 | awk -F'[,x]' '{ print $3 }')
SPLIT=$((8 + W0 + 4))
"$B/wsctl" move "$SPLIT" 400
"$B/wsctl" down
"$B/wsctl" move 400 400
"$B/wsctl" move 500 400
"$B/wsctl" up
settle
status
W1=$(slot_field 0 5 | awk -F'[,x]' '{ print $3 }')
[ "$W1" = "488" ] && pass "dragging the split to x=500 made the first tile 488 wide" ||
    fail "first tile is $W1 wide after the split drag (was $W0)"
mark
P5=$M
check_not "the split drag gave no app a click" "$P4" "$P5" 'type=mouse-down'

# Drag the first window onto the last tile: they swap.
LAST=$(slot_field 3 3)
LT=$(slot_field 3 5)
FT=$(slot_field 0 5)
set -- $(corner "$FT")
"$B/wsctl" move "$1" "$2"
"$B/wsctl" down
settle
set -- $(corner "$LT")
"$B/wsctl" move $(($1 - 60)) $(($2 - 60))
settle
"$B/wsctl" move "$1" "$2"
settle
"$B/wsctl" up
settle
status
NF0=$(slot_field 0 3)
NL=$(slot_field 3 3)
[ "$NF0" = "$LAST" ] && [ "$NL" = "$FIRST" ] &&
    pass "a window dragged onto another tile swapped places with it" ||
    fail "after the drag: first $NF0 last $NL (was $FIRST, $LAST)"
F=$(slot_field 3 6)
[ "$F" = "$LT" ] && pass "the dragged window went to its new tile" ||
    fail "the dragged window is at $F, its tile $LT"
# Dragged and let go over no other tile: back to its own.
set -- $(corner "$LT")
"$B/wsctl" move "$1" "$2"
"$B/wsctl" down
"$B/wsctl" move $(($1 + 3)) $(($2 + 3))
settle
"$B/wsctl" up
settle
status
F=$(slot_field 3 6)
[ "$(slot_field 3 3)" = "$FIRST" ] && [ "$F" = "$LT" ] &&
    pass "dropped on its own tile: back in place" ||
    fail "the window is at $F, its tile $LT"

# ---- master and stack ----
"$B/wsctl" tiling layout master || fail "wsctl tiling layout master"
settle
expect_status '^mode=tiling layout=master tiled=4 ' "layout master, live"
status
MT=$(awk '$1 == "tile" && $2 == 0 { sub("tile=", "", $5); print $5 }' "$LOG/status")
[ "$MT" = "8,8,691x784" ] && pass "the master takes 55% on the left" ||
    fail "master tile $MT"
grep -q '^tile 1 .* tile=707,8,565x256 ' "$LOG/status" &&
    grep -q '^tile 2 .* tile=707,272,565x256 ' "$LOG/status" &&
    grep -q '^tile 3 .* tile=707,536,565x256 ' "$LOG/status" &&
    pass "the stack shares the right side" || { fail "the stack"; cat "$LOG/status"; }
"$B/wsctl" snapshot "$LOG/tiling-master.ppm" >/dev/null
"$B/wsctl" settings set TilingLayout spiral 2>/dev/null &&
    fail "TilingLayout spiral accepted" || pass "TilingLayout spiral refused"
"$B/wsctl" settings set TilingModifier Command+Shift 2>/dev/null &&
    fail "TilingModifier with Shift accepted" || pass "TilingModifier with Shift refused"

# ---- the Dock's strip and gaps ----
"$B/wsctl" dock edge right 80 60 >"$LOG/dock.log" 2>&1 &
DPID=$!
sleep 1
settle
expect_status 'area=8,8,1184x784 ' "a Dock on the right: the tiles keep its strip free"
status
grep -q '^tile 1 .* tile=[0-9]*,8,[0-9]*x256 ' "$LOG/status" &&
    ! awk '$1 == "tile" { sub("tile=", "", $5); split($5, a, "[,x]");
	if (a[1] + a[3] > 1192) bad = 1 } END { exit !bad }' "$LOG/status" &&
    pass "no tile reaches under the Dock" || { fail "tiles under the Dock"; cat "$LOG/status"; }
"$B/wsctl" snapshot "$LOG/tiling-dock-right.ppm" >/dev/null
kill "$DPID"
wait "$DPID" 2>/dev/null
DPID=
"$B/wsctl" dock edge bottom 70 60 >"$LOG/dock.log" 2>&1 &
DPID=$!
sleep 1
settle
expect_status 'area=8,8,1264x714 ' "a Dock at the bottom"
"$B/wsctl" snapshot "$LOG/tiling-dock-bottom.ppm" >/dev/null
kill "$DPID"
wait "$DPID" 2>/dev/null
DPID=
settle
expect_status 'area=8,8,1264x784 ' "the Dock gone: the whole screen again"
"$B/wsctl" tiling gaps 0 0 || fail "wsctl tiling gaps 0 0"
settle
expect_status 'area=0,0,1280x800 gaps=0,0 ' "no gaps"
status
grep -q '^tile 0 .* tile=0,0,704x800 ' "$LOG/status" &&
    pass "the master fills the left edge with no gap" || { fail "master with no gaps"; cat "$LOG/status"; }
"$B/wsctl" tiling gaps 8 8 >/dev/null
"$B/wsctl" tiling layout dwindle >/dev/null
settle

# ---- windows come and go ----
# New windows split the largest tile from here on (the default), so
# there is room for those that keep their own size.
"$B/wsctl" settings set TilingNewWindowSplits largest >/dev/null
click_on T3
"$B/wsctl" key h
settle
expect_status '^mode=tiling .*tiled=3 ' "T2 hidden: three tiles"
[ -z "$(tile T2)" ] && pass "T2 left the layout" || fail "T2 still tiled"
"$B/wsctl" key u
settle
expect_status '^mode=tiling .*tiled=4 ' "T2 shown again: four"
"$B/wsctl" key n
settle
expect_status '^mode=tiling .*tiled=5 ' "a new window T4 is tiled"
[ -n "$(tile T4)" ] && pass "T4 has a tile" || fail "T4 not tiled"
"$B/wsctl" key m
settle
expect_status '^mode=tiling .*tiled=4 ' "the key window minimized: four"
"$B/wsctl" list | grep -q ' minimized' && pass "...and it is minimized" ||
    fail "nothing minimized"
click_on T1
"$B/wsctl" key x
settle
expect_status '^mode=tiling .*tiled=3 ' "a window closed: three"

# Windows larger than their tiles tile all the same (as on Hyprland).
"$B/wsctl" key b
settle
expect_status '^mode=tiling .*tiled=4 ' "M1 (900x700 at least) tiles all the same"
MT=$(tile M1)
[ -n "$MT" ] && [ "$(frame M1)" = "$MT" ] && pass "...at its tile ($MT)" ||
    fail "M1 at $(frame M1), tile $MT"
"$B/wsctl" tiling status | grep -q '^float ' && fail "something floats" ||
    pass "nothing floats"
"$B/wsctl" key k
sleep 1
settle
expect_status '^mode=tiling .*tiled=5 ' "S1, which keeps taking 1000x700, tiles"
expect_status "^tile [0-9]* window=$(id S1) .* cut" "...cut to its tile"
ST=$(tile S1)
SF=$(frame S1)
[ -n "$ST" ] && [ "$SF" = "$ST" ] && pass "...its frame is its tile ($SF)" ||
    fail "S1 at $SF, tile $ST"
grep -q "keeps taking 1000x700.*cut to its tile" "$LOG/server.log" &&
    pass "...and the server says why" || fail "no log line about S1"
# (1000x700 itself only if its tile's corner isn't where it put it)
check "S1's app heard its tile's corner" 0 \
    "$(wc -l <"$LOG/wstiling.log")" \
    "win=S1 frame=$(echo "$ST" | cut -d, -f1,2),[0-9]+x[0-9]+ tiled\$"
click_on S1
"$B/wsctl" key x
click_on M1
"$B/wsctl" key x
settle
expect_status '^mode=tiling .*tiled=3 ' "S1 and M1 closed: three"
"$B/wsctl" key c
settle
status
CT=$(tile C1)
CF=$(frame C1)
echo "$CT $CF" | awk -F'[ ,x]' '{ exit !($1 == $5 && $2 == $6 && $7 % 50 == 0 &&
    $8 % 50 == 0 && $7 <= $3 && $8 <= $4) }' &&
    pass "C1 keeps its smaller, 50-pixel-step size at its tile's corner ($CF in $CT)" ||
    fail "C1 at $CF, tile $CT"

# An owner that only moves its windows (an X server): once it has drawn
# at its own size for a while, the window keeps that size in its tile.
"$B/wsctl" key o
sleep 2
settle
status
XT=$(tile X1)
XF=$(frame X1)
echo "$XT $XF" | awk -F'[ ,x]' '{ exit !($1 == $5 && $2 == $6 && $7 == 250 &&
    $8 == 150) }' &&
    pass "X1, which only moves, keeps the 250x150 it draws at its tile's corner ($XF in $XT)" ||
    fail "X1 at $XF, tile $XT"

# ---- the screen changes ----
"$B/wsctl" screen size 1600x900 >/dev/null
settle
expect_status 'area=8,8,1584x884 ' "a bigger screen: laid out again"
"$B/wsctl" screen size 1280x800 >/dev/null
settle

# ---- tiling off ----
mark
POFF=$M
"$B/wsctl" tiling off || fail "wsctl tiling off"
settle
expect_status '^mode=stacking .*tiled=0 ' "tiling off"
[ "$(frame T3)" = "300,200,400x300" ] && pass "T3 back at its frame from before" ||
    fail "T3 at $(frame T3)"
[ "$(frame O1)" = "500,400,400x300" ] && pass "O1 too" || fail "O1 at $(frame O1)"
[ "$(frame T2)" = "200,150,400x300" ] && pass "T2 too (hidden and shown meanwhile)" ||
    fail "T2 at $(frame T2)"
"$B/wsctl" list | grep -q ' tiled ' && fail "windows still marked tiled" ||
    pass "no window is marked tiled"
expect_pixel 7 400 202020 "no border"
"$B/wsctl" snapshot "$LOG/tiling-stacking.ppm" >/dev/null
mark
P6=$M
check "T3's app heard its old frame back, not as a tile" "$POFF" "$P6" \
    'win=T3 frame=300,200,400x300$'
check_not "no frame given back is marked as a tile" "$POFF" "$P6" \
    'type=window-frame .* tiled$'
check_not "no errors" 0 "$P6" 'type=error'
"$B/wsctl" tiling focus left 2>/dev/null &&
    fail "focus while stacking" || pass "tiling requests fail while stacking"

# ---- windows opened one by one: where they go, and the focus ----
# A user opening Terminal windows (apps-windowserver: "i opened 4
# terminals, the left window never offered to take one").
kill "$TPID"
wait "$TPID" 2>/dev/null
"$B/wsctl" settings unset TilingNewWindowSplits TilingFocusFollowsMouse \
    >/dev/null || fail "settings unset"
mv "$LOG/wstiling.log" "$LOG/wstiling-1.log"
"$B/wstiling" -e >"$LOG/wstiling.log" 2>&1 &
TPID=$!
wait_for "$LOG/wstiling.log" "^ready" 10 || { fail "wstiling -e didn't get ready"; cat "$LOG/wstiling.log"; exit 1; }
"$B/wsctl" tiling on >/dev/null
"$B/wsctl" move 200 200
settle

# open: a key typed into the key window opens the next one, which is
# made key; wait for it to be tiled.
open() {
	"$B/wsctl" key "${2:-w}"
	i=0
	while [ $i -lt 30 ]; do
		status
		[ -n "$(tile "$1")" ] && break
		sleep 0.1
		i=$((i + 1))
	done
	settle
}

# focused_is NAME WHAT
focused_is() {
	expect_status "^mode=tiling .*focused=$(id "$1") " "$2"
}

# center NAME: the middle of NAME's tile.
center() {
	tile "$1" | awk -F'[,x]' '{ printf "%d %d\n", $1 + $3 / 2, $2 + $4 / 2 }'
}

expect_tile W1 8,8,1264x784 "largest: W1 alone takes the area"
open W2
expect_tile W1 8,8,628x784 "W2: side by side, W1 on the left"
expect_tile W2 644,8,628x784 "...W2 on the right"
focused_is W2 "the new window has the focus, the pointer not moved"
set -- $(center W1)
"$B/wsctl" move "$1" "$2"
settle
focused_is W1 "the pointer onto W1: W1 has the focus"
open W3
expect_tile W1 8,8,628x388 "W3 splits the left tile, top and bottom"
expect_tile W3 8,404,628x388 "...W3 below W1"
expect_tile W2 644,8,628x784 "...the right one untouched"
open W4
expect_tile W2 644,8,628x388 "W4 splits the right tile: a 2x2 grid"
expect_tile W4 644,404,628x388 ""
expect_tile W1 8,8,628x388 "...the left column untouched"
"$B/wsctl" snapshot "$LOG/tiling-ffm-grid.ppm" >/dev/null
open W5
expect_tile W1 8,8,310x388 "W5 splits the top left tile, side by side"
expect_tile W5 326,8,310x388 ""
open W6
expect_tile W3 8,404,310x388 "W6 splits the bottom left tile"
expect_tile W6 326,404,310x388 ""
expect_tile W2 644,8,628x388 "...the right column untouched"

# Focus follows the pointer.
mark
F0=$M
set -- $(center W2)
"$B/wsctl" move "$1" "$2"
settle
focused_is W2 "the pointer onto W2: W2 has the focus"
mark
check "W2's app heard it is key" "$F0" "$M" 'type=key-window-gained win=W2'
set -- $(center W4)
"$B/wsctl" move "$1" "$2"
settle
focused_is W4 "onto W4: W4"
expect_pixel 643 600 4a7fc0 "the border follows"
"$B/wsctl" move 640 600
settle
focused_is W4 "over the gap: W4 keeps it"
open V1 v
focused_is V1 "V1 in the other app opens with the focus"
mark
F0=$M
set -- $(center W4)
"$B/wsctl" move "$1" "$2"
settle
focused_is W4 "the pointer onto W4: W4's app is active again"
mark
check "Tiler was activated" "$F0" "$M" 'app=Tiler type=app-activated'
check "Other was deactivated" "$F0" "$M" 'app=Other type=app-deactivated'
set -- $(center V1)
"$B/wsctl" move "$1" "$2"
settle
focused_is V1 "onto V1: the other app"
"$B/wsctl" snapshot "$LOG/tiling-ffm-follow.ppm" >/dev/null

# No focus change while a button is held, nor with a popup's grab.
set -- $(center W2)
"$B/wsctl" down right
"$B/wsctl" move "$1" "$2"
settle
focused_is V1 "the right button held: V1 keeps the focus over W2"
"$B/wsctl" up right
settle
set -- $(center V1)
"$B/wsctl" move "$1" "$2"
settle
"$B/wsctl" key g
settle
grep -q "^grab app=Other 0" "$LOG/wstiling.log" && pass "Other grabbed the pointer" ||
    fail "no grab"
set -- $(center W2)
"$B/wsctl" move "$1" "$2"
settle
focused_is V1 "a popup's pointer grab: V1 keeps the focus over W2"
set -- $(center V1)
"$B/wsctl" move "$1" "$2"
"$B/wsctl" key G
settle
set -- $(center W2)
"$B/wsctl" move "$1" "$2"
settle
focused_is W2 "the grab ended: the pointer gives W2 the focus"
# A panel that floats over the tiles.
"$B/wsctl" key p
settle
"$B/wsctl" move 640 360
settle
focused_is W2 "over the panel Q1: W2 keeps the focus"
"$B/wsctl" settings set TilingFocusFollowsMouse false >/dev/null
set -- $(center W4)
"$B/wsctl" move "$1" "$2"
settle
focused_is W2 "TilingFocusFollowsMouse off: W2 keeps it over W4"
"$B/wsctl" settings unset TilingFocusFollowsMouse >/dev/null

# Closing: the sibling takes the space, the pointer's tile the focus.
set -- $(center W2)
"$B/wsctl" move "$1" "$2"
settle
focused_is W2 "W2 focused again"
"$B/wsctl" key x
settle
expect_tile V1 644,8,628x388 "W2 closed: V1, its sibling, takes its space"
expect_tile W4 644,404,628x388 "...W4 stays"
focused_is V1 "...and V1, under the pointer now, the focus"

# TilingNewWindowSplits pointer: the tile under the pointer (Hyprland).
kill "$TPID"
wait "$TPID" 2>/dev/null
"$B/wsctl" settings set TilingNewWindowSplits pointer >/dev/null ||
    fail "TilingNewWindowSplits pointer"
mv "$LOG/wstiling.log" "$LOG/wstiling-2.log"
"$B/wstiling" -e >"$LOG/wstiling.log" 2>&1 &
TPID=$!
wait_for "$LOG/wstiling.log" "^ready" 10 || { fail "wstiling -e didn't get ready"; exit 1; }
"$B/wsctl" move 200 200
settle
open W2
expect_tile W2 644,8,628x784 "pointer: W2 splits W1, under the pointer"
"$B/wsctl" move 200 200
settle
focused_is W1 "the pointer over the left tile: W1"
open W3
expect_tile W1 8,8,628x388 "W3 splits the left tile, under the pointer"
expect_tile W3 8,404,628x388 ""
"$B/wsctl" move 200 200
settle
open W4
expect_tile W1 8,8,310x388 "W4 splits the left tile again (W1, under the pointer)"
expect_tile W4 326,8,310x388 ""
expect_tile W2 644,8,628x784 "...the right one untouched"
"$B/wsctl" snapshot "$LOG/tiling-ffm-pointer.ppm" >/dev/null
"$B/wsctl" key p
settle
"$B/wsctl" move 640 360
settle
open W5
expect_tile W4 326,8,310x190 "over the panel: W5 splits the focused tile (W4)"
expect_tile W5 326,206,310x190 ""

# ---- stacking: click to focus as before ----
"$B/wsctl" tiling off >/dev/null
settle
mark
F0=$M
"$B/wsctl" move 200 200
"$B/wsctl" move 900 500
settle
mark
check_not "stacking: the pointer gives no window the focus" "$F0" "$M" \
    'type=key-window-gained'
"$B/wsctl" settings unset TilingNewWindowSplits >/dev/null

kill "$TPID"
wait "$TPID" 2>/dev/null
TPID=
kill "$SPID"
wait "$SPID" 2>/dev/null
SPID=
wait_for "$LOG/server.log" "exiting" 5 && pass "server shut down cleanly" ||
    fail "server shutdown"

echo "logs: $LOG"
if [ $FAILS -ne 0 ]; then
	echo "$FAILS check(s) failed"
	exit 1
fi
echo "all checks passed"
