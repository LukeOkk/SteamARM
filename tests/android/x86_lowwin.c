// What x86-64 ART needs from FEX's 64-bit low window under the runtime
// (benchmarks/stage25-art-x86-fex.txt), as a freestanding static x86-64
// program: raw syscalls only, so it runs under FEX in any x86-64 root.
//
//   tests/android/run.sh builds it (clang --target=x86_64-linux-gnu) into the
//   x86_64 Android root and runs it through scripts/run-android-x86.sh.
//
// Checks (each prints "ok" or "MAL" and a reason; the last line is the
// verdict "== x86_lowwin: N ok, M mal"):
//   map32     mmap(MAP_32BIT) lands below 4 GiB, and the memory works: ART's
//             heap spaces are mapped this way (MemMap::MapInternal on x86-64)
//   zero      madvise(MADV_DONTNEED) on that memory reads back as zeros, as
//             Linux promises for private anonymous memory: ART relies on it
//             to clear heap regions (kMadviseZeroes)
//   hint      a plain hint below 4 GiB (no MAP_FIXED) is honoured when the
//             range is free: ART reserves its boot image at 0x70000000 plus
//             a random delta that way
//   stack     /proc/self/stat's startstack lies in a /proc/self/maps entry
//             that contains this thread's stack: bionic's
//             pthread_getattr_np() of the main thread finds its stack so
//   rlimit    getrlimit(RLIMIT_NOFILE) (x86-64 syscall 97) works, and a
//             "no limit" reads as Linux's RLIM_INFINITY (~0), not 2^63-1:
//             bionic's fdsan sizes a table from it
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

enum { NR_read = 0, NR_write = 1, NR_open = 2, NR_close = 3, NR_mmap = 9, NR_munmap = 11,
       NR_madvise = 28, NR_getrlimit = 97, NR_exit_group = 231 };
enum { PROT_RW = 3, MAP_PRIV_ANON = 0x22, MAP_32BIT = 0x40, MADV_DONTNEED = 4, RLIMIT_NOFILE = 7 };

