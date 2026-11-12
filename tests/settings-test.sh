#!/bin/sh
# Copyright (c) 2026, Joseph Maloney
# SPDX-License-Identifier: BSD-2-Clause
#
# Headless test of settings that change while WindowServer runs
# (WS_SERVER_SETTINGS): wsctl settings get/set/unset/reload/reset, the
# server-only and unknown keys refused, the keymap compiled again with a
# layout list (us,gr), WS_EVENT_KEYMAP and WS_EVENT_MODIFIERS sent
# again to an XKB connection, keysyms and text of a plain connection
# following the active layout, switching layouts with a request and
# with the KeyboardLayoutShortcut keys, WS_EVENT_KEYBOARD_LAYOUT and
# WS_EVENT_SETTINGS_CHANGED to tools, a setting read on every event
# (NaturalScrolling), key repeat values, the input device list, and
# SIGHUP reading the system's settings again instead of stopping.
#
# It runs against a headless server that is already running, started
# by ws-session so its Mach service stays registered:
#
#   WS_SERVICE_NAME=org.nextbsd.windowserver.mytest \
#       ws-session start --headless 1280x800 --server build/WindowServer
#   WS_SERVICE_NAME=org.nextbsd.windowserver.mytest \
#       sh tests/settings-test.sh [BUILD_DIR]
#   WS_SERVICE_NAME=org.nextbsd.windowserver.mytest ws-session stop
#
# Its session values are dropped again at the end (wsctl settings
# reset). SIGHUP is sent only when it runs as root (as ws-session's
# job, the server runs as root until it drops to _windowserver).

B=${1:-build}
if [ -z "$WS_SERVICE_NAME" ]; then
	echo "settings-test: set WS_SERVICE_NAME to a running headless server's" >&2
	exit 2
fi
export WS_SERVICE_NAME
LOG=${TMPDIR:-/tmp}/ws-settings-test-$$
mkdir -p "$LOG" || exit 1
FAILS=0
TPID=
WPID=

fail() {
	echo "FAIL: $*"
	FAILS=$((FAILS + 1))
}

pass() {
	echo "ok:   $*"
}

cleanup() {
	"$B/wsctl" settings reset 2>/dev/null
	[ -n "$WPID" ] && kill "$WPID" 2>/dev/null
	[ -n "$TPID" ] && kill "$TPID" 2>/dev/null
	wait 2>/dev/null
}
trap cleanup EXIT INT TERM

# wait_for FILE PATTERN SECONDS
wait_for() {
	i=0
	while [ $i -lt $(($3 * 10)) ]; do
		grep -q -E -- "$2" "$1" 2>/dev/null && return 0
		sleep 0.1
		i=$((i + 1))
	done
	return 1
}

# count FILE PATTERN: matching lines
count() {
	grep -c -E -- "$2" "$1" 2>/dev/null
}

# setting KEY: the value in effect
setting() {
	"$B/wsctl" settings get "$1"
}

# expect_setting KEY VALUE WHAT
expect_setting() {
	got=$(setting "$1")
	if [ "$got" = "$2" ]; then
		pass "$1 is \"$2\" ($3)"
	else
		fail "$1 is \"$got\", expected \"$2\" ($3)"
	fi
}

# refused EXPECTED-ERROR-TEXT WHAT -- wsctl ARGS...
refused() {
	_want=$1
	_what=$2
	shift 3
	if "$B/wsctl" "$@" >"$LOG/err" 2>&1; then
		fail "$_what was accepted"
	elif grep -q -- "$_want" "$LOG/err"; then
		pass "$_what refused ($(cat "$LOG/err"))"
	else
		fail "$_what: $(cat "$LOG/err"), expected $_want"
	fi
}

# key_text NAME: type NAME, print the text of the first key-down Beta
# gets that has text (not the modifiers')
key_text() {
	_n=$(wc -l <"$LOG/wstest.log")
	"$B/wsctl" key "$1"
	"$B/wsctl" sync
	sleep 0.3
	awk -v from="$_n" 'NR > from && $3 == "app=Beta" &&
	    $4 == "type=key-down" { for (i = 5; i <= NF; i++)
	    if ($i ~ /^text=/) { sub("text=", "", $i); gsub("\"", "", $i);
	    if ($i != "") { print $i; exit } } }' "$LOG/wstest.log"
}

