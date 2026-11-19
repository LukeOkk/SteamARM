use serde_json::{json, Value};
use std::collections::HashSet;
use std::env;
use std::ffi::{c_char, c_void, CStr, CString};
use std::fs::{self, File};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const ICD_DIRS: &[&str] = &[
    "/opt/homebrew/share/vulkan/icd.d",
    "/opt/homebrew/etc/vulkan/icd.d",
    "/usr/local/share/vulkan/icd.d",
    "/usr/local/etc/vulkan/icd.d",
    "~/.local/share/vulkan/icd.d",
    "/etc/vulkan/icd.d",
];
const SHIM_KK_PATHS: &[&str] = &[
    "/opt/homebrew/lib/libvulkan_kosmickrisp.dylib",
    "/usr/local/lib/libvulkan_kosmickrisp.dylib",
];
const GLX_PATCH: &str = "xquartz-glx-mesa-direct.patch";
const NOTE: &str = "Instalado no garantiza compatibilidad con cada juego. FEX de Steam no sustituye al FEX adaptado a macOS.";
const NATIVE_ARM_REASON: &str = "Requiere Linux ARM64. Este runtime macOS aún no soporta las reservas de memoria baja ni el TEB x18 de Wine ARM64EC.";

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

fn xquartz_dir() -> PathBuf {
    env::var_os("XQ_ROOT")
        .filter(|v| !v.is_empty())
        .map(|v| expand_home(&v.to_string_lossy()))
        .unwrap_or_else(|| build_dir().join("xquartz"))
}

fn contains_bytes(path: &Path, needle: &[u8]) -> bool {
    if needle.is_empty() {
        return false;
    }
    let Ok(mut file) = File::open(path) else {
        return false;
    };
    let mut block = [0u8; 64 * 1024];
    let mut carry = Vec::with_capacity(needle.len().saturating_sub(1));
    loop {
        let Ok(n) = file.read(&mut block) else {
            return false;
        };
        if n == 0 {
            return false;
        }
        carry.extend_from_slice(&block[..n]);
        if carry.windows(needle.len()).any(|part| part == needle) {
            return true;
        }
        let keep = needle.len().saturating_sub(1).min(carry.len());
        carry.drain(..carry.len() - keep);
    }
}

fn macos_version() -> String {
    run_output("/usr/bin/sw_vers", &["-productVersion"]).unwrap_or_default()
}

fn kosmickrisp_build_dir() -> Option<PathBuf> {
    let out = build_dir().join("mesa-kk/out");
    out.join("libvulkan_kosmickrisp.dylib")
        .is_file()
        .then_some(out)
}

fn shim_kk_paths() -> Vec<PathBuf> {
    kosmickrisp_build_dir()
        .map(|dir| vec![dir.join("libvulkan_kosmickrisp.dylib")])
        .unwrap_or_default()
        .into_iter()
        .chain(SHIM_KK_PATHS.iter().map(PathBuf::from))
        .collect()
}

#[derive(Clone)]
struct IcdManifest {
    path: PathBuf,
    icd: Value,
    found: Option<PathBuf>,
}

fn icd_manifests(dirs: &[PathBuf]) -> Vec<IcdManifest> {
    let mut found = Vec::new();
    for dir in dirs {
        let Ok(entries) = fs::read_dir(dir) else {
            continue;
        };
        let mut manifests = entries
            .filter_map(Result::ok)
            .map(|entry| entry.path())
            .filter(|path| path.extension().is_some_and(|ext| ext == "json"))
            .collect::<Vec<_>>();
        manifests.sort();
        for path in manifests {
            let Ok(data) = fs::read(&path) else { continue };
            let Ok(value) = serde_json::from_slice::<Value>(&data) else {
                continue;
            };
            let Some(icd) = value.get("ICD") else { continue };
            let Some(library) = icd.get("library_path").and_then(Value::as_str) else {
                continue;
            };
            if !Path::new(library)
                .file_name()
                .is_some_and(|name| name.to_string_lossy().to_lowercase().contains("kosmickrisp"))
            {
                continue;
            }
            let candidates = if Path::new(library).is_absolute() {
                vec![PathBuf::from(library)]
            } else if library.contains('/') {
                vec![path.parent().unwrap_or(Path::new(".")).join(library)]
            } else {
                vec![
                    PathBuf::from("/opt/homebrew/lib").join(library),
                    PathBuf::from("/usr/local/lib").join(library),
                ]
            };
            found.push(IcdManifest {
                path,
                icd: icd.clone(),
                found: candidates.into_iter().find(|candidate| candidate.is_file()),
            });
        }
    }
    found
}

