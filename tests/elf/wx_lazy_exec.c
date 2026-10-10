// Lazy execute (runtime/wxsplit.c): with LXRT_LAZY_EXEC, a read-execute
// mprotect the W^X table takes leaves the host page READ-ONLY until its first
// instruction fetch, which scans it as the split's execute flip does, and a
// store into it is the guest's own fault -- delivered, never flipped
// writable. Mode 2 (the default in Wine's loaders) takes only ranges the
// table already holds (RWX made R-X: FEX's self-modifying-code trap), mode 1
// every aligned R-X request of private anonymous memory. Written as a guest
// would, with nothing but mmap/mprotect/mremap and stores:
//
//   basic   RWX commit, code written, R-X, called (r--p in /proc/self/maps
//           until then, r-xp after); a store into it raises SIGSEGV at that
//           address and does not land, the code still runs; RWX, rewritten,
//           R-X, the new code runs -- 200 rounds;
//   svc     a function with a `svc #0`, written under RWX and first run
//           after R-X: rewritten before it runs; a page scanned once,
//           written again under RWX with a svc and made R-X: rescanned; a
//           store into a page never fetched faults too;
//   again   R-X over a range already R-X (nothing to do), before and after
//           its first call;
//   smc     FEX's trap and untrap natively: the SIGSEGV handler makes the
//           page RWX and returns, the store lands, the page is written and
//           made R-X again, the new code runs; then the same with nothing
//           run natively between the traps, as FEX's x64 pages (rounds/s
//           printed for both);
//   rw      RW -> R-X, a range the table does not hold: lazy with mode 1
//           only; the svc is rewritten either way;
//   adopt   a 4 KiB R-X mprotect beside a lazy page's code hands the host
//           page to subpage.c (adopt_untracked), which has to scan the lazy
//           guest pages before the host page runs: the svc comes back
//           rewritten, a store still faults; the same with a 4 KiB
//           read-write MAP_FIXED mapping there, which makes the host page
//           writable and executable to the guest;
//   mremap  a lazy range moved and grown (MREMAP_MAYMOVE, no room in
//           place): it runs at the new address, its svc rewritten there,
//           stores fault, in the grown tail too (with lazy execute off, no
//           svc: one scanned in place is a PC-relative branch, which does
//           not survive a move -- the known limit, runtime/wxsplit.c);
//   fixed   MREMAP_FIXED of a shared memfd mapping over a range the table
//           holds: the destination leaves the table -- a fetch there faults
//           (the mapping is not executable), nothing is flipped or scanned
//           (through a second view the file still holds its svc), a store
//           lands in the file;
//   alias   the same for lxrt_alias (private syscall 0x4C580032) of private
//           memory over a held range;
//   race    3 threads call into a 1 MiB range while this one cycles RWX ->
//           write (a svc among it) -> R-X, 200 rounds: no thread faults,
//           every call is right, every svc rewritten;
//   racestress  the race in six processes at once, the CPU shared: the
//           threads are preempted at every point, and a caller's fault lands
//           in the middle of a protection change (with lazy execute off every
//           R-X empties the table, and the fault handler does not wait for
//           the lock of an empty table: the RWX entry must go in before the
//           host change, and a fault found stale while the page is back in
//           the table must be retried, not declined);
//   decline an RWX mprotect over a lazy page and a shared memfd page, which
//           the table declines (not all private anonymous) and the ordinary
//           path makes read-write: the lazy page's entry becomes RWX -- R-X
//           again makes stores fault, and stores land after a call;
//   brk     a held page in the heap, the heap shrunk below it and grown
//           back: fresh read-write memory, never flipped executable, and R-X
//           over it refuses stores;
//   adoptjit  lxrt_jit_wx(3) (dispatch.c jit_adopt) takes an RWX range of
//           the split as MAP_JIT and refuses a lazily executable one;
//   fetched a lazy range with one page run (read-execute beside read-only)
//           and an RWX range with one page run and one written, each moved
//           and grown by mremap: Linux moves the one VMA, and so must this;
//   freed   a 4 KiB munmap inside a held host page, the rest of the page then
//           run: the freed slot's svc is rewritten too, never live (RWX, and
//           lazy R-X);
//   keeprx  R-X over code that ran since its RWX commit: that page stays
//           r-xp, only the written one becomes r--p (no refault, no rescan).
//
// argv[1]: the LXRT_LAZY_EXEC of this run (0, 1 or 2), for what
// /proc/self/maps must show; every other check is the same in all three.
// argv[2...]: only these cases. A `svc` the runtime did not rewrite runs as
// a Darwin syscall: x16 = 20 makes that getpid, where the rewritten one is
// Linux getppid (x8 = 173).
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>

#define KB 1024ul
#define MB (1024ul * KB)
#define PG (16 * KB)

static int ok, bad;
#define CHECK(c, ...) do { if (c) { ok++; printf("  OK   " __VA_ARGS__); } \
                           else { bad++; printf("  MAL  " __VA_ARGS__); } putchar('\n'); fflush(stdout); } while (0)

static const int RWX = PROT_READ | PROT_WRITE | PROT_EXEC;
static const int RX = PROT_READ | PROT_EXEC;
static const uint32_t RET = 0xd65f03c0, SVC0 = 0xd4000001;
typedef long (*fn_t)(void);

