// A native JIT's read-write-execute code range given another protection
// while other threads use it (runtime/dispatch.c wx_leave, runtime/wxsplit.c).
//
// On Linux an mprotect never faults an access that both the old and the new
// protection allow. Here an RWX range is split W^X page by page, and the
// split's fault handler has to own every page until the range is handed
// over: before, the range was forgotten first and then opened read-write for
// the exec rescan, and a thread running code in it took an unclaimed fault
// (SIGSEGV) in between. Checked by default:
//   A. RWX -> RX -> RWX, 300 rounds over 4 MiB while 3 threads call code on
//      every other page and this thread writes code (a svc among it) into the
//      pages between, which are read-write when RX is asked: no thread
//      faults, every call is right, and every svc returns getpid();
//   C. the protection asked for is the one in force afterwards: RX refuses a
//      store, RW refuses a fetch, R refuses both, and RWX runs code again;
//   D. RX over a range the split holds only half of (the rest ordinary RW
//      memory): svc sites in both halves are rewritten before they run.
// With the argument "stress" (exit 0: no fault, 2: faults; counts printed):
//   S1. as A, but the callers also run the pages being written;
//   S2. RWX -> RW -> RWX, 300 rounds while 3 threads store into every page
//       and this thread runs code on every page before each RW.
// There a thread can fault on a page the split still holds and wait for the
// split's lock while the range is handed over; its handler then finds the
// range gone and declines a fault the page no longer has. Only the handler
// can tell that (runtime/wxsplit.c), so S1/S2 measure it rather than judge.
// Linux aarch64 passes all of it.
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define KB 1024ul
#define MB (1024 * KB)
#define PG (16 * KB)
#define RANGE (4 * MB)
#define NPAGES (RANGE / PG)
#define NTHREADS 3
#define ROUNDS 300

static int ok, bad;
#define CHECK(c, ...) do { if (c) { ok++; printf("  OK   " __VA_ARGS__); } \
                           else { bad++; printf("  MAL  " __VA_ARGS__); } putchar('\n'); fflush(stdout); } while (0)

static const uint32_t RET = 0xd65f03c0;
static uint32_t movz_w0(unsigned v) { return 0x52800000u | ((v & 0xffffu) << 5); }
typedef long (*fn_t)(void);

static fn_t emit_const(uint32_t *p, unsigned v)
{
    p[0] = movz_w0(v);
    p[1] = RET;
    __builtin___clear_cache((char *)p, (char *)(p + 2));
    return (fn_t)(uintptr_t)p;
}

// mov x8,#172 (getpid); svc #0; ret
static fn_t emit_getpid(uint32_t *p)
{
    p[0] = 0xd2801588;
    p[1] = 0xd4000001;
    p[2] = RET;
    __builtin___clear_cache((char *)p, (char *)(p + 3));
    return (fn_t)(uintptr_t)p;
}

// Per-thread fault recovery: a fault while armed jumps back and is counted.
static __thread sigjmp_buf t_jb;
static __thread volatile int t_armed;
static void on_fault(int sig)
{
    if (t_armed)
        siglongjmp(t_jb, 1);
    static const char m[] = "wx_mprotect_race: unexpected fault\n";
    (void)!write(2, m, sizeof m - 1);
    _exit(99);
}

// Does calling fn fault? Storing into (write) or reading p?
static int call_faults(fn_t fn)
{
    int f = 0;
    t_armed = 1;
    if (sigsetjmp(t_jb, 1) == 0) (void)fn();
    else f = 1;
    t_armed = 0;
    return f;
}
static int access_faults(volatile uint32_t *p, int write)
{
    int f = 0;
    t_armed = 1;
    if (sigsetjmp(t_jb, 1) == 0) { if (write) *p = 0x12345678; else (void)*p; }
    else f = 1;
    t_armed = 0;
    return f;
}

static char *R;
static atomic_int g_stop, g_all_pages;
static atomic_long g_calls, g_wrong, g_faults, g_stores;
static atomic_long g_laps[NTHREADS];

// Page i holds `mov w0, #i; ret` at its start. Callers run the even pages
// (every page with g_all_pages) and count their laps.
static void *caller(void *arg)
{
    int me = (int)(uintptr_t)arg;
    volatile unsigned i = (unsigned)me * 38;
    t_armed = 1;
    if (sigsetjmp(t_jb, 1) != 0)
        atomic_fetch_add(&g_faults, 1);         // and carry on
    while (!atomic_load(&g_stop)) {
        unsigned step = atomic_load(&g_all_pages) ? 1 : 2;
        i = ((i + step) % NPAGES) & ~(step - 1);
        if (i < step)
            atomic_fetch_add(&g_laps[me], 1);
        long r = ((fn_t)(uintptr_t)(R + (uint64_t)i * PG))();
        if (r != (long)i) atomic_fetch_add(&g_wrong, 1);
        atomic_fetch_add(&g_calls, 1);
    }
    t_armed = 0;
    return NULL;
}

