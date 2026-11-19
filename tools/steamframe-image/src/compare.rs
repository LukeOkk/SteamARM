//! Compare the package database embedded in a Steam Frame root with pacman sync databases.

use serde::Serialize;
use serde_json::{json, Value};
use std::collections::{BTreeMap, BTreeSet};
use std::fs::{self, File};
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const STATES: [&str; 4] = ["same", "differs", "image-only", "repo-only"];

#[derive(Clone, Default)]
struct Package {
    version: String,
    arch: String,
    provides: Vec<String>,
    replaces: Vec<String>,
}

#[derive(Clone)]
struct Row {
    package: String,
    image: String,
    repository: String,
    repo: String,
    arch: String,
    state: String,
    newer: String,
    note: String,
}

pub fn run(args: impl IntoIterator<Item = std::ffi::OsString>) -> Result<i32, String> {
    let mut args = args.into_iter();
    let inventory_path = args.next().ok_or_else(usage)?;
    let mut repos = Vec::new();
    let mut md: Option<PathBuf> = None;
    let mut json_out: Option<PathBuf> = None;
    let mut all = false;
    let mut states = None;
    while let Some(arg) = args.next() {
        let arg = arg.to_string_lossy().into_owned();
        match arg.as_str() {
            "--md" => md = Some(args.next().ok_or_else(usage)?.into()),
            "--json" => json_out = Some(args.next().ok_or_else(usage)?.into()),
            "--all" => all = true,
            "--states" => {
                states = Some(
                    args.next()
                        .ok_or_else(usage)?
                        .to_string_lossy()
                        .into_owned(),
                )
            }
            _ if arg.starts_with('-') => return Err(usage()),
            _ => repos.push(arg),
        }
    }
    if repos.is_empty() {
        return Err(usage());
    }
    let (image, image_src) = load_image_packages(Path::new(&inventory_path))?;
    let mut image_map = BTreeMap::new();
    for (name, package) in image {
        image_map.insert(name, package);
    }
    let mut repo_maps = Vec::new();
    let mut sources = Vec::new();
    for src in repos {
        let raw = fetch_bytes(&src)?;
        let name = repo_name(&src);
        let db = parse_repo_db(&raw)?;
        repo_maps.push((name.clone(), db));
        sources.push(json!({"repo":name,"source":source_label(&src),"bytes":raw.len(),"sha256":sha256_hex(&raw),"packages":repo_maps.last().map_or(0,|x|x.1.len())}));
    }
    let mut merged: BTreeMap<String, (String, Package)> = BTreeMap::new();
    for (repo, packages) in repo_maps {
        for (name, pkg) in packages {
            merged.entry(name).or_insert((repo.clone(), pkg));
        }
    }
    let (rows, counts) = compare_packages(&image_map, &merged);
    let show: BTreeSet<String> = if all {
        STATES.iter().map(|s| s.to_string()).collect()
    } else if let Some(states) = states {
        states
            .split(',')
            .map(str::trim)
            .filter(|s| !s.is_empty())
            .map(str::to_owned)
            .collect()
    } else {
        STATES
            .iter()
            .filter(|s| **s != "same")
            .map(|s| s.to_string())
            .collect()
    };
    let bad = show
        .iter()
        .filter(|s| !STATES.contains(&s.as_str()))
        .cloned()
        .collect::<Vec<_>>();
    if !bad.is_empty() {
        return Err(format!(
            "--states: unknown {:?}; known: {}",
            bad,
            STATES.join(", ")
        ));
    }
    let markdown = render_markdown(&image_src, image_map.len(), &sources, &counts, &rows, &show);
    if let Some(path) = json_out.as_ref() {
        let json_rows = rows.iter().map(|r| json!({"package":r.package,"image":r.image,"repository":r.repository,"repo":r.repo,"arch":r.arch,"state":r.state,"newer":r.newer,"note":r.note})).collect::<Vec<_>>();
        let report = json!({"image":{"source":image_src,"packages":image_map.len()},"repositories":sources,"counts":counts,"rows":json_rows});
        write_json(path, &report)?;
    }
    if let Some(path) = md.as_ref() {
        fs::write(path, &markdown).map_err(|e| format!("write {}: {e}", path.display()))?;
    }
    if md.is_some() || json_out.is_some() {
        let summary = STATES
            .iter()
            .filter_map(|s| counts.get(*s).map(|n| format!("'{s}': {n}")))
            .collect::<Vec<_>>();
        let count: usize = STATES
            .iter()
            .map(|s| counts.get(*s).copied().unwrap_or(0))
            .sum();
        let md_name = md
            .as_ref()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        let json_name = json_out
            .as_ref()
            .map(|p| p.to_string_lossy().into_owned())
            .unwrap_or_default();
        println!(
            "{count} packages: {{{}}} -> {} {}",
            summary.join(", "),
            md_name,
            json_name
        );
    } else {
        print!("{markdown}");
    }
    Ok(0)
}

