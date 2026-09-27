#!/bin/bash
# Controllers for guest programs, no VM: steamarm-inputd (tools/inputd) reads
# the Mac's game controllers and publishes each player of the launcher's
# Entrada page as /tmp/lxrt-input/eventN, which the runtime shows guests as
# /dev/input/eventN (runtime/evdev.c, tools/inputd/PROTOCOL.md).
#
#   scripts/input.sh start | stop | status
set -u
BUILD="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
DIR=/tmp/lxrt-input
LOG="$STATE/logs/inputd.log"
cd "$(dirname "$0")/.." || exit 1
BIN=""
for b in "$BUILD/steamarm-inputd" build/steamarm-inputd; do
    [ -x "$b" ] && { BIN="$b"; break; }
done

running() { [ -f "$DIR/inputd.pid" ] && kill -0 "$(cat "$DIR/inputd.pid" 2>/dev/null)" 2>/dev/null; }

case "${1:-status}" in
start)
    running && exit 0
    [ -n "$BIN" ] || { echo "input: steamarm-inputd not built (tools/inputd/build.sh)" >&2; exit 1; }
    mkdir -p "$STATE/logs"
    STEAMARM_STATE="$STATE" nohup "$BIN" --dir "$DIR" --config "$STATE/launcher/controllers.json" \
        >> "$LOG" 2>&1 < /dev/null &
    for _ in $(seq 1 30); do running && exit 0; sleep 0.1; done
    echo "input: steamarm-inputd did not start (log $LOG)" >&2
    exit 1
    ;;
stop)
    running && kill "$(cat "$DIR/inputd.pid")"
    true
    ;;
status)
    if running; then
        echo "input: running (pid $(cat "$DIR/inputd.pid")), devices: $(cd "$DIR" && ls -d event* 2>/dev/null | tr '\n' ' ')"
    else
        echo "input: stopped"
    fi
    ;;
*) sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
