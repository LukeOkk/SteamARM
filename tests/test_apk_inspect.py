"""scripts/apk-inspect.py on synthetic APKs (tests/apk_fixtures.py).

Run: python3 -m unittest tests/test_apk_inspect.py     (no network, no guest)

The real-APK validation against F-Droid's index is recorded in
benchmarks/stage25-apk-install.txt; no third-party APK is committed.
"""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import zipfile

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tests"))
import apk_fixtures as fx  # noqa: E402

spec = importlib.util.spec_from_file_location("apk_inspect", REPO / "scripts/apk-inspect.py")
ai = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ai)

E, ref, hexint = fx.E, fx.ref, fx.hexint
CERT_A, CERT_B = fx.fake_cert("SteamARM test key A"), fx.fake_cert("SteamARM test key B")
SHA_A, SHA_B = hashlib.sha256(CERT_A).hexdigest(), hashlib.sha256(CERT_B).hexdigest()

# Resource ids of the table below: type 1 string, 2 mipmap, 3 drawable, 4 color, 5 integer.
STR_APP, STR_VERSION = 0x7F010000, 0x7F010001
MIP_ICON, MIP_FG, MIP_VECTOR = 0x7F020000, 0x7F020001, 0x7F020002
DRAW_ALIAS = 0x7F030000
COLOR_BG = 0x7F040000
INT_MIN = 0x7F050000


def table(layout="dense", utf8=True, icon_configs=None):
    icon_configs = icon_configs if icon_configs is not None else [
        (fx.config(density=160), "res/mipmap-mdpi-v4/ic_launcher.png"),
        (fx.config(density=480), "res/mipmap-xxhdpi-v4/ic_launcher.png"),
        (fx.config(density=640), "res/Xy.png"),                       # a shortened path
        (fx.config(density=0xFFFE, sdk=26), "res/mipmap-anydpi-v26/ic_launcher.xml"),
        (fx.config(density=640, night=True), "res/night.png"),
    ]
    return fx.arsc({
        "string": {0: ("app_name", [(fx.config(), "Juego de Prueba"), (fx.config("fr"), "Jeu d'essai"),
                                    (fx.config("en"), "Test Game")]),
                   1: ("version", [(fx.config(), "9.9-ref")])},
        "mipmap": {0: ("ic_launcher", icon_configs),
                   1: ("ic_launcher_foreground", [(fx.config(density=320), "res/fg-xhdpi.webp"),
                                                  (fx.config(density=480), "res/fg-xxhdpi.webp")]),
                   2: ("ic_vector", [(fx.config(density=0xFFFE, sdk=26), "res/vector.xml")])},
        "drawable": {0: ("alias", [(fx.config(), ref(MIP_FG))])},
        "color": {0: ("bg", [(fx.config(), 0xFF00FF00)])},
        "integer": {0: ("min", [(fx.config(), 26)])},
    }, layout=layout, utf8=utf8)


def standard_files():
    return {
        "res/mipmap-mdpi-v4/ic_launcher.png": fx.png(48, 48),
        "res/mipmap-xxhdpi-v4/ic_launcher.png": fx.png(144, 144),
        "res/Xy.png": fx.png(192, 192),
        "res/night.png": fx.png(512, 512),
        "res/mipmap-anydpi-v26/ic_launcher.xml": fx.axml(E("adaptive-icon", [], [
            E("background", [("android:drawable", ref(COLOR_BG))]),
            E("foreground", [("android:drawable", ref(MIP_FG))])])),
        "res/fg-xhdpi.webp": fx.webp_lossy(216, 216),
        "res/fg-xxhdpi.webp": fx.webp_lossy(324, 324),
        "res/vector.xml": fx.axml(E("vector", [("android:name", "v")], [E("path", [("android:name", "p")])])),
        "lib/arm64-v8a/libgame.so": fx.elf(183, True, 0x4000),
        "lib/arm64-v8a/libold.so": fx.elf(183, True, 0x1000),
        "lib/x86_64/libgame.so": fx.elf(62, True, 0x1000),
        "lib/armeabi-v7a/libgame.so": fx.elf(40, False, 0x1000),
    }


