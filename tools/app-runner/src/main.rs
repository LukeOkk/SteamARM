use serde::Deserialize;
use std::env;
use std::fs::{self, File, OpenOptions};
use std::io::{self, Write};
use std::os::raw::{c_char, c_int};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{FileTypeExt, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::{Command, ExitStatus, Output, Stdio};
use std::thread;
use std::time::Duration;

#[path = "../../common/safeguard.rs"]
mod safeguard;

const X86_ROOT: &str = "/tmp/lxrt-steamroot";
const ARM64_ROOT: &str = "/tmp/lxrt-arm64root";
const X11_BUNDLE_ID: &str = "org.steamarm.X11";
const W_OK: c_int = 2;

extern "C" {
    fn access(path: *const c_char, mode: c_int) -> c_int;
}

#[derive(Deserialize)]
#[serde(rename_all = "camelCase")]
struct AppPlan {
    name: String,
    architecture: String,
    android_package: String,
    kind: String,
    root: String,
    in_x86_root: bool,
    fex_rootfs: String,
    prefix: String,
    geometry: String,
    display_mode: String,
    environment: Vec<String>,
    command: Vec<String>,
}

struct Config {
    repo: PathBuf,
    state: PathBuf,
    logs: PathBuf,
    launcher: PathBuf,
    root: PathBuf,
    settings_env: PathBuf,
    app_plan: PathBuf,
    session: PathBuf,
    proton: PathBuf,
    wine_options: PathBuf,
    vnc: PathBuf,
}

fn home_dir() -> PathBuf {
    env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"))
}

fn state_dir() -> PathBuf {
    if let Some(value) = env::var_os("STEAMARM_STATE").filter(|value| !value.is_empty()) {
        return PathBuf::from(value);
    }
    let newer = home_dir().join("Developer/SteamARM-roots");
    let older = home_dir().join("SteamARM-roots");
    if newer.is_dir() || !older.is_dir() {
        newer
    } else {
        older
    }
}

fn repo_dir() -> PathBuf {
    env::current_dir().unwrap_or_else(|_| PathBuf::from("."))
}

fn tool_path(variable: &str, name: &str, repo: &Path) -> PathBuf {
    if let Some(value) = env::var_os(variable).filter(|value| !value.is_empty()) {
        return PathBuf::from(value);
    }
    if let Some(bundled) = env::current_exe()
        .ok()
        .and_then(|executable| executable.parent().map(|parent| parent.join(name)))
        .filter(|path| is_executable(path))
    {
        return bundled;
    }
    repo.join("build").join(name)
}

impl Config {
    fn new() -> Self {
        let repo = repo_dir();
        let state = state_dir();
        let launcher = state.join("launcher");
        Self {
            logs: state.join("logs"),
            root: PathBuf::from(X86_ROOT),
            settings_env: tool_path("STEAMARM_SETTINGS_ENV_BIN", "steamarm-settings-env", &repo),
            app_plan: tool_path("STEAMARM_APP_PLAN_BIN", "steamarm-app-plan", &repo),
            session: tool_path("STEAMARM_SESSION_BIN", "steamarm-session", &repo),
            proton: tool_path(
                "STEAMARM_PROTON_COMMAND_BIN",
                "steamarm-proton-command",
                &repo,
            ),
            wine_options: tool_path(
                "STEAMARM_WINE_PREFIX_OPTIONS_BIN",
                "steamarm-wine-prefix-options",
                &repo,
            ),
            vnc: tool_path("STEAMARM_VNC_CONTROL_BIN", "steamarm-vnc-control", &repo),
            repo,
            state,
            launcher,
        }
    }

    fn export_tool_paths(&self) {
        env::set_var("STEAMARM_SETTINGS_ENV_BIN", &self.settings_env);
        env::set_var("STEAMARM_APP_PLAN_BIN", &self.app_plan);
        env::set_var("STEAMARM_SESSION_BIN", &self.session);
        env::set_var("STEAMARM_PROTON_COMMAND_BIN", &self.proton);
        env::set_var("STEAMARM_WINE_PREFIX_OPTIONS_BIN", &self.wine_options);
        env::set_var("STEAMARM_VNC_CONTROL_BIN", &self.vnc);
    }

    fn path(&self, name: &str) -> PathBuf {
        self.repo.join(name)
    }

    fn build(&self, name: &str) -> PathBuf {
        if let Some(bundled) = env::current_exe()
            .ok()
            .and_then(|executable| executable.parent().map(|parent| parent.join(name)))
            .filter(|path| is_executable(path))
        {
            bundled
        } else {
            self.repo.join("build").join(name)
        }
    }
}

fn usage() -> &'static str {
    "Usage: scripts/run-app.sh <app-id>\n       scripts/run-app.sh --stop\n       scripts/run-app.sh --reap [SID]\n       scripts/run-app.sh --dry-run <app-id>\n       scripts/run-app.sh --help\n"
}

