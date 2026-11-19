use std::env;
use std::fs::{self, File, OpenOptions};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::Duration;

struct X11 {
    bundle: PathBuf,
    binary: PathBuf,
    bundle_id: String,
    log_dir: PathBuf,
    spawn_dir: PathBuf,
    log: PathBuf,
    stdio_log: PathBuf,
    xdpyinfo: PathBuf,
    display: String,
    number: String,
    lock: PathBuf,
    socket: PathBuf,
    wm: PathBuf,
}

fn state_root(home: &Path) -> PathBuf {
    if let Some(path) = env::var_os("STEAMARM_STATE") {
        return PathBuf::from(path);
    }
    let developer = home.join("Developer/SteamARM-roots");
    if developer.is_dir() || !home.join("SteamARM-roots").is_dir() {
        developer
    } else {
        home.join("SteamARM-roots")
    }
}

fn build_root(home: &Path) -> PathBuf {
    if let Some(path) = env::var_os("STEAMARM_BUILD") {
        return PathBuf::from(path);
    }
    let developer = home.join("Developer/SteamARM-build");
    if developer.is_dir() || !home.join("SteamARM-build").is_dir() {
        developer
    } else {
        home.join("SteamARM-build")
    }
}

fn process_table() -> Vec<(i32, String)> {
    let Ok(output) = Command::new("ps").args(["-axo", "pid=,command="]).output() else {
        return Vec::new();
    };
    String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter_map(|line| {
            let line = line.trim();
            let (pid, command) = line.split_once(char::is_whitespace)?;
            Some((pid.trim().parse().ok()?, command.trim().to_owned()))
        })
        .collect()
}

fn pid_alive(pid: i32) -> bool {
    Command::new("kill")
        .args(["-0", &pid.to_string()])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|s| s.success())
}

fn command_for(pid: i32) -> String {
    Command::new("ps")
        .args(["-o", "command=", "-p", &pid.to_string()])
        .output()
        .ok()
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_owned())
        .unwrap_or_default()
}

fn server_pid(x11: &X11) -> Option<i32> {
    let pid = fs::read_to_string(&x11.lock)
        .ok()?
        .trim()
        .parse::<i32>()
        .ok()?;
    (pid_alive(pid) && command_for(pid).contains("SteamARM-X11.app/Contents/MacOS/X11.bin"))
        .then_some(pid)
}

fn x11_servers() -> Vec<(i32, String)> {
    process_table()
        .into_iter()
        .filter(|(_, command)| command.contains("SteamARM-X11.app/Contents/MacOS/X11.bin"))
        .collect()
}

fn command_display(command: &str) -> Option<&str> {
    command.split_whitespace().find(|arg| {
        arg.strip_prefix(':')
            .is_some_and(|number| !number.is_empty() && number.chars().all(|c| c.is_ascii_digit()))
    })
}

fn process_environment(pid: i32) -> String {
    Command::new("ps")
        .args(["eww", "-p", &pid.to_string()])
        .output()
        .ok()
        .map(|output| String::from_utf8_lossy(&output.stdout).into_owned())
        .unwrap_or_default()
}

fn belongs_to_display(pid: i32, command: &str, x11: &X11) -> Option<bool> {
    if let Some(display) = command_display(command) {
        return Some(display == x11.display);
    }

    // XQuartz sometimes re-parents/restarts X11.bin without keeping `:N` in
    // its command line. Its launch environment still carries the per-display
    // spawn socket (and log path), which lets stop/start target the orphan
    // without disturbing a different SteamARM display.
    let environment = process_environment(pid);
    let socket = x11
        .spawn_dir
        .join(format!("x11-spawn{}.sock", x11.number))
        .to_string_lossy()
        .into_owned();
    let expected_socket = format!("XQUARTZ_SPAWN_SOCKET={socket}");
    if environment.contains(&expected_socket) {
        return Some(true);
    }
    if environment.contains("XQUARTZ_SPAWN_SOCKET=") {
        return Some(false);
    }
    let expected_log = format!("XQUARTZ_LOG_FILE={}", x11.log.display());
    if environment.contains(&expected_log) {
        return Some(true);
    }
    if environment.contains("XQUARTZ_LOG_FILE=") {
        return Some(false);
    }
    None
}