class Base(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="apk-inspect-test-")
        self.dir = Path(self._tmp.name)

    def tearDown(self):
        self._tmp.cleanup()

    def build(self, name="app.apk", man=None, res="default", files=None, v1=(CERT_A,), v2=None, v3=None, **kw):
        res = table() if res == "default" else res
        man = man if man is not None else fx.manifest(icon=ref(MIP_ICON), label=ref(STR_APP))
        return fx.apk(str(self.dir / name), man, res, standard_files() if files is None else files,
                      v1=list(v1) if v1 else None, v2=v2, v3=v3, **kw)

    def report(self, path):
        return ai.inspect(path)


class StringPoolTests(Base):
    def test_utf8_and_utf16_with_long_and_non_ascii_strings(self):
        long = "x" * 300 + "ñ"
        for utf8 in (True, False):
            pool_bytes = fx.string_pool(["", "Año", long, "日本"], utf8=utf8)
            pool = ai.StringPool(pool_bytes, 0)
            self.assertEqual([pool.get(i) for i in range(4)], ["", "Año", long, "日本"], utf8)
            self.assertIsNone(pool.get(4))

    def test_modified_utf8_surrogate_pairs(self):
        # U+1F3AE written as two 3-byte surrogates (CESU-8), as older aapt did.
        hi, lo = struct.unpack("<2H", "\U0001F3AE".encode("utf-16-le"))
        cesu = (chr(hi) + chr(lo)).encode("utf-8", "surrogatepass")
        self.assertEqual(ai.decode_utf8(b"Game " + cesu), "Game \U0001F3AE")
        self.assertEqual(ai.decode_utf8(b"bad \xff"), "bad �")
        self.assertEqual(ai.decode_utf8(chr(hi).encode("utf-8", "surrogatepass")), "�")

    def test_offsets_overrunning_the_chunk_are_an_error(self):
        pool_bytes = bytearray(fx.string_pool(["a", "b"]))
        struct.pack_into("<I", pool_bytes, 8, 10_000)      # stringCount
        with self.assertRaises(ai.APKError):
            ai.StringPool(bytes(pool_bytes), 0)


