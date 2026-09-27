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
