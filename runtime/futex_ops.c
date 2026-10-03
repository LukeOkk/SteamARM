// The rest of the futex family: requeue, WAKE_OP, the process-shared forms of
// wait and wake, and the PI operations.
//
// thread.c answers four of the thirteen futex operations (WAIT, WAKE and their
// BITSET forms) and returns -ENOSYS for the rest. That gap is expensive:
// benchmarks/stage6-steam-gap.txt measures futex at 50210 calls per 10 s across
// Steam's process tree and names CMP_REQUEUE second on the list of what to
// implement next, because glibc's pthread_cond_broadcast is built on it.
//
// The obstacle is that Darwin has no requeue. __ulock_wait/__ulock_wake are a
// wait queue keyed by address, and there is no operation -- public or not --
// that MOVES a sleeper from one address to another; the ulock hash buckets are
// private to the kernel and there is nothing below __ulock_* to reach them
// with. So the requeue operations are emulated by WAKING the waiters Linux
// would have moved. requeue_emulate() below states exactly what that costs and
// what it is not.
//
// Everything here is a filter in front of lxrt_futex(): an op this module does
// not own comes back as LXRT_FUTEX_NOT_HANDLED so the caller falls through.
//
// Every Linux-side number and behaviour cited below was MEASURED against a real
// Linux 6.17.1-300.fc43.aarch64 guest over ssh rather than recalled, and what
// the probe printed is quoted at the point it decided something. The Darwin
// side was measured on this machine the same way. "MEASURED" in a comment here
// means a program was run and this is what it printed; where it says Linux
// returns a number, that number came back from the guest.

#include "futex_ops.h"

#include "lxrt.h"
#include <pthread.h>

#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define LERR(e) (-lxrt_errno_to_linux(e))

// Declared here rather than in lxrt.h because this module may not edit the
// shared header; thread.c carries the same three lines for the same reason.
// These are not public Darwin API, but os_unfair_lock and libc++ are built on
// them and they are the only compare-and-wait primitive on the system.
extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value,
                        uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);

extern bool lxrt_trace_on(void);
int lxrt_gettid(void);

// ------------------------------------------- ulock flavours, and why two
//
// A ulock queue is keyed by its FLAVOUR as well as by its address, and the two
// compare-and-wait flavours use different keys:
//
//   UL_COMPARE_AND_WAIT        (1)  key = {task, address}      -- task-local
//   UL_COMPARE_AND_WAIT_SHARED (3)  key = {VM object, offset}  -- crosses tasks
//
// MEASURED on this machine, across a real fork(2): a parent maps
// MAP_ANON|MAP_SHARED, forks, the child parks on the page with flavour 1, the
// parent wakes with flavour 1 -- the wake returns -ENOENT and the child sleeps
// to its timeout (-60). The identical run with flavour 3 on both ends wakes the
// child. A wake issued on the wrong flavour is not a slow wake, it is no wake
// at all, in either direction: parking on 1 and waking with 3, and the reverse,
// both lose the wakeup.
//
// So FUTEX_PRIVATE_FLAG is not free here. The previous version of this file
// hardcoded flavour 1 and stripped the flag, which meant a guest
// pthread_cond_broadcast on a PTHREAD_PROCESS_SHARED condvar in shm returned
// "nobody was waiting" having left every waiter in every other process asleep.
// Measured against that version: the cross-process CMP_REQUEUE above returned
// 0 and the child timed out. A hang, reported as success.
//
// Both ends have to pick the same flavour or the queues never meet, so this
// module owns BOTH ends of a process-shared futex: the non-private forms of
// WAIT, WAKE, WAIT_BITSET and WAKE_BITSET are answered here on flavour 3 and
// the private forms are still thread.c's on flavour 1. That split is Linux's
// own -- a private key and a shared key never match there either, which is why
// a guest that waits private and wakes shared hangs on real Linux too.
//
// Flavour 3 was checked against every kind of memory a guest futex word can
// live in -- stack, bss, malloc heap, MAP_PRIVATE|MAP_ANON, MAP_SHARED|MAP_ANON
// and a MAP_SHARED file mapping -- and it woke in all six, in-process as well
// as across a fork. So nothing is given up by using it for every shared futex
// rather than trying to detect which ones are really cross-process, and there
// is no region-inspection heuristic here to get wrong.
#define UL_COMPARE_AND_WAIT        1
#define UL_COMPARE_AND_WAIT_SHARED 3
#define ULF_NO_ERRNO               0x01000000u
#define ULF_WAKE_ALL               0x00000100u

// __ulock_wait takes an unsigned 32-bit microsecond timeout in which 0 means
// "no timeout", so a wait longer than this has to be split into chunks.
#define ULOCK_MAX_US 0xfffffffeu

// ------------------------------------------------------- Linux constants
//
// Every value below was read out of the guest's own uapi/linux/futex.h over
// ssh, not out of a sysroot and not out of memory. Darwin has no counterpart
// for any of them: __ulock_* takes an entirely different operation space
// (UL_COMPARE_AND_WAIT = 1, UL_UNFAIR_LOCK = 2, ULF_WAKE_ALL = 0x100 ...), so
// these are pure guest-side numbers and the only divergence risk is against
// Linux itself, not against a Darwin constant that happens to share the low
// range.

#define FUTEX_WAIT               0
#define FUTEX_WAKE               1
#define FUTEX_REQUEUE            3
#define FUTEX_CMP_REQUEUE        4
#define FUTEX_WAKE_OP            5
#define FUTEX_LOCK_PI            6
#define FUTEX_UNLOCK_PI          7
#define FUTEX_TRYLOCK_PI         8
#define FUTEX_WAIT_BITSET        9
#define FUTEX_WAKE_BITSET       10
#define FUTEX_WAIT_REQUEUE_PI   11
#define FUTEX_CMP_REQUEUE_PI    12
#define FUTEX_LOCK_PI2          13

#define FUTEX_PRIVATE_FLAG     128
#define FUTEX_CLOCK_REALTIME   256
// 512 and 1024 are new in the robust-futex rework and are absent from the
// sysroot copy of this header the constants were first taken from; the guest's
// own header has them, and its FUTEX_CMD_MASK is
// ~(PRIVATE|CLOCK_REALTIME|ROBUST_UNLOCK|ROBUST_LIST32). They are modifiers on
// WAKE, WAKE_BITSET and UNLOCK_PI meaning "also clear the robust list's pending
// entry", with LIST32 selecting the 32-bit list layout.
#define FUTEX_ROBUST_UNLOCK    512
#define FUTEX_ROBUST_LIST32   1024

