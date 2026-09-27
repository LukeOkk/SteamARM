// 4 KiB guest pages inside 16 KiB host pages, under the i386 guest base:
// map 8 pages, fill them, then unmap / mprotect / remap single pages and
// check that every neighbour keeps its content and its access.
typedef unsigned u32;
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
    long r; __asm__ volatile("push %%ebp\n mov %7, %%ebp\n int $0x80\n pop %%ebp"
        : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "g"(f) : "memory"); return r; }
static long sys3(long n, long a, long b, long c) { long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory"); return r; }
static void put(const char *s) { long n = 0; while (s[n]) n++; sys3(4, 1, (long)s, n); }
static void hex(u32 v) { char b[11] = "0x00000000\n"; for (int i = 0; i < 8; i++) b[9 - i] = "0123456789abcdef"[(v >> (i * 4)) & 15]; sys3(4, 1, (long)b, 11); }
void _start(void) {
    int bad = 0;
    u32 base = (u32)sys6(192, 0, 8 * 4096, 3, 0x22, (u32)-1, 0);          // mmap2 8 pages RW
    for (int p = 0; p < 8; p++) *(volatile u32 *)(base + p * 4096) = 0x1000 + p;
    sys3(91, base + 2 * 4096, 4096, 0);                                    // munmap page 2
    for (int p = 0; p < 8; p++) if (p != 2 && *(volatile u32 *)(base + p * 4096) != 0x1000u + p) { put("MAL after munmap, page "); hex(p); bad++; }
    for (int p = 0; p < 8; p++) if (p != 2) *(volatile u32 *)(base + p * 4096) = 0x2000 + p;   // writable still?
    sys3(125, base + 5 * 4096, 4096, 1);                                   // mprotect page 5 read-only
    for (int p = 0; p < 8; p++) if (p != 2 && p != 5) *(volatile u32 *)(base + p * 4096) += 1; // neighbours writable?
    if (*(volatile u32 *)(base + 5 * 4096) != 0x2005) { put("MAL ro page content\n"); bad++; }
    u32 again = (u32)sys6(192, base + 2 * 4096, 4096, 3, 0x32, (u32)-1, 0); // MAP_FIXED page 2 back
    if (again != base + 2 * 4096 || *(volatile u32 *)again != 0) { put("MAL remap page 2\n"); bad++; }
    *(volatile u32 *)again = 7;
    for (int p = 0; p < 8; p++) if (p != 2 && p != 5 && *(volatile u32 *)(base + p * 4096) != 0x2001u + p) { put("MAL final, page "); hex(p); bad++; }
    put(bad ? "== subpage: MAL\n" : "== subpage: ok\n");
    sys3(252, bad, 0, 0); for (;;) ;
}
