use std::collections::{BTreeMap, HashMap, VecDeque};
use std::env;
use std::fs::{self, File};
use std::io::Write;
use std::os::unix::fs::{FileExt, PermissionsExt};
use std::path::Path;
use std::process::Command;

mod compare;
mod diagnostics;
mod extractor;
mod inventory;

const SUPER_OFF: u64 = 0x10000;
const BTRFS_MAGIC: &[u8; 8] = b"_BHRfS_M";
const HEADER: usize = 101;
const ITEM: usize = 25;
const KEYPTR: usize = 33;
const CHUNK_ITEM: u8 = 228;
const ROOT_ITEM: u8 = 132;
const ROOT_REF: u8 = 156;
const DIR_ITEM: u8 = 84;
const INODE_ITEM: u8 = 1;
const XATTR_ITEM: u8 = 24;
const DIR_INDEX: u8 = 96;
const EXTENT_DATA: u8 = 108;
const ROOT_TREE_DIR_OBJECTID: u64 = 6;
const FS_TREE_OBJECTID: u64 = 5;
const FIRST_FREE_OBJECTID: u64 = 256;

#[derive(Debug)]
struct Image {
    file: File,
    size: u64,
}

impl Image {
    fn open(path: &Path) -> Result<Self, String> {
        let file = File::open(path).map_err(|error| format!("open {}: {error}", path.display()))?;
        let size = file
            .metadata()
            .map_err(|error| format!("stat {}: {error}", path.display()))?
            .len();
        Ok(Self { file, size })
    }

    fn read(&self, offset: u64, length: usize) -> Result<Vec<u8>, String> {
        let end = offset
            .checked_add(length as u64)
            .ok_or_else(|| "image read offset overflow".to_owned())?;
        if end > self.size {
            return Err(format!(
                "read {length} bytes at {offset:#x} past image end ({:#x}); image or partition is truncated",
                self.size
            ));
        }
        let mut out = vec![0; length];
        let mut read = 0;
        while read < length {
            let count = self
                .file
                .read_at(&mut out[read..], offset + read as u64)
                .map_err(|error| format!("read image at {:#x}: {error}", offset + read as u64))?;
            if count == 0 {
                return Err(format!("short read at {:#x}", offset + read as u64));
            }
            read += count;
        }
        Ok(out)
    }
}

fn bytes(data: &[u8], offset: usize, length: usize) -> Result<&[u8], String> {
    data.get(offset..offset.saturating_add(length))
        .ok_or_else(|| format!("truncated structure at byte {offset}"))
}

fn u8_at(data: &[u8], offset: usize) -> Result<u8, String> {
    data.get(offset)
        .copied()
        .ok_or_else(|| format!("truncated u8 at byte {offset}"))
}

fn u16_at(data: &[u8], offset: usize) -> Result<u16, String> {
    Ok(u16::from_le_bytes(
        bytes(data, offset, 2)?.try_into().expect("fixed slice"),
    ))
}

fn u32_at(data: &[u8], offset: usize) -> Result<u32, String> {
    Ok(u32::from_le_bytes(
        bytes(data, offset, 4)?.try_into().expect("fixed slice"),
    ))
}

fn u64_at(data: &[u8], offset: usize) -> Result<u64, String> {
    Ok(u64::from_le_bytes(
        bytes(data, offset, 8)?.try_into().expect("fixed slice"),
    ))
}

fn human(mut size: u64) -> String {
    for unit in ["B", "KiB", "MiB", "GiB", "TiB"] {
        if size < 1024 || unit == "TiB" {
            return if unit == "B" {
                format!("{size} B")
            } else {
                format!("{:.1} {unit}", size as f64)
            };
        }
        size /= 1024;
    }
    unreachable!()
}

fn crc32(data: &[u8]) -> u32 {
    let mut crc = !0u32;
    for byte in data {
        crc ^= u32::from(*byte);
        for _ in 0..8 {
            crc = (crc >> 1) ^ (0xedb8_8320 & (0u32.wrapping_sub(crc & 1)));
        }
    }
    !crc
}

fn guid_text(data: &[u8]) -> Result<String, String> {
    let raw = bytes(data, 0, 16)?;
    Ok(format!(
        "{:08X}-{:04X}-{:04X}-{:02X}{:02X}-{:02X}{:02X}{:02X}{:02X}{:02X}{:02X}",
        u32::from_le_bytes(raw[0..4].try_into().expect("fixed slice")),
        u16::from_le_bytes(raw[4..6].try_into().expect("fixed slice")),
        u16::from_le_bytes(raw[6..8].try_into().expect("fixed slice")),
        raw[8],
        raw[9],
        raw[10],
        raw[11],
        raw[12],
        raw[13],
        raw[14],
        raw[15]
    ))
}

fn guid_type(guid: &str) -> &'static str {
    match guid {
        "C12A7328-F81F-11D2-BA4B-00A0C93EC93B" => "EFI system",
        "0FC63DAF-8483-4772-8E79-3D69D8477DE4" => "Linux filesystem",
        "B921B045-1DF0-41C3-AF44-4C6F280D3FAE" => "Linux root (ARM64)",
        "4F68BCE3-E8CD-4DB1-96E7-FBCAF984B709" => "Linux root (x86-64)",
        "933AC7E1-2EB4-4F13-B844-0E14E2AEF915" => "Linux /home",
        "4D21B016-B534-45C2-A9FB-5C16E091FD2D" => "Linux /var",
        "0657FD6D-A4AB-43C4-84E5-0933C84B4F4F" => "Linux swap",
        _ => "",
    }
}

#[derive(Debug)]
struct Partition {
    index: u32,
    name: String,
    type_guid: String,
    offset: u64,
    size: u64,
    truncated: bool,
    fs: String,
}

