#include "timerfd_signalfd.h"
#include "lxrt.h"
#include "android_ids.h"
#include <sys/event.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))
#define NONBLOCK 0x800
#define CLOEXEC 0x80000
#define NS 1000000000LL
struct ltime { int64_t sec, nsec; };
struct lspec { struct ltime interval, value; };
_Static_assert(sizeof(struct lspec) == 32, "itimerspec ABI");
/* Wide arithmetic preserves the full signed-64-bit seconds input ABI. */
typedef __int128 ticks;
struct state {
    unsigned refs;
    int kind, flags;
    clockid_t clock;
    ticks next, interval;
    uint64_t mask;
};
struct alias { int fd; struct state *s; struct alias *next; };
static struct alias *aliases;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(timerfd_signalfd_lock, lock)
static struct state *lookup(int fd, int kind)
{
    for (struct alias *a = aliases; a; a = a->next)
        if (a->fd == fd && a->s->kind == kind) return a->s;
    return NULL;
}
static long badfd(int fd) { return LERR(fcntl(fd, F_GETFD) < 0 ? EBADF : EINVAL); }
static bool is_kind(int fd, int kind)
{
    pthread_mutex_lock(&lock);
    bool yes = lookup(fd, kind) != NULL;
    pthread_mutex_unlock(&lock);
    return yes;
}
bool lxrt_timerfd_is(int fd) { return is_kind(fd, 1); }
bool lxrt_signalfd_is(int fd) { return is_kind(fd, 2); }
static void drop(int fd, int kind)
{
    struct alias **p = &aliases;
    while (*p) {
        struct alias *a = *p;
        if (a->fd == fd && a->s->kind == kind) {
            *p = a->next;
            if (!--a->s->refs) free(a->s);
            free(a);
            return;
        }
        p = &a->next;
    }
}
static void close_kind(int fd, int kind)
{
    pthread_mutex_lock(&lock); drop(fd, kind); pthread_mutex_unlock(&lock);
}
void lxrt_timerfd_close(int fd) { close_kind(fd, 1); }
void lxrt_signalfd_close(int fd) { close_kind(fd, 2); }
static void dup_kind(int oldfd, int newfd, int kind)
{
    if (oldfd == newfd) return;
    pthread_mutex_lock(&lock);
    struct state *s = lookup(oldfd, kind);
    drop(newfd, kind);
    if (s) {
        struct alias *a = malloc(sizeof(*a));
        /* The void hook cannot report allocation failure. No fixed alias cap. */
        if (a) { *a = (struct alias){newfd, s, aliases}; aliases = a; s->refs++; }
    }
    pthread_mutex_unlock(&lock);
}
void lxrt_timerfd_dup(int a, int b) { dup_kind(a, b, 1); }
void lxrt_signalfd_dup(int a, int b) { dup_kind(a, b, 2); }
static int create(int kind, int flags, clockid_t clock)
{
    struct state *s = calloc(1, sizeof(*s));
    struct alias *a = malloc(sizeof(*a));
    if (!s || !a) { free(s); free(a); return LERR(ENOMEM); }
    int fd = kqueue();
    if (fd < 0) { int e = errno; free(s); free(a); return LERR(e); }
    if (fcntl(fd, F_SETFD, flags & CLOEXEC ? FD_CLOEXEC : 0) < 0) {
        int e = errno; close(fd); free(s); free(a); return LERR(e);
    }
    /* kqueue does not support F_SETFL(O_NONBLOCK); keep the flag in state. */
    s->refs = 1; s->kind = kind; s->flags = flags; s->clock = clock;
    *a = (struct alias){fd, s, aliases}; aliases = a;
    return fd;
}
// The same counter the guest reads through clock_gettime (dispatch.c): an
// absolute deadline the guest computed from ITS clock is only meaningful
// against that clock. Darwin's CLOCK_MONOTONIC is a different base (measured
// 3.8 s apart here) and ABSTIME never fired.
static ticks now(clockid_t clock)
{
    return (ticks)lxrt_guest_clock_ns(clock == CLOCK_REALTIME ? 0 : 1);
}
static ticks to_ticks(struct ltime t) { return (ticks)t.sec * NS + t.nsec; }
static struct ltime to_time(ticks t)
{
    return (struct ltime){(int64_t)(t / NS), (int64_t)(t % NS)};
}
static bool valid(struct ltime t) { return t.sec >= 0 && t.nsec >= 0 && t.nsec < NS; }
static struct lspec snapshot(struct state *s)
{
    ticks left = s->next ? s->next - now(s->clock) : 0;
    if (left <= 0 && s->next && s->interval)
        left += ((-left) / s->interval + 1) * s->interval;
    return (struct lspec){to_time(s->interval), to_time(left > 0 ? left : 0)};
}
static int arm(int fd, struct state *s)
{
    struct kevent e;
    ticks delay = s->next - now(s->clock);
    if (delay < 1) delay = 1;
    if (delay > INT64_MAX) delay = INT64_MAX;
    EV_SET(&e, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, NOTE_NSECONDS,
           (intptr_t)delay, NULL);
    return kevent(fd, &e, 1, NULL, 0, NULL);
}
long lxrt_timerfd_create(int clockid, int flags)
{
    // CLOCK_REALTIME 0, MONOTONIC 1, BOOTTIME 7, REALTIME_ALARM 8,
    // BOOTTIME_ALARM 9. The alarm clocks are their base clocks that would
    // also wake a suspended machine; a Mac guest cannot suspend anything, so
    // they are the base clocks here. Linux wants CAP_WAKE_ALARM (35) for
    // them (fs/timerfd.c): Android's AlarmManagerService makes one of each
    // in system_server, which has it, and dies without them ("kernel does
    // not support timerfd_create() with alarm timers", MEASURED at stage 28).
    if ((flags & ~(NONBLOCK | CLOEXEC)) ||
        (clockid != 0 && clockid != 1 && clockid != 7 && clockid != 8 && clockid != 9))
        return LERR(EINVAL);
    if ((clockid == 8 || clockid == 9) && !lxrt_aids_capable(35))
        return LERR(EPERM);
    pthread_mutex_lock(&lock);
    int fd = create(1, flags, clockid == 0 || clockid == 8 ? CLOCK_REALTIME : CLOCK_MONOTONIC);
    pthread_mutex_unlock(&lock); return fd;
}
long lxrt_timerfd_settime(int fd, int flags, const void *in, void *out)
{
    // TFD_TIMER_ABSTIME 1, TFD_TIMER_CANCEL_ON_SET 2 (Linux's
    // TFD_SETTIME_FLAGS). CANCEL_ON_SET asks for ECANCELED when the wall
    // clock is set; it is accepted and never fires: the runtime sees no
    // clock change (AlarmManagerService arms one and dies if the call fails).
    if (flags & ~3) return LERR(EINVAL);
    if (!in) return LERR(EFAULT);
    struct lspec spec; memcpy(&spec, in, sizeof(spec));
    if (!valid(spec.interval) || !valid(spec.value)) return LERR(EINVAL);
    pthread_mutex_lock(&lock);
    struct state *s = lookup(fd, 1);
    if (!s) { pthread_mutex_unlock(&lock); return badfd(fd); }
    struct lspec old = snapshot(s);
    struct state replacement = *s;
    replacement.interval = to_ticks(spec.interval);
    replacement.next = to_ticks(spec.value);
    if (replacement.next && !(flags & 1)) replacement.next += now(s->clock);
    int rc;
    if (replacement.next) rc = arm(fd, &replacement);
    else {
        struct kevent e; EV_SET(&e, 1, EVFILT_TIMER, EV_DELETE, 0, 0, NULL);
        rc = kevent(fd, &e, 1, NULL, 0, NULL);
        if (rc < 0 && errno == ENOENT) rc = 0;
    }
    long result = rc < 0 ? LERR(errno) : 0;
    if (!result) { *s = replacement; if (out) memcpy(out, &old, sizeof(old)); }
    pthread_mutex_unlock(&lock); return result;
}
long lxrt_timerfd_gettime(int fd, void *out)
{
    if (!out) return LERR(EFAULT);
    pthread_mutex_lock(&lock);
    struct state *s = lookup(fd, 1);
    if (!s) { pthread_mutex_unlock(&lock); return badfd(fd); }
    struct lspec spec = snapshot(s); memcpy(out, &spec, sizeof(spec));
    pthread_mutex_unlock(&lock); return 0;
}
/* Never hold the registry lock across a blocking wait. Recheck state on wake. */
static int wait_readable(int fd, bool nonblock)
{
    struct pollfd p = {fd, POLLIN, 0};
    int rc = poll(&p, 1, nonblock ? 0 : -1);
    if (rc > 0 && (p.revents & POLLNVAL)) { errno = EBADF; return -1; }
    return rc;
}
long lxrt_timerfd_read(int fd, void *buf, size_t len)
{
    if (len < 8) return LERR(EINVAL);
    if (!buf) return LERR(EFAULT);
    for (;;) {
        pthread_mutex_lock(&lock);
        struct state *s = lookup(fd, 1);
        if (!s) { pthread_mutex_unlock(&lock); return badfd(fd); }
        bool nb = s->flags & NONBLOCK;
        struct kevent e; struct timespec zero = {0, 0};
        int n = kevent(fd, NULL, 0, &e, 1, &zero);
        long error = n < 0 ? LERR(errno) : 0;
        ticks t = now(s->clock);
        if (n > 0 && s->next && t >= s->next) {
            ticks count = s->interval ? 1 + (t - s->next) / s->interval : 1;
            uint64_t value = count > UINT64_MAX ? UINT64_MAX : (uint64_t)count;
            if (s->interval) s->next += count * s->interval;
            else s->next = 0;
            if (s->next && arm(fd, s) < 0) error = LERR(errno);
            if (!error) memcpy(buf, &value, 8);
            pthread_mutex_unlock(&lock); return error ? error : 8;
        }
        /* An extremely distant timer is chunked at INT64_MAX nanoseconds. */
        if (n > 0 && s->next && arm(fd, s) < 0) error = LERR(errno);
        pthread_mutex_unlock(&lock);
        if (error) return error;
        if (nb) return LERR(EAGAIN);
        if (wait_readable(fd, false) < 0) return LERR(errno);
    }
}
static int signal_changes(int fd, uint64_t before, uint64_t after)
{
    for (int l = 1; l <= 64; l++) {
        uint64_t bit = UINT64_C(1) << (l - 1);
        int d = lxrt_signo_to_darwin(l);
        if (!d || !((before ^ after) & bit)) continue;
        struct kevent e;
        EV_SET(&e, d, EVFILT_SIGNAL, after & bit ? EV_ADD | EV_CLEAR : EV_DELETE, 0, 0, NULL);
        if (kevent(fd, &e, 1, NULL, 0, NULL) < 0) return -1;
    }
    return 0;
}
/* Preserve level readiness when several standard signals are pending, and
 * expose signals that were already pending when the descriptor was created. */
