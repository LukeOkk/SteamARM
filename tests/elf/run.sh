#!/bin/bash
# MIGRATION_PLAN Stage 2 test suite: assert on capabilities, not screenshots.
set -uo pipefail
cd "$(dirname "$0")/../.."
# The guest root and the test programs: /tmp/lxrt-root and /tmp/lxrt-samples,
# or LXRT_ROOT / LXRT_SAMPLES (e.g. a root and samples from
# scripts/mkroot-rpm.sh). LXRT_ROOT is then unset: the tests that need a root
# pass it per command, the static ones run without one, as before.
GUEST_ROOT="${LXRT_ROOT:-${GUEST_ROOT:-/tmp/lxrt-root}}"
SAMPLES="${LXRT_SAMPLES:-/tmp/lxrt-samples}"
unset LXRT_ROOT

PASS=0; FAIL=0; XFAIL=0
ok()   { echo "  ok    $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL  $1"; echo "        $2"; FAIL=$((FAIL+1)); }
xfail() { echo "  xfail $1"; echo "        $2"; XFAIL=$((XFAIL+1)); }

# A dry-run's x18 line with every site rewritten; sets X18_FOUND.
x18_rewrites_ok() {
    [[ "$1" =~ x18\ ([0-9]+)\ found\ in\ .*\ ([0-9]+)\ rewritten,\ 0\ unsupported,\ 0\ unreachable ]] || return 1
    X18_FOUND=${BASH_REMATCH[1]}
    [ "$X18_FOUND" -eq "${BASH_REMATCH[2]}" ]
}
# macOS has no timeout(1). SIGALRM survives exec: exit status 142 when it fires.
deadline() { perl -e 'alarm shift; exec @ARGV' "$@"; }

CROSS_LD=/opt/homebrew/opt/lld/bin/ld.lld
build_guest() {
    clang -target aarch64-unknown-linux-gnu -nostdlib -static-pie -fPIE \
          -fuse-ld=$CROSS_LD -Wl,-e,_start -o "build/$1" "tests/elf/$1.S" 2>&1
}

echo "== tests/elf"

if err=$(build_guest x18_branch); then
    out=$(./build/lxrun --dry-run build/x18_branch 2>&1)
    if grep -q 'x18 6 found in .*6 rewritten, 0 unsupported, 0 unreachable' <<<"$out" &&
       ./build/lxrun build/x18_branch >/dev/null 2>&1; then
        ok "x18 indirect call, branch and return preserve guest control flow"
    else
        bad "x18 indirect branch execution" "$out"
    fi
else
    bad "build x18_branch" "$err"
fi

# The x18 planner on the host, no guest: decoder fields, every plan's words
# (tests/x18_check.c). Only run by hand until now.
if out=$(make -s build/x18_check 2>&1 && build/x18_check --self-test 2>&1); then
    ok "x18 decoder/planner self-test (host only)"
else
    bad "x18 decoder/planner self-test" "$(tail -5 <<<"$out")"
fi

# x18 loads and stores at sp offsets the immediate field cannot absorb
# (Steam's stp x18, x17, [sp, #0x1f8]): addressed through a copy of the
# original sp. Plain loads read back what the rewritten stores wrote.
if err=$(build_guest x18_spoff); then
    out=$(./build/lxrun --dry-run build/x18_spoff 2>&1)
    run=$(deadline 20 ./build/lxrun build/x18_spoff 2>&1); rc=$?
    if x18_rewrites_ok "$out" && [ "$rc" -eq 0 ]; then
        ok "x18 stp/ldp/str/ldur at large sp offsets: $X18_FOUND sites rewritten and executed"
    else
        bad "x18 at large sp offsets" "rc=$rc (exit code = failed check) $(grep x18 <<<"$out")"
    fi
else
    bad "build x18_spoff" "$err"
fi

# The .eh_frame FDE filter: a table in .text outside every FDE is data and
# must stay byte-for-byte (OpenSSL's round constants, "http error 0").
# LXRT_X18_ALL_TEXT turns the filter off for one file: the same two words
# are then rewritten and the program sees its table corrupted.
if err=$(build_guest x18_fde); then
    # A here-string, not a pipe: grep -q exiting early is SIGPIPE under pipefail.
    if ! grep -q '\.eh_frame' <<<"$(/opt/homebrew/opt/llvm/bin/llvm-readelf -S build/x18_fde)"; then
        bad "x18 FDE filter precondition" "build/x18_fde has no .eh_frame"
    else
        out=$(./build/lxrun --dry-run build/x18_fde 2>&1)
        deadline 20 ./build/lxrun build/x18_fde >/dev/null 2>&1; rc=$?
        if x18_rewrites_ok "$out" && [ "$X18_FOUND" -eq 2 ] && [ "$rc" -eq 0 ]; then
            ok "x18 FDE filter: 2 sites inside FDEs rewritten, the .text table outside them untouched"
        else
            bad "x18 FDE filter" "rc=$rc $(grep x18 <<<"$out")"
        fi
        out=$(LXRT_X18_ALL_TEXT=x18_fde ./build/lxrun --dry-run build/x18_fde 2>&1)
        LXRT_X18_ALL_TEXT=x18_fde deadline 20 ./build/lxrun build/x18_fde >/dev/null 2>&1; rc=$?
        if x18_rewrites_ok "$out" && [ "$X18_FOUND" -eq 4 ] && [ "$rc" -ne 0 ]; then
            ok "LXRT_X18_ALL_TEXT control: 4 sites, the table words rewritten too (rc=$rc)"
        else
            bad "LXRT_X18_ALL_TEXT control" "expected 4 sites and a failure, rc=$rc $(grep x18 <<<"$out")"
        fi
    fi
else
    bad "build x18_fde" "$err"
fi

