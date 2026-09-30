#!/usr/bin/env python3
"""Google apps for SteamARM's Android root, from a package the owner supplies
(docs/PLAY_STORE_SETUP.md). Python 3.9+, standard library only.

SteamARM never downloads, bundles or redistributes Google's apps (GMS, Google
Play Store, Google Services Framework), never fakes device certification and
never bypasses Play Integrity or SafetyNet (docs/PLAY_STORE_RESEARCH.md). This
program opens no network connection. It reads a GApps zip the owner obtained
themselves, says what it would install, builds an overlay tree next to the
pristine Android root without touching the root, and prints -- without running
them -- the commands that layer the overlay onto an APFS clone of the root.

  scripts/android-gapps.py inspect [--arch A | --root R] [--json] [-v] package.zip
  scripts/android-gapps.py overlay [--arch A | --root R] [--exclude NAME]...
                                   [--density DPI] [--allow-mismatch] package.zip OUT
  scripts/android-gapps.py apply-plan [--root R] [--derived D] [--json] OUT
  scripts/android-gapps.py gsf-id [--root R] [--read]

Layouts read (both VERIFIED IN SOURCE, docs/PLAY_STORE_SETUP.md):
  MindTheGapps   build.prop (arch=, version=<SDK>, version_nice=) and a
                 system/ tree (system/product/..., system/system_ext/...)
  OpenGApps      g.prop (ro.addon.arch, ro.addon.sdk, ...) and one tar per
                 app, Core/<app>-<arch>.tar.lz (or .tar.xz, .tar), whose
                 members are <app>-<arch>/<density|common>/<path under /system>
  a plain system/ tree with no metadata is read too, with a warning.

The target is the Android root's ABI and API level: --arch arm64|x86_64, or
--root (its build.prop), or else the default root if it exists
($STEAMARM_ANDROID_ROOT, $ANDROID_X86_ROOT, /Volumes/SteamARMAndroid/root-x86_64).
SteamARM's roots are Android 11 (API 30).

Exit status: 0 done (inspect: no errors); 2 bad input (not a zip, unreadable,
an entry whose path could escape the output directory, not an overlay made by
this program); 3 unsupported layout (a single APK, a system image, an unknown
zip); 4 refused (inspect: the package does not suit the root; overlay: the same
without --allow-mismatch, or an output directory inside the repository, inside
an Android root, or not empty and not an earlier overlay); 5 not found
(gsf-id --read: no gservices.db yet); 6 I/O error.
"""
import argparse
import hashlib
import importlib.util
import json
import lzma
import os
import re
import shlex
import shutil
import sqlite3
import stat
import struct
import sys
import tarfile
import tempfile
import xml.etree.ElementTree as ET
import zipfile
import zlib
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
_spec = importlib.util.spec_from_file_location("apk_inspect", HERE / "apk-inspect.py")
apk_inspect = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(apk_inspect)

EXIT_OK, EXIT_INPUT, EXIT_UNSUPPORTED, EXIT_REFUSED, EXIT_MISSING, EXIT_IO = 0, 2, 3, 4, 5, 6

TARGET_SDK = 30                                   # both SteamARM roots: LineageOS 18.1, Android 11
ANDROID_RELEASE = {23: "6.0", 24: "7.0", 25: "7.1", 26: "8.0", 27: "8.1", 28: "9", 29: "10", 30: "11",
                   31: "12", 32: "12L", 33: "13", 34: "14", 35: "15", 36: "16"}
RELEASE_SDK = {"6.0": 23, "7.0": 24, "7.1": 25, "8.0": 26, "8.1": 27, "9.0": 28, "10.0": 29, "11.0": 30,
               "12.0": 31, "12.1": 32, "12L": 32, "13.0": 33, "14.0": 34, "15.0": 35, "16.0": 36}
DEFAULT_ROOT = "/Volumes/SteamARMAndroid/root-x86_64"
ROOT_MARK = ".steamarm-androidroot"               # written by scripts/mkandroidroot.sh
MANIFEST_NAME = ".steamarm-gapps-overlay.json"
LICENCE = ("Google proprietary, no redistribution licence: the owner's own copy, kept on this Mac, "
           "never committed, bundled or uploaded (docs/PLAY_STORE_RESEARCH.md, rules 1-2)")
KEY_PACKAGES = (("com.google.android.gms", "Google Play services"),
                ("com.google.android.gsf", "Google Services Framework"),
                ("com.android.vending", "Google Play Store"))
KEY_NAMES = dict(KEY_PACKAGES)
UNCERTIFIED_URL = "https://www.google.com/android/uncertified/"
GSF_DB = "/data/data/com.google.android.gsf/databases/gservices.db"
GSF_QUERY = "select value from main where name = 'android_id';"
# FEXServer's socket for a root is <root>/data/local/tmp/steamarm-android-
# <12 hex>.FEXServer.Socket (scripts/run-android-x86.sh, scripts/android-boot.py)
# and must fit Darwin's 104-byte sun_path with its NUL: a root path of at most
# 103 - 62 = 41 bytes (a 44-byte one failed, docs/ANDROID_RUNTIME_ARCHITECTURE.md).
SOCKET_TAIL = len("/data/local/tmp/steamarm-android-") + 12 + len(".FEXServer.Socket")
MAX_X86_ROOT_PATH = 103 - SOCKET_TAIL

MAX_ENTRIES = 50000               # zip entries, and members of one inner tar
MAX_FILE = 1 << 30                # one installed file (Play services is ~150 MB)
MAX_TOTAL = 8 << 30               # everything unpacked from one package
MAX_META = 1 << 20                # build.prop, g.prop, an overlay manifest
MAX_XML = 4 << 20                 # a permissions or sysconfig file
CHUNK = 1 << 20

ABI_ALIASES = {
    "aarch64": "arm64-v8a", "arm64": "arm64-v8a", "arm64-v8a": "arm64-v8a", "armv8": "arm64-v8a",
    "arm": "armeabi-v7a", "armv7l": "armeabi-v7a", "armv7": "armeabi-v7a", "armv8l": "armeabi-v7a",
    "armeabi-v7a": "armeabi-v7a", "armeabi": "armeabi",
    "x86": "x86", "i386": "x86", "i686": "x86", "x86_64": "x86_64", "amd64": "x86_64",
}
AARCH32 = ("armeabi-v7a", "armeabi")
SHORT_ARCH = {"arm64-v8a": "arm64", "armeabi-v7a": "arm", "armeabi": "arm", "x86_64": "x86_64", "x86": "x86"}
ROOT_ABILIST = {"arm64-v8a": ["arm64-v8a", "armeabi-v7a", "armeabi"], "x86_64": ["x86_64", "x86"]}

CATEGORIES = (
    ("privApps", "privileged apps (priv-app)"),
    ("apps", "apps (app)"),
    ("overlays", "resource overlays"),
    ("permissions", "permissions and privapp allow-lists (etc/permissions)"),
    ("defaultPermissions", "default permission grants (etc/default-permissions)"),
    ("sysconfig", "sysconfig (etc/sysconfig)"),
    ("preferredApps", "preferred apps (etc/preferred-apps)"),
    ("framework", "framework jars"),
    ("libs", "native libraries"),
    ("other", "other files"),
)
CATEGORY_NAMES = [c for c, _ in CATEGORIES]

INSTALLER_FILES = {
    "build.prop": "MindTheGapps' package metadata (arch, SDK)",
    "g.prop": "OpenGApps' package metadata (arch, SDK, variant)",
    "toybox": "the recovery installer's toybox",
    "installer.sh": "the recovery installer",
    "bkup_tail.sh": "the installer's addon.d backup script",
    "app_densities.txt": "OpenGApps' density index (the densities are read from each archive instead)",
    "app_sizes.txt": "the installer's size table",
    "gapps-remove.txt": "the installer's list of files to delete (never done here)",
    "LICENSE": "the packager's licence text",
}


class GappsError(Exception):
    def __init__(self, status, code, message, **extra):
        super().__init__(message)
        self.status, self.code, self.message, self.extra = status, code, message, extra


def finding(level, code, message, **extra):
    f = {"level": level, "code": code, "message": message}
    f.update(extra)
    return f


# ------------------------------------------------------------------ paths

def safe_parts(name):
    """The components of an archive member's name, or None if the name could
    point outside the directory it is unpacked into: absolute, a drive letter,
    a backslash, a NUL, or a '..' component."""
    if not isinstance(name, str) or not name or "\x00" in name or "\\" in name:
        return None
    if name.startswith("/") or re.match(r"^[A-Za-z]:", name):
        return None
    parts = [p for p in name.split("/") if p not in ("", ".")]
    if not parts or any(p == ".." for p in parts):
        return None
    return parts


def zip_is_symlink(info):
    return info.create_system == 3 and stat.S_ISLNK(info.external_attr >> 16)


def inside(path, base):
    path, base = os.path.realpath(path), os.path.realpath(base)
    return path == base or path.startswith(base.rstrip(os.sep) + os.sep)


