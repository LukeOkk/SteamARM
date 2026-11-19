use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::env;
use std::fs::{self, File};
use std::io::{self, Read};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const OVERRIDABLE: &[&str] = &[
    "display",
    "vsync",
    "synchronization",
    "graphicsBackend",
    "scalingFilter",
];
const SCALING_FILTERS: &[&str] = &[
    "auto",
    "linear",
    "nearest",
    "fsr",
    "metalfx",
    "metalfx-temporal",
];
const STEAM_UI_GPU_SWITCHES: &[&str] = &[
    "--use-gl=angle",
    "--use-angle=vulkan",
    "--enable-features=Vulkan,VulkanFromANGLE,DefaultANGLEVulkan",
    "--ignore-gpu-blocklist",
];
const STEAM_UI_SOFTWARE_SWITCHES: &[&str] = &["--disable-gpu"];
const ZINK_GL45_OVERRIDE: &str =
    "+GL_ARB_vertex_type_2_10_10_10_rev +GL_ARB_texture_buffer_object_rgb32";
const RENDER_SCALES: &[(f64, &str)] = &[
    (1.0, "nativa"),
    (0.77, "ultra calidad"),
    (0.67, "calidad"),
    (0.59, "equilibrado"),
    (0.5, "rendimiento"),
];
const GPU_CORE_SPEED: &[(u64, f64)] = &[(1, 0.70), (2, 0.80), (3, 0.90), (4, 1.00)];
const PIXELS_PER_CORE: f64 = 72_000.0;
const COMMON_MODES: &[(u32, u32)] = &[
    (3840, 2160),
    (3200, 1800),
    (2880, 1620),
    (2560, 1440),
    (2304, 1296),
    (2048, 1152),
    (1920, 1080),
    (1600, 900),
    (1366, 768),
    (1280, 720),
    (1152, 648),
    (1024, 576),
    (960, 540),
    (854, 480),
    (640, 360),
    (2560, 1600),
    (2304, 1440),
    (1920, 1200),
    (1680, 1050),
    (1440, 900),
    (1280, 800),
    (1152, 720),
    (1024, 640),
    (960, 600),
];

#[derive(Clone)]
struct Chip {
    brand: String,
    generation: u64,
    gpu_cores: u64,
}

fn home_dir() -> PathBuf {
    env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"))
}

fn expand_home(value: &str) -> PathBuf {
    if value == "~" {
        home_dir()
    } else if let Some(rest) = value.strip_prefix("~/") {
        home_dir().join(rest)
    } else {
        PathBuf::from(value)
    }
}

fn resolve_dir(var: &str, name: &str) -> PathBuf {
    if let Some(value) = env::var_os(var).filter(|v| !v.is_empty()) {
        return expand_home(&value.to_string_lossy());
    }
    let newer = home_dir().join("Developer").join(name);
    let older = home_dir().join(name);
    if newer.is_dir() || !older.is_dir() {
        newer
    } else {
        older
    }
}

fn state_dir() -> PathBuf {
    resolve_dir("STEAMARM_STATE", "SteamARM-roots")
}

fn build_dir() -> PathBuf {
    resolve_dir("STEAMARM_BUILD", "SteamARM-build")
}

