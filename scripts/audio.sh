#!/bin/bash
# Sound for guest programs, no VM: a PulseAudio server on the Mac (Homebrew's,
# playing through CoreAudio) whose socket lives inside the Steam root, where
# guests reach it as unix:/tmp/pulse/native (scripts/settings-env.py exports
# PULSE_SERVER). The guests' own libpulse speaks to it; pressure-vessel binds
# the socket into its container like on Linux.
#
#   scripts/audio.sh start [volume%]   start (idempotent) and set the volume
#   scripts/audio.sh volume N          set the output volume (0-100)
#   scripts/audio.sh stop | status
set -u
ROOT="${LXRT_ROOT:-/tmp/lxrt-steamroot}"
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
PA="${BREW:-/opt/homebrew}/opt/pulseaudio/bin"
DIR="$ROOT/tmp/pulse"
SOCK="$DIR/native"
LOG="$STATE/logs/pulseaudio.log"
CONF="$STATE/launcher/pulse.pa"
pactl_() { "$PA/pactl" -s "unix:$SOCK" "$@"; }

running() { [ -S "$SOCK" ] && pactl_ info >/dev/null 2>&1; }

start() {
    [ -x "$PA/pulseaudio" ] || { echo "audio: PulseAudio not installed (brew install pulseaudio)" >&2; return 1; }
    if ! running; then
        mkdir -p "$DIR" "$STATE/logs" "$(dirname "$CONF")"
        rm -f "$SOCK"
        # Only what is needed: the Mac's outputs and the guests' socket
        # (anonymous: the socket is only reachable from this user's Steam root).
        cat > "$CONF" <<EOF
load-module module-coreaudio-detect
load-module module-native-protocol-unix socket=$SOCK auth-anonymous=1
load-module module-always-sink
EOF
        "$PA/pulseaudio" --daemonize=yes --exit-idle-time=-1 --use-pid-file=no \
            --disallow-exit --disable-shm=yes -n -F "$CONF" --log-target=file:"$LOG" \
            || { echo "audio: PulseAudio did not start (log $LOG)" >&2; return 1; }
        for _ in $(seq 1 50); do running && break; sleep 0.1; done
        running || { echo "audio: no socket at $SOCK (log $LOG)" >&2; return 1; }
    fi
    follow_default
    [ -n "${1:-}" ] && volume "$1"
    return 0
}

# The sink of the Mac's current default output (CoreAudio names it; the
# PulseAudio sinks carry that name as device.description).
follow_default() {
    local want sink
    want="$(/usr/bin/python3 "$(dirname "$0")/default-output.py" 2>/dev/null)"
    [ -n "$want" ] || return 0
    sink="$(pactl_ list sinks 2>/dev/null | awk -v w="$want" '
        /^[^ \t]/ { name = "" }
        /^[ \t]+(Nombre|Name):/ { name = $2 }
        index($0, "device.description = \"" w "\"") { print name; exit }')"
    [ -n "$sink" ] && pactl_ set-default-sink "$sink"
}

volume() {
    local v="${1:-100}"
    case "$v" in ''|*[!0-9]*) v=100 ;; esac
    [ "$v" -gt 100 ] && v=100
    pactl_ set-sink-volume @DEFAULT_SINK@ "${v}%"
}

case "${1:-status}" in
    start)  start "${2:-}" ;;
    volume) running || start; volume "${2:-100}" ;;
    stop)   running && pactl_ exit 2>/dev/null; pkill -f "pulseaudio.*$CONF" 2>/dev/null; rm -f "$SOCK"; true ;;
    follow) running && follow_default ;;
    status) if running; then echo "audio: running ($SOCK), default sink $(pactl_ get-default-sink)"; else echo "audio: stopped"; fi ;;
    *) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
