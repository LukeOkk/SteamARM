use serde_json::{json, Map, Value};
use std::collections::BTreeMap;
use std::env;
use std::fs;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::process::{Command, Stdio};

const OVERRIDABLE: &[&str] = &[
    "display",
    "vsync",
    "synchronization",
    "graphicsBackend",
    "scalingFilter",
];
const ARM64_ROOT: &str = "/tmp/lxrt-arm64root";
const X86_ROOT: &str = "/tmp/lxrt-steamroot";

fn load(path: &Path, fallback: Value) -> Value {
    fs::read(path)
        .ok()
        .and_then(|bytes| serde_json::from_slice::<Value>(&bytes).ok())
        .unwrap_or(fallback)
}

fn home_dir() -> PathBuf {
    env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("/"))
}

fn state_dir() -> PathBuf {
    if let Some(value) = env::var_os("STEAMARM_STATE").filter(|v| !v.is_empty()) {
        let value = PathBuf::from(value);
        if value == Path::new("~") {
            return home_dir();
        }
        if let Some(text) = value.to_str() {
            if let Some(rest) = text.strip_prefix("~/") {
                return home_dir().join(rest);
            }
        }
        return value;
    }
    let newer = home_dir().join("Developer/SteamARM-roots");
    let older = home_dir().join("SteamARM-roots");
    if newer.is_dir() || !older.is_dir() {
        newer
    } else {
        older
    }
}

fn string_value(value: &Value) -> String {
    match value {
        Value::Null => "None".to_owned(),
        Value::Bool(true) => "True".to_owned(),
        Value::Bool(false) => "False".to_owned(),
        Value::String(value) => value.clone(),
        Value::Number(value) => value.to_string(),
        _ => value.to_string(),
    }
}

fn obj_value<'a>(object: &'a Value, key: &str) -> Option<&'a Value> {
    object.as_object()?.get(key)
}

fn str_value<'a>(object: &'a Value, key: &str) -> Option<&'a str> {
    obj_value(object, key).and_then(Value::as_str)
}

fn nonempty_env(name: &str) -> Option<String> {
    env::var(name).ok().filter(|value| !value.is_empty())
}

fn app_id_is(value: &Value, id: &str) -> bool {
    str_value(value, "id") == Some(id)
}

fn apps_from(value: Value) -> Vec<Value> {
    match value {
        Value::Array(values) => values,
        Value::Object(mut object) => object
            .remove("apps")
            .and_then(|apps| apps.as_array().cloned())
            .unwrap_or_default(),
        _ => Vec::new(),
    }
}

fn default_steam() -> Value {
    json!({
        "id": "steam",
        "name": "Steam",
        "command": ["/bin/bash", "/tmp/fexhome/.local/share/Steam/steam.sh", "-noverifyfiles"],
        "root": X86_ROOT,
        "fexRootfs": "/",
        "env": {},
        "architecture": "x86_64"
    })
}

fn valid_package(value: &str) -> bool {
    let segments = value.split('.').collect::<Vec<_>>();
    segments.len() >= 2
        && segments.iter().all(|part| {
            let mut bytes = part.bytes();
            bytes
                .next()
                .is_some_and(|first| first.is_ascii_alphabetic())
                && bytes.all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
        })
}

fn valid_identifier(value: &str) -> bool {
    let mut bytes = value.bytes();
    bytes
        .next()
        .is_some_and(|first| first.is_ascii_alphabetic() || first == b'_')
        && bytes.all(|byte| byte.is_ascii_alphanumeric() || byte == b'_')
}

fn shell_quote(value: &str) -> String {
    if value.is_empty() {
        return "''".to_owned();
    }
    if value
        .bytes()
        .all(|byte| byte.is_ascii_alphanumeric() || b"_@%+=:,./-".contains(&byte))
    {
        value.to_owned()
    } else {
        format!("'{}'", value.replace('\'', "'\\''"))
    }
}

