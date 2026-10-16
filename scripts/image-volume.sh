#!/bin/bash
# SteamARM's case-sensitive disk images: the Steam Frame root
# ($STATE/steamframe-root.sparsebundle at /Volumes/SteamFrameRoot) and the
# Android root ($STATE/android.sparsebundle at /Volumes/SteamARMAndroid).
#
#   scripts/image-volume.sh ensure BUNDLE MOUNT
#       attaches BUNDLE at MOUNT if it is not, and grows it first to the size
#       of the Mac volume that holds it when it is smaller. An image already
#       attached is grown only when nothing has a file open on it (detached,
#       grown, attached again); otherwise it is left as it is. An image
#       attached read-only is attached again, read-write, when nothing has a
#       file open on it; otherwise ensure fails and says so. One ensure at a
#       time (lockf on $STATE/launcher/.image-volume.lock).
#   scripts/image-volume.sh backing
#       prints LXRT_STATFS_BACKING for the attached images (runtime/
#       fileops2.c): free space inside an image is capped by the Mac's.
#
# Why: the Steam Frame root's image was made with a 40 GB capacity and Steam
# showed 22 GB free on a Mac with 219 GB (benchmarks/stage44). A sparse
# bundle takes only the space of its data, so its capacity can be the whole
# disk; the cap keeps Steam from counting space the Mac does not have.
set -u
STATE="${STEAMARM_STATE:-$(d="$HOME/Developer/SteamARM-roots"; [ -d "$d" ] || [ ! -d "$HOME/SteamARM-roots" ] || d="$HOME/SteamARM-roots"; echo "$d")}"
log() { echo "[image-volume] $*" >&2; }

# One ensure at a time. The launcher attaches the image when it opens
# (LauncherModel.attachRootImages) and run-app.sh before every start: two at
# once detached what the other had just attached ("could not attach"), and an
# attach while the other had the bundle open came up read-only -- Steam's
# root read-only, PulseAudio unable to make its socket there (MEASURED
# 2026-10-15, benchmarks/stage63-pulseaudio-fork.txt).
if [ "${1:-}" = ensure ] && [ -z "${STEAMARM_IMAGE_VOLUME_LOCKED:-}" ] && [ -x /usr/bin/lockf ]; then
    mkdir -p "$STATE/launcher" 2>/dev/null
    STEAMARM_IMAGE_VOLUME_LOCKED=1 exec /usr/bin/lockf -k -t 300 "$STATE/launcher/.image-volume.lock" "$0" "$@"
fi

# Bytes of the volume holding PATH.
volume_bytes() { df -k "$1" 2>/dev/null | awk 'NR == 2 { printf "%.0f\n", $2 * 1024 }'; }
# The capacity BUNDLE should have: the Mac volume's (STEAMARM_IMAGE_TARGET_BYTES
# instead, for tests/launcher/image_volume.sh).
target_bytes() { echo "${STEAMARM_IMAGE_TARGET_BYTES:-$(volume_bytes "$(dirname "$1")")}"; }
# True when the volume mounted at MOUNT is read-only (the image or the mount).
read_only() { diskutil info "$1" 2>/dev/null | grep -q -E '^ *(Media|Volume) Read-Only: *Yes'; }
# Capacity of a detached image, in bytes.
image_bytes() { hdiutil imageinfo "$1" 2>/dev/null | awk '/^[[:space:]]*Total Bytes:/ { print $3; exit }'; }
# Capacity of an attached one (imageinfo refuses those: "resource temporarily
# unavailable"): its block count in hdiutil info. Not the mounted volume's
# size: that leaves out the partition map and the EFI partition (1861.8 GiB
# of an image of 1862), and every check found the image "smaller" than the
# target and detached it again (MEASURED 2026-10-15).
attached_bytes() {
    hdiutil info 2>/dev/null | awk -v img="$1" '
        /^image-path/ { cur = substr($0, index($0, ":") + 2); n = bs = 0 }
        cur == img && /^blockcount/ { n = $NF }
        cur == img && /^blocksize/ { bs = $NF }
        cur == img && /^\/dev\/disk/ && n && bs { printf "%.0f\n", n * bs; exit }'
}
# The mounted volume's size, plus what its partitions take, when hdiutil info
# gives no block count.
mounted_bytes() {
    diskutil info "$1" 2>/dev/null | sed -n 's/^ *Disk Size:.*(\([0-9]*\) Bytes).*/\1/p' |
        awk 'NR == 1 { printf "%.0f\n", $1 + 536870912 }'
}
attached_at() { # bundle -> mount point, empty when not attached
    hdiutil info 2>/dev/null | awk -v img="$1" '
        /^image-path/ { cur = substr($0, index($0, ":") + 2) }
        /^\/dev\/disk/ && cur == img && NF >= 3 && $NF ~ /^\// { print $NF; exit }'
}

