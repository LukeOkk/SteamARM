#!/usr/bin/env python3
"""The launcher's settings.json as the environment of the programs SteamARM
starts (launcher/SETTINGS_SPEC.md). run-app.sh imports env_from_settings() and
with_overrides(); run-steam.sh evaluates --shell.

  scripts/settings-env.py --shell [settings.json]   export lines for a shell
  scripts/settings-env.py --json  [settings.json]   the environment as JSON
  scripts/settings-env.py --volume [settings.json]  output volume, empty when muted

Also writes $STATE/launcher/limits.env (the memory ceiling the guard reads).
Games inherit Steam's environment, so a change reaches them when Steam starts.
"""
import json
import mmap
import os
import shlex
import subprocess
import sys

STATE = os.environ.get("STEAMARM_STATE") or os.path.expanduser("~/SteamARM-roots")
SHIM = os.path.join(STATE, "steamroot", "usr", "lib", "lxrt-emu", "libvulkan.so.1")
ICD_MARKER = b"STEAMARM_VK_ICD"
OVERRIDABLE = ("display", "vsync", "synchronization", "graphicsBackend")


def effective_synchronization(value):
    """The backend Proton actually gets. Mirrors RuntimeCapabilities.effectiveSynchronization
    (launcher/ApplicationCore.swift): fsync needs futex_waitv, which lxrun lacks (ENOSYS);
    no MSync-capable Wine exists here; esync is experimental (Proton 10.0 only) so AUTO does
    not pick it. Anything else is Wine's default, wineserver."""
    return "esync" if value == "esync" else "wineserver"


def file_contains(path, needle):
    """Search an installed binary without reading it into memory."""
    try:
        with open(path, "rb") as source:
            if os.fstat(source.fileno()).st_size == 0:
                return False
            with mmap.mmap(source.fileno(), 0, access=mmap.ACCESS_READ) as data:
                return data.find(needle) != -1
    except (OSError, ValueError):
        return False


def shim_selects_icd(path=None):
    """True when the installed Vulkan shim reads STEAMARM_VK_ICD (it can load an ICD such as
    KosmicKrisp); an older shim only knows MoltenVK and ignores the variable."""
    return file_contains(path or SHIM, ICD_MARKER)


def with_overrides(s, overrides):
    """Settings with an app's own choices on top (AppEntry.overrides). Only keys in
    OVERRIDABLE and non-empty strings count; other values are ignored."""
    merged = dict(s)
    if isinstance(overrides, dict):
        merged.update({key: value for key, value in overrides.items()
                       if key in OVERRIDABLE and isinstance(value, str) and value})
    return merged


def total_ram_gb():
    try:
        out = subprocess.run(["/usr/sbin/sysctl", "-n", "hw.memsize"],
                             capture_output=True, text=True, check=True).stdout
        return int(out) // (1 << 30)
    except (OSError, ValueError, subprocess.CalledProcessError):
        return 8


def usable_gb(total=None):
    """The top of the DRAM/VRAM lists: T-2 up to 8 GB, T-4 above."""
    t = total if total is not None else total_ram_gb()
    return max(2, t - 2 if t <= 8 else t - 4)


def host_timezone():
    # /etc/localtime -> /var/db/timezone/zoneinfo/<Area>/<City>
    try:
        link = os.readlink("/etc/localtime")
    except OSError:
        return None
    marker = "zoneinfo/"
    i = link.find(marker)
    return link[i + len(marker):] if i >= 0 else None


