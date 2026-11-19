use std::ffi::{CString, OsStr, OsString};
use std::io::{self, Write};
use std::os::raw::{c_char, c_int, c_uint};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::process::{CommandExt, ExitStatusExt};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, ExitStatus, Stdio};
use std::sync::atomic::{AtomicBool, AtomicI32, Ordering};
use std::thread;
use std::time::{Duration, Instant};

const SIGHUP: c_int = 1;
const SIGINT: c_int = 2;
const SIGPIPE: c_int = 13;
const SIGKILL: c_int = 9;
const SIGTERM: c_int = 15;
const SIGXFSZ: c_int = 25;
const SIG_DFL: usize = 0;
const SIG_IGN: usize = 1;
const SIG_ERR: usize = usize::MAX;

const GRACE: f64 = 5.0;
const KILL_AFTER: f64 = 2.0;
const MAX_RESTARTS: usize = 5;
const RESTART_WINDOW: f64 = 600.0;

static STOPPING: AtomicBool = AtomicBool::new(false);
static FORWARD_PGID: AtomicI32 = AtomicI32::new(0);

extern "C" {
    fn setsid() -> c_int;
    fn getsid(pid: c_int) -> c_int;
    fn getpgrp() -> c_int;
    fn getuid() -> c_uint;
    fn kill(pid: c_int, sig: c_int) -> c_int;
    fn signal(sig: c_int, handler: usize) -> usize;
    fn execvp(file: *const c_char, argv: *const *const c_char) -> c_int;
}

fn log(message: impl AsRef<str>) {
    let _ = writeln!(io::stderr().lock(), "[session] {}", message.as_ref());
}

extern "C" fn on_stop(_: c_int) {
    STOPPING.store(true, Ordering::Relaxed);
}

extern "C" fn forward_signal(sig: c_int) {
    let pgid = FORWARD_PGID.load(Ordering::Relaxed);
    if pgid > 0 {
        unsafe {
            kill(-pgid, sig);
        }
    }
}

fn set_signal(sig: c_int, handler: usize) -> io::Result<usize> {
    let previous = unsafe { signal(sig, handler) };
    if previous == SIG_ERR {
        Err(io::Error::last_os_error())
    } else {
        Ok(previous)
    }
}

fn signal_is_ignored(sig: c_int) -> bool {
    match set_signal(sig, SIG_IGN) {
        Ok(previous) => {
            let _ = set_signal(sig, previous);
            previous == SIG_IGN
        }
        Err(_) => false,
    }
}

fn install_stop_handlers() {
    STOPPING.store(false, Ordering::Relaxed);
    for sig in [SIGTERM, SIGINT, SIGHUP] {
        if let Err(error) = set_signal(sig, on_stop as *const () as usize) {
            log(format!("cannot handle signal {sig}: {error}"));
        }
    }
}

fn install_forward_handlers(pgid: i32) {
    FORWARD_PGID.store(pgid, Ordering::Relaxed);
    for sig in [SIGTERM, SIGINT, SIGHUP] {
        if let Err(error) = set_signal(sig, forward_signal as *const () as usize) {
            log(format!("cannot forward signal {sig}: {error}"));
        }
    }
}

fn remove_legacy_python_env() {
    if std::env::var_os("STEAMARM_SESSION_PY").is_some() {
        std::env::remove_var("STEAMARM_SESSION_PY");
        std::env::remove_var("PYTHONCOERCECLOCALE");
    }
}

fn atomic_write(path: &Path, contents: &str) -> io::Result<()> {
    let name = path
        .file_name()
        .unwrap_or_else(|| OsStr::new("status"))
        .to_string_lossy();
    let temporary = path.with_file_name(format!("{name}.new"));
    std::fs::write(&temporary, contents)?;
    std::fs::rename(temporary, path)
}

fn shell_code(status: &ExitStatus) -> i32 {
    status
        .code()
        .or_else(|| status.signal().map(|sig| 128 + sig))
        .unwrap_or(1)
}

fn status_line(status: &ExitStatus) -> String {
    match status.signal() {
        Some(sig) => format!("{} signal {sig}", 128 + sig),
        None => status.code().unwrap_or(1).to_string(),
    }
}

