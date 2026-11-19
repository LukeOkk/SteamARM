use std::env;
use std::error::Error;
use std::fs::{self, File};
use std::io;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};

const FRAME_IMAGE: &str = "steamframe-oobe-repair-20260922.5153644-0.3.0.img";
// Match the dbus-daemon package in the pinned Ubuntu 24.04 FEX rootfs.
const DBUS_X11_DEB: &str = "dbus-x11_1.14.10-4ubuntu4.1_amd64.deb";
const DBUS_X11_SHA256: &str = "6fce42d69d92882fff7bcfd12708d2b7aea110d36f9828d9f7137fbe59521d9b";
const DBUS_X11_URL: &str =
    "https://archive.ubuntu.com/ubuntu/pool/main/d/dbus/dbus-x11_1.14.10-4ubuntu4.1_amd64.deb";
const STEPS: &[&str] = &[
    "check",
    "brew",
    "frameroot",
    "make",
    "fex",
    "thunks",
    "xquartz",
    "kosmickrisp",
    "pulseaudio",
    "roots",
    "steam",
    "test",
    "done",
];

struct Setup {
    repo: PathBuf,
    build: PathBuf,
    state: PathBuf,
    brew: PathBuf,
    frame_installer: PathBuf,
    frame_image_tool: PathBuf,
    steamroot_builder: PathBuf,
}

