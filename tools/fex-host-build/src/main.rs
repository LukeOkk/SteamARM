use std::cmp::Ordering;
use std::env;
use std::ffi::{OsStr, OsString};
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Output, Stdio};
use std::time::Instant;

const FEX_URL: &str = "https://github.com/FEX-Emu/FEX.git";
const FEX_COMMIT: &str = "08f451d3bce62a68aa6307dced5a8eaeb85c39bc";
const SUBMODULES: &[&str] = &[
    "External/fmt",
    "External/range-v3",
    "External/rpmalloc",
    "External/drm-headers",
    "External/xxhash",
    "External/unordered_dense",
    "Source/Common/cpp-optparse",
];
const PATCHES: &[&str] = &[
    "fex-lxrt-x18.patch",
    "fex-lxrt-guest-base.patch",
    "fex-lxrt-sbrk-page-align.patch",
    "fex-lxrt-wx.patch",
    "fex-lxrt-hide-hypervisor.patch",
    "fex-lxrt-shebang.patch",
    "fex-lxrt-guest-reserve.patch",
    "fex-lxrt-thunk-args.patch",
    "fex-lxrt-arch-prctl.patch",
    "fex-lxrt-lowwin-hint.patch",
    "fex-lxrt-smc-mprotect-mirrors.patch",
    "fex-lxrt-interrupt-page.patch",
    "fex-lxrt-seccomp-cap-sys-admin.patch",
    "fex-lxrt-seccomp-trap-skip.patch",
    "fex-lxrt-guest-ld-preload.patch",
    "fex-lxrt-sigsuspend-pending.patch",
    "fex-lxrt-i386-bionic.patch",
    "fex-lxrt-execve-no-fd.patch",
    "fex-upstream-cpuid-affinity.patch",
];
const TARGETS: &[&str] = &["FEX", "FEXServer", "FEXGetConfig"];
const EMU_PREFIX: &str = "/usr/lib/lxrt-emu";

struct Config {
    repo: PathBuf,
    work: PathBuf,
    sysroot: PathBuf,
    src: PathBuf,
    build: PathBuf,
    out: PathBuf,
    state: PathBuf,
    marker: PathBuf,
    llvm_bin: PathBuf,
    lld: PathBuf,
    toolchain: PathBuf,
    jobs: String,
    build_types: Vec<String>,
    lto: String,
    tune_cpu: String,
}

fn fail(error: impl AsRef<str>) -> ! {
    eprintln!("steamarm-fex-host-build: error: {}", error.as_ref());
    std::process::exit(1);
}

fn log(message: &str) {
    println!("{message}");
}

fn selected_dir(home: &Path, env_name: &str, name: &str) -> PathBuf {
    if let Some(value) = env::var_os(env_name).filter(|v| !v.is_empty()) {
        return PathBuf::from(value);
    }
    let developer = home.join("Developer").join(name);
    let legacy = home.join(name);
    if developer.is_dir() || !legacy.is_dir() {
        developer
    } else {
        legacy
    }
}

