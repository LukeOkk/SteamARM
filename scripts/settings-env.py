#!/usr/bin/env python3
"""The launcher's settings.json as the environment of the programs SteamARM
starts (launcher/SETTINGS_SPEC.md). run-app.sh imports env_from_settings() and
with_overrides(); run-steam.sh evaluates --shell.

  scripts/settings-env.py --shell [settings.json]   export lines for a shell
  scripts/settings-env.py --json  [settings.json]   the environment as JSON
  scripts/settings-env.py --volume [settings.json]  output volume, empty when muted
  scripts/settings-env.py --emulate-modeset [settings.json]  "on" or "off" (Escala de resolución)
  scripts/settings-env.py --upscaling        this Mac's upscaling recommendation as JSON

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
OVERRIDABLE = ("display", "vsync", "synchronization", "graphicsBackend", "scalingFilter")
SCALING_FILTERS = ("auto", "linear", "nearest", "fsr", "metalfx", "metalfx-temporal")
STEAM_UI_GPU_SWITCHES = ("--use-gl=angle", "--use-angle=vulkan",
                         "--enable-features=Vulkan,VulkanFromANGLE,DefaultANGLEVulkan",
                         "--ignore-gpu-blocklist")


ZINK_GL45_OVERRIDE = "+GL_ARB_vertex_type_2_10_10_10_rev +GL_ARB_texture_buffer_object_rgb32"


def effective_synchronization(value):
    """The backend Proton actually gets. Mirrors RuntimeCapabilities.effectiveSynchronization
    (launcher/ApplicationCore.swift): esync, fsync (futex_waitv, runtime/futex_waitv.c) and
    ntsync (/dev/ntsync, runtime/ntsync.c)
    are experimental, so only an explicit choice gets them and AUTO does not; no
    MSync-capable Wine exists here. Anything else is Wine's default, wineserver."""
    return value if value in ("esync", "fsync", "ntsync") else "wineserver"


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


def kosmickrisp_build_dir(build=None):
    """The directory of SteamARM's own KosmicKrisp build
    (scripts/build-kosmickrisp.sh), or "" if there is none."""
    root = build or os.environ.get("STEAMARM_BUILD") or os.path.expanduser("~/SteamARM-build")
    out = os.path.join(root, "mesa-kk", "out")
    return out if os.path.isfile(os.path.join(out, "libvulkan_kosmickrisp.dylib")) else ""


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


# Set by the launcher (FallbackPolicy.environment, launcher/ApplicationCore.swift)
# when its fallback policy lets a launch go ahead without a setting that cannot
# work on this Mac; run-app.sh puts them over the settings, so the fallback the
# launcher announced is the one that runs. (The display's, STEAMARM_DISPLAY,
# run-app.sh reads itself.)
FALLBACK_VARIABLES = {"STEAMARM_SYNCHRONIZATION": "synchronization",
                      "STEAMARM_GRAPHICS_BACKEND": "graphicsBackend"}


def fallback_overrides(environ):
    """The launcher's fallbacks in `environ`, as overrides for with_overrides()."""
    return {key: environ[name] for name, key in FALLBACK_VARIABLES.items() if environ.get(name)}


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


# Upscaling, "Filtro de escalado" AUTO (the default): the Vulkan shim takes
# Apple's MetalFX spatial scaler where the driver exports its Metal objects
# (MoltenVK; SteamARM's KosmicKrisp from patches/kosmickrisp-10 on) and FSR
# 1.0 where it does not (LXRT_VK_SCALER=auto, shim/scaler.c). MetalFX
# temporal needs depth, motion vectors and the camera jitter of the 3D scene,
# which a picture handed to vkQueuePresentKHR does not have: spatial only.
# What depends on the chip is how far below the display a game should render
# for the shim to enlarge: the largest of FSR 1.0's scales (render size per
# axis) that the GPU's budget allows, as STEAMARM_RENDER_SCALE and
# STEAMARM_RENDER_SIZE (the launcher shows them; in Counter-Strike 2, the
# resolution to choose).
RENDER_SCALES = ((1.0, "nativa"), (0.77, "ultra calidad"), (0.67, "calidad"),
                 (0.59, "equilibrado"), (0.5, "rendimiento"))
