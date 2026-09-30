#!/usr/bin/env python3
"""A minimal init for an Android root under lxrun: no VM (AGENTS.md).

Android's init is PID 1 in a mount namespace with SELinux, cgroups and
device nodes; none of that exists for an lxrun guest, so this program does
the part of init's job that a headless framework needs, and nothing more
(docs/ANDROID_RUNTIME_ARCHITECTURE.md, "Boot"):

  1. the property service (runtime/propsvc.c) for the root, told to hand
     ctl.* messages and property changes to this program (LXRT_PROPERTY_INIT);
  2. the image's .rc files, parsed as init parses them: /system/etc/init/hw/
     init.rc with its imports, then /system/etc/init, /system_ext/etc/init,
     /product/etc/init, /odm/etc/init, /vendor/etc/init ("override" honoured);
  3. init's boot triggers in init's order (early-init, init, late-init and
     the triggers it queues, then the property triggers, and "on property:"
     triggers as properties change). Of the actions only
     setprop, mkdir/write/symlink/copy under /data, /mnt and /storage,
     export, trigger,
     mount_all (as Waydroid's init does it: /data is not encrypted),
     start/stop/restart, class_start/class_stop and enable run; the rest
     (chmod, chown, mount, restorecon, exec ...) are counted and skipped;
  4. services: only those in the PROFILE below are started when an action or
     a ctl.start asks for them (Lepton does the same by disabling what games
     do not need, docs/LEPTON_REUSE_ANALYSIS.md 0.3). A service runs with the
     .rc file's command line, its sockets (bound here, passed as
     ANDROID_SOCKET_<name>), init.environ.rc's environment plus setenv, and
     its user, groups and capabilities as Android ids (runtime/android_ids.h,
     LXRT_ANDROID_IDS);
  5. restarts as init does them: a service that exits is started again after
     5 s unless oneshot, "onrestart" restart/setprop run, a critical service
     that exits more than 4 times in 4 minutes ends the boot (init would
     reboot into the bootloader), init.svc.<name> follows the state;
  6. ctl.start/stop/restart and ctl.interface_* from any guest, handed over by
     the property service, act on the services as init's would.

Before the first action it sets HOST_PROPS, as Waydroid's and Lepton's hosts
set theirs. On an x86_64 root every service is an x86-64 (or i386) program
under FEX (scripts/run-android-x86.sh's environment and FEXServer).

Logs: <state>/init.log (this program), <state>/logcat.txt (tests/android/
logd.py, the stand-in for logd's write socket), <state>/svc/<name>.log (a
service's stdout and stderr).

  scripts/android-boot.py [--root R] [--state DIR] [--seconds N]
                          [--until-prop NAME=VALUE] [--keep-running]
                          [--profile NAME] [--also SVC] [--all] [--no-zygote]
                          [--prop NAME=VALUE] [--svc-env SVC:VAR=VALUE]
                          [--trace SVC] [--persist FILE] [--bootargs ARGS]

Stops, at the end, every process it started and every process those left in
the root's state (found by the state directory in their environment), and
nothing else.
"""
import argparse
import os
import re
import shlex
import signal
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# Android ids, from system/core/libcutils/include/private/
# android_filesystem_config.h (Android 11). All 96 agree with bionic's own
# getpwnam in the x86_64 image (MEASURED, benchmarks/stage27-android-
# framework.txt).
AID = {
    "root": 0, "system": 1000, "radio": 1001, "bluetooth": 1002, "graphics": 1003,
    "input": 1004, "audio": 1005, "camera": 1006, "log": 1007, "compass": 1008,
    "mount": 1009, "wifi": 1010, "adb": 1011, "install": 1012, "media": 1013,
    "dhcp": 1014, "sdcard_rw": 1015, "vpn": 1016, "keystore": 1017, "usb": 1018,
    "drm": 1019, "mdnsr": 1020, "gps": 1021, "media_rw": 1023, "mtp": 1024,
    "drmrpc": 1026, "nfc": 1027, "sdcard_r": 1028, "clat": 1029, "loop_radio": 1030,
    "mediadrm": 1031, "package_info": 1032, "sdcard_pics": 1033, "sdcard_av": 1034,
    "sdcard_all": 1035, "logd": 1036, "shared_relro": 1037, "dbus": 1038,
    "tlsdate": 1039, "mediaex": 1040, "audioserver": 1041, "metrics_coll": 1042,
    "metricsd": 1043, "webserv": 1044, "debuggerd": 1045, "mediacodec": 1046,
    "cameraserver": 1047, "firewall": 1048, "trunks": 1049, "nvram": 1050,
    "dns": 1051, "dns_tether": 1052, "webview_zygote": 1053, "vehicle_network": 1054,
    "media_audio": 1055, "media_video": 1056, "media_image": 1057, "tombstoned": 1058,
    "media_obb": 1059, "ese": 1060, "ota_update": 1061, "automotive_evs": 1062,
    "lowpan": 1063, "hsm": 1064, "reserved_disk": 1065, "statsd": 1066,
    "incidentd": 1067, "secure_element": 1068, "lmkd": 1069, "llkd": 1070,
    "iorapd": 1071, "gpu_service": 1072, "network_stack": 1073, "gsid": 1074,
    "fsverity_cert": 1075, "credstore": 1076, "external_storage": 1077,
    "ext_data_rw": 1078, "ext_obb_rw": 1079, "context_hub": 1080,
    "shell": 2000, "cache": 2001, "diag": 2002,
    "net_bt_admin": 3001, "net_bt": 3002, "inet": 3003, "net_raw": 3004,
    "net_admin": 3005, "net_bw_stats": 3006, "net_bw_acct": 3007,
    "readproc": 3009, "wakelock": 3010, "uhid": 3011,
    "everybody": 9997, "misc": 9998, "nobody": 9999,
}
# include/uapi/linux/capability.h
CAP = {n: i for i, n in enumerate((
    "CHOWN DAC_OVERRIDE DAC_READ_SEARCH FOWNER FSETID KILL SETGID SETUID SETPCAP "
    "LINUX_IMMUTABLE NET_BIND_SERVICE NET_BROADCAST NET_ADMIN NET_RAW IPC_LOCK IPC_OWNER "
    "SYS_MODULE SYS_RAWIO SYS_CHROOT SYS_PTRACE SYS_PACCT SYS_ADMIN SYS_BOOT SYS_NICE "
    "SYS_RESOURCE SYS_TIME SYS_TTY_CONFIG MKNOD LEASE AUDIT_WRITE AUDIT_CONTROL SETFCAP "
    "MAC_OVERRIDE MAC_ADMIN SYSLOG WAKE_ALARM BLOCK_SUSPEND AUDIT_READ PERFMON BPF "
    "CHECKPOINT_RESTORE").split())}
