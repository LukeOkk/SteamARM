#include "lxrt.h"
#include "android_ids.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))
#define NS 1000000000LL
typedef __int128 ticks;
struct ltime { int64_t sec, nsec; };
struct lspec { struct ltime interval, value; };
_Static_assert(sizeof(struct lspec) == 32, "Linux itimerspec ABI");

struct timer {
    int clock, notify, signo, tid;
    uint64_t sigval;
    ticks next, interval;
    int overrun;
    bool delivering;
};
static struct timer **timers;
static size_t capacity;
static bool worker_started;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
LXRT_FORK_SAFE(posixtimer_lock, lock)

// pthread_atfork runs child handlers in registration order. The lock handler
// above releases first; the vanished helper's wait queue must then be reset.
static void child_reset(void)
{
    for (size_t i = 0; i < capacity; i++) free(timers[i]);
    free(timers);
    timers = NULL;
    capacity = 0;
    worker_started = false;
    pthread_cond_t fresh = PTHREAD_COND_INITIALIZER;
    changed = fresh;
}
__attribute__((constructor(201))) static void register_child_reset(void)
{
    pthread_atfork(NULL, NULL, child_reset);
}

static ticks now(int clock) { return (ticks)lxrt_guest_clock_ns(clock); }
static ticks to_ticks(struct ltime t) { return (ticks)t.sec * NS + t.nsec; }
static struct ltime to_time(ticks t)
{
    return (struct ltime){(int64_t)(t / NS), (int64_t)(t % NS)};
}
static bool valid(struct ltime t) { return t.sec >= 0 && t.nsec >= 0 && t.nsec < NS; }
static struct timer *lookup(int id)
{
    return id >= 0 && (size_t)id < capacity ? timers[id] : NULL;
}
static struct lspec snapshot(struct timer *t)
{
    ticks left = t->next ? t->next - now(t->clock) : 0;
    if (left <= 0 && t->next && t->interval)
        left += ((-left) / t->interval + 1) * t->interval;
    return (struct lspec){to_time(t->interval), to_time(left > 0 ? left : 0)};
}

static void *timer_worker(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&lock);
    for (;;) {
        struct timer *due = NULL;
        ticks shortest = 0;
        for (size_t i = 0; i < capacity; i++) {
            struct timer *t = timers[i];
            if (!t || !t->next) continue;
            ticks delay = t->next - now(t->clock);
            if (delay <= 0) { due = t; break; }
            if (!shortest || delay < shortest) shortest = delay;
        }
        if (!due) {
            if (!shortest) pthread_cond_wait(&changed, &lock);
            else {
                // Bound host wait: guest REALTIME and host REALTIME can jump.
                if (shortest > NS) shortest = NS;
                struct timespec until;
                clock_gettime(CLOCK_REALTIME, &until);
                ticks n = (ticks)until.tv_nsec + shortest;
                until.tv_sec += (time_t)(n / NS);
                until.tv_nsec = (long)(n % NS);
                pthread_cond_timedwait(&changed, &lock, &until);
            }
            continue;
        }

        ticks count = due->interval ? 1 + (now(due->clock) - due->next) / due->interval : 1;
        if (due->interval) due->next += count * due->interval;
        else due->next = 0;
        due->overrun = count - 1 > INT_MAX ? INT_MAX : (int)(count - 1);
        due->delivering = true;
        int notify = due->notify, signo = due->signo, tid = due->tid;
        pthread_mutex_unlock(&lock);
        if (notify == 4) {
            long rc = lxrt_tgkill(0, tid, signo);
            if (rc == LERR(ESRCH)) lxrt_kill(getpid(), signo);
        } else if (notify == 0 || notify == 2) {
            lxrt_kill(getpid(), signo);
        }
        pthread_mutex_lock(&lock);
        due->delivering = false;
        pthread_cond_broadcast(&changed);
    }
    return NULL;
}

