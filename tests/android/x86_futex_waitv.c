// futex_waitv (449) from an x86-64 program under FEX, the way Proton's
// x86-64 Wine reaches it (FEX passes it through to the runtime): the probe
// futex_waitv(NULL, 0) is EINVAL, a differing word EAGAIN, a timeout
// ETIMEDOUT, and a waiter in a forked child on a shared mapping is woken
// by the parent's store and FUTEX_WAKE. Freestanding: raw syscalls.
//   run-android-x86.sh /data/local/tmp/x86_futex_waitv
typedef unsigned long u64;
typedef unsigned int u32;
static long sys(long n, long a, long b, long c, long d, long e, long f)
{
    long r;
    register long r10 __asm__("r10") = d;
    register long r8 __asm__("r8") = e;
    register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory");
    return r;
}
static void out(const char *s) { long n = 0; while (s[n]) n++; sys(1, 1, (long)s, n, 0, 0, 0); }
struct waitv { u64 val, uaddr; u32 flags, reserved; };
struct ts { long sec, nsec; };

void _start(void)
{
    int fails = 0;
    long r = sys(449, 0, 0, 0, 0, 0, 0);
    if (r == -22) out("  OK   probe futex_waitv(NULL, 0): EINVAL\n"); else { out("  MAL  probe: not EINVAL (ENOSYS: fsync off)\n"); fails++; }
    u32 *shm = (u32 *)sys(9, 0, 4096, 3, 0x21 /* MAP_SHARED|MAP_ANONYMOUS */, -1, 0);
    shm[10] = 4; shm[600] = 0;
    struct waitv w[2];
    w[0].val = 5; w[0].uaddr = (u64)&shm[10]; w[0].flags = 2; w[0].reserved = 0;
    w[1].val = 0; w[1].uaddr = (u64)&shm[600]; w[1].flags = 2; w[1].reserved = 0;
    if (sys(449, (long)w, 2, 0, 0, 1, 0) == -11) out("  OK   a differing word: EAGAIN\n"); else { out("  MAL  EAGAIN\n"); fails++; }
    w[0].val = 4;
    struct ts t;
    sys(228, 1, (long)&t, 0, 0, 0, 0);       // clock_gettime(CLOCK_MONOTONIC)
    t.nsec += 30000000; if (t.nsec >= 1000000000) { t.sec++; t.nsec -= 1000000000; }
    if (sys(449, (long)w, 2, 0, (long)&t, 1, 0) == -110) out("  OK   absolute timeout: ETIMEDOUT\n"); else { out("  MAL  timeout\n"); fails++; }
    long kid = sys(57, 0, 0, 0, 0, 0, 0);    // fork
    if (kid == 0) {
        sys(228, 1, (long)&t, 0, 0, 0, 0);
        t.sec += 5;
        long cr = sys(449, (long)w, 2, 0, (long)&t, 1, 0);
        sys(60, cr == 1 ? 0 : 3, 0, 0, 0, 0, 0);
    }
    struct ts nap = { 0, 80000000 };
    sys(35, (long)&nap, 0, 0, 0, 0, 0);      // nanosleep
    __atomic_store_n(&shm[600], 1, __ATOMIC_SEQ_CST);
    sys(202, (long)&shm[600], 1 /* FUTEX_WAKE */, 1, 0, 0, 0);
    int st = -1;
    sys(61, kid, (long)&st, 0, 0, 0, 0);     // wait4
    if (st == 0) out("  OK   a waiter in a forked child, woken by the parent: returns 1\n"); else { out("  MAL  cross-process wake\n"); fails++; }
    out(fails ? "== x86_futex_waitv: FAIL\n" : "== x86_futex_waitv: PASS\n");
    sys(231, fails != 0, 0, 0, 0, 0, 0);
    for (;;) {}
}
