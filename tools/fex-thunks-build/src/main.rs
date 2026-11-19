use std::cmp::Ordering;
use std::env;
use std::ffi::{OsStr, OsString};
use std::fs;
use std::io::Write;
use std::os::unix::fs::{symlink, PermissionsExt};
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};

const FEX_COMMIT: &str = "08f451d3bce62a68aa6307dced5a8eaeb85c39bc";
const UBUNTU: &str = "https://archive.ubuntu.com/ubuntu";
const PATCHES: &[&str] = &[
    "fex-lxrt-x18.patch",
    "fex-lxrt-guest-base.patch",
    "fex-lxrt-wx.patch",
    "fex-thunkgen-macos.patch",
    "fex-thunks-guestbase32.patch",
    "fex-thunkgen-32bit.patch",
];
const SUBMODULES: &[&str] = &["External/fmt", "External/Vulkan-Headers"];

#[derive(Clone, Copy)]
struct Deb {
    path: &'static str,
    sha256: &'static str,
}

const DEBS64: &[Deb] = &[
    Deb {
        path: "main/g/glibc/libc6_2.39-0ubuntu8.12_amd64.deb",
        sha256: "d821b41fc59cb4978ecd53bc83e5c964ac15d305c8d2ac4c8a35aebe55010fb9",
    },
    Deb {
        path: "main/g/glibc/libc6-dev_2.39-0ubuntu8.12_amd64.deb",
        sha256: "d81dd6dc9d515762cc93f338efa6219252acf3e2e87b5b89dd7dc517741e6eed",
    },
    Deb {
        path: "main/l/linux/linux-libc-dev_6.8.0-149.149_amd64.deb",
        sha256: "e4b7b9c5023ba727908868661e1113d28ed9b68e47aed00e314723dad4b2b31d",
    },
    Deb {
        path: "main/g/gcc-14/libstdc++6_14.2.0-4ubuntu2~24.04.1_amd64.deb",
        sha256: "a51f8de7829211db961a31f02158058ad1a95f92ac6d0a5dff6350e2821c54c0",
    },
    Deb {
        path: "universe/g/gcc-14/libstdc++-14-dev_14.2.0-4ubuntu2~24.04.1_amd64.deb",
        sha256: "6d223262b8cbe6d5150435056d58dffa8159d8582fc0df47fb45ea550cd37424",
    },
    Deb {
        path: "main/g/gcc-14/libgcc-14-dev_14.2.0-4ubuntu2~24.04.1_amd64.deb",
        sha256: "055dfa5fa7448fe20ad2f3832df2edd2b0ffca7f8a6c5fbbc5576dc61b2eb8b7",
    },
    Deb {
        path: "main/g/gcc-14/libgcc-s1_14.2.0-4ubuntu2~24.04.1_amd64.deb",
        sha256: "aa7fadbe33b78bcf99885318040601c550c208929565b179891d9a3cc2aa68cd",
    },
    Deb {
        path: "main/libx/libx11/libx11-dev_1.8.7-1ubuntu0.1_amd64.deb",
        sha256: "467e28dafc2a51a59bf8615325e2e6bd42da63d3e346580a5baf14fdfde8a8e1",
    },
    Deb {
        path: "main/x/xorgproto/x11proto-dev_2023.2-1_all.deb",
        sha256: "3bf13a2ffb79ecd4014d438936014e5863f79eb59ebf76e8f43356271c90e7ce",
    },
    Deb {
        path: "main/libx/libxcb/libxcb1-dev_1.15-1ubuntu2_amd64.deb",
        sha256: "1bafe3432feafc9e57f858721da524dfb3ee1f6fc4ef6b0c73023e79eadb9c28",
    },
    Deb {
        path: "main/libx/libxrandr/libxrandr-dev_1.5.2-2build1_amd64.deb",
        sha256: "8c0ee697a9052f36103529ebe4ace096da845e2b517f39d206302f3c7709ec6f",
    },
    Deb {
        path: "main/libx/libxrender/libxrender-dev_0.9.10-1.1build1_amd64.deb",
        sha256: "839558c9de049636e13e22e409c04f3226614abf176c4a3b7088058576d74848",
    },
];

const DEBS32: &[Deb] = &[
    Deb {
        path: "main/g/glibc/libc6_2.39-0ubuntu8.12_i386.deb",
        sha256: "9df3a419c17e1d5148ba881c9c23c61a2fc47de6e78d6bc884e2be74a9a709ac",
    },
    Deb {
        path: "main/g/glibc/libc6-dev_2.39-0ubuntu8.12_i386.deb",
        sha256: "1b8f40c82d6e7fb8bc97922460a5a34184a1070503f78f6350aa11d5b562efda",
    },
    Deb {
        path: "main/l/linux/linux-libc-dev_6.8.0-149.149_i386.deb",
        sha256: "d2acf433c10fcbb484989fc7e3ff5bbf7871cf2b99d98d22d21d1c76ed671883",
    },
    Deb {
        path: "main/g/gcc-14/libstdc++6_14.2.0-4ubuntu2~24.04.1_i386.deb",
        sha256: "899d77d8b4876f545bd362f60cb4aa0263a8033777e9ecca4742c890c4c54a68",
    },
    Deb {
        path: "universe/g/gcc-14/libstdc++-14-dev_14.2.0-4ubuntu2~24.04.1_i386.deb",
        sha256: "091d52807b50ad4321a580ddad29fbc0f524ce564825c8c965b42d5f197bf48b",
    },
    Deb {
        path: "main/g/gcc-14/libgcc-14-dev_14.2.0-4ubuntu2~24.04.1_i386.deb",
        sha256: "b7e90e0d45b860bce7f5fd9e49d33d433d53a4c5bbfe34a3429f2c193f7f2327",
    },
    Deb {
        path: "main/g/gcc-14/libgcc-s1_14.2.0-4ubuntu2~24.04.1_i386.deb",
        sha256: "40879f5dae85f90837ce882527bb85e388fa0e57f7ec4fba2060f1d24ce3dbb7",
    },
];

