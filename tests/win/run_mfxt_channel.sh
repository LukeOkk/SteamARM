#!/bin/bash
# The MetalFX temporal command channel (shim/mfx_temporal.h) from a Windows
# program, no VM: tests/win/vk_mfxt_channel.c under Proton Experimental,
# launched the way tests/win/run.sh launches its probes, through vulkan-1 ->
# winevulkan -> FEX's Vulkan thunks -> the Vulkan shim -> MoltenVK or
# KosmicKrisp.
#
#   tests/win/run_mfxt_channel.sh                    MoltenVK, then KosmicKrisp
#   tests/win/run_mfxt_channel.sh moltenvk           one of them
#
# The shim under test is build/libvulkan.so.1 (MFXT_SHIM overrides), NOT the
# one installed in the Steam root: a private copy is bound over the root's
# /usr/lib64/libvulkan.so.1 and /usr/lib/lxrt-emu/libvulkan.so.1 for this
# run's processes only (the runtime's bind table, LXRT_MOUNTS, which every
# child inherits). Steam's games keep the installed shim.
#
# MFXT_PFX       guest prefix (default run.sh's, /tmp/fexhome/wtest)
# MFXT_TIMEOUT   seconds per run (default 120); then only this run's
#                processes are stopped (by their STEAMARM_RUN_ID)
# KosmicKrisp: SteamARM's build in $STEAMARM_BUILD/mesa-kk/out when there is
# one (as scripts/settings-env.py picks it), Homebrew's otherwise.
#
# Needs: the Steam root with Proton Experimental and the Vulkan thunks
# (scripts/install-steamroot-gfx.sh), mingw-w64 and vulkan-headers (Homebrew).
set -u
cd "$(dirname "$0")/../.." || exit 1
. scripts/roots.sh
ROOT=/tmp/lxrt-steamroot
PFX_GUEST="${MFXT_PFX:-/tmp/fexhome/wtest}"
PROTON_GUEST="/tmp/fexhome/.local/share/Steam/steamapps/common/Proton - Experimental/files"
PROTON="$ROOT$PROTON_GUEST"
LOGS="${STEAMARM_STATE:-$HOME/SteamARM-roots}/logs"
TIMEOUT="${MFXT_TIMEOUT:-120}"
SHIM="${MFXT_SHIM:-build/libvulkan.so.1}"
KK_DIR="${STEAMARM_KK_OWN:-${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out}"
VK_HEADERS="$(brew --prefix vulkan-headers 2>/dev/null || echo /opt/homebrew/opt/vulkan-headers)"
mkdir -p "$LOGS"
[ -x "$PROTON/bin/wine" ] || { echo "no Proton Experimental in the Steam root" >&2; exit 1; }
[ -f "$ROOT/usr/lib/lxrt-emu/thunks/HostThunks/libvulkan-host.so" ] ||
    { echo "no Vulkan thunks in the Steam root (scripts/install-steamroot-gfx.sh)" >&2; exit 1; }
[ -f "$SHIM" ] || { echo "no $SHIM (make build/libvulkan.so.1)" >&2; exit 1; }
scripts/run-x11-native.sh start :2 >/dev/null || exit 1

