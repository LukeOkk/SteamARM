#!/bin/bash
# The D3D12 side of the FSR -> MetalFX temporal path (tests/win/d3d12_mfxt_interop.c)
# under Proton Experimental, no VM: VKD3D-Proton's interop interface hands
# the program the command list's VkCommandBuffer and the textures' VkImages,
# and the command (shim/mfx_temporal.h) goes vulkan-1 -> winevulkan -> FEX's
# Vulkan thunks -> the Vulkan shim -> KosmicKrisp -> MetalFX.
#
#   tests/win/run_d3d12_mfxt_interop.sh              fl11 fl12 ots
#   tests/win/run_d3d12_mfxt_interop.sh fl12         one of them
#
# fl11   no VKD3D_FEATURE_LEVEL: 12_0 is refused on KosmicKrisp, the program
#        goes on at 11_0
# fl12   VKD3D_FEATURE_LEVEL=12_0, as AMD's DX12 samples need on KosmicKrisp
# ots    fl12 plus VKD3D_CONFIG=one_time_submit (vkd3d's command buffers
#        ONE_TIME_SUBMIT, KosmicKrisp's replay path not used; no second
#        execution of a command list)
#
# The shim under test is build/libvulkan.so.1 (MFXT_SHIM overrides), bound
# over the root's for this run's processes only (LXRT_MOUNTS), as
# tests/win/run_mfxt_channel.sh does; nothing is installed. KosmicKrisp:
# STEAMARM_KK_OWN, default ${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out-test
# (the build with patch 20), copied privately for the run.
#
# MFXT_PFX       guest prefix (default /tmp/fexhome/mfxt-d3d12, its own)
# MFXT_TIMEOUT   seconds per run (default 90; the program ends itself at
#                55 s); then only this run's processes are stopped (by their
#                STEAMARM_RUN_ID; a FEXServer it started is left alone, other
#                sessions share it)
# MFXT_FRAMES    frames per case (default 40)
#
# Needs: the Steam root with Proton Experimental and the Vulkan thunks,
# mingw-w64 and vulkan-headers (Homebrew).
set -u
cd "$(dirname "$0")/../.." || exit 1
. scripts/roots.sh
ROOT=/tmp/lxrt-steamroot
PFX_GUEST="${MFXT_PFX:-/tmp/fexhome/mfxt-d3d12}"
PROTON_GUEST="/tmp/fexhome/.local/share/Steam/steamapps/common/Proton - Experimental/files"
PROTON="$ROOT$PROTON_GUEST"
LOGS="${STEAMARM_STATE:-$HOME/SteamARM-roots}/logs"
TIMEOUT="${MFXT_TIMEOUT:-90}"
FRAMES="${MFXT_FRAMES:-40}"
SHIM="${MFXT_SHIM:-build/libvulkan.so.1}"
KK_DIR="${STEAMARM_KK_OWN:-${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out-test}"
VK_HEADERS="$(brew --prefix vulkan-headers 2>/dev/null || echo /opt/homebrew/opt/vulkan-headers)"
mkdir -p "$LOGS"
[ -x "$PROTON/bin/wine" ] || { echo "no Proton Experimental in the Steam root" >&2; exit 1; }
[ -f "$ROOT/usr/lib/lxrt-emu/thunks/HostThunks/libvulkan-host.so" ] ||
    { echo "no Vulkan thunks in the Steam root" >&2; exit 1; }
[ -f "$SHIM" ] || { echo "no $SHIM (make build/libvulkan.so.1)" >&2; exit 1; }
[ -f "$KK_DIR/libvulkan_kosmickrisp.dylib" ] || { echo "no $KK_DIR/libvulkan_kosmickrisp.dylib" >&2; exit 1; }
scripts/run-x11-native.sh start :2 >/dev/null || exit 1

RUN_ID="mfxt-d3d12.$$.$(date +%s)"
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
PRIV=$(mktemp -d /tmp/steamarm-mfxt-d3d12.XXXXXX) || exit 1
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
        scripts/run-fex.sh "$PROTON_GUEST/bin/wine" wineboot -i >/dev/null 2>&1
    for _ in $(seq 1 15); do [ -n "$(ours)" ] || break; sleep 1; done
    stop_ours
fi
# VKD3D-Proton (and DXVK's dxgi, which it asks for the adapter), as Proton's
# own script copies them into a game's prefix.
cp -f "$PROTON"/lib/wine/vkd3d-proton/x86_64-windows/*.dll "$SYS32/" &&
    cp -f "$PROTON"/lib/wine/dxvk/x86_64-windows/dxgi.dll "$SYS32/" || { echo "  FAIL  prefix"; rm -rf "$PRIV"; exit 1; }
exe="$ROOT/tmp/d3d12_mfxt_interop.exe"
x86_64-w64-mingw32-gcc -O2 -mconsole -Wall -Wextra -I"$VK_HEADERS/include" -o "$exe" tests/win/d3d12_mfxt_interop.c \
    -ld3d12 -ldxguid -luuid ||
    { echo "  FAIL  build"; rm -rf "$PRIV"; exit 1; }

PASS=0 FAIL=0
for mode in ${@:-fl11 fl12 ots}; do
    args=("frames=$FRAMES")
    case $mode in
        fl11) menv=() ;;
        fl12) menv=(VKD3D_FEATURE_LEVEL=12_0) ;;
        ots)  menv=(VKD3D_FEATURE_LEVEL=12_0 VKD3D_CONFIG=one_time_submit); args+=(noresubmit) ;;
        *) echo "  FAIL  unknown mode $mode"; FAIL=$((FAIL + 1)); continue ;;
    esac
    log="$LOGS/win-d3d12-mfxt-$mode.log"
    start=$SECONDS
    env STEAMARM_RUN_ID=$RUN_ID LXRT_MOUNTS="$MOUNTS" LXRT_VK_DEBUG=1 "${menv[@]}" \
        STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$PRIV/kk" VKD3D_DEBUG=warn \
        WINEDLLOVERRIDES="d3d12,d3d12core,dxgi=n" \
        DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all LXRT_ROOT=$ROOT FEX_ROOTFS=/ \
        scripts/run-fex.sh "$PROTON_GUEST/bin/wine" 'Z:\tmp\d3d12_mfxt_interop.exe' "${args[@]}" > "$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 "$TIMEOUT"); do kill -0 $pid 2>/dev/null || break; sleep 1; done
    if kill -0 $pid 2>/dev/null; then
        echo "        (timeout: stopping this run's processes)"
        stop_ours
    fi
    wait $pid 2>/dev/null
    for _ in $(seq 1 15); do [ -n "$(ours)" ] || break; sleep 1; done
    stop_ours
    if grep -q "^== d3d12 mfxt interop: ok" "$log"; then
        echo "  ok    $mode ($((SECONDS - start)) s)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $mode ($((SECONDS - start)) s; log $log)"
        FAIL=$((FAIL + 1))
    fi
    grep -E '^(env:|D3D12CreateDevice|device:|interop:|vkd3d.s|vulkan:|vk:|layouts|footprints|  ok|  FAIL|    |== d3d12|setup:|total:|\[shim\] (vk driver|mfx-temporal)|kk: MetalFX)' "$log" |
        sed 's/^/        /'
done
rm -rf "$PRIV"
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
