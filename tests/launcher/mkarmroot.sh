#!/bin/bash
# scripts/mkarmroot.sh on a fake stage in a temporary directory
# (MKARMROOT_SKIP_RPM: nothing is fetched, nothing real is touched):
#   - out-dir is resolved before anything happens: with a trailing slash the
#     build once went inside the old root, and the swap deleted it together
#     with the client's home;
#   - every rebuild carries tmp/ (the client home) and opt/apps over;
#   - an interrupted swap is put back, from this builder's record and from
#     the leftovers of the previous one, and a leftover that still holds a
#     home is never deleted;
#   - the layouts it refuses: the state or build directory itself, a root
#     inside the stage, a dangling link, a symlinked armroot.new, a directory
#     it did not make, a root a guest runs on;
#   - a symlinked stage is cloned, never written through.
set -u
cd "$(dirname "$0")/../.."
REPO=$PWD
. scripts/roots.sh                       # ROOT_CARRIED_HEAD
G=""
T=$(mktemp -d); trap 'kill $G 2>/dev/null; rm -rf "$T"' EXIT
pass=0 fail=0
check() { if eval "$2"; then echo "  ok    $1"; pass=$((pass+1)); else echo "  FAIL  $1"; fail=$((fail+1)); sed 's/^/        | /' "$T/mk.log"; fi; }
if [ "$(uname)" != Darwin ]; then echo "  skip  mkarmroot (APFS clones: macOS only)"; exit 0; fi

B="$T/build" S="$T/build/armstage-f43" ST="$T/state" R="$T/state/armroot"
mkdir -p "$S/etc/pki/ca-trust/extracted/pem" "$S/usr/lib64/pkcs11" "$S/usr/share/zoneinfo/Etc" "$S/usr/bin" "$ST"
echo stage > "$S/etc/stage-file"; echo x > "$S/usr/share/zoneinfo/Etc/UTC"; echo x > "$S/usr/bin/bash"
GNAME=mkatest-guest-$$
mk() {
    STEAMARM_STATE="$ST" STEAMARM_BUILD="${BUILD_DIR:-$B}" MKARMROOT_SKIP_RPM=1 ROOTS_GUEST_NAME="$GNAME" \
        "$REPO/scripts/mkarmroot.sh" "$@" >"$T/mk.log" 2>&1
}
home() { cat "$R/tmp/armhome/h" 2>/dev/null; }
apps() { cat "$R/opt/apps/a/f" 2>/dev/null; }
snap() { (cd "$S" && find . | LC_ALL=C sort | xargs stat -f '%N %p %z %m' 2>/dev/null | shasum); }
before=$(snap)

# 1. Builds and rebuilds
mk; rc=$?
check "a fresh root is built (rc $rc)" \
    '[ $rc -eq 0 ] && [ -f "$R/.lxrt-armroot" ] && grep -q "^nameserver " "$R/etc/resolv.conf" && [ "$(cat "$R/etc/stage-file")" = stage ]'
mkdir -p "$R/tmp/armhome" "$R/opt/apps/a"
echo home > "$R/tmp/armhome/h"; echo app > "$R/opt/apps/a/f"; echo stale > "$R/etc/stale"
mk "$R/"; rc=$?
check "out-dir with a trailing slash: home and apps kept (rc $rc)" '[ $rc -eq 0 ] && [ "$(home)" = home ] && [ "$(apps)" = app ]'
check "... built beside the old root: its other changes gone, no leftovers" \
    '[ ! -e "$R/etc/stale" ] && [ ! -e "$R/.new" ] && [ ! -e "$R.new" ] && [ ! -e "$R.old" ] && [ ! -e "$R/.lxrt-carried" ]'
mk "$R//"; rc=$?
check "two trailing slashes (rc $rc)" '[ $rc -eq 0 ] && [ "$(home)" = home ] && [ ! -e "$R/.new" ]'
ln -s "$R" "$T/link"
mk "$T/link/"; rc=$?
check "out-dir through a symlink: the root it names is rebuilt (rc $rc)" \
    '[ $rc -eq 0 ] && [ -L "$T/link" ] && [ ! -L "$R" ] && [ "$(home)" = home ] && [ ! -e "$T/link.new" ]'
