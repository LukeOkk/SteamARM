// A thread blocked in a wait gets SIGUSR1 from ANOTHER process (tgkill), as
// wineserver does to hand a thread an APC under fsync or NTSYNC. The handler
// must run, the process must live, and the wait must end when its word
// changes. Under FEX the signal used to arrive nested in the runtime's
// carrier handler, FEX redirected that frame to its dispatcher, and the
// process aborted (runtime/signal.c; benchmarks/stage50). Built for aarch64
// and for x86-64 (tests/elf/run_fex_rules.sh).
//
//   xproc_signal_wait [restart|norestart] [waitv|futex|sleep]
#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
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
static volatile int handled;
static void on_usr1(int s, siginfo_t *si, void *uc) { (void)s; (void)si; (void)uc; handled++; }

int main(int argc, char **argv)
{
    int restart = argc > 1 && !strcmp(argv[1], "restart");
    const char *how = argc > 2 ? argv[2] : "waitv";
    uint32_t *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    struct sigaction sa = {0};
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | (restart ? SA_RESTART : 0);
    stack_t ss = {.ss_sp = malloc(1 << 16), .ss_size = 1 << 16};
    sigaltstack(&ss, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    pid_t me = getpid();
    long tid = syscall(SYS_gettid);
    pid_t c = fork();
    if (c == 0) {
        usleep(300000);
        long r = syscall(SYS_tgkill, me, tid, SIGUSR1);
        usleep(300000);
        w[0] = 1;
        syscall(SYS_futex, w, FUTEX_WAKE, 1, NULL, NULL, 0);
        _exit(r ? 9 : 0);
    }
    struct waitv wv = {0, (uint64_t)(uintptr_t)w, 0x02 /* a 32-bit word, shared */, 0};
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    ts.tv_sec += 5;
    long r;
    int eintr = 0;
    for (;;) {
        if (!strcmp(how, "futex")) {
            struct timespec rel = {5, 0};
            r = syscall(SYS_futex, w, FUTEX_WAIT, 0, &rel, NULL, 0);
        } else if (!strcmp(how, "sleep")) {
            struct timespec rel = {0, 600000000};
            r = nanosleep(&rel, NULL);
            while (!w[0]) usleep(1000);
            if (r < 0 && errno == EINTR) eintr++;
            r = 0;
            break;
        } else
            r = syscall(SYS_futex_waitv, &wv, 1, 0, &ts, CLOCK_MONOTONIC);
        if (r < 0 && errno == EINTR) { eintr++; continue; }
        break;
    }
    int e = errno, st = 0;
    waitpid(c, &st, 0);
    printf("%s -> %ld errno %d, EINTR %d times, handler ran %d, sender rc %d\n", how, r, r < 0 ? e : 0, eintr, handled,
           WEXITSTATUS(st));
    int ok = handled == 1 && (r == 0 || (r < 0 && e == EAGAIN)) && w[0] == 1 && WEXITSTATUS(st) == 0;
    printf("== xproc_signal_wait %s%s: %s\n", how, restart ? " (SA_RESTART)" : "", ok ? "ok" : "FAIL");
    return !ok;
}
