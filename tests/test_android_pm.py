"""scripts/android-pm.py on synthetic APKs, in a throw-away state directory.

Run: python3 -m unittest tests/test_android_pm.py      (no network, no guest)
"""
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tests"))
import apk_fixtures as fx  # noqa: E402

spec = importlib.util.spec_from_file_location("android_pm", REPO / "scripts/android-pm.py")
pm = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pm)
session_spec = importlib.util.spec_from_file_location("android_session", REPO / "scripts/android-session.py")
session = importlib.util.module_from_spec(session_spec)
session_spec.loader.exec_module(session)

E, ref = fx.E, fx.ref
CERT_A, CERT_B = fx.fake_cert("SteamARM test key A"), fx.fake_cert("SteamARM test key B")
SHA_A, SHA_B = hashlib.sha256(CERT_A).hexdigest(), hashlib.sha256(CERT_B).hexdigest()
ICON = 0x7F020000
RES = fx.arsc({"string": {0: ("app_name", [(fx.config(), "Juego")])},
               "mipmap": {0: ("ic_launcher", [(fx.config(density=480), "res/i.png")])}})
RES_WEBP = fx.arsc({"string": {0: ("app_name", [(fx.config(), "Juego")])},
                    "mipmap": {0: ("ic_launcher", [(fx.config(density=480), "res/i.webp")])}})


class PMTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="android-pm-test-")
        self.dir = Path(self._tmp.name)
        self.layout = pm.Layout(self.dir / "state")

    def tearDown(self):
        self._tmp.cleanup()

    def apk(self, name="app.apk", package="org.example.game", code=10, version="1.0", signer=(CERT_A,),
            abis=("arm64-v8a",), res=RES, extra_manifest=(), meta=(), v3=None, lineage=(), v31=None):
        files = {"res/i.png": fx.png(144, 144), "res/i.webp": fx.webp_lossy(144, 144)}
        for abi in abis:
            files["lib/%s/libgame.so" % abi] = fx.elf()
        man = fx.manifest(package=package, version_code=code, version_name=version, label=ref(0x7F010000),
                          icon=ref(ICON), extra_manifest=extra_manifest, meta=meta)
        return fx.apk(str(self.dir / name), man, res, files, v1=list(signer) if signer else None,
                      v3=v3, lineage=lineage, v31=v31)

    def refused(self, status, code, fn, *a, **kw):
        with self.assertRaises(pm.PMError) as cm:
            fn(*a, **kw)
        self.assertEqual((cm.exception.status, cm.exception.code), (status, code), cm.exception.message)
        return cm.exception

    # -- install, list, info

    def test_install_layout_and_meta(self):
        out = pm.install(self.layout, self.apk())
        self.assertEqual((out["ok"], out["action"], out["package"], out["label"]),
                         (True, "installed", "org.example.game", "Juego"))
        pdir = self.layout.package_dir("org.example.game")
        self.assertEqual(sorted(p.name for p in pdir.iterdir()), ["base.apk", "icon.png", "meta.json"])
        self.assertEqual((pdir / "base.apk").read_bytes(), Path(self.dir / "app.apk").read_bytes())
        self.assertEqual((pdir / "icon.png").read_bytes(), fx.png(144, 144))
        self.assertTrue(self.layout.data_dir("org.example.game").is_dir())
        meta = json.loads((pdir / "meta.json").read_text())
        self.assertEqual((meta["versionCode"], meta["abiVerdict"]["id"], meta["icon"], meta["schema"]),
                         (10, "arm64", "icon.png", 1))
        self.assertEqual(meta["signing"]["certificates"], [SHA_A])
        self.assertEqual(out["icon"], str(pdir / "icon.png"))
        self.assertEqual(out["abiVerdict"], "arm64")
        listing = pm.list_packages(self.layout)
        self.assertEqual([p["package"] for p in listing["packages"]], ["org.example.game"])
        self.assertEqual(pm.info(self.layout, "org.example.game")["meta"]["label"], "Juego")
        # No stray staging directories.
        self.assertEqual([p.name for p in self.layout.packages.iterdir()], ["org.example.game"])

    def test_abi_verdicts_are_recorded(self):
        cases = {"org.example.a64": (("arm64-v8a", "x86_64"), "arm64"), "org.example.v7": (("armeabi-v7a",), "arm32-only"),
                 "org.example.x86": (("x86",), "x86-only"), "org.example.java": ((), "none")}
        for i, (package, (abis, verdict)) in enumerate(cases.items()):
            out = pm.install(self.layout, self.apk("a%d.apk" % i, package=package, abis=abis))
            self.assertEqual(out["abiVerdict"], verdict, package)

    # -- updates

    def test_update_keeps_data_and_refuses_downgrade(self):
        pm.install(self.layout, self.apk(code=10, version="1.0"))
        data = self.layout.data_dir("org.example.game") / "save.dat"
        data.write_text("progress")
        first = json.loads((self.layout.package_dir("org.example.game") / "meta.json").read_text())
        out = pm.install(self.layout, self.apk("v2.apk", code=11, version="1.1"))
        self.assertEqual((out["action"], out["previousVersion"], out["dataKept"]), ("updated", "1.0", True))
        self.assertEqual(data.read_text(), "progress")
        meta = json.loads((self.layout.package_dir("org.example.game") / "meta.json").read_text())
        self.assertEqual(meta["installedAt"], first["installedAt"])
        self.assertEqual(pm.install(self.layout, self.apk("v2b.apk", code=11, version="1.1"))["action"], "reinstalled")
        self.refused(4, "downgrade", pm.install, self.layout, self.apk("v1.apk", code=10, version="1.0"))
        out = pm.install(self.layout, self.apk("v1.apk", code=10, version="1.0"), allow_downgrade=True)
        self.assertEqual((out["action"], out["versionCode"]), ("updated", 10))
        self.assertEqual(data.read_text(), "progress")

    def test_a_failed_swap_keeps_the_installed_version(self):
        pm.install(self.layout, self.apk(code=10, version="1.0"))
        real_rename = pm.os.rename

        def failing(src, dst):
            if ".install-" in str(src):
                raise OSError(28, "No space left on device")
            return real_rename(src, dst)

        with mock.patch.object(pm.os, "rename", side_effect=failing):
            self.refused(6, "io", pm.install, self.layout, self.apk("v2.apk", code=11, version="1.1"))
        meta = json.loads((self.layout.package_dir("org.example.game") / "meta.json").read_text())
        self.assertEqual(meta["versionCode"], 10)
        self.assertEqual(sorted(p.name for p in self.layout.packages.iterdir()), ["org.example.game"])

    def test_update_with_another_signer_is_refused_unless_forced(self):
        pm.install(self.layout, self.apk())
        data = self.layout.data_dir("org.example.game") / "save.dat"
        data.write_text("progress")
        e = self.refused(4, "different-signer", pm.install, self.layout, self.apk("evil.apk", code=11, signer=(CERT_B,)))
        self.assertIn(SHA_B[:16], e.message)
        meta = json.loads((self.layout.package_dir("org.example.game") / "meta.json").read_text())
        self.assertEqual(meta["versionCode"], 10)                       # untouched
        out = pm.install(self.layout, self.apk("evil.apk", code=11, signer=(CERT_B,)), force=True)
        self.assertEqual(out["action"], "updated")
        meta = json.loads((self.layout.package_dir("org.example.game") / "meta.json").read_text())
        self.assertEqual(meta["installedWith"]["replacedSigner"], [SHA_A])
        self.assertEqual(data.read_text(), "progress")

    def test_key_rotation_through_v3_lineage_is_accepted(self):
        pm.install(self.layout, self.apk(signer=(CERT_A,)))
        rotated = self.apk("rotated.apk", code=11, signer=(CERT_B,), v3=[CERT_B], lineage=[CERT_A, CERT_B])
        self.assertEqual(pm.install(self.layout, rotated)["action"], "updated")

    def test_same_signer_rules(self):
        self.assertTrue(pm.same_signer({"certificates": ["a"]}, {"certificates": ["a"]})[0])
        self.assertFalse(pm.same_signer({"certificates": ["a"]}, {"certificates": ["b"]})[0])
        self.assertTrue(pm.same_signer({"certificates": ["a"]}, {"certificates": ["b"], "lineage": ["a", "b"]})[0])
        self.assertFalse(pm.same_signer({"certificates": []}, {"certificates": ["b"]})[0])

    # -- uninstall

    def test_uninstall_removes_or_keeps_data(self):
        pm.install(self.layout, self.apk())
        (self.layout.data_dir("org.example.game") / "save.dat").write_text("progress")
        out = pm.uninstall(self.layout, "org.example.game", keep_data=True)
        self.assertEqual((out["action"], out["dataKept"]), ("uninstalled", True))
        self.assertFalse(self.layout.package_dir("org.example.game").exists())
        self.assertEqual((self.layout.data_dir("org.example.game") / "save.dat").read_text(), "progress")
        self.assertEqual(pm.list_packages(self.layout)["keptData"], ["org.example.game"])
        # The kept data belongs to the old signer: another one is refused.
        self.refused(4, "different-signer", pm.install, self.layout, self.apk("b.apk", signer=(CERT_B,)))
        out = pm.install(self.layout, self.apk())
        self.assertEqual((out["action"], out["dataKept"]), ("installed", True))
        self.assertEqual(pm.list_packages(self.layout)["keptData"], [])
        pm.uninstall(self.layout, "org.example.game")
        self.assertFalse(self.layout.data_dir("org.example.game").exists())
        self.refused(5, "not-installed", pm.uninstall, self.layout, "org.example.game")

    def test_kept_data_deleted_by_hand_releases_its_signer(self):
        pm.install(self.layout, self.apk())
        pm.uninstall(self.layout, "org.example.game", keep_data=True)
        shutil.rmtree(str(self.layout.data_dir("org.example.game")))
        self.assertEqual(pm.list_packages(self.layout)["keptData"], [])
        out = pm.install(self.layout, self.apk("b.apk", signer=(CERT_B,)))
        self.assertEqual((out["action"], out["dataKept"]), ("installed", False))

    def test_unknown_signer_is_not_handed_the_data(self):
        pm.install(self.layout, self.apk())
        (self.layout.data_dir("org.example.game") / "save.dat").write_text("progress")
        (self.layout.package_dir("org.example.game") / "meta.json").unlink()      # lost or corrupt
        self.refused(4, "different-signer", pm.install, self.layout, self.apk("b.apk", signer=(CERT_B,)))
        self.refused(4, "different-signer", pm.install, self.layout, self.apk("a.apk"))
        out = pm.install(self.layout, self.apk("a.apk"), force=True)
        self.assertEqual((out["action"], out["dataKept"]), ("installed", True))
        # Data nobody accounts for (no package, no kept record) is refused too...
        shutil.rmtree(str(self.layout.package_dir("org.example.game")))
        self.refused(4, "different-signer", pm.install, self.layout, self.apk("b.apk", signer=(CERT_B,)))
        # ...but an empty data directory is nothing to protect.
        shutil.rmtree(str(self.layout.data_dir("org.example.game")))
        self.layout.data_dir("org.example.game").mkdir()
        self.assertEqual(pm.install(self.layout, self.apk("b.apk", signer=(CERT_B,)))["action"], "installed")

    def test_v31_rotated_identity(self):
        pm.install(self.layout, self.apk(signer=(CERT_A,), v3=[CERT_A], v31=[CERT_B]))
        # The next version is signed by the rotated key only.
        self.assertEqual(pm.install(self.layout, self.apk("b.apk", code=11, signer=(CERT_B,)))["action"], "updated")

    def test_an_interrupted_swap_is_recovered(self):
        pm.install(self.layout, self.apk())
        final = self.layout.package_dir("org.example.game")
        old = self.layout.packages / ".old-org.example.game-x"
        old.mkdir()
        final.rename(old / "pkg")                          # killed between the two renames
        stale = self.layout.packages / ".install-org.example.game-y"
        stale.mkdir()
        listing = pm.list_packages(self.layout)
        self.assertEqual([p["package"] for p in listing["packages"]], [])   # list does not lock
        pm.uninstall(self.layout, "org.example.game", keep_data=True)       # takes the lock: recovered first
        self.assertFalse(old.exists() or stale.exists())
        self.assertFalse(final.exists())

    def test_io_failures_leave_a_true_state(self):
        pm.install(self.layout, self.apk(code=10))
        # The data directory cannot be made: nothing is replaced.
        real_mkdir = pm.Path.mkdir

        def no_data(path, *a, **kw):
            if "data" in path.parts:
                raise PermissionError(13, "Permission denied")
            return real_mkdir(path, *a, **kw)
        shutil.rmtree(str(self.layout.data_dir("org.example.game")))
        with mock.patch.object(pm.Path, "mkdir", no_data):
            self.refused(6, "io", pm.install, self.layout, self.apk("v2.apk", code=11))
        self.assertEqual(json.loads((self.layout.package_dir("org.example.game") / "meta.json").read_text())["versionCode"], 10)
        # The data cannot be removed: the app stays installed and the error says so.
        self.layout.data_dir("org.example.game").mkdir(parents=True)
        real_rmtree = pm.shutil.rmtree

        def no_rm(path, *a, **kw):
            if "/data/" in str(path):
                raise PermissionError(13, "Permission denied")
            return real_rmtree(path, *a, **kw)
        with mock.patch.object(pm.shutil, "rmtree", no_rm):
            self.refused(6, "io", pm.uninstall, self.layout, "org.example.game")
        self.assertTrue(self.layout.package_dir("org.example.game").exists())

    # -- what is refused

    def test_package_names_are_validated(self):
        for bad in ("../../etc", "org.example/..", "org.example\n", "single", ".org.example", "org..example", "org.1x", "", None,
                    "a" * 260 + ".b.c"):
            self.refused(2, "bad-package", pm.check_package, bad)
        self.assertEqual(pm.check_package("org.example_1.Game2"), "org.example_1.Game2")
        self.refused(2, "bad-package", pm.uninstall, self.layout, "../evil")
        self.refused(2, "bad-package", pm.install, self.layout, self.apk(package="bad/../name"))

    def test_unsigned_and_lone_split_are_refused(self):
        self.refused(3, "unsigned", pm.install, self.layout, self.apk(signer=None))
        self.refused(3, "split-apk", pm.install, self.layout, self.apk(extra_manifest=[("split", "config.xxhdpi")]))
        self.refused(3, "needs-splits", pm.install, self.layout,
                     self.apk(meta=[("com.android.vending.splits.required", True)]))
        junk = self.dir / "junk.apk"
        junk.write_bytes(b"junk")
        self.refused(2, "not-apk", pm.install, self.layout, str(junk))
        self.assertFalse(self.layout.packages.exists() and any(self.layout.packages.iterdir()))

    def test_bundle_install_update_and_uninstall(self):
        files = fx.split_set(self.dir, CERT_A, package="org.example.game", code=10)
        obb = "Android/obb/org.example.game/main.10.org.example.game.obb"
        xapk = fx.zip_of(str(self.dir / "game.xapk"), {"manifest.json": b"{}", **files, obb: b"game-data"})
        with mock.patch.dict(pm.os.environ, {"LANG": "es_UY.UTF-8"}):
            out = pm.install(self.layout, xapk)
        self.assertEqual((out["package"], out["abiVerdict"]), ("org.example.game", "x86-only"))
        pdir = self.layout.package_dir("org.example.game")
        meta = json.loads((pdir / "meta.json").read_text())
        self.assertEqual(meta["splits"], ["split_config.x86_64.apk", "split_config.xxhdpi.apk", "split_config.es.apk"])
        self.assertEqual(meta["obb"], ["obb/main.10.org.example.game.obb"])
        self.assertEqual((pdir / "base.apk").read_bytes(), files["base.apk"])
        self.assertEqual((pdir / meta["splits"][0]).read_bytes(), files["config.x86_64.apk"])
        self.assertEqual((pdir / meta["obb"][0]).read_bytes(), b"game-data")
        (self.layout.data_dir("org.example.game") / "save").write_text("kept")
        newer = fx.split_set(self.dir, CERT_A, package="org.example.game", code=11)
        apks = fx.zip_of(str(self.dir / "game.apks"), {"toc.pb": b"\0", **{"splits/" + k: v for k, v in newer.items()}})
        self.assertEqual(pm.install(self.layout, apks)["action"], "updated")
        self.assertEqual((self.layout.data_dir("org.example.game") / "save").read_text(), "kept")
        meta = json.loads((pdir / "meta.json").read_text())
        self.assertEqual((meta["versionCode"], meta["obb"]), (11, []))
        self.refused(4, "downgrade", pm.install, self.layout, xapk)
        self.assertEqual(pm.install(self.layout, xapk, allow_downgrade=True)["action"], "updated")
        pm.uninstall(self.layout, "org.example.game")
        self.assertFalse(pdir.exists())
        self.assertFalse(self.layout.data_dir("org.example.game").exists())

    def test_session_falls_back_to_explicit_split_install_and_copies_obb(self):
        files = fx.split_set(self.dir, CERT_A, package="org.example.game", code=10)
        source = fx.zip_of(str(self.dir / "game.xapk"), {"manifest.json": b"{}", **files,
            "Android/obb/org.example.game/main.10.org.example.game.obb": b"game-data"})
        pm.install(self.layout, source)
        root = self.dir / "session-root"
        (root / "data/local/tmp").mkdir(parents=True)
        commands = []

        def guest(_session, argv, timeout=120, ids="root"):
            commands.append(argv)
            command = argv[1]
            if command == "install":
                return 1, "multi-path install failed"
            if command == "install-create":
                return 0, "Success: created install session [23]"
            if command == "install-write":
                return 0, "Success: streamed bytes"
            return 0, "Success"

        with mock.patch.object(session, "ADIR", str(self.layout.root)), \
             mock.patch.object(session, "installed_version", return_value=None), \
             mock.patch.object(session, "guest", side_effect=guest), \
             mock.patch.object(session, "log"):
            self.assertTrue(session.install({"root": str(root)}, "org.example.game"))
        self.assertEqual([c[1] for c in commands], ["install", "install-create"]
                         + ["install-write"] * 4 + ["install-commit"])
        self.assertEqual((root / "data/media/0/Android/obb/org.example.game/main.10.org.example.game.obb").read_bytes(),
                         b"game-data")
        self.assertEqual(list((root / "data/local/tmp").glob("*.apk")), [])

    # -- icons

    def test_webp_icon_is_converted_or_kept(self):
        # Without sips (Linux, or conversion failing) the WebP is kept as it is.
        with mock.patch.object(pm, "find_sips", return_value=None):
            out = pm.install(self.layout, self.apk(res=RES_WEBP))
        self.assertTrue(out["icon"].endswith("icon.webp"))
        self.assertEqual(Path(out["icon"]).read_bytes(), fx.webp_lossy(144, 144))

    # -- the command line

    def test_cli_json_and_exit_statuses(self):
        state = str(self.dir / "cli-state")

        def run(*args):
            p = subprocess.run([sys.executable, str(REPO / "scripts/android-pm.py"), "--state", state] + list(args),
                               capture_output=True, text=True)
            return p.returncode, json.loads(p.stdout)

        code, out = run("install", self.apk())
        self.assertEqual((code, out["ok"], out["action"]), (0, True, "installed"))
        code, out = run("install", self.apk("b.apk", signer=(CERT_B,)))
        self.assertEqual((code, out["ok"], out["code"]), (4, False, "different-signer"))
        code, out = run("list")
        self.assertEqual((code, [p["package"] for p in out["packages"]]), (0, ["org.example.game"]))
        code, out = run("info", "org.example.game")
        self.assertEqual((code, out["meta"]["package"]), (0, "org.example.game"))
        code, out = run("uninstall", "--keep-data", "org.example.game")
        self.assertEqual((code, out["dataKept"]), (0, True))
        code, out = run("info", "org.example.game")
        self.assertEqual((code, out["code"]), (5, "not-installed"))

    def test_state_directory_follows_steamarm_state(self):
        with mock.patch.dict(pm.os.environ, {"STEAMARM_STATE": str(self.dir / "s")}):
            self.assertEqual(pm.state_dir(), self.dir / "s")
        self.assertEqual(pm.state_dir(str(self.dir / "x")), self.dir / "x")


if __name__ == "__main__":
    unittest.main()
