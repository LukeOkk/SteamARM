#!/bin/bash
# Windows probes launched through "SteamARM: Proton x86 via FEX", the
# compatibility tool of the native arm64 Steam client (tools/steamarm-fex-proton,
# docs/FEX_GAME_BOUNDARY.md), invoked the way the client invokes a tool:
# from an aarch64 process in the client's ARM64 root, under the client's own
# reaper, with Steam's STEAM_COMPAT_* environment and the game's directory as
# the working directory:
#
#   <Steam>/steamrtarm64/reaper SteamLaunch AppId=<id> -- \
#       <Steam>/compatibilitytools.d/steamarm-fex-proton/steamarm-fex-proton waitforexitandrun <exe>
#
# No Steam UI, no sign-in and no game: the probes are tests/win's programs,
# placed in the client's library as steamapps/common/SteamARM-Probe.
#
#   tests/win/run_fex_boundary.sh [d3d11] [d3d12] [tone]     (default: d3d11 d3d12)
#
# ARMROOT    armroot (default) or arm64root: the ARM64 root and its client
# PROTON     the x86 Proton the tool is installed with (default "Proton - Experimental")
# APPID      the probe's app id, and so its prefix (default 999999)
# NO_REAPER=1       run the tool directly, without the client's reaper
# PROBE_TIMEOUT     seconds per probe (default 300); then only this run's
#                   processes are stopped (by their STEAMARM_RUN_ID)
#
# Writes: the tool (scripts/install-fex-proton-tool.sh), the probes and the
# prefix, all under <ARMROOT>/tmp/armhome/.local/share/Steam. Nothing in the
# x86 Steam root is written by this script; running x86 Proton and its
# runtime leaves their own temporary files there, as a run by the x86 client does.
set -u
cd "$(dirname "$0")/../.." || exit 1
. scripts/roots.sh
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
case "${ARMROOT:-armroot}" in
    armroot) HOSTROOT="$STATE/armroot" LINK=/tmp/lxrt-armroot ;;
    arm64root) HOSTROOT="$STATE/arm64root" LINK=/tmp/lxrt-arm64root ;;
    *) echo "ARMROOT is armroot or arm64root" >&2; exit 2 ;;
esac
PROTON="${PROTON:-Proton - Experimental}"
APPID="${APPID:-999999}"
PROBE_TIMEOUT="${PROBE_TIMEOUT:-300}"
G=/tmp/armhome/.local/share/Steam
PROBE=$G/steamapps/common/SteamARM-Probe
PFX=$G/steamapps/compatdata/$APPID
TOOL=$G/compatibilitytools.d/steamarm-fex-proton/steamarm-fex-proton
LOGS="$STATE/logs"
mkdir -p "$LOGS"
[ -f "$HOSTROOT$G/steamrtarm64/steam" ] || { echo "no native arm64 client in $HOSTROOT" >&2; exit 1; }
# A Proton other than the measured one is installed --unverified: measuring
# it is what this script is for.
inst=(--root "$HOSTROOT" --proton "$PROTON")
[ "$PROTON" = "Proton - Experimental" ] || inst+=(--unverified)
scripts/install-fex-proton-tool.sh "${inst[@]}" >/dev/null || exit 1
scripts/run-x11-native.sh start :2 >/dev/null || exit 1
mkdir -p "$HOSTROOT$PROBE" "$HOSTROOT$PFX"
if [ "${NO_REAPER:-}" = 1 ]; then LAUNCH=("$TOOL")
else LAUNCH=("$G/steamrtarm64/reaper" SteamLaunch "AppId=$APPID" -- "$TOOL"); fi

RUN_ID="fex-boundary.$$.$(date +%s)"
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
    echo "        (stopping this run's guests: $o)"
    # shellcheck disable=SC2086
    kill -TERM $o 2>/dev/null; sleep 2
    o=$(ours)
    # shellcheck disable=SC2086
    [ -z "$o" ] || kill -KILL $o 2>/dev/null
}
trap 'stop_ours; exit 130' INT TERM

