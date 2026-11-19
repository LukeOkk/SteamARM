use std::collections::{HashMap, HashSet};
use std::env;
use std::ffi::{c_int, c_uint, c_void, OsStr, OsString};
use std::fs::{self, File};
use std::io;
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::{symlink, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::{Child, Command, Stdio};
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

#[path = "../../common/safeguard.rs"]
mod safeguard;

const SIGINT: c_int = 2;
const SIGTERM: c_int = 15;
const SIGKILL: c_int = 9;
const SIGINTERRUPTED: i32 = 130;
const DEFAULT_LINK: &str = "/tmp/lxrt-arm64root";
const DEFAULT_HOME: &str = "/tmp/armhome";
const STEAM_CLIENT: &str = "/tmp/armhome/.local/share/Steam/steamrtarm64/steam";
const GUEST_ENV_PRESET: &[&[u8]] = &[
    b"DISPLAY",
    b"TMPDIR",
    b"PATH",
    b"OBJC_DISABLE_INITIALIZE_FORK_SAFETY",
    b"LXRT_ROOT",
    b"LXRT_GUEST_PAGE",
    b"HOME",
    b"LXRT_X18_ALL_TEXT",
    b"SDL_HIDAPI_LIBUSB",
    b"STEAMARM_RUN_ID",
    b"STEAMARM_HOST_PAGE_SIZE",
];
const ENV_UNSET_BEFORE_GUEST: &[&[u8]] = &[b"LD_LIBRARY_PATH", b"FEX_ROOTFS", b"FEX_GUESTBASE"];

static INTERRUPTED: AtomicBool = AtomicBool::new(false);

unsafe extern "C" {
    fn kill(pid: c_int, signal: c_int) -> c_int;
    fn signal(signal: c_int, handler: usize) -> usize;
    #[cfg(target_os = "macos")]
    fn sysctl(
        name: *mut c_int,
        name_len: c_uint,
        old_value: *mut c_void,
        old_len: *mut usize,
        new_value: *mut c_void,
        new_len: usize,
    ) -> c_int;
}

extern "C" fn on_signal(_: c_int) {
    INTERRUPTED.store(true, Ordering::Relaxed);
}

fn install_signal_handlers() {
    unsafe {
        signal(SIGINT, on_signal as *const () as usize);
        signal(SIGTERM, on_signal as *const () as usize);
    }
}

fn project_root() -> PathBuf {
    env::current_dir().unwrap_or_else(|_| PathBuf::from("."))
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
    let home = home_dir();
    let newer = home.join("Developer/SteamARM-roots");
    let older = home.join("SteamARM-roots");
    if newer.is_dir() || !older.is_dir() {
        newer
    } else {
        older
    }
}

fn absolute_from(path: &Path, base: &Path) -> PathBuf {
    if path.is_absolute() {
        path.to_path_buf()
    } else {
        base.join(path)
    }
}

// Mirrors roots.sh's canon_path: resolve the existing directory prefix and
// append a not-yet-created tail; reject dangling/non-directory symlinks.
fn canonical_path(path: &Path, base: &Path) -> Result<PathBuf, String> {
    if path.as_os_str().is_empty() {
        return Err("empty path".into());
    }
    let mut current = absolute_from(path, base);
    let mut tail = Vec::<OsString>::new();
    loop {
        if current.is_dir() {
            let mut result = fs::canonicalize(&current)
                .map_err(|error| format!("cannot resolve {}: {error}", current.display()))?;
            for part in tail.iter().rev() {
                result.push(part);
            }
            return Ok(result);
        }
        match fs::symlink_metadata(&current) {
            Ok(metadata) if metadata.file_type().is_symlink() => {
                return Err(format!(
                    "{} is a dangling or non-directory symlink",
                    current.display()
                ))
            }
            Err(error) if error.kind() != io::ErrorKind::NotFound => {
                return Err(format!("cannot inspect {}: {error}", current.display()))
            }
            _ => {}
        }
        let name = current
            .file_name()
            .ok_or_else(|| format!("cannot resolve {}", current.display()))?;
        if name == OsStr::new(".") || name == OsStr::new("..") {
            return Err(format!("cannot resolve {}", current.display()));
        }
        tail.push(name.to_os_string());
        current = current
            .parent()
            .filter(|parent| !parent.as_os_str().is_empty())
            .unwrap_or_else(|| Path::new("/"))
            .to_path_buf();
    }
}

fn valid_env_key(key: &[u8]) -> bool {
    let Some((&first, rest)) = key.split_first() else {
        return false;
    };
    (first == b'_' || first.is_ascii_alphabetic())
        && rest
            .iter()
            .all(|byte| *byte == b'_' || byte.is_ascii_alphanumeric())
}

fn guest_environment(root: &Path) -> Vec<(OsString, OsString)> {
    let Ok(contents) = fs::read(root.join(".lxrt-guest-env")) else {
        return Vec::new();
    };
    let mut seen = HashSet::<Vec<u8>>::new();
    contents
        .split(|byte| *byte == b'\n')
        .filter_map(|line| {
            let equals = line.iter().position(|byte| *byte == b'=')?;
            let key = &line[..equals];
            let value = &line[equals + 1..];
            if !valid_env_key(key)
                || GUEST_ENV_PRESET.contains(&key)
                || (env::var_os(OsStr::from_bytes(key)).is_some()
                    && !ENV_UNSET_BEFORE_GUEST.contains(&key))
                || value.contains(&0)
                || !seen.insert(key.to_vec())
            {
                return None;
            }
            Some((
                OsString::from_vec(key.to_vec()),
                OsString::from_vec(value.to_vec()),
            ))
        })
        .collect()
}

#[derive(Clone, Debug)]
struct Process {
    pid: i32,
    ppid: i32,
    elapsed: String,
    command: String,
}

fn process_rows(args: &[&str]) -> Vec<String> {
    Command::new("/bin/ps")
        .args(args)
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| {
            String::from_utf8_lossy(&output.stdout)
                .lines()
                .map(ToOwned::to_owned)
                .collect()
        })
        .unwrap_or_default()
}

