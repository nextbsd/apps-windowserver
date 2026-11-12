#!/bin/sh
# Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless test of drag and drop ("Drag and drop" in WSProtocol.h):
# drags inside one app and between two, the image above every window,
# the operations each window takes and the cursor for them, the
# modifiers narrowing the operations, a drop nobody takes sliding back,
# Escape cancelling, and who may start a drag. Runs wsdrag (two apps,
# six windows, see tools/wsdrag.c) and drives it with wsctl.
#
#   sh tests/drag-test.sh [BUILD_DIR]
#
# Needs no privileges and doesn't touch the console or the GPU.

B=${1:-build}
export WS_SERVICE_NAME="org.nextbsd.windowserver.dtest.$$"
export WS_BackgroundColor=202020
LOG="/tmp/ws-dtest-$$"
SOCK="$LOG/ws.sock"
mkdir -p "$LOG" || exit 1
FAILS=0
SPID=
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

expect_not_pixel() {
	got=$("$B/wsctl" pixel "$1" "$2" | awk '{print $2}')
	if [ "$got" != "$3" ]; then
		pass "pixel $1,$2 isn't $3 ($4)"
	else
		fail "pixel $1,$2 is $3 ($4)"
	fi
}

# The drag image: magenta at 80% over what is below, so red and blue
# above 0xc0 and green below 0x40.
expect_magenta() {
	got=$("$B/wsctl" pixel "$1" "$2" | awk '{print $2}')
	r=$(printf '%d' "0x$(echo "$got" | cut -c1-2)" 2>/dev/null)
	g=$(printf '%d' "0x$(echo "$got" | cut -c3-4)" 2>/dev/null)
	b=$(printf '%d' "0x$(echo "$got" | cut -c5-6)" 2>/dev/null)
	if [ -n "$got" ] && [ "$r" -ge 192 ] && [ "$b" -ge 192 ] && [ "$g" -lt 64 ]; then
		pass "pixel $1,$2 is the drag image ($got, $3)"
	else
		fail "pixel $1,$2 is $got, not the drag image ($3)"
	fi
}

M=0
mark() {
	"$B/wsctl" sync
	sleep 0.5
	M=$(wc -l <"$LOG/wsdrag.log")
}

seen() {
	awk -v from="$1" -v to="$2" 'NR > from && NR <= to' \
	    "$LOG/wsdrag.log" | grep -q -E -- "$3"
}

check() {
	if seen "$2" "$3" "$4"; then pass "$1"; else fail "$1"; fi
}

check_not() {
	if seen "$2" "$3" "$4"; then fail "$1"; else pass "$1"; fi
}

# serial of the first line matching PATTERN in FROM+1..TO
serial_of() {
	awk -v from="$1" -v to="$2" 'NR > from && NR <= to' \
	    "$LOG/wsdrag.log" | grep -E -- "$3" | head -1 |
	    sed -n 's/.*serial=\([0-9]*\).*/\1/p'
}

state() {
	"$B/wsctl" drag
}

"$B/WindowServer" --headless 1280x800 --socket "$SOCK" --config /dev/null -v \
    >"$LOG/server.log" 2>&1 &
SPID=$!
wait_for "$LOG/server.log" "ready:" 10 || { fail "server didn't start"; cat "$LOG/server.log"; exit 1; }
pass "server started (pid $SPID, service $WS_SERVICE_NAME)"

"$B/wsdrag" >"$LOG/wsdrag.log" 2>&1 &
DPID=$!
wait_for "$LOG/wsdrag.log" "^ready" 10 || { fail "wsdrag didn't get ready"; cat "$LOG/wsdrag.log"; exit 1; }
pass "wsdrag connected two apps and created six windows"
"$B/wsctl" list
ALPHA=$(awk '/^connected app=Alpha/ { sub("client=", "", $3); print $3 }' "$LOG/wsdrag.log")
expect_pixel 250 250 c03030 "A1 shows"
expect_pixel 750 200 30a040 "B1 shows"
state | grep -q "^drag none sessions=0" && pass "no drag yet" || fail "no drag yet: $(state)"
mark
P0=$M

