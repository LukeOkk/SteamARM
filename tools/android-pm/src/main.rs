use serde_json::{json, Map, Value};
use std::collections::BTreeSet;
use std::env;
use std::ffi::{c_int, OsString};
use std::fs::{self, File, OpenOptions};
use std::io;
use std::os::fd::AsRawFd;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};
use std::time::{SystemTime, UNIX_EPOCH};

const EXIT_OK: i32 = 0;
const EXIT_INPUT: i32 = 2;
const EXIT_UNSUPPORTED: i32 = 3;
const EXIT_REFUSED: i32 = 4;
const EXIT_MISSING: i32 = 5;
const EXIT_IO: i32 = 6;
const USAGE: &str = "SteamARM Android package manager\n\n  steamarm-android-pm [--state DIR] install [--force] [--allow-downgrade] app.apk\n  steamarm-android-pm [--state DIR] uninstall [--keep-data] <package>\n  steamarm-android-pm [--state DIR] list\n  steamarm-android-pm [--state DIR] info <package>";

unsafe extern "C" {
    fn flock(fd: c_int, operation: c_int) -> c_int;
}

#[derive(Debug)]
struct PmError {
    status: i32,
    code: String,
    message: String,
    extra: Map<String, Value>,
}

impl PmError {
    fn new(status: i32, code: &str, message: impl Into<String>) -> Self {
        Self {
            status,
            code: code.to_owned(),
            message: message.into(),
            extra: Map::new(),
        }
    }

    fn with(mut self, key: &str, value: Value) -> Self {
        self.extra.insert(key.to_owned(), value);
        self
    }

    fn output(self) -> Value {
        let mut value = Map::new();
        value.insert("ok".into(), Value::Bool(false));
        value.insert("code".into(), Value::String(self.code));
        value.insert("error".into(), Value::String(self.message));
        value.extend(self.extra);
        Value::Object(value)
    }
}

#[derive(Clone)]
struct Layout {
    root: PathBuf,
    packages: PathBuf,
    data: PathBuf,
    kept: PathBuf,
}

impl Layout {
    fn new(state: PathBuf) -> Self {
        let root = state.join("android");
        Self {
            packages: root.join("packages"),
            data: root.join("data"),
            kept: root.join("kept"),
            root,
        }
    }

    fn package_dir(&self, package: &str) -> PathBuf {
        self.packages.join(package)
    }

    fn data_dir(&self, package: &str) -> PathBuf {
        self.data.join(package)
    }

    fn kept_record(&self, package: &str) -> PathBuf {
        self.kept.join(format!("{package}.json"))
    }

    fn meta(&self, package: &str) -> Option<Value> {
        let bytes = fs::read(self.package_dir(package).join("meta.json")).ok()?;
        serde_json::from_slice(&bytes).ok()
    }
}

fn home_dir() -> PathBuf {
    env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"))
}

fn expand_path(path: PathBuf) -> PathBuf {
    if path == Path::new("~") {
        return home_dir();
    }
    if let Some(value) = path.to_str().and_then(|s| s.strip_prefix("~/")) {
        return home_dir().join(value);
    }
    path
}

fn state_dir(argument: Option<PathBuf>) -> PathBuf {
    if let Some(path) = argument {
        return expand_path(path);
    }
    if let Some(path) = env::var_os("STEAMARM_STATE").filter(|v| !v.is_empty()) {
        return expand_path(PathBuf::from(path));
    }
    let newer = home_dir().join("Developer/SteamARM-roots");
    let older = home_dir().join("SteamARM-roots");
    if newer.is_dir() || !older.is_dir() {
        newer
    } else {
        older
    }
}

fn project_root() -> PathBuf {
    env::var_os("STEAMARM_PROJECT_ROOT")
        .map(PathBuf::from)
        .unwrap_or_else(|| Path::new(env!("CARGO_MANIFEST_DIR")).join("../.."))
}

fn inspector_script() -> PathBuf {
    project_root().join("scripts/apk-inspect.py")
}

fn check_package(package: &str) -> Result<(), PmError> {
    let valid = package.len() <= 255
        && package.split('.').count() >= 2
        && package.split('.').all(|segment| {
            let mut chars = segment.bytes();
            chars.next().is_some_and(|c| c.is_ascii_alphabetic())
                && chars.all(|c| c.is_ascii_alphanumeric() || c == b'_')
        });
    if !valid {
        return Err(PmError::new(
            EXIT_INPUT,
            "bad-package",
            format!("invalid Android package name {package:?}"),
        ));
    }
    Ok(())
}

