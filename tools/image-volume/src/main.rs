use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const GIB: u64 = 1_073_741_824;

fn state_root() -> PathBuf {
    if let Some(state) = env::var_os("STEAMARM_STATE") {
        return PathBuf::from(state);
    }
    let home = env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"));
    let preferred = home.join("Developer/SteamARM-roots");
    let legacy = home.join("SteamARM-roots");
    if preferred.is_dir() || !legacy.is_dir() {
        preferred
    } else {
        legacy
    }
}

fn capture(program: &str, args: &[&str]) -> Option<String> {
    let output = Command::new(program)
        .args(args)
        .stdin(Stdio::null())
        .stderr(Stdio::null())
        .output()
        .ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).into_owned())
}

fn volume_bytes(path: &Path) -> Option<u64> {
    let output = capture("df", &["-k", path.to_str()?])?;
    let row = output.lines().nth(1)?;
    row.split_whitespace()
        .nth(1)?
        .parse::<u64>()
        .ok()?
        .checked_mul(1024)
}

fn target_bytes(bundle: &Path) -> Option<u64> {
    if let Some(value) = env::var("STEAMARM_IMAGE_TARGET_BYTES").ok() {
        return value.parse().ok();
    }
    volume_bytes(bundle.parent()?)
}

fn image_bytes(bundle: &Path) -> Option<u64> {
    let output = capture("hdiutil", &["imageinfo", bundle.to_str()?])?;
    output.lines().find_map(|line| {
        line.trim()
            .strip_prefix("Total Bytes:")
            .and_then(|value| value.trim().parse().ok())
    })
}

fn image_info() -> String {
    capture("hdiutil", &["info"]).unwrap_or_default()
}

fn info_value(line: &str) -> Option<&str> {
    line.strip_prefix("image-path")
        .and_then(|s| s.trim_start().strip_prefix(':'))
        .map(str::trim)
}

fn attached_at(bundle: &Path) -> Option<PathBuf> {
    let target = bundle.to_str()?;
    let mut match_image = false;
    for line in image_info().lines() {
        if let Some(image) = info_value(line) {
            match_image = image == target;
            continue;
        }
        if match_image && line.starts_with("/dev/disk") {
            if let Some(mount) = line
                .split_whitespace()
                .last()
                .filter(|s| s.starts_with('/'))
            {
                return Some(PathBuf::from(mount));
            }
        }
    }
    None
}

fn attached_bytes(bundle: &Path) -> Option<u64> {
    let target = bundle.to_str()?;
    let mut match_image = false;
    let mut blocks = None;
    let mut block_size = None;
    for line in image_info().lines() {
        if let Some(image) = info_value(line) {
            match_image = image == target;
            blocks = None;
            block_size = None;
            continue;
        }
        if !match_image {
            continue;
        }
        let trimmed = line.trim();
        if let Some(value) = trimmed.strip_prefix("blockcount") {
            blocks = value
                .split_whitespace()
                .last()
                .and_then(|s| s.parse::<u64>().ok());
        } else if let Some(value) = trimmed.strip_prefix("blocksize") {
            block_size = value
                .split_whitespace()
                .last()
                .and_then(|s| s.parse::<u64>().ok());
        } else if line.starts_with("/dev/disk") {
            if let (Some(n), Some(size)) = (blocks, block_size) {
                return n.checked_mul(size);
            }
        }
    }
    None
}

fn mounted_bytes(mount: &Path) -> Option<u64> {
    let output = capture("diskutil", &["info", mount.to_str()?])?;
    output.lines().find_map(|line| {
        let value = line.trim().strip_prefix("Disk Size:")?;
        let start = value.rfind('(')? + 1;
        let end = value[start..].find(" Bytes)")? + start;
        value[start..end]
            .trim()
            .parse::<u64>()
            .ok()?
            .checked_add(512 * 1024 * 1024)
    })
}

fn read_only(mount: &Path) -> bool {
    capture("diskutil", &["info", mount.to_str().unwrap_or_default()]).is_some_and(|output| {
        output.lines().any(|line| {
            let line = line.trim();
            (line.starts_with("Media Read-Only:") || line.starts_with("Volume Read-Only:"))
                && line.ends_with("Yes")
        })
    })
}

fn log(message: &str) {
    eprintln!("[image-volume] {message}");
}

fn in_use(mount: &Path) -> bool {
    capture("lsof", &["-t", mount.to_str().unwrap_or_default()])
        .is_some_and(|output| output.lines().any(|line| !line.trim().is_empty()))
}

fn command_ok(program: &str, args: &[&str]) -> bool {
    Command::new(program)
        .args(args)
        .status()
        .is_ok_and(|s| s.success())
}

fn grow(bundle: &Path) -> Result<(), String> {
    let (Some(want), Some(have)) = (target_bytes(bundle), image_bytes(bundle)) else {
        return Ok(());
    };
    if have >= want.saturating_sub(GIB) {
        return Ok(());
    }
    let gib = want / GIB;
    log(&format!(
        "growing {} from {} GiB to {gib} GiB (sparse)",
        bundle.file_name().unwrap_or_default().to_string_lossy(),
        have / GIB
    ));
    let bytes = format!("{}B", gib.saturating_mul(GIB));
    if command_ok(
        "diskutil",
        &[
            "image",
            "resize",
            "--size",
            &bytes,
            bundle.to_str().unwrap_or_default(),
        ],
    ) {
        return Ok(());
    }
    if command_ok(
        "hdiutil",
        &[
            "resize",
            "-size",
            &format!("{gib}g"),
            bundle.to_str().unwrap_or_default(),
        ],
    ) {
        return Ok(());
    }
    Err(format!("could not grow {}", bundle.display()))
}

