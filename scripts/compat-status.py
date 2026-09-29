#!/usr/bin/env python3
"""Read-only compatibility inventory: MoltenVK, KosmicKrisp, Vulkan shim ICD
selection, Proton sync features and display pieces. Launches nothing.

MoltenVK uses decimal M*10000+m*100+p for driverVersion. Steam's generic
Vulkan decoder renders 10402 as 0.2.2210; query the library for its version.
"""
import argparse
import ctypes
import importlib.util
import json
import os
import platform
import re
from pathlib import Path

ICD_DIRS = ("/opt/homebrew/share/vulkan/icd.d", "/opt/homebrew/etc/vulkan/icd.d",
            "/usr/local/share/vulkan/icd.d", "/usr/local/etc/vulkan/icd.d",
            "~/.local/share/vulkan/icd.d", "/etc/vulkan/icd.d")
spec = importlib.util.spec_from_file_location("settings_env", Path(__file__).with_name("settings-env.py"))
settings_env = importlib.util.module_from_spec(spec)
spec.loader.exec_module(settings_env)


def kosmickrisp(icd_dirs=ICD_DIRS, macos=None, load=True):
    version = macos if macos is not None else platform.mac_ver()[0]
    try:
        os_ok = int(version.split(".")[0]) >= 26
    except (ValueError, IndexError):
        os_ok = False
    result = {"icd_json": "", "library": "", "version": "", "api_version": "",
              "os_ok": os_ok, "exports_icd": False}
    for directory in icd_dirs:
        for manifest in sorted(Path(directory).expanduser().glob("*.json")):
            try:
                data = json.loads(manifest.read_text())
                icd = data["ICD"]
                library_path = icd["library_path"]
                if not isinstance(library_path, str) or "kosmickrisp" not in os.path.basename(library_path).lower():
                    continue
            except (OSError, ValueError, KeyError, TypeError):
                continue
            if os.path.isabs(library_path):
                candidates = [library_path]
            elif "/" in library_path:
                candidates = [str(manifest.parent / library_path)]
            else:
                candidates = [str(Path(base) / library_path) for base in
                              ("/opt/homebrew/lib", "/usr/local/lib")]
            for candidate in candidates:
                if not os.path.isfile(candidate):
                    continue
                library = os.path.realpath(candidate)
                match = re.search(r"/Cellar/mesa/([^/]+)/", library)
                result.update(icd_json=str(manifest), library=library,
                              version=match.group(1) if match else "",
                              api_version=str(icd.get("api_version") or ""))
                if load:
                    try:
                        getattr(ctypes.CDLL(library), "vk_icdGetInstanceProcAddr")
                        result["exports_icd"] = True
                    except (OSError, AttributeError):
                        pass
                return result
    return result


def shim(state):
    path = state / "steamroot/usr/lib/lxrt-emu/libvulkan.so.1"
    return {"path": str(path), "installed": path.is_file(),
            "icd_selection": settings_env.file_contains(path, settings_env.ICD_MARKER)}


def presentation(state, xq_root=None):
    xq = xq_root or os.environ.get("XQ_ROOT") or os.path.join(
        os.environ.get("STEAMARM_BUILD", os.path.expanduser("~/SteamARM-build")), "xquartz")
    screen_sharing = any(Path(path).exists() for path in (
        "/System/Applications/Utilities/Screen Sharing.app",
        "/System/Library/CoreServices/Applications/Screen Sharing.app"))
    return {"native_x": (Path(xq) / "SteamARM-X11.app/Contents/MacOS/X11.bin").is_file(),
            "xvnc": (state / "steamroot/usr/bin/Xvnc").is_file(),
            "screen_sharing": screen_sharing}


def proton_sync(folder):
    ntdll = next((folder / name for name in
                  ("files/lib/wine/x86_64-unix/ntdll.so", "files/lib/wine/aarch64-unix/ntdll.so")
                  if (folder / name).is_file()), None)
    wineserver = next((folder / name for name in
                       ("files/bin/wineserver", "files/bin-arm64/wineserver")
                       if (folder / name).is_file()), None)
    return {key: any(path is not None and settings_env.file_contains(path, marker)
                     for path in (ntdll, wineserver)) for key, marker in
            (("esync", b"esync_init"), ("fsync", b"fsync_init"),
             ("ntsync", b"/dev/ntsync"))}


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
                      "source": source, **proton_sync(folder)})
    return {
        "moltenvk": moltenvk(),
        "kosmickrisp": kosmickrisp(),
        "shim": shim(state),
        "presentation": presentation(state),
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
        yes = lambda value: "sí" if value else "no"
        print(f'  esync/fsync/ntsync: {yes(tool["esync"])}/{yes(tool["fsync"])}/{yes(tool["ntsync"])}')
    print(report["nativeArmReason"])
    print(report["note"])
    kk = report["kosmickrisp"]
    label = kk["version"] or "versión desconocida"
    if kk["library"] and not report["shim"]["icd_selection"]:
        label += " (sin selección de ICD en el shim)"
    print("KosmicKrisp: " + (label if kk["library"] else "no instalado"))
    print("Shim Vulkan: selección de ICD " + ("sí" if report["shim"]["icd_selection"] else "no"))
    pres = report["presentation"]
    print("Pantallas: X11 nativo %s, Xvnc %s, Compartir pantalla %s" % tuple(
        "sí" if pres[key] else "no" for key in ("native_x", "xvnc", "screen_sharing")))


if __name__ == "__main__":
    main()
