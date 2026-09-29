#!/bin/bash
# Heroic Games Launcher for linux-arm64, into the Fedora ARM64 root
# (scripts/mkarmroot.sh) at opt/apps/heroic: no FEX, no VM.
#
#   scripts/install-heroic-arm64.sh [--dest DIR] [--root DIR] [--downloads DIR]
#
# Heroic publishes Linux builds for x64 only (UPSTREAM DOCUMENTED: the v2.22.3
# release assets; its CI builds an arm64 AppImage but only as a 14-day
# workflow artifact). Heroic is Electron with no native Node module, so its
# arm64 build is assembled here the way electron-builder assembles it, from
# official release files only, each checked against the sha256 below:
#   - Electron 43.1.1 linux-arm64 (the version Heroic 2.22.3's pnpm-lock pins),
#     with `electron` named `heroic`, as electron-builder names it;
#   - resources/ of the official Heroic 2.22.3 linux-x64 tarball, whose
#     app.asar is re-pointed from its x64 helpers to arm64 ones
#     (scripts/heroic-asar.py; only the archive's header changes);
#   - the arm64 helpers Heroic's own build fetches (meta/downloadHelperBinaries.ts
#     at v2.22.3): legendary, gogdl, nile, comet from their releases, and
#     vulkan-helper from Heroic's repository at the tag.
# MEASURED (benchmarks/stage24-heroic.txt): the resulting app.asar is
# byte-identical to the one `electron-builder --linux dir --arm64` makes from
# Heroic's source tag, and the Electron files are the same as in that build.
#
# The root needs GTK 3, libsecret, libnotify and python3 (scripts/mkarmroot.sh
# seeds them; rebuild the root if they are missing). A rebuild of the root
# keeps opt/apps and tmp/, so the install survives it.
#   --dest DIR       install into DIR/Heroic-<version>-linux-arm64
#                    (default <root>/opt/apps/heroic)
#   --root DIR       the ARM64 root (default $STEAMARM_STATE/armroot)
#   --downloads DIR  download cache (default $STEAMARM_BUILD/downloads/heroic-<version>)
# Progress lines start with "[n/6]"; the last line is "installed: <guest program>".
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
REPO=$PWD
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
BUILD="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
VERSION=2.22.3
ELECTRON=43.1.1
ROOT="$STATE/armroot"
DEST=""
DL=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dest) DEST="${2:?}"; shift 2 ;;
        --root) ROOT="${2:?}"; shift 2 ;;
        --downloads) DL="${2:?}"; shift 2 ;;
        -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
done
DEST="${DEST:-$ROOT/opt/apps/heroic}"
DL="${DL:-$BUILD/downloads/heroic-$VERSION}"
NAME="Heroic-$VERSION-linux-arm64"

# file  url  sha256  (licence)
FILES="
Heroic-$VERSION-linux-x64.tar.xz https://github.com/Heroic-Games-Launcher/HeroicGamesLauncher/releases/download/v$VERSION/Heroic-$VERSION-linux-x64.tar.xz 7382d719a5114c5f0761eea05cdb22f73f58fe98ec840d16b3d85a054d0d3efd
electron-v$ELECTRON-linux-arm64.zip https://github.com/electron/electron/releases/download/v$ELECTRON/electron-v$ELECTRON-linux-arm64.zip 30ed0b3672dd71672f6496fa0ba0c770ac7f8c6d9b38f01f615a6923b1541311
legendary https://github.com/legendary-gl/legendary/releases/download/0.21.1/legendary_linux_arm64 e901cb52ada10d5cef6ffb2a0985099958124457bfb455a571023d1e042143d9
gogdl https://github.com/Heroic-Games-Launcher/heroic-gogdl/releases/download/v1.3.0/gogdl_linux_arm64 c49e1519146523ec94f33e2d21eedcc9a167004d7da218621e6d5fb84a7a0f4c
nile https://github.com/imLinguin/nile/releases/download/v1.2.0/nile_linux_arm64 197859c629c47e4e4d7ed1d2a290b7bef1e778f891ff8ca42ba65dbb020ba751
comet https://github.com/imLinguin/comet/releases/download/v0.2.0/comet-aarch64-unknown-linux-gnu 7eca0d84d3c0b0e563a732a6300a20c115b5d60d25e44735d18e86a773b97a2f
vulkan-helper https://raw.githubusercontent.com/Heroic-Games-Launcher/HeroicGamesLauncher/v$VERSION/public/bin/arm64/linux/vulkan-helper 19cc3e7a27df3c75adb37b7474dca44991e142f5a9fe339dc94d3d90598aeea0
"
HELPERS="legendary gogdl nile comet vulkan-helper"

die() { echo "install-heroic-arm64: $*" >&2; exit 1; }
sha() { shasum -a 256 "$1" | cut -d' ' -f1; }

