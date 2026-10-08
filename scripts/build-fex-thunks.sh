#!/usr/bin/env bash
# Build FEX's Vulkan library thunks on the macOS host, with no VM:
#
#   x86-64 guest  libvulkan.so.1 (= libvulkan-guest.so, x86-64 ELF)
#        |  0F 3F thunk opcode, handled by the FEX we already run
#        v
#   aarch64 host  libvulkan-host.so (aarch64 ELF, dlopened by FEX)
#        |  dlopen("libvulkan.so.1")
#        v
#   the runtime's ELF shim build/libvulkan.so.1 -> MoltenVK -> Metal
#
# Separate from scripts/build-fex-host.sh on purpose: that script owns
# $WORK/FEX (it resets the tree whenever a patch changes) and the running
# Steam uses its outputs. This one works in a git worktree of the same
# commit, $WORK/FEX-thunks, and writes only under $WORK/thunks-build.
# FEX itself is NOT rebuilt: the thunk handler (OP_THUNK, fex:loadlib,
# ThunksDB overlays) is always compiled into FEX; BUILD_THUNKS only controls
# the libraries, and those are built here without FEX's CMake (its
# BUILD_THUNKS path would compile thunkgen with the aarch64 cross toolchain).
#
# Steps:
#   source    worktree $WORK/FEX-thunks at FEX_COMMIT + the same patches as
#             build-fex-host.sh + patches/fex-thunkgen-macos.patch, and the
#             submodules the thunks need (External/Vulkan-Headers, fmt)
#   x86root   an x86-64 *development* sysroot ($OUT/sysroot-x86_64): the
#             Ubuntu 24.04 rootfs FEX runs has no headers, so pinned noble
#             .debs (glibc, libstdc++-14, libgcc-14, kernel and X11/xcb
#             headers) are fetched over HTTPS, checked against sha256, and
#             unpacked with bsdtar. Nothing goes into the live rootfs.
#   thunkgen  FEX's generator, a macOS arm64 program linked against Homebrew
#             LLVM's libclang-cpp/libLLVM (it runs at build time only)
#   gen       thunkgen on libvulkan_interface.cpp: guest .inl (x86-64 ABI)
#             and host .inl (aarch64-linux ABI, via --target/--sysroot)
#   host      $OUT/HostThunks/libvulkan-host.so   (clang, Fedora 43 sysroot)
#   guest     $OUT/GuestThunks/libvulkan-guest.so (clang, x86-64 sysroot)
#   probe     $OUT/x86/vkshim_test_x86_64: tests/elf/vk_shim_consumer.c built
#             for x86-64, linked by SONAME against libvulkan.so.1
#   install   (not part of "all") copy the two libraries, the probe, FEX's
#             ThunksDB.json and a Config.json enabling "Vulkan" into
#             $LXRT_ROOT/opt/vkthunks (default /tmp/lxrt-root, the test
#             root; the Steam root is refused). Nothing else in the root and
#             nothing in the shared $HOME/.fex-emu changes; undo with
#             rm -rf $LXRT_ROOT/opt/vkthunks. Then, for any x86-64 program:
#
#     FEX_APP_CONFIG_LOCATION=/opt/vkthunks/config/ \
#     FEX_THUNKHOSTLIBS=/opt/vkthunks/HostThunks \
#     FEX_THUNKGUESTLIBS=/opt/vkthunks/GuestThunks \
#         scripts/run-fex.sh /opt/vkthunks/x86/vkshim_test_x86_64
#
#             FEX then overlays the rootfs's /usr/lib/x86_64-linux-gnu/
#             libvulkan.so.1 with the guest thunk (benchmarks/stage10-vulkan-thunks.txt).
#
# Usage: scripts/build-fex-thunks.sh [source|x86root|thunkgen|gen|host|guest|probe|install|all]...
#
# Environment: STEAMARM_BUILD (default ~/SteamARM-build), LLVM_BIN (default
# /opt/homebrew/opt/llvm/bin), LLD (default ld.lld next to clang, else PATH), OPENSSL (default $(brew --prefix openssl@3)).
# Needs the Fedora sysroot of scripts/build-fex-host.sh (step sysroot) and
# $WORK/FEX (step source there); the thunks run on the FEX it built.
set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WORK="${STEAMARM_BUILD:-$HOME/SteamARM-build}"
MAIN="$WORK/FEX"
SRC="$WORK/FEX-thunks"
OUT="$WORK/thunks-build"
DEBDIR="$WORK/debs"
FEDORA_SYSROOT="$WORK/sysroot-f43"
X86ROOT="$OUT/sysroot-x86_64"
X86ROOT32="$OUT/sysroot-i386"
XINC="$OUT/xinclude"
LLVM_BIN="${LLVM_BIN:-/opt/homebrew/opt/llvm/bin}"
LLVM_PREFIX="$(cd "$LLVM_BIN/.." && pwd)"
if [ -z "${LLD:-}" ]; then
    if [ -x "$LLVM_BIN/ld.lld" ]; then LLD="$LLVM_BIN/ld.lld"; else LLD="$(command -v ld.lld || true)"; fi