impl Setup {
    fn new() -> Result<Self, Box<dyn Error>> {
        let repo = env::var_os("STEAMARM_PROJECT_DIR")
            .map(PathBuf::from)
            .or_else(|| env::current_dir().ok())
            .ok_or("cannot determine project directory")?;
        let home = env::var_os("HOME")
            .map(PathBuf::from)
            .ok_or("HOME is not set")?;
        let build = env::var_os("STEAMARM_BUILD")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                first_existing(
                    &[
                        home.join("Developer/SteamARM-build"),
                        home.join("SteamARM-build"),
                    ],
                    home.join("Developer/SteamARM-build"),
                )
            });
        let state = env::var_os("STEAMARM_STATE")
            .map(PathBuf::from)
            .unwrap_or_else(|| {
                first_existing(
                    &[
                        home.join("Developer/SteamARM-roots"),
                        home.join("SteamARM-roots"),
                    ],
                    home.join("Developer/SteamARM-roots"),
                )
            });
        let brew = env::var_os("BREW")
            .map(PathBuf::from)
            .unwrap_or_else(|| PathBuf::from("/opt/homebrew"));
        let frame_installer = env::var_os("STEAMARM_FRAME_CLIENT_INSTALLER")
            .map(PathBuf::from)
            .unwrap_or_else(|| repo.join("build/steamarm-frame-client-install"));
        let frame_image_tool = env::var_os("STEAMARM_FRAME_IMAGE_TOOL")
            .map(PathBuf::from)
            .or_else(|| {
                let sibling = env::current_exe()
                    .ok()?
                    .parent()?
                    .join("steamarm-frame-image");
                is_executable(&sibling).then_some(sibling)
            })
            .unwrap_or_else(|| repo.join("build/steamarm-frame-image"));
        let steamroot_builder = env::var_os("STEAMARM_STEAMROOT_BUILDER")
            .map(PathBuf::from)
            .unwrap_or_else(|| repo.join("build/steamarm-steamroot-build"));
        env::set_var("STEAMARM_BUILD", &build);
        env::set_var("STEAMARM_STATE", &state);
        env::set_var("STEAMARM_FRAME_SYSROOT", state.join("arm64root"));
        Ok(Self {
            repo,
            build,
            state,
            brew,
            frame_installer,
            frame_image_tool,
            steamroot_builder,
        })
    }

    fn log(&self, message: &str) {
        println!("\n\x1b[1m[setup] {message}\x1b[0m");
    }

    fn run(
        &self,
        program: impl AsRef<Path>,
        args: &[impl AsRef<std::ffi::OsStr>],
    ) -> Result<(), String> {
        let status = Command::new(program.as_ref())
            .args(args)
            .current_dir(&self.repo)
            .status()
            .map_err(|e| format!("{}: {e}", program.as_ref().display()))?;
        if status.success() {
            Ok(())
        } else {
            Err(format!(
                "{} exited with {status}",
                program.as_ref().display()
            ))
        }
    }

    fn run_optional(
        &self,
        program: impl AsRef<Path>,
        args: &[impl AsRef<std::ffi::OsStr>],
        warning: &str,
    ) {
        if let Err(error) = self.run(program, args) {
            println!("[setup] WARNING: {warning}: {error}");
        }
    }

    fn script(&self, name: &str, args: &[&str]) -> Result<(), String> {
        let mut command = Command::new(self.repo.join("scripts").join(name));
        command.args(args).current_dir(&self.repo);
        status_result(command.status(), name)
    }

    fn ensure_dbus_x11(&self, root: &Path) -> Result<(), String> {
        let launcher = root.join("usr/bin/dbus-launch");
        if launcher.is_file() {
            return Ok(());
        }

        self.log(&format!("install dbus-x11 into {}", root.display()));
        let cache = self.build.join("rootfs-images/ubuntu-debs");
        fs::create_dir_all(&cache).map_err(display_error)?;
        let deb = cache.join(DBUS_X11_DEB);
        let digest = |path: &Path| -> Result<String, String> {
            let output = Command::new("shasum")
                .args(["-a", "256"])
                .arg(path)
                .output()
                .map_err(display_error)?;
            if !output.status.success() {
                return Err(format!("shasum failed for {}", path.display()));
            }
            String::from_utf8_lossy(&output.stdout)
                .split_whitespace()
                .next()
                .map(str::to_owned)
                .ok_or_else(|| format!("shasum returned no digest for {}", path.display()))
        };
        if !deb.is_file() || digest(&deb)? != DBUS_X11_SHA256 {
            let part = cache.join(format!("{DBUS_X11_DEB}.part"));
            let status = Command::new("curl")
                .args(["-fL", "--retry", "3", "--connect-timeout", "20", "-o"])
                .arg(&part)
                .arg(DBUS_X11_URL)
                .status()
                .map_err(display_error)?;
            if !status.success() {
                let _ = fs::remove_file(&part);
                return Err("cannot download the official Ubuntu dbus-x11 package".into());
            }
            if digest(&part)? != DBUS_X11_SHA256 {
                let _ = fs::remove_file(&part);
                return Err("official Ubuntu dbus-x11 package SHA-256 mismatch".into());
            }
            fs::rename(&part, &deb).map_err(display_error)?;
        }

        let stage = cache.join(format!(".dbus-x11-{}", std::process::id()));
        if stage.exists() {
            fs::remove_dir_all(&stage).map_err(display_error)?;
        }
        fs::create_dir_all(&stage).map_err(display_error)?;
        let result = (|| {
            let status = Command::new("ar")
                .arg("x")
                .arg(&deb)
                .current_dir(&stage)
                .status()
                .map_err(display_error)?;
            if !status.success() {
                return Err("cannot unpack the dbus-x11 Debian archive".into());
            }
            let data_zst = stage.join("data.tar.zst");
            let data_tar = stage.join("data.tar");
            let status = Command::new("zstd")
                .arg("-d")
                .arg("-f")
                .arg(&data_zst)
                .arg("-o")
                .arg(&data_tar)
                .status()
                .map_err(display_error)?;
            if !status.success() {
                return Err("cannot decompress dbus-x11 package data".into());
            }
            let status = Command::new("tar")
                .arg("-xf")
                .arg(&data_tar)
                .arg("-C")
                .arg(root)
                .status()
                .map_err(display_error)?;
            if !status.success() || !launcher.is_file() {
                return Err(format!("dbus-x11 did not install {}", launcher.display()));
            }
            Ok(())
        })();
        let _ = fs::remove_dir_all(&stage);
        result
    }

    fn capture(&self, program: &str, args: &[&str]) -> Result<String, String> {
        let output = Command::new(program)
            .args(args)
            .current_dir(&self.repo)
            .output()
            .map_err(|e| format!("{program}: {e}"))?;
        if !output.status.success() {
            return Err(format!("{program} exited with {}", output.status));
        }
        Ok(String::from_utf8_lossy(&output.stdout).trim().to_owned())
    }

    fn check(&self) -> Result<(), String> {
        if self.capture("uname", &["-s"])? != "Darwin" || self.capture("uname", &["-m"])? != "arm64"
        {
            return Err("needs macOS on Apple Silicon".into());
        }
        if Command::new("xcode-select")
            .arg("-p")
            .stdout(Stdio::null())
            .stderr(Stdio::null())
            .status()
            .map(|s| !s.success())
            .unwrap_or(true)
        {
            return Err("Xcode Command Line Tools missing: xcode-select --install".into());
        }
        if !self.brew.join("bin/brew").is_file() {
            return Err(format!(
                "Homebrew missing at {} (https://brew.sh)",
                self.brew.display()
            ));
        }
        let home = env::var_os("HOME")
            .map(PathBuf::from)
            .ok_or("HOME is not set")?;
        let output = self.capture("df", &["-g", home.to_str().ok_or("invalid HOME path")?])?;
        let free: u64 = output
            .lines()
            .nth(1)
            .and_then(|line| line.split_whitespace().nth(3))
            .and_then(|n| n.parse().ok())
            .unwrap_or(0);
        if free < 25 {
            return Err(format!("less than 25 GB free in {}", home.display()));
        }
        let marker = self.state.join("arm64root/.lxrt-frameroot");
        let bundle = self.state.join("steamframe-root.sparsebundle");
        if !marker.is_file() && !bundle.is_dir() && self.frame_image().is_none() {
            return Err(format!("Steam Frame base image required. Accept Valve's license and download {FRAME_IMAGE}.bz2 from https://store.steampowered.com/steamos/download/?ver=steamframe; keep it in ~/Downloads (or set STEAMARM_FRAME_IMAGE)."));
        }
        self.log(&format!(
            "check: macOS {} arm64, CLT, Homebrew, {free} GB free",
            self.capture("sw_vers", &["-productVersion"])?
        ));
        Ok(())
    }

    fn frame_image(&self) -> Option<PathBuf> {
        if let Some(path) = env::var_os("STEAMARM_FRAME_IMAGE").map(PathBuf::from) {
            return path.is_file().then_some(path);
        }
        let home = env::var_os("HOME").map(PathBuf::from)?;
        [
            home.join("Downloads").join(FRAME_IMAGE),
            home.join("Downloads").join(format!("{FRAME_IMAGE}.bz2")),
        ]
        .into_iter()
        .find(|p| p.is_file())
    }

    fn brew(&self) -> Result<(), String> {
        let mut formulae = vec![
            "llvm",
            "lld",
            "cmake",
            "ninja",
            "zstd",
            "xz",
            "openssl@3",
            "squashfs",
            "xxhash",
            "python",
            "molten-vk",
            "vulkan-headers",
            "sdl2",
            "sdl3",
            "mingw-w64",
            "pulseaudio",
            "meson",
            "glslang",
            "autoconf",
            "bison",
            "flex",
        ];
        if !is_executable(&self.frame_installer)
            || !is_executable(&self.frame_image_tool)
            || !is_executable(&self.steamroot_builder)
            || !is_executable(&self.repo.join("build/steamarm-settings-env"))
            || !is_executable(&self.repo.join("build/steamarm-wine-prefix-options"))
        {
            formulae.push("rust");
        }
        self.log(&format!("brew: {}", formulae.join(" ")));
        self.run(
            self.brew.join("bin/brew"),
            &["install".to_owned()]
                .into_iter()
                .chain(formulae.iter().map(|s| s.to_string()))
                .collect::<Vec<_>>(),
        )
    }

    fn make(&self) -> Result<(), String> {
        if !self.state.join("arm64root/.lxrt-frameroot").is_file() {
            self.frameroot()?;
        }
        self.log("make: runtime, Steam Frame ARM64 tools, Vulkan shim, launcher, controller daemon, FidelityFX DLLs");
        let count = self
            .capture("sysctl", &["-n", "hw.ncpu"])
            .unwrap_or_else(|_| "4".into());
        self.run("make", &[format!("-j{count}"), "all".to_owned()])
    }

    fn roots(&self) -> Result<(), String> {
        fs::create_dir_all(self.state.join("logs")).map_err(display_error)?;
        let arm = self.state.join("arm64root");
        let x86 = self.state.join("x86-rootfs");
        let steam = self.state.join("steamroot");
        if !arm.join(".lxrt-frameroot").is_file() || !arm.join("usr/include/stdio.h").is_file() {
            return Err(format!(
                "Steam Frame Arch base is missing at {}; run the frameroot setup step first",
                arm.display()
            ));
        }
        if !x86.join("usr").is_dir() {
            self.log("x86-64 rootfs (scripts/fetch-x86-rootfs.sh)");
            self.script(
                "fetch-x86-rootfs.sh",
                &[x86.to_str().ok_or("invalid x86 root path")?],
            )?;
        }
        self.ensure_dbus_x11(&x86)?;
        let frame_marker = fs::read(arm.join(".lxrt-frameroot")).map_err(display_error)?;
        let prior_frame_marker = fs::read(steam.join(".lxrt-steamroot-frame-base")).ok();
        let needs_steamroot_rebuild = !steam.join("usr").is_dir()
            || prior_frame_marker.as_deref() != Some(frame_marker.as_slice());
        if needs_steamroot_rebuild {
            self.log("Steam x86 root (scripts/mksteamroot.sh)");
            let had_previous = fs::symlink_metadata(&steam).is_ok();
            if had_previous
                && Command::new("pgrep")
                    .args(["-f", "build/lxrun .*ubuntu12_32/steam "])
                    .status()
                    .map(|s| s.success())
                    .unwrap_or(false)
            {
                return Err("Steam is running; close it with scripts/run-steam.sh --stop before rebuilding its root".into());
            }
            let staged = if had_previous {
                self.state
                    .join(format!("steamroot.new-{}", std::process::id()))
            } else {
                steam.clone()
            };
            let builder = self.prepare_steamroot_builder()?;
            self.run(
                builder,
                &[
                    x86.to_str().ok_or("invalid x86 root path")?,
                    arm.to_str().ok_or("invalid Steam Frame root path")?,
                    staged.to_str().ok_or("invalid staged Steam root path")?,
                    self.build
                        .join("out/FEX-emu")
                        .to_str()
                        .ok_or("invalid FEX path")?,
                    arm.to_str().ok_or("invalid Steam Frame runtime root")?,
                ],
            )?;
            if had_previous {
                // Keep guest state and installed apps when adopting an older
                // or incomplete root; retain the whole old tree as rollback.
                for name in ["tmp", "opt/apps"] {
                    let from = steam.join(name);
                    if from.is_dir() {
                        self.run(
                            "ditto",
                            &[
                                from.to_string_lossy().into_owned(),
                                staged.join(name).to_string_lossy().into_owned(),
                            ],
                        )?;
                    }
                }
                let stamp = SystemTime::now()
                    .duration_since(UNIX_EPOCH)
                    .map_err(display_error)?
                    .as_secs();
                let backup = self.state.join(format!("steamroot.previous-{stamp}"));
                fs::rename(&steam, &backup).map_err(display_error)?;
                if let Err(error) = fs::rename(&staged, &steam) {
                    let _ = fs::rename(&backup, &steam);
                    return Err(format!(
                        "cannot activate new Steam root: {error}; previous root restored"
                    ));
                }
                self.log(&format!(
                    "previous Steam root preserved at {}",
                    backup.display()
                ));
            }
        }
        self.ensure_dbus_x11(&steam)?;
        let guest_home = steam.join("tmp/fexhome");
        let guest_rootfs = guest_home.join(".local/share/fex-emu/RootFS/Ubuntu_24_04");
        if !guest_rootfs.join("usr").is_dir() {
            self.log("FEX rootfs in the guest home");
            fs::create_dir_all(guest_rootfs.parent().ok_or("invalid FEX rootfs path")?)
                .map_err(display_error)?;
            self.run(
                "cp",
                &[
                    "-Rc".to_owned(),
                    x86.to_string_lossy().into_owned(),
                    guest_rootfs.to_string_lossy().into_owned(),
                ],
            )?;
        }
        self.ensure_dbus_x11(&guest_rootfs)?;
        self.script(
            "env-links.sh",
            &[self.state.to_str().ok_or("invalid state path")?],
        )?;
        self.log(&format!("roots ready under {}", self.state.display()));
        Ok(())
    }

    fn frameroot(&self) -> Result<(), String> {
        let bundle = self.state.join("steamframe-root.sparsebundle");
        let volume = Path::new("/Volumes/SteamFrameRoot");
        let frame_root = volume.join("rootfs");
        let image = self.frame_image();
        if !self.state.join("arm64root/.lxrt-frameroot").is_file() && !bundle.is_dir() {
            if image.is_none() {
                return Err(format!("Steam Frame image missing; see https://store.steampowered.com/steamos/download/?ver=steamframe"));
            }
            fs::create_dir_all(&self.state).map_err(display_error)?;
            self.run(
                "hdiutil",
                &[
                    "create".to_owned(),
                    "-size".to_owned(),
                    "40g".to_owned(),
                    "-type".to_owned(),
                    "SPARSEBUNDLE".to_owned(),
                    "-fs".to_owned(),
                    "Case-sensitive APFS".to_owned(),
                    "-volname".to_owned(),
                    "SteamFrameRoot".to_owned(),
                    bundle.to_string_lossy().into_owned(),
                ],
            )?;
        }
        if bundle.is_dir() {
            self.script(
                "image-volume.sh",
                &[
                    "ensure",
                    bundle.to_str().ok_or("invalid bundle path")?,
                    volume.to_str().ok_or("invalid volume path")?,
                ],
            )?;
        }
        if !self.state.join("arm64root/.lxrt-frameroot").is_file() {
            if !frame_root.join("etc/os-release").is_file() {
                let image = image
                    .ok_or("Steam Frame extraction missing and source image is unavailable")?;
                let raw = if image.extension().is_some_and(|x| x == "bz2") {
                    let raw = self.build.join("images").join(FRAME_IMAGE);
                    if !raw.is_file()
                        || image.metadata().and_then(|a| a.modified()).ok()
                            > raw.metadata().and_then(|a| a.modified()).ok()
                    {
                        self.log(&format!(
                            "decompress: {}",
                            image.file_name().unwrap_or_default().to_string_lossy()
                        ));
                        fs::create_dir_all(raw.parent().ok_or("invalid image cache path")?)
                            .map_err(display_error)?;
                        let part = raw.with_extension("img.part");
                        let file = File::create(&part).map_err(display_error)?;
                        let status = Command::new("bzip2")
                            .args(["-dc"])
                            .arg(&image)
                            .stdout(file)
                            .current_dir(&self.repo)
                            .status()
                            .map_err(display_error)?;
                        if !status.success() {
                            let _ = fs::remove_file(&part);
                            return Err("Steam Frame image decompression failed".into());
                        }
                        fs::rename(&part, &raw).map_err(display_error)?;
                    }
                    raw
                } else {
                    image
                };
                self.log(&format!(
                    "extract: Steam Frame image to {} (resumable)",
                    frame_root.display()
                ));
                self.log("extract: Steam Frame Btrfs image with the Rust image reader");
                self.run(
                    &self.frame_image_tool,
                    &[
                        "extract".to_owned(),
                        raw.to_string_lossy().into_owned(),
                        frame_root.to_string_lossy().into_owned(),
                        "--resume".to_owned(),
                    ],
                )?;
            }
            self.log("make: Steam Frame ARM64 base (Rust frame-root builder)");
            self.script(
                "mkframeroot.sh",
                &[frame_root.to_str().ok_or("invalid Frame root path")?],
            )?;
        }
        let frame = self.state.join("arm64root");
        if !frame.join(".lxrt-frameroot").is_file() {
            return Err("Steam Frame ARM64 root was not created".into());
        }
        self.script(
            "env-links.sh",
            &[self.state.to_str().ok_or("invalid state path")?],
        )?;
        self.log("Vulkan in the Steam Frame root (scripts/install-frameroot-vulkan.sh)");
        self.script(
            "install-frameroot-vulkan.sh",
            &[frame.to_str().ok_or("invalid Frame root path")?],
        )?;
        self.log(
            "SteamOS update client in the Steam Frame root (scripts/install-frameroot-atomupd.sh)",
        );
        self.run_optional(
            self.repo.join("scripts/install-frameroot-atomupd.sh"),
            &[frame.to_string_lossy().into_owned()],
            "atomupd-manager left as the image has it",
        );
        let installer = self.prepare_frame_installer()?;
        self.log("Steam ARM64 client from the local Frame image");
        self.run(installer, &[frame.to_string_lossy().into_owned()])?;

        if frame
            .join("tmp/armhome/.local/share/Steam/compatibilitytools.d/steamarm-native-proton/dist")
            .is_dir()
        {
            for (label, script, warning) in [
                (
                    "vkd3d-proton with SteamARM's patches (scripts/build-vkd3d-proton.sh)",
                    "build-vkd3d-proton.sh",
                    "vkd3d-proton was not built; the tool keeps Valve's",
                ),
                (
                    "FEX ARM64EC with SteamARM's patches (scripts/build-fex-arm64ec.sh)",
                    "build-fex-arm64ec.sh",
                    "FEX ARM64EC was not built; the tool keeps Valve's",
                ),
                (
                    "Wine ntdll.dll with SteamARM's patches (scripts/build-wine-ntdll.sh)",
                    "build-wine-ntdll.sh",
                    "ntdll.dll was not built; the tool keeps Valve's",
                ),
            ] {
                self.log(label);
                self.run_optional(
                    self.repo.join("scripts").join(script),
                    &[] as &[&str],
                    warning,
                );
            }
            self.log("native Proton tool (scripts/install-native-proton.sh)");
            self.run_optional(
                self.repo.join("scripts/install-native-proton.sh"),
                &["--root".to_owned(), "arm64root".to_owned()],
                "native Proton tool left as it was (setup goes on)",
            );
        }
        Ok(())
    }

    fn prepare_frame_installer(&self) -> Result<PathBuf, String> {
        if is_executable(&self.frame_installer) {
            return Ok(self.frame_installer.clone());
        }
        if !command_exists("cargo") {
            return Err("Rust is missing (brew install rust)".into());
        }
        self.log("Rust Steam Frame client installer");
        self.run(
            "cargo",
            &[
                "build".to_owned(),
                "--offline".to_owned(),
                "--locked".to_owned(),
                "--release".to_owned(),
                "--manifest-path".to_owned(),
                self.repo
                    .join("tools/frame-client-install/Cargo.toml")
                    .to_string_lossy()
                    .into_owned(),
                "--target-dir".to_owned(),
                self.repo
                    .join("build/rust-frame-client-install")
                    .to_string_lossy()
                    .into_owned(),
            ],
        )?;
        let built = self
            .repo
            .join("build/rust-frame-client-install/release/steamarm-frame-client-install");
        let installed = self.repo.join("build/steamarm-frame-client-install");
        fs::copy(&built, &installed).map_err(display_error)?;
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&installed, fs::Permissions::from_mode(0o755))
            .map_err(display_error)?;
        Ok(installed)
    }

    fn prepare_steamroot_builder(&self) -> Result<PathBuf, String> {
        if is_executable(&self.steamroot_builder) {
            return Ok(self.steamroot_builder.clone());
        }
        if !command_exists("cargo") {
            return Err("Rust is missing (brew install rust)".into());
        }
        self.log("Rust Steam root builder");
        self.run(
            "cargo",
            &[
                "build".to_owned(),
                "--offline".to_owned(),
                "--locked".to_owned(),
                "--release".to_owned(),
                "--manifest-path".to_owned(),
                self.repo
                    .join("tools/steamroot-build/Cargo.toml")
                    .to_string_lossy()
                    .into_owned(),
                "--target-dir".to_owned(),
                self.repo
                    .join("build/rust-steamroot-build")
                    .to_string_lossy()
                    .into_owned(),
            ],
        )?;
        let built = self
            .repo
            .join("build/rust-steamroot-build/release/steamarm-steamroot-build");
        if let Some(parent) = self.steamroot_builder.parent() {
            fs::create_dir_all(parent).map_err(display_error)?;
        }
        fs::copy(&built, &self.steamroot_builder).map_err(display_error)?;
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&self.steamroot_builder, fs::Permissions::from_mode(0o755))
            .map_err(display_error)?;
        Ok(self.steamroot_builder.clone())
    }

    fn run_step(&self, step: &str) -> Result<(), String> {
        match step {
            "check" => self.check(),
            "brew" => self.brew(),
            "make" => self.make(),
            "fex" => {
                if !self.state.join("arm64root/.lxrt-frameroot").is_file() {
                    self.frameroot()?;
                }
                self.log("FEX (Rust host builder; Steam Frame/Arch sysroot)");
                self.script("build-fex-host.sh", &["all"])
            }
            "thunks" => {
                self.log("Rust FEX Vulkan thunk builder, 64- and 32-bit guests");
                self.script("build-fex-thunks.sh", &["all"])
            }
            "xquartz" => self.xquartz(),
            "kosmickrisp" => {
                self.log("KosmicKrisp with SteamARM's patches (scripts/build-kosmickrisp.sh)");
                self.run_optional(
                    self.repo.join("scripts/build-kosmickrisp.sh"),
                    &[] as &[&str],
                    "KosmicKrisp was not built; Homebrew's driver stays in use",
                );
                Ok(())
            }
            "pulseaudio" => {
                self.log("PulseAudio with SteamARM's patch (scripts/build-pulseaudio.sh)");
                self.run_optional(
                    self.repo.join("scripts/build-pulseaudio.sh"),
                    &[] as &[&str],
                    "PulseAudio was not built; Homebrew's stays in use",
                );
                Ok(())
            }
            "roots" => self.roots(),
            "frameroot" => self.frameroot(),
            "steam" => self.steam(),
            "test" => self.smoke_tests(),
            "done" => self.done(),
            _ => Err(format!("unknown step '{step}' (--list)")),
        }
    }

    fn xquartz(&self) -> Result<(), String> {
        self.log("native X server + window manager (scripts/build-xquartz.sh)");
        self.script("build-xquartz.sh", &[])?;
        let running = self
            .capture(
                "pgrep",
                &["-f", "SteamARM-X11.app/Contents/MacOS/X11.bin :2"],
            )
            .is_ok();
        let guest = self.capture("pgrep", &["-f", "build/lxrun "]).is_ok();
        if running && !guest {
            let _ = self.script("run-x11-native.sh", &["restart", ":2"]);
        }
        Ok(())
    }

    fn steam(&self) -> Result<(), String> {
        self.log("Steam client and graphics in the Steam Frame Arch root");
        self.script("install-steam.sh", &[])?;
        self.script("install-fex-host.sh", &[])?;
        let status = Command::new(self.repo.join("scripts/build-fex-thunks.sh"))
            .arg("install")
            .env("LXRT_ROOT", "/tmp/lxrt-root")
            .current_dir(&self.repo)
            .status()
            .map_err(display_error)?;
        if !status.success() {
            return Err(format!("build-fex-thunks.sh install exited with {status}"));
        }
        self.script("install-steamroot-gfx.sh", &[])?;
        self.run(
            self.repo.join("build/steamarm-ffx-metalfx-install"),
            &[] as &[&str],
        )
    }

    fn smoke_tests(&self) -> Result<(), String> {
        self.log("smoke tests");
        for (command, warning) in [
            (
                "tests/elf/run.sh | tail -1",
                "tests/elf: a test failed (setup goes on; tests/elf/run.sh shows which)",
            ),
            (
                "tests/elf/run_vk_device.sh | tail -1",
                "Vulkan/OpenGL probes: a probe failed (setup goes on)",
            ),
        ] {
            if Command::new("/bin/bash")
                .args(["-o", "pipefail", "-c", command])
                .current_dir(&self.repo)
                .status()
                .map(|s| !s.success())
                .unwrap_or(true)
            {
                println!("[setup] {warning}");
            }
        }
        let proton = Path::new("/tmp/lxrt-steamroot/tmp/fexhome/.local/share/Steam/steamapps/common/Proton - Experimental/files/bin/wine");
        if proton.is_file() {
            if Command::new("/bin/bash")
                .args(["-o", "pipefail", "-c", "tests/win/run.sh | tail -9"])
                .current_dir(&self.repo)
                .status()
                .map(|s| !s.success())
                .unwrap_or(true)
            {
                println!("[setup] Windows probes: a probe failed (setup goes on)");
            }
        } else {
            self.log("Windows probes skipped: no Proton yet (start Steam, log in, install Proton Experimental; then scripts/setup.sh test)");
        }
        Ok(())
    }

    fn done(&self) -> Result<(), String> {
        let version = self.repo.join(".bundle-version");
        if version.is_file() {
            fs::copy(version, self.repo.join("build/.setup-version")).map_err(display_error)?;
        }
        self.log("done. Start SteamARM:");
        println!(
            "  open {}        (launcher: Steam and other apps)",
            self.repo.join("build/SteamARM.app").display()
        );
        println!("  scripts/run-steam.sh                  (Steam directly)");
        println!("The first Steam start updates the client (several minutes).");
        Ok(())
    }
}

