"""scripts/android-gapps.py on synthetic GApps packages built here.

Run: python3 -m unittest tests/test_android_gapps.py    (no network, no guest)

No Google file is used or committed: every "Play services", "GSF" or "Play
Store" below is a few-hundred-byte APK made by tests/apk_fixtures.py that only
carries the package name, laid out as MindTheGapps and OpenGApps lay out their
zips (docs/PLAY_STORE_SETUP.md, "Package layouts").
"""
import ast
import contextlib
import hashlib
import importlib.util
import io
import json
import lzma
import os
from pathlib import Path
import sqlite3
import stat
import struct
import sys
import tarfile
import tempfile
import unittest
from unittest import mock
import zipfile
import zlib

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / "tests"))
import apk_fixtures as fx  # noqa: E402

spec = importlib.util.spec_from_file_location("android_gapps", REPO / "scripts/android-gapps.py")
ga = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ga)

CERT = fx.fake_cert("SteamARM test signer")
GMS, GSF, VENDING = "com.google.android.gms", "com.google.android.gsf", "com.android.vending"
ELF_MACHINE = {"arm64-v8a": (183, True), "armeabi-v7a": (40, False), "x86_64": (62, True), "x86": (3, False)}

PRIVAPP = b"""<?xml version="1.0" encoding="utf-8"?>
<permissions>
    <privapp-permissions package="com.android.vending">
        <permission name="android.permission.INSTALL_PACKAGES"/>
        <permission name="android.permission.DELETE_PACKAGES"/>
    </privapp-permissions>
    <privapp-permissions package="com.google.android.gms">
        <permission name="android.permission.READ_PRIVILEGED_PHONE_STATE"/>
        <deny-permission name="android.permission.MANAGE_USERS"/>
    </privapp-permissions>
    <privapp-permissions package="com.google.android.googlequicksearchbox">
        <permission name="android.permission.CAPTURE_AUDIO_HOTWORD"/>
    </privapp-permissions>
    <privapp-permissions package="com.google.android.setupwizard">
        <permission name="android.permission.MANAGE_USERS"/>
    </privapp-permissions>
</permissions>
"""
PRIVAPP_SE = b"""<permissions>
    <privapp-permissions package="com.google.android.gsf">
        <permission name="android.permission.WRITE_SECURE_SETTINGS"/>
    </privapp-permissions>
</permissions>
"""
DEFAULTS = b"""<exceptions>
    <exception package="com.google.android.gms">
        <permission name="android.permission.ACCESS_FINE_LOCATION" fixed="false"/>
        <permission name="android.permission.READ_CONTACTS" fixed="false"/>
    </exception>
</exceptions>
"""
SYSCONFIG = b"""<config>
    <feature name="com.google.android.feature.GOOGLE_BUILD"/>
    <allow-in-power-save package="com.google.android.gms"/>
    <system-user-whitelisted-app package="com.android.vending"/>
</config>
"""
DIALER_LIB = b"""<permissions>
    <library name="com.google.android.dialer.support"
             file="/product/framework/com.google.android.dialer.support.jar"/>
</permissions>
"""
MAPS_LIB = b"""<permissions>
    <library name="com.google.android.maps" file="/system/framework/com.google.android.maps.jar"/>
</permissions>
"""


def apk(package, version="1.0", code=1, min_sdk=21, abis=(), permissions=()):
    """A tiny signed APK that only names its package (no Google code)."""
    with tempfile.TemporaryDirectory() as d:
        path = os.path.join(d, "a.apk")
        files = {}
        for abi in abis:
            machine, is64 = ELF_MACHINE[abi]
            files["lib/%s/libstub.so" % abi] = fx.elf(machine=machine, is64=is64)
        man = fx.manifest(package=package, version_code=code, version_name=version, min_sdk=min_sdk,
                          target_sdk=30, label=package, permissions=list(permissions))
        fx.apk(path, man, None, files, v1=[CERT])
        return Path(path).read_bytes()


def lzip(data, dict_log=20, frac=0):
    """One lzip member, as lzip(1) writes it (header, LZMA stream, trailer)."""
    base = 1 << dict_log
    dict_size = base - (base // 16) * frac
    body = lzma.compress(data, format=lzma.FORMAT_RAW, filters=[
        {"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": 3, "lp": 0, "pb": 2}])
    member = b"LZIP\x01" + bytes([(frac << 5) | dict_log]) + body
    return member + struct.pack("<IQQ", zlib.crc32(data), len(data), len(member) + 20)


def tar_of(members):
    """members: [(name, bytes)] or [(name, TarInfo type, linkname)]."""
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.GNU_FORMAT) as t:
        for m in members:
            ti = tarfile.TarInfo(m[0])
            ti.mtime = 0
            if len(m) == 2:                    # (name, data); tarfile's type codes are bytes too
                ti.size = len(m[1])
                t.addfile(ti, io.BytesIO(m[1]))
            else:
                ti.type, ti.linkname = m[1], m[2]
                t.addfile(ti)
    return buf.getvalue()


def write_zip(path, files, links=()):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name, data in files.items():
            z.writestr(name, data if isinstance(data, bytes) else data.encode())
        for name, target in links:
            info = zipfile.ZipInfo(name)
            info.create_system = 3
            info.external_attr = (stat.S_IFLNK | 0o777) << 16
            z.writestr(info, target)
    return str(path)