fn usage() -> String {
    "Usage: steamarm-frame-image compare INVENTORY_OR_ROOT REPO.db... [--md PATH] [--json PATH] [--all] [--states LIST]".into()
}

fn load_image_packages(path: &Path) -> Result<(BTreeMap<String, Package>, String), String> {
    if path.is_dir() {
        let (db, items) = crate::inventory::packages_from_root(path);
        let Some(db) = db else {
            return Err(format!("{}: no pacman local database (usr/lib/holo/pacmandb/local, var/lib/pacman/local, ...)",path.display()));
        };
        return Ok((map_inventory_packages(&items), db));
    }
    let raw = fs::read(path).map_err(|e| format!("read {}: {e}", path.display()))?;
    let inv: Value =
        serde_json::from_slice(&raw).map_err(|e| format!("parse {}: {e}", path.display()))?;
    let items = inv["packages"]
        .as_array()
        .ok_or_else(|| format!("{}: inventory JSON has no packages array", path.display()))?;
    let src = inv["pacman_db"]
        .as_str()
        .filter(|s| !s.is_empty())
        .map(str::to_owned)
        .unwrap_or_else(|| {
            path.file_name()
                .unwrap_or_default()
                .to_string_lossy()
                .into_owned()
        });
    Ok((map_inventory_packages(items), src))
}
fn map_inventory_packages(items: &[Value]) -> BTreeMap<String, Package> {
    let mut out = BTreeMap::new();
    for v in items {
        let Some(name) = v["name"].as_str() else {
            continue;
        };
        let p = Package {
            version: string(&v["version"]),
            arch: string(&v["arch"]),
            provides: string_array(&v["provides"]),
            replaces: string_array(&v["replaces"]),
            ..Package::default()
        };
        out.insert(name.to_owned(), p);
    }
    out
}
fn string(v: &Value) -> String {
    v.as_str().unwrap_or("").to_owned()
}
fn string_array(v: &Value) -> Vec<String> {
    v.as_array()
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .map(str::to_owned)
        .collect()
}

fn fetch_bytes(src: &str) -> Result<Vec<u8>, String> {
    if src.starts_with("http://") || src.starts_with("https://") {
        let out = Command::new("curl")
            .args([
                "--fail",
                "--silent",
                "--show-error",
                "--location",
                "--max-time",
                "60",
                src,
            ])
            .output()
            .map_err(|e| format!("fetch {src}: cannot run curl: {e}"))?;
        if !out.status.success() {
            return Err(format!(
                "fetch {src}: {}",
                String::from_utf8_lossy(&out.stderr).trim()
            ));
        }
        Ok(out.stdout)
    } else {
        fs::read(src).map_err(|e| format!("read {src}: {e}"))
    }
}
fn decode(raw: &[u8]) -> Result<Vec<u8>, String> {
    let utility = if raw.starts_with(&[0x28, 0xb5, 0x2f, 0xfd]) {
        Some(("zstd", vec!["-dcq"]))
    } else if raw.starts_with(&[0x1f, 0x8b]) {
        Some(("gzip", vec!["-dc"]))
    } else if raw.starts_with(&[0xfd, b'7', b'z', b'X', b'Z', 0]) {
        Some(("xz", vec!["-dc"]))
    } else if raw.starts_with(b"BZh") {
        Some(("bzip2", vec!["-dc"]))
    } else {
        None
    };
    let Some((bin, args)) = utility else {
        return Ok(raw.to_vec());
    };
    let mut child = Command::new(bin)
        .args(args)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|e| format!("{} database decoder unavailable: {e}", bin))?;
    child
        .stdin
        .take()
        .unwrap()
        .write_all(raw)
        .map_err(|e| format!("write to {bin}: {e}"))?;
    let out = child
        .wait_with_output()
        .map_err(|e| format!("wait for {bin}: {e}"))?;
    if !out.status.success() {
        return Err(format!(
            "{bin} could not decode repository database: {}",
            String::from_utf8_lossy(&out.stderr).trim()
        ));
    }
    Ok(out.stdout)
}

