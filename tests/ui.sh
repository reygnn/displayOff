#!/bin/bash
# Black-box test of displayOff under a headless mwc with simulated mouse and keyboard.
# It has its own Wayland socket, and switches no real monitor.
#
# Needs a built mwc with build/vinput and build/pngtool in MWC_DIR (by default ../mwc
# or ../mwm), grim and foot. Output goes to build/ui.log, displayOff's own to
# build/ui-do.log.
DO_DIR=$(cd "$(dirname "$0")/.." && pwd)
DO=${DO:-$DO_DIR/displayOff}
LOG=$DO_DIR/build/ui.log
DO_LOG=$DO_DIR/build/ui-do.log
if [ -z "${MWC_DIR:-}" ]; then
	MWC_DIR=$DO_DIR/../mwc
	[ -d "$MWC_DIR" ] || MWC_DIR=$DO_DIR/../mwm
fi
[ -x "$DO" ] || { echo "FAIL: $DO not built"; exit 1; }
[ -x "$DO_DIR/build/inhibit" ] || { echo "FAIL: build/inhibit not built"; exit 1; }
[ -d "$MWC_DIR" ] || { echo "FAIL: no mwc in $MWC_DIR (set MWC_DIR)"; exit 1; }
cd "$MWC_DIR" || exit 1
. tests/lib.sh
require_tools grim foot
for tool in build/vinput build/pngtool; do
	[ -x "$tool" ] || { echo "FAIL: $MWC_DIR/$tool not built (make $tool)"; exit 1; }
done

export MWC_TEST_STATE=$SMOKE_DIR/state
export LC_ALL=C.UTF-8
export HOME=$SMOKE_DIR/home
# Super+Esc finds it by name.
export PATH=$(dirname "$DO"):$PATH
mkdir -p "$HOME" "$XDG_CONFIG_HOME/mwc"
: >"$DO_LOG"
SHOT=$SMOKE_DIR/shot.png

power_is() { refresh; [ "$(global output on)" = "$1" ]; }

expect_power() { # <description> <0|1>
	wait_for "$1" power_is "$2"
}

# It must stay at this for that many seconds.
stays_power() { # <description> <0|1> <seconds>
	local end=$((SECONDS + $3))
	while [ "$SECONDS" -lt "$end" ]; do
		power_is "$2" || fail "$1: the power changed"
		sleep 0.2
	done
	echo "ok: $1"
}

daemon_start() { # <seconds>
	WAYLAND_DISPLAY=wayland-0 "$DO" -t "$1" >>"$DO_LOG" 2>&1 &
	DAEMON=$!
	sleep 0.2
}

# proc_exits <description> <pid>: must end by itself, and with status 0.
proc_exits() {
	local i status
	for ((i = 0; i < 50; i++)); do
		kill -0 "$2" 2>/dev/null || break
		sleep 0.1
	done
	kill -0 "$2" 2>/dev/null && fail "$1: still running"
	wait "$2"
	status=$?
	[ "$status" = 0 ] || fail "$1: exit status $status"
	echo "ok: $1"
}

stars_start() {
	WAYLAND_DISPLAY=wayland-0 "$DO" --stars >>"$DO_LOG" 2>&1 &
	STARS=$!
	check "the stars take the keyboard" '[ "$(get foot kbfocus)" = 0 ]'
	pixel_is "the stars cover the window" "$TITLE_X" "$TITLE_Y" 000000
}

geometry() { refresh; echo "$(get foot x) $(get foot y) $(get foot w) $(get foot h)"; }

trap 'kill -CONT "$pid" 2>/dev/null; WAYLAND_DISPLAY=wayland-0 grim "$DO_DIR/build/ui-last.png" 2>/dev/null; cleanup' EXIT

