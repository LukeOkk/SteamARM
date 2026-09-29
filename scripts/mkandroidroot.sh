#!/bin/bash
# An Android 11 (LineageOS 18.1) arm64 root for lxrun, from Waydroid's public
# images, with no VM and no mount of a Linux filesystem (docs/ANDROID_RUNTIME_
# ARCHITECTURE.md, benchmarks/stage25-android-userspace.txt).
#
#   scripts/mkandroidroot.sh            download (if needed), verify, extract
#   scripts/mkandroidroot.sh --rebuild  make the root again from the images,
#                                       keeping its /data (dalvik-cache, ...)
#   scripts/mkandroidroot.sh --verify   check the pristine images' sha256 only
#
# What it makes:
#   $ANDROID_STATE/images/            the two zips as downloaded (read-only),
#                                     Waydroid's OTA index they came from
#   $ANDROID_STATE/images/extracted/  system.img, vendor.img (raw; read-only)
#   $STATE/android.sparsebundle       a case-sensitive APFS volume, attached at
#                                     /Volumes/SteamARMAndroid (Android's tree
#                                     has names that differ only by case)
#   /Volumes/SteamARMAndroid/root     the root: system.img's tree (system-as-
#                                     root), vendor.img's under /vendor, each
#                                     flattened APEX of /system/apex copied
#                                     (APFS clones) to /apex/<manifest name>,
#                                     and empty state directories under /data
# Run a program in it:
#   LXRT_ROOT=/Volumes/SteamARMAndroid/root LXRT_GUEST_PAGE=4096 \
#       build/lxrun /system/bin/toybox uname -a
#
# Licences (recorded in the root's .steamarm-androidroot): the images are
# Waydroid's LineageOS 18.1 builds, Apache-2.0 and GPL-2.0 (the AOSP/LineageOS
# licences, NOTICE files inside the image) -- VANILLA means no Google apps.
# Nothing from them is committed or bundled; they stay on this Mac.
set -euo pipefail
CANON_BASE=$PWD
cd "$(dirname "$0")/.." || exit 1
. scripts/roots.sh

STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
ANDROID_STATE="${ANDROID_STATE:-$STATE/android}"
IMAGES="$ANDROID_STATE/images"
RAW="$IMAGES/extracted"
BUNDLE="${ANDROID_BUNDLE:-$STATE/android.sparsebundle}"
MOUNT="${ANDROID_MOUNT:-/Volumes/SteamARMAndroid}"
ROOT="$MOUNT/root"
DEBUGFS="${DEBUGFS:-/opt/homebrew/opt/e2fsprogs/sbin/debugfs}"
FSCK_EROFS="${FSCK_EROFS:-/opt/homebrew/opt/erofs-utils/bin/fsck.erofs}"
PY=scripts/androidroot.py

# Pinned: Waydroid's OTA index (https://ota.waydro.id/system/lineage/
# waydroid_arm64/VANILLA.json, .../vendor/waydroid_arm64/MAINLINE.json) lists
# each build with its sha256 as "id"; these are the last lineage-18.1
# (Android 11) entries there. Its download URLs are SourceForge's.
SF=https://sourceforge.net/projects/waydroid/files/images
SYSTEM_ZIP=lineage-18.1-20250628-VANILLA-waydroid_arm64-system.zip
SYSTEM_SHA=2700a68255c234f04453da15bfdaed0b0d30343f3af968cf39a096657d88a625
SYSTEM_URL="$SF/system/lineage/waydroid_arm64/$SYSTEM_ZIP/download"
VENDOR_ZIP=lineage-18.1-20250628-MAINLINE-waydroid_arm64-vendor.zip
VENDOR_SHA=e3b81b0c7f86d316350081a5d43622dd5dc2f9e028f10a715422133853ba886c
VENDOR_URL="$SF/vendor/waydroid_arm64/$VENDOR_ZIP/download"

MODE=build
case "${1:-}" in
    --rebuild) MODE=rebuild ;;
    --verify) MODE=verify ;;
    "") ;;
    *) echo "usage: $0 [--rebuild|--verify]" >&2; exit 2 ;;
esac

say() { echo "mkandroidroot: $*"; }
die() { echo "mkandroidroot: $*" >&2; exit 1; }

sha_ok() { [ "$(shasum -a 256 "$1" | awk '{print $1}')" = "$2" ]; }

fetch() {   # fetch NAME SHA URL
    local f="$IMAGES/$1"
    if [ -f "$f" ]; then
        sha_ok "$f" "$2" || die "$f: sha256 is not $2 (delete it to download again)"
        say "$1: present, sha256 ok"
        return 0
    fi
    say "downloading $3"
    curl -L --fail -sS -o "$f.part" "$3"
    sha_ok "$f.part" "$2" || { rm -f "$f.part"; die "$1: sha256 mismatch, not kept"; }
    mv "$f.part" "$f"
    chmod 444 "$f"
    say "$1: downloaded, sha256 ok"
}

unpack_img() {  # unpack_img ZIP NAME.img
    local out="$RAW/$2" kind
    [ -f "$out" ] && { say "$2: already extracted"; return 0; }
    unzip -o -q -d "$RAW" "$IMAGES/$1" "$2"
    kind=$(python3 $PY image-kind "$out")
    if [ "$kind" = sparse ]; then
        python3 $PY unsparse "$out" "$out.raw" && mv "$out.raw" "$out"
        kind=$(python3 $PY image-kind "$out")
    fi
    chmod 444 "$out"
    say "$2: $kind"
}

