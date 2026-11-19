use std::env;
use std::ffi::{c_int, OsStr, OsString};
use std::fs::{self, OpenOptions};
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::Duration;

const INPUT_DIR: &str = "/tmp/lxrt-input";
const SIGTERM: c_int = 15;

unsafe extern "C" {
    fn kill(pid: c_int, signal: c_int) -> c_int;
}

fn project_root() -> PathBuf {
    if let Some(root) = env::var_os("STEAMARM_PROJECT_ROOT") {
        return PathBuf::from(root);
    }
    if let Ok(current) = env::current_dir() {
        if current.join("scripts/input.sh").is_file() {
            return current;
        }
    }
    env::current_dir().unwrap_or_else(|_| PathBuf::from("."))
}

fn nonempty_env(name: &str) -> Option<OsString> {
    env::var_os(name).filter(|value| !value.is_empty())
}

fn state_root(home: &Path) -> PathBuf {
    if let Some(state) = nonempty_env("STEAMARM_STATE") {
        return PathBuf::from(state);
    }
    let preferred = home.join("Developer/SteamARM-roots");
    let legacy = home.join("SteamARM-roots");
    if preferred.is_dir() || !legacy.is_dir() {
        preferred
    } else {
        legacy
    }
}

fn build_root(home: &Path) -> PathBuf {
    if let Some(build) = nonempty_env("STEAMARM_BUILD") {
        return PathBuf::from(build);
    }
    let preferred = home.join("Developer/SteamARM-build");
    let legacy = home.join("SteamARM-build");
    if preferred.is_dir() || !legacy.is_dir() {
        preferred
    } else {
        legacy
    }
}

fn daemon_path(root: &Path, build: &Path) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(path) = nonempty_env("STEAMARM_INPUTD_BIN") {
        candidates.push(PathBuf::from(path));
    }
    candidates.push(root.join("build/steamarm-inputd"));
    candidates.push(root.join("build/SteamARM.app/Contents/Resources/steamarm-inputd"));
    candidates.push(build.join("steamarm-inputd"));
    candidates.into_iter().find(|path| {
        fs::metadata(path)
            .map(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
            .unwrap_or(false)
    })
}

fn read_pid(pid_file: &Path) -> Option<i32> {
    fs::read_to_string(pid_file).ok()?.trim().parse().ok()
}

fn running(pid_file: &Path) -> Option<i32> {
    let pid = read_pid(pid_file)?;
    (unsafe { kill(pid, 0) } == 0).then_some(pid)
}

fn devices(directory: &Path) -> String {
    let mut names = fs::read_dir(directory)
        .into_iter()
        .flatten()
        .flatten()
        .filter_map(|entry| entry.file_name().into_string().ok())
        .filter(|name| name.starts_with("event"))
        .collect::<Vec<_>>();
    names.sort();
    names.join(" ")
}

fn start(root: &Path, state: &Path, directory: &Path, pid_file: &Path) -> i32 {
    if running(pid_file).is_some() {
        return 0;
    }
    let home = env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    let build = build_root(&home);
    let Some(binary) = daemon_path(root, &build) else {
        eprintln!("input: steamarm-inputd not built (tools/inputd/build.sh)");
        return 1;
    };
    let logs = state.join("logs");
    if let Err(error) = fs::create_dir_all(&logs) {
        eprintln!("input: cannot create {}: {error}", logs.display());
        return 1;
    }
    let log_path = logs.join("inputd.log");
    let stdout = match OpenOptions::new().create(true).append(true).open(&log_path) {
        Ok(log) => log,
        Err(error) => {
            eprintln!("input: cannot open {}: {error}", log_path.display());
            return 1;
        }
    };
    let stderr = match stdout.try_clone() {
        Ok(log) => log,
        Err(error) => {
            eprintln!("input: cannot attach log {}: {error}", log_path.display());
            return 1;
        }
    };
    let config = state.join("launcher/controllers.json");
    let child = Command::new(binary)
        .args([OsStr::new("--dir"), directory.as_os_str()])
        .arg("--config")
        .arg(config)
        .current_dir(root)
        .env("STEAMARM_STATE", state)
        .stdin(Stdio::null())
        .stdout(Stdio::from(stdout))
        .stderr(Stdio::from(stderr))
        .spawn();
    let mut child = match child {
        Ok(child) => child,
        Err(error) => {
            eprintln!("input: cannot start steamarm-inputd: {error}");
            return 1;
        }
    };
    let _ = child.wait();
    for _ in 0..30 {
        if running(pid_file).is_some() {
            return 0;
        }
        thread::sleep(Duration::from_millis(100));
    }
    eprintln!(
        "input: steamarm-inputd did not start (log {})",
        log_path.display()
    );
    1
}

fn usage() -> ! {
    eprintln!("Controllers for guest programs: steamarm-inputd exposes macOS controllers as Linux input devices.");
    eprintln!("Usage: scripts/input.sh start | stop | status");
    std::process::exit(2)
}

fn run() -> i32 {
    let root = project_root();
    let home = env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    let state = state_root(&home);
    let directory = nonempty_env("STEAMARM_INPUT_DIR")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(INPUT_DIR));
    let pid_file = directory.join("inputd.pid");
    match env::args().nth(1).as_deref().unwrap_or("status") {
        "start" => start(&root, &state, &directory, &pid_file),
        "stop" => {
            if let Some(pid) = running(&pid_file) {
                unsafe {
                    kill(pid, SIGTERM);
                }
            }
            0
        }
        "status" => {
            match running(&pid_file) {
                Some(pid) => println!(
                    "input: running (pid {pid}), devices: {}",
                    devices(&directory)
                ),
                None => println!("input: stopped"),
            }
            0
        }
        _ => usage(),
    }
}

fn main() {
    std::process::exit(run());
}
