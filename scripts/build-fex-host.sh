#!/usr/bin/env bash
# Build FEX (a Linux aarch64 ELF) on the macOS host, with no VM anywhere.
#
# Why this exists: exit criterion #8 -- FEX and every component the runtime
# needs must build WITHOUT the Fedora VM. Until now FEX was compiled inside
# the VM (~/FEX/Build there) and copied out. This script is the VM-free
# replacement, end to end on a clean Mac:
#
#   1. sysroot   Fedora 43 aarch64 RPMs (glibc, glibc-devel, kernel-headers,
#                libgcc, libstdc++, libstdc++-devel, gcc for crtbegin*.o,
#                libgcc.a, libgcc_s.so, and gdb for <gdb/jit-reader.h>)
#                downloaded over HTTPS from dl.fedoraproject.org (Koji as
#                fallback), checked against the sha256 pins below, unpacked
#                with bsdtar. No rpm, no dnf. 65 MB unpacked.
#   2. toolchain Homebrew clang + lld cross-compile for aarch64-redhat-linux-gnu
#                against that sysroot: scripts/toolchain-aarch64-linux-fedora.cmake
#   3. FEX       upstream at the pinned commit, the patches/ files in the
#                PATCHES list below (that order; the newest,
#                fex-lxrt-lowwin-hint.patch, is for x86-64 Android's ART) and
#                patches/LxrtJit.h;
#                CMake + Ninja; targets FEX, FEXServer,
#                FEXGetConfig; Release (the VM's Build/) and RelWithDebInfo
#                (the VM's Build-dbg/), both with -ffixed-x18 as on the VM.
#   4. out       $WORK/out/: FEX FEXServer FEXGetConfig (Release),
#                FEX-dbg FEXServer-dbg FEXGetConfig-dbg (RelWithDebInfo), and
#                FEX-emu: the emulator-prefix copy (PT_INTERP and DT_RPATH in
#                /usr/lib/lxrt-emu), linked that way instead of patchelf'd.
#                Nothing is installed into a guest root.
#
# Usage: scripts/build-fex-host.sh [sysroot|source|build|emu|all]...  (default all)
#
# Environment:
#   STEAMARM_BUILD   work directory               (default $HOME/SteamARM-build)
#   LLVM_BIN         clang, llvm-*                (default /opt/homebrew/opt/llvm/bin)
#   LLD              ld.lld                       (default next to clang, else PATH)
#   JOBS             ninja -j                     (default: all cores)
#   FEX_BUILD_TYPES  "Release RelWithDebInfo"
#   FEX_LTO          ON|OFF                       (default ON, as on the VM)
#   FEX_TUNE_CPU     -mcpu for FEX's own code     (default apple-m1; the VM used
#                    TUNE_CPU=native, meaningless when cross compiling)
#
# Needs (Homebrew): llvm lld cmake ninja zstd -- bsdtar hands Fedora's zstd
# RPM payloads to the zstd program. Plus the system git, curl, python3.
#
# Idempotent: RPMs are cached and re-verified, the sysroot is rebuilt only
# when the pin list changes, the FEX tree is reset and re-patched only when
# the commit or a patch file changes, a build dir is reconfigured from
# scratch only when the sysroot, toolchain file or flags change, and ninja
# rebuilds incrementally. From an empty work dir on an M4: ~2 min.
#
# To move a pin: the repodata of the updates repo lists the current file
# name and sha256 (repodata/repomd.xml -> *-primary.xml.zst); Koji keeps
# every build forever at $KOJI/<source-package>/<version>/<release>/aarch64/.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
SYSROOT="$WORK/sysroot-f43"
RPMDIR="$WORK/rpms"
SRC="$WORK/FEX"
BUILD="$WORK/build"
OUT="$WORK/out"
STATE="$WORK/state"
LLVM_BIN="${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}"
if [ -z "${LLD:-}" ]; then
    if [ -x "$LLVM_BIN/ld.lld" ]; then LLD="$LLVM_BIN/ld.lld"; else LLD="$(command -v ld.lld || true)"; fi
fi
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || echo 8)}"
BUILD_TYPES="${FEX_BUILD_TYPES:-Release RelWithDebInfo}"
LTO="${FEX_LTO:-ON}"
TUNE_CPU="${FEX_TUNE_CPU:-apple-m1}"
TOOLCHAIN="$REPO/scripts/toolchain-aarch64-linux-fedora.cmake"

