// A 4 KiB guard page that shares its 16 KiB host page with live memory, as
// FEX's ARM64EC JIT lays out its call-return stack (VirtualAlloc: reserve
// size + 2 pages with no access, commit everything between the two guard
// pages): a stack that grows down must fault INSIDE the bottom guard, where
// FEX looks for the fault to reset the stack. The runtime cannot enforce a
// 4 KiB guard on a 16 KiB host page (the union of the protections is read-
// write); it reports a fault just below such a lost guard inside it
// (runtime/subpage.c lxrt_subpage_lost_guard). Run with LXRT_GUEST_PAGE=4096.
// Prints PASS when every fault address lands in the guard.
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf back;
static volatile uintptr_t fault_at;

static void on_segv(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)uc;
    fault_at = (uintptr_t)si->si_addr;
    siglongjmp(back, 1);
}

int main(void)
{
    const size_t page = 4096, stack = 1 << 20, granule = 1 << 16;
    int mal = 0;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_segv;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);

    for (int round = 0; round < 3; round++) {
        // A 64 KiB-aligned reservation, as Wine's VirtualAlloc gives.
        char *raw = mmap(NULL, stack + 2 * page + granule, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (raw == MAP_FAILED) { printf("  MAL mmap\n"); return 1; }
        char *alloc = (char *)(((uintptr_t)raw + granule - 1) & ~(uintptr_t)(granule - 1));
        char *base = alloc + page;
        if (mprotect(base, stack, PROT_READ | PROT_WRITE)) { printf("  MAL mprotect\n"); return 1; }

        // Grow down 16 bytes at a time from a quarter of the way up.
        volatile uint64_t *sp = (volatile uint64_t *)(base + stack / 4);
        fault_at = 0;
        if (sigsetjmp(back, 1) == 0) {
            for (;;) {
                sp -= 2;
                sp[0] = 1;
                sp[1] = 2;
            }
        }
        uintptr_t a = fault_at;
        if (a >= (uintptr_t)alloc && a < (uintptr_t)base)
            printf("  OK  round %d: fault at base-%#lx, in the bottom guard\n", round, (unsigned long)((uintptr_t)base - a));
        else {
            printf("  MAL round %d: fault at %#lx, guard is %p..%p\n", round, (unsigned long)a, (void *)alloc, (void *)base);
            mal++;
        }
        munmap(raw, stack + 2 * page + granule);
    }
    printf("%s\n", mal ? "FAIL" : "PASS");
    return mal != 0;
}
