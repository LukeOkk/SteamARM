#!/usr/bin/env bash
# Fetch FEX's x86-64 rootfs (Ubuntu 24.04) and unpack it on the macOS host,
# with no VM anywhere.
#
# Why this exists: exit criterion #8 -- every component must be obtainable
# without the Fedora VM. The x86 tree FEX overlays on "/" (FEX_ROOTFS in
# scripts/run-fex.sh) and that scripts/mksteamroot.sh turns into the Steam
# root was fetched and unpacked by FEXRootFSFetcher inside the VM, then copied
# out. This script gets the same image the same way FEXRootFSFetcher does:
#
#   index    https://rootfs.fex-emu.gg/RootFS_links.json -- the DownloadURL in
#            FEX's Source/Tools/FEXRootFSFetcher/Main.cpp. Per distro it lists
#            a SquashFS and an EroFS image and "Hash": XXH3-64, seed 0, of the
#            whole file (XXFileHash.cpp: XXH3_64bits_reset_withSeed(0)).
#   fetch    the pinned image below over HTTPS (cached in
#            $STEAMARM_BUILD/rootfs-images/<distro>/<date>/), checked against
#            the index's XXH3 (xxhsum -H3) and against our own sha256 pin.
#   extract  SquashFS (default): unsquashfs -f -d, as FEXRootFSFetcher's
#            UnsquashRootFS() runs it (Homebrew squashfs; zstd-compressed).
#            EroFS (--erofs): fsck.erofs --extract, as its ExtractEroFS()
#            does (Homebrew erofs-utils; lz4). Both into <out-dir>.new.<pid>,
#            moved to <out-dir> only when complete and checked.
#   check    the loader, libc, bash and python3 are there, and every path of
#            the image is there once -- unless the volume is case-insensitive
#            (the macOS default) and two paths differ only in case: those are
#            listed (Ubuntu 24.04: perl's Pod/pod and Sys/sys directories,
#            which then share one directory -- no file is lost).
#   fixups   the changes made on top of the image
#            (skipped with --pristine), so the result equals it:
#            - 8 Vulkan ICD manifests (asahi dzn freedreno gfxstream_vk
#              intel_hasvk intel nouveau radeon) moved from
#              usr/share/vulkan/icd.d to icd.d.disabled, and virtio's deleted
#              (Mesa Venus, which needs a virtio-gpu device that only the
#              retired VM had): only lvp stays visible to the loader.
#            - Ubuntu 24.04's i386 GTK 2 and libXtst (libgtk2.0-0t64
#              2.24.33-4ubuntu1.1, libxtst6 2:1.2.3-1.1build1; the image has
#              neither), from archive.ubuntu.com (Launchpad as fallback),
#              sha256 pinned from the noble Packages index; only their
#              usr/lib/i386-linux-gnu is unpacked, as in the VM-era tree.
#            - lsof (4.95.0-1build3) with libtirpc3t64 and libtirpc-common
#              (1.3.4+ds-1.1build1), amd64: the Steam client identifies the
#              process behind every websocket connection from its UI with
#              `lsof -i TCP@127.0.0.1:<port>` and rejects the connection when
#              that fails ("lsof is required to run steam"; without it the
#              UI dies with "Unexpected Transport Error 0x3008"). The image
#              has none of the three; the krb5 libraries libtirpc needs are
#              there.
#
# The pins: FEX publishes new images under new dated paths and keeps the old
# ones; `--index` prints the current index entries to move to a newer one.
# Nothing is run from the image (no chroot.py, no scriptlets); FEX uses the
# tree as is.
#
# Usage: scripts/fetch-x86-rootfs.sh [--erofs] [--pristine] <out-dir>
#        scripts/fetch-x86-rootfs.sh --index
#   <out-dir> must not exist, e.g. $HOME/SteamARM-roots/x86-rootfs, or
#   <lxrt-root>/tmp/fexhome/.local/share/fex-emu/RootFS/Ubuntu_24_04 (then
#   FEX_ROOTFS=/tmp/fexhome/.local/share/fex-emu/RootFS/Ubuntu_24_04).
#
# Environment: STEAMARM_BUILD  work directory (default $HOME/SteamARM-build)
#
# Needs (Homebrew): xxhash (xxhsum), squashfs (unsquashfs) or, for --erofs,
# erofs-utils (fsck.erofs), zstd (the .deb payloads). Plus the system curl,
# shasum, python3, bsdtar.
set -euo pipefail

