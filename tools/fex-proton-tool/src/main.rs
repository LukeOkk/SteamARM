use std::collections::BTreeMap;
use std::env;
use std::ffi::{OsStr, OsString};
use std::fs;
use std::os::unix::fs::{symlink, FileTypeExt, PermissionsExt};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{self, Command};

#[path = "../../fex-proton-install/src/compat_path.rs"]
mod compat_path;

const CHECK_MODE: &str = "--steamarm-fex-proton-check";
const FFX_DIR: &str = "/opt/steamarm/wine";
const FFX_DLLS: &str = "amd_fidelityfx_dx12,amd_fidelityfx_vk,amd_fidelityfx_upscaler_dx12";
const FFX_DLL_NAMES: [&str; 3] = [
    "amd_fidelityfx_dx12",
    "amd_fidelityfx_vk",
    "amd_fidelityfx_upscaler_dx12",
];
const RS: char = '\u{1e}';
const US: char = '\u{1f}';

#[derive(Debug)]
struct Failure {
    code: i32,
    message: String,
    reported: bool,
}

type Result<T> = std::result::Result<T, Failure>;

fn failure(code: i32, message: impl Into<String>) -> Failure {
    Failure {
        code,
        message: message.into(),
        reported: false,
    }
}

#[derive(Clone)]
struct Config {
    x86_root: PathBuf,
    x86_home: PathBuf,
    x86_steam: PathBuf,
    proton: String,
    runtime: String,
    fex_dir: PathBuf,
    fex_bin: String,
}

fn env_string(name: &str) -> Option<String> {
    env::var_os(name).map(|v| v.to_string_lossy().into_owned())
}

fn env_nonempty(name: &str) -> Option<String> {
    env_string(name).filter(|v| !v.is_empty())
}

fn set_default(name: &str, value: &str) {
    if env::var_os(name).is_none_or(|v| v.is_empty()) {
        env::set_var(name, value);
    }
}

fn set_if_source(source: &str, target: &str) {
    if let Some(value) = env::var_os(source) {
        if env::var_os(target).is_none_or(|v| v.is_empty()) {
            env::set_var(target, value);
        }
    }
}

fn program_name() -> String {
    env::args_os()
        .next()
        .and_then(|arg| Path::new(&arg).file_name().map(OsStr::to_os_string))
        .unwrap_or_else(|| OsString::from("steamarm-fex-proton"))
        .to_string_lossy()
        .into_owned()
}

fn say(name: &str, message: &str) {
    eprintln!("{name}: {message}");
}

fn die(name: &str, code: i32, message: impl Into<String>) -> Failure {
    let message = message.into();
    say(name, &message);
    Failure {
        code,
        message,
        reported: true,
    }
}

fn tool_dir() -> PathBuf {
    env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| env::current_dir().unwrap_or_else(|_| PathBuf::from(".")))
}

fn load_config(name: &str, linux: bool) -> Result<Config> {
    let dir = tool_dir();
    let config_path = env::var_os("STEAMARM_FEX_PROTON_CONFIG")
        .map(PathBuf::from)
        .unwrap_or_else(|| dir.join(format!("{name}.conf")));
    let mut values = BTreeMap::<String, String>::new();
    if config_path.is_file() {
        let content = fs::read_to_string(&config_path).map_err(|e| {
            failure(
                3,
                format!("{}: cannot read config: {e}", config_path.display()),
            )
        })?;
        for (index, raw) in content.lines().enumerate() {
            let line = raw.trim_end_matches('\r');
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            let Some((key, value)) = line.split_once('=') else {
                return Err(failure(
                    3,
                    format!(
                        "{}:{}: not KEY=VALUE: {line}",
                        config_path.display(),
                        index + 1
                    ),
                ));
            };
            if !matches!(
                key,
                "X86_ROOT" | "X86_HOME" | "X86_STEAM" | "PROTON" | "RUNTIME" | "FEXDIR" | "FEXBIN"
            ) {
                return Err(failure(
                    3,
                    format!("{}: unknown key {key}", config_path.display()),
                ));
            }
            values.insert(key.to_owned(), value.to_owned());
        }
    }

    let get = |key: &str, default: &str| {
        values
            .get(key)
            .cloned()
            .unwrap_or_else(|| default.to_owned())
    };
    let proton_default = if linux { "" } else { "Proton - Experimental" };
    let runtime_default = if linux {
        "SteamLinuxRuntime_sniper"
    } else {
        "SteamLinuxRuntime_4"
    };
    let config = Config {
        x86_root: PathBuf::from(get("X86_ROOT", "/tmp/lxrt-steamroot")),
        x86_home: PathBuf::from(get("X86_HOME", "/tmp/fexhome")),
        x86_steam: PathBuf::from(get("X86_STEAM", "/tmp/fexhome/.local/share/Steam")),
        proton: get("PROTON", proton_default),
        runtime: get("RUNTIME", runtime_default),
        fex_dir: PathBuf::from(get("FEXDIR", "/tmp/lxrt-root/usr/bin")),
        fex_bin: get("FEXBIN", "FEX-gb"),
    };

    for path in [
        &config.x86_root,
        &config.x86_home,
        &config.x86_steam,
        &config.fex_dir,
    ] {
        if !path.is_absolute() {
            return Err(die(
                name,
                3,
                format!(
                    "{}: '{}' is not an absolute path",
                    config_path.display(),
                    path.display()
                ),
            ));
        }
    }
    if [&config.proton, &config.runtime, &config.fex_bin]
        .iter()
        .any(|v| v.contains('/'))
    {
        return Err(die(
            name,
            3,
            format!(
                "{}: PROTON, RUNTIME and FEXBIN are names, not paths",
                config_path.display()
            ),
        ));
    }
    if config.proton.contains("ARM64") || config.proton.contains("arm64") {
        return Err(die(
            name,
            6,
            format!(
                "refusing {}: Proton ARM64 cannot run on macOS (benchmarks/stage18)",
                config.proton
            ),
        ));
    }
    if !linux && config.proton.is_empty() {
        return Err(die(
            name,
            3,
            format!("{}: PROTON is empty", config_path.display()),
        ));
    }
    Ok(config)
}

fn check_mode(args: &[OsString]) -> i32 {
    // Arguments after CHECK_MODE: marker, x86 bind, FEX bind, Proton dir,
    // SLR dir, FEX name, FEXServer socket, Proton name, optional FFX dir.
    if args.len() < 9 {
        println!("invalid-check-arguments");
        return 2;
    }
    let strings: Vec<String> = args
        .iter()
        .map(|v| v.to_string_lossy().into_owned())
        .collect();
    let mut missing = String::new();
    let x86 = &strings[1];
    let fex = &strings[2];
    let proton_dir = &strings[3];
    let slr_dir = &strings[4];
    let fex_bin = &strings[5];
    let socket = &strings[6];
    let proton = &strings[7];
    let ffx_dir = &strings[8];
    let joined = |root: &str, path: &str| PathBuf::from(format!("{root}{path}"));

    if !proton.is_empty() {
        if !joined(x86, &format!("{proton_dir}/proton")).is_file() {
            missing.push_str(&format!(" proton:{proton_dir}"));
        }
        if !joined(x86, &format!("{proton_dir}/files/bin/wine")).is_file() {
            missing.push_str(&format!(" wine:{proton_dir}"));
        }
        if joined(x86, &format!("{proton_dir}/files/bin-arm64")).is_dir() {
            missing.push_str(&format!(" arm64:{proton_dir}"));
        }
    }
    if !joined(x86, &format!("{slr_dir}/_v2-entry-point")).is_file() {
        missing.push_str(&format!(" runtime:{slr_dir}"));
    }
    if !joined(fex, &format!("/{fex_bin}")).is_file() {
        missing.push_str(&format!(" fex:{fex_bin}"));
    }
    let socket_is_socket = fs::metadata(socket)
        .map(|metadata| metadata.file_type().is_socket())
        .unwrap_or(false);
    if !socket_is_socket {
        missing.push_str(" fexserver");
    }
    if !ffx_dir.is_empty()
        && FFX_DLL_NAMES
            .iter()
            .any(|dll| !joined(x86, &format!("{ffx_dir}/x86_64-windows/{dll}.dll")).is_file())
    {
        missing.push_str(" noffx");
    }
    println!("missing:{missing}");
    0
}

fn normalize_path(path: &str) -> String {
    if path == "/" {
        return path.to_owned();
    }
    path.trim_end_matches('/').to_owned()
}

fn is_same_or_child(path: &str, base: &str) -> bool {
    path == base || (base == "/" && path.starts_with('/')) || path.starts_with(&format!("{base}/"))
}

fn is_system_path(path: &str) -> bool {
    [
        "/tmp", "/usr", "/lib", "/lib64", "/lib32", "/bin", "/sbin", "/etc", "/proc", "/sys",
        "/dev", "/run", "/var", "/opt", "/home", "/root",
    ]
    .iter()
    .any(|root| is_same_or_child(path, root))
}

