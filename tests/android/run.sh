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
# The same, for a daemon started with `gbg ... &`: the background subshell
# execs into env, perl and lxrun, so $! is the daemon's own PID to stop.
gbg() {
    exec env -i HOME=/data/local/tmp PATH=/system/bin:/system/xbin TERM=dumb \
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

# 6. Binder (runtime/binder.c, runtime/binder_hub.c; benchmarks/stage25-
# binder.txt): Android's own servicemanager as context manager, Android's own
# `service` client, and a native service (tests/android/binder_service.c,
# raw ioctls, built with the Fedora glibc sysroot when present) registering
# with it, called with an int and a file descriptor, then dying. A private
# hub directory; the hub leaves 2 s after its last client; every daemon here
# is stopped by its own PID.
bdir=$(mktemp -d /tmp/lxrt-binder-android.XXXXXX)
BENV="LXRT_BINDER_DIR=$bdir LXRT_BINDER_HUB_IDLE=2"
GENV="$BENV" DL=90 gbg /system/bin/servicemanager >/dev/null &
smpid=$!
ready=""
for _ in $(seq 1 60); do
    GENV="$BENV" DL=10 g /system/bin/service check manager | grep -q 'found' && { ready=1; break; }
    sleep 0.1
done
out=$(GENV="$BENV" DL=20 g /system/bin/service list); rc=$?
if [ -n "$ready" ] && [ "$rc" -eq 0 ] && grep -q '^Found 1 services' <<<"$out" &&
   grep -q 'manager: \[android.os.IServiceManager\]' <<<"$out"; then
    ok "servicemanager on /dev/binder; service list: $(tr '\n\t' '  ' <<<"$out")"
else bad "servicemanager + service list" "ready=${ready:-no} rc=$rc $(head -3 <<<"$out")"; fi

STAGE="${STEAMARM_BUILD:-$HOME/SteamARM-build}/rootstage-f43"
GCCDIR=$(ls -d "$STAGE"/usr/lib/gcc/aarch64-redhat-linux/* 2>/dev/null | tail -1)
if [ -n "$ready" ] && [ -f "$STAGE/usr/include/linux/android/binder.h" ] && [ -n "$GCCDIR" ] &&
   /opt/homebrew/opt/llvm/bin/clang --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" \
       --gcc-install-dir="$GCCDIR" -fuse-ld=lld --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -w \
       -static-pie -O2 -o build/binder_service tests/android/binder_service.c 2>/dev/null; then
    mkdir -p "$ROOT/data/local/tmp"
    cp build/binder_service "$ROOT/data/local/tmp/binder_service"
    svclog=$(mktemp -t binder-service)
    GENV="$BENV" DL=60 gbg /data/local/tmp/binder_service >"$svclog" &
    svcpid=$!
    for _ in $(seq 1 50); do grep -q registered "$svclog" && break; sleep 0.1; done
    out=$(GENV="$BENV" DL=20 g /system/bin/service list)
    if grep -q '^Found 2 services' <<<"$out" && grep -q 'steamarm.test: \[steamarm.test.IEcho\]' <<<"$out"; then
        ok "a native service registered with servicemanager; service list: steamarm.test: [steamarm.test.IEcho]"
    else bad "native service registration" "$(tr '\n' ' ' <<<"$out") / $(head -2 "$svclog")"; fi
    out=$(GENV="$BENV" DL=20 g /system/bin/service call steamarm.test 1 i32 41)
    grep -q 'Result: Parcel(00000000 0000002a' <<<"$out" && ok "service call steamarm.test 1 i32 41 -> 42" \
        || bad "service call i32" "$out"
    want=$(python3 -c "import sys; d=open(sys.argv[1],'rb').read(); print('%08x %08x' % (len(d), sum(d)))" "$ROOT/system/etc/hosts")
    out=$(GENV="$BENV" DL=20 g /system/bin/service call steamarm.test 2 fd /system/etc/hosts)
    grep -q "Result: Parcel(00000000 $want" <<<"$out" && ok "service call ... fd /system/etc/hosts: the service read it through the passed descriptor ($want)" \
        || bad "service call fd" "want $want: $out"
    out=$(GENV="$BENV" DL=20 g /system/bin/service call steamarm.test 3)
    wait "$svcpid" 2>/dev/null
    gone=""
    for _ in $(seq 1 20); do
        out=$(GENV="$BENV" DL=20 g /system/bin/service list)
        grep -q '^Found 1 services' <<<"$out" && { gone=1; break; }
        sleep 0.1
    done
    [ -n "$gone" ] && ok "the service exited; servicemanager's death notification removed it" \
        || bad "death notification" "$(tr '\n' ' ' <<<"$out")"
    rm -f "$svclog" "$ROOT/data/local/tmp/binder_service"
else
    echo "  skip  native binder service (no $STAGE sysroot, or servicemanager not ready)"
fi
kill "$smpid" 2>/dev/null; wait "$smpid" 2>/dev/null

# vndservicemanager, the vendor context (/dev/vndbinder), and its client.
GENV="$BENV" DL=60 gbg /vendor/bin/vndservicemanager /dev/vndbinder >/dev/null &
vpid=$!
out=""
for _ in $(seq 1 50); do
    out=$(GENV="$BENV" DL=10 g /vendor/bin/vndservice list)
    grep -q '^Found 1 services' <<<"$out" && break
    sleep 0.1
done
grep -q 'manager: \[android.os.IServiceManager\]' <<<"$out" && ok "vndservicemanager on /dev/vndbinder; vndservice list: manager" \
    || bad "vndservicemanager + vndservice list" "$(head -3 <<<"$out")"
kill "$vpid" 2>/dev/null; wait "$vpid" 2>/dev/null
sleep 3
rm -rf "$bdir"

echo
summary="== $PASS passed, $FAIL failed"
[ "$XFAIL" -eq 0 ] || summary="$summary ($XFAIL expected failures)"
echo "$summary"
[ "$FAIL" -eq 0 ]