fn detect_fs(image: &Image, offset: u64, size: u64) -> String {
    if size > SUPER_OFF + 0x48 {
        if image
            .read(offset.saturating_add(SUPER_OFF + 0x40), 8)
            .is_ok_and(|magic| magic.as_slice() == BTRFS_MAGIC)
        {
            return "btrfs".into();
        }
    }
    let length = usize::try_from(size.min(4096)).unwrap_or(0);
    let head = match image.read(offset, length) {
        Ok(data) => data,
        Err(_) => return "unreadable".into(),
    };
    if head.len() >= 1024 + 58 && u16_at(&head, 1024 + 56).ok() == Some(0xef53) {
        let compat = u32_at(&head, 1024 + 0x5c).unwrap_or(0);
        let incompat = u32_at(&head, 1024 + 0x60).unwrap_or(0);
        return if incompat & 0x40 != 0 {
            "ext4"
        } else if compat & 0x4 != 0 {
            "ext3"
        } else {
            "ext2"
        }
        .into();
    }
    if head.len() >= 1028 && u32_at(&head, 1024).ok() == Some(0xe0f5_e1e2) {
        return "erofs".into();
    }
    if head.starts_with(b"hsqs") {
        return "squashfs".into();
    }
    if head.len() >= 90
        && (head.get(82..87).is_some_and(|value| value == b"FAT32")
            || head
                .get(54..59)
                .is_some_and(|value| value == b"FAT16" || value == b"FAT12"))
    {
        return "vfat".into();
    }
    if head.iter().all(|byte| *byte == 0) {
        return "empty".into();
    }
    "unknown".into()
}

fn partitions(image: &Image) -> Result<Vec<Partition>, String> {
    for sector in [512u32, 4096] {
        let lba = u64::from(sector);
        if image.size < lba * 2 + 92 {
            continue;
        }
        let header = image.read(lba, 92)?;
        if &header[..8] != b"EFI PART" {
            continue;
        }
        let entries_lba = u64_at(&header, 72)?;
        let count = u32_at(&header, 80)?;
        let entry_size = u32_at(&header, 84)?;
        if count > 1_000_000 || entry_size < 128 {
            return Err(format!(
                "invalid GPT entry table ({count} entries, {entry_size} bytes each)"
            ));
        }
        let table_size = u64::from(count)
            .checked_mul(u64::from(entry_size))
            .ok_or_else(|| "GPT entry table size overflow".to_owned())?;
        let table_len = usize::try_from(table_size)
            .map_err(|_| "GPT entry table exceeds addressable memory".to_owned())?;
        let table = image.read(
            entries_lba
                .checked_mul(lba)
                .ok_or_else(|| "GPT table offset overflow".to_owned())?,
            table_len,
        )?;
        if crc32(&table) != u32_at(&header, 88)? {
            eprintln!("  warning: GPT entry array CRC mismatch (sector size {sector})");
        }
        let mut result = Vec::new();
        for index in 0..count {
            let start = usize::try_from(index)
                .ok()
                .and_then(|i| i.checked_mul(entry_size as usize))
                .ok_or_else(|| "GPT entry offset overflow".to_owned())?;
            let entry = bytes(&table, start, entry_size as usize)?;
            if entry[..16].iter().all(|byte| *byte == 0) {
                continue;
            }
            let first = u64_at(entry, 32)?;
            let last = u64_at(entry, 40)?;
            if last < first {
                return Err(format!("GPT partition {} has an invalid range", index + 1));
            }
            let offset = first
                .checked_mul(lba)
                .ok_or_else(|| "GPT partition offset overflow".to_owned())?;
            let size = (last - first + 1)
                .checked_mul(lba)
                .ok_or_else(|| "GPT partition size overflow".to_owned())?;
            let name_bytes = bytes(entry, 56, 72)?;
            let words = name_bytes
                .chunks_exact(2)
                .map(|pair| u16::from_le_bytes([pair[0], pair[1]]))
                .take_while(|word| *word != 0);
            let name = String::from_utf16_lossy(&words.collect::<Vec<_>>());
            let type_guid = guid_text(&entry[..16])?;
            let _uuid = guid_text(&entry[16..32])?;
            let truncated = offset.saturating_add(size) > image.size;
            let readable = image.size.saturating_sub(offset).min(size);
            let fs = detect_fs(image, offset, readable);
            result.push(Partition {
                index: index + 1,
                name,
                type_guid,
                offset,
                size,
                truncated,
                fs,
            });
        }
        return Ok(result);
    }
    Ok(vec![Partition {
        index: 0,
        name: "(whole image)".into(),
        type_guid: String::new(),
        offset: 0,
        size: image.size,
        truncated: false,
        fs: detect_fs(image, 0, image.size),
    }])
}

#[derive(Clone, Debug, Eq, PartialEq)]
struct Chunk {
    logical: u64,
    length: u64,
    kind: u64,
    stripes: Vec<(u64, u64)>,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq, Ord, PartialOrd)]
struct Key {
    object: u64,
    kind: u8,
    offset: u64,
}

#[derive(Clone, Debug)]
struct Root {
    bytenr: u64,
    dirid: u64,
    flags: u64,
}

#[derive(Clone, Debug)]
struct RootRef {
    parent: u64,
    name: String,
}

#[derive(Clone, Debug)]
struct Inode {
    size: u64,
    links: u32,
    uid: u32,
    gid: u32,
    mode: u32,
    rdev: u64,
    atime: f64,
    mtime: f64,
}

#[derive(Clone, Debug)]
struct DirectoryEntry {
    index: u64,
    child: u64,
    child_type: u8,
    name: Vec<u8>,
}

#[derive(Clone, Debug)]
struct FileExtent {
    offset: u64,
    ram_bytes: u64,
    compression: u8,
    encryption: u8,
    other_encoding: u16,
    extent_type: u8,
    inline: Vec<u8>,
    disk: u64,
    disk_len: u64,
    extent_offset: u64,
    extent_len: u64,
}

#[derive(Clone, Debug, Default)]
struct FileRecord {
    inode: Option<Inode>,
    directories: Vec<DirectoryEntry>,
    extents: Vec<FileExtent>,
    xattrs: BTreeMap<String, Vec<u8>>,
}

