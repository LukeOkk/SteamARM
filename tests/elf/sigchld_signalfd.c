// A parent that blocks SIGCHLD and reads it from a signalfd, as Steam's
// fossilize_replay master does (SIGCHLD blocked, disposition left at SIG_DFL,
// the signalfd watched through epoll with a non-blocking read): the child's
// end must show up as a signalfd_siginfo. Linux keeps a blocked SIGCHLD
// pending whatever its disposition ("blocked signals are never ignored",
// kernel/signal.c sig_ignored). Darwin discards a signal whose disposition
// is SIG_DFL-ignore (SIGCHLD, SIGWINCH, SIGURG, SIGINFO) at post time, blocked
// or not, and binds a pending one to a single thread (sig_stranded.c).
//
// Freestanding static-pie, raw syscalls. Checks, each with its own child:
//   dfl-epoll     SIG_DFL, epoll_wait(signalfd) then read (fossilize's shape)
//   dfl-read      SIG_DFL, blocking read on the signalfd
//   handler-epoll a guest handler for SIGCHLD installed (never runs: blocked)
//   dfl-poll      SIG_DFL, ppoll on the signalfd then read
//   dfl-batch     SIG_DFL, three children end, read reports SIGCHLD and
//                 wait4 reaps all three (Linux merges them: >= 1 siginfo)
//   dfl-epoll-mt  the signalfd is read on a second thread (clone with
//                 CLONE_THREAD), both threads block SIGCHLD; the first thread
//                 forks the child and waits
// Each check waits up to 2 s. Last line: "== sigchld_signalfd: N ok, M mal".
typedef unsigned long u64;
typedef unsigned int u32;
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
#define sys4(n, a, b, c, d) sys6(n, (i64)(a), (i64)(b), (i64)(c), (i64)(d), 0, 0)
enum { NR_epoll_create1 = 20, NR_epoll_ctl = 21, NR_epoll_pwait = 22, NR_close = 57,
       NR_read = 63, NR_write = 64, NR_ppoll = 73, NR_signalfd4 = 74, NR_exit = 93,
       NR_exit_group = 94, NR_nanosleep = 101, NR_clock_gettime = 113, NR_kill = 129,
       NR_rt_sigaction = 134, NR_rt_sigprocmask = 135, NR_getpid = 172,
       NR_clone = 220, NR_mmap = 222, NR_wait4 = 260 };
enum { SIGKILL = 9, SIGCHLD = 17 };
enum { SIG_BLOCK = 0, SIG_UNBLOCK = 1, WNOHANG = 1 };
enum { EPOLLIN = 1, EPOLL_CTL_ADD = 1, EAGAIN = 11, SFD_NONBLOCK = 0x800, SFD_CLOEXEC = 0x80000 };
enum { CLONE_VM = 0x100, CLONE_FS = 0x200, CLONE_FILES = 0x400, CLONE_SIGHAND = 0x800,
       CLONE_THREAD = 0x10000, CLONE_SYSVSEM = 0x40000 };

struct epoll_event { u32 events; u64 data; } __attribute__((packed));
struct signalfd_siginfo { u32 ssi_signo; int ssi_errno, ssi_code; u32 ssi_pid, ssi_uid; int ssi_fd;
                          u32 ssi_tid, ssi_band, ssi_overrun, ssi_trapno; int ssi_status;
                          u32 pad[21]; };
_Static_assert(sizeof(struct signalfd_siginfo) == 128, "signalfd ABI");

static void out(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    sys3(NR_write, 1, s, n);
}
static void outn(i64 v)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    int neg = v < 0;
    u64 u = neg ? (u64)-v : (u64)v;
    do { b[--i] = (char)('0' + u % 10); u /= 10; } while (u);
    if (neg) b[--i] = '-';
    out(b + i);
}
static int oks, mals;
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
static i64 fork_(void) { return sys6(NR_clone, SIGCHLD, 0, 0, 0, 0, 0); }

static volatile int n_handler;
__attribute__((used, noinline)) static void on_chld(int s) { (void)s; n_handler++; }
static void install_handler(void)
{
    u64 h;
    __asm__("adrp %0, on_chld\n add %0, %0, :lo12:on_chld" : "=r"(h));
    struct { u64 handler, flags, restorer, mask; } sa = { h, 0, 0, 0 };
    sys6(NR_rt_sigaction, SIGCHLD, (i64)&sa, 0, 8, 0, 0);
}
static void reset_handler(void)
{
    struct { u64 handler, flags, restorer, mask; } sa = { 0, 0, 0, 0 };
    sys6(NR_rt_sigaction, SIGCHLD, (i64)&sa, 0, 8, 0, 0);
}