fn now() -> String {
    Command::new("date")
        .args(["-u", "+%Y-%m-%dT%H:%M:%SZ"])
        .output()
        .ok()
        .filter(|o| o.status.success())
        .map(|o| String::from_utf8_lossy(&o.stdout).trim().to_owned())
        .filter(|s| !s.is_empty())
        .unwrap_or_else(|| {
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap_or_default()
                .as_secs()
                .to_string()
        })
}

fn read_json(path: &Path) -> Option<Value> {
    serde_json::from_slice(&fs::read(path).ok()?).ok()
}

fn write_json(path: &Path, value: &Value) -> io::Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)?;
    }
    let mut bytes = serde_json::to_vec_pretty(value).map_err(io::Error::other)?;
    bytes.push(b'\n');
    fs::write(path, bytes)
}

fn next_temp_dir(parent: &Path, prefix: &str) -> io::Result<PathBuf> {
    fs::create_dir_all(parent)?;
    let epoch = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_nanos();
    for sequence in 0..1000u32 {
        let candidate = parent.join(format!("{prefix}{}-{epoch}-{sequence}", std::process::id()));
        match fs::create_dir(&candidate) {
            Ok(()) => return Ok(candidate),
            Err(e) if e.kind() == io::ErrorKind::AlreadyExists => continue,
            Err(e) => return Err(e),
        }
    }
    Err(io::Error::new(
        io::ErrorKind::AlreadyExists,
        "could not allocate a unique staging directory",
    ))
}

fn recover(layout: &Layout) -> io::Result<()> {
    if !layout.packages.is_dir() {
        return Ok(());
    }
    let mut entries = fs::read_dir(&layout.packages)?
        .filter_map(Result::ok)
        .collect::<Vec<_>>();
    entries.sort_by_key(|entry| entry.file_name());
    for entry in entries {
        let name = entry.file_name().to_string_lossy().into_owned();
        let path = entry.path();
        if name.starts_with(".old-") && path.is_dir() {
            let old = path.join("pkg");
            let package = read_json(&old.join("meta.json")).and_then(|meta| {
                meta.get("package")
                    .and_then(Value::as_str)
                    .map(str::to_owned)
            });
            if let Some(package) = package.filter(|p| check_package(p).is_ok()) {
                let final_path = layout.package_dir(&package);
                if !final_path.exists() {
                    fs::rename(&old, &final_path)?;
                }
            }
            let _ = fs::remove_dir_all(path);
        } else if name.starts_with(".install-") && path.is_dir() {
            let _ = fs::remove_dir_all(path);
        }
    }
    Ok(())
}

fn lock(layout: &Layout) -> Result<File, PmError> {
    fs::create_dir_all(&layout.root)
        .map_err(|e| PmError::new(EXIT_IO, "io", format!("install failed: {e}")))?;
    let file = OpenOptions::new()
        .create(true)
        .truncate(true)
        .write(true)
        .open(layout.root.join(".android-pm.lock"))
        .map_err(|e| PmError::new(EXIT_IO, "io", format!("cannot lock Android packages: {e}")))?;
    const LOCK_EX: c_int = 2;
    if unsafe { flock(file.as_raw_fd(), LOCK_EX) } != 0 {
        return Err(PmError::new(
            EXIT_IO,
            "io",
            format!(
                "cannot lock Android packages: {}",
                io::Error::last_os_error()
            ),
        ));
    }
    recover(layout).map_err(|e| {
        PmError::new(
            EXIT_IO,
            "io",
            format!("cannot recover Android package state: {e}"),
        )
    })?;
    Ok(file)
}

fn string_set(value: Option<&Value>) -> BTreeSet<String> {
    value
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(Value::as_str)
        .map(str::to_owned)
        .collect()
}

fn same_signer(old: Option<&Value>, new: Option<&Value>) -> (bool, String) {
    let old = old.unwrap_or(&Value::Null);
    let new = new.unwrap_or(&Value::Null);
    let a = string_set(old.get("certificates"));
    let b = string_set(new.get("certificates"));
    if a.is_empty() || b.is_empty() {
        return (
            false,
            "the signing certificate of the installed app or of the APK is unknown".to_owned(),
        );
    }
    if a == b {
        return (true, "same signing certificate".to_owned());
    }
    let mut a_all = a.clone();
    a_all.extend(string_set(old.get("rotatedCertificates")));
    let mut b_all = b.clone();
    b_all.extend(string_set(new.get("rotatedCertificates")));
    if a_all.iter().any(|cert| b_all.contains(cert)) {
        return (
            true,
            "same signer (a v3.1 rotated key on one side)".to_owned(),
        );
    }
    let lineage = string_set(new.get("lineage"));
    if a_all.iter().any(|cert| lineage.contains(cert)) {
        return (
            true,
            "key rotation: the new APK's v3 lineage includes the installed certificate".to_owned(),
        );
    }
    let short = |items: &BTreeSet<String>| {
        items
            .iter()
            .map(|s| s.chars().take(16).collect::<String>())
            .collect::<Vec<_>>()
            .join(", ")
    };
    (
        false,
        format!(
            "the APK is signed with a different certificate ({}) than the installed app ({})",
            short(&b),
            short(&a)
        ),
    )
}

