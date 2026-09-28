#!/bin/bash
# Start one app of the SteamARM launcher, no VM: the X server if it is not
# running, then the app's guest command under the runtime -- directly for an
# aarch64 program (scripts/run-native.sh), through FEX for an x86 one
# (scripts/run-fex.sh), by the entry's "architecture" (default x86_64). The
# launcher's counterpart of scripts/run-steam.sh (see launcher/SPEC.md).
#
#   scripts/run-app.sh <app-id>            start (or re-show) an app from
#                                          $STATE/launcher/apps.json; "steam"
#                                          is the built-in Steam client
#   scripts/run-app.sh --stop              stop the guest processes (X keeps running)
#   scripts/run-app.sh --dry-run <app-id>  print what would run; start nothing
#   scripts/run-app.sh --help
#
# Display ("display" in settings, or STEAMARM_DISPLAY which overrides it):
#   native (default)  the native rootless X server (scripts/run-x11-native.sh,
#                     display :2): every X window is a real macOS window
#   vnc               Xvnc on display :1, shown through macOS Screen Sharing
# Settings ($STATE/launcher/settings.json): "display" as above; "resolution"
# is the Xvnc geometry (vnc only), applied when Xvnc is (re)started with no app
# running; "metalHud" exports MTL_HUD_ENABLED=1; "extraEnv" goes to every app.
# The PID of the launched program goes to $STATE/launcher/running.pid (its id
# to running.id, its display mode to running.display, "<arch> <translator>" to
# running.arch) and its output to $STATE/logs/<id>-<time>.log.
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT=/tmp/lxrt-steamroot
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
LOGS="$STATE/logs"
LDIR="$STATE/launcher"
PIDFILE="$LDIR/running.pid"
IDFILE="$LDIR/running.id"
MODEFILE="$LDIR/running.display"
ARCHFILE="$LDIR/running.arch"
X11_BUNDLE_ID=org.steamarm.X11
STEAM_PATTERN='build/lxrun .*ubuntu12_32/steam '

usage() { sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; }

# Runtime processes that are guest programs: everything but Xvnc and FEXServer.
# (The pattern must name build/lxrun: a bare word would match this shell. The
# native X server is a host process, not an lxrun one, so it never shows up.)
guest_pids() {
    for p in $(pgrep -f "build/lxrun"); do
        case "$(ps -o command= -p "$p")" in
            *Xvnc*|*FEXServer*) ;;
            *) echo "$p" ;;
        esac
    done
}

stop_guests() {
    for p in $(guest_pids); do kill -9 "$p" 2>/dev/null; done
    rm -f "$ROOT/tmp/fexhome/.steam/steam.pid" "$PIDFILE" "$IDFILE" "$MODEFILE" "$ARCHFILE"
}