fn spawn_program(argv: &[OsString], hup_ignored: bool) -> io::Result<Child> {
    let mut command = Command::new(&argv[0]);
    command.args(&argv[1..]).stdin(Stdio::null());
    unsafe {
        command.pre_exec(move || {
            for sig in [SIGTERM, SIGINT, SIGHUP, SIGPIPE, SIGXFSZ] {
                let handler = if sig == SIGHUP && hup_ignored {
                    SIG_IGN
                } else {
                    SIG_DFL
                };
                if signal(sig, handler) == SIG_ERR {
                    return Err(io::Error::last_os_error());
                }
            }
            Ok(())
        });
    }
    command.spawn()
}

fn child_session(executable: &Path, internal: &str, arguments: &[OsString]) -> io::Result<Child> {
    let mut command = Command::new(executable);
    command.arg(internal).args(arguments);
    unsafe {
        command.pre_exec(|| {
            if setsid() < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        });
    }
    command.spawn()
}

fn wait_child(mut child: Child) -> i32 {
    let pgid = child.id() as i32;
    install_forward_handlers(pgid);
    match child.wait() {
        Ok(status) => shell_code(&status),
        Err(error) => {
            log(format!("cannot wait for session process: {error}"));
            1
        }
    }
}

fn members(pgid: i32) -> Option<Vec<i32>> {
    for _ in 0..3 {
        let ps = Command::new("ps")
            .args(["-Ao", "pid=,pgid=,stat="])
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .ok()?;
        let ps_pid = ps.id() as i32;
        let output = ps.wait_with_output().ok()?;
        if !output.status.success() || output.stdout.is_empty() {
            thread::sleep(Duration::from_millis(100));
            continue;
        }
        let text = String::from_utf8_lossy(&output.stdout);
        let mut pids = Vec::new();
        for line in text.lines() {
            let mut fields = line.split_whitespace();
            let (Some(pid), Some(group), Some(state)) =
                (fields.next(), fields.next(), fields.next())
            else {
                continue;
            };
            if group.parse::<i32>().ok() != Some(pgid) || state.starts_with('Z') {
                continue;
            }
            if let Ok(pid) = pid.parse::<i32>() {
                if pid != pgid && pid != ps_pid {
                    pids.push(pid);
                }
            }
        }
        return Some(pids);
    }
    None
}

fn command_of(pid: i32) -> String {
    Command::new("ps")
        .args(["-o", "command=", "-p", &pid.to_string()])
        .output()
        .ok()
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
        .unwrap_or_default()
}

fn process_output(args: &[&str]) -> String {
    Command::new("ps")
        .args(args)
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()
        .ok()
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
        .unwrap_or_default()
}

fn matching_pids(pattern: &str) -> Vec<i32> {
    let output = Command::new("pgrep")
        .args(["-f", pattern])
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output();
    output
        .ok()
        .map(|output| {
            String::from_utf8_lossy(&output.stdout)
                .lines()
                .filter_map(|line| line.trim().parse::<i32>().ok())
                .collect()
        })
        .unwrap_or_default()
}

fn process_has_env(pid: i32, expected: &str) -> bool {
    process_output(&["-E", "-o", "command=", "-p", &pid.to_string()])
        .split_whitespace()
        .any(|item| item == expected)
}

fn guest_processes() -> Vec<i32> {
    matching_pids("build/lxrun")
        .into_iter()
        .filter(|pid| {
            let command = command_of(*pid);
            !command.contains("Xvnc")
                && !command.contains("FEXServer")
                && !process_has_env(*pid, "LXRT_SESSION=android")
        })
        .collect()
}

fn print_pids(pids: &[i32]) {
    for pid in pids {
        println!("{pid}");
    }
}

fn outside_client(id: &str) -> Option<i32> {
    match id {
        "steam" => matching_pids("build/lxrun .*ubuntu12_32/steam ")
            .into_iter()
            .next(),
        "steam-arm64" | "steam-arm64-frame" => {
            let root = if id == "steam-arm64-frame" {
                "/tmp/lxrt-arm64root"
            } else {
                "/tmp/lxrt-armroot"
            };
            matching_pids("build/lxrun .*steamrtarm64/steam ")
                .into_iter()
                .find(|pid| process_has_env(*pid, &format!("LXRT_ROOT={root}")))
        }
        _ => None,
    }
}

fn first_process(pattern: &str) -> Option<i32> {
    matching_pids(pattern).into_iter().next()
}

