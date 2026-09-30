// Guest threads and futexes.
//
// A Linux thread is created by clone(CLONE_VM|CLONE_THREAD|...); a Darwin
// thread is created by pthread_create. The two are close enough that the guest
// thread can simply run on a Darwin thread -- what differs is everything around
// it: the guest supplies its own stack, its own thread pointer, and expects a
// tid, a parent/child tid write-back, and a futex wake when the thread dies.
// glibc's pthread_join blocks on exactly that last one, so a thread runtime
// that skips it hangs instead of failing.

#include "lxrt.h"
#include "binder.h"
#include <signal.h>
#include "fileops2.h"

#include <errno.h>

#define LERR(e) (-lxrt_errno_to_linux(e))
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Darwin's futex primitives. Not public API, but they are what
// os_unfair_lock and libc++ are built on, and they are the only thing on this
// system with compare-and-wait semantics.
extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value,
                        uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);

#define UL_COMPARE_AND_WAIT        1
#define UL_COMPARE_AND_WAIT_SHARED 3
#define ULF_NO_ERRNO               0x01000000u
#define ULF_WAKE_ALL               0x00000100u

// Linux clone flags, the subset that means "make a thread".
#define CLONE_VM             0x00000100
#define CLONE_FS             0x00000200
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_THREAD         0x00010000
#define CLONE_SYSVSEM        0x00040000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

// Linux futex operations.
#define FUTEX_WAIT            0
#define FUTEX_WAKE            1
#define FUTEX_WAIT_BITSET     9
#define FUTEX_WAKE_BITSET    10
#define FUTEX_BITSET_MATCH_ANY 0xffffffffu
#define FUTEX_PRIVATE_FLAG  128
#define FUTEX_CLOCK_REALTIME 256

struct linux_timespec64 { int64_t tv_sec; int64_t tv_nsec; };

// Per-guest-thread state. Lives in Darwin TSD so it follows the thread without
// a lookup table.
struct guest_thread {
    int       tid;
    uint32_t *clear_child_tid;  // written to 0 and woken when the thread exits
};

// tid -> Darwin thread, so a thread-directed signal actually reaches the thread
// the guest named. Mapping tgkill onto a process-directed kill() does not work
// here: the process also contains AppKit's main thread and libdispatch's
// workers, and a signal handed to one of those is either lost or, worse, has
// its context rewritten into guest code.
#define MAX_GUEST_THREADS 512
#define RT_QUEUE_DEPTH 32
static struct {
    int tid;
    pthread_t th;
    bool used;
    // Linux realtime signals are QUEUED, not merely pending, so a count is not
    // enough -- each delivery has to come back out in order.
    uint8_t rt[RT_QUEUE_DEPTH];
    uint8_t rt_head, rt_tail;
} g_threads[MAX_GUEST_THREADS];
static pthread_mutex_t g_threads_lock = PTHREAD_MUTEX_INITIALIZER;

// The signal handler takes g_threads_lock too (realtime queue: pending and
// dequeue), so a host signal landing while a thread holds it deadlocked that
// thread on itself (MEASURED: steam.sh's bash parked forever in
// lxrt_rt_pending_unblocked -> SIGCHLD -> host_handler -> same mutex). Every
// holder therefore blocks host signals for the duration.
static _Thread_local sigset_t g_threads_lock_saved;
static void threads_lock(void)
{
    sigset_t all;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &g_threads_lock_saved);
    pthread_mutex_lock(&g_threads_lock);
}
static void threads_unlock(void)
{
    pthread_mutex_unlock(&g_threads_lock);
    pthread_sigmask(SIG_SETMASK, &g_threads_lock_saved, NULL);
}
LXRT_FORK_SAFE(thread_g_threads_lock, g_threads_lock)

static void register_thread(int tid, pthread_t th)
{
    threads_lock();
    for (int i = 0; i < MAX_GUEST_THREADS; i++)
        if (!g_threads[i].used) {
            memset(&g_threads[i], 0, sizeof(g_threads[i]));
            g_threads[i].tid = tid;
            g_threads[i].th = th;
            g_threads[i].used = true;
            break;
        }
    threads_unlock();
}

