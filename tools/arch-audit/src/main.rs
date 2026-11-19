//! Inspect the architectures of SteamARM's installed files and active guests.

use std::env;
use std::fs;
use std::io;
use std::path::{Path, PathBuf};
use std::process::Command;

const MACHINES: &[(u16, &str)] = &[
    (0x014c, "x86"),
    (0x8664, "x64"),
    (0xaa64, "ARM64"),
    (0xa641, "ARM64EC"),
    (0xa64e, "ARM64X"),
];
const ELF_MACHINES: &[(u16, &str)] = &[(0x03, "i386"), (0x3e, "x86-64"), (0xb7, "aarch64")];

fn invalid_data() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "malformed executable header")
}

fn bytes(data: &[u8], offset: usize, size: usize) -> io::Result<&[u8]> {
    let end = offset.checked_add(size).ok_or_else(invalid_data)?;
    data.get(offset..end).ok_or_else(invalid_data)
}

fn u16le(data: &[u8], offset: usize) -> io::Result<u16> {
    Ok(u16::from_le_bytes(
        bytes(data, offset, 2)?.try_into().unwrap(),
    ))
}

fn u32le(data: &[u8], offset: usize) -> io::Result<u32> {
    Ok(u32::from_le_bytes(
        bytes(data, offset, 4)?.try_into().unwrap(),
    ))
}

fn u64le(data: &[u8], offset: usize) -> io::Result<u64> {
    Ok(u64::from_le_bytes(
        bytes(data, offset, 8)?.try_into().unwrap(),
    ))
}

fn mapped_file_offset(sections: &[(u32, u32, u32, u32)], rva: u32) -> Option<usize> {
    sections
        .iter()
        .find_map(|&(va, virtual_size, raw, raw_size)| {
            let span = u64::from(virtual_size.max(raw_size));
            let rva64 = u64::from(rva);
            let start = u64::from(va);
            (start <= rva64 && rva64 < start.saturating_add(span))
                .then(|| usize::try_from(u64::from(raw) + (rva64 - start)).ok())
                .flatten()
        })
}

fn machine_name(machine: u16, table: &[(u16, &str)]) -> String {
    table
        .iter()
        .find_map(|&(id, name)| (id == machine).then(|| name.to_owned()))
        .unwrap_or_else(|| format!("machine {machine:#x}"))
}

fn pe(data: &[u8]) -> io::Result<Option<String>> {
    if !data.starts_with(b"MZ") {
        return Ok(None);
    }
    let pe_offset = u32le(data, 0x3c)? as usize;
    if bytes(data, pe_offset, 4)? != b"PE\0\0" {
        return Ok(None);
    }

    let machine = u16le(data, pe_offset + 4)?;
    let section_count = u16le(data, pe_offset + 6)? as usize;
    let optional = pe_offset + 24;
    let is_pe32_plus = u16le(data, optional)? == 0x20b;
    let (directory_count_offset, directories_offset) = if is_pe32_plus {
        (optional + 108, optional + 112)
    } else {
        (optional + 92, optional + 96)
    };
    let directory_count = u32le(data, directory_count_offset)?;
    let section_table = optional + u16le(data, pe_offset + 20)? as usize;
    let mut sections = Vec::with_capacity(section_count);
    for index in 0..section_count {
        let offset = section_table + 40 * index;
        sections.push((
            u32le(data, offset + 12)?,
            u32le(data, offset + 8)?,
            u32le(data, offset + 20)?,
            u32le(data, offset + 16)?,
        ));
    }

    let mut kind = machine_name(machine, MACHINES);
    let mut chpe = 0;
    let mut volatile_metadata = 0;
    if directory_count > 10 && is_pe32_plus {
        let load_config_rva = u32le(data, directories_offset + 8 * 10)?;
        let load_config_size = u32le(data, directories_offset + 8 * 10 + 4)?;
        if let Some(offset) =
            mapped_file_offset(&sections, load_config_rva).filter(|_| load_config_size != 0)
        {
            let structure_size = u32le(data, offset)?;
            if structure_size as usize >= 0xc8 + 8 {
                chpe = u64le(data, offset + 0xc8)?;
            }
            if structure_size as usize >= 0x108 + 8 {
                volatile_metadata = u64le(data, offset + 0x108)?;
            }
        }
    }
    if chpe != 0 {
        kind = match machine {
            0xaa64 => "ARM64X".to_owned(),
            0x8664 => "ARM64EC".to_owned(),
            _ => format!("{kind}+CHPE"),
        };
    }
    let notes = if matches!(kind.as_str(), "x64" | "x86") {
        if volatile_metadata != 0 {
            " (volatile metadata)"
        } else {
            " (no volatile metadata)"
        }
    } else {
        ""
    };
    Ok(Some(format!("PE {kind}{notes}")))
}

