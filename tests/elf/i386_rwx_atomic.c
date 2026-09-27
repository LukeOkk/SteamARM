// i386 probe: an unaligned locked instruction on a page the guest made
// read-write-execute (V8 does this with its code space). The 4 KiB page
// shares a 16 KiB host page with read-write neighbours, so the runtime tracks
// it as a sub-page mapping whose host page wants write AND execute. FEX
// emulates an atomic that straddles 16 bytes from the SIGBUS alignment fault;
// the runtime's sub-page W^X handler used to take that SIGBUS for a
// protection fault, mprotect, and retry forever (steamwebhelper renderer,
// ~440k SIGBUS/s, Steam UI never loaded).
#include <stddef.h>
#include <stdint.h>
extern int printf(const char *, ...);
extern void exit(int) __attribute__((noreturn));
extern void *mmap(void *, size_t, int, int, int, long);
extern int mprotect(void *, size_t, int);

static int check(void)
{
    uintptr_t base = (uintptr_t)mmap(0, 65536, 3, 0x22, -1, 0);   // RW, private anonymous
    uintptr_t page = (base + 16383) & ~(uintptr_t)16383;
    if (mprotect((void *)page, 4096, 7) != 0) {                      // RWX, 4 KiB of a 16 KiB page
        printf("mprotect RWX failed\n");
        return 1;
    }
    // 4 bytes at +0x8e straddle the 16-byte boundary at +0x90.
    volatile uint32_t *p = (volatile uint32_t *)(page + 0x8e);
    *p = 0;
    for (int i = 0; i < 1000; i++) {
        uint32_t one = 1;
        __asm__ volatile("lock xaddl %0, %1" : "+r"(one), "+m"(*p) :: "memory");
    }
    printf("value %u\n", *p);
    int ok = *p == 1000;
    printf(ok ? "== rwx atomic: ok\n" : "== rwx atomic: MAL\n");
    return !ok;
}
void _start(void) { exit(check()); }