static void unregister_thread(int tid)
{
    threads_lock();
    for (int i = 0; i < MAX_GUEST_THREADS; i++)
        if (g_threads[i].used && g_threads[i].tid == tid) {
            lxrt_fileops2_thread_exited(g_threads[i].th);
            g_threads[i].used = false;
            break;
        }
    threads_unlock();
    // /proc/self/task/<tid> disappears with the thread: Chromium stops a
    // helper thread and polls fstatat(proc_fd, "self/task/<tid>") -- a
    // relative lookup no regeneration sees -- until it is gone (FATAL
    // "Stopped thread does not disappear in /proc" otherwise, MEASURED).
    lxrt_proc_thread_gone(tid);
}

// The first guest thread, which is where process-directed signals are sent.
static pthread_t g_main_guest;
static bool g_main_guest_set;

// Every registered guest thread's tid, for /proc/self/task. Returns the
// count (which may exceed max; only max are written).
int lxrt_thread_list(int *tids, int max)
{
    int n = 0;
    threads_lock();
    for (int i = 0; i < MAX_GUEST_THREADS; i++)
        if (g_threads[i].used) {
            if (n < max) tids[n] = g_threads[i].tid;
            n++;
        }
    threads_unlock();
    return n;
}

bool lxrt_thread_lookup(int tid, pthread_t *out)
{
    threads_lock();
    bool found = false;
    for (int i = 0; i < MAX_GUEST_THREADS; i++)
        if (g_threads[i].used && g_threads[i].tid == tid) {
            *out = g_threads[i].th;
            found = true;
            break;
        }
    threads_unlock();
    return found;
}

bool lxrt_main_guest_thread(pthread_t *out)
{
    if (!g_main_guest_set)
        return false;
    *out = g_main_guest;
    return true;
}

static pthread_key_t g_gt_key;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static atomic_int g_next_tid;

static void gt_free(void *p)
{
    struct guest_thread *gt = p;
    unregister_thread(gt->tid);   // Also cover host pthread return/cancellation.
    free(gt);
}

// Thread ids. The main guest thread's tid is the pid, as on Linux. Every
// other thread gets 200000 + pid*10000 + n: disjoint from Darwin pids (all
// below 100000), unique per process, and below 2^30 (FUTEX_TID_MASK: glibc
// keeps the owner tid of a robust/PI mutex in 30 bits). The old pid+1, pid+2...
// scheme collided with the pids of later children: the i386 Steam client's
// fork returned a pid equal to one of its own thread ids, and the child's exit
// was taken for that thread's (measured: tests/elf/jit_fork_mt.c).
static int next_tid(void)
{
    int n = atomic_fetch_add(&g_next_tid, 1);
    return 200000 + ((int)getpid() % 100000) * 10000 + (n % 10000);
}

static void gt_init(void)
{
    pthread_key_create(&g_gt_key, gt_free);
    atomic_store(&g_next_tid, 1);
}

static struct guest_thread *gt_self(void)
{
    pthread_once(&g_once, gt_init);
    struct guest_thread *gt = pthread_getspecific(g_gt_key);
    if (!gt) {
        gt = calloc(1, sizeof(*gt));
        if (!gt)
            return NULL;
        gt->tid = g_main_guest_set ? next_tid() : (int)getpid();
        pthread_setspecific(g_gt_key, gt);
        register_thread(gt->tid, pthread_self());
        if (!g_main_guest_set) {
            g_main_guest = pthread_self();
            g_main_guest_set = true;
        }
    }
    return gt;
}

