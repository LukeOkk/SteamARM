#!/bin/bash
# AMD's FidelityFX SDK DX12 samples, end to end, with SteamARM's FidelityFX
# API DLLs (tools/ffx-metalfx) over the samples' own: FSR 3.1 (SDK 1.1.4,
# FFX_API_FSR_DX12.exe with amd_fidelityfx_dx12.dll) and FSR 3.1.x / FSR 4
# through the SDK 2.x loader (FidelityFX_FSR.exe with
# amd_fidelityfx_loader_dx12.dll, which LoadLibrary's our
# amd_fidelityfx_upscaler_dx12.dll), each in its benchmark mode, on
# KosmicKrisp (VKD3D_FEATURE_LEVEL=12_0), with AMD's own upscaler as the
# baseline. Per run: the samples' JSON (frames per second, the upscale
# pass's GPU time), the shim's presents per second (frame generation shows
# as presents above the frame rate), the shim's ENCODE statuses,
# KosmicKrisp's refusals, the presents per benchmark frame (2 = frame
# generation shows every frame twice), and a captured frame
# (LXRT_VK_SNAPSHOT, taken FFX_SNAP_AFTER seconds after the sample's
# Cauldron.log says "commencing benchmark": the same moment of the same
# camera path in every run) with a sharpness number and, against the
# baseline's frame, a difference number (tests/win/ppm_compare.py, the
# sample's UI panel on the left skipped).
#
#   tests/win/run_ffx_samples.sh                v114 v230, each amd then ours
#   tests/win/run_ffx_samples.sh v230 ours      a subset (modes amd, ours)
#
# The samples are not in the repository: they are expected in the x86 root
# at FFX_SAMPLES (default /tmp/ffxsdk, a guest path) as v114/bin
# (FFX_API_FSR_DX12.exe, AMD's amd_fidelityfx_dx12.dll 1.1.4, configs/,
# media at v114/media) and
# v230/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release (FidelityFX_FSR.exe,
# amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll 2.3.0,
# media at v230/media). Results go to $STEAMARM_STATE/logs (default
# ~/SteamARM-roots/logs) as ffx-e2e-<sample>-<mode>.log, the summary of each
# variant as ffx-e2e-<sample>-<mode>.summary (that variant's, rewritten when
# it runs again) and all of them appended, with a dated header per
# invocation, to ffx-e2e-summary.txt (never truncated: a subset run keeps
# the other variants' lines); the JSON, frames and frame times under the
# root's /tmp/ffx-e2e/<sample>-<mode>/.
#
# FFX_PFX      guest prefix (default /tmp/fexhome/ffx-t4, tests/win/run_d3d12_ffx.sh's)
# FFX_FRAMES   benchmark frames (default 240; the sample runs them twice)
# FFX_TIMEOUT  seconds per run (default 100); then only this run's
#              processes are stopped (STEAMARM_RUN_ID)
# FFX_SNAP_AFTER seconds into the benchmark to capture the frame (default 3)
# FFX_SNAP_AT  seconds after launch to capture it when the benchmark's start
#              is not seen in Cauldron.log by then (default 30)
# MFXT_SHIM, STEAMARM_KK_OWN as in tests/win/run_d3d12_ffx.sh.
set -u
cd "$(dirname "$0")/../.." || exit 1
. scripts/roots.sh
ROOT=/tmp/lxrt-steamroot
PFX_GUEST="${FFX_PFX:-/tmp/fexhome/ffx-t4}"
PROTON_GUEST="/tmp/fexhome/.local/share/Steam/steamapps/common/Proton - Experimental/files"
PROTON="$ROOT$PROTON_GUEST"
LOGS="${STEAMARM_STATE:-$HOME/SteamARM-roots}/logs"
TIMEOUT="${FFX_TIMEOUT:-100}"
FRAMES="${FFX_FRAMES:-240}"
SNAP_AT="${FFX_SNAP_AT:-30}"
SNAP_AFTER="${FFX_SNAP_AFTER:-3}"
SHIM="${MFXT_SHIM:-build/libvulkan.so.1}"
KK_DIR="${STEAMARM_KK_OWN:-${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out}"
SAMPLES="${FFX_SAMPLES:-/tmp/ffxsdk}"
E=/tmp/ffx-e2e   # guest
mkdir -p "$LOGS"
[ -x "$PROTON/bin/wine" ] || { echo "no Proton Experimental in the Steam root" >&2; exit 1; }
[ -f "$SHIM" ] || { echo "no $SHIM (make build/libvulkan.so.1)" >&2; exit 1; }
[ -f "$KK_DIR/libvulkan_kosmickrisp.dylib" ] || { echo "no $KK_DIR/libvulkan_kosmickrisp.dylib" >&2; exit 1; }
scripts/run-x11-native.sh start :2 >/dev/null || exit 1

