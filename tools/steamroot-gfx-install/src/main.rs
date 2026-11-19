use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, ExitCode};

const STEAM_PATTERN: &str = "build/lxrun .*ubuntu12_32/steam ";
const X_LIBRARIES: &[&str] = &[
    "libX11.so.6",
    "libX11-xcb.so.1",
    "libxcb.so.1",
    "libXau.so.6",
    "libXdmcp.so.6",
    "libxcb-dri3.so.0",
    "libxcb-present.so.0",
    "libxcb-randr.so.0",
    "libxcb-sync.so.1",
    "libxcb-xfixes.so.0",
];

struct Config {
    repo: PathBuf,
    root: PathBuf,
    build: PathBuf,
    armlibs: PathBuf,
    test_root: PathBuf,
}

fn fail(message: impl AsRef<str>) -> ! {
    eprintln!("[install-steamroot-gfx] error: {}", message.as_ref());
    std::process::exit(1)
}

fn log(message: impl AsRef<str>) {
    println!("[install-steamroot-gfx] {}", message.as_ref());
}

fn home() -> PathBuf {
    env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"))
}

fn selected_dir(variable: &str, name: &str) -> PathBuf {
    if let Some(value) = env::var_os(variable).filter(|v| !v.is_empty()) {
        return PathBuf::from(value);
    }
    let newer = home().join("Developer").join(name);
    let legacy = home().join(name);
    if newer.is_dir() || !legacy.is_dir() {
        newer
    } else {
        legacy
    }
}

fn config() -> Config {
    let manifest_root = PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..");
    let repo = env::var_os("STEAMARM_PROJECT_ROOT")
        .map(PathBuf::from)
        .unwrap_or(manifest_root);
    let state = selected_dir("STEAMARM_STATE", "SteamARM-roots");
    let root = env::var_os("LXRT_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp/lxrt-steamroot"));
    let armlibs = env::var_os("ARMLIBS")
        .map(PathBuf::from)
        .unwrap_or_else(|| state.join("arm64root/usr/lib"));
    Config {
        repo,
        root,
        build: selected_dir("STEAMARM_BUILD", "SteamARM-build"),
        armlibs,
        test_root: env::var_os("STEAMARM_TEST_ROOT")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("/tmp/lxrt-root")),
    }
}

fn canonical(path: &Path) -> Result<PathBuf, String> {
    fs::canonicalize(path).map_err(|e| format!("cannot resolve {}: {e}", path.display()))
}

fn require_frame_root(root: &Path) -> Result<PathBuf, String> {
    let real = canonical(root)?;
    if !real.join(".lxrt-steamroot-frame-base").is_file() {
        return Err(format!(
            "{} is not a Steam Frame based root (missing .lxrt-steamroot-frame-base); run setup roots first",
            real.display()
        ));
    }
    if !real.join("usr/lib/lxrt-emu").is_dir() {
        return Err(format!("no {}", real.join("usr/lib/lxrt-emu").display()));
    }
    Ok(real)
}