fn parse_repo_db(raw: &[u8]) -> Result<BTreeMap<String, Package>, String> {
    let tar = decode(raw)?;
    let mut out = BTreeMap::new();
    let mut pos = 0usize;
    while pos + 512 <= tar.len() {
        let h = &tar[pos..pos + 512];
        if h.iter().all(|b| *b == 0) {
            break;
        }
        let name = trim_nul(&h[..100]);
        let prefix = trim_nul(&h[345..500]);
        let path = if prefix.is_empty() {
            name
        } else {
            format!("{prefix}/{name}")
        };
        let size =
            parse_octal(&h[124..136]).ok_or_else(|| format!("invalid tar size for {path}"))?;
        let start = pos + 512;
        let end = start
            .checked_add(size)
            .filter(|x| *x <= tar.len())
            .ok_or_else(|| format!("truncated tar member {path}"))?;
        if path.ends_with("/desc") || path == "desc" {
            let text = String::from_utf8_lossy(&tar[start..end]);
            let fields = parse_desc(&text);
            let name = one(&fields, "NAME");
            if !name.is_empty() {
                out.insert(
                    name,
                    Package {
                        version: one(&fields, "VERSION"),
                        arch: one(&fields, "ARCH"),
                        provides: fields.get("PROVIDES").cloned().unwrap_or_default(),
                        replaces: fields.get("REPLACES").cloned().unwrap_or_default(),
                    },
                );
            }
        }
        let rounded = (size + 511) & !511;
        pos = start
            .checked_add(rounded)
            .ok_or_else(|| "tar offset overflow".to_owned())?;
    }
    Ok(out)
}
fn trim_nul(raw: &[u8]) -> String {
    String::from_utf8_lossy(raw.split(|b| *b == 0).next().unwrap_or_default())
        .trim()
        .to_owned()
}
fn parse_octal(raw: &[u8]) -> Option<usize> {
    let s = trim_nul(raw);
    if s.is_empty() {
        return Some(0);
    }
    usize::from_str_radix(s.trim(), 8).ok()
}
fn parse_desc(text: &str) -> BTreeMap<String, Vec<String>> {
    let mut out = BTreeMap::new();
    let mut cur: Option<String> = None;
    for l in text.lines() {
        if l.starts_with('%') && l.ends_with('%') {
            let k = l.trim_matches('%').to_owned();
            out.insert(k.clone(), Vec::new());
            cur = Some(k);
        } else if !l.is_empty() {
            if let Some(k) = cur.as_ref() {
                out.entry(k.clone()).or_insert_with(Vec::new).push(l.into());
            }
        }
    }
    out
}
fn one(f: &BTreeMap<String, Vec<String>>, k: &str) -> String {
    f.get(k)
        .and_then(|x| x.first())
        .cloned()
        .unwrap_or_default()
}
fn repo_name(src: &str) -> String {
    let path = if let Some((_, rest)) = src.split_once("://") {
        rest.split_once('/').map(|(_, path)| path).unwrap_or("")
    } else {
        src
    };
    let path = path.split(|c| c == '?' || c == '#').next().unwrap_or(path);
    let base = path
        .trim_end_matches('/')
        .rsplit('/')
        .next()
        .unwrap_or(path);
    base.split(".db").next().unwrap_or(base).to_owned()
}
fn source_label(src: &str) -> String {
    if src.starts_with("http://") || src.starts_with("https://") {
        crate::inventory::redact_url(src)
    } else {
        Path::new(src)
            .file_name()
            .unwrap_or_default()
            .to_string_lossy()
            .into_owned()
    }
}

