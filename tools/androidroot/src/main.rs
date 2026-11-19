use std::collections::BTreeMap;
use std::env;
use std::fs::{self, File};
use std::io::{Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::thread;
use std::time::{Duration, Instant};

const SPARSE_MAGIC: u32 = 0xed26_ff3a;
const PT_LOAD: u32 = 1;
const PT_INTERP: u32 = 3;
const PT_TLS: u32 = 7;
const USAGE: &str = "Helpers for Android image roots.\n\n  steamarm-androidroot-tools image-kind <img>\n  steamarm-androidroot-tools unsparse <in> <out>\n  steamarm-androidroot-tools apex-names <root>\n  steamarm-androidroot-tools inventory <root> [--lxrun build/lxrun] [--x18-limit N]";

#[derive(Debug)]
struct ElfInfo {
    class: u8,
    machine: String,
    kind: u16,
    interp: bool,
    tls: bool,
    align: u64,
    exec_load: bool,
}

fn machine_name(number: u16) -> String {
    match number {
        3 => "i386".to_owned(),
        8 => "mips".to_owned(),
        40 => "arm".to_owned(),
        62 => "x86-64".to_owned(),
        183 => "aarch64".to_owned(),
        243 => "riscv".to_owned(),
        other => other.to_string(),
    }
}

fn le_u16(bytes: &[u8]) -> Option<u16> {
    Some(u16::from_le_bytes(bytes.get(..2)?.try_into().ok()?))
}

fn le_u32(bytes: &[u8]) -> Option<u32> {
    Some(u32::from_le_bytes(bytes.get(..4)?.try_into().ok()?))
}

fn le_u64(bytes: &[u8]) -> Option<u64> {
    Some(u64::from_le_bytes(bytes.get(..8)?.try_into().ok()?))
}

fn elf_info(path: &Path) -> Option<ElfInfo> {
    let mut file = File::open(path).ok()?;
    let mut header = [0u8; 64];
    let count = file.read(&mut header).ok()?;
    if count < 52 || &header[..4] != b"\x7fELF" {
        return None;
    }
    let class = header[4];
    if header[5] != 1 {
        return Some(ElfInfo {
            class,
            machine: "big-endian".to_owned(),
            kind: 0,
            interp: false,
            tls: false,
            align: 0,
            exec_load: false,
        });
    }
    let kind = le_u16(&header[16..])?;
    let machine = machine_name(le_u16(&header[18..])?);
    if class != 2 {
        return Some(ElfInfo {
            class: 32,
            machine,
            kind,
            interp: false,
            tls: false,
            align: 0,
            exec_load: false,
        });
    }
    if count < 64 {
        return None;
    }

    let phoff = le_u64(&header[32..])?;
    let phentsize = le_u16(&header[54..])? as u64;
    let phnum = le_u16(&header[56..])? as u64;
    if phnum > 0 && phentsize < 56 {
        return None;
    }
    let mut interp = false;
    let mut tls = false;
    let mut align: Option<u64> = None;
    let mut exec_load = false;
    for index in 0..phnum {
        let offset = phoff.checked_add(index.checked_mul(phentsize)?)?;
        if file.seek(SeekFrom::Start(offset)).is_err() {
            return None;
        }
        let mut program = [0u8; 56];
        if file.read_exact(&mut program).is_err() {
            return None;
        }
        let segment = le_u32(&program)?;
        let flags = le_u32(&program[4..])?;
        if segment == PT_LOAD {
            let segment_align = le_u64(&program[48..])?;
            align = Some(align.map_or(segment_align, |old| old.min(segment_align)));
            exec_load |= flags & 1 != 0;
        } else if segment == PT_INTERP {
            interp = true;
        } else if segment == PT_TLS {
            tls = true;
        }
    }
    Some(ElfInfo {
        class: 64,
        machine,
        kind,
        interp,
        tls,
        align: align.unwrap_or(0),
        exec_load,
    })
}

fn classify_image(path: &Path) -> Result<&'static str, String> {
    let file = File::open(path).map_err(|error| format!("{}: {error}", path.display()))?;
    let mut head = Vec::with_capacity(2048);
    file.take(2048)
        .read_to_end(&mut head)
        .map_err(|error| format!("{}: {error}", path.display()))?;
    if head.len() >= 4 && le_u32(&head).is_some_and(|magic| magic == SPARSE_MAGIC) {
        return Ok("sparse");
    }
    if head.len() >= 1082 && head[1080..1082] == [0x53, 0xef] {
        return Ok("ext4");
    }
    if head.len() >= 1028 && le_u32(&head[1024..]).is_some_and(|magic| magic == 0xe0f5_e1e2) {
        return Ok("erofs");
    }
    Ok("unknown")
}