# X18_CALL_RETURN: blr/br/ret x18 keep x17 and sp, blr exposes the guest
# return address, the virtual x18 keeps the target, nested calls unwind.
if err=$(build_guest x18_callret); then
    out=$(./build/lxrun --dry-run build/x18_callret 2>&1)
    deadline 20 ./build/lxrun build/x18_callret >/dev/null 2>&1; rc=$?
    if x18_rewrites_ok "$out" && [ "$rc" -eq 0 ]; then
        ok "X18_CALL_RETURN: nested blr x18, br x18, ret x18 ($X18_FOUND sites) keep x17, sp, x30 and x18"
    else
        bad "X18_CALL_RETURN" "rc=$rc (exit code = failed check) $(grep x18 <<<"$out")"
    fi
else
    bad "build x18_callret" "$err"
fi

# The forms refused (and poisoned) until now: ldar/stlr, mrs/msr of
# NZCV/FPCR/FPSR, and sp computed from x18 in both directions, misaligned
# included. A value within 16 bytes above the saved pair must trap instead.
if err=$(build_guest x18_forms); then
    out=$(./build/lxrun --dry-run build/x18_forms 2>&1)
    deadline 20 ./build/lxrun build/x18_forms >/dev/null 2>&1; rc=$?
    if x18_rewrites_ok "$out" && [ "$rc" -eq 0 ]; then
        ok "x18 ldar/stlr, mrs/msr nzcv/fpcr/fpsr and sp writes: $X18_FOUND sites rewritten and executed"
    else
        bad "x18 ldar/stlr, nzcv/fpcr/fpsr, sp writes" "rc=$rc (exit code = failed check) $(grep x18 <<<"$out")"
    fi
    LXRT_NO_X18=1 deadline 20 ./build/lxrun build/x18_forms >/dev/null 2>&1; rc=$?
    if [ "$rc" -ne 0 ]; then
        ok "without the x18 pass the same program fails (LXRT_NO_X18=1: rc=$rc)"
    else
        bad "LXRT_NO_X18 control for x18_forms" "expected a failure, got rc=0"
    fi
    run=$(deadline 20 ./build/lxrun build/x18_forms trap 2>&1); rc=$?
    if [ "$rc" -eq 133 ] && grep -q 'SIGTRAP at pc.*inside a trampoline pool' <<<"$run"; then
        ok "sp write within 16 bytes of the saved pair traps (brk #1) instead of corrupting"
    else
        bad "sp write trap" "rc=$rc $(grep SIG <<<"$run")"
    fi
else
    bad "build x18_forms" "$err"
fi

# A form the planner still refuses (an exclusive load: a trampoline inside
# an LL/SC sequence can clear the monitor forever) must trap. The poison was
# `svc #1` up to 0.3.4: with x16 = 20 it silently ran Darwin's getpid.
if err=$(build_guest x18_poison); then
    out=$(./build/lxrun --dry-run build/x18_poison 2>&1)
    run=$(deadline 20 ./build/lxrun build/x18_poison 2>&1); rc=$?
    if grep -q 'x18 1 found in .* 0 rewritten, 1 unsupported, 0 unreachable' <<<"$out" &&
       [ "$rc" -eq 133 ] && grep -q 'SIGTRAP at pc.*insn 0xd4200020' <<<"$run"; then
        ok "refused x18 site (ldaxr) is poisoned with brk #1 and traps (rc=133)"
    else
        bad "refused x18 site traps" "rc=$rc $(grep x18 <<<"$out") $(grep SIG <<<"$run")"
    fi
else
    bad "build x18_poison" "$err"
fi

# A 4 KiB-aligned image (Valve's native arm64 client) through elf.c's
# sub-page path: the end of the code and the start of the data share a
# 16 KiB host page, flipped RW/RX on faults. The writer loop runs from
# another host page. A store executed from the shared page itself never makes
# progress (MEASURED: endless "jit fault" pairs) -- kept as a known failure.
if err=$(clang -target aarch64-unknown-linux-gnu -nostdlib -static-pie -fPIE \
               -fuse-ld=$CROSS_LD -Wl,-e,_start -Wl,-z,max-page-size=4096 \
               -Wl,-z,common-page-size=4096 -Wl,-z,norelro \
               -o build/subpage4k tests/elf/subpage4k.S 2>&1); then
    headers=$(/opt/homebrew/opt/llvm/bin/llvm-readelf -lW build/subpage4k)
    aligns=( $(awk '$1 == "LOAD" { print $NF }' <<<"$headers") )
    read -r re_start re_size <<<"$(awk '$1 == "LOAD" && / R E / { print $3, $5; exit }' <<<"$headers")"
    rw_start=$(awk '$1 == "LOAD" && / RW / { print $3; exit }' <<<"$headers")
    subpage_ok=1
    for align in "${aligns[@]}"; do [ "$align" = 0x1000 ] || subpage_ok=0; done
    if [ "${#aligns[@]}" -eq 0 ] || [ -z "${re_start:-}" ] || [ -z "${rw_start:-}" ]; then
        subpage_ok=0
    elif [ $(((re_start + re_size) >> 14)) -ne $((rw_start >> 14)) ]; then
        subpage_ok=0
    fi
    if [ "$subpage_ok" -ne 1 ]; then
        bad "4 KiB ELF precondition: 4 KiB LOADs, R E end and RW start in one 16 KiB page" "$(grep LOAD <<<"$headers")"
    else
        out=$(./build/lxrun --dry-run build/subpage4k 2>&1)
        deadline 20 ./build/lxrun build/subpage4k >/dev/null 2>&1; rc=$?
        if [[ "$out" =~ svc\ ([0-9]+)\ found,\ ([0-9]+)\ rewritten,\ 0\ poisoned ]] &&
           [ "${BASH_REMATCH[1]}" -gt 0 ] && [ "${BASH_REMATCH[1]}" -eq "${BASH_REMATCH[2]}" ] &&
           [ "$rc" -eq 0 ]; then
            ok "4 KiB ELF via subpage: shared code/data host page flipped RW/RX 1000 times"
        else
            bad "4 KiB ELF via subpage" "rc=$rc (exit code = failed check) $(grep 'svc' <<<"$out")"
        fi
        deadline 5 ./build/lxrun build/subpage4k selfwrite >/dev/null 2>&1; rc=$?
        if [ "$rc" -eq 142 ]; then
            xfail "4 KiB ELF: a store executed from the host page it writes spins forever (W/X flip livelock)" "killed after 5 s"
        elif [ "$rc" -eq 0 ]; then
            ok "4 KiB ELF self-write (XPASS: the W/X livelock no longer reproduces)"
        else
            bad "4 KiB ELF self-write" "rc=$rc"
        fi
    fi
