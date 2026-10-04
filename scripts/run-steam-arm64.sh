#!/bin/bash
# Valve's native arm64 Steam client (steamrtarm64/steam) under the runtime:
# no FEX for the client, no VM. EXPERIMENTAL: where it stops is recorded in
# benchmarks/stage22-native-arm64-bringup.txt. The working Steam is still the
# x86 client under FEX (scripts/run-steam.sh).
#
#   scripts/run-steam-arm64.sh [--for SECONDS] [--jitless] [steam args...]
#
# Root: scripts/mkarmroot.sh ($STEAMARM_STATE/armroot, seen by the runtime as
# /tmp/lxrt-armroot; ARMROOT=<dir> ARMROOT_LINK=/tmp/<name> for another root,
# e.g. the Steam Frame root of scripts/mkframeroot.sh:
#   ARMROOT=$STEAMARM_STATE/arm64root ARMROOT_LINK=/tmp/lxrt-arm64root);
# the client lives in its tmp/armhome (HOME=/tmp/armhome in the guest).
# ARMROOT without ARMROOT_LINK: the link env-links.sh gives that root
# ($STATE/armroot, $STATE/arm64root); any other root needs its own link.
# Refused while a guest runs on the root or on the link (one client per home;
# the runtime follows the link on every lookup, so re-pointing it would move
# a running session).
# X: the native X server on :2 (scripts/run-x11-native.sh), started if needed.
# Log (runtime + client stdout/stderr):
#   $STATE/logs/native-arm64-steam-<date>.log     (STATE: ~/SteamARM-roots)
# The client's own logs are in <root>/tmp/armhome/.local/share/Steam/logs.
# --for SECONDS stops the session after that long. Stopping it (--for,
# Ctrl-C) signals only the processes this run started: by PID, and orphans
# by this run's STEAMARM_RUN_ID in their environment.
# --jitless: no longer needed (stage 23): V8's read-write-execute code pages
#   are split W^X page by page by the runtime (runtime/wxsplit.c) and the
#   login window comes with the JIT on. Kept as a control and a fallback: it
#   adds --js-flags=--jitless to Valve's steamwebhelper.sh for the session
#   (and -noverifyfiles, or the client restores the file at start), then
#   puts the original back. If this script is killed first, the client's own
#   file verification restores it on the next normal start (MEASURED).
# LXRT_WX_STATS=/host/file collects the renderers' W^X flip counters.
# LXRT_* variables are passed through. LXRT_X18_ALL_TEXT defaults to
# libcef.so: libcef has x18 uses outside its FDEs (stage 21, change 5).
# STEAMARM_LXRUN (tests): a stand-in for build/lxrun.
set -u
cd "$(dirname "$0")/.." || exit 1
. scripts/roots.sh
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
# The /tmp link the runtime sees the root through. Each root has its own, as
# scripts/env-links.sh makes them: /tmp/lxrt-armroot is the launcher's Fedora
# root (scripts/builtin-apps.json, steam-arm64), /tmp/lxrt-arm64root its
# Steam Frame root. Pointing one of those at another root would also move
# every later launch from the launcher, so such a root names its own link.
if [ -n "${ARMROOT_LINK:-}" ]; then
    ARMROOT="${ARMROOT:-$STATE/armroot}" LINK="$ARMROOT_LINK"
elif [ -z "${ARMROOT:-}" ]; then
    ARMROOT="$STATE/armroot" LINK=/tmp/lxrt-armroot
else
    a=$(canon_path "$ARMROOT") || { echo "no root at $ARMROOT" >&2; exit 1; }
    if [ "$a" = "$(canon_path "$STATE/armroot")" ]; then LINK=/tmp/lxrt-armroot
    elif [ "$a" = "$(canon_path "$STATE/arm64root")" ]; then LINK=/tmp/lxrt-arm64root
    else
        echo "ARMROOT=$ARMROOT is neither \$STEAMARM_STATE/armroot nor .../arm64root: set ARMROOT_LINK=/tmp/<a name of its own> as well" >&2
        exit 1
    fi
