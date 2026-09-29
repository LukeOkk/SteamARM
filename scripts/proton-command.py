#!/usr/bin/env python3
"""Resolve an installed x86 Proton and its Steam runtime for a Windows app."""
import os
from pathlib import Path
import re

STEAM = "/tmp/fexhome/.local/share/Steam"
RUNTIMES = {
    "1070560": "SteamLinuxRuntime",
    "1391110": "SteamLinuxRuntime_soldier",
    "1628350": "SteamLinuxRuntime_sniper",
    "4183110": "SteamLinuxRuntime_4",
}


def resolve(app, root):
    """Return (argv, environment), without starting anything or writing files."""
    tool = app.get("protonTool") or "Proton - Experimental"
    if tool in (".", "..") or "/" in tool or "\0" in tool:
        raise ValueError("Nombre de Proton inválido")
    app_id = app["id"]
    if not re.fullmatch(r"[A-Za-z0-9_-]+", app_id):
        raise ValueError("Identificador de app inválido")
    command = app.get("command") or []
    if not command or not command[0].startswith("/"):
        raise ValueError("La app Windows necesita una ruta absoluta del invitado")
    root_path = Path(root)
    steam_root = root_path / STEAM.lstrip("/")
    candidates = (steam_root / "steamapps/common/" / tool,
                  steam_root / "compatibilitytools.d" / tool,
                  steam_root / "steamapps/compatibilitytools.d" / tool)
    host_tool = next((candidate for candidate in candidates
                      if candidate.is_dir() and (candidate / "proton").is_file()), None)
    if host_tool is None:
        raise ValueError(f"Instala {tool} desde la biblioteca de herramientas de Steam")
    guest_tool = "/" + str(host_tool.relative_to(root_path)).replace(os.sep, "/")
    if (host_tool / "files/bin-arm64").is_dir() or "ARM64" in tool.upper():
        raise ValueError("Proton ARM64 aún no está implementado en el runtime zero-VM de macOS")
    if not (host_tool / "proton").is_file() or not (host_tool / "files/bin/wine").is_file():
        raise ValueError(f"Instala {tool} desde la biblioteca de herramientas de Steam")
    manifest = next((host_tool / name for name in
                     ("toolmanifest.vdf", "compatibilitytool.vdf")
                     if (host_tool / name).is_file()), None)
    if manifest is None:
        raise ValueError(f"Falta toolmanifest.vdf en {tool}; verifica su instalación en Steam")
    match = re.search(r'"require_tool_appid"\s+"(\d+)"', manifest.read_text())
    argv = [guest_tool + "/proton", "waitforexitandrun", *command]
    if match:
        runtime = RUNTIMES.get(match.group(1))
        if runtime is None:
            raise ValueError(f"Steam runtime {match.group(1)} aún no soportado")
        entry = STEAM + "/steamapps/common/" + runtime + "/_v2-entry-point"
        if not (Path(root) / entry.lstrip("/")).is_file():
            raise ValueError(f"Instala {runtime} desde Steam antes de iniciar esta app")
        argv = ["/bin/bash", entry, "--verb=waitforexitandrun", "--", *argv]
    return argv, {
        "STEAM_COMPAT_CLIENT_INSTALL_PATH": STEAM,
        "STEAM_COMPAT_DATA_PATH": "/tmp/fexhome/.steamarm/prefixes/" + app_id,
        "STEAM_COMPAT_INSTALL_PATH": os.path.dirname(command[0]),
        "SteamAppId": "0",
        "SteamGameId": "0",
    }
