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
#include <stdlib.h>
#include <stdio.h>

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
