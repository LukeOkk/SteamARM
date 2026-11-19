use std::env;
use std::ffi::OsStr;
use std::fs;
use std::io::{self, Read};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{self, Command, Stdio};

const DEFAULT_PROTON: &str = "Proton - Experimental";
const X86_HOME: &str = "/tmp/fexhome";
const X86_STEAM: &str = "/tmp/fexhome/.local/share/Steam";
const DEFAULT_X86_LINK: &str = "/tmp/lxrt-steamroot";
const DEFAULT_FEX_DIR: &str = "/tmp/lxrt-root/usr/bin";
const DEFAULT_FEX_BIN: &str = "FEX-gb";

const HELP: &str = "Install SteamARM's x86 Proton compatibility tool for the native ARM64 client.

Usage:
  install-fex-proton-tool.sh [--root arm64root|armroot|DIR] [--proton NAME [--unverified]] [--dry-run]
  install-fex-proton-tool.sh [--root arm64root|DIR] --linux-only [--dry-run]
  install-fex-proton-tool.sh --uninstall [--root ...]

Default root: $STEAMARM_STATE/arm64root (Steam Frame SteamOS/Arch).
The x86 root and FEX are read through /tmp/lxrt-steamroot and /tmp/lxrt-root.
--linux-only installs the native Linux x86 FEX tool using Steam Frame's Arch FEX root
and the current ARM64 Steam client's FEX-Emu and Steam Linux Runtime.
Only Proton - Experimental is measured to work; other builds require --unverified.
";

#[derive(Debug)]
struct Options {
    root: PathBuf,
    proton: String,
    unverified: bool,
    dry_run: bool,
    linux_only: bool,
    uninstall: bool,
}

fn state_dir() -> PathBuf {
    if let Some(value) = env::var_os("STEAMARM_STATE").filter(|value| !value.is_empty()) {
        return PathBuf::from(value);
    }
    let home = env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"));
    let newer = home.join("Developer/SteamARM-roots");
    let older = home.join("SteamARM-roots");
    if newer.is_dir() || !older.is_dir() {
        newer
    } else {
        older
    }
}

fn parse_args(args: &[String], state: &Path) -> Result<Option<Options>, (i32, String)> {
    let mut options = Options {
        // Steam Frame is the canonical ARM64 base. Fedora armroot remains an
        // explicit transition target for older installations.
        root: state.join("arm64root"),
        proton: DEFAULT_PROTON.into(),
        unverified: false,
        dry_run: false,
        linux_only: false,
        uninstall: false,
    };
    let mut index = 0;
    while index < args.len() {
        match args[index].as_str() {
            "--root" => {
                index += 1;
                let value = args
                    .get(index)
                    .ok_or_else(|| (1, "--root needs armroot, arm64root or a directory".into()))?;
                options.root = match value.as_str() {
                    "armroot" => state.join("armroot"),
                    "arm64root" => state.join("arm64root"),
                    other => PathBuf::from(other),
                };
            }
            "--proton" => {
                index += 1;
                options.proton = args
                    .get(index)
                    .ok_or_else(|| (1, "--proton needs a name".into()))?
                    .clone();
            }
            "--unverified" => options.unverified = true,
            "--dry-run" => options.dry_run = true,
            "--linux-only" => options.linux_only = true,
            "--uninstall" => options.uninstall = true,
            "-h" | "--help" => return Ok(None),
            other => return Err((2, format!("unknown argument: {other}"))),
        }
        index += 1;
    }
    Ok(Some(options))
}

fn x86_link() -> PathBuf {
    env::var_os("X86_LINK")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(DEFAULT_X86_LINK))
}

fn fex_dir() -> PathBuf {
    env::var_os("FEXDIR")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(DEFAULT_FEX_DIR))
}

fn fex_bin() -> String {
    env::var("FEXBIN")
        .ok()
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| DEFAULT_FEX_BIN.into())
}

