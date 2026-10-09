// dual_map_jit: a JIT's dual mapping (GStreamer's ORC): one file mapped
// PROT_READ|PROT_EXEC and PROT_READ|PROT_WRITE, both MAP_SHARED; code written
// through the second runs from the first. The runtime used to make the
// executable view a private snapshot of the still-empty file, and the call
// ran zeros (benchmarks/stage62, 15).
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    char path[] = "/tmp/dual_map_jit-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 1; }
    unlink(path);
    const size_t size = 65536;
    if (ftruncate(fd, size) != 0) { perror("ftruncate"); return 1; }
    uint32_t *x = mmap(NULL, size, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    uint32_t *w = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (x == MAP_FAILED || w == MAP_FAILED) { perror("mmap"); return 1; }
    int bad = 0;
    for (int k = 0; k < 4; k++) {
        // mov w0, #(42+k); ret -- at a fresh page each round
        uint32_t *at = w + k * 1024;
        at[0] = 0x52800000u | ((uint32_t)(42 + k) << 5);
        at[1] = 0xd65f03c0u;
        __builtin___clear_cache((char *)(x + k * 1024), (char *)(x + k * 1024 + 2));
        int (*fn)(void) = (int (*)(void))(void *)(x + k * 1024);
        int got = fn();
        if (got != 42 + k) { printf("round %d: returned %d\n", k, got); bad++; }
    }
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad != 0;
}