(cd "$ST" && STEAMARM_STATE="$ST" STEAMARM_BUILD="$B" MKARMROOT_SKIP_RPM=1 ROOTS_GUEST_NAME="$GNAME" \
    "$REPO/scripts/mkarmroot.sh" armroot >"$T/mk.log" 2>&1); rc=$?
check "a relative out-dir is the caller's (rc $rc)" '[ $rc -eq 0 ] && [ "$(home)" = home ] && [ ! -e "$REPO/armroot" ]'
mk "$ST/fresh/"; rc=$?
check "a first build with a trailing slash (rc $rc)" '[ $rc -eq 0 ] && [ -f "$ST/fresh/.lxrt-armroot" ] && [ ! -e "$ST/fresh/.new" ]'

# 2. Interrupted swaps
# a) this builder's record; stopped between its two renames
mv "$R" "$R.old"; mkdir -p "$R.new/opt"; mv "$R.old/tmp" "$R.new/tmp"; mv "$R.old/opt/apps" "$R.new/opt/apps"
printf '%s\ntmp\nopt/apps\n' "$ROOT_CARRIED_HEAD" > "$R.new/.lxrt-carried"
mk; rc=$?
check "swap stopped between the renames: recovered (rc $rc)" \
    '[ $rc -eq 0 ] && [ "$(home)" = home ] && [ "$(apps)" = app ] && [ ! -e "$R.new" ] && [ ! -e "$R.old" ]'
# b) the previous builder (no record): home and apps in armroot.new, old root deleted
mkdir -p "$R.new/opt"; mv "$R/tmp" "$R.new/tmp"; mv "$R/opt/apps" "$R.new/opt/apps"; rm -rf "$R"
mk; rc=$?
check "previous builder stopped after deleting the root: recovered (rc $rc)" \
    '[ $rc -eq 0 ] && [ "$(home)" = home ] && [ "$(apps)" = app ] && [ ! -e "$R.new" ]'
# c) ... stopped while deleting it, the marker already gone
mkdir -p "$R.new/opt"; mv "$R/tmp" "$R.new/tmp"; mv "$R/opt/apps" "$R.new/opt/apps"; rm -f "$R/.lxrt-armroot"
mk; rc=$?
check "half-deleted root without its marker: refused, home left in armroot.new (rc $rc)" \
    '[ $rc -ne 0 ] && grep -q "Keep .*armroot.new" "$T/mk.log" && [ "$(cat "$R.new/tmp/armhome/h")" = home ]'
rm -rf "$R"                              # what the refusal asks for
mk; rc=$?
check "... and once that is removed, recovered (rc $rc)" \
    '[ $rc -eq 0 ] && [ "$(home)" = home ] && [ "$(apps)" = app ] && [ ! -e "$R.new" ]'
# d) a carried home, and the root has one again
mkdir "$R.new"; mv "$R/tmp" "$R.new/tmp"; printf '%s\ntmp\n' "$ROOT_CARRIED_HEAD" > "$R.new/.lxrt-carried"
mkdir -p "$R/tmp/armhome"; echo other > "$R/tmp/armhome/h"
mk; rc=$?
check "a carried home and another in the root: refused, both kept (rc $rc)" \
    '[ $rc -ne 0 ] && [ "$(cat "$R.new/tmp/armhome/h")" = home ] && [ "$(home)" = other ]'
rm -rf "$R/tmp"; mv "$R.new/tmp" "$R/tmp"; rm -rf "$R.new"
# e) no record, a home in armroot.new (tmp/fexhome) and the root has a tmp/
mkdir -p "$R.new/tmp/fexhome" "$R.new/opt/apps/x"; touch "$R.new/opt/apps/x/f"
mk; rc=$?
check "an unrecorded leftover with a home the root's tmp/ would clash with: refused, nothing deleted (rc $rc)" \
    '[ $rc -ne 0 ] && [ -d "$R.new/tmp/fexhome" ] && [ -f "$R.new/opt/apps/x/f" ] && [ "$(home)" = home ]'
rm -rf "$R.new"
# f) this builder's record, stopped after tmp moved and before opt/apps did
mkdir -p "$R.new/opt"; mv "$R/tmp" "$R.new/tmp"
printf '%s\ntmp\n' "$ROOT_CARRIED_HEAD" > "$R.new/.lxrt-carried"
mk; rc=$?
check "swap stopped between the two state moves: recovered (rc $rc)" \
    '[ $rc -eq 0 ] && [ "$(home)" = home ] && [ "$(apps)" = app ] && [ ! -e "$R.new" ]'