fn same_path(left: &str, right: &str) -> bool {
    fn normalized(value: &str) -> PathBuf {
        let path = PathBuf::from(value);
        if let Ok(canonical) = fs::canonicalize(&path) {
            return canonical;
        }
        let mut out = PathBuf::new();
        for component in path.components() {
            match component {
                std::path::Component::CurDir => {}
                std::path::Component::ParentDir => {
                    out.pop();
                }
                other => out.push(other.as_os_str()),
            }
        }
        out
    }
    normalized(left) == normalized(right)
}

fn emit(name: &str, value: &str) {
    println!("{name}={}", shell_quote(value));
}

fn run_json(program: &str, args: &[&str], input: Option<&Value>) -> Result<Value, String> {
    let mut command = Command::new(program);
    command.args(args).stdin(if input.is_some() {
        Stdio::piped()
    } else {
        Stdio::null()
    });
    let mut child = command
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .spawn()
        .map_err(|error| format!("{program}: {error}"))?;
    if let Some(input) = input {
        let bytes = serde_json::to_vec(input).map_err(|error| error.to_string())?;
        child
            .stdin
            .take()
            .ok_or_else(|| format!("{program}: stdin unavailable"))?
            .write_all(&bytes)
            .map_err(|error| format!("{program}: {error}"))?;
    }
    let output = child
        .wait_with_output()
        .map_err(|error| error.to_string())?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).trim().to_owned());
    }
    serde_json::from_slice(&output.stdout).map_err(|error| {
        format!(
            "invalid JSON from {program}: {error}: {}",
            String::from_utf8_lossy(&output.stdout).trim()
        )
    })
}

fn run_text(program: &str, args: &[&str]) -> Result<String, String> {
    let output = Command::new(program)
        .args(args)
        .stdin(Stdio::null())
        .output()
        .map_err(|error| format!("{program}: {error}"))?;
    if !output.status.success() {
        return Err(String::from_utf8_lossy(&output.stderr).trim().to_owned());
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_owned())
}

fn android_reason(app: &Value, pkg: &str) -> Result<(), String> {
    if !valid_package(pkg) {
        return Err("it names no valid package".to_owned());
    }
    let info = obj_value(app, "android").filter(|value| value.is_object());
    let meta = load(
        &state_dir()
            .join("android/packages")
            .join(pkg)
            .join("meta.json"),
        json!({}),
    );
    let meta = meta.as_object().map(|_| &meta);
    let abis = info
        .and_then(|value| obj_value(value, "abis"))
        .filter(|value| value.is_array())
        .or_else(|| meta.and_then(|value| obj_value(value, "abis")))
        .and_then(Value::as_array)
        .map(|values| values.iter().map(string_value).collect::<Vec<_>>());
    let min_sdk = info
        .and_then(|value| obj_value(value, "minSdk"))
        .filter(|value| !value.is_null())
        .or_else(|| meta.and_then(|value| obj_value(value, "minSdk")))
        .map(string_value)
        .unwrap_or_default();
    let Some(abis) = abis else {
        return Err("its ABIs are unknown (reinstall the APK)".to_owned());
    };
    if !min_sdk.is_empty() && !min_sdk.bytes().all(|byte| byte.is_ascii_digit()) {
        return Err(format!(
            "it needs a preview Android (minSdk {min_sdk}); the session is Android 11 (API 30)"
        ));
    }
    if let Ok(min) = min_sdk.parse::<u32>() {
        if min > 30 {
            return Err(format!(
                "it needs API {min}; the session is Android 11 (API 30)"
            ));
        }
    }
    if abis.contains(&"x86_64".to_owned()) || abis.is_empty() {
        return Ok(());
    }
    if abis.contains(&"arm64-v8a".to_owned()) {
        return Err("its native code is arm64-v8a only: Android's arm64 ART does not start on macOS (its heap must be below 4 GiB; docs/ANDROID_RUNTIME_ARCHITECTURE.md), and the session is x86_64 under FEX".to_owned());
    }
    if abis
        .iter()
        .any(|abi| abi == "armeabi-v7a" || abi == "armeabi")
    {
        return Err(
            "its native code is 32-bit ARM only, which Apple silicon does not run".to_owned(),
        );
    }
    if abis.contains(&"x86".to_owned()) {
        return Err(
            "its native code is 32-bit x86 only; the session runs x86_64 and dex-only apps"
                .to_owned(),
        );
    }
    let mut abis = abis;
    abis.sort();
    Err(format!("its native code is for {} only", abis.join(", ")))
}

