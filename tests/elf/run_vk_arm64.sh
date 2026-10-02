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
echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