fn run_inspector(apk: &Path, icon_out: Option<&Path>) -> Result<(Value, i32), PmError> {
    let mut command = Command::new("python3");
    command.arg(inspector_script());
    if let Some(path) = icon_out {
        command.arg("--extract-icon").arg(path);
    }
    let output = command
        .arg(apk)
        .stdin(Stdio::null())
        .output()
        .map_err(|e| PmError::new(EXIT_IO, "io", format!("cannot start APK inspector: {e}")))?;
    let status = output.status.code().unwrap_or(EXIT_IO);
    let stdout = String::from_utf8_lossy(&output.stdout);
    let value = serde_json::from_str::<Value>(&stdout).map_err(|e| {
        let detail = String::from_utf8_lossy(&output.stderr);
        PmError::new(
            EXIT_IO,
            "io",
            format!(
                "APK inspector returned invalid JSON: {e}; {}",
                detail.trim()
            ),
        )
    })?;
    Ok((value, status))
}

fn file_sha256(path: &Path) -> Result<String, PmError> {
    let output = Command::new("shasum")
        .arg("-a")
        .arg("256")
        .arg(path)
        .stdin(Stdio::null())
        .output()
        .map_err(|e| {
            PmError::new(
                EXIT_IO,
                "io",
                format!("cannot hash {}: {e}", path.display()),
            )
        })?;
    if !output.status.success() {
        return Err(PmError::new(
            EXIT_IO,
            "io",
            format!("cannot hash {}", path.display()),
        ));
    }
    Ok(String::from_utf8_lossy(&output.stdout)
        .split_whitespace()
        .next()
        .unwrap_or_default()
        .to_owned())
}

fn absolute(path: &Path) -> PathBuf {
    if path.is_absolute() {
        path.to_owned()
    } else {
        env::current_dir()
            .unwrap_or_else(|_| PathBuf::from("."))
            .join(path)
    }
}

fn summary(meta: &Value, layout: &Layout) -> Value {
    let package = meta
        .get("package")
        .and_then(Value::as_str)
        .unwrap_or_default();
    let icon = meta.get("icon").and_then(Value::as_str);
    let permissions = meta
        .get("permissions")
        .and_then(Value::as_array)
        .into_iter()
        .flatten()
        .filter_map(|permission| permission.get("name").cloned())
        .collect::<Vec<_>>();
    json!({
        "package": meta.get("package").cloned().unwrap_or(Value::Null),
        "label": meta.get("label").cloned().unwrap_or(Value::Null),
        "versionName": meta.get("versionName").cloned().unwrap_or(Value::Null),
        "versionCode": meta.get("versionCode").cloned().unwrap_or(Value::Null),
        "minSdk": meta.get("minSdk").cloned().unwrap_or(Value::Null),
        "targetSdk": meta.get("targetSdk").cloned().unwrap_or(Value::Null),
        "abis": meta.get("abis").cloned().unwrap_or(Value::Null),
        "abiVerdict": meta.pointer("/abiVerdict/id").cloned().unwrap_or(Value::Null),
        "launcherActivity": meta.get("launcherActivity").cloned().unwrap_or(Value::Null),
        "packageDir": layout.package_dir(package).to_string_lossy(),
        "dataDir": layout.data_dir(package).to_string_lossy(),
        "icon": icon.map(|name| layout.package_dir(package).join(name).to_string_lossy().into_owned()),
        "installedAt": meta.get("installedAt").cloned().unwrap_or(Value::Null),
        "updatedAt": meta.get("updatedAt").cloned().unwrap_or(Value::Null),
        "sha256": meta.get("sha256").cloned().unwrap_or(Value::Null),
        "permissions": permissions,
    })
}

fn value_u64(value: Option<&Value>) -> Option<u64> {
    value.and_then(Value::as_u64).or_else(|| {
        value
            .and_then(Value::as_i64)
            .and_then(|v| u64::try_from(v).ok())
    })
}

