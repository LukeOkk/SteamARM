#!/bin/bash
# tests/elf/vk_device.c through FEX's Vulkan thunks, as an x86-64 and as an
# i386 program: device, queue, mapped memory, command buffers, a submit and a
# GPU fill read back. The i386 build is the 32-bit thunk path (handles,
# pointers and mapped memory of a guest living at FEX's guest base). Then
# OpenGL through Zink on the same thunk (tests/elf/gl_zink.c), both arches.
#
# Usage: tests/elf/run_vk_device.sh [steam-root]   (default /tmp/lxrt-steamroot)
# Needs the graphics stack in the root (scripts/install-steamroot-gfx.sh) and
# the thunk build's sysroots (scripts/build-fex-thunks.sh x86root x86root32).
set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT="${1:-/tmp/lxrt-steamroot}"
OUT="${STEAMARM_BUILD:-$HOME/SteamARM-build}/thunks-build"
CLANG=/opt/homebrew/opt/llvm/bin/clang
pass=0 fail=0
for arch in x86_64 i386; do
    case $arch in
        x86_64) flags=(--target=x86_64-linux-gnu --sysroot="$OUT/sysroot-x86_64") guest="$OUT/GuestThunks/libvulkan-guest.so" ;;
        i386)   flags=(-m32 --target=i686-linux-gnu --sysroot="$OUT/sysroot-i386") guest="$OUT/GuestThunks_32/libvulkan-guest.so" ;;
    esac
    exe="$ROOT/tmp/vk_device_$arch"
    if ! "$CLANG" "${flags[@]}" -fuse-ld=lld -O2 -I/opt/homebrew/include -o "$exe" tests/elf/vk_device.c "$guest"; then
        printf '  FAIL  vk_device %-6s (build)\n' "$arch"; fail=$((fail + 1)); continue
    fi
    LXRT_ROOT="$ROOT" FEX_ROOTFS=/ scripts/run-fex.sh "/tmp/vk_device_$arch" > "$ROOT/tmp/vk_device_$arch.log" 2>&1 &
    pid=$!
    for _ in $(seq 1 60); do kill -0 $pid 2>/dev/null || break; sleep 1; done
    kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
    v=$(grep -E '^== vk_device' "$ROOT/tmp/vk_device_$arch.log" | tail -1)
    if [ "$v" = "== vk_device: ok" ]; then
        printf '  ok    vk_device %-6s %s\n' "$arch" "$(grep -E '^readback' "$ROOT/tmp/vk_device_$arch.log")"; pass=$((pass + 1))
    else
        printf '  FAIL  vk_device %-6s (log %s)\n' "$arch" "$ROOT/tmp/vk_device_$arch.log"; fail=$((fail + 1))
    fi
    # OpenGL on the same thunk through Mesa's Zink (tests/elf/gl_zink.c):
    # surfaceless EGL, a triangle into a framebuffer object, pixels read back.
    # MoltenVK: KosmicKrisp 26.2.3 fails to compile one of Zink's shaders to
    # MSL (benchmarks/stage38-opengl-zink.txt).
    gexe="$ROOT/tmp/gl_zink_$arch"
    if ! "$CLANG" "${flags[@]}" -fuse-ld=lld -O2 -o "$gexe" tests/elf/gl_zink.c -ldl; then
        printf '  FAIL  gl_zink   %-6s (build)\n' "$arch"; fail=$((fail + 1)); continue
    fi
    # With the two extensions scripts/settings-env.py announces for WineD3D
    # (OpenGL 4.5 instead of 3.2; stage 39).
    LXRT_ROOT="$ROOT" FEX_ROOTFS=/ STEAMARM_VK_ICD=moltenvk EGL_PLATFORM=surfaceless GALLIUM_DRIVER=zink \
        MESA_EXTENSION_OVERRIDE="+GL_ARB_vertex_type_2_10_10_10_rev +GL_ARB_texture_buffer_object_rgb32" \
        scripts/run-fex.sh "/tmp/gl_zink_$arch" > "$ROOT/tmp/gl_zink_$arch.log" 2>&1 &
    pid=$!
    for _ in $(seq 1 60); do kill -0 $pid 2>/dev/null || break; sleep 1; done
    kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null
    if grep -q '^== gl_zink: ok' "$ROOT/tmp/gl_zink_$arch.log" && grep -q '^renderer: zink' "$ROOT/tmp/gl_zink_$arch.log" &&
       grep -q '^version:  4\.5' "$ROOT/tmp/gl_zink_$arch.log"; then
        printf '  ok    gl_zink   %-6s OpenGL 4.5, %s\n' "$arch" "$(grep -E '^readback' "$ROOT/tmp/gl_zink_$arch.log")"; pass=$((pass + 1))
    else
        printf '  FAIL  gl_zink   %-6s (log %s)\n' "$arch" "$ROOT/tmp/gl_zink_$arch.log"; fail=$((fail + 1))
    fi
done
echo "== vk_device: $pass passed, $fail failed"
[ $fail -eq 0 ]
