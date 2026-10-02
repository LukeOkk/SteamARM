#!/bin/bash
# Start one app of the SteamARM launcher, no VM: the X server if it is not
# running, then the app's guest command under the runtime -- directly for an
# aarch64 program (scripts/run-native.sh), through FEX for an x86 one
# (scripts/run-fex.sh), by the entry's "architecture" (default x86_64). The
# launcher's counterpart of scripts/run-steam.sh (see launcher/SPEC.md).
#
#   scripts/run-app.sh <app-id>            start (or re-show) an app from
#                                          $STATE/launcher/apps.json;
#                                          scripts/builtin-apps.json defines "steam"
#                                          (x86), "steam-arm64" and "steam-arm64-frame" (experimental)
#   scripts/run-app.sh --stop              stop the guest processes (X keeps running)
#   scripts/run-app.sh --reap [SID]        stop what a finished session left behind
#                                          (default: the session in running.pgid)
#   scripts/run-app.sh --dry-run <app-id>  print what would run; start nothing
#   scripts/run-app.sh --help
#
# On success the last line says what happened: "session=started", or
# "session=reshown" (the app was already running), or "session=adopted" (a
# Steam that scripts/run-steam.sh started: no session wrapper).
#
# Display ("display" in settings, or STEAMARM_DISPLAY which overrides it):
#   native (default)  the native rootless X server (scripts/run-x11-native.sh,
#                     display :2): every X window is a real macOS window
#   vnc               Xvnc on display :1, shown through macOS Screen Sharing
# Settings ($STATE/launcher/settings.json): "display" as above; "resolution"
# is the Xvnc geometry (vnc only), applied when Xvnc is (re)started with no app
# running; "metalHud" exports MTL_HUD_ENABLED=1; "extraEnv" goes to every app.
# An Android card ("kind": "android") runs scripts/android-session.py run
# <package> in that session: Weston on :2, the x86_64 Android root booted under
# FEX, the APK installed with pm and started with am (docs/APK_SUPPORT.md);
# what the session cannot run (arm64-v8a-only code, 32-bit ARM or x86, a minSdk
# above 30) is refused with the reason, exit 2.
# Every app runs in a process group of its own (scripts/session.py): the PID
# of that group's leader goes to $STATE/launcher/running.pid (its id to
# running.id, its display mode to running.display, "<arch> <translator>" to
# running.arch, the group to running.pgid), the program's exit status to
# running.status when it ends, and its output to $STATE/logs/<id>-<time>.log.
# --stop signals that group first; leftover guest processes are killed after.
# The wrapper also leads a session of its own. A helper the program puts in
# a process group of its own leaves the group but not the session, and can
# outlive the program with parent 1: the native client's webhelper zygotes
# do (MEASURED, benchmarks/stage23-frame-root.txt C2 and F7; that they keep
# the session is the POSIX rule, not measured on them). Once the wrapper has
# exited, the guest programs still in its session get SIGTERM, then SIGKILL
# 2 s later (--reap; a watcher started with the session runs it, and so does
# the launcher). Nothing outside that session is touched; a program that
# starts a session of its own (setsid) is not in it, and only --stop stops it.
# STEAMARM_GRAPHICS_BACKEND and STEAMARM_SYNCHRONIZATION, set by the launcher
# when its fallback policy launches without a setting that cannot work here,
# go over the settings (scripts/settings-env.py fallback_overrides).
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
PGIDFILE="$LDIR/running.pgid"
STATUSFILE="$LDIR/running.status"   # read and removed by the launcher, not here
X11_BUNDLE_ID=org.steamarm.X11
STEAM_PATTERN='build/lxrun .*ubuntu12_32/steam '
# The native ARM64 client (scripts/run-steam-arm64.sh); which entry it is
# comes from its root, in its environment (the runtime keeps LXRT_* through
# guest execs): /tmp/lxrt-armroot is steam-arm64, /tmp/lxrt-arm64root
# steam-arm64-frame (scripts/builtin-apps.json).
ARM64_PATTERN='build/lxrun .*steamrtarm64/steam '