def mindthegapps(path, arch="aarch64", version="30", nice="11.0.0", gms_abis=("arm64-v8a",), build_prop=True,
                 privapp=PRIVAPP, setup_wizard=False, extra=None, links=()):
    files = {}
    if build_prop:
        files["build.prop"] = "arch=%s\nversion=%s\nversion_nice=%s\n" % (arch, version, nice)
    files.update({
        "META-INF/com/google/android/update-binary": "#!/sbin/sh\n",
        "META-INF/com/google/android/updater-script": "# installer\n",
        "toybox": b"\x7fELF stand-in",
        "system/addon.d/addond_head": "#!/sbin/sh\n",
        "system/product/priv-app/PrebuiltGmsCore/PrebuiltGmsCore.apk": apk(
            GMS, "21.39.18", 213918000, abis=gms_abis,
            permissions=["android.permission.READ_PRIVILEGED_PHONE_STATE", "android.permission.INTERNET"]),
        "system/system_ext/priv-app/GoogleServicesFramework/GoogleServicesFramework.apk": apk(
            GSF, "11-6800000", 30, permissions=["android.permission.WRITE_SECURE_SETTINGS"]),
        "system/product/priv-app/Phonesky/Phonesky.apk": apk(
            VENDING, "27.1.16", 82711600,
            permissions=["android.permission.INSTALL_PACKAGES", "android.permission.INTERNET"]),
        "system/product/priv-app/Velvet/Velvet.apk": apk("com.google.android.googlequicksearchbox", "12.0"),
        "system/product/app/GoogleCalendarSyncAdapter/GoogleCalendarSyncAdapter.apk": apk(
            "com.google.android.syncadapters.calendar", "11"),
        "system/product/app/MarkupGoogle/MarkupGoogle.apk": apk("com.google.android.markup", "1.0"),
        "system/product/app/MarkupGoogle/lib/arm64/libsketchology_native.so": fx.elf(),
        "system/product/lib64/libjni_latinimegoogle.so": fx.elf(),
        "system/product/etc/permissions/privapp-permissions-google-p.xml": privapp,
        "system/system_ext/etc/permissions/privapp-permissions-google-se.xml": PRIVAPP_SE,
        "system/product/etc/permissions/com.google.android.dialer.support.xml": DIALER_LIB,
        "system/product/etc/default-permissions/default-permissions-google.xml": DEFAULTS,
        "system/product/etc/sysconfig/google.xml": SYSCONFIG,
        "system/product/framework/com.google.android.dialer.support.jar": b"PK stand-in jar",
    })
    if setup_wizard:
        files["system/product/priv-app/SetupWizardPrebuilt/SetupWizardPrebuilt.apk"] = apk(
            "com.google.android.setupwizard", "2.0", permissions=["android.permission.MANAGE_USERS"])
    files.update(extra or {})
    return write_zip(path, files, links)


def opengapps(path, arch="x86_64", sdk="30", compression="lz", gms_abis=("x86_64",), extra_archives=None,
              raw_archives=None):
    """extra_archives: {"GApps/<app>": [(path under <app>/, bytes)]}; raw_archives: the same with whole
    member names (for hostile ones)."""
    suffix = {"lz": ".tar.lz", "xz": ".tar.xz", "none": ".tar"}[compression]
    pack = {"lz": lzip, "xz": lambda b: lzma.compress(b, format=lzma.FORMAT_XZ), "none": lambda b: b}[compression]
    files = {
        "g.prop": "# begin addon properties\nro.addon.type=gapps\nro.addon.arch=%s\nro.addon.sdk=%s\n"
                  "ro.addon.platform=11.0\nro.addon.open_type=pico\nro.addon.open_version=20220503\n"
                  "# end addon properties\n" % (arch, sdk),
        "installer.sh": "#!/sbin/sh\n", "bkup_tail.sh": "#!/sbin/sh\n", "gapps-remove.txt": "",
        "app_densities.txt": "", "app_sizes.txt": "", "busybox-x86": b"\x7fELF stand-in",
        "tar-x86": b"\x7fELF stand-in",
        "META-INF/com/google/android/update-binary": "#!/sbin/sh\n",
        "META-INF/com/google/android/updater-script": "# Dummy file; update-binary is a shell script.\n",
    }
    garch = {"x86_64": "x86_64", "x86": "x86", "arm64": "arm64"}[arch]
    archives = {
        "Core/defaultetc-common": [
            ("common/etc/permissions/privapp-permissions-google.xml", PRIVAPP + b""),
            ("common/etc/default-permissions/default-permissions.xml", DEFAULTS),
            ("common/etc/sysconfig/google.xml", SYSCONFIG),
            ("common/etc/preferred-apps/google.xml", b"<preferred-activities/>\n")],
        "Core/defaultframework-common": [
            ("common/etc/permissions/com.google.android.maps.xml", MAPS_LIB),
            ("common/framework/com.google.android.maps.jar", b"PK stand-in jar")],
        "Core/gmscore-" + garch: [
            ("nodpi/priv-app/PrebuiltGmsCore/PrebuiltGmsCore.apk",
             apk(GMS, "21.39.18", 213918000, abis=gms_abis,
                 permissions=["android.permission.READ_PRIVILEGED_PHONE_STATE"]))],
        "Core/gsfcore-all": [
            ("nodpi/priv-app/GoogleServicesFramework/GoogleServicesFramework.apk", apk(GSF, "11-6800000", 30))],
        "Core/vending-all": [
            ("nodpi/priv-app/Phonesky/Phonesky.apk",
             apk(VENDING, "27.1.16", 82711600, permissions=["android.permission.INSTALL_PACKAGES"]))],
        "Core/vending-common": [
            ("common/product/overlay/PlayStoreOverlay.apk", apk("com.android.vending.overlay", "1"))],
        "GApps/calsync-all": [
            ("240/app/GoogleCalendarSyncAdapter/GoogleCalendarSyncAdapter.apk",
             apk("com.google.android.syncadapters.calendar", "2.4")),
            ("480/app/GoogleCalendarSyncAdapter/GoogleCalendarSyncAdapter.apk",
             apk("com.google.android.syncadapters.calendar", "4.8"))],
    }
    archives.update(extra_archives or {})
    for name, members in archives.items():
        app = name.split("/", 1)[1]
        files[name + suffix] = pack(tar_of([(app + "/" + m[0],) + tuple(m[1:]) for m in members]))
    for name, members in (raw_archives or {}).items():
        files[name + suffix] = pack(tar_of(members))
    return write_zip(path, files)


