// A Linux syscall preserves the whole FP/SIMD register file: v0-v31 and
// FPSR (Linux arm64 entry code saves no user FP state it then changes; the
// registers a task sees after `svc` are the ones it had before). Compilers
// rely on it for inline syscalls: glibc's INTERNAL_SYSCALL and freestanding
// code declare only "memory" as clobbered, so a value may live in a vector
// register across the `svc`. lxrun's trampoline calls Darwin C code, which
// clobbers vector registers (libsystem's memcpy/memset), so it has to save
// them (runtime/trampoline.S). MEASURED before the fix: a raw Wayland client
// kept a msghdr template in q2 across its syscalls and sent iovlen 0
// (benchmarks/stage27-android-display.txt).
//
// Freestanding static-pie, raw syscalls. Checks:
//   vregs   v0-v31 hold 32 distinct 128-bit patterns before a syscall that
//           makes the runtime do real work (openat + read of /proc/self/maps,
//           then getpid, 2000 rounds) and still hold them after each
//   fpsr    FPSR's cumulative flags (IOC|DZC|OFC|UFC|IXC, 0x1f) set before
//           the syscall are still set after it, and clear ones stay clear
//   nzcv    the condition flags (all 16 combinations) survive a syscall
//   cost    ns per getpid round trip (printed, not judged)
// Last line: "== simd_syscall: N ok, M mal".
typedef unsigned long u64;
typedef long i64;

static i64 sys3(i64 n, i64 a, i64 b, i64 c)
{
    register i64 x8 __asm__("x8") = n;
    register i64 x0 __asm__("x0") = a;
    register i64 x1 __asm__("x1") = b;
    register i64 x2 __asm__("x2") = c;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
    return x0;
}
enum { NR_openat = 56, NR_close = 57, NR_read = 63, NR_write = 64, NR_exit_group = 94,
       NR_clock_gettime = 113, NR_getpid = 172 };

static void out(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    sys3(NR_write, 1, (i64)s, (i64)n);
}
static void outn(u64 v)
{
    char b[24];
    int i = 23;
    b[i] = 0;
    do { b[--i] = (char)('0' + v % 10); v /= 10; } while (v);
    out(b + i);
}
static int oks, mals;
static void verdict(const char *name, int ok, const char *why)
{
    out(ok ? "  ok   " : "  MAL  "); out(name); out(": "); out(why); out("\n");
    if (ok) oks++; else mals++;
}
static u64 now_ns(void)
{
    i64 ts[2];
    sys3(NR_clock_gettime, 1, (i64)ts, 0);
    return (u64)ts[0] * 1000000000ull + (u64)ts[1];
}

// Fill v0-v31 from pat (32 x 16 bytes), make the syscall(s), store them into
// got. All in one asm block so the compiler cannot move anything between.
static char g_buf[4096];
static void round_trip(const unsigned char *pat, unsigned char *got, int heavy)
{
    __asm__ volatile(
        "ldp q0, q1, [%[p], #0]\n ldp q2, q3, [%[p], #32]\n ldp q4, q5, [%[p], #64]\n ldp q6, q7, [%[p], #96]\n"
        "ldp q8, q9, [%[p], #128]\n ldp q10, q11, [%[p], #160]\n ldp q12, q13, [%[p], #192]\n ldp q14, q15, [%[p], #224]\n"
        "ldp q16, q17, [%[p], #256]\n ldp q18, q19, [%[p], #288]\n ldp q20, q21, [%[p], #320]\n ldp q22, q23, [%[p], #352]\n"
        "ldp q24, q25, [%[p], #384]\n ldp q26, q27, [%[p], #416]\n ldp q28, q29, [%[p], #448]\n ldp q30, q31, [%[p], #480]\n"
        "cbz %w[h], 1f\n"
        // openat(AT_FDCWD, "/proc/self/maps", O_RDONLY); read; close
        "mov x0, #-100\n mov x1, %[path]\n mov x2, #0\n mov x3, #0\n mov x8, #56\n svc #0\n"
        "mov x9, x0\n"
        "tbnz x0, #63, 1f\n"
        "mov x1, %[buf]\n mov x2, #4096\n mov x8, #63\n svc #0\n"
        "mov x0, x9\n mov x8, #57\n svc #0\n"
        "1:\n"
        "mov x8, #172\n svc #0\n"
        "stp q0, q1, [%[g], #0]\n stp q2, q3, [%[g], #32]\n stp q4, q5, [%[g], #64]\n stp q6, q7, [%[g], #96]\n"
        "stp q8, q9, [%[g], #128]\n stp q10, q11, [%[g], #160]\n stp q12, q13, [%[g], #192]\n stp q14, q15, [%[g], #224]\n"
        "stp q16, q17, [%[g], #256]\n stp q18, q19, [%[g], #288]\n stp q20, q21, [%[g], #320]\n stp q22, q23, [%[g], #352]\n"
        "stp q24, q25, [%[g], #384]\n stp q26, q27, [%[g], #416]\n stp q28, q29, [%[g], #448]\n stp q30, q31, [%[g], #480]\n"
        :
        : [p] "r"(pat), [g] "r"(got), [h] "r"(heavy), [path] "r"("/proc/self/maps"), [buf] "r"(g_buf)
        : "x0", "x1", "x2", "x3", "x8", "x9", "memory",
          "v0", "v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9", "v10", "v11", "v12", "v13", "v14", "v15",
          "v16", "v17", "v18", "v19", "v20", "v21", "v22", "v23", "v24", "v25", "v26", "v27", "v28", "v29", "v30", "v31");
}

