/* Priority-inheritance mutexes (FUTEX_LOCK_PI / TRYLOCK_PI / UNLOCK_PI).
   glibc probes the kernel once with UNLOCK_PI and, if it is missing, fails
   every pthread_mutex_init with PTHREAD_PRIO_INHERIT (ENOTSUP); PipeWire's
   event loop needs one, so without these no PipeWire client starts (wpctl
   died at 0x10). Checks: init succeeds; four threads x 200000 contended
   lock/unlock keep a counter exact; trylock on a held mutex is EBUSY;
   timedlock on a held mutex times out (ETIMEDOUT) near its deadline; an
   error-checking PI mutex relocked by its owner is EDEADLK; a process-shared
   PI mutex in shared memory serialises a parent and a forked child.
   Prints "== pi mutex: ok" or "MAL". */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static pthread_mutex_t m;
static long counter;
#define N 200000

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < N; i++) {
        pthread_mutex_lock(&m);
        counter++;
        pthread_mutex_unlock(&m);
    }
    return NULL;
}

static void *holder(void *arg)
{
    pthread_mutex_lock(&m);
    usleep(400000);
    pthread_mutex_unlock(&m);
    (void)arg;
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_setprotocol(&a, PTHREAD_PRIO_INHERIT);
    int r = pthread_mutex_init(&m, &a);
    if (r) { printf("pthread_mutex_init(PRIO_INHERIT) -> %s\n== pi mutex: MAL\n", strerror(r)); return 1; }

    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, worker, NULL);
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    if (counter != 4L * N) { printf("counter %ld, want %ld\n== pi mutex: MAL\n", counter, 4L * N); return 1; }
    printf("contended: %ld increments, exact\n", counter);

    pthread_t h; pthread_create(&h, NULL, holder, NULL);
    usleep(100000);
    r = pthread_mutex_trylock(&m);
    if (r != EBUSY) { printf("trylock on a held mutex -> %d, want EBUSY\n== pi mutex: MAL\n", r); return 1; }
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += 100000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
    struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
    r = pthread_mutex_timedlock(&m, &ts);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    if (r != ETIMEDOUT || ms < 90 || ms > 400) { printf("timedlock -> %d after %.0f ms, want ETIMEDOUT near 100\n== pi mutex: MAL\n", r, ms); return 1; }
    printf("timedlock: ETIMEDOUT after %.0f ms\n", ms);
    pthread_join(h, NULL);
    if ((r = pthread_mutex_trylock(&m)) != 0) { printf("trylock on a free mutex -> %d\n== pi mutex: MAL\n", r); return 1; }
    pthread_mutex_unlock(&m);

    pthread_mutex_t e;
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&e, &a);
    pthread_mutex_lock(&e);
    r = pthread_mutex_lock(&e);
    if (r != EDEADLK) { printf("relock of an error-checking PI mutex -> %d, want EDEADLK\n== pi mutex: MAL\n", r); return 1; }
    pthread_mutex_unlock(&e);

    struct shm { pthread_mutex_t m; long n; } *s = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    pthread_mutexattr_t sa;
    pthread_mutexattr_init(&sa);
    pthread_mutexattr_setprotocol(&sa, PTHREAD_PRIO_INHERIT);
    pthread_mutexattr_setpshared(&sa, PTHREAD_PROCESS_SHARED);
    if ((r = pthread_mutex_init(&s->m, &sa))) { printf("shared PI init -> %d\n== pi mutex: MAL\n", r); return 1; }
    pid_t c = fork();
    for (int i = 0; i < 50000; i++) { pthread_mutex_lock(&s->m); s->n++; pthread_mutex_unlock(&s->m); }
    if (c == 0) _exit(0);
    int st; waitpid(c, &st, 0);
    if (s->n != 100000) { printf("shared counter %ld, want 100000\n== pi mutex: MAL\n", s->n); return 1; }
    printf("process-shared: %ld increments, exact\n== pi mutex: ok\n", s->n);
    return 0;
}