FEX_URL=https://github.com/FEX-Emu/FEX.git
FEX_COMMIT=08f451d3bce62a68aa6307dced5a8eaeb85c39bc      # FEX-2609-113-g08f451d3b
# Only what this configuration compiles (no tests, thunks, FEXConfig, Zydis,
# tracy, jemalloc_glibc): the posixtest/gvisor/gcc test-bin repos are skipped.
FEX_SUBMODULES=(External/fmt External/range-v3 External/rpmalloc External/drm-headers
                External/xxhash External/unordered_dense Source/Common/cpp-optparse)
PATCHES=(fex-lxrt-x18.patch fex-lxrt-guest-base.patch fex-lxrt-wx.patch fex-lxrt-hide-hypervisor.patch fex-lxrt-shebang.patch fex-lxrt-guest-reserve.patch fex-lxrt-thunk-args.patch fex-lxrt-arch-prctl.patch fex-lxrt-lowwin-hint.patch fex-lxrt-smc-mprotect-mirrors.patch fex-lxrt-interrupt-page.patch)
TARGETS=(FEX FEXServer FEXGetConfig)
EMU_PREFIX=/usr/lib/lxrt-emu

F43_UPDATES=https://dl.fedoraproject.org/pub/fedora/linux/updates/43/Everything/aarch64/Packages
F43_RELEASE=https://dl.fedoraproject.org/pub/fedora/linux/releases/43/Everything/aarch64/os/Packages
F43_ARCHIVE=https://dl.fedoraproject.org/pub/archive/fedora/linux/updates/43/Everything/aarch64/Packages
KOJI=https://kojipkgs.fedoraproject.org/packages

# file  source-package  sha256   (Fedora 43 updates, 2026-09-26; the VM's FEX
# links glibc 2.42 and says "GCC: (GNU) 15.3.1 20260722 (Red Hat 15.3.1-1)")
RPMS=(
  "glibc-2.42-16.fc43.aarch64.rpm            glibc          0a6651548c09a06a74ee92598dc69e0d3d0004b05b47d2d9be4cd4d129d95d6f"
  "glibc-devel-2.42-16.fc43.aarch64.rpm      glibc          d42b7f380d83bfa8d436957f65111085e16224663e7e0ad9635635946b40fa7f"
  "kernel-headers-7.2.4-100.fc43.aarch64.rpm kernel-headers 1083cc19cbaa03800cb1cfd95fba3aa621c4ccade815da2718ff2f6b593b9218"
  "libgcc-15.3.1-1.fc43.aarch64.rpm          gcc            150d21714e9e046680eed73811267e75b774cf2cf655a1f76583ce5eaff5b79d"
  "libstdc++-15.3.1-1.fc43.aarch64.rpm       gcc            d0658a38e029feb227797a8549e37237a9931257be2daa8fb3bea793352afb51"
  "libstdc++-devel-15.3.1-1.fc43.aarch64.rpm gcc            d05e067c5895f99ed4f92a28220a47ed1312a39d9497e38f05ea63034d62448d"
  "gcc-15.3.1-1.fc43.aarch64.rpm             gcc            3ab396c6b37a580e5c8886cf4e9c4532ed36f75b7f48e9e303ec5b82538b7432"
  "gdb-17.2-2.fc43.aarch64.rpm               gdb            e5f15b06699c96f575f301adbc7c6bc684632ea30f163eb8ac99c46c09708f06"
)

log() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }

check_tools() {
    local t
    for t in "$LLVM_BIN/clang" "$LLVM_BIN/clang++" "$LLVM_BIN/llvm-ar" "$LLD"; do
        [ -x "$t" ] || die "missing $t (brew install llvm lld)"
    done
    for t in cmake ninja git curl python3 tar shasum zstd; do
        command -v "$t" >/dev/null || die "missing $t (brew install cmake ninja zstd)"
    done
}