fn first_existing(candidates: &[PathBuf], fallback: PathBuf) -> PathBuf {
    candidates
        .iter()
        .find(|p| p.is_dir())
        .cloned()
        .unwrap_or(fallback)
}

fn is_executable(path: &Path) -> bool {
    use std::os::unix::fs::PermissionsExt;
    fs::metadata(path)
        .map(|m| m.is_file() && m.permissions().mode() & 0o111 != 0)
        .unwrap_or(false)
}

fn command_exists(program: &str) -> bool {
    Command::new("/usr/bin/which")
        .arg(program)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .map(|s| s.success())
        .unwrap_or(false)
}

fn status_result(status: io::Result<std::process::ExitStatus>, name: &str) -> Result<(), String> {
    let status = status.map_err(|e| format!("{name}: {e}"))?;
    if status.success() {
        Ok(())
    } else {
        Err(format!("{name} exited with {status}"))
    }
}

fn display_error(error: impl std::fmt::Display) -> String {
    error.to_string()
}

fn main() {
    let args: Vec<String> = env::args().skip(1).collect();
    if args.as_slice() == ["--list"] {
        for step in STEPS {
            println!("{step}");
        }
        return;
    }
    let selected = if args.is_empty() {
        STEPS.to_vec()
    } else {
        args.iter().map(String::as_str).collect()
    };
    let setup = match Setup::new() {
        Ok(setup) => setup,
        Err(error) => {
            eprintln!("[setup] error: {error}");
            std::process::exit(1);
        }
    };
    for step in selected {
        if !STEPS.contains(&step) {
            eprintln!("[setup] error: unknown step '{step}' (--list)");
            std::process::exit(1);
        }
        if let Err(error) = setup.run_step(step) {
            eprintln!("[setup] error: {error}");
            std::process::exit(1);
        }
    }
}