fi
OPENSSL="${OPENSSL:-$(brew --prefix openssl@3 2>/dev/null || echo /opt/homebrew/opt/openssl@3)}"

FEX_COMMIT=08f451d3bce62a68aa6307dced5a8eaeb85c39bc      # same pin as build-fex-host.sh
PATCHES=(fex-lxrt-x18.patch fex-lxrt-guest-base.patch fex-lxrt-wx.patch fex-thunkgen-macos.patch fex-thunks-guestbase32.patch fex-thunkgen-32bit.patch)
SUBMODULES=(External/fmt External/Vulkan-Headers)

UBUNTU=https://archive.ubuntu.com/ubuntu
# file-in-pool  sha256    (Ubuntu 24.04 noble / noble-updates, 2026-09-26; the
# rootfs runs libc6 2.39-0ubuntu8.8 and libstdc++6 14.2.0-4ubuntu2~24.04.1)
DEBS=(
  "main/g/glibc/libc6_2.39-0ubuntu8.9_amd64.deb                              ff5557d99b51f761c4b7c92368b9cc45565eda17df9bf9eb4b134d09825008be"
  "main/g/glibc/libc6-dev_2.39-0ubuntu8.9_amd64.deb                          e13d5fcc1b2a86f75bca8e0026a8e39f24fe97ca86e92be79991f8697eb1306f"
  "main/l/linux/linux-libc-dev_6.8.0-142.142_amd64.deb                       937db1a88a4fa2ea97fd4eab89f2cd9d077290f6a26bcd27b1a6d24fa3d706b6"
  "main/g/gcc-14/libstdc++6_14.2.0-4ubuntu2~24.04.1_amd64.deb                a51f8de7829211db961a31f02158058ad1a95f92ac6d0a5dff6350e2821c54c0"
  "universe/g/gcc-14/libstdc++-14-dev_14.2.0-4ubuntu2~24.04.1_amd64.deb      6d223262b8cbe6d5150435056d58dffa8159d8582fc0df47fb45ea550cd37424"
  "main/g/gcc-14/libgcc-14-dev_14.2.0-4ubuntu2~24.04.1_amd64.deb             055dfa5fa7448fe20ad2f3832df2edd2b0ffca7f8a6c5fbbc5576dc61b2eb8b7"
  "main/g/gcc-14/libgcc-s1_14.2.0-4ubuntu2~24.04.1_amd64.deb                 aa7fadbe33b78bcf99885318040601c550c208929565b179891d9a3cc2aa68cd"
  "main/libx/libx11/libx11-dev_1.8.7-1build1_amd64.deb                       1969e200607ffe34070b92fe1bd3ba76ac2e2bd2f3a54530800c5872076fad24"
  "main/x/xorgproto/x11proto-dev_2023.2-1_all.deb                            3bf13a2ffb79ecd4014d438936014e5863f79eb59ebf76e8f43356271c90e7ce"
  "main/libx/libxcb/libxcb1-dev_1.15-1ubuntu2_amd64.deb                      1bafe3432feafc9e57f858721da524dfb3ee1f6fc4ef6b0c73023e79eadb9c28"
  "main/libx/libxrandr/libxrandr-dev_1.5.2-2build1_amd64.deb                 8c0ee697a9052f36103529ebe4ace096da845e2b517f39d206302f3c7709ec6f"
  "main/libx/libxrender/libxrender-dev_0.9.10-1.1build1_amd64.deb            839558c9de049636e13e22e409c04f3226614abf176c4a3b7088058576d74848"
)