fn xvnc_geometry(pid: i32) -> String {
    let args = command_of(pid);
    let fields = args.split_whitespace().collect::<Vec<_>>();
    fields
        .windows(2)
        .find(|pair| pair[0] == "-geometry")
        .map(|pair| pair[1].to_owned())
        .unwrap_or_default()
}

fn display_mode(pid: i32) -> Option<&'static str> {
    let command = process_output(&["-E", "-o", "command=", "-p", &pid.to_string()]);
    if command.split_whitespace().any(|item| item == "DISPLAY=:1") {
        Some("vnc")
    } else if command.split_whitespace().any(|item| item == "DISPLAY=:2") {
        Some("native")
    } else {
        None
    }
}

fn parse_pid(args: &[OsString]) -> Option<i32> {
    args.first()
        .and_then(|value| value.to_str())
        .and_then(|value| value.parse::<i32>().ok())
        .filter(|value| *value > 0)
}

fn process_signal(pid: i32, signal: c_int) -> bool {
    unsafe { kill(pid, signal) == 0 }
}

fn kill_guest_processes() {
    for pid in guest_processes() {
        unsafe {
            kill(pid, SIGKILL);
        }
    }
}

fn clear_stale_shm() {
    if !guest_processes().is_empty() {
        return;
    }
    let uid = unsafe { getuid() };
    let directory = PathBuf::from(format!("/tmp/lxrt-shm-{uid}"));
    let prefix = format!("u{uid}-Shm_");
    let Ok(entries) = std::fs::read_dir(directory) else {
        return;
    };
    for entry in entries.flatten() {
        if entry
            .file_name()
            .to_str()
            .is_some_and(|name| name.starts_with(&prefix))
        {
            let _ = std::fs::remove_file(entry.path());
        }
    }
}

fn session_guest_processes(session: i32) -> Vec<i32> {
    guest_processes()
        .into_iter()
        .filter(|pid| unsafe { getsid(*pid) } == session)
        .collect()
}

fn reap_session(session: i32) {
    if session <= 1 || alive(session) {
        return;
    }
    let mut pids = session_guest_processes(session);
    if pids.is_empty() {
        return;
    }
    log(format!(
        "stopping guest processes left by session {session}: {pids:?}"
    ));
    for pid in &pids {
        unsafe {
            kill(*pid, SIGTERM);
        }
    }
    for _ in 0..10 {
        thread::sleep(Duration::from_millis(200));
        pids = session_guest_processes(session);
        if pids.is_empty() {
            return;
        }
    }
    for pid in pids {
        unsafe {
            kill(pid, SIGKILL);
        }
    }
}

fn reap_command(args: &[OsString]) -> i32 {
    let Some(session) = args
        .first()
        .and_then(|value| value.to_str())
        .and_then(|value| value.parse::<i32>().ok())
    else {
        return 0;
    };
    reap_session(session);
    0
}

fn reap_after_command(args: &[OsString]) -> i32 {
    let Some(session) = args
        .first()
        .and_then(|value| value.to_str())
        .and_then(|value| value.parse::<i32>().ok())
        .filter(|value| *value > 1)
    else {
        return invalid_usage();
    };
    while alive(session) {
        thread::sleep(Duration::from_secs(1));
    }
    reap_session(session);
    0
}

fn alive(pid: i32) -> bool {
    if pid <= 0 {
        return false;
    }
    if unsafe { kill(pid, 0) } == 0 {
        return true;
    }
    io::Error::last_os_error().raw_os_error() == Some(1) // EPERM
}

fn wait_empty(pgid: i32, seconds: f64) -> bool {
    let deadline = Instant::now() + Duration::from_secs_f64(seconds.max(0.0));
    loop {
        if matches!(members(pgid), Some(pids) if pids.is_empty()) {
            return true;
        }
        if Instant::now() >= deadline {
            return false;
        }
        thread::sleep(Duration::from_millis(100));
    }
}

fn kill_members(pgid: i32, sig: c_int, leader_too: bool) {
    let Some(pids) = members(pgid) else {
        if sig == SIGKILL && !leader_too {
            log("cannot list the group; SIGKILL not sent");
            return;
        }
        unsafe {
            kill(-pgid, sig);
        }
        return;
    };
    for pid in pids {
        unsafe {
            kill(pid, sig);
        }
    }
}

