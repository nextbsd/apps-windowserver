#!/bin/sh
# Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
#
# Check the menu bar on every display (apps-windowserver#100) on a
# running session, read-only: run it after each menu style, window
# mode, Dock edge or display change, on a real machine. It changes
# nothing; it reads wsctl screen, displays and list, and checks:
#
#   - every display of the desktop has a strip while the active style
#     has one (menubar > 0), and none otherwise; its usable area starts
#     below the strip (and a top Dock), a mirror's is its frame;
#   - at most one live bar (WS_LEVEL_MENU_BAR) is on screen, along the
#     top of one display of the desktop, exactly as wide as it, never
#     across two; the status items (WS_LEVEL_STATUS_ITEM) are in that
#     display's strip; none of either without a strip;
#   - outside the Mac style, the active app's NeXT main menu (level 20)
#     has its top at the top of the main display's usable area, inside
#     it.
#
# The dimmed copies are pixels: tests/menubar-displays-test.sh and
# tests/menubar-matrix-test.sh check them headless; on a GPU session
# (Renderer gles2, where snapshots aren't available) look at them.
#
#   sh tests/menubar-live-check.sh [WSCTL] [LABEL]

W=${1:-/usr/libexec/WindowServer/wsctl}
LABEL=${2:-now}
T=$(mktemp -d /tmp/mbcheck.XXXXXX) || exit 1
FAILS=0

fail() {
	echo "FAIL: [$LABEL] $*"
	FAILS=$((FAILS + 1))
}

pass() {
	echo "ok:   [$LABEL] $*"
}

"$W" screen >"$T/screen" || { echo "FAIL: wsctl screen"; exit 1; }
"$W" displays >"$T/displays" || { echo "FAIL: wsctl displays"; exit 1; }
"$W" list >"$T/list" || { echo "FAIL: wsctl list"; exit 1; }
MB=$(awk '$1 == "menubar" { print $2 }' "$T/screen")
SI=$(awk '$1 == "statusitems" { print $2 }' "$T/screen")
echo "[$LABEL] menu bar strip $MB, status items $SI"
sed "s/^/[$LABEL]   /" "$T/displays"

# each display: id x y w h ux uy uw uh mirror(0/1) main(0/1)
awk '{
	f = ""; u = ""; m = 0; mn = 0
	for (i = 1; i <= NF; i++) {
		if ($i ~ /^frame=/) f = substr($i, 7)
		if ($i ~ /^usable=/) u = substr($i, 8)
		if ($i ~ /^mirror-of=/) m = 1
		if ($i == "main") mn = 1
	}
	gsub(/[,x]/, " ", f); gsub(/[,x]/, " ", u)
	print $1, f, u, m, mn
}' "$T/displays" >"$T/geo"

# a top Dock: a Dock window as wide as the main display along its top,
# just below the strip; its strip comes off below the bar's
MAINGEO=$(awk '$11 == 1 { print $2, $3 + '"$MB"', $4 }' "$T/geo")
TOPDOCK=$(echo "$MAINGEO" | awk -v list="$T/list" '{ x = $1; y = $2; w = $3
    while ((getline l < list) > 0) { if (l !~ /level=21 /) continue
    n = split(l, f, / +/); for (i = 1; i <= n; i++) if (f[i] ~ /^frame=/) {
    split(substr(f[i], 7), a, /[,x]/)
    if (a[1] == x && a[2] == y && a[3] == w) print a[2] + a[4] } } }' |
    sort -n | tail -1)