fn runtime_for_app(appid: &str, proton: &str, proton_dir: &Path) -> Result<&'static str, String> {
    match appid {
        "1628350" => Ok("SteamLinuxRuntime_sniper"),
        "4183110" => Ok("SteamLinuxRuntime_4"),
        "1391110" => Ok("SteamLinuxRuntime_soldier"),
        "" => Err(format!(
            "{}/toolmanifest.vdf names no require_tool_appid: which runtime {proton} needs is unknown",
            proton_dir.display()
        )),
        other => Err(format!(
            "{proton} requires tool {other}, a runtime this installer does not know"
        )),
    }
}

fn required_appid(manifest: &Path) -> io::Result<Option<String>> {
    let content = fs::read_to_string(manifest)?;
    for line in content.lines() {
        if !line.contains("require_tool_appid") {
            continue;
        }
        let fields: Vec<&str> = line.split('"').collect();
        if fields
            .get(1)
            .is_some_and(|key| key.trim() == "require_tool_appid")
        {
            return Ok(fields.get(3).map(|value| value.trim().to_owned()));
        }
    }
    Ok(None)
}

fn wine_machine(path: &Path) -> i32 {
    let Ok(mut file) = fs::File::open(path) else {
        return -1;
    };
    let mut header = [0_u8; 20];
    if file.read_exact(&mut header).is_err() || &header[..4] != b"\x7fELF" {
        return -1;
    }
    u16::from_le_bytes([header[18], header[19]]) as i32
}

fn root_steam(root: &Path) -> PathBuf {
    root.join("tmp/armhome/.local/share/Steam")
}

fn project_root() -> PathBuf {
    let cwd = env::current_dir().unwrap_or_else(|_| PathBuf::from("."));
    if cwd.join("tools/fex-proton-install/Cargo.toml").is_file() {
        return cwd;
    }
    if let Ok(executable) = env::current_exe() {
        if let Some(resources) = executable.parent() {
            if resources
                .join("steamarm-fex-linux/toolmanifest.vdf")
                .is_file()
            {
                return resources.to_path_buf();
            }
        }
    }
    cwd
}

fn project_asset_dir(project_root: &Path, relative: &str) -> Result<PathBuf, String> {
    let candidates = [
        project_root.join(relative),
        project_root.join("tools").join(relative),
    ];
    candidates
        .into_iter()
        .find(|path| path.join("toolmanifest.vdf").is_file())
        .ok_or_else(|| format!("missing bundled or source tool assets: {relative}"))
}

fn project_asset_file(project_root: &Path, relative: &str) -> Result<PathBuf, String> {
    let candidates = [
        project_root.join(relative),
        project_root.join("tools").join(relative),
    ];
    candidates
        .into_iter()
        .find(|path| path.is_file())
        .ok_or_else(|| format!("missing bundled or source asset: {relative}"))
}

fn install_file(source: &Path, dest: &Path, executable: bool) -> io::Result<()> {
    let name = dest
        .file_name()
        .and_then(OsStr::to_str)
        .unwrap_or("steamarm-install");
    let temporary = dest.with_file_name(format!(".{name}.tmp-{}", process::id()));
    fs::copy(source, &temporary)?;
    if executable {
        fs::set_permissions(&temporary, fs::Permissions::from_mode(0o755))?;
    }
    fs::rename(temporary, dest)?;
    Ok(())
}

