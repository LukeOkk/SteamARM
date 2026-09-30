// A process-directed signal that arrives while every thread blocks it must
// stay pending for the process and be delivered as soon as a thread unblocks
// it (Linux: the shared pending set). XNU instead binds the signal to one
// thread when it is posted; if no thread accepts it at that moment it goes to
// the process's first thread -- under lxrun the host main thread, which blocks
// everything for good -- and stays there (MEASURED on macOS 27,
// benchmarks/stage28-android-reliability.txt). A shell that blocks SIGCHLD
// around fork and then waits in sigsuspend never woke: the intermittent hang
// of x86-64 Android's mksh under FEX (stage 27), and any guest can hit it.
//
// Freestanding static-pie, raw syscalls. Checks:
//   chld     SIGCHLD blocked, a child exits, 150 ms later SIGCHLD is unblocked:
//            the handler runs (and reaps the child)
//   suspend  the same, but the wait is rt_sigsuspend(empty set), as mksh's
//            j_waitj; a watchdog child sends SIGUSR2 after 2 s if it hangs
//   kill     SIGUSR1 blocked, a child kill()s the parent and exits; unblocking
//            SIGUSR1 runs the handler
// Last line: "== sig_stranded: N ok, M mal".
typedef unsigned long u64;
typedef long i64;

static i64 sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;
    register i64 x3 __asm__("x3") = d;
    register i64 x4 __asm__("x4") = e;
    register i64 x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
    return x0;
}
#define sys3(n, a, b, c) sys6(n, (i64)(a), (i64)(b), (i64)(c), 0, 0, 0)
enum { NR_write = 64, NR_exit_group = 94, NR_nanosleep = 101, NR_clock_gettime = 113,
       NR_kill = 129, NR_rt_sigsuspend = 133, NR_rt_sigaction = 134, NR_rt_sigprocmask = 135,
       NR_getpid = 172, NR_getppid = 173, NR_clone = 220, NR_wait4 = 260 };
enum { SIGUSR1 = 10, SIGUSR2 = 12, SIGKILL = 9, SIGCHLD = 17 };
enum { SIG_BLOCK = 0, SIG_UNBLOCK = 1, SIG_SETMASK = 2, WNOHANG = 1 };

static void out(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    sys3(NR_write, 1, s, n);
}
static void outn(u64 v)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(b + i);
}
static int oks, mals;
static void verdict(const char *name, int ok, const char *why)
{
    out(ok ? "  ok   " : "  MAL  "); out(name); out(": "); out(why); out("\n");
    if (ok) oks++; else mals++;
}
static u64 now_ns(void)
{
    i64 ts[2];
    sys3(NR_clock_gettime, 1, ts, 0);
    return (u64)ts[0] * 1000000000ull + (u64)ts[1];
}
static void sleep_ms(i64 ms)
{
    i64 ts[2] = { ms / 1000, (ms % 1000) * 1000000 };
    sys3(NR_nanosleep, ts, 0, 0);
}
static u64 bit(int s) { return 1ull << (s - 1); }
static void mask(int how, u64 set) { sys6(NR_rt_sigprocmask, how, (i64)&set, 0, 8, 0, 0); }

static volatile int n_chld, n_usr1, n_usr2, reaped;
__attribute__((used, noinline)) static void on_signal(int s)
{
    if (s == SIGCHLD) {
        n_chld++;
        int st;
        while (sys6(NR_wait4, -1, (i64)&st, WNOHANG, 0, 0, 0) > 0)
            reaped++;
    } else if (s == SIGUSR1) {
        n_usr1++;
    } else if (s == SIGUSR2) {
        n_usr2++;
    }
}
static void handle(int s)
{
    // The address pc-relative: a static-pie that relocates nothing must not
    // take it from a data word (an R_AARCH64_RELATIVE nobody applies).
    u64 h;
    __asm__("adrp %0, on_signal\n add %0, %0, :lo12:on_signal" : "=r"(h));
    struct { u64 handler, flags, restorer, mask; } sa;
    sa.handler = h; sa.flags = 0; sa.restorer = 0; sa.mask = 0;
    sys6(NR_rt_sigaction, s, (i64)&sa, 0, 8, 0, 0);
}
static i64 fork_(void) { return sys6(NR_clone, SIGCHLD, 0, 0, 0, 0, 0); }

