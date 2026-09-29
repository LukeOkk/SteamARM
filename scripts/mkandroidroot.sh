#!/bin/bash
# An Android 11 (LineageOS 18.1) root for lxrun, from Waydroid's public
# images, with no VM and no mount of a Linux filesystem (docs/ANDROID_RUNTIME_
# ARCHITECTURE.md, benchmarks/stage25-android-userspace.txt,
# benchmarks/stage25-art-x86-fex.txt).
#
#   scripts/mkandroidroot.sh            download (if needed), verify, extract
#   scripts/mkandroidroot.sh --rebuild  make the root again from the images,
#                                       keeping its /data (dalvik-cache, ...)
#   scripts/mkandroidroot.sh --verify   check the pristine images' sha256 only
#   --arch arm64 (default) | x86_64     which of Waydroid's builds: arm64 runs
#                                       natively under lxrun; x86_64 runs under
#                                       SteamARM's FEX (scripts/run-android-x86.sh)
#   scripts/mkandroidroot.sh --arch x86_64 --emu
#                                       only (re)install the x86_64 root's
#                                       emulator side, e.g. after a FEX rebuild
#
# What it makes (arm64 keeps the names it always had):
#   $ANDROID_STATE/images/            the zips as downloaded (read-only),
#                                     Waydroid's OTA indexes they came from
#   $ANDROID_STATE/images/extracted/  arm64 system.img, vendor.img (raw; read-only)
#   $ANDROID_STATE/images/extracted-x86_64/   the same for x86_64
#   $STATE/android.sparsebundle       a case-sensitive APFS volume, attached at
#                                     /Volumes/SteamARMAndroid (Android's tree
#                                     has names that differ only by case)
#   /Volumes/SteamARMAndroid/root     the arm64 root: system.img's tree (system-
#                                     as-root), vendor.img's under /vendor, each
#                                     flattened APEX of /system/apex copied
#                                     (APFS clones) to /apex/<manifest name>,
#                                     and empty state directories under /data
#   /Volumes/SteamARMAndroid/root-x86_64
#                                     the x86_64 root, made the same way, plus
#                                     the aarch64 side of the emulator in
#                                     /usr/lib/lxrt-emu (as scripts/mksteamroot.sh
#                                     does for the Steam root): FEX-emu as FEX,
#                                     FEXServer, their loader and libraries.
#                                     The runtime runs an x86 ELF under
#                                     /usr/lib/lxrt-emu/FEX (runtime/main.c).
# Run a program in it:
#   LXRT_ROOT=/Volumes/SteamARMAndroid/root LXRT_GUEST_PAGE=4096 \
#       build/lxrun /system/bin/toybox uname -a
#   scripts/run-android-x86.sh /system/bin/toybox uname -a      (x86_64 root)
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
BUNDLE="${ANDROID_BUNDLE:-$STATE/android.sparsebundle}"
MOUNT="${ANDROID_MOUNT:-/Volumes/SteamARMAndroid}"
DEBUGFS="${DEBUGFS:-/opt/homebrew/opt/e2fsprogs/sbin/debugfs}"
FSCK_EROFS="${FSCK_EROFS:-/opt/homebrew/opt/erofs-utils/bin/fsck.erofs}"
PY=scripts/androidroot.py
# The emulator side of the x86_64 root: FEX-emu has PT_INTERP
# /usr/lib/lxrt-emu/ld-linux-aarch64.so.1 and DT_RPATH /usr/lib/lxrt-emu
# (scripts/build-fex-host.sh); FEXServer is the plain build, started through
# that loader (scripts/run-android-x86.sh). The glibc libraries come from the
# aarch64 root, as in scripts/mksteamroot.sh.
FEX_OUT="${STEAMARM_BUILD:-$HOME/SteamARM-build}/out"
FEXEMU="${FEXEMU:-$FEX_OUT/FEX-emu}"
FEXSERVER="${FEXSERVER:-$FEX_OUT/FEXServer}"
ARM_ROOT="${ARM_ROOT:-/tmp/lxrt-root}"

