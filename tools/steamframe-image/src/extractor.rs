use super::{human, partitions, Btrfs, FileRecord, Image, Partition, ROOT_ITEM};
use serde::Serialize;
use std::collections::{BTreeMap, HashMap};
use std::env;
use std::ffi::{CString, OsStr, OsString};
use std::fs::{self, OpenOptions, Permissions};
use std::io::{Seek, SeekFrom, Write};
use std::os::unix::ffi::{OsStrExt, OsStringExt};
use std::os::unix::fs::PermissionsExt;
use std::path::{Component, Path, PathBuf};

#[derive(Default, Serialize)]
struct Manifest {
    case_collisions: Vec<[String; 2]>,
    errors: Vec<String>,
    owners: BTreeMap<String, [u32; 2]>,
    special: BTreeMap<String, SpecialEntry>,
    subvolumes: Vec<SubvolumeEntry>,
    symlinks_rewritten: BTreeMap<String, String>,
    xattrs: BTreeMap<String, BTreeMap<String, String>>,
    #[serde(skip_serializing_if = "map_is_empty")]
    modes: BTreeMap<String, String>,
}

fn map_is_empty<T>(map: &BTreeMap<String, T>) -> bool {
    map.is_empty()
}

#[derive(Serialize)]
struct SpecialEntry {
    kind: &'static str,
    mode: String,
    rdev: u64,
    major: u64,
    minor: u64,
}

#[derive(Serialize)]
struct SubvolumeEntry {
    id: u64,
    path: String,
}

#[derive(Default)]
struct Counts {
    files: u64,
    dirs: u64,
    symlinks: u64,
    hardlinks: u64,
    special: u64,
    skipped: u64,
    bytes: u64,
    errors: u64,
}

struct Extractor<'a, 'image> {
    fs: &'a mut Btrfs<'image>,
    out: PathBuf,
    no_subvols: bool,
    keep_absolute_symlinks: bool,
    resume: bool,
    case_insensitive: bool,
    lower_paths: BTreeMap<String, PathBuf>,
    hardlinks: HashMap<(u64, u64), PathBuf>,
    manifest: Manifest,
    counts: Counts,
    notes: BTreeMap<String, u64>,
}

impl<'a, 'image> Extractor<'a, 'image> {
    fn new(
        fs: &'a mut Btrfs<'image>,
        out: PathBuf,
        no_subvols: bool,
        keep_absolute_symlinks: bool,
        resume: bool,
    ) -> Self {
        Self {
            fs,
            out,
            no_subvols,
            keep_absolute_symlinks,
            resume,
            case_insensitive: false,
            lower_paths: BTreeMap::new(),
            hardlinks: HashMap::new(),
            manifest: Manifest::default(),
            counts: Counts::default(),
            notes: BTreeMap::new(),
        }
    }

    fn run(&mut self, subvol: u64) -> Result<PathBuf, String> {
        fs::create_dir_all(&self.out)
            .map_err(|error| format!("create {}: {error}", self.out.display()))?;
        self.case_insensitive = self.probe_case()?;
        if self.case_insensitive {
            eprintln!(
                "warning: output is on a case-insensitive volume; Linux names differing only in case are recorded and the later entry is skipped. Use case-sensitive APFS to preserve them all."
            );
        }
        self.extract_tree(subvol, &self.out.clone(), 0)?;
        let manifest_path = self.out.join(".steamarm-manifest.json");
        let json = serde_json::to_vec_pretty(&self.manifest)
            .map_err(|error| format!("serialize extraction manifest: {error}"))?;
        fs::write(&manifest_path, json)
            .map_err(|error| format!("write {}: {error}", manifest_path.display()))?;
        Ok(manifest_path)
    }

