#!/bin/bash
# macOS empties /tmp on every boot. The guest roots live in a persistent
# directory and are reached through /tmp links (guest-visible paths such as
# /tmp/fexhome are baked into Steam's config and symlinks).
#
#   scripts/env-links.sh [roots-dir]     (default ~/SteamARM-roots)
set -eu
R="${1:-${STEAMARM_STATE:-$HOME/SteamARM-roots}}"
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
