#!/bin/bash
# Pointer confinement on SteamARM's X server (patches/xquartz-pointer-confine.patch),
# the way SDL 3 grabs for mouse-look: tests/x11/xgrab_confine.c grabs with
# confine_to = its window, and
#   1. with an empty cursor: the macOS cursor must stay where it is while
#      mouse motion (deltas at a fixed location, what a dissociated mouse
#      reports) reaches X clients as XInput 2 raw motion on relative X / Y
#      axes, both the way Wine reads it (axes 0 and 1: patches/
#      xquartz-relative-pointer.patch) and the way SDL 3 does (Rel X / Rel Y);
#   2. with a visible cursor: the cursor must not leave the window.
# MOVES THE MOUSE POINTER: run it while nobody uses the Mac. Needs the X server
# on :2 (scripts/run-x11-native.sh) and the Homebrew X libraries.
set -u
cd "$(dirname "$0")/../.." || exit 1
B="${TMPDIR:-/tmp}/steamarm-confine"; mkdir -p "$B"
CFLAGS="-O1 -I/opt/homebrew/include -L/opt/homebrew/lib"
clang $CFLAGS -o "$B/xgrab_confine" tests/x11/xgrab_confine.c -lX11 || exit 1
clang $CFLAGS -o "$B/xiwine" tests/x11/xiwine.c -lX11 -lXi || exit 1
clang -O1 -o "$B/mousedelta" tests/x11/mousedelta.c -framework ApplicationServices || exit 1
idle=$(ioreg -c IOHIDSystem | awk '/HIDIdleTime/ {print int($NF/1000000000); exit}')
if [ "${FORCE:-0}" != 1 ] && [ "${idle:-0}" -lt 30 ]; then
    echo "  skip  someone used the mouse ${idle}s ago (FORCE=1 runs anyway)"; exit 0
fi
pass=0 fail=0
grab() { # seconds [visible]
    : > "$B/grab.out"
    ( DISPLAY=:2 "$B/xgrab_confine" "$@" > "$B/grab.out" 2>&1 & )
    for _ in $(seq 1 50); do grep -q grabbed "$B/grab.out" && break; sleep 0.1; done
    sleep 0.5
}
grab 4
( "$B/xiwine" 2 > "$B/raw.out" & ); sleep 0.2
moved=$("$B/mousedelta" 100 6 3 fixed); sleep 2
raw=$(cat "$B/raw.out")
if echo "$moved" | awk '{split($2,a,","); split($4,b,","); exit !(a[1]==b[1] && a[2]==b[2])}' &&
   echo "$raw" | grep -q "wine relative X sum 600.0 Y sum 300.0; sdl relative X sum 600.0 Y sum 300.0"; then
    echo "  ok    hidden cursor held, deltas delivered ($moved; $raw)"; pass=$((pass + 1))
else
    echo "  FAIL  hidden cursor: $moved; $raw"; fail=$((fail + 1))
fi
sleep 2.5
grab 4 visible
right=$(DISPLAY=:2 xwininfo -name xgrab_confine 2>/dev/null | awk '/Absolute upper-left X/ {x=$NF} /Width/ {w=$NF} END {print x+w}')
moved=$("$B/mousedelta" 120 10 0); sleep 3
x=$(echo "$moved" | awk '{split($4,b,","); print b[1]}')
if [ -n "$right" ] && [ "${x%.*}" -lt "$right" ]; then
    echo "  ok    visible cursor kept in the window ($moved, window right edge $right)"; pass=$((pass + 1))
else
    echo "  FAIL  visible cursor left the window ($moved, window right edge ${right:-?})"; fail=$((fail + 1))
fi
sleep 1.5
echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