def snapshot(root):
    out = {}
    for dirpath, dirs, files in os.walk(root):
        for n in dirs + files:
            p = os.path.join(dirpath, n)
            st = os.lstat(p)
            digest = hashlib.sha256(Path(p).read_bytes()).hexdigest() if stat.S_ISREG(st.st_mode) else None
            out[os.path.relpath(p, root)] = (stat.S_IMODE(st.st_mode), digest)
    return out


class Base(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="android-gapps-test-")
        self.dir = Path(os.path.realpath(self._tmp.name))
        env = {k: v for k, v in os.environ.items() if k not in ("ANDROID_X86_ROOT",)}
        env["STEAMARM_ANDROID_ROOT"] = str(self.dir / "no-root-here")
        self._env = mock.patch.dict(os.environ, env, clear=True)
        self._env.start()

    def tearDown(self):
        self._env.stop()
        self._tmp.cleanup()

    def run_tool(self, *argv):
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = ga.main([str(a) for a in argv])
        return code, out.getvalue(), err.getvalue()

    def inspect(self, zpath, *args, code=None):
        rc, out, _err = self.run_tool("inspect", "--json", *args, zpath)
        rep = json.loads(out)
        if code is not None:
            self.assertEqual(rc, code, rep.get("findings") or rep)
        return rep

    @staticmethod
    def codes(rep, level=None):
        return {f["code"] for f in rep["findings"] if level is None or f["level"] == level}

    @staticmethod
    def paths(rep, category):
        return [d["path"] for d in rep["install"][category]]

    def fake_root(self, name="root", abilist="x86_64,x86", sdk="30", mark=True):
        root = self.dir / name
        (root / "system/etc/permissions").mkdir(parents=True)
        (root / "system/product").mkdir()
        (root / "system/system_ext/priv-app/Provision").mkdir(parents=True)
        (root / "system/build.prop").write_text(
            "ro.build.version.sdk=%s\nro.product.cpu.abilist=%s\n" % (sdk, abilist))
        (root / "data").mkdir()
        if mark:
            (root / ga.ROOT_MARK).write_text("# test root\narch %s\n" % abilist.split(",")[0])
        return root