fn config() -> Result<Config, String> {
    let home = env::var_os("HOME")
        .map(PathBuf::from)
        .ok_or("HOME is not set")?;
    let repo = env::var_os("STEAMARM_PROJECT_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from(env!("CARGO_MANIFEST_DIR")).join("../.."));
    let work = selected_dir(&home, "STEAMARM_BUILD", "SteamARM-build");
    let roots = selected_dir(&home, "STEAMARM_STATE", "SteamARM-roots");
    let sysroot = env::var_os("STEAMARM_FRAME_SYSROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| roots.join("arm64root"));
    let llvm_bin = env::var_os("LLVM_BIN")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/opt/homebrew/opt/llvm/bin"));
    let lld = env::var_os("LLD")
        .filter(|s| !s.is_empty())
        .map(PathBuf::from)
        .or_else(|| {
            let nearby = llvm_bin.join("ld.lld");
            nearby.is_file().then_some(nearby)
        })
        .or_else(|| find_in_path("ld.lld"))
        .unwrap_or_default();
    let jobs = env::var("JOBS")
        .ok()
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| {
            capture("sysctl", &["-n", "hw.ncpu"])
                .filter(|s| !s.is_empty())
                .unwrap_or_else(|| "8".into())
        });
    let build_types = env::var("FEX_BUILD_TYPES")
        .unwrap_or_else(|_| "Release RelWithDebInfo".into())
        .split_whitespace()
        .map(str::to_owned)
        .collect::<Vec<_>>();
    Ok(Config {
        repo: repo.clone(),
        work: work.clone(),
        sysroot,
        src: work.join("FEX"),
        build: work.join("build"),
        out: work.join("out"),
        state: work.join("state"),
        marker: work.join("state/frame-sysroot.sha256"),
        llvm_bin,
        lld,
        toolchain: repo.join("scripts/toolchain-aarch64-linux-steamos.cmake"),
        jobs,
        build_types,
        lto: env::var("FEX_LTO").unwrap_or_else(|_| "ON".into()),
        tune_cpu: env::var("FEX_TUNE_CPU").unwrap_or_else(|_| "apple-m1".into()),
    })
}

fn find_in_path(name: &str) -> Option<PathBuf> {
    env::split_paths(&env::var_os("PATH")?)
        .map(|dir| dir.join(name))
        .find(|p| {
            fs::metadata(p).is_ok_and(|m| {
                m.is_file() && {
                    use std::os::unix::fs::PermissionsExt;
                    m.permissions().mode() & 0o111 != 0
                }
            })
        })
}

fn executable(path: &Path) -> bool {
    fs::metadata(path).is_ok_and(|m| {
        use std::os::unix::fs::PermissionsExt;
        m.is_file() && m.permissions().mode() & 0o111 != 0
    })
}

fn command(
    program: &OsStr,
    args: &[OsString],
    cwd: Option<&Path>,
    envs: &[(&str, &OsStr)],
) -> Result<Output, String> {
    let mut cmd = Command::new(program);
    cmd.args(args);
    if let Some(cwd) = cwd {
        cmd.current_dir(cwd);
    }
    for (key, value) in envs {
        cmd.env(key, value);
    }
    cmd.output()
        .map_err(|e| format!("cannot run {}: {e}", program.to_string_lossy()))
}

fn run(
    program: &OsStr,
    args: &[OsString],
    cwd: Option<&Path>,
    envs: &[(&str, &OsStr)],
) -> Result<(), String> {
    let mut cmd = Command::new(program);
    cmd.args(args);
    if let Some(cwd) = cwd {
        cmd.current_dir(cwd);
    }
    for (key, value) in envs {
        cmd.env(key, value);
    }
    let status = cmd
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

fn capture(program: &str, args: &[&str]) -> Option<String> {
    let output = Command::new(program).args(args).output().ok()?;
    output
        .status
        .success()
        .then(|| String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

fn hash_bytes(bytes: &[u8]) -> Result<String, String> {
    let mut child = Command::new("shasum")
        .args(["-a", "256"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("cannot run shasum: {e}"))?;
    child
        .stdin
        .take()
        .ok_or("shasum stdin unavailable")?
        .write_all(bytes)
        .map_err(|e| format!("write hash input: {e}"))?;
    let output = child
        .wait_with_output()
        .map_err(|e| format!("wait for shasum: {e}"))?;
    if !output.status.success() {
        return Err(format!(
            "shasum failed: {}",
            String::from_utf8_lossy(&output.stderr)
        ));
    }
    String::from_utf8_lossy(&output.stdout)
        .split_whitespace()
        .next()
        .map(str::to_owned)
        .ok_or_else(|| "shasum returned no digest".into())
}

fn hash_file(path: &Path) -> Result<String, String> {
    let output = Command::new("shasum")
        .args(["-a", "256"])
        .arg(path)
        .output()
        .map_err(|e| format!("cannot hash {}: {e}", path.display()))?;
    if !output.status.success() {
        return Err(format!("shasum failed for {}", path.display()));
    }
    String::from_utf8_lossy(&output.stdout)
        .split_whitespace()
        .next()
        .map(str::to_owned)
        .ok_or_else(|| "shasum returned no digest".into())
}

fn version_cmp(a: &OsStr, b: &OsStr) -> Ordering {
    let parse = |s: &OsStr| {
        s.to_string_lossy()
            .split(['.', '-', '_'])
            .map(|part| part.parse::<u64>().unwrap_or(0))
            .collect::<Vec<_>>()
    };
    parse(a).cmp(&parse(b))
}

fn latest_gcc_dir(root: &Path) -> Result<PathBuf, String> {
    let base = root.join("usr/lib/gcc/aarch64-unknown-linux-gnu");
    let mut dirs = fs::read_dir(&base)
        .map_err(|e| format!("read GCC directories {}: {e}", base.display()))?
        .filter_map(Result::ok)
        .filter(|entry| entry.path().is_dir())
        .map(|entry| entry.path())
        .collect::<Vec<_>>();
    dirs.sort_by(|a, b| {
        version_cmp(
            a.file_name().unwrap_or_default(),
            b.file_name().unwrap_or_default(),
        )
    });
    dirs.pop()
        .ok_or_else(|| format!("no GCC support directory in {}", base.display()))
}

fn check_tools(c: &Config) -> Result<(), String> {
    for path in [
        c.llvm_bin.join("clang"),
        c.llvm_bin.join("clang++"),
        c.llvm_bin.join("llvm-ar"),
        c.lld.clone(),
    ] {
        if !executable(&path) {
            return Err(format!(
                "missing {} (install Homebrew llvm and lld)",
                path.display()
            ));
        }
    }
    for tool in ["cmake", "ninja", "git", "curl", "python3", "shasum"] {
        if find_in_path(tool).is_none() {
            return Err(format!("missing {tool}"));
        }
    }
    Ok(())
}

fn sysroot(c: &Config) -> Result<(), String> {
    log(&format!(
        "== sysroot: Steam Frame SteamOS (Arch-based) -> {}",
        c.sysroot.display()
    ));
    let os_release = c.sysroot.join("etc/os-release");
    let marker = c.sysroot.join(".lxrt-frameroot");
    if !marker.is_file() {
        return Err(format!(
            "Steam Frame root missing at {}",
            c.sysroot.display()
        ));
    }
    let os = fs::read_to_string(&os_release)
        .map_err(|e| format!("read {}: {e}", os_release.display()))?;
    if !os.lines().any(|line| line == "ID=steamos") {
        return Err(format!(
            "{} is not the Steam Frame SteamOS root",
            c.sysroot.display()
        ));
    }
    let gcc = latest_gcc_dir(&c.sysroot)?;
    for path in [
        c.sysroot.join("usr/include/stdio.h"),
        c.sysroot.join("usr/include/gdb/jit-reader.h"),
        gcc.join("crtbeginS.o"),
        c.sysroot.join("usr/lib/ld-linux-aarch64.so.1"),
        c.sysroot.join("usr/lib/libc.so.6"),
        c.sysroot.join("usr/lib/libstdc++.so.6"),
        c.sysroot.join("usr/lib/libgcc_s.so.1"),
    ] {
        if !path.exists() {
            return Err(format!(
                "Steam Frame sysroot lacks required file {}",
                path.display()
            ));
        }
    }
    let mut fingerprint = fs::read(&os_release).map_err(|e| e.to_string())?;
    fingerprint.extend(fs::read(&marker).map_err(|e| e.to_string())?);
    for path in [
        c.sysroot.join("usr/lib/ld-linux-aarch64.so.1"),
        c.sysroot.join("usr/lib/libc.so.6"),
        c.sysroot.join("usr/lib/libstdc++.so.6"),
        c.sysroot.join("usr/lib/libgcc_s.so.1"),
        gcc.join("crtbeginS.o"),
    ] {
        fingerprint.extend(hash_file(&path)?.as_bytes());
        fingerprint.extend(path.as_os_str().as_encoded_bytes());
        fingerprint.push(b'\n');
    }
    let digest = hash_bytes(&fingerprint)?;
    fs::create_dir_all(&c.state).map_err(|e| format!("create {}: {e}", c.state.display()))?;
    let temp = c.marker.with_extension("sha256.tmp");
    fs::write(&temp, format!("{digest}\n"))
        .map_err(|e| format!("write {}: {e}", temp.display()))?;
    fs::rename(&temp, &c.marker).map_err(|e| format!("install sysroot fingerprint: {e}"))?;
    log(&format!(
        "  read-only sysroot ready; GCC {}",
        gcc.file_name().unwrap_or_default().to_string_lossy()
    ));
    Ok(())
}

fn git(c: &Config, args: &[OsString]) -> Result<Output, String> {
    command(OsStr::new("git"), args, Some(&c.src), &[])
}

fn do_source(c: &Config) -> Result<(), String> {
    log(&format!(
        "== FEX source: {FEX_COMMIT} -> {}",
        c.src.display()
    ));
    if !c.src.join(".git").is_dir() {
        fs::create_dir_all(&c.work).map_err(|e| format!("create {}: {e}", c.work.display()))?;
        let args = vec![
            "--filter=blob:none".into(),
            "--no-checkout".into(),
            FEX_URL.into(),
            c.src.clone().into_os_string(),
        ];
        run(OsStr::new("git"), &args, Some(&c.work), &[])?;
    }
    let commit_ref = OsString::from(format!("{FEX_COMMIT}^{{commit}}"));
    let cat = git(
        c,
        &[OsString::from("cat-file"), OsString::from("-e"), commit_ref],
    );
    if cat.is_err() || !cat.as_ref().is_ok_and(|o| o.status.success()) {
        run(
            OsStr::new("git"),
            &["fetch".into(), "--tags".into(), "origin".into()],
            Some(&c.src),
            &[],
        )?;
    }

    let mut source_hash = format!("{FEX_COMMIT}\n").into_bytes();
    for patch in PATCHES {
        let bytes = fs::read(c.repo.join("patches").join(patch))
            .map_err(|e| format!("read FEX patch {patch}: {e}"))?;
        source_hash.extend(bytes);
    }
    source_hash.extend(
        fs::read(c.repo.join("patches/LxrtJit.h")).map_err(|e| format!("read LxrtJit.h: {e}"))?,
    );
    let wanted = hash_bytes(&source_hash)?;
    let current = fs::read_to_string(c.state.join("patched"))
        .unwrap_or_default()
        .trim()
        .to_owned();
    let head = git(c, &["rev-parse".into(), "HEAD".into()])?;
    let head = String::from_utf8_lossy(&head.stdout).trim().to_owned();
    if current == wanted && head == FEX_COMMIT {
        log("  tree at pinned FEX commit with current patches");
    } else {
        let _ = fs::remove_file(c.state.join("patched"));
        run(
            OsStr::new("git"),
            &[
                "checkout".into(),
                "-q".into(),
                "-f".into(),
                "--detach".into(),
                FEX_COMMIT.into(),
            ],
            Some(&c.src),
            &[],
        )?;
        run(
            OsStr::new("git"),
            &["clean".into(), "-fdxq".into()],
            Some(&c.src),
            &[],
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
                Some(&c.src),
                &[],
            )?;
        }
        let header = c.src.join("FEXCore/include/FEXCore/Utils/LxrtJit.h");
        fs::create_dir_all(header.parent().ok_or("invalid FEX header path")?)
            .map_err(|e| format!("create FEX header directory: {e}"))?;
        fs::copy(c.repo.join("patches/LxrtJit.h"), &header)
            .map_err(|e| format!("copy LxrtJit.h: {e}"))?;
        fs::create_dir_all(&c.state).map_err(|e| format!("create FEX state: {e}"))?;
        fs::write(c.state.join("patched"), format!("{wanted}\n"))
            .map_err(|e| format!("write FEX patch stamp: {e}"))?;
    }
    let mut sub_args = vec![
        "submodule".into(),
        "update".into(),
        "--init".into(),
        "--depth".into(),
        "1".into(),
        "--".into(),
    ];
    sub_args.extend(SUBMODULES.iter().map(OsString::from));
    let shallow = run(OsStr::new("git"), &sub_args, Some(&c.src), &[]).is_ok();
    if !shallow {
        let mut retry = vec![
            "submodule".into(),
            "update".into(),
            "--init".into(),
            "--".into(),
        ];
        retry.extend(SUBMODULES.iter().map(OsString::from));
        run(OsStr::new("git"), &retry, Some(&c.src), &[])?;
    }
    let version = git(
        c,
        &["describe".into(), "--tags".into(), "--abbrev=9".into()],
    )?;
    let changed = git(c, &["status".into(), "--porcelain".into()])?;
    let changed_count = String::from_utf8_lossy(&changed.stdout).lines().count();
    log(&format!(
        "  {}, {changed_count} changed/new files",
        String::from_utf8_lossy(&version.stdout).trim()
    ));
    Ok(())
}

fn hash_stamp(c: &Config, args: &[OsString], cflags: &str, epoch: &str) -> Result<String, String> {
    let mut input = fs::read(&c.marker).map_err(|e| format!("read sysroot fingerprint: {e}"))?;
    input.extend(fs::read(&c.toolchain).map_err(|e| format!("read toolchain: {e}"))?);
    input.extend(format!("{cflags}\n{epoch}\n").bytes());
    for arg in args {
        input.extend(arg.as_encoded_bytes());
        input.push(b'\n');
    }
    hash_bytes(&input)
}

fn missing_cached_tool(cache: &Path) -> Option<String> {
    let content = fs::read_to_string(cache).ok()?;
    content.lines().find_map(|line| {
        let (_, value) = line.split_once(":FILEPATH=")?;
        (value.starts_with('/') && !Path::new(value).exists()).then(|| value.to_owned())
    })
}

fn build(c: &Config) -> Result<(), String> {
    let cflags = "-ffixed-x18";
    let epoch = git(
        c,
        &[
            "log".into(),
            "-1".into(),
            "--format=%ct".into(),
            FEX_COMMIT.into(),
        ],
    )?;
    let epoch = String::from_utf8_lossy(&epoch.stdout).trim().to_owned();
    for build_type in &c.build_types {
        let dir = c.build.join(build_type);
        let mut args = vec![
            "-G".into(),
            "Ninja".into(),
            format!("-DCMAKE_TOOLCHAIN_FILE={}", c.toolchain.display()).into(),
            format!("-DSTEAMOS_SYSROOT={}", c.sysroot.display()).into(),
            format!("-DLLVM_BIN={}", c.llvm_bin.display()).into(),
            format!("-DLLD={}", c.lld.display()).into(),
            format!("-DCMAKE_BUILD_TYPE={build_type}").into(),
            "-DBUILD_TESTING=OFF".into(),
            "-DBUILD_THUNKS=OFF".into(),
            "-DBUILD_FEXCONFIG=OFF".into(),
            // Steam's FEXServer uses per-app sockets and lock paths. Match
            // Valve's Steam build so this interpreter cannot attach to an
            // unrelated global FEXServer left by another rootfs.
            "-DBUILD_STEAM_SUPPORT=ON".into(),
            "-DCMAKE_DISABLE_FIND_PACKAGE_fmt=TRUE".into(),
            format!("-DENABLE_LTO={}", c.lto).into(),
            "-DENABLE_FEX_ALLOCATOR=ON".into(),
            "-DENABLE_JEMALLOC_GLIBC_ALLOC=OFF".into(),
            "-DENABLE_GDB_SYMBOLS=ON".into(),
            format!("-DTUNE_CPU={}", c.tune_cpu).into(),
            "-DTUNE_ARCH=generic".into(),
            "-DUSE_LEGACY_BINFMTMISC=OFF".into(),
            "-DENABLE_CCACHE=OFF".into(),
        ];
        let stamp = hash_stamp(c, &args, cflags, &epoch)?;
        let cache = dir.join("CMakeCache.txt");
        let gone = missing_cached_tool(&cache);
        if let Some(path) = gone.as_ref() {
            log(&format!(
                "{path} is gone (toolchain upgrade?): reconfigure {build_type}"
            ));
        }
        if !dir.join("build.ninja").is_file()
            || fs::read_to_string(dir.join(".lxrt-config"))
                .unwrap_or_default()
                .trim()
                != stamp
            || gone.is_some()
        {
            if dir.exists() {
                fs::remove_dir_all(&dir).map_err(|e| format!("clear {}: {e}", dir.display()))?;
            }
            log(&format!("== configure {build_type} -> {}", dir.display()));
            fs::create_dir_all(&c.build).map_err(|e| format!("create build directory: {e}"))?;
            let mut configure = vec![
                "-S".into(),
                c.src.clone().into_os_string(),
                "-B".into(),
                dir.clone().into_os_string(),
            ];
            configure.append(&mut args);
            run(
                OsStr::new("cmake"),
                &configure,
                Some(&c.repo),
                &[
                    ("CFLAGS", OsStr::new(cflags)),
                    ("CXXFLAGS", OsStr::new(cflags)),
                    ("SOURCE_DATE_EPOCH", OsStr::new(&epoch)),
                ],
            )?;
            fs::write(dir.join(".lxrt-config"), format!("{stamp}\n"))
                .map_err(|e| format!("write build stamp: {e}"))?;
        }
        let cache_text =
            fs::read_to_string(&cache).map_err(|e| format!("read {}: {e}", cache.display()))?;
        if !cache_text.contains("-ffixed-x18") {
            return Err(format!(
                "{} lost CMAKE_CXX_FLAGS=-ffixed-x18",
                cache.display()
            ));
        }
        log(&format!(
            "== build {build_type}: {} (-j{})",
            TARGETS.join(" "),
            c.jobs
        ));
        let start = Instant::now();
        let mut ninja_args = vec![
            "-C".into(),
            dir.clone().into_os_string(),
            "-j".into(),
            c.jobs.clone().into(),
        ];
        ninja_args.extend(TARGETS.iter().map(OsString::from));
        run(
            OsStr::new("ninja"),
            &ninja_args,
            Some(&c.repo),
            &[("SOURCE_DATE_EPOCH", OsStr::new(&epoch))],
        )?;
        log(&format!(
            "  {build_type}: targets up to date after {} s",
            start.elapsed().as_secs()
        ));
    }
    fs::create_dir_all(&c.out).map_err(|e| format!("create output: {e}"))?;
    for target in TARGETS {
        for (build_type, suffix) in [("Release", ""), ("RelWithDebInfo", "-dbg")] {
            let source = c.build.join(build_type).join("Bin").join(target);
            if source.is_file() {
                let dest_name = if suffix.is_empty() {
                    (*target).to_owned()
                } else {
                    format!("{target}{suffix}")
                };
                fs::copy(&source, c.out.join(dest_name))
                    .map_err(|e| format!("copy {}: {e}", source.display()))?;
            }
        }
    }
    Ok(())
}

fn emu(c: &Config) -> Result<(), String> {
    let build = c.build.join("Release");
    let ninja = build.join("build.ninja");
    if !ninja.is_file() {
        return Err(format!("no Release build in {}", build.display()));
    }
    log(&format!(
        "== FEX-emu: relink with PT_INTERP/DT_RPATH in {EMU_PREFIX}"
    ));
    let out = c.out.join("FEX-emu");
    let source = build.join("Bin/FEX");
    if out.is_file()
        && fs::metadata(&out).and_then(|m| m.modified()).ok()
            > fs::metadata(&source).and_then(|m| m.modified()).ok()
    {
        log("  up to date");
        return Ok(());
    }
    let output = Command::new("ninja")
        .args([
            "-C",
            build.to_str().unwrap_or_default(),
            "-t",
            "commands",
            "Bin/FEX",
        ])
        .output()
        .map_err(|e| format!("get FEX link command: {e}"))?;
    if !output.status.success() {
        return Err("ninja could not return FEX link command".into());
    }
    let mut link = String::from_utf8_lossy(&output.stdout)
        .lines()
        .last()
        .unwrap_or_default()
        .to_owned();
    if !link.contains(" -o Bin/FEX ") {
        return Err("unexpected FEX link command".into());
    }
    while let Some(start) = link.find(" -Xlinker --dependency-file=") {
        let end = link[start + 1..]
            .find(' ')
            .map(|n| start + 1 + n)
            .unwrap_or(link.len());
        link.replace_range(start..end, "");
    }
    while let Some(start) = link.find(" --dependency-file=") {
        let end = link[start + 1..]
            .find(' ')
            .map(|n| start + 1 + n)
            .unwrap_or(link.len());
        link.replace_range(start..end, "");
    }
    let replacement = format!(
        " -o {} -Wl,--dynamic-linker={}/ld-linux-aarch64.so.1 -Wl,-rpath,{} -Wl,--disable-new-dtags ",
        out.display(), EMU_PREFIX, EMU_PREFIX
    );
    link = link.replacen(" -o Bin/FEX ", &replacement, 1);
    fs::create_dir_all(&c.out).map_err(|e| format!("create output directory: {e}"))?;
    let status = Command::new("sh")
        .arg("-c")
        .arg(&link)
        .current_dir(&build)
        .status()
        .map_err(|e| format!("relink FEX-emu: {e}"))?;
    if !status.success() {
        return Err(format!("FEX-emu link failed with {status}"));
    }
    let readelf = c.llvm_bin.join("llvm-readelf");
    let details = Command::new(readelf)
        .args(["-l", "-d"])
        .arg(&out)
        .output()
        .map_err(|e| format!("inspect FEX-emu: {e}"))?;
    if !details.status.success() {
        return Err("llvm-readelf failed for FEX-emu".into());
    }
    for line in String::from_utf8_lossy(&details.stdout).lines() {
        if line.contains("interpreter") || line.contains("RPATH") || line.contains("RUNPATH") {
            println!("  {line}");
        }
    }
    Ok(())
}

fn summary(c: &Config) -> Result<(), String> {
    log(&format!("== {}", c.out.display()));
    if !c.out.is_dir() {
        return Ok(());
    }
    let mut files = fs::read_dir(&c.out)
        .map_err(|e| format!("read output directory: {e}"))?
        .filter_map(Result::ok)
        .map(|entry| entry.path())
        .filter(|p| p.is_file())
        .collect::<Vec<_>>();
    files.sort();
    for file in files {
        let size = fs::metadata(&file)
            .map_err(|e| format!("stat {}: {e}", file.display()))?
            .len();
        log(&format!(
            "  {:<18} {:>10} bytes  sha256 {}",
            file.file_name().unwrap_or_default().to_string_lossy(),
            size,
            hash_file(&file)?
        ));
    }
    Ok(())
}

fn run_all() -> Result<(), String> {
    let c = config()?;
    check_tools(&c)?;
    let steps = env::args().skip(1).collect::<Vec<_>>();
    let steps = if steps.is_empty() {
        vec!["all".to_owned()]
    } else {
        steps
    };
    for step in &steps {
        match step.as_str() {
            "sysroot" => sysroot(&c)?,
            "source" => do_source(&c)?,
            "build" => build(&c)?,
            "emu" => emu(&c)?,
            "all" => {
                sysroot(&c)?;
                do_source(&c)?;
                build(&c)?;
                emu(&c)?;
            }
            "-h" | "--help" => {
                println!("Usage: build-fex-host.sh [sysroot|source|build|emu|all]...\nBuild pinned FEX against the Steam Frame SteamOS/Arch sysroot.");
                return Ok(());
            }
            _ => {
                return Err(format!(
                    "unknown step '{step}' (sysroot|source|build|emu|all)"
                ))
            }
        }
    }
    summary(&c)
}

fn main() {
    if let Err(error) = run_all() {
        fail(error);
    }
}
