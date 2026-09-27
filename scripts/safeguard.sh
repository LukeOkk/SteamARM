#!/bin/bash
# Keep a runaway SteamARM guest from taking the Mac down (two watchdog hangs on
# 2026-09-27 during a Wine test; the first panic log showed the compressor at
# 100%, 2.8 GB of kernel "vm objects", fseventsd at 5.9 GB, 130 lxrun
# processes and the old VM still running).
#
#   scripts/safeguard.sh start      run detached (survives the calling shell)
#   scripts/safeguard.sh stop | status
#
# Every second it checks, and kills EVERY guest (build/lxrun) process when any
# limit is crossed:
#   - kern.memorystatus_level (free memory %) below SAFEGUARD_MIN_FREE (35)
#   - fseventsd resident size above SAFEGUARD_FSEVENTS_MB (1500)
#   - guests' total resident size above the launcher's DRAM setting
#     ($STATE/launcher/limits.env) or SAFEGUARD_GUEST_MB (8192 without either)
#   - more than SAFEGUARD_MAX_PROCS (80) lxrun processes
#   - kernel VM objects above SAFEGUARD_MAX_VMOBJ (1500000, ~380 MB of kernel
#     zone) or VM map entries above SAFEGUARD_MAX_MAPENT (1500000): kernel
#     memory no RSS figure shows, and what the panicking kernel had run out of
#     (the "vm objects" zone was 2856 MB)
# A status line goes to $STEAMARM_STATE/logs/safeguard.log (~/SteamARM-roots) every 5 s (synced
# to disk), so a hang leaves a trail.
set -u
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
LOG="$STATE/logs/safeguard.log"
PIDF="$STATE/safeguard.pid"
MIN_FREE="${SAFEGUARD_MIN_FREE:-35}"
FSE_MB="${SAFEGUARD_FSEVENTS_MB:-1500}"
GUEST_MB="${SAFEGUARD_GUEST_MB:-8192}"
MAX_PROCS="${SAFEGUARD_MAX_PROCS:-80}"
MAX_VMOBJ="${SAFEGUARD_MAX_VMOBJ:-1500000}"
MAX_MAPENT="${SAFEGUARD_MAX_MAPENT:-1500000}"
mkdir -p "$STATE/logs"

running() { [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }

case "${1:-}" in
    start)
        if running; then echo "safeguard running (pid $(cat "$PIDF"))"; exit 0; fi
        nohup "$0" run </dev/null >>"$LOG" 2>&1 &
        echo $! > "$PIDF"
        disown 2>/dev/null
        echo "safeguard started (pid $!), log $LOG"
        exit 0 ;;
    stop)
        running && kill "$(cat "$PIDF")"; rm -f "$PIDF"; echo "safeguard stopped"; exit 0 ;;
    status)
        running && echo "running (pid $(cat "$PIDF"))" || echo "not running"
        tail -3 "$LOG" 2>/dev/null; exit 0 ;;
    run) ;;
    *) sed -n '2,20p' "$0"; exit 2 ;;
esac

# Guests are the processes whose executable is lxrun. Matching "build/lxrun"
# anywhere in the command line also killed shells that merely mentioned it.
guest_pids() { ps -Ao pid=,comm= | awk '$2 ~ /(^|\/)lxrun$/ {print $1}'; }

kill_guests() {
    echo "$(date '+%F %T') KILL: $1"
    guest_pids | xargs kill -9 2>/dev/null
    sync
}

n=0
while true; do
    # The launcher's DRAM setting (scripts/settings-env.py writes it); an
    # explicit SAFEGUARD_GUEST_MB wins.
    if [ -z "${SAFEGUARD_GUEST_MB:-}" ] && [ -r "$STATE/launcher/limits.env" ]; then
        g=$(sed -n 's/^GUEST_MAX_MB=\([0-9][0-9]*\)$/\1/p' "$STATE/launcher/limits.env")
        [ -n "$g" ] && GUEST_MB=$g
    fi
    level=$(sysctl -n kern.memorystatus_level 2>/dev/null || echo 100)
    read -r gcount grss <<<"$(ps -Ao rss=,comm= | awk '$2 ~ /(^|\/)lxrun$/ {c++; s+=$1} END {print c+0, int((s+0)/1024)}')"
    fse=$(ps -Ao rss,comm | awk '/fseventsd$/ {print int($1/1024); exit}')
    fse=${fse:-0}
    read -r vmobj mapent <<<"$(zprint 2>/dev/null | awk '$1=="vm.objects" {o=$7} $1=="VM.map.entries" {e=$7} END {print o+0, e+0}')"
    if [ "$level" -lt "$MIN_FREE" ]; then kill_guests "free memory ${level}% < ${MIN_FREE}%"
    elif [ "$fse" -gt "$FSE_MB" ]; then kill_guests "fseventsd ${fse} MB > ${FSE_MB} MB"
    elif [ "$grss" -gt "$GUEST_MB" ]; then kill_guests "guests ${grss} MB > ${GUEST_MB} MB"
    elif [ "$gcount" -gt "$MAX_PROCS" ]; then kill_guests "${gcount} lxrun processes > ${MAX_PROCS}"
    elif [ "$vmobj" -gt "$MAX_VMOBJ" ]; then kill_guests "kernel vm.objects ${vmobj} > ${MAX_VMOBJ}"
    elif [ "$mapent" -gt "$MAX_MAPENT" ]; then kill_guests "kernel VM.map.entries ${mapent} > ${MAX_MAPENT}"
    fi
    n=$((n + 1))
    # FEXServer is shared by every x86 program. If it is gone while guests
    # run (it crashed, or an older one timed out), a game started from Steam
    # dies in 5 s with "Couldn't connect to FEXServer socket" (MEASURED):
    # bring it back the way run-fex.sh starts it.
    if [ $((n % 5)) -eq 0 ] && [ "$gcount" -gt 0 ] && ! pgrep -f 'lxrun .*FEXServer' >/dev/null; then
        echo "$(date '+%F %T') FEXServer missing with ${gcount} guests: restarting it"
        (cd "$(dirname "$0")/.." && scripts/run-fex.sh /bin/true >/dev/null 2>&1 &)
    fi
    if [ $((n % 5)) -eq 0 ]; then
        echo "$(date '+%F %T') free=${level}% guests=${gcount} rss=${grss}MB fseventsd=${fse}MB vmobj=${vmobj} mapent=${mapent}"
        sync
    fi
    sleep 1
done