fn rpmvercmp(a: &str, b: &str) -> i32 {
    if a == b {
        return 0;
    }
    let aa = a.as_bytes();
    let bb = b.as_bytes();
    let (mut i, mut j, mut p1, mut p2) = (0usize, 0usize, 0usize, 0usize);
    while i < aa.len() && j < bb.len() {
        while i < aa.len() && !aa[i].is_ascii_alphanumeric() {
            i += 1;
        }
        while j < bb.len() && !bb[j].is_ascii_alphanumeric() {
            j += 1;
        }
        if i >= aa.len() || j >= bb.len() {
            break;
        }
        if i - p1 != j - p2 {
            return if i - p1 < j - p2 { -1 } else { 1 };
        }
        p1 = i;
        p2 = j;
        let numeric = aa[p1].is_ascii_digit();
        while p1 < aa.len()
            && (if numeric {
                aa[p1].is_ascii_digit()
            } else {
                aa[p1].is_ascii_alphabetic()
            })
        {
            p1 += 1;
        }
        while p2 < bb.len()
            && (if numeric {
                bb[p2].is_ascii_digit()
            } else {
                bb[p2].is_ascii_alphabetic()
            })
        {
            p2 += 1;
        }
        if j == p2 {
            return if numeric { 1 } else { -1 };
        }
        let (mut s1, mut s2) = (&aa[i..p1], &bb[j..p2]);
        if numeric {
            while s1.first() == Some(&b'0') {
                s1 = &s1[1..];
            }
            while s2.first() == Some(&b'0') {
                s2 = &s2[1..];
            }
            if s1.len() != s2.len() {
                return if s1.len() > s2.len() { 1 } else { -1 };
            }
        }
        if s1 != s2 {
            return if s1 > s2 { 1 } else { -1 };
        }
        i = p1;
        j = p2;
    }
    if i >= aa.len() && j >= bb.len() {
        return 0;
    }
    let one = aa.get(i).copied();
    let two = bb.get(j).copied();
    if one.is_none() && two.is_some_and(|c| !c.is_ascii_alphabetic())
        || one.is_some_and(|c| c.is_ascii_alphabetic())
    {
        -1
    } else {
        1
    }
}
fn split_evr(v: &str) -> (String, String, Option<String>) {
    let bytes = v.as_bytes();
    let mut s = 0;
    while s < bytes.len() && bytes[s].is_ascii_digit() {
        s += 1;
    }
    let has_epoch = s < bytes.len() && bytes[s] == b':';
    let (start, epoch) = if has_epoch {
        (
            s + 1,
            if s == 0 {
                "0".to_owned()
            } else {
                v[..s].to_owned()
            },
        )
    } else {
        (0, "0".into())
    };
    let sep = v[start..].rfind('-').map(|x| x + start);
    if let Some(i) = sep {
        (epoch, v[start..i].to_owned(), Some(v[i + 1..].to_owned()))
    } else {
        (epoch, v[start..].to_owned(), None)
    }
}
fn vercmp(a: &str, b: &str) -> i32 {
    if a == b {
        return 0;
    }
    let (e1, v1, r1) = split_evr(a);
    let (e2, v2, r2) = split_evr(b);
    let mut c = rpmvercmp(&e1, &e2);
    if c == 0 {
        c = rpmvercmp(&v1, &v2);
        if c == 0 {
            if let (Some(x), Some(y)) = (r1, r2) {
                c = rpmvercmp(&x, &y);
            }
        }
    }
    c
}
fn parse_dep(dep: &str) -> (String, String, String) {
    let s = dep.trim();
    let bytes = s.as_bytes();
    for (i, c) in bytes.iter().enumerate() {
        if [b'<', b'>', b'='].contains(c) {
            let mut j = i + 1;
            if j < bytes.len() && bytes[j] == b'=' && *c != b'=' {
                j += 1;
            }
            return (s[..i].trim().into(), s[i..j].into(), s[j..].trim().into());
        }
    }
    (s.into(), String::new(), String::new())
}
fn dep_allows(op: &str, want: &str, have: &str) -> bool {
    if op.is_empty() || want.is_empty() {
        return true;
    }
    let c = vercmp(have, want);
    match op {
        "<" => c < 0,
        "<=" => c <= 0,
        "=" => c == 0,
        ">=" => c >= 0,
        ">" => c > 0,
        _ => true,
    }
}
fn relations(p: &Package) -> Vec<(String, String, String, String)> {
    let mut out = Vec::new();
    for (kind, list) in [("provides", &p.provides), ("replaces", &p.replaces)] {
        for d in list {
            let (n, op, w) = parse_dep(d);
            out.push((kind.into(), n, op, w));
        }
    }
    out
}
fn compare_packages(
    image: &BTreeMap<String, Package>,
    repos: &BTreeMap<String, (String, Package)>,
) -> (Vec<Row>, BTreeMap<String, usize>) {
    let image_only = image
        .iter()
        .filter(|(n, _)| !repos.contains_key(*n))
        .map(|(n, p)| (n.clone(), p.clone()))
        .collect::<BTreeMap<_, _>>();
    let repo_only = repos
        .iter()
        .filter(|(n, _)| !image.contains_key(*n))
        .map(|(n, (_, p))| (n.clone(), p.clone()))
        .collect::<BTreeMap<_, _>>();
    fn counterparts(
        pkg: &str,
        info: &Package,
        other: &BTreeMap<String, Package>,
    ) -> Vec<(String, String)> {
        let mut out = Vec::new();
        for (kind, n, op, want) in relations(info) {
            if let Some(other_pkg) = other.get(&n) {
                if n != pkg && (kind == "provides" || dep_allows(&op, &want, &other_pkg.version)) {
                    out.push((
                        n,
                        if kind == "provides" {
                            "provided by this".into()
                        } else {
                            "replaced by this".into()
                        },
                    ));
                }
            }
        }
        for (n, other_info) in other {
            for (kind, dn, op, want) in relations(other_info) {
                if dn == pkg
                    && n != pkg
                    && (kind == "provides" || dep_allows(&op, &want, &info.version))
                {
                    out.push((n.clone(), format!("{kind} this")));
                }
            }
        }
        let mut seen = BTreeSet::new();
        out.retain(|(n, _)| seen.insert(n.clone()));
        out
    }
    let mut counts = BTreeMap::new();
    let mut rows = Vec::new();
    let names = image
        .keys()
        .chain(repos.keys())
        .cloned()
        .collect::<BTreeSet<_>>();
    for name in names {
        let ii = image.get(&name);
        let ri = repos.get(&name);
        let iv = ii.map_or("", |p| p.version.as_str());
        let rv = ri.map_or("", |(_, p)| p.version.as_str());
        let mut newer = String::new();
        let mut note = String::new();
        let state;
        if let (Some(_ip), Some((_repo_name, _rp))) = (ii, ri) {
            if iv == rv {
                state = "same";
            } else {
                state = "differs";
                let order = vercmp(iv, rv);
                newer = if order > 0 {
                    "image"
                } else if order < 0 {
                    "repo"
                } else {
                    ""
                }
                .into();
                let (e1, v1, _) = split_evr(iv);
                let (e2, v2, _) = split_evr(rv);
                note = if rpmvercmp(&e1, &e2) == 0 && rpmvercmp(&v1, &v2) == 0 {
                    "pkgrel"
                } else {
                    "version"
                }
                .into();
                *counts
                    .entry(format!(
                        "differs:{}",
                        if newer.is_empty() {
                            "equal-order"
                        } else {
                            &newer
                        }
                    ))
                    .or_default() += 1;
                *counts.entry(format!("differs:{note}")).or_default() += 1;
            }
        } else if let Some(ip) = ii {
            state = "image-only";
            let cp = counterparts(&name, ip, &repo_only);
            if !cp.is_empty() {
                note = format!(
                    "repo: {}",
                    cp.iter()
                        .map(|(n, rel)| format!("{} {} ({rel})", n, repos[n].1.version))
                        .collect::<Vec<_>>()
                        .join(", ")
                );
                *counts.entry("image-only:counterpart".into()).or_default() += 1;
            }
        } else {
            state = "repo-only";
            let (rn, rp) = ri.unwrap();
            let cp = counterparts(&name, rp, &image_only);
            if !cp.is_empty() {
                note = format!(
                    "image: {}",
                    cp.iter()
                        .map(|(n, rel)| format!("{} {} ({rel})", n, image[n].version))
                        .collect::<Vec<_>>()
                        .join(", ")
                );
                *counts.entry("repo-only:counterpart".into()).or_default() += 1;
            }
            let _ = rn;
        }
        *counts.entry(state.into()).or_default() += 1;
        let repo = ri.map_or("", |x| x.0.as_str());
        let arch = ii.map_or_else(
            || ri.map_or("", |x| x.1.arch.as_str()),
            |p| {
                if p.arch.is_empty() {
                    ri.map_or("", |x| x.1.arch.as_str())
                } else {
                    p.arch.as_str()
                }
            },
        );
        rows.push(Row {
            package: name,
            image: iv.into(),
            repository: rv.into(),
            repo: repo.into(),
            arch: arch.into(),
            state: state.into(),
            newer,
            note,
        });
    }
    (rows, counts)
}

