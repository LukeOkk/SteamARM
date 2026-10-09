#!/bin/bash
# Build FEX's ARM64EC module (libarm64ecfex.dll: the x86-64 JIT that Wine loads
# into a Windows process) for the native Proton tool, at the commit of Valve's
# Proton ARM64 build plus SteamARM's patches. scripts/install-native-proton.sh
# puts it in place of the copy of Valve's. Without sudo, into
# $STEAMARM_BUILD/fex-arm64ec (default ~/SteamARM-build).
#
#   sources : FEX at FEX_COMMIT (Valve's: "FEX-2609-137-g0df84d3")
#             + patches/fex-arm64ec/*.patch
#   output  : $OUT/libarm64ecfex.dll   (OUT=$FEX_EC_ROOT/out)
#
# The patches:
#   0001  the file name of a mapped image is converted on the stack, not in
#         the process heap: FEX does it holding its thread lock, and a thread
#         holding the process heap's lock takes that lock when the heap grows
#         or writes to a page FEX write-protected for self-modifying code.
#         Minecraft Dungeons II hung at its intro (each thread waiting on the
#         other's lock; benchmarks/stage62, 15 l).
#   0002-0012  Dmage22/FEX-D2 (branch d2r/clean): the ARM64EC fixes that run
#         Blizzard's Arxan-protected loaders -- executable views of non-image
#         sections, the OS asked when an address has no executable interval,
#         page faults raised as access violations at the real fetch address,
#         stale NoExec stubs retried, EFlags PF/AF carried through exceptions
#         (with patches/wine-ntdll), the compiler stopped at inaccessible pages,
#         per-exception debug lines only with FEX_DEBUGLOG=1, 0x67 MOVS/STOS and
#         MOV/POP FS/GS in 64-bit mode.
#   0013  a thread_local of 0007 moved into the thread's FEX state (a TLS slot
#         in this DLL is the guest's), NtDllRedirectionLUTSize 64-bit (Module.S
#         loads it with a 64-bit ldr).
#   0014  JIT-generated guest exceptions raised by a direct call instead of a
#         host trap (STEAMARM_FEX_TRAP_HOOK=0 restores the trap).
#   0015  SteamARM's own DiskCache format version: the cache is keyed on it
#         and the host only, and Proton's FEX shares the Steam shader cache
#         directory, so each would run the other's code.
#   0016  self-modifying code tracked in 16 KiB blocks: a 16 KiB host page
#         carries the union of its four 4 KiB guest pages' protections, so a
#         translated page is only write-protected while all four are
#         (Minecraft Dungeons II's protector ran stale stubs ~140 s in).
#         STEAMARM_FEX_SMC_BLOCK=4096 restores FEX's own granularity.
#
# Needs cmake, ninja and python3, and llvm-mingw (downloaded into
# $STEAMARM_BUILD/toolchains when missing; LLVM_MINGW=<dir> picks another).
# Idempotent: nothing is rebuilt when the commit and the patches are those of
# the last build (stamp file).
set -euo pipefail
cd "$(dirname "$0")/.."
REPO=$PWD
B="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
FEX_EC_ROOT="${FEX_EC_ROOT:-$B/fex-arm64ec}"
FEX_COMMIT=0df84d3844bcdb87bb7d3f5b8fb0959cd009c038
FEX_VERSION=FEX-2609-137-g0df84d3-steamarm   # Valve's build plus ours; a shallow clone has no tags to describe
MINGW_REL=20260922
OUT="$FEX_EC_ROOT/out"
log() { printf '[build-fex-arm64ec] %s\n' "$*"; }

for t in cmake ninja python3 git curl; do
    command -v "$t" >/dev/null || { log "missing $t (brew install cmake ninja python)"; exit 1; }
done
MINGW="${LLVM_MINGW:-$B/toolchains/llvm-mingw-$MINGW_REL-ucrt-macos-universal}"
if [ ! -x "$MINGW/bin/arm64ec-w64-mingw32-clang" ]; then
    log "llvm-mingw $MINGW_REL"
    mkdir -p "$B/toolchains"
    curl -fsSL "https://github.com/mstorsjo/llvm-mingw/releases/download/$MINGW_REL/llvm-mingw-$MINGW_REL-ucrt-macos-universal.tar.xz" \
        | tar -xJ -C "$B/toolchains"
fi

stamp="$FEX_COMMIT $FEX_VERSION $(cat "$REPO"/patches/fex-arm64ec/*.patch | shasum -a 256 | cut -c1-16)"
if [ -f "$OUT/libarm64ecfex.dll" ] && [ "$(cat "$OUT/.stamp" 2>/dev/null)" = "$stamp" ]; then
    log "up to date ($OUT)"
    exit 0
fi

SRC="$FEX_EC_ROOT/src"
if [ ! -d "$SRC/.git" ] || [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null)" != "$FEX_COMMIT" ]; then
    log "sources at ${FEX_COMMIT:0:12}"
    rm -rf "${SRC:?}"
    mkdir -p "$SRC"
    git -C "$SRC" init -q
    git -C "$SRC" remote add origin https://github.com/FEX-Emu/FEX.git
    git -C "$SRC" fetch -q --depth 1 origin "$FEX_COMMIT"
    git -C "$SRC" checkout -q FETCH_HEAD
    # What the ARM64EC module builds from (not the test binaries).
    git -C "$SRC" submodule update -q --init --depth 1 -- External/fmt External/range-v3 External/rpmalloc \
        External/unordered_dense External/vixl External/xxhash External/tracy External/Vulkan-Headers \
        External/drm-headers External/Catch2 Source/Common/cpp-optparse
fi
git -C "$SRC" checkout -q -- .
for p in "$REPO"/patches/fex-arm64ec/*.patch; do
    git -C "$SRC" apply "$p" || { log "patch $(basename "$p") does not apply"; exit 1; }
done

BUILD="$FEX_EC_ROOT/build"
rm -rf "${BUILD:?}"
export PATH="$MINGW/bin:$PATH"
log "configure (ARM64EC, llvm-mingw)"
# TUNE_CPU=none: -mcpu=apple-m1 made clang fail on SEH unwind info.
cmake -S "$SRC" -B "$BUILD" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$SRC/Data/CMake/toolchain_mingw.cmake" \
    -DMINGW_TRIPLE=arm64ec-w64-mingw32 -DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=False -DBUILD_TESTING=False \
    -DENABLE_ASSERTIONS=OFF -DBUILD_FEXCONFIG=OFF -DTUNE_CPU=none -DOVERRIDE_VERSION="$FEX_VERSION" > "$FEX_EC_ROOT/cmake.log" 2>&1 \
    || { log "cmake failed: $FEX_EC_ROOT/cmake.log"; exit 1; }
log "build"
ninja -C "$BUILD" arm64ecfex > "$FEX_EC_ROOT/ninja.log" 2>&1 || { log "build failed: $FEX_EC_ROOT/ninja.log"; exit 1; }
mkdir -p "$OUT"
cp "$BUILD/Bin/libarm64ecfex.dll" "$OUT/"
printf '%s' "$stamp" > "$OUT/.stamp"
log "built $OUT ($(shasum -a 256 "$OUT/libarm64ecfex.dll" | cut -c1-12))"
