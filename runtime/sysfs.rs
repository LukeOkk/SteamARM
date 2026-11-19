// Materialize the guest's minimal arm64 CPU sysfs tree under LXRT_ROOT.

use std::env;
use std::ffi::{c_char, c_int, c_void};
use std::fs::{self, DirBuilder, File, OpenOptions};
use std::io::{Read, Write};
use std::mem::size_of;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{DirBuilderExt, OpenOptionsExt};
use std::path::{Path, PathBuf};

#[link(name = "System")]
unsafe extern "C" {
    fn sysctlbyname(
        name: *const c_char,
        old_value: *mut c_void,
        old_length: *mut usize,
        new_value: *mut c_void,
        new_length: usize,
    ) -> c_int;
    fn lxrt_synthetic_sysreg(instruction: u32) -> u64;
}

fn mkdirs(path: &Path) {
    let mut builder = DirBuilder::new();
    builder.recursive(true).mode(0o755);
    let _ = builder.create(path);
}

fn put(path: &Path, text: &str) {
    let mut temporary = path.as_os_str().to_os_string();
    temporary.push(format!(".{}", std::process::id()));
    let temporary = PathBuf::from(temporary);
    let mut options = OpenOptions::new();
    options.write(true).create(true).truncate(true).mode(0o644);
    let Ok(mut file) = options.open(&temporary) else {
        return;
    };
    let written = file.write(text.as_bytes());
    drop(file);
    if matches!(written, Ok(length) if length == text.len()) {
        let _ = fs::rename(temporary, path);
    } else {
        let _ = fs::remove_file(temporary);
    }
}

fn logical_cpu_count() -> c_int {
    let mut count = 0;
    let mut length = size_of::<c_int>();
    let status = unsafe {
        sysctlbyname(
            b"hw.logicalcpu\0".as_ptr().cast(),
            (&mut count as *mut c_int).cast(),
            &mut length,
            std::ptr::null_mut(),
            0,
        )
    };
    if status != 0 || count < 1 {
        1
    } else {
        count
    }
}

fn online_text(count: c_int) -> String {
    if count > 1 {
        format!("0-{}\n", count - 1)
    } else {
        "0\n".to_owned()
    }
}

fn existing_online(path: &Path) -> Option<String> {
    let mut file = File::open(path).ok()?;
    let mut bytes = [0u8; 31];
    let read = file.read(&mut bytes).unwrap_or(0);
    let bytes = &bytes[..read];
    let bytes = &bytes[..bytes
        .iter()
        .position(|byte| *byte == 0)
        .unwrap_or(bytes.len())];
    Some(String::from_utf8_lossy(bytes).into_owned())
}

#[no_mangle]
pub extern "C" fn lxrt_sysfs_init() {
    let Some(root) = env::var_os("LXRT_ROOT") else {
        return;
    };
    if root.as_os_str().as_bytes().is_empty() {
        return;
    }

    let count = logical_cpu_count();
    let want = online_text(count);
    let directory = PathBuf::from(root).join("sys/devices/system/cpu");
    let online = directory.join("online");
    if existing_online(&online).as_deref() == Some(want.as_str())
        && directory.join("cpu0/regs/identification/midr_el1").exists()
    {
        return;
    }

    mkdirs(&directory);
    let midr_value = unsafe { lxrt_synthetic_sysreg(0xD538_0000) };
    let midr = format!("0x{midr_value:016x}\n");
    for cpu in 0..count {
        let identification = directory
            .join(format!("cpu{cpu}"))
            .join("regs/identification");
        mkdirs(&identification);
        put(&identification.join("midr_el1"), &midr);
    }

    // Stop at the first absent directory, matching the original bounded sweep.
    for cpu in count..1024 {
        let stale = directory.join(format!("cpu{cpu}"));
        if let Err(error) = fs::remove_dir(stale) {
            if error.kind() == std::io::ErrorKind::NotFound {
                break;
            }
        }
    }

    put(&directory.join("present"), &want);
    put(&directory.join("possible"), &want);
    put(&online, &want); // written last: it marks the tree complete
}
