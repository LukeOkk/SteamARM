use std::env;
use std::ffi::OsString;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

const SECTION: &[u8] = br"Software\\Wine\\X11 Driver";
const VALUE_PREFIX: &[u8] = b"\"EmulateModeset\"=";
const VALUE: &[u8] = b"\"EmulateModeset\"=\"Y\"";
const MARKER: &str = "launcher/emulate-modeset.applied";

fn home_dir() -> PathBuf {
    env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"))
}

fn state_dir() -> PathBuf {
    if let Some(value) = env::var_os("STEAMARM_STATE").filter(|v| !v.is_empty()) {
        let value = value.to_string_lossy();
        if value == "~" {
            return home_dir();
        }
        if let Some(rest) = value.strip_prefix("~/") {
            return home_dir().join(rest);
        }
        return PathBuf::from(value.as_ref());
    }
    let newer = home_dir().join("Developer/SteamARM-roots");
    let older = home_dir().join("SteamARM-roots");
    if newer.is_dir() || !older.is_dir() {
        newer
    } else {
        older
    }
}

fn has_section(line: &[u8]) -> bool {
    if !line.starts_with(b"[") || line.len() < SECTION.len() + 2 {
        return false;
    }
    let end = SECTION.len() + 1;
    if !line[1..end].eq_ignore_ascii_case(SECTION) || line[end] != b']' {
        return false;
    }
    line.len() == end + 1 || line[end + 1].is_ascii_whitespace()
}

fn has_value(line: &[u8]) -> bool {
    line.len() >= VALUE_PREFIX.len()
        && line[..VALUE_PREFIX.len()].eq_ignore_ascii_case(VALUE_PREFIX)
}

fn set_value(text: &[u8], on: bool) -> io::Result<Vec<u8>> {
    let mut lines = text
        .split(|byte| *byte == b'\n')
        .map(<[u8]>::to_vec)
        .collect::<Vec<_>>();
    let Some(start) = lines.iter().position(|line| has_section(line)) else {
        if !on {
            return Ok(text.to_vec());
        }
        let stamp = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .map_err(io::Error::other)?
            .as_secs();
        let mut out = text.to_vec();
        if !out.ends_with(b"\n") {
            out.push(b'\n');
        }
        out.extend_from_slice(b"\n[");
        out.extend_from_slice(SECTION);
        out.extend_from_slice(format!("] {stamp}\n").as_bytes());
        out.extend_from_slice(VALUE);
        out.push(b'\n');
        return Ok(out);
    };
    let end = lines
        .iter()
        .enumerate()
        .skip(start + 1)
        .find_map(|(idx, line)| line.starts_with(b"[").then_some(idx))
        .unwrap_or(lines.len());
    let idx = lines[start + 1..end]
        .iter()
        .position(|line| has_value(line))
        .map(|idx| start + 1 + idx);
    if on {
        if let Some(idx) = idx {
            if lines[idx] == VALUE {
                return Ok(text.to_vec());
            }
            lines[idx] = VALUE.to_vec();
        } else {
            let mut at = start + 1;
            while at < end && lines[at].starts_with(b"#") {
                at += 1;
            }
            lines.insert(at, VALUE.to_vec());
        }
    } else if let Some(idx) = idx {
        lines.remove(idx);
    } else {
        return Ok(text.to_vec());
    }
    Ok(lines.join(&b'\n'))
}

fn apply(path: &Path, on: bool) -> io::Result<bool> {
    let text = match fs::read(path) {
        Ok(text) => text,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(false),
        Err(error) => return Err(error),
    };
    let updated = set_value(&text, on)?;
    if updated == text {
        return Ok(false);
    }
    let mut temporary = path.as_os_str().to_os_string();
    temporary.push(".steamarm-tmp");
    fs::write(&temporary, updated)?;
    fs::rename(temporary, path)?;
    Ok(true)
}

fn collect_steam_prefixes(state: &Path, root: &str, home: &str, out: &mut Vec<PathBuf>) {
    let compatdata = state
        .join(root)
        .join("tmp")
        .join(home)
        .join(".local/share/Steam/steamapps/compatdata");
    let Ok(entries) = fs::read_dir(compatdata) else {
        return;
    };
    let mut prefixes = entries
        .filter_map(Result::ok)
        .map(|entry| entry.path().join("pfx/user.reg"))
        .collect::<Vec<_>>();
    prefixes.sort();
    out.extend(prefixes);
}

fn default_files(state: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    for (root, home) in [
        ("steamroot", "fexhome"),
        ("arm64root", "armhome"),
        ("armroot", "armhome"),
    ] {
        collect_steam_prefixes(state, root, home, &mut out);
    }
    let app_prefixes = state.join("steamroot/tmp/fexhome/.steamarm/prefixes");
    if let Ok(entries) = fs::read_dir(app_prefixes) {
        let mut prefixes = entries
            .filter_map(Result::ok)
            .map(|entry| entry.path().join("pfx/user.reg"))
            .collect::<Vec<_>>();
        prefixes.sort();
        out.extend(prefixes);
    }
    out
}

fn usage() -> &'static str {
    "Wine options SteamARM keeps in Wine prefixes.\n\nUsage: steamarm-wine-prefix-options emulate-modeset on|off [USER_REG...]\n\nWithout USER_REG, it processes Steam game prefixes in the x86 Steam root and\nboth ARM64 roots, plus launcher-managed Windows app prefixes. `off` undoes\nonly a value previously added by SteamARM. Run only while no wineserver is active.\n"
}

fn run(args: Vec<OsString>) -> Result<i32, String> {
    if args.len() < 2 || args[0] != "emulate-modeset" || (args[1] != "on" && args[1] != "off") {
        eprint!("{}", usage());
        return Ok(2);
    }
    let on = args[1] == "on";
    let explicit = args.len() > 2;
    let state = state_dir();
    let marker = state.join(MARKER);
    if !explicit && !on && !marker.exists() {
        return Ok(0);
    }
    let files = if explicit {
        args.into_iter().skip(2).map(PathBuf::from).collect()
    } else {
        default_files(&state)
    };
    for path in files {
        match apply(&path, on) {
            Ok(true) => println!(
                "{}: EmulateModeset {}",
                path.display(),
                if on { "Y" } else { "removed" }
            ),
            Ok(false) => {}
            Err(error) => return Err(format!("{}: {error}", path.display())),
        }
    }
    if !explicit {
        if on {
            if let Some(parent) = marker.parent() {
                fs::create_dir_all(parent).map_err(|e| format!("{}: {e}", parent.display()))?;
            }
            fs::File::create(&marker).map_err(|e| format!("{}: {e}", marker.display()))?;
        } else if marker.exists() {
            fs::remove_file(&marker).map_err(|e| format!("{}: {e}", marker.display()))?;
        }
    }
    Ok(0)
}

fn main() {
    let args = env::args_os().skip(1).collect();
    match run(args) {
        Ok(status) => std::process::exit(status),
        Err(error) => {
            eprintln!("steamarm-wine-prefix-options: {error}");
            std::process::exit(1);
        }
    }
}
