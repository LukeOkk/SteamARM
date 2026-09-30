#!/bin/bash
# Run an x86-64 Android 11 program -- Waydroid's LineageOS 18.1 x86_64 build,
# scripts/mkandroidroot.sh --arch x86_64 -- through SteamARM's FEX under
# lxrun. No VM (AGENTS.md). docs/ANDROID_RUNTIME_ARCHITECTURE.md, "x86_64
# Android under FEX"; benchmarks/stage25-art-x86-fex.txt.
#
#   scripts/run-android-x86.sh <guest program> [args...]
#   scripts/run-android-x86.sh --server-stop    stop this root's FEXServer
#   scripts/run-android-x86.sh --server-pid     print its PID, if it runs
#
# The guest sees the Android root as "/" (LXRT_ROOT) with Android's own
# environment (init.environ.rc) and nothing of the Mac's. The runtime hands
# every x86 ELF to /usr/lib/lxrt-emu/FEX (runtime/main.c, "binfmt_misc"),
# which the root carries; FEX_ROOTFS=/ because the root IS the x86 tree.
#
# FEXServer: FEX takes its rootfs from the server it connects to, and the
# shared one (scripts/run-fex.sh) serves the Ubuntu x86 rootfs. This root has
# a server of its own, on its own socket name (FEX_SERVERSOCKETPATH) and with
# its own lock (HOME=/data/local/tmp), so neither can see or replace the
# other. It is started here if it is not running, detached (scripts/session.py).
# It does NOT exit by itself when idle: FEXServer ignores --persistent's
# timeout when it runs with --foreground (FEX's ProcessPipe.cpp waits with no
# timeout when Foreground is set; MEASURED, three servers idle for 7-10 min,
# benchmarks/stage28-android-reliability.txt), and --foreground is what keeps
# it from daemonizing away from the PID file. Stop it with --server-stop; each
# one left counts toward scripts/safeguard.sh's 80 lxrun processes.
#
# Environment:
#   ANDROID_X86_ROOT   the root         (/Volumes/SteamARMAndroid/root-x86_64)
#   LXRUN              the runtime      (build/lxrun)
#   ANDROID_X86_GENV   extra NAME=VALUE words for the guest environment
#   FEX_LOWWINDOW      1 (default): the 64-bit low window for every process,
#                      so MAP_32BIT and fixed mappings below 4 GiB -- ART's
#                      heap and boot image -- have somewhere to go
#                      (patches/fex-lxrt-guest-base.patch); 0: PIE-only rules
#   FEX_SMCCHECKS      mtrack (default, FEX's page tracking), full, none
#   ANDROID_X86_SERVER_IDLE  FEXServer's --persistent (60; no effect with --foreground, above)
set -u
cd "$(dirname "$0")/.." || exit 1
. scripts/roots.sh
ROOT="${ANDROID_X86_ROOT:-/Volumes/SteamARMAndroid/root-x86_64}"
LXRUN="${LXRUN:-build/lxrun}"
# One server per root: the socket name carries a hash of the root's path.
SOCK_NAME="steamarm-android-$(printf '%s' "$ROOT" | shasum | cut -c1-12).FEXServer.Socket"
uid=$(id -u)
socket="/tmp/lxrt-abstract-$uid/$SOCK_NAME"

pidfile="$ROOT/data/local/tmp/.fexserver.pid"
server_pids() {   # this root's server, if it still runs: the PID file's
    local p r     # process must be an lxrt-emu FEXServer AND run on this root
    p=$(cat "$pidfile" 2>/dev/null) || return 0
    case "$p" in ''|*[!0-9]*) return 0 ;; esac
    ps -p "$p" -o command= 2>/dev/null | grep -q 'lxrt-emu/FEXServer' || return 0
    r=$(procs_env LXRT_ROOT "$p")
    [ "${r#* }" = "$ROOT" ] && echo "$p"
    return 0
}

case "${1:-}" in
    --server-stop) pids=$(server_pids); [ -z "$pids" ] || kill "$pids"; exit 0 ;;
    --server-pid) server_pids; exit 0 ;;