class ManifestTests(Base):
    def test_basic_fields_and_utf16_manifest(self):
        for utf8 in (True, False):
            man = fx.manifest(icon=ref(MIP_ICON), label=ref(STR_APP), utf8=utf8, permissions=[
                "android.permission.INTERNET",
                E("uses-permission", [("android:name", "android.permission.READ_EXTERNAL_STORAGE"),
                                      ("android:maxSdkVersion", 32)]),
                E("uses-permission-sdk-23", [("android:name", "android.permission.CAMERA")])])
            r = self.report(self.build(man=man, res=table(utf8=utf8)))
            self.assertEqual(r["package"], "org.example.app")
            self.assertEqual((r["versionCode"], r["versionName"]), (42, "1.2.3"))
            self.assertEqual((r["minSdk"], r["targetSdk"]), (24, 34))
            self.assertEqual(r["label"], "Juego de Prueba")    # the default configuration wins
            self.assertEqual(r["launcherActivity"], "org.example.app.MainActivity")
            self.assertEqual(r["permissions"], [
                {"name": "android.permission.INTERNET"},
                {"name": "android.permission.READ_EXTERNAL_STORAGE", "maxSdkVersion": 32},
                {"name": "android.permission.CAMERA", "sdk23": True}])
            self.assertTrue(r["hasCode"])
            self.assertEqual(r["format"], "apk")
            self.assertTrue(r["supported"])

    def test_sdk_defaults_and_references(self):
        # No targetSdkVersion: it defaults to minSdkVersion; no uses-sdk: min 1.
        r = self.report(self.build(man=fx.manifest(target_sdk=None, min_sdk=21, label="L")))
        self.assertEqual((r["minSdk"], r["targetSdk"]), (21, 21))
        r = self.report(self.build(man=fx.manifest(uses_sdk=False, label="L")))
        self.assertEqual((r["minSdk"], r["targetSdk"]), (1, 1))
        # minSdkVersion and versionName given as resource references.
        r = self.report(self.build(man=fx.manifest(min_sdk=ref(INT_MIN), version_name=ref(STR_VERSION), label="L")))
        self.assertEqual((r["minSdk"], r["versionName"]), (26, "9.9-ref"))
        # A preview codename stays a string.
        r = self.report(self.build(man=fx.manifest(min_sdk="Baklava", target_sdk=None, label="L")))
        self.assertEqual(r["minSdk"], "Baklava")

    def test_obfuscated_attribute_names_are_read_by_resource_id(self):
        man = fx.manifest(label=ref(STR_APP), icon=ref(MIP_ICON), obfuscate_names=True)
        r = self.report(self.build(man=man))
        self.assertEqual((r["versionCode"], r["minSdk"], r["label"]), (42, 24, "Juego de Prueba"))
        self.assertEqual(r["launcherActivity"], "org.example.app.MainActivity")
        self.assertEqual(r["icon"]["path"], "res/Xy.png")

    def test_launcher_activity_rules(self):
        acts = [
            E("activity", [("android:name", ".Settings")]),                            # no intent filter
            fx.launcher_activity(".Disabled", extra=[("android:enabled", False)]),
            fx.launcher_activity("TvMain", category="android.intent.category.LEANBACK_LAUNCHER"),
            fx.launcher_activity("org.example.app.Alias", tag="activity-alias",
                                 extra=[("android:targetActivity", ".RealMain")]),
        ]
        r = self.report(self.build(man=fx.manifest(activities=acts, label="L")))
        self.assertEqual(r["launcherActivity"], "org.example.app.RealMain")
        self.assertEqual(r["launcherAlias"], "org.example.app.Alias")
        self.assertEqual(r["leanbackLauncherActivity"], "org.example.app.TvMain")
        r = self.report(self.build(man=fx.manifest(activities=[acts[0]], label="L")))
        self.assertIsNone(r["launcherActivity"])

    def test_label_fallbacks(self):
        only_fr_en = fx.arsc({"string": {0: ("app_name", [(fx.config("fr"), "Jeu"), (fx.config("en"), "Game")])}})
        r = self.report(self.build(man=fx.manifest(label=ref(STR_APP)), res=only_fr_en, files={}))
        self.assertEqual(r["label"], "Game")                 # English before other locales
        act = fx.launcher_activity(".Main", extra=[("android:label", "Activity Label")])
        man = fx.axml(E("manifest", [("package", "org.example.nolabel")], [
            E("application", [], [act])]))
        r = self.report(self.build(man=man, res=None, files={}))
        self.assertEqual((r["label"], r["labelSource"]), ("Activity Label", "activity"))
        man = fx.axml(E("manifest", [("package", "org.example.bare")], [E("application", [], [])]))
        r = self.report(self.build(man=man, res=None, files={}))
        self.assertEqual((r["label"], r["labelSource"]), ("org.example.bare", "package"))

    def test_features_gles_and_vulkan(self):
        feats = [
            E("uses-feature", [("android:glEsVersion", hexint(0x00030002)), ("android:required", True)]),
            E("uses-feature", [("android:glEsVersion", hexint(0x00020000))]),
            E("uses-feature", [("android:name", "android.hardware.vulkan.version"),
                               ("android:version", hexint((1 << 22) | (1 << 12))), ("android:required", False)]),
            E("uses-feature", [("android:name", "android.hardware.vulkan.level"), ("android:version", 1)]),
            E("uses-feature", [("android:name", "android.hardware.touchscreen"), ("android:required", False)]),
        ]
        r = self.report(self.build(man=fx.manifest(features=feats, label="L")))
        self.assertEqual(r["glEsVersion"], "3.2")
        self.assertEqual(r["vulkan"], {"version": "1.1.0", "required": False, "level": 1})
        self.assertIn({"name": "android.hardware.touchscreen", "required": False}, r["features"])

    def test_implied_permissions(self):
        self.assertEqual(ai.implied_permissions(["android.permission.WRITE_EXTERNAL_STORAGE"], 30),
                         ["android.permission.READ_EXTERNAL_STORAGE"])
        self.assertEqual(ai.implied_permissions([], 3), [
            "android.permission.READ_EXTERNAL_STORAGE", "android.permission.READ_PHONE_STATE",
            "android.permission.WRITE_EXTERNAL_STORAGE"])
        self.assertEqual(ai.implied_permissions(["android.permission.READ_CONTACTS"], 15),
                         ["android.permission.READ_CALL_LOG"])
        self.assertEqual(ai.implied_permissions(["android.permission.READ_CONTACTS"], 16), [])

    def test_game_flag(self):
        r = self.report(self.build(man=fx.manifest(label="L", app_attrs=[("android:appCategory", 0)])))
        self.assertTrue(r["isGame"])
        r = self.report(self.build(man=fx.manifest(label="L", app_attrs=[("android:isGame", True)])))
        self.assertTrue(r["isGame"])
        r = self.report(self.build(man=fx.manifest(label="L")))
        self.assertFalse(r["isGame"])

    def test_xml_dump_names_references(self):
        with ai.APK(self.build()) as apk:
            text = ai.xml_text(apk.manifest, apk.resources)
        self.assertIn('android:label="@string/app_name"', text)
        self.assertIn('package="org.example.app"', text)


