#!/bin/bash
# Android 11 userspace under lxrun, no VM (benchmarks/stage25-android-
# userspace.txt, benchmarks/stage25-art-x86-fex.txt,
# docs/ANDROID_RUNTIME_ARCHITECTURE.md). Runs bionic programs from the roots
# scripts/mkandroidroot.sh makes; each section skips cleanly without its root.
#
#   tests/android/run.sh            ANDROID_ROOT_DIR (default
#                                   /Volumes/SteamARMAndroid/root, arm64),
#                                   ANDROID_X86_ROOT (default
#                                   /Volumes/SteamARMAndroid/root-x86_64, run
#                                   under FEX), LXRUN (default build/lxrun)
#
# Writes only inside a root's /data/local/tmp and dev/socket (the logd
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

X86_ROOT="${ANDROID_X86_ROOT:-/Volumes/SteamARMAndroid/root-x86_64}"
[ -x "$LXRUN" ] || { echo "  FAIL  no $LXRUN (make lxrt)"; exit 1; }

# The test programs, built on the Mac: Hello, Loop and HeapRef (javac, then
# D8 from R8_JAR: com.android.tools:r8 from Google Maven) into
# build/android/dex/classes.dex, and a generated JitChurn into
# build/android/churn/classes.dex. HAVE_DEX=1 when both exist.
R8_JAR="${R8_JAR:-${STEAMARM_STATE:-$HOME/SteamARM-roots}/android/tools/r8-9.4.27.jar}"
HAVE_DEX=0
if [ -f "$R8_JAR" ] && command -v javac >/dev/null; then
    mkdir -p build/android/classes build/android/dex build/android/churn/classes
    if javac --release 8 -nowarn -d build/android/classes tests/android/java/Hello.java \
            tests/android/java/Loop.java tests/android/java/HeapRef.java 2>/dev/null &&
       java -cp "$R8_JAR" com.android.tools.r8.D8 --min-api 30 --release --output build/android/dex \
            build/android/classes/Hello.class build/android/classes/Loop.class \
            build/android/classes/HeapRef.class &&
       python3 tests/android/gen_jitchurn.py build/android/churn 400 &&
       javac --release 8 -nowarn -d build/android/churn/classes build/android/churn/JitChurn.java 2>/dev/null &&
       java -cp "$R8_JAR" com.android.tools.r8.D8 --min-api 30 --release --output build/android/churn \
            build/android/churn/classes/JitChurn.class; then
        HAVE_DEX=1
    else
        echo "  FAIL  build the test dex files (javac or D8 failed)"; FAIL=$((FAIL+1))
    fi
fi

run_arm64() {
echo "== tests/android (arm64, native)"
if [ ! -x "$ROOT/system/bin/toybox" ]; then
    echo "  skip  no Android root at $ROOT (scripts/mkandroidroot.sh)"
    return 0
fi

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
# the Mac with D8.
if [ "$HAVE_DEX" = 1 ]; then
    if true; then
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
    fi
else
    echo "  skip  ART checks (no $R8_JAR or no javac)"
fi
}