#define FUTEX_BITSET_MATCH_ANY 0xffffffffu

// FUTEX_WAKE_OP packs its whole instruction into val3:
//
//   bits 31..28  op     (bit 31, FUTEX_OP_OPARG_SHIFT << 28, is a modifier)
//   bits 27..24  cmp
//   bits 23..12  oparg  (12 bits, SIGNED)
//   bits 11..0   cmparg (12 bits, SIGNED)
//
// The op field is four bits wide in the FUTEX_OP() macro userspace builds with,
// but the kernel decodes only three of them (encoded & 0x70000000) and treats
// the fourth, 0x8, as "oparg is a shift count, use 1 << oparg". Reading the op
// as a full nibble is therefore wrong for exactly the shifted forms and right
// for everything else -- the late-failure shape this project keeps meeting.
#define FUTEX_OP_SET    0
#define FUTEX_OP_ADD    1
#define FUTEX_OP_OR     2
#define FUTEX_OP_ANDN   3
#define FUTEX_OP_XOR    4

#define FUTEX_OP_CMP_EQ 0
#define FUTEX_OP_CMP_NE 1
#define FUTEX_OP_CMP_LT 2
#define FUTEX_OP_CMP_LE 3
#define FUTEX_OP_CMP_GT 4
#define FUTEX_OP_CMP_GE 5

// The guest is aarch64, so its struct __kernel_timespec is two signed 64-bit
// fields with no padding and no 32-bit compat form to worry about.
struct guest_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

#define NS_PER_S 1000000000LL
// Linux does not reject a seconds field too large for a ktime_t, it saturates:
// ktime_set() returns KTIME_MAX at and above KTIME_SEC_MAX. A deadline past
// that is therefore an untimed wait on Linux, and is one here too, which also
// keeps the nanosecond arithmetic below inside int64_t.
#define GUEST_SEC_MAX (INT64_MAX / NS_PER_S)

// The guest's futex word and ours are the same memory: there is no VM here, so
// a C11 atomic on this address is the same LDXR/STXR or LSE instruction pair
// the guest's own libc emits on it, on the same core, under the same memory
// model. That is what makes FUTEX_WAKE_OP implementable at all. It only holds
// while the atomic is lock-free -- a libatomic fallback would take a side lock
// the guest knows nothing about and the "atomically" in the Linux contract
// would quietly become a lie.
_Static_assert(ATOMIC_INT_LOCK_FREE == 2,
               "32-bit atomics must be lock-free to be atomic against guest code");

// ------------------------------------------------------ guest pointers
//
// Darwin's wake primitive does not validate addresses: measured on this
// machine, __ulock_wake() on an unmapped page returns -ENOENT ("no waiters"),
// the same answer as a live but empty queue, and __ulock_wake(NULL) returns
// -EINVAL. Linux returns EFAULT for both -- measured on the guest, FUTEX_WAKE
// on an unmapped address and on NULL are both -14. Passing a bad guest pointer
// through would therefore turn a guest bug into a silent 0 return rather than
// an error, and any address this module dereferences itself -- CMP_REQUEUE's
// compare word, WAKE_OP's target, a WAIT timeout -- would fault the host
// process instead.
//
// So the mapping is checked up front. mach_vm_region costs 458 ns against
// __ulock_wake's 111 ns (200k iterations each, this machine), and each of these
// operations pays for two of them, which is a real multiple but not a real
// cost: at the 5021 futex calls per second stage6-steam-gap.txt measures, two
// probes on every one of them would be under half a percent of one core, and
// only a fraction of those calls reach this file at all. That is cheap enough
// to pay unconditionally, so there is no region cache here and therefore no
// window in which a cached region has already been unmapped.
//
// What the check is not: it is a test, not a lock. A guest that munmaps a futex
// word concurrently with a wake on it still faults the host. Closing that needs
// a SIGSEGV handler, and SIGSEGV already belongs to main.c's fault reporter and
// signal.c's guest delivery path -- a third claimant there would break guest
// signal handling to fix a case a correct guest never reaches.
static bool guest_range_ok(const void *p, size_t len, vm_prot_t need)
{
    if (!p)
        return false;

    mach_vm_address_t want = (mach_vm_address_t)(uintptr_t)p;
    mach_vm_address_t addr = want;
    mach_vm_size_t size = 0;
    vm_region_basic_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;

    if (mach_vm_region(mach_task_self(), &addr, &size, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&info, &count, &obj) != KERN_SUCCESS)
        return false;
    // Measured MACH_PORT_NULL for every VM_REGION_BASIC_INFO_64 query (20000
    // iterations, port count unchanged), but a leak on this path would be
    // unbounded if a future kernel started handing one back.
    if (obj != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), obj);

    // mach_vm_region answers with the first region at OR ABOVE the address it
    // is given, so an unmapped address comes back as KERN_SUCCESS describing
    // the next mapping up. Without this test every hole in the address space
    // reads as valid.
    if (want < addr || want + len > addr + size)
        return false;
    return (info.protection & need) == need;
}

static bool guest_word_ok(const void *p, vm_prot_t need)
{
    return guest_range_ok(p, sizeof(uint32_t), need);
}

// Linux rejects a futex word that is not 4-byte aligned with EINVAL, in
// get_futex_key, for every operation -- measured -22 on the guest for WAIT,
// CMP_REQUEUE and WAKE_OP, on uaddr and on uaddr2 alike. Darwin agrees for
// waits -- measured __ulock_wait(unaligned) = -EINVAL -- but not for wakes:
// __ulock_wake on the same unaligned address returns -ENOENT, which this module
// would otherwise report to the guest as "zero waiters woken" instead of an
// error.

// ------------------------------------------- which futexes really cross tasks
//
// Linux keys a "shared" futex on PRIVATE memory by {mm, address}; only a
// futex in a MAP_SHARED / SysV-shm mapping is keyed by the backing object.
// The ulock flavour 3 key is always {VM object, offset}, and after a fork a
// private page becomes copy-on-write: the parent's next store moves it to a
// new object, so a waiter parked before the fork and a waker after it no
// longer meet (measured: pthread_join -- glibc waits on the tid with a
// SHARED futex -- never returned for a thread that had forked,
// tests/elf/jit_fork_mt.c; the i386 Steam client). So flavour 3 is used only
// inside the mappings this runtime created as shared; everything else is
// task-local, flavour 1, exactly Linux's split.
#define MAX_SHARED_RANGES 1024
static struct { uint64_t start, end; } g_shared[MAX_SHARED_RANGES];
static int g_nshared;
static pthread_mutex_t g_shared_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(futex_shared, g_shared_lock)