CAP_ALL = (1 << 41) - 1

# What a headless framework is given. Services named here start when the
# .rc files' actions (or a ctl.start) ask for them; everything else is
# logged as "not in the profile". Each entry says why it is needed or why a
# service that init would start is left out (MEASURED in
# benchmarks/stage27-android-framework.txt unless marked).
PROFILES = {
    "headless": {
        "start": {
            # binder context managers (post-fs in init.rc)
            "servicemanager", "hwservicemanager", "vndservicemanager",
            # HIDL HALs system_server or its native services ask for at boot
            "hidl_memory",                         # android.hidl.allocator@1.0 (ashmem allocator)
            "vendor.configstore-hal",              # ISurfaceFlingerConfigs
            "vendor.health-hal-2-0",               # BatteryService: IHealth
            "system_suspend",                      # PowerManagerService: suspend_control
            "vendor.power-hal-1-0",                # PowerManagerService
            "vendor.light-hal-2-0",                # LightsService
            "vendor.memtrack-hal-1-0",             # ActivityManager memory stats
            "vendor.gralloc-2-0",                  # IAllocator (graphics buffers)
            "vendor.keymaster-4-0",                # keystore
            "vendor.gatekeeper-1-0",
            "vendor.sensors-hal-1-0",              # SensorService
            "vendor.vibrator-1-0",
            # native daemons
            "keystore", "gatekeeperd", "installd", "credstore", "idmap2d",
            "audioserver", "surfaceflinger",
            # the framework
            "zygote",
        },
        # Extra environment for a service (x86_64 roots): FEX emulates seccomp
        # only when asked, and the zygote's children install Android's
        # seccomp policy and abort if they cannot ("Could not set seccomp
        # filter of size 238: Invalid argument", MEASURED). Inherited by
        # system_server and every app the zygote forks.
        "env_x86": {"zygote": {"FEX_NEEDSSECCOMP": "1"}},
        # Asked for by the .rc files but left out, and why (the boot logs
        # each one it skips).
        "left_out": {
            "ueventd": "no uevents and no device nodes to make (ro.cold_boot_done=true)",
            "logd": "tests/android/logd.py stands in for its write socket",
            "lmkd": "no memory cgroups or PSI; ActivityManager keeps working without it",
            "vold": "no block devices, no FUSE: /data is a directory of the root",
            "apexd": "flattened APEXes (ro.apex.updatable unset), already under /apex",
            "netd": "exits at once: NETLINK_KOBJECT_UEVENT, route netlink, iptables, BPF; its "
                    "'onrestart restart zygote' then kills the zygote every 5 s",
            "vendor.audio-hal": "i386 (32-bit) binary: FEX stops it at once ('NoExec instruction in "
                                "entry block'); audioserver waits for it",
            "vendor.audio-hal-2-0": "declares the same interface; no such binary in this image",
            "vendor.hwcomposer-2-1": "Waydroid's composer is a Wayland client: no display here",
            "zygote_secondary": "app_process32 (i386): 32-bit apps only",
            "bootanim": "graphics",
            "statsd": "cannot link: the root's /linkerconfig (written by the image's linkerconfig "
                      "before properties existed) has no namespace for the statsd APEX",
        },
    },
}