struct Btrfs<'a> {
    image: &'a Image,
    base: u64,
    fsid: Vec<u8>,
    metadata_uuid: Vec<u8>,
    generation: u64,
    root_bytenr: u64,
    chunk_root: u64,
    total_bytes: u64,
    bytes_used: u64,
    num_devices: u64,
    sectorsize: u32,
    nodesize: u32,
    compat_ro: u64,
    incompat: u64,
    checksum_type: u16,
    devid: u64,
    label: String,
    chunks: Vec<Chunk>,
    roots: BTreeMap<u64, Root>,
    root_refs: HashMap<u64, RootRef>,
    default_subvol: u64,
    extent_cache: HashMap<u64, Vec<u8>>,
    extent_order: VecDeque<u64>,
}

impl<'a> Btrfs<'a> {
    fn open(image: &'a Image, base: u64) -> Result<Self, String> {
        let sb = image.read(base + SUPER_OFF, 4096)?;
        if bytes(&sb, 0x40, 8)? != BTRFS_MAGIC {
            return Err("no btrfs superblock at this partition".into());
        }
        let mut fs = Self {
            image,
            base,
            fsid: bytes(&sb, 0x20, 16)?.to_vec(),
            metadata_uuid: bytes(&sb, 0x23b, 16)?.to_vec(),
            generation: u64_at(&sb, 0x48)?,
            root_bytenr: u64_at(&sb, 0x50)?,
            chunk_root: u64_at(&sb, 0x58)?,
            total_bytes: u64_at(&sb, 0x70)?,
            bytes_used: u64_at(&sb, 0x78)?,
            num_devices: u64_at(&sb, 0x88)?,
            sectorsize: u32_at(&sb, 0x90)?,
            nodesize: u32_at(&sb, 0x94)?,
            compat_ro: u64_at(&sb, 0xb4)?,
            incompat: u64_at(&sb, 0xbc)?,
            checksum_type: u16_at(&sb, 0xc4)?,
            devid: u64_at(&sb, 0xc9)?,
            label: String::from_utf8_lossy(bytes(&sb, 0x12b, 0x100)?)
                .trim_end_matches('\0')
                .to_owned(),
            chunks: Vec::new(),
            roots: BTreeMap::new(),
            root_refs: HashMap::new(),
            default_subvol: FS_TREE_OBJECTID,
            extent_cache: HashMap::new(),
            extent_order: VecDeque::new(),
        };
        let sys_len = u32_at(&sb, 0xa0)? as usize;
        let sys_array = bytes(&sb, 0x32b, sys_len)?;
        let mut pos = 0;
        while pos < sys_len {
            let logical = u64_at(sys_array, pos + 9)?;
            pos += 17;
            let used = fs.parse_chunk(sys_array, pos, logical)?;
            pos = pos
                .checked_add(used)
                .ok_or_else(|| "system chunk array overflow".to_owned())?;
        }
        fs.chunks.sort_by_key(|chunk| chunk.logical);
        let chunk_items = fs.walk(fs.chunk_root, Some(&[CHUNK_ITEM]))?;
        for (key, data) in chunk_items {
            fs.parse_chunk(&data, 0, key.offset)?;
        }
        fs.chunks.sort_by_key(|chunk| chunk.logical);
        fs.chunks.dedup();
        Ok(fs)
    }

    fn parse_chunk(&mut self, data: &[u8], pos: usize, logical: u64) -> Result<usize, String> {
        let length = u64_at(data, pos)?;
        let kind = u64_at(data, pos + 24)?;
        let stripe_count = u16_at(data, pos + 44)? as usize;
        let stripe_bytes = stripe_count
            .checked_mul(32)
            .ok_or_else(|| "btrfs stripe list overflow".to_owned())?;
        let total = 48usize
            .checked_add(stripe_bytes)
            .ok_or_else(|| "btrfs chunk size overflow".to_owned())?;
        bytes(data, pos, total)?;
        let mut stripes = Vec::with_capacity(stripe_count);
        for i in 0..stripe_count {
            let stripe = pos + 48 + i * 32;
            stripes.push((u64_at(data, stripe)?, u64_at(data, stripe + 8)?));
        }
        self.chunks.push(Chunk {
            logical,
            length,
            kind,
            stripes,
        });
        Ok(total)
    }

    fn phys(&self, logical: u64) -> Result<u64, String> {
        let chunk = self
            .chunks
            .iter()
            .find(|chunk| chunk.logical <= logical && logical < chunk.logical + chunk.length)
            .ok_or_else(|| format!("logical {logical:#x} is in no chunk"))?;
        for (bit, name) in [
            (1 << 3, "RAID0"),
            (1 << 6, "RAID10"),
            (1 << 7, "RAID5"),
            (1 << 8, "RAID6"),
        ] {
            if chunk.kind & bit != 0 {
                return Err(format!(
                    "{name} block group at {logical:#x}: striped profiles need every device"
                ));
            }
        }
        let physical = chunk
            .stripes
            .iter()
            .find(|(device, _)| *device == self.devid)
            .or_else(|| (chunk.stripes.len() == 1).then(|| &chunk.stripes[0]))
            .or_else(|| chunk.stripes.first())
            .ok_or_else(|| format!("logical {logical:#x} has no device stripe"))?
            .1;
        self.base
            .checked_add(physical)
            .and_then(|base| base.checked_add(logical - chunk.logical))
            .ok_or_else(|| "btrfs physical offset overflow".to_owned())
    }

    fn read_logical(&self, mut logical: u64, mut length: usize) -> Result<Vec<u8>, String> {
        let mut result = Vec::with_capacity(length);
        while length > 0 {
            let chunk = self
                .chunks
                .iter()
                .find(|chunk| chunk.logical <= logical && logical < chunk.logical + chunk.length)
                .ok_or_else(|| format!("logical {logical:#x} is in no chunk"))?;
            let remaining =
                usize::try_from(chunk.logical + chunk.length - logical).unwrap_or(usize::MAX);
            let take = length.min(remaining);
            result.extend(self.image.read(self.phys(logical)?, take)?);
            logical = logical
                .checked_add(take as u64)
                .ok_or_else(|| "btrfs logical offset overflow".to_owned())?;
            length -= take;
        }
        Ok(result)
    }

