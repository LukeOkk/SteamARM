#!/bin/bash
# The cost of FEX's x86 memory-ordering (TSO) emulation, measured: builds
# benchmarks/tso_bench.c as an x86-64 Linux program against the thunk build's
# x86-64 sysroot (scripts/build-fex-thunks.sh), puts it in the x86 root's /tmp
# and runs it through FEX under the runtime (scripts/run-fex.sh) RUNS times
# under each profile of Procesador -> Orden de memoria, with exactly the
# variables scripts/settings-env.py exports for that profile. No Proton, no
# window, nothing installed. Record: benchmarks/stage58-tso-cost.txt.
#
# Usage: benchmarks/tso_bench.sh [steam-root]     (default /tmp/lxrt-steamroot)
#   RUNS=3           runs per profile
#   SCALE=1          tso_bench.c's argument (0.25 for a quick look)
#   PROFILES="off fast full"
set -u
cd "$(dirname "$0")/.." || exit 1
ROOT="${1:-/tmp/lxrt-steamroot}"
OUT="${STEAMARM_BUILD:-$HOME/SteamARM-build}/thunks-build"
CLANG=/opt/homebrew/opt/llvm/bin/clang
RUNS="${RUNS:-3}" SCALE="${SCALE:-1}" PROFILES="${PROFILES:-off fast full}"
[ -d "$OUT/sysroot-x86_64" ] && [ -d "$ROOT/tmp" ] ||
    { echo "tso_bench: no x86-64 sysroot ($OUT/sysroot-x86_64) or root ($ROOT)" >&2; exit 2; }
BIN="tso_bench.$$"
"$CLANG" --target=x86_64-linux-gnu --sysroot="$OUT/sysroot-x86_64" -fuse-ld=lld -O2 -Wall -Wextra -pthread \
    -o "$ROOT/tmp/$BIN" benchmarks/tso_bench.c || exit 1
trap 'rm -f "$ROOT/tmp/$BIN"' EXIT
FEXDIR="${FEXDIR:-/tmp/lxrt-root/usr/bin}"
echo "tso_bench: $(sysctl -n machdep.cpu.brand_string), macOS $(sw_vers -productVersion)," \
     "FEX $(strings "$FEXDIR/${FEXBIN:-FEX-gb}" | grep -m1 -E '^FEX-[0-9]{4}-'), scale $SCALE, $RUNS runs per profile"
for profile in $PROFILES; do
    vars=$(scripts/settings-env.py --tso-profile "$profile") || exit 1
    echo "== profile $profile: $(printf '%s' "${vars:-FEX defaults, nothing set}" | tr '\n' ' ')"
    for run in $(seq "$RUNS"); do
        echo "-- run $run"
        # Every run is a new FEX process: the profile decides how it translates
        # loads and stores (AffectsCodeGen), nothing is cached on disk.
        ( eval "$vars"
          LXRT_ROOT="$ROOT" FEX_ROOTFS=/ perl -e 'alarm 300; exec @ARGV' scripts/run-fex.sh "/tmp/$BIN" "$SCALE" 2>&1 |
              grep -v '^\[lxrt\]' )
    done
done
