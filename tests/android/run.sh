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
#                                   under FEX), LXRUN (default build/lxrun),
#                                   WESTON_ROOT (the display section,
#                                   scripts/mkwestonroot.sh), ANDROID_DISPLAY_SF
#                                   (0: no SurfaceFlinger in it)
#
# Writes only inside a root's /data/local/tmp and dev/socket (the logd
# stand-in), the Weston root's /tmp, and in private temporary directories for
# the binder hub, the property service (both leave by themselves when idle)
# and the Wayland socket. The display section shows Weston's window on the X
# server :2 for a few seconds when one answers there. Starts no daemon it
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
# dlrun SECONDS cmd...: run cmd in a process group of its own and stop the
# whole group at the deadline (TERM, then KILL 2 s later; status 142 as an
# alarm gives). A plain alarm stopped only the first process: a guest shell's
# child stayed, held the pipe open, and a hung x86_64 `sh -c $(toybox ...)`
# (the intermittent SIGCHLD-under-FEX hang) held the suite for 43 minutes.
# DLRUN_PL is the same, for callers that exec through env (a function cannot).
DLRUN_PL='my $t = shift; my $p = fork; die "fork: $!" unless defined $p;
    if (!$p) { setpgrp(0, 0); exec @ARGV or exit 127 }
    $SIG{ALRM} = sub { kill "TERM", -$p; sleep 2; kill "KILL", -$p; waitpid($p, 0); exit 142 };
    alarm $t; waitpid($p, 0); my $s = $?;
    exit($s & 127 ? 128 + ($s & 127) : $s >> 8)'
dlrun() { /usr/bin/perl -e "$DLRUN_PL" "$@"; }

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
            tests/android/java/Loop.java tests/android/java/HeapRef.java tests/android/java/NullCheck.java 2>/dev/null &&
       java -cp "$R8_JAR" com.android.tools.r8.D8 --min-api 30 --release --output build/android/dex \
            build/android/classes/Hello.class build/android/classes/Loop.class \
            build/android/classes/HeapRef.class build/android/classes/NullCheck.class &&
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
        /usr/bin/perl -e "$DLRUN_PL" "${DL:-30}" "$LXRUN" "$@" 2>/dev/null
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
        dlrun "$dl" scripts/run-android-x86.sh "$@" 2>&1 |
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
# A command substitution that runs a program: mksh blocks SIGCHLD around
# fork and waits for the child's SIGCHLD in rt_sigsuspend. Three ways it was
# lost, each once a hang until the deadline: FEX deferring a signal that lands
# in one of its own critical sections (patches/fex-lxrt-interrupt-page.patch),
# XNU leaving a SIGCHLD that came while the guest blocked it on the host main
# thread (runtime/signal.c, lxrt_signal_rescue_stranded), and FEX keeping one
# that came in its mask-update window in PendingSignals, which sigsuspend did
# not look at (patches/fex-lxrt-sigsuspend-pending.patch). Rare each, so
# ANDROID_SIG_RUNS runs (40): stage 27 measured ~1 in 40, stage 28 12 in 300
# (benchmarks/stage28-android-reliability.txt).
local runs="${ANDROID_SIG_RUNS:-40}" good=0 hung=0 other="" t0=$SECONDS
for ((i = 0; i < runs; i++)); do
    out=$(x 15 /system/bin/sh -c 'x=$(toybox echo x); y=$(toybox seq 3 | toybox wc -l); echo got $x $y'); rc=$?
    if [ "$rc" -eq 0 ] && [ "$out" = "got x 3" ]; then good=$((good+1))
    elif [ "$rc" -eq 142 ]; then hung=$((hung+1))
    else other="rc=$rc '$out'"; fi
done
if [ "$good" -eq "$runs" ]; then
    ok "x86_64 sh: \$(toybox ...) returns, $good of $runs runs ($((SECONDS - t0)) s; SIGCHLD during rt_sigsuspend)"
elif [ -z "$other" ] && ! grep -aq lxrt-sigsuspend-pending "$X86_ROOT/usr/lib/lxrt-emu/FEX"; then
    xfail "x86_64 sh: \$(toybox ...) returned $good of $runs times, $hung hung" \
          "the root's FEX predates patches/fex-lxrt-sigsuspend-pending.patch (scripts/build-fex-host.sh, scripts/mkandroidroot.sh --arch x86_64 --emu)"
else bad "x86_64 sh \$(toybox ...)" "$good of $runs returned, $hung hung until the 15 s deadline ${other}"; fi
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
    # ART's implicit null checks: the registers its SIGSEGV handler reads
    # (RIP, RSP, [RSP]) as the code left them, from a file, anonymous and
    # dual-mapped code and a second thread (benchmarks/stage28-android-apk.txt).
    if clang --target=x86_64-linux-gnu -O1 -ffreestanding -fno-stack-protector -nostdlib -static-pie -fPIE \
             -fuse-ld=lld -o "$X86_ROOT/data/local/tmp/x86_segv_ctx" tests/android/x86_segv_ctx.c 2>/dev/null; then
        out=$(x 60 /data/local/tmp/x86_segv_ctx); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== x86_segv_ctx: [0-9]* ok, 0 mal' <<<"$out"; then
            ok "x86_segv_ctx: a SIGSEGV handler sees si_addr, RIP, RSP and [RSP] as ART's fault handler needs ($(grep -o '[0-9]* ok' <<<"$out"))"
        else bad "x86_segv_ctx" "rc=$rc $(grep -E 'MAL|==' <<<"$out" | tr '\n' ' ')"; fi
    fi
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
    # Implicit null checks in JIT-compiled code: the fault becomes a
    # NullPointerException (process_vm_readv for ART's SafeCopy, stage 28).
    out=$(x 200 $dvm -Xjitthreshold:50 -cp $cp NullCheck 200000); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "NullCheck: sum 198000 caught 2000 of 2000" <<<"$out"; then
        ok "dalvikvm64 NullCheck: 2000 NullPointerExceptions from JIT-compiled field reads caught"
    else bad "dalvikvm64 NullCheck" "rc=$rc $(tr '\n' ' ' <<<"$out" | head -c 300)"; fi
    out=$(x 300 $dvm -Xjitinitialsize:64K -Xjitmaxsize:128K -Xjitthreshold:100 \
            -cp /data/local/tmp/churn.dex JitChurn 6 300); rc=$?
    if [ "$rc" -eq 0 ] && grep -qx "total=$ref_churn" <<<"$out"; then
        ok "dalvikvm64 JIT code cache churn (128 KiB cache, collected and reused): the Mac JVM's checksum"
    else bad "dalvikvm64 JIT code cache churn (stale translations?)" "rc=$rc want total=$ref_churn: $(tr '\n' ' ' <<<"$out" | head -c 300)"; fi
else
    echo "  skip  x86_64 ART checks (no $R8_JAR or no javac)"
fi
# 4. i386: the image's 32-bit programs under FEX's 32-bit mode (zygote_secondary,
# dex2oat32, dalvikvm32, the 32-bit HALs), which needed
# patches/fex-lxrt-i386-bionic.patch and runtime fixes
# (benchmarks/stage28-android-reliability.txt). A root whose FEX predates the
# patch (no "lxrt-i386-bionic" in it) makes these expected failures.
local i386_fex=0 i386_why="the root's FEX predates patches/fex-lxrt-i386-bionic.patch (scripts/build-fex-host.sh; scripts/mkandroidroot.sh --arch x86_64 --emu)"
# 32-bit bionic refuses to start when its main thread's tid is above 65535,
# and under lxrun that tid is the Mac's pid, which goes up to 99999: those
# runs are expected failures, named as such (tests/android/i386_rmutex.c,
# benchmarks/stage28-android-reliability.txt).
local i386_pid_why="the Mac gave the process a pid above 65535: 32-bit bionic keeps tids in 16 bits and refuses to start (lxrun's main-thread tid is the Mac's pid)"
pid_cap() { grep -q 'only accepts pid <= 65535' <<<"$1"; }
grep -aq lxrt-i386-bionic "$X86_ROOT/usr/lib/lxrt-emu/FEX" && i386_fex=1
local BLIBC32="$X86_ROOT/apex/com.android.runtime/lib/bionic/libc.so"
if [ -f "$BLIBC32" ] && /opt/homebrew/opt/llvm/bin/clang --target=i686-linux-android30 -O2 -fPIE -pie -nostdlib \
       -fno-stack-protector -fuse-ld=lld --ld-path=/opt/homebrew/opt/lld/bin/ld.lld \
       -Wl,--dynamic-linker=/system/bin/linker -Wl,-z,max-page-size=4096 \
       -o "$X86_ROOT/data/local/tmp/i386_bionic" tests/android/i386_bionic.c "$BLIBC32" 2>/dev/null; then
    out=$(x 60 /data/local/tmp/i386_bionic); rc=$?
    if [ "$rc" -eq 0 ] && grep -q '== i386_bionic: 8 ok, 0 mal' <<<"$out"; then
        ok "i386 bionic under FEX: AT_SYSINFO, CMP_REQUEUE, sigwait, SO_RCVTIMEO, SO_DOMAIN, SCM_RIGHTS, mmap hint, msync (8 ok)"
    elif [ "$i386_fex" = 0 ]; then
        xfail "i386 bionic under FEX (rc=$rc $(grep '^== ' <<<"$out"))" "$i386_why"
    elif pid_cap "$out"; then xfail "i386 bionic under FEX (rc=$rc)" "$i386_pid_why: $(grep -o 'current pid is [0-9]*' <<<"$out" | head -1)"
    else bad "i386 bionic under FEX" "rc=$rc $(grep -E 'MAL|^== |NoExec' <<<"$out" | tr '\n' ' ')"; fi
    rm -f "$X86_ROOT/data/local/tmp/i386_bionic"
    # A second thread's own recursive / error-checking mutex (32-bit bionic
    # keeps the owner's tid in 16 bits; lxrun's thread tids are 200000 +
    # pid * 10000 + n): expected to fail until the runtime gives 32-bit
    # bionic guests small tids (tests/android/i386_rmutex.c).
    if /opt/homebrew/opt/llvm/bin/clang --target=i686-linux-android30 -O2 -fPIE -pie -nostdlib \
           -fno-stack-protector -fuse-ld=lld --ld-path=/opt/homebrew/opt/lld/bin/ld.lld \
           -Wl,--dynamic-linker=/system/bin/linker -Wl,-z,max-page-size=4096 \
           -o "$X86_ROOT/data/local/tmp/i386_rmutex" tests/android/i386_rmutex.c "$BLIBC32" 2>/dev/null; then
        out=$(x 30 /data/local/tmp/i386_rmutex); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== i386_rmutex: ok' <<<"$out"; then
            ok "i386 bionic: a second thread relocks and unlocks its own recursive and error-checking mutexes"
        elif [ "$i386_fex" = 0 ]; then xfail "i386_rmutex (rc=$rc)" "$i386_why"
        elif pid_cap "$out"; then xfail "i386_rmutex (rc=$rc)" "$i386_pid_why"
        elif grep -q '== i386_rmutex: MAL' <<<"$out"; then
            xfail "i386 bionic: a second thread's own recursive mutex ($(grep '^thread tid' <<<"$out"))" \
                  "32-bit bionic keeps the owner's tid in 16 bits and lxrun's thread tids are above 200000: relock EBUSY, unlock EPERM, stdio from such a thread hangs"
        else bad "i386_rmutex" "rc=$rc $(tail -2 <<<"$out" | tr '\n' ' ')"; fi
        rm -f "$X86_ROOT/data/local/tmp/i386_rmutex"
    fi