fn render_markdown(
    image_src: &str,
    image_count: usize,
    sources: &[Value],
    counts: &BTreeMap<String, usize>,
    rows: &[Row],
    show: &BTreeSet<String>,
) -> String {
    let mut l = vec![
        "## Steam Frame image vs repository (generated by steamarm-frame-image compare)\n".into(),
        format!("Image: {image_count} packages ({image_src})."),
        "Repositories, in pacman order (the first one that has a name wins):\n".into(),
    ];
    for s in sources {
        l.push(format!(
            "- `{}`: `{}`, {} packages, {} bytes, sha256 `{}`",
            s["repo"], s["source"], s["packages"], s["bytes"], s["sha256"]
        ));
    }
    l.push(String::new());
    l.push(format!(
        "{}\n",
        STATES
            .iter()
            .filter_map(|k| counts.get(*k).map(|v| format!("{k} {v}")))
            .collect::<Vec<_>>()
            .join(", ")
    ));
    l.push(format!("differs: image newer {}, repository newer {}, same order {}; pkgrel only {}, version {}. image-only with a repository counterpart (PROVIDES/REPLACES) {}; repo-only with an image counterpart {}.\n",counts.get("differs:image").unwrap_or(&0),counts.get("differs:repo").unwrap_or(&0),counts.get("differs:equal-order").unwrap_or(&0),counts.get("differs:pkgrel").unwrap_or(&0),counts.get("differs:version").unwrap_or(&0),counts.get("image-only:counterpart").unwrap_or(&0),counts.get("repo-only:counterpart").unwrap_or(&0)));
    l.push("| package | image | repository | repo | arch | state | newer | note |".into());
    l.push("|---|---|---|---|---|---|---|---|".into());
    for r in rows {
        if show.contains(&r.state) {
            l.push(format!(
                "| {} | {} | {} | {} | {} | {} | {} | {} |",
                r.package, r.image, r.repository, r.repo, r.arch, r.state, r.newer, r.note
            ));
        }
    }
    l.join("\n") + "\n"
}
fn write_json(path: &Path, value: &Value) -> Result<(), String> {
    let file = File::create(path).map_err(|e| format!("create {}: {e}", path.display()))?;
    let fmt = serde_json::ser::PrettyFormatter::with_indent(b" ");
    let mut ser = serde_json::Serializer::with_formatter(file, fmt);
    value
        .serialize(&mut ser)
        .map_err(|e| format!("write {}: {e}", path.display()))?;
    Ok(())
}