fn steam_is_running() -> Result<bool, String> {
    let output = Command::new("pgrep")
        .args(["-f", STEAM_PATTERN])
        .output()
        .map_err(|e| format!("cannot run pgrep: {e}"))?;
    match output.status.code() {
        Some(0) => Ok(true),
        Some(1) => Ok(false),
        _ => Err(format!(
            "pgrep failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        )),
    }
}

fn put(source: &Path, destination: &Path) -> Result<(), String> {
    let parent = destination
        .parent()
        .ok_or_else(|| format!("no parent directory for {}", destination.display()))?;
    fs::create_dir_all(parent).map_err(|e| format!("create {}: {e}", parent.display()))?;
    let name = destination
        .file_name()
        .ok_or_else(|| format!("invalid destination {}", destination.display()))?;
    let temporary = parent.join(format!(
        "{}.new.{}",
        name.to_string_lossy(),
        std::process::id()
    ));
    let _ = fs::remove_file(&temporary);
    if let Err(error) = fs::copy(source, &temporary) {
        let _ = fs::remove_file(&temporary);
        return Err(format!("copy {}: {error}", source.display()));
    }
    if let Err(error) = fs::rename(&temporary, destination) {
        let _ = fs::remove_file(&temporary);
        return Err(format!("replace {}: {error}", destination.display()));
    }
    Ok(())
}

fn required_file(path: &Path, hint: &str) -> Result<(), String> {
    if path.is_file() {
        Ok(())
    } else {
        Err(format!("missing {} ({hint})", path.display()))
    }
}

fn copy_thunks(source: &Path, destination: &Path, include_32: bool) -> Result<(), String> {
    for (folder, file) in [
        ("HostThunks", "libvulkan-host.so"),
        ("GuestThunks", "libvulkan-guest.so"),
    ] {
        put(
            &source.join(folder).join(file),
            &destination.join(folder).join(file),
        )?;
    }
    if include_32
        && source.join("HostThunks_32/libvulkan-host.so").is_file()
        && source.join("GuestThunks_32/libvulkan-guest.so").is_file()
    {
        for (folder, file) in [
            ("HostThunks_32", "libvulkan-host.so"),
            ("GuestThunks_32", "libvulkan-guest.so"),
        ] {
            put(
                &source.join(folder).join(file),
                &destination.join(folder).join(file),
            )?;
        }
    }
    Ok(())
}

fn install(config: &Config) -> Result<(), String> {
    let root = require_frame_root(&config.root)?;
    let emulator = root.join("usr/lib/lxrt-emu");
    if steam_is_running()? {
        return Err("Steam is running (scripts/run-steam.sh --stop first)".into());
    }

    let thunk_build = config.build.join("thunks-build");
    let inputs = [
        (config.build.join("out/FEX-emu"), "build-fex-host.sh emu"),
        (
            thunk_build.join("HostThunks/libvulkan-host.so"),
            "build-fex-thunks.sh all",
        ),
        (
            thunk_build.join("GuestThunks/libvulkan-guest.so"),
            "build-fex-thunks.sh all",
        ),
        (config.repo.join("build/libvulkan.so.1"), "make shim"),
    ];
    for (path, hint) in &inputs {
        required_file(path, hint)?;
    }

    log(format!("FEX -> {}/FEX", emulator.display()));
    put(&inputs[0].0, &emulator.join("FEX"))?;

    log(format!("Vulkan thunks -> {}/thunks", emulator.display()));
    copy_thunks(&thunk_build, &emulator.join("thunks"), true)?;

    let test_root_real = fs::canonicalize(&config.test_root).ok();
    if config.test_root.join("usr").is_dir()
        && test_root_real.as_ref().is_some_and(|other| other != &root)
    {
        let other = test_root_real.expect("checked above");
        let test_thunks = other.join("usr/lib/lxrt-emu/thunks");
        log(format!("Vulkan thunks -> {}", test_thunks.display()));
        copy_thunks(&thunk_build, &test_thunks, true)?;
    }

    let shim = config.repo.join("build/libvulkan.so.1");
    log(format!(
        "Vulkan shim -> {} and {}/lib64",
        emulator.display(),
        root.display()
    ));
    put(&shim, &emulator.join("libvulkan.so.1"))?;
    put(&shim, &root.join("lib64/libvulkan.so.1"))?;

    log(format!("aarch64 X libraries -> {}", emulator.display()));
    for library in X_LIBRARIES {
        let source = config.armlibs.join(library);
        if !source.exists() {
            return Err(format!("no {}", source.display()));
        }
        put(&source, &emulator.join(library))?;
    }

    let fex_config = root.join("tmp/fexhome/.fex-emu");
    log(format!("FEX configuration -> {}", fex_config.display()));
    fs::create_dir_all(&fex_config).map_err(|e| format!("create {}: {e}", fex_config.display()))?;
    let primary_db = config.build.join("FEX-thunks/Data/ThunksDB.json");
    let fallback_db = root.join("opt/vkthunks/config/ThunksDB.json");
    let db = if primary_db.is_file() {
        primary_db
    } else if fallback_db.is_file() {
        fallback_db
    } else {
        return Err(format!(
            "missing {} and {}",
            primary_db.display(),
            fallback_db.display()
        ));
    };
    put(&db, &fex_config.join("ThunksDB.json"))?;
    atomic_write(
        &fex_config.join("Config.json"),
        br#"{"Config":{"RootFS":"Ubuntu_24_04","ThunkHostLibs":"/usr/lib/lxrt-emu/thunks/HostThunks","ThunkGuestLibs":"/usr/lib/lxrt-emu/thunks/GuestThunks"},"ThunksDB":{"Vulkan":1}}
"#,
    )?;
    let app_config = fex_config.join("AppConfig");
    fs::create_dir_all(&app_config).map_err(|e| format!("create {}: {e}", app_config.display()))?;
    atomic_write(
        &app_config.join("steam.json"),
        br#"{"Config":{"HideHypervisorBit":"1"}}
"#,
    )?;
    atomic_write(
        &app_config.join("steamwebhelper.json"),
        br#"{"ThunksDB":{"Vulkan":0}}
"#,
    )?;
    log("done");
    Ok(())
}

fn atomic_write(path: &Path, contents: &[u8]) -> Result<(), String> {
    let parent = path
        .parent()
        .ok_or_else(|| format!("no parent directory for {}", path.display()))?;
    fs::create_dir_all(parent).map_err(|e| format!("create {}: {e}", parent.display()))?;
    let name = path
        .file_name()
        .ok_or_else(|| format!("invalid path {}", path.display()))?;
    let temporary = parent.join(format!(
        "{}.new.{}",
        name.to_string_lossy(),
        std::process::id()
    ));
    fs::write(&temporary, contents).map_err(|e| format!("write {}: {e}", temporary.display()))?;
    if let Err(error) = fs::rename(&temporary, path) {
        let _ = fs::remove_file(&temporary);
        return Err(format!("replace {}: {error}", path.display()));
    }
    Ok(())
}

fn main() -> ExitCode {
    let args = env::args().skip(1).collect::<Vec<_>>();
    if args == ["--help"] || args == ["-h"] {
        println!("Usage: install-steamroot-gfx.sh [--help]\nInstalls FEX, Vulkan thunks, the Vulkan shim and FEX settings into the Steam Frame based Arch root.");
        return ExitCode::SUCCESS;
    }
    if !args.is_empty() {
        eprintln!("Usage: install-steamroot-gfx.sh [--help]");
        return ExitCode::from(2);
    }
    match install(&config()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(error) => {
            fail(error);
        }
    }
}