run_x86_64() {
# x86-64 Android (Waydroid's x86_64 build of the same LineageOS 18.1) under
# SteamARM's FEX, through scripts/run-android-x86.sh: FEX's 64-bit low window
# (FEX_LOWWINDOW=1) gives ART the address space below 4 GiB that macOS
# refuses a native arm64 process (benchmarks/stage25-art-x86-fex.txt).
echo "== tests/android (x86_64, under FEX)"
if [ ! -x "$X86_ROOT/system/bin/toybox" ] || [ ! -x "$X86_ROOT/usr/lib/lxrt-emu/FEX" ]; then
    echo "  skip  no x86_64 Android root with FEX at $X86_ROOT (scripts/mkandroidroot.sh --arch x86_64)"
    return 0
fi
# The FEXServer scripts/run-android-x86.sh starts is stopped at the end,
# unless one was already running.
local pidfile="$X86_ROOT/data/local/tmp/.fexserver.pid" had_server=0 p
if p=$(cat "$pidfile" 2>/dev/null) && ps -p "$p" -o command= 2>/dev/null | grep -q lxrt-emu/FEXServer; then
    had_server=1
fi
x() {   # x DEADLINE guest-command...: the guest's output; $? is its status
    local dl=$1; shift
    ANDROID_X86_ROOT="$X86_ROOT" LXRUN="$LXRUN" \
        /usr/bin/perl -e 'alarm shift; exec @ARGV' "$dl" scripts/run-android-x86.sh "$@" 2>&1 |
        grep -v -e '^\[lxrt\]' -e 'Abort trap'
    return "${PIPESTATUS[0]}"
}
local out rc want

# 1. bionic x86-64 programs: the runtime hands them to /usr/lib/lxrt-emu/FEX.
out=$(x 60 /system/bin/toybox ls /); rc=$?
if [ "$rc" -eq 0 ] && grep -qx system <<<"$out" && grep -qx apex <<<"$out"; then
    ok "x86_64 toybox ls / under FEX: $(wc -l <<<"$out" | tr -d ' ') entries"
else bad "x86_64 toybox ls /" "rc=$rc $(head -3 <<<"$out")"; fi
out=$(x 60 /system/bin/toybox uname -m); rc=$?
[ "$rc" -eq 0 ] && [ "$out" = x86_64 ] && ok "x86_64 toybox uname -m: $out" || bad "x86_64 uname -m" "rc=$rc $out"
out=$(x 60 /system/bin/sh -c 'echo $((6*7)); x=$(echo sub); echo $x; toybox seq 1 100 | toybox wc -l'); rc=$?
if [ "$rc" -eq 0 ] && [ "$(tr '\n' ' ' <<<"$out")" = "42 sub 100 " ]; then
    ok "x86_64 sh -c: arithmetic, \$(...) fork, a toybox pipe"
else bad "x86_64 sh -c" "rc=$rc $(tr '\n' ' ' <<<"$out")"; fi
# A command substitution that runs a program: mksh waits for the child's
# SIGCHLD in rt_sigsuspend, and FEX defers a signal that lands in one of its
# own critical sections (patches/fex-lxrt-interrupt-page.patch: without it
# this never returned).
out=$(x 30 /system/bin/sh -c 'x=$(toybox echo x); y=$(toybox seq 3 | toybox wc -l); echo got $x $y'); rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "got x 3" ]; then
    ok "x86_64 sh: \$(toybox ...) returns (SIGCHLD during rt_sigsuspend)"
else bad "x86_64 sh \$(toybox ...)" "rc=$rc '$out' (a lost SIGCHLD hangs here until the deadline)"; fi
want=$(shasum -a 256 "$X86_ROOT/system/framework/framework.jar" | awk '{print $1}')
out=$(x 60 /system/bin/toybox sha256sum /system/framework/framework.jar); rc=$?
if [ "$rc" -eq 0 ] && [ "${out%% *}" = "$want" ]; then
    ok "x86_64 toybox sha256sum (libcrypto) = the Mac's shasum"
else bad "x86_64 toybox sha256sum" "rc=$rc got '${out%% *}' want $want"; fi
out=$(x 60 /system/bin/toybox id); rc=$?
if [ "$rc" -eq 0 ] && grep -q "uid=$(id -u)(system_steamarm)" <<<"$out"; then
    ok "x86_64 toybox id: the root's account for the Mac's uid"
else bad "x86_64 toybox id" "rc=$rc $out"; fi

# 2. The runtime and FEX pieces ART needs, as freestanding x86-64 probes.
if command -v clang >/dev/null &&
   clang --target=x86_64-linux-gnu -O1 -ffreestanding -fno-stack-protector -nostdlib -static-pie -fPIE \
         -fuse-ld=lld -o "$X86_ROOT/data/local/tmp/x86_lowwin" tests/android/x86_lowwin.c 2>/dev/null &&
   clang --target=x86_64-linux-gnu -O1 -ffreestanding -fno-stack-protector -nostdlib -static-pie -fPIE \
         -fuse-ld=lld -o "$X86_ROOT/data/local/tmp/x86_dualview" tests/android/x86_dualview.c 2>/dev/null; then
    out=$(x 60 /data/local/tmp/x86_lowwin); rc=$?
    if [ "$rc" -eq 0 ] && grep -q '== x86_lowwin: 5 ok, 0 mal' <<<"$out"; then
        ok "x86_lowwin: MAP_32BIT below 4 GiB, MADV_DONTNEED zeroes there, a low hint honoured, startstack, RLIM_INFINITY"
    else bad "x86_lowwin" "rc=$rc $(grep -E 'MAL|==' <<<"$out" | tr '\n' ' ')"; fi
    out=$(x 60 /data/local/tmp/x86_dualview low toggle); rc=$?
    if [ "$rc" -eq 0 ] && grep -q ' 0 mal' <<<"$out"; then
        ok "x86_dualview low toggle: code rewritten in a dual-mapped memfd is never stale ($(grep -o '[0-9]* ok' <<<"$out"))"
    else bad "x86_dualview low toggle (stale translations: patches/fex-lxrt-smc-mprotect-mirrors.patch)" "rc=$rc $(grep '==' <<<"$out")"; fi
