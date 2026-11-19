#!/bin/sh
# Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless matrix of the menu bar on every display (apps-windowserver
# #100) across everything that changes the strips and the usable areas:
#
#   menu style	mac (a bar: a strip on every display), next and
#		inwindow (no bar: no strip; the same to WindowServer,
#		which hears WS_MSG_SET_MENU_BAR 0 for both)
#   window mode	stacking, tiling (WindowMode)
#   displays	built-in only, mirrored, extended with the monitor on
#		the right, extended with it on the left
#   Dock edge	left, bottom, right, and top outside the Mac style
#
# For each displays x Dock pair it walks every transition between the
# style x mode states (each ordered pair once), and in each state
# checks every display's usable area, where the live bar and the
# status item are, that every other display shows their dimmed copy
# (the Mac style) or no strip and no copy left behind (the others),
# that a press on a copy moves the bar there with the strip never
# showing the desktop, and that unplugging the monitor and plugging it
# back in gives the same answers. Uses wsmenubar (SIGHUP switches its
# style, as NBMenuStyle does) and wsctl.
#
#   sh tests/menubar-matrix-test.sh [BUILD_DIR]
#
# Needs no privileges and doesn't touch the console or the GPU. Takes
# about a quarter of an hour; VERBOSE=1 prints every check.

B=${1:-build}
export WS_SERVICE_NAME="org.nextbsd.windowserver.mbmtest.$$"
export WS_BackgroundColor=202020
export WS_TilingGapsOuter=8
LOG="/tmp/ws-mbmtest-$$"
SOCK="$LOG/ws.sock"
mkdir -p "$LOG" || exit 1
export WS_DisplayStore="$LOG/displays.plist"
FAILS=0
CHECKS=0
SPID=
PIDS=
DOCK=

fail() {
	echo "FAIL: [$STATE] $*"
	FAILS=$((FAILS + 1))
	CHECKS=$((CHECKS + 1))
}

pass() {
	[ -n "$VERBOSE" ] && echo "ok:   [$STATE] $*"
	CHECKS=$((CHECKS + 1))
}

cleanup() {
	for p in $PIDS $DOCK; do
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
	sleep 0.2
	"$B/wsctl" sync
}

pixel() {
	if [ "$1" = -d ]; then
		"$B/wsctl" pixel -d "$2" "$3" "$4" | awk '{print $2}'
	else
		"$B/wsctl" pixel "$1" "$2" | awk '{print $2}'
	fi
}

near() {
	awk -v a="$1" -v b="$2" 'function h(s, i) { return ("0x" substr(s,
	    i, 2)) + 0 } BEGIN { if (length(a) != 6) exit 1
	    for (i = 1; i <= 5; i += 2) { d = h(a, i) - h(b, i)
	    if (d < -2 || d > 2) exit 1 } exit 0 }'
}

winfo() {
	"$B/wsctl" list | awk -v t="\"$1\"" -v f="$2" '$NF == t {
	    for (i = 1; i <= NF; i++) if (index($i, f "=") == 1) {
	    sub(f "=", "", $i); print $i } }'
}

displays() {
	"$B/wsctl" displays >"$LOG/displays" 2>&1
}

field() {
	awk -v id="$1" -v k="$2=" '$1 == id { for (i = 1; i <= NF; i++)
	    if (index($i, k) == 1) { print substr($i, length(k) + 1) } }' \
	    "$LOG/displays"
}

check() {	# GOT WANT WHAT
	if [ "$1" = "$2" ]; then
		pass "$3: $2"
	else
		fail "$3: '$1', expected '$2'"
	fi
}

check_pixel() {	# DISPLAY X Y WANT WHAT (DISPLAY 0: the main one)
	if [ "$1" = 0 ]; then
		got=$(pixel "$2" "$3")
	else
		got=$(pixel -d "$1" "$2" "$3")
	fi
	if near "$got" "$4"; then
		pass "$5: $4"
	else
		fail "$5: display $1 pixel $2,$3 is $got, expected $4"
	fi
}

start() {
	name=$1
	shift
	"$B/wsmenubar" "$@" >"$LOG/$name.log" 2>&1 &
	LAST=$!
	PIDS="$PIDS $LAST"
	wait_for "$LOG/$name.log" "^ready" 10 ||
	    { fail "$name didn't get ready"; cat "$LOG/$name.log"; }
	settle
}

# ---- the states ----

STYLE=mac
MODE=stacking
DISP=single
EDGE=bottom
STATE=start

set_style() {	# mac | next | inwindow
	want=$1
	was_mac=$([ "$STYLE" = mac ] && echo y)
	now_mac=$([ "$want" = mac ] && echo y)
	if [ "$was_mac" != "$now_mac" ]; then
		n=$(grep -c "^style" "$LOG/A.log")
		kill -HUP "$APID"
		i=0
		while [ "$(grep -c "^style" "$LOG/A.log")" = "$n" ] &&
		    [ $i -lt 50 ]; do
			sleep 0.1
			i=$((i + 1))
		done
	fi
	STYLE=$want
	settle
}