    fn probe_case(&self) -> Result<bool, String> {
        let probe_name = format!(".steamarm-CaseProbe-{}", std::process::id());
        let probe = self.out.join(&probe_name);
        let lower = self.out.join(probe_name.to_ascii_lowercase());
        let file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&probe)
            .map_err(|error| {
                format!("create case-sensitivity probe {}: {error}", probe.display())
            })?;
        drop(file);
        let insensitive = lower.exists();
        fs::remove_file(&probe).map_err(|error| {
            format!("remove case-sensitivity probe {}: {error}", probe.display())
        })?;
        Ok(insensitive)
    }

    fn rel(&self, path: &Path) -> String {
        match path.strip_prefix(&self.out) {
            Ok(relative) if relative.as_os_str().is_empty() => ".".to_owned(),
            Ok(relative) => relative.to_string_lossy().into_owned(),
            Err(_) => path.to_string_lossy().into_owned(),
        }
    }

    fn check_case(&mut self, path: &Path) -> bool {
        let key = path.to_string_lossy().to_lowercase();
        if let Some(other) = self.lower_paths.get(&key) {
            if other != path {
                self.manifest
                    .case_collisions
                    .push([self.rel(other), self.rel(path)]);
                return false;
            }
        }
        self.lower_paths.insert(key, path.to_path_buf());
        true
    }

    fn extract_tree(&mut self, subvol: u64, dest: &Path, depth: usize) -> Result<(), String> {
        if depth > 64 {
            return Err("nested btrfs subvolume depth exceeds 64".into());
        }
        let (inodes, top) = self.fs.load_tree(subvol)?;
        self.manifest.subvolumes.push(SubvolumeEntry {
            id: subvol,
            path: self.rel(dest),
        });
        self.extract_dir(subvol, &inodes, top, dest, depth)
    }

    fn extract_dir(
        &mut self,
        subvol: u64,
        inodes: &BTreeMap<u64, FileRecord>,
        ino: u64,
        dest: &Path,
        depth: usize,
    ) -> Result<(), String> {
        fs::create_dir_all(dest).map_err(|error| format!("create {}: {error}", dest.display()))?;
        let Some(record) = inodes.get(&ino) else {
            return Ok(());
        };
        for entry in &record.directories {
            if entry.name == b"." || entry.name == b".." || entry.name.contains(&b'/') {
                continue;
            }
            let path = dest.join(OsString::from_vec(entry.name.clone()));
            if self.case_insensitive && !self.check_case(&path) {
                continue;
            }
            if entry.child_type == ROOT_ITEM {
                if self.no_subvols {
                    fs::create_dir_all(&path)
                        .map_err(|error| format!("create {}: {error}", path.display()))?;
                } else {
                    self.extract_tree(entry.child, &path, depth + 1)?;
                }
                continue;
            }
            let Some(child) = inodes.get(&entry.child) else {
                self.record_error(format!("{}: no inode {}", self.rel(&path), entry.child));
                continue;
            };
            if child.inode.is_none() {
                self.record_error(format!("{}: no inode {}", self.rel(&path), entry.child));
                continue;
            }
            if let Err(error) = self.extract_one(subvol, inodes, entry.child, child, &path, depth) {
                self.record_error(format!("{}: {error}", self.rel(&path)));
            }
        }
        self.finish(record, dest, true);
        Ok(())
    }

    fn extract_one(
        &mut self,
        subvol: u64,
        inodes: &BTreeMap<u64, FileRecord>,
        ino: u64,
        record: &FileRecord,
        path: &Path,
        depth: usize,
    ) -> Result<(), String> {
        let inode = record
            .inode
            .as_ref()
            .ok_or_else(|| format!("no inode {ino}"))?;
        let mode = inode.mode;
        let file_type = mode & 0o170000;
        let relative = self.rel(path);
        match file_type {
            0o040000 => {
                self.extract_dir(subvol, inodes, ino, path, depth)?;
                self.counts.dirs += 1;
            }
            0o120000 => {
                let mut target = Vec::new();
                for extent in &record.extents {
                    target.extend(self.fs.extent_bytes(extent, &mut self.notes)?);
                }
                target.truncate(
                    usize::try_from(inode.size)
                        .map_err(|_| "symlink target exceeds addressable memory".to_owned())?,
                );
                let mut link = OsString::from_vec(target.clone());
                if target.first() == Some(&b'/') && !self.keep_absolute_symlinks {
                    let start = target
                        .iter()
                        .position(|byte| *byte != b'/')
                        .unwrap_or(target.len());
                    let absolute_target =
                        self.out.join(OsString::from_vec(target[start..].to_vec()));
                    let parent = path.parent().unwrap_or(&self.out);
                    link = lexical_relative(&absolute_target, parent);
                    self.manifest.symlinks_rewritten.insert(
                        relative.clone(),
                        String::from_utf8_lossy(&target).into_owned(),
                    );
                }
                remove_existing(path)?;
                std::os::unix::fs::symlink(&link, path)
                    .map_err(|error| format!("create symlink: {error}"))?;
                self.record_owner(&relative, record);
                self.counts.symlinks += 1;
            }
            0o100000 => {
                let key = (subvol, ino);
                if inode.links > 1 {
                    if let Some(existing) = self.hardlinks.get(&key) {
                        remove_existing(path)?;
                        fs::hard_link(existing, path).map_err(|error| {
                            format!("create hardlink to {}: {error}", existing.display())
                        })?;
                        self.counts.hardlinks += 1;
                        return Ok(());
                    }
                }
                let size = inode.size;
                if self.can_resume(path, size, inode.mtime) {
                    self.counts.skipped += 1;
                } else {
                    self.write_file(path, record, size)?;
                }
                self.hardlinks.insert(key, path.to_path_buf());
                self.finish(record, path, false);
                self.counts.files += 1;
                self.counts.bytes = self.counts.bytes.saturating_add(size);
            }
            _ => {
                let kind = match file_type {
                    0o020000 => "chrdev",
                    0o060000 => "blkdev",
                    0o010000 => "fifo",
                    0o140000 => "socket",
                    _ => "unknown",
                };
                let device = inode.rdev;
                self.manifest.special.insert(
                    relative,
                    SpecialEntry {
                        kind,
                        mode: format!("{mode:#o}"),
                        rdev: device,
                        major: device >> 20,
                        minor: device & 0xFFFFF,
                    },
                );
                self.counts.special += 1;
            }
        }
        Ok(())
    }

    fn can_resume(&self, path: &Path, size: u64, mtime: f64) -> bool {
        if !self.resume {
            return false;
        }
        let Ok(metadata) = fs::symlink_metadata(path) else {
            return false;
        };
        if metadata.file_type().is_symlink() || !metadata.is_file() || metadata.len() != size {
            return false;
        }
        let existing = metadata
            .modified()
            .ok()
            .and_then(|time| time.duration_since(std::time::UNIX_EPOCH).ok())
            .map(|duration| duration.as_secs_f64());
        existing.is_some_and(|time| (time - mtime).abs() < 1.0)
    }

    fn write_file(&mut self, path: &Path, record: &FileRecord, size: u64) -> Result<(), String> {
        let mut temporary_name = path
            .file_name()
            .unwrap_or_else(|| OsStr::new("file"))
            .to_os_string();
        temporary_name.push(".steamarm-part");
        let temporary = path.with_file_name(temporary_name);
        let mut file = OpenOptions::new()
            .write(true)
            .create(true)
            .truncate(true)
            .open(&temporary)
            .map_err(|error| format!("open temporary file {}: {error}", temporary.display()))?;
        for extent in &record.extents {
            if extent.offset >= size {
                break;
            }
            if extent.extent_type != 0 && (extent.extent_type == 2 || extent.disk == 0) {
                continue;
            }
            let mut data = self.fs.extent_bytes(extent, &mut self.notes)?;
            let remaining = usize::try_from(size - extent.offset).unwrap_or(usize::MAX);
            data.truncate(data.len().min(remaining));
            file.seek(SeekFrom::Start(extent.offset))
                .map_err(|error| format!("seek {}: {error}", temporary.display()))?;
            file.write_all(&data)
                .map_err(|error| format!("write {}: {error}", temporary.display()))?;
        }
        file.set_len(size)
            .map_err(|error| format!("set length {}: {error}", temporary.display()))?;
        drop(file);
        fs::rename(&temporary, path).map_err(|error| format!("replace {}: {error}", path.display()))
    }

    fn record_owner(&mut self, relative: &str, record: &FileRecord) {
        if let Some(inode) = &record.inode {
            if inode.uid != 0 || inode.gid != 0 {
                self.manifest
                    .owners
                    .insert(relative.to_owned(), [inode.uid, inode.gid]);
            }
        }
        if !record.xattrs.is_empty() {
            self.manifest.xattrs.insert(
                relative.to_owned(),
                record
                    .xattrs
                    .iter()
                    .map(|(name, value)| (name.clone(), hex(value)))
                    .collect(),
            );
        }
    }

    fn finish(&mut self, record: &FileRecord, path: &Path, is_dir: bool) {
        let relative = self.rel(path);
        self.record_owner(&relative, record);
        let Some(inode) = &record.inode else {
            return;
        };
        let permissions = inode.mode & 0o7777;
        let adjusted = permissions | if is_dir { 0o700 } else { 0o200 };
        let _ = fs::set_permissions(path, Permissions::from_mode(adjusted));
        if permissions & 0o7000 != 0 || permissions & 0o200 == 0 {
            self.manifest
                .modes
                .insert(relative, format!("{permissions:#o}"));
        }
        set_times_nofollow(path, inode.atime, inode.mtime);
    }

    fn record_error(&mut self, error: String) {
        self.manifest.errors.push(error);
        self.counts.errors += 1;
    }
}