run_probe() {   # $1 = probe name; the launch line, from an aarch64 bash in the ARM64 root
    # The client's own environment for itself (4 KiB pages, libcef's x18
    # pass) is set too: the tool must not hand it to FEX.
    STEAMARM_RUN_ID=$RUN_ID LXRT_ROOT=$LINK HOME_IN_GUEST=/tmp/armhome \
    LXRT_GUEST_PAGE=4096 LXRT_X18_ALL_TEXT=libcef.so \
    DISPLAY=:2 PULSE_SERVER=unix:/tmp/pulse/native \
    STEAM_COMPAT_CLIENT_INSTALL_PATH=$G STEAM_COMPAT_DATA_PATH=$PFX \
    STEAM_COMPAT_INSTALL_PATH=$PROBE STEAM_COMPAT_LIBRARY_PATHS=$G/steamapps \
    STEAM_COMPAT_SHADER_PATH=$G/steamapps/shadercache/$APPID STEAM_COMPAT_APP_ID=$APPID \
    SteamAppId=$APPID SteamGameId=$APPID SteamEnv=1 \
        scripts/run-native.sh /bin/bash -c 'cd "$1" && shift && exec "$@"' launch "$PROBE" \
        "${LAUNCH[@]}" waitforexitandrun "$PROBE/$1_clear.exe"
}

PASS=0 FAIL=0
ok()  { echo "  ok    $*"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL  $*"; FAIL=$((FAIL + 1)); }
echo "== $PROTON through the tool, root $LINK, app $APPID$([ "${NO_REAPER:-}" = 1 ] && echo ", no reaper")"
for t in ${@:-d3d11 d3d12}; do
    log="$LOGS/fexboundary-$t.log"
    exe="$HOSTROOT$PROBE/${t}_clear.exe"
    case $t in
        d3d11|d3d12)
            x86_64-w64-mingw32-gcc -O2 -mconsole -o "$exe" "tests/win/${t}_clear.c" \
                -l"$t" -ldxgi -luser32 -lgdi32 -luuid -ldxguid || { bad "build $t"; continue; } ;;
        tone)
            x86_64-w64-mingw32-gcc -O2 -mconsole -o "$exe" tests/win/tone.c -lwinmm || { bad "build tone"; continue; }
            scripts/audio.sh start >/dev/null || { bad "tone: no PulseAudio (scripts/audio.sh)"; continue; } ;;
        *) bad "unknown probe $t"; continue ;;
    esac
    results=("$HOSTROOT$PROBE/probe-result.txt" "$HOSTROOT$PFX/pfx/drive_c/windows/probe-result.txt")
    rm -f "${results[@]}"
    start=$SECONDS seen=0 timed_out=0
    run_probe "$t" > "$log" 2>&1 &
    pid=$!
    while kill -0 $pid 2>/dev/null; do
        if [ "$t" = tone ] && [ $seen = 0 ]; then
            /opt/homebrew/opt/pulseaudio/bin/pactl -s unix:/tmp/lxrt-steamroot/tmp/pulse/native \
                list sink-inputs 2>/dev/null | grep -q tone_clear && seen=1
        fi
        [ $((SECONDS - start)) -lt "$PROBE_TIMEOUT" ] || { timed_out=1; break; }
        sleep 1
    done
    if [ $timed_out = 1 ]; then
        # What kept the session alive: the Windows programs among this run's guests.
        o=$(ours)
        # shellcheck disable=SC2086
        [ -z "$o" ] || echo "        (still running: $(ps -o command= -p "$(echo $o | tr ' ' ,)" 2>/dev/null |
            grep -o '[^\\/ ]*\.exe' | sort | uniq -c | tr -s ' \n' ' '))"
    fi
    stop_ours
    wait $pid 2>/dev/null; rc=$?
    took=$((SECONDS - start))
    res=""
    for r in "${results[@]}"; do [ -f "$r" ] && res=$r && break; done
    [ -z "$res" ] || { echo "--- $res"; cat "$res"; } >> "$log"
    ended="exited after $took s, rc $rc"
    [ $timed_out = 0 ] || ended="did NOT exit: stopped after $took s"
    case $t in
        tone)
            if [ $seen = 1 ] && [ $timed_out = 0 ]; then ok "tone: a stream reached PulseAudio; $ended"
            else bad "tone: stream seen: $seen; $ended (log $log)"; fi ;;
        *)
            if [ -n "$res" ] && grep -q "== $t probe: ok" "$res"; then
                fps="$(grep -ho '[0-9.]* fps' "$res" | head -1) ($(grep -h 'frames in' "$res" | head -1))"
                # Rendering is not enough: a session that never ends is a
                # game Steam shows as running forever.
                if [ $timed_out = 0 ]; then ok "$t: $fps; $ended"
                else bad "$t: rendered, $fps, but $ended (log $log)"; fi
            else
                bad "$t: no result; $ended (log $log)"
                grep -v '^\[lxrt\]\|^[ID] ' "$log" | tail -8 | sed 's/^/        /'
            fi ;;
    esac
    grep -h "^steamarm-fex-proton:" "$log" | head -3 | sed 's/^/        /'
done
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
