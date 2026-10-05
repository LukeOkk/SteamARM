#!/bin/bash
# The downloadable package: build/release/SteamARM-<version>.dmg with
# SteamARM.app inside. The app carries the source tree (the files git tracks)
# as Contents/Resources/SteamARM-src.tar.gz and unpacks it on first start
# into ~/Library/Application Support/SteamARM/src, where its "Instalar"
# button runs scripts/setup.sh (docs/INSTALL.md). No path of the machine that
# built it is recorded anywhere in the package.
#
#   scripts/make-release.sh            build the .dmg (and a .zip of the app)
#   STEAMARM_RELEASE_OUT=/new/path scripts/make-release.sh
#                                    retain packages and staging in a new directory
#
# The app is signed ad hoc (no Apple Developer ID): the first open needs
# System Settings > Privacy & Security > "Open Anyway" (docs/INSTALL.md).
set -euo pipefail
if [ "${1:-}" = --help ]; then
    printf '%s\n' 'Usage: scripts/make-release.sh' \
        'STEAMARM_RELEASE_OUT selects a new output directory (default: build/release).' \
        'Existing output directories are refused; packages and staging are preserved.' \
        'STEAMARM_RELEASE_TAG (or TAG) must match the app version when supplied.'
    exit 0
fi
[ $# -eq 0 ] || { echo 'make-release: unexpected argument (use --help)' >&2; exit 2; }
cd "$(dirname "$0")/.." || exit 1
VERSION=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" launcher/Info.plist.in)
OUT="${STEAMARM_RELEASE_OUT:-build/release}"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
APP=$OUT/SteamARM.app
[ ! -e "$OUT" ] && [ ! -L "$OUT" ] ||
    { echo 'make-release: output already exists; select a new STEAMARM_RELEASE_OUT' >&2; exit 1; }
RELEASE_TAG="${STEAMARM_RELEASE_TAG:-${TAG:-}}"
[ -z "$RELEASE_TAG" ] || [ "$RELEASE_TAG" = "v$VERSION" ] ||
    { echo "make-release: app version $VERSION does not match tag $RELEASE_TAG" >&2; exit 1; }
# The launcher and source archive must describe the same committed revision.
git diff --quiet HEAD -- ||
    { echo 'make-release: commit tracked changes before packaging' >&2; exit 1; }
# make launcher also reads wildcard Swift sources and bundled resources.
[ -z "$(git ls-files --others --exclude-standard -- launcher resources)" ] ||
    { echo 'make-release: untracked launcher inputs would differ from the source archive' >&2; exit 1; }
mkdir -p "$(dirname "$OUT")"
mkdir "$OUT"

# The package carries the source, and the user's Mac builds lxrun from it
# (scripts/setup.sh: make all). Its Makefile must link lxrun as SDK 12.3 by
# default, so the kernel keeps x18 for JIT code (benchmarks/stage28-keep-x18.txt).
grep -q '^LXRT_KEEP_X18 ?= 1$' Makefile ||
    { echo "make-release: the Makefile no longer defaults to LXRT_KEEP_X18=1; refusing" >&2; exit 1; }

make launcher >/dev/null
cp -R build/SteamARM.app "$APP"
# No checkout path in a distributed app: the launcher falls back to the
# unpacked source.
/usr/libexec/PlistBuddy -c "Set :SteamARMProjectDir ''" "$APP/Contents/Info.plist"

# The source is exactly the committed tree. Never package local, untracked files.
git archive --format=tar HEAD | gzip -n > "$APP/Contents/Resources/SteamARM-src.tar.gz"
# Nothing personal may ship: fail on a home path or a user name in the tree.
python3 -m unittest discover -s tests -p test_no_personal_data.py
PRIVATE_HOME="/Users/$(id -un)"
# GitHub's standard runner path is a public CI/example path, explicitly
# allowed by the source privacy test. It is not this user's home directory.
if [ "${GITHUB_ACTIONS:-}" = true ] && [ "$PRIVATE_HOME" = /Users/runner ]; then
    PRIVATE_HOME=""
fi
if [ -n "$PRIVATE_HOME" ] && tar -xzOf "$APP/Contents/Resources/SteamARM-src.tar.gz" 2>/dev/null | grep -aF "$PRIVATE_HOME" >/dev/null; then
    echo "make-release: the source contains a private build home; refusing" >&2
    exit 1
fi
if [ -n "$PRIVATE_HOME" ] && strings "$APP/Contents/MacOS/SteamARM" | grep -F "$PRIVATE_HOME" >/dev/null; then
    echo "make-release: the launcher binary contains a private build home; refusing" >&2
    exit 1
fi

xattr -cr "$APP"
codesign --force --deep --sign - "$APP"
codesign --verify --deep "$APP"

ditto -c -k --keepParent "$APP" "$OUT/SteamARM-$VERSION-macOS-arm64.zip"
STAGE=$OUT/dmg
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
cp docs/INSTALL.md "$STAGE/INSTALL.md"
hdiutil create -volname "SteamARM $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO \
    "$OUT/SteamARM-$VERSION-macOS-arm64.dmg" >/dev/null
(cd "$OUT" && shasum -a 256 SteamARM-"$VERSION"-macOS-arm64.* > SHA256SUMS.txt)
ls -lh "$OUT"
