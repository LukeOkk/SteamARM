#!/usr/bin/env python3
"""SteamARM's Android package manager (docs/APK_SUPPORT.md). It installs
APKs and selected split APKs into the state directory, keeps app data across
updates, lists and uninstalls them. It runs nothing; android-session.py
installs the staged APKs into the zero-VM Android session at launch.

  scripts/android-pm.py install [--force] [--allow-downgrade] app.apk
  scripts/android-pm.py uninstall [--keep-data] <package>
  scripts/android-pm.py list
  scripts/android-pm.py info <package>

Layout, under $STEAMARM_STATE (default ~/SteamARM-roots) or --state:
  android/packages/<package>/base.apk    a copy of the APK
  android/packages/<package>/meta.json   what scripts/apk-inspect.py read, and when
  android/packages/<package>/icon.png    the launcher icon (icon.webp when it
                                          could not be converted)
  android/data/<package>/                the app's data: kept by an update, and
                                          by an uninstall with --keep-data
  android/kept/<package>.json            the signer of data kept that way

Output is JSON on stdout. Exit status: 0 done; 2 bad input (not an APK, bad
package name); 3 not supported (an invalid bundle, AAB, a lone split APK,
an APK that needs missing splits, an unsigned APK); 4 refused (another signer, a downgrade: --force
and --allow-downgrade override); 5 not installed; 6 I/O error.

The signer check compares the certificates the APK names (its v3, else v2,
else v1 signer). Signatures are not verified cryptographically.
"""
import argparse
import datetime
import fcntl
import hashlib
import importlib.util
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
_spec = importlib.util.spec_from_file_location("apk_inspect", HERE / "apk-inspect.py")
apk_inspect = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(apk_inspect)

# Android's rule for a package name (PackageParser.validateName): at least two
# segments, each a letter followed by letters, digits or underscores. The name
# becomes a directory name here, so nothing else is accepted.
PACKAGE_RE = re.compile(r"[A-Za-z][A-Za-z0-9_]*(\.[A-Za-z][A-Za-z0-9_]*)+")   # fullmatch only
MAX_PACKAGE = 255

EXIT_OK, EXIT_INPUT, EXIT_UNSUPPORTED, EXIT_REFUSED, EXIT_MISSING, EXIT_IO = 0, 2, 3, 4, 5, 6


class PMError(Exception):
    def __init__(self, status, code, message, **extra):
        super().__init__(message)
        self.status, self.code, self.message, self.extra = status, code, message, extra


def state_dir(arg=None):
    if arg:
        return Path(arg).expanduser()
    env = os.environ.get("STEAMARM_STATE")
    return Path(env).expanduser() if env else Path.home() / "SteamARM-roots"


class Layout:
    def __init__(self, state):
        self.root = Path(state) / "android"
        self.packages = self.root / "packages"
        self.data = self.root / "data"
        self.kept = self.root / "kept"

    def package_dir(self, package):
        return self.packages / package

    def data_dir(self, package):
        return self.data / package

    def kept_record(self, package):
        return self.kept / (package + ".json")

    def meta(self, package):
        path = self.package_dir(package) / "meta.json"
        try:
            return json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return None


def check_package(package):
    if not isinstance(package, str) or len(package) > MAX_PACKAGE or not PACKAGE_RE.fullmatch(package):
        raise PMError(EXIT_INPUT, "bad-package", "invalid Android package name %r" % (package,))
    return package


def now():
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def same_signer(old, new):
    """(accepted, why). Android accepts an update signed by the same
    certificate, or by a new key whose v3 proof-of-rotation lineage holds the
    old certificate. An APK with a v3.1 block has two identities (its v3
    signer, and the rotated key Android 13 and later use); either one counts."""
    old, new = old or {}, new or {}
    a = set(old.get("certificates") or [])
    b = set(new.get("certificates") or [])
    if not a or not b:
        return False, "the signing certificate of the installed app or of the APK is unknown"
    if a == b:
        return True, "same signing certificate"
    a_all = a | set(old.get("rotatedCertificates") or [])
    b_all = b | set(new.get("rotatedCertificates") or [])
    if a_all & b_all:
        return True, "same signer (a v3.1 rotated key on one side)"
    if a_all & set(new.get("lineage") or []):
        return True, "key rotation: the new APK's v3 lineage includes the installed certificate"
    return False, "the APK is signed with a different certificate (%s) than the installed app (%s)" % (
        ", ".join(sorted(c[:16] for c in b)), ", ".join(sorted(c[:16] for c in a)))


def lock(layout):
    """One command at a time, and what an interrupted one left put right."""
    layout.root.mkdir(parents=True, exist_ok=True)
    fh = open(layout.root / ".android-pm.lock", "w")
    fcntl.flock(fh, fcntl.LOCK_EX)
    recover(layout)
    return fh


