// Microbenchmark of the two calls a game's main loop makes most when it has
// nothing to wait for: epoll_wait with a timeout of 0 (SDL's event pump, the
// Steam overlay's IPC poll) and FUTEX_WAKE with nobody parked (every unlock
// of a glibc mutex that had a waiter once, every condvar signal). Counter-
// Strike 2's main thread spent 10 % in epoll waits and 7-12 % in __ulock_wake
// (benchmarks/stage52, section 4).
//
// Prints one line per case with the median and the mean nanoseconds per
// call. Exit status 0 when every call returned what Linux would (0 events,
// 0 waiters woken); checks are few, the numbers are the point. The runtime's
// LXRT_BENCH_ITERS overrides the iteration count.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what) { if (!ok) { printf("  MAL  %s\n", what); fails++; } }

static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

// Time `iters` calls of fn one by one (the per-call cost, not the throughput
// of a tight loop: a game makes one of these between two frames' worth of
// other work). The clock reads themselves cost ~20 ns each on the vDSO.
static int g_iters = 20000;
static void bench(const char *name, long (*fn)(void *), void *arg, long expect)
{
    uint64_t *t = malloc(sizeof *t * (size_t)g_iters);
    long bad = 0;
    for (int i = 0; i < 200; i++)             // warm up
        fn(arg);
    for (int i = 0; i < g_iters; i++) {
        uint64_t a = now_ns();
        long r = fn(arg);
        uint64_t b = now_ns();
        t[i] = b - a;
        if (r != expect)
            bad++;
    }
    qsort(t, (size_t)g_iters, sizeof *t, cmp_u64);
    double sum = 0;
    for (int i = 0; i < g_iters; i++)
        sum += (double)t[i];
    printf("  %-44s median %7.0f ns  mean %7.0f ns  p99 %7.0f ns%s\n", name,
           (double)t[g_iters / 2], sum / g_iters, (double)t[g_iters * 99 / 100],
           bad ? "  (WRONG RETURN)" : "");
    if (bad) {
        char what[128];
        snprintf(what, sizeof what, "%s: %ld of %d calls returned something other than %ld", name, bad, g_iters, expect);
        check(0, what);
    }
    free(t);
}

// ---------------------------------------------------------------- epoll

struct ep_case {
    int epfd;
    int maxevents;
    struct epoll_event ev[64];
    const sigset_t *mask;
};

static long do_epoll_wait0(void *p)
{
    struct ep_case *c = p;
    return epoll_wait(c->epfd, c->ev, c->maxevents, 0);
}

static long do_epoll_pwait0_mask(void *p)
{
    struct ep_case *c = p;
    return epoll_pwait(c->epfd, c->ev, c->maxevents, 0, c->mask);
}

static long do_epoll_pwait_raw(void *p)
{
    // The syscall as x86 glibc under FEX makes it: epoll_pwait with a NULL
    // sigmask (epoll_wait is not a syscall on aarch64 at all).
    struct ep_case *c = p;
    long r = syscall(SYS_epoll_pwait, c->epfd, c->ev, c->maxevents, 0, NULL, 8);
    return r < 0 ? -errno : r;
}

static long do_getppid(void *p)
{
    (void)p;
    return syscall(SYS_getppid);
}

struct poll_case { struct pollfd pf[9]; int n; };
static long do_poll0(void *p)
{
    struct poll_case *c = p;
    return poll(c->pf, (nfds_t)c->n, 0);     // ppoll with a zero timespec
}

