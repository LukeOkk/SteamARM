//! Inventory extracted Steam Frame roots without mounting the source image.
//! Kept dependency-free beyond the crate's already vendored serde_json.

use serde::Serialize;
use serde_json::{json, Map, Value};
use std::collections::BTreeMap;
use std::env;
use std::fs::{self, File};
use std::io::Read;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const HOST_PAGE: u64 = 0x4000;
const SKIP_TOP: &[&str] = &["proc", "sys", "dev", "run", "tmp"];
const COMPONENTS: &[(&str, &[&str])] = &[
    (
        "steam-client",
        &[
            "usr/bin/steam",
            "usr/lib/steam/",
            "usr/share/steam/",
            "bin_steam.sh",
            "steamos-steam",
        ],
    ),
    ("steamwebhelper/cef", &["steamwebhelper", "libcef.so"]),
    (
        "fex",
        &[
            "FEXInterpreter",
            "FEXServer",
            "libFEXCore",
            "fex-emu",
            "binfmt.d/fex",
            "FEXLoader",
        ],
    ),
    ("proton", &["proton", "compatibilitytools.d"]),
    (
        "steam-linux-runtime",
        &["SteamLinuxRuntime", "steamrt", "pressure-vessel"],
    ),
    ("gamescope", &["gamescope"]),
    ("lepton", &["lepton", "Lepton"]),
    ("vulkan-icd", &["share/vulkan/icd.d/"]),
    (
        "mesa",
        &[
            "libvulkan_freedreno",
            "libvulkan_radeon",
            "dri/",
            "libgallium",
            "libEGL_mesa",
            "libGLX_mesa",
        ],
    ),
    (
        "nss",
        &[
            "libnss3.so",
            "libsoftokn3.so",
            "libfreeblpriv3.so",
            "libnssckbi.so",
            "libnspr4.so",
        ],
    ),
    ("x11", &["libX11.so", "Xwayland", "libxcb.so"]),
    ("audio", &["pipewire", "libpulse", "wireplumber"]),
    (
        "qualcomm/hardware",
        &["qcom", "adreno", "firmware/qcom", "hexagon", "fastrpc"],
    ),
];

fn usage() -> String {
    "Usage: steamarm-frame-image inventory ROOT [--json PATH] [--md PATH]".into()
}

pub fn run(args: impl IntoIterator<Item = std::ffi::OsString>) -> Result<i32, String> {
    let mut args = args.into_iter();
    let root = args.next().ok_or_else(usage)?;
    let mut json_path = None;
    let mut md_path = None;
    while let Some(arg) = args.next() {
        let key = arg.to_string_lossy();
        let value = args.next().ok_or_else(usage)?;
        match key.as_ref() {
            "--json" => json_path = Some(PathBuf::from(value)),
            "--md" => md_path = Some(PathBuf::from(value)),
            _ => return Err(usage()),
        }
    }
    let root = PathBuf::from(root);
    let root = if root.is_absolute() {
        root
    } else {
        env::current_dir().map_err(|e| e.to_string())?.join(root)
    };
    let report = inventory(&root)?;
    if let Some(path) = json_path {
        write_json(&path, &report)?;
    }
    let md = inventory_markdown(&report);
    if let Some(path) = md_path {
        fs::write(&path, md).map_err(|e| format!("write {}: {e}", path.display()))?;
    } else {
        print!("{md}");
    }
    Ok(0)
}

