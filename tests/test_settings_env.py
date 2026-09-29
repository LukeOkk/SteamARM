"""Settings environment and per-app choices without writing the real state."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[1]


class SettingsEnvironmentTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="steamarm-settings-test-")
        with patch.dict(os.environ, {"STEAMARM_STATE": cls.temp.name}):
            spec = importlib.util.spec_from_file_location(
                "settings_env_test", REPO / "scripts/settings-env.py")
            cls.settings = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(cls.settings)

    @classmethod
    def tearDownClass(cls):
        cls.temp.cleanup()

    def env(self, settings):
        return self.settings.env_from_settings(settings, total=16)

    def test_with_overrides(self):
        original = {"display": "native", "vsync": "game", "extraEnv": {"A": "B"}}
        merged = self.settings.with_overrides(original, {
            "display": "vnc", "vsync": "", "synchronization": "esync",
            "graphicsBackend": 42, "bogus": "x"})
        self.assertEqual(merged, {"display": "vnc", "vsync": "game",
                                  "synchronization": "esync", "extraEnv": {"A": "B"}})
        self.assertEqual(original, {"display": "native", "vsync": "game",
                                    "extraEnv": {"A": "B"}})
        self.assertIsNot(merged, original)
        self.assertEqual(self.settings.with_overrides(original, None), original)
        self.assertIsNot(self.settings.with_overrides(original, None), original)

    def test_vsync(self):
        for value, present, interval in (("on", "FIFO", "1"),
                                         ("off", "IMMEDIATE", "0")):
            with self.subTest(value=value):
                env = self.env({"vsync": value})
                self.assertEqual(env["VKD3D_SWAPCHAIN_PRESENT_MODE"], present)
                self.assertIn("dxgi.syncInterval = " + interval, env["DXVK_CONFIG"])
        for settings in ({"vsync": "game"}, {}):
            self.assertNotIn("VKD3D_SWAPCHAIN_PRESENT_MODE", self.env(settings))

    def test_synchronization(self):
        for choice in ("auto", "wineserver", "fsync", "msync", "bogus"):
            with self.subTest(choice=choice):
                env = self.env({"synchronization": choice})
                self.assertEqual((env.get("PROTON_NO_ESYNC"), env.get("PROTON_NO_FSYNC")),
                                 ("1", "1"))
        self.assertEqual((self.env({}).get("PROTON_NO_ESYNC"),
                          self.env({}).get("PROTON_NO_FSYNC")), ("1", "1"))
        cases = (({"synchronization": "esync"}, None, "1"),
                 ({"esync": True, "fsync": True}, None, None),
                 ({"esync": False, "fsync": True}, "1", None),
                 ({"esync": True, "fsync": False}, None, "1"),
                 ({"synchronization": "auto", "esync": True, "fsync": True}, "1", "1"))
        for settings, esync, fsync in cases:
            with self.subTest(settings=settings):
                env = self.env(settings)
                self.assertEqual((env.get("PROTON_NO_ESYNC"), env.get("PROTON_NO_FSYNC")),
                                 (esync, fsync))

    def test_graphics_backend(self):
        with patch.object(self.settings, "shim_selects_icd", return_value=True):
            self.assertEqual(self.env({"graphicsBackend": "vulkanKosmicKrisp"})[
                "STEAMARM_VK_ICD"], "kosmickrisp")
            for backend in ("auto", "vulkan", "vulkanMoltenVK", "openGLWineD3D"):
                env = self.env({"graphicsBackend": backend})
                self.assertNotIn("STEAMARM_VK_ICD", env)
                self.assertNotIn("PROTON_USE_WINED3D", env)
        with patch.object(self.settings, "shim_selects_icd", return_value=False):
            self.assertNotIn("STEAMARM_VK_ICD", self.env({
                "graphicsBackend": "vulkanKosmicKrisp"}))

    def test_launcher_fallbacks_win(self):
        """The fallback the launcher announced is the one that runs: its variables
        go over the settings and the app's own choices (run-app.sh resolve_app)."""
        environ = {"STEAMARM_GRAPHICS_BACKEND": "vulkanMoltenVK",
                   "STEAMARM_SYNCHRONIZATION": "wineserver", "STEAMARM_DISPLAY": "native",
                   "UNRELATED": "x"}
        self.assertEqual(self.settings.fallback_overrides(environ),
                         {"graphicsBackend": "vulkanMoltenVK", "synchronization": "wineserver"})
        self.assertEqual(self.settings.fallback_overrides({"STEAMARM_GRAPHICS_BACKEND": ""}), {})
        chosen = self.settings.with_overrides(
            {"graphicsBackend": "vulkanKosmicKrisp", "synchronization": "esync"},
            {"synchronization": "esync"})
        applied = self.settings.with_overrides(chosen, self.settings.fallback_overrides(environ))
        with patch.object(self.settings, "shim_selects_icd", return_value=True):
            self.assertEqual(self.env(chosen)["STEAMARM_VK_ICD"], "kosmickrisp")
            self.assertNotIn("PROTON_NO_ESYNC", self.env(chosen))
            env = self.env(applied)
            self.assertNotIn("STEAMARM_VK_ICD", env)
            self.assertEqual(env["PROTON_NO_ESYNC"], "1")

    def test_shim_selects_icd(self):
        with tempfile.TemporaryDirectory(prefix="steamarm-shim-test-") as temp:
            path = Path(temp) / "libvulkan.so.1"
            self.assertFalse(self.settings.shim_selects_icd(path))
            path.write_bytes(b"")
            self.assertFalse(self.settings.shim_selects_icd(path))
            path.write_bytes(b"old shim")
            self.assertFalse(self.settings.shim_selects_icd(path))
            path.write_bytes(b"...STEAMARM_VK_ICD...")
            self.assertTrue(self.settings.shim_selects_icd(path))


if __name__ == "__main__":
    unittest.main()