fn command_output(program: &Path, args: &[String]) -> io::Result<Output> {
    Command::new(program)
        .args(args)
        .stdin(Stdio::null())
        .output()
}

fn run_status(program: &Path, args: &[String], quiet: bool) -> io::Result<ExitStatus> {
    run_status_env(program, args, &[], quiet, quiet)
}

fn run_status_env(
    program: &Path,
    args: &[String],
    envs: &[(&str, &str)],
    quiet_stdout: bool,
    quiet_stderr: bool,
) -> io::Result<ExitStatus> {
    let mut command = Command::new(program);
    command
        .args(args)
        .envs(envs.iter().copied())
        .stdin(Stdio::null());
    if quiet_stdout {
        command.stdout(Stdio::null());
    }
    if quiet_stderr {
        command.stderr(Stdio::null());
    }
    command.status()
}

fn run_stdout_null(program: &Path, args: &[String]) -> io::Result<ExitStatus> {
    run_status_env(program, args, &[], true, false)
}

fn output_text(program: &Path, args: &[String]) -> String {
    command_output(program, args)
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
        .unwrap_or_default()
}

fn status_code(status: ExitStatus) -> i32 {
    status.code().unwrap_or(1)
}

fn is_socket(path: &Path) -> bool {
    fs::metadata(path).is_ok_and(|metadata| metadata.file_type().is_socket())
}

fn is_executable(path: &Path) -> bool {
    fs::metadata(path)
        .is_ok_and(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
}

fn is_writable(path: &Path) -> bool {
    use std::ffi::CString;
    CString::new(path.as_os_str().as_bytes())
        .is_ok_and(|path| unsafe { access(path.as_ptr(), W_OK) == 0 })
}

fn session_output(config: &Config, args: &[String]) -> Output {
    Command::new(&config.session)
        .args(args)
        .stdin(Stdio::null())
        .output()
        .unwrap_or_else(|error| Output {
            status: ExitStatus::from_raw(127 << 8),
            stdout: Vec::new(),
            stderr: format!("steamarm-session: {error}\n").into_bytes(),
        })
}

#[cfg(unix)]
use std::os::unix::process::ExitStatusExt;

fn session_has_process(config: &Config, pattern: &str) -> bool {
    session_output(config, &["has-process".into(), pattern.into()])
        .status
        .success()
}

fn guest_pids(config: &Config) -> Vec<String> {
    String::from_utf8_lossy(&session_output(config, &["guest-pids".into()]).stdout)
        .lines()
        .map(str::trim)
        .filter(|pid| !pid.is_empty())
        .map(str::to_owned)
        .collect()
}

fn outside_client(config: &Config, id: &str) -> String {
    String::from_utf8_lossy(&session_output(config, &["outside-client".into(), id.into()]).stdout)
        .trim()
        .to_owned()
}

fn session_call(config: &Config, args: &[String], quiet: bool) -> i32 {
    run_status(&config.session, args, quiet)
        .map(status_code)
        .unwrap_or(127)
}

fn read_text(path: &Path) -> String {
    fs::read_to_string(path)
        .map(|value| value.trim().to_owned())
        .unwrap_or_default()
}

fn write_text(path: &Path, value: &str) -> Result<(), String> {
    fs::write(path, format!("{value}\n"))
        .map_err(|error| format!("write {}: {error}", path.display()))
}

fn remove_file(path: &Path) {
    let _ = fs::remove_file(path);
}

fn display_mode(config: &Config, pid: &str) -> String {
    String::from_utf8_lossy(
        &session_output(config, &["display-mode".into(), pid.to_owned()]).stdout,
    )
    .trim()
    .to_owned()
}

fn session_alive(config: &Config, pid: &str) -> bool {
    session_output(config, &["alive".into(), pid.to_owned()])
        .status
        .success()
}

fn show_display(config: &Config, mode: &str) {
    if mode == "native" {
        if session_has_process(config, "SteamARM-X11.app/Contents/MacOS/X11.bin :2") {
            let _ = Command::new("osascript")
                .args([
                    "-e",
                    &format!("tell application id \"{X11_BUNDLE_ID}\" to activate"),
                ])
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .status();
        }
    } else {
        let _ = Command::new("open")
            .arg("vnc://127.0.0.1:5901")
            .stdin(Stdio::null())
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status();
    }
}

fn clear_session_files(config: &Config) {
    for name in [
        "running.pgid",
        "running.arch",
        "running.pid",
        "running.id",
        "running.display",
    ] {
        remove_file(&config.launcher.join(name));
    }
}

fn stop_guests(config: &Config) {
    let _ = session_call(
        config,
        &[
            "stop".into(),
            config.launcher.display().to_string(),
            "3".into(),
        ],
        false,
    );
    let _ = session_call(config, &["kill-guests".into()], false);
    remove_file(&config.root.join("tmp/fexhome/.steam/steam.pid"));
    clear_session_files(config);
    thread::sleep(Duration::from_secs(1));
    let _ = session_call(config, &["clear-stale-shm".into()], false);
}

fn app_plan(config: &Config, id: &str) -> Result<AppPlan, String> {
    for (path, label, target) in [
        (&config.settings_env, "Rust settings engine", "settings-env"),
        (
            &config.wine_options,
            "Rust Wine prefix tool",
            "wine-prefix-options",
        ),
        (&config.app_plan, "Rust app planner", "app-plan"),
    ] {
        if !is_executable(path) {
            return Err(format!(
                "run-app: {label} missing at {} (run make {target})",
                path.display()
            ));
        }
    }
    let output = command_output(
        &config.app_plan,
        &[
            "--json".into(),
            id.into(),
            config.launcher.join("apps.json").display().to_string(),
            config.launcher.join("settings.json").display().to_string(),
            config
                .path("scripts/builtin-apps.json")
                .display()
                .to_string(),
        ],
    )
    .map_err(|error| format!("run-app: cannot start Rust app planner: {error}"))?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).trim().to_owned());
    }
    serde_json::from_slice(&output.stdout)
        .map_err(|error| format!("run-app: invalid Rust app plan: {error}"))
}

