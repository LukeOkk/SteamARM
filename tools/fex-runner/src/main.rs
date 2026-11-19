use std::env;
use std::ffi::{OsStr, OsString};
use std::fs::{self, OpenOptions};
use std::io;
use std::os::unix::fs::{MetadataExt, PermissionsExt};
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::Duration;

#[path = "../../common/safeguard.rs"]
mod safeguard;

const DEFAULT_ROOT: &str = "/tmp/lxrt-steamroot";
const FRAME_ROOT: &str = "/tmp/lxrt-root";
const DEFAULT_ROOTFS: &str = "/tmp/fexhome/.local/share/fex-emu/RootFS/Ubuntu_24_04";

fn project_root() -> PathBuf {
    if let Some(root) = env::var_os("STEAMARM_PROJECT_ROOT") {
        return PathBuf::from(root);
    }
    if let Ok(current) = env::current_dir() {
        if current.join("scripts/run-fex.sh").is_file() {
            return current;
        }
    }
    if let Ok(executable) = env::current_exe() {
        for ancestor in executable.ancestors() {
            if ancestor.join("scripts/run-fex.sh").is_file() {
                return ancestor.to_path_buf();
            }
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

fn run_quiet(root: &Path, script: &str, args: &[&OsStr]) -> io::Result<bool> {
    let status = Command::new(root.join(script))
        .args(args)
        .current_dir(root)
        .stdout(Stdio::null())
        .status()?;
    Ok(status.success())
}

fn command_text(program: &str, arg: &str) -> io::Result<String> {
    let output = Command::new(program).arg(arg).output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!("{program} {arg} failed")));
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

fn executable(path: &Path) -> bool {
    fs::metadata(path)
        .map(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
        .unwrap_or(false)
}

fn running(pattern: &str) -> bool {
    Command::new("pgrep")
        .args(["-f", pattern])
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|status| status.success())
        .unwrap_or(false)
}

fn inode(path: &Path) -> Option<u64> {
    fs::metadata(path).ok().map(|metadata| metadata.ino())
}

fn configure_environment(state: &Path) {
    env::set_var("STEAMARM_STATE", state);
    env::set_var(
        "LXRT_ROOT",
        nonempty_env("LXRT_ROOT").unwrap_or_else(|| DEFAULT_ROOT.into()),
    );
    env::set_var("TMPDIR", "/tmp");
    env::remove_var("LD_LIBRARY_PATH");
    env::set_var("HOME", "/tmp/fexhome");
    env::set_var("OBJC_DISABLE_INITIALIZE_FORK_SAFETY", "YES");
    if env::var_os("FEX_ROOTFS").is_none() {
        env::set_var("FEX_ROOTFS", DEFAULT_ROOTFS);
    }
    if nonempty_env("FEX_SERVER_ROOTFS").is_none() {
        env::set_var("FEX_SERVER_ROOTFS", DEFAULT_ROOTFS);
    }
    env::set_var("FEX_SILENTLOG", "0");
    env::set_var("FEX_OUTPUTLOG", "stderr");
    env::set_var(
        "DISPLAY",
        nonempty_env("DISPLAY").unwrap_or_else(|| ":2".into()),
    );
    env::set_var(
        "FEX_SMCCHECKS",
        nonempty_env("FEX_SMCCHECKS").unwrap_or_else(|| "mtrack".into()),
    );
    env::set_var("FEX_GUESTBASE", "1");
}

fn fex_locations(home: &Path) -> (PathBuf, OsString, PathBuf) {
    let mut fex_bin = nonempty_env("FEXBIN").unwrap_or_else(|| "FEX-gb".into());
    let mut fex_dir = PathBuf::from(
        nonempty_env("FEXDIR")
            .unwrap_or_else(|| PathBuf::from(FRAME_ROOT).join("usr/bin").into_os_string()),
    );
    let mut build_dir = nonempty_env("STEAMARM_BUILD")
        .map(PathBuf::from)
        .unwrap_or_default();
    if build_dir.as_os_str().is_empty() {
        let preferred = home.join("Developer/SteamARM-build");
        let legacy = home.join("SteamARM-build");
        build_dir = if preferred.is_dir() || !legacy.is_dir() {
            preferred
        } else {
            legacy
        };
    }

    let current_client = fex_dir.join(&fex_bin);
    if (!executable(&current_client) || !executable(&fex_dir.join("FEXServer")))
        && executable(&build_dir.join("out/FEX"))
        && executable(&build_dir.join("out/FEXServer"))
    {
        fex_dir = build_dir.join("out");
        if !executable(&fex_dir.join(&fex_bin)) && fex_bin == OsStr::new("FEX-gb") {
            fex_bin = "FEX".into();
        }
    }
    (fex_dir, fex_bin, build_dir)
}

fn start_server(root: &Path, state: &Path, fex_dir: &Path) -> io::Result<bool> {
    let server = fex_dir.join("FEXServer");
    let pattern = format!("lxrun {}", server.display());
    if running(&pattern) {
        return Ok(true);
    }

    let uid = command_text("id", "-u")?;
    let socket = PathBuf::from(format!("/tmp/lxrt-abstract-{uid}/{uid}.FEXServer.Socket"));
    let old_inode = inode(&socket);
    let log_path = state.join("logs/fexserver.log");
    let log_out = OpenOptions::new()
        .create(true)
        .truncate(true)
        .write(true)
        .open(&log_path)?;
    let log_err = log_out.try_clone()?;
    let session_bin = nonempty_env("STEAMARM_SESSION_BIN")
        .map(PathBuf::from)
        .unwrap_or_else(|| root.join("build/steamarm-session"));
    let server_rootfs = env::var_os("FEX_SERVER_ROOTFS").unwrap_or_else(|| DEFAULT_ROOTFS.into());
    let server_root = nonempty_env("FEX_SERVER_LXRT_ROOT").unwrap_or_else(|| DEFAULT_ROOT.into());

    Command::new(session_bin)
        .arg("detach")
        .arg("./build/lxrun")
        .arg(&server)
        .args(["--foreground", "--persistent=0"])
        .current_dir(root)
        .env("TMPDIR", "/tmp")
        .env("LXRT_ROOT", server_root)
        .env("FEX_ROOTFS", server_rootfs)
        .env("PYTHONCOERCECLOCALE", "0")
        .env("STEAMARM_SESSION_PY", "1")
        .stdin(Stdio::null())
        .stdout(Stdio::from(log_out))
        .stderr(Stdio::from(log_err))
        .spawn()?;

    for _ in 0..80 {
        let current_inode = inode(&socket);
        if current_inode.is_some() && current_inode != old_inode && running(&pattern) {
            thread::sleep(Duration::from_millis(100));
            return Ok(true);
        }
        thread::sleep(Duration::from_millis(100));
    }
    Ok(false)
}

fn usage() -> ! {
    eprintln!("usage: scripts/run-fex.sh [--trace] <program> [args...]");
    std::process::exit(2)
}

fn run() -> i32 {
    let root = project_root();
    if env::set_current_dir(&root).is_err() {
        eprintln!("run-fex: cannot enter project root {}", root.display());
        return 1;
    }
    let host_home = env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    let state = state_root(&host_home);
    env::set_var("STEAMARM_STATE", &state);

    if !run_quiet(&root, "scripts/env-links.sh", &[state.as_os_str()]).unwrap_or(false) {
        eprintln!("run-fex: cannot prepare Steam Frame root links");
        return 1;
    }
    let logs = state.join("logs");
    if let Err(error) = fs::create_dir_all(&logs) {
        eprintln!("run-fex: cannot create {}: {error}", logs.display());
        return 1;
    }
    safeguard::start(&root);

    let mut args = env::args_os().skip(1).collect::<Vec<_>>();
    let trace = args.first().is_some_and(|arg| arg == OsStr::new("--trace"));
    if trace {
        args.remove(0);
    }
    if args.is_empty() {
        usage();
    }

    configure_environment(&state);
    let (fex_dir, fex_bin, _) = fex_locations(&host_home);
    let client = fex_dir.join(&fex_bin);
    let server = fex_dir.join("FEXServer");
    if !executable(&client) {
        eprintln!("run-fex: FEX not found at {}", client.display());
        return 1;
    }
    if !executable(&server) {
        eprintln!("run-fex: FEXServer not found at {}", server.display());
        return 1;
    }
    env::set_var("FEXBIN", fex_bin);
    env::set_var("FEXDIR", &fex_dir);

    match start_server(&root, &state, &fex_dir) {
        Ok(true) => {}
        Ok(false) => {
            eprintln!(
                "run-fex: FEXServer did not start (see {})",
                logs.join("fexserver.log").display()
            );
            return 1;
        }
        Err(error) => {
            eprintln!("run-fex: cannot start FEXServer: {error}");
            return 1;
        }
    }

    let mut command = Command::new(root.join("build/lxrun"));
    if trace {
        command.arg("--trace");
    }
    command.arg(client).args(args);
    let error = command.exec();
    eprintln!("run-fex: cannot execute lxrun: {error}");
    1
}

fn main() {
    std::process::exit(run());
}