static void epoll_cases(void)
{
    static struct ep_case c;
    c.maxevents = 64;
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    c.mask = &mask;

    int pipes[8][2];
    int evfds[8];
    for (int i = 0; i < 8; i++) {
        check(pipe2(pipes[i], O_NONBLOCK | O_CLOEXEC) == 0, "pipe2");
        evfds[i] = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        check(evfds[i] >= 0, "eventfd");
    }
    long ppid = getppid();
    bench("getppid (dispatch floor)", do_getppid, NULL, ppid);

    // poll(2) with a zero timeout on the same descriptors: the other way a
    // program asks "anything now?".
    {
        static struct poll_case pc;
        for (int n = 1; n <= 8; n *= 8) {
            pc.n = n;
            for (int i = 0; i < n; i++) { pc.pf[i].fd = pipes[i][0]; pc.pf[i].events = POLLIN; }
            char name[80];
            snprintf(name, sizeof name, "poll(0), %d pipe fd%s, nothing ready", n, n > 1 ? "s" : "");
            bench(name, do_poll0, &pc, 0);
        }
        int sv[2];
        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0, "socketpair");
        pc.pf[8].fd = sv[0]; pc.pf[8].events = POLLOUT; pc.n = 9;
        bench("poll(0), 8 pipes + 1 ready socket", do_poll0, &pc, 1);
        close(sv[1]);
        bench("poll(0), 8 pipes + 1 hung-up socket", do_poll0, &pc, 1);
        close(sv[0]);
        pc.n = 0;
        bench("poll(0), no fds", do_poll0, &pc, 0);

        // What poll(0) reports in the edge cases, printed so that two runs
        // of the runtime can be compared (LXRT_POLL0_SELECT=0 and 1).
        struct { const char *what; int fd; short events; } cases[6];
        int q[2], w[2], u[2];
        pipe2(q, O_NONBLOCK); close(q[1]);              // writer gone
        pipe2(w, O_NONBLOCK); close(w[0]);              // reader gone
        pipe2(u, O_NONBLOCK);
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv); close(sv[1]);
        int dead = dup(u[0]); close(dead);              // closed descriptor (nothing opened after it)
        cases[0] = (typeof(cases[0])){ "pipe read end, writer gone, POLLIN", q[0], POLLIN };
        cases[1] = (typeof(cases[0])){ "pipe write end, reader gone, POLLOUT", w[1], POLLOUT };
        cases[2] = (typeof(cases[0])){ "closed fd, POLLIN", dead, POLLIN };
        cases[3] = (typeof(cases[0])){ "pipe read end, writer gone, events 0", q[0], 0 };
        cases[4] = (typeof(cases[0])){ "socket, peer gone, POLLIN", sv[0], POLLIN };
        cases[5] = (typeof(cases[0])){ "live pipe write end, POLLIN|POLLOUT", u[1], POLLIN | POLLOUT };
        for (int i = 0; i < 6; i++) {
            struct pollfd one = { .fd = cases[i].fd, .events = cases[i].events, .revents = 0 };
            int r = poll(&one, 1, 0);
            printf("  poll(0) %-40s r=%d revents=0x%x\n", cases[i].what, r, (unsigned)one.revents);
        }
    }

    // Pipes, read interest only, nothing readable: 1, 2, 4, 8 of them.
    for (int n = 1; n <= 8; n *= 2) {
        c.epfd = epoll_create1(EPOLL_CLOEXEC);
        check(c.epfd >= 0, "epoll_create1");
        for (int i = 0; i < n; i++) {
            struct epoll_event e = { .events = EPOLLIN, .data.fd = pipes[i][0] };
            check(epoll_ctl(c.epfd, EPOLL_CTL_ADD, pipes[i][0], &e) == 0, "epoll_ctl ADD pipe");
        }
        char name[80];
        snprintf(name, sizeof name, "epoll_wait(0), %d pipe fd%s EPOLLIN", n, n > 1 ? "s" : "");
        bench(name, do_epoll_wait0, &c, 0);
        if (n == 1 || n == 8) {
            snprintf(name, sizeof name, "epoll_pwait(0, mask), %d pipe fd%s", n, n > 1 ? "s" : "");
            bench(name, do_epoll_pwait0_mask, &c, 0);
            snprintf(name, sizeof name, "raw epoll_pwait(0, NULL), %d pipe fd%s", n, n > 1 ? "s" : "");
            bench(name, do_epoll_pwait_raw, &c, 0);
        }
        close(c.epfd);
    }

    // eventfds, in and out interest (two knotes each), nothing readable: the
    // write side is always ready, so the wait returns n events. This is the
    // copy-out path.
    for (int n = 1; n <= 8; n *= 4) {
        c.epfd = epoll_create1(EPOLL_CLOEXEC);
        for (int i = 0; i < n; i++) {
            struct epoll_event e = { .events = EPOLLIN, .data.fd = evfds[i] };
            check(epoll_ctl(c.epfd, EPOLL_CTL_ADD, evfds[i], &e) == 0, "epoll_ctl ADD eventfd");
        }
        char name[80];
        snprintf(name, sizeof name, "epoll_wait(0), %d eventfd%s EPOLLIN", n, n > 1 ? "s" : "");
        bench(name, do_epoll_wait0, &c, 0);
        close(c.epfd);
    }
    {
        // One event ready: a unix socket pair with EPOLLIN|EPOLLOUT, the write
        // side ready on every call.
        int sv[2];
        check(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0, "socketpair");
        c.epfd = epoll_create1(EPOLL_CLOEXEC);
        struct epoll_event e = { .events = EPOLLIN | EPOLLOUT, .data.fd = sv[0] };
        check(epoll_ctl(c.epfd, EPOLL_CTL_ADD, sv[0], &e) == 0, "epoll_ctl ADD socket");
        for (int i = 0; i < 7; i++) {
            struct epoll_event p = { .events = EPOLLIN, .data.fd = pipes[i][0] };
            check(epoll_ctl(c.epfd, EPOLL_CTL_ADD, pipes[i][0], &p) == 0, "epoll_ctl ADD pipe");
        }
        bench("epoll_wait(0), 1 ready socket + 7 pipes", do_epoll_wait0, &c, 1);
        close(c.epfd);
        close(sv[0]);
        close(sv[1]);
    }
}