class MindTheGappsTests(Base):
    def test_layout_and_what_it_installs(self):
        z = mindthegapps(self.dir / "MindTheGapps-11.0.0-arm64-20230922_081122.zip")
        rep = self.inspect(z, "--arch", "arm64", code=0)
        p = rep["package"]
        self.assertEqual((p["layout"], p["sdk"], p["abi"], p["android"]), ("mindthegapps", 30, "arm64-v8a", "11"))
        self.assertEqual(p["metadata"], {"arch": "aarch64", "version": "30", "version_nice": "11.0.0"})
        self.assertTrue(rep["ok"])
        self.assertEqual(rep["counts"], {"privApps": 4, "apps": 2, "overlays": 0, "permissions": 3,
                                         "defaultPermissions": 1, "sysconfig": 1, "preferredApps": 0,
                                         "framework": 1, "libs": 2, "other": 0})
        self.assertIn("system/product/priv-app/Phonesky/Phonesky.apk", self.paths(rep, "privApps"))
        self.assertIn("system/system_ext/priv-app/GoogleServicesFramework/GoogleServicesFramework.apk",
                      self.paths(rep, "privApps"))
        self.assertIn("system/product/app/MarkupGoogle/lib/arm64/libsketchology_native.so", self.paths(rep, "libs"))
        skipped = {s["name"] for s in rep["notInstalled"]}
        self.assertTrue({"build.prop", "toybox", "META-INF/com/google/android/update-binary",
                         "system/addon.d/addond_head"} <= skipped)
        key = {k["package"]: k for k in rep["keyComponents"]}
        self.assertEqual(key[VENDING]["versionName"], "27.1.16")
        self.assertEqual(key[GMS]["versionCode"], 213918000)
        self.assertTrue(all(k["present"] for k in key.values()))
        self.assertIn("Google proprietary", rep["licence"])

    def test_privileged_permissions_and_config(self):
        rep = self.inspect(mindthegapps(self.dir / "m.zip"), "--arch", "arm64", code=0)
        priv = {d["apk"]["package"]: d.get("privileged") for d in rep["install"]["privApps"]}
        self.assertEqual(priv[VENDING]["granted"], ["android.permission.INSTALL_PACKAGES"])
        self.assertEqual(priv[VENDING]["allowListedNotRequested"], ["android.permission.DELETE_PACKAGES"])
        self.assertEqual(priv[GMS]["denied"], ["android.permission.MANAGE_USERS"])
        self.assertEqual(priv[GSF]["declaredIn"],
                         ["system/system_ext/etc/permissions/privapp-permissions-google-se.xml"])
        gms = next(d for d in rep["install"]["privApps"] if d["apk"]["package"] == GMS)
        self.assertEqual(gms["defaultGrants"], ["android.permission.ACCESS_FINE_LOCATION",
                                                "android.permission.READ_CONTACTS"])
        self.assertEqual(rep["libraries"][0]["name"], "com.google.android.dialer.support")
        self.assertTrue(rep["libraries"][0]["inPackage"])
        self.assertIn("com.google.android.feature.GOOGLE_BUILD", rep["features"])
        self.assertEqual(rep["sysconfig"]["allow-in-power-save"], [GMS])

    def test_arm64_package_on_the_x86_64_root(self):
        rep = self.inspect(mindthegapps(self.dir / "m.zip"), "--arch", "x86_64", code=4)
        self.assertFalse(rep["ok"])
        self.assertIn("abi-mismatch", self.codes(rep, "error"))
        errs = {(f["package"], f["level"]) for f in rep["findings"] if f["code"] == "apk-abi-mismatch"}
        # Play services is fatal; an optional app with arm64 code (lib/arm64 beside its APK) only warns.
        self.assertEqual(errs, {(GMS, "error"), ("com.google.android.markup", "warning")})

    def test_api_mismatch(self):
        z = mindthegapps(self.dir / "m.zip", version="33", nice="13.0.0")
        rep = self.inspect(z, "--arch", "arm64", code=4)
        msg = next(f["message"] for f in rep["findings"] if f["code"] == "api-mismatch")
        self.assertIn("Android 13 (API 33)", msg)
        self.assertIn("Android 11 (API 30)", msg)

    def test_aarch32_package_is_refused(self):
        rep = self.inspect(mindthegapps(self.dir / "m.zip", arch="armv7l", gms_abis=("armeabi-v7a",)),
                           "--arch", "arm64", code=4)
        self.assertIn("AArch32", next(f["message"] for f in rep["findings"] if f["code"] == "abi-mismatch"))

    def test_missing_allowlist_and_min_sdk(self):
        privapp = PRIVAPP.replace(b'package="com.android.vending"', b'package="org.example.other"')
        extra = {"system/product/app/Newer/Newer.apk": apk("org.example.newer", min_sdk=33)}
        rep = self.inspect(mindthegapps(self.dir / "m.zip", privapp=privapp, extra=extra), "--arch", "arm64",
                           code=0)
        missing = [f for f in rep["findings"] if f["code"] == "privapp-allowlist-missing"]
        self.assertEqual([f["package"] for f in missing], [VENDING])
        low = next(f for f in rep["findings"] if f["code"] == "apk-min-sdk")
        self.assertEqual((low["package"], low["level"]), ("org.example.newer", "warning"))

    def test_system_tree_without_metadata(self):
        z = mindthegapps(self.dir / "gapps.zip", build_prop=False)
        rep = self.inspect(z, "--arch", "arm64", code=0)
        self.assertEqual(rep["package"]["layout"], "system-tree")
        self.assertTrue({"package-api-unknown", "package-arch-unknown"} <= self.codes(rep, "warning"))
        named = mindthegapps(self.dir / "MindTheGapps-11.0.0-arm64-20230922_081122.zip", build_prop=False)
        rep = self.inspect(named, "--arch", "arm64", code=0)
        self.assertEqual((rep["package"]["sdk"], rep["package"]["abi"]), (30, "arm64-v8a"))
        self.assertTrue({"sdk-from-name", "arch-from-name"} <= self.codes(rep, "note"))

    def test_text_report(self):
        rc, out, _ = self.run_tool("inspect", "--arch", "arm64", mindthegapps(self.dir / "m.zip"))
        self.assertEqual(rc, 0)
        for text in ("Layout: MindTheGapps", "Google Play Store", "privileged: 2 allow-listed, 1 of them requested",
                     "Verdict: suitable for the root", "Not installed"):
            self.assertIn(text, out)