static i64 spawn_exiting_child(int after_ms)
{
    i64 c = fork_();
    if (c == 0) {
        if (after_ms) sleep_ms(after_ms);
        sys3(NR_exit_group, 0, 0, 0);
    }
    return c;
}

// Waits on the signalfd the way `how` says, up to 2 s; returns the number of
// siginfo records read (0 on timeout), *ms the wait, *why a note.
enum { HOW_EPOLL, HOW_READ, HOW_PPOLL };
static i64 wait_signalfd(int sfd, int how, i64 *ms, const char **why)
{
    u64 t0 = now_ns();
    struct signalfd_siginfo si[4];
    i64 got = 0;
    *why = "";
    if (how == HOW_READ) {
        // The blocking read; a watchdog child kills the test on 2 s? No: use
        // a non-blocking fd and poll with ppoll in HOW_PPOLL instead; here
        // the fd was created without SFD_NONBLOCK and read blocks. A sibling
        // alarm is not available freestanding, so the deadline is enforced by
        // the child that ends: if read never returns the harness's deadline
        // (tests/elf/run.sh) ends the run.
        i64 r = sys3(NR_read, sfd, si, sizeof si);
        *ms = (i64)((now_ns() - t0) / 1000000);
        if (r < 0) { *why = "read failed"; return r; }
        return r / (i64)sizeof si[0];
    }
    int ep = -1;
    if (how == HOW_EPOLL) {
        ep = (int)sys3(NR_epoll_create1, 0, 0, 0);
        struct epoll_event ev = { EPOLLIN, (u64)sfd };
        sys4(NR_epoll_ctl, ep, EPOLL_CTL_ADD, sfd, &ev);
    }
    while (now_ns() - t0 < 2000000000ull) {
        i64 left = 2000 - (i64)((now_ns() - t0) / 1000000);
        if (left < 0) left = 0;
        i64 n;
        if (how == HOW_EPOLL) {
            struct epoll_event ev[4];
            n = sys6(NR_epoll_pwait, ep, (i64)ev, 4, left, 0, 8);
        } else {
            struct { int fd; short events, revents; } p = { sfd, 1, 0 };
            i64 ts[2] = { left / 1000, (left % 1000) * 1000000 };
            n = sys6(NR_ppoll, (i64)&p, 1, (i64)ts, 0, 8, 0);
        }
        if (n <= 0) break;
        i64 r = sys3(NR_read, sfd, si, sizeof si);
        if (r > 0) { got = r / (i64)sizeof si[0]; break; }
        if (r == -EAGAIN) { *why = " (woke, read EAGAIN)"; continue; }
        *why = " (read failed)"; got = r; break;
    }
    if (ep >= 0) sys3(NR_close, ep, 0, 0);
    *ms = (i64)((now_ns() - t0) / 1000000);
    if (got > 0 && si[0].ssi_signo != SIGCHLD) *why = " (not SIGCHLD)";
    return got;
}

static int reap_all(void)
{
    int st, n = 0;
    while (sys6(NR_wait4, -1, (i64)&st, WNOHANG, 0, 0, 0) > 0) n++;
    return n;
}
static void verdict(const char *name, i64 got, i64 ms, int reaped, int want_reaped, const char *why)
{
    int ok = got >= 1 && reaped == want_reaped;
    out(ok ? "  ok   " : "  MAL  "); out(name); out(": ");
    if (got >= 1) { out("SIGCHLD read from the signalfd "); outn(ms); out(" ms after fork, "); }
    else if (got == 0) { out("nothing on the signalfd within 2 s, "); }
    else { out("read error "); outn(got); out(", "); }
    outn(reaped); out("/"); outn(want_reaped); out(" reaped"); out(why); out("\n");
    if (ok) oks++; else mals++;
}

static int make_sfd(int flags)
{
    u64 m = bit(SIGCHLD);
    return (int)sys4(NR_signalfd4, -1, &m, 8, flags);
}

