#!/usr/bin/env python3
"""Read-only compatibility inventory. Does not launch Steam or change its tools.

MoltenVK uses decimal M*10000+m*100+p for driverVersion. Steam's generic
Vulkan decoder renders 10402 as 0.2.2210; query the library for its version.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path


def moltenvk():
    # Keep in the same order as shim/gen.py: this is the library the shim uses.
    for path in ("/opt/homebrew/lib/libMoltenVK.dylib",
                 "/usr/local/lib/libMoltenVK.dylib"):
        try:
            library = ctypes.CDLL(path)
        except OSError:
            continue
        result = {"path": str(Path(path).resolve()), "version": "desconocida"}
        try:
            query = library.vkGetVersionStringsMVK
            query.argtypes = [ctypes.c_char_p, ctypes.c_uint32,
                              ctypes.c_char_p, ctypes.c_uint32]
            query.restype = None
            mvk, vk = ctypes.create_string_buffer(256), ctypes.create_string_buffer(256)
            query(mvk, len(mvk), vk, len(vk))
            result.update(version=mvk.value.decode(), vulkan=vk.value.decode())
        except AttributeError:
            pass
        return result
    return {"path": "", "version": "no instalado"}


def proton_directories(steam):
    """Find Valve and locally installed compatibility tools without launching them."""
    roots = (
        (steam / "steamapps/common", "Steam"),
        (steam / "compatibilitytools.d", "Personalizado"),
        (steam / "steamapps/compatibilitytools.d", "Personalizado"),
    )
    seen = set()
    for root, source in roots:
        if not root.is_dir():
            continue
        for folder in sorted(root.iterdir(), key=lambda path: path.name.casefold()):
            if not folder.is_dir() or not (folder / "proton").is_file():
                continue
            manifest = next((folder / name for name in
                             ("toolmanifest.vdf", "compatibilitytool.vdf")
                             if (folder / name).is_file()), None)
            if manifest is None:
                continue
            key = folder.name.casefold()
            if key in seen:
                continue
            seen.add(key)
            yield folder, manifest, source


def inventory(state):
    steam = state / "steamroot/tmp/fexhome/.local/share/Steam"
    common = steam / "steamapps/common"
    native_reason = ("Requiere Linux ARM64. Este runtime macOS aún no soporta "
                     "las reservas de memoria baja ni el TEB x18 de Wine ARM64EC.")
    tools = []
    for folder, _manifest, source in proton_directories(steam):
        arm = (folder / "files/bin-arm64").is_dir() or "ARM64" in folder.name.upper()
        wine = folder / ("files/bin-arm64/wine" if arm else "files/bin/wine")
        tools.append({"name": folder.name, "architecture": "ARM64" if arm else "x86_64",
                      "installed": wine.is_file(), "supported": not arm and wine.is_file(),
                      "source": source})
    return {
        "moltenvk": moltenvk(),
        "fex": {"patchedInstalled": (state / "lxrt-root/usr/bin/FEX-gb").is_file(),
                "steamInstalled": (common / "FEX-Emu/FEXCompatTool").is_file()},
        "protons": tools,
        "nativeArmReason": native_reason,
        "note": "Instalado no garantiza compatibilidad con cada juego. FEX de Steam no sustituye al FEX adaptado a macOS.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--json", action="store_true")
    args = parser.parse_args()
    state = Path(os.environ.get("STEAMARM_STATE", str(Path.home() / "SteamARM-roots")))
    report = inventory(state)
    if args.json:
        print(json.dumps(report, ensure_ascii=False))
        return
    print("MoltenVK: " + report["moltenvk"]["version"])
    print("Biblioteca: " + report["moltenvk"]["path"])
    print("FEX macOS: " + ("instalado" if report["fex"]["patchedInstalled"] else "falta"))
    print("FEX de Steam: " + ("instalado; requiere Linux" if report["fex"]["steamInstalled"] else "no instalado"))
    for tool in report["protons"]:
        status = "disponible" if tool["supported"] else "no compatible" if tool["installed"] else "incompleto"
        print(f'{tool["name"]}: {status} ({tool["architecture"]})')
    print(report["nativeArmReason"])
    print(report["note"])


if __name__ == "__main__":
    main()
