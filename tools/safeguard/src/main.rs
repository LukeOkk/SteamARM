use std::env;
use std::ffi::c_int;
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

const SIGTERM: c_int = 15;
const SIGKILL: c_int = 9;

unsafe extern "C" {
    fn kill(pid: c_int, signal: c_int) -> c_int;
}

#[derive(Clone, Copy)]
struct Limits {
    min_free: u64,
    low_secs: u64,
    crit_secs: u64,
    grace_secs: u64,
    fseventsd_mb: u64,
    guest_mb: u64,
    max_procs: u64,
    android_procs: u64,
    max_vm_objects: u64,
    max_map_entries: u64,
}

#[derive(Clone)]
struct Guest {
    pid: i32,
    rss_kb: u64,
}

fn project_root() -> PathBuf {
    env::var_os("STEAMARM_PROJECT_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../.."))
}

fn state_dir() -> PathBuf {
    if let Some(value) = env::var_os("STEAMARM_STATE").filter(|v| !v.is_empty()) {
        let value = PathBuf::from(value);
        if value == Path::new("~") {
            return home_dir();
        }
        if let Some(text) = value.to_str() {
            if let Some(rest) = text.strip_prefix("~/") {
                return home_dir().join(rest);
            }
        }
        return value;
    }
    let newer = home_dir().join("Developer/SteamARM-roots");
    let older = home_dir().join("SteamARM-roots");
    if newer.is_dir() || !older.is_dir() {
        newer
    } else {
        older
    }
}

fn home_dir() -> PathBuf {
    env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"))
}

fn state_file(name: &str) -> PathBuf {
    state_dir().join(name)
}

fn log_file() -> PathBuf {
    state_dir().join("logs/safeguard.log")
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

fn env_limit(name: &str, default: u64) -> u64 {
    env::var(name)
        .ok()
        .and_then(|v| v.parse().ok())
        .unwrap_or(default)
}

fn limits() -> Limits {
    Limits {
        min_free: env_limit("SAFEGUARD_MIN_FREE", 12),
        low_secs: env_limit("SAFEGUARD_LOW_SECS", 3),
        crit_secs: env_limit("SAFEGUARD_CRIT_SECS", 2),
        grace_secs: env_limit("SAFEGUARD_GRACE_SECS", 5),
        fseventsd_mb: env_limit("SAFEGUARD_FSEVENTS_MB", 1500),
        guest_mb: env_limit("SAFEGUARD_GUEST_MB", 8192),
        max_procs: env_limit("SAFEGUARD_MAX_PROCS", 80),
        android_procs: env_limit("SAFEGUARD_ANDROID_PROCS", 100),
        max_vm_objects: env_limit("SAFEGUARD_MAX_VMOBJ", 1_500_000),
        max_map_entries: env_limit("SAFEGUARD_MAX_MAPENT", 1_500_000),
    }
}

fn guest_name() -> String {
    env::var("SAFEGUARD_GUEST_NAME").unwrap_or_else(|_| "lxrun".into())
}

fn command_alive(pid: i32) -> bool {
    pid > 0 && unsafe { kill(pid, 0) } == 0
}

fn send_signal(pid: i32, signal: c_int) {
    if pid > 0 {
        unsafe {
            kill(pid, signal);
        }
    }
}

fn atomic_write(path: &Path, contents: &str) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let temp = path.with_extension(format!("tmp-{}", std::process::id()));
    fs::write(&temp, contents)?;
    fs::rename(temp, path)
}

fn session_binary() -> PathBuf {
    if let Some(path) = env::var_os("STEAMARM_SESSION_BIN") {
        return PathBuf::from(path);
    }
    let sibling = env::current_exe()
        .ok()
        .and_then(|p| p.parent().map(|dir| dir.join("steamarm-session")));
    if let Some(path) = sibling.filter(|p| p.is_file()) {
        return path;
    }
    project_root().join("build/steamarm-session")
}

