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
#   - memory pressure: macOS reports critical pressure
#     (kern.memorystatus_vm_pressure_level 4) for SAFEGUARD_CRIT_SECS (2) checks
#     in a row, or free memory (kern.memorystatus_level) stays below
#     SAFEGUARD_MIN_FREE (12 %) for SAFEGUARD_LOW_SECS (3) checks. The largest
#     guest (usually the game) is stopped first; the rest only if the pressure
#     is still there SAFEGUARD_GRACE_SECS (5) later.
#     The old rule (free < 35 %, one sample) stopped Steam on a 16 GB Mac at
#     34 % free with 2.7 GB of guests and normal pressure (0.3.4, MEASURED).
#   - fseventsd resident size above SAFEGUARD_FSEVENTS_MB (1500)
#   - guests' total resident size above the launcher's DRAM setting
#     ($STATE/launcher/limits.env) or SAFEGUARD_GUEST_MB (8192 without either)
#   - more than SAFEGUARD_MAX_PROCS (80) lxrun processes, plus
#     SAFEGUARD_ANDROID_PROCS (100) while an Android session runs
#     ($STATE/android/session.json names a live boot, scripts/android-session.py):
#     Android is one lxrun process per service and per app, 60-80 of them
#     after its boot (MEASURED, benchmarks/stage28-android-apk.txt), and the
#     old limit stopped the session, and every other guest, during its first
#     pm install; the kernel-object and memory rules still apply to it
#   - kernel VM objects above SAFEGUARD_MAX_VMOBJ (1500000, ~380 MB of kernel
#     zone) or VM map entries above SAFEGUARD_MAX_MAPENT (1500000): kernel
#     memory no RSS figure shows, and what the panicking kernel had run out of
#     (the "vm objects" zone was 2856 MB)
# All but the first (fseventsd, guest RSS, process count, kernel objects) stop
# every guest at once, and are checked before the memory rule, so its grace
# period never delays them.
# A status line goes to $STEAMARM_STATE/logs/safeguard.log (~/SteamARM-roots) every 5 s.
# Every stop is also written to logs/safeguard.last ("<epoch> <reason>") and
# to the running app's log, so the launcher can say why the app ended.
# Flush only that log: macOS sync(8) flushes every filesystem and can stall
# game I/O at the same five-second cadence.
set -u
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
LOG="$STATE/logs/safeguard.log"
PIDF="$STATE/safeguard.pid"
MIN_FREE="${SAFEGUARD_MIN_FREE:-12}"
LOW_SECS="${SAFEGUARD_LOW_SECS:-3}"
CRIT_SECS="${SAFEGUARD_CRIT_SECS:-2}"
GRACE_SECS="${SAFEGUARD_GRACE_SECS:-5}"
FSE_MB="${SAFEGUARD_FSEVENTS_MB:-1500}"
GUEST_MB="${SAFEGUARD_GUEST_MB:-8192}"
MAX_PROCS="${SAFEGUARD_MAX_PROCS:-80}"
ANDROID_PROCS="${SAFEGUARD_ANDROID_PROCS:-100}"
MAX_VMOBJ="${SAFEGUARD_MAX_VMOBJ:-1500000}"
MAX_MAPENT="${SAFEGUARD_MAX_MAPENT:-1500000}"
mkdir -p "$STATE/logs"

running() { [ -f "$PIDF" ] && kill -0 "$(cat "$PIDF")" 2>/dev/null; }

case "${1:-}" in
    start)
        if running; then echo "safeguard running (pid $(cat "$PIDF"))"; exit 0; fi
        # Its own session (scripts/session.py detach): started from an app's
        # launch, it must outlive that app's process group.
        nohup env PYTHONCOERCECLOCALE=0 STEAMARM_SESSION_PY=1 \
            /usr/bin/python3 "$(dirname "$0")/session.py" detach "$0" run </dev/null >>"$LOG" 2>&1 &
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
# Match the LAST field: comm is the path lxrun was exec'd by, and a release
# lives under "~/Library/Application Support/SteamARM/src"; with a space in
# that path, $2 would miss the guest and the limits below would not see it.
# SAFEGUARD_GUEST_NAME: tests/launcher/safeguard.sh uses its own name so it
# never touches real guests.
GUEST_NAME="${SAFEGUARD_GUEST_NAME:-lxrun}"
guest_pids() { ps -Ao pid=,comm= | awk -v n="$GUEST_NAME" '$NF == n || $NF ~ ("/" n "$") {print $1}'; }

