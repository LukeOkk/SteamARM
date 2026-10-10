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
# An lxrun linked as built against a pre-13 macOS SDK (the default since
# stage 28; make lxrt LXRT_KEEP_X18=0 opts out) has its x18 kept by the
# kernel in the process it exec'd: the LXRT_NO_X18 controls below then see
# x18 survive without the pass instead of lost (tests/x18_preserve/run.sh,
# benchmarks/stage24-minecraft-prism.txt, benchmarks/stage28-keep-x18.txt).
LXRUN_SDK=$(otool -l build/lxrun 2>/dev/null | awk '/LC_BUILD_VERSION/{f=1} f && $1 == "sdk" {print $2; exit}')
kernel_keeps_x18() { [ -n "$LXRUN_SDK" ] && [ "${LXRUN_SDK%%.*}" -lt 13 ]; }

CROSS_LD=/opt/homebrew/opt/lld/bin/ld.lld
STAGE="${STEAMARM_BUILD:-$HOME/SteamARM-build}/rootstage-f43"
GCCDIR=$(ls -d "$STAGE"/usr/lib/gcc/aarch64-redhat-linux/* 2>/dev/null | tail -1)
glibc_cc() { /opt/homebrew/opt/llvm/bin/clang --target=aarch64-redhat-linux-gnu --sysroot="$STAGE" \
                    --gcc-install-dir="$GCCDIR" -fuse-ld=lld --ld-path=$CROSS_LD -w "$@" 2>&1; }
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

# What the runtime keeps per file mapping goes with the guest's munmap, on the
# host (tests/elfsect_unmap_check.c): glibc's dlopen/dlclose of the root's own
# libraries at a new address every cycle. The Steam client reloads libusb
# four times a second, and each cycle used to leave its function tables
# (4 x 16 KiB) and file names behind.
unmap_libs=()
for l in lib64/libc.so.6 lib64/libvulkan.so.1 usr/lib64/libstdc++.so.6; do
    [ -f "$GUEST_ROOT/$l" ] && unmap_libs+=("$GUEST_ROOT/$l")
done
if [ ${#unmap_libs[@]} -gt 0 ]; then
    if out=$(make -s build/elfsect_unmap_check 2>&1 &&
             build/elfsect_unmap_check -n 1000 "${unmap_libs[@]}" 2>&1); then
        ok "function tables and file names freed at munmap: ${#unmap_libs[@]} libraries, 1000 dlopen/dlclose cycles, $(sed -n 's/.*heap growth over the last \([0-9]*\) cycles: \(-*[0-9]*\) bytes.*/\2 bytes of heap growth in \1/p' <<<"$out"), lookups unchanged"
    else
        bad "function tables and file names across munmap" "$(grep -E 'FAIL' <<<"$out" | head -5)"
    fi
else
    echo "  skip  elfsect_unmap_check (no libc.so.6 in $GUEST_ROOT)"
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
    if kernel_keeps_x18; then
        if [ "$rc" -eq 0 ]; then
            ok "without the x18 pass the same program passes: the kernel keeps x18 (lxrun sdk $LXRUN_SDK)"
        else
            bad "LXRT_NO_X18 control for x18_forms (lxrun sdk $LXRUN_SDK)" "expected x18 kept, got rc=$rc"
        fi
    elif [ "$rc" -ne 0 ]; then
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
# another host page. A store executed from the shared page itself could never
# complete (the page flipped RW/RX at the same pc forever, up to stage 22);
# the runtime now performs that store itself (runtime/storemu.c), and code
# stored into an executable 4 KiB page is rewritten before it runs.
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
        out=$(LXRT_SUBPAGE_LOG=100 deadline 10 ./build/lxrun build/subpage4k selfwrite 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && [ "$(grep -c 'subpage emulate' <<<"$out")" -eq 10 ]; then
            ok "4 KiB ELF: 10 stores from the host page they write, each performed by the runtime (no W/X livelock)"
        else
            bad "4 KiB ELF self-write" "rc=$rc (142: the W/X livelock; 4: wrong count) $(grep -c 'subpage emulate' <<<"$out") emulated"
        fi
        # Negative control: a store from the shared page into its own
        # read-execute code page is a fault on Linux, and must stay one.
        out=$(deadline 10 ./build/lxrun build/subpage4k codewrite 2>&1); rc=$?
        if [ "$rc" -eq 138 ] && grep -q 'SIGBUS at pc.*insn 0xb900001f' <<<"$out"; then
            ok "4 KiB ELF: a store into the read-execute code page still faults (not emulated)"
        else
            bad "4 KiB ELF code-page store faults" "rc=$rc $(grep SIG <<<"$out")"
        fi
        # Code stored into an executable 4 KiB page (made RWX by the guest) is
        # rescanned before it runs: `svc #0` then `udf`. Rewritten: exit(42).
        # Live: Darwin getpid (x16 = 20), then SIGILL (132) -- which is what
        # LXRT_NO_RESCAN=1, the negative control, must show.
        for mode in patch far; do
            deadline 10 ./build/lxrun build/subpage4k $mode >/dev/null 2>&1; rc=$?
            LXRT_NO_RESCAN=1 deadline 10 ./build/lxrun build/subpage4k $mode >/dev/null 2>&1; nrc=$?
            what="an emulated store"; [ $mode = far ] && what="a store from another page (W/X flip)"
            if [ "$rc" -eq 42 ] && [ "$nrc" -eq 132 ]; then
                ok "4 KiB ELF: svc written into an RWX 4 KiB page by $what is rewritten before it runs (control: live svc without the rescan)"
            else
                bad "4 KiB ELF: code written by $what rescanned" "rc=$rc (want 42), LXRT_NO_RESCAN rc=$nrc (want 132)"
            fi
        done
    fi
else
    bad "build subpage4k" "$err"
fi

# Store forms executed from the host page they write (runtime/storemu.c):
# the same routine runs against plain memory (the hardware) and against the
# shared page (the runtime) and must leave identical bytes and registers --
# base and SIMD&FP stores in every addressing mode, pairs, ST1-ST4, ordered
# stores, LSE atomics, CAS/CASP, LL/SC loops (including ones whose body
# overwrites the loaded register: the runtime runs the loop's retry), DC ZVA.
if err=$(clang -target aarch64-unknown-linux-gnu -nostdlib -static-pie -fPIE \
               -fuse-ld=$CROSS_LD -Wl,-e,_start -Wl,-z,max-page-size=4096 \
               -Wl,-z,common-page-size=4096 -Wl,-z,norelro \
               -o build/subpage_stores tests/elf/subpage_stores.S 2>&1); then
    syms=$(/opt/homebrew/opt/llvm/bin/llvm-readelf -sW build/subpage_stores)
    kit=$((16#$(awk '$8 == "kit" { print $2 }' <<<"$syms")))
    kit_end=$((16#$(awk '$8 == "kit_end" { print $2 }' <<<"$syms")))
    tgt=$((16#$(awk '$8 == "target" { print $2 }' <<<"$syms")))
    tgt_end=$((16#$(awk '$8 == "target_end" { print $2 }' <<<"$syms")))
    if [ $((kit >> 14)) -ne $((tgt >> 14)) ] || [ $((kit_end >> 14)) -ne $((tgt >> 14)) ] ||
       [ $(((tgt_end - 1) >> 14)) -ne $((tgt >> 14)) ]; then
        bad "store-forms precondition: kit and target in one 16 KiB page" "kit $kit-$kit_end target $tgt-$tgt_end"
    else
        out=$(LXRT_SUBPAGE_LOG=1000 deadline 30 ./build/lxrun build/subpage_stores 2>&1); rc=$?
        n=$(grep -c 'subpage emulate' <<<"$out")
        if [ "$rc" -eq 0 ] && [ "$n" -eq 87 ] && ! grep -q 'cannot be emulated' <<<"$out"; then
            ok "4 KiB ELF: 87 store forms from the page they write match the hardware (bytes, results, flags)"
        else
            bad "4 KiB ELF store forms" "rc=$rc (3: memory differs, 4: registers differ) $n emulated $(grep 'cannot be emulated' <<<"$out" | head -2)"
        fi
    fi
else
    bad "build subpage_stores" "$err"
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
if grep -q "ELF 64-bit LSB.*ARM aarch64" <<<"$(file build/hello 2>/dev/null)"; then
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
    if kernel_keeps_x18; then
        if ! grep -q "MAL" <<<"$out" && [ "$(grep -c "^  ok " <<<"$out")" -ge 20 ]; then
            ok "without the x18 pass the same binary keeps x18: the kernel keeps it (lxrun sdk $LXRUN_SDK)"
        else
            bad "LXRT_NO_X18 control (lxrun sdk $LXRUN_SDK)" "expected x18 kept: $(grep MAL <<<"$out" | head -3 | tr '\n' ';')"
        fi
    elif grep -q "MAL" <<<"$out"; then
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

# 19b. Linux per-process POSIX timers, including thread-directed delivery.
POSIX_TIMER_SAMPLE=$SAMPLES/posix_timer_test
if [ -f "$POSIX_TIMER_SAMPLE" ]; then
    out=$(./build/lxrun "$POSIX_TIMER_SAMPLE" 2>/dev/null); rc=$?
    n_ok=$(grep -c "^  OK  " <<<"$out"); n_bad=$(grep -c "MAL" <<<"$out")
    if [ "$rc" -eq 0 ] && [ "$n_ok" -eq 43 ] && [ "$n_bad" -eq 0 ]; then
        ok "POSIX timers: 43/43 (thread ID, periodic, ABSTIME, delete, fork)"
    else
        bad "POSIX timers 43/43" "rc=$rc, $n_ok ok, $n_bad bad"
    fi
else
    echo "  skip  POSIX timer test (run scripts/mkroot-rpm.sh first)"
fi

# 19d. A SIGSEGV handler that never fixes the fault: the process dies of
# signal 11 after 10000 identical faults (runtime/signal.c) instead of
# spinning forever (an i386 Android daemon under FEX did, at 100% CPU).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O0 -o build/segv_spin tests/elf/segv_spin.c 2>&1); then
        t0=$SECONDS
        out=$(deadline 60 ./build/lxrun "$PWD/build/segv_spin" 2>&1); rc=$?
        if [ "$rc" -eq 139 ] && grep -q "10000 times in a row" <<<"$out"; then
            ok "a fault its handler never fixes ends the process (signal 11 after $((SECONDS - t0)) s)"
        else
            bad "unfixed SIGSEGV ends the process" "rc=$rc $(tail -2 <<<"$out" | tr '\n' ' ')"
        fi
    else
        bad "build segv_spin" "$err"
    fi
fi

# 19e. A signal that runs no handler interrupts nothing: a realtime signal
# from another process that the target has blocked still arrives as the
# carrier, and Darwin's sleeps and waits came back EINTR (the Steam client's
# ThreadSleep then slept zero). Each call must run its whole 300 ms, and the
# signal must stay pending (rt_sigpending, which was ENOSYS).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/quiet_interrupt tests/elf/quiet_interrupt.c 2>&1); then
        out=$(deadline 60 ./build/lxrun "$PWD/build/quiet_interrupt" 2>&1); rc=$?
        n_ok=$(grep -c "^  OK  " <<<"$out")
        if [ "$rc" -eq 0 ] && [ "$n_ok" -eq 6 ]; then
            ok "a blocked signal from another process cuts no wait short: nanosleep, clock_nanosleep, three futex waits ran 300 ms; still pending"
        else
            bad "waits a blocked signal must not cut short" "rc=$rc, $n_ok/6: $(grep MAL <<<"$out" | head -3 | tr '\n' ' ')"
        fi
    else
        bad "build quiet_interrupt" "$err"
    fi
fi

# 19e2. Virtual pages below 4 GiB for a native guest (runtime/lowpage.c,
# LXRT_LOWPAGES=1): Wine ARM64 maps KUSER_SHARED_DATA fixed at 0x7ffe0000,
# which macOS cannot map; the runtime keeps it elsewhere and carries out the
# faulting loads and stores itself. Every addressing form, protection and
# unmapping, and the answers Wine's allocator gets for its probes below 4 GiB.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O1 -o build/lowpage tests/elf/lowpage.c 2>&1); then
        out=$(LXRT_LOWPAGES=1 deadline 60 ./build/lxrun "$PWD/build/lowpage" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q "^lowpage: [0-9]* ok, 0 failed" <<<"$out"; then
            ok "virtual page at 0x7ffe0000: $(grep -o 'lowpage: [0-9]* ok' <<<"$out") -- loads, stores, pairs, SIMD, writeback, acquire/release, read-only, unmapped"
        else
            bad "virtual page at 0x7ffe0000" "rc=$rc: $(grep FAIL <<<"$out" | head -3 | tr '\n' ' ')"
        fi
        out=$(deadline 60 ./build/lxrun "$PWD/build/lowpage" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q "^lowpage: 1 ok, 0 failed" <<<"$out"; then
            ok "without LXRT_LOWPAGES a page below 4 GiB is still refused"
        else
            bad "page below 4 GiB refused without LXRT_LOWPAGES" "rc=$rc: $(tail -2 <<<"$out" | tr '\n' ' ')"
        fi
    else
        bad "build lowpage" "$err"
    fi
fi

# 19f. readv, preadv, pwritev, preadv2 and pwritev2 (all ENOSYS before), and
# read() on a seqpacket pair whose peer died without writing: end of file,
# not a read that never returns (Steam's web helper and its zygotes).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/vector_io tests/elf/vector_io.c 2>&1); then
        out=$(deadline 60 ./build/lxrun "$PWD/build/vector_io" 2>&1); rc=$?
        n_ok=$(grep -c "^  OK  " <<<"$out")
        if [ "$rc" -eq 0 ] && [ "$n_ok" -eq 10 ]; then
            ok "vector I/O: readv, preadv, pwritev, preadv2, pwritev2; read() on a seqpacket pair ends at its dead peer"
        else
            bad "vector I/O and seqpacket end of file" "rc=$rc, $n_ok/10: $(grep MAL <<<"$out" | head -3 | tr '\n' ' ')"
        fi
    else
        bad "build vector_io" "$err"
    fi
fi

# 19g. fork() while another thread places sub-page mappings next to a
# mirrored shared view: the fork's prepare handlers must take the page lock
# before the mirror's (Steam's native web helper deadlocked there).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -pthread -o build/fork_mirror tests/elf/fork_mirror.c 2>&1); then
        out=$(deadline 40 ./build/lxrun "$PWD/build/fork_mirror" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q "no deadlock" <<<"$out"; then
            ok "fork against sub-page placement: $(grep -o '[0-9]* sub-page placements, [0-9]* forks' <<<"$out"), no deadlock"
        else
            bad "fork against sub-page placement" "rc=$rc (a deadlock ends at the 40 s deadline) $(tail -1 <<<"$out")"
        fi
    else
        bad "build fork_mirror" "$err"
    fi
fi

# 19h. The runtime's own descriptors stay out of the guest's way: after a
# directory is read, the next open() is the lowest free number, and
# /proc/self/fd shows no directory the program never opened (Android's
# zygote aborts on one before it forks).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/private_fds tests/elf/private_fds.c 2>&1); then
        out=$(deadline 30 ./build/lxrun "$PWD/build/private_fds" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && [ "$(grep -c '^  OK  ' <<<"$out")" -eq 3 ]; then
            ok "the runtime's descriptors: the lowest free number after a readdir, none in /proc/self/fd"
        else
            bad "the runtime's descriptors out of the guest's way" "rc=$rc $(grep -E 'MAL|never opened' <<<"$out" | head -3 | tr '\n' ' ')"
        fi
    else
        bad "build private_fds" "$err"
    fi
fi

# 19i. Linux abstract socket names ('/', '%', 100 bytes, NUL-padded): bind,
# connect, and getsockname gives the abstract address back (Android's zygote
# checks a child zygote's socket by it). rt_tgsigqueueinfo to this thread or
# process: the handler gets the queued siginfo (FEX's seccomp emulation
# raises SIGSYS so).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    for t in abstract_names sigqueueinfo; do
        if err=$(glibc_cc -static-pie -O2 -o build/$t tests/elf/$t.c 2>&1); then
            out=$(deadline 30 ./build/lxrun "$PWD/build/$t" 2>&1); rc=$?
            if [ "$rc" -eq 0 ] && grep -q '^PASS' <<<"$out"; then
                ok "$t: $(grep -c '^  OK  ' <<<"$out") of $(grep -cE '^  (OK |MAL)' <<<"$out")"
            else
                bad "$t" "rc=$rc $(grep MAL <<<"$out" | head -3 | tr '\n' ' ')"
            fi
        else
            bad "build $t" "$err"
        fi
    done
fi

# 19j. memfd seals bind a process that received the descriptor (SCM_RIGHTS
# to a child that exec'd): F_GET_SEALS, no writable shared mapping. And a
# syscall's write into a read-only 4 KiB guest page is EFAULT although the
# host page stays writable. Both were checks Chromium's WebView renderer
# makes before it runs. exec with a guest environment of its own (Termux's)
# keeps the runtime's settings and /proc; pseudo-terminals the Linux way
# (Termux's terminal).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    for t in memfd_seal_xproc readonly_efault lost_guard exec_env pty unlink_dir fd_scan_threads futex_waitv futex_long futex_long_spin eventfd_scm ntsync self_signal self_signal_kernel; do
        src=$t; [ "$t" = futex_long_spin ] && src=futex_long     # the same program with the runtime's spin on
        [ "$t" = self_signal_kernel ] && src=self_signal         # and with Darwin delivering the self-signal
        if err=$(glibc_cc -static-pie -O2 -o build/$t tests/elf/$src.c 2>&1); then
            env=""
            if [ "$t" = futex_long_spin ]; then
                env="LXRT_FUTEX_SPIN_US=50"
                out=$(LXRT_FUTEX_SPIN_US=50 deadline 60 ./build/lxrun "$PWD/build/$t" 2>&1); rc=$?
            elif [ "$t" = readonly_efault ] || [ "$t" = lost_guard ]; then
                env="LXRT_GUEST_PAGE=4096"
                out=$(LXRT_GUEST_PAGE=4096 deadline 30 ./build/lxrun "$PWD/build/$t" 2>&1); rc=$?
            elif [ "$t" = self_signal_kernel ]; then
                env="LXRT_SYNC_SELF_SIGNAL=0"
                out=$(LXRT_SYNC_SELF_SIGNAL=0 deadline 30 ./build/lxrun "$PWD/build/$t" 2>&1); rc=$?
            elif [ "$t" = exec_env ]; then
                out=$(LXRT_TEST_EXEC_ENV=kept deadline 30 ./build/lxrun "$PWD/build/$t" 2>&1); rc=$?
            elif [ "$t" = ntsync ]; then
                # /dev/ntsync exists only when the launcher asks for it.
                env="LXRT_NTSYNC=1"
                out=$(LXRT_NTSYNC=1 deadline 30 ./build/lxrun "$PWD/build/$t" 2>&1); rc=$?
            else
                out=$(deadline 30 ./build/lxrun "$PWD/build/$t" 2>&1); rc=$?
            fi
            if [ "$rc" -eq 0 ] && grep -q '^PASS' <<<"$out"; then
                ok "$t: $(grep -c '^  OK  ' <<<"$out") of $(grep -cE '^  (OK |MAL)' <<<"$out")${env:+ ($env)}"
            else
                bad "$t" "rc=$rc $(grep MAL <<<"$out" | head -3 | tr '\n' ' ')"
            fi
        else
            bad "build $t" "$err"
        fi
    done
fi

# 19c. 32-bit bionic needs process and thread IDs below 65536. Exercise the
# opt-in namespace with native aarch64 first, leaving the normal run untouched.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -pthread -o build/small_ids_test tests/elf/small_ids_test.c 2>&1); then
        out=$(deadline 90 env LXRT_SMALL_IDS=1 ./build/lxrun "$PWD/build/small_ids_test" 2>&1); rc=$?
        n_ok=$(grep -c "^  OK  " <<<"$out"); n_bad=$(grep -c "MAL" <<<"$out")
        if [ "$rc" -eq 0 ] && [ "$n_ok" -eq 12 ] && [ "$n_bad" -eq 0 ]; then
            ok "small IDs: 12/12 (threads, fork, signals, proc, credentials, wait, reuse)"
        else
            bad "small IDs 12/12" "rc=$rc, $n_ok ok, $n_bad bad: $(tail -5 <<<"$out" | tr '\n' ' ')"
        fi
    else
        bad "build small_ids_test" "$err"
    fi
else
    echo "  skip  small IDs test (no $STAGE)"
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

# 20b. FIONREAD on an inotify descriptor is the size of the queued events, as
# on Linux. Qt reads exactly that many bytes on a blocking descriptor; the
# pipe's readiness byte (1) made it read(fd, buf, 1) and left Prism
# Launcher's window blank (benchmarks/stage24-minecraft-prism.txt).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/inotify_fionread tests/elf/inotify_fionread.c); then
        out=$(deadline 30 ./build/lxrun "$PWD/build/inotify_fionread" 2>&1)
        if grep -q "== inotify fionread: ok" <<<"$out"; then
            ok "inotify FIONREAD: the queued events' size ($(grep -o 'FIONREAD after one create: [0-9]*' <<<"$out" | grep -o '[0-9]*$') B for one), Qt's read of exactly that"
        else
            bad "inotify FIONREAD" "$(grep -E 'FAIL|read\(' <<<"$out" | head -6)"
        fi
    else
        bad "build inotify_fionread" "$err"
    fi
else
    echo "  skip  inotify FIONREAD (no $STAGE)"
fi

# 20c. An eventfd is one object across fork(), as on Linux. Qt's QProcess
# (forkfd) parks the child in eventfd_read until the parent writes; with the
# counter in process memory the child spun for ever and no QProcess started.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/eventfd_fork tests/elf/eventfd_fork.c); then
        out=$(deadline 30 ./build/lxrun "$PWD/build/eventfd_fork" 2>&1)
        if grep -q "== eventfd fork: ok" <<<"$out"; then
            ok "eventfd across fork: the child reads the parent's write ($(grep -o 'after [0-9.]* s' <<<"$out")), and back"
        else
            bad "eventfd across fork" "$(grep -E 'FAIL|child exit' <<<"$out" | head -6)"
        fi
    else
        bad "build eventfd_fork" "$err"
    fi
else
    echo "  skip  eventfd across fork (no $STAGE)"
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
# The same with LXRT_INPUT_DIR: a /dev/input of the guest's own (Android's
# composer FIFOs, per root), not /tmp/lxrt-input, so it runs even while
# steamarm-inputd does; without the variable /dev/input stays /tmp/lxrt-input
# (the controllers' directory).
if [ -d "$SYSROOT" ] && [ -x "$GUEST_ROOT/tmp/evdev_test" ]; then
    idir=$(mktemp -d /tmp/lxrt-inp.XXXXXX)
    LXRT_INPUT_DIR="$idir" python3 tests/elf/fake_inputd.py 20 --once > "$idir.log" 2>&1 &
    fake=$!
    for _ in $(seq 1 50); do [ -S "$idir/event0" ] && break; sleep 0.1; done
    out=$(LXRT_ROOT="$GUEST_ROOT" LXRT_INPUT_DIR="$idir" ./build/lxrun /tmp/evdev_test 2>&1)
    sleep 0.3
    kill $fake 2>/dev/null; wait $fake 2>/dev/null
    # Which directory /dev/input is: a marker made in the private directory
    # is seen only with the variable; and something that is in
    # /tmp/lxrt-input and not in the private directory (read only; nothing
    # is made there), when there is such a thing, only without it.
    # fake_inputd.py makes meta/ in both, so a name the two share proves
    # nothing (MEASURED: /tmp/lxrt-input held only meta/, and a check that
    # probed it failed).
    marker="steamarm-input-marker-$$"
    : > "$idir/$marker"
    m_shared=$(LXRT_ROOT="$GUEST_ROOT" ./build/lxrun /usr/bin/bash -c "test -e '/dev/input/$marker' && echo yes" 2>/dev/null)
    m_private=$(LXRT_ROOT="$GUEST_ROOT" LXRT_INPUT_DIR="$idir" ./build/lxrun /usr/bin/bash -c "test -e '/dev/input/$marker' && echo yes" 2>/dev/null)
    probe=$(comm -23 <(ls /tmp/lxrt-input 2>/dev/null | sort) <(ls "$idir" 2>/dev/null | sort) | head -1)
    shared="" private=""
    if [ -n "$probe" ]; then
        shared=$(LXRT_ROOT="$GUEST_ROOT" ./build/lxrun /usr/bin/bash -c "test -e '/dev/input/$probe' && echo yes" 2>/dev/null)
        private=$(LXRT_ROOT="$GUEST_ROOT" LXRT_INPUT_DIR="$idir" ./build/lxrun /usr/bin/bash -c "test -e '/dev/input/$probe' && echo yes" 2>/dev/null)
    fi
    if ! grep -q "== evdev: ok" <<<"$out" || ! grep -q "rumble strong=32768 weak=16384 ms=250" "$idir.log"; then
        bad "LXRT_INPUT_DIR evdev" "$(grep FAIL <<<"$out" | head -6)"
    elif [ "$m_private" != yes ] || [ -n "$m_shared" ]; then
        bad "LXRT_INPUT_DIR marker" "a file of the private directory seen as /dev/input/$marker: with the variable '$m_private', without it '$m_shared'"
    elif [ -n "$probe" ] && { [ "$shared" != yes ] || [ -n "$private" ]; }; then
        bad "LXRT_INPUT_DIR default" "/tmp/lxrt-input/$probe seen as /dev/input/$probe: without the variable '$shared', with it '$private'"
    else
        ok "LXRT_INPUT_DIR: evdev against a daemon in a private /dev/input, a file there seen only with the variable${probe:+; $probe of /tmp/lxrt-input only without it}"
    fi
    rm -rf "$idir" "$idir.log"
fi


# ARM64_INITIAL_STACK_BOUNDS: entry stack alignment, auxv right after envp,
# the pad page above the strings, the main thread's pthread bounds, a 6 MiB
# recursion, and Linux's argv-then-env string order (stack.c, 0.3.4 had env
# below argv). Run by absolute path: see the known failure below.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O1 -pthread -o build/stack_bounds tests/elf/stack_bounds.c); then
        out16=$(deadline 30 ./build/lxrun "$PWD/build/stack_bounds" a bb ccc 2>&1); rc16=$?
        out4=$(LXRT_GUEST_PAGE=4096 deadline 30 ./build/lxrun "$PWD/build/stack_bounds" a bb ccc 2>&1); rc4=$?
        if [ "$rc16" -eq 0 ] && [ "$rc4" -eq 0 ] &&
           grep -q '== stack bounds: ok' <<<"$out16" && grep -q '== stack bounds: ok' <<<"$out4"; then
            ok "ARM64_INITIAL_STACK_BOUNDS: entry alignment, auxv after envp, pad page, main-thread bounds, 6 MiB recursion, Linux string order (16 KiB and 4 KiB AT_PAGESZ)"
        else
            bad "ARM64_INITIAL_STACK_BOUNDS" "rc=$rc16/$rc4 $(grep FAIL <<<"$out16"; grep FAIL <<<"$out4")"
        fi
        # Started by a relative path: /proc/self/exe must still be absolute,
        # or static glibc asserts at start-up (_dl_get_origin, SIGABRT, rc 134
        # up to stage 22).
        out=$(deadline 30 ./build/lxrun build/stack_bounds 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== stack bounds: ok' <<<"$out"; then
            ok "static glibc started by a relative path (/proc/self/exe absolute; was _dl_get_origin abort)"
        else
            bad "relative program path" "rc=$rc $(grep -v '^\[lxrt\]' <<<"$out" | tail -3)"
        fi
    else bad "build stack_bounds" "$err"; fi
else
    echo "  skip  ARM64_INITIAL_STACK_BOUNDS (no $STAGE)"
fi

# A large anonymous mapping 8 KiB into a host page, as Wine's ntdll reserves
# its low address space: zero, mprotect-able in 4 KiB pieces, and not one
# kernel map entry per 16 KiB host page (runtime/subpage.c; a game's start
# made a million and safeguard.sh stopped every guest). Under FEX's 4 KiB
# guest pages (LXRT_GUEST_PAGE=4096), where the sub-page path is taken.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/subpage_big_anon tests/elf/subpage_big_anon.c); then
        out=build/subpage_big_anon.out
        LXRT_GUEST_PAGE=4096 deadline 30 ./build/lxrun build/subpage_big_anon > "$out" 2>&1 &
        sbp=$!
        for _ in $(seq 1 50); do grep -q '^mapped' "$out" && break; sleep 0.1; done
        regions=$(vmmap -summary "$(awk '/^mapped/ {print $2}' "$out")" 2>/dev/null | awk '/^TOTAL/ && !/MALLOC/ {print $NF; exit}')
        wait $sbp
        if grep -q '^== subpage_big_anon: ok' "$out" && [ -n "$regions" ] && [ "$regions" -lt 5000 ]; then
            ok "512 MiB anonymous mapping 8 KiB into a host page: zero, 4 KiB mprotect, $regions regions"
        else
            bad "subpage_big_anon" "regions=${regions:-?} $(grep -E 'FAIL' "$out" | head -3)"
        fi
    else bad "build subpage_big_anon" "$err"; fi
else
    echo "  skip  subpage_big_anon (no $STAGE)"
fi

# /proc/<pid>/root of another guest process: that process's "/" (PipeWire's
# Flatpak check opens it, and took every client for a Flatpak app while it
# was missing; runtime/procpid.c).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/proc_pid_root tests/elf/proc_pid_root.c); then
        PR_ROOT="$PWD/build/exe-root"
        mkdir -p "$PR_ROOT/tmp" && cp build/proc_pid_root "$PR_ROOT/tmp/"
        r=$(LXRT_ROOT="$PR_ROOT" deadline 20 ./build/lxrun /tmp/proc_pid_root /tmp/proc_pid_root.marker 2>&1)
        if grep -q '^== proc_pid_root: ok' <<<"$r"; then
            ok "/proc/<pid>/root of another guest process: its \"/\", .flatpak-info absent"
        else
            bad "/proc/<pid>/root" "$(grep -E 'FAIL' <<<"$r" | head -3)"
        fi
    else bad "build proc_pid_root" "$err"; fi
else
    echo "  skip  /proc/<pid>/root (no $STAGE)"
fi

# /proc/self/exe as Linux has it, however the program was named: absolute, in
# guest terms, equal through readlink and realpath, and open()able. Relative,
# with "..", absolute (the control: named as the host path already), and
# under LXRT_ROOT by a relative and by an absolute guest path.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/proc_self_exe tests/elf/proc_self_exe.c); then
        EXE_ROOT="$PWD/build/exe-root"
        mkdir -p "$EXE_ROOT/tmp" && cp build/proc_self_exe "$EXE_ROOT/tmp/"
        here=$(pwd -P)
        lx="$PWD/build/lxrun"
        r1=$(deadline 20 ./build/lxrun build/proc_self_exe "$here/build/proc_self_exe" 2>&1); c1=$?
        r2=$(deadline 20 ./build/lxrun ./build/../build/proc_self_exe "$here/build/proc_self_exe" 2>&1); c2=$?
        r3=$(deadline 20 ./build/lxrun "$PWD/build/proc_self_exe" "$PWD/build/proc_self_exe" 2>&1); c3=$?
        r4=$(cd "$EXE_ROOT/tmp" && LXRT_ROOT="$EXE_ROOT" deadline 20 "$lx" ./proc_self_exe /tmp/proc_self_exe 2>&1); c4=$?
        r5=$(LXRT_ROOT="$EXE_ROOT" deadline 20 ./build/lxrun /tmp/proc_self_exe /tmp/proc_self_exe 2>&1); c5=$?
        if [ "$c1$c2$c3$c4$c5" = 00000 ]; then
            ok "/proc/self/exe: absolute in guest terms, readlink = realpath, openable (relative, '..', absolute, rooted relative, rooted absolute)"
        else
            bad "/proc/self/exe" "rc=$c1/$c2/$c3/$c4/$c5 $(grep -hE '^exe|FAIL|_dl_get_origin' <<<"$r1$r2$r3$r4$r5" | head -6)"
        fi
    else bad "build proc_self_exe" "$err"; fi
else
    echo "  skip  /proc/self/exe (no $STAGE)"
fi

# ARM64_AUXV_LAYOUT: a dynamic program checks every auxv pair against what
# glibc and ld.so report, at AT_PAGESZ 16384 and at LXRT_GUEST_PAGE=4096, and
# dlopens a 4 KiB-aligned library: refused at 16 KiB, loaded at 4 KiB. Its
# written counter sits on its own host page (the W/X livelock above).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ] &&
   [ -f "$GUEST_ROOT/lib64/libc.so.6" ] && [ -f "$GUEST_ROOT/lib/ld-linux-aarch64.so.1" ]; then
    # A private two-file guest root under build/ (ld.so and libc from the
    # guest root): nothing is written into the shared root other guests use.
    AUX_ROOT="$PWD/build/auxv-root"
    mkdir -p "$AUX_ROOT/lib" "$AUX_ROOT/lib64" "$AUX_ROOT/tmp"
    if ! cp "$GUEST_ROOT/lib/ld-linux-aarch64.so.1" "$AUX_ROOT/lib/" ||
       ! cp "$GUEST_ROOT/lib64/libc.so.6" "$AUX_ROOT/lib64/"; then
        bad "prepare auxv_layout guest root" "$GUEST_ROOT"
    elif err=$(glibc_cc -O2 -pthread -o "$AUX_ROOT/tmp/auxv_layout" tests/elf/auxv_layout.c) &&
       err=$(glibc_cc -shared -fPIC -O2 -Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096 \
                      -o "$AUX_ROOT/tmp/libpg4k.so" tests/elf/libpg4k.c); then
        headers=$(/opt/homebrew/opt/llvm/bin/llvm-readelf -lW "$AUX_ROOT/tmp/libpg4k.so")
        symbols=$(/opt/homebrew/opt/llvm/bin/llvm-readelf -sW "$AUX_ROOT/tmp/libpg4k.so")
        counter=$(awk '$8 == "pg4k_counter" { print $2; exit }' <<<"$symbols")
        precond=0
        if [ -n "$counter" ]; then
            aligned=$(awk '$1 == "LOAD" && $NF == "0x1000" { print 1; exit }' <<<"$headers")
            read -r exec_start exec_size <<<"$(awk '$1 == "LOAD" && $8 == "E" { print $3, $6; exit }' <<<"$headers")"
            counter_addr=$((16#$counter))
            data_segment=0
            while read -r data_start data_size; do
                if [ "$counter_addr" -ge "$((data_start))" ] &&
                   [ "$counter_addr" -lt "$((data_start + data_size))" ]; then data_segment=1; fi
            done <<<"$(awk '$1 == "LOAD" && $7 == "RW" { print $3, $6 }' <<<"$headers")"
            if [ "${aligned:-0}" -eq 1 ] && [ -n "$exec_start" ] && [ "$data_segment" -eq 1 ] &&
               { [ $((counter_addr / 16384)) -lt $((exec_start / 16384)) ] ||
                 [ $((counter_addr / 16384)) -gt $(((exec_start + exec_size - 1) / 16384)) ]; }; then
                precond=1
            fi
        fi
        if [ "$precond" -eq 1 ]; then
            out16=$(LXRT_ROOT="$AUX_ROOT" deadline 30 ./build/lxrun /tmp/auxv_layout 16384 /tmp/libpg4k.so 2>&1); rc16=$?
            out4=$(LXRT_ROOT="$AUX_ROOT" LXRT_GUEST_PAGE=4096 deadline 30 ./build/lxrun /tmp/auxv_layout 4096 /tmp/libpg4k.so 2>&1); rc4=$?
            if [ "$rc16" -eq 0 ] && [ "$rc4" -eq 0 ] &&
               grep -q '== auxv layout: ok (pagesz 16384)' <<<"$out16" &&
               grep -q '== auxv layout: ok (pagesz 4096)' <<<"$out4"; then
                ok "ARM64_AUXV_LAYOUT: auxv pairs, AT_PAGESZ 16384 and 4096 (LXRT_GUEST_PAGE), 4 KiB library refused at 16 KiB and loaded at 4 KiB"
            else bad "ARM64_AUXV_LAYOUT" "rc=$rc16/$rc4 $(grep FAIL <<<"$out16"; grep FAIL <<<"$out4")"; fi
        else bad "4 KiB DSO precondition" "$(grep LOAD <<<"$headers") counter=$counter"; fi
        # FORK_LAZY_BIND: a fork child's first lazy binding under LXRT_NO_X18
        # with a preloaded library (wineserver's daemon under a Steam launch).
        if err=$(glibc_cc -O2 -o "$AUX_ROOT/tmp/fork_lazy_bind" tests/elf/fork_lazy_bind.c) &&
           err=$(glibc_cc -shared -fPIC -O2 -o "$AUX_ROOT/tmp/libpreload_dummy.so" tests/elf/preload_dummy.c); then
            # The Steam Frame root's ld.so is the one that keeps the scope
            # length in x18 (Fedora's does not): a private root with its ld.so
            # and libc when that root is on this Mac, the auxv root otherwise.
            FLB_ROOT="$AUX_ROOT" FLB_WHICH="Fedora ld.so"
            FRAME="${STEAMARM_STATE:-$HOME/SteamARM-roots}/arm64root"
            if [ -f "$FRAME/usr/lib/libc.so.6" ] && [ -e "$FRAME/usr/lib/ld-linux-aarch64.so.1" ]; then
                FLB_ROOT="$PWD/build/frame-ld-root"
                mkdir -p "$FLB_ROOT/lib" "$FLB_ROOT/usr/lib" "$FLB_ROOT/tmp" &&
                    cp -L "$FRAME/usr/lib/ld-linux-aarch64.so.1" "$FLB_ROOT/lib/" &&
                    cp -L "$FRAME/usr/lib/libc.so.6" "$FLB_ROOT/usr/lib/" &&
                    cp "$AUX_ROOT/tmp/fork_lazy_bind" "$AUX_ROOT/tmp/libpreload_dummy.so" "$FLB_ROOT/tmp/" &&
                    FLB_WHICH="Steam Frame ld.so"
            fi
            out=$(LXRT_ROOT="$FLB_ROOT" LXRT_NO_X18=1 LD_PRELOAD=/tmp/libpreload_dummy.so \
                  deadline 60 ./build/lxrun /tmp/fork_lazy_bind 50 2>&1); rc=$?
            if [ "$rc" -eq 0 ] && grep -q '^PASS' <<<"$out"; then
                ok "FORK_LAZY_BIND: 50 fork children bind setsid lazily under LXRT_NO_X18 with a preload ($FLB_WHICH)"
            else bad "FORK_LAZY_BIND" "rc=$rc $(grep -E 'MAL|undefined symbol' <<<"$out" | head -2)"; fi
        else bad "build fork_lazy_bind" "$err"; fi
        # DUAL_MAP_JIT: code written through a writable shared view of a file
        # runs from its read-execute shared view (GStreamer's ORC).
        if err=$(glibc_cc -O2 -o "$AUX_ROOT/tmp/dual_map_jit" tests/elf/dual_map_jit.c); then
            out=$(LXRT_ROOT="$AUX_ROOT" deadline 30 ./build/lxrun /tmp/dual_map_jit 2>&1); rc=$?
            if [ "$rc" -eq 0 ] && grep -q '^PASS' <<<"$out"; then
                ok "DUAL_MAP_JIT: code written through a shared RW view runs from the shared RX view (4 rounds)"
            else bad "DUAL_MAP_JIT" "rc=$rc $(grep -E 'round|FAIL|SIG' <<<"$out" | head -3 | tr '\n' ' ')"; fi
        else bad "build dual_map_jit" "$err"; fi
        # SHARED_VIEW_CYCLE: a section view shorter than a host page, released
        # by a PROT_NONE mapping over its length and mapped again, stays shared.
        if err=$(glibc_cc -O2 -o "$AUX_ROOT/tmp/shared_view_cycle" tests/elf/shared_view_cycle.c); then
            out=$(LXRT_ROOT="$AUX_ROOT" LXRT_GUEST_PAGE=4096 deadline 30 ./build/lxrun /tmp/shared_view_cycle 2>&1); rc=$?
            if [ "$rc" -eq 0 ] && grep -q '^PASS' <<<"$out"; then
                ok "SHARED_VIEW_CYCLE: 50 views of an 8 KiB memfd at one address, each released with PROT_NONE, all shared"
            else bad "SHARED_VIEW_CYCLE" "rc=$rc $(grep -E 'view|FAIL' <<<"$out" | head -3 | tr '\n' ' ')"; fi
        else bad "build shared_view_cycle" "$err"; fi
        # NETIF: getifaddrs(), if_nameindex() and the SIOCGIF* ioctls through
        # the runtime's NETLINK_ROUTE answers (Wine's nsiproxy hung without them).
        if err=$(glibc_cc -O2 -o "$AUX_ROOT/tmp/netif" tests/elf/netif.c); then
            out=$(LXRT_ROOT="$AUX_ROOT" deadline 30 ./build/lxrun /tmp/netif 2>&1); rc=$?
            if [ "$rc" -eq 0 ] && grep -q '^PASS' <<<"$out"; then
                ok "NETIF: $(grep '^getifaddrs' <<<"$out" | sed 's/^getifaddrs: //')"
            else bad "NETIF" "rc=$rc $(grep FAIL <<<"$out" | head -3 | tr '\n' ' ')"; fi
        else bad "build netif" "$err"; fi
        # TASK_STAT: another thread's CPU time in /proc/self/task/<tid>/stat,
        # which Wine's GetThreadTimes reads (zero for every thread without it).
        if err=$(glibc_cc -O2 -pthread -o "$AUX_ROOT/tmp/task_stat" tests/elf/task_stat.c); then
            out=$(LXRT_ROOT="$AUX_ROOT" deadline 30 ./build/lxrun /tmp/task_stat 2>&1); rc=$?
            if [ "$rc" -eq 0 ] && grep -q '^PASS' <<<"$out"; then
                ok "TASK_STAT: $(grep '^thread' <<<"$out")"
            else bad "TASK_STAT" "rc=$rc $(grep -E 'FAIL|thread' <<<"$out" | head -3 | tr '\n' ' ')"; fi
        else bad "build task_stat" "$err"; fi
        # A 4 KiB library whose code writes its own data from the host page
        # that holds both (the W/X livelock up to stage 22), dlopened at
        # LXRT_GUEST_PAGE=4096: plain stores, LL/SC loops as clang emits them
        # (the loaded register reused) and LSE atomics, four threads racing
        # on one counter through both. Precondition: the writers and the data
        # share a 16 KiB page.
        if err=$(glibc_cc -O2 -pthread -o "$AUX_ROOT/tmp/dlopen_self4k" tests/elf/dlopen_self4k.c) &&
           err=$(glibc_cc -shared -fPIC -O2 -march=armv8-a -mno-outline-atomics \
                          -Wl,-z,max-page-size=4096 -Wl,-z,common-page-size=4096 \
                          -o "$AUX_ROOT/tmp/libself4k.so" tests/elf/libself4k.c); then
            syms=$(/opt/homebrew/opt/llvm/bin/llvm-readelf -sW "$AUX_ROOT/tmp/libself4k.so")
            pages=$(awk '$8 ~ /^self4k_/ && $7 != "UND" { print $2 }' <<<"$syms" |
                    while read -r a; do echo $((16#$a >> 14)); done | sort -u)
            if [ -z "$pages" ] || [ "$(wc -l <<<"$pages" | tr -d ' ')" -ne 1 ]; then
                bad "4 KiB self-writing DSO precondition" "host pages: $(tr '\n' ' ' <<<"$pages")"
            else
                out=$(LXRT_SUBPAGE_LOG=100000 LXRT_ROOT="$AUX_ROOT" LXRT_GUEST_PAGE=4096 deadline 60 \
                      ./build/lxrun /tmp/dlopen_self4k /tmp/libself4k.so 2>&1); rc=$?
                n=$(grep -c 'subpage emulate' <<<"$out")
                if [ "$rc" -eq 0 ] && grep -q '== self4k: ok (16000 contended atomic adds)' <<<"$out" &&
                   [ "$n" -ge 17000 ] && ! grep -q 'cannot be emulated' <<<"$out"; then
                    ok "4 KiB DSO writing its own data from the same host page: stores, LL/SC and LSE atomics exact under 4-thread contention ($n emulated)"
                else
                    bad "4 KiB self-writing DSO" "rc=$rc (142: the W/X livelock) $n emulated $(grep -E 'FAIL|cannot be emulated' <<<"$out" | head -3)"
                fi
            fi
        else bad "build dlopen_self4k/libself4k" "$err"; fi
    else bad "build auxv_layout/libpg4k" "$err"; fi
else
    echo "  skip  ARM64_AUXV_LAYOUT (no $STAGE or $GUEST_ROOT/lib64/libc.so.6)"
fi

# X18_THREAD_ISOLATION: nine threads keep distinct values in x18 through
# 20000 raw syscalls each, with signals to one of them whose handler writes x18.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -pthread -o build/x18_threads tests/elf/x18_threads.c); then
        out=$(deadline 60 ./build/lxrun "$PWD/build/x18_threads" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== x18 threads: ok' <<<"$out"; then
            ok "X18_THREAD_ISOLATION: 9 threads x 20000 syscalls keep their own x18 (signals included)"
        else bad "X18_THREAD_ISOLATION" "rc=$rc $(grep -E 'FAIL|thread [0-9]+: bad=' <<<"$out")"; fi
    else bad "build x18_threads" "$err"; fi
else
    echo "  skip  X18_THREAD_ISOLATION (no $STAGE)"
fi

# X18_JUMP_TABLE (stage 28): a jump table dispatched through `br x18` keeps
# x16 and x17 live across the branch (V8's TurboFan does; the trampoline used
# to carry the target in x16), in 16 preempted threads; then both recoveries
# of the trampoline's hardware-x18 tail, forced from generated code.
# X18_JIT: generated code (never rewritten) keeps x18 across guest signal
# handlers only where the kernel keeps it -- the keep-x18 build, in the
# process lxrun was exec'd as. A forked child loses it on either build: xnu
# does not carry preserve_x18 across fork (tests/x18_preserve/run.sh).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -pthread -o build/x18_jumptable tests/elf/x18_jumptable.c); then
        out=$(LXRT_X18_STATS=1 deadline 120 ./build/lxrun "$PWD/build/x18_jumptable" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== x18_jumptable: ok' <<<"$out"; then
            ok "X18_JUMP_TABLE: br x18 keeps x16/x17 (16 threads x 3M dispatches); forced x18 = 0 in the trampoline recovered both ways ($(grep -o '[0-9]* restarts after a fault, [0-9]* branches to 0 recovered' <<<"$out"))"
        else bad "X18_JUMP_TABLE" "rc=$rc $(grep -E 'FAIL|thread [0-9]+:|SIG' <<<"$out" | head -4 | tr '\n' ' ')"; fi
    else bad "build x18_jumptable" "$err"; fi
    if err=$(glibc_cc -static-pie -O2 -o build/x18_jit_signal tests/elf/x18_jit_signal.c); then
        out=$(deadline 60 ./build/lxrun "$PWD/build/x18_jit_signal" 2>&1); rc=$?
        verdict=$(grep -o '== x18_jit: .*' <<<"$out")
        if kernel_keeps_x18; then
            if [ "$rc" -eq 0 ] && [ "$verdict" = "== x18_jit: parent kept, fork child lost" ]; then
                ok "X18_JIT: generated code keeps x18 through $(grep -o '[0-9]* signal handlers' <<<"$out") (lxrun sdk $LXRUN_SDK); a fork child loses it, as the kernel does"
            else bad "X18_JIT (lxrun sdk $LXRUN_SDK)" "rc=$rc '$verdict' $(grep -E 'lost|kept' <<<"$out" | tr '\n' ' ')"; fi
        elif [ "$verdict" = "== x18_jit: parent lost, fork child lost" ]; then
            xfail "X18_JIT: generated code loses x18 (lxrun sdk $LXRUN_SDK)" "the kernel zeroes x18 for a binary linked against SDK 13 or later (make lxrt LXRT_KEEP_X18=0); the default build keeps it"
        else bad "X18_JIT (lxrun sdk $LXRUN_SDK)" "rc=$rc '$verdict'"; fi
    else bad "build x18_jit_signal" "$err"; fi
else
    echo "  skip  X18_JUMP_TABLE, X18_JIT (no $STAGE)"
fi

# CEF_FILE_BACKED_MAPPING_FAST_PATH: a 4 KiB-offset file mapping whose 16 KiB
# interior is mapped straight from the file (subpage.c); LXRT_NO_FAST_SUBPAGE
# takes the copy path and must read the same bytes.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/fast_subpage tests/elf/fast_subpage.c); then
        out=$(deadline 30 ./build/lxrun --trace "$PWD/build/fast_subpage" 2>&1); rc=$?
        control=$(LXRT_NO_FAST_SUBPAGE=1 deadline 30 ./build/lxrun --trace "$PWD/build/fast_subpage" 2>&1); control_rc=$?
        if [ "$rc" -eq 0 ] && [ "$control_rc" -eq 0 ] &&
           grep -q '== fast subpage: ok' <<<"$out" &&
           grep -q 'subpage file interior mapped directly' <<<"$out" &&
           grep -q '== fast subpage: ok' <<<"$control" &&
           ! grep -q 'subpage file interior mapped directly' <<<"$control"; then
            ok "CEF_FILE_BACKED_MAPPING_FAST_PATH: 4 KiB-offset file mapping served from the file (copy path agrees)"
        else bad "CEF_FILE_BACKED_MAPPING_FAST_PATH" "rc=$rc/$control_rc $(grep -E 'FAIL|subpage file interior mapped directly' <<<"$out"; grep -E 'FAIL|subpage file interior mapped directly' <<<"$control")"; fi
    else bad "build fast_subpage" "$err"; fi
else
    echo "  skip  CEF_FILE_BACKED_MAPPING_FAST_PATH (no $STAGE)"
fi

# NEAR_CODE_TRAMPOLINE_ALLOCATION: a 144 MiB code segment mapped like ld.so
# maps libcef (reservation first, 4 KiB-aligned MAP_FIXED); the first slice's
# only reachable free space is the hole between two PT_LOADs.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/pool_near tests/elf/pool_near.c); then
        out=$(deadline 60 ./build/lxrun --trace "$PWD/build/pool_near" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== pool near: ok' <<<"$out" &&
           grep -q 'trampoline pool in ELF load gap' <<<"$out" &&
           grep -q 'sub-page code at 0x[0-9a-f]*: 2 svc sites, 2 rewritten, 0 poisoned' <<<"$out"; then
            ok "NEAR_CODE_TRAMPOLINE_ALLOCATION: pool in the ELF load gap, svc/TLS 144 MiB apart all rewritten"
        else bad "NEAR_CODE_TRAMPOLINE_ALLOCATION" "rc=$rc $(grep -E 'FAIL|ELF load gap|sub-page code|WARNING' <<<"$out")"; fi
    else bad "build pool_near" "$err"; fi
else
    echo "  skip  NEAR_CODE_TRAMPOLINE_ALLOCATION (no $STAGE)"
fi

# JIT_RWX_NATIVE: V8's read-write-execute code pages with no help from the
# guest (runtime/wxsplit.c): a 256 MiB reservation committed RWX in chunks,
# code rewritten and re-run, a second thread calling while this one writes
# (also into its own 16 KiB page), 8 threads on one page's first fetch, a
# live svc and an mrs tpidr_el0 in generated code (rewritten before they can
# run), RWX -> RX -> RWX, munmap + recommit, DONTNEED, PROT_NONE inside, a
# 4 KiB guard inside a committed host page, 5000 separate commits, a 4 KiB
# commit (subpage.c), V8's whole-range pattern, and a forked child.
# At the default 16 KiB and at LXRT_GUEST_PAGE=4096; LXRT_WX_SPLIT=0 (the old
# read-write grant) must fail on the first call.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -pthread -o build/jit_rwx tests/elf/jit_rwx.c); then
        out16=$(deadline 120 ./build/lxrun "$PWD/build/jit_rwx" 2>&1); rc16=$?
        out4=$(LXRT_GUEST_PAGE=4096 deadline 120 ./build/lxrun "$PWD/build/jit_rwx" 2>&1); rc4=$?
        control=$(LXRT_WX_SPLIT=0 deadline 30 ./build/lxrun "$PWD/build/jit_rwx" 2>&1); control_rc=$?
        n_svc=$(grep -c "JIT output: 1 svc sites rewritten" <<<"$out16")
        n_tls=$(grep -c "JIT output: 0 svc sites rewritten, 0 poisoned (W^X page 0x[0-9a-f]*: tls 1" <<<"$out16")
        if [ "$rc16" -eq 0 ] && [ "$rc4" -eq 0 ] &&
           grep -q '== jit_rwx: ok' <<<"$out16" && grep -q '== jit_rwx: ok' <<<"$out4" &&
           [ "$n_svc" -ge 2 ] && [ "$n_tls" -ge 1 ] &&
           [ "$control_rc" -ne 0 ] && ! grep -q '== jit_rwx: ok' <<<"$control"; then
            ok "JIT_RWX_NATIVE: $(grep -o '[0-9]* ok, 0 mal' <<<"$out16") at 16 and 4 KiB pages, svc/TLS in generated code rewritten before execution ($n_svc+$n_tls scans), old RW grant fails (rc=$control_rc)"
        else bad "JIT_RWX_NATIVE" "rc=$rc16/$rc4/control $control_rc svc=$n_svc tls=$n_tls $(grep -E 'MAL|memlog|SIG' <<<"$out16" | head -3; grep -E 'MAL|memlog|SIG' <<<"$out4" | head -3)"; fi
    else bad "build jit_rwx" "$err"; fi
else
    echo "  skip  JIT_RWX_NATIVE (no $STAGE)"
fi

# WX_ONE_OWNER: one owner and one scan for every split host page, and fault
# handling a guest signal handler cannot deadlock (tests/elf/wx_owner.c; the
# stage 23 review's sequences). A mixed page (W^X table + a 4 KiB RWX page
# only subpage.c knew), a 4 KiB guard over written code, RW -> RX sealed
# 4 KiB at a time, generated code beside a store the runtime performs, and a
# tgkill'd handler running split-page code: every svc must come back
# rewritten (Linux getppid, not a live Darwin getpid), at 16 and 4 KiB pages.
# LXRT_NO_RESCAN=1 is the control: the same svcs run live. SIGSEGV/SIGBUS
# ignored: the runtime's own flips still happen, a sent SIGSEGV is ignored, a
# real fault kills (Linux force_sig). No SIGBUS action: a misaligned
# store-release into a W^X page and a misaligned CASAL into a split page are
# reported (lxrun's SIGBUS, 138), not retried forever (142).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -pthread -o build/wx_owner tests/elf/wx_owner.c); then
        out16=$(deadline 60 ./build/lxrun "$PWD/build/wx_owner" 2>&1); rc16=$?
        out4=$(LXRT_GUEST_PAGE=4096 deadline 60 ./build/lxrun "$PWD/build/wx_owner" 2>&1); rc4=$?
        control=$(LXRT_NO_RESCAN=1 deadline 60 ./build/lxrun "$PWD/build/wx_owner" 2>&1); crc=$?
        n_live=$(grep -c 'MAL .*LIVE Darwin getpid' <<<"$control")
        ign=$(deadline 20 ./build/lxrun "$PWD/build/wx_owner" sigign 2>&1); irc=$?
        stlr=$(deadline 10 ./build/lxrun "$PWD/build/wx_owner" misaligned stlr 2>&1); src=$?
        casal=$(deadline 10 ./build/lxrun "$PWD/build/wx_owner" misaligned casal 2>&1); arc=$?
        if [ "$rc16" -eq 0 ] && [ "$rc4" -eq 0 ] &&
           grep -q '== wx_owner: ok' <<<"$out16" && grep -q '== wx_owner: ok' <<<"$out4" &&
           [ "$crc" -ne 0 ] && [ "$n_live" -ge 5 ] &&
           [ "$irc" -eq 0 ] && grep -q '== wx_owner sigign: ok' <<<"$ign" &&
           [ "$src" -eq 138 ] && grep -q 'SIGBUS at pc' <<<"$stlr" &&
           [ "$arc" -eq 138 ] && grep -q 'SIGBUS at pc' <<<"$casal"; then
            ok "WX_ONE_OWNER: $(grep -o '[0-9]* ok, 0 mal' <<<"$out16") at 16 and 4 KiB pages (control: $n_live live svcs), SIG_IGN flips + force_sig, misaligned stlr/casal reported"
        else bad "WX_ONE_OWNER" "rc=$rc16/$rc4 control=$crc live=$n_live sigign=$irc stlr=$src casal=$arc (142: retried forever) $(grep -E 'MAL|SIG' <<<"$out16" | head -3; grep -E 'MAL|SIG' <<<"$out4" | head -3; grep -E 'MAL' <<<"$ign" | head -2)"; fi
    else bad "build wx_owner" "$err"; fi
else
    echo "  skip  WX_ONE_OWNER (no $STAGE)"
fi

# MMAP_OFFSET_4K: file mappings at a 4 KiB offset that is not on a 16 KiB host
# page (runtime/offmap.c, dispatch.c do_mmap/do_munmap/do_madvise). munmap
# frees the head and tail host pages with the guest's last byte there (whole,
# piece by piece, 300 map/unmap cycles: /proc/self/maps no longer), a page
# mapped into the head's spare bytes survives, and MADV_DONTNEED keeps
# MAP_SHARED contents and reads a private file mapping back from the file
# instead of zeroing the partial host pages at either end.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/mmap_offset4k tests/elf/mmap_offset4k.c); then
        out4=$(LXRT_GUEST_PAGE=4096 deadline 60 ./build/lxrun "$PWD/build/mmap_offset4k" 2>&1); rc4=$?
        out16=$(deadline 60 ./build/lxrun "$PWD/build/mmap_offset4k" 2>&1); rc16=$?
        if [ "$rc4" -eq 0 ] && [ "$rc16" -eq 0 ] &&
           grep -q '== mmap offset4k: ok' <<<"$out4" && grep -q '== mmap offset4k: ok' <<<"$out16"; then
            ok "MMAP_OFFSET_4K: $(grep -o '[0-9]* ok, 0 mal' <<<"$out4") at 4 and 16 KiB pages; $(grep -o '[0-9]* lines before, [0-9]* after' <<<"$out4") in /proc/self/maps over 300 cycles"
        else bad "MMAP_OFFSET_4K" "rc=$rc4/$rc16 $(grep MAL <<<"$out4" | head -4; grep MAL <<<"$out16" | head -2)"; fi
    else bad "build mmap_offset4k" "$err"; fi
else
    echo "  skip  MMAP_OFFSET_4K (no $STAGE)"
fi

# WX_MPROTECT_RACE: an RWX range given RX, RW, R while other threads run code
# in it (runtime/dispatch.c wx_leave): no thread faults while the range is
# handed over, svc sites written before RX are rewritten before they run, and
# the protection asked for is the one in force afterwards. The stress mode
# (callers on the pages being written; storers under RWX -> RW) also counts
# faults a thread took while the split still held the page and whose handler
# ran after the hand-over: the handler waits for the hand-over and rechecks
# the page (runtime/wxsplit.c, stale_fault_retry); before that, tens of them
# per run reached the guest as SIGSEGV (kept below as an expected failure).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -pthread -o build/wx_mprotect_race tests/elf/wx_mprotect_race.c); then
        out=$(deadline 180 ./build/lxrun "$PWD/build/wx_mprotect_race" 2>&1); rc=$?
        out4=$(LXRT_GUEST_PAGE=4096 deadline 180 ./build/lxrun "$PWD/build/wx_mprotect_race" 2>&1); rc4=$?
        if [ "$rc" -eq 0 ] && [ "$rc4" -eq 0 ] &&
           grep -q '== wx_mprotect_race: ok' <<<"$out" && grep -q '== wx_mprotect_race: ok' <<<"$out4"; then
            ok "WX_MPROTECT_RACE: $(grep -o 'A\. [0-9]* x RWX -> RX -> RWX .*faults, 0 wrong of [0-9]* calls' <<<"$out"), at 16 and 4 KiB pages"
        else bad "WX_MPROTECT_RACE" "rc=$rc/$rc4 $(grep MAL <<<"$out" | head -3; grep MAL <<<"$out4" | head -2)"; fi
        st=$(deadline 180 ./build/lxrun "$PWD/build/wx_mprotect_race" stress 2>&1); src=$?
        counts=$(grep -o 'S1 [0-9]* faults, S2 [0-9]* faults' <<<"$st")
        s1=0
        [[ "$counts" =~ S1\ ([0-9]+) ]] && s1=${BASH_REMATCH[1]}
        if [ "$src" -eq 0 ]; then
            ok "WX_MPROTECT_RACE stress: $counts"
        elif [ "$src" -eq 2 ] && [ "$s1" -lt 10000 ]; then
            # (Forgetting the range before the rescan faulted callers for
            # the whole scan: ~500000 faults in S1.)
            xfail "WX_MPROTECT_RACE stress: $counts" "faults handled after the hand-over: wxsplit.c declines them without rechecking the page"
        else bad "WX_MPROTECT_RACE stress" "rc=$src $(grep -E 'S1|S2|MAL' <<<"$st" | head -4)"; fi
    else bad "build wx_mprotect_race" "$err"; fi
else
    echo "  skip  WX_MPROTECT_RACE (no $STAGE)"
fi

# ELECTRON_RUNTIME: what Heroic's Electron needed (benchmarks/stage24-heroic.txt):
# prlimit64 EFAULT on a read-only page, mprotect of a partial last page, SysV
# IPC_RMID deferred while attached, execve ENOENT before the exec, a dup of an
# epoll descriptor, and SOCK_SEQPACKET end of file for a blocked recvmsg/ppoll.
# At the host's 16 KiB page only: with LXRT_GUEST_PAGE=4096 a read-only 4 KiB
# page shares its host page with writable neighbours (runtime/subpage.c), so
# the kernel writes into it and the prlimit checks cannot pass there.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -pthread -o build/electron_runtime tests/elf/electron_runtime.c); then
        out=$(deadline 90 ./build/lxrun "$PWD/build/electron_runtime" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== electron runtime: ok' <<<"$out"; then
            ok "ELECTRON_RUNTIME: $(grep -o '([0-9]* checks)' <<<"$out" | tr -d '()')"
        else bad "ELECTRON_RUNTIME" "rc=$rc $(grep -E 'MAL|SIGBUS|SIGSEGV|lxrun:' <<<"$out" | head -6)"; fi
    else bad "build electron_runtime" "$err"; fi
else
    echo "  skip  ELECTRON_RUNTIME (no $STAGE)"
fi

# ANDROID_BIONIC_RT: what Android 11's bionic and ART needed, stage 25
# (benchmarks/stage25-android-userspace.txt): PR_SET_TAGGED_ADDR_CTRL refused
# (bionic tags its heap otherwise, and Darwin's syscalls reject the pointers),
# msync (ART probes free address space with it), mremap(MREMAP_FIXED) of 4 KiB
# pages into a reservation (bionic's CFI shadow) -- at the host page and with
# LXRT_GUEST_PAGE=4096, as Android runs; getrlimit (163), RLIM_INFINITY,
# /proc/self/stat's startstack and big unix datagrams (x86-64 ART under FEX,
# benchmarks/stage25-art-x86-fex.txt); then TPIDR_EL0 reads left in place
# inside a BoringSSL FIPS range and fixed up after context switches
# (runtime/tls.c), and the LXRT_TLS_KEEP=0 control that rewrites them.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -pthread -Wl,--export-dynamic -o build/android_bionic_rt tests/elf/android_bionic_rt.c); then
        for pg in host 4096; do
            if [ "$pg" = 4096 ]; then
                out=$(LXRT_GUEST_PAGE=4096 deadline 60 ./build/lxrun "$PWD/build/android_bionic_rt" 2>&1); rc=$?
            else
                out=$(deadline 60 ./build/lxrun "$PWD/build/android_bionic_rt" 2>&1); rc=$?
            fi
            if [ "$rc" -eq 0 ] && grep -q '== android bionic runtime: ok' <<<"$out"; then
                ok "ANDROID_BIONIC_RT ($pg pages): $(grep -c '^  ok ' <<<"$out") checks (tagged-address prctl, msync, 4 KiB mremap, clone TLS/CLONE_FILES, rt_tgsigqueueinfo, getrlimit/RLIM_INFINITY, startstack, 60 KiB unix datagrams)"
            else bad "ANDROID_BIONIC_RT ($pg pages)" "rc=$rc $(grep -E 'MAL|SIGBUS|SIGSEGV|lxrun:' <<<"$out" | head -6)"; fi
        done
        out=$(LXRT_TLS_KEEP_LOG=1 deadline 120 ./build/lxrun "$PWD/build/android_bionic_rt" tlskeep 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== android tlskeep: ok' <<<"$out"; then
            ok "kept TLS reads (BoringSSL FIPS range): word untouched, 0 mismatches over 100k loads, $(grep -o '[0-9]* kept-TLS faults fixed' <<<"$out" | head -1)"
        else bad "kept TLS reads" "rc=$rc $(grep -E 'MAL|SIG|lxrun:' <<<"$out" | head -6)"; fi
        out=$(LXRT_TLS_KEEP=0 deadline 120 ./build/lxrun "$PWD/build/android_bionic_rt" tlskeep 2>&1); rc=$?
        if [ "$rc" -ne 0 ] && grep -q 'MAL  the TPIDR_EL0 read in the kept range is untouched' <<<"$out"; then
            ok "LXRT_TLS_KEEP=0 control: the read is rewritten (the bytes a FIPS self-test hashes change)"
        else bad "LXRT_TLS_KEEP=0 control" "rc=$rc, expected the kept word to be rewritten"; fi
    else bad "build android_bionic_rt" "$err"; fi
else
    echo "  skip  ANDROID_BIONIC_RT (no $STAGE)"
fi

# ANDROID_IDS: Android ids (runtime/android_ids.h, benchmarks/stage27-android-
# framework.txt): with LXRT_ANDROID_IDS=root, what zygote does to become
# system_server -- unshare(CLONE_NEWNS), a bind mount, the bounding set,
# setgroups, keepcaps, setresgid/setresuid, capset, ambient caps -- by
# Linux's rules, then across fork (SO_PEERCRED of a child that dropped to
# another uid) and execve (ids, ambient caps, no_new_privs, the bind); and
# without it, the Mac's ids and nothing emulated.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/android_ids tests/elf/android_ids.c); then
        out=$(LXRT_ANDROID_IDS=root deadline 60 ./build/lxrun "$PWD/build/android_ids" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '^== android ids: ok' <<<"$out"; then
            ok "ANDROID_IDS: $(grep -c '^  ok ' <<<"$out") checks (setresuid/setresgid/setgroups, keepcaps, capset, ambient, bounding set, chown, unshare, bind mount, fork, execve, SO_PEERCRED)"
        else bad "ANDROID_IDS" "rc=$rc $(grep -E 'MAL|SIG|lxrun:' <<<"$out" | head -6)"; fi
        out=$(deadline 60 ./build/lxrun "$PWD/build/android_ids" off 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '^== android ids off: ok' <<<"$out"; then
            ok "ANDROID_IDS off: the Mac's ids, setresuid(0) EPERM, unshare ENOSYS"
        else bad "ANDROID_IDS off" "rc=$rc $(grep -E 'MAL|SIG|lxrun:' <<<"$out" | head -6)"; fi
    else bad "build android_ids" "$err"; fi
else
    echo "  skip  ANDROID_IDS (no $STAGE)"
fi

# ANDROID_BOOT_RT: what booting Android's framework to an app needed from the
# runtime (benchmarks/stage28-android-apk.txt): a futex deadline past 64-bit
# nanoseconds, the alarm timerfd clocks (CAP_WAKE_ALARM), SO_DOMAIN and
# SO_PROTOCOL, an empty SCM_RIGHTS and MSG_TRUNC on input, "user." xattrs,
# process_vm_readv/writev on itself. Without and with Android ids.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -pthread -o build/android_boot_rt tests/elf/android_boot_rt.c); then
        out=$(deadline 60 ./build/lxrun "$PWD/build/android_boot_rt" 2>&1); rc=$?
        out2=$(LXRT_ANDROID_IDS=root deadline 60 ./build/lxrun "$PWD/build/android_boot_rt" ids 2>&1); rc2=$?
        if [ "$rc" -eq 0 ] && [ "$rc2" -eq 0 ] && grep -q '^== android boot rt: ok' <<<"$out" &&
           grep -q '^== android boot rt: ok' <<<"$out2"; then
            ok "ANDROID_BOOT_RT: $(grep -c '^  ok ' <<<"$out2") checks with Android ids, $(grep -c '^  ok ' <<<"$out") without (futex forever, alarm timerfds, SO_DOMAIN, empty SCM_RIGHTS, user. xattrs, process_vm_readv)"
        else bad "ANDROID_BOOT_RT" "rc=$rc/$rc2 $(grep -hE 'MAL|SIG|lxrun:' <<<"$out$out2" | head -6)"; fi
    else bad "build android_boot_rt" "$err"; fi
else
    echo "  skip  ANDROID_BOOT_RT (no $STAGE)"
fi

# BINDER_IPC: Android binder between lxrun processes, raw ioctls against the
# Linux UAPI header (runtime/binder.c, runtime/binder_hub.c; benchmarks/
# stage25-binder.txt). A private hub directory, and a hub that leaves 2 s
# after its last client: nothing outlives the test.
if [ -f "$STAGE/usr/include/linux/android/binder.h" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -pthread -o build/binder_ipc tests/elf/binder_ipc.c); then
        bdir=$(mktemp -d /tmp/lxrt-binder-test.XXXXXX)
        out=$(LXRT_BINDER_DIR=$bdir LXRT_BINDER_HUB_IDLE=2 deadline 60 ./build/lxrun "$PWD/build/binder_ipc" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== binder ipc: ok' <<<"$out"; then
            ok "BINDER_IPC: $(grep -c '^  ok ' <<<"$out") checks across two processes (transaction/reply, FD, node refs, nested call, oneway order, SG+FDA, death notification, poll)"
        else bad "BINDER_IPC" "rc=$rc $(grep -E 'MAL|lxrun:' <<<"$out" | head -6)"; fi
        out=$(LXRT_BINDER_DIR=$bdir LXRT_BINDER_HUB_IDLE=2 deadline 60 ./build/lxrun "$PWD/build/binder_ipc" pool 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== binder pool: ok' <<<"$out"; then
            ok "BINDER_POOL: BR_SPAWN_LOOPER, two looper threads serving at once, EINTR restart, BINDER_THREAD_EXIT"
        else bad "BINDER_POOL" "rc=$rc $(grep -E 'MAL|lxrun:' <<<"$out" | head -6)"; fi
        sleep 3
        rm -rf "$bdir"
    else bad "build binder_ipc" "$err"; fi
else
    echo "  skip  BINDER_IPC (no $STAGE with linux/android/binder.h)"
fi

# BINDER_ONE_HUB: a hub started for a directory whose hub is alive does not
# take over its socket (it waits for hub.pid's lock, held for the live hub's
# whole life). Before, the second hub unlinked the first one's socket and
# bound its own: two binder drivers for one user.
bdir=$(mktemp -d /tmp/lxrt-binder-hubs.XXXXXX)
LXRT_BINDER_HUB_IDLE=2 ./build/lxrun --binder-hub "$bdir" --daemon; rc=$?
ino1=$(stat -f %i "$bdir/hub.sock" 2>/dev/null)
LXRT_BINDER_HUB_IDLE=2 ./build/lxrun --binder-hub "$bdir" --daemon & second=$!
sleep 1
ino2=$(stat -f %i "$bdir/hub.sock" 2>/dev/null)
kill "$second" 2>/dev/null; wait "$second" 2>/dev/null
if [ "$rc" -eq 0 ] && [ -n "$ino1" ] && [ "$ino1" = "$ino2" ]; then
    ok "BINDER_ONE_HUB: a second hub for a live hub's directory leaves its socket alone"
else bad "BINDER_ONE_HUB" "rc=$rc socket inode $ino1 -> $ino2"; fi
sleep 3
rm -rf "$bdir"

# SIMD_SYSCALL: a syscall preserves v0-v31, FPSR and NZCV, as on Linux
# (runtime/trampoline.S). Compilers keep values in vector registers and the
# flags across an inline `svc`; the dispatcher is Darwin C code that clobbers
# both. Freestanding: needs only Homebrew's llvm clang and lld.
if err=$(/opt/homebrew/opt/llvm/bin/clang --target=aarch64-linux-gnu -O2 -ffreestanding -fno-stack-protector \
             -fno-builtin -nostdlib -static-pie -fPIE -fuse-ld=lld --ld-path=$CROSS_LD \
             -o build/simd_syscall tests/elf/simd_syscall.c 2>&1); then
    out=$(deadline 60 ./build/lxrun build/simd_syscall 2>&1); rc=$?
    if [ "$rc" -eq 0 ] && grep -q '== simd_syscall: 3 ok, 0 mal' <<<"$out"; then
        ok "SIMD_SYSCALL: v0-v31 over 2000 syscall rounds, FPSR, all 16 NZCV values survive a syscall ($(grep -o 'getpid: .*' <<<"$out"))"
    else bad "SIMD_SYSCALL" "rc=$rc $(grep -E 'MAL|round|==' <<<"$out" | tr '\n' ' ')"; fi
else bad "build simd_syscall" "$err"; fi

# SIG_STRANDED: a process-directed signal that arrives while every thread
# blocks it stays pending for the process and is delivered once a thread
# unblocks it (runtime/signal.c, lxrt_signal_rescue_stranded). XNU binds it to
# the process's first thread -- the host main thread, which blocks everything
# -- and it stayed there: the intermittent `sh -c $(toybox ...)` hang under FEX
# (benchmarks/stage28-android-reliability.txt). Freestanding.
if err=$(/opt/homebrew/opt/llvm/bin/clang --target=aarch64-linux-gnu -O2 -ffreestanding -fno-stack-protector \
             -fno-builtin -nostdlib -static-pie -fPIE -fuse-ld=lld --ld-path=$CROSS_LD \
             -o build/sig_stranded tests/elf/sig_stranded.c 2>&1); then
    out=$(deadline 60 ./build/lxrun build/sig_stranded 2>&1); rc=$?
    if [ "$rc" -eq 0 ] && grep -q '== sig_stranded: 3 ok, 0 mal' <<<"$out"; then
        ok "SIG_STRANDED: SIGCHLD and a kill() that came while blocked are delivered on unblock and wake rt_sigsuspend"
    else bad "SIG_STRANDED" "rc=$rc $(grep -E 'MAL|==' <<<"$out" | tr '\n' ' ')"; fi
else bad "build sig_stranded" "$err"; fi

# SIGCHLD_SIGNALFD: a parent that blocks SIGCHLD (disposition SIG_DFL) and
# reads it from a signalfd, through epoll, ppoll, a blocking read, from a
# second thread, and with three children ending -- Steam's fossilize_replay
# master (benchmarks/stage51 section 10: the shader pre-cache never ended).
# Darwin drops a blocked SIGCHLD at SIG_DFL at post time and binds a kept one
# to the host main thread; the runtime keeps a host handler on every signal a
# signalfd watches and routes what reaches the main thread into the
# signalfd's shared queue (runtime/timerfd_signalfd.c, lxrt_signalfd_take).
# Freestanding.
if err=$(/opt/homebrew/opt/llvm/bin/clang --target=aarch64-linux-gnu -O2 -ffreestanding -fno-stack-protector \
             -fno-builtin -nostdlib -static-pie -fPIE -fuse-ld=lld --ld-path=$CROSS_LD \
             -o build/sigchld_signalfd tests/elf/sigchld_signalfd.c 2>&1); then
    out=$(deadline 60 ./build/lxrun build/sigchld_signalfd 2>&1); rc=$?
    if [ "$rc" -eq 0 ] && grep -q '== sigchld_signalfd: 6 ok, 0 mal' <<<"$out"; then
        ok "SIGCHLD_SIGNALFD: a blocked SIGCHLD at SIG_DFL reaches a signalfd (epoll, ppoll, blocking read, second thread, three children)"
    else bad "SIGCHLD_SIGNALFD" "rc=$rc $(grep -E 'MAL|==' <<<"$out" | tr '\n' ' ')"; fi
else bad "build sigchld_signalfd" "$err"; fi

# SHM_MREMAP: growing a MAP_SHARED file mapping maps more of the file
# (runtime/mremap.c, remap_shared_file): a Wayland compositor's wl_shm pool
# grown with MREMAP_MAYMOVE sees what the client writes past the old end.
# At 16 KiB and 4 KiB guest pages.
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/shm_mremap tests/elf/shm_mremap.c); then
        out16=$(deadline 60 ./build/lxrun "$PWD/build/shm_mremap" 2>&1); rc16=$?
        out4=$(LXRT_GUEST_PAGE=4096 deadline 60 ./build/lxrun "$PWD/build/shm_mremap" 2>&1); rc4=$?
        if [ "$rc16" -eq 0 ] && [ "$rc4" -eq 0 ] &&
           grep -q '== shm mremap: ok' <<<"$out16" && grep -q '== shm mremap: ok' <<<"$out4"; then
            ok "SHM_MREMAP: $(grep -c '^  ok ' <<<"$out16") checks at 16 and 4 KiB pages (MAYMOVE grow, file-backed past the old end, another process's stores, in place, shrink, FIXED)"
        else bad "SHM_MREMAP" "rc=$rc16/$rc4 $(grep -E 'MAL|lxrt\] mremap' <<<"$out16$out4" | head -4 | tr '\n' ' ')"; fi
    else bad "build shm_mremap" "$err"; fi
else
    echo "  skip  SHM_MREMAP (no $STAGE)"
fi

# PI_MUTEX: priority-inheritance mutexes (FUTEX_LOCK_PI / TRYLOCK_PI /
# UNLOCK_PI, runtime/futex_ops.c). Without them glibc fails every
# pthread_mutex_init(PTHREAD_PRIO_INHERIT) with ENOTSUP and no PipeWire client
# starts (wpctl, which Steam runs for its audio settings, died at 0x10).
if [ -f "$STAGE/usr/lib64/libc.a" ] && [ -n "$GCCDIR" ]; then
    if err=$(glibc_cc -static-pie -O2 -o build/pi_mutex tests/elf/pi_mutex.c -lpthread); then
        out=$(deadline 120 perl -e 'setpgrp(0,0); exec @ARGV' ./build/lxrun "$PWD/build/pi_mutex" 2>&1); rc=$?
        if [ "$rc" -eq 0 ] && grep -q '== pi mutex: ok' <<<"$out"; then
            ok "PI_MUTEX: contended, trylock, timedlock, EDEADLK, process-shared ($(grep -o 'ETIMEDOUT after [0-9]* ms' <<<"$out"))"
        else bad "PI_MUTEX" "rc=$rc $(grep -E 'MAL|->' <<<"$out" | head -3 | tr '\n' ' ')"; fi
    else bad "build pi_mutex" "$err"; fi
else
    echo "  skip  PI_MUTEX (no $STAGE)"
fi

echo
summary="== $PASS passed, $FAIL failed"
[ "$XFAIL" -eq 0 ] || summary="$summary ($XFAIL expected failures)"
echo "$summary"
[ "$FAIL" -eq 0 ]
