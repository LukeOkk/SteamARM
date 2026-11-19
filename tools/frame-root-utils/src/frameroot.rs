use std::env;
use std::ffi::{OsStr, OsString};
use std::fs::{self, File};
use std::io::{self, Write};
use std::os::unix::fs::{symlink, MetadataExt};
use std::path::{Path, PathBuf};
use std::process::Command;

const KEEP: [&str; 2] = ["tmp", "opt/apps"];
const CARRIED: &str =
    "# root_swap (scripts/roots.sh): each directory below was named before it moved here";
const GUEST_ENV: &str = "# Guest environment for this root (Steam Frame SteamOS).\n\
__GLX_VENDOR_LIBRARY_NAME=mesa\n\
MESA_LOADER_DRIVER_OVERRIDE=swrast\n\
LIBGL_ALWAYS_INDIRECT=1\n";

type Result<T> = std::result::Result<T, String>;

fn err(context: &str, source: io::Error) -> String {
    format!("{context}: {source}")
}

fn exists(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}

fn symlink_path(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok_and(|m| m.file_type().is_symlink())
}

fn canon(input: &Path, base: &Path) -> Result<PathBuf> {
    if input.as_os_str().is_empty() {
        return Err("empty path".into());
    }
    let mut path = if input.is_absolute() {
        input.to_path_buf()
    } else {
        base.join(input)
    };
    let mut tail: Vec<OsString> = Vec::new();
    loop {
        match fs::symlink_metadata(&path) {
            Ok(meta) if meta.file_type().is_symlink() => {
                if !fs::metadata(&path).is_ok_and(|m| m.is_dir()) {
                    return Err(format!(
                        "{} is a dangling symlink or symlink to a file",
                        path.display()
                    ));
                }
                let mut resolved = path
                    .canonicalize()
                    .map_err(|e| err("cannot resolve path", e))?;
                for name in tail.iter().rev() {
                    resolved.push(name);
                }
                return Ok(resolved);
            }
            Ok(meta) if meta.is_dir() => {
                let mut resolved = path
                    .canonicalize()
                    .map_err(|e| err("cannot resolve path", e))?;
                for name in tail.iter().rev() {
                    resolved.push(name);
                }
                return Ok(resolved);
            }
            Ok(_) | Err(_) => {
                let name = path.file_name().ok_or_else(|| {
                    format!("{} has an unresolved . or .. component", path.display())
                })?;
                if name == OsStr::new(".") || name == OsStr::new("..") {
                    return Err(format!(
                        "{} has an unresolved . or .. component",
                        path.display()
                    ));
                }
                tail.push(name.to_os_string());
                path = path
                    .parent()
                    .ok_or_else(|| "path has no existing directory ancestor".to_owned())?
                    .to_path_buf();
            }
        }
    }
}

fn within(path: &Path, parent: &Path) -> bool {
    path == parent || path.starts_with(parent)
}

fn ancestor(path: &Path) -> Result<PathBuf> {
    let mut current = path;
    loop {
        if exists(current) {
            return Ok(current.to_path_buf());
        }
        current = current
            .parent()
            .ok_or_else(|| format!("{} has no existing ancestor", path.display()))?;
    }
}

