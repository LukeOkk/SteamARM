#!/bin/bash
# scripts/run-steam-arm64.sh with a stand-in for lxrun (tests/launcher/
# fake_lxrun.c, STEAMARM_LXRUN), roots and links in a temporary directory, no
# memory guard and no X server; no guest runs, nothing real is touched:
#   1. ARMROOT without ARMROOT_LINK: a root that is not one of the state
#      directory's is refused, and the launcher's /tmp/lxrt-armroot is left
#      alone;
#   2. a link a running guest uses is not re-pointed, and a root a guest runs
#      on is not started twice;
#   3. stopping the session stops this run's orphaned "zygote" (parent 1) and
#      leaves another session's alone, though both fit the old pattern
#      (this checkout's lxrun, parent 1, a steamrtarm64 program, started
#      during the session).
set -u
cd "$(dirname "$0")/../.."
# An orphan's parent is launchd (1) on macOS; Linux may give it a subreaper.
if [ "$(uname)" != Darwin ]; then echo "  skip  run_steam_arm64 (macOS only)"; exit 0; fi
T=$(mktemp -d)
FAKE="$T/bin/fakelxrun-$$"
G=""
cleanup() {
    kill $G 2>/dev/null
    pkill -f "$FAKE" 2>/dev/null
    rm -rf "$T"
}
trap cleanup EXIT
pass=0 fail=0
check() { if eval "$2"; then echo "  ok    $1"; pass=$((pass+1)); else echo "  FAIL  $1"; fail=$((fail+1)); sed 's/^/        | /' "$T/run.log"; fi; }
mkdir -p "$T/bin" "$T/state" "$T/fake"
cc -o "$FAKE" tests/launcher/fake_lxrun.c 2>/dev/null || { echo "SKIP: no C compiler"; exit 0; }
GNAME=rsatest-guest-$$
printf '#include <unistd.h>\nint main(void){sleep(60);return 0;}\n' > "$T/g.c"
cc -o "$T/bin/$GNAME" "$T/g.c"
for r in rootA rootB; do
    mkdir -p "$T/$r/tmp/armhome/.local/share/Steam/steamrtarm64"
    echo client > "$T/$r/tmp/armhome/.local/share/Steam/steamrtarm64/steam"
done
run() {
    env STEAMARM_STATE="$T/state" STEAMARM_LXRUN="$FAKE" STEAMARM_NO_SAFEGUARD=1 DISPLAY=:99 \
        ROOTS_GUEST_NAME="$GNAME" FAKE_DIR="$T/fake" "$@" >"$T/run.log" 2>&1
}

# 1. The shared link is not taken for a root of its own. rootC has no client,
# so a script that got this wrong would still stop (at "no arm64 client")
# before it could touch the real /tmp/lxrt-armroot.
mkdir -p "$T/rootC"
tmp_link_before=$(readlink /tmp/lxrt-armroot 2>/dev/null || echo none)
run ARMROOT="$T/rootC" scripts/run-steam-arm64.sh --for 1; rc=$?
check "ARMROOT that is not \$STATE/armroot, without ARMROOT_LINK: refused (rc $rc)" \
    '[ $rc -ne 0 ] && grep -q "ARMROOT_LINK" "$T/run.log" && [ -z "$(ls "$T/fake")" ]'
check "... /tmp/lxrt-armroot left as it was" '[ "$(readlink /tmp/lxrt-armroot 2>/dev/null || echo none)" = "$tmp_link_before" ]'

# 2. Guests on the link or on the root.
ln -s "$T/rootA" "$T/link"
env LXRT_ROOT="$T/link" "$T/bin/$GNAME" & G=$!
sleep 0.5
run ARMROOT="$T/rootB" ARMROOT_LINK="$T/link" scripts/run-steam-arm64.sh --for 1; rc=$?
check "a link a running guest uses is not re-pointed (rc $rc)" \
    '[ $rc -ne 0 ] && grep -q "guests are running" "$T/run.log" && [ "$(readlink "$T/link")" = "$T/rootA" ] && [ -z "$(ls "$T/fake")" ]'
run ARMROOT="$T/rootA" ARMROOT_LINK="$T/link2" scripts/run-steam-arm64.sh --for 1; rc=$?
check "a root a guest runs on is not started again through another link (rc $rc)" \
    '[ $rc -ne 0 ] && grep -q "guests are running" "$T/run.log" && [ ! -e "$T/link2" ] && [ -z "$(ls "$T/fake")" ]'
kill $G 2>/dev/null; wait $G 2>/dev/null; G=""

# 3. Orphans: this run's, and another session's started during it.
run ARMROOT="$T/rootB" ARMROOT_LINK="$T/link" scripts/run-steam-arm64.sh --for 5 &
S=$!
sleep 1
env FAKE_DIR="$T/fake" STEAMARM_RUN_ID=other-session "$FAKE" /tmp/armhome/.local/share/Steam/steamrtarm64/steam &
OTHER=$!
wait $S; rc=$?
check "the session ran on the re-pointed link (rc $rc)" \
    '[ $rc -eq 0 ] && [ "$(readlink "$T/link")" = "$T/rootB" ] && grep -q "re-pointing" "$T/run.log"'
ours=$(ls "$T/fake" | sed -n 's/^zygote\.steam-arm64\..*\.\([0-9]*\)$/\1/p')
theirs=$(ls "$T/fake" | sed -n 's/^zygote\.other-session\.\([0-9]*\)$/\1/p')
check "this run's zygote was there" '[ -n "$ours" ]'
check "... and is stopped as an orphan" \
    '[ -n "$ours" ] && ! kill -0 $ours 2>/dev/null && grep -q "stopping orphaned webhelper processes: $ours " "$T/run.log"'
check "the other session's orphaned zygote is left running" '[ -n "$theirs" ] && kill -0 $theirs 2>/dev/null'
check "... and so is its client" 'kill -0 $OTHER 2>/dev/null'
check "... and the log says why" 'grep -q "left running, without this run" "$T/run.log"'
kill $OTHER $theirs 2>/dev/null
echo "== run_steam_arm64: $pass passed, $fail failed"
[ $fail -eq 0 ]