class ResourceTableTests(Base):
    def test_every_entry_layout_resolves(self):
        for layout in ("dense", "sparse", "offset16", "compact"):
            r = self.report(self.build(res=table(layout=layout)))
            self.assertEqual(r["label"], "Juego de Prueba", layout)
            self.assertEqual(r["icon"]["path"], "res/Xy.png", layout)

    def test_ui_mode_type_is_not_the_default(self):
        res = fx.arsc({"string": {0: ("app_name", [(fx.config(ui_mode=0x03), "Coche"),     # car
                                                   (fx.config(ui_mode=0x10), "Día"),       # notnight
                                                   (fx.config(), "Normal")])}})
        r = self.report(self.build(man=fx.manifest(label=ref(STR_APP)), res=res, files={}))
        self.assertEqual(r["label"], "Normal")
        self.assertFalse(ai.Config(fx.config(ui_mode=0x04)).is_default)       # television

    def test_config_parsing(self):
        c = ai.Config(fx.config("es", density=480, sdk=26, night=True))
        self.assertEqual((c.language, c.density, c.sdk, c.night), ("es", 480, 26, True))
        self.assertFalse(c.is_default)
        self.assertTrue(ai.Config(fx.config()).is_default)
        self.assertEqual(ai.Config(fx.config(size=28)).density, 0)    # an old, short config


class IconTests(Base):
    def test_best_density_raster_over_adaptive_xml_and_night(self):
        r = self.report(self.build())
        self.assertEqual(r["icon"], {"path": "res/Xy.png", "density": 640, "format": "png", "width": 192,
                                     "height": 192, "bytes": len(fx.png(192, 192)), "source": "manifest"})

    def test_adaptive_icon_falls_back_to_its_foreground(self):
        only_xml = table(icon_configs=[(fx.config(density=0xFFFE, sdk=26), "res/mipmap-anydpi-v26/ic_launcher.xml")])
        r = self.report(self.build(res=only_xml))
        self.assertEqual((r["icon"]["path"], r["icon"]["source"]), ("res/fg-xxhdpi.webp", "adaptive-foreground"))
        self.assertEqual((r["icon"]["format"], r["icon"]["width"], r["icon"]["height"]), ("webp", 324, 324))

    def test_vector_only_icon_has_no_path_and_says_why(self):
        man = fx.manifest(icon=ref(MIP_VECTOR), label="L")
        files = {k: v for k, v in standard_files().items() if "ic_launcher.png" not in k}
        r = self.report(self.build(man=man, files=files))
        self.assertIsNone(r["icon"]["path"])
        self.assertIn("<vector>", r["icon"]["note"])
        # With a launcher bitmap named as usual, that one is taken, and the note kept.
        r = self.report(self.build(man=man))
        self.assertEqual((r["icon"]["path"], r["icon"]["source"]), ("res/mipmap-xxhdpi-v4/ic_launcher.png", "filename"))
        self.assertIn("<vector>", r["icon"]["note"])

    def test_file_name_fallback_without_resources(self):
        files = {"res/mipmap-hdpi/ic_launcher.png": fx.png(72, 72),
                 "res/mipmap-xxxhdpi/ic_launcher.png": fx.png(192, 192),
                 "res/mipmap-xxxhdpi/ic_launcher_round.png": fx.png(192, 192)}
        r = self.report(self.build(man=fx.manifest(label="L"), res=None, files=files))
        self.assertEqual((r["icon"]["path"], r["icon"]["source"]), ("res/mipmap-xxxhdpi/ic_launcher.png", "filename"))

    def test_reference_chain_to_a_raster(self):
        r = self.report(self.build(man=fx.manifest(icon=ref(DRAW_ALIAS), label="L")))
        self.assertEqual(r["icon"]["path"], "res/fg-xxhdpi.webp")

    def test_image_headers(self):
        self.assertEqual(ai.image_info(fx.png(10, 20)), ("png", 10, 20))
        self.assertEqual(ai.image_info(fx.webp_lossy(30, 40)), ("webp", 30, 40))
        self.assertEqual(ai.image_info(b"nothing")[0], None)

    def test_extract_icon_cli(self):
        path = self.build()
        out = self.dir / "icon.png"
        p = subprocess.run([sys.executable, str(REPO / "scripts/apk-inspect.py"), "--extract-icon", str(out), path],
                           capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stdout + p.stderr)
        self.assertEqual(out.read_bytes(), fx.png(192, 192))
        self.assertEqual(json.loads(p.stdout)["icon"]["extractedTo"], str(out))


