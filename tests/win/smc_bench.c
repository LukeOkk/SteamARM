// Self-modifying x64 code at the rate a protector rewrites its stubs, for the
// cost of one SMC round under FEX: a thread writes a small function into a
// read-write-execute ring slot, calls it, rewrites its immediate and calls it
// again. Every rewrite of code FEX has translated is a write fault on a page
// FEX protected, an invalidation under FEX's code lock, and a recompile.
// Minecraft Dungeons II's protector does this ~3,000 times a second into a 16
// MiB ring (benchmarks/stage62, 15 q). Other threads keep compiling and
// running ordinary code meanwhile, to show the lock waits.
//   smc_bench.exe [result file] [writer threads 1-4] [seconds]   (or SMC_WRITERS, SMC_SECONDS)
// Prints rounds per second and how many calls returned the wrong value (a
// stale translation).
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static volatile LONG stop, bad;
static volatile LONG64 rounds, other;
static uint8_t *ring;
static const size_t RING = 16 << 20, SLOT = 256;

// mov eax, imm32 ; ret
static void emit(uint8_t *p, uint32_t v)
{
    p[0] = 0xb8;
    memcpy(p + 1, &v, 4);
    p[5] = 0xc3;
}

static DWORD WINAPI writer(void *arg)
{
    uintptr_t id = (uintptr_t)arg;
    // Each writer walks its own quarter of the ring, slot by slot, wrapping:
    // the protector's ring pattern (a slot is rewritten once per lap).
    size_t base = id * (RING / 4), slots = (RING / 4) / SLOT;
    uint32_t v = (uint32_t)id << 24;
    for (size_t i = 0; !stop; i = (i + 1) % slots) {
        uint8_t *p = ring + base + i * SLOT;
        for (int k = 0; k < 2; k++) {
            v++;
            emit(p, v);
            FlushInstructionCache(GetCurrentProcess(), p, 8);
            uint32_t got = ((uint32_t (*)(void))p)();
            if (got != v)
                InterlockedIncrement(&bad);
        }
        InterlockedIncrement64(&rounds);
    }
    return 0;
}

// Ordinary code that keeps translated blocks hot: a small interpreter loop.
static DWORD WINAPI busy(void *arg)
{
    (void)arg;
    volatile uint64_t x = 1;
    while (!stop) {
        for (int i = 0; i < 100000; i++)
            x = x * 6364136223846793005ull + 1442695040888963407ull + (uint64_t)(x >> 7);
        InterlockedIncrement64(&other);
    }
    return 0;
}

int main(int argc, char **argv)
{
    FILE *out = fopen(argc > 1 ? argv[1] : "smc_bench.txt", "a");
    // The launch harnesses pass no arguments: SMC_WRITERS / SMC_SECONDS too.
    const char *ew = getenv("SMC_WRITERS"), *es = getenv("SMC_SECONDS");
    int nw = argc > 2 ? atoi(argv[2]) : ew ? atoi(ew) : 1;
    int secs = argc > 3 ? atoi(argv[3]) : es ? atoi(es) : 20;
    if (nw < 1 || nw > 4) nw = 1;
    if (secs < 1 || secs > 600) secs = 20;
    ring = VirtualAlloc(NULL, RING, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!ring) {
        fprintf(out ? out : stdout, "smc_bench: VirtualAlloc failed %lu\n", GetLastError());
        return 1;
    }
    HANDLE t[6];
    for (int i = 0; i < nw; i++) t[i] = CreateThread(NULL, 0, writer, (void *)(uintptr_t)i, 0, NULL);
    t[nw] = CreateThread(NULL, 0, busy, NULL, 0, NULL);
    Sleep(secs * 1000);
    stop = 1;
    WaitForMultipleObjects(nw + 1, t, TRUE, 10000);
    fprintf(out ? out : stdout, "smc_bench: %d writers, %lld rounds in %d s (%.0f/s), %ld stale, busy loops %lld\n",
            nw, (long long)rounds, secs, (double)rounds / secs, (long)bad, (long long)other);
    return 0;
}