static void signal_ready(int fd, uint64_t mask)
{
    sigset_t p;
    if (sigpending(&p) < 0) return;
    for (int l = 1; l <= 64; l++) {
        int d = lxrt_signo_to_darwin(l);
        if (d && (mask & (UINT64_C(1) << (l - 1))) && sigismember(&p, d)) {
            struct kevent e;
            EV_SET(&e, 2, EVFILT_USER, EV_ADD | EV_CLEAR, NOTE_TRIGGER, 0, NULL);
            (void)kevent(fd, &e, 1, NULL, 0, NULL);
            return;
        }
    }
}
// Called by signal.c after it sends a guest signal with pthread_kill().
// kqueue's EVFILT_SIGNAL fires only for process-directed delivery (measured:
// kill(getpid()) -> inner poll=1, pthread_kill(self) -> 0), and the runtime
// delivers self-directed signals thread-directed on purpose, so a signalfd
// watched through epoll never became readable although read() found the
// signal pending. This is the wake-up Linux performs at enqueue time.
void lxrt_signalfd_notify(int lsig)
{
    if (lsig < 1 || lsig > 64) return;
    uint64_t bit = UINT64_C(1) << (lsig - 1);
    pthread_mutex_lock(&lock);
    for (struct alias *a = aliases; a; a = a->next) {
        if (a->s->kind != 2 || !(a->s->mask & bit)) continue;
        struct kevent e;
        EV_SET(&e, 2, EVFILT_USER, EV_ADD | EV_CLEAR, NOTE_TRIGGER, 0, NULL);
        (void)kevent(a->fd, &e, 1, NULL, 0, NULL);
    }
    pthread_mutex_unlock(&lock);
}