else
    echo "  skip  x86-64 probes (no clang for x86_64-linux-gnu)"
fi

# 3. ART: dalvikvm64 with the boot image, the heap below 4 GiB, the
# interpreter and the JIT against the Mac's JVM, and a JIT code cache that
# is collected and reused.
if [ "$HAVE_DEX" = 1 ]; then
    cp build/android/dex/classes.dex "$X86_ROOT/data/local/tmp/hello.dex"
    cp build/android/churn/classes.dex "$X86_ROOT/data/local/tmp/churn.dex"
    local dvm=/apex/com.android.art/bin/dalvikvm64 cp=/data/local/tmp/hello.dex
    out=$(x 120 $dvm -cp $cp Hello); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "Hello from Java on ART, no VM" <<<"$out" && grep -q "os.arch=x86_64" <<<"$out"; then
        ok "dalvikvm64 (x86_64) Hello: $(head -1 <<<"$out"); $(grep -o 'java.vm.name=[^ ]*' <<<"$out")"
    else bad "dalvikvm64 (x86_64) Hello" "rc=$rc $(head -c 300 <<<"$out")"; fi
    out=$(x 120 $dvm -cp $cp HeapRef); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "HEAP BELOW 4 GiB" <<<"$out"; then
        ok "dalvikvm64 HeapRef: $(grep -v HEAP <<<"$out" | tr '\n' ' ')"
    else bad "dalvikvm64 HeapRef" "rc=$rc $(tr '\n' ' ' <<<"$out" | head -c 300)"; fi
    # Reference values: the Mac's JVM when there is one, else the ones it
    # gave (benchmarks/stage25-art-x86-fex.txt).
    local ref_int=-6435145747647352456 ref_jit=6336216350770228723 ref_churn=-1445667666822057310 r
    if java -version >/dev/null 2>&1; then
        r=$(java -cp build/android/classes Loop 2000000 1 2>/dev/null | sed -n 's/.*result=\([-0-9]*\).*/\1/p' | head -1)
        [ -z "$r" ] || ref_int=$r
        r=$(java -cp build/android/classes Loop 20000000 1 2>/dev/null | sed -n 's/.*result=\([-0-9]*\).*/\1/p' | head -1)
        [ -z "$r" ] || ref_jit=$r
        r=$(java -cp build/android/churn/classes JitChurn 6 300 2>/dev/null | sed -n 's/^total=//p')
        [ -z "$r" ] || ref_churn=$r
    fi
    out=$(x 300 $dvm -Xint -cp $cp Loop 2000000 2); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "LOOP OK" <<<"$out" && grep -q "result=$ref_int " <<<"$out"; then
        ok "dalvikvm64 -Xint Loop: the Mac JVM's result, $(grep -o 'ms=[0-9.]*' <<<"$out" | tail -1) per round"
    else bad "dalvikvm64 -Xint Loop" "rc=$rc want $ref_int: $(tr '\n' ' ' <<<"$out" | head -c 300)"; fi
    out=$(x 300 $dvm -cp $cp Loop 20000000 5); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "LOOP OK" <<<"$out" && grep -q "result=$ref_jit " <<<"$out"; then
        ok "dalvikvm64 JIT Loop: the Mac JVM's result, $(grep -o 'ms=[0-9.]*' <<<"$out" | tail -1) per round after warm-up"
    else bad "dalvikvm64 JIT Loop" "rc=$rc want $ref_jit: $(tr '\n' ' ' <<<"$out" | head -c 300)"; fi
    out=$(x 300 $dvm -Xjitinitialsize:64K -Xjitmaxsize:128K -Xjitthreshold:100 \
            -cp /data/local/tmp/churn.dex JitChurn 6 300); rc=$?
    if [ "$rc" -eq 0 ] && grep -qx "total=$ref_churn" <<<"$out"; then
        ok "dalvikvm64 JIT code cache churn (128 KiB cache, collected and reused): the Mac JVM's checksum"
    else bad "dalvikvm64 JIT code cache churn (stale translations?)" "rc=$rc want total=$ref_churn: $(tr '\n' ' ' <<<"$out" | head -c 300)"; fi
else
    echo "  skip  x86_64 ART checks (no $R8_JAR or no javac)"
fi
[ "$had_server" = 1 ] || ANDROID_X86_ROOT="$X86_ROOT" scripts/run-android-x86.sh --server-stop
}

run_arm64
run_x86_64

echo
summary="== $PASS passed, $FAIL failed"
[ "$XFAIL" -eq 0 ] || summary="$summary ($XFAIL expected failures)"
echo "$summary"
[ "$FAIL" -eq 0 ]
