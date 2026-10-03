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
        for choice in ("auto", "wineserver", "msync", "bogus"):
            with self.subTest(choice=choice):
                env = self.env({"synchronization": choice})
                self.assertEqual((env.get("PROTON_NO_ESYNC"), env.get("PROTON_NO_FSYNC")),
                                 ("1", "1"))
        self.assertEqual((self.env({}).get("PROTON_NO_ESYNC"),
                          self.env({}).get("PROTON_NO_FSYNC")), ("1", "1"))
        # Only an explicit choice gets an experimental fast path; the old
        # booleans keep what they always ran with (fsync off).
        cases = (({"synchronization": "esync"}, None, "1"),
                 ({"synchronization": "fsync"}, "1", None),
                 ({"esync": True, "fsync": True}, None, "1"),
                 ({"esync": False, "fsync": True}, "1", "1"),
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
            for backend in ("auto", "vulkan", "vulkanMoltenVK"):
                env = self.env({"graphicsBackend": backend})
                self.assertNotIn("STEAMARM_VK_ICD", env)
                self.assertNotIn("PROTON_USE_WINED3D", env)
                self.assertNotIn("GALLIUM_DRIVER", env)
            # WineD3D: OpenGL from Zink on the Vulkan thunk, MoltenVK.
            env = self.env({"graphicsBackend": "openGLWineD3D"})
            self.assertEqual((env.get("PROTON_USE_WINED3D"), env.get("GALLIUM_DRIVER")), ("1", "zink"))
            self.assertIn("+GL_ARB_vertex_type_2_10_10_10_rev", env.get("MESA_EXTENSION_OVERRIDE", ""))
            self.assertIn("+GL_ARB_texture_buffer_object_rgb32", env.get("MESA_EXTENSION_OVERRIDE", ""))
            self.assertNotIn("STEAMARM_VK_ICD", env)
        with patch.object(self.settings, "shim_selects_icd", return_value=False):
            self.assertNotIn("STEAMARM_VK_ICD", self.env({
                "graphicsBackend": "vulkanKosmicKrisp"}))

    def test_kosmickrisp_prefers_the_steamarm_build(self):
        # scripts/build-kosmickrisp.sh's driver, when there is one: the shim
        # is told its directory. Without one, only the name (Homebrew's).
        chosen = {"graphicsBackend": "vulkanKosmicKrisp"}
        with patch.object(self.settings, "shim_selects_icd", return_value=True):
            with patch.object(self.settings, "kosmickrisp_build_dir", return_value="/b/mesa-kk/out"):
                env = self.env(chosen)
                self.assertEqual(env["STEAMARM_VK_ICD"], "kosmickrisp")
                self.assertEqual(env["STEAMARM_KK_DIR"], "/b/mesa-kk/out")
                self.assertNotIn("STEAMARM_KK_DIR", self.env({"graphicsBackend": "vulkanMoltenVK"}))
            with patch.object(self.settings, "kosmickrisp_build_dir", return_value=""):
                self.assertNotIn("STEAMARM_KK_DIR", self.env(chosen))

    def test_kosmickrisp_build_dir_needs_the_driver_file(self):
        import tempfile
        with tempfile.TemporaryDirectory() as build:
            self.assertEqual(self.settings.kosmickrisp_build_dir(build), "")
            out = os.path.join(build, "mesa-kk", "out")
            os.makedirs(out)
            open(os.path.join(out, "libvulkan_kosmickrisp.dylib"), "w").close()
            self.assertEqual(self.settings.kosmickrisp_build_dir(build), out)

    def test_emulate_modeset_flag(self):
        import io, contextlib, json, tempfile, os
        for value, want in ((True, "on"), (False, "off"), (None, "off")):
            with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False) as f:
                json.dump({} if value is None else {"resolutionScaling": value}, f)
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                self.settings.main(["settings-env.py", "--emulate-modeset", f.name])
            os.unlink(f.name)
            self.assertEqual(out.getvalue().strip(), want)

    def test_steam_ui_acceleration(self):
        self.assertNotIn("LXRT_EXEC_ARGS", self.settings.env_from_settings({"steamUIAcceleration": False}))
        env = self.settings.env_from_settings({})
        self.assertTrue(env["LXRT_EXEC_ARGS"].startswith("steamwebhelper!--type=:--use-gl=angle --use-angle=vulkan"))
        self.assertNotIn(";", env["LXRT_EXEC_ARGS"])

    def test_scaling_filter(self):
        # AUTO by default: the shim's choice, with this chip's render scale
        # (the exact ratio of the mode the policy snaps to).
        m4 = self.settings.apple_chip("Apple M4", 10)
        env = self.settings.env_from_settings({}, chip=m4, display=(1920, 1080))
        self.assertEqual((env["LXRT_VK_SCALER"], env["STEAMARM_RENDER_SCALE"], env["STEAMARM_RENDER_SIZE"]),
                         ("auto", "0.6667", "1280x720"))
        # The render scale does not depend on the filter.
        env = self.settings.env_from_settings({"scalingFilter": "linear"}, chip=m4, display=(1920, 1080))
        self.assertNotIn("LXRT_VK_SCALER", env)
        self.assertEqual(env["STEAMARM_RENDER_SIZE"], "1280x720")
        env = self.settings.env_from_settings({"scalingFilter": "fsr", "fsrSharpness": 40})
        self.assertEqual((env["LXRT_VK_SCALER"], env["LXRT_VK_FSR_SHARPNESS"]), ("fsr", "40"))
        env = self.settings.env_from_settings({"scalingFilter": "fsr", "fsrSharpness": 500})
        self.assertEqual(env["LXRT_VK_FSR_SHARPNESS"], "100")
        env = self.settings.env_from_settings({"scalingFilter": "metalfx"})
        self.assertEqual(env["LXRT_VK_SCALER"], "metalfx")
        self.assertNotIn("LXRT_VK_FSR_SHARPNESS", env)
        self.assertNotIn("LXRT_VK_SCALER", self.settings.env_from_settings({"scalingFilter": "bogus"}))
        merged = self.settings.with_overrides({"scalingFilter": "linear"}, {"scalingFilter": "nearest"})
        self.assertEqual(self.settings.env_from_settings(merged)["LXRT_VK_SCALER"], "nearest")

    def test_sound(self):
        env = self.settings.env_from_settings({})
        self.assertEqual(env["PULSE_SERVER"], "unix:/tmp/pulse/native")
        # Steam's PipeWire (scripts/pipewire.sh) is found by its runtime dir;
        # SDL games stay on PulseAudio even though SDL prefers PipeWire.
        self.assertEqual(env["PIPEWIRE_RUNTIME_DIR"], "/tmp/steamarm-pipewire")
        self.assertEqual((env["SDL_AUDIO_DRIVER"], env["SDL_AUDIODRIVER"]), ("pulseaudio", "pulseaudio"))
        off = self.settings.env_from_settings({"audioBackend": "none"})
        self.assertEqual((off["PULSE_SERVER"], off["SDL_AUDIODRIVER"]), ("none", "dummy"))
        self.assertNotIn("PIPEWIRE_RUNTIME_DIR", off)
        self.assertEqual(off["SDL_AUDIO_DRIVER"], "dummy")

    def test_render_scale(self):
        m4 = self.settings.apple_chip("Apple M4", 10)
        def scale(value):
            env = self.settings.env_from_settings({"renderScale": value}, chip=m4, display=(1920, 1080))
            return env.get("STEAMARM_RENDER_SCALE"), env.get("STEAMARM_RENDER_SIZE")
        self.assertEqual(scale("1.0"), (None, None))         # native: nothing for the shim
        self.assertEqual(scale("0.77"), ("0.7700", "1478x832"))
        self.assertEqual(scale("0.5"), ("0.5000", "960x540"))
        self.assertEqual(scale("bogus"), (None, None))
        self.assertEqual(scale("0.2"), (None, None))         # out of range: native
        self.assertEqual(scale("auto"), ("0.6667", "1280x720"))

    def test_upscaling_policy(self):
        chip = self.settings.apple_chip
        policy = self.settings.upscaling_policy
        self.assertIsNone(chip("Intel(R) Core(TM) i9-9980HK CPU @ 2.40GHz", 0))
        self.assertEqual(policy(None)["filter"], "fsr")
        m4 = chip("Apple M4", 10)
        self.assertEqual((m4["generation"], m4["tier"], m4["gpu_cores"]), (4, "base", 10))
        self.assertEqual(chip("Apple M3 Max", 40)["tier"], "Max")
        # Without the IORegistry's count: the smallest GPU of the tier.
        with patch.object(self.settings, "_run", return_value=""):
            self.assertEqual(chip("Apple M2 Pro")["gpu_cores"], 14)
        cases = [  # chip, GPU cores, display -> scale, render size
            ("Apple M1", 8, (1920, 1080), 0.5, [960, 540]),
            ("Apple M2", 10, (1920, 1080), 0.59, [1152, 648]),
            ("Apple M4", 10, (1920, 1080), 0.67, [1280, 720]),
            ("Apple M4", 10, (2560, 1440), 0.5, [1280, 720]),
            ("Apple M4 Pro", 20, (1920, 1080), 0.77, [1366, 768]),
            ("Apple M4 Pro", 20, (2560, 1440), 0.67, [1600, 900]),
            ("Apple M3 Max", 40, (1920, 1080), 1.0, [1920, 1080]),
            ("Apple M3 Max", 40, (3840, 2160), 0.59, [2304, 1296]),
            ("Apple M2 Ultra", 76, (2560, 1440), 1.0, [2560, 1440]),
        ]
        for brand, cores, display, scale, render in cases:
            got = policy(chip(brand, cores), display)
            self.assertEqual((got["filter"], got["scale"], got["render"]), ("metalfx", scale, render),
                             (brand, cores, display))
        # A display no common mode fits: the scaled size, even.
        self.assertEqual(policy(m4, (1512, 982))["render"], [1164, 756])

    def test_antialiasing(self):
        for value in (2, 4, 8):
            cfg = self.env({"antialiasing": value}).get("DXVK_CONFIG", "")
            self.assertIn("d3d9.forceSwapchainMSAA = %d" % value, cfg)
        for value in (0, 3, 16):
            self.assertNotIn("forceSwapchainMSAA", self.env({"antialiasing": value}).get("DXVK_CONFIG", ""))

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
