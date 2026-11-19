use std::env;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

fn fail(message: impl AsRef<str>) -> ! {
    eprintln!("steamroot-build: error: {}", message.as_ref());
    std::process::exit(1);
}

fn run(program: &str, args: &[&std::ffi::OsStr]) -> io::Result<()> {
    let status = Command::new(program).args(args).status()?;
    if status.success() {
        Ok(())
    } else {
        Err(io::Error::other(format!("{program} exited with {status}")))
    }
}

fn exists(path: &Path) -> bool {
    fs::symlink_metadata(path).is_ok()
}

fn copy_missing_tree(source: &Path, destination: &Path) -> io::Result<()> {
    let metadata = fs::symlink_metadata(source)?;
    if metadata.file_type().is_symlink() {
        if exists(destination) {
            return Ok(());
        }
        if let Some(parent) = destination.parent() {
            fs::create_dir_all(parent)?;
        }
        let target = fs::read_link(source)?;
        #[cfg(unix)]
        std::os::unix::fs::symlink(target, destination)?;
    } else if metadata.is_dir() {
        if exists(destination) && !fs::symlink_metadata(destination)?.file_type().is_dir() {
            return Ok(());
        }
        fs::create_dir_all(destination)?;
        for entry in fs::read_dir(source)? {
            let entry = entry?;
            copy_missing_tree(&entry.path(), &destination.join(entry.file_name()))?;
        }
    } else if metadata.is_file() {
        if exists(destination) {
            return Ok(());
        }
        if let Some(parent) = destination.parent() {
            fs::create_dir_all(parent)?;
        }
        fs::copy(source, destination)?;
    }
    Ok(())
}

fn copy_file(source: &Path, destination: &Path, overwrite: bool) -> io::Result<()> {
    if !source.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("missing {}", source.display()),
        ));
    }
    if exists(destination) && !overwrite {
        return Ok(());
    }
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent)?;
    }
    fs::copy(source, destination)?;
    Ok(())
}