extract_fs() {  # extract_fs IMG DEST
    local kind
    kind=$(python3 $PY image-kind "$1")
    mkdir -p "$2"
    case "$kind" in
        ext4)
            [ -x "$DEBUGFS" ] || die "debugfs not found ($DEBUGFS): brew install e2fsprogs"
            # rdump cannot chown as a user; those lines are the only noise.
            "$DEBUGFS" -R "rdump / $2" "$1" 2>&1 | grep -v -e 'changing ownership' -e '^debugfs ' || true ;;
        erofs)
            [ -x "$FSCK_EROFS" ] || die "fsck.erofs not found: brew install erofs-utils"
            "$FSCK_EROFS" --extract="$2" "$1" ;;
        *) die "$1: unknown filesystem ($kind)" ;;
    esac
}

mkdir -p "$IMAGES" "$RAW"
curl -sS -m 60 -o "$IMAGES/ota-system-VANILLA.json.new" \
    https://ota.waydro.id/system/lineage/waydroid_arm64/VANILLA.json 2>/dev/null &&
    [ ! -f "$IMAGES/ota-system-VANILLA.json" ] &&
    mv "$IMAGES/ota-system-VANILLA.json.new" "$IMAGES/ota-system-VANILLA.json" || true
rm -f "$IMAGES/ota-system-VANILLA.json.new"
fetch "$SYSTEM_ZIP" "$SYSTEM_SHA" "$SYSTEM_URL"
fetch "$VENDOR_ZIP" "$VENDOR_SHA" "$VENDOR_URL"
[ "$MODE" = verify ] && exit 0
unpack_img "$SYSTEM_ZIP" system.img
unpack_img "$VENDOR_ZIP" vendor.img

# The volume: case-sensitive, because Android's tree has names that differ
# only by case (a case-insensitive APFS home would merge them).
if [ ! -d "$MOUNT" ] || ! mount | grep -q " on $MOUNT "; then
    if [ ! -d "$BUNDLE" ]; then
        say "creating $BUNDLE (Case-sensitive APFS, sparse, 24 GB max)"
        hdiutil create -quiet -size 24g -type SPARSEBUNDLE -fs "Case-sensitive APFS" \
            -volname SteamARMAndroid "$BUNDLE"
    fi
    hdiutil attach -quiet -nobrowse -mountpoint "$MOUNT" "$BUNDLE"
fi
diskutil info "$MOUNT" | grep -q "Case-sensitive" || die "$MOUNT is not case-sensitive"

busy=$(guests_on_root "$ROOT")
[ -z "$busy" ] || die "guests run on $ROOT (PIDs $busy); stop them first"

if [ -d "$ROOT/system" ]; then
    [ "$MODE" = rebuild ] || { say "$ROOT exists (use --rebuild to make it again)"; exit 0; }
fi
NEW="$MOUNT/root.new"
rm -rf "$NEW"
say "extracting system.img -> $NEW"
extract_fs "$RAW/system.img" "$NEW"
say "extracting vendor.img -> $NEW/vendor"
rm -rf "$NEW/vendor"
extract_fs "$RAW/vendor.img" "$NEW/vendor"

# apexd's job on a device with flattened APEXes: /system/apex/<dir> appears
# at /apex/<name from apex_manifest.pb> (com.android.art.release ->
# com.android.art). Real directories, not links: bionic's linker checks a
# library's real path against its namespace's permitted paths (/apex/...).
python3 $PY apex-names "$NEW" > "$NEW/apex/.names"
while read -r dir name _ver; do
    if [ "$name" = ZIP ]; then
        die "/system/apex/$dir is a packed APEX; this image was expected flattened"
    fi
    cp -cR "$NEW/system/apex/$dir" "$NEW/apex/$name"
done < "$NEW/apex/.names"
rm -f "$NEW/apex/.names"

cat > "$NEW/.steamarm-androidroot" <<EOF
# Made by scripts/mkandroidroot.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)
system $SYSTEM_ZIP sha256 $SYSTEM_SHA
  $SYSTEM_URL
vendor $VENDOR_ZIP sha256 $VENDOR_SHA
  $VENDOR_URL
licence: LineageOS 18.1 / AOSP (Apache-2.0, GPL-2.0 for the kernel-facing
  parts; NOTICE.xml.gz in the image). VANILLA: no Google apps or services.
EOF

if [ -d "$ROOT" ]; then
    # Keep the old root's /data (dalvik-cache, test files).
    if [ -d "$ROOT/data" ]; then
        rm -rf "$NEW/data"
        mv "$ROOT/data" "$NEW/data"
    fi
    mv "$ROOT" "$MOUNT/root.old"
    mv "$NEW" "$ROOT"
    rm -rf "$MOUNT/root.old"
else
    mv "$NEW" "$ROOT"
fi
# State init would create at boot (after the swap: an old /data came along).
# The rest of /data is empty.
mkdir -p "$ROOT/data/dalvik-cache/arm64" "$ROOT/data/local/tmp" "$ROOT/data/misc" \
         "$ROOT/data/system" "$ROOT/dev/socket"
say "root ready: $ROOT"