// Waits up to 2 s for *counter to move past `before`; returns the ms it took, or -1.
static i64 wait_for(volatile int *counter, int before)
{
    u64 t0 = now_ns();
    while (now_ns() - t0 < 2000000000ull) {
        if (*counter > before)
            return (i64)((now_ns() - t0) / 1000000);
        sleep_ms(1);
    }
    return -1;
}
static void report(const char *name, i64 ms, const char *what)
{
    char why[160];
    int n = 0;
    const char *a = ms >= 0 ? "delivered " : "not delivered within 2 s of the unblock";
    for (int i = 0; a[i]; i++) why[n++] = a[i];
    why[n] = 0;
    out(ms >= 0 ? "  ok   " : "  MAL  "); out(name); out(": "); out(what); out(", "); out(why);
    if (ms >= 0) { outn((u64)ms); out(" ms after the unblock"); }
    out("\n");
    if (ms >= 0) oks++; else mals++;
}

void _start(void)
{
    handle(SIGCHLD);
    handle(SIGUSR1);
    handle(SIGUSR2);

    // chld
    mask(SIG_BLOCK, bit(SIGCHLD));
    i64 c = fork_();
    if (c == 0) sys3(NR_exit_group, 0, 0, 0);
    sleep_ms(150);
    int before = n_chld;
    mask(SIG_UNBLOCK, bit(SIGCHLD));
    i64 ms = wait_for(&n_chld, before);
    report("chld", ms, "SIGCHLD of a child that exited while it was blocked");
    if (ms >= 0 && reaped < 1)
        verdict("chld reap", 0, "the handler ran but wait4 found no child");

    // suspend
    int r0 = reaped;
    mask(SIG_BLOCK, bit(SIGCHLD));
    i64 parent = sys3(NR_getpid, 0, 0, 0);
    i64 dog = fork_();
    if (dog == 0) {
        sleep_ms(2000);
        sys3(NR_kill, parent, SIGUSR2, 0);
        sys3(NR_exit_group, 0, 0, 0);
    }
    c = fork_();
    if (c == 0) sys3(NR_exit_group, 0, 0, 0);
    sleep_ms(150);
    before = n_chld;
    int u2 = n_usr2;
    u64 t0 = now_ns();
    u64 empty = 0;
    sys6(NR_rt_sigsuspend, (i64)&empty, 8, 0, 0, 0, 0);
    ms = (i64)((now_ns() - t0) / 1000000);
    int got = n_chld > before && n_usr2 == u2;
    sys3(NR_kill, dog, SIGKILL, 0);
    mask(SIG_UNBLOCK, bit(SIGCHLD));
    if (got) {
        out("  ok   suspend: rt_sigsuspend(empty) returned for the SIGCHLD of a child that exited while it was blocked, ");
        outn((u64)ms); out(" ms\n"); oks++;
    } else {
        out("  MAL  suspend: rt_sigsuspend woke only for the 2 s watchdog (SIGCHLD ");
        outn((u64)(n_chld - before)); out(", SIGUSR2 "); outn((u64)(n_usr2 - u2)); out(")\n"); mals++;
    }
    for (int i = 0; i < 200 && reaped < r0 + 2; i++) sleep_ms(5);

    // kill
    mask(SIG_BLOCK, bit(SIGUSR1));
    c = fork_();
    if (c == 0) {
        sys3(NR_kill, sys3(NR_getppid, 0, 0, 0), SIGUSR1, 0);
        sys3(NR_exit_group, 0, 0, 0);
    }
    sleep_ms(150);
    before = n_usr1;
    int pre = n_usr1;
    mask(SIG_UNBLOCK, bit(SIGUSR1));
    ms = wait_for(&n_usr1, before);
    (void)pre;
    report("kill", ms, "SIGUSR1 another process sent while it was blocked");

    out("== sig_stranded: "); outn((u64)oks); out(" ok, "); outn((u64)mals); out(" mal\n");
    sys3(NR_exit_group, mals ? 1 : 0, 0, 0);
}
