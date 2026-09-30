#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifdef __aarch64__
_Static_assert(SYS_timer_create == 107 && SYS_timer_gettime == 108 &&
               SYS_timer_getoverrun == 109 && SYS_timer_settime == 110 &&
               SYS_timer_delete == 111, "aarch64 timer syscall ABI");
#endif
static int ok, mal;
static volatile sig_atomic_t usr1_hits, alrm_hits;
static volatile int64_t first_usr1_ns;
#define CHECK(name, expr) do { if (expr) { ++ok; printf("  OK  %s\n", name); } \
    else { ++mal; printf("MAL %s (errno=%d)\n", name, errno); } } while (0)

static void caught(int sig)
{
    if (sig == SIGUSR1) {
        if (!usr1_hits) {
            struct timespec t;
            clock_gettime(CLOCK_MONOTONIC, &t);
            first_usr1_ns = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
        }
        usr1_hits++;
    }
    if (sig == SIGALRM) alrm_hits++;
}
static int64_t mono_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static void wait_ms(int ms)
{
    int64_t end = mono_ns() + (int64_t)ms * 1000000;
    while (mono_ns() < end) {
        int64_t left = end - mono_ns();
        if (left <= 0) break;
        struct timespec t = {left / 1000000000, left % 1000000000};
        nanosleep(&t, NULL);
    }
}
static long arm(int id, int flags, int64_t first_ns, int64_t interval_ns,
                struct itimerspec *old)
{
    struct itimerspec t = {
        .it_interval = {interval_ns / 1000000000, interval_ns % 1000000000},
        .it_value = {first_ns / 1000000000, first_ns % 1000000000},
    };
    return syscall(SYS_timer_settime, id, flags, &t, old);
}
static long create(int clock, struct sigevent *event, int *id)
{
    return syscall(SYS_timer_create, clock, event, id);
}

