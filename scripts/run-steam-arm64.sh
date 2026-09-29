#!/bin/bash
# Valve's native arm64 Steam client (steamrtarm64/steam) under the runtime:
# no FEX for the client, no VM. EXPERIMENTAL: where it stops is recorded in
# benchmarks/stage22-native-arm64-bringup.txt. The working Steam is still the
# x86 client under FEX (scripts/run-steam.sh).
#
#   scripts/run-steam-arm64.sh [--for SECONDS] [--jitless] [steam args...]
#
# Root: scripts/mkarmroot.sh ($STEAMARM_STATE/armroot, seen by the runtime as
# /tmp/lxrt-armroot; ARMROOT=<dir> ARMROOT_LINK=/tmp/<name> for another root);
# the client lives in its tmp/armhome (HOME=/tmp/armhome in the guest).
# X: the native X server on :2 (scripts/run-x11-native.sh), started if needed.
# Log (runtime + client stdout/stderr):
#   $STATE/logs/native-arm64-steam-<date>.log     (STATE: ~/SteamARM-roots)
# The client's own logs are in <root>/tmp/armhome/.local/share/Steam/logs.
# --for SECONDS stops the session after that long. Stopping it (--for,
# Ctrl-C) signals only the processes this run started, by PID.
# --jitless: a WORKAROUND, not a fix. V8 in the webhelper's renderers asks for
#   read-write-execute code pages; the runtime grants them read-write only
#   (runtime/dispatch.c, do_mprotect_inner), so every renderer dies on its
#   first JIT call and the login window never comes. This adds
#   --js-flags=--jitless to Valve's steamwebhelper.sh for the session (and
#   -noverifyfiles, or the client restores the file at start), then puts the
#   original back. If this script is killed first, the client's own file
#   verification restores it on the next normal start (MEASURED).
# LXRT_* variables are passed through. LXRT_X18_ALL_TEXT defaults to
# libcef.so: libcef has x18 uses outside its FDEs (stage 21, change 5).
set -u
cd "$(dirname "$0")/.." || exit 1
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
ARMROOT="${ARMROOT:-$STATE/armroot}"
LINK="${ARMROOT_LINK:-/tmp/lxrt-armroot}"
FOR="" JITLESS=0
while [ $# -gt 0 ]; do
    case "$1" in
        --for) FOR="${2:?--for needs seconds}"; shift 2 ;;
        --jitless) JITLESS=1; shift ;;
        *) break ;;
    esac
done
[ -x build/lxrun ] || { echo "no build/lxrun (make lxrt)" >&2; exit 1; }
CLIENT="$ARMROOT/tmp/armhome/.local/share/Steam/steamrtarm64/steam"
[ -f "$CLIENT" ] || { echo "no arm64 client at $CLIENT" >&2; exit 1; }
if [ "$(readlink "$LINK" 2>/dev/null)" != "$ARMROOT" ]; then
    [ ! -e "$LINK" ] || [ -L "$LINK" ] || { echo "$LINK exists and is not a link" >&2; exit 1; }
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
echo "log: $LOG"
LXRUN="$PWD/build/lxrun"
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
stop() {
    track
    [ -z "$PIDS" ] || kill -TERM $PIDS 2>/dev/null
    for _ in 1 2 3 4 5; do track; [ -n "$PIDS" ] || return 0; sleep 1; done
    kill -KILL $PIDS 2>/dev/null
}
trap 'stop; restore_wh; exit 130' INT TERM
SECONDS=0
while track; [ -n "$PIDS" ]; do
    if [ -n "$FOR" ] && [ "$SECONDS" -ge "$FOR" ]; then stop; break; fi
    sleep 1
done
restore_wh
echo "ended after ${SECONDS} s; log: $LOG"