fn inventory(root: &Path) -> Result<Value, String> {
    if !root.is_dir() {
        return Err(format!("{}: root is not a directory", root.display()));
    }
    let mut report = Map::new();
    report.insert("root".into(), json!(root.to_string_lossy()));
    let mut os_release = read_kv(&root.join("etc/os-release"));
    if os_release.is_empty() {
        os_release = read_kv(&root.join("usr/lib/os-release"));
    }
    report.insert("os_release".into(), json!(os_release));
    let (db, packages) = pacman_packages(root);
    report.insert("pacman_db".into(), db.map_or(Value::Null, |x| json!(x)));
    report.insert("packages".into(), json!(packages));

    let mut files = 0u64;
    let mut elf_by_machine: BTreeMap<String, u64> = BTreeMap::new();
    let mut interpreters: BTreeMap<(String, String), u64> = BTreeMap::new();
    let mut non_aarch64 = Vec::new();
    let mut components: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let mut key_libraries: BTreeMap<String, Vec<String>> = BTreeMap::new();
    let mut align: BTreeMap<u64, u64> = BTreeMap::new();
    let mut exec_a64 = Vec::new();
    let mut below_page = Vec::new();
    walk(root, root, true, &mut |path, rel| {
        files += 1;
        let rels = rel.to_string_lossy().replace('\\', "/");
        for (name, patterns) in COMPONENTS {
            if patterns.iter().any(|pattern| rels.contains(pattern)) {
                let found = components.entry((*name).to_owned()).or_default();
                if found.len() < 400 {
                    found.push(rels.clone());
                }
            }
        }
        let metadata = match fs::symlink_metadata(path) {
            Ok(x) => x,
            Err(_) => return,
        };
        if metadata.file_type().is_symlink() || !metadata.is_file() {
            return;
        }
        let Some(info) = elf_info(path) else {
            return;
        };
        *elf_by_machine.entry(info.machine.clone()).or_default() += 1;
        if let Some(interp) = info.interp.as_ref() {
            *interpreters
                .entry((info.machine.clone(), interp.clone()))
                .or_default() += 1;
        }
        if info.machine != "aarch64" {
            non_aarch64.push(json!({"path": rels, "machine": info.machine, "interp": info.interp}));
        } else {
            if let Some(min_align) = info.aligns.iter().min() {
                *align.entry(*min_align).or_default() += 1;
            }
            if info.kind.as_deref() == Some("exec") {
                exec_a64.push(rels.clone());
            } else if let Some(min_align) = info.aligns.iter().min() {
                if min_align % HOST_PAGE != 0 {
                    let name = path.file_name().unwrap_or_default().to_string_lossy();
                    below_page.push(json!({"path": rels, "p_align": min_align,
                        "program": info.interp.is_some() || name.contains("ld-linux")}));
                }
            }
        }
        let name = path
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned();
        if [
            "libc.so.6",
            "libstdc++.so.6.",
            "libnss3.so",
            "libsoftokn3.so",
            "libfreeblpriv3.so",
            "libvulkan.so.1",
        ]
        .iter()
        .any(|prefix| name.starts_with(prefix))
        {
            key_libraries.entry(name).or_default().push(rels);
        }
    })?;
    report.insert("files".into(), json!(files));
    report.insert("elf_by_machine".into(), json!(elf_by_machine));
    let mut interpreter_rows: Vec<_> = interpreters
        .into_iter()
        .map(|((machine, interp), count)| json!({"machine":machine,"interp":interp,"count":count}))
        .collect();
    interpreter_rows.sort_by(|a, b| {
        b["count"]
            .as_u64()
            .cmp(&a["count"].as_u64())
            .then_with(|| a["machine"].as_str().cmp(&b["machine"].as_str()))
    });
    report.insert("interpreters".into(), json!(interpreter_rows));
    report.insert("non_aarch64_elf".into(), json!(non_aarch64));
    let min_align: BTreeMap<String, u64> =
        align.iter().map(|(a, c)| (format!("{a:#x}"), *c)).collect();
    below_page.sort_by(|a, b| {
        b["program"]
            .as_bool()
            .cmp(&a["program"].as_bool())
            .then_with(|| a["path"].as_str().cmp(&b["path"].as_str()))
    });
    exec_a64.sort();
    report.insert(
        "aarch64_loading".into(),
        json!({"min_p_align":min_align,"et_exec":exec_a64,"below_host_page":below_page}),
    );
    report.insert("kernel_pages".into(), json!(kernel_pages(root)));
    for values in components.values_mut() {
        values.sort();
    }
    report.insert("components".into(), json!(components));
    report.insert("key_libraries".into(), json!(key_libraries));
    report.insert("vulkan_icds".into(), json!(vulkan_icds(root)));
    report.insert("binfmt".into(), json!(binfmt(root)));
    report.insert("systemd_units".into(), json!(systemd_units(root)));
    report.insert("pacman_repos".into(), json!(pacman_repos(root)));
    Ok(Value::Object(report))
}

