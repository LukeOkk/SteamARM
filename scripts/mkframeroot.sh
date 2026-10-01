#!/bin/bash
# The root Valve's native arm64 Steam client runs in, derived from the Steam
# Frame image (SteamOS "holo", Arch-based; docs/STEAM_FRAME_IMAGE.md) instead
# of the Fedora root of scripts/mkarmroot.sh. No VM, no emulation.
#
#   scripts/mkframeroot.sh [--adopt] [--home-from DIR] [SRC [OUT]]
#
#   SRC  the extraction of the image (default /Volumes/SteamFrameRoot/rootfs).
#        Only read: the derived root is an APFS clone of it (cp -c), so SRC
#        and OUT must be on the same volume.
#   OUT  the derived root (default: arm64root beside SRC). Linked as
#        $STEAMARM_STATE/arm64root; scripts/env-links.sh then links
#        /tmp/lxrt-arm64root to it.
#   --adopt          OUT exists but was not made by this script (the stage 22
#                    hand clone): rebuild it like one of ours.
#   --home-from DIR  OUT has no client home yet (tmp/armhome): copy one from
#                    DIR, without steamapps/common (for example the Fedora
#                    root's, ~/SteamARM-roots/armroot/tmp/armhome).
#
# A rebuild keeps OUT/tmp (the client's home, tmp/armhome) and OUT/opt/apps,
# and is built beside the old root (OUT.new) and swapped in at the end.
# Nothing extracted from the image leaves the Mac: the image holds proprietary
# Valve software (docs/STEAM_FRAME_IMAGE.md, "Licence").
#
# What the clone gets on top of the extraction (benchmarks/stage23-frame-root.txt):
#   etc/resolv.conf  The image has none (systemd-resolved writes it on the
#                    device). Without it glibc asks 127.0.0.1 and the client's
#                    update check fails with "http error 0" (MEASURED, F1).
#   etc/localtime    The host's zone (the image says America/Los_Angeles), so
#                    the client's logs line up with the Mac's clock.
#   .lxrt-guest-env  Environment for guests of this root, read by
#                    scripts/run-steam-arm64.sh. The image's Mesa
#                    (deckard-mesa: zink and display-only drivers, no swrast
#                    or llvmpipe) finds no GLX visual on the Mac's X server,
#                    and the client stops at "glXChooseVisual failed"
#                    (stage 22 E19). Indirect GLX through the X server works:
#                      __GLX_VENDOR_LIBRARY_NAME=mesa  libglvnd's fallback
#                        vendor is "indirect", and the image has no
#                        libGLX_indirect.so.0;
#                      MESA_LOADER_DRIVER_OVERRIDE=swrast  anything but zink:
#                        otherwise this Mesa insists on zink and gives up
#                        ("DRI3 not available") instead of going indirect;
#                      LIBGL_ALWAYS_INDIRECT=1  skip the direct attempt.
#                    (MEASURED with a GLX probe: GL 1.4 "Apple M4" through
#                    the X server's +iglx.)
#   usr/lib/libvulkan_steamarm.so, usr/share/vulkan/icd.d/steamarm_icd.aarch64.json
#                    SteamARM's Vulkan shim as an ICD for the image's own
#                    loader; the Qualcomm ICD moved to icd.d.lxrt-off
#                    (scripts/install-frameroot-vulkan.sh, which setup.sh
#                    runs again after an update).
#   .lxrt-frameroot  this script's marker: the image's BUILD_ID and the
#                    list above.
# What the image already has that the Fedora root needed seeds for (stage 22):
# X locale data, C.UTF-8 and en_US/es_* glibc locales, lsof, libnssckbi.so.
set -euo pipefail
CANON_BASE=$PWD                        # relative SRC/OUT/DIR: the caller's directory
cd "$(dirname "$0")/.." || exit 1
. scripts/roots.sh
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
ADOPT=0 HOME_FROM=""
while [ $# -gt 0 ]; do
    case "$1" in
        --adopt) ADOPT=1; shift ;;
        --home-from) HOME_FROM="${2:?--home-from needs a directory}"; shift 2 ;;
        -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
        -*) echo "unknown option $1" >&2; exit 2 ;;
        *) break ;;
    esac
