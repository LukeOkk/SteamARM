#!/bin/bash
# SteamARM's FidelityFX API DLL (tools/ffx-metalfx) used by a D3D12 program
# the way a game uses it (tests/win/d3d12_ffx.c), under Proton Experimental
# with no VM, through vkd3d-proton's interop, the Vulkan shim and
# KosmicKrisp's MetalFX temporal entry -- and AMD's real DLL for comparison.
#
#   tests/win/run_d3d12_ffx.sh                  metalfx forward amd blit
#   tests/win/run_d3d12_ffx.sh metalfx          one or more of them
#
# metalfx  STEAMARM_WIN_UPSCALER=metalfx, our builtin over the game's copy
#          (WINEDLLPATH + amd_fidelityfx_dx12=b,n): the upscale must be
#          MetalFX's (shim log: ENCODE st=OK), the queries identical to
#          AMD's DLL's (loaded in the same process under another name).
# forward  the variable unset: our builtin forwards everything to the game's
#          copy; the output must be byte for byte AMD's (the amd run's, of
#          the same invocation: the two outputs are compared only when both
#          modes ran in this run, from this run's output directory).
# amd      no override: the game's copy (AMD's DLL) alone.
# blit     metalfx with a game copy that cannot be loaded (a stub DLL whose
#          DllMain says no, in a game directory of its own: our builtin is
#          mapped in its place as usual, then finds nothing to fall back
#          to) and SA_FFX_TEST_REFUSE=1, which makes the DLL send every
#          ENCODE malformed so the shim refuses it (BAD_ARGS): the DLL must
#          blit the colour input to the output instead (its log line), the
#          program must still find a picture there (RMSE under
#          FFX_BLIT_MAXERR: a bilinear upscale of the jittered input, see
#          the default) and every dispatch must return OK.
#
# The "game copy" is AMD's amd_fidelityfx_dx12.dll 1.1.4 from the FidelityFX
# SDK samples, expected in the x86 root at FFX_AMD_DLL (default
# /tmp/ffxsdk/v114/bin/amd_fidelityfx_dx12.dll, a guest path; never part of
# the repository). Without it only metalfx and blit can run, with no
# comparison.
#
# The shim under test is build/libvulkan.so.1 (MFXT_SHIM overrides), bound
# over the root's for this run's processes only (LXRT_MOUNTS); the DLLs are
# built into the root's /tmp/ffx-t4 by tools/ffx-metalfx/build.sh. Nothing is
# installed. KosmicKrisp: STEAMARM_KK_OWN, default
# ${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out (SteamARM's build,
# patch 20), copied privately for the run.
#
# FFX_PFX       guest prefix (default /tmp/fexhome/ffx-t4, its own)
# FFX_TIMEOUT   seconds per run (default 120; the program ends itself at
#               55 s); then only this run's processes are stopped (by their
#               STEAMARM_RUN_ID)
# FFX_FRAMES    frames per case (default 24)
# FFX_BLIT_MAXERR  the blit mode's RMSE limit (default 0.15: a bilinear blit
#               of an input jittered by up to half a pixel, against the
#               supersampled reference, MEASURED 0.0967 still and moving on
#               KosmicKrisp, benchmarks/stage59-ffx-metalfx-e2e.txt; MetalFX
#               gives 0.036/0.063, AMD's FSR 3.1 0.056/0.061)
#
# Needs: the Steam root with Proton Experimental and the Vulkan thunks,
# mingw-w64 and vulkan-headers (Homebrew).
set -u
cd "$(dirname "$0")/../.." || exit 1
. scripts/roots.sh
ROOT=/tmp/lxrt-steamroot
PFX_GUEST="${FFX_PFX:-/tmp/fexhome/ffx-t4}"
PROTON_GUEST="/tmp/fexhome/.local/share/Steam/steamapps/common/Proton - Experimental/files"
PROTON="$ROOT$PROTON_GUEST"
LOGS="${STEAMARM_STATE:-$HOME/SteamARM-roots}/logs"
TIMEOUT="${FFX_TIMEOUT:-120}"
FRAMES="${FFX_FRAMES:-24}"
BLIT_MAXERR="${FFX_BLIT_MAXERR:-0.15}"
SHIM="${MFXT_SHIM:-build/libvulkan.so.1}"
KK_DIR="${STEAMARM_KK_OWN:-${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out}"
AMD_DLL="${FFX_AMD_DLL:-/tmp/ffxsdk/v114/bin/amd_fidelityfx_dx12.dll}"
VK_HEADERS="$(brew --prefix vulkan-headers 2>/dev/null || echo /opt/homebrew/opt/vulkan-headers)"
T=/tmp/ffx-t4   # guest
mkdir -p "$LOGS"
[ -x "$PROTON/bin/wine" ] || { echo "no Proton Experimental in the Steam root" >&2; exit 1; }
[ -f "$ROOT/usr/lib/lxrt-emu/thunks/HostThunks/libvulkan-host.so" ] || { echo "no Vulkan thunks in the Steam root" >&2; exit 1; }
[ -f "$SHIM" ] || { echo "no $SHIM (make build/libvulkan.so.1)" >&2; exit 1; }
[ -f "$KK_DIR/libvulkan_kosmickrisp.dylib" ] || { echo "no $KK_DIR/libvulkan_kosmickrisp.dylib" >&2; exit 1; }
scripts/run-x11-native.sh start :2 >/dev/null || exit 1

