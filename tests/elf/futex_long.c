// Futex waits the runtime used to get wrong, and the wakes it now answers
// without the kernel:
//   - a wait of more than 71 minutes (__ulock_wait counts 32-bit
//     microseconds) returned ETIMEDOUT at once: Source 2's thread pool waits
//     0xffffffff ms on its condition variable and spun (Counter-Strike 2
//     never drew a frame, benchmarks/stage51). Relative (FUTEX_WAIT) and
//     absolute (FUTEX_WAIT_BITSET, both clocks).
//   - a private wake with nobody parked is skipped (runtime/thread.c,
//     g_parked): a waiter that is about to park must still be woken, 20000
//     times in a row, with and without LXRT_FUTEX_SPIN_US.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what) { printf("  %s  %s\n", ok ? "OK " : "MAL", what); fails += !ok; }

static long futex(uint32_t *a, int op, uint32_t v, const struct timespec *ts, uint32_t v3)
{
    long r = syscall(SYS_futex, a, op, v, ts, NULL, v3);
    return r < 0 ? -errno : r;
}

static double now(clockid_t c)
{
    struct timespec t;
    clock_gettime(c, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static _Atomic uint32_t word;
static int g_op;
static struct timespec g_ts;
static long g_ret;
static double g_waited;

static void *waiter(void *arg)
{
    (void)arg;
    double t0 = now(CLOCK_MONOTONIC);
    g_ret = futex((uint32_t *)&word, g_op, 0, &g_ts, FUTEX_BITSET_MATCH_ANY);
    g_waited = now(CLOCK_MONOTONIC) - t0;
    return NULL;
}

// A waiter with the given timeout, woken after 150 ms: it must have slept
// until the wake and returned 0.
static void long_wait(int op, struct timespec ts, const char *what)
{
    pthread_t th;
    atomic_store(&word, 0);
    g_op = op;
    g_ts = ts;
    pthread_create(&th, NULL, waiter, NULL);
    usleep(150000);
    atomic_store(&word, 1);
    futex((uint32_t *)&word, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, 0);
    pthread_join(th, NULL);
    char msg[160];
    snprintf(msg, sizeof msg, "%s: woken after %.0f ms with %ld (0 after >= 140 ms)", what, g_waited * 1e3, g_ret);
    check(g_ret == 0 && g_waited >= 0.14 && g_waited < 5, msg);
}

// Ping-pong: each side stores, wakes the other and waits for its turn.
static _Atomic uint32_t turn;
static _Atomic int rounds;
enum { ROUNDS = 20000 };

static void *pong(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t t;
        while (((t = atomic_load(&turn)) & 1) == 0) {
            if (t == 0xfffffffe)
                return NULL;
            futex((uint32_t *)&turn, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, t, NULL, 0);
        }
        atomic_fetch_add(&rounds, 1);
        atomic_store(&turn, t + 1);
        futex((uint32_t *)&turn, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, 0);
    }
}

int main(void)
{
    struct timespec rel = { 4294967, 296000000 };                       // 0xffffffff ms
    long_wait(FUTEX_WAIT | FUTEX_PRIVATE_FLAG, rel, "FUTEX_WAIT for 0xffffffff ms");
    struct timespec mono, real;
    clock_gettime(CLOCK_MONOTONIC, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    mono.tv_sec += 4294967;
    real.tv_sec += 4294967;
    long_wait(FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG, mono, "FUTEX_WAIT_BITSET to now + 0xffffffff ms (monotonic)");
    long_wait(FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME, real,
              "FUTEX_WAIT_BITSET to now + 0xffffffff ms (realtime)");
    struct timespec hour = { 5000, 0 };
    long_wait(FUTEX_WAIT | FUTEX_PRIVATE_FLAG, hour, "FUTEX_WAIT for 5000 s");

    // A timeout that is due still times out.
    struct timespec ms20 = { 0, 20000000 };
    atomic_store(&word, 0);
    double t0 = now(CLOCK_MONOTONIC);
    long r = futex((uint32_t *)&word, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, &ms20, 0);
    double dt = now(CLOCK_MONOTONIC) - t0;
    char msg[160];
    snprintf(msg, sizeof msg, "FUTEX_WAIT for 20 ms with no wake: %ld after %.1f ms (ETIMEDOUT after >= 20 ms)", r, dt * 1e3);
    check(r == -ETIMEDOUT && dt >= 0.0199 && dt < 1, msg);

    pthread_t th;
    atomic_store(&turn, 0);
    pthread_create(&th, NULL, pong, NULL);
    t0 = now(CLOCK_MONOTONIC);
    for (int i = 0; i < ROUNDS; i++) {
        uint32_t t = atomic_load(&turn);
        atomic_store(&turn, t + 1);                                      // odd: pong's turn
        futex((uint32_t *)&turn, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, 0);
        while ((atomic_load(&turn) & 1) == 1) {
            struct timespec guard = { 5, 0 };
            if (futex((uint32_t *)&turn, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, t + 1, &guard, 0) == -ETIMEDOUT) {
                check(0, "a wake was lost: the waiter slept 5 s on a word that had changed hands");
                printf("FAIL\n");
                return 1;
            }
        }
    }
    dt = now(CLOCK_MONOTONIC) - t0;
    atomic_store(&turn, 0xfffffffe);
    futex((uint32_t *)&turn, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, 1, NULL, 0);
    pthread_join(th, NULL);
    snprintf(msg, sizeof msg, "%d wake/wait round trips between two threads, none lost (%.1f us each)", atomic_load(&rounds),
             dt / ROUNDS * 1e6);
    check(atomic_load(&rounds) == ROUNDS, msg);

    printf(fails ? "FAIL\n" : "PASS\n");
    return fails != 0;
}