fn take_word<'a>(input: &mut &'a str) -> Option<&'a str> {
    *input = input.trim_start();
    if input.is_empty() {
        return None;
    }
    match input.find(char::is_whitespace) {
        Some(end) => {
            let word = &input[..end];
            *input = &input[end..];
            Some(word)
        }
        None => {
            let word = *input;
            *input = "";
            Some(word)
        }
    }
}

fn parse_process_rows(args: &[&str], with_elapsed: bool) -> Vec<Process> {
    process_rows(args)
        .into_iter()
        .filter_map(|line| {
            let mut rest = line.as_str();
            let pid = take_word(&mut rest)?.parse().ok()?;
            let ppid = take_word(&mut rest)?.parse().ok()?;
            let elapsed = if with_elapsed {
                take_word(&mut rest)?.to_owned()
            } else {
                String::new()
            };
            let command = rest.trim().to_owned();
            Some(Process {
                pid,
                ppid,
                elapsed,
                command,
            })
        })
        .collect()
}

fn env_of_pid(pid: i32) -> Vec<Vec<u8>> {
    #[cfg(target_os = "linux")]
    if let Ok(contents) = fs::read(format!("/proc/{pid}/environ")) {
        return contents
            .split(|byte| *byte == 0)
            .map(ToOwned::to_owned)
            .collect();
    }

    #[cfg(target_os = "macos")]
    {
        let mut mib = [1_i32, 49_i32, pid]; // CTL_KERN, KERN_PROCARGS2, pid
        let mut size = 0_usize;
        let first = unsafe {
            sysctl(
                mib.as_mut_ptr(),
                mib.len() as c_uint,
                std::ptr::null_mut(),
                &mut size,
                std::ptr::null_mut(),
                0,
            )
        };
        if first != 0 || size < 4 {
            return Vec::new();
        }
        let mut buffer = vec![0_u8; size];
        let second = unsafe {
            sysctl(
                mib.as_mut_ptr(),
                mib.len() as c_uint,
                buffer.as_mut_ptr().cast::<c_void>(),
                &mut size,
                std::ptr::null_mut(),
                0,
            )
        };
        if second != 0 || size < 4 {
            return Vec::new();
        }
        buffer.truncate(size);
        let argc = i32::from_ne_bytes(buffer[..4].try_into().unwrap_or([0; 4]));
        if argc < 0 {
            return Vec::new();
        }
        let fields: Vec<&[u8]> = buffer[4..].split(|byte| *byte == 0).collect();
        let mut index = 1;
        while index < fields.len() && fields[index].is_empty() {
            index += 1;
        }
        index = match index.checked_add(argc as usize) {
            Some(index) if index < fields.len() => index,
            _ => return Vec::new(),
        };
        let mut result = Vec::new();
        for field in &fields[index..] {
            if field.is_empty() {
                break;
            }
            result.push(field.to_vec());
        }
        return result;
    }

    #[allow(unreachable_code)]
    Vec::new()
}

