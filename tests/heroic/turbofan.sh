#!/bin/bash
# V8's TurboFan under lxrun without Chromium: Heroic's Electron binary run as
# Node (ELECTRON_RUN_AS_NODE=1), the reproducer of
# benchmarks/stage24-heroic.txt section 6 (T2 and T3).
#
#   tests/heroic/turbofan.sh [RUNS] [V8/Node flag...]      (RUNS default 3)
#
#   small   one function, 2,000,000 calls (T2): the sum must be 4e12
#   heavy   60 functions made with new Function, 3000 calls each (T3)
#
# Each run prints its exit status and, for a crash, lxrun's fault line.
# LXRT_* variables are passed through (LXRT_NO_X18=1, LXRT_TRACE=1, ...).
# Needs build/lxrun and Heroic in the armroot (scripts/install-heroic-arm64.sh).
# Starts one guest at a time, each under a 60 s alarm; stops nothing else.
set -u
cd "$(dirname "$0")/../.." || exit 1
RUNS="${1:-3}"; [ $# -gt 0 ] && shift
ROOT="${LXRT_ROOT:-/tmp/lxrt-armroot}"
PROG=/opt/apps/heroic/Heroic-2.22.3-linux-arm64/heroic
[ -x "$ROOT$PROG" ] || { echo "SKIP: no Heroic at $ROOT$PROG (scripts/install-heroic-arm64.sh)"; exit 0; }
SMALL='function f(x){return x*2+1} let s=0; for(let i=0;i<2e6;i++) s+=f(i); console.log(s===4e12?"TURBOFAN small ok":"TURBOFAN small WRONG "+s)'
HEAVY='let fs=[]; for(let k=0;k<60;k++) fs.push(new Function("a","b","let r=0; for(let j=0;j<32;j++){ r=(r*31+a*"+(k+3)+"+j)^(b>>>"+(k%7)+"); if(r>1e9) r-=1e9; } return r;")); let t=0; for(let k=0;k<60;k++) for(let i=0;i<3000;i++) t=(t+fs[k](i,k*i))%1000000007; console.log("TURBOFAN heavy ok "+t)'
tot_ok=0 tot=0
for name in small heavy; do
    [ $name = small ] && js="$SMALL" || js="$HEAVY"
    ok=0
    for i in $(seq 1 "$RUNS"); do
        out=$(LXRT_ROOT="$ROOT" LXRT_X18_ALL_TEXT="${LXRT_X18_ALL_TEXT:-/opt/apps/heroic/}" \
              HOME_IN_GUEST=/tmp/heroichome ELECTRON_RUN_AS_NODE=1 \
              /usr/bin/perl -e 'alarm shift; exec @ARGV' 60 scripts/run-native.sh "$PROG" "$@" -e "$js" 2>&1)
        rc=$?
        if grep -q "^TURBOFAN $name ok" <<<"$out"; then
            ok=$((ok + 1)); echo "  $name run $i: rc $rc, $(grep "^TURBOFAN" <<<"$out" | head -1)"
        else
            echo "  $name run $i: rc $rc, $(grep -E '^TURBOFAN|\[lxrt\] SIG|Trace/breakpoint|Segmentation' <<<"$out" | head -2 | tr '\n' ' ')"
        fi
    done
    echo "== turbofan $name: $ok of $RUNS ok${*:+ (flags: $*)}"
    tot_ok=$((tot_ok + ok)); tot=$((tot + RUNS))
done
echo "== turbofan: $tot_ok of $tot runs ok"
[ "$tot_ok" -eq "$tot" ]
