// A large anonymous mapping at an address 8 KiB into a 16 KiB host page, as
// Wine's ntdll reserves its low address space (0x112000 up to 0x68000000):
// contents zero at both edges and inside, a 4 KiB mprotect inside it works,
// and -- checked by the runner with vmmap while this sleeps -- the process
// does not end up with one kernel map entry per host page (105,000 for Wine's
// 1.6 GiB, a million for a game's start; runtime/subpage.c).
// Prints "mapped <pid>", sleeps 3 s, then "== subpage_big_anon: ok" or FAIL.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MB (1024UL * 1024)

int main(void)
{
    size_t span = 520 * MB;
    uint8_t *res = mmap(NULL, span, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (res == MAP_FAILED) { printf("FAIL reserve\n"); return 1; }
    uint8_t *base = (uint8_t *)(((uintptr_t)res + 16384 - 1) & ~(uintptr_t)16383);
    int ok = 1;

    // PROT_NONE over 512 MiB from 8 KiB into a host page (Wine's reservation).
    uint8_t *none = base + 0x2000;
    if (mmap(none, 512 * MB, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0) != none) {
        printf("FAIL PROT_NONE map\n"); return 1;
    }
    // Then a committed read-write piece inside it, as an allocation would be.
    uint8_t *rw = none + 64 * MB + 0x1000;
    if (mmap(rw, 32 * MB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != rw) {
        printf("FAIL read-write map\n"); return 1;
    }
    size_t probes[] = { 0, 4096, 16 * MB, 32 * MB - 4096, 32 * MB - 1 };
    for (unsigned i = 0; i < sizeof probes / sizeof *probes; i++)
        if (rw[probes[i]] != 0) { printf("FAIL not zero at +0x%zx\n", probes[i]); ok = 0; }
    memset(rw, 0xa5, 32 * MB);
    if (rw[0] != 0xa5 || rw[32 * MB - 1] != 0xa5) { printf("FAIL write\n"); ok = 0; }
    // A 4 KiB page made read-only and one made writable inside the PROT_NONE part.
    if (mprotect(rw + 8 * MB, 4096, PROT_READ) != 0) { printf("FAIL mprotect ro\n"); ok = 0; }
    if (rw[8 * MB] != 0xa5) { printf("FAIL read after mprotect\n"); ok = 0; }
    uint8_t *g = none + 300 * MB;
    if (mprotect(g, 4096, PROT_READ | PROT_WRITE) != 0) { printf("FAIL mprotect rw\n"); ok = 0; }
    else { g[0] = 7; if (g[0] != 7 || g[4095] != 0) { printf("FAIL guest page\n"); ok = 0; } }

    printf("mapped %d\n", (int)getpid());
    fflush(stdout);
    sleep(3);
    munmap(res, span);
    printf("== subpage_big_anon: %s\n", ok ? "ok" : "FAIL");
    return !ok;
}
