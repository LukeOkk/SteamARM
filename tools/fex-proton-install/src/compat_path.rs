use std::env;
use std::fs;
use std::os::unix::fs::symlink;
use std::path::{Path, PathBuf};

const ORIGINAL_PATH_ENV: &str = "STEAMARM_ORIGINAL_COMPAT_DATA_PATH";

fn nonempty_env(names: &[&str]) -> Option<String> {
    names
        .iter()
        .find_map(|name| env::var(name).ok().filter(|value| !value.trim().is_empty()))
}

fn app_id(path: &Path) -> Option<String> {
    nonempty_env(&[
        "STEAM_COMPAT_APP_ID",
        "SteamAppId",
        "SteamAppID",
        "STEAMAPPID",
        "SteamOverlayGameId",
        "SteamGameId",
    ])
    .filter(|value| value.bytes().all(|byte| byte.is_ascii_digit()))
    .or_else(|| {
        path.file_name()
            .and_then(|name| name.to_str())
            .filter(|value| value.bytes().all(|byte| byte.is_ascii_digit()))
            .map(str::to_owned)
    })
}

fn steam_root() -> Result<PathBuf, String> {
    if let Some(path) = nonempty_env(&[
        "STEAM_COMPAT_CLIENT_INSTALL_PATH",
        "STEAM_CLIENT_INSTALL_PATH",
        "STEAMROOT",
    ]) {
        return Ok(PathBuf::from(path));
    }
    let home = env::var_os("HOME")
        .map(PathBuf::from)
        .ok_or_else(|| "Steam did not provide a compatdata path or HOME".to_owned())?;
    if let Some(data_home) = env::var_os("XDG_DATA_HOME") {
        return Ok(PathBuf::from(data_home).join("Steam"));
    }
    Ok(home.join(".local/share/Steam"))
}

fn original_compat_data_path() -> Result<PathBuf, String> {
    if let Some(path) = nonempty_env(&[ORIGINAL_PATH_ENV, "STEAM_COMPAT_DATA_PATH"]) {
        return Ok(PathBuf::from(path));
    }
    let id = nonempty_env(&[
        "STEAM_COMPAT_APP_ID",
        "SteamAppId",
        "SteamAppID",
        "STEAMAPPID",
        "SteamOverlayGameId",
        "SteamGameId",
    ])
    .filter(|value| value.bytes().all(|byte| byte.is_ascii_digit()))
    .ok_or_else(|| "Steam did not provide a numeric app ID or compatdata path".to_owned())?;
    Ok(steam_root()?.join("steamapps/compatdata").join(id))
}

fn stable_hash(path: &Path) -> String {
    let mut hash = 0xcbf29ce484222325_u64;
    for byte in path.as_os_str().to_string_lossy().bytes() {
        hash ^= u64::from(byte);
        hash = hash.wrapping_mul(0x100000001b3);
    }
    format!("{hash:016x}")
}

fn relative_link_target(base: &Path, path: &Path) -> Result<PathBuf, String> {
    let base_components: Vec<_> = base.components().collect();
    let path_components: Vec<_> = path.components().collect();
    let common = base_components
        .iter()
        .zip(path_components.iter())
        .take_while(|(left, right)| left == right)
        .count();
    let mut target = PathBuf::new();
    for _ in common..base_components.len() {
        target.push("..");
    }
    for component in path_components.iter().skip(common) {
        target.push(component.as_os_str());
    }
    if target.as_os_str().is_empty() {
        target.push(".");
    }
    Ok(target)
}