    fn node(&self, logical: u64) -> Result<Vec<u8>, String> {
        let block = self.read_logical(logical, self.nodesize as usize)?;
        let fsid = bytes(&block, 0x20, 16)?;
        if fsid != self.fsid && fsid != self.metadata_uuid {
            return Err(format!(
                "tree block {logical:#x}: fsid mismatch (wrong offset or foreign data)"
            ));
        }
        let bytenr = u64_at(&block, 0x30)?;
        if bytenr != logical {
            return Err(format!("tree block {logical:#x}: header says {bytenr:#x}"));
        }
        Ok(block)
    }

    fn walk(&self, bytenr: u64, want: Option<&[u8]>) -> Result<Vec<(Key, Vec<u8>)>, String> {
        let mut out = Vec::new();
        self.walk_into(bytenr, want, 0, &mut out)?;
        Ok(out)
    }

    fn walk_into(
        &self,
        bytenr: u64,
        want: Option<&[u8]>,
        depth: u8,
        out: &mut Vec<(Key, Vec<u8>)>,
    ) -> Result<(), String> {
        if depth > 16 {
            return Err("btrfs tree depth exceeds 16".into());
        }
        let block = self.node(bytenr)?;
        let item_count = u32_at(&block, 0x60)? as usize;
        let level = u8_at(&block, 0x64)?;
        if level == 0 {
            for i in 0..item_count {
                let pos = HEADER
                    .checked_add(i.checked_mul(ITEM).ok_or("btrfs leaf offset overflow")?)
                    .ok_or("btrfs leaf offset overflow")?;
                let key = Key {
                    object: u64_at(&block, pos)?,
                    kind: u8_at(&block, pos + 8)?,
                    offset: u64_at(&block, pos + 9)?,
                };
                if want.is_some_and(|types| !types.contains(&key.kind)) {
                    continue;
                }
                let data_offset = u32_at(&block, pos + 17)? as usize;
                let data_len = u32_at(&block, pos + 21)? as usize;
                out.push((key, bytes(&block, HEADER + data_offset, data_len)?.to_vec()));
            }
            return Ok(());
        }
        for i in 0..item_count {
            let pos = HEADER
                .checked_add(
                    i.checked_mul(KEYPTR)
                        .ok_or("btrfs pointer offset overflow")?,
                )
                .ok_or("btrfs pointer offset overflow")?;
            let child = u64_at(&block, pos + 17)?;
            self.walk_into(child, want, depth + 1, out)?;
        }
        Ok(())
    }

    fn load_roots(&mut self) -> Result<(), String> {
        if !self.roots.is_empty() {
            return Ok(());
        }
        for (key, data) in self.walk(self.root_bytenr, None)? {
            match key.kind {
                ROOT_ITEM if data.len() >= 239 => {
                    self.roots.insert(
                        key.object,
                        Root {
                            bytenr: u64_at(&data, 176)?,
                            dirid: u64_at(&data, 168)?,
                            flags: u64_at(&data, 208)?,
                        },
                    );
                }
                ROOT_REF if data.len() >= 18 => {
                    let name_len = u16_at(&data, 16)? as usize;
                    let name = String::from_utf8_lossy(bytes(&data, 18, name_len)?).into_owned();
                    self.root_refs.insert(
                        key.offset,
                        RootRef {
                            parent: key.object,
                            name,
                        },
                    );
                }
                DIR_ITEM if key.object == ROOT_TREE_DIR_OBJECTID && data.len() >= 30 => {
                    let name_len = u16_at(&data, 27)? as usize;
                    if bytes(&data, 30, name_len)? == b"default" {
                        self.default_subvol = u64_at(&data, 0)?;
                    }
                }
                _ => {}
            }
        }
        Ok(())
    }

    fn subvolumes(&mut self) -> Result<Vec<(u64, String, bool, bool)>, String> {
        self.load_roots()?;
        let mut result = Vec::new();
        for (id, root) in &self.roots {
            if *id != FS_TREE_OBJECTID && !(*id >= FIRST_FREE_OBJECTID && *id < (1 << 63)) {
                continue;
            }
            let mut path = Vec::new();
            let mut current = *id;
            for _ in 0..64 {
                let Some(reference) = self.root_refs.get(&current) else {
                    break;
                };
                path.push(reference.name.clone());
                current = reference.parent;
            }
            path.reverse();
            let path = if path.is_empty() {
                "<top level>".into()
            } else {
                path.join("/")
            };
            result.push((*id, path, root.flags & 1 != 0, *id == self.default_subvol));
        }
        result.sort_by_key(|entry| entry.0);
        Ok(result)
    }