// After fork(): only the forking thread survives, and Linux makes it the
// new process's main thread -- tid == pid. Every other registry entry is a
// dead thread, and a lock one of them held would never be released.
void lxrt_thread_after_fork(void)
{
    pthread_mutex_init(&g_threads_lock, NULL);
    pthread_t me = pthread_self();
    for (int i = 0; i < MAX_GUEST_THREADS; i++) {
        if (!g_threads[i].used) continue;
        if (pthread_equal(g_threads[i].th, me))
            g_threads[i].tid = (int)getpid();
        else
            g_threads[i].used = false;
    }
    struct guest_thread *gt = gt_self();
    if (gt)
        gt->tid = (int)getpid();
    atomic_store(&g_next_tid, 1);
    g_main_guest = me;
    g_main_guest_set = true;
}


int lxrt_gettid(void)
{
    struct guest_thread *gt = gt_self();
    return gt ? gt->tid : (int)getpid();
}

long lxrt_set_tid_address(uint32_t *ctid)
{
    struct guest_thread *gt = gt_self();
    if (!gt)
        return LERR(ENOMEM);
    gt->clear_child_tid = ctid;
    return gt->tid;
}

// ---------------------------------------------------------------- futex
//
// Releasing waiters, and why one wake needs two syscalls.
//
// A ulock queue is keyed by its FLAVOUR as well as by its address, and the two
// flavours never see each other. Measured on this machine, both directions:
// four threads parked on UL_COMPARE_AND_WAIT and woken with
// UL_COMPARE_AND_WAIT_SHARED got -ENOENT and 0 of 4 released, and the same four
// went 4 of 4 the instant the flavours matched; four parked on the shared
// flavour and woken on the task-local one behaved identically.
//
// Which flavour a WAITER lands on is decided by FUTEX_PRIVATE_FLAG -- the
// private forms park here on the task-local flavour, the shared forms park in
// futex_ops.c on flavour 3. A WAKER does not always get that choice, because
// Linux's own key derivation collapses the two for anonymous memory and glibc
// leans on it: it waits with the shared form on words that only ever get woken
// privately. So a wake asks the other queue whenever its own answers "empty".
// The price is one extra 111 ns syscall on wakes that found nobody, and one
// divergence -- a private wake can release a shared waiter, an extra wakeup,
// the same direction futex_ops.c's mirror-image fallback already accepts.
//
// The count is a LOWER BOUND, not the number Linux returns, because Darwin will
// not supply one and it cannot be recovered by waking one at a time: measured,
// a single parked waiter answered eight consecutive __ulock_wake calls with
// success (and two waiters likewise answered eight), because a ulock keeps
// reporting success until the thread it already released is scheduled and
// dequeues itself. So the loop would count scheduling latency, and it would be
// wrong in the dangerous direction. 1 when Darwin released somebody, 0 when the
// queue was empty -- the same rule futex_ops.c settled on, so both halves of
// the futex family answer alike. A low count is a value Linux itself can
// return; an inflated one is not.
static int wake_waiters(uint32_t *addr, int32_t budget)
{
    uint32_t flags = ULF_NO_ERRNO;
    // futex_wake() tests its budget only AFTER releasing a waiter
    // (`if (++ret >= nr_wake) break;`), so val 0 still releases exactly one.
    if (budget > 1)
        flags |= ULF_WAKE_ALL;
    // -ENOENT is an empty queue, -EINVAL a malformed address. Neither released
    // anything. Measured: __ulock_wake returns 0 on success whatever it woke,
    // and its `wake_value` argument is ignored without ULF_WAKE_THREAD -- 0 and
    // 0x7fffffff both released all four waiters.
    if (__ulock_wake(flags | UL_COMPARE_AND_WAIT, addr, 0) == 0)
        return 1;
    if (__ulock_wake(flags | UL_COMPARE_AND_WAIT_SHARED, addr, 0) == 0)
        return 1;
    return 0;
}