fn extract_member(archive: &Path, member: &str, destination: &Path) -> Result<(), PmError> {
    if let Some(parent) = destination.parent() {
        fs::create_dir_all(parent).map_err(|e| {
            PmError::new(
                EXIT_IO,
                "io",
                format!("cannot create {}: {e}", parent.display()),
            )
        })?;
    }
    let output = File::create(destination).map_err(|e| {
        PmError::new(
            EXIT_IO,
            "io",
            format!("cannot create {}: {e}", destination.display()),
        )
    })?;
    let status = Command::new("unzip")
        .arg("-p")
        .arg(archive)
        .arg(member)
        .stdin(Stdio::null())
        .stdout(Stdio::from(output))
        .stderr(Stdio::null())
        .status()
        .map_err(|e| PmError::new(EXIT_IO, "io", format!("cannot extract {member}: {e}")))?;
    if !status.success() {
        return Err(PmError::new(
            EXIT_INPUT,
            "not-apk",
            format!("cannot extract archive member {member}"),
        ));
    }
    Ok(())
}

fn convert_icon(raw: &Path, format: &str, stage: &Path) -> Option<PathBuf> {
    let raw_path = stage.join(format!(
        "icon.{}",
        if format.is_empty() { "bin" } else { format }
    ));
    if fs::rename(raw, &raw_path).is_err() {
        return None;
    }
    let png = stage.join("icon.png");
    if format == "png" {
        return Some(png);
    }
    let sips = if Path::new("/usr/bin/sips").is_file() {
        Some(PathBuf::from("/usr/bin/sips"))
    } else {
        Command::new("which")
            .arg("sips")
            .output()
            .ok()
            .filter(|o| o.status.success())
            .map(|o| PathBuf::from(String::from_utf8_lossy(&o.stdout).trim()))
    };
    let Some(sips) = sips else {
        return Some(raw_path);
    };
    let result = Command::new(sips)
        .args(["-s", "format", "png"])
        .arg(&raw_path)
        .arg("--out")
        .arg(&png)
        .stdout(Stdio::null())
        .stderr(Stdio::null())
        .status()
        .ok()?;
    if result.success() && png.is_file() && fs::metadata(&png).ok()?.len() > 0 {
        let _ = fs::remove_file(raw_path);
        Some(png)
    } else {
        let _ = fs::remove_file(png);
        Some(raw_path)
    }
}

fn write_icon(apk: &Path, report: &Value, stage: &Path) -> Result<Option<String>, PmError> {
    let icon = report.get("icon").unwrap_or(&Value::Null);
    if icon.get("path").and_then(Value::as_str).is_none() {
        return Ok(None);
    }
    let format = icon.get("format").and_then(Value::as_str).unwrap_or("bin");
    let temporary = stage.join(format!(".icon-source-{format}"));
    let (extracted, status) = run_inspector(apk, Some(&temporary))?;
    if status != EXIT_OK || !temporary.is_file() {
        return Err(PmError::new(
            EXIT_IO,
            "io",
            extracted
                .get("error")
                .and_then(Value::as_str)
                .map(|s| format!("cannot extract APK icon: {s}"))
                .unwrap_or_else(|| "cannot extract APK icon".to_owned()),
        ));
    }
    let path = convert_icon(&temporary, format, stage).unwrap_or_else(|| {
        let fallback = stage.join(format!("icon.{format}"));
        let _ = fs::rename(&temporary, &fallback);
        fallback
    });
    Ok(path.file_name().map(|s| s.to_string_lossy().into_owned()))
}

