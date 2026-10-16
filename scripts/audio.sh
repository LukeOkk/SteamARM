#!/bin/bash
# Sound for guest programs, no VM: one PulseAudio server on the Mac (Homebrew's,
# playing through CoreAudio). Each guest root gets a socket on it at
# <root>/tmp/pulse/native, where guests reach it as unix:/tmp/pulse/native
# (scripts/settings-env.py exports PULSE_SERVER). The guests' own libpulse
# speaks to it; pressure-vessel binds the socket into its container like on
# Linux. LXRT_ROOT names the root (default: the Steam root).
#
#   scripts/audio.sh start [volume%]   start (idempotent), socket in LXRT_ROOT, set the volume
#   scripts/audio.sh volume N          set the output volume (0-100)
#   scripts/audio.sh stop | status
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${LXRT_ROOT:-/tmp/lxrt-steamroot}"
STATE="${STEAMARM_STATE:-$(d="$HOME/Developer/SteamARM-roots"; [ -d "$d" ] || [ ! -d "$HOME/SteamARM-roots" ] || d="$HOME/SteamARM-roots"; echo "$d")}"
# SteamARM's own build when it is there (scripts/build-pulseaudio.sh: the
# Mac's devices with their real latency, not PulseAudio's default 250 ms, with
# which every guest's sound was half a second late), Homebrew's otherwise.
# STEAMARM_BREW_PULSE=1: Homebrew's always.
OWN_PA="${STEAMARM_BUILD:-$(d="$HOME/Developer/SteamARM-build"; [ -d "$d" ] || [ ! -d "$HOME/SteamARM-build" ] || d="$HOME/SteamARM-build"; echo "$d")}/pulseaudio/out/bin"
if [ -x "$OWN_PA/pulseaudio" ] && [ "${STEAMARM_BREW_PULSE:-0}" != 1 ]; then
    PA="$OWN_PA"
else
    PA="${BREW:-/opt/homebrew}/opt/pulseaudio/bin"
fi
DIR="$ROOT/tmp/pulse"
SOCK="$DIR/native"
LOG="$STATE/logs/pulseaudio.log"
CONF="$STATE/launcher/pulse.pa"
DAEMONCONF="$STATE/launcher/pulse-daemon.conf"
SRVFILE="$STATE/launcher/pulse.socket"     # the socket the server was started with
BINFILE="$STATE/launcher/pulse.bin"        # the server binary it was started from

live() { [ -S "$1" ] && "$PA/pactl" -s "unix:$1" info >/dev/null 2>&1; }
# A socket of the running server: its own, else this root's.
server() {
    local s
    s="$(cat "$SRVFILE" 2>/dev/null)"
    if [ -n "$s" ] && live "$s"; then echo "$s"; elif live "$SOCK"; then echo "$SOCK"; fi
}
pactl_() { "$PA/pactl" -s "unix:$(server)" "$@"; }
running() { [ -n "$(server)" ]; }
# The server process this script started (its command line names $CONF).
server_pid() { pgrep -f "pulseaudio.*-F $CONF" 2>/dev/null | head -1; }

start() {
    [ -x "$PA/pulseaudio" ] || { echo "audio: PulseAudio not installed (brew install pulseaudio)" >&2; return 1; }
    # A server of the other build (Homebrew's, started before SteamARM had
    # its own) outlives Steam's restarts: replaced while nothing is connected,
    # with the sockets it served for other roots made again on the new one.
    # Which binary it runs is what start wrote down (pulse.bin); a server
    # older than that file is known by its command line. (It runs with
    # --disallow-exit: stopped by its pid, not "pactl exit".)
    local restore=""
    if running; then
        local pid bin
        read -r pid bin <<<"$(ps -axo pid=,command= | awk -v c="$CONF" \
            'index($0, "pulseaudio") && index($0, "-F " c) {print $1, $2; exit}')"
        [ -f "$BINFILE" ] && bin="$(cat "$BINFILE")"
        if [ -n "$pid" ] && [ -n "$bin" ] && [ "$bin" != "$PA/pulseaudio" ] &&
           [ "$(pactl_ list clients short 2>/dev/null | grep -vc pactl)" -eq 0 ]; then
            restore="$(pactl_ list modules short 2>/dev/null |
                sed -n 's/.*module-native-protocol-unix.*socket=\([^ ]*\).*/\1/p' | grep -vxF "$SOCK")"
            kill "$pid" 2>/dev/null
            for _ in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
            rm -f "$SOCK"
        fi
    fi
    if ! live "$SOCK"; then
        mkdir -p "$DIR" "$STATE/logs" "$(dirname "$CONF")"
        rm -f "$SOCK"
        if running; then
            # Already serving another root (the Steam root and the ARM64 root
            # are separate trees): one server, one more socket on it.
            pactl_ load-module module-native-protocol-unix socket="$SOCK" auth-anonymous=1 >/dev/null \
                || { echo "audio: cannot add a socket at $SOCK (log $LOG)" >&2; return 1; }
        else
            # Only what is needed: the Mac's outputs and the guests' socket
            # (anonymous: the socket is only reachable from this user's roots).
            cat > "$CONF" <<EOF
