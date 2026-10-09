// suspend_storm: three threads compute the same thing twice and compare,
// while the main thread suspends each, reads its context and resumes it as
// fast as it can for 40 s -- what Minecraft Dungeons II's integrity scanner
// does to every thread of the game. Under FEX ARM64EC a suspension (Wine's
// SIGUSR1) that arrived inside the runtime's own delivery of another signal
// built its frame over that delivery's frame, and the process died within
// seconds (benchmarks/stage62, 15). Writes one line to the file named by
// argv[1] (default Z:\tmp\suspend_storm.txt); PASS is "0 mismatches" after
// the full 40 s from both builds. Built with llvm-mingw (x86_64- and
// aarch64-w64-mingw32-clang -O2) and run as a game through the native tool,
// e.g. in place of the exe in ~/SteamARM-build/bench/nativegame.sh.
#include <windows.h>
#include <stdio.h>
#include <math.h>
static volatile LONG stop, iters, bad;
static double work(unsigned seed)
{
    double a = seed * 0.5 + 1.0, b = 1.0;
    unsigned long long h = seed * 0x9E3779B97F4A7C15ull;
    double arr[64];
    for (int i = 0; i < 64; i++) arr[i] = (double)(h >> (i % 48)) * 1e-9 + i;
    for (int r = 0; r < 200; r++) {
        for (int i = 0; i < 64; i++) {
            a = a * 1.0000001 + arr[i] * 0.5;
            b += sqrt(fabs(a)) * 1e-3;
            h ^= (unsigned long long)(a * 1000.0) + (h << 7) + (h >> 3);
            if ((h & 7) == 3) arr[i] += 1.0; else arr[(i * 7) & 63] -= 0.25;
        }
    }
    return a + b + (double)(h & 0xffff);
}
static DWORD WINAPI worker(void *p)
{
    unsigned s = 1;
    while (!stop) {
        double x = work(s), y = work(s);
        if (memcmp(&x, &y, sizeof x)) InterlockedIncrement(&bad);
        InterlockedIncrement(&iters);
        s++;
    }
    return 0;
}
int main(int argc, char **argv)
{
    FILE *out = fopen(argc > 1 ? argv[1] : "Z:\\tmp\\suspend_storm.txt", "a");
    HANDLE t[3];
    for (int i = 0; i < 3; i++) t[i] = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    DWORD start = GetTickCount(); long susp = 0;
    while (GetTickCount() - start < 40000) {
        for (int i = 0; i < 3; i++) {
            if (SuspendThread(t[i]) == (DWORD)-1) continue;
            CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
            GetThreadContext(t[i], &c);
            ResumeThread(t[i]);
            susp++;
        }
    }
    stop = 1;
    WaitForMultipleObjects(3, t, TRUE, 10000);
    fprintf(out ? out : stdout, "suspend_storm %s: %ld suspend/get/resume, %ld paired computations, %ld mismatches\n",
#ifdef __aarch64__
        "arm64",
#else
        "x64",
#endif
        susp, (long)iters, (long)bad);
    return 0;
}