fn unsparse(source: &Path, destination: &Path) -> Result<(), String> {
    let mut input = File::open(source).map_err(|error| format!("{}: {error}", source.display()))?;
    let mut raw_header = [0u8; 28];
    input
        .read_exact(&mut raw_header)
        .map_err(|error| format!("{}: {error}", source.display()))?;
    let magic = le_u32(&raw_header).unwrap_or(0);
    let major = le_u16(&raw_header[4..]).unwrap_or(0);
    let file_header_size = le_u16(&raw_header[8..]).unwrap_or(0) as u64;
    let chunk_header_size = le_u16(&raw_header[10..]).unwrap_or(0) as u64;
    let block_size = le_u32(&raw_header[12..]).unwrap_or(0) as u64;
    let total_blocks = le_u32(&raw_header[16..]).unwrap_or(0) as u64;
    let total_chunks = le_u32(&raw_header[20..]).unwrap_or(0);
    if magic != SPARSE_MAGIC || major != 1 {
        return Err(format!(
            "{}: not an Android sparse image v1",
            source.display()
        ));
    }
    input
        .seek(SeekFrom::Start(file_header_size))
        .map_err(|error| format!("{}: {error}", source.display()))?;
    let mut output =
        File::create(destination).map_err(|error| format!("{}: {error}", destination.display()))?;
    let mut buffer = vec![0u8; 1 << 22];
    for _ in 0..total_chunks {
        let mut chunk_header = [0u8; 12];
        input
            .read_exact(&mut chunk_header)
            .map_err(|_| format!("{}: truncated chunk header", source.display()))?;
        let chunk_type = le_u16(&chunk_header).unwrap_or(0);
        let chunk_blocks = le_u32(&chunk_header[4..]).unwrap_or(0) as u64;
        let total_size = le_u32(&chunk_header[8..]).unwrap_or(0);
        let extra_header = chunk_header_size
            .checked_sub(12)
            .ok_or_else(|| format!("{}: invalid sparse chunk header size", source.display()))?;
        input
            .seek(SeekFrom::Current(extra_header as i64))
            .map_err(|error| format!("{}: {error}", source.display()))?;
        let bytes = chunk_blocks
            .checked_mul(block_size)
            .ok_or_else(|| format!("{}: sparse chunk is too large", source.display()))?;
        match chunk_type {
            0xcac1 => {
                let mut remaining = bytes;
                while remaining > 0 {
                    let take = remaining.min(buffer.len() as u64) as usize;
                    let count = input
                        .read(&mut buffer[..take])
                        .map_err(|error| format!("{}: {error}", source.display()))?;
                    if count == 0 {
                        return Err(format!("{}: truncated raw chunk", source.display()));
                    }
                    output
                        .write_all(&buffer[..count])
                        .map_err(|error| format!("{}: {error}", destination.display()))?;
                    remaining -= count as u64;
                }
            }
            0xcac2 => {
                let mut word = [0u8; 4];
                input
                    .read_exact(&mut word)
                    .map_err(|_| format!("{}: truncated fill chunk", source.display()))?;
                let mut remaining = (bytes / 4) * 4;
                while remaining > 0 {
                    let count = remaining.min(buffer.len() as u64) as usize & !3;
                    for chunk in buffer[..count].chunks_exact_mut(4) {
                        chunk.copy_from_slice(&word);
                    }
                    output
                        .write_all(&buffer[..count])
                        .map_err(|error| format!("{}: {error}", destination.display()))?;
                    remaining -= count as u64;
                }
            }
            0xcac3 => {
                output
                    .seek(SeekFrom::Current(bytes as i64))
                    .map_err(|error| format!("{}: {error}", destination.display()))?;
            }
            0xcac4 => {
                let mut crc = [0u8; 4];
                let _ = input.read(&mut crc);
            }
            other => {
                return Err(format!(
                    "{}: unknown chunk type 0x{other:x}",
                    source.display()
                ))
            }
        }
        let _ = total_size;
    }
    let total_size = total_blocks
        .checked_mul(block_size)
        .ok_or_else(|| format!("{}: sparse image is too large", source.display()))?;
    output
        .set_len(total_size)
        .map_err(|error| format!("{}: {error}", destination.display()))?;
    Ok(())
}

