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
ROOT="${LXRT_ROOT:-/tmp/lxrt-steamroot}"
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
PA="${BREW:-/opt/homebrew}/opt/pulseaudio/bin"
DIR="$ROOT/tmp/pulse"
SOCK="$DIR/native"
LOG="$STATE/logs/pulseaudio.log"
CONF="$STATE/launcher/pulse.pa"
DAEMONCONF="$STATE/launcher/pulse-daemon.conf"
SRVFILE="$STATE/launcher/pulse.socket"     # the socket the server was started with

live() { [ -S "$1" ] && "$PA/pactl" -s "unix:$1" info >/dev/null 2>&1; }
# A socket of the running server: its own, else this root's.
server() {
    local s
    s="$(cat "$SRVFILE" 2>/dev/null)"
    if [ -n "$s" ] && live "$s"; then echo "$s"; elif live "$SOCK"; then echo "$SOCK"; fi
}
pactl_() { "$PA/pactl" -s "unix:$(server)" "$@"; }
running() { [ -n "$(server)" ]; }

start() {
    [ -x "$PA/pulseaudio" ] || { echo "audio: PulseAudio not installed (brew install pulseaudio)" >&2; return 1; }
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
            PULSE_CONFIG="$DAEMONCONF" "$PA/pulseaudio" --daemonize=yes --exit-idle-time=-1 --use-pid-file=no \
                --disallow-exit --disable-shm=yes -n -F "$CONF" --log-target=file:"$LOG" \
                || { echo "audio: PulseAudio did not start (log $LOG)" >&2; return 1; }
            echo "$SOCK" > "$SRVFILE"
        fi
        for _ in $(seq 1 50); do live "$SOCK" && break; sleep 0.1; done
        live "$SOCK" || { echo "audio: no socket at $SOCK (log $LOG)" >&2; return 1; }
    fi
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
            rm -f "$SOCK" "$SRVFILE"; true ;;
    follow) running && follow_default ;;
    status) if running; then echo "audio: running ($(server)), default sink $(pactl_ get-default-sink)"; else echo "audio: stopped"; fi ;;
    *) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 2 ;;
esac