# An x86 guest is an lxrun process running FEX: run-fex.sh starts lxrun FEX-gb,
# and FEX re-execs itself for every x86 exec (runtime/process.c, lxrt_execve),
# so a FEX path is in its command line. FEXServer does not count. An aarch64
# session (scripts/run-native.sh) has none, and must not get a FEXServer.
has_x86_guest() {
    local p
    for p in $(guest_pids); do
        ps -o command= -p "$p" 2>/dev/null |
            grep -qE '/FEX(Interpreter|Loader)?(-[A-Za-z0-9]+)?( |$)' && return 0
    done
    return 1
}

# The detached guard's stdout is the log, opened by start above. fsync(1)
# preserves crash diagnostics without forcing writes from every process and
# volume. A direct interactive "run" has a terminal on fd 1; ignore EINVAL.
flush_log() {
    /usr/bin/python3 -c 'import os; os.fsync(1)' 2>/dev/null || true
}

# Tell the launcher (logs/safeguard.last) and the app's own log why guests
# were stopped: without it the user only sees "terminó con la señal 9".
note_stop() {
    echo "$(date +%s) $1" > "$STATE/logs/safeguard.last"
    local cur
    cur=$(cat "$STATE/logs/current" 2>/dev/null)
    [ -n "$cur" ] && [ -f "$cur" ] && echo "[safeguard] $(date '+%F %T') $1" >> "$cur"
}

kill_guests() {
    echo "$(date '+%F %T') KILL: $1"
    note_stop "$1"
    guest_pids | xargs kill -9 2>/dev/null
    flush_log
}

# The guest with the largest resident size (pid rss_MB command), or nothing.
largest_guest() {
    ps -Ao pid=,rss=,comm= | awk -v n="$GUEST_NAME" '($NF == n || $NF ~ ("/" n "$")) && $2 > m {m = $2; p = $1} END {if (p) print p, int(m / 1024)}'
}

# Memory pressure: stop the largest guest first, everything only if the
# pressure outlasts the grace period.
relieve_memory() {
    local now big
    now=$(date +%s)
    if [ "$first_stop" -gt 0 ] && [ $((now - first_stop)) -ge "$GRACE_SECS" ]; then
        kill_guests "$1 (still after stopping the largest guest)"
        first_stop=0
        return
    fi
    [ "$first_stop" -gt 0 ] && return
    read -r big bigmb <<<"$(largest_guest)"
    [ -z "$big" ] && return
    local what
    what=$(ps -o command= -p "$big" 2>/dev/null | cut -c1-160)
    echo "$(date '+%F %T') STOP largest guest: $1: pid $big ${bigmb} MB: $what"
    note_stop "$1: stopped the largest guest (pid $big, ${bigmb} MB)"
    kill -9 "$big" 2>/dev/null
    first_stop=$now
    flush_log
}

