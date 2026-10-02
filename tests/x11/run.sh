#!/bin/bash
# The native XQuartz (SteamARM-X11.app, DISPLAY :2) as Chromium's X11 output
# surfaces see it, from an aarch64 guest under lxrun (tests/x11/xquartz_probe.c):
#  1. RandR's current mode has a real refresh rate. Stock XQuartz registers
#     its rootless mode at 1 Hz (hw/xquartz/quartzRandR.c FAKE_REFRESH_ROOTLESS)
#     and Chromium paced the Steam window at one frame per second;
#     patches/xquartz-randr-real-refresh.patch reports the display's rate.
#  2. MIT-SHM attaches a segment under kern.sysv.shmmax and ShmCompletion
#     events come back (Chromium's swap acknowledgement in software mode).
# Needs the server running (scripts/run-x11-native.sh). Opens no window.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
PASS=0 FAIL=0
ok()  { echo "  ok    $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL  $1"; [ -n "${2:-}" ] && echo "        $2"; FAIL=$((FAIL + 1)); }
echo "== tests/x11 (native XQuartz from a guest)"

STAGE="${STEAMARM_BUILD:-$HOME/SteamARM-build}/rootstage-f43"
ROOT="${LXRT_ROOT:-/tmp/lxrt-arm64root}"
GCCDIR=$(ls -d "$STAGE"/usr/lib/gcc/aarch64-redhat-linux/* 2>/dev/null | tail -1)
[ -S /tmp/.X11-unix/X2 ] || { echo "  skip  no X server on :2"; exit 0; }
[ -n "$GCCDIR" ] && [ -d "$ROOT/usr" ] || { echo "  skip  no sysroot ($STAGE) or guest root ($ROOT)"; exit 0; }

# The sysroot has the libraries but not their headers; Homebrew's X11 headers
# (installed for build-xquartz.sh) are architecture-neutral.
/opt/homebrew/opt/llvm/bin/clang --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" \
    --gcc-install-dir="$GCCDIR" -fuse-ld=lld --ld-path=/opt/homebrew/bin/ld.lld -O1 -w \
    -idirafter /opt/homebrew/include -o build/xquartz_probe tests/x11/xquartz_probe.c \
    "$STAGE"/usr/lib64/libX11.so.6 "$STAGE"/usr/lib64/libXext.so.6 "$STAGE"/usr/lib64/libXrandr.so.2 \
    -Wl,--allow-shlib-undefined 2>&1 || { bad "build xquartz_probe"; exit 1; }

out=$(DISPLAY=:2 LXRT_GUEST_PAGE=4096 LXRT_ROOT="$ROOT" perl -e 'alarm 60; exec @ARGV' \
      build/lxrun "$PWD/build/xquartz_probe" 2>&1 | grep -v '^\[lxrt\]')
echo "$out" | sed 's/^/        /'

hz=$(sed -n 's/.*-> refresh \([0-9.]*\) Hz.*/\1/p' <<<"$out" | head -1)
if [ -n "$hz" ] && awk -v h="$hz" 'BEGIN { exit !(h >= 20) }'; then
    ok "RandR current mode refresh $hz Hz"
else
    bad "RandR current mode refresh ${hz:-?} Hz" "Chromium paces its compositor at this rate; rebuild XQuartz with patches/xquartz-randr-real-refresh.patch"
fi
small=$(sed -n '/-- 700x440/,/-- 1280x800/p' <<<"$out")
grep -q "XShmAttach: OK" <<<"$small" && ok "MIT-SHM attach under shmmax" || bad "MIT-SHM attach under shmmax"
grep -q "ShmCompletion: 20/20" <<<"$small" && ok "ShmCompletion events" || bad "ShmCompletion events"

echo "== tests/x11: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