long lxrt_futex(uint32_t *uaddr, int op, uint32_t val, uint64_t timeout_or_val2,
                uint32_t *uaddr2, uint32_t val3)
{
    (void)uaddr2;
    if (!uaddr)
        return LERR(EFAULT);
    // Linux rejects a futex word that is not 4-byte aligned with EINVAL, in
    // get_futex_key, for every operation. Darwin agrees for waits -- measured
    // __ulock_wait on a 2-aligned address = -EINVAL -- but not for wakes, where
    // the same address comes back -ENOENT and would be reported to the guest as
    // "nobody was waiting" instead of as the error it is.
    if ((uintptr_t)uaddr % sizeof(uint32_t))
        return LERR(EINVAL);

    int base = op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME);

    switch (base) {
    case FUTEX_WAIT:
    case FUTEX_WAIT_BITSET: {
        // A zero bitset is EINVAL on Linux -- measured -22 on the guest for
        // both BITSET forms. A partial one cannot be honoured, because a ulock
        // queue has no bitset; futex_ops.c carries the full argument for why
        // waking a waiter Linux would have skipped is the safe direction.
        if (base == FUTEX_WAIT_BITSET && val3 == 0)
            return LERR(EINVAL);

        // Linux compares *uaddr against val under the futex hash-bucket lock
        // and returns EAGAIN on a mismatch. __ulock_wait compares too, but
        // MEASURED on this machine a mismatch comes back as 0, not -EAGAIN --
        // on both flavours and from bss, data, heap, mmap and stack addresses
        // alike -- which a guest reads as a spurious wakeup rather than as the
        // error Linux documents. So the compare is made here as well. The
        // residual race, the word changing between this load and Darwin's own
        // compare, still returns 0: a wakeup every futex caller re-checks for.
        // It runs before the deadline arithmetic because EAGAIN outranks
        // ETIMEDOUT on Linux when both apply.
        uint32_t cur = atomic_load_explicit((_Atomic uint32_t *)uaddr,
                                            memory_order_seq_cst);
        if (cur != val)
            return LERR(EAGAIN);

        // Linux FUTEX_WAIT takes a relative timeout; FUTEX_WAIT_BITSET takes an
        // absolute one. Darwin's __ulock_wait only does relative microseconds,
        // and 0 means "no timeout".
        uint32_t us = 0;
        const struct linux_timespec64 *ts =
            (const struct linux_timespec64 *)(uintptr_t)timeout_or_val2;
        // A deadline past what 64-bit nanoseconds can hold is forever, as on
        // Linux (futex_ops.c's GUEST_SEC_MAX). Multiplying it overflowed and
        // came back negative, so the wait returned ETIMEDOUT at once: bionic's
        // Condition::waitRelative(INT64_MAX) asks for {LONG_MAX, ...}, and
        // audioserver's TimeCheck thread, waiting like that on an empty list,
        // woke immediately and aborted the process ("TimeCheck timeout for
        // <unspecified>", MEASURED at stage 28).
        if (ts && ts->tv_sec >= 0 && ts->tv_sec >= INT64_MAX / 1000000000LL &&
            ts->tv_nsec >= 0 && ts->tv_nsec < 1000000000LL)
            ts = NULL;
        if (ts) {
            int64_t ns;
            if (base == FUTEX_WAIT_BITSET) {
                // The deadline is on the guest's clock: compare against the
                // same counter it read (Darwin's CLOCK_MONOTONIC is another
                // base -- seconds apart here, hours on a laptop that slept).
                int64_t now = (int64_t)lxrt_guest_clock_ns((op & FUTEX_CLOCK_REALTIME) ? 0 : 1);
                ns = ts->tv_sec * 1000000000LL + ts->tv_nsec - now;
            } else {
                ns = ts->tv_sec * 1000000000LL + ts->tv_nsec;
            }
            if (ns <= 0)
                return LERR(ETIMEDOUT);
            // Clamp rather than overflow: a wait longer than ~71 minutes
            // becomes an untimed wait, which is closer to the intent than a
            // wrapped short one.
            int64_t clamped = ns / 1000;
            us = clamped > 0 && clamped < UINT32_MAX ? (uint32_t)clamped : 0;
            if (us == 0)
                us = 1;
        }

        int r = __ulock_wait(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO, uaddr, val, us);
        // A successful __ulock_wait returns the number of waiters STILL parked
        // on the address, not 0 -- measured 3, 2, 1, 0 across four threads
        // released by one ULF_WAKE_ALL. Linux's FUTEX_WAIT returns 0 on a
        // wakeup, so every non-negative answer collapses to 0 here.
        if (r >= 0)
            return 0;
        // __ulock_wait with ULF_NO_ERRNO returns a negative DARWIN errno.
        return LERR(-r);
    }

    case FUTEX_WAKE:
    case FUTEX_WAKE_BITSET: {
        if (base == FUTEX_WAKE_BITSET && val3 == 0)
            return LERR(EINVAL);
        // val is a signed count on Linux and a budget of 0 or less still
        // releases one waiter; see wake_waiters() for why the answer is a lower
        // bound rather than the number woken.
        int32_t nr = (int32_t)val;
        // A ulock queue has no bitset, so "wake one waiter whose bitset
        // matches" cannot be targeted: waking one could release a thread of
        // the other class and leave the intended one parked forever. FEX's
        // WritePriorityMutex wakes one WRITER with bitset 2 while readers wait
        // on bitset 1 -- pressure-vessel under FEX deadlocked with four
        // threads parked on one word (MEASURED). A partial bitset wakes every
        // waiter instead; they re-check, which Linux futex users must anyway.
        if (base == FUTEX_WAKE_BITSET && val3 != FUTEX_BITSET_MATCH_ANY)
            nr = INT32_MAX;
        return wake_waiters(uaddr, nr <= 0 ? 1 : nr);
    }

    default:
        // FUTEX_REQUEUE, FUTEX_CMP_REQUEUE, PI futexes and WAKE_OP have no
        // Darwin equivalent and are not emulated. Report rather than pretend.
        return LERR(ENOSYS);
    }
}

