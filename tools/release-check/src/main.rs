use std::env;
use std::process::Command;

const RELEASE_API: &str = "https://api.github.com/repos/LukeOkk/SteamARM/releases/latest";

fn latest_tag() -> Result<String, String> {
    let response = Command::new("/usr/bin/curl")
        .args([
            "--fail",
            "--silent",
            "--show-error",
            "--location",
            "--retry",
            "2",
            "--connect-timeout",
            "15",
            "--max-time",
            "30",
            "-H",
            "Accept: application/vnd.github+json",
            "-H",
            "User-Agent: SteamARM-release-check",
            RELEASE_API,
        ])
        .output()
        .map_err(|e| format!("could not run curl: {e}"))?;
    if !response.status.success() {
        let detail = String::from_utf8_lossy(&response.stderr).trim().to_owned();
        return Err(if detail.is_empty() {
            format!("GitHub request failed ({})", response.status)
        } else {
            detail
        });
    }

    let value: serde_json::Value = serde_json::from_slice(&response.stdout)
        .map_err(|e| format!("GitHub returned invalid release JSON: {e}"))?;
    let tag = value
        .get("tag_name")
        .and_then(serde_json::Value::as_str)
        .ok_or("GitHub response has no release tag")?;
    tag.strip_prefix('v')
        .map(str::to_owned)
        .ok_or_else(|| format!("unexpected release tag: {tag}"))
}

fn version_parts(version: &str) -> Result<Vec<u64>, String> {
    let parts = version
        .split('.')
        .map(|part| {
            part.parse::<u64>()
                .map_err(|_| format!("unsupported version: {version}"))
        })
        .collect::<Result<Vec<_>, _>>()?;
    if parts.len() < 2 || parts.is_empty() {
        return Err(format!("unsupported version: {version}"));
    }
    Ok(parts)
}

fn run() -> Result<(), String> {
    let current = env::args()
        .nth(1)
        .ok_or("usage: steamarm-release-check CURRENT_VERSION")?;
    let latest = latest_tag()?;
    let latest_parts = version_parts(&latest)?;
    let current_parts = version_parts(&current)?;
    if latest_parts > current_parts {
        println!("update\t{latest}");
    } else if current_parts > latest_parts {
        println!("development\t{latest}");
    } else {
        println!("current\t{latest}");
    }
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("error: {error}");
        std::process::exit(1);
    }
}
