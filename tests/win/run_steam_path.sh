#!/bin/bash
# Windows probes launched the way Steam launches a game: the Steam Linux
# Runtime container (pressure-vessel) around Proton, no VM.
#
#   tone     sound: waveOut -> winepulse -> the container's PulseAudio socket
#            (the bind pressure-vessel makes of scripts/audio.sh's socket)
#   xinput   a controller: XInput -> winebus -> SDL -> /dev/input/event0
#            (runtime/evdev.c), fed by the fake daemon tests/elf/fake_inputd.py
#            -- or by the real steamarm-inputd with --fake xbox360
#   d3d11    DXVK and VKD3D-Proton: the tests/win/*_clear.c probes, pass/fail
#   d3d12    and fps; the baseline of tests/win/run_fex_boundary.sh, which runs
#            the same probes through the native arm64 client's tool
#
#   tests/win/run_steam_path.sh [tone] [xinput] [d3d11] [d3d12]   (default: tone xinput)
#
# STEAMPATH_PROTON  the x86 Proton (default "Proton - Experimental"); the
#                   runtime is the one its toolmanifest.vdf requires
# STEAMPATH_PROBE   guest directory of the probes (default
#                   $G/steamapps/common/SteamARM-Probe)
# STEAMPATH_PFX     guest compat data path, the prefix (default
#                   $G/steamapps/compatdata/999999). Another Proton upgrades
#                   a prefix it did not make: give each Proton its own.
# PROBE_TIMEOUT     seconds per d3d probe (default 300); then only this run's
#                   processes are stopped (by their STEAMARM_RUN_ID)
#
# Needs: Steam installed in the Steam root with that Proton and its Steam
# Linux Runtime, mingw-w64, PulseAudio (for tone).
set -u
cd "$(dirname "$0")/../.." || exit 1
. scripts/roots.sh
R=/tmp/lxrt-steamroot
G=/tmp/fexhome/.local/share/Steam
PROTON_TOOL="${STEAMPATH_PROTON:-Proton - Experimental}"
PROBE="${STEAMPATH_PROBE:-$G/steamapps/common/SteamARM-Probe}"
PFX="${STEAMPATH_PFX:-$G/steamapps/compatdata/999999}"
PROBE_TIMEOUT="${PROBE_TIMEOUT:-300}"
LOGS="${STEAMARM_STATE:-$HOME/SteamARM-roots}/logs"
PACTL=/opt/homebrew/opt/pulseaudio/bin/pactl
appid=$(sed -n 's/.*"require_tool_appid"[[:space:]]*"\([0-9]*\)".*/\1/p' \
        "$R$G/steamapps/common/$PROTON_TOOL/toolmanifest.vdf" 2>/dev/null | head -1)
case "$appid" in
    1628350) SLR=SteamLinuxRuntime_sniper ;;
    4183110) SLR=SteamLinuxRuntime_4 ;;
    *) echo "no x86 '$PROTON_TOOL' with a known runtime in the Steam root (require_tool_appid '$appid')" >&2; exit 1 ;;
esac
mkdir -p "$LOGS" "$R$PROBE" "$R$PFX"
[ -f "$R$G/steamapps/common/$SLR/_v2-entry-point" ] ||
    { echo "no $SLR in the Steam root" >&2; exit 1; }
scripts/run-x11-native.sh start :2 >/dev/null || exit 1
RUN_ID="steampath.$$.$(date +%s)"

run_probe() {   # $1 = probe name; the game's launch line, as Steam builds it
    STEAMARM_RUN_ID=$RUN_ID DISPLAY=:2 PULSE_SERVER=unix:/tmp/pulse/native \
    STEAM_COMPAT_CLIENT_INSTALL_PATH=$G STEAM_COMPAT_DATA_PATH=$PFX \
    STEAM_COMPAT_INSTALL_PATH=$PROBE SteamAppId=999999 SteamGameId=999999 \
    LXRT_ROOT=$R FEX_ROOTFS=/ scripts/run-fex.sh /bin/bash \
        "$G/steamapps/common/$SLR/_v2-entry-point" --verb=waitforexitandrun -- \
        "$G/steamapps/common/$PROTON_TOOL/proton" waitforexitandrun "$PROBE/$1_clear.exe" \
        > "$LOGS/steampath-$1.log" 2>&1
}
ours() {   # this run's guests, by the STEAMARM_RUN_ID every one of them inherits
    local pids
    pids=$(ps -Ao pid=,comm= | awk '$NF ~ /(^|\/)lxrun$/ {print $1}')
    [ -n "$pids" ] || return 0
    # shellcheck disable=SC2086
    procs_env STEAMARM_RUN_ID $pids | awk -v id="$RUN_ID" '$2 == id { printf "%s ", $1 }'
}

