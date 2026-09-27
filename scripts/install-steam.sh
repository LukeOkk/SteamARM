#!/bin/bash
# Put the Linux Steam client's bootstrap into the Steam root, no VM.
#
#   scripts/install-steam.sh            (idempotent: skips an existing client)
#
# Valve's steam_latest.deb carries bootstraplinux_ubuntu12_32.tar.xz, the
# minimal client that steam.sh then updates to the current one on its first
# run. It is unpacked into the guest home's ~/.local/share/Steam
# ($ROOT/tmp/fexhome/.local/share/Steam); nothing else of the package (the
# /usr/bin/steam wrapper, udev rules) is needed here.
set -euo pipefail
ROOT="${LXRT_ROOT:-/tmp/lxrt-steamroot}"
BUILD="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
URL="${STEAM_DEB_URL:-https://repo.steampowered.com/steam/archive/stable/steam_latest.deb}"
DEST="$ROOT/tmp/fexhome/.local/share/Steam"
log() { printf '[install-steam] %s\n' "$*"; }
die() { log "error: $*"; exit 1; }

[ -d "$ROOT/usr" ] || die "no Steam root at $ROOT (scripts/setup.sh builds it)"
if [ -x "$DEST/steam.sh" ]; then
    log "Steam client already in $DEST"
    exit 0
fi

mkdir -p "$BUILD/debs"
deb="$BUILD/debs/steam_latest.deb"
log "fetch $URL"
curl -fsSL --retry 3 -o "$deb.part" "$URL" || die "download failed"
mv "$deb.part" "$deb"

tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
( cd "$tmp" && tar -xf "$deb" )                          # ar archive: bsdtar reads it
data="$(ls "$tmp"/data.tar.* 2>/dev/null | head -1)"
[ -n "$data" ] || die "no data.tar in $deb"
tar -xf "$data" -C "$tmp" ./usr/lib/steam/bootstraplinux_ubuntu12_32.tar.xz 2>/dev/null ||
    tar -xf "$data" -C "$tmp" usr/lib/steam/bootstraplinux_ubuntu12_32.tar.xz
boot="$tmp/usr/lib/steam/bootstraplinux_ubuntu12_32.tar.xz"
[ -f "$boot" ] || die "bootstrap tarball not found in the package"

mkdir -p "$DEST"
tar -xf "$boot" -C "$DEST"
[ -x "$DEST/steam.sh" ] || die "bootstrap did not provide steam.sh"
log "Steam bootstrap unpacked into $DEST; it updates itself on first start (scripts/run-steam.sh)"
