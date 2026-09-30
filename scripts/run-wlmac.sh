#!/bin/bash
# Native Wayland compositor. Guests see /dev/shm; lxrun maps it to /tmp/lxrt-shm-UID.
set -euo pipefail
cd "$(dirname "$0")/.."
XDG="${WLMAC_XDG:-/dev/shm/steamarm-wlmac}"
SOCKET="${WLMAC_SOCKET:-wayland-0}"
case "$XDG" in /dev/shm/*) ;; *) echo 'run-wlmac: WLMAC_XDG must be under /dev/shm' >&2; exit 2;; esac
case "$SOCKET" in */*|'') echo 'run-wlmac: WLMAC_SOCKET must be a filename' >&2; exit 2;; esac
HOSTXDG="/tmp/lxrt-shm-$(id -u)${XDG#/dev/shm}"
PIDFILE="$HOSTXDG/$SOCKET.wlmac-pid"
LOG="$HOSTXDG/$SOCKET.wlmac.log"
pid() {
    local p
    p=$(cat "$PIDFILE" 2>/dev/null) || return 1
    case "$p" in ''|*[!0-9]*) return 1;; esac
    ps -p "$p" -o command= 2>/dev/null | grep -q 'steamarm-wlmac --socket ' || return 1
    echo "$p"
}
case "${1:-status}" in
start)
    if p=$(pid); then echo "run-wlmac: already running pid $p"; exit 0; fi
    tools/wlmac/build.sh >/dev/null
    mkdir -p "$HOSTXDG"; chmod 700 "$HOSTXDG"
    build/steamarm-wlmac --socket "$HOSTXDG/$SOCKET" "${@:2}" >"$LOG" 2>&1 </dev/null &
    echo $! >"$PIDFILE"
    for ((i=0;i<100;i++)); do
        if [ -S "$HOSTXDG/$SOCKET" ] && pid >/dev/null; then
            echo "run-wlmac: pid $(pid), XDG_RUNTIME_DIR=$XDG WAYLAND_DISPLAY=$SOCKET (log $LOG)"; exit 0
        fi
        sleep 0.1
    done
    echo "run-wlmac: failed; log $LOG" >&2; tail -10 "$LOG" >&2; exit 1;;
stop)
    if p=$(pid); then
        kill "$p"
        for ((i=0;i<50;i++)); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
        echo "run-wlmac: stopped $p"
    else echo 'run-wlmac: not running'; fi
    rm -f "$PIDFILE";;
status)
    if p=$(pid); then echo "run-wlmac: running pid $p, $XDG/$SOCKET"; else echo 'run-wlmac: not running'; exit 3; fi;;
*) echo 'usage: scripts/run-wlmac.sh start [--dump-dir DIR] [--verbose] | stop | status' >&2; exit 2;;
esac
