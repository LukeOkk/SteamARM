// BT/BTS with a memory operand (immediate and register bit index, including a
// negative index that reaches below the base) and XLAT, under the guest base.
static long sys3(long n, long a, long b, long c) { long r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(n), "b"(a), "c"(b), "d"(c) : "memory"); return r; }
static void put(const char *s) { long n = 0; while (s[n]) n++; sys3(4, 1, (long)s, n); }
static unsigned char mem[64] __attribute__((aligned(16)));
static const unsigned char table[256] = { [7] = 0x5a, [200] = 0xa5 };
void _start(void) {
    int bad = 0; unsigned char c;
    mem[16 + 3] = 0x04;                                         // bit 26 of the dword at mem+16
    __asm__ volatile("btl $26, %1; setc %0" : "=q"(c) : "m"(*(unsigned *)(mem + 16)) : "cc");
    if (!c) { put("MAL bt imm\n"); bad++; }
    __asm__ volatile("btsl $5, %0" : "+m"(*(unsigned *)(mem + 32)) : : "cc");
    if (mem[32] != 0x20) { put("MAL bts imm\n"); bad++; }
    int idx = -8;                                               // bit -8 = byte -1, bit 0
    mem[15] = 0x01;
    __asm__ volatile("btl %2, %1; setc %0" : "=q"(c) : "m"(*(unsigned *)(mem + 16)), "r"(idx) : "cc");
    if (!c) { put("MAL bt negative reg index\n"); bad++; }
    unsigned char r1, r2;
    __asm__ volatile("xlatb" : "=a"(r1) : "a"(7), "b"(table));
    __asm__ volatile("xlatb" : "=a"(r2) : "a"(200), "b"(table));
    if (r1 != 0x5a || r2 != 0xa5) { put("MAL xlat\n"); bad++; }
    put(bad ? "== bt/xlat: MAL\n" : "== bt/xlat: ok\n");
    sys3(252, bad, 0, 0); for (;;) ;
}