// ---------------------------------------------------------------- clone

struct clone_ctx {
    struct lxrt_regs regs;   // the parent's register file, inherited by the child
    uint64_t  stack;
    uint64_t  tls;
    uint64_t  x18;           // the parent's virtual x18: registers are inherited
    uint64_t  rt_mask;       // the parent's realtime signal mask (signal.c)
    uint32_t *ctid;
    int       tid;
    bool      set_tls;
    bool      clear_ctid;
};

static void *thread_start(void *arg)
{
    struct clone_ctx ctx = *(struct clone_ctx *)arg;
    free(arg);

    struct guest_thread *gt = gt_self();
    if (gt) {
        // gt_self registered a provisional tid; replace it with the one the
        // parent already reported to the guest.
        unregister_thread(gt->tid);
        gt->tid = ctx.tid;
        gt->clear_child_tid = ctx.clear_ctid ? ctx.ctid : NULL;
        register_thread(gt->tid, pthread_self());
    }
    if (ctx.set_tls)
        lxrt_tls_set(ctx.tls);
    lxrt_x18_set(ctx.x18);
    lxrt_rt_mask_set(ctx.rt_mask);

    // Does not return: switches to the guest's stack, restores the parent's
    // registers and resumes at the instruction after the guest's `svc`, with
    // x0 = 0 as Linux promises.
    static _Thread_local struct lxrt_regs regs;
    regs = ctx.regs;
    lxrt_host_altstack_install();   // host signal frames never touch the guest stack
    lxrt_thread_enter(&regs, ctx.stack);
}