fn terminate_x11_pid(pid: i32, x11: &X11) {
    let _ = Command::new("kill")
        .args(["-TERM", &pid.to_string()])
        .status();
    for _ in 0..50 {
        if !pid_alive(pid) {
            return;
        }
        thread::sleep(Duration::from_millis(200));
    }
    message(x11, &format!("pid {pid} did not stop; sending SIGKILL"));
    let _ = Command::new("kill")
        .args(["-KILL", &pid.to_string()])
        .status();
    thread::sleep(Duration::from_millis(300));
}

fn reap_display_servers(x11: &X11) {
    let servers = x11_servers();
    let reap_sole_unknown = servers.len() == 1
        && belongs_to_display(servers[0].0, &servers[0].1, x11).is_none();
    for (pid, command) in servers {
        if belongs_to_display(pid, &command, x11) == Some(true) || reap_sole_unknown {
            message(x11, &format!("stopping stale X11 server pid {pid}"));
            terminate_x11_pid(pid, x11);
        }
    }
}

fn responsive(x11: &X11) -> bool {
    Command::new(&x11.xdpyinfo)
        .env("DISPLAY", &x11.display)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|s| s.success())
}

fn message(x11: &X11, text: &str) {
    eprintln!("[x11-native {}] {text}", x11.display);
}