// Stores into a data word in the second half of every page.
static void *storer(void *arg)
{
    volatile unsigned i = (unsigned)(uintptr_t)arg * 53;
    t_armed = 1;
    if (sigsetjmp(t_jb, 1) != 0)
        atomic_fetch_add(&g_faults, 1);
    while (!atomic_load(&g_stop)) {
        i = (i + 1) % NPAGES;
        *(volatile uint32_t *)(R + (uint64_t)i * PG + 8 * KB) = i;
        atomic_fetch_add(&g_stores, 1);
    }
    t_armed = 0;
    return NULL;
}

// Every caller has made two more laps: none of them is still inside a fault
// it took before now.
static void wait_laps(void)
{
    long base[NTHREADS];
    for (int t = 0; t < NTHREADS; t++) base[t] = atomic_load(&g_laps[t]);
    for (int t = 0; t < NTHREADS; t++)
        while (atomic_load(&g_laps[t]) < base[t] + 2)
            ;
}

static void start(pthread_t *th, void *(*fn)(void *))
{
    atomic_store(&g_stop, 0);
    atomic_store(&g_faults, 0);
    atomic_store(&g_calls, 0);
    atomic_store(&g_wrong, 0);
    atomic_store(&g_stores, 0);
    for (int t = 0; t < NTHREADS; t++) pthread_create(&th[t], NULL, fn, (void *)(uintptr_t)t);
    while (atomic_load(&g_calls) + atomic_load(&g_stores) < 10000) ;
}

static void stop(pthread_t *th)
{
    atomic_store(&g_stop, 1);
    for (int t = 0; t < NTHREADS; t++) pthread_join(th[t], NULL);
}

