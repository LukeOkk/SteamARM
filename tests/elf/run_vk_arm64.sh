#!/bin/bash
# tests/elf/vk_pipeline.c as a native aarch64 program on the Vulkan shim, on
# MoltenVK and KosmicKrisp: a graphics pipeline compiled (Metal's shader
# compiler, an XPC service) in the program itself, in a fork+exec child, and
# in a fork child without exec -- the way Chromium's unsandboxed zygote
# starts Steam's GPU process, which could not reach MTLCompilerService until
# runtime/process.c (lxrt_metal_attach) registered it again.
#
# Usage: tests/elf/run_vk_arm64.sh [arm64-root]   (default /tmp/lxrt-arm64root)
# Needs the Frame root's shim (/usr/lib/libvulkan.so.1) and the aarch64
# sysroot scripts/mkframeroot.sh stages.
set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT="${1:-/tmp/lxrt-arm64root}"
STAGE="${STEAMARM_BUILD:-$HOME/SteamARM-build}/rootstage-f43"
CLANG=/opt/homebrew/opt/llvm/bin/clang
GCCDIR=$(ls -d "$STAGE"/usr/lib/gcc/aarch64-redhat-linux/* 2>/dev/null | tail -1)
mkdir -p "$ROOT/tmp/vktest"
exe="$ROOT/tmp/vktest/vk_pipeline"
if ! "$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
        --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 -I/opt/homebrew/include -o "$exe" \
        tests/elf/vk_pipeline.c "$ROOT/usr/lib/libvulkan.so.1"; then
    echo "  FAIL  vk_pipeline (build)"; exit 1
fi
pass=0 fail=0
for mode in plain fork zygote; do
    for icd in moltenvk kosmickrisp; do
        arg=$mode; [ "$mode" = plain ] && arg=
        log="$ROOT/tmp/vktest/vk_pipeline_${mode}_$icd.log"
        # OBJC_DISABLE_INITIALIZE_FORK_SAFETY: what the runtime gives every
        # process it execs (runtime/process.c); this first one is started here.
        OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES STEAMARM_VK_ICD=$icd LXRT_ROOT="$ROOT" \
            perl -e 'alarm 60; exec @ARGV' ./build/lxrun /tmp/vktest/vk_pipeline $arg > "$log" 2>&1
        if [ "$(grep -E '^== vk_pipeline' "$log" | tail -1)" = "== vk_pipeline: ok" ]; then
            printf '  ok    vk_pipeline %-6s %s\n' "$mode" "$icd"; pass=$((pass + 1))
        else
            printf '  FAIL  vk_pipeline %-6s %s (log %s)\n' "$mode" "$icd" "$log"; fail=$((fail + 1))
        fi
    done
done
# vk_passes: consecutive render passes on the same attachments, a clear
# inside one, a return to a target after another one, and a copy out. On
# MoltenVK, on Homebrew's KosmicKrisp, and on SteamARM's own KosmicKrisp
# (scripts/build-kosmickrisp.sh: patches/kosmickrisp-04 draws them in one
# Metal encoder) with the merging on and off.
exe2="$ROOT/tmp/vktest/vk_passes"
OWN="${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out"
if ! "$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
        --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 -I/opt/homebrew/include -o "$exe2" \
        tests/elf/vk_passes.c "$ROOT/usr/lib/libvulkan.so.1"; then
    echo "  FAIL  vk_passes (build)"; fail=$((fail + 1))
else
    run_passes() { # label, then environment assignments
        local label=$1; shift
        local log="$ROOT/tmp/vktest/vk_passes_${label// /_}.log"
        env OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES LXRT_ROOT="$ROOT" "$@" \
            perl -e 'alarm 60; exec @ARGV' ./build/lxrun /tmp/vktest/vk_passes > "$log" 2>&1
        if [ "$(grep -E '^== vk_passes' "$log" | tail -1)" = "== vk_passes: ok" ]; then
            printf '  ok    vk_passes %s\n' "$label"; pass=$((pass + 1))
        else
            printf '  FAIL  vk_passes %s: %s (log %s)\n' "$label" "$(grep -B1 '^== vk_passes' "$log" | head -1)" "$log"
            fail=$((fail + 1))
        fi
    }
    run_passes moltenvk STEAMARM_VK_ICD=moltenvk
    run_passes "kosmickrisp homebrew" STEAMARM_VK_ICD=kosmickrisp
    if [ -f "$OWN/libvulkan_kosmickrisp.dylib" ]; then
        run_passes "kosmickrisp steamarm" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN"
        run_passes "kosmickrisp steamarm unmerged" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN" KK_MERGE_PASSES=0
        run_passes "kosmickrisp steamarm old load-store" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN" KK_APP_LOAD_STORE=0
    else
        echo "  skip  vk_passes kosmickrisp steamarm (no $OWN: scripts/build-kosmickrisp.sh)"
    fi
fi
echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