RUN_ID="ffx-e2e.$$.$(date +%s)"
ours() {
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
PRIV=$(mktemp -d /tmp/steamarm-ffx-e2e.XXXXXX) || exit 1
trap 'stop_ours; rm -rf "$PRIV"; exit 130' INT TERM
cp "$SHIM" "$PRIV/libvulkan.so.1"
mkdir -p "$PRIV/kk"
cp "$KK_DIR/libvulkan_kosmickrisp.dylib" "$PRIV/kk/"
RS=$'\x1e' US=$'\x1f'
MOUNTS="/usr/lib64/libvulkan.so.1${RS}$PRIV/libvulkan.so.1${RS}1${US}/usr/lib/lxrt-emu/libvulkan.so.1${RS}$PRIV/libvulkan.so.1${RS}1${US}"
echo "== shim under test: $SHIM (sha1 $(shasum "$PRIV/libvulkan.so.1" | cut -c1-12)), installed: $(shasum "$ROOT/lib64/libvulkan.so.1" | cut -c1-12)"
echo "== KosmicKrisp: $KK_DIR (sha1 $(shasum "$PRIV/kk/libvulkan_kosmickrisp.dylib" | cut -c1-12))"
SYS32="$ROOT$PFX_GUEST/drive_c/windows/system32"
[ -d "$SYS32" ] || { echo "no prefix $PFX_GUEST: run tests/win/run_d3d12_ffx.sh first" >&2; rm -rf "$PRIV"; exit 1; }
mkdir -p "$ROOT$E"
tools/ffx-metalfx/build.sh "$ROOT$E/ours" > "$LOGS/ffx-e2e-build.log" 2>&1 || { echo "  FAIL  build DLLs (log $LOGS/ffx-e2e-build.log)"; rm -rf "$PRIV"; exit 1; }
echo "== our DLLs: $(shasum "$ROOT$E/ours/x86_64-windows/amd_fidelityfx_dx12.dll" | cut -c1-12)"
rm -rf "$ROOT$PFX_GUEST/drive_c/users/steamuser/AppData/Local/SteamARM/ffx"

samples="" modes=""
for a in "$@"; do case $a in v114|v230) samples="$samples $a" ;; amd|ours) modes="$modes $a" ;; *) echo "unknown: $a" >&2; exit 2 ;; esac; done
[ -n "$samples" ] || samples="v114 v230"
[ -n "$modes" ] || modes="amd ours"