fn process_env(pid: i32, name: &str) -> Option<String> {
    let mut prefix = name.as_bytes().to_vec();
    prefix.push(b'=');
    env_of_pid(pid).into_iter().find_map(|entry| {
        entry
            .strip_prefix(prefix.as_slice())
            .map(String::from_utf8_lossy)
            .map(|v| v.into_owned())
    })
}

fn guest_name() -> String {
    env::var("ROOTS_GUEST_NAME").unwrap_or_else(|_| "lxrun".into())
}

fn guests_on_root(path: &Path, base: &Path) -> Vec<i32> {
    let Ok(want) = canonical_path(path, base) else {
        return Vec::new();
    };
    let name = guest_name();
    process_rows(&["-Ao", "pid=,comm=,command="])
        .into_iter()
        .filter_map(|line| {
            let mut fields = line.split_whitespace();
            let pid: i32 = fields.next()?.parse().ok()?;
            let command = fields.next()?;
            let args = fields.collect::<Vec<_>>().join(" ");
            let basename = command.rsplit('/').next().unwrap_or(command);
            if basename != name || is_runtime_audio_service(&args) {
                return None;
            }
            Some(pid)
        })
        .filter(|pid| {
            let Some(root) = process_env(*pid, "LXRT_ROOT") else {
                return false;
            };
            canonical_path(Path::new(&root), Path::new("/")).is_ok_and(|actual| actual == want)
        })
        .collect()
}

fn is_runtime_audio_service(args: &str) -> bool {
    matches!(
        args.split_whitespace().nth(1),
        Some(
            "/usr/bin/pipewire"
                | "/usr/bin/pipewire-pulse"
                | "/usr/bin/wireplumber"
                | "/usr/bin/pulseaudio"
        )
    )
}

fn executable(path: &Path) -> bool {
    fs::metadata(path)
        .map(|metadata| metadata.permissions().mode() & 0o111 != 0)
        .unwrap_or(false)
}

fn replace_jitless_call(contents: &[u8]) -> Vec<u8> {
    let needle = b"steamwebhelper \"$@\" &>";
    let replacement = b"steamwebhelper \"$@\" --js-flags=--jitless &>";
    let mut result = Vec::with_capacity(contents.len() + replacement.len());
    let mut start = 0;
    while start < contents.len() {
        let end = contents[start..]
            .iter()
            .position(|byte| *byte == b'\n')
            .map(|offset| start + offset + 1)
            .unwrap_or(contents.len());
        let line = &contents[start..end];
        if let Some(position) = line
            .windows(needle.len())
            .position(|window| window == needle)
        {
            result.extend_from_slice(&line[..position]);
            result.extend_from_slice(replacement);
            result.extend_from_slice(&line[position + needle.len()..]);
        } else {
            result.extend_from_slice(line);
        }
        start = end;
    }
    result
}

fn restore_webhelper(path: &Path) -> io::Result<bool> {
    let backup = path.with_extension("sh.steamarm-orig");
    if !backup.is_file() {
        return Ok(false);
    }
    fs::rename(backup, path)?;
    Ok(true)
}

struct WebHelperRestore(PathBuf);

impl Drop for WebHelperRestore {
    fn drop(&mut self) {
        let _ = restore_webhelper(&self.0);
    }
}

fn set_jitless(path: &Path) -> Result<WebHelperRestore, String> {
    let backup = path.with_extension("sh.steamarm-orig");
    let status = Command::new("/bin/cp")
        .args([OsStr::new("-p")])
        .arg(path)
        .arg(&backup)
        .stdin(Stdio::null())
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map_err(|error| format!("cannot preserve {}: {error}", path.display()))?;
    if !status.success() {
        return Err(format!("cannot preserve {}", path.display()));
    }
    let original = fs::read(&backup).map_err(|error| error.to_string())?;
    let modified = replace_jitless_call(&original);
    fs::write(path, &modified).map_err(|error| error.to_string())?;
    if !modified
        .windows(b"--js-flags=--jitless".len())
        .any(|window| window == b"--js-flags=--jitless")
    {
        let _ = restore_webhelper(path);
        return Err("--jitless: steamwebhelper.sh has an unexpected form".into());
    }
    Ok(WebHelperRestore(path.to_path_buf()))
}