fn clean_group(pgid: i32) {
    if !wait_empty(pgid, GRACE) {
        log("group still has process(es) left: SIGTERM");
        kill_members(pgid, SIGTERM, false);
        if !wait_empty(pgid, KILL_AFTER) {
            log("SIGKILL to what is left");
            kill_members(pgid, SIGKILL, false);
            wait_empty(pgid, 1.0);
        }
    }
}

fn restart_code() -> Option<i32> {
    let raw = std::env::var("STEAMARM_SESSION_RESTART").ok()?;
    std::env::remove_var("STEAMARM_SESSION_RESTART");
    if raw.is_empty() || !raw.bytes().all(|byte| byte.is_ascii_digit()) {
        return None;
    }
    raw.parse().ok()
}

fn run_supervisor(ldir: &Path, argv: &[OsString], hup_override: Option<bool>) -> i32 {
    if argv.is_empty() {
        log("run: no command");
        return 2;
    }
    let restart = restart_code();
    let hup_ignored = hup_override.unwrap_or_else(|| signal_is_ignored(SIGHUP));
    let pgid = unsafe { getpgrp() };
    if let Err(error) = std::fs::create_dir_all(ldir) {
        log(format!("cannot create {}: {error}", ldir.display()));
        return 1;
    }
    let pgid_path = ldir.join("running.pgid");
    if let Err(error) = atomic_write(&pgid_path, &format!("{pgid}\n")) {
        log(format!("cannot write {}: {error}", pgid_path.display()));
        return 1;
    }

    install_stop_handlers();
    let name = Path::new(&argv[0])
        .file_name()
        .unwrap_or(&argv[0])
        .to_string_lossy()
        .into_owned();
    log(format!(
        "process group {pgid}: {}",
        argv.iter()
            .map(|a| a.to_string_lossy())
            .collect::<Vec<_>>()
            .join(" ")
    ));

    let mut restarts: Vec<Instant> = Vec::new();
    let status = loop {
        let mut child = match spawn_program(argv, hup_ignored) {
            Ok(child) => child,
            Err(error) => {
                log(format!(
                    "cannot start {}: {error}",
                    argv[0].to_string_lossy()
                ));
                let status_path = ldir.join("running.status");
                if let Err(write_error) = atomic_write(&status_path, "127\n") {
                    log(format!(
                        "cannot write {}: {write_error}",
                        status_path.display()
                    ));
                }
                return 127;
            }
        };
        let status = match child.wait() {
            Ok(status) => status,
            Err(error) => {
                log(format!(
                    "cannot wait for {}: {error}",
                    argv[0].to_string_lossy()
                ));
                return 1;
            }
        };
        if restart != status.code() || STOPPING.load(Ordering::Relaxed) {
            break status;
        }
        let now = Instant::now();
        restarts.retain(|time| now.duration_since(*time).as_secs_f64() < RESTART_WINDOW);
        if restarts.len() >= MAX_RESTARTS {
            log(format!(
                "{name} asked to be started again {} times in {} s: not again",
                restarts.len() + 1,
                RESTART_WINDOW as u64
            ));
            break status;
        }
        restarts.push(now);
        log(format!(
            "{name} ended with status {}: started again, as it asked",
            shell_code(&status)
        ));
        clean_group(pgid);
        if STOPPING.load(Ordering::Relaxed) {
            break status;
        }
    };

    let line = status_line(&status);
    let status_path = ldir.join("running.status");
    if let Err(error) = atomic_write(&status_path, &format!("{line}\n")) {
        log(format!("cannot write {}: {error}", status_path.display()));
    }
    log(format!("{name} ended with status {line}"));
    clean_group(pgid);
    shell_code(&status)
}

fn run(ldir: PathBuf, argv: &[OsString]) -> i32 {
    if argv.is_empty() {
        log("run: no command");
        return 2;
    }
    let hup_ignored = signal_is_ignored(SIGHUP);
    if unsafe { setsid() } < 0 {
        let error = io::Error::last_os_error();
        if error.raw_os_error() != Some(1) {
            log(format!("cannot create session: {error}"));
            return 1;
        }
        let mut arguments = vec![
            OsString::from(if hup_ignored { "1" } else { "0" }),
            ldir.into_os_string(),
        ];
        arguments.extend_from_slice(argv);
        let executable = match std::env::current_exe() {
            Ok(path) => path,
            Err(error) => {
                log(format!("cannot find session executable: {error}"));
                return 1;
            }
        };
        return match child_session(&executable, "__run-isolated", &arguments) {
            Ok(child) => wait_child(child),
            Err(error) => {
                log(format!("cannot start isolated session: {error}"));
                1
            }
        };
    }
    run_supervisor(&ldir, argv, Some(hup_ignored))
}

