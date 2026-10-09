// shared_view_cycle: Wine maps a section view (a shared mapping of a memfd
// shorter than a 16 KiB host page) and releases it by mapping PROT_NONE over
// exactly its length, over and over at one address. Every view must stay
// shared: a write through it must reach the file. After the first release the
// runtime used to adopt the rest of the host page as live memory, and every
// later view was a private copy (benchmarks/stage62, 15).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    int fd = memfd_create("shared_view_cycle", 0);
    if (fd < 0 || ftruncate(fd, 0x2000) != 0) { perror("memfd"); return 1; }
    // A 64 KiB reservation, as Wine's views sit at 64 KiB granularity.
    uint8_t *res = mmap(NULL, 0x20000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (res == MAP_FAILED) { perror("reserve"); return 1; }
    uint8_t *base = (uint8_t *)(((uintptr_t)res + 0xffff) & ~(uintptr_t)0xffff);
    int bad = 0;
    for (int i = 1; i <= 50; i++) {
        uint8_t *v = mmap(base, 0x2000, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0);
        if (v != base) { perror("view"); return 1; }
        if (v[0] != (uint8_t)(i - 1)) { printf("view %d reads %d, not %d\n", i, v[0], i - 1); bad++; }
        v[0] = (uint8_t)i;
        v[0x1fff] = (uint8_t)i;
        if (mmap(base, 0x2000, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != base) {
            perror("release"); return 1;
        }
        uint8_t got[2];
        if (pread(fd, &got[0], 1, 0) != 1 || pread(fd, &got[1], 1, 0x1fff) != 1) { perror("pread"); return 1; }
        if (got[0] != (uint8_t)i || got[1] != (uint8_t)i) {
            printf("view %d: file reads %d/%d after the write\n", i, got[0], got[1]);
            bad++;
        }
        if (bad > 3) break;
    }
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad != 0;
}