RUN_ID="mfxt-channel.$$.$(date +%s)"
ours() {   # this run's guests, by the STEAMARM_RUN_ID every one of them inherits
    local pids
    pids=$(ps -Ao pid=,comm= | awk '$NF ~ /(^|\/)lxrun$/ {print $1}')
    [ -n "$pids" ] || return 0
    # shellcheck disable=SC2086
    procs_env STEAMARM_RUN_ID $pids | awk -v id="$RUN_ID" '$2 == id { printf "%s ", $1 }'
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
PRIV=$(mktemp -d /tmp/steamarm-mfxt.XXXXXX) || exit 1
trap 'stop_ours; rm -rf "$PRIV"; exit 130' INT TERM
cp "$SHIM" "$PRIV/libvulkan.so.1"
# Records: guest path \x1e host path \x1e read-only, separated by \x1f
# (runtime/mounts.c). /lib64 is a link to usr/lib64 in the root, and the
# table is matched after the root's links are followed.
RS=$'\x1e' US=$'\x1f'
MOUNTS="/usr/lib64/libvulkan.so.1${RS}$PRIV/libvulkan.so.1${RS}1${US}/usr/lib/lxrt-emu/libvulkan.so.1${RS}$PRIV/libvulkan.so.1${RS}1${US}"
echo "== shim under test: $SHIM (sha1 $(shasum "$PRIV/libvulkan.so.1" | cut -c1-12)), installed: $(shasum "$ROOT/lib64/libvulkan.so.1" | cut -c1-12)"

SYS32="$ROOT$PFX_GUEST/drive_c/windows/system32"
if [ ! -d "$SYS32" ]; then
    STEAMARM_RUN_ID=$RUN_ID DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all LXRT_ROOT=$ROOT FEX_ROOTFS=/ \
        scripts/run-fex.sh "$PROTON_GUEST/bin/wine" wineboot -i >/dev/null 2>&1
fi
exe="$ROOT/tmp/vk_mfxt_channel.exe"
x86_64-w64-mingw32-gcc -O2 -mconsole -Wall -Wextra -I"$VK_HEADERS/include" -o "$exe" tests/win/vk_mfxt_channel.c ||
    { echo "  FAIL  build"; rm -rf "$PRIV"; exit 1; }

PASS=0 FAIL=0
for drv in ${@:-moltenvk kosmickrisp}; do
    case $drv in
        moltenvk)    denv=(STEAMARM_VK_ICD=moltenvk) ;;
        kosmickrisp) denv=(STEAMARM_VK_ICD=kosmickrisp)
                     [ -f "$KK_DIR/libvulkan_kosmickrisp.dylib" ] && denv+=(STEAMARM_KK_DIR="$KK_DIR") ;;
        *) echo "  FAIL  unknown driver $drv"; FAIL=$((FAIL + 1)); continue ;;
    esac
    log="$LOGS/win-mfxt-channel-$drv.log"
    env STEAMARM_RUN_ID=$RUN_ID LXRT_MOUNTS="$MOUNTS" LXRT_VK_DEBUG=1 "${denv[@]}" \
        DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all LXRT_ROOT=$ROOT FEX_ROOTFS=/ \
        scripts/run-fex.sh "$PROTON_GUEST/bin/wine" 'Z:\tmp\vk_mfxt_channel.exe' > "$log" 2>&1 &
    pid=$!
    for _ in $(seq 1 "$TIMEOUT"); do kill -0 $pid 2>/dev/null || break; sleep 1; done
    if kill -0 $pid 2>/dev/null; then
        echo "        (timeout: stopping this run's processes)"
        stop_ours
    fi
    wait $pid 2>/dev/null
    # The prefix's wineserver and services started by this run: theirs only.
    for _ in $(seq 1 15); do [ -n "$(ours)" ] || break; sleep 1; done
    stop_ours
    drv_line=$(grep -m1 '^\[shim\] vk driver' "$log")
    if grep -q "== mfxt channel probe: ok" "$log" && grep -q '^\[shim\] mfx-temporal ctx' "$log"; then
        echo "  ok    $drv: $(grep -m1 '^== mfxt channel probe' "$log" | sed 's/^== mfxt channel probe: //')"
        PASS=$((PASS + 1))
    else
        echo "  FAIL  $drv (log $log)"
        FAIL=$((FAIL + 1))
    fi
    echo "        ${drv_line:-(no shim driver line)}"
    grep -E '^(device:|probe|command|near miss|buffer:|\[shim\] mfx-temporal)' "$log" | sed 's/^/        /'
done
rm -rf "$PRIV"
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