    fn load_tree(&mut self, root_id: u64) -> Result<(BTreeMap<u64, FileRecord>, u64), String> {
        self.load_roots()?;
        let root = self
            .roots
            .get(&root_id)
            .cloned()
            .ok_or_else(|| format!("no subvolume with id {root_id}"))?;
        let want = [INODE_ITEM, DIR_INDEX, EXTENT_DATA, XATTR_ITEM];
        let mut inodes = BTreeMap::<u64, FileRecord>::new();
        for (key, data) in self.walk(root.bytenr, Some(&want))? {
            let record = inodes.entry(key.object).or_default();
            match key.kind {
                INODE_ITEM if data.len() >= 160 => {
                    record.inode = Some(Inode {
                        size: u64_at(&data, 16)?,
                        links: u32_at(&data, 40)?,
                        uid: u32_at(&data, 44)?,
                        gid: u32_at(&data, 48)?,
                        mode: u32_at(&data, 52)?,
                        rdev: u64_at(&data, 56)?,
                        atime: u64_at(&data, 112)? as f64
                            + u32_at(&data, 120)? as f64 / 1_000_000_000.0,
                        mtime: u64_at(&data, 136)? as f64
                            + u32_at(&data, 144)? as f64 / 1_000_000_000.0,
                    });
                }
                DIR_INDEX if data.len() >= 30 => {
                    let name_len = u16_at(&data, 27)? as usize;
                    record.directories.push(DirectoryEntry {
                        index: key.offset,
                        child: u64_at(&data, 0)?,
                        child_type: u8_at(&data, 8)?,
                        name: bytes(&data, 30, name_len)?.to_vec(),
                    });
                }
                EXTENT_DATA if data.len() >= 21 => {
                    let extent_type = u8_at(&data, 20)?;
                    let mut extent = FileExtent {
                        offset: key.offset,
                        ram_bytes: u64_at(&data, 8)?,
                        compression: u8_at(&data, 16)?,
                        encryption: u8_at(&data, 17)?,
                        other_encoding: u16_at(&data, 18)?,
                        extent_type,
                        inline: Vec::new(),
                        disk: 0,
                        disk_len: 0,
                        extent_offset: 0,
                        extent_len: 0,
                    };
                    if extent_type == 0 {
                        extent.inline = data[21..].to_vec();
                    } else if data.len() >= 53 {
                        extent.disk = u64_at(&data, 21)?;
                        extent.disk_len = u64_at(&data, 29)?;
                        extent.extent_offset = u64_at(&data, 37)?;
                        extent.extent_len = u64_at(&data, 45)?;
                    } else {
                        return Err("truncated regular btrfs extent".into());
                    }
                    record.extents.push(extent);
                }
                XATTR_ITEM => {
                    let mut pos = 0usize;
                    while pos + 30 <= data.len() {
                        let value_len = u16_at(&data, pos + 25)? as usize;
                        let name_len = u16_at(&data, pos + 27)? as usize;
                        let name_start = pos + 30;
                        let value_start = name_start
                            .checked_add(name_len)
                            .ok_or_else(|| "btrfs xattr size overflow".to_owned())?;
                        let end = value_start
                            .checked_add(value_len)
                            .ok_or_else(|| "btrfs xattr size overflow".to_owned())?;
                        let name = String::from_utf8_lossy(bytes(&data, name_start, name_len)?)
                            .into_owned();
                        record
                            .xattrs
                            .insert(name, bytes(&data, value_start, value_len)?.to_vec());
                        pos = end;
                    }
                }
                _ => {}
            }
        }
        for record in inodes.values_mut() {
            record.directories.sort_by_key(|entry| entry.index);
            record.extents.sort_by_key(|extent| extent.offset);
        }
        Ok((inodes, root.dirid))
    }

    fn extent_bytes(
        &mut self,
        extent: &FileExtent,
        notes: &mut BTreeMap<String, u64>,
    ) -> Result<Vec<u8>, String> {
        if extent.encryption != 0 || extent.other_encoding != 0 {
            return Err("encrypted or otherwise-encoded extent: not supported".into());
        }
        if extent.extent_type == 0 {
            if extent.compression == 0 {
                return Ok(extent
                    .inline
                    .iter()
                    .take(extent.ram_bytes as usize)
                    .copied()
                    .collect());
            }
            let ram = usize::try_from(extent.ram_bytes)
                .map_err(|_| "extent exceeds addressable memory".to_owned())?;
            let (decoded, note) = self.decompress(&extent.inline, ram, extent.compression)?;
            add_note(notes, note);
            return Ok(decoded);
        }
        let output_len = usize::try_from(extent.extent_len)
            .map_err(|_| "extent exceeds addressable memory".to_owned())?;
        if extent.extent_type == 2 || extent.disk == 0 {
            return Ok(vec![0; output_len]);
        }
        if extent.compression == 0 {
            let logical = extent
                .disk
                .checked_add(extent.extent_offset)
                .ok_or_else(|| "extent logical address overflow".to_owned())?;
            return self.read_logical(logical, output_len.min(extent.extent_len as usize));
        }
        let decoded = if let Some(cached) = self.extent_cache.get(&extent.disk) {
            cached.clone()
        } else {
            let disk_len = usize::try_from(extent.disk_len)
                .map_err(|_| "compressed extent exceeds addressable memory".to_owned())?;
            let raw = self.read_logical(extent.disk, disk_len)?;
            let ram = usize::try_from(extent.ram_bytes)
                .map_err(|_| "extent exceeds addressable memory".to_owned())?;
            let (decoded, note) = self.decompress(&raw, ram, extent.compression)?;
            add_note(notes, note);
            self.extent_cache.insert(extent.disk, decoded.clone());
            self.extent_order.push_back(extent.disk);
            if self.extent_order.len() > 64 {
                if let Some(oldest) = self.extent_order.pop_front() {
                    self.extent_cache.remove(&oldest);
                }
            }
            decoded
        };
        let start = usize::try_from(extent.extent_offset)
            .map_err(|_| "extent offset exceeds addressable memory".to_owned())?;
        let end = start
            .checked_add(output_len)
            .ok_or_else(|| "extent slice overflow".to_owned())?;
        Ok(bytes(&decoded, start, output_len.min(end - start))?.to_vec())
    }

    fn decompress(
        &self,
        raw: &[u8],
        ram_bytes: usize,
        compression: u8,
    ) -> Result<(Vec<u8>, Option<String>), String> {
        let tail = self.sectorsize as usize;
        match compression {
            1 => zlib_decode(raw, ram_bytes, tail),
            2 => {
                let data = btrfs_lzo(raw, ram_bytes, tail)?;
                Ok(classify(data, ram_bytes, true, tail))
            }
            3 => zstd_decode(raw, ram_bytes, tail),
            other => Err(format!("unknown compression type {other}")),
        }
    }

