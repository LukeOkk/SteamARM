#!/bin/bash
# Pixel correctness and bounded-cache verification; timings are not game FPS.
# Retain each run's binary and logs. Does not install or replace any driver.
# STEAMARM_KK_OWN selects a driver built with patch 22.
set -eu
cd "$(dirname "$0")/../.."
ROOT="${1:-/tmp/lxrt-arm64root}"
STAGE="${STEAMARM_BUILD:-$HOME/SteamARM-build}/rootstage-f43"
OWN="${STEAMARM_KK_OWN:-${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out}"
CLANG=/opt/homebrew/opt/llvm/bin/clang
GCCDIR=$(ls -d "$STAGE"/usr/lib/gcc/aarch64-redhat-linux/* | tail -1)
mkdir -p "$ROOT/tmp"
OUT=$(mktemp -d "$ROOT/tmp/vk-depth.XXXXXX")
GUEST="/tmp/${OUT##*/}/vk_dynamic_depth"
"$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" \
    --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
    --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 \
    -I/opt/homebrew/include -Itests/elf -o "$OUT/vk_dynamic_depth" \
    tests/elf/vk_dynamic_depth.c "$ROOT/usr/lib/libvulkan.so.1"

run() {
    local label=$1; shift
    local log="$OUT/$label.log"
    if ! env LXRT_ROOT="$ROOT" "$@" perl -e 'alarm 60; exec @ARGV' \
        scripts/run-native.sh "$GUEST" > "$log" 2>&1; then
        echo "FAIL $label; log: $log"; return 1
    fi
    if ! rg -q '^== vk_dynamic_depth: ok$' "$log"; then
        echo "FAIL $label; log: $log"; return 1
    fi
    rg '^(dynamic depth:|== vk_dynamic_depth:)|depth state cache:' "$log"
}
run moltenvk STEAMARM_VK_ICD=moltenvk
if [ ! -f "$OWN/libvulkan_kosmickrisp.dylib" ]; then
    echo "No selected KosmicKrisp driver: $OWN"; exit 1
fi
run kosmickrisp-off STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN" KK_DS_CACHE=0 KK_DS_CACHE_LOG=0
run kosmickrisp-on STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN" KK_DS_CACHE=1 KK_DS_CACHE_LOG=1
if ! rg -q 'depth state cache: entries=64 hits=[1-9][0-9]* misses=[1-9][0-9]* full=[1-9][0-9]*' "$OUT/kosmickrisp-on.log"; then
    echo "FAIL: selected driver did not prove cache hits and capacity fallback; build patch 22."
    exit 1
fi
echo "PASS; retained artifacts: $OUT"