fn elf(data: &[u8]) -> io::Result<Option<String>> {
    if !data.starts_with(b"\x7fELF") {
        return Ok(None);
    }
    let machine = u16le(data, 18)?;
    Ok(Some(format!("ELF {}", machine_name(machine, ELF_MACHINES))))
}

fn macho(path: &Path, data: &[u8]) -> Option<String> {
    let magic = data.get(..4)?;
    if magic == b"\xcf\xfa\xed\xfe" {
        let cpu = u32::from_le_bytes(data.get(4..8)?.try_into().ok()?);
        let name = match cpu {
            0x0100_000c => "arm64".to_owned(),
            0x0100_0007 => "x86_64".to_owned(),
            _ => format!("cpu {cpu:#x}"),
        };
        return Some(format!("Mach-O {name}"));
    }
    if magic == b"\xca\xfe\xba\xbe" || magic == b"\xca\xfe\xba\xbf" {
        let output = Command::new("lipo").arg("-archs").arg(path).output().ok();
        let arches = output
            .map(|output| String::from_utf8_lossy(&output.stdout).trim().to_owned())
            .unwrap_or_default();
        return Some(format!("Mach-O universal ({arches})"));
    }
    None
}

fn classify(path: &Path) -> String {
    let data = match fs::read(path) {
        Ok(data) => data,
        Err(error) => return format!("unreadable ({})", error_name(&error)),
    };
    for result in [pe(&data), elf(&data)] {
        match result {
            Ok(Some(description)) => return description,
            Ok(None) => {}
            Err(error) => return format!("unreadable ({})", error_name(&error)),
        }
    }
    macho(path, &data).unwrap_or_else(|| "not a program".to_owned())
}

fn error_name(error: &io::Error) -> &'static str {
    if error.kind() == io::ErrorKind::InvalidData {
        "error"
    } else {
        "OSError"
    }
}

fn expand_home(path: &Path) -> PathBuf {
    let text = path.to_string_lossy();
    if text == "~" || text.starts_with("~/") {
        if let Some(home) = env::var_os("HOME") {
            return PathBuf::from(home).join(text.strip_prefix("~/").unwrap_or(""));
        }
    }
    path.to_path_buf()
}

fn resolve_dir(variable: &str, name: &str) -> PathBuf {
    if let Some(value) = env::var_os(variable).filter(|value| !value.is_empty()) {
        return expand_home(Path::new(&value));
    }
    let home = env::var_os("HOME").map(PathBuf::from).unwrap_or_default();
    let preferred = home.join("Developer").join(name);
    let legacy = home.join(name);
    if preferred.is_dir() || !legacy.is_dir() {
        preferred
    } else {
        legacy
    }
}

fn find_named_file(root: &Path, name: &str) -> Option<PathBuf> {
    let entries = fs::read_dir(root).ok()?;
    for entry in entries.flatten() {
        let path = entry.path();
        if path.file_name().is_some_and(|file| file == name) && path.is_file() {
            return Some(path);
        }
        if entry.file_type().map(|kind| kind.is_dir()).unwrap_or(false) {
            if let Some(found) = find_named_file(&path, name) {
                return Some(found);
            }
        }
    }
    None
}

fn proton_arm64(frame: &Path, x86: &Path) -> Option<PathBuf> {
    for directory in [frame, x86] {
        for name in ["Proton Experimental (ARM64)", "Proton 11.0 (ARM64)"] {
            let candidate = directory.join(name);
            if candidate
                .join("files/lib/wine/aarch64-windows/ntdll.dll")
                .exists()
            {
                return Some(candidate);
            }
        }
    }
    None
}