    fn incompat_names(&self) -> Vec<String> {
        feature_names(
            self.incompat,
            &[
                "MIXED_BACKREF",
                "DEFAULT_SUBVOL",
                "MIXED_GROUPS",
                "COMPRESS_LZO",
                "COMPRESS_ZSTD",
                "BIG_METADATA",
                "EXTENDED_IREF",
                "RAID56",
                "SKINNY_METADATA",
                "NO_HOLES",
                "METADATA_UUID",
                "RAID1C34",
                "ZONED",
                "EXTENT_TREE_V2",
                "RAID_STRIPE_TREE",
                "bit15",
                "SIMPLE_QUOTA",
                "REMAP_TREE",
            ],
        )
    }

    fn compat_ro_names(&self) -> Vec<String> {
        feature_names(
            self.compat_ro,
            &[
                "FREE_SPACE_TREE",
                "FREE_SPACE_TREE_VALID",
                "VERITY",
                "BLOCK_GROUP_TREE",
            ],
        )
    }

    fn profiles(&self) -> BTreeMap<(String, String), u64> {
        let mut result = BTreeMap::new();
        for chunk in &self.chunks {
            let kind = if chunk.kind & 1 != 0 {
                "data"
            } else if chunk.kind & 2 != 0 {
                "system"
            } else {
                "metadata"
            };
            let profile = [
                (1 << 3, "RAID0"),
                (1 << 6, "RAID10"),
                (1 << 7, "RAID5"),
                (1 << 8, "RAID6"),
                (1 << 4, "RAID1"),
                (1 << 5, "DUP"),
            ]
            .into_iter()
            .find_map(|(bit, name)| (chunk.kind & bit != 0).then_some(name))
            .unwrap_or("single");
            *result
                .entry((kind.to_owned(), profile.to_owned()))
                .or_insert(0) += chunk.length;
        }
        result
    }
}

fn feature_names(value: u64, names: &[&str]) -> Vec<String> {
    let mut result = names
        .iter()
        .enumerate()
        .filter_map(|(bit, name)| (value & (1u64 << bit) != 0).then(|| (*name).to_owned()))
        .collect::<Vec<_>>();
    let rest = value & !((1u64 << names.len()) - 1);
    if rest != 0 {
        result.push(format!("unknown:{rest:#x}"));
    }
    result
}

fn add_note(notes: &mut BTreeMap<String, u64>, note: Option<String>) {
    if let Some(note) = note {
        let name = note
            .split(' ')
            .next()
            .filter(|name| !name.is_empty())
            .unwrap_or(&note)
            .to_owned();
        *notes.entry(name).or_insert(0) += 1;
    }
}

fn classify(
    mut output: Vec<u8>,
    expected: usize,
    finished: bool,
    tail: usize,
) -> (Vec<u8>, Option<String>) {
    if output.len() > expected {
        let extra = output.len() - expected;
        output.truncate(expected);
        return (output, Some(format!("frame-larger-than-ram (+{extra})")));
    }
    if output.len() < expected {
        let missing = expected - output.len();
        output.resize(expected, 0);
        if finished && missing < tail {
            return (output, None);
        }
        return (
            output,
            Some(if finished {
                format!("short-output (-{missing})")
            } else {
                "frame-incomplete-input".into()
            }),
        );
    }
    (output, None)
}

fn lzo_byte(input: &[u8], pos: &mut usize) -> Result<u8, String> {
    let value = *input
        .get(*pos)
        .ok_or_else(|| "lzo: truncated input".to_owned())?;
    *pos += 1;
    Ok(value)
}

fn lzo_u16(input: &[u8], pos: usize) -> Result<u16, String> {
    Ok(u16::from_le_bytes(
        bytes(input, pos, 2)?.try_into().expect("fixed slice"),
    ))
}

fn lzo_length(input: &[u8], pos: &mut usize, base: usize, mask: u8) -> Result<usize, String> {
    let mut length = usize::from(lzo_byte(input, pos)? & mask);
    if length == 0 {
        while input.get(*pos) == Some(&0) {
            length = length
                .checked_add(255)
                .ok_or_else(|| "lzo length overflow".to_owned())?;
            *pos += 1;
        }
        length = length
            .checked_add(usize::from(mask))
            .and_then(|value| value.checked_add(usize::from(lzo_byte(input, pos).ok()?)))
            .ok_or_else(|| "lzo length overflow".to_owned())?;
    }
    length
        .checked_add(base)
        .ok_or_else(|| "lzo length overflow".to_owned())
}

fn lzo_copy(output: &mut Vec<u8>, distance: usize, length: usize) -> Result<(), String> {
    let start = output
        .len()
        .checked_sub(distance)
        .ok_or_else(|| "lzo: match before start of output".to_owned())?;
    for index in 0..length {
        let byte = *output
            .get(start + index)
            .ok_or_else(|| "lzo: invalid match distance".to_owned())?;
        output.push(byte);
    }
    Ok(())
}

