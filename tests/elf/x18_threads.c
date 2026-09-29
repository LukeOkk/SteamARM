// X18_THREAD_ISOLATION: nine values survive concurrent raw syscalls and signal return.
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

static pthread_barrier_t barrier;
static unsigned long bad[9];
static pthread_t threads[8];
static volatile sig_atomic_t done[9];
static volatile sig_atomic_t signals;

static void handler(int sig)
{
    (void)sig;
    signals++;
    __asm__ volatile("mov x18, #0xdead" ::: "x18", "memory");
}

static void run_loop(int id)
{
    unsigned long value = 0x1800000000UL + (unsigned long)id;
    unsigned long errors = 0;
    __asm__ volatile(
        "mov x18, %[value]\n"
        "mov x10, #20000\n"
        "1: mov x8, #124\n"
        "svc #0\n"
        "mov x11, #2000\n"
        "2: subs x11, x11, #1\n"
        "b.ne 2b\n"
        "cmp x18, %[value]\n"
        "cinc %[errors], %[errors], ne\n"
        "mov x18, %[value]\n"
        "subs x10, x10, #1\n"
        "b.ne 1b\n"
        : [errors] "+r"(errors) : [value] "r"(value)
        : "x0", "x8", "x10", "x11", "x18", "cc", "memory"); // svc returns in x0
    bad[id] = errors;
    done[id] = 1;
}

static void *worker(void *arg)
{
    pthread_barrier_wait(&barrier);
    run_loop((int)(uintptr_t)arg);
    return NULL;
}

int main(void)
{
    struct sigaction sa = {.sa_handler = handler};
    sigemptyset(&sa.sa_mask);
    int good = sigaction(SIGUSR1, &sa, NULL) == 0 && pthread_barrier_init(&barrier, NULL, 9) == 0;
    for (int i = 0; good && i < 8; i++)
        good = pthread_create(&threads[i], NULL, worker, (void *)(uintptr_t)(i + 1)) == 0;
    if (!good) {
        puts("  FAIL  pthread setup");
        puts("== x18 threads: FAIL");
        return 1;
    }
    pthread_barrier_wait(&barrier);
    for (int i = 0; i < 8; i++) {
        good &= pthread_kill(threads[3], SIGUSR1) == 0;
        sched_yield();
    }
    run_loop(0);
    for (int i = 0; i < 8; i++) pthread_join(threads[i], NULL);
    pthread_barrier_destroy(&barrier);
    printf("  %s  SIGUSR1 delivered %d times\n", signals ? "ok  " : "FAIL", signals);
    good &= signals > 0;
    for (int i = 0; i < 9; i++) {
        printf("thread %d: bad=%lu\n", i, bad[i]);
        printf("  %s  thread %d x18\n", bad[i] ? "FAIL" : "ok  ", i);
        good &= bad[i] == 0 && done[i];
    }
    puts(good ? "== x18 threads: ok" : "== x18 threads: FAIL");
    return !good;
}