# Properties the host sets before init's first action, as Waydroid's host
# does with waydroid.prop and Lepton's with lepton.prop
# (docs/LEPTON_REUSE_ANALYSIS.md 0.3; compat_tool/liblepton/properties.sh).
# Each is a real AOSP or Waydroid property with its documented meaning; none
# changes what the build says it is.
HOST_PROPS = [
    # No BPF here, so no bpfloader: netd waits for this forever otherwise
    # ("LibBpfLoader: Waited 5s for bpf.progs_loaded, still waiting...",
    # MEASURED). Lepton sets the same.
    ("bpf.progs_loaded", "1"),
    # No ueventd, no cold boot to wait for (Lepton, Waydroid 0013).
    ("ro.cold_boot_done", "true"),
    # No /dev/ashmem: libcutils uses memfd (Lepton; Waydroid's init lets it be
    # set before boot, base-patches-30/system/core/0009).
    ("sys.use_memfd", "true"),
    # Software rendering, as Waydroid configures it without a GPU:
    # SwiftShader's EGL/GLES and the default gralloc. Without it libEGL mixed
    # SwiftShader's EGL with Mesa's GLESv2 and SurfaceFlinger crashed
    # (MEASURED).
    ("ro.hardware.egl", "swiftshader"),
    ("ro.hardware.gralloc", "default"),
    # installd runs dex2oat32 unless this is set (dexopt.cpp
    # select_execution_binary); i386 programs do not run under FEX here yet
    # (dex2oat32 died of SIGSEGV in FEX's 32-bit mode, MEASURED), x86-64
    # dex2oat64 does (benchmarks/stage25-art-x86-fex.txt).
    ("dalvik.vm.dex2oat64.enabled", "true"),
]


def log(msg):
    t = time.strftime("%H:%M:%S") + ".%03d" % (int(time.time() * 1000) % 1000)
    line = "%s %s" % (t, msg)
    print(line, flush=True)
    if LOGF:
        LOGF.write(line + "\n")
        LOGF.flush()


LOGF = None


def own_group():
    # init's Service::Start: setpgid(0, getpid()) in the child, no new
    # session. zygote's first act is Os.setpgid(0, 0), which a session
    # leader is refused (EPERM): with setsid() here it threw "Failed to
    # setpgid(0,0)" before preloading anything (MEASURED).
    os.setpgid(0, 0)


# ------------------------------------------------------------------ .rc files
class Service:
    def __init__(self, name, argv, src):
        self.name, self.argv, self.src = name, argv, src
        self.classes = ["default"]
        self.user = "root"
        self.groups = []
        self.caps = None
        self.sockets = []
        self.env = {}
        self.oneshot = self.disabled = self.critical = False
        self.onrestart = []
        self.interfaces = []
        self.proc = None
        self.state = "stopped"
        self.crashes = []
        self.restart_at = None
        self.starts = 0
        self.exec_waiter = False


class Action:
    def __init__(self, event, props, src):
        self.event, self.props, self.src = event, props, src
        self.cmds = []


def rc_lines(text):
    buf = ""
    for raw in text.split("\n"):
        line = raw.rstrip()
        if line.endswith("\\"):
            buf += line[:-1] + " "
            continue
        line = buf + line
        buf = ""
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        try:
            yield shlex.split(s, comments=False)
        except ValueError:
            yield s.split()


class Rc:
    def __init__(self, root, props):
        self.root, self.props = root, props
        self.services = {}
        self.order = []
        self.actions = []
        self.parsed = set()
        self.unknown_opts = {}

    def expand(self, s):
        return re.sub(r"\$\{([^}:]+)(?::-([^}]*))?\}",
                      lambda m: self.props.get(m.group(1)) or (m.group(2) or ""), s)

    def parse_file(self, gpath):
        hp = self.root + gpath
        if gpath in self.parsed or not os.path.isfile(hp):
            return
        self.parsed.add(gpath)
        imports = []
        cur = None
        for tok in rc_lines(open(hp, errors="replace").read()):
            kw = tok[0]
            if kw == "import":
                imports.append(self.expand(tok[1]))
                cur = None
            elif kw == "on":
                event, props = None, []
                for t in " ".join(tok[1:]).split("&&"):
                    t = t.strip()
                    if t.startswith("property:"):
                        k, _, v = t[9:].partition("=")
                        props.append((k, v))
                    else:
                        event = t
                cur = Action(event, props, gpath)
                self.actions.append(cur)
            elif kw == "service":
                name = tok[1]
                svc = Service(name, tok[2:], gpath)
                override = False
                cur = svc
                # options follow; "override" may be among them: decide at the end
                svc._pending = True
                self.services.setdefault("__pending__", []).append(svc)
            elif isinstance(cur, Action):
                cur.cmds.append(tok)
            elif isinstance(cur, Service):
                self.service_option(cur, tok)
        for svc in self.services.pop("__pending__", []):
            if svc.name in self.services and not getattr(svc, "override", False):
                log("rc: %s: ignoring duplicate service '%s' (first in %s)" %
                    (svc.src, svc.name, self.services[svc.name].src))
                continue
            if svc.name not in self.services:
                self.order.append(svc.name)
            self.services[svc.name] = svc
        for imp in imports:
            if os.path.isdir(self.root + imp):
                self.parse_dir(imp)
            else:
                self.parse_file(imp)

    def parse_dir(self, gdir):
        hd = self.root + gdir
        if not os.path.isdir(hd):
            return
        for f in sorted(os.listdir(hd)):
            if f.endswith(".rc"):
                self.parse_file(gdir.rstrip("/") + "/" + f)

    def service_option(self, svc, tok):
        o = tok[0]
        a = tok[1:]
        if o == "class":
            svc.classes = a
        elif o == "user":
            svc.user = a[0]
        elif o == "group":
            svc.groups = a
        elif o == "capabilities":
            svc.caps = [c.upper() for c in a]
        elif o == "socket":
            svc.sockets.append(a)
        elif o == "setenv":
            svc.env[a[0]] = a[1] if len(a) > 1 else ""
        elif o == "oneshot":
            svc.oneshot = True
        elif o == "disabled":
            svc.disabled = True
        elif o == "critical":
            svc.critical = True
        elif o == "onrestart":
            svc.onrestart.append(a)
        elif o == "interface":
            svc.interfaces.append(" ".join(a))
        elif o == "override":
            svc.override = True
        else:
            self.unknown_opts[o] = self.unknown_opts.get(o, 0) + 1


