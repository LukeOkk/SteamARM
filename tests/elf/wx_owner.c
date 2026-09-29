// One owner for the protection of every split host page, no executable guest
// byte the scan has not seen, and fault handling that a guest signal handler
// cannot deadlock (runtime/wxsplit.c, runtime/subpage.c, runtime/signal.c).
// Each case is a sequence the stage 23 review found, written as a guest
// would, with nothing but mmap/mprotect and stores:
//
//   mixed      a 16 KiB host page holding W^X-table pages and a 4 KiB RWX
//              page only subpage.c knew about: code in the second ran
//              unscanned after a fetch from the first;
//   guard      a 4 KiB PROT_NONE guard made the host page executable with
//              the code already written into it unscanned;
//   rxseal     code written read-write and sealed read-execute 4 KiB at a
//              time was never scanned (with and without a writable
//              neighbour in the host page);
//   emulate    a store the runtime performs (storemu.c) resealed its host
//              page executable over what another thread had just written;
//   nested     an asynchronous signal whose handler runs code on a split
//              page, arriving while the runtime held the page lock;
//   sigign     SIGSEGV/SIGBUS ignored by the guest: the runtime's own flips
//              were ignored with them (a thread spinning on one pc), and a
//              real fault must still kill as Linux's force_sig does;
//   misaligned no SIGBUS action (lxrun's first handler): an alignment fault
//              in a W^X page was taken for a protection flip and retried
//              forever.
//
// A `svc` the runtime did not rewrite runs as a Darwin syscall: x16 = 20
// makes that getpid, where the rewritten one is Linux getppid (x8 = 173), so
// every generated call says which it was.
//
// Modes: (none) = mixed, guard, rxseal, emulate, nested; `sigign`;
// `misaligned stlr` (W^X page); `misaligned casal` (4 KiB split page).
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define KB 1024ul

static int ok, bad;
#define CHECK(c, ...) do { if (c) { ok++; printf("  OK   " __VA_ARGS__); } \
                           else { bad++; printf("  MAL  " __VA_ARGS__); } putchar('\n'); fflush(stdout); } while (0)

static const uint32_t RET = 0xd65f03c0;
typedef long (*fn_t)(void);
typedef void (*store_t)(volatile uint32_t *, uint32_t);

static void flush(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }

static fn_t emit_const(uint32_t *p, unsigned v)
{
    p[0] = 0x52800000u | ((v & 0xffffu) << 5);        // mov w0, #v
    p[1] = RET;
    flush(p, 8);
    return (fn_t)(uintptr_t)p;
}

// mov x16, #20 (Darwin getpid, if it runs live); mov x8, #173 (Linux
// getppid); svc #0; ret
static fn_t emit_ppid(uint32_t *p)
{
    p[0] = 0xd2800290;
    p[1] = 0xd28015a8;
    p[2] = 0xd4000001;
    p[3] = RET;
    flush(p, 16);
    return (fn_t)(uintptr_t)p;
}

// str w1, [x0]; ret
static store_t emit_store(uint32_t *p)
{
    p[0] = 0xb9000001;
    p[1] = RET;
    flush(p, 8);
    return (store_t)(uintptr_t)p;
}

