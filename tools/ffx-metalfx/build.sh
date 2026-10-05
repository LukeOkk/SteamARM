#!/bin/bash
# Build SteamARM's FidelityFX API DLLs (ffx_stub.c) as Wine builtins, in the
# layout WINEDLLPATH expects: one source, one DLL per name AMD ships.
#
#   tools/ffx-metalfx/build.sh [OUTDIR]     (default build/ffx-metalfx)
#     -> OUTDIR/x86_64-windows/amd_fidelityfx_dx12.dll           WINEDLLPATH=OUTDIR
#        OUTDIR/x86_64-windows/amd_fidelityfx_vk.dll
#        OUTDIR/x86_64-windows/amd_fidelityfx_upscaler_dx12.dll
#   (make ffx-metalfx builds them; scripts/install-ffx-metalfx.sh puts them in
#   the x86 Steam root's /opt/steamarm/wine.)
#
# Each DLL is the same source built with FFX_DLL_NAME (its name without .dll,
# a C string) defined and the export table's LIBRARY name set to its file name.
#
# FFX_NAMES="a b"    the names to build (default the three above)
# FFX_IDENT=name     what the DLL answers to SA_FFX_QUERY_IDENT (default steamarm-builtin)
# FFX_MARK=0         no builtin marker: a plain native DLL (a test's "game" copy)
# FFX_PREFER_NATIVE=0  without Wine's prefer-native flag (default 1: the flag is
#                    set, so the builtin replaces a game's copy only where the
#                    load order says b, WINEDLLOVERRIDES=amd_fidelityfx_dx12=b,n;
#                    without it Wine's default load order already takes the
#                    builtin over the game's file -- tests/win/run_ffx_load.sh)
# FFX_OUT=file       write exactly this one file instead (its name, without
#                    .dll, is the DLL's name)
#
# Needs mingw-w64 (x86_64-w64-mingw32-gcc, Homebrew) and python3.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/../.."
out=${1:-build/ffx-metalfx}
CC=${CC:-x86_64-w64-mingw32-gcc}
command -v "$CC" >/dev/null || CC=/opt/homebrew/bin/x86_64-w64-mingw32-gcc
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

build_one() {   # build_one FILE: the DLL named after FILE
    local dll=$1 name def
    name=${dll##*/}; name=${name%.[dD][lL][lL]}
    def="$tmp/$name.def"
    sed "s/^LIBRARY .*/LIBRARY $name.dll/" "$here/ffx_stub.def" > "$def"
    mkdir -p "$(dirname "$dll")"
    # KERNEL32 only: no C runtime, so DllMain is the entry point.
    "$CC" -O2 -shared -s -nostdlib -ffreestanding -fno-stack-protector \
        -DFFX_IDENT="\"${FFX_IDENT:-steamarm-builtin}\"" -DFFX_DLL_NAME="\"$name\"" \
        -Wl,-e,DllMain -o "$dll" "$here/ffx_stub.c" "$def" -lkernel32
    if [ "${FFX_MARK:-1}" = 1 ]; then
        local flags=()
        [ "${FFX_PREFER_NATIVE:-1}" = 1 ] && flags+=(--prefer-native)
        python3 "$here/wine-builtin-mark.py" ${flags[@]+"${flags[@]}"} "$dll"
    fi
    python3 "$here/wine-builtin-mark.py" --check "$dll"
}

if [ -n "${FFX_OUT:-}" ]; then
    build_one "$FFX_OUT"
else
    for n in ${FFX_NAMES:-amd_fidelityfx_dx12 amd_fidelityfx_vk amd_fidelityfx_upscaler_dx12}; do
        build_one "$out/x86_64-windows/$n.dll"
    done
fi
