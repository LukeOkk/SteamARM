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
# stand-in), and in private temporary directories for the binder hub and the
# property service (both leave by themselves when idle). Starts no daemon it
# does not stop; stops only its own PIDs.
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
# Every guest here shares one private property state (runtime/propsvc.c):
# the service is started by the first bionic process and leaves 3 s after
# its last setprop; persistent properties go to a file in the same
# directory instead of the root's /data/property.
pdir=$(mktemp -d /tmp/lxrt-props-android.XXXXXX)
PENV="LXRT_PROPERTY_DIR=$pdir LXRT_PROPERTY_IDLE=3 LXRT_PROPERTY_PERSIST=$pdir/persistent_properties"
g() {
    env -i HOME=/data/local/tmp PATH=/system/bin:/system/xbin TERM=dumb \
        ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_ART_ROOT=/apex/com.android.art \
        ANDROID_I18N_ROOT=/apex/com.android.i18n ANDROID_TZDATA_ROOT=/apex/com.android.tzdata \
        ANDROID_STORAGE=/storage BOOTCLASSPATH="$BCP" \
        LXRT_ROOT="$ROOT" LXRT_GUEST_PAGE=4096 $PENV ${GENV:-} \
        /usr/bin/perl -e 'alarm shift; exec @ARGV' "${DL:-30}" "$LXRUN" "$@" 2>/dev/null
}
# The same, for a daemon started with `gbg ... &`: the background subshell
# execs into env, perl and lxrun, so $! is the daemon's own PID to stop.
gbg() {
    exec env -i HOME=/data/local/tmp PATH=/system/bin:/system/xbin TERM=dumb \
        ANDROID_ROOT=/system ANDROID_DATA=/data ANDROID_ART_ROOT=/apex/com.android.art \
        ANDROID_I18N_ROOT=/apex/com.android.i18n ANDROID_TZDATA_ROOT=/apex/com.android.tzdata \
        ANDROID_STORAGE=/storage BOOTCLASSPATH="$BCP" \
        LXRT_ROOT="$ROOT" LXRT_GUEST_PAGE=4096 $PENV ${GENV:-} \
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

# 4. Properties (runtime/props.c, runtime/propsvc.c; benchmarks/stage26-
# android-properties.txt): /dev/__properties__ built from the image's own
# property_contexts and .prop files as init builds it, read by bionic itself;
# setprop through /dev/socket/property_service. Without the service (the
# switch off), bionic answers empty and getprop exits 0, as before.
out=$(GENV=LXRT_PROPERTY_SERVICE=0 g /system/bin/getprop ro.build.version.sdk); rc=$?
[ "$rc" -eq 0 ] && [ -z "$out" ] && ok "LXRT_PROPERTY_SERVICE=0: no /dev/__properties__, getprop empty, rc 0" \
    || bad "getprop without the property service" "rc=$rc '$out'"
sdk=$(sed -n 's/^ro.build.version.sdk=//p' "$ROOT/system/build.prop")
out=$(g /system/bin/getprop ro.build.version.sdk); rc=$?
[ "$rc" -eq 0 ] && [ -n "$sdk" ] && [ "$out" = "$sdk" ] && ok "getprop ro.build.version.sdk: $out (the image's /system/build.prop)" \
    || bad "getprop ro.build.version.sdk" "rc=$rc '$out', want '$sdk' ($(tail -3 "$pdir/service.log" 2>/dev/null))"
all=$(g /system/bin/getprop); rc=$?
n=0; miss=""
while IFS='=' read -r k v; do
    n=$((n+1))
    grep -qxF "[$k]: [$v]" <<<"$all" || miss="$miss $k"
done < <(grep '^ro\.build\.' "$ROOT/system/build.prop")
cnt=$(grep -c '^\[' <<<"$all")
if [ "$rc" -eq 0 ] && [ "$n" -gt 10 ] && [ -z "$miss" ]; then
    ok "getprop lists $cnt properties; all $n ro.build.* values of /system/build.prop are there"
else bad "getprop (list)" "rc=$rc, $cnt listed, missing:$miss"; fi
# The image sets no ro.build.fingerprint: init derives it from the image's
# own values (property_derive_build_fingerprint), nothing is made up.
fp=$(sed -n 's/^\[ro.build.fingerprint\]: \[\(.*\)\]$/\1/p' <<<"$all")
sysfp=$(sed -n 's/^ro.system.build.fingerprint=//p' "$ROOT/system/build.prop")
[ -n "$fp" ] && [ "$fp" = "$sysfp" ] && ok "ro.build.fingerprint derived as init does: $fp (= the image's ro.system.build.fingerprint)" \
    || bad "ro.build.fingerprint" "'$fp' vs ro.system.build.fingerprint '$sysfp'"

g /system/bin/setprop steamarm.test.prop "hello world"; rc=$?
out=$(g /system/bin/getprop steamarm.test.prop)
[ "$rc" -eq 0 ] && [ "$out" = "hello world" ] && ok "setprop in one guest, getprop in another: steamarm.test.prop=[$out]" \
    || bad "setprop/getprop" "rc=$rc '$out'"
# ro.* is written once (init's PropertySet): the image's values stay, and a
# new ro.* property takes its first value only.
g /system/bin/setprop ro.build.version.sdk 99; rc=$?
out=$(g /system/bin/getprop ro.build.version.sdk)
if [ "$rc" -ne 0 ] && [ "$out" = "$sdk" ] &&
   grep -q "Unable to set property 'ro.build.version.sdk'.*Read-only property was already set" "$pdir/service.log"; then
    ok "setprop ro.build.version.sdk 99 refused (PROP_ERROR_READ_ONLY_PROPERTY), still $out"
else bad "ro.* immutability" "rc=$rc '$out'"; fi
g /system/bin/setprop ro.steamarm.test first; r1=$?
g /system/bin/setprop ro.steamarm.test second; r2=$?
out=$(g /system/bin/getprop ro.steamarm.test)
[ "$r1" -eq 0 ] && [ "$r2" -ne 0 ] && [ "$out" = first ] && ok "a new ro.* property: first setprop taken, second refused" \
    || bad "new ro.* property" "rc $r1/$r2 '$out'"
# Types from property_contexts: service.adb.tcp.port is "exact int".
g /system/bin/setprop service.adb.tcp.port abc; r1=$?
g /system/bin/setprop service.adb.tcp.port 5555; r2=$?
out=$(g /system/bin/getprop service.adb.tcp.port)
[ "$r1" -ne 0 ] && [ "$r2" -eq 0 ] && [ "$out" = 5555 ] && ok "type check: service.adb.tcp.port (int) refuses 'abc', takes 5555" \
    || bad "type check" "rc $r1/$r2 '$out'"
# ctl.*: there is no init to start or stop a service.
g /system/bin/setprop ctl.start steamarm_nothing; rc=$?
[ "$rc" -ne 0 ] && grep -q "control message 'start' for 'steamarm_nothing'.*refused" "$pdir/service.log" &&
    ok "setprop ctl.start: refused (PROP_ERROR_HANDLE_CONTROL_MESSAGE) and logged" || bad "ctl.start" "rc=$rc"
# persist.*: stored in init's protobuf file, restored by a fresh boot of
# another property state that uses the same file.
g /system/bin/setprop persist.steamarm.test kept; rc=$?
stored=$(python3 - "$pdir/persistent_properties" <<'EOF' 2>/dev/null
import sys
d = open(sys.argv[1], 'rb').read()
def varint(b, i):
    v = s = 0
    while True:
        c = b[i]; i += 1; v |= (c & 0x7f) << s; s += 7
        if not c & 0x80: return v, i
i, out = 0, []
while i < len(d):
    k, i = varint(d, i); n, i = varint(d, i); rec = d[i:i + n]; i += n
    j, f = 0, {}
    while j < len(rec):
        k2, j = varint(rec, j); n2, j = varint(rec, j); f[k2 >> 3] = rec[j:j + n2].decode(); j += n2
    out.append('%s=%s' % (f.get(1, ''), f.get(2, '')))
print(' '.join(out))
EOF
)
pdir2=$(mktemp -d /tmp/lxrt-props-android.XXXXXX)
out=$(GENV="LXRT_PROPERTY_DIR=$pdir2" g /system/bin/getprop persist.steamarm.test)
ready=$(GENV="LXRT_PROPERTY_DIR=$pdir2" g /system/bin/getprop ro.persistent_properties.ready)
if [ "$rc" -eq 0 ] && [ "$stored" = "persist.steamarm.test=kept" ] && [ "$out" = kept ] && [ "$ready" = true ]; then
    ok "persist.steamarm.test: stored as init's PersistentProperties protobuf, restored by a fresh boot"
else bad "persist.*" "rc=$rc stored '$stored' fresh boot '$out' ready '$ready'"; fi

# __system_property_wait across processes: a bionic program linked against
# the image's own libc.so (tests/android/props_wait.c; no NDK needed) waits
# for a property that does not exist yet; two setprops from other guests
# create and change it; the service's futex wake reaches it.
CLANG=/opt/homebrew/opt/llvm/bin/clang
BLIBC="$ROOT/apex/com.android.runtime/lib64/bionic/libc.so"
if [ -x "$CLANG" ] && [ -x /opt/homebrew/opt/lld/bin/ld.lld ] && [ -f "$BLIBC" ] &&
   "$CLANG" --target=aarch64-linux-android30 -O2 -fPIE -pie -nostdlib -fno-stack-protector -ffixed-x18 \
       -fuse-ld=lld --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -Wl,--dynamic-linker=/system/bin/linker64 \
       -Wl,-z,max-page-size=4096 -o build/props_wait tests/android/props_wait.c "$BLIBC" 2>/dev/null; then
    mkdir -p "$ROOT/data/local/tmp"
    cp build/props_wait "$ROOT/data/local/tmp/props_wait"
    wlog=$(mktemp -t props-wait)
    DL=30 g /data/local/tmp/props_wait wait steamarm.test.wait go 20 >"$wlog" &
    wpid=$!
    for _ in $(seq 1 100); do grep -q '^waiting' "$wlog" && break; sleep 0.1; done
    g /system/bin/setprop steamarm.test.wait notyet
    g /system/bin/setprop steamarm.test.wait go
    wait "$wpid"; rc=$?
    if [ "$rc" -eq 0 ] && grep -q '^woke: steamarm.test.wait=go' "$wlog"; then
        ok "__system_property_wait in a third guest (linked against the image's libc): $(grep '^woke' "$wlog")"
    else bad "__system_property_wait" "rc=$rc $(tr '\n' ' ' <"$wlog")"; fi
    rm -f "$wlog" "$ROOT/data/local/tmp/props_wait"
else
    echo "  skip  __system_property_wait (no llvm clang/lld to link against the image's libc)"
fi

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
local had_server=0
if [ -n "$(ANDROID_X86_ROOT="$X86_ROOT" scripts/run-android-x86.sh --server-pid)" ]; then
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
    # dumpsys lists only when there is more than one service (dumpsys.cpp,
    # "if (N > 1)"), each checked with checkService.
    out=$(GENV="$BENV" DL=20 g /system/bin/dumpsys -l)
    grep -q '^  steamarm.test' <<<"$out" && grep -q '^  manager' <<<"$out" &&
        ok "dumpsys -l: $(tr '\n' ' ' <<<"$out")" || bad "dumpsys -l" "$(tr '\n' ' ' <<<"$out")"
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
# A real libbinder daemon: idmap2d publishes "idmap" (BinderService::publish)
# and serves from libbinder's own thread pool (startThreadPool +
# joinThreadPool); IIdmap2::getIdmapPath is transaction 1.
if [ -n "$ready" ]; then
    GENV="$BENV" DL=60 gbg /system/bin/idmap2d >/dev/null &
    idpid=$!
    for _ in $(seq 1 50); do
        GENV="$BENV" DL=10 g /system/bin/service check idmap | grep -q 'found' && break
        sleep 0.1
    done
    out=$(GENV="$BENV" DL=20 g /system/bin/service call idmap 1 s16 /product/overlay/Test.apk i32 0)
    flat=$(tr -d '\n' <<<"$out" | sed "s/'[^']*'//g")
    if grep -q '^Result: Parcel' <<<"$out" && grep -q '00000000 00000033' <<<"$flat"; then
        ok "idmap2d (libbinder's thread pool) registered and answered getIdmapPath: a 51-character path"
    else bad "idmap2d getIdmapPath" "$(head -3 <<<"$out")"; fi
    kill "$idpid" 2>/dev/null; wait "$idpid" 2>/dev/null
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

# hwservicemanager, the HIDL context (/dev/hwbinder): it announces itself
# with hwservicemanager.ready through the property service, and lshal
# (libhidl waits for that property first) lists what it serves. lshal's
# exit status stays 72 (DUMP_BINDERIZED_ERROR | IO_ERROR): its thread and
# client columns read /dev/binderfs/binder_logs/proc/<pid>, the binder
# driver's debug state, which the userspace driver does not publish.
GENV="$BENV" DL=60 gbg /system/bin/hwservicemanager >/dev/null &
hpid=$!
hready=""
for _ in $(seq 1 50); do
    [ "$(GENV="$BENV" DL=10 g /system/bin/getprop hwservicemanager.ready)" = true ] && { hready=1; break; }
    sleep 0.1
done
[ -n "$hready" ] && ok "hwservicemanager set hwservicemanager.ready=true through the property service" \
    || bad "hwservicemanager.ready" "$(grep hwservicemanager "$pdir/service.log" | tail -2)"
out=$(GENV="$BENV" DL=30 g /system/bin/lshal list); rc=$?
nif=$(grep -cE "::I[A-Za-z]+/default +N/A +$hpid" <<<"$out")
if grep -qE "android\.hidl\.manager@1\.0::IServiceManager/default +N/A +$hpid" <<<"$out"; then
    ok "lshal list (exit $rc): $nif interfaces served by hwservicemanager (pid $hpid), android.hidl.manager@1.0::IServiceManager among them"
else bad "lshal list" "rc=$rc $(head -4 <<<"$out" | tr '\n' ' ')"; fi
# A HIDL HAL of the image registers with it: the ashmem allocator
# (libhidl's registerAsService, after the same property wait).
GENV="$BENV" DL=60 gbg /system/bin/hw/android.hidl.allocator@1.0-service >/dev/null &
apid=$!
for _ in $(seq 1 50); do
    out=$(GENV="$BENV" DL=30 g /system/bin/lshal list)
    grep -qE "android\.hidl\.allocator@1\.0::IAllocator/ashmem +N/A +$apid" <<<"$out" && break
    sleep 0.2
done
grep -qE "android\.hidl\.allocator@1\.0::IAllocator/ashmem +N/A +$apid" <<<"$out" &&
    ok "a HIDL HAL registered with hwservicemanager: android.hidl.allocator@1.0::IAllocator/ashmem (pid $apid)" \
    || bad "HIDL HAL registration" "$(grep -i allocator <<<"$out" | head -2)"
kill "$apid" 2>/dev/null; wait "$apid" 2>/dev/null
kill "$hpid" 2>/dev/null; wait "$hpid" 2>/dev/null
sleep 3
rm -rf "$bdir"

# The property service leaves when idle; its areas stay, readable without
# it, and the next setprop starts it again, which adopts them as they are.
gone=""
for _ in $(seq 1 100); do [ -e "$pdir/service.pid" ] || { gone=1; break; }; sleep 0.1; done
out=$(g /system/bin/getprop steamarm.test.prop)
g /system/bin/setprop steamarm.test.prop again; rc=$?
out2=$(g /system/bin/getprop steamarm.test.prop)
if [ -n "$gone" ] && [ "$out" = "hello world" ] && [ "$rc" -eq 0 ] && [ "$out2" = again ] &&
   grep -q "adopted .*/${pdir##*/} in" "$pdir/service.log"; then
    ok "the service left when idle, the areas stayed readable ('$out'), setprop started it again (adopted): '$out2'"
else bad "idle and adopt" "gone=${gone:-no} '$out' rc=$rc '$out2' $(grep -c adopted "$pdir/service.log") adopted"; fi
for d in "$pdir" "$pdir2"; do
    for _ in $(seq 1 100); do [ -e "$d/service.pid" ] || break; sleep 0.1; done
done
[ -e "$pdir/service.pid" ] || [ -e "$pdir2/service.pid" ] && bad "property services still running" "$pdir $pdir2"
rm -rf "$pdir" "$pdir2"

echo
summary="== $PASS passed, $FAIL failed"
[ "$XFAIL" -eq 0 ] || summary="$summary ($XFAIL expected failures)"
echo "$summary"
[ "$FAIL" -eq 0 ]