# g) the record written, nothing moved yet: the new tree's own tmp/ is not
# taken for the client's
mkdir -p "$R.new/tmp/stage-copy"; echo "$ROOT_CARRIED_HEAD" > "$R.new/.lxrt-carried"
mk; rc=$?
check "swap stopped before anything moved: the new tree is dropped (rc $rc)" \
    '[ $rc -eq 0 ] && [ "$(home)" = home ] && [ ! -e "$R/tmp/stage-copy" ] && [ ! -e "$R.new" ]'
# h) an armroot.old that holds a home is not deleted
mkdir -p "$R.old/tmp/armhome"; echo old > "$R.old/tmp/armhome/h"
mk; rc=$?
check "an armroot.old holding a home: refused, kept (rc $rc)" \
    '[ $rc -ne 0 ] && [ "$(cat "$R.old/tmp/armhome/h")" = old ] && [ "$(home)" = home ]'
rm -rf "$R.old"

# 3. Refusals
touch "$ST/.lxrt-armroot"
mk "$ST"; rc=$?
check "refuses the state directory itself (rc $rc)" \
    '[ $rc -ne 0 ] && grep -q "refusing" "$T/mk.log" && [ "$(home)" = home ] && [ ! -e "$ST.new" ]'
rm -f "$ST/.lxrt-armroot"
touch "$B/.lxrt-armroot"
mk "$B"; rc=$?
check "refuses a root that holds the stage (rc $rc)" '[ $rc -ne 0 ] && [ -f "$S/etc/stage-file" ] && [ ! -e "$B.new" ]'
rm -f "$B/.lxrt-armroot"
mk "$S/sub"; rc=$?
check "refuses a root inside the stage (rc $rc)" '[ $rc -ne 0 ] && [ ! -e "$S/sub" ] && [ ! -e "$S/sub.new" ]'
ln -s "$T/nowhere" "$T/dangling"
mk "$T/dangling"; rc=$?
check "refuses a dangling symlink (rc $rc)" '[ $rc -ne 0 ] && [ ! -e "$T/nowhere" ]'
mkdir "$T/elsewhere"; echo keep > "$T/elsewhere/f"; ln -s "$T/elsewhere" "$R.new"
mk; rc=$?
check "refuses a symlinked armroot.new, deletes nothing through it (rc $rc)" \
    '[ $rc -ne 0 ] && [ "$(cat "$T/elsewhere/f")" = keep ] && [ "$(home)" = home ]'
rm "$R.new"
mkdir -p "$T/handmade/tmp"
mk "$T/handmade"; rc=$?
check "refuses a directory it did not make (rc $rc)" \
    '[ $rc -ne 0 ] && grep -q "not made by this script" "$T/mk.log" && [ -d "$T/handmade/tmp" ]'
# A guest on the root, through a link as /tmp/lxrt-armroot is (a program of
# our own: macOS shows no environment for its platform binaries).
printf '#include <unistd.h>\nint main(void){sleep(60);return 0;}\n' > "$T/g.c"
if cc -o "$T/$GNAME" "$T/g.c" 2>/dev/null; then
    env LXRT_ROOT="$T/link" "$T/$GNAME" & G=$!
    sleep 0.5
    mk; rc=$?
    check "refuses while a guest runs on the root (rc $rc)" \
        '[ $rc -ne 0 ] && grep -q "guests are running" "$T/mk.log" && [ "$(home)" = home ] && [ ! -e "$R.new" ]'
    kill $G 2>/dev/null; wait $G 2>/dev/null; G=""
else
    echo "  skip  a guest on the root (no C compiler)"
fi

# 4. A symlinked stage
mkdir -p "$T/build2"; ln -s "$S" "$T/build2/armstage-f43"
BUILD_DIR="$T/build2" mk; rc=$?
check "a symlinked stage is cloned, not linked (rc $rc)" \
    '[ $rc -eq 0 ] && [ ! -L "$R" ] && [ -f "$R/etc/stage-file" ] && [ -f "$R/etc/resolv.conf" ] && [ "$(home)" = home ]'
after=$(snap)
check "the stage is unchanged" '[ "$before" = "$after" ]'
echo "== mkarmroot: $pass passed, $fail failed"
[ $fail -eq 0 ]