# The client of this entry, started outside this script (run-steam.sh,
# run-steam-arm64.sh): its pid, or nothing.
outside_client() {
    local p want
    case "$ID" in
        steam) pgrep -f "$STEAM_PATTERN" | head -1 ;;
        steam-arm64|steam-arm64-frame)
            want=/tmp/lxrt-armroot
            [ "$ID" = steam-arm64-frame ] && want=/tmp/lxrt-arm64root
            for p in $(pgrep -f "$ARM64_PATTERN"); do
                ps -E -o command= -p "$p" 2>/dev/null | tr ' ' '\n' | grep -qx "LXRT_ROOT=$want" && { echo "$p"; return; }
            done ;;
    esac
}

# "Escala de resolución" (Settings): Wine's display-mode emulation in the game
# and app prefixes (scripts/wine-prefix-options.py). Only while no wineserver
# runs: one rewrites its prefix's user.reg when it exits.
apply_resolution_scaling() {
    pgrep -f "build/lxrun .*wineserver" >/dev/null 2>&1 && return 0
    /usr/bin/python3 scripts/wine-prefix-options.py emulate-modeset \
        "$(/usr/bin/python3 scripts/settings-env.py --emulate-modeset "$1")" >/dev/null 2>&1 || true
}

usage() { awk 'NR > 1 { if ($0 !~ /^#/) exit; print }' "$0" | sed 's/^# \{0,1\}//'; }

# Runtime processes that are guest programs: everything but Xvnc and FEXServer.
# (The pattern must name build/lxrun: a bare word would match this shell. The
# native X server is a host process, not an lxrun one, so it never shows up.)
# The Android session's processes (LXRT_SESSION=android, set by
# scripts/android-session.py and carried through guest execs) are not the
# launched app's: stopping a game must not end the Android session.
guest_pids() {
    for p in $(pgrep -f "build/lxrun"); do
        case "$(ps -o command= -p "$p")" in
            *Xvnc*|*FEXServer*) ;;
            *) ps -E -o command= -p "$p" 2>/dev/null | tr ' ' '\n' | grep -qx "LXRT_SESSION=android" ||
                   echo "$p" ;;
        esac
    done
}

stop_guests() {
    # The session's group first (SIGTERM, then SIGKILL after 3 s); anything
    # else that is a guest program afterwards, as before.
    /usr/bin/python3 scripts/session.py stop "$LDIR" 3
    for p in $(guest_pids); do kill -9 "$p" 2>/dev/null; done
    rm -f "$ROOT/tmp/fexhome/.steam/steam.pid" "$PIDFILE" "$IDFILE" "$MODEFILE" "$ARCHFILE" "$PGIDFILE"
    # Steam's shared memory objects (/dev/shm/u<uid>-Shm_<hex>, 26 MB each,
    # in the runtime's /tmp/lxrt-shm-<uid>) outlive a client that is stopped
    # rather than quit: 193 of them, 4.4 GB, after a day of starts (MEASURED
    # 2026-09-30). With no guest program left, none of them is in use.
    sleep 1
    if [ -z "$(guest_pids)" ]; then
        rm -f "/tmp/lxrt-shm-$(id -u)/u$(id -u)-Shm_"* 2>/dev/null
    fi
}

# Guest programs (as guest_pids) in session $1: the session a launcher
# session's wrapper (scripts/session.py run) leads, so it is also the
# wrapper's PID and running.pgid.
session_guests() {
    local pids
    pids="$(guest_pids)"
    [ -n "$pids" ] || return 0
    # shellcheck disable=SC2086
    /usr/bin/python3 -c '
import os, sys
sid = int(sys.argv[1])
for pid in sys.argv[2:]:
    try:
        if os.getsid(int(pid)) == sid:
            print(pid)
    except OSError:
        pass
' "$1" $pids
}

