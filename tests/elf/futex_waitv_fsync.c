// futex_waitv(2) on one word, the way Proton's fsync waits for one object
// (dlls/ntdll/unix/fsync.c): the object's word in a shared memory object
// mapped 64 KiB at a time, at another address in each process (get_shm),
// FUTEX2_PRIVATE clear, an absolute deadline on CLOCK_MONOTONIC or
// CLOCK_REALTIME, and every signal a store followed by a plain shared
// FUTEX_WAKE of INT_MAX (futex_wake). runtime/futex_waitv.c parks such a wait
// on the word itself (wait_one); LXRT_WAITV_DIRECT=0 puts it back on the
// board. A wake that misses a waiter is a hang, so every wait here carries a
// deadline and a lost wakeup shows up as a timeout. Every check is one Linux
// passes too.
//
// "pingpong" as the only argument runs just the round-trip measurements,
// which older runtimes can run as well (they fault on the EFAULT check).
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_futex_waitv
#define SYS_futex_waitv 449
#endif
struct waitv { uint64_t val, uaddr; uint32_t flags, reserved; };
#define F32 0x02
#define FPRIV 128
#define CHUNK 65536                 // FSYNC_SHM_PAGE_SIZE
#define LONG_NS 2000000000LL        // a wait that should end long before this

static int fails;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "OK " : "MAL", what);
    fflush(stdout);
    fails += !ok;
}

static int64_t now(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return t.tv_sec * 1000000000LL + t.tv_nsec;
}

static struct timespec ts_of(int64_t ns) { return (struct timespec){ ns / 1000000000LL, ns % 1000000000LL }; }

// One word, as fsync's futex_vector_set builds it, until the absolute time
// `end` on clock `c` (end < 0: no timeout).
static long wait1(uint32_t *w, uint32_t val, int priv, clockid_t c, int64_t end)
{
    struct waitv v = { val, (uint64_t)(uintptr_t)w, F32 | (priv ? FPRIV : 0), 0 };
    struct timespec ts = ts_of(end < 0 ? 0 : end);
    long r = syscall(SYS_futex_waitv, &v, 1, 0, end < 0 ? NULL : &ts, end < 0 ? 0 : c);
    return r < 0 ? -errno : r;
}

static long futex(uint32_t *w, int op, uint32_t val, uint64_t val2, uint32_t *w2, uint32_t val3)
{
    long r = syscall(SYS_futex, w, op, val, val2, w2, val3);
    return r < 0 ? -errno : r;
}

static void wake_shared(uint32_t *w) { futex(w, FUTEX_WAKE, INT_MAX, 0, NULL, 0); }

// fsync_set_event on an auto-reset event.
static void set_event(uint32_t *e)
{
    if (!__atomic_exchange_n(e, 1, __ATOMIC_SEQ_CST))
        wake_shared(e);
}