fn start() -> i32 {
    let pid_file = state_file("safeguard.pid");
    if let Ok(raw) = fs::read_to_string(&pid_file) {
        if raw.trim().parse::<i32>().is_ok_and(command_alive) {
            println!("safeguard running (pid {})", raw.trim());
            return 0;
        }
    }
    let log = log_file();
    if let Some(parent) = log.parent() {
        if let Err(error) = fs::create_dir_all(parent) {
            eprintln!("cannot create {}: {error}", parent.display());
            return 1;
        }
    }
    let stdout = match OpenOptions::new().create(true).append(true).open(&log) {
        Ok(file) => file,
        Err(error) => {
            eprintln!("cannot open {}: {error}", log.display());
            return 1;
        }
    };
    let stderr = match stdout.try_clone() {
        Ok(file) => file,
        Err(error) => {
            eprintln!("cannot duplicate {}: {error}", log.display());
            return 1;
        }
    };
    let executable = match env::current_exe() {
        Ok(path) => path,
        Err(error) => {
            eprintln!("cannot find safeguard executable: {error}");
            return 1;
        }
    };
    let session = session_binary();
    let child = match Command::new(&session)
        .arg("detach")
        .arg(&executable)
        .arg("run")
        .env("PYTHONCOERCECLOCALE", "0")
        .env("STEAMARM_SESSION_PY", "1")
        .stdin(Stdio::null())
        .stdout(Stdio::from(stdout))
        .stderr(Stdio::from(stderr))
        .spawn()
    {
        Ok(child) => child,
        Err(error) => {
            eprintln!("cannot start {}: {error}", session.display());
            return 1;
        }
    };
    if let Err(error) = atomic_write(&pid_file, &format!("{}\n", child.id())) {
        eprintln!("cannot write {}: {error}", pid_file.display());
        send_signal(child.id() as i32, SIGTERM);
        return 1;
    }
    println!(
        "safeguard started (pid {}), log {}",
        child.id(),
        log.display()
    );
    0
}

fn stop() -> i32 {
    let pid_file = state_file("safeguard.pid");
    if let Ok(raw) = fs::read_to_string(&pid_file) {
        if let Ok(pid) = raw.trim().parse::<i32>() {
            send_signal(pid, SIGTERM);
        }
    }
    let _ = fs::remove_file(pid_file);
    println!("safeguard stopped");
    0
}

fn status() -> i32 {
    let pid_file = state_file("safeguard.pid");
    let running = fs::read_to_string(&pid_file)
        .ok()
        .and_then(|s| s.trim().parse::<i32>().ok())
        .is_some_and(command_alive);
    if running {
        if let Ok(pid) = fs::read_to_string(&pid_file) {
            println!("running (pid {})", pid.trim());
        }
    } else {
        println!("not running");
    }
    if let Ok(contents) = fs::read_to_string(log_file()) {
        for line in contents
            .lines()
            .rev()
            .take(3)
            .collect::<Vec<_>>()
            .into_iter()
            .rev()
        {
            println!("{line}");
        }
    }
    0
}

fn parse_processes() -> Vec<Guest> {
    let name = guest_name();
    let Some(output) = run_output("ps", &["-Ao", "pid=,rss=,comm="]) else {
        return Vec::new();
    };
    output
        .lines()
        .filter_map(|line| {
            let mut parts = line.split_whitespace();
            let pid = parts.next()?.parse::<i32>().ok()?;
            let rss_kb = parts.next()?.parse::<u64>().unwrap_or(0);
            let executable = parts.collect::<Vec<_>>().join(" ");
            let base = executable.rsplit('/').next().unwrap_or(&executable);
            (base == name || executable.ends_with(&format!("/{name}")))
                .then_some(Guest { pid, rss_kb })
        })
        .collect()
}

fn process_command(pid: i32) -> String {
    run_output("ps", &["-o", "command=", "-p", &pid.to_string()]).unwrap_or_default()
}