# 1. From Alpha to Beta: B1 takes a copy.
"$B/wsctl" move 200 200
"$B/wsctl" down
mark
P1=$M
check "a press in A1 started a drag" "$P0" "$P1" 'drag-begin app=Alpha ok session=1'
check "A1, Alpha's own window, heard the drag enter, with Alpha's local operations" \
    "$P0" "$P1" 'app=Alpha type=drag win=A1 phase=enter session=1 x=100 y=100 ops=0x17 src='"$ALPHA"' pb=NSDragPboard'
S=$(state)
echo "$S"
echo "$S" | grep -q "session=1 source=$ALPHA target=[0-9]* operation=0x10 operations=0x17 image=180,180,40x40 sessions=1 types=NSFilenamesPboardType,NSStringPboardType" &&
    pass "the drag state: A1 under it takes Move, the image at the pointer, both types" ||
    fail "the drag state: $S"
expect_magenta 185 185 "the image is held at its middle"

"$B/wsctl" move 500 200
"$B/wsctl" move 700 200
mark
P2=$M
check "A1 heard the drag leave" "$P1" "$P2" 'app=Alpha type=drag win=A1 phase=exit'
check "B1 heard it enter, with the operations for other apps" "$P1" "$P2" \
    'app=Beta type=drag win=B1 phase=enter session=1 x=100 y=100 ops=0x7 '
check "Alpha heard B1 takes a copy" "$P1" "$P2" \
    'app=Alpha type=drag-status win=- session=1 target=B1 op=0x1 ops=0x7'
check_not "no pointer motion reached anyone during the drag" "$P1" "$P2" \
    'type=mouse-(dragged|entered|exited)'
expect_magenta 685 185 "the image follows the pointer, above B1"
expect_pixel 713 216 28b440 "the copy cursor's green plus"

# Control narrows the operations to Link, which B1 doesn't take.
"$B/wsctl" keydown Control_L
mark
P3=$M
check "Control: B1 hears only Link is offered" "$P2" "$P3" \
    'app=Beta type=drag win=B1 phase=update session=1 .* ops=0x2 '
check "...and Alpha hears B1 takes nothing" "$P2" "$P3" \
    'type=drag-status win=- session=1 target=B1 op=0x0 ops=0x2'
expect_not_pixel 713 216 28b440 "no plus: the arrow"
"$B/wsctl" keyup Control_L
mark
P4=$M
check "Control up: Copy again" "$P3" "$P4" \
    'type=drag-status win=- session=1 target=B1 op=0x1 ops=0x7'

# The overview doesn't open during a drag.
"$B/wsctl" overview all
"$B/wsctl" overview status | grep -q "closed" && pass "no overview during a drag" ||
    fail "the overview opened during a drag: $("$B/wsctl" overview status)"

"$B/wsctl" up
mark
P5=$M
check "B1 got the drop" "$P4" "$P5" 'app=Beta type=drag win=B1 phase=drop session=1 x=100 y=100 ops=0x7'
check "the release went to A1, where the button went down" "$P4" "$P5" \
    'app=Alpha type=mouse-up win=A1'
check "Alpha heard the drag ended: Copy, on B1" "$P4" "$P5" \
    'app=Alpha type=drag-end win=- session=1 op=0x1 target=B1 dropped$'
UP=$(serial_of "$P4" "$P5" 'app=Alpha type=mouse-up')
END=$(serial_of "$P4" "$P5" 'app=Alpha type=drag-end')
[ -n "$UP" ] && [ -n "$END" ] && [ "$UP" -lt "$END" ] &&
    pass "the release came before the end ($UP < $END)" ||
    fail "the release ($UP) didn't come before the end ($END)"
state | grep -q "^drag none sessions=1" && pass "no drag after the drop" || fail "after the drop: $(state)"
expect_pixel 685 185 30a040 "the image went away"

# 2. Inside Alpha: A1 to A2, a move.
"$B/wsctl" move 200 200
"$B/wsctl" down
"$B/wsctl" move 200 350
"$B/wsctl" move 200 500
mark
P6=$M
check "A2 heard the drag enter, with Alpha's local operations" "$P5" "$P6" \
    'app=Alpha type=drag win=A2 phase=enter session=2 x=100 y=100 ops=0x17'