usage() { echo "usage: $0 [--arch arm64|x86_64] [--rebuild|--verify|--emu]" >&2; exit 2; }
ARCH=arm64
MODE=build
while [ $# -gt 0 ]; do
    case "$1" in
        --rebuild) MODE=rebuild ;;
        --verify) MODE=verify ;;
        --emu) MODE=emu ;;
        --arch) [ $# -ge 2 ] || usage; ARCH=$2; shift ;;
        --arch=*) ARCH=${1#--arch=} ;;
        *) usage ;;
    esac
    shift
done

# Pinned: Waydroid's OTA index (https://ota.waydro.id/system/lineage/
# waydroid_<arch>/VANILLA.json, .../vendor/waydroid_<arch>/MAINLINE.json) lists
# each build with its sha256 as "id"; these are the last lineage-18.1
# (Android 11) entries there, the same date for both architectures. Its
# download URLs are SourceForge's.
SF=https://sourceforge.net/projects/waydroid/files/images
case "$ARCH" in
    arm64)
        SYSTEM_ZIP=lineage-18.1-20250628-VANILLA-waydroid_arm64-system.zip
        SYSTEM_SHA=2700a68255c234f04453da15bfdaed0b0d30343f3af968cf39a096657d88a625
        VENDOR_ZIP=lineage-18.1-20250628-MAINLINE-waydroid_arm64-vendor.zip
        VENDOR_SHA=e3b81b0c7f86d316350081a5d43622dd5dc2f9e028f10a715422133853ba886c
        RAW="$IMAGES/extracted"
        ROOT="$MOUNT/root"
        IDX="" ;;
    x86_64)
        SYSTEM_ZIP=lineage-18.1-20250628-VANILLA-waydroid_x86_64-system.zip
        SYSTEM_SHA=662562aa718da1a584520367b39db68f001db5353ebcbf0cf93005088d284097
        VENDOR_ZIP=lineage-18.1-20250628-MAINLINE-waydroid_x86_64-vendor.zip
        VENDOR_SHA=5414a273ac972aa1a65bcca597d5d32933d846dd1628cfdc92413b7da5332189
        RAW="$IMAGES/extracted-x86_64"
        ROOT="$MOUNT/root-x86_64"
        IDX="-x86_64" ;;
    *) echo "mkandroidroot: unknown architecture '$ARCH' (arm64 or x86_64)" >&2; exit 2 ;;
esac
SYSTEM_URL="$SF/system/lineage/waydroid_$ARCH/$SYSTEM_ZIP/download"
VENDOR_URL="$SF/vendor/waydroid_$ARCH/$VENDOR_ZIP/download"
[ "$MODE" != emu ] || [ "$ARCH" = x86_64 ] || { echo "mkandroidroot: --emu is for --arch x86_64" >&2; exit 2; }

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
    # SourceForge's mirrors close a transfer now and then (MEASURED: every
    # ~10 MB); resume where it stopped.
    local i
    for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16 17 18 19 20; do
        curl -L --fail -sS -C - --connect-timeout 30 --speed-limit 20000 --speed-time 30 \
            -o "$f.part" "$3" && break
        say "$1: transfer interrupted ($(stat -f %z "$f.part" 2>/dev/null || echo 0) bytes), resuming"
    done
    sha_ok "$f.part" "$2" || { rm -f "$f.part"; die "$1: sha256 mismatch, not kept"; }
    mv "$f.part" "$f"
    chmod 444 "$f"
    say "$1: downloaded, sha256 ok"
}