fn has_dot_component(path: &str) -> bool {
    Path::new(path).components().any(|part| {
        matches!(
            part,
            std::path::Component::CurDir | std::path::Component::ParentDir
        )
    })
}

#[derive(Default)]
struct BindSet {
    paths: Vec<String>,
    table: String,
}

impl BindSet {
    fn add(
        &mut self,
        name: &str,
        source: &str,
        value: Option<OsString>,
        arm_root: &str,
        config: &Config,
        allow_guest_home: bool,
    ) {
        let Some(value) = value else { return };
        let path = value.to_string_lossy();
        if !path.starts_with('/') {
            return;
        }
        let path = normalize_path(&path);
        if path.is_empty() || has_dot_component(&path) {
            say(
                name,
                &format!("not binding {source} '{path}': not a plain path"),
            );
            return;
        }
        let x86_home = normalize_path(&config.x86_home.to_string_lossy());
        let x86_root = normalize_path(&config.x86_root.to_string_lossy());
        if is_same_or_child(&path, &x86_home) || is_same_or_child(&path, &x86_root) {
            say(
                name,
                &format!("not binding {source} '{path}': the x86 side's own"),
            );
            return;
        }
        if is_same_or_child(&x86_home, &path) {
            say(
                name,
                &format!("not binding {source} '{path}': it holds the x86 home"),
            );
            return;
        }
        // SteamARM's ARM64 home lives at /tmp/armhome inside the isolated
        // guest root. Bind that one HOME into FEX; keep other /tmp paths out.
        let guest_tmp_home = allow_guest_home && path != "/tmp" && is_same_or_child(&path, "/tmp");
        if is_system_path(&path) && !guest_tmp_home {
            say(
                name,
                &format!("not binding {source} '{path}': a system path of the x86 root"),
            );
            return;
        }
        if self
            .paths
            .iter()
            .any(|bound| is_same_or_child(&path, bound))
        {
            return;
        }
        self.paths.push(path.clone());
        let host_path = if arm_root == "/" {
            path.clone()
        } else {
            format!("{arm_root}{path}")
        };
        self.table
            .push_str(&format!("{path}{RS}{host_path}{RS}0{US}"));
    }
}

fn show_table(table: &str) -> String {
    table.replace(RS, "|").replace(US, ";")
}

fn ffx_list_path() -> PathBuf {
    let dir = tool_dir();
    let local = dir.join("anticheat-appids.txt");
    if local.is_file() {
        return local;
    }
    let checkout = dir.join("../ffx-metalfx/anticheat-appids.txt");
    if checkout.is_file() {
        return checkout;
    }
    checkout
}

fn ffx_game_dirs(executable: &str) -> Vec<PathBuf> {
    let path = Path::new(executable);
    let Some(parent) = path.parent().filter(|p| p.is_absolute()) else {
        return Vec::new();
    };
    let components: Vec<OsString> = parent
        .components()
        .filter_map(|c| match c {
            std::path::Component::RootDir => None,
            other => Some(other.as_os_str().to_os_string()),
        })
        .collect();
    if let Some(index) = components
        .windows(3)
        .position(|w| w[0] == OsStr::new("steamapps") && w[1] == OsStr::new("common"))
    {
        if index + 2 < components.len() {
            let top_parts = &components[..index + 3];
            let top = PathBuf::from("/").join(top_parts.iter().collect::<PathBuf>());
            let mut dirs = Vec::new();
            let mut current = parent.to_path_buf();
            loop {
                dirs.push(current.clone());
                if current == top {
                    break;
                }
                let Some(next) = current.parent() else { break };
                if next == current {
                    break;
                }
                current = next.to_path_buf();
            }
            return dirs;
        }
    }
    vec![parent.to_path_buf()]
}

fn add_unique_dir(dirs: &mut Vec<PathBuf>, path: PathBuf) {
    if path.is_dir() && !dirs.iter().any(|item| item == &path) {
        dirs.push(path);
    }
}

fn has_anticheat(path: &Path) -> Option<PathBuf> {
    let entries = fs::read_dir(path).ok()?;
    for entry in entries.flatten() {
        let file = entry.path();
        let name = entry.file_name().to_string_lossy().to_ascii_lowercase();
        let exact_dir = name == "easyanticheat" || name == "battleye";
        let matching_file = name.starts_with("easyanticheat_eos")
            || (name.starts_with("beservice") && name.ends_with(".exe"))
            || name == "start_protected_game.exe";
        if (exact_dir && file.is_dir()) || matching_file {
            return Some(file);
        }
    }
    None
}

fn app_is_denied(appid: &str, list: &Path) -> bool {
    if appid.is_empty() {
        return false;
    }
    let Ok(content) = fs::read_to_string(list) else {
        return false;
    };
    content.lines().any(|line| {
        line.split('#')
            .next()
            .unwrap_or_default()
            .chars()
            .filter(|ch| !ch.is_whitespace())
            .collect::<String>()
            == appid
    })
}

#[derive(Default)]
struct FfxDecision {
    why: String,
    dirs: Vec<PathBuf>,
}

fn ffx_decide(executable: &str) -> FfxDecision {
    let mut decision = FfxDecision::default();
    decision.dirs = ffx_game_dirs(executable);
    if let Some(path) = env_nonempty("STEAM_COMPAT_INSTALL_PATH") {
        add_unique_dir(
            &mut decision.dirs,
            PathBuf::from(path.trim_end_matches('/')),
        );
    }
    for dir in &decision.dirs {
        if let Some(path) = has_anticheat(dir) {
            decision.why = format!("anti-cheat: {}", path.display());
            break;
        }
    }
    let appid = env_nonempty("STEAM_COMPAT_APP_ID")
        .or_else(|| env_nonempty("SteamAppId"))
        .or_else(|| env_nonempty("SteamGameId"))
        .unwrap_or_default();
    if decision.why.is_empty() && app_is_denied(&appid, &ffx_list_path()) {
        decision.why = format!("app {appid} is in {}", ffx_list_path().display());
    }
    decision
}

fn ffx_apply(dir: &str) {
    append_colon("WINEDLLPATH", dir);
    let current = env_string("WINEDLLOVERRIDES").unwrap_or_default();
    if !(current == format!("{FFX_DLLS}=b,n") || current.starts_with(&format!("{FFX_DLLS}=b,n;"))) {
        let value = if current.is_empty() {
            format!("{FFX_DLLS}=b,n")
        } else {
            format!("{FFX_DLLS}=b,n;{current}")
        };
        env::set_var("WINEDLLOVERRIDES", value);
    }
    append_colon("PRESSURE_VESSEL_FILESYSTEMS_RO", dir);
}

fn append_colon(name: &str, item: &str) {
    let current = env_string(name).unwrap_or_default();
    if current.split(':').any(|part| part == item) {
        return;
    }
    env::set_var(
        name,
        if current.is_empty() {
            item.to_owned()
        } else {
            format!("{current}:{item}")
        },
    );
}

fn ffx_say(name: &str, decision: &FfxDecision, source: &str) {
    if decision.why.is_empty() {
        say(
            name,
            &format!(
                "FSR 3.1/4 -> MetalFX: SteamARM's FidelityFX DLLs from {source} ({FFX_DLLS}=b,n)"
            ),
        );
    } else {
        say(
            name,
            &format!("FSR 3.1/4 -> MetalFX off for this game ({}): its own FidelityFX DLLs load untouched", decision.why),
        );
    }
}

fn show_ffx_env() {
    for name in [
        "WINEDLLPATH",
        "WINEDLLOVERRIDES",
        "PRESSURE_VESSEL_FILESYSTEMS_RO",
    ] {
        if let Some(value) = env::var_os(name) {
            println!("env: {name}={}", value.to_string_lossy());
        }
    }
}

fn exec_command(name: &str, command: &mut Command) -> Failure {
    let error = command.exec();
    die(name, 127, format!("cannot exec command: {error}"))
}

fn args_display(args: &[OsString]) -> String {
    args.iter()
        .map(|arg| format!(" [{}]", arg.to_string_lossy()))
        .collect::<String>()
}

