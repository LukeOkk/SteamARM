#!/bin/bash
# Run an aarch64 Linux program directly under the runtime: no FEX, no VM.
# The ARM64-first counterpart of scripts/run-fex.sh; scripts/run-app.sh picks
# it for launcher entries whose "architecture" is aarch64.
#
#   scripts/run-native.sh [--trace] <program> [args...]
#
# LXRT_ROOT is the ARM64 base root the program lives in (default
# /tmp/lxrt-arm64root: the Steam Frame / Holo userspace, docs/STEAM_FRAME_IMAGE.md).
# <program> is a guest path inside it. HOME defaults to /tmp/fexhome, where the
# existing Steam configuration lives; an app sets HOME_IN_GUEST to change it.
set -u
cd "$(dirname "$0")/.." || exit 1
# Never run a guest without the memory guard (two Mac hangs without it).
[ -n "${STEAMARM_NO_SAFEGUARD:-}" ] || scripts/safeguard.sh start >/dev/null
TRACE=""
if [ "${1:-}" = "--trace" ]; then TRACE="--trace"; shift; fi
[ $# -ge 1 ] || { echo "usage: $0 [--trace] <program> [args...]" >&2; exit 2; }
export LXRT_ROOT="${LXRT_ROOT:-/tmp/lxrt-arm64root}"
[ -d "$LXRT_ROOT" ] || { echo "run-native: no ARM64 root at $LXRT_ROOT" >&2; exit 1; }
# As run-fex.sh: macOS's per-user TMPDIR and LD_LIBRARY_PATH must not leak in.
export TMPDIR=/tmp
unset LD_LIBRARY_PATH
export HOME="${HOME_IN_GUEST:-/tmp/fexhome}"
# libobjc reads this at start-up; see runtime/process.c (lxrt_execve).
export OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES
export DISPLAY="${DISPLAY:-:2}"   # the native X server (scripts/run-x11-native.sh)
# No FEX_* here: translator settings belong to x86 payloads only.
unset FEX_ROOTFS FEX_GUESTBASE
# The Mac's PATH means nothing inside the root: the native Steam client looks
# for bash along it (MEASURED: "lxrun: open /opt/homebrew/.../bash" from a
# launcher start), as scripts/run-steam-arm64.sh already knew. PATH_IN_GUEST
# overrides.
export PATH="${PATH_IN_GUEST:-/usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin}"
exec ./build/lxrun $TRACE "$@"
