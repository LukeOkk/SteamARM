// futex_waitv(2) -- syscall 449, wait on up to 128 futexes at once.
//
// Proton's fsync is built on it (Wine's ntdll/unix/fsync.c): every Windows
// wait on several objects is one futex_waitv over the objects' words in a
// shared memory region, and every signal is a store plus FUTEX_WAKE on one
// word. Proton probes it with futex_waitv(NULL, 0, ...) and turns fsync off
// on ENOSYS -- which is all this runtime answered before.
//
// Darwin parks a thread on ONE ulock at a time, so a wait on several words is
// built from a board shared by every process of this user (a small file in the
// process tree's host temporary directory, mapped MAP_SHARED, so its words are
// shared-flavour ulocks keyed by the file's page whatever address each process
// maps it at):
//
//   gen[b]      a generation counter per bucket b of futex words
//   single[b]   futex_waitv callers whose words all fall in bucket b
//   multi[b]    callers with words in several buckets, one of them b
//   multigen    the generation those callers park on
//
// A word's bucket is its offset within its 4 KiB page, in words: the same
// for every process that maps the same shared page, whatever virtual address
// it got (Wine maps its fsync region at different addresses in each process),
// and the same under FEX's guest base, which is page aligned. Collisions only
// cost a wakeup that finds nothing changed.
//
// A waker (every FUTEX_WAKE in this runtime: thread.c wake_waiters,
// futex_ops.c wake_up_to) bumps gen[b] and wakes it only when single[b] says
// a futex_waitv caller is parked there, and multigen when multi[b] does: one
// relaxed load per wake when nobody uses futex_waitv.
//
// A caller registers, reads the generation, checks every word (EAGAIN if one
// differs from its expected value, as Linux), and parks on the generation.
// Woken, it returns the index of a word whose value changed. A wake that
// changed no word is not seen: the caller parks again. Linux would return
// that word's index; fsync -- the user this exists for -- always stores before
// it wakes (events set signaled, semaphores count, mutexes the owner), and
// re-checks its objects after any return.
//
// A wait on ONE word skips the board (wait_one below): it parks on the word
// itself, as a FUTEX_WAIT would -- what CrossOver's msync does on macOS with
// __ulock_wait2 on the object's own word. fsync waits on one word whenever a
// thread waits for one object without an alert, which is most of its waits.
#include "lxrt.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LERR(e) (-(long)lxrt_errno_to_linux(e))

extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wait2(uint32_t operation, void *addr, uint64_t value, uint64_t timeout_ns, uint64_t value2);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
#define UL_COMPARE_AND_WAIT_SHARED 3
#define ULF_WAKE_ALL               0x00000100
#define ULF_NO_ERRNO               0x01000000

#define BUCKETS        1024
#define WAITV_MAX      128
#define FUTEX2_SIZE_U32 0x02
#define FUTEX2_SIZE_MASK 0x03
#define FUTEX2_PRIVATE  128

struct board {
    _Atomic uint32_t gen[BUCKETS];
    _Atomic uint32_t single[BUCKETS];
    _Atomic uint32_t multi[BUCKETS];
    _Atomic uint32_t multigen;
};
#define BOARD_BYTES 16384

static struct board *g_board;
static pthread_once_t g_board_once = PTHREAD_ONCE_INIT;

