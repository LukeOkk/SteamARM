// MADV_DONTNEED on private anonymous memory must read back as zeros (Linux
// semantics). FEX clears its lookup caches and call-return stack this way;
// stale entries there send execution into freed code buffers.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

static int check(const char *what, uint8_t *p, size_t len)
{
    memset(p, 0xa5, len);
    if (madvise(p, len, MADV_DONTNEED) != 0) { printf("  MAL  %s: madvise failed\n", what); return 1; }
    for (size_t i = 0; i < len; i++)
        if (p[i]) { printf("  MAL  %s: byte %zu = 0x%02x after MADV_DONTNEED\n", what, i, p[i]); return 1; }
    printf("  OK   %s\n", what);
    return 0;
}

int main(void)
{
    int bad = 0;
    uint8_t *a = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    bad += check("1 MiB anonymous", a, 1 << 20);
    bad += check("4 KiB inside", a + 4096 * 3, 4096);
    bad += check("unaligned 4 KiB span", a + 4096 * 5, 4096 * 7);
    // FEX's call-ret stack shape: PROT_NONE reservation, inner part RW, 4 KiB guards.
    uint8_t *r = mmap(NULL, 1 << 20, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    mprotect(r + 4096, (1 << 20) - 8192, PROT_READ | PROT_WRITE);
    bad += check("guarded RW span (call-ret stack shape)", r + 4096, (1 << 20) - 8192);
    // FEX's lookup cache for a 64-bit guest: one pointer per 4 KiB of a
    // 2^47 address space, reserved MAP_NORESERVE and touched sparsely.
    size_t big = (size_t)1 << 38;
    uint8_t *L = mmap(NULL, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (L == MAP_FAILED) { printf("  MAL  256 GiB reserve failed\n"); bad++; }
    else {
        size_t offs[] = { 0, 4096 * 17, (size_t)1 << 30, ((size_t)1 << 37) + 12345, big - 64 };
        for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++) L[offs[i]] = 0x5a;
        int r = madvise(L, big, MADV_DONTNEED);
        int stale = 0;
        for (unsigned i = 0; i < sizeof offs / sizeof offs[0]; i++) stale += L[offs[i]] != 0;
        printf(r == 0 && !stale ? "  OK   256 GiB sparse lookup-cache shape\n"
                                : "  MAL  256 GiB sparse lookup-cache shape: madvise %d, %d stale\n", r, stale);
        bad += r != 0 || stale;
    }
    printf("== madvise zero: %s\n", bad ? "MAL" : "ok");
    return bad;
}