static unsigned char g_pat[512], g_got[512];

void cstart(void)
{
    // vregs
    int bad_round = -1, bad_reg = -1;
    for (int round = 0; round < 2000 && bad_round < 0; round++) {
        for (int i = 0; i < 512; i++)
            g_pat[i] = (unsigned char)(i * 7 + round * 13 + (i >> 4) * 31 + 1);
        for (int i = 0; i < 512; i++) g_got[i] = 0;
        round_trip(g_pat, g_got, round & 1);
        for (int i = 0; i < 512; i++)
            if (g_got[i] != g_pat[i]) { bad_round = round; bad_reg = i / 16; break; }
    }
    if (bad_round < 0) {
        verdict("vregs", 1, "v0-v31 unchanged across 2000 syscall rounds (openat/read/close/getpid)");
    } else {
        verdict("vregs", 0, "a vector register changed across a syscall");
        out("       round "); outn((u64)bad_round); out(", v"); outn((u64)bad_reg); out("\n");
    }

    // fpsr
    u64 before = 0x1f, after = 0, clear_after = 1;
    __asm__ volatile("msr fpsr, %0" :: "r"(before));
    sys3(NR_getpid, 0, 0, 0);
    __asm__ volatile("mrs %0, fpsr" : "=r"(after));
    __asm__ volatile("msr fpsr, %0" :: "r"(0ul));
    sys3(NR_getpid, 0, 0, 0);
    __asm__ volatile("mrs %0, fpsr" : "=r"(clear_after));
    verdict("fpsr", (after & 0x1f) == 0x1f && (clear_after & 0x1f) == 0,
            (after & 0x1f) == 0x1f && (clear_after & 0x1f) == 0 ? "cumulative flags kept, clear ones stay clear"
                                                                 : "FPSR changed across a syscall");

    // nzcv: each of the four condition flags set (then clear) before the
    // syscall, read back after it. Compilers keep a comparison's result in
    // the flags across an inline `svc` (a counted loop: subs; svc; b.ne).
    int nzcv_bad = 0;
    for (u64 want = 0; want < 16; want++) {
        u64 got;
        __asm__ volatile("msr nzcv, %[w]\n mov x8, #172\n svc #0\n mrs %[g], nzcv"
                         : [g] "=r"(got) : [w] "r"(want << 28) : "x0", "x8", "memory", "cc");
        if ((got >> 28) != want) nzcv_bad++;
    }
    verdict("nzcv", !nzcv_bad, !nzcv_bad ? "all 16 flag combinations survive a syscall"
                                         : "the condition flags changed across a syscall");

    // cost: a counted loop in one asm block (the count in a register, not
    // the flags), so it runs exactly N calls whatever the flags do.
    u64 n = 200000;
    u64 t0 = now_ns();
    __asm__ volatile("1: mov x8, #172\n svc #0\n sub %[n], %[n], #1\n cbnz %[n], 1b"
                     : [n] "+r"(n) : : "x0", "x8", "memory", "cc");
    u64 ns = now_ns() - t0;
    out("       getpid: "); outn(ns / 200); out(" ns per 1000 calls ("); outn(ns / 200000); out(" ns each)\n");

    out("== simd_syscall: "); outn((u64)oks); out(" ok, "); outn((u64)mals); out(" mal\n");
    sys3(NR_exit_group, mals ? 1 : 0, 0, 0);
    for (;;) {}
}
__asm__(".globl _start\n_start:\n mov x29, #0\n mov x30, #0\n mov x0, sp\n and sp, x0, #-16\n bl cstart\n brk #0\n");