fn run(
    app_id: &str,
    apps_path: &Path,
    settings_path: &Path,
    builtins_path: &Path,
    shell_output: bool,
) -> Result<(), String> {
    let apps = apps_from(load(apps_path, json!([])));
    let mut builtins = load(builtins_path, json!([]))
        .as_array()
        .cloned()
        .unwrap_or_default();
    if !builtins.iter().any(|value| app_id_is(value, "steam")) {
        builtins.push(default_steam());
    }
    let builtin = builtins.iter().find(|value| app_id_is(value, app_id));
    let is_builtin = builtin.is_some();
    let mut app = builtin
        .cloned()
        .or_else(|| apps.into_iter().find(|value| app_id_is(value, app_id)))
        .ok_or_else(|| {
            format!(
                "run-app: unknown app id '{app_id}' (not in {})",
                apps_path.display()
            )
        })?;

    let mut android_pkg = String::new();
    if str_value(&app, "kind") == Some("android") {
        let info = obj_value(&app, "android").filter(|value| value.is_object());
        let pkg = info
            .and_then(|value| str_value(value, "package"))
            .unwrap_or("")
            .to_owned();
        if let Err(reason) = android_reason(&app, &pkg) {
            return Err(format!(
                "run-app: app '{app_id}' is an Android app SteamARM's Android session cannot run: {reason}"
            ));
        }
        android_pkg = pkg.clone();
        let mut updated = app.as_object().cloned().unwrap_or_default();
        updated.insert("command".into(), json!(["run", pkg]));
        updated.insert("architecture".into(), json!("x86_64"));
        updated.insert(
            "root".into(),
            json!(nonempty_env("ANDROID_SESSION_ROOT")
                .unwrap_or_else(|| "/Volumes/SteamARMAndroid/session".to_owned())),
        );
        app = Value::Object(updated);
    }

    let mut command = obj_value(&app, "command")
        .and_then(Value::as_array)
        .map(|values| values.iter().map(string_value).collect::<Vec<_>>())
        .unwrap_or_default();
    if app_id == "steam"
        && !Path::new("/tmp/lxrt-steamroot/tmp/fexhome/.local/share/Steam/ubuntu12_32/steamui.so")
            .exists()
    {
        command.retain(|value| value != "-noverifyfiles");
    }
    if command.is_empty() {
        return Err(format!("run-app: app '{app_id}' has no command"));
    }

    let arch = str_value(&app, "architecture")
        .unwrap_or("x86_64")
        .to_owned();
    if !["aarch64", "x86_64", "i386"].contains(&arch.as_str()) {
        return Err(format!(
            "run-app: app '{app_id}' has architecture '{arch}' (aarch64, x86_64 or i386)"
        ));
    }
    let mut settings = load(settings_path, json!({}));
    if !settings.is_object() {
        settings = json!({});
    }
    let builtin_overrides = obj_value(&settings, "builtinOverrides")
        .and_then(|value| obj_value(value, app_id))
        .filter(|value| value.is_object());
    if is_builtin {
        if let Some(overrides) = builtin_overrides {
            let mut app_object = app.as_object().cloned().unwrap_or_default();
            let mut merged = obj_value(&app, "overrides")
                .and_then(Value::as_object)
                .cloned()
                .unwrap_or_default();
            if let Some(values) = overrides.as_object() {
                merged.extend(values.clone());
            }
            app_object.insert("overrides".into(), Value::Object(merged));
            app = Value::Object(app_object);
        }
    }

    let mut fallbacks = Map::new();
    if let Some(value) = nonempty_env("STEAMARM_SYNCHRONIZATION") {
        fallbacks.insert("synchronization".into(), json!(value));
    }
    if let Some(value) = nonempty_env("STEAMARM_GRAPHICS_BACKEND") {
        fallbacks.insert("graphicsBackend".into(), json!(value));
    }
    if let Some(object) = settings.as_object_mut() {
        for source in [
            obj_value(&app, "overrides").and_then(Value::as_object),
            Some(&fallbacks),
        ]
        .into_iter()
        .flatten()
        {
            for (key, value) in source {
                if OVERRIDABLE.contains(&key.as_str())
                    && value.as_str().is_some_and(|text| !text.is_empty())
                {
                    object.insert(key.clone(), value.clone());
                }
            }
        }
    }

    let kind = str_value(&app, "kind").unwrap_or("").to_owned();
    let mut app_env = Map::new();
    if kind == "steam" {
        app_env.insert(
            "STEAM_ENABLE_SHADER_CACHE_MANAGEMENT".into(),
            json!(
                nonempty_env("STEAM_ENABLE_SHADER_CACHE_MANAGEMENT").unwrap_or_else(|| "0".into())
            ),
        );
    }
    if let Some(values) = obj_value(&app, "env").and_then(Value::as_object) {
        app_env.extend(values.clone());
    }
    let settings_bin = env::var("STEAMARM_SETTINGS_ENV_BIN")
        .map_err(|_| "run-app: Rust settings engine path is unset".to_owned())?;
    let request = json!({
        "settings": settings.clone(),
        "overrides": obj_value(&app, "overrides").cloned().unwrap_or(json!({})),
        "fallbacks": fallbacks
    });
    let configured = run_json(&settings_bin, &["--env-json-stdin"], Some(&request))
        .map_err(|error| format!("run-app: Rust settings engine failed: {error}"))?;
    if let Some(values) = configured.as_object() {
        app_env.extend(values.clone());
    }

    if kind == "windows" {
        let mut root = str_value(&app, "root").unwrap_or(X86_ROOT).to_owned();
        if root == X86_ROOT {
            root = PathBuf::from(
                run_text(&settings_bin, &["--state-dir"])
                    .map_err(|error| format!("run-app: {error}"))?,
            )
            .join("steamroot")
            .display()
            .to_string();
        }
        let proton_bin = env::var("STEAMARM_PROTON_COMMAND_BIN")
            .map_err(|_| "run-app: Proton command path is unset".to_owned())?;
        let proton = run_json(
            &proton_bin,
            &["--json-stdin"],
            Some(&json!({"app":app.clone(),"root":root})),
        )
        .map_err(|error| format!("run-app: {error}"))?;
        command = proton
            .get("argv")
            .and_then(Value::as_array)
            .map(|values| values.iter().map(string_value).collect())
            .ok_or_else(|| "run-app: Proton command did not return argv".to_owned())?;
        if let Some(values) = proton.get("environment").and_then(Value::as_object) {
            app_env.extend(values.clone());
        }
    }
    if let Some(extra) = nonempty_env("LXRT_EXEC_ARGS_EXTRA") {
        let prior = app_env
            .get("LXRT_EXEC_ARGS")
            .map(string_value)
            .unwrap_or_default();
        app_env.insert(
            "LXRT_EXEC_ARGS".into(),
            json!(if prior.is_empty() {
                extra
            } else {
                format!("{prior};{extra}")
            }),
        );
    }

    let game_only = BTreeMap::from([
        ("LXRT_VK_FPS_LIMIT", "STEAMARM_VK_FPS_LIMIT"),
        ("LXRT_VK_ANISOTROPY", "STEAMARM_VK_ANISOTROPY"),
        ("MTL_HUD_ENABLED", "STEAMARM_METAL_HUD"),
    ]);
    let mut pairs = Vec::new();
    for (key, value) in &app_env {
        let mut key = key.clone();
        if arch == "aarch64" && key.starts_with("FEX_") {
            key = format!("STEAMARM_FEXOPT_{}", &key[4..]);
        }
        if kind == "steam" {
            if let Some(mapped) = game_only.get(key.as_str()) {
                key = (*mapped).to_owned();
            }
        }
        if valid_identifier(&key) {
            pairs.push(format!("{key}={}", string_value(value)));
        }
    }
    pairs.sort();

    let geometry = str_value(&settings, "resolution")
        .filter(|value| {
            let Some((width, height)) = value.split_once('x') else {
                return false;
            };
            (3..=5).contains(&width.len())
                && (3..=5).contains(&height.len())
                && width.bytes().all(|byte| byte.is_ascii_digit())
                && height.bytes().all(|byte| byte.is_ascii_digit())
        })
        .unwrap_or("1600x900");
    let mode = match str_value(&settings, "display") {
        Some("native") => "native",
        Some("vnc") => "vnc",
        _ => "native",
    };
    let name = str_value(&app, "name")
        .filter(|value| !value.is_empty())
        .unwrap_or(app_id);
    let name = name
        .chars()
        .map(|character| {
            if (character as u32) <= 0x1f || character == '\u{7f}' {
                ' '
            } else {
                character
            }
        })
        .collect::<String>();
    let root = str_value(&app, "root")
        .filter(|value| !value.is_empty())
        .unwrap_or(if arch == "aarch64" {
            ARM64_ROOT
        } else {
            X86_ROOT
        });
    if (arch == "aarch64" && same_path(root, X86_ROOT))
        || (arch != "aarch64" && same_path(root, ARM64_ROOT))
    {
        let want = if arch == "aarch64" {
            ARM64_ROOT
        } else {
            X86_ROOT
        };
        return Err(format!("run-app: app '{app_id}' is {arch} but its root is {root}; {arch} programs run in {want}. Install it there."));
    }
    let fex_root = obj_value(&app, "fexRootfs")
        .filter(|value| !value.is_null())
        .map(string_value)
        .unwrap_or_else(|| "/".to_owned());
    let prefix = if kind == "windows" {
        app_env
            .get("STEAM_COMPAT_DATA_PATH")
            .map(string_value)
            .unwrap_or_default()
    } else {
        String::new()
    };

    if shell_output {
        emit("APP_NAME", &name);
        emit("APP_ARCH", &arch);
        emit("APP_ANDROID", &android_pkg);
        emit("APP_KIND", &kind);
        emit("APP_ROOT", root);
        println!("APP_IN_X86_ROOT={}", i32::from(same_path(root, X86_ROOT)));
        emit("APP_FEXROOTFS", &fex_root);
        emit("APP_PREFIX", &prefix);
        emit("GEOMETRY", geometry);
        emit("DMODE", mode);
        println!(
            "APP_ENV=({})",
            pairs
                .iter()
                .map(|value| shell_quote(value))
                .collect::<Vec<_>>()
                .join(" ")
        );
        println!(
            "APP_CMD=({})",
            command
                .iter()
                .map(|value| shell_quote(value))
                .collect::<Vec<_>>()
                .join(" ")
        );
    } else {
        println!(
            "{}",
            serde_json::to_string(&json!({
                "name": name,
                "architecture": arch,
                "androidPackage": android_pkg,
                "kind": kind,
                "root": root,
                "inX86Root": same_path(root, X86_ROOT),
                "fexRootfs": fex_root,
                "prefix": prefix,
                "geometry": geometry,
                "displayMode": mode,
                "environment": pairs,
                "command": command,
            }))
            .map_err(|error| format!("serialize app plan: {error}"))?
        );
    }
    Ok(())
}

fn main() {
    let args = env::args().skip(1).collect::<Vec<_>>();
    if args.len() != 5 || !["--shell", "--json"].contains(&args[0].as_str()) {
        eprintln!(
            "usage: steamarm-app-plan --shell|--json APP_ID APPS_JSON SETTINGS_JSON BUILTIN_APPS_JSON"
        );
        std::process::exit(2);
    }
    if let Err(error) = run(
        &args[1],
        Path::new(&args[2]),
        Path::new(&args[3]),
        Path::new(&args[4]),
        args[0] == "--shell",
    ) {
        eprintln!("{error}");
        std::process::exit(2);
    }
}
