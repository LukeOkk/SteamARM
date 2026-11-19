use std::env;
use std::fs;
use std::io;
use std::os::unix::fs::symlink;
use std::path::{Path, PathBuf};
use std::time::{SystemTime, UNIX_EPOCH};

fn fail(message: impl AsRef<str>) -> ! {
    eprintln!("env-links: {}", message.as_ref());
    std::process::exit(1);
}

fn state_root(args: &[String]) -> Result<PathBuf, String> {
    if args.len() > 1 {
        return Err("usage: steamarm-env-links [roots-dir]".into());
    }
    if let Some(path) = args.first() {
        return Ok(PathBuf::from(path));
    }
    if let Some(path) = env::var_os("STEAMARM_STATE") {
        return Ok(PathBuf::from(path));
    }
    let home = env::var_os("HOME").ok_or("HOME is not set")?;
    let home = PathBuf::from(home);
    let developer = home.join("Developer/SteamARM-roots");
    if developer.is_dir() || !home.join("SteamARM-roots").is_dir() {
        Ok(developer)
    } else {
        Ok(home.join("SteamARM-roots"))
    }
}

fn only_temporary_pulse_tree(root: &Path) -> io::Result<bool> {
    for entry in fs::read_dir(root)? {
        let entry = entry?;
        if entry.file_name() != "tmp" {
            return Ok(false);
        }
        let kind = entry.file_type()?;
        if kind.is_dir() {
            for child in fs::read_dir(entry.path())? {
                if child?.file_name() != "pulse" {
                    return Ok(false);
                }
            }
        }
    }
    Ok(true)
}

fn prepare_link_path(link: &Path) -> io::Result<()> {
    let metadata = match fs::symlink_metadata(link) {
        Ok(metadata) => metadata,
        Err(error) if error.kind() == io::ErrorKind::NotFound => return Ok(()),
        Err(error) => return Err(error),
    };
    if metadata.file_type().is_symlink() {
        fs::remove_file(link)?;
        return Ok(());
    }
    if metadata.is_dir() && only_temporary_pulse_tree(link)? {
        fs::remove_dir_all(link)?;
        return Ok(());
    }
    Err(io::Error::new(
        io::ErrorKind::AlreadyExists,
        format!("{} is a real path; preserving it", link.display()),
    ))
}

fn set_link(target: &Path, link: &Path) -> io::Result<()> {
    prepare_link_path(link)?;
    symlink(target, link)?;
    eprintln!("env-links: {} -> {}", link.display(), target.display());
    Ok(())
}

fn backup_host_home(home: &Path) -> io::Result<PathBuf> {
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    for suffix in 0..1000 {
        let backup = PathBuf::from(format!(
            "/tmp/fexhome.host-backup-{now}-{}-{suffix}",
            std::process::id()
        ));
        if fs::symlink_metadata(&backup).is_err() {
            fs::rename(home, &backup)?;
            return Ok(backup);
        }
    }
    Err(io::Error::new(
        io::ErrorKind::AlreadyExists,
        "cannot choose a backup path for /tmp/fexhome",
    ))
}

fn link_guest_home(state: &Path) -> io::Result<()> {
    let guest_home = state.join("steamroot/tmp/fexhome");
    fs::create_dir_all(&guest_home)?;
    let link = Path::new("/tmp/fexhome");
    match fs::symlink_metadata(link) {
        Ok(metadata) if metadata.file_type().is_symlink() => fs::remove_file(link)?,
        Ok(_) => {
            let backup = backup_host_home(link)?;
            eprintln!(
                "env-links: preserved the pre-existing host /tmp/fexhome at {}",
                backup.display()
            );
        }
        Err(error) if error.kind() == io::ErrorKind::NotFound => {}
        Err(error) => return Err(error),
    }
    symlink(&guest_home, link)?;
    eprintln!("env-links: {} -> {}", link.display(), guest_home.display());
    Ok(())
}

fn run() -> Result<(), String> {
    let args = env::args().skip(1).collect::<Vec<_>>();
    let state = state_root(&args)?;
    let links = [
        (state.join("arm64root"), PathBuf::from("/tmp/lxrt-root")),
        (
            state.join("steamroot"),
            PathBuf::from("/tmp/lxrt-steamroot"),
        ),
        (state.join("samples"), PathBuf::from("/tmp/lxrt-samples")),
    ];
    for (target, link) in &links {
        set_link(target, link).map_err(|error| error.to_string())?;
    }

    for (name, link) in [
        ("arm64root", "/tmp/lxrt-arm64root"),
        ("armroot", "/tmp/lxrt-armroot"),
    ] {
        let target = state.join(name);
        if target.exists() {
            set_link(&target, Path::new(link)).map_err(|error| error.to_string())?;
        }
    }
    link_guest_home(&state).map_err(|error| error.to_string())?;
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        fail(error);
    }
}
