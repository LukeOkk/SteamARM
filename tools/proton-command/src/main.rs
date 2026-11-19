use serde_json::{json, Value};
use std::io::{self, Read, Write};
use std::path::{Path, PathBuf};

const STEAM: &str = "/tmp/fexhome/.local/share/Steam";
const STEAM_REL: &str = "tmp/fexhome/.local/share/Steam";

fn runtime_for(app_id: &str) -> Option<&'static str> {
    match app_id {
        "1070560" => Some("SteamLinuxRuntime"),
        "1391110" => Some("SteamLinuxRuntime_soldier"),
        "1628350" => Some("SteamLinuxRuntime_sniper"),
        "4183110" => Some("SteamLinuxRuntime_4"),
        _ => None,
    }
}

fn string<'a>(object: &'a Value, key: &str) -> Result<&'a str, String> {
    object
        .get(key)
        .and_then(Value::as_str)
        .ok_or_else(|| format!("Falta {key}"))
}

fn guest_path(host: &Path, root: &Path) -> Result<String, String> {
    let relative = host
        .strip_prefix(root)
        .map_err(|_| "La herramienta Proton está fuera de la raíz Steam".to_owned())?;
    let relative = relative
        .components()
        .map(|part| part.as_os_str().to_string_lossy())
        .collect::<Vec<_>>()
        .join("/");
    Ok(format!("/{relative}"))
}

fn required_runtime(manifest: &str) -> Option<&str> {
    let after_key = manifest.split_once("\"require_tool_appid\"")?.1;
    let trimmed = after_key.trim_start();
    if trimmed.len() == after_key.len() {
        return None;
    }
    let value = trimmed.strip_prefix('"')?;
    let digits = value.bytes().take_while(u8::is_ascii_digit).count();
    if digits == 0 || value.as_bytes().get(digits) != Some(&b'"') {
        return None;
    }
    std::str::from_utf8(&value.as_bytes()[..digits]).ok()
}

fn resolve(app: &Value, root: &Path) -> Result<Value, String> {
    let tool = app
        .get("protonTool")
        .and_then(Value::as_str)
        .filter(|value| !value.is_empty())
        .unwrap_or("Proton - Experimental");
    if tool == "." || tool == ".." || tool.contains('/') || tool.contains('\0') {
        return Err("Nombre de Proton inválido".to_owned());
    }
    let app_id = string(app, "id")?;
    if app_id.is_empty()
        || !app_id
            .bytes()
            .all(|byte| byte.is_ascii_alphanumeric() || byte == b'_' || byte == b'-')
    {
        return Err("Identificador de app inválido".to_owned());
    }
    let command = app
        .get("command")
        .and_then(Value::as_array)
        .filter(|values| !values.is_empty())
        .ok_or_else(|| "La app Windows necesita una ruta absoluta del invitado".to_owned())?;
    let command = command
        .iter()
        .map(|value| {
            value
                .as_str()
                .map(str::to_owned)
                .ok_or_else(|| "El comando de la app debe ser una lista de cadenas".to_owned())
        })
        .collect::<Result<Vec<_>, _>>()?;
    if !command[0].starts_with('/') {
        return Err("La app Windows necesita una ruta absoluta del invitado".to_owned());
    }

    let steam_root = root.join(STEAM_REL);
    let candidates = [
        steam_root.join("steamapps/common").join(tool),
        steam_root.join("compatibilitytools.d").join(tool),
        steam_root.join("steamapps/compatibilitytools.d").join(tool),
    ];
    let host_tool = candidates
        .iter()
        .find(|candidate| candidate.is_dir() && candidate.join("proton").is_file())
        .ok_or_else(|| format!("Instala {tool} desde la biblioteca de herramientas de Steam"))?;
    let guest_tool = guest_path(host_tool, root)?;

    if host_tool.join("files/bin-arm64").is_dir() || tool.to_uppercase().contains("ARM64") {
        return Err(
            "Proton ARM64 aún no está implementado en el runtime zero-VM de macOS".to_owned(),
        );
    }
    if !host_tool.join("proton").is_file() || !host_tool.join("files/bin/wine").is_file() {
        return Err(format!(
            "Instala {tool} desde la biblioteca de herramientas de Steam"
        ));
    }

    let manifest = ["toolmanifest.vdf", "compatibilitytool.vdf"]
        .iter()
        .map(|name| host_tool.join(name))
        .find(|path| path.is_file())
        .ok_or_else(|| {
            format!("Falta toolmanifest.vdf en {tool}; verifica su instalación en Steam")
        })?;
    let manifest = std::fs::read_to_string(manifest).map_err(|error| error.to_string())?;

    let mut argv = vec![
        format!("{guest_tool}/proton"),
        "waitforexitandrun".to_owned(),
    ];
    argv.extend(command.iter().cloned());
    if let Some(required_app) = required_runtime(&manifest) {
        let runtime = runtime_for(required_app)
            .ok_or_else(|| format!("Steam runtime {required_app} aún no soportado"))?;
        let entry_guest = format!("{STEAM}/steamapps/common/{runtime}/_v2-entry-point");
        let entry_host = root.join(entry_guest.trim_start_matches('/'));
        if !entry_host.is_file() {
            return Err(format!(
                "Instala {runtime} desde Steam antes de iniciar esta app"
            ));
        }
        let mut wrapped = vec![
            "/bin/bash".to_owned(),
            entry_guest,
            "--verb=waitforexitandrun".to_owned(),
            "--".to_owned(),
        ];
        wrapped.extend(argv);
        argv = wrapped;
    }

    let install_path = Path::new(&command[0])
        .parent()
        .unwrap_or_else(|| Path::new(""))
        .to_string_lossy()
        .into_owned();
    Ok(json!({
        "argv": argv,
        "environment": {
            "STEAM_COMPAT_CLIENT_INSTALL_PATH": STEAM,
            "STEAM_COMPAT_DATA_PATH": format!("/tmp/fexhome/.steamarm/prefixes/{app_id}"),
            "STEAM_COMPAT_INSTALL_PATH": install_path,
            "SteamAppId": "0",
            "SteamGameId": "0"
        }
    }))
}

fn main() {
    let mut input = String::new();
    if let Err(error) = io::stdin().read_to_string(&mut input) {
        eprintln!("{error}");
        std::process::exit(2);
    }
    let request: Value = match serde_json::from_str(&input) {
        Ok(value) => value,
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(2);
        }
    };
    if std::env::args().nth(1).as_deref() != Some("--json-stdin") {
        eprintln!("Usage: steamarm-proton-command --json-stdin");
        std::process::exit(2);
    }
    let app = request.get("app").unwrap_or(&Value::Null);
    let root = request
        .get("root")
        .and_then(Value::as_str)
        .map(PathBuf::from)
        .unwrap_or_default();
    match resolve(app, &root) {
        Ok(plan) => {
            if let Err(error) = serde_json::to_writer(io::stdout().lock(), &plan) {
                eprintln!("{error}");
                std::process::exit(2);
            }
            println!();
        }
        Err(error) => {
            eprintln!("{error}");
            std::process::exit(2);
        }
    }
    let _ = io::stdout().flush();
}