WORK="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
INDEX_URL=https://rootfs.fex-emu.gg/RootFS_links.json
DISTRO="Ubuntu 24.04"                     # the index entry name, minus " (SquashFS)"
DATE=2026-08-11                           # the image FEX published that day
BASE="https://rootfs.fex-emu.gg/Ubuntu_24_04/$DATE"
# kind  file               size       XXH3 (the index)  sha256 (measured here, 2026-09-26)
SQSH=(squashfs Ubuntu_24_04.sqsh 525615104 3517e0e5ea25a473 2854b06d3ff1b8f6e526135bfb6dd5b7b30ab3ab73e79ae933a3d9fed959a178)
ERO=(erofs     Ubuntu_24_04.ero  884666368 52d6c0dab39efaed 005ec6a45f014244feea7e5250a965b654d70c7ad048117288ba7f5532c33c02)

# The i386 additions (fixups): file, sha256 (noble / noble-updates Packages).
UBUNTU_POOL=http://archive.ubuntu.com/ubuntu/pool/main
LAUNCHPAD=https://launchpad.net/ubuntu/+archive/primary/+files
# path in the pool, sha256, the member of the package's data to unpack.
DEBS=(
  "g/gtk+2.0/libgtk2.0-0t64_2.24.33-4ubuntu1.1_i386.deb 0b3da149174bd015808eaa2fea6cb6613f1cc03497722cc0ee0b247c55423762 ./usr/lib/i386-linux-gnu"
  "libx/libxtst/libxtst6_1.2.3-1.1build1_i386.deb       c5183c2ab743b708671494f176f1b947936da4964bc234fcd0a5daebe0e60629 ./usr/lib/i386-linux-gnu"
  "l/lsof/lsof_4.95.0-1build3_amd64.deb                 46165f06b9568f9e2718f1fdd3d3a8db46aa597f5ff163c1d21c0d1daa12191d ./usr/bin/lsof"
  "libt/libtirpc/libtirpc3t64_1.3.4+ds-1.1build1_amd64.deb 3a3cd37160399ab235fdf2f13159fd288940abb9660e0ed1afb418b44c73d43a ./usr/lib/x86_64-linux-gnu"
  "libt/libtirpc/libtirpc-common_1.3.4+ds-1.1build1_all.deb 212d8873ac952bc68f4cf56d7eaf17566f86f5c7cc4b971899a0c56e174699cc ./etc/netconfig"
)
DISABLED_ICDS=(asahi dzn freedreno gfxstream_vk intel_hasvk intel nouveau radeon)
DELETED_ICDS=(virtio)

log() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null || die "missing $1 (brew install $2)"; }

show_index() {
    log "== $INDEX_URL"
    curl -fsSL --retry 3 --connect-timeout 20 "$INDEX_URL" | python3 -c '
import json, sys
for name, e in json.load(sys.stdin)["v1"].items():
    print("  %-28s %-8s %16s  %s" % (name, e["Type"], e["Hash"], e["URL"]))'
}

