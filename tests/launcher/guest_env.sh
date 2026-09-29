#!/bin/bash
# A root's guest environment (scripts/guest-env.sh, read by run-native.sh and
# run-steam-arm64.sh) and the Steam Frame root builder (scripts/mkframeroot.sh)
# on a fake extraction: the overlay, the carried-over client home, the
# refusals, paths resolved before anything is touched (a symlinked SRC, an
# OUT through a link, nested or relative paths), and an extraction left
# exactly as it was. Nothing real is touched.
set -u
cd "$(dirname "$0")/../.."
REPO=$PWD
. scripts/roots.sh                       # ROOT_CARRIED_HEAD, canon_path
G=""
T=$(mktemp -d); trap 'kill $G 2>/dev/null; rm -rf "$T"' EXIT
pass=0 fail=0
check() { if eval "$2"; then echo "  ok    $1"; pass=$((pass+1)); else echo "  FAIL  $1"; fail=$((fail+1)); fi; }

# 1. guest_env_from_root
mkdir -p "$T/r"
printf '# comment\nGE_A=1\nGE_B=two words\nGE_SET=root\nGE_EMPTY=root\nnot a line\n1BAD=x\nGE_LAST=end' > "$T/r/.lxrt-guest-env"
out=$(env -u GE_A -u GE_B -u GE_LAST GE_SET=caller GE_EMPTY= bash -c '
    . scripts/guest-env.sh; guest_env_from_root "$1"
    printf "%s|%s|%s|%s|%s|%s" "$GE_A" "$GE_B" "$GE_SET" "${GE_EMPTY-unset}" "$GE_LAST" "$(env | grep -c "^1BAD=")"' _ "$T/r")
check "unset variables come from the root's file" '[ "${out%%|*}" = 1 ]'
check "values keep their spaces" '[[ "$out" == "1|two words|"* ]]'
check "a variable the caller set wins" '[[ "$out" == *"|caller|"* ]]'
check "so does one set to empty" '[[ "$out" == *"|caller||"* ]]'
check "last line without a newline is read" '[[ "$out" == *"|end|"* ]]'
check "a line that is not NAME=VALUE is skipped" '[[ "$out" == *"|0" ]]'
out=$(bash -c '. scripts/guest-env.sh; guest_env_from_root "$1"; echo "rc=$?"' _ "$T/none")
check "no file: nothing done, status 0" '[ "$out" = rc=0 ]'

# 2. mkframeroot.sh (APFS clones: macOS only)
if [ "$(uname)" != Darwin ]; then
    echo "  skip  mkframeroot (not macOS)"
else
    S="$T/vol/rootfs" O="$T/vol/arm64root"
    mkdir -p "$S/etc" "$S/tmp" "$S/usr/share/X11/locale" "$S/usr/lib/locale/C.utf8" \
        "$S/usr/bin" "$S/etc/ssl/certs" "$S/usr/share/zoneinfo/Etc"
    printf 'NAME="SteamOS"\nID=steamos\nBUILD_ID=20260922.5152327\nVERSION_ID=0.3.0\n' > "$S/etc/os-release"
    for f in usr/share/X11/locale/locale.dir usr/bin/lsof usr/lib/libnssckbi.so usr/lib/libGLX_mesa.so.0 \
             usr/lib/libGL.so.1 etc/ssl/certs/ca-certificates.crt usr/share/zoneinfo/Etc/UTC tmp/initrd.img; do
        echo x > "$S/$f"
    done
    ln -s ../usr/share/zoneinfo/Etc/UTC "$S/etc/localtime"
    before=$(cd "$S" && find . | LC_ALL=C sort | xargs stat -f '%N %p %z %m' 2>/dev/null | shasum)
    mk() { STEAMARM_STATE="$T/state" scripts/mkframeroot.sh "$@" >"$T/mk.log" 2>&1; }
    mk "$S" "$O"; rc=$?
    check "a fresh root is built (rc $rc)" '[ $rc -eq 0 ] && [ -f "$O/.lxrt-frameroot" ]'
    check "resolv.conf and the GL environment in the derived root" \
        'grep -q "^nameserver " "$O/etc/resolv.conf" && grep -qx "LIBGL_ALWAYS_INDIRECT=1" "$O/.lxrt-guest-env" &&
         grep -qx "__GLX_VENDOR_LIBRARY_NAME=mesa" "$O/.lxrt-guest-env" && grep -qx "MESA_LOADER_DRIVER_OVERRIDE=swrast" "$O/.lxrt-guest-env"'
    check "linked from the state dir" '[ "$(readlink "$T/state/arm64root")" = "$(canon_path "$O")" ]'
    mkdir -p "$O/tmp/armhome/.local/share/Steam/steamrtarm64" "$O/opt/apps/a"
    echo home > "$O/tmp/armhome/.local/share/Steam/steamrtarm64/steam"; echo app > "$O/opt/apps/a/f"
    echo stale > "$O/etc/stale"
    mk "$S" "$O"; rc=$?
    check "a rebuild keeps tmp (the client home) and opt/apps (rc $rc)" \
        '[ "$(cat "$O/tmp/armhome/.local/share/Steam/steamrtarm64/steam")" = home ] && [ "$(cat "$O/opt/apps/a/f")" = app ]'
    check "a rebuild drops other changes to the old root" '[ ! -e "$O/etc/stale" ] && [ ! -e "$O.old" ] && [ ! -e "$O.new" ]'
    # An interrupted swap: the home sits in OUT.new, the old root in OUT.old.
    mv "$O" "$O.old"; mkdir -p "$O.new"; mv "$O.old/tmp" "$O.new/tmp"; touch "$O.new/.lxrt-carried"
    mk "$S" "$O"; rc=$?
    check "an interrupted swap is recovered, home kept (rc $rc)" \
        '[ "$(cat "$O/tmp/armhome/.local/share/Steam/steamrtarm64/steam" 2>/dev/null)" = home ]'
    mkdir -p "$T/vol/handmade/tmp"
    mk "$S" "$T/vol/handmade"; rc=$?
    check "refuses a root it did not make" '[ $rc -ne 0 ] && grep -q "not made by this script" "$T/mk.log"'
    mk --adopt "$S" "$T/vol/handmade"; rc=$?
    check "--adopt rebuilds it (rc $rc)" '[ $rc -eq 0 ] && [ -f "$T/vol/handmade/.lxrt-frameroot" ]'
    mk "$S" "$S/sub"; rc=$?
    check "refuses a derived root inside the extraction" '[ $rc -ne 0 ] && grep -q "inside the extraction" "$T/mk.log"'
    mkdir -p "$T/notsteamos/etc"; echo ID=arch > "$T/notsteamos/etc/os-release"
    mk "$T/notsteamos" "$T/vol/x"; rc=$?
    check "refuses a root that is not SteamOS" '[ $rc -ne 0 ] && [ ! -e "$T/vol/x" ]'
    # Paths are resolved before anything is made, cloned or deleted.
    homeok() { [ "$(cat "$O/tmp/armhome/.local/share/Steam/steamrtarm64/steam" 2>/dev/null)" = home ]; }
    mk "$S" "$S/work/arm64root"; rc=$?
    check "refuses a root nested inside the extraction before making anything there" \
        '[ $rc -ne 0 ] && grep -q "inside the extraction" "$T/mk.log" && [ ! -e "$S/work" ]'
    ln -s rootfs "$T/vol/current"
    mk "$T/vol/current/"; rc=$?
    check "SRC through a symlink: the tree is cloned, not the link (rc $rc)" \
        '[ $rc -eq 0 ] && [ -d "$O" ] && [ ! -L "$O" ] && [ -f "$O/.lxrt-frameroot" ] && [ ! -e "$S/.lxrt-frameroot" ] && homeok'
    mk "$S" "$T/state/arm64root"; rc=$?
    check "OUT through the state link: the root it names is rebuilt (rc $rc)" \
        '[ $rc -eq 0 ] && [ -L "$T/state/arm64root" ] && [ ! -L "$O" ] && homeok && [ ! -e "$T/state/arm64root.new" ] && [ ! -e "$T/state/arm64root.old" ]'
    (cd "$T/vol" && STEAMARM_STATE="$T/state2" "$REPO/scripts/mkframeroot.sh" rootfs rel >"$T/mk.log" 2>&1); rc=$?
    check "relative SRC and OUT are the caller's (rc $rc)" '[ $rc -eq 0 ] && [ -f "$T/vol/rel/.lxrt-frameroot" ] && [ ! -e "$REPO/rel" ]'
    mk --adopt "$S" "$T/vol"; rc=$?
    check "refuses, even with --adopt, an OUT that holds the extraction" \
        '[ $rc -ne 0 ] && grep -q "extraction is inside" "$T/mk.log" && [ ! -e "$T/vol.new" ]'
    mkdir "$T/elsewhere"; echo keep > "$T/elsewhere/f"; ln -s "$T/elsewhere" "$O.new"
    mk "$S" "$O"; rc=$?
    check "refuses a symlinked OUT.new, deletes nothing through it" \
        '[ $rc -ne 0 ] && [ "$(cat "$T/elsewhere/f")" = keep ] && homeok'
    rm "$O.new"
    # A carried home, and OUT has one again: both kept.
    mkdir "$O.new"; mv "$O/tmp" "$O.new/tmp"; printf '%s\ntmp\n' "$ROOT_CARRIED_HEAD" > "$O.new/.lxrt-carried"
    mkdir -p "$O/tmp/armhome"; echo other > "$O/tmp/armhome/x"
    mk "$S" "$O"; rc=$?
    check "a carried home and another in OUT: refused, both kept (rc $rc)" \
        '[ $rc -ne 0 ] && [ "$(cat "$O.new/tmp/armhome/.local/share/Steam/steamrtarm64/steam")" = home ] && [ -f "$O/tmp/armhome/x" ]'
    rm -rf "$O/tmp"; mv "$O.new/tmp" "$O/tmp"; rm -rf "$O.new"
    # A guest on the root (a program of our own: macOS shows no environment
    # for its platform binaries).
    GNAME=mkftest-guest-$$
    printf '#include <unistd.h>\nint main(void){sleep(60);return 0;}\n' > "$T/g.c"
    if cc -o "$T/$GNAME" "$T/g.c" 2>/dev/null; then
        env LXRT_ROOT="$T/state/arm64root" "$T/$GNAME" & G=$!
        sleep 0.5
        ROOTS_GUEST_NAME=$GNAME mk "$S" "$O"; rc=$?
        check "refuses while a guest runs on the root (rc $rc)" \
            '[ $rc -ne 0 ] && grep -q "guests are running" "$T/mk.log" && homeok && [ ! -e "$O.new" ]'
        kill $G 2>/dev/null; wait $G 2>/dev/null; G=""
    else
        echo "  skip  a guest on the root (no C compiler)"
    fi
    after=$(cd "$S" && find . | LC_ALL=C sort | xargs stat -f '%N %p %z %m' 2>/dev/null | shasum)
    check "the extraction is unchanged" '[ "$before" = "$after" ]'
fi
echo "== guest_env: $pass passed, $fail failed"
[ $fail -eq 0 ]