fn fseventsd_mb() -> u64 {
    run_output("ps", &["-Ao", "rss=,comm="])
        .and_then(|output| {
            output.lines().find_map(|line| {
                let mut parts = line.split_whitespace();
                let rss = parts.next()?.parse::<u64>().ok()?;
                let command = parts.collect::<Vec<_>>().join(" ");
                (command.rsplit('/').next().unwrap_or(&command) == "fseventsd")
                    .then_some(rss / 1024)
            })
        })
        .unwrap_or(0)
}

fn sysctl_number(name: &str, fallback: u64) -> u64 {
    run_output("sysctl", &["-n", name])
        .and_then(|s| s.parse().ok())
        .unwrap_or(fallback)
}

fn kernel_counters() -> (u64, u64) {
    let Some(output) = run_output("zprint", &[]) else {
        return (0, 0);
    };
    let mut objects = 0;
    let mut entries = 0;
    for line in output.lines() {
        let fields = line.split_whitespace().collect::<Vec<_>>();
        let Some(value) = fields.get(6).and_then(|v| v.parse::<u64>().ok()) else {
            continue;
        };
        match fields.first().copied() {
            Some("vm.objects") => objects = value,
            Some("VM.map.entries") => entries = value,
            _ => {}
        }
    }
    (objects, entries)
}

fn android_boot_pid() -> Option<i32> {
    let path = state_dir().join("android/session.json");
    let content = fs::read_to_string(path).ok()?;
    let start = content.find("\"bootPid\"")? + "\"bootPid\"".len();
    let rest = content[start..].trim_start();
    let rest = rest.strip_prefix(':')?.trim_start();
    let digits = rest
        .chars()
        .take_while(|c| c.is_ascii_digit())
        .collect::<String>();
    digits.parse().ok()
}

fn note_stop(reason: &str) {
    let epoch = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    let _ = fs::write(
        state_dir().join("logs/safeguard.last"),
        format!("{epoch} {reason}\n"),
    );
    let current = fs::read_to_string(state_dir().join("logs/current")).unwrap_or_default();
    let current = current.trim();
    if !current.is_empty() && Path::new(current).is_file() {
        if let Ok(mut file) = OpenOptions::new().append(true).open(current) {
            let stamp = run_output("date", &["+%F %T"]).unwrap_or_else(|| epoch.to_string());
            let _ = writeln!(file, "[safeguard] {stamp} {reason}");
            let _ = file.sync_data();
        }
    }
}

fn record(message: &str) {
    println!("{message}");
    let _ = io::stdout().flush();
    let _ = OpenOptions::new()
        .write(true)
        .open(log_file())
        .and_then(|file| file.sync_data());
}

fn kill_guests(reason: &str, guests: &[Guest]) {
    record(&format!("{} KILL: {reason}", timestamp()));
    note_stop(reason);
    for guest in guests {
        send_signal(guest.pid, SIGKILL);
    }
}

fn largest_guest(guests: &[Guest]) -> Option<&Guest> {
    guests.iter().max_by_key(|guest| guest.rss_kb)
}

fn timestamp() -> String {
    run_output("date", &["+%F %T"]).unwrap_or_else(|| {
        SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap_or_default()
            .as_secs()
            .to_string()
    })
}

fn relieve_memory(
    reason: &str,
    limits: Limits,
    guests: &[Guest],
    first_stop: &mut Option<Instant>,
) {
    if let Some(started) = *first_stop {
        if started.elapsed() >= Duration::from_secs(limits.grace_secs) {
            kill_guests(
                &format!("{reason} (still after stopping the largest guest)"),
                guests,
            );
            *first_stop = None;
        }
        return;
    }
    let Some(guest) = largest_guest(guests) else {
        return;
    };
    let rss_mb = guest.rss_kb / 1024;
    let command = process_command(guest.pid);
    record(&format!(
        "{} STOP largest guest: {reason}: pid {} {rss_mb} MB: {}",
        timestamp(),
        guest.pid,
        command.chars().take(160).collect::<String>()
    ));
    note_stop(&format!(
        "{reason}: stopped the largest guest (pid {}, {rss_mb} MB)",
        guest.pid
    ));
    send_signal(guest.pid, SIGKILL);
    *first_stop = Some(Instant::now());
}