// ---------------------------------------------------------------- futex

static _Atomic uint32_t g_word;
static long futex_wake_private(void *p)
{
    (void)p;
    long r = syscall(SYS_futex, &g_word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    return r < 0 ? -errno : r;
}
static long futex_wake_shared(void *p)
{
    (void)p;
    long r = syscall(SYS_futex, &g_word, FUTEX_WAKE, 1, NULL, NULL, 0);
    return r < 0 ? -errno : r;
}
static long futex_wake_bitset_private(void *p)
{
    (void)p;
    long r = syscall(SYS_futex, &g_word, FUTEX_WAKE_BITSET_PRIVATE, INT32_MAX, NULL, NULL, FUTEX_BITSET_MATCH_ANY);
    return r < 0 ? -errno : r;
}

// A waiter parked on another word, in the same bucket of the runtime's parked
// table or not: the wake must still go nowhere.
static _Atomic uint32_t g_other[4096 * 2];
static _Atomic int g_parked_ready;
static void *parker(void *arg)
{
    _Atomic uint32_t *w = arg;
    atomic_store(&g_parked_ready, 1);
    syscall(SYS_futex, w, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    return NULL;
}

static long futex_wait_eagain(void *p)
{
    (void)p;
    // A wait whose word already changed: EAGAIN without parking (the common
    // "lock is free again" path of a mutex).
    long r = syscall(SYS_futex, &g_word, FUTEX_WAIT_PRIVATE, 12345, NULL, NULL, 0);
    return r < 0 ? -errno : r;
}

static void futex_cases(void)
{
    bench("futex WAKE_PRIVATE, no waiter", futex_wake_private, NULL, 0);
    bench("futex WAKE_BITSET_PRIVATE, no waiter", futex_wake_bitset_private, NULL, 0);
    bench("futex WAKE (shared), no waiter", futex_wake_shared, NULL, 0);
    bench("futex WAIT_PRIVATE, value differs (EAGAIN)", futex_wait_eagain, NULL, -EAGAIN);

    // Somebody parked on a different word that shares the bucket of g_word:
    // the runtime's table says "somebody", the kernel says "nobody".
    pthread_t th;
    _Atomic uint32_t *collide = &g_other[0];
    {
        // Find a word whose bucket collides: the hash is (a>>2 ^ a>>14) & 4095.
        uintptr_t a = (uintptr_t)&g_word >> 2;
        unsigned want = (unsigned)((a ^ (a >> 12)) & 4095);
        for (size_t i = 0; i < sizeof g_other / sizeof g_other[0]; i++) {
            uintptr_t b = (uintptr_t)&g_other[i] >> 2;
            if ((unsigned)((b ^ (b >> 12)) & 4095) == want) { collide = &g_other[i]; break; }
        }
    }
    atomic_store(&g_parked_ready, 0);
    pthread_create(&th, NULL, parker, collide);
    while (!atomic_load(&g_parked_ready)) ;
    usleep(20000);
    bench("futex WAKE_PRIVATE, no waiter, bucket shared", futex_wake_private, NULL, 0);
    atomic_store(collide, 1);
    syscall(SYS_futex, collide, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    pthread_join(th, NULL);
}

int main(void)
{
    const char *e = getenv("LXRT_BENCH_ITERS");
    if (e && atoi(e) > 0)
        g_iters = atoi(e);
    printf("epoll/futex microbenchmark, %d calls per case\n", g_iters);
    epoll_cases();
    futex_cases();
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
}
