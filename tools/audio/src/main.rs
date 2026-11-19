use std::env;
use std::fs;
use std::io;
use std::os::unix::fs::FileTypeExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::Duration;

struct Paths {
    repo: PathBuf,
    pa: PathBuf,
    session: PathBuf,
    settings: PathBuf,
    dir: PathBuf,
    socket: PathBuf,
    log: PathBuf,
    conf: PathBuf,
    daemon_conf: PathBuf,
    server_file: PathBuf,
    binary_file: PathBuf,
}

fn selected(home: &Path, env_name: &str, name: &str) -> PathBuf {
    if let Some(path) = env::var_os(env_name).filter(|s| !s.is_empty()) {
        return PathBuf::from(path);
    }
    let preferred = home.join("Developer").join(name);
    let older = home.join(name);
    if preferred.is_dir() || !older.is_dir() {
        preferred
    } else {
        older
    }
}

fn paths() -> Paths {
    let home = env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"));
    let repo = env::var_os("STEAMARM_PROJECT_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../.."));
    let state = selected(&home, "STEAMARM_STATE", "SteamARM-roots");
    let build = selected(&home, "STEAMARM_BUILD", "SteamARM-build");
    let own_pa = build.join("pulseaudio/out/bin");
    let pa = if own_pa.join("pulseaudio").is_file()
        && env::var("STEAMARM_BREW_PULSE").as_deref() != Ok("1")
    {
        own_pa
    } else {
        PathBuf::from(env::var_os("BREW").unwrap_or_else(|| "/opt/homebrew".into()))
            .join("opt/pulseaudio/bin")
    };
    let root = env::var_os("LXRT_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp/lxrt-steamroot"));
    let dir = root.join("tmp/pulse");
    let launcher = state.join("launcher");
    Paths {
        repo: repo.clone(),
        pa,
        session: env::var_os("STEAMARM_SESSION_BIN")
            .map(PathBuf::from)
            .unwrap_or_else(|| repo.join("build/steamarm-session")),
        settings: env::var_os("STEAMARM_SETTINGS_ENV_BIN")
            .map(PathBuf::from)
            .unwrap_or_else(|| repo.join("build/steamarm-settings-env")),
        socket: dir.join("native"),
        dir,
        log: state.join("logs/pulseaudio.log"),
        conf: launcher.join("pulse.pa"),
        daemon_conf: launcher.join("pulse-daemon.conf"),
        server_file: launcher.join("pulse.socket"),
        binary_file: launcher.join("pulse.bin"),
    }
}

fn live(paths: &Paths, socket: &Path) -> bool {
    if !fs::metadata(socket).is_ok_and(|m| m.file_type().is_socket()) {
        return false;
    }
    Command::new(paths.pa.join("pactl"))
        .args(["-s", &format!("unix:{}", socket.display()), "info"])
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .is_ok_and(|s| s.success())
}

fn server(paths: &Paths) -> Option<PathBuf> {
    let saved = fs::read_to_string(&paths.server_file)
        .ok()
        .map(|s| PathBuf::from(s.trim()));
    if let Some(saved) = saved.filter(|s| live(paths, s)) {
        return Some(saved);
    }
    live(paths, &paths.socket).then(|| paths.socket.clone())
}

fn pactl(paths: &Paths, args: &[&str]) -> io::Result<std::process::Output> {
    let socket = server(paths).ok_or_else(|| {
        io::Error::new(io::ErrorKind::NotConnected, "PulseAudio socket is not live")
    })?;
    Command::new(paths.pa.join("pactl"))
        .args(["-s", &format!("unix:{}", socket.display())])
        .args(args)
        .output()
}

fn running(paths: &Paths) -> bool {
    server(paths).is_some()
}