void lxrt_futex_shared_add(uint64_t addr, uint64_t len)
{
    if (!len) return;
    pthread_mutex_lock(&g_shared_lock);
    if (g_nshared < MAX_SHARED_RANGES) {
        g_shared[g_nshared].start = addr;
        g_shared[g_nshared].end = addr + len;
        g_nshared++;
    }
    pthread_mutex_unlock(&g_shared_lock);
}

void lxrt_futex_shared_remove(uint64_t addr, uint64_t len)
{
    uint64_t end = addr + len;
    pthread_mutex_lock(&g_shared_lock);
    for (int i = 0; i < g_nshared; ) {
        uint64_t s = g_shared[i].start, e = g_shared[i].end;
        if (e <= addr || s >= end) { i++; continue; }
        if (s < addr && e > end && g_nshared < MAX_SHARED_RANGES) {   // split
            g_shared[g_nshared].start = end;
            g_shared[g_nshared].end = e;
            g_nshared++;
            g_shared[i].end = addr;
            i++;
        } else if (s < addr) {
            g_shared[i].end = addr; i++;
        } else if (e > end) {
            g_shared[i].start = end; i++;
        } else {
            g_shared[i] = g_shared[--g_nshared];
        }
    }
    pthread_mutex_unlock(&g_shared_lock);
}

static bool truly_shared(const void *p)
{
    uint64_t a = (uint64_t)(uintptr_t)p;
    bool found = false;
    pthread_mutex_lock(&g_shared_lock);
    for (int i = 0; i < g_nshared && !found; i++)
        found = a >= g_shared[i].start && a < g_shared[i].end;
    pthread_mutex_unlock(&g_shared_lock);
    return found;
}

static bool aligned(const void *p)
{
    return ((uintptr_t)p % sizeof(uint32_t)) == 0;
}