static void out(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    sys3(NR_write, 1, (i64)s, (i64)n);
}
static void hex(u64 v)
{
    char b[19] = "0x";
    for (int i = 0; i < 16; i++) {
        int d = (int)((v >> (60 - 4 * i)) & 15);
        b[2 + i] = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    b[18] = 0;
    out(b);
}
static int oks, mals;
static void verdict(const char *name, int ok, const char *why)
{
    out(ok ? "  ok   " : "  MAL  ");
    out(name);
    out(": ");
    out(why);
    out("\n");
    if (ok) oks++; else mals++;
}

static char buf[65536];
static u64 slurp(const char *path)
{
    i64 fd = sys3(NR_open, (i64)path, 0, 0);
    if (fd < 0) return 0;
    u64 n = 0;
    for (;;) {
        i64 r = sys3(NR_read, fd, (i64)(buf + n), (i64)(sizeof buf - 1 - n));
        if (r <= 0) break;
        n += (u64)r;
        if (n >= sizeof buf - 1) break;
    }
    sys3(NR_close, fd, 0, 0);
    buf[n] = 0;
    return n;
}
static u64 parse_hex(const char **p)
{
    u64 v = 0;
    for (;;) {
        char c = **p;
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (d < 0) return v;
        v = v * 16 + (u64)d;
        (*p)++;
    }
}

void _start(void)
{
    // map32 + zero
    u64 len = 1 << 20;
    i64 m = sys6(NR_mmap, 0, (i64)len, PROT_RW, MAP_PRIV_ANON | MAP_32BIT, -1, 0);
    if (m < 0 || (u64)m >= (1ull << 32)) {
        verdict("map32", 0, "mmap(MAP_32BIT) did not land below 4 GiB");
        out("       got "); hex((u64)m); out("\n");
    } else {
        volatile unsigned char *p = (volatile unsigned char *)m;
        for (u64 i = 0; i < len; i += 4096) p[i] = 0xa5;
        int back = 1;
        for (u64 i = 0; i < len; i += 4096) if (p[i] != 0xa5) back = 0;
        verdict("map32", back, back ? "mmap(MAP_32BIT) below 4 GiB, readable and writable" : "written bytes did not read back");
        out("       at "); hex((u64)m); out("\n");
        i64 r = sys3(NR_madvise, m, (i64)len, MADV_DONTNEED);
        int zero = r == 0;
        for (u64 i = 0; zero && i < len; i += 4096) if (p[i] != 0) zero = 0;
        verdict("zero", zero, zero ? "madvise(MADV_DONTNEED) below 4 GiB reads back as zeros"
                                   : "madvise(MADV_DONTNEED) left the old bytes (or failed)");
        sys3(NR_munmap, m, (i64)len, 0);
    }

    // hint: a free spot below 4 GiB, asked for without MAP_FIXED
    u64 want = 0x71230000ull;
    i64 h = sys6(NR_mmap, (i64)want, 0x100000, PROT_RW, MAP_PRIV_ANON, -1, 0);
    verdict("hint", h == (i64)want, h == (i64)want ? "a free hint below 4 GiB is honoured"
                                                   : "a free hint below 4 GiB was not honoured");
    if (h != (i64)want) { out("       got "); hex((u64)h); out("\n"); }
    if (h > 0) sys3(NR_munmap, h, 0x100000, 0);

    // stack: field 28 of /proc/self/stat, then the maps entry holding it
    u64 startstack = 0;
    if (slurp("/proc/self/stat")) {
        const char *p = buf;
        const char *rp = p;
        for (const char *q = p; *q; q++) if (*q == ')') rp = q;   // after the comm
        int field = 2;
        for (const char *q = rp + 1; *q; q++) {
            if (*q == ' ') {
                field++;
                if (field == 28) {
                    u64 v = 0;
                    for (q++; *q >= '0' && *q <= '9'; q++) v = v * 10 + (u64)(*q - '0');
                    startstack = v;
                    break;
                }
            }
        }
    }
    u64 sp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(sp));
    int found = 0, holds_sp = 0;
    if (startstack && slurp("/proc/self/maps")) {
        const char *p = buf;
        while (*p) {
            u64 lo = parse_hex(&p);
            if (*p == '-') p++;
            u64 hi = parse_hex(&p);
            if (lo <= startstack && startstack <= hi) {
                found = 1;
                holds_sp = lo <= sp && sp < hi;
                break;
            }
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
    }
    verdict("stack", found && holds_sp,
            !startstack ? "/proc/self/stat startstack is 0"
            : !found ? "startstack is in no /proc/self/maps entry"
            : !holds_sp ? "startstack's maps entry is not this thread's stack"
            : "startstack is in the maps entry of this thread's stack");
    out("       startstack "); hex(startstack); out(", sp "); hex(sp); out("\n");

    // rlimit
    u64 rl[2] = { 1, 1 };
    i64 g = sys3(NR_getrlimit, RLIMIT_NOFILE, (i64)rl, 0);
    int rok = g == 0 && rl[1] != 0x7fffffffffffffffull && rl[0] <= rl[1];
    verdict("rlimit", rok, g != 0 ? "getrlimit(RLIMIT_NOFILE) failed"
                          : rl[1] == 0x7fffffffffffffffull ? "rlim_max is Darwin's RLIM_INFINITY (2^63-1)"
                          : "getrlimit(RLIMIT_NOFILE) works; no limit reads as ~0");
    out("       cur "); hex(rl[0]); out(" max "); hex(rl[1]); out("\n");

    out("== x86_lowwin: ");
    char n[4] = { (char)('0' + oks), 0 };
    out(n); out(" ok, ");
    n[0] = (char)('0' + mals);
    out(n); out(" mal\n");
    sys3(NR_exit_group, mals ? 1 : 0, 0, 0);
    for (;;) {}
}
