#!/bin/bash
# Build Mesa's KosmicKrisp (Vulkan on Metal 4) with SteamARM's patches, for
# macOS arm64, without sudo, into $HOME/SteamARM-build/mesa-kk.
#
#   sources : $KK_ROOT/mesa-<ver>             (+ patches/kosmickrisp-*.patch)
#   output  : $KK_ROOT/out/libvulkan_kosmickrisp.dylib
#
# The launcher points the Vulkan shim at the output when it exists
# (STEAMARM_KK_DIR, scripts/settings-env.py); Homebrew's unpatched driver is
# what the shim loads otherwise. The patches are what Counter-Strike 2 needed
# (benchmarks/stage51-cs2-linux-native.txt):
#   01  command allocators borrowed from the device and returned when the GPU
#       has finished: Source 2's thousands of command buffers each kept three,
#       with their memory (29 GB in half a minute);
#   02  an occlusion query takes its visibility-buffer slot when it is begun:
#       the third pool of 16384 queries could not be created;
#   03  subgroup operations outside fragment and compute shaders act on a
#       subgroup of one: Metal has no SIMD groups in a vertex function;
#   04  consecutive passes on the same attachments in one Metal encoder, the
#       application's load and store operations, cheaper barriers: the GPU
#       pays for every pass, whatever its size;
#   05  a vertex stage does not wait for the render pass before it, unless
#       it samples a texture or the pass's shaders write memory
#       (benchmarks/stage53-fullscreen-layer-order.txt);
#   06  KK_PRESENT_LOG=1: when the GPU finished each presented frame and when
#       the display showed it; KK_PRESENT=1|2|3: other orders of a present;
#   07  the retain taken on every drawable presented, or acquired and never
#       presented, is given back: after two swapchains with an image in hand
#       the layer had no drawable left (a game changing its video settings:
#       black, a second per frame; benchmarks/stage54). KK_DRAWABLE_LOG=1.
#   08  alpha-to-coverage from the alpha the application's shader wrote, in
#       the shader when its blending, write mask or trimming to the
#       attachment changes that alpha (a depth prepass with colour writes off
#       covered every sample; B10G11R11 has no alpha), and in the shader key
#       with the depth/stencil formats: through a VkPipelineCache a pipeline
#       was handed the one compiled without alpha-to-coverage. Cut-out
#       foliage drawn opaque (de_dust2's palms; tests/elf/vk_alphamask.c);
#   09  a pipeline with no fragment shader (a depth or shadow prepass) hashes
#       its render-pass and depth state too: through a VkPipelineCache it was
#       handed another's depth state (tests/elf/vk_vsonly.c).
#
# Only Homebrew formulae and a Python virtualenv are installed. Re-running is
# safe: the download is cached and the build is incremental. Needs macOS 26.
set -euo pipefail

KK_ROOT="${KK_ROOT:-${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk}"
BREW="${BREW:-/opt/homebrew}"
PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

MESA_VER=26.2.4
MESA_SHA=bce5f7fbebb934373b86c999a064d52fb5065878dc57f287f95346648ec832e9

log() { printf '[build-kosmickrisp] %s\n' "$*"; }

if [ "$(sw_vers -productVersion | cut -d. -f1)" -lt 26 ]; then
    log "KosmicKrisp needs Metal 4 (macOS 26 or later): nothing to build"
    exit 0
fi

# 1. Homebrew formulae: the driver's own dependencies, and LLVM for mesa_clc
#    (the driver's built-in shaders are compiled from OpenCL C at build time).
if [ "${SKIP_BREW:-0}" != 1 ]; then
    log "installing Homebrew formulae"
    "$BREW/bin/brew" install meson ninja pkgconf python@3.14 llvm spirv-llvm-translator spirv-tools \
        glslang zstd libx11 libxcb molten-vk
fi
export PATH="$BREW/bin:$PATH"
export PKG_CONFIG_PATH="$BREW/lib/pkgconfig:$BREW/share/pkgconfig"

mkdir -p "$KK_ROOT/out"
cd "$KK_ROOT"

