// Read-write-execute code pages the way V8 uses them, with no help from the
// guest: no private syscall, no MAP_JIT, only mmap/mprotect/madvise/munmap.
//
// A 256 MiB PROT_NONE reservation; chunks of it committed read-write-execute
// (and RW then RX, and RX then RWX again); code written into them and called
// at once; rewritten in place and called again; called from a second thread
// while this one writes; run by several threads at once on its first fetch;
// a live `svc #0` (getpid) and an `mrs tpidr_el0` inside generated code, which
// only a scan before execution makes return the right values on Darwin; a
// chunk unmapped and committed again; madvise(DONTNEED); PROT_NONE inside a
// committed range; 5000 separate RWX commits; a 4 KiB commit inside a 16 KiB
// host page; and V8's own pattern (the whole range RWX at once, then
// DONTNEED, then an RW -> RX sub-range for the builtins).
//
// The same binary passes on Linux aarch64 with 4 KiB pages, for the boring
// reason: the kernel gives RWX pages as asked.
#define _GNU_SOURCE
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define KB 1024ul
#define MB (1024ul * 1024ul)

static int ok, bad;
#define CHECK(c, ...) do { if (c) { ok++; printf("  OK   " __VA_ARGS__); } \
                           else { bad++; printf("  MAL  " __VA_ARGS__); } putchar('\n'); fflush(stdout); } while (0)

static const uint32_t RET = 0xd65f03c0;
static uint32_t movz_w0(unsigned v) { return 0x52800000u | ((v & 0xffffu) << 5); }

typedef long (*fn_t)(void);

// Write a two-word function (mov w0,#v; ret) at p and make it runnable.
static fn_t emit_const(uint32_t *p, unsigned v)
{
    p[0] = movz_w0(v);
    p[1] = RET;
    __builtin___clear_cache((char *)p, (char *)(p + 2));
    return (fn_t)(uintptr_t)p;
}

// mov x8,#172 (getpid); svc #0; ret
static fn_t emit_getpid(uint32_t *p)
{
    p[0] = 0xd2801588;
    p[1] = 0xd4000001;
    p[2] = RET;
    __builtin___clear_cache((char *)p, (char *)(p + 3));
    return (fn_t)(uintptr_t)p;
}

// mrs x0, tpidr_el0; ret
static fn_t emit_tp(uint32_t *p)
{
    p[0] = 0xd53bd040;
    p[1] = RET;
    __builtin___clear_cache((char *)p, (char *)(p + 2));
    return (fn_t)(uintptr_t)p;
}

// Does touching p (read or write) fault?
static sigjmp_buf g_jb;
static volatile sig_atomic_t g_faulted;
static void on_fault(int sig)
{
    (void)sig;
    g_faulted = 1;
    siglongjmp(g_jb, 1);
}
static int faults(volatile uint32_t *p, int write)
{
    g_faulted = 0;
    if (sigsetjmp(g_jb, 1) == 0) {
        if (write) *p = 0x12345678;
        else (void)*p;
    }
    return g_faulted;
}

// ------------------------------------------------------------ threads

struct spinner {
    fn_t fn;
    long want;
    atomic_int stop;
    atomic_long calls, wrong;
};
static void *spin(void *arg)
{
    struct spinner *s = arg;
    while (!atomic_load(&s->stop)) {
        if (s->fn() != s->want)
            atomic_fetch_add(&s->wrong, 1);
        atomic_fetch_add(&s->calls, 1);
    }
    return NULL;
}

