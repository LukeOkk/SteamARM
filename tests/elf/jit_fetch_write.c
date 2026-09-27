// A thread that runs JIT code while still in WRITE mode.
//
// On Apple silicon a MAP_JIT page cannot be executed by a thread in write
// mode, and a flip to execute made inside the fault handler does not survive
// sigreturn when the thread was interrupted in write mode (measured, macOS
// 27). This happens when a signal handler redirects a thread that was
// emitting code into generated code (FEX delivering a guest signal). The
// runtime parks the context, runs a stub that flips on the thread's own
// stack, and resumes (runtime/jit.c, trampoline.S). Every argument register
// and the flags must survive the detour.
#include <stdint.h>
#include <stdio.h>
#include <sys/mman.h>

static long lxrt_jit_wx(long enable, uint64_t addr, uint64_t len)
{
    register long x8 asm("x8") = 0x4C580020;
    register long x0 asm("x0") = enable;
    register uint64_t x1 asm("x1") = addr;
    register uint64_t x2 asm("x2") = len;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

int main(void)
{
    uint32_t *code = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { printf("mmap failed\n"); return 1; }
    int ok = 1;
    for (int round = 0; round < 3; round++) {
        lxrt_jit_wx(0, 0, 0);                   // write mode, and stay there
        // add x0, x0, x1 ; add x0, x0, x7 ; ret
        code[0] = 0x8b010000; code[1] = 0x8b070000; code[2] = 0xd65f03c0;
        __builtin___clear_cache((char *)code, (char *)(code + 3));
        long (*fn)(long, long, long, long, long, long, long, long) = (void *)code;
        long r = fn(40, 2, 0, 0, 0, 0, 0, 100 * round);   // fetch in write mode
        printf("round %d: %ld (want %d)\n", round, r, 42 + 100 * round);
        ok &= r == 42 + 100 * round;
    }
    printf(ok ? "== jit fetch in write mode: ok\n" : "== jit fetch in write mode: MAL\n");
    return !ok;
}
