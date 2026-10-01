#!/bin/bash
# Start Steam, no VM: the X server (Xvnc, display :1) if it is not running,
# then the Linux Steam client under FEX and the runtime, and macOS's own
# Screen Sharing on the display.
#
#   scripts/run-steam.sh            start (or re-show) Steam in native windows
#   scripts/run-steam.sh --stop     stop the Steam processes (X keeps running)
#
# Display: by default the native rootless X server (scripts/run-x11-native.sh,
# display :2): every X window is a real macOS window. STEAMARM_DISPLAY=vnc
# uses the old path instead: Xvnc on :1 shown through Screen Sharing.
#
# The display is only reachable from this Mac (Xvnc -localhost) and asks for
# a VNC password, because Screen Sharing will not connect to a server without
# one. The password is in $STATE/vncpasswd.txt (default "steamarm"); change it
# with STEAMARM_VNC_PASSWORD=... before the first start.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=/tmp/lxrt-steamroot
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
LOGS="$STATE/logs"
mkdir -p "$LOGS"

# A runtime process of Steam's root: its environment names LXRT_ROOT=$ROOT
# (the runtime carries LXRT_* variables through every guest exec). Other
# roots' programs -- the Android session, the arm64 client, other apps --
# are not Steam's to stop; this used to kill every lxrun process.
in_steam_root() {
    ps -E -o command= -p "$1" 2>/dev/null | tr ' ' '\n' | grep -qx "LXRT_ROOT=$ROOT"
}

stop_steam() {
    for p in $(pgrep -f "build/lxrun"); do
        case "$(ps -o command= -p "$p")" in
            *Xvnc*|*FEXServer*) ;;
            *) in_steam_root "$p" && kill -9 "$p" 2>/dev/null ;;
        esac
    done
    rm -f "$ROOT/tmp/fexhome/.steam/steam.pid"
}

if [ "${1:-}" = "--stop" ]; then
    stop_steam
    exit 0
fi

scripts/env-links.sh "$STATE" >/dev/null || exit 1

# VNC password file: the classic VNC obfuscation (DES, fixed key) of at most
# eight characters, which is what Xvnc's -rfbauth reads.
PWTXT="$STATE/vncpasswd.txt"
PWFILE="$STATE/vncpasswd"
if [ ! -s "$PWFILE" ]; then
    pw="${STEAMARM_VNC_PASSWORD:-steamarm}"
    printf '%s\n' "$pw" > "$PWTXT"
    chmod 600 "$PWTXT"
    printf '%-8.8s' "$pw" | tr ' ' '\0' |
        openssl enc -des-ecb -K e84ad660c4721ae0 -nopad -nosalt > "$PWFILE"
    chmod 600 "$PWFILE"
fi

MODE="${STEAMARM_DISPLAY:-native}"
if [ "$MODE" = native ]; then
    DISP=:2
    scripts/run-x11-native.sh start :2 >/dev/null || { echo "native X server failed to start" >&2; exit 1; }
else
    DISP=:1
fi

if [ "$MODE" != native ] && ! pgrep -f 'lxrun /usr/bin/Xvnc' >/dev/null; then
    cp "$PWFILE" "$ROOT/tmp/.vncpasswd"
    # Xvnc compiles its keymap with popen("xkbcomp ...") through /bin/sh, an
    # x86 bash here: it needs FEX's environment and a FEXServer, or the
    # keyboard fails to initialise and the server exits.
    pgrep -f 'lxrun /tmp/lxrt-root/usr/bin/FEXServer' >/dev/null || scripts/run-fex.sh /bin/true >/dev/null 2>&1
    TMPDIR=/tmp HOME=/tmp/fexhome FEX_ROOTFS=/ FEX_GUESTBASE=1 \
    LXRT_ROOT=$ROOT nohup ./build/lxrun /usr/bin/Xvnc :1 -geometry 1600x900 -depth 24 \
        -SecurityTypes VncAuth -rfbauth /tmp/.vncpasswd -localhost \
        -rfbport 5901 +extension GLX -fp built-ins -nolisten tcp \
        > "$LOGS/xvnc.log" 2>&1 &
    sleep 3
fi

if ! pgrep -f 'build/lxrun .*ubuntu12_32/steam ' >/dev/null; then
    stop_steam
    L="$LOGS/steam-$(date +%H%M%S).log"
    echo "$L" > "$LOGS/current"
    # -noverifyfiles skips a slow check of an installed client. On a fresh
    # install it also skips the download of the client itself: the bootstrap
    # found no steamui.so and exited (MEASURED, clean install).
    VERIFY=()
    [ -f "$ROOT/tmp/fexhome/.local/share/Steam/ubuntu12_32/steamui.so" ] && VERIFY=(-noverifyfiles)
    # The launcher's settings (scripts/settings-env.py): games inherit them.
    eval "$(/usr/bin/python3 scripts/settings-env.py --shell)"
    VOL="$(/usr/bin/python3 scripts/settings-env.py --volume)"
    [ -n "$VOL" ] && { scripts/audio.sh start "$VOL" >/dev/null || echo "run-steam: no sound (scripts/audio.sh)" >&2; }
    scripts/input.sh start >/dev/null || echo "run-steam: no controllers for games (scripts/input.sh)" >&2
    # Valve's Linux shader replay stalls here while processing Schedule I.
    # This disables Steam's pre-caching, not DXVK/VKD3D or Metal's own caches.
    DISPLAY=$DISP LXRT_ROOT=$ROOT FEX_ROOTFS=/ \
        STEAM_ENABLE_SHADER_CACHE_MANAGEMENT="${STEAM_ENABLE_SHADER_CACHE_MANAGEMENT:-0}" \
        nohup scripts/run-fex.sh /bin/bash /tmp/fexhome/.local/share/Steam/steam.sh ${VERIFY[@]+"${VERIFY[@]}"} \
        > "$L" 2>&1 &
    echo "Steam starting (log $L); the window appears in about a minute."
fi

if [ "$MODE" != native ]; then
    open "vnc://127.0.0.1:5901"
    echo "Screen Sharing: password in $PWTXT"
fi