fn short_alias(base: &Path, original: &Path, id: &str) -> Result<PathBuf, String> {
    if !base.is_absolute() || !base.is_dir() {
        return Err(format!(
            "Steam install path is not an available absolute directory: {}",
            base.display()
        ));
    }
    if !original.is_absolute() {
        return Err(format!(
            "Steam compatdata path is not absolute: {}",
            original.display()
        ));
    }
    if !original.is_dir() {
        return Err(format!(
            "Steam compatdata path is unavailable: {}",
            original.display()
        ));
    }
    let hash = stable_hash(original);
    let alias = base.join(format!(".fex-{id}-{}", &hash[..8]));
    let relative_target = relative_link_target(base, original)?;
    match fs::symlink_metadata(&alias) {
        Ok(metadata) if metadata.file_type().is_symlink() => {
            let target = fs::read_link(&alias)
                .map_err(|error| format!("cannot read {}: {error}", alias.display()))?;
            if target != relative_target {
                let resolved_target = if target.is_absolute() {
                    target
                } else {
                    alias.parent().unwrap_or(base).join(target)
                };
                let same_target = fs::canonicalize(&resolved_target)
                    .ok()
                    .zip(fs::canonicalize(original).ok())
                    .is_some_and(|(left, right)| left == right);
                if !same_target {
                    return Err(format!(
                        "SteamARM short-path collision at {}",
                        alias.display()
                    ));
                }
                fs::remove_file(&alias)
                    .map_err(|error| format!("cannot refresh {}: {error}", alias.display()))?;
                symlink(&relative_target, &alias)
                    .map_err(|error| format!("cannot create {}: {error}", alias.display()))?;
            }
        }
        Ok(_) => {
            return Err(format!(
                "SteamARM short-path is occupied: {}",
                alias.display()
            ));
        }
        Err(error) if error.kind() == std::io::ErrorKind::NotFound => {
            symlink(&relative_target, &alias)
                .map_err(|error| format!("cannot create {}: {error}", alias.display()))?;
        }
        Err(error) => return Err(format!("cannot inspect {}: {error}", alias.display())),
    }
    Ok(alias)
}

fn current_uid() -> Result<String, String> {
    let status = fs::read_to_string("/proc/self/status")
        .map_err(|error| format!("cannot read current Linux UID: {error}"))?;
    status
        .lines()
        .find_map(|line| line.strip_prefix("Uid:"))
        .and_then(|line| line.split_whitespace().next())
        .filter(|uid| uid.bytes().all(|byte| byte.is_ascii_digit()))
        .map(str::to_owned)
        .ok_or_else(|| "cannot determine current Linux UID for FEX socket".to_owned())
}

#[allow(dead_code)] // Each Rust launcher uses the path details it needs.
pub struct Paths {
    pub original: PathBuf,
    pub alias: PathBuf,
    pub fex_data: PathBuf,
    pub socket: PathBuf,
}

pub struct CompatPaths {
    pub original: PathBuf,
    pub alias: PathBuf,
    pub fex_data: PathBuf,
}

pub fn configure_compat_paths() -> Result<CompatPaths, String> {
    let original = original_compat_data_path()?;
    let id =
        app_id(&original).ok_or_else(|| "Steam FEX manager has no numeric app ID".to_owned())?;
    let steam = steam_root()?;
    let alias = short_alias(&steam, &original, &id)?;
    let fex_data = alias.join("fex-emu");

    env::set_var(ORIGINAL_PATH_ENV, &original);
    env::set_var("STEAM_COMPAT_DATA_PATH", &alias);
    env::set_var("FEX_APP_DATA_LOCATION", &fex_data);
    env::set_var("FEX_APP_CONFIG_LOCATION", &fex_data);

    Ok(CompatPaths {
        original,
        alias,
        fex_data,
    })
}

pub fn configure() -> Result<Paths, String> {
    let compat = configure_compat_paths()?;
    let socket = compat
        .fex_data
        .join("Server")
        .join(format!("{}.FEXServer.Socket", current_uid()?));
    env::set_var("FEX_SERVERSOCKETPATH", &socket);

    Ok(Paths {
        original: compat.original,
        alias: compat.alias,
        fex_data: compat.fex_data,
        socket,
    })
}
