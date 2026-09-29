/* Does Darwin keep a value in x18 across preemption, signals and yields?
 *
 * xnu zeroes x18 on every return to user mode unless the task has
 * preserve_x18. machine_task_process_signature() (osfmk/arm64/machine_task.c,
 * public xnu main) sets it for two private entitlements and, as a "temporary
 * override for tasks before macOS 13", for any macOS binary whose
 * LC_BUILD_VERSION sdk is below 13.0. run.sh builds this program twice, with
 * the current SDK and with sdk 12.3, and runs both.
 *
 * If the sdk-12.3 build keeps x18, lxrun could let Linux code use x18 as a
 * normal register instead of rewriting every x18 instruction (runtime/x18.c),
 * and the ARM64 Windows ABI (TEB in x18) would have its register.
 *
 * Nothing here needs the guest runtime. Exit 0: preserved everywhere;
 * 1: zeroed or changed somewhere.
 */
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define MAGIC UINT64_C(0x5A18C0DE12345678)
#define SECONDS 3

static inline void set_x18(uint64_t v) { __asm__ volatile("mov x18, %0" ::"r"(v) : "x18"); }
static inline uint64_t get_x18(void) {
    uint64_t v;
    __asm__ volatile("mov %0, x18" : "=r"(v));
    return v;
}

static volatile sig_atomic_t ticks;
static void on_alarm(int sig) { (void)sig; ticks++; }

struct result { uint64_t seen; long loops; const char *what; };

/* Hold MAGIC in x18 and spin: the scheduler preempts us many times. */
static void *spin(void *arg) {
    struct result *r = arg;
    uint64_t want = MAGIC ^ (uint64_t)(uintptr_t)arg;
    struct timeval t0, t;
    gettimeofday(&t0, NULL);
    set_x18(want);
    long n = 0;
    do {
        for (volatile int i = 0; i < 100000; i++) { }
        n++;
        if (get_x18() != want) break;
        gettimeofday(&t, NULL);   /* libc; does not use x18 */
    } while (t.tv_sec - t0.tv_sec < SECONDS);
    r->seen = get_x18() ^ (uint64_t)(uintptr_t)arg;
    r->loops = n;
    return NULL;
}

static int check(const char *what, uint64_t seen) {
    int ok = seen == MAGIC;
    printf("  %-34s x18 = %#018llx  %s\n", what, (unsigned long long)seen,
           ok ? "PRESERVED" : seen == 0 ? "ZEROED" : "CHANGED");
    return ok;
}

int main(void) {
    int ok = 1;
    /* 1. preemption, one thread per core-ish */
    enum { N = 8 };
    pthread_t th[N];
    struct result res[N];
    memset(res, 0, sizeof res);
    for (int i = 0; i < N; i++) pthread_create(&th[i], NULL, spin, &res[i]);
    for (int i = 0; i < N; i++) pthread_join(th[i], NULL);
    for (int i = 0; i < N; i++) {
        char what[64];
        snprintf(what, sizeof what, "preemption, thread %d (%ld loops)", i, res[i].loops);
        ok &= check(what, res[i].seen);
    }
    /* 2. a signal delivered while x18 holds the value */
    signal(SIGALRM, on_alarm);
    struct itimerval it = {{0, 0}, {0, 200000}};
    set_x18(MAGIC);
    setitimer(ITIMER_REAL, &it, NULL);
    while (!ticks) { }
    ok &= check("after SIGALRM handler", get_x18());
    /* 3. voluntary yields and a blocking syscall */
    set_x18(MAGIC);
    for (int i = 0; i < 1000; i++) sched_yield();
    ok &= check("after 1000 sched_yield", get_x18());
    set_x18(MAGIC);
    usleep(300000);
    ok &= check("after usleep(300 ms)", get_x18());
    printf("  => %s\n", ok ? "x18 is preserved for this binary" : "x18 is NOT preserved for this binary");
    return ok ? 0 : 1;
}