fn output(program: &str, args: &[&OsStr]) -> Result<String> {
    let output = Command::new(program)
        .args(args)
        .output()
        .map_err(|e| format!("cannot run {program}: {e}"))?;
    if !output.status.success() {
        return Err(format!("{program} exited with {}", output.status));
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

#[cfg(target_os = "macos")]
fn process_env(pid: i32) -> Option<Vec<Vec<u8>>> {
    use std::ffi::c_void;
    use std::ptr;
    unsafe extern "C" {
        fn sysctl(
            mib: *mut i32,
            len: u32,
            old: *mut c_void,
            oldlen: *mut usize,
            new: *mut c_void,
            newlen: usize,
        ) -> i32;
    }
    unsafe {
        let mut mib = [1, 49, pid]; // CTL_KERN, KERN_PROCARGS2
        let mut size = 0usize;
        if sysctl(
            mib.as_mut_ptr(),
            3,
            ptr::null_mut(),
            &mut size,
            ptr::null_mut(),
            0,
        ) != 0
            || size < 4
        {
            return None;
        }
        let mut bytes = vec![0u8; size];
        if sysctl(
            mib.as_mut_ptr(),
            3,
            bytes.as_mut_ptr().cast(),
            &mut size,
            ptr::null_mut(),
            0,
        ) != 0
        {
            return None;
        }
        bytes.truncate(size);
        let argc = i32::from_ne_bytes(bytes.get(..4)?.try_into().ok()?) as usize;
        let parts = bytes[4..].split(|b| *b == 0).collect::<Vec<_>>();
        let mut i = 1usize;
        while i < parts.len() && parts[i].is_empty() {
            i += 1;
        }
        for _ in 0..argc {
            i += 1;
        }
        let mut values = Vec::new();
        for part in parts.into_iter().skip(i) {
            if part.is_empty() {
                break;
            }
            values.push(part.to_vec());
        }
        Some(values)
    }
}

#[cfg(target_os = "linux")]
fn process_env(pid: i32) -> Option<Vec<Vec<u8>>> {
    fs::read(format!("/proc/{pid}/environ"))
        .ok()
        .map(|b| b.split(|c| *c == 0).map(<[u8]>::to_vec).collect())
}

#[cfg(not(any(target_os = "macos", target_os = "linux")))]
fn process_env(_pid: i32) -> Option<Vec<Vec<u8>>> {
    None
}

fn guests(root: &Path) -> Result<Vec<i32>> {
    let name = env::var("ROOTS_GUEST_NAME").unwrap_or_else(|_| "lxrun".into());
    let listing = Command::new("ps")
        .args(["-Ao", "pid=,comm="])
        .output()
        .map_err(|e| format!("cannot list processes: {e}"))?;
    if !listing.status.success() {
        return Err(format!("ps exited with {}", listing.status));
    }
    let mut found = Vec::new();
    for line in String::from_utf8_lossy(&listing.stdout).lines() {
        let mut cols = line.split_whitespace();
        let Some(pid) = cols.next().and_then(|s| s.parse::<i32>().ok()) else {
            continue;
        };
        let Some(comm) = cols.last() else { continue };
        if comm != name && !comm.ends_with(&format!("/{name}")) {
            continue;
        }
        let Some(env) = process_env(pid) else {
            continue;
        };
        let Some(value) = env.iter().find_map(|pair| pair.strip_prefix(b"LXRT_ROOT=")) else {
            continue;
        };
        if canon(
            Path::new(&String::from_utf8_lossy(value).into_owned()),
            Path::new("/"),
        )
        .ok()
        .as_deref()
            == Some(root)
        {
            found.push(pid);
        }
    }
    Ok(found)
}

fn holds(root: &Path, only: Option<&str>) -> bool {
    if only.is_none() || only == Some("tmp") {
        if let Ok(entries) = fs::read_dir(root.join("tmp")) {
            if entries
                .flatten()
                .any(|e| e.file_name().to_string_lossy().ends_with("home") && exists(&e.path()))
            {
                return true;
            }
        }
    }
    (only.is_none() || only == Some("opt/apps"))
        && fs::read_dir(root.join("opt/apps")).is_ok_and(|mut i| i.next().is_some())
}

fn carry_list(root: &Path) -> Result<Option<Vec<String>>> {
    let path = root.join(".lxrt-carried");
    if !path.is_file() {
        return Ok(None);
    }
    let content = fs::read_to_string(path).map_err(|e| err("cannot read swap record", e))?;
    let mut lines = content.lines();
    if lines.next() != Some(CARRIED) {
        return Ok(None);
    }
    Ok(Some(lines.map(str::to_owned).collect()))
}

fn recover(root: &Path, marker: Option<&str>) -> Result<()> {
    let new = PathBuf::from(format!("{}.new", root.display()));
    let old = PathBuf::from(format!("{}.old", root.display()));
    if !new.is_dir() || symlink_path(&new) {
        return Ok(());
    }
    let record = carry_list(&new)?;
    if record.is_none() && !holds(&new, None) {
        return Ok(());
    }
    let carried = KEEP
        .iter()
        .copied()
        .filter(|d| {
            exists(&new.join(d))
                && record
                    .as_ref()
                    .map_or_else(|| holds(&new, Some(d)), |r| r.iter().any(|x| x == d))
        })
        .collect::<Vec<_>>();
    if !exists(root) && old.is_dir() && !symlink_path(&old) {
        fs::rename(&old, root).map_err(|e| err("cannot restore old root", e))?;
    }
    if !carried.is_empty() && marker.is_some_and(|m| exists(root) && !root.join(m).is_file()) {
        return Err(format!(
            "{} holds carried Steam state but {} has no marker; preserve it and recover by hand",
            new.display(),
            root.display()
        ));
    }
    for d in &carried {
        if exists(&root.join(d)) {
            return Err(format!("both {} and {} contain carried state; preserve the client data and resolve by hand", new.join(d).display(), root.join(d).display()));
        }
    }
    if !exists(root) {
        fs::create_dir_all(root).map_err(|e| err("cannot restore root directory", e))?;
        if let Some(marker) = marker {
            File::create(root.join(marker)).map_err(|e| err("cannot create marker", e))?;
        }
    }
    for d in KEEP {
        let from = new.join(d);
        let to = root.join(d);
        let recorded = record
            .as_ref()
            .map_or(true, |r| r.iter().any(|x| x == d) || holds(&new, Some(d)));
        if exists(&from) && !exists(&to) && !symlink_path(&to) && recorded {
            fs::create_dir_all(to.parent().ok_or("invalid state path")?)
                .map_err(|e| err("cannot create state parent", e))?;
            fs::rename(from, to).map_err(|e| err("cannot restore carried state", e))?;
        }
    }
    let record = new.join(".lxrt-carried");
    if exists(&record) {
        fs::remove_file(record).map_err(|e| err("cannot clear swap record", e))?;
    }
    Ok(())
}

fn clear_staging(root: &Path) -> Result<()> {
    for suffix in [".new", ".old"] {
        let path = PathBuf::from(format!("{}{suffix}", root.display()));
        if symlink_path(&path) {
            return Err(format!(
                "{} is a symlink; remove it by hand",
                path.display()
            ));
        }
        if !exists(&path) {
            continue;
        }
        if holds(&path, None) || exists(&path.join(".lxrt-carried")) {
            return Err(format!(
                "refusing to delete {}: it holds a Steam home or installed apps",
                path.display()
            ));
        }
        if path.is_dir() {
            fs::remove_dir_all(path).map_err(|e| err("cannot clear staging directory", e))?;
        } else {
            fs::remove_file(path).map_err(|e| err("cannot clear staging file", e))?;
        }
    }
    Ok(())
}

fn swap(root: &Path) -> Result<()> {
    let new = PathBuf::from(format!("{}.new", root.display()));
    let old = PathBuf::from(format!("{}.old", root.display()));
    if exists(&old) {
        return Err(format!(
            "{} appeared during the build; nothing swapped",
            old.display()
        ));
    }
    if !exists(root) {
        return fs::rename(new, root).map_err(|e| err("cannot install new root", e));
    }
    let record_path = new.join(".lxrt-carried");
    let mut record = File::create(&record_path).map_err(|e| err("cannot write swap record", e))?;
    writeln!(record, "{CARRIED}").map_err(|e| err("cannot write swap record", e))?;
    let mut moved = Vec::new();
    for d in KEEP {
        let from = root.join(d);
        let to = new.join(d);
        if !exists(&from) {
            continue;
        }
        if exists(&to) {
            if to.is_dir() && !symlink_path(&to) {
                fs::remove_dir_all(&to).map_err(|e| err("cannot clear cloned state", e))?;
            } else {
                fs::remove_file(&to).map_err(|e| err("cannot clear cloned state", e))?;
            }
        }
        fs::create_dir_all(to.parent().ok_or("invalid state path")?)
            .map_err(|e| err("cannot create state parent", e))?;
        writeln!(record, "{d}").map_err(|e| err("cannot update swap record", e))?;
        record
            .sync_all()
            .map_err(|e| err("cannot flush swap record", e))?;
        fs::rename(from, to).map_err(|e| err("cannot carry Steam state", e))?;
        moved.push(d);
    }
    drop(record);
    fs::rename(root, &old).map_err(|e| err("cannot stage old root", e))?;
    fs::rename(&new, root).map_err(|e| err("cannot install new root", e))?;
    for d in moved {
        if !exists(&root.join(d)) {
            return Err(format!(
                "{d} did not arrive; {} is preserved",
                old.display()
            ));
        }
    }
    let record = root.join(".lxrt-carried");
    if exists(&record) {
        fs::remove_file(record).map_err(|e| err("cannot clear swap record", e))?;
    }
    if holds(&old, None) {
        eprintln!(
            "mkframeroot: note: {} gained runtime state during swap; preserved",
            old.display()
        );
    } else if old.is_dir() {
        fs::remove_dir_all(old).map_err(|e| err("cannot remove old root", e))?;
    }
    Ok(())
}

fn state_root(home: &Path) -> PathBuf {
    if let Some(path) = env::var_os("STEAMARM_STATE") {
        return path.into();
    }
    let dev = home.join("Developer/SteamARM-roots");
    if dev.is_dir() || !home.join("SteamARM-roots").is_dir() {
        dev
    } else {
        home.join("SteamARM-roots")
    }
}

fn run() -> Result<()> {
    let cwd = env::current_dir().map_err(|e| err("cannot get working directory", e))?;
    let home = PathBuf::from(env::var_os("HOME").ok_or("HOME is not set")?);
    let state = state_root(&home);
    let mut adopt = false;
    let mut home_from = None;
    let mut positional = Vec::new();
    let mut args = env::args_os().skip(1);
    while let Some(arg) = args.next() {
        match arg.to_string_lossy().as_ref() {
            "--adopt" => adopt = true,
            "--home-from" => {
                home_from = Some(PathBuf::from(
                    args.next().ok_or("--home-from needs a directory")?,
                ))
            }
            "-h" | "--help" => {
                println!("Usage: mkframeroot.sh [--adopt] [--home-from DIR] [SRC [OUT]]\nBuild the Steam Frame SteamOS (Arch-based) ARM64 root.");
                return Ok(());
            }
            value if value.starts_with('-') => return Err(format!("unknown option {value}")),
            _ => positional.push(PathBuf::from(arg)),
        }
    }
    if positional.len() > 2 {
        return Err("usage: mkframeroot.sh [--adopt] [--home-from DIR] [SRC [OUT]]".into());
    }
    let src_arg = positional
        .first()
        .cloned()
        .unwrap_or_else(|| "/Volumes/SteamFrameRoot/rootfs".into());
    let out_arg = positional
        .get(1)
        .cloned()
        .unwrap_or_else(|| src_arg.parent().unwrap_or(Path::new(".")).join("arm64root"));
    let src = canon(&src_arg, &cwd)?;
    let out = canon(&out_arg, &cwd)
        .map_err(|_| format!("{} is not a usable output path", out_arg.display()))?;
    if !src.is_dir() || !src.join("etc/os-release").is_file() {
        return Err(format!(
            "no Steam Frame extraction at {}",
            src_arg.display()
        ));
    }
    let os = fs::read_to_string(src.join("etc/os-release"))
        .map_err(|e| err("cannot read os-release", e))?;
    if !os.lines().any(|l| l == "ID=steamos") {
        return Err(format!("{} is not a SteamOS root", src_arg.display()));
    }
    if exists(&out) && !out.is_dir() {
        return Err(format!("{} is not a directory", out_arg.display()));
    }
    if within(&out, &src) {
        return Err(format!("{} is inside the extraction", out_arg.display()));
    }
    let new = PathBuf::from(format!("{}.new", out.display()));
    let old = PathBuf::from(format!("{}.old", out.display()));
    for victim in [&out, &new, &old] {
        if within(&src, victim) {
            return Err(format!("the extraction is inside {}", victim.display()));
        }
    }
    for protected in [&state, &cwd, &home] {
        if let Ok(canonical) = canon(protected, &cwd) {
            for victim in [&out, &new, &old] {
                if within(&canonical, victim) {
                    return Err(format!(
                        "{} would contain protected path {}",
                        victim.display(),
                        protected.display()
                    ));
                }
            }
        }
    }
    let src_dev = fs::metadata(&src)
        .map_err(|e| err("cannot inspect source volume", e))?
        .dev();
    let out_parent = out.parent().ok_or("output has no parent")?;
    let device = fs::metadata(ancestor(out_parent)?)
        .map_err(|e| err("cannot inspect output volume", e))?
        .dev();
    if src_dev != device {
        return Err(format!(
            "{} is not on the extraction volume (APFS clone required)",
            out.display()
        ));
    }
    if exists(&out) && !out.join(".lxrt-frameroot").is_file() && !adopt {
        return Err(format!(
            "{} exists and was not made by this script (--adopt to rebuild it)",
            out_arg.display()
        ));
    }
    let home_from = if let Some(path) = home_from {
        let resolved = canon(&path, &cwd)
            .map_err(|_| format!("invalid --home-from path {}", path.display()))?;
        if !resolved.join(".local/share/Steam").is_dir() {
            return Err(format!(
                "--home-from has no Steam install: {}",
                resolved.display()
            ));
        }
        let kept = within(&resolved, &out.join("tmp")) || within(&resolved, &out.join("opt/apps"));
        if !kept && [&out, &new, &old].iter().any(|v| within(&resolved, v)) {
            return Err(format!(
                "--home-from {} lies in a root being replaced",
                resolved.display()
            ));
        }
        Some(resolved)
    } else {
        None
    };
    let active = guests(&out)?;
    if !active.is_empty() {
        return Err(format!(
            "guests are running on {} (pid {})",
            out.display(),
            active[0]
        ));
    }

    let build = os
        .lines()
        .find_map(|l| l.strip_prefix("BUILD_ID="))
        .unwrap_or("unknown")
        .trim_matches('"');
    let version = os
        .lines()
        .find_map(|l| l.strip_prefix("VERSION_ID="))
        .unwrap_or("unknown")
        .trim_matches('"');
    fs::create_dir_all(out.parent().unwrap()).map_err(|e| err("cannot create output parent", e))?;
    recover(&out, (!adopt).then_some(".lxrt-frameroot"))?;
    clear_staging(&out)?;
    let status = Command::new("cp")
        .args(["-Rc"])
        .arg(&src)
        .arg(&new)
        .status()
        .map_err(|e| format!("cannot run APFS clone (cp -Rc): {e}"))?;
    if !status.success() {
        return Err(format!("APFS clone failed with {status}"));
    }
    println!("cloned {} -> {}", src.display(), new.display());
    fs::create_dir_all(new.join("run")).map_err(|e| err("cannot create /run", e))?;
    fs::create_dir_all(new.join("dev/shm")).map_err(|e| err("cannot create /dev/shm", e))?;
    let resolv = new.join("etc/resolv.conf");
    if exists(&resolv) {
        fs::remove_file(&resolv).map_err(|e| err("cannot replace resolv.conf", e))?;
    }
    fs::write(&resolv, "nameserver 1.1.1.1\nnameserver 8.8.8.8\n")
        .map_err(|e| err("cannot write resolv.conf", e))?;
    let zone = fs::read_link("/etc/localtime")
        .ok()
        .and_then(|p| {
            p.to_string_lossy()
                .split("zoneinfo/")
                .last()
                .map(str::to_owned)
        })
        .filter(|s| !s.is_empty());
    if let Some(zone) = &zone {
        if new.join("usr/share/zoneinfo").join(zone).is_file() {
            let localtime = new.join("etc/localtime");
            if exists(&localtime) {
                fs::remove_file(&localtime).map_err(|e| err("cannot replace localtime", e))?;
            }
            symlink(Path::new("../usr/share/zoneinfo").join(zone), localtime)
                .map_err(|e| err("cannot link localtime", e))?;
        }
    }
    fs::write(new.join(".lxrt-guest-env"), GUEST_ENV)
        .map_err(|e| err("cannot write guest environment", e))?;
    if !Path::new("build/libvulkan.so.1").is_file() {
        eprintln!(
            "mkframeroot: note: no Vulkan shim in build; root will have no SteamARM Vulkan ICD"
        );
    } else if new.join("usr/lib/libvulkan.so.1").is_file() && new.join("usr/share/vulkan").is_dir()
    {
        for (script, note) in [
            ("install-frameroot-vulkan.sh", "Vulkan shim not installed"),
            (
                "install-frameroot-atomupd.sh",
                "atomupd-manager left as the image has it",
            ),
        ] {
            let project = env::current_dir().map_err(|e| err("cannot locate project", e))?;
            let status = Command::new(project.join("scripts").join(script))
                .arg(&new)
                .current_dir(project)
                .status();
            if !status.is_ok_and(|s| s.success()) {
                eprintln!("mkframeroot: note: {note}");
            }
        }
    } else {
        eprintln!("mkframeroot: note: the image has no Vulkan loader");
    }
    let stamp = output(
        "date",
        &[OsStr::new("-u"), OsStr::new("+%Y-%m-%dT%H:%M:%SZ")],
    )?;
    let marker = format!("made by Rust Steam Frame root builder on {stamp}\nfrom: SteamOS {version} BUILD_ID={build}\noverlay: etc/resolv.conf etc/localtime({}) .lxrt-guest-env vulkan-icd\n", zone.as_deref().unwrap_or("unchanged"));
    fs::write(new.join(".lxrt-frameroot"), marker)
        .map_err(|e| err("cannot write root marker", e))?;
    swap(&out)?;
    fs::create_dir_all(out.join("tmp")).map_err(|e| err("cannot create root tmp", e))?;
    if let Some(home) = home_from {
        if !out.join("tmp/armhome/.local/share/Steam").is_dir() {
            fs::create_dir_all(out.join("tmp/armhome"))
                .map_err(|e| err("cannot create client home", e))?;
            let status = Command::new("rsync")
                .args(["-a", "--exclude", "/.local/share/Steam/steamapps/common/"])
                .arg(format!("{}/", home.display()))
                .arg(format!("{}/", out.join("tmp/armhome").display()))
                .status()
                .map_err(|e| format!("cannot run rsync: {e}"))?;
            if !status.success() {
                return Err(format!("rsync exited with {status}"));
            }
            println!("client home copied from {}", home.display());
        }
    }
    fs::create_dir_all(&state).map_err(|e| err("cannot create state root", e))?;
    let link = state.join("arm64root");
    if !exists(&link) || symlink_path(&link) {
        if symlink_path(&link) {
            fs::remove_file(&link).map_err(|e| err("cannot replace root link", e))?;
        }
        symlink(&out, &link).map_err(|e| err("cannot link Steam Frame root", e))?;
    }
    let mut missing = 0;
    for item in [
        "usr/share/X11/locale/locale.dir",
        "usr/lib/locale/C.utf8",
        "usr/bin/lsof",
        "usr/lib/libnssckbi.so",
        "usr/lib/libGLX_mesa.so.0",
        "usr/lib/libGL.so.1",
        "etc/ssl/certs/ca-certificates.crt",
        "etc/resolv.conf",
    ] {
        if exists(&out.join(item)) {
            println!("  ok       {item}");
        } else {
            println!("  MISSING  {item}");
            missing += 1;
        }
    }
    if out
        .join("tmp/armhome/.local/share/Steam/steamrtarm64/steam")
        .is_file()
    {
        println!("  ok       Steam ARM64 client");
    } else {
        println!("  note     no client yet (--home-from DIR)");
    }
    println!(
        "root: {} (SteamOS {version}, BUILD_ID {build}); linked from {}",
        out.display(),
        link.display()
    );
    if missing != 0 {
        return Err(format!("{missing} required Steam Frame files are missing"));
    }
    Ok(())
}

fn main() {
    if let Err(error) = run() {
        eprintln!("mkframeroot: error: {error}");
        std::process::exit(1);
    }
}