fn run_output(program: &str, args: &[&str]) -> Option<String> {
    let output = Command::new(program)
        .args(args)
        .stdin(Stdio::null())
        .output()
        .ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

fn number(value: Option<&Value>) -> Option<i64> {
    match value? {
        Value::Number(n) => n.as_i64().or_else(|| n.as_f64().map(|x| x as i64)),
        Value::String(s) => s.trim().parse().ok(),
        Value::Bool(b) => Some(i64::from(*b)),
        _ => None,
    }
}

fn setting_string<'a>(settings: &'a Value, key: &str, default: &'a str) -> &'a str {
    settings
        .get(key)
        .and_then(Value::as_str)
        .filter(|s| !s.is_empty())
        .unwrap_or(default)
}

fn setting_bool(settings: &Value, key: &str, default: bool) -> bool {
    settings
        .get(key)
        .and_then(Value::as_bool)
        .unwrap_or(default)
}

fn state_file(name: &str) -> PathBuf {
    state_dir().join(name)
}

fn file_contains(path: &Path, needle: &[u8]) -> bool {
    if needle.is_empty() {
        return false;
    }
    let Ok(mut file) = File::open(path) else {
        return false;
    };
    let mut chunk = [0u8; 64 * 1024];
    let mut carry = Vec::with_capacity(needle.len().saturating_sub(1));
    loop {
        let Ok(read) = file.read(&mut chunk) else {
            return false;
        };
        if read == 0 {
            return false;
        }
        carry.extend_from_slice(&chunk[..read]);
        if find_bytes(&carry, needle) {
            return true;
        }
        let keep = needle.len().saturating_sub(1).min(carry.len());
        carry.drain(..carry.len() - keep);
    }
}

fn find_bytes(haystack: &[u8], needle: &[u8]) -> bool {
    !needle.is_empty() && haystack.windows(needle.len()).any(|w| w == needle)
}

fn own_kk_dir() -> Option<PathBuf> {
    let out = build_dir().join("mesa-kk/out");
    out.join("libvulkan_kosmickrisp.dylib")
        .is_file()
        .then_some(out)
}

fn kosmickrisp_available() -> bool {
    let version = run_output("/usr/bin/sw_vers", &["-productVersion"]).unwrap_or_default();
    let major = version
        .split('.')
        .next()
        .and_then(|v| v.parse::<u32>().ok());
    if major.is_none_or(|v| v < 26) {
        return false;
    }
    let candidates = own_kk_dir()
        .into_iter()
        .map(|p| p.join("libvulkan_kosmickrisp.dylib"))
        .chain([
            PathBuf::from("/opt/homebrew/lib/libvulkan_kosmickrisp.dylib"),
            PathBuf::from("/usr/local/lib/libvulkan_kosmickrisp.dylib"),
        ]);
    candidates.into_iter().any(|p| p.is_file())
}

fn shim_selects_icd() -> bool {
    file_contains(
        &state_file("steamroot/usr/lib/lxrt-emu/libvulkan.so.1"),
        b"STEAMARM_VK_ICD",
    )
}

fn effective_synchronization(value: Option<&Value>) -> &'static str {
    match value.and_then(Value::as_str) {
        Some("esync") => "esync",
        Some("fsync") => "fsync",
        Some("ntsync") => "ntsync",
        _ => "wineserver",
    }
}

fn host_timezone() -> Option<String> {
    let link = fs::read_link("/etc/localtime").ok()?;
    let path = link.to_string_lossy();
    let marker = "zoneinfo/";
    let offset = path.find(marker)? + marker.len();
    Some(path[offset..].to_owned())
}

fn total_ram_gb() -> u64 {
    run_output("/usr/sbin/sysctl", &["-n", "hw.memsize"])
        .and_then(|s| s.parse::<u64>().ok())
        .map(|n| n / (1 << 30))
        .unwrap_or(8)
}

fn usable_gb(total: Option<u64>) -> u64 {
    let total = total.unwrap_or_else(total_ram_gb);
    (if total <= 8 {
        total.saturating_sub(2)
    } else {
        total.saturating_sub(4)
    })
    .max(2)
}

