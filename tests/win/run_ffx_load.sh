#!/bin/bash
# Proof 2 of the FSR 3.1 -> MetalFX design: which amd_fidelityfx_dx12.dll
# Wine/Proton gives a game that ships its own, when SteamARM puts a Wine
# builtin of that name in WINEDLLPATH (tools/ffx-metalfx). tests/win/ffx_load.c
# loads the DLL from a folder that holds a "game" copy and prints who answered
# ffxQuery and GetModuleFileNameW.
#
#   tests/win/run_ffx_load.sh [wine] [steam]       (default: both)
#
# wine   Proton Experimental's wine straight under FEX, as tests/win/run.sh
#        runs its probes, in a prefix of its own (/tmp/fexhome/ffxload-pfx):
#        the load-order cases below.
# steam  the way Steam launches a game: the Steam Linux Runtime container
#        (pressure-vessel, _v2-entry-point) around "proton waitforexitandrun",
#        as tests/win/run_steam_path.sh; the builtin in a folder of the x86
#        root that pressure-vessel's plan does not bind (/opt/...), without
#        and with PRESSURE_VESSEL_FILESYSTEMS_RO. The folder is removed after.
#        MEASURED (2026-10-04): SteamARM's container shows it either way --
#        pv binds nothing there, but FEX's rootfs is the x86 root
#        (FEX_ROOTFS=/), so the path still resolves; on a stock Linux
#        pressure-vessel only the variable would bring it in.
#
# Each case prints "ok"/"FAIL" against what Wine's loader code says should
# happen; logs in $STEAMARM_STATE/logs (default ~/SteamARM-roots/logs),
# ffxload-<case>.log. Only this run's processes are ever stopped (by the
# STEAMARM_RUN_ID they inherit).
set -u
cd "$(dirname "$0")/../.." || exit 1
. scripts/roots.sh
R=/tmp/lxrt-steamroot
G=/tmp/fexhome/.local/share/Steam
PROTON_TOOL="Proton - Experimental"
PROTON_GUEST="$G/steamapps/common/$PROTON_TOOL/files"
PFX_GUEST=/tmp/fexhome/ffxload-pfx
T=/tmp/ffxload                              # guest folder of the wine cases
SPROBE="$G/steamapps/common/SteamARM-FFXLoad"   # the steam cases' "game"
SPFX="$G/steamapps/compatdata/999996"
OPT=/opt/steamarm-ffxtest/wine              # builtin folder for the steam cases
LOGS="${STEAMARM_STATE:-$HOME/SteamARM-roots}/logs"
RUN_ID="ffxload.$$.$(date +%s)"
mkdir -p "$LOGS"
[ -x "$R$PROTON_GUEST/bin/wine" ] || { echo "no $PROTON_TOOL in the Steam root" >&2; exit 1; }
scripts/run-x11-native.sh start :2 >/dev/null || exit 1

ours() {   # this run's guests
    local pids
    pids=$(ps -Ao pid=,comm= | awk '$NF ~ /(^|\/)lxrun$/ {print $1}')
    [ -n "$pids" ] || return 0
    # shellcheck disable=SC2086
    procs_env STEAMARM_RUN_ID $pids | awk -v id="$RUN_ID" '$2 == id { printf "%s ", $1 }'
}
stop_ours() {
    local left
    left=$(ours)
    [ -n "$left" ] || return 0
    echo "        (stopping this run's guests: $left)"
    # shellcheck disable=SC2086
    kill -TERM $left 2>/dev/null; sleep 2
    left=$(ours)
    # shellcheck disable=SC2086
    [ -z "$left" ] || kill -KILL $left 2>/dev/null
}
wait_limit() {   # wait_limit PID SECONDS: 1 if it had to be stopped
    local pid=$1 limit=$2 start=$SECONDS
    while kill -0 "$pid" 2>/dev/null && [ $((SECONDS - start)) -lt "$limit" ]; do sleep 1; done
    if kill -0 "$pid" 2>/dev/null; then stop_ours; wait "$pid" 2>/dev/null; return 1; fi
    wait "$pid" 2>/dev/null
    return 0
}

# ------------------------------------------------------------ the DLLs
build() {   # build OUTFILE [VAR=value...]: tools/ffx-metalfx/build.sh with settings
    local out=$1; shift
    env "$@" FFX_OUT="$out" tools/ffx-metalfx/build.sh >/dev/null || { echo "build failed: $out" >&2; exit 1; }
}
rm -rf "${R:?}$T"
for d in game nogame dllpath/empty; do mkdir -p "$R$T/$d"; done
build "$R$T/dllpath/ours/x86_64-windows/amd_fidelityfx_dx12.dll"
build "$R$T/dllpath/ours-nopn/x86_64-windows/amd_fidelityfx_dx12.dll" FFX_PREFER_NATIVE=0
build "$R$T/dllpath/ours-unmarked/x86_64-windows/amd_fidelityfx_dx12.dll" FFX_MARK=0
build "$R$T/dllpath/ours-flat/amd_fidelityfx_dx12.dll"
build "$R$T/game/amd_fidelityfx_dx12.dll" FFX_MARK=0 FFX_IDENT=game-native
x86_64-w64-mingw32-gcc -O2 -Wall -mconsole -o "$R$T/game/ffx_load.exe" tests/win/ffx_load.c || exit 1
cp "$R$T/game/ffx_load.exe" "$R$T/nogame/"