struct Config {
    repo: PathBuf,
    main_source: PathBuf,
    source: PathBuf,
    out: PathBuf,
    debs: PathBuf,
    frame_sysroot: PathBuf,
    x86root: PathBuf,
    x86root32: PathBuf,
    xinc: PathBuf,
    llvm_bin: PathBuf,
    llvm_prefix: PathBuf,
    lld: PathBuf,
    openssl: PathBuf,
}

fn error(message: impl AsRef<str>) -> ! {
    eprintln!("steamarm-fex-thunks-build: error: {}", message.as_ref());
    std::process::exit(1);
}

fn log(message: &str) {
    println!("{message}");
}

fn selected(home: &Path, variable: &str, name: &str) -> PathBuf {
    if let Some(path) = env::var_os(variable).filter(|v| !v.is_empty()) {
        return path.into();
    }
    let preferred = home.join("Developer").join(name);
    let legacy = home.join(name);
    if preferred.is_dir() || !legacy.is_dir() {
        preferred
    } else {
        legacy
    }
}

fn find_path(program: &str) -> Option<PathBuf> {
    env::split_paths(&env::var_os("PATH")?)
        .map(|p| p.join(program))
        .find(|p| fs::metadata(p).is_ok_and(|m| m.is_file() && m.permissions().mode() & 0o111 != 0))
}

fn config() -> Result<Config, String> {
    let home = env::var_os("HOME")
        .map(PathBuf::from)
        .ok_or("HOME is not set")?;
    let repo = env::var_os("STEAMARM_PROJECT_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../.."));
    let work = selected(&home, "STEAMARM_BUILD", "SteamARM-build");
    let state = selected(&home, "STEAMARM_STATE", "SteamARM-roots");
    let llvm_bin = env::var_os("LLVM_BIN")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/opt/homebrew/opt/llvm/bin"));
    let llvm_prefix = llvm_bin
        .parent()
        .unwrap_or(Path::new("/opt/homebrew/opt/llvm"))
        .to_path_buf();
    let lld = env::var_os("LLD")
        .filter(|v| !v.is_empty())
        .map(PathBuf::from)
        .or_else(|| {
            let nearby = llvm_bin.join("ld.lld");
            nearby.is_file().then_some(nearby)
        })
        .or_else(|| find_path("ld.lld"))
        .unwrap_or_default();
    let openssl = env::var_os("OPENSSL")
        .map(PathBuf::from)
        .or_else(|| capture("brew", &["--prefix", "openssl@3"]).map(PathBuf::from))
        .unwrap_or_else(|| PathBuf::from("/opt/homebrew/opt/openssl@3"));
    let frame_sysroot = env::var_os("STEAMARM_FRAME_SYSROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| state.join("arm64root"));
    Ok(Config {
        repo,
        main_source: work.join("FEX"),
        source: work.join("FEX-thunks"),
        out: work.join("thunks-build"),
        debs: work.join("debs"),
        frame_sysroot,
        x86root: work.join("thunks-build/sysroot-x86_64"),
        x86root32: work.join("thunks-build/sysroot-i386"),
        xinc: work.join("thunks-build/xinclude"),
        llvm_bin,
        llvm_prefix,
        lld,
        openssl,
    })
}

fn capture(program: &str, args: &[&str]) -> Option<String> {
    let output = Command::new(program).args(args).output().ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

fn osargs(values: &[&str]) -> Vec<OsString> {
    values.iter().map(OsString::from).collect()
}

fn output(
    program: impl AsRef<OsStr>,
    args: &[OsString],
    cwd: Option<&Path>,
) -> Result<Output, String> {
    let program = program.as_ref();
    let mut command = Command::new(program);
    command.args(args);
    if let Some(cwd) = cwd {
        command.current_dir(cwd);
    }
    command
        .output()
        .map_err(|e| format!("cannot run {}: {e}", program.to_string_lossy()))
}

fn run(program: impl AsRef<OsStr>, args: &[OsString], cwd: Option<&Path>) -> Result<(), String> {
    run_env(program, args, cwd, &[])
}

fn run_env(
    program: impl AsRef<OsStr>,
    args: &[OsString],
    cwd: Option<&Path>,
    envs: &[(&str, &str)],
) -> Result<(), String> {
    let program = program.as_ref();
    let mut command = Command::new(program);
    command.args(args);
    if let Some(cwd) = cwd {
        command.current_dir(cwd);
    }
    command.envs(envs.iter().copied());
    let status = command
        .status()
        .map_err(|e| format!("cannot run {}: {e}", program.to_string_lossy()))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!(
            "{} exited with {status}",
            program.to_string_lossy()
        ))
    }
}

fn require_frame(c: &Config) -> Result<(), String> {
    if !c.frame_sysroot.join(".lxrt-frameroot").is_file()
        || !c.frame_sysroot.join("usr/include/stdio.h").is_file()
    {
        return Err(format!(
            "Steam Frame SteamOS sysroot missing at {}; run scripts/build-fex-host.sh sysroot",
            c.frame_sysroot.display()
        ));
    }
    Ok(())
}

fn check_tools(c: &Config) -> Result<(), String> {
    for path in [
        c.llvm_bin.join("clang"),
        c.llvm_bin.join("clang++"),
        c.lld.clone(),
        c.llvm_bin.join("llvm-readelf"),
    ] {
        if !path.is_file()
            || path
                .metadata()
                .is_ok_and(|m| m.permissions().mode() & 0o111 == 0)
        {
            return Err(format!(
                "missing {} (install Homebrew llvm/lld)",
                path.display()
            ));
        }
    }
    if !c.llvm_prefix.join("lib/libclang-cpp.dylib").is_file() {
        return Err(format!(
            "no libclang-cpp in {}",
            c.llvm_prefix.join("lib").display()
        ));
    }
    if !c.openssl.join("include/openssl/sha.h").is_file() {
        return Err(format!("no OpenSSL headers in {}", c.openssl.display()));
    }
    for tool in ["git", "curl", "tar", "shasum", "zstd", "xz"] {
        if find_path(tool).is_none() {
            return Err(format!("missing {tool}"));
        }
    }
    Ok(())
}