static int g_mode;      // LXRT_LAZY_EXEC of this run
static long want;       // getppid(): what a rewritten svc returns

static void flush(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }

// mov w0, #v; ret
static fn_t emit_const(void *at, unsigned v)
{
    uint32_t *p = at;
    p[0] = 0x52800000u | ((v & 0xffffu) << 5);
    p[1] = RET;
    flush(p, 8);
    return (fn_t)(uintptr_t)p;
}

// mov x16, #20 (Darwin getpid, if it runs live); mov x8, #173 (Linux
// getppid); svc #0; ret
static fn_t emit_ppid(void *at)
{
    uint32_t *p = at;
    p[0] = 0xd2800290;
    p[1] = 0xd28015a8;
    p[2] = SVC0;
    p[3] = RET;
    flush(p, 16);
    return (fn_t)(uintptr_t)p;
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

static const char *live(long got) { return got == getpid() ? ", got LIVE Darwin getpid" : ""; }

// ------------------------------------------------------------ faults

// Per thread: a fault while armed jumps back, with its si_addr noted.
static __thread sigjmp_buf t_jb;
static __thread volatile int t_armed;
static __thread void *volatile t_addr;
// smc: a fault in this page is the trap, lifted in the handler.
static char *volatile g_smc_page;
static atomic_long g_smc_traps;

static void say_hex(const char *what, uint64_t v)
{
    char b[64];
    int n = 0;
    while (*what) b[n++] = *what++;
    b[n++] = '0';
    b[n++] = 'x';
    for (int s = 60; s >= 0; s -= 4)
        b[n++] = "0123456789abcdef"[(v >> s) & 15];
    b[n++] = '\n';
    (void)!write(2, b, (size_t)n);
}

static void on_fault(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    char *a = si->si_addr, *p = g_smc_page;
    if (p && a >= p && a < p + PG) {
        mprotect(p, PG, RWX);
        atomic_fetch_add(&g_smc_traps, 1);
        return;
    }
    if (t_armed) {
        t_addr = a;
        siglongjmp(t_jb, 1);
    }
    say_hex(sig == SIGBUS ? "wx_lazy_exec: unexpected SIGBUS at " : "wx_lazy_exec: unexpected SIGSEGV at ",
            (uint64_t)(uintptr_t)a);
    _exit(99);
}

// Does the store fault? (t_addr: where.)
static int store_faults(volatile void *at, uint32_t v)
{
    int f = 0;
    t_addr = NULL;
    t_armed = 1;
    if (sigsetjmp(t_jb, 1) == 0) *(volatile uint32_t *)at = v;
    else f = 1;
    t_armed = 0;
    return f;
}

// Does calling fn fault? (*got: what it returned otherwise.)
static int call_faults(fn_t fn, long *got)
{
    int f = 0;
    t_addr = NULL;
    t_armed = 1;
    if (sigsetjmp(t_jb, 1) == 0) *got = fn();
    else f = 1;
    t_armed = 0;
    return f;
}

// The protection /proc/self/maps shows for the region holding p ("r--p").
static const char *maps_prot(const void *p, char out[8])
{
    snprintf(out, 8, "?");
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return out;
    char line[512];
    unsigned long a = (unsigned long)(uintptr_t)p;
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[8];
        if (sscanf(line, "%lx-%lx %7s", &lo, &hi, perm) == 3 && lo <= a && a < hi) {
            snprintf(out, 8, "%s", perm);
            break;
        }
    }
    fclose(f);
    return out;
}

// A range the table holds read-write-execute: reserved, then committed RWX
// (as Wine commits a VirtualAlloc, as V8 commits its code range).
static char *held(size_t len)
{
    char *r = map(len, PROT_NONE);
    if (r && mprotect(r, len, RWX) != 0) {
        munmap(r, len);
        r = NULL;
    }
    return r;
}

// ------------------------------------------------------------ cases

static void basic(void)
{
    char *r = held(64 * KB);
    if (!r) { CHECK(0, "basic: RWX commit"); return; }
    fn_t f = emit_const(r, 41);
    CHECK(mprotect(r, 64 * KB, RX) == 0, "basic: RWX -> R-X");
    char p0[8], p1[8];
    const char *exp = g_mode ? "r--p" : "r-xp";
    maps_prot(r, p0);
    long got = f();
    maps_prot(r, p1);
    CHECK(got == 41 && !strcmp(p0, exp) && !strcmp(p1, "r-xp"),
          "basic: R-X code runs (%ld); /proc/self/maps %s before its first call, %s after (want %s, r-xp)",
          got, p0, p1, exp);
    volatile uint32_t *data = (volatile uint32_t *)(r + 8 * KB);
    uint32_t before = *data;
    int fl = store_faults(data, 0x12345678);
    void *where = t_addr;
    got = f();
    CHECK(fl && where == (void *)data && *data == before && got == 41,
          "basic: a store into it raises SIGSEGV at %p (si_addr %p), does not land (%#x), the code still runs (%ld)",
          (void *)data, where, *data, got);
    long wrong = 0, through = 0, perr = 0;
    for (int i = 0; i < 200; i++) {
        if (mprotect(r, 64 * KB, RWX) != 0) { perr++; break; }
        f = emit_const(r, 100 + i);
        if (mprotect(r, 64 * KB, RX) != 0) { perr++; break; }
        if (f() != 100 + i) wrong++;
        if (i % 20 == 0 && !store_faults(data, (uint32_t)i)) through++;
    }
    CHECK(wrong == 0 && through == 0 && perr == 0,
          "basic: 200 x RWX -> rewrite -> R-X -> call: %ld wrong, %ld stores let through, %ld mprotect errors",
          wrong, through, perr);
    munmap(r, 64 * KB);
}