fn is_frame_steam(plan: &AppPlan) -> bool {
    plan.kind == "steam" && plan.architecture == "aarch64" && plan.root == ARM64_ROOT
}

fn runner_for(plan: &AppPlan) -> (String, String) {
    if is_frame_steam(plan) {
        // The Steam Frame client needs the dedicated Rust runner: besides
        // setting the 4 KiB guest ABI, it passes the Mac's host page size to
        // SteamARM's FEX compatibility tool. run-native.sh omits that value,
        // so FEX appears in Steam but exits as soon as a game selects it.
        ("scripts/run-steam-arm64.sh".into(), "none".into())
    } else if !plan.android_package.is_empty() {
        ("scripts/android-session.py".into(), "FEX".into())
    } else if plan.architecture == "aarch64" {
        ("scripts/run-native.sh".into(), "none".into())
    } else {
        ("scripts/run-fex.sh".into(), "FEX".into())
    }
}

fn runner_args(plan: &AppPlan) -> Vec<String> {
    if is_frame_steam(plan) {
        // run-steam-arm64.sh already knows the Steam Frame client path; pass
        // only optional Steam startup arguments from the app entry.
        plan.command.iter().skip(1).cloned().collect()
    } else {
        plan.command.clone()
    }
}

fn effective_mode(plan: &AppPlan) -> Result<(String, String), String> {
    let mut mode = env::var("STEAMARM_DISPLAY").unwrap_or_else(|_| plan.display_mode.clone());
    if !plan.android_package.is_empty() && mode == "vnc" {
        eprintln!("run-app: {} is an Android app; its screen is a Weston window on the native X server, using native windows", plan.name);
        mode = "native".into();
    }
    if mode == "vnc" && !plan.in_x86_root {
        eprintln!(
            "run-app: {} is outside the x86 Steam root; VNC cannot serve it, using native windows",
            plan.name
        );
        mode = "native".into();
    }
    match mode.as_str() {
        "native" => Ok((mode, ":2".into())),
        "vnc" => Ok((mode, ":1".into())),
        _ => Err(format!(
            "run-app: unknown display mode '{mode}' (native or vnc)"
        )),
    }
}