# Prints shell assignments (APP_NAME, APP_ARCH, APP_ROOT, APP_FEXROOTFS, GEOMETRY, DMODE
# and the arrays APP_ENV, APP_CMD) for app $1, from apps.json and settings.json.
resolve_app() {
    /usr/bin/python3 - "$1" "$LDIR/apps.json" "$LDIR/settings.json" <<'PY'
import json, os, re, shlex, sys
app_id, apps_path, settings_path = sys.argv[1:4]

def load(path, default):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default

steam = {
    "id": "steam", "name": "Steam",
    "command": ["/bin/bash", "/tmp/fexhome/.local/share/Steam/steam.sh", "-noverifyfiles"],
    "root": "/tmp/lxrt-steamroot", "fexRootfs": "/", "env": {},
    # The x86 client under FEX: TRANSITIONAL_COMPATIBILITY (docs/APPLICATION_MANAGER.md).
    "architecture": "x86_64",
}
apps = load(apps_path, [])
if isinstance(apps, dict):
    apps = apps.get("apps", [])
app = steam if app_id == "steam" else next((a for a in apps if a.get("id") == app_id), None)
if app is None:
    sys.stderr.write("run-app: unknown app id %r (not in %s)\n" % (app_id, apps_path))
    sys.exit(2)
cmd = [str(c) for c in app.get("command") or []]
# -noverifyfiles on a fresh install also skips downloading the client itself
# (the bootstrap finds no steamui.so and exits): only for an installed client.
if app_id == "steam" and not os.path.exists(
        "/tmp/lxrt-steamroot/tmp/fexhome/.local/share/Steam/ubuntu12_32/steamui.so"):
    cmd = [c for c in cmd if c != "-noverifyfiles"]
if not cmd:
    sys.stderr.write("run-app: app %r has no command\n" % app_id)
    sys.exit(2)
# ARM64-first: aarch64 runs directly under the runtime, x86 through FEX.
arch = str(app.get("architecture") or "x86_64")
if arch not in ("aarch64", "x86_64", "i386"):
    sys.stderr.write("run-app: app %r has architecture %r (aarch64, x86_64 or i386)\n" % (app_id, arch))
    sys.exit(2)
settings = load(settings_path, {})
if not isinstance(settings, dict):
    settings = {}

import importlib.util
spec = importlib.util.spec_from_file_location(
    "settings_env", os.path.join(os.getcwd(), "scripts", "settings-env.py"))   # cwd: the checkout
env = {}
env.update(app.get("env") or {})
# The launcher's settings (launcher/SETTINGS_SPEC.md) -> environment.
senv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(senv)
env.update(senv.env_from_settings(settings))
senv.write_limits(settings)
pairs = []
for k, v in sorted(env.items()):
    # Translator settings are for x86 payloads only, never a native program.
    if arch == "aarch64" and str(k).startswith("FEX_"):
        continue
    if re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", str(k)):
        pairs.append("%s=%s" % (k, v))

geometry = str(settings.get("resolution") or "1600x900")
if not re.match(r"^[0-9]{3,5}x[0-9]{3,5}$", geometry):
    geometry = "1600x900"

mode = str(settings.get("display") or "native")
if mode not in ("native", "vnc"):
    mode = "native"

q = shlex.quote
print("APP_NAME=%s" % q(str(app.get("name") or app_id)))
print("APP_ARCH=%s" % q(arch))
default_root = "/tmp/lxrt-arm64root" if arch == "aarch64" else "/tmp/lxrt-steamroot"
print("APP_ROOT=%s" % q(str(app.get("root") or default_root)))
fr = app.get("fexRootfs")
print("APP_FEXROOTFS=%s" % q("/" if fr is None else str(fr)))
print("GEOMETRY=%s" % q(geometry))
print("DMODE=%s" % q(mode))
print("APP_ENV=(%s)" % " ".join(q(p) for p in pairs))
print("APP_CMD=(%s)" % " ".join(q(c) for c in cmd))
PY
}

xvnc_pid() { pgrep -f 'lxrun /usr/bin/Xvnc' | head -1; }

xvnc_geometry() {
    ps -o command= -p "$1" 2>/dev/null | sed -n 's/.*-geometry \([0-9]*x[0-9]*\).*/\1/p'
}

# VNC password file: the classic VNC obfuscation (DES, fixed key) of at most
# eight characters, which is what Xvnc's -rfbauth reads (as run-steam.sh).
ensure_password() {
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
}