static void svc(void)
{
    char *r = held(64 * KB);
    if (!r) { CHECK(0, "svc: RWX commit"); return; }
    fn_t c = emit_const(r, 1);
    long got = c();                                     // fetched under RWX: scanned, read-execute
    fn_t g = emit_ppid(r + 16 * KB);                    // written, never run
    CHECK(got == 1 && mprotect(r, 64 * KB, RX) == 0, "svc: code run under RWX, a svc written beside it, R-X");
    volatile uint32_t *u = (volatile uint32_t *)(r + 32 * KB);
    int fl = store_faults(u, 7);
    CHECK(fl && *u == 0, "svc: a store into an R-X page never fetched faults (si_addr %p), does not land",
          t_addr);
    got = g();
    CHECK(got == want, "svc: svc #0 written under RWX, first run after R-X, returns %ld (want getppid %ld%s)",
          got, want, live(got));
    // The page that was scanned before: written again under RWX, a svc now.
    CHECK(mprotect(r, 64 * KB, RWX) == 0, "svc: RWX again");
    g = emit_ppid(r);
    CHECK(mprotect(r, 64 * KB, RX) == 0, "svc: a svc written over scanned code, R-X");
    got = g();
    CHECK(got == want, "svc: ... rescanned before it runs: %ld (want getppid %ld%s)", got, want, live(got));
    munmap(r, 64 * KB);
}

static void again(void)
{
    char *r = held(64 * KB);
    if (!r) { CHECK(0, "again: RWX commit"); return; }
    fn_t f = emit_const(r, 9);
    char p0[8], p1[8];
    const char *exp = g_mode ? "r--p" : "r-xp";
    bool twice = mprotect(r, 64 * KB, RX) == 0 && mprotect(r, 64 * KB, RX) == 0;
    maps_prot(r, p0);
    long a = f();
    bool thrice = mprotect(r, 64 * KB, RX) == 0;
    maps_prot(r, p1);
    long b = f();
    CHECK(twice && thrice && a == 9 && b == 9 && !strcmp(p0, exp) && !strcmp(p1, "r-xp"),
          "again: R-X twice (%s), runs (%ld); R-X again after the call (%s), runs (%ld)", p0, a, p1, b);
    munmap(r, 64 * KB);
}

static void smc(void)
{
    char *r = held(64 * KB);
    if (!r) { CHECK(0, "smc: RWX commit"); return; }
    char *page = r + PG;
    volatile uint32_t *data = (volatile uint32_t *)(page + 8 * KB);
    fn_t f = emit_const(page, 0);
    CHECK(mprotect(page, PG, RX) == 0 && f() == 0, "smc: one 16 KiB page R-X, runs");
    enum { N = 2000 };
    long wrong = 0, perr = 0;
    atomic_store(&g_smc_traps, 0);
    double t0 = now();
    for (int i = 0; i < N; i++) {
        g_smc_page = page;
        *data = (uint32_t)i;                    // the trap: the handler lifts it, the store lands
        g_smc_page = NULL;
        f = emit_const(page, (unsigned)i);
        if (mprotect(page, PG, RX) != 0) perr++;  // and trapped again
        if (f() != (i & 0xffff) || *data != (uint32_t)i) wrong++;
    }
    double dt = now() - t0;
    CHECK(wrong == 0 && perr == 0 && atomic_load(&g_smc_traps) == N,
          "smc: %d x store -> SIGSEGV -> RWX in the handler -> store lands -> rewrite -> R-X -> call: "
          "%ld traps, %ld wrong, %ld mprotect errors (%.0f rounds/s)",
          N, (long)atomic_load(&g_smc_traps), wrong, perr, N / dt);
    // FEX's own pattern: the trapped bytes are x64 code nothing runs
    // natively, so no call between the traps.
    wrong = perr = 0;
    atomic_store(&g_smc_traps, 0);
    t0 = now();
    for (int i = 0; i < N; i++) {
        g_smc_page = page;
        *data = (uint32_t)i;
        g_smc_page = NULL;
        if (mprotect(page, PG, RX) != 0) perr++;
        if (*data != (uint32_t)i) wrong++;
    }
    dt = now() - t0;
    CHECK(wrong == 0 && perr == 0 && atomic_load(&g_smc_traps) == N,
          "smc: %d x store -> SIGSEGV -> RWX in the handler -> store lands -> R-X, no native call: "
          "%ld traps, %ld wrong, %ld mprotect errors (%.0f rounds/s)",
          N, (long)atomic_load(&g_smc_traps), wrong, perr, N / dt);
    munmap(r, 64 * KB);
}

