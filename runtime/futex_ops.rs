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
//
// (Ported from futex_ops.c; the contract is still stated in futex_ops.h.)

use std::cell::UnsafeCell;
use std::ffi::{c_char, c_int, c_long, c_void};
use std::mem::size_of;
use std::sync::atomic::{AtomicI32, AtomicU32, Ordering};

// "This op is not mine." See futex_ops.h: LONG_MIN, where neither a count nor
// a negated Linux errno can reach.
const LXRT_FUTEX_NOT_HANDLED: c_long = c_long::MIN;

// Darwin errno values (host side; converted with lxrt_errno_to_linux).
const EPERM: c_int = 1;
const EINTR: c_int = 4;
const EDEADLK: c_int = 11;
const EFAULT: c_int = 14;
const EINVAL: c_int = 22;
const EAGAIN: c_int = 35;
const ETIMEDOUT: c_int = 60;
const ENOSYS: c_int = 78;

const CLOCK_REALTIME: c_int = 0;
const CLOCK_MONOTONIC: c_int = 6;

// <mach/vm_prot.h>, <mach/vm_region.h>, <mach/kern_return.h>
type VmProt = c_int;
const VM_PROT_READ: VmProt = 0x01;
const VM_PROT_WRITE: VmProt = 0x02;
const VM_REGION_BASIC_INFO_64: c_int = 9;
const KERN_SUCCESS: c_int = 0;
const MACH_PORT_NULL: u32 = 0;

// vm_region_basic_info_data_64_t; the header declares it under
// #pragma pack(push, 4).
#[repr(C, packed(4))]
struct VmRegionBasicInfo64 {
    protection: VmProt,
    max_protection: VmProt,
    inheritance: u32,
    shared: c_int,
    reserved: c_int,
    offset: u64,
    behavior: c_int,
    user_wired_count: u16,
}
const _: [(); 36] = [(); size_of::<VmRegionBasicInfo64>()];
const VM_REGION_BASIC_INFO_COUNT_64: u32 =
    (size_of::<VmRegionBasicInfo64>() / size_of::<c_int>()) as u32;

#[repr(C)]
struct Timespec {
    tv_sec: c_long,
    tv_nsec: c_long,
}

// Darwin pthread_mutex_t: a signature word plus 56 opaque bytes.
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}
const _: [(); 64] = [(); size_of::<PthreadMutex>()];
const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: u32;

    // These are not public Darwin API, but os_unfair_lock and libc++ are built
    // on them and they are the only compare-and-wait primitive on the system.
    fn __ulock_wait(operation: u32, addr: *mut c_void, value: u64, timeout_us: u32) -> c_int;
    fn __ulock_wake(operation: u32, addr: *mut c_void, wake_value: u64) -> c_int;

    fn mach_vm_region(
        target_task: u32,
        address: *mut u64,
        size: *mut u64,
        flavor: c_int,
        info: *mut c_int,
        info_count: *mut u32,
        object_name: *mut u32,
    ) -> c_int;
    fn mach_port_deallocate(task: u32, name: u32) -> c_int;
    fn clock_gettime(clock: c_int, ts: *mut Timespec) -> c_int;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;

    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
    fn lxrt_gettid() -> c_int;
    fn lxrt_futex_waitv_notify(addr: *const c_void);
    fn lxrt_futex_park_enter(addr: *const c_void); // thread.c
    fn lxrt_futex_park_leave(addr: *const c_void);
    // lxrt.h's lxrt_interrupted_internally() is a static inline reading the
    // thread-local lxrt_sig_during_syscall; this is its out-of-line form
    // (dispatch.c), the one Rust modules call.
    fn lxrt_interrupted_internally_ffi() -> bool;
}

#[inline(always)]
fn lerr(error: c_int) -> c_long {
    (-unsafe { lxrt_errno_to_linux(error) }) as c_long
}

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
const UL_COMPARE_AND_WAIT: u32 = 1;
const UL_COMPARE_AND_WAIT_SHARED: u32 = 3;
const ULF_NO_ERRNO: u32 = 0x0100_0000;
const ULF_WAKE_ALL: u32 = 0x0000_0100;

// __ulock_wait takes an unsigned 32-bit microsecond timeout in which 0 means
// "no timeout", so a wait longer than this has to be split into chunks.
const ULOCK_MAX_US: u32 = 0xffff_fffe;

// ------------------------------------------------------- Linux constants
//
// Every value below was read out of the guest's own uapi/linux/futex.h over
// ssh, not out of a sysroot and not out of memory. Darwin has no counterpart
// for any of them: __ulock_* takes an entirely different operation space
// (UL_COMPARE_AND_WAIT = 1, UL_UNFAIR_LOCK = 2, ULF_WAKE_ALL = 0x100 ...), so
// these are pure guest-side numbers and the only divergence risk is against
// Linux itself, not against a Darwin constant that happens to share the low
// range.

