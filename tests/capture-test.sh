#!/bin/sh
# Copyright (C) 2026 Joseph Maloney <pkgdemonteam@gmail.com>
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless test of screen capture ("Screen capture" in WSProtocol.h):
# consent asked of a consent agent (none connected at first, then one
# that allows, one that refuses), frames of the screen with the
# indicator left out and the pointer only when asked, damage, scaling,
# a window's frames whether covered or not, a helper asking on behalf
# of another program, stopping and revoking (forwarded to the agent,
# or kept until one connects), one-shot captures, the indicator border
# and the agent's activity notices, and root's exemption. Runs wstest
# (four windows, see tools/wstest.c), wscapture (a capture client and a
# consent agent, see tools/wscapture.c) and wsctl.
#
#   sh tests/capture-test.sh [BUILD_DIR]
#
# Needs no privileges. Run as root, it starts the clients that need
# consent as $CAPTURE_TEST_USER (nobody by default), since root is
# exempt, and checks the exemption too; the server then runs as a
# temporary launchd job, as ws-session's do, so its Mach service is in
# the system bootstrap, which the su sessions of those clients don't
# take away when they end.

B=${1:-build}
case $B in /*) ;; *) B=$(pwd)/$B ;; esac
export WS_SERVICE_NAME="org.nextbsd.windowserver.captest.$$"
export WS_BackgroundColor=202020
# never start the real consent agent: the test plays it
export WS_CAPTURE_AGENT=none
LOG="/tmp/ws-captest-$$"
SOCK="$LOG/ws.sock"
mkdir -p "$LOG" || exit 1
chmod 755 "$LOG"
FAILS=0
SPID=
TPID=
APID=
TU=
if [ "$(id -u)" = 0 ]; then
	TU=${CAPTURE_TEST_USER:-nobody}
fi

fail() {
	echo "FAIL: $*"
	FAILS=$((FAILS + 1))
}

pass() {
	echo "ok:   $*"
}

# run "$@" as the unprivileged test user
as_user() {
	if [ -n "$TU" ]; then
		su -m "$TU" -c "exec $*"
	else
		"$@"
	fi
}

# end a background as_user job: su waits for its child, so end the child
stop_job() {
	[ -n "$1" ] || return
	pkill -P "$1" 2>/dev/null
	kill "$1" 2>/dev/null
	wait "$1" 2>/dev/null
}

cleanup() {
	stop_job "$APID"
	[ -n "$TPID" ] && kill "$TPID" 2>/dev/null
	[ -n "$SPID" ] && kill "$SPID" 2>/dev/null
	[ -n "$TU" ] && pkill -U "$TU" -x wscapture 2>/dev/null
	[ -n "$TU" ] && launchctl unload "$LOG/server.plist" 2>/dev/null
	wait 2>/dev/null
}
trap cleanup EXIT INT TERM

wait_for() {
	i=0
	while [ $i -lt $(($3 * 10)) ]; do
		grep -q -E -- "$2" "$1" 2>/dev/null && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

check_log() {
	if wait_for "$2" "$3" "${4:-5}"; then pass "$1"; else
		fail "$1"; echo "--- $2"; cat "$2"; fi
}

check_not_log() {
	if grep -q -E -- "$3" "$2" 2>/dev/null; then fail "$1"; echo "--- $2"
		cat "$2"; else pass "$1"; fi
}

screen_pixel() {
	"$B/wsctl" pixel "$1" "$2" | awk '{print $2}'
}

expect_screen() {
	got=$(screen_pixel "$1" "$2")
	if [ "$got" = "$3" ]; then
		pass "screen $1,$2 is $3 ($4)"
	else
		fail "screen $1,$2 is $got, expected $3 ($4)"
	fi
}

# the agent: POLICY, log to $LOG/agent-N.log
AN=0
agent() {
	stop_job "$APID"
	AN=$((AN + 1))
	ALOG="$LOG/agent-$AN.log"
	as_user "$B/wscapture" agent "$1" -t 120 >"$ALOG" 2>&1 &
	APID=$!
	wait_for "$ALOG" "^agent " 5
}

# a capture client in the background: NAME ARGS...; log $LOG/NAME.log
CPID=
stop_capture() {
	stop_job "$CPID"
}

capture() {
	n=$1
	shift
	as_user "$B/wscapture" stream "$@" >"$LOG/$n.log" 2>&1 &
	CPID=$!
}

if [ -n "$TU" ]; then
	cat >"$LOG/server.plist" <<EOP
<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict>
<key>Label</key><string>org.nextbsd.WindowServer.captest.$$</string>
<key>ProgramArguments</key><array><string>$B/WindowServer</string>
<string>--headless</string><string>1280x800</string>
<string>--socket</string><string>$SOCK</string>
<string>--config</string><string>/dev/null</string><string>-v</string>
<string>--service</string><string>$WS_SERVICE_NAME</string></array>
<key>MachServices</key><dict><key>$WS_SERVICE_NAME</key><true/></dict>
<key>EnvironmentVariables</key><dict>
<key>WS_BackgroundColor</key><string>202020</string></dict>
<key>RunAtLoad</key><true/>
<key>StandardOutPath</key><string>$LOG/server.log</string>
<key>StandardErrorPath</key><string>$LOG/server.log</string>
</dict></plist>
EOP
	launchctl load "$LOG/server.plist"
else
	"$B/WindowServer" --headless 1280x800 --socket "$SOCK" --config /dev/null -v \
	    >"$LOG/server.log" 2>&1 &
	SPID=$!
fi
wait_for "$LOG/server.log" "ready:" 10 || { fail "server didn't start"; cat "$LOG/server.log"; exit 1; }
pass "server started (service $WS_SERVICE_NAME)"
"$B/wstest" >"$LOG/wstest.log" 2>&1 &
TPID=$!
wait_for "$LOG/wstest.log" "W3" 10
"$B/wsctl" sync
sleep 0.5
expect_screen 700 600 3050c0 "Beta's W3 shows"
W3=$("$B/wsctl" dock list | sed -n 's/^id=\([0-9]*\) .*frame=300,250,500x400.*/\1/p')
[ -n "$W3" ] && pass "W3 is window $W3" || fail "W3's id"
[ -n "$TU" ] && echo "(clients that need consent run as $TU)"