static void rw(void)
{
    char *q = map(64 * KB, PROT_READ | PROT_WRITE);
    if (!q) { CHECK(0, "rw: mmap"); return; }
    fn_t g = emit_ppid(q + 4 * KB);
    char p0[8];
    const char *exp = g_mode == 1 ? "r--p" : "r-xp";
    bool prot = mprotect(q, 64 * KB, RX) == 0;
    maps_prot(q, p0);
    long got = g();
    int fl = store_faults(q + 32 * KB, 1);
    CHECK(prot && got == want && fl && !strcmp(p0, exp),
          "rw: RW -> R-X (%s, want %s): svc returns %ld (want getppid %ld%s), a store faults", p0, exp,
          got, want, live(got));
    munmap(q, 64 * KB);
}

static void adopt(void)
{
    char *r = held(64 * KB);
    if (!r) { CHECK(0, "adopt: RWX commit"); return; }
    fn_t g = emit_ppid(r);                              // host page 0, guest page 0: never run
    CHECK(mprotect(r, 64 * KB, RX) == 0, "adopt: svc written under RWX, R-X");
    if (mprotect(r + 12 * KB, 4 * KB, RX) != 0) {       // guest page 3: subpage.c takes the host page
        printf("  skip adopt (4 KiB mprotect refused: page size %d)\n", getpagesize());
        munmap(r, 64 * KB);
        return;
    }
    long got = g();
    int fl = store_faults(r + 4 * KB, 1);
    CHECK(got == want && fl, "adopt: host page taken by a 4 KiB R-X mprotect beside the code: svc returns %ld "
          "(want getppid %ld%s), a store into guest page 1 faults", got, want, live(got));
    munmap(r, 64 * KB);

    // The same with a 4 KiB read-write mapping placed there: the host page is
    // then writable and executable to the guest, split by subpage.c.
    r = held(64 * KB);
    if (!r) { CHECK(0, "adopt: RWX commit"); return; }
    g = emit_ppid(r);
    CHECK(mprotect(r, 64 * KB, RX) == 0, "adopt: svc written under RWX, R-X");
    char *q = mmap(r + 12 * KB, 4 * KB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (q != r + 12 * KB) {
        printf("  skip adopt with MAP_FIXED (4 KiB mapping refused)\n");
        munmap(r, 64 * KB);
        return;
    }
    q[0] = 1;                                           // the new page takes stores
    got = g();
    fl = store_faults(r + 64, 1);
    CHECK(got == want && fl && q[0] == 1, "adopt: host page taken by a 4 KiB MAP_FIXED read-write mapping: svc "
          "returns %ld (want getppid %ld%s), a store into the code's guest page faults", got, want, live(got));
    munmap(r, 64 * KB);
}

static void remap(void)
{
    // The lazy range is the first half; the second stays PROT_NONE, so the
    // range cannot grow in place and moves.
    char *m = map(128 * KB, PROT_NONE);
    if (!m || mprotect(m, 64 * KB, RWX) != 0) { CHECK(0, "mremap: RWX commit"); return; }
    emit_const(m, 3);
    // Lazy, the svc is first scanned where it runs, after the move. Scanned
    // in place (0: the eager R-X), it is a PC-relative branch by then, which
    // does not survive a move (the known limit, runtime/wxsplit.c's header).
    if (g_mode)
        emit_ppid(m + 16 * KB);
    // Never fetched: one host protection over all of it, which mremap.c needs.
    CHECK(mprotect(m, 64 * KB, RX) == 0, "mremap: code and a svc written under RWX, R-X");
    char *n = mremap(m, 64 * KB, 128 * KB, MREMAP_MAYMOVE);
    if (n == MAP_FAILED) {
        CHECK(0, "mremap: 64 -> 128 KiB with MREMAP_MAYMOVE failed");
        munmap(m, 128 * KB);
        return;
    }
    long a = ((fn_t)(uintptr_t)n)(), b = g_mode ? ((fn_t)(uintptr_t)(n + 16 * KB))() : want;
    int f1 = store_faults(n + 8 * KB, 1), f2 = store_faults(n + 80 * KB, 1);
    CHECK(n != m && a == 3 && b == want && f1 && f2,
          "mremap: moved %p -> %p and grown: code runs (%ld), svc returns %ld (want getppid %ld%s%s), "
          "stores fault (%d, grown tail %d)", (void *)m, (void *)n, a, b, want, live(b),
          g_mode ? "" : "; none moved with eager R-X", f1, f2);
    munmap(n, 128 * KB);
    if (n != m)
        munmap(m + 64 * KB, 64 * KB);
}

// A range the table holds, a page of it fetched (scanned, read-execute).
static char *held_run(void)
{
    char *d = held(64 * KB);
    if (d && emit_const(d, 4)() != 4) {
        munmap(d, 64 * KB);
        d = NULL;
    }
    return d;
}

static void fixed(void)
{
    char *d = held_run();
    int fd = memfd_create("wx_lazy_exec", MFD_CLOEXEC);
    if (!d || fd < 0 || ftruncate(fd, 64 * KB) != 0) { CHECK(0, "fixed: setup"); return; }
    char *s = mmap(NULL, 64 * KB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    volatile uint32_t *view = mmap(NULL, 64 * KB, PROT_READ, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED || view == MAP_FAILED) { CHECK(0, "fixed: memfd mappings"); return; }
    emit_ppid(s);
    char *t = mremap(s, 64 * KB, 64 * KB, MREMAP_MAYMOVE | MREMAP_FIXED, d);
    CHECK(t == d, "fixed: shared memfd mapping moved over the held range (%p -> %p)", (void *)s, (void *)t);
    if (t != d) { munmap(d, 64 * KB); return; }
    long got = 0;
    int fl = call_faults((fn_t)(uintptr_t)d, &got);
    void *where = t_addr;
    CHECK(fl && where == (void *)d && view[2] == SVC0,
          "fixed: a fetch there faults (si_addr %p, want %p%s), the file still holds its svc (%08x, want %08x)",
          where, (void *)d, fl ? "" : ": ran -- flipped and scanned", view[2], SVC0);
    int sf = store_faults(d + 8 * KB, 0xabcd);
    CHECK(!sf && view[2 * KB] == 0xabcd, "fixed: a store there lands in the file (%#x)", view[2 * KB]);
    munmap(d, 64 * KB);
    munmap((void *)view, 64 * KB);
    close(fd);
}

static void alias(void)
{
    char *d = held_run();
    char *src = map(64 * KB, PROT_READ | PROT_WRITE);
    if (!d || !src) { CHECK(0, "alias: setup"); return; }
    emit_ppid(src);
    long rc = syscall(0x4C580032, src, 64 * KB, d);     // lxrt_alias: d becomes a shared view of src
    CHECK(rc == 0, "alias: private memory aliased over the held range (rc %ld)", rc);
    if (rc != 0) { munmap(d, 64 * KB); munmap(src, 64 * KB); return; }
    long got = 0;
    int fl = call_faults((fn_t)(uintptr_t)d, &got);
    void *where = t_addr;
    uint32_t w = ((volatile uint32_t *)src)[2];
    CHECK(fl && where == (void *)d && w == SVC0,
          "alias: a fetch there faults (si_addr %p, want %p%s), the source still holds its svc (%08x)",
          where, (void *)d, fl ? "" : ": ran -- flipped and scanned", w);
    int sf = store_faults(d + 8 * KB, 0x5151);
    CHECK(!sf && ((volatile uint32_t *)src)[2 * KB] == 0x5151, "alias: a store there lands in the source");
    munmap(d, 64 * KB);
    munmap(src, 64 * KB);
}

// ------------------------------------------------------------ race

#define RACE_PAGES 64                   // 1 MiB
#define RACE_THREADS 3
static char *g_race;
static atomic_int g_race_stop;
static atomic_long g_race_calls, g_race_wrong, g_race_faults;

// Page i holds `mov w0, #i; ret` at its start.
static void *race_caller(void *arg)
{
    volatile unsigned i = (unsigned)(uintptr_t)arg * 21;
    t_armed = 1;
    if (sigsetjmp(t_jb, 1) != 0)
        atomic_fetch_add(&g_race_faults, 1);    // and carry on
    while (!atomic_load(&g_race_stop)) {
        i = (i + 1) % RACE_PAGES;
        if (((fn_t)(uintptr_t)(g_race + (uint64_t)i * PG))() != (long)i)
            atomic_fetch_add(&g_race_wrong, 1);
        atomic_fetch_add(&g_race_calls, 1);
    }
    t_armed = 0;
    return NULL;
}

static void race(void)
{
    g_race = held(RACE_PAGES * PG);
    if (!g_race) { CHECK(0, "race: RWX commit"); return; }
    for (unsigned i = 0; i < RACE_PAGES; i++)
        emit_const(g_race + (uint64_t)i * PG, i);
    pthread_t th[RACE_THREADS];
    for (int t = 0; t < RACE_THREADS; t++)
        pthread_create(&th[t], NULL, race_caller, (void *)(uintptr_t)t);
    while (atomic_load(&g_race_calls) < 10000)
        ;
    long svc_bad = 0, perr = 0;
    for (int round = 0; round < 200; round++) {
        unsigned k = (unsigned)(round * 7) % RACE_PAGES;
        emit_const(g_race + (uint64_t)k * PG, k);           // a page the callers run, written again
        fn_t g = emit_ppid(g_race + (uint64_t)((round * 13) % RACE_PAGES) * PG + 8 * KB);
        if (mprotect(g_race, RACE_PAGES * PG, RX) != 0) perr++;
        if (g() != want) svc_bad++;
        if (mprotect(g_race, RACE_PAGES * PG, RWX) != 0) perr++;
    }
    atomic_store(&g_race_stop, 1);
    for (int t = 0; t < RACE_THREADS; t++)
        pthread_join(th[t], NULL);
    CHECK(atomic_load(&g_race_faults) == 0 && atomic_load(&g_race_wrong) == 0 && svc_bad == 0 && perr == 0,
          "race: 200 x RWX -> write -> R-X over 1 MiB under %d callers: %ld faults, %ld wrong of %ld calls, "
          "%ld svc wrong, %ld mprotect errors", RACE_THREADS, (long)atomic_load(&g_race_faults),
          (long)atomic_load(&g_race_wrong), (long)atomic_load(&g_race_calls), svc_bad, perr);
    munmap(g_race, RACE_PAGES * PG);
}

// The race in six processes at once. With the CPU shared, the thread that
// changes the protection gets preempted inside lxrt_wx_protect, and a
// caller's fault arrives in between. In mode 0 every R-X makes the range
// leave the table, so the next RWX starts from an empty table, and the fault
// handler does not wait for the lock of an empty table: with the host made
// read-write before the entry went in, such a fault reached the guest
// (runtime/wxsplit.c, lxrt_wx_protect). A caller preempted in its handler
// instead found the page gone, declined it while the next RWX had put it
// back, and the next R-X had taken it out again by the time the last
// handler looked (wxsplit.c, stale_fault_retry).
static void racestress(void)
{
    enum { PROCS = 6 };
    pid_t pid[PROCS];
    fflush(stdout);
    for (int k = 0; k < PROCS; k++) {
        pid[k] = fork();
        if (pid[k] == 0) {
            ok = bad = 0;
            want = getppid();           // a rewritten svc's answer here: this test's pid
            race();
            fflush(stdout);
            _exit(bad ? 1 : 0);
        }
    }
    int failed = 0, lost = 0;
    for (int k = 0; k < PROCS; k++) {
        int st = 0;
        if (pid[k] < 0 || waitpid(pid[k], &st, 0) != pid[k])
            lost++;
        else if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
            failed++;
    }
    CHECK(!failed && !lost, "racestress: the race in %d processes at once: %d failed, %d not run",
          PROCS, failed, lost);
}

// A lazy page (an R-X entry in modes 1 and 2) beside a shared memfd page.
static char *decline_setup(int *fd)
{
    char *r = map(2 * PG, PROT_NONE);
    *fd = memfd_create("wx_lazy_decline", MFD_CLOEXEC);
    if (!r || *fd < 0 || ftruncate(*fd, PG) != 0 ||
        mmap(r + PG, PG, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, *fd, 0) != r + PG ||
        mprotect(r, PG, RWX) != 0) {
        if (r)
            munmap(r, 2 * PG);
        if (*fd >= 0)
            close(*fd);
        return NULL;
    }
    emit_const(r, 7);
    if (mprotect(r, PG, RX) != 0) {
        munmap(r, 2 * PG);
        close(*fd);
        return NULL;
    }
    return r;
}

static void decline(void)
{
    // RWX over both pages: the table declines it (not all private anonymous
    // memory) and the ordinary path makes both read-write; R-X right after
    // over the lazy page has to make it refuse stores again (the table
    // found it "already so" and left it writable).
    int fd;
    char *r = decline_setup(&fd);
    if (!r) { CHECK(0, "decline: setup"); return; }
    bool rwx = mprotect(r, 2 * PG, RWX) == 0;
    bool rx = mprotect(r, PG, RX) == 0;
    char p[8];
    maps_prot(r, p);
    int sf = store_faults(r + 4 * KB, 3);
    long got = 0;
    int cf = call_faults((fn_t)(uintptr_t)r, &got);
    CHECK(rwx && rx && sf && !cf && got == 7,
          "decline: RWX over a lazy page and a shared memfd page (declined), R-X again (%s): a store "
          "faults (%d), the code runs (%ld)", p, sf, got);
    munmap(r, 2 * PG);
    close(fd);

    // The same RWX, then the page used as RWX: stores land before a call and
    // after it (the call made it read-execute, an R-X entry refused the next
    // store). The call runs where the table keeps the page (modes 1 and 2);
    // with lazy execute off the page left the table at its R-X and the
    // declined RWX gave it read-write only -- executing it faults there, as
    // it does for any RWX memory the split does not take (not checked).
    r = decline_setup(&fd);
    if (!r) { CHECK(0, "decline: setup"); return; }
    rwx = mprotect(r, 2 * PG, RWX) == 0;
    int s0 = store_faults(r + 4 * KB, 1);
    got = 0;
    cf = call_faults((fn_t)(uintptr_t)r, &got);
    int s1 = store_faults(r + 4 * KB, 2);
    CHECK(rwx && !s0 && !s1 && (!g_mode || (!cf && got == 7)),
          "decline: the same RWX, the page used: stores land (%d, after the call %d), the call %s (%ld)",
          !s0, !s1, cf ? "faults" : "runs", got);
    munmap(r, 2 * PG);
    close(fd);
}

// brk gives heap pages back and maps them again: whatever the W^X table held
// there goes with them (runtime/dispatch.c brk_forget). Nothing that may
// allocate (stdio, maps_prot) between moving the break and the checks:
// glibc's malloc grows its heap with the same break.
static void brkcase(void)
{
    char *cur = sbrk(0);
    if (cur == (void *)-1) { CHECK(0, "brk: sbrk(0)"); return; }
    uintptr_t a = ((uintptr_t)cur + PG - 1) & ~(uintptr_t)(PG - 1);
    char *p = (char *)a + PG;
    fn_t f = (fn_t)(uintptr_t)p;
    if (sbrk((intptr_t)(a + 4 * PG - (uintptr_t)cur)) == (void *)-1) { CHECK(0, "brk: grow"); return; }
    // A held RWX page in the heap, run; the heap shrinks below it and grows
    // back: fresh read-write memory, which a fetch must not run.
    emit_const(p, 5);
    bool ok1 = mprotect(p, PG, RWX) == 0;
    long a1 = f();
    bool moved1 = brk((void *)a) == 0 && sbrk((intptr_t)(4 * PG)) != (void *)-1;
    int s0 = store_faults(p + 64, 1);
    emit_const(p, 6);
    long a2 = 0;
    int cf = call_faults(f, &a2);
    void *where = t_addr;
    // R-X there (lazy in modes 1 and 2), run; shrink, grow back; R-X over
    // the fresh page has to refuse stores.
    bool ok2 = mprotect(p, PG, RWX) == 0;
    emit_const(p, 8);
    ok2 = ok2 && mprotect(p, PG, RX) == 0;
    long a3 = f();
    bool moved2 = brk((void *)a) == 0 && sbrk((intptr_t)(4 * PG)) != (void *)-1;
    int s1 = store_faults(p + 64, 1);
    emit_const(p, 9);
    bool ok3 = mprotect(p, PG, RX) == 0;
    int s2 = store_faults(p + 128, 2);
    long a4 = 0;
    int cf2 = call_faults(f, &a4);
    CHECK(ok1 && a1 == 5 && moved1 && !s0 && cf && where == (void *)p,
          "brk: a held RWX heap page run (%ld), the heap shrunk below it and grown back: a store lands "
          "(%d), a fetch faults (si_addr %p, want %p%s)", a1, !s0, where, (void *)p,
          cf ? "" : ": ran -- a W^X entry outlived the shrink");
    CHECK(ok2 && a3 == 8 && moved2 && !s1 && ok3 && s2 && !cf2 && a4 == 9,
          "brk: an R-X heap page run (%ld), shrunk away and grown back: a store lands (%d); R-X over it "
          "refuses a store (%d), runs (%ld)", a3, !s1, s2, a4);
}

// lxrt_jit_wx(3, addr, len): dispatch.c's jit_adopt makes an RWX range of
// the split MAP_JIT (FEX's ARM64EC code buffer). Never a lazily executable
// one: the guest made it read-execute, and a writable MAP_JIT region would
// take the stores it must see fault. jit_adopt asks again under the page
// lock; the race that closes (an R-X from another thread between its check
// and its munmap) has no outcome a test can tell from a valid order.
static long jit_wx(long enable, void *addr, size_t len)
{
    return syscall(0x4C580020, enable, addr, len);
}

static void adoptjit(void)
{
    char *r = held(64 * KB);
    if (!r) { CHECK(0, "adoptjit: RWX commit"); return; }
    emit_const(r, 1);
    bool rx = mprotect(r, 64 * KB, RX) == 0;
    errno = 0;
    long rc = jit_wx(3, r, 64 * KB);
    int e = errno;
    int sf = store_faults(r + 8 * KB, 1);
    long got = ((fn_t)(uintptr_t)r)();
    CHECK(rx && rc == -1 && e == EINVAL && sf && got == 1,
          "adoptjit: an R-X range refused (rc %ld, errno %d, want EINVAL), still refuses a store (%d), runs (%ld)",
          rc, e, sf, got);
    munmap(r, 64 * KB);
    r = held(64 * KB);
    if (!r) { CHECK(0, "adoptjit: RWX commit"); return; }
    errno = 0;
    rc = jit_wx(3, r, 64 * KB);
    e = errno;
    CHECK(rc == 0, "adoptjit: an RWX range of the split taken as MAP_JIT (rc %ld, errno %d)", rc, e);
    munmap(r, 64 * KB);
}

// mremap of a range the table holds whole, its host pages in different
// states. Linux moves the one VMA; mremap.c moves one host protection and
// refused these (EFAULT) until wxsplit.c gave the range one
// (lxrt_wx_unify_prot).
static void fetched(void)
{
    // Lazy: page 0 run (read-execute), pages 1-3 not (read-only); the
    // PROT_NONE half above leaves no room in place.
    char *m = map(8 * PG, PROT_NONE);
    if (!m || mprotect(m, 4 * PG, RWX) != 0) { CHECK(0, "fetched: RWX commit"); return; }
    emit_const(m, 3);
    emit_const(m + PG, 4);
    if (g_mode)
        emit_ppid(m + 2 * PG);          // scanned after the move, where it runs (see "mremap")
    bool rx = mprotect(m, 4 * PG, RX) == 0;
    long a = ((fn_t)(uintptr_t)m)();
    char *n = mremap(m, 4 * PG, 8 * PG, MREMAP_MAYMOVE);
    if (n == MAP_FAILED) {
        CHECK(0, "fetched: R-X range with one page run, mremap 64 -> 128 KiB: %s", strerror(errno));
        munmap(m, 8 * PG);
    } else {
        long b = ((fn_t)(uintptr_t)n)(), c = ((fn_t)(uintptr_t)(n + PG))();
        long d = g_mode ? ((fn_t)(uintptr_t)(n + 2 * PG))() : want;
        int f1 = store_faults(n + 8 * KB, 1), f2 = store_faults(n + 6 * PG, 1);
        CHECK(rx && a == 3 && n != m && b == 3 && c == 4 && d == want && f1 && f2,
              "fetched: R-X range with one page run, moved %p -> %p and grown: runs (%ld, %ld), svc %ld "
              "(want getppid %ld%s), stores fault (%d, grown tail %d)", (void *)m, (void *)n, b, c, d, want,
              live(d), f1, f2);
        munmap(n, 8 * PG);
        if (n != m)
            munmap(m + 4 * PG, 4 * PG);
    }

    // RWX: page 0 run (read-execute), page 1 written (read-write).
    m = map(8 * PG, PROT_NONE);
    if (!m || mprotect(m, 4 * PG, RWX) != 0) { CHECK(0, "fetched: RWX commit"); return; }
    long e0 = emit_const(m, 5)();
    emit_const(m + PG, 6);
    n = mremap(m, 4 * PG, 8 * PG, MREMAP_MAYMOVE);
    if (n == MAP_FAILED) {
        CHECK(0, "fetched: RWX range with one page run, one written, mremap 64 -> 128 KiB: %s", strerror(errno));
        munmap(m, 8 * PG);
        return;
    }
    long g0 = ((fn_t)(uintptr_t)n)(), g1 = ((fn_t)(uintptr_t)(n + PG))();
    int s0 = store_faults(n + 8 * KB, 1), s1 = store_faults(n + PG + 8 * KB, 1), s2 = store_faults(n + 6 * PG, 1);
    CHECK(e0 == 5 && n != m && g0 == 5 && g1 == 6 && !s0 && !s1 && !s2,
          "fetched: RWX range with one page run and one written, moved %p -> %p and grown: runs (%ld, %ld), "
          "stores land (%d, %d, grown tail %d)", (void *)m, (void *)n, g0, g1, !s0, !s1, !s2);
    munmap(n, 8 * PG);
    if (n != m)
        munmap(m + 4 * PG, 4 * PG);
}

// A 4 KiB munmap inside a held host page leaves the freed slot's bytes in
// the page (runtime/dispatch.c do_munmap); a fetch from the rest makes all
// 16 KiB executable, so the slot is scanned too: its svc comes back
// rewritten, never live (wxsplit.c, lxrt_wx_flip_locked). RWX alone, then
// lazy R-X (eager with lazy execute off: scanned before the munmap).
static void freed(void)
{
    for (int lazy = 0; lazy < 2; lazy++) {
        char *r = held(4 * PG);
        if (!r) { CHECK(0, "freed: RWX commit"); return; }
        fn_t f = emit_const(r, 6);
        fn_t g = emit_ppid(r + 12 * KB);
        bool rx = !lazy || mprotect(r, 4 * PG, RX) == 0;
        bool um = munmap(r + 12 * KB, 4 * KB) == 0;
        long a = f();
        long got = 0;
        int cf = call_faults(g, &got);
        CHECK(rx && um && a == 6 && (cf || got == want),
              "freed: %s, 4 KiB at +12 KiB unmapped, the page run (%ld): the freed slot %s (%ld, want "
              "getppid %ld or a fault%s)", lazy ? "R-X" : "RWX", a, cf ? "faults" : "runs", got, want, live(got));
        munmap(r, 4 * PG);
    }
}

// R-X over code that ran since its RWX commit: the page read-execute
// (scanned) and not written since stays so; only the written page becomes
// read-only (wxsplit.c, lazy_host_locked). Read-only over all of it made
// every RWX -> R-X cycle refault and rescan code nothing had changed.
static void keeprx(void)
{
    char *r = held(64 * KB);
    if (!r) { CHECK(0, "keeprx: RWX commit"); return; }
    fn_t f = emit_const(r, 8);
    long a = f();
    fn_t g = emit_const(r + 16 * KB, 9);
    bool rx = mprotect(r, 64 * KB, RX) == 0;
    char p0[8], p1[8];
    const char *exp1 = g_mode ? "r--p" : "r-xp";
    maps_prot(r, p0);
    maps_prot(r + 16 * KB, p1);
    long b = f(), c = g();
    int sf = store_faults(r + 8 * KB, 1);
    CHECK(rx && a == 8 && b == 8 && c == 9 && sf && !strcmp(p0, "r-xp") && !strcmp(p1, exp1),
          "keeprx: R-X over a page run since its RWX commit and one written: %s and %s (want r-xp, %s), "
          "both run (%ld, %ld), a store faults (%d)", p0, p1, exp1, b, c, sf);
    munmap(r, 64 * KB);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    g_mode = argc > 1 ? atoi(argv[1]) : 0;
    want = getppid();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    printf("LXRT_LAZY_EXEC=%d (as told); getpid %ld, getppid %ld\n", g_mode, (long)getpid(), want);
    static const struct { const char *name; void (*fn)(void); } cases[] = {
        { "basic", basic }, { "svc", svc }, { "again", again }, { "smc", smc }, { "rw", rw },
        { "adopt", adopt }, { "mremap", remap }, { "fixed", fixed }, { "alias", alias }, { "race", race },
        { "racestress", racestress }, { "decline", decline }, { "brk", brkcase }, { "adoptjit", adoptjit },
        { "fetched", fetched }, { "freed", freed }, { "keeprx", keeprx },
    };
    // argv[2...]: only the cases named (all by default).
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        bool run = argc <= 2;
        for (int a = 2; a < argc; a++)
            run |= !strcmp(argv[a], cases[i].name);
        if (run)
            cases[i].fn();
    }
    printf("== wx_lazy_exec: %s (%d ok, %d mal)\n", bad ? "FAIL" : "ok", ok, bad);
    return bad ? 1 : 0;
}
