// futex_waitv(2), as Proton's fsync uses it: the probe (NULL, 0 -> EINVAL,
// not ENOSYS), EAGAIN when a word already differs, an absolute timeout,
// waking one of several words in one bucket and across buckets, a waiter in
// another process on a shared memory region mapped at another address (Wine
// maps its fsync region that way), and the round-trip latency of a wake.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
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

static int fails;
static void check(int ok, const char *what) { printf("  %s  %s\n", ok ? "OK " : "MAL", what); fails += !ok; }

static long waitv(struct waitv *w, unsigned n, struct timespec *ts)
{
    long r = syscall(SYS_futex_waitv, w, n, 0, ts, CLOCK_MONOTONIC);
    return r < 0 ? -errno : r;
}
static void wake(uint32_t *a, int shared) { syscall(SYS_futex, a, shared ? FUTEX_WAKE : FUTEX_WAKE_PRIVATE, 1, 0, 0, 0); }
static int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }

static uint32_t words[4096];       // words in different buckets: indexes 0, 1, 2 at distinct page offsets
static struct { uint32_t *w; int idx; } g_wake;

static void *waker(void *a)
{
    (void)a;
    usleep(30000);
    __atomic_store_n(g_wake.w, 1, __ATOMIC_SEQ_CST);
    wake(g_wake.w, 0);
    return NULL;
}

static volatile uint32_t ping, pong;
static int rounds = 2000;
static void *ponger(void *a)
{
    (void)a;
    for (int i = 1; i <= rounds; i++) {
        struct waitv w[2] = { { (uint32_t)(i - 1), (uint64_t)(uintptr_t)&ping, F32 | FPRIV, 0 },
                              { 0xdead, (uint64_t)(uintptr_t)&words[700], F32 | FPRIV, 0 } };
        while (__atomic_load_n(&ping, __ATOMIC_SEQ_CST) == (uint32_t)(i - 1)) waitv(w, 2, NULL);
        __atomic_store_n(&pong, i, __ATOMIC_SEQ_CST);
        wake((uint32_t *)&pong, 0);
    }
    return NULL;
}

int main(void)
{
    errno = 0;
    long r = syscall(SYS_futex_waitv, NULL, 0, 0, NULL, 0);
    check(r == -1 && errno == EINVAL, "Proton's probe futex_waitv(NULL, 0): EINVAL, not ENOSYS");

    struct waitv w[3];
    words[0] = 5; words[1100] = 6; words[2200] = 7;
    w[0] = (struct waitv){ 5, (uint64_t)(uintptr_t)&words[0], F32 | FPRIV, 0 };
    w[1] = (struct waitv){ 6, (uint64_t)(uintptr_t)&words[1100], F32 | FPRIV, 0 };
    w[2] = (struct waitv){ 99, (uint64_t)(uintptr_t)&words[2200], F32 | FPRIV, 0 };
    check(waitv(w, 3, NULL) == -EAGAIN, "a word that already differs: EAGAIN");
    w[2].val = 7;

    struct waitv bad = { 0, (uint64_t)(uintptr_t)&words[0], 0x01 /* U16 */, 0 };
    check(waitv(&bad, 1, NULL) == -EINVAL, "a 16-bit futex: EINVAL");
    check(syscall(SYS_futex_waitv, w, 129, 0, NULL, 0) == -1 && errno == EINVAL, "129 futexes: EINVAL");

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_nsec += 50000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    int64_t t0 = now_ns();
    r = waitv(w, 3, &ts);
    int64_t dt = now_ns() - t0;
    check(r == -ETIMEDOUT && dt >= 45000000 && dt < 500000000, "absolute CLOCK_MONOTONIC timeout: ETIMEDOUT after ~50 ms");
    printf("       (%.1f ms)\n", dt / 1e6);

    // Several buckets: the third word is woken.
    pthread_t t;
    g_wake.w = &words[2200];
    pthread_create(&t, NULL, waker, NULL);
    t0 = now_ns();
    r = waitv(w, 3, NULL);
    dt = now_ns() - t0;
    pthread_join(t, NULL);
    check(r == 2, "three words in three buckets, the third stored and woken: returns 2");
    printf("       (returned %ld after %.1f ms)\n", r, dt / 1e6);

    // One bucket.
    words[3000] = 0;
    struct waitv one = { 0, (uint64_t)(uintptr_t)&words[3000], F32 | FPRIV, 0 };
    g_wake.w = &words[3000];
    pthread_create(&t, NULL, waker, NULL);
    r = waitv(&one, 1, NULL);
    pthread_join(t, NULL);
    check(r == 0, "one word, stored and woken: returns 0");

    // Another process, the region mapped at another address.
    int mfd = memfd_create("fsync", 0);
    ftruncate(mfd, 65536);
    uint32_t *shm = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    shm[100] = 0; shm[5000] = 0;
    pid_t kid = fork();
    if (kid == 0) {
        uint32_t *mine = mmap(NULL, 65536, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);   // another address
        struct waitv cw[2] = { { 0, (uint64_t)(uintptr_t)&mine[100], F32, 0 },
                               { 0, (uint64_t)(uintptr_t)&mine[5000], F32, 0 } };
        struct timespec cts;
        clock_gettime(CLOCK_MONOTONIC, &cts);
        cts.tv_sec += 5;
        long cr = waitv(cw, 2, &cts);
        _exit(cr == 1 && mine != shm ? 0 : (cr == -ETIMEDOUT ? 2 : 3));
    }
    usleep(100000);
    __atomic_store_n(&shm[5000], 1, __ATOMIC_SEQ_CST);
    t0 = now_ns();
    wake(&shm[5000], 1);
    int st = 0;
    waitpid(kid, &st, 0);
    dt = now_ns() - t0;
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0 && dt < 1000000000LL,
          "a waiter in another process, its mapping elsewhere, woken by a shared FUTEX_WAKE: returns 1");
    printf("       (child status %d, %.1f ms after the wake)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1, dt / 1e6);

    // Round trips: futex_waitv on one side, a plain futex wait on the other.
    pthread_create(&t, NULL, ponger, NULL);
    t0 = now_ns();
    for (int i = 1; i <= rounds; i++) {
        __atomic_store_n(&ping, i, __ATOMIC_SEQ_CST);
        wake((uint32_t *)&ping, 0);
        while (__atomic_load_n(&pong, __ATOMIC_SEQ_CST) != (uint32_t)i)
            syscall(SYS_futex, &pong, FUTEX_WAIT_PRIVATE, (uint32_t)(i - 1), NULL, 0, 0);
    }
    dt = now_ns() - t0;
    pthread_join(t, NULL);
    check(pong == (uint32_t)rounds, "2000 round trips, futex_waitv on one side");
    printf("       (%.1f us per round trip)\n", dt / 1e3 / rounds);

    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