# Relative GPU throughput per core by generation (M4 = 1). M1-M3 from the
# public per-core results of the Metal benchmarks, rounded; an assumption,
# not measured here.
GPU_CORE_SPEED = {1: 0.70, 2: 0.80, 3: 0.90, 4: 1.00}
# Pixels per frame one M4 GPU core shades at a game's highest settings and
# the rate it is played at, calibrated on this machine's one data point:
# Counter-Strike 2, everything at its highest with 4x MSAA, 1920x1080, M4 with
# 10 GPU cores, 33-40 frames a second, GPU-bound (benchmarks/stage53): 10
# cores x 93,000 = 0.67^2 x 1920x1080, the "calidad" scale.
PIXELS_PER_CORE = 93000
# Resolutions games list (16:9 and 16:10): the render size is the largest of
# these with the display's shape that fits the scale, so that it can be
# picked in a game's menu.
COMMON_MODES = ((3840, 2160), (3200, 1800), (2880, 1620), (2560, 1440), (2304, 1296), (2048, 1152),
                (1920, 1080), (1600, 900), (1366, 768), (1280, 720), (1152, 648), (1024, 576), (960, 540),
                (854, 480), (640, 360), (2560, 1600), (2304, 1440), (1920, 1200), (1680, 1050), (1440, 900),
                (1280, 800), (1152, 720), (1024, 640), (960, 600))


def render_size(display, scale):
    """The resolution to render at for `scale` of `display` (w, h)."""
    w, h = display
    if scale >= 1.0:
        return [w, h]
    rw, rh = w * scale, h * scale
    shape = float(w) / h
    fits = [m for m in COMMON_MODES
            if abs(float(m[0]) / m[1] - shape) <= 0.01 * shape and m[0] <= rw * 1.02 and m[0] < w]
    if fits:
        return list(max(fits))
    return [max(2, int(round(rw / 2.0)) * 2), max(2, int(round(rh / 2.0)) * 2)]