const FUTEX_WAIT: c_int = 0;
const FUTEX_WAKE: c_int = 1;
const FUTEX_REQUEUE: c_int = 3;
const FUTEX_CMP_REQUEUE: c_int = 4;
const FUTEX_WAKE_OP: c_int = 5;
const FUTEX_LOCK_PI: c_int = 6;
const FUTEX_UNLOCK_PI: c_int = 7;
const FUTEX_TRYLOCK_PI: c_int = 8;
const FUTEX_WAIT_BITSET: c_int = 9;
const FUTEX_WAKE_BITSET: c_int = 10;
const FUTEX_WAIT_REQUEUE_PI: c_int = 11;
const FUTEX_CMP_REQUEUE_PI: c_int = 12;
const FUTEX_LOCK_PI2: c_int = 13;

const FUTEX_PRIVATE_FLAG: c_int = 128;
const FUTEX_CLOCK_REALTIME: c_int = 256;
// 512 and 1024 are new in the robust-futex rework and are absent from the
// sysroot copy of this header the constants were first taken from; the guest's
// own header has them, and its FUTEX_CMD_MASK is
// ~(PRIVATE|CLOCK_REALTIME|ROBUST_UNLOCK|ROBUST_LIST32). They are modifiers on
// WAKE, WAKE_BITSET and UNLOCK_PI meaning "also clear the robust list's pending
// entry", with LIST32 selecting the 32-bit list layout.
const FUTEX_ROBUST_UNLOCK: c_int = 512;
const FUTEX_ROBUST_LIST32: c_int = 1024;

const FUTEX_BITSET_MATCH_ANY: u32 = 0xffff_ffff;

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
const FUTEX_OP_SET: u32 = 0;
const FUTEX_OP_ADD: u32 = 1;
const FUTEX_OP_OR: u32 = 2;
const FUTEX_OP_ANDN: u32 = 3;
const FUTEX_OP_XOR: u32 = 4;

const FUTEX_OP_CMP_EQ: u32 = 0;
const FUTEX_OP_CMP_NE: u32 = 1;
const FUTEX_OP_CMP_LT: u32 = 2;
const FUTEX_OP_CMP_LE: u32 = 3;
const FUTEX_OP_CMP_GT: u32 = 4;
const FUTEX_OP_CMP_GE: u32 = 5;

// The guest is aarch64, so its struct __kernel_timespec is two signed 64-bit
// fields with no padding and no 32-bit compat form to worry about.
#[repr(C)]
struct GuestTimespec {
    tv_sec: i64,
    tv_nsec: i64,
}
const _: [(); 16] = [(); size_of::<GuestTimespec>()];

const NS_PER_S: i64 = 1_000_000_000;
// Linux does not reject a seconds field too large for a ktime_t, it saturates:
// ktime_set() returns KTIME_MAX at and above KTIME_SEC_MAX. A deadline past
// that is therefore an untimed wait on Linux, and is one here too, which also
// keeps the nanosecond arithmetic below inside int64_t.
const GUEST_SEC_MAX: i64 = i64::MAX / NS_PER_S;

// The guest's futex word and ours are the same memory: there is no VM here, so
// an atomic on this address is the same LDXR/STXR or LSE instruction pair the
// guest's own libc emits on it, on the same core, under the same memory model.
// That is what makes FUTEX_WAKE_OP implementable at all. It only holds while
// the atomic is lock-free -- a fallback would take a side lock the guest knows
// nothing about and the "atomically" in the Linux contract would quietly
// become a lie. AtomicU32 exists only on targets where it is lock-free, and it
// has the size and alignment of the word it stands on.
#[cfg(not(target_has_atomic = "32"))]
compile_error!("32-bit atomics must be lock-free to be atomic against guest code");
const _: [(); size_of::<u32>()] = [(); size_of::<AtomicU32>()];

#[inline(always)]
unsafe fn word<'a>(p: *mut u32) -> &'a AtomicU32 {
    // Every caller has checked alignment and the mapping first, as the C did
    // before its (_Atomic uint32_t *) cast.
    AtomicU32::from_ptr(p)
}

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
unsafe fn guest_range_ok(p: *const c_void, len: usize, need: VmProt) -> bool {
    if p.is_null() {
        return false;
    }

    let want: u64 = p as usize as u64;
    let mut addr: u64 = want;
    let mut size: u64 = 0;
    let mut info = VmRegionBasicInfo64 {
        protection: 0,
        max_protection: 0,
        inheritance: 0,
        shared: 0,
        reserved: 0,
        offset: 0,
        behavior: 0,
        user_wired_count: 0,
    };
    let mut count: u32 = VM_REGION_BASIC_INFO_COUNT_64;
    let mut obj: u32 = MACH_PORT_NULL;

    if mach_vm_region(
        mach_task_self_,
        &mut addr,
        &mut size,
        VM_REGION_BASIC_INFO_64,
        (&raw mut info).cast::<c_int>(),
        &mut count,
        &mut obj,
    ) != KERN_SUCCESS
    {
        return false;
    }
    // Measured MACH_PORT_NULL for every VM_REGION_BASIC_INFO_64 query (20000
    // iterations, port count unchanged), but a leak on this path would be
    // unbounded if a future kernel started handing one back.
    if obj != MACH_PORT_NULL {
        let _ = mach_port_deallocate(mach_task_self_, obj);
    }

    // mach_vm_region answers with the first region at OR ABOVE the address it
    // is given, so an unmapped address comes back as KERN_SUCCESS describing
    // the next mapping up. Without this test every hole in the address space
    // reads as valid.
    if want < addr || want.wrapping_add(len as u64) > addr.wrapping_add(size) {
        return false;
    }
    let protection = info.protection;
    (protection & need) == need
}