fetch() {   # kind file size xxh3 sha256 -> cached image path on stdout
    local kind="$1" file="$2" size="$3" xxh="$4" sum="$5"
    local dir="$WORK/rootfs-images/Ubuntu_24_04/$DATE" img
    img="$dir/$file"
    mkdir -p "$dir"
    # The index prints the hash with %x (no leading zeros): compare as numbers.
    # The pin must still be what the index says today for this URL.
    local listed
    listed="$(curl -fsSL --retry 3 --connect-timeout 20 "$INDEX_URL" | python3 -c '
import json, sys
url = sys.argv[1]
for e in json.load(sys.stdin)["v1"].values():
    if e["URL"] == url:
        print(e["Hash"])' "$BASE/$file" || true)"
    if [ -z "$listed" ]; then
        log "  note: $BASE/$file is no longer in the index (--index shows what is); using the pin" >&2
    elif [ "$((16#$listed))" != "$((16#$xxh))" ]; then
        die "the index now says XXH3 $listed for $BASE/$file, the pin says $xxh"
    fi
    if [ -f "$img" ] && [ "$(stat -f %z "$img")" = "$size" ]; then
        log "  cached  $img" >&2
    else
        log "  fetch   $BASE/$file ($size bytes)" >&2
        curl -fL --retry 3 --connect-timeout 20 -C - -o "$img.part" "$BASE/$file" 2>/dev/null \
            || curl -fL --retry 3 --connect-timeout 20 -o "$img.part" "$BASE/$file" \
            || die "cannot download $BASE/$file"
        mv "$img.part" "$img"
    fi
    local got
    got="$(xxhsum -H3 "$img" | awk '{print $1}')"; got="${got#XXH3_}"
    [ "$((16#$got))" = "$((16#$xxh))" ] || die "$img: XXH3 $got, expected $xxh (delete it and retry)"
    got="$(shasum -a 256 "$img" | cut -d' ' -f1)"
    [ "$got" = "$sum" ] || die "$img: sha256 $got, expected $sum"
    log "  ok      XXH3 $xxh (as in FEX's index), sha256 $sum" >&2
    printf '%s\n' "$img"
}

# Paths of the image, one per line ("/usr/bin/bash"), from the image itself.
list_image() {   # kind image
    if [ "$1" = squashfs ]; then
        unsquashfs -d '' -l "$2" 2>/dev/null
    else
        python3 - "$2" <<'PY'
import subprocess, sys
# dump.erofs --ls --path=X lists one directory ("NID TYPE FILENAME" rows,
# type 2 = directory): walk the tree with it.
todo, out = ['/'], []
while todo:
    d = todo.pop()
    r = subprocess.run(['dump.erofs', '--ls', f'--path={d}', sys.argv[1]],
                       capture_output=True, text=True, check=True)
    rows = r.stdout.splitlines()
    start = next(i for i, l in enumerate(rows) if l.split() == ['NID', 'TYPE', 'FILENAME']) + 1
    for line in rows[start:]:
        nid, typ, name = line.split(None, 2)
        if name in ('.', '..'):
            continue
        p = d.rstrip('/') + '/' + name
        out.append(p)
        if typ == '2':
            todo.append(p)
print('\n'.join(out))
PY
    fi
}

extract() {   # kind image dest
    local kind="$1" img="$2" dest="$3"
    if [ "$kind" = squashfs ]; then
        need unsquashfs squashfs
        # No xattrs in these images ("Xattrs are not stored"); -no-xattrs
        # anyway, macOS would refuse Linux security.* names. -f: on a
        # case-insensitive volume the second of Pod/pod already "exists".
        unsquashfs -f -no-xattrs -no-progress -d "$dest" "$img" > "$dest.log" 2>&1 \
            || { cat "$dest.log" >&2; die "unsquashfs failed"; }
    else
        need fsck.erofs erofs-utils
        # Exact permissions, owned by the user running this (not root).
        fsck.erofs --extract="$dest" --preserve-perms --no-preserve-owner "$img" > "$dest.log" 2>&1 \
            || { cat "$dest.log" >&2; die "fsck.erofs failed"; }
    fi
    awk '/^created [0-9]+ (files|directories|symlinks|hardlinks)$/ {
             printf "%s%s %s", n++ ? ", " : "  ", $2, $3 } END { if (n) print "" }' "$dest.log"
    rm -f "$dest.log"
}

check_tree() {   # kind image dest
    local kind="$1" img="$2" dest="$3" f
    for f in usr/lib/x86_64-linux-gnu/libc.so.6 lib64/ld-linux-x86-64.so.2 usr/bin/bash usr/bin/python3; do
        [ -e "$dest/$f" ] || die "extracted tree has no $f"
    done
    list_image "$kind" "$img" | python3 -c '
import collections, os, sys
dest = sys.argv[1]
paths = sorted({p.rstrip("\n") for p in sys.stdin if p.strip() not in ("", "/")})
by = collections.defaultdict(list)
for p in paths:
    by[p.lower()].append(p)
clash = [v for v in by.values() if len(v) > 1]
missing = [p for p in paths if not os.path.lexists(dest + p)]
for c in clash:
    print("  case-insensitive clash (one entry on this volume):", " ".join(c))
for p in missing[:20]:
    print("  MISSING", p)
print(f"  {len(paths)} paths in the image, {len(missing)} missing from the tree, {len(clash)} case clashes")
sys.exit(1 if missing else 0)' "$dest"
}