fn walk(
    root: &Path,
    dir: &Path,
    is_root: bool,
    callback: &mut impl FnMut(&Path, &Path),
) -> Result<(), String> {
    let read_dir = match fs::read_dir(dir) {
        Ok(entries) => entries,
        Err(_) if !is_root => return Ok(()),
        Err(error) => return Err(format!("read {}: {error}", dir.display())),
    };
    let mut entries = read_dir.filter_map(Result::ok).collect::<Vec<_>>();
    entries.sort_by_key(|e| e.file_name());
    for ent in entries {
        let path = ent.path();
        let rel = path.strip_prefix(root).unwrap_or(&path);
        if is_root && SKIP_TOP.iter().any(|x| ent.file_name() == *x) {
            continue;
        }
        let file_type = ent.file_type().ok();
        let is_dir = file_type.as_ref().is_some_and(|t| t.is_dir());
        let is_dir_link = file_type.as_ref().is_some_and(|t| t.is_symlink()) && path.is_dir();
        if !is_dir && !is_dir_link {
            callback(&path, rel);
        }
        if is_dir {
            walk(root, &path, false, callback)?;
        }
    }
    Ok(())
}

struct ElfInfo {
    machine: String,
    kind: Option<String>,
    interp: Option<String>,
    aligns: Vec<u64>,
}

fn elf_info(path: &Path) -> Option<ElfInfo> {
    let mut file = File::open(path).ok()?;
    let mut h = [0u8; 64];
    let count = file.read(&mut h).ok()?;
    if count < 52 || &h[..4] != b"\x7fELF" {
        return None;
    }
    let is64 = h[4] == 2;
    if h[5] != 1 {
        return Some(ElfInfo {
            machine: "big-endian".into(),
            kind: None,
            interp: None,
            aligns: vec![],
        });
    }
    let etype = le16(&h, 16)?;
    let machine_id = le16(&h, 18)?;
    let machine = match machine_id {
        3 => "i386",
        40 => "arm",
        62 => "x86-64",
        183 => "aarch64",
        243 => "riscv",
        _ => {
            return Some(ElfInfo {
                machine: machine_id.to_string(),
                kind: None,
                interp: None,
                aligns: vec![],
            })
        }
    }
    .to_owned();
    let (phoff, entsize, phnum) = if is64 {
        (le64(&h, 32)?, le16(&h, 54)? as u64, le16(&h, 56)? as usize)
    } else {
        (
            le32(&h, 28)? as u64,
            le16(&h, 42)? as u64,
            le16(&h, 44)? as usize,
        )
    };
    let mut aligns = Vec::new();
    let mut interp = None;
    if phnum > 0 && phnum < 256 && entsize > 0 && phoff < 1 << 40 {
        let size = (entsize as usize).checked_mul(phnum)?;
        if size < 65536 {
            let mut ph = vec![0u8; size];
            if file.seek(std::io::SeekFrom::Start(phoff)).is_ok()
                && file.read_exact(&mut ph).is_ok()
            {
                for idx in 0..phnum {
                    let o = idx * entsize as usize;
                    if o + if is64 { 56 } else { 32 } > ph.len() {
                        continue;
                    }
                    let kind = le32(&ph, o)?;
                    if kind == 1 {
                        aligns.push(if is64 {
                            le64(&ph, o + 48)?
                        } else {
                            le32(&ph, o + 28)? as u64
                        });
                    }
                    if kind != 3 {
                        continue;
                    }
                    let (off, len) = if is64 {
                        (le64(&ph, o + 8)?, le64(&ph, o + 32)?)
                    } else {
                        (le32(&ph, o + 4)? as u64, le32(&ph, o + 16)? as u64)
                    };
                    if len < 4096 && off < 1 << 40 {
                        let mut bytes = vec![0; len as usize];
                        if file.seek(std::io::SeekFrom::Start(off)).is_ok()
                            && file.read_exact(&mut bytes).is_ok()
                        {
                            while bytes.last() == Some(&0) {
                                bytes.pop();
                            }
                            interp = Some(String::from_utf8_lossy(&bytes).into_owned());
                        }
                    }
                }
            }
        }
    }
    Some(ElfInfo {
        machine,
        kind: match etype {
            2 => Some("exec".into()),
            3 => Some("dyn".into()),
            _ => Some(etype.to_string()),
        },
        interp,
        aligns,
    })
}