fn run_defaults(bundle_id: &str, key: &str, kind: &str, value: &str) -> Result<(), String> {
    let status = Command::new("defaults")
        .args(["write", bundle_id, key, kind, value])
        .status()
        .map_err(|e| format!("cannot write XQuartz preference {key}: {e}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("cannot write XQuartz preference {key}"))
    }
}

fn spawn_wm(x11: &X11) {
    if !x11.wm.is_file() || fs::metadata(&x11.wm).is_ok_and(|m| m.permissions().mode() & 0o111 == 0)
    {
        message(
            x11,
            &format!(
                "no window manager ({}); windows get no title bars",
                x11.wm.display()
            ),
        );
        return;
    }
    if process_table()
        .iter()
        .any(|(_, command)| command.contains(&x11.wm.to_string_lossy().to_string()))
    {
        return;
    }
    let Ok(log) = OpenOptions::new()
        .create(true)
        .append(true)
        .open(x11.log_dir.join("quartz-wm.log"))
    else {
        message(x11, "cannot open quartz-wm log");
        return;
    };
    let Ok(error) = log.try_clone() else {
        message(x11, "cannot clone quartz-wm log");
        return;
    };
    let result = Command::new("nohup")
        .arg(&x11.wm)
        .current_dir("/")
        .env("DISPLAY", &x11.display)
        .stdin(Stdio::null())
        .stdout(Stdio::from(log))
        .stderr(Stdio::from(error))
        .spawn();
    if result.is_ok() {
        message(x11, "window manager started (quartz-wm)");
    }
}

fn start(x11: &X11) -> Result<(), String> {
    if let Some(pid) = server_pid(x11) {
        if responsive(x11) {
            message(x11, &format!("already running (pid {pid})"));
            spawn_wm(x11);
            return Ok(());
        }
        message(x11, &format!("pid {pid} is stale; restarting X11"));
    } else if responsive(x11) {
        message(x11, "display already answers; adopting it");
        spawn_wm(x11);
        return Ok(());
    }
    if !x11.binary.is_file()
        || fs::metadata(&x11.binary).is_ok_and(|m| m.permissions().mode() & 0o111 == 0)
    {
        return Err(format!(
            "XQuartz server not built: {} (run scripts/build-xquartz.sh)",
            x11.binary.display()
        ));
    }
    // A dead XQuartz instance can survive without its lock/socket and without
    // `:N` in argv. Reap the target display (or a sole unidentifiable orphan)
    // before refusing startup; the old code left pid 60386 behind and every
    // subsequent SteamARM launch failed with code 1.
    reap_display_servers(x11);
    let other = x11_servers()
        .into_iter()
        .filter(|(pid, command)| belongs_to_display(*pid, command, x11) != Some(true))
        .map(|(pid, _)| pid)
        .collect::<Vec<_>>();
    if !other.is_empty() {
        return Err(format!(
            "another SteamARM-X11 server is running (pids: {other:?}); stop it first"
        ));
    }
    if x11.lock.exists() {
        message(
            x11,
            &format!(
                "note: stale {} (server will clean it up)",
                x11.lock.display()
            ),
        );
    }

    run_defaults(&x11.bundle_id, "rootless", "-bool", "true")?;
    run_defaults(&x11.bundle_id, "nolisten_tcp", "-bool", "true")?;
    run_defaults(&x11.bundle_id, "no_randr_alert", "-bool", "true")?;
    run_defaults(&x11.bundle_id, "no_quit_alert", "-bool", "true")?;
    run_defaults(
        &x11.bundle_id,
        "window_item_modifiers",
        "-string",
        "command",
    )?;
    run_defaults(
        &x11.bundle_id,
        "enable_test_extensions",
        "-bool",
        if env::var("STEAMARM_X11_XTEST").as_deref() == Ok("0") {
            "false"
        } else {
            "true"
        },
    )?;

    fs::create_dir_all(&x11.log_dir).map_err(|e| format!("create X11 log directory: {e}"))?;
    fs::create_dir_all(&x11.spawn_dir)
        .map_err(|e| format!("create spawn-service directory: {e}"))?;
    if !Path::new("/tmp/.X11-unix").is_dir() {
        fs::create_dir_all("/tmp/.X11-unix")
            .map_err(|e| format!("create X11 socket directory: {e}"))?;
        fs::set_permissions("/tmp/.X11-unix", fs::Permissions::from_mode(0o1777))
            .map_err(|e| format!("set X11 socket-directory permissions: {e}"))?;
    }

    message(
        x11,
        &format!(
            "starting {} {} (log {})",
            x11.binary.display(),
            x11.display,
            x11.log.display()
        ),
    );
    let spawn_socket = x11.spawn_dir.join(format!("x11-spawn{}.sock", x11.number));
    let stdout = File::create(&x11.stdio_log).map_err(|e| format!("create X11 log: {e}"))?;
    let stderr = stdout
        .try_clone()
        .map_err(|e| format!("clone X11 log: {e}"))?;
    let mut open = Command::new("open");
    open.args(["-g", "-n", "-a"])
        .arg(&x11.bundle)
        .args(["--env", &format!("XQUARTZ_LOG_FILE={}", x11.log.display())])
        .args([
            "--env",
            &format!("XQUARTZ_SPAWN_SOCKET={}", spawn_socket.display()),
        ])
        .arg("--stdout")
        .arg(&x11.stdio_log)
        .arg("--stderr")
        .arg(&x11.stdio_log)
        .arg("--args")
        .arg(&x11.display)
        .args(["-nolisten", "tcp", "+iglx"])
        .args(
            env::var("X11_NATIVE_ARGS")
                .unwrap_or_default()
                .split_whitespace(),
        )
        .stdin(Stdio::null())
        .stdout(Stdio::from(stdout))
        .stderr(Stdio::from(stderr));
    let direct_mode = env::var("STEAMARM_X11_EXEC").as_deref() == Ok("1");
    let opened = !direct_mode && open.status().is_ok_and(|s| s.success());
    if !opened {
        let out = OpenOptions::new()
            .create(true)
            .append(true)
            .open(&x11.stdio_log)
            .map_err(|e| format!("open X11 log: {e}"))?;
        let error = out.try_clone().map_err(|e| format!("clone X11 log: {e}"))?;
        let mut direct = Command::new("nohup");
        direct
            .arg(&x11.binary)
            .arg(&x11.display)
            .args(["-nolisten", "tcp", "+iglx"])
            .args(
                env::var("X11_NATIVE_ARGS")
                    .unwrap_or_default()
                    .split_whitespace(),
            )
            .current_dir("/")
            .env_remove("DISPLAY")
            .env("CFProcessPath", x11.bundle.join("Contents/MacOS/X11"))
            .env("XQUARTZ_LOG_FILE", &x11.log)
            .env("XQUARTZ_SPAWN_SOCKET", spawn_socket)
            .stdin(Stdio::null())
            .stdout(Stdio::from(out))
            .stderr(Stdio::from(error))
            .spawn()
            .map_err(|e| format!("start XQuartz directly: {e}"))?;
    }
    for i in 1..=60 {
        if responsive(x11) {
            let pid = server_pid(x11).map_or_else(|| "?".into(), |p| p.to_string());
            message(
                x11,
                &format!(
                    "ready after ~{}s (pid {pid}, DISPLAY={})",
                    i / 4,
                    x11.display
                ),
            );
            spawn_wm(x11);
            return Ok(());
        }
        thread::sleep(Duration::from_millis(250));
    }
    Err(format!(
        "XQuartz did not answer within 15s; see {}",
        x11.log.display()
    ))
}

fn stop(x11: &X11) {
    if let Some(pid) = server_pid(x11) {
        message(x11, &format!("stopping pid {pid}"));
        terminate_x11_pid(pid, x11);
        let _ = fs::remove_file(&x11.lock);
        let _ = fs::remove_file(&x11.socket);
    } else {
        message(x11, "not running");
    }
    reap_display_servers(x11);
    for (pid, command) in process_table() {
        if command.contains(&x11.wm.to_string_lossy().to_string()) {
            let _ = Command::new("kill")
                .args(["-KILL", &pid.to_string()])
                .status();
        }
    }
}

fn usage() -> &'static str {
    "usage: run-x11-native.sh [start|stop|restart|status] [:N]"
}