class TargetTests(Base):
    def test_root_build_prop(self):
        root = self.fake_root()
        rep = self.inspect(opengapps(self.dir / "o.zip"), "--root", root, code=0)
        self.assertEqual(rep["target"]["abilist"], ["x86_64", "x86"])
        self.assertEqual((rep["target"]["source"], rep["target"]["root"]), ("root build.prop", str(root)))

    def test_root_marker_and_toybox(self):
        root = self.dir / "r1"
        (root / "system").mkdir(parents=True)
        (root / ga.ROOT_MARK).write_text("arch arm64\n")
        t = ga.resolve_target(root=str(root))
        self.assertEqual((t.abi, t.runnable(), t.sdk), ("arm64-v8a", ["arm64-v8a"], 30))
        root2 = self.dir / "r2"
        (root2 / "system/bin").mkdir(parents=True)
        (root2 / "system/bin/toybox").write_bytes(b"\x7fELF" + bytes(14) + struct.pack("<H", 62))
        self.assertEqual(ga.resolve_target(root=str(root2)).abilist, ["x86_64", "x86"])

    def test_default_root_from_environment(self):
        root = self.fake_root()
        os.environ["STEAMARM_ANDROID_ROOT"] = str(root)
        rep = self.inspect(opengapps(self.dir / "o.zip"), code=0)
        self.assertEqual(rep["target"]["root"], str(root))

    def test_no_target(self):
        rep = self.inspect(opengapps(self.dir / "o.zip"), code=0)
        self.assertIn("target-unknown", self.codes(rep, "warning"))
        rc, out, _ = self.run_tool("inspect", opengapps(self.dir / "o2.zip"))
        self.assertIn("none found and no --arch", out)

    def test_bad_arguments(self):
        rc, _, err = self.run_tool("inspect", "--arch", "mips", opengapps(self.dir / "o.zip"))
        self.assertEqual(rc, 2)
        rc, _, _ = self.run_tool("inspect", "--root", self.dir / "nothing", opengapps(self.dir / "o.zip"))
        self.assertEqual(rc, 2)


class OpenGAppsTests(Base):
    def test_lzip_layout(self):
        rep = self.inspect(opengapps(self.dir / "open_gapps-x86_64-11.0-pico-20220503.zip"), "--arch", "x86_64",
                           code=0)
        p = rep["package"]
        self.assertEqual((p["layout"], p["sdk"], p["abi"], p["variant"]), ("opengapps", 30, "x86_64", "pico"))
        self.assertEqual(self.paths(rep, "privApps"), [
            "system/priv-app/GoogleServicesFramework/GoogleServicesFramework.apk",
            "system/priv-app/Phonesky/Phonesky.apk",
            "system/priv-app/PrebuiltGmsCore/PrebuiltGmsCore.apk"])
        self.assertEqual(self.paths(rep, "overlays"), ["system/product/overlay/PlayStoreOverlay.apk"])
        self.assertEqual(self.paths(rep, "framework"), ["system/framework/com.google.android.maps.jar"])
        self.assertEqual(self.paths(rep, "preferredApps"), ["system/etc/preferred-apps/google.xml"])
        self.assertTrue(rep["libraries"][0]["inPackage"])
        arch = {a["app"]: a for a in rep["archives"]}
        self.assertEqual(arch["gmscore-x86_64"]["chosen"], "nodpi")
        self.assertEqual((arch["calsync-all"]["densities"], arch["calsync-all"]["chosen"]), (["240", "480"], "480"))
        self.assertEqual(rep["install"]["apps"][0]["apk"]["versionName"], "4.8")
        skipped = {s["name"] for s in rep["notInstalled"]}
        self.assertTrue({"g.prop", "installer.sh", "busybox-x86", "gapps-remove.txt"} <= skipped)
        self.assertTrue(rep["ok"])

    def test_density_option(self):
        rep = self.inspect(opengapps(self.dir / "o.zip"), "--arch", "x86_64", "--density", "240", code=0)
        self.assertEqual(rep["install"]["apps"][0]["apk"]["versionName"], "2.4")

    def test_xz_and_plain_tar(self):
        want = self.inspect(opengapps(self.dir / "lz.zip"), "--arch", "x86_64", code=0)["install"]
        for comp in ("xz", "none"):
            with self.subTest(compression=comp):
                got = self.inspect(opengapps(self.dir / (comp + ".zip"), compression=comp), "--arch", "x86_64",
                                   code=0)["install"]
                self.assertEqual({c: [d["path"] for d in v] for c, v in got.items()},
                                 {c: [d["path"] for d in v] for c, v in want.items()})

    def test_x86_package_on_the_x86_64_root(self):
        rep = self.inspect(opengapps(self.dir / "o.zip", arch="x86", gms_abis=("x86",)), "--arch", "x86_64", code=0)
        self.assertTrue({"abi-secondary", "apk-abi-secondary"} <= self.codes(rep, "warning"))

    def test_arm_only_app_in_an_x86_64_package(self):
        extra = {"GApps/markup-arm": [("nodpi/app/MarkupGoogle/MarkupGoogle.apk",
                                       apk("com.google.android.markup", abis=("armeabi-v7a",)))]}
        rep = self.inspect(opengapps(self.dir / "o.zip", extra_archives=extra), "--arch", "x86_64", code=0)
        f = next(f for f in rep["findings"] if f["code"] == "apk-abi-mismatch")
        self.assertEqual((f["package"], f["level"]), ("com.google.android.markup", "warning"))
        rep = self.inspect(opengapps(self.dir / "o2.zip", gms_abis=("arm64-v8a",)), "--arch", "x86_64", code=4)
        self.assertEqual([f["package"] for f in rep["findings"] if f["code"] == "apk-abi-mismatch"], [GMS])