fn patch_steam_emulator_json(path: &Path) -> Result<(), String> {
    let original = fs::read_to_string(path)
        .map_err(|error| format!("cannot read Steam FEX metadata {}: {error}", path.display()))?;
    let stock = "\"./usr/bin/FEX\"";
    let patched = "\"./usr/bin/FEX-SteamARM\"";
    let rust_wrapper = "\"./usr/bin/steamarm-fex\"";
    let stock_server = "\"./usr/bin/FEXServerManager\"";
    let patched_server = "\"./usr/bin/steamarm-fexserver-manager\"";
    let count = original.matches(stock).count();
    let patched_count = original.matches(patched).count();
    let server_count = original.matches(stock_server).count();
    if count == 0 && patched_count == 0 && !original.contains(rust_wrapper) {
        return Err(format!(
            "Steam FEX metadata has no expected argv in {}",
            path.display()
        ));
    }
    if server_count == 0 && !original.contains(patched_server) {
        return Err(format!(
            "Steam FEX metadata has no expected server_argv in {}",
            path.display()
        ));
    }
    let mut updated = original
        .replace(stock, rust_wrapper)
        .replace(patched, rust_wrapper)
        .replace(stock_server, patched_server);

    // pressure-vessel starts FEX with ld.so as argv[0] and the game helper as
    // an argument. In that form FEX cannot inspect the eventual ET_EXEC when
    // choosing its Darwin-safe low-address window; without guest-base and
    // low-window configuration, ld.so
    // tries to map the helper at 0x400000, inside macOS PAGEZERO, and exits
    // with ENOMEM. Set the option both outside and inside the container so
    // every FEX invocation made through either argv uses the same mapping.
    // Valve's upstream portable manifest leaves the rootfs empty; Steam
    // Frame's Arch FEX Mesa root is mounted at this stable guest path.
    updated = ensure_json_string_field(
        &updated,
        "environment",
        "FEX_ROOTFS",
        "/usr/share/guestos/fex-mesa",
    )?;
    updated = ensure_json_string_field(&updated, "environment", "FEX_GUESTBASE", "1")?;
    updated = ensure_json_string_field(&updated, "environment", "FEX_LOWWINDOW", "1")?;
    updated = ensure_json_string_field(
        &updated,
        "container_environment",
        "FEX_ROOTFS",
        "/usr/share/guestos/fex-mesa",
    )?;
    updated = ensure_json_string_field(&updated, "container_environment", "FEX_GUESTBASE", "1")?;
    updated = ensure_json_string_field(&updated, "container_environment", "FEX_LOWWINDOW", "1")?;

    if updated == original {
        println!("  FEX Arch rootfs, guest-base, and low-window env already configured");
        return Ok(());
    }
    let temporary = path.with_file_name(format!("emulator.json.tmp-{}", process::id()));
    fs::write(&temporary, updated)
        .map_err(|error| format!("cannot write Steam FEX metadata: {error}"))?;
    fs::rename(&temporary, path)
        .map_err(|error| format!("cannot install Steam FEX metadata: {error}"))?;
    if count + patched_count > 0 {
        println!(
            "  routed {} Steam FEX argv entries through Rust short-path launcher",
            count + patched_count
        );
    }
    if server_count > 0 {
        println!("  routed Steam FEXServerManager through the Rust SteamARM wrapper");
    }
    println!("  configured FEX Arch rootfs, guest base, and low window");
    Ok(())
}

fn ensure_json_string_field(
    document: &str,
    object_key: &str,
    field: &str,
    value: &str,
) -> Result<String, String> {
    let object_marker = format!("\"{object_key}\"");
    let object_start = document
        .find(&object_marker)
        .ok_or_else(|| format!("Steam FEX metadata is missing {object_key}"))?;
    let open = document[object_start + object_marker.len()..]
        .find('{')
        .map(|offset| object_start + object_marker.len() + offset)
        .ok_or_else(|| format!("Steam FEX metadata has invalid {object_key}"))?;
    let close = document[open + 1..]
        .find('}')
        .map(|offset| open + 1 + offset)
        .ok_or_else(|| format!("Steam FEX metadata has invalid {object_key}"))?;
    let body = &document[open + 1..close];
    let field_marker = format!("\"{field}\"");

    if let Some(relative) = body.find(&field_marker) {
        let field_start = open + 1 + relative;
        let colon = document[field_start + field_marker.len()..]
            .find(':')
            .map(|offset| field_start + field_marker.len() + offset)
            .ok_or_else(|| format!("Steam FEX metadata has invalid {field}"))?;
        let value_start = document[colon + 1..]
            .find('"')
            .map(|offset| colon + 1 + offset + 1)
            .ok_or_else(|| format!("Steam FEX metadata has invalid {field}"))?;
        let value_end = document[value_start..]
            .find('"')
            .map(|offset| value_start + offset)
            .ok_or_else(|| format!("Steam FEX metadata has invalid {field}"))?;
        if &document[value_start..value_end] == value {
            return Ok(document.to_owned());
        }
        let mut updated = document.to_owned();
        updated.replace_range(value_start..value_end, value);
        return Ok(updated);
    }

    let insertion = if body.trim().is_empty() {
        format!("\n      \"{field}\": \"{value}\"\n    ")
    } else {
        format!(", \"{field}\": \"{value}\"")
    };
    let mut updated = document.to_owned();
    updated.insert_str(close, &insertion);
    Ok(updated)
}