fn apple_chip() -> Option<Chip> {
    let brand = run_output("/usr/sbin/sysctl", &["-n", "machdep.cpu.brand_string"])?;
    let words = brand.split_whitespace().collect::<Vec<_>>();
    let model = *words.get(1)?;
    let generation = model.strip_prefix('M')?.parse::<u64>().ok()?;
    if words.first() != Some(&"Apple") {
        return None;
    }
    let tier = match words.get(2).copied() {
        Some("Pro") => "Pro",
        Some("Max") => "Max",
        Some("Ultra") => "Ultra",
        _ => "base",
    };
    let gpu_cores = run_output(
        "/usr/sbin/ioreg",
        &["-r", "-c", "AGXAccelerator", "-d", "1"],
    )
    .and_then(|out| {
        out.lines()
            .find(|line| line.contains("\"gpu-core-count\""))
            .and_then(|line| line.split('=').nth(1))
            .and_then(|n| n.trim().parse::<u64>().ok())
    })
    .filter(|n| *n > 0)
    .unwrap_or(match tier {
        "Pro" => 14,
        "Max" => 24,
        "Ultra" => 48,
        _ => 7,
    });
    Some(Chip {
        brand,
        generation,
        gpu_cores,
    })
}

#[cfg(target_os = "macos")]
fn main_display_size() -> Option<(u32, u32)> {
    #[link(name = "CoreGraphics", kind = "framework")]
    unsafe extern "C" {
        fn CGMainDisplayID() -> u32;
        fn CGDisplayPixelsWide(display: u32) -> usize;
        fn CGDisplayPixelsHigh(display: u32) -> usize;
    }
    unsafe {
        let display = CGMainDisplayID();
        let width = CGDisplayPixelsWide(display) as u32;
        let height = CGDisplayPixelsHigh(display) as u32;
        (width > 0 && height > 0).then_some((width, height))
    }
}

#[cfg(not(target_os = "macos"))]
fn main_display_size() -> Option<(u32, u32)> {
    None
}

fn render_size(display: (u32, u32), scale: f64) -> (u32, u32) {
    let (width, height) = display;
    if scale >= 1.0 {
        return display;
    }
    let rw = width as f64 * scale;
    let rh = height as f64 * scale;
    let shape = width as f64 / height as f64;
    let best = COMMON_MODES
        .iter()
        .filter(|(w, h)| {
            ((*w as f64 / *h as f64) - shape).abs() <= 0.01 * shape
                && (*w as f64) <= rw * 1.02
                && *w < width
        })
        .max_by_key(|(w, h)| (*w, *h));
    if let Some(&(w, h)) = best {
        return (w, h);
    }
    let even = |v: f64| ((v / 2.0).round_ties_even() as u32 * 2).max(2);
    (even(rw), even(rh))
}

fn upscaling_policy(chip: Option<Chip>, display: Option<(u32, u32)>) -> Value {
    let (width, height) = display.unwrap_or((1920, 1080));
    let Some(chip) = chip else {
        return json!({
            "filter":"fsr", "scale":1.0, "preset":"nativa",
            "render":[width,height], "display":[width,height], "chip":null,
            "reason":"no Apple silicon: no MetalFX"
        });
    };
    let speed = GPU_CORE_SPEED
        .iter()
        .find(|(generation, _)| *generation == chip.generation)
        .map(|(_, speed)| *speed)
        .unwrap_or(1.0);
    let ideal =
        (chip.gpu_cores as f64 * speed * PIXELS_PER_CORE / (width as f64 * height as f64)).sqrt();
    let mut chosen = *RENDER_SCALES.last().unwrap_or(&(0.5, "rendimiento"));
    for &(scale, name) in RENDER_SCALES {
        if ideal + 0.01 >= scale {
            chosen = (scale, name);
            break;
        }
    }
    let (rw, rh) = render_size((width, height), chosen.0);
    json!({
        "filter":"fsr", "scale":chosen.0, "preset":chosen.1,
        "render":[rw,rh], "display":[width,height],
        "chip":format!("{}, GPU de {} núcleos", chip.brand, chip.gpu_cores),
        "reason":"FSR 1.0: el filtro más rápido medido (MetalFX espacial, 7-9 fps menos)"
    })
}

fn game_upscaler(settings: &Value) -> &'static str {
    match settings.get("gameUpscaler").and_then(Value::as_str) {
        Some("fsr") => "fsr",
        _ => "auto",
    }
}