use std::io::Seek;
fn le16(b: &[u8], o: usize) -> Option<u16> {
    Some(u16::from_le_bytes(b.get(o..o + 2)?.try_into().ok()?))
}
fn le32(b: &[u8], o: usize) -> Option<u32> {
    Some(u32::from_le_bytes(b.get(o..o + 4)?.try_into().ok()?))
}
fn le64(b: &[u8], o: usize) -> Option<u64> {
    Some(u64::from_le_bytes(b.get(o..o + 8)?.try_into().ok()?))
}

fn parse_desc(text: &str) -> BTreeMap<String, Vec<String>> {
    let mut fields = BTreeMap::new();
    let mut current: Option<String> = None;
    for line in text.lines() {
        if line.starts_with('%') && line.ends_with('%') {
            let key = line.trim_matches('%').to_owned();
            fields.insert(key.clone(), Vec::new());
            current = Some(key);
        } else if !line.is_empty() {
            if let Some(key) = current.as_ref() {
                fields
                    .entry(key.clone())
                    .or_insert_with(Vec::new)
                    .push(line.to_owned());
            }
        }
    }
    fields
}
fn field(fields: &BTreeMap<String, Vec<String>>, key: &str) -> String {
    fields
        .get(key)
        .and_then(|v| v.first())
        .cloned()
        .unwrap_or_default()
}
fn field_list(fields: &BTreeMap<String, Vec<String>>, key: &str) -> Vec<String> {
    fields.get(key).cloned().unwrap_or_default()
}
fn int_field(fields: &BTreeMap<String, Vec<String>>, key: &str) -> u64 {
    field(fields, key).parse().unwrap_or(0)
}
fn pacman_packages(root: &Path) -> (Option<String>, Vec<Value>) {
    for db in [
        "usr/lib/holo/pacmandb/local",
        "var/lib/pacman/local",
        "usr/share/factory/var/lib/pacman/local",
        "usr/lib/pacman/local",
    ] {
        let base = root.join(db);
        if !base.is_dir() {
            continue;
        }
        let Ok(entries) = fs::read_dir(&base) else {
            continue;
        };
        let mut entries = entries.filter_map(Result::ok).collect::<Vec<_>>();
        entries.sort_by_key(|e| e.file_name());
        let mut pkgs = Vec::new();
        for ent in entries {
            let path = ent.path().join("desc");
            let Some(text) = read_lossy(&path) else {
                continue;
            };
            let f = parse_desc(&text);
            pkgs.push(json!({"name":field(&f,"NAME"),"version":field(&f,"VERSION"),"arch":field(&f,"ARCH"),"license":field_list(&f,"LICENSE"),"url":field(&f,"URL"),"packager":field(&f,"PACKAGER"),"size":int_field(&f,"SIZE"),"desc":field(&f,"DESC"),"builddate":int_field(&f,"BUILDDATE"),"provides":field_list(&f,"PROVIDES"),"replaces":field_list(&f,"REPLACES")}));
        }
        return (Some(db.into()), pkgs);
    }
    (None, vec![])
}
pub fn packages_from_root(root: &Path) -> (Option<String>, Vec<Value>) {
    pacman_packages(root)
}
fn read_kv(path: &Path) -> BTreeMap<String, String> {
    let mut out = BTreeMap::new();
    let Some(text) = read_lossy(path) else {
        return out;
    };
    for line in text.lines() {
        if line.trim_start().starts_with('#') {
            continue;
        }
        if let Some((k, v)) = line.split_once('=') {
            out.insert(k.trim().into(), v.trim().trim_matches('"').into());
        }
    }
    out
}
fn read_lossy(path: &Path) -> Option<String> {
    fs::read(path)
        .ok()
        .map(|bytes| String::from_utf8_lossy(&bytes).into_owned())
}
fn module_dirs(root: &Path) -> Vec<PathBuf> {
    let mut out = Vec::new();
    let base = root.join("usr/lib/modules");
    if let Ok(rd) = fs::read_dir(base) {
        for e in rd.flatten() {
            if e.path().is_dir() {
                out.push(e.path());
            }
        }
    }
    out.sort();
    out
}
fn kernel_pages(root: &Path) -> Vec<Value> {
    let mut out = Vec::new();
    let mut configs = Vec::new();
    for dir in module_dirs(root) {
        configs.extend([dir.join("config"), dir.join("build/.config")]);
    }
    if let Ok(entries) = fs::read_dir(root.join("boot")) {
        configs.extend(entries.flatten().filter_map(|entry| {
            entry
                .file_name()
                .to_string_lossy()
                .starts_with("config")
                .then(|| entry.path())
        }));
    }
    configs.sort();
    for p in configs {
        if !p.is_file() {
            continue;
        }
        let cfg = read_kv(&p);
        let page = cfg.iter().find_map(|(k, v)| {
            if v == "y" && k.starts_with("CONFIG_ARM64_") && k.ends_with("_PAGES") {
                let x = k
                    .trim_start_matches("CONFIG_ARM64_")
                    .trim_end_matches("_PAGES");
                ["4K", "16K", "64K"].contains(&x).then(|| x.to_owned())
            } else {
                None
            }
        });
        let mut row = json!({"file":rel(root,&p),"source":"config","page":page});
        if let Some(va) = cfg.get("CONFIG_ARM64_VA_BITS") {
            row["va_bits"] = json!(va);
        }
        out.push(row);
    }
    let mut images = Vec::new();
    for dir in module_dirs(root) {
        for e in fs::read_dir(&dir).into_iter().flatten().flatten() {
            let n = e.file_name().to_string_lossy().to_string();
            if n == "vmlinuz" || n.starts_with("Image") {
                images.push(e.path());
            }
        }
    }
    for dir in [root.join("boot")] {
        if let Ok(rd) = fs::read_dir(dir) {
            for e in rd.flatten() {
                let n = e.file_name().to_string_lossy().to_string();
                if n.starts_with("Image") || n.starts_with("vmlinuz") {
                    images.push(e.path());
                }
            }
        }
    }
    images.sort();
    images.dedup();
    for p in images {
        let mut head = vec![0; 1 << 20];
        let Ok(mut f) = File::open(&p) else { continue };
        let n = f.read(&mut head).unwrap_or(0);
        head.truncate(n);
        let mut kind = "raw";
        if head.starts_with(&[0x1f, 0x8b]) {
            kind = "gzip";
            head = gzip_prefix(&p).unwrap_or_default();
        } else if head.starts_with(b"MZ") && head.get(4..8) == Some(b"zimg") {
            out.push(
                json!({"file":rel(root,&p),"source":"EFI zboot (compressed)","page":Value::Null}),
            );
            continue;
        }
        if head.len() >= 64 && head.get(0x38..0x3c) == Some(b"ARM\x64") {
            let flags = le64(&head, 0x18).unwrap_or(0);
            let page = match (flags >> 1) & 3 {
                1 => "4K",
                2 => "16K",
                3 => "64K",
                _ => "unspecified",
            };
            out.push(
                json!({"file":rel(root,&p),"source":format!("Image header ({kind})"),"page":page}),
            );
        } else {
            out.push(json!({"file":rel(root,&p),"source":format!("not an arm64 Image ({kind})"),"page":Value::Null}));
        }
    }
    out
}
fn gzip_prefix(path: &Path) -> Option<Vec<u8>> {
    let mut child = Command::new("gzip")
        .arg("-dc")
        .arg(path)
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .ok()?;
    let mut out = Vec::new();
    child.stdout.take()?.take(64).read_to_end(&mut out).ok()?;
    let _ = child.wait();
    Some(out)
}
fn rel(root: &Path, p: &Path) -> String {
    p.strip_prefix(root)
        .unwrap_or(p)
        .to_string_lossy()
        .replace('\\', "/")
}
pub fn redact_url(url: &str) -> String {
    let Some((scheme, rest)) = url.split_once("://") else {
        return url.into();
    };
    let authority = rest.split('/').next().unwrap_or("");
    let host = authority
        .rsplit('@')
        .next()
        .unwrap_or("")
        .split(':')
        .next()
        .unwrap_or("")
        .to_lowercase();
    let path = rest.strip_prefix(authority).unwrap_or("");
    let upper = url.to_ascii_uppercase();
    let private = upper.contains("DO_NOT_SHARE")
        || ((host == "steamos.cloud" || host.ends_with(".steamos.cloud"))
            && path
                .split('/')
                .any(|s| s.len() >= 32 && s.chars().all(|c| c.is_ascii_hexdigit())));
    if !private {
        return url.into();
    }
    if host.is_empty()
        || host.contains("DO_NOT_SHARE")
        || host.len() >= 32 && host.chars().all(|c| c.is_ascii_hexdigit())
    {
        return "<private URL redacted>".into();
    }
    format!("{}://{host}/<private path redacted>", scheme)
}
fn pacman_repos(root: &Path) -> Vec<Value> {
    let conf = root.join("etc/pacman.conf");
    let Some(text) = read_lossy(&conf) else {
        return vec![];
    };
    let mut arch = "aarch64".to_owned();
    let (mut repos, mut current): (Vec<Value>, Option<usize>) = (Vec::new(), None);
    for line in text.lines() {
        let line = line.split('#').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        if line.starts_with('[') && line.ends_with(']') {
            let name = &line[1..line.len() - 1];
            current = if name == "options" {
                None
            } else {
                repos.push(json!({"name":name,"servers":[]}));
                Some(repos.len() - 1)
            };
            continue;
        }
        let Some((k, v)) = line.split_once('=') else {
            continue;
        };
        let (k, v) = (k.trim(), v.trim());
        if k == "Architecture" && v != "auto" && !v.is_empty() {
            arch = v.split_whitespace().next().unwrap_or("aarch64").into();
        } else if let Some(idx) = current {
            if k == "Server" {
                let name = repos[idx]["name"].as_str().unwrap_or("").to_owned();
                let u = redact_url(&v.replace("$repo", &name).replace("$arch", &arch));
                repos[idx]["servers"].as_array_mut().unwrap().push(json!(u));
            } else if k == "Include" {
                let path = root.join(v.trim_start_matches('/'));
                if let Some(includes) = read_lossy(&path) {
                    let name = repos[idx]["name"].as_str().unwrap_or("").to_owned();
                    for ln in includes.lines() {
                        let ln = ln.split('#').next().unwrap_or("").trim();
                        if let Some((key, url)) = ln.split_once('=') {
                            if key.trim() == "Server" {
                                let u = redact_url(
                                    &url.trim().replace("$repo", &name).replace("$arch", &arch),
                                );
                                repos[idx]["servers"].as_array_mut().unwrap().push(json!(u));
                            }
                        }
                    }
                }
            }
        }
    }
    repos
}
fn vulkan_icds(root: &Path) -> Vec<Value> {
    let mut out = Vec::new();
    for d in ["usr/share/vulkan/icd.d", "etc/vulkan/icd.d"] {
        let base = root.join(d);
        let Ok(rd) = fs::read_dir(&base) else {
            continue;
        };
        let mut es = rd.filter_map(Result::ok).collect::<Vec<_>>();
        es.sort_by_key(|e| e.file_name());
        for e in es {
            let file = format!("{d}/{}", e.file_name().to_string_lossy());
            let val = fs::read_to_string(e.path())
                .ok()
                .and_then(|s| serde_json::from_str::<Value>(&s).ok());
            if let Some(v) = val {
                out.push(json!({"file":file,"library":v.pointer("/ICD/library_path").cloned().unwrap_or(Value::Null),"api":v.pointer("/ICD/api_version").cloned().unwrap_or(Value::Null)}));
            } else {
                out.push(json!({"file":file,"library":Value::Null,"api":Value::Null}));
            }
        }
    }
    out
}
fn binfmt(root: &Path) -> Vec<Value> {
    let mut out = Vec::new();
    for d in ["usr/lib/binfmt.d", "etc/binfmt.d"] {
        let base = root.join(d);
        let Ok(rd) = fs::read_dir(base) else { continue };
        let mut es = rd.filter_map(Result::ok).collect::<Vec<_>>();
        es.sort_by_key(|e| e.file_name());
        for e in es {
            if let Ok(s) = fs::read_to_string(e.path()) {
                out.push(json!({"file":format!("{d}/{}",e.file_name().to_string_lossy()),"text":s.trim()}));
            }
        }
    }
    out
}
fn systemd_units(root: &Path) -> Vec<String> {
    let mut out = Vec::new();
    for d in [
        "usr/lib/systemd/system",
        "usr/lib/systemd/user",
        "etc/systemd/system",
    ] {
        let base = root.join(d);
        let Ok(rd) = fs::read_dir(base) else { continue };
        for e in rd.flatten() {
            let n = e.file_name().to_string_lossy().to_lowercase();
            if [
                "steam",
                "fex",
                "lepton",
                "gamescope",
                "holo",
                "jupiter",
                "frame",
            ]
            .iter()
            .any(|x| n.contains(x))
            {
                out.push(format!("{d}/{}", e.file_name().to_string_lossy()));
            }
        }
    }
    out
}
fn write_json(path: &Path, value: &Value) -> Result<(), String> {
    let file = File::create(path).map_err(|e| format!("create {}: {e}", path.display()))?;
    let formatter = serde_json::ser::PrettyFormatter::with_indent(b" ");
    let mut ser = serde_json::Serializer::with_formatter(file, formatter);
    value
        .serialize(&mut ser)
        .map_err(|e| format!("write {}: {e}", path.display()))?;
    Ok(())
}