done
SRC_ARG="${1:-/Volumes/SteamFrameRoot/rootfs}"
refuse() { echo "refusing: $*" >&2; exit 1; }
# Every path resolved (symlinks, trailing slashes) before anything is made,
# cloned or deleted: a SRC that is a symlink was cloned as the link itself,
# and every write meant for the derived root then went into the extraction.
SRC=$(canon_path "$SRC_ARG") && [ -d "$SRC" ] ||
    { echo "no extraction at $SRC_ARG (docs/STEAM_FRAME_IMAGE.md)" >&2; exit 1; }
[ -f "$SRC/etc/os-release" ] || { echo "no extraction at $SRC_ARG (docs/STEAM_FRAME_IMAGE.md)" >&2; exit 1; }
grep -qx 'ID=steamos' "$SRC/etc/os-release" ||
    { echo "$SRC_ARG is not a SteamOS root (etc/os-release)" >&2; exit 1; }
OUT_ARG="${2:-$(dirname "$SRC_ARG")/arm64root}"
OUT=$(canon_path "$OUT_ARG") ||
    refuse "$OUT_ARG: not a usable path (dangling symlink, a symlink to a file, or . / .. in a part that does not exist)"
[ ! -e "$OUT" ] || [ -d "$OUT" ] || refuse "$OUT_ARG is not a directory"
# Neither may hold the other: OUT inside the extraction writes into it (even
# mkdir -p of OUT's parent did, before this check came first), and an
# extraction inside OUT, OUT.new or OUT.old is deleted with them.
if path_within "$OUT" "$SRC"; then refuse "$OUT_ARG is inside the extraction"; fi
for victim in "$OUT" "$OUT.new" "$OUT.old"; do
    if path_within "$SRC" "$victim"; then refuse "the extraction is inside $victim"; fi
done
for spared in "$STATE" "$PWD" "$HOME"; do
    s=$(canon_path "$spared") || continue
    for victim in "$OUT" "$OUT.new" "$OUT.old"; do
        if path_within "$s" "$victim"; then refuse "$OUT_ARG: $victim would contain $spared"; fi
    done
done
# OUT.new is made in OUT's parent (or where mkdir -p would put it).
[ "$(stat -f %d "$SRC")" = "$(stat -f %d "$(existing_ancestor "$(dirname "$OUT")")")" ] ||
    refuse "$OUT_ARG is not on the extraction's volume (no APFS clone)"
if [ -e "$OUT" ] && [ ! -f "$OUT/.lxrt-frameroot" ] && [ "$ADOPT" != 1 ]; then
    refuse "$OUT_ARG exists and was not made by this script (--adopt to rebuild it)"
fi
if [ -n "$HOME_FROM" ]; then
    h=$(canon_path "$HOME_FROM") || h=""
    [ -n "$h" ] && [ -d "$h/.local/share/Steam" ] ||
        { echo "--home-from: no Steam install in $HOME_FROM/.local/share/Steam" >&2; exit 1; }
    HOME_FROM=$h
    # Read after the swap: what the swap deletes is gone by then.
    case "$HOME_FROM/" in "$OUT"/tmp/*|"$OUT"/opt/apps/*) ;;
        *) for victim in "$OUT" "$OUT.new" "$OUT.old"; do
               if path_within "$HOME_FROM" "$victim"; then refuse "--home-from $HOME_FROM is inside $victim"; fi
           done ;;
    esac
fi
in_use=$(guests_on_root "$OUT")
[ -z "$in_use" ] || refuse "guests are running on $OUT (pid $(echo $in_use)); stop them first"
build_id=$(sed -n 's/^BUILD_ID=//p' "$SRC/etc/os-release")
version_id=$(sed -n 's/^VERSION_ID=//p' "$SRC/etc/os-release" | tr -d '"')

NEW="$OUT.new"
mkdir -p "$(dirname "$OUT")"
# A run that stopped during the swap left the client's home in $NEW: put it
# back before anything is deleted; a leftover that still holds a home is
# never deleted (scripts/roots.sh). --adopt: OUT has no marker of ours.
if [ "$ADOPT" = 1 ]; then root_recover "$OUT"; else root_recover "$OUT" .lxrt-frameroot; fi
root_clear "$OUT"
SECONDS=0
cp -Rc "$SRC" "$NEW"
echo "cloned $SRC in ${SECONDS} s"
mkdir -p "$NEW/run" "$NEW/dev/shm"

rm -f "$NEW/etc/resolv.conf"
printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > "$NEW/etc/resolv.conf"
host_zone=$(readlink /etc/localtime 2>/dev/null || true)
host_zone=${host_zone##*/zoneinfo/}
if [ -n "$host_zone" ] && [ -f "$NEW/usr/share/zoneinfo/$host_zone" ]; then
    ln -sfn "../usr/share/zoneinfo/$host_zone" "$NEW/etc/localtime"