fn timestamp() -> String {
    Command::new("/bin/date")
        .arg("+%Y%m%d-%H%M%S")
        .output()
        .ok()
        .filter(|output| output.status.success())
        .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
        .filter(|text| !text.is_empty())
        .unwrap_or_else(|| {
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default()
                .as_secs()
                .to_string()
        })
}

fn now_seconds() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs()
}

fn print_pid_list(pids: &[i32]) -> String {
    let mut text = String::new();
    for pid in pids {
        text.push_str(&format!("{pid} "));
    }
    text
}

fn tracked_processes(lxrun: &str, known: &[i32]) -> Vec<i32> {
    let rows = parse_process_rows(&["-axo", "pid=,ppid=,command="], false);
    let mut lx_processes = HashMap::<i32, i32>::new();
    for row in rows {
        if row.command.contains(lxrun) {
            lx_processes.insert(row.pid, row.ppid);
        }
    }
    let mut ours: HashSet<i32> = known
        .iter()
        .filter(|pid| lx_processes.contains_key(pid))
        .copied()
        .collect();
    loop {
        let previous = ours.len();
        for (&pid, &ppid) in &lx_processes {
            if ours.contains(&ppid) {
                ours.insert(pid);
            }
        }
        if ours.len() == previous {
            break;
        }
    }
    let mut result: Vec<i32> = ours.into_iter().collect();
    result.sort_unstable();
    result
}

fn elapsed_seconds(value: &str) -> Option<u64> {
    let (days, time) = if let Some((days, time)) = value.split_once('-') {
        (days.parse::<u64>().ok()?, time)
    } else {
        (0, value)
    };
    let values: Vec<u64> = time
        .split(':')
        .map(str::parse::<u64>)
        .collect::<Result<_, _>>()
        .ok()?;
    let seconds = match values.as_slice() {
        [minutes, seconds] => minutes.checked_mul(60)?.checked_add(*seconds)?,
        [hours, minutes, seconds] => hours
            .checked_mul(3600)?
            .checked_add(minutes.checked_mul(60)?)?
            .checked_add(*seconds)?,
        _ => return None,
    };
    days.checked_mul(86_400)?.checked_add(seconds)
}

fn orphan_candidates(lxrun: &str, max_age: u64) -> Vec<i32> {
    let rows = parse_process_rows(&["-axo", "pid=,ppid=,etime=,command="], true);
    rows.into_iter()
        .filter(|row| {
            row.ppid == 1
                && row.command.contains(&format!("{lxrun} "))
                && row.command.contains("/steamrtarm64/")
                && elapsed_seconds(&row.elapsed).is_some_and(|age| age <= max_age)
        })
        .map(|row| row.pid)
        .collect()
}

fn own_orphans(lxrun: &str, run_id: &str, max_age: u64) -> Vec<i32> {
    orphan_candidates(lxrun, max_age)
        .into_iter()
        .filter(|pid| process_env(*pid, "STEAMARM_RUN_ID").as_deref() == Some(run_id))
        .collect()
}

fn send_signal(pids: &[i32], signal_number: c_int) {
    for pid in pids {
        unsafe {
            kill(*pid, signal_number);
        }
    }
}

fn reap_orphans(lxrun: &str, run_id: &str, elapsed: u64) {
    let candidates = orphan_candidates(lxrun, elapsed.saturating_add(2));
    if candidates.is_empty() {
        return;
    }
    let orphans = own_orphans(lxrun, run_id, elapsed.saturating_add(2));
    let other: Vec<i32> = candidates
        .iter()
        .filter(|pid| !orphans.contains(pid))
        .copied()
        .collect();
    if !other.is_empty() {
        eprintln!(
            "left running, without this run's STEAMARM_RUN_ID:{}",
            print_pid_list(&other)
        );
    }
    if orphans.is_empty() {
        return;
    }
    eprintln!(
        "stopping orphaned webhelper processes: {}",
        print_pid_list(&orphans)
    );
    send_signal(&orphans, SIGTERM);
    thread::sleep(Duration::from_secs(2));
    let remaining = own_orphans(lxrun, run_id, elapsed.saturating_add(4));
    send_signal(&remaining, SIGKILL);
}

