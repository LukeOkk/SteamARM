use serde_json::Value;
use std::collections::BTreeMap;
use std::env;
use std::fs::{self, File};
use std::io::{self, Write};
use std::os::unix::fs::{FileTypeExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, SystemTime, UNIX_EPOCH};

const ROOT: &str = "/tmp/lxrt-steamroot";

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

fn selected_dir(variable: &str, name: &str) -> PathBuf {
    if let Some(value) = env::var_os(variable).filter(|v| !v.is_empty()) {
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

fn repo_root() -> PathBuf {
    if let Some(path) = env::var_os("STEAMARM_PROJECT_ROOT").filter(|v| !v.is_empty()) {
        return PathBuf::from(path);
    }
    PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../..")
}

fn executable(variable: &str, name: &str) -> PathBuf {
    if let Some(path) = env::var_os(variable).filter(|v| !v.is_empty()) {
        return PathBuf::from(path);
    }
    let root = repo_root();
    let candidates = [
        root.join("build").join(name),
        root.join("build/SteamARM.app/Contents/Resources")
            .join(name),
        root.join("build/launcher/SteamARM.app/Contents/Resources")
            .join(name),
    ];
    candidates
        .into_iter()
        .find(|path| path.is_file())
        .unwrap_or_else(|| root.join("build").join(name))
}

fn helper(name: &str) -> PathBuf {
    repo_root().join("scripts").join(name)
}

fn state_dir() -> PathBuf {
    selected_dir("STEAMARM_STATE", "SteamARM-roots")
}

fn logs_dir() -> PathBuf {
    state_dir().join("logs")
}

fn now_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

fn run_status(program: &Path, args: &[&str], envs: &[(&str, &str)]) -> io::Result<bool> {
    let status = Command::new(program)
        .args(args)
        .envs(envs.iter().copied())
        .status()?;
    Ok(status.success())
}

fn run_output(program: &Path, args: &[&str]) -> Option<String> {
    let output = Command::new(program)
        .args(args)
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()
        .ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

fn script_status(name: &str, args: &[&str], envs: &[(&str, &str)]) -> io::Result<bool> {
    run_status(&helper(name), args, envs)
}

fn pids(pattern: &str) -> Vec<i32> {
    run_output(Path::new("pgrep"), &["-f", pattern])
        .unwrap_or_default()
        .lines()
        .filter_map(|line| line.trim().parse().ok())
        .collect()
}

fn process_command(pid: i32, include_env: bool) -> String {
    let mut command = Command::new("ps");
    if include_env {
        command.args(["-E", "-o", "command=", "-p"]);
    } else {
        command.args(["-o", "command=", "-p"]);
    }
    command
        .arg(pid.to_string())
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()
        .ok()
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
        .unwrap_or_default()
}

fn is_android_session(pid: i32) -> bool {
    process_command(pid, true)
        .split_whitespace()
        .any(|part| part == "LXRT_SESSION=android")
}

fn stop_steam() {
    for pid in pids("build/lxrun") {
        let command = process_command(pid, false);
        if command.contains("Xvnc") || command.contains("FEXServer") || is_android_session(pid) {
            continue;
        }
        let _ = Command::new("kill")
            .args(["-9", &pid.to_string()])
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }
    let _ = fs::remove_file(Path::new(ROOT).join("tmp/fexhome/.steam/steam.pid"));
}

fn ensure_vnc_password() -> Result<(), String> {
    let state = state_dir();
    fs::create_dir_all(&state).map_err(|e| format!("create {}: {e}", state.display()))?;
    let text_path = state.join("vncpasswd.txt");
    let password_path = state.join("vncpasswd");
    if password_path
        .metadata()
        .map(|m| m.len() > 0)
        .unwrap_or(false)
    {
        return Ok(());
    }
    let password = env::var("STEAMARM_VNC_PASSWORD").unwrap_or_else(|_| "steamarm".into());
    fs::write(&text_path, format!("{password}\n"))
        .map_err(|e| format!("write {}: {e}", text_path.display()))?;
    fs::set_permissions(&text_path, fs::Permissions::from_mode(0o600))
        .map_err(|e| format!("chmod {}: {e}", text_path.display()))?;

    let mut input = password
        .as_bytes()
        .iter()
        .copied()
        .take(8)
        .collect::<Vec<_>>();
    input.resize(8, b' ');
    for byte in &mut input {
        if *byte == b' ' {
            *byte = 0;
        }
    }
    let output = File::create(&password_path)
        .map_err(|e| format!("create {}: {e}", password_path.display()))?;
    let mut child = Command::new("openssl")
        .args([
            "enc",
            "-des-ecb",
            "-K",
            "e84ad660c4721ae0",
            "-nopad",
            "-nosalt",
        ])
        .stdin(Stdio::piped())
        .stdout(Stdio::from(output))
        .stderr(Stdio::inherit())
        .spawn()
        .map_err(|e| format!("start openssl: {e}"))?;
    if let Some(mut stdin) = child.stdin.take() {
        stdin
            .write_all(&input)
            .map_err(|e| format!("send VNC password to openssl: {e}"))?;
    }
    let status = child.wait().map_err(|e| format!("wait for openssl: {e}"))?;
    if !status.success() {
        return Err("openssl could not create the VNC password file".into());
    }
    fs::set_permissions(&password_path, fs::Permissions::from_mode(0o600))
        .map_err(|e| format!("chmod {}: {e}", password_path.display()))?;
    Ok(())
}

fn has_process(pattern: &str) -> bool {
    !pids(pattern).is_empty()
}

fn detached_command(
    session: &Path,
    program: &Path,
    args: &[&str],
    envs: &[(&str, &str)],
    stdout: Stdio,
    stderr: Stdio,
) -> io::Result<()> {
    let mut command = Command::new(session);
    command
        .arg("detach")
        .arg(program)
        .args(args)
        .envs(envs.iter().copied())
        .stdin(Stdio::null())
        .stdout(stdout)
        .stderr(stderr);
    command.spawn().map(|_| ())
}

fn start_xvnc(
    session: &Path,
    state: &Path,
    password_file: &Path,
    display: &str,
) -> Result<(), String> {
    if has_process("lxrun /usr/bin/Xvnc") {
        return Ok(());
    }
    let root = Path::new(ROOT);
    let root_password = root.join("tmp/.vncpasswd");
    fs::copy(password_file, &root_password).map_err(|e| format!("copy VNC password: {e}"))?;
    if !has_process("lxrun /tmp/lxrt-root/usr/bin/FEXServer") {
        let ok = script_status("run-fex.sh", &["/bin/true"], &[])
            .map_err(|e| format!("start FEX for Xvnc: {e}"))?;
        if !ok {
            return Err("FEX server setup failed before Xvnc".into());
        }
    }
    let output =
        File::create(state.join("logs/xvnc.log")).map_err(|e| format!("create Xvnc log: {e}"))?;
    let error = output
        .try_clone()
        .map_err(|e| format!("open Xvnc log: {e}"))?;
    let lxrun = repo_root().join("build/lxrun");
    detached_command(
        session,
        &lxrun,
        &[
            "/usr/bin/Xvnc",
            display,
            "-geometry",
            "1600x900",
            "-depth",
            "24",
            "-SecurityTypes",
            "VncAuth",
            "-rfbauth",
            "/tmp/.vncpasswd",
            "-localhost",
            "-rfbport",
            "5901",
            "+extension",
            "GLX",
            "-fp",
            "built-ins",
            "-nolisten",
            "tcp",
        ],
        &[
            ("TMPDIR", "/tmp"),
            ("HOME", "/tmp/fexhome"),
            ("FEX_ROOTFS", "/"),
            ("FEX_GUESTBASE", "1"),
            ("LXRT_ROOT", ROOT),
        ],
        Stdio::from(output),
        Stdio::from(error),
    )
    .map_err(|e| format!("start Xvnc: {e}"))?;
    thread::sleep(Duration::from_secs(3));
    Ok(())
}

fn settings_env(path: &Path) -> Result<BTreeMap<String, String>, String> {
    let output = Command::new(path)
        .arg("--json")
        .stdin(Stdio::null())
        .output()
        .map_err(|e| format!("run settings engine {}: {e}", path.display()))?;
    if !output.status.success() {
        return Err(format!(
            "settings engine failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    let value: Value = serde_json::from_slice(&output.stdout)
        .map_err(|e| format!("read settings engine output: {e}"))?;
    let object = value
        .as_object()
        .ok_or_else(|| "settings engine returned a non-object".to_string())?;
    Ok(object
        .iter()
        .filter_map(|(key, value)| value.as_str().map(|s| (key.clone(), s.to_owned())))
        .collect())
}

fn setting_output(path: &Path, option: &str) -> Result<String, String> {
    let output = Command::new(path)
        .arg(option)
        .stdin(Stdio::null())
        .output()
        .map_err(|e| format!("run {} {option}: {e}", path.display()))?;
    if !output.status.success() {
        return Err(format!(
            "{} {option} failed: {}",
            path.display(),
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

fn background_dbus(
    session: &Path,
    display: &str,
    logs: &Path,
    suffix: &str,
) -> Result<(String, PathBuf), String> {
    let address = format!("unix:path=/tmp/fexhome/.steamarm-session-{suffix}");
    let socket = Path::new(ROOT).join(format!("tmp/fexhome/.steamarm-session-{suffix}"));
    let log = File::create(logs.join(format!("dbus-{suffix}.log")))
        .map_err(|e| format!("create D-Bus log: {e}"))?;
    let error = log
        .try_clone()
        .map_err(|e| format!("open D-Bus log: {e}"))?;
    detached_command(
        session,
        &helper("run-fex.sh"),
        &[
            "/usr/bin/dbus-daemon",
            "--session",
            "--nofork",
            &format!("--address={address}"),
        ],
        &[
            ("LXRT_ROOT", ROOT),
            ("FEX_ROOTFS", "/"),
            ("DISPLAY", display),
        ],
        Stdio::from(log),
        Stdio::from(error),
    )
    .map_err(|e| format!("start session D-Bus: {e}"))?;
    for _ in 0..80 {
        if socket_is_ready(&socket) {
            return Ok((address, socket));
        }
        thread::sleep(Duration::from_millis(100));
    }
    Err(format!(
        "session bus failed (see {})",
        logs.join(format!("dbus-{suffix}.log")).display()
    ))
}

fn socket_is_ready(path: &Path) -> bool {
    fs::metadata(path)
        .map(|metadata| metadata.file_type().is_socket())
        .unwrap_or(false)
}

fn start_steam(
    session: &Path,
    settings: &Path,
    wine_options: &Path,
    display: &str,
    logs: &Path,
) -> Result<Option<PathBuf>, String> {
    if has_process("build/lxrun .*ubuntu12_32/steam ") {
        return Ok(None);
    }
    stop_steam();
    let log_path = logs.join(format!("steam-{}.log", chrono_suffix()));
    fs::write(logs.join("current"), format!("{}\n", log_path.display()))
        .map_err(|e| format!("write current Steam log path: {e}"))?;
    let steam_home = Path::new(ROOT).join("tmp/fexhome/.local/share/Steam");
    let steam_ui = steam_home.join("ubuntu12_32/steamui.so");
    let verify = steam_ui.is_file();

    let values = settings_env(settings)?;
    for (key, value) in values {
        env::set_var(key, value);
    }
    let emulate = setting_output(settings, "--emulate-modeset")?;
    if !has_process("build/lxrun .*wineserver") {
        let _ = Command::new(wine_options)
            .arg("emulate-modeset")
            .arg(emulate)
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }
    let volume = setting_output(settings, "--volume").unwrap_or_default();
    if !volume.is_empty() {
        match script_status("audio.sh", &["start", &volume], &[]) {
            Ok(true) => {}
            Ok(false) | Err(_) => eprintln!("run-steam: no sound (scripts/audio.sh)"),
        }
    }
    match script_status("input.sh", &["start"], &[]) {
        Ok(true) => {}
        Ok(false) | Err(_) => eprintln!("run-steam: no controllers for games (scripts/input.sh)"),
    }

    let suffix = format!("{}-{}", now_seconds(), std::process::id());
    let (dbus_address, _) = background_dbus(session, display, logs, &suffix)?;
    let output = File::create(&log_path).map_err(|e| format!("create Steam log: {e}"))?;
    let error = output
        .try_clone()
        .map_err(|e| format!("open Steam log: {e}"))?;
    let steam_script = Path::new("/tmp/fexhome/.local/share/Steam/steam.sh");
    let mut args = vec!["/bin/bash".to_string(), steam_script.display().to_string()];
    if verify {
        args.push("-noverifyfiles".into());
    }
    let args = args.iter().map(String::as_str).collect::<Vec<_>>();
    let shader_setting =
        env::var("STEAM_ENABLE_SHADER_CACHE_MANAGEMENT").unwrap_or_else(|_| "0".into());
    let statfs = run_output(&helper("image-volume.sh"), &["backing"]).unwrap_or_default();
    let envs = [
        ("DBUS_SESSION_BUS_ADDRESS", dbus_address.as_str()),
        ("DISPLAY", display),
        ("LXRT_ROOT", ROOT),
        ("FEX_ROOTFS", "/"),
        (
            "STEAM_ENABLE_SHADER_CACHE_MANAGEMENT",
            shader_setting.as_str(),
        ),
        ("LXRT_STATFS_BACKING", statfs.as_str()),
    ];
    detached_command(
        session,
        &helper("run-fex.sh"),
        &args,
        &envs,
        Stdio::from(output),
        Stdio::from(error),
    )
    .map_err(|e| format!("start Steam client: {e}"))?;
    Ok(Some(log_path))
}

fn chrono_suffix() -> String {
    let mut command = Command::new("date");
    command.arg("+%H%M%S").stdin(Stdio::null());
    command
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_owned())
        .unwrap_or_else(|| now_seconds().to_string())
}

fn launch() -> Result<(), String> {
    let repo = repo_root();
    let logs = logs_dir();
    fs::create_dir_all(&logs).map_err(|e| format!("create {}: {e}", logs.display()))?;
    let settings = executable("STEAMARM_SETTINGS_ENV_BIN", "steamarm-settings-env");
    if !settings.is_file() {
        return Err(format!(
            "Rust settings engine missing at {} (run make settings-env)",
            settings.display()
        ));
    }
    let wine_options = executable(
        "STEAMARM_WINE_PREFIX_OPTIONS_BIN",
        "steamarm-wine-prefix-options",
    );
    if !wine_options.is_file() {
        return Err(format!(
            "Rust Wine prefix tool missing at {} (run make wine-prefix-options)",
            wine_options.display()
        ));
    }
    let session = executable("STEAMARM_SESSION_BIN", "steamarm-session");
    if !session.is_file() {
        return Err(format!(
            "Rust session tool missing at {} (run make session)",
            session.display()
        ));
    }
    env::set_var("STEAMARM_PROJECT_ROOT", &repo);
    env::set_var("STEAMARM_SETTINGS_ENV_BIN", &settings);
    env::set_var("STEAMARM_WINE_PREFIX_OPTIONS_BIN", &wine_options);
    env::set_var("STEAMARM_SESSION_BIN", &session);

    let ok = script_status(
        "env-links.sh",
        &[state_dir().to_string_lossy().as_ref()],
        &[],
    )
    .map_err(|e| format!("link runtime environment: {e}"))?;
    if !ok {
        return Err("environment links failed".into());
    }
    ensure_vnc_password()?;
    let password_file = state_dir().join("vncpasswd");
    let mode = env::var("STEAMARM_DISPLAY").unwrap_or_else(|_| "native".into());
    let display = if mode == "native" { ":2" } else { ":1" };
    if mode == "native" {
        let ok = script_status("run-x11-native.sh", &["start", display], &[])
            .map_err(|e| format!("start native X server: {e}"))?;
        if !ok {
            return Err("native X server failed to start".into());
        }
    } else {
        start_xvnc(&session, &state_dir(), &password_file, display)?;
    }

    let backing = run_output(&helper("image-volume.sh"), &["backing"]).unwrap_or_default();
    env::set_var("LXRT_STATFS_BACKING", backing);
    let log = start_steam(&session, &settings, &wine_options, display, &logs)?;
    if let Some(path) = log {
        println!(
            "Steam starting (log {}); the window appears in about a minute.",
            path.display()
        );
    }

    let steam_bin_host = Path::new(ROOT).join("tmp/fexhome/.local/share/Steam/ubuntu12_32/steam");
    let steam_bin_guest = "/tmp/fexhome/.local/share/Steam/ubuntu12_32/steam";
    if steam_bin_host.is_file() {
        for _ in 0..80 {
            if has_process("build/lxrun .*ubuntu12_32/steam ") {
                let wake_log = logs.join(format!("steam-wake-{}.log", chrono_suffix()));
                let output =
                    File::create(&wake_log).map_err(|e| format!("create Steam wake log: {e}"))?;
                let error = output
                    .try_clone()
                    .map_err(|e| format!("open Steam wake log: {e}"))?;
                detached_command(
                    &session,
                    &helper("run-fex.sh"),
                    &[steam_bin_guest, "steam://open/main"],
                    &[
                        ("LXRT_ROOT", ROOT),
                        ("FEX_ROOTFS", "/"),
                        ("DISPLAY", display),
                        ("STEAMARM_NO_SAFEGUARD", "1"),
                    ],
                    Stdio::from(output),
                    Stdio::from(error),
                )
                .map_err(|e| format!("wake Steam main window: {e}"))?;
                break;
            }
            thread::sleep(Duration::from_secs(1));
        }
    }
    if mode != "native" {
        let _ = Command::new("open")
            .arg("vnc://127.0.0.1:5901")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
        println!(
            "Screen Sharing: password in {}",
            state_dir().join("vncpasswd.txt").display()
        );
    }
    Ok(())
}

fn run() -> Result<(), String> {
    if env::args().nth(1).as_deref() == Some("--stop") {
        stop_steam();
        return Ok(());
    }
    launch()
}

fn main() {
    if let Err(error) = run() {
        eprintln!("run-steam: {error}");
        std::process::exit(1);
    }
}