fn read_varint(bytes: &[u8], cursor: &mut usize) -> Result<u64, String> {
    let mut value = 0u64;
    for shift in (0..70).step_by(7) {
        let byte = *bytes
            .get(*cursor)
            .ok_or_else(|| "truncated protobuf varint".to_owned())?;
        *cursor += 1;
        if shift == 63 && byte > 1 {
            return Err("protobuf varint overflows u64".to_owned());
        }
        value |= ((byte & 0x7f) as u64)
            .checked_shl(shift)
            .ok_or_else(|| "protobuf varint overflows u64".to_owned())?;
        if byte < 0x80 {
            return Ok(value);
        }
    }
    Err("protobuf varint exceeds ten bytes".to_owned())
}

fn apex_manifest(path: &Path) -> Result<(Option<String>, Option<u64>), String> {
    let bytes = fs::read(path).map_err(|error| format!("{}: {error}", path.display()))?;
    let mut cursor = 0;
    let mut name = None;
    let mut version = None;
    while cursor < bytes.len() {
        let key = read_varint(&bytes, &mut cursor)?;
        let field = key >> 3;
        match key & 7 {
            2 => {
                let length = read_varint(&bytes, &mut cursor)? as usize;
                let end = cursor
                    .checked_add(length)
                    .filter(|end| *end <= bytes.len())
                    .ok_or_else(|| format!("{}: truncated protobuf field", path.display()))?;
                if field == 1 {
                    name = Some(
                        std::str::from_utf8(&bytes[cursor..end])
                            .map_err(|error| {
                                format!("{}: invalid APEX name: {error}", path.display())
                            })?
                            .to_owned(),
                    );
                }
                cursor = end;
            }
            0 => {
                let value = read_varint(&bytes, &mut cursor)?;
                if field == 2 {
                    version = Some(value);
                }
            }
            1 => cursor = cursor.saturating_add(8),
            5 => cursor = cursor.saturating_add(4),
            _ => break,
        }
    }
    Ok((name, version))
}

fn apex_names(root: &Path) -> Result<(), String> {
    let base = root.join("system/apex");
    let mut dirs = fs::read_dir(&base)
        .map_err(|error| format!("{}: {error}", base.display()))?
        .map(|entry| entry.map_err(|error| format!("{}: {error}", base.display())))
        .collect::<Result<Vec<_>, _>>()?;
    dirs.sort_by_key(|entry| entry.file_name());
    for entry in dirs {
        let dir = entry.file_name().to_string_lossy().into_owned();
        let manifest = entry.path().join("apex_manifest.pb");
        if manifest.is_file() {
            let (name, version) = apex_manifest(&manifest)?;
            println!(
                "{} {} {}",
                dir,
                name.filter(|value| !value.is_empty())
                    .unwrap_or_else(|| dir.clone()),
                version.map_or_else(|| "?".to_owned(), |value| value.to_string())
            );
        } else if dir.ends_with(".apex") || dir.ends_with(".capex") {
            println!("{dir} ZIP ?");
        }
    }
    Ok(())
}

fn walk_files(base: &Path, output: &mut Vec<PathBuf>) {
    let Ok(entries) = fs::read_dir(base) else {
        return;
    };
    for entry in entries.flatten() {
        let path = entry.path();
        let Ok(file_type) = entry.file_type() else {
            continue;
        };
        if file_type.is_symlink() {
            continue;
        }
        if file_type.is_dir() {
            walk_files(&path, output);
        } else if file_type.is_file() {
            output.push(path);
        }
    }
    output.sort();
}

fn parse_x18(stderr: &str) -> Option<[u64; 5]> {
    let tokens = stderr.split_whitespace().collect::<Vec<_>>();
    for start in 0..tokens.len() {
        if tokens.get(start).copied() != Some("x18") {
            continue;
        }
        if start + 13 > tokens.len()
            || tokens[start + 2] != "found"
            || tokens[start + 3] != "in"
            || tokens[start + 5] != "code"
            || tokens[start + 6] != "windows,"
            || tokens[start + 8] != "rewritten,"
            || tokens[start + 10] != "unsupported,"
            || tokens[start + 12] != "unreachable"
        {
            continue;
        }
        let values = [
            tokens[start + 1].parse().ok()?,
            tokens[start + 4].parse().ok()?,
            tokens[start + 7].parse().ok()?,
            tokens[start + 9].parse().ok()?,
            tokens[start + 11].parse().ok()?,
        ];
        return Some(values);
    }
    None
}