fn stop_session(lxrun: &str, mut pids: Vec<i32>, run_id: &str, started: Instant) {
    pids = tracked_processes(lxrun, &pids);
    send_signal(&pids, SIGTERM);
    for _ in 0..5 {
        pids = tracked_processes(lxrun, &pids);
        if pids.is_empty() {
            break;
        }
        thread::sleep(Duration::from_secs(1));
    }
    send_signal(&pids, SIGKILL);
    reap_orphans(lxrun, run_id, started.elapsed().as_secs());
}

fn set_root_link(root: &Path, link: &Path, base: &Path) -> Result<(), String> {
    let root_canonical = canonical_path(root, base)?;
    let link_canonical = canonical_path(link, base).unwrap_or_default();
    if root_canonical == link_canonical {
        return Ok(());
    }
    let link_path = absolute_from(link, base);
    match fs::symlink_metadata(&link_path) {
        Ok(metadata) if !metadata.file_type().is_symlink() => {
            if fs::metadata(&link_path).is_ok() {
                return Err(format!("{} exists and is not a link", link.display()));
            }
            fs::remove_file(&link_path).map_err(|error| error.to_string())?;
        }
        Ok(_) => {
            let previous = fs::read_link(&link_path)
                .map(|path| path.display().to_string())
                .unwrap_or_else(|_| "?".into());
            eprintln!(
                "re-pointing {}: {} -> {}",
                link.display(),
                previous,
                root.display()
            );
            fs::remove_file(&link_path).map_err(|error| error.to_string())?;
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(format!("cannot inspect {}: {error}", link.display())),
    }
    symlink(root, &link_path).map_err(|error| {
        format!(
            "cannot link {} -> {}: {error}",
            link.display(),
            root.display()
        )
    })
}

fn start_safeguard(project_root: &Path) {
    safeguard::start(project_root);
}

fn start_x11(project_root: &Path) -> bool {
    Command::new(project_root.join("scripts/run-x11-native.sh"))
        .args(["start", ":2"])
        .current_dir(project_root)
        .stdout(Stdio::null())
        .status()
        .is_ok_and(|status| status.success())
}

fn parse_run_roots(project_root: &Path, state: &Path) -> Result<(PathBuf, PathBuf), String> {
    let explicit_root = env::var_os("ARMROOT").filter(|value| !value.is_empty());
    let explicit_link = env::var_os("ARMROOT_LINK").filter(|value| !value.is_empty());
    if let Some(link) = explicit_link {
        let root = explicit_root
            .map(PathBuf::from)
            .unwrap_or_else(|| state.join("armroot"));
        return Ok((root, PathBuf::from(link)));
    }
    let Some(root) = explicit_root else {
        return Ok((state.join("arm64root"), PathBuf::from(DEFAULT_LINK)));
    };
    let root = PathBuf::from(root);
    let actual = canonical_path(&root, project_root)?;
    let old = canonical_path(&state.join("armroot"), project_root)?;
    let frame = canonical_path(&state.join("arm64root"), project_root)?;
    if actual == old {
        Ok((root, PathBuf::from("/tmp/lxrt-armroot")))
    } else if actual == frame {
        Ok((root, PathBuf::from(DEFAULT_LINK)))
    } else {
        Err(format!(
            "ARMROOT={} is neither $STEAMARM_STATE/armroot nor .../arm64root: set ARMROOT_LINK=/tmp/<a name of its own> as well",
            root.display()
        ))
    }
}

fn parse_args(args: Vec<OsString>) -> Result<(Option<u64>, bool, Vec<OsString>), String> {
    let mut remaining = args;
    let (duration, jitless, rest) = parse_options(&mut remaining)?;
    Ok((duration, jitless, rest))
}

fn parse_options(args: &mut Vec<OsString>) -> Result<(Option<u64>, bool, Vec<OsString>), String> {
    let mut duration = None;
    let mut jitless = false;
    while !args.is_empty() {
        match args[0].to_str() {
            Some("--for") => {
                if args.len() < 2 {
                    return Err("--for needs seconds".into());
                }
                let value = &args[1];
                if value.is_empty() {
                    duration = None;
                } else {
                    let text = value
                        .to_str()
                        .ok_or_else(|| "--for needs seconds".to_string())?;
                    duration = Some(
                        text.parse::<u64>()
                            .map_err(|_| format!("--for needs whole seconds: {text}"))?,
                    );
                }
                args.drain(0..2);
            }
            Some("--jitless") => {
                jitless = true;
                args.remove(0);
            }
            _ => break,
        }
    }
    Ok((duration, jitless, args.clone()))
}

fn host_page_size() -> Option<u64> {
    Command::new("/usr/bin/getconf")
        .arg("PAGE_SIZE")
        .output()
        .ok()
        .filter(|output| output.status.success())
        .and_then(|output| {
            String::from_utf8_lossy(&output.stdout)
                .trim()
                .parse::<u64>()
                .ok()
        })
}

fn persist_fex_host_page_size(arm_root: &Path) -> Result<(), String> {
    let tool_dir = arm_root
        .join(DEFAULT_HOME.trim_start_matches('/'))
        .join(".local/share/Steam/compatibilitytools.d/steamarm-fex-linux");
    if !tool_dir.is_dir() {
        return Ok(());
    }
    let sidecar = tool_dir.join("steamarm-host-page-size");
    let Some(size) = host_page_size() else {
        let _ = fs::remove_file(&sidecar);
        return Ok(());
    };
    fs::write(&sidecar, format!("{size}\n")).map_err(|error| {
        format!(
            "cannot persist host page size for the FEX compatibility tool at {}: {error}",
            sidecar.display()
        )
    })
}

fn spawn_steam(
    lxrun: &OsStr,
    link: &OsStr,
    log: &Path,
    guest_env: &[(OsString, OsString)],
    run_id: &str,
    args: &[OsString],
) -> io::Result<Child> {
    let stdout = File::create(log)?;
    let stderr = stdout.try_clone()?;
    let mut command = Command::new(lxrun);
    command
        .arg(STEAM_CLIENT)
        .args(args)
        .current_dir(project_root())
        .stdout(Stdio::from(stdout))
        .stderr(Stdio::from(stderr));
    for name in ENV_UNSET_BEFORE_GUEST {
        command.env_remove(OsStr::from_bytes(name));
    }
    command.envs(guest_env.iter().map(|(key, value)| (key, value)));
    let display = env::var_os("DISPLAY")
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| ":2".into());
    let x18 = env::var_os("LXRT_X18_ALL_TEXT").unwrap_or_else(|| "libcef.so".into());
    let hidapi = env::var_os("SDL_HIDAPI_LIBUSB").unwrap_or_else(|| "0".into());
    let host_page_size = host_page_size()
        .map(|size| size.to_string())
        .unwrap_or_else(|| "unknown".into());
    command
        .env("DISPLAY", display)
        .env("TMPDIR", "/tmp")
        .env(
            "PATH",
            "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin",
        )
        .env("OBJC_DISABLE_INITIALIZE_FORK_SAFETY", "YES")
        .env("LXRT_ROOT", link)
        .env("LXRT_GUEST_PAGE", "4096")
        .env("HOME", DEFAULT_HOME)
        .env("LXRT_X18_ALL_TEXT", x18)
        .env("SDL_HIDAPI_LIBUSB", hidapi)
        .env("STEAMARM_HOST_PAGE_SIZE", host_page_size)
        .env("STEAMARM_RUN_ID", run_id);
    Ok(command.spawn()?)
}

