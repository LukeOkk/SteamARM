#!/bin/bash
# Run an x86 / i386 Linux program through FEX under the runtime, no VM.
#
#   scripts/run-fex.sh [--trace] <program> [args...]
#
# Two guest roots (both persistent, linked from /tmp after a reboot by
# scripts/env-links.sh):
#   /tmp/lxrt-root       aarch64 tree (scripts/mkroot-rpm.sh); x86 programs
#                        see FEX's rootfs overlay (FEX_ROOTFS below)
#   /tmp/lxrt-steamroot  x86-64 tree as "/" plus the aarch64 emulator side
#                        (scripts/mksteamroot.sh); FEX_ROOTFS=/
# Guest paths are what the program sees: /tmp/fexhome is $HOME inside.
#
# Overrides: LXRT_ROOT, FEX_ROOTFS, FEXBIN (default FEX-gb), DISPLAY, and
# FEXDIR, the host directory FEX and FEXServer are loaded from (default
# /tmp/lxrt-root/usr/bin; e.g. <root>/usr/bin of a scripts/mkroot-rpm.sh root).
set -u
cd "$(dirname "$0")/.." || exit 1
# Never run a guest without the memory guard (two Mac hangs without it).
[ -n "${STEAMARM_NO_SAFEGUARD:-}" ] || scripts/safeguard.sh start >/dev/null
TRACE=""
if [ "${1:-}" = "--trace" ]; then TRACE="--trace"; shift; fi
[ $# -ge 1 ] || { echo "usage: $0 [--trace] <program> [args...]" >&2; exit 2; }
export LXRT_ROOT="${LXRT_ROOT:-/tmp/lxrt-root}"
# The guest must not inherit macOS's per-user TMPDIR (a host path under /var/folders).
export TMPDIR=/tmp
# LD_LIBRARY_PATH is NOT exported: it leaks into the guest, whose i386 ld.so then
# picked /lib64/libGL.so.1 (aarch64) for Steam's steamui.so ("wrong ELF class").
unset LD_LIBRARY_PATH
FEXSERVER_LOG="${STEAMARM_STATE:-$HOME/SteamARM-roots}/logs/fexserver.log"
export HOME=/tmp/fexhome
# libobjc reads this at start-up; see runtime/process.c (lxrt_execve).
export OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES
export FEX_ROOTFS="${FEX_ROOTFS-/tmp/fexhome/.local/share/fex-emu/RootFS/Ubuntu_24_04}"
export FEX_SILENTLOG=0 FEX_OUTPUTLOG=stderr
export DISPLAY="${DISPLAY:-:2}"   # the native X server (scripts/run-x11-native.sh)
# SMC detection by page tracking (FEX's default for Linux guests).
export FEX_SMCCHECKS="${FEX_SMCCHECKS:-mtrack}"
# 32-bit guests live at host = base + guest (benchmarks/stage7-guest-base.txt);
# ET_EXEC x86-64 images get a low window.
export FEX_GUESTBASE=1
FEXBIN="${FEXBIN:-FEX-gb}"
FEXDIR="${FEXDIR:-/tmp/lxrt-root/usr/bin}"
# FEXServer owns the rootfs and shared config; FEX connects to it over a
# unix socket the runtime materialises for the abstract name.
# One FEXServer serves every FEX client of this user, and it hands each of them
# ITS rootfs path -- whatever the caller that happened to start it had. Started
# from a Steam run (FEX_ROOTFS=/), it told pressure-vessel the interpreter root
# was "/": pv then set up no /run/pressure-vessel/interpreter-root, FEX looked
# up /proc inside the container root first, and every steamwebhelper zygote
# died (FATAL thread_helpers.cc: fstatat(/proc, "self/task/") ENOENT). So the
# server always gets the same environment, whoever starts it.
# TMPDIR too: FEXServer binds its socket under $TMPDIR, and a macOS one
# (/var/folders/...) does not exist in the guest root -- bind failed with
# ENOENT, the server exited, and every FEX client that could not reach it
# started more processes until the safeguard killed 99 of them.
# --persistent: FEXServer otherwise exits once it sees no clients, and a FEX
# started later cannot bring it back here (it looks for FEXServer on its own
# PATH): Steam's "Play" then died in 5 s with "Couldn't connect to FEXServer
# socket" (MEASURED, Schedule I).
if ! pgrep -f 'lxrun .*FEXServer' >/dev/null 2>&1; then
    # In a subshell: the server must not be a child of this shell, which is
    # about to exec the guest -- a program that reaps all its children (pressure-
    # vessel's pv-adverb) then waited for FEXServer forever (MEASURED).
    # In its own session too (session.py detach): the server serves every x86
    # program of this user, so it must not die with one app's process group.
    ( TMPDIR=/tmp LXRT_ROOT=/tmp/lxrt-root FEX_ROOTFS=/tmp/fexhome/.local/share/fex-emu/RootFS/Ubuntu_24_04 \
        nohup env PYTHONCOERCECLOCALE=0 STEAMARM_SESSION_PY=1 /usr/bin/python3 scripts/session.py detach \
        ./build/lxrun "$FEXDIR"/FEXServer --foreground --persistent=0 >"$FEXSERVER_LOG" 2>&1 & )
    sleep 4
    if ! pgrep -f 'lxrun .*FEXServer' >/dev/null 2>&1; then
        echo "run-fex: FEXServer did not start (see $FEXSERVER_LOG); not starting the guest" >&2
        exit 1
    fi
fi
exec ./build/lxrun $TRACE "$FEXDIR"/"$FEXBIN" "$@"