# Not start_mwc: that fixes the software renderer, and a real monitor has a GPU. Under
# pixman the first wake-up stays black until the next one, under GL it doesn't.
RENDERER=pixman
[ -e /dev/dri/renderD128 ] && RENDERER=gles2
echo "renderer: $RENDERER"
export XDG_RUNTIME_DIR=$SMOKE_DIR WLR_BACKENDS=headless WLR_LIBINPUT_NO_DEVICES=1 WLR_RENDERER=$RENDERER
unset DISPLAY
"$MWC" -s 'touch "$SMOKE_DIR/up"; exec sleep 600' >"$LOG" 2>&1 &
pid=$!
wait_for "Wayland socket" test -S "$SMOKE_DIR/wayland-0"
wait_for "mwc is up" test -e "$SMOKE_DIR/up"
start_vinput
refresh
B=$(global theme border)
T=$(global theme title)
WAYLAND_DISPLAY=wayland-0 foot >>"$DO_LOG" 2>&1 &
FOOT=$!
check "a window is up" '[ "$(get foot kbfocus)" = 1 ]'
TITLE_X=$(($(get foot x) + B + 40))
TITLE_Y=$(($(get foot y) + B + T / 2))
do_input move 1200 700
sleep 0.5
WAYLAND_DISPLAY=wayland-0 grim "$SHOT" || fail "no screenshot"
TITLE_COLOUR=$(build/pngtool pixel "$SHOT" "$TITLE_X" "$TITLE_Y")
GEOMETRY=$(geometry)

# ---- off after the timeout, on with input --------------------------------------

daemon_start 2
stays_power "still on before the timeout" 1 1
expect_power "off after the timeout" 0
[ "$(geometry)" = "$GEOMETRY" ] || fail "the window moved when the monitor went off"
do_input key a
expect_power "a key switches it on" 1
[ "$RENDERER" = pixman ] || pixel_is "the desktop is back" "$TITLE_X" "$TITLE_Y" "$TITLE_COLOUR"
[ "$(geometry)" = "$GEOMETRY" ] || fail "the window moved when the monitor came on"

expect_power "off again, the timer starts over" 0
do_input move 1100 600
expect_power "the mouse switches it on" 1
expect_power "off once more" 0
do_input click left
expect_power "a button switches it on" 1
pixel_is "the desktop is there again" "$TITLE_X" "$TITLE_Y" "$TITLE_COLOUR"
do_input key Escape     # that click also opened the root menu

# Input keeps it on, past the timeout.
for _ in 1 2 3 4 5 6 7 8; do
	do_input move $((1000 + RANDOM % 100)) 500
	sleep 0.4
	power_is 1 || fail "off although there was input"
done
echo "ok: input keeps it on"

# ---- a player asks not to be idle ----------------------------------------------

WAYLAND_DISPLAY=wayland-0 "$DO_DIR/build/inhibit" >>"$DO_LOG" 2>&1 &
INHIBIT=$!
sleep 0.2
stays_power "stays on while something inhibits" 1 4
WAYLAND_DISPLAY=wayland-0 "$DO_DIR/build/inhibit" >>"$DO_LOG" 2>&1 &
INHIBIT2=$!
sleep 0.2
kill "$INHIBIT"
wait "$INHIBIT" 2>/dev/null
stays_power "one of two going doesn't end it" 1 4
kill "$INHIBIT2"
wait "$INHIBIT2" 2>/dev/null
expect_power "off when the last one is gone" 0

# ---- ending it -----------------------------------------------------------------

kill "$DAEMON"
expect_power "ending it while off switches on" 1
proc_exits "and it ends cleanly" "$DAEMON"

# ---- --now ---------------------------------------------------------------------

now_start() {
	WAYLAND_DISPLAY=wayland-0 "$DO" --now >>"$DO_LOG" 2>&1 &
	NOW=$!
}

now_start
expect_power "--now switches off at once" 0
stays_power "and stays off without input" 0 2
kill -0 "$NOW" 2>/dev/null || fail "--now ended by itself"
do_input key a
expect_power "a key switches on" 1
proc_exits "and --now ends" "$NOW"

# The hand that clicked the menu item is still moving.
now_start
expect_power "off again" 0
do_input move 1000 500
do_input move 1010 510
stays_power "input right after it doesn't wake" 0 2
do_input move 1100 600
expect_power "later input does" 1
proc_exits "and --now ends" "$NOW"