def norm_abi(s):
    return ABI_ALIASES.get((s or "").strip().lower())


def release_of(sdk):
    return ANDROID_RELEASE.get(sdk, "API %s" % sdk) if sdk else "unknown"


def parse_props(data):
    props = {}
    for line in data.decode("utf-8", "replace").splitlines():
        line = line.strip()
        if not line or line.startswith("#") or "=" not in line:
            continue
        k, v = line.split("=", 1)
        props.setdefault(k.strip(), v.strip())
    return props


def read_props_file(path):
    try:
        with open(path, "rb") as f:
            return parse_props(f.read(MAX_META))
    except OSError:
        return {}


def as_int(v):
    try:
        return int(str(v).strip())
    except (TypeError, ValueError):
        return None


# ------------------------------------------------------------------ the target root

class Target:
    """The Android root the package is checked against."""

    def __init__(self, abilist=None, sdk=TARGET_SDK, source="none", root=None):
        self.abilist = [a for a in (abilist or []) if a]
        self.sdk = sdk
        self.source = source
        self.root = root

    @property
    def abi(self):
        return self.abilist[0] if self.abilist else None

    def runnable(self):
        """What the root can execute on Apple silicon: no AArch32 at all
        (docs/APK_SUPPORT.md, ABI policy)."""
        return [a for a in self.abilist if a not in AARCH32]

    def as_dict(self):
        return {"abi": self.abi, "abilist": self.abilist, "runnable": self.runnable(), "sdk": self.sdk,
                "android": release_of(self.sdk), "source": self.source, "root": self.root}


def root_target(root):
    """The ABI list and API level an Android root declares (its build.prop
    files), else what scripts/mkandroidroot.sh recorded, else its toybox."""
    root = os.path.realpath(os.path.expanduser(root))
    props = {}
    for rel in ("system/build.prop", "vendor/build.prop", "system/vendor/build.prop", "build.prop"):
        for k, v in read_props_file(os.path.join(root, rel)).items():
            props.setdefault(k, v)
    abilist = []
    for key in ("ro.product.cpu.abilist", "ro.system.product.cpu.abilist", "ro.vendor.product.cpu.abilist"):
        if props.get(key):
            abilist = [a.strip() for a in props[key].split(",") if a.strip()]
            break
    if not abilist and props.get("ro.product.cpu.abi"):
        abilist = [props["ro.product.cpu.abi"]]
    source = "root build.prop"
    if not abilist:
        try:
            with open(os.path.join(root, ROOT_MARK), "rb") as f:
                mark = f.read(MAX_META).decode("utf-8", "replace").splitlines()
        except OSError:
            mark = []
        for line in mark:                       # "arch x86_64" (scripts/mkandroidroot.sh)
            words = line.split()
            if len(words) == 2 and words[0] == "arch":
                abilist = list(ROOT_ABILIST.get(norm_abi(words[1]), []))
                source = "root %s" % ROOT_MARK
    if not abilist:
        try:
            with open(os.path.join(root, "system/bin/toybox"), "rb") as f:
                head = f.read(20)
            machine = struct.unpack_from("<H", head, 18)[0] if len(head) == 20 and head[:4] == b"\x7fELF" else 0
        except OSError:
            machine = 0
        abilist = list(ROOT_ABILIST.get({62: "x86_64", 183: "arm64-v8a"}.get(machine), []))
        source = "root toybox ELF"
    sdk = as_int(props.get("ro.build.version.sdk")) or as_int(props.get("ro.system.build.version.sdk"))
    if not abilist and sdk is None and not os.path.isdir(os.path.join(root, "system")):
        raise GappsError(EXIT_INPUT, "no-root", "%s does not look like an Android root (no system/)" % root)
    return Target(abilist, sdk or TARGET_SDK, source if abilist else "root (ABI not found)", root)


def default_root():
    for cand in (os.environ.get("STEAMARM_ANDROID_ROOT"), os.environ.get("ANDROID_X86_ROOT"), DEFAULT_ROOT):
        if cand:
            return cand
    return DEFAULT_ROOT


def resolve_target(arch=None, root=None):
    if arch:
        abi = norm_abi(arch)
        if abi not in ROOT_ABILIST:
            raise GappsError(EXIT_INPUT, "bad-arch", "--arch is arm64 or x86_64 (SteamARM's two Android roots), not %r"
                             % arch)
        t = Target(ROOT_ABILIST[abi], TARGET_SDK, "--arch %s" % arch, None)
        if root:
            t.root = os.path.realpath(os.path.expanduser(root))
        return t
    if root:
        return root_target(root)
    cand = default_root()
    if os.path.isfile(os.path.join(cand, "system/build.prop")) or os.path.isfile(os.path.join(cand, ROOT_MARK)):
        return root_target(cand)
    return Target([], TARGET_SDK, "none", None)


# ------------------------------------------------------------------ decompression

class _Reader:
    """A byte stream with push-back, for lzip's member trailers."""

    def __init__(self, f):
        self.f, self.buf = f, b""

    def read(self, n):
        if self.buf:
            b, self.buf = self.buf[:n], self.buf[n:]
            return b
        return self.f.read(n)

    def read_exact(self, n):
        out = b""
        while len(out) < n:
            b = self.read(n - len(out))
            if not b:
                break
            out += b
        return out

    def unread(self, b):
        self.buf = b + self.buf


def _pump(dec, reader, dst, budget, what, used=0):
    """Decompress until the stream ends. Returns (bytes out, bytes fed, crc32)."""
    fed, out_n, crc = 0, 0, 0
    while not dec.eof:
        if dec.needs_input:
            chunk = reader.read(CHUNK)
            if not chunk:
                raise GappsError(EXIT_INPUT, "bad-archive", "%s: the compressed stream is truncated" % what)
            fed += len(chunk)
        else:
            chunk = b""
        try:
            out = dec.decompress(chunk, CHUNK)
        except lzma.LZMAError as e:
            raise GappsError(EXIT_INPUT, "bad-archive", "%s: %s" % (what, e))
        out_n += len(out)
        if used + out_n > budget:
            raise GappsError(EXIT_INPUT, "too-large", "%s unpacks to more than %d bytes" % (what, budget))
        crc = zlib.crc32(out, crc)
        dst.write(out)
    return out_n, fed, crc


def lzip_decompress(src, dst, budget=MAX_TOTAL, what="lzip stream"):
    """Decode an lzip file (https://www.nongnu.org/lzip/manual/lzip_manual.html,
    "File format") with the standard library: each member is "LZIP", version 1,
    a coded dictionary size, an LZMA stream (lc=3, lp=0, pb=2) ending with an
    end-of-stream marker, then CRC32, data size and member size, all checked.
    Data after the last member is ignored unless it starts like a member, as
    lzip itself does. Returns the number of bytes written."""
    r = _Reader(src)
    total = members = 0
    while True:
        head = r.read_exact(6)
        if members and (not head or head[:4] != b"LZIP"[:len(head[:4])]):
            return total                    # the end, or trailing data that is not a member
        if len(head) < 6 or head[:4] != b"LZIP":
            raise GappsError(EXIT_INPUT, "bad-archive", "%s: not an lzip member (or a corrupt header)" % what)
        if head[4] != 1:
            raise GappsError(EXIT_INPUT, "bad-archive", "%s: lzip version %d is not supported" % (what, head[4]))
        coded = head[5]
        base = 1 << (coded & 0x1F)
        dict_size = base - (base >> 4) * (coded >> 5)
        if not (1 << 12) <= dict_size <= (1 << 29):
            raise GappsError(EXIT_INPUT, "bad-archive", "%s: invalid lzip dictionary size" % what)
        dec = lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[
            {"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": 3, "lp": 0, "pb": 2}])
        size, fed, crc = _pump(dec, r, dst, budget, what, total)
        total += size
        rest = dec.unused_data
        r.unread(rest)
        trailer = r.read_exact(20)
        if len(trailer) < 20:
            raise GappsError(EXIT_INPUT, "bad-archive", "%s: lzip member trailer is truncated" % what)
        t_crc, t_size, t_member = struct.unpack("<IQQ", trailer)
        if t_crc != crc or t_size != size or t_member != 6 + (fed - len(rest)) + 20:
            raise GappsError(EXIT_INPUT, "bad-archive", "%s: lzip CRC or size check failed" % what)
        members += 1


def xz_decompress(src, dst, budget=MAX_TOTAL, what="xz stream"):
    return _pump(lzma.LZMADecompressor(lzma.FORMAT_XZ), _Reader(src), dst, budget, what)[0]


def copy_capped(src, dst, limit, what):
    """Copy a stream, counting what is actually read (not what a header says);
    returns (bytes, sha256)."""
    h, n = hashlib.sha256(), 0
    while True:
        b = src.read(CHUNK)
        if not b:
            return n, h.hexdigest()
        n += len(b)
        if n > limit:
            raise GappsError(EXIT_INPUT, "too-large", "%s is larger than %d bytes" % (what, limit))
        h.update(b)
        dst.write(b)