PASS=0 FAIL=0
check() {   # check CASE LOG WANT-WHO WHY
    local who
    who=$(sed -n 's/.*== ffx_load probe: done answered=//p' "$2" | tail -1 | tr -d '\r')
    if [ "$who" = "$3" ]; then echo "  ok    $1: $who ($4)"; PASS=$((PASS + 1))
    else echo "  FAIL  $1: answered '${who:-nothing}', wanted $3 ($4; log $2)"; FAIL=$((FAIL + 1)); fi
    grep -a -E '^(GetModuleFileNameW|LoadLibraryW|copy|exists|mapped)' "$2" | tr -d '\r' | sed 's/^/        /'
    grep -a -E 'Loaded .*amd_fidelityfx|amd_fidelityfx.*(not a builtin|prefer-native|got |looking for)' "$2" |
        tr -d '\r' | sed 's/^[0-9a-f]*:[0-9a-f]*://; s/^/        wine: /' | head -6
}

# ------------------------------------------------------------ wine cases
wine_case() {   # wine_case NAME WANT WHY WINEDLLPATH OVERRIDES EXEDIR MODE
    local name=$1 want=$2 why=$3 dllpath=$4 ovr=$5 exedir=$6 mode=$7 log="$LOGS/ffxload-$1.log"
    local e=(env -u WINEDLLOVERRIDES -u WINEDLLPATH STEAMARM_RUN_ID="$RUN_ID" DISPLAY=:2
             WINEPREFIX=$PFX_GUEST WINEDEBUG=-all,+loaddll,+module)
    [ -z "$dllpath" ] || e+=(WINEDLLPATH="$dllpath")
    [ -z "$ovr" ] || e+=(WINEDLLOVERRIDES="$ovr")
    "${e[@]}" LXRT_ROOT=$R FEX_ROOTFS=/ scripts/run-fex.sh "$PROTON_GUEST/bin/wine" \
        "Z:\\tmp\\ffxload\\$exedir\\ffx_load.exe" "$mode" > "$log" 2>&1 &
    wait_limit $! 120 || echo "        (timeout)"
    check "$name" "$log" "$want" "$why"
}
run_wine() {
    if [ ! -d "$R$PFX_GUEST/drive_c/windows/system32" ]; then
        echo "  (creating the prefix $PFX_GUEST)"
        env -u WINEDLLOVERRIDES -u WINEDLLPATH STEAMARM_RUN_ID="$RUN_ID" DISPLAY=:2 WINEPREFIX=$PFX_GUEST \
            WINEDEBUG=-all LXRT_ROOT=$R FEX_ROOTFS=/ \
            scripts/run-fex.sh "$PROTON_GUEST/bin/wine" wineboot -i > "$LOGS/ffxload-wineboot.log" 2>&1 &
        wait_limit $! 300 || echo "        (wineboot timeout)"
    fi
    local O=$T/dllpath
    wine_case override       steamarm-builtin "b,n: builtin from WINEDLLPATH over the game's file" \
        "$O/ours" "amd_fidelityfx_dx12=b,n" game name
    wine_case override-path  steamarm-builtin "b,n, loaded by full path" \
        "$O/ours" "amd_fidelityfx_dx12=b,n" game path
    wine_case no-override    game-native "no override: the builtin has the prefer-native flag" \
        "$O/ours" "" game name
    wine_case nopn-no-override steamarm-builtin "no override, builtin WITHOUT prefer-native: load order 'default' takes it" \
        "$O/ours-nopn" "" game name
    wine_case missing        game-native "b,n, no builtin in WINEDLLPATH: falls back to the game's" \
        "$O/empty" "amd_fidelityfx_dx12=b,n" game name
    wine_case unmarked       game-native "b,n, WINEDLLPATH file without the marker is ignored" \
        "$O/ours-unmarked" "amd_fidelityfx_dx12=b,n" game name
    wine_case flat           steamarm-builtin "b,n, builtin at WINEDLLPATH/<name> (no x86_64-windows/)" \
        "$O/ours-flat" "amd_fidelityfx_dx12=b,n" game name
    wine_case no-game-file   none "b,n, no file anywhere: Wine looks in WINEDLLPATH only for a file it found" \
        "$O/ours" "amd_fidelityfx_dx12=b,n" nogame name
    wine_case native-only    game-native "override n: the game's" \
        "$O/ours" "amd_fidelityfx_dx12=n" game name
    env -u WINEDLLOVERRIDES STEAMARM_RUN_ID="$RUN_ID" WINEPREFIX=$PFX_GUEST WINEDEBUG=-all LXRT_ROOT=$R FEX_ROOTFS=/ \
        scripts/run-fex.sh "$PROTON_GUEST/bin/wineserver" -k >/dev/null 2>&1
    sleep 2; stop_ours
}

