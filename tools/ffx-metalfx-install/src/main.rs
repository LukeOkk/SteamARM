use std::env;
use std::fs;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{self, Command, ExitCode};

const DLLS: [&str; 3] = [
    "amd_fidelityfx_dx12",
    "amd_fidelityfx_vk",
    "amd_fidelityfx_upscaler_dx12",
];
const DEST_RELATIVE: &str = "opt/steamarm/wine";
const HELP: &str =
    "Install SteamARM's FidelityFX DLLs and MetalFX wrapper in the x86 Steam root.\n\
\n\
Usage:\n\
  install-ffx-metalfx.sh [--root DIR] [--dry-run]\n\
  install-ffx-metalfx.sh --uninstall [--root DIR]\n\
\n\
The default root is LXRT_ROOT, or /tmp/lxrt-steamroot.\n\
The installer and runtime wrapper are Rust; Cargo uses no external crates.\n";

#[derive(Default)]
struct Options {
    root: Option<PathBuf>,
    dry_run: bool,
    uninstall: bool,
}

#[derive(Debug)]
struct Failure {
    code: i32,
    message: String,
}

type Result<T> = std::result::Result<T, Failure>;

fn error(code: i32, message: impl Into<String>) -> Failure {
    Failure {
        code,
        message: message.into(),
    }
}

fn project_root() -> PathBuf {
    env::var_os("STEAMARM_PROJECT_DIR")
        .map(PathBuf::from)
        .or_else(|| env::current_dir().ok())
        .unwrap_or_else(|| PathBuf::from("."))
}

fn executable_dir() -> PathBuf {
    env::current_exe()
        .ok()
        .and_then(|path| path.parent().map(Path::to_path_buf))
        .unwrap_or_else(|| project_root())
}

fn parse_args() -> Result<Option<Options>> {
    let mut options = Options::default();
    let args: Vec<_> = env::args_os().skip(1).collect();
    let mut index = 0;
    while index < args.len() {
        match args[index].to_string_lossy().as_ref() {
            "--root" => {
                index += 1;
                let Some(value) = args.get(index) else {
                    return Err(error(2, "--root needs a directory"));
                };
                options.root = Some(PathBuf::from(value));
            }
            "--dry-run" => options.dry_run = true,
            "--uninstall" => options.uninstall = true,
            "-h" | "--help" => return Ok(None),
            other => return Err(error(2, format!("unknown argument: {other}"))),
        }
        index += 1;
    }
    Ok(Some(options))
}

fn root(options: &Options) -> PathBuf {
    options.root.clone().unwrap_or_else(|| {
        env::var_os("LXRT_ROOT")
            .filter(|path| !path.is_empty())
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("/tmp/lxrt-steamroot"))
    })
}

fn log(message: &str) {
    println!("install-ffx-metalfx: {message}");
}

fn all_dlls(path: &Path) -> bool {
    DLLS.iter().all(|name| {
        path.join("x86_64-windows")
            .join(format!("{name}.dll"))
            .is_file()
    })
}

fn resolve_relative(root: &Path, path: PathBuf) -> PathBuf {
    if path.is_absolute() {
        path
    } else {
        root.join(path)
    }
}

fn find_ffx_build(repo: &Path) -> Option<PathBuf> {
    let mut candidates = Vec::new();
    if let Some(value) = env::var_os("FFX_BUILD") {
        candidates.push(resolve_relative(repo, PathBuf::from(value)));
    } else {
        candidates.push(repo.join("build/ffx-metalfx"));
        candidates.push(executable_dir().join("ffx-metalfx"));
    }
    candidates.into_iter().find(|path| all_dlls(path))
}

fn make_target(repo: &Path, target: &str) -> std::io::Result<std::process::ExitStatus> {
    Command::new("make")
        .args(["-s", target])
        .current_dir(repo)
        .status()
}

fn ffx_build(repo: &Path) -> Result<PathBuf> {
    let specified = env::var_os("FFX_BUILD").is_some();
    if let Some(path) = find_ffx_build(repo) {
        if specified {
            return Ok(path);
        }
        // A packaged app carries the finished DLLs beside this installer.
        if path == executable_dir().join("ffx-metalfx") {
            return Ok(path);
        }
    }

    if !specified {
        let build_result = make_target(repo, "ffx-metalfx");
        if build_result.as_ref().is_ok_and(|status| status.success()) {
            let path = repo.join("build/ffx-metalfx");
            if all_dlls(&path) {
                return Ok(path);
            }
        }
        if let Some(path) = find_ffx_build(repo) {
            log(&format!(
                "warning: make ffx-metalfx failed; installing the DLLs already in {}",
                path.display()
            ));
            return Ok(path);
        }
        return Err(error(
            1,
            format!(
                "could not build FidelityFX DLLs (make ffx-metalfx); missing {}",
                repo.join("build/ffx-metalfx/x86_64-windows/amd_fidelityfx_dx12.dll")
                    .display()
            ),
        ));
    }

    let path = env::var_os("FFX_BUILD")
        .map(PathBuf::from)
        .unwrap_or_default();
    Err(error(
        1,
        format!(
            "missing FidelityFX DLLs in {} (make ffx-metalfx)",
            resolve_relative(repo, path).display()
        ),
    ))
}

