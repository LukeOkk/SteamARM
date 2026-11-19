use std::env;
use std::ffi::OsString;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Component, Path, PathBuf};
use std::process::{Command, ExitCode, Stdio};

const DEFAULT_URL: &str = "https://repo.steampowered.com/steam/archive/stable/steam_latest.deb";
const BOOTSTRAP_MEMBER: &str = "usr/lib/steam/bootstraplinux_ubuntu12_32.tar.xz";

struct Config {
    root: PathBuf,
    build: PathBuf,
    url: String,
}

struct TempDir(PathBuf);

impl Drop for TempDir {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn home() -> PathBuf {
    env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"))
}

fn selected_build_dir() -> PathBuf {
    if let Some(value) = env::var_os("STEAMARM_BUILD").filter(|v| !v.is_empty()) {
        return PathBuf::from(value);
    }
    let newer = home().join("Developer/SteamARM-build");
    let legacy = home().join("SteamARM-build");
    if newer.is_dir() || !legacy.is_dir() {
        newer
    } else {
        legacy
    }
}

fn config() -> Config {
    Config {
        root: env::var_os("LXRT_ROOT")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("/tmp/lxrt-steamroot")),
        build: selected_build_dir(),
        url: env::var("STEAM_DEB_URL").unwrap_or_else(|_| DEFAULT_URL.into()),
    }
}

fn log(message: impl AsRef<str>) {
    println!("[install-steam] {}", message.as_ref());
}

fn error(message: impl AsRef<str>) -> ! {
    eprintln!("[install-steam] error: {}", message.as_ref());
    std::process::exit(1)
}