static void one(const char *name, int how, int with_handler, int children)
{
    if (with_handler) install_handler(); else reset_handler();
    mask(SIG_BLOCK, bit(SIGCHLD));
    int sfd = make_sfd(how == HOW_READ ? 0 : SFD_NONBLOCK | SFD_CLOEXEC);
    for (int i = 0; i < children; i++) spawn_exiting_child(20 * (i + 1));
    i64 ms; const char *why;
    i64 got = wait_signalfd(sfd, how, &ms, &why);
    if (children > 1) sleep_ms(100);   // let the last of the batch end
    int reaped = reap_all();
    verdict(name, got, ms, reaped, children, why);
    sys3(NR_close, sfd, 0, 0);
    mask(SIG_UNBLOCK, bit(SIGCHLD));
    reset_handler();
}

// dfl-epoll-mt: the reader is a second thread.
static volatile i64 mt_got, mt_ms, mt_done;
static const char *mt_why;
static int mt_sfd;
__attribute__((used, noinline)) static void mt_thread(void)
{
    mask(SIG_BLOCK, bit(SIGCHLD));
    i64 ms; const char *why;
    mt_got = wait_signalfd(mt_sfd, HOW_EPOLL, &ms, &why);
    mt_ms = ms; mt_why = why;
    __atomic_store_n(&mt_done, 1, __ATOMIC_RELEASE);
    sys3(NR_exit, 0, 0, 0);
}
static void multithreaded(void)
{
    mask(SIG_BLOCK, bit(SIGCHLD));
    mt_sfd = make_sfd(SFD_NONBLOCK | SFD_CLOEXEC);
    u64 stack = (u64)sys6(NR_mmap, 0, 1 << 20, 3, 0x22, -1, 0);
    u64 sp = (stack + (1 << 20)) & ~15ull;
    u64 fn;
    __asm__("adrp %0, mt_thread\n add %0, %0, :lo12:mt_thread" : "=r"(fn));
    // clone(flags, sp): the child returns here with x0 == 0 on the new stack;
    // branch to the thread body straight away.
    register i64 x8 __asm__("x8") = NR_clone;
    register i64 x0 __asm__("x0") = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD | CLONE_SYSVSEM;
    register i64 x1 __asm__("x1") = (i64)sp;
    register i64 x2 __asm__("x2") = 0;
    register i64 x3 __asm__("x3") = 0;
    register i64 x4 __asm__("x4") = 0;
    register i64 x9 __asm__("x9") = (i64)fn;
    __asm__ volatile("svc #0\n"
                     "cbnz x0, 1f\n"
                     "blr x9\n"
                     "1:\n"
                     : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x9) : "memory", "x30");
    i64 tid = x0;
    sleep_ms(50);
    spawn_exiting_child(20);
    i64 ms; const char *why;
    u64 t0 = now_ns();
    while (!__atomic_load_n(&mt_done, __ATOMIC_ACQUIRE) && now_ns() - t0 < 2500000000ull) sleep_ms(5);
    sleep_ms(50);
    int reaped = reap_all();
    if (!mt_done) { mals++; out("  MAL  dfl-epoll-mt: the reader thread never returned\n"); }
    else { ms = mt_ms; why = mt_why; verdict("dfl-epoll-mt", mt_got, ms, reaped, 1, why); }
    (void)tid;
    sys3(NR_close, mt_sfd, 0, 0);
    mask(SIG_UNBLOCK, bit(SIGCHLD));
}

void _start(void)
{
    one("dfl-epoll", HOW_EPOLL, 0, 1);
    one("dfl-ppoll", HOW_PPOLL, 0, 1);
    one("handler-epoll", HOW_EPOLL, 1, 1);
    if (n_handler) { out("  MAL  handler-epoll: the handler ran although SIGCHLD was blocked\n"); mals++; }
    one("dfl-batch", HOW_EPOLL, 0, 3);
    multithreaded();
    // Last: a blocking read has no deadline of its own. A sibling kills the
    // process after 3 s so the run still ends with a verdict line.
    i64 me = sys3(NR_getpid, 0, 0, 0);
    i64 dog = fork_();
    if (dog == 0) {
        sleep_ms(3000);
        out("  MAL  dfl-read: the blocking read never returned (watchdog)\n");
        out("== sigchld_signalfd: "); outn(oks); out(" ok, "); outn(mals + 1); out(" mal\n");
        sys3(NR_kill, me, SIGKILL, 0);
        sys3(NR_exit_group, 0, 0, 0);
    }
    one("dfl-read", HOW_READ, 0, 1);
    sys3(NR_kill, dog, SIGKILL, 0);
    sleep_ms(20); reap_all();

    out("== sigchld_signalfd: "); outn(oks); out(" ok, "); outn(mals); out(" mal\n");
    sys3(NR_exit_group, mals ? 1 : 0, 0, 0);
}