fn dry_run(
    config: &Config,
    id: &str,
    plan: &AppPlan,
    mode: &str,
    display: &str,
    runner: &str,
    translator: &str,
) {
    println!("app:      {id} ({})", plan.name);
    println!("display:  {mode} (DISPLAY={display})");
    println!(
        "arch:     {} (translator: {translator}, session: ZERO-VM)",
        plan.architecture
    );
    if !plan.android_package.is_empty() {
        println!(
            "android:  {} in the Android session (root {}, Weston on {display})",
            plan.android_package, plan.root
        );
    } else if plan.architecture == "aarch64" {
        println!("root:     LXRT_ROOT={} DISPLAY={display}", plan.root);
    } else {
        println!(
            "root:     LXRT_ROOT={} FEX_ROOTFS={} DISPLAY={display}",
            plan.root, plan.fex_rootfs
        );
    }
    println!("env:      {}", plan.environment.join(" "));
    println!("command:  {runner} {}", runner_args(plan).join(" "));
    if mode == "native" {
        if session_has_process(config, "SteamARM-X11.app/Contents/MacOS/X11.bin :2") {
            println!("x11:      native X server running on :2");
        } else {
            println!("x11:      would start the native X server on :2 (scripts/run-x11-native.sh)");
        }
    } else {
        let pid = String::from_utf8_lossy(
            &session_output(
                config,
                &["first-process".into(), "lxrun /usr/bin/Xvnc".into()],
            )
            .stdout,
        )
        .trim()
        .to_owned();
        if pid.is_empty() {
            println!("xvnc:     would start at {}", plan.geometry);
        } else {
            let geometry = String::from_utf8_lossy(
                &session_output(config, &["xvnc-geometry".into(), pid.clone()]).stdout,
            )
            .trim()
            .to_owned();
            if geometry != plan.geometry {
                println!(
                    "xvnc:     running at {geometry}; restart to {} only if no app runs",
                    plan.geometry
                );
            } else {
                println!("xvnc:     running at {}", plan.geometry);
            }
        }
    }
    let guests = guest_pids(config).join(" ");
    println!(
        "guests:   {}",
        if guests.is_empty() { "none" } else { &guests }
    );
}

fn ensure_native_x(config: &Config) -> Result<(), String> {
    let status = run_stdout_null(
        &config.path("scripts/run-x11-native.sh"),
        &["start".into(), ":2".into()],
    )
    .map_err(|error| format!("The native X server (display :2) failed to start: {error}"))?;
    if status.success() {
        Ok(())
    } else {
        Err("The native X server (display :2) failed to start.".into())
    }
}

fn xvnc_pid(config: &Config) -> String {
    String::from_utf8_lossy(
        &session_output(
            config,
            &["first-process".into(), "lxrun /usr/bin/Xvnc".into()],
        )
        .stdout,
    )
    .trim()
    .to_owned()
}

fn xvnc_geometry(config: &Config, pid: &str) -> String {
    String::from_utf8_lossy(
        &session_output(config, &["xvnc-geometry".into(), pid.to_owned()]).stdout,
    )
    .trim()
    .to_owned()
}

fn signal_process(config: &Config, pid: &str, signal: &str) {
    let _ = session_call(config, &["signal".into(), pid.into(), signal.into()], true);
}

fn ensure_xvnc(config: &Config, geometry: &str) -> Result<(), String> {
    let mut pid = xvnc_pid(config);
    if !pid.is_empty() && xvnc_geometry(config, &pid) != geometry && guest_pids(config).is_empty() {
        println!("Restarting Xvnc at {geometry}");
        signal_process(config, &pid, "TERM");
        for _ in 0..10 {
            if !session_alive(config, &pid) {
                break;
            }
            thread::sleep(Duration::from_millis(500));
        }
        signal_process(config, &pid, "KILL");
        remove_file(&config.root.join("tmp/.X1-lock"));
        remove_file(&config.root.join("tmp/.X11-unix/X1"));
        pid.clear();
    }
    if !pid.is_empty() {
        return Ok(());
    }
    let status = run_status(&config.vnc, &["password".into()], false)
        .map_err(|error| format!("run-app: cannot make VNC password: {error}"))?;
    if !status.success() {
        return Err("run-app: cannot make VNC password".into());
    }
    fs::copy(
        config.state.join("vncpasswd"),
        config.root.join("tmp/.vncpasswd"),
    )
    .map_err(|error| format!("run-app: cannot install VNC password: {error}"))?;
    if !session_has_process(config, "lxrun /tmp/lxrt-root/usr/bin/FEXServer") {
        let _ = run_status(
            &config.path("scripts/run-fex.sh"),
            &["/bin/true".into()],
            true,
        );
    }
    fs::create_dir_all(&config.logs).map_err(|error| format!("create logs directory: {error}"))?;
    let log = OpenOptions::new()
        .create(true)
        .append(true)
        .open(config.logs.join("xvnc.log"))
        .map_err(|error| format!("open Xvnc log: {error}"))?;
    let mut command = Command::new("nohup");
    command
        .args([
            "./build/lxrun",
            "/usr/bin/Xvnc",
            ":1",
            "-geometry",
            geometry,
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
        ])
        .current_dir(&config.repo)
        .env("TMPDIR", "/tmp")
        .env("HOME", "/tmp/fexhome")
        .env("FEX_ROOTFS", "/")
        .env("FEX_GUESTBASE", "1")
        .env("LXRT_ROOT", X86_ROOT)
        .stdin(Stdio::null())
        .stdout(Stdio::from(
            log.try_clone().map_err(|error| error.to_string())?,
        ))
        .stderr(Stdio::from(log));
    command
        .spawn()
        .map_err(|error| format!("run-app: cannot start Xvnc: {error}"))?;
    thread::sleep(Duration::from_secs(3));
    Ok(())
}

