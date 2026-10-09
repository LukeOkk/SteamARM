#!/bin/bash
# Build vkd3d-proton (D3D12 on Vulkan) with SteamARM's patches, as an ARM64X
# Windows DLL pair, for the native Proton tool (scripts/install-native-proton.sh
# puts it in place of the copy of Valve's). Without sudo, into
# $STEAMARM_BUILD/vkd3d-proton (default ~/SteamARM-build).
#
#   sources : upstream at VKD3D_COMMIT + patches/vkd3d-proton/*.patch
#   output  : $OUT/d3d12.dll, $OUT/d3d12core.dll   (OUT=$VKD3D_ROOT/out)
#
# The patches:
#   0001  a placement that does not meet D3D12's 64 KiB but does meet what
#         Vulkan can bind is accepted (FIXME once) instead of E_INVALIDARG.
#         D3D12's release runtime does not refuse it, and Unreal Engine 5.6's
#         transient allocator aligns GPU virtual addresses, which KosmicKrisp
#         gives at 32 KiB: Minecraft Dungeons II stopped on a fatal
#         "D3D12Resources.cpp:1042 E_INVALIDARG" (benchmarks/stage62, 15).
#
# Needs meson, ninja and glslang (Homebrew) and llvm-mingw, downloaded into
# $STEAMARM_BUILD/toolchains when missing (LLVM_MINGW=<dir> picks another).
# Idempotent: nothing is rebuilt when the commit and the patches are those of
# the last build (stamp file).
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
B="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
VKD3D_ROOT="${VKD3D_ROOT:-$B/vkd3d-proton}"
VKD3D_COMMIT=2230755878b01993b4b82d0ac0c0624f3ab4d333
MINGW_REL=20260922
OUT="$VKD3D_ROOT/out"
log() { printf '[build-vkd3d-proton] %s\n' "$*"; }

for t in meson ninja glslangValidator git curl; do
    command -v "$t" >/dev/null || { log "missing $t (brew install meson ninja glslang)"; exit 1; }
done
MINGW="${LLVM_MINGW:-$B/toolchains/llvm-mingw-$MINGW_REL-ucrt-macos-universal}"
if [ ! -x "$MINGW/bin/aarch64-w64-mingw32-clang" ]; then
    log "llvm-mingw $MINGW_REL"
    mkdir -p "$B/toolchains"
    curl -fsSL "https://github.com/mstorsjo/llvm-mingw/releases/download/$MINGW_REL/llvm-mingw-$MINGW_REL-ucrt-macos-universal.tar.xz" \
        | tar -xJ -C "$B/toolchains"
fi

stamp="$VKD3D_COMMIT $(cat "$REPO"/patches/vkd3d-proton/*.patch | shasum -a 256 | cut -c1-16)"
if [ -f "$OUT/d3d12core.dll" ] && [ "$(cat "$OUT/.stamp" 2>/dev/null)" = "$stamp" ]; then
    log "up to date ($OUT)"
    exit 0
fi

SRC="$VKD3D_ROOT/src"
if [ ! -d "$SRC/.git" ] || [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" != "$VKD3D_COMMIT" ]; then
    log "sources at ${VKD3D_COMMIT:0:12}"
    rm -rf "${SRC:?}"
    mkdir -p "$SRC"
    git -C "$SRC" init -q
    git -C "$SRC" remote add origin https://github.com/HansKristian-Work/vkd3d-proton
    git -C "$SRC" fetch -q --depth 1 origin "$VKD3D_COMMIT"
    git -C "$SRC" checkout -q FETCH_HEAD
    git -C "$SRC" submodule update -q --init --recursive --depth 1
fi
git -C "$SRC" checkout -q -- .
for p in "$REPO"/patches/vkd3d-proton/*.patch; do
    git -C "$SRC" apply "$p" || { log "patch $(basename "$p") does not apply"; exit 1; }
done

cross="$VKD3D_ROOT/llvm-mingw-arm64x.txt"
cat > "$cross" <<CROSS
[binaries]
c = '$MINGW/bin/aarch64-w64-mingw32-clang'
cpp = '$MINGW/bin/aarch64-w64-mingw32-clang++'
ar = '$MINGW/bin/llvm-ar'
strip = '$MINGW/bin/llvm-strip'
widl = '$MINGW/bin/aarch64-w64-mingw32-widl'
widl-mingw-tools-fallback = '$MINGW/bin/aarch64-w64-mingw32-widl'
windres = '$MINGW/bin/aarch64-w64-mingw32-windres'

[properties]
needs_exe_wrapper = true

[built-in options]
c_args = ['-marm64x']
cpp_args = ['-marm64x']
c_link_args = ['-marm64x']
cpp_link_args = ['-marm64x']

[host_machine]
system = 'windows'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
CROSS
BUILD="$VKD3D_ROOT/build-arm64x"
rm -rf "${BUILD:?}"
log "configure (ARM64X, llvm-mingw)"
meson setup --cross-file "$cross" --buildtype release -Denable_extras=false "$BUILD" "$SRC" > "$VKD3D_ROOT/meson.log" 2>&1 \
    || { log "meson failed: $VKD3D_ROOT/meson.log"; exit 1; }
log "build"
ninja -C "$BUILD" > "$VKD3D_ROOT/ninja.log" 2>&1 || { log "build failed: $VKD3D_ROOT/ninja.log"; exit 1; }
mkdir -p "$OUT"
cp "$BUILD/libs/d3d12/d3d12.dll" "$BUILD/libs/d3d12core/d3d12core.dll" "$OUT/"
printf '%s' "$stamp" > "$OUT/.stamp"
log "built $OUT ($(shasum -a 256 "$OUT/d3d12core.dll" | cut -c1-12))"