#[inline(always)]
unsafe fn guest_word_ok(p: *const u32, need: VmProt) -> bool {
    guest_range_ok(p.cast(), size_of::<u32>(), need)
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
const MAX_SHARED_RANGES: usize = 1024;

#[repr(C)]
#[derive(Clone, Copy)]
struct SharedRange {
    start: u64,
    end: u64,
}

// g_shared / g_nshared: plain data, every access under G_SHARED_LOCK.
struct SharedTable {
    ranges: UnsafeCell<[SharedRange; MAX_SHARED_RANGES]>,
    count: UnsafeCell<c_int>,
}
unsafe impl Sync for SharedTable {}

static G_SHARED: SharedTable = SharedTable {
    ranges: UnsafeCell::new([SharedRange { start: 0, end: 0 }; MAX_SHARED_RANGES]),
    count: UnsafeCell::new(0),
};

struct Lock(UnsafeCell<PthreadMutex>);
unsafe impl Sync for Lock {}

static G_SHARED_LOCK: Lock = Lock(UnsafeCell::new(PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
}));

#[inline(always)]
unsafe fn shared_lock() {
    let _ = pthread_mutex_lock(G_SHARED_LOCK.0.get());
}

#[inline(always)]
unsafe fn shared_unlock() {
    let _ = pthread_mutex_unlock(G_SHARED_LOCK.0.get());
}

// LXRT_FORK_SAFE(futex_shared, g_shared_lock): the mutex is taken before fork
// and released after it in both parent and child (the child's only thread is
// the one that took it).
extern "C" fn futex_shared_fork_prepare() {
    unsafe { shared_lock() }
}

extern "C" fn futex_shared_fork_release() {
    unsafe { shared_unlock() }
}

