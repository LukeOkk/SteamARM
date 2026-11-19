// Linux timerfd and signalfd on top of Darwin kqueues (timerfd_signalfd.h).
//
// The guest descriptor is a kqueue, directly watchable with EVFILT_READ by
// epoll. The registry is a malloc'd alias list under one pthread mutex, exactly
// as the C original kept it: no Rust allocator, no std locks, no unwinding.
// Host constants are the macOS arm64 ABI values (the module is self-contained,
// like the C file was, and reaches the rest of the runtime only through the
// same C symbols).

use std::arch::global_asm;
use std::cell::UnsafeCell;
use std::ffi::{c_int, c_long, c_short, c_uint, c_void};
use std::mem::{size_of, MaybeUninit};
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};

// Linux flag values (the guest ABI).
const NONBLOCK: c_int = 0x800;
const CLOEXEC: c_int = 0x80000;
const NS: Ticks = 1_000_000_000;

// Darwin <errno.h>.
const EPERM: c_int = 1;
const ENOENT: c_int = 2;
const EBADF: c_int = 9;
const ENOMEM: c_int = 12;
const EFAULT: c_int = 14;
const EINVAL: c_int = 22;
const EAGAIN: c_int = 35;
// Darwin <fcntl.h>.
const F_GETFD: c_int = 1;
const F_SETFD: c_int = 2;
const FD_CLOEXEC: c_int = 1;
// Darwin <sys/event.h>.
const EVFILT_SIGNAL: i16 = -6;
const EVFILT_TIMER: i16 = -7;
const EVFILT_USER: i16 = -10;
const EV_ADD: u16 = 0x0001;
const EV_DELETE: u16 = 0x0002;
const EV_ONESHOT: u16 = 0x0010;
const EV_CLEAR: u16 = 0x0020;
const NOTE_NSECONDS: u32 = 0x0000_0004;
const NOTE_TRIGGER: u32 = 0x0100_0000;
// Darwin <poll.h>.
const POLLIN: c_short = 0x0001;
const POLLNVAL: c_short = 0x0020;
// Darwin <signal.h>.
const SIG_BLOCK: c_int = 1;
const DARWIN_NSIG: c_int = 32;
// Darwin <pthread.h>: PTHREAD_MUTEX_INITIALIZER.
const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;

#[repr(C)]
#[derive(Clone, Copy)]
struct LTime {
    sec: i64,
    nsec: i64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct LSpec {
    interval: LTime,
    value: LTime,
}

// itimerspec ABI
const _: [(); 32] = [(); size_of::<LSpec>()];

/* Wide arithmetic preserves the full signed-64-bit seconds input ABI. */
type Ticks = i128;

#[repr(C)]
#[derive(Clone, Copy)]
struct State {
    refs: c_uint,
    kind: c_int,
    flags: c_int,
    clock: c_int,
    next: Ticks,
    interval: Ticks,
    mask: u64,
}

#[repr(C)]
struct Alias {
    fd: c_int,
    s: *mut State,
    next: *mut Alias,
}

/* Linux aarch64 ABI, independent of Darwin's siginfo_t. */
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SignalfdSiginfo {
    ssi_signo: u32,
    ssi_errno: i32,
    ssi_code: i32,
    ssi_pid: u32,
    ssi_uid: u32,
    ssi_fd: i32,
    ssi_tid: u32,
    ssi_band: u32,
    ssi_overrun: u32,
    ssi_trapno: u32,
    ssi_status: i32,
    ssi_int: i32,
    ssi_ptr: u64,
    ssi_utime: u64,
    ssi_stime: u64,
    ssi_addr: u64,
    ssi_addr_lsb: u16,
    __pad2: u16,
    ssi_syscall: i32,
    ssi_call_addr: u64,
    ssi_arch: u32,
    __pad: [u8; 28],
}

// signalfd ABI
const _: [(); 128] = [(); size_of::<SignalfdSiginfo>()];
const INFO_SIZE: usize = size_of::<SignalfdSiginfo>();

#[repr(C)]
#[derive(Clone, Copy)]
struct Kevent {
    ident: usize,
    filter: i16,
    flags: u16,
    fflags: u32,
    data: isize,
    udata: *mut c_void,
}

const _: [(); 32] = [(); size_of::<Kevent>()];

#[repr(C)]
struct Timespec {
    tv_sec: c_long,
    tv_nsec: c_long,
}

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: c_short,
    revents: c_short,
}