fn install(
    layout: &Layout,
    apk_path: &Path,
    force: bool,
    allow_downgrade: bool,
) -> Result<Value, PmError> {
    let apk_path = absolute(apk_path);
    let (report, inspect_status) = run_inspector(&apk_path, None)?;
    if inspect_status == EXIT_UNSUPPORTED {
        let format = report
            .get("format")
            .and_then(Value::as_str)
            .unwrap_or("bundle");
        let reason = report
            .get("reason")
            .and_then(Value::as_str)
            .unwrap_or("unsupported Android bundle");
        return Err(PmError::new(
            EXIT_UNSUPPORTED,
            "bundle",
            format!("{}: {reason}", format.to_uppercase()),
        )
        .with("format", Value::String(format.to_owned())));
    }
    if inspect_status != EXIT_OK {
        return Err(PmError::new(
            EXIT_INPUT,
            "not-apk",
            report
                .get("error")
                .and_then(Value::as_str)
                .unwrap_or("not a readable APK"),
        ));
    }
    let package = report
        .get("package")
        .and_then(Value::as_str)
        .unwrap_or_default()
        .to_owned();
    check_package(&package)?;
    let splits = report.get("splits").cloned().unwrap_or_else(|| json!({}));
    let bundle = report
        .get("bundle")
        .filter(|v| v.as_object().is_some_and(|object| !object.is_empty()))
        .cloned()
        .unwrap_or_else(|| json!({}));
    if splits.get("isSplit").and_then(Value::as_bool) == Some(true)
        && bundle.as_object().is_some_and(Map::is_empty)
    {
        let split = splits
            .get("split")
            .and_then(Value::as_str)
            .unwrap_or("unknown");
        return Err(PmError::new(
            EXIT_UNSUPPORTED,
            "split-apk",
            format!(
                "INSTALL_FAILED_MISSING_SPLIT: {package} is a split APK ({split}), not a base APK"
            ),
        ));
    }
    if splits.get("needsSplits").and_then(Value::as_bool) == Some(true)
        && bundle.as_object().is_some_and(Map::is_empty)
    {
        return Err(PmError::new(
            EXIT_UNSUPPORTED,
            "needs-splits",
            format!(
                "INSTALL_FAILED_MISSING_SPLIT: {package} needs split APKs absent from this file"
            ),
        ));
    }
    let signing = report.get("signing").cloned().unwrap_or_else(|| json!({}));
    if string_set(signing.get("certificates")).is_empty() {
        return Err(PmError::new(
            EXIT_UNSUPPORTED,
            "unsigned",
            format!("{package} has no v1, v2 or v3 signature whose certificate could be read"),
        ));
    }

    let _guard = lock(layout)?;
    let old = layout.meta(&package);
    let data_dir = layout.data_dir(&package);
    let kept = if data_dir.is_dir() {
        read_json(&layout.kept_record(&package))
    } else {
        None
    };
    let mut previous = old.clone().or(kept);
    if previous.is_none() {
        let has_data = data_dir.is_dir()
            && fs::read_dir(&data_dir)
                .ok()
                .is_some_and(|mut entries| entries.next().is_some());
        if layout.package_dir(&package).exists() || has_data {
            previous = Some(json!({"signing": null}));
        }
    }
    if let Some(previous_meta) = previous.as_ref() {
        let (accepted, reason) = same_signer(previous_meta.get("signing"), Some(&signing));
        if !accepted && !force {
            return Err(PmError::new(EXIT_REFUSED, "different-signer", reason)
                .with("package", Value::String(package.clone())));
        }
        let code = value_u64(report.get("versionCode"));
        let old_code = value_u64(previous_meta.get("versionCode"));
        if old.is_some()
            && code.zip(old_code).is_some_and(|(new, old)| new < old)
            && !allow_downgrade
        {
            let old_meta = old.as_ref().unwrap();
            return Err(PmError::new(
                EXIT_REFUSED,
                "downgrade",
                format!(
                    "{package} {} ({}) is older than the installed {} ({})",
                    report
                        .get("versionName")
                        .and_then(Value::as_str)
                        .unwrap_or("None"),
                    code.unwrap_or_default(),
                    old_meta
                        .get("versionName")
                        .and_then(Value::as_str)
                        .unwrap_or("None"),
                    old_code.unwrap_or_default()
                ),
            )
            .with("package", Value::String(package.clone())));
        }
    }
    let action = if old.is_none() {
        "installed"
    } else if old.as_ref().and_then(|m| m.get("versionCode")) == report.get("versionCode") {
        "reinstalled"
    } else {
        "updated"
    };
    fs::create_dir_all(&data_dir)
        .and_then(|_| fs::create_dir_all(&layout.packages))
        .map_err(|e| PmError::new(EXIT_IO, "io", format!("install failed: {e}")))?;
    let stage = next_temp_dir(&layout.packages, &format!(".install-{package}-"))
        .map_err(|e| PmError::new(EXIT_IO, "io", format!("install failed: {e}")))?;
    let mut split_files = Vec::<String>::new();
    let mut obb_files = Vec::<String>::new();
    let staging_result = (|| -> Result<Option<String>, PmError> {
        let base = stage.join("base.apk");
        if !bundle.as_object().is_some_and(Map::is_empty) {
            let expected = report
                .get("sha256")
                .and_then(Value::as_str)
                .unwrap_or_default();
            if file_sha256(&apk_path)? != expected {
                return Err(PmError::new(
                    EXIT_IO,
                    "io",
                    "the bundle changed after inspection",
                ));
            }
            let members = bundle
                .get("members")
                .and_then(Value::as_array)
                .cloned()
                .unwrap_or_default();
            let base_member = bundle
                .get("base")
                .and_then(Value::as_str)
                .unwrap_or_default();
            let by_path = members
                .iter()
                .filter_map(|member| Some((member.get("path")?.as_str()?.to_owned(), member)))
                .collect::<std::collections::HashMap<_, _>>();
            let chosen = bundle
                .get("chosen")
                .and_then(Value::as_array)
                .cloned()
                .unwrap_or_default();
            let mut found_base = false;
            for item in chosen {
                let member = item
                    .as_str()
                    .ok_or_else(|| PmError::new(EXIT_INPUT, "not-apk", "invalid bundle member"))?;
                if member == base_member {
                    extract_member(&apk_path, member, &base)?;
                    found_base = true;
                } else {
                    let split = by_path
                        .get(member)
                        .and_then(|v| v.get("split"))
                        .and_then(Value::as_str)
                        .filter(|s| {
                            !s.is_empty()
                                && s.bytes()
                                    .all(|c| c.is_ascii_alphanumeric() || b"_.-".contains(&c))
                        })
                        .ok_or_else(|| {
                            PmError::new(
                                EXIT_INPUT,
                                "not-apk",
                                format!("invalid split name: {member:?}"),
                            )
                        })?;
                    let filename = format!("split_{split}.apk");
                    if split_files.contains(&filename) {
                        return Err(PmError::new(
                            EXIT_INPUT,
                            "not-apk",
                            format!("duplicate split name: {split}"),
                        ));
                    }
                    split_files.push(filename.clone());
                    extract_member(&apk_path, member, &stage.join(filename))?;
                }
            }
            if !found_base || !base.is_file() {
                return Err(PmError::new(EXIT_INPUT, "not-apk", "no base APK selected"));
            }
            for item in bundle
                .get("obb")
                .and_then(Value::as_array)
                .into_iter()
                .flatten()
            {
                let member = item.as_str().unwrap_or_default();
                let name = Path::new(member)
                    .file_name()
                    .and_then(|s| s.to_str())
                    .unwrap_or_default();
                if name.is_empty() {
                    return Err(PmError::new(
                        EXIT_INPUT,
                        "not-apk",
                        format!("invalid OBB name: {member:?}"),
                    ));
                }
                let rel = PathBuf::from("obb").join(name);
                let target = stage.join(&rel);
                if target.exists() {
                    return Err(PmError::new(
                        EXIT_INPUT,
                        "not-apk",
                        format!("duplicate OBB name: {name}"),
                    ));
                }
                extract_member(&apk_path, member, &target)?;
                obb_files.push(rel.to_string_lossy().into_owned());
            }
        } else {
            fs::copy(&apk_path, &base)
                .map_err(|e| PmError::new(EXIT_IO, "io", format!("install failed: {e}")))?;
            if file_sha256(&base)?
                != report
                    .get("sha256")
                    .and_then(Value::as_str)
                    .unwrap_or_default()
            {
                return Err(PmError::new(
                    EXIT_IO,
                    "io",
                    format!(
                        "the copy of {} does not match the APK that was read",
                        apk_path.display()
                    ),
                ));
            }
        }
        write_icon(&apk_path, &report, &stage)
    })();
    let icon = match staging_result {
        Ok(icon) => icon,
        Err(error) => {
            let _ = fs::remove_dir_all(&stage);
            return Err(error);
        }
    };

    let mut metadata = report.as_object().cloned().unwrap_or_default();
    metadata.remove("fdroid");
    metadata.insert("schema".into(), json!(1));
    metadata.insert("splits".into(), json!(split_files));
    metadata.insert("obb".into(), json!(obb_files));
    metadata.insert(
        "icon".into(),
        icon.as_ref().map_or(Value::Null, |s| json!(s)),
    );
    metadata.insert(
        "installedAt".into(),
        old.as_ref()
            .and_then(|m| m.get("installedAt"))
            .filter(|v| !v.is_null())
            .cloned()
            .unwrap_or_else(|| json!(now())),
    );
    metadata.insert("updatedAt".into(), json!(now()));
    let mut installed_with = json!({"force": force, "allowDowngrade": allow_downgrade});
    if previous.is_some() && force {
        installed_with["replacedSigner"] = previous
            .as_ref()
            .and_then(|m| m.pointer("/signing/certificates"))
            .cloned()
            .unwrap_or(Value::Null);
    }
    metadata.insert("installedWith".into(), installed_with);
    let meta = Value::Object(metadata);
    write_json(&stage.join("meta.json"), &meta)
        .map_err(|e| PmError::new(EXIT_IO, "io", format!("install failed: {e}")))?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        fs::set_permissions(&stage, fs::Permissions::from_mode(0o755))
            .map_err(|e| PmError::new(EXIT_IO, "io", format!("install failed: {e}")))?;
    }
    let final_path = layout.package_dir(&package);
    let retired = if final_path.exists() {
        let retired = next_temp_dir(&layout.packages, &format!(".old-{package}-"))
            .map_err(|e| PmError::new(EXIT_IO, "io", format!("install failed: {e}")))?;
        if let Err(error) = fs::rename(&final_path, retired.join("pkg")) {
            let _ = fs::remove_dir_all(&stage);
            let _ = fs::remove_dir_all(&retired);
            return Err(PmError::new(
                EXIT_IO,
                "io",
                format!("install failed: {error}"),
            ));
        }
        Some(retired)
    } else {
        None
    };
    if let Err(error) = fs::rename(&stage, &final_path) {
        if let Some(retired) = retired.as_ref() {
            let _ = fs::rename(retired.join("pkg"), &final_path);
            let _ = fs::remove_dir_all(retired);
        }
        let _ = fs::remove_dir_all(&stage);
        return Err(PmError::new(
            EXIT_IO,
            "io",
            format!("install failed: {error}"),
        ));
    }
    if let Some(retired) = retired {
        let _ = fs::remove_dir_all(retired);
    }
    let _ = fs::remove_file(layout.kept_record(&package));
    let mut output = json!({"ok": true, "action": action});
    if let Some(object) = output.as_object_mut() {
        object.extend(
            summary(&meta, layout)
                .as_object()
                .cloned()
                .unwrap_or_default(),
        );
        object.insert(
            "abiSummary".into(),
            meta.pointer("/abiVerdict/summary")
                .cloned()
                .unwrap_or(Value::Null),
        );
        object.insert(
            "previousVersion".into(),
            old.as_ref()
                .and_then(|m| m.get("versionName"))
                .cloned()
                .unwrap_or(Value::Null),
        );
        object.insert("dataKept".into(), json!(previous.is_some()));
    }
    Ok(output)
}

