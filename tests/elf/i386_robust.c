// i386 probe: get_robust_list must hand back what glibc registered with
// set_robust_list, as a 32-bit guest pointer and len 12. The Steam client
// checks exactly this (head != NULL, len == 12, head->futex_offset == -20)
// and deliberately crashes otherwise (steamclient.so, userinterface.cpp).
#include <stdint.h>
extern int printf(const char *, ...);
extern long syscall(long, ...);
extern void exit(int) __attribute__((noreturn));
static int check(void)
{
    uint32_t head = 0xdeadbeef, len = 0;
    long r = syscall(312, 0, &head, &len);       // get_robust_list(0, &head, &len)
    int32_t off = head && head != 0xdeadbeef ? ((volatile int32_t *)(uintptr_t)head)[1] : 0;
    printf("get_robust_list -> %ld head 0x%x len %u futex_offset %d\n", r, head, len, off);
    int ok = r == 0 && head && head != 0xdeadbeef && len == 12 && off == -20;
    printf(ok ? "== robust: ok\n" : "== robust: MAL\n");
    return !ok;
}
void _start(void) { exit(check()); }
