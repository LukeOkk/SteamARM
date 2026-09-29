#!/bin/bash
# scripts/session.py: one process group and an exit status per launcher
# session (docs/APPLICATION_MANAGER.md). Real processes, no guest: fake
# runners stand in for run-native.sh/run-fex.sh. Runs on macOS and Linux.
#
#   tests/launcher/session.sh
set -u
cd "$(dirname "$0")/../.." || exit 1
S=scripts/session.py
# How the scripts start it (run-app.sh, run-fex.sh, safeguard.sh).
PY=(env PYTHONCOERCECLOCALE=0 STEAMARM_SESSION_PY=1 python3 "$S")
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
status() { cat "$W/ldir/running.status" 2>/dev/null; }
fresh() { rm -f "$W/ldir/running.status" "$W/ldir/running.pgid"; }

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
"${PY[@]}" run "$W/ldir" "$W/runner1" >"$W/log1" 2>&1
rc=$?
[ "$rc" = 7 ] && ok "wrapper exits with the program's status (7)" || bad "wrapper exit $rc"
[ "$(status)" = 7 ] && ok "running.status = 7" || bad "running.status: $(status)"
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
fresh
"${PY[@]}" run "$W/ldir" "$W/runner2" >"$W/log2" 2>&1 &
wrapper=$!
await 3 "$W/ldir/running.pgid" || bad "no pgid file"
sleep 0.5
pgid=$(cat "$W/ldir/running.pgid")
# $! is env's pid, which execs python: the same process.
[ "$pgid" = "$wrapper" ] && ok "the wrapper leads the group (pid = pgid)" || bad "pgid $pgid vs wrapper $wrapper"
[ "$(ps -o pgid= -p $$ | tr -d ' ')" != "$pgid" ] && ok "the group is not the test's" || bad "same group as the test"
t0=$(date +%s)
"${PY[@]}" stop "$W/ldir" 3
wait "$wrapper" 2>/dev/null
[ "$(status)" = "143 signal 15" ] && ok "stop -> '143 signal 15'" || bad "status after stop: $(status)"
[ "$(elapsed $t0)" -le 3 ] && ok "a program that honours SIGTERM stops at once" || bad "stop took $(elapsed $t0)s"
pgrep -f session-test-marker-main >/dev/null && bad "program still running" || ok "program gone"

echo "== stop: a program that ignores SIGTERM is killed after the grace period"
cat > "$W/runner3" <<'EOF'
#!/bin/bash
trap '' TERM
exec -a session-test-marker-stubborn bash -c 'trap "" TERM; while :; do sleep 1; done'
EOF
chmod +x "$W/runner3"
fresh
"${PY[@]}" run "$W/ldir" "$W/runner3" >"$W/log3" 2>&1 &
wrapper=$!
await 3 "$W/ldir/running.pgid" || bad "no pgid file"
sleep 0.5
t0=$(date +%s)
"${PY[@]}" stop "$W/ldir" 2
wait "$wrapper" 2>/dev/null
[ "$(status)" = "137 signal 9" ] && ok "stop -> '137 signal 9'" || bad "status: $(status)"
e=$(elapsed $t0); [ "$e" -ge 2 ] && [ "$e" -le 6 ] && ok "killed after the grace period (${e}s)" || bad "took ${e}s"

echo "== stop with nothing recorded, or a stale group, is a no-op"
fresh
"${PY[@]}" stop "$W/ldir" && ok "no pgid file: exit 0" || bad "stop without pgid"
echo 1 > "$W/ldir/running.pgid"        # pid 1 is not a session.py wrapper
"${PY[@]}" stop "$W/ldir" && ok "stale pgid (pid 1): not signalled, exit 0" || bad "stale pgid"

echo "== already a group leader: a child takes the new session"
# Under job control the wrapper leads the pipeline's group; setsid is refused
# and the rest of the pipeline must not be treated as the program's leftovers.
fresh
t0=$(date +%s)
out=$(bash -c 'set -m; "$@" /bin/sh -c "exit 4" | cat; echo "pipe=${PIPESTATUS[*]}"' _ "${PY[@]}" run "$W/ldir" 2>"$W/log4")
[ "$out" = "pipe=4 0" ] && ok "pipeline leader: exit 4 relayed, cat untouched" || { bad "pipeline: '$out'"; cat "$W/log4"; }
[ "$(elapsed $t0)" -le 2 ] && ok "no grace period spent on the pipe's reader" || bad "took $(elapsed $t0)s"
[ "$(status)" = 4 ] && ok "running.status = 4" || bad "status: $(status)"
if command -v setsid >/dev/null; then
    fresh
    setsid "${PY[@]}" run "$W/ldir" /bin/sh -c 'exit 3' >"$W/log5" 2>&1
    [ "$(status)" = 3 ] && ok "session leader: status 3" || { bad "leader case"; cat "$W/log5"; }
else
    echo "  skip  no setsid command (macOS): session-leader case not exercised"
fi

echo "== what the program inherits from the wrapper"
fresh
env -i PATH="$PATH" HOME="$HOME" PYTHONCOERCECLOCALE=0 STEAMARM_SESSION_PY=1 python3 "$S" run "$W/ldir" \
    /bin/sh -c 'echo "ctype=${LC_CTYPE-unset} coerce=${PYTHONCOERCECLOCALE-unset} marker=${STEAMARM_SESSION_PY-unset}"' \
    >"$W/log6" 2>&1
grep -q "ctype=unset coerce=unset marker=unset" "$W/log6" \
    && ok "no locale from Python, no helper variables" || { bad "environment"; cat "$W/log6"; }
