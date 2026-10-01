#!/bin/bash
# SteamARM's case-sensitive disk images: the Steam Frame root
# ($STATE/steamframe-root.sparsebundle at /Volumes/SteamFrameRoot) and the
# Android root ($STATE/android.sparsebundle at /Volumes/SteamARMAndroid).
#
#   scripts/image-volume.sh ensure BUNDLE MOUNT
#       attaches BUNDLE at MOUNT if it is not, and grows it first to the size
#       of the Mac volume that holds it when it is smaller. An image already
#       attached is grown only when nothing has a file open on it (detached,
#       grown, attached again); otherwise it is left as it is.
#   scripts/image-volume.sh backing
#       prints LXRT_STATFS_BACKING for the attached images (runtime/
#       fileops2.c): free space inside an image is capped by the Mac's.
#
# Why: the Steam Frame root's image was made with a 40 GB capacity and Steam
# showed 22 GB free on a Mac with 219 GB (benchmarks/stage44). A sparse
# bundle takes only the space of its data, so its capacity can be the whole
# disk; the cap keeps Steam from counting space the Mac does not have.
set -u
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
log() { echo "[image-volume] $*" >&2; }

# Bytes of the volume holding PATH.
volume_bytes() { df -k "$1" 2>/dev/null | awk 'NR == 2 { printf "%.0f\n", $2 * 1024 }'; }
# Capacity of a detached image, in bytes.
image_bytes() { hdiutil imageinfo "$1" 2>/dev/null | awk '/^[[:space:]]*Total Bytes:/ { print $3; exit }'; }
# Capacity of an attached one (imageinfo refuses those: "resource temporarily
# unavailable"), from its volume.
mounted_bytes() { diskutil info "$1" 2>/dev/null | sed -n 's/^ *Disk Size:.*(\([0-9]*\) Bytes).*/\1/p' | head -1; }
attached_at() { # bundle -> mount point, empty when not attached
    hdiutil info 2>/dev/null | awk -v img="$1" '
        /^image-path/ { cur = substr($0, index($0, ":") + 2) }
        /^\/dev\/disk/ && cur == img && NF >= 3 && $NF ~ /^\// { print $NF; exit }'
}

grow() { # bundle: grows it, detached, to the size of the volume holding it
    local bundle=$1 want have
    want=$(volume_bytes "$(dirname "$bundle")")
    have=$(image_bytes "$bundle")
    [ -n "$want" ] && [ -n "$have" ] || return 0
    # 1 GiB of slack: a bundle within that of the disk's size is grown already.
    [ "$have" -lt $((want - 1073741824)) ] || return 0
    local gib=$((want / 1073741824))
    log "growing $(basename "$bundle") from $((have / 1073741824)) GiB to $gib GiB (sparse: no space is taken)"
    diskutil image resize --size "${gib}g" "$bundle" >/dev/null 2>&1 ||
        hdiutil resize -size "${gib}g" "$bundle" >/dev/null 2>&1 ||
        { log "could not grow $bundle"; return 1; }
}

ensure() {
    local bundle=$1 mount=$2 at
    [ -d "$bundle" ] || return 0
    at=$(attached_at "$bundle")
    if [ -n "$at" ]; then
        local want have
        want=$(volume_bytes "$(dirname "$bundle")")
        have=$(mounted_bytes "$at")
        if [ -n "$want" ] && [ -n "$have" ] && [ "$have" -lt $((want - 1073741824)) ]; then
            if [ -z "$(lsof -t "$at" 2>/dev/null | head -1)" ] && hdiutil detach -quiet "$at" 2>/dev/null; then
                at=""
            else
                log "$(basename "$bundle") is in use: it grows the next time nothing has it open"
                return 0
            fi
        else
            return 0
        fi
    fi
    grow "$bundle"
    hdiutil attach -quiet -nobrowse -mountpoint "$mount" "$bundle" ||
        { log "could not attach $bundle at $mount"; return 1; }
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