fi
cat > "$NEW/.lxrt-guest-env" <<'EOF'
# Guest environment for this root (scripts/mkframeroot.sh); a variable
# already set by the caller wins. Indirect GLX through the X server: the
# image's Mesa has no software driver usable on macOS.
__GLX_VENDOR_LIBRARY_NAME=mesa
MESA_LOADER_DRIVER_OVERRIDE=swrast
LIBGL_ALWAYS_INDIRECT=1
EOF
# Vulkan for the client (steamsysinfo's GPU topology, the webhelper): the
# shim as an ICD, the image's Qualcomm ICD set aside.
if [ ! -f build/libvulkan.so.1 ]; then
    echo "note: no build/libvulkan.so.1 (make shim): no Vulkan device in this root"
elif [ ! -f "$NEW/usr/lib/libvulkan.so.1" ] || [ ! -d "$NEW/usr/share/vulkan" ]; then
    echo "note: the image has no Vulkan loader: no Vulkan device in this root"
else
    scripts/install-frameroot-vulkan.sh "$NEW" || echo "note: Vulkan not installed in this root"
    scripts/install-frameroot-atomupd.sh "$NEW" || echo "note: atomupd-manager left as the image has it"
fi
{
    echo "made by scripts/mkframeroot.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)"
    echo "from: SteamOS $version_id BUILD_ID=$build_id"
    echo "overlay: etc/resolv.conf etc/localtime(${host_zone:-unchanged}) .lxrt-guest-env vulkan-icd"
} > "$NEW/.lxrt-frameroot"

# Swap in, carrying the runtime state (tmp/, opt/apps) over; the old tree
# then goes (scripts/roots.sh, root_swap).
root_swap "$OUT"
mkdir -p "$OUT/tmp"
if [ -n "$HOME_FROM" ] && [ ! -d "$OUT/tmp/armhome/.local/share/Steam" ]; then
    mkdir -p "$OUT/tmp/armhome"
    rsync -a --exclude '/.local/share/Steam/steamapps/common/' "$HOME_FROM/" "$OUT/tmp/armhome/"
    echo "client home copied from $HOME_FROM"
fi
mkdir -p "$STATE"
if [ ! -e "$STATE/arm64root" ] || [ -L "$STATE/arm64root" ]; then
    ln -sfn "$OUT" "$STATE/arm64root"
fi

# What the client needs from the root, checked (a symlink must resolve).
miss=0
for f in usr/share/X11/locale/locale.dir usr/lib/locale/C.utf8 usr/bin/lsof \
         usr/lib/libnssckbi.so usr/lib/libGLX_mesa.so.0 usr/lib/libGL.so.1 \
         etc/ssl/certs/ca-certificates.crt etc/resolv.conf; do
    if [ -e "$OUT/$f" ]; then echo "  ok       $f"; else echo "  MISSING  $f"; miss=$((miss + 1)); fi
done
if [ -f "$OUT/tmp/armhome/.local/share/Steam/steamrtarm64/steam" ]; then
    echo "  ok       client: tmp/armhome/.local/share/Steam/steamrtarm64/steam"
else
    echo "  note     no client yet in $OUT/tmp/armhome (--home-from DIR)"
fi
echo "root: $OUT (SteamOS $version_id, BUILD_ID $build_id), linked from $STATE/arm64root; ${SECONDS} s"
echo "run:  ARMROOT=$STATE/arm64root ARMROOT_LINK=/tmp/lxrt-arm64root scripts/run-steam-arm64.sh"
[ "$miss" -eq 0 ]