fn remove_existing(path: &Path) -> Result<(), String> {
    if fs::symlink_metadata(path).is_ok() {
        fs::remove_file(path)
            .map_err(|error| format!("remove existing {}: {error}", path.display()))?;
    }
    Ok(())
}

fn hex(bytes: &[u8]) -> String {
    const HEX: &[u8; 16] = b"0123456789abcdef";
    let mut output = String::with_capacity(bytes.len() * 2);
    for byte in bytes {
        output.push(HEX[(byte >> 4) as usize] as char);
        output.push(HEX[(byte & 0x0f) as usize] as char);
    }
    output
}

fn lexical_relative(path: &Path, base: &Path) -> OsString {
    let path_parts = normal_parts(path);
    let base_parts = normal_parts(base);
    let common = path_parts
        .iter()
        .zip(base_parts.iter())
        .take_while(|(left, right)| left == right)
        .count();
    let mut relative = PathBuf::new();
    for _ in common..base_parts.len() {
        relative.push("..");
    }
    for part in &path_parts[common..] {
        relative.push(part);
    }
    if relative.as_os_str().is_empty() {
        relative.push(".");
    }
    relative.into_os_string()
}

fn normal_parts(path: &Path) -> Vec<OsString> {
    let mut parts = Vec::new();
    for component in path.components() {
        match component {
            Component::RootDir | Component::Prefix(_) | Component::CurDir => {}
            Component::ParentDir => {
                if parts.last().is_some_and(|part| part != OsStr::new("..")) {
                    parts.pop();
                } else {
                    parts.push(OsString::from(".."));
                }
            }
            Component::Normal(part) => parts.push(part.to_os_string()),
        }
    }
    parts
}