# ------------------------------------------------------------------ what goes where

def classify(dest):
    """(category, partition, app directory) of a path under the Android root
    (system/...), or (None, partition, None) for installer-only files."""
    parts = dest.split("/")
    if parts[0] != "system" or len(parts) < 2:
        return "other", None, None
    rest, partition = parts[1:], "system"
    if len(rest) > 1 and rest[0] in ("product", "system_ext", "vendor", "odm"):
        partition, rest = rest[0], rest[1:]
    rel = "/".join(rest)
    if rest[0] == "addon.d":
        return None, partition, None
    m = re.fullmatch(r"(priv-app|app)/([^/]+)/[^/]+\.apk", rel)
    if m:
        return ("privApps" if m.group(1) == "priv-app" else "apps"), partition, m.group(2)
    m = re.fullmatch(r"(?:priv-app|app)/([^/]+)/lib/[^/]+/[^/]+\.so", rel)
    if m:
        return "libs", partition, m.group(1)
    m = re.fullmatch(r"(?:priv-app|app)/([^/]+)/.+", rel)
    if m:
        return "other", partition, m.group(1)
    if re.fullmatch(r"lib(64)?/.+\.so", rel):
        return "libs", partition, None
    if re.fullmatch(r"etc/permissions/[^/]+\.xml", rel):
        return "permissions", partition, None
    if re.fullmatch(r"etc/default-permissions/[^/]+\.xml", rel):
        return "defaultPermissions", partition, None
    if re.fullmatch(r"etc/sysconfig/[^/]+\.xml", rel):
        return "sysconfig", partition, None
    if re.fullmatch(r"etc/preferred-apps/[^/]+\.xml", rel):
        return "preferredApps", partition, None
    if re.fullmatch(r"framework/[^/]+\.jar", rel):
        return "framework", partition, None
    if re.fullmatch(r"overlay/(?:[^/]+/)?[^/]+\.apk", rel):
        return "overlays", partition, None
    return "other", partition, None


class Item:
    """One file the package would install, at `dest` under the Android root."""

    def __init__(self, dest, size, zinfo=None, tar=None, member=None, archive=None, app=None):
        self.dest = dest
        self.category, self.partition, self.appdir = classify(dest)
        self.size = size
        self.zinfo, self.tar, self.member, self.archive, self.app = zinfo, tar, member, archive, app
        self.apk = None

    @property
    def source(self):
        return "%s!%s" % (self.archive, self.member.name) if self.member is not None else self.zinfo.filename

    def as_dict(self):
        d = {"path": self.dest, "size": self.size, "partition": self.partition, "source": self.source}
        if self.appdir:
            d["appDir"] = self.appdir
        if self.apk is not None:
            d["apk"] = self.apk
        return d


def choose_density(keys, want=None):
    """OpenGApps' per-density APK directories: the requested density, else
    nodpi, else the highest (the installer's which_dpi picks the device's)."""
    dens = [k for k in keys if re.fullmatch(r"nodpi|\d+(?:-\d+)*", k)]
    if not dens:
        return None
    if want:
        for k in sorted(dens):
            if str(want) in k.split("-"):
                return k
    if "nodpi" in dens:
        return "nodpi"
    return max(dens, key=lambda k: max(int(x) for x in k.split("-")))


# ------------------------------------------------------------------ the package

