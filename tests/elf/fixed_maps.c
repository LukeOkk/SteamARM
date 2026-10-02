// The addresses a 64-bit Wine asks for by number, from an x86-64 program
// under FEX (tests/elf/run_fex_rules.sh), each start of which is a new
// process with a new place for the host's heap:
//  - MAP_FIXED at 0x140000000, where every 64-bit Windows program is linked
//    to load: the runtime keeps that range for the guest (runtime/arena.c);
//    the host's malloc sat there in one start in eight and Wine ran a
//    program that was never mapped;
//  - MAP_FIXED_NOREPLACE just above 4 GiB, where Wine starts looking for
//    room for a DLL: "taken" (EEXIST), so it goes on -- ENOMEM ended the
//    search and the DLL was not loaded;
//  - the same at the top of the 47-bit address space, which Darwin does not
//    have: EEXIST again, for Wine's top-down allocations.
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

int main(void)
{
    int ok = 1;
    char *p = mmap((void *)0x140000000ul, 0x2a000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p != (char *)0x140000000ul) { printf("MAP_FIXED 0x140000000: %s\n", strerror(errno)); ok = 0; }
    else { p[0] = 1; p[0x29fff] = 2; }
    // Once mapped, the same range is busy for MAP_FIXED_NOREPLACE; once unmapped, free again.
    void *q = mmap((void *)0x140000000ul, 0x1000, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (q != MAP_FAILED || errno != EEXIST) { printf("NOREPLACE over a mapping: %p %s\n", q, strerror(errno)); ok = 0; }
    if (p != MAP_FAILED) munmap(p, 0x2a000);
    q = mmap((void *)0x140000000ul, 0x10000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (q != (void *)0x140000000ul) { printf("NOREPLACE after munmap: %p %s\n", q, strerror(errno)); ok = 0; }
    q = mmap((void *)0x100000000ul, 0x100000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (q != MAP_FAILED || errno != EEXIST) { printf("NOREPLACE at 4 GiB: %p %s\n", q, strerror(errno)); ok = 0; }
    q = mmap((void *)0x7ffffffd0000ul, 0x1f000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (q != MAP_FAILED || errno != EEXIST) { printf("NOREPLACE at the top: %p %s\n", q, strerror(errno)); ok = 0; }
    printf("== fixed_maps: %s\n", ok ? "ok" : "FAIL");
    return !ok;
}