fn ensure(bundle: &Path, mount: &Path) -> Result<(), String> {
    if !bundle.is_dir() {
        return Ok(());
    }
    let mut attached = attached_at(bundle);
    if let Some(at) = attached.as_ref() {
        let want = target_bytes(bundle);
        let mut have = attached_bytes(bundle);
        if have.is_none() {
            have = mounted_bytes(at);
        }
        let why = if read_only(at) {
            Some("read-only")
        } else if want
            .zip(have)
            .is_some_and(|(w, h)| h < w.saturating_sub(GIB))
        {
            Some("small")
        } else {
            None
        };
        let Some(why) = why else { return Ok(()) };
        if !in_use(at)
            && command_ok(
                "hdiutil",
                &["detach", "-quiet", at.to_str().unwrap_or_default()],
            )
        {
            attached = None;
        } else if why == "read-only" {
            return Err(format!(
                "{} is attached READ-ONLY at {} and in use; stop its users before retrying",
                bundle.file_name().unwrap_or_default().to_string_lossy(),
                at.display()
            ));
        } else {
            log(&format!(
                "{} is in use; it will grow after its users close it",
                bundle.file_name().unwrap_or_default().to_string_lossy()
            ));
            return Ok(());
        }
    }
    if attached.is_none() {
        grow(bundle)?;
        let status = Command::new("hdiutil")
            .args(["attach", "-quiet", "-nobrowse", "-mountpoint"])
            .arg(mount)
            .arg(bundle)
            .status()
            .map_err(|e| format!("cannot attach {}: {e}", bundle.display()))?;
        if !status.success() {
            return Err(format!(
                "could not attach {} at {}",
                bundle.display(),
                mount.display()
            ));
        }
        if read_only(mount) {
            return Err(format!(
                "{} was attached READ-ONLY at {} (another process may be using it)",
                bundle.file_name().unwrap_or_default().to_string_lossy(),
                mount.display()
            ));
        }
    }
    Ok(())
}

fn ensure_locked(args: &[String]) -> Result<(), (i32, String)> {
    if env::var_os("STEAMARM_IMAGE_VOLUME_LOCKED").is_some()
        || !Path::new("/usr/bin/lockf").is_file()
    {
        return Ok(());
    }
    let state = state_root();
    let lock_dir = state.join("launcher");
    fs::create_dir_all(&lock_dir)
        .map_err(|e| (1, format!("create {}: {e}", lock_dir.display())))?;
    let executable =
        env::current_exe().map_err(|e| (1, format!("locate image-volume tool: {e}")))?;
    let mut child = Command::new("/usr/bin/lockf");
    child
        .args(["-k", "-t", "300"])
        .arg(lock_dir.join(".image-volume.lock"))
        .arg(executable)
        .args(args)
        .env("STEAMARM_IMAGE_VOLUME_LOCKED", "1");
    let status = child
        .status()
        .map_err(|e| (1, format!("lock image-volume operation: {e}")))?;
    if status.success() {
        Ok(())
    } else {
        Err((
            status.code().unwrap_or(1),
            "locked image operation failed".into(),
        ))
    }
}

fn run() -> Result<(), (i32, String)> {
    let args = env::args().skip(1).collect::<Vec<_>>();
    if args.first().map(String::as_str) == Some("ensure") && args.len() != 3 {
        return Err((2, "usage: image-volume.sh ensure BUNDLE MOUNT".into()));
    }
    if args.first().map(String::as_str) == Some("ensure") {
        ensure_locked(&args)?;
    }
    match args.first().map(String::as_str) {
        Some("ensure") => ensure(Path::new(&args[1]), Path::new(&args[2])).map_err(|e| (1, e)),
        Some("backing") if args.len() == 1 => {
            let state = state_root();
            let mut entries = fs::read_dir(&state)
                .map_err(|e| (1, format!("read {}: {e}", state.display())))?
                .filter_map(Result::ok)
                .filter(|entry| {
                    entry
                        .file_name()
                        .to_string_lossy()
                        .ends_with(".sparsebundle")
                        && entry.path().is_dir()
                })
                .filter_map(|entry| {
                    let bundle = entry.path();
                    attached_at(&bundle).map(|mount| {
                        format!(
                            "{}={}",
                            mount.display(),
                            bundle.parent().unwrap_or(&state).display()
                        )
                    })
                })
                .collect::<Vec<_>>();
            entries.sort();
            println!("{}", entries.join(";"));
            Ok(())
        }
        _ => Err((
            2,
            "usage: image-volume.sh backing | ensure BUNDLE MOUNT".into(),
        )),
    }
}

fn main() {
    if let Err((code, message)) = run() {
        eprintln!("[image-volume] {message}");
        std::process::exit(code);
    }
}