class GappsPackage:
    """A GApps zip, read once. Nothing is written outside a private temporary
    directory until write_overlay."""

    def __init__(self, path, density=None):
        self.path = str(path)
        self.density = density
        self.findings = []
        self.unsafe = []
        self.items = []
        self.skipped = []
        self.archives = []
        self.meta = {}
        self.sdk = None
        self.abi = None
        self.arch_label = None
        self.variant = None
        self.layout = None
        self._tars = []
        self._tmp = tempfile.TemporaryDirectory(prefix="steamarm-gapps-")
        self.unpacked = 0
        try:
            self.size = os.path.getsize(self.path)
            self._f = open(self.path, "rb")
        except OSError as e:
            self._tmp.cleanup()
            raise GappsError(EXIT_INPUT, "unreadable", "cannot read %s: %s" % (self.path, e.strerror or e))
        try:
            self.zf = zipfile.ZipFile(self._f)
            self.infos = self.zf.infolist()
            if len(self.infos) > MAX_ENTRIES:
                raise GappsError(EXIT_INPUT, "too-large", "more than %d entries" % MAX_ENTRIES)
            self._read()
        except GappsError:
            self.close()
            raise
        except (zipfile.BadZipFile, zlib.error, EOFError, NotImplementedError, RuntimeError,
                ValueError, struct.error, tarfile.TarError) as e:
            self.close()
            raise GappsError(EXIT_INPUT, "not-a-package", "%s is not a readable zip: %s" % (self.path, e))

    def close(self):
        for t in self._tars:
            t.close()
        self._tars = []
        if getattr(self, "zf", None) is not None:
            self.zf.close()
        if getattr(self, "_f", None) is not None:
            self._f.close()
        self._tmp.cleanup()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    @property
    def sha256(self):
        if getattr(self, "_sha256", None) is None:
            self._sha256 = sha256_file(self.path)
        return self._sha256

    # -- reading

    def _small(self, info, limit=MAX_META):
        if info.file_size > limit:
            raise GappsError(EXIT_INPUT, "too-large", "%s is larger than %d bytes" % (info.filename, limit))
        with self.zf.open(info) as f:
            data = f.read(limit + 1)
        if len(data) > limit:
            raise GappsError(EXIT_INPUT, "too-large", "%s is larger than %d bytes" % (info.filename, limit))
        return data

    def open_item(self, item):
        if item.member is not None:
            f = item.tar.extractfile(item.member)
            if f is None:
                raise GappsError(EXIT_INPUT, "bad-archive", "%s: not a regular file" % item.source)
            return f
        return self.zf.open(item.zinfo)

    def item_bytes(self, item, limit):
        with self.open_item(item) as f:
            data = f.read(limit + 1)
        if len(data) > limit:
            raise GappsError(EXIT_INPUT, "too-large", "%s is larger than %d bytes" % (item.source, limit))
        return data

    def _read(self):
        names = {}
        for info in self.infos:
            if info.filename in names:
                self.findings.append(finding("warning", "duplicate-entry",
                                             "%s appears twice in the zip; the last copy is used" % info.filename))
            names[info.filename] = info
            if safe_parts(info.filename) is None or zip_is_symlink(info):
                self.unsafe.append(info.filename)
        unsafe = set(self.unsafe)
        safe = [i for i in self.infos if i.filename not in unsafe]
        top = {i.filename for i in safe if not i.is_dir()}

        if "AndroidManifest.xml" in top:
            raise GappsError(EXIT_UNSUPPORTED, "single-apk",
                             "this is a single APK, not a GApps package: scripts/android-pm.py installs APKs")
        images = sorted(n for n in top if re.fullmatch(r"(system|vendor|product|system_ext|super)\.img", n))
        if images:
            raise GappsError(EXIT_UNSUPPORTED, "system-image",
                             "this holds a partition image (%s), not a GApps package; SteamARM's root is built by "
                             "scripts/mkandroidroot.sh from Waydroid's VANILLA images, and this program does not "
                             "read a GAPPS image" % ", ".join(images))
        archives = sorted((i for i in safe if not i.is_dir()
                           and re.fullmatch(r"[^/]+/[^/]+\.tar(\.lz|\.xz)?", i.filename)), key=lambda i: i.filename)
        gprop = parse_props(self._small(names["g.prop"])) if "g.prop" in top else {}
        if gprop.get("ro.addon.type") == "gapps" or archives:
            self.layout = "opengapps"
            self._read_opengapps(safe, archives, gprop)
        elif any(n.startswith("system/") for n in top):
            bp = parse_props(self._small(names["build.prop"])) if "build.prop" in top else {}
            if bp.get("arch") and bp.get("version"):
                self.layout = "mindthegapps"
                self.meta = {"arch": bp.get("arch"), "version": bp.get("version"),
                             "version_nice": bp.get("version_nice")}
                self.arch_label = bp.get("arch")
                self.abi = norm_abi(bp.get("arch"))
                self.sdk = as_int(bp.get("version"))
            else:
                self.layout = "system-tree"
            self._read_system_tree(safe)
        else:
            raise GappsError(EXIT_UNSUPPORTED, "unknown-layout",
                             "neither a MindTheGapps zip (build.prop + system/) nor an OpenGApps zip "
                             "(g.prop + Core/*.tar.lz); nothing here is read as installable")
        self._from_file_name()
        self._dedupe()

    def _from_file_name(self):
        """The zip's own name, when its metadata does not say (a hint only):
        MindTheGapps-11.0.0-arm64-<date>.zip, open_gapps-x86_64-11.0-pico-<date>.zip."""
        base = os.path.basename(self.path)
        m = re.match(r"MindTheGapps-(\d+(?:\.\d+)*)-([A-Za-z0-9_]+?)(?:-ATV)?-\d", base)
        o = re.match(r"open_gapps-([A-Za-z0-9_]+)-(\d+(?:\.\d+)?|12L)-[a-z_]+-", base)
        release = arch = None
        if m:
            release, arch = m.group(1), m.group(2)
        elif o:
            arch, release = o.group(1), o.group(2)
        if self.abi is None and arch and norm_abi(arch):
            self.abi, self.arch_label = norm_abi(arch), arch
            self.findings.append(finding("note", "arch-from-name",
                                         "architecture taken from the file name (%s): the package does not declare it"
                                         % arch))
        if self.sdk is None and release:
            key = release if release == "12L" else ".".join((release.split(".") + ["0"])[:2])
            sdk = RELEASE_SDK.get(key) or RELEASE_SDK.get(key.split(".")[0] + ".0")
            if sdk:
                self.sdk = sdk
                self.findings.append(finding("note", "sdk-from-name",
                                             "Android version taken from the file name (%s): the package does not "
                                             "declare it" % release))

    def _read_system_tree(self, safe):
        for info in safe:
            if info.is_dir():
                continue
            parts = safe_parts(info.filename)
            if parts[0] != "system":
                self.skipped.append({"name": info.filename, "why": self._installer_why(info.filename)})
                continue
            item = Item("/".join(parts), info.file_size, zinfo=info)
            if item.category is None:
                self.skipped.append({"name": info.filename, "why": "addon.d: an OTA backup script for recovery"})
                continue
            self.items.append(item)

    @staticmethod
    def _installer_why(name):
        if name.startswith("META-INF/"):
            return "the recovery installer (update-binary, signature)"
        if re.fullmatch(r"(busybox|tar|unzip|zip|xzdec)-(arm|arm64|x86|x86_64)", name):
            return "a tool of the recovery installer"
        return INSTALLER_FILES.get(name, "outside system/: not installed by the installer")

    def _unpack(self, info):
        out = os.path.join(self._tmp.name, "archive-%04d.tar" % len(self._tars))
        name = info.filename
        with self.zf.open(info) as src, open(out, "wb") as dst:
            budget = MAX_TOTAL - self.unpacked
            if name.endswith(".lz"):
                n = lzip_decompress(src, dst, budget, name)
            elif name.endswith(".xz"):
                n = xz_decompress(src, dst, budget, name)
            else:
                n = copy_capped(src, dst, budget, name)[0]
        self.unpacked += n
        return out

    def _read_opengapps(self, safe, archives, gprop):
        self.meta = {k: v for k, v in gprop.items() if k.startswith("ro.addon.")}
        self.arch_label = gprop.get("ro.addon.arch")
        self.abi = norm_abi(gprop.get("ro.addon.arch"))
        self.sdk = as_int(gprop.get("ro.addon.sdk"))
        self.variant = gprop.get("ro.addon.open_type")
        archive_names = {i.filename for i in archives}
        for info in safe:
            if not info.is_dir() and info.filename not in archive_names:
                self.skipped.append({"name": info.filename, "why": self._installer_why(info.filename)})
        for info in archives:
            app = re.sub(r"\.tar(\.lz|\.xz)?$", "", os.path.basename(info.filename))
            m = re.search(r"-(?:lib-)?(arm64|arm|x86_64|x86|mips64|mips|all|common)$", app)
            arch = m.group(1) if m else None
            tar = tarfile.open(self._unpack(info), "r:")
            self._tars.append(tar)
            members = []
            for n, mem in enumerate(tar):
                if n >= MAX_ENTRIES:
                    raise GappsError(EXIT_INPUT, "too-large", "%s: more than %d members" % (info.filename, MAX_ENTRIES))
                members.append(mem)
            groups = {}
            for mem in members:
                parts = safe_parts(mem.name)
                if parts is None or not (mem.isfile() or mem.isdir()):
                    self.unsafe.append("%s!%s" % (info.filename, mem.name))
                    continue
                if mem.isdir():
                    continue
                if len(parts) < 3 or parts[0] != app:
                    self.findings.append(finding("warning", "unexpected-member",
                                                 "%s!%s is not <app>/<density>/<path>: not installed"
                                                 % (info.filename, mem.name)))
                    continue
                groups.setdefault(parts[1], []).append((mem, parts))
            densities = sorted(k for k in groups if k != "common")
            chosen = choose_density(densities, self.density)
            for k in densities:
                if k != chosen and not re.fullmatch(r"nodpi|\d+(?:-\d+)*", k):
                    self.findings.append(finding("warning", "unexpected-member",
                                                 "%s: directory %r is neither a density nor 'common': not installed"
                                                 % (info.filename, k)))
            for key in ("common", chosen):
                for mem, parts in groups.get(key, []) if key else []:
                    item = Item("system/" + "/".join(parts[2:]), mem.size, tar=tar, member=mem,
                                archive=info.filename, app=app)
                    if item.category is None:
                        continue
                    self.items.append(item)
            self.archives.append({"name": info.filename, "app": app, "arch": arch, "densities": densities,
                                  "chosen": chosen})

    def _dedupe(self):
        seen = {}
        for item in self.items:
            if item.dest in seen:
                self.findings.append(finding("warning", "duplicate-destination",
                                             "%s comes from both %s and %s; the later one is used"
                                             % (item.dest, seen[item.dest].source, item.source)))
            seen[item.dest] = item
        self.items = sorted(seen.values(), key=lambda i: i.dest)
        lower = {}
        for item in self.items:
            lower.setdefault(item.dest.lower(), []).append(item.dest)
        for paths in lower.values():
            if len(paths) > 1:
                self.findings.append(finding("warning", "case-collision",
                                             "%s differ only by case: a case-insensitive volume would merge them "
                                             "(SteamARM's Android volume is case-sensitive)" % ", ".join(paths)))

    # -- APKs and XML

    def inspect_apks(self):
        """scripts/apk-inspect.py on every APK the package installs."""
        for item in self.items:
            if not item.dest.endswith(".apk"):
                continue
            fd, tmp = tempfile.mkstemp(suffix=".apk", dir=self._tmp.name)
            try:
                with os.fdopen(fd, "wb") as dst, self.open_item(item) as src:
                    copy_capped(src, dst, MAX_FILE, item.source)
                try:
                    rep = apk_inspect.inspect(tmp)
                except apk_inspect.UnsupportedBundle as e:
                    item.apk = {"error": "%s: %s" % (e.format, e.reason)}
                    continue
                except apk_inspect.APKError as e:
                    item.apk = {"error": str(e)}
                    continue
                signing = rep.get("signing") or {}
                item.apk = {
                    "package": rep.get("package"), "versionName": rep.get("versionName"),
                    "versionCode": rep.get("versionCode"), "minSdk": rep.get("minSdk"),
                    "targetSdk": rep.get("targetSdk"), "abis": rep.get("abis") or [],
                    "hasCode": rep.get("hasCode"), "sha256": rep.get("sha256"),
                    "permissions": sorted({p.get("name") for p in rep.get("permissions") or []
                                           if isinstance(p, dict) and p.get("name")}),
                    "signers": [c[:16] for c in signing.get("certificates") or []],
                    "signatureVerified": False,
                }
            finally:
                try:
                    os.unlink(tmp)
                except OSError:
                    pass

    def xml_items(self, category):
        """(item, root element) of each well-formed XML file of a category.
        A DOCTYPE or ENTITY is refused rather than expanded."""
        out = []
        for item in self.items:
            if item.category != category or not item.dest.endswith(".xml"):
                continue
            try:
                data = self.item_bytes(item, MAX_XML)
            except GappsError as e:
                self.findings.append(finding("warning", "xml-unreadable", "%s: %s" % (item.dest, e.message)))
                continue
            if b"<!DOCTYPE" in data or b"<!ENTITY" in data:
                self.findings.append(finding("warning", "xml-refused",
                                             "%s declares a DOCTYPE or ENTITY; not parsed" % item.dest))
                continue
            try:
                out.append((item, ET.fromstring(data)))
            except ET.ParseError as e:
                self.findings.append(finding("warning", "xml-unreadable", "%s: %s" % (item.dest, e)))
        return out

    def permission_config(self):
        """privapp-permissions, libraries and features of etc/permissions;
        default grants of etc/default-permissions; sysconfig's package lists.
        Read once (after any --exclude)."""
        if getattr(self, "_perm", None) is None:
            self._perm = self._permission_config()
        return self._perm

    def _permission_config(self):
        privapp, libraries, features = {}, [], []
        for item, root in self.xml_items("permissions"):
            for el in root.iter("privapp-permissions"):
                pkg = el.get("package")
                if not pkg:
                    continue
                entry = privapp.setdefault(pkg, {"allow": set(), "deny": set(), "files": set()})
                entry["files"].add(item.dest)
                for p in el:
                    if p.tag == "permission" and p.get("name"):
                        entry["allow"].add(p.get("name"))
                    elif p.tag == "deny-permission" and p.get("name"):
                        entry["deny"].add(p.get("name"))
            for el in root.iter("library"):
                if el.get("name"):
                    libraries.append({"name": el.get("name"), "file": el.get("file"), "declaredIn": item.dest})
            for el in root.iter("feature"):
                if el.get("name"):
                    features.append(el.get("name"))
        grants = {}
        for item, root in self.xml_items("defaultPermissions"):
            for el in root.iter("exception"):
                pkg = el.get("package")
                if pkg:
                    grants.setdefault(pkg, set()).update(p.get("name") for p in el.iter("permission") if p.get("name"))
        sysconfig = {}
        for item, root in self.xml_items("sysconfig"):
            for el in root:
                if el.tag == "feature" and el.get("name"):
                    features.append(el.get("name"))
                elif el.get("package"):
                    sysconfig.setdefault(el.tag, set()).add(el.get("package"))
        return privapp, libraries, sorted(set(features)), grants, sysconfig

    def exclude(self, names):
        """Leave out whole apps: by app directory (Velvet), by package name
        (com.google.android.googlequicksearchbox) or by OpenGApps archive
        (setupwizarddefault or setupwizarddefault-all)."""
        names = [n for n in (names or []) if n]
        if not names:
            return []
        want, matched, keys = set(names), set(), set()
        for item in self.items:
            pkg = (item.apk or {}).get("package")
            base_app = re.sub(r"-(?:lib-)?[A-Za-z0-9_]+$", "", item.app) if item.app else None
            for n in want & {item.appdir, pkg}:
                if item.appdir:
                    keys.add(("dir", item.partition, item.appdir))
                    matched.add(n)
            for n in want & {item.app, base_app}:
                keys.add(("archive", item.app))
                matched.add(n)
        out, keep = [], []
        for item in self.items:
            hit = ("dir", item.partition, item.appdir) in keys or ("archive", item.app) in keys
            (out if hit else keep).append(item)
        self.items = keep
        self._perm = None
        for n in names:
            if n not in matched:
                self.findings.append(finding("warning", "exclude-unmatched", "--exclude %s matched nothing" % n))
        for i in out:
            pkg = (i.apk or {}).get("package")
            if pkg in KEY_NAMES:
                self.findings.append(finding("warning", "key-component-excluded",
                                             "%s (%s) is excluded: Play Store cannot work without it"
                                             % (KEY_NAMES[pkg], pkg)))
        return sorted(i.dest for i in out)