# ------------------------------------------------------------------ properties
class Props(dict):
    """The root's properties as this init knows them: a dump at start, then
    every change the property service reports (LXRT_PROPERTY_INIT)."""

    def __init__(self, boot):
        super().__init__()
        self.boot = boot

    def set(self, name, value):
        """setprop through the property service, as a guest would
        (PROP_MSG_SETPROP2, bionic's system_property_set.cpp)."""
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        try:
            s.connect(os.path.join(self.boot.propdir, "property_service"))
            n, v = name.encode(), value.encode()
            s.sendall(struct.pack("<II", 0x00020001, len(n)) + n + struct.pack("<I", len(v)) + v)
            r = s.recv(4)
            rc = struct.unpack("<I", r)[0] if len(r) == 4 else -1
        except OSError as e:
            log("setprop %s=%s: %s" % (name, value, e))
            return False
        finally:
            s.close()
        if rc != 0:
            log("setprop %s=%s refused (0x%x)" % (name, value, rc))
            return False
        self[name] = value
        return True


# ------------------------------------------------------------------ the init
class Boot:
    def __init__(self, a):
        self.a = a
        self.root = os.path.realpath(a.root)
        self.state = a.state
        self.propdir = os.path.join(self.state, "props")
        self.binderdir = os.path.join(self.state, "binder")
        self.lxrun = os.path.realpath(a.lxrun)
        self.profile = PROFILES[a.profile]
        self.props = Props(self)
        self.children = []            # (Popen, what)
        self.env = {}
        self.skipped = {}
        self.not_started = {}
        self.queue = []
        self.class_started = set()
        self.x86 = self.detect_x86()
        self.t0 = time.time()

    def detect_x86(self):
        with open(self.root + "/system/bin/toybox", "rb") as f:
            h = f.read(20)
        return struct.unpack_from("<H", h, 18)[0] == 62      # EM_X86_64

    # -- processes
    def base_env(self):
        e = {
            "LXRT_ROOT": self.root, "TMPDIR": "/tmp", "HOME": "/data/local/tmp",
            "OBJC_DISABLE_INITIALIZE_FORK_SAFETY": "YES",
            "LXRT_PROPERTY_DIR": self.propdir, "LXRT_BINDER_DIR": self.binderdir,
            "LXRT_BINDER_HUB_IDLE": "30",
        }
        if self.x86:
            # The same as scripts/run-android-x86.sh (its FEXServer is used).
            sock = "steamarm-android-%s.FEXServer.Socket" % self.fex_hash()
            e.update({
                "XDG_RUNTIME_DIR": "/data/local/tmp", "FEX_ROOTFS": "/",
                "FEX_SERVERSOCKETPATH": sock, "FEX_GUESTBASE": "1",
                "FEX_LOWWINDOW": os.environ.get("FEX_LOWWINDOW", "1"),
                "FEX_SMCCHECKS": os.environ.get("FEX_SMCCHECKS", "mtrack"),
                "FEX_SILENTLOG": "1", "FEX_OUTPUTLOG": "stderr",
            })
        else:
            e["LXRT_GUEST_PAGE"] = "4096"
        return e

    def fex_hash(self):
        import hashlib
        return hashlib.sha1(self.root.encode()).hexdigest()[:12]

    def spawn(self, argv, env, out, pass_fds=(), what=""):
        p = subprocess.Popen([self.lxrun] + argv, env=env, stdin=subprocess.DEVNULL,
                             stdout=out, stderr=subprocess.STDOUT, pass_fds=pass_fds,
                             preexec_fn=own_group, cwd="/")
        self.children.append((p, what))
        return p

    def guest(self, argv, timeout=60, env_extra=None):
        env = dict(self.base_env(), **self.env)
        env.update(env_extra or {})
        r = subprocess.run([self.lxrun] + argv, env=env, stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout)
        return r.returncode, r.stdout.decode(errors="replace")

    # -- start-up
    def start_fexserver(self):
        if not self.x86:
            return
        # scripts/run-android-x86.sh starts the root's own FEXServer (and
        # writes /linkerconfig once); a no-op program is enough.
        env = dict(os.environ, ANDROID_X86_ROOT=self.root, LXRUN=self.lxrun)
        subprocess.run([os.path.join(HERE, "run-android-x86.sh"), "/system/bin/toybox", "true"],
                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)

    def start_propsvc(self):
        os.makedirs(self.propdir, mode=0o700, exist_ok=True)
        os.chmod(self.propdir, 0o700)
        self.ctl = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        cp = os.path.join(self.state, "init.sock")
        try:
            os.unlink(cp)
        except FileNotFoundError:
            pass
        self.ctl.bind(cp)
        self.ctl.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 << 20)
        self.ctl.setblocking(False)
        env = dict(self.base_env(), LXRT_PROPERTY_IDLE="0", LXRT_PROPERTY_INIT=cp)
        if self.a.bootargs:
            env["LXRT_PROPERTY_BOOTARGS"] = self.a.bootargs
        if self.a.persist:
            env["LXRT_PROPERTY_PERSIST"] = self.a.persist
        out = open(os.path.join(self.state, "propsvc.out"), "a")
        self.propsvc = self.spawn(["--property-service", self.propdir, self.root], env, out,
                                  what="property service")
        sp = os.path.join(self.propdir, "property_service")
        for _ in range(200):
            if os.path.exists(sp) and os.path.exists(os.path.join(self.propdir, "service.pid")):
                break
            if self.propsvc.poll() is not None:
                raise SystemExit("property service exited (%s)" % os.path.join(self.propdir, "service.log"))
            time.sleep(0.05)
        rc, out = self.guest(["/system/bin/getprop"])
        for line in out.splitlines():
            m = re.match(r"^\[(.*)\]: \[(.*)\]$", line)
            if m:
                self.props[m.group(1)] = m.group(2)
        log("property service up: %d properties" % len(self.props))
        for k, v in HOST_PROPS + [tuple(x.split("=", 1)) for x in (self.a.prop or [])]:
            if self.props.get(k) != v:
                self.props.set(k, v)
        log("host properties: %s" % ", ".join("%s=%s" % (k, self.props.get(k, "")) for k, _ in HOST_PROPS))

    def start_logd(self):
        out = open(os.path.join(self.state, "logd.out"), "a")
        p = subprocess.Popen([sys.executable, os.path.join(REPO, "tests/android/logd.py"), self.root,
                              "--out", os.path.join(self.state, "logcat.txt")],
                             stdout=out, stderr=subprocess.STDOUT, start_new_session=True)
        self.children.append((p, "logd stand-in"))
        for _ in range(100):
            if os.path.exists(self.root + "/dev/socket/logdw"):
                break
            time.sleep(0.05)
        self.props.set("init.svc.logd", "running")
        log("logd stand-in on /dev/socket/logdw -> %s" % os.path.join(self.state, "logcat.txt"))

    # -- identities
    def uid_of(self, name):
        if name == "host":            # Waydroid's init: waydroid.host.uid, default 1000
            return int(self.props.get("waydroid.host.uid") or 1000)
        if name in AID:
            return AID[name]
        if name.isdigit():
            return int(name)
        m = re.match(r"^[a-z_]+_(\d+)$", name)
        return int(m.group(1)) if m else None

    def ids_for(self, svc):
        uid = self.uid_of(svc.user)
        gids = [self.uid_of(g) for g in svc.groups]
        if uid is None or None in gids:
            log("%s: unknown user or group (%s %s); runs as root" % (svc.name, svc.user, svc.groups))
            return "root"
        gid = gids[0] if gids else uid
        sup = ",".join(str(g) for g in gids[1:])
        if svc.caps is not None:
            m = 0
            for c in svc.caps:
                if c in CAP:
                    m |= 1 << CAP[c]
            # init: SetCapsForExec (bounding = inheritable = ambient = the
            # list), then setuid with keepcaps; after exec permitted =
            # effective = ambient.
            return "%d:%d:%s:%x,%x,%x,%x,%x" % (uid, gid, sup, m, m, m, m, m)
        if uid == 0:
            return "0:%d:%s:%x,%x,0,%x,0" % (gid, sup, CAP_ALL, CAP_ALL, CAP_ALL)
        return "%d:%d:%s" % (uid, gid, sup)

    # -- services
    def make_sockets(self, svc):
        fds, env = [], {}
        for spec in svc.sockets:
            name, typ = spec[0], spec[1]
            perm = int(spec[2], 8) if len(spec) > 2 else 0o660
            base = typ.split("+")[0]
            st = {"stream": socket.SOCK_STREAM, "dgram": socket.SOCK_DGRAM,
                  "seqpacket": socket.SOCK_DGRAM}[base]      # Darwin: no AF_UNIX SEQPACKET
            path = self.root + "/dev/socket/" + name
            os.makedirs(os.path.dirname(path), exist_ok=True)
            try:
                os.unlink(path)
            except FileNotFoundError:
                pass
            s = socket.socket(socket.AF_UNIX, st)
            try:
                s.bind(path)
            except OSError as e:
                log("%s: socket %s: %s" % (svc.name, name, e))
                s.close()
                continue
            os.chmod(path, perm)
            fd = s.detach()
            os.set_inheritable(fd, True)
            fds.append(fd)
            env["ANDROID_SOCKET_" + re.sub(r"[^A-Za-z0-9_]", "_", name)] = str(fd)
        return fds, env

    def start(self, name, why):
        svc = self.rc.services.get(name)
        if not svc:
            log("start %s (%s): no such service" % (name, why))
            return False
        if name not in self.profile["start"] and name not in (self.a.also or []) and not self.a.all:
            if name not in self.not_started:
                reason = self.profile.get("left_out", {}).get(name)
                log("start %s (%s): not in the '%s' profile, not started%s" %
                    (name, why, self.a.profile, (": " + reason) if reason else ""))
            self.not_started[name] = why
            return False
        if svc.proc and svc.proc.poll() is None:
            return True
        if name == "zygote" and self.a.no_zygote:
            log("start zygote (%s): --no-zygote" % why)
            return False
        fds, senv = self.make_sockets(svc)
        env = dict(self.base_env(), **self.env)
        env.update(svc.env)
        env.update(senv)
        env["LXRT_ANDROID_IDS"] = self.ids_for(svc)
        if self.x86:
            env.update(self.profile.get("env_x86", {}).get(name, {}))
        for spec in self.a.svc_env or []:
            who, _, kv = spec.partition(":")
            if who in (name, "all") and "=" in kv:
                env[kv.split("=", 1)[0]] = kv.split("=", 1)[1]
        if name in (self.a.trace or []):
            env["LXRT_TRACE"] = "1"
            env["LXRT_TRACE_FILE"] = os.path.join(self.state, "svc", name + ".trace")
        os.makedirs(os.path.join(self.state, "svc"), exist_ok=True)
        out = open(os.path.join(self.state, "svc", name + ".log"), "a")
        out.write("---- start %s (%s) ids %s\n" % (time.strftime("%H:%M:%S"), why, env["LXRT_ANDROID_IDS"]))
        out.flush()
        if svc.argv and "app_process" in svc.argv[0] and not self.a.zygote_stdio:
            # init gives every service /dev/null as stdio. The zygote checks
            # each descriptor it would hand to a child against a list of
            # allowed paths and aborts on anything else: with this log file
            # as stdout it died forking system_server ("Not whitelisted (32):
            # .../svc/zygote.log", MEASURED). Other services keep the log.
            out.close()
            out = subprocess.DEVNULL
        svc.proc = self.spawn(svc.argv, env, out, pass_fds=fds, what="service " + name)
        for fd in fds:
            os.close(fd)
        svc.state = "running"
        svc.starts += 1
        svc.started_at = time.time()
        self.props.set("init.svc." + name, "running")
        if svc.starts == 1:
            self.props.set("ro.boottime." + name, str(int(time.monotonic() * 1e9)))
        log("started %s (pid %d, %s): %s [%s]" % (name, svc.proc.pid, why, " ".join(svc.argv),
                                                  env["LXRT_ANDROID_IDS"]))
        return True

    def stop(self, name, why, restart=False):
        svc = self.rc.services.get(name)
        if not svc:
            return
        if svc.proc and svc.proc.poll() is None:
            try:
                os.killpg(svc.proc.pid, signal.SIGKILL)
            except OSError:
                pass
            svc.proc.wait()
        svc.state = "stopped"
        svc.restart_at = time.time() if restart else None
        if not restart:
            self.props.set("init.svc." + name, "stopped")
        log("%s %s (%s)" % ("restarting" if restart else "stopped", name, why))

    def reap(self):
        for name, svc in self.rc.services.items():
            if svc.proc is None or svc.state != "running":
                continue
            rc = svc.proc.poll()
            if rc is None:
                continue
            how = ("signal %d" % -rc) if rc < 0 else ("status %d" % rc)
            log("service %s (pid %d) exited with %s after %.1f s" %
                (name, svc.proc.pid, how, time.time() - svc.started_at))
            if svc.oneshot:
                svc.state = "stopped"
                self.props.set("init.svc." + name, "stopped")
                continue
            now = time.time()
            svc.crashes = [t for t in svc.crashes if now - t < 240] + [now]
            if svc.critical and len(svc.crashes) > 4:
                log("critical service %s crashed %d times in 4 minutes: init would reboot into the "
                    "bootloader; the boot stops here" % (name, len(svc.crashes)))
                self.failed = "critical service %s crash loop" % name
                return
            svc.state = "restarting"
            svc.restart_at = now + 5
            self.props.set("init.svc." + name, "restarting")
            for cmd in svc.onrestart:
                self.run_cmd(cmd, "onrestart " + name)
        for name, svc in self.rc.services.items():
            if svc.state == "restarting" and svc.restart_at and time.time() >= svc.restart_at:
                svc.restart_at = None
                self.start(name, "restart")

    # -- actions
    def conds_ok(self, act):
        for k, v in act.props:
            cur = self.props.get(k, "")
            if v == "*":
                if cur == "":
                    return False
            elif cur != v:
                return False
        return True

    def trigger(self, event):
        for act in self.rc.actions:
            if act.event == event and self.conds_ok(act):
                self.queue.append(act)

    def property_changed(self, name):
        for act in self.rc.actions:
            if any(k == name for k, _ in act.props) and self.conds_ok(act):
                if act.event is None:
                    self.queue.append(act)

    def run_queue(self):
        while self.queue:
            act = self.queue.pop(0)
            for cmd in act.cmds:
                self.run_cmd(cmd, "%s: on %s" % (act.src, act.event or ""))

    def hpath(self, g):
        return self.root + g

    def data_path(self, g):
        # What init writes that is the root's own state: /data, and the
        # mount points under /mnt and /storage the zygote binds per user
        # (MountEmulatedStorage: fs_prepare_dir of /mnt/user/0 failed without
        # them, MEASURED). /dev, /proc, /sys, /acct, /config are the kernel's.
        return any(g == d or g.startswith(d + "/") for d in ("/data", "/mnt", "/storage"))

    def skip(self, cmd, why):
        k = cmd[0]
        self.skipped.setdefault(k, []).append(" ".join(cmd) + (" (%s)" % why if why else ""))

    def run_cmd(self, cmd, where):
        cmd = [self.rc.expand(t) for t in cmd]
        c, a = cmd[0], cmd[1:]
        try:
            if c == "setprop":
                self.props.set(a[0], a[1] if len(a) > 1 else "")
            elif c == "export":
                self.env[a[0]] = a[1] if len(a) > 1 else ""
            elif c == "mkdir":
                if not self.data_path(a[0]):
                    return self.skip(cmd, "not /data, /mnt or /storage")
                mode = int(a[1], 8) if len(a) > 1 and a[1].isdigit() else 0o755
                os.makedirs(self.hpath(a[0]), exist_ok=True)
                os.chmod(self.hpath(a[0]), mode)
            elif c == "write":
                if not self.data_path(a[0]):
                    return self.skip(cmd, "not /data, /mnt or /storage")
                with open(self.hpath(a[0]), "w") as f:
                    f.write(a[1])
            elif c == "copy":
                if not self.data_path(a[1]):
                    return self.skip(cmd, "not /data, /mnt or /storage")
                with open(self.hpath(a[0]), "rb") as f:
                    d = f.read()
                with open(self.hpath(a[1]), "wb") as f:
                    f.write(d)
            elif c == "symlink":
                if not self.data_path(a[1]):
                    return self.skip(cmd, "not /data, /mnt or /storage")
                if not os.path.lexists(self.hpath(a[1])):
                    os.symlink(a[0], self.hpath(a[1]))
            elif c == "trigger":
                self.trigger(a[0])
            elif c == "mount_all":
                # Waydroid's init (base-patches-30/system/core/0002): mounts
                # are the container's; the result is always "not encrypted".
                self.props.set("ro.crypto.state", "unencrypted")
                self.trigger("nonencrypted")
            elif c == "start":
                self.start(a[0], where)
            elif c == "stop":
                self.stop(a[0], where)
            elif c == "restart":
                self.stop(a[0], where, restart=True)
            elif c == "enable":
                svc = self.rc.services.get(a[0])
                if svc and svc.disabled:
                    svc.disabled = False
                    if any(k in self.class_started for k in svc.classes):
                        self.start(a[0], where + " (enable)")
            elif c in ("class_start", "class_start_post_data"):
                self.class_started.add(a[0])
                for name in self.rc.order:
                    svc = self.rc.services[name]
                    if a[0] in svc.classes and not svc.disabled:
                        self.start(name, "class_start " + a[0])
            elif c in ("class_stop", "class_reset", "class_reset_post_data"):
                for name in self.rc.order:
                    if a[0] in self.rc.services[name].classes:
                        self.stop(name, c + " " + a[0])
            elif c == "exec_start":
                svc = self.rc.services.get(a[0])
                if svc and (a[0] in self.profile["start"] or a[0] in (self.a.also or []) or self.a.all):
                    self.start(a[0], where + " (exec_start)")
                    try:
                        svc.proc.wait(timeout=60)
                    except subprocess.TimeoutExpired:
                        log("exec_start %s: still running after 60 s" % a[0])
                else:
                    self.skip(cmd, "not in the profile")
            else:
                self.skip(cmd, "")
        except (OSError, IndexError, ValueError) as e:
            log("%s: %s: %s" % (where, " ".join(cmd), e))

    # -- control messages and property changes from the property service
    def poll_ctl(self):
        while True:
            try:
                d = self.ctl.recv(4096)
            except BlockingIOError:
                return
            parts = d.split(b"\0")
            if len(parts) < 4:
                continue
            kind, x, y, pid = (p.decode(errors="replace") for p in parts[:4])
            if kind == "set":
                old = self.props.get(x)
                self.props[x] = y
                if old != y:
                    if x in self.watch:
                        log("property %s=%s" % (x, y))
                    self.property_changed(x)
            elif kind == "ctl":
                log("ctl.%s %s (from pid %s)" % (x, y, pid))
                name = y
                if x.startswith("interface_"):
                    name = next((n for n, s in self.rc.services.items()
                                 if any(i.split()[-1] == y or " ".join(i.split()) == y.replace("/", " ")
                                        or i.replace(" ", "/") == y for i in s.interfaces)), None)
                    if not name:
                        log("ctl.%s %s: no service declares that interface" % (x, y))
                        continue
                    x = x[len("interface_"):]
                if x == "start":
                    self.start(name, "ctl.start from pid " + pid)
                elif x == "stop":
                    self.stop(name, "ctl.stop from pid " + pid)
                elif x == "restart":
                    self.stop(name, "ctl.restart from pid " + pid, restart=True)
                else:
                    log("ctl.%s: not handled" % x)

    # -- the boot
    def run(self):
        os.makedirs(self.state, mode=0o700, exist_ok=True)
        os.chmod(self.state, 0o700)
        os.makedirs(self.binderdir, mode=0o700, exist_ok=True)
        os.chmod(self.binderdir, 0o700)
        global LOGF
        LOGF = open(os.path.join(self.state, "init.log"), "a")
        log("== android-boot: root %s (%s), state %s, lxrun %s" %
            (self.root, "x86_64 under FEX" if self.x86 else "arm64", self.state, self.lxrun))
        self.failed = None
        self.watch = set(self.a.watch or []) | {"sys.boot_completed", "dev.bootcomplete",
                                                   "hwservicemanager.ready", "service.bootanim.exit",
                                                   "sys.system_server.start_count", "ro.crypto.state"}
        try:
            self.start_fexserver()
            self.start_propsvc()
            self.start_logd()
            self.rc = Rc(self.root, self.props)
            self.rc.parse_file("/system/etc/init/hw/init.rc")
            for d in ("/system/etc/init", "/system_ext/etc/init", "/product/etc/init",
                      "/odm/etc/init", "/vendor/etc/init"):
                self.rc.parse_dir(d)
            # The APEXes' own .rc files, which init reads once they are
            # mounted (perform_apex_config; flattened here, under /apex).
            for apex in sorted(os.listdir(self.root + "/apex")) if os.path.isdir(self.root + "/apex") else []:
                self.rc.parse_dir("/apex/%s/etc" % apex)
            log("rc: %d files, %d services, %d actions" %
                (len(self.rc.parsed), len(self.rc.services), len(self.rc.actions)))
            for ev in ("early-init", "init", "late-init"):
                self.trigger(ev)
                self.run_queue()
                self.settle(0.2)
            # queue_property_triggers: every "on property:" whose values hold now
            for act in self.rc.actions:
                if act.event is None and act.props and self.conds_ok(act):
                    self.queue.append(act)
            self.run_queue()
            log("boot actions done in %.1f s; skipped commands: %s" %
                (time.time() - self.t0, ", ".join("%s x%d" % (k, len(v)) for k, v in sorted(self.skipped.items()))))
            log("asked for but not in the profile: %s" % ", ".join(sorted(self.not_started)))
            self.loop()
        finally:
            self.shutdown()

    def settle(self, secs):
        end = time.time() + secs
        while time.time() < end:
            self.poll_ctl()
            self.run_queue()
            time.sleep(0.05)

    def loop(self):
        end = self.t0 + self.a.seconds if self.a.seconds else None
        want = None
        if self.a.until_prop:
            k, _, v = self.a.until_prop.partition("=")
            want = (k, v)
            self.watch.add(k)
        while True:
            self.poll_ctl()
            self.run_queue()
            self.reap()
            if self.failed:
                log("boot failed: %s" % self.failed)
                return
            if want and self.props.get(want[0]) == want[1]:
                log("%s=%s after %.1f s" % (want[0], want[1], time.time() - self.t0))
                if not self.a.keep_running:
                    return
                want = None
            if end and time.time() >= end:
                log("--seconds %d reached" % self.a.seconds)
                return
            time.sleep(0.1)

    def shutdown(self):
        log("stopping")
        for name, svc in getattr(self, "rc", Rc(self.root, {})).services.items():
            if svc.proc and svc.proc.poll() is None:
                try:
                    os.killpg(svc.proc.pid, signal.SIGTERM)
                except OSError:
                    pass
        time.sleep(1)
        # What the services left (zygote's children, the binder hub): every
        # process whose environment names this state directory.
        mine = set()
        try:
            out = subprocess.run(["ps", "-axEww", "-o", "pid=,command="], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, timeout=30).stdout.decode(errors="replace")
            for line in out.splitlines():
                if ("LXRT_PROPERTY_DIR=" + self.propdir) in line or ("--binder-hub " + self.binderdir) in line:
                    pid = int(line.split()[0])
                    if pid != os.getpid():
                        mine.add(pid)
        except (OSError, subprocess.SubprocessError, ValueError):
            pass
        for p, what in self.children:
            if p.poll() is None:
                mine.add(p.pid)
        for pid in mine:
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        for p, _ in self.children:
            try:
                p.wait(timeout=5)
            except subprocess.TimeoutExpired:
                pass
        log("stopped %d processes" % len(mine))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.environ.get("ANDROID_X86_ROOT", "/Volumes/SteamARMAndroid/root-x86_64"))
    ap.add_argument("--lxrun", default=os.environ.get("LXRUN", os.path.join(REPO, "build/lxrun")))
    ap.add_argument("--state", default=None, help="private state directory (default /tmp/lxrt-android-<uid>-<hash>)")
    ap.add_argument("--seconds", type=int, default=300)
    ap.add_argument("--until-prop", default=None, help="NAME=VALUE: stop once the property has the value")
    ap.add_argument("--keep-running", action="store_true")
    ap.add_argument("--profile", default="headless", choices=sorted(PROFILES))
    ap.add_argument("--all", action="store_true", help="start every service the actions ask for")
    ap.add_argument("--also", action="append", help="start this service too (outside the profile)")
    ap.add_argument("--no-zygote", action="store_true")
    ap.add_argument("--zygote-stdio", action="store_true", help="the zygote's stdio to its log too (it then refuses to fork)")
    ap.add_argument("--bootargs", default=os.environ.get("LXRT_PROPERTY_BOOTARGS", ""))
    ap.add_argument("--persist", default=None, help="persistent property file (default: the root's /data/property)")
    ap.add_argument("--watch", action="append", help="log changes of this property")
    ap.add_argument("--prop", action="append", help="NAME=VALUE set before the first action")
    ap.add_argument("--trace", action="append", help="LXRT_TRACE for this service (<state>/svc/NAME.trace)")
    ap.add_argument("--svc-env", action="append", help="SERVICE:VAR=VALUE (SERVICE 'all' for every one)")
    a = ap.parse_args()
    if not a.state:
        import hashlib
        a.state = "/tmp/lxrt-android-%d-%s" % (os.getuid(),
                                                hashlib.sha1(os.path.realpath(a.root).encode()).hexdigest()[:10])
    boot = Boot(a)
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(1))
    boot.run()
    return 1 if boot.failed else 0


if __name__ == "__main__":
    sys.exit(main())