fn lzo1x_decompress(input: &[u8], output_limit: usize) -> Result<Vec<u8>, String> {
    let mut output = Vec::new();
    let mut pos = 0usize;
    let mut state = 0usize;
    if input.first().copied().unwrap_or(0) > 17 {
        pos += 1;
        let initial = usize::from(input[0] - 17);
        let end = pos
            .checked_add(initial)
            .ok_or_else(|| "lzo literal offset overflow".to_owned())?;
        output.extend_from_slice(bytes(input, pos, initial)?);
        pos = end;
        state = if initial >= 4 { 4 } else { initial };
    }
    while pos < input.len() {
        let token = input[pos];
        if token < 16 {
            if state == 0 {
                let length = if token == 0 {
                    lzo_length(input, &mut pos, 3, 15)?
                } else {
                    pos += 1;
                    usize::from(token) + 3
                };
                let end = pos
                    .checked_add(length)
                    .ok_or_else(|| "lzo literal offset overflow".to_owned())?;
                output.extend_from_slice(bytes(input, pos, length)?);
                pos = end;
                state = 4;
                continue;
            }
            pos += 1;
            let distance;
            let length;
            if state == 4 {
                distance = 1
                    + 0x800
                    + usize::from(token >> 2)
                    + (usize::from(lzo_byte(input, &mut pos)?) << 2);
                length = 3;
            } else {
                distance =
                    1 + usize::from(token >> 2) + (usize::from(lzo_byte(input, &mut pos)?) << 2);
                length = 2;
            }
            lzo_copy(&mut output, distance, length)?;
            state = usize::from(token & 3);
        } else if token >= 64 {
            pos += 1;
            let distance =
                1 + usize::from((token >> 2) & 7) + (usize::from(lzo_byte(input, &mut pos)?) << 3);
            lzo_copy(&mut output, distance, usize::from(token >> 5) + 1)?;
            state = usize::from(token & 3);
        } else if token >= 32 {
            let length = lzo_length(input, &mut pos, 2, 31)?;
            let distance = 1 + usize::from(lzo_u16(input, pos)? >> 2);
            state = usize::from(lzo_byte(input, &mut pos)? & 3);
            pos += 1;
            lzo_copy(&mut output, distance, length)?;
        } else {
            let length = lzo_length(input, &mut pos, 2, 7)?;
            let distance_bits = usize::from(token & 8) << 11;
            let encoded = lzo_u16(input, pos)?;
            pos += 2;
            state = usize::from(encoded & 3);
            let distance = distance_bits + usize::from(encoded >> 2);
            if distance == 0 {
                break;
            }
            lzo_copy(&mut output, distance + 0x4000, length)?;
        }
        if state != 0 {
            let end = pos
                .checked_add(state)
                .ok_or_else(|| "lzo literal offset overflow".to_owned())?;
            output.extend_from_slice(bytes(input, pos, state)?);
            pos = end;
        }
        if output.len() >= output_limit {
            break;
        }
    }
    Ok(output)
}

fn btrfs_lzo(input: &[u8], expected: usize, sectorsize: usize) -> Result<Vec<u8>, String> {
    let total = u32_at(input, 0)? as usize;
    let mut pos = 4usize;
    let mut output = Vec::new();
    while pos < total.min(input.len()) && output.len() < expected {
        let left = sectorsize.saturating_sub(pos % sectorsize);
        if left < 4 {
            pos = pos
                .checked_add(left)
                .ok_or_else(|| "lzo segment offset overflow".to_owned())?;
            continue;
        }
        let segment_len = u32_at(input, pos)? as usize;
        pos += 4;
        let segment = bytes(input, pos, segment_len)?;
        output.extend(lzo1x_decompress(segment, 4 << 20)?);
        pos = pos
            .checked_add(segment_len)
            .ok_or_else(|| "lzo segment offset overflow".to_owned())?;
    }
    Ok(output)
}

#[repr(C)]
struct ZStream {
    next_in: *mut u8,
    avail_in: u32,
    total_in: std::os::raw::c_ulong,
    next_out: *mut u8,
    avail_out: u32,
    total_out: std::os::raw::c_ulong,
    message: *mut std::os::raw::c_char,
    state: *mut std::os::raw::c_void,
    allocate: Option<
        unsafe extern "C" fn(*mut std::os::raw::c_void, u32, u32) -> *mut std::os::raw::c_void,
    >,
    release: Option<unsafe extern "C" fn(*mut std::os::raw::c_void, *mut std::os::raw::c_void)>,
    opaque: *mut std::os::raw::c_void,
    data_type: std::os::raw::c_int,
    adler: std::os::raw::c_ulong,
    reserved: std::os::raw::c_ulong,
}

impl Default for ZStream {
    fn default() -> Self {
        Self {
            next_in: std::ptr::null_mut(),
            avail_in: 0,
            total_in: 0,
            next_out: std::ptr::null_mut(),
            avail_out: 0,
            total_out: 0,
            message: std::ptr::null_mut(),
            state: std::ptr::null_mut(),
            allocate: None,
            release: None,
            opaque: std::ptr::null_mut(),
            data_type: 0,
            adler: 0,
            reserved: 0,
        }
    }
}

#[link(name = "z")]
unsafe extern "C" {
    fn zlibVersion() -> *const std::os::raw::c_char;
    fn inflateInit_(
        stream: *mut ZStream,
        version: *const std::os::raw::c_char,
        stream_size: std::os::raw::c_int,
    ) -> std::os::raw::c_int;
    fn inflate(stream: *mut ZStream, flush: std::os::raw::c_int) -> std::os::raw::c_int;
    fn inflateEnd(stream: *mut ZStream) -> std::os::raw::c_int;
}

fn zlib_decode(
    input: &[u8],
    expected: usize,
    tail: usize,
) -> Result<(Vec<u8>, Option<String>), String> {
    if input.len() > u32::MAX as usize || expected >= u32::MAX as usize {
        return Err("zlib extent exceeds decoder limits".into());
    }
    let capacity = expected.saturating_add(1).max(1);
    let mut output = vec![0; capacity];
    let mut stream = ZStream::default();
    stream.next_in = input.as_ptr() as *mut u8;
    stream.avail_in = input.len() as u32;
    stream.next_out = output.as_mut_ptr();
    stream.avail_out = capacity as u32;
    // zlib owns only its internal state; input and output buffers remain live
    // until inflateEnd below.
    let init = unsafe {
        inflateInit_(
            &mut stream,
            zlibVersion(),
            std::mem::size_of::<ZStream>() as std::os::raw::c_int,
        )
    };
    if init != 0 {
        return Err(format!("zlib initialization failed ({init})"));
    }
    let mut finished = false;
    let mut failure = None;
    loop {
        let before_in = stream.total_in;
        let before_out = stream.total_out;
        let status = unsafe { inflate(&mut stream, 0) };
        if status == 1 {
            finished = true;
            break;
        }
        if status != 0 {
            if status != -5 {
                failure = Some(format!("zlib-error ({status})"));
            }
            break;
        }
        if stream.avail_out == 0 || (before_in == stream.total_in && before_out == stream.total_out)
        {
            break;
        }
    }
    let output_len = (stream.total_out as usize).min(output.len());
    unsafe {
        inflateEnd(&mut stream);
    }
    output.truncate(output_len);
    if let Some(note) = failure {
        return Ok((vec![0; expected], Some(note)));
    }
    Ok(classify(output, expected, finished, tail))
}

