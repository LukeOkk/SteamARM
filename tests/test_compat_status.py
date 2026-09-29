"""Installed Valve and custom Proton discovery, without launching Steam."""
import importlib.util
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

            with patch.object(compat, "moltenvk", return_value={"path": "", "version": "test"}):
                report = compat.inventory(state)

        by_name = {tool["name"]: tool for tool in report["protons"]}
        self.assertTrue(by_name["Proton - Experimental"]["supported"])
        self.assertEqual(by_name["Bannerlator Proton (ARM64)"]["architecture"], "ARM64")
        self.assertFalse(by_name["Bannerlator Proton (ARM64)"]["supported"])
        self.assertEqual(by_name["Bannerlator Proton (ARM64)"]["source"], "Personalizado")


if __name__ == "__main__":
    unittest.main()
