#!/bin/bash
# A Wayland compositor for Linux and Android guests, with no VM: Weston
# (Fedora aarch64, scripts/mkwestonroot.sh) under lxrun, with its X11 backend
# on SteamARM's native X server (scripts/run-x11-native.sh, :2), so its output
# is a macOS window; or headless. docs/ANDROID_RUNTIME_ARCHITECTURE.md,
# "Display"; benchmarks/stage27-android-display.txt.
#
#   scripts/run-weston.sh start [--headless] [--kiosk] [--size WxH]
#   scripts/run-weston.sh stop | status
#   scripts/run-weston.sh client <guest program> [args]   a Wayland client of
#                              the Weston root, pointed at this compositor
#
# The socket: WAYLAND_DISPLAY=$WESTON_SOCKET in XDG_RUNTIME_DIR=$WESTON_XDG, a
# guest path under /dev/shm. The runtime maps /dev/shm to one host directory
# per user (/tmp/lxrt-shm-<uid>) whatever the guest's root, so a client in any
# root -- the Weston root, an x86-64 root under FEX, an Android root --
# reaches the same socket with the same two variables (Android's
# hwcomposer.waydroid takes them from waydroid.xdg_runtime_dir and
# waydroid.wayland_display).
#
# Environment:
#   WESTON_ROOT    the root             ($STEAMARM_STATE/westonroot)
#   WESTON_XDG     XDG_RUNTIME_DIR      (/dev/shm/steamarm-wayland)
#   WESTON_SOCKET  WAYLAND_DISPLAY      (wayland-0)
#   DISPLAY        the X server         (:2)
#   LXRUN          the runtime          (build/lxrun)
# Stops only the Weston it started (the PID file in the socket directory,
# checked to be lxrun running /usr/bin/weston on this socket).
set -u
cd "$(dirname "$0")/.." || exit 1
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
ROOT="${WESTON_ROOT:-$STATE/westonroot}"
XDG="${WESTON_XDG:-/dev/shm/steamarm-wayland}"
SOCKET="${WESTON_SOCKET:-wayland-0}"
LXRUN="${LXRUN:-build/lxrun}"
DISP="${DISPLAY:-:2}"
case "$XDG" in /dev/shm/*) ;; *) echo "run-weston: WESTON_XDG must be under /dev/shm" >&2; exit 2 ;; esac
HOSTXDG="/tmp/lxrt-shm-$(id -u)${XDG#/dev/shm}"
PIDFILE="$HOSTXDG/$SOCKET.weston-pid"
LOG="$HOSTXDG/$SOCKET.weston.log"

guest_env() {   # the environment every guest of the Weston root gets
    # LXRT_SESSION (scripts/android-session.py sets it): an Android session's
    # Weston is the session's, and scripts/run-app.sh leaves it alone.
    echo LXRT_ROOT="$ROOT" TMPDIR=/tmp HOME=/tmp OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES \
        PATH=/usr/bin:/bin LANG=C.UTF-8 DISPLAY="$DISP" XDG_RUNTIME_DIR="$XDG" WAYLAND_DISPLAY="$SOCKET" \
        ${LXRT_SESSION:+LXRT_SESSION=$LXRT_SESSION}
}
weston_pid() {
    local p
    p=$(cat "$PIDFILE" 2>/dev/null) || return 1
    case "$p" in ''|*[!0-9]*) return 1 ;; esac
    ps -p "$p" -o command= 2>/dev/null | grep -q "lxrun /usr/bin/weston .*--socket=$SOCKET" || return 1
    echo "$p"
}

cmd="${1:-status}"; shift || true
case "$cmd" in
start)
    headless=0 shell=desktop size=800x600
    while [ $# -gt 0 ]; do
        case "$1" in
            --headless) headless=1 ;;
            --kiosk) shell=kiosk ;;
            --size) size="$2"; shift ;;
            *) echo "run-weston: unknown option $1" >&2; exit 2 ;;
        esac
        shift
    done
    [ -x "$ROOT/usr/bin/weston" ] || { echo "run-weston: no Weston root at $ROOT (scripts/mkwestonroot.sh)" >&2; exit 1; }
    [ -x "$LXRUN" ] || { echo "run-weston: no $LXRUN (make lxrt)" >&2; exit 1; }
    if p=$(weston_pid); then echo "run-weston: already running (pid $p, $XDG/$SOCKET)"; exit 0; fi
    [ -n "${STEAMARM_NO_SAFEGUARD:-}" ] || scripts/safeguard.sh start >/dev/null
    # Weston wants its runtime directory private (0700), as logind makes it.
    mkdir -p "$HOSTXDG" && chmod 700 "$HOSTXDG"
    backend=(--backend=x11 --width="${size%x*}" --height="${size#*x}")
    if [ "$headless" = 1 ]; then
        backend=(--backend=headless --width="${size%x*}" --height="${size#*x}")
    elif ! DISPLAY="$DISP" "$(command -v xdpyinfo || echo /opt/homebrew/bin/xdpyinfo)" >/dev/null 2>&1; then
        echo "run-weston: no X server on $DISP (scripts/run-x11-native.sh start)" >&2; exit 1
    fi
    # shellcheck disable=SC2046
    ( env -i $(guest_env) "$LXRUN" /usr/bin/weston "${backend[@]}" --renderer=pixman \
          --socket="$SOCKET" --idle-time=0 --shell="$shell" >"$LOG" 2>&1 </dev/null &
      echo $! > "$PIDFILE" )
    for ((i = 0; i < 100; i++)); do
        [ -S "$HOSTXDG/$SOCKET" ] && p=$(weston_pid) && { echo "run-weston: pid $p, WAYLAND_DISPLAY=$SOCKET XDG_RUNTIME_DIR=$XDG (log $LOG)"; exit 0; }
        weston_pid >/dev/null || break
        sleep 0.1
    done
    echo "run-weston: Weston did not come up; $LOG:" >&2
    grep -v '^\[lxrt\]' "$LOG" | tail -5 >&2
    exit 1 ;;
stop)
    if p=$(weston_pid); then
        kill "$p"
        for ((i = 0; i < 50; i++)); do kill -0 "$p" 2>/dev/null || break; sleep 0.1; done
        echo "run-weston: stopped pid $p"
    else
        echo "run-weston: not running"
    fi
    rm -f "$PIDFILE" ;;
status)
    if p=$(weston_pid); then echo "run-weston: running (pid $p, $XDG/$SOCKET)"; else echo "run-weston: not running"; exit 3; fi ;;
client)
    [ $# -ge 1 ] || { echo "usage: $0 client <guest program> [args]" >&2; exit 2; }
    # shellcheck disable=SC2046
    exec env -i $(guest_env) ${WESTON_CLIENT_ENV:-} "$LXRUN" "$@" ;;
*)
    sed -n '2,30p' "$0"; exit 2 ;;
esac