#[cfg(target_os = "macos")]
#[link(name = "System")]
extern "C" {
    fn dlopen(path: *const c_char, mode: i32) -> *mut c_void;
    fn dlsym(handle: *mut c_void, symbol: *const c_char) -> *mut c_void;
    fn dlclose(handle: *mut c_void) -> i32;
}

#[cfg(target_os = "macos")]
unsafe fn open_library(path: &Path) -> Option<*mut c_void> {
    let name = CString::new(path.to_string_lossy().as_bytes()).ok()?;
    let handle = dlopen(name.as_ptr(), 0x2 | 0x4); // RTLD_NOW | RTLD_LOCAL
    (!handle.is_null()).then_some(handle)
}

#[cfg(not(target_os = "macos"))]
unsafe fn open_library(_path: &Path) -> Option<*mut c_void> {
    None
}

#[cfg(target_os = "macos")]
unsafe fn has_icd_entry(handle: *mut c_void) -> bool {
    let Ok(symbol) = CString::new("vk_icdGetInstanceProcAddr") else {
        return false;
    };
    !dlsym(handle, symbol.as_ptr()).is_null()
}

#[cfg(not(target_os = "macos"))]
unsafe fn has_icd_entry(_handle: *mut c_void) -> bool {
    false
}

#[cfg(target_os = "macos")]
unsafe fn moltenvk_strings(handle: *mut c_void) -> Option<(String, String)> {
    type Query = unsafe extern "C" fn(*mut c_char, u32, *mut c_char, u32);
    let symbol = CString::new("vkGetVersionStringsMVK").ok()?;
    let address = dlsym(handle, symbol.as_ptr());
    if address.is_null() {
        return None;
    }
    let query: Query = std::mem::transmute(address);
    let mut mvk = [0 as c_char; 256];
    let mut vulkan = [0 as c_char; 256];
    query(mvk.as_mut_ptr(), mvk.len() as u32, vulkan.as_mut_ptr(), vulkan.len() as u32);
    let mvk = CStr::from_ptr(mvk.as_ptr()).to_string_lossy().into_owned();
    let vulkan = CStr::from_ptr(vulkan.as_ptr()).to_string_lossy().into_owned();
    Some((mvk, vulkan))
}

#[cfg(not(target_os = "macos"))]
unsafe fn moltenvk_strings(_handle: *mut c_void) -> Option<(String, String)> {
    None
}

fn moltenvk() -> Value {
    for path in [
        PathBuf::from("/opt/homebrew/lib/libMoltenVK.dylib"),
        PathBuf::from("/usr/local/lib/libMoltenVK.dylib"),
    ] {
        let handle = unsafe { open_library(&path) };
        let Some(handle) = handle else { continue };
        let mut result = json!({
            "path": path.canonicalize().unwrap_or(path).to_string_lossy(),
            "version": "desconocida"
        });
        if let Some((version, vulkan)) = unsafe { moltenvk_strings(handle) } {
            result["version"] = json!(version);
            result["vulkan"] = json!(vulkan);
        }
        #[cfg(target_os = "macos")]
        unsafe {
            dlclose(handle);
        }
        return result;
    }
    json!({"path": "", "version": "no instalado"})
}

fn mesa_version(path: &Path) -> String {
    let parts = path.components().collect::<Vec<_>>();
    for pair in parts.windows(2) {
        if pair[0].as_os_str() == "mesa" {
            return pair[1].as_os_str().to_string_lossy().into_owned();
        }
    }
    String::new()
}

fn kosmickrisp() -> Value {
    let version = macos_version();
    let os_ok = version
        .split('.')
        .next()
        .and_then(|v| v.parse::<u32>().ok())
        .is_some_and(|major| major >= 26);
    let dirs = ICD_DIRS.iter().map(|dir| expand_home(dir)).collect::<Vec<_>>();
    let manifests = icd_manifests(&dirs);
    let mut result = json!({
        "icd_json": "",
        "library": "",
        "version": "",
        "api_version": "",
        "os_ok": os_ok,
        "exports_icd": false,
        "found_elsewhere": ""
    });
    for candidate in shim_kk_paths() {
        if !candidate.is_file() {
            continue;
        }
        let Some(handle) = (unsafe { open_library(&candidate) }) else {
            continue;
        };
        let library = candidate.canonicalize().unwrap_or(candidate);
        result["library"] = json!(library.to_string_lossy());
        result["version"] = json!(mesa_version(&library));
        if let Some(manifest) = manifests.iter().find(|manifest| {
            manifest
                .found
                .as_ref()
                .and_then(|path| path.canonicalize().ok())
                .is_some_and(|path| path == library)
        }) {
            result["icd_json"] = json!(manifest.path.to_string_lossy());
            result["api_version"] = json!(manifest.icd.get("api_version").and_then(Value::as_str).unwrap_or(""));
        }
        result["exports_icd"] = json!(unsafe { has_icd_entry(handle) });
        #[cfg(target_os = "macos")]
        unsafe {
            dlclose(handle);
        }
        return result;
    }
    if let Some(path) = manifests.iter().find_map(|manifest| {
        manifest.found.as_ref()?.canonicalize().ok()
    }) {
        result["found_elsewhere"] = json!(path.to_string_lossy());
    }
    result
}