fn x18_count(lxrun: &Path, guest: &Path, root: &Path) -> Option<[u64; 5]> {
    let mut child = Command::new(lxrun)
        .arg("--dry-run")
        .arg(guest)
        .env("LXRT_ROOT", root)
        .env("LXRT_GUEST_PAGE", "4096")
        .stdout(Stdio::null())
        .stderr(Stdio::piped())
        .spawn()
        .ok()?;
    let mut stderr = child.stderr.take()?;
    let reader = thread::spawn(move || {
        let mut bytes = Vec::new();
        stderr.read_to_end(&mut bytes).map(|_| bytes)
    });
    let started = Instant::now();
    loop {
        match child.try_wait() {
            Ok(Some(_)) => break,
            Ok(None) if started.elapsed() < Duration::from_secs(60) => {
                thread::sleep(Duration::from_millis(10));
            }
            Ok(None) | Err(_) => {
                let _ = child.kill();
                let _ = child.wait();
                let _ = reader.join();
                return None;
            }
        }
    }
    let bytes = reader.join().ok()?.ok()?;
    parse_x18(&String::from_utf8_lossy(&bytes))
}

fn count_rows<K: Ord + Clone>(counts: &BTreeMap<K, usize>) -> Vec<(K, usize)> {
    let mut rows = counts
        .iter()
        .map(|(key, count)| (key.clone(), *count))
        .collect::<Vec<_>>();
    rows.sort_by(|a, b| b.1.cmp(&a.1));
    rows
}