fn stack() -> Vec<(String, PathBuf)> {
    let state = resolve_dir("STEAMARM_STATE", "SteamARM-roots");
    let build = resolve_dir("STEAMARM_BUILD", "SteamARM-build");
    let home = env::var_os("HOME").map(PathBuf::from).unwrap_or_default();
    let x86 = state.join("steamroot/tmp/fexhome/.local/share/Steam/steamapps/common");
    let frame = state.join("arm64root/tmp/armhome/.local/share/Steam/steamapps/common");
    let mut items = vec![
        (
            "SteamARM.app".to_owned(),
            PathBuf::from("/Applications/SteamARM.app/Contents/MacOS/SteamARM"),
        ),
        (
            "runtime lxrun".to_owned(),
            home.join("Library/Application Support/SteamARM/src/build/lxrun"),
        ),
        (
            "X server".to_owned(),
            build.join("xquartz/SteamARM-X11.app/Contents/MacOS/X11.bin"),
        ),
        (
            "KosmicKrisp".to_owned(),
            build.join("mesa-kk/out/libvulkan_kosmickrisp.dylib"),
        ),
        (
            "MoltenVK".to_owned(),
            find_named_file(&build.join("moltenvk"), "libMoltenVK.dylib").unwrap_or_default(),
        ),
        (
            "FEX (host, translates x86 guests)".to_owned(),
            state.join("steamroot/usr/lib/lxrt-emu/FEX"),
        ),
        (
            "Vulkan shim (in the x86 root)".to_owned(),
            state.join("steamroot/usr/lib/lxrt-emu/libvulkan.so.1"),
        ),
        (
            "Steam client (SteamARM)".to_owned(),
            state.join("arm64root/tmp/armhome/.local/share/Steam/steamrtarm64/steam"),
        ),
        (
            "x86 Proton: wineserver".to_owned(),
            x86.join("Proton - Experimental/files/bin/wineserver"),
        ),
        (
            "x86 Proton: wine (unix side)".to_owned(),
            x86.join("Proton - Experimental/files/bin/wine"),
        ),
        (
            "x86 Proton: ntdll.so (unix)".to_owned(),
            x86.join("Proton - Experimental/files/lib/wine/x86_64-unix/ntdll.so"),
        ),
        (
            "x86 Proton: ntdll.dll".to_owned(),
            x86.join("Proton - Experimental/files/lib/wine/x86_64-windows/ntdll.dll"),
        ),
        (
            "x86 Proton: kernel32.dll".to_owned(),
            x86.join("Proton - Experimental/files/lib/wine/x86_64-windows/kernel32.dll"),
        ),
        (
            "x86 Proton: win32u.dll".to_owned(),
            x86.join("Proton - Experimental/files/lib/wine/x86_64-windows/win32u.dll"),
        ),
        (
            "x86 Proton: DXVK d3d11.dll".to_owned(),
            x86.join("Proton - Experimental/files/lib/wine/dxvk/x86_64-windows/d3d11.dll"),
        ),
        (
            "x86 Proton: vkd3d-proton d3d12.dll".to_owned(),
            x86.join("Proton - Experimental/files/lib/wine/vkd3d-proton/x86_64-windows/d3d12.dll"),
        ),
    ];
    if let Some(proton) = proton_arm64(&frame, &x86) {
        let wine = proton.join("files/lib/wine");
        items.extend([
            (
                "ARM64 Proton (Valve, not usable on macOS yet): wineserver".to_owned(),
                proton.join("files/bin-arm64/wineserver"),
            ),
            (
                "ARM64 Proton: wine (loader)".to_owned(),
                proton.join("files/bin-arm64/wine"),
            ),
            (
                "ARM64 Proton: ntdll.so (unix)".to_owned(),
                wine.join("aarch64-unix/ntdll.so"),
            ),
            (
                "ARM64 Proton: ntdll.dll".to_owned(),
                wine.join("aarch64-windows/ntdll.dll"),
            ),
            (
                "ARM64 Proton: kernel32.dll".to_owned(),
                wine.join("aarch64-windows/kernel32.dll"),
            ),
            (
                "ARM64 Proton: win32u.dll".to_owned(),
                wine.join("aarch64-windows/win32u.dll"),
            ),
            (
                "ARM64 Proton: FEX ARM64EC (x64 code in-process)".to_owned(),
                wine.join("aarch64-windows/libarm64ecfex.dll"),
            ),
            (
                "ARM64 Proton: FEX WoW64 (x86 code)".to_owned(),
                wine.join("aarch64-windows/libwow64fex.dll"),
            ),
            (
                "ARM64 Proton: xtajit64.dll".to_owned(),
                wine.join("aarch64-windows/xtajit64.dll"),
            ),
            (
                "ARM64 Proton: DXVK d3d11.dll".to_owned(),
                wine.join("dxvk/aarch64-windows/d3d11.dll"),
            ),
            (
                "ARM64 Proton: DXVK dxgi.dll".to_owned(),
                wine.join("dxvk/aarch64-windows/dxgi.dll"),
            ),
            (
                "ARM64 Proton: vkd3d-proton d3d12.dll".to_owned(),
                wine.join("vkd3d-proton/aarch64-windows/d3d12.dll"),
            ),
            (
                "ARM64 Proton: x64 ntdll.dll (for x64 programs)".to_owned(),
                wine.join("x86_64-windows/ntdll.dll"),
            ),
        ]);
    }
    let game = frame.join("Schedule I");
    if game.is_dir() {
        items.extend([
            (
                "Schedule I: Schedule I.exe".to_owned(),
                game.join("Schedule I.exe"),
            ),
            (
                "Schedule I: UnityPlayer.dll".to_owned(),
                game.join("UnityPlayer.dll"),
            ),
            (
                "Schedule I: GameAssembly.dll".to_owned(),
                game.join("GameAssembly.dll"),
            ),
        ]);
    }
    items
}