set_mode() {	# stacking | tiling
	"$B/wsctl" settings set WindowMode "$1" >/dev/null ||
	    fail "WindowMode $1"
	MODE=$1
	settle
}

set_dock() {	# left | bottom | right | top
	[ -n "$DOCK" ] && { kill "$DOCK" 2>/dev/null; wait "$DOCK" 2>/dev/null; }
	"$B/wsctl" dock edge "$1" 64 3600 >"$LOG/dock.log" 2>&1 &
	DOCK=$!
	EDGE=$1
	sleep 0.3
	settle
}

ext_id() {
	displays
	awk '$4 == "TST-0002-00000001" { print $1 }' "$LOG/displays"
}

set_disp() {	# single | mirrored | right | left
	EXT=$(ext_id)
	case $1 in
	single)
		[ -n "$EXT" ] && "$B/wsctl" displays unplug "$EXT"
		;;
	*)
		if [ -z "$EXT" ]; then
			"$B/wsctl" displays plug 1920x1080 0x0 \
			    TST-0002-00000001 || fail "plug"
			settle
			EXT=$(ext_id)
		fi
		case $1 in
		mirrored)
			"$B/wsctl" displays "$EXT" mirror "$BUILTIN" ;;
		right)
			"$B/wsctl" displays "$EXT" mirror off
			settle
			"$B/wsctl" displays "$EXT" origin 1280,0
			settle
			# from the left the right place is past the
			# built-in display
			displays
			[ "$(field "$EXT" frame)" = "1280,0,1920x1080" ] ||
			    "$B/wsctl" displays "$EXT" origin 3200,0 ;;
		left)
			"$B/wsctl" displays "$EXT" mirror off
			settle
			"$B/wsctl" displays "$EXT" origin -1920,0 ;;
		esac
		;;
	esac
	DISP=$1
	settle
	EXT=$(ext_id)
	geometry
	# focus on the built-in display, from a press on its desktop
	"$B/wsctl" click "$((MX + 640))" 700 >/dev/null
	settle
}

# ---- what each state should look like ----

# Every rect is x,y,WxH. M: the built-in display's frame, X: the
# monitor's, for the current arrangement.
geometry() {
	case $DISP in
	single|mirrored)
		MX=0; XX=; ;;
	right)
		MX=0; XX=1280 ;;
	left)
		MX=1920; XX=0 ;;
	esac
}

# usable FRAME_X FRAME_W FRAME_H HAS_STRIP DOCK_HERE
usable() {
	awk -v x="$1" -v w="$2" -v h="$3" -v s="$4" -v e="$5" 'BEGIN {
	    y = 0
	    if (s == "y") { y += 24; h -= 24 }
	    if (e == "left") { x += 64; w -= 64 }
	    if (e == "right") { w -= 64 }
	    if (e == "bottom") { h -= 64 }
	    if (e == "top") { y += 64; h -= 64 }
	    printf "%d,%d,%dx%d\n", x, y, w, h }'
}

