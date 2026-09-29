// Guest thread-local storage.
//
// Linux aarch64 keeps the thread pointer in TPIDR_EL0. Darwin does not preserve
// that register: a syscall leaves it alone, but any preemption or block
// restores a kernel-managed value (benchmarks/stage3-tls.txt). A guest that
// ran long enough to be descheduled would silently lose its TLS -- which, like
// a missed `svc`, is corruption rather than a crash.
//
// The replacement is Darwin's own TSD slot array, the same one
// pthread_getspecific indexes off TPIDRRO_EL0. It is preserved per thread and
// costs 2.34 ns to read with three instructions and no scratch register.
// rewrite.c turns every `mrs Xt, TPIDR_EL0` and `msr TPIDR_EL0, Xt` in guest
// code into a branch to a trampoline built around that sequence.

#include "lxrt.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/ucontext.h>

static pthread_key_t g_key;
static unsigned long g_slot_offset;
static bool g_ready;
// The virtual x18: same mechanism, second slot. Darwin zeroes the real x18
// on every exception return (benchmarks/stage5-x18.txt); Linux code keeps
// temporaries in it. runtime/x18.c plans the per-site trampolines.
static pthread_key_t g_x18_key;
static unsigned long g_x18_offset;
static bool g_x18_ready;

int lxrt_tls_init(void)
{
    if (g_ready)
        return 0;
    // An escape hatch purely for demonstrating what the rewriting prevents:
    // with it set, guest TLS stays in TPIDR_EL0 and is lost the first time the
    // thread is descheduled. tests/elf/run.sh uses it to prove the failure is
    // real rather than asserting it.
    if (getenv("LXRT_NO_TLS_REWRITE")) {
        fprintf(lxrt_trace_stream(), "[lxrt] LXRT_NO_TLS_REWRITE set: leaving TPIDR_EL0 in "
                        "place. Guest TLS will break on the first context "
                        "switch.\n");
        return 0;
    }
    if (pthread_key_create(&g_key, NULL) != 0)
        return -1;
    g_slot_offset = (unsigned long)g_key * 8;
    // The rewritten load is `ldr Xt, [Xt, #imm12*8]`, so the slot has to be
    // within the unsigned scaled range. Darwin hands out low key numbers, but
    // check rather than emit a silently wrong instruction.
    if (g_slot_offset % 8 != 0 || g_slot_offset / 8 > 4095) {
        fprintf(lxrt_trace_stream(), "[lxrt] TSD key %lu is out of `ldr` immediate range\n",
                (unsigned long)g_key);
        return -1;
    }
    g_ready = true;
    if (!getenv("LXRT_NO_X18") && pthread_key_create(&g_x18_key, NULL) == 0) {
        g_x18_offset = (unsigned long)g_x18_key * 8;
        if (g_x18_offset % 8 == 0 && g_x18_offset / 8 <= 4095)
            g_x18_ready = true;
    }
    if (!g_x18_ready)
        fprintf(lxrt_trace_stream(), "[lxrt] x18 virtualisation OFF%s: guest code that keeps "
                        "values in x18 will lose them on the next context switch\n",
                getenv("LXRT_NO_X18") ? " (LXRT_NO_X18)" : "");
    return 0;
}

unsigned long lxrt_x18_slot_offset(void) { return g_x18_offset; }
bool lxrt_x18_enabled(void) { return g_x18_ready; }
uint64_t lxrt_x18_get(void)
{
    return g_x18_ready ? (uint64_t)(uintptr_t)pthread_getspecific(g_x18_key) : 0;
}
void lxrt_x18_set(uint64_t v)
{
    if (g_x18_ready) pthread_setspecific(g_x18_key, (void *)(uintptr_t)v);
}

unsigned long lxrt_tls_slot_offset(void) { return g_slot_offset; }
bool lxrt_tls_ready(void) { return g_ready; }

// Used by the runtime itself; guest code reaches the same slot through the
// rewritten instructions, not through here.
void lxrt_tls_set(uint64_t v) { pthread_setspecific(g_key, (void *)(uintptr_t)v); }
uint64_t lxrt_tls_get(void)
{
    return (uint64_t)(uintptr_t)pthread_getspecific(g_key);
}