fn sha256_hex(data: &[u8]) -> String {
    const K: [u32; 64] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    ];
    let mut msg = data.to_vec();
    let bits = (msg.len() as u64).wrapping_mul(8);
    msg.push(0x80);
    while msg.len() % 64 != 56 {
        msg.push(0);
    }
    msg.extend_from_slice(&bits.to_be_bytes());
    let mut h = [
        0x6a09e667u32,
        0xbb67ae85,
        0x3c6ef372,
        0xa54ff53a,
        0x510e527f,
        0x9b05688c,
        0x1f83d9ab,
        0x5be0cd19,
    ];
    for chunk in msg.chunks_exact(64) {
        let mut w = [0u32; 64];
        for i in 0..16 {
            w[i] = u32::from_be_bytes(chunk[i * 4..i * 4 + 4].try_into().unwrap());
        }
        for i in 16..64 {
            let s0 = w[i - 15].rotate_right(7) ^ w[i - 15].rotate_right(18) ^ (w[i - 15] >> 3);
            let s1 = w[i - 2].rotate_right(17) ^ w[i - 2].rotate_right(19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16]
                .wrapping_add(s0)
                .wrapping_add(w[i - 7])
                .wrapping_add(s1);
        }
        let (mut a, mut b, mut c, mut d, mut e, mut f, mut g, mut z) =
            (h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
        for i in 0..64 {
            let s1 = e.rotate_right(6) ^ e.rotate_right(11) ^ e.rotate_right(25);
            let ch = (e & f) ^ (!e & g);
            let t1 = z
                .wrapping_add(s1)
                .wrapping_add(ch)
                .wrapping_add(K[i])
                .wrapping_add(w[i]);
            let s0 = a.rotate_right(2) ^ a.rotate_right(13) ^ a.rotate_right(22);
            let maj = (a & b) ^ (a & c) ^ (b & c);
            let t2 = s0.wrapping_add(maj);
            z = g;
            g = f;
            f = e;
            e = d.wrapping_add(t1);
            d = c;
            c = b;
            b = a;
            a = t1.wrapping_add(t2);
        }
        for (i, x) in [a, b, c, d, e, f, g, z].iter().enumerate() {
            h[i] = h[i].wrapping_add(*x);
        }
    }
    h.iter().map(|x| format!("{x:08x}")).collect()
}
