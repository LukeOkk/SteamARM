use std::env;
use std::fs;
use std::path::{Path, PathBuf};
use std::process::Command;

const UPDATES: &str =
    "https://dl.fedoraproject.org/pub/fedora/linux/updates/43/Everything/aarch64/Packages";
const RELEASE: &str =
    "https://dl.fedoraproject.org/pub/fedora/linux/releases/43/Everything/aarch64/os/Packages";
const ARCHIVE: &str =
    "https://dl.fedoraproject.org/pub/archive/fedora/linux/updates/43/Everything/aarch64/Packages";
const KOJI: &str = "https://kojipkgs.fedoraproject.org/packages";

fn checksum(path: &Path) -> Result<String, String> {
    let output = Command::new("shasum")
        .args(["-a", "256"])
        .arg(path)
        .output()
        .map_err(|e| format!("could not run shasum: {e}"))?;
    if !output.status.success() {
        return Err("shasum failed".into());
    }
    String::from_utf8_lossy(&output.stdout)
        .split_whitespace()
        .next()
        .map(str::to_owned)
        .ok_or_else(|| "shasum returned no digest".into())
}

fn nvr(file: &str) -> Result<(String, String), String> {
    let stem = file
        .strip_suffix(".aarch64.rpm")
        .ok_or_else(|| format!("not an aarch64 RPM filename: {file}"))?;
    let mut parts = stem.rsplitn(3, '-');
    let release = parts.next().ok_or("missing RPM release")?;
    let version = parts.next().ok_or("missing RPM version")?;
    let package = parts.next().ok_or("missing RPM package name")?;
    if package.is_empty() || version.is_empty() || release.is_empty() {
        return Err(format!("invalid RPM filename: {file}"));
    }
    Ok((version.to_owned(), release.to_owned()))
}

fn fetch(file: &str, source: &str, expected: &str, dest: &Path) -> Result<(), String> {
    if Path::new(file).file_name().and_then(|s| s.to_str()) != Some(file) {
        return Err("RPM filename must not contain a path".into());
    }
    if expected.len() != 64 || !expected.bytes().all(|b| b.is_ascii_hexdigit()) {
        return Err("expected SHA-256 must be 64 hexadecimal characters".into());
    }
    let (version, release) = nvr(file)?;
    let first = file.chars().next().ok_or("empty RPM filename")?;
    let sources = [
        format!("{UPDATES}/{first}/{file}"),
        format!("{RELEASE}/{first}/{file}"),
        format!("{ARCHIVE}/{first}/{file}"),
        format!("{KOJI}/{source}/{version}/{release}/aarch64/{file}"),
    ];
    let part = PathBuf::from(format!("{}.part", dest.display()));

    for url in sources {
        println!("  fetch   {url}");
        let _ = fs::remove_file(&part);
        let status = Command::new("curl")
            .args(["-fsSL", "--retry", "3", "--connect-timeout", "20", "-o"])
            .arg(&part)
            .arg(&url)
            .status()
            .map_err(|e| format!("could not run curl: {e}"))?;
        if !status.success() {
            continue;
        }
        if checksum(&part)? == expected {
            fs::rename(&part, dest)
                .map_err(|e| format!("could not install {}: {e}", dest.display()))?;
            println!("          ok, sha256 {expected}");
            return Ok(());
        }
        println!("          sha256 mismatch, trying the next source");
    }
    let _ = fs::remove_file(&part);
    Err(format!("could not fetch {file} with sha256 {expected}"))
}

fn run() -> Result<(), String> {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.len() != 4 {
        return Err("usage: steamarm-rpm-fetch FILE SOURCE_PACKAGE SHA256 DESTINATION".into());
    }
    fetch(&args[0], &args[1], &args[2], Path::new(&args[3]))
}

fn main() {
    if let Err(error) = run() {
        eprintln!("error: {error}");
        std::process::exit(1);
    }
}