# 1. Nobody to ask yet: the stream waits, and nothing shows.
capture s1 -t 60 -p 0,0 -p 700,600 -p 702,606
check_log "with no consent agent the stream waits, and the reply says so" \
    "$LOG/s1.log" "^started stream=[0-9]+ asking no-agent"
check_log "its state is pending" "$LOG/s1.log" "state stream=[0-9]+ pending"
"$B/wsctl" capture list >"$LOG/list1" 2>&1
grep -q "pending .*screen indicator.*name=\"wscapture\" key=.*/wscapture$" "$LOG/list1" &&
    pass "wsctl capture list shows it pending, keyed by its executable" ||
    { fail "capture list: $(cat "$LOG/list1")"; }
check_not_log "no frames before consent" "$LOG/s1.log" "^frame"
expect_screen 0 0 202020 "no indicator while only asking"

# 2. An agent connects and allows: the request waiting goes to it.
agent allow
check_log "the agent got the waiting request, naming the app and its executable" \
    "$ALOG" "consent request=[0-9]+ pid=[0-9]+ uid=[0-9]+ name=\"wscapture\" key=/.*wscapture window=0 helper=\"wscapture\""
check_log "the stream runs" "$LOG/s1.log" "state stream=[0-9]+ running"
check_log "its buffers come mapped" "$LOG/s1.log" "buffer stream=[0-9]+ index=0 gen=1 1280x800 stride=5120 mapped=yes"
check_log "the first frame is all new" "$LOG/s1.log" "frame stream=[0-9]+ seq=1 index=0 gen=1 full damage=0,0,1280x800"
check_log "frames show the screen (W3)" "$LOG/s1.log" "pixel seq=1 700,600 3050c0"
check_log "frames leave the indicator out" "$LOG/s1.log" "pixel seq=1 0,0 202020"
expect_screen 0 0 ff9500 "the indicator border shows on screen"
expect_screen 2 400 ff9500 "the border's left edge"
expect_screen 1277 400 ff9500 "the border's right edge"
expect_screen 640 798 ff9500 "the border's bottom edge"
expect_screen 640 3 202020 "the border is 3 pixels wide"
check_log "the agent heard of the activity" "$ALOG" "activity running=1 pending=0"
"$B/wsctl" capture list | grep -q "running .*screen indicator" &&
    pass "the list shows it running" || fail "list: $("$B/wsctl" capture list)"