fn uninstall(layout: &Layout, package: &str, keep_data: bool) -> Result<Value, PmError> {
    check_package(package)?;
    let _guard = lock(layout)?;
    let package_dir = layout.package_dir(package);
    let meta = layout.meta(package);
    if !package_dir.exists() {
        return Err(PmError::new(
            EXIT_MISSING,
            "not-installed",
            format!("{package} is not installed"),
        )
        .with("package", json!(package)));
    }
    let data_dir = layout.data_dir(package);
    if keep_data {
        let record = json!({
            "package": package,
            "versionCode": meta.as_ref().and_then(|m| m.get("versionCode")).cloned().unwrap_or(Value::Null),
            "versionName": meta.as_ref().and_then(|m| m.get("versionName")).cloned().unwrap_or(Value::Null),
            "signing": meta.as_ref().and_then(|m| m.get("signing")).cloned().unwrap_or(Value::Null),
            "keptAt": now(),
        });
        write_json(&layout.kept_record(package), &record).map_err(|e| {
            PmError::new(EXIT_IO, "io", format!("uninstall of {package} failed: {e}"))
        })?;
    }
    let remove_result = (|| -> io::Result<()> {
        if !keep_data && data_dir.exists() {
            fs::remove_dir_all(&data_dir)?;
        }
        fs::remove_dir_all(&package_dir)
    })();
    if let Err(error) = remove_result {
        return Err(PmError::new(
            EXIT_IO,
            "io",
            format!("uninstall of {package} failed: {error}"),
        )
        .with("package", json!(package)));
    }
    if !keep_data {
        let _ = fs::remove_file(layout.kept_record(package));
    }
    Ok(json!({
        "ok": true,
        "action": "uninstalled",
        "package": package,
        "dataKept": keep_data,
        "dataDir": data_dir.to_string_lossy(),
    }))
}

