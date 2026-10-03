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
OWN="${STEAMARM_KK_OWN:-${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out}"
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
# vk_vtxread: a vertex shader that fetches the target the pass before drew
# into, and one that reads a buffer its fragment shader stored into. SteamARM's
# KosmicKrisp lets the vertex stage of a pass start before the fragments of
# the pass before are shaded (patches/kosmickrisp-05) except in these cases;
# KK_VERTEX_BARRIER=1 is the driver as it was.
exe4="$ROOT/tmp/vktest/vk_vtxread"
if ! "$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
        --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 -I/opt/homebrew/include -o "$exe4" \
        tests/elf/vk_vtxread.c "$ROOT/usr/lib/libvulkan.so.1"; then
    echo "  FAIL  vk_vtxread (build)"; fail=$((fail + 1))
else
    run_vtxread() { # label, then environment assignments
        local label=$1; shift
        local log="$ROOT/tmp/vktest/vk_vtxread_${label// /_}.log"
        env OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES LXRT_ROOT="$ROOT" "$@" \
            perl -e 'alarm 120; exec @ARGV' ./build/lxrun /tmp/vktest/vk_vtxread > "$log" 2>&1
        if [ "$(grep -E '^== vk_vtxread' "$log" | tail -1)" = "== vk_vtxread: ok" ]; then
            printf '  ok    vk_vtxread %s\n' "$label"; pass=$((pass + 1))
        else
            printf '  FAIL  vk_vtxread %s: %s (log %s)\n' "$label" "$(grep -B1 '^== vk_vtxread' "$log" | head -1)" "$log"
            fail=$((fail + 1))
        fi
    }
    run_vtxread moltenvk STEAMARM_VK_ICD=moltenvk
    run_vtxread "kosmickrisp homebrew" STEAMARM_VK_ICD=kosmickrisp
    if [ -f "$OWN/libvulkan_kosmickrisp.dylib" ]; then
        run_vtxread "kosmickrisp steamarm" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN"
        run_vtxread "kosmickrisp steamarm vertex barrier" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN" KK_VERTEX_BARRIER=1
    else
        echo "  skip  vk_vtxread kosmickrisp steamarm (no $OWN: scripts/build-kosmickrisp.sh)"
    fi
fi
# vk_alphamask: cut-out foliage at 4x MSAA (alpha-to-coverage, discard,
# demote, sample masks, depth prepasses), with and without a VkPipelineCache.
# Homebrew's KosmicKrisp takes the coverage from the colour after its
# in-shader blending and write mask, and hands an alpha-to-coverage pipeline
# a cached one without it; SteamARM's (patches/kosmickrisp-08) must not.
exe6="$ROOT/tmp/vktest/vk_alphamask"
if ! "$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
        --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 -I/opt/homebrew/include -o "$exe6" \
        tests/elf/vk_alphamask.c "$ROOT/usr/lib/libvulkan.so.1"; then
    echo "  FAIL  vk_alphamask (build)"; fail=$((fail + 1))
else
    run_alphamask() { # label, cache|"", then environment assignments
        local label=$1 cache=$2; shift 2
        local log="$ROOT/tmp/vktest/vk_alphamask_${label// /_}.log"
        env OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES LXRT_ROOT="$ROOT" "$@" \
            perl -e 'alarm 120; exec @ARGV' ./build/lxrun /tmp/vktest/vk_alphamask "" $cache > "$log" 2>&1
        if [ "$(grep -E '^== vk_alphamask' "$log" | tail -1)" = "== vk_alphamask: ok" ]; then
            printf '  ok    vk_alphamask %s\n' "$label"; pass=$((pass + 1))
        else
            printf '  FAIL  vk_alphamask %s: %s (log %s)\n' "$label" "$(grep -m1 FAIL "$log")" "$log"
            fail=$((fail + 1))
        fi
    }
    run_alphamask moltenvk "" STEAMARM_VK_ICD=moltenvk
    run_alphamask "moltenvk cache" cache STEAMARM_VK_ICD=moltenvk
    if [ -f "$OWN/libvulkan_kosmickrisp.dylib" ]; then
        run_alphamask "kosmickrisp steamarm" "" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN"
        run_alphamask "kosmickrisp steamarm cache" cache STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN"
    else
        echo "  skip  vk_alphamask kosmickrisp steamarm (no $OWN: scripts/build-kosmickrisp.sh)"
    fi