fn zstd_decode(
    input: &[u8],
    expected: usize,
    tail: usize,
) -> Result<(Vec<u8>, Option<String>), String> {
    let mut child = Command::new("zstd")
        .args(["-dcq", "--no-check"])
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::null())
        .spawn()
        .map_err(|error| format!("zstd extents need Homebrew zstd ({error})"))?;
    child
        .stdin
        .take()
        .ok_or_else(|| "could not open zstd input".to_owned())?
        .write_all(input)
        .map_err(|error| format!("write compressed data to zstd: {error}"))?;
    let result = child
        .wait_with_output()
        .map_err(|error| format!("wait for zstd: {error}"))?;
    Ok(classify(
        result.stdout,
        expected,
        result.status.success(),
        tail,
    ))
}

fn sha256(path: &Path) -> Result<String, String> {
    let result = Command::new("shasum")
        .args(["-a", "256"])
        .arg(path)
        .output()
        .map_err(|error| format!("run shasum: {error}"))?;
    if !result.status.success() {
        return Err("shasum failed".into());
    }
    String::from_utf8_lossy(&result.stdout)
        .split_whitespace()
        .next()
        .map(str::to_owned)
        .ok_or_else(|| "shasum returned no digest".into())
}

fn info(path: &Path, hash: bool) -> Result<(), String> {
    let image = Image::open(path)?;
    let metadata =
        fs::metadata(path).map_err(|error| format!("stat {}: {error}", path.display()))?;
    println!("image     {}", path.display());
    println!("size      {} bytes ({})", image.size, human(image.size));
    println!(
        "writable  {}",
        if metadata.permissions().mode() & 0o222 != 0 {
            "yes -- chmod a-w recommended"
        } else {
            "no"
        }
    );
    if hash {
        println!("sha256    {}", sha256(path)?);
    }
    let parts = partitions(&image)?;
    println!("\npartitions");
    for part in &parts {
        let type_name = if guid_type(&part.type_guid).is_empty() {
            &part.type_guid
        } else {
            guid_type(&part.type_guid)
        };
        println!(
            "  {:>2}  {:<20} {:<9} {:>10}  at {:#x}  {}{}",
            part.index,
            part.name,
            part.fs,
            human(part.size),
            part.offset,
            type_name,
            if part.truncated { "  TRUNCATED" } else { "" }
        );
    }
    for part in parts.iter().filter(|part| part.fs == "btrfs") {
        let mut fs = Btrfs::open(&image, part.offset)?;
        println!("\nbtrfs on partition {} ({})", part.index, part.name);
        println!(
            "  label {:?}  generation {}  devices {}",
            fs.label, fs.generation, fs.num_devices
        );
        println!(
            "  sectorsize {}  nodesize {}  csum {}",
            fs.sectorsize,
            fs.nodesize,
            match fs.checksum_type {
                0 => "crc32c",
                1 => "xxhash64",
                2 => "sha256",
                3 => "blake2b",
                _ => "?",
            }
        );
        println!(
            "  size {}  used {}",
            human(fs.total_bytes),
            human(fs.bytes_used)
        );
        println!("  incompat  {}", fs.incompat_names().join(" "));
        let compat_ro = fs.compat_ro_names();
        println!(
            "  compat_ro {}",
            if compat_ro.is_empty() {
                "-".into()
            } else {
                compat_ro.join(" ")
            }
        );
        let profiles = fs
            .profiles()
            .into_iter()
            .map(|((kind, profile), size)| format!("{kind} {profile} {}", human(size)))
            .collect::<Vec<_>>();
        println!("  block groups {}", profiles.join(", "));
        if fs.num_devices != 1 {
            println!("  warning: multi-device filesystem; only this device can be read");
        }
        if fs.incompat & (1 << 13) != 0 {
            println!("  warning: extent-tree-v2 is not supported by this reader");
        }
        println!("  subvolumes");
        for (id, subvol_path, readonly, default) in fs.subvolumes()? {
            println!(
                "    {:>6}  {}{}{}",
                id,
                subvol_path,
                if readonly { "  (read-only)" } else { "" },
                if default { "  (default)" } else { "" }
            );
        }
    }
    Ok(())
}

fn usage() -> &'static str {
    "Usage: steamarm-frame-image info IMAGE [--hash]\n\
     Usage: steamarm-frame-image diagnose IMAGE [--part N] [--subvol-id N] [--examples N] [--json PATH]\n\
     Usage: steamarm-frame-image extract IMAGE OUT [--part N] [--subvol-id N] [--no-subvols] [--keep-absolute-symlinks] [--resume]\n\
     Usage: steamarm-frame-image inventory ROOT [--json PATH] [--md PATH]\n\
     Usage: steamarm-frame-image compare INVENTORY_OR_ROOT REPO.db... [--md PATH] [--json PATH] [--all] [--states LIST]\n"
}

fn run() -> Result<i32, String> {
    let mut args = env::args_os().skip(1);
    let Some(command) = args.next() else {
        return Err(usage().into());
    };
    match command.to_string_lossy().as_ref() {
        "info" => {
            let mut hash = false;
            let mut image = None;
            for arg in args {
                if arg == "--hash" {
                    hash = true;
                } else if image.is_none() {
                    image = Some(arg);
                } else {
                    return Err(usage().into());
                }
            }
            let image = image.ok_or_else(|| usage().to_owned())?;
            info(Path::new(&image), hash).map(|()| 0)
        }
        "diagnose" => diagnostics::run(args.collect()),
        "extract" => extractor::run(args.collect()).map(|()| 0),
        "inventory" => inventory::run(args),
        "compare" => compare::run(args),
        _ => Err(usage().into()),
    }
}

fn main() {
    let code = match run() {
        Ok(code) => code,
        Err(error) => {
            eprintln!("error: {error}");
            1
        }
    };
    std::process::exit(code);
}