int main(void)
{
    struct sigaction sa = {.sa_handler = caught};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);

    struct sigevent thread = {.sigev_notify = SIGEV_THREAD_ID,
                              .sigev_signo = SIGUSR1};
    thread._sigev_un._tid = (int)syscall(SYS_gettid);
    struct sigevent none = {.sigev_notify = SIGEV_NONE};
    int once = -1, periodic = -1, quiet = -1;
    CHECK("create SIGEV_THREAD_ID", create(CLOCK_MONOTONIC, &thread, &once) == 0);
    CHECK("create periodic", create(CLOCK_MONOTONIC, &thread, &periodic) == 0);
    CHECK("create SIGEV_NONE", create(CLOCK_MONOTONIC, &none, &quiet) == 0);
    CHECK("first IDs 0,1,2", once == 0 && periodic == 1 && quiet == 2);

    int64_t began = mono_ns();
    CHECK("one-shot 50ms arm", arm(once, 0, 50000000, 0, NULL) == 0);
    struct itimerspec cur;
    CHECK("gettime armed", syscall(SYS_timer_gettime, once, &cur) == 0 &&
          cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0 &&
          cur.it_value.tv_nsec <= 50000000 && cur.it_interval.tv_sec == 0 &&
          cur.it_interval.tv_nsec == 0);
    wait_ms(90);
    CHECK("SIGEV_THREAD_ID fires once near 50ms",
          usr1_hits == 1 && first_usr1_ns - began >= 30000000 &&
          first_usr1_ns - began <= 150000000);
    CHECK("gettime disarmed after one-shot", syscall(SYS_timer_gettime, once, &cur) == 0 &&
          cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec == 0);

    int before = usr1_hits;
    CHECK("periodic 20ms arm", arm(periodic, 0, 20000000, 20000000, NULL) == 0);
    wait_ms(110);
    CHECK("periodic fires at least three times", usr1_hits - before >= 3);
    CHECK("gettime interval", syscall(SYS_timer_gettime, periodic, &cur) == 0 &&
          cur.it_interval.tv_sec == 0 && cur.it_interval.tv_nsec == 20000000 &&
          cur.it_value.tv_sec == 0 && cur.it_value.tv_nsec > 0 &&
          cur.it_value.tv_nsec <= 20000000);
    CHECK("getoverrun valid", syscall(SYS_timer_getoverrun, periodic) >= 0);
    struct itimerspec old;
    CHECK("settime returns old value", arm(periodic, 0, 200000000, 0, &old) == 0 &&
          old.it_interval.tv_nsec == 20000000 && old.it_value.tv_nsec > 0 &&
          old.it_value.tv_nsec <= 20000000);
    CHECK("disarm periodic", arm(periodic, 0, 0, 0, NULL) == 0);

    before = usr1_hits;
    CHECK("SIGEV_NONE arm", arm(quiet, 0, 120000000, 0, NULL) == 0);
    struct itimerspec q1, q2;
    int got1 = syscall(SYS_timer_gettime, quiet, &q1) == 0;
    wait_ms(30);
    int got2 = syscall(SYS_timer_gettime, quiet, &q2) == 0;
    CHECK("SIGEV_NONE counts down", got1 && got2 && q1.it_value.tv_nsec >
          q2.it_value.tv_nsec && q2.it_value.tv_nsec > 0);
    wait_ms(110);
    CHECK("SIGEV_NONE sends no signal", usr1_hits == before);

    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    int64_t abs_ns = (int64_t)deadline.tv_sec * 1000000000 + deadline.tv_nsec + 50000000;
    before = usr1_hits;
    CHECK("TIMER_ABSTIME arm", arm(once, 1, abs_ns, 0, NULL) == 0);
    wait_ms(90);
    CHECK("TIMER_ABSTIME fires", usr1_hits == before + 1);

    before = usr1_hits;
    CHECK("arm before delete", arm(once, 0, 80000000, 0, NULL) == 0);
    CHECK("delete armed timer", syscall(SYS_timer_delete, once) == 0);
    wait_ms(110);
    CHECK("delete prevents signal", usr1_hits == before);
    CHECK("delete unknown ID EINVAL", syscall(SYS_timer_delete, 123456) == -1 &&
          errno == EINVAL);
    CHECK("invalid clock EINVAL", create(123456, &thread, &once) == -1 &&
          errno == EINVAL);
    struct sigevent bad = {.sigev_notify = 99, .sigev_signo = SIGUSR1};
    CHECK("bad notify EINVAL", create(CLOCK_MONOTONIC, &bad, &once) == -1 &&
          errno == EINVAL);
    bad = (struct sigevent){.sigev_notify = SIGEV_SIGNAL, .sigev_signo = 0};
    CHECK("bad signo EINVAL", create(CLOCK_MONOTONIC, &bad, &once) == -1 &&
          errno == EINVAL);
    bad = thread;
    bad._sigev_un._tid = 123456789;
    CHECK("foreign thread ID EINVAL", create(CLOCK_MONOTONIC, &bad, &once) == -1 &&
          errno == EINVAL);
    CHECK("NULL timerid pointer EFAULT", create(CLOCK_MONOTONIC, &thread, NULL) == -1 &&
          errno == EFAULT);
    CHECK("CPU clock EINVAL", create(CLOCK_PROCESS_CPUTIME_ID, &thread, &once) == -1 &&
          errno == EINVAL);
    CHECK("bad settime flags EINVAL", arm(periodic, 2, 1000000, 0, NULL) == -1 &&
          errno == EINVAL);
    struct itimerspec invalid = {.it_value = {0, 1000000000}};
    CHECK("bad nanoseconds EINVAL", syscall(SYS_timer_settime, periodic, 0,
          &invalid, NULL) == -1 && errno == EINVAL);
    CHECK("NULL gettime pointer EFAULT", syscall(SYS_timer_gettime, periodic,
          NULL) == -1 && errno == EFAULT);

    struct sigevent signal_event = {.sigev_notify = SIGEV_SIGNAL,
                                    .sigev_signo = SIGUSR1};
    int signal_id = -1;
    CHECK("create SIGEV_SIGNAL", create(CLOCK_MONOTONIC, &signal_event,
          &signal_id) == 0);
    before = usr1_hits;
    CHECK("arm SIGEV_SIGNAL", arm(signal_id, 0, 30000000, 0, NULL) == 0);
    wait_ms(70);
    CHECK("SIGEV_SIGNAL fires", usr1_hits == before + 1);
    syscall(SYS_timer_delete, signal_id);
    signal_event.sigev_notify = SIGEV_THREAD;
    CHECK("create raw SIGEV_THREAD", create(CLOCK_MONOTONIC, &signal_event,
          &signal_id) == 0);
    before = usr1_hits;
    CHECK("arm raw SIGEV_THREAD", arm(signal_id, 0, 30000000, 0, NULL) == 0);
    wait_ms(70);
    CHECK("raw SIGEV_THREAD fires", usr1_hits == before + 1);
    syscall(SYS_timer_delete, signal_id);

    int default_id = -1;
    CHECK("NULL sevp creates SIGALRM timer", create(CLOCK_MONOTONIC, NULL, &default_id) == 0);
    CHECK("lowest ID reused", default_id == 0);
    before = alrm_hits;
    CHECK("NULL sevp arm", arm(default_id, 0, 40000000, 0, NULL) == 0);
    wait_ms(80);
    CHECK("NULL sevp fires SIGALRM", alrm_hits == before + 1);

    pid_t child = fork();
    if (child == 0) {
        int rc = syscall(SYS_timer_gettime, periodic, &cur);
        _exit(rc == -1 && errno == EINVAL ? 0 : 1);
    }
    int status = 0;
    int waited = child > 0 ? waitpid(child, &status, 0) : -1;
    CHECK("fork child has no timers", waited == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);

    syscall(SYS_timer_delete, default_id);
    syscall(SYS_timer_delete, periodic);
    syscall(SYS_timer_delete, quiet);
    printf("== %d ok, %d mal\n", ok, mal);
    return mal ? 1 : 0;
}