fn in_x11(
    config: &Config,
    mode: &str,
    socket: &Path,
    program: &Path,
    args: &[String],
    envs: &[(&str, &str)],
    quiet_stdout: bool,
) -> io::Result<ExitStatus> {
    let spawn = config.build("x11spawn");
    let spawn_enabled = env::var("STEAMARM_X11_SPAWN")
        .map(|value| value != "0")
        .unwrap_or(true);
    if mode == "native" && spawn_enabled && is_socket(socket) && spawn.is_file() {
        let mut command = Command::new(&spawn);
        command
            .args(["--wait", "--stdio", "/dev/null"])
            .arg(socket)
            .arg(program)
            .args(args)
            .envs(envs.iter().copied())
            .stdin(Stdio::null());
        if quiet_stdout {
            command.stdout(Stdio::null());
        }
        let status = command.status()?;
        if status.code() != Some(125) {
            return Ok(status);
        }
    }
    let mut command = Command::new(program);
    command
        .args(args)
        .envs(envs.iter().copied())
        .stdin(Stdio::null());
    if quiet_stdout {
        command.stdout(Stdio::null());
    }
    command.status()
}

fn ensure_frame_root(config: &Config, android: bool) -> Result<(), String> {
    let image = config.state.join("steamframe-root.sparsebundle");
    if android || !image.is_dir() {
        return Ok(());
    }
    let link = fs::read_link(config.state.join("arm64root")).unwrap_or_default();
    let mut components = link.components();
    if components.next() != Some(std::path::Component::RootDir)
        || components
            .next()
            .is_none_or(|part| part.as_os_str() != "Volumes")
    {
        return Ok(());
    }
    let Some(volume) = components.next() else {
        return Ok(());
    };
    if components.next().is_none() {
        return Ok(());
    }
    let mount = Path::new("/Volumes").join(volume.as_os_str());
    let status = run_status(
        &config.path("scripts/image-volume.sh"),
        &[
            "ensure".into(),
            image.display().to_string(),
            mount.display().to_string(),
        ],
        false,
    )
    .map_err(|error| format!("attach Steam Frame root: {error}"))?;
    if status.success() {
        Ok(())
    } else {
        Err("run-app: could not attach Steam Frame root".into())
    }
}

fn apply_resolution_scaling(config: &Config) {
    if session_has_process(config, "build/lxrun .*wineserver") {
        return;
    }
    let mode = output_text(
        &config.settings_env,
        &[
            "--emulate-modeset".into(),
            config.launcher.join("settings.json").display().to_string(),
        ],
    );
    let _ = run_status(
        &config.wine_options,
        &["emulate-modeset".into(), mode],
        true,
    );
}

fn setting_volume(config: &Config) -> String {
    output_text(
        &config.settings_env,
        &[
            "--volume".into(),
            config.launcher.join("settings.json").display().to_string(),
        ],
    )
}

fn ensure_audio(
    config: &Config,
    mode: &str,
    socket: &Path,
    root: &str,
    kind: &str,
    volume: &str,
) -> Result<(), String> {
    if volume.is_empty() {
        return Ok(());
    }
    let audio = config.path("scripts/audio.sh");
    let args = vec!["start".to_owned(), volume.to_owned()];
    let first = in_x11(
        config,
        mode,
        socket,
        &audio,
        &args,
        &[("LXRT_ROOT", root)],
        true,
    )
    .map(|status| status.success())
    .unwrap_or(false);
    let second = if first {
        true
    } else {
        run_status_env(&audio, &args, &[("LXRT_ROOT", root)], true, false)
            .map(|status| status.success())
            .unwrap_or(false)
    };
    if !second && kind == "steam" {
        return Err(format!("run-app: the sound server (PulseAudio) did not start, and Steam's client does not open without it; not starting this client. Log: {}", config.logs.join("pulseaudio.log").display()));
    }
    if !second {
        eprintln!("run-app: no sound (scripts/audio.sh)");
    }
    Ok(())
}

fn ensure_display(config: &Config, mode: &str, geometry: &str) -> Result<(), String> {
    if mode == "native" {
        ensure_native_x(config)
    } else {
        ensure_xvnc(config, geometry)
    }
}

fn guests_other_app_error(config: &Config) -> bool {
    !guest_pids(config).is_empty()
}

fn timestamp() -> String {
    output_text(Path::new("/bin/date"), &["+%Y%m%d-%H%M%S".into()])
}

fn log_file(config: &Config, id: &str) -> Result<PathBuf, String> {
    fs::create_dir_all(&config.logs).map_err(|error| format!("create log directory: {error}"))?;
    let name = format!("{id}-{}.log", timestamp());
    let path = config.logs.join(name);
    File::create(&path).map_err(|error| format!("create log {}: {error}", path.display()))?;
    write_text(&config.logs.join("current"), &path.display().to_string())?;
    Ok(path)
}

