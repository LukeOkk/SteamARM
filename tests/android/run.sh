#!/bin/bash
# Android 11 userspace under lxrun, no VM (benchmarks/stage25-android-
# userspace.txt, docs/ANDROID_RUNTIME_ARCHITECTURE.md). Runs bionic programs
# from the root scripts/mkandroidroot.sh makes; skips cleanly without it.
#
#   tests/android/run.sh            ANDROID_ROOT_DIR (default
#                                   /Volumes/SteamARMAndroid/root), LXRUN
#                                   (default build/lxrun)
#
# Writes only inside the root's /data/local/tmp and dev/socket (the logd
# stand-in). Starts no daemon it does not stop; stops only its own PIDs.
set -uo pipefail
cd "$(dirname "$0")/../.." || exit 1
ROOT="${ANDROID_ROOT_DIR:-/Volumes/SteamARMAndroid/root}"
LXRUN="${LXRUN:-build/lxrun}"
PASS=0; FAIL=0; XFAIL=0
ok()    { echo "  ok    $1"; PASS=$((PASS+1)); }
bad()   { echo "  FAIL  $1"; echo "        $2"; FAIL=$((FAIL+1)); }
xfail() { echo "  xfail $1"; echo "        $2"; XFAIL=$((XFAIL+1)); }
deadline() { perl -e 'alarm shift; exec @ARGV' "$@"; }

echo "== tests/android"
if [ ! -x "$ROOT/system/bin/toybox" ]; then
    echo "  skip  no Android root at $ROOT (scripts/mkandroidroot.sh)"
    exit 0
fi
[ -x "$LXRUN" ] || { echo "  FAIL  no $LXRUN (make lxrt)"; exit 1; }

# A guest command with Android's environment (init.environ.rc) and nothing
# of the host's. LXRT_GUEST_PAGE=4096: bionic's linker maps with its 4 KiB
# PAGE_SIZE whatever AT_PAGESZ says, and Android 11's ELF files are 4 KiB-
# aligned (runtime/subpage.c serves them).
BCP=$(sed -n 's/^ *export BOOTCLASSPATH //p' "$ROOT/init.environ.rc")
g() {
    env -i HOME=/data/local/tmp PATH=/system/bin:/system/xbin TERM=dumb \
        ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_ART_ROOT=/apex/com.android.art \
        ANDROID_I18N_ROOT=/apex/com.android.i18n ANDROID_TZDATA_ROOT=/apex/com.android.tzdata \
        ANDROID_STORAGE=/storage BOOTCLASSPATH="$BCP" \
        LXRT_ROOT="$ROOT" LXRT_GUEST_PAGE=4096 ${GENV:-} \
        /usr/bin/perl -e 'alarm shift; exec @ARGV' "${DL:-30}" "$LXRUN" "$@" 2>/dev/null
}

# 1. bionic's linker run as a program, as the task names it.
out=$(g /system/bin/linker64 /system/bin/toybox ls /); rc=$?
if [ "$rc" -eq 0 ] && grep -qx system <<<"$out" && grep -qx apex <<<"$out"; then
    ok "linker64 /system/bin/toybox ls /: $(wc -l <<<"$out" | tr -d ' ') entries"
else bad "linker64 toybox ls /" "rc=$rc $(head -3 <<<"$out")"; fi

# 2. toybox through PT_INTERP. It links libcrypto.so, whose BoringSSL FIPS
# module hashes its own code at load (runtime/tls.c keeps its bytes).
out=$(g /system/bin/toybox echo toybox echo works); rc=$?
[ "$rc" -eq 0 ] && [ "$out" = "toybox echo works" ] && ok "toybox echo" || bad "toybox echo" "rc=$rc $out"
out=$(g /system/bin/toybox uname -m -s); rc=$?
[ "$rc" -eq 0 ] && [ "$out" = "Linux aarch64" ] && ok "toybox uname -s -m: $out" || bad "toybox uname" "rc=$rc $out"
want=$(shasum -a 256 "$ROOT/system/framework/framework.jar" | awk '{print $1}')
out=$(g /system/bin/toybox sha256sum /system/framework/framework.jar); rc=$?
if [ "$rc" -eq 0 ] && [ "${out%% *}" = "$want" ]; then
    ok "toybox sha256sum (libcrypto, FIPS self-test passed) = the Mac's shasum"
