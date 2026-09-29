#!/bin/bash
# GLX in the Steam Frame root (scripts/mkframeroot.sh), an executable probe:
# tests/arm64/glx_probe.c, built against the root and run in it under lxrun
# on the native X server (:2), with the environment the root names in
# .lxrt-guest-env (must get a GL context: the client's glXChooseVisual needs
# it) and, for comparison, with none of it and with the vendor name alone.
# Skips when the root or the X server is not there; starts no X server.
#
#   tests/arm64/frame_glx.sh [root]      (default $STEAMARM_STATE/arm64root)
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
STATE="${STEAMARM_STATE:-$HOME/SteamARM-roots}"
ROOT="${1:-${ARMROOT:-$STATE/arm64root}}"
echo "== tests/arm64/frame_glx"
if ! grep -qx 'ID=steamos' "$ROOT/etc/os-release" 2>/dev/null || [ ! -f "$ROOT/.lxrt-guest-env" ]; then
    echo "  skip  no Steam Frame root made by scripts/mkframeroot.sh at $ROOT"; exit 0
fi
[ -S /tmp/.X11-unix/X2 ] || { echo "  skip  no X server on :2 (scripts/run-x11-native.sh start :2)"; exit 0; }
[ -x build/lxrun ] || { echo "  FAIL  no build/lxrun (make lxrt)"; exit 1; }
ROOT=$(cd -P "$ROOT" && pwd)
mkdir -p "$ROOT/tmp/steamarm-tests"
if ! err=$(/opt/homebrew/opt/llvm/bin/clang --target=aarch64-unknown-linux-gnu --sysroot="$ROOT" \
        -fuse-ld=/opt/homebrew/opt/lld/bin/ld.lld -O1 -o "$ROOT/tmp/steamarm-tests/glx_probe" \
        tests/arm64/glx_probe.c -L"$ROOT/usr/lib" -lGL -lX11 2>&1); then
    echo "  FAIL  build against the root"; echo "$err" | sed 's/^/        /'; exit 1
fi
probe() {   # probe NAME=VALUE... : run the probe with exactly these GL variables
    env -i PATH=/usr/bin:/bin TMPDIR=/tmp HOME=/tmp DISPLAY=:2 LXRT_ROOT="$ROOT" \
        OBJC_DISABLE_INITIALIZE_FORK_SAFETY=YES "$@" \
        build/lxrun /tmp/steamarm-tests/glx_probe 2>&1 | grep -v '^\[lxrt\]'
}
GUEST_ENV=$(grep -E '^[A-Za-z_][A-Za-z0-9_]*=' "$ROOT/.lxrt-guest-env")
pass=0 fail=0
# shellcheck disable=SC2086
out=$(probe $GUEST_ENV); rc=$?
if [ $rc -eq 0 ] && grep -q '^glXChooseVisual: ok' <<<"$out" && grep -q '^GL_RENDERER: ' <<<"$out"; then
    echo "  ok    with the root's environment ($(echo $GUEST_ENV)): $(grep -E '^(context|GL_RENDERER|GL_VERSION):' <<<"$out" | tr '\n' ' ')"
    pass=$((pass + 1))
else
    echo "  FAIL  with the root's environment"; echo "$out" | sed 's/^/        /'; fail=$((fail + 1))
fi
# For comparison only (the image's Mesa decides these; stage 23 measured both as failures).
out=$(probe); echo "  note  no GL environment: $(grep -E '^(glXChooseVisual|visuals):' <<<"$out" | tr '\n' ' ')"
out=$(probe __GLX_VENDOR_LIBRARY_NAME=mesa)
echo "  note  vendor name alone: $(grep -E '^(glXChooseVisual|visuals):' <<<"$out" | tr '\n' ' ')"
rm -f "$ROOT/tmp/steamarm-tests/glx_probe"
echo "== frame_glx: $pass passed, $fail failed"
[ $fail -eq 0 ]
