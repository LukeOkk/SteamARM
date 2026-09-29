#!/bin/bash
# scripts/session.py: one process group and an exit status per launcher
# session (docs/APPLICATION_MANAGER.md). Real processes, no guest: fake
# runners stand in for run-native.sh/run-fex.sh. Runs on macOS and Linux.
#
#   tests/launcher/session.sh
set -u
cd "$(dirname "$0")/../.." || exit 1
S=scripts/session.py
W="$(mktemp -d "${TMPDIR:-/tmp}/session-test.XXXXXX")"
pass=0 fail=0
ok()  { pass=$((pass + 1)); echo "  ok    $1"; }
bad() { fail=$((fail + 1)); echo "  FAIL  $1"; }
cleanup() { pkill -f "session-test-marker" 2>/dev/null; rm -rf "$W"; }
trap cleanup EXIT
# Waits up to $1 seconds for the file $2 to appear.
await() { local i; for i in $(seq 1 $(( ${1} * 10 ))); do [ -e "$2" ] && return 0; sleep 0.1; done; return 1; }
# Live members of group $1 other than its leader (zombies are not processes to stop).
in_group() { ps -Ao pid=,pgid=,stat= | awk -v g="$1" '$2 == g && $1 != g && $3 !~ /^Z/ {n++} END {print n+0}'; }
elapsed() { echo $(( $(date +%s) - $1 )); }

echo "== the program's status is recorded, its leftovers are cleaned"
# The "program" leaves a child behind and exits 7. The child ignores SIGTERM
# so that the wrapper's SIGKILL step is exercised too.
cat > "$W/runner1" <<'EOF'
#!/bin/bash
bash -c 'trap "" TERM; exec -a session-test-marker-orphan sleep 300' &
exit 7
EOF
chmod +x "$W/runner1"
mkdir -p "$W/ldir"
t0=$(date +%s)
python3 $S run "$W/ldir" "$W/runner1" >"$W/log1" 2>&1
rc=$?
[ "$rc" = 7 ] && ok "wrapper exits with the program's status (7)" || bad "wrapper exit $rc"
[ "$(cat "$W/ldir/running.status" 2>/dev/null)" = 7 ] && ok "running.status = 7" || bad "running.status: $(cat "$W/ldir/running.status" 2>/dev/null)"
pgid=$(cat "$W/ldir/running.pgid" 2>/dev/null)
[ -n "$pgid" ] && ok "running.pgid written ($pgid)" || bad "no running.pgid"
[ "$(in_group "$pgid")" = 0 ] && ok "orphan (ignoring SIGTERM) killed with the group" || bad "$(in_group "$pgid") left in group $pgid"
e=$(elapsed $t0); [ "$e" -ge 6 ] && [ "$e" -le 12 ] && ok "grace + SIGTERM + SIGKILL took ${e}s" || bad "took ${e}s"
grep -q "SIGKILL" "$W/log1" && ok "log names the SIGKILL step" || { bad "log"; cat "$W/log1"; }

echo "== stop: SIGTERM to the group, status recorded"
cat > "$W/runner2" <<'EOF'
#!/bin/bash
exec -a session-test-marker-main sleep 300
EOF
chmod +x "$W/runner2"
rm -f "$W/ldir/running.status"
python3 $S run "$W/ldir" "$W/runner2" >"$W/log2" 2>&1 &
wrapper=$!
await 3 "$W/ldir/running.pgid" || bad "no pgid file"
sleep 0.5
pgid=$(cat "$W/ldir/running.pgid")
[ "$pgid" = "$wrapper" ] && ok "the wrapper leads the group (pid = pgid)" || bad "pgid $pgid vs wrapper $wrapper"
[ "$(ps -o pgid= -p $$ | tr -d ' ')" != "$pgid" ] && ok "the group is not the test's" || bad "same group as the test"
t0=$(date +%s)
python3 $S stop "$W/ldir" 3
wait "$wrapper" 2>/dev/null
[ "$(cat "$W/ldir/running.status" 2>/dev/null)" = 143 ] && ok "stop -> status 143 (SIGTERM)" || bad "status after stop: $(cat "$W/ldir/running.status" 2>/dev/null)"
[ "$(elapsed $t0)" -le 3 ] && ok "a program that honours SIGTERM stops at once" || bad "stop took $(elapsed $t0)s"
pgrep -f session-test-marker-main >/dev/null && bad "program still running" || ok "program gone"

echo "== stop: a program that ignores SIGTERM is killed after the grace period"
cat > "$W/runner3" <<'EOF'
#!/bin/bash
trap '' TERM
exec -a session-test-marker-stubborn bash -c 'trap "" TERM; while :; do sleep 1; done'
EOF
chmod +x "$W/runner3"
rm -f "$W/ldir/running.status"
python3 $S run "$W/ldir" "$W/runner3" >"$W/log3" 2>&1 &
wrapper=$!
await 3 "$W/ldir/running.pgid" || bad "no pgid file"
sleep 0.5
t0=$(date +%s)
python3 $S stop "$W/ldir" 2
wait "$wrapper" 2>/dev/null
[ "$(cat "$W/ldir/running.status" 2>/dev/null)" = 137 ] && ok "stop -> status 137 (SIGKILL)" || bad "status: $(cat "$W/ldir/running.status" 2>/dev/null)"
e=$(elapsed $t0); [ "$e" -ge 2 ] && [ "$e" -le 6 ] && ok "killed after the grace period (${e}s)" || bad "took ${e}s"

echo "== stop with nothing recorded, or a stale group, is a no-op"
rm -f "$W/ldir/running.pgid"
python3 $S stop "$W/ldir" && ok "no pgid file: exit 0" || bad "stop without pgid"
echo 1 > "$W/ldir/running.pgid"        # pid 1 is not a session.py wrapper
python3 $S stop "$W/ldir" && ok "stale pgid (pid 1): not signalled, exit 0" || bad "stale pgid"

echo "== already a group leader: falls back to a new process group"
if command -v setsid >/dev/null; then
    rm -f "$W/ldir/running.status" "$W/ldir/running.pgid"
    setsid python3 $S run "$W/ldir" /bin/sh -c 'exit 3' >"$W/log4" 2>&1
    [ "$(cat "$W/ldir/running.status" 2>/dev/null)" = 3 ] && ok "status 3 as a session leader" || { bad "leader case"; cat "$W/log4"; }
else
    echo "  skip  no setsid command (macOS): leader fallback not exercised"
fi

echo "== detach: a service leaves the caller's group"
python3 $S detach bash -c 'exec -a session-test-marker-svc sleep 300' &
svc=$!
sleep 0.5
mine=$(ps -o pgid= -p $$ | tr -d ' ')
theirs=$(ps -o pgid= -p "$svc" | tr -d ' ')
[ -n "$theirs" ] && [ "$theirs" != "$mine" ] && ok "detached service in group $theirs, test in $mine" || bad "service group $theirs vs $mine"
kill "$svc" 2>/dev/null

echo "== a command that cannot start"
rm -f "$W/ldir/running.status"
python3 $S run "$W/ldir" "$W/does-not-exist" >"$W/log5" 2>&1
[ "$?" = 127 ] && [ "$(cat "$W/ldir/running.status")" = 127 ] && ok "missing program: status 127" || bad "missing program"

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