fn list_packages(layout: &Layout) -> Value {
    let mut packages = Vec::<Value>::new();
    if let Ok(entries) = fs::read_dir(&layout.packages) {
        let mut entries = entries.filter_map(Result::ok).collect::<Vec<_>>();
        entries.sort_by_key(|entry| entry.file_name());
        for entry in entries {
            let name = entry.file_name().to_string_lossy().into_owned();
            if name.starts_with('.') || check_package(&name).is_err() {
                continue;
            }
            if let Some(meta) = layout.meta(&name) {
                if meta.get("package").and_then(Value::as_str) == Some(&name) {
                    packages.push(summary(&meta, layout));
                }
            }
        }
    }
    let mut kept = Vec::<String>::new();
    if let Ok(entries) = fs::read_dir(&layout.kept) {
        for entry in entries.filter_map(Result::ok) {
            let path = entry.path();
            if path.extension().and_then(|s| s.to_str()) != Some("json") {
                continue;
            }
            if let Some(package) = path.file_stem().and_then(|s| s.to_str()) {
                if check_package(package).is_ok() && layout.data_dir(package).is_dir() {
                    kept.push(package.to_owned());
                }
            }
        }
    }
    kept.sort();
    json!({"ok": true, "packages": packages, "keptData": kept})
}