def _run(argv):
    try:
        return subprocess.run(argv, capture_output=True, text=True, check=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def apple_chip(brand=None, gpu_cores=None):
    """This Mac's chip: {"brand", "generation", "tier", "gpu_cores", "p_cores",
    "e_cores"}, or None where it is not Apple silicon. `brand` and `gpu_cores`
    override what sysctl(8) and the IORegistry say (tests)."""
    if brand is None:
        brand = _run(["/usr/sbin/sysctl", "-n", "machdep.cpu.brand_string"]).strip()
    words = brand.split()
    if len(words) < 2 or words[0] != "Apple" or not words[1].startswith("M") or not words[1][1:].isdigit():
        return None
    tier = words[2] if len(words) > 2 and words[2] in ("Pro", "Max", "Ultra") else "base"
    if gpu_cores is None:
        # AGXAccelerator's "gpu-core-count" (MEASURED on the M4: 10, the
        # same as GPUConfigurationVariable num_cores).
        for line in _run(["/usr/sbin/ioreg", "-r", "-c", "AGXAccelerator", "-d", "1"]).splitlines():
            if '"gpu-core-count"' in line:
                try:
                    gpu_cores = int(line.split("=")[1])
                except (IndexError, ValueError):
                    pass
                break
    if not gpu_cores:
        # The smallest GPU of the tier, where the IORegistry does not say.
        gpu_cores = {"base": 7, "Pro": 14, "Max": 24, "Ultra": 48}[tier]

    def cores(level):
        out = _run(["/usr/sbin/sysctl", "-n", "hw.perflevel%d.physicalcpu" % level]).strip()
        return int(out) if out.isdigit() else 0
    return {"brand": brand, "generation": int(words[1][1:]), "tier": tier, "gpu_cores": int(gpu_cores),
            "p_cores": cores(0), "e_cores": cores(1)}


def main_display_size():
    """The main display's size in the units the X screen has (CoreGraphics'
    points: 1920x1080 on the test display, MEASURED), or None."""
    try:
        import ctypes
        cg = ctypes.CDLL("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics")
        cg.CGMainDisplayID.restype = ctypes.c_uint32
        for f in (cg.CGDisplayPixelsWide, cg.CGDisplayPixelsHigh):
            f.restype = ctypes.c_size_t
            f.argtypes = [ctypes.c_uint32]
        d = cg.CGMainDisplayID()
        w, h = int(cg.CGDisplayPixelsWide(d)), int(cg.CGDisplayPixelsHigh(d))
        return (w, h) if w > 0 and h > 0 else None
    except (OSError, AttributeError, ValueError):
        return None


def upscaling_policy(chip, display=None):
    """What AUTO means on `chip` (apple_chip()) for a display of `display`
    (w, h; default 1920x1080): {"filter", "scale", "preset", "render", ...}."""
    w, h = display or (1920, 1080)
    if chip is None:
        return {"filter": "fsr", "scale": 1.0, "preset": "nativa", "render": [w, h], "display": [w, h],
                "chip": None, "reason": "no Apple silicon: no MetalFX"}
    speed = GPU_CORE_SPEED.get(chip["generation"], max(GPU_CORE_SPEED.values()))
    ideal = (chip["gpu_cores"] * speed * PIXELS_PER_CORE / float(w * h)) ** 0.5
    # The largest scale the budget allows (a hundredth of slack: 0.6697
    # is "calidad"); "rendimiento" below that.
    scale, preset = RENDER_SCALES[-1]
    for value, name in RENDER_SCALES:
        if ideal + 0.01 >= value:
            scale, preset = value, name
            break
    return {"filter": "metalfx", "scale": scale, "preset": preset, "render": render_size((w, h), scale),
            "display": [w, h],
            "chip": "%s, GPU de %d núcleos" % (chip["brand"], chip["gpu_cores"]),
            "reason": "MetalFX espacial (temporal: imposible al presentar)"}


def host_timezone():
    # /etc/localtime -> /var/db/timezone/zoneinfo/<Area>/<City>
    try:
        link = os.readlink("/etc/localtime")
    except OSError:
        return None
    marker = "zoneinfo/"
    i = link.find(marker)
    return link[i + len(marker):] if i >= 0 else None


def env_from_settings(s, total=None, chip=None, display=None):
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
        env["PROTON_NO_FSYNC"] = "1"   # what these settings always ran with: fsync never worked then
    else:
        eff = effective_synchronization(sync)
        if eff != "esync":
            env["PROTON_NO_ESYNC"] = "1"
        # Proton turns fsync on by itself once futex_waitv answers: only an
        # explicit choice may get it while it is experimental.
        if eff != "fsync":
            env["PROTON_NO_FSYNC"] = "1"
        # /dev/ntsync exists only when asked for (runtime/ntsync.c): Proton
        # Experimental and Hotfix use it whenever it opens.
        if eff == "ntsync":
            env["LXRT_NTSYNC"] = "1"

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
    # driver; only a shim that reads the variable gets it. AUTO is MoltenVK.
    # WineD3D renders with OpenGL from Mesa's Zink on the same Vulkan thunk,
    # on MoltenVK (KosmicKrisp 26.2.3 cannot compile Zink's shaders to MSL);
    # the launcher only offers it where Zink and direct GLX exist
    # (benchmarks/stage38-opengl-zink.txt).
    gfx = s.get("graphicsBackend") or "auto"
    # Zink on MoltenVK stops at OpenGL 3.2 for want of two vertex/texel
    # formats Metal does not have (10:10:10:2 non-normalized vertex
    # attributes, RGB32 texel buffers); announcing the two extensions gives
    # OpenGL 4.5, and WineD3D cannot make its context current below 3.3
    # (MEASURED, benchmarks/stage39-wined3d-gl45.txt). Only programs that
    # use those two formats see a difference.
    if gfx == "vulkanKosmicKrisp" and shim_selects_icd():
        env["STEAMARM_VK_ICD"] = "kosmickrisp"
        # SteamARM's patched build, when scripts/build-kosmickrisp.sh made
        # one: the shim loads it instead of Homebrew's (Source 2 needs the
        # patches, benchmarks/stage51-cs2-linux-native.txt).
        own = kosmickrisp_build_dir()
        if own:
            env["STEAMARM_KK_DIR"] = own
    elif gfx == "openGLWineD3D":
        env["PROTON_USE_WINED3D"] = "1"
        env["GALLIUM_DRIVER"] = "zink"
        env["MESA_EXTENSION_OVERRIDE"] = ZINK_GL45_OVERRIDE
    if s.get("shaderCache") is False:
        env["DXVK_SHADER_CACHE"] = "0"
        env["VKD3D_SHADER_CACHE_PATH"] = "0"
    # MSAA on D3D9 swapchains (DXVK's d3d9.forceSwapchainMSAA): MEASURED on
    # MoltenVK, the D3D9 probe runs with 4x and 8x (benchmarks/stage42).
    msaa = int(s.get("antialiasing") or 0)
    if msaa in (2, 4, 8):
        dxvk.append("d3d9.forceSwapchainMSAA = %d" % msaa)
    # "Escala de resolución": Wine's emulated display modes (the prefixes'
    # EmulateModeset, wine-prefix-options.py) and the Vulkan shim enlarging
    # the game, not Proton's fullscreen hack: on MoltenVK that one made its
    # swapchain 1x1, then drew nothing into it (MEASURED, benchmarks/stage44).
    # The shim shows the game on its full-screen toplevel (shim/wsi.c).
    if s.get("resolutionScaling"):
        env["WINE_DISABLE_FULLSCREEN_HACK"] = "1"
        env["LXRT_VK_PADDED_FULLSCREEN"] = "1"
    # A picture smaller than its window: the Vulkan shim's scaling pass
    # (shim/scaler.c). "linear" is MoltenVK's own stretch; AUTO (the
    # default) is MetalFX where the driver can, FSR 1.0 where not, and the
    # render scale this chip is recommended (upscaling_policy).
    # MEASURED: tests/win/run.sh modeset_* (benchmarks/stage44).
    flt = s.get("scalingFilter") or "auto"
    if flt in SCALING_FILTERS and flt != "linear":
        env["LXRT_VK_SCALER"] = flt
    # "Escala de render": games whose window covers the screen render at this
    # fraction of it (the game tool passes STEAMARM_RENDER_SCALE to the game
    # only as LXRT_VK_RENDER_SCALE; shim/wsi.c render_scale) and the shim
    # enlarges their picture with the filter above. AUTO (the default): the
    # chip's recommendation (upscaling_policy).
    rs = str(s.get("renderScale") or "auto")
    if rs == "auto":
        disp = display or main_display_size()
        pol = upscaling_policy(chip if chip is not None else apple_chip(), disp)
        scale = pol["scale"]
        # The policy snaps to a common mode (1280x720 for 0.67 at 1080p):
        # the exact ratio, so that the shim lands on that size.
        if disp and pol.get("render") and pol["render"][0] and scale < 0.995:
            scale = pol["render"][0] / float(disp[0])
    else:
        try:
            scale = float(rs)
        except ValueError:
            scale = 1.0
        # Down to a third: MetalFX enlarges up to 3x ("rendimiento máximo";
        # its temporal scaler reports 1.0-3.0 on an M4).
        scale = 1.0 if not 0.33 <= scale < 0.995 else scale
    if scale >= 0.995 and flt == "metalfx-temporal":
        # 100 % with MetalFX temporal: its temporal pass at the native size,
        # as antialiasing (the game tool passes it to the game only).
        env["STEAMARM_MFX_NATIVE"] = "1"
    if scale < 0.995:
        disp = display or main_display_size()
        env["STEAMARM_RENDER_SCALE"] = "%.4f" % scale
        if disp:
            env["STEAMARM_RENDER_SIZE"] = "%dx%d" % (int(disp[0] * scale + 0.5) & ~1, int(disp[1] * scale + 0.5) & ~1)
        if flt == "fsr":
            try:
                sharp = int(s.get("fsrSharpness", 90))
            except (TypeError, ValueError):
                sharp = 90
            env["LXRT_VK_FSR_SHARPNESS"] = str(max(0, min(100, sharp)))
    # The ARM64 clients' web helper (Chromium) on ANGLE over Vulkan: the
    # Steam Frame root has only indirect GLX, its GPU process could not start
    # GL ES and Steam fell back to software ("Disabling GPU acceleration").
    # With these switches the GPU process stays up (MEASURED, stage 46).
    # runtime/process.c appends them when steamwebhelper is executed.
    if s.get("steamUIAcceleration", True):
        # The browser process only (no --type=): Chromium passes the
        # switches on to its GPU process itself.
        env["LXRT_EXEC_ARGS"] = ("steamwebhelper!--type=:" + " ".join(STEAM_UI_GPU_SWITCHES))
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
        env["SDL_AUDIODRIVER"] = "dummy"          # SDL 2
        env["SDL_AUDIO_DRIVER"] = "dummy"         # SDL 3
    else:
        # scripts/audio.sh: PulseAudio on the Mac, socket inside the guest root.
        env["PULSE_SERVER"] = "unix:/tmp/pulse/native"
        # scripts/pipewire.sh: the ARM64 Steam client's PipeWire (its audio
        # settings and voice), with tunnels to that same PulseAudio server.
        # SDL prefers PipeWire when it finds one; games stay on PulseAudio,
        # the direct way, as before there was a PipeWire to find.
        env["PIPEWIRE_RUNTIME_DIR"] = "/tmp/steamarm-pipewire"
        env["SDL_AUDIO_DRIVER"] = "pulseaudio"     # SDL 3
        env["SDL_AUDIODRIVER"] = "pulseaudio"      # SDL 2

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
    if mode == "--upscaling":
        # Answers only, as --emulate-modeset.
        print(json.dumps(upscaling_policy(apple_chip(), main_display_size()), indent=1, sort_keys=True,
                         ensure_ascii=False))
        return
    if mode == "--emulate-modeset":
        # "Escala de resolución" (scripts/wine-prefix-options.py): on or off.
        # Answers only: no limits file is written for this question.
        print("on" if s.get("resolutionScaling") else "off")
        return
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