fn inventory_markdown(r: &Value) -> String {
    let mut l = Vec::new();
    let osr = &r["os_release"];
    l.push("## Inventory (generated by steamarm-frame-image inventory)\n".to_owned());
    l.push("| | |\n|---|---|".into());
    for k in [
        "NAME",
        "ID",
        "ID_LIKE",
        "VERSION_ID",
        "BUILD_ID",
        "VARIANT_ID",
        "VERSION_CODENAME",
    ] {
        if let Some(v) = osr[k].as_str() {
            l.push(format!("| {k} | `{v}` |"));
        }
    }
    l.push(format!("| files | {} |", r["files"]));
    l.push(format!(
        "| pacman db | `{}` ({} packages) |",
        display_json(&r["pacman_db"]),
        r["packages"].as_array().map_or(0, Vec::len)
    ));
    l.push(String::new());
    l.push("### ELF files by machine\n".into());
    l.push("| machine | files |\n|---|---|".into());
    let mut machines = r["elf_by_machine"]
        .as_object()
        .map(|m| m.iter().collect::<Vec<_>>())
        .unwrap_or_default();
    machines.sort_by(|a, b| b.1.as_u64().cmp(&a.1.as_u64()));
    for (m, c) in machines {
        l.push(format!("| {m} | {} |", c));
    }
    l.push("\n### Program interpreters\n".into());
    l.push("| machine | PT_INTERP | files |\n|---|---|---|".into());
    if let Some(xs) = r["interpreters"].as_array() {
        for x in xs.iter().take(20) {
            l.push(format!(
                "| {} | `{}` | {} |",
                x["machine"], x["interp"], x["count"]
            ));
        }
    }
    let ld = &r["aarch64_loading"];
    if ld.is_object() {
        let mut aligns = Vec::new();
        if let Some(m) = ld["min_p_align"].as_object() {
            for (a, c) in m {
                aligns.push(format!("{a} {c}"));
            }
        }
        l.push("\n### Loading under lxrun (aarch64 ELF)\n".into());
        l.push(format!(
            "Smallest PT_LOAD p_align per file: {}",
            aligns.join(", ")
        ));
        let exec = ld["et_exec"].as_array().cloned().unwrap_or_default();
        l.push(format!(
            "\n- ET_EXEC (lxrun refuses them, runtime/elf.c): {}",
            exec.len()
        ));
        for p in exec.iter().take(20) {
            l.push(format!("  - `{}`", p.as_str().unwrap_or("")));
        }
        let below = ld["below_host_page"]
            .as_array()
            .cloned()
            .unwrap_or_default();
        let progs = below
            .iter()
            .filter(|x| x["program"].as_bool() == Some(true))
            .collect::<Vec<_>>();
        l.push(format!("- executables (PT_INTERP) and ld.so below the 16 KiB host page (runtime/subpage.c path; refused below 4 KiB): {}",progs.len()));
        for x in progs.iter().take(20) {
            l.push(format!(
                "  - `{}` p_align {:#x}",
                x["path"].as_str().unwrap_or(""),
                x["p_align"].as_u64().unwrap_or(0)
            ));
        }
        l.push(format!("- libraries below the 16 KiB host page (runtime/subpage.c path; glibc needs LXRT_GUEST_PAGE=4096): {}",below.len()-progs.len()));
        let tiny = below
            .iter()
            .filter(|x| x["p_align"].as_u64().unwrap_or(0) % 0x1000 != 0)
            .collect::<Vec<_>>();
        l.push(format!(
            "- below 4 KiB (runtime/elf.c refuses the programs among them): {}",
            tiny.len()
        ));
        for x in r["kernel_pages"].as_array().into_iter().flatten() {
            l.push(format!(
                "- kernel `{}` ({}): page size {}{}",
                x["file"],
                x["source"],
                if x["page"].is_null() {
                    "unknown"
                } else {
                    x["page"].as_str().unwrap_or("unknown")
                },
                x["va_bits"]
                    .as_str()
                    .map(|v| format!(", VA bits {v}"))
                    .unwrap_or_default()
            ));
        }
    }
    l.push("\n### Components found (paths, first 12 each)\n".into());
    if let Some(m) = r["components"].as_object() {
        for (k, v) in m {
            let xs = v.as_array().cloned().unwrap_or_default();
            let paths = xs
                .iter()
                .take(12)
                .map(|x| format!("`{}`", x.as_str().unwrap_or("")))
                .collect::<Vec<_>>();
            l.push(format!("- **{k}** ({}): {}", xs.len(), paths.join(", ")));
        }
    }
    l.push("\n### Key libraries\n".into());
    if let Some(m) = r["key_libraries"].as_object() {
        for (k, v) in m {
            let xs = v.as_array().cloned().unwrap_or_default();
            l.push(format!(
                "- `{k}`: {}",
                xs.iter()
                    .take(6)
                    .map(|x| format!("`{}`", x.as_str().unwrap_or("")))
                    .collect::<Vec<_>>()
                    .join(", ")
            ));
        }
    }
    l.push("\n### Vulkan ICDs\n".into());
    for x in r["vulkan_icds"].as_array().into_iter().flatten() {
        l.push(format!(
            "- `{}` -> `{}` (API {})",
            x["file"],
            display_json(&x["library"]),
            display_json(&x["api"])
        ));
    }
    l.push("\n### binfmt_misc (how x86 is dispatched)\n".into());
    for x in r["binfmt"].as_array().into_iter().flatten() {
        let t = x["text"].as_str().unwrap_or("");
        let short = t.chars().take(160).collect::<String>();
        l.push(format!("- `{}`: `{short}`", x["file"]));
    }
    let non = r["non_aarch64_elf"].as_array().cloned().unwrap_or_default();
    l.push(format!("\n### Non-AArch64 ELF files: {}\n", non.len()));
    let mut dirs: BTreeMap<String, u64> = BTreeMap::new();
    for x in non {
        let p = x["path"].as_str().unwrap_or("");
        let d = p.rsplit_once('/').map(|x| x.0).unwrap_or("");
        *dirs.entry(d.into()).or_default() += 1;
    }
    let mut dirs = dirs.into_iter().collect::<Vec<_>>();
    dirs.sort_by(|a, b| b.1.cmp(&a.1));
    for (d, c) in dirs.into_iter().take(25) {
        l.push(format!("- `{d}/` {c}"));
    }
    l.push("\n### pacman repositories (etc/pacman.conf)\n".into());
    for x in r["pacman_repos"].as_array().into_iter().flatten() {
        let urls = x["servers"]
            .as_array()
            .into_iter()
            .flatten()
            .take(3)
            .map(|u| format!("`{}`", u.as_str().unwrap_or("")))
            .collect::<Vec<_>>();
        l.push(format!("- `[{}]`: {}", x["name"], urls.join(", ")));
    }
    l.push("\n### Packages (name, version, arch, license)\n".into());
    l.push("| package | version | arch | license |\n|---|---|---|---|".into());
    for p in r["packages"].as_array().into_iter().flatten() {
        let lic = p["license"]
            .as_array()
            .into_iter()
            .flatten()
            .filter_map(Value::as_str)
            .collect::<Vec<_>>()
            .join(" ");
        l.push(format!(
            "| {} | {} | {} | {} |",
            p["name"],
            p["version"],
            p["arch"],
            lic.chars().take(40).collect::<String>()
        ));
    }
    l.push(String::new());
    l.join("\n")
}
fn display_json(v: &Value) -> String {
    match v {
        Value::String(s) => s.clone(),
        Value::Null => "None".into(),
        _ => v.to_string(),
    }
}