long lxrt_clone(uint64_t flags, uint64_t child_stack, uint32_t *ptid,
                uint64_t tls, uint32_t *ctid, const struct lxrt_regs *parent)
{
    // clone without CLONE_VM|CLONE_THREAD is fork: a new process, not a new
    // thread. See process.c for why the runtime has one.
    if (!(flags & CLONE_THREAD) || !(flags & CLONE_VM))
        return lxrt_fork();
    if (!child_stack)
        return LERR(EINVAL);
    // A thread with its own copy of the descriptor table (no CLONE_FILES)
    // cannot be made: a Darwin thread shares its process's table. bionic's
    // crash handler clones such a "pseudothread" (debuggerd_handler.cpp,
    // "pthread_create without CLONE_FILES") and closes every descriptor in
    // it; run as an ordinary thread here it closed the process's stdin,
    // stdout and stderr, and the crash ended in _exit(1) instead of the
    // signal (stage 25). Refused, the handler gives up and the process dies
    // by its signal, as Linux reports it.
    if (!(flags & CLONE_FILES)) {
        extern bool lxrt_trace_on(void);
        if (lxrt_trace_on())
            fprintf(lxrt_trace_stream(), "[lxrt] clone: a thread without CLONE_FILES (flags 0x%llx) "
                            "cannot have its own descriptor table: EINVAL\n",
                    (unsigned long long)flags);
        return LERR(EINVAL);
    }

    pthread_once(&g_once, gt_init);
    int tid = next_tid();

    struct clone_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return LERR(ENOMEM);
    ctx->regs = *parent;
    ctx->x18 = lxrt_x18_get();
    ctx->rt_mask = lxrt_rt_mask_get();
    ctx->stack = child_stack;
    // Without CLONE_SETTLS the child starts with the parent's thread pointer:
    // Linux copies TPIDR_EL0 into the new task (arm64 copy_thread). bionic's
    // crash handler clones such a "pseudothread" (debuggerd_handler.cpp);
    // started with 0 here, its first stack-protector load read address 0x28
    // and every Android abort became a recursive SIGSEGV (stage 25).
    ctx->tls = (flags & CLONE_SETTLS) ? tls : lxrt_tls_get();
    ctx->ctid = ctid;
    ctx->tid = tid;
    ctx->set_tls = true;
    ctx->clear_ctid = (flags & CLONE_CHILD_CLEARTID) != 0;

    // Both tid write-backs happen before the thread starts, so the parent can
    // rely on ptid immediately and the child sees ctid already set.
    if ((flags & CLONE_PARENT_SETTID) && ptid)
        *ptid = (uint32_t)tid;
    if ((flags & CLONE_CHILD_SETTID) && ctid)
        *ctid = (uint32_t)tid;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    // The guest runs on its own stack; Darwin's is only used until the switch,
    // so the smallest the platform accepts is enough.
    pthread_attr_setstacksize(&attr, 512 * 1024);

    extern bool lxrt_trace_on(void);
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] clone: tid=%d stack=0x%llx tls=0x%llx pc=0x%llx "
                        "flags=0x%llx\n", tid, (unsigned long long)child_stack,
                (unsigned long long)tls, (unsigned long long)parent->pc_after,
                (unsigned long long)flags);

    pthread_t th;
    int rc = pthread_create(&th, &attr, thread_start, ctx);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(ctx);
        return LERR(EAGAIN);
    }
    return tid;
}

// glibc 2.34+ calls clone3 first and only falls back to clone on ENOSYS -- a
// fallback that stayed dead until errno translation was fixed, because Darwin's
// ENOSYS (78) reads as EREMCHG on Linux.
struct linux_clone_args {
    uint64_t flags, pidfd, child_tid, parent_tid, exit_signal;
    uint64_t stack, stack_size, tls, set_tid, set_tid_size, cgroup;
};

long lxrt_clone3(const void *uargs, uint64_t size, const struct lxrt_regs *parent)
{
    if (!uargs)
        return LERR(EFAULT);
    // The struct grows over kernel versions; a short one is valid and the
    // missing tail reads as zero, a long one must not be truncated silently.
    if (size < 8 * 8)
        return LERR(EINVAL);

    struct linux_clone_args a;
    memset(&a, 0, sizeof(a));
    memcpy(&a, uargs, size < sizeof(a) ? (size_t)size : sizeof(a));

    if (a.set_tid_size != 0)
        return LERR(ENOSYS);   // choosing the child's tid is not supported

    // clone3 gives the stack's LOW address plus a size; clone gives the top.
    if (!a.stack || !a.stack_size)
        return LERR(EINVAL);
    uint64_t top = a.stack + a.stack_size;

    return lxrt_clone(a.flags, top, (uint32_t *)(uintptr_t)a.parent_tid,
                      a.tls, (uint32_t *)(uintptr_t)a.child_tid, parent);
}