# Xvnc at $GEOMETRY. A running server with another geometry is restarted only
# when no guest program is using the display.
ensure_xvnc() {
    local x
    x="$(xvnc_pid)"
    if [ -n "$x" ] && [ "$(xvnc_geometry "$x")" != "$GEOMETRY" ] && [ -z "$(guest_pids)" ]; then
        echo "Restarting Xvnc at $GEOMETRY"
        kill "$x" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$x" 2>/dev/null || break
            sleep 0.5
        done
        kill -9 "$x" 2>/dev/null
        rm -f "$ROOT/tmp/.X1-lock" "$ROOT/tmp/.X11-unix/X1"
        x=""
    fi
    if [ -z "$x" ]; then
        ensure_password
        cp "$PWFILE" "$ROOT/tmp/.vncpasswd"
        # Xvnc compiles its keymap with popen("xkbcomp ...") through /bin/sh, an
        # x86 bash here: it needs FEX's environment and a FEXServer.
        pgrep -f 'lxrun .*FEXServer' >/dev/null || scripts/run-fex.sh /bin/true >/dev/null 2>&1
        TMPDIR=/tmp HOME=/tmp/fexhome FEX_ROOTFS=/ FEX_GUESTBASE=1 \
        LXRT_ROOT=$ROOT nohup ./build/lxrun /usr/bin/Xvnc :1 -geometry "$GEOMETRY" -depth 24 \
            -SecurityTypes VncAuth -rfbauth /tmp/.vncpasswd -localhost \
            -rfbport 5901 +extension GLX -fp built-ins -nolisten tcp \
            > "$LOGS/xvnc.log" 2>&1 < /dev/null &
        sleep 3
    fi
}

# The native rootless X server on :2 (a host app, SteamARM-X11).
native_x_running() { pgrep -f 'SteamARM-X11.app/Contents/MacOS/X11.bin :2' >/dev/null; }

ensure_native_x() {
    scripts/run-x11-native.sh start :2 >/dev/null ||
        { echo "The native X server (display :2) failed to start." >&2; exit 1; }
}

# Bring the display to the front: the X server app in native mode (its windows
# are the app's windows), Screen Sharing in vnc mode. $1 = mode.
show_display() {
    if [ "$1" = native ]; then
        native_x_running &&
            osascript -e "tell application id \"$X11_BUNDLE_ID\" to activate" >/dev/null 2>&1
        return 0
    fi
    open "vnc://127.0.0.1:5901"
}

DRY=0
case "${1:-}" in
    ""|-h|--help) usage; [ -n "${1:-}" ]; exit $? ;;
    --stop) stop_guests; exit 0 ;;
    --dry-run) DRY=1; shift ;;
esac
[ $# -ge 1 ] || { usage >&2; exit 2; }
ID="$1"
mkdir -p "$LOGS" "$LDIR"

SPEC="$(resolve_app "$ID")" || exit 2
eval "$SPEC"
if [ "$APP_ARCH" = aarch64 ]; then
    RUNNER=scripts/run-native.sh; TRANSLATOR=none
else
    RUNNER=scripts/run-fex.sh; TRANSLATOR=FEX
fi
MODE="${STEAMARM_DISPLAY:-$DMODE}"
case "$MODE" in
    native) DISP=:2 ;;
    vnc) DISP=:1 ;;
    *) echo "run-app: unknown display mode '$MODE' (native or vnc)" >&2; exit 2 ;;
esac

if [ "$DRY" = 1 ]; then
    echo "app:      $ID ($APP_NAME)"
    echo "display:  $MODE (DISPLAY=$DISP)"
    echo "arch:     $APP_ARCH (translator: $TRANSLATOR, session: ZERO-VM)"
    if [ "$APP_ARCH" = aarch64 ]; then echo "root:     LXRT_ROOT=$APP_ROOT DISPLAY=$DISP"
    else echo "root:     LXRT_ROOT=$APP_ROOT FEX_ROOTFS=$APP_FEXROOTFS DISPLAY=$DISP"; fi
    echo "env:      ${APP_ENV[*]+${APP_ENV[*]}}"
    echo "command:  $RUNNER ${APP_CMD[*]}"
    if [ "$MODE" = native ]; then
        if native_x_running; then echo "x11:      native X server running on :2"
        else echo "x11:      would start the native X server on :2 (scripts/run-x11-native.sh)"; fi
    else
        x="$(xvnc_pid)"
        if [ -z "$x" ]; then echo "xvnc:     would start at $GEOMETRY"
        elif [ "$(xvnc_geometry "$x")" != "$GEOMETRY" ]; then
            echo "xvnc:     running at $(xvnc_geometry "$x"); restart to $GEOMETRY only if no app runs"
        else echo "xvnc:     running at $GEOMETRY"; fi
    fi
    g="$(guest_pids | tr '\n' ' ')"
    echo "guests:   ${g:-none}"
    exit 0