fn ensure_audio_for_games(config: &Config, mode: &str, socket: &Path, root: &str, volume: &str) {
    if volume.is_empty() || root == X86_ROOT || !Path::new(X86_ROOT).join("usr").is_dir() {
        return;
    }
    let audio = config.path("scripts/audio.sh");
    let args = vec!["start".to_owned()];
    if !in_x11(
        config,
        mode,
        socket,
        &audio,
        &args,
        &[("LXRT_ROOT", X86_ROOT)],
        true,
    )
    .map(|status| status.success())
    .unwrap_or(false)
    {
        eprintln!("run-app: no sound for games (scripts/audio.sh)");
    }
}

fn ensure_root(plan: &AppPlan) -> Result<(), String> {
    if !plan.android_package.is_empty() {
        return Ok(());
    }
    let root = Path::new(&plan.root);
    if !root.join("usr").is_dir() {
        return Err(format!(
            "run-app: the root {} of {} is not there (a volume that is not attached?)",
            plan.root, plan.name
        ));
    }
    if root.join("tmp").is_dir() && !is_writable(&root.join("tmp")) {
        return Err(format!("run-app: the root {} of {} is read-only (its disk image attached read-only?): stop what uses it, then start again", plan.root, plan.name));
    }
    Ok(())
}

fn ensure_links(config: &Config, android: bool) -> Result<(), String> {
    if android {
        return Ok(());
    }
    let status = run_stdout_null(
        &config.path("scripts/env-links.sh"),
        &[config.state.display().to_string()],
    )
    .map_err(|error| format!("run-app: cannot link guest roots: {error}"))?;
    if status.success() {
        Ok(())
    } else {
        Err("run-app: guest root links could not be prepared".into())
    }
}

fn parse_plan_env(plan: &AppPlan) -> Vec<(String, String)> {
    plan.environment
        .iter()
        .filter_map(|entry| {
            let (key, value) = entry.split_once('=')?;
            Some((key.to_owned(), value.to_owned()))
        })
        .collect()
}

fn app_kind_is_steam(plan: &AppPlan) -> bool {
    plan.kind == "steam"
}

fn setup_arm64_steam(
    config: &Config,
    plan: &AppPlan,
    mode: &str,
    socket: &Path,
    volume: &str,
    envs: &[(String, String)],
) {
    if plan.architecture != "aarch64" || !app_kind_is_steam(plan) {
        return;
    }
    if !session_has_process(config, "lxrun /tmp/lxrt-root/usr/bin/FEXServer") {
        let _ = in_x11(
            config,
            mode,
            socket,
            &config.path("scripts/run-fex.sh"),
            &["/bin/true".into()],
            &[],
            true,
        );
    }
    let guest_home = envs
        .iter()
        .find_map(|(key, value)| {
            (key == "HOME_IN_GUEST" && value.starts_with('/')).then_some(value.as_str())
        })
        .unwrap_or("/tmp/armhome");
    let _ = run_status(
        &config.path("scripts/steam-arm64-links.sh"),
        &[plan.root.clone(), guest_home.to_owned()],
        false,
    );
    if !volume.is_empty() {
        let pipewire = config.path("scripts/pipewire.sh");
        let args = vec!["start".into(), plan.root.clone()];
        let status = in_x11(config, mode, socket, &pipewire, &args, &[], true);
        if !status.is_ok_and(|status| status.success()) {
            eprintln!("run-app: no PipeWire for Steam's audio settings (scripts/pipewire.sh)");
        }
    }
    let tool_dir = Path::new(&plan.root)
        .join(guest_home.trim_start_matches('/'))
        .join(".local/share/Steam/compatibilitytools.d");
    let source = config.build("steamarm-fex-proton-arm64");
    if !source.is_file() {
        return;
    }
    for name in ["steamarm-fex-proton", "steamarm-fex-linux"] {
        let target = tool_dir.join(name).join(name);
        if target.is_file() && !same_file(&target, &source) {
            if fs::copy(&source, &target).is_ok() {
                use std::os::unix::fs::PermissionsExt;
                let _ = fs::set_permissions(&target, fs::Permissions::from_mode(0o755));
            }
        }
    }
}

fn same_file(left: &Path, right: &Path) -> bool {
    fs::read(left).ok() == fs::read(right).ok()
}

fn spawn_watcher(config: &Config, leader: &str, log: &File) {
    let Ok(stdout) = log.try_clone() else {
        return;
    };
    let Ok(stderr) = log.try_clone() else {
        return;
    };
    let mut command = Command::new("nohup");
    command
        .arg(&config.session)
        .args(["reap-after", leader])
        .stdin(Stdio::null())
        .stdout(Stdio::from(stdout))
        .stderr(Stdio::from(stderr));
    let _ = command.spawn();
}