"$B/wsctl" sync || { echo "settings-test: no server at $WS_SERVICE_NAME"; exit 1; }
"$B/wsctl" settings reset || { fail "settings reset"; exit 1; }
if ! "$B/wsctl" settings get >"$LOG/get" 2>&1; then
	fail "the server has no settings requests: $(cat "$LOG/get")"
	exit 1
fi
pass "settings get lists $(wc -l <"$LOG/get" | tr -d ' ') keys"
SYS_LAYOUT=$(setting KeyboardLayout)
grep -q '^DRMDevice .*server only' "$LOG/get" &&
    grep -q '^KeyboardLayout .*(built-in)$\|^KeyboardLayout .*(system)$' "$LOG/get" &&
    pass "server-only keys and sources are shown" ||
    fail "settings get: $(head -3 "$LOG/get")"

"$B/wstest" -t 120 >"$LOG/wstest.log" 2>&1 &
TPID=$!
wait_for "$LOG/wstest.log" "^ready" 10 || { fail "wstest didn't start"; cat "$LOG/wstest.log"; exit 1; }
"$B/wsctl" layout watch 100 >"$LOG/watch.log" 2>&1 &
WPID=$!
wait_for "$LOG/watch.log" "^watching" 5 || fail "layout watch didn't start"

# Beta (a plain connection) has the key window.
t=$(key_text a)
[ "$t" = "a" ] && pass "us: a types \"a\"" || fail "us: a typed \"$t\""

# ---- refused changes ----
refused "Operation not permitted" "a server-only key (DRMDevice)" -- \
    settings set DRMDevice /dev/dri/card9
refused "Invalid argument" "an unknown key" -- settings set NoSuchKey 1
refused "Invalid argument" "PointerSpeed 3" -- settings set PointerSpeed 3
refused "Invalid argument" "a layout XKB doesn't know" -- \
    settings set KeyboardLayout nosuchlayout
refused "Invalid argument" "five layouts" -- \
    settings set KeyboardLayout us,gr,de,fr,ru
refused "Invalid argument" "a bad shortcut" -- \
    settings set KeyboardLayoutShortcut Hyper
refused "Invalid argument" "a bad boolean" -- settings set ClickThrough maybe
expect_setting KeyboardLayout "$SYS_LAYOUT" "nothing changed after refusals"
# A refused key fails the whole request.
refused "DRMDevice" "a request with one bad key" -- \
    settings set NaturalScrolling true DRMDevice x
expect_setting NaturalScrolling false "the good key of a refused request"

# ---- a layout list ----
K0=$(count "$LOG/wstest.log" "app=Gamma type=keymap")
if "$B/wsctl" settings set KeyboardLayout us,gr; then
	pass "KeyboardLayout us,gr accepted"
else
	fail "KeyboardLayout us,gr"
fi
"$B/wsctl" sync
sleep 0.5
"$B/wsctl" settings get >"$LOG/get"
grep -q '^KeyboardLayout *= us,gr (session)$' "$LOG/get" &&
    pass "settings get: KeyboardLayout us,gr from the session" ||
    fail "settings get: $(grep '^KeyboardLayout ' "$LOG/get")"
K1=$(count "$LOG/wstest.log" "app=Gamma type=keymap")
[ "$K1" -gt "$K0" ] && pass "the XKB connection got the new keymap" ||
    fail "no new keymap for the XKB connection ($K0 -> $K1)"
grep -q "app=Gamma type=keymap.*text=xkb_keymap sealed=yes" "$LOG/wstest.log" &&
    pass "the new keymap is a sealed xkb_keymap" ||
    fail "keymap event: $(grep 'app=Gamma type=keymap' "$LOG/wstest.log" | tail -1)"
awk '$3 == "app=Gamma" && $4 == "type=keymap" { k = NR }
    k && NR > k && $3 == "app=Gamma" && $4 == "type=modifiers" { m = 1 }
    END { exit !m }' "$LOG/wstest.log" &&
    pass "the XKB connection got the state after the keymap" ||
    fail "no modifiers after the new keymap"
wait_for "$LOG/watch.log" "settings-changed" 3 &&
    pass "tools hear settings-changed" || fail "no settings-changed"
"$B/wsctl" layout list >"$LOG/layouts"
if [ "$(wc -l <"$LOG/layouts" | tr -d ' ')" = 2 ] &&
    grep -q '^0 us "English (US)" active$' "$LOG/layouts" &&
    grep -q '^1 gr "Greek"$' "$LOG/layouts"; then
	pass "layout list: us (active), gr"