fn patched_fex_binary(project_root: &Path) -> Result<PathBuf, String> {
    let mut candidates = Vec::new();
    if let Some(path) = env::var_os("STEAMARM_FEX_BINARY") {
        candidates.push(PathBuf::from(path));
    }
    candidates.extend([
        project_root.join("build/out/FEX"),
        project_root.join("FEX-SteamARM"),
    ]);
    if let Some(build) = env::var_os("STEAMARM_BUILD") {
        candidates.push(PathBuf::from(build).join("out/FEX"));
    }
    if let Some(home) = env::var_os("HOME") {
        let home = PathBuf::from(home);
        candidates.push(home.join("Developer/SteamARM-build/out/FEX"));
        candidates.push(home.join("SteamARM-build/out/FEX"));
    }
    let binary = candidates
        .into_iter()
        .find(|path| path.is_file())
        .ok_or_else(|| {
            "SteamARM-patched FEX build is missing; run the FEX host build first".to_owned()
        })?;
    if wine_machine(&binary) != 183 {
        return Err(format!(
            "{} is not an AArch64 Linux FEX binary",
            binary.display()
        ));
    }
    Ok(binary)
}

fn install_linux_tool(
    options: &Options,
    project_root: &Path,
    steam: &Path,
    dest: &Path,
) -> Result<(), (i32, String)> {
    let os_release = fs::read_to_string(options.root.join("etc/os-release"))
        .map_err(|error| (1, format!("cannot read Steam Frame OS identity: {error}")))?;
    let arch_base = os_release.lines().any(|line| {
        let line = line.trim();
        line == "ID=arch"
            || line == "ID=steamos"
            || line.strip_prefix("ID_LIKE=").is_some_and(|value| {
                value
                    .trim_matches('"')
                    .split_whitespace()
                    .any(|id| id == "arch")
            })
    });
    if !arch_base {
        return Err((
            1,
            format!(
                "{} is not an Arch/SteamOS root; SteamARM Linux FEX requires the Steam Frame Arch base",
                options.root.display()
            ),
        ));
    }
    let client = steam.join("steamrtarm64/steam");
    let fex = steam.join("steamapps/common/FEX-Emu/usr/bin/FEX");
    let runtime = steam.join("steamapps/common/SteamLinuxRuntime_sniper/_v2-entry-point");
    let rootfs = options.root.join("usr/share/guestos/fex-mesa");
    if !client.is_file() {
        return Err((
            1,
            format!("no native arm64 Steam client at {}", client.display()),
        ));
    }
    if !runtime.is_file() {
        return Err((
            1,
            format!(
                "Steam Linux Runtime sniper is missing at {}",
                runtime.display()
            ),
        ));
    }
    if !fex.is_file() {
        return Err((1, format!("Steam FEX is missing at {}", fex.display())));
    }
    let patched_fex = patched_fex_binary(project_root).map_err(|error| (1, error))?;
    if !rootfs.join("lib/ld-linux-x86-64.so.2").is_file() {
        return Err((
            1,
            format!(
                "Steam Frame Arch FEX root is incomplete at {}",
                rootfs.display()
            ),
        ));
    }
    if options.dry_run {
        println!(
            "would install the Rust Linux FEX tool into {}",
            dest.display()
        );
        println!("  Arch root: {}", options.root.display());
        println!("  Steam FEX: {}", fex.display());
        println!("  patched FEX: {}", patched_fex.display());
        println!("  runtime: {}", runtime.display());
        return Ok(());
    }

    let tool_binary = rust_tool_binary(project_root).map_err(|error| (1, error))?;
    let server_manager_binary =
        rust_server_manager_binary(project_root).map_err(|error| (1, error))?;
    let fex_wrapper_binary = rust_fex_wrapper_binary(project_root).map_err(|error| (1, error))?;
    fs::create_dir_all(dest)
        .map_err(|error| (1, format!("cannot create {}: {error}", dest.display())))?;
    let assets =
        project_asset_dir(project_root, "steamarm-fex-linux").map_err(|error| (1, error))?;
    for name in ["toolmanifest.vdf", "compatibilitytool.vdf"] {
        install_file(&assets.join(name), &dest.join(name), false).map_err(|error| {
            (
                1,
                format!("cannot install {}: {error}", dest.join(name).display()),
            )
        })?;
    }
    install_file(&tool_binary, &dest.join("steamarm-fex-linux"), true)
        .map_err(|error| (1, format!("cannot install Rust tool: {error}")))?;
    let patched_fex_dest = fex.with_file_name("FEX-SteamARM");
    install_file(&patched_fex, &patched_fex_dest, true).map_err(|error| {
        (
            1,
            format!(
                "cannot install patched FEX at {}: {error}",
                patched_fex_dest.display()
            ),
        )
    })?;
    let fex_wrapper_dest = fex
        .parent()
        .ok_or_else(|| {
            (
                1,
                format!("cannot locate FEX usr/bin from {}", fex.display()),
            )
        })?
        .join("steamarm-fex");
    install_file(&fex_wrapper_binary, &fex_wrapper_dest, true).map_err(|error| {
        (
            1,
            format!(
                "cannot install Rust FEX launcher at {}: {error}",
                fex_wrapper_dest.display()
            ),
        )
    })?;
    // Valve's FEXCompatTool hard-codes ./usr/bin/FEX and ignores emulator.json
    // for its interpreter path. Route that entry point through our Rust
    // wrapper too, while retaining the upstream binary for recovery/debugging.
    let fex_is_wrapper = fs::read(&fex)
        .and_then(|installed| fs::read(&fex_wrapper_binary).map(|wrapper| installed == wrapper))
        .map_err(|error| {
            (
                1,
                format!(
                    "cannot inspect Steam FEX entry point {}: {error}",
                    fex.display()
                ),
            )
        })?;
    if !fex_is_wrapper {
        let upstream_fex = fex.with_file_name("FEX-Valve");
        install_file(&fex, &upstream_fex, true).map_err(|error| {
            (
                1,
                format!(
                    "cannot preserve upstream Steam FEX at {}: {error}",
                    upstream_fex.display()
                ),
            )
        })?;
    }
    install_file(&fex_wrapper_binary, &fex, true).map_err(|error| {
        (
            1,
            format!(
                "cannot route Valve's FEX entry point through Rust at {}: {error}",
                fex.display()
            ),
        )
    })?;
    let server_manager_dest = fex
        .parent()
        .ok_or_else(|| {
            (
                1,
                format!("cannot locate FEX usr/bin from {}", fex.display()),
            )
        })?
        .join("steamarm-fexserver-manager");
    install_file(&server_manager_binary, &server_manager_dest, true).map_err(|error| {
        (
            1,
            format!(
                "cannot install Rust FEX server manager at {}: {error}",
                server_manager_dest.display()
            ),
        )
    })?;
    let emulator = fex
        .parent()
        .and_then(Path::parent)
        .and_then(Path::parent)
        .ok_or_else(|| {
            (
                1,
                format!("cannot locate FEX-Emu directory from {}", fex.display()),
            )
        })?
        .join("emulator.json");
    patch_steam_emulator_json(&emulator).map_err(|error| (1, error))?;
    println!("installed {}", dest.display());
    println!("  Steam Frame Arch FEX root: {}", rootfs.display());
    println!("  Rust FEX launchers route game and server through short per-app socket paths");
    println!("  Valve's hard-coded FEX entry point now routes through Rust to FEX-SteamARM");
    println!("  original Valve interpreter preserved as FEX-Valve");
    println!("  patched FEX-SteamARM and sniper runtime installed beside Steam FEX");
    println!("  restart Steam to reload compatibility tools");
    Ok(())
}

