#!/bin/bash
# DLSS -> MetalFX temporal through SteamARM's nvngx.dll (tools/dlss-metalfx)
# under Proton Experimental, no VM: tests/win/d3d12_dlss.c drives NVIDIA's
# NGX API against the DLL the way a Unreal / Unity game does, and the DLL
# records the upscale (shim/mfx_temporal.h) into the command list through
# VKD3D-Proton's interop -> vulkan-1 -> winevulkan -> FEX's Vulkan thunks ->
# the Vulkan shim -> KosmicKrisp -> MetalFX.
#
#   tests/win/run_d3d12_dlss.sh                kk sys32 nokk mvk vk vkmvk
#   tests/win/run_d3d12_dlss.sh kk             one or more of them
#
# kk     KosmicKrisp, the DLL next to the program and in WINEDLLPATH, loaded
#        by name as a game does ("nvngx.dll"): DLSS available, the picture
#        checks, the fallback blit of a refused 4x upscale, and DLSS 1 as
#        the shim's spatial pass (MetalFX spatial: "kk: FSR by MetalFX")
# sys32  KosmicKrisp, "_nvngx.dll" loaded by its C:\windows\system32 path, as
#        NVIDIA's static NGX SDK loads the NGX core (the registry's NGXCore
#        FullPath, which Proton's prefixes set to System32): the DLL is
#        copied into THIS test prefix's system32 for the run and removed after
# nokk   KosmicKrisp with KK_MFXT=0 (MetalFX temporal off in the driver):
#        the probe fails, SuperSampling.Available = 0, CreateFeature refused;
#        DLSS 1 still available (the spatial pass is the shim's)
# mvk    MoltenVK: no kk_steamarm_upscale_temporal, so the same as nokk;
#        DLSS 1 as FSR 1 (EASU + RCAS) in the shim
# vk     the Vulkan side (tests/win/vk_dlss.c: NVSDK_NGX_VULKAN_* on the
#        program's own VkCommandBuffer), KosmicKrisp
# vkmvk  the same on MoltenVK (DLSS 2+ unavailable, DLSS 1 as FSR 1)
#
# The shim under test is build/libvulkan.so.1 (DLSS_SHIM overrides), bound
# over the root's for this run's processes only (LXRT_MOUNTS, as
# tests/win/run_mfxt_channel.sh); the DLLs are built into a folder of the
# guest's /tmp; nothing is installed. KosmicKrisp: STEAMARM_KK_OWN, default
# ${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out (patch 20's entry),
# copied privately for the run.
#
# DLSS_PFX      guest prefix (default /tmp/fexhome/dlss-ngx, its own)
# DLSS_TIMEOUT  seconds per run (default 90; the program ends itself at 55 s);
#               then only this run's processes are stopped (STEAMARM_RUN_ID)
# DLSS_FRAMES   frames per case (default 40)
#
# Needs: the Steam root with Proton Experimental and the Vulkan thunks,
# mingw-w64 and vulkan-headers (Homebrew).
set -u
cd "$(dirname "$0")/../.." || exit 1
. scripts/roots.sh
ROOT=/tmp/lxrt-steamroot
PFX_GUEST="${DLSS_PFX:-/tmp/fexhome/dlss-ngx}"
PROTON_GUEST="/tmp/fexhome/.local/share/Steam/steamapps/common/Proton - Experimental/files"
PROTON="$ROOT$PROTON_GUEST"
LOGS="${STEAMARM_STATE:-$HOME/SteamARM-roots}/logs"
TIMEOUT="${DLSS_TIMEOUT:-90}"
FRAMES="${DLSS_FRAMES:-40}"
SHIM="${DLSS_SHIM:-build/libvulkan.so.1}"
KK_DIR="${STEAMARM_KK_OWN:-${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out}"
VK_HEADERS="$(brew --prefix vulkan-headers 2>/dev/null || echo /opt/homebrew/opt/vulkan-headers)"
T=/tmp/dlss-ngx-test                           # guest folder: the program, the DLLs
mkdir -p "$LOGS"
[ -x "$PROTON/bin/wine" ] || { echo "no Proton Experimental in the Steam root" >&2; exit 1; }
[ -f "$ROOT/usr/lib/lxrt-emu/thunks/HostThunks/libvulkan-host.so" ] ||
    { echo "no Vulkan thunks in the Steam root" >&2; exit 1; }