RUN_ID="ffx-t4.$$.$(date +%s)"
ours() {   # this run's guests (by the STEAMARM_RUN_ID they inherit), FEXServer excepted
    local pids
    pids=$(ps -Ao pid=,comm= | awk '$NF ~ /(^|\/)lxrun$/ {print $1}')
    [ -n "$pids" ] || return 0
    # shellcheck disable=SC2086
    procs_env STEAMARM_RUN_ID $pids | awk -v id="$RUN_ID" '$2 == id { print $1 }' | while read -r p; do
        ps -o command= -p "$p" 2>/dev/null | grep -q FEXServer || printf "%s " "$p"
    done
}
stop_ours() {
    local o; o=$(ours)
    [ -n "$o" ] || return 0
    # shellcheck disable=SC2086
    kill -TERM $o 2>/dev/null; sleep 2
    o=$(ours)
    # shellcheck disable=SC2086
    [ -z "$o" ] || kill -KILL $o 2>/dev/null
}
PRIV=$(mktemp -d /tmp/steamarm-ffx-t4.XXXXXX) || exit 1
trap 'stop_ours; rm -rf "$PRIV"; exit 130' INT TERM
cp "$SHIM" "$PRIV/libvulkan.so.1"
mkdir -p "$PRIV/kk"
cp "$KK_DIR/libvulkan_kosmickrisp.dylib" "$PRIV/kk/"
RS=$'\x1e' US=$'\x1f'
MOUNTS="/usr/lib64/libvulkan.so.1${RS}$PRIV/libvulkan.so.1${RS}1${US}/usr/lib/lxrt-emu/libvulkan.so.1${RS}$PRIV/libvulkan.so.1${RS}1${US}"
echo "== shim under test: $SHIM (sha1 $(shasum "$PRIV/libvulkan.so.1" | cut -c1-12)), installed: $(shasum "$ROOT/lib64/libvulkan.so.1" | cut -c1-12)"
echo "== KosmicKrisp: $KK_DIR (sha1 $(shasum "$PRIV/kk/libvulkan_kosmickrisp.dylib" | cut -c1-12))"
echo "== vkd3d-proton: $(cat "$PROTON/lib/wine/vkd3d-proton/version" 2>/dev/null)"

SYS32="$ROOT$PFX_GUEST/drive_c/windows/system32"
if [ ! -d "$SYS32" ]; then
    echo "== new prefix $PFX_GUEST"
    STEAMARM_RUN_ID=$RUN_ID DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all LXRT_ROOT=$ROOT FEX_ROOTFS=/ \
        scripts/run-fex.sh "$PROTON_GUEST/bin/wine" wineboot -i > "$LOGS/win-d3d12-ffx-wineboot.log" 2>&1
    for _ in $(seq 1 30); do [ -n "$(ours)" ] || break; sleep 1; done
    stop_ours
    [ -d "$SYS32" ] || { echo "  FAIL  prefix (log $LOGS/win-d3d12-ffx-wineboot.log)"; rm -rf "$PRIV"; exit 1; }