if command -v perl >/dev/null; then
    "${PY[@]}" detach perl -e 'print "pipe=", ($SIG{PIPE} || "DEFAULT"), "\n"' >"$W/log7" 2>&1
    grep -q "pipe=DEFAULT" "$W/log7" && ok "detach: SIGPIPE back to default" || { bad "detach SIGPIPE"; cat "$W/log7"; }
    fresh
    nohup "${PY[@]}" run "$W/ldir" perl -e 'print "hup=", ($SIG{HUP} || "DEFAULT"), "\n"' >"$W/log8" 2>&1
    grep -q "hup=IGNORE" "$W/log8" && ok "run under nohup: SIGHUP still ignored" || { bad "nohup SIGHUP"; cat "$W/log8"; }
else
    echo "  skip  no perl: signal dispositions not checked"
fi

echo "== detach: a service leaves the caller's group"
"${PY[@]}" detach bash -c 'exec -a session-test-marker-svc sleep 300' &
svc=$!
sleep 0.5
mine=$(ps -o pgid= -p $$ | tr -d ' ')
theirs=$(ps -o pgid= -p "$svc" | tr -d ' ')
[ -n "$theirs" ] && [ "$theirs" != "$mine" ] && ok "detached service in group $theirs, test in $mine" || bad "service group $theirs vs $mine"
kill "$svc" 2>/dev/null; wait "$svc" 2>/dev/null

echo "== a command that cannot start"
fresh
"${PY[@]}" run "$W/ldir" "$W/does-not-exist" >"$W/log9" 2>&1
[ "$?" = 127 ] && [ "$(status)" = 127 ] && ok "missing program: status 127" || bad "missing program"

echo "== run-app.sh --reap: what a finished session left in its session, and nothing else"
# The native client's webhelper zygotes leave the program's process group and
# outlive it with parent 1 (benchmarks/stage23-frame-root.txt C2, F7). Fake
# guests stand in for them: only their command line names build/lxrun.
R="$W/state"
mkdir -p "$R/launcher"
export FAKE_LX="$W/build/lxrun"
# The program leaves a helper in a process group of its own that ignores
# SIGTERM, and exits (or, with "stay", keeps running).
cat > "$W/runner-zygote" <<'EOF'
#!/bin/bash
python3 - "$FAKE_LX session-test-marker-zygote" <<'PY' &
import os, signal, sys
os.setpgid(0, 0)
signal.signal(signal.SIGTERM, signal.SIG_IGN)
os.execvp("sleep", [sys.argv[1], "300"])
PY
sleep 0.5
[ "${1:-}" = stay ] && exec -a session-test-marker-program sleep 300
exit 0
EOF
chmod +x "$W/runner-zygote"
zygote() { pgrep -f "session-test-marker-zygote"; }
# A guest of another session (a terminal's run-steam.sh, say): never touched.
"${PY[@]}" detach bash -c 'exec -a "$0" sleep 300' "$FAKE_LX session-test-marker-other" &
other=$!
"${PY[@]}" run "$R/launcher" "$W/runner-zygote" >"$W/log10" 2>&1 &
wrapper=$!
wait "$wrapper"
sid=$(cat "$R/launcher/running.pgid" 2>/dev/null)
z=$(zygote)
[ -n "$z" ] && [ "$(ps -o pgid= -p "$z" | tr -d ' ')" != "$sid" ] \
    && ok "the helper outlived the wrapper, outside its group" || bad "no helper left ($z) or in group $sid"
t0=$(date +%s)
STEAMARM_STATE="$R" scripts/run-app.sh --reap "$sid" >"$W/log11" 2>&1
[ -z "$(zygote)" ] && ok "--reap: the session's helper is gone (SIGKILL after SIGTERM)" || bad "--reap left $(zygote)"
e=$(elapsed $t0); [ "$e" -le 5 ] && ok "--reap took ${e}s" || bad "--reap took ${e}s"
grep -q "left behind: $z" "$W/log11" && ok "--reap names what it stopped" || { bad "--reap output"; cat "$W/log11"; }
kill -0 "$other" 2>/dev/null && ok "another session's guest untouched" || bad "another session's guest was stopped"

echo "== --reap waits for the wrapper; the session's watcher reaps after it"
rm -f "$R/launcher/running.status" "$R/launcher/running.pgid"
"${PY[@]}" run "$R/launcher" "$W/runner-zygote" stay >"$W/log12" 2>&1 &
wrapper=$!
await 3 "$R/launcher/running.pgid" || bad "no pgid file"
sleep 1
z=$(zygote)
STEAMARM_STATE="$R" scripts/run-app.sh --reap >"$W/log13" 2>&1
[ -n "$z" ] && kill -0 "$z" 2>/dev/null && ok "--reap of a running session: nothing" || bad "a running session was reaped"
STEAMARM_STATE="$R" scripts/run-app.sh --reap-after "$wrapper" >"$W/log14" 2>&1 &
watcher=$!
"${PY[@]}" stop "$R/launcher" 1
wait "$wrapper" 2>/dev/null
kill -0 "$z" 2>/dev/null && ok "a stop signals the group: the helper outside it survives" || bad "helper gone at stop"
wait "$watcher"
[ -z "$(zygote)" ] && ok "the watcher reaped it once the wrapper exited" || bad "watcher left $(zygote)"
kill -0 "$other" 2>/dev/null && ok "another session's guest still untouched" || bad "another session's guest was stopped"
kill "$other" 2>/dev/null; wait "$other" 2>/dev/null

echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