else
	fail "layout list: $(cat "$LOG/layouts")"
fi
t=$(key_text a)
[ "$t" = "a" ] && pass "still us: a types \"a\"" || fail "us,gr: a typed \"$t\""

# ---- switching with the request ----
"$B/wsctl" layout set gr && pass "layout set gr" || fail "layout set gr"
wait_for "$LOG/watch.log" '^layout 1 gr "Greek" active' 3 &&
    pass "tools hear: layout 1 gr active" ||
    fail "no layout event: $(tail -2 "$LOG/watch.log")"
t=$(key_text a)
[ "$t" = "α" ] && pass "gr: a types \"α\" (plain connection)" ||
    fail "gr: a typed \"$t\""
t=$(key_text shift+b)
[ "$t" = "Β" ] && pass "gr: shift+b types \"Β\"" || fail "gr: shift+b typed \"$t\""
grep "app=Beta type=key-down" "$LOG/wstest.log" | grep -q 'keysym=0x7e1 ' &&
    pass "gr: the keysym is Greek_alpha" ||
    fail "keysym: no Greek_alpha key-down for Beta"
refused "Invalid argument" "layout 5" -- layout set 5

# The XKB connection gets keys: click its window (Gamma's W4 at 900,100).
"$B/wsctl" click 1000 150
"$B/wsctl" sync
sleep 0.3
awk '$3 == "app=Gamma" && $4 == "type=modifiers" { l = $0 } END { print l }' \
    "$LOG/wstest.log" | grep -q 'group=1' &&
    pass "the XKB connection's state says group 1" ||
    fail "XKB modifiers: $(grep 'app=Gamma type=modifiers' "$LOG/wstest.log" | tail -1)"
"$B/wsctl" layout set 0
"$B/wsctl" sync
sleep 0.3
awk '$3 == "app=Gamma" && $4 == "type=modifiers" { l = $0 } END { print l }' \
    "$LOG/wstest.log" | grep -q 'group=0' &&
    pass "layout set 0: the XKB connection hears group 0" ||
    fail "XKB modifiers: $(grep 'app=Gamma type=modifiers' "$LOG/wstest.log" | tail -1)"
# Back to Beta.
"$B/wsctl" click 500 500
"$B/wsctl" sync

# ---- the active layout survives a new keymap ----
"$B/wsctl" layout set gr
"$B/wsctl" settings set KeyboardLayoutShortcut AltShift ||
    fail "KeyboardLayoutShortcut AltShift"
"$B/wsctl" sync
sleep 0.3
"$B/wsctl" layout list | grep -q '^1 gr "Greek" active$' &&
    pass "gr stays active across a new keymap" ||
    fail "after a new keymap: $("$B/wsctl" layout list | tr '\n' ' ')"

# ---- switching with the shortcut ----
n=$(count "$LOG/watch.log" "^layout ")
"$B/wsctl" key alt+Shift_L
"$B/wsctl" sync
sleep 0.3
tail -1 "$LOG/watch.log" | grep -q '^layout 0 us .*active' &&
    [ "$(count "$LOG/watch.log" "^layout ")" -gt "$n" ] &&
    pass "Alt+Shift switches to us, and tools hear it" ||
    fail "Alt+Shift: $(tail -1 "$LOG/watch.log")"
t=$(key_text a)
[ "$t" = "a" ] && pass "after Alt+Shift: a types \"a\"" || fail "after Alt+Shift: \"$t\""
"$B/wsctl" settings set KeyboardLayoutShortcut ControlSpace
"$B/wsctl" key ctrl+space
"$B/wsctl" sync
sleep 0.3
t=$(key_text a)
[ "$t" = "α" ] && pass "Control+Space switches to gr" || fail "after Control+Space: \"$t\""
"$B/wsctl" settings unset KeyboardLayoutShortcut
expect_setting KeyboardLayoutShortcut None "unset: back to the system's"

# ---- key repeat ----
"$B/wsctl" settings set KeyRepeatDelay 250 KeyRepeatInterval 20
"$B/wsctl" sync
sleep 0.3
grep 'app=Gamma type=keymap' "$LOG/wstest.log" | tail -1 |
    grep -q 'delay=250 interval=20' &&
    pass "repeat values reach the XKB connection" ||
    fail "repeat: $(grep 'app=Gamma type=keymap' "$LOG/wstest.log" | tail -1)"