class ABITests(Base):
    def test_native_libs_and_alignment(self):
        r = self.report(self.build())
        self.assertEqual(r["abis"], ["arm64-v8a", "armeabi-v7a", "x86_64"])
        self.assertEqual(r["abiVerdict"]["id"], "arm64")
        libs = r["nativeLibs"]
        self.assertEqual((libs["arm64-v8a"]["count"], libs["arm64-v8a"]["minLoadAlign"]), (2, 0x1000))
        self.assertEqual(libs["armeabi-v7a"]["minLoadAlign"], 0x1000)     # a 32-bit ELF
        self.assertEqual(libs["x86_64"]["minLoadAlign"], 0x1000)

    def test_verdicts(self):
        cases = [
            (["arm64-v8a", "armeabi-v7a", "x86", "x86_64"], "arm64", "arm64-v8a"),
            (["armeabi-v7a"], "arm32-only", "armeabi-v7a"),
            (["armeabi"], "arm32-only", "armeabi"),
            (["armeabi", "armeabi-v7a"], "arm32-only", "armeabi-v7a"),
            (["x86_64"], "x86-only", "x86_64"),
            (["x86"], "x86-only", "x86"),
            (["mips"], "unsupported", "mips"),
            ([], "none", None),
        ]
        for abis, verdict, abi in cases:
            v = ai.abi_verdict(abis)
            self.assertEqual((v["id"], v["abi"]), (verdict, abi), abis)
        self.assertIn("AArch32", ai.abi_verdict(["armeabi-v7a"])["summary"])
        self.assertIn("FEX", ai.abi_verdict(["x86"])["summary"])

    def test_java_only(self):
        r = self.report(self.build(files={}))
        self.assertEqual((r["abis"], r["abiVerdict"]["id"]), ([], "none"))


class SigningTests(Base):
    def test_unsigned(self):
        s = self.report(self.build(v1=None))["signing"]
        self.assertEqual((s["v1"], s["v2"], s["v3"], s["certificates"], s["certificateSource"]),
                         (False, False, False, [], None))

    def test_v1_certificate_from_pkcs7(self):
        s = self.report(self.build(v1=(CERT_A,)))["signing"]
        self.assertEqual((s["v1"], s["certificates"], s["certificateSource"]), (True, [SHA_A], "v1"))
        self.assertFalse(s["verified"])

    def test_v2_block(self):
        s = self.report(self.build(v1=(CERT_A,), v2=[CERT_A]))["signing"]
        self.assertEqual((s["v2"], s["blocks"], s["certificateSource"], s["certificates"]),
                         (True, ["v2"], "v2", [SHA_A]))

    def test_v3_wins_and_lineage(self):
        path = self.build(v1=(CERT_A,), v2=[CERT_A], v3=[CERT_B], lineage=[CERT_A, CERT_B],
                          extra_blocks={0x42726577: b"\0" * 12})
        s = self.report(path)["signing"]
        self.assertEqual((s["v3"], s["certificateSource"], s["certificates"]), (True, "v3", [SHA_B]))
        self.assertEqual(s["lineage"], [SHA_A, SHA_B])
        self.assertEqual(s["blocks"], ["verity-padding", "v2", "v3"])

    def test_v1_signer_is_the_certificate_its_signerinfo_names(self):
        signer, extra = fx.x509ish("App key", 7), fx.x509ish("Intermediate", 9)
        s = self.report(self.build(v1=(extra, signer), v1_signer=signer))["signing"]
        self.assertEqual(s["certificates"], [hashlib.sha256(signer).hexdigest()])
        # Without a SignerInfo to match, every certificate is kept.
        s = self.report(self.build(name="b.apk", v1=(extra, signer)))["signing"]
        self.assertEqual(len(s["certificates"]), 2)

    def test_v31_rotated_signer_is_reported(self):
        s = self.report(self.build(v1=(CERT_A,), v3=[CERT_A], v31=[CERT_B]))["signing"]
        self.assertEqual((s["v31"], s["certificates"], s["rotatedCertificates"]), (True, [SHA_A], [SHA_B]))
        self.assertIn("v3.1", s["blocks"])

    def test_zip_still_reads_with_a_signing_block(self):
        path = self.build(v2=[CERT_A])
        with zipfile.ZipFile(path) as z:
            self.assertIn("AndroidManifest.xml", z.namelist())


