#[path = "compat_path.rs"]
mod compat_path;

use std::env;
use std::os::unix::process::CommandExt;
use std::path::{Path, PathBuf};
use std::process::{self, Command};

fn run() -> Result<(), String> {
    let paths = compat_path::configure()?;
    // Valve's stock emulator.json leaves FEX_ROOTFS empty. Steam Frame's
    // Arch FEX root is mounted at this stable guest path, so fill it only
    // when the caller has not selected a pressure-vessel interpreter root.
    if env::var_os("FEX_ROOTFS").map_or(true, |value| value.is_empty()) {
        let rootfs = Path::new("/usr/share/guestos/fex-mesa");
        if !rootfs.is_dir() {
            return Err(format!(
                "Steam Frame Arch FEX root is unavailable at {}",
                rootfs.display()
            ));
        }
        env::set_var("FEX_ROOTFS", rootfs);
    }
    if env::var_os("PRESSURE_VESSEL_RUNTIME").is_some() {
        // Keep the Steam Frame FEX root's native x86 library directories in
        // the guest path. pressure-vessel can replace LD_LIBRARY_PATH with
        // aliases that are only visible in its container, while FEX resolves
        // libraries through the shared Arch rootfs (for example libuuid).
        let mut search_paths = vec![
            PathBuf::from("/usr/lib"),
            PathBuf::from("/usr/lib32"),
            PathBuf::from("/usr/lib/pressure-vessel/overrides/lib/x86_64-linux-gnu"),
            PathBuf::from("/usr/lib/pressure-vessel/overrides/lib/i386-linux-gnu"),
        ];
        if let Some(existing) = env::var_os("LD_LIBRARY_PATH") {
            for path in env::split_paths(&existing) {
                if !search_paths.contains(&path) {
                    search_paths.push(path);
                }
            }
        }
        let library_path = env::join_paths(search_paths).map_err(|error| {
            format!("cannot configure pressure-vessel x86 library path: {error}")
        })?;
        env::set_var("LD_LIBRARY_PATH", library_path);
    }
    // pressure-vessel chroots the game and exposes the host filesystem at
    // /run/host. FEX's Steam build derives its socket directory from
    // STEAM_COMPAT_DATA_PATH, so translate only inside that container. The
    // short alias keeps the resulting Unix socket path below sun_path's cap.
    if env::var_os("PRESSURE_VESSEL_RUNTIME").is_some() && Path::new("/run/host").is_dir() {
        let host_compat =
            PathBuf::from("/run/host").join(paths.alias.strip_prefix("/").map_err(|error| {
                format!("cannot map FEX compat path into pressure-vessel: {error}")
            })?);
        let host_fex = host_compat.join("fex-emu");
        env::set_var("STEAM_COMPAT_DATA_PATH", &host_compat);
        env::set_var("FEX_APP_DATA_LOCATION", &host_fex);
        env::set_var("FEX_APP_CONFIG_LOCATION", &host_fex);
    }
    let fex = env::current_exe()
        .map_err(|error| format!("cannot locate SteamARM FEX wrapper: {error}"))?
        .with_file_name("FEX-SteamARM");
    if !fex.is_file() {
        return Err(format!(
            "SteamARM-patched FEX is missing beside the wrapper: {}",
            fex.display()
        ));
    }

    let error = Command::new(&fex).args(env::args_os().skip(1)).exec();
    Err(format!(
        "cannot exec SteamARM-patched FEX at {}: {error}",
        fex.display()
    ))
}

fn main() {
    if let Err(error) = run() {
        eprintln!("steamarm-fex: {error}");
        process::exit(126);
    }
}