static int64_t now_ns(bool realtime)
{
    struct timespec t;
    clock_gettime(realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * NS_PER_S + (int64_t)t.tv_nsec;
}

// ---------------------------------------------------------------- waking
//
// Every operation in this file returns a count of waiters released, and Darwin
// will not supply one. Worse, the count cannot be recovered by waking one at a
// time and stopping at -ENOENT, which was this module's first design. Measured
// here: eight threads parked on one address, then __ulock_wake in a tight loop
// until it reported -ENOENT -- it reported SUCCESS 123 TIMES, and exactly eight
// threads woke. A ulock keeps reporting success until the threads it has
// already released are scheduled and remove themselves from the queue, so the
// loop spends its extra iterations waking sleepers that are already awake. The
// identical loop paced at 1 ms per iteration reported success exactly 8 times.
// A count obtained by looping therefore measures scheduling latency, not
// waiters, and it is wrong in the dangerous direction: too high.
//
// So the count Linux defines is not reproducible, and the rule adopted instead
// is to never claim a waiter that was not released -- one wake syscall per
// group, and an answer that is a lower bound:
//
//   budget 0    -> 0, no syscall
//   budget 1    -> one wake; 1 if Darwin released a sleeper, else 0
//   budget > 1  -> one ULF_WAKE_ALL; 1 if the queue was not empty, else 0
//
// What that under-reports: a broadcast that releases eight waiters returns 1
// where Linux returns 8. A low count is a value Linux itself can return -- it
// is what the caller would have seen with fewer waiters -- while an inflated
// one is not, and callers that read the value at all read it as "was anybody
// there". glibc's condvar ignores it. thread.c's FUTEX_WAKE already answers
// this way for val > 1, so the runtime is at least consistent with itself.
//
// What ULF_WAKE_ALL over-does: for 1 < budget < waiters it releases every
// sleeper instead of `budget` of them. The extras re-check their predicate and
// park again, the same spurious wakeup the requeue emulation below already
// imposes on everything it touches.
//
// The one inaccuracy left in the other direction: if another thread issued a
// wake on this same address a moment ago, the queue can still report a sleeper
// that has already been released, so a call with no real waiters can return 1.
// Nothing in userspace can tell those apart.
//
// The fallback in the shared case: a shared wake goes to flavour 3, where
// shared_wait() below parks its waiters, and only if that queue is empty does
// it also try the task-local queue. That second call matters when dispatch.c
// has NOT been wired to offer WAIT to this module (see lxrt_futex_ext), because
// then a shared waiter is parked by thread.c on flavour 1 and a flavour-3-only
// wake would turn today's working in-process broadcast into a hang. Its cost is
// one extra 111 ns syscall on shared wakes that found nobody, and its one
// divergence is that a PRIVATE waiter on the same address can be released by a
// SHARED wake, which Linux would not do -- an extra wakeup, which is the
// direction every caller here already has to tolerate.
static int wake_up_to(void *addr, int64_t budget, bool shared)
{
    if (budget <= 0)
        return 0;
    lxrt_futex_waitv_notify(addr);      // futex_waitv callers parked on this word
    uint32_t flags = ULF_NO_ERRNO;
    if (budget > 1)
        flags |= ULF_WAKE_ALL;

    // -ENOENT is an empty queue. -EINVAL would be a malformed address, already
    // ruled out by the caller. Neither released anything.
    uint32_t first = (shared && truly_shared(addr)) ? UL_COMPARE_AND_WAIT_SHARED : UL_COMPARE_AND_WAIT;
    if (__ulock_wake(flags | first, addr, 0) == 0)
        return 1;
    if (shared && first == UL_COMPARE_AND_WAIT_SHARED && __ulock_wake(flags | UL_COMPARE_AND_WAIT, addr, 0) == 0)
        return 1;
    return 0;
}

// ------------------------------------------- FUTEX_WAIT / FUTEX_WAKE
//
// Only the process-shared forms. The private ones stay in thread.c, which is
// right where they are: they are the hot path, they already work, and a second
// implementation of them here would be a second thing to keep in step.
//
// What the shared form needs that thread.c cannot give it is flavour 3 on the
// parking side, so that the wakes this file issues for a shared futex -- from
// here, from requeue_emulate() and from wake_op() -- land on the queue the
// waiters are actually on even when the waiter is in another process.

static long shared_wait(uint32_t *uaddr, uint32_t val, uint64_t utime,
                        uint32_t bitset, bool bitset_form, bool realtime)
{
    if (!aligned(uaddr))
        return LERR(EINVAL);
    if (!guest_word_ok(uaddr, VM_PROT_READ))
        return LERR(EFAULT);
    // Measured on the guest: FUTEX_WAIT_BITSET and FUTEX_WAKE_BITSET both
    // return -22 for a zero bitset, and -11/0 for a non-zero one.
    if (bitset_form && bitset == 0)
        return LERR(EINVAL);

    // A partial bitset is honoured by Linux -- measured: a waiter with bitset
    // 0x1 is NOT released by a wake with bitset 0x2 -- and cannot be honoured
    // here, because a ulock queue has no bitset and no way to add one. The
    // divergence is deliberately left in the safe direction: this file wakes a
    // waiter Linux would have skipped rather than skipping one Linux would have
    // woken, so a guest sees a spurious wakeup (which every futex user must
    // already re-check for) instead of a lost one (which hangs). Refusing
    // partial bitsets with ENOSYS was the alternative and is worse: glibc never
    // emits one, so the only programs it would break are the ones that work.

    bool timed = false;
    bool deadline_realtime = false;
    int64_t deadline = 0;
    if (utime) {
        const struct guest_timespec *ts =
            (const struct guest_timespec *)(uintptr_t)utime;
        if (!guest_range_ok(ts, sizeof(*ts), VM_PROT_READ))
            return LERR(EFAULT);
        int64_t sec = ts->tv_sec;
        int64_t nsec = ts->tv_nsec;
        // timespec64_valid(). Measured on the guest: {-1, 0} -> -22 and
        // {0, 1000000000} -> -22, while {0, 0} is valid and comes straight back
        // as -110 ETIMEDOUT.
        if (sec < 0 || nsec < 0 || nsec >= NS_PER_S)
            return LERR(EINVAL);
        if (sec < GUEST_SEC_MAX) {
            timed = true;
            if (bitset_form) {
                // FUTEX_WAIT_BITSET's timeout is ABSOLUTE, on CLOCK_REALTIME
                // when the guest asked for it and CLOCK_MONOTONIC otherwise.
                deadline = sec * NS_PER_S + nsec;
                deadline_realtime = realtime;
            } else {
                // FUTEX_WAIT's is RELATIVE and always CLOCK_MONOTONIC --
                // FUTEX_WAIT | FUTEX_CLOCK_REALTIME is -38 on Linux, measured,
                // and never reaches here.
                int64_t rel = sec * NS_PER_S + nsec;
                int64_t base = now_ns(false);
                if (rel > INT64_MAX - base)
                    timed = false;          // saturates, as above
                else
                    deadline = base + rel;
            }
        }
    }

    // Linux compares *uaddr against val under the futex hash-bucket lock and
    // returns EAGAIN on a mismatch -- measured -11. __ulock_wait compares too,
    // but MEASURED on this machine it returns 0, not -EAGAIN, when the word
    // does not match -- on both flavours -- which a guest would read as a
    // spurious wakeup rather than as the error Linux documents. So the
    // compare is made here as well and the common case reports what Linux
    // reports. The residual race -- the word changes between this load and the
    // kernel's own compare -- still comes back as 0, a wakeup every futex
    // caller already has to re-check for.
    uint32_t cur = atomic_load_explicit((_Atomic uint32_t *)uaddr,
                                        memory_order_seq_cst);
    if (cur != val)
        return LERR(EAGAIN);

    uint32_t flavour = (truly_shared(uaddr) ? UL_COMPARE_AND_WAIT_SHARED : UL_COMPARE_AND_WAIT) | ULF_NO_ERRNO;
    for (;;) {
        uint32_t us = 0;   // 0 is "no timeout" to __ulock_wait
        if (timed) {
            int64_t left = deadline - now_ns(deadline_realtime);
            if (left <= 0)
                return LERR(ETIMEDOUT);
            // Round up: a wait that returns early is a wait that reports
            // ETIMEDOUT before the deadline the guest asked for.
            int64_t left_us = (left + 999) / 1000;
            us = left_us > (int64_t)ULOCK_MAX_US ? ULOCK_MAX_US
                                                 : (uint32_t)left_us;
        }

        // Counted as parked for thread.c's private wakes, which skip the
        // kernel when nobody of this process is (g_parked there): glibc
        // waits with the shared form on words that are woken privately.
        lxrt_futex_park_enter(uaddr);
        if (atomic_load_explicit((_Atomic uint32_t *)uaddr, memory_order_seq_cst) != val) {
            lxrt_futex_park_leave(uaddr);
            return LERR(EAGAIN);
        }
        int r = __ulock_wait(flavour, uaddr, val, us);
        lxrt_futex_park_leave(uaddr);
        if (r >= 0)
            return 0;
        // A chunk expiring is not the deadline expiring: a timeout beyond
        // ULOCK_MAX_US (about 71.5 minutes) has to be split, and the loop
        // re-derives what is left. Under that limit this runs exactly once, so
        // the re-park window -- during which a wake would be missed -- exists
        // only for waits longer than 71 minutes, and at most once per 71
        // minutes of one. thread.c's alternative for the same case is to drop
        // the timeout entirely and wait forever.
        if (-r == ETIMEDOUT && timed && us == ULOCK_MAX_US)
            continue;
        // Interrupted by a signal that ran no guest handler: Linux would
        // still be waiting. The loop re-derives what is left of the deadline.
        if (-r == EINTR && lxrt_interrupted_internally())
            continue;
        // __ulock_wait with ULF_NO_ERRNO returns a negative DARWIN errno.
        return LERR(-r);
    }
}

static long shared_wake(uint32_t *uaddr, uint32_t val, uint32_t bitset,
                        bool bitset_form)
{
    if (!aligned(uaddr))
        return LERR(EINVAL);
    if (!guest_word_ok(uaddr, VM_PROT_READ))
        return LERR(EFAULT);
    if (bitset_form && bitset == 0)
        return LERR(EINVAL);

    // futex_wake()'s loop tests its budget only AFTER releasing a waiter
    // (`if (++ret >= nr_wake) break;`), so a budget of 0 or a negative one
    // still releases exactly one. MEASURED on the guest: FUTEX_WAKE with val=0
    // and one waiter parked returns 1 and the waiter runs.
    int32_t nr = (int32_t)val;
    // No bitset in a ulock queue: a targeted wake of one could release the
    // wrong class of waiter (see thread.c, FUTEX_WAKE_BITSET). Wake them all.
    if (bitset_form && bitset != FUTEX_BITSET_MATCH_ANY)
        nr = INT32_MAX;
    return wake_up_to(uaddr, nr <= 0 ? 1 : nr, true);
}

// ------------------------------------------------- FUTEX_(CMP_)REQUEUE
//
// Linux's contract: optionally check *uaddr == val3 (EAGAIN if not), wake
// `nr_wake` waiters on uaddr, and MOVE up to `nr_requeue` of the remaining
// waiters to uaddr2 without waking them. The point of the move is that a
// broadcast on a condvar does not release N threads to fight over one mutex; it
// releases one and parks the other N-1 directly on the mutex they are about to
// need.
//
// This wakes them instead. What that IS: every waiter Linux would have moved
// returns from its FUTEX_WAIT with 0, re-checks its predicate -- which every
// correct futex user does, because Linux permits spurious wakeups -- and blocks
// again on uaddr2 by itself. glibc's condvar waiters do exactly that; they come
// out of the wait and call pthread_mutex_lock, which is the same futex they
// would have been requeued onto.
//
// What it is NOT: it is not free and it is not invisible. The N-1 threads take
// a wakeup, a scheduling slot and a failed mutex acquisition each, which is the
// thundering herd requeue exists to prevent. On a broadcast to 64 waiters that
// is 63 wasted wakeups. It is a performance regression against Linux, not a
// correctness one -- but a guest that broadcasts in a tight loop will feel it.
//
// Atomicity of the compare, precisely:
//
//   * The load is one seq_cst 32-bit atomic load of the same memory the guest
//     stores to, so it cannot tear and cannot be stale. A mismatch returns
//     EAGAIN having woken nobody, which is exactly Linux -- measured -11 for a
//     val3 mismatch and 0 for a match.
//   * The wakes are issued after the load, and Darwin removes a waiter from a
//     ulock queue only when it is woken or times out. So every waiter enqueued
//     at the instant of the load is still there when the wake runs. This
//     emulation therefore never wakes FEWER waiters than Linux would.
//   * What is lost: Linux holds the futex hash-bucket lock across the compare
//     and the wake, which serialises them against other futex SYSCALLS on the
//     same word (it was never a lock against the guest's own plain stores --
//     that is why the condvar protocol bumps the word before requeueing and why
//     a late waiter fails its own compare). Our compare runs in userspace and
//     the waiter's compare runs inside Darwin's kernel under Darwin's lock, so
//     those two are not serialised against each other. The residual window is:
//     the guest changes *uaddr and a new waiter enqueues, both after our load
//     but before our wake, and we wake a thread Linux would have left alone.
//     The divergence is one-sided -- extra wakeups, never missing ones -- and
//     a caller that could not survive an extra wakeup could not survive this
//     emulation's core behaviour either.
static long requeue_emulate(uint32_t *uaddr, uint32_t *uaddr2, uint32_t val,
                            uint64_t val2, uint32_t val3, bool compare,
                            bool shared)
{
    // The fourth syscall argument is the `timeout` pointer slot, and for the
    // requeue and WAKE_OP operations the kernel reinterprets it as a count with
    // `val2 = (u32)(unsigned long)utime`. The upper 32 bits are DISCARDED, not
    // rejected, so a guest passing a 64-bit garbage value gets its low half
    // used; truncating here rather than testing the 64-bit value keeps that
    // behaviour instead of inventing an error Linux does not return.
    int32_t nr_wake = (int32_t)val;
    int32_t nr_requeue = (int32_t)(uint32_t)val2;

    // Both counts are `int` on the kernel side, so 0xffffffff arrives as -1 and
    // is rejected rather than read as a four-billion-waiter budget. Unlike
    // futex_wake_op(), futex_requeue() really does open with this check --
    // MEASURED on the guest: CMP_REQUEUE with nr_wake=-1 and with nr_requeue=-1
    // are both -22, while the same negative counts on WAKE_OP are not errors at
    // all (see wake_op below).
    if (nr_wake < 0 || nr_requeue < 0)
        return LERR(EINVAL);
    if (!aligned(uaddr) || !aligned(uaddr2))
        return LERR(EINVAL);
    // Linux takes a futex key for uaddr2 as well, even though a plain requeue
    // never reads it, so a bad uaddr2 is EFAULT there too -- measured -14.
    if (!guest_word_ok(uaddr, VM_PROT_READ) ||
        !guest_word_ok(uaddr2, VM_PROT_READ))
        return LERR(EFAULT);

    if (compare) {
        uint32_t cur = atomic_load_explicit((_Atomic uint32_t *)uaddr,
                                            memory_order_seq_cst);
        // Unsigned equality: Linux compares the raw u32, and a futex word with
        // its top bit set (FUTEX_WAITERS is 0x80000000) is ordinary.
        if (cur != val3)
            return LERR(EAGAIN);
    }

    // Both groups come off the same queue, so one budget covers them: the first
    // nr_wake releases are the ones Linux would have woken, the rest the ones it
    // would have moved. Linux reports them as a single number -- futex_requeue()
    // returns task_count, which is incremented in both arms -- so the sum is the
    // right quantity to ask for even though what comes back is the lower bound
    // the comment above wake_up_to() explains. (The man page says FUTEX_REQUEUE
    // "returns the number of waiters that were woken up"; the kernel runs both
    // operations through the same function and returns the combined count for
    // each, so that sentence describes the deprecated intent rather than the
    // code.)
    //
    // A zero budget really is zero here, unlike in wake_op: futex_requeue()'s
    // loop tests `task_count - nr_wake >= nr_requeue` BEFORE touching a waiter,
    // so 0 and 0 release nobody.
    int n = wake_up_to(uaddr, (int64_t)nr_wake + (int64_t)nr_requeue, shared);

    if (lxrt_trace_on() && n > 0 && nr_requeue > 0)
        fprintf(lxrt_trace_stream(), "[lxrt] futex requeue: woke the queue on %p instead of "
                        "moving up to %d of it to %p\n",
                (void *)uaddr, nr_requeue, (void *)uaddr2);
    return n;
}

// ------------------------------------------------------- FUTEX_WAKE_OP

static int32_t sign_extend_12(uint32_t v)
{
    // Defined for every input, unlike a cast through a signed shift: the field
    // is biased into [0, 0xfff] and then unbiased.
    return (int32_t)((v & 0xfffu) ^ 0x800u) - 0x800;
}

static long wake_op(uint32_t *uaddr, uint32_t val, uint64_t val2,
                    uint32_t *uaddr2, uint32_t encoded, bool shared)
{
    int32_t nr_wake = (int32_t)val;
    int32_t nr_wake2 = (int32_t)(uint32_t)val2;   // same truncation as requeue

    if (!aligned(uaddr) || !aligned(uaddr2))
        return LERR(EINVAL);
    // uaddr is only woken, but uaddr2 is read-modify-written, so it needs write
    // permission -- a futex word in a region the guest mapped read-only is
    // EFAULT on Linux and a host SIGSEGV here without this test.
    if (!guest_word_ok(uaddr, VM_PROT_READ) ||
        !guest_word_ok(uaddr2, VM_PROT_READ | VM_PROT_WRITE))
        return LERR(EFAULT);

    // This function used to reject a negative nr_wake/nr_wake2 with EINVAL, by
    // generalising futex_requeue()'s check to a function that does not have
    // one. MEASURED on the guest: FUTEX_WAKE_OP with nr_wake = -1 returns 0 on
    // an empty queue and 1 with a waiter parked, and the waiter really runs;
    // nr_wake = 0 behaves identically, because futex_wake_op()'s loop tests the
    // budget only after releasing a waiter (`if (++ret >= nr_wake) break;`).
    // nr_wake2 measures the same. So a budget of 0 or less is a budget of one,
    // not an error and not nothing.
    int64_t budget = nr_wake <= 0 ? 1 : nr_wake;
    int64_t budget2 = nr_wake2 <= 0 ? 1 : nr_wake2;

    unsigned op = (encoded & 0x70000000u) >> 28;
    unsigned cmp = (encoded & 0x0f000000u) >> 24;
    int32_t oparg = sign_extend_12((encoded & 0x00fff000u) >> 12);
    int32_t cmparg = sign_extend_12(encoded & 0x00000fffu);

    if (encoded & 0x80000000u) {          // FUTEX_OP_OPARG_SHIFT << 28
        // Linux 4.15 briefly made an out-of-range shift count -EINVAL; current
        // kernels log "tries to shift op by %d; fix this program" and mask with
        // & 31 instead, which is also what every kernel before 4.15 did.
        // Masking is what is implemented: it is the behaviour at both ends of
        // the version range, and inventing an EINVAL a guest's kernel would not
        // have returned breaks a program that works on Linux. MEASURED on the
        // guest: a shift of 33 sets 0x2 and a shift of -1 sets 0x80000000,
        // which is & 31 in both directions.
        //
        // Shifted through unsigned because a shift count of 31 is the useful
        // one -- it is how a waiters flag at 0x80000000 is set -- and `1 << 31`
        // on a signed int is an overflow the standard does not define.
        oparg = (int32_t)(1u << ((uint32_t)oparg & 31u));
    }

    _Atomic uint32_t *target = (_Atomic uint32_t *)uaddr2;
    uint32_t oldval;

    // The op is validated BEFORE the memory is touched; an unknown one is
    // -ENOSYS, not -EINVAL, and leaves *uaddr2 alone. Measured: op 7 with
    // oparg 5 returns -38 and leaves the word at 0.
    switch (op) {
    case FUTEX_OP_SET:
        oldval = atomic_exchange_explicit(target, (uint32_t)oparg,
                                          memory_order_seq_cst);
        break;
    case FUTEX_OP_ADD:
        // Wrapping is the point: oparg is signed and a negative one is an
        // atomic subtract, which is how a lock word is released.
        oldval = atomic_fetch_add_explicit(target, (uint32_t)oparg,
                                           memory_order_seq_cst);
        break;
    case FUTEX_OP_OR:
        oldval = atomic_fetch_or_explicit(target, (uint32_t)oparg,
                                          memory_order_seq_cst);
        break;
    case FUTEX_OP_ANDN:
        // ANDN is AND NOT: *uaddr2 &= ~oparg. Reading it as a plain AND is a
        // one-character mistake that inverts the meaning of every bit.
        oldval = atomic_fetch_and_explicit(target, ~(uint32_t)oparg,
                                           memory_order_seq_cst);
        break;
    case FUTEX_OP_XOR:
        oldval = atomic_fetch_xor_explicit(target, (uint32_t)oparg,
                                           memory_order_seq_cst);
        break;
    default:
        return LERR(ENOSYS);
    }

    // The comparison is SIGNED -- the kernel holds oldval in an int and
    // sign-extends cmparg out of a 12-bit field. glibc's lock words use the top
    // bit as a waiters flag, so a word like 0x80000001 is a large positive
    // number under an unsigned compare and a negative one under Linux's: a
    // FUTEX_OP_CMP_GT that should fail would succeed and wake the wrong queue.
    // Measured on the guest: CMP_GT 0 against an old value of 0x80000001 does
    // not wake.
    int32_t old = (int32_t)oldval;
    bool wake2;
    switch (cmp) {
    case FUTEX_OP_CMP_EQ: wake2 = old == cmparg; break;
    case FUTEX_OP_CMP_NE: wake2 = old != cmparg; break;
    case FUTEX_OP_CMP_LT: wake2 = old <  cmparg; break;
    case FUTEX_OP_CMP_LE: wake2 = old <= cmparg; break;
    case FUTEX_OP_CMP_GT: wake2 = old >  cmparg; break;
    case FUTEX_OP_CMP_GE: wake2 = old >= cmparg; break;
    default:
        // Linux validates the comparison only after the atomic has already
        // been applied, so the write above stands and no wake happens. Matched
        // deliberately: a guest that retries after this error must see the same
        // futex word it would see on Linux. Measured: op=ADD oparg=5 with an
        // invalid cmp returns -38 AND leaves the word at 5.
        return LERR(ENOSYS);
    }

    int n = wake_up_to(uaddr, budget, shared);
    if (wake2)
        n += wake_up_to(uaddr2, budget2, shared);
    return n;
}

// -------------------------------------------------------- PI operations
//
// LOCK_PI, LOCK_PI2, TRYLOCK_PI and UNLOCK_PI keep Linux's ownership protocol
// on the futex word -- the owner's TID in bits 0-29, FUTEX_WAITERS (bit 31)
// telling the owner to unlock through the kernel, FUTEX_OWNER_DIED (bit 30)
// left as found -- without the priority inheritance: Darwin has no way to
// tell the scheduler that one thread is blocking another (__ulock_* has an
// unfair-lock flavour but no ownership; thread_policy_set moves a priority
// and knows nothing about who waits on what). gVisor's PI futexes make the
// same trade.
//
// They used to answer ENOSYS, on the grounds that a PI mutex without the
// inheritance can invert priorities under load. The price of that was
// higher: glibc probes PI support once per process with UNLOCK_PI on a free
// word and, on ENOSYS, makes every pthread_mutex_init with
// PTHREAD_PRIO_INHERIT fail with ENOTSUP. PipeWire's event loop takes such a
// mutex, so no PipeWire client could start: wpctl, which Steam runs for its
// audio settings, died at address 0x10 after "can't make support.loop
// handle: Operation not supported" -- 180 times in fifteen Steam starts
// (2026-10-03, /tmp/lxrt-faults.log).
//
// The protocol here:
//   LOCK_PI    a free word (no TID) is taken with FUTEX_WAITERS set -- a
//              thread that came through the kernel has had company, and others
//              may still be parked, so the owner's unlock must come back here;
//              a word this thread owns is EDEADLK; otherwise FUTEX_WAITERS is
//              set and the thread parks on the word until it changes, then
//              tries again. The timeout is absolute: CLOCK_REALTIME for
//              LOCK_PI, CLOCK_MONOTONIC for LOCK_PI2 unless it carries
//              FUTEX_CLOCK_REALTIME. Signals that run no handler do not end
//              the wait (Linux restarts it).
//   TRYLOCK_PI the same take without the wait: EAGAIN (EWOULDBLOCK) if owned.
//   UNLOCK_PI  EPERM unless this thread owns the word; then the word goes to
//              0 and every parked waiter is woken to compete for it: one takes
//              it (setting FUTEX_WAITERS again for the rest), the others park
//              again. Linux instead hands the lock to the top waiter; waking
//              all of them is what keeps a waiter whose timeout expired at the
//              same moment from stranding the rest.
// WAIT_REQUEUE_PI and CMP_REQUEUE_PI stay ENOSYS: glibc's condition variables
// have not used them since 2.25, and pthread_cond_broadcast's attempt falls
// back to a plain wake on any error from them.
#define FUTEX_WAITERS    0x80000000u
#define FUTEX_OWNER_DIED 0x40000000u
#define FUTEX_TID_MASK   0x3fffffffu

static long pi_unsupported(void)
{
    return LERR(ENOSYS);
}

// LXRT_NO_PI=1: the PI operations answer ENOSYS again (glibc then refuses
// PTHREAD_PRIO_INHERIT mutexes and programs fall back to plain ones).
static bool pi_off(void)
{
    static int off = -1;
    if (off < 0)
        off = getenv("LXRT_NO_PI") && *getenv("LXRT_NO_PI") == '1';
    return off;
}

static long pi_lock(uint32_t *uaddr, uint64_t utime, bool realtime, bool shared, bool try_only)
{
    if (!aligned(uaddr))
        return LERR(EINVAL);
    if (!guest_word_ok(uaddr, VM_PROT_READ | VM_PROT_WRITE))
        return LERR(EFAULT);
    bool timed = false;
    int64_t deadline = 0;
    if (utime && !try_only) {
        const struct guest_timespec *ts = (const struct guest_timespec *)(uintptr_t)utime;
        if (!guest_range_ok(ts, sizeof(*ts), VM_PROT_READ))
            return LERR(EFAULT);
        int64_t sec = ts->tv_sec, nsec = ts->tv_nsec;
        if (sec < 0 || nsec < 0 || nsec >= NS_PER_S)
            return LERR(EINVAL);
        if (sec < GUEST_SEC_MAX) {
            timed = true;
            deadline = sec * NS_PER_S + nsec;
        }
    }
    uint32_t tid = (uint32_t)lxrt_gettid() & FUTEX_TID_MASK;
    _Atomic uint32_t *w = (_Atomic uint32_t *)uaddr;
    uint32_t flavour = ((shared && truly_shared(uaddr)) ? UL_COMPARE_AND_WAIT_SHARED : UL_COMPARE_AND_WAIT) | ULF_NO_ERRNO;
    for (;;) {
        uint32_t cur = atomic_load_explicit(w, memory_order_acquire);
        uint32_t owner = cur & FUTEX_TID_MASK;
        if (owner == 0) {
            uint32_t want = tid | (cur & FUTEX_OWNER_DIED) |
                            (try_only ? (cur & FUTEX_WAITERS) : FUTEX_WAITERS);
            if (atomic_compare_exchange_strong_explicit(w, &cur, want, memory_order_acq_rel,
                                                        memory_order_acquire))
                return 0;
            continue;
        }
        if (owner == tid)
            return LERR(EDEADLK);
        if (try_only)
            return LERR(EAGAIN);
        if (!(cur & FUTEX_WAITERS)) {
            uint32_t marked = cur | FUTEX_WAITERS;
            if (!atomic_compare_exchange_strong_explicit(w, &cur, marked, memory_order_acq_rel,
                                                         memory_order_acquire))
                continue;
            cur = marked;
        }
        uint32_t us = 0;
        if (timed) {
            int64_t left = deadline - now_ns(realtime);
            if (left <= 0)
                return LERR(ETIMEDOUT);
            int64_t left_us = (left + 999) / 1000;
            us = left_us > (int64_t)ULOCK_MAX_US ? ULOCK_MAX_US : (uint32_t)left_us;
        }
        lxrt_futex_park_enter(uaddr);
        int r = __ulock_wait(flavour, uaddr, cur, us);
        lxrt_futex_park_leave(uaddr);
        if (r >= 0 || -r == EINTR || -r == ETIMEDOUT)
            continue;           // the word changed, a signal, or a chunk ended: look again
        return LERR(-r);
    }
}

static long pi_unlock(uint32_t *uaddr, bool shared)
{
    if (!aligned(uaddr))
        return LERR(EINVAL);
    if (!guest_word_ok(uaddr, VM_PROT_READ | VM_PROT_WRITE))
        return LERR(EFAULT);
    uint32_t tid = (uint32_t)lxrt_gettid() & FUTEX_TID_MASK;
    _Atomic uint32_t *w = (_Atomic uint32_t *)uaddr;
    uint32_t cur = atomic_load_explicit(w, memory_order_acquire);
    for (;;) {
        if ((cur & FUTEX_TID_MASK) != tid)
            return LERR(EPERM);
        if (atomic_compare_exchange_weak_explicit(w, &cur, 0, memory_order_acq_rel, memory_order_acquire))
            break;
    }
    if (cur & FUTEX_WAITERS)
        wake_up_to(uaddr, INT32_MAX, shared);
    return 0;
}

// ---------------------------------------------------------- entry point

// The flag bits ride in the op word and must come off before the command is
// compared, exactly as the guest's own FUTEX_CMD_MASK does. All four of them:
// the sysroot header this file's constants were first taken from stops at
// FUTEX_CLOCK_REALTIME, but the guest kernel (6.17.1-300.fc43.aarch64, read
// over ssh) also defines FUTEX_ROBUST_UNLOCK (512) and FUTEX_ROBUST_LIST32
// (1024) and masks all four. Masking only 128|256 left an op like
// FUTEX_UNLOCK_PI|FUTEX_ROBUST_UNLOCK (519) matching no case arm at all.
//
// Masking is only half of it -- the flags are stripped from the command, and
// then rejected, because none of them is free:
//
//   FUTEX_PRIVATE_FLAG     selects the ulock flavour; see the flavour comment.
//   FUTEX_CLOCK_REALTIME   MEASURED -38 on the guest for every command in this
//                          file (REQUEUE, CMP_REQUEUE, WAKE_OP, LOCK_PI,
//                          UNLOCK_PI, TRYLOCK_PI, CMP_REQUEUE_PI) and for WAIT,
//                          WAKE and WAKE_BITSET as well. do_futex() allows the
//                          bit on exactly three commands -- WAIT_BITSET,
//                          WAIT_REQUEUE_PI and LOCK_PI2 -- and the last two are
//                          refused here for reasons that have nothing to do
//                          with the clock, so passing it through for
//                          WAIT_BITSET and answering ENOSYS everywhere else
//                          gives the guest the number its own kernel would, on
//                          all thirteen commands. Measured on the guest:
//                          WAIT_BITSET|256 is -11 (the value mismatch, not a
//                          rejection) and LOCK_PI2|256 succeeds outright.
//   FUTEX_ROBUST_*         MEASURED -38 on the guest for every command tried:
//                          UNLOCK_PI|512, UNLOCK_PI|512|1024, CMP_REQUEUE|512,
//                          WAKE_OP|512, REQUEUE|1024, WAKE|512, WAKE|512|1024,
//                          WAIT|512 and WAKE_BITSET|512. They mean "also clear
//                          the robust
//                          list's pending entry", and this runtime has no
//                          robust-list support at all, so ENOSYS is both what
//                          the guest kernel answers today and the honest answer
//                          on a kernel that implements them.
static int base_op(int op)
{
    return op & ~(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME |
                  FUTEX_ROBUST_UNLOCK | FUTEX_ROBUST_LIST32);
}

static bool handles_cmd(int cmd, bool shared)
{
    switch (cmd) {
    case FUTEX_REQUEUE:
    case FUTEX_CMP_REQUEUE:
    case FUTEX_WAKE_OP:
    case FUTEX_LOCK_PI:
    case FUTEX_UNLOCK_PI:
    case FUTEX_TRYLOCK_PI:
    case FUTEX_LOCK_PI2:
    case FUTEX_WAIT_REQUEUE_PI:
    case FUTEX_CMP_REQUEUE_PI:
        return true;
    case FUTEX_WAIT:
    case FUTEX_WAKE:
    case FUTEX_WAIT_BITSET:
    case FUTEX_WAKE_BITSET:
        // Only the process-shared forms, which need ulock flavour 3 on both
        // ends. The private ones are thread.c's and stay there.
        return shared;
    default:
        return false;
    }
}

bool lxrt_futex_ext_handles(int op)
{
    return handles_cmd(base_op(op), !(op & FUTEX_PRIVATE_FLAG));
}

// INTEGRATION, and it is load-bearing: dispatch.c must offer EVERY futex op to
// this function first and fall back to lxrt_futex() only on
// LXRT_FUTEX_NOT_HANDLED. Wiring it the other way round -- calling lxrt_futex()
// first and coming here only for what it rejects -- silently un-does the
// process-shared fix, because the shared waits then park on thread.c's
// task-local flavour where no wake from another process can reach them. The
// wakes here fall back to that flavour when the shared queue is empty, so that
// wiring degrades to today's behaviour rather than to a new hang, but it is not
// the fix.
long lxrt_futex_ext(uint32_t *uaddr, int op, uint32_t val, uint64_t val2,
                    uint32_t *uaddr2, uint32_t val3)
{
    int cmd = base_op(op);
    bool shared = !(op & FUTEX_PRIVATE_FLAG);

    if (!handles_cmd(cmd, shared))
        // WAIT, WAKE, WAIT_BITSET and WAKE_BITSET in their PRIVATE forms belong
        // to thread.c; FUTEX_FD (2) was removed from Linux in 2.6.26 and is
        // -ENOSYS there too, which is what thread.c's default arm returns.
        return LXRT_FUTEX_NOT_HANDLED;

    if (op & (FUTEX_ROBUST_UNLOCK | FUTEX_ROBUST_LIST32))
        return LERR(ENOSYS);
    if ((op & FUTEX_CLOCK_REALTIME) && cmd != FUTEX_WAIT_BITSET && cmd != FUTEX_LOCK_PI2)
        return LERR(ENOSYS);

    switch (cmd) {
    case FUTEX_WAIT:
        return shared_wait(uaddr, val, val2, FUTEX_BITSET_MATCH_ANY, false,
                           false);

    case FUTEX_WAIT_BITSET:
        return shared_wait(uaddr, val, val2, val3, true,
                           (op & FUTEX_CLOCK_REALTIME) != 0);

    case FUTEX_WAKE:
        return shared_wake(uaddr, val, FUTEX_BITSET_MATCH_ANY, false);

    case FUTEX_WAKE_BITSET:
        return shared_wake(uaddr, val, val3, true);

    case FUTEX_REQUEUE:
        // No compare. The uncompared form has been deprecated since 2.6.22
        // because it cannot be used safely with a condvar, but glibc's older
        // lowlevellock still emits it, it is still accepted by the 6.17 guest
        // (measured: returns 0, not -38), and it costs one bool to support.
        return requeue_emulate(uaddr, uaddr2, val, val2, val3, false, shared);

    case FUTEX_CMP_REQUEUE:
        return requeue_emulate(uaddr, uaddr2, val, val2, val3, true, shared);

    case FUTEX_WAKE_OP:
        return wake_op(uaddr, val, val2, uaddr2, val3, shared);

    case FUTEX_LOCK_PI:
        if (pi_off()) return pi_unsupported();
        return pi_lock(uaddr, val2, true, shared, false);
    case FUTEX_LOCK_PI2:
        if (pi_off()) return pi_unsupported();
        return pi_lock(uaddr, val2, (op & FUTEX_CLOCK_REALTIME) != 0, shared, false);
    case FUTEX_TRYLOCK_PI:
        if (pi_off()) return pi_unsupported();
        return pi_lock(uaddr, 0, false, shared, true);
    case FUTEX_UNLOCK_PI:
        if (pi_off()) return pi_unsupported();
        return pi_unlock(uaddr, shared);
    case FUTEX_WAIT_REQUEUE_PI:
    case FUTEX_CMP_REQUEUE_PI:
        return pi_unsupported();

    default:
        // Unreachable: handles_cmd() above admits exactly the arms listed here.
        return LXRT_FUTEX_NOT_HANDLED;
    }
}