// ---------------------------------------------------------------------------
// Kept TLS reads.
//
// BoringSSL's FIPS module (Android's libcrypto.so) hashes its own .text at
// load time and aborts when the HMAC differs from the one baked into the
// file ("FIPS integrity test failed", BORINGSSL_bcm_power_on_self_test). The
// module's code reads the thread pointer in every stack-protector prologue
// (`mrs Xn, TPIDR_EL0; ldr Xm, [Xn, #0x28]`, bionic's TLS_SLOT_STACK_GUARD),
// and rewriting those reads is what changed the bytes: every Android program
// that links libcrypto (toybox) died before main (MEASURED,
// benchmarks/stage25-android-userspace.txt). Recomputing the baked HMAC would
// make the self-test pass on code it is meant to reject, so the runtime
// leaves the module's bytes alone instead:
//
//   * elfsect.c registers [BORINGSSL_bcm_text_start, BORINGSSL_bcm_text_end)
//     here when it maps such a library; rewrite.c then leaves the TLS READS
//     in that range as they are (every other site kind is still rewritten,
//     and said -- the module has none: MEASURED 0 svc, 0 x18, 0 system
//     register sites in Android 11's libcrypto.so);
//   * such a read returns the hardware TPIDR_EL0, which belongs to Darwin
//     (0, 2, 5, 0x1007, 0x1000000000001 MEASURED, benchmarks/stage3-tls.txt
//     and stage 25). None of those is a mapped address -- the low 4 GiB is
//     __PAGEZERO, and bit 48 is outside the user address space -- so the
//     first load or store through it faults;
//   * lxrt_tlskeep_fixup, early in the fault path, recognises that fault (pc
//     in a kept range, a load/store whose base register holds such a value),
//     replaces the base register with the guest's thread pointer and retries
//     the instruction. TPIDR_EL0 is not written: Darwin's allocator uses it
//     (below), so each kept read that is dereferenced costs one fault.
//
// A copy of the bad value spilled to the stack faults and is fixed the same
// way when it is reloaded and used. What this cannot fix: a Darwin value
// used for something other than an address. In the FIPS module every read
// feeds a stack-protector load (112 of 119) or a spill of the pointer (7).
// LXRT_TLS_KEEP=0 turns the whole mechanism off (the reads are rewritten,
// and the module's self-test fails as before).
#define TLSKEEP_MAX 32
static struct { _Atomic uint64_t start, end; } g_keep[TLSKEEP_MAX];
static _Atomic int g_nkeep;
static atomic_ulong g_keep_fixups;

bool lxrt_tlskeep_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_TLS_KEEP");
        on = !(e && strcmp(e, "0") == 0);
    }
    return on && g_ready;
}

void lxrt_tlskeep_add(uint64_t start, uint64_t end, const char *why)
{
    if (!lxrt_tlskeep_enabled() || end <= start)
        return;
    int n = atomic_load(&g_nkeep);
    for (int i = 0; i < n; i++)
        if (atomic_load(&g_keep[i].start) == start && atomic_load(&g_keep[i].end) == end)
            return;
    // A slot whose range was unmapped is not reclaimed: TLSKEEP_MAX modules
    // per process is far more than a process loads.
    int i = atomic_fetch_add(&g_nkeep, 1);
    if (i >= TLSKEEP_MAX) {
        atomic_store(&g_nkeep, TLSKEEP_MAX);
        fprintf(lxrt_trace_stream(), "[lxrt] tls keep: table full, %s at 0x%llx is rewritten\n",
                why ? why : "range", (unsigned long long)start);
        return;
    }
    atomic_store(&g_keep[i].end, end);
    atomic_store(&g_keep[i].start, start);
    if (getenv("LXRT_TRACE") || getenv("LXRT_TLS_KEEP_LOG"))
        fprintf(lxrt_trace_stream(), "[lxrt] tls keep: %s 0x%llx-0x%llx: TLS reads left in place\n",
                why ? why : "range", (unsigned long long)start, (unsigned long long)end);
}

bool lxrt_tlskeep_contains(uint64_t addr)
{
    int n = atomic_load(&g_nkeep);
    for (int i = 0; i < n && i < TLSKEEP_MAX; i++) {
        uint64_t s = atomic_load(&g_keep[i].start);
        if (s && addr >= s && addr < atomic_load(&g_keep[i].end))
            return true;
    }
    return false;
}

unsigned long lxrt_tlskeep_fixups(void) { return atomic_load(&g_keep_fixups); }

bool lxrt_tlskeep_fixup(int sig, void *uap)
{
    if ((sig != SIGSEGV && sig != SIGBUS) || !uap || !atomic_load(&g_nkeep))
        return false;
    ucontext_t *uc = (ucontext_t *)uap;
    _STRUCT_ARM_THREAD_STATE64 *ts = &uc->uc_mcontext->__ss;
    uint64_t pc = ts->__pc;
    if (!lxrt_tlskeep_contains(pc))
        return false;
    uint32_t w = *(const uint32_t *)(uintptr_t)pc;
    // The load/store encoding group (op0 = x1x0); Rn is bits 9:5 there, and
    // 31 is sp, never a thread pointer.
    if ((w & 0x0a000000u) != 0x08000000u)
        return false;
    unsigned rn = (w >> 5) & 31u;
    if (rn == 31)
        return false;
    uint64_t *reg = rn == 29 ? &ts->__fp : rn == 30 ? &ts->__lr : &ts->__x[rn];
    uint64_t v = *reg;
    // Only what Darwin leaves in TPIDR_EL0: a real pointer (a genuine fault
    // of the program's own) goes to the guest untouched.
    if (v >= (1ull << 32) && v < (1ull << 48))
        return false;
    uint64_t tp = lxrt_tls_get();
    if (!tp || v == tp)
        return false;
    *reg = tp;
    // TPIDR_EL0 itself is left to Darwin: its xzone malloc keeps its own
    // per-thread value there, and writing the guest's pointer into it made
    // the runtime's next malloc fault (MEASURED, stage 25, with an lxrun
    // built against the current SDK; the 12.3-SDK build gets the older
    // allocator and did not care). So every kept read faults once, not once
    // per context switch.
    atomic_fetch_add(&g_keep_fixups, 1);
    return true;
}
