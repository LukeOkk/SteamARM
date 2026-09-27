// M6 probe: the runtime's vDSO. glibc must find it (AT_SYSINFO_EHDR), its
// clocks must agree with the real syscalls, stay monotonic, and be cheap.
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <unistd.h>

static int64_t ns(struct timespec t) { return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec; }

int main(void)
{
    int ok = 1;
    unsigned long ehdr = getauxval(AT_SYSINFO_EHDR);
    printf("AT_SYSINFO_EHDR %#lx\n", ehdr);
    if (!ehdr) ok = 0;
    int clocks[] = { CLOCK_MONOTONIC, CLOCK_REALTIME, CLOCK_MONOTONIC_RAW, CLOCK_BOOTTIME };
    for (int i = 0; i < 4; i++) {
        struct timespec a, b, c;
        syscall(SYS_clock_gettime, clocks[i], &a);
        clock_gettime(clocks[i], &b);
        syscall(SYS_clock_gettime, clocks[i], &c);
        int64_t d1 = ns(b) - ns(a), d2 = ns(c) - ns(b);
        printf("clock %d: vdso-sys %lld ns, sys-vdso %lld ns\n", clocks[i], (long long)d1, (long long)d2);
        if (d1 < -1000000 || d2 < -1000000 || d1 > 50000000 || d2 > 50000000) ok = 0;
    }
    struct timeval tv; struct timespec rt;
    gettimeofday(&tv, NULL);
    syscall(SYS_clock_gettime, CLOCK_REALTIME, &rt);
    int64_t dtv = ns(rt) - ((int64_t)tv.tv_sec * 1000000000 + tv.tv_usec * 1000);
    printf("gettimeofday vs syscall: %lld ns\n", (long long)dtv);
    if (dtv < -2000000 || dtv > 50000000) ok = 0;
    struct timespec prev, cur, t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &prev);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    const int N = 1000000;
    for (int i = 0; i < N; i++) {
        clock_gettime(CLOCK_MONOTONIC, &cur);
        if (ns(cur) < ns(prev)) { ok = 0; printf("went backwards\n"); break; }
        prev = cur;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double per = (double)(ns(t1) - ns(t0)) / N;
    printf("clock_gettime(MONOTONIC): %.1f ns/call\n", per);
    struct timespec r;
    clock_getres(CLOCK_MONOTONIC, &r);
    printf("getres %ld ns\n", r.tv_nsec);
    printf(ok ? "== vdso clock: ok\n" : "== vdso clock: MAL\n");
    return !ok;
}