# 2. Mesa source + SteamARM patches
SRC="$KK_ROOT/mesa-$MESA_VER"
TARBALL="$KK_ROOT/mesa-$MESA_VER.tar.xz"
[ -f "$TARBALL" ] || curl -fL --retry 3 -o "$TARBALL" "https://archive.mesa3d.org/mesa-$MESA_VER.tar.xz"
echo "$MESA_SHA  $TARBALL" | shasum -a 256 -c - >/dev/null || { log "sha256 mismatch: $TARBALL"; exit 1; }
# A fresh tree whenever the set of patches changes: they build on each
# other's files, so "is it applied already" cannot be asked of each alone
# (see build-xquartz.sh). The build directory is outside the tree and kept;
# the sources get the time of extraction, so the driver is compiled again.
PATCH_STAMP="$( cd "$PROJECT_DIR/patches" && cat kosmickrisp-*.patch | shasum -a 256 | cut -d' ' -f1 )"
if [ ! -d "$SRC" ] || [ "$(cat "$SRC/.steamarm-patches" 2>/dev/null)" != "$PATCH_STAMP" ]; then
    log "fresh source tree (new or changed patches)"
    rm -rf "$SRC"
    tar xmf "$TARBALL"
    for p in "$PROJECT_DIR"/patches/kosmickrisp-*.patch; do
        [ -f "$p" ] || continue
        log "applying $(basename "$p")"
        patch -d "$SRC" -p1 -N -s < "$p"
    done
    echo "$PATCH_STAMP" > "$SRC/.steamarm-patches"
fi

# 3. Python modules Mesa's generators need (not Homebrew's: a virtualenv)
if [ ! -x "$KK_ROOT/venv/bin/python" ]; then
    log "python virtualenv"
    "$BREW/bin/python3.14" -m venv "$KK_ROOT/venv"
    "$KK_ROOT/venv/bin/pip" -q install mako pyyaml packaging
fi
# LLVM last: its llvm-config is needed, its clang is not (Apple's compiles).
export PATH="$KK_ROOT/venv/bin:$PATH:$BREW/opt/llvm/bin"

# 4. Configure (this driver only) + build
if [ ! -f "$KK_ROOT/build/build.ninja" ] || ! ninja -C "$KK_ROOT/build" -n >/dev/null 2>&1; then
    log "meson setup"
    rm -rf "$KK_ROOT/build"
    CC=/usr/bin/clang CXX=/usr/bin/clang++ OBJC=/usr/bin/clang meson setup "$KK_ROOT/build" "$SRC" \
        -Dbuildtype=release -Db_ndebug=true -Dvulkan-drivers=kosmickrisp -Dgallium-drivers= \
        -Dopengl=false -Dglx=disabled -Degl=disabled -Dgles1=disabled -Dgles2=disabled -Dgbm=disabled \
        -Dllvm=enabled -Dshared-llvm=enabled -Dplatforms=macos -Dvideo-codecs= -Dtools= -Dvulkan-layers= \
        -Dzstd=enabled -Dmoltenvk-dir="$BREW/opt/molten-vk" > "$KK_ROOT/setup.log" 2>&1 ||
        { tail -20 "$KK_ROOT/setup.log"; log "meson setup failed (log: $KK_ROOT/setup.log)"; exit 1; }
fi
log "ninja"
ninja -C "$KK_ROOT/build" src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib > "$KK_ROOT/ninja.log" 2>&1 ||
    { tail -30 "$KK_ROOT/ninja.log"; log "build failed (log: $KK_ROOT/ninja.log)"; exit 1; }

# 5. The driver the shim loads: replaced only by one that loads and exports
#    the ICD entry point.
NEW="$KK_ROOT/build/src/kosmickrisp/vulkan/libvulkan_kosmickrisp.dylib"
nm -gU "$NEW" | grep -q " _vk_icdGetInstanceProcAddr$" || { log "built driver has no vk_icdGetInstanceProcAddr"; exit 1; }
install -m 0755 "$NEW" "$KK_ROOT/out/libvulkan_kosmickrisp.dylib.new"
mv -f "$KK_ROOT/out/libvulkan_kosmickrisp.dylib.new" "$KK_ROOT/out/libvulkan_kosmickrisp.dylib"
log "driver: $KK_ROOT/out/libvulkan_kosmickrisp.dylib ($(shasum -a 256 "$KK_ROOT/out/libvulkan_kosmickrisp.dylib" | cut -c1-12))"