while read -r id x y w h ux uy uw uh mir main; do
	if [ "$mir" = 1 ]; then
		[ "$ux,$uy,$uw,$uh" = "$x,$y,$w,$h" ] &&
		    pass "display $id (a mirror): usable area is its frame" ||
		    fail "display $id (a mirror): usable $ux,$uy,${uw}x$uh, frame $x,$y,${w}x$h"
		continue
	fi
	top=$((y + MB))
	if [ "$main" = 1 ] && [ -n "$TOPDOCK" ] && [ "$TOPDOCK" -gt "$top" ]; then
		top=$TOPDOCK
	fi
	if [ "$uy" = "$top" ]; then
		pass "display $id: usable area starts at y=$uy (strip $MB)"
	else
		fail "display $id: usable area starts at y=$uy, expected $top (strip $MB)"
	fi
	[ $((uy + uh)) -le $((y + h)) ] && [ "$ux" -ge "$x" ] &&
	    [ $((ux + uw)) -le $((x + w)) ] &&
	    pass "display $id: usable area inside the display" ||
	    fail "display $id: usable area $ux,$uy,${uw}x$uh outside $x,$y,${w}x$h"
done <"$T/geo"

# the live bar and the items
awk '/level=24 / || /level=25 / { for (i = 1; i <= NF; i++)
    if ($i ~ /^frame=/) { f = substr($i, 7); gsub(/[,x]/, " ", f) }
    lv = ($0 ~ /level=24 /) ? 24 : 25; print lv, f }' "$T/list" >"$T/bars"
NB=$(awk '$1 == 24' "$T/bars" | wc -l | tr -d ' ')
NI=$(awk '$1 == 25' "$T/bars" | wc -l | tr -d ' ')
if [ "$MB" = 0 ]; then
	[ "$NB" = 0 ] && [ "$NI" = 0 ] &&
	    pass "no strip: no bar and no status item on screen" ||
	    fail "no strip, yet $NB bar(s) and $NI item(s) on screen"
else
	[ "$NB" -le 1 ] && pass "$NB live bar on screen" ||
	    fail "$NB bars on screen at once"
	FOCUS=
	if [ "$NB" = 1 ]; then
		read -r lv bx by bw bh <<EOF
$(awk '$1 == 24' "$T/bars")
EOF
		FOCUS=$(awk -v bx="$bx" -v by="$by" -v bw="$bw" '$10 == 0 &&
		    $2 == bx && $3 == by && $4 == bw { print $1 }' "$T/geo")
		[ -n "$FOCUS" ] &&
		    pass "the live bar is along display $FOCUS's top, as wide as it" ||
		    fail "the live bar at $bx,$by ${bw}x$bh isn't one display's strip (across two?)"
	fi
	if [ -n "$FOCUS" ]; then
		read -r id x y w h rest <<EOF
$(awk -v f="$FOCUS" '$1 == f' "$T/geo")
EOF
		awk -v x="$x" -v y="$y" -v w="$w" '$1 == 25 && ($2 < x ||
		    $2 + $4 > x + w || $3 != y)' "$T/bars" | grep -q . &&
		    fail "a status item outside display $FOCUS's strip" ||
		    pass "the $NI status item(s) in display $FOCUS's strip"
	fi
fi

# the NeXT main menu, outside the Mac style
if [ "$MB" = 0 ]; then
	read -r id x y w h ux uy uw uh rest <<EOF
$(awk '$11 == 1' "$T/geo")
EOF
	awk '/level=20 / && / active / { for (i = 1; i <= NF; i++)
	    if ($i ~ /^frame=/) { f = substr($i, 7); gsub(/[,x]/, " ", f);
	    print f } }' "$T/list" | head -1 >"$T/menu"
	if [ -s "$T/menu" ]; then
		read -r mx my mw mh <"$T/menu"
		[ "$my" = "$uy" ] && [ "$mx" -ge "$ux" ] &&
		    [ $((mx + mw)) -le $((ux + uw)) ] &&
		    pass "the NeXT main menu at the top of the main display's usable area ($mx,$my)" ||
		    fail "the NeXT main menu at $mx,$my, the main display's usable area is $ux,$uy ${uw}x$uh"
	else
		echo "note: [$LABEL] no active app's main menu on screen (in-window style, or none)"
	fi
fi

rm -rf "$T"
[ $FAILS = 0 ] && echo "[$LABEL] all checks passed" ||
    echo "[$LABEL] $FAILS check(s) failed"
exit $((FAILS != 0))