# ------------------------------------------------------------------ checks

def app_abis(pkg, item):
    """The ABIs of an app: its APK's lib/<abi>/ plus <app dir>/lib/<arch>/."""
    abis = set((item.apk or {}).get("abis") or [])
    if item.appdir:
        for other in pkg.items:
            if other.category == "libs" and other.appdir == item.appdir and other.partition == item.partition:
                m = re.search(r"/lib/([^/]+)/[^/]+$", other.dest)
                if m:
                    abis.add(norm_abi(m.group(1)) or m.group(1))
    return sorted(abis)


def check(pkg, target):
    """Findings: errors make the package unsuitable for the root, warnings are
    what the owner should know, notes explain."""
    privapp = pkg.permission_config()[0]            # its XML warnings join pkg.findings
    out = list(pkg.findings)
    for name in pkg.unsafe:
        out.append(finding("error", "unsafe-path",
                           "%s: an absolute path, '..', a backslash, a link or a device; nothing from this package "
                           "will be written" % name, entry=name))
    # API level.
    if pkg.sdk is None:
        out.append(finding("warning", "package-api-unknown",
                           "the package does not say which Android version it is for; the root is Android %s "
                           "(API %d)" % (release_of(target.sdk), target.sdk)))
    elif pkg.sdk != target.sdk:
        out.append(finding("error", "api-mismatch",
                           "the package is for Android %s (API %d), the root is Android %s (API %d)"
                           % (release_of(pkg.sdk), pkg.sdk, release_of(target.sdk), target.sdk)))
    # Architecture.
    runnable = target.runnable()
    if pkg.abi is None:
        out.append(finding("warning", "package-arch-unknown",
                           "the package does not say which architecture it is for (%s)" % (pkg.arch_label or "none")))
    elif not target.abilist:
        out.append(finding("warning", "target-unknown",
                           "no Android root found and no --arch: the package (%s) was not checked against a root"
                           % pkg.abi))
    elif pkg.abi not in target.abilist:
        out.append(finding("error", "abi-mismatch",
                           "the package is built for %s, the root is %s (%s)"
                           % (pkg.abi, target.abi, ", ".join(target.abilist))))
    elif pkg.abi not in runnable:
        out.append(finding("error", "abi-mismatch",
                           "the package is built for %s: Apple silicon has no AArch32, so the root cannot run it"
                           % pkg.abi))
    elif pkg.abi != target.abi:
        out.append(finding("warning", "abi-secondary",
                           "the package is built for %s, the root's secondary ABI: its apps would run as 32-bit "
                           "processes, and FEX's 32-bit mode does not run the image's own i386 services yet" % pkg.abi))
    if target.abi == "arm64-v8a":
        out.append(finding("note", "arm64-root-no-java",
                           "the arm64 root cannot start ART yet (the ART heap wall, "
                           "docs/ANDROID_RUNTIME_ARCHITECTURE.md): no Java app, Google's included, runs there"))
    # Per APK.
    present = {}
    for item in pkg.items:
        a = item.apk
        if a is None:
            continue
        if a.get("error"):
            out.append(finding("warning", "apk-unreadable", "%s: %s" % (item.dest, a["error"])))
            continue
        package = a.get("package")
        level = "error" if package in KEY_NAMES else "warning"
        if item.category in ("privApps", "apps") and package:
            present.setdefault(package, item)
        abis = app_abis(pkg, item)
        if abis and runnable and not set(abis) & set(runnable):
            out.append(finding(level, "apk-abi-mismatch",
                               "%s (%s) ships native code only for %s; the root runs %s and has no native bridge"
                               % (package, item.dest, ", ".join(abis), ", ".join(runnable)), package=package))
        elif abis and runnable and target.abi not in abis:
            out.append(finding("warning", "apk-abi-secondary",
                               "%s ships native code for %s, not for the root's primary %s"
                               % (package, ", ".join(abis), target.abi), package=package))
        min_sdk = a.get("minSdk")
        if isinstance(min_sdk, int) and min_sdk > target.sdk:
            out.append(finding(level, "apk-min-sdk",
                               "%s needs API %d, the root is API %d: Android would refuse it "
                               "(INSTALL_FAILED_OLDER_SDK)" % (package, min_sdk, target.sdk), package=package))
    for package, name in KEY_PACKAGES:
        if package not in present:
            out.append(finding("warning", "key-component-missing",
                               "no %s (%s) in the package: Play Store cannot work without it" % (name, package)))
    # Privileged permissions (PermissionManagerService: with
    # ro.control_privapp_permissions=enforce, as LineageOS sets it, a
    # privileged app whose privileged permissions are not allow-listed stops
    # system_server at boot).
    for item in pkg.items:
        if item.category == "privApps" and item.apk and item.apk.get("package") and \
                item.apk["package"] not in privapp:
            out.append(finding("warning", "privapp-allowlist-missing",
                               "%s is a privileged app with no privapp-permissions entry in the package; with "
                               "ro.control_privapp_permissions=enforce system_server stops at boot if it asks for "
                               "a privileged permission" % item.apk["package"], package=item.apk["package"]))
    # Files an installer would put in odd places.
    for item in pkg.items:
        if item.partition == "vendor":
            out.append(finding("warning", "vendor-file", "%s writes into /vendor" % item.dest))
        elif item.category == "other":
            extra = " (an init script: scripts/android-boot.py reads .rc files there)" \
                if re.search(r"/etc/init/[^/]+\.rc$", item.dest) else ""
            out.append(finding("note", "other-file", "%s is installed as it is%s" % (item.dest, extra)))
    if any(i.category == "privApps" and "SetupWizard" in (i.appdir or "") for i in pkg.items):
        out.append(finding("note", "setup-wizard",
                           "the package has a Google setup wizard; MindTheGapps' installer then deletes "
                           "system_ext/priv-app/Provision. The overlay never deletes: apply-plan shows the step "
                           "for the derived root"))
    return out


def has_errors(findings):
    return any(f["level"] == "error" for f in findings)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(CHUNK), b""):
            h.update(b)
    return h.hexdigest()


