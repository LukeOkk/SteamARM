// x18 is an ordinary temporary on Linux. Darwin zeroes it on every exception
// return (benchmarks/stage5-x18.txt), so under the runtime it is virtualised:
// each instruction naming x18 is rewritten to work on a per-thread slot. This
// program keeps values in x18 across everything Darwin can interrupt with,
// through every instruction form the census found in FEX and glibc, and
// checks them. On a Linux kernel it passes trivially; under the runtime it
// passes only with the x18 pass. Build: gcc -O1 -static-pie -pthread.
//
// Every x18 use is in inline asm with explicit clobbers, so the compiler's
// own (possible) use of x18 never confuses the checks: the test measures the
// rewriter, not gcc's register allocator.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static int ok, bad;
#define CHECK(name, cond) do { if (cond) { ok++; printf("  ok    %s\n", name); } \
                               else { bad++; printf("  MAL   %s\n", name); } } while (0)

#define MAGIC 0x1234567800abcdefULL
#define SET18(v) __asm__ volatile("mov x18, %0" :: "r"((uint64_t)(v)) : "x18")
static inline uint64_t get18(void) { uint64_t v; __asm__ volatile("mov %0, x18" : "=r"(v)); return v; }

static void handler(int s) { (void)s; SET18(0xdeadULL); }   // a handler may trash it; sigreturn restores
static void *spin(void *a) { (void)a; for (volatile uint64_t i = 0; i < 400000000ULL; i++) ; return NULL; }
static uint64_t now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static void *thread_body(void *a)
{
    // A new thread starts with its own x18 and keeps it across a syscall.
    uint64_t v = (uint64_t)(uintptr_t)a;
    SET18(v);
    syscall(SYS_getpid);
    return (void *)(uintptr_t)(get18() == v);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    uint64_t v;

    SET18(MAGIC); v = get18();
    CHECK("mov x18 / mov from x18", v == MAGIC);

    SET18(MAGIC); syscall(SYS_getpid); v = get18();
    CHECK("x18 survives a raw syscall (getpid)", v == MAGIC);

    SET18(MAGIC); usleep(20000); v = get18();
    CHECK("x18 survives nanosleep (a context switch)", v == MAGIC);

    signal(SIGUSR1, handler);
    SET18(MAGIC); raise(SIGUSR1); v = get18();
    CHECK("x18 survives a signal whose handler clobbers it", v == MAGIC);

    // Preemption with no syscalls: 8 spinners compete for ~400 ms.
    pthread_t th[8];
    for (int i = 0; i < 8; i++) pthread_create(&th[i], NULL, spin, NULL);
    uint64_t t0 = now_ms(); int losses = 0;
    SET18(MAGIC);
    for (uint64_t i = 0; ; i++) {
        if (get18() != MAGIC) { losses++; SET18(MAGIC); }
        if ((i & 0xffff) == 0 && now_ms() - t0 > 400) break;
    }
    for (int i = 0; i < 8; i++) pthread_join(th[i], NULL);
    CHECK("x18 survives 400 ms of preemption (no syscalls)", losses == 0);

    // The instruction forms the census found. Each block sets x18, uses the
    // form, and reads back through a different path.
    uint64_t out = 0, buf[4] = {0, 0, 0, 0};
    __asm__ volatile("mov x18, %1\n add %0, x18, #5" : "=r"(out) : "r"(MAGIC) : "x18");
    CHECK("add Xd, x18, #imm", out == MAGIC + 5);
    __asm__ volatile("mov x18, %1\n str x18, [%2]\n ldr %0, [%2]" : "=r"(out) : "r"(MAGIC), "r"(buf) : "x18", "memory");
    CHECK("str x18, [Xn]", out == MAGIC);
    __asm__ volatile("mov x18, %1\n ldr x18, [x18]\n mov %0, x18" : "=r"(out) : "r"(buf) : "x18", "memory");
    CHECK("ldr x18, [x18] (base and destination)", out == MAGIC);
    // Not tested: a pre/post-indexed access with sp as the base (`str x18,
    // [sp, #-16]!`). Compiled code never pushes x18 (it is caller-saved) and
    // the planner refuses the form; accesses below sp collide with the
    // trampoline's own save area anyway (no red zone on AArch64 Linux).
    __asm__ volatile("sub sp, sp, #32\n mov x18, %1\n str x18, [sp, #8]\n ldr x18, [sp, #8]\n add x18, x18, #1\n mov %0, x18\n add sp, sp, #32" : "=r"(out) : "r"(MAGIC) : "x18", "memory");
    CHECK("str/ldr x18, [sp, #imm] (sp-relative, no writeback)", out == MAGIC + 1);
    __asm__ volatile("sub sp, sp, #32\n add x18, sp, #16\n mov x1, sp\n sub %0, x18, x1\n add sp, sp, #32" : "=r"(out) :: "x18", "x1");
    CHECK("add x18, sp, #imm", out == 16);
    __asm__ volatile("mov x18, %1\n stp x18, x18, [%2]\n ldp %0, x18, [%2]\n add %0, %0, x18" : "=&r"(out) : "r"(MAGIC), "r"(buf) : "x18", "memory");
    CHECK("stp/ldp with x18", out == 2 * MAGIC);
    __asm__ volatile("mov x18, %1\n cbz x18, 1f\n mov %0, #1\n b 2f\n1: mov %0, #2\n2:" : "=r"(out) : "r"(MAGIC) : "x18");
    CHECK("cbz x18 (not taken)", out == 1);
    __asm__ volatile("mov x18, #0\n cbz x18, 1f\n mov %0, #1\n b 2f\n1: mov %0, #2\n2:" : "=r"(out) :: "x18");
    CHECK("cbz x18 (taken)", out == 2);
    __asm__ volatile("mov x18, #4\n tbz w18, #2, 1f\n mov %0, #1\n b 2f\n1: mov %0, #2\n2:" : "=r"(out) :: "x18");
    CHECK("tbz w18 (not taken: bit set)", out == 1);
    __asm__ volatile("adrp x18, main\n add x18, x18, :lo12:main\n mov %0, x18" : "=r"(out) :: "x18");
    CHECK("adrp x18 + add :lo12:", out == (uint64_t)(uintptr_t)main);
    __asm__ volatile("mov x18, %1\n cmp x18, %1\n cset %0, eq" : "=r"(out) : "r"(MAGIC) : "x18", "cc");
    CHECK("cmp x18, Xm", out == 1);
    __asm__ volatile("mov x18, %1\n csel x18, x18, xzr, ne\n mov %0, x18" : "=r"(out) : "r"(MAGIC) : "x18", "cc");
    CHECK("csel x18", out == MAGIC || out == 0);
    __asm__ volatile("mov w18, #7\n mov %w0, w18" : "=r"(out) :: "x18");
    CHECK("mov w18, #imm (32-bit write zero-extends)", out == 7);
    __asm__ volatile("movz x18, #0x1234, lsl #16\n movk x18, #0x5678\n mov %0, x18" : "=r"(out) :: "x18");
    CHECK("movz/movk x18", out == 0x12345678ULL);
    buf[0] = 10;
    __asm__ volatile(".arch_extension lse\n mov x18, #10\n mov x1, #11\n casal x18, x1, [%1]\n mov %0, x18" : "=r"(out) : "r"(buf) : "x18", "x1", "memory");
    CHECK("casal x18, Xs, [Xn]", out == 10 && buf[0] == 11);
    __asm__ volatile("mov x18, %1\n fmov d0, x18\n fmov %0, d0" : "=r"(out) : "r"(MAGIC) : "x18", "d0");
    CHECK("fmov d0, x18 / fmov x, d0", out == MAGIC);
    __asm__ volatile("mrs x18, tpidr_el0\n mrs x1, tpidr_el0\n cmp x18, x1\n cset %0, eq" : "=r"(out) :: "x18", "x1", "cc");
    CHECK("mrs x18, TPIDR_EL0 equals mrs x1, TPIDR_EL0", out == 1);
    __asm__ volatile("mov x18, %1\n mul x18, x18, x18\n mov %0, x18" : "=r"(out) : "r"(3ULL) : "x18");
    CHECK("mul x18, x18, x18", out == 9);
    __asm__ volatile("mov x18, %1\n lsr x18, x18, #4\n mov %0, x18" : "=r"(out) : "r"(0x1230ULL) : "x18");
    CHECK("lsr x18, x18, #4", out == 0x123);

    pthread_t t2; void *r2 = NULL;
    pthread_create(&t2, NULL, thread_body, (void *)0x42);
    pthread_join(t2, &r2);
    CHECK("a new thread keeps its own x18 across a syscall", r2 == (void *)1);
    SET18(MAGIC); v = get18();
    CHECK("main thread's x18 untouched by the other thread", v == MAGIC);

    printf("== %d ok, %d mal\n", ok, bad);
    return bad ? 1 : 0;
}
