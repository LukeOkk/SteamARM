use std::env;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;

const MARKER: &[u8] = b"SteamARM: answers for atomupd-daemon";
const STUB: &str = r##"#!/bin/sh
# SteamARM: answers for atomupd-daemon (scripts/install-frameroot-atomupd.sh).
# This root is SteamARM's and is updated by SteamARM, not by SteamOS: there
# is no atomupd-daemon and no system D-Bus to reach one. The image's own
# client is /usr/bin/atomupd-manager.lxrt-off.
case "${1:-}" in
    --version) echo "atomupd-manager (SteamARM stand-in)" ;;
    check) echo "No update available" ;;
    get-update-status) echo "idle" ;;
    tracked-variant) echo "steamarm" ;;
    tracked-branch) echo "stable" ;;
    list-variants) echo "steamarm" ;;
    list-branches) echo "stable" ;;
    list-builds) ;;
    status) echo "SteamARM root: updated by SteamARM (scripts/setup.sh)" ;;
    update|custom-update|switch-variant|switch-branch|simulate-update|create-dev-conf)
        echo "This system is updated by SteamARM, not by SteamOS" >&2
        exit 1 ;;
    *) ;;
esac
exit 0
"##;

fn state_root() -> Result<PathBuf, String> {
    if let Some(path) = env::var_os("STEAMARM_STATE") {
        return Ok(PathBuf::from(path));
    }
    let home = PathBuf::from(env::var_os("HOME").ok_or("HOME is not set")?);
    let developer = home.join("Developer/SteamARM-roots");
    if developer.is_dir() || !home.join("SteamARM-roots").is_dir() {
        Ok(developer)
    } else {
        Ok(home.join("SteamARM-roots"))
    }
}

fn contains_marker(bytes: &[u8]) -> bool {
    bytes.windows(MARKER.len()).any(|window| window == MARKER)
}

fn install(root: &PathBuf) -> Result<(), String> {
    let bin = root.join("usr/bin/atomupd-manager");
    if !bin.exists() {
        println!(
            "[install-frameroot-atomupd] no atomupd-manager in {}",
            root.display()
        );
        return Ok(());
    }

    let is_steamarm_stub = fs::read(&bin)
        .map(|contents| contains_marker(&contents))
        .unwrap_or(false);
    let original = PathBuf::from(format!("{}.lxrt-off", bin.display()));
    if !is_steamarm_stub {
        fs::rename(&bin, &original)
            .map_err(|error| format!("cannot preserve {}: {error}", bin.display()))?;
    }

    let temp = PathBuf::from(format!("{}.new.{}", bin.display(), std::process::id()));
    fs::write(&temp, STUB.as_bytes()).map_err(|error| error.to_string())?;
    fs::set_permissions(&temp, fs::Permissions::from_mode(0o755))
        .map_err(|error| error.to_string())?;
    if let Err(error) = fs::rename(&temp, &bin) {
        let _ = fs::remove_file(&temp);
        return Err(format!("cannot install {}: {error}", bin.display()));
    }

    println!(
        "[install-frameroot-atomupd] atomupd-manager answered by SteamARM in {} (the image's is atomupd-manager.lxrt-off)",
        root.display()
    );
    Ok(())
}

fn run() -> Result<(), String> {
    let args = env::args_os().skip(1).collect::<Vec<_>>();
    if args.len() > 1 {
        return Err("usage: install-frameroot-atomupd.sh [ROOT]".into());
    }
    let root = match args.first() {
        Some(path) => PathBuf::from(path),
        None => state_root()?.join("arm64root"),
    };
    install(&root)
}

fn main() {
    if let Err(error) = run() {
        eprintln!("[install-frameroot-atomupd] {error}");
        std::process::exit(1);
    }
}
