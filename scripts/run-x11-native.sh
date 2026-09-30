#!/bin/bash
# Start/stop the native (XQuartz, rootless) X server built by build-xquartz.sh.
# Every X11 toplevel becomes a native macOS window owned by SteamARM-X11.app.
#
#   run-x11-native.sh [start|stop|restart|status] [:N]      (default: start :2)
#
# Idempotent: "start" on a running display is a no-op, "stop" on a stopped one too.
# Display :1 is refused (Xvnc serves it for the running Steam).
# Server log: ~/SteamARM-roots/logs/x11-native.log (previous run: .old)
#
# Env overrides: XQ_ROOT, X11_NATIVE_LOG, X11_NATIVE_ARGS (extra server args),
# STEAMARM_X11_XTEST (1, the default: the XTEST extension; 0: none).
set -euo pipefail

XQ_ROOT="${XQ_ROOT:-${STEAMARM_BUILD:-$HOME/SteamARM-build}/xquartz}"
BUNDLE="$XQ_ROOT/SteamARM-X11.app"
BIN="$BUNDLE/Contents/MacOS/X11.bin"
BUNDLE_ID=org.steamarm.X11
LOGDIR="${STEAMARM_STATE:-$HOME/SteamARM-roots}/logs"
LOG="${X11_NATIVE_LOG:-$LOGDIR/x11-native.log}"
STDIO_LOG="${LOG%.log}.stdio.log"
XDPYINFO="$(command -v xdpyinfo || echo /opt/homebrew/bin/xdpyinfo)"

cmd=start
disp=":2"
for a in "$@"; do
    case "$a" in
        start|stop|restart|status) cmd=$a ;;
        :[0-9]*) disp=$a ;;
        [0-9]*) disp=":$a" ;;
        -h|--help) sed -n '2,12p' "$0"; exit 0 ;;
        *) echo "unknown argument: $a" >&2; exit 2 ;;
    esac