class SplitAndBundleTests(Base):
    def test_split_hints(self):
        man = fx.manifest(label="L", extra_manifest=[("split", "config.arm64_v8a")])
        sp = self.report(self.build(man=man))["splits"]
        self.assertTrue(sp["isSplit"])
        self.assertEqual(sp["split"], "config.arm64_v8a")
        man = fx.manifest(label="L", meta=[("com.android.vending.splits.required", True)])
        sp = self.report(self.build(man=man))["splits"]
        self.assertTrue(sp["needsSplits"])
        self.assertFalse(sp["isSplit"])
        self.assertFalse(self.report(self.build())["splits"]["needsSplits"])

    def test_bundles_are_recognised_and_refused(self):
        base = self.build("base.apk")
        data = Path(base).read_bytes()
        cases = {
            "game.xapk": ({"manifest.json": b"{}", "org.example.app.apk": data, "config.arm64_v8a.apk": data}, "xapk"),
            "game.apks": ({"toc.pb": b"\0", "splits/base-master.apk": data}, "apks"),
            "game.apkm": ({"info.json": b"{}", "base.apk": data}, "apkm"),
            "game.aab": ({"BundleConfig.pb": b"\0", "base/manifest/AndroidManifest.xml": b"\0"}, "aab"),
            "renamed.zip": ({"a.apk": data, "b.apk": data}, "apk-bundle"),
        }
        for name, (files, fmt) in cases.items():
            path = fx.zip_of(str(self.dir / name), files)
            with self.assertRaises(ai.UnsupportedBundle) as cm:
                ai.inspect(path)
            self.assertEqual(cm.exception.format, fmt, name)
        p = subprocess.run([sys.executable, str(REPO / "scripts/apk-inspect.py"), str(self.dir / "game.xapk")],
                           capture_output=True, text=True)
        self.assertEqual(p.returncode, 3)
        out = json.loads(p.stdout)
        self.assertEqual((out["supported"], out["format"]), (False, "xapk"))
        self.assertIn("split", out["reason"])