# What session $1 left behind once its wrapper is gone: SIGTERM, then SIGKILL
# for what is still there after 2 s (scripts/session.py's KILL_AFTER). Does
# nothing while a process with that PID lives: the session is still running,
# or the number is another process's now.
reap_session() {
    local sid="$1" left
    case "$sid" in ''|*[!0-9]*) return 0 ;; esac
    [ "$sid" -gt 1 ] || return 0
    kill -0 "$sid" 2>/dev/null && return 0
    left="$(session_guests "$sid")"
    [ -n "$left" ] || return 0
    echo "run-app: stopping what session $sid left behind: $(echo $left)"
    # shellcheck disable=SC2086
    kill -TERM $left 2>/dev/null
    for _ in 1 2 3 4 5 6 7 8 9 10; do
        sleep 0.2
        left="$(session_guests "$sid")"
        [ -n "$left" ] || return 0
    done
    # shellcheck disable=SC2086
    kill -KILL $left 2>/dev/null
    return 0
}

# Prints shell assignments (APP_NAME, APP_ARCH, APP_ROOT, APP_IN_X86_ROOT, APP_FEXROOTFS, GEOMETRY, DMODE
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
builtins = load(os.path.join(os.getcwd(), "scripts", "builtin-apps.json"), [])   # cwd: the checkout
if not isinstance(builtins, list):
    builtins = []
if not any(b.get("id") == "steam" for b in builtins if isinstance(b, dict)):
    builtins.append(steam)
# Built-ins first: an apps.json entry cannot shadow them.
app = next((a for a in builtins if isinstance(a, dict) and a.get("id") == app_id), None) \
      or next((a for a in apps if isinstance(a, dict) and a.get("id") == app_id), None)
if app is None:
    sys.stderr.write("run-app: unknown app id %r (not in %s)\n" % (app_id, apps_path))
    sys.exit(2)
# An Android app (docs/APK_SUPPORT.md): installed by scripts/android-pm.py,
# opened by scripts/android-session.py in SteamARM's Android session -- the
# x86_64 Android 11 root under FEX, zero VM, its screen a macOS window
# (benchmarks/stage28-android-apk.txt). What the session cannot run is refused
# with the reason, as the launcher's card says it (AndroidApps.unavailableReason):
# arm64-v8a-only code (Android's arm64 ART does not start on macOS: the ART
# heap wall), 32-bit ARM, 32-bit x86, other ABIs, a minSdk above API 30.
android_pkg = None
if app.get("kind") == "android":
    info = app.get("android") if isinstance(app.get("android"), dict) else {}
    pkg = str(info.get("package") or "")
    state = os.environ.get("STEAMARM_STATE") or os.path.expanduser("~/SteamARM-roots")
    meta = load(os.path.join(state, "android", "packages", pkg, "meta.json"), {}) if pkg else {}
    if not isinstance(meta, dict):
        meta = {}
    abis = info.get("abis") if isinstance(info.get("abis"), list) else meta.get("abis")
    abis = [str(x) for x in abis] if isinstance(abis, list) else None
    min_sdk = str(info.get("minSdk") if info.get("minSdk") is not None else meta.get("minSdk") or "")
    why = None
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+", pkg):
        why = "it names no valid package"
    elif abis is None:
        why = "its ABIs are unknown (reinstall the APK)"
    elif min_sdk and not min_sdk.isdigit():
        why = "it needs a preview Android (minSdk %s); the session is Android 11 (API 30)" % min_sdk
    elif min_sdk.isdigit() and int(min_sdk) > 30:
        why = "it needs API %s; the session is Android 11 (API 30)" % min_sdk
    elif "x86_64" in abis or not abis:
        why = None
    elif "arm64-v8a" in abis:
        why = ("its native code is arm64-v8a only: Android's arm64 ART does not start on macOS (its heap "
               "must be below 4 GiB; docs/ANDROID_RUNTIME_ARCHITECTURE.md), and the session is x86_64 under FEX")
    elif set(abis) & {"armeabi-v7a", "armeabi"}:
        why = "its native code is 32-bit ARM only, which Apple silicon does not run"
    elif "x86" in abis:
        why = "its native code is 32-bit x86 only; the session runs x86_64 and dex-only apps"
    else:
        why = "its native code is for %s only" % ", ".join(sorted(abis))
    if why:
        sys.stderr.write("run-app: app %r is an Android app SteamARM's Android session cannot run: %s\n"
                         % (app_id, why))
        sys.exit(2)
    android_pkg = pkg
    app = dict(app, command=["run", pkg], architecture="x86_64",
               root=os.environ.get("ANDROID_SESSION_ROOT") or "/Volumes/SteamARMAndroid/session")
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
senv = importlib.util.module_from_spec(spec)
spec.loader.exec_module(senv)
# A built-in entry's own settings live in settings.json (the launcher's
# builtinOverrides): its definition comes from builtin-apps.json.
_bo = settings.get("builtinOverrides")
if isinstance(_bo, dict) and isinstance(_bo.get(app_id), dict) and \
        any(isinstance(b, dict) and b.get("id") == app_id for b in builtins):
    app = dict(app, overrides=dict(app.get("overrides") or {}, **_bo[app_id]))
