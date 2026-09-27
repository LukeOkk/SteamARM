// Maps the runtime's vDSO (runtime/vdso/vdso.c) into the process and keeps its
// data page current. MIGRATION_PLAN M6: clock_gettime is ~70% of all guest
// syscalls; with the vDSO the common clocks are answered in guest user space.
//
// Layout: [vvar, 16 KiB, host-written][image, read-execute]. The image finds
// the vvar PC-relatively (vdso.lds puts __vvar 16 KiB below its base), so the
// two must stay adjacent. AT_SYSINFO_EHDR points at the image.
//
// CLOCK_MONOTONIC here and in lxrt_guest_clock_ns is the same integer
// arithmetic on mach_absolute_time(), so deadlines the guest computes from a
// vDSO read compare exactly against the runtime's clock. CLOCK_REALTIME is
// MONOTONIC plus an offset refreshed every 250 ms (and after fork), which
// tracks Darwin's slewed wall clock to well under a millisecond.
//
// LXRT_NO_VDSO=1 maps nothing and omits AT_SYSINFO_EHDR (glibc then traps).
#include "lxrt.h"

#include <mach/mach_time.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

extern const unsigned char lxrt_vdso_image[], lxrt_vdso_image_end[];

struct vvar {
    uint32_t seq;
    uint32_t enabled;
    uint32_t numer, denom;
    uint64_t mult;
    int64_t rt_offset_ns;
};

static volatile struct vvar *g_vvar;
static uint64_t g_ehdr;

// ns = ticks * mult >> 32: one multiply-high instead of a divide, and the
// exact formula the vDSO uses.
static uint64_t g_mult;
static uint64_t mono_mult(void)
{
    if (!g_mult) {
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        g_mult = (uint64_t)(((unsigned __int128)tb.numer << 32) / tb.denom);
    }
    return g_mult;
}

uint64_t lxrt_mono_ns(void)
{
    return (uint64_t)(((unsigned __int128)mach_absolute_time() * mono_mult()) >> 32);
}

void lxrt_vdso_refresh(void)
{
    if (!g_vvar)
        return;
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    uint64_t mono = lxrt_mono_ns();
    int64_t off = (int64_t)((uint64_t)rt.tv_sec * 1000000000ull + (uint64_t)rt.tv_nsec) - (int64_t)mono;
    __atomic_add_fetch(&g_vvar->seq, 1, __ATOMIC_ACQ_REL);   // odd: update in progress
    g_vvar->rt_offset_ns = off;
    __atomic_add_fetch(&g_vvar->seq, 1, __ATOMIC_RELEASE);
}

static void *refresher(void *arg)
{
    (void)arg;
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, NULL);   // never a target for guest signals
    for (;;) {
        usleep(250000);
        lxrt_vdso_refresh();
    }
    return NULL;
}

static void start_refresher(void)
{
    pthread_t t;
    if (pthread_create(&t, NULL, refresher, NULL) == 0)
        pthread_detach(t);
}

// fork() keeps only the calling thread: the child needs its own refresher,
// and a fresh offset right away.
static void after_fork_child(void)
{
    if (!g_vvar)
        return;
    lxrt_vdso_refresh();
    start_refresher();
}

uint64_t lxrt_vdso_setup(void)
{
    if (g_ehdr || getenv("LXRT_NO_VDSO"))
        return g_ehdr;
    size_t isz = (size_t)(lxrt_vdso_image_end - lxrt_vdso_image);
    size_t ipages = LXRT_ALIGN_UP(isz, LXRT_HOST_PAGE);
    size_t total = LXRT_HOST_PAGE + ipages;
    uint8_t *base = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (base == MAP_FAILED)
        return 0;
    uint8_t *img = base + LXRT_HOST_PAGE;
    memcpy(img, lxrt_vdso_image, isz);
    // Its fallback paths are `svc #0`, like any guest code.
    struct lxrt_rewrite_report rep;
    char *err = NULL;
    if (lxrt_rewrite_range((uint64_t)img, (uint64_t)img + isz, &rep, &err) != 0) {
        munmap(base, total);
        return 0;
    }
    if (mprotect(img, ipages, PROT_READ | PROT_EXEC) != 0) {
        munmap(base, total);
        return 0;
    }
    g_vvar = (volatile struct vvar *)base;
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_vvar->numer = tb.numer;
    g_vvar->denom = tb.denom;
    g_vvar->mult = mono_mult();
    // The commpage timebase type the image reads: 1..3 are user-readable
    // counters; anything else means "ask the kernel", so the image traps.
    uint8_t type = *(const volatile uint8_t *)0x0000000FFFFFC090ull;
    lxrt_vdso_refresh();
    g_vvar->enabled = (type >= 1 && type <= 3) ? 1 : 0;
    start_refresher();
    pthread_atfork(NULL, NULL, after_fork_child);
    g_ehdr = (uint64_t)img;
    return g_ehdr;
}

bool lxrt_vdso_contains(uint64_t addr)
{
    return g_ehdr && addr >= g_ehdr - LXRT_HOST_PAGE &&
           addr < g_ehdr + LXRT_ALIGN_UP((size_t)(lxrt_vdso_image_end - lxrt_vdso_image), LXRT_HOST_PAGE);
}

uint64_t lxrt_vdso_ehdr(void) { return g_ehdr; }
