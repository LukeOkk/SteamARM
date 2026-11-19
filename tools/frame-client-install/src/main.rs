use std::env;
use std::error::Error;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::PathBuf;
use std::process::{Command, Stdio};

fn run(root: PathBuf) -> Result<(), Box<dyn Error>> {
    let archive = root.join("usr/lib/steam/steam.tar.zst");
    let steam_home = root.join("tmp/armhome/.local/share/Steam");
    let client = steam_home.join("steamrtarm64/steam");
    let marker = steam_home.join(".steamarm-frame-client");

    if marker.is_file() && executable(&client) {
        println!(
            "Steam Frame ARM64 client already installed at {}",
            client.display()
        );
        return Ok(());
    }
    if executable(&client) {
        println!(
            "Steam Frame ARM64 client already present at {}",
            client.display()
        );
        return Ok(());
    }
    if !archive.is_file() {
        return Err(format!(
            "Steam client archive missing from the licensed Frame image: {}",
            archive.display()
        )
        .into());
    }

    fs::create_dir_all(&steam_home)?;
    println!("Installing the Steam client from the local Steam Frame image...");

    let mut zstd = Command::new("zstd")
        .arg("-dc")
        .arg(&archive)
        .stdout(Stdio::piped())
        .stderr(Stdio::inherit())
        .spawn()?;
    let decoded = zstd.stdout.take().ok_or("could not open zstd output")?;
    let mut tar = Command::new("tar")
        .args(["-xf", "-", "-C"])
        .arg(&steam_home)
        .stdin(decoded)
        .stdout(Stdio::inherit())
        .stderr(Stdio::inherit())
        .spawn()?;

    let tar_status = tar.wait()?;
    let zstd_status = zstd.wait()?;
    if !zstd_status.success() || !tar_status.success() {
        return Err(
            format!("client extraction failed (zstd: {zstd_status}, tar: {tar_status})").into(),
        );
    }
    if !executable(&client) {
        return Err(format!("archive did not provide an executable {}", client.display()).into());
    }

    let build_id = fs::read_to_string(root.join("etc/os-release"))
        .ok()
        .and_then(|s| {
            s.lines()
                .find_map(|line| line.strip_prefix("BUILD_ID=").map(str::to_owned))
        })
        .unwrap_or_else(|| "unknown".to_owned());
    fs::write(marker, format!("Steam Frame BUILD_ID={build_id}\n"))?;
    println!("Steam Frame ARM64 client installed at {}", client.display());
    Ok(())
}

fn executable(path: &std::path::Path) -> bool {
    fs::metadata(path)
        .map(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
        .unwrap_or(false)
}

fn main() {
    let mut args = env::args_os().skip(1);
    let Some(root) = args.next() else {
        eprintln!("usage: steamarm-frame-client-install ROOT");
        std::process::exit(2);
    };
    if args.next().is_some() {
        eprintln!("usage: steamarm-frame-client-install ROOT");
        std::process::exit(2);
    }
    if let Err(error) = run(PathBuf::from(root)) {
        eprintln!("steamarm-frame-client-install: {error}");
        std::process::exit(1);
    }
}