verify() {
	STATE="$STYLE/$MODE/$DISP/dock-$EDGE"
	geometry
	mac=$([ "$STYLE" = mac ] && echo y || echo n)
	# where the Dock is: left/right on the outermost display, else main
	mdock=$EDGE
	xdock=none
	if [ -n "$XX" ]; then
		if [ "$EDGE" = left ] && [ "$DISP" = left ]; then
			mdock=none; xdock=left
		fi
		if [ "$EDGE" = right ] && [ "$DISP" = right ]; then
			mdock=none; xdock=right
		fi
	fi
	displays
	check "$(field "$BUILTIN" usable)" \
	    "$(usable $MX 1280 800 $mac $mdock)" "built-in usable area"
	u=$("$B/wsctl" screen | awk '$1 == "usable" { print $2 " " $3 }' |
	    tr ' ' ',')
	check "$u" "$(usable $MX 1280 800 $mac $mdock)" \
	    "WS_EVENT_USABLE_AREA (the main display's)"
	if [ "$DISP" = mirrored ]; then
		check "$(field "$EXT" usable)" "0,0,1280x800" \
		    "a mirror: its source's part, no strip of its own"
	elif [ -n "$XX" ]; then
		check "$(field "$EXT" usable)" \
		    "$(usable $XX 1920 1080 $mac $xdock)" "monitor usable area"
	fi
	if [ $mac = y ]; then
		check "$(winfo A-bar frame)" "$MX,0,1280x24" \
		    "the live bar on the built-in display, as wide as it"
		check "$(winfo clock frame)" "$((MX + 1200)),0,80x24" \
		    "the clock at its right end"
		check_pixel 0 50 10 c03030 "the live bar"
		if [ -n "$XX" ] && [ "$DISP" != mirrored ]; then
			check_pixel "$EXT" 50 10 c88080 "the monitor: the bar dimmed"
			check_pixel "$EXT" 1900 10 888888 \
			    "the monitor: the clock dimmed"
			# a press on the copy: the bar moves, in one frame
			"$B/wsctl" click "$((XX + 50))" 10 >/dev/null
			g1=$(pixel 50 10)
			g2=$(pixel -d "$EXT" 50 10)
			settle
			check "$(winfo A-bar frame)" "$XX,0,1920x24" \
			    "a press on the copy: the live bar there, as wide"
			check "$(winfo clock frame)" "$((XX + 1840)),0,80x24" \
			    "and the clock at the monitor's right end"
			[ "$g1" != 202020 ] && [ "$g2" != 202020 ] &&
			    pass "neither strip showed the desktop" ||
			    fail "a strip showed the desktop on the move"
			check_pixel "$EXT" 50 10 c03030 "the monitor: the live bar"
			check_pixel 0 50 10 c88080 "the built-in display: the copy"
			"$B/wsctl" click "$((MX + 640))" 700 >/dev/null
			settle
			check "$(winfo A-bar frame)" "$MX,0,1280x24" \
			    "a press on the built-in desktop: the bar back"
		fi
		if [ "$DISP" = mirrored ]; then
			# 1280x800 in the 1728x1080 middle of the panel
			check_pixel "$EXT" 200 13 c03030 \
			    "the mirror shows the live bar, not a copy"
		fi
	else
		check "$(winfo A-bar frame)" "" "no bar on screen"
		check "$(winfo clock frame)" "" "no status item on screen"
		if [ -n "$XX" ] && [ "$DISP" != mirrored ]; then
			check_pixel "$EXT" 50 10 202020 \
			    "no strip and no copy left on the monitor"
			check_pixel "$EXT" 1900 10 202020 \
			    "no copy of the clock left"
		fi
	fi
}

# Every ordered pair of the style x mode states once (an Euler circuit
# of the complete graph on them): 30 transitions, or 12 among the four
# states without a bar for a top Dock.
WALK="mac/stacking mac/tiling mac/stacking next/stacking mac/stacking
next/tiling mac/stacking inwindow/stacking mac/stacking inwindow/tiling
mac/tiling next/stacking mac/tiling next/tiling mac/tiling
inwindow/stacking mac/tiling inwindow/tiling next/stacking next/tiling
next/stacking inwindow/stacking next/stacking inwindow/tiling
next/tiling inwindow/stacking next/tiling inwindow/tiling
inwindow/stacking inwindow/tiling mac/stacking"
WALK_NOBAR="next/stacking next/tiling next/stacking inwindow/stacking
next/stacking inwindow/tiling next/tiling inwindow/stacking next/tiling
inwindow/tiling inwindow/stacking inwindow/tiling next/stacking"

walk() {
	if [ "$EDGE" = top ]; then
		w=$WALK_NOBAR
	else
		w=$WALK
	fi
	for s in $w; do
		st=${s%/*}
		md=${s#*/}
		[ "$st" != "$STYLE" ] && set_style "$st"
		[ "$md" != "$MODE" ] && set_mode "$md"
		verify
	done
}

# ---- run ----

"$B/WindowServer" --headless 1280x800 --socket "$SOCK" --config /dev/null -v \
    >"$LOG/server.log" 2>&1 &
SPID=$!
wait_for "$LOG/server.log" "ready:" 10 || { echo "FAIL: server didn't start"; cat "$LOG/server.log"; exit 1; }
displays
BUILTIN=$(awk 'NR == 1 { print $1 }' "$LOG/displays")
MX=0

start A bar A d0d0d0 c03030
APID=$LAST
start clock item clock 80 2000 404040
set_dock bottom

for disp in single mirrored right left; do
	set_disp $disp
	for edge in left bottom right top; do
		# a top Dock only outside the Mac style (the Dock refuses
		# top there, apps-dock)
		if [ "$edge" = top ] && [ "$STYLE" = mac ]; then
			set_style next
		fi
		set_dock $edge
		walk
		# hotplug in this state and in the next style
		if [ "$disp" != single ]; then
			for st in $([ "$edge" = top ] && echo next inwindow ||
			    echo mac next); do
				set_style $st
				keep=$DISP
				set_disp single
				verify
				set_disp $keep
				verify
			done
		fi
		[ "$STYLE" != mac ] && [ "$edge" != top ] && set_style mac
	done
	[ "$STYLE" != mac ] && set_style mac
done

STATE=end
kill -0 "$SPID" 2>/dev/null && pass "server still running" ||
    fail "server died"

echo
if [ $FAILS -eq 0 ]; then
	echo "all $CHECKS menu bar matrix checks passed"
	rm -rf "$LOG"
	exit 0
fi
echo "$FAILS of $CHECKS checks failed; logs in $LOG"
exit 1