fn rust_tool_binary(project_root: &Path) -> Result<PathBuf, String> {
    let mut candidates = Vec::new();
    if let Some(path) = env::var_os("STEAMARM_FEX_PROTON_BIN") {
        candidates.push(PathBuf::from(path));
    }
    candidates.push(project_root.join("build/steamarm-fex-proton-arm64"));
    if let Ok(executable) = env::current_exe() {
        if let Some(resources) = executable.parent() {
            candidates.push(resources.join("steamarm-fex-proton-arm64"));
        }
    }
    let Some(path) = candidates.into_iter().find(|path| path.is_file()) else {
        return Err("no prebuilt ARM64 Rust compatibility tool; run make fex-proton-tool".into());
    };
    if wine_machine(&path) != 183 {
        return Err(format!(
            "{} is not an AArch64 Linux executable; rebuild with make fex-proton-tool",
            path.display()
        ));
    }
    Ok(path)
}

fn rust_server_manager_binary(project_root: &Path) -> Result<PathBuf, String> {
    let mut candidates = Vec::new();
    if let Some(path) = env::var_os("STEAMARM_FEX_SERVER_MANAGER_BIN") {
        candidates.push(PathBuf::from(path));
    }
    candidates.push(project_root.join("build/steamarm-fexserver-manager-arm64"));
    candidates.push(project_root.join("build/steamarm-fexserver-manager"));
    candidates.push(
        project_root.join("build/rust-fex-proton-install/release/steamarm-fexserver-manager"),
    );
    if let Ok(executable) = env::current_exe() {
        if let Some(resources) = executable.parent() {
            candidates.push(resources.join("steamarm-fexserver-manager-arm64"));
            candidates.push(resources.join("steamarm-fexserver-manager"));
        }
    }
    let path = candidates
        .into_iter()
        .find(|path| path.is_file())
        .ok_or_else(|| {
            "no prebuilt ARM64 Rust FEX server manager; run make fex-proton-install".to_owned()
        })?;
    if wine_machine(&path) != 183 {
        return Err(format!(
            "{} is not an AArch64 Linux executable",
            path.display()
        ));
    }
    Ok(path)
}