settings = senv.with_overrides(settings, app.get("overrides"))
# The launcher's fallbacks for this launch go over the app's own choices.
settings = senv.with_overrides(settings, senv.fallback_overrides(os.environ))
# Steam's Linux fossilize replay stalls here while processing Schedule I.
# This flag affects Valve's pre-cache only; DXVK/VKD3D and Metal cache remain.
env = {"STEAM_ENABLE_SHADER_CACHE_MANAGEMENT":
       os.environ.get("STEAM_ENABLE_SHADER_CACHE_MANAGEMENT", "0")} if app_id == "steam" else {}
env.update(app.get("env") or {})
# The launcher's settings (launcher/SETTINGS_SPEC.md) -> environment.
env.update(senv.env_from_settings(settings))
senv.write_limits(settings)
if app.get("kind") == "windows":
    pspec = importlib.util.spec_from_file_location(
        "proton_command", os.path.join(os.getcwd(), "scripts", "proton-command.py"))
    proton = importlib.util.module_from_spec(pspec)
    pspec.loader.exec_module(proton)
    try:
        root = app.get("root") or "/tmp/lxrt-steamroot"
        if root == "/tmp/lxrt-steamroot":
            root = os.path.join(senv.STATE, "steamroot")
        cmd, proton_env = proton.resolve(app, root)
    except (OSError, ValueError) as error:
        sys.stderr.write("run-app: %s\n" % error)
        sys.exit(2)
    env.update(proton_env)
# LXRT_EXEC_ARGS_EXTRA (debugging, from the caller): more switches for a
# program, after the ones the settings give (runtime/process.c applies
# every matching entry).
extra_args = os.environ.get("LXRT_EXEC_ARGS_EXTRA")
if extra_args:
    env["LXRT_EXEC_ARGS"] = (env["LXRT_EXEC_ARGS"] + ";" if env.get("LXRT_EXEC_ARGS") else "") + extra_args
pairs = []
for k, v in sorted(env.items()):
    # Translator settings are for x86 payloads only, never a native program:
    # an ARM64 Steam client carries them as STEAMARM_FEXOPT_* to the games
    # it starts through FEX (tools/steamarm-fex-proton turns them back).
    if arch == "aarch64" and str(k).startswith("FEX_"):
        k = "STEAMARM_FEXOPT_" + k[4:]
    if re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", str(k)):
        pairs.append("%s=%s" % (k, v))

geometry = str(settings.get("resolution") or "1600x900")
if not re.match(r"^[0-9]{3,5}x[0-9]{3,5}$", geometry):
    geometry = "1600x900"

mode = str(settings.get("display") or "native")
if mode not in ("native", "vnc"):
    mode = "native"