# The root must be able to run it: an Electron binary needs libgtk-3 (and
# Chromium dlopens libsecret and libnotify); legendary and gogdl are python3
# zipapps.
[ -d "$ROOT" ] || die "no ARM64 root at $ROOT (scripts/mkarmroot.sh)"
missing=""
for f in usr/lib64/libgtk-3.so.0 usr/lib64/libsecret-1.so.0 usr/lib64/libnotify.so.4 usr/bin/python3; do
    [ -e "$ROOT/$f" ] || missing="$missing /$f"
done
[ -z "$missing" ] || die "the ARM64 root at $ROOT lacks$missing: rebuild it with scripts/mkarmroot.sh (it keeps opt/apps and tmp/)"

echo "[1/6] downloading to $DL"
mkdir -p "$DL"
echo "$FILES" | while read -r f url want; do
    [ -n "$f" ] || continue
    if [ -f "$DL/$f" ] && [ "$(sha "$DL/$f")" = "$want" ]; then
        echo "      $f (cached)"
        continue
    fi
    echo "      $f <- $url"
    curl -fL --retry 3 --silent --show-error -o "$DL/$f.part" "$url" || die "download failed: $url"
    got=$(sha "$DL/$f.part")
    [ "$got" = "$want" ] || { rm -f "$DL/$f.part"; die "$f: sha256 $got, expected $want"; }
    mv "$DL/$f.part" "$DL/$f"
done

WORK=$(mktemp -d "${TMPDIR:-/tmp}/heroic-arm64.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
trap 'exit 130' INT TERM          # the launcher's Cancel: clean up, then stop
echo "[2/6] unpacking the Electron $ELECTRON linux-arm64 runtime"
OUT="$WORK/$NAME"
mkdir -p "$OUT"
unzip -q "$DL/electron-v$ELECTRON-linux-arm64.zip" -d "$OUT"
# What electron-builder does to the runtime: its launcher takes the product's
# executable name, the licence file is renamed, and the default app and the
# version file are not shipped.
mv "$OUT/electron" "$OUT/heroic"
mv "$OUT/LICENSE" "$OUT/LICENSE.electron.txt"
rm -f "$OUT/version" "$OUT/resources/default_app.asar"

echo "[3/6] taking resources/ from the linux-x64 release"
tar -xJf "$DL/Heroic-$VERSION-linux-x64.tar.xz" -C "$WORK" "Heroic-$VERSION-linux-x64/resources"
X64="$WORK/Heroic-$VERSION-linux-x64/resources"
cp -R "$X64/app.asar.unpacked" "$X64/app-update.yml" "$OUT/resources/"
rm -rf "$OUT/resources/app.asar.unpacked/build/bin/x64/linux"
BIN="$OUT/resources/app.asar.unpacked/build/bin/arm64/linux"
mkdir -p "$BIN"
for h in $HELPERS; do
    install -m 0755 "$DL/$h" "$BIN/$h"
done

echo "[4/6] re-pointing app.asar at the arm64 helpers"
/usr/bin/python3 scripts/heroic-asar.py retarget "$X64/app.asar" "$OUT/resources/app.asar" "$BIN"

echo "[5/6] checking the result"
file "$OUT/heroic" | grep -q 'ARM aarch64' || die "$OUT/heroic is not an aarch64 ELF"
listed=$(/usr/bin/python3 scripts/heroic-asar.py list "$OUT/resources/app.asar" build/bin/arm64/linux/ | awk '{print $1}' | sed 's|.*/||' | sort | tr '\n' ' ')
[ "$listed" = "$(echo $HELPERS | tr ' ' '\n' | sort | tr '\n' ' ')" ] || die "app.asar lists [$listed] under build/bin/arm64/linux"
if /usr/bin/python3 scripts/heroic-asar.py list "$OUT/resources/app.asar" build/bin/x64/linux/ | grep -q .; then
    die "app.asar still lists x64 helpers"
fi

echo "[6/6] installing into $DEST/$NAME"
mkdir -p "$DEST"
rm -rf "${DEST:?}/$NAME.new"
mv "$OUT" "$DEST/$NAME.new"
rm -rf "${DEST:?}/$NAME.old"
[ ! -e "$DEST/$NAME" ] || mv "$DEST/$NAME" "$DEST/$NAME.old"
mv "$DEST/$NAME.new" "$DEST/$NAME"
rm -rf "${DEST:?}/$NAME.old"
# Heroic's home in the guest (HOME_IN_GUEST of its launcher entry): the root's
# tmp/, which a rebuild of the root keeps.
mkdir -p "$ROOT/tmp/heroichome"
echo "installed: /opt/apps/heroic/$NAME/heroic"