fn has_x86_guest(guests: &[Guest]) -> bool {
    guests
        .iter()
        .any(|guest| process_command(guest.pid).contains("/FEX"))
}

fn fex_server_running() -> bool {
    run_output("ps", &["-Ao", "command="]).is_some_and(|output| {
        // The server's path moved with the build tree (it now runs from
        // SteamARM-build/out); match the program, not one install location.
        output.lines().any(|line| {
            let mut words = line.split_whitespace();
            words.next().is_some_and(|w| w.ends_with("lxrun"))
                && words.next().is_some_and(|w| w.ends_with("/FEXServer"))
        })
    })
}

/// Starts the FEXServer helper and returns its handle. The caller owns the
/// child and must reap it: a dropped `Child` is never waited for, and each
/// one left a zombie until the user's process table was full (2,400 of them
/// in three hours, after which nothing on the Mac could fork).
fn start_fex_server() -> Option<Child> {
    let script = project_root().join("scripts/run-fex.sh");
    Command::new(script)
        .arg("/bin/true")
        .current_dir(project_root())
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .spawn()
        .ok()
}

/// True while the previous FEXServer helper is still running; reaps it once
/// it has exited.
fn fex_restart_pending(helper: &mut Option<Child>) -> bool {
    match helper.as_mut().map(Child::try_wait) {
        Some(Ok(None)) => true,
        Some(_) => {
            *helper = None;
            false
        }
        None => false,
    }
}

fn effective_guest_limit(mut configured: u64) -> u64 {
    if env::var_os("SAFEGUARD_GUEST_MB").is_some() {
        return configured;
    }
    let path = state_dir().join("launcher/limits.env");
    if let Ok(contents) = fs::read_to_string(path) {
        for line in contents.lines() {
            if let Some(value) = line.strip_prefix("GUEST_MAX_MB=") {
                if let Ok(value) = value.trim().parse() {
                    configured = value;
                }
            }
        }
    }
    configured
}

fn effective_kernel_limit(name: &str, configured: u64, baseline: u64, headroom: u64) -> u64 {
    env::var(name)
        .ok()
        .and_then(|value| value.parse::<u64>().ok())
        .unwrap_or_else(|| configured.max(baseline.saturating_add(headroom)))
}