fn run_start(config: &Config, id: &str, dry: bool) -> Result<i32, String> {
    fs::create_dir_all(&config.logs)
        .and_then(|_| fs::create_dir_all(&config.launcher))
        .map_err(|error| format!("run-app: cannot create state directories: {error}"))?;
    let plan = app_plan(config, id)?;
    env::remove_var("STEAMARM_GRAPHICS_BACKEND");
    env::remove_var("STEAMARM_SYNCHRONIZATION");
    if !plan.android_package.is_empty() {
        if env::var_os("ANDROID_SESSION_COMPOSITOR").is_none() {
            env::set_var("ANDROID_SESSION_COMPOSITOR", "wlmac");
        }
    }
    let (runner, translator) = runner_for(&plan);
    let args = runner_args(&plan);
    let (mode, display) = effective_mode(&plan)?;
    let socket = config
        .launcher
        .join(format!("x11-spawn{}.sock", display.trim_start_matches(':')));
    if dry {
        dry_run(config, id, &plan, &mode, &display, &runner, &translator);
        return Ok(0);
    }
    let _ = session_call(
        config,
        &[
            "reap".into(),
            read_text(&config.launcher.join("running.pgid")),
        ],
        true,
    );
    if guests_other_app_error(config) {
        let running_id = read_text(&config.launcher.join("running.id"));
        let running_pid = read_text(&config.launcher.join("running.pid"));
        let running_mode = read_text(&config.launcher.join("running.display"));
        if running_id == id && !running_pid.is_empty() && session_alive(config, &running_pid) {
            show_display(
                config,
                if running_mode.is_empty() {
                    &mode
                } else {
                    &running_mode
                },
            );
            println!("{} is already running.", plan.name);
            println!("session=reshown");
            return Ok(0);
        }
        let adopted = outside_client(config, id);
        if !adopted.is_empty() {
            let detected = display_mode(config, &adopted);
            let mode = if detected.is_empty() { mode } else { detected };
            remove_file(&config.launcher.join("running.status"));
            remove_file(&config.launcher.join("running.pgid"));
            remove_file(&config.launcher.join("running.arch"));
            if id.starts_with("steam-arm64") {
                write_text(&config.launcher.join("running.arch"), "aarch64 none")?;
            }
            write_text(&config.launcher.join("running.pid"), &adopted)?;
            write_text(&config.launcher.join("running.id"), id)?;
            write_text(&config.launcher.join("running.display"), &mode)?;
            show_display(config, &mode);
            println!("Steam is already running.");
            println!("session=adopted");
            return Ok(0);
        }
        return Err(
            "Another Linux program is running (scripts/run-app.sh --stop stops it).".into(),
        );
    }

    ensure_frame_root(config, !plan.android_package.is_empty())?;
    ensure_links(config, !plan.android_package.is_empty())?;
    ensure_root(&plan)?;
    if !plan.prefix.is_empty() {
        fs::create_dir_all(format!("{}{}", plan.root, plan.prefix))
            .map_err(|error| format!("run-app: cannot create app prefix: {error}"))?;
    }
    ensure_display(config, &mode, &plan.geometry)?;

    let volume = if plan.android_package.is_empty() {
        apply_resolution_scaling(config);
        let volume = setting_volume(config);
        ensure_audio(config, &mode, &socket, &plan.root, &plan.kind, &volume)?;
        ensure_audio_for_games(config, &mode, &socket, &plan.root, &volume);
        if !run_stdout_null(&config.path("scripts/input.sh"), &["start".into()])
            .is_ok_and(|status| status.success())
        {
            eprintln!("run-app: no controllers for games (scripts/input.sh)");
        }
        volume
    } else {
        String::new()
    };

    safeguard::start(&config.repo);

    let logfile_path = log_file(config, id)?;
    remove_file(&config.launcher.join("running.status"));
    remove_file(&config.launcher.join("running.pgid"));
    let mut base_env = parse_plan_env(&plan);
    let backing = output_text(&config.path("scripts/image-volume.sh"), &["backing".into()]);
    env::set_var("LXRT_STATFS_BACKING", &backing);
    base_env.push(("LXRT_STATFS_BACKING".into(), backing));
    setup_arm64_steam(config, &plan, &mode, &socket, &volume, &base_env);

    let mut process_env = base_env;
    process_env.push(("DISPLAY".into(), display.clone()));
    process_env.push(("LXRT_ROOT".into(), plan.root.clone()));
    if plan.architecture != "aarch64" {
        process_env.push(("FEX_ROOTFS".into(), plan.fex_rootfs.clone()));
    }
    process_env.push(("PYTHONCOERCECLOCALE".into(), "0".into()));
    process_env.push(("STEAMARM_SESSION_PY".into(), "1".into()));
    if plan.kind == "steam" {
        process_env.push(("STEAMARM_SESSION_RESTART".into(), "42".into()));
    }
    let mut session_args = vec![
        "run".into(),
        config.launcher.display().to_string(),
        runner.clone(),
    ];
    session_args.extend(args.iter().cloned());
    let mut logfile = OpenOptions::new()
        .create(true)
        .append(true)
        .open(&logfile_path)
        .map_err(|error| format!("open app log {}: {error}", logfile_path.display()))?;

    let mut leader = String::new();
    let spawn = config.build("x11spawn");
    let spawn_enabled = env::var("STEAMARM_X11_SPAWN")
        .map(|value| value != "0")
        .unwrap_or(true);
    if mode == "native" && spawn_enabled && is_socket(&socket) && spawn.is_file() {
        let stderr = logfile.try_clone().map_err(|error| error.to_string())?;
        let mut command = Command::new(spawn);
        command
            .args(["--stdio"])
            .arg(&logfile_path)
            .arg(&socket)
            .arg(&config.session)
            .args(&session_args)
            .args(&args)
            .envs(process_env.iter().map(|(key, value)| (key, value)))
            .stdin(Stdio::null())
            .stderr(Stdio::from(stderr));
        if let Ok(output) = command.output() {
            if output.status.success() {
                leader = String::from_utf8_lossy(&output.stdout).trim().to_owned();
                if !leader.is_empty() {
                    let _ = writeln!(logfile, "run-app: started by the X server (pid {leader})");
                }
            }
        }
    }
    if leader.is_empty() {
        let stdout = logfile.try_clone().map_err(|error| error.to_string())?;
        let stderr = logfile.try_clone().map_err(|error| error.to_string())?;
        let mut command = Command::new("nohup");
        command
            .arg(&config.session)
            .args(&session_args)
            .args(&args)
            .envs(process_env.iter().map(|(key, value)| (key, value)))
            .stdin(Stdio::null())
            .stdout(Stdio::from(stdout))
            .stderr(Stdio::from(stderr));
        let child = command
            .spawn()
            .map_err(|error| format!("run-app: cannot start session: {error}"))?;
        leader = child.id().to_string();
    }
    write_text(&config.launcher.join("running.pid"), &leader)?;
    write_text(&config.launcher.join("running.id"), id)?;
    write_text(&config.launcher.join("running.display"), &mode)?;
    write_text(
        &config.launcher.join("running.arch"),
        &format!("{} {translator}", plan.architecture),
    )?;
    spawn_watcher(config, &leader, &logfile);
    println!(
        "{} ({}, translator: {translator}) starting on {display} (log {}).",
        plan.name,
        plan.architecture,
        logfile_path.display()
    );
    if mode == "vnc" {
        show_display(config, "vnc");
        println!(
            "Screen Sharing: password in {}",
            config.state.join("vncpasswd.txt").display()
        );
    } else {
        println!("Its windows open as native macOS windows.");
    }
    println!("session=started");
    Ok(0)
}

