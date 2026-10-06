#!/bin/bash
# Build SteamARM's NGX DLLs (nvngx.c: DLSS -> MetalFX temporal) as Wine
# builtins, in the layout WINEDLLPATH expects: one source, the two names
# NVIDIA's NGX goes by.
#
#   tools/dlss-metalfx/build.sh [OUTDIR]     (default build/dlss-metalfx)
#     -> OUTDIR/x86_64-windows/nvngx.dll             WINEDLLPATH=OUTDIR
#        OUTDIR/x86_64-windows/_nvngx.dll
#   (make dlss-metalfx builds them; scripts/install-ffx-metalfx.sh puts them
#   in the x86 Steam root's /opt/steamarm/wine with the FidelityFX DLLs.)
#
# Each DLL is the same source built with SA_NGX_DLL_NAME (its name without
# .dll, a C string) defined and the export table's LIBRARY name set to its
# file name. The same marker and flags as tools/ffx-metalfx/build.sh:
#
# NGX_NAMES="a b"    the names to build (default "nvngx _nvngx")
# NGX_IDENT=name     what SteamARM_NGX_Ident answers (default steamarm-nvngx-metalfx)
# NGX_MARK=0         no builtin marker: a plain native DLL
# NGX_PREFER_NATIVE=0  without Wine's prefer-native flag (default 1: Wine takes
#                    the builtin over a file of that name only where the load
#                    order says b, WINEDLLOVERRIDES=nvngx,_nvngx=b,n)
# NGX_OUT=file       write exactly this one file instead
#
# Needs mingw-w64 (x86_64-w64-mingw32-gcc, Homebrew), the Vulkan headers
# (Homebrew vulkan-headers) and python3.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/../.."
out=${1:-build/dlss-metalfx}
CC=${CC:-x86_64-w64-mingw32-gcc}
command -v "$CC" >/dev/null || CC=/opt/homebrew/bin/x86_64-w64-mingw32-gcc
VK_HEADERS="${VK_HEADERS:-$(brew --prefix vulkan-headers 2>/dev/null || echo /opt/homebrew/opt/vulkan-headers)}"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

build_one() {   # build_one FILE: the DLL named after FILE
    local dll=$1 name def
    name=${dll##*/}; name=${name%.[dD][lL][lL]}
    def="$tmp/$name.def"
    sed "s/^LIBRARY .*/LIBRARY $name.dll/" "$here/nvngx.def" > "$def"
    mkdir -p "$(dirname "$dll")"
    # KERNEL32 only: no C runtime, so DllMain is the entry point; libgcc
    # for the compiler's own helpers (static, no DLL behind it).
    "$CC" -O2 -shared -s -nostdlib -ffreestanding -fno-stack-protector -fno-builtin \
        -Wall -Wextra -Wno-unused-function -I"$VK_HEADERS/include" \
        -DSA_NGX_IDENT="\"${NGX_IDENT:-steamarm-nvngx-metalfx}\"" -DSA_NGX_DLL_NAME="\"$name\"" \
        -Wl,-e,DllMain -o "$dll" "$here/nvngx.c" "$def" -lkernel32 -lgcc
    if [ "${NGX_MARK:-1}" = 1 ]; then
        local flags=()
        [ "${NGX_PREFER_NATIVE:-1}" = 1 ] && flags+=(--prefer-native)
        python3 tools/ffx-metalfx/wine-builtin-mark.py ${flags[@]+"${flags[@]}"} "$dll"
    fi
    python3 tools/ffx-metalfx/wine-builtin-mark.py --check "$dll"
}

if [ -n "${NGX_OUT:-}" ]; then
    build_one "$NGX_OUT"
else
    for n in ${NGX_NAMES:-nvngx _nvngx}; do
        build_one "$out/x86_64-windows/$n.dll"
    done
fi