fn info(layout: &Layout, package: &str) -> Result<Value, PmError> {
    check_package(package)?;
    let meta = layout.meta(package).ok_or_else(|| {
        PmError::new(
            EXIT_MISSING,
            "not-installed",
            format!("{package} is not installed"),
        )
    })?;
    let mut output = summary(&meta, layout)
        .as_object()
        .cloned()
        .unwrap_or_default();
    output.insert("ok".into(), Value::Bool(true));
    output.insert("meta".into(), meta);
    Ok(Value::Object(output))
}

fn usage_error(message: &str) -> i32 {
    eprintln!("{message}\n\n{USAGE}");
    EXIT_INPUT
}

fn next_text(args: &[OsString], index: usize) -> Option<&str> {
    args.get(index).and_then(|s| s.to_str())
}

fn run_main() -> i32 {
    let mut args = env::args_os().skip(1).collect::<Vec<_>>();
    if args.is_empty() {
        return usage_error("a command is required");
    }
    let mut state = None;
    if next_text(&args, 0) == Some("--help") || next_text(&args, 0) == Some("-h") {
        println!("{USAGE}");
        return EXIT_OK;
    }
    if next_text(&args, 0) == Some("--state") {
        if args.len() < 2 {
            return usage_error("--state requires a directory");
        }
        state = Some(PathBuf::from(args.remove(1)));
        args.remove(0);
    } else if let Some(value) = next_text(&args, 0).and_then(|s| s.strip_prefix("--state=")) {
        state = Some(PathBuf::from(value));
        args.remove(0);
    }
    if args.is_empty() {
        return usage_error("a command is required");
    }
    let command = next_text(&args, 0).unwrap_or_default();
    let rest = &args[1..];
    let layout = Layout::new(state_dir(state));
    let result = match command {
        "install" => {
            let mut force = false;
            let mut allow_downgrade = false;
            let mut path = None;
            for item in rest {
                match item.to_str() {
                    Some("--force") => force = true,
                    Some("--allow-downgrade") => allow_downgrade = true,
                    Some(value) if value.starts_with('-') => {
                        return usage_error(&format!("unknown install option: {value}"))
                    }
                    _ if path.is_none() => path = Some(PathBuf::from(item)),
                    _ => return usage_error("install accepts one APK path"),
                }
            }
            let Some(path) = path else {
                return usage_error("install requires an APK path");
            };
            install(&layout, &path, force, allow_downgrade)
        }
        "uninstall" => {
            let mut keep_data = false;
            let mut package = None;
            for item in rest {
                match item.to_str() {
                    Some("--keep-data") => keep_data = true,
                    Some(value) if value.starts_with('-') => {
                        return usage_error(&format!("unknown uninstall option: {value}"))
                    }
                    _ if package.is_none() => package = item.to_str(),
                    _ => return usage_error("uninstall accepts one package name"),
                }
            }
            let Some(package) = package else {
                return usage_error("uninstall requires a package name");
            };
            uninstall(&layout, package, keep_data)
        }
        "list" => {
            if !rest.is_empty() {
                return usage_error("list accepts no arguments");
            }
            Ok(list_packages(&layout))
        }
        "info" => {
            if rest.len() != 1 {
                return usage_error("info requires one package name");
            }
            let Some(package) = rest[0].to_str() else {
                return usage_error("package name must be UTF-8");
            };
            info(&layout, package)
        }
        _ => return usage_error(&format!("unknown command: {command}")),
    };
    match result {
        Ok(value) => {
            println!(
                "{}",
                serde_json::to_string_pretty(&value).unwrap_or_else(|_| "{}".to_owned())
            );
            EXIT_OK
        }
        Err(error) => {
            let status = error.status;
            let output = error.output();
            println!(
                "{}",
                serde_json::to_string(&output).unwrap_or_else(|_| "{\"ok\":false}".to_owned())
            );
            status
        }
    }
}

fn main() {
    std::process::exit(run_main());
}