q = shlex.quote
# One line: the launcher reads this script's last line ("session=...").
print("APP_NAME=%s" % q(re.sub(r"[\x00-\x1f\x7f]+", " ", str(app.get("name") or app_id))))
print("APP_ARCH=%s" % q(arch))
ARM64_ROOT, X86_ROOT = "/tmp/lxrt-arm64root", "/tmp/lxrt-steamroot"
root = str(app.get("root") or (ARM64_ROOT if arch == "aarch64" else X86_ROOT))
# The rule of LaunchPlanner (launcher/ApplicationCore.swift): the ARM64 base
# runs aarch64 code only, the x86-64 Steam root only x86 code under FEX. An
# aarch64 program in the x86 root would find no aarch64 loader or libraries.
same = lambda a, b: os.path.realpath(a) == os.path.realpath(b)
print("APP_ANDROID=%s" % q(android_pkg or ""))
print("APP_KIND=%s" % q(str(app.get("kind") or "")))
if (arch == "aarch64" and same(root, X86_ROOT)) or (arch != "aarch64" and same(root, ARM64_ROOT)):
    want = ARM64_ROOT if arch == "aarch64" else X86_ROOT
    sys.stderr.write("run-app: app %r is %s but its root is %s; %s programs run in %s. "
                     "Install it there.\n" % (app_id, arch, root, arch, want))
    sys.exit(2)
print("APP_ROOT=%s" % q(root))
print("APP_IN_X86_ROOT=%s" % (1 if same(root, X86_ROOT) else 0))
fr = app.get("fexRootfs")
print("APP_FEXROOTFS=%s" % q("/" if fr is None else str(fr)))
print("APP_PREFIX=%s" % q(env.get("STEAM_COMPAT_DATA_PATH", "") if app.get("kind") == "windows" else ""))
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
        pgrep -f 'lxrun /tmp/lxrt-root/usr/bin/FEXServer' >/dev/null || scripts/run-fex.sh /bin/true >/dev/null 2>&1
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
    --reap) reap_session "${2:-$(cat "$PGIDFILE" 2>/dev/null)}"; exit 0 ;;
    --reap-after)
        # The watcher each session gets (below): once the wrapper $2 has
        # exited, what its session left behind.
        case "${2:-}" in ''|*[!0-9]*) usage >&2; exit 2 ;; esac
        while kill -0 "$2" 2>/dev/null; do sleep 1; done
        reap_session "$2"; exit 0 ;;
    --dry-run) DRY=1; shift ;;
esac
[ $# -ge 1 ] || { usage >&2; exit 2; }
ID="$1"
mkdir -p "$LOGS" "$LDIR"

SPEC="$(resolve_app "$ID")" || exit 2
eval "$SPEC"
# Applied to the settings above; not for the program's environment.
unset STEAMARM_GRAPHICS_BACKEND STEAMARM_SYNCHRONIZATION
if [ -n "$APP_ANDROID" ]; then
    # The Android session: the x86_64 root under FEX
    # (scripts/android-session.py run <package>), each app in a macOS window
    # of its own through steamarm-wlmac (tools/wlmac);
    # ANDROID_SESSION_COMPOSITOR=weston: all of Android in one window, Weston
    # on the native X server.
    RUNNER=scripts/android-session.py; TRANSLATOR=FEX
    export ANDROID_SESSION_COMPOSITOR="${ANDROID_SESSION_COMPOSITOR:-wlmac}"
elif [ "$APP_ARCH" = aarch64 ]; then
    RUNNER=scripts/run-native.sh; TRANSLATOR=none
else
    RUNNER=scripts/run-fex.sh; TRANSLATOR=FEX
fi
MODE="${STEAMARM_DISPLAY:-$DMODE}"
if [ -n "$APP_ANDROID" ] && [ "$MODE" = vnc ]; then
    echo "run-app: $APP_NAME is an Android app; its screen is a Weston window on the native X server, using native windows" >&2
    MODE=native
fi
# Xvnc runs inside the x86 Steam root and binds its socket there; lxrun's
# connect fallback reaches only the host's /tmp/.X11-unix (runtime/socket.c),
# so a program in another root would find no display :1. Native windows instead.
if [ "$MODE" = vnc ] && [ "$APP_IN_X86_ROOT" = 0 ]; then
    echo "run-app: $APP_NAME is outside the x86 Steam root; VNC cannot serve it, using native windows" >&2
    MODE=native
fi
case "$MODE" in
    native) DISP=:2 ;;
    vnc) DISP=:1 ;;
    *) echo "run-app: unknown display mode '$MODE' (native or vnc)" >&2; exit 2 ;;
