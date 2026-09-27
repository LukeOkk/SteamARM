// Wine's clocks as a Windows program reads them: GetTickCount and
// GetTickCount64 come from KUSER_SHARED_DATA, a page wineserver keeps
// updating (runtime/shmirror.c); QueryPerformanceCounter and the system time
// do not. All four must advance by about a second across Sleep(1000).
#include <windows.h>
#include <stdio.h>

int main(void)
{
    LARGE_INTEGER q0, q1, fr;
    FILETIME a, b;
    QueryPerformanceFrequency(&fr);
    QueryPerformanceCounter(&q0);
    DWORD t0 = GetTickCount();
    ULONGLONG u0 = GetTickCount64();
    GetSystemTimeAsFileTime(&a);
    Sleep(1000);
    QueryPerformanceCounter(&q1);
    DWORD t1 = GetTickCount();
    ULONGLONG u1 = GetTickCount64();
    GetSystemTimeAsFileTime(&b);
    double qpc = (double)(q1.QuadPart - q0.QuadPart) / fr.QuadPart * 1000;
    double sys = ((((ULONGLONG)b.dwHighDateTime << 32) | b.dwLowDateTime) -
                  (((ULONGLONG)a.dwHighDateTime << 32) | a.dwLowDateTime)) / 1e4;
    double tick = t1 - t0, tick64 = (double)(u1 - u0);
    int ok = qpc > 900 && qpc < 1500 && sys > 900 && sys < 1500 &&
             tick > 900 && tick < 1500 && tick64 > 900 && tick64 < 1500;
    printf("ms across Sleep(1000): QPC %.0f, system time %.0f, GetTickCount %.0f, GetTickCount64 %.0f\n",
           qpc, sys, tick, tick64);
    printf(ok ? "== tick probe: ok\n" : "== tick probe: FAIL\n");
    FILE *f = fopen("probe-result.txt", "a");
    if (f) {
        fprintf(f, "GetTickCount %.0f ms, QPC %.0f ms\n%s", tick, qpc,
                ok ? "== tick probe: ok\n" : "== tick probe: FAIL\n");
        fclose(f);
    }
    return !ok;
}
