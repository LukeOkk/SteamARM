#!/bin/bash
# Run every i386 probe (tests/elf/i386_*.c, built by build_i386.sh into
# <lxrt-root>/tmp) through FEX under the runtime and tally the "== ...: ok"
# verdicts. Probes that print no verdict line (x11 needs a display, the
# cx8 loop is a benchmark) are listed but not counted.
#
# Usage: tests/elf/run_i386.sh [lxrt-root]     (default /tmp/lxrt-root)
set -u
cd "$(dirname "$0")/../.." || exit 1
ROOT="${1:-/tmp/lxrt-root}"
pass=0 fail=0
for src in tests/elf/i386_*.c; do
    name=$(basename "$src" .c)
    [ -x "$ROOT/tmp/$name" ] || { printf '  --    %-16s not built\n' "$name"; continue; }
    out=$(LXRT_ROOT="$ROOT" scripts/run-fex.sh "/tmp/$name" 2>&1)
    v=$(printf '%s\n' "$out" | grep -E '^== ' | tail -1)
    if [ -z "$v" ]; then
        printf '  --    %-16s (no verdict)\n' "$name"
    elif printf '%s' "$v" | grep -q ': xfail'; then
        printf '  xfail %-16s %s\n' "$name" "$v"
    elif printf '%s' "$v" | grep -qE ': ok|[0-9]+ ok, 0 mal'; then
        printf '  ok    %-16s %s\n' "$name" "$v"; pass=$((pass + 1))
    else
        printf '  FAIL  %-16s %s\n' "$name" "$v"; fail=$((fail + 1))
    fi
done
echo "== i386: $pass passed, $fail failed"
[ $fail -eq 0 ]