long lxrt_posix_timer_create(int clockid, const void *event, int *id_out)
{
    // CPU-time clocks (2, 3, and negative clock IDs) are not implemented.
    if (clockid != 0 && clockid != 1 && clockid != 7 && clockid != 8 && clockid != 9)
        return LERR(EINVAL);
    if ((clockid == 8 || clockid == 9) && !lxrt_aids_capable(35))
        return LERR(EPERM);
    if (!id_out) return LERR(EFAULT);
    int notify = 0, signo = 14, tid = 0;
    uint64_t sigval = 0;
    if (event) {
        // Linux's 64-bit sigevent has an 8-byte sigval, two ints, then a
        // 48-byte union whose first int is sigev_notify_thread_id.
        unsigned char raw[64];
        memcpy(raw, event, sizeof(raw));
        memcpy(&sigval, raw, sizeof(sigval));
        memcpy(&signo, raw + 8, sizeof(signo));
        memcpy(&notify, raw + 12, sizeof(notify));
        memcpy(&tid, raw + 16, sizeof(tid));
        if (notify != 0 && notify != 1 && notify != 2 && notify != 4)
            return LERR(EINVAL);
        if (notify != 1 && (signo < 1 || signo > 64)) return LERR(EINVAL);
        pthread_t target;
        if (notify == 4 && !lxrt_thread_lookup(tid, &target)) return LERR(EINVAL);
    }
    pthread_mutex_lock(&lock);
    size_t id = 0;
    while (id < capacity && timers[id]) id++;
    if (id == INT_MAX) { pthread_mutex_unlock(&lock); return LERR(EAGAIN); }
    if (id == capacity) {
        size_t next = capacity ? capacity * 2 : 8;
        if (next > INT_MAX) next = INT_MAX;
        struct timer **grown = realloc(timers, next * sizeof(*timers));
        if (!grown) { pthread_mutex_unlock(&lock); return LERR(ENOMEM); }
        memset(grown + capacity, 0, (next - capacity) * sizeof(*grown));
        timers = grown;
        capacity = next;
    }
    struct timer *t = calloc(1, sizeof(*t));
    if (!t) { pthread_mutex_unlock(&lock); return LERR(ENOMEM); }
    if (!event) sigval = (uint32_t)id;
    *t = (struct timer){.clock = clockid, .notify = notify, .signo = signo,
                        .tid = tid, .sigval = sigval};
    timers[id] = t;
    int result = (int)id;
    memcpy(id_out, &result, sizeof(result));
    pthread_mutex_unlock(&lock);
    return 0;
}

long lxrt_posix_timer_gettime(int id, void *out)
{
    if (!out) return LERR(EFAULT);
    pthread_mutex_lock(&lock);
    struct timer *t = lookup(id);
    if (!t) { pthread_mutex_unlock(&lock); return LERR(EINVAL); }
    struct lspec spec = snapshot(t);
    memcpy(out, &spec, sizeof(spec));
    pthread_mutex_unlock(&lock);
    return 0;
}

long lxrt_posix_timer_getoverrun(int id)
{
    pthread_mutex_lock(&lock);
    struct timer *t = lookup(id);
    long result = t ? t->overrun : LERR(EINVAL);
    pthread_mutex_unlock(&lock);
    return result;
}

long lxrt_posix_timer_settime(int id, int flags, const void *in, void *old)
{
    if (flags & ~1) return LERR(EINVAL);
    if (!in) return LERR(EFAULT);
    struct lspec spec;
    memcpy(&spec, in, sizeof(spec));
    if (!valid(spec.interval) || !valid(spec.value)) return LERR(EINVAL);
    pthread_mutex_lock(&lock);
    struct timer *t = lookup(id);
    if (!t) { pthread_mutex_unlock(&lock); return LERR(EINVAL); }
    if ((spec.value.sec || spec.value.nsec) && !worker_started) {
        // No guest signal may be delivered on this host thread: it starts
        // with every signal blocked (as shmirror.c's thread does), else
        // Darwin could hand it a process-directed signal meant for a guest.
        pthread_t worker;
        sigset_t all, previous_mask;
        sigfillset(&all);
        pthread_sigmask(SIG_BLOCK, &all, &previous_mask);
        int rc = pthread_create(&worker, NULL, timer_worker, NULL);
        pthread_sigmask(SIG_SETMASK, &previous_mask, NULL);
        if (rc) { pthread_mutex_unlock(&lock); return LERR(rc); }
        pthread_detach(worker);
        worker_started = true;
    }
    struct lspec previous = snapshot(t);
    t->interval = to_ticks(spec.interval);
    t->next = to_ticks(spec.value);
    if (t->next && !(flags & 1)) t->next += now(t->clock);
    if (old) memcpy(old, &previous, sizeof(previous));
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&lock);
    return 0;
}

long lxrt_posix_timer_delete(int id)
{
    pthread_mutex_lock(&lock);
    struct timer *t = lookup(id);
    if (!t) { pthread_mutex_unlock(&lock); return LERR(EINVAL); }
    t->next = 0;
    pthread_cond_broadcast(&changed);
    while (t->delivering) pthread_cond_wait(&changed, &lock);
    timers[id] = NULL;
    free(t);
    pthread_mutex_unlock(&lock);
    return 0;
}
