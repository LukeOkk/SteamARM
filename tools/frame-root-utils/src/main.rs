use std::env;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::Command;

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

fn sha256_prefix(path: &Path) -> Result<String, String> {
    let output = Command::new("shasum")
        .args(["-a", "256"])
        .arg(path)
        .output()
        .map_err(|error| format!("cannot run shasum: {error}"))?;
    if !output.status.success() {
        return Err(format!("shasum failed for {}", path.display()));
    }
    let digest = String::from_utf8_lossy(&output.stdout)
        .split_whitespace()
        .next()
        .ok_or("shasum returned no digest")?
        .to_owned();
    Ok(digest.chars().take(12).collect())
}

fn install(root: &Path, repo: &Path) -> Result<(), String> {
    if !root.join("usr/share/vulkan").is_dir() {
        return Err(format!("no Steam Frame root at {}", root.display()));
    }

    let shim = repo.join("build/libvulkan.so.1");
    if !shim.is_file() {
        return Err("no build/libvulkan.so.1 (make shim)".into());
    }
    let loader = root.join("usr/lib/libvulkan.so.1");
    if !loader.is_file() {
        return Err(format!(
            "no Vulkan loader in {}",
            loader.parent().unwrap().display()
        ));
    }

    let library = root.join("usr/lib/libvulkan_steamarm.so");
    fs::copy(&shim, &library)
        .map_err(|error| format!("cannot install {}: {error}", library.display()))?;
    fs::set_permissions(&library, fs::Permissions::from_mode(0o755))
        .map_err(|error| format!("cannot set permissions on {}: {error}", library.display()))?;

    let icd_dir = root.join("usr/share/vulkan/icd.d");
    fs::create_dir_all(&icd_dir).map_err(|error| error.to_string())?;
    let manifest = icd_dir.join("steamarm_icd.aarch64.json");
    let manifest_tmp = icd_dir.join(format!(
        ".steamarm_icd.aarch64.json.new.{}",
        std::process::id()
    ));
    fs::write(
        &manifest_tmp,
        b"{\n    \"file_format_version\": \"1.0.1\",\n    \"ICD\": {\n        \"library_path\": \"/usr/lib/libvulkan_steamarm.so\",\n        \"library_arch\": \"64\",\n        \"api_version\": \"1.4.0\"\n    }\n}\n",
    )
    .map_err(|error| error.to_string())?;
    fs::rename(&manifest_tmp, &manifest).map_err(|error| error.to_string())?;

    let freedreno = icd_dir.join("freedreno_icd.aarch64.json");
    if freedreno.is_file() {
        let disabled_dir = root.join("usr/share/vulkan/icd.d.lxrt-off");
        fs::create_dir_all(&disabled_dir).map_err(|error| error.to_string())?;
        fs::rename(&freedreno, disabled_dir.join("freedreno_icd.aarch64.json"))
            .map_err(|error| format!("cannot disable freedreno ICD: {error}"))?;
        println!(
            "[install-frameroot-vulkan] freedreno ICD moved to usr/share/vulkan/icd.d.lxrt-off"
        );
    }

    let digest = sha256_prefix(&shim)?;
    println!(
        "[install-frameroot-vulkan] shim installed as an ICD in {} ({digest})",
        root.display()
    );
    Ok(())
}

fn run() -> Result<(), String> {
    let args = env::args_os().skip(1).collect::<Vec<_>>();
    if args.len() > 1 {
        return Err("usage: install-frameroot-vulkan.sh [ROOT]".into());
    }
    let repo = env::current_dir().map_err(|error| error.to_string())?;
    let root = match args.first() {
        Some(path) => PathBuf::from(path),
        None => state_root()?.join("arm64root"),
    };
    install(&root, &repo)
}

fn main() {
    if let Err(error) = run() {
        eprintln!("[install-frameroot-vulkan] {error}");
        std::process::exit(1);
    }
}