# The same pins for i386: the 32-bit guest thunk (Vulkan for 32-bit games in
# Proton's i386 Wine) is built against these.
DEBS32=(
  "main/g/glibc/libc6_2.39-0ubuntu8.9_i386.deb                               5dd733076cd5490c912c663f1e844b1d0a214301d9515708c236e6fbeb38ff8e"
  "main/g/glibc/libc6-dev_2.39-0ubuntu8.9_i386.deb                           b62a3ce1f4f044d9c100dc72f555de82c8a30f68f5888aecf80fa6a82d9089c8"
  "main/l/linux/linux-libc-dev_6.8.0-142.142_i386.deb                        65330ef0d2c8fe8dc123a575937ae083f6cd8e0d166bb39b3d4ca2422ef5d22a"
  "main/g/gcc-14/libstdc++6_14.2.0-4ubuntu2~24.04.1_i386.deb                 899d77d8b4876f545bd362f60cb4aa0263a8033777e9ecca4742c890c4c54a68"
  "universe/g/gcc-14/libstdc++-14-dev_14.2.0-4ubuntu2~24.04.1_i386.deb       091d52807b50ad4321a580ddad29fbc0f524ce564825c8c965b42d5f197bf48b"
  "main/g/gcc-14/libgcc-14-dev_14.2.0-4ubuntu2~24.04.1_i386.deb              b7e90e0d45b860bce7f5fd9e49d33d433d53a4c5bbfe34a3429f2c193f7f2327"
  "main/g/gcc-14/libgcc-s1_14.2.0-4ubuntu2~24.04.1_i386.deb                  40879f5dae85f90837ce882527bb85e388fa0e57f7ec4fba2060f1d24ce3dbb7"
)

log() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
sha256() { shasum -a 256 "$1" | cut -d' ' -f1; }

check_tools() {
    local t
    for t in "$LLVM_BIN/clang" "$LLVM_BIN/clang++" "$LLD" "$LLVM_BIN/llvm-readelf"; do
        [ -x "$t" ] || die "missing $t (brew install llvm lld)"
    done
    [ -f "$LLVM_PREFIX/lib/libclang-cpp.dylib" ] || die "no libclang-cpp in $LLVM_PREFIX/lib"
    [ -f "$OPENSSL/include/openssl/sha.h" ] || die "no OpenSSL headers in $OPENSSL (brew install openssl@3)"
    for t in git curl tar shasum zstd xz; do command -v "$t" >/dev/null || die "missing $t"; done
}

# ------------------------------------------------------------------- source
do_source() {
    log "== source: worktree $SRC at $FEX_COMMIT"
    [ -d "$MAIN/.git" ] || die "no FEX clone at $MAIN (scripts/build-fex-host.sh source)"
    if [ ! -e "$SRC/.git" ]; then
        git -C "$MAIN" worktree add --detach "$SRC" "$FEX_COMMIT"
    fi
    local p want have=""
    want="$( { echo "$FEX_COMMIT"; for p in "${PATCHES[@]}"; do cat "$REPO/patches/$p"; done
               cat "$REPO/patches/LxrtJit.h"; } | shasum -a 256 | cut -d' ' -f1)"
    [ -f "$OUT/.patched" ] && have="$(cat "$OUT/.patched")"
    if [ "$have" = "$want" ] && [ "$(git -C "$SRC" rev-parse HEAD)" = "$FEX_COMMIT" ]; then
        log "  tree at $FEX_COMMIT with the current patches"
    else
        git -C "$SRC" checkout -q -f --detach "$FEX_COMMIT"
        git -C "$SRC" clean -fdxq
        for p in "${PATCHES[@]}"; do
            log "  apply   patches/$p"
            git -C "$SRC" apply --whitespace=nowarn "$REPO/patches/$p"
        done
        cp "$REPO/patches/LxrtJit.h" "$SRC/FEXCore/include/FEXCore/Utils/LxrtJit.h"
        mkdir -p "$OUT"; echo "$want" > "$OUT/.patched"
    fi
    git -C "$SRC" submodule update --init --depth 1 -- "${SUBMODULES[@]}" \
        || git -C "$SRC" submodule update --init -- "${SUBMODULES[@]}"
    log "  Vulkan-Headers $(git -C "$SRC/External/Vulkan-Headers" log -1 --format=%s)"
}

