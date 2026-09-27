#!/bin/bash
# Windows probes launched the way Steam launches a game: the Steam Linux
# Runtime container (pressure-vessel) around Proton, no VM.
#
#   tone     sound: waveOut -> winepulse -> the container's PulseAudio socket
#            (the bind pressure-vessel makes of scripts/audio.sh's socket)
#   xinput   a controller: XInput -> winebus -> SDL -> /dev/input/event0
#            (runtime/evdev.c), fed by the fake daemon tests/elf/fake_inputd.py
#            -- or by the real steamarm-inputd with --fake xbox360
#
#   tests/win/run_steam_path.sh [tone] [xinput]
#
# Needs: Steam installed in the Steam root with Proton Experimental and
# Steam Linux Runtime 4 (SteamLinuxRuntime_4), mingw-w64, PulseAudio (for tone).
set -u
cd "$(dirname "$0")/../.." || exit 1
R=/tmp/lxrt-steamroot
G=/tmp/fexhome/.local/share/Steam
PROBE=$G/steamapps/common/SteamARM-Probe
PFX=$G/steamapps/compatdata/999999
LOGS="${STEAMARM_STATE:-$HOME/SteamARM-roots}/logs"
PACTL=/opt/homebrew/opt/pulseaudio/bin/pactl
mkdir -p "$LOGS" "$R$PROBE" "$R$PFX"
[ -f "$R$G/steamapps/common/SteamLinuxRuntime_4/_v2-entry-point" ] ||
    { echo "no Steam Linux Runtime 4 in the Steam root" >&2; exit 1; }
scripts/run-x11-native.sh start :2 >/dev/null || exit 1

run_probe() {   # $1 = probe name; the game's launch line, as Steam builds it
    DISPLAY=:2 PULSE_SERVER=unix:/tmp/pulse/native \
    STEAM_COMPAT_CLIENT_INSTALL_PATH=$G STEAM_COMPAT_DATA_PATH=$PFX \
    STEAM_COMPAT_INSTALL_PATH=$PROBE SteamAppId=999999 SteamGameId=999999 \
    LXRT_ROOT=$R FEX_ROOTFS=/ scripts/run-fex.sh /bin/bash \
        "$G/steamapps/common/SteamLinuxRuntime_4/_v2-entry-point" --verb=waitforexitandrun -- \
        "$G/steamapps/common/Proton - Experimental/proton" waitforexitandrun "$PROBE/$1_clear.exe" \
        > "$LOGS/steampath-$1.log" 2>&1
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
    *) bad "unknown probe $t" ;;
    esac
done
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