"$B/wsctl" up
mark
P7=$M
check "A2 got the drop" "$P6" "$P7" 'app=Alpha type=drag win=A2 phase=drop session=2'
check "Alpha heard the drag ended: Move, on A2" "$P6" "$P7" \
    'app=Alpha type=drag-end win=- session=2 op=0x10 target=A2 dropped$'

# 3. B2 takes a link; the link cursor.
"$B/wsctl" move 200 200
"$B/wsctl" down
"$B/wsctl" move 500 450
"$B/wsctl" move 700 500
mark
P8=$M
check "B2 takes a link" "$P7" "$P8" 'type=drag-status win=- session=3 target=B2 op=0x2'
expect_pixel 713 513 ffffff "the link cursor's box"
"$B/wsctl" up
mark
P9=$M
check "Alpha heard the drag ended: Link, on B2" "$P8" "$P9" \
    'app=Alpha type=drag-end win=- session=3 op=0x2 target=B2 dropped$'

# 4. B4 takes nothing: the image slides back.
"$B/wsctl" move 200 200
"$B/wsctl" down
"$B/wsctl" move 700 450
"$B/wsctl" move 1050 500
mark
P10=$M
check "B4 heard the drag and took nothing" "$P9" "$P10" 'drag-answer app=Beta win=B4 op=0x0'
"$B/wsctl" up
S=$(state)
echo "$S" | grep -q "sliding" && pass "the image slides back ($S)" || echo "note: slide already over: $S"
sleep 0.6
mark
P11=$M
check "B4 heard the drag leave" "$P10" "$P11" 'app=Beta type=drag win=B4 phase=exit session=4'
check_not "...and got no drop" "$P10" "$P11" 'win=B4 phase=drop'
check "Alpha heard the drag ended with nothing, after the slide" "$P10" "$P11" \
    'app=Alpha type=drag-end win=- session=4 op=0x0 target=- slid-back$'
expect_pixel 1030 480 8030a0 "the image is gone from B4"

# 5. B3 doesn't take drags: it hears nothing.
"$B/wsctl" move 200 200
"$B/wsctl" down
"$B/wsctl" move 700 150
"$B/wsctl" move 1050 150
mark
P12=$M
check "Alpha heard nothing takes drags under it" "$P11" "$P12" \
    'type=drag-status win=- session=5 target=- op=0x0'
"$B/wsctl" up
sleep 0.6
mark
P13=$M
check_not "B3 heard nothing of the drag" "$P11" "$P13" 'type=drag win=B3'
check "the drag over B3 ended with nothing" "$P12" "$P13" \
    'app=Alpha type=drag-end win=- session=5 op=0x0 target=- slid-back$'

# 6. Escape cancels; the release then goes to nobody.
"$B/wsctl" move 200 200
"$B/wsctl" down
"$B/wsctl" move 700 200
"$B/wsctl" key Escape
sleep 0.6
mark
P14=$M
check "Escape: B1 heard the drag leave" "$P13" "$P14" 'app=Beta type=drag win=B1 phase=exit session=6'
check "Escape: Alpha heard it was cancelled" "$P13" "$P14" \
    'app=Alpha type=drag-end win=- session=6 op=0x0 target=- cancelled slid-back$'
check_not "Escape reached no app" "$P13" "$P14" 'type=key-down'
"$B/wsctl" up
mark
P15=$M
check_not "the release after a cancel went to nobody" "$P14" "$P15" 'type=mouse-up'
expect_pixel 750 200 30a040 "B1 shows again, no image"

# 7. Who may start a drag.
sleep 0.7
kill -USR1 "$DPID"
mark
P16=$M
check "Beta can't start a drag with nothing pressed" "$P15" "$P16" 'drag-begin app=Beta refused EPERM'
"$B/wsctl" key b
mark
P17=$M
check "Alpha can't either, with no button held" "$P16" "$P17" 'drag-begin app=Alpha refused EPERM'
check_not "no errors" 0 "$P17" 'type=error'
state | grep -q "^drag none sessions=6" && pass "six drags, none left" || fail "at the end: $(state)"
grep -q "drag" "$LOG/server.log" && pass "the server logged the drags"

echo
if [ "$FAILS" -eq 0 ]; then
	echo "drag test passed"
	exit 0
fi
echo "drag test: $FAILS failure(s); logs in $LOG"
exit 1