esac
[ $# -ge 1 ] || { echo "usage: $0 <guest program> [args...] | --server-stop" >&2; exit 2; }
[ -x "$ROOT/usr/lib/lxrt-emu/FEX" ] || {
    echo "run-android-x86: no x86_64 Android root with FEX at $ROOT (scripts/mkandroidroot.sh --arch x86_64)" >&2; exit 1; }
[ -x "$LXRUN" ] || { echo "run-android-x86: no $LXRUN (make lxrt)" >&2; exit 1; }
[ -n "${STEAMARM_NO_SAFEGUARD:-}" ] || scripts/safeguard.sh start >/dev/null

BCP=$(sed -n 's/^ *export BOOTCLASSPATH //p' "$ROOT/init.environ.rc")
DBCP=$(sed -n 's/^ *export DEX2OATBOOTCLASSPATH //p' "$ROOT/init.environ.rc")
# What the runtime and FEX need (TMPDIR is the HOST directory where the
# runtime materialises Linux abstract sockets; FEX looks for XDG_RUNTIME_DIR
# first, a guest path), then Android's own variables.
base_env=(
    LXRT_ROOT="$ROOT" TMPDIR=/tmp XDG_RUNTIME_DIR=/data/local/tmp
    OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES
    FEX_ROOTFS=/ FEX_SERVERSOCKETPATH="$SOCK_NAME" FEX_GUESTBASE=1
    FEX_LOWWINDOW="${FEX_LOWWINDOW:-1}" FEX_SMCCHECKS="${FEX_SMCCHECKS:-mtrack}"
    FEX_SILENTLOG="${FEX_SILENTLOG:-1}" FEX_OUTPUTLOG=stderr
    HOME=/data/local/tmp
    # Process and thread ids below 65536 for every process of the stack
    # (runtime/ids.c): 32-bit bionic keeps them in 16 bits and aborted with
    # the Mac's pids past 65535 (LXRT_SMALL_IDS=0 turns it off).
    LXRT_SMALL_IDS="${LXRT_SMALL_IDS:-1}"
)
android_env=(
    PATH=/system/bin:/system/xbin TERM=dumb
    ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_ART_ROOT=/apex/com.android.art
    ANDROID_I18N_ROOT=/apex/com.android.i18n ANDROID_TZDATA_ROOT=/apex/com.android.tzdata
    ANDROID_STORAGE=/storage BOOTCLASSPATH="$BCP" DEX2OATBOOTCLASSPATH="$DBCP"
)

if [ -z "$(server_pids)" ]; then
    old_inode=$(stat -f %i "$socket" 2>/dev/null || true)
    # In a subshell, so the server is no child of the guest this script
    # becomes (scripts/run-fex.sh says why); detach gives it its own session.
    ( env -i "${base_env[@]}" FEX_SILENTLOG=0 PYTHONCOERCECLOCALE=0 STEAMARM_SESSION_PY=1 \
        /usr/bin/python3 scripts/session.py detach \
        "$LXRUN" /usr/lib/lxrt-emu/ld-linux-aarch64.so.1 --library-path /usr/lib/lxrt-emu \
        /usr/lib/lxrt-emu/FEXServer --foreground --persistent="${ANDROID_X86_SERVER_IDLE:-60}" \
        >"$ROOT/data/local/tmp/fexserver.log" 2>&1 &
      echo $! > "$pidfile" )
    ready=0
    for ((i=0; i<100; i++)); do
        inode=$(stat -f %i "$socket" 2>/dev/null || true)
        if [ -n "$inode" ] && [ "$inode" != "$old_inode" ] && [ -n "$(server_pids)" ]; then
            ready=1; break
        fi
        sleep 0.1
    done
    [ "$ready" = 1 ] || { echo "run-android-x86: FEXServer did not start ($ROOT/data/local/tmp/fexserver.log)" >&2; exit 1; }
fi

# Android's linker namespaces, which init has linkerconfig write at boot:
# here the first time. This image's linkerconfig is a static x86-64 ET_EXEC,
# fine for FEX. With no property service it writes /linkerconfig/ld.config.txt
# (the "legacy" layout: ro.vndk.version reads empty) and then aborts on the
# first APEX that needs VENDOR_VNDK_VERSION (MEASURED); the main file is what
# ART needs (libandroid_runtime.so's libstatssocket.so is in the statsd APEX).
if [ ! -s "$ROOT/linkerconfig/ld.config.txt" ] && [ "$1" != /system/bin/linkerconfig ]; then
    # In a subshell: the shell's own "Abort trap" line goes to /dev/null too.
    ( env -i "${base_env[@]}" "${android_env[@]}" "$LXRUN" /system/bin/linkerconfig --target /linkerconfig ) \
        >/dev/null 2>&1 || true
    [ -s "$ROOT/linkerconfig/ld.config.txt" ] ||
        echo "run-android-x86: linkerconfig wrote no ld.config.txt; the linker keeps its defaults" >&2
fi

exec env -i "${base_env[@]}" "${android_env[@]}" ${ANDROID_X86_GENV:-} "$LXRUN" "$@"
