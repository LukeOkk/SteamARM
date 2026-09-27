// The futex operations thread.c does not implement, plus the process-shared
// forms of the ones it does.
//
// thread.c covers FUTEX_WAIT/WAKE and their BITSET forms on top of Darwin's
// __ulock_wait/__ulock_wake and returns -ENOSYS for everything else. This
// module picks up the rest of the family: the requeue operations, WAKE_OP, and
// an explicit refusal for the priority-inheritance ones.
//
// It also picks up WAIT/WAKE/WAIT_BITSET/WAKE_BITSET when the guest did NOT set
// FUTEX_PRIVATE_FLAG. Those need Darwin's UL_COMPARE_AND_WAIT_SHARED ulock
// flavour, whose key crosses tasks, and thread.c parks on the task-local
// flavour whose key does not: measured, a wake from another process on the
// task-local flavour reaches nobody and the waiter sleeps to its timeout. Both
// ends of a shared futex therefore have to live in one place, and this is it.
// The private forms stay in thread.c untouched.
//
// The entry point is a filter, not a replacement: it answers only for the
// operations listed in futex_ops.c and returns LXRT_FUTEX_NOT_HANDLED for the
// others, so the caller falls through to lxrt_futex() unchanged.

#ifndef LXRT_FUTEX_OPS_H
#define LXRT_FUTEX_OPS_H

#include <stdbool.h>
#include <stdint.h>

// "This op is not mine." A futex return is either a count (>= 0) or a negated
// Linux errno (-1 .. -4095, the range Linux itself reserves via MAX_ERRNO), so
// the sentinel is placed where neither can reach. LONG_MIN rather than a value
// just past the errno range on purpose: if the caller ever forgets to filter
// it, a guest that receives it sees an absurd number instead of something that
// reads as a plausible failure like -38 (ENOSYS).
#define LXRT_FUTEX_NOT_HANDLED ((long)(-0x7fffffffffffffffLL - 1))

// True when lxrt_futex_ext() will answer for this op. Pass the op exactly as
// the guest supplied it: all four flag bits (FUTEX_PRIVATE_FLAG 128,
// FUTEX_CLOCK_REALTIME 256, FUTEX_ROBUST_UNLOCK 512, FUTEX_ROBUST_LIST32 1024)
// are handled internally, and FUTEX_PRIVATE_FLAG changes the answer -- the
// shared forms of WAIT and WAKE are handled here, the private forms are not.
bool lxrt_futex_ext_handles(int op);

// Same argument order as the Linux futex syscall and as lxrt_futex(): the
// fourth argument is the `timeout` slot, which the requeue and WAKE_OP
// operations reinterpret as a count rather than a pointer. Returns
// LXRT_FUTEX_NOT_HANDLED when the op belongs to thread.c, otherwise a
// Linux-convention result: a count, or a negated LINUX errno.
//
// dispatch.c must call THIS FIRST for every futex op and fall back to
// lxrt_futex() only on LXRT_FUTEX_NOT_HANDLED. Calling lxrt_futex() first and
// arriving here only for what it rejects leaves process-shared waits parked on
// the wrong ulock flavour, where no cross-process wake can reach them.
long lxrt_futex_ext(uint32_t *uaddr, int op, uint32_t val, uint64_t val2,
                    uint32_t *uaddr2, uint32_t val3);

#endif