#[repr(C)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: i64,
}

unsafe extern "C" {
    fn utimensat(
        dirfd: i32,
        pathname: *const std::ffi::c_char,
        times: *const Timespec,
        flags: i32,
    ) -> i32;
}

#[cfg(target_os = "macos")]
const AT_FDCWD: i32 = -2;
#[cfg(not(target_os = "macos"))]
const AT_FDCWD: i32 = -100;
const AT_SYMLINK_NOFOLLOW: i32 = 0x20;

fn timespec(seconds: f64) -> Timespec {
    let mut sec = seconds.floor();
    let mut nsec = ((seconds - sec) * 1_000_000_000.0).round();
    if nsec >= 1_000_000_000.0 {
        sec += 1.0;
        nsec -= 1_000_000_000.0;
    }
    Timespec {
        tv_sec: sec as i64,
        tv_nsec: nsec as i64,
    }
}

fn set_times_nofollow(path: &Path, atime: f64, mtime: f64) {
    let Ok(path) = CString::new(path.as_os_str().as_bytes()) else {
        return;
    };
    let times = [timespec(atime), timespec(mtime)];
    unsafe {
        let _ = utimensat(AT_FDCWD, path.as_ptr(), times.as_ptr(), AT_SYMLINK_NOFOLLOW);
    }
}