fn rust_fex_wrapper_binary(project_root: &Path) -> Result<PathBuf, String> {
    let mut candidates = Vec::new();
    if let Some(path) = env::var_os("STEAMARM_FEX_LAUNCHER_BIN") {
        candidates.push(PathBuf::from(path));
    }
    candidates.push(project_root.join("build/steamarm-fex-arm64"));
    if let Ok(executable) = env::current_exe() {
        if let Some(resources) = executable.parent() {
            candidates.push(resources.join("steamarm-fex-arm64"));
        }
    }
    let path = candidates
        .into_iter()
        .find(|path| path.is_file())
        .ok_or_else(|| {
            "no prebuilt ARM64 Rust FEX launcher; run make fex-proton-install".to_owned()
        })?;
    if wine_machine(&path) != 183 {
        return Err(format!(
            "{} is not an AArch64 Linux executable",
            path.display()
        ));
    }
    Ok(path)
}

fn remove_any(path: &Path) -> io::Result<()> {
    let metadata = match fs::symlink_metadata(path) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error),
    };
    if metadata.file_type().is_symlink() || metadata.is_file() {
        fs::remove_file(path)
    } else if metadata.is_dir() {
        fs::remove_dir_all(path)
    } else {
        fs::remove_file(path)
    }
}

fn env_links(project_root: &Path) -> Result<(), String> {
    if env::var("NO_ENV_LINKS").as_deref() == Ok("1") {
        return Ok(());
    }
    let source_script = project_root.join("scripts/env-links.sh");
    let bundled_tool = project_root.join("steamarm-env-links");
    let (program, current_dir) = if source_script.is_file() {
        (source_script, project_root.to_path_buf())
    } else if bundled_tool.is_file() {
        (bundled_tool, project_root.to_path_buf())
    } else {
        return Err("missing scripts/env-links.sh and bundled steamarm-env-links".into());
    };
    let status = Command::new(program)
        .current_dir(current_dir)
        .stdout(Stdio::null())
        .status()
        .map_err(|error| format!("cannot update SteamARM root links: {error}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("scripts/env-links.sh failed with {status}"))
    }
}

fn checked_paths(
    options: &Options,
    project_root: &Path,
) -> Result<(String, String, String, String), String> {
    let steam = root_steam(&options.root);
    let client = steam.join("steamrtarm64/steam");
    if !client.is_file() {
        return Err(format!(
            "no native arm64 client at {} (docs/STEAM_ARM64_BRINGUP.md)",
            client.display()
        ));
    }
    if options.proton.is_empty()
        || options.proton == "."
        || options.proton == ".."
        || options.proton.contains('/')
    {
        return Err("--proton takes a directory name in steamapps/common, not a path".into());
    }
    if options.proton.contains("ARM64") || options.proton.contains("arm64") {
        return Err(format!(
            "{}: Proton ARM64 cannot run on macOS (benchmarks/stage18); pick an x86 Proton",
            options.proton
        ));
    }

    let status = match options.proton.as_str() {
        DEFAULT_PROTON => "",
        "Proton 10.0" => "measured NOT to start: its Wine does not finish starting under FEX in 240-300 s, on the x86 client's own path as well (benchmarks/stage24-fex-game-boundary.txt)",
        _ => "not measured with this tool",
    };
    if !status.is_empty() {
        if !options.unverified {
            return Err(format!(
                "{} is {status}; --unverified installs it anyway",
                options.proton
            ));
        }
        eprintln!("warning: {} is {status}", options.proton);
    }

    env_links(project_root)?;
    let x86 = x86_link();
    if !x86.is_dir() {
        return Err(format!(
            "no x86 Steam root at {} (scripts/env-links.sh; scripts/setup.sh installs it)",
            x86.display()
        ));
    }
    let common = x86
        .join(X86_STEAM.trim_start_matches('/'))
        .join("steamapps/common");
    let proton = common.join(&options.proton);
    let wine = proton.join("files/bin/wine");
    if !proton.join("proton").is_file() || !wine.is_file() {
        return Err(format!(
            "x86 {} is not installed in the x86 Steam root ({}); install it from the x86 client's library",
            options.proton,
            proton.display()
        ));
    }
    if proton.join("files/bin-arm64").is_dir() {
        return Err(format!(
            "{} is an ARM64 build: Proton ARM64 cannot run on macOS",
            proton.display()
        ));
    }
    let machine = wine_machine(&wine);
    if machine != 62 && machine != 3 {
        return Err(format!(
            "{} is not an x86 ELF (e_machine {machine})",
            wine.display()
        ));
    }
    let appid = required_appid(&proton.join("toolmanifest.vdf"))
        .map_err(|error| format!("cannot read {}/toolmanifest.vdf: {error}", proton.display()))?
        .unwrap_or_default();
    let runtime = runtime_for_app(&appid, &options.proton, &proton)?;
    if !common.join(runtime).join("_v2-entry-point").is_file() {
        return Err(format!(
            "{} requires {} (app {}), which is not installed in the x86 Steam root",
            options.proton, runtime, appid
        ));
    }
    let fex_dir = fex_dir();
    let fex_bin = fex_bin();
    let fex_path = fex_dir.join(&fex_bin);
    if !fex_path.is_file() {
        return Err(format!(
            "no FEX at {} (scripts/setup.sh builds it)",
            fex_path.display()
        ));
    }
    Ok((
        x86.to_string_lossy().into_owned(),
        runtime.into(),
        fex_dir.to_string_lossy().into_owned(),
        appid,
    ))
}

fn run() -> Result<(), (i32, String)> {
    let project_root = project_root();
    let state = state_dir();
    let args: Vec<String> = env::args().skip(1).collect();
    let Some(options) = parse_args(&args, &state)? else {
        print!("{HELP}");
        return Ok(());
    };
    let steam = root_steam(&options.root);
    let dest = steam.join("compatibilitytools.d/steamarm-fex-proton");
    let linux_dest = steam.join("compatibilitytools.d/steamarm-fex-linux");
    if options.uninstall {
        if options.dry_run {
            if dest.exists() {
                println!("would remove {}", dest.display());
            }
            if linux_dest.exists() {
                println!("would remove {}", linux_dest.display());
            }
            return Ok(());
        }
        let mut removed = false;
        for path in [&dest, &linux_dest] {
            if path.exists() {
                remove_any(path)
                    .map_err(|error| (1, format!("cannot remove {}: {error}", path.display())))?;
                println!("removed {}", path.display());
                removed = true;
            }
        }
        if !removed {
            println!("not installed: {}", dest.display());
        }
        return Ok(());
    }

    if options.linux_only {
        return install_linux_tool(&options, &project_root, &steam, &linux_dest);
    }

    let (x86, runtime, fex_dir, appid) =
        checked_paths(&options, &project_root).map_err(|error| (1, error))?;
    let fex_bin = fex_bin();
    let conf = format!(
        "# Written by scripts/install-fex-proton-tool.sh; read by steamarm-fex-proton.\n# Host paths are the runtime's /tmp links (scripts/env-links.sh).\nX86_ROOT={x86}\nX86_HOME={X86_HOME}\nX86_STEAM={X86_STEAM}\nPROTON={}\nRUNTIME={runtime}\nFEXDIR={fex_dir}\nFEXBIN={fex_bin}",
        options.proton
    );
    if options.dry_run {
        println!(
            "would install tools/steamarm-fex-proton into {} with:",
            dest.display()
        );
        println!("{conf}");
        return Ok(());
    }

    let tool_binary = rust_tool_binary(&project_root).map_err(|error| (1, error))?;
    let anticheat_list = project_asset_file(&project_root, "ffx-metalfx/anticheat-appids.txt")
        .map_err(|error| (1, error))?;

    fs::create_dir_all(&dest)
        .map_err(|error| (1, format!("cannot create {}: {error}", dest.display())))?;
    let assets =
        project_asset_dir(&project_root, "steamarm-fex-proton").map_err(|error| (1, error))?;
    for name in ["toolmanifest.vdf", "compatibilitytool.vdf"] {
        install_file(&assets.join(name), &dest.join(name), false).map_err(|error| {
            (
                1,
                format!("cannot install {}: {error}", dest.join(name).display()),
            )
        })?;
    }
    install_file(&tool_binary, &dest.join("steamarm-fex-proton"), true)
        .map_err(|error| (1, format!("cannot install Rust tool: {error}")))?;
    install_file(&anticheat_list, &dest.join("anticheat-appids.txt"), false)
        .map_err(|error| (1, format!("cannot install anti-cheat list: {error}")))?;
    fs::write(dest.join("steamarm-fex-proton.conf"), format!("{conf}\n"))
        .map_err(|error| (1, format!("cannot write tool config: {error}")))?;
    println!("installed {}", dest.display());
    println!(
        "  x86 {} + {} (app {}) from {}, FEX {}/{}",
        options.proton, runtime, appid, x86, fex_dir, fex_bin
    );

    install_linux_tool(&options, &project_root, &steam, &linux_dest)?;
    println!("  restart the client so that it rereads compatibilitytools.d (whether it lists the tool before a sign-in: docs/FEX_GAME_BOUNDARY.md)");
    Ok(())
}

fn main() {
    if let Err((code, message)) = run() {
        eprintln!("install-fex-proton-tool: {message}");
        std::process::exit(code);
    }
}
