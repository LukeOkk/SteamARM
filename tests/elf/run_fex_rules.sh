#!/bin/bash
# What an x86-64 Wine needs of the runtime's memory and signal rules, as
# small x86-64 programs under SteamARM's FEX (no Wine, no window):
#   fixed_maps          tests/elf/fixed_maps.c, 24 starts (each a new place
#                       for the host's heap; one start in eight failed)
#   xproc_signal_wait   tests/elf/xproc_signal_wait.c: a signal from another
#                       process to a thread in futex_waitv, futex and nanosleep
# and FEX itself starting 24 times ("Couldn't allocate page after SBRK" in
# 15 of 40 before the guest's break had a hole of its own).
#
# Usage: tests/elf/run_fex_rules.sh [steam-root]   (default /tmp/lxrt-steamroot)
# Needs the x86-64 sysroot of the thunk build (scripts/build-fex-thunks.sh).
set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT="${1:-/tmp/lxrt-steamroot}"
OUT="${STEAMARM_BUILD:-$HOME/SteamARM-build}/thunks-build"
CLANG=/opt/homebrew/opt/llvm/bin/clang
[ -d "$OUT/sysroot-x86_64" ] && [ -d "$ROOT/tmp" ] || { echo "  skip  no x86-64 sysroot ($OUT) or root ($ROOT)"; exit 0; }
pass=0 fail=0
ok()  { echo "  ok    $1"; pass=$((pass + 1)); }
bad() { echo "  FAIL  $1"; fail=$((fail + 1)); }
fex() { LXRT_ROOT="$ROOT" FEX_ROOTFS=/ perl -e 'alarm 60; exec @ARGV' scripts/run-fex.sh "$@" 2>&1; }
for t in fixed_maps xproc_signal_wait; do
    "$CLANG" --target=x86_64-linux-gnu --sysroot="$OUT/sysroot-x86_64" -fuse-ld=lld -O1 -o "$ROOT/tmp/fexrule_$t" "tests/elf/$t.c" ||
        { bad "$t (build)"; continue; }
done
n=0
for _ in $(seq 24); do fex /bin/echo fex-started | grep -q '^fex-started$' && n=$((n + 1)); done
[ "$n" = 24 ] && ok "FEX starts: 24 of 24" || bad "FEX starts: $n of 24"
n=0 last=
for _ in $(seq 24); do
    out=$(fex /tmp/fexrule_fixed_maps | grep -v '^\[lxrt\]\|GuestBase')
    grep -q '^== fixed_maps: ok' <<<"$out" && n=$((n + 1)) || last=$out
done
[ "$n" = 24 ] && ok "fixed_maps: 24 of 24 starts" || bad "fixed_maps: $n of 24 ($(tr '\n' ' ' <<<"$last"))"
for how in waitv futex sleep; do
    for mode in norestart restart; do
        out=$(fex /tmp/fexrule_xproc_signal_wait $mode $how | grep -v '^\[lxrt\]\|GuestBase')
        if grep -q '^== xproc_signal_wait .*: ok' <<<"$out"; then ok "xproc_signal_wait $how $mode"
        else bad "xproc_signal_wait $how $mode ($(tr '\n' ' ' <<<"$out"))"; fi
    done
done
echo "== $pass passed, $fail failed"
[ "$fail" -eq 0 ]