fn env_from_settings(settings: &Value, total: Option<u64>) -> BTreeMap<String, String> {
    let mut out = BTreeMap::new();
    let mut dxvk = Vec::new();
    let lang = setting_string(settings, "guestLanguage", "auto");
    if lang != "auto" {
        out.insert("LANG".into(), lang.into());
        out.insert("LC_ALL".into(), lang.into());
    }
    let tz = setting_string(settings, "timezone", "auto");
    let timezone = if tz == "auto" {
        host_timezone()
    } else {
        Some(tz.to_owned())
    };
    if let Some(tz) = timezone {
        out.insert("TZ".into(), tz);
    }
    let vsync = setting_string(settings, "vsync", "game");
    if settings.get("adaptiveSync").and_then(Value::as_bool) == Some(true) {
        out.insert("LXRT_VK_ADAPTIVE_SYNC".into(), "1".into());
    }
    if matches!(vsync, "on" | "off") {
        let n = if vsync == "on" { "1" } else { "0" };
        dxvk.push(format!("dxgi.syncInterval = {n}"));
        dxvk.push(format!("d3d9.presentInterval = {n}"));
        out.insert(
            "VKD3D_SWAPCHAIN_PRESENT_MODE".into(),
            if vsync == "on" { "FIFO" } else { "IMMEDIATE" }.into(),
        );
    }
    let top = usable_gb(total);
    let vram = number(settings.get("vramGB"))
        .filter(|n| *n != 0)
        .unwrap_or(top as i64)
        .max(0) as u64;
    let vram = vram.min(top);
    out.insert("LXRT_VK_MAX_VRAM_MB".into(), (vram * 1024).to_string());
    dxvk.push(format!("dxgi.maxDeviceMemory = {}", vram * 1024));

    if settings.get("synchronization").is_none()
        && (settings.get("esync").is_some() || settings.get("fsync").is_some())
    {
        if settings.get("esync").and_then(Value::as_bool) == Some(false) {
            out.insert("PROTON_NO_ESYNC".into(), "1".into());
        }
        out.insert("PROTON_NO_FSYNC".into(), "1".into());
    } else {
        let sync = effective_synchronization(settings.get("synchronization"));
        if sync != "esync" {
            out.insert("PROTON_NO_ESYNC".into(), "1".into());
        }
        if sync != "fsync" {
            out.insert("PROTON_NO_FSYNC".into(), "1".into());
        }
        if sync == "ntsync" {
            out.insert("LXRT_NTSYNC".into(), "1".into());
        }
    }

    if setting_bool(settings, "fexDiskCache", false) {
        out.insert("FEX_DISKCACHE".into(), "1".into());
    }
    match setting_string(settings, "fexTSO", "full") {
        "off" => {
            out.insert("FEX_TSOENABLED".into(), "0".into());
        }
        "fast" => {
            out.insert("FEX_TSOENABLED".into(), "1".into());
            out.insert("FEX_VECTORTSOENABLED".into(), "0".into());
            out.insert("FEX_MEMCPYSETTSOENABLED".into(), "0".into());
        }
        _ => {}
    }
    if settings.get("fexMultiblock").and_then(Value::as_bool) == Some(false) {
        out.insert("FEX_MULTIBLOCK".into(), "0".into());
    }
    match setting_string(settings, "fexSMC", "mtrack") {
        smc @ ("none" | "full") => {
            out.insert("FEX_SMCCHECKS".into(), smc.into());
        }
        _ => {}
    }
    if setting_bool(settings, "fexX87Reduced", false) {
        out.insert("FEX_X87REDUCEDPRECISION".into(), "1".into());
    }
    if settings.get("execution").and_then(Value::as_str) == Some("lightningJIT") {
        out.insert("FEX_DISKCACHE".into(), "1".into());
        for key in [
            "FEX_TSOENABLED",
            "FEX_VECTORTSOENABLED",
            "FEX_MEMCPYSETTSOENABLED",
            "FEX_MULTIBLOCK",
            "FEX_SMCCHECKS",
        ] {
            out.remove(key);
        }
        out.insert("FEX_X87REDUCEDPRECISION".into(), "1".into());
    }

    let gfx = setting_string(settings, "graphicsBackend", "auto");
    let use_kk = gfx == "vulkanKosmicKrisp" || (gfx == "auto" && kosmickrisp_available());
    if use_kk && shim_selects_icd() {
        out.insert("STEAMARM_VK_ICD".into(), "kosmickrisp".into());
        if let Some(own) = own_kk_dir() {
            out.insert("STEAMARM_KK_DIR".into(), own.to_string_lossy().into_owned());
        }
    } else if gfx == "openGLWineD3D" {
        out.insert("PROTON_USE_WINED3D".into(), "1".into());
        out.insert("GALLIUM_DRIVER".into(), "zink".into());
        out.insert("MESA_EXTENSION_OVERRIDE".into(), ZINK_GL45_OVERRIDE.into());
    }
    if settings.get("shaderCache").and_then(Value::as_bool) == Some(false) {
        out.insert("DXVK_SHADER_CACHE".into(), "0".into());
        out.insert("VKD3D_SHADER_CACHE_PATH".into(), "0".into());
    }
    let msaa = number(settings.get("antialiasing")).unwrap_or(0);
    if matches!(msaa, 2 | 4 | 8) {
        dxvk.push(format!("d3d9.forceSwapchainMSAA = {msaa}"));
    }
    if setting_bool(settings, "resolutionScaling", false) {
        out.insert("WINE_DISABLE_FULLSCREEN_HACK".into(), "1".into());
        out.insert("LXRT_VK_PADDED_FULLSCREEN".into(), "1".into());
    }
    let filter = setting_string(settings, "scalingFilter", "auto");
    if SCALING_FILTERS.contains(&filter) && filter != "linear" {
        out.insert(
            "LXRT_VK_SCALER".into(),
            if filter == "auto" { "fsr" } else { filter }.into(),
        );
    }
    let upscaler = game_upscaler(settings);
    out.insert(
        "KK_FSR_METALFX".into(),
        if upscaler == "auto" { "1" } else { "0" }.into(),
    );
    if upscaler == "auto" && out.get("STEAMARM_VK_ICD").map(String::as_str) == Some("kosmickrisp") {
        out.insert("STEAMARM_WIN_UPSCALER".into(), "metalfx".into());
    }
    let scale_text = setting_string(settings, "renderScale", "1.0");
    let mut scale = if scale_text == "auto" {
        1.0
    } else {
        scale_text.parse::<f64>().unwrap_or(1.0)
    };
    if !(0.33..0.995).contains(&scale) {
        scale = 1.0;
    }
    if scale >= 0.995
        && filter == "metalfx-temporal"
        && setting_bool(settings, "metalfxNativeAA", false)
    {
        out.insert("STEAMARM_MFX_NATIVE".into(), "1".into());
    }
    if scale < 0.995 {
        let display = main_display_size();
        out.insert("STEAMARM_RENDER_SCALE".into(), format!("{scale:.4}"));
        if let Some((width, height)) = display {
            let side = |n: u32| {
                let value = ((n as f64 * scale + 0.5) as u32) & !1;
                let minimum = ((n + 5) / 6 * 2).max(2);
                value.max(minimum)
            };
            out.insert(
                "STEAMARM_RENDER_SIZE".into(),
                format!("{}x{}", side(width), side(height)),
            );
        }
    }
    if filter == "fsr" || filter == "auto" {
        let sharpness = number(settings.get("fsrSharpness"))
            .unwrap_or(90)
            .clamp(0, 100);
        out.insert("LXRT_VK_FSR_SHARPNESS".into(), sharpness.to_string());
    }
    let steam_ui_switches = if setting_bool(settings, "steamUIAcceleration", false) {
        STEAM_UI_GPU_SWITCHES
    } else {
        STEAM_UI_SOFTWARE_SWITCHES
    };
    out.insert(
        "LXRT_EXEC_ARGS".into(),
        format!("steamwebhelper!--type=:{}", steam_ui_switches.join(" ")),
    );
    let aniso = number(settings.get("anisotropy")).unwrap_or(0);
    if matches!(aniso, 2 | 4 | 8 | 16) {
        dxvk.push(format!("d3d11.samplerAnisotropy = {aniso}"));
        dxvk.push(format!("d3d9.samplerAnisotropy = {aniso}"));
        out.insert("LXRT_VK_ANISOTROPY".into(), aniso.to_string());
    }
    let fps = number(settings.get("frameRateLimit")).unwrap_or(0);
    if fps > 0 {
        dxvk.push(format!("dxgi.maxFrameRate = {fps}"));
        dxvk.push(format!("d3d9.maxFrameRate = {fps}"));
        out.insert("VKD3D_FRAME_RATE".into(), fps.to_string());
        out.insert("LXRT_VK_FPS_LIMIT".into(), fps.to_string());
    }
    match setting_string(settings, "dxvkHud", "off") {
        "fps" => {
            out.insert("DXVK_HUD".into(), "fps".into());
        }
        "full" => {
            out.insert("DXVK_HUD".into(), "full".into());
        }
        _ => {}
    }
    if setting_bool(settings, "metalHud", false) {
        out.insert("MTL_HUD_ENABLED".into(), "1".into());
    }
    if !dxvk.is_empty() {
        out.insert("DXVK_CONFIG".into(), dxvk.join("; "));
    }

    if setting_string(settings, "audioBackend", "coreaudio") == "none" {
        out.insert("PULSE_SERVER".into(), "none".into());
        out.insert("SDL_AUDIODRIVER".into(), "dummy".into());
        out.insert("SDL_AUDIO_DRIVER".into(), "dummy".into());
    } else {
        out.insert("PULSE_SERVER".into(), "unix:/tmp/pulse/native".into());
        out.insert(
            "PIPEWIRE_RUNTIME_DIR".into(),
            "/tmp/steamarm-pipewire".into(),
        );
        out.insert("SDL_AUDIO_DRIVER".into(), "pulseaudio".into());
        out.insert("SDL_AUDIODRIVER".into(), "pulseaudio".into());
    }
    out.insert("SDL_HIDAPI_LIBUSB".into(), "0".into());

    if setting_bool(settings, "protonLog", false) {
        out.insert("PROTON_LOG".into(), "1".into());
    }
    if let Some(value) = settings
        .get("wineDebug")
        .filter(|v| !v.is_null() && *v != "")
    {
        out.insert("WINEDEBUG".into(), value_to_string(value));
    }
    let dxvk_log = setting_string(settings, "dxvkLogLevel", "warn");
    if dxvk_log != "warn" {
        out.insert("DXVK_LOG_LEVEL".into(), dxvk_log.into());
    }
    let vkd3d_log = setting_string(settings, "vkd3dLogLevel", "err");
    if vkd3d_log != "err" {
        out.insert("VKD3D_DEBUG".into(), vkd3d_log.into());
    }
    if setting_bool(settings, "guestFaults", true) {
        out.insert("LXRT_GUEST_FAULTS".into(), "1".into());
    }
    if let Some(value) = settings
        .get("traceMatch")
        .filter(|v| !v.is_null() && *v != "")
    {
        out.insert("LXRT_TRACE_MATCH".into(), value_to_string(value));
    }
    if setting_bool(settings, "vulkanDebug", false) {
        out.insert("LXRT_VK_DEBUG".into(), "1".into());
    }
    if let Some(extra) = settings.get("extraEnv").and_then(Value::as_object) {
        for (key, value) in extra {
            out.insert(key.clone(), value_to_string(value));
        }
    }
    out
}

