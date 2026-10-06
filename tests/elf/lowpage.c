// Virtual pages below 4 GiB for a native guest (runtime/lowpage.c,
// LXRT_LOWPAGES=1): the page Windows keeps at 0x7ffe0000 (KUSER_SHARED_DATA),
// which Wine ARM64 maps fixed. macOS has nothing below 4 GiB, so the runtime
// keeps the memory elsewhere and carries out each faulting load and store
// itself. Every addressing form a compiler or Wine emits is tried: unsigned
// offset, unscaled, pre- and post-index with writeback, register offset with
// an extend, pairs, sign-extending loads, acquire/release, SIMD registers,
// and what the compiler and memset pick. Then the page is made read-only (a
// store must fault), unmapped (any access must fault), and the probes Wine's
// allocator makes elsewhere below 4 GiB must be told "busy" (EEXIST) for a
// no-replace request and ENOMEM for a fixed one. Without LXRT_LOWPAGES the
// page is refused as before.
#define _GNU_SOURCE
#include <errno.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

static int pass, fail;
static void check(int c, const char *what)
{
    printf("  %s  %s\n", c ? "OK  " : "FAIL", what);
    if (c) pass++; else fail++;
}

static sigjmp_buf jb;
static volatile int got_fault;
static void on_fault(int sig)
{
    (void)sig;
    got_fault = 1;
    siglongjmp(jb, 1);
}

