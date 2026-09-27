#!/bin/bash
# The downloadable package: build/release/SteamARM-<version>.dmg with
# SteamARM.app inside. The app carries the source tree (the files git tracks)
# as Contents/Resources/SteamARM-src.tar.gz and unpacks it on first start
# into ~/Library/Application Support/SteamARM/src, where its "Instalar"
# button runs scripts/setup.sh (docs/INSTALL.md). No path of the machine that
# built it is recorded anywhere in the package.
#
#   scripts/make-release.sh            build the .dmg (and a .zip of the app)
#
# The app is signed ad hoc (no Apple Developer ID): the first open needs
# System Settings > Privacy & Security > "Open Anyway" (docs/INSTALL.md).
set -euo pipefail
cd "$(dirname "$0")/.." || exit 1
VERSION=$(/usr/libexec/PlistBuddy -c "Print :CFBundleShortVersionString" launcher/Info.plist.in)
OUT=build/release
APP=$OUT/SteamARM.app
rm -rf "$OUT"
mkdir -p "$OUT"

make launcher >/dev/null
cp -R build/SteamARM.app "$APP"
# No checkout path in a distributed app: the launcher falls back to the
# unpacked source.
/usr/libexec/PlistBuddy -c "Set :SteamARMProjectDir ''" "$APP/Contents/Info.plist"

# The source: what git tracks plus new files it does not ignore.
git ls-files -co --exclude-standard -z | xargs -0 tar -czf "$APP/Contents/Resources/SteamARM-src.tar.gz" \
    --no-mac-metadata --uid 0 --gid 0 --uname "" --gname ""
# Nothing personal may ship: fail on a home path or a user name in the tree.
if tar -xzOf "$APP/Contents/Resources/SteamARM-src.tar.gz" 2>/dev/null | grep -aq "/Users/$(id -un)"; then
    echo "make-release: the source contains /Users/$(id -un); refusing" >&2
    exit 1
fi
if strings "$APP/Contents/MacOS/SteamARM" | grep -q "/Users/$(id -un)"; then
    echo "make-release: the launcher binary contains /Users/$(id -un); refusing" >&2
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
    -ov "$OUT/SteamARM-$VERSION-macOS-arm64.dmg" >/dev/null
rm -rf "$STAGE"
(cd "$OUT" && shasum -a 256 SteamARM-"$VERSION"-macOS-arm64.* > SHA256SUMS.txt)
ls -lh "$OUT"