// A guest thread calling exit(2) means "this thread", not the process. glibc's
// pthread_join waits for the CLONE_CHILD_CLEARTID write and the futex wake
// that follows it, so both must happen or the join hangs forever.
static void exit_on_host_stack(void *arg) __attribute__((noreturn));

void lxrt_thread_exit(int code)
{
    lxrt_binder_thread_exit();      // the hub releases this thread's binder_threads
    struct guest_thread *gt = pthread_getspecific(g_gt_key);
    if (gt)
        unregister_thread(gt->tid);
    // exit(2) of the last thread ends the process with that status on Linux.
    // Here the host main thread would go on waiting for the guest forever
    // (MEASURED: a static probe ending in SYS_exit never returned).
    {
        int live = 0;
        threads_lock();
        for (int i = 0; i < MAX_GUEST_THREADS; i++)
            live += g_threads[i].used;
        threads_unlock();
        if (live == 0) {
            lxrt_sigstats_flush();
            lxrt_sysv_exit();
            lxrt_proc_cleanup();
            _exit(code & 0xff);
        }
    }
    // Everything from the clear-tid wake on runs on this host thread's own
    // stack, not the guest's: the dispatcher runs on the guest stack, and the
    // joiner may unmap that stack as soon as it wakes (glibc frees thread
    // stacks beyond its 40 MiB cache at join). pthread_exit's teardown then
    // faulted inside libsystem_kernel on the unmapped stack and the fault went
    // to the guest (MEASURED: tests/elf/jit_rwx.c, 8 threads x 20 rounds of
    // create/join, SIGSEGV at stat+0x10 / mach_port_mod_refs+0x68 with a
    // guest-stack address, 2 of 4 runs). The top of the host pthread's stack
    // holds only the abandoned frames of thread_start, which never returns.
    extern void lxrt_run_on_stack(uint64_t sp, void (*fn)(void *), void *arg)
        __attribute__((noreturn));
    uint64_t top = (uint64_t)(uintptr_t)pthread_get_stackaddr_np(pthread_self());
    lxrt_run_on_stack((top - 16384) & ~15ull, exit_on_host_stack, gt);
}

static void exit_on_host_stack(void *arg)
{
    struct guest_thread *gt = arg;
    if (gt && gt->clear_child_tid) {
        __atomic_store_n(gt->clear_child_tid, 0, __ATOMIC_SEQ_CST);
        // Both flavours, and this is the whole of a hang that looked like a
        // broken pthread_cond_broadcast: glibc parks pthread_join on the tid
        // word with a NON-private futex -- measured on the guest, op 0x109 =
        // FUTEX_WAIT_BITSET|FUTEX_CLOCK_REALTIME with FUTEX_PRIVATE_FLAG
        // clear -- so dispatch.c routes it to futex_ops.c, which parks it on
        // UL_COMPARE_AND_WAIT_SHARED. A wake on the task-local flavour alone
        // reached nobody (measured -ENOENT, 0 of 4 released) and every join
        // that arrived before its thread exited slept forever. Linux never has
        // to choose: for anonymous memory its futex key is the same either way,
        // which is exactly why glibc can use the shared form on a private word.
        // Unconditional rather than fallback-on-empty -- a thread exits once
        // and a missed join wake is a hang.
        __ulock_wake(UL_COMPARE_AND_WAIT | ULF_NO_ERRNO | ULF_WAKE_ALL,
                     gt->clear_child_tid, 0);
        __ulock_wake(UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO | ULF_WAKE_ALL,
                     gt->clear_child_tid, 0);
        gt->clear_child_tid = NULL;
    }
    pthread_exit(NULL);
}