def recover(layout):
    """An install killed between its two renames leaves the installed
    version in packages/.old-*/pkg and no packages/<package>: it goes back.
    Stages of installs that never finished are removed."""
    if not layout.packages.is_dir():
        return
    for d in sorted(layout.packages.iterdir()):
        if d.name.startswith(".old-") and d.is_dir():
            old = d / "pkg"
            try:
                meta = json.loads((old / "meta.json").read_text(encoding="utf-8"))
                package = meta.get("package")
            except (OSError, ValueError, AttributeError):
                package = None
            if isinstance(package, str) and PACKAGE_RE.fullmatch(package) \
                    and not layout.package_dir(package).exists():
                os.rename(str(old), str(layout.package_dir(package)))
            shutil.rmtree(str(d), ignore_errors=True)
        elif d.name.startswith(".install-") and d.is_dir():
            shutil.rmtree(str(d), ignore_errors=True)


def write_icon(apk_path, report, dest_dir):
    """The launcher icon as icon.png (a WebP converted with sips when it can
    be), else icon.webp; None when the APK has no raster icon."""
    icon = report.get("icon") or {}
    if not icon.get("path"):
        return None
    with apk_inspect.APK(str(apk_path)) as apk:
        data = apk.icon_bytes(icon["path"])
    fmt = icon.get("format")
    png = dest_dir / "icon.png"
    if fmt == "png":
        png.write_bytes(data)
        return png
    raw = dest_dir / ("icon." + (fmt or "bin"))
    raw.write_bytes(data)
    sips = find_sips()
    if fmt in ("webp", "jpeg") and sips:
        r = subprocess.run([sips, "-s", "format", "png", str(raw), "--out", str(png)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode == 0 and png.exists() and png.stat().st_size > 0:
            raw.unlink()
            return png
        if png.exists():
            png.unlink()
    return raw


def find_sips():
    """sips, when this Mac has it. The tests replace this function to exercise
    the fallback; replacing os.path.exists instead also fooled pathlib, which
    calls it on Python 3.13 (the macOS CI runner)."""
    return shutil.which("sips") or ("/usr/bin/sips" if os.path.exists("/usr/bin/sips") else None)


def summary(meta, layout):
    pkg = meta.get("package")
    icon = meta.get("icon")
    return {
        "package": pkg,
        "label": meta.get("label"),
        "versionName": meta.get("versionName"),
        "versionCode": meta.get("versionCode"),
        "minSdk": meta.get("minSdk"),
        "targetSdk": meta.get("targetSdk"),
        "abis": meta.get("abis"),
        "abiVerdict": (meta.get("abiVerdict") or {}).get("id"),
        "launcherActivity": meta.get("launcherActivity"),
        "packageDir": str(layout.package_dir(pkg)),
        "dataDir": str(layout.data_dir(pkg)),
        "icon": str(layout.package_dir(pkg) / icon) if icon else None,
        "installedAt": meta.get("installedAt"),
        "updatedAt": meta.get("updatedAt"),
        "sha256": meta.get("sha256"),
        "permissions": [p.get("name") for p in meta.get("permissions") or [] if isinstance(p, dict)],
    }


# ------------------------------------------------------------------ commands

def install(layout, apk_path, force=False, allow_downgrade=False):
    apk_path = Path(apk_path)
    try:
        report = apk_inspect.inspect(str(apk_path))
    except apk_inspect.UnsupportedBundle as e:
        raise PMError(EXIT_UNSUPPORTED, "bundle", "%s: %s" % (e.format.upper(), e.reason), format=e.format)
    except apk_inspect.APKError as e:
        raise PMError(EXIT_INPUT, "not-apk", str(e))
    package = check_package(report.get("package"))
    splits = report.get("splits") or {}
    bundle = report.get("bundle") or {}
    if splits.get("isSplit") and not bundle:
        raise PMError(EXIT_UNSUPPORTED, "split-apk",
                      "INSTALL_FAILED_MISSING_SPLIT: %s is a split APK (%s), not a base APK"
                      % (package, splits.get("split")))
    if splits.get("needsSplits") and not bundle:
        # Android refuses such a base APK alone (INSTALL_FAILED_MISSING_SPLIT).
        raise PMError(EXIT_UNSUPPORTED, "needs-splits",
                      "INSTALL_FAILED_MISSING_SPLIT: %s needs split APKs absent from this file" % package)
    signing = report.get("signing") or {}
    if not signing.get("certificates"):
        # Android refuses an APK without a signature (INSTALL_PARSE_FAILED_NO_CERTIFICATES).
        raise PMError(EXIT_UNSUPPORTED, "unsigned",
                      "%s has no v1, v2 or v3 signature whose certificate could be read" % package)

    with lock(layout):
        old = layout.meta(package)
        ddir = layout.data_dir(package)
        kept = None
        # Data kept by uninstall --keep-data belongs to its signer, as long
        # as that data is still there.
        if ddir.is_dir():
            try:
                kept = json.loads(layout.kept_record(package).read_text(encoding="utf-8"))
            except (OSError, ValueError):
                pass
        previous = old or kept
        if previous is None and (layout.package_dir(package).exists()
                                 or (ddir.is_dir() and any(ddir.iterdir()))):
            # An installed app whose meta.json is gone or unreadable, or data
            # nobody accounts for: its signer is unknown, so it is not handed
            # to whichever APK comes next.
            previous = {"signing": None}
        if previous is not None:
            ok, why = same_signer(previous.get("signing"), signing)
            if not ok and not force:
                raise PMError(EXIT_REFUSED, "different-signer", why, package=package)
            code, old_code = report.get("versionCode"), previous.get("versionCode")
            if (old is not None and isinstance(code, int) and isinstance(old_code, int)
                    and code < old_code and not allow_downgrade):
                raise PMError(EXIT_REFUSED, "downgrade",
                              "%s %s (%d) is older than the installed %s (%d)"
                              % (package, report.get("versionName"), code, old.get("versionName"), old_code),
                              package=package)
        action = "installed" if old is None else (
            "reinstalled" if old.get("versionCode") == report.get("versionCode") else "updated")

        try:
            # Before anything is replaced: a data directory that cannot be
            # made fails the install while the old version is still in place.
            ddir.mkdir(parents=True, exist_ok=True)
            layout.packages.mkdir(parents=True, exist_ok=True)
        except OSError as e:
            raise PMError(EXIT_IO, "io", "install failed: %s" % e)
        stage = Path(tempfile.mkdtemp(prefix=".install-%s-" % package, dir=str(layout.packages)))
        try:
            base = stage / "base.apk"
            split_files, obb_files = [], []
            if bundle:
                # The outer digest protects the selection from a source file
                # replaced between inspection and staging.
                if sha256(apk_path) != report["sha256"]:
                    raise PMError(EXIT_IO, "io", "the bundle changed after inspection")
                by_path = {m["path"]: m for m in bundle["members"]}
                with zipfile.ZipFile(apk_path) as archive:
                    for member in bundle["chosen"]:
                        if member == bundle["base"]:
                            target = base
                        else:
                            split = by_path[member]["split"]
                            if not isinstance(split, str) or not re.fullmatch(r"[A-Za-z0-9_.-]+", split):
                                raise PMError(EXIT_INPUT, "not-apk", "invalid split name: %r" % split)
                            target = stage / ("split_%s.apk" % split)
                            if target.name in split_files:
                                raise PMError(EXIT_INPUT, "not-apk", "duplicate split name: %s" % split)
                            split_files.append(target.name)
                        with archive.open(member) as source, target.open("wb") as dest:
                            shutil.copyfileobj(source, dest)
                    if not base.exists():
                        raise PMError(EXIT_INPUT, "not-apk", "no base APK selected")
                    for member in bundle["obb"]:
                        name = Path(member).name
                        target = stage / "obb" / name
                        target.parent.mkdir(exist_ok=True)
                        if target.exists():
                            raise PMError(EXIT_INPUT, "not-apk", "duplicate OBB name: %s" % name)
                        with archive.open(member) as source, target.open("wb") as dest:
                            shutil.copyfileobj(source, dest)
                        obb_files.append("obb/" + name)
            else:
                shutil.copyfile(str(apk_path), str(base))
                if sha256(base) != report["sha256"]:
                    raise PMError(EXIT_IO, "io", "the copy of %s does not match the APK that was read" % apk_path)
            icon = write_icon(base, report, stage)
            meta = dict(report)
            meta.pop("fdroid", None)
            meta.update({
                "schema": 1,
                "splits": split_files,
                "obb": obb_files,
                "icon": icon.name if icon else None,
                "installedAt": (old or {}).get("installedAt") or now(),
                "updatedAt": now(),
                "installedWith": {"force": bool(force), "allowDowngrade": bool(allow_downgrade)},
            })
            if previous is not None and force:
                meta["installedWith"]["replacedSigner"] = (previous.get("signing") or {}).get("certificates")
            (stage / "meta.json").write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n",
                                             encoding="utf-8")
            os.chmod(str(stage), 0o755)
            final = layout.package_dir(package)
            retired = None
            if final.exists():
                retired = Path(tempfile.mkdtemp(prefix=".old-%s-" % package, dir=str(layout.packages)))
                os.rename(str(final), str(retired / "pkg"))
            try:
                os.rename(str(stage), str(final))
            except OSError:
                # Put the installed version back: a failed update leaves it as it was.
                if retired is not None:
                    os.rename(str(retired / "pkg"), str(final))
                    shutil.rmtree(str(retired), ignore_errors=True)
                raise
            stage = None
            if retired is not None:
                shutil.rmtree(str(retired), ignore_errors=True)
        except OSError as e:
            raise PMError(EXIT_IO, "io", "install failed: %s" % e)
        finally:
            if stage is not None:
                shutil.rmtree(str(stage), ignore_errors=True)
        try:
            layout.kept_record(package).unlink()
        except OSError:
            pass
    out = {"ok": True, "action": action}
    out.update(summary(meta, layout))
    out["abiSummary"] = (meta.get("abiVerdict") or {}).get("summary")
    out["previousVersion"] = old.get("versionName") if old else None
    out["dataKept"] = previous is not None
    return out


def uninstall(layout, package, keep_data=False):
    check_package(package)
    with lock(layout):
        pdir = layout.package_dir(package)
        meta = layout.meta(package)
        if not pdir.exists():
            raise PMError(EXIT_MISSING, "not-installed", "%s is not installed" % package, package=package)
        ddir = layout.data_dir(package)
        if keep_data:
            layout.kept.mkdir(parents=True, exist_ok=True)
            record = {"package": package, "versionCode": (meta or {}).get("versionCode"),
                      "versionName": (meta or {}).get("versionName"),
                      "signing": (meta or {}).get("signing"), "keptAt": now()}
            layout.kept_record(package).write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")
        try:
            if not keep_data and ddir.exists():
                # The data first: if it cannot all go, the app stays
                # installed and the error says so (no orphaned data).
                shutil.rmtree(str(ddir))
            shutil.rmtree(str(pdir))
        except OSError as e:
            raise PMError(EXIT_IO, "io", "uninstall of %s failed: %s" % (package, e), package=package)
        if not keep_data:
            try:
                layout.kept_record(package).unlink()
            except OSError:
                pass
    return {"ok": True, "action": "uninstalled", "package": package, "dataKept": bool(keep_data),
            "dataDir": str(ddir)}


def list_packages(layout):
    out = []
    if layout.packages.is_dir():
        for d in sorted(layout.packages.iterdir()):
            if d.name.startswith(".") or not PACKAGE_RE.fullmatch(d.name):
                continue
            meta = layout.meta(d.name)
            if meta and meta.get("package") == d.name:
                out.append(summary(meta, layout))
    kept = []
    if layout.kept.is_dir():
        kept = sorted(p.stem for p in layout.kept.glob("*.json")
                      if PACKAGE_RE.fullmatch(p.stem) and layout.data_dir(p.stem).is_dir())
    return {"ok": True, "packages": out, "keptData": kept}


def info(layout, package):
    check_package(package)
    meta = layout.meta(package)
    if meta is None:
        raise PMError(EXIT_MISSING, "not-installed", "%s is not installed" % package, package=package)
    out = {"ok": True}
    out.update(summary(meta, layout))
    out["meta"] = meta
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description="Install, list and uninstall Android APKs for SteamARM (no runtime yet).")
    ap.add_argument("--state", help="state directory (default $STEAMARM_STATE or ~/SteamARM-roots)")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("install", help="install or update an APK")
    p.add_argument("apk")
    p.add_argument("--force", action="store_true", help="replace an app signed with another certificate")
    p.add_argument("--allow-downgrade", action="store_true", help="install an older versionCode")
    p = sub.add_parser("uninstall", help="remove an app")
    p.add_argument("package")
    p.add_argument("--keep-data", action="store_true", help="keep android/data/<package>")
    sub.add_parser("list", help="installed apps, as JSON")
    p = sub.add_parser("info", help="one app's meta.json")
    p.add_argument("package")
    args = ap.parse_args(argv)
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")      # labels are not ASCII
    layout = Layout(state_dir(args.state))
    try:
        if args.cmd == "install":
            out = install(layout, args.apk, force=args.force, allow_downgrade=args.allow_downgrade)
        elif args.cmd == "uninstall":
            out = uninstall(layout, args.package, keep_data=args.keep_data)
        elif args.cmd == "list":
            out = list_packages(layout)
        else:
            out = info(layout, args.package)
    except PMError as e:
        err = {"ok": False, "code": e.code, "error": e.message}
        err.update(e.extra)
        print(json.dumps(err, ensure_ascii=False))
        return e.status
    except OSError as e:
        print(json.dumps({"ok": False, "code": "io", "error": str(e)}, ensure_ascii=False))
        return EXIT_IO
    print(json.dumps(out, indent=2, ensure_ascii=False))
    return EXIT_OK


if __name__ == "__main__":
    sys.exit(main())