fi

# One app at a time. Re-show the display when this app is already running (on
# the display it was started on, whatever the setting says now).
if [ -n "$(guest_pids)" ]; then
    rid="$(cat "$IDFILE" 2>/dev/null)"
    rpid="$(cat "$PIDFILE" 2>/dev/null)"
    rmode="$(cat "$MODEFILE" 2>/dev/null)"
    if [ "$rid" = "$ID" ] && [ -n "$rpid" ] && kill -0 "$rpid" 2>/dev/null; then
        show_display "${rmode:-$MODE}"
        echo "$APP_NAME is already running."
        exit 0
    fi
    if [ "$ID" = steam ] && spid="$(pgrep -f "$STEAM_PATTERN" | head -1)" && [ -n "$spid" ]; then
        # Started elsewhere (e.g. run-steam.sh): its DISPLAY tells the mode.
        case "$(ps -E -o command= -p "$spid" 2>/dev/null)" in
            *" DISPLAY=:1"*) rmode=vnc ;;
            *" DISPLAY=:2"*) rmode=native ;;
            *) rmode="$MODE" ;;
        esac
        echo "$spid" > "$PIDFILE"
        echo steam > "$IDFILE"
        echo "$rmode" > "$MODEFILE"
        show_display "$rmode"
        echo "Steam is already running."
        exit 0
    fi
    echo "Another Linux program is running (scripts/run-app.sh --stop stops it)." >&2
    exit 3
fi

if [ "$MODE" = native ]; then ensure_native_x; else ensure_xvnc; fi

# Sound (scripts/audio.sh) at the launcher's volume, unless it is muted.
VOL="$(/usr/bin/python3 scripts/settings-env.py --volume "$LDIR/settings.json")"
[ -n "$VOL" ] && { LXRT_ROOT="$APP_ROOT" scripts/audio.sh start "$VOL" >/dev/null || echo "run-app: no sound (scripts/audio.sh)" >&2; }
# Controllers (scripts/input.sh): the launcher's Entrada page, as /dev/input.
scripts/input.sh start >/dev/null || echo "run-app: no controllers for games (scripts/input.sh)" >&2

L="$LOGS/$ID-$(date +%Y%m%d-%H%M%S).log"
echo "$L" > "$LOGS/current"
# env execs nohup, which execs the runner, which execs build/lxrun: $! ends up
# being the runtime process of the program itself.
if [ "$APP_ARCH" = aarch64 ]; then
    env ${APP_ENV[@]+"${APP_ENV[@]}"} DISPLAY=$DISP LXRT_ROOT="$APP_ROOT" \
        nohup "$RUNNER" "${APP_CMD[@]}" > "$L" 2>&1 < /dev/null &
else
    env ${APP_ENV[@]+"${APP_ENV[@]}"} DISPLAY=$DISP LXRT_ROOT="$APP_ROOT" FEX_ROOTFS="$APP_FEXROOTFS" \
        nohup "$RUNNER" "${APP_CMD[@]}" > "$L" 2>&1 < /dev/null &
fi
echo $! > "$PIDFILE"
echo "$ID" > "$IDFILE"
echo "$MODE" > "$MODEFILE"
echo "$APP_ARCH $TRANSLATOR" > "$ARCHFILE"
echo "$APP_NAME ($APP_ARCH, translator: $TRANSLATOR) starting on $DISP (log $L)."

if [ "$MODE" = vnc ]; then
    show_display vnc
    echo "Screen Sharing: password in $STATE/vncpasswd.txt"
else
    echo "Its windows open as native macOS windows."
fi