fn run(program: &str, args: &[OsString], cwd: Option<&Path>) -> Result<(), String> {
    let mut command = Command::new(program);
    command.args(args);
    if let Some(path) = cwd {
        command.current_dir(path);
    }
    let status = command
        .status()
        .map_err(|e| format!("cannot run {program}: {e}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("{program} exited with {status}"))
    }
}

fn run_capture(program: &str, args: &[OsString]) -> Result<Vec<u8>, String> {
    let output = Command::new(program)
        .args(args)
        .output()
        .map_err(|e| format!("cannot run {program}: {e}"))?;
    if output.status.success() {
        Ok(output.stdout)
    } else {
        Err(format!(
            "{program} exited with {}: {}",
            output.status,
            String::from_utf8_lossy(&output.stderr).trim()
        ))
    }
}

fn frame_root(root: &Path) -> Result<PathBuf, String> {
    let real = fs::canonicalize(root)
        .map_err(|e| format!("no Steam Frame root at {}: {e}", root.display()))?;
    if !real.join(".lxrt-steamroot-frame-base").is_file() {
        return Err(format!(
            "{} is not based on the Steam Frame Arch root (missing .lxrt-steamroot-frame-base)",
            real.display()
        ));
    }
    if !real.join("usr").is_dir() {
        return Err(format!(
            "no Steam root at {} (run setup roots first)",
            real.display()
        ));
    }
    Ok(real)
}

fn safe_archive(archive: &Path) -> Result<(), String> {
    let listing = run_capture("tar", &["-tf".into(), archive.as_os_str().to_owned()])?;
    for raw in String::from_utf8_lossy(&listing).lines() {
        let member = raw.strip_prefix("./").unwrap_or(raw);
        if member.is_empty() {
            continue;
        }
        let path = Path::new(member);
        if path.components().any(|component| {
            matches!(
                component,
                Component::ParentDir | Component::RootDir | Component::Prefix(_)
            )
        }) {
            return Err(format!("unsafe path in Valve bootstrap archive: {raw}"));
        }
    }
    Ok(())
}

fn create_temp_dir() -> Result<TempDir, String> {
    let output = Command::new("mktemp")
        .arg("-d")
        .stdout(Stdio::piped())
        .output()
        .map_err(|e| format!("cannot create temporary directory: {e}"))?;
    if !output.status.success() {
        return Err(format!(
            "mktemp failed: {}",
            String::from_utf8_lossy(&output.stderr).trim()
        ));
    }
    let path = PathBuf::from(String::from_utf8_lossy(&output.stdout).trim());
    Ok(TempDir(path))
}

fn install(config: &Config) -> Result<(), String> {
    let root = frame_root(&config.root)?;
    let destination = root.join("tmp/fexhome/.local/share/Steam");
    if destination.join("steam.sh").is_file()
        && fs::metadata(destination.join("steam.sh"))
            .is_ok_and(|metadata| metadata.permissions().mode() & 0o111 != 0)
    {
        log(format!("Steam client already in {}", destination.display()));
        return Ok(());
    }

    let deb_dir = config.build.join("debs");
    fs::create_dir_all(&deb_dir).map_err(|e| format!("create {}: {e}", deb_dir.display()))?;
    let deb = deb_dir.join("steam_latest.deb");
    let partial = deb_dir.join("steam_latest.deb.part");
    log(format!("fetch {}", config.url));
    let status = Command::new("curl")
        .args(["-fsSL", "--retry", "3", "-o"])
        .arg(&partial)
        .arg(&config.url)
        .status()
        .map_err(|e| format!("cannot run curl: {e}"))?;
    if !status.success() {
        let _ = fs::remove_file(&partial);
        return Err("download failed".into());
    }
    fs::rename(&partial, &deb).map_err(|e| format!("install {}: {e}", deb.display()))?;

    let temporary = create_temp_dir()?;
    run(
        "tar",
        &[
            "-xf".into(),
            deb.as_os_str().to_owned(),
            "-C".into(),
            temporary.0.as_os_str().to_owned(),
        ],
        None,
    )
    .map_err(|e| format!("unpack Debian archive: {e}"))?;
    let mut data_archives = fs::read_dir(&temporary.0)
        .map_err(|e| format!("list {}: {e}", temporary.0.display()))?
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|path| {
            path.file_name()
                .is_some_and(|name| name.to_string_lossy().starts_with("data.tar."))
        })
        .collect::<Vec<_>>();
    data_archives.sort();
    let data = data_archives
        .into_iter()
        .next()
        .ok_or_else(|| format!("no data.tar in {}", deb.display()))?;
    let listing = run_capture("tar", &["-tf".into(), data.as_os_str().to_owned()])?;
    let member = String::from_utf8_lossy(&listing)
        .lines()
        .find(|line| line.trim_start_matches("./") == BOOTSTRAP_MEMBER)
        .ok_or_else(|| format!("bootstrap member not found in {}", deb.display()))?
        .to_owned();
    let boot = temporary.0.join("bootstraplinux_ubuntu12_32.tar.xz");
    let status = Command::new("tar")
        .arg("-xOf")
        .arg(&data)
        .arg(member)
        .stdout(fs::File::create(&boot).map_err(|e| format!("create bootstrap: {e}"))?)
        .status()
        .map_err(|e| format!("extract bootstrap member: {e}"))?;
    if !status.success() {
        return Err("bootstrap extraction failed".into());
    }
    safe_archive(&boot)?;
    fs::create_dir_all(&destination)
        .map_err(|e| format!("create {}: {e}", destination.display()))?;
    run(
        "tar",
        &[
            "-xf".into(),
            boot.as_os_str().to_owned(),
            "-C".into(),
            destination.as_os_str().to_owned(),
        ],
        None,
    )
    .map_err(|e| format!("unpack Steam bootstrap: {e}"))?;
    let steam = destination.join("steam.sh");
    if !steam.is_file()
        || !fs::metadata(&steam).is_ok_and(|metadata| metadata.permissions().mode() & 0o111 != 0)
    {
        return Err("bootstrap did not provide executable steam.sh".into());
    }
    log(format!(
        "Steam bootstrap unpacked into {}; it updates itself on first start (scripts/run-steam.sh)",
        destination.display()
    ));
    Ok(())
}

fn main() -> ExitCode {
    let args = env::args().skip(1).collect::<Vec<_>>();
    if args == ["--help"] || args == ["-h"] {
        println!("Usage: install-steam.sh [--help]\nDownloads Valve's current Steam bootstrap into the Steam Frame based Arch root.");
        return ExitCode::SUCCESS;
    }
    if !args.is_empty() {
        eprintln!("Usage: install-steam.sh [--help]");
        return ExitCode::from(2);
    }
    match install(&config()) {
        Ok(()) => ExitCode::SUCCESS,
        Err(message) => error(message),
    }
}