fn value_to_string(value: &Value) -> String {
    match value {
        Value::Null => "None".into(),
        Value::String(s) => s.clone(),
        Value::Bool(b) => b.to_string(),
        Value::Number(n) => n.to_string(),
        _ => value.to_string(),
    }
}

fn audio_volume(settings: &Value) -> Option<u64> {
    if setting_string(settings, "audioBackend", "coreaudio") == "none" {
        return None;
    }
    Some(number(settings.get("volume")).unwrap_or(100).clamp(0, 100) as u64)
}

#[cfg(target_os = "macos")]
fn default_output_name() -> Option<String> {
    use std::ffi::{c_char, c_void, CStr};
    use std::ptr;

    type AudioObjectId = u32;
    #[repr(C)]
    struct AudioObjectPropertyAddress {
        selector: u32,
        scope: u32,
        element: u32,
    }

    #[link(name = "CoreAudio", kind = "framework")]
    unsafe extern "C" {
        fn AudioObjectGetPropertyData(
            object_id: AudioObjectId,
            address: *const AudioObjectPropertyAddress,
            qualifier_size: u32,
            qualifier_data: *const c_void,
            data_size: *mut u32,
            data: *mut c_void,
        ) -> i32;
    }
    #[link(name = "CoreFoundation", kind = "framework")]
    unsafe extern "C" {
        fn CFStringGetCString(
            value: *const c_void,
            buffer: *mut c_char,
            buffer_size: isize,
            encoding: u32,
        ) -> u8;
    }

    const SYSTEM_OBJECT: AudioObjectId = 1;
    const GLOBAL: u32 = u32::from_be_bytes(*b"glob");
    const DEFAULT_OUTPUT: u32 = u32::from_be_bytes(*b"dOut");
    const LOCALIZED_NAME: u32 = u32::from_be_bytes(*b"lnam");
    const UTF8: u32 = 0x0800_0100;

    unsafe {
        let mut address = AudioObjectPropertyAddress {
            selector: DEFAULT_OUTPUT,
            scope: GLOBAL,
            element: 0,
        };
        let mut device = 0u32;
        let mut size = std::mem::size_of_val(&device) as u32;
        if AudioObjectGetPropertyData(
            SYSTEM_OBJECT,
            &address,
            0,
            ptr::null(),
            &mut size,
            (&mut device as *mut AudioObjectId).cast(),
        ) != 0
            || device == 0
        {
            return None;
        }

        address.selector = LOCALIZED_NAME;
        let mut name: *const c_void = ptr::null();
        size = std::mem::size_of_val(&name) as u32;
        if AudioObjectGetPropertyData(
            device,
            &address,
            0,
            ptr::null(),
            &mut size,
            (&mut name as *mut *const c_void).cast(),
        ) != 0
            || name.is_null()
        {
            return None;
        }

        let mut buffer = [0 as c_char; 512];
        if CFStringGetCString(name, buffer.as_mut_ptr(), buffer.len() as isize, UTF8) == 0 {
            return None;
        }
        CStr::from_ptr(buffer.as_ptr())
            .to_str()
            .ok()
            .map(str::to_owned)
    }
}