# ------------------------------------------------------------------ sysroot
fetch_rpm() {   # file source-package sha256
    local f="$1" src="$2" sum="$3" dest="$RPMDIR/$1"
    if [ -f "$dest" ] && [ "$(sha256 "$dest")" = "$sum" ]; then
        log "  cached  $f"; return 0
    fi
    local nvr="${f%.aarch64.rpm}"; local rel="${nvr##*-}"; local nv="${nvr%-*}"; local ver="${nv##*-}"
    local l; l="$(printf %s "$f" | cut -c1)"
    local url
    for url in "$F43_UPDATES/$l/$f" "$F43_RELEASE/$l/$f" "$F43_ARCHIVE/$l/$f" "$KOJI/$src/$ver/$rel/aarch64/$f"; do
        log "  fetch   $url"
        if curl -fsSL --retry 3 --connect-timeout 20 -o "$dest.part" "$url"; then
            if [ "$(sha256 "$dest.part")" = "$sum" ]; then
                mv "$dest.part" "$dest"; log "          ok, sha256 $sum"; return 0
            fi
            log "          sha256 mismatch, trying the next source"
        fi
    done
    rm -f "$dest.part"
    die "could not fetch $f with sha256 $sum"
}

do_sysroot() {
    log "== sysroot: Fedora 43 aarch64 RPMs -> $SYSROOT"
    mkdir -p "$RPMDIR"
    local e
    for e in "${RPMS[@]}"; do
        # shellcheck disable=SC2086
        set -- $e; fetch_rpm "$1" "$2" "$3"
    done
    local want; want="$(printf '%s\n' "${RPMS[@]}" | shasum -a 256 | cut -d' ' -f1)"
    if [ -f "$SYSROOT/.lxrt-sysroot" ] && [ "$(cat "$SYSROOT/.lxrt-sysroot")" = "$want" ]; then
        log "  sysroot up to date"; return 0
    fi
    local tmp="$SYSROOT.tmp"
    rm -rf "$tmp"; mkdir -p "$tmp"
    for e in "${RPMS[@]}"; do
        # shellcheck disable=SC2086
        set -- $e
        log "  unpack  $1"
        case "$1" in
            # gcc: only the GCC install dir (crt files, libgcc.a, libgcc_s.so
            # script); the compiler binaries are 100+ MB nobody here runs.
            gcc-[0-9]*) tar -xf "$RPMDIR/$1" -C "$tmp" ./usr/lib/gcc ;;
            # gdb: only <gdb/jit-reader.h>. The VM had gdb installed, so FEX's
            # CMake turned ENABLE_GDB_SYMBOLS on there (FEX-gb-dbg carries
            # __jit_debug_register_code); same here.
            gdb-[0-9]*) tar -xf "$RPMDIR/$1" -C "$tmp" ./usr/include/gdb ;;
            *) tar -xf "$RPMDIR/$1" -C "$tmp" --exclude './usr/share/*' \
                   --exclude './usr/lib/.build-id*' --exclude './usr/lib64/gconv*' ;;
        esac
    done
    # Merged /usr, as on Fedora (the filesystem package, not installed, owns
    # these links). Some payloads still use the old paths (libgcc ships
    # /lib64/libgcc_s.so.1): fold those into usr/ first.
    local d
    for d in bin sbin lib lib64; do
        if [ -d "$tmp/$d" ] && [ ! -L "$tmp/$d" ]; then
            mkdir -p "$tmp/usr/$d"
            cp -a "$tmp/$d/." "$tmp/usr/$d/"
            rm -rf "${tmp:?}/$d"
        fi
        [ -L "$tmp/$d" ] || ln -s "usr/$d" "$tmp/$d"
    done
    # Absolute symlinks would point into the Mac's own /usr: make them relative.
    python3 - "$tmp" <<'PY'
import os, sys
root = sys.argv[1]
n = 0
for dp, dns, fns in os.walk(root):
    for name in dns + fns:
        p = os.path.join(dp, name)
        if os.path.islink(p):
            t = os.readlink(p)
            if t.startswith('/'):
                os.unlink(p)
                os.symlink(os.path.relpath(os.path.join(root, t.lstrip('/')), dp), p)
                n += 1
