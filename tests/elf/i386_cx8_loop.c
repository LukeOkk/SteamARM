// Atomic 64-bit increment with a lock cmpxchg8b retry loop on an address that
// crosses a 16-byte boundary (FEX's CASP needs 8-byte alignment, so every
// iteration goes through its SIGBUS unaligned-atomic path).
static long sys3(long n, long a, long b, long c) { long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory"); return r; }
static void put(const char *s) { long n = 0; while (s[n]) n++; sys3(4, 1, (long)s, n); }
static unsigned char buf[64] __attribute__((aligned(16)));
static void add1(volatile unsigned long long *p) {
    unsigned lo = (unsigned)*p, hi = (unsigned)(*p >> 32);
    for (int tries = 0; tries < 100000; tries++) {
        unsigned nlo = lo + 1, nhi = hi + (nlo == 0);
        unsigned char ok;
        __asm__ volatile("lock cmpxchg8b %1; sete %0" : "=q"(ok), "+m"(*p), "+a"(lo), "+d"(hi) : "b"(nlo), "c"(nhi) : "memory");
        if (ok) return;
    }
    put("== cx8 loop: MAL (never succeeded)\n"); sys3(252, 2, 0, 0);
}
void _start(void) {
    volatile unsigned long long *p = (volatile unsigned long long *)(buf + 12);
    *p = 0xfffffff0ULL;                       // carries into the high word
    for (int i = 0; i < 1000; i++) add1(p);
    put(*p == 0xfffffff0ULL + 1000 ? "== cx8 loop: ok\n" : "== cx8 loop: MAL (wrong value)\n");
    sys3(252, 0, 0, 0); for (;;) ;
}
