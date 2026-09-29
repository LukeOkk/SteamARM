// A JIT code cache the way ART makes one (benchmarks/stage25-art-x86-fex.txt),
// as a freestanding static x86-64 program for FEX: one memfd mapped twice,
// an executable view and a writable view. Code is written through the
// writable view, run from the executable one, then rewritten in place and
// run again. FEX must notice every rewrite (its SMC tracking write-protects
// the writable mirror of translated code) or it runs the old translation.
//
//   x86_dualview [low] [toggle]
//        low     the executable view below 4 GiB (MAP_32BIT), as ART maps it
//                (default: anywhere)
//        toggle  the writable view is read-only except around each write,
//                which mprotect()s it read-write and back, as ART's
//                ScopedCodeCacheWrite does. That mprotect lifts the write
//                protection FEX's SMC tracking put there; FEX has to drop the
//                executable view's translations when it happens
//                (patches/fex-lxrt-smc-mprotect-mirrors.patch; without it,
//                stale results, MEASURED)
//
// Verdict line: "== x86_dualview: N ok, M mal" (M counts stale results).
typedef unsigned long u64;
typedef long i64;

static i64 sys6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
    i64 r;
    register i64 r10 __asm__("r10") = d;
    register i64 r8 __asm__("r8") = e;
    register i64 r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return r;
}
#define sys3(n, a, b, c) sys6(n, a, b, c, 0, 0, 0)
enum { NR_write = 1, NR_mmap = 9, NR_mprotect = 10, NR_ftruncate = 77, NR_memfd_create = 319,
       NR_exit_group = 231 };

static void out(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    sys3(NR_write, 1, (i64)s, (i64)n);
}
static void num(u64 v)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(b + i);
}

#define SIZE (64 * 1024)
#define SLOTS 64            // 1 KiB apart: 4 per 4 KiB page, 16 per 16 KiB host page

static void put(volatile unsigned char *w, u64 value)
{
    // mov eax, imm32 ; ret
    w[0] = 0xb8;
    w[1] = (unsigned char)value;
    w[2] = (unsigned char)(value >> 8);
    w[3] = (unsigned char)(value >> 16);
    w[4] = (unsigned char)(value >> 24);
    w[5] = 0xc3;
}

__attribute__((used)) static void main2(u64 argc, char **argv)
{
    int low = 0, toggle = 0;
    for (u64 a = 1; a < argc; a++) {
        if (argv[a][0] == 'l') low = 1;
        if (argv[a][0] == 't') toggle = 1;
    }
    i64 fd = sys3(NR_memfd_create, (i64)"dualview", 0, 0);
    if (fd < 0 || sys3(NR_ftruncate, fd, SIZE, 0) != 0) {
        out("== x86_dualview: 0 ok, 1 mal (memfd)\n");
        sys3(NR_exit_group, 2, 0, 0);
    }
    // PROT_READ|PROT_EXEC, MAP_SHARED (| MAP_32BIT); PROT_READ|PROT_WRITE, MAP_SHARED
    i64 x = sys6(NR_mmap, 0, SIZE, 5, 0x01 | (low ? 0x40 : 0), fd, 0);
    i64 w = sys6(NR_mmap, 0, SIZE, toggle ? 1 : 3, 0x01, fd, 0);
    if (x < 0 || w < 0) {
        out("== x86_dualview: 0 ok, 1 mal (mmap)\n");
        sys3(NR_exit_group, 2, 0, 0);
    }
    u64 oks = 0, mals = 0;
    // Round r writes value r*1000+slot into every slot, in an order that
    // changes each round, and runs each slot right after writing it and
    // again after all slots are written.
    for (u64 r = 1; r <= 12; r++) {
        for (u64 j = 0; j < SLOTS; j++) {
            u64 s = (r & 1) ? j : SLOTS - 1 - j;
            s = (s * 5 + r) % SLOTS;
            if (toggle) sys3(NR_mprotect, w, SIZE, 3);
            put((volatile unsigned char *)(w + (i64)s * 1024), r * 1000 + s);
            if (toggle) sys3(NR_mprotect, w, SIZE, 1);
            u64 (*f)(void) = (u64 (*)(void))(x + (i64)s * 1024);
            if ((f() & 0xffffffff) == r * 1000 + s) oks++; else mals++;
        }
        for (u64 s = 0; s < SLOTS; s++) {
            u64 (*f)(void) = (u64 (*)(void))(x + (i64)s * 1024);
            if ((f() & 0xffffffff) == r * 1000 + s) oks++; else mals++;
        }
    }
    out("   executable view ");
    out(low ? "below 4 GiB" : "anywhere");
    out(toggle ? ", writable view mprotect()ed read-write around each write" : "");
    out("\n== x86_dualview: ");
    num(oks);
    out(" ok, ");
    num(mals);
    out(" mal\n");
    sys3(NR_exit_group, mals ? 1 : 0, 0, 0);
}

// _start: argc at (%rsp), argv after it.
__asm__(".globl _start\n_start:\n"
        "  mov (%rsp), %rdi\n"
        "  lea 8(%rsp), %rsi\n"
        "  and $-16, %rsp\n"
        "  call main2\n"
        "  ud2\n");