class RobustnessTests(Base):
    def cli(self, path):
        p = subprocess.run([sys.executable, str(REPO / "scripts/apk-inspect.py"), str(path)],
                           capture_output=True, text=True)
        return p.returncode, json.loads(p.stdout)

    def test_not_a_zip_and_no_manifest(self):
        junk = self.dir / "junk.apk"
        junk.write_bytes(b"not a zip at all")
        self.assertEqual(self.cli(junk)[0], 2)
        empty = fx.zip_of(str(self.dir / "empty.apk"), {"readme.txt": b"hi"})
        code, out = self.cli(empty)
        self.assertEqual(code, 2)
        self.assertIn("AndroidManifest.xml", out["error"])

    def test_truncated_and_corrupt_manifests(self):
        good = fx.manifest(label="L")
        for bad in (good[:40], good[:len(good) // 2], b"\x03\x00\x08\x00\xff\xff\xff\x7f", b"\x01\x00" + good[2:]):
            path = self.build(name="bad.apk", man=bad)
            with self.assertRaises(ai.APKError):
                ai.inspect(path)
            self.assertEqual(self.cli(path)[0], 2)

    def test_deep_nesting_is_not_a_recursion_error(self):
        deep = E("leaf", [("android:name", "x")])
        for _ in range(5000):
            deep = E("inset", [], [deep])
        files = dict(standard_files())
        limit = sys.getrecursionlimit()
        sys.setrecursionlimit(20000)          # the fixture writer recurses; the parser must not
        try:
            files["res/mipmap-anydpi-v26/ic_launcher.xml"] = fx.axml(E("adaptive-icon", [], [deep]))
        finally:
            sys.setrecursionlimit(limit)
        only_xml = table(icon_configs=[(fx.config(density=0xFFFE, sdk=26), "res/mipmap-anydpi-v26/ic_launcher.xml")])
        r = self.report(self.build(res=only_xml, files={k: v for k, v in files.items() if "ic_launcher.png" not in k}))
        self.assertIsNone(r["icon"]["path"])
        with ai.APK(self.build(name="x.apk")) as apk:
            root = ai.parse_axml(files["res/mipmap-anydpi-v26/ic_launcher.xml"])
            self.assertEqual(sum(1 for _ in root.iter()), 5002)
            self.assertEqual(ai.xml_text(root).count("</inset>"), 5000)

    def test_a_file_gone_before_its_hash_is_an_apk_error(self):
        path = self.build()
        real = ai.sha256_file

        def gone(p):
            Path(p).unlink()
            return real(p)
        ai.sha256_file = gone
        try:
            with self.assertRaises(ai.APKError):
                ai.inspect(path)
        finally:
            ai.sha256_file = real

    def test_corrupt_resource_table_keeps_the_manifest(self):
        path = self.build(res=table()[:100])
        r = ai.inspect(path)
        self.assertEqual(r["package"], "org.example.app")
        self.assertIn("resourcesError", r)

    def test_unknown_compression_method_reads_as_stored(self):
        man = fx.manifest(label="Stored")
        path = self.dir / "method.apk"
        with zipfile.ZipFile(path, "w", zipfile.ZIP_STORED) as z:
            z.writestr("AndroidManifest.xml", man)
        data = bytearray(path.read_bytes())
        # Method 0x3141 in the local header and in the central directory entry.
        struct.pack_into("<H", data, 8, 0x3141)
        cd = data.rfind(b"PK\x01\x02")
        struct.pack_into("<H", data, cd + 10, 0x3141)
        path.write_bytes(bytes(data))
        self.assertEqual(ai.inspect(str(path))["label"], "Stored")


    def test_encryption_flag_is_ignored_and_bad_deflate_is_an_apk_error(self):
        man = fx.manifest(label="Flagged")
        path = self.dir / "flag.apk"
        with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
            z.writestr("AndroidManifest.xml", man)
        data = bytearray(path.read_bytes())
        # General purpose bit 0 ("encrypted") in the local and central headers.
        data[6] |= 1
        cd = data.rfind(b"PK\x01\x02")
        data[cd + 8] |= 1
        path.write_bytes(bytes(data))
        self.assertEqual(ai.inspect(str(path))["label"], "Flagged")
        # Garbage where the deflate stream is: an APKError, nothing else.
        data[6] &= ~1
        data[cd + 8] &= ~1
        start = 30 + len("AndroidManifest.xml")
        data[start:start + 40] = b"\xff" * 40
        path.write_bytes(bytes(data))
        with self.assertRaises(ai.APKError):
            ai.inspect(str(path))


class FDroidCompareTests(Base):
    def test_matches_an_index_entry_by_file_hash(self):
        path = self.build(files={"lib/arm64-v8a/libx.so": fx.elf()},
                          man=fx.manifest(label="L", permissions=["android.permission.WRITE_EXTERNAL_STORAGE"]))
        r = ai.inspect(path)
        index = {"packages": {"org.example.app": {"metadata": {"name": {"en-US": "Example"}}, "versions": {
            "x": {"file": {"sha256": r["sha256"], "size": r["size"]}, "manifest": {
                "versionCode": 42, "versionName": "1.2.3", "usesSdk": {"minSdkVersion": 24, "targetSdkVersion": 34},
                "nativecode": ["arm64-v8a"], "signer": {"sha256": [SHA_A]},
                "usesPermission": [{"name": "android.permission.WRITE_EXTERNAL_STORAGE"},
                                   {"name": "android.permission.READ_EXTERNAL_STORAGE"}]}}}}}}
        idx = self.dir / "index-v2.json"
        idx.write_text(json.dumps(index))
        out = ai.fdroid_compare(r, str(idx))
        self.assertEqual(out["mismatched"], {})
        self.assertEqual(out["fdroidName"], "Example")
        index["packages"]["org.example.app"]["versions"]["x"]["manifest"]["versionCode"] = 43
        idx.write_text(json.dumps(index))
        self.assertEqual(ai.fdroid_compare(r, str(idx))["mismatched"]["versionCode"], {"ours": 42, "fdroid": 43})


if __name__ == "__main__":
    unittest.main()