PASS=0 FAIL=0
ok()  { echo "  ok    $*"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL  $*"; FAIL=$((FAIL + 1)); }

for t in ${@:-tone xinput}; do
    case $t in
    tone)
        x86_64-w64-mingw32-gcc -O2 -mconsole -o "$R$PROBE/tone_clear.exe" tests/win/tone.c -lwinmm || { bad "build tone"; continue; }
        scripts/audio.sh start >/dev/null || { bad "tone: no PulseAudio (scripts/audio.sh)"; continue; }
        run_probe tone &
        pid=$! seen=0
        for _ in $(seq 1 600); do
            "$PACTL" -s "unix:$R/tmp/pulse/native" list sink-inputs 2>/dev/null | grep -q tone_clear && { seen=1; break; }
            kill -0 $pid 2>/dev/null || break
            sleep 0.5
        done
        wait $pid
        [ $seen = 1 ] && ok "tone: a stream from the container reached PulseAudio" ||
            bad "tone: no stream (log $LOGS/steampath-tone.log)"
        ;;
    xinput)
        x86_64-w64-mingw32-gcc -O2 -mconsole -o "$R$PROBE/xinput_clear.exe" tests/win/xinput.c -lxinput || { bad "build xinput"; continue; }
        RES="$R$PFX/pfx/drive_c/windows/probe-result.txt"
        rm -f "$RES"
        if pgrep -qx steamarm-inputd; then
            echo "  (steamarm-inputd is running: stop it first, the probe needs the fake pad)"
            bad "xinput"; continue
        fi
        python3 tests/elf/fake_inputd.py 300 > "$LOGS/steampath-fake-inputd.log" 2>&1 &
        fake=$!
        sleep 1
        run_probe xinput
        kill $fake 2>/dev/null; wait $fake 2>/dev/null
        rm -f /tmp/lxrt-input/event0 /tmp/lxrt-input/meta/event0
        if grep -q "== xinput probe: ok" "$RES" 2>/dev/null; then
            r=$(grep -c "^rumble strong=[1-9]" "$LOGS/steampath-fake-inputd.log")
            ok "xinput: $(head -1 "$RES"); rumble records at the daemon: $r"
        else
            bad "xinput: $(head -1 "$RES" 2>/dev/null) (log $LOGS/steampath-xinput.log)"
        fi
        ;;
    d3d11|d3d12)
        x86_64-w64-mingw32-gcc -O2 -mconsole -o "$R$PROBE/${t}_clear.exe" "tests/win/${t}_clear.c" \
            -l"$t" -ldxgi -luser32 -lgdi32 -luuid -ldxguid || { bad "build $t"; continue; }
        # The probe appends to probe-result.txt in its working directory; Wine
        # makes that C:\windows when the Unix one is not in the container.
        results=("$R$PROBE/probe-result.txt" "$R$PFX/pfx/drive_c/windows/probe-result.txt")
        rm -f "${results[@]}"
        start=$SECONDS
        run_probe "$t" &
        pid=$!
        while kill -0 $pid 2>/dev/null && [ $((SECONDS - start)) -lt "$PROBE_TIMEOUT" ]; do sleep 1; done
        timed_out=0
        ! kill -0 $pid 2>/dev/null || timed_out=1
        left=$(ours)
        if [ -n "$left" ]; then
            echo "  (stopping this run's guests: $left)"
            # shellcheck disable=SC2086
            kill -TERM $left 2>/dev/null; sleep 2
            left=$(ours)
            # shellcheck disable=SC2086
            [ -z "$left" ] || kill -KILL $left 2>/dev/null
        fi
        wait $pid 2>/dev/null; rc=$?
        took=$((SECONDS - start)) res=""
        for r in "${results[@]}"; do [ -f "$r" ] && res=$r && break; done
        [ -z "$res" ] || { echo "--- $res"; cat "$res"; } >> "$LOGS/steampath-$t.log"
        ended="exited after $took s, rc $rc"
        [ $timed_out = 0 ] || ended="did NOT exit: stopped after $took s"
        if [ -n "$res" ] && grep -q "== $t probe: ok" "$res" && [ $timed_out = 0 ]; then
            ok "$t: $(grep -ho '[0-9.]* fps' "$res" | head -1) ($PROTON_TOOL, $SLR); $ended"
        else
            bad "$t ($PROTON_TOOL, $SLR; result: $(grep -ho '[0-9.]* fps\|probe: [a-zA-Z]*' "$res" 2>/dev/null | tr '\n' ' ')$ended; log $LOGS/steampath-$t.log)"
        fi
        ;;
    *) bad "unknown probe $t" ;;
    esac
done
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