else
    echo "  skip  i386 bionic probe (no llvm clang/lld or no 32-bit libc.so)"
fi
if [ "$HAVE_DEX" = 1 ]; then
    out=$(x 120 /apex/com.android.art/bin/dalvikvm32 -cp /data/local/tmp/hello.dex Hello); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "Hello from Java on ART, no VM" <<<"$out" && grep -q "os.arch=i686" <<<"$out"; then
        ok "dalvikvm32 (i386) Hello: $(grep -o 'java.vm.name=[^ ]* .*os.arch=[^ ]*' <<<"$out")"
    elif [ "$i386_fex" = 0 ]; then xfail "dalvikvm32 (i386) Hello (rc=$rc)" "$i386_why"
    elif pid_cap "$out"; then xfail "dalvikvm32 (i386) Hello (rc=$rc)" "$i386_pid_why: $(grep -o 'current pid is [0-9]*' <<<"$out" | head -1)"
    else bad "dalvikvm32 (i386) Hello" "rc=$rc $(head -c 300 <<<"$out")"; fi
    rm -rf "$X86_ROOT/data/local/tmp/oat/x86"
    mkdir -p "$X86_ROOT/data/local/tmp/oat/x86"
    out=$(x 120 /apex/com.android.art/bin/dex2oat32 --dex-file=/data/local/tmp/hello.dex \
            --oat-file=/data/local/tmp/oat/x86/hello.odex --instruction-set=x86 --compiler-filter=speed); rc=$?
    local odex="$X86_ROOT/data/local/tmp/oat/x86/hello.odex"
    if [ "$rc" -eq 0 ] && [ -s "$odex" ] && file -b "$odex" | grep -q 'ELF 32-bit.*80386'; then
        out=$(x 120 /apex/com.android.art/bin/dalvikvm32 -Xusejit:false -cp /data/local/tmp/hello.dex Hello); rc=$?
        if [ "$rc" -eq 0 ] && grep -q "Hello from Java on ART, no VM" <<<"$out"; then
            ok "dex2oat32 (i386) compiled hello.dex to a $(wc -c <"$odex" | tr -d ' ')-byte i386 odex, and dalvikvm32 runs it"
        else bad "dalvikvm32 with the dex2oat32 odex" "rc=$rc $(head -c 300 <<<"$out")"; fi
    elif [ "$i386_fex" = 0 ]; then xfail "dex2oat32 (i386) hello.dex (rc=$rc)" "$i386_why"
    elif pid_cap "$out"; then xfail "dex2oat32 (i386) hello.dex (rc=$rc)" "$i386_pid_why: $(grep -o 'current pid is [0-9]*' <<<"$out" | head -1)"
    else bad "dex2oat32 (i386) hello.dex" "rc=$rc odex $(wc -c <"$odex" 2>/dev/null) bytes $(tail -c 300 <<<"$out")"; fi
    rm -rf "$X86_ROOT/data/local/tmp/oat/x86"
fi
# 5. A /dev/input per stack (LXRT_INPUT_DIR, runtime/evdev.c): the composer's
# FIFO protocol between two x86-64 guests (tests/android/input_fifo.c), and a
# guest with another LXRT_INPUT_DIR that does not see the FIFO.
local BLIBC64I="$X86_ROOT/apex/com.android.runtime/lib64/bionic/libc.so"
if [ -f "$BLIBC64I" ] && /opt/homebrew/opt/llvm/bin/clang --target=x86_64-linux-android30 -O2 -fPIE -pie -nostdlib \
       -fno-stack-protector -fuse-ld=lld --ld-path=/opt/homebrew/opt/lld/bin/ld.lld \
       -Wl,--dynamic-linker=/system/bin/linker64 -Wl,-z,max-page-size=4096 \
       -o "$X86_ROOT/data/local/tmp/input_fifo" tests/android/input_fifo.c "$BLIBC64I" 2>/dev/null; then
    local ia ib ilog ipid wout other same
    ia=$(mktemp -d /tmp/lxrt-inA.XXXXXX); ib=$(mktemp -d /tmp/lxrt-inB.XXXXXX); ilog=$(mktemp -t input-fifo)
    (ANDROID_X86_ROOT="$X86_ROOT" LXRUN="$LXRUN" ANDROID_X86_GENV="LXRT_INPUT_DIR=$ia" \
        dlrun 60 scripts/run-android-x86.sh /data/local/tmp/input_fifo read wl_pointer_events 50 >"$ilog" 2>&1) &
    ipid=$!
    wout=$(ANDROID_X86_ROOT="$X86_ROOT" LXRUN="$LXRUN" ANDROID_X86_GENV="LXRT_INPUT_DIR=$ia" \
        dlrun 60 scripts/run-android-x86.sh /data/local/tmp/input_fifo write wl_pointer_events 50 2>&1 | grep -v '^\[lxrt')
    wait "$ipid"; rc=$?
    other=$(ANDROID_X86_ROOT="$X86_ROOT" LXRUN="$LXRUN" ANDROID_X86_GENV="LXRT_INPUT_DIR=$ib" \
        dlrun 30 scripts/run-android-x86.sh /data/local/tmp/input_fifo stat wl_pointer_events 2>/dev/null | grep -v '^\[lxrt')
    if [ "$rc" -eq 0 ] && grep -q '^reader: 150 records' "$ilog" && [ -p "$ia/wl_pointer_events" ] && [ "$other" = absent ]; then
        ok "LXRT_INPUT_DIR: a composer-style FIFO writer and an EventHub-style reader (x86-64, FEX) meet in their /dev/input; another stack's does not have it ($(sed -n 's/^reader: 150 records: //p' "$ilog"))"
    else bad "LXRT_INPUT_DIR FIFO" "rc=$rc other='$other' $(grep -v '^\[lxrt' "$ilog" | tail -2 | tr '\n' ' ') $wout"; fi
    rm -rf "$ia" "$ib" "$ilog" "$X86_ROOT/data/local/tmp/input_fifo"
fi
run_x86_64_services
[ "$had_server" = 1 ] || ANDROID_X86_ROOT="$X86_ROOT" scripts/run-android-x86.sh --server-stop
}

