#!/bin/bash
# PipeWire for the ARM64 guests (resources/pipewire): the daemon, its tunnels
# to the Mac's PulseAudio server (scripts/audio.sh) and WirePlumber, run as
# guest processes of the ARM64 root. The Steam client uses PipeWire for its
# own audio settings and voice (wpctl); games keep PulseAudio
# (scripts/settings-env.py). scripts/run-app.sh starts this for an ARM64
# Steam client; run-app.sh --stop takes it down with the other guests.
#
#   scripts/pipewire.sh start [ROOT]    ROOT: the ARM64 root (default
#                                       $LXRT_ROOT or /tmp/lxrt-arm64root)
#   scripts/pipewire.sh stop | status
#
# The guests find it through PIPEWIRE_RUNTIME_DIR=/tmp/steamarm-pipewire
# (a guest path: the root's tmp), where its sockets and configs live.
set -u
cd "$(dirname "$0")/.."
CMD="${1:-status}"
ROOT="${2:-${LXRT_ROOT:-/tmp/lxrt-arm64root}}"
GDIR=/tmp/steamarm-pipewire
HDIR="$ROOT$GDIR"
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
LOG="$STATE/logs/pipewire.log"

# Ours only: the daemon and tunnels by their config path, WirePlumber by its
# binary (nothing else in the guests runs it).
pids() {
    ps -axo pid=,command= | awk -v d="$GDIR" '$2 ~ /lxrun$/ &&
        (index($0, d "/pipewire.conf") || index($0, d "/tunnels.conf") || $3 == "/usr/bin/wireplumber") {print $1}'
}
running() { [ -S "$HDIR/pipewire-0" ] && [ "$(pids | wc -l | tr -d ' ')" -ge 3 ]; }

guest() {   # log name, guest command...
    local name=$1; shift
    env PIPEWIRE_RUNTIME_DIR=$GDIR PULSE_SERVER=unix:/tmp/pulse/native HOME_IN_GUEST=/tmp/armhome \
        LXRT_ROOT="$ROOT" nohup scripts/run-native.sh "$@" >> "$LOG" 2>&1 < /dev/null &
}

case "$CMD" in
start)
    running && { echo "pipewire: running"; exit 0; }
    for p in $(pids); do kill "$p" 2>/dev/null; done
    [ -d "$ROOT/usr" ] || { echo "pipewire: no ARM64 root at $ROOT" >&2; exit 1; }
    [ -x "$ROOT/usr/bin/pipewire" ] || { echo "pipewire: not in this root" >&2; exit 1; }
    mkdir -p "$HDIR" "$(dirname "$LOG")" || exit 1
    install -m 0644 resources/pipewire/pipewire.conf resources/pipewire/tunnels.conf "$HDIR/" || exit 1
    rm -f "$HDIR"/pipewire-0 "$HDIR"/pipewire-0.lock "$HDIR"/pipewire-0-manager "$HDIR"/pipewire-0-manager.lock
    : > "$LOG"
    guest daemon /usr/bin/pipewire -c "$GDIR/pipewire.conf"
    for _ in $(seq 1 50); do [ -S "$HDIR/pipewire-0" ] && break; sleep 0.1; done
    [ -S "$HDIR/pipewire-0" ] || { echo "pipewire: the daemon did not start (log $LOG)" >&2; exit 1; }
    guest tunnels /usr/bin/pipewire -c "$GDIR/tunnels.conf"
    guest wireplumber /usr/bin/wireplumber
    echo "pipewire: started ($GDIR in $ROOT)"
    ;;
stop)
    for p in $(pids); do kill "$p" 2>/dev/null; done
    echo "pipewire: stopped"
    ;;
status)
    if running; then echo "pipewire: running ($(pids | wc -l | tr -d ' ') processes)"; else echo "pipewire: stopped"; fi
    ;;
*)
    echo "usage: $0 start [ROOT] | stop | status" >&2; exit 2 ;;
esac