# ---- a setting read on every event ----
"$B/wsctl" settings set NaturalScrolling true
n=$(wc -l <"$LOG/wstest.log")
"$B/wsctl" scroll 0 10
"$B/wsctl" sync
sleep 0.3
awk -v from="$n" 'NR > from && $3 == "app=Beta" && $4 == "type=scroll"' \
    "$LOG/wstest.log" | grep -q 'dy=-10' &&
    pass "NaturalScrolling true: the scroll is reversed" ||
    fail "NaturalScrolling: $(awk -v from="$n" 'NR > from && /type=scroll/' "$LOG/wstest.log" | head -1)"

# ---- device settings (headless: no devices) ----
"$B/wsctl" settings set TrackpadSpeed 0.5 MouseSpeed -0.25 \
    MouseAccelProfile flat LeftHanded true TapToClick false &&
    pass "device settings accepted" || fail "device settings"
expect_setting MouseAccelProfile flat "device setting stored"
"$B/wsctl" settings set TrackpadSpeed ""
expect_setting TrackpadSpeed "" "TrackpadSpeed \"\" falls back to PointerSpeed"
"$B/wsctl" devices >"$LOG/devices" && pass "devices: $(wc -l <"$LOG/devices" | tr -d ' ') (headless)" ||
    fail "devices"

# ---- reload from a defaults file ----
cat >"$LOG/org.nextbsd.WindowServer.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>KeyboardLayout</key>
	<string>de,gr</string>
	<key>KeyRepeatInterval</key>
	<integer>50</integer>
	<key>ScrollSpeed</key>
	<real>2.5</real>
	<key>RaiseOnClick</key>
	<false/>
	<key>DRMDevice</key>
	<string>/dev/dri/card7</string>
	<key>NSWindow Frame Something</key>
	<string>0 0 100 100</string>
</dict>
</plist>
EOF
if "$B/wsctl" settings reload "$LOG/org.nextbsd.WindowServer.plist" 2>"$LOG/err"; then
	pass "reload from a file ($(cat "$LOG/err"))"
else
	fail "reload: $(cat "$LOG/err")"
fi
expect_setting KeyboardLayout de,gr "from the file"
expect_setting KeyRepeatInterval 50 "from the file"
expect_setting ScrollSpeed 2.5 "from the file"
expect_setting RaiseOnClick false "from the file"
expect_setting DRMDevice "" "a server-only key in the file is left out"
expect_setting NaturalScrolling false "reload replaced the earlier session values"
expect_setting MouseAccelProfile adaptive "reload replaced the earlier session values"
# gr was active, and the new list has it too.
"$B/wsctl" layout list | tr '\n' ' ' |
    grep -q '^0 de "German" 1 gr "Greek" active $' &&
    pass "layout list follows the reload (gr still active)" ||
    fail "after reload: $("$B/wsctl" layout list | tr '\n' ' ')"
"$B/wsctl" layout set de || fail "layout set de (by name)"
# Keys are typed by keysym name, looked up in the first layout: ä is
# only on the German one.
t=$(key_text adiaeresis)
[ "$t" = "ä" ] && pass "de: the ä key types \"ä\"" || fail "de: ä typed \"$t\""
"$B/wsctl" settings reload "$LOG/no-such-file.plist" &&
    expect_setting KeyboardLayout "$SYS_LAYOUT" "a missing file is an empty set" ||
    fail "reload of a missing file"

# ---- SIGHUP reads the system's settings again ----
if [ "$(id -u)" = 0 ]; then
	SPID=$(pgrep -f "^$B/WindowServer .*--service $WS_SERVICE_NAME\$" | head -1)
	[ -z "$SPID" ] && SPID=$(pgrep -f "WindowServer .*--service $WS_SERVICE_NAME\$" | head -1)
	if [ -n "$SPID" ]; then
		kill -HUP "$SPID"
		sleep 0.5
		"$B/wsctl" sync && pass "SIGHUP: the server keeps running" ||
		    fail "SIGHUP stopped the server"
	else
		echo "skip: no server pid for SIGHUP"
	fi
fi

"$B/wsctl" settings reset || fail "settings reset"
if "$B/wsctl" settings get | grep -q '(session)'; then
	fail "reset left session values"
else
	pass "reset drops every session value"
fi

if [ $FAILS -eq 0 ]; then
	echo "PASS: settings ($LOG)"
	exit 0
fi
echo "$FAILS failure(s); logs in $LOG"
exit 1