print(f"  {n} absolute symlinks made relative")
PY
    [ -f "$tmp/usr/include/stdio.h" ] && [ -f "$tmp/usr/include/linux/version.h" ] \
        && [ -e "$tmp/usr/lib64/libc.so.6" ] && [ -e "$tmp/usr/lib/ld-linux-aarch64.so.1" ] \
        && ls "$tmp"/usr/lib/gcc/aarch64-redhat-linux/*/crtbeginS.o >/dev/null \
        || die "sysroot incomplete after unpacking"
    echo "$want" > "$tmp/.lxrt-sysroot"
    rm -rf "$SYSROOT"; mv "$tmp" "$SYSROOT"
    log "  sysroot ready: $(du -sh "$SYSROOT" | cut -f1)"
}

# ------------------------------------------------------------------- source
do_source() {
    log "== FEX source: $FEX_COMMIT -> $SRC"
    if [ ! -d "$SRC/.git" ]; then
        # Blobless: full commit/tag history (git describe gives the version
        # string FEX embeds) without every historical file.
        git clone --filter=blob:none --no-checkout "$FEX_URL" "$SRC"
    fi
    git -C "$SRC" cat-file -e "$FEX_COMMIT^{commit}" 2>/dev/null || git -C "$SRC" fetch --tags origin
    local p want have=""
    want="$( { echo "$FEX_COMMIT"; for p in "${PATCHES[@]}"; do cat "$REPO/patches/$p"; done
               cat "$REPO/patches/LxrtJit.h"; } | shasum -a 256 | cut -d' ' -f1)"
    [ -f "$STATE/patched" ] && have="$(cat "$STATE/patched")"
    if [ "$have" = "$want" ] && [ "$(git -C "$SRC" rev-parse HEAD)" = "$FEX_COMMIT" ]; then
        log "  tree at $FEX_COMMIT with the current patches"
    else
        rm -f "$STATE/patched"
        git -C "$SRC" checkout -q -f --detach "$FEX_COMMIT"
        git -C "$SRC" clean -fdxq      # the patches' new files; builds live outside
        for p in "${PATCHES[@]}"; do
            log "  apply   patches/$p"
            git -C "$SRC" apply --whitespace=nowarn "$REPO/patches/$p"
        done
        # fex-lxrt-wx.patch includes <FEXCore/Utils/LxrtJit.h>.
        log "  copy    patches/LxrtJit.h -> FEXCore/include/FEXCore/Utils/"
        cp "$REPO/patches/LxrtJit.h" "$SRC/FEXCore/include/FEXCore/Utils/LxrtJit.h"
        mkdir -p "$STATE"; echo "$want" > "$STATE/patched"
    fi
    git -C "$SRC" submodule update --init --depth 1 -- "${FEX_SUBMODULES[@]}" \
        || git -C "$SRC" submodule update --init -- "${FEX_SUBMODULES[@]}"
    log "  $(git -C "$SRC" describe --tags --abbrev=9), $(git -C "$SRC" status --porcelain | wc -l | tr -d ' ') changed/new files"
}

# -------------------------------------------------------------------- build
do_build() {
    # -ffixed-x18 as on the VM (Darwin zeroes x18; benchmarks/stage5-x18.txt).
    # Passed as CFLAGS/CXXFLAGS so the toolchain's --gcc-install-dir stays.
    local cflags=-ffixed-x18
    # FEX embeds __DATE__/__TIME__ (its uname version string); pinning them to
    # the commit date makes two builds of the same inputs bit-identical.
    SOURCE_DATE_EPOCH="$(git -C "$SRC" log -1 --format=%ct "$FEX_COMMIT")"
    export SOURCE_DATE_EPOCH
    local bt b t0 stamp
    for bt in $BUILD_TYPES; do
        b="$BUILD/$bt"
        local args=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$TOOLCHAIN"
            -DFEDORA_SYSROOT="$SYSROOT" -DLLVM_BIN="$LLVM_BIN" -DLLD="$LLD"
            -DCMAKE_BUILD_TYPE="$bt"
            -DBUILD_TESTING=OFF -DBUILD_THUNKS=OFF -DBUILD_FEXCONFIG=OFF
            -DENABLE_LTO="$LTO" -DENABLE_FEX_ALLOCATOR=ON -DENABLE_JEMALLOC_GLIBC_ALLOC=OFF
            -DENABLE_GDB_SYMBOLS=ON
            -DTUNE_CPU="$TUNE_CPU" -DTUNE_ARCH=generic -DUSE_LEGACY_BINFMTMISC=OFF
            -DENABLE_CCACHE=OFF)
        # A new sysroot, toolchain file or flag set means a fresh configure:
        # CMake caches compiler checks and never re-reads *_INIT flags.
        stamp="$( { cat "$SYSROOT/.lxrt-sysroot" "$TOOLCHAIN"
                    printf '%s\n' "$cflags" "$SOURCE_DATE_EPOCH" "${args[@]}"; } \
                  | shasum -a 256 | cut -d' ' -f1)"
        if [ ! -f "$b/build.ninja" ] || [ "$(cat "$b/.lxrt-config" 2>/dev/null)" != "$stamp" ]; then
            rm -rf "$b"
            log "== configure $bt -> $b"
            CFLAGS="$cflags" CXXFLAGS="$cflags" cmake -S "$SRC" -B "$b" "${args[@]}"
            echo "$stamp" > "$b/.lxrt-config"
        fi
        grep -q -- '-ffixed-x18' "$b/CMakeCache.txt" || die "$b: CMAKE_CXX_FLAGS lost -ffixed-x18"
        log "== build $bt: ${TARGETS[*]} (-j$JOBS)"
        t0=$(date +%s)
        ninja -C "$b" -j "$JOBS" "${TARGETS[@]}"
        log "  $bt: ${TARGETS[*]} up to date after $(( $(date +%s) - t0 )) s"
    done
    mkdir -p "$OUT"
    local t
    for t in "${TARGETS[@]}"; do
        [ -f "$BUILD/Release/Bin/$t" ] && cp -p "$BUILD/Release/Bin/$t" "$OUT/$t"
        [ -f "$BUILD/RelWithDebInfo/Bin/$t" ] && cp -p "$BUILD/RelWithDebInfo/Bin/$t" "$OUT/$t-dbg"
    done
    return 0
}

# ---------------------------------------------------------------------- emu
# FEX for the emulator prefix: the loader and libraries it uses live in
# /usr/lib/lxrt-emu of every guest view (scripts/mksteamroot.sh). The VM made
# this copy with patchelf; here the Release link is re-run with the loader
# path and a DT_RPATH (not RUNPATH: it must also win for dependencies).
do_emu() {
    local b="$BUILD/Release"
    [ -f "$b/build.ninja" ] || die "no Release build in $b"
    log "== FEX-emu: relink with PT_INTERP/DT_RPATH in $EMU_PREFIX"
    if [ -f "$OUT/FEX-emu" ] && [ "$OUT/FEX-emu" -nt "$b/Bin/FEX" ]; then
        log "  up to date"; return 0
    fi
    local cmd
    cmd="$(ninja -C "$b" -t commands Bin/FEX | tail -n 1)"
    case "$cmd" in *" -o Bin/FEX "*) ;; *) die "unexpected link command: $cmd" ;; esac
    mkdir -p "$OUT"
    # Same objects and flags; only the output, the loader path and the rpath
    # change (and ninja's depfile for Bin/FEX is left alone).
    cmd="$(printf '%s' "$cmd" | sed -E 's/ -Xlinker --dependency-file=[^ ]+//')"
    cmd="${cmd/ -o Bin\/FEX / -o $OUT/FEX-emu -Wl,--dynamic-linker=$EMU_PREFIX/ld-linux-aarch64.so.1 -Wl,-rpath,$EMU_PREFIX -Wl,--disable-new-dtags }"
    (cd "$b" && eval "$cmd")
    "$LLVM_BIN/llvm-readelf" -l -d "$OUT/FEX-emu" | grep -E 'interpreter|RPATH|RUNPATH' | sed 's/^/  /'
}

summary() {
    log "== $OUT"
    local f
    for f in "$OUT"/*; do
        [ -f "$f" ] || continue
        printf '  %-18s %10s bytes  sha256 %s\n' "$(basename "$f")" "$(stat -f %z "$f")" "$(sha256 "$f")"
    done
}

main() {
    [ $# -gt 0 ] || set -- all
    check_tools
    local step
    for step in "$@"; do
        case "$step" in
            sysroot) do_sysroot ;;
            source)  do_source ;;
            build)   do_build ;;
            emu)     do_emu ;;
            all)     do_sysroot; do_source; do_build; do_emu ;;
            *) die "unknown step '$step' (sysroot|source|build|emu|all)" ;;
        esac
    done
    summary
}
main "$@"