int main(void)
{
    const uintptr_t usd = 0x7ffe0000;
    const char *e = getenv("LXRT_LOWPAGES");
    int on = e && e[0] == '1';
    void *p = mmap((void *)usd, 4096, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (!on) {
        check(p == MAP_FAILED && errno == ENOMEM, "without LXRT_LOWPAGES a page below 4 GiB is refused (ENOMEM)");
        printf("lowpage: %d ok, %d failed\n", pass, fail);
        return fail != 0;
    }
    check(p == (void *)usd, "MAP_FIXED_NOREPLACE at 0x7ffe0000 answered with the address");
    void *q = mmap((void *)0x10000, 0x10000, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    check(q == MAP_FAILED && errno == EEXIST, "a no-replace probe elsewhere below 4 GiB: EEXIST (busy)");
    q = mmap((void *)0x20000000, 0x10000, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    check(q == MAP_FAILED && errno == ENOMEM, "a fixed mapping elsewhere below 4 GiB: ENOMEM");

    uint64_t v64 = 0x1122334455667788ull;
    uint32_t v32 = 0xA1B2C3D4u;
    uint16_t v16 = 0xBEEF;
    uint8_t v8 = 0x5A;
    __asm__ volatile("str %0, [%1]" :: "r"(v64), "r"(usd) : "memory");
    __asm__ volatile("str %w0, [%1, #8]" :: "r"(v32), "r"(usd) : "memory");
    __asm__ volatile("strh %w0, [%1, #12]" :: "r"(v16), "r"(usd) : "memory");
    __asm__ volatile("strb %w0, [%1, #14]" :: "r"(v8), "r"(usd) : "memory");
    __asm__ volatile("stp %0, %1, [%2, #16]" :: "r"(v64 + 1), "r"(v64 + 2), "r"(usd) : "memory");
    __asm__ volatile("stur %0, [%1, #-8]" :: "r"(v64 + 3), "r"(usd + 48) : "memory");   // at 40
    {
        uint64_t a = usd + 56;
        __asm__ volatile("str %1, [%0], #8" : "+r"(a) : "r"(v64 + 4) : "memory");       // at 56
        check(a == usd + 64, "post-index store writes the base back");
    }
    {
        uint64_t a = usd + 56;
        __asm__ volatile("str %1, [%0, #8]!" : "+r"(a) : "r"(v64 + 5) : "memory");      // at 64
        check(a == usd + 64, "pre-index store writes the base back");
    }
    {
        uint64_t off = 72;
        __asm__ volatile("str %0, [%1, %2]" :: "r"(v64 + 6), "r"(usd), "r"(off) : "memory");
    }
    {
        uint32_t idx = 10;
        __asm__ volatile("str %0, [%1, %w2, uxtw #3]" :: "r"(v64 + 7), "r"(usd), "r"(idx) : "memory");  // at 80
    }

    uint64_t r64, rs;
    uint32_t r32;
    __asm__ volatile("ldr %0, [%1]" : "=r"(r64) : "r"(usd));
    check(r64 == v64, "ldr x reads what str x stored");
    __asm__ volatile("ldr %w0, [%1, #8]" : "=r"(r32) : "r"(usd));
    check(r32 == v32, "ldr w");
    __asm__ volatile("ldrh %w0, [%1, #12]" : "=r"(r32) : "r"(usd));
    check(r32 == v16, "ldrh");
    __asm__ volatile("ldrb %w0, [%1, #14]" : "=r"(r32) : "r"(usd));
    check(r32 == v8, "ldrb");
    __asm__ volatile("ldrsh %0, [%1, #12]" : "=r"(rs) : "r"(usd));
    check(rs == (uint64_t)(int64_t)(int16_t)v16, "ldrsh sign-extends to 64 bits");
    __asm__ volatile("ldrsb %w0, [%1, #14]" : "=r"(r32) : "r"(usd));
    check(r32 == (uint32_t)(int32_t)(int8_t)v8, "ldrsb sign-extends to 32 bits");
    {
        uint64_t a, b;
        __asm__ volatile("ldp %0, %1, [%2, #16]" : "=r"(a), "=r"(b) : "r"(usd));
        check(a == v64 + 1 && b == v64 + 2, "ldp reads the pair stp stored");
    }
    __asm__ volatile("ldur %0, [%1, #-8]" : "=r"(r64) : "r"(usd + 48));
    check(r64 == v64 + 3, "ldur reads what stur stored");
    __asm__ volatile("ldr %0, [%1, #56]" : "=r"(r64) : "r"(usd));
    check(r64 == v64 + 4, "the post-index store landed");
    __asm__ volatile("ldr %0, [%1, #64]" : "=r"(r64) : "r"(usd));
    check(r64 == v64 + 5, "the pre-index store landed");
    __asm__ volatile("ldr %0, [%1, #72]" : "=r"(r64) : "r"(usd));
    check(r64 == v64 + 6, "the register-offset store landed");
    __asm__ volatile("ldr %0, [%1, #80]" : "=r"(r64) : "r"(usd));
    check(r64 == v64 + 7, "the uxtw #3 store landed");
    {
        uint64_t a = usd + 16, x;
        __asm__ volatile("ldr %0, [%1], #8" : "=r"(x), "+r"(a));
        check(x == v64 + 1 && a == usd + 24, "ldr post-index");
    }
    {
        uint64_t a = usd + 16, x;
        __asm__ volatile("ldr %0, [%1, #8]!" : "=r"(x), "+r"(a));
        check(x == v64 + 2 && a == usd + 24, "ldr pre-index");
    }
    __asm__ volatile("stlr %0, [%1]" :: "r"(v64 + 9), "r"(usd + 88) : "memory");
    __asm__ volatile("ldar %0, [%1]" : "=r"(r64) : "r"(usd + 88));
    check(r64 == v64 + 9, "ldar reads what stlr stored");
    {
        uint64_t a, b;
        __asm__ volatile("ldr q0, [%0, #16]\n\tstr q0, [%0, #96]" :: "r"(usd) : "memory", "v0");
        __asm__ volatile("ldp %0, %1, [%2, #96]" : "=r"(a), "=r"(b) : "r"(usd));
        check(a == v64 + 1 && b == v64 + 2, "ldr q / str q move 16 bytes");
    }
    {
        volatile uint64_t *w = (volatile uint64_t *)usd;
        w[13] = 0xCAFEF00Dull;
        check(w[13] == 0xCAFEF00Dull, "the compiler's own store and load");
    }
    memset((void *)(usd + 200), 0x77, 50);
    check(((uint8_t *)usd)[249] == 0x77 && ((uint8_t *)usd)[250] == 0, "memset through the page");

    check(mprotect((void *)usd, 4096, PROT_READ) == 0, "mprotect PROT_READ");
    signal(SIGSEGV, on_fault);
    signal(SIGBUS, on_fault);
    got_fault = 0;
    if (sigsetjmp(jb, 1) == 0)
        *(volatile uint64_t *)usd = 1;
    check(got_fault == 1, "a store on the read-only page faults");
    __asm__ volatile("ldr %0, [%1]" : "=r"(r64) : "r"(usd));
    check(r64 == v64, "a load still works on the read-only page");
    check(munmap((void *)usd, 4096) == 0, "munmap");
    got_fault = 0;
    if (sigsetjmp(jb, 1) == 0)
        r64 = *(volatile uint64_t *)usd;
    check(got_fault == 1, "after munmap the address faults again");
    printf("lowpage: %d ok, %d failed\n", pass, fail);
    return fail != 0;
}