fi
# vk_resubmit: the same command buffer submitted three times with other work
# in between, and secondaries: KosmicKrisp records it again at every submit
# and, since patches/kosmickrisp-12, returns the memory a recording uploads
# into to a pool another command buffer takes from. Also with each render
# pass in its own Metal command buffer (KK_CMDBUF_PER_PASS=1).
exe8b="$ROOT/tmp/vktest/vk_resubmit"
if ! "$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
        --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 -I/opt/homebrew/include -Itests/elf -o "$exe8b" \
        tests/elf/vk_resubmit.c "$ROOT/usr/lib/libvulkan.so.1"; then
    echo "  FAIL  vk_resubmit (build)"; fail=$((fail + 1))
else
    run_resubmit() { # label, then environment assignments
        local label=$1; shift
        local log="$ROOT/tmp/vktest/vk_resubmit_${label// /_}.log"
        env OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES LXRT_ROOT="$ROOT" "$@" \
            perl -e 'alarm 120; exec @ARGV' ./build/lxrun /tmp/vktest/vk_resubmit > "$log" 2>&1
        if [ "$(grep -E '^== vk_resubmit' "$log" | tail -1)" = "== vk_resubmit: ok" ]; then
            printf '  ok    vk_resubmit %s\n' "$label"; pass=$((pass + 1))
        else
            printf '  FAIL  vk_resubmit %s: %s (log %s)\n' "$label" "$(grep -B1 '^== vk_resubmit' "$log" | head -1)" "$log"
            fail=$((fail + 1))
        fi
    }
    run_resubmit moltenvk STEAMARM_VK_ICD=moltenvk
    if [ -f "$OWN/libvulkan_kosmickrisp.dylib" ]; then
        run_resubmit "kosmickrisp steamarm" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN"
        run_resubmit "kosmickrisp steamarm per pass" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN" KK_CMDBUF_PER_PASS=1
    fi
fi
# vk_vsonly: a depth prepass made of pipelines with no fragment shader,
# through a VkPipelineCache: KosmicKrisp handed one the other's depth state
# (patches/kosmickrisp-09). Homebrew's driver fails it with the cache.
exe7="$ROOT/tmp/vktest/vk_vsonly"
if ! "$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
        --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 -I/opt/homebrew/include -Itests/elf -o "$exe7" \
        tests/elf/vk_vsonly.c "$ROOT/usr/lib/libvulkan.so.1"; then
    echo "  FAIL  vk_vsonly (build)"; fail=$((fail + 1))
else
    run_vsonly() { # label, cache argument, then environment assignments
        local label=$1 cache=$2; shift 2
        local log="$ROOT/tmp/vktest/vk_vsonly_${label// /_}.log"
        env OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES LXRT_ROOT="$ROOT" "$@" \
            perl -e 'alarm 120; exec @ARGV' ./build/lxrun /tmp/vktest/vk_vsonly $cache > "$log" 2>&1
        if [ "$(grep -E '^== vk_vsonly' "$log" | tail -1)" = "== vk_vsonly: ok" ]; then
            printf '  ok    vk_vsonly %s\n' "$label"; pass=$((pass + 1))
        else
            printf '  FAIL  vk_vsonly %s: %s (log %s)\n' "$label" "$(grep -m1 FAIL "$log")" "$log"; fail=$((fail + 1))
        fi
    }
    run_vsonly "moltenvk cache" cache STEAMARM_VK_ICD=moltenvk
    if [ -f "$OWN/libvulkan_kosmickrisp.dylib" ]; then
        run_vsonly "kosmickrisp steamarm" "" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN"
        run_vsonly "kosmickrisp steamarm cache" cache STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN"
    fi
fi
# vk_metalfx: MetalFX between two Vulkan submissions, as the shim's scaler
# encodes it (shim/scaler.c, runtime/metalfx.m), without a window: the
# driver's Metal objects (VK_EXT_metal_objects; SteamARM's KosmicKrisp from
# patches/kosmickrisp-10 on), Apple's spatial scaler on the driver's queue
# (MoltenVK) or the runtime's own (KosmicKrisp), the pixels read back.
# Homebrew's KosmicKrisp has no such extension: the program says skip.
exe8="$ROOT/tmp/vktest/vk_metalfx"
if ! "$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
        --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 -I/opt/homebrew/include -Iruntime/include -o "$exe8" \
        tests/elf/vk_metalfx.c "$ROOT/usr/lib/libvulkan.so.1"; then
    echo "  FAIL  vk_metalfx (build)"; fail=$((fail + 1))
