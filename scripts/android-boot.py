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
                          [--linkerconfig] [--wayland XDG[:SOCKET]] [--abi32]
                          [--exit-with PID]

--linkerconfig runs init's update_linker_config (the image's linkerconfig,
with the property service up): it rewrites the root's /linkerconfig, which
every guest of the root reads, so it is not the default. The 32-bit audio
HAL needs it (--also vendor.audio-hal --linkerconfig).

Stops, at the end, every process it started and every process those left in
the root's state (found by the state directory in their environment), and
nothing else.
"""
import argparse
import os
import re
import shlex
import shutil
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
        "env_x86": {"zygote": {"FEX_NEEDSSECCOMP": "1"}, "zygote_secondary": {"FEX_NEEDSSECCOMP": "1"}},
        # Asked for by the .rc files but left out, and why (the boot logs
        # each one it skips).
        "left_out": {
            "ueventd": "no uevents and no device nodes to make (ro.cold_boot_done=true)",
            "logd": "tests/android/logd.py stands in for its write socket",
            "lmkd": "no memory cgroups or PSI; its socket has a stand-in (Boot.start_lmkd_socket)",
            "vold": "no block devices, no FUSE: /data is a directory of the root",
            "apexd": "flattened APEXes (ro.apex.updatable unset), already under /apex",
            "netd": "exits at once: NETLINK_KOBJECT_UEVENT, route netlink, iptables, BPF; its "
                    "'onrestart restart zygote' then kills the zygote every 5 s",
            # Started instead with --linkerconfig when the root's FEX carries
            # patches/fex-lxrt-i386-bionic.patch (Boot.__init__).
            "vendor.audio-hal": "i386 (32-bit): needs a FEX with patches/fex-lxrt-i386-bionic.patch and "
                                "--linkerconfig (the legacy /linkerconfig has no VNDK namespace for "
                                "android.hardware.audio@4.0.so); audioserver waits for it",
            "vendor.audio-hal-2-0": "declares the same interface; no such binary in this image",
            "vendor.hwcomposer-2-1": "Waydroid's composer is a Wayland client: no display here",
            # Started instead when the root's FEX carries
            # patches/fex-lxrt-i386-bionic.patch (Boot.__init__); otherwise the
            # host .prop declares no 32-bit ABI (HOST_ABI_PROPS), or
            # system_server waits 20 s for it (hasSecondZygote, "x86_64,x86").
            "zygote_secondary": "app_process32 (i386): the root's FEX predates patches/fex-lxrt-i386-bionic.patch",
            "bootanim": "graphics",
            "statsd": "cannot link: the root's /linkerconfig (written by the image's linkerconfig "
                      "before properties existed) has no namespace for the statsd APEX",
        },
    },
}

# The same with a screen (stage 28, benchmarks/stage28-android-apk.txt):
# Waydroid's composer HAL connects to a Wayland compositor (scripts/run-
# weston.sh; --wayland names its socket), SurfaceFlinger presents through it,
# and init's bootanim runs until system_server says the boot is complete.
PROFILES["display"] = {
    "start": PROFILES["headless"]["start"] | {
        "vendor.hwcomposer-2-1",   # composer@2.1 + hwcomposer.waydroid (a Wayland client)
        "task-hal-1-0",            # vendor.waydroid.task@1.0: the composer waits for it
        "bootanim",                # started by SurfaceFlinger (ctl.start), stopped by system_server
        "netd", "vold",            # stand-ins (below)
        "mediametrics",            # media.metrics: audioserver waits 5 s for it per call and its
                                   # TimeCheck aborts it (MEASURED)
        "mediaextractor",          # media.extractor and media.swcodec, which SoundPool and
        "media.swcodec",           # MediaCodec need ("extractor service not running", MEASURED);
                                   # both x86-64; mediaextractor's watchdog needs timer_create
                                   # (runtime/posixtimer.c)
        "media",                   # mediaserver (media.player) and the OMX store, which
        "vendor.media.omx",        # MediaCodecList asks first; both i386 (runtime/ids.c)
        "traced",                  # perfetto's service: Traceur asks `perfetto --is_detached`
                                   # at every boot and crashed without it ("Perfetto error: 1").
                                   # (Not tombstoned: its listening SEQPACKET sockets are
                                   # datagram sockets here, runtime/socket.c, and it aborts.)
    },
    # Started after sys.boot_completed=1: started with the boot they slowed it
    # enough that an app process attached to ActivityManager before its pid
    # was recorded ("No pending application record ... dropping process"),
    # the network stack was killed with it and system_server went down with
    # the zygote, again and again (MEASURED).
    "after_boot": {"media", "vendor.media.omx", "traced"},
    # 32-bit vendor daemons: their own linker configuration (the legacy
    # /linkerconfig has no namespace for them: the OMX store could not link
    # libminijail.so, MEASURED) and image libraries replaced by stand-ins
    # (scripts/android/shims) for them alone. The OMX store's minijail setup:
    # under FEX's seccomp emulation its 32-bit filter crashed FEX in its own
    # code and the store spun at 100% CPU instead of registering; without
    # the emulation the filter cannot be installed and the store aborts
    # (MEASURED). The stand-in installs no filter.
    "vendor32": {"vendor.media.omx": {"/vendor/lib/libavservices_minijail.so": "avservices_minijail_noop.c"}},
    # audioserver's audio HAL: Waydroid's, with the presentation position it
    # lacks (scripts/android/shims/audio_hal_presentation.c), loaded as
    # audio.primary.default.so (DISPLAY_PROPS: ro.hardware.audio.primary).
    "shims64": {"audioserver": {"/vendor/lib64/hw/audio.primary.default.so": "audio_hal_presentation.c"}},
    # audioserver loads Waydroid's audio HAL in-process (vintf_passthrough,
    # below), 64-bit, and the HAL opens ALSA's "pulse" device; the image's
    # 64-bit alsa-lib was built with /vendor/lib/hw/ as its plugin
    # directory, where the plugin is 32-bit ("is 32-bit" x4400, no sound,
    # MEASURED). ALSA_PLUGIN_DIR is alsa-lib's own override.
    "env_x86": dict(PROFILES["headless"]["env_x86"],
                    audioserver={"ALSA_PLUGIN_DIR": "/vendor/lib64/hw"},
                    # Both install a seccomp filter (minijail), as the zygote does.
                    mediaextractor={"FEX_NEEDSSECCOMP": "1"},
                    **{"media.swcodec": {"FEX_NEEDSSECCOMP": "1"}}),
    "left_out": {k: v for k, v in PROFILES["headless"]["left_out"].items()
                 if k not in ("vendor.hwcomposer-2-1", "bootanim", "netd", "vold")},
    # Services replaced by a stand-in (scripts/android/java/.../BinderStandIn.java):
    # the same name, user and capabilities, the image's own AIDL interfaces,
    # every call answered with success and empty values. netd cannot run
    # (no netlink route/uevent sockets, iptables, BPF, tc on a Mac), and
    # system_server waits for its "netd" and "dnsresolver" services forever
    # ("NetdService: WARNING: returning null INetd instance.", MEASURED).
    # So the framework boots with no network at all.
    # Of the daemon's .rc sockets only those named with socket= are made:
    # NsdService waits in system_server's start-up until it has connected to
    # netd's "mdns" (MEASURED). fwmarkd and dnsproxyd stay absent, so every
    # app's libnetd_client finds no fwmark server and carries on, and a DNS
    # lookup fails at once instead of waiting for an answer.
    # vold likewise (no block devices, FUSE, dm or fscrypt here): without its
    # "vold" service StorageManagerService's calls met a null IVold and
    # finishBooting's checkpoint commit rebooted the framework ("Rebooting,
    # reason: Checkpoint commit failed", MEASURED); its booleans are questions
    # (isCheckpointing, needsCheckpoint, ...), answered no. "storaged" goes
    # with it (StorageManagerService asks for both).
    "stand_ins": {
        # network=@mac: the one network, the Mac's (NetworkAgentStandIn;
        # Boot.network_spec fills it in).
        "netd": ["netd=android.net.INetd", "dnsresolver=android.net.IDnsResolver", "socket=mdns", "network=@mac"],
        "vold": ["vold=android.os.IVold:false", "storaged=android.os.IStoraged"],
    },
    # HIDL HALs whose service binary cannot run here, served in-process
    # instead: hwservicemanager is shown a copy of the vendor manifest with
    # these HALs' transport "passthrough" (a bind of that one file, for that
    # one process), so a client loads the HAL's x86-64 implementation from
    # /vendor/lib64/hw itself, as libhidl does for passthrough HALs. The
    # audio HAL's service is i386 and FEX's 32-bit mode stops it; audioserver
    # then waited for IDevicesFactory forever and never published
    # media.audio_flinger, which AudioService waits for (MEASURED).
    "vintf_passthrough": ["android.hardware.audio", "android.hardware.audio.effect"],
}


def host_timezone():
    """The Mac's time zone for Android (persist.sys.timezone, as Settings
    sets it): its IANA name from /etc/localtime's link (a name the image's
    tzdata lacks leaves Android on GMT, as with none). Without it Android
    kept UTC (MEASURED: 11:27 on a Mac at 08:27, UTC-3)."""
    try:
        link = os.readlink("/etc/localtime")
    except OSError:
        return []
    m = re.search(r"zoneinfo/(.+)$", link)
    return [("persist.sys.timezone", m.group(1))] if m else []


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
    # Media codecs from the OMX store (Google's software codecs and
    # Waydroid's FFmpeg plugin), not Codec2: Codec2's software components
    # crashed at 0x0 in their MediaCodec_loop thread here, and the OMX
    # vorbis decoder decodes (MEASURED). AOSP's own switch (0: no Codec2).
    ("debug.stagefright.ccodec", "0"),
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
    # select_execution_binary). dex2oat32 died of SIGSEGV in FEX's 32-bit
    # mode until patches/fex-lxrt-i386-bionic.patch (stage 28); x86-64
    # dex2oat64 is kept: it needs no i386 fix and ran first
    # (benchmarks/stage25-art-x86-fex.txt).
    ("dalvik.vm.dex2oat64.enabled", "true"),
    # Waydroid's sensors HAL serves stub sensors only with this (Lepton's
    # properties.sh sets it, docs/LEPTON_REUSE_ANALYSIS.md 0.3); without it
    # the HAL exits at once and system_server's StartSensorService waits for
    # ISensors until the Watchdog kills it (MEASURED, stage 28).
    ("waydroid.stub_sensors_hal", "1"),
    # Android 11's app data isolation: the zygote mounts a tmpfs over
    # /data/user, /data/data and /data/misc/profiles/cur in each app's mount
    # namespace and binds back only that app's directories. The runtime
    # emulates bind mounts (per process) but not tmpfs, so every app died in
    # the zygote ("Failed to mount tmpfs to /data/misc/profiles/cur:
    # Operation not permitted", SystemUI first, MEASURED). The framework's
    # switch for it (ProcessList, persist.zygote.app_data_isolation, default
    # true) turns it off; every guest is the same Mac user anyway, so the
    # isolation had nothing under it here.
    ("persist.zygote.app_data_isolation", "false"),
]

# The display profile adds these. The boot animation is SwiftShader drawing
# 57 frames a second on the CPU (bootanimation 35% and SurfaceFlinger 40% of a
# core, stage 27) while every framework process starts under FEX; the network
# stack's TetheringService then missed ActivityManager's 20 s service timeout
# and its loss took system_server down ("Lost network stack", MEASURED).
# debug.sf.nobootanimation is SurfaceFlinger's own switch: it does not start
# bootanim, and the screen stays black until the first window.
DISPLAY_PROPS = [("debug.sf.nobootanimation", "1"),
                 # audioserver's HAL module: "default" is the one bound to
                 # scripts/android/shims/audio_hal_presentation.c ("shims64").
                 ("ro.hardware.audio.primary", "default")]


# What this host can run, which the image cannot know: the property service
# loads these, with HOST_PROPS and --prop, from a .prop file of this script's
# (LXRT_PROPERTY_HOST, loaded last among the image's .prop files, as Waydroid's
# container manager writes waydroid.prop for each boot).
HOST_ABI_PROPS = [
    # Without a FEX that runs i386 bionic (patches/fex-lxrt-i386-bionic.patch;
    # before it: "NoExec instruction in entry block", stage 27), or in the
    # display profile without --abi32 (Boot.abi32), no 32-bit code runs, so
    # the device declares none.
    # With the image's "x86_64,x86" the primary zygote waited 20 s for a
    # secondary one, finishBooting died telling the absent 32-bit zygote the
    # boot was complete ("Failed to inform zygote of boot_completed"), and the
    # WebView loader's 32-bit process failed to start, whereupon
    # ActivityManager force-stopped package "android" and the settings
    # provider every app needs went missing (MEASURED, stage 28).
    ("ro.product.cpu.abilist", "x86_64"),
    ("ro.product.cpu.abilist32", ""),
]


def log(msg):
    t = time.strftime("%H:%M:%S") + ".%03d" % (int(time.time() * 1000) % 1000)
    line = "%s %s" % (t, msg)
    print(line, flush=True)
    if LOGF:
        LOGF.write(line + "\n")
        LOGF.flush()


LOGF = None

# Virtual file ownership (runtime/android_ids.h): the owner guests with
# Android ids see for a file is this extended attribute of the Mac's file;
# init's mkdir/chown here record it the same way a guest's chown does.
OWNER_XATTR = b"org.steamarm.lxrt.owner"
_libc = None


def set_owner(host_path, uid, gid=None):
    global _libc
    import ctypes
    if _libc is None:
        _libc = ctypes.CDLL(None, use_errno=True)
        _libc.setxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.c_uint32, ctypes.c_int]
        _libc.getxattr.argtypes = list(_libc.setxattr.argtypes)
        _libc.getxattr.restype = _libc.setxattr.restype = ctypes.c_ssize_t
    p = host_path.encode()
    if uid is None or gid is None:
        buf = ctypes.create_string_buffer(32)
        n = _libc.getxattr(p, OWNER_XATTR, buf, 31, 0, 1)
        cur = buf.raw[:n].decode().split(":") if n > 0 else [str(os.getuid()), str(os.getgid())]
        uid = int(cur[0]) if uid is None else uid
        gid = int(cur[1]) if gid is None else gid
    v = ("%d:%d" % (uid, gid)).encode()
    if _libc.setxattr(p, OWNER_XATTR, v, len(v), 0, 1) != 0:        # XATTR_NOFOLLOW
        log("set_owner %s %d:%d: errno %d" % (host_path, uid, gid, ctypes.get_errno()))


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
        self.inputdir = os.path.join(self.state, "input")
        self.lxrun = os.path.realpath(a.lxrun)
        self.profile = PROFILES[a.profile]
        self.props = Props(self)
        self.children = []            # (Popen, what)
        self.env = {}
        self.skipped = {}
        self.not_started = {}
        self.queue = []
        self.class_started = set()
        self.deferred = {}
        self.x86 = self.detect_x86()
        # i386 programs run under a FEX with patches/fex-lxrt-i386-bionic.patch
        # (it carries this string): then the secondary zygote is started too
        # (benchmarks/stage28-android-reliability.txt).
        # With --linkerconfig the 32-bit audio HAL links as well (binder for
        # 32-bit guests, runtime/binder.c), and audioserver stops waiting.
        # The display profile leaves 32-bit out unless asked (--abi32): the
        # device then declares no 32-bit ABI (HOST_ABI_PROPS) and one zygote
        # fewer shares the CPU with the boot.
        self.abi32 = bool(self.x86 and (a.profile == "headless" or a.abi32) and self.fex_has(b"lxrt-i386-bionic"))
        if self.abi32:
            extra = {"zygote_secondary"} | ({"vendor.audio-hal"} if a.linkerconfig else set())
            prof = dict(self.profile)
            prof["start"] = set(prof["start"]) | extra
            prof["left_out"] = {k: v for k, v in prof["left_out"].items() if k not in extra}
            self.profile = prof
        self.t0 = time.time()

    def fex_has(self, marker):
        try:
            with open(self.root + "/usr/lib/lxrt-emu/FEX", "rb") as f:
                return marker in f.read()
        except OSError:
            return False

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
            # No runtime chatter on Android's stderr (runtime lxrt_info_stream):
            # programs capture their children's (Termux's `$(id -u 2>&1)`).
            "LXRT_QUIET": os.environ.get("LXRT_QUIET", "1"),
            # This boot's /dev/input (runtime/evdev.c): the display composer's
            # input FIFOs and InputFlinger's EventHub meet there, not in the
            # shared /tmp/lxrt-input of steamarm-inputd's controllers.
            "LXRT_INPUT_DIR": self.inputdir,
            # Ids below 65536 for every process of the stack (runtime/ids.c),
            # as scripts/run-android-x86.sh gives its guests: 32-bit bionic
            # keeps them in 16 bits, and binder, /proc and kill must agree.
            "LXRT_SMALL_IDS": os.environ.get("LXRT_SMALL_IDS", "1"),
            # Marks every process of the boot as the Android session's, so
            # scripts/run-steam.sh and scripts/run-app.sh, which stop guest
            # programs, leave it alone (carried through guest execs).
            "LXRT_SESSION": "android",
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
        # cwd: the root, which is the guest's "/" (init's cwd). A relative
        # path resolves against the host's working directory; with the Mac's
        # "/" there, PackageManagerServiceUtils.makeDirRecursive (which walks
        # "data", "data/app", ... relative) found none of them, tried to make
        # "/data" and every `pm install` failed with "Failed rename"
        # (MEASURED at stage 28). getcwd still answers "/".
        p = subprocess.Popen([self.lxrun] + argv, env=env, stdin=subprocess.DEVNULL,
                             stdout=out, stderr=subprocess.STDOUT, pass_fds=pass_fds,
                             preexec_fn=own_group, cwd=self.root)
        self.children.append((p, what))
        return p

    def guest(self, argv, timeout=60, env_extra=None):
        env = dict(self.base_env(), **self.env)
        env.update(env_extra or {})
        r = subprocess.run([self.lxrun] + argv, env=env, stdin=subprocess.DEVNULL,
                           stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout, cwd=self.root)
        return r.returncode, r.stdout.decode(errors="replace")

    # -- start-up
    def start_fexserver(self):
        if not self.x86:
            return
        # scripts/run-android-x86.sh starts the root's own FEXServer (and
        # writes /linkerconfig once); a no-op program is enough.
        # It outlives its clients (FEXServer ignores --persistent's timeout
        # with --foreground), so shutdown() stops it again unless it was
        # already running: each one left counts toward the memory guard.
        env = dict(os.environ, ANDROID_X86_ROOT=self.root, LXRUN=self.lxrun)
        self.fexserver_env = env
        self.fexserver_was_running = bool(self.fexserver_pid())
        subprocess.run([os.path.join(HERE, "run-android-x86.sh"), "/system/bin/toybox", "true"],
                       env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)

    def fexserver_pid(self):
        env = dict(os.environ, ANDROID_X86_ROOT=self.root, LXRUN=self.lxrun)
        try:
            return subprocess.run([os.path.join(HERE, "run-android-x86.sh"), "--server-pid"], env=env,
                                  stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                  timeout=30).stdout.decode(errors="replace").strip()
        except (OSError, subprocess.SubprocessError):
            return ""

    def stop_fexserver(self):
        if getattr(self, "fexserver_was_running", True):
            return
        try:
            subprocess.run([os.path.join(HERE, "run-android-x86.sh"), "--server-stop"], env=self.fexserver_env,
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
        except (OSError, subprocess.SubprocessError):
            pass

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
        hostprop = os.path.join(self.state, "host.prop")
        extra = [tuple(x.split("=", 1)) for x in (self.a.prop or []) if "=" in x]
        with open(hostprop, "w") as f:
            f.write("# scripts/android-boot.py: this host's properties (LXRT_PROPERTY_HOST)\n")
            abi = [] if self.abi32 else HOST_ABI_PROPS
            for k, v in abi + HOST_PROPS + (DISPLAY_PROPS if self.a.profile == "display" else []) + extra:
                f.write("%s=%s\n" % (k, v))
        env = dict(self.base_env(), LXRT_PROPERTY_IDLE="0", LXRT_PROPERTY_INIT=cp,
                   LXRT_PROPERTY_HOST=hostprop)
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
        host = HOST_PROPS + host_timezone()
        for k, v in host + [tuple(x.split("=", 1)) for x in (self.a.prop or [])]:
            if self.props.get(k) != v:
                self.props.set(k, v)
        log("host properties: %s" % ", ".join("%s=%s" % (k, self.props.get(k, "")) for k, _ in host))

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

    def start_lmkd_socket(self):
        """lmkd's socket, with nothing behind it but a reader. lmkd itself
        cannot work here (no memory cgroups, no PSI), and without its socket
        every oom_adj change in ActivityManager waited 3 s for a connection
        (ProcessList.writeLmkd: waitForConnection(3 * LMKD_RECONNECT_DELAY_MS))
        with the ActivityManager lock held: 56 "SLOW OOM ADJ: 3000ms" in one
        boot, then ANRs of SystemUI and the network stack, whose loss took
        system_server down ("Lost network stack", MEASURED at stage 28). The
        guest's SEQPACKET socket is a datagram socket here (runtime/socket.c),
        so this is one too; every command is read and dropped: no process is
        ever killed for memory, as with no lmkd. The only commands that want
        an answer (LMK_GETKILLCNT) come from dumps (dumpsys activity lmk,
        ActivityManagerService.reportLmkKillAtOrBelow; VERIFIED IN SOURCE, the
        image's services.jar) and would wait for one."""
        import threading
        path = self.root + "/dev/socket/lmkd"
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        s = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        try:
            s.bind(path)
        except OSError as e:
            log("lmkd socket: %s" % e)
            s.close()
            return
        os.chmod(path, 0o660)
        self.lmkd_msgs = 0

        def reader():
            while True:
                try:
                    if not s.recv(4096):
                        continue
                except OSError:
                    return
                self.lmkd_msgs += 1

        threading.Thread(target=reader, name="lmkd", daemon=True).start()
        log("lmkd stand-in on /dev/socket/lmkd (commands read and dropped)")

    def network_spec(self):
        """The netd stand-in's network=IFACE,ADDRESS,GATEWAY,DNS;...: an
        Ethernet interface with a nominal address (sockets go out through
        the Mac, whatever it says) and the Mac's name servers.
        STEAMARM_ANDROID_NETWORK=0: none, as before."""
        if os.environ.get("STEAMARM_ANDROID_NETWORK") == "0":
            return None
        import android_dnsproxy
        # A scoped link-local server (fe80::1%en0) names a Mac interface.
        dns = [d for d in android_dnsproxy.nameservers() if "%" not in d]
        return "network=eth0,10.0.2.15/24,10.0.2.2," + ";".join(dns)

    def start_dnsproxy(self):
        """netd's /dev/socket/dnsproxyd, answered by the Mac's resolver
        (scripts/android_dnsproxy.py): netd itself is a stand-in here, and
        with no dnsproxyd every app's getaddrinfo failed at once."""
        import android_dnsproxy
        try:
            self.dnsproxy, self.dns_counts = android_dnsproxy.serve(self.root, log)
        except OSError as e:
            log("dnsproxyd: %s" % e)
            return
        log("dnsproxyd on /dev/socket/dnsproxyd (the Mac's resolver)")

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
    def make_sockets(self, svc, only=None):
        fds, env = [], {}
        for spec in svc.sockets:
            name, typ = spec[0], spec[1]
            if only is not None and name not in only:
                continue
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
        if name in self.profile.get("after_boot", ()) and self.props.get("sys.boot_completed") != "1":
            # Started once the system has booted (profile "after_boot").
            if name not in self.deferred:
                log("start %s (%s): after sys.boot_completed=1" % (name, why))
            self.deferred[name] = why
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
        stand_in = self.profile.get("stand_ins", {}).get(name)
        argv = svc.argv
        if stand_in:
            dex = self.standin_dex()
            if not dex:
                log("start %s (%s): its stand-in could not be built" % (name, why))
                return False
            args = [self.network_spec() if x == "network=@mac" else x for x in stand_in]
            argv = ["/system/bin/app_process64", "/system/bin", "org.steamarm.android.BinderStandIn"] + \
                [x for x in args if x]
            keep = [x.split("=", 1)[1] for x in stand_in if x.startswith("socket=")]
            fds, senv = self.make_sockets(svc, only=keep)
            senv["CLASSPATH"] = dex + ":/system/framework/services.jar"
        else:
            fds, senv = self.make_sockets(svc)
        env = dict(self.base_env(), **self.env)
        env.update(svc.env)
        env.update(senv)
        env["LXRT_ANDROID_IDS"] = self.ids_for(svc)
        if getattr(self, "storage_mounts", ""):
            env["LXRT_MOUNTS"] = self.storage_mounts + env.get("LXRT_MOUNTS", "")
        if self.x86:
            env.update(self.profile.get("env_x86", {}).get(name, {}))
        if name == "hwservicemanager" and self.profile.get("vintf_passthrough"):
            m = self.passthrough_manifest(self.profile["vintf_passthrough"])
            if m:
                # One bind in the runtime's table (runtime/mounts.c): this
                # process reads the copy at the image's path.
                env["LXRT_MOUNTS"] = env.get("LXRT_MOUNTS", "") + "/vendor/etc/vintf/manifest.xml\x1e%s\x1e1\x1f" % m
        # 32-bit vendor daemons (profile "vendor32"): the image's generated
        # linker configuration for them alone, and no seccomp policy.
        v32 = self.profile.get("vendor32", {}).get(name)
        if v32 is not None:
            cfg = self.side_linkerconfig()
            if cfg:
                env["LD_CONFIG_FILE"] = cfg
            for guest_lib, src in v32.items():
                so = self.shim(src, os.path.basename(guest_lib))
                if so:      # one bind for this process: the image's library path -> the shim
                    env["LXRT_MOUNTS"] = env.get("LXRT_MOUNTS", "") + "%s\x1e%s\x1e1\x1f" % (guest_lib, so)
        # 64-bit stand-ins (profile "shims64"): an image library path bound to
        # a library built from scripts/android/shims, for that process only.
        for guest_lib, src in self.profile.get("shims64", {}).get(name, {}).items():
            so = self.shim(src, os.path.basename(guest_lib), arch="x86_64")
            if so:
                env["LXRT_MOUNTS"] = env.get("LXRT_MOUNTS", "") + "%s\x1e%s\x1e1\x1f" % (guest_lib, so)
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
        svc.proc = self.spawn(argv, env, out, pass_fds=fds, what="service " + name)
        for fd in fds:
            os.close(fd)
        svc.state = "running"
        svc.starts += 1
        svc.started_at = time.time()
        self.props.set("init.svc." + name, "running")
        if svc.starts == 1:
            self.props.set("ro.boottime." + name, str(int(time.monotonic() * 1e9)))
        log("started %s (pid %d, %s): %s%s [%s]" % (name, svc.proc.pid, why, " ".join(argv),
                                                    " (stand-in)" if stand_in else "", env["LXRT_ANDROID_IDS"]))
        return True

    def shim(self, src, soname, arch="i686"):
        """scripts/android/shims/<src> built for i686 (or x86_64) Android with
        Homebrew clang into build/android/shims (again when the source is
        newer). An x86_64 one links the image's libc, libdl and liblog.
        Its host path, or None."""
        c = os.path.join(HERE, "android", "shims", src)
        out = os.path.join(REPO, "build", "android", "shims", os.path.splitext(src)[0] + ".so")
        libs = []
        if arch == "x86_64":
            def in_root(guest):     # symbolic links inside the image (libc -> /apex/...)
                p = self.root + guest
                for _ in range(8):
                    if not os.path.islink(p):
                        break
                    t = os.readlink(p)
                    p = self.root + t if t.startswith("/") else os.path.join(os.path.dirname(p), t)
                return p
            libs = [in_root("/system/lib64/" + l) for l in ("libc.so", "libdl.so", "liblog.so")]
        try:
            if not os.path.exists(out) or os.path.getmtime(out) < os.path.getmtime(c):
                os.makedirs(os.path.dirname(out), exist_ok=True)
                subprocess.run(["/opt/homebrew/opt/llvm/bin/clang", "--target=%s-linux-android30" % arch, "-O2",
                                "-fPIC", "-shared", "-nostdlib", "-fuse-ld=lld",
                                "--ld-path=/opt/homebrew/opt/lld/bin/ld.lld",
                                "-Wl,-soname," + soname, "-Wl,-z,max-page-size=4096",
                                "-o", out, c] + libs, check=True, timeout=120,
                               stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        except (OSError, subprocess.SubprocessError) as e:
            log("shim %s: %s" % (src, e))
            return None
        return out

    def side_linkerconfig(self):
        """The image's linker configuration generated into the root's
        /data/local/tmp/steamarm-linkerconfig by its own linkerconfig (with
        the property service up), for the processes given it through
        LD_CONFIG_FILE, which the linker of this userdebug build honours.
        The root's own /linkerconfig stays as it is: rewritten there,
        audioserver exited at once in a loop and the boot never completed
        (MEASURED). Its guest path, or None."""
        if getattr(self, "_side_cfg", False) is not False:
            return self._side_cfg
        self._side_cfg = None
        target = "/data/local/tmp/steamarm-linkerconfig"
        os.makedirs(self.root + target, exist_ok=True)
        rc, _ = self.guest(["/system/bin/linkerconfig", "--target", target], timeout=120)
        cfg = target + "/ld.config.txt"
        if rc == 0 and os.path.isfile(self.root + cfg):
            self._side_cfg = cfg
        log("side linker configuration: %s" % (cfg if self._side_cfg else "none (linkerconfig exited %d)" % rc))
        return self._side_cfg

    def passthrough_manifest(self, hals):
        """A copy of the image's vendor VINTF manifest in the state directory
        with the named HIDL HALs' transport made passthrough. Its host path,
        or None."""
        src = self.root + "/vendor/etc/vintf/manifest.xml"
        try:
            text = open(src).read()
        except OSError as e:
            log("vintf: %s" % e)
            return None
        done = []

        def fix(m):
            block = m.group(0)
            nm = re.search(r"<name>\s*([^<\s]+)\s*</name>", block)
            if nm and nm.group(1) in hals and "<transport>hwbinder</transport>" in block:
                done.append(nm.group(1))
                return block.replace("<transport>hwbinder</transport>",
                                     '<transport arch="32+64">passthrough</transport>')
            return block
        text = re.sub(r'<hal format="hidl">.*?</hal>', fix, text, flags=re.S)
        out = os.path.join(self.state, "vintf-manifest.xml")
        with open(out, "w") as f:
            f.write(text)
        log("vintf: %s passthrough for hwservicemanager (%s)" % (", ".join(done) or "nothing", out))
        return out

    def standin_dex(self):
        """The stand-ins' dex (scripts/android/java), built with javac and D8
        (R8_JAR, as tests/android/run.sh builds its test dex) when missing or
        older than the source, and copied into the root. Its guest path, or
        None."""
        if getattr(self, "_standin", None):
            return self._standin
        pkgsrc = os.path.join(HERE, "android", "java", "org", "steamarm", "android")
        srcs = sorted(os.path.join(pkgsrc, f) for f in os.listdir(pkgsrc) if f.endswith(".java"))
        out = os.path.join(REPO, "build", "android", "standin")
        dex = os.path.join(out, "classes.dex")
        # R8_JAR, else the state dir's copy, else the default state dir's: a
        # session with a scratch STEAMARM_STATE (tests/android/run.sh) has no
        # tools/ of its own, and without the stand-ins netd never answers and
        # system_server's Watchdog kills it at NetworkManagementService.
        cands = [os.environ.get("R8_JAR")] + [
            os.path.join(d, "android/tools/r8-9.4.27.jar")
            for d in (os.environ.get("STEAMARM_STATE"), os.path.expanduser("~/SteamARM-roots")) if d]
        r8 = next((c for c in cands if c and os.path.exists(c)), cands[-1])
        try:
            if not os.path.exists(dex) or os.path.getmtime(dex) < max(os.path.getmtime(f) for f in srcs):
                cls = os.path.join(out, "classes")
                shutil.rmtree(cls, ignore_errors=True)
                os.makedirs(cls, exist_ok=True)
                subprocess.run(["javac", "--release", "8", "-nowarn", "-d", cls, "-sourcepath",
                                os.path.join(HERE, "android", "stubs")] + srcs, check=True, timeout=300)
                pkg = os.path.join(cls, "org", "steamarm", "android")
                subprocess.run(["java", "-cp", r8, "com.android.tools.r8.D8", "--min-api", "30", "--release",
                                "--output", out] + [os.path.join(pkg, f) for f in sorted(os.listdir(pkg))
                                                    if f.endswith(".class")],
                               check=True, timeout=300, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            guest = "/data/local/tmp/steamarm-standin.dex"
            os.makedirs(self.root + "/data/local/tmp", exist_ok=True)
            with open(dex, "rb") as f, open(self.root + guest, "wb") as g:
                g.write(f.read())
        except (OSError, subprocess.SubprocessError) as e:
            log("stand-in dex: %s (needs javac and %s)" % (e, r8))
            return None
        self._standin = guest
        return guest

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
        # init's `restart` (and ctl.restart): stop, then start again at once.
        # The state has to say so, or reap() never starts it again (a
        # gralloc exit's "onrestart restart surfaceflinger" left SurfaceFlinger
        # stopped for the rest of the boot, MEASURED at stage 28).
        svc.state = "restarting" if restart else "stopped"
        svc.restart_at = time.time() if restart else None
        self.props.set("init.svc." + name, "restarting" if restart else "stopped")
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
            # init's Service::Reap: KillProcessGroup(SIGKILL) -- what the
            # service forked dies with it. The zygote's system_server stays in
            # its group: left running after a zygote abort, it kept its
            # services registered, and every new system_server died on them
            # ("BinderProxy cannot be cast to PermissionManagerService", a
            # crash loop to the end of the boot, MEASURED).
            try:
                os.killpg(svc.proc.pid, signal.SIGKILL)
            except OSError:
                pass
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

    def init_user0(self):
        """init's init_user0 asks vold (vdc cryptfs init_user0) to prepare
        user 0's device-encrypted storage, and the framework later asks it
        for the credential-encrypted half (StorageManagerService.
        prepareUserStorage). There is no vold here and /data is not
        encrypted (mount_all above), so both are made now, with the modes and
        owners of vold's fscrypt_prepare_user_storage and vold_prepare_subdirs
        (system/vold FsCrypt.cpp, vold_prepare_subdirs.cpp, Android 11).
        Without them installd could create no app's data and SettingsProvider
        could not open its database: system_server died in
        installSystemProviders (MEASURED, stage 28)."""
        u = 0
        dirs = [
            # DE
            ("/data/system/users/%d" % u, 0o700, "system", "system"),
            ("/data/misc/profiles/cur/%d" % u, 0o771, "system", "system"),
            ("/data/system_de/%d" % u, 0o770, "system", "system"),
            ("/data/misc_de/%d" % u, 0o1771, "system", "misc"),
            ("/data/vendor_de/%d" % u, 0o771, "root", "root"),
            ("/data/user_de/%d" % u, 0o771, "system", "system"),
            # CE (for user 0 on the default volume the CE app directory is /data/data)
            ("/data/system_ce/%d" % u, 0o770, "system", "system"),
            ("/data/misc_ce/%d" % u, 0o1771, "system", "misc"),
            ("/data/vendor_ce/%d" % u, 0o771, "root", "root"),
            ("/data/media/%d" % u, 0o770, "media_rw", "media_rw"),
        ]
        for base in ("/data/misc_de/%d" % u, "/data/misc_ce/%d" % u):
            dirs += [(base + "/" + d, 0o700, "root", "root") for d in ("vold", "storaged", "rollback", "apexrollback")]
            dirs.append((base + "/apexdata", 0o711, "root", "root"))
            for apex in sorted(os.listdir(self.root + "/apex")) if os.path.isdir(self.root + "/apex") else []:
                if "@" not in apex:
                    dirs.append((base + "/apexdata/" + apex, 0o771, "root", "system"))
        dirs += [("/data/vendor_de/%d/fpdata" % u, 0o700, "system", "system"),
                 ("/data/vendor_de/%d/facedata" % u, 0o700, "system", "system"),
                 ("/data/system_ce/%d/backup" % u, 0o700, "system", "system"),
                 ("/data/system_ce/%d/backup_stage" % u, 0o700, "system", "system"),
                 ("/data/vendor_ce/%d/facedata" % u, 0o700, "system", "system")]
        for path, mode, user, group in dirs:
            hp = self.hpath(path)
            os.makedirs(hp, exist_ok=True)
            os.chmod(hp, mode)
            set_owner(hp, AID[user], AID[group])
        log("init_user0: user 0's storage prepared as vold would (%d directories)" % len(dirs))
        self.storage_layout()

    def storage_layout(self):
        """External storage the way vold and the zygote leave it, without
        FUSE: /storage a real directory (canonical paths must start with
        /storage/<volume>/<user>/ -- StorageManagerService.mkdirs refused
        every app's external files directory, "Invalid mkdirs path:
        /sdcard/Android/data/<package>/files", while /storage was a symlink
        to /mnt/runtime/default and /storage/emulated did not exist),
        /storage/emulated/0 and /mnt/user/0/emulated/0 bound to /data/media/0
        in every process's mount table (storage_mounts; the zygote binds
        /mnt/user/0 over /storage for each app), and self/primary pointing at
        /storage/emulated/0 in both, as vold's symlinks do."""
        st = self.hpath("/storage")
        if os.path.islink(st):
            os.unlink(st)
        for d in ("/storage", "/storage/emulated", "/storage/emulated/0", "/storage/self",
                  "/mnt/user/0", "/mnt/user/0/emulated", "/mnt/user/0/emulated/0", "/mnt/user/0/self"):
            os.makedirs(self.hpath(d), exist_ok=True)
        for link in ("/storage/self/primary", "/mnt/user/0/self/primary", "/mnt/user/0/primary"):
            hp = self.hpath(link)
            if os.path.islink(hp) and os.readlink(hp) == "/storage/emulated/0":
                continue
            if os.path.lexists(hp):
                os.unlink(hp)
            os.symlink("/storage/emulated/0", hp)
        media = self.hpath("/data/media/0")
        for d in ("Android", "Android/data", "Android/media", "Android/obb",
                  "Download", "Pictures", "Music", "Movies", "DCIM", "Documents"):
            p = os.path.join(media, d)
            os.makedirs(p, exist_ok=True)
            os.chmod(p, 0o777)
        os.chmod(media, 0o777)
        # Longest prefix wins in the runtime's table, so these hold whatever
        # the zygote binds over /storage for an app (/mnt/user/0 with FUSE,
        # /mnt/runtime/<mode> without).
        binds = [("/storage/emulated/0", media), ("/mnt/user/0/emulated/0", media),
                 ("/storage/emulated", self.hpath("/storage/emulated")),
                 ("/storage/self", self.hpath("/storage/self"))]
        self.storage_mounts = "".join("%s\x1e%s\x1e0\x1f" % b for b in binds)
        log("storage: /storage/emulated/0 is /data/media/0 for every process")

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
                opts = [t for t in a[1:] if "=" not in t]     # mode, owner, group (encryption=... ignored)
                mode = int(opts[0], 8) if opts and opts[0].isdigit() else 0o755
                hp = self.hpath(a[0])
                if os.path.islink(hp):
                    # /data/user/0 is a symlink here (the bind below); init's
                    # mkdir would not follow it and change /data/data's mode.
                    return
                existed = os.path.isdir(hp)
                os.makedirs(hp, exist_ok=True)
                # init: a new directory is 0755 root:root unless the line says
                # otherwise; an existing one changes only what the line names.
                # Guests with Android ids see the owner (virtual ownership).
                if opts or not existed:
                    os.chmod(hp, mode)
                if len(opts) > 1:
                    uid = self.uid_of(opts[1])
                    set_owner(hp, uid, self.uid_of(opts[2]) if len(opts) > 2 else uid)
                elif not existed:
                    set_owner(hp, 0, 0)
            elif c in ("chown", "chmod"):
                p = a[-1]
                if not self.data_path(p) or not os.path.lexists(self.hpath(p)):
                    return self.skip(cmd, "not /data, /mnt or /storage, or not there")
                if c == "chmod":
                    os.chmod(self.hpath(p), int(a[0], 8))
                else:
                    uid = self.uid_of(a[0])
                    gid = self.uid_of(a[1]) if len(a) > 2 else None
                    set_owner(self.hpath(p), uid, gid)
            elif c == "mount" and len(a) >= 4 and a[3] == "bind" and self.data_path(a[1]) and self.data_path(a[2]):
                # init.rc binds /data/data on /data/user/0. A bind is only per
                # process here (the runtime's bind table), so the boot does
                # what Android did before 11: /data/user/0 -> /data/data, a
                # symlink the runtime resolves inside the root. installd uses
                # /data/data for user 0 and the framework /data/user/0.
                src, dst = self.hpath(a[1]), self.hpath(a[2])
                if os.path.islink(dst):
                    return
                if os.path.isdir(dst) and not os.listdir(dst):
                    os.rmdir(dst)
                if not os.path.lexists(dst):
                    os.symlink(a[1], dst)
                    log("%s: bind %s on %s made a symlink" % (where, a[1], a[2]))
                else:
                    return self.skip(cmd, "target not empty")
            elif c == "init_user0":
                self.init_user0()
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
            elif c == "update_linker_config" and self.a.linkerconfig:
                # init's builtin (early-init, after the bootstrap APEXes): the
                # image's linkerconfig, now with the property service up, so
                # it writes the full layout (VNDK, statsd's APEX) instead of
                # the "legacy" one written before properties existed. The
                # 32-bit audio HAL needs it: "library android.hardware.audio@
                # 4.0.so not found ... in namespace (default)" with the legacy
                # file (MEASURED, benchmarks/stage28-android-reliability.txt).
                # It rewrites the root's /linkerconfig, which every guest of
                # the root reads: opt-in (--linkerconfig).
                rc, _ = self.guest(["/system/bin/linkerconfig", "--target", "/linkerconfig"], timeout=120)
                try:
                    size = os.path.getsize(self.hpath("/linkerconfig/ld.config.txt"))
                except OSError:
                    size = -1
                log("update_linker_config: linkerconfig exited %d, /linkerconfig/ld.config.txt %d bytes" % (rc, size))
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
                    if x == "sys.boot_completed" and y == "1":
                        for n, why in list(self.deferred.items()):
                            del self.deferred[n]
                            self.start(n, why + ", after boot")
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
        os.makedirs(self.inputdir, mode=0o700, exist_ok=True)
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
            self.start_lmkd_socket()
            self.start_dnsproxy()
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
            if self.a.exit_with and time.time() >= getattr(self, "_next_parent_check", 0):
                self._next_parent_check = time.time() + 1
                try:
                    os.kill(self.a.exit_with, 0)
                except ProcessLookupError:
                    log("--exit-with %d: that process is gone" % self.a.exit_with)
                    return
                except PermissionError:
                    pass
            time.sleep(0.1)

    def shutdown(self):
        log("stopping (lmkd stand-in read %d commands; dnsproxyd answered %s)" %
            (getattr(self, "lmkd_msgs", 0), getattr(self, "dns_counts", None) or "nothing"))
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
        self.stop_fexserver()
        log("stopped %d processes" % len(mine))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=os.environ.get("ANDROID_X86_ROOT", "/Volumes/SteamARMAndroid/root-x86_64"))
    ap.add_argument("--lxrun", default=os.environ.get("LXRUN", os.path.join(REPO, "build/lxrun")))
    ap.add_argument("--state", default=None, help="private state directory (default /tmp/lxrt-android-<uid>-<hash>)")
    ap.add_argument("--seconds", type=int, default=300)
    ap.add_argument("--until-prop", default=None, help="NAME=VALUE: stop once the property has the value")
    ap.add_argument("--keep-running", action="store_true")
    ap.add_argument("--exit-with", type=int, default=0, metavar="PID",
                    help="stop the boot when this process is gone (scripts/android-session.py run)")
    ap.add_argument("--profile", default="headless", choices=sorted(PROFILES))
    ap.add_argument("--all", action="store_true", help="start every service the actions ask for")
    ap.add_argument("--also", action="append", help="start this service too (outside the profile)")
    ap.add_argument("--no-zygote", action="store_true")
    ap.add_argument("--linkerconfig", action="store_true",
                    help="run init's update_linker_config: rewrite the root's /linkerconfig with properties up")
    ap.add_argument("--abi32", action="store_true",
                    help="the display profile with the image's 32-bit ABI and zygote_secondary (needs a FEX with "
                         "patches/fex-lxrt-i386-bionic.patch; the headless profile does this whenever it can)")
    ap.add_argument("--zygote-stdio", action="store_true", help="the zygote's stdio to its log too (it then refuses to fork)")
    ap.add_argument("--bootargs", default=os.environ.get("LXRT_PROPERTY_BOOTARGS", ""))
    ap.add_argument("--persist", default=None, help="persistent property file (default: the root's /data/property)")
    ap.add_argument("--watch", action="append", help="log changes of this property")
    ap.add_argument("--prop", action="append", help="NAME=VALUE set before the first action")
    ap.add_argument("--pulse", default=None, metavar="GUEST_DIR",
                    help="the directory of a PulseAudio socket 'native' in the root for the audio HAL "
                         "(scripts/audio.sh with LXRT_ROOT=<root>: /tmp/pulse)")
    ap.add_argument("--wayland", default=None, metavar="XDG[:SOCKET]",
                    help="the Wayland compositor for hwcomposer.waydroid: its XDG_RUNTIME_DIR (a guest "
                         "path under /dev/shm, scripts/run-weston.sh) and socket (wayland-0); sets "
                         "Waydroid's host properties and selects the display profile")
    ap.add_argument("--trace", action="append", help="LXRT_TRACE for this service (<state>/svc/NAME.trace)")
    ap.add_argument("--svc-env", action="append", help="SERVICE:VAR=VALUE (SERVICE 'all' for every one)")
    a = ap.parse_args()
    if a.wayland:
        # What Waydroid's container manager writes into waydroid.prop for the
        # composer (hwcomposer.cpp: XDG_RUNTIME_DIR/WAYLAND_DISPLAY from these,
        # one window with the whole screen, the window made at open).
        xdg, _, sock = a.wayland.partition(":")
        # Defaults; a --prop of the same name wins (a session in Waydroid's
        # multi-window mode sets waydroid.active_apps=none: with "Waydroid"
        # the composer puts the whole desktop in one window).
        given = {x.split("=", 1)[0] for x in (a.prop or [])}
        a.prop = (a.prop or []) + [kv for kv in ["waydroid.xdg_runtime_dir=" + xdg,
                                                  "waydroid.wayland_display=" + (sock or "wayland-0"),
                                                  "waydroid.active_apps=Waydroid",
                                                  "waydroid.background_start=false"]
                                   if kv.split("=", 1)[0] not in given]
        if a.profile == "headless":
            a.profile = "display"
    if a.pulse:
        # Waydroid's audio HAL points libpulse at waydroid.pulse_runtime_path
        # (audio.primary.waydroid: PULSE_RUNTIME_PATH, default /run/user/1000/
        # pulse); scripts/audio.sh gives each root a socket on SteamARM's
        # PulseAudio at <root>/tmp/pulse/native.
        a.prop = (a.prop or []) + ["waydroid.pulse_runtime_path=" + a.pulse]
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