fn command_text(program: &str, arg: Option<&str>) -> io::Result<String> {
    let mut command = Command::new(program);
    if let Some(arg) = arg {
        command.arg(arg);
    }
    let output = command.output()?;
    if !output.status.success() {
        return Err(io::Error::other(format!(
            "{program} {} failed",
            arg.unwrap_or_default()
        )));
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

fn build(
    x86: &Path,
    frame: &Path,
    output: &Path,
    fex_emu: &Path,
    runtime: &Path,
) -> io::Result<()> {
    if !x86.join("usr").is_dir() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            "x86 root has no usr directory",
        ));
    }
    if !frame.join(".lxrt-frameroot").is_file()
        || !fs::read_to_string(frame.join("etc/os-release"))?
            .lines()
            .any(|line| line == "ID=steamos")
    {
        return Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "ARM base is not a Steam Frame SteamOS root",
        ));
    }
    if !fex_emu.is_file() {
        return Err(io::Error::new(
            io::ErrorKind::NotFound,
            format!("missing {}", fex_emu.display()),
        ));
    }
    if exists(output) {
        return Err(io::Error::new(
            io::ErrorKind::AlreadyExists,
            format!("refusing existing output {}", output.display()),
        ));
    }

    if let Err(error) = run(
        "cp",
        &[
            std::ffi::OsStr::new("-Rc"),
            x86.as_os_str(),
            output.as_os_str(),
        ],
    ) {
        let _ = fs::remove_dir_all(output);
        return Err(error);
    }
    let result: io::Result<()> = (|| {
        let frame_lib64 = frame.join("usr/lib64");
        if frame_lib64.is_dir() {
            for entry in fs::read_dir(&frame_lib64)? {
                let entry = entry?;
                copy_missing_tree(
                    &entry.path(),
                    &output.join("usr/lib64").join(entry.file_name()),
                )?;
            }
        }
        let out_lib = output.join("usr/lib64");
        fs::create_dir_all(&out_lib)?;
        for name in [
            "ld-linux-aarch64.so.1",
            "libstdc++.so.6",
            "libm.so.6",
            "libgcc_s.so.1",
            "libc.so.6",
        ] {
            let mut source = runtime.join("usr/lib64").join(name);
            if !exists(&source) {
                source = runtime.join("usr/lib").join(name);
            }
            copy_file(&source, &out_lib.join(name), false)?;
            if name == "ld-linux-aarch64.so.1" {
                copy_file(&source, &output.join("usr/lib").join(name), false)?;
            }
        }

        for name in [
            "FEX",
            "FEX-gb",
            "FEXServer",
            "FEXGetConfig",
            "Xvnc",
            "xkbcomp",
        ] {
            let source = frame.join("usr/bin").join(name);
            if source.is_file() {
                copy_file(&source, &output.join("usr/bin").join(name), false)?;
            }
        }
        let source_x11 = frame.join("usr/share/X11");
        if source_x11.is_dir() {
            for entry in fs::read_dir(&source_x11)? {
                let entry = entry?;
                copy_missing_tree(
                    &entry.path(),
                    &output.join("usr/share/X11").join(entry.file_name()),
                )?;
            }
        }
        for name in ["resolv.conf", "hosts"] {
            let source = frame.join("etc").join(name);
            if exists(&source) {
                copy_file(&source, &output.join("etc").join(name), false)?;
            }
        }

        let uid = command_text("id", Some("-u"))?;
        let gid = command_text("id", Some("-g"))?;
        fs::write(
            output.join("etc/passwd"),
            format!("root:x:0:0:root:/root:/bin/bash\nsteam:x:{uid}:{gid}:Steam:/tmp/fexhome:/bin/bash\nnobody:x:65534:65534:nobody:/nonexistent:/usr/sbin/nologin\n"),
        )?;
        fs::write(
            output.join("etc/group"),
            format!("root:x:0:\nsteam:x:{gid}:\nnogroup:x:65534:\n"),
        )?;
        let machine_id = output.join("etc/machine-id");
        if !fs::read_to_string(&machine_id).is_ok_and(|contents| !contents.trim().is_empty()) {
            let uuid = command_text("uuidgen", None)?
                .replace('-', "")
                .to_ascii_lowercase();
            fs::write(machine_id, uuid)?;
        }
        fs::create_dir_all(output.join("var/lib/dbus"))?;
        let dbus_id = output.join("var/lib/dbus/machine-id");
        if exists(&dbus_id) {
            fs::remove_file(&dbus_id)?;
        }
        #[cfg(unix)]
        std::os::unix::fs::symlink("/etc/machine-id", dbus_id)?;

        let emulator = output.join("usr/lib/lxrt-emu");
        fs::create_dir_all(&emulator)?;
        copy_file(fex_emu, &emulator.join("FEX"), true)?;
        for name in [
            "ld-linux-aarch64.so.1",
            "libstdc++.so.6",
            "libm.so.6",
            "libgcc_s.so.1",
            "libc.so.6",
        ] {
            let mut source = runtime.join("usr/lib64").join(name);
            if !exists(&source) {
                source = runtime.join("usr/lib").join(name);
            }
            copy_file(&source, &emulator.join(name), true)?;
        }
        for path in ["tmp", "dev/shm", "dev/input"] {
            fs::create_dir_all(output.join(path))?;
        }
        copy_file(
            &frame.join(".lxrt-frameroot"),
            &output.join(".lxrt-steamroot-frame-base"),
            true,
        )?;
        Ok(())
    })();
    if result.is_err() {
        let _ = fs::remove_dir_all(output);
    }
    result?;
    println!("ready: LXRT_ROOT={} FEX_ROOTFS=/", output.display());
    Ok(())
}

fn main() {
    let args = env::args_os()
        .skip(1)
        .map(PathBuf::from)
        .collect::<Vec<_>>();
    if args.len() < 4 || args.len() > 5 {
        fail("usage: steamarm-steamroot-build <x86-rootfs> <steam-frame-root> <out> <FEX-emu> [runtime-root]");
    }
    let runtime = args.get(4).unwrap_or(&args[1]);
    if let Err(error) = build(&args[0], &args[1], &args[2], &args[3], runtime) {
        fail(error.to_string());
    }
}