fn absolute_path(path: &Path) -> Result<PathBuf, String> {
    let joined = if path.is_absolute() {
        path.to_path_buf()
    } else {
        env::current_dir()
            .map_err(|error| format!("current directory: {error}"))?
            .join(path)
    };
    let mut normalized = PathBuf::new();
    for component in joined.components() {
        match component {
            Component::RootDir => normalized.push("/"),
            Component::Prefix(prefix) => normalized.push(prefix.as_os_str()),
            Component::CurDir => {}
            Component::ParentDir => {
                normalized.pop();
            }
            Component::Normal(part) => normalized.push(part),
        }
    }
    Ok(normalized)
}

pub(super) fn select_partition<'a>(
    parts: &'a [Partition],
    requested: Option<u32>,
) -> Result<&'a Partition, String> {
    if let Some(index) = requested {
        let partition = parts
            .iter()
            .find(|partition| partition.index == index)
            .ok_or_else(|| format!("no partition {index}"))?;
        if partition.fs != "btrfs" {
            return Err(format!("partition {index} is {}, not btrfs", partition.fs));
        }
        return Ok(partition);
    }
    let mut candidates = parts
        .iter()
        .filter(|partition| partition.fs == "btrfs")
        .collect::<Vec<_>>();
    if candidates.is_empty() {
        return Err("no btrfs partition in this image".into());
    }
    if candidates.len() > 1 {
        let rootish = candidates
            .iter()
            .copied()
            .filter(|partition| partition.name.to_lowercase().contains("root"))
            .collect::<Vec<_>>();
        if !rootish.is_empty() {
            candidates = rootish;
        }
        candidates.sort_by_key(|partition| std::cmp::Reverse(partition.size));
        eprintln!(
            "note: multiple btrfs partitions; using {} ({}); choose another with --part",
            candidates[0].index, candidates[0].name
        );
    }
    Ok(candidates[0])
}

