#!/bin/bash
# A Wayland compositor on the Mac without a VM: Weston (Fedora 43 aarch64
# packages, no emulation) run by lxrun with its X11 backend, so its output is
# an X window on SteamARM's XQuartz server (:2), that is, a macOS window, and
# its Wayland socket serves native aarch64 clients and x86-64 ones under FEX.
# Waydroid's hwcomposer (Android's display HAL) is a Wayland client:
# docs/ANDROID_RUNTIME_ARCHITECTURE.md, "Display";
# benchmarks/stage27-android-display.txt.
#
# A root of its own, beside the Steam client's (scripts/mkarmroot.sh), so the
# client's root and lock are not re-resolved for it. The package set is the
# shared-library closure of the seeds, resolved by scripts/mkroot-rpm.sh into
# its own lock (scripts/mkwestonroot.lock) and stage: a rebuild reproduces it
# package for package.
#
#   scripts/mkwestonroot.sh [out-dir]      default $STEAMARM_STATE/westonroot
#   scripts/mkwestonroot.sh --stage-only   refresh the RPM lock/stage only
# A root a guest is running on is not rebuilt. Its tmp/ is kept across a
# rebuild (scripts/roots.sh, root_swap).
set -euo pipefail
CANON_BASE=$PWD
cd "$(dirname "$0")/.." || exit 1
. scripts/roots.sh
BUILD="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
OUT="${1:-$STATE/westonroot}"
STAGE_ONLY=0
if [ "${1:-}" = --stage-only ]; then STAGE_ONLY=1; OUT="${2:-$STATE/westonroot}"; fi
# weston            the compositor, weston-terminal, weston-screenshooter;
#                   weston-libs (pulled by soname) has the x11 backend and
#                   the pixman and GL renderers
# weston-demo       weston-simple-shm, weston-simple-damage, weston-flower,
#                   weston-presentation-shm ... (wl_shm clients)
# wayland-utils     wayland-info: the globals and protocol versions offered
# xkeyboard-config  the keymap weston compiles and hands its clients
# xwd, xwininfo, xdpyinfo  to read the X window weston draws into back
# fontconfig, dejavu  weston-terminal's and the desktop shell's text
SEEDS="glibc libgcc libstdc++ bash coreutils-single weston weston-demo wayland-utils
 xkeyboard-config xwd xwininfo xdpyinfo fontconfig dejavu-sans-fonts dejavu-sans-mono-fonts
 glibc-common glibc-langpack-en"
SEEDS=$(echo $SEEDS)                  # one line: mkroot-rpm reads a single line
STAGE="$BUILD/westonstage-f43"
LOCK="$PWD/scripts/mkwestonroot.lock"
RELOCK=
if [ ! -f "$LOCK" ] || [ "$(sed -n 's/^# root seeds: \(.*\) + their shared-library closure$/\1/p' "$LOCK")" != "$SEEDS" ]; then
    RELOCK=--relock
fi
if [ -z "${MKWESTONROOT_SKIP_RPM:-}" ]; then
    MKROOT_SEEDS="$SEEDS" MKROOT_LOCK="$LOCK" MKROOT_STAGE="$STAGE" \
        MKROOT_STAGE_ONLY=1 scripts/mkroot-rpm.sh $RELOCK "$BUILD/westonroot-unused"
fi
if [ "$STAGE_ONLY" -eq 1 ]; then echo "westonroot stage ready: $STAGE"; exit 0; fi
refuse() { echo "refusing: $*" >&2; exit 1; }
FINAL=$(canon_path "$OUT") ||
    refuse "$OUT: not a usable path (dangling symlink, a symlink to a file, or . / .. in a part that does not exist)"
STAGE_REAL=$(canon_path "$STAGE") && [ -d "$STAGE_REAL" ] || refuse "no stage at $STAGE"
[ "$FINAL" != / ] || refuse "$OUT is /"
[ ! -e "$FINAL" ] || [ -d "$FINAL" ] || refuse "$OUT is not a directory"
for spared in "$STAGE_REAL" "$BUILD" "$STATE" "$PWD" "$HOME"; do
    s=$(canon_path "$spared") || continue
    for victim in "$FINAL" "$FINAL.new" "$FINAL.old"; do
        if path_within "$s" "$victim"; then refuse "$OUT: $victim would contain $spared"; fi
    done
done
if path_within "$FINAL" "$STAGE_REAL"; then refuse "$OUT is inside the stage $STAGE"; fi
if [ -e "$FINAL" ] && [ ! -f "$FINAL/.lxrt-westonroot" ]; then
    root_recover "$FINAL" .lxrt-westonroot
    refuse "$OUT exists and was not made by this script"
fi
in_use=$(guests_on_root "$FINAL")
[ -z "$in_use" ] || refuse "guests are running on $FINAL (pid $(echo $in_use)); stop them first"
mkdir -p "$(dirname "$FINAL")"
root_recover "$FINAL" .lxrt-westonroot
root_clear "$FINAL"
OUT="$FINAL.new"
cp -Rc "$STAGE_REAL" "$OUT"
touch "$OUT/.lxrt-westonroot"
mkdir -p "$OUT/etc" "$OUT/tmp" "$OUT/dev/shm" "$OUT/run"
[ -e "$OUT/etc/machine-id" ] || uuidgen | tr -d '-' | tr 'A-Z' 'a-z' > "$OUT/etc/machine-id"
# fontconfig's cache is a scriptlet (fc-cache) on Fedora; without it every
# client scans the fonts once, which is only slower.
root_swap "$FINAL"
OUT="$FINAL"
echo "westonroot ready: $OUT ($(du -sh "$OUT" | cut -f1))"
