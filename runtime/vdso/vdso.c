// The runtime's vDSO for aarch64 guests (MIGRATION_PLAN M6).
//
// clock_gettime is ~70% of all guest syscalls (benchmarks/stage1-syscall-mix):
// FEX's x86 guests reach it through FEX's own VDSO emulation, which calls the
// host glibc, which without AT_SYSINFO_EHDR traps into the runtime every time.
// This image answers the common clocks in user space, the same way Darwin's
// own mach_absolute_time() does: the commpage says which counter register to
// read and what offset to add. The runtime fills the data page ("vvar",
// mapped just below this image) with the timebase and a realtime offset it
// refreshes; anything not handled here falls back to the real syscall.
//
// Built as a Linux aarch64 shared object (vdso.lds), embedded in lxrun
// (vdso_blob.S), mapped per process by runtime/vdso_map.c. Must not touch x18.
#include <stdint.h>

struct lx_timespec { int64_t tv_sec; int64_t tv_nsec; };
struct lx_timeval { int64_t tv_sec; int64_t tv_usec; };

struct vvar {
    uint32_t seq;           // odd while the host updates rt_offset_ns
    uint32_t enabled;       // 0: every call takes the syscall
    uint32_t numer, denom;  // mach timebase (informational)
    uint64_t mult;          // ns = ticks * mult >> 32 (the runtime uses the same)
    int64_t rt_offset_ns;   // CLOCK_REALTIME - CLOCK_MONOTONIC
};

extern const volatile struct vvar __vvar __attribute__((visibility("hidden")));   // vdso.lds: 16 KiB below

#define COMM_TIMEBASE_OFFSET 0x0000000FFFFFC088ull
#define COMM_TIMEBASE_TYPE   0x0000000FFFFFC090ull

static inline int64_t sys2(long nr, long a, long b)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1) : "memory");
    return x0;
}

// mach_absolute_time(), as libsystem_kernel does it. 0 = no user timebase.
static inline int abs_ticks(uint64_t *out)
{
    const volatile uint64_t *off = (const volatile uint64_t *)COMM_TIMEBASE_OFFSET;
    uint8_t type = *(const volatile uint8_t *)COMM_TIMEBASE_TYPE;
    uint64_t o1, o2, t;
    do {
        o1 = *off;
        switch (type) {
        case 1: __asm__ volatile("isb\n\tmrs %0, cntvct_el0" : "=r"(t) :: "memory"); break;
        case 2: __asm__ volatile("mrs %0, s3_3_c14_c0_6" : "=r"(t) :: "memory"); break;    // CNTVCTSS_EL0
        case 3: __asm__ volatile("mrs %0, s3_4_c15_c10_6" : "=r"(t) :: "memory"); break;   // ACNTVCT_EL0
        default: return 0;
        }
        o2 = *off;
    } while (o1 != o2);
    *out = t + o1;
    return 1;
}

static inline int mono_ns(uint64_t *ns)
{
    uint64_t t;
    if (!__vvar.enabled || !abs_ticks(&t))
        return 0;
    *ns = (uint64_t)(((unsigned __int128)t * __vvar.mult) >> 32);
    return 1;
}

static inline int real_ns(uint64_t *ns)
{
    uint64_t m;
    uint32_t s1, s2;
    int64_t off;
    do {
        s1 = __atomic_load_n(&__vvar.seq, __ATOMIC_ACQUIRE);
        if (s1 & 1)
            continue;
        if (!mono_ns(&m))
            return 0;
        off = __vvar.rt_offset_ns;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s2 = __atomic_load_n(&__vvar.seq, __ATOMIC_RELAXED);
    } while ((s1 & 1) || s1 != s2);
    *ns = m + (uint64_t)off;
    return 1;
}

int __kernel_clock_gettime(long clk, struct lx_timespec *ts)
{
    uint64_t ns;
    int ok = 0;
    // MONOTONIC, MONOTONIC_RAW, MONOTONIC_COARSE: the runtime's own
    // definition (mach_absolute_time scaled), so futex/timerfd deadlines agree.
    if (clk == 1 || clk == 4 || clk == 6)
        ok = mono_ns(&ns);
    else if (clk == 0 || clk == 5)
        ok = real_ns(&ns);
    if (!ok)
        return (int)sys2(113, clk, (long)ts);
    ts->tv_sec = (int64_t)(ns / 1000000000ull);
    ts->tv_nsec = (int64_t)(ns % 1000000000ull);
    return 0;
}

int __kernel_gettimeofday(struct lx_timeval *tv, void *tz)
{
    uint64_t ns;
    if (tz || !real_ns(&ns))
        return (int)sys2(169, (long)tv, (long)tz);
    if (tv) {
        tv->tv_sec = (int64_t)(ns / 1000000000ull);
        tv->tv_usec = (int64_t)(ns % 1000000000ull / 1000);
    }
    return 0;
}

int __kernel_clock_getres(long clk, struct lx_timespec *res)
{
    if ((clk == 0 || clk == 1 || clk == 4) && __vvar.enabled) {
        if (res) { res->tv_sec = 0; res->tv_nsec = 1; }
        return 0;
    }
    return (int)sys2(114, clk, (long)res);
}