fn hash(path: &Path) -> Result<String, String> {
    let output = Command::new("shasum")
        .args(["-a", "256"])
        .arg(path)
        .output()
        .map_err(|e| format!("hash {}: {e}", path.display()))?;
    if !output.status.success() {
        return Err(format!("SHA-256 failed for {}", path.display()));
    }
    String::from_utf8_lossy(&output.stdout)
        .split_whitespace()
        .next()
        .map(str::to_owned)
        .ok_or_else(|| "shasum returned no digest".into())
}

fn hash_bytes(bytes: &[u8]) -> Result<String, String> {
    let mut child = Command::new("shasum")
        .args(["-a", "256"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("start shasum: {e}"))?;
    child
        .stdin
        .take()
        .ok_or("shasum stdin unavailable")?
        .write_all(bytes)
        .map_err(|e| format!("write hash input: {e}"))?;
    let result = child
        .wait_with_output()
        .map_err(|e| format!("wait for shasum: {e}"))?;
    if !result.status.success() {
        return Err("shasum failed".into());
    }
    String::from_utf8_lossy(&result.stdout)
        .split_whitespace()
        .next()
        .map(str::to_owned)
        .ok_or_else(|| "shasum returned no digest".into())
}

fn remove_path(path: &Path) -> Result<(), String> {
    let Ok(metadata) = fs::symlink_metadata(path) else {
        return Ok(());
    };
    if metadata.file_type().is_symlink() || metadata.is_file() {
        fs::remove_file(path).map_err(|e| format!("remove {}: {e}", path.display()))
    } else {
        fs::remove_dir_all(path).map_err(|e| format!("remove {}: {e}", path.display()))
    }
}

fn git(args: &[OsString], cwd: &Path) -> Result<Output, String> {
    output(OsStr::new("git"), args, Some(cwd))
}

fn source(c: &Config) -> Result<(), String> {
    log(&format!(
        "== source: FEX worktree {} at {FEX_COMMIT}",
        c.source.display()
    ));
    if !c.main_source.join(".git").is_dir() {
        return Err(format!(
            "no FEX clone at {} (run scripts/build-fex-host.sh source)",
            c.main_source.display()
        ));
    }
    if !c.source.join(".git").exists() {
        run(
            OsStr::new("git"),
            &[
                "worktree".into(),
                "add".into(),
                "--detach".into(),
                c.source.clone().into_os_string(),
                FEX_COMMIT.into(),
            ],
            Some(&c.main_source),
        )?;
    }
    let mut fingerprint = format!("{FEX_COMMIT}\n").into_bytes();
    for patch in PATCHES {
        fingerprint.extend(
            fs::read(c.repo.join("patches").join(patch))
                .map_err(|e| format!("read patches/{patch}: {e}"))?,
        );
    }
    fingerprint.extend(
        fs::read(c.repo.join("patches/LxrtJit.h")).map_err(|e| format!("read LxrtJit.h: {e}"))?,
    );
    let wanted = hash_bytes(&fingerprint)?;
    let previous = fs::read_to_string(c.out.join(".patched"))
        .unwrap_or_default()
        .trim()
        .to_owned();
    let head = git(&["rev-parse".into(), "HEAD".into()], &c.source)?;
    if previous == wanted && String::from_utf8_lossy(&head.stdout).trim() == FEX_COMMIT {
        log("  worktree at pinned commit with current thunk patches");
    } else {
        run(
            OsStr::new("git"),
            &[
                "checkout".into(),
                "-q".into(),
                "-f".into(),
                "--detach".into(),
                FEX_COMMIT.into(),
            ],
            Some(&c.source),
        )?;
        run(
            OsStr::new("git"),
            &["clean".into(), "-fdxq".into()],
            Some(&c.source),
        )?;
        for patch in PATCHES {
            log(&format!("  apply   patches/{patch}"));
            run(
                OsStr::new("git"),
                &[
                    "apply".into(),
                    "--whitespace=nowarn".into(),
                    c.repo.join("patches").join(patch).into_os_string(),
                ],
                Some(&c.source),
            )?;
        }
        let header = c.source.join("FEXCore/include/FEXCore/Utils/LxrtJit.h");
        fs::create_dir_all(header.parent().ok_or("invalid FEX header destination")?)
            .map_err(|e| format!("create header directory: {e}"))?;
        fs::copy(c.repo.join("patches/LxrtJit.h"), &header)
            .map_err(|e| format!("copy LxrtJit.h: {e}"))?;
        fs::create_dir_all(&c.out).map_err(|e| format!("create {}: {e}", c.out.display()))?;
        fs::write(c.out.join(".patched"), format!("{wanted}\n"))
            .map_err(|e| format!("write patch stamp: {e}"))?;
    }
    let mut submodules = osargs(&["submodule", "update", "--init", "--depth", "1", "--"]);
    submodules.extend(SUBMODULES.iter().map(OsString::from));
    if run(OsStr::new("git"), &submodules, Some(&c.source)).is_err() {
        let mut retry = osargs(&["submodule", "update", "--init", "--"]);
        retry.extend(SUBMODULES.iter().map(OsString::from));
        run(OsStr::new("git"), &retry, Some(&c.source))?;
    }
    let version = git(
        &["log".into(), "-1".into(), "--format=%s".into()],
        &c.source.join("External/Vulkan-Headers"),
    )?;
    log(&format!(
        "  Vulkan-Headers {}",
        String::from_utf8_lossy(&version.stdout).trim()
    ));
    Ok(())
}

fn fetch_deb(c: &Config, deb: Deb) -> Result<PathBuf, String> {
    let file = Path::new(deb.path).file_name().ok_or("invalid deb path")?;
    let destination = c.debs.join(file);
    if destination.is_file() && hash(&destination)? == deb.sha256 {
        log(&format!("  cached  {}", file.to_string_lossy()));
        return Ok(destination);
    }
    fs::create_dir_all(&c.debs).map_err(|e| format!("create {}: {e}", c.debs.display()))?;
    let part = destination.with_extension("deb.part");
    log(&format!("  fetch   {UBUNTU}/pool/{}", deb.path));
    let status = Command::new("curl")
        .args(["-fsSL", "--retry", "3", "--connect-timeout", "20", "-o"])
        .arg(&part)
        .arg(format!("{UBUNTU}/pool/{}", deb.path))
        .status()
        .map_err(|e| format!("start curl: {e}"))?;
    if !status.success() {
        let _ = fs::remove_file(&part);
        return Err(format!("download failed: {}", deb.path));
    }
    if hash(&part)? != deb.sha256 {
        let _ = fs::remove_file(&part);
        return Err(format!("SHA-256 mismatch: {}", file.to_string_lossy()));
    }
    fs::rename(&part, &destination)
        .map_err(|e| format!("install {}: {e}", destination.display()))?;
    Ok(destination)
}

fn unpack_deb(deb: &Path, root: &Path) -> Result<(), String> {
    let listing = Command::new("tar")
        .arg("-tf")
        .arg(deb)
        .output()
        .map_err(|e| format!("list {}: {e}", deb.display()))?;
    if !listing.status.success() {
        return Err(format!("cannot list {}", deb.display()));
    }
    let member = String::from_utf8_lossy(&listing.stdout)
        .lines()
        .find(|line| line.starts_with("data.tar"))
        .ok_or_else(|| format!("no data.tar member in {}", deb.display()))?
        .to_owned();
    for attempt in 0..2 {
        let mut extract = Command::new("tar")
            .arg("-xOf")
            .arg(deb)
            .arg(&member)
            .stdout(Stdio::piped())
            .spawn()
            .map_err(|e| format!("read {member} from {}: {e}", deb.display()))?;
        let stream = extract.stdout.take().ok_or("tar output unavailable")?;
        let unpack = Command::new("tar")
            .args(["-xf", "-", "-C"])
            .arg(root)
            .args(["--exclude", "./usr/share/*"])
            .stdin(Stdio::from(stream))
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map_err(|e| format!("unpack {}: {e}", deb.display()))?;
        let packed = extract.wait().map_err(|e| format!("wait for tar: {e}"))?;
        if unpack.success() && packed.success() {
            return Ok(());
        }
        if attempt == 1 {
            return Err(format!("could not unpack {}", deb.display()));
        }
    }
    Err(format!("could not unpack {}", deb.display()))
}

fn relative_link(root: &Path, link: &Path, absolute_target: &Path) -> Result<PathBuf, String> {
    let target = root.join(absolute_target.strip_prefix("/").unwrap_or(absolute_target));
    let parent = link.parent().ok_or("symlink has no parent")?;
    let relative_parent = parent.strip_prefix(root).map_err(|e| e.to_string())?;
    let relative_target = target.strip_prefix(root).map_err(|e| e.to_string())?;
    let from = relative_parent.components().collect::<Vec<_>>();
    let to = relative_target.components().collect::<Vec<_>>();
    let common = from.iter().zip(&to).take_while(|(a, b)| a == b).count();
    let mut output = PathBuf::new();
    for _ in common..from.len() {
        output.push("..");
    }
    for component in &to[common..] {
        output.push(component.as_os_str());
    }
    Ok(output)
}

fn relativize_links(root: &Path, dir: &Path) -> Result<usize, String> {
    let mut changed = 0;
    for entry in fs::read_dir(dir).map_err(|e| format!("read {}: {e}", dir.display()))? {
        let path = entry.map_err(|e| e.to_string())?.path();
        let metadata = fs::symlink_metadata(&path).map_err(|e| e.to_string())?;
        if metadata.file_type().is_symlink() {
            let target =
                fs::read_link(&path).map_err(|e| format!("read link {}: {e}", path.display()))?;
            if target.is_absolute() {
                let relative = relative_link(root, &path, &target)?;
                fs::remove_file(&path)
                    .map_err(|e| format!("replace symlink {}: {e}", path.display()))?;
                symlink(relative, &path)
                    .map_err(|e| format!("write relative symlink {}: {e}", path.display()))?;
                changed += 1;
            }
        } else if metadata.is_dir() {
            changed += relativize_links(root, &path)?;
        }
    }
    Ok(changed)
}

fn version_cmp(a: &OsStr, b: &OsStr) -> Ordering {
    let parts = |s: &OsStr| {
        s.to_string_lossy()
            .split(['.', '-', '_'])
            .map(|p| p.parse::<u64>().unwrap_or(0))
            .collect::<Vec<_>>()
    };
    parts(a).cmp(&parts(b))
}

fn file_exists(root: &Path, relative: &str) -> bool {
    root.join(relative).exists()
}

fn make_sysroot(
    c: &Config,
    root: &Path,
    triplet: &str,
    debs: &[Deb],
    is_64: bool,
) -> Result<(), String> {
    log(&format!(
        "== x86root: Ubuntu 24.04 {triplet} dev sysroot -> {}",
        root.display()
    ));
    let mut packages = Vec::new();
    for deb in debs {
        packages.push(fetch_deb(c, *deb)?);
    }
    let stamp_input = debs
        .iter()
        .map(|d| format!("{} {}\n", d.path, d.sha256))
        .collect::<String>();
    let stamp = hash_bytes(stamp_input.as_bytes())?;
    if fs::read_to_string(root.join(".lxrt-sysroot"))
        .unwrap_or_default()
        .trim()
        == stamp
    {
        log("  sysroot up to date");
    } else {
        let temp = PathBuf::from(format!("{}.tmp", root.display()));
        remove_path(&temp)?;
        fs::create_dir_all(&temp).map_err(|e| format!("create {}: {e}", temp.display()))?;
        for (deb, package) in debs.iter().zip(&packages) {
            log(&format!(
                "  unpack  {}",
                package.file_name().unwrap_or_default().to_string_lossy()
            ));
            unpack_deb(package, &temp).map_err(|e| format!("{}: {e}", deb.path))?;
        }
        for name in ["bin", "sbin", "lib", "lib64", "lib32"] {
            let old = temp.join(name);
            let new = temp.join("usr").join(name);
            if old.is_dir()
                && !old
                    .symlink_metadata()
                    .is_ok_and(|m| m.file_type().is_symlink())
            {
                fs::create_dir_all(&new).map_err(|e| format!("create {}: {e}", new.display()))?;
                let contents = old.join(".");
                let status = Command::new("cp")
                    .args(["-a"])
                    .arg(contents)
                    .arg(format!("{}/", new.display()))
                    .status()
                    .map_err(|e| format!("merge usr/{name}: {e}"))?;
                if !status.success() {
                    return Err(format!("merge usr/{name} failed"));
                }
                remove_path(&old)?;
            }
            if new.exists() && !old.exists() {
                symlink(PathBuf::from("usr").join(name), &old)
                    .map_err(|e| format!("create usrmerge symlink {}: {e}", old.display()))?;
            }
        }
        let changed = relativize_links(&temp, &temp)?;
        log(&format!("  {changed} absolute symlinks made relative"));
        let gnu = temp.join(format!("usr/lib/{triplet}"));
        let gcc_top = temp.join("usr/lib/gcc");
        let crt = fs::read_dir(&gcc_top)
            .ok()
            .into_iter()
            .flatten()
            .flatten()
            .map(|entry| entry.path().join("14/crtbeginS.o"))
            .any(|path| path.is_file());
        if !file_exists(&temp, "usr/include/stdio.h") || !gnu.join("crti.o").is_file() || !crt {
            return Err(format!("{triplet} sysroot incomplete"));
        }
        fs::write(temp.join(".lxrt-sysroot"), format!("{stamp}\n"))
            .map_err(|e| format!("write sysroot stamp: {e}"))?;
        remove_path(root)?;
        fs::rename(&temp, root).map_err(|e| format!("install {}: {e}", root.display()))?;
    }
    if is_64 {
        remove_path(&c.xinc)?;
        fs::create_dir_all(&c.xinc).map_err(|e| format!("create X11 include directory: {e}"))?;
        for name in ["X11", "xcb"] {
            let source = root.join("usr/include").join(name);
            let status = Command::new("cp")
                .args(["-a"])
                .arg(&source)
                .arg(&c.xinc)
                .status()
                .map_err(|e| format!("copy {}: {e}", source.display()))?;
            if !status.success() {
                return Err(format!("copy X11 headers {} failed", source.display()));
            }
        }
    }
    let size = capture("du", &["-sh", root.to_str().unwrap_or_default()])
        .and_then(|s| s.split_whitespace().next().map(str::to_owned))
        .unwrap_or_default();
    log(&format!("  {triplet} sysroot ready: {size}"));
    Ok(())
}

fn do_thunkgen(c: &Config) -> Result<(), String> {
    let llvm_config = c.llvm_bin.join("llvm-config");
    let version = capture(
        llvm_config.to_str().ok_or("invalid llvm-config path")?,
        &["--version"],
    )
    .ok_or("llvm-config failed")?;
    log(&format!("== thunkgen (macOS arm64, libclang {version})"));
    let gen = c.source.join("ThunkLibs/Generator");
    let out = c.out.join("thunkgen");
    fs::create_dir_all(&out).map_err(|e| format!("create {}: {e}", out.display()))?;
    let resource = capture(
        c.llvm_bin
            .join("clang")
            .to_str()
            .ok_or("invalid clang path")?,
        &["-print-resource-dir"],
    )
    .ok_or("clang could not report its resource directory")?;
    let stamp = format!("{resource} {version}");
    if fs::read_to_string(out.join(".llvm"))
        .unwrap_or_default()
        .trim()
        != stamp
    {
        for name in ["main.o", "analysis.o", "data_layout.o", "gen.o"] {
            let _ = fs::remove_file(out.join(name));
        }
        fs::write(out.join(".llvm"), format!("{stamp}\n"))
            .map_err(|e| format!("write thunkgen stamp: {e}"))?;
    }
    for file in ["main", "analysis", "data_layout", "gen"] {
        let source = gen.join(format!("{file}.cpp"));
        let object = out.join(format!("{file}.o"));
        let fresh = fs::metadata(&source).and_then(|a| a.modified()).ok()
            > fs::metadata(&object).and_then(|a| a.modified()).ok();
        if !object.is_file() || fresh {
            log(&format!("  cc      {file}.cpp"));
            let args = vec![
                "-std=c++20".into(),
                "-O2".into(),
                "-fno-rtti".into(),
                "-isystem".into(),
                c.llvm_prefix.join("include").into_os_string(),
                "-I".into(),
                c.source.join("External/fmt/include").into_os_string(),
                "-DFMT_HEADER_ONLY".into(),
                "-I".into(),
                c.openssl.join("include").into_os_string(),
                format!("-DCLANG_RESOURCE_DIR=\"{resource}\"").into(),
                "-c".into(),
                source.into_os_string(),
                "-o".into(),
                object.into_os_string(),
            ];
            run(&c.llvm_bin.join("clang++"), &args, Some(&c.repo))?;
        }
    }
    let bin = out.join("thunkgen");
    let args = vec![
        "-o".into(),
        bin.clone().into_os_string(),
        out.join("main.o").into_os_string(),
        out.join("analysis.o").into_os_string(),
        out.join("data_layout.o").into_os_string(),
        out.join("gen.o").into_os_string(),
        format!("-L{}", c.llvm_prefix.join("lib").display()).into(),
        "-lclang-cpp".into(),
        "-lLLVM".into(),
        format!("-Wl,-rpath,{}", c.llvm_prefix.join("lib").display()).into(),
        c.openssl.join("lib/libcrypto.dylib").into_os_string(),
    ];
    run(&c.llvm_bin.join("clang++"), &args, Some(&c.repo))?;
    log(&format!("  {}", bin.display()));
    Ok(())
}

fn include_args(c: &Config) -> Vec<OsString> {
    let tl = c.source.join("ThunkLibs");
    vec![
        "-isystem".into(),
        tl.join("include").into_os_string(),
        "-isystem".into(),
        c.source
            .join("External/Vulkan-Headers/include")
            .into_os_string(),
    ]
}

fn do_gen(c: &Config) -> Result<(), String> {
    require_frame(c)?;
    let tl = c.source.join("ThunkLibs");
    let iface = tl.join("libvulkan/libvulkan_interface.cpp");
    let generator = c.out.join("thunkgen/thunkgen");
    let incs = include_args(c);
    fs::create_dir_all(c.out.join("gen")).map_err(|e| e.to_string())?;
    fs::create_dir_all(c.out.join("gen_64")).map_err(|e| e.to_string())?;
    log("== gen: guest (x86-64)");
    let mut guest = vec![
        iface.clone().into_os_string(),
        "libvulkan".into(),
        "-guest".into(),
        c.out
            .join("gen/thunkgen_guest_libvulkan.inl")
            .into_os_string(),
        c.x86root.clone().into_os_string(),
        "--".into(),
        "-std=c++20".into(),
        "--target=x86_64-linux-gnu".into(),
        "--sysroot".into(),
        c.x86root.clone().into_os_string(),
    ];
    guest.extend(incs.clone());
    run(&generator, &guest, Some(&c.repo))?;
    log("== gen: host (aarch64-linux)");
    let mut host = vec![
        iface.into_os_string(),
        "libvulkan".into(),
        "-host".into(),
        c.out
            .join("gen_64/thunkgen_host_libvulkan.inl")
            .into_os_string(),
        c.x86root.clone().into_os_string(),
        "--".into(),
        "-std=c++20".into(),
        "--target=aarch64-unknown-linux-gnu".into(),
        "--sysroot".into(),
        c.frame_sysroot.clone().into_os_string(),
        "-DARCHITECTURE_arm64=1".into(),
    ];
    host.extend(incs);
    host.extend(["-isystem".into(), c.xinc.clone().into_os_string()]);
    run(&generator, &host, Some(&c.repo))?;
    log_generated(c, &["gen", "gen_64"]);
    Ok(())
}

fn latest_gcc(root: &Path) -> Result<PathBuf, String> {
    let dir = root.join("usr/lib/gcc/aarch64-unknown-linux-gnu");
    let mut entries = fs::read_dir(&dir)
        .map_err(|e| format!("read {}: {e}", dir.display()))?
        .filter_map(Result::ok)
        .filter(|e| e.path().is_dir())
        .map(|e| e.path())
        .collect::<Vec<_>>();
    entries.sort_by(|a, b| {
        version_cmp(
            a.file_name().unwrap_or_default(),
            b.file_name().unwrap_or_default(),
        )
    });
    entries
        .pop()
        .ok_or_else(|| format!("no GCC support files under {}", dir.display()))
}

fn readelf(c: &Config, file: &Path, options: &[&str], filters: &[&str]) -> Result<(), String> {
    let result = Command::new(c.llvm_bin.join("llvm-readelf"))
        .args(options)
        .arg(file)
        .output()
        .map_err(|e| format!("run llvm-readelf: {e}"))?;
    if !result.status.success() {
        return Err(format!("llvm-readelf failed: {}", file.display()));
    }
    for line in String::from_utf8_lossy(&result.stdout).lines() {
        if filters.iter().any(|filter| line.contains(filter)) {
            println!("  {line}");
        }
    }
    Ok(())
}

fn do_host(c: &Config, is_32: bool) -> Result<(), String> {
    require_frame(c)?;
    let gcc = latest_gcc(&c.frame_sysroot)?;
    let tl = c.source.join("ThunkLibs");
    let suffix = if is_32 { "_32" } else { "" };
    let generated = if is_32 { "gen_32" } else { "gen_64" };
    let out = c.out.join(format!("HostThunks{suffix}"));
    fs::create_dir_all(&out).map_err(|e| format!("create {}: {e}", out.display()))?;
    log(&format!(
        "== host{suffix}: {}/libvulkan-host.so (aarch64 Linux ELF)",
        out.display()
    ));
    let mut args = vec![
        "--target=aarch64-unknown-linux-gnu".into(),
        format!("--sysroot={}", c.frame_sysroot.display()).into(),
        format!("--gcc-install-dir={}", gcc.display()).into(),
        "-std=c++20".into(),
        "-O2".into(),
        "-g0".into(),
        "-fPIC".into(),
        "-fwrapv".into(),
        "-ffixed-x18".into(),
        "-DARCHITECTURE_arm64=1".into(),
    ];
    if is_32 {
        args.push("-DIS_32BIT_THUNK".into());
    }
    args.extend([
        "-I".into(),
        c.out.join(generated).into_os_string(),
        "-isystem".into(),
        tl.join("include").into_os_string(),
        "-isystem".into(),
        c.source
            .join("External/Vulkan-Headers/include")
            .into_os_string(),
        "-isystem".into(),
        c.xinc.clone().into_os_string(),
        "-shared".into(),
        "-fuse-ld=lld".into(),
        format!("--ld-path={}", c.lld.display()).into(),
        "-Wl,--build-id=sha1".into(),
        "-Wl,--no-undefined".into(),
        "-Wl,-soname,libvulkan-host.so".into(),
        "-o".into(),
        out.join("libvulkan-host.so").into_os_string(),
        tl.join("libvulkan/Host.cpp").into_os_string(),
        "-ldl".into(),
    ]);
    run(&c.llvm_bin.join("clang++"), &args, Some(&c.repo))?;
    readelf(
        c,
        &out.join("libvulkan-host.so"),
        &["-h", "-d"],
        &["Machine", "NEEDED"],
    )
}

fn do_guest(c: &Config, is_32: bool) -> Result<(), String> {
    let tl = c.source.join("ThunkLibs");
    let suffix = if is_32 { "_32" } else { "" };
    let (arch, sysroot, generated) = if is_32 {
        ("i686-linux-gnu", &c.x86root32, "gen32")
    } else {
        ("x86_64-linux-gnu", &c.x86root, "gen")
    };
    let out = c.out.join(format!("GuestThunks{suffix}"));
    fs::create_dir_all(&out).map_err(|e| format!("create {}: {e}", out.display()))?;
    log(&format!(
        "== guest{suffix}: {}/libvulkan-guest.so ({arch} ELF)",
        out.display()
    ));
    let mut args = vec![
        format!("--target={arch}").into(),
        format!("--sysroot={}", sysroot.display()).into(),
    ];
    if is_32 {
        args.push("-m32".into());
    }
    args.extend([
        "-std=c++20".into(),
        "-O2".into(),
        "-g0".into(),
        "-fPIC".into(),
        "-fwrapv".into(),
        "-msse2".into(),
        "-mfpmath=sse".into(),
        "-DGUEST_THUNK_LIBRARY".into(),
    ]);
    if is_32 {
        args.push("-DIS_32BIT_THUNK".into());
    }
    args.extend([
        "-I".into(),
        c.out.join(generated).into_os_string(),
        "-isystem".into(),
        tl.join("include").into_os_string(),
        "-isystem".into(),
        c.source
            .join("External/Vulkan-Headers/include")
            .into_os_string(),
        "-shared".into(),
        "-fuse-ld=lld".into(),
        format!("--ld-path={}", c.lld.display()).into(),
        "-Wl,--build-id=sha1".into(),
    ]);
    if is_32 {
        let script = tl.join("libvulkan/libvulkan_Guest_32.lds");
        if script.is_file() {
            args.push(format!("-Wl,-T,{}", script.display()).into());
        }
    }
    args.extend([
        "-Wl,-soname,libvulkan.so.1".into(),
        "-Wl,-z,nodelete".into(),
        "-o".into(),
        out.join("libvulkan-guest.so").into_os_string(),
        tl.join("libvulkan/Guest.cpp").into_os_string(),
    ]);
    run(&c.llvm_bin.join("clang++"), &args, Some(&c.repo))?;
    readelf(
        c,
        &out.join("libvulkan-guest.so"),
        &["-h", "-d"],
        &["Machine", "NEEDED", "SONAME"],
    )
}

fn do_gen32(c: &Config) -> Result<(), String> {
    require_frame(c)?;
    let tl = c.source.join("ThunkLibs");
    let iface = tl.join("libvulkan/libvulkan_interface.cpp");
    let generator = c.out.join("thunkgen/thunkgen");
    let incs = include_args(c);
    for dir in ["gen32", "gen_32"] {
        fs::create_dir_all(c.out.join(dir)).map_err(|e| e.to_string())?;
    }
    log("== gen32: guest (i386)");
    let mut guest = vec![
        iface.clone().into_os_string(),
        "libvulkan".into(),
        "-guest".into(),
        c.out
            .join("gen32/thunkgen_guest_libvulkan.inl")
            .into_os_string(),
        c.x86root32.clone().into_os_string(),
        "-for-32bit-guest".into(),
        "--".into(),
        "-std=c++20".into(),
        "-m32".into(),
        "--target=i686-linux-gnu".into(),
        "--sysroot".into(),
        c.x86root32.clone().into_os_string(),
        "-DIS_32BIT_THUNK".into(),
    ];
    guest.extend(incs.clone());
    guest.extend(["-isystem".into(), c.xinc.clone().into_os_string()]);
    run_env(
        &generator,
        &guest,
        Some(&c.repo),
        &[("THUNKGEN_SKIP_UNREPACKABLE", "1")],
    )
    .map_err(|e| format!("32-bit guest thunkgen: {e}"))?;
    log("== gen32: host (aarch64-linux, for i386 guests)");
    let mut host = vec![
        iface.into_os_string(),
        "libvulkan".into(),
        "-host".into(),
        c.out
            .join("gen_32/thunkgen_host_libvulkan.inl")
            .into_os_string(),
        c.x86root32.clone().into_os_string(),
        "-for-32bit-guest".into(),
        "--".into(),
        "-std=c++20".into(),
        "--target=aarch64-unknown-linux-gnu".into(),
        "--sysroot".into(),
        c.frame_sysroot.clone().into_os_string(),
        "-DARCHITECTURE_arm64=1".into(),
        "-DIS_32BIT_THUNK".into(),
    ];
    host.extend(incs);
    host.extend(["-isystem".into(), c.xinc.clone().into_os_string()]);
    run_env(
        &generator,
        &host,
        Some(&c.repo),
        &[("THUNKGEN_SKIP_UNREPACKABLE", "1")],
    )
    .map_err(|e| format!("32-bit host thunkgen: {e}"))?;
    log_generated(c, &["gen32", "gen_32"]);
    Ok(())
}

fn log_generated(c: &Config, dirs: &[&str]) {
    let mut count = 0u64;
    for dir in dirs {
        if let Ok(entries) = fs::read_dir(c.out.join(dir)) {
            for entry in entries
                .flatten()
                .filter(|e| e.path().extension().is_some_and(|x| x == "inl"))
            {
                count += fs::read_to_string(entry.path())
                    .map(|s| s.lines().count() as u64)
                    .unwrap_or(0);
            }
        }
    }
    log(&format!("  generated {count} thunk lines"));
}

fn do_probe(c: &Config) -> Result<(), String> {
    fs::create_dir_all(c.out.join("x86")).map_err(|e| format!("create probe output: {e}"))?;
    log("== probe: tests/elf/vk_shim_consumer.c -> x86-64");
    let args = vec![
        "--target=x86_64-linux-gnu".into(),
        format!("--sysroot={}", c.x86root.display()).into(),
        "-O2".into(),
        "-fPIE".into(),
        "-pie".into(),
        "-fuse-ld=lld".into(),
        format!("--ld-path={}", c.lld.display()).into(),
        "-o".into(),
        c.out.join("x86/vkshim_test_x86_64").into_os_string(),
        c.repo.join("tests/elf/vk_shim_consumer.c").into_os_string(),
        c.out
            .join("GuestThunks/libvulkan-guest.so")
            .into_os_string(),
    ];
    run(&c.llvm_bin.join("clang"), &args, Some(&c.repo))
}

fn install(c: &Config) -> Result<(), String> {
    let root = env::var_os("LXRT_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/tmp/lxrt-root"));
    let real = fs::canonicalize(&root).map_err(|e| format!("no root {}: {e}", root.display()))?;
    if real.to_string_lossy().contains("steamroot")
        && env::var("STEAMARM_THUNKS_STEAMROOT").as_deref() != Ok("1")
    {
        return Err(format!(
            "refusing to install into Steam root {}; set STEAMARM_THUNKS_STEAMROOT=1 to allow",
            real.display()
        ));
    }
    let destination = root.join("opt/vkthunks");
    log(&format!("== install -> {}", destination.display()));
    for dir in ["HostThunks", "GuestThunks", "config", "x86"] {
        fs::create_dir_all(destination.join(dir))
            .map_err(|e| format!("create destination: {e}"))?;
    }
    for (from, to) in [
        (
            c.out.join("HostThunks/libvulkan-host.so"),
            destination.join("HostThunks/libvulkan-host.so"),
        ),
        (
            c.out.join("GuestThunks/libvulkan-guest.so"),
            destination.join("GuestThunks/libvulkan-guest.so"),
        ),
    ] {
        fs::copy(&from, &to).map_err(|e| format!("copy {}: {e}", from.display()))?;
    }
    let probe = c.out.join("x86/vkshim_test_x86_64");
    if probe.is_file() {
        fs::copy(&probe, destination.join("x86/vkshim_test_x86_64"))
            .map_err(|e| format!("copy probe: {e}"))?;
    }
    fs::copy(
        c.source.join("Data/ThunksDB.json"),
        destination.join("config/ThunksDB.json"),
    )
    .map_err(|e| format!("copy FEX ThunksDB: {e}"))?;
    fs::write(destination.join("config/Config.json"),
        "{\"Config\":{\"RootFS\":\"/tmp/fexhome/.local/share/fex-emu/RootFS/Ubuntu_24_04\"},\"ThunksDB\":{\"Vulkan\":1}}\n")
        .map_err(|e| format!("write FEX Vulkan config: {e}"))?;
    for entry in fs::read_dir(&destination).map_err(|e| format!("list install: {e}"))? {
        let path = entry.map_err(|e| e.to_string())?.path();
        if path.is_dir() {
            for item in fs::read_dir(path).map_err(|e| e.to_string())? {
                let item = item.map_err(|e| e.to_string())?.path();
                if item.is_file() {
                    println!("  {}", item.display());
                }
            }
        } else if path.is_file() {
            println!("  {}", path.display());
        }
    }
    Ok(())
}