fi
cp -f "$PROTON"/lib/wine/vkd3d-proton/x86_64-windows/*.dll "$SYS32/" &&
    cp -f "$PROTON"/lib/wine/dxvk/x86_64-windows/dxgi.dll "$SYS32/" || { echo "  FAIL  prefix"; rm -rf "$PRIV"; exit 1; }

# The game's directory: the program, AMD's DLL as the game's copy, and the
# same file under another name for the in-process comparison of queries.
rm -rf "${ROOT:?}$T"
mkdir -p "$ROOT$T/game" "$ROOT$T/ours" "$ROOT$T/noamd"
# This run's outputs (out-<mode>.bin): a directory of this run alone, so a
# comparison never reads another invocation's file.
OUT="$T/out.$RUN_ID"
mkdir -p "$ROOT$OUT"
WOUT="Z:${OUT//\//\\}"
have_amd=0
if [ -f "$ROOT$AMD_DLL" ]; then
    cp "$ROOT$AMD_DLL" "$ROOT$T/game/amd_fidelityfx_dx12.dll"
    cp "$ROOT$AMD_DLL" "$ROOT$T/game/ffx_amd_orig.dll"
    have_amd=1
    echo "== AMD's DLL: $AMD_DLL ($(stat -f %z "$ROOT$AMD_DLL") bytes, sha1 $(shasum "$ROOT$AMD_DLL" | cut -c1-12))"
else
    echo "== no AMD DLL at $AMD_DLL in the root: no game copy, no comparison"
fi
tools/ffx-metalfx/build.sh "$ROOT$T/ours" > "$LOGS/win-d3d12-ffx-build.log" 2>&1 || { echo "  FAIL  build DLLs (log $LOGS/win-d3d12-ffx-build.log)"; rm -rf "$PRIV"; exit 1; }
echo "== our DLL: $(shasum "$ROOT$T/ours/x86_64-windows/amd_fidelityfx_dx12.dll" | cut -c1-12) ($(stat -f %z "$ROOT$T/ours/x86_64-windows/amd_fidelityfx_dx12.dll") bytes)"
exe="$ROOT$T/game/d3d12_ffx.exe"
x86_64-w64-mingw32-gcc -O2 -mconsole -Wall -Wextra -Wno-missing-field-initializers -I"$VK_HEADERS/include" -o "$exe" tests/win/d3d12_ffx.c \
    -ld3d12 -ldxguid -luuid || { echo "  FAIL  build"; rm -rf "$PRIV"; exit 1; }
# The blit mode's game directory: the program beside a "game copy" of
# amd_fidelityfx_dx12.dll that cannot be loaded (its DllMain returns FALSE:
# LoadLibrary fails with ERROR_DLL_INIT_FAILED). Wine's override still finds
# the file and maps our builtin in its place; our DLL then copies the file,
# fails to load it and has no fallback. (A directory with no file at all is
# not the case: without a file of that name Wine finds no builtin either.)
cp "$exe" "$ROOT$T/noamd/d3d12_ffx.exe"
printf '#include <windows.h>\nBOOL WINAPI DllMain(HINSTANCE i, DWORD r, LPVOID p) { (void)i; (void)r; (void)p; return FALSE; }\n' > "$PRIV/stub.c"
x86_64-w64-mingw32-gcc -shared -s -o "$ROOT$T/noamd/amd_fidelityfx_dx12.dll" "$PRIV/stub.c" || { echo "  FAIL  build stub"; rm -rf "$PRIV"; exit 1; }
# A stale fallback copy from a previous build must not be reused: the name
# is the hash of the game's file, so only AMD's file ever matches.
rm -rf "$ROOT$PFX_GUEST/drive_c/users/steamuser/AppData/Local/SteamARM/ffx"

PASS=0 FAIL=0
ran_forward=0 ran_amd=0
for mode in ${@:-metalfx forward amd blit}; do
    args=("frames=$FRAMES" "out=$WOUT\\out-$mode.bin")
    wexe="Z:\\tmp\\ffx-t4\\game\\d3d12_ffx.exe"
    case $mode in
        metalfx) menv=(STEAMARM_WIN_UPSCALER=metalfx WINEDLLPATH=$T/ours "WINEDLLOVERRIDES=amd_fidelityfx_dx12=b,n;d3d12,d3d12core,dxgi=n")
                 [ $have_amd = 1 ] && args+=("orig=Z:\\tmp\\ffx-t4\\game\\ffx_amd_orig.dll") ;;
        forward) [ $have_amd = 1 ] || { echo "  skip  forward (no AMD DLL)"; continue; }
                 menv=(WINEDLLPATH=$T/ours "WINEDLLOVERRIDES=amd_fidelityfx_dx12=b,n;d3d12,d3d12core,dxgi=n") ;;
        amd)     [ $have_amd = 1 ] || { echo "  skip  amd (no AMD DLL)"; continue; }
                 menv=("WINEDLLOVERRIDES=amd_fidelityfx_dx12=n;d3d12,d3d12core,dxgi=n") ;;
        blit)    menv=(STEAMARM_WIN_UPSCALER=metalfx SA_FFX_TEST_REFUSE=1 WINEDLLPATH=$T/ours "WINEDLLOVERRIDES=amd_fidelityfx_dx12=b,n;d3d12,d3d12core,dxgi=n")
                 wexe="Z:\\tmp\\ffx-t4\\noamd\\d3d12_ffx.exe"
                 args+=("maxerr=$BLIT_MAXERR") ;;
        *) echo "  FAIL  unknown mode $mode"; FAIL=$((FAIL + 1)); continue ;;
    esac
    log="$LOGS/win-d3d12-ffx-$mode.log"
    start=$SECONDS
    env -u STEAMARM_WIN_UPSCALER -u WINEDLLPATH -u WINEDLLOVERRIDES -u SA_FFX_TEST_REFUSE \
        STEAMARM_RUN_ID=$RUN_ID LXRT_MOUNTS="$MOUNTS" LXRT_VK_DEBUG=1 SA_FFX_DEBUG=1 "${menv[@]}" \
        STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$PRIV/kk" VKD3D_DEBUG=warn VKD3D_FEATURE_LEVEL=12_0 \
        DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all LXRT_ROOT=$ROOT FEX_ROOTFS=/ \
        scripts/run-fex.sh "$PROTON_GUEST/bin/wine" "$wexe" "${args[@]}" > "$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 "$TIMEOUT"); do kill -0 $pid 2>/dev/null || break; sleep 1; done
    if kill -0 $pid 2>/dev/null; then
        echo "        (timeout: stopping this run's processes)"
        stop_ours
    fi
    wait $pid 2>/dev/null
    for _ in $(seq 1 15); do [ -n "$(ours)" ] || break; sleep 1; done
    stop_ours
    ok=0
    grep -q "^== d3d12 ffx: ok" "$log" && ok=1
    case $mode in
        metalfx)
            grep -q 'mfx-temporal ctx [0-9]* op encode .* st=OK' "$log" || { echo "        no ENCODE st=OK in the shim log"; ok=0; }
            grep -q 'create upscale: .* MetalFX temporal' "$log" || { echo "        the context is not MetalFX's"; ok=0; }
            grep -q 'ffx-metalfx.*the game.s own upscaler from now on' "$log" && { echo "        fell back to the game's upscaler"; ok=0; }
            ;;
        forward)
            grep -q 'FORWARD mode' "$log" || { echo "        the DLL did not say FORWARD mode"; ok=0; }
            [ $ok = 1 ] && ran_forward=1
            ;;
        amd)
            [ $ok = 1 ] && ran_amd=1
            ;;
        blit)
            grep -q 'ffx-metalfx.*ENCODE BAD_ARGS: the colour input is blitted to the output' "$log" || { echo "        the DLL did not blit"; ok=0; }
            grep -q 'mfx-temporal ctx [0-9]* op encode .* st=BAD_ARGS' "$log" || { echo "        the shim did not refuse the ENCODE"; ok=0; }
            grep -q 'original DLL: .*no fallback' "$log" || { echo "        the DLL found a game copy to fall back to"; ok=0; }
            grep -q 'ffx-metalfx.*the game.s own upscaler from now on' "$log" && { echo "        fell back to a game upscaler"; ok=0; }
            ;;
    esac
    if [ $ok = 1 ]; then echo "  ok    $mode ($((SECONDS - start)) s)"; PASS=$((PASS + 1))
    else echo "  FAIL  $mode ($((SECONDS - start)) s; log $log)"; FAIL=$((FAIL + 1)); fi
    grep -a -E '^(env:|D3D12CreateDevice|dll |versions:|    version|ffxCreateContext|provider version|gpu memory|    render|    ratios|    phase|    jitter|    differ|    non-OK|  ok|  FAIL|    still|    moving|    output|    dispatch|ffxDestroy|== d3d12|\[shim\] (vk driver|mfx-temporal)|\[ffx-metalfx|kk: MetalFX)' "$log" |
        sed 's/^/        /'
done
# forward against amd: both from this invocation (ran and passed), from
# this run's output directory; one alone is said, never compared.
if [ $ran_forward = 1 ] && [ $ran_amd = 1 ]; then
    if cmp -s "$ROOT$OUT/out-forward.bin" "$ROOT$OUT/out-amd.bin"; then
        echo "  ok    forward output == amd output (byte for byte, $(stat -f %z "$ROOT$OUT/out-amd.bin") bytes, this run)"; PASS=$((PASS + 1))
    else
        echo "  FAIL  forward output differs from amd output: $(cmp "$ROOT$OUT/out-forward.bin" "$ROOT$OUT/out-amd.bin" 2>&1 | head -1)"
        FAIL=$((FAIL + 1))
    fi
elif [ $ran_forward = 1 ] || [ $ran_amd = 1 ]; then
    echo "  --    forward vs amd output not compared: both must run (and pass) in the same invocation"
fi
rm -rf "$PRIV"
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
