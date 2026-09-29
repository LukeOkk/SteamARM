#!/bin/bash
# scripts/safeguard.sh memory rule, with two fake guests (a tiny program
# built here -- macOS kills relocated copies of its own binaries -- under a
# name of its own, SAFEGUARD_GUEST_NAME, so real lxrun guests are
# never touched) and a free-memory threshold forced above the real level:
#   1. one guest (the largest) is stopped after SAFEGUARD_LOW_SECS checks;
#   2. the others only SAFEGUARD_GRACE_SECS later, if the pressure remains;
#   3. the reason reaches logs/safeguard.last and the app log in logs/current;
#   4. no "ps: stdout: Broken pipe" noise in the guard's log.
set -u
cd "$(dirname "$0")/../.."
NAME=sgtest-guest-$$
T=$(mktemp -d); trap 'kill $SG $A $B 2>/dev/null; rm -rf "$T"' EXIT
mkdir -p "$T/state/logs" "$T/bin"
printf '#include <unistd.h>\nint main(void){sleep(60);return 0;}\n' > "$T/g.c"
cc -o "$T/bin/$NAME" "$T/g.c" || { echo "SKIP: no C compiler"; exit 0; }
: > "$T/state/logs/app.log"
echo "$T/state/logs/app.log" > "$T/state/logs/current"
"$T/bin/$NAME" 60 & A=$!
"$T/bin/$NAME" 60 & B=$!
sleep 1
pass=0 fail=0
check() { if eval "$2"; then echo "  ok    $1"; pass=$((pass+1)); else echo "  FAIL  $1"; fail=$((fail+1)); fi; }
alive() { kill -0 "$1" 2>/dev/null; }
count() { n=0; alive $A && n=$((n+1)); alive $B && n=$((n+1)); echo $n; }
STEAMARM_STATE="$T/state" SAFEGUARD_GUEST_NAME="$NAME" SAFEGUARD_MIN_FREE=101 \
    SAFEGUARD_LOW_SECS=2 SAFEGUARD_GRACE_SECS=4 \
    scripts/safeguard.sh run >"$T/sg.log" 2>&1 & SG=$!
sleep 3.5
check "one guest stopped first" '[ "$(count)" -eq 1 ]'
check "STOP line in the guard log" 'grep -q "STOP largest guest: free memory" "$T/sg.log"'
check "reason in safeguard.last" 'grep -q "stopped the largest guest" "$T/state/logs/safeguard.last"'
check "reason in the app log" 'grep -q "^\[safeguard\] .*free memory" "$T/state/logs/app.log"'
sleep 5
check "the rest stopped after the grace period" '[ "$(count)" -eq 0 ]'
check "KILL line in the guard log" 'grep -q "KILL: free memory .*still after" "$T/sg.log"'
check "no ps broken-pipe noise" '! grep -q "Broken pipe" "$T/sg.log"'
echo "$pass passed, $fail failed"
[ $fail -eq 0 ]