# x86-64 Android's services under FEX (benchmarks/stage27-android-framework.txt):
# the property service, binder with the image's service managers and an
# x86-64 native service, init-style sockets, and the boot of the framework
# (scripts/android-boot.py). Every x86 guest reaches the runtime's binder
# driver and property service through FEX's syscall passthrough. Private
# property and binder directories; daemons stopped by their own PIDs.
run_x86_64_services() {
local xp xb XENV out rc
xp=$(mktemp -d /tmp/lxrt-props-android-x86.XXXXXX)
xb=$(mktemp -d /tmp/lxrt-binder-android-x86.XXXXXX)
XENV="LXRT_PROPERTY_DIR=$xp LXRT_PROPERTY_IDLE=5 LXRT_PROPERTY_PERSIST=$xp/persistent_properties LXRT_BINDER_DIR=$xb LXRT_BINDER_HUB_IDLE=3"
xg() { ANDROID_X86_GENV="$XENV" x "$@"; }
# A daemon for `xgbg DEADLINE cmd... &`: the background subshell execs into
# perl, the script, env and lxrun, so $! is the daemon's own PID to stop.
xgbg() {
    local dl=$1; shift
    exec env ANDROID_X86_ROOT="$X86_ROOT" LXRUN="$LXRUN" ANDROID_X86_GENV="$XENV" \
        /usr/bin/perl -e 'alarm shift; exec @ARGV' "$dl" scripts/run-android-x86.sh "$@"
}
mkdir -p "$X86_ROOT/data/local/tmp"

# Properties: bionic x86-64 reads the areas and talks to property_service.
local sdk
sdk=$(sed -n 's/^ro.build.version.sdk=//p' "$X86_ROOT/system/build.prop")
out=$(xg 60 /system/bin/getprop ro.build.version.sdk); rc=$?
[ "$rc" -eq 0 ] && [ -n "$sdk" ] && [ "$out" = "$sdk" ] && ok "x86_64 getprop ro.build.version.sdk: $out (the property service, through FEX)" \
    || bad "x86_64 getprop" "rc=$rc '$out' want '$sdk'"
xg 60 /system/bin/setprop steamarm.x86.prop "hello from x86"; rc=$?
out=$(xg 60 /system/bin/getprop steamarm.x86.prop)
[ "$rc" -eq 0 ] && [ "$out" = "hello from x86" ] && ok "x86_64 setprop in one guest, getprop in another: [$out]" \
    || bad "x86_64 setprop/getprop" "rc=$rc '$out'"
local BLIBC64="$X86_ROOT/apex/com.android.runtime/lib64/bionic/libc.so"
if [ -f "$BLIBC64" ] && /opt/homebrew/opt/llvm/bin/clang --target=x86_64-linux-android30 -O2 -fPIE -pie -nostdlib \
       -fno-stack-protector -fuse-ld=lld --ld-path=/opt/homebrew/opt/lld/bin/ld.lld \
       -Wl,--dynamic-linker=/system/bin/linker64 -Wl,-z,max-page-size=4096 \
       -o "$X86_ROOT/data/local/tmp/props_wait" tests/android/props_wait.c "$BLIBC64" 2>/dev/null; then
    local wlog wpid
    wlog=$(mktemp -t props-wait-x86)
    (xgbg 60 /data/local/tmp/props_wait wait steamarm.x86.wait go 40 >"$wlog" 2>&1) &
    wpid=$!
    for _ in $(seq 1 150); do grep -q '^waiting' "$wlog" && break; sleep 0.1; done
    xg 30 /system/bin/setprop steamarm.x86.wait notyet >/dev/null
    xg 30 /system/bin/setprop steamarm.x86.wait go >/dev/null
    wait "$wpid"; rc=$?
    if [ "$rc" -eq 0 ] && grep -q '^woke: steamarm.x86.wait=go' "$wlog"; then
        ok "x86_64 __system_property_wait (FUTEX_WAIT under FEX, woken by the service): $(grep '^woke' "$wlog")"
    else bad "x86_64 __system_property_wait" "rc=$rc $(grep -v '^\[lxrt' "$wlog" | tr '\n' ' ' | head -c 200)"; fi
    rm -f "$wlog" "$X86_ROOT/data/local/tmp/props_wait"
else
    echo "  skip  x86_64 __system_property_wait (no llvm clang/lld or no x86_64 libc.so)"
fi

# Binder: the image's servicemanager, service, dumpsys; an x86-64 native
# service linked against the image's bionic (tests/android/bionic_min.h)
# that receives a file descriptor.
local smpid ready=""
(xgbg 120 /system/bin/servicemanager >/dev/null 2>&1) &
smpid=$!
for _ in $(seq 1 60); do
    xg 20 /system/bin/service check manager | grep -q found && { ready=1; break; }
    sleep 0.2
done
out=$(xg 30 /system/bin/service list); rc=$?
if [ -n "$ready" ] && grep -q '^Found 1 services' <<<"$out" && grep -q 'manager: \[android.os.IServiceManager\]' <<<"$out"; then
    ok "x86_64 servicemanager on /dev/binder; service list: $(grep -c ': \[' <<<"$out") service"
else bad "x86_64 servicemanager + service list" "ready=${ready:-no} rc=$rc $(head -3 <<<"$out" | tr '\n' ' ')"; fi
STAGE="${STEAMARM_BUILD:-$HOME/SteamARM-build}/rootstage-f43"
if [ -n "$ready" ] && [ -f "$STAGE/usr/include/linux/android/binder.h" ] && [ -f "$BLIBC64" ] &&
   /opt/homebrew/opt/llvm/bin/clang --target=x86_64-linux-android30 -DBIONIC_MIN -O2 -fPIE -pie -nostdlib \
       -fno-stack-protector -Itests/android -idirafter "$STAGE/usr/include" -fuse-ld=lld \
       --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -Wl,--dynamic-linker=/system/bin/linker64 \
       -Wl,-z,max-page-size=4096 -o "$X86_ROOT/data/local/tmp/binder_service" tests/android/binder_service.c \
       "$BLIBC64" 2>/dev/null; then
    local svclog svcpid want gone=""
    svclog=$(mktemp -t binder-service-x86)
    (xgbg 90 /data/local/tmp/binder_service >"$svclog" 2>&1) &
    svcpid=$!
    for _ in $(seq 1 100); do grep -q registered "$svclog" && break; sleep 0.1; done
    out=$(xg 30 /system/bin/service list)
    grep -q '^Found 2 services' <<<"$out" && grep -q 'steamarm.test: \[steamarm.test.IEcho\]' <<<"$out" &&
        ok "an x86-64 native service (bionic) registered; service list: steamarm.test: [steamarm.test.IEcho]" ||
        bad "x86_64 native service registration" "$(tr '\n' ' ' <<<"$out") / $(grep -v '^\[lxrt' "$svclog" | head -2)"
    out=$(xg 30 /system/bin/service call steamarm.test 1 i32 41)
    grep -q 'Result: Parcel(00000000 0000002a' <<<"$out" && ok "x86_64 service call steamarm.test 1 i32 41 -> 42" \
        || bad "x86_64 service call i32" "$out"
    want=$(python3 -c "import sys; d=open(sys.argv[1],'rb').read(); print('%08x %08x' % (len(d), sum(d)))" "$X86_ROOT/system/etc/hosts")
    out=$(xg 30 /system/bin/service call steamarm.test 2 fd /system/etc/hosts)
    grep -q "Result: Parcel(00000000 $want" <<<"$out" &&
        ok "x86_64 service call ... fd: the x86-64 service read the descriptor it was passed ($want)" ||
        bad "x86_64 service call fd" "want $want: $out"
    out=$(xg 30 /system/bin/dumpsys -l)
    grep -q '^  steamarm.test' <<<"$out" && ok "x86_64 dumpsys -l lists manager and steamarm.test" || bad "x86_64 dumpsys -l" "$(tr '\n' ' ' <<<"$out")"
    xg 30 /system/bin/service call steamarm.test 3 >/dev/null
    wait "$svcpid" 2>/dev/null
    for _ in $(seq 1 30); do
        out=$(xg 30 /system/bin/service list)
        grep -q '^Found 1 services' <<<"$out" && { gone=1; break; }
        sleep 0.2
    done
    [ -n "$gone" ] && ok "x86_64: the service exited; servicemanager's death notification removed it" \
        || bad "x86_64 death notification" "$(tr '\n' ' ' <<<"$out")"
    rm -f "$svclog" "$X86_ROOT/data/local/tmp/binder_service"
else
    echo "  skip  x86_64 native binder service (no $STAGE headers, no x86_64 libc.so, or servicemanager not ready)"
fi
# The same service built for i386 (32-bit bionic under FEX's 32-bit mode): its
# binder structures carry 32-bit addresses the runtime gives the guest base
# (runtime/binder.c, gp(); gbase.c for the ioctl argument), and the hub hands
# it guest addresses of its receive buffer.
local BLIBC32B="$X86_ROOT/apex/com.android.runtime/lib/bionic/libc.so" i386_ok=0
grep -aq lxrt-i386-bionic "$X86_ROOT/usr/lib/lxrt-emu/FEX" && i386_ok=1
if [ -n "$ready" ] && [ -f "$STAGE/usr/include/linux/android/binder.h" ] && [ -f "$BLIBC32B" ] &&
   /opt/homebrew/opt/llvm/bin/clang --target=i686-linux-android30 -DBIONIC_MIN -O2 -fPIE -pie -nostdlib \
       -fno-stack-protector -Itests/android -idirafter "$STAGE/usr/include" -fuse-ld=lld \
       --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -Wl,--dynamic-linker=/system/bin/linker \
       -Wl,-z,max-page-size=4096 -o "$X86_ROOT/data/local/tmp/binder_service32" tests/android/binder_service.c \
       "$BLIBC32B" 2>/dev/null; then
    local svclog32 svcpid32 got32=""
    svclog32=$(mktemp -t binder-service-i386)
    (xgbg 90 /data/local/tmp/binder_service32 >"$svclog32" 2>&1) &
    svcpid32=$!
    for _ in $(seq 1 100); do grep -q registered "$svclog32" && break; sleep 0.1; done
    out=$(xg 30 /system/bin/service call steamarm.test 1 i32 41)
    grep -q 'Result: Parcel(00000000 0000002a' <<<"$out" && got32=1
    want=$(python3 -c "import sys; d=open(sys.argv[1],'rb').read(); print('%08x %08x' % (len(d), sum(d)))" "$X86_ROOT/system/etc/hosts")
    local out2
    out2=$(xg 30 /system/bin/service call steamarm.test 2 fd /system/etc/hosts)
    xg 30 /system/bin/service call steamarm.test 3 >/dev/null
    # Bounded: perl's alarm reaches the guest, not lxrun, and an i386 bionic
    # that aborted on a pid above 65535 spun under FEX instead of dying
    # (MEASURED: 12 minutes at 99% CPU with the Mac's pids past 78000).
    local w
    for w in $(seq 1 150); do kill -0 "$svcpid32" 2>/dev/null || break; sleep 0.2; done
    kill -9 "$svcpid32" 2>/dev/null
    wait "$svcpid32" 2>/dev/null
    if [ -n "$got32" ] && grep -q "Result: Parcel(00000000 $want" <<<"$out2"; then
        ok "an i386 native service (32-bit bionic under FEX) registered and answered an x86-64 client: i32 41 -> 42, and read the descriptor it was passed ($want)"
    elif [ "$i386_ok" = 0 ]; then xfail "i386 native binder service" "the root's FEX predates patches/fex-lxrt-i386-bionic.patch"
    elif pid_cap "$(cat "$svclog32")"; then
        xfail "i386 native binder service" "32-bit bionic keeps pids in 16 bits and this Mac's pids are past 65535 ($(grep -o 'current pid is [0-9]*' "$svclog32" | head -1))"
    else bad "i386 native binder service" "i32: $(tr '\n' ' ' <<<"$out") fd: $(tr '\n' ' ' <<<"$out2") / $(grep -v '^\[lxrt' "$svclog32" | head -2 | tr '\n' ' ')"; fi
    rm -f "$svclog32" "$X86_ROOT/data/local/tmp/binder_service32"
fi
kill "$smpid" 2>/dev/null; wait "$smpid" 2>/dev/null

# vndservicemanager, the vendor context.
local vpid
(xgbg 60 /vendor/bin/vndservicemanager /dev/vndbinder >/dev/null 2>&1) &
vpid=$!
out=""
for _ in $(seq 1 60); do
    out=$(xg 20 /vendor/bin/vndservice list)
    grep -q '^Found 1 services' <<<"$out" && break
    sleep 0.2
done
grep -q 'manager: \[android.os.IServiceManager\]' <<<"$out" && ok "x86_64 vndservicemanager on /dev/vndbinder; vndservice list: manager" \
    || bad "x86_64 vndservicemanager" "$(head -3 <<<"$out" | tr '\n' ' ')"
kill "$vpid" 2>/dev/null; wait "$vpid" 2>/dev/null

# hwservicemanager, lshal, and an x86-64 HIDL HAL of the image.
local hpid apid hready=""
(xgbg 90 /system/bin/hwservicemanager >/dev/null 2>&1) &
hpid=$!
for _ in $(seq 1 60); do
    [ "$(xg 20 /system/bin/getprop hwservicemanager.ready)" = true ] && { hready=1; break; }
    sleep 0.2
done
[ -n "$hready" ] && ok "x86_64 hwservicemanager set hwservicemanager.ready=true" || bad "x86_64 hwservicemanager.ready" "not set"
out=$(xg 40 /system/bin/lshal list)
# lshal prints guest pids (small ids under LXRT_SMALL_IDS=1, runtime/ids.c).
local ghpid gapid
ghpid=$(python3 tests/android/guest_pid.py "$hpid")
grep -qE "android\.hidl\.manager@1\.0::IServiceManager/default +N/A +$ghpid" <<<"$out" &&
    ok "x86_64 lshal list: $(grep -cE "::I[A-Za-z]+/default +N/A +$ghpid" <<<"$out") interfaces served by hwservicemanager (pid $ghpid)" ||
    bad "x86_64 lshal list" "$(head -4 <<<"$out" | tr '\n' ' ')"
(xgbg 60 /system/bin/hw/android.hidl.allocator@1.0-service >/dev/null 2>&1) &
apid=$!
for _ in $(seq 1 30); do
    out=$(xg 40 /system/bin/lshal list)
    gapid=$(python3 tests/android/guest_pid.py "$apid")
    grep -qE "android\.hidl\.allocator@1\.0::IAllocator/ashmem +N/A +$gapid" <<<"$out" && break
    sleep 0.3
done
grep -qE "android\.hidl\.allocator@1\.0::IAllocator/ashmem +N/A +$gapid" <<<"$out" &&
    ok "an x86-64 HIDL HAL registered: android.hidl.allocator@1.0::IAllocator/ashmem (pid $gapid)" ||
    bad "x86_64 HIDL HAL registration" "$(grep -i allocator <<<"$out" | head -2)"
kill "$apid" "$hpid" 2>/dev/null; wait "$apid" "$hpid" 2>/dev/null

# An init-style socket (bound on the host, inherited as a descriptor, the
# way scripts/android-boot.py hands ANDROID_SOCKET_<name> to a service), as
# libcutils and the zygote check it; /proc/self/fd without the runtime's and
# FEX's descriptors; /proc/self/attr/current (tests/android/x86_initsock.c).
if clang --target=x86_64-linux-gnu -O1 -ffreestanding -fno-stack-protector -nostdlib -static-pie -fPIE \
         -fuse-ld=lld -o "$X86_ROOT/data/local/tmp/x86_initsock" tests/android/x86_initsock.c 2>/dev/null; then
    out=$(python3 - "$X86_ROOT" "$LXRUN" <<'EOF'
import os, socket, subprocess, sys
root, lxrun = sys.argv[1], sys.argv[2]
g = "/dev/socket/steamarm_initsock"
try:
    os.unlink(root + g)
except FileNotFoundError:
    pass
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.bind(root + g)                      # as Python binds: no NUL in the length
fd = s.detach()
os.set_inheritable(fd, True)
env = dict(os.environ, ANDROID_X86_ROOT=root, LXRUN=lxrun)
r = subprocess.run(["/usr/bin/perl", "-e", "alarm shift; exec @ARGV", "60", "scripts/run-android-x86.sh",
                    "/data/local/tmp/x86_initsock", str(fd), g], pass_fds=[fd], env=env,
                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
os.unlink(root + g)
print("\n".join(l for l in r.stdout.decode().splitlines() if not l.startswith("[lxrt]")))
print("rc=%d" % r.returncode)
EOF
)
    if grep -q '== x86_initsock: 4 ok, 0 mal' <<<"$out"; then
        ok "x86_initsock: an init socket's name (guest path, Linux's length), listen, /proc/self/fd without runtime/FEX descriptors, attr/current"
    else bad "x86_initsock" "$(grep -E 'MAL|stranger|==|rc=' <<<"$out" | tr '\n' ' ')"; fi
    rm -f "$X86_ROOT/data/local/tmp/x86_initsock"
else
    echo "  skip  x86_initsock (no clang for x86_64-linux-gnu)"
fi
# seccomp SECCOMP_RET_TRAP under FEX's emulation, as Chromium's sandbox (a
# WebView renderer) uses it: SIGSYS with Linux's fields (the runtime carries
# rt_tgsigqueueinfo's siginfo) and the trapped call not run
# (patches/fex-lxrt-seccomp-trap-skip.patch; tests/android/x86_seccomp_trap.c).
if clang --target=x86_64-linux-gnu -O1 -ffreestanding -fno-stack-protector -nostdlib -static-pie -fPIE \
         -fuse-ld=lld -o "$X86_ROOT/data/local/tmp/x86_seccomp_trap" tests/android/x86_seccomp_trap.c 2>/dev/null; then
    out=$(ANDROID_X86_ROOT="$X86_ROOT" LXRUN="$LXRUN" ANDROID_X86_GENV="FEX_NEEDSSECCOMP=1" \
          perl -e 'alarm shift; exec @ARGV' 60 scripts/run-android-x86.sh /data/local/tmp/x86_seccomp_trap 2>&1 | grep -v '^\[lxrt')
    if grep -q '== x86_seccomp_trap: PASS' <<<"$out"; then
        ok "seccomp RET_TRAP under FEX: $(grep -o 'SIGSYS with .*' <<<"$out")"
    else bad "seccomp RET_TRAP under FEX" "$(grep -E 'MAL|si_code|==' <<<"$out" | tr '\n' ' ')"; fi
    rm -f "$X86_ROOT/data/local/tmp/x86_seccomp_trap"
else
    echo "  skip  x86_seccomp_trap (no clang for x86_64-linux-gnu)"
fi
# exec from a process with a seccomp filter, as every app is: a script and an
# x86 program start (patches/fex-lxrt-execve-no-fd.patch;
# tests/android/x86_seccomp_exec.c).
if clang --target=x86_64-linux-gnu -O1 -ffreestanding -fno-stack-protector -nostdlib -static-pie -fPIE \
         -fuse-ld=lld -o "$X86_ROOT/data/local/tmp/x86_seccomp_exec" tests/android/x86_seccomp_exec.c 2>/dev/null; then
    out=$(ANDROID_X86_ROOT="$X86_ROOT" LXRUN="$LXRUN" ANDROID_X86_GENV="FEX_NEEDSSECCOMP=1" \
          perl -e 'alarm shift; exec @ARGV' 90 scripts/run-android-x86.sh /data/local/tmp/x86_seccomp_exec 2>&1 | grep -v '^\[lxrt')
    if grep -q '== x86_seccomp_exec: PASS' <<<"$out"; then
        ok "exec under a seccomp filter (FEX): a script and an x86 program start"
    else bad "exec under a seccomp filter (FEX)" "$(grep -E 'MAL|==|Invalid' <<<"$out" | tr '\n' ' ')"; fi
    rm -f "$X86_ROOT/data/local/tmp/x86_seccomp_exec"
else
    echo "  skip  x86_seccomp_exec (no clang for x86_64-linux-gnu)"
fi
# futex_waitv from x86-64 under FEX, the way Proton's fsync reaches it
# (runtime/futex_waitv.c; tests/android/x86_futex_waitv.c).
if clang --target=x86_64-linux-gnu -O1 -ffreestanding -fno-stack-protector -nostdlib -static-pie -fPIE \
         -fuse-ld=lld -o "$X86_ROOT/data/local/tmp/x86_futex_waitv" tests/android/x86_futex_waitv.c 2>/dev/null; then
    out=$(ANDROID_X86_ROOT="$X86_ROOT" LXRUN="$LXRUN" \
          perl -e 'alarm shift; exec @ARGV' 90 scripts/run-android-x86.sh /data/local/tmp/x86_futex_waitv 2>&1 | grep -v '^\[lxrt')
    if grep -q '== x86_futex_waitv: PASS' <<<"$out"; then
        ok "futex_waitv under FEX (Proton's fsync): probe, EAGAIN, timeout, a waiter in another process woken"
    else bad "futex_waitv under FEX" "$(grep -E 'MAL|==' <<<"$out" | tr '\n' ' ')"; fi
    rm -f "$X86_ROOT/data/local/tmp/x86_futex_waitv"
else
    echo "  skip  x86_futex_waitv (no clang for x86_64-linux-gnu)"
fi
for _ in $(seq 1 80); do [ -e "$xp/service.pid" ] || break; sleep 0.1; done
rm -rf "$xp" "$xb"

# The framework, headless (scripts/android-boot.py): init's boot actions, the
# service managers, the HALs of the profile, zygote64 and system_server under
# FEX with Android ids. It gets as far as SurfaceFlinger: the image's only
# composer is Waydroid's Wayland client and there is no display here.
if [ -n "${ANDROID_SKIP_BOOT:-}" ]; then
    echo "  skip  android-boot (ANDROID_SKIP_BOOT)"
    return 0
fi
local bst
bst=$(mktemp -d /tmp/lxrt-boot-test.XXXXXX)
python3 scripts/android-boot.py --root "$X86_ROOT" --lxrun "$LXRUN" --state "$bst" --seconds 75 \
    --persist "$bst/persistent_properties" >/dev/null 2>&1; rc=$?
local lc="$bst/logcat.txt" il="$bst/init.log"
if grep -q 'started zygote' "$il" && grep -aq 'Zygote: Accepting command socket connections' "$lc"; then
    ok "android-boot: init actions, service managers, HALs; zygote64 preloaded and listening ($(grep -ao 'preloaded [0-9]* classes in [0-9]*ms' "$lc" | head -1))"
else bad "android-boot: zygote" "$(tail -3 "$il" | tr '\n' ' ')"; fi
if grep -aq 'SystemServer: Entered the Android system server!' "$lc"; then
    ok "android-boot: system_server forked (Android ids, seccomp under FEX) and running: $(grep -a 'SystemServerTiming: Start' "$lc" | grep -vc took) bootstrap services started"
else bad "android-boot: system_server" "$(grep -aE 'FatalError|Fatal signal' "$lc" | head -2 | tr '\n' ' ')"; fi
# The secondary zygote (app_process32, i386) is started when the root's FEX
# can run it; system_server then does not wait 20 s for it.
if grep -aq lxrt-i386-bionic "$X86_ROOT/usr/lib/lxrt-emu/FEX"; then
    local zs
    zs=$(sed -n 's/.*started zygote_secondary (pid \([0-9]*\).*/\1/p' "$il" | head -1)
    # init.log has the host pid, logcat the guest's (runtime/ids.c), and
    # the process is gone by now: both zygotes' "Accepting" lines instead.
    if [ -n "$zs" ] && [ "$(grep -ac 'Zygote: Accepting command socket connections' "$lc")" -ge 2 ] &&
       ! grep -aq 'Failed to connect to Zygote through socket zygote_secondary' "$lc" &&
       ! grep -q 'service zygote_secondary .* exited' "$il"; then
        ok "android-boot: zygote_secondary (i386 app_process32) listening, system_server connected to it"
    else bad "android-boot: zygote_secondary" "$(grep zygote_secondary "$il" | tail -2 | tr '\n' ' ') $(grep -a 'zygote_secondary\|tid '"$zs"' .*[EF] ' "$lc" | head -2 | tr '\n' ' ')"; fi
fi
if grep -aq "Waiting for service 'SurfaceFlinger'" "$lc"; then
    xfail "android-boot: system_server past LightsService" "it waits for SurfaceFlinger, which waits for a composer (Waydroid's is a Wayland client; no display here)"
else bad "android-boot: the SurfaceFlinger wall moved" "$(grep -a 'SystemServerTiming' "$lc" | tail -2 | tr '\n' ' ')"; fi
rm -rf "$bst"
}

run_display() {
# The display path (docs/ANDROID_RUNTIME_ARCHITECTURE.md, "Display";
# benchmarks/stage27-android-display.txt): Weston (scripts/mkwestonroot.sh)
# under lxrun, a Wayland client in another process drawing into it through a
# memfd passed over the socket -- an aarch64 one and an x86-64 one under FEX
# (tests/android/wl_shm_client.c) -- headless first; then, when SteamARM's X
# server answers on :2, with the X11 backend (a macOS window for a few
# seconds) and the pixels read back with xwd; then Android's own
# SurfaceFlinger (x86_64 root, under FEX) presenting its boot animation
# through Waydroid's hwcomposer into it (scripts/run-android-display.sh;
# ANDROID_DISPLAY_SF=0 skips it). A private socket directory, binder hub and
# property state; everything started is stopped.
echo "== tests/android (display: Wayland under lxrun)"
local wroot="${WESTON_ROOT:-${STEAMARM_STATE:-$HOME/SteamARM-roots}/westonroot}"
if [ ! -x "$wroot/usr/bin/weston" ]; then
    echo "  skip  no Weston root at $wroot (scripts/mkwestonroot.sh)"
    return 0
fi
local llvm=/opt/homebrew/opt/llvm/bin/clang
local cflags="-O2 -ffreestanding -fno-stack-protector -fno-builtin -nostdlib -static-pie -fPIE -fuse-ld=lld"
# shellcheck disable=SC2086
if ! "$llvm" --target=aarch64-linux-gnu $cflags -o build/wl_shm_client.aarch64 tests/android/wl_shm_client.c 2>/dev/null ||
   ! "$llvm" --target=x86_64-linux-gnu $cflags -o build/wl_shm_client.x86_64 tests/android/wl_shm_client.c 2>/dev/null; then
    bad "build wl_shm_client" "llvm clang"
    return 0
fi
local have_x86=0 had_server=0
[ -x "$X86_ROOT/usr/lib/lxrt-emu/FEX" ] && have_x86=1
# The x86_64 root's FEXServer, if x86c() starts it, is stopped at the end.
[ -n "$(ANDROID_X86_ROOT="$X86_ROOT" scripts/run-android-x86.sh --server-pid)" ] && had_server=1
cp build/wl_shm_client.aarch64 "$wroot/tmp/wl_shm_client"
[ "$have_x86" = 1 ] && cp build/wl_shm_client.x86_64 "$X86_ROOT/data/local/tmp/wl_shm_client"
local xdg=/dev/shm/steamarm-wltest-$$ sock=wayland-0 out rc n
export WESTON_XDG=$xdg WESTON_SOCKET=$sock
wc_() {   # a guest of the Weston root, pointed at this Weston
    local dl=$1; shift
    dlrun "$dl" scripts/run-weston.sh client "$@" 2>&1 | grep --line-buffered -v '^\[lxrt\]'
    return "${PIPESTATUS[0]}"
}
x86c() {  # a guest of the x86_64 Android root under FEX, pointed at it
    local dl=$1; shift
    ANDROID_X86_ROOT="$X86_ROOT" LXRUN="$LXRUN" ANDROID_X86_GENV="XDG_RUNTIME_DIR=$xdg WAYLAND_DISPLAY=$sock" \
        dlrun "$dl" scripts/run-android-x86.sh "$@" 2>&1 | grep --line-buffered -v '^\[lxrt\]'
    return "${PIPESTATUS[0]}"
}

# 1. Headless: the protocol, and buffers shared between processes.
if out=$(scripts/run-weston.sh start --headless --kiosk 2>&1); then
    out=$(wc_ 30 /usr/bin/wayland-info)
    local need="wl_compositor wl_subcompositor wl_shm xdg_wm_base wl_output wp_presentation wp_viewporter"
    local miss="" i
    for i in $need; do grep -q "interface: '$i'" <<<"$out" || miss="$miss $i"; done
    if [ -z "$miss" ]; then
        ok "Weston (headless, pixman) under lxrun: wayland-info lists $(grep -c "interface:" <<<"$out") globals, among them the ones hwcomposer.waydroid needs ($need; wl_seat comes with the X11 backend)"
    else bad "wayland-info" "missing:$miss"; fi
    out=$(wc_ 60 /tmp/wl_shm_client -n 60); rc=$?
    if [ "$rc" -eq 0 ] && grep -q '== wl_shm_client: 60 frames' <<<"$out"; then
        ok "aarch64 wl_shm client, memfd buffers passed over the socket: $(sed -n 's/== wl_shm_client: //p' <<<"$out")"
    else bad "aarch64 wl_shm client" "rc=$rc $(tail -2 <<<"$out" | tr '\n' ' ')"; fi
    if [ "$have_x86" = 1 ]; then
        out=$(x86c 90 /data/local/tmp/wl_shm_client -n 60); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== wl_shm_client: 60 frames' <<<"$out"; then
            ok "x86-64 wl_shm client under FEX into the aarch64 Weston: $(sed -n 's/== wl_shm_client: //p' <<<"$out")"
        else bad "x86-64 wl_shm client under FEX" "rc=$rc $(tail -2 <<<"$out" | tr '\n' ' ')"; fi
    fi
    scripts/run-weston.sh stop >/dev/null
else bad "Weston headless" "$out"; fi

# 2. On the X server: the pixels, read back from Weston's X window.
local xdpy xwin
xdpy=$(command -v xdpyinfo || echo /opt/homebrew/bin/xdpyinfo)
xwin=$(command -v xwininfo || echo /opt/homebrew/bin/xwininfo)
if ! DISPLAY=:2 "$xdpy" >/dev/null 2>&1 || [ ! -x "$xwin" ]; then
    echo "  skip  Weston on X11 (no X server on :2: scripts/run-x11-native.sh start)"
else
    local before after id shot="$wroot/tmp/wltest-$$.xwd" clog cpid who=aarch64
    before=$(DISPLAY=:2 "$xwin" -root -tree | awk '/"Weston Compositor/ {print $1}' | sort)
    if out=$(scripts/run-weston.sh start 2>&1); then
        after=$(DISPLAY=:2 "$xwin" -root -tree | awk '/"Weston Compositor/ {print $1}' | sort)
        id=$(comm -13 <(echo "$before") <(echo "$after") | head -1)
        clog=$(mktemp -t wlclient)
        if [ "$have_x86" = 1 ]; then
            who=x86-64
            x86c 90 /data/local/tmp/wl_shm_client -n 60 -H 6000 >"$clog" &
        else
            wc_ 90 /tmp/wl_shm_client -n 60 -H 6000 >"$clog" &
        fi
        cpid=$!
        for n in $(seq 1 80); do grep -q '^connected' "$clog" && break; sleep 0.25; done
        sleep 2.5
        wc_ 30 /usr/bin/xwd -id "$id" -silent -out "/tmp/wltest-$$.xwd" >/dev/null
        out=$(python3 tests/android/xwd_colors.py "$shot" --expect 2080c0 c04020 20c040 e0e0e0 --min-frac 0.05); rc=$?
        wait "$cpid"
        if [ "$rc" -eq 0 ] && grep -q '== wl_shm_client: 60 frames' "$clog"; then
            ok "Weston's X window $id on :2 (a macOS window) shows the $who client's four colours:$(grep -o 'expect [0-9a-f]*: [0-9]* pixels' <<<"$out" | sed 's/expect //' | tr '\n' ' ')"
        else bad "Weston on X11, pixels" "window '$id' rc=$rc $(tr '\n' ' ' <<<"$out") $(tail -1 "$clog")"; fi
        rm -f "$shot" "$clog"
        # 3. SurfaceFlinger's boot animation, through hwcomposer.waydroid.
        if [ "$have_x86" = 1 ] && [ "${ANDROID_DISPLAY_SF:-1}" = 1 ]; then
            local ddir teal=0 f0 f1 t0 t1
            ddir=$(mktemp -d /tmp/lxrt-android-display.XXXXXX)
            out=$(ANDROID_X86_ROOT="$X86_ROOT" ANDROID_DISPLAY_DIR="$ddir" scripts/run-android-display.sh start 2>&1)
            for n in $(seq 1 40); do
                wc_ 30 /usr/bin/xwd -id "$id" -silent -out "/tmp/wltest-$$.xwd" >/dev/null
                python3 tests/android/xwd_colors.py "$shot" --expect 167c80 --min-frac 0.002 >/dev/null && { teal=1; break; }
                sleep 1
            done
            flips() {   # SurfaceFlinger's page-flip count (transaction 1013; AID_SYSTEM may ask)
                ANDROID_X86_ROOT="$X86_ROOT" LXRUN="$LXRUN" \
                ANDROID_X86_GENV="LXRT_BINDER_DIR=$ddir/binder LXRT_PROPERTY_DIR=$ddir/props LXRT_BINDER_UID=1000" \
                    perl -e 'alarm 30; exec @ARGV' scripts/run-android-x86.sh /system/bin/service call SurfaceFlinger 1013 2>/dev/null |
                    sed -n 's/.*Parcel(\([0-9a-f]*\) .*/\1/p'
            }
            f0=$(flips); t0=$(date +%s); sleep 5; f1=$(flips); t1=$(date +%s)
            if [ "$teal" = 1 ] && [ -n "$f0" ] && [ -n "$f1" ] && [ "$t1" -gt "$t0" ]; then
                ok "Android SurfaceFlinger (x86_64, FEX) presents the LineageOS boot animation in Weston's window: $(( (16#$f1 - 16#$f0) / (t1 - t0) )) page flips/s (SwiftShader GLES, gralloc default, wl_shm)"
            else bad "SurfaceFlinger boot animation" "teal=$teal flips '$f0' '$f1' $(tail -3 <<<"$out" | tr '\n' ' ')"; fi
            # 4. Input: hwcomposer.waydroid's FIFOs are in this stack's own
            # /dev/input (LXRT_INPUT_DIR=$ddir/input), and what it writes
            # there when the pointer moves over its window is read the way
            # Waydroid's EventHub reads it (tests/android/input_fifo.c; no
            # system_server here to run InputFlinger itself).
            local ifi="$X86_ROOT/data/local/tmp/input_fifo" ilog
            if [ -p "$ddir/input/wl_pointer_events" ] && [ -f /opt/homebrew/lib/libX11.dylib ] &&
               "$llvm" --target=x86_64-linux-android30 -O2 -fPIE -pie -nostdlib -fno-stack-protector -fuse-ld=lld \
                   --ld-path=/opt/homebrew/opt/lld/bin/ld.lld -Wl,--dynamic-linker=/system/bin/linker64 \
                   -Wl,-z,max-page-size=4096 -o "$ifi" tests/android/input_fifo.c \
                   "$X86_ROOT/apex/com.android.runtime/lib64/bionic/libc.so" 2>/dev/null; then
                ilog=$(mktemp -t input-fifo)
                (ANDROID_X86_ROOT="$X86_ROOT" LXRUN="$LXRUN" ANDROID_X86_GENV="LXRT_INPUT_DIR=$ddir/input" \
                    dlrun 45 scripts/run-android-x86.sh /data/local/tmp/input_fifo read wl_pointer_events 1000 >"$ilog" 2>&1) &
                local ipid=$!
                for n in $(seq 1 100); do grep -q '^reader: open' "$ilog" && break; sleep 0.1; done
                python3 tests/android/xsend_motion.py :2 "$id" 30 >/dev/null 2>&1
                wait "$ipid"
                local recs
                recs=$(sed -n 's/^reader: \([0-9]*\) records.*/\1/p' "$ilog")
                if [ "${recs:-0}" -ge 60 ] && grep -q ' EV_ABS' "$ilog" && ! grep -q ' 0 EV_ABS' "$ilog"; then
                    ok "input: 30 pointer motions into Weston's window came out of hwcomposer.waydroid's FIFO in this stack's own /dev/input: $(sed -n 's/^reader: //p' "$ilog" | tail -1)"
                else bad "input through the composer's FIFO" "$(grep -v '^\[lxrt' "$ilog" | tail -3 | tr '\n' ' ')"; fi
                rm -f "$ilog" "$ifi"
            else
                echo "  skip  input through the composer's FIFO (no FIFO in $ddir/input, no libX11 or no clang)"
            fi
            ANDROID_X86_ROOT="$X86_ROOT" ANDROID_DISPLAY_DIR="$ddir" scripts/run-android-display.sh stop >/dev/null
            sleep 2
            rm -f "$shot"
            rm -rf "$ddir"
        fi
        scripts/run-weston.sh stop >/dev/null
    else bad "Weston on X11" "$out"; fi
fi
rm -f "$wroot/tmp/wl_shm_client"
[ "$have_x86" = 1 ] && rm -f "$X86_ROOT/data/local/tmp/wl_shm_client"
[ "$have_x86" = 0 ] || [ "$had_server" = 1 ] || ANDROID_X86_ROOT="$X86_ROOT" scripts/run-android-x86.sh --server-stop
rm -rf "/tmp/lxrt-shm-$(id -u)/${xdg#/dev/shm/}"
unset WESTON_XDG WESTON_SOCKET
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
    # LXRT_BINDER_UID: the uid binder peers see (getCallingUid), as init's
    # `user` line gives a service; without it, the Mac's (runtime/binder.c).
    out=$(GENV="$BENV LXRT_BINDER_UID=1003" DL=20 g /system/bin/service call steamarm.test 1 i32 7)
    if grep -q 'Result: Parcel(00000000 00000008' <<<"$out" && grep -q 'call 1: 7 -> 8 (from pid [0-9]* uid 1003)' "$svclog" &&
       grep -q "call 1: 41 -> 42 (from pid [0-9]* uid $(id -u))" "$svclog"; then
        ok "LXRT_BINDER_UID=1003: the service sees sender_euid 1003 (the Mac's $(id -u) without it)"
    else bad "LXRT_BINDER_UID" "$out / $(grep 'call 1' "$svclog" | tr '\n' ' ')"; fi
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

run_session() {
# An APK end to end (scripts/android-session.py, benchmarks/stage28-android-
# apk.txt): Weston headless, the x86_64 root booted with its display profile to
# sys.boot_completed=1, the APK installed with the image's `pm install` and
# its launcher activity started with `am start`, its window focused. In a
# root of its own ($ANDROID_TEST_SESSION_ROOT, an APFS clone of the x86_64
# root made once and kept, so later runs boot warm) with a private state
# directory; the APK is Simple Solitaire Collection (F-Droid, dex only) from
# ~/SteamARM-roots/android/apk-samples, skipped when it is not there.
echo "== tests/android (an APK in the Android session)"
local apk="${ANDROID_TEST_APK:-${STEAMARM_STATE:-$HOME/SteamARM-roots}/android/apk-samples/de.tobiasbielefeld.solitaire_71.apk}"
local wroot="${WESTON_ROOT:-${STEAMARM_STATE:-$HOME/SteamARM-roots}/westonroot}"
if [ -n "${ANDROID_SKIP_SESSION:-}" ]; then echo "  skip  the Android session (ANDROID_SKIP_SESSION)"; return 0; fi
if [ ! -f "$apk" ] || [ ! -x "$wroot/usr/bin/weston" ] || [ ! -x "$X86_ROOT/usr/lib/lxrt-emu/FEX" ]; then
    echo "  skip  the Android session (needs $apk, a Weston root and the x86_64 root)"
    return 0
fi
local st sdir sroot="${ANDROID_TEST_SESSION_ROOT:-${X86_ROOT%/*}/sesstest}"
st=$(mktemp -d /tmp/lxrt-session-test.XXXXXX)
sdir="/tmp/lxrt-stest-$$"
local senv=(STEAMARM_STATE="$st" ANDROID_X86_ROOT="$X86_ROOT" ANDROID_SESSION_ROOT="$sroot"
            ANDROID_SESSION_DIR="$sdir" ANDROID_SESSION_XDG=/dev/shm/steamarm-android-test
            WESTON_ROOT="$wroot" LXRUN="$LXRUN" STEAMARM_NO_SAFEGUARD=1)
env "${senv[@]}" python3 scripts/android-pm.py install "$apk" >/dev/null 2>&1
local t0=$SECONDS out rc
out=$(env "${senv[@]}" perl -e 'alarm shift; exec @ARGV' 1500 python3 scripts/android-session.py launch de.tobiasbielefeld.solitaire --headless 2>&1); rc=$?
local boot inst start focus
boot=$(grep -o 'sys.boot_completed=1: [0-9.]* s' <<<"$out")
inst=$(grep -oE 'pm install: [A-Za-z_]+|is installed' <<<"$out" | tail -1)
start=$(grep -o 'am start -W .*: [a-z]*, TotalTime [0-9]* ms' <<<"$out" | sed 's/.*: //')
focus=$(grep -o 'focused window: [^ ]*' <<<"$out")
if [ -n "$boot" ]; then ok "Android session: $boot (display profile, headless Weston)"
else bad "Android session: boot" "rc=$rc $(tail -3 <<<"$out" | tr '\n' ' ')"; fi
if grep -qE 'Success|is installed' <<<"$inst"; then ok "pm install of an F-Droid APK (dex only): $inst"
else bad "pm install" "$(grep -E 'pm install' <<<"$out" | tail -2 | tr '\n' ' ')"; fi
if [ "$rc" -eq 0 ] && grep -q '^ok' <<<"$start" && grep -q 'de.tobiasbielefeld.solitaire/' <<<"$focus"; then
    ok "am start: $start; $focus ($((SECONDS - t0)) s in all)"
else bad "am start and focus" "rc=$rc start='$start' $(grep -E 'focus|am start' <<<"$out" | tail -2 | tr '\n' ' ')"; fi
# The network (scripts/android_dnsproxy.py, NetworkAgentStandIn.java): the
# Mac's network as Android's default one, validated by the network stack's
# own HTTP probes, and the image's curl resolving and fetching through it.
# Needs the Mac online: skipped when the Mac's own curl gets no 204.
local conn code n
for n in $(seq 1 15); do    # the agent registers after boot; the probes take seconds
    conn=$(env "${senv[@]}" python3 scripts/android-session.py shell /system/bin/dumpsys connectivity 2>/dev/null)
    grep -q VALIDATED <<<"$conn" && break
    sleep 2
done
if grep -q 'Active default network: [0-9]' <<<"$conn" && grep -q 'type: Ethernet.*state: CONNECTED' <<<"$conn"; then
    if [ "$(curl -s -m 10 -o /dev/null -w '%{http_code}' http://connectivitycheck.gstatic.com/generate_204)" = 204 ]; then
        code=$(env "${senv[@]}" python3 scripts/android-session.py shell --timeout 60 /system/bin/curl -s -m 20 -o /dev/null \
               -w '%{http_code}' https://www.google.com/generate_204 2>/dev/null)
        if grep -q 'VALIDATED' <<<"$conn" && [ "$code" = 204 ]; then
            ok "network: Android's default network is the Mac's (Ethernet, validated by the network stack), curl https in Android: $code"
        else bad "network through the Mac" "validated: $(grep -c VALIDATED <<<"$conn"), curl: '$code'"; fi
    else
        echo "  skip  network validation and curl (the Mac itself got no 204)"
    fi
else bad "network: a default network" "$(grep -E 'Active default network|NetworkAgentInfo' <<<"$conn" | head -2 | cut -c1-200 | tr '\n' ' ')"; fi
# Shared storage (scripts/android-boot.py storage_layout): /sdcard,
# /storage/emulated/0 and /storage/self/primary are one directory, the
# primary volume's /data/media/0, and no app's external directory was refused
# by StorageManagerService's canonical-path check ("Invalid mkdirs", which
# /storage as a symlink to /data/media gave every app, MEASURED).
local sto
sto=$(env "${senv[@]}" python3 scripts/android-session.py shell --timeout 60 /system/bin/sh -c \
      'echo lxrt-storage > /sdcard/Download/lxrt-storage.txt && cat /storage/emulated/0/Download/lxrt-storage.txt && cat /storage/self/primary/Download/lxrt-storage.txt' 2>/dev/null)
if [ "$(grep -c '^lxrt-storage' <<<"$sto")" = 2 ] && [ -f "$sroot/data/media/0/Download/lxrt-storage.txt" ] &&
   ! grep -aq 'Invalid mkdirs' "$sdir/logcat.txt" 2>/dev/null; then
    ok "shared storage: /sdcard, /storage/emulated/0 and /storage/self/primary are one directory (/data/media/0); no \"Invalid mkdirs\""
else bad "shared storage" "read back: $(tr '\n' ' ' <<<"$sto") $(grep -a -m1 'Invalid mkdirs' "$sdir/logcat.txt" 2>/dev/null | cut -c1-160)"; fi
rm -f "$sroot/data/media/0/Download/lxrt-storage.txt"
# Compressed sound decoded in Android: an .ogg of the image through
# MediaExtractor and MediaCodec, which pick the OMX store's vorbis decoder
# (i386 under FEX, started after the boot; tests/android/java/media).
local dd=build/android/decode dout
rm -rf "$dd"; mkdir -p "$dd/classes"
if [ -f "$R8_JAR" ] && javac --release 8 -nowarn -d "$dd/classes" -sourcepath tests/android/java/media/stubs \
       tests/android/java/media/Decode.java 2>/dev/null &&
   java -cp "$R8_JAR" com.android.tools.r8.D8 --min-api 30 --output "$dd" "$dd/classes/Decode.class" 2>/dev/null; then
    cp "$dd/classes.dex" "$sroot/data/local/tmp/decode.dex"
    for n in $(seq 1 30); do
        env "${senv[@]}" python3 scripts/android-session.py shell --timeout 60 /system/bin/lshal list -i 2>/dev/null |
            grep -q 'IOmxStore/default' && break
        sleep 2
    done
    dout=$(env "${senv[@]}" perl -e 'alarm shift; exec @ARGV' 200 python3 scripts/android-session.py shell --timeout 180 \
           /system/bin/env CLASSPATH=/data/local/tmp/decode.dex /system/bin/app_process64 /system/bin Decode \
           /system/product/media/audio/ui/Effect_Tick.ogg 2>&1 | grep -v '^\[lxrt')
    if grep -q '^codec OMX\.' <<<"$dout" && grep -qE '^decoded [1-9][0-9]* bytes' <<<"$dout"; then
        ok "compressed sound decoded in Android: $(grep '^codec' <<<"$dout" | cut -d' ' -f2) $(grep -o 'decoded .*' <<<"$dout")"
    else bad "compressed sound decoded in Android" "$(tail -3 <<<"$dout" | tr '\n' ' ')"; fi
    rm -f "$sroot/data/local/tmp/decode.dex"
else
    echo "  skip  compressed sound (no $R8_JAR or no javac)"
fi
env "${senv[@]}" python3 scripts/android-session.py stop >/dev/null 2>&1
local left
left=$(ps -axEww -o pid=,command= 2>/dev/null | grep -F "LXRT_PROPERTY_DIR=$sdir/props" | grep -v grep | wc -l | tr -d ' ')
# The session root's FEXServer too (android-boot.py starts it and must stop it).
[ -z "$(ANDROID_X86_ROOT="$sroot" scripts/run-android-x86.sh --server-pid)" ] || left=$((left + 1))
[ "$left" = 0 ] && ok "android-session.py stop: nothing of the session left" || bad "session stop" "$left processes left"
# Clicks and keys from the Mac, as a person's (XTEST on SteamARM's X server,
# scripts/run-x11-native.sh; tests/android/xtest_input.py): the same session
# with its window on :2 (a macOS window for about a minute). A click on the
# status bar opens the notification shade (focus: NotificationShade); Escape,
# which XQuartz now numbers as Linux does (patches/xquartz-evdev-keycodes.
# patch), reaches Android as BACK and closes it; A arrives as KEYCODE_A.
# (xdpyinfo's output taken whole: under pipefail, grep -q closing the pipe
# early made the check fail whenever XTEST was listed before the end.)
local xinfo
xinfo=$(DISPLAY=:2 /opt/homebrew/bin/xdpyinfo 2>/dev/null)
if [ -z "${ANDROID_SKIP_INPUT:-}" ] && grep -q XTEST <<<"$xinfo"; then
    local wid focus1 focus2 keys
    out=$(env "${senv[@]}" perl -e 'alarm shift; exec @ARGV' 900 python3 scripts/android-session.py launch de.tobiasbielefeld.solitaire 2>&1); rc=$?
    # Weston's window, by its class: its title is the app's name now.
    wid=$(DISPLAY=:2 /opt/homebrew/bin/xwininfo -root -tree 2>/dev/null | grep '("weston' | grep -o '0x[0-9a-f]*' | head -1)
    if [ "$rc" -eq 0 ] && [ -n "$wid" ]; then
        python3 tests/android/xtest_input.py :2 "$wid" click 8 8 >/dev/null
        sleep 2
        focus1=$(env "${senv[@]}" python3 scripts/android-session.py shell /system/bin/dumpsys window 2>/dev/null | grep -m1 mCurrentFocus)
        python3 tests/android/xtest_input.py :2 "$wid" key Escape >/dev/null
        sleep 2
        focus2=$(env "${senv[@]}" python3 scripts/android-session.py shell /system/bin/dumpsys window 2>/dev/null | grep -m1 mCurrentFocus)
        python3 tests/android/xtest_input.py :2 "$wid" key a >/dev/null
        sleep 1
        keys=$(env "${senv[@]}" python3 scripts/android-session.py shell /system/bin/dumpsys input 2>/dev/null |
               grep -c 'KeyEvent(.*action=DOWN.*keyCode=29, scanCode=30,')
        if grep -q NotificationShade <<<"$focus1" && grep -q de.tobiasbielefeld.solitaire <<<"$focus2" && [ "$keys" -ge 1 ]; then
            ok "input from the Mac window: a click opened the notification shade, Escape (BACK) closed it, A arrived as KEYCODE_A"
        else bad "input from the Mac window" "after click: '$focus1'; after Escape: '$focus2'; KEYCODE_A events: $keys"; fi
    else bad "input from the Mac window" "windowed session rc=$rc window '$wid' $(tail -2 <<<"$out" | tr '\n' ' ')"; fi
    env "${senv[@]}" python3 scripts/android-session.py stop >/dev/null 2>&1
else
    echo "  skip  input from the Mac window (no XTEST on :2, or ANDROID_SKIP_INPUT)"
fi
# One Mac window per app (steamarm-wlmac, Waydroid's multi-window mode; the
# launcher's default): the app's window alone, titled with its name; a click
# on a game's tile through the compositor's input path opens it; A arrives
# as KEYCODE_A. The compositor's test commands (SIGUSR1, <socket>.cmd) feed
# the same path as the Mac's mouse and keyboard. A macOS window for a while.
if [ -z "${ANDROID_SKIP_WLMAC:-}" ] && { [ -x build/steamarm-wlmac ] || tools/wlmac/build.sh >/dev/null 2>&1; }; then
    local wx=/dev/shm/steamarm-android-wltest wh wpid wlog wact wkeys
    wh="/tmp/lxrt-shm-$(id -u)${wx#/dev/shm}"
    # The clipboard on a private named pasteboard, never the Mac's own.
    local board=steamarm-android-clip-$$
    out=$(env "${senv[@]}" ANDROID_SESSION_XDG=$wx ANDROID_SESSION_COMPOSITOR=wlmac WLMAC_PASTEBOARD=$board \
          perl -e 'alarm shift; exec @ARGV' 900 \
          python3 scripts/android-session.py launch de.tobiasbielefeld.solitaire 2>&1); rc=$?
    wlog="$wh/wayland-0.wlmac.log"
    wpid=$(cat "$wh/wayland-0.wlmac-pid" 2>/dev/null)
    sleep 3
    if [ "$rc" -eq 0 ] && [ -n "$wpid" ] && grep -q 'window title=Simple Solitaire Collection' "$wlog"; then
        echo "click 94 468" > "$wh/wayland-0.cmd"; kill -USR1 "$wpid"; sleep 4
        wact=$(env "${senv[@]}" ANDROID_SESSION_XDG=$wx python3 scripts/android-session.py shell /system/bin/dumpsys activity activities 2>/dev/null |
               grep -m1 mResumedActivity)
        echo "key 0" > "$wh/wayland-0.cmd"; kill -USR1 "$wpid"; sleep 2
        wkeys=$(env "${senv[@]}" ANDROID_SESSION_XDG=$wx python3 scripts/android-session.py shell /system/bin/dumpsys input 2>/dev/null |
                grep -c 'KeyEvent(.*action=DOWN.*keyCode=29, scanCode=30,')
        if grep -q 'GameManager' <<<"$wact" && [ "$wkeys" -ge 1 ]; then
            ok "one Mac window per app (steamarm-wlmac): \"Simple Solitaire Collection\" alone; a click opened a game, A arrived as KEYCODE_A"
        else bad "one Mac window per app" "after the click: '$wact'; KEYCODE_A events: $wkeys"; fi
        # The clipboard both ways (tests/android/java/clip): text put on the
        # pasteboard is Android's primary clip once the app's window is key
        # (a test command makes it so); a clip set in Android reaches the
        # pasteboard. Through hwcomposer.waydroid's clipboard HAL and
        # steamarm-wlmac's wl_data_device.
        local cd=build/android/clip cget cmac
        rm -rf "$cd"; mkdir -p "$cd/classes"
        if [ -f "$R8_JAR" ] && javac --release 8 -nowarn -d "$cd/classes" tests/android/java/clip/Clip.java 2>/dev/null &&
           java -cp "$R8_JAR" com.android.tools.r8.D8 --min-api 30 --output "$cd" "$cd/classes/Clip.class" 2>/dev/null; then
            cp "$cd/classes.dex" "$sroot/data/local/tmp/clip.dex"
            clip() { env "${senv[@]}" ANDROID_SESSION_XDG=$wx perl -e 'alarm shift; exec @ARGV' 200 python3 scripts/android-session.py \
                     shell --timeout 180 /system/bin/env CLASSPATH=/data/local/tmp/clip.dex /system/bin/app_process64 /system/bin Clip "$@" \
                     2>&1 | grep -v '^\[lxrt'; }
            python3 tests/wlmac/pasteboard.py "$board" "del Mac: ñandú"
            echo "focus" > "$wh/wayland-0.cmd"; kill -USR1 "$wpid"; sleep 3
            cget=$(clip get | grep '^clip:')
            clip set "de Android: pingüino" >/dev/null
            cmac=""
            for n in 1 2 3 4 5 6 7 8 9 10; do
                cmac=$(python3 tests/wlmac/pasteboard.py "$board"); [ "$cmac" = "de Android: pingüino" ] && break; sleep 1
            done
            if [ "$cget" = "clip: del Mac: ñandú" ] && [ "$cmac" = "de Android: pingüino" ]; then
                ok "clipboard both ways: the Mac's text is Android's clip, Android's clip reaches the Mac (private pasteboard)"
            else bad "clipboard both ways" "Android got '$cget'; the pasteboard has '$cmac'"; fi
            rm -f "$sroot/data/local/tmp/clip.dex"
        else
            echo "  skip  clipboard (no $R8_JAR or no javac)"
        fi
        # WebView (Chromium's renderer: a separate, isolated process forked
        # from WebView's own zygote, under a seccomp sandbox): an app with a
        # WebView, built here with the Android SDK (tests/android/webview),
        # runs JavaScript, finishes its page and draws it.
        local wvapk
        if wvapk=$(tests/android/webview/build.sh 2>/dev/null); then
            env "${senv[@]}" ANDROID_SESSION_XDG=$wx ANDROID_SESSION_COMPOSITOR=wlmac WLMAC_PASTEBOARD=$board \
                perl -e 'alarm shift; exec @ARGV' 600 python3 scripts/android-session.py launch org.steamarm.webviewprobe \
                --apk "$wvapk" >/dev/null 2>&1
            local wvlog="" n
            for n in $(seq 1 30); do
                wvlog=$(grep -a 'WebViewProbe' "$sdir/logcat.txt" 2>/dev/null)
                grep -q 'drawn [1-9]' <<<"$wvlog" && break
                sleep 2
            done
            if grep -q 'js sum=500500' <<<"$wvlog" && grep -q 'page finished' <<<"$wvlog" && grep -q 'drawn [1-9]' <<<"$wvlog"; then
                ok "WebView: its renderer ran JavaScript (sum=500500), finished the page and drew it ($(grep -o 'drawn [0-9]* non-white pixels' <<<"$wvlog" | tail -1))"
            else bad "WebView" "$(tail -3 <<<"$wvlog" | tr '\n' ' ') $(grep -a 'crash detected' "$sdir/logcat.txt" 2>/dev/null | tail -1)"; fi
        else
            echo "  skip  WebView (no Android SDK platform 30 and build-tools)"
        fi
    else bad "one Mac window per app" "rc=$rc windows: $(grep 'window title' "$wlog" 2>/dev/null | tr '\n' ' ') $(tail -2 <<<"$out" | tr '\n' ' ')"; fi
    env "${senv[@]}" ANDROID_SESSION_XDG=$wx python3 scripts/android-session.py stop >/dev/null 2>&1
    rm -f "$wh/wayland-0.cmd"
fi
rm -rf "$st" "$sdir"
}

run_display
run_session

echo
summary="== $PASS passed, $FAIL failed"
[ "$XFAIL" -eq 0 ] || summary="$summary ($XFAIL expected failures)"
echo "$summary"
[ "$FAIL" -eq 0 ]
