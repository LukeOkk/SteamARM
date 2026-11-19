use std::env;
use std::os::unix::fs::PermissionsExt;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

fn executable(path: &Path) -> bool {
    path.metadata()
        .is_ok_and(|metadata| metadata.is_file() && metadata.permissions().mode() & 0o111 != 0)
}

pub fn binary(project_root: &Path) -> Option<PathBuf> {
    if let Some(path) = env::var_os("STEAMARM_SAFEGUARD_BIN").map(PathBuf::from) {
        if executable(&path) {
            return Some(path);
        }
    }

    let bundled = env::current_exe().ok().and_then(|path| {
        path.parent()
            .map(|parent| parent.join("steamarm-safeguard"))
    });
    if let Some(path) = bundled.filter(|path| executable(path)) {
        return Some(path);
    }

    let built = project_root.join("build/steamarm-safeguard");
    executable(&built).then_some(built)
}

pub fn start(project_root: &Path) {
    if env::var_os("STEAMARM_NO_SAFEGUARD").is_some_and(|value| !value.is_empty()) {
        return;
    }
    let Some(binary) = binary(project_root) else {
        return;
    };
    let _ = Command::new(binary)
        .arg("start")
        .current_dir(project_root)
        .env("STEAMARM_PROJECT_ROOT", project_root)
        .stdout(Stdio::null())
        .status();
}
