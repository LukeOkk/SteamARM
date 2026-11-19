#[path = "compat_path.rs"]
mod compat_path;

use std::env;
use std::os::unix::process::CommandExt;
use std::process::{self, Command};

fn run() -> Result<(), String> {
    let paths = compat_path::configure()?;
    let manager = env::current_exe()
        .map_err(|error| format!("cannot locate Steam FEX server manager wrapper: {error}"))?
        .with_file_name("FEXServerManager");
    if !manager.is_file() {
        return Err(format!(
            "Valve FEXServerManager is missing beside the SteamARM wrapper: {}",
            manager.display()
        ));
    }

    eprintln!(
        "SteamARM FEX server: STEAM_COMPAT_DATA_PATH={}, original={}, FEX_APP_DATA_LOCATION={}, FEX_SERVERSOCKETPATH={}",
        paths.alias.display(),
        paths.original.display(),
        paths.fex_data.display(),
        paths.socket.display()
    );
    let error = Command::new(&manager).args(env::args_os().skip(1)).exec();
    Err(format!(
        "cannot exec Valve FEXServerManager at {}: {error}",
        manager.display()
    ))
}

fn main() {
    if let Err(error) = run() {
        eprintln!("steamarm-fexserver-manager: {error}");
        process::exit(126);
    }
}
