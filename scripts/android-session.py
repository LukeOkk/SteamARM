#!/usr/bin/env python3
"""One Android session on the Mac, with no VM (AGENTS.md): Weston (a macOS
window through SteamARM's X server, or headless), the x86_64 Android root
booted by scripts/android-boot.py until sys.boot_completed=1, and an app from
the launcher's library installed with `pm install` and started with
`am start`. docs/ANDROID_RUNTIME_ARCHITECTURE.md ("Session"),
docs/APK_SUPPORT.md, benchmarks/stage28-android-apk.txt.

  scripts/android-session.py start [--headless] [--size WxH] [--timeout S] [--fex FILE]
  scripts/android-session.py stop
  scripts/android-session.py status
  scripts/android-session.py install <package> [--apk FILE] [--force]
  scripts/android-session.py launch <package> [--apk FILE] [start's options]
  scripts/android-session.py run <package> [start's options]
  scripts/android-session.py shell <guest program> [args...]
  scripts/android-session.py xwd <out.xwd>

start     Weston, then the boot; returns once sys.boot_completed=1 (or fails,
          and stops what it started). One session at a time: a second start
          finds the first ($STATE/android/session.json) and does nothing.
          The first boot of a session root disables the image's apps a window
          on the Mac does not need (TRIM_PACKAGES: camera, gallery, music,
          clock, contacts, backup ...; `shell /system/bin/pm enable <pkg>`
          brings one back), which keeps the session near 55-60 lxrun
          processes (scripts/safeguard.sh stops every guest past its limit).
stop      the boot (every Android process with it), then Weston.
install   <package>'s base and selected split APKs from $STATE/android/packages,
          or --apk, with the image's own `pm install`. Copies stored OBB data
          into the session root. Skips an unchanged installed package.
launch    start if needed, install if needed, then `am start` of the launcher
          activity (meta.json's launcherActivity, else the package manager's
          answer), and wait until its window has the focus.
run       launch, then stay until the session ends: SIGTERM/SIGINT/SIGHUP
          (the launcher's Detener), Weston gone (its window closed) or the
          boot gone. Then stop. scripts/run-app.sh runs Android cards so.
shell     a guest program in the session (root, Android ids): pm, am, cmd,
          dumpsys, input, getprop ...
xwd       a dump of Weston's X window (the macOS window's contents), by the
          Weston root's xwd; tests/android/xwd_colors.py reads it.

The root: $ANDROID_SESSION_ROOT (/Volumes/SteamARMAndroid/session), an APFS
clone of $ANDROID_X86_ROOT (/Volumes/SteamARMAndroid/root-x86_64) made the
first time (seconds, no space), so the session's /data -- installed apps and
their data -- is its own and the root the tests use is not written. --fex
puts a FEX-emu (scripts/build-fex-host.sh) into it when it is made. The
volume is attached from $STATE/android.sparsebundle when it is not.
Host state (sockets, so it must be short): $ANDROID_SESSION_DIR
(/tmp/lxrt-android-session-<uid>): binder, properties, /dev/input, init.log,
logcat.txt. Log: $STATE/logs/android-<date>.log (this program and the boot).
Weston: WESTON_XDG=$ANDROID_SESSION_XDG (/dev/shm/steamarm-android), kiosk
shell (Android's one window fills Weston's output), 1024x768 by default.

Sound (with a window; STEAMARM_ANDROID_SOUND=0: none): SteamARM's PulseAudio
(scripts/audio.sh) with a socket in the session root, /tmp/pulse/native to
Android, where Waydroid's audio HAL in audioserver plays to it
(android-boot.py --pulse).

Stops only what it started: the boot it recorded (checked to be
android-boot.py on this session's directory) and the Weston on its own socket.
"""
import argparse
import datetime
import filecmp
import fcntl
import json
import os
import re
import shutil
import signal
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
STATE = os.environ.get("STEAMARM_STATE") or os.path.expanduser("~/SteamARM-roots")
ADIR = os.path.join(STATE, "android")
SESSION_FILE = os.path.join(ADIR, "session.json")
LOCK_FILE = os.path.join(ADIR, "session.lock")
LOGS = os.path.join(STATE, "logs")
VOLUME = "/Volumes/SteamARMAndroid"
BUNDLE = os.path.join(STATE, "android.sparsebundle")
BASE_ROOT = os.environ.get("ANDROID_X86_ROOT") or VOLUME + "/root-x86_64"
ROOT = os.environ.get("ANDROID_SESSION_ROOT") or VOLUME + "/session"
RUN_DIR = os.environ.get("ANDROID_SESSION_DIR") or "/tmp/lxrt-android-session-%d" % os.getuid()
XDG = os.environ.get("ANDROID_SESSION_XDG") or "/dev/shm/steamarm-android"
SOCKET = "wayland-0"
LXRUN = os.path.realpath(os.environ.get("LXRUN") or os.path.join(REPO, "build/lxrun"))
DISPLAY = os.environ.get("ANDROID_SESSION_DISPLAY") or ":2"
# FEX's server socket, <root>/data/local/tmp/steamarm-android-<12 hex>.FEXServer.Socket,
# must fit Darwin's 104-byte sun_path (scripts/run-android-display.sh).
FEX_SOCKET_TAIL = len("/data/local/tmp/steamarm-android-000000000000.FEXServer.Socket")