done
num=${disp#:}
if [ "$num" = 1 ]; then
    echo "refusing display :1 (reserved for Xvnc / running Steam)" >&2
    exit 2
fi
LOCK="/tmp/.X$num-lock"
SOCK="/tmp/.X11-unix/X$num"

msg() { printf '[x11-native %s] %s\n' "$disp" "$*"; }

server_pid() { # prints pid of our server on $disp, if running
    local pid
    [ -f "$LOCK" ] || return 1
    pid=$(tr -dc '0-9' < "$LOCK")
    [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null || return 1
    ps -o command= -p "$pid" | grep -q "SteamARM-X11.app/Contents/MacOS/X11.bin" || return 1
    echo "$pid"
}

responsive() { DISPLAY="$disp" "$XDPYINFO" >/dev/null 2>&1; }

do_status() {
    local pid
    if pid=$(server_pid); then
        if responsive; then msg "running (pid $pid, socket $SOCK)"; else msg "pid $pid alive but not answering"; fi
        return 0
    fi
    msg "not running"; return 3
}

# quartz-wm (build-xquartz.sh step 5): native title bars, close/minimise/zoom
# buttons and resizing for every X toplevel. Without it windows are bare
# rectangles that cannot be moved.
WM="$XQ_ROOT/quartz-wm/src/quartz-wm"
start_wm() {
    [ -x "$WM" ] || { msg "no window manager ($WM); windows get no title bars"; return 0; }
    pgrep -f "$WM" >/dev/null && return 0
    ( cd / && DISPLAY="$disp" exec nohup "$WM" >>"$LOGDIR/quartz-wm.log" 2>&1 </dev/null ) &
    disown || true
    msg "window manager started (quartz-wm)"
}

do_start() {
    local pid other i
    if pid=$(server_pid) && responsive; then
        msg "already running (pid $pid)"; start_wm; return 0
    fi
    [ -x "$BIN" ] || { msg "server not built: $BIN (run scripts/build-xquartz.sh)"; exit 1; }
    # The Mach bootstrap name ($BUNDLE_ID) is per-bundle, so only one instance
    # of this bundle can run at a time.
    other=$(pgrep -f "SteamARM-X11.app/Contents/MacOS/X11.bin" || true)
    if [ -n "$other" ]; then
        msg "another SteamARM-X11 server is running (pids: $(echo $other)); stop it first"; exit 1
    fi
    if [ -e "$LOCK" ] && ! server_pid >/dev/null; then
        msg "note: stale $LOCK (server will clean it up)"
    fi

    # Per-app preferences (our own defaults domain only; nothing system-wide):
    #  rootless, no TCP, no modal "enter RandR mode?" / quit dialogs, and fix the
    #  localized "window item modifiers" string that some locales mistranslate.
    defaults write "$BUNDLE_ID" rootless -bool true
    defaults write "$BUNDLE_ID" nolisten_tcp -bool true
    defaults write "$BUNDLE_ID" no_randr_alert -bool true
    defaults write "$BUNDLE_ID" no_quit_alert -bool true
    defaults write "$BUNDLE_ID" window_item_modifiers -string command
    #  XTEST, which XQuartz leaves off unless asked (enable_test_extensions):
    #  Steam Input's desktop configuration (a controller as mouse and keys)
    #  types and clicks through it, and tests/android/xtest_input.py sends
    #  real clicks and keys with it. It acts inside this X server only, whose
    #  clients are SteamARM's own guests (no TCP); STEAMARM_X11_XTEST=0 leaves
    #  it off. Read when the server starts.
    if [ "${STEAMARM_X11_XTEST:-1}" = 0 ]; then
        defaults write "$BUNDLE_ID" enable_test_extensions -bool false
    else
        defaults write "$BUNDLE_ID" enable_test_extensions -bool true
    fi

    mkdir -p "$LOGDIR"
    # xtrans refuses to create the socket dir when euid != 0; make sure it exists.
    [ -d /tmp/.X11-unix ] || { mkdir -p /tmp/.X11-unix && chmod 1777 /tmp/.X11-unix; }
    msg "starting $BIN $disp (log $LOG)"
    # X11.bin with ":N" as argv[1] runs as a plain DDX: it registers the
    # $BUNDLE_ID Mach service, forks a helper that sends the argv over Mach IPC
    # and then runs the server + NSApplication itself (no launchd needed).
    # shellcheck disable=SC2086
    ( cd / && exec env -u DISPLAY XQUARTZ_LOG_FILE="$LOG" \
        nohup "$BIN" "$disp" -nolisten tcp +iglx ${X11_NATIVE_ARGS:-} \
        >>"$STDIO_LOG" 2>&1 </dev/null ) &
    disown || true
    for i in $(seq 1 60); do
        if responsive; then
            msg "ready after ~$((i / 4))s (pid $(server_pid || echo '?'), DISPLAY=$disp)"
            start_wm
            return 0
        fi
        sleep 0.25
    done
    msg "server did not answer within 15s; see $LOG"; exit 1
}

do_stop() {
    local pid i
    if pid=$(server_pid); then
        msg "stopping pid $pid"
        kill -TERM "$pid" 2>/dev/null || true
        # XQuartz takes several seconds to tear down AppKit after SIGTERM.
        for i in $(seq 1 100); do
            kill -0 "$pid" 2>/dev/null || break
            sleep 0.2
        done
        if kill -0 "$pid" 2>/dev/null; then
            msg "pid $pid still alive after 20s, sending SIGKILL"
            kill -KILL "$pid" 2>/dev/null || true
            sleep 0.5
            rm -f "$LOCK" "$SOCK"
        fi
    else
        msg "not running"
    fi
    # The startup helper child blocks in a Mach RPC that never gets a reply;
    # make sure no process of ours for this display is left behind.
    pkill -f "SteamARM-X11.app/Contents/MacOS/X11.bin $disp( |\$)" 2>/dev/null || true
    pkill -f "$WM" 2>/dev/null || true
    return 0
}

case "$cmd" in
    start) do_start ;;
    stop) do_stop ;;
    restart) do_stop; do_start ;;
    status) do_status ;;
esac