def report(pkg, target, findings, excluded=()):
    privapp, libraries, features, grants, sysconfig = pkg.permission_config()
    installed = {c: [] for c in CATEGORY_NAMES}
    for item in pkg.items:
        d = item.as_dict()
        pkgname = (item.apk or {}).get("package")
        if item.category == "privApps" and pkgname:
            requested = set(item.apk.get("permissions") or [])
            entry = privapp.get(pkgname, {"allow": set(), "deny": set(), "files": set()})
            d["privileged"] = {
                "allowListed": sorted(entry["allow"]),
                "granted": sorted(requested & entry["allow"]),
                "denied": sorted(entry["deny"]),
                "allowListedNotRequested": sorted(entry["allow"] - requested),
                "declaredIn": sorted(entry["files"]),
            }
        if pkgname in grants:
            d["defaultGrants"] = sorted(grants[pkgname])
        installed[item.category].append(d)
    dests = {i.dest for i in pkg.items}
    for lib in libraries:
        f = (lib.get("file") or "").lstrip("/")       # /system/framework/x.jar, /product/framework/x.jar
        lib["inPackage"] = bool(f) and (f if f.startswith("system/") else "system/" + f) in dests
    key = []
    for package, name in KEY_PACKAGES:
        hit = next((i for i in pkg.items if (i.apk or {}).get("package") == package
                    and i.category in ("privApps", "apps")), None)
        key.append({"package": package, "name": name, "present": hit is not None,
                    "path": hit.dest if hit else None,
                    "versionName": hit.apk.get("versionName") if hit else None,
                    "versionCode": hit.apk.get("versionCode") if hit else None})
    return {
        "tool": "scripts/android-gapps.py",
        "ok": not has_errors(findings),
        "package": {
            "fileName": os.path.basename(pkg.path), "size": pkg.size, "sha256": pkg.sha256,
            "layout": pkg.layout, "metadata": pkg.meta, "sdk": pkg.sdk, "android": release_of(pkg.sdk),
            "abi": pkg.abi, "archLabel": pkg.arch_label, "variant": pkg.variant,
        },
        "target": target.as_dict(),
        "licence": LICENCE,
        "keyComponents": key,
        "install": installed,
        "counts": {c: len(v) for c, v in installed.items()},
        "totalBytes": sum(i.size for i in pkg.items),
        "excluded": list(excluded),
        "libraries": libraries,
        "features": features,
        "sysconfig": {k: sorted(v) for k, v in sorted(sysconfig.items())},
        "archives": pkg.archives,
        "notInstalled": pkg.skipped,
        "findings": findings,
    }


# ------------------------------------------------------------------ text output

def _short_list(xs, verbose, n=6):
    xs = list(xs)
    if verbose or len(xs) <= n:
        return ", ".join(xs)
    return ", ".join(xs[:n]) + ", ... (%d more; -v lists all)" % (len(xs) - n)


def render_inspect(rep, verbose=False):
    p, t = rep["package"], rep["target"]
    lines = ["GApps package: %s (%s bytes, sha256 %s)" % (p["fileName"], format(p["size"], ","), p["sha256"]),
             "Layout: %s %s" % ({"mindthegapps": "MindTheGapps", "opengapps": "OpenGApps",
                                 "system-tree": "a system/ tree with no metadata"}.get(p["layout"], p["layout"]),
                                " ".join("%s=%s" % kv for kv in sorted((p["metadata"] or {}).items()))),
             "Built for: Android %s (API %s), %s%s" % (p["android"], p["sdk"] if p["sdk"] else "?",
                                                       p["abi"] or "architecture unknown",
                                                       ", variant %s" % p["variant"] if p["variant"] else ""),
             ("Target root: %s, API %s (from %s%s)" % (", ".join(t["abilist"]), t["sdk"], t["source"],
                                                       ": %s" % t["root"] if t["root"] else "")
              if t["abilist"] else
              "Target root: none found and no --arch (API %s assumed); pass --arch or --root" % t["sdk"]),
             "Licence: %s" % rep["licence"], "", "Key components:"]
    for k in rep["keyComponents"]:
        lines.append("  %-26s %-24s %s" % (k["name"], k["package"],
                                           "%s (%s)  %s" % (k["versionName"], k["versionCode"], k["path"])
                                           if k["present"] else "MISSING"))
    lines += ["", "Would install %d files, %s bytes:" % (sum(rep["counts"].values()),
                                                         format(rep["totalBytes"], ","))]
    for cat, title in CATEGORIES:
        items = rep["install"][cat]
        if not items:
            continue
        lines.append("  %s (%d):" % (title, len(items)))
        for d in items:
            a = d.get("apk")
            if a and not a.get("error"):
                lines.append("    %s  %s %s (%s) minSdk %s, ABIs %s" % (
                    d["path"], a["package"], a["versionName"], a["versionCode"], a["minSdk"],
                    ", ".join(a["abis"]) or "none"))
                if "privileged" in d and not d["privileged"]["declaredIn"]:
                    lines.append("      privileged: no privapp-permissions entry in the package")
                elif "privileged" in d:
                    pv = d["privileged"]
                    lines.append("      privileged: %d allow-listed, %d of them requested%s" % (
                        len(pv["allowListed"]), len(pv["granted"]),
                        (": " + _short_list(pv["granted"], verbose)) if pv["granted"] else ""))
                    if pv["denied"]:
                        lines.append("      denied: %s" % _short_list(pv["denied"], verbose))
                if d.get("defaultGrants"):
                    lines.append("      default grants: %d" % len(d["defaultGrants"]))
            elif a:
                lines.append("    %s  (not readable: %s)" % (d["path"], a["error"]))
            else:
                lines.append("    %s" % d["path"])
    if rep["libraries"]:
        lines.append("  shared libraries declared: %s" % ", ".join(
            "%s%s" % (l["name"], "" if l["inPackage"] else " (jar not in package)") for l in rep["libraries"]))
    if rep["excluded"]:
        lines += ["", "Excluded (%d): %s" % (len(rep["excluded"]), _short_list(rep["excluded"], verbose))]
    if rep["notInstalled"]:
        lines += ["", "Not installed (%d installer or metadata files):" % len(rep["notInstalled"])]
        shown = rep["notInstalled"] if verbose else rep["notInstalled"][:8]
        lines += ["  %s: %s" % (s["name"], s["why"]) for s in shown]
        if len(shown) < len(rep["notInstalled"]):
            lines.append("  ... (%d more; -v lists all)" % (len(rep["notInstalled"]) - len(shown)))
    lines += ["", "Findings:"]
    order = {"error": 0, "warning": 1, "note": 2}
    for f in sorted(rep["findings"], key=lambda f: order[f["level"]]):
        lines.append("  %-7s %s: %s" % (f["level"].upper(), f["code"], f["message"]))
    if not rep["findings"]:
        lines.append("  none")
    errors = sum(1 for f in rep["findings"] if f["level"] == "error")
    lines += ["", "Verdict: %s" % ("suitable for the root" if not errors else
                                   "NOT suitable for the root (%d error%s)" % (errors, "s" if errors > 1 else ""))]
    return "\n".join(lines)


# ------------------------------------------------------------------ overlay

def refuse_destination(out, target):
    real = os.path.realpath(out)
    if inside(real, REPO):
        raise GappsError(EXIT_REFUSED, "inside-repository",
                         "%s is inside the SteamARM repository: Google's files must never be committed; "
                         "choose a directory outside it" % out)
    probe = real
    while True:
        if os.path.isfile(os.path.join(probe, ROOT_MARK)):
            raise GappsError(EXIT_REFUSED, "inside-android-root",
                             "%s is inside the Android root %s, which stays pristine; the overlay goes next to it"
                             % (out, probe))
        parent = os.path.dirname(probe)
        if parent == probe:
            break
        probe = parent
    if target.root and inside(real, target.root):
        raise GappsError(EXIT_REFUSED, "inside-android-root",
                         "%s is inside the Android root %s, which stays pristine" % (out, target.root))
    if os.path.lexists(real):
        if not os.path.isdir(real) or os.path.islink(out):
            raise GappsError(EXIT_REFUSED, "not-a-directory", "%s exists and is not a directory" % out)
        if os.listdir(real) and not os.path.isfile(os.path.join(real, MANIFEST_NAME)):
            raise GappsError(EXIT_REFUSED, "not-empty",
                             "%s is not empty and is not an overlay made by this program; nothing was changed" % out)