else
    run_metalfx() { # label, then environment assignments
        local label=$1; shift
        local log="$ROOT/tmp/vktest/vk_metalfx_${label// /_}.log"
        env OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES LXRT_ROOT="$ROOT" "$@" \
            perl -e 'alarm 60; exec @ARGV' ./build/lxrun /tmp/vktest/vk_metalfx > "$log" 2>&1
        if [ "$(grep -E '^== vk_metalfx' "$log" | tail -1)" = "== vk_metalfx: ok" ]; then
            printf '  ok    vk_metalfx %s (%s)\n' "$label" "$(grep -o 'MetalFX [0-9.]* ms' "$log")"; pass=$((pass + 1))
        else
            printf '  FAIL  vk_metalfx %s: %s (log %s)\n' "$label" "$(grep -B1 '^== vk_metalfx' "$log" | head -1)" "$log"
            fail=$((fail + 1))
        fi
    }
    run_metalfx moltenvk STEAMARM_VK_ICD=moltenvk
    run_metalfx "moltenvk temporal" STEAMARM_VK_ICD=moltenvk VK_METALFX_TEMPORAL=1
    if [ -f "$OWN/libvulkan_kosmickrisp.dylib" ]; then
        run_metalfx "kosmickrisp steamarm" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN"
        run_metalfx "kosmickrisp steamarm temporal" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN" VK_METALFX_TEMPORAL=1
    fi
fi
# vk_fsr: a game's FSR 1 upscale (EASU) replaced by MetalFX inside the driver
# (patches/kosmickrisp-14): a shader with EASU's constants that paints
# magenta; SteamARM's KosmicKrisp must give the input enlarged instead,
# MoltenVK and KK_FSR_METALFX=0 the shader's own magenta.
exe9="$ROOT/tmp/vktest/vk_fsr"
if ! "$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
        --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 -I/opt/homebrew/include -o "$exe9" \
        tests/elf/vk_fsr.c "$ROOT/usr/lib/libvulkan.so.1"; then
    echo "  FAIL  vk_fsr (build)"; fail=$((fail + 1))
else
    run_fsr() { # label, expected result (metalfx|shader), then environment assignments
        local label=$1 want=$2; shift 2
        local log="$ROOT/tmp/vktest/vk_fsr_${label// /_}.log"
        env OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES LXRT_ROOT="$ROOT" "$@" \
            perl -e 'alarm 60; exec @ARGV' ./build/lxrun /tmp/vktest/vk_fsr "$want" > "$log" 2>&1
        if [ "$(grep -E '^== vk_fsr' "$log" | tail -1)" = "== vk_fsr: ok" ]; then
            printf '  ok    vk_fsr %s (%s)\n' "$label" "$(grep -m1 '^result:' "$log" | cut -d' ' -f2)"; pass=$((pass + 1))
        else
            printf '  FAIL  vk_fsr %s: %s (log %s)\n' "$label" "$(grep -m1 '^result:' "$log")" "$log"
            fail=$((fail + 1))
        fi
    }
    run_fsr moltenvk shader STEAMARM_VK_ICD=moltenvk
    if [ -f "$OWN/libvulkan_kosmickrisp.dylib" ]; then
        run_fsr "kosmickrisp steamarm" metalfx STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN"
        run_fsr "kosmickrisp steamarm off" shader STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN" KK_FSR_METALFX=0
    fi
fi
# vk_x11_present: a swapchain on an X window of SteamARM's X server. With
# IMMEDIATE the shim's mailbox (shim/mailbox.c) must not wait for the display
# (the layer is shown by another process and takes one drawable per refresh:
# 165 fps on a 165 Hz display before); with FIFO it must.
exe3="$ROOT/tmp/vktest/vk_x11_present"
XDISP="${DISPLAY:-:2}"
if ! DISPLAY=$XDISP xdpyinfo >/dev/null 2>&1; then
    echo "  skip  vk_x11_present (no X server on $XDISP)"
elif ! "$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
        --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 -I/opt/homebrew/include -o "$exe3" \
        tests/elf/vk_x11_present.c "$ROOT/usr/lib/libvulkan.so.1" "$(ls "$ROOT"/usr/lib/libxcb.so.1* | head -1)"; then
    echo "  FAIL  vk_x11_present (build)"; fail=$((fail + 1))