fn command_capture(program: &str, args: &[&str]) -> Option<String> {
    let output = Command::new(program)
        .args(args)
        .stdin(Stdio::null())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .output()
        .ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

fn server_pid(paths: &Paths) -> Option<i32> {
    let pattern = format!("pulseaudio.*-F {}", paths.conf.display());
    command_capture("pgrep", &["-f", &pattern])?
        .lines()
        .find_map(|s| s.trim().parse().ok())
}

fn ps_command(pid: i32) -> Option<String> {
    command_capture("ps", &["-o", "command=", "-p", &pid.to_string()])
}

fn client_count(paths: &Paths) -> usize {
    pactl(paths, &["list", "clients", "short"])
        .ok()
        .filter(|o| o.status.success())
        .map(|o| {
            String::from_utf8_lossy(&o.stdout)
                .lines()
                .filter(|l| !l.contains("pactl"))
                .count()
        })
        .unwrap_or(usize::MAX)
}

fn modules(paths: &Paths) -> Vec<PathBuf> {
    let Ok(output) = pactl(paths, &["list", "modules", "short"]) else {
        return Vec::new();
    };
    if !output.status.success() {
        return Vec::new();
    }
    String::from_utf8_lossy(&output.stdout)
        .lines()
        .filter_map(|line| {
            if !line.contains("module-native-protocol-unix") {
                return None;
            }
            line.split_whitespace()
                .find_map(|word| word.strip_prefix("socket="))
                .filter(|socket| Path::new(socket) != paths.socket)
                .map(PathBuf::from)
        })
        .collect()
}

fn start(paths: &Paths, requested_volume: Option<&str>) -> Result<(), String> {
    let daemon = paths.pa.join("pulseaudio");
    if !daemon.is_file() {
        return Err(format!(
            "PulseAudio not installed at {} (install Homebrew pulseaudio)",
            daemon.display()
        ));
    }
    let mut restore = Vec::new();
    if running(paths) {
        if let Some(pid) = server_pid(paths) {
            let mut binary =
                ps_command(pid).and_then(|s| s.split_whitespace().next().map(PathBuf::from));
            if let Ok(saved) = fs::read_to_string(&paths.binary_file) {
                binary = Some(PathBuf::from(saved.trim()));
            }
            if let Some(binary) = binary {
                if binary != daemon && client_count(paths) == 0 {
                    restore = modules(paths);
                    let _ = Command::new("kill").arg(pid.to_string()).status();
                    for _ in 0..50 {
                        if !Command::new("kill")
                            .args(["-0", &pid.to_string()])
                            .stdin(Stdio::null())
                            .stdout(Stdio::null())
                            .stderr(Stdio::null())
                            .status()
                            .is_ok_and(|s| s.success())
                        {
                            break;
                        }
                        thread::sleep(Duration::from_millis(100));
                    }
                    let _ = fs::remove_file(&paths.socket);
                }
            }
        }
    }
    if !live(paths, &paths.socket) {
        fs::create_dir_all(&paths.dir)
            .map_err(|e| format!("create {}: {e}", paths.dir.display()))?;
        fs::create_dir_all(paths.log.parent().ok_or("invalid PulseAudio log path")?)
            .map_err(|e| format!("create log directory: {e}"))?;
        fs::create_dir_all(
            paths
                .conf
                .parent()
                .ok_or("invalid PulseAudio config path")?,
        )
        .map_err(|e| format!("create config directory: {e}"))?;
        let _ = fs::remove_file(&paths.socket);
        if running(paths) {
            let result = pactl(
                paths,
                &[
                    "load-module",
                    "module-native-protocol-unix",
                    &format!("socket={} auth-anonymous=1", paths.socket.display()),
                ],
            )
            .map_err(|e| format!("add PulseAudio socket: {e}"))?;
            if !result.status.success() {
                return Err(format!("cannot add socket at {}", paths.socket.display()));
            }
        } else {
            fs::write(&paths.conf, format!(
                "load-module module-coreaudio-detect\nload-module module-native-protocol-unix socket={} auth-anonymous=1\nload-module module-always-sink\nload-module module-suspend-on-idle timeout=3\n",
                paths.socket.display()))
                .map_err(|e| format!("write PulseAudio config: {e}"))?;
            fs::write(&paths.daemon_conf,
                "default-sample-format = float32le\ndefault-sample-rate = 48000\nalternate-sample-rate = 44100\nresample-method = soxr-hq\navoid-resampling = yes\nflat-volumes = no\ndefault-fragments = 4\ndefault-fragment-size-msec = 20\n")
                .map_err(|e| format!("write PulseAudio daemon config: {e}"))?;
            let _ = fs::remove_file(&paths.log);
            let child = Command::new(&paths.session)
                .arg("detach")
                .arg(&daemon)
                .args([
                    "--daemonize=no",
                    "--exit-idle-time=-1",
                    "--use-pid-file=no",
                    "--disallow-exit",
                    "--disable-shm=yes",
                    "-n",
                    "-F",
                ])
                .arg(&paths.conf)
                .arg(format!("--log-target=file:{}", paths.log.display()))
                .env("PULSE_CONFIG", &paths.daemon_conf)
                .env("PYTHONCOERCECLOCALE", "0")
                .env("STEAMARM_SESSION_PY", "1")
                .current_dir(&paths.repo)
                .stdin(Stdio::null())
                .stdout(Stdio::null())
                .stderr(Stdio::null())
                .spawn()
                .map_err(|e| format!("start PulseAudio session: {e}"))?;
            drop(child);
            fs::write(&paths.server_file, format!("{}\n", paths.socket.display()))
                .map_err(|e| format!("write PulseAudio socket state: {e}"))?;
            fs::write(&paths.binary_file, format!("{}\n", daemon.display()))
                .map_err(|e| format!("write PulseAudio binary state: {e}"))?;
        }
        for i in 0..200 {
            if live(paths, &paths.socket) {
                break;
            }
            if i >= 19 && server_pid(paths).is_none() {
                break;
            }
            thread::sleep(Duration::from_millis(100));
        }
        if !live(paths, &paths.socket) {
            let tail = command_capture("tail", &["-3", &paths.log.to_string_lossy()]);
            if let Some(tail) = tail {
                eprintln!("{tail}");
            }
            return Err(format!(
                "PulseAudio did not start: no socket at {} (log {})",
                paths.socket.display(),
                paths.log.display()
            ));
        }
    }
    for socket in restore {
        let _ = fs::remove_file(&socket);
        let _ = pactl(
            paths,
            &[
                "load-module",
                "module-native-protocol-unix",
                &format!("socket={} auth-anonymous=1", socket.display()),
            ],
        );
    }
    let modules = pactl(paths, &["list", "modules", "short"])
        .ok()
        .map(|o| String::from_utf8_lossy(&o.stdout).contains("module-suspend-on-idle"))
        .unwrap_or(false);
    if !modules {
        let _ = pactl(
            paths,
            &["load-module", "module-suspend-on-idle", "timeout=3"],
        );
    }
    follow_default(paths);
    if let Some(volume) = requested_volume.filter(|v| !v.is_empty()) {
        set_volume(paths, volume);
    }
    Ok(())
}

fn follow_default(paths: &Paths) {
    if !paths.settings.is_file() {
        return;
    }
    let Some(wanted) = command_capture(
        paths.settings.to_str().unwrap_or_default(),
        &["--default-output"],
    )
    .filter(|s| !s.is_empty()) else {
        return;
    };
    let Ok(output) = pactl(paths, &["list", "sinks"]) else {
        return;
    };
    let text = String::from_utf8_lossy(&output.stdout);
    let mut name = String::new();
    let mut selected = None;
    for line in text.lines() {
        let trimmed = line.trim();
        if let Some(value) = trimmed
            .strip_prefix("Name:")
            .or_else(|| trimmed.strip_prefix("Nombre:"))
        {
            name = value.trim().to_owned();
        }
        if trimmed.contains(&format!("device.description = \"{wanted}\"")) && !name.is_empty() {
            selected = Some(name.clone());
            break;
        }
    }
    if let Some(sink) = selected {
        let _ = pactl(paths, &["set-default-sink", &sink]);
    }
}

fn set_volume(paths: &Paths, input: &str) {
    let volume = input.parse::<u32>().unwrap_or(100).min(100);
    let _ = pactl(
        paths,
        &["set-sink-volume", "@DEFAULT_SINK@", &format!("{volume}%")],
    );
}

fn stop(paths: &Paths) {
    if running(paths) {
        let _ = pactl(paths, &["exit"]);
    }
    let pattern = format!("pulseaudio.*{}", paths.conf.display());
    let _ = Command::new("pkill").args(["-f", &pattern]).status();
    for file in [&paths.socket, &paths.server_file, &paths.binary_file] {
        let _ = fs::remove_file(file);
    }
}

fn run() -> Result<(), String> {
    let paths = paths();
    let args = env::args().skip(1).collect::<Vec<_>>();
    match args.first().map(String::as_str).unwrap_or("status") {
        "start" => start(&paths, args.get(1).map(String::as_str)),
        "volume" => {
            if !running(&paths) {
                start(&paths, None)?;
            }
            set_volume(&paths, args.get(1).map(String::as_str).unwrap_or("100"));
            Ok(())
        }
        "stop" => {
            stop(&paths);
            Ok(())
        }
        "follow" => {
            if running(&paths) {
                follow_default(&paths);
            }
            Ok(())
        }
        "status" => {
            if let Some(socket) = server(&paths) {
                let default = pactl(&paths, &["get-default-sink"])
                    .ok()
                    .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_owned())
                    .unwrap_or_default();
                println!(
                    "audio: running ({}){}{}",
                    socket.display(),
                    if default.is_empty() {
                        ""
                    } else {
                        ", default sink "
                    },
                    default
                );
            } else {
                println!("audio: stopped");
            }
            Ok(())
        }
        _ => Err("usage: audio.sh start [volume%] | volume N | follow | stop | status".into()),
    }
}

fn main() {
    if let Err(message) = run() {
        eprintln!("audio: {message}");
        std::process::exit(1);
    }
}