fn summary(c: &Config) -> Result<(), String> {
    log(&format!("== {}", c.out.display()));
    for path in [
        c.out.join("thunkgen/thunkgen"),
        c.out.join("HostThunks/libvulkan-host.so"),
        c.out.join("GuestThunks/libvulkan-guest.so"),
        c.out.join("x86/vkshim_test_x86_64"),
    ] {
        if !path.is_file() {
            continue;
        }
        let size = fs::metadata(&path).map_err(|e| e.to_string())?.len();
        log(&format!(
            "  {:<22} {:>9} bytes  sha256 {}",
            path.file_name().unwrap_or_default().to_string_lossy(),
            size,
            hash(&path)?
        ));
    }
    Ok(())
}

fn run_all() -> Result<(), String> {
    let steps = env::args().skip(1).collect::<Vec<_>>();
    if steps.iter().any(|step| step == "-h" || step == "--help") {
        println!("Usage: build-fex-thunks.sh [source|x86root|x86root32|thunkgen|gen|host|guest|gen32|host32|guest32|probe|install|all]...");
        return Ok(());
    }
    let c = config()?;
    check_tools(&c)?;
    let steps = if steps.is_empty() {
        vec!["all".to_owned()]
    } else {
        steps
    };
    for step in &steps {
        match step.as_str() {
            "source" => source(&c)?,
            "x86root" => make_sysroot(&c, &c.x86root, "x86_64-linux-gnu", DEBS64, true)?,
            "x86root32" => make_sysroot(&c, &c.x86root32, "i386-linux-gnu", DEBS32, false)?,
            "thunkgen" => do_thunkgen(&c)?,
            "gen" => do_gen(&c)?,
            "host" => do_host(&c, false)?,
            "guest" => do_guest(&c, false)?,
            "gen32" => do_gen32(&c)?,
            "host32" => do_host(&c, true)?,
            "guest32" => do_guest(&c, true)?,
            "probe" => do_probe(&c)?,
            "install" => install(&c)?,
            "all" => {
                source(&c)?;
                make_sysroot(&c, &c.x86root, "x86_64-linux-gnu", DEBS64, true)?;
                make_sysroot(&c, &c.x86root32, "i386-linux-gnu", DEBS32, false)?;
                do_thunkgen(&c)?;
                do_gen(&c)?;
                do_host(&c, false)?;
                do_guest(&c, false)?;
                do_gen32(&c)?;
                do_host(&c, true)?;
                do_guest(&c, true)?;
                do_probe(&c)?;
            }
            _ => return Err(format!("unknown step {step}")),
        }
    }
    summary(&c)
}

fn main() {
    if let Err(message) = run_all() {
        error(message);
    }
}