load-module module-coreaudio-detect
load-module module-native-protocol-unix socket=$SOCK auth-anonymous=1
load-module module-always-sink
load-module module-suspend-on-idle timeout=3
EOF
            # The daemon's own settings (PULSE_CONFIG names the file). Games
            # and the Mac's devices run at 48 kHz: the server's default
            # format is that, so that nothing is resampled on the way when
            # it need not be (avoid-resampling lets a device follow a lone
            # stream's rate), and when it is, soxr-hq instead of the default
            # speex-float-1, whose aliasing is audible on voice. Buffers of
            # the device side: 4 x 20 ms.
            cat > "$DAEMONCONF" <<EOF
default-sample-format = float32le
default-sample-rate = 48000
alternate-sample-rate = 44100
resample-method = soxr-hq
avoid-resampling = yes
flat-volumes = no
default-fragments = 4
default-fragment-size-msec = 20
EOF
            # Not --daemonize: PulseAudio daemonizes with fork() and no exec,
            # and module-coreaudio-detect then loads CoreAudio in the fork
            # child, which macOS does not support. It aborted there ("crashed
            # on child side of fork pre-exec") whenever the parent had threads
            # at the fork -- and it had them whenever LANG was unset, as for
            # everything the SteamARM app starts: libintl's setlocale() asks
            # CoreFoundation for the locale, which starts two threads. So
            # after every restart of the Mac the app's first start had no
            # sound server, and without one Steam's client never opened its
            # window; a server started from a terminal (LANG set) hid it
            # until the next restart (MEASURED 2026-10-15,
            # benchmarks/stage63-pulseaudio-fork.txt). The server runs in the
            # foreground instead, exec'd in a session of its own
            # (session.py detach): no fork child, and no stop of an app's
            # process group takes it down.
            rm -f "$LOG"
            ( PULSE_CONFIG="$DAEMONCONF" nohup env PYTHONCOERCECLOCALE=0 STEAMARM_SESSION_PY=1 \
                /usr/bin/python3 "$HERE/session.py" detach \
                "$PA/pulseaudio" --daemonize=no --exit-idle-time=-1 --use-pid-file=no \
                --disallow-exit --disable-shm=yes -n -F "$CONF" --log-target=file:"$LOG" \
                </dev/null >/dev/null 2>&1 & )
            echo "$SOCK" > "$SRVFILE"
            echo "$PA/pulseaudio" > "$BINFILE"
        fi
        # CoreAudio lists every device of the Mac first: 3 s with a monitor,
        # a USB interface and Steam's virtual devices (MEASURED). Up to 20 s;
        # a server that is already gone is not waited for.
        local i
        for i in $(seq 1 200); do
            live "$SOCK" && break
            [ "$i" -lt 20 ] || [ -n "$(server_pid)" ] || break
            sleep 0.1
        done
        live "$SOCK" || {
            echo "audio: PulseAudio did not start: no socket at $SOCK (log $LOG)" >&2
            tail -3 "$LOG" 2>/dev/null >&2
            return 1
        }
    fi
    local s
    while IFS= read -r s; do
        [ -n "$s" ] || continue
        rm -f "$s"
        pactl_ load-module module-native-protocol-unix socket="$s" auth-anonymous=1 >/dev/null 2>&1 || true
    done <<<"$restore"
    # Devices nobody plays to or records from are closed after 3 s. Without
    # it every input of the Mac stayed open (IDLE) for the life of the
    # server: the microphone in use -- macOS's orange dot -- although no
    # guest was recording. Also loaded into a server started before this.
    pactl_ list modules short 2>/dev/null | grep -q module-suspend-on-idle ||
        pactl_ load-module module-suspend-on-idle timeout=3 >/dev/null 2>&1 || true
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
    stop)   running && pactl_ exit 2>/dev/null; pkill -f "pulseaudio.*$CONF" 2>/dev/null
            rm -f "$SOCK" "$SRVFILE" "$BINFILE"; true ;;
    follow) running && follow_default ;;
    status) if running; then echo "audio: running ($(server)), default sink $(pactl_ get-default-sink)"; else echo "audio: stopped"; fi ;;
    *) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