fn find_file(repo: &Path, relative: &str, resource_relative: &str) -> Option<PathBuf> {
    [
        repo.join(relative),
        executable_dir().join(resource_relative),
    ]
    .into_iter()
    .find(|path| path.is_file())
}

fn elf_machine(path: &Path) -> i32 {
    let Ok(bytes) = fs::read(path) else {
        return -1;
    };
    if bytes.len() < 20 || bytes.get(..4) != Some(b"\x7fELF") {
        return -1;
    }
    i32::from(u16::from_le_bytes([bytes[18], bytes[19]]))
}

fn find_wrapper(repo: &Path) -> Result<PathBuf> {
    if let Some(path) = env::var_os("STEAMARM_FFX_WRAPPER") {
        let path = resolve_relative(repo, PathBuf::from(path));
        if elf_machine(&path) == 62 {
            return Ok(path);
        }
        return Err(error(
            1,
            format!("{} is not an x86_64 Linux executable", path.display()),
        ));
    }
    let candidates = [
        repo.join("build/steamarm-ffx-x86_64"),
        executable_dir().join("steamarm-ffx-x86_64"),
    ];
    if let Some(path) = candidates.into_iter().find(|path| elf_machine(path) == 62) {
        return Ok(path);
    }
    let status = make_target(repo, "fex-proton-tool")
        .map_err(|e| error(1, format!("cannot build the Rust x86_64 wrapper: {e}")))?;
    if !status.success() {
        return Err(error(
            1,
            "could not build the Rust x86_64 wrapper (make fex-proton-tool)",
        ));
    }
    let path = repo.join("build/steamarm-ffx-x86_64");
    if elf_machine(&path) != 62 {
        return Err(error(
            1,
            format!("missing x86_64 Linux wrapper at {}", path.display()),
        ));
    }
    Ok(path)
}

fn atomic_put(source: &Path, destination: &Path, mode: u32) -> std::io::Result<()> {
    let name = destination
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("artifact");
    let temporary = destination.with_file_name(format!("{name}.new.{}", process::id()));
    let result = (|| {
        fs::copy(source, &temporary)?;
        fs::set_permissions(&temporary, fs::Permissions::from_mode(mode))?;
        fs::rename(&temporary, destination)
    })();
    if result.is_err() {
        let _ = fs::remove_file(&temporary);
    }
    result
}

fn uninstall(dest: &Path, dry_run: bool) -> Result<()> {
    if !dest.is_dir() {
        log(&format!("not installed: {}", dest.display()));
        return Ok(());
    }
    if dry_run {
        log(&format!("would remove {}", dest.display()));
        return Ok(());
    }
    fs::remove_dir_all(dest)
        .map_err(|e| error(1, format!("cannot remove {}: {e}", dest.display())))?;
    if let Some(parent) = dest.parent() {
        let _ = fs::remove_dir(parent);
    }
    log(&format!("removed {}", dest.display()));
    Ok(())
}

fn run() -> Result<()> {
    let Some(options) = parse_args()? else {
        print!("{HELP}");
        return Ok(());
    };
    let root = root(&options);
    if !root.join("usr").is_dir() {
        return Err(error(
            1,
            format!(
                "no x86 Steam root at {} (scripts/setup.sh roots)",
                root.display()
            ),
        ));
    }
    let dest = root.join(DEST_RELATIVE);
    if options.uninstall {
        return uninstall(&dest, options.dry_run);
    }

    let repo = project_root();
    let source = ffx_build(&repo)?;
    let appids = find_file(
        &repo,
        "tools/ffx-metalfx/anticheat-appids.txt",
        "ffx-metalfx/anticheat-appids.txt",
    )
    .ok_or_else(|| error(1, "missing tools/ffx-metalfx/anticheat-appids.txt"))?;
    if options.dry_run {
        log(&format!(
            "would install {} (.dll), anticheat-appids.txt and steamarm-ffx into {}",
            DLLS.join(" "),
            dest.display()
        ));
        return Ok(());
    }
    let wrapper = find_wrapper(&repo)?;
    fs::create_dir_all(dest.join("x86_64-windows"))
        .map_err(|e| error(1, format!("cannot create {}: {e}", dest.display())))?;
    for name in DLLS {
        let file = source.join("x86_64-windows").join(format!("{name}.dll"));
        atomic_put(
            &file,
            &dest.join("x86_64-windows").join(format!("{name}.dll")),
            0o644,
        )
        .map_err(|e| error(1, format!("cannot install {}: {e}", file.display())))?;
    }
    atomic_put(&appids, &dest.join("anticheat-appids.txt"), 0o644)
        .map_err(|e| error(1, format!("cannot install anti-cheat list: {e}")))?;
    atomic_put(&wrapper, &dest.join("steamarm-ffx"), 0o755)
        .map_err(|e| error(1, format!("cannot install Rust wrapper: {e}")))?;
    log(&format!(
        "installed into {}: {} (.dll), anticheat-appids.txt, steamarm-ffx",
        dest.display(),
        DLLS.join(" ")
    ));
    Ok(())
}

fn main() -> ExitCode {
    match run() {
        Ok(()) => ExitCode::SUCCESS,
        Err(failure) => {
            eprintln!("install-ffx-metalfx: error: {}", failure.message);
            ExitCode::from(failure.code as u8)
        }
    }
}
