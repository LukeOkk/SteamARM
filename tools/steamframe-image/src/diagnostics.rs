use super::{partitions, Btrfs, Image};
use serde_json::{json, Value};
use std::collections::{BTreeMap, HashSet};
use std::ffi::OsString;
use std::fs;
use std::path::PathBuf;

#[derive(Hash, PartialEq, Eq)]
enum ExtentKey {
    Inline(u64, u64),
    Disk(u64),
}

fn compression_name(compression: u8) -> String {
    match compression {
        0 => "none".into(),
        1 => "zlib".into(),
        2 => "lzo".into(),
        3 => "zstd".into(),
        other => other.to_string(),
    }
}

fn zstd_frame_content_size(raw: &[u8]) -> Value {
    if raw.len() < 5 || raw[..4] != [0x28, 0xb5, 0x2f, 0xfd] {
        return json!("no-magic");
    }
    let descriptor = raw[4];
    let size_flag = descriptor >> 6;
    let single_segment = descriptor & 0x20 != 0;
    let dictionary_size = match descriptor & 0x03 {
        0 => 0,
        1 => 1,
        2 => 2,
        _ => 4,
    };
    let content_size_len = match size_flag {
        0 if single_segment => 1,
        0 => 0,
        1 => 2,
        2 => 4,
        _ => 8,
    };
    if content_size_len == 0 {
        return Value::Null;
    }
    let content_size_offset: usize = 5 + if single_segment { 0 } else { 1 } + dictionary_size;
    let Some(end) = content_size_offset.checked_add(content_size_len) else {
        return Value::Null;
    };
    let Some(encoded) = raw.get(content_size_offset..end) else {
        return Value::Null;
    };
    let mut bytes = [0u8; 8];
    bytes[..content_size_len].copy_from_slice(encoded);
    let mut size = u64::from_le_bytes(bytes);
    if size_flag == 1 {
        size += 256;
    }
    json!(size)
}

fn error_kind(compression: u8) -> &'static str {
    match compression {
        1 => "zlib-error",
        2 => "lzo-error",
        3 => "zstd-error",
        _ => "compression-error",
    }
}

fn parse_number<T: std::str::FromStr>(value: &str, label: &str) -> Result<T, String> {
    value
        .parse::<T>()
        .map_err(|_| format!("invalid {label} {value:?}"))
}

fn usage() -> &'static str {
    "Usage: steamarm-frame-image diagnose IMAGE [--part N] [--subvol-id N] [--examples N] [--json PATH]"
}