esac

if [ "$DRY" = 1 ]; then
    echo "app:      $ID ($APP_NAME)"
    echo "display:  $MODE (DISPLAY=$DISP)"
    echo "arch:     $APP_ARCH (translator: $TRANSLATOR, session: ZERO-VM)"
    if [ -n "$APP_ANDROID" ]; then echo "android:  $APP_ANDROID in the Android session (root $APP_ROOT, Weston on $DISP)"
    elif [ "$APP_ARCH" = aarch64 ]; then echo "root:     LXRT_ROOT=$APP_ROOT DISPLAY=$DISP"
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

# What a finished session left behind (its watcher normally got there first).
reap_session "$(cat "$PGIDFILE" 2>/dev/null)"

# One app at a time. Re-show the display when this app is already running (on
# the display it was started on, whatever the setting says now).
if [ -n "$(guest_pids)" ]; then
    rid="$(cat "$IDFILE" 2>/dev/null)"
    rpid="$(cat "$PIDFILE" 2>/dev/null)"
    rmode="$(cat "$MODEFILE" 2>/dev/null)"
    if [ "$rid" = "$ID" ] && [ -n "$rpid" ] && kill -0 "$rpid" 2>/dev/null; then
        show_display "${rmode:-$MODE}"
        echo "$APP_NAME is already running."
        echo "session=reshown"
        exit 0
    fi
    if spid="$(outside_client)" && [ -n "$spid" ]; then
        # Started elsewhere (run-steam.sh, run-steam-arm64.sh): its DISPLAY
        # tells the mode.
        case "$(ps -E -o command= -p "$spid" 2>/dev/null)" in
            *" DISPLAY=:1"*) rmode=vnc ;;
            *" DISPLAY=:2"*) rmode=native ;;
            *) rmode="$MODE" ;;
        esac
        # No wrapper: a status, group or architecture from an earlier
        # session is not this one's.
        rm -f "$STATUSFILE" "$PGIDFILE" "$ARCHFILE"
        # ...but the architecture of the client found is known.
        case "$ID" in steam-arm64*) echo "aarch64 none" > "$ARCHFILE" ;; esac
        echo "$spid" > "$PIDFILE"
        echo "$ID" > "$IDFILE"
        echo "$rmode" > "$MODEFILE"
        show_display "$rmode"
        echo "Steam is already running."
        echo "session=adopted"
        exit 0
    fi
    echo "Another Linux program is running (scripts/run-app.sh --stop stops it)." >&2
    exit 3
fi