fn shim(state: &Path) -> Value {
    let path = state.join("steamroot/usr/lib/lxrt-emu/libvulkan.so.1");
    json!({
        "path": path.to_string_lossy(),
        "installed": path.is_file(),
        "icd_selection": contains_bytes(&path, b"STEAMARM_VK_ICD")
    })
}

fn screen_sharing_available() -> bool {
    [
        "/System/Applications/Utilities/Screen Sharing.app",
        "/System/Library/CoreServices/Applications/Screen Sharing.app",
    ]
    .iter()
    .any(|path| Path::new(path).exists())
}

fn presentation(state: &Path, xq: &Path) -> Value {
    json!({
        "native_x": xq.join("SteamARM-X11.app/Contents/MacOS/X11.bin").is_file(),
        "xvnc": state.join("steamroot/usr/bin/Xvnc").is_file(),
        "screen_sharing": screen_sharing_available()
    })
}

fn opengl(state: &Path, xq: &Path) -> Value {
    let zink = ["x86_64-linux-gnu", "i386-linux-gnu"]
        .iter()
        .all(|triplet| state.join(format!("steamroot/usr/lib/{triplet}/dri/zink_dri.so")).is_file());
    let marker = xq.join("SteamARM-X11.app/Contents/Resources/steamarm-patches.txt");
    let glx_direct = fs::read_to_string(marker)
        .map(|text| text.split_whitespace().any(|line| line == GLX_PATCH))
        .unwrap_or(false);
    json!({"zink": zink, "glx_direct": glx_direct})
}

fn proton_sync(folder: &Path) -> Value {
    let ntdll_paths = [
        folder.join("files/lib/wine/x86_64-unix/ntdll.so"),
        folder.join("files/lib/wine/aarch64-unix/ntdll.so"),
    ];
    let server_paths = [folder.join("files/bin/wineserver"), folder.join("files/bin-arm64/wineserver")];
    let has = |marker: &[u8]| {
        ntdll_paths.iter().chain(server_paths.iter()).any(|path| {
            path.is_file() && contains_bytes(path, marker)
        })
    };
    json!({
        "esync": has(b"esync_init"),
        "fsync": has(b"fsync_init"),
        "ntsync": has(b"/dev/ntsync")
    })
}

fn proton_directories(steam: &Path) -> Vec<(PathBuf, String)> {
    let roots = [
        (steam.join("steamapps/common"), "Steam"),
        (steam.join("compatibilitytools.d"), "Personalizado"),
        (steam.join("steamapps/compatibilitytools.d"), "Personalizado"),
    ];
    let mut seen = HashSet::new();
    let mut tools = Vec::new();
    for (root, source) in roots {
        let Ok(entries) = fs::read_dir(root) else { continue };
        let mut folders = entries
            .filter_map(Result::ok)
            .map(|entry| entry.path())
            .filter(|path| path.is_dir())
            .collect::<Vec<_>>();
        folders.sort_by_key(|path| path.file_name().unwrap_or_default().to_string_lossy().to_lowercase());
        for folder in folders {
            if !folder.join("proton").is_file() {
                continue;
            }
            if !["toolmanifest.vdf", "compatibilitytool.vdf"]
                .iter()
                .any(|name| folder.join(name).is_file())
            {
                continue;
            }
            let name = folder.file_name().unwrap_or_default().to_string_lossy().into_owned();
            if seen.insert(name.to_lowercase()) {
                tools.push((folder, source.to_owned()));
            }
        }
    }
    tools
}

