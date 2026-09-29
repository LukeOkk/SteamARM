"""Windows launch planning: real manifest format, paths and missing runtimes."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("proton_command", REPO / "scripts/proton-command.py")
proton = importlib.util.module_from_spec(spec)
spec.loader.exec_module(proton)


class ProtonLaunchTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="steamarm-proton-test-")
        self.addCleanup(self.tmp.cleanup)
        self.state = Path(self.tmp.name)
        self.root = self.state / "steamroot"
        self.common = self.root / proton.STEAM.lstrip("/") / "steamapps/common"
        self.tool = self.common / "Proton - Experimental"
        (self.tool / "files/bin").mkdir(parents=True)
        (self.tool / "files/bin/wine").touch()
        (self.tool / "proton").touch()
        (self.tool / "toolmanifest.vdf").write_text('"manifest" { "require_tool_appid" "4183110" }')
        self.runtime = self.common / "SteamLinuxRuntime_4/_v2-entry-point"
        self.runtime.parent.mkdir()
        self.runtime.touch()
        self.app = {"id": "app-one", "name": "Windows app", "kind": "windows",
                    "command": ["/opt/apps/my app/app.exe", "argument with spaces", "$(literal)"],
                    "protonTool": self.tool.name}

    def test_preserves_arguments_and_isolates_prefix(self):
        argv, env = proton.resolve(self.app, self.root)
        self.assertEqual(argv[-3:], self.app["command"])
        self.assertIn("--verb=waitforexitandrun", argv)
        self.assertEqual(env["STEAM_COMPAT_DATA_PATH"], "/tmp/fexhome/.steamarm/prefixes/app-one")
        self.assertEqual(env["STEAM_COMPAT_INSTALL_PATH"], "/opt/apps/my app")

    def test_missing_or_unknown_runtime_fails_before_launch(self):
        self.runtime.unlink()
        with self.assertRaisesRegex(ValueError, "Instala SteamLinuxRuntime_4"):
            proton.resolve(self.app, self.root)
        (self.tool / "toolmanifest.vdf").write_text('"require_tool_appid" "999"')
        with self.assertRaisesRegex(ValueError, "999"):
            proton.resolve(self.app, self.root)

    def test_arm64_and_path_escape_are_not_silently_replaced(self):
        for changes in ({"protonTool": "Proton 11.0 (ARM64)"}, {"protonTool": "../other"}, {"id": "../prefix"}):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                proton.resolve({**self.app, **changes}, self.root)

    def test_community_tool_from_compatibilitytools_d_resolves(self):
        tool = self.root / proton.STEAM.lstrip("/") / "compatibilitytools.d/GE-Proton"
        (tool / "files/bin").mkdir(parents=True)
        (tool / "files/bin/wine").touch()
        (tool / "proton").touch()
        (tool / "compatibilitytool.vdf").write_text(
            '"compatibilitytools" { "compat_tools" { "GE-Proton" '
            '{ "require_tool_appid" "4183110" } } }')
        argv, _env = proton.resolve({**self.app, "protonTool": "GE-Proton"}, self.root)
        self.assertIn(proton.STEAM + "/compatibilitytools.d/GE-Proton/proton", argv)
        self.assertTrue(any("SteamLinuxRuntime_4/_v2-entry-point" in arg for arg in argv))

    def test_custom_arm64_tool_is_found_but_never_sent_through_x86_fex(self):
        tool = self.root / proton.STEAM.lstrip("/") / "compatibilitytools.d/Bannerlator Proton (ARM64)"
        (tool / "files/bin-arm64").mkdir(parents=True)
        (tool / "files/bin-arm64/wine").touch()
        (tool / "proton").touch()
        (tool / "compatibilitytool.vdf").write_text('"compatibilitytools" {}')
        with self.assertRaisesRegex(ValueError, "Proton ARM64"):
            proton.resolve({**self.app, "protonTool": tool.name}, self.root)

    def test_launcher_dry_run_resolves_persistent_root_before_bootstrap(self):
        launcher = self.state / "launcher"
        launcher.mkdir()
        (launcher / "apps.json").write_text(json.dumps([self.app]))
        result = subprocess.run(["bash", str(REPO / "scripts/run-app.sh"), "--dry-run", "app-one"],
                                env={**os.environ, "STEAMARM_STATE": str(self.state)},
                                capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("SteamLinuxRuntime_4/_v2-entry-point", result.stdout)
        self.assertIn("app-one", result.stdout)
        self.assertFalse((self.root / "tmp/fexhome/.steamarm").exists())


if __name__ == "__main__":
    unittest.main()