fn run() -> Result<i32, (i32, String)> {
    INTERRUPTED.store(false, Ordering::Relaxed);
    let project_root = project_root();
    let state = state_dir();
    let (arm_root, link) = parse_run_roots(&project_root, &state).map_err(|error| (1, error))?;
    let raw_args: Vec<OsString> = env::args_os().skip(1).collect();
    let (limit, jitless, mut steam_args) = parse_args(raw_args).map_err(|error| (2, error))?;

    let default_lxrun = project_root.join("build/lxrun");
    let lxrun = env::var_os("STEAMARM_LXRUN")
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| default_lxrun.as_os_str().to_os_string());
    let lxrun_path = absolute_from(Path::new(&lxrun), &project_root);
    if !executable(&lxrun_path) {
        return Err((1, "no build/lxrun (make lxrt)".into()));
    }
    let client = absolute_from(
        &arm_root.join("tmp/armhome/.local/share/Steam/steamrtarm64/steam"),
        &project_root,
    );
    if !client.is_file() {
        return Err((1, format!("no arm64 client at {}", client.display())));
    }

    let busy_root = guests_on_root(&arm_root, &project_root);
    let busy_link = guests_on_root(&link, &project_root);
    let mut busy: Vec<i32> = busy_root.into_iter().chain(busy_link).collect();
    busy.sort_unstable();
    busy.dedup();
    if !busy.is_empty() {
        let pids = busy
            .iter()
            .map(i32::to_string)
            .collect::<Vec<_>>()
            .join(" ");
        return Err((
            1,
            format!(
                "guests are running on {} or {} (pid {}): stop them first",
                arm_root.display(),
                link.display(),
                pids
            ),
        ));
    }
    set_root_link(&arm_root, &link, &project_root).map_err(|error| (1, error))?;

    let webhelper = client.parent().unwrap_or(&client).join("steamwebhelper.sh");
    let _ = restore_webhelper(&webhelper);
    let webhelper_guard = if jitless {
        match set_jitless(&webhelper) {
            Ok(guard) => {
                steam_args.insert(0, OsString::from("-noverifyfiles"));
                Some(guard)
            }
            Err(error) => {
                let _ = restore_webhelper(&webhelper);
                return Err((1, error));
            }
        }
    } else {
        None
    };

    start_safeguard(&project_root);
    let display = env::var_os("DISPLAY")
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| ":2".into());
    if display == OsStr::new(":2") && !start_x11(&project_root) {
        drop(webhelper_guard);
        return Err((1, "native X server failed to start".into()));
    }

    let logs = state.join("logs");
    fs::create_dir_all(&logs)
        .map_err(|error| (1, format!("cannot create {}: {error}", logs.display())))?;
    let log = logs.join(format!("native-arm64-steam-{}.log", timestamp()));
    println!("log: {}", log.display());

    let guest_values = guest_environment(&absolute_from(&arm_root, &project_root));
    persist_fex_host_page_size(&arm_root).map_err(|error| (1, error))?;
    let run_id = format!("steam-arm64.{}.{}", std::process::id(), now_seconds());
    install_signal_handlers();
    let lxrun_text = lxrun.to_string_lossy().into_owned();
    let mut child = match spawn_steam(
        &lxrun,
        link.as_os_str(),
        &log,
        &guest_values,
        &run_id,
        &steam_args,
    ) {
        Ok(child) => child,
        Err(error) => {
            drop(webhelper_guard);
            return Err((
                1,
                format!("cannot start {}: {error}", Path::new(&lxrun).display()),
            ));
        }
    };
    let child_pid = child.id() as i32;
    let started = Instant::now();
    let mut tracked = vec![child_pid];
    let mut timed_out = false;

    loop {
        let _ = child.try_wait(); // reap the initial lxrun before scanning ps
        tracked = tracked_processes(&lxrun_text, &tracked);
        if INTERRUPTED.load(Ordering::Relaxed) {
            stop_session(&lxrun_text, tracked, &run_id, started);
            let _ = child.wait();
            drop(webhelper_guard);
            return Ok(SIGINTERRUPTED);
        }
        if tracked.is_empty() {
            break;
        }
        if limit.is_some_and(|seconds| started.elapsed().as_secs() >= seconds) {
            timed_out = true;
            stop_session(&lxrun_text, tracked, &run_id, started);
            break;
        }
        thread::sleep(Duration::from_secs(1));
    }
    if !timed_out {
        reap_orphans(&lxrun_text, &run_id, started.elapsed().as_secs());
    }
    let _ = child.wait();
    drop(webhelper_guard);
    println!(
        "ended after {} s; log: {}",
        started.elapsed().as_secs(),
        log.display()
    );
    Ok(0)
}

fn main() {
    match run() {
        Ok(code) => std::process::exit(code),
        Err((code, message)) => {
            eprintln!("{message}");
            std::process::exit(code);
        }
    }
}