# The Steam Frame root's case-sensitive image: attached when it is not, and
# grown to the Mac's disk when nothing has it open (scripts/image-volume.sh;
# it was made with 40 GB and Steam showed 22 GB free).
if [ -z "$APP_ANDROID" ] && [ -d "$STATE/steamframe-root.sparsebundle" ]; then
    frame_link=$(readlink "$STATE/arm64root" 2>/dev/null)
    case "$frame_link" in
        /Volumes/*/*) scripts/image-volume.sh ensure "$STATE/steamframe-root.sparsebundle" \
                          "/Volumes/$(echo "${frame_link#/Volumes/}" | cut -d/ -f1)" ;;
    esac
fi
[ -n "$APP_ANDROID" ] || scripts/env-links.sh "$STATE" >/dev/null || exit 1
# The guest root must be there before anything is made in it: a root on a
# volume that is not attached (the Steam Frame root's sparsebundle) left
# /tmp/lxrt-arm64root missing, and scripts/audio.sh then created
# /tmp/lxrt-arm64root/tmp/pulse as a real directory where the link belongs.
if [ -z "$APP_ANDROID" ] && [ ! -d "$APP_ROOT/usr" ]; then
    echo "run-app: the root $APP_ROOT of $APP_NAME is not there (a volume that is not attached?)" >&2
    exit 2
fi
[ -z "$APP_PREFIX" ] || mkdir -p "$APP_ROOT$APP_PREFIX" || exit 1
if [ "$MODE" = native ]; then ensure_native_x; else ensure_xvnc; fi

if [ -z "$APP_ANDROID" ]; then
# Sound (scripts/audio.sh) at the launcher's volume, unless it is muted.
apply_resolution_scaling "$LDIR/settings.json"
VOL="$(/usr/bin/python3 scripts/settings-env.py --volume "$LDIR/settings.json")"
[ -n "$VOL" ] && { LXRT_ROOT="$APP_ROOT" scripts/audio.sh start "$VOL" >/dev/null || echo "run-app: no sound (scripts/audio.sh)" >&2; }
# Controllers (scripts/input.sh): the launcher's Entrada page, as /dev/input.
scripts/input.sh start >/dev/null || echo "run-app: no controllers for games (scripts/input.sh)" >&2
fi   # the Android session has its own /dev/input (the composer's) and no sound yet

# The memory guard, before the session exists: started here it stays outside
# the app's process group, and a stop does not take it down.
scripts/safeguard.sh start >/dev/null

L="$LOGS/$ID-$(date +%Y%m%d-%H%M%S).log"
echo "$L" > "$LOGS/current"
rm -f "$STATUSFILE" "$PGIDFILE"
# env execs nohup, which execs session.py: $! is the session's group leader.
# It lives while the program does, plus up to 8 s while it clears what the
# program left in its group (scripts/session.py). The runner execs build/lxrun
# under it. PYTHONCOERCECLOCALE: see session.py (no Python locale for guests).
# The log is opened for appending: the session's watcher writes to it too.
SESSION=(env PYTHONCOERCECLOCALE=0 STEAMARM_SESSION_PY=1 /usr/bin/python3 scripts/session.py run "$LDIR" "$RUNNER")
# Free space inside the disk images is the Mac's (runtime/fileops2.c).
LXRT_STATFS_BACKING="$(scripts/image-volume.sh backing 2>/dev/null)"
export LXRT_STATFS_BACKING
# An ARM64 Steam client starts x86 games through SteamARM's FEX
# (tools/steamarm-fex-proton), which needs SteamARM's FEXServer up and will
# not start one itself (docs/FEX_GAME_BOUNDARY.md): started here, with
# scripts/run-fex.sh's fixed environment.
if [ "$APP_ARCH" = aarch64 ] && [ "$APP_KIND" = steam ]; then
    pgrep -f 'lxrun /tmp/lxrt-root/usr/bin/FEXServer' >/dev/null || scripts/run-fex.sh /bin/true >/dev/null 2>&1
fi
if [ "$APP_ARCH" = aarch64 ]; then
    env ${APP_ENV[@]+"${APP_ENV[@]}"} DISPLAY=$DISP LXRT_ROOT="$APP_ROOT" \
        nohup "${SESSION[@]}" "${APP_CMD[@]}" >> "$L" 2>&1 < /dev/null &
else
    env ${APP_ENV[@]+"${APP_ENV[@]}"} DISPLAY=$DISP LXRT_ROOT="$APP_ROOT" FEX_ROOTFS="$APP_FEXROOTFS" \
        nohup "${SESSION[@]}" "${APP_CMD[@]}" >> "$L" 2>&1 < /dev/null &
fi
LEADER=$!
echo "$LEADER" > "$PIDFILE"
echo "$ID" > "$IDFILE"
echo "$MODE" > "$MODEFILE"
echo "$APP_ARCH $TRANSLATOR" > "$ARCHFILE"
# The session's watcher: whoever else is watching (the launcher may be
# closed), what the session leaves behind is stopped once its wrapper exits.
# The wrapper is a background job of this non-interactive shell, never a group
# leader, so its setsid succeeds and the session's ID is its PID
# (scripts/session.py own_group).
nohup /bin/bash scripts/run-app.sh --reap-after "$LEADER" >> "$L" 2>&1 < /dev/null &
echo "$APP_NAME ($APP_ARCH, translator: $TRANSLATOR) starting on $DISP (log $L)."

if [ "$MODE" = vnc ]; then
    show_display vnc
    echo "Screen Sharing: password in $STATE/vncpasswd.txt"
else
    echo "Its windows open as native macOS windows."
fi
echo "session=started"