n=0
low=0
crit=0
first_stop=0
warned=0
while true; do
    # The launcher's DRAM setting (scripts/settings-env.py writes it); an
    # explicit SAFEGUARD_GUEST_MB wins.
    if [ -z "${SAFEGUARD_GUEST_MB:-}" ] && [ -r "$STATE/launcher/limits.env" ]; then
        g=$(sed -n 's/^GUEST_MAX_MB=\([0-9][0-9]*\)$/\1/p' "$STATE/launcher/limits.env")
        [ -n "$g" ] && GUEST_MB=$g
    fi
    level=$(sysctl -n kern.memorystatus_level 2>/dev/null || echo 100)
    pressure=$(sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo 1)
    read -r gcount grss <<<"$(ps -Ao rss=,comm= | awk -v n="$GUEST_NAME" '$NF == n || $NF ~ ("/" n "$") {c++; s+=$1} END {print c+0, int((s+0)/1024)}')"
    maxp=$MAX_PROCS
    apid=$(sed -n 's/.*"bootPid": *\([0-9][0-9]*\).*/\1/p' "$STATE/android/session.json" 2>/dev/null | head -1)
    if [ -n "$apid" ] && kill -0 "$apid" 2>/dev/null; then maxp=$((MAX_PROCS + ANDROID_PROCS)); fi
    # No early exit in awk: ps would die of SIGPIPE ("ps: stdout: Broken pipe").
    fse=$(ps -Ao rss,comm | awk '/fseventsd$/ && !f {print int($1/1024); f = 1}')
    fse=${fse:-0}
    read -r vmobj mapent <<<"$(zprint 2>/dev/null | awk '$1=="vm.objects" {o=$7} $1=="VM.map.entries" {e=$7} END {print o+0, e+0}')"
    if [ "$pressure" -ge 4 ]; then crit=$((crit + 1)); else crit=0; fi
    if [ "$level" -lt "$MIN_FREE" ]; then low=$((low + 1)); else low=0; fi
    if [ "$crit" -eq 0 ] && [ "$low" -eq 0 ]; then first_stop=0; fi
    if [ "$pressure" -ge 2 ] || [ "$level" -lt $((MIN_FREE + 8)) ]; then
        if [ "$warned" -eq 0 ] && [ "$gcount" -gt 0 ]; then
            echo "$(date '+%F %T') WARN: memory pressure level ${pressure}, free ${level}%, guests ${grss} MB"
            warned=1
        fi
    else
        warned=0
    fi
    # The limits that stop everything at once come first, every second: after
    # the memory rule has stopped the largest guest, its grace period used to
    # hide them (a process-count or vm.objects runaway went on for
    # SAFEGUARD_GRACE_SECS while memory stayed short).
    if [ "$fse" -gt "$FSE_MB" ]; then kill_guests "fseventsd ${fse} MB > ${FSE_MB} MB"
    elif [ "$grss" -gt "$GUEST_MB" ]; then kill_guests "guests ${grss} MB > ${GUEST_MB} MB"
    elif [ "$gcount" -gt "$maxp" ]; then kill_guests "${gcount} lxrun processes > ${maxp}"
    elif [ "$vmobj" -gt "$MAX_VMOBJ" ]; then kill_guests "kernel vm.objects ${vmobj} > ${MAX_VMOBJ}"
    elif [ "$mapent" -gt "$MAX_MAPENT" ]; then kill_guests "kernel VM.map.entries ${mapent} > ${MAX_MAPENT}"
    elif [ "$gcount" -gt 0 ] && [ "$crit" -ge "$CRIT_SECS" ]; then
        relieve_memory "critical memory pressure (free ${level}%)"
    elif [ "$gcount" -gt 0 ] && [ "$low" -ge "$LOW_SECS" ]; then
        relieve_memory "free memory ${level}% < ${MIN_FREE}%"
    fi
    n=$((n + 1))
    # FEXServer is shared by every x86 program. If it is gone while x86 guests
    # run (it crashed, or an older one timed out), a game started from Steam
    # dies in 5 s with "Couldn't connect to FEXServer socket" (MEASURED):
    # bring it back the way run-fex.sh starts it. Only for x86 guests: an
    # aarch64 session otherwise got a FEXServer (and a FEX /bin/true) every 5 s.
    if [ $((n % 5)) -eq 0 ] && [ "$gcount" -gt 0 ] && ! pgrep -f 'lxrun .*FEXServer' >/dev/null \
       && has_x86_guest; then
        echo "$(date '+%F %T') FEXServer missing with ${gcount} guests: restarting it"
        (cd "$(dirname "$0")/.." && scripts/run-fex.sh /bin/true >/dev/null 2>&1 &)
    fi
    if [ $((n % 5)) -eq 0 ]; then
        echo "$(date '+%F %T') free=${level}% pressure=${pressure} guests=${gcount} rss=${grss}MB fseventsd=${fse}MB vmobj=${vmobj} mapent=${mapent}"
        flush_log
    fi
    sleep 1
done
