// LXRT_GUESTPROF=1: where the process's first guest thread spends its time,
// sampled a thousand times a second and reported every ten seconds.
//
// macOS's sample(1) sees a FEX guest as "??? (in <unknown binary>)": the
// translated code has no image. What it is translated FROM is in FEX's CPU
// state (x28 at every syscall the thread makes; its rip is kept current to
// the block), and the x86 library that address belongs to says whether a
// game's main thread is simulating, building draw calls, or spinning while
// it waits for its render thread -- which the host's view cannot tell apart
// (Counter-Strike 2: that thread "86 % busy" with the GPU at 99 %,
// benchmarks/stage51).
//
// A sample is: suspend the thread, read pc, resume; then name the pc. Host
// code is named by its image (and by symbol in libsystem_kernel, where the
// waits are); FEX's own code (compiling, dispatching) by "fex"; translated
// code by the guest file mapped at the state's rip.

#include "lxrt.h"

#include <dlfcn.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int lxrt_guestprof_on;
static pthread_t g_target;
static mach_port_t g_target_port;
static _Atomic uint64_t g_state;     // the target's FEX CPU state, 0 for a native guest

#define KEYS 192
static struct { char name[72]; unsigned n; } g_keys[KEYS];
static unsigned g_nkeys, g_total;

static void count(const char *name)
{
    g_total++;
    for (unsigned i = 0; i < g_nkeys; i++)
        if (!strcmp(g_keys[i].name, name)) {
            g_keys[i].n++;
            return;
        }
    if (g_nkeys < KEYS) {
        snprintf(g_keys[g_nkeys].name, sizeof g_keys[0].name, "%s", name);
        g_keys[g_nkeys++].n = 1;
    }
}

// The file mapped at addr, "" if the memory there is anonymous.
// proc_regionfilename answers for the NEXT region when addr is in none, or
// in an anonymous one before a file's: the region's own bounds decide.
static bool file_at(uint64_t addr, char *path, size_t n)
{
    struct proc_regionwithpathinfo ri;
    path[0] = 0;
    if (proc_pidinfo(getpid(), PROC_PIDREGIONPATHINFO, addr, &ri, sizeof ri) != (int)sizeof ri)
        return false;
    if (addr < ri.prp_prinfo.pri_address || addr >= ri.prp_prinfo.pri_address + ri.prp_prinfo.pri_size ||
        !ri.prp_vip.vip_path[0])
        return false;
    snprintf(path, n, "%s", ri.prp_vip.vip_path);
    return true;
}

static const char *base_of(const char *path)
{
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

static void report(void)
{
    char line[1600];
    int n = snprintf(line, sizeof line, "[lxrt] guestprof pid %d, %u samples:", (int)getpid(), g_total);
    for (int k = 0; k < 16 && n < (int)sizeof line - 96; k++) {
        unsigned best = 0;
        for (unsigned i = 1; i < g_nkeys; i++)
            if (g_keys[i].n > g_keys[best].n)
                best = i;
        if (!g_nkeys || !g_keys[best].n)
            break;
        n += snprintf(line + n, sizeof line - (size_t)n, " %s=%.1f%%", g_keys[best].name,
                      100.0 * g_keys[best].n / (g_total ? g_total : 1));
        g_keys[best].n = 0;
    }
    fprintf(lxrt_trace_stream(), "%s\n", line);
    g_nkeys = g_total = 0;
}

static void *sampler(void *unused)
{
    (void)unused;
    pthread_setname_np("lxrt-guestprof");
    uint64_t last = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    for (;;) {
        usleep(1000);
        arm_thread_state64_t ts;
        mach_msg_type_number_t cnt = ARM_THREAD_STATE64_COUNT;
        if (thread_suspend(g_target_port) != KERN_SUCCESS)
            return NULL;
        kern_return_t kr = thread_get_state(g_target_port, ARM_THREAD_STATE64, (thread_state_t)&ts, &cnt);
        thread_resume(g_target_port);
        if (kr != KERN_SUCCESS)
            return NULL;
        uint64_t pc = arm_thread_state64_get_pc(ts);
        char name[96], path[PROC_PIDPATHINFO_MAXSIZE];
        Dl_info di;
        if (dladdr((void *)(uintptr_t)pc, &di) && di.dli_fname) {
            const char *b = base_of(di.dli_fname);
            if (!strcmp(b, "libsystem_kernel.dylib") && di.dli_sname)
                snprintf(name, sizeof name, "host:%s", di.dli_sname);
            else
                snprintf(name, sizeof name, "host:%s", b);
        } else if (file_at(pc, path, sizeof path)) {
            // A guest image executing as itself: FEX (an aarch64 ELF), or a
            // native guest's own code.
            snprintf(name, sizeof name, "guest:%s", base_of(path));
        } else {
            // Translated code. Which x86 code: the state's rip.
            uint64_t st = atomic_load_explicit(&g_state, memory_order_relaxed), rip = 0;
            mach_vm_size_t got = 0;
            if (st && mach_vm_read_overwrite(mach_task_self(), st + 24, 8, (mach_vm_address_t)(uintptr_t)&rip, &got) ==
                          KERN_SUCCESS &&
                rip && file_at(rip, path, sizeof path))
                snprintf(name, sizeof name, "x86:%s", base_of(path));
            else
                snprintf(name, sizeof name, st ? "x86:(anonymous)" : "jit");
        }
        count(name);
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (now - last > 10000000000ull) {
            report();
            last = now;
        }
    }
}

// The first guest thread, before it runs any guest code.
void lxrt_guestprof_start(void)
{
    const char *e = getenv("LXRT_GUESTPROF");
    if (!e || *e != '1')
        return;
    g_target = pthread_self();
    g_target_port = pthread_mach_thread_np(g_target);
    pthread_t th;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&th, &at, sampler, NULL) == 0)
        lxrt_guestprof_on = 1;
}

// From the dispatcher: FEX's CPU state is x28 at a syscall.
void lxrt_guestprof_note(uint64_t x28)
{
    if (pthread_equal(pthread_self(), g_target) && x28 > (1ull << 32))
        atomic_store_explicit(&g_state, x28, memory_order_relaxed);
}