[ -f "$SHIM" ] || { echo "no $SHIM (make build/libvulkan.so.1)" >&2; exit 1; }
[ -f "$KK_DIR/libvulkan_kosmickrisp.dylib" ] || { echo "no $KK_DIR/libvulkan_kosmickrisp.dylib" >&2; exit 1; }
scripts/run-x11-native.sh start :2 >/dev/null || exit 1

RUN_ID="dlss-d3d12.$$.$(date +%s)"
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
PRIV=$(mktemp -d /tmp/steamarm-dlss-d3d12.XXXXXX) || exit 1
SYS32="$ROOT$PFX_GUEST/drive_c/windows/system32"
cleanup() { stop_ours; rm -rf "$PRIV"; rm -f "$SYS32/_nvngx.dll.steamarm-test" "$SYS32/nvngx.dll.steamarm-test"; }
trap 'cleanup; exit 130' INT TERM
cp "$SHIM" "$PRIV/libvulkan.so.1"
mkdir -p "$PRIV/kk"
cp "$KK_DIR/libvulkan_kosmickrisp.dylib" "$PRIV/kk/"
RS=$'\x1e' US=$'\x1f'
MOUNTS="/usr/lib64/libvulkan.so.1${RS}$PRIV/libvulkan.so.1${RS}1${US}/usr/lib/lxrt-emu/libvulkan.so.1${RS}$PRIV/libvulkan.so.1${RS}1${US}"
echo "== shim under test: $SHIM (sha1 $(shasum "$PRIV/libvulkan.so.1" | cut -c1-12)), installed: $(shasum "$ROOT/lib64/libvulkan.so.1" | cut -c1-12)"
echo "== KosmicKrisp: $KK_DIR (sha1 $(shasum "$PRIV/kk/libvulkan_kosmickrisp.dylib" | cut -c1-12))"
echo "== vkd3d-proton: $(cat "$PROTON/lib/wine/vkd3d-proton/version" 2>/dev/null)"

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
    cp -f "$PROTON"/lib/wine/dxvk/x86_64-windows/dxgi.dll "$SYS32/" || { echo "  FAIL  prefix"; cleanup; exit 1; }
# The DLLs: in WINEDLLPATH's layout and, the "game's" copy, next to the program.
rm -rf "${ROOT:?}$T"
mkdir -p "$ROOT$T/game" "$ROOT$T/dllpath"
tools/dlss-metalfx/build.sh "$ROOT$T/dllpath" >/dev/null || { echo "  FAIL  build (DLLs)"; cleanup; exit 1; }
cp "$ROOT$T/dllpath/x86_64-windows/nvngx.dll" "$ROOT$T/dllpath/x86_64-windows/_nvngx.dll" "$ROOT$T/game/"
x86_64-w64-mingw32-gcc -O2 -mconsole -Wall -Wextra -Wno-missing-field-initializers -I"$VK_HEADERS/include" \
    -o "$ROOT$T/game/d3d12_dlss.exe" tests/win/d3d12_dlss.c -ld3d12 -ldxguid -luuid ||
    { echo "  FAIL  build"; cleanup; exit 1; }
x86_64-w64-mingw32-gcc -O2 -mconsole -Wall -Wextra -Wno-missing-field-initializers -I"$VK_HEADERS/include" \
    -o "$ROOT$T/game/vk_dlss.exe" tests/win/vk_dlss.c ||
    { echo "  FAIL  build (vk)"; cleanup; exit 1; }