#[cfg(not(target_os = "macos"))]
fn default_output_name() -> Option<String> {
    None
}

fn write_limits(settings: &Value) -> io::Result<()> {
    let top = usable_gb(None);
    let dram = number(settings.get("dramGB"))
        .filter(|n| *n != 0)
        .unwrap_or(top as i64)
        .max(0) as u64;
    let dram = dram.min(top);
    let dir = state_dir().join("launcher");
    fs::create_dir_all(&dir)?;
    let tmp = dir.join("limits.env.tmp");
    fs::write(&tmp, format!("GUEST_MAX_MB={}\n", dram * 1024))?;
    fs::rename(tmp, dir.join("limits.env"))
}

fn load(path: &Path) -> Value {
    fs::read(path)
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
        .filter(Value::is_object)
        .unwrap_or_else(|| json!({}))
}

fn shell_quote(value: &str) -> String {
    if value.is_empty() {
        return "''".into();
    }
    let safe = value
        .bytes()
        .all(|b| b.is_ascii_alphanumeric() || b"_@%+=:,./-".contains(&b));
    if safe {
        value.to_owned()
    } else {
        format!("'{}'", value.replace('\'', "'\\''"))
    }
}

fn merged_settings(input: &Value) -> Value {
    let settings = input.get("settings").unwrap_or(input);
    let mut merged = settings.as_object().cloned().unwrap_or_default();
    let allow = OVERRIDABLE.iter().copied().collect::<BTreeSet<_>>();
    for key in ["overrides", "fallbacks"] {
        if let Some(values) = input.get(key).and_then(Value::as_object) {
            for (name, value) in values {
                if allow.contains(name.as_str()) && value.as_str().is_some_and(|s| !s.is_empty()) {
                    merged.insert(name.clone(), value.clone());
                }
            }
        }
    }
    Value::Object(merged)
}