LOGF = None

# The image's apps the session disables once (pm disable-user, kept in the
# session root's /data; `pm enable` brings one back), Lepton-style: what a
# phone has and a window on the Mac does not need, each started at boot as a
# process of its own under FEX. Android is one lxrun process per service and
# per app; the Mac's safeguard (scripts/safeguard.sh) stops every guest past
# its process limit, and these took the first boot of a session over it
# during its first pm install (MEASURED, benchmarks/stage28-android-apk.txt).
# Not here: SystemUI, the launcher, the keyboard, the providers, telephony
# (com.android.phone is persistent), permission controller, settings.
TRIM_PACKAGES = [
    "com.android.camera2",            # no camera (it ANRs on BOOT_COMPLETED waiting for one)
    "com.android.gallery3d",
    "org.lineageos.eleven",           # music player
    "com.android.deskclock",
    "com.android.contacts",
    "com.stevesoltys.seedvault",      # backup
    "com.android.localtransport",     # backup transport
    "com.android.printspooler",
    "com.android.smspush",
    "com.android.dynsystem",          # dynamic system updates
    "org.protonaosp.deviceconfig",
    "com.android.calendar", "org.lineageos.etar",
    "org.lineageos.recorder", "org.lineageos.jelly",
]
TRIM_MARK = "/data/local/tmp/.steamarm-session-trimmed"


def log(msg):
    line = "%s android-session: %s" % (time.strftime("%H:%M:%S"), msg)
    print(line, flush=True)
    if LOGF:
        LOGF.write(line + "\n")
        LOGF.flush()


def die(msg, code=1):
    log(msg)
    sys.exit(code)


def read_session():
    try:
        with open(SESSION_FILE) as f:
            s = json.load(f)
        return s if isinstance(s, dict) else None
    except (OSError, ValueError):
        return None


def write_session(s):
    os.makedirs(ADIR, exist_ok=True)
    tmp = SESSION_FILE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(s, f, indent=1)
    os.replace(tmp, SESSION_FILE)


def open_log(path):
    global LOGF
    os.makedirs(os.path.dirname(path), exist_ok=True)
    LOGF = open(path, "a")


class Lock:
    """start and stop take this: two of them never interleave."""

    def __enter__(self):
        os.makedirs(ADIR, exist_ok=True)
        self.f = open(LOCK_FILE, "w")
        fcntl.flock(self.f, fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc):
        fcntl.flock(self.f, fcntl.LOCK_UN)
        self.f.close()


def command_of(pid):
    try:
        return subprocess.run(["ps", "-o", "command=", "-p", str(pid)], stdout=subprocess.PIPE,
                              stderr=subprocess.DEVNULL, timeout=10).stdout.decode(errors="replace").strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def boot_alive(s):
    """The recorded boot, if it still runs and is this session's."""
    pid = (s or {}).get("bootPid")
    if not isinstance(pid, int) or pid <= 1:
        return False
    try:
        os.kill(pid, 0)
    except OSError:
        return False
    c = command_of(pid)
    return "android-boot.py" in c and s.get("dir", "") in c


