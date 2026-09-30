// A signal that runs no handler interrupts nothing on Linux. Under lxrun a
// realtime signal from another process travels as a carrier (SIGEMT) that
// lands on some thread of the target; when that thread has the signal
// blocked, no handler runs, and Darwin's sleep or wait still came back
// EINTR -- the Steam client's ThreadSleep (one nanosleep, no retry) then
// slept zero and its two-minute wait for the web helper ended at once.
//
// Here the main thread blocks SIGRTMIN, a child sends it SIGRTMIN (and a few
// more) while the parent sleeps or waits on a futex, and each call must run
// its whole time: nanosleep returns 0, clock_nanosleep returns 0, FUTEX_WAIT
// (private and shared) and FUTEX_WAIT_BITSET return ETIMEDOUT, none early.
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;

static int64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

// A child that sends SIGRTMIN to the parent a few times while it waits.
static pid_t pester(void)
{
    pid_t parent = getpid();
    pid_t c = fork();
    if (c == 0) {
        for (int i = 0; i < 5; i++) {
            usleep(30 * 1000);
            kill(parent, SIGRTMIN);
        }
        _exit(0);
    }
    return c;
}

static void check(const char *what, long r, int err, long want, int want_err, int64_t ms)
{
    int good = r == want && (want == 0 || err == want_err) && ms >= 290;
    printf("  %s  %s: %ld (%s) after %lld ms\n", good ? "OK " : "MAL", what, r,
           r ? strerror(err) : "ok", (long long)ms);
    if (!good)
        fails++;
}

int main(void)
{
    sigset_t rt;
    sigemptyset(&rt);
    sigaddset(&rt, SIGRTMIN);
    sigprocmask(SIG_BLOCK, &rt, NULL);
    struct timespec t300 = { 0, 300 * 1000 * 1000 };
    static uint32_t word;

    for (int k = 0; k < 5; k++) {
        pid_t c = pester();
        int64_t t0 = mono_ms();
        long r = 0;
        int err = 0;
        const char *what = "";
        long want = 0;
        int want_err = 0;
        switch (k) {
        case 0:
            what = "nanosleep 300 ms";
            r = nanosleep(&t300, NULL); err = errno;
            break;
        case 1: {
            what = "clock_nanosleep TIMER_ABSTIME +300 ms";
            struct timespec d;
            clock_gettime(CLOCK_MONOTONIC, &d);
            d.tv_nsec += 300 * 1000 * 1000;
            if (d.tv_nsec >= 1000000000) { d.tv_sec++; d.tv_nsec -= 1000000000; }
            r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &d, NULL);   // returns the error
            err = (int)r;
            break;
        }
        case 2:
            what = "FUTEX_WAIT_PRIVATE 300 ms";
            r = syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, &t300, NULL, 0); err = errno;
            want = -1; want_err = ETIMEDOUT;
            break;
        case 3:
            what = "FUTEX_WAIT (shared) 300 ms";
            r = syscall(SYS_futex, &word, FUTEX_WAIT, 0, &t300, NULL, 0); err = errno;
            want = -1; want_err = ETIMEDOUT;
            break;
        case 4: {
            what = "FUTEX_WAIT_BITSET_PRIVATE +300 ms";
            struct timespec d;
            clock_gettime(CLOCK_MONOTONIC, &d);
            d.tv_nsec += 300 * 1000 * 1000;
            if (d.tv_nsec >= 1000000000) { d.tv_sec++; d.tv_nsec -= 1000000000; }
            r = syscall(SYS_futex, &word, FUTEX_WAIT_BITSET_PRIVATE, 0, &d, NULL, FUTEX_BITSET_MATCH_ANY);
            err = errno;
            want = -1; want_err = ETIMEDOUT;
            break;
        }
        }
        check(what, r, err, want, want_err, mono_ms() - t0);
        waitpid(c, NULL, 0);
    }
    sigset_t p;
    sigpending(&p);
    int pending = sigismember(&p, SIGRTMIN);
    printf("  %s  SIGRTMIN still pending, never handled: %d\n", pending ? "OK " : "MAL", pending);
    if (!pending)
        fails++;
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