fi
FOR="" JITLESS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --for) FOR="${2:?--for needs seconds}"; shift 2 ;;
        --jitless) JITLESS=1; shift ;;
        *) break ;;
    esac
done
LXRUN="${STEAMARM_LXRUN:-$PWD/build/lxrun}"
[ -x "$LXRUN" ] || { echo "no build/lxrun (make lxrt)" >&2; exit 1; }
CLIENT="$ARMROOT/tmp/armhome/.local/share/Steam/steamrtarm64/steam"
[ -f "$CLIENT" ] || { echo "no arm64 client at $CLIENT" >&2; exit 1; }
# One client per home, and never a link moved under a running guest.
busy="$(guests_on_root "$ARMROOT") $(guests_on_root "$LINK")"
busy=$(echo $busy)
[ -z "$busy" ] || { echo "guests are running on $ARMROOT or $LINK (pid $busy): stop them first" >&2; exit 1; }
if [ "$(canon_path "$LINK")" != "$(canon_path "$ARMROOT")" ]; then
    [ ! -e "$LINK" ] || [ -L "$LINK" ] || { echo "$LINK exists and is not a link" >&2; exit 1; }
    [ ! -L "$LINK" ] || echo "re-pointing $LINK: $(readlink "$LINK") -> $ARMROOT"
    ln -sfn "$ARMROOT" "$LINK"
fi
WH="${CLIENT%/steam}/steamwebhelper.sh"
restore_wh() { [ -f "$WH.steamarm-orig" ] && mv -f "$WH.steamarm-orig" "$WH"; }
restore_wh                               # left over from a killed run
if [ "$JITLESS" = 1 ]; then
    cp -p "$WH" "$WH.steamarm-orig"
    sed 's|steamwebhelper "$@" &>|steamwebhelper "$@" --js-flags=--jitless \&>|' \
        "$WH.steamarm-orig" > "$WH"
    grep -q -- '--js-flags=--jitless' "$WH" ||
        { restore_wh; echo "--jitless: steamwebhelper.sh has an unexpected form" >&2; exit 1; }
    set -- -noverifyfiles "$@"
fi
# Never run a guest without the memory guard (two Mac hangs without it).
[ -n "${STEAMARM_NO_SAFEGUARD:-}" ] || scripts/safeguard.sh start >/dev/null
export DISPLAY="${DISPLAY:-:2}"
[ "$DISPLAY" != :2 ] || scripts/run-x11-native.sh start :2 >/dev/null ||
    { restore_wh; echo "native X server failed to start" >&2; exit 1; }