else bad "toybox sha256sum" "rc=$rc got '${out%% *}' want $want"; fi

# 3. mksh: arithmetic, loops, command substitution (fork), pipes.
out=$(g /system/bin/sh -c 'echo $((6*7)); x=$(echo sub); echo $x; toybox seq 1 100 | toybox wc -l'); rc=$?
if [ "$rc" -eq 0 ] && [ "$(tr '\n' ' ' <<<"$out")" = "42 sub 100 " ]; then
    ok "sh -c: arithmetic, \$(...) fork, a toybox pipe"
else bad "sh -c" "rc=$rc $(tr '\n' ' ' <<<"$out")"; fi

# 4. Properties: there is no property service (/dev/__properties__); bionic
# answers empty, and getprop exits 0.
out=$(g /system/bin/getprop ro.build.version.sdk); rc=$?
[ "$rc" -eq 0 ] && [ -z "$out" ] && ok "getprop without /dev/__properties__: empty, rc 0" \
    || bad "getprop" "rc=$rc '$out'"

# 5. ART's dex tooling (libdexfile), and ART itself, on a Hello.dex made on
# the Mac with D8 (R8_JAR: com.android.tools:r8 from Google Maven).
R8_JAR="${R8_JAR:-${STEAMARM_STATE:-$HOME/SteamARM-roots}/android/tools/r8-9.4.27.jar}"
if [ -f "$R8_JAR" ] && command -v javac >/dev/null; then
    mkdir -p build/android/classes build/android/dex
    if javac --release 8 -nowarn -d build/android/classes tests/android/java/Hello.java tests/android/java/Loop.java 2>/dev/null &&
       java -cp "$R8_JAR" com.android.tools.r8.D8 --min-api 30 --release --output build/android/dex \
            build/android/classes/Hello.class build/android/classes/Loop.class; then
        mkdir -p "$ROOT/data/local/tmp"
        cp build/android/dex/classes.dex "$ROOT/data/local/tmp/hello.dex"
        out=$(g /apex/com.android.art/bin/dexdump /data/local/tmp/hello.dex); rc=$?
        if [ "$rc" -eq 0 ] && grep -q "Class descriptor  : 'LHello;'" <<<"$out" &&
           grep -q "Class descriptor  : 'LLoop;'" <<<"$out"; then
            ok "dexdump (ART's libdexfile) reads the D8 output: LHello; and LLoop;"
        else bad "dexdump" "rc=$rc $(head -3 <<<"$out")"; fi

        # dalvikvm64 cannot start its heap: ART keeps object references in
        # 32 bits, so every heap space must be mapped below 4 GiB, and xnu
        # maps nothing there in an arm64 process (__PAGEZERO). Its log goes
        # to logd; tests/android/logd.py stands in for it.
        log=$(mktemp -t android-logd)
        python3 tests/android/logd.py "$ROOT" --seconds 60 --out "$log" &
        lpid=$!
        for _ in 1 2 3 4 5 6 7 8 9 10; do [ -S "$ROOT/dev/socket/logdw" ] && break; sleep 0.2; done
        out=$(DL=60 g /apex/com.android.art/bin/dalvikvm64 -cp /data/local/tmp/hello.dex Hello); rc=$?
        sleep 0.5
        kill "$lpid" 2>/dev/null; wait "$lpid" 2>/dev/null
        if grep -q "Hello from Java on ART" <<<"$out"; then
            ok "dalvikvm64 Hello: $out"
        elif grep -q "Could not find contiguous low-memory space" "$log"; then
            xfail "dalvikvm64 Hello (rc=$rc)" "$(grep -o 'Check failed: non_moving_space_mem_map.IsValid().*Out of memory' "$log" | head -1); ART's heap must be below 4 GiB (docs/ANDROID_RUNTIME_ARCHITECTURE.md)"
        else bad "dalvikvm64 Hello" "rc=$rc $(head -c 300 "$log")"; fi
        rm -f "$log"
    else bad "build hello.dex" "javac or D8 failed"; fi
else
    echo "  skip  ART checks (no $R8_JAR or no javac)"
fi

echo
summary="== $PASS passed, $FAIL failed"
[ "$XFAIL" -eq 0 ] || summary="$summary ($XFAIL expected failures)"
echo "$summary"
[ "$FAIL" -eq 0 ]