long lxrt_signalfd4(int fd, const uint64_t *mask, size_t size, int flags)
{
    if (size != 8 || (flags & ~(NONBLOCK | CLOEXEC))) return LERR(EINVAL);
    if (!mask) return LERR(EFAULT);
    uint64_t m; memcpy(&m, mask, 8);
    m &= ~((UINT64_C(1) << 8) | (UINT64_C(1) << 18));
    pthread_mutex_lock(&lock);
    bool fresh = fd == -1;
    if (fresh) fd = create(2, flags, CLOCK_MONOTONIC);
    if (fd < 0 && fresh) { pthread_mutex_unlock(&lock); return fd; }
    struct state *s = lookup(fd, 2);
    if (!s) { pthread_mutex_unlock(&lock); return badfd(fd); }
    if (signal_changes(fd, s->mask, m) < 0) {
        int e = errno;
        /* Restore all registrations, including a partially applied update. */
        signal_changes(fd, m | s->mask, 0);
        signal_changes(fd, 0, s->mask);
        if (fresh) { drop(fd, 2); close(fd); }
        pthread_mutex_unlock(&lock); return LERR(e);
    }
    s->mask = m;
    signal_ready(fd, m);
    pthread_mutex_unlock(&lock); return fd;
}
__attribute__((weak))
int lxrt_signal_dequeue_pending(uint64_t mask, struct signalfd_siginfo *out)
{
    memset(out, 0, sizeof(*out));
    sigset_t p, blocked;
    if (sigpending(&p) < 0) return LERR(errno);
    int rc = pthread_sigmask(SIG_BLOCK, NULL, &blocked);
    if (rc) return LERR(rc);
    for (int l = 1; l <= 64; l++) {
        int d = lxrt_signo_to_darwin(l);
        if (!(mask & (UINT64_C(1) << (l - 1))) || !d ||
            !sigismember(&p, d) || !sigismember(&blocked, d)) continue;
        sigset_t one; sigemptyset(&one); sigaddset(&one, d);
        int got; rc = sigwait(&one, &got);
        if (rc) return LERR(rc);
        out->ssi_signo = (uint32_t)l;
        out->ssi_code = 0x80; /* SI_KERNEL: Darwin sigwait has no siginfo. */
        return 1;
    }
    return 0;
}
long lxrt_signalfd_read(int fd, void *buf, size_t len)
{
    if (len < sizeof(struct signalfd_siginfo)) return LERR(EINVAL);
    if (!buf) return LERR(EFAULT);
    size_t used = 0;
    for (;;) {
        pthread_mutex_lock(&lock);
        struct state *s = lookup(fd, 2);
        if (!s) { pthread_mutex_unlock(&lock); return used ? (long)used : badfd(fd); }
        bool nb = s->flags & NONBLOCK;
        /* Clear stale wakeups before querying pending state; reverse ordering
         * could consume the notification for a signal arriving after dequeue. */
        struct kevent events[32]; struct timespec zero = {0, 0};
        int n = kevent(fd, NULL, 0, events, 32, &zero);
        uint64_t mask = s->mask;
        int error = n < 0 ? (int)LERR(errno) : 0;
        pthread_mutex_unlock(&lock);
        struct signalfd_siginfo info;
        int rc = error ? error : lxrt_signal_dequeue_pending(mask, &info);
        if (rc > 0) {
            pthread_mutex_lock(&lock);
            s = lookup(fd, 2);
            if (s) signal_ready(fd, s->mask);
            pthread_mutex_unlock(&lock);
        }
        if (rc > 0) {
            memcpy((char *)buf + used, &info, sizeof(info)); used += sizeof(info);
            if (len - used < sizeof(info)) return (long)used;
            continue;
        }
        if (used) return (long)used;
        if (rc < 0) return rc;
        if (nb) return LERR(EAGAIN);
        if (wait_readable(fd, false) < 0) return LERR(errno);
    }
}