# A player that keeps everyone from being idle must not make it impossible to wake.
WAYLAND_DISPLAY=wayland-0 "$DO_DIR/build/inhibit" >>"$DO_LOG" 2>&1 &
INHIBIT=$!
sleep 0.2
now_start
expect_power "off also while something inhibits" 0
sleep 1.5
do_input key a
expect_power "and a key still wakes" 1
proc_exits "and --now ends" "$NOW"
kill "$INHIBIT"
wait "$INHIBIT" 2>/dev/null

now_start
expect_power "off once more" 0
kill "$NOW"
expect_power "ending --now switches on" 1
proc_exits "and it ends cleanly" "$NOW"

# Beside the timer: both go when there is input, and nothing else is left over.
daemon_start 2
now_start
expect_power "--now beside the timer" 0
sleep 1.5
do_input key a
expect_power "a key switches on" 1
proc_exits "--now ends" "$NOW"
kill -0 "$DAEMON" 2>/dev/null || fail "the timer ended with --now"
kill "$DAEMON"
proc_exits "the timer still runs and ends" "$DAEMON"

# ---- the stars -----------------------------------------------------------------

stars_start
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/a.png" || fail "no screenshot"
sleep 0.2
WAYLAND_DISPLAY=wayland-0 grim "$SMOKE_DIR/b.png" || fail "no screenshot"
cmp -s "$SMOKE_DIR/a.png" "$SMOKE_DIR/b.png" && fail "the stars don't move"
echo "ok: the stars move"
do_input key a
proc_exits "a key ends the stars" "$STARS"
check "the window has the keyboard again" '[ "$(get foot kbfocus)" = 1 ]'
pixel_is "and is visible" "$TITLE_X" "$TITLE_Y" "$TITLE_COLOUR"

do_input move 1100 600
stars_start
do_input move 1101 601  # hardly anything
do_input move 1102 602
do_input move 1101 600
sleep 0.5
kill -0 "$STARS" 2>/dev/null || fail "a tiny move of the mouse ended the stars"
echo "ok: a tiny move doesn't end them"
do_input move 800 300
proc_exits "a clear move ends them" "$STARS"

stars_start
do_input click left
proc_exits "a button ends them" "$STARS"

# No move since they came up: the pointer rests on them all the same.
stars_start
do_input wheel 1
proc_exits "the wheel ends them" "$STARS"
do_input move 1200 700

# ---- together: the stars while it goes off --------------------------------------

daemon_start 2
stars_start
expect_power "off under the stars" 0
sleep 1
kill -0 "$STARS" 2>/dev/null || fail "the stars ended by themselves"
do_input key a
expect_power "a key switches it on" 1
proc_exits "and ends the stars" "$STARS"
pixel_is "the window is back" "$TITLE_X" "$TITLE_Y" "$TITLE_COLOUR"

# ---- Super+Esc -----------------------------------------------------------------

do_input key super+Escape
check "Super+Esc starts the stars" '[ "$(get foot kbfocus)" = 0 ]'
pixel_is "they are up" "$TITLE_X" "$TITLE_Y" 000000
do_input key a
check "a key ends them" '[ "$(get foot kbfocus)" = 1 ]'

printf 'Mwm*screenSaver: touch %s/saver\n' "$SMOKE_DIR" >"$XDG_CONFIG_HOME/mwc/mwmrc"
kill -HUP "$pid"
sleep 0.5
do_input key super+Escape
wait_for "Mwm*screenSaver is what it runs" test -e "$SMOKE_DIR/saver"

# ---- errors --------------------------------------------------------------------

"$DO" -t 0 2>/dev/null && fail "-t 0 was accepted"
"$DO" -t x 2>/dev/null && fail "-t x was accepted"
"$DO" --nonsense 2>/dev/null && fail "--nonsense was accepted"
echo "ok: bad arguments are refused"

# ---- shutdown with all of it still there ----------------------------------------

WAYLAND_DISPLAY=wayland-0 "$DO_DIR/build/inhibit" >>"$DO_LOG" 2>&1 &
sleep 0.2
trap cleanup EXIT
stop_vinput
stop_mwc
echo "PASS: ui"