fn run_cli() -> Result<i32, (i32, String)> {
    let config = Config::new();
    config.export_tool_paths();
    let args = env::args().skip(1).collect::<Vec<_>>();
    if args.is_empty() || args[0] == "--help" || args[0] == "-h" {
        print!("{}", usage());
        return Ok(0);
    }
    match args[0].as_str() {
        "--stop" => {
            stop_guests(&config);
            Ok(0)
        }
        "--reap" => {
            let sid = args
                .get(1)
                .cloned()
                .unwrap_or_else(|| read_text(&config.launcher.join("running.pgid")));
            Ok(session_call(&config, &["reap".into(), sid], false))
        }
        "--reap-after" => {
            let Some(sid) = args.get(1) else {
                return Err((2, usage().into()));
            };
            Ok(session_call(
                &config,
                &["reap-after".into(), sid.clone()],
                false,
            ))
        }
        "--dry-run" => {
            let Some(id) = args.get(1) else {
                return Err((2, usage().into()));
            };
            run_start(&config, id, true).map_err(|error| (2, error))
        }
        _ => run_start(&config, &args[0], false).map_err(|error| {
            let code = if error.starts_with("Another Linux program") {
                3
            } else if error.starts_with("run-app: the sound server") {
                4
            } else if error.starts_with("The native X server") {
                1
            } else {
                2
            };
            (code, error)
        }),
    }
}

fn main() {
    match run_cli() {
        Ok(code) => std::process::exit(code),
        Err((code, message)) => {
            eprintln!("{message}");
            std::process::exit(code);
        }
    }
}
