// lock cmpxchg8b on an address that is 4- but not 8-aligned (crosses 16 bytes)
static long sys3(long n, long a, long b, long c) { long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory"); return r; }
static void put(const char *s) { long n = 0; while (s[n]) n++; sys3(4, 1, (long)s, n); }
static unsigned char buf[64] __attribute__((aligned(16)));
void _start(void) {
    volatile unsigned long long *p = (volatile unsigned long long *)(buf + 12); /* 12..19 crosses 16 */
    *p = 0x1111111122222222ULL;
    unsigned lo = 0x22222222, hi = 0x11111111, nlo = 0x44444444, nhi = 0x33333333;
    unsigned char ok;
    __asm__ volatile("lock cmpxchg8b %1; sete %0" : "=q"(ok), "+m"(*p), "+a"(lo), "+d"(hi) : "b"(nlo), "c"(nhi) : "memory");
    put(ok && *p == 0x3333333344444444ULL ? "== cx8 unaligned: ok\n" : "== cx8 unaligned: MAL\n");
    sys3(252, 0, 0, 0);
    for (;;) ;
}