def env_from_settings(s, total=None):
    env = {}
    dxvk = []

    # Sistema
    lang = s.get("guestLanguage") or "auto"
    if lang != "auto":
        env["LANG"] = lang
        env["LC_ALL"] = lang
    tz = s.get("timezone") or "auto"
    tz = host_timezone() if tz == "auto" else tz
    if tz:
        env["TZ"] = tz
    vsync = s.get("vsync") or "game"
    if vsync in ("on", "off"):
        n = "1" if vsync == "on" else "0"
        dxvk += ["dxgi.syncInterval = %s" % n, "d3d9.presentInterval = %s" % n]
        # vkd3d-proton compares with strcmp against FIFO, IMMEDIATE, ...: lowercase
        # is ignored. Proton 10.0's vkd3d has no such variable.
        env["VKD3D_SWAPCHAIN_PRESENT_MODE"] = "FIFO" if vsync == "on" else "IMMEDIATE"
    top = usable_gb(total)
    vram = int(s.get("vramGB") or 0) or top
    vram = min(vram, top)
    env["LXRT_VK_MAX_VRAM_MB"] = str(vram * 1024)
    dxvk.append("dxgi.maxDeviceMemory = %d" % (vram * 1024))
    sync = s.get("synchronization")
    if sync is None and ("esync" in s or "fsync" in s):
        # settings.json written before the selector: the two booleans, exactly as before.
        if s.get("esync") is False:
            env["PROTON_NO_ESYNC"] = "1"
        if s.get("fsync") is False:
            env["PROTON_NO_FSYNC"] = "1"
    else:
        if effective_synchronization(sync) != "esync":
            env["PROTON_NO_ESYNC"] = "1"
        env["PROTON_NO_FSYNC"] = "1"   # fsync cannot work under lxrun

    # Procesador (FEX reads FEX_<OPTION>)
    if s.get("fexDiskCache"):
        env["FEX_DISKCACHE"] = "1"
    tso = s.get("fexTSO") or "full"
    if tso == "off":
        env["FEX_TSOENABLED"] = "0"
    elif tso == "fast":
        env["FEX_TSOENABLED"] = "1"
        env["FEX_VECTORTSOENABLED"] = "0"
        env["FEX_MEMCPYSETTSOENABLED"] = "0"
    if s.get("fexMultiblock") is False:
        env["FEX_MULTIBLOCK"] = "0"
    smc = {"none": "0", "mtrack": "1", "full": "2"}.get(s.get("fexSMC") or "mtrack")
    if smc and smc != "1":
        env["FEX_SMCCHECKS"] = smc
    if s.get("fexX87Reduced"):
        env["FEX_X87REDUCEDPRECISION"] = "1"

    # Gráficos. The shim loads MoltenVK unless STEAMARM_VK_ICD names another
    # driver; only a shim that reads the variable gets it. AUTO is MoltenVK,
    # and WineD3D is never set (guest GL is software llvmpipe, no GL thunk).
    if (s.get("graphicsBackend") or "auto") == "vulkanKosmicKrisp" and shim_selects_icd():
        env["STEAMARM_VK_ICD"] = "kosmickrisp"
    if s.get("shaderCache") is False:
        env["DXVK_SHADER_CACHE"] = "0"
        env["VKD3D_SHADER_CACHE_PATH"] = "0"
    aniso = int(s.get("anisotropy") or 0)
    if aniso in (2, 4, 8, 16):
        dxvk += ["d3d11.samplerAnisotropy = %d" % aniso, "d3d9.samplerAnisotropy = %d" % aniso]
    fps = int(s.get("frameRateLimit") or 0)
    if fps > 0:
        dxvk += ["dxgi.maxFrameRate = %d" % fps, "d3d9.maxFrameRate = %d" % fps]
        env["VKD3D_FRAME_RATE"] = str(fps)
    hud = s.get("dxvkHud") or "off"
    if hud == "fps":
        env["DXVK_HUD"] = "fps"
    elif hud == "full":
        env["DXVK_HUD"] = "full"
    if s.get("metalHud"):
        env["MTL_HUD_ENABLED"] = "1"
    if dxvk:
        env["DXVK_CONFIG"] = "; ".join(dxvk)

    # Sonido
    if (s.get("audioBackend") or "coreaudio") == "none":
        env["PULSE_SERVER"] = "none"
        env["SDL_AUDIODRIVER"] = "dummy"
    else:
        # scripts/audio.sh: PulseAudio on the Mac, socket inside the guest root.
        env["PULSE_SERVER"] = "unix:/tmp/pulse/native"

    # Registros / Depuración
    if s.get("protonLog"):
        env["PROTON_LOG"] = "1"
    if s.get("wineDebug"):
        env["WINEDEBUG"] = str(s["wineDebug"])
    lvl = s.get("dxvkLogLevel") or "warn"
    if lvl != "warn":
        env["DXVK_LOG_LEVEL"] = lvl
    v = s.get("vkd3dLogLevel") or "err"
    if v != "err":
        env["VKD3D_DEBUG"] = v
    if s.get("guestFaults", True):
        env["LXRT_GUEST_FAULTS"] = "1"
    if s.get("traceMatch"):
        env["LXRT_TRACE_MATCH"] = str(s["traceMatch"])
    if s.get("vulkanDebug"):
        env["LXRT_VK_DEBUG"] = "1"

    # Last word to the user's own variables.
    for k, val in (s.get("extraEnv") or {}).items():
        env[str(k)] = str(val)
    return env


def audio_volume(s):
    """0-100, or None when sound is off."""
    if (s.get("audioBackend") or "coreaudio") == "none":
        return None
    try:
        return max(0, min(100, int(s.get("volume", 100))))
    except (TypeError, ValueError):
        return 100


def write_limits(s, total=None):
    top = usable_gb(total)
    dram = int(s.get("dramGB") or 0) or top
    dram = min(dram, top)
    d = os.path.join(STATE, "launcher")
    os.makedirs(d, exist_ok=True)
    tmp = os.path.join(d, "limits.env.tmp")
    with open(tmp, "w") as f:
        f.write("GUEST_MAX_MB=%d\n" % (dram * 1024))
    os.replace(tmp, os.path.join(d, "limits.env"))


def load(path):
    try:
        with open(path) as f:
            s = json.load(f)
        return s if isinstance(s, dict) else {}
    except (OSError, ValueError):
        return {}


def main(argv):
    mode = argv[1] if len(argv) > 1 else "--shell"
    path = argv[2] if len(argv) > 2 else os.path.join(STATE, "launcher", "settings.json")
    s = load(path)
    env = env_from_settings(s)
    write_limits(s)
    if mode == "--volume":
        v = audio_volume(s)
        print("" if v is None else v)
        return
    if mode == "--json":
        print(json.dumps(env, indent=1, sort_keys=True))
    else:
        for k, v in sorted(env.items()):
            print("export %s=%s" % (k, shlex.quote(v)))


if __name__ == "__main__":
    main(sys.argv)