fixups() {   # dest
    local dest="$1" icd e path sum member deb
    mkdir -p "$dest/usr/share/vulkan/icd.d.disabled"
    for icd in "${DISABLED_ICDS[@]}"; do
        mv "$dest/usr/share/vulkan/icd.d/${icd}_icd.json" "$dest/usr/share/vulkan/icd.d.disabled/"
    done
    for icd in "${DELETED_ICDS[@]}"; do
        rm -f "$dest/usr/share/vulkan/icd.d/${icd}_icd".*json
    done
    log "  ${#DISABLED_ICDS[@]} Vulkan ICDs -> usr/share/vulkan/icd.d.disabled, ${DELETED_ICDS[*]} deleted (left: $(ls "$dest/usr/share/vulkan/icd.d" | tr '\n' ' '))"
    local dir="$WORK/rootfs-images/ubuntu-debs"
    mkdir -p "$dir"
    for e in "${DEBS[@]}"; do
        # shellcheck disable=SC2086
        set -- $e; path="$1" sum="$2" member="$3" deb="$dir/$(basename "$1")"
        if [ ! -f "$deb" ] || [ "$(shasum -a 256 "$deb" | cut -d' ' -f1)" != "$sum" ]; then
            curl -fsL --retry 3 --connect-timeout 20 -o "$deb.part" "$UBUNTU_POOL/$path" \
                || curl -fsL --retry 3 --connect-timeout 20 -o "$deb.part" "$LAUNCHPAD/$(basename "$path")" \
                || die "cannot download $(basename "$path")"
            [ "$(shasum -a 256 "$deb.part" | cut -d' ' -f1)" = "$sum" ] || die "$(basename "$path"): sha256 mismatch"
            mv "$deb.part" "$deb"
        fi
        # A .deb is an ar archive; bsdtar reads it and its data.tar.zst.
        tar -xOf "$deb" 'data.tar*' | tar -xf - -C "$dest" "$member"
        log "  $(basename "$deb") -> ${member#./} (sha256 ${sum:0:16}...)"
    done
}

main() {
    local img_spec=("${SQSH[@]}") pristine=0
    while [ $# -gt 0 ]; do
        case "$1" in
            --index) show_index; return 0 ;;
            --erofs) img_spec=("${ERO[@]}"); shift ;;
            --pristine) pristine=1; shift ;;
            *) break ;;
        esac
    done
    [ $# -eq 1 ] || { sed -n '/^# Usage:/,/^#   FEX_ROOTFS=/p' "$0" >&2; exit 2; }
    local out="$1"
    [ -e "$out" ] && die "refusing: $out exists (remove it first)"
    need curl curl; need python3 python; need xxhsum xxhash; need zstd zstd
    local t0; t0=$(date +%s)
    log "== $DISTRO ($DATE, ${img_spec[0]}) -> $out"
    local img; img="$(fetch "${img_spec[@]}")"
    local new="$out.new.$$"
    rm -rf "$new"; mkdir -p "$(dirname "$out")"
    log "== extract $(basename "$img")"
    local t1; t1=$(date +%s)
    extract "${img_spec[0]}" "$img" "$new"
    log "  extracted in $(( $(date +%s) - t1 )) s"
    log "== check"
    check_tree "${img_spec[0]}" "$img" "$new"
    if [ "$pristine" = 0 ]; then
        log "== fixups (--pristine skips them)"
        fixups "$new"
    fi
    mv "$new" "$out"
    log "== done in $(( $(date +%s) - t0 )) s: $out, $(du -sh "$out" | cut -f1)"
    log "   as FEX's rootfs: FEX_ROOTFS=<guest path of $out>; for Steam: scripts/mksteamroot.sh $out ..."
}
main "$@"
