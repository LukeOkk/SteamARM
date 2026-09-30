#!/bin/bash
# macOS empties /tmp on every boot. The guest roots live in a persistent
# directory and are reached through /tmp links (guest-visible paths such as
# /tmp/fexhome are baked into Steam's config and symlinks).
#
#   scripts/env-links.sh [roots-dir]     (default ~/SteamARM-roots)
set -eu
R="${1:-${STEAMARM_STATE:-$HOME/SteamARM-roots}}"
# ln -sfn into a real directory puts the link inside it. A /tmp/lxrt-* that
# is a directory holding only tmp/pulse was made by scripts/audio.sh while
# its root was missing (run-app.sh now refuses that); take it away.
for l in /tmp/lxrt-root /tmp/lxrt-steamroot /tmp/lxrt-samples /tmp/lxrt-arm64root /tmp/lxrt-armroot; do
    if [ -d "$l" ] && [ ! -L "$l" ]; then
        if [ -z "$(find "$l" -mindepth 1 ! -path "$l/tmp" ! -path "$l/tmp/pulse" ! -path "$l/tmp/pulse/*" -print -quit)" ]; then
            rm -rf "$l"
        else
            echo "env-links: $l is a directory, not a link; left alone" >&2
        fi
    fi
done
ln -sfn "$R/lxrt-root" /tmp/lxrt-root
ln -sfn "$R/steamroot" /tmp/lxrt-steamroot
ln -sfn "$R/samples"   /tmp/lxrt-samples
ls -la /tmp/lxrt-root /tmp/lxrt-steamroot /tmp/lxrt-samples
# The ARM64 base (docs/STEAM_FRAME_IMAGE.md): $R/arm64root points at the root
# derived from the Steam Frame image. Linked only while it is reachable (it can
# live on a sparsebundle that is not attached); scripts/run-native.sh needs it.
if [ -e "$R/arm64root" ]; then
    ln -sfn "$R/arm64root" /tmp/lxrt-arm64root
    ls -la /tmp/lxrt-arm64root
fi
# The ARM64 root of scripts/mkarmroot.sh (Fedora 43 aarch64): the native arm64
# Steam client lives in it (benchmarks/stage21). Linked only when it exists.
if [ -e "$R/armroot" ]; then
    ln -sfn "$R/armroot" /tmp/lxrt-armroot
    ls -la /tmp/lxrt-armroot
fi