# 3. Damage: Beta redraws W3 green.
"$B/wsctl" key c
check_log "a frame for the change, with W3's rectangle as damage" \
    "$LOG/s1.log" "frame stream=[0-9]+ seq=[0-9]+ index=[01] gen=1 damage=300,250,500x400"
check_log "the frame shows W3 green" "$LOG/s1.log" "pixel seq=[0-9]+ 700,600 20a050"

# 4. The pointer: not in frames unless asked for.
capture s2 -c -t 60 -p 702,606
check_log "a second stream, with the pointer, runs at once (the app is allowed)" \
    "$LOG/s2.log" "state stream=[0-9]+ running"
"$B/wsctl" move 700 600
"$B/wsctl" sync
sleep 0.5
onscreen=$(screen_pixel 702 606)
[ "$onscreen" != 20a050 ] && pass "the pointer is on screen at 702,606 ($onscreen)" ||
    fail "the pointer isn't on screen"
check_log "frames with -c show the pointer" "$LOG/s2.log" "pixel seq=[0-9]+ 702,606 $onscreen"
L=$(grep "^pixel .* 702,606" "$LOG/s1.log" | tail -1)
case $L in
*20a050) pass "frames without it don't ($L)" ;;
*) fail "frames without the pointer: $L" ;;
esac
"$B/wsctl" move 100 700
sleep 0.5
check_log "the pointer moving away gives the -c stream a frame without it" \
    "$LOG/s2.log" "pixel seq=[0-9]+ 702,606 20a050"
stop_capture

# 5. Scaled, and a window's own frames.
capture s3 -s 640x400 -t 60 -p 350,300 -p 0,0
check_log "a 640x400 stream gets 640x400 buffers" "$LOG/s3.log" "buffer stream=[0-9]+ index=0 gen=1 640x400 stride=2560"
check_log "its frames are the screen scaled down" "$LOG/s3.log" "pixel seq=1 350,300 20a050"
stop_capture
expect_screen 320 270 30a040 "W2 (main menu level) covers W3's corner on screen"
capture s4 -w "$W3" -t 60 -p 20,20 -p 250,200
check_log "a stream of W3 asks the agent, saying it's one window" \
    "$ALOG" "consent request=[0-9]+ .*window=$W3 window helper"
check_log "W3's frames are its frame's size" "$LOG/s4.log" "buffer stream=[0-9]+ index=0 gen=1 500x400"
check_log "W3's frames show W3 where W2 covers it on screen" "$LOG/s4.log" "pixel seq=1 20,20 20a050"
"$B/wsctl" capture list | grep -q "window=$W3" && pass "the list shows the window stream" ||
    fail "list: $("$B/wsctl" capture list)"
stop_capture

# 6. On behalf of another program.
# a program of the test user's for the helper to ask for
as_user sleep 300 &
SLEEPSU=$!
sleep 0.3
SLEEP=$(pgrep -P "$SLEEPSU" sleep || echo "$SLEEPSU")
capture s5 -b "$SLEEP" -n Firefox -t 30
check_log "a helper joined to an app may ask for another of the user's programs" \
    "$ALOG" "consent request=[0-9]+ pid=$SLEEP .*name=\"Firefox\" key=/bin/sleep window=0 on-behalf helper=\"wscapture\" helper_pid=[0-9]+"
check_log "and gets its frames once allowed" "$LOG/s5.log" "state stream=[0-9]+ running"
stop_capture
capture s6 -B "$SLEEP" -t 10
check_log "a connection that didn't join an app may not" "$LOG/s6.log" "start failed error=EPERM"
if [ -n "$TU" ]; then
	capture s7 -b 1 -t 10
	check_log "nor ask for another user's process (launchd)" "$LOG/s7.log" "start failed error=EPERM"
fi
kill "$SLEEP" "$SLEEPSU" 2>/dev/null