mkdir -p "$STATE/logs"
LOG="$STATE/logs/native-arm64-steam-$(date +%Y%m%d-%H%M%S).log"
# As scripts/run-native.sh: macOS's per-user TMPDIR and LD_LIBRARY_PATH must
# not leak into the guest; libobjc reads the fork-safety switch at start-up.
# The Mac's PATH means nothing inside the root (the client searched it for
# lsof and bash).
export TMPDIR=/tmp PATH=/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin
unset LD_LIBRARY_PATH FEX_ROOTFS FEX_GUESTBASE
export OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES
export LXRT_ROOT="$LINK" LXRT_GUEST_PAGE=4096 HOME=/tmp/armhome
export LXRT_X18_ALL_TEXT="${LXRT_X18_ALL_TEXT-libcef.so}"
# No libusb for SDL's hidapi: it failed and was reloaded at every controller
# poll (scripts/settings-env.py says why).
export SDL_HIDAPI_LIBUSB="${SDL_HIDAPI_LIBUSB-0}"
# The environment the root names for its guests (the Steam Frame root's
# indirect GLX, scripts/mkframeroot.sh); a variable the caller set wins.
. scripts/guest-env.sh
guest_env_from_root "$ARMROOT"
echo "log: $LOG"
# This run's mark: the runtime hands a guest's environment on through every
# exec (runtime/process.c, lxrt_execve) and a fork keeps it, so each process
# of the session carries it, and the kernel shows the environment each one
# was exec'd with (procs_env). How an orphan is told from another session's.
export STEAMARM_RUN_ID="steam-arm64.$$.$(date +%s)"
"$LXRUN" /tmp/armhome/.local/share/Steam/steamrtarm64/steam "$@" >"$LOG" 2>&1 &
# The session's processes: every lxrun of this checkout descended from the
# client, remembered once seen. The client and the webhelper start their own
# process groups, and children are reparented when a parent exits, so neither
# the group nor the parent chain alone finds them all.
PIDS=$!
track() {
    PIDS=$(/bin/ps -axo pid=,ppid=,command= | /usr/bin/awk -v known=" $PIDS " -v lx="$LXRUN " '
        index($0, lx) { lxp[$1] = 1; par[$1] = $2 }
        END {
            n = split(known, k, " ")
            for (i = 1; i <= n; i++) if (k[i] in lxp) ours[k[i]] = 1
            do { ch = 0; for (p in lxp) if (!(p in ours) && (par[p] in ours)) { ours[p] = 1; ch = 1 } } while (ch)
            for (p in ours) printf "%s ", p
        }')
}
# A client that exits in its first seconds can leave webhelper zygotes that
# were never seen as its descendants: reparented to launchd between two polls
# (MEASURED, benchmarks/stage23-frame-root.txt C2). Candidates are this
# checkout's lxrun, parent 1, running a steamrtarm64 program, started after
# this session; that also fits another session's (a second run of this script
# on the other root, a launcher session), so only those with this run's
# STEAMARM_RUN_ID are ours.
orphan_candidates() {
    /bin/ps -axo pid=,ppid=,etime=,command= | /usr/bin/awk -v lx="$LXRUN " -v max=$((SECONDS + 2)) '
        function secs(t,  a, n, d) {
            d = 0; if (index(t, "-")) { split(t, a, "-"); d = a[1]; t = a[2] }
            n = split(t, a, ":")
            return d * 86400 + (n == 3 ? a[1] * 3600 + a[2] * 60 + a[3] : a[1] * 60 + a[2])
        }
        $2 == 1 && index($0, lx) && index($0, "/steamrtarm64/") && secs($3) <= max { printf "%s ", $1 }'
}
orphans() {
    local c; c=$(orphan_candidates)
    [ -n "$c" ] || return 0
    procs_env STEAMARM_RUN_ID $c | /usr/bin/awk -v id="$STEAMARM_RUN_ID" '$2 == id { printf "%s ", $1 }'
}
reap_orphans() {
    local o c others p
    c=$(orphan_candidates)
    [ -n "$c" ] || return 0
    o=$(orphans)
    others=""
    for p in $c; do case " $o " in *" $p "*) ;; *) others="$others $p" ;; esac; done
    [ -z "$others" ] || echo "left running, without this run's STEAMARM_RUN_ID:$others"
    [ -n "$o" ] || return 0
    echo "stopping orphaned webhelper processes: $o"
    kill -TERM $o 2>/dev/null; sleep 2
    o=$(orphans); [ -z "$o" ] || kill -KILL $o 2>/dev/null
}
stop() {
    track
    [ -z "$PIDS" ] || kill -TERM $PIDS 2>/dev/null
    for _ in 1 2 3 4 5; do track; [ -n "$PIDS" ] || break; sleep 1; done
    [ -z "$PIDS" ] || kill -KILL $PIDS 2>/dev/null
    reap_orphans
}
trap 'stop; restore_wh; exit 130' INT TERM
SECONDS=0
while track; [ -n "$PIDS" ]; do
    if [ -n "$FOR" ] && [ "$SECONDS" -ge "$FOR" ]; then stop; break; fi
    sleep 1
done
reap_orphans
restore_wh
echo "ended after ${SECONDS} s; log: $LOG"