index() {   # index KIND URL: keep the first copy of an OTA index
    local f="$IMAGES/ota-$1$IDX.json"
    [ -f "$f" ] && return 0
    curl -sS -m 60 -o "$f.new" "$2" 2>/dev/null && mv "$f.new" "$f" && chmod 444 "$f" || true
    rm -f "$f.new"
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

install_emu() {  # install_emu ROOT: the aarch64 side of the emulator
    local d="$1/usr/lib/lxrt-emu" l src
    [ -f "$FEXEMU" ] || die "no $FEXEMU (scripts/build-fex-host.sh builds FEX-emu)"
    [ -f "$FEXSERVER" ] || die "no $FEXSERVER (scripts/build-fex-host.sh)"
    mkdir -p "$d"
    cp -f "$FEXEMU" "$d/FEX.new" && mv -f "$d/FEX.new" "$d/FEX"
    cp -f "$FEXSERVER" "$d/FEXServer.new" && mv -f "$d/FEXServer.new" "$d/FEXServer"
    for l in ld-linux-aarch64.so.1 libstdc++.so.6 libm.so.6 libgcc_s.so.1 libc.so.6; do
        src="$ARM_ROOT/usr/lib64/$l"; [ -e "$src" ] || src="$ARM_ROOT/usr/lib/$l"
        [ -e "$src" ] || die "no $l in $ARM_ROOT (the aarch64 root, scripts/mkroot-rpm.sh)"
        cp -fL "$src" "$d/$l"
    done
    say "emulator side: $d ($(shasum -a 256 "$d/FEX" | cut -c1-12)... FEX-emu)"
}

if [ "$MODE" = emu ]; then
    [ -d "$ROOT/system" ] || die "no root at $ROOT (run without --emu first)"
    busy=$(guests_on_root "$ROOT")
    [ -z "$busy" ] || die "guests run on $ROOT (PIDs $busy); stop them first"
    install_emu "$ROOT"
    exit 0
fi

mkdir -p "$IMAGES" "$RAW"
index system-VANILLA "https://ota.waydro.id/system/lineage/waydroid_$ARCH/VANILLA.json"
index vendor-MAINLINE "https://ota.waydro.id/vendor/waydroid_$ARCH/MAINLINE.json"
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
NEW="$ROOT.new"
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

[ "$ARCH" = arm64 ] || install_emu "$NEW"

# An account for the Mac user's uid, which the runtime passes through (as
# scripts/mksteamroot.sh does for the Steam root). bionic knows Android's
# fixed ids (AID_*) and app ids, and reads /system/etc/passwd and group (empty
# in this image) for others, if the name starts with "system_". Without an
# entry getpwuid(getuid()) is ENOENT: toybox id says "bad uid", and ART's
# System.<clinit> throws (user.home, user.name) and the runtime aborts
# (MEASURED, benchmarks/stage25-art-x86-fex.txt).
ME_UID=$(id -u) ME_GID=$(id -g)
printf 'system_steamarm:x:%s:%s::/data/local/tmp:/system/bin/sh\n' "$ME_UID" "$ME_GID" > "$NEW/system/etc/passwd"
printf 'system_steamarm:x:%s:\n' "$ME_GID" > "$NEW/system/etc/group"

cat > "$NEW/.steamarm-androidroot" <<EOF
# Made by scripts/mkandroidroot.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)
arch $ARCH
system $SYSTEM_ZIP sha256 $SYSTEM_SHA
  $SYSTEM_URL
vendor $VENDOR_ZIP sha256 $VENDOR_SHA
  $VENDOR_URL
licence: LineageOS 18.1 / AOSP (Apache-2.0, GPL-2.0 for the kernel-facing
  parts; NOTICE.xml.gz in the image). VANILLA: no Google apps or services.
EOF
[ "$ARCH" = arm64 ] || cat >> "$NEW/.steamarm-androidroot" <<EOF
usr/lib/lxrt-emu: SteamARM's FEX (MIT, patches/ in the repository) and the
  aarch64 glibc/libstdc++ it links against (LGPL-2.1+/GPL-3.0 with the GCC
  runtime exception), copied from $ARM_ROOT.
EOF

if [ -d "$ROOT" ]; then
    # Keep the old root's /data (dalvik-cache, test files).
    if [ -d "$ROOT/data" ]; then
        rm -rf "$NEW/data"
        mv "$ROOT/data" "$NEW/data"
    fi
    mv "$ROOT" "$ROOT.old"
    mv "$NEW" "$ROOT"
    rm -rf "$ROOT.old"
else
    mv "$NEW" "$ROOT"
fi
# State init would create at boot (after the swap: an old /data came along).
# The rest of /data is empty.
mkdir -p "$ROOT/data/dalvik-cache/$ARCH" "$ROOT/data/local/tmp" "$ROOT/data/misc" \
         "$ROOT/data/system" "$ROOT/dev/socket"
say "root ready: $ROOT"