def write_overlay(pkg, out, target, findings, excluded):
    """Write every item under a staging directory next to `out`, then swap it
    into place; an earlier overlay there is replaced as a whole."""
    real = os.path.realpath(out)
    parent = os.path.dirname(real)
    os.makedirs(parent, exist_ok=True)
    stage = tempfile.mkdtemp(prefix=".gapps-overlay-", dir=parent)
    try:
        files, total = [], 0
        for item in pkg.items:
            parts = safe_parts(item.dest)
            if parts is None:
                raise GappsError(EXIT_INPUT, "unsafe-path", "%s would leave the overlay" % item.dest)
            dst = os.path.join(stage, *parts)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if not inside(os.path.dirname(dst), stage):      # nothing here makes links; belt and braces
                raise GappsError(EXIT_INPUT, "unsafe-path", "%s would leave the overlay" % item.dest)
            with pkg.open_item(item) as src, open(dst, "xb") as f:
                n, digest = copy_capped(src, f, MAX_FILE, item.source)
            total += n
            if total > MAX_TOTAL:
                raise GappsError(EXIT_INPUT, "too-large", "the package installs more than %d bytes" % MAX_TOTAL)
            os.chmod(dst, 0o644)
            entry = {"path": item.dest, "size": n, "sha256": digest, "category": item.category}
            if item.apk and item.apk.get("package"):
                entry["package"] = item.apk["package"]
                entry["versionName"] = item.apk.get("versionName")
                entry["versionCode"] = item.apk.get("versionCode")
            files.append(entry)
        for d, _dirs, _files in os.walk(stage):
            os.chmod(d, 0o755)
        manifest = {
            "schema": 1,
            "tool": "scripts/android-gapps.py",
            "source": {"fileName": os.path.basename(pkg.path), "size": pkg.size, "sha256": pkg.sha256,
                       "layout": pkg.layout, "metadata": pkg.meta, "sdk": pkg.sdk, "abi": pkg.abi},
            "target": target.as_dict(),
            "licence": LICENCE,
            "excluded": list(excluded),
            "setupWizard": any(i.category == "privApps" and "SetupWizard" in (i.appdir or "") for i in pkg.items),
            "files": files,
            "findings": findings,
        }
        with open(os.path.join(stage, MANIFEST_NAME), "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=2, sort_keys=True, ensure_ascii=False)
            f.write("\n")
        os.chmod(os.path.join(stage, MANIFEST_NAME), 0o644)
        if os.path.isdir(real):
            if not os.listdir(real):
                os.rmdir(real)
                os.rename(stage, real)
            else:
                old = tempfile.mkdtemp(prefix=".gapps-overlay-old-", dir=parent)
                os.rename(real, os.path.join(old, "previous"))
                try:
                    os.rename(stage, real)
                except OSError:
                    os.rename(os.path.join(old, "previous"), real)
                    raise
                shutil.rmtree(old, ignore_errors=True)
        else:
            os.rename(stage, real)
        stage = None
        return manifest
    except OSError as e:
        raise GappsError(EXIT_IO, "io", "writing the overlay failed: %s" % e)
    finally:
        if stage is not None:
            shutil.rmtree(stage, ignore_errors=True)


# ------------------------------------------------------------------ apply-plan

def q(s):
    return shlex.quote(str(s))


def load_overlay(path):
    man_path = os.path.join(path, MANIFEST_NAME)
    try:
        with open(man_path, "rb") as f:
            data = f.read(MAX_META * 64)
        man = json.loads(data.decode("utf-8"))
    except (OSError, ValueError) as e:
        raise GappsError(EXIT_INPUT, "not-an-overlay",
                         "%s has no readable %s: make it with 'android-gapps.py overlay' (%s)"
                         % (path, MANIFEST_NAME, e))
    if not isinstance(man, dict) or man.get("schema") != 1 or not isinstance(man.get("files"), list):
        raise GappsError(EXIT_INPUT, "not-an-overlay", "%s: unknown overlay manifest" % man_path)
    return man


def plan(overlay, root=None, derived=None):
    overlay = os.path.realpath(os.path.expanduser(overlay))
    man = load_overlay(overlay)
    tgt = man.get("target") or {}
    root = os.path.realpath(os.path.expanduser(root or tgt.get("root") or default_root()))
    abi = tgt.get("abi") or man["source"].get("abi")
    derived = os.path.abspath(os.path.expanduser(
        derived or os.path.join(os.path.dirname(root), "gapps-%s" % SHORT_ARCH.get(abi, "root"))))
    notes = []
    changed = []
    for f in man["files"]:
        parts = safe_parts(f.get("path")) if isinstance(f, dict) else None
        if parts is None:
            raise GappsError(EXIT_INPUT, "unsafe-path", "the overlay manifest names %r" % (f,))
        p = os.path.join(overlay, *parts)
        try:
            if os.path.getsize(p) != f.get("size"):
                changed.append(f["path"])
        except OSError:
            changed.append(f["path"])
    if changed:
        notes.append(finding("error", "overlay-changed",
                             "%d files differ from the overlay's manifest (%s): make the overlay again"
                             % (len(changed), ", ".join(changed[:5]))))
    if derived == root or inside(derived, root):
        raise GappsError(EXIT_REFUSED, "derived-is-root", "the derived root must not be the pristine root or inside it")
    if inside(derived, REPO):
        raise GappsError(EXIT_REFUSED, "inside-repository", "the derived root must not be inside the repository")
    root_present = os.path.isdir(os.path.join(root, "system"))
    replaced = []
    if root_present:
        if not os.path.isfile(os.path.join(root, ROOT_MARK)):
            notes.append(finding("warning", "root-unmarked",
                                 "%s has no %s: not made by scripts/mkandroidroot.sh" % (root, ROOT_MARK)))
        replaced = [f["path"] for f in man["files"] if os.path.lexists(os.path.join(root, *f["path"].split("/")))]
        if replaced:
            notes.append(finding("note", "replaces-files",
                                 "%d overlay files replace files of the image in the derived root: %s"
                                 % (len(replaced), ", ".join(replaced[:8]))))
        for part in ("system/product", "system/system_ext"):
            used = any(f["path"].startswith(part + "/") for f in man["files"])
            if used and not os.path.isdir(os.path.join(root, part)):
                notes.append(finding("warning", "partition-missing",
                                     "the overlay writes under %s, which the root does not have as a directory"
                                     % part))
        try:
            if os.stat(root).st_dev != os.stat(overlay).st_dev:
                notes.append(finding("note", "different-volume",
                                     "the overlay is on another volume than the root: cp -c clones only within one "
                                     "APFS volume, so step 3 copies (use cp -R there, or keep the overlay on the "
                                     "Android volume)"))
        except OSError:
            pass
    else:
        notes.append(finding("note", "root-absent", "%s is not here: the plan is printed for it anyway" % root))
    if os.path.lexists(derived):
        notes.append(finding("warning", "derived-exists",
                             "%s already exists: remove it (step 8) or pass another --derived" % derived))
    if abi == "x86_64" and len(derived.encode()) > MAX_X86_ROOT_PATH:
        notes.append(finding("warning", "derived-path-too-long",
                             "%s is %d bytes; an x86_64 root's path must be at most %d bytes, or its FEXServer "
                             "socket does not fit Darwin's sun_path" % (derived, len(derived.encode()),
                                                                         MAX_X86_ROOT_PATH)))
    if abi == "arm64-v8a":
        notes.append(finding("note", "arm64-root-no-java",
                             "the arm64 root does not start ART yet: nothing Google ships can run there"))
    env = "ANDROID_X86_ROOT=%s " % q(derived) if abi == "x86_64" else ""
    steps = [
        ("Stop everything that runs on the pristine root (an android-boot.py session ends its own services).",
         ["ANDROID_X86_ROOT=%s scripts/run-android-x86.sh --server-stop" % q(root)] if abi == "x86_64" else []),
        ("Make the derived root: an APFS clone of the pristine one (copy-on-write, instant, no extra space until "
         "files change; the same volume).",
         ["cp -cR %s %s" % (q(root), q(derived))]),
        ("Layer the overlay into the clone. With the trailing slash BSD cp copies the directory's contents into "
         "system/, merging.",
         ["cp -cR %s %s" % (q(os.path.join(overlay, "system") + "/"), q(os.path.join(derived, "system")))]),
    ]
    if man.get("setupWizard"):
        steps.append(("The package has a Google setup wizard: remove AOSP's Provision in the derived root, as "
                      "MindTheGapps' installer does (never in the pristine root).",
                      ["rm -rf %s" % q(os.path.join(derived, "system/system_ext/priv-app/Provision"))]))
    steps += [
        ("Record what was layered.",
         ["cp %s %s" % (q(os.path.join(overlay, MANIFEST_NAME)), q(os.path.join(derived, MANIFEST_NAME)))]),
        ("Boot the derived root (its /data, and so the GSF ID, stays its own).",
         ["%sscripts/android-boot.py --root %s --seconds 900" % (env, q(derived))]),
        ("Once Google Play services has checked in, read the GSF ID and register it yourself.",
         ["scripts/android-gapps.py gsf-id --root %s" % q(derived)]),
        ("To undo: delete the derived root. The pristine root and the overlay are untouched.",
         ["rm -rf %s" % q(derived)]),
    ]
    return {"ok": not has_errors(notes), "overlay": overlay, "root": root, "derived": derived, "abi": abi,
            "rootPresent": root_present, "files": len(man["files"]), "replaced": replaced,
            "steps": [{"what": w, "commands": c} for w, c in steps], "findings": notes,
            "ran": False}


def render_plan(p):
    lines = ["Plan (nothing was run; these are the commands to run yourself, from the repository):",
             "  overlay: %s (%d files)" % (p["overlay"], p["files"]),
             "  pristine root: %s%s" % (p["root"], "" if p["rootPresent"] else " (not present here)"),
             "  derived root: %s" % p["derived"], ""]
    n = 0
    for s in p["steps"]:
        n += 1
        lines.append("%d. %s" % (n, s["what"]))
        lines += ["     %s" % c for c in s["commands"]]
    if p["findings"]:
        lines += ["", "Findings:"]
        lines += ["  %-7s %s: %s" % (f["level"].upper(), f["code"], f["message"]) for f in p["findings"]]
    return "\n".join(lines)


# ------------------------------------------------------------------ GSF ID

def gsf_text(root):
    env = "ANDROID_X86_ROOT=%s " % q(root)
    return "\n".join([
        "The Google Services Framework (GSF) Android ID",
        "",
        "Google blocks its apps on uncertified Android builds unless the owner registers the device's GSF",
        "Android ID on Google's page for uncertified devices, signed in with their own Google account:",
        "",
        "    %s" % UNCERTIFIED_URL,
        "",
        "You do this yourself, in your own browser. SteamARM never signs in, never fills in the form, never",
        "types or stores a password and never sends the ID anywhere. Registration does not make the root",
        "Play Protect certified and does not change Play Integrity verdicts (docs/PLAY_STORE_SETUP.md).",
        "",
        "The ID exists only after Google Play services and GSF have started and checked in with Google",
        "(networking, a booted framework). It lives in the root's /data: it changes if /data is wiped, and",
        "must then be registered again.",
        "",
        "Read it inside the Android session (x86_64 root, from the repository, while it runs):",
        "",
        "    %sscripts/run-android-x86.sh /system/bin/sqlite3 \\" % env,
        "        %s \\" % GSF_DB,
        '        "%s"' % GSF_QUERY,
        "",
        "or from an Android shell (the command Waydroid documents):",
        "",
        "    sqlite3 /data/data/*/*/gservices.db 'select value from main where name = \"android_id\";'",
        "",
        "or on the Mac, from a copy of the database (no guest, no network):",
        "",
        "    scripts/android-gapps.py gsf-id --root %s --read" % q(root),
        "",
        "Paste the number into the page, submit, wait a few minutes, then restart the Android session.",
    ])


def read_gsf_id(root):
    """The android_id value of gservices.db in the root's /data, read from a
    private copy (with its -wal/-shm files) so the live database is never
    opened or locked."""
    db = os.path.join(os.path.realpath(os.path.expanduser(root)), *GSF_DB.strip("/").split("/"))
    if not os.path.isfile(db):
        raise GappsError(EXIT_MISSING, "no-gservices",
                         "%s does not exist: Google Play services and GSF have not checked in yet (they need a "
                         "booted framework and networking)" % db)
    with tempfile.TemporaryDirectory(prefix="steamarm-gsf-") as tmp:
        copy = os.path.join(tmp, "gservices.db")
        for suffix in ("", "-wal", "-shm", "-journal"):
            if os.path.isfile(db + suffix):
                shutil.copyfile(db + suffix, copy + suffix)
        try:
            con = sqlite3.connect(copy)
            try:
                row = con.execute("select value from main where name = 'android_id'").fetchone()
            finally:
                con.close()
        except sqlite3.Error as e:
            raise GappsError(EXIT_INPUT, "gservices-unreadable", "%s: %s" % (db, e))
    if not row or row[0] is None or not re.fullmatch(r"-?\d+", str(row[0]).strip()):
        raise GappsError(EXIT_MISSING, "no-android-id",
                         "%s has no android_id yet: GSF has not checked in" % db)
    return str(row[0]).strip()


# ------------------------------------------------------------------ commands

def cmd_inspect(args):
    target = resolve_target(args.arch, args.root)
    with GappsPackage(args.zip, density=args.density) as pkg:
        pkg.inspect_apks()
        excluded = pkg.exclude(args.exclude)
        findings = check(pkg, target)
        rep = report(pkg, target, findings, excluded)
    emit(rep, args.json, lambda r: render_inspect(r, args.verbose))
    return EXIT_OK if rep["ok"] else EXIT_REFUSED


def cmd_overlay(args):
    target = resolve_target(args.arch, args.root)
    refuse_destination(args.out, target)
    with GappsPackage(args.zip, density=args.density) as pkg:
        pkg.inspect_apks()
        excluded = pkg.exclude(args.exclude)
        findings = check(pkg, target)
        if pkg.unsafe:
            raise GappsError(EXIT_INPUT, "unsafe-path",
                             "the package has entries whose paths could escape the overlay (%s); nothing was written"
                             % ", ".join(pkg.unsafe[:5]), findings=findings)
        errors = [f for f in findings if f["level"] == "error"]
        if errors and not args.allow_mismatch:
            raise GappsError(EXIT_REFUSED, "mismatch",
                             "the package does not suit the root (%s); nothing was written (--allow-mismatch "
                             "builds it anyway)" % "; ".join(f["message"] for f in errors[:3]), findings=findings)
        manifest = write_overlay(pkg, args.out, target, findings, excluded)
    out = {"ok": True, "overlay": os.path.realpath(args.out), "files": len(manifest["files"]),
           "bytes": sum(f["size"] for f in manifest["files"]), "excluded": manifest["excluded"],
           "findings": findings}
    emit(out, args.json, lambda r: "\n".join(
        ["Overlay written: %s (%d files, %s bytes)" % (r["overlay"], r["files"], format(r["bytes"], ",")),
         "The Android root was not touched. Next: scripts/android-gapps.py apply-plan %s" % q(r["overlay"])] +
        ["  %-7s %s: %s" % (f["level"].upper(), f["code"], f["message"]) for f in r["findings"]
         if f["level"] != "note" or args.verbose]))
    return EXIT_OK


def cmd_apply_plan(args):
    p = plan(args.overlay, args.root, args.derived)
    emit(p, args.json, render_plan)
    return EXIT_OK


def cmd_gsf_id(args):
    root = os.path.realpath(os.path.expanduser(args.root or default_root()))
    print(gsf_text(root))
    if args.read:
        value = read_gsf_id(root)
        print("")
        print("GSF Android ID (from %s): %s" % (GSF_DB, value))
    return EXIT_OK


def emit(obj, as_json, render):
    if as_json:
        print(json.dumps(obj, indent=2, ensure_ascii=False))
    else:
        print(render(obj))


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Check and layer a GApps package the owner supplies (no download, no sign-in).",
        epilog="docs/PLAY_STORE_SETUP.md")
    sub = ap.add_subparsers(dest="cmd", required=True)

    def target_args(p):
        p.add_argument("--arch", help="the root's architecture: arm64 or x86_64")
        p.add_argument("--root", help="the Android root to check against (its build.prop)")

    p = sub.add_parser("inspect", help="what a GApps zip would install, and whether it suits the root")
    target_args(p)
    p.add_argument("--density", help="OpenGApps: the density to pick (default nodpi, else the highest)")
    p.add_argument("--exclude", action="append", help="leave out an app (directory, package or archive name)")
    p.add_argument("--json", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("zip")
    p = sub.add_parser("overlay", help="build the overlay tree; the Android root is not touched")
    target_args(p)
    p.add_argument("--density")
    p.add_argument("--exclude", action="append")
    p.add_argument("--allow-mismatch", action="store_true",
                   help="build it even when the package does not suit the root")
    p.add_argument("--json", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    p.add_argument("zip")
    p.add_argument("out")
    p = sub.add_parser("apply-plan", help="print (never run) the commands that layer an overlay on a clone")
    p.add_argument("--root", help="the pristine root (default: the overlay's target, else the default root)")
    p.add_argument("--derived", help="the derived root to create (default: gapps-<arch> next to the root)")
    p.add_argument("--json", action="store_true")
    p.add_argument("overlay")
    p = sub.add_parser("gsf-id", help="how to read the GSF Android ID for Google's uncertified-device page")
    p.add_argument("--root", help="the (derived) Android root")
    p.add_argument("--read", action="store_true", help="read it from a copy of the root's gservices.db")
    args = ap.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    handler = {"inspect": cmd_inspect, "overlay": cmd_overlay, "apply-plan": cmd_apply_plan,
               "gsf-id": cmd_gsf_id}[args.cmd]
    try:
        return handler(args)
    except GappsError as e:
        err = {"ok": False, "code": e.code, "error": e.message}
        err.update(e.extra)
        if getattr(args, "json", False):
            print(json.dumps(err, indent=2, ensure_ascii=False))
        else:
            print("android-gapps: %s" % e.message, file=sys.stderr)
            for f in e.extra.get("findings") or []:
                if f["level"] == "error":
                    print("  ERROR   %s: %s" % (f["code"], f["message"]), file=sys.stderr)
        return e.status
    except OSError as e:
        print("android-gapps: %s" % e, file=sys.stderr)
        return EXIT_IO


if __name__ == "__main__":
    sys.exit(main())