PASS=0 FAIL=0
for mode in ${@:-kk sys32 nokk mvk vk vkmvk}; do
    args=("frames=$FRAMES")
    sys32_copy=0 exe=d3d12_dlss.exe okline="== d3d12 dlss: ok"
    case $mode in
        kk)    menv=(STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$PRIV/kk") ;;
        sys32) menv=(STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$PRIV/kk"); args+=(load=sys32); sys32_copy=1 ;;
        nokk)  menv=(STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$PRIV/kk" KK_MFXT=0); args+=(expect=unavailable) ;;
        mvk)   menv=(STEAMARM_VK_ICD=moltenvk); args+=(expect=unavailable) ;;
        vk)    menv=(STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$PRIV/kk"); exe=vk_dlss.exe okline="== vk dlss: ok" ;;
        vkmvk) menv=(STEAMARM_VK_ICD=moltenvk); args+=(expect=unavailable); exe=vk_dlss.exe okline="== vk dlss: ok" ;;
        *) echo "  FAIL  unknown mode $mode"; FAIL=$((FAIL + 1)); continue ;;
    esac
    if [ "$sys32_copy" = 1 ]; then
        # Only into this test's own prefix; a file of that name already
        # there (not ours) is left alone and the case is skipped.
        if [ -e "$SYS32/_nvngx.dll" ]; then
            echo "  skip  $mode: $SYS32/_nvngx.dll exists already"; continue
        fi
        cp "$ROOT$T/dllpath/x86_64-windows/_nvngx.dll" "$SYS32/_nvngx.dll"
    fi
    log="$LOGS/win-d3d12-dlss-$mode.log"
    start=$SECONDS
    env STEAMARM_RUN_ID=$RUN_ID LXRT_MOUNTS="$MOUNTS" LXRT_VK_DEBUG=1 STEAMARM_NGX_DEBUG=1 "${menv[@]}" \
        VKD3D_FEATURE_LEVEL=12_0 VKD3D_DEBUG=warn \
        WINEDLLPATH="$T/dllpath" WINEDLLOVERRIDES="nvngx,_nvngx=b,n;d3d12,d3d12core,dxgi=n" \
        DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all,+loaddll LXRT_ROOT=$ROOT FEX_ROOTFS=/ \
        scripts/run-fex.sh "$PROTON_GUEST/bin/wine" "Z:${T//\//\\}\\game\\$exe" "${args[@]}" > "$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 "$TIMEOUT"); do kill -0 $pid 2>/dev/null || break; sleep 1; done
    if kill -0 $pid 2>/dev/null; then
        echo "        (timeout: stopping this run's processes)"
        stop_ours
    fi
    wait $pid 2>/dev/null
    for _ in $(seq 1 15); do [ -n "$(ours)" ] || break; sleep 1; done
    stop_ours
    [ "$sys32_copy" = 0 ] || rm -f "$SYS32/_nvngx.dll"
    if grep -q "^$okline" "$log"; then
        echo "  ok    $mode ($((SECONDS - start)) s)"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $mode ($((SECONDS - start)) s; log $log)"
        FAIL=$((FAIL + 1))
    fi
    grep -a -E '^(env:|LoadLibraryW|module:|ident:|D3D12CreateDevice|NVSDK_NGX|device:|caps:|vk:|  ok|  FAIL|    |== (d3d12|vk) dlss|\[shim\] (mfx-temporal|spatial)|\[n?_?nvngx-metalfx\]|kk: (MetalFX|FSR by MetalFX))' "$log" |
        tr -d '\r' | sed 's/^/        /'
    grep -a -E 'Loaded L?"[^"]*n?_?nvngx' "$log" | tr -d '\r' | sed 's/^[0-9a-f]*:[0-9a-f]*://; s/^/        wine: /' | head -3
done
rm -rf "${ROOT:?}$T"
cleanup
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