# 7. Stopping and revoking.
S1=$(sed -n 's/^started stream=\([0-9]*\).*/\1/p' "$LOG/s1.log")
"$B/wsctl" capture stop "$S1"
check_log "wsctl capture stop ends a stream (ECANCELED)" "$LOG/s1.log" "state stream=$S1 stopped error=ECANCELED"
sleep 0.3
check_not_log "stopping doesn't make the agent forget" "$ALOG" "^revoked"
expect_screen 0 0 202020 "the indicator goes with the last stream"
check_log "the agent heard nothing records" "$ALOG" "activity running=0"
capture s8 -t 60
check_log "a new stream of the allowed app runs" "$LOG/s8.log" "state stream=[0-9]+ running"
KEY=$(sed -n 's/^consent .* key=\([^ ]*\) window=0 helper="wscapture" helper_pid=0$/\1/p' "$ALOG" | head -1)
"$B/wsctl" capture revoke "$KEY"
check_log "wsctl capture revoke KEY ends its streams" "$LOG/s8.log" "state stream=[0-9]+ stopped error=ECANCELED"
check_log "and the agent is told to forget the app" "$ALOG" "^revoked key=$KEY"

# 8. Refused.
agent deny
capture s9 -t 30 -p 0,0
check_log "the new agent is asked" "$ALOG" "consent request=[0-9]+ .*name=\"wscapture\""
check_log "a refusal: denied, EPERM" "$LOG/s9.log" "state stream=[0-9]+ denied error=EPERM"
check_not_log "no frames after a refusal" "$LOG/s9.log" "^frame|^buffer"
expect_screen 0 0 202020 "no indicator after a refusal"
"$B/wsctl" capture image "$LOG/denied.ppm" >"$LOG/img-root" 2>&1
if [ -n "$TU" ]; then
	grep -q "denied.ppm: 1280x800" "$LOG/img-root" &&
	    pass "root's one-shot capture is exempt from consent" ||
	    fail "root's one-shot capture: $(cat "$LOG/img-root")"
	grep -q "is exempt from consent" "$LOG/server.log" &&
	    pass "and the server logs the exemption" || fail "no exemption logged"
fi

# 9. A revocation with no agent connected waits for the next one.
stop_job "$APID"
APID=
sleep 0.3
"$B/wsctl" capture revoke all
agent allow
check_log "the next agent hears of a revocation made while none was there" \
    "$ALOG" "^revoked all key="
as_user "$B/wscapture" agent allow -t 2 >"$LOG/agent-busy.log" 2>&1
grep -q "agent refused: EBUSY" "$LOG/agent-busy.log" &&
    pass "a second consent agent is refused (EBUSY)" ||
    fail "second agent: $(cat "$LOG/agent-busy.log")"

# 10. One-shot captures with consent.
as_user "$B/wsctl" capture image "$LOG/shot.ppm" >"$LOG/img" 2>&1
grep -q "shot.ppm: 1280x800" "$LOG/img" && pass "wsctl capture image (one-shot, allowed)" ||
    fail "one-shot: $(cat "$LOG/img")"
if [ -f "$LOG/shot.ppm" ]; then
	# the pixel at 700,600 of a P6 1280x800 image (16-byte header)
	px=$(dd if="$LOG/shot.ppm" bs=1 skip=$((16 + (600 * 1280 + 700) * 3)) count=3 2>/dev/null | od -An -tx1 | tr -d ' \n')
	[ "$px" = 20a050 ] && pass "the one-shot picture shows W3 green" ||
	    fail "one-shot pixel 700,600 is $px"
fi
as_user "$B/wsctl" capture image "$W3" "$LOG/w3.ppm" >"$LOG/img3" 2>&1
grep -q "w3.ppm: 500x400" "$LOG/img3" && pass "a one-shot of one window" ||
    fail "window one-shot: $(cat "$LOG/img3")"
expect_screen 0 0 202020 "one-shots don't show the indicator"

# 11. Clients going away end their streams.
capture s11 -t 60
check_log "a stream runs" "$LOG/s11.log" "state stream=[0-9]+ running"
stop_capture
sleep 0.5
n=$("$B/wsctl" capture list | grep -c "^stream=")
[ "$n" = 0 ] && pass "no streams are left" || fail "streams left: $("$B/wsctl" capture list)"
expect_screen 0 0 202020 "and no indicator"

grep -E "WARN|ERROR|error:" "$LOG/server.log" | grep -v "doesn't exist" | head -5
echo
if [ "$FAILS" -eq 0 ]; then
	echo "capture test: all passed"
	exit 0
fi
echo "capture test: $FAILS failed (logs in $LOG)"
exit 1