static void board_map(void)
{
    char path[600];
    snprintf(path, sizeof path, "%s/lxrt-futexwaitv-%u", lxrt_host_tmpdir(), (unsigned)getuid());
    int fd = lxrt_open_private(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    struct stat st;
    if (fstat(fd, &st) == 0 && st.st_size < BOARD_BYTES)
        (void)ftruncate(fd, BOARD_BYTES);    // a new file reads as zeros
    void *p = mmap(NULL, BOARD_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    lxrt_close_private(fd);
    if (p != MAP_FAILED)
        g_board = p;
}

static struct board *board(void)
{
    pthread_once(&g_board_once, board_map);
    return g_board;
}

static unsigned bucket_of(const void *addr)
{
    return ((uintptr_t)addr >> 2) & (BUCKETS - 1);
}

void lxrt_futex_waitv_notify(const void *addr)
{
    struct board *b = board();
    if (!b)
        return;
    // The waker's store to the futex word comes before this; a waiter's
    // registration before its reads of the words: whichever comes second
    // sees the other (registered waiter and bump, or the new value).
    atomic_thread_fence(memory_order_seq_cst);
    unsigned k = bucket_of(addr);
    if (atomic_load_explicit(&b->single[k], memory_order_relaxed)) {
        atomic_fetch_add_explicit(&b->gen[k], 1, memory_order_seq_cst);
        __ulock_wake(UL_COMPARE_AND_WAIT_SHARED | ULF_WAKE_ALL | ULF_NO_ERRNO, &b->gen[k], 0);
    }
    if (atomic_load_explicit(&b->multi[k], memory_order_relaxed)) {
        atomic_fetch_add_explicit(&b->multigen, 1, memory_order_seq_cst);
        __ulock_wake(UL_COMPARE_AND_WAIT_SHARED | ULF_WAKE_ALL | ULF_NO_ERRNO, &b->multigen, 0);
    }
}

struct linux_futex_waitv { uint64_t val; uint64_t uaddr; uint32_t flags; uint32_t reserved; };
struct waitv_ts { int64_t tv_sec; int64_t tv_nsec; };

static int64_t clock_ns(int clockid)
{
    // The deadline is on the guest's own clock, as thread.c's FUTEX_WAIT_BITSET.
    return (int64_t)lxrt_guest_clock_ns(clockid == 0 ? 0 : 1);
}

// LXRT_WAITV_DIRECT=0: single-word waits use the board as well, the way every
// futex_waitv did before wait_one.
static bool direct_on(void)
{
    static _Atomic int on = -1;
    int v = atomic_load_explicit(&on, memory_order_relaxed);
    if (v < 0) {
        const char *e = getenv("LXRT_WAITV_DIRECT");
        v = !(e && *e == '0');
        atomic_store_explicit(&on, v, memory_order_relaxed);
    }
    return v;
}

// One __ulock_wait2 sleeps at most this long; the loop re-derives the rest (a
// wait longer than this re-parks once per slice, as futex_ops.c's waits do).
#define SLICE_NS 4294967294000ull               // ULOCK_MAX_US, ~71.6 minutes

// --------------------------------------------------------- one word, no board
//
// The waiter parks on the futex word itself, on the ulock flavour futex_ops.c
// gives a FUTEX_WAIT of the same kind (lxrt_futex_flavour): the process-shared
// flavour for an entry without FUTEX2_PRIVATE inside a mapping this runtime
// made shared, the task-local one everywhere else. That is the queue every
// wake of the word already reaches, because a FUTEX_WAIT parks there:
//   shared FUTEX_WAKE, WAKE_BITSET, requeue, WAKE_OP, UNLOCK_PI, from any
//       process: flavour 3 for a shared word, then flavour 1 when this
//       process has a thread parked (futex_ops.c wake_up_to);
//   private FUTEX_WAKE(_BITSET) from this process: flavour 1, then 3
//       (thread.c wake_waiters);
//   a thread's exit (CLONE_CHILD_CLEARTID): both flavours (thread.c).
// The waiter counts itself in thread.c's g_parked before it last reads the
// word, as every waiter does: those wakes skip a task-local queue only when
// nobody of their process is counted there. Fsync's words live in 64 KiB
// MAP_SHARED pieces of one shared memory object, mapped at its own address in
// each process, so its waits and wakes meet on flavour 3, keyed by the object
// and offset whatever the address.
//
// No lost wakeup: a waker stores to the word before it wakes, and Darwin
// compares the word with val and queues the waiter under the queue's lock,
// which the wake takes too -- either the waiter is queued when the wake runs,
// or its compare sees the store and it does not sleep. With the word changed
// before it parks, the waiter returns at once (EAGAIN on the first look, 0
// after: the word changed during the call, which the board path reported as a
// wake of that word too).
//
// Returns 0 when woken, which Linux also does for a wake that changed nothing;
// fsync re-checks its objects after every return, and the board path's
// re-park for a wake that changed no word stays where the board is used.
static long wait_one(_Atomic uint32_t *addr, uint32_t val, bool priv, bool timed,
                     int64_t deadline, int32_t clockid)
{
    uint32_t op = lxrt_futex_flavour((const void *)addr, !priv) | ULF_NO_ERRNO;
    unsigned faults = 0;
    for (bool first = true;; first = false) {
        uint64_t ns = 0;                             // 0: no timeout
        if (timed) {
            int64_t left = deadline - clock_ns(clockid);
            if (left <= 0)
                return LERR(ETIMEDOUT);
            ns = (uint64_t)left > SLICE_NS ? SLICE_NS : (uint64_t)left;
        }
        lxrt_futex_park_enter((const void *)addr);
        uint32_t cur;
        if (!lxrt_probe_u32((uint64_t)(uintptr_t)addr, &cur)) {
            lxrt_futex_park_leave((const void *)addr);
            return LERR(EFAULT);                     // unmapped while we waited
        }
        if (cur != val) {
            lxrt_futex_park_leave((const void *)addr);
            return first ? LERR(EAGAIN) : 0;
        }
        int r = __ulock_wait2(op, (void *)addr, val, ns, 0);
        lxrt_futex_park_leave((const void *)addr);
        if (r >= 0)
            return 0;                                // woken, or the word moved as it parked
        if (r == -ETIMEDOUT)
            continue;                                // the guest's clock decides, above
        if (r == -EINTR && lxrt_interrupted_internally())
            continue;                                // no guest handler ran: Linux would still wait
        // Darwin reads the word with page faults disabled and answers EFAULT
        // when the page is not resident -- compressed or paged out since our
        // own read -- for the caller to fault it in and ask again (xnu
        // sys_ulock_wait2; msync retries the same way). The probe above does
        // that, or reports the word unmapped.
        if (r == -EFAULT && ++faults < 1000)
            continue;
        return LERR(-r);                             // EINTR: a guest handler ran
    }
}

long lxrt_futex_waitv(uint64_t waiters, uint32_t nr, uint32_t flags, uint64_t timeout, int32_t clockid)
{
    // Linux's order of checks (kernel/futex/syscalls.c): flags, count,
    // clock, then each entry.
    if (flags)
        return LERR(EINVAL);
    if (!nr || nr > WAITV_MAX || !waiters)
        return LERR(EINVAL);
    const struct waitv_ts *ts = (const struct waitv_ts *)(uintptr_t)timeout;
    if (ts && clockid != 0 && clockid != 1)          // CLOCK_REALTIME, CLOCK_MONOTONIC
        return LERR(EINVAL);
    if (ts && (ts->tv_sec < 0 || ts->tv_nsec < 0 || ts->tv_nsec >= 1000000000))
        return LERR(EINVAL);

    const struct linux_futex_waitv *w = (const struct linux_futex_waitv *)(uintptr_t)waiters;
    _Atomic uint32_t *addr[WAITV_MAX];
    uint32_t val[WAITV_MAX];
    uint64_t base = lxrt_gbase();
    for (uint32_t i = 0; i < nr; i++) {
        if ((w[i].flags & ~(uint32_t)(FUTEX2_SIZE_MASK | FUTEX2_PRIVATE)) ||
            (w[i].flags & FUTEX2_SIZE_MASK) != FUTEX2_SIZE_U32 || w[i].reserved)
            return LERR(EINVAL);
        if (w[i].val >> 32)                          // does not fit the 32-bit word
            return LERR(EINVAL);
        uint64_t a = w[i].uaddr;
        if (base && a && a < (1ull << 32))           // FEX's low window (gbase.c)
            a += base;
        if (!a || (a & 3))
            return LERR(EINVAL);
        addr[i] = (_Atomic uint32_t *)(uintptr_t)a;
        val[i] = (uint32_t)w[i].val;
    }

    // A deadline past what 64-bit nanoseconds hold is no deadline: Linux
    // saturates it (ktime_set), and the product below would overflow into
    // one already past.
    bool timed = ts && ts->tv_sec < INT64_MAX / 1000000000LL;
    int64_t deadline = timed ? ts->tv_sec * 1000000000LL + ts->tv_nsec : 0;

    // The first look, in index order as Linux's: a word that cannot be read is
    // EFAULT, not a fault in the runtime; one that already differs is EAGAIN.
    for (uint32_t i = 0; i < nr; i++) {
        uint32_t cur;
        if (!lxrt_probe_u32((uint64_t)(uintptr_t)addr[i], &cur))
            return LERR(EFAULT);
        if (cur != val[i])
            return LERR(EAGAIN);
    }

    // Without the full table of shared ranges (no memory to record one), the
    // flavour of a word could not be told: the board knows no flavours.
    if (nr == 1 && direct_on() && !lxrt_futex_shared_lost())
        return wait_one(addr[0], val[0], (w[0].flags & FUTEX2_PRIVATE) != 0, timed, deadline, clockid);

    struct board *b = board();
    if (!b)
        return LERR(ENOMEM);

    unsigned k0 = bucket_of((const void *)addr[0]);
    bool one_bucket = true;
    for (uint32_t i = 1; i < nr; i++)
        if (bucket_of((const void *)addr[i]) != k0) one_bucket = false;
    if (one_bucket)
        atomic_fetch_add_explicit(&b->single[k0], 1, memory_order_seq_cst);
    else
        for (uint32_t i = 0; i < nr; i++)
            atomic_fetch_add_explicit(&b->multi[bucket_of((const void *)addr[i])], 1, memory_order_seq_cst);
    _Atomic uint32_t *gen = one_bucket ? &b->gen[k0] : &b->multigen;

    long ret;
    bool first = true;
    for (;;) {
        uint32_t g = atomic_load_explicit(gen, memory_order_seq_cst);
        atomic_thread_fence(memory_order_seq_cst);
        ret = -1;
        for (uint32_t i = 0; i < nr && ret < 0; i++)
            if (atomic_load_explicit(addr[i], memory_order_seq_cst) != val[i])
                ret = first ? LERR(EAGAIN) : (long)i;
        if (ret != -1)
            break;
        first = false;
        uint32_t us = 0;                             // 0: no timeout
        if (timed) {
            int64_t left = deadline - clock_ns(clockid);
            if (left <= 0) { ret = LERR(ETIMEDOUT); break; }
            int64_t u = left / 1000;
            us = u <= 0 ? 1 : (u >= UINT32_MAX ? UINT32_MAX - 1 : (uint32_t)u);
        }
        int r = __ulock_wait(UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO, (void *)gen, g, us);
        if (r == -EINTR && !lxrt_interrupted_internally()) {
            ret = LERR(EINTR);                       // a guest handler ran
            break;
        }
        // Woken, the generation moved, a timeout slice ended, or a signal
        // that ran no guest handler: look at the words again.
    }

    if (one_bucket)
        atomic_fetch_sub_explicit(&b->single[k0], 1, memory_order_seq_cst);
    else
        for (uint32_t i = 0; i < nr; i++)
            atomic_fetch_sub_explicit(&b->multi[bucket_of((const void *)addr[i])], 1, memory_order_seq_cst);
    return ret;
}