// fsync_wait_objects on one auto-reset event: take it, or wait for 0 to move.
// -ETIMEDOUT only if LONG_NS passed: a lost wakeup.
static long take_event(uint32_t *e)
{
    int64_t end = now(CLOCK_MONOTONIC) + LONG_NS;
    for (;;) {
        uint32_t one = 1;
        if (__atomic_compare_exchange_n(e, &one, 0, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
            return 0;
        if (wait1(e, 0, 0, CLOCK_MONOTONIC, end) == -ETIMEDOUT)
            return -ETIMEDOUT;
    }
}

// fsync's semaphore: release adds and wakes everyone, a waiter takes one.
static void release_sem(uint32_t *c)
{
    __atomic_add_fetch(c, 1, __ATOMIC_SEQ_CST);
    wake_shared(c);
}

static long take_sem(uint32_t *c)
{
    int64_t end = now(CLOCK_MONOTONIC) + LONG_NS;
    for (;;) {
        uint32_t cur = __atomic_load_n(c, __ATOMIC_SEQ_CST);
        while (cur)
            if (__atomic_compare_exchange_n(c, &cur, cur - 1, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
                return 0;
        if (wait1(c, 0, 0, CLOCK_MONOTONIC, end) == -ETIMEDOUT)
            return -ETIMEDOUT;
    }
}

static int g_fd;
// Piece 1 of the object, mapped fresh: another address on every call.
static uint32_t *map_piece(void)
{
    void *p = mmap(NULL, CHUNK, PROT_READ | PROT_WRITE, MAP_SHARED, g_fd, CHUNK);
    return p == MAP_FAILED ? NULL : p;
}
// The word of object i (16 bytes each, as fsync's).
static uint32_t *obj(uint32_t *piece, int i) { return piece + 4 * i; }

// ------------------------------------------------------------ thread wakers

enum { BY_WAKE, BY_BITSET, BY_REQUEUE, BY_PRIVATE };
static struct { uint32_t *w; int how; } g_waker;

static void *waker(void *a)
{
    (void)a;
    usleep(20000);                  // long enough for the waiter to park
    __atomic_store_n(g_waker.w, 1, __ATOMIC_SEQ_CST);
    switch (g_waker.how) {
    case BY_WAKE:    futex(g_waker.w, FUTEX_WAKE, INT_MAX, 0, NULL, 0); break;
    case BY_BITSET:  futex(g_waker.w, FUTEX_WAKE_BITSET, INT_MAX, 0, NULL, FUTEX_BITSET_MATCH_ANY); break;
    case BY_REQUEUE: futex(g_waker.w, FUTEX_CMP_REQUEUE, 1, 0, g_waker.w + 1, 1); break;
    case BY_PRIVATE: futex(g_waker.w, FUTEX_WAKE_PRIVATE, INT_MAX, 0, NULL, 0); break;
    }
    return NULL;
}

// Waits on *w == 0 while another thread stores 1 and wakes it `how`.
static void woken_by_thread(uint32_t *w, int priv, int how, const char *what)
{
    *w = 0;
    g_waker.w = w;
    g_waker.how = how;
    pthread_t t;
    pthread_create(&t, NULL, waker, NULL);
    int64_t t0 = now(CLOCK_MONOTONIC);
    long r = wait1(w, 0, priv, CLOCK_MONOTONIC, t0 + LONG_NS);
    int64_t dt = now(CLOCK_MONOTONIC) - t0;
    pthread_join(t, NULL);
    char line[200];
    snprintf(line, sizeof line, "%s: returns 0 (%ld after %.1f ms)", what, r, dt / 1e6);
    check(r == 0 && dt < LONG_NS / 2, line);
}

static void on_usr1(int s) { (void)s; }
static pthread_t g_main;
static void *kicker(void *a)
{
    (void)a;
    usleep(20000);
    pthread_kill(g_main, SIGUSR1);
    return NULL;
}

// ------------------------------------------------------------- round trips
//
// Round trips run in one of two regimes: each side parks and is woken, or --
// when a wake takes longer than the other side's turn -- each finds the
// other's event already set and nobody parks at all. So what a parked waiter
// costs is measured apart as well: how soon it runs, and how long the waker
// spends waking it.

static int rounds = 20000;
#define BYSTANDERS 8                // per event, on words in the same board bucket
static uint32_t *g_ping, *g_pong;
static int g_lost;

static void *pong_thread(void *a)
{
    (void)a;
    for (int i = 0; i < rounds; i++) {
        if (take_event(g_ping)) { g_lost++; break; }
        set_event(g_pong);
    }
    return NULL;
}

// A thread of another object, parked until its word is set: on Linux nothing
// ever disturbs it. Objects 256 apart are 4 KiB apart, the same bucket of
// runtime/futex_waitv.c's board. Non-zero if the wake after the store that
// releases it was lost (its wait ran to the deadline with the word set).
static void *bystander(void *a)
{
    uint32_t *w = a;
    while (!__atomic_load_n(w, __ATOMIC_SEQ_CST))
        if (wait1(w, 0, 0, CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + LONG_NS) == -ETIMEDOUT &&
            __atomic_load_n(w, __ATOMIC_SEQ_CST))
            return (void *)1;
    return NULL;
}

// Round trips between two auto-reset events, the answering side a thread or
// a process with its own mapping, optionally with bystanders parked beside
// both events in the answering process. Microseconds per round trip, or -1.
static double ping_pong(int cross_process, int bystanders)
{
    uint32_t *piece = map_piece();
    g_ping = obj(piece, 100);
    g_pong = obj(piece, 200);       // another bucket of the board
    *g_ping = *g_pong = 0;
    for (int k = 1; k <= BYSTANDERS; k++)
        *obj(piece, 100 + 256 * k) = *obj(piece, 200 + 256 * k) = 0;
    g_lost = 0;
    pid_t kid = -1;
    pthread_t t;
    if (cross_process) {
        kid = fork();
        if (kid == 0) {
            uint32_t *mine = map_piece();
            uint32_t *ping = obj(mine, 100), *pong = obj(mine, 200);
            pthread_t by[2 * BYSTANDERS];
            int nby = bystanders ? 2 * BYSTANDERS : 0;
            for (int k = 0; k < nby; k++)
                pthread_create(&by[k], NULL, bystander, obj(mine, (k & 1 ? 200 : 100) + 256 * (k / 2 + 1)));
            int st = mine != piece ? 0 : 3;
            for (int i = 0; i < rounds && !st; i++) {
                if (take_event(ping))
                    st = 2;
                set_event(pong);
            }
            for (int k = 0; k < nby; k++) {
                uint32_t *w = obj(mine, (k & 1 ? 200 : 100) + 256 * (k / 2 + 1));
                __atomic_store_n(w, 1, __ATOMIC_SEQ_CST);
                wake_shared(w);
                void *res = NULL;
                pthread_join(by[k], &res);
                if (res && !st)
                    st = 4;
            }
            _exit(st);
        }
    } else {
        pthread_create(&t, NULL, pong_thread, NULL);
    }
    usleep(20000);                  // the bystanders park
    int64_t t0 = now(CLOCK_MONOTONIC);
    int lost = 0;
    for (int i = 0; i < rounds && !lost; i++) {
        set_event(g_ping);
        lost = take_event(g_pong) != 0;
    }
    int64_t dt = now(CLOCK_MONOTONIC) - t0;
    int ok = !lost;
    if (cross_process) {
        int st = 0;
        if (lost)
            kill(kid, SIGKILL);
        waitpid(kid, &st, 0);
        ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    } else {
        if (lost)
            set_event(g_ping);      // let it out (it may have lost its wake too)
        pthread_join(t, NULL);
        ok = ok && !g_lost;
    }
    munmap(piece, CHUNK);
    return ok ? dt / 1e3 / rounds : -1;
}

// A waiter that is surely parked: the waker lets 300 us pass, notes the time
// and sets the event; the waiter notes how long after that its wait returned,
// the waker how long its SetEvent took. Medians, in microseconds.
#define LAT_SAMPLES 1000
struct lat { uint32_t ev, ack; int64_t t_set; int64_t back[LAT_SAMPLES]; };
static struct lat *g_lat;

static int lat_waiter(struct lat *l)
{
    for (int i = 0; i < LAT_SAMPLES; i++) {
        if (take_event(&l->ev))
            return 1;
        l->back[i] = now(CLOCK_MONOTONIC) - __atomic_load_n(&l->t_set, __ATOMIC_SEQ_CST);
        set_event(&l->ack);
    }
    return 0;
}

static void *lat_thread(void *a) { (void)a; return (void *)(intptr_t)lat_waiter(g_lat); }

static int cmp64(const void *a, const void *b)
{
    int64_t x = *(const int64_t *)a, y = *(const int64_t *)b;
    return x < y ? -1 : x > y;
}

static int wake_latency(int cross_process, double *woken_us, double *waker_us)
{
    uint32_t *piece = map_piece();
    struct lat *l = (struct lat *)obj(piece, 64);
    memset(l, 0, sizeof *l);
    g_lat = l;
    static int64_t spent[LAT_SAMPLES];
    pid_t kid = -1;
    pthread_t t;
    if (cross_process) {
        kid = fork();
        if (kid == 0) {
            uint32_t *mine = map_piece();
            _exit(lat_waiter((struct lat *)obj(mine, 64)));
        }
    } else {
        pthread_create(&t, NULL, lat_thread, NULL);
    }
    int lost = 0;
    for (int i = 0; i < LAT_SAMPLES && !lost; i++) {
        usleep(300);
        int64_t t0 = now(CLOCK_MONOTONIC);
        __atomic_store_n(&l->t_set, t0, __ATOMIC_SEQ_CST);
        set_event(&l->ev);
        spent[i] = now(CLOCK_MONOTONIC) - t0;
        lost = take_event(&l->ack) != 0;
    }
    int ok = !lost;
    if (cross_process) {
        int st = 0;
        if (lost)
            kill(kid, SIGKILL);
        waitpid(kid, &st, 0);
        ok = ok && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    } else {
        void *res = NULL;
        if (lost)
            set_event(&l->ev);
        pthread_join(t, &res);
        ok = ok && !res;
    }
    if (ok) {
        qsort(l->back, LAT_SAMPLES, sizeof l->back[0], cmp64);
        qsort(spent, LAT_SAMPLES, sizeof spent[0], cmp64);
        *woken_us = l->back[LAT_SAMPLES / 2] / 1e3;
        *waker_us = spent[LAT_SAMPLES / 2] / 1e3;
    }
    munmap(piece, CHUNK);
    return ok;
}

// What a SetEvent nobody waits for costs: the exchange and one shared
// FUTEX_WAKE that finds nobody. Microseconds per call.
static double idle_wake_cost(void)
{
    uint32_t *piece = map_piece();
    uint32_t *e = obj(piece, 300);
    int n = 200000;
    int64_t t0 = now(CLOCK_MONOTONIC);
    for (int i = 0; i < n; i++) {
        __atomic_store_n(e, 0, __ATOMIC_SEQ_CST);
        set_event(e);
    }
    int64_t dt = now(CLOCK_MONOTONIC) - t0;
    munmap(piece, CHUNK);
    return dt / 1e3 / n;
}

// ------------------------------------------------------------- semaphore

#define SEM_TAKES 1000
static uint32_t *g_sem;
static void *sem_taker(void *a)
{
    uint32_t *c = a;
    for (int i = 0; i < SEM_TAKES; i++)
        if (take_sem(c))
            return (void *)1;
    return NULL;
}

static void *sem_releaser(void *a)
{
    (void)a;
    for (int i = 0; i < 4 * SEM_TAKES; i++) {
        release_sem(g_sem);
        if (i % 20 == 0)
            usleep(100);            // the count drains and every taker parks
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int bench_only = argc > 1 && !strcmp(argv[1], "pingpong");
    g_fd = memfd_create("fsync", 0);
    if (g_fd < 0 || ftruncate(g_fd, 2 * CHUNK)) {
        printf("memfd: %s\nFAIL\n", strerror(errno));
        return 1;
    }

    if (!bench_only) {
        uint32_t *piece = map_piece();
        uint32_t *w = obj(piece, 1);

        *w = 5;
        check(wait1(w, 4, 0, CLOCK_MONOTONIC, -1) == -EAGAIN, "a word that already differs: EAGAIN");
        check(wait1(w, 4, 0, CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) - 1000000000LL) == -EAGAIN,
              "EAGAIN before ETIMEDOUT when both apply");

        int64_t t0 = now(CLOCK_MONOTONIC);
        long r = wait1(w, 5, 0, CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) - 1000000000LL);
        int64_t dt = now(CLOCK_MONOTONIC) - t0;
        check(r == -ETIMEDOUT && dt < 100000000LL, "a deadline already past: ETIMEDOUT at once");

        t0 = now(CLOCK_MONOTONIC);
        r = wait1(w, 5, 0, CLOCK_MONOTONIC, t0 + 30000000LL);
        dt = now(CLOCK_MONOTONIC) - t0;
        char line[200];
        snprintf(line, sizeof line, "absolute CLOCK_MONOTONIC deadline 30 ms ahead: ETIMEDOUT after %.1f ms", dt / 1e6);
        check(r == -ETIMEDOUT && dt >= 29000000LL && dt < 1000000000LL, line);

        int64_t rt0 = now(CLOCK_REALTIME);
        t0 = now(CLOCK_MONOTONIC);
        r = wait1(w, 5, 0, CLOCK_REALTIME, rt0 + 30000000LL);
        dt = now(CLOCK_MONOTONIC) - t0;
        snprintf(line, sizeof line, "absolute CLOCK_REALTIME deadline 30 ms ahead (fsync's for NT times): ETIMEDOUT after %.1f ms", dt / 1e6);
        check(r == -ETIMEDOUT && dt >= 25000000LL && dt < 1000000000LL, line);

        // A deadline past what 64-bit nanoseconds hold is no deadline (Linux
        // saturates it): the wait ends when woken, not at once.
        uint32_t *hw = obj(piece, 10);
        *hw = 0;
        g_waker.w = hw;
        g_waker.how = BY_WAKE;
        pthread_t ht;
        pthread_create(&ht, NULL, waker, NULL);
        struct waitv hv = { 0, (uint64_t)(uintptr_t)hw, F32, 0 };
        struct timespec far = { LLONG_MAX / 2, 0 };
        r = syscall(SYS_futex_waitv, &hv, 1, 0, &far, CLOCK_MONOTONIC);
        if (r < 0)
            r = -errno;
        pthread_join(ht, NULL);
        snprintf(line, sizeof line, "a deadline beyond 64-bit nanoseconds waits like none: returns 0 when woken (%ld)", r);
        check(r == 0, line);

        void *gone = mmap(NULL, 16384, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        munmap(gone, 16384);
        check(wait1((uint32_t *)gone, 0, 0, CLOCK_MONOTONIC, -1) == -EFAULT, "an unmapped word: EFAULT");

        woken_by_thread(obj(piece, 2), 0, BY_WAKE, "shm word, woken by another thread's shared FUTEX_WAKE");
        woken_by_thread(obj(piece, 3), 0, BY_BITSET, "shm word, woken by FUTEX_WAKE_BITSET (match any)");
        woken_by_thread(obj(piece, 4), 0, BY_REQUEUE, "shm word, woken by FUTEX_CMP_REQUEUE");
        woken_by_thread(obj(piece, 5), 1, BY_PRIVATE, "shm word, FUTEX2_PRIVATE, woken by FUTEX_WAKE_PRIVATE");
        static uint32_t anon_words[64];
        woken_by_thread(&anon_words[8], 0, BY_WAKE, "private memory, woken by a shared FUTEX_WAKE");
        woken_by_thread(&anon_words[16], 1, BY_PRIVATE, "private memory, FUTEX2_PRIVATE, woken by FUTEX_WAKE_PRIVATE");

        // A guest handler without SA_RESTART ends the wait with EINTR.
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = on_usr1;
        sigaction(SIGUSR1, &sa, NULL);
        g_main = pthread_self();
        uint32_t *iw = obj(piece, 6);
        *iw = 0;
        pthread_t t;
        pthread_create(&t, NULL, kicker, NULL);
        t0 = now(CLOCK_MONOTONIC);
        r = wait1(iw, 0, 0, CLOCK_MONOTONIC, t0 + LONG_NS);
        dt = now(CLOCK_MONOTONIC) - t0;
        pthread_join(t, NULL);
        snprintf(line, sizeof line, "a signal handler without SA_RESTART: EINTR (%ld after %.1f ms)", r, dt / 1e6);
        check(r == -EINTR && dt < LONG_NS / 2, line);

        // Another process, its own mapping of the object, either way round.
        uint32_t *xw = obj(piece, 7);
        *xw = 0;
        pid_t kid = fork();
        if (kid == 0) {
            uint32_t *mine = map_piece();
            long cr = wait1(obj(mine, 7), 0, 0, CLOCK_MONOTONIC, now(CLOCK_MONOTONIC) + LONG_NS);
            _exit(cr == 0 && mine != piece ? 0 : cr == -ETIMEDOUT ? 2 : 3);
        }
        usleep(50000);
        __atomic_store_n(xw, 1, __ATOMIC_SEQ_CST);
        t0 = now(CLOCK_MONOTONIC);
        wake_shared(xw);
        int st = 0;
        waitpid(kid, &st, 0);
        dt = now(CLOCK_MONOTONIC) - t0;
        snprintf(line, sizeof line, "a waiter in another process, woken through this process's mapping: returns 0 "
                 "(child status %d, %.1f ms after the wake)", WIFEXITED(st) ? WEXITSTATUS(st) : -1, dt / 1e6);
        check(WIFEXITED(st) && WEXITSTATUS(st) == 0, line);

        xw = obj(piece, 8);
        *xw = 0;
        kid = fork();
        if (kid == 0) {
            uint32_t *mine = map_piece();
            usleep(50000);
            __atomic_store_n(obj(mine, 8), 1, __ATOMIC_SEQ_CST);
            wake_shared(obj(mine, 8));
            _exit(0);
        }
        t0 = now(CLOCK_MONOTONIC);
        r = wait1(xw, 0, 0, CLOCK_MONOTONIC, t0 + LONG_NS);
        dt = now(CLOCK_MONOTONIC) - t0;
        waitpid(kid, &st, 0);
        snprintf(line, sizeof line, "woken by another process through its own mapping: returns 0 (%ld after %.1f ms)",
                 r, dt / 1e6);
        check(r == 0 && dt < LONG_NS / 2, line);

        // A semaphore: two takers here, two in another process, 4000 releases
        // one at a time, each with a wake of everyone.
        g_sem = obj(piece, 9);
        *g_sem = 0;
        kid = fork();
        if (kid == 0) {
            uint32_t *mine = map_piece();
            pthread_t k[2];
            void *res[2];
            for (int i = 0; i < 2; i++)
                pthread_create(&k[i], NULL, sem_taker, obj(mine, 9));
            for (int i = 0; i < 2; i++)
                pthread_join(k[i], &res[i]);
            _exit(res[0] || res[1] ? 2 : 0);
        }
        pthread_t k[3];
        void *res[3] = { 0 };
        for (int i = 0; i < 2; i++)
            pthread_create(&k[i], NULL, sem_taker, g_sem);
        t0 = now(CLOCK_MONOTONIC);
        pthread_create(&k[2], NULL, sem_releaser, NULL);
        for (int i = 0; i < 3; i++)
            pthread_join(k[i], &res[i]);
        waitpid(kid, &st, 0);
        dt = now(CLOCK_MONOTONIC) - t0;
        snprintf(line, sizeof line, "semaphore: 4 takers in 2 processes, 4000 releases, no wait timed out (%.0f ms, count left %u)",
                 dt / 1e6, *g_sem);
        check(!res[0] && !res[1] && WIFEXITED(st) && WEXITSTATUS(st) == 0 && *g_sem == 0, line);
        munmap(piece, CHUNK);
    }

    char line[200];
    static const char *const where[2] = { "threads", "cross-process" };
    for (int x = 1; x >= 0; x--) {
        double rt = ping_pong(x, 0);
        snprintf(line, sizeof line, "%d round trips between two %s, events in shared memory: no wakeup lost",
                 rounds, x ? "processes" : "threads");
        check(rt >= 0, line);
        printf("       (%.2f us per round trip, %s)\n", rt, where[x]);
    }
    rounds = 5000;
    double rb = ping_pong(1, 1);
    snprintf(line, sizeof line, "%d round trips between two processes, %d threads parked beside the events: no wakeup lost",
             rounds, 2 * BYSTANDERS);
    check(rb >= 0, line);
    printf("       (%.2f us per round trip, cross-process, with the bystanders)\n", rb);
    for (int x = 1; x >= 0; x--) {
        double woken = -1, waker = -1;
        int ok = wake_latency(x, &woken, &waker);
        snprintf(line, sizeof line, "%d wakes of a parked waiter, %s: none lost", LAT_SAMPLES, where[x]);
        check(ok, line);
        printf("       (median %.2f us from the store to the waiter running, %.2f us in the waker's SetEvent, %s)\n",
               woken, waker, where[x]);
    }
    printf("       (%.3f us per SetEvent that nobody waits for)\n", idle_wake_cost());

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