else
    bad "build subpage4k" "$err"
fi
if err=$(clang -target aarch64-unknown-linux-gnu -nostdlib -static-pie -fPIE \
               -fuse-ld=$CROSS_LD -Wl,-e,_start -Wl,-z,max-page-size=2048 \
               -Wl,-z,common-page-size=2048 -Wl,-z,norelro \
               -o build/subpage2k tests/elf/subpage4k.S 2>&1); then
    out=$(deadline 20 ./build/lxrun build/subpage2k 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && grep -q 'not a multiple of 4 KiB' <<<"$out"; then
        ok "2 KiB-aligned PT_LOAD refused with the reason"
    else
        bad "2 KiB-aligned PT_LOAD refused" "rc=$rc out='$out'"
    fi
else
    echo "  skip  2 KiB-aligned PT_LOAD (lld refuses it: $(tail -1 <<<"$err"))"
fi
rm -f build/subpage2k

# LARGE_EXECUTABLE_TRAMPOLINE_RANGE: `b` reaches +/-128 MiB, so each 32 MiB
# slice of a large code segment gets its own pool (libcef's is 162 MiB).
# Two svc sites and a TLS read 144 MiB apart. The image is ~144 MiB: removed.
if err=$(build_guest big_text); then
    out=$(./build/lxrun --dry-run build/big_text 2>&1)
    deadline 20 ./build/lxrun build/big_text >/dev/null 2>&1; rc=$?
    if [[ "$out" =~ svc\ ([0-9]+)\ found,\ ([0-9]+)\ rewritten,\ 0\ poisoned ]] &&
       [ "${BASH_REMATCH[1]}" -eq 4 ] && [ "${BASH_REMATCH[2]}" -eq 4 ] &&
       grep -q 'tls 1 reads + 0 writes, 1 rewritten, 0 poisoned' <<<"$out" && [ "$rc" -eq 0 ]; then
        ok "LARGE_EXECUTABLE_TRAMPOLINE_RANGE: svc/TLS sites 144 MiB apart rewritten and executed"
    else
        bad "LARGE_EXECUTABLE_TRAMPOLINE_RANGE" "rc=$rc $(grep 'svc' <<<"$out")"
    fi
else
    bad "build big_text" "$err"
fi
rm -f build/big_text

# 1. The toolchain can still emit a Linux aarch64 PIE from macOS.
if err=$(build_guest hello); then ok "build hello-linux"; else bad "build hello-linux" "$err"; fi
if file build/hello 2>/dev/null | grep -q "ELF 64-bit LSB.*ARM aarch64"; then
    ok "hello is an aarch64 Linux ELF"
else
    bad "hello is an aarch64 Linux ELF" "$(file build/hello 2>&1)"
fi

# 2. Load, rewrite, and report without running.
out=$(./build/lxrun --dry-run build/hello 2>&1)
if grep -q "svc 2 found, 2 rewritten, 0 poisoned" <<<"$out"; then
    ok "rewrite found and rewrote both svc sites"
else
    bad "rewrite found and rewrote both svc sites" "$out"
fi

# 3. The whole path: a Linux binary produces Linux output, with no VM.
out=$(./build/lxrun build/hello 2>/dev/null); rc=$?
if [ "$rc" -eq 0 ] && [ "$out" = "hello from a Linux ELF, no VM" ]; then
    ok "hello runs and writes to stdout (rc=$rc)"
else
    bad "hello runs and writes to stdout" "rc=$rc out='$out'"
fi

# 4. A non-PIE image must be refused with the real reason, not a crash.
clang -target aarch64-unknown-linux-gnu -nostdlib -static \
      -fuse-ld=$CROSS_LD -Wl,-e,_start -o build/hello-nopie tests/elf/hello.S 2>/dev/null
out=$(./build/lxrun --dry-run build/hello-nopie 2>&1); rc=$?
if [ "$rc" -ne 0 ] && grep -q "__PAGEZERO" <<<"$out"; then
    ok "non-PIE image refused with the __PAGEZERO reason"
else
    bad "non-PIE image refused with the __PAGEZERO reason" "rc=$rc out='$out'"
fi

# 5. The hot path: clock_gettime through the rewritten trampoline.
# Timed inside one python process -- two separate `perf_counter_ns` calls from
# two shells do not measure what they look like they measure.
if err=$(build_guest clockloop); then
    res=$(python3 - <<'PYEOF'
import subprocess, time
ITERS = 1024000
# One empty run first, to charge process start-up to the baseline rather than
# to the syscalls.
def run(argv):
    t0 = time.perf_counter_ns()
    rc = subprocess.run(argv, stdout=subprocess.DEVNULL,
                        stderr=subprocess.DEVNULL).returncode
    return rc, time.perf_counter_ns() - t0

rc_base, t_base = min((run(["./build/lxrun", "build/hello"]) for _ in range(5)),
                      key=lambda r: r[1])
rc_loop, t_loop = min((run(["./build/lxrun", "build/clockloop"]) for _ in range(5)),
                      key=lambda r: r[1])
if rc_base or rc_loop:
    print(f"ERR rc_base={rc_base} rc_loop={rc_loop}")
else:
    print(f"{(t_loop - t_base) / ITERS:.1f} {t_base/1e6:.1f}")
PYEOF
)
    if [[ "$res" == ERR* ]]; then
        bad "clockloop runs" "$res"
    else
        set -- $res
        ok "clockloop: $1 ns per clock_gettime (start-up $2 ms subtracted)"
    fi
else
    bad "build clockloop" "$err"
fi

# 6. A real glibc binary, if a sample is available (scripts/mkroot-rpm.sh
# <root> <samples> builds them on the host).
GLIBC_SAMPLE=$SAMPLES/hello_glibc
if [ -f "$GLIBC_SAMPLE" ]; then
    out=$(./build/lxrun "$GLIBC_SAMPLE" 2>/dev/null); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "glibc static-pie alive" <<<"$out" \
                       && grep -q "malloc+stdio+snprintf ok" <<<"$out"; then
        ok "real glibc static-PIE binary runs (malloc, stdio, clock_gettime)"
    else
        bad "real glibc static-PIE binary runs" "rc=$rc out='$out'"
    fi
    # Everything glibc needs must be implemented except rseq, which glibc is
    # designed to fall back from.
    miss=$(./build/lxrun --trace "$GLIBC_SAMPLE" 2>&1 \
           | grep -o "unimplemented syscall [0-9]*" | awk '{print $3}' | sort -u | tr '\n' ' ')
    if [ "$(echo $miss)" = "293" ] || [ -z "$(echo $miss)" ]; then
        ok "glibc start-up needs nothing unimplemented but rseq (${miss:-none})"
    else
        bad "glibc start-up needs nothing unimplemented but rseq" "missing: $miss"
    fi
else
    echo "  skip  real glibc test (no $GLIBC_SAMPLE)"
fi

# 7. Dynamic linking: PT_INTERP, the guest's own ld.so, and libraries it maps
# itself -- whose code arrives full of unrewritten `svc`.
DYN_SAMPLE=$SAMPLES/hello_dyn
if [ -f "$DYN_SAMPLE" ] && [ -d "$GUEST_ROOT" ]; then
    out=$(LXRT_ROOT=$GUEST_ROOT ./build/lxrun "$DYN_SAMPLE" 2>&1); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "malloc+stdio+snprintf ok" <<<"$out"; then
        ok "dynamic binary runs through the guest ld.so"
    else
        bad "dynamic binary runs through the guest ld.so" "rc=$rc out='$out'"
    fi

    tr_out=$(LXRT_ROOT=$GUEST_ROOT ./build/lxrun --trace "$DYN_SAMPLE" 2>&1)
    if grep -q "mapped code at .*415 svc sites, 415 rewritten, 0 poisoned" <<<"$tr_out"; then
        ok "libc mapped by ld.so had all 415 svc sites rewritten in flight"
    else
        bad "libc mapped by ld.so had all svc sites rewritten" \
            "$(grep 'mapped code' <<<"$tr_out" || echo 'no rewrite of mapped code happened')"
    fi

    # A trampoline pool landing inside a span the loader reserved gets mapped
    # over, and the symptom is corruption a long way from the cause.
    if grep -q "WARNING" <<<"$tr_out"; then
        bad "no trampoline pool was mapped over" "$(grep WARNING <<<"$tr_out" | head -2)"
    else
        ok "no trampoline pool was mapped over by the guest loader"
    fi
else
    echo "  skip  dynamic-linking tests (run scripts/mkroot-rpm.sh first)"
fi

# 8. Guest TLS must survive a context switch. Darwin clobbers TPIDR_EL0 on any
# deschedule, so the runtime rewrites every access to a Darwin TSD slot. Both
# directions are run: the failure this prevents is demonstrated, not asserted.
TLS_SAMPLE=$SAMPLES/tls_test
if [ -f "$TLS_SAMPLE" ]; then
    out=$(./build/lxrun "$TLS_SAMPLE" 2>&1); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "PRESERVADO" <<<"$out"; then
        ok "guest TLS survives 300 context switches"
    else
        bad "guest TLS survives 300 context switches" "rc=$rc out='$out'"
    fi

    out=$(LXRT_NO_TLS_REWRITE=1 ./build/lxrun "$TLS_SAMPLE" 2>&1); rc=$?
    if [ "$rc" -ne 0 ] && ! grep -q "PRESERVADO" <<<"$out"; then
        ok "without rewriting the same binary loses its TLS (rc=$rc)"
    else
        bad "without rewriting the same binary loses its TLS" \
            "expected a failure, got rc=$rc out='$out'"
    fi

    n=$(./build/lxrun --dry-run "$TLS_SAMPLE" 2>&1 | grep -oE "tls [0-9]+ reads" | awk '{print $2}')
    if [ "${n:-0}" -gt 0 ]; then
        ok "TLS rewriting found $n reads in the image"
    else
        bad "TLS rewriting found reads in the image" "found ${n:-none}"
    fi
else
    echo "  skip  TLS tests (run scripts/mkroot-rpm.sh first)"
fi

# 9. Threads: clone3, futex, per-thread TLS, and the CLONE_CHILD_CLEARTID wake
# that pthread_join blocks on.
PTH_SAMPLE=$SAMPLES/pth_test
if [ -f "$PTH_SAMPLE" ]; then
    fails=0
    for _ in 1 2 3 4 5; do
        out=$(./build/lxrun "$PTH_SAMPLE" 2>/dev/null) || fails=$((fails+1))
        grep -q "80000 (esperado 80000) OK, TLS malo en 0 hilos" <<<"$out" || fails=$((fails+1))
    done
    if [ "$fails" -eq 0 ]; then
        ok "4 pthreads with mutex+condvar, 5 runs, all correct"
    else
        bad "4 pthreads with mutex+condvar" "$fails failures in 5 runs; last out='$out'"
    fi
else
    echo "  skip  thread tests (run scripts/mkroot-rpm.sh first)"
fi

# 10. Signals: sigaction with SA_SIGINFO, a Linux siginfo_t and ucontext_t on
# the guest stack, blocking, and asynchronous delivery from a timer.
SIG_SAMPLE=$SAMPLES/sig_test
if [ -f "$SIG_SAMPLE" ]; then
    out=$(./build/lxrun "$SIG_SAMPLE" 2>/dev/null); rc=$?
    nok=$(grep -c "OK$" <<<"$out")
    if [ "$rc" -eq 0 ] && [ "$nok" -eq 4 ]; then
        ok "signals: raise, context preserved, blocking, async SIGALRM (4/4)"
    else
        bad "signals" "rc=$rc, $nok/4 OK; out='$out'"
    fi
else
    echo "  skip  signal tests (run scripts/mkroot-rpm.sh first)"
fi

# 11. The graphics bridge: a Linux ELF calling Mach-O MoltenVK in-process.
VK_SAMPLE=$SAMPLES/vk_test
if [ -f "$VK_SAMPLE" ]; then
    out=$(./build/lxrun "$VK_SAMPLE" 2>/dev/null); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "vkCreateInstance -> 0" <<<"$out" \
                       && grep -qE "\[0\] Apple" <<<"$out"; then
        ok "guest ELF creates a Vulkan instance on MoltenVK and sees the real GPU"
    else
        bad "guest ELF reaches MoltenVK" "rc=$rc out='$out'"
    fi
    ns=$(grep -oE "llamada directa al anfitrion: [0-9.]+" <<<"$out" | awk '{print $NF}')
    if [ -n "$ns" ]; then
        ok "direct guest->host call costs ${ns} ns (no marshalling)"
    else
        bad "direct guest->host call measured" "no timing line in output"
    fi
else
    echo "  skip  Vulkan bridge test (run scripts/mkroot-rpm.sh first)"
fi

# 12. The ELF libvulkan.so.1 shim: a plain consumer linked by SONAME, resolved
# by the guest's own ld.so, tail-calling into MoltenVK.
SHIM_SAMPLE=$SAMPLES/vkshim_test
if [ -f "$SHIM_SAMPLE" ] && [ -f "$GUEST_ROOT/lib64/libvulkan.so.1" ]; then
    out=$(LXRT_ROOT=$GUEST_ROOT LD_LIBRARY_PATH=/lib64 ./build/lxrun "$SHIM_SAMPLE" 2>/dev/null)
    rc=$?
    if [ "$rc" -eq 0 ] && grep -q "vkCreateInstance -> 0" <<<"$out" \
                       && grep -q "vkDestroyInstance ok" <<<"$out"; then
        ok "plain -lvulkan consumer runs through the ELF shim"
    else
        bad "plain -lvulkan consumer runs through the ELF shim" "rc=$rc out='$out'"
    fi
    ns=$(grep -oE "por el shim: [0-9.]+" <<<"$out" | awk '{print $NF}')
    [ -n "$ns" ] && ok "shim thunk measured: ${ns} ns per Vulkan call" \
                 || bad "shim thunk measured" "no timing line"
else
    echo "  skip  Vulkan shim test (needs make shim + scripts/mkroot-rpm.sh)"
fi

# 13. Presentation: a window the runtime owns, a surface on its CAMetalLayer,
# and a swapchain acquire/present loop. Opens a real window for a moment.
PRESENT_SAMPLE=$SAMPLES/vkpresent_test
if [ -f "$PRESENT_SAMPLE" ] && [ -f "$GUEST_ROOT/lib64/libvulkan.so.1" ]; then
    out=$(LXRT_ROOT=$GUEST_ROOT LD_LIBRARY_PATH=/lib64 \
          ./build/lxrun "$PRESENT_SAMPLE" 90 2>/dev/null); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "presentacion completa" <<<"$out"; then
        line=$(grep -oE "mediana [0-9.]+ ms \([0-9.]+ fps\)" <<<"$out")
        ok "swapchain on a CAMetalLayer presents: $line"
    else
        bad "swapchain presents" "rc=$rc out='$out'"
    fi
else
    echo "  skip  presentation test (needs make shim + scripts/mkroot-rpm.sh)"
fi

# 14. A real, unmodified Vulkan tool: Fedora's vulkaninfo, which dlopens
# libvulkan.so.1 and knows nothing about the runtime.
VKINFO="$GUEST_ROOT/usr/bin/vulkaninfo"
if [ -x "$VKINFO" ] && [ -f "$GUEST_ROOT/lib64/libvulkan.so.1" ]; then
    out=$(LXRT_ROOT=$GUEST_ROOT LD_LIBRARY_PATH=/lib64 ./build/lxrun "$VKINFO" 2>/dev/null)
    rc=$?
    lines=$(wc -l <<<"$out" | tr -d " ")
    if [ "$rc" -eq 0 ] && grep -q "driverID *= *DRIVER_ID_MOLTENVK" <<<"$out"; then
        ok "Fedora vulkaninfo runs unmodified ($lines lines, MoltenVK)"
    else
        bad "Fedora vulkaninfo runs unmodified" "rc=$rc, $lines lines"
    fi
else
    echo "  skip  vulkaninfo test (no $VKINFO)"
fi

# 15. The full render path: SPIR-V -> MSL -> Metal, a render pass and a draw.
TRI_SAMPLE=$SAMPLES/vktri_test
if [ -f "$TRI_SAMPLE" ] && [ -f "$GUEST_ROOT/lib64/libvulkan.so.1" ]; then
    out=$(LXRT_ROOT=$GUEST_ROOT LD_LIBRARY_PATH=/lib64 \
          ./build/lxrun "$TRI_SAMPLE" 120 2>/dev/null); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "triangulo dibujado" <<<"$out"; then
        pipe=$(grep -oE "compilado en [0-9.]+ ms" <<<"$out")
        med=$(grep -oE "mediana [0-9.]+ ms \([0-9.]+ fps\)" <<<"$out")
        ok "triangle renders through a real pipeline ($med, pipeline $pipe)"
    else
        bad "triangle renders" "rc=$rc out='$out'"
    fi
else
    echo "  skip  triangle test (needs make shim + scripts/mkroot-rpm.sh)"
fi

# 16. The syscall families added for Steam: epoll, eventfd, System V IPC,
# memfd, flock, statx and the futex operations glibc's condvar needs.
FAM_SAMPLE=$SAMPLES/fam_test
if [ -f "$FAM_SAMPLE" ]; then
    out=$(./build/lxrun "$FAM_SAMPLE" 2>/dev/null); rc=$?
    n_ok=$(grep -c "^  OK  " <<<"$out")
    n_bad=$(grep -c "MAL" <<<"$out")
    # The same binary scores 26/26 on a real Linux kernel, so anything less is
    # this runtime's gap, not the test's.
    if [ "$rc" -eq 0 ] && [ "$n_ok" -eq 26 ] && [ "$n_bad" -eq 0 ]; then
        ok "syscall families: 26/26 (epoll, eventfd, SysV IPC, memfd, flock, statx, futex)"
    else
        bad "syscall families 26/26" "rc=$rc, $n_ok ok, $n_bad bad -- Linux native scores 26/26"
    fi
else
    echo "  skip  syscall family tests (run scripts/mkroot-rpm.sh first)"
fi

# 17. Guest-driven W^X: the mechanism FEX's JIT relies on, proven without FEX.
# Private syscall 0x4C580020 opens/closes the per-thread write window on a
# MAP_JIT region, and the execute flip rescans the emitted range -- round 3
# emits a live `svc` from "JIT" code and only a rewrite makes it return
# getpid() rather than running whatever Darwin syscall is in x16.
JITWX_SAMPLE=$SAMPLES/jit_wx
if [ -f "$JITWX_SAMPLE" ]; then
    out=$(./build/lxrun "$JITWX_SAMPLE" 2>&1); rc=$?
    n_ok=$(grep -c "^  OK  " <<<"$out")
    n_bad=$(grep -c "MAL" <<<"$out")
    n_rw=$(grep -c "JIT output: 1 svc sites rewritten" <<<"$out")
    if [ "$rc" -eq 0 ] && [ "$n_ok" -eq 6 ] && [ "$n_bad" -eq 0 ] && [ "$n_rw" -ge 2 ]; then
        ok "guest-driven W^X + sub-page guard: 6/6, JIT-emitted svc rewritten on the execute flip ($n_rw rescans)"
    else
        bad "guest-driven W^X + sub-page guard 6/6" "rc=$rc, $n_ok ok, $n_bad bad, $n_rw rescans -- Linux native scores 6/6"
    fi
else
    echo "  skip  guest-driven W^X test (run scripts/mkroot-rpm.sh first)"
fi

# 17b. fork + JIT + SIGCHLD without FEX: synchronous delivery and Darwin's
# syscall restart (x16) after a handler, 40 children.
JITFORK_SAMPLE=$SAMPLES/jit_fork
if [ -f "$JITFORK_SAMPLE" ]; then
    out=$(./build/lxrun "$JITFORK_SAMPLE" 40 2>/dev/null); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "^== 40 ok, 0 mal" <<<"$out"; then
        ok "fork + JIT + SIGCHLD x40 without FEX: 40/40"
    else
        bad "fork + JIT + SIGCHLD x40" "rc=$rc: $(grep '^==' <<<"$out")"
    fi
else
    echo "  skip  jit_fork (run scripts/mkroot-rpm.sh first)"
fi

# 17d. The same with threads: region made by one thread, fork on another while
# a third keeps emitting code. Measured hangs before: a runtime mutex held at
# fork time, a tid equal to the child's pid, and pthread_join's SHARED futex on
# a copy-on-write page.
JFMT_SAMPLE=$SAMPLES/jit_fork_mt
if [ -f "$JFMT_SAMPLE" ]; then
    out=$(LXRT_ROOT=$GUEST_ROOT LD_LIBRARY_PATH=/lib64 ./build/lxrun "$JFMT_SAMPLE" 20 2>/dev/null); rc=$?
    if [ "$rc" -eq 0 ] && grep -q "20 ok, 0 mal" <<<"$out"; then ok "multithreaded JIT fork: 20/20"
    else bad "multithreaded JIT fork" "rc=$rc out='$(tail -1 <<<"$out")'"; fi
else
    echo "  skip  jit_fork_mt (run scripts/mkroot-rpm.sh first)"
fi

# 17c. x18. Darwin zeroes it on every exception return; Linux code keeps
# temporaries in it. The rewriter virtualises every instruction naming it
# inside executable sections (benchmarks/stage5-x18.txt). Same binary is
# all-green on Linux.
X18_SAMPLE=$SAMPLES/x18_test
if [ -f "$X18_SAMPLE" ]; then
    out=$(./build/lxrun "$X18_SAMPLE" 2>/dev/null); rc=$?
    n_bad=$(grep -c "MAL" <<<"$out"); n_ok=$(grep -c "^  ok " <<<"$out")
    if [ "$rc" -eq 0 ] && [ "$n_bad" -eq 0 ] && [ "$n_ok" -ge 20 ]; then
        ok "x18 virtualised: $n_ok/$n_ok checks (syscall, sleep, signal, preemption, every census form)"
    else
        bad "x18 virtualised" "rc=$rc, $n_ok ok, $n_bad bad: $(grep MAL <<<"$out" | head -3 | tr '\n' ';')"
    fi
    out=$(LXRT_NO_X18=1 ./build/lxrun "$X18_SAMPLE" 2>/dev/null)
    if grep -q "MAL" <<<"$out"; then
        ok "without the x18 pass the same binary loses x18 (LXRT_NO_X18=1: $(grep -c MAL <<<"$out") failures)"
    else
        bad "LXRT_NO_X18 control" "expected failures without the pass, saw none"
    fi
else
    echo "  skip  x18_test (run scripts/mkroot-rpm.sh first)"
fi

# 18. mremap, which Darwin does not have. glibc's realloc of large blocks uses
# it, so does FEX's code cache. The same binary scores 22/22 on Linux.
MREMAP_SAMPLE=$SAMPLES/mremap_test
if [ -f "$MREMAP_SAMPLE" ]; then
    out=$(./build/lxrun "$MREMAP_SAMPLE" 2>/dev/null); rc=$?
    n_ok=$(grep -c "^  OK  " <<<"$out"); n_bad=$(grep -c "MAL" <<<"$out")
    if [ "$rc" -eq 0 ] && [ "$n_ok" -eq 22 ] && [ "$n_bad" -eq 0 ]; then
        ok "mremap: 22/22 (shrink, grow in place, move, FIXED, DONTUNMAP, errors)"
    else
        bad "mremap 22/22" "rc=$rc, $n_ok ok, $n_bad bad -- Linux native scores 22/22"
    fi
else
    echo "  skip  mremap test (run scripts/mkroot-rpm.sh first)"
fi

# 19. timerfd and signalfd over kqueue. Steam holds 2 timerfd and 4 signalfd
# descriptors live (benchmarks/stage6-steam-gap.txt). Linux scores 23/23.
TFD_SAMPLE=$SAMPLES/tfd_sfd_test
if [ -f "$TFD_SAMPLE" ]; then
    out=$(./build/lxrun "$TFD_SAMPLE" 2>/dev/null); rc=$?
    n_ok=$(grep -c "^  OK  " <<<"$out"); n_bad=$(grep -c "MAL" <<<"$out")
    if [ "$rc" -eq 0 ] && [ "$n_ok" -eq 23 ] && [ "$n_bad" -eq 0 ]; then
        ok "timerfd + signalfd: 23/23 (one-shot, periodic, ABSTIME, epoll, NONBLOCK)"
    else
        bad "timerfd + signalfd 23/23" "rc=$rc, $n_ok ok, $n_bad bad -- Linux native scores 23/23"
    fi
else
    echo "  skip  timerfd/signalfd test (run scripts/mkroot-rpm.sh first)"
fi

# 20. inotify over kqueue EVFILT_VNODE. CEF, SDL3 and pressure-vessel import it.
# IN_OPEN/IN_ACCESS/IN_CLOSE_* cannot be observed on Darwin and the test does
# not depend on them. Linux scores 41/41.
INO_SAMPLE=$SAMPLES/inotify_test
if [ -f "$INO_SAMPLE" ]; then
    out=$(./build/lxrun "$INO_SAMPLE" 2>/dev/null); rc=$?
    n_ok=$(grep -c "^  OK  " <<<"$out"); n_bad=$(grep -c "MAL" <<<"$out")
    if [ "$rc" -eq 0 ] && [ "$n_ok" -eq 41 ] && [ "$n_bad" -eq 0 ]; then
        ok "inotify: 41/41 (create, modify, move pairs, delete, IN_IGNORED, epoll)"
    else
        bad "inotify 41/41" "rc=$rc, $n_ok ok, $n_bad bad -- Linux native scores 41/41"
    fi
else
    echo "  skip  inotify test (run scripts/mkroot-rpm.sh first)"
fi

# 21. A bwrap plan, interpreted (runtime/mounts.c). pressure-vessel builds
# every game's sandbox with bwrap, which needs mount namespaces Darwin lacks;
# the plan is materialised and bind-mapped instead. The same binary runs the
# same plan through the real bwrap on Linux and scores 12/12 there.
BWRAP_SAMPLE=$SAMPLES/bwrap_test
if [ -f "$BWRAP_SAMPLE" ]; then
    out=$(LXRT_ROOT=$GUEST_ROOT ./build/lxrun "$BWRAP_SAMPLE" 2>/dev/null); rc=$?
    n_ok=$(grep -c "^  OK  " <<<"$out"); n_bad=$(grep -c "MAL" <<<"$out")
    if [ "$rc" -eq 0 ] && [ "$n_ok" -eq 12 ] && [ "$n_bad" -eq 0 ]; then
        ok "fake bwrap: 12/12 (ro-bind, bind, symlink, tmpfs, bind-data, setenv, chdir, proc, chmod)"
    else
        bad "fake bwrap 12/12" "rc=$rc, $n_ok ok, $n_bad bad -- Linux with real bwrap scores 12/12"
    fi
else
    echo "  skip  bwrap test (run scripts/mkroot-rpm.sh first)"
fi

# M6: the runtime's vDSO (runtime/vdso). Needs the Fedora sysroot for glibc.
SYSROOT="${STEAMARM_BUILD:-$HOME/SteamARM-build}/sysroot-f43"
if [ -d "$SYSROOT" ] && [ -d "$GUEST_ROOT/tmp" ]; then
    if err=$(/opt/homebrew/opt/llvm/bin/clang --target=aarch64-redhat-linux-gnu --sysroot="$SYSROOT" \
                 -fuse-ld=lld -O2 -o "$GUEST_ROOT/tmp/vdso_clock" tests/elf/vdso_clock.c 2>&1); then
        out=$(LXRT_ROOT="$GUEST_ROOT" ./build/lxrun /tmp/vdso_clock 2>&1)
        if grep -q "== vdso clock: ok" <<<"$out"; then
            ok "vdso: $(grep -o '[0-9.]* ns/call' <<<"$out") clock_gettime, agrees with the syscall"
        else
            bad "vdso clock" "$(grep -v '^\[lxrt\]' <<<"$out" | tail -8)"
        fi
    else
        bad "build vdso_clock" "$err"
    fi
else
    echo "  skip  vdso test (no sysroot)"
fi

# MAP_PRIVATE of a memfd follows the file until written (runtime/privmap.c).
if [ -d "$SYSROOT" ] && [ -d "$GUEST_ROOT/tmp" ]; then
    if err=$(/opt/homebrew/opt/llvm/bin/clang --target=aarch64-redhat-linux-gnu --sysroot="$SYSROOT" \
                 -fuse-ld=lld -O2 -o "$GUEST_ROOT/tmp/memfd_private" tests/elf/memfd_private.c 2>&1); then
        out=$(LXRT_ROOT="$GUEST_ROOT" ./build/lxrun /tmp/memfd_private 2>&1)
        if grep -q "== memfd private: ok" <<<"$out"; then
            ok "memfd MAP_PRIVATE: sees file updates until written, copy on write per page"
        else
            bad "memfd MAP_PRIVATE" "$(grep -v '^\[lxrt\]' <<<"$out" | tail -6)"
        fi
    else
        bad "build memfd_private" "$err"
    fi
fi

# Signals to one thread of another process, and realtime signals (signal.c:
# mailbox + carrier) -- wineserver's SIGUSR1 to client threads.
if [ -d "$SYSROOT" ] && [ -d "$GUEST_ROOT/tmp" ]; then
    if err=$(/opt/homebrew/opt/llvm/bin/clang --target=aarch64-redhat-linux-gnu --sysroot="$SYSROOT" \
                 -fuse-ld=lld -O2 -pthread -o "$GUEST_ROOT/tmp/xproc_signal" tests/elf/xproc_signal.c 2>&1); then
        out=$(LXRT_ROOT="$GUEST_ROOT" ./build/lxrun /tmp/xproc_signal 2>&1)
        if grep -q "== xproc signal: ok" <<<"$out"; then
            ok "signals: tgkill to a thread of another process, realtime across processes and locally"
        else
            bad "signals across processes" "$(grep -v '^\[lxrt\]' <<<"$out" | tail -6)"
        fi
    else
        bad "build xproc_signal" "$err"
    fi
fi

# O_PATH of a socket and a FIFO (pathfd.c) -- pressure-vessel's check of the
# PulseAudio socket before binding it into Steam's container.
if [ -d "$SYSROOT" ] && [ -d "$GUEST_ROOT/tmp" ]; then
    if err=$(/opt/homebrew/opt/llvm/bin/clang --target=aarch64-redhat-linux-gnu --sysroot="$SYSROOT" \
                 -fuse-ld=lld -O2 -o "$GUEST_ROOT/tmp/opath_special" tests/elf/opath_special.c 2>&1); then
        out=$(LXRT_ROOT="$GUEST_ROOT" ./build/lxrun /tmp/opath_special 2>&1)
        if grep -q "== opath special: ok" <<<"$out"; then
            ok "O_PATH names sockets and FIFOs (fstat, AT_EMPTY_PATH, /proc/self/fd)"
        else
            bad "O_PATH of special files" "$(grep -v '^\[lxrt\]' <<<"$out" | grep FAIL | head -6)"
        fi
    else
        bad "build opath_special" "$err"
    fi
fi

# /dev/input/eventN (evdev.c) against the fake controller daemon: ioctls,
# events, force feedback -- what SDL and Proton's winebus do with a pad.
if [ -d "$SYSROOT" ] && [ -d "$GUEST_ROOT/tmp" ] && ! pgrep -qx steamarm-inputd; then
    if err=$(/opt/homebrew/opt/llvm/bin/clang --target=aarch64-redhat-linux-gnu --sysroot="$SYSROOT" \
                 -fuse-ld=lld -O2 -o "$GUEST_ROOT/tmp/evdev_test" tests/elf/evdev_test.c 2>&1); then
        python3 tests/elf/fake_inputd.py 20 --once > /tmp/lxrt-fake-inputd.log 2>&1 &
        fake=$!
        for _ in $(seq 1 50); do [ -S /tmp/lxrt-input/event0 ] && break; sleep 0.1; done
        out=$(LXRT_ROOT="$GUEST_ROOT" ./build/lxrun /tmp/evdev_test 2>&1)
        sleep 0.3
        kill $fake 2>/dev/null; wait $fake 2>/dev/null
        rm -f /tmp/lxrt-input/event0 /tmp/lxrt-input/meta/event0
        if grep -q "== evdev: ok" <<<"$out" && grep -q "rumble strong=32768 weak=16384 ms=250" /tmp/lxrt-fake-inputd.log; then
            ok "/dev/input: evdev ioctls, events and rumble to the controller daemon"
        else
            bad "/dev/input evdev" "$(grep FAIL <<<"$out" | head -6)"
        fi
    else
        bad "build evdev_test" "$err"
    fi
fi

echo
summary="== $PASS passed, $FAIL failed"
[ "$XFAIL" -eq 0 ] || summary="$summary ($XFAIL expected failures)"
echo "$summary"
[ "$FAIL" -eq 0 ]
