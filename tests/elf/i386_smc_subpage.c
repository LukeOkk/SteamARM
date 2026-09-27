// i386 probe: self-modifying code in a 4 KiB page that shares its 16 KiB host
// page with other writable guest pages. FEX catches writes to translated code
// by write-protecting the guest page (SMC tracking, "mtrack"); on a 16 KiB
// host page that protection is only the union with the neighbours, which are
// writable, so the write went through unseen and FEX kept running the old
// translation. V8's WebAssembly lazy compilation patches a jump table this
// way: the renderer compiled the same function forever and died with "V8
// process OOM (Exceeding maximum wasm committed code space)".
// Patches a function five times, each time calling it before and after, with
// the patched page and a data page in the same 16 KiB host page.
#include <stddef.h>
#include <stdint.h>
extern int printf(const char *, ...);
extern void exit(int) __attribute__((noreturn));
extern void *mmap(void *, size_t, int, int, int, long);

typedef int (*fn_t)(void);

static void emit(volatile uint8_t *p, int v)
{
    p[0] = 0xb8;                              // mov eax, imm32
    p[1] = (uint8_t)v; p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)(v >> 16); p[4] = (uint8_t)(v >> 24);
    p[5] = 0xc3;                              // ret
}

static int check(void)
{
    uintptr_t base = (uintptr_t)mmap(0, 65536, 7, 0x22, -1, 0);   // RWX, private anonymous
    uintptr_t hp = (base + 16383) & ~(uintptr_t)16383;
    volatile uint8_t *code = (volatile uint8_t *)(hp + 0x1000);    // second 4 KiB of the host page
    volatile uint32_t *data = (volatile uint32_t *)(hp + 0x2000);  // third: plain data
    int bad = 0;
    for (int round = 1; round <= 5; round++) {
        emit(code, round * 10);
        data[0] = (uint32_t)round;
        int got = ((fn_t)(uintptr_t)code)();
        int again = ((fn_t)(uintptr_t)code)();
        printf("round %d: %d %d (want %d)\n", round, got, again, round * 10);
        if (got != round * 10 || again != round * 10)
            bad++;
    }
    // Known gap (FEX patch note): 32-bit guests keep 4 KiB SMC tracking, so
    // this is expected to fail until that is solved another way.
    printf(bad ? "== smc subpage: xfail (32-bit SMC in a shared 16 KiB page)\n" : "== smc subpage: ok\n");
    return bad != 0;
}
void _start(void) { exit(check()); }
