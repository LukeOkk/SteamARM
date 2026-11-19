use std::env;
use std::ffi::{OsStr, OsString};
use std::fs;
use std::os::unix::ffi::OsStringExt;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::Command;

#[path = "../../common/safeguard.rs"]
mod safeguard;

const DEFAULT_ROOT: &str = "/tmp/lxrt-arm64root";
const DEFAULT_HOME: &str = "/tmp/fexhome";
const DEFAULT_GUEST_PATH: &str = "/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin";

fn valid_key(key: &[u8]) -> bool {
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
    let mut values = Vec::new();
    for line in contents.split(|byte| *byte == b'\n') {
        let Some(separator) = line.iter().position(|byte| *byte == b'=') else {
            continue;
        };
        let (key, value) = line.split_at(separator);
        if !valid_key(key) || key == b"LXRT_ROOT" || value[1..].contains(&0) {
            continue;
        }
        let name = OsString::from_vec(key.to_vec());
        // A caller value, even an empty one, wins as in guest-env.sh.
        if env::var_os(&name).is_some() {
            continue;
        }
        values.push((name, OsString::from_vec(value[1..].to_vec())));
    }
    values
}

fn effective_env(name: &str, guest: &[(OsString, OsString)]) -> Option<OsString> {
    env::var_os(name).or_else(|| {
        guest
            .iter()
            .find(|(key, _)| key == OsStr::new(name))
            .map(|(_, value)| value.clone())
    })
}

fn nonempty(value: Option<OsString>) -> Option<OsString> {
    value.filter(|value| !value.is_empty())
}

fn fail(code: i32, message: impl AsRef<str>) -> ! {
    eprintln!("{}", message.as_ref());
    std::process::exit(code);
}

fn run() -> Result<(), (i32, String)> {
    let project_root = env::current_dir().map_err(|error| (1, error.to_string()))?;

    // Start the bundled Rust watchdog before argument and root checks.
    safeguard::start(&project_root);

    let mut args = env::args_os();
    let program = args.next().unwrap_or_else(|| "run-native".into());
    let mut payload: Vec<OsString> = args.collect();
    let trace = payload.first().is_some_and(|arg| arg == "--trace");
    if trace {
        payload.remove(0);
    }
    if payload.is_empty() {
        return Err((
            2,
            format!(
                "usage: {} [--trace] <program> [args...]",
                Path::new(&program).display()
            ),
        ));
    }

    let root = nonempty(env::var_os("LXRT_ROOT")).unwrap_or_else(|| DEFAULT_ROOT.into());
    let root_path = PathBuf::from(&root);
    let host_root = if root_path.is_absolute() {
        root_path.clone()
    } else {
        project_root.join(&root_path)
    };
    if !host_root.is_dir() {
        return Err((
            1,
            format!("run-native: no ARM64 root at {}", root.to_string_lossy()),
        ));
    }

    let guest = guest_environment(&host_root);
    let home =
        nonempty(effective_env("HOME_IN_GUEST", &guest)).unwrap_or_else(|| DEFAULT_HOME.into());
    let display = nonempty(effective_env("DISPLAY", &guest)).unwrap_or_else(|| ":2".into());
    let guest_path = nonempty(effective_env("PATH_IN_GUEST", &guest))
        .unwrap_or_else(|| DEFAULT_GUEST_PATH.into());

    let mut command = Command::new(project_root.join("build/lxrun"));
    command.current_dir(&project_root);
    if trace {
        command.arg("--trace");
    }
    command.args(payload);
    // Match the shell entrypoint's order: root-provided values are imported
    // first, then run-native's fixed guest environment takes precedence.
    command.envs(guest.iter().map(|(key, value)| (key, value)));
    command
        .env("LXRT_ROOT", &root)
        // SteamFrame's ARM64 client loads ELF DSOs aligned to Linux's 4 KiB
        // page size. On Apple Silicon, the host's 16 KiB granule otherwise
        // leaks through AT_PAGESZ and dlmopen rejects steamui.so before the
        // client UI can appear. LXRT maps guest pages onto host pages.
        .env("LXRT_GUEST_PAGE", "4096")
        .env("TMPDIR", "/tmp")
        .env_remove("LD_LIBRARY_PATH")
        .env("HOME", home)
        .env("OBJC_DISABLE_INITIALIZE_FORK_SAFETY", "YES")
        .env("DISPLAY", display)
        .env_remove("FEX_ROOTFS")
        .env_remove("FEX_GUESTBASE")
        .env("PATH", guest_path);

    Err((
        1,
        format!(
            "run-native: cannot execute {}: {}",
            project_root.join("build/lxrun").display(),
            command.exec()
        ),
    ))
}

fn main() {
    if let Err((code, message)) = run() {
        fail(code, message);
    }
}