fn ffx_only(name: &str, args: &[OsString]) -> Result<()> {
    if args.is_empty() || args[0] == OsStr::new("-h") || args[0] == OsStr::new("--help") {
        return Err(die(
            name,
            2,
            format!("usage: {name} <command...>  (a game's launch options in SteamARM's x86 Steam client: {FFX_DIR}/{name} %command%)"),
        ));
    }
    if env_nonempty("STEAMARM_WIN_UPSCALER").as_deref() == Some("metalfx") {
        let mut executable = String::new();
        let mut previous = String::new();
        let mut before_previous = String::new();
        for arg in args {
            let current = arg.to_string_lossy().into_owned();
            if before_previous.ends_with("/proton") {
                executable = current;
                break;
            }
            before_previous = previous;
            previous = current;
        }
        if executable.is_empty() {
            executable = args
                .iter()
                .map(|arg| arg.to_string_lossy().into_owned())
                .find(|arg| arg.to_ascii_lowercase().ends_with(".exe"))
                .unwrap_or_default();
        }
        let dir = fs::canonicalize(tool_dir())
            .unwrap_or_else(|_| tool_dir())
            .to_string_lossy()
            .into_owned();
        let mut decision = ffx_decide(&executable);
        if decision.why.is_empty() {
            for dll in FFX_DLL_NAMES {
                let path = Path::new(&dir)
                    .join("x86_64-windows")
                    .join(format!("{dll}.dll"));
                if !path.is_file() {
                    decision.why =
                        format!("no {} (scripts/install-ffx-metalfx.sh)", path.display());
                    break;
                }
            }
        }
        if decision.why.is_empty() {
            ffx_apply(&dir);
        }
        ffx_say(name, &decision, &dir);
    }
    if env_nonempty("STEAMARM_FEX_PROTON_DRYRUN").as_deref() == Some("1") {
        show_ffx_env();
        println!("exec:{}", args_display(args));
        return Ok(());
    }
    let mut command = Command::new(&args[0]);
    command.args(&args[1..]);
    Err(exec_command(name, &mut command))
}

fn make_check_mounts(config: &Config, uid: &str) -> String {
    let x86 = "/run/steamarm-x86";
    let fex = "/run/steamarm-fex";
    let sock = "/run/steamarm-fexserver";
    format!(
        "{x86}{RS}{}{RS}1{US}{fex}{RS}{}{RS}1{US}{sock}{RS}/tmp/lxrt-abstract-{uid}{RS}1{US}",
        config.x86_root.display(),
        config.fex_dir.display()
    )
}

fn run_check(
    config: &Config,
    proton_dir: &str,
    slr_dir: &str,
    uid: &str,
    ffx_dir: &str,
    dry_run: bool,
) -> Result<String> {
    let x86 = "/run/steamarm-x86";
    let fex = "/run/steamarm-fex";
    let socket = format!("/run/steamarm-fexserver/{uid}.FEXServer.Socket");
    let mounts = make_check_mounts(config, uid);
    if dry_run {
        println!(
            "check: LXRT_MOUNTS={} [x86 root {}: {}{}; {}/_v2-entry-point; {}/{}; FEXServer socket /tmp/lxrt-abstract-{uid}/{uid}.FEXServer.Socket{}]",
            show_table(&mounts),
            config.x86_root.display(),
            if config.proton.is_empty() { String::from("native Linux") } else { proton_dir.to_owned() },
            if config.proton.is_empty() { String::new() } else { String::from("/proton") },
            slr_dir,
            config.fex_dir.display(),
            config.fex_bin,
            if ffx_dir.is_empty() { String::new() } else { format!("; FidelityFX DLLs {ffx_dir}/x86_64-windows") }
        );
        return Ok(String::new());
    }

    let executable = env::current_exe()
        .map_err(|e| failure(4, format!("could not locate tool for x86-side check: {e}")))?;
    let args = [
        OsString::from("check"),
        OsString::from(x86),
        OsString::from(fex),
        OsString::from(proton_dir),
        OsString::from(slr_dir),
        OsString::from(&config.fex_bin),
        OsString::from(socket),
        OsString::from(&config.proton),
        OsString::from(ffx_dir),
    ];
    let output = Command::new(executable)
        .arg(CHECK_MODE)
        .args(args)
        .env("LXRT_MOUNTS", &mounts)
        .output()
        .map_err(|e| failure(4, format!("could not check the x86 side (output: '{e}')")))?;
    let stdout = String::from_utf8_lossy(&output.stdout).trim().to_owned();
    if !output.status.success() || !stdout.starts_with("missing:") {
        return Err(failure(
            4,
            format!("could not check the x86 side (output: '{stdout}')"),
        ));
    }
    Ok(stdout.trim_start_matches("missing:").trim().to_owned())
}

