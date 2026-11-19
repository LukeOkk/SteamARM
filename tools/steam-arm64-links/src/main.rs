use std::env;
use std::fmt::Display;
use std::fs;
use std::io;
use std::os::unix::fs::symlink;
use std::path::{Path, PathBuf};

const DEFAULT_HOME: &str = "/tmp/armhome";
const STEAM_GUEST: &str = ".local/share/Steam";

fn fail(message: impl Display) -> ! {
    eprintln!("steam-arm64-links: {message}");
    std::process::exit(1);
}

fn guest_path(root: &Path, path: &Path) -> PathBuf {
    root.join(path.strip_prefix("/").unwrap_or(path))
}

fn set_link(dot: &Path, name: &str, target: &Path) -> io::Result<()> {
    let link = dot.join(name);
    match fs::symlink_metadata(&link) {
        Ok(metadata) if !metadata.file_type().is_symlink() => {
            eprintln!(
                "steam-arm64-links: {} exists and is not a link: left alone",
                link.display()
            );
            return Ok(());
        }
        Ok(_) => {
            if fs::read_link(&link).ok().as_deref() == Some(target) {
                return Ok(());
            }
            fs::remove_file(&link)?;
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error),
    }
    symlink(target, &link)
}

fn run() -> Result<(), String> {
    let mut args = env::args_os().skip(1);
    let Some(root) = args.next() else {
        return Err("usage: steam-arm64-links.sh ROOT [GUEST_HOME]".into());
    };
    let root = PathBuf::from(root);
    let home = args
        .next()
        .filter(|value| !value.is_empty())
        .unwrap_or_else(|| DEFAULT_HOME.into());
    let home = PathBuf::from(home);
    let steam = home.join(STEAM_GUEST);

    // Valve's ARM64 launcher used steamrtarm64, falling back to linuxarm64;
    // sdkarm64 used linuxarm64, falling back to steamrtarm64.
    let runtime = if guest_path(&root, &steam.join("steamrtarm64")).is_dir() {
        "steamrtarm64"
    } else {
        "linuxarm64"
    };
    let sdk = if guest_path(&root, &steam.join("linuxarm64")).is_dir() {
        "linuxarm64"
    } else {
        "steamrtarm64"
    };
    if !guest_path(&root, &steam.join(runtime)).is_dir() {
        return Ok(());
    }

    let dot = guest_path(&root, &home.join(".steam"));
    fs::create_dir_all(&dot)
        .map_err(|error| format!("cannot create {}: {error}", dot.display()))?;
    for (name, target) in [
        ("steam", steam.clone()),
        ("root", steam.clone()),
        ("sdk32", steam.join("linux32")),
        ("sdk64", steam.join("linux64")),
        ("sdkarm64", steam.join(sdk)),
        ("binarm64", steam.join(runtime)),
        ("bin32", steam.join("ubuntu12_32")),
        ("bin64", steam.join("ubuntu12_64")),
    ] {
        set_link(&dot, name, &target).map_err(|error| {
            format!(
                "cannot link {} -> {}: {error}",
                dot.join(name).display(),
                target.display()
            )
        })?;
    }
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        fail(error);
    }
}