fn extract(
    image_path: &Path,
    out_path: &Path,
    part: Option<u32>,
    subvol_id: Option<u64>,
    no_subvols: bool,
    keep_absolute_symlinks: bool,
    resume: bool,
) -> Result<(), String> {
    let image = Image::open(image_path)?;
    let parts = partitions(&image)?;
    let partition = select_partition(&parts, part)?;
    if partition.truncated {
        eprintln!(
            "warning: partition {} ends past the image; the image may be truncated",
            partition.index
        );
    }
    let out = absolute_path(out_path)?;
    let mut fs = Btrfs::open(&image, partition.offset)?;
    fs.load_roots()?;
    let subvol = subvol_id.unwrap_or(fs.default_subvol);
    eprintln!(
        "partition {} ({}), subvolume {} -> {}",
        partition.index,
        partition.name,
        subvol,
        out.display()
    );
    let mut extractor = Extractor::new(&mut fs, out, no_subvols, keep_absolute_symlinks, resume);
    let manifest_path = extractor.run(subvol)?;
    let counts = &extractor.counts;
    println!(
        "  {} files ({}), {} dirs, {} symlinks, {} hardlinks, {} special files (manifest only){}",
        counts.files,
        human(counts.bytes),
        counts.dirs,
        counts.symlinks,
        counts.hardlinks,
        counts.special,
        if counts.skipped > 0 {
            format!(", {} unchanged", counts.skipped)
        } else {
            String::new()
        }
    );
    if !extractor.notes.is_empty() {
        println!(
            "  decompression notes: {}",
            extractor
                .notes
                .iter()
                .map(|(name, count)| format!("{name} {count}"))
                .collect::<Vec<_>>()
                .join(", ")
        );
    }
    if !extractor.manifest.case_collisions.is_empty() {
        println!(
            "  {} names differ only in case and were skipped (manifest)",
            extractor.manifest.case_collisions.len()
        );
    }
    if let Some(error) = extractor.manifest.errors.first() {
        println!(
            "  {} errors; first: {error}",
            extractor.manifest.errors.len()
        );
    }
    println!("  manifest {}", manifest_path.display());
    let bad_notes = extractor
        .notes
        .iter()
        .filter(|(name, _)| name.as_str() != "frame-larger-than-ram")
        .map(|(_, count)| *count)
        .sum::<u64>();
    let bad = bad_notes + extractor.manifest.errors.len() as u64;
    if bad == 0 {
        Ok(())
    } else {
        Err(format!("extraction completed with {bad} reported issue(s)"))
    }
}

fn usage() -> &'static str {
    "Usage: steamarm-frame-image extract IMAGE OUT [--part N] [--subvol-id N] [--no-subvols] [--keep-absolute-symlinks] [--resume]"
}

pub fn run(args: Vec<std::ffi::OsString>) -> Result<(), String> {
    let mut part = None;
    let mut subvol_id = None;
    let mut no_subvols = false;
    let mut keep_absolute_symlinks = false;
    let mut resume = false;
    let mut positionals = Vec::new();
    let mut args = args.into_iter();
    let mut positional_only = false;
    while let Some(arg) = args.next() {
        let text = arg.to_string_lossy().into_owned();
        if positional_only {
            positionals.push(arg);
            continue;
        }
        if text == "--" {
            positional_only = true;
            continue;
        }
        if text == "--help" || text == "-h" {
            println!("{}", usage());
            return Ok(());
        }
        if text == "--no-subvols" {
            no_subvols = true;
        } else if text == "--keep-absolute-symlinks" {
            keep_absolute_symlinks = true;
        } else if text == "--resume" {
            resume = true;
        } else if text == "--part" || text == "--subvol-id" {
            let value = args.next().ok_or_else(|| usage().to_owned())?;
            let value = value
                .to_str()
                .ok_or_else(|| "numeric option is not valid UTF-8".to_owned())?;
            if text == "--part" {
                part = Some(
                    value
                        .parse::<u32>()
                        .map_err(|_| format!("invalid partition index {value:?}"))?,
                );
            } else {
                subvol_id = Some(
                    value
                        .parse::<u64>()
                        .map_err(|_| format!("invalid subvolume id {value:?}"))?,
                );
            }
        } else if let Some(value) = text.strip_prefix("--part=") {
            part = Some(
                value
                    .parse::<u32>()
                    .map_err(|_| format!("invalid partition index {value:?}"))?,
            );
        } else if let Some(value) = text.strip_prefix("--subvol-id=") {
            subvol_id = Some(
                value
                    .parse::<u64>()
                    .map_err(|_| format!("invalid subvolume id {value:?}"))?,
            );
        } else if text.starts_with('-') {
            return Err(format!("unknown option {text:?}\n{}", usage()));
        } else {
            positionals.push(arg);
        }
    }
    if positionals.len() != 2 {
        return Err(usage().into());
    }
    extract(
        Path::new(&positionals[0]),
        Path::new(&positionals[1]),
        part,
        subvol_id,
        no_subvols,
        keep_absolute_symlinks,
        resume,
    )
}