def start_sound():
    """SteamARM's PulseAudio with a socket at ROOT/tmp/pulse/native
    (scripts/audio.sh; one server for every root). False when it cannot
    start: the session then has no sound, as before."""
    r = subprocess.run([os.path.join(HERE, "audio.sh"), "start"], cwd=REPO,
                       env=dict(os.environ, LXRT_ROOT=ROOT), stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    ok = r.returncode == 0 and os.path.exists(ROOT + "/tmp/pulse/native")
    log("sound: %s" % ("PulseAudio on /tmp/pulse/native" if ok else
                       "none (%s)" % r.stdout.decode(errors="replace").strip()[-200:]))
    return ok


# The compositor: Weston under lxrun with one macOS window for all of
# Android (its X11 backend on SteamARM's X server; the default), or
# steamarm-wlmac (tools/wlmac, ANDROID_SESSION_COMPOSITOR=wlmac), a native
# macOS Wayland compositor in which Waydroid's multi-window mode gives every
# Android app its own macOS window. Headless sessions always use Weston.
COMPOSITOR = os.environ.get("ANDROID_SESSION_COMPOSITOR") or "weston"


def compositor_script(s=None):
    kind = (s or {}).get("compositor") or COMPOSITOR
    return os.path.join(HERE, "run-wlmac.sh" if kind == "wlmac" else "run-weston.sh")


def weston_env():
    return dict(os.environ, WESTON_XDG=XDG, WESTON_SOCKET=SOCKET, WLMAC_XDG=XDG, WLMAC_SOCKET=SOCKET,
                LXRUN=LXRUN, DISPLAY=DISPLAY)


def weston_pid(s=None):
    r = subprocess.run([compositor_script(s), "status"], env=weston_env(),
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, cwd=REPO)
    m = re.search(r"pid (\d+)", r.stdout.decode(errors="replace"))
    return int(m.group(1)) if r.returncode == 0 and m else None


# ------------------------------------------------------------------ the root
def ensure_volume():
    if os.path.isdir(BASE_ROOT) or not BASE_ROOT.startswith(VOLUME + "/"):
        return
    if not os.path.isdir(BUNDLE):
        die("no Android root at %s and no %s (scripts/mkandroidroot.sh --arch x86_64)" % (BASE_ROOT, BUNDLE))
    log("attaching %s at %s" % (BUNDLE, VOLUME))
    subprocess.run(["hdiutil", "attach", "-nobrowse", "-mountpoint", VOLUME, BUNDLE],
                   stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT, check=False)
    if not os.path.isdir(BASE_ROOT):
        die("no Android root at %s after attaching the volume" % BASE_ROOT)


def refresh_emu(fex=None):
    """A session root is cloned once; the base root's emulator side
    (/usr/lib/lxrt-emu: FEX and its libraries, scripts/mkandroidroot.sh
    --emu) can change after that. Take the base's again when its FEX differs,
    so a FEX update reaches existing sessions. Guest data is not touched."""
    if fex:
        return
    base, mine = BASE_ROOT + "/usr/lib/lxrt-emu", ROOT + "/usr/lib/lxrt-emu"
    try:
        if filecmp.cmp(base + "/FEX", mine + "/FEX", shallow=False):
            return
    except OSError:
        if not os.path.isfile(base + "/FEX"):
            return
    tmp = "%s.new-%d" % (mine, os.getpid())
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run(["cp", "-c", "-R", base, tmp], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.isfile(tmp + "/FEX"):
        shutil.rmtree(tmp, ignore_errors=True)
        log("session root: could not take %s's emulator side; keeping the old one" % BASE_ROOT)
        return
    old = "%s.old-%d" % (mine, os.getpid())
    if os.path.isdir(mine):
        os.rename(mine, old)
    os.rename(tmp, mine)
    shutil.rmtree(old, ignore_errors=True)
    log("session root %s: emulator side (/usr/lib/lxrt-emu) taken again from %s" % (ROOT, BASE_ROOT))


def ensure_root(fex=None):
    if len(ROOT) + FEX_SOCKET_TAIL > 103:
        die("the session root's path is too long for FEX's server socket (%d bytes; at most %d): %s"
            % (len(ROOT), 103 - FEX_SOCKET_TAIL, ROOT))
    if os.path.isfile(ROOT + "/system/bin/toybox"):
        refresh_emu(fex)
        return
    if not os.path.isfile(BASE_ROOT + "/usr/lib/lxrt-emu/FEX"):
        die("%s is not an x86_64 Android root with FEX (scripts/mkandroidroot.sh --arch x86_64)" % BASE_ROOT)
    tmp = "%s.new-%d" % (ROOT, os.getpid())
    t0 = time.time()
    # cp -c: APFS clones, file by file (the root's sockets are not copied,
    # which is right: they belong to whoever bound them). Its status is not
    # 0 because of those, so the result is checked instead.
    subprocess.run(["cp", "-c", "-R", BASE_ROOT, tmp], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.isfile(tmp + "/system/bin/toybox"):
        shutil.rmtree(tmp, ignore_errors=True)
        die("could not clone %s to %s (cp -c needs both on the same APFS volume)" % (BASE_ROOT, tmp))
    for stale in ("/data/local/tmp/.fexserver.pid",):
        try:
            os.unlink(tmp + stale)
        except OSError:
            pass
    if fex:
        shutil.copyfile(fex, tmp + "/usr/lib/lxrt-emu/FEX")
        os.chmod(tmp + "/usr/lib/lxrt-emu/FEX", 0o755)
    with open(tmp + "/.steamarm-session", "w") as f:
        f.write("# scripts/android-session.py: an APFS clone of %s, %s%s\n"
                % (BASE_ROOT, datetime.datetime.now().isoformat(timespec="seconds"),
                   (", FEX from " + fex) if fex else ""))
    os.rename(tmp, ROOT)
    log("session root %s: a clone of %s (%.1f s)" % (ROOT, BASE_ROOT, time.time() - t0))


# ------------------------------------------------------------------ guests
def guest(s, argv, timeout=120, ids="root"):
    """A guest program in the session. (returncode, stdout)."""
    genv = "LXRT_BINDER_DIR=%s/binder LXRT_PROPERTY_DIR=%s/props LXRT_INPUT_DIR=%s/input LXRT_ANDROID_IDS=%s" % (
        s["dir"], s["dir"], s["dir"], ids)
    env = dict(os.environ, ANDROID_X86_ROOT=s["root"], LXRUN=LXRUN, ANDROID_X86_GENV=genv,
               STEAMARM_NO_SAFEGUARD="1")
    try:
        r = subprocess.run([os.path.join(HERE, "run-android-x86.sh")] + argv, env=env, cwd=REPO,
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           timeout=timeout)
        return r.returncode, r.stdout.decode(errors="replace")
    except subprocess.TimeoutExpired as e:
        return 124, (e.stdout or b"").decode(errors="replace")


# ------------------------------------------------------------------ commands
def start(a, exit_with=0):
    with Lock():
        s = read_session()
        if s and boot_alive(s):
            if s.get("bootCompleted"):
                log("already running (boot pid %d, root %s, log %s)" % (s["bootPid"], s["root"], s.get("log")))
                if not LOGF and s.get("log"):
                    open_log(s["log"])
                return s
            log("a boot is under way (pid %d); waiting for it" % s["bootPid"])
            if not LOGF and s.get("log"):
                open_log(s["log"])
            return wait_boot(s, a.timeout)
        if s:
            leftover(s)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        logpath = os.path.join(LOGS, "android-%s.log" % stamp)
        open_log(logpath)
        log("== session: root %s, host state %s, %s" % (ROOT, RUN_DIR, "headless" if a.headless else "Weston on " + DISPLAY))
        if not os.access(LXRUN, os.X_OK):
            die("no %s (make lxrt)" % LXRUN)
        ensure_volume()
        ensure_root(a.fex)
        # A fresh host state: the binder hub, the property service and the
        # boot's own files are this session's alone.
        if os.path.isdir(RUN_DIR):
            if os.stat(RUN_DIR).st_uid != os.getuid():
                die("%s is not this user's" % RUN_DIR)
            shutil.rmtree(RUN_DIR, ignore_errors=True)
        os.makedirs(RUN_DIR, mode=0o700)
        t0 = time.time()
        # The display: SteamARM's X server on :2 (its windows are macOS
        # windows), then Weston on it -- or headless.
        wenv = weston_env()
        kind = "wlmac" if COMPOSITOR == "wlmac" and not a.headless else "weston"
        state = {"compositor": kind}
        if kind == "weston" and not a.headless:
            r = subprocess.run([os.path.join(HERE, "run-x11-native.sh"), "start", DISPLAY], cwd=REPO,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            if r.returncode != 0:
                die("the X server on %s did not start: %s" % (DISPLAY, r.stdout.decode(errors="replace").strip()))
        if kind == "wlmac":
            # The session ends with its windows, as with Weston's one window:
            # the compositor leaves 5 s after the last app window closed.
            wargs = ["start", "--exit-when-empty"]
        else:
            wargs = ["start", "--kiosk", "--size", a.size] + (["--headless"] if a.headless else [])
        had_weston = weston_pid(state) is not None
        r = subprocess.run([compositor_script(state)] + wargs, env=wenv, cwd=REPO,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        log(r.stdout.decode(errors="replace").strip())
        wpid = weston_pid(state)
        if r.returncode != 0 or not wpid:
            die("the compositor (%s) did not start" % kind)
        # The boot, in a session of its own so that it outlives this command
        # (start returns; stop or run's end stop it).
        cmd = [sys.executable, os.path.join(HERE, "android-boot.py"), "--root", ROOT, "--state", RUN_DIR,
               "--lxrun", LXRUN, "--wayland", "%s:%s" % (XDG, SOCKET), "--seconds", "0",
               "--until-prop", "sys.boot_completed=1", "--keep-running"]
        if not a.headless and os.environ.get("STEAMARM_ANDROID_SOUND") != "0" and start_sound():
            cmd += ["--pulse", "/tmp/pulse"]
        if kind == "wlmac":
            # Waydroid's composer in its multi-window mode: one Wayland
            # toplevel per Android task, titled with the app's name (its
            # hwcomposer's select_mode, read every frame: active_apps "none"
            # shows nothing, "Waydroid" the whole screen in one window, any
            # other value one window per task). No window while it boots:
            # background_start=false would make the composer open its
            # "Waydroid" window and set active_apps=Waydroid itself.
            cmd += ["--prop", "persist.waydroid.multi_windows=true", "--prop", "waydroid.active_apps=none",
                    "--prop", "waydroid.background_start=true"]
        if exit_with:
            cmd += ["--exit-with", str(exit_with)]
        p = subprocess.Popen(cmd, stdin=subprocess.DEVNULL, stdout=LOGF, stderr=subprocess.STDOUT,
                             start_new_session=True, cwd=REPO, env=dict(os.environ, LXRUN=LXRUN))
        s = {"bootPid": p.pid, "root": ROOT, "dir": RUN_DIR, "xdg": XDG, "socket": SOCKET,
             "display": "headless" if a.headless else ("wlmac" if kind == "wlmac" else DISPLAY),
             "size": a.size, "log": logpath, "compositor": kind,
             "westonPid": wpid, "westonWasRunning": had_weston, "started": time.time()}
        write_session(s)
        log("boot started (pid %d); Weston pid %d; waiting for sys.boot_completed=1" % (p.pid, wpid))
    return wait_boot(s, a.timeout, t0)


def wait_boot(s, timeout, t0=None):
    t0 = t0 or s.get("started") or time.time()
    initlog = os.path.join(s["dir"], "init.log")
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with open(initlog, errors="replace") as f:
                text = f.read()
        except OSError:
            text = ""
        m = re.search(r"sys\.boot_completed=1 after ([\d.]+) s", text)
        if m:
            s["bootCompleted"] = time.time()
            s["bootSeconds"] = round(time.time() - t0, 1)
            write_session(s)
            log("sys.boot_completed=1: %.1f s after the session started (the boot's own count: %s s)"
                % (time.time() - t0, m.group(1)))
            trim(s)
            keyboard_layout(s)
            return s
        if "boot failed:" in text or not boot_alive(s):
            tail = "\n".join(text.splitlines()[-5:])
            stop_session(s, "the boot ended before sys.boot_completed=1")
            die("the boot ended before sys.boot_completed=1:\n" + tail)
        time.sleep(1)
    stop_session(s, "no sys.boot_completed=1 after %d s" % timeout)
    die("no sys.boot_completed=1 after %d s (%s)" % (timeout, s.get("log")))


# macOS keyboard layouts (com.apple.keylayout.<name>) and the Android
# layout of the same arrangement (InputDevices' keyboard_layout_<name>).
MAC_TO_ANDROID_LAYOUT = {
    "US": "english_us", "ABC": "english_us", "USInternational-PC": "english_us_intl",
    "British": "english_uk", "British-PC": "english_uk", "Australian": "english_us",
    "Dvorak": "english_us_dvorak", "Colemak": "english_us_colemak",
    "Spanish": "spanish", "Spanish-ISO": "spanish", "LatinAmerican": "spanish_latin",
    "German": "german", "Austrian": "german", "French": "french", "French-PC": "french",
    "French-numerical": "french", "Canadian-CSA": "french_ca", "CanadianFrench-PC": "french_ca",
    "Italian": "italian", "Italian-Pro": "italian", "Portuguese": "portuguese",
    "Brazilian": "brazilian", "Brazilian-ABNT2": "brazilian", "Brazilian-Pro": "brazilian",
    "SwissFrench": "swiss_french", "SwissGerman": "swiss_german", "Belgian": "belgian",
    "Danish": "danish", "Norwegian": "norwegian", "Swedish": "swedish", "Swedish-Pro": "swedish",
    "Finnish": "finnish", "Icelandic": "icelandic", "Estonian": "estonian",
    "Latvian": "latvian_qwerty", "Lithuanian": "lithuanian", "Polish": "polish", "PolishPro": "polish",
    "Czech": "czech", "Czech-QWERTY": "czech", "Slovak": "slovak", "Hungarian": "hungarian",
    "Croatian": "croatian_and_slovenian", "Slovenian": "croatian_and_slovenian",
    "Turkish": "turkish", "Turkish-QWERTY": "turkish", "Turkish-QWERTY-PC": "turkish",
    "Greek": "greek", "Russian": "russian_mac", "Russian-PC": "russian", "Ukrainian": "ukrainian",
    "Ukrainian-PC": "ukrainian", "Bulgarian": "bulgarian", "Hebrew": "hebrew", "Arabic": "arabic",
    "Persian": "persian", "Azeri": "azerbaijani",
}


def keyboard_layout(s):
    """Android's layout for the Mac's keyboard, the Mac's own (the keys
    reach Android by position; KeyboardLayout.java). Windowed sessions only;
    STEAMARM_ANDROID_KEYBOARD=<android layout> chooses one, =none skips."""
    if s.get("display") in (None, "headless"):
        return
    want = os.environ.get("STEAMARM_ANDROID_KEYBOARD")
    if not want:
        r = subprocess.run(["defaults", "read", "com.apple.HIToolbox", "AppleCurrentKeyboardLayoutInputSourceID"],
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
        mac = r.stdout.decode(errors="replace").strip()
        want = MAC_TO_ANDROID_LAYOUT.get(mac.rsplit(".", 1)[-1]) if mac.startswith("com.apple.keylayout.") else None
        if not want:
            log("keyboard: no Android layout for the Mac's %r; Android's default (US)" % (mac or "unknown"))
            return
    if want == "none":
        return
    dex = "/data/local/tmp/steamarm-standin.dex"
    if not os.path.isfile(s["root"] + dex):
        log("keyboard: no stand-in dex in the root")
        return
    rc, out = guest(s, ["/system/bin/env", "CLASSPATH=" + dex, "/system/bin/app_process64", "/system/bin",
                        "org.steamarm.android.KeyboardLayout", "keyboard_layout_" + want], timeout=120)
    log("keyboard: %s" % (out.strip().splitlines()[-1] if out.strip() else "rc %d" % rc))


def trim(s):
    """TRIM_PACKAGES disabled, once per session root."""
    if os.path.exists(s["root"] + TRIM_MARK):
        return
    t0 = time.time()
    script = "; ".join("/system/bin/pm disable-user --user 0 %s >/dev/null 2>&1" % p for p in TRIM_PACKAGES)
    rc, out = guest(s, ["/system/bin/sh", "-c", script + "; /system/bin/cmd package list packages -d"], timeout=600)
    disabled = [l.split(":", 1)[1] for l in out.split() if l.startswith("package:")]
    done = [p for p in TRIM_PACKAGES if p in disabled]
    with open(s["root"] + TRIM_MARK, "w") as f:
        f.write("# scripts/android-session.py TRIM_PACKAGES, disabled with pm disable-user\n%s\n" % "\n".join(done))
    log("disabled %d of the image's apps the session does not need (%.1f s): %s"
        % (len(done), time.time() - t0, ", ".join(done)))


def lxrun_count():
    try:
        out = subprocess.run(["ps", "-Ao", "comm="], stdout=subprocess.PIPE, timeout=10).stdout.decode(errors="replace")
    except (OSError, subprocess.SubprocessError):
        return -1
    return sum(1 for l in out.splitlines() if l.strip().endswith("/lxrun") or l.strip() == "lxrun")


def leftover(s):
    """A session.json whose boot is gone: stop what it may have left."""
    log("the recorded session (boot pid %s) is gone; clearing it" % s.get("bootPid"))
    stop_weston(s)
    try:
        os.unlink(SESSION_FILE)
    except OSError:
        pass


def stop_weston(s):
    r = subprocess.run([compositor_script(s), "stop"], env=weston_env(), cwd=REPO,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    log(r.stdout.decode(errors="replace").strip())


def stop_session(s, why):
    log("stopping: %s" % why)
    pid = s.get("bootPid")
    if boot_alive(s):
        os.kill(pid, signal.SIGTERM)        # android-boot.py stops every process it started
        for _ in range(300):
            if not boot_alive(s):
                break
            time.sleep(0.1)
        if boot_alive(s):
            log("the boot did not stop in 30 s: SIGKILL to its process group")
            try:
                os.killpg(pid, signal.SIGKILL)
            except OSError:
                pass
    stop_weston(s)
    try:
        if (read_session() or {}).get("bootPid") == pid:
            os.unlink(SESSION_FILE)
    except OSError:
        pass
    log("stopped")


def cmd_stop(a):
    with Lock():
        s = read_session()
        if not s:
            log("no session")
            return 0
        if s.get("log"):
            open_log(s["log"])
        stop_session(s, "asked")
    return 0


def cmd_status(a):
    s = read_session()
    alive = bool(s and boot_alive(s))
    out = dict(s or {}, running=alive, westonRunning=bool(weston_pid(s)))
    if alive:
        rc, o = guest(s, ["/system/bin/getprop", "sys.boot_completed"], timeout=60)
        out["sys.boot_completed"] = o.strip()
    print(json.dumps(out, indent=1))
    return 0 if alive else 3


def package_meta(pkg):
    try:
        with open(os.path.join(ADIR, "packages", pkg, "meta.json")) as f:
            m = json.load(f)
        return m if isinstance(m, dict) else {}
    except (OSError, ValueError):
        return {}


def meta_field(m, key):
    """A field of android-pm.py's meta.json, which keeps apk-inspect's report."""
    if key in m:
        return m[key]
    for sub in ("apk", "report", "inspect"):
        if isinstance(m.get(sub), dict) and key in m[sub]:
            return m[sub][key]
    return None


PKG_RE = re.compile(r"[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+")


def installed_version(s, pkg):
    rc, out = guest(s, ["/system/bin/cmd", "package", "list", "packages", "--show-versioncode", pkg], timeout=120)
    for line in out.splitlines():
        m = re.match(r"package:(\S+) versionCode:(\d+)", line.strip())
        if m and m.group(1) == pkg:
            return int(m.group(2))
    return None


def install(s, pkg, apk=None, force=False):
    if not PKG_RE.fullmatch(pkg):
        die("not a package name: %r" % pkg, 2)
    package_dir = os.path.join(ADIR, "packages", pkg)
    apk = apk or os.path.join(package_dir, "base.apk")
    if not os.path.isfile(apk):
        die("no APK for %s at %s (scripts/android-pm.py install)" % (pkg, apk), 5)
    meta = package_meta(pkg)
    same_base = os.path.abspath(apk) == os.path.abspath(os.path.join(package_dir, "base.apk"))
    split_names = (meta.get("splits") or []) if same_base else []
    obb_names = (meta.get("obb") or []) if same_base else []
    apks = [apk]
    for name in split_names:
        if not isinstance(name, str) or not re.fullmatch(r"split_[A-Za-z0-9_.-]+\.apk", name):
            die("invalid stored split APK name for %s: %r" % (pkg, name), 5)
        path = os.path.join(package_dir, name)
        if not os.path.isfile(path):
            die("missing split APK for %s: %s" % (pkg, path), 5)
        apks.append(path)
    # OBB data belongs to the session root, independent of whether pm
    # already has the current version of the package.
    for name in obb_names:
        if not isinstance(name, str) or not re.fullmatch(r"obb/[^/]+\.obb", name):
            die("invalid stored OBB name for %s: %r" % (pkg, name), 5)
        source = os.path.join(package_dir, name)
        if not os.path.isfile(source):
            die("missing OBB for %s: %s" % (pkg, source), 5)
        target_dir = os.path.join(s["root"], "data/media/0/Android/obb", pkg)
        os.makedirs(target_dir, exist_ok=True)
        target = os.path.join(target_dir, os.path.basename(name))
        shutil.copyfile(source, target)
        log("OBB copied for %s: %s" % (pkg, target))
    want = meta_field(meta, "versionCode")
    have = installed_version(s, pkg)
    digest = meta_field(meta, "sha256") if same_base else None
    marker = os.path.join(s["root"], "data/local/tmp/steamarm-installed-%s.sha256" % pkg)
    try:
        installed_digest = open(marker, encoding="ascii").read().strip()
    except OSError:
        installed_digest = None
    if have is not None and not force and (want is None or have == want) and (not digest or installed_digest == digest):
        log("%s is installed (versionCode %s)" % (pkg, have))
        return True
    staged = ["/data/local/tmp/steamarm-install-%s-%d.apk" % (pkg, i) for i in range(len(apks))]
    for source, dest in zip(apks, staged):
        shutil.copyfile(source, s["root"] + dest)
    t0 = time.time()
    log("pm install %s (%d APKs: %s; installed now: %s)" % (pkg, len(apks), ", ".join(apks), have))
    try:
        rc, out = guest(s, ["/system/bin/pm", "install", "-r"] + staged, timeout=900)
        if (rc or not out.strip().endswith("Success")) and len(staged) > 1:
            # Android 11's pm accepts several paths in one install session;
            # explicit sessions cover images whose pm wrapper rejects them.
            log("pm install with several APKs failed: %s; using install-create/write/commit"
                % (out.strip().splitlines()[-1] if out.strip() else "rc %d" % rc))
            total = sum(os.path.getsize(source) for source in apks)
            rc, created = guest(s, ["/system/bin/pm", "install-create", "-r", "-S", str(total)], timeout=120)
            match = re.search(r"\[(\d+)\]", created)
            if rc or not match:
                out = created
            else:
                session = match.group(1)
                for i, (source, dest) in enumerate(zip(apks, staged)):
                    rc, out = guest(s, ["/system/bin/pm", "install-write", "-S", str(os.path.getsize(source)),
                                        session, "base" if i == 0 else "split%d" % i, dest], timeout=900)
                    if rc or not out.strip().startswith("Success"):
                        guest(s, ["/system/bin/pm", "install-abandon", session], timeout=120)
                        break
                else:
                    rc, out = guest(s, ["/system/bin/pm", "install-commit", session], timeout=900)
                    if rc or not out.strip().endswith("Success"):
                        guest(s, ["/system/bin/pm", "install-abandon", session], timeout=120)
    finally:
        for dest in staged:
            try:
                os.unlink(s["root"] + dest)
            except OSError:
                pass
    out = out.strip()
    log("pm install: %s (%.1f s)" % (out.splitlines()[-1] if out else "rc %d" % rc, time.time() - t0))
    success = rc == 0 and out.endswith("Success")
    if success and digest:
        with open(marker, "w", encoding="ascii") as f:
            f.write(digest + "\n")
    return success


def launcher_activity(s, pkg):
    act = meta_field(package_meta(pkg), "launcherActivity")
    if isinstance(act, str) and act:
        return act
    rc, out = guest(s, ["/system/bin/cmd", "package", "resolve-activity", "--brief", "-c",
                        "android.intent.category.LAUNCHER", pkg], timeout=120)
    lines = [l.strip() for l in out.splitlines() if "/" in l]
    if lines:
        comp = lines[-1]
        p, _, c = comp.partition("/")
        return p + c if c.startswith(".") else c
    return None


def focused(s):
    rc, out = guest(s, ["/system/bin/dumpsys", "window"], timeout=120)
    m = re.search(r"mCurrentFocus=Window\{[^}]*\s(\S+)\}", out)
    return m.group(1) if m else ""


def name_window(s, title):
    """The session's macOS window takes the app's name: it is Weston's X
    window, titled "Weston Compositor - screen0" by Weston's X11 backend,
    and quartz-wm shows WM_NAME / _NET_WM_NAME in the title bar and the
    Window menu. Nothing to do headless."""
    if s.get("display") in (None, "headless", "wlmac"):
        return          # steamarm-wlmac titles each window with the app's own title
    try:
        import ctypes
        X = ctypes.CDLL("/opt/homebrew/lib/libX11.dylib")
    except OSError:
        return
    X.XOpenDisplay.restype = ctypes.c_void_p
    X.XOpenDisplay.argtypes = [ctypes.c_char_p]
    X.XInternAtom.restype = ctypes.c_ulong
    X.XInternAtom.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int]
    X.XStoreName.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_char_p]
    X.XChangeProperty.argtypes = [ctypes.c_void_p, ctypes.c_ulong, ctypes.c_ulong, ctypes.c_ulong,
                                  ctypes.c_int, ctypes.c_int, ctypes.c_char_p, ctypes.c_int]
    X.XFlush.argtypes = [ctypes.c_void_p]
    X.XCloseDisplay.argtypes = [ctypes.c_void_p]
    r = subprocess.run(["/opt/homebrew/bin/xwininfo", "-root", "-tree"], env=dict(os.environ, DISPLAY=s["display"]),
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    m = re.search(r"(0x[0-9a-f]+) \"Weston Compositor[^\"]*\"", r.stdout.decode(errors="replace"))
    d = X.XOpenDisplay(s["display"].encode())
    if not m or not d:
        return
    win, name = int(m.group(1), 16), title.encode("utf-8")
    X.XStoreName(d, win, name)
    utf8 = X.XInternAtom(d, b"UTF8_STRING", 0)
    X.XChangeProperty(d, win, X.XInternAtom(d, b"_NET_WM_NAME", 0), utf8, 8, 0, name, len(name))
    X.XFlush(d)
    X.XCloseDisplay(d)
    log("window: %s" % title)


def launch(s, pkg, apk=None, force=False):
    if not install(s, pkg, apk, force):
        die("%s was not installed" % pkg, 4)
    act = launcher_activity(s, pkg)
    if not act:
        die("%s has no launcher activity" % pkg, 4)
    if s.get("compositor") == "wlmac":
        # Windows from now on, one per task (see start).
        guest(s, ["/system/bin/setprop", "waydroid.active_apps", pkg], timeout=60)
    t0 = time.time()
    rc, out = guest(s, ["/system/bin/am", "start", "-W", "-n", "%s/%s" % (pkg, act)], timeout=300)
    status = re.search(r"Status: (\S+)", out)
    total = re.search(r"TotalTime: (\d+)", out)
    log("am start -W -n %s/%s: %s%s (%.1f s)" % (pkg, act, status.group(1) if status else "no status",
                                                ", TotalTime %s ms" % total.group(1) if total else "",
                                                time.time() - t0))
    for n in range(60):
        f = focused(s)
        if n == 15 and not f.startswith(pkg + "/"):
            # Once in a while the focus stays on the home screen after a
            # successful am start (MEASURED: tests/android/run.sh, stage 29);
            # asking again brings the app's task to the front.
            log("no focus after 30 s (focus: %s): am start again" % (f or "none"))
            guest(s, ["/system/bin/am", "start", "-n", "%s/%s" % (pkg, act)], timeout=120)
        if f.startswith(pkg + "/"):
            log("focused window: %s (%.1f s after am start; %d lxrun processes on the Mac)"
                % (f, time.time() - t0, lxrun_count()))
            s["app"] = pkg
            write_session(s)
            name_window(s, meta_field(package_meta(pkg), "label") or pkg)
            return True
        time.sleep(2)
    log("its window never had the focus (last focus: %s)" % (focused(s) or "none"))
    return False


def cmd_install(a):
    s = read_session()
    if not s or not boot_alive(s) or not s.get("bootCompleted"):
        die("no session (scripts/android-session.py start)", 3)
    if s.get("log"):
        open_log(s["log"])
    return 0 if install(s, a.package, a.apk, a.force) else 4


def cmd_launch(a):
    s = start(a)
    return 0 if launch(s, a.package, a.apk, a.force) else 1


def cmd_run(a):
    stop_now = []

    def on_signal(sig, frame):
        stop_now.append(sig)
    for sig in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(sig, on_signal)
    s = start(a, exit_with=os.getpid())
    ok = launch(s, a.package, a.apk, a.force)
    if not ok:
        stop_session(s, "the app did not start")
        return 1
    log("running; the session ends with Detener (SIGTERM), when Weston's window is closed, or with the boot")
    while not stop_now:
        if not boot_alive(s):
            log("the boot is gone")
            stop_session(s, "the boot ended")
            return 1
        if s.get("westonPid"):
            try:
                os.kill(s["westonPid"], 0)
            except OSError:
                stop_session(s, "Weston is gone (its window was closed)")
                return 0
        time.sleep(1)
    with Lock():
        stop_session(s, "signal %d" % stop_now[0])
    return 0


def cmd_shell(a):
    s = read_session()
    if not s or not boot_alive(s):
        die("no session (scripts/android-session.py start)", 3)
    rc, out = guest(s, a.argv, timeout=a.timeout)
    sys.stdout.write(out)
    return rc


def cmd_xwd(a):
    s = read_session()
    if not s or s.get("display") in (None, "headless"):
        die("no session with a window (start without --headless)", 3)
    r = subprocess.run(["/opt/homebrew/bin/xwininfo", "-root", "-tree"], env=dict(os.environ, DISPLAY=DISPLAY),
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    m = re.search(r"(0x[0-9a-f]+) \"Weston Compositor[^\"]*\"", r.stdout.decode(errors="replace"))
    if not m:
        die("no Weston window on %s" % DISPLAY)
    out = os.path.abspath(a.out)
    wroot = os.environ.get("WESTON_ROOT") or os.path.join(STATE, "westonroot")
    guest_out = "/tmp/android-session-%d.xwd" % os.getpid()
    env = dict(os.environ, WESTON_XDG=XDG, WESTON_SOCKET=SOCKET, LXRUN=LXRUN, DISPLAY=DISPLAY)
    r = subprocess.run([os.path.join(HERE, "run-weston.sh"), "client", "/usr/bin/xwd", "-silent", "-id", m.group(1),
                        "-out", guest_out], env=env, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    host = wroot + guest_out
    if r.returncode != 0 or not os.path.isfile(host):
        die("xwd failed: %s" % r.stderr.decode(errors="replace")[-300:])
    shutil.move(host, out)
    print("%s: window %s" % (out, m.group(1)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    def start_opts(p):
        p.add_argument("--headless", action="store_true", help="Weston with no window (tests)")
        p.add_argument("--size", default=os.environ.get("ANDROID_SESSION_SIZE", "1024x768"),
                       help="Weston's output, which is Android's screen (WxH)")
        p.add_argument("--timeout", type=int, default=900, help="seconds to wait for sys.boot_completed=1")
        p.add_argument("--fex", default=None, help="a FEX-emu to put in the session root when it is made")
    p = sub.add_parser("start")
    start_opts(p)
    sub.add_parser("stop")
    sub.add_parser("status")
    for name in ("install", "launch", "run"):
        p = sub.add_parser(name)
        p.add_argument("package")
        p.add_argument("--apk", default=None, help="this APK instead of the library's copy")
        p.add_argument("--force", action="store_true", help="install even if that version is installed")
        if name != "install":
            start_opts(p)
    p = sub.add_parser("shell")
    p.add_argument("--timeout", type=int, default=300)
    p.add_argument("argv", nargs=argparse.REMAINDER)
    p = sub.add_parser("xwd")
    p.add_argument("out")
    a = ap.parse_args()
    if getattr(a, "size", None) and not re.fullmatch(r"\d{3,4}x\d{3,4}", a.size):
        die("--size WxH", 2)
    if a.cmd == "start":
        start(a)
        return 0
    return {"stop": cmd_stop, "status": cmd_status, "install": cmd_install, "launch": cmd_launch,
            "run": cmd_run, "shell": cmd_shell, "xwd": cmd_xwd}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