fn read_stdin_json() -> Result<Value, String> {
    let mut input = String::new();
    io::stdin()
        .read_to_string(&mut input)
        .map_err(|e| format!("read stdin: {e}"))?;
    serde_json::from_str(&input).map_err(|e| format!("invalid JSON: {e}"))
}

fn print_json(value: &Value) -> Result<(), String> {
    println!(
        "{}",
        serde_json::to_string_pretty(value).map_err(|e| e.to_string())?
    );
    Ok(())
}

fn run() -> Result<(), String> {
    let args = env::args().skip(1).collect::<Vec<_>>();
    let mode = args.first().map(String::as_str).unwrap_or("--shell");
    if mode == "--state-dir" {
        println!("{}", state_dir().display());
        return Ok(());
    }
    if mode == "--build-dir" {
        println!("{}", build_dir().display());
        return Ok(());
    }
    if mode == "--env-json-stdin" {
        let settings = merged_settings(&read_stdin_json()?);
        write_limits(&settings).map_err(|e| format!("write memory limit: {e}"))?;
        let env = env_from_settings(&settings, None);
        return print_json(&Value::Object(
            env.into_iter()
                .map(|(k, v)| (k, Value::String(v)))
                .collect(),
        ));
    }
    let path = args
        .get(1)
        .map(PathBuf::from)
        .unwrap_or_else(|| state_dir().join("launcher/settings.json"));
    let settings = load(&path);
    match mode {
        "--default-output" => {
            if let Some(name) = default_output_name() {
                println!("{name}");
            }
            Ok(())
        }
        "--upscaling" => print_json(&upscaling_policy(apple_chip(), main_display_size())),
        "--emulate-modeset" => {
            println!(
                "{}",
                if setting_bool(&settings, "resolutionScaling", false) {
                    "on"
                } else {
                    "off"
                }
            );
            Ok(())
        }
        "--volume" => {
            write_limits(&settings).map_err(|e| format!("write memory limit: {e}"))?;
            if let Some(volume) = audio_volume(&settings) {
                println!("{volume}");
            }
            Ok(())
        }
        "--json" | "--shell" => {
            let values = env_from_settings(&settings, None);
            write_limits(&settings).map_err(|e| format!("write memory limit: {e}"))?;
            if mode == "--json" {
                print_json(&Value::Object(
                    values.into_iter().map(|(k, v)| (k, Value::String(v))).collect(),
                ))
            } else {
                for (key, value) in values {
                    println!("export {key}={}", shell_quote(&value));
                }
                Ok(())
            }
        }
        _ => Err(format!("unknown mode {mode}; expected --shell, --json, --volume, --emulate-modeset, --upscaling, --default-output, or --env-json-stdin")),
    }
}

fn main() {
    if let Err(error) = run() {
        eprintln!("steamarm-settings-env: {error}");
        std::process::exit(1);
    }
}