fn run() -> i32 {
    let limits = limits();
    let (vm_objects_baseline, map_entries_baseline) = kernel_counters();
    let max_vm_objects = effective_kernel_limit(
        "SAFEGUARD_MAX_VMOBJ",
        limits.max_vm_objects,
        vm_objects_baseline,
        500_000,
    );
    let max_map_entries = effective_kernel_limit(
        "SAFEGUARD_MAX_MAPENT",
        limits.max_map_entries,
        map_entries_baseline,
        1_000_000,
    );
    record(&format!(
        "kernel baseline vmobj={vm_objects_baseline} mapent={map_entries_baseline}; limits vmobj={max_vm_objects} mapent={max_map_entries}"
    ));
    let mut low = 0u64;
    let mut critical = 0u64;
    let mut first_stop = None;
    let mut warned = false;
    let mut cycles = 0u64;
    let mut fex_helper: Option<Child> = None;
    let mut fex_backoff = 0u64;
    let mut fex_retry_at = 0u64;
    let guest_name = guest_name();
    loop {
        let guests = parse_processes();
        let guest_count = guests.len() as u64;
        let guest_rss_mb = guests.iter().map(|guest| guest.rss_kb).sum::<u64>() / 1024;
        let mut max_processes = limits.max_procs;
        if android_boot_pid().is_some_and(command_alive) {
            max_processes += limits.android_procs;
        }
        let free = sysctl_number("kern.memorystatus_level", 100);
        let pressure = sysctl_number("kern.memorystatus_vm_pressure_level", 1);
        let fseventsd = fseventsd_mb();
        let (vm_objects, map_entries) = kernel_counters();
        let guest_mb_limit = effective_guest_limit(limits.guest_mb);

        if pressure >= 4 {
            critical += 1;
        } else {
            critical = 0;
        }
        if free < limits.min_free {
            low += 1;
        } else {
            low = 0;
        }
        if critical == 0 && low == 0 {
            first_stop = None;
        }
        if pressure >= 2 || free < limits.min_free + 8 {
            if !warned && guest_count > 0 {
                record(&format!(
                    "{} WARN: memory pressure level {pressure}, free {free}%, guests {guest_rss_mb} MB",
                    timestamp()
                ));
                warned = true;
            }
        } else {
            warned = false;
        }

        let stop_reason = if fseventsd > limits.fseventsd_mb {
            Some(format!(
                "fseventsd {fseventsd} MB > {} MB",
                limits.fseventsd_mb
            ))
        } else if guest_rss_mb > guest_mb_limit {
            Some(format!("guests {guest_rss_mb} MB > {guest_mb_limit} MB"))
        } else if guest_count > max_processes {
            Some(format!(
                "{guest_count} {guest_name} processes > {max_processes}"
            ))
        } else if vm_objects > max_vm_objects {
            Some(format!(
                "kernel vm.objects {vm_objects} > {}",
                max_vm_objects
            ))
        } else if map_entries > max_map_entries {
            Some(format!(
                "kernel VM.map.entries {map_entries} > {}",
                max_map_entries
            ))
        } else {
            None
        };
        if let Some(reason) = stop_reason {
            if guest_count > 0 {
                kill_guests(&reason, &guests);
            }
        } else if guest_count > 0 && critical >= limits.crit_secs {
            relieve_memory(
                &format!("critical memory pressure (free {free}%)"),
                limits,
                &guests,
                &mut first_stop,
            );
        } else if guest_count > 0 && low >= limits.low_secs {
            relieve_memory(
                &format!("free memory {free}% < {}%", limits.min_free),
                limits,
                &guests,
                &mut first_stop,
            );
        }

        cycles += 1;
        let helper_running = fex_restart_pending(&mut fex_helper);
        if cycles % 5 == 0
            && !helper_running
            && cycles >= fex_retry_at
            && guest_count > 0
            && !fex_server_running()
            && has_x86_guest(&guests)
        {
            record(&format!(
                "{} FEXServer missing with {guest_count} guests: restarting it",
                timestamp()
            ));
            fex_helper = start_fex_server();
            // Back off while restarts do not bring it back: 5 s, 10 s, ... 5 min.
            fex_backoff = (fex_backoff * 2).clamp(5, 300);
            fex_retry_at = cycles + fex_backoff;
        } else if cycles % 5 == 0 && !helper_running && (guest_count == 0 || fex_server_running()) {
            fex_backoff = 0;
            fex_retry_at = 0;
        }
        if cycles % 5 == 0 {
            record(&format!(
                "{} free={free}% pressure={pressure} guests={guest_count} rss={guest_rss_mb}MB fseventsd={fseventsd}MB vmobj={vm_objects} mapent={map_entries}",
                timestamp()
            ));
        }
        thread::sleep(Duration::from_secs(1));
    }
}

fn usage() -> i32 {
    eprintln!("Usage: steamarm-safeguard start | stop | status | run");
    2
}

fn main() {
    let code = match env::args().nth(1).as_deref() {
        Some("start") => start(),
        Some("stop") => stop(),
        Some("status") => status(),
        Some("run") => run(),
        _ => usage(),
    };
    std::process::exit(code);
}