PASS=0 FAIL=0
summary="$LOGS/ffx-e2e-summary.txt"
vsummary=""
echo "== $(date '+%Y-%m-%d %H:%M:%S') run $RUN_ID: samples$samples, modes$modes (shim $(shasum "$PRIV/libvulkan.so.1" | cut -c1-12), KK $(shasum "$PRIV/kk/libvulkan_kosmickrisp.dylib" | cut -c1-12))" >> "$summary"
note() { echo "$*"; echo "$*" >> "$summary"; [ -n "$vsummary" ] && echo "$*" >> "$vsummary"; }
for s in $samples; do
    case $s in
        v114) dir="$SAMPLES/v114/bin"; exe=FFX_API_FSR_DX12.exe; dlls="amd_fidelityfx_dx12" ;;
        v230) dir="$SAMPLES/v230/Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release"; exe=FidelityFX_FSR.exe; dlls="amd_fidelityfx_upscaler_dx12" ;;
    esac
    if [ ! -f "$ROOT$dir/$exe" ]; then echo "  skip  $s: no $dir/$exe in the root"; continue; fi
    for mode in $modes; do
        out="$E/$s-$mode"
        rm -rf "${ROOT:?}$out"
        mkdir -p "$ROOT$out/snap"
        vsummary="$LOGS/ffx-e2e-$s-$mode.summary"
        echo "== $(date '+%Y-%m-%d %H:%M:%S') run $RUN_ID" > "$vsummary"
        rm -f "$ROOT$dir/Cauldron.log"
        case $mode in
            amd)  menv=("WINEDLLOVERRIDES=d3d12,d3d12core,dxgi=n") ;;
            ours) menv=(STEAMARM_WIN_UPSCALER=metalfx "WINEDLLPATH=$E/ours" "WINEDLLOVERRIDES=$dlls=b,n;d3d12,d3d12core,dxgi=n") ;;
        esac
        windir="Z:${dir//\//\\}"
        winout="Z:${out//\//\\}"
        log="$LOGS/ffx-e2e-$s-$mode.log"
        start=$SECONDS
        env -u STEAMARM_WIN_UPSCALER -u WINEDLLPATH -u WINEDLLOVERRIDES \
            STEAMARM_RUN_ID=$RUN_ID LXRT_MOUNTS="$MOUNTS" LXRT_VK_DEBUG=1 SA_FFX_DEBUG=1 "${menv[@]}" \
            STEAMARM_VK_ICD=kosmickrisp STEAMARM_KK_DIR="$PRIV/kk" VKD3D_FEATURE_LEVEL=12_0 \
            LXRT_VK_SNAPSHOT="$out/snap" LXRT_VK_SNAPSHOT_FULL=1 LXRT_VK_FRAMETIMES="$out/frametimes.txt" \
            DISPLAY=:2 WINEPREFIX=$PFX_GUEST WINEDEBUG=-all LXRT_ROOT=$ROOT FEX_ROOTFS=/ \
            scripts/run-fex.sh "$PROTON_GUEST/bin/wine" cmd /c "cd /d $windir && $exe -benchmark duration=$FRAMES path=$winout json" > "$log" 2>&1 &
        pid=$!
        snapped=0 bench_at="" snap_note="not taken"
        for _ in $(seq 1 "$TIMEOUT"); do
            kill -0 $pid 2>/dev/null || break
            if [ $snapped = 0 ]; then
                if [ -z "$bench_at" ] && grep -q "commencing benchmark" "$ROOT$dir/Cauldron.log" 2>/dev/null; then bench_at=$SECONDS; fi
                if [ -n "$bench_at" ] && [ $((SECONDS - bench_at)) -ge "$SNAP_AFTER" ]; then
                    touch "$ROOT$out/snap/request"; snapped=1; snap_note="requested $((SECONDS - bench_at)) s into the benchmark ($((SECONDS - start)) s after launch)"
                elif [ -z "$bench_at" ] && [ $((SECONDS - start)) -ge "$SNAP_AT" ]; then
                    touch "$ROOT$out/snap/request"; snapped=1; snap_note="requested $((SECONDS - start)) s after launch, the benchmark's start not seen"
                fi
            fi
            sleep 1
        done
        if kill -0 $pid 2>/dev/null; then echo "        (timeout: stopping this run's processes)"; stop_ours; fi
        wait $pid 2>/dev/null
        for _ in $(seq 1 15); do [ -n "$(ours)" ] || break; sleep 1; done
        stop_ours
        cp -f "$ROOT$dir/Cauldron.log" "$ROOT$out/Cauldron.log" 2>/dev/null
        json=$(ls -t "$ROOT$out"/*.json 2>/dev/null | head -1)
        encodes=$(grep -c 'mfx-temporal ctx [0-9]* op encode .* st=OK' "$log")
        refused=$(grep -c 'kk: MetalFX temporal: context .*: the game.s own upscaler\|mfx-temporal ctx [0-9]* op encode .* st=\(BAD_ARGS\|UNSUPPORTED\|NO_SCALER\|NO_DRIVER\)' "$log")
        fell=$(grep -c 'ffx-metalfx.*the game.s own upscaler from now on' "$log")
        stats="" presents=""
        if [ -n "$json" ]; then
            stats=$(python3 - "$json" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8", errors="replace"))
labels = d.get("GPULabels", {})
ups = [(k, v["avg_ms"]) for k, v in labels.items() if "Upscal" in k]
fg = [(k, v["avg_ms"]) for k, v in labels.items() if "Frame Generation" in k or "FrameGen" in k or "Interpolation" in k]
print("fps %.1f over %.1f s, render %s display %s; upscale pass %s; frame generation pass %s" % (
    d.get("AvgFPS", 0), d.get("Runtime", 0), d.get("RenderResolution"), d.get("DisplayResolution"),
    ", ".join("%s %.3f ms" % (k, v) for k, v in ups) or "(no label)",
    ", ".join("%s %.3f ms" % (k, v) for k, v in fg) or "(no label: the 1.1.4 sample's runs in the swapchain)"))
PY
)
            # The presents of the benchmark's own seconds (the end of the
            # run) against its frames: 2 per frame = frame generation.
            if [ -s "$ROOT$out/frametimes.txt" ]; then
                presents=$(python3 - "$json" "$ROOT$out/frametimes.txt" <<'PY'
import json, sys
d = json.load(open(sys.argv[1], encoding="utf-8", errors="replace"))
runtime, fps = float(d.get("Runtime", 0)), float(d.get("AvgFPS", 0))
us = [int(x) for x in open(sys.argv[2]).read().split() if x.isdigit()]
# The last `runtime` seconds of presents, the shutdown's tail excepted.
t, n = 0, 0
for v in reversed(us):
    if t + v > runtime * 1e6:
        break
    t += v
    n += 1
if t > 0 and fps > 0:
    rate = n * 1e6 / t
    print("%d presents in the last %.1f s = %.1f/s = %.2f per benchmark frame (%d in the whole run)" % (n, t / 1e6, rate, rate / fps, len(us)))
else:
    print("%d presents in the whole run" % len(us))
PY
)
            fi
        fi
        ok=1
        [ -n "$json" ] || { note "        $s $mode: no benchmark JSON"; ok=0; }
        if [ $mode = ours ]; then
            [ "$encodes" -gt 0 ] || { note "        $s ours: no ENCODE st=OK in the shim log"; ok=0; }
            [ "$refused" = 0 ] || { note "        $s ours: $refused refusals"; ok=0; }
            [ "$fell" = 0 ] || { note "        $s ours: fell back to the game's upscaler ($fell)"; ok=0; }
        fi
        if [ $ok = 1 ]; then note "  ok    $s $mode ($((SECONDS - start)) s)"; PASS=$((PASS + 1))
        else note "  FAIL  $s $mode ($((SECONDS - start)) s; log $log)"; FAIL=$((FAIL + 1)); fi
        note "        json: ${stats:-none}"
        note "        presents: ${presents:-none}; shim: $encodes ENCODE OK, $refused refused; fallbacks $fell"
        grep -a -E '^\[ffx-metalfx|^\[shim\] mfx-temporal|^kk: MetalFX temporal|Upscaler Context versionid|Upscaler [0-9.]+:|required_resources' "$log" "$ROOT$out/Cauldron.log" 2>/dev/null | sed 's/^.*\.log://; s/^/        /' | sort -u | head -24 | tee -a "$summary" "$vsummary"
        if [ -f "$ROOT$out/snap/frame.ppm" ]; then
            note "        frame: $out/snap/frame.ppm ($(head -c 20 "$ROOT$out/snap/frame.ppm" | sed -n 2p)), $snap_note"
        else
            note "        frame: none captured ($snap_note)"
        fi
    done
    vsummary=""
    if [ -f "$ROOT$E/$s-ours/snap/frame.ppm" ] && [ -f "$ROOT$E/$s-amd/snap/frame.ppm" ]; then
        note "        $s frames ours vs amd: $(python3 tests/win/ppm_compare.py "$ROOT$E/$s-ours/snap/frame.ppm" "$ROOT$E/$s-amd/snap/frame.ppm" left=440) (ours $(stat -f %Sm -t %H:%M:%S "$ROOT$E/$s-ours/snap/frame.ppm"), amd $(stat -f %Sm -t %H:%M:%S "$ROOT$E/$s-amd/snap/frame.ppm"))"
    fi
done
rm -rf "$PRIV"
note "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