fn run() -> Result<(), (i32, String)> {
    let home = env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"));
    let build = build_root(&home);
    let state = state_root(&home);
    let xq_root = env::var_os("XQ_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| build.join("xquartz"));
    let mut command = "start".to_owned();
    let mut display = ":2".to_owned();
    for arg in env::args().skip(1) {
        if ["start", "stop", "restart", "status"].contains(&arg.as_str()) {
            command = arg;
        } else if let Some(number) = arg.strip_prefix(':') {
            if number.is_empty() || !number.bytes().all(|b| b.is_ascii_digit()) {
                return Err((2, format!("unknown argument: {arg}\n{}", usage())));
            }
            display = arg;
        } else if !arg.is_empty() && arg.bytes().all(|b| b.is_ascii_digit()) {
            display = format!(":{arg}");
        } else if arg == "-h" || arg == "--help" {
            println!("{}", usage());
            return Ok(());
        } else {
            return Err((2, format!("unknown argument: {arg}\n{}", usage())));
        }
    }
    let number = display.trim_start_matches(':').to_owned();
    if number == "1" {
        return Err((
            2,
            "refusing display :1 (reserved for Xvnc / running Steam)".into(),
        ));
    }
    let log_dir = state.join("logs");
    let log = env::var_os("X11_NATIVE_LOG")
        .map(PathBuf::from)
        .unwrap_or_else(|| log_dir.join("x11-native.log"));
    let stdio_log = log.with_extension("stdio.log");
    let xdpyinfo = env::var_os("XDPYINFO")
        .map(PathBuf::from)
        .or_else(|| {
            Command::new("which")
                .arg("xdpyinfo")
                .output()
                .ok()
                .filter(|o| o.status.success())
                .map(|o| PathBuf::from(String::from_utf8_lossy(&o.stdout).trim()))
        })
        .unwrap_or_else(|| PathBuf::from("/opt/homebrew/bin/xdpyinfo"));
    let x11 = X11 {
        bundle: xq_root.join("SteamARM-X11.app"),
        binary: xq_root.join("SteamARM-X11.app/Contents/MacOS/X11.bin"),
        bundle_id: "org.steamarm.X11".into(),
        spawn_dir: state.join("launcher"),
        log_dir: log_dir.clone(),
        log,
        stdio_log,
        xdpyinfo,
        number: number.clone(),
        display,
        lock: PathBuf::from(format!("/tmp/.X{number}-lock")),
        socket: PathBuf::from(format!("/tmp/.X11-unix/X{number}")),
        wm: xq_root.join("quartz-wm/src/quartz-wm"),
    };
    match command.as_str() {
        "start" => start(&x11).map_err(|e| (1, e)),
        "stop" => {
            stop(&x11);
            Ok(())
        }
        "restart" => {
            stop(&x11);
            start(&x11).map_err(|e| (1, e))
        }
        "status" => {
            if let Some(pid) = server_pid(&x11) {
                if responsive(&x11) {
                    message(
                        &x11,
                        &format!("running (pid {pid}, socket {})", x11.socket.display()),
                    );
                } else {
                    message(&x11, &format!("pid {pid} alive but not answering"));
                }
                Ok(())
            } else {
                message(&x11, "not running");
                Err((3, "not running".into()))
            }
        }
        _ => unreachable!(),
    }
}

fn main() {
    if let Err((status, message)) = run() {
        if status != 3 {
            eprintln!("run-x11-native: {message}");
        }
        std::process::exit(status);
    }
}
