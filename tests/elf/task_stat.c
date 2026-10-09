// /proc/self/task/<tid>/stat as Wine's GetThreadTimes reads it: another
// thread's CPU time (fields 14 and 15, clock ticks) moves while it runs.
// Without the file, Wine reported zero for every thread but the caller.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static atomic_int tid, stop;

static void *spin(void *arg)
{
    (void)arg;
    atomic_store(&tid, (int)syscall(SYS_gettid));
    volatile unsigned long x = 0;
    while (!atomic_load(&stop)) x++;
    return NULL;
}

static int ticks(int t, unsigned long *usr, unsigned long *sys)
{
    char path[64], buf[512];
    snprintf(path, sizeof path, "/proc/%d/task/%d/stat", getpid(), t);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    char *p = fgets(buf, sizeof buf, f);
    fclose(f);
    if (p) p = strrchr(p, ')');
    for (int i = 0; i < 12 && p; i++) p = strchr(p + 1, ' ');
    return p && sscanf(p + 1, "%lu %lu", usr, sys) == 2 ? 0 : -2;
}

int main(void)
{
    pthread_t th;
    pthread_create(&th, NULL, spin, NULL);
    while (!atomic_load(&tid)) usleep(1000);
    unsigned long u0, s0, u1, s1;
    if (ticks(atomic_load(&tid), &u0, &s0)) { printf("FAIL no stat for tid %d\n", atomic_load(&tid)); return 1; }
    struct timespec d = { 0, 400 * 1000 * 1000 };
    nanosleep(&d, NULL);
    if (ticks(atomic_load(&tid), &u1, &s1)) { printf("FAIL stat vanished\n"); return 1; }
    atomic_store(&stop, 1);
    pthread_join(th, NULL);
    unsigned long used = (u1 + s1) - (u0 + s0);
    // Wine reads it on every GetThreadTimes; a protection loop does that
    // thousands of times a second.
    struct timespec a, z;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (int i = 0; i < 2000; i++) ticks(atomic_load(&tid), &u1, &s1);
    clock_gettime(CLOCK_MONOTONIC, &z);
    double us = ((z.tv_sec - a.tv_sec) * 1e9 + (z.tv_nsec - a.tv_nsec)) / 2000 / 1e3;
    printf("thread %d: %lu ticks in 0.4 s, %.0f us per read\n", atomic_load(&tid), used, us);
    // A spinning thread: close to 40 ticks; at least 20.
    if (used < 20 || used > 60 || us > 500) { printf("FAIL\n"); return 1; }
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%d/stat", atomic_load(&tid));
    usleep(100000);
    FILE *f = fopen(path, "r");
    if (f) { fclose(f); printf("FAIL exited thread still listed\n"); return 1; }
    printf("PASS\n");
    return 0;
}