#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}

type SigSet = u32;

struct Shared<T>(UnsafeCell<T>);
// Every access is under `LOCK` (or is the pthread mutex itself).
unsafe impl<T> Sync for Shared<T> {}

static ALIASES: Shared<*mut Alias> = Shared(UnsafeCell::new(ptr::null_mut()));
static LOCK: Shared<PthreadMutex> = Shared(UnsafeCell::new(PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
}));

/* The shared pending set for the signals a signalfd watches (header): one
 * record per standard signal, the lowest number first on a read, as Linux
 * dequeues them. Under `lock`. */
static Q_REC: Shared<[SignalfdSiginfo; 65]> =
    Shared(UnsafeCell::new(unsafe { std::mem::zeroed() }));
// written under `lock`; read lock-free by lxrt_signalfd_unqueue
static Q_MASK: AtomicU64 = AtomicU64::new(0);

#[link(name = "System")]
unsafe extern "C" {
    fn __error() -> *mut c_int;
    fn calloc(count: usize, size: usize) -> *mut c_void;
    fn malloc(size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn close(fd: c_int) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn kqueue() -> c_int;
    fn kevent(
        kq: c_int,
        changelist: *const Kevent,
        nchanges: c_int,
        eventlist: *mut Kevent,
        nevents: c_int,
        timeout: *const Timespec,
    ) -> c_int;
    fn poll(fds: *mut PollFd, count: c_uint, timeout: c_int) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_sigmask(how: c_int, set: *const c_void, old: *mut c_void) -> c_int;
    fn sigpending(set: *mut SigSet) -> c_int;
    fn sigwait(set: *const SigSet, signal: *mut c_int) -> c_int;

    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_guest_clock_ns(clock: c_long) -> u64;
    fn lxrt_signo_to_darwin(linux_signal: c_int) -> c_int;
    fn lxrt_aids_capable(capability: c_int) -> bool;
    // signal.c: +1 while a signalfd watches `lsig`, -1 when it stops.
    fn lxrt_signal_keep(lsig: c_int, delta: c_int);
    // The weak definition at the bottom of this file, or a strong one from
    // signal.c: always called through the symbol so an override wins.
    fn lxrt_signal_dequeue_pending(mask: u64, out: *mut SignalfdSiginfo) -> c_int;
}

#[inline(always)]
fn lerr(error: c_int) -> c_int {
    unsafe { lxrt_errno_to_linux(error) }.wrapping_neg()
}

#[inline(always)]
fn errno() -> c_int {
    unsafe { *__error() }
}

#[inline(always)]
fn lock() {
    unsafe { pthread_mutex_lock(LOCK.0.get()) };
}

#[inline(always)]
fn unlock() {
    unsafe { pthread_mutex_unlock(LOCK.0.get()) };
}

#[inline(always)]
fn bit_of(lsig: c_int) -> u64 {
    1u64 << ((lsig - 1) as u32 & 63)
}

// Darwin's <signal.h> inline forms (sigismember and friends are macros there).
#[inline(always)]
fn sigbits(signo: c_int) -> SigSet {
    if signo == 0 || signo > DARWIN_NSIG {
        0
    } else {
        1u32 << ((signo - 1) as u32 & 31)
    }
}

#[inline(always)]
fn sig_is_member(set: SigSet, signo: c_int) -> bool {
    set & sigbits(signo) != 0
}

#[inline(always)]
fn ev_set(ident: usize, filter: i16, flags: u16, fflags: u32, data: isize) -> Kevent {
    Kevent {
        ident,
        filter,
        flags,
        fflags,
        data,
        udata: ptr::null_mut(),
    }
}

// LXRT_FORK_SAFE(timerfd_signalfd_lock, lock): taken before fork, released
// after it in both parent and child (the child's only thread is the one that
// took it).
extern "C" fn timerfd_signalfd_lock_fork_prepare() {
    lock();
}

extern "C" fn timerfd_signalfd_lock_fork_release() {
    unlock();
}

/* A fork child starts with nothing pending (Linux); the parent's queue is
 * the parent's. */
extern "C" fn q_child() {
    Q_MASK.store(0, Ordering::SeqCst);
}

// The C file had two constructors, priority 200 (the lock) then 201 (the
// queue); one initializer keeps that registration order.
extern "C" fn fork_register() {
    unsafe {
        let _ = pthread_atfork(
            Some(timerfd_signalfd_lock_fork_prepare),
            Some(timerfd_signalfd_lock_fork_release),
            Some(timerfd_signalfd_lock_fork_release),
        );
        let _ = pthread_atfork(None, None, Some(q_child));
    }
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FORK_REGISTER_INIT: extern "C" fn() = fork_register;

unsafe fn lookup(fd: c_int, kind: c_int) -> *mut State {
    let mut a = *ALIASES.0.get();
    while !a.is_null() {
        if (*a).fd == fd && (*(*a).s).kind == kind {
            return (*a).s;
        }
        a = (*a).next;
    }
    ptr::null_mut()
}

fn badfd(fd: c_int) -> c_long {
    lerr(if unsafe { fcntl(fd, F_GETFD) } < 0 {
        EBADF
    } else {
        EINVAL
    }) as c_long
}

fn is_kind(fd: c_int, kind: c_int) -> bool {
    lock();
    let yes = !unsafe { lookup(fd, kind) }.is_null();
    unlock();
    yes
}

#[no_mangle]
pub extern "C" fn lxrt_timerfd_is(fd: c_int) -> bool {
    is_kind(fd, 1)
}

#[no_mangle]
pub extern "C" fn lxrt_signalfd_is(fd: c_int) -> bool {
    is_kind(fd, 2)
}

/* Returns the mask of the signalfd state this call freed (0 if none), for the
 * caller to release with lxrt_signal_keep once the registry lock is dropped:
 * signal.c takes its own lock and calls sigaction() in there. */
unsafe fn drop_alias(fd: c_int, kind: c_int) -> u64 {
    let mut p: *mut *mut Alias = ALIASES.0.get();
    while !(*p).is_null() {
        let a = *p;
        if (*a).fd == fd && (*(*a).s).kind == kind {
            let mut freed = 0u64;
            *p = (*a).next;
            let s = (*a).s;
            (*s).refs = (*s).refs.wrapping_sub(1);
            if (*s).refs == 0 {
                freed = if kind == 2 { (*s).mask } else { 0 };
                free(s.cast());
            }
            free(a.cast());
            return freed;
        }
        p = ptr::addr_of_mut!((*a).next);
    }
    0
}

fn keep_bits(mut bits: u64, delta: c_int) {
    let mut l: c_int = 1;
    while l <= 64 && bits != 0 {
        if bits & 1 != 0 {
            unsafe { lxrt_signal_keep(l, delta) };
        }
        l += 1;
        bits >>= 1;
    }
}

fn close_kind(fd: c_int, kind: c_int) {
    lock();
    let freed = unsafe { drop_alias(fd, kind) };
    unlock();
    if freed != 0 {
        keep_bits(freed, -1);
    }
}

#[no_mangle]
pub extern "C" fn lxrt_timerfd_close(fd: c_int) {
    close_kind(fd, 1);
}

#[no_mangle]
pub extern "C" fn lxrt_signalfd_close(fd: c_int) {
    close_kind(fd, 2);
}

fn dup_kind(oldfd: c_int, newfd: c_int, kind: c_int) {
    if oldfd == newfd {
        return;
    }
    lock();
    let freed = unsafe {
        let s = lookup(oldfd, kind);
        let freed = drop_alias(newfd, kind);
        if !s.is_null() {
            let a = malloc(size_of::<Alias>()).cast::<Alias>();
            /* The void hook cannot report allocation failure. No fixed alias cap. */
            if !a.is_null() {
                a.write(Alias {
                    fd: newfd,
                    s,
                    next: *ALIASES.0.get(),
                });
                *ALIASES.0.get() = a;
                (*s).refs = (*s).refs.wrapping_add(1);
            }
        }
        freed
    };
    unlock();
    if freed != 0 {
        keep_bits(freed, -1);
    }
}

#[no_mangle]
pub extern "C" fn lxrt_timerfd_dup(oldfd: c_int, newfd: c_int) {
    dup_kind(oldfd, newfd, 1);
}

#[no_mangle]
pub extern "C" fn lxrt_signalfd_dup(oldfd: c_int, newfd: c_int) {
    dup_kind(oldfd, newfd, 2);
}

unsafe fn create(kind: c_int, flags: c_int, clock: c_int) -> c_int {
    let s = calloc(1, size_of::<State>()).cast::<State>();
    let a = malloc(size_of::<Alias>()).cast::<Alias>();
    if s.is_null() || a.is_null() {
        free(s.cast());
        free(a.cast());
        return lerr(ENOMEM);
    }
    let fd = kqueue();
    if fd < 0 {
        let e = errno();
        free(s.cast());
        free(a.cast());
        return lerr(e);
    }
    if fcntl(
        fd,
        F_SETFD,
        if flags & CLOEXEC != 0 { FD_CLOEXEC } else { 0 },
    ) < 0
    {
        let e = errno();
        close(fd);
        free(s.cast());
        free(a.cast());
        return lerr(e);
    }
    /* kqueue does not support F_SETFL(O_NONBLOCK); keep the flag in state. */
    (*s).refs = 1;
    (*s).kind = kind;
    (*s).flags = flags;
    (*s).clock = clock;
    a.write(Alias {
        fd,
        s,
        next: *ALIASES.0.get(),
    });
    *ALIASES.0.get() = a;
    fd
}

// The same counter the guest reads through clock_gettime (dispatch.c): an
// absolute deadline the guest computed from ITS clock is only meaningful
// against that clock. Darwin's CLOCK_MONOTONIC is a different base (measured
// 3.8 s apart here) and ABSTIME never fired.
// `clock` is the guest's own clock id (0 REALTIME, 1 MONOTONIC, 7 BOOTTIME,
// 8/9 their _ALARM forms): BOOTTIME is not MONOTONIC here, it counts the
// Mac's sleep (lxrt_guest_clock_ns), and AlarmManagerService arms its
// elapsed-realtime timers with BOOTTIME deadlines -- measured against
// MONOTONIC they came due as much later as the Mac had slept since boot.
fn now(clock: c_int) -> Ticks {
    unsafe { lxrt_guest_clock_ns(clock as c_long) as Ticks }
}

fn to_ticks(t: LTime) -> Ticks {
    (t.sec as Ticks).wrapping_mul(NS).wrapping_add(t.nsec as Ticks)
}

// Truncating division that cannot trap: the callers only divide by NS or by a
// non-zero interval, as the C did.
#[inline(always)]
fn div(a: Ticks, b: Ticks) -> Ticks {
    a.checked_div(b).unwrap_or(0)
}

fn to_time(t: Ticks) -> LTime {
    // The remainder from the quotient, as clang lowered `t % NS`: one
    // __divti3 call and no __modti3.
    let sec = div(t, NS);
    LTime {
        sec: sec as i64,
        nsec: t.wrapping_sub(sec.wrapping_mul(NS)) as i64,
    }
}

fn valid(t: LTime) -> bool {
    t.sec >= 0 && t.nsec >= 0 && (t.nsec as Ticks) < NS
}

unsafe fn snapshot(s: *const State) -> LSpec {
    let next = (*s).next;
    let interval = (*s).interval;
    let mut left = if next != 0 {
        next.wrapping_sub(now((*s).clock))
    } else {
        0
    };
    if left <= 0 && next != 0 && interval != 0 {
        left = left.wrapping_add(
            div(left.wrapping_neg(), interval)
                .wrapping_add(1)
                .wrapping_mul(interval),
        );
    }
    LSpec {
        interval: to_time(interval),
        value: to_time(if left > 0 { left } else { 0 }),
    }
}

unsafe fn arm(fd: c_int, s: *const State) -> c_int {
    let mut delay = (*s).next.wrapping_sub(now((*s).clock));
    if delay < 1 {
        delay = 1;
    }
    if delay > i64::MAX as Ticks {
        delay = i64::MAX as Ticks;
    }
    let e = ev_set(
        1,
        EVFILT_TIMER,
        EV_ADD | EV_ONESHOT,
        NOTE_NSECONDS,
        delay as isize,
    );
    kevent(fd, &e, 1, ptr::null_mut(), 0, ptr::null())
}

#[no_mangle]
pub extern "C" fn lxrt_timerfd_create(clockid: c_int, flags: c_int) -> c_long {
    // CLOCK_REALTIME 0, MONOTONIC 1, BOOTTIME 7, REALTIME_ALARM 8,
    // BOOTTIME_ALARM 9. The alarm clocks are their base clocks that would
    // also wake a suspended machine; a Mac guest cannot suspend anything, so
    // they are the base clocks here. Linux wants CAP_WAKE_ALARM (35) for
    // them (fs/timerfd.c): Android's AlarmManagerService makes one of each
    // in system_server, which has it, and dies without them ("kernel does
    // not support timerfd_create() with alarm timers", MEASURED at stage 28).
    if (flags & !(NONBLOCK | CLOEXEC)) != 0
        || (clockid != 0 && clockid != 1 && clockid != 7 && clockid != 8 && clockid != 9)
    {
        return lerr(EINVAL) as c_long;
    }
    if (clockid == 8 || clockid == 9) && !unsafe { lxrt_aids_capable(35) } {
        return lerr(EPERM) as c_long;
    }
    lock();
    let fd = unsafe { create(1, flags, clockid) };
    unlock();
    fd as c_long
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_timerfd_settime(
    fd: c_int,
    flags: c_int,
    input: *const c_void,
    out: *mut c_void,
) -> c_long {
    // TFD_TIMER_ABSTIME 1, TFD_TIMER_CANCEL_ON_SET 2 (Linux's
    // TFD_SETTIME_FLAGS). CANCEL_ON_SET asks for ECANCELED when the wall
    // clock is set; it is accepted and never fires: the runtime sees no
    // clock change (AlarmManagerService arms one and dies if the call fails).
    if flags & !3 != 0 {
        return lerr(EINVAL) as c_long;
    }
    if input.is_null() {
        return lerr(EFAULT) as c_long;
    }
    let spec = ptr::read_unaligned(input.cast::<LSpec>());
    if !valid(spec.interval) || !valid(spec.value) {
        return lerr(EINVAL) as c_long;
    }
    lock();
    let s = lookup(fd, 1);
    if s.is_null() {
        unlock();
        return badfd(fd);
    }
    let old = snapshot(s);
    let mut replacement = *s;
    replacement.interval = to_ticks(spec.interval);
    replacement.next = to_ticks(spec.value);
    if replacement.next != 0 && flags & 1 == 0 {
        replacement.next = replacement.next.wrapping_add(now((*s).clock));
    }
    let mut rc;
    if replacement.next != 0 {
        rc = arm(fd, &replacement);
    } else {
        let e = ev_set(1, EVFILT_TIMER, EV_DELETE, 0, 0);
        rc = kevent(fd, &e, 1, ptr::null_mut(), 0, ptr::null());
        if rc < 0 && errno() == ENOENT {
            rc = 0;
        }
    }
    let result: c_long = if rc < 0 { lerr(errno()) as c_long } else { 0 };
    if result == 0 {
        *s = replacement;
        if !out.is_null() {
            ptr::write_unaligned(out.cast::<LSpec>(), old);
        }
    }
    unlock();
    result
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_timerfd_gettime(fd: c_int, out: *mut c_void) -> c_long {
    if out.is_null() {
        return lerr(EFAULT) as c_long;
    }
    lock();
    let s = lookup(fd, 1);
    if s.is_null() {
        unlock();
        return badfd(fd);
    }
    let spec = snapshot(s);
    ptr::write_unaligned(out.cast::<LSpec>(), spec);
    unlock();
    0
}

/* Never hold the registry lock across a blocking wait. Recheck state on wake. */
fn wait_readable(fd: c_int, nonblock: bool) -> c_int {
    let mut p = PollFd {
        fd,
        events: POLLIN,
        revents: 0,
    };
    let rc = unsafe { poll(&mut p, 1, if nonblock { 0 } else { -1 }) };
    if rc > 0 && (p.revents & POLLNVAL) != 0 {
        unsafe { *__error() = EBADF };
        return -1;
    }
    rc
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_timerfd_read(fd: c_int, buf: *mut c_void, len: usize) -> c_long {
    if len < 8 {
        return lerr(EINVAL) as c_long;
    }
    if buf.is_null() {
        return lerr(EFAULT) as c_long;
    }
    loop {
        lock();
        let s = lookup(fd, 1);
        if s.is_null() {
            unlock();
            return badfd(fd);
        }
        let nb = (*s).flags & NONBLOCK != 0;
        let mut e = MaybeUninit::<Kevent>::uninit();
        let zero = Timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        let n = kevent(fd, ptr::null(), 0, e.as_mut_ptr(), 1, &zero);
        let mut error: c_long = if n < 0 { lerr(errno()) as c_long } else { 0 };
        let t = now((*s).clock);
        if n > 0 && (*s).next != 0 && t >= (*s).next {
            let interval = (*s).interval;
            let count: Ticks = if interval != 0 {
                div(t.wrapping_sub((*s).next), interval).wrapping_add(1)
            } else {
                1
            };
            let value: u64 = if count > u64::MAX as Ticks {
                u64::MAX
            } else {
                count as u64
            };
            if interval != 0 {
                (*s).next = (*s).next.wrapping_add(count.wrapping_mul(interval));
            } else {
                (*s).next = 0;
            }
            if (*s).next != 0 && arm(fd, s) < 0 {
                error = lerr(errno()) as c_long;
            }
            if error == 0 {
                ptr::write_unaligned(buf.cast::<u64>(), value);
            }
            unlock();
            return if error != 0 { error } else { 8 };
        }
        /* An extremely distant timer is chunked at INT64_MAX nanoseconds. */
        if n > 0 && (*s).next != 0 && arm(fd, s) < 0 {
            error = lerr(errno()) as c_long;
        }
        unlock();
        if error != 0 {
            return error;
        }
        if nb {
            return lerr(EAGAIN) as c_long;
        }
        if wait_readable(fd, false) < 0 {
            return lerr(errno()) as c_long;
        }
    }
}

unsafe fn signal_changes(fd: c_int, before: u64, after: u64) -> c_int {
    for l in 1..=64 as c_int {
        let bit = bit_of(l);
        let d = lxrt_signo_to_darwin(l);
        if d == 0 || (before ^ after) & bit == 0 {
            continue;
        }
        let e = ev_set(
            d as usize,
            EVFILT_SIGNAL,
            if after & bit != 0 {
                EV_ADD | EV_CLEAR
            } else {
                EV_DELETE
            },
            0,
            0,
        );
        if kevent(fd, &e, 1, ptr::null_mut(), 0, ptr::null()) < 0 {
            return -1;
        }
    }
    0
}

#[inline(always)]
unsafe fn user_trigger(fd: c_int) {
    let e = ev_set(2, EVFILT_USER, EV_ADD | EV_CLEAR, NOTE_TRIGGER, 0);
    let _ = kevent(fd, &e, 1, ptr::null_mut(), 0, ptr::null());
}

/* Preserve level readiness when several standard signals are pending, and
 * expose signals that were already pending when the descriptor was created. */
unsafe fn signal_ready(fd: c_int, mask: u64) {
    if Q_MASK.load(Ordering::SeqCst) & mask != 0 {
        user_trigger(fd);
        return;
    }
    let mut p: SigSet = 0;
    if sigpending(&mut p) < 0 {
        return;
    }
    for l in 1..=64 as c_int {
        let d = lxrt_signo_to_darwin(l);
        if d != 0 && (mask & bit_of(l)) != 0 && sig_is_member(p, d) {
            user_trigger(fd);
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
unsafe fn q_wake_all(bits: u64) {
    let mut a = *ALIASES.0.get();
    while !a.is_null() {
        let s = (*a).s;
        if (*s).kind == 2 && ((*s).mask & bits) != 0 {
            user_trigger((*a).fd);
        }
        a = (*a).next;
    }
}

#[no_mangle]
pub extern "C" fn lxrt_signalfd_notify(lsig: c_int) {
    if !(1..=64).contains(&lsig) {
        return;
    }
    lock();
    unsafe { q_wake_all(bit_of(lsig)) };
    unlock();
}

unsafe fn watched_locked(bit: u64) -> bool {
    let mut a = *ALIASES.0.get();
    while !a.is_null() {
        let s = (*a).s;
        if (*s).kind == 2 && ((*s).mask & bit) != 0 {
            return true;
        }
        a = (*a).next;
    }
    false
}

#[no_mangle]
pub extern "C" fn lxrt_signalfd_watched(lsig: c_int) -> bool {
    if !(1..=64).contains(&lsig) {
        return false;
    }
    lock();
    let yes = unsafe { watched_locked(bit_of(lsig)) };
    unlock();
    yes
}

// A process-directed signal every guest thread blocks, offered by signal.c
// (header). Steam's fossilize_replay: SIGCHLD blocked, left at SIG_DFL, a
// blocking signalfd read after epoll says readable -- the kqueue's signal
// filter fired at post time (it does, even for a signal Darwin then drops),
// the read found nothing pending on its thread and slept for good; the
// shader pre-cache never ended (benchmarks/stage51, section 10).
#[no_mangle]
pub unsafe extern "C" fn lxrt_signalfd_take(rec: *const SignalfdSiginfo) -> bool {
    let lsig = (*rec).ssi_signo as c_int;
    if !(1..=64).contains(&lsig) {
        return false;
    }
    let bit = bit_of(lsig);
    lock();
    let yes = watched_locked(bit);
    if yes {
        if Q_MASK.load(Ordering::SeqCst) & bit == 0 {
            Q_REC
                .0
                .get()
                .cast::<SignalfdSiginfo>()
                .add(lsig as usize)
                .write(*rec);
            Q_MASK.fetch_or(bit, Ordering::SeqCst);
        }
        q_wake_all(bit);
    }
    unlock();
    yes
}

unsafe fn q_take(mask: u64, out: *mut SignalfdSiginfo) -> c_int {
    let have = Q_MASK.load(Ordering::SeqCst) & mask;
    if have == 0 {
        return 0;
    }
    let l = have.trailing_zeros() as usize + 1;
    out.write(Q_REC.0.get().cast::<SignalfdSiginfo>().add(l).read());
    Q_MASK.fetch_and(!(1u64 << (l - 1)), Ordering::SeqCst);
    1
}

// A thread unblocks `lmask` (rt_sigprocmask, rt_sigsuspend): what the queue
// holds of it is that thread's now, as Linux's dequeue of the shared pending
// set on the first thread to unblock; the caller raises each on itself. The
// records are dropped (a handler's siginfo loses the sender; the sigframe
// fills si_pid as it always did).
#[no_mangle]
pub extern "C" fn lxrt_signalfd_unqueue(lmask: u64) -> u64 {
    if Q_MASK.load(Ordering::Relaxed) & lmask == 0 {
        return 0;
    }
    lock();
    let got = Q_MASK.load(Ordering::SeqCst) & lmask;
    Q_MASK.fetch_and(!got, Ordering::SeqCst);
    unlock();
    got
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_signalfd4(
    mut fd: c_int,
    mask: *const u64,
    size: usize,
    flags: c_int,
) -> c_long {
    if size != 8 || (flags & !(NONBLOCK | CLOEXEC)) != 0 {
        return lerr(EINVAL) as c_long;
    }
    if mask.is_null() {
        return lerr(EFAULT) as c_long;
    }
    let mut m = ptr::read_unaligned(mask);
    m &= !((1u64 << 8) | (1u64 << 18));
    lock();
    let fresh = fd == -1;
    if fresh {
        fd = create(2, flags, 1);
    }
    if fd < 0 && fresh {
        unlock();
        return fd as c_long;
    }
    let s = lookup(fd, 2);
    if s.is_null() {
        unlock();
        return badfd(fd);
    }
    if signal_changes(fd, (*s).mask, m) < 0 {
        let e = errno();
        /* Restore all registrations, including a partially applied update. */
        signal_changes(fd, m | (*s).mask, 0);
        signal_changes(fd, 0, (*s).mask);
        if fresh {
            drop_alias(fd, 2);
            close(fd);
        }
        unlock();
        return lerr(e) as c_long;
    }
    let added = m & !(*s).mask;
    let removed = (*s).mask & !m;
    (*s).mask = m;
    signal_ready(fd, m);
    unlock();
    /* The host keeps a handler on each watched signal (lxrt_signal_keep);
     * after the registry lock, signal.c locks and calls sigaction(). */
    if added != 0 {
        keep_bits(added, 1);
    }
    if removed != 0 {
        keep_bits(removed, -1);
    }
    fd as c_long
}

// The native fallback behind the weak `lxrt_signal_dequeue_pending` symbol.
unsafe extern "C" fn signal_dequeue_pending_fallback(
    mask: u64,
    out: *mut SignalfdSiginfo,
) -> c_int {
    ptr::write_bytes(out.cast::<u8>(), 0, INFO_SIZE);
    let mut p: SigSet = 0;
    let mut blocked: SigSet = 0;
    if sigpending(&mut p) < 0 {
        return lerr(errno());
    }
    let mut rc = pthread_sigmask(SIG_BLOCK, ptr::null(), ptr::addr_of_mut!(blocked).cast());
    if rc != 0 {
        return lerr(rc);
    }
    for l in 1..=64 as c_int {
        let d = lxrt_signo_to_darwin(l);
        if (mask & bit_of(l)) == 0 || d == 0 || !sig_is_member(p, d) || !sig_is_member(blocked, d)
        {
            continue;
        }
        let one: SigSet = sigbits(d);
        let mut got: c_int = 0;
        rc = sigwait(&one, &mut got);
        if rc != 0 {
            return lerr(rc);
        }
        (*out).ssi_signo = l as u32;
        (*out).ssi_code = 0x80; /* SI_KERNEL: Darwin sigwait has no siginfo. */
        return 1;
    }
    0
}

// `__attribute__((weak)) int lxrt_signal_dequeue_pending(...)`: stable Rust has
// no weak linkage attribute, so the exported weak definition is a one-branch
// thunk to the fallback above. A strong definition elsewhere replaces it.
global_asm!(
    ".section __TEXT,__text,regular,pure_instructions",
    ".globl _lxrt_signal_dequeue_pending",
    ".weak_definition _lxrt_signal_dequeue_pending",
    ".p2align 2",
    "_lxrt_signal_dequeue_pending:",
    "b {fallback}",
    fallback = sym signal_dequeue_pending_fallback,
);

#[no_mangle]
pub unsafe extern "C" fn lxrt_signalfd_read(fd: c_int, buf: *mut c_void, len: usize) -> c_long {
    if len < INFO_SIZE {
        return lerr(EINVAL) as c_long;
    }
    if buf.is_null() {
        return lerr(EFAULT) as c_long;
    }
    let mut used: usize = 0;
    loop {
        lock();
        let mut s = lookup(fd, 2);
        if s.is_null() {
            unlock();
            return if used != 0 { used as c_long } else { badfd(fd) };
        }
        let nb = (*s).flags & NONBLOCK != 0;
        /* Clear stale wakeups before querying pending state; reverse ordering
         * could consume the notification for a signal arriving after dequeue. */
        let mut events = MaybeUninit::<[Kevent; 32]>::uninit();
        let zero = Timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        let n = kevent(fd, ptr::null(), 0, events.as_mut_ptr().cast(), 32, &zero);
        let mask = (*s).mask;
        let error: c_int = if n < 0 { lerr(errno()) } else { 0 };
        let mut info = MaybeUninit::<SignalfdSiginfo>::uninit();
        /* The shared pending set first (any thread), then this thread's own. */
        let mut rc = if error != 0 {
            error
        } else {
            q_take(mask, info.as_mut_ptr())
        };
        unlock();
        if rc == 0 {
            rc = lxrt_signal_dequeue_pending(mask, info.as_mut_ptr());
        }
        if rc > 0 {
            lock();
            s = lookup(fd, 2);
            if !s.is_null() {
                signal_ready(fd, (*s).mask);
            }
            unlock();
        }
        if rc > 0 {
            ptr::copy_nonoverlapping(
                info.as_ptr().cast::<u8>(),
                buf.cast::<u8>().add(used),
                INFO_SIZE,
            );
            used += INFO_SIZE;
            if len - used < INFO_SIZE {
                return used as c_long;
            }
            continue;
        }
        if used != 0 {
            return used as c_long;
        }
        if rc < 0 {
            return rc as c_long;
        }
        if nb {
            return lerr(EAGAIN) as c_long;
        }
        if wait_readable(fd, false) < 0 {
            return lerr(errno()) as c_long;
        }
    }
}