# ------------------------------------------------------------ steam cases
run_steam() {
    local SLR appid
    appid=$(sed -n 's/.*"require_tool_appid"[[:space:]]*"\([0-9]*\)".*/\1/p' \
            "$R$G/steamapps/common/$PROTON_TOOL/toolmanifest.vdf" 2>/dev/null | head -1)
    case "$appid" in
        1628350) SLR=SteamLinuxRuntime_sniper ;;
        4183110) SLR=SteamLinuxRuntime_4 ;;
        *) echo "  FAIL  steam: no known runtime for $PROTON_TOOL (require_tool_appid '$appid')"; FAIL=$((FAIL + 1)); return ;;
    esac
    mkdir -p "$R$SPROBE" "$R$SPFX" "$R$OPT/x86_64-windows"
    cp "$R$T/game/ffx_load.exe" "$R$T/game/amd_fidelityfx_dx12.dll" "$R$SPROBE/"
    cp "$R$T/dllpath/ours/x86_64-windows/amd_fidelityfx_dx12.dll" "$R$OPT/x86_64-windows/"
    local winpath="Z:${OPT//\//\\}\\x86_64-windows\\amd_fidelityfx_dx12.dll" n=0
    steam_case() {   # steam_case NAME WANT WHY [VAR=value...]
        local name=$1 want=$2 why=$3 log="$LOGS/ffxload-$1.log" limit=300
        shift 3
        n=$((n + 1)); [ $n = 1 ] && [ ! -d "$R$SPFX/pfx" ] && limit=600   # the first run makes the prefix
        rm -f "$R$SPROBE/ffx_load-result.txt" "$R/tmp/fexhome/steam-999996.log"
        env -u WINEDLLOVERRIDES -u WINEDLLPATH -u PRESSURE_VESSEL_FILESYSTEMS_RO \
            STEAMARM_RUN_ID="$RUN_ID" DISPLAY=:2 PULSE_SERVER=unix:/tmp/pulse/native \
            STEAM_COMPAT_CLIENT_INSTALL_PATH=$G STEAM_COMPAT_DATA_PATH=$SPFX \
            STEAM_COMPAT_INSTALL_PATH=$SPROBE SteamAppId=999996 SteamGameId=999996 PROTON_LOG=1 \
            WINEDLLPATH=$OPT WINEDLLOVERRIDES="amd_fidelityfx_dx12=b,n" "$@" \
            LXRT_ROOT=$R FEX_ROOTFS=/ scripts/run-fex.sh /bin/bash \
            "$G/steamapps/common/$SLR/_v2-entry-point" --verb=waitforexitandrun -- \
            "$G/steamapps/common/$PROTON_TOOL/proton" waitforexitandrun "$SPROBE/ffx_load.exe" name "$winpath" \
            > "$log" 2>&1 &
        wait_limit $! $limit || echo "        (timeout after $limit s)"
        { echo "--- ffx_load-result.txt"; cat "$R$SPROBE/ffx_load-result.txt" 2>/dev/null
          echo "--- steam-999996.log (PROTON_LOG)"; cat "$R/tmp/fexhome/steam-999996.log" 2>/dev/null; } >> "$log"
        check "$name" "$log" "$want" "$why ($SLR)"
        grep -a -E '^(Effective|System) WINEDLL' "$log" | sed 's/^/        proton: /'
    }
    steam_case steam-no-fs   steamarm-builtin "WINEDLLPATH=$OPT, b,n, no PRESSURE_VESSEL_FILESYSTEMS_RO: still seen through FEX's rootfs"
    steam_case steam-fs-ro   steamarm-builtin "WINEDLLPATH=$OPT, b,n, PRESSURE_VESSEL_FILESYSTEMS_RO=$OPT" \
        PRESSURE_VESSEL_FILESYSTEMS_RO=$OPT
    stop_ours
    rm -rf "${R:?}/opt/steamarm-ffxtest"
}

for what in ${@:-wine steam}; do
    case $what in
        wine) run_wine ;;
        steam) run_steam ;;
        *) echo "unknown: $what" >&2; FAIL=$((FAIL + 1)) ;;
    esac
done
echo "== $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