fn inventory(root_arg: &Path, lxrun: Option<&Path>, x18_limit: usize) {
    let root = fs::canonicalize(root_arg).unwrap_or_else(|_| {
        if root_arg.is_absolute() {
            root_arg.to_path_buf()
        } else {
            env::current_dir()
                .unwrap_or_else(|_| PathBuf::from("."))
                .join(root_arg)
        }
    });
    let mut files = Vec::new();
    for top in ["system", "vendor", "apex"] {
        walk_files(&root.join(top), &mut files);
    }

    let mut by_machine: BTreeMap<(u8, String), usize> = BTreeMap::new();
    let mut aligns: BTreeMap<u64, usize> = BTreeMap::new();
    let mut kinds: BTreeMap<String, usize> = BTreeMap::new();
    let mut tls = 0usize;
    let mut elfs64 = Vec::new();
    for path in &files {
        let Some(elf) = elf_info(path) else {
            continue;
        };
        *by_machine
            .entry((elf.class, elf.machine.clone()))
            .or_default() += 1;
        if elf.class != 64 || elf.machine != "aarch64" {
            continue;
        }
        let relative = path
            .strip_prefix(&root)
            .unwrap_or(path)
            .to_string_lossy()
            .into_owned();
        let mut kind = match elf.kind {
            2 => "EXEC".to_owned(),
            3 => "DYN".to_owned(),
            other => format!("type{other}"),
        };
        if elf.kind == 3 && elf.interp {
            kind = "DYN+INTERP (PIE executable)".to_owned();
        }
        *kinds.entry(kind).or_default() += 1;
        *aligns.entry(elf.align).or_default() += 1;
        if elf.tls {
            tls += 1;
        }
        elfs64.push((relative, elf));
    }

    println!("root: {}", root.display());
    println!(
        "files scanned (regular, not symlinks) under system/ vendor/ apex/: {}",
        files.len()
    );
    println!("ELF by class/machine:");
    for ((class, machine), count) in count_rows(&by_machine) {
        println!("  ELF{class} {machine:<8} {count:>6}");
    }
    println!("ELF64 aarch64 by type:");
    for (kind, count) in count_rows(&kinds) {
        println!("  {kind:<28} {count:>6}");
    }
    println!("ELF64 aarch64 by smallest PT_LOAD p_align:");
    for (align, count) in &aligns {
        println!("  {:<8} {count:>6}", format!("{align:#x}"));
    }
    println!("ELF64 aarch64 with PT_TLS: {tls}");
    for relative in [
        "system/apex/com.android.runtime/bin/linker64",
        "system/apex/com.android.runtime/lib64/bionic/libc.so",
        "system/bin/bootstrap/linker64",
        "system/lib64/bootstrap/libc.so",
        "system/bin/toybox",
        "system/bin/sh",
        "system/bin/dalvikvm64",
        "system/apex/com.android.art.release/lib64/libart.so",
    ] {
        let path = root.join(relative);
        if path.is_file() {
            if let Some(elf) = elf_info(&path) {
                let kind = match elf.kind {
                    2 => "EXEC".to_owned(),
                    3 => "DYN".to_owned(),
                    other => format!("type{other}"),
                };
                println!(
                    "  {relative:<58} {} {kind} p_align {:#x}{}",
                    elf.machine,
                    elf.align,
                    if elf.interp { " PT_INTERP" } else { "" }
                );
            }
        }
    }

    if let Some(lxrun) = lxrun {
        let mut candidates = elfs64
            .iter()
            .filter(|(relative, elf)| elf.exec_load && !relative.starts_with("apex/"))
            .collect::<Vec<_>>();
        if x18_limit > 0 {
            candidates.truncate(x18_limit);
        }
        let mut total = [0u64; 4];
        let mut files_with = 0usize;
        let mut refused = Vec::new();
        let mut top = Vec::new();
        for (relative, _) in &candidates {
            let guest = root.join(relative.as_str());
            let Some(counts) = x18_count(lxrun, &guest, &root) else {
                refused.push((*relative).clone());
                continue;
            };
            let [found, _windows, rewritten, unsupported, unreachable] = counts;
            total[0] += found;
            total[1] += rewritten;
            total[2] += unsupported;
            total[3] += unreachable;
            if found > 0 {
                files_with += 1;
                top.push((found, (*relative).clone(), unsupported, unreachable));
            }
        }
        println!(
            "x18 (lxrun --dry-run, LXRT_GUEST_PAGE=4096) over {} aarch64 ELF files with code:",
            candidates.len()
        );
        println!(
            "  files with x18 sites: {files_with}; sites found {}, rewritten {}, unsupported {}, unreachable {}",
            total[0], total[1], total[2], total[3]
        );
        let refused_detail = if refused.is_empty() {
            String::new()
        } else {
            format!(
                ": {}",
                refused
                    .iter()
                    .take(10)
                    .cloned()
                    .collect::<Vec<_>>()
                    .join(", ")
            )
        };
        println!(
            "  refused by lxrun (no x18 line): {}{refused_detail}",
            refused.len()
        );
        top.sort_by(|a, b| b.cmp(a));
        for (found, relative, unsupported, unreachable) in top.into_iter().take(15) {
            println!(
                "  {found:>6}  {relative}{}",
                if unsupported > 0 || unreachable > 0 {
                    format!(" (unsupported {unsupported}, unreachable {unreachable})")
                } else {
                    String::new()
                }
            );
        }
    }
}

fn run(args: &[String]) -> Result<i32, String> {
    if args.len() >= 2 && args[0] == "image-kind" {
        println!("{}", classify_image(Path::new(&args[1]))?);
        return Ok(0);
    }
    if args.len() >= 3 && args[0] == "unsparse" {
        unsparse(Path::new(&args[1]), Path::new(&args[2]))?;
        return Ok(0);
    }
    if args.len() >= 2 && args[0] == "apex-names" {
        apex_names(Path::new(&args[1]))?;
        return Ok(0);
    }
    if args.len() >= 2 && args[0] == "inventory" {
        let mut lxrun = None;
        let mut limit = 0usize;
        let mut index = 2;
        while index < args.len() {
            match args[index].as_str() {
                "--lxrun" if index + 1 < args.len() => {
                    lxrun = Some(PathBuf::from(&args[index + 1]));
                    index += 2;
                }
                "--x18-limit" if index + 1 < args.len() => {
                    limit = args[index + 1]
                        .parse()
                        .map_err(|_| format!("invalid --x18-limit: {}", args[index + 1]))?;
                    index += 2;
                }
                option => {
                    eprintln!("unknown option {option}");
                    return Ok(2);
                }
            }
        }
        inventory(Path::new(&args[1]), lxrun.as_deref(), limit);
        return Ok(0);
    }
    eprintln!("{USAGE}");
    Ok(2)
}

fn main() {
    let args = env::args().skip(1).collect::<Vec<_>>();
    match run(&args) {
        Ok(status) => std::process::exit(status),
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(1);
        }
    }
}