// ------------------------------------------------- realtime signal queues
//
// Linux has 32 realtime signals; Darwin has none. Both ends of every delivery
// here are guest code going through this runtime, so the runtime carries them
// itself: the number is queued against the target thread and a carrier signal
// wakes it. See signal.c for the carrier and the delivery side.

bool lxrt_rt_enqueue(int tid, int lsig)
{
    bool ok = false;
    threads_lock();
    for (int i = 0; i < MAX_GUEST_THREADS; i++) {
        if (!g_threads[i].used || g_threads[i].tid != tid)
            continue;
        uint8_t next = (uint8_t)((g_threads[i].rt_tail + 1) % RT_QUEUE_DEPTH);
        if (next != g_threads[i].rt_head) {     // full queues drop, as Linux does
            g_threads[i].rt[g_threads[i].rt_tail] = (uint8_t)lsig;
            g_threads[i].rt_tail = next;
            ok = true;
        }
        break;
    }
    threads_unlock();
    return ok;
}

// Pops the first queued realtime signal of the CALLING thread that `blocked`
// (Linux mask bits, bit n-1 for signal n) does not block; 0 if none. Blocked
// ones stay queued, in order, until the mask changes -- as Linux keeps them
// pending.
int lxrt_rt_dequeue_self_mask(uint64_t blocked)
{
    int tid = lxrt_gettid();
    int sig = 0;
    threads_lock();
    for (int i = 0; i < MAX_GUEST_THREADS; i++) {
        if (!g_threads[i].used || g_threads[i].tid != tid)
            continue;
        uint8_t h = g_threads[i].rt_head, t = g_threads[i].rt_tail;
        for (uint8_t k = h; k != t; k = (uint8_t)((k + 1) % RT_QUEUE_DEPTH)) {
            int s = g_threads[i].rt[k];
            if (s >= 1 && s <= 64 && (blocked >> (s - 1)) & 1)
                continue;
            sig = s;
            // Close the gap: shift the later entries down by one.
            for (uint8_t j = k; ; ) {
                uint8_t nx = (uint8_t)((j + 1) % RT_QUEUE_DEPTH);
                if (nx == t) break;
                g_threads[i].rt[j] = g_threads[i].rt[nx];
                j = nx;
            }
            g_threads[i].rt_tail = (uint8_t)((t + RT_QUEUE_DEPTH - 1) % RT_QUEUE_DEPTH);
            break;
        }
        break;
    }
    threads_unlock();
    return sig;
}

// Is anything queued for the calling thread that `blocked` lets through?
bool lxrt_rt_pending_unblocked(uint64_t blocked)
{
    int tid = lxrt_gettid();
    bool any = false;
    threads_lock();
    for (int i = 0; i < MAX_GUEST_THREADS && !any; i++) {
        if (!g_threads[i].used || g_threads[i].tid != tid)
            continue;
        for (uint8_t k = g_threads[i].rt_head; k != g_threads[i].rt_tail;
             k = (uint8_t)((k + 1) % RT_QUEUE_DEPTH)) {
            int s = g_threads[i].rt[k];
            if (!(s >= 1 && s <= 64 && (blocked >> (s - 1)) & 1)) { any = true; break; }
        }
        break;
    }
    threads_unlock();
    return any;
}

// Pops the next realtime signal queued for the CALLING thread, or 0.
int lxrt_rt_dequeue_self(void)
{
    int tid = lxrt_gettid();
    int sig = 0;
    threads_lock();
    for (int i = 0; i < MAX_GUEST_THREADS; i++) {
        if (!g_threads[i].used || g_threads[i].tid != tid)
            continue;
        if (g_threads[i].rt_head != g_threads[i].rt_tail) {
            sig = g_threads[i].rt[g_threads[i].rt_head];
            g_threads[i].rt_head = (uint8_t)((g_threads[i].rt_head + 1) % RT_QUEUE_DEPTH);
        }
        break;
    }
    threads_unlock();
    return sig;
}
