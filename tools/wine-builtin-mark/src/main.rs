use std::env;
use std::fs::{self, OpenOptions};
use std::io::{self, Write};
use std::path::Path;

const MARKER_OFFSET: usize = 0x40;
const MARKER_TEXT: &[u8] = b"Wine builtin DLL";
const MARKER_SIZE: usize = 32;
const PREFER_NATIVE: u16 = 0x0010;

fn fail(message: impl AsRef<str>) -> ! {
    eprintln!("{}", message.as_ref());
    std::process::exit(1);
}

fn u16le(data: &[u8], offset: usize) -> io::Result<u16> {
    let end = offset
        .checked_add(2)
        .ok_or_else(|| io::Error::new(io::ErrorKind::InvalidData, "offset overflow"))?;
    let bytes = data
        .get(offset..end)
        .ok_or_else(|| io::Error::new(io::ErrorKind::UnexpectedEof, "truncated PE header"))?;
    Ok(u16::from_le_bytes([bytes[0], bytes[1]]))
}

fn pe_offset(data: &[u8], name: &Path) -> Result<usize, String> {
    if !data.starts_with(b"MZ") {
        return Err(format!("{}: not an MZ file", name.display()));
    }
    let raw = data
        .get(0x3c..0x40)
        .ok_or_else(|| format!("{}: truncated DOS header", name.display()))?;
    let offset = u32::from_le_bytes(raw.try_into().unwrap()) as usize;
    let signature = offset
        .checked_add(4)
        .and_then(|end| data.get(offset..end))
        .ok_or_else(|| format!("{}: no PE header at e_lfanew {offset:#x}", name.display()))?;
    if signature != b"PE\0\0" {
        return Err(format!(
            "{}: no PE header at e_lfanew {offset:#x}",
            name.display()
        ));
    }
    Ok(offset)
}

fn marker() -> [u8; MARKER_SIZE] {
    let mut bytes = [0u8; MARKER_SIZE];
    bytes[..MARKER_TEXT.len()].copy_from_slice(MARKER_TEXT);
    bytes
}

fn process_file(path: &Path, check: bool, prefer_native: bool) -> Result<(), String> {
    let mut data = fs::read(path).map_err(|error| format!("{}: {error}", path.display()))?;
    let offset = pe_offset(&data, path)?;
    let characteristics_offset = offset
        .checked_add(0x5e)
        .ok_or_else(|| format!("{}: PE header offset overflow", path.display()))?;
    let characteristics = u16le(&data, characteristics_offset)
        .map_err(|error| format!("{}: {error}", path.display()))?;

    if check {
        let signature = marker();
        let marked = data
            .get(MARKER_OFFSET..MARKER_OFFSET + 17)
            .is_some_and(|bytes| bytes == &signature[..17]);
        println!(
            "{}: e_lfanew {offset:#x}, builtin marker {}, DllCharacteristics {characteristics:#06x} (prefer-native {})",
            path.display(),
            if marked { "yes" } else { "no" },
            if characteristics & PREFER_NATIVE != 0 { "yes" } else { "no" }
        );
        return Ok(());
    }

    if offset < MARKER_OFFSET + MARKER_SIZE {
        return Err(format!(
            "{}: not enough space ({offset:#x}) for the Wine signature",
            path.display()
        ));
    }
    if data.len() < MARKER_OFFSET + MARKER_SIZE {
        return Err(format!(
            "{}: truncated space for the Wine signature",
            path.display()
        ));
    }
    data[MARKER_OFFSET..MARKER_OFFSET + MARKER_SIZE].copy_from_slice(&marker());
    if prefer_native {
        data[characteristics_offset..characteristics_offset + 2]
            .copy_from_slice(&(characteristics | PREFER_NATIVE).to_le_bytes());
    }
    let mut file = OpenOptions::new()
        .write(true)
        .open(path)
        .map_err(|error| format!("{}: {error}", path.display()))?;
    file.write_all(&data)
        .map_err(|error| format!("{}: {error}", path.display()))?;
    Ok(())
}

fn main() {
    let mut check = false;
    let mut prefer_native = false;
    let mut files = Vec::new();
    for argument in env::args_os().skip(1) {
        match argument.to_string_lossy().as_ref() {
            "--check" => check = true,
            "--prefer-native" => prefer_native = true,
            value if value.starts_with("--") => {}
            _ => files.push(argument),
        }
    }
    if files.is_empty() {
        fail("steamarm-wine-builtin-mark [--prefer-native] FILE... | --check FILE...");
    }
    for file in files {
        let path = Path::new(&file);
        if let Err(error) = process_file(path, check, prefer_native) {
            fail(error);
        }
    }
}
