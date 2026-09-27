// A freestanding i386 Linux program: raw int 0x80, no libc, ET_EXEC at the
// classic 0x08048000 when linked -no-pie -- exactly what Darwin's __PAGEZERO
// forbids and what FEX's guest base (benchmarks/stage7-guest-base.txt) is for.
// Exercises: write, mmap/munmap of anonymous memory, stores and loads through
// it, a stack array, brk, and exit with a computed status.
// Build (on the Linux guest):
//   clang --target=i386-linux-gnu -m32 -nostdlib -static -fno-pie -no-pie -O1 -o i386_probe i386_probe.c
//   clang --target=i386-linux-gnu -m32 -nostdlib -static -fPIE -pie -O1 -o i386_probe_pie i386_probe.c
typedef unsigned int u32;
static inline int sys3(int nr, u32 a, u32 b, u32 c) {
    int r; __asm__ volatile("int $0x80" : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c) : "memory"); return r;
}
static inline int sys6(int nr, u32 a, u32 b, u32 c, u32 d, u32 e, u32 f) {
    int r;
    __asm__ volatile("push %%ebp\n mov %7, %%ebp\n int $0x80\n pop %%ebp"
                     : "=a"(r) : "a"(nr), "b"(a), "c"(b), "d"(c), "S"(d), "D"(e), "m"(f) : "memory");
    return r;
}
static u32 slen(const char *s) { u32 n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { sys3(4 /*write*/, 1, (u32)s, slen(s)); }
static void puthex(u32 v) { char b[11]; b[0]='0'; b[1]='x'; for (int i = 0; i < 8; i++) { int d = (v >> (28 - 4 * i)) & 15; b[2 + i] = d < 10 ? '0' + d : 'a' + d - 10; } b[10] = '\n'; sys3(4, 1, (u32)b, 11); }
static int g_data = 0x11223344;          // .data, at an ET_EXEC address below 4 GiB
static int g_bss[64];                    // .bss
void _start(void) {
    int bad = 0;
    put("i386 probe: hello from int 0x80\n");
    int stack_arr[16];
    for (int i = 0; i < 16; i++) stack_arr[i] = i * 7;
    int s = 0; for (int i = 0; i < 16; i++) s += stack_arr[i];
    if (s != 7 * 120) { put("MAL stack array\n"); bad++; }
    if (g_data != 0x11223344) { put("MAL .data\n"); bad++; }
    for (int i = 0; i < 64; i++) g_bss[i] = i; if (g_bss[63] != 63) { put("MAL .bss\n"); bad++; }
    put("g_data at "); puthex((u32)&g_data);
    u32 m = (u32)sys6(192 /*mmap2*/, 0, 0x4000, 3 /*RW*/, 0x22 /*PRIVATE|ANON*/, (u32)-1, 0);
    put("mmap2 -> "); puthex(m);
    if (m > 0xfffff000u) { put("MAL mmap2 failed\n"); bad++; }
    else {
        volatile u32 *p = (volatile u32 *)m;
        for (int i = 0; i < 0x1000; i++) p[i] = 0xA0000000u + i;
        for (int i = 0; i < 0x1000; i++) if (p[i] != 0xA0000000u + i) { put("MAL mmap contents\n"); bad++; break; }
        if (sys3(91 /*munmap*/, m, 0x4000, 0) != 0) { put("MAL munmap\n"); bad++; }
    }
    u32 brk0 = (u32)sys3(45 /*brk*/, 0, 0, 0); u32 brk1 = (u32)sys3(45, brk0 + 0x10000, 0, 0);
    put("brk "); puthex(brk0); put("  -> "); puthex(brk1);
    if (brk1 != brk0 + 0x10000) { put("MAL brk\n"); bad++; }
    else { volatile u32 *h = (volatile u32 *)brk0; h[0] = 0xdeadbeef; if (h[0] != 0xdeadbeef) { put("MAL heap store\n"); bad++; } }
    // a mapping at a fixed low address, MAP_FIXED_NOREPLACE
    u32 f = (u32)sys6(192, 0x30000000u, 0x2000, 3, 0x22 | 0x100000, (u32)-1, 0);
    put("mmap2 FIXED_NOREPLACE @0x30000000 -> "); puthex(f);
    if (f != 0x30000000u) { put("MAL fixed mapping\n"); bad++; } else *(volatile u32 *)f = 5;
    put(bad ? "== i386 probe: MAL\n" : "== i386 probe: 8 ok, 0 mal\n");
    sys3(252 /*exit_group, not 1=exit: FEX passes it straight through as a raw thread exit, which leaves FEX's own internal threads running (measured); real glibc binaries always call exit_group at process end*/, bad ? 1 : 0, 0, 0);
    for (;;) ;
}