fn stop(ldir: &Path, grace: f64) -> i32 {
    let path = ldir.join("running.pgid");
    let Ok(raw) = std::fs::read_to_string(path) else {
        return 0;
    };
    let Ok(pgid) = raw.trim().parse::<i32>() else {
        return 0;
    };
    if !alive(pgid) {
        return 0;
    }
    let command = command_of(pgid);
    let is_session = command.contains("steamarm-session")
        && (command.contains(" run ") || command.contains("__run-isolated"));
    if !is_session {
        return 0;
    }
    if unsafe { kill(-pgid, SIGTERM) } < 0 {
        return 0;
    }
    if !wait_empty(pgid, grace) {
        kill_members(pgid, SIGKILL, true);
        wait_empty(pgid, 1.0);
    }
    let deadline = Instant::now() + Duration::from_secs_f64(KILL_AFTER);
    while alive(pgid) && Instant::now() < deadline {
        thread::sleep(Duration::from_millis(100));
    }
    if alive(pgid) {
        unsafe {
            kill(pgid, SIGKILL);
        }
    }
    0
}

fn exec_program(argv: &[OsString], hup_ignored: bool) -> ! {
    if argv.is_empty() {
        log("detach: no command");
        std::process::exit(2);
    }
    for (sig, handler) in [
        (SIGPIPE, SIG_DFL),
        (SIGXFSZ, SIG_DFL),
        (SIGHUP, if hup_ignored { SIG_IGN } else { SIG_DFL }),
    ] {
        if let Err(error) = set_signal(sig, handler) {
            log(format!("cannot reset signal {sig}: {error}"));
            std::process::exit(127);
        }
    }
    let strings: Result<Vec<CString>, _> = argv
        .iter()
        .map(|argument| CString::new(argument.as_os_str().as_bytes()))
        .collect();
    let strings = match strings {
        Ok(strings) => strings,
        Err(_) => {
            log("detach: argument contains a NUL byte");
            std::process::exit(127);
        }
    };
    let pointers: Vec<*const c_char> = strings
        .iter()
        .map(|argument| argument.as_ptr())
        .chain(std::iter::once(std::ptr::null()))
        .collect();
    unsafe {
        execvp(strings[0].as_ptr(), pointers.as_ptr());
    }
    log(format!(
        "cannot start {}: {}",
        argv[0].to_string_lossy(),
        io::Error::last_os_error()
    ));
    std::process::exit(127)
}

fn detach(argv: &[OsString]) -> i32 {
    if argv.is_empty() {
        log("detach: no command");
        return 2;
    }
    let hup_ignored = signal_is_ignored(SIGHUP);
    if unsafe { setsid() } < 0 {
        let error = io::Error::last_os_error();
        if error.raw_os_error() != Some(1) {
            log(format!("cannot create session: {error}"));
            return 1;
        }
        let mut arguments = vec![OsString::from(if hup_ignored { "1" } else { "0" })];
        arguments.extend_from_slice(argv);
        let executable = match std::env::current_exe() {
            Ok(path) => path,
            Err(error) => {
                log(format!("cannot find session executable: {error}"));
                return 1;
            }
        };
        return match child_session(&executable, "__detach-isolated", &arguments) {
            Ok(child) => wait_child(child),
            Err(error) => {
                log(format!("cannot start isolated detach: {error}"));
                1
            }
        };
    }
    exec_program(argv, hup_ignored)
}

fn invalid_usage() -> i32 {
    let _ = writeln!(
        io::stderr().lock(),
        "Usage: steamarm-session run <launcher-dir> <command> [args...]\n       steamarm-session stop <launcher-dir> [grace-seconds]\n       steamarm-session detach <command> [args...]\n       steamarm-session members <session-id> <pid...>\n       steamarm-session guest-pids\n       steamarm-session outside-client <app-id>\n       steamarm-session has-process <pgrep-pattern>\n       steamarm-session first-process <pgrep-pattern>\n       steamarm-session xvnc-geometry <pid>\n       steamarm-session display-mode <pid>\n       steamarm-session alive <pid>\n       steamarm-session signal <pid> TERM|KILL\n       steamarm-session reap [session-id]\n       steamarm-session reap-after <session-id>"
    );
    2
}

