#!/bin/bash
# SteamARM KosmicKrisp's MetalFX temporal upscale
# (patches/kosmickrisp-20-metalfx-temporal-entry.patch), on the host:
#   mfxt_bridge.m   the bridge's encode (bridge/mtl_metalfx.m compiled in),
#                   with the output written directly and through the private
#                   copy (KK_MFXT_SCRATCH=1);
#   vk_mfxt_entry.c kk_steamarm_upscale_temporal in the built driver, called
#                   as SteamARM's Vulkan shim calls it;
#   vk_geometry.c   geometry shaders (patches/kosmickrisp-27), the same
#                   scenes drawn with llvmpipe and compared.
#
# Usage: tests/kk/run.sh
#   STEAMARM_KK_SRC  patched Mesa tree (default
#                    ${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/mesa-26.2.4)
#   STEAMARM_KK_OWN  directory of the built libvulkan_kosmickrisp.dylib
#                    (default ${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out-test)
set -u
cd "$(dirname "$0")/../.." || exit 1
B="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
SRC="${STEAMARM_KK_SRC:-$B/mesa-kk/mesa-26.2.4}"
OWN="${STEAMARM_KK_OWN:-$B/mesa-kk/out-test}"
BRIDGE="$SRC/src/kosmickrisp/bridge"
OUT="${TMPDIR:-/tmp}/steamarm-kk-tests"
mkdir -p "$OUT"
pass=0 fail=0
result() { # name, log
    if [ "$(grep -E "^== $1" "$2" | tail -1)" = "== $1: ok" ]; then
        printf '  ok    %s\n' "$3"; pass=$((pass + 1))
    else
        printf '  FAIL  %s (log %s)\n' "$3" "$2"; fail=$((fail + 1))
    fi
}

if ! grep -q mtl_metalfx_temporal_encode "$BRIDGE/mtl_metalfx.m" 2>/dev/null; then
    echo "  FAIL  mfxt_bridge: no temporal encode in $BRIDGE/mtl_metalfx.m (patch 20 not applied?)"; fail=$((fail + 1))
elif ! clang -fno-objc-arc -O2 -Wall -Wno-unused-function -Wno-unguarded-availability-new \
        -I"$BRIDGE" tests/kk/mfxt_bridge.m -framework Metal -framework MetalFX -framework Foundation \
        -o "$OUT/mfxt_bridge"; then
    echo "  FAIL  mfxt_bridge (build)"; fail=$((fail + 1))
else
    for scratch in 0 1; do
        log="$OUT/mfxt_bridge_scratch$scratch.log"
        KK_MFXT_SCRATCH=$scratch perl -e 'alarm 300; exec @ARGV' "$OUT/mfxt_bridge" > "$log" 2>&1
        grep -E '^  (ok|FAIL)|^    ' "$log" | sed 's/^/    /'
        result mfxt_bridge "$log" "mfxt_bridge KK_MFXT_SCRATCH=$scratch"
    done
fi

if ! clang -O2 -Wall -I/opt/homebrew/include tests/kk/vk_mfxt_entry.c -o "$OUT/vk_mfxt_entry"; then
    echo "  FAIL  vk_mfxt_entry (build)"; fail=$((fail + 1))
else
    for scratch in 0 1; do
        log="$OUT/vk_mfxt_entry_scratch$scratch.log"
        KK_MFXT_SCRATCH=$scratch STEAMARM_KK_OWN="$OWN" perl -e 'alarm 300; exec @ARGV' "$OUT/vk_mfxt_entry" > "$log" 2>&1
        grep -E '^  (ok|FAIL)' "$log" | sed 's/^/    /'
        result vk_mfxt_entry "$log" "vk_mfxt_entry KK_MFXT_SCRATCH=$scratch"
    done
fi
LVP="${STEAMARM_LVP:-$(ls /opt/homebrew/opt/mesa/lib/libvulkan_lvp.dylib 2>/dev/null)}"
if ! clang -O2 -Wall -I/opt/homebrew/include tests/kk/vk_geometry.c -o "$OUT/vk_geometry"; then
    echo "  FAIL  vk_geometry (build)"; fail=$((fail + 1))
elif [ -z "$LVP" ]; then
    echo "  skip  vk_geometry (no llvmpipe: brew install mesa, or STEAMARM_LVP)"
else
    log="$OUT/vk_geometry.log"
    perl -e 'alarm 300; exec @ARGV' "$OUT/vk_geometry" "$OWN/libvulkan_kosmickrisp.dylib" "$LVP" > "$log" 2>&1
    grep -E '^  (ok|FAIL|skip)' "$log" | sed 's/^/    /'
    result vk_geometry "$log" "vk_geometry (geometry shaders against llvmpipe)"
fi
echo "== $pass passed, $fail failed"
[ "$fail" = 0 ]