grow() { # bundle: grows it, detached, to the size of the volume holding it
    local bundle=$1 want have
    want=$(target_bytes "$bundle")
    have=$(image_bytes "$bundle")
    [ -n "$want" ] && [ -n "$have" ] || return 0
    # 1 GiB of slack: a bundle within that of the disk's size is grown already.
    [ "$have" -lt $((want - 1073741824)) ] || return 0
    local gib=$((want / 1073741824))
    log "growing $(basename "$bundle") from $((have / 1073741824)) GiB to $gib GiB (sparse: no space is taken)"
    # In bytes: diskutil's "g" is 10^9 bytes (hdiutil's is 2^30). "${gib}g"
    # gave 1734 GiB for 1862: still "smaller", so every start detached the
    # image, grew it to the same size and attached it again (MEASURED
    # 2026-10-15).
    diskutil image resize --size "$((gib * 1073741824))B" "$bundle" >/dev/null 2>&1 ||
        hdiutil resize -size "${gib}g" "$bundle" >/dev/null 2>&1 ||
        { log "could not grow $bundle"; return 1; }
}

ensure() {
    local bundle=$1 mount=$2 at
    [ -d "$bundle" ] || return 0
    at=$(attached_at "$bundle")
    if [ -n "$at" ]; then
        local want have why=""
        want=$(target_bytes "$bundle")
        have=$(attached_bytes "$bundle")
        [ -n "$have" ] || have=$(mounted_bytes "$at")
        if read_only "$at"; then
            why=read-only
        elif [ -n "$want" ] && [ -n "$have" ] && [ "$have" -lt $((want - 1073741824)) ]; then
            why=small
        fi
        [ -n "$why" ] || return 0
        if [ -z "$(lsof -t "$at" 2>/dev/null | head -1)" ] && hdiutil detach -quiet "$at" 2>/dev/null; then
            at=""
        elif [ "$why" = read-only ]; then
            log "$(basename "$bundle") is attached READ-ONLY at $at and in use: stop what uses it, then start again"
            return 1
        else
            log "$(basename "$bundle") is in use: it grows the next time nothing has it open"
            return 0
        fi
    fi
    grow "$bundle"
    hdiutil attach -quiet -nobrowse -mountpoint "$mount" "$bundle" ||
        { log "could not attach $bundle at $mount"; return 1; }
    if read_only "$mount"; then
        log "$(basename "$bundle") was attached READ-ONLY at $mount (is another process using the image?)"
        return 1
    fi
}

case "${1:-}" in
    ensure)
        [ $# -eq 3 ] || { echo "usage: $0 ensure BUNDLE MOUNT" >&2; exit 2; }
        ensure "$2" "$3"
        ;;
    backing)
        out=""
        for b in "$STATE"/*.sparsebundle; do
            [ -d "$b" ] || continue
            at=$(attached_at "$b")
            [ -n "$at" ] && out="${out:+$out;}$at=$(dirname "$b")"
        done
        echo "$out"
        ;;
    *)
        sed -n '2,20p' "$0" >&2
        exit 2
        ;;
esac
