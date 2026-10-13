// getrusage(RUSAGE_THREAD) and getcpu, as Wine reads them.
//   - NtYieldExecution (SwitchToThread, Sleep(0)) brackets its sched_yield
//     with two getrusage(RUSAGE_THREAD) and reports the yield as performed
//     only when ru_nvcsw or ru_nivcsw moved. The runtime answered with the
//     process's counts, from a getrusage that walks every thread of the task
//     (5-6 us a yield with 130 threads): now this thread's times and the
//     switches the runtime saw it make (runtime/dispatch.c, thread_rusage).
//   - NtGetCurrentProcessorNumber is sched_getcpu, which is getcpu here
//     (rseq is ENOSYS): it answered CPU 0 for every thread.
//   - "server": wineserver's start -- init_threading's nice probe, then the
//     fork it serves from -- for run.sh, which runs this under the name
//     wineserver and reads the child's thread priority with ps.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what) { printf("  %s  %s\n", ok ? "OK " : "MAL", what); fails += !ok; }

static double mono(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double cputime(void)
{
    struct timespec t;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static double secs(struct timeval t) { return t.tv_sec + t.tv_usec / 1e6; }
static double used(const struct rusage *r) { return secs(r->ru_utime) + secs(r->ru_stime); }

// Every field written: the buffer starts as a pattern no field may keep.
static int ru(int who, struct rusage *r)
{
    memset(r, 0xa5, sizeof *r);
    return getrusage(who, r);
}

static int sane(const struct rusage *r)
{
    const long *f = (const long *)r;
    for (size_t i = 0; i < sizeof *r / sizeof(long); i++)
        if (f[i] < 0 || (unsigned long)f[i] == 0xa5a5a5a5a5a5a5a5ul)
            return 0;
    return r->ru_utime.tv_usec < 1000000 && r->ru_stime.tv_usec < 1000000;
}

// Spin until this thread has used `s` seconds of CPU.
static void burn(double s)
{
    double end = cputime() + s;
    volatile unsigned long x = 0;
    while (cputime() < end)
        for (int i = 0; i < 1000; i++) x++;
}

static atomic_int go, stop;
static _Atomic uint32_t word;

static void *burner(void *arg) { (void)arg; burn(0.1); atomic_store(&stop, 1); return NULL; }

static void *waker(void *arg)
{
    (void)arg;
    usleep(20000);
    atomic_store(&word, 1);
    syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    return NULL;
}

static void *spinner(void *arg)
{
    (void)arg;
    volatile unsigned long x = 0;
    while (!atomic_load_explicit(&stop, memory_order_relaxed)) x++;
    return NULL;
}

// getcpu sampled while every CPU has a thread spinning on it.
static int g_ncpu;
static _Atomic uint64_t seen;
static atomic_int bad_cpu, migrated;
static void *sampler(void *arg)
{
    (void)arg;
    while (!atomic_load(&go)) {}
    unsigned first = ~0u;
    int moved = 0;
    double end = mono() + 0.1;
    while (mono() < end) {
        unsigned cpu = ~0u;
        if (syscall(SYS_getcpu, &cpu, NULL, NULL) != 0 || cpu >= (unsigned)g_ncpu) {
            atomic_fetch_add(&bad_cpu, 1);
            continue;
        }
        atomic_fetch_or(&seen, 1ull << (cpu % 64));
        if (first == ~0u)
            first = cpu;
        moved |= cpu != first;
    }
    atomic_fetch_add(&migrated, moved);
    return NULL;
}

static int server(void)
{
    // init_threading: "if wineserver has cap_sys_nice we are unlimited".
    int a = setpriority(PRIO_PROCESS, getpid(), -20);
    int b = setpriority(PRIO_PROCESS, getpid(), 0);
    errno = 0;
    int n = getpriority(PRIO_PROCESS, 0);
    pid_t p = fork();               // open_master_socket: the child serves
    if (p < 0)
        return 1;
    if (p == 0) {
        printf("child %d nice %d after setpriority -20 -> %d, 0 -> %d\n", (int)getpid(), n, a, b);
        fflush(stdout);
        // Short naps: a timesharing thread's priority, decayed by the work
        // of the fork, is recomputed as it wakes.
        for (int i = 0; i < 15; i++) {
            struct timespec t = { 0, 100000000 };
            nanosleep(&t, NULL);
        }
        _exit(0);
    }
    int st = 0;
    waitpid(p, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "server"))
        return server();
    char msg[256];
    struct rusage t0, t1, p0, p1;

    // This thread's CPU time, not the process's.
    int r0 = ru(RUSAGE_THREAD, &t0);
    double c0 = cputime();
    burn(0.1);
    double c1 = cputime();
    int r1 = ru(RUSAGE_THREAD, &t1);
    double grew = used(&t1) - used(&t0);
    snprintf(msg, sizeof msg, "100 ms of this thread's CPU: ru_utime+ru_stime grew %.1f ms (CLOCK_THREAD_CPUTIME_ID %.1f ms)",
             grew * 1e3, (c1 - c0) * 1e3);
    check(!r0 && !r1 && sane(&t0) && sane(&t1) && grew >= 0.08 && grew <= 0.15, msg);
    check(secs(t1.ru_utime) > secs(t0.ru_utime), "ru_utime grew with a user-space loop");

    pthread_t th;
    ru(RUSAGE_THREAD, &t0);
    ru(RUSAGE_SELF, &p0);
    atomic_store(&stop, 0);
    pthread_create(&th, NULL, burner, NULL);
    while (!atomic_load(&stop)) {
        struct timespec ms = { 0, 1000000 };
        nanosleep(&ms, NULL);
    }
    pthread_join(th, NULL);
    ru(RUSAGE_THREAD, &t1);
    ru(RUSAGE_SELF, &p1);
    snprintf(msg, sizeof msg, "another thread's 100 ms: %.1f ms on this thread, %.1f ms on the process",
             (used(&t1) - used(&t0)) * 1e3, (used(&p1) - used(&p0)) * 1e3);
    check(used(&t1) - used(&t0) < 0.03 && used(&p1) - used(&p0) >= 0.08, msg);
    check(sane(&p1) && used(&p1) >= used(&t1) && t1.ru_maxrss > 0 && t1.ru_maxrss <= p1.ru_maxrss,
          "RUSAGE_SELF covers the thread; ru_maxrss is the process's (KiB)");

    // Voluntary switches: waits that slept.
    ru(RUSAGE_THREAD, &t0);
    struct timespec ms2 = { 0, 2000000 };
    nanosleep(&ms2, NULL);
    ru(RUSAGE_THREAD, &t1);
    snprintf(msg, sizeof msg, "nanosleep(2 ms): ru_nvcsw %ld -> %ld", t0.ru_nvcsw, t1.ru_nvcsw);
    check(t1.ru_nvcsw > t0.ru_nvcsw, msg);

    atomic_store(&word, 0);
    pthread_create(&th, NULL, waker, NULL);
    ru(RUSAGE_THREAD, &t0);
    while (atomic_load(&word) == 0)
        syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    ru(RUSAGE_THREAD, &t1);
    pthread_join(th, NULL);
    snprintf(msg, sizeof msg, "FUTEX_WAIT woken by another thread: ru_nvcsw %ld -> %ld, ru_nivcsw %ld -> %ld",
             t0.ru_nvcsw, t1.ru_nvcsw, t0.ru_nivcsw, t1.ru_nivcsw);
    check(t1.ru_nvcsw > t0.ru_nvcsw && t1.ru_nivcsw == t0.ru_nivcsw, msg);

    ru(RUSAGE_THREAD, &t0);
    long e = syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 12345, NULL, NULL, 0);
    int eagain = e < 0 && errno == EAGAIN;
    ru(RUSAGE_THREAD, &t1);
    check(eagain && t1.ru_nvcsw == t0.ru_nvcsw, "FUTEX_WAIT on a word that had changed (EAGAIN): no switch counted");

    // NtYieldExecution's test, alone and then with more threads spinning
    // than there are CPUs.
    cpu_set_t set;
    CPU_ZERO(&set);
    sched_getaffinity(0, sizeof set, &set);
    g_ncpu = CPU_COUNT(&set);
    int alone = 0, nv_moved = 0;
    double ya = mono();
    for (int i = 0; i < 2000; i++) {
        ru(RUSAGE_THREAD, &t0);
        sched_yield();
        ru(RUSAGE_THREAD, &t1);
        alone += t0.ru_nvcsw != t1.ru_nvcsw || t0.ru_nivcsw != t1.ru_nivcsw;
        nv_moved += t0.ru_nvcsw != t1.ru_nvcsw;
    }
    double t_yield = (mono() - ya) / 2000;
    int nspin = g_ncpu + 2, crowded = 0, tries = 0;
    pthread_t sp[130];
    if (nspin > 130) nspin = 130;
    atomic_store(&stop, 0);
    for (int i = 0; i < nspin; i++)
        pthread_create(&sp[i], NULL, spinner, NULL);
    double until = mono() + 0.3;
    while (mono() < until && crowded < 20) {
        ru(RUSAGE_THREAD, &t0);
        sched_yield();
        ru(RUSAGE_THREAD, &t1);
        tries++;
        crowded += t0.ru_nvcsw != t1.ru_nvcsw || t0.ru_nivcsw != t1.ru_nivcsw;
        nv_moved += t0.ru_nvcsw != t1.ru_nvcsw;
    }
    atomic_store(&stop, 1);
    for (int i = 0; i < nspin; i++)
        pthread_join(sp[i], NULL);
    snprintf(msg, sizeof msg, "yields that gave the CPU away: %d of 2000 alone, %d of %d with %d threads spinning on %d CPUs",
             alone, crowded, tries, nspin, g_ncpu);
    check(crowded > 0, msg);
    check(nv_moved == 0, "a yield is no voluntary switch: ru_nvcsw never moved across one");

    // getcpu.
    unsigned cpu = ~0u, node = ~0u;
    long gr = syscall(SYS_getcpu, &cpu, &node, NULL);
    int sg = sched_getcpu();
    snprintf(msg, sizeof msg, "getcpu: cpu %u node %u, sched_getcpu %d (%d CPUs)", cpu, node, sg, g_ncpu);
    check(gr == 0 && cpu < (unsigned)g_ncpu && node == 0 && sg >= 0 && sg < g_ncpu, msg);
    int nsam = g_ncpu < 64 ? g_ncpu : 64;
    pthread_t sa[64];
    atomic_store(&go, 0);
    for (int i = 0; i < nsam; i++)
        pthread_create(&sa[i], NULL, sampler, NULL);
    atomic_store(&go, 1);
    for (int i = 0; i < nsam; i++)
        pthread_join(sa[i], NULL);
    int distinct = __builtin_popcountll(atomic_load(&seen));
    snprintf(msg, sizeof msg, "%d threads sampling getcpu for 100 ms: %d distinct CPUs, %d threads seen to move, %d out of range",
             nsam, distinct, atomic_load(&migrated), atomic_load(&bad_cpu));
    check(atomic_load(&bad_cpu) == 0 && (g_ncpu < 2 || distinct >= 2), msg);

    printf("ntdll's yield (getrusage, sched_yield, getrusage): %.2f us\n", t_yield * 1e6);
    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