fn uid(name: &str) -> Result<String> {
    let output = Command::new("id")
        .arg("-u")
        .output()
        .map_err(|e| die(name, 4, format!("cannot determine uid: {e}")))?;
    if !output.status.success() {
        return Err(die(name, 4, "cannot determine uid"));
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

fn report_missing(name: &str, config: &Config, missing: &str, uid: &str) -> Result<()> {
    for item in missing.split_whitespace() {
        if let Some(path) = item
            .strip_prefix("proton:")
            .or_else(|| item.strip_prefix("wine:"))
        {
            return Err(die(
                name,
                4,
                format!(
                    "x86 {} is not installed in the x86 Steam root ({}: {path}); install it from the x86 Steam client's library",
                    config.proton,
                    config.x86_root.display()
                ),
            ));
        }
        if item.starts_with("arm64:") {
            return Err(die(
                name,
                6,
                format!(
                    "{} in the x86 root is an ARM64 build: Proton ARM64 cannot run on macOS (benchmarks/stage18)",
                    config.proton
                ),
            ));
        }
        if let Some(path) = item.strip_prefix("runtime:") {
            return Err(die(
                name,
                4,
                format!(
                    "{} is not installed in the x86 Steam root ({}: {path}); {} requires it",
                    config.runtime,
                    config.x86_root.display(),
                    if config.proton.is_empty() {
                        "a native Linux game"
                    } else {
                        &config.proton
                    }
                ),
            ));
        }
        if let Some(path) = item.strip_prefix("fex:") {
            return Err(die(
                name,
                4,
                format!(
                    "no FEX at {}/{path} (scripts/setup.sh builds it)",
                    config.fex_dir.display()
                ),
            ));
        }
        if item == "noffx" {
            continue;
        }
        if item == "fexserver" {
            return Err(die(
                name,
                5,
                format!("FEXServer is not running (no /tmp/lxrt-abstract-{uid}/{uid}.FEXServer.Socket): start SteamARM's x86 side first (scripts/run-fex.sh /bin/true, or the launcher's Steam entry). Not starting one from here: a FEXServer started with a game's environment serves the wrong x86 root to every later FEX program (scripts/run-fex.sh)"),
            ));
        }
    }
    Ok(())
}

fn drop_root_guest_env(name: &str) -> Vec<String> {
    let path = env::var_os("STEAMARM_FEX_PROTON_ROOT_ENV")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/.lxrt-guest-env"));
    let Ok(content) = fs::read_to_string(path) else {
        return Vec::new();
    };
    let mut dropped = Vec::new();
    for line in content.lines() {
        let key = line.split('=').next().unwrap_or_default();
        if key.is_empty()
            || key.starts_with('#')
            || key.as_bytes()[0].is_ascii_digit()
            || !key.bytes().all(|b| b.is_ascii_alphanumeric() || b == b'_')
            || !line.contains('=')
        {
            continue;
        }
        if env::var_os(key).is_some() {
            env::remove_var(key);
            dropped.push(key.to_owned());
        }
    }
    if !dropped.is_empty() {
        say(
            name,
            &format!(
                "not passing on the ARM64 root's own guest environment: {}",
                dropped.join(" ")
            ),
        );
    }
    dropped
}

fn transfer_fex_options() -> Vec<String> {
    let mut source_names: Vec<OsString> = env::vars_os()
        .filter_map(|(key, _)| {
            let key = key.to_string_lossy();
            key.starts_with("STEAMARM_FEXOPT_")
                .then(|| OsString::from(key.as_ref()))
        })
        .collect();
    source_names.sort();
    let mut targets = Vec::new();
    for source in source_names {
        let source = source.to_string_lossy().into_owned();
        let Some(value) = env::var_os(&source) else {
            continue;
        };
        let target = format!("FEX_{}", &source["STEAMARM_FEXOPT_".len()..]);
        env::set_var(&target, value);
        env::remove_var(&source);
        targets.push(target);
    }
    targets
}

fn fex_app_config() -> String {
    let mut config = Vec::new();
    let mut thunks = Vec::new();
    if let Ok(value) = env::var("STEAM_FEX_TSOENABLED") {
        if let Ok(value) = value.parse::<i32>() {
            config.push(format!("\"TSOEnabled\":{}", value != 0));
        }
    }
    if let Ok(value) = env::var("STEAM_FEX_MULTIBLOCK") {
        if let Ok(value) = value.parse::<i32>() {
            config.push(format!("\"Multiblock\":{}", value != 0));
        }
    }
    if let Some(overrides) = env_nonempty("STEAM_COMPAT_FEX_CONFIG") {
        config.clear();
        config.push(format!(
            "\"TSOEnabled\":\"{}\"",
            if overrides.contains("TSOEnabled:1") {
                "1"
            } else {
                "0"
            }
        ));
        config.push(format!(
            "\"Multiblock\":\"{}\"",
            if overrides.contains("Multiblock:1") {
                "1"
            } else {
                "0"
            }
        ));
        thunks.push(format!(
            "\"GL\":\"{}\"",
            if overrides.contains("ThunksDB_GL:1") {
                "1"
            } else {
                "0"
            }
        ));
        thunks.push(format!(
            "\"Vulkan\":\"{}\"",
            if overrides.contains("ThunksDB_Vulkan:0") {
                "0"
            } else {
                "1"
            }
        ));
    } else {
        // Steam Frame's ARM64 client leaves STEAM_COMPAT_FEX_CONFIG empty.
        // Native x86 Linux games still need FEX's Vulkan thunk so calls reach
        // SteamARM's aarch64 Vulkan shim and its Metal-backed driver; without
        // it, pressure-vessel selects the Arch rootfs's Freedreno ICD.
        thunks.push("\"Vulkan\":\"1\"".to_owned());
    }
    format!(
        "{{\"Config\":{{{}}},\"ThunksDB\":{{{}}}}}",
        config.join(","),
        thunks.join(",")
    )
}

/// A directory holding only `libvulkan.so.1 -> <tool>/usr/share/fex-emu/
/// GuestThunks/libvulkan-guest.so`, for the front of an x86 game's
/// LD_LIBRARY_PATH (see the direct CS2 launch). Refreshed when the link points
/// elsewhere, so an updated FEX tool is picked up.
fn vulkan_thunk_dir(name: &str, fex_config_dir: &Path, tool_dir: &Path) -> Result<PathBuf> {
    let guest = tool_dir.join("usr/share/fex-emu/GuestThunks/libvulkan-guest.so");
    if !guest.is_file() {
        return Err(die(
            name,
            6,
            format!("FEX's Vulkan guest thunk is missing: {}", guest.display()),
        ));
    }
    let dir = fex_config_dir.join("steamarm-vulkan-thunk");
    fs::create_dir_all(&dir)
        .map_err(|error| failure(6, format!("cannot create {}: {error}", dir.display())))?;
    let link = dir.join("libvulkan.so.1");
    if fs::read_link(&link).ok().as_deref() != Some(guest.as_path()) {
        let temporary = dir.join(format!("libvulkan.so.1.tmp-{}", process::id()));
        let _ = fs::remove_file(&temporary);
        symlink(&guest, &temporary)
            .and_then(|()| fs::rename(&temporary, &link))
            .map_err(|error| failure(6, format!("cannot link {}: {error}", link.display())))?;
    }
    Ok(dir)
}

fn direct_cs2_paths(args: &[OsString]) -> Option<(PathBuf, PathBuf)> {
    let launcher = Path::new(args.first()?);
    let game_dir = launcher.parent()?;
    let install_dir = game_dir.parent()?;
    if launcher.file_name() != Some(OsStr::new("cs2.sh"))
        || game_dir.file_name() != Some(OsStr::new("game"))
        || install_dir.file_name() != Some(OsStr::new("Counter-Strike Global Offensive"))
    {
        return None;
    }
    Some((
        game_dir.to_path_buf(),
        game_dir.join("bin/linuxsteamrt64/cs2"),
    ))
}

fn client_install_path(args: &[OsString]) -> Option<PathBuf> {
    if let Some(path) = env::var_os("STEAM_COMPAT_CLIENT_INSTALL_PATH") {
        return Some(PathBuf::from(path));
    }
    for arg in args {
        let path = Path::new(arg);
        if path.file_name() != Some(OsStr::new("_v2-entry-point")) {
            continue;
        }
        let root = path.parent()?.parent()?.parent()?.parent()?;
        return Some(root.to_path_buf());
    }
    None
}

fn fex_tool_dir(client: &Path) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(emulator) = env::var_os("STEAM_COMPAT_EMULATOR") {
        let emulator = Path::new(&emulator);
        for ancestor in emulator.ancestors() {
            if ancestor.join("usr/bin/FEX").is_file() {
                candidates.push(ancestor.to_path_buf());
            }
        }
        if let Some(parent) = emulator.parent() {
            candidates.push(parent.to_path_buf());
        }
    }
    if let Some(paths) = env_nonempty("STEAM_COMPAT_TOOL_PATHS") {
        candidates.extend(paths.split(':').map(PathBuf::from));
    }
    // Steam's FEX compatibility layer may set CLIENT_INSTALL_PATH to the
    // legacy x86 tree. Prefer its explicit native tool paths before probing
    // that x86 root, which can be a translated lxrun mount.
    candidates.push(client.join("steamapps/common/FEX-Emu"));
    candidates
        .into_iter()
        .find(|path| path.join("usr/bin/FEX").is_file())
}

fn native_steam_root(tool_dir: &Path) -> Option<PathBuf> {
    tool_dir
        .ancestors()
        .find(|path| path.join("steamrtarm64/steam").is_file())
        .map(Path::to_path_buf)
}

fn page_size_sidecar(path: &Path) -> Option<u64> {
    let directory = if path.is_dir() {
        path
    } else {
        path.parent().unwrap_or_else(|| Path::new("."))
    };
    fs::read_to_string(directory.join("steamarm-host-page-size"))
        .ok()?
        .trim()
        .parse()
        .ok()
}

fn host_page_size() -> Option<u64> {
    // Steam may preserve neither our custom environment nor /proc/self/exe's
    // guest spelling. argv[0] and STEAM_COMPAT_TOOL_PATHS retain the tool
    // installation path, where the native runner persists this host value.
    let mut candidates = Vec::new();
    if let Some(argument_zero) = env::args_os().next() {
        candidates.push(PathBuf::from(argument_zero));
    }
    if let Some(paths) = env::var_os("STEAM_COMPAT_TOOL_PATHS") {
        candidates.extend(env::split_paths(&paths));
    }
    if let Some(path) = env::var_os("STEAM_COMPAT_TOOL_PATH") {
        candidates.push(PathBuf::from(path));
    }
    if let Ok(executable) = env::current_exe() {
        candidates.push(executable);
    }
    for candidate in candidates {
        if let Some(size) = page_size_sidecar(&candidate) {
            return Some(size);
        }
    }
    env::var("STEAMARM_HOST_PAGE_SIZE")
        .ok()
        .and_then(|value| value.trim().parse().ok())
}

fn ensure_steam_compat_link(
    name: &str,
    link: &Path,
    target: &Path,
    replace_file: bool,
) -> Result<()> {
    let parent = link.parent().ok_or_else(|| {
        failure(
            6,
            format!("invalid Steam compatibility path: {}", link.display()),
        )
    })?;
    fs::create_dir_all(parent)
        .map_err(|error| failure(6, format!("cannot create {}: {error}", parent.display())))?;
    if !target.exists() {
        return Err(die(
            name,
            6,
            format!("Steam integration target is missing: {}", target.display()),
        ));
    }
    match fs::symlink_metadata(link) {
        Ok(metadata) if metadata.file_type().is_symlink() => {
            let current = fs::read_link(link)
                .map_err(|error| failure(6, format!("cannot read {}: {error}", link.display())))?;
            if current == target
                || fs::canonicalize(&link)
                    .ok()
                    .zip(fs::canonicalize(target).ok())
                    .is_some_and(|(left, right)| left == right)
            {
                return Ok(());
            }
            fs::remove_file(link).map_err(|error| {
                failure(6, format!("cannot refresh {}: {error}", link.display()))
            })?;
        }
        Ok(metadata) if metadata.is_dir() => {
            if fs::canonicalize(link)
                .ok()
                .zip(fs::canonicalize(target).ok())
                .is_some_and(|(left, right)| left == right)
            {
                return Ok(());
            }
            return Err(die(
                name,
                6,
                format!("Steam compatibility path is occupied: {}", link.display()),
            ));
        }
        Ok(metadata) if replace_file && (metadata.is_file() || metadata.file_type().is_fifo()) => {
            fs::remove_file(link).map_err(|error| {
                failure(6, format!("cannot refresh {}: {error}", link.display()))
            })?;
        }
        Ok(_) => {
            return Err(die(
                name,
                6,
                format!("Steam compatibility path is occupied: {}", link.display()),
            ));
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {}
        Err(error) => {
            return Err(failure(
                6,
                format!("cannot inspect {}: {error}", link.display()),
            ))
        }
    }
    symlink(target, link).map_err(|error| {
        failure(
            6,
            format!(
                "cannot link {} to {}: {error}",
                link.display(),
                target.display()
            ),
        )
    })
}

fn ensure_steam_compat_home(name: &str, home: &Path, steam: &Path) -> Result<()> {
    let compat = home.join(".steam");
    let native_home = steam.ancestors().nth(3).ok_or_else(|| {
        failure(
            6,
            format!("cannot determine Steam's home from {}", steam.display()),
        )
    })?;
    let native_compat = native_home.join(".steam");
    fs::create_dir_all(&compat)
        .map_err(|error| failure(6, format!("cannot create {}: {error}", compat.display())))?;
    let same_compat_home = match (fs::canonicalize(&compat), fs::canonicalize(&native_compat)) {
        (Ok(compat), Ok(native)) => compat == native,
        _ => compat == native_compat,
    };
    let mut links = vec![
        ("steam", steam.to_path_buf(), false),
        ("root", steam.to_path_buf(), false),
        ("sdk64", steam.join("linux64"), false),
        ("sdk32", steam.join("linux32"), false),
    ];
    // The FEX HOME normally differs from the ARM64 Steam home. If Steam
    // passed its own HOME through, never replace the live IPC socket with a
    // symlink to itself.
    if !same_compat_home {
        links.extend([
            ("steam.pipe", native_compat.join("steam.pipe"), true),
            ("steam.pid", native_compat.join("steam.pid"), true),
            ("steam.token", native_compat.join("steam.token"), true),
        ]);
    }
    for (relative, target, replace_file) in links {
        ensure_steam_compat_link(name, &compat.join(relative), &target, replace_file)?;
    }
    Ok(())
}

/// The real file behind `soname` in the first of `dirs` that has it: the
/// exact name (followed through links: Debian ships libselinux.so.1 as a
/// plain file, libva.so.2 as a link) or, failing that, a versioned
/// `soname.N...` file. Returns the source and the file name to install.
fn find_runtime_library(dirs: &[PathBuf], soname: &str) -> Option<(PathBuf, String)> {
    for dir in dirs {
        let exact = dir.join(soname);
        if let Ok(real) = fs::canonicalize(&exact) {
            if real.is_file() {
                let name = real
                    .file_name()
                    .map(|name| name.to_string_lossy().into_owned())
                    .filter(|name| name.starts_with(soname))
                    .unwrap_or_else(|| soname.to_owned());
                return Some((real, name));
            }
        }
        let prefix = format!("{soname}.");
        let mut versioned: Vec<(PathBuf, String)> = fs::read_dir(dir)
            .ok()?
            .filter_map(|entry| entry.ok())
            .filter_map(|entry| {
                let name = entry.file_name().to_string_lossy().into_owned();
                (name.starts_with(&prefix) && entry.path().is_file()).then(|| (entry.path(), name))
            })
            .collect();
        versioned.sort();
        if let Some(found) = versioned.pop() {
            return Some(found);
        }
    }
    None
}

fn ensure_sniper_guest_libraries(name: &str, steam: &Path, rootfs: &Path) -> Result<()> {
    // The Steam Frame FEX-Mesa root is intentionally lean and does not carry
    // the text stack required by CS2's Panorama Pango module. Sniper already
    // ships the matching x86_64 runtime libraries; expose its missing SONAMEs
    // in the Frame-based root without replacing any of the Frame's drivers.
    const SONAMES: &[&str] = &[
        "libglib-2.0.so.0",
        "libgobject-2.0.so.0",
        "libgio-2.0.so.0",
        "libgmodule-2.0.so.0",
        "libpango-1.0.so.0",
        "libpangoft2-1.0.so.0",
        "libfreetype.so.6",
        "libfontconfig.so.1",
        "libfribidi.so.0",
        "libthai.so.0",
        "libharfbuzz.so.0",
        "libpcre.so.3",
        "libffi.so.7",
        "libpng16.so.16",
        "libselinux.so.1",
        "libdatrie.so.1",
        "libgraphite2.so.3",
        "libpcre2-8.so.0",
        "libva.so.2",
        "libvdpau.so.1",
        // SDL3 dlopens these (sdl/src/video/x11/SDL_x11dyn.h, audio): without
        // libXrandr it reported a 0 Hz display mode (CS2 wrote
        // refreshrate 0/0 to cs2_video.txt), and without libXi (XInput2) its
        // relative mouse mode had no raw motion, so the pointer ran into the
        // screen's corners instead of turning the view.
        "libXrandr.so.2",
        "libXi.so.6",
        "libXcursor.so.1",
        "libXfixes.so.3",
        "libXss.so.1",
        "libXrender.so.1",
        "libXinerama.so.1",
        "libXtst.so.6",
        "libxkbcommon.so.0",
        "libdbus-1.so.3",
        "libusb-1.0.so.0",
        // Sound: SDL's and CS2's PulseAudio client, and what it links.
        "libpulse.so.0",
        "libpulse-simple.so.0",
        "libpulsecommon-14.2.so",
        "libasound.so.2",
        "libsndfile.so.1",
        "libasyncns.so.0",
        "libFLAC.so.8",
        "libvorbis.so.0",
        "libvorbisenc.so.2",
        "libopus.so.0",
        "libogg.so.0",
    ];

    let runtime = steam.join("steamapps/common/SteamLinuxRuntime_sniper");
    let platforms = fs::read_dir(&runtime).map_err(|error| {
        failure(
            6,
            format!(
                "cannot list Steam Linux Runtime at {}: {error}",
                runtime.display()
            ),
        )
    })?;
    let mut candidates = Vec::new();
    for entry in platforms {
        let entry =
            entry.map_err(|error| failure(6, format!("cannot read Sniper runtime: {error}")))?;
        if entry
            .file_type()
            .map_err(|error| failure(6, format!("cannot inspect Sniper runtime: {error}")))?
            .is_dir()
            && entry
                .file_name()
                .to_string_lossy()
                .starts_with("sniper_platform_")
        {
            candidates.push(entry.path());
        }
    }
    candidates.sort();
    // Sniper's platform first; then the x86_64 platform of the client's own
    // Steam Runtime (steamrt64/pv-runtime), which has what pressure-vessel
    // would otherwise import from a host's graphics stack (libva, libvdpau:
    // CS2's libpanoramauiclient.so needs both and failed to load without them).
    let mut source_dirs: Vec<PathBuf> = candidates
        .last()
        .map(|path| path.join("files/lib/x86_64-linux-gnu"))
        .filter(|path| path.is_dir())
        .into_iter()
        .collect();
    // libpulsecommon lives in a private subdirectory (libpulse's RUNPATH).
    source_dirs.extend(
        source_dirs
            .iter()
            .map(|dir| dir.join("pulseaudio"))
            .filter(|dir| dir.is_dir())
            .collect::<Vec<_>>(),
    );
    if source_dirs.is_empty() {
        return Err(failure(
            6,
            format!(
                "Steam Linux Runtime has no x86_64 library directory under {}",
                runtime.display()
            ),
        ));
    }
    let pv_runtime = steam.join("steamrt64/pv-runtime/steam-runtime-steamrt");
    if let Ok(entries) = fs::read_dir(&pv_runtime) {
        let mut platforms: Vec<PathBuf> = entries
            .filter_map(|entry| entry.ok().map(|entry| entry.path()))
            .filter(|path| {
                path.file_name()
                    .is_some_and(|name| name.to_string_lossy().starts_with("steamrt3c_platform_"))
            })
            .map(|path| path.join("files/lib/x86_64-linux-gnu"))
            .filter(|path| path.is_dir())
            .collect();
        platforms.sort();
        if let Some(platform) = platforms.pop() {
            let pulse = platform.join("pulseaudio");
            source_dirs.push(platform);
            if pulse.is_dir() {
                source_dirs.push(pulse);
            }
        }
    }
    let destination = rootfs.join("usr/lib");
    fs::create_dir_all(&destination).map_err(|error| {
        failure(
            6,
            format!(
                "cannot prepare Steam Frame FEX libraries at {}: {error}",
                destination.display()
            ),
        )
    })?;

    let mut installed = 0usize;
    for soname in SONAMES {
        let alias = destination.join(soname);
        if alias.is_file() {
            continue;
        }
        let Some((source, versioned_name)) = find_runtime_library(&source_dirs, soname) else {
            continue;
        };
        let versioned = destination.join(&versioned_name);
        if !versioned.is_file() {
            if fs::symlink_metadata(&versioned).is_ok() {
                // A dangling link of the same name.
                let _ = fs::remove_file(&versioned);
            }
            let temporary = destination.join(format!("{versioned_name}.tmp-{}", process::id()));
            fs::copy(&source, &temporary).map_err(|error| {
                failure(
                    6,
                    format!(
                        "cannot copy {} into Steam Frame FEX root: {error}",
                        source.display()
                    ),
                )
            })?;
            fs::rename(&temporary, &versioned).map_err(|error| {
                let _ = fs::remove_file(&temporary);
                failure(
                    6,
                    format!(
                        "cannot install {} into Steam Frame FEX root: {error}",
                        versioned.display()
                    ),
                )
            })?;
        }
        if versioned_name != *soname {
            if let Ok(metadata) = fs::symlink_metadata(&alias) {
                if metadata.file_type().is_symlink() {
                    let _ = fs::remove_file(&alias);
                }
            }
            if !alias.exists() {
                symlink(&versioned_name, &alias).map_err(|error| {
                    failure(
                        6,
                        format!("cannot link Sniper library {}: {error}", alias.display()),
                    )
                })?;
            }
        }
        installed += 1;
    }

    // CS2 also dlopens this development-style name without a SONAME suffix.
    let pango_link = destination.join("libpangoft2-1.0.so");
    if !pango_link.exists() {
        symlink("libpangoft2-1.0.so.0", &pango_link).map_err(|error| {
            failure(
                6,
                format!(
                    "cannot link the CS2 Pango library {}: {error}",
                    pango_link.display()
                ),
            )
        })?;
        installed += 1;
    }

    if installed > 0 {
        say(
            name,
            &format!("added {installed} missing x86_64 text-runtime links from Sniper to the Steam Frame FEX root"),
        );
    }
    Ok(())
}

fn check_host_page_size(name: &str) -> Result<()> {
    let page_size = host_page_size().ok_or_else(|| {
        die(
            name,
            6,
            "SteamARM did not pass the host page size to the FEX tool; restart Steam with the SteamARM native ARM64 launcher.",
        )
    })?;
    let experimental =
        env_nonempty("STEAMARM_FEX_ALLOW_UNSUPPORTED_PAGE_SIZE").as_deref() == Some("1");
    if !matches!(page_size, 4096 | 16384) && !experimental {
        return Err(die(
            name,
            6,
            format!(
                "SteamARM FEX needs 4096-byte Linux guest pages. LXRT supports those on 4096- and 16384-byte hosts; this runtime reports {page_size}. Update the SteamARM native runtime for this host, or set STEAMARM_FEX_ALLOW_UNSUPPORTED_PAGE_SIZE=1 for an experimental attempt."
            ),
        ));
    }
    if experimental && !matches!(page_size, 4096 | 16384) {
        say(
            name,
            &format!("unsupported {page_size}-byte host pages explicitly allowed for an experimental run"),
        );
    } else {
        say(
            name,
            &format!("host page size {page_size}; SteamARM LXRT supports a 4096-byte FEX guest"),
        );
    }
    // FEX is itself an AArch64 Linux executable running through LXRT. Its
    // Linux view must remain 4 KiB even when Darwin's VM granule is 16 KiB;
    // runtime/subpage.c maps those guest pages onto the host granule.
    env::set_var("LXRT_GUEST_PAGE", "4096");
    Ok(())
}

fn run_linux_fex(name: &str, verb: &str, raw_args: &[OsString]) -> Result<()> {
    env::set_var("LXRT_GUEST_PAGE", "4096");
    let host_page_size = host_page_size().unwrap_or(16_384);
    env::set_var("LXRT_HOST_PAGE_SIZE", host_page_size.to_string());
    let args = if raw_args.first().is_some_and(|arg| arg == OsStr::new("--")) {
        &raw_args[1..]
    } else {
        raw_args
    };
    if args.is_empty() {
        return Err(die(name, 2, "Steam supplied no command to run"));
    }
    let client = client_install_path(args).ok_or_else(|| {
        die(
            name,
            6,
            "Steam did not provide STEAM_COMPAT_CLIENT_INSTALL_PATH or a Steam Linux Runtime command",
        )
    })?;
    let tool_dir = fex_tool_dir(&client).ok_or_else(|| {
        die(
            name,
            6,
            format!("cannot find FEX-Emu under {}", client.display()),
        )
    })?;
    let native_steam = native_steam_root(&tool_dir).ok_or_else(|| {
        die(
            name,
            6,
            format!(
                "cannot locate the native ARM64 Steam root above FEX-Emu at {}",
                tool_dir.display()
            ),
        )
    })?;
    // The x86 Linux game runs with its own HOME, separate from SteamARM's
    // native client. Link Steam's active IPC pipe and x86 SDK into that HOME
    // so SteamAPI_Init connects to the already-running ARM64 client.
    let fex_home = env::var_os("STEAMARM_FEX_HOME")
        .filter(|value| !value.is_empty())
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp/fexhome"));
    env::set_var("HOME", &fex_home);
    ensure_steam_compat_home(name, &fex_home, &native_steam)?;
    // pressure-vessel only bind-mounts XDG_RUNTIME_DIR into its game
    // container. Use the same short compatdata alias as the FEXServer
    // manager before starting Sniper, so both sides see one socket path.
    env::set_var("STEAM_COMPAT_CLIENT_INSTALL_PATH", &native_steam);
    let compat_paths = compat_path::configure_compat_paths().map_err(|error| {
        failure(
            6,
            format!("cannot configure the SteamARM FEX compat path: {error}"),
        )
    })?;
    let runtime = native_steam.join("steamapps/common/SteamLinuxRuntime_sniper/_v2-entry-point");
    if !runtime.is_file() {
        return Err(die(
            name,
            6,
            format!(
                "SteamLinuxRuntime_sniper is missing from the native Steam root: {}",
                runtime.display()
            ),
        ));
    }
    // Valve's bundled FEX maps ET_EXEC at 0x400000, which Darwin reserves in
    // PAGEZERO. SteamARM's patched host FEX provides the low-window mapping.
    // Keep Valve's original binary untouched and install our build beside it.
    let fex = env::var_os("STEAMARM_FEX_BINARY")
        .map(PathBuf::from)
        .unwrap_or_else(|| tool_dir.join("usr/bin/FEX-SteamARM"));
    let emulator = tool_dir.join("emulator.json");
    let rootfs = env::var_os("STEAMARM_FEX_ROOTFS")
        .map(PathBuf::from)
        .or_else(|| {
            env::var_os("STEAM_COMPAT_GRAPHICS_PROVIDER").map(|path| {
                let path = PathBuf::from(path);
                if path.is_file() {
                    path.parent().unwrap_or(Path::new("/")).to_path_buf()
                } else {
                    path
                }
            })
        })
        .unwrap_or_else(|| PathBuf::from("/usr/share/guestos/fex-mesa"));
    if !fex.is_file() || !emulator.is_file() {
        return Err(die(
            name,
            6,
            format!(
                "SteamARM-patched FEX or FEX emulator metadata is missing under {}; run the SteamARM installer to build and install FEX-SteamARM",
                tool_dir.display()
            ),
        ));
    }
    if !rootfs.is_dir() || !rootfs.join("lib/ld-linux-x86-64.so.2").is_file() {
        return Err(die(
            name,
            6,
            format!("Arch FEX root is incomplete: {}", rootfs.display()),
        ));
    }
    let fex_config_dir = compat_paths.fex_data;
    let fex_runtime_dir = fex_config_dir.join("Server");
    say(
        name,
        &format!(
            "using FEX at {}, native Steam root {}, Sniper runtime {}",
            fex.display(),
            native_steam.display(),
            runtime.display()
        ),
    );

    if env_nonempty("STEAMARM_FEX_PROTON_DRYRUN").as_deref() == Some("1") {
        let mut command = vec![fex.as_os_str().to_owned()];
        command.push(OsString::from("/bin/bash"));
        command.push(runtime.as_os_str().to_owned());
        command.push(OsString::from(format!("--verb={verb}")));
        command.push(OsString::from("--"));
        command.extend(args.iter().cloned());
        println!("mode: Arch FEX x86_64 Linux");
        println!("machine architecture: x86_64-linux-gnu");
        println!("env: LXRT_GUEST_PAGE=4096");
        println!("env: LXRT_HOST_PAGE_SIZE={host_page_size}");
        println!("env: FEX_GUESTBASE=1");
        println!("env: FEX_SMCCHECKS=mtrack");
        println!(
            "env: FEX_THUNKGUESTLIBS={}",
            tool_dir.join("usr/share/fex-emu/GuestThunks").display()
        );
        println!(
            "env: FEX_THUNKHOSTLIBS={}",
            tool_dir
                .join("usr/lib/aarch64-linux-gnu/fex-emu/HostThunks")
                .display()
        );
        println!("env: XDG_RUNTIME_DIR={}", fex_runtime_dir.display());
        println!("env: FEX_SILENTLOG=0");
        println!("env: FEX_OUTPUTLOG=stderr");
        println!("env: SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0");
        println!("env: STEAM_LINUX_RUNTIME_VERBOSE=1");
        println!("env: STEAM_LINUX_RUNTIME_LOG=1");
        println!("env: STEAM_LINUX_RUNTIME_KEEP_LOGS=1");
        println!("native Steam root: {}", native_steam.display());
        println!("Steam Linux Runtime: {}", runtime.display());
        println!("rootfs: {}", rootfs.display());
        println!("exec:{}", args_display(&command));
        return Ok(());
    }

    ensure_sniper_guest_libraries(name, &native_steam, &rootfs)?;

    // FEX's Steam socket falls back to XDG_RUNTIME_DIR when the per-game
    // socket under compatdata exceeds AF_UNIX's path limit. Keep that socket
    // inside Steam's compatdata bind mount so pressure-vessel and the host
    // FEXServerManager see the same endpoint.
    fs::create_dir_all(&fex_runtime_dir)
        .map_err(|error| failure(6, format!("cannot create FEX runtime directory: {error}")))?;
    fs::set_permissions(&fex_runtime_dir, fs::Permissions::from_mode(0o700))
        .map_err(|error| failure(6, format!("cannot secure FEX runtime directory: {error}")))?;
    env::set_var("XDG_RUNTIME_DIR", &fex_runtime_dir);
    let fex_config = fex_config_dir.join("Config.json");
    if !fex_config.is_file()
        || fs::read_to_string(&fex_config).is_ok_and(|contents| {
            contents.contains("@FEX_ROOTFS_PATH@") || contents.contains("@FEX_COMPAT_TOOL@")
        })
    {
        let template = fs::read_to_string(tool_dir.join("ConfigTemplate.json"))
            .map_err(|error| failure(6, format!("cannot read FEX config template: {error}")))?;
        let contents = template
            .replace("@FEX_COMPAT_TOOL@", &tool_dir.to_string_lossy())
            .replace("@FEX_ROOTFS_PATH@", &rootfs.to_string_lossy());
        fs::create_dir_all(&fex_config_dir)
            .map_err(|error| failure(6, format!("cannot create FEX config directory: {error}")))?;
        let temporary = fex_config_dir.join(format!("Config.json.tmp-{}", process::id()));
        fs::write(&temporary, contents)
            .map_err(|error| failure(6, format!("cannot write FEX config: {error}")))?;
        fs::rename(&temporary, &fex_config)
            .map_err(|error| failure(6, format!("cannot install FEX config: {error}")))?;
    }

    let temporary_app_config = if env::var_os("FEX_APP_CONFIG").is_none() {
        let path = PathBuf::from(format!("/tmp/fex-app-config-{}.json", process::id()));
        fs::write(&path, fex_app_config())
            .map_err(|error| failure(6, format!("cannot write FEX app config: {error}")))?;
        env::set_var("FEX_APP_CONFIG", &path);
        Some(path)
    } else {
        None
    };

    env::set_var("FEX_PORTABLE", "1");
    env::set_var("FEX_ROOTFS", &rootfs);
    env::set_var("FEX_APP_CONFIG_LOCATION", &fex_config_dir);
    env::set_var("FEX_APP_DATA_LOCATION", &fex_config_dir);
    // FEX-SteamARM's patched executable name can bypass portable global
    // config discovery on some builds. Ensure its Vulkan thunk loader sees
    // the libraries shipped beside this compatibility tool, not /usr/local.
    env::set_var(
        "FEX_THUNKGUESTLIBS",
        tool_dir.join("usr/share/fex-emu/GuestThunks"),
    );
    env::set_var(
        "FEX_THUNKHOSTLIBS",
        tool_dir.join("usr/lib/aarch64-linux-gnu/fex-emu/HostThunks"),
    );
    env::set_var("STEAM_COMPAT_EMULATOR", &emulator);
    env::set_var("FEX_GUESTBASE", "1");
    set_default("FEX_SMCCHECKS", "mtrack");
    env::set_var("FEX_SILENTLOG", "0");
    env::set_var("FEX_OUTPUTLOG", "stderr");
    set_default("SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS", "0");
    env::set_var("STEAM_LINUX_RUNTIME_VERBOSE", "1");
    env::set_var("STEAM_LINUX_RUNTIME_LOG", "1");
    env::set_var("STEAM_LINUX_RUNTIME_KEEP_LOGS", "1");
    // Valve's ARM64 FEX tool points this at the legacy x86 Steam tree. The
    // native Sniper runtime is installed beside the ARM64 client instead.
    // The game and pressure-vessel run inside FEX's x86_64 guest. Selecting
    // the host's AArch64 helper makes lxrun map its ET_EXEC image into
    // Darwin's reserved 4 GiB PAGEZERO; let FEX execute the x86_64 helper.
    env::set_var("STEAM_COMPAT_MACHINE_ARCHITECTURE", "x86_64-linux-gnu");
    if env::var_os("tu_override_uncached_as_cache_coherent").is_none() {
        env::set_var("tu_override_uncached_as_cache_coherent", "true");
    }
    env::remove_var("LD_PRELOAD");

    let mut command = Command::new(&fex);
    if let Some((game_dir, executable)) = direct_cs2_paths(args) {
        if !executable.is_file() {
            return Err(die(
                name,
                6,
                format!("CS2 Linux executable is missing: {}", executable.display()),
            ));
        }
        let libraries = game_dir.join("bin/linuxsteamrt64");
        say(
            name,
            "launching CS2 outside Steam Linux Runtime so FEX Vulkan thunks stay visible",
        );
        // FEX-SteamARM resolves the ThunksDB overlay (@PREFIX_LIB@) to
        // /usr/lib/x86_64-linux-gnu, but Steam Frame's fex-mesa rootfs is an
        // Arch tree with libvulkan.so.1 in /usr/lib: the overlay never
        // matched, the game loaded the x86 loader and the rootfs's Freedreno
        // ICD, and CS2 reported "Failed to init Vulkan". The guest thunk's
        // SONAME is libvulkan.so.1, so a link of that name ahead of the
        // game's libraries makes every dlopen("libvulkan.so.1") take it.
        let thunk_dir = vulkan_thunk_dir(name, &fex_config_dir, &tool_dir)?;
        let library_path = env::join_paths([thunk_dir, libraries])
            .map_err(|error| failure(6, format!("cannot build LD_LIBRARY_PATH: {error}")))?;
        command
            .arg(&executable)
            .args(args.iter().skip(1))
            .current_dir(&game_dir)
            .env("LD_LIBRARY_PATH", &library_path)
            .env("SDL_VIDEO_DRIVER", "x11")
            .env("ENABLE_PATHMATCH", "1")
            .env("SteamAppId", "730")
            .env("SteamGameId", "730")
            .env("SteamOverlayGameId", "730");
    } else {
        command
            .arg("/bin/bash")
            .arg(&runtime)
            .arg(format!("--verb={verb}"))
            .arg("--")
            .args(args);
    }
    let status = command
        .status()
        .map_err(|error| failure(127, format!("cannot start FEX: {error}")))?;
    if let Some(path) = temporary_app_config {
        let _ = fs::remove_file(path);
    }
    process::exit(status.code().unwrap_or(128));
}

fn run() -> Result<()> {
    let all_args: Vec<OsString> = env::args_os().collect();
    if all_args
        .get(1)
        .is_some_and(|arg| arg == OsStr::new(CHECK_MODE))
    {
        process::exit(check_mode(&all_args[2..]));
    }
    let name = program_name();
    let linux = name == "steamarm-fex-linux";
    let is_ffx_tool = name == "steamarm-ffx";
    let is_fex_tool = name.starts_with("steamarm-fex-") || is_ffx_tool;
    if is_fex_tool && env_nonempty("STEAMARM_FEX_PROTON_DRYRUN").as_deref() != Some("1") {
        check_host_page_size(&name)?;
    }
    let args = &all_args[1..];
    if is_ffx_tool {
        return ffx_only(&name, args);
    }
    if args.is_empty() || args[0] == OsStr::new("-h") || args[0] == OsStr::new("--help") {
        return Err(die(
            &name,
            2,
            format!(
                "usage: {name} <verb> [command...]  (run by Steam: toolmanifest.vdf \"%verb%\")"
            ),
        ));
    }
    let verb = args[0]
        .to_str()
        .ok_or_else(|| die(&name, 2, "refusing a non-UTF-8 Steam verb"))?;
    if verb.is_empty() || !verb.bytes().all(|b| b.is_ascii_lowercase() || b == b'_') {
        return Err(die(
            &name,
            2,
            format!("refusing verb '{verb}' (Steam's verbs are lower-case words)"),
        ));
    }
    let game_args = &args[1..];
    if linux {
        return run_linux_fex(&name, verb, game_args);
    }
    let config = load_config(&name, false)?;
    let arm_root_raw = env_nonempty("LXRT_ROOT").unwrap_or_default();
    if !Path::new(&arm_root_raw).is_absolute() {
        return Err(die(
            &name,
            6,
            "LXRT_ROOT is not set: this tool runs inside SteamARM's runtime (lxrun), in an ARM64 root",
        ));
    }
    let arm_root = normalize_path(&arm_root_raw);
    if arm_root == normalize_path(&config.x86_root.to_string_lossy()) {
        return Err(die(
            &name,
            6,
            format!(
                "called from the x86 root itself ({arm_root}): this tool is for the ARM64 client"
            ),
        ));
    }
    if env::var_os("LXRT_MOUNTS").is_some_and(|v| !v.is_empty()) {
        say(
            &name,
            "note: ignoring this process's own bind table (LXRT_MOUNTS)",
        );
    }

    let mut binds = BindSet::default();
    binds.add(&name, "HOME", env::var_os("HOME"), &arm_root, &config, true);
    binds.add(
        &name,
        "STEAM_COMPAT_CLIENT_INSTALL_PATH",
        env::var_os("STEAM_COMPAT_CLIENT_INSTALL_PATH"),
        &arm_root,
        &config,
        false,
    );
    if let Some(libraries) = env_nonempty("STEAM_COMPAT_LIBRARY_PATHS") {
        for path in libraries.split(':') {
            binds.add(
                &name,
                "STEAM_COMPAT_LIBRARY_PATHS",
                Some(OsString::from(path)),
                &arm_root,
                &config,
                false,
            );
        }
    }
    for key in [
        "STEAM_COMPAT_INSTALL_PATH",
        "STEAM_COMPAT_DATA_PATH",
        "STEAM_COMPAT_SHADER_PATH",
    ] {
        binds.add(&name, key, env::var_os(key), &arm_root, &config, false);
    }
    let cwd = env::current_dir().unwrap_or_default();
    binds.add(
        &name,
        "working directory",
        Some(cwd.into_os_string()),
        &arm_root,
        &config,
        false,
    );

    let proton_dir = config
        .x86_steam
        .join("steamapps/common")
        .join(&config.proton);
    let slr_dir = config
        .x86_steam
        .join("steamapps/common")
        .join(&config.runtime);
    let uid = uid(&name)?;
    let mut ffx_check = String::new();
    let mut ffx_decision = FfxDecision::default();
    if !linux && env_nonempty("STEAMARM_WIN_UPSCALER").as_deref() == Some("metalfx") {
        let executable = game_args
            .first()
            .map(|arg| arg.to_string_lossy().into_owned())
            .unwrap_or_default();
        ffx_decision = ffx_decide(&executable);
        if ffx_decision.why.is_empty() {
            ffx_check = FFX_DIR.to_owned();
        }
    }
    let dry_run = env_nonempty("STEAMARM_FEX_PROTON_DRYRUN").as_deref() == Some("1");
    let missing = run_check(
        &config,
        &proton_dir.to_string_lossy(),
        &slr_dir.to_string_lossy(),
        &uid,
        &ffx_check,
        dry_run,
    )?;
    report_missing(&name, &config, &missing, &uid)?;
    if missing.split_whitespace().any(|item| item == "noffx") {
        ffx_decision.why = format!(
            "SteamARM's FidelityFX DLLs are not in the x86 root ({}{FFX_DIR}: scripts/install-ffx-metalfx.sh)",
            config.x86_root.display()
        );
    }

    let fex_options = transfer_fex_options();
    env::set_var("LXRT_ROOT", &config.x86_root);
    env::set_var("LXRT_MOUNTS", &binds.table);
    env::set_var("FEX_ROOTFS", "/");
    env::set_var("TMPDIR", "/tmp");
    if linux {
        let fex_home = config.x86_home.join(".fex-emu/");
        env::set_var("FEX_APP_CONFIG_LOCATION", &fex_home);
        env::set_var("FEX_APP_DATA_LOCATION", &fex_home);
        set_default("SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS", "0");
    } else {
        env::set_var("HOME", &config.x86_home);
    }
    set_if_source("STEAMARM_MFX_NATIVE", "LXRT_VK_SCALER_NATIVE");
    set_if_source("STEAMARM_RENDER_SCALE", "LXRT_VK_RENDER_SCALE");
    set_if_source("STEAMARM_VK_FPS_LIMIT", "LXRT_VK_FPS_LIMIT");
    set_if_source("STEAMARM_VK_ANISOTROPY", "LXRT_VK_ANISOTROPY");
    if env_nonempty("STEAMARM_METAL_HUD").as_deref() == Some("1") {
        env::set_var("MTL_HUD_ENABLED", "1");
    }
    set_default("PROTON_USE_XALIA", "0");
    set_default("VKD3D_FEATURE_LEVEL", "12_0");
    env::set_var("FEX_GUESTBASE", "1");
    set_default("FEX_SMCCHECKS", "mtrack");
    env::set_var("FEX_SILENTLOG", "0");
    env::set_var("FEX_OUTPUTLOG", "stderr");
    env::set_var("OBJC_DISABLE_INITIALIZE_FORK_SAFETY", "YES");
    env::set_var(
        "PATH",
        "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
    );
    set_default("DISPLAY", ":2");
    set_default("PULSE_SERVER", "unix:/tmp/pulse/native");
    let tool_paths = if linux {
        slr_dir.clone()
    } else {
        PathBuf::from(format!("{}:{}", proton_dir.display(), slr_dir.display()))
    };
    env::set_var("STEAM_COMPAT_TOOL_PATHS", tool_paths);
    // Keep LXRT's 4 KiB guest page size for the native ARM64 FEX process.
    // Darwin's 16 KiB host granule is handled by LXRT's subpage mapper.
    env::set_var("LXRT_GUEST_PAGE", "4096");
    env::set_var("LXRT_X18_ALL_TEXT", "");
    for key in [
        "LD_LIBRARY_PATH",
        "STEAM_RUNTIME_LIBRARY_PATH",
        "STEAM_COMPAT_EMULATOR",
        "PRESSURE_VESSEL_EMULATOR",
        "STEAM_COMPAT_MACHINE_ARCHITECTURE",
        "STEAM_COMPAT_GRAPHICS_PROVIDER",
    ] {
        env::remove_var(key);
    }
    if env_nonempty("STEAMARM_FEX_KEEP_LD_PRELOAD").as_deref() != Some("1") {
        env::remove_var("LD_PRELOAD");
    }
    let dropped = drop_root_guest_env(&name);
    if !linux && env_nonempty("STEAMARM_WIN_UPSCALER").as_deref() == Some("metalfx") {
        if ffx_decision.why.is_empty() {
            ffx_apply(FFX_DIR);
        }
        ffx_say(&name, &ffx_decision, &format!("{FFX_DIR} in the x86 root"));
    }

    let first = game_args
        .first()
        .map(|v| {
            Path::new(v)
                .file_name()
                .unwrap_or(v)
                .to_string_lossy()
                .into_owned()
        })
        .unwrap_or_default();
    let appid = env_nonempty("SteamAppId")
        .or_else(|| env_nonempty("STEAM_COMPAT_APP_ID"))
        .unwrap_or_else(|| "?".to_owned());
    let proton_text = if config.proton.is_empty() {
        String::new()
    } else {
        format!("{} + ", config.proton)
    };
    say(
        &name,
        &format!(
            "{} {} (app {}) via x86 {}{} under FEX (root {}); client paths bound from {}: {}",
            verb,
            first,
            appid,
            proton_text,
            config.runtime,
            config.x86_root.display(),
            arm_root,
            if binds.paths.is_empty() {
                "none".to_owned()
            } else {
                binds.paths.join(" ")
            }
        ),
    );

    let fex_path = config.fex_dir.join(&config.fex_bin);
    let mut command_args: Vec<OsString> = vec![
        OsString::from("/bin/bash"),
        slr_dir.join("_v2-entry-point").into_os_string(),
        OsString::from(format!("--verb={verb}")),
        OsString::from("--"),
    ];
    if linux {
        command_args.extend(game_args.iter().cloned());
    } else {
        command_args.push(proton_dir.join("proton").into_os_string());
        command_args.push(OsString::from(verb));
        command_args.extend(game_args.iter().cloned());
    }

    if dry_run {
        for key in [
            "LXRT_ROOT",
            "FEX_ROOTFS",
            "HOME",
            "TMPDIR",
            "FEX_GUESTBASE",
            "DISPLAY",
            "PULSE_SERVER",
            "STEAM_COMPAT_TOOL_PATHS",
            "LXRT_GUEST_PAGE",
        ]
        .into_iter()
        .chain(fex_options.iter().map(String::as_str))
        {
            if let Some(value) = env::var_os(key) {
                println!("env: {key}={}", value.to_string_lossy());
            }
        }
        println!("env: LXRT_MOUNTS={}", show_table(&binds.table));
        show_ffx_env();
        for key in [
            "LXRT_X18_ALL_TEXT",
            "LD_LIBRARY_PATH",
            "LD_PRELOAD",
            "STEAM_COMPAT_EMULATOR",
        ]
        .into_iter()
        .chain(dropped.iter().map(String::as_str))
        {
            if env_nonempty(key).is_some() {
                println!("env: {key} still set");
            }
        }
        println!(
            "dropped from the ARM64 root's guest environment: {}",
            if dropped.is_empty() {
                "none".to_owned()
            } else {
                dropped.join(" ")
            }
        );
        let mut display = vec![fex_path.into_os_string()];
        display.extend(command_args.iter().cloned());
        println!("exec:{}", args_display(&display));
        return Ok(());
    }
    let mut command = Command::new(fex_path);
    command.args(command_args);
    Err(exec_command(&name, &mut command))
}

fn main() {
    if let Err(error) = run() {
        if !error.reported {
            eprintln!("{}: {}", program_name(), error.message);
        }
        process::exit(error.code);
    }
}