class LzipTests(unittest.TestCase):
    def decode(self, blob):
        out = io.BytesIO()
        ga.lzip_decompress(io.BytesIO(blob), out)
        return out.getvalue()

    def test_members_trailing_data_and_dictionary_coding(self):
        a, b = os.urandom(3000) + b"a" * 70000, b"second member " * 999
        self.assertEqual(self.decode(lzip(a) + lzip(b, dict_log=21, frac=4) + bytes(64)), a + b)
        self.assertEqual(self.decode(lzip(b"") + b"trailing, not a member"), b"")

    def test_damage_is_detected(self):
        good = lzip(b"payload " * 5000)
        bad_crc = good[:-20] + struct.pack("<I", 1) + good[-16:]
        for name, blob in (("crc", bad_crc), ("truncated", good[:len(good) // 2]), ("no trailer", good[:-20]),
                           ("not lzip", b"PK\x03\x04" + good[4:]), ("version", good[:4] + b"\x02" + good[5:]),
                           ("corrupt second header", good + b"LZIP\x07")):
            with self.subTest(name):
                with self.assertRaises(ga.GappsError):
                    self.decode(blob)

    def test_output_cap(self):
        with self.assertRaises(ga.GappsError) as cm:
            ga.lzip_decompress(io.BytesIO(lzip(b"x" * 100000)), io.BytesIO(), budget=1000)
        self.assertEqual(cm.exception.code, "too-large")


class UnsupportedTests(Base):
    def test_not_gapps(self):
        cases = {
            "single-apk": {"AndroidManifest.xml": b"\x03\x00", "classes.dex": b"dex"},
            "system-image": {"system.img": b"\0" * 64, "vendor.img": b"\0" * 64},
            "unknown-layout": {"README.txt": "hello"},
        }
        for code, files in cases.items():
            with self.subTest(code):
                rc, out, _ = self.run_tool("inspect", "--json", "--arch", "x86_64",
                                           write_zip(self.dir / (code + ".zip"), files))
                self.assertEqual((rc, json.loads(out)["code"]), (3, code))
        (self.dir / "plain.txt").write_text("not a zip")
        rc, out, _ = self.run_tool("inspect", "--json", "--arch", "x86_64", self.dir / "plain.txt")
        self.assertEqual((rc, json.loads(out)["code"]), (2, "not-a-package"))


class TraversalTests(Base):
    EVIL = ("../evil-1.txt", "/evil-2.txt", "system/../../evil-3.txt",
            "system/priv-app/X/..\\..\\evil-4.txt", "C:/evil-5.txt")

    def assert_nothing_escaped(self):
        found = [str(p) for p in self.dir.rglob("evil-*")]
        self.assertEqual(found, [])

    def test_zip_entries(self):
        z = mindthegapps(self.dir / "m.zip", extra={n: b"x" for n in self.EVIL},
                         links=[("system/priv-app/Link", "../../../etc")])
        rep = self.inspect(z, "--arch", "arm64", code=4)
        unsafe = [f["entry"] for f in rep["findings"] if f["code"] == "unsafe-path"]
        self.assertEqual(sorted(unsafe), sorted(self.EVIL + ("system/priv-app/Link",)))
        (self.dir / "a" / "b").mkdir(parents=True)
        out = self.dir / "a" / "b" / "overlay"
        rc, _, err = self.run_tool("overlay", "--arch", "arm64", "--allow-mismatch", z, out)
        self.assertEqual(rc, 2, err)
        self.assertEqual(list((self.dir / "a" / "b").iterdir()), [])
        self.assert_nothing_escaped()

    def test_tar_members(self):
        hostile = {"Core/evil-all": [
            ("evil-all/nodpi/../../../evil-6.txt", b"x"),
            ("/evil-7.txt", b"x"),
            ("evil-all/nodpi/priv-app/L/L.apk", tarfile.SYMTYPE, "../../../../etc/passwd"),
            ("evil-all/nodpi/priv-app/H/H.apk", tarfile.LNKTYPE, "evil-all/nodpi/x"),
            ("evil-all/nodpi/dev", tarfile.CHRTYPE, "")]}
        z = opengapps(self.dir / "o.zip", raw_archives=hostile)
        rep = self.inspect(z, "--arch", "x86_64", code=4)
        unsafe = sorted(f["entry"] for f in rep["findings"] if f["code"] == "unsafe-path")
        self.assertEqual(unsafe, sorted("Core/evil-all.tar.lz!" + m[0] for m in hostile["Core/evil-all"]))
        out = self.dir / "overlay"
        rc, _, _ = self.run_tool("overlay", "--arch", "x86_64", z, out)
        self.assertEqual(rc, 2)
        self.assertFalse(out.exists())
        self.assert_nothing_escaped()


class OverlayTests(Base):
    def overlay(self, zpath, out, *args, code=0):
        rc, stdout, err = self.run_tool("overlay", "--json", *args, zpath, out)
        self.assertEqual(rc, code, stdout + err)
        return json.loads(stdout) if stdout.strip() else None

    def test_contents_modes_and_manifest(self):
        out = self.dir / "overlay"
        res = self.overlay(mindthegapps(self.dir / "m.zip"), out, "--arch", "arm64")
        self.assertEqual(res["files"], 14)
        tree = snapshot(out)
        for rel in ("system/product/priv-app/Phonesky/Phonesky.apk",
                    "system/system_ext/priv-app/GoogleServicesFramework/GoogleServicesFramework.apk",
                    "system/product/etc/permissions/privapp-permissions-google-p.xml",
                    "system/product/etc/default-permissions/default-permissions-google.xml",
                    "system/product/etc/sysconfig/google.xml",
                    "system/product/framework/com.google.android.dialer.support.jar",
                    "system/product/lib64/libjni_latinimegoogle.so",
                    "system/product/app/MarkupGoogle/lib/arm64/libsketchology_native.so"):
            self.assertEqual(tree[rel][0], 0o644, rel)
        self.assertTrue(all(mode == 0o755 for rel, (mode, digest) in tree.items() if digest is None))
        for absent in ("build.prop", "toybox", "META-INF", "system/addon.d"):
            self.assertNotIn(absent, tree)
        man = json.loads((out / ga.MANIFEST_NAME).read_text())
        self.assertEqual(man["source"]["layout"], "mindthegapps")
        self.assertIn("never committed", man["licence"])
        self.assertEqual({f["path"]: f["sha256"] for f in man["files"]},
                         {rel: d for rel, (m, d) in tree.items() if d and rel != ga.MANIFEST_NAME})

    def test_idempotent(self):
        z, out = mindthegapps(self.dir / "m.zip"), self.dir / "work" / "overlay"
        self.overlay(z, out, "--arch", "arm64")
        first, man1 = snapshot(out), (out / ga.MANIFEST_NAME).read_bytes()
        self.overlay(z, out, "--arch", "arm64")
        self.assertEqual(snapshot(out), first)
        self.assertEqual((out / ga.MANIFEST_NAME).read_bytes(), man1)
        self.assertEqual(sorted(p.name for p in (self.dir / "work").iterdir()), ["overlay"])

    def test_replaces_its_own_overlay_only(self):
        z, out = mindthegapps(self.dir / "m.zip"), self.dir / "overlay"
        self.overlay(z, out, "--arch", "arm64")
        res = self.overlay(z, out, "--arch", "arm64", "--exclude", "Velvet",
                           "--exclude", "com.google.android.syncadapters.calendar")
        self.assertFalse((out / "system/product/priv-app/Velvet").exists())
        self.assertFalse((out / "system/product/app/GoogleCalendarSyncAdapter").exists())
        self.assertEqual(res["excluded"], ["system/product/app/GoogleCalendarSyncAdapter/GoogleCalendarSyncAdapter.apk",
                                           "system/product/priv-app/Velvet/Velvet.apk"])
        foreign = self.dir / "mine"
        foreign.mkdir()
        (foreign / "keep.txt").write_text("the owner's")
        self.overlay(z, foreign, "--arch", "arm64", code=4)
        self.assertEqual(sorted(p.name for p in foreign.iterdir()), ["keep.txt"])

    def test_exclude_findings(self):
        rep = self.inspect(mindthegapps(self.dir / "m.zip"), "--arch", "arm64", "--exclude", VENDING,
                           "--exclude", "NoSuchApp", code=0)
        self.assertTrue({"key-component-excluded", "exclude-unmatched", "key-component-missing"}
                        <= self.codes(rep, "warning"))
        rep = self.inspect(opengapps(self.dir / "o.zip"), "--arch", "x86_64", "--exclude", "calsync", code=0)
        self.assertEqual(rep["counts"]["apps"], 0)

    def test_refuses_the_repository_and_the_android_root(self):
        z = opengapps(self.dir / "o.zip")
        inside_repo = REPO / "build" / ("gapps-overlay-test-%d" % os.getpid())
        self.overlay(z, inside_repo, "--arch", "x86_64", code=4)
        self.assertFalse(inside_repo.exists())
        root = self.fake_root()
        before = snapshot(root)
        self.overlay(z, root / "data" / "overlay", "--arch", "x86_64", code=4)
        self.overlay(z, root / "system", "--root", root, code=4)
        self.overlay(z, self.dir / "overlay", "--root", root)
        self.assertEqual(snapshot(root), before)

    def test_refuses_a_mismatch_unless_asked(self):
        z, out = mindthegapps(self.dir / "m.zip"), self.dir / "overlay"
        rc, _, err = self.run_tool("overlay", "--arch", "x86_64", z, out)
        self.assertEqual(rc, 4)
        self.assertIn("abi-mismatch", err)
        self.assertFalse(out.exists())
        self.overlay(z, out, "--arch", "x86_64", "--allow-mismatch")
        self.assertTrue((out / ga.MANIFEST_NAME).is_file())


class ApplyPlanTests(Base):
    def make(self, zpath, *args):
        out = self.dir / "overlay"
        rc, stdout, err = self.run_tool("overlay", *args, zpath, out)
        self.assertEqual(rc, 0, stdout + err)
        return out

    def plan(self, overlay, *args, code=0):
        rc, out, err = self.run_tool("apply-plan", "--json", *args, overlay)
        self.assertEqual(rc, code, out + err)
        return json.loads(out)

    def test_commands_and_nothing_is_run(self):
        root = self.fake_root()
        (root / "system/etc/permissions/privapp-permissions-google.xml").write_text("<permissions/>")
        before = snapshot(root)
        overlay = self.make(opengapps(self.dir / "o.zip"), "--root", root)
        p = self.plan(overlay)
        derived = self.dir / "gapps-x86_64"
        self.assertEqual((p["root"], p["derived"], p["ran"]), (str(root), str(derived), False))
        commands = [c for s in p["steps"] for c in s["commands"]]
        self.assertIn("cp -cR %s %s" % (root, derived), commands)
        self.assertIn("cp -cR %s/system/ %s/system" % (overlay, derived), commands)
        self.assertIn("rm -rf %s" % derived, commands)
        self.assertTrue(any(c.startswith("ANDROID_X86_ROOT=%s scripts/android-boot.py --root %s" % (derived, derived))
                            for c in commands))
        self.assertFalse(derived.exists())
        self.assertEqual(snapshot(root), before)
        self.assertEqual(p["replaced"], ["system/etc/permissions/privapp-permissions-google.xml"])
        codes = {f["code"] for f in p["findings"]}
        if len(str(derived).encode()) > ga.MAX_X86_ROOT_PATH:
            self.assertIn("derived-path-too-long", codes)
        rc, text, _ = self.run_tool("apply-plan", overlay)
        self.assertIn("nothing was run", text)

    def test_short_derived_path_passes_and_setup_wizard_step(self):
        overlay = self.make(mindthegapps(self.dir / "m.zip", setup_wizard=True), "--arch", "arm64")
        p = self.plan(overlay, "--root", "/Volumes/SteamARMAndroid/root", "--derived",
                      "/Volumes/SteamARMAndroid/gapps-arm64")
        commands = [c for s in p["steps"] for c in s["commands"]]
        self.assertIn("rm -rf /Volumes/SteamARMAndroid/gapps-arm64/system/system_ext/priv-app/Provision", commands)
        self.assertIn("arm64-root-no-java", {f["code"] for f in p["findings"]})
        self.assertNotIn("derived-path-too-long", {f["code"] for f in p["findings"]})

    def test_refusals(self):
        (self.dir / "empty").mkdir()
        self.plan(self.dir / "empty", code=2)
        root = self.fake_root()
        overlay = self.make(opengapps(self.dir / "o.zip"), "--root", root)
        self.plan(overlay, "--derived", root / "inside", code=4)
        self.plan(overlay, "--derived", REPO / "build" / "derived", code=4)
        apk_path = overlay / "system/priv-app/Phonesky/Phonesky.apk"
        apk_path.write_bytes(apk_path.read_bytes() + b"changed")
        p = self.plan(overlay)
        self.assertIn("overlay-changed", {f["code"] for f in p["findings"]})
        self.assertFalse(p["ok"])


class GsfIdTests(Base):
    def db_path(self, root):
        return root / "data/data/com.google.android.gsf/databases/gservices.db"

    def test_instructions(self):
        root = self.fake_root()
        rc, out, _ = self.run_tool("gsf-id", "--root", root)
        self.assertEqual(rc, 0)
        for text in (ga.UNCERTIFIED_URL, "scripts/run-android-x86.sh /system/bin/sqlite3", ga.GSF_DB,
                     '"%s"' % ga.GSF_QUERY, "never signs in",
                     "ANDROID_X86_ROOT=%s" % root):
            self.assertIn(text, out)

    def test_read_from_a_copy_including_the_wal(self):
        root = self.fake_root()
        db = self.db_path(root)
        db.parent.mkdir(parents=True)
        con = sqlite3.connect(str(db))
        try:
            con.execute("pragma journal_mode=wal")
            con.execute("create table main (name text primary key, value text)")
            con.execute("insert into main values ('android_id', '3912345678901234567')")
            con.commit()                                   # kept open: the row is still only in the WAL
            rc, out, _ = self.run_tool("gsf-id", "--root", root, "--read")
        finally:
            con.close()
        self.assertEqual(rc, 0)
        self.assertIn("GSF Android ID (from %s): 3912345678901234567" % ga.GSF_DB, out)

    def test_not_there_yet(self):
        root = self.fake_root()
        rc, _, err = self.run_tool("gsf-id", "--root", root, "--read")
        self.assertEqual(rc, 5)
        self.assertIn("have not checked in", err)
        db = self.db_path(root)
        db.parent.mkdir(parents=True)
        con = sqlite3.connect(str(db))
        con.execute("create table main (name text primary key, value text)")
        con.commit()
        con.close()
        rc, _, _ = self.run_tool("gsf-id", "--root", root, "--read")
        self.assertEqual(rc, 5)


class PolicyTests(unittest.TestCase):
    def test_the_tool_cannot_reach_the_network(self):
        """Rule 1-2 of docs/PLAY_STORE_RESEARCH.md: nothing is downloaded, no
        browser is opened, no sign-in is automated."""
        tree = ast.parse((REPO / "scripts/android-gapps.py").read_text())
        imported = set()
        for node in ast.walk(tree):
            if isinstance(node, ast.Import):
                imported |= {a.name.split(".")[0] for a in node.names}
            elif isinstance(node, ast.ImportFrom):
                imported.add((node.module or "").split(".")[0])
        banned = {"urllib", "http", "socket", "ssl", "ftplib", "webbrowser", "requests", "subprocess", "smtplib"}
        self.assertEqual(imported & banned, set())

    def test_no_google_package_in_the_repository(self):
        found = []
        for dirpath, dirs, files in os.walk(REPO):
            dirs[:] = [d for d in dirs if not d.startswith(".") and d not in ("build", "node_modules", "graphify-out")]
            found += [os.path.join(dirpath, f) for f in files
                      if f.lower().endswith((".apk", ".apks", ".xapk", ".tar.lz")) or
                      f.lower().startswith(("mindthegapps", "open_gapps", "litegapps"))]
        self.assertEqual(found, [])


if __name__ == "__main__":
    unittest.main()