// S1 and S2 (see the header).
static int stress(long want)
{
    pthread_t th[NTHREADS];
    atomic_store(&g_all_pages, 1);
    start(th, caller);
    long perr = 0, svc_bad = 0;
    for (int round = 0; round < ROUNDS; round++) {
        for (int k = 0; k < 4; k++) {
            unsigned i = (unsigned)(round * 7 + k * 61) % NPAGES;
            emit_const((uint32_t *)(R + (uint64_t)i * PG), i);
        }
        uint32_t *svc = (uint32_t *)(R + (uint64_t)((round * 13) % NPAGES) * PG + 12 * KB);
        emit_getpid(svc);
        if (mprotect(R, RANGE, PROT_READ | PROT_EXEC) != 0) perr++;
        if (((fn_t)(uintptr_t)svc)() != want) svc_bad++;
        if (mprotect(R, RANGE, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) perr++;
    }
    stop(th);
    long f1 = atomic_load(&g_faults), wrong = atomic_load(&g_wrong);
    printf("  S1   %d x RWX -> RX -> RWX, callers on the pages being written: %ld faults, %ld wrong of %ld calls, "
           "%ld svc wrong, %ld mprotect errors\n", ROUNDS, f1, wrong, (long)atomic_load(&g_calls), svc_bad, perr);

    start(th, storer);
    long run_bad = 0;
    for (int round = 0; round < ROUNDS; round++) {
        for (unsigned i = 0; i < NPAGES; i++)
            if (((fn_t)(uintptr_t)(R + (uint64_t)i * PG))() != (long)i) run_bad++;
        if (mprotect(R, RANGE, PROT_READ | PROT_WRITE) != 0) perr++;
        if (mprotect(R, RANGE, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) perr++;
    }
    stop(th);
    long f2 = atomic_load(&g_faults);
    printf("  S2   %d x RWX -> RW -> RWX under %d storers: %ld faults of %ld stores, %ld wrong calls, "
           "%ld mprotect errors\n", ROUNDS, NTHREADS, f2, (long)atomic_load(&g_stores), run_bad, perr);
    printf("== wx_mprotect_race stress: S1 %ld faults, S2 %ld faults\n", f1, f2);
    if (perr || svc_bad || run_bad || wrong)
        return 1;
    return f1 || f2 ? 2 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    long want = getpid();

    R = mmap(NULL, RANGE, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(R != MAP_FAILED, "reserve %lu MiB", RANGE / MB);
    if (R == MAP_FAILED) return 1;
    CHECK(mprotect(R, RANGE, PROT_READ | PROT_WRITE | PROT_EXEC) == 0, "commit it RWX");
    long pre = 0;
    for (unsigned i = 0; i < NPAGES; i++)
        if (emit_const((uint32_t *)(R + (uint64_t)i * PG), i)() != (long)i) pre++;
    CHECK(pre == 0, "%lu pages of code written and run: %ld wrong", NPAGES, pre);

    if (argc > 1 && !strcmp(argv[1], "stress"))
        return stress(want);

    // A. RWX -> RX -> RWX under callers on the even pages; the odd pages are
    // written, so they are read-write when RX is asked.
    pthread_t th[NTHREADS];
    start(th, caller);
    long perr = 0, svc_bad = 0, again_bad = 0;
    for (int round = 0; round < ROUNDS; round++) {
        for (int k = 0; k < 4; k++) {
            unsigned i = ((unsigned)(round * 7 + k * 61) % NPAGES) | 1;
            emit_const((uint32_t *)(R + (uint64_t)i * PG), i);
        }
        uint32_t *svc = (uint32_t *)(R + (uint64_t)(((unsigned)round * 13 % NPAGES) | 1) * PG + 12 * KB);
        emit_getpid(svc);
        if (mprotect(R, RANGE, PROT_READ | PROT_EXEC) != 0) perr++;
        if (((fn_t)(uintptr_t)svc)() != want) svc_bad++;
        if (mprotect(R, RANGE, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) perr++;
        // RWX made every page read-write again: run the callers' pages here
        // and let every caller finish the faults that took it (see S1).
        for (unsigned i = 0; i < NPAGES; i += 2)
            if (((fn_t)(uintptr_t)(R + (uint64_t)i * PG))() != (long)i) again_bad++;
        wait_laps();
    }
    stop(th);
    CHECK(perr == 0 && again_bad == 0 && atomic_load(&g_faults) == 0 && atomic_load(&g_wrong) == 0,
          "A. %d x RWX -> RX -> RWX over %lu MiB under %d callers: %ld faults, %ld wrong of %ld calls, %ld mprotect errors",
          ROUNDS, RANGE / MB, NTHREADS, (long)atomic_load(&g_faults), (long)atomic_load(&g_wrong) + again_bad,
          (long)atomic_load(&g_calls), perr);
    CHECK(svc_bad == 0, "A. a svc written before each RX returns getpid(): %ld wrong", svc_bad);

    // C. The protection asked for is the one in force.
    fn_t f0 = (fn_t)(uintptr_t)R, f9 = (fn_t)(uintptr_t)(R + 9 * PG);
    CHECK(mprotect(R, RANGE, PROT_READ | PROT_EXEC) == 0 && f0() == 0 && f9() == 9 &&
          access_faults((uint32_t *)(R + 9 * PG + 8 * KB), 1), "C. RX: code runs, a store faults");
    CHECK(mprotect(R, RANGE, PROT_READ | PROT_WRITE) == 0 && call_faults(f9) &&
          !access_faults((uint32_t *)(R + 9 * PG + 8 * KB), 1), "C. RW: a fetch faults, a store does not");
    CHECK(mprotect(R, RANGE, PROT_READ | PROT_WRITE | PROT_EXEC) == 0 && f9() == 9, "C. RWX: runs again");
    CHECK(mprotect(R, RANGE, PROT_READ) == 0 && call_faults(f9) &&
          access_faults((uint32_t *)(R + 9 * PG + 8 * KB), 1) &&
          !access_faults((uint32_t *)(R + 9 * PG), 0), "C. R: a fetch and a store fault, a read does not");
    CHECK(mprotect(R, RANGE, PROT_READ | PROT_WRITE | PROT_EXEC) == 0 && f9() == 9 && f0() == 0,
          "C. RWX after R: runs again");
    CHECK(munmap(R, RANGE) == 0, "range unmapped");

    // D. RX over half an RWX range and half ordinary read-write memory.
    char *M = mmap(NULL, 256 * KB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(M != MAP_FAILED && mprotect(M, 128 * KB, PROT_READ | PROT_WRITE | PROT_EXEC) == 0,
          "D. 256 KiB: the first half RWX, the second RW");
    fn_t lo = emit_getpid((uint32_t *)(M + 64 * KB)), hi = emit_getpid((uint32_t *)(M + 192 * KB));
    fn_t lo2 = emit_const((uint32_t *)(M + 16 * KB), 5);
    CHECK(lo2() == 5, "D. code in the RWX half runs");
    CHECK(mprotect(M, 256 * KB, PROT_READ | PROT_EXEC) == 0, "D. RX over all of it");
    long glo = lo(), ghi = hi();
    CHECK(glo == want && ghi == want && lo2() == 5, "D. svc in both halves returns getpid(): %ld %ld (want %ld)", glo, ghi, want);
    munmap(M, 256 * KB);

    printf("== wx_mprotect_race: %s (%d ok, %d mal)\n", bad ? "FAIL" : "ok", ok, bad);
    return bad ? 1 : 0;
}