extern "C" fn futex_shared_fork_register() {
    unsafe {
        let _ = pthread_atfork(
            Some(futex_shared_fork_prepare),
            Some(futex_shared_fork_release),
            Some(futex_shared_fork_release),
        );
    }
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FUTEX_SHARED_FORK_REGISTER: extern "C" fn() = futex_shared_fork_register;

#[no_mangle]
pub unsafe extern "C" fn lxrt_futex_shared_add(addr: u64, len: u64) {
    if len == 0 {
        return;
    }
    shared_lock();
    let ranges = G_SHARED.ranges.get().cast::<SharedRange>();
    let count = G_SHARED.count.get();
    if (*count as usize) < MAX_SHARED_RANGES {
        let slot = ranges.add(*count as usize);
        (*slot).start = addr;
        (*slot).end = addr.wrapping_add(len);
        *count += 1;
    }
    shared_unlock();
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_futex_shared_remove(addr: u64, len: u64) {
    let end = addr.wrapping_add(len);
    shared_lock();
    let ranges = G_SHARED.ranges.get().cast::<SharedRange>();
    let count = G_SHARED.count.get();
    let mut i: c_int = 0;
    while i < *count {
        let entry = ranges.add(i as usize);
        let s = (*entry).start;
        let e = (*entry).end;
        if e <= addr || s >= end {
            i += 1;
            continue;
        }
        if s < addr && e > end && (*count as usize) < MAX_SHARED_RANGES {
            // split
            let tail = ranges.add(*count as usize);
            (*tail).start = end;
            (*tail).end = e;
            *count += 1;
            (*entry).end = addr;
            i += 1;
        } else if s < addr {
            (*entry).end = addr;
            i += 1;
        } else if e > end {
            (*entry).start = end;
            i += 1;
        } else {
            *count -= 1;
            *entry = *ranges.add(*count as usize);
        }
    }
    shared_unlock();
}

unsafe fn truly_shared(p: *const c_void) -> bool {
    let a = p as usize as u64;
    let mut found = false;
    shared_lock();
    let ranges = G_SHARED.ranges.get().cast::<SharedRange>();
    let count = *G_SHARED.count.get();
    let mut i: c_int = 0;
    while i < count && !found {
        let entry = ranges.add(i as usize);
        found = a >= (*entry).start && a < (*entry).end;
        i += 1;
    }
    shared_unlock();
    found
}

#[inline(always)]
fn aligned(p: *const u32) -> bool {
    (p as usize) % size_of::<u32>() == 0
}

unsafe fn now_ns(realtime: bool) -> i64 {
    let mut t = Timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    let _ = clock_gettime(
        if realtime {
            CLOCK_REALTIME
        } else {
            CLOCK_MONOTONIC
        },
        &mut t,
    );
    (t.tv_sec as i64)
        .wrapping_mul(NS_PER_S)
        .wrapping_add(t.tv_nsec as i64)
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
unsafe fn wake_up_to(addr: *mut c_void, budget: i64, shared: bool) -> c_int {
    if budget <= 0 {
        return 0;
    }
    lxrt_futex_waitv_notify(addr); // futex_waitv callers parked on this word
    let mut flags = ULF_NO_ERRNO;
    if budget > 1 {
        flags |= ULF_WAKE_ALL;
    }

    // -ENOENT is an empty queue. -EINVAL would be a malformed address, already
    // ruled out by the caller. Neither released anything.
    let first = if shared && truly_shared(addr) {
        UL_COMPARE_AND_WAIT_SHARED
    } else {
        UL_COMPARE_AND_WAIT
    };
    if __ulock_wake(flags | first, addr, 0) == 0 {
        return 1;
    }
    if shared
        && first == UL_COMPARE_AND_WAIT_SHARED
        && __ulock_wake(flags | UL_COMPARE_AND_WAIT, addr, 0) == 0
    {
        return 1;
    }
    0
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

unsafe fn shared_wait(
    uaddr: *mut u32,
    val: u32,
    utime: u64,
    bitset: u32,
    bitset_form: bool,
    realtime: bool,
) -> c_long {
    if !aligned(uaddr) {
        return lerr(EINVAL);
    }
    if !guest_word_ok(uaddr, VM_PROT_READ) {
        return lerr(EFAULT);
    }
    // Measured on the guest: FUTEX_WAIT_BITSET and FUTEX_WAKE_BITSET both
    // return -22 for a zero bitset, and -11/0 for a non-zero one.
    if bitset_form && bitset == 0 {
        return lerr(EINVAL);
    }

    // A partial bitset is honoured by Linux -- measured: a waiter with bitset
    // 0x1 is NOT released by a wake with bitset 0x2 -- and cannot be honoured
    // here, because a ulock queue has no bitset and no way to add one. The
    // divergence is deliberately left in the safe direction: this file wakes a
    // waiter Linux would have skipped rather than skipping one Linux would have
    // woken, so a guest sees a spurious wakeup (which every futex user must
    // already re-check for) instead of a lost one (which hangs). Refusing
    // partial bitsets with ENOSYS was the alternative and is worse: glibc never
    // emits one, so the only programs it would break are the ones that work.

    let mut timed = false;
    let mut deadline_realtime = false;
    let mut deadline: i64 = 0;
    if utime != 0 {
        let ts = utime as usize as *const GuestTimespec;
        if !guest_range_ok(ts.cast(), size_of::<GuestTimespec>(), VM_PROT_READ) {
            return lerr(EFAULT);
        }
        let sec: i64 = (*ts).tv_sec;
        let nsec: i64 = (*ts).tv_nsec;
        // timespec64_valid(). Measured on the guest: {-1, 0} -> -22 and
        // {0, 1000000000} -> -22, while {0, 0} is valid and comes straight back
        // as -110 ETIMEDOUT.
        if sec < 0 || nsec < 0 || nsec >= NS_PER_S {
            return lerr(EINVAL);
        }
        if sec < GUEST_SEC_MAX {
            timed = true;
            if bitset_form {
                // FUTEX_WAIT_BITSET's timeout is ABSOLUTE, on CLOCK_REALTIME
                // when the guest asked for it and CLOCK_MONOTONIC otherwise.
                deadline = sec.wrapping_mul(NS_PER_S).wrapping_add(nsec);
                deadline_realtime = realtime;
            } else {
                // FUTEX_WAIT's is RELATIVE and always CLOCK_MONOTONIC --
                // FUTEX_WAIT | FUTEX_CLOCK_REALTIME is -38 on Linux, measured,
                // and never reaches here.
                let rel = sec.wrapping_mul(NS_PER_S).wrapping_add(nsec);
                let base = now_ns(false);
                if rel > i64::MAX.wrapping_sub(base) {
                    timed = false; // saturates, as above
                } else {
                    deadline = base.wrapping_add(rel);
                }
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
    let cur = word(uaddr).load(Ordering::SeqCst);
    if cur != val {
        return lerr(EAGAIN);
    }

    let flavour = (if truly_shared(uaddr.cast()) {
        UL_COMPARE_AND_WAIT_SHARED
    } else {
        UL_COMPARE_AND_WAIT
    }) | ULF_NO_ERRNO;
    loop {
        let mut us: u32 = 0; // 0 is "no timeout" to __ulock_wait
        if timed {
            let left = deadline.wrapping_sub(now_ns(deadline_realtime));
            if left <= 0 {
                return lerr(ETIMEDOUT);
            }
            // Round up: a wait that returns early is a wait that reports
            // ETIMEDOUT before the deadline the guest asked for.
            let left_us = left.wrapping_add(999) / 1000;
            us = if left_us > ULOCK_MAX_US as i64 {
                ULOCK_MAX_US
            } else {
                left_us as u32
            };
        }

        // Counted as parked for thread.c's private wakes, which skip the
        // kernel when nobody of this process is (g_parked there): glibc
        // waits with the shared form on words that are woken privately.
        lxrt_futex_park_enter(uaddr.cast());
        if word(uaddr).load(Ordering::SeqCst) != val {
            lxrt_futex_park_leave(uaddr.cast());
            return lerr(EAGAIN);
        }
        let r = __ulock_wait(flavour, uaddr.cast(), val as u64, us);
        lxrt_futex_park_leave(uaddr.cast());
        if r >= 0 {
            return 0;
        }
        let e = r.wrapping_neg();
        // A chunk expiring is not the deadline expiring: a timeout beyond
        // ULOCK_MAX_US (about 71.5 minutes) has to be split, and the loop
        // re-derives what is left. Under that limit this runs exactly once, so
        // the re-park window -- during which a wake would be missed -- exists
        // only for waits longer than 71 minutes, and at most once per 71
        // minutes of one. thread.c's alternative for the same case is to drop
        // the timeout entirely and wait forever.
        if e == ETIMEDOUT && timed && us == ULOCK_MAX_US {
            continue;
        }
        // Interrupted by a signal that ran no guest handler: Linux would
        // still be waiting. The loop re-derives what is left of the deadline.
        if e == EINTR && lxrt_interrupted_internally_ffi() {
            continue;
        }
        // __ulock_wait with ULF_NO_ERRNO returns a negative DARWIN errno.
        return lerr(e);
    }
}

unsafe fn shared_wake(uaddr: *mut u32, val: u32, bitset: u32, bitset_form: bool) -> c_long {
    if !aligned(uaddr) {
        return lerr(EINVAL);
    }
    if !guest_word_ok(uaddr, VM_PROT_READ) {
        return lerr(EFAULT);
    }
    if bitset_form && bitset == 0 {
        return lerr(EINVAL);
    }

    // futex_wake()'s loop tests its budget only AFTER releasing a waiter
    // (`if (++ret >= nr_wake) break;`), so a budget of 0 or a negative one
    // still releases exactly one. MEASURED on the guest: FUTEX_WAKE with val=0
    // and one waiter parked returns 1 and the waiter runs.
    let mut nr = val as i32;
    // No bitset in a ulock queue: a targeted wake of one could release the
    // wrong class of waiter (see thread.c, FUTEX_WAKE_BITSET). Wake them all.
    if bitset_form && bitset != FUTEX_BITSET_MATCH_ANY {
        nr = i32::MAX;
    }
    wake_up_to(uaddr.cast(), if nr <= 0 { 1 } else { nr as i64 }, true) as c_long
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
unsafe fn requeue_emulate(
    uaddr: *mut u32,
    uaddr2: *mut u32,
    val: u32,
    val2: u64,
    val3: u32,
    compare: bool,
    shared: bool,
) -> c_long {
    // The fourth syscall argument is the `timeout` pointer slot, and for the
    // requeue and WAKE_OP operations the kernel reinterprets it as a count with
    // `val2 = (u32)(unsigned long)utime`. The upper 32 bits are DISCARDED, not
    // rejected, so a guest passing a 64-bit garbage value gets its low half
    // used; truncating here rather than testing the 64-bit value keeps that
    // behaviour instead of inventing an error Linux does not return.
    let nr_wake = val as i32;
    let nr_requeue = val2 as u32 as i32;

    // Both counts are `int` on the kernel side, so 0xffffffff arrives as -1 and
    // is rejected rather than read as a four-billion-waiter budget. Unlike
    // futex_wake_op(), futex_requeue() really does open with this check --
    // MEASURED on the guest: CMP_REQUEUE with nr_wake=-1 and with nr_requeue=-1
    // are both -22, while the same negative counts on WAKE_OP are not errors at
    // all (see wake_op below).
    if nr_wake < 0 || nr_requeue < 0 {
        return lerr(EINVAL);
    }
    if !aligned(uaddr) || !aligned(uaddr2) {
        return lerr(EINVAL);
    }
    // Linux takes a futex key for uaddr2 as well, even though a plain requeue
    // never reads it, so a bad uaddr2 is EFAULT there too -- measured -14.
    if !guest_word_ok(uaddr, VM_PROT_READ) || !guest_word_ok(uaddr2, VM_PROT_READ) {
        return lerr(EFAULT);
    }

    if compare {
        let cur = word(uaddr).load(Ordering::SeqCst);
        // Unsigned equality: Linux compares the raw u32, and a futex word with
        // its top bit set (FUTEX_WAITERS is 0x80000000) is ordinary.
        if cur != val3 {
            return lerr(EAGAIN);
        }
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
    let n = wake_up_to(uaddr.cast(), nr_wake as i64 + nr_requeue as i64, shared);

    if lxrt_trace_on() && n > 0 && nr_requeue > 0 {
        let _ = fprintf(
            lxrt_trace_stream(),
            b"[lxrt] futex requeue: woke the queue on %p instead of moving up to %d of it to %p\n\0"
                .as_ptr()
                .cast(),
            uaddr.cast::<c_void>(),
            nr_requeue as c_int,
            uaddr2.cast::<c_void>(),
        );
    }
    n as c_long
}

// ------------------------------------------------------- FUTEX_WAKE_OP

#[inline(always)]
fn sign_extend_12(v: u32) -> i32 {
    // Defined for every input, unlike a cast through a signed shift: the field
    // is biased into [0, 0xfff] and then unbiased.
    (((v & 0xfff) ^ 0x800) as i32) - 0x800
}

unsafe fn wake_op(
    uaddr: *mut u32,
    val: u32,
    val2: u64,
    uaddr2: *mut u32,
    encoded: u32,
    shared: bool,
) -> c_long {
    let nr_wake = val as i32;
    let nr_wake2 = val2 as u32 as i32; // same truncation as requeue

    if !aligned(uaddr) || !aligned(uaddr2) {
        return lerr(EINVAL);
    }
    // uaddr is only woken, but uaddr2 is read-modify-written, so it needs write
    // permission -- a futex word in a region the guest mapped read-only is
    // EFAULT on Linux and a host SIGSEGV here without this test.
    if !guest_word_ok(uaddr, VM_PROT_READ) || !guest_word_ok(uaddr2, VM_PROT_READ | VM_PROT_WRITE)
    {
        return lerr(EFAULT);
    }

    // This function used to reject a negative nr_wake/nr_wake2 with EINVAL, by
    // generalising futex_requeue()'s check to a function that does not have
    // one. MEASURED on the guest: FUTEX_WAKE_OP with nr_wake = -1 returns 0 on
    // an empty queue and 1 with a waiter parked, and the waiter really runs;
    // nr_wake = 0 behaves identically, because futex_wake_op()'s loop tests the
    // budget only after releasing a waiter (`if (++ret >= nr_wake) break;`).
    // nr_wake2 measures the same. So a budget of 0 or less is a budget of one,
    // not an error and not nothing.
    let budget: i64 = if nr_wake <= 0 { 1 } else { nr_wake as i64 };
    let budget2: i64 = if nr_wake2 <= 0 { 1 } else { nr_wake2 as i64 };

    let op = (encoded & 0x7000_0000) >> 28;
    let cmp = (encoded & 0x0f00_0000) >> 24;
    let mut oparg = sign_extend_12((encoded & 0x00ff_f000) >> 12);
    let cmparg = sign_extend_12(encoded & 0x0000_0fff);

    if encoded & 0x8000_0000 != 0 {
        // FUTEX_OP_OPARG_SHIFT << 28
        //
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
        oparg = (1u32 << ((oparg as u32) & 31)) as i32;
    }

    let target = word(uaddr2);

    // The op is validated BEFORE the memory is touched; an unknown one is
    // -ENOSYS, not -EINVAL, and leaves *uaddr2 alone. Measured: op 7 with
    // oparg 5 returns -38 and leaves the word at 0.
    let oldval: u32 = match op {
        FUTEX_OP_SET => target.swap(oparg as u32, Ordering::SeqCst),
        // Wrapping is the point: oparg is signed and a negative one is an
        // atomic subtract, which is how a lock word is released.
        FUTEX_OP_ADD => target.fetch_add(oparg as u32, Ordering::SeqCst),
        FUTEX_OP_OR => target.fetch_or(oparg as u32, Ordering::SeqCst),
        // ANDN is AND NOT: *uaddr2 &= ~oparg. Reading it as a plain AND is a
        // one-character mistake that inverts the meaning of every bit.
        FUTEX_OP_ANDN => target.fetch_and(!(oparg as u32), Ordering::SeqCst),
        FUTEX_OP_XOR => target.fetch_xor(oparg as u32, Ordering::SeqCst),
        _ => return lerr(ENOSYS),
    };

    // The comparison is SIGNED -- the kernel holds oldval in an int and
    // sign-extends cmparg out of a 12-bit field. glibc's lock words use the top
    // bit as a waiters flag, so a word like 0x80000001 is a large positive
    // number under an unsigned compare and a negative one under Linux's: a
    // FUTEX_OP_CMP_GT that should fail would succeed and wake the wrong queue.
    // Measured on the guest: CMP_GT 0 against an old value of 0x80000001 does
    // not wake.
    let old = oldval as i32;
    let wake2 = match cmp {
        FUTEX_OP_CMP_EQ => old == cmparg,
        FUTEX_OP_CMP_NE => old != cmparg,
        FUTEX_OP_CMP_LT => old < cmparg,
        FUTEX_OP_CMP_LE => old <= cmparg,
        FUTEX_OP_CMP_GT => old > cmparg,
        FUTEX_OP_CMP_GE => old >= cmparg,
        // Linux validates the comparison only after the atomic has already
        // been applied, so the write above stands and no wake happens. Matched
        // deliberately: a guest that retries after this error must see the same
        // futex word it would see on Linux. Measured: op=ADD oparg=5 with an
        // invalid cmp returns -38 AND leaves the word at 5.
        _ => return lerr(ENOSYS),
    };

    let mut n = wake_up_to(uaddr.cast(), budget, shared);
    if wake2 {
        n = n.wrapping_add(wake_up_to(uaddr2.cast(), budget2, shared));
    }
    n as c_long
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
const FUTEX_WAITERS: u32 = 0x8000_0000;
const FUTEX_OWNER_DIED: u32 = 0x4000_0000;
const FUTEX_TID_MASK: u32 = 0x3fff_ffff;

#[inline(always)]
fn pi_unsupported() -> c_long {
    lerr(ENOSYS)
}

// LXRT_NO_PI=1: the PI operations answer ENOSYS again (glibc then refuses
// PTHREAD_PRIO_INHERIT mutexes and programs fall back to plain ones).
unsafe fn pi_off() -> bool {
    // The C's `static int off = -1`: a plain word, racing initialisers all
    // compute the same value.
    static OFF: AtomicI32 = AtomicI32::new(-1);
    let mut off = OFF.load(Ordering::Relaxed);
    if off < 0 {
        const NAME: *const c_char = b"LXRT_NO_PI\0".as_ptr().cast();
        off = (!getenv(NAME).is_null() && *getenv(NAME) == b'1' as c_char) as c_int;
        OFF.store(off, Ordering::Relaxed);
    }
    off != 0
}

unsafe fn pi_lock(
    uaddr: *mut u32,
    utime: u64,
    realtime: bool,
    shared: bool,
    try_only: bool,
) -> c_long {
    if !aligned(uaddr) {
        return lerr(EINVAL);
    }
    if !guest_word_ok(uaddr, VM_PROT_READ | VM_PROT_WRITE) {
        return lerr(EFAULT);
    }
    let mut timed = false;
    let mut deadline: i64 = 0;
    if utime != 0 && !try_only {
        let ts = utime as usize as *const GuestTimespec;
        if !guest_range_ok(ts.cast(), size_of::<GuestTimespec>(), VM_PROT_READ) {
            return lerr(EFAULT);
        }
        let sec: i64 = (*ts).tv_sec;
        let nsec: i64 = (*ts).tv_nsec;
        if sec < 0 || nsec < 0 || nsec >= NS_PER_S {
            return lerr(EINVAL);
        }
        if sec < GUEST_SEC_MAX {
            timed = true;
            deadline = sec.wrapping_mul(NS_PER_S).wrapping_add(nsec);
        }
    }
    let tid = (lxrt_gettid() as u32) & FUTEX_TID_MASK;
    let w = word(uaddr);
    let flavour = (if shared && truly_shared(uaddr.cast()) {
        UL_COMPARE_AND_WAIT_SHARED
    } else {
        UL_COMPARE_AND_WAIT
    }) | ULF_NO_ERRNO;
    loop {
        let mut cur = w.load(Ordering::Acquire);
        let owner = cur & FUTEX_TID_MASK;
        if owner == 0 {
            let want = tid
                | (cur & FUTEX_OWNER_DIED)
                | (if try_only {
                    cur & FUTEX_WAITERS
                } else {
                    FUTEX_WAITERS
                });
            if w.compare_exchange(cur, want, Ordering::AcqRel, Ordering::Acquire)
                .is_ok()
            {
                return 0;
            }
            continue;
        }
        if owner == tid {
            return lerr(EDEADLK);
        }
        if try_only {
            return lerr(EAGAIN);
        }
        if cur & FUTEX_WAITERS == 0 {
            let marked = cur | FUTEX_WAITERS;
            if w.compare_exchange(cur, marked, Ordering::AcqRel, Ordering::Acquire)
                .is_err()
            {
                continue;
            }
            cur = marked;
        }
        let mut us: u32 = 0;
        if timed {
            let left = deadline.wrapping_sub(now_ns(realtime));
            if left <= 0 {
                return lerr(ETIMEDOUT);
            }
            let left_us = left.wrapping_add(999) / 1000;
            us = if left_us > ULOCK_MAX_US as i64 {
                ULOCK_MAX_US
            } else {
                left_us as u32
            };
        }
        lxrt_futex_park_enter(uaddr.cast());
        let r = __ulock_wait(flavour, uaddr.cast(), cur as u64, us);
        lxrt_futex_park_leave(uaddr.cast());
        let e = r.wrapping_neg();
        if r >= 0 || e == EINTR || e == ETIMEDOUT {
            continue; // the word changed, a signal, or a chunk ended: look again
        }
        return lerr(e);
    }
}

unsafe fn pi_unlock(uaddr: *mut u32, shared: bool) -> c_long {
    if !aligned(uaddr) {
        return lerr(EINVAL);
    }
    if !guest_word_ok(uaddr, VM_PROT_READ | VM_PROT_WRITE) {
        return lerr(EFAULT);
    }
    let tid = (lxrt_gettid() as u32) & FUTEX_TID_MASK;
    let w = word(uaddr);
    let mut cur = w.load(Ordering::Acquire);
    loop {
        if (cur & FUTEX_TID_MASK) != tid {
            return lerr(EPERM);
        }
        match w.compare_exchange_weak(cur, 0, Ordering::AcqRel, Ordering::Acquire) {
            Ok(_) => break,
            Err(seen) => cur = seen,
        }
    }
    if cur & FUTEX_WAITERS != 0 {
        let _ = wake_up_to(uaddr.cast(), i32::MAX as i64, shared);
    }
    0
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
#[inline(always)]
fn base_op(op: c_int) -> c_int {
    op & !(FUTEX_PRIVATE_FLAG | FUTEX_CLOCK_REALTIME | FUTEX_ROBUST_UNLOCK | FUTEX_ROBUST_LIST32)
}

#[inline(always)]
fn handles_cmd(cmd: c_int, shared: bool) -> bool {
    match cmd {
        FUTEX_REQUEUE
        | FUTEX_CMP_REQUEUE
        | FUTEX_WAKE_OP
        | FUTEX_LOCK_PI
        | FUTEX_UNLOCK_PI
        | FUTEX_TRYLOCK_PI
        | FUTEX_LOCK_PI2
        | FUTEX_WAIT_REQUEUE_PI
        | FUTEX_CMP_REQUEUE_PI => true,
        // Only the process-shared forms, which need ulock flavour 3 on both
        // ends. The private ones are thread.c's and stay there.
        FUTEX_WAIT | FUTEX_WAKE | FUTEX_WAIT_BITSET | FUTEX_WAKE_BITSET => shared,
        _ => false,
    }
}

// True when lxrt_futex_ext() will answer for this op (futex_ops.h).
#[no_mangle]
pub extern "C" fn lxrt_futex_ext_handles(op: c_int) -> bool {
    handles_cmd(base_op(op), op & FUTEX_PRIVATE_FLAG == 0)
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
#[no_mangle]
pub unsafe extern "C" fn lxrt_futex_ext(
    uaddr: *mut u32,
    op: c_int,
    val: u32,
    val2: u64,
    uaddr2: *mut u32,
    val3: u32,
) -> c_long {
    let cmd = base_op(op);
    let shared = op & FUTEX_PRIVATE_FLAG == 0;

    if !handles_cmd(cmd, shared) {
        // WAIT, WAKE, WAIT_BITSET and WAKE_BITSET in their PRIVATE forms belong
        // to thread.c; FUTEX_FD (2) was removed from Linux in 2.6.26 and is
        // -ENOSYS there too, which is what thread.c's default arm returns.
        return LXRT_FUTEX_NOT_HANDLED;
    }

    if op & (FUTEX_ROBUST_UNLOCK | FUTEX_ROBUST_LIST32) != 0 {
        return lerr(ENOSYS);
    }
    if (op & FUTEX_CLOCK_REALTIME) != 0 && cmd != FUTEX_WAIT_BITSET && cmd != FUTEX_LOCK_PI2 {
        return lerr(ENOSYS);
    }

    match cmd {
        FUTEX_WAIT => shared_wait(uaddr, val, val2, FUTEX_BITSET_MATCH_ANY, false, false),

        FUTEX_WAIT_BITSET => shared_wait(
            uaddr,
            val,
            val2,
            val3,
            true,
            (op & FUTEX_CLOCK_REALTIME) != 0,
        ),

        FUTEX_WAKE => shared_wake(uaddr, val, FUTEX_BITSET_MATCH_ANY, false),

        FUTEX_WAKE_BITSET => shared_wake(uaddr, val, val3, true),

        // No compare. The uncompared form has been deprecated since 2.6.22
        // because it cannot be used safely with a condvar, but glibc's older
        // lowlevellock still emits it, it is still accepted by the 6.17 guest
        // (measured: returns 0, not -38), and it costs one bool to support.
        FUTEX_REQUEUE => requeue_emulate(uaddr, uaddr2, val, val2, val3, false, shared),

        FUTEX_CMP_REQUEUE => requeue_emulate(uaddr, uaddr2, val, val2, val3, true, shared),

        FUTEX_WAKE_OP => wake_op(uaddr, val, val2, uaddr2, val3, shared),

        FUTEX_LOCK_PI => {
            if pi_off() {
                return pi_unsupported();
            }
            pi_lock(uaddr, val2, true, shared, false)
        }
        FUTEX_LOCK_PI2 => {
            if pi_off() {
                return pi_unsupported();
            }
            pi_lock(uaddr, val2, (op & FUTEX_CLOCK_REALTIME) != 0, shared, false)
        }
        FUTEX_TRYLOCK_PI => {
            if pi_off() {
                return pi_unsupported();
            }
            pi_lock(uaddr, 0, false, shared, true)
        }
        FUTEX_UNLOCK_PI => {
            if pi_off() {
                return pi_unsupported();
            }
            pi_unlock(uaddr, shared)
        }
        FUTEX_WAIT_REQUEUE_PI | FUTEX_CMP_REQUEUE_PI => pi_unsupported(),

        // Unreachable: handles_cmd() above admits exactly the arms listed here.
        _ => LXRT_FUTEX_NOT_HANDLED,
    }
}