# ------------------------------------------------------------------ x86root
fetch_deb() {   # pool-path sha256
    local path="$1" sum="$2" f dest
    f="$(basename "$path")"; dest="$DEBDIR/$f"
    if [ -f "$dest" ] && [ "$(sha256 "$dest")" = "$sum" ]; then log "  cached  $f"; return 0; fi
    log "  fetch   $UBUNTU/pool/$path"
    curl -fsSL --retry 3 --connect-timeout 20 -o "$dest.part" "$UBUNTU/pool/$path" \
        || die "download failed: $path"
    [ "$(sha256 "$dest.part")" = "$sum" ] || die "sha256 mismatch: $f"
    mv "$dest.part" "$dest"
}

do_x86root() { mk_sysroot "$X86ROOT" x86_64-linux-gnu DEBS; }
do_x86root32() { mk_sysroot "$X86ROOT32" i386-linux-gnu DEBS32; }

# mk_sysroot <dir> <triplet> <name of the deb array>
mk_sysroot() {
    local X86ROOT="$1" triplet="$2"
    local debs=()
    eval "debs=(\"\${$3[@]}\")"          # bash 3.2 (macOS): no namerefs
    log "== x86root: Ubuntu 24.04 $triplet dev sysroot -> $X86ROOT"
    mkdir -p "$DEBDIR"
    local e want
    for e in "${debs[@]}"; do
        # shellcheck disable=SC2086
        set -- $e; fetch_deb "$1" "$2"
    done
    want="$(printf '%s\n' "${debs[@]}" | shasum -a 256 | cut -d' ' -f1)"
    if [ -f "$X86ROOT/.lxrt-sysroot" ] && [ "$(cat "$X86ROOT/.lxrt-sysroot")" = "$want" ]; then
        log "  sysroot up to date"
    else
        local tmp="$X86ROOT.tmp" f member
        rm -rf "$tmp"; mkdir -p "$tmp"
        for e in "${debs[@]}"; do
            # shellcheck disable=SC2086
            set -- $e; f="$DEBDIR/$(basename "$1")"
            member="$(tar -tf "$f" | grep '^data\.tar')"
            log "  unpack  $(basename "$1")"
            tar -xOf "$f" "$member" | tar -xf - -C "$tmp" --exclude './usr/share/*' 2>/dev/null \
                || tar -xOf "$f" "$member" | tar -xf - -C "$tmp" --exclude './usr/share/*'
        done
        # usrmerge, as on the rootfs; absolute symlinks made relative.
        local d
        for d in bin sbin lib lib64 lib32; do
            if [ -d "$tmp/$d" ] && [ ! -L "$tmp/$d" ]; then
                mkdir -p "$tmp/usr/$d"; cp -a "$tmp/$d/." "$tmp/usr/$d/"; rm -rf "${tmp:?}/$d"
            fi
            [ -e "$tmp/usr/$d" ] && { [ -L "$tmp/$d" ] || ln -s "usr/$d" "$tmp/$d"; }
        done
        python3 - "$tmp" <<'PY'
import os, sys
root = sys.argv[1]; n = 0
for dp, dns, fns in os.walk(root):
    for name in dns + fns:
        p = os.path.join(dp, name)
        if os.path.islink(p) and os.readlink(p).startswith('/'):
            t = os.readlink(p); os.unlink(p)
            os.symlink(os.path.relpath(os.path.join(root, t.lstrip('/')), dp), p); n += 1
print(f"  {n} absolute symlinks made relative")
PY
        [ -f "$tmp/usr/include/stdio.h" ] && [ -f "$tmp/usr/lib/$triplet/crti.o" ] \
            && ls "$tmp"/usr/lib/gcc/*/14/crtbeginS.o >/dev/null \
            || die "$triplet sysroot incomplete"
        echo "$want" > "$tmp/.lxrt-sysroot"
        rm -rf "$X86ROOT"; mv "$tmp" "$X86ROOT"
    fi
    # X11/xcb headers are architecture-independent: the host (aarch64) build
    # takes them from here too, without the x86 glibc next to them. (The
    # i386 sysroot has no X11 packages; it uses the same copies.)
    if [ "$triplet" = x86_64-linux-gnu ]; then
        rm -rf "$XINC"; mkdir -p "$XINC"
        cp -a "$X86ROOT/usr/include/X11" "$X86ROOT/usr/include/xcb" "$XINC/"
    fi
    log "  $triplet sysroot ready: $(du -sh "$X86ROOT" | cut -f1)"
}

# ----------------------------------------------------------------- thunkgen
do_thunkgen() {
    log "== thunkgen (macOS arm64, libclang $("$LLVM_BIN/llvm-config" --version))"
    local g="$SRC/ThunkLibs/Generator" o="$OUT/thunkgen" f objs=()
    mkdir -p "$o"
    local resdir; resdir=$("$LLVM_BIN/clang" -print-resource-dir)
    local cxx=("$LLVM_BIN/clang++" -std=c++20 -O2 -fno-rtti
        -isystem "$LLVM_PREFIX/include" -I "$SRC/External/fmt/include" -DFMT_HEADER_ONLY
        -I "$OPENSSL/include" -DCLANG_RESOURCE_DIR="\"$resdir\"")
    # The objects carry clang's resource directory, a versioned Homebrew path
    # (Cellar/llvm/23.1.2/lib/clang/23): after a Homebrew LLVM upgrade the old
    # one is gone, and thunkgen found no stddef.h. A new LLVM compiles them
    # again.
    local stamp="$resdir $("$LLVM_BIN/llvm-config" --version)"
    if [ "$(cat "$o/.llvm" 2>/dev/null)" != "$stamp" ]; then
        rm -f "$o"/*.o
        echo "$stamp" > "$o/.llvm"
    fi
    for f in main analysis data_layout gen; do
        if [ ! -f "$o/$f.o" ] || [ "$g/$f.cpp" -nt "$o/$f.o" ]; then
            log "  cc      $f.cpp"
            "${cxx[@]}" -c "$g/$f.cpp" -o "$o/$f.o"
        fi
        objs+=("$o/$f.o")
    done
    "$LLVM_BIN/clang++" -o "$o/thunkgen" "${objs[@]}" -L"$LLVM_PREFIX/lib" -lclang-cpp -lLLVM \
        -Wl,-rpath,"$LLVM_PREFIX/lib" "$OPENSSL/lib/libcrypto.dylib"
    log "  $o/thunkgen"
}

# ---------------------------------------------------------------------- gen
TL() { echo "$SRC/ThunkLibs"; }
do_gen() {
    local tl; tl="$(TL)"
    local iface="$tl/libvulkan/libvulkan_interface.cpp" tg="$OUT/thunkgen/thunkgen"
    local incs=(-isystem "$tl/include" -isystem "$SRC/External/Vulkan-Headers/include")
    mkdir -p "$OUT/gen" "$OUT/gen_64"
    log "== gen: guest (x86-64)"
    # The x86 rootfs argument is also used for the data-layout pass of both runs.
    "$tg" "$iface" libvulkan -guest "$OUT/gen/thunkgen_guest_libvulkan.inl" "$X86ROOT" -- \
        -std=c++20 --target=x86_64-linux-gnu --sysroot "$X86ROOT" "${incs[@]}"
    log "== gen: host (aarch64-linux)"
    # Upstream runs this pass natively on an aarch64 Linux host; here the host
    # ABI is chosen explicitly (without --target libclang would parse as
    # arm64-apple-darwin). No --gcc-install-dir: the guest data-layout pass of
    # this run inherits these flags and must find the x86 libstdc++, not
    # Fedora's; clang finds each GCC install through --sysroot on its own.
    "$tg" "$iface" libvulkan -host "$OUT/gen_64/thunkgen_host_libvulkan.inl" "$X86ROOT" -- \
        -std=c++20 --target=aarch64-redhat-linux-gnu --sysroot "$FEDORA_SYSROOT" \
        -DARCHITECTURE_arm64=1 "${incs[@]}" -isystem "$XINC"
    wc -l "$OUT"/gen/*.inl "$OUT"/gen_64/*.inl | sed 's/^/  /'
}

# --------------------------------------------------------------------- host
do_host() {
    local tl gccdir; tl="$(TL)"
    gccdir="$(ls -d "$FEDORA_SYSROOT"/usr/lib/gcc/aarch64-redhat-linux/* | sort -V | tail -n 1)"
    mkdir -p "$OUT/HostThunks"
    log "== host: libvulkan-host.so (aarch64 Linux ELF)"
    # -ffixed-x18 as FEX itself (Darwin zeroes x18: benchmarks/stage5-x18.txt);
    # this code calls straight into MoltenVK.
    "$LLVM_BIN/clang++" --target=aarch64-redhat-linux-gnu --sysroot="$FEDORA_SYSROOT" \
        --gcc-install-dir="$gccdir" -std=c++20 -O2 -g0 -fPIC -fwrapv -ffixed-x18 \
        -DARCHITECTURE_arm64=1 -I "$OUT/gen_64" -isystem "$tl/include" \
        -isystem "$SRC/External/Vulkan-Headers/include" -isystem "$XINC" \
        -shared -fuse-ld=lld --ld-path="$LLD" -Wl,--build-id=sha1 -Wl,--no-undefined \
        -Wl,-soname,libvulkan-host.so \
        -o "$OUT/HostThunks/libvulkan-host.so" "$tl/libvulkan/Host.cpp" -ldl
    "$LLVM_BIN/llvm-readelf" -h -d "$OUT/HostThunks/libvulkan-host.so" | grep -E 'Machine|NEEDED' | sed 's/^/  /'
}

# -------------------------------------------------------------------- guest
do_guest() {
    local tl; tl="$(TL)"
    mkdir -p "$OUT/GuestThunks"
    log "== guest: libvulkan-guest.so (x86-64 Linux ELF, SONAME libvulkan.so.1)"
    "$LLVM_BIN/clang++" --target=x86_64-linux-gnu --sysroot="$X86ROOT" \
        -std=c++20 -O2 -g0 -fPIC -fwrapv -msse2 -mfpmath=sse -DGUEST_THUNK_LIBRARY \
        -I "$OUT/gen" -isystem "$tl/include" -isystem "$SRC/External/Vulkan-Headers/include" \
        -shared -fuse-ld=lld --ld-path="$LLD" -Wl,--build-id=sha1 \
        -Wl,-soname,libvulkan.so.1 -Wl,-z,nodelete \
        -o "$OUT/GuestThunks/libvulkan-guest.so" "$tl/libvulkan/Guest.cpp"
    "$LLVM_BIN/llvm-readelf" -h -d "$OUT/GuestThunks/libvulkan-guest.so" | grep -E 'Machine|NEEDED|SONAME' | sed 's/^/  /'
}


# ------------------------------------------------------ 32-bit guest thunks
# Vulkan for 32-bit guests (Proton runs 32-bit Windows programs in an i386
# Wine; FEX here cannot run new-WoW64 Wine, MEASURED: "Trying to execute
# 32-bit syscall from a 64-bit process"). thunkgen's -for-32bit-guest makes
# the host side repack every structure between the i386 and the aarch64
# layout; the host library is then a separate build (HostThunks_32).
do_gen32() {
    local tl; tl="$(TL)"
    local iface="$tl/libvulkan/libvulkan_interface.cpp" tg="$OUT/thunkgen/thunkgen"
    local incs=(-isystem "$tl/include" -isystem "$SRC/External/Vulkan-Headers/include")
    mkdir -p "$OUT/gen32" "$OUT/gen_32"
    log "== gen32: guest (i386)"
    THUNKGEN_SKIP_UNREPACKABLE=1 "$tg" "$iface" libvulkan -guest "$OUT/gen32/thunkgen_guest_libvulkan.inl" "$X86ROOT32" -for-32bit-guest -- \
        -std=c++20 -m32 --target=i686-linux-gnu --sysroot "$X86ROOT32" -DIS_32BIT_THUNK "${incs[@]}" -isystem "$XINC"
    log "== gen32: host (aarch64-linux, for i386 guests)"
    THUNKGEN_SKIP_UNREPACKABLE=1 "$tg" "$iface" libvulkan -host "$OUT/gen_32/thunkgen_host_libvulkan.inl" "$X86ROOT32" -for-32bit-guest -- \
        -std=c++20 --target=aarch64-redhat-linux-gnu --sysroot "$FEDORA_SYSROOT" \
        -DARCHITECTURE_arm64=1 -DIS_32BIT_THUNK "${incs[@]}" -isystem "$XINC"
    wc -l "$OUT"/gen32/*.inl "$OUT"/gen_32/*.inl | sed 's/^/  /'
}

do_host32() {
    local tl gccdir; tl="$(TL)"
    gccdir="$(ls -d "$FEDORA_SYSROOT"/usr/lib/gcc/aarch64-redhat-linux/* | sort -V | tail -n 1)"
    mkdir -p "$OUT/HostThunks_32"
    log "== host32: HostThunks_32/libvulkan-host.so (aarch64, for i386 guests)"
    "$LLVM_BIN/clang++" --target=aarch64-redhat-linux-gnu --sysroot="$FEDORA_SYSROOT" \
        --gcc-install-dir="$gccdir" -std=c++20 -O2 -g0 -fPIC -fwrapv -ffixed-x18 \
        -DARCHITECTURE_arm64=1 -DIS_32BIT_THUNK -I "$OUT/gen_32" -isystem "$tl/include" \
        -isystem "$SRC/External/Vulkan-Headers/include" -isystem "$XINC" \
        -shared -fuse-ld=lld --ld-path="$LLD" -Wl,--build-id=sha1 -Wl,--no-undefined \
        -Wl,-soname,libvulkan-host.so \
        -o "$OUT/HostThunks_32/libvulkan-host.so" "$tl/libvulkan/Host.cpp" -ldl
    "$LLVM_BIN/llvm-readelf" -h "$OUT/HostThunks_32/libvulkan-host.so" | grep -E 'Machine' | sed 's/^/  /'
}

do_guest32() {
    local tl; tl="$(TL)"
    mkdir -p "$OUT/GuestThunks_32"
    log "== guest32: GuestThunks_32/libvulkan-guest.so (i386 Linux ELF, SONAME libvulkan.so.1)"
    local lds=()
    [ -f "$tl/libvulkan/libvulkan_Guest_32.lds" ] && lds=(-Wl,-T,"$tl/libvulkan/libvulkan_Guest_32.lds")
    "$LLVM_BIN/clang++" -m32 --target=i686-linux-gnu --sysroot="$X86ROOT32" \
        -std=c++20 -O2 -g0 -fPIC -fwrapv -msse2 -mfpmath=sse -DGUEST_THUNK_LIBRARY -DIS_32BIT_THUNK \
        -I "$OUT/gen32" -isystem "$tl/include" -isystem "$SRC/External/Vulkan-Headers/include" -isystem "$XINC" \
        -shared -fuse-ld=lld --ld-path="$LLD" -Wl,--build-id=sha1 ${lds[@]+"${lds[@]}"} \
        -Wl,-soname,libvulkan.so.1 -Wl,-z,nodelete \
        -o "$OUT/GuestThunks_32/libvulkan-guest.so" "$tl/libvulkan/Guest.cpp"
    "$LLVM_BIN/llvm-readelf" -h -d "$OUT/GuestThunks_32/libvulkan-guest.so" | grep -E 'Machine|NEEDED|SONAME' | sed 's/^/  /'
}

# -------------------------------------------------------------------- probe
do_probe() {
    mkdir -p "$OUT/x86"
    log "== probe: tests/elf/vk_shim_consumer.c -> x86-64"
    "$LLVM_BIN/clang" --target=x86_64-linux-gnu --sysroot="$X86ROOT" -O2 -fPIE -pie \
        -fuse-ld=lld --ld-path="$LLD" -o "$OUT/x86/vkshim_test_x86_64" \
        "$REPO/tests/elf/vk_shim_consumer.c" "$OUT/GuestThunks/libvulkan-guest.so"
}

# ------------------------------------------------------------------ install
do_install() {
    local root="${LXRT_ROOT:-/tmp/lxrt-root}" real
    real="$(cd "$root" && pwd -P)" || die "no root $root"
    # The Steam root only on request (STEAMARM_THUNKS_STEAMROOT=1): it is
    # what the live Steam client runs from.
    case "$real" in *steamroot*)
        [ "${STEAMARM_THUNKS_STEAMROOT:-0}" = 1 ] ||
            die "refusing to install into the Steam root ($real); STEAMARM_THUNKS_STEAMROOT=1 to allow" ;;
    esac
    local d="$root/opt/vkthunks"
    log "== install -> $d"
    mkdir -p "$d/HostThunks" "$d/GuestThunks" "$d/config" "$d/x86"
    cp "$OUT/HostThunks/libvulkan-host.so" "$d/HostThunks/"
    cp "$OUT/GuestThunks/libvulkan-guest.so" "$d/GuestThunks/"
    [ -f "$OUT/x86/vkshim_test_x86_64" ] && cp "$OUT/x86/vkshim_test_x86_64" "$d/x86/"
    cp "$SRC/Data/ThunksDB.json" "$d/config/ThunksDB.json"
    # FEX_APP_CONFIG_LOCATION replaces the user config dir (the shared
    # /tmp/fexhome/.fex-emu), so the RootFS is restated here.
    printf '%s\n' '{"Config":{"RootFS":"/tmp/fexhome/.local/share/fex-emu/RootFS/Ubuntu_24_04"},"ThunksDB":{"Vulkan":1}}' \
        > "$d/config/Config.json"
    find "$d" -type f | sed 's/^/  /'
}

summary() {
    log "== $OUT"
    local f
    for f in "$OUT/thunkgen/thunkgen" "$OUT/HostThunks/libvulkan-host.so" "$OUT/GuestThunks/libvulkan-guest.so" "$OUT/x86/vkshim_test_x86_64"; do
        [ -f "$f" ] || continue
        printf '  %-22s %9s bytes  sha256 %s\n' "$(basename "$f")" "$(stat -f %z "$f")" "$(sha256 "$f")"
    done
}

main() {
    [ $# -gt 0 ] || set -- all
    check_tools
    local step
    for step in "$@"; do
        case "$step" in
            source)   do_source ;;
            x86root)  do_x86root ;;
            thunkgen) do_thunkgen ;;
            gen)      do_gen ;;
            host)     do_host ;;
            guest)    do_guest ;;
            x86root32) do_x86root32 ;;
            gen32)    do_gen32 ;;
            host32)   do_host32 ;;
            guest32)  do_guest32 ;;
            probe)    do_probe ;;
            install)  do_install ;;
            all)      do_source; do_x86root; do_x86root32; do_thunkgen; do_gen; do_host; do_guest
                      do_gen32; do_host32; do_guest32; do_probe ;;
            *) die "unknown step '$step' (source|x86root|x86root32|thunkgen|gen|host|guest|gen32|host32|guest32|probe|install|all)" ;;
        esac
    done
    summary
}
main "$@"
