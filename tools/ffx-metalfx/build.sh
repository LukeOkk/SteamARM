#!/bin/bash
# Build SteamARM's amd_fidelityfx_dx12.dll stub (ffx_stub.c) as a Wine
# builtin, in the layout WINEDLLPATH expects:
#
#   tools/ffx-metalfx/build.sh [OUTDIR]     (default build/ffx-metalfx)
#     -> OUTDIR/x86_64-windows/amd_fidelityfx_dx12.dll     WINEDLLPATH=OUTDIR
#
# FFX_IDENT=name     what the DLL answers to SA_FFX_QUERY_IDENT (default steamarm-builtin)
# FFX_MARK=0         no builtin marker: a plain native DLL (a test's "game" copy)
# FFX_PREFER_NATIVE=0  without Wine's prefer-native flag (default 1: the flag is
#                    set, so the builtin replaces a game's copy only where the
#                    load order says b, WINEDLLOVERRIDES=amd_fidelityfx_dx12=b,n;
#                    without it Wine's default load order already takes the
#                    builtin over the game's file -- tests/win/run_ffx_load.sh)
# FFX_OUT=file       write exactly this file instead
#
# Needs mingw-w64 (x86_64-w64-mingw32-gcc, Homebrew) and python3.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
cd "$here/../.."
out=${1:-build/ffx-metalfx}
dll=${FFX_OUT:-$out/x86_64-windows/amd_fidelityfx_dx12.dll}
CC=${CC:-x86_64-w64-mingw32-gcc}
command -v "$CC" >/dev/null || CC=/opt/homebrew/bin/x86_64-w64-mingw32-gcc
mkdir -p "$(dirname "$dll")"
# KERNEL32 only: no C runtime, so DllMain is the entry point.
"$CC" -O2 -shared -s -nostdlib -ffreestanding -fno-stack-protector \
    -DFFX_IDENT="\"${FFX_IDENT:-steamarm-builtin}\"" \
    -Wl,-e,DllMain -o "$dll" "$here/ffx_stub.c" "$here/ffx_stub.def" -lkernel32
if [ "${FFX_MARK:-1}" = 1 ]; then
    flags=()
    [ "${FFX_PREFER_NATIVE:-1}" = 1 ] && flags+=(--prefer-native)
    python3 "$here/wine-builtin-mark.py" ${flags[@]+"${flags[@]}"} "$dll"
fi
python3 "$here/wine-builtin-mark.py" --check "$dll"