#[cfg(target_os = "macos")]
fn translated(pid: i32) -> Option<bool> {
    use std::ffi::c_void;
    use std::ptr;

    unsafe extern "C" {
        fn sysctl(
            name: *mut i32,
            name_length: u32,
            old_value: *mut c_void,
            old_length: *mut usize,
            new_value: *mut c_void,
            new_length: usize,
        ) -> i32;
    }

    let mut mib = [1, 14, 1, pid]; // CTL_KERN, KERN_PROC, KERN_PROC_PID
    let mut size = 0usize;
    let first = unsafe {
        sysctl(
            mib.as_mut_ptr(),
            4,
            ptr::null_mut(),
            &mut size,
            ptr::null_mut(),
            0,
        )
    };
    if first != 0 || size < 40 {
        return None;
    }
    let mut buffer = vec![0u8; size];
    let second = unsafe {
        sysctl(
            mib.as_mut_ptr(),
            4,
            buffer.as_mut_ptr().cast(),
            &mut size,
            ptr::null_mut(),
            0,
        )
    };
    if second != 0 || size < 36 {
        return None;
    }
    let flags = i32::from_ne_bytes(buffer.get(32..36)?.try_into().ok()?);
    Some(flags & 0x0002_0000 != 0) // P_TRANSLATED
}

#[cfg(not(target_os = "macos"))]
fn translated(_pid: i32) -> Option<bool> {
    None
}

fn processes() -> (Vec<String>, usize, usize) {
    let output = Command::new("ps")
        .args(["-axwwo", "pid=,command="])
        .output()
        .ok();
    let stdout = output
        .map(|output| String::from_utf8_lossy(&output.stdout).into_owned())
        .unwrap_or_default();
    let mut rosetta = Vec::new();
    let (mut native_aarch64, mut x86_through_fex) = (0usize, 0usize);
    for line in stdout.lines() {
        let mut fields = line.trim().splitn(2, char::is_whitespace);
        let (Some(pid), Some(command)) = (fields.next(), fields.next()) else {
            continue;
        };
        let Ok(pid) = pid.parse::<i32>() else {
            continue;
        };
        let command = command.trim_start();
        if command.starts_with("arch -x86_64") || translated(pid) == Some(true) {
            rosetta.push(command.chars().take(80).collect());
        }
        let mut words = command.split_whitespace();
        if let (Some(program), Some(guest)) = (words.next(), words.next()) {
            if program.ends_with("/lxrun") {
                if guest.contains("/FEX") || guest.ends_with("FEX-gb") {
                    x86_through_fex += 1;
                } else {
                    native_aarch64 += 1;
                }
            }
        }
    }
    (rosetta, native_aarch64, x86_through_fex)
}

fn main() {
    let arguments: Vec<PathBuf> = env::args_os().skip(1).map(PathBuf::from).collect();
    if !arguments.is_empty() {
        for path in arguments {
            println!("{}: {}", path.display(), classify(&path));
        }
        return;
    }
    for (label, path) in stack() {
        let result = if path.as_os_str().is_empty() || !path.exists() {
            "not installed".to_owned()
        } else {
            classify(&path)
        };
        println!("{label:<58} {result}");
    }
    let (rosetta, native_aarch64, x86_through_fex) = processes();
    println!("\nRosetta (translated) processes: {}", rosetta.len());
    for command in rosetta {
        println!("   {command}");
    }
    println!(
        "runtime guests now: native aarch64 {native_aarch64}, x86 through FEX {x86_through_fex}"
    );
}