fn inventory(state: &Path) -> Value {
    let steam = state.join("steamroot/tmp/fexhome/.local/share/Steam");
    let common = steam.join("steamapps/common");
    let mut protons = Vec::new();
    for (folder, source) in proton_directories(&steam) {
        let name = folder.file_name().unwrap_or_default().to_string_lossy().into_owned();
        let arm = folder.join("files/bin-arm64").is_dir() || name.to_uppercase().contains("ARM64");
        let architecture = if arm { "ARM64" } else { "x86_64" };
        let wine = if arm {
            folder.join("files/bin-arm64/wine")
        } else {
            folder.join("files/bin/wine")
        };
        let installed = wine.is_file();
        let sync = proton_sync(&folder);
        protons.push(json!({
            "name": name,
            "architecture": architecture,
            "installed": installed,
            "supported": !arm && installed,
            "source": source,
            "esync": sync["esync"],
            "fsync": sync["fsync"],
            "ntsync": sync["ntsync"]
        }));
    }
    json!({
        "moltenvk": moltenvk(),
        "kosmickrisp": kosmickrisp(),
        "shim": shim(state),
        "presentation": presentation(state, &xquartz_dir()),
        "opengl": opengl(state, &xquartz_dir()),
        "fex": {
            "patchedInstalled": state.join("lxrt-root/usr/bin/FEX-gb").is_file(),
            "steamInstalled": common.join("FEX-Emu/FEXCompatTool").is_file()
        },
        "protons": protons,
        "runtime": {
            "ntsync": contains_bytes(&build_dir().join("build/lxrun"), b"/tmp/lxrt-ntsync-")
        },
        "nativeArmReason": NATIVE_ARM_REASON,
        "note": NOTE
    })
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

fn yes(value: &Value) -> &'static str {
    if value.as_bool().unwrap_or(false) { "sí" } else { "no" }
}

fn print_report(report: &Value) {
    let moltenvk = &report["moltenvk"];
    println!("MoltenVK: {}", moltenvk["version"].as_str().unwrap_or(""));
    println!("Biblioteca: {}", moltenvk["path"].as_str().unwrap_or(""));
    println!("FEX macOS: {}", if report["fex"]["patchedInstalled"] == true { "instalado" } else { "falta" });
    println!("FEX de Steam: {}", if report["fex"]["steamInstalled"] == true { "instalado; requiere Linux" } else { "no instalado" });
    if let Some(tools) = report["protons"].as_array() {
        for tool in tools {
            let status = if tool["supported"] == true { "disponible" } else if tool["installed"] == true { "no compatible" } else { "incompleto" };
            println!("{}: {} ({})", tool["name"].as_str().unwrap_or(""), status, tool["architecture"].as_str().unwrap_or(""));
            println!("  esync/fsync/ntsync: {}/{}/{}", yes(&tool["esync"]), yes(&tool["fsync"]), yes(&tool["ntsync"]));
        }
    }
    println!("{}", report["nativeArmReason"].as_str().unwrap_or(""));
    println!("{}", report["note"].as_str().unwrap_or(""));
    let kk = &report["kosmickrisp"];
    let library = kk["library"].as_str().unwrap_or("");
    let elsewhere = kk["found_elsewhere"].as_str().unwrap_or("");
    if !library.is_empty() {
        let mut label = kk["version"].as_str().filter(|v| !v.is_empty()).unwrap_or("versión desconocida").to_owned();
        if report["shim"]["icd_selection"] != true {
            label.push_str(" (sin selección de ICD en el shim)");
        }
        println!("KosmicKrisp: {label}");
    } else if !elsewhere.is_empty() {
        println!("KosmicKrisp: en {elsewhere}, que el shim no carga (solo {})", SHIM_KK_PATHS.join(" o "));
    } else {
        println!("KosmicKrisp: no instalado");
    }
    println!("Shim Vulkan: selección de ICD {}", yes(&report["shim"]["icd_selection"]));
    let pres = &report["presentation"];
    println!("Pantallas: X11 nativo {}, Xvnc {}, Compartir pantalla {}", yes(&pres["native_x"]), yes(&pres["xvnc"]), yes(&pres["screen_sharing"]));
}

fn main() {
    let mut json_output = false;
    for arg in env::args().skip(1) {
        match arg.as_str() {
            "--json" => json_output = true,
            "--help" | "-h" => {
                println!("Read-only SteamARM compatibility inventory. Usage: steamarm-compat-status [--json]");
                return;
            }
            _ => {
                eprintln!("steamarm-compat-status: unknown argument: {arg}");
                std::process::exit(2);
            }
        }
    }
    let report = inventory(&state_dir());
    if json_output {
        println!("{}", report);
    } else {
        print_report(&report);
    }
}
