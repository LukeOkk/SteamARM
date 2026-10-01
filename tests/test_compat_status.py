"""Installed Valve and custom Proton discovery, without launching Steam."""
import importlib.util
import json
import re
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location(
    "compat_status", REPO / "scripts/compat-status.py")
compat = importlib.util.module_from_spec(spec)
spec.loader.exec_module(compat)


class CompatibilityInventoryTests(unittest.TestCase):
    def test_lists_custom_arm64_tools_and_marks_them_unavailable(self):
        with tempfile.TemporaryDirectory(prefix="steamarm-compat-test-") as temp:
            state = Path(temp)
            steam = state / "steamroot/tmp/fexhome/.local/share/Steam"
            common = steam / "steamapps/common/Proton - Experimental"
            custom = steam / "compatibilitytools.d/Bannerlator Proton (ARM64)"
            for tool, wine_path, manifest_name in (
                    (common, "files/bin/wine", "toolmanifest.vdf"),
                    (custom, "files/bin-arm64/wine", "compatibilitytool.vdf")):
                (tool / wine_path).parent.mkdir(parents=True)
                (tool / wine_path).touch()
                (tool / "proton").touch()
                (tool / manifest_name).write_text('"compatibilitytools" {}')

            with patch.object(compat, "moltenvk", return_value={"path": "", "version": "test"}), \
                    patch.object(compat, "kosmickrisp", return_value={"icd_json": "", "library": "",
                                                                    "version": "", "api_version": "",
                                                                    "os_ok": False, "exports_icd": False}):
                report = compat.inventory(state)

        by_name = {tool["name"]: tool for tool in report["protons"]}
        self.assertTrue(by_name["Proton - Experimental"]["supported"])
        self.assertEqual(by_name["Bannerlator Proton (ARM64)"]["architecture"], "ARM64")
        self.assertFalse(by_name["Bannerlator Proton (ARM64)"]["supported"])
        self.assertEqual(by_name["Bannerlator Proton (ARM64)"]["source"], "Personalizado")

    def test_kosmickrisp_where_the_shim_loads_it(self):
        with tempfile.TemporaryDirectory(prefix="steamarm-compat-test-") as temp:
            root = Path(temp)
            icd = root / "share/vulkan/icd.d"
            icd.mkdir(parents=True)
            library = root / "Cellar/mesa/26.2.3/lib/libvulkan_kosmickrisp.dylib"
            library.parent.mkdir(parents=True)
            library.touch()
            # The shim's fixed path, a link into the Cellar as Homebrew makes it.
            shim_path = root / "lib/libvulkan_kosmickrisp.dylib"
            shim_path.parent.mkdir(parents=True)
            shim_path.symlink_to(library)
            shim_paths = (str(root / "missing/libvulkan_kosmickrisp.dylib"), str(shim_path))
            (icd / "00-invalid.json").write_text("{")
            (icd / "01-lvp.json").write_text(json.dumps({"ICD": {
                "library_path": str(library).replace("kosmickrisp", "lvp")}}))
            absolute = icd / "02-kosmickrisp.json"
            absolute.write_text(json.dumps({"ICD": {"library_path": str(library),
                                                    "api_version": "1.4.0"}}))
            result = compat.kosmickrisp((str(icd),), macos="27.0", load=False, shim_paths=shim_paths)
            self.assertEqual(result["icd_json"], str(absolute))
            self.assertEqual(result["library"], str(library.resolve()))
            self.assertEqual(result["version"], "26.2.3")
            self.assertEqual(result["api_version"], "1.4.0")
            self.assertTrue(result["os_ok"])
            self.assertFalse(result["exports_icd"])
            self.assertEqual(result["found_elsewhere"], "")

            # A manifest is not needed: the shim does not read one.
            absolute.unlink()
            result = compat.kosmickrisp((str(icd),), macos="15.5", load=False, shim_paths=shim_paths)
            self.assertEqual((result["library"], result["icd_json"], result["api_version"]),
                             (str(library.resolve()), "", ""))
            self.assertFalse(result["os_ok"])

    def test_kosmickrisp_elsewhere_is_not_usable(self):
        """A Mesa built with another prefix (a manifest in ~/.local/share/vulkan/icd.d
        naming ~/.local/lib/...): the shim only tries its two paths and would fall
        back to MoltenVK, so it is reported as found elsewhere, never as the library."""
        with tempfile.TemporaryDirectory(prefix="steamarm-compat-test-") as temp:
            root = Path(temp)
            icd = root / "share/vulkan/icd.d"
            icd.mkdir(parents=True)
            library = root / "local/lib/libvulkan_kosmickrisp.dylib"
            library.parent.mkdir(parents=True)
            library.touch()
            (icd / "kosmickrisp.json").write_text(json.dumps({"ICD": {
                "library_path": "../../../local/lib/libvulkan_kosmickrisp.dylib"}}))
            shim_paths = (str(root / "opt/lib/libvulkan_kosmickrisp.dylib"),)
            result = compat.kosmickrisp((str(icd),), macos="27.0", load=False, shim_paths=shim_paths)
            self.assertEqual(result, {"icd_json": "", "library": "", "version": "",
                                      "api_version": "", "os_ok": True, "exports_icd": False,
                                      "found_elsewhere": str(library.resolve())})
            (icd / "kosmickrisp.json").unlink()
            self.assertEqual(compat.kosmickrisp((str(icd),), macos="27.0", load=False,
                                                shim_paths=shim_paths)["found_elsewhere"], "")

    def test_kosmickrisp_a_path_that_does_not_load_is_skipped(self):
        """The shim's loop keeps the first path dlopen accepts; so does detection."""
        with tempfile.TemporaryDirectory(prefix="steamarm-compat-test-") as temp:
            root = Path(temp)
            broken, good = root / "a/libvulkan_kosmickrisp.dylib", root / "b/libvulkan_kosmickrisp.dylib"
            for path in (broken, good):
                path.parent.mkdir(parents=True)
                path.touch()

            class Library:
                vk_icdGetInstanceProcAddr = object()

            def fake_cdll(path):
                if path == str(broken):
                    raise OSError("not a Mach-O")
                return Library()
            with patch.object(compat.ctypes, "CDLL", side_effect=fake_cdll):
                result = compat.kosmickrisp((), macos="27.0", shim_paths=(str(broken), str(good)))
            self.assertEqual(result["library"], str(good.resolve()))
            self.assertTrue(result["exports_icd"])

    def test_kosmickrisp_paths_are_the_shims(self):
        source = (REPO / "shim/gen.py").read_text()
        block = source[source.index("k_kk_paths[]"):]
        block = block[:block.index("};")]
        self.assertEqual(tuple(re.findall(r'"([^"]+)"', block)), compat.SHIM_KK_PATHS)

    def test_shim_marker(self):
        with tempfile.TemporaryDirectory(prefix="steamarm-compat-test-") as temp:
            state = Path(temp)
            path = state / "steamroot/usr/lib/lxrt-emu/libvulkan.so.1"
            self.assertEqual(compat.shim(state), {"path": str(path), "installed": False,
                                                   "icd_selection": False})
            path.parent.mkdir(parents=True)
            path.write_bytes(b"before STEAMARM_VK_ICD after")
            self.assertTrue(compat.shim(state)["icd_selection"])
            path.write_bytes(b"old shim")
            self.assertFalse(compat.shim(state)["icd_selection"])
            self.assertTrue(compat.shim(state)["installed"])

    def test_proton_sync_features(self):
        with tempfile.TemporaryDirectory(prefix="steamarm-compat-test-") as temp:
            state = Path(temp)
            common = state / "steamroot/tmp/fexhome/.local/share/Steam/steamapps/common"
            old = common / "Proton 10.0"
            experimental = common / "Proton - Experimental"
            for folder, ntdll_data, server_data in (
                    (old, b"esync_init fsync_init", b"server"),
                    (experimental, b"fsync_init", b"server /dev/ntsync")):
                ntdll = folder / "files/lib/wine/x86_64-unix/ntdll.so"
                server = folder / "files/bin/wineserver"
                ntdll.parent.mkdir(parents=True)
                server.parent.mkdir(parents=True)
                ntdll.write_bytes(ntdll_data)
                server.write_bytes(server_data)
                (folder / "files/bin/wine").touch()
                (folder / "proton").touch()
                (folder / "toolmanifest.vdf").touch()
            with patch.object(compat, "moltenvk", return_value={"path": "", "version": "test"}), \
                    patch.object(compat, "kosmickrisp", return_value={}):
                tools = compat.inventory(state)["protons"]
            by_name = {tool["name"]: tool for tool in tools}
            self.assertEqual([by_name["Proton 10.0"][key] for key in
                              ("esync", "fsync", "ntsync")], [True, True, False])
            self.assertEqual([by_name["Proton - Experimental"][key] for key in
                              ("esync", "fsync", "ntsync")], [False, True, True])

    def test_presentation_native_x(self):
        with tempfile.TemporaryDirectory(prefix="steamarm-compat-test-") as temp:
            root = Path(temp)
            xq = root / "build/xquartz"
            binary = xq / "SteamARM-X11.app/Contents/MacOS/X11.bin"
            self.assertFalse(compat.presentation(root, xq_root=xq)["native_x"])
            binary.parent.mkdir(parents=True)
            binary.touch()
            self.assertTrue(compat.presentation(root, xq_root=xq)["native_x"])
            xvnc = root / "steamroot/usr/bin/Xvnc"
            xvnc.parent.mkdir(parents=True)
            xvnc.touch()
            self.assertTrue(compat.presentation(root, xq_root=xq)["xvnc"])


    def test_opengl(self):
        with tempfile.TemporaryDirectory(prefix="steamarm-compat-test-") as temp:
            root = Path(temp)
            xq = root / "build/xquartz"
            self.assertEqual(compat.opengl(root, xq_root=xq), {"zink": False, "glx_direct": False})
            for triplet in ("x86_64-linux-gnu", "i386-linux-gnu"):
                dri = root / "steamroot/usr/lib" / triplet / "dri"
                dri.mkdir(parents=True)
                (dri / "zink_dri.so").touch()
            marker = xq / "SteamARM-X11.app/Contents/Resources/steamarm-patches.txt"
            marker.parent.mkdir(parents=True)
            marker.write_text("xquartz-evdev-keycodes.patch\n")
            self.assertEqual(compat.opengl(root, xq_root=xq), {"zink": True, "glx_direct": False})
            marker.write_text("xquartz-evdev-keycodes.patch\nxquartz-glx-mesa-direct.patch\n")
            self.assertTrue(compat.opengl(root, xq_root=xq)["glx_direct"])


if __name__ == "__main__":
    unittest.main()