else
    present() { # driver, mode letter, what the median frame time must be: "below N" or "above N" (ms)
        local log="$ROOT/tmp/vktest/vk_x11_present_$1_$2.log" med
        OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES STEAMARM_VK_ICD=$1 LXRT_ROOT="$ROOT" DISPLAY=$XDISP \
            perl -e 'alarm 90; exec @ARGV' ./build/lxrun /tmp/vktest/vk_x11_present 900 "$2" > "$log" 2>&1
        med=$(sed -n 's/.*mediana \([0-9.]*\) ms.*/\1/p' "$log" | tail -1)
        if grep -q '^== vk x11 surface: ok' "$log" && [ -n "$med" ] &&
           awk -v m="$med" -v how="$3" -v lim="$4" 'BEGIN { exit !((how == "below" && m < lim) || (how == "above" && m > lim)) }'; then
            printf '  ok    vk_x11_present %-11s %s: median %s ms\n' "$1" "$([ "$2" = i ] && echo IMMEDIATE || echo FIFO)" "$med"
            pass=$((pass + 1))
        else
            printf '  FAIL  vk_x11_present %s %s: median "%s" ms, wanted %s %s (log %s)\n' "$1" "$2" "$med" "$3" "$4" "$log"
            fail=$((fail + 1))
        fi
    }
    present kosmickrisp i below 3
    present kosmickrisp f above 3
    present moltenvk f above 3
    # vk_x11_modes: the swapchain made again and again (a game changing its
    # video settings): fullscreen on and off, borderless, resizes. Homebrew's
    # KosmicKrisp keeps a retain on every drawable and stops after the second
    # swapchain (patches/kosmickrisp-07); SteamARM's and MoltenVK must not.
    exe5="$ROOT/tmp/vktest/vk_x11_modes"
    if ! "$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
            --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 -I/opt/homebrew/include -o "$exe5" \
            tests/elf/vk_x11_modes.c "$ROOT/usr/lib/libvulkan.so.1" "$(ls "$ROOT"/usr/lib/libxcb.so.1* | head -1)"; then
        echo "  FAIL  vk_x11_modes (build)"; fail=$((fail + 1))
    else
        modes() { # label, then environment assignments
            local label=$1; shift
            local log="$ROOT/tmp/vktest/vk_x11_modes_${label// /_}.log"
            env OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES LXRT_ROOT="$ROOT" DISPLAY=$XDISP "$@" \
                perl -e 'alarm 150; exec @ARGV' ./build/lxrun /tmp/vktest/vk_x11_modes 60 wrssfubd > "$log" 2>&1
            if grep -q '^== vk x11 modes: ok' "$log"; then
                printf '  ok    vk_x11_modes %s\n' "$label"; pass=$((pass + 1))
            else
                printf '  FAIL  vk_x11_modes %s: %s (log %s)\n' "$label" "$(grep -c 'picture stopped' "$log") phases stopped" "$log"
                fail=$((fail + 1))
            fi
        }
        modes moltenvk STEAMARM_VK_ICD=moltenvk
        if [ -f "$OWN/libvulkan_kosmickrisp.dylib" ]; then
            modes "kosmickrisp steamarm" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN"
        fi
    fi
    # vk_twodev: a device made and destroyed, then the one that draws, whose
    # first acquire comes before any present. The mailbox signalled that
    # acquire on the destroyed device's queue (Steam's web helper crashed in
    # vk_queue_submit_alloc 13-17 s into most starts). MallocScribble makes the
    # freed device unmistakable.
    exe9="$ROOT/tmp/vktest/vk_twodev"
    if ! "$CLANG" --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" --gcc-install-dir="$GCCDIR" -fuse-ld=lld \
            --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w -O2 -I/opt/homebrew/include -o "$exe9" \
            tests/elf/vk_twodev.c "$ROOT/usr/lib/libvulkan.so.1" "$(ls "$ROOT"/usr/lib/libxcb.so.1* | head -1)"; then
        echo "  FAIL  vk_twodev (build)"; fail=$((fail + 1))
    else
        twodev() { # label, then environment assignments
            local label=$1; shift
            local log="$ROOT/tmp/vktest/vk_twodev_${label// /_}.log"
            env OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES LXRT_ROOT="$ROOT" DISPLAY=$XDISP MallocScribble=1 "$@" \
                perl -e 'alarm 60; exec @ARGV' ./build/lxrun /tmp/vktest/vk_twodev > "$log" 2>&1
            if grep -q '^== vk twodev: ok' "$log"; then
                printf '  ok    vk_twodev %s\n' "$label"; pass=$((pass + 1))
            else
                printf '  FAIL  vk_twodev %s: %s (log %s)\n' "$label" "$(grep -a -m1 'FALLO\|SIGSEGV\|SIGBUS' "$log" | cut -c1-80)" "$log"
                fail=$((fail + 1))
            fi
        }
        twodev moltenvk STEAMARM_VK_ICD=moltenvk
        if [ -f "$OWN/libvulkan_kosmickrisp.dylib" ]; then
            twodev "kosmickrisp steamarm" STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$OWN"
        fi
    fi
fi
echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