fn session_members_command(args: &[OsString]) -> i32 {
    let Some(session) = args
        .first()
        .and_then(|value| value.to_str())
        .and_then(|value| value.parse::<c_int>().ok())
        .filter(|value| *value > 1)
    else {
        return invalid_usage();
    };
    for pid in args.iter().skip(1) {
        let Some(pid) = pid
            .to_str()
            .and_then(|value| value.parse::<c_int>().ok())
            .filter(|value| *value > 1)
        else {
            continue;
        };
        if unsafe { getsid(pid) } == session {
            println!("{pid}");
        }
    }
    0
}

fn main() {
    remove_legacy_python_env();
    let args: Vec<OsString> = std::env::args_os().skip(1).collect();
    let code = match args.first().and_then(|arg| arg.to_str()) {
        Some("run") if args.len() >= 2 => run(PathBuf::from(&args[1]), &args[2..]),
        Some("stop") if args.len() >= 2 => {
            let grace = if args.len() >= 3 {
                match args[2].to_string_lossy().parse::<f64>() {
                    Ok(value) => value.max(0.0),
                    Err(error) => {
                        log(format!("stop: invalid grace interval: {error}"));
                        std::process::exit(2);
                    }
                }
            } else {
                3.0
            };
            stop(Path::new(&args[1]), grace)
        }
        Some("detach") if args.len() >= 2 => detach(&args[1..]),
        Some("members") if args.len() >= 2 => session_members_command(&args[1..]),
        Some("guest-pids") => {
            print_pids(&guest_processes());
            0
        }
        Some("guest-pids-line") => {
            let line = guest_processes()
                .iter()
                .map(i32::to_string)
                .collect::<Vec<_>>()
                .join(" ");
            println!("{line}");
            0
        }
        Some("outside-client") if args.len() >= 2 => {
            if let Some(id) = args[1].to_str() {
                if let Some(pid) = outside_client(id) {
                    println!("{pid}");
                }
                0
            } else {
                invalid_usage()
            }
        }
        Some("has-process") if args.len() >= 2 => {
            if first_process(&args[1].to_string_lossy()).is_some() {
                0
            } else {
                1
            }
        }
        Some("first-process") if args.len() >= 2 => {
            if let Some(pid) = first_process(&args[1].to_string_lossy()) {
                println!("{pid}");
            }
            0
        }
        Some("xvnc-geometry") if args.len() >= 2 => {
            if let Some(pid) = parse_pid(&args[1..]) {
                println!("{}", xvnc_geometry(pid));
                0
            } else {
                invalid_usage()
            }
        }
        Some("display-mode") if args.len() >= 2 => {
            if let Some(pid) = parse_pid(&args[1..]) {
                if let Some(mode) = display_mode(pid) {
                    println!("{mode}");
                }
                0
            } else {
                invalid_usage()
            }
        }
        Some("alive") if args.len() >= 2 => {
            if let Some(pid) = parse_pid(&args[1..]) {
                i32::from(!alive(pid))
            } else {
                invalid_usage()
            }
        }
        Some("signal") if args.len() >= 3 => {
            let Some(pid) = parse_pid(&args[1..]) else {
                std::process::exit(invalid_usage());
            };
            let signal = match args[2].to_str() {
                Some("TERM") => SIGTERM,
                Some("KILL") => SIGKILL,
                _ => std::process::exit(invalid_usage()),
            };
            i32::from(!process_signal(pid, signal))
        }
        Some("kill-guests") => {
            kill_guest_processes();
            0
        }
        Some("clear-stale-shm") => {
            clear_stale_shm();
            0
        }
        Some("reap") => reap_command(&args[1..]),
        Some("reap-after") => reap_after_command(&args[1..]),
        Some("__run-isolated") if args.len() >= 3 => {
            let hup = args[1] == OsStr::new("1");
            run_supervisor(Path::new(&args[2]), &args[3..], Some(hup))
        }
        Some("__detach-isolated") if args.len() >= 2 => {
            let hup = args[1] == OsStr::new("1");
            exec_program(&args[2..], hup)
        }
        _ => invalid_usage(),
    };
    std::process::exit(code);
}
