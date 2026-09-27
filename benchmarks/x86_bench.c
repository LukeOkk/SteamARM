// Freestanding x86-64 Linux benchmark: no libc, no rootfs, raw syscalls only.
// Built on the Mac with clang + lld (see benchmarks/README.md). The point is
// translation throughput: the same static ELF runs under FEX on a Linux kernel
// and under FEX under the runtime on macOS, and the two numbers are compared.
//
// Phases: sieve of Eratosthenes to 16M (branchy, memory-bound bytes), FNV-1a
// over a 32 MiB buffer x8 (tight integer loop), and a 4 MiB block copy x32.
// Each prints milliseconds from CLOCK_MONOTONIC.
typedef unsigned long u64; typedef unsigned int u32; typedef unsigned char u8;
#if defined(__x86_64__)
// x86-64 Linux: syscall numbers clock_gettime=228 write=1 mmap=9 exit_group=231
#define NR_clock_gettime 228
#define NR_write 1
#define NR_mmap 9
#define NR_exit_group 231
static long sys3(long n, long a, long b, long c) {
    long r; __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory"); return r; }
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
    long r; register long r10 __asm__("r10") = d; register long r8 __asm__("r8") = e; register long r9 __asm__("r9") = f;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9) : "rcx", "r11", "memory"); return r; }
#elif defined(__aarch64__)
// aarch64 Linux, the "no FEX" column: same program, native syscalls.
#define NR_clock_gettime 113
#define NR_write 64
#define NR_mmap 222
#define NR_exit_group 94
static long sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long x8 __asm__("x8") = n; register long x0 __asm__("x0") = a; register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c; register long x3 __asm__("x3") = d; register long x4 __asm__("x4") = e; register long x5 __asm__("x5") = f;
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc"); return x0; }
static long sys3(long n, long a, long b, long c) { return sys6(n, a, b, c, 0, 0, 0); }
#endif
struct ts { long s, ns; };
static u64 now_ms(void) { struct ts t; sys3(NR_clock_gettime, 1, (long)&t, 0); return (u64)t.s * 1000 + (u64)t.ns / 1000000; }
static void out(const char *s) { long n = 0; while (s[n]) n++; sys3(NR_write, 1, (long)s, n); }
static void outnum(const char *label, u64 v) {
    char b[32]; int i = 31; b[i] = 0; if (!v) b[--i] = '0'; while (v) { b[--i] = '0' + v % 10; v /= 10; }
    out(label); out(b + i); out(" ms\n"); }
void _start(void) {
    const u64 N = 16u << 20, BUF = 32u << 20;
    u8 *sieve = (u8 *)sys6(NR_mmap, 0, N, 3, 0x22, -1, 0);       // mmap rw anon
    u8 *buf = (u8 *)sys6(NR_mmap, 0, BUF, 3, 0x22, -1, 0);
    u8 *dst = (u8 *)sys6(NR_mmap, 0, 4u << 20, 3, 0x22, -1, 0);
    u64 t0 = now_ms();
    for (u64 i = 0; i < N; i++) sieve[i] = 1;
    u64 primes = 0;
    for (u64 i = 2; i < N; i++) { if (!sieve[i]) continue; primes++; for (u64 j = i * i; j < N; j += i) sieve[j] = 0; }
    u64 t1 = now_ms();
    for (u64 i = 0; i < BUF; i++) buf[i] = (u8)(i * 2654435761u >> 13);
    u64 h = 1469598103934665603ull;
    for (int r = 0; r < 8; r++) for (u64 i = 0; i < BUF; i++) { h ^= buf[i]; h *= 1099511628211ull; }
    u64 t2 = now_ms();
    for (int r = 0; r < 32; r++) { u64 *s = (u64 *)(buf + (u64)r * (1u << 20) % (BUF - (4u << 20))), *d = (u64 *)dst; for (u64 i = 0; i < (4u << 20) / 8; i++) d[i] = s[i]; }
    u64 t3 = now_ms();
    outnum("sieve16M:  ", t1 - t0); outnum("fnv32MiBx8:", t2 - t1); outnum("copy4MiBx32:", t3 - t2);
    outnum("total:     ", t3 - t0);
    outnum("fnv:       ", h % 1000000007ull); if (primes != 1077871) out("CHECK FAILED\n"); else out("check ok (1077871 primes)\n");
    sys3(NR_exit_group, 0, 0, 0);
}