pub fn run(args: Vec<OsString>) -> Result<i32, String> {
    let mut part = None;
    let mut subvol_id = None;
    let mut example_limit = 5usize;
    let mut json_path = None;
    let mut image_path = None;
    let mut args = args.into_iter();
    let mut positional_only = false;

    while let Some(arg) = args.next() {
        let text = arg.to_string_lossy().into_owned();
        if positional_only {
            if image_path.replace(PathBuf::from(arg)).is_some() {
                return Err(usage().into());
            }
            continue;
        }
        if text == "--" {
            positional_only = true;
        } else if text == "--help" || text == "-h" {
            println!("{}", usage());
            return Ok(0);
        } else if text == "--part"
            || text == "--subvol-id"
            || text == "--examples"
            || text == "--json"
        {
            let value = args
                .next()
                .ok_or_else(|| format!("missing value for {text}\n{}", usage()))?;
            let value = value
                .to_str()
                .ok_or_else(|| format!("value for {text} is not valid UTF-8"))?;
            match text.as_str() {
                "--part" => part = Some(parse_number::<u32>(value, "partition index")?),
                "--subvol-id" => subvol_id = Some(parse_number::<u64>(value, "subvolume id")?),
                "--examples" => {
                    let value = parse_number::<i64>(value, "example count")?;
                    example_limit = usize::try_from(value.max(0)).unwrap_or(usize::MAX);
                }
                "--json" => json_path = Some(PathBuf::from(value)),
                _ => unreachable!(),
            }
        } else if let Some(value) = text.strip_prefix("--part=") {
            part = Some(parse_number::<u32>(value, "partition index")?);
        } else if let Some(value) = text.strip_prefix("--subvol-id=") {
            subvol_id = Some(parse_number::<u64>(value, "subvolume id")?);
        } else if let Some(value) = text.strip_prefix("--examples=") {
            let value = parse_number::<i64>(value, "example count")?;
            example_limit = usize::try_from(value.max(0)).unwrap_or(usize::MAX);
        } else if let Some(value) = text.strip_prefix("--json=") {
            json_path = Some(PathBuf::from(value));
        } else if text.starts_with('-') {
            return Err(format!("unknown option {text:?}\n{}", usage()));
        } else if image_path.replace(PathBuf::from(arg)).is_some() {
            return Err(usage().into());
        }
    }

    let image_path = image_path.ok_or_else(|| usage().to_owned())?;
    let image = Image::open(&image_path)?;
    let parts = partitions(&image)?;
    let partition = super::extractor::select_partition(&parts, part)?;
    let mut btrfs = Btrfs::open(&image, partition.offset)?;
    btrfs.load_roots()?;
    let subvolume = subvol_id.unwrap_or(btrfs.default_subvol);
    eprintln!(
        "partition {} ({}), subvolume {}: loading the tree...",
        partition.index, partition.name, subvolume
    );
    let (inodes, _) = btrfs.load_tree(subvolume)?;

    let mut counts = BTreeMap::<String, u64>::new();
    let mut by_compression = BTreeMap::<String, u64>::new();
    let mut examples = BTreeMap::<String, Vec<Value>>::new();
    let mut seen = HashSet::new();

    for (&ino, record) in &inodes {
        for extent in &record.extents {
            let compression = compression_name(extent.compression);
            *by_compression.entry(compression).or_default() += 1;
            if extent.compression == 0 {
                continue;
            }
            let key = if extent.extent_type == 0 {
                ExtentKey::Inline(ino, extent.offset)
            } else {
                ExtentKey::Disk(extent.disk)
            };
            if !seen.insert(key) {
                continue;
            }

            let raw = if extent.extent_type == 0 {
                Ok(extent.inline.clone())
            } else {
                usize::try_from(extent.disk_len)
                    .map_err(|_| "compressed extent exceeds addressable memory".to_owned())
                    .and_then(|length| btrfs.read_logical(extent.disk, length))
            };
            let raw = match raw {
                Ok(raw) => raw,
                Err(error) => {
                    *counts.entry("unreadable".into()).or_default() += 1;
                    examples
                        .entry("unreadable".into())
                        .or_default()
                        .push(json!({"ino": ino, "err": error}));
                    continue;
                }
            };

            let frame_content_size = if extent.compression == 3 {
                zstd_frame_content_size(&raw)
            } else {
                Value::Null
            };
            let ram_bytes = usize::try_from(extent.ram_bytes)
                .map_err(|_| format!("extent for inode {ino} exceeds addressable memory"))?;
            let (note, kind) = match btrfs.decompress(&raw, ram_bytes, extent.compression) {
                Ok((_, note)) => {
                    let kind = note
                        .as_deref()
                        .unwrap_or("ok")
                        .split_whitespace()
                        .next()
                        .unwrap_or("ok")
                        .to_owned();
                    (note, kind)
                }
                Err(error) => {
                    let kind = error_kind(extent.compression);
                    (Some(format!("{kind} ({error})")), kind.to_owned())
                }
            };
            *counts.entry(kind.clone()).or_default() += 1;
            if kind != "ok" {
                let entries = examples.entry(kind).or_default();
                if entries.len() < example_limit {
                    entries.push(json!({
                        "ino": ino,
                        "file_offset": extent.offset,
                        "inline": extent.extent_type == 0,
                        "disk_bytenr": if extent.extent_type == 0 { None } else { Some(extent.disk) },
                        "disk_len": if extent.extent_type == 0 { raw.len() as u64 } else { extent.disk_len },
                        "ram_bytes": extent.ram_bytes,
                        "frame_content_size": frame_content_size,
                        "note": note,
                    }));
                }
            }
        }
    }

    println!(
        "file extents by compression: {}",
        serde_json::to_string(&by_compression).map_err(|error| error.to_string())?
    );
    println!(
        "distinct compressed extents: {}",
        counts.values().sum::<u64>()
    );
    let mut ordered_counts = counts.iter().collect::<Vec<_>>();
    ordered_counts.sort_by(|(left_name, left_count), (right_name, right_count)| {
        right_count
            .cmp(left_count)
            .then_with(|| left_name.cmp(right_name))
    });
    for (kind, count) in ordered_counts {
        println!("  {kind:<28} {count}");
    }
    for (kind, entries) in &examples {
        println!("\n{kind}: first {}", entries.len());
        for example in entries {
            println!(
                "  {}",
                serde_json::to_string(example).map_err(|error| error.to_string())?
            );
        }
    }
    println!("\nMeaning:");
    println!("  ok                     frame decodes to exactly ram_bytes");
    println!("  frame-larger-than-ram  the frame holds more than the extent: the kernel stops at");
    println!("                         ram_bytes; btrfs-progs `restore` reports \"zstd frame incomplete\"");
    println!(
        "  frame-incomplete-input input ends before the frame does: damaged or truncated image"
    );
    println!("  short-output           the stream ended more than a sector short of ram_bytes");
    println!("  zstd-error/zlib-error  the data does not decode at all: damaged or misplaced");

    if let Some(path) = json_path {
        let report = json!({
            "partition": partition.index,
            "subvolume": subvolume,
            "counts": counts,
            "by_compression": by_compression,
            "examples": examples,
        });
        let encoded = serde_json::to_vec_pretty(&report)
            .map_err(|error| format!("serialize diagnostic report: {error}"))?;
        fs::write(&path, encoded)
            .map_err(|error| format!("write diagnostic report {}: {error}", path.display()))?;
    }

    let bad = counts
        .iter()
        .filter(|(kind, _)| kind.as_str() != "ok" && kind.as_str() != "frame-larger-than-ram")
        .map(|(_, count)| *count)
        .sum::<u64>();
    Ok(i32::from(bad > 0))
}