struct racer {
    fn_t *fnp;
    atomic_int *go;
    long got;
};
static void *race(void *arg)
{
    struct racer *r = arg;
    while (!atomic_load(r->go))
        ;
    r->got = (*r->fnp)();
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fault;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    const size_t RES = 256 * MB;
    char *res = mmap(NULL, RES, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(res != MAP_FAILED, "reserve 256 MiB PROT_NONE at %p", (void *)res);
    if (res == MAP_FAILED) return 1;

    // 1. Commit RWX, write, call.
    uint32_t *a = (uint32_t *)(res + 0);
    CHECK(mprotect(a, 64 * KB, PROT_READ | PROT_WRITE | PROT_EXEC) == 0, "mprotect 64 KiB RWX");
    fn_t f = emit_const(a, 41);
    CHECK(f() == 41, "generated code runs: %ld", f());

    // 2. Rewrite in place, call again: the old bytes must not run.
    f = emit_const(a, 42);
    long r = f();
    CHECK(r == 42, "rewritten in place and called again: %ld (want 42)", r);
    for (int i = 0; i < 100; i++) {
        emit_const(a, 1000 + i);
        if (f() != 1000 + i) { r = -1; break; }
    }
    CHECK(r != -1, "100 write/call rounds on the same instructions");

    // 3. svc in generated code: getpid() through the runtime, not Darwin.
    uint32_t *sc = a + 1024;
    fn_t g = emit_getpid(sc);
    long got = g(), want = getpid();
    uint32_t word = sc[1];
    CHECK(got == want, "svc #0 in generated code returns getpid()=%ld (want %ld); word now %08x",
          got, want, word);
    // Again after a rewrite of the same page (a second scan of it).
    emit_const(a + 8, 7);
    got = g();
    CHECK(got == want && ((fn_t)(uintptr_t)(a + 8))() == 7, "svc site still right after its page was written again");

    // 4. TLS read in generated code: the guest's thread pointer.
    fn_t t = emit_tp(a + 2048);
    long tp = t(), mytp = (long)(uintptr_t)__builtin_thread_pointer();
    CHECK(tp == mytp, "mrs tpidr_el0 in generated code = %#lx (want %#lx)", tp, mytp);

    // 5. RWX -> RX -> RWX.
    uint32_t *d = (uint32_t *)(res + 2 * MB);
    mprotect(d, 64 * KB, PROT_READ | PROT_WRITE | PROT_EXEC);
    f = emit_const(d, 5);
    CHECK(f() == 5, "RWX chunk runs");
    CHECK(mprotect(d, 64 * KB, PROT_READ | PROT_EXEC) == 0 && f() == 5, "RX: still runs");
    CHECK(faults(d + 100, 1), "RX: a store faults");
    CHECK(mprotect(d, 64 * KB, PROT_READ | PROT_WRITE | PROT_EXEC) == 0, "RWX again");
    f = emit_const(d, 6);
    CHECK(f() == 6 && !faults(d + 100, 1), "RWX again: writable and runs the new code (%ld)", f());

    // 6. munmap a chunk and commit it again.
    uint32_t *e = (uint32_t *)(res + 3 * MB);
    mprotect(e, 64 * KB, PROT_READ | PROT_WRITE | PROT_EXEC);
    emit_const(e, 9);
    CHECK(munmap(e, 64 * KB) == 0, "munmap a committed chunk");
    CHECK(mmap(e, 64 * KB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == e,
          "re-reserve it (MAP_FIXED PROT_NONE)");
    CHECK(faults(e, 0), "re-reserved chunk is inaccessible");
    CHECK(mprotect(e, 64 * KB, PROT_READ | PROT_WRITE | PROT_EXEC) == 0 && e[0] == 0,
          "commit again: RWX, zero-filled");
    f = emit_const(e, 10);
    CHECK(f() == 10, "code in the re-committed chunk runs");

    // 7. madvise(DONTNEED): reads back zero, can be written and run again.
    uint32_t *fz = (uint32_t *)(res + 4 * MB);
    mprotect(fz, 64 * KB, PROT_READ | PROT_WRITE | PROT_EXEC);
    f = emit_const(fz, 11);
    CHECK(f() == 11, "chunk runs before DONTNEED");
    CHECK(madvise(fz, 64 * KB, MADV_DONTNEED) == 0 && fz[0] == 0 && fz[1] == 0,
          "madvise(DONTNEED): zeros (%08x %08x)", fz[0], fz[1]);
    f = emit_const(fz, 12);
    CHECK(f() == 12, "written and run again after DONTNEED");

    // 8. PROT_NONE inside a committed RWX range, then commit again.
    uint32_t *gz = (uint32_t *)(res + 5 * MB);
    mprotect(gz, 128 * KB, PROT_READ | PROT_WRITE | PROT_EXEC);
    fn_t g1 = emit_const(gz, 13);
    fn_t g2 = emit_const(gz + 16 * KB, 14);          // 64 KiB further on
    CHECK(g1() == 13 && g2() == 14, "two functions 64 KiB apart");
    CHECK(mprotect(gz + 16 * KB, 64 * KB, PROT_NONE) == 0, "decommit the second half (PROT_NONE)");
    CHECK(faults(gz + 16 * KB, 0) && faults(gz + 16 * KB, 1), "decommitted half: read and write fault");
    CHECK(g1() == 13, "first half still runs");
    emit_const(gz + 2, 15);
    CHECK(((fn_t)(uintptr_t)(gz + 2))() == 15, "first half still writable");
    CHECK(mprotect(gz + 16 * KB, 64 * KB, PROT_READ | PROT_WRITE | PROT_EXEC) == 0, "recommit");
    g2 = emit_const(gz + 16 * KB, 16);
    CHECK(g2() == 16, "recommitted half runs new code");

    // 8b. A 4 KiB PROT_NONE guard inside a committed RWX range (V8 at 4 KiB
    // pages puts guards at that granularity): code in the rest of that 16 KiB
    // host page keeps running, can be rewritten, and a svc there is still
    // rewritten before it runs. (The guard itself is not checked: on 16 KiB
    // host pages it shares the union protection, runtime/subpage.c.)
    uint32_t *gd = (uint32_t *)(res + 5 * MB + 256 * KB);
    mprotect(gd, 64 * KB, PROT_READ | PROT_WRITE | PROT_EXEC);
    fn_t gd1 = emit_const(gd, 17);
    CHECK(gd1() == 17, "function in a 64 KiB RWX chunk");
    if (mprotect(gd + 1 * KB, 4 * KB, PROT_NONE) == 0) {    // bytes 4..8 KiB
        CHECK(gd1() == 17, "4 KiB guard in its host page: the function still runs");
        gd1 = emit_const(gd, 18);
        fn_t gd2 = emit_getpid(gd + 2 * KB);                  // bytes 8..12 KiB
        long gp = gd2();
        CHECK(gd1() == 18 && gp == want, "same host page: rewritten (%ld) and a svc returns getpid()=%ld", gd1(), gp);
    } else {
        printf("  skip 4 KiB guard (mprotect: page size %d)\n", getpagesize());
    }

    // 9. A second thread keeps calling one function while this one writes
    // and runs code elsewhere -- in other host pages, then in the SAME 16 KiB
    // page as the function the other thread is running.
    uint32_t *c = (uint32_t *)(res + 1 * MB);
    mprotect(c, 1 * MB, PROT_READ | PROT_WRITE | PROT_EXEC);
    struct spinner s = { .fn = emit_const(c, 77), .want = 77 };
    s.fn();
    pthread_t th;
    pthread_create(&th, NULL, spin, &s);
    while (atomic_load(&s.calls) < 1000)
        ;
    long badcalls = 0;
    for (int i = 0; i < 400; i++) {                  // other pages: 64 KiB .. 1 MiB
        uint32_t *p = c + (64 * KB + (uint64_t)i * 2 * KB) / 4;
        if (emit_const(p, 3000 + i)() != 3000 + i) badcalls++;
    }
    long calls_other = atomic_load(&s.calls);
    for (int i = 1; i < 200; i++) {                  // the spinner's own page
        uint32_t *p = c + (uint64_t)i * 16;
        if (emit_const(p, 5000 + i)() != 5000 + i) badcalls++;
    }
    atomic_store(&s.stop, 1);
    pthread_join(th, NULL);
    CHECK(badcalls == 0 && atomic_load(&s.wrong) == 0,
          "writer + concurrent caller: %ld bad writer calls, %ld/%ld bad spinner calls (%ld during the other-page phase)",
          badcalls, (long)atomic_load(&s.wrong), (long)atomic_load(&s.calls), calls_other);

    // 10. Eight threads fetch a freshly written page at the same moment.
    uint32_t *h = (uint32_t *)(res + 6 * MB);
    mprotect(h, 64 * KB, PROT_READ | PROT_WRITE | PROT_EXEC);
    long race_bad = 0;
    for (int round = 0; round < 20; round++) {
        atomic_int go = 0;
        fn_t fp = emit_const(h + round * 4, 600 + round);
        struct racer rr[8];
        pthread_t rt[8];
        for (int i = 0; i < 8; i++) {
            rr[i] = (struct racer){ .fnp = &fp, .go = &go };
            pthread_create(&rt[i], NULL, race, &rr[i]);
        }
        atomic_store(&go, 1);
        for (int i = 0; i < 8; i++) {
            pthread_join(rt[i], NULL);
            if (rr[i].got != 600 + round) race_bad++;
        }
    }
    CHECK(race_bad == 0, "8 threads x 20 rounds on a page's first fetch: %ld wrong", race_bad);

    // 11. A 4 KiB commit inside a 16 KiB host page (V8 at LXRT_GUEST_PAGE=4096
    // commits at that granularity), with a live svc in it.
    uint32_t *sub = (uint32_t *)(res + 7 * MB + 4 * KB);
    if (mprotect(sub, 4 * KB, PROT_READ | PROT_WRITE | PROT_EXEC) == 0) {
        g = emit_getpid(sub);
        got = g();
        CHECK(got == want, "4 KiB RWX commit: svc in it returns getpid()=%ld (want %ld)", got, want);
        // (Its PROT_NONE 4 KiB neighbours are NOT checked: on 16 KiB host
        // pages they share the union protection, runtime/subpage.c.)
        emit_const(sub + 16, 21);
        CHECK(((fn_t)(uintptr_t)(sub + 16))() == 21 && g() == want, "4 KiB commit: rewritten, runs again");
    } else {
        printf("  skip 4 KiB commit (mprotect: page size %d)\n", getpagesize());
    }

    // 12. 5000 separate RWX commits (every other 16 KiB), each run once, then
    // all decommitted.
    char *stress = res + 16 * MB;
    long sbad = 0, serr = 0;
    for (int i = 0; i < 5000; i++) {
        uint32_t *p = (uint32_t *)(stress + (uint64_t)i * 32 * KB);
        if (mprotect(p, 16 * KB, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) { serr++; continue; }
        if (emit_const(p, (unsigned)i)() != i) sbad++;
    }
    CHECK(serr == 0 && sbad == 0, "5000 separate RWX commits run: %ld mprotect errors, %ld wrong", serr, sbad);
    long again = 0;
    for (int i = 0; i < 5000; i += 7) {
        uint32_t *p = (uint32_t *)(stress + (uint64_t)i * 32 * KB);
        if (((fn_t)(uintptr_t)p)() != i) again++;
    }
    CHECK(again == 0, "a sample of them runs again: %ld wrong", again);
    long derr = 0;
    for (int i = 0; i < 5000; i++)
        if (mprotect(stress + (uint64_t)i * 32 * KB, 16 * KB, PROT_NONE) != 0) derr++;
    CHECK(derr == 0 && faults((uint32_t *)stress, 0), "all 5000 decommitted (%ld errors), inaccessible", derr);

    // 13. V8's own pattern (MEASURED in a webhelper renderer): the whole
    // reservation RWX at once, DONTNEED over all of it, an RW -> RX sub-range
    // for a copy of code, and code written anywhere else.
    char *v8 = mmap(NULL, 64 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    CHECK(v8 != MAP_FAILED && mprotect(v8, 64 * MB, PROT_READ | PROT_WRITE | PROT_EXEC) == 0 &&
          madvise(v8, 64 * MB, MADV_DONTNEED) == 0, "V8 pattern: 64 MiB RWX at once, then DONTNEED");
    uint32_t *blt = (uint32_t *)(v8 + 60 * MB);
    CHECK(mprotect(blt, 1 * MB, PROT_READ | PROT_WRITE) == 0, "builtins copy: RW");
    emit_getpid(blt);
    CHECK(mprotect(blt, 1 * MB, PROT_READ | PROT_EXEC) == 0 && ((fn_t)(uintptr_t)blt)() == want,
          "builtins copy: RX, its svc returns getpid()");
    long vbad = 0;
    for (int i = 0; i < 64; i++) {
        uint32_t *p = (uint32_t *)(v8 + (uint64_t)i * 900 * KB);
        if (emit_const(p, 700 + i)() != 700 + i) vbad++;
    }
    CHECK(vbad == 0, "64 functions across the RWX range: %ld wrong", vbad);
    CHECK(munmap(v8, 64 * MB) == 0, "V8 range unmapped");

    // 14. The same in a forked child, which is what a webhelper renderer is
    // (forked from the zygote, never exec'd): a fresh RWX range, and the
    // parent's already-flipped chunk written and run again.
    fflush(stdout);
    pid_t kid = fork();
    if (kid == 0) {
        uint32_t *k = (uint32_t *)(res + 8 * MB);
        if (mprotect(k, 64 * KB, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) _exit(10);
        if (emit_const(k, 31)() != 31) _exit(11);
        if (emit_getpid(k + 64)() != getpid()) _exit(12);
        if (emit_const(a, 32)() != 32) _exit(13);   // inherited, read-execute now
        _exit(0);
    }
    int st = -1;
    waitpid(kid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "forked child: RWX commit, svc and an inherited chunk (status %#x)", st);

    CHECK(munmap(res, RES) == 0, "reservation unmapped");
    printf("== jit_rwx: %s (%d ok, %d mal)\n", bad ? "FAIL" : "ok", ok, bad);
    return bad ? 1 : 0;
}