static char *map(size_t len, int prot)
{
    char *p = mmap(NULL, len, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const int RWX = PROT_READ | PROT_WRITE | PROT_EXEC;
static long want;       // getppid(): what a rewritten svc returns

// ------------------------------------------------------------ mixed, guard

static void mixed(void)
{
    // W^X table for the whole range, then one 4 KiB page of it taken out and
    // put back RWX: that page is known to subpage.c only.
    char *r = map(64 * KB, PROT_NONE);
    if (!r || mprotect(r, 64 * KB, RWX) != 0 ||
        mprotect(r + 4 * KB, 4 * KB, PROT_NONE) != 0 ||
        mprotect(r + 4 * KB, 4 * KB, RWX) != 0) {
        printf("  skip mixed page (4 KiB mprotect refused)\n");
        return;
    }
    fn_t a = emit_const((uint32_t *)r, 11);
    fn_t b = emit_ppid((uint32_t *)(r + 4 * KB));
    long ra = a();                   // a fetch in the W^X-table page first
    long rb = b();                   // then the subpage-only page
    CHECK(ra == 11 && rb == want, "mixed host page: code at +0 = %ld, svc at +4 KiB returns %ld "
          "(want getppid %ld, %s)", ra, rb, want, rb == getpid() ? "LIVE Darwin getpid" : "-");
    // And again after both were written once more.
    a = emit_const((uint32_t *)r, 12);
    b = emit_ppid((uint32_t *)(r + 4 * KB));
    ra = a();
    rb = b();
    CHECK(ra == 12 && rb == want, "mixed host page, rewritten: %ld, svc %ld", ra, rb);
    munmap(r, 64 * KB);
}

static void guard(void)
{
    char *r = map(64 * KB, PROT_NONE);
    if (!r || mprotect(r, 64 * KB, RWX) != 0) {
        CHECK(0, "guard: RWX commit");
        return;
    }
    fn_t f = emit_ppid((uint32_t *)(r + 8 * KB));   // written, never run
    if (mprotect(r + 4 * KB, 4 * KB, PROT_NONE) != 0) {
        printf("  skip 4 KiB guard (mprotect refused)\n");
        return;
    }
    long got = f();
    CHECK(got == want, "4 KiB guard after code was written: svc returns %ld (want getppid %ld%s)",
          got, want, got == getpid() ? ", got LIVE Darwin getpid" : "");
    munmap(r, 64 * KB);
}

// ------------------------------------------------------------ rxseal

static void rxseal(void)
{
    // RW neighbours in the same host page.
    char *q = map(64 * KB, PROT_READ | PROT_WRITE);
    fn_t f = emit_ppid((uint32_t *)(q + 4 * KB));
    if (mprotect(q + 4 * KB, 4 * KB, PROT_READ | PROT_EXEC) != 0) {
        printf("  skip RX seal (4 KiB mprotect refused)\n");
        return;
    }
    long got = f();
    CHECK(got == want, "RW -> RX at 4 KiB beside RW pages: svc returns %ld (want getppid %ld%s)",
          got, want, got == getpid() ? ", got LIVE Darwin getpid" : "");
    q[0] = 1;                                         // the neighbour still takes stores
    got = f();
    CHECK(got == want && q[0] == 1, "... after a store beside it: %ld", got);

    // A host page with nothing writable left: no flip will ever come.
    char *q2 = map(64 * KB, PROT_READ | PROT_WRITE);
    f = emit_ppid((uint32_t *)q2);
    CHECK(mprotect(q2 + 4 * KB, 12 * KB, PROT_READ) == 0 &&
          mprotect(q2, 4 * KB, PROT_READ | PROT_EXEC) == 0, "RX at 4 KiB, read-only neighbours");
    got = f();
    CHECK(got == want, "... svc returns %ld (want getppid %ld%s)", got, want,
          got == getpid() ? ", got LIVE Darwin getpid" : "");
    munmap(q, 64 * KB);
    munmap(q2, 64 * KB);
}

// ------------------------------------------------------------ emulate

// Host page p: [0,4K) RWX code that stores into [4K,8K) (RW data): every
// call is a store executed from the page it writes, performed by the
// runtime. [8K,12K) RWX: generated code, written and run by another thread.
struct emu {
    store_t st;
    volatile uint32_t *data;
    atomic_int stop;
    atomic_long calls;
};
static void *emu_loop(void *arg)
{
    struct emu *e = arg;
    for (uint32_t i = 0; !atomic_load(&e->stop); i++) {
        e->st(e->data, i);
        atomic_fetch_add(&e->calls, 1);
    }
    return NULL;
}

static void emulate(void)
{
    char *p = map(64 * KB, PROT_READ | PROT_WRITE);
    if (!p || mprotect(p, 4 * KB, RWX) != 0 || mprotect(p + 8 * KB, 4 * KB, RWX) != 0) {
        printf("  skip emulate (4 KiB mprotect refused)\n");
        return;
    }
    struct emu e = { .st = emit_store((uint32_t *)p), .data = (volatile uint32_t *)(p + 4 * KB) };
    e.st(e.data, 1);
    CHECK(*e.data == 1, "store from its own host page performed");
    pthread_t th;
    pthread_create(&th, NULL, emu_loop, &e);
    while (atomic_load(&e.calls) < 100)
        ;
    long wrong = 0, live = 0, n = 0;
    double t0 = now();
    for (; n < 400 && now() - t0 < 20; n++) {
        fn_t f = emit_ppid((uint32_t *)(p + 8 * KB));
        long got = f();
        if (got != want) {
            wrong++;
            if (got == getpid()) live++;
        }
    }
    atomic_store(&e.stop, 1);
    pthread_join(th, NULL);
    CHECK(wrong == 0 && n >= 100, "generated code beside a store the runtime performs: %ld/%ld calls wrong "
          "(%ld LIVE Darwin getpid), %ld stores performed meanwhile", wrong, n, live,
          (long)atomic_load(&e.calls));
    munmap(p, 64 * KB);
}

// ------------------------------------------------------------ nested

// SIGUSR1 from a second thread (tgkill), every ~50 us, at the thread that
// keeps executing a store from the split page into it: some arrive while the
// runtime is inside that fault with the page open for writing, and the
// handler then runs code on the very same page.
static fn_t g_h;
static atomic_long g_hits, g_hwrong;
static void on_usr1(int sig)
{
    (void)sig;
    if (g_h() == 7) atomic_fetch_add(&g_hits, 1);
    else atomic_fetch_add(&g_hwrong, 1);
}
struct kicker { pthread_t target; atomic_int stop; atomic_long sent; };
static void *kick(void *arg)
{
    struct kicker *k = arg;
    struct timespec ts = { 0, 50000 };
    while (!atomic_load(&k->stop)) {
        pthread_kill(k->target, SIGUSR1);
        atomic_fetch_add(&k->sent, 1);
        nanosleep(&ts, NULL);
    }
    return NULL;
}

static void nested(void)
{
    char *p = map(64 * KB, PROT_READ | PROT_WRITE);
    if (!p || mprotect(p, 4 * KB, RWX) != 0 || mprotect(p + 8 * KB, 4 * KB, RWX) != 0) {
        printf("  skip nested (4 KiB mprotect refused)\n");
        return;
    }
    store_t st = emit_store((uint32_t *)p);
    volatile uint32_t *data = (volatile uint32_t *)(p + 4 * KB);
    g_h = emit_const((uint32_t *)(p + 8 * KB), 7);
    CHECK(g_h() == 7, "handler code on the split page runs");
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr1;
    sa.sa_flags = SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    struct kicker k = { .target = pthread_self() };
    pthread_t th;
    pthread_create(&th, NULL, kick, &k);
    long n = 0;
    double t0 = now();
    while ((now() - t0 < 1.0 || atomic_load(&g_hits) < 200) && now() - t0 < 20)
        st(data, (uint32_t)n++);
    atomic_store(&k.stop, 1);
    pthread_join(th, NULL);
    CHECK(atomic_load(&g_hits) >= 200 && atomic_load(&g_hwrong) == 0 && *data == (uint32_t)(n - 1),
          "signal handler running split-page code during %ld performed stores: %ld handler calls "
          "(%ld signals sent), %ld wrong", n, (long)atomic_load(&g_hits), (long)atomic_load(&k.sent),
          (long)atomic_load(&g_hwrong));
    munmap(p, 64 * KB);
}

// ------------------------------------------------------------ sigign

static int wait_child(pid_t kid, double limit)
{
    int st = -1;
    double t0 = now();
    for (;;) {
        pid_t r = waitpid(kid, &st, WNOHANG);
        if (r == kid) return st;
        if (now() - t0 > limit) {
            kill(kid, SIGKILL);
            waitpid(kid, &st, 0);
            return -1;
        }
        usleep(10000);
    }
}

static int sigign(void)
{
    signal(SIGSEGV, SIG_IGN);
    signal(SIGBUS, SIG_IGN);
    printf("SIGSEGV and SIGBUS ignored\n");
    fflush(stdout);
    // The runtime's own faults still arrive: a W^X page written and run.
    char *r = map(64 * KB, PROT_NONE);
    CHECK(r && mprotect(r, 64 * KB, RWX) == 0, "RWX commit");
    fn_t f = emit_const((uint32_t *)r, 5);
    long a = f();
    f = emit_const((uint32_t *)r, 6);
    long b = f();
    CHECK(a == 5 && b == 6, "W^X page written, run, written again, run: %ld %ld", a, b);
    // A 4 KiB split page the same way.
    char *q = map(64 * KB, PROT_READ | PROT_WRITE);
    if (q && mprotect(q, 4 * KB, RWX) == 0) {
        f = emit_const((uint32_t *)q, 8);
        a = f();
        q[5 * KB] = 1;
        f = emit_const((uint32_t *)q, 9);
        b = f();
        CHECK(a == 8 && b == 9 && q[5 * KB] == 1, "4 KiB split page written and run: %ld %ld", a, b);
    }
    // Sent, not raised: ignored as asked.
    kill(getpid(), SIGSEGV);
    CHECK(1, "kill(self, SIGSEGV) ignored");
    // Raised by an instruction: Linux resets the ignored disposition and the
    // process dies of it (force_sig_fault).
    fflush(stdout);
    pid_t kid = fork();
    if (kid == 0) {
        *(volatile int *)(uintptr_t)16 = 1;
        _exit(0);                       // survived: wrong
    }
    int st = wait_child(kid, 10);
    CHECK(st != -1 && WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV,
          "a real fault under SIG_IGN kills the process with SIGSEGV (status %#x%s)", st,
          st == -1 ? ": still running after 10 s, killed" : "");
    printf("== wx_owner sigign: %s (%d ok, %d mal)\n", bad ? "FAIL" : "ok", ok, bad);
    return bad ? 1 : 0;
}

// ------------------------------------------------------------ misaligned

static int misaligned(const char *how)
{
    uint64_t v = 0x1122334455667788ull;
    if (!strcmp(how, "stlr")) {
        // A W^X-table page, read-write at this point: a store-release that
        // crosses a 16-byte boundary is an alignment fault (DFSC 0x21, WnR 1).
        char *r = map(64 * KB, PROT_NONE);
        if (!r || mprotect(r, 64 * KB, RWX) != 0) return 2;
        printf("misaligned stlr into a W^X page\n");
        fflush(stdout);
        __asm__ volatile("stlr %0, [%1]" :: "r"(v), "r"(r + 12) : "memory");
    } else {
        // A 4 KiB split page whose union is RWX: a CASAL that crosses a
        // 16-byte boundary (DFSC 0x21, WnR 0).
        char *q = map(64 * KB, PROT_READ | PROT_WRITE);
        if (!q || mprotect(q, 4 * KB, RWX) != 0) return 2;
        emit_const((uint32_t *)q, 1)();                 // the page is read-execute now
        printf("misaligned casal into a split page\n");
        fflush(stdout);
        uint64_t c = 0;
        __asm__ volatile(".arch_extension lse\n\tcasal %0, %1, [%2]"
                         : "+r"(c) : "r"(v), "r"(q + 4 * KB + 12) : "memory");
    }
    printf("survived the misaligned access\n");
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    want = getppid();
    if (argc > 1 && !strcmp(argv[1], "sigign"))
        return sigign();
    if (argc > 2 && !strcmp(argv[1], "misaligned"))
        return misaligned(argv[2]);
    printf("getpid %ld, getppid %ld\n", (long)getpid(), want);
    mixed();
    guard();
    rxseal();
    emulate();
    nested();
    printf("== wx_owner: %s (%d ok, %d mal)\n", bad ? "FAIL" : "ok", ok, bad);
    return bad ? 1 : 0;
}
