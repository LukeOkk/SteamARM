// Linux per-process POSIX timers, independent of file descriptors.

use std::ffi::{c_int, c_long, c_void};
use std::mem::{size_of, MaybeUninit};
use std::ptr;
use std::sync::{Condvar, Mutex, MutexGuard, Once};
use std::thread;
use std::time::Duration;

include!(concat!(env!("OUT_DIR"), "/timer_constants.rs"));

const NS: i128 = 1_000_000_000;
const LINUX_ESRCH: c_long = -3;
const LINUX_EFAULT: c_long = -14;
const LINUX_EINVAL: c_long = -22;
const LINUX_EPERM: c_long = -1;
const LINUX_EAGAIN: c_long = -11;
const LINUX_ENOMEM: c_long = -12;
const LINUX_ENOSYS: c_long = -38;

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct LTime {
    sec: i64,
    nsec: i64,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct LSpec {
    interval: LTime,
    value: LTime,
}

const _: [(); 32] = [(); size_of::<LSpec>()];

struct Timer {
    clock: c_int,
    notify: c_int,
    signo: c_int,
    tid: c_int,
    #[allow(dead_code)]
    sigval: u64,
    next: i128,
    interval: i128,
    overrun: c_int,
    delivering: bool,
}

struct State {
    timers: Vec<Option<Timer>>,
    worker_started: bool,
}

static STATE: Mutex<State> = Mutex::new(State {
    timers: Vec::new(),
    worker_started: false,
});

// Reinitialized in the fork child because its helper thread and waiters no
// longer exist there. The old value is intentionally overwritten, not dropped.
struct ForkCondvar(std::cell::UnsafeCell<Condvar>);
unsafe impl Sync for ForkCondvar {}

impl ForkCondvar {
    const fn new() -> Self {
        Self(std::cell::UnsafeCell::new(Condvar::new()))
    }

    fn get(&self) -> &Condvar {
        unsafe { &*self.0.get() }
    }

    unsafe fn reset_after_fork(&self) {
        ptr::write(self.0.get(), Condvar::new());
    }
}

static CHANGED: ForkCondvar = ForkCondvar::new();
static FORK_HOOKS: Once = Once::new();
type StateGuard = MutexGuard<'static, State>;
static mut FORK_GUARD: MaybeUninit<StateGuard> = MaybeUninit::uninit();

#[link(name = "System")]
unsafe extern "C" {
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn pthread_sigmask(how: c_int, set: *const c_void, oldset: *mut c_void) -> c_int;
    fn sigfillset(set: *mut c_void) -> c_int;
    fn lxrt_guest_clock_ns(clock: c_long) -> u64;
    fn lxrt_aids_capable(capability: c_int) -> bool;
    fn lxrt_thread_lookup(tid: c_int, out: *mut usize) -> bool;
    fn lxrt_tgkill(tgid: c_int, tid: c_int, signal: c_int) -> c_long;
    fn lxrt_kill(pid: c_int, signal: c_int) -> c_long;
}

fn lock_state() -> StateGuard {
    STATE
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

extern "C" fn fork_prepare() {
    let guard = lock_state();
    unsafe {
        ptr::addr_of_mut!(FORK_GUARD)
            .cast::<StateGuard>()
            .write(guard);
    }
}

extern "C" fn fork_parent() {
    unsafe {
        ptr::drop_in_place(ptr::addr_of_mut!(FORK_GUARD).cast::<StateGuard>());
    }
}

extern "C" fn fork_child() {
    unsafe {
        ptr::drop_in_place(ptr::addr_of_mut!(FORK_GUARD).cast::<StateGuard>());
    }
    {
        let mut state = lock_state();
        state.timers.clear();
        state.worker_started = false;
    }
    unsafe { CHANGED.reset_after_fork() };
}

extern "C" fn register_fork_hooks() {
    FORK_HOOKS.call_once(|| unsafe {
        let _ = pthread_atfork(Some(fork_prepare), Some(fork_parent), Some(fork_child));
    });
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FORK_HOOKS_INIT: extern "C" fn() = register_fork_hooks;

fn ensure_fork_hooks() {
    register_fork_hooks();
}

fn linux_error(host_errno: c_int) -> c_long {
    -(crate::errno_map::map_errno(host_errno) as c_long)
}

fn now(clock: c_int) -> i128 {
    unsafe { lxrt_guest_clock_ns(clock as c_long) as i128 }
}

fn to_ticks(value: LTime) -> i128 {
    value.sec as i128 * NS + value.nsec as i128
}

fn to_time(value: i128) -> LTime {
    LTime {
        sec: (value / NS) as i64,
        nsec: (value % NS) as i64,
    }
}

fn valid(value: LTime) -> bool {
    value.sec >= 0 && value.nsec >= 0 && value.nsec < NS as i64
}

fn snapshot(timer: &Timer) -> LSpec {
    let mut left = if timer.next != 0 {
        timer.next - now(timer.clock)
    } else {
        0
    };
    if left <= 0 && timer.next != 0 && timer.interval != 0 {
        left += ((-left) / timer.interval + 1) * timer.interval;
    }
    LSpec {
        interval: to_time(timer.interval),
        value: to_time(left.max(0)),
    }
}

fn timer_worker() {
    let mut state = lock_state();
    loop {
        let mut due = None;
        let mut shortest = 0i128;
        for (id, slot) in state.timers.iter().enumerate() {
            let Some(timer) = slot.as_ref() else {
                continue;
            };
            if timer.next == 0 {
                continue;
            }
            let delay = timer.next - now(timer.clock);
            if delay <= 0 {
                due = Some(id);
                break;
            }
            if shortest == 0 || delay < shortest {
                shortest = delay;
            }
        }

        let Some(id) = due else {
            if shortest == 0 {
                state = CHANGED
                    .get()
                    .wait(state)
                    .unwrap_or_else(std::sync::PoisonError::into_inner);
            } else {
                // Guest and host clock changes are observed within one second.
                let timeout_ns = shortest.min(NS) as u64;
                state = CHANGED
                    .get()
                    .wait_timeout(state, Duration::from_nanos(timeout_ns))
                    .unwrap_or_else(std::sync::PoisonError::into_inner)
                    .0;
            }
            continue;
        };

        let (notify, signo, tid) = {
            let timer = state.timers[id].as_mut().expect("due timer exists");
            let count = if timer.interval != 0 {
                1 + (now(timer.clock) - timer.next) / timer.interval
            } else {
                1
            };
            if timer.interval != 0 {
                timer.next += count * timer.interval;
            } else {
                timer.next = 0;
            }
            timer.overrun = (count - 1).min(c_int::MAX as i128) as c_int;
            timer.delivering = true;
            (timer.notify, timer.signo, timer.tid)
        };

        drop(state);
        unsafe {
            if notify == 4 {
                let rc = lxrt_tgkill(0, tid, signo);
                if rc == LINUX_ESRCH {
                    lxrt_kill(std::process::id() as c_int, signo);
                }
            } else if notify == 0 || notify == 2 {
                lxrt_kill(std::process::id() as c_int, signo);
            }
        }
        state = lock_state();
        if let Some(Some(timer)) = state.timers.get_mut(id) {
            timer.delivering = false;
        }
        CHANGED.get().notify_all();
    }
}

fn start_worker() -> Result<(), c_int> {
    // The helper inherits a full signal mask so host delivery cannot consume
    // a process-directed signal intended for a guest thread.
    let mut all = [0u64; 16];
    let mut previous = [0u64; 16];
    unsafe {
        let _ = sigfillset(all.as_mut_ptr().cast());
        let _ = pthread_sigmask(
            D_SIG_BLOCK,
            all.as_ptr().cast(),
            previous.as_mut_ptr().cast(),
        );
    }
    let spawned = thread::Builder::new()
        .name("steamarm-posix-timer".to_owned())
        .spawn(timer_worker);
    unsafe {
        let _ = pthread_sigmask(D_SIG_SETMASK, previous.as_ptr().cast(), ptr::null_mut());
    }
    spawned
        .map(|handle| drop(handle))
        .map_err(|error| error.raw_os_error().unwrap_or(5))
}

#[no_mangle]
pub extern "C" fn lxrt_posix_timer_create(
    clockid: c_int,
    event: *const c_void,
    id_out: *mut c_int,
) -> c_long {
    ensure_fork_hooks();
    if clockid != 0 && clockid != 1 && clockid != 7 && clockid != 8 && clockid != 9 {
        return LINUX_EINVAL;
    }
    if (clockid == 8 || clockid == 9) && !unsafe { lxrt_aids_capable(35) } {
        return LINUX_EPERM;
    }
    if id_out.is_null() {
        return LINUX_EFAULT;
    }

    let mut notify = 0;
    let mut signo = 14;
    let mut tid = 0;
    let mut sigval = 0u64;
    if !event.is_null() {
        unsafe {
            let raw = event.cast::<u8>();
            sigval = ptr::read_unaligned(raw.cast::<u64>());
            signo = ptr::read_unaligned(raw.add(8).cast::<c_int>());
            notify = ptr::read_unaligned(raw.add(12).cast::<c_int>());
            tid = ptr::read_unaligned(raw.add(16).cast::<c_int>());
        }
        if notify != 0 && notify != 1 && notify != 2 && notify != 4 {
            return LINUX_EINVAL;
        }
        if notify != 1 && !(1..=64).contains(&signo) {
            return LINUX_EINVAL;
        }
        // SIGEV_THREAD uses realtime signal metadata the signal runtime does
        // not yet preserve, so callers retain their existing ENOSYS fallback.
        if notify != 1 && signo >= 32 {
            return LINUX_ENOSYS;
        }
        if notify == 4 {
            let mut target = 0usize;
            if !unsafe { lxrt_thread_lookup(tid, &mut target) } {
                return LINUX_EINVAL;
            }
        }
    }

    let mut state = lock_state();
    let id = state
        .timers
        .iter()
        .position(|slot| slot.is_none())
        .unwrap_or(state.timers.len());
    if id >= c_int::MAX as usize {
        return LINUX_EAGAIN;
    }
    if id == state.timers.len() {
        let current_len = state.timers.len();
        let next = if id == 0 {
            8
        } else {
            current_len.saturating_mul(2).min(c_int::MAX as usize)
        };
        if next <= id || state.timers.try_reserve(next - current_len).is_err() {
            return LINUX_ENOMEM;
        }
        state.timers.resize_with(next, || None);
    }
    if event.is_null() {
        sigval = id as u32 as u64;
    }
    state.timers[id] = Some(Timer {
        clock: clockid,
        notify,
        signo,
        tid,
        sigval,
        next: 0,
        interval: 0,
        overrun: 0,
        delivering: false,
    });
    unsafe { ptr::write_unaligned(id_out, id as c_int) };
    0
}

#[no_mangle]
pub extern "C" fn lxrt_posix_timer_gettime(id: c_int, out: *mut c_void) -> c_long {
    ensure_fork_hooks();
    if out.is_null() {
        return LINUX_EFAULT;
    }
    let state = lock_state();
    let Some(Some(timer)) = state.timers.get(id.max(0) as usize).filter(|_| id >= 0) else {
        return LINUX_EINVAL;
    };
    unsafe { ptr::write_unaligned(out.cast::<LSpec>(), snapshot(timer)) };
    0
}

#[no_mangle]
pub extern "C" fn lxrt_posix_timer_getoverrun(id: c_int) -> c_long {
    ensure_fork_hooks();
    let state = lock_state();
    state
        .timers
        .get(id.max(0) as usize)
        .filter(|_| id >= 0)
        .and_then(Option::as_ref)
        .map(|timer| timer.overrun as c_long)
        .unwrap_or(LINUX_EINVAL)
}

#[no_mangle]
pub extern "C" fn lxrt_posix_timer_settime(
    id: c_int,
    flags: c_int,
    input: *const c_void,
    old: *mut c_void,
) -> c_long {
    ensure_fork_hooks();
    if flags & !1 != 0 {
        return LINUX_EINVAL;
    }
    if input.is_null() {
        return LINUX_EFAULT;
    }
    let spec = unsafe { ptr::read_unaligned(input.cast::<LSpec>()) };
    if !valid(spec.interval) || !valid(spec.value) {
        return LINUX_EINVAL;
    }

    let mut state = lock_state();
    let Some(Some(_)) = state.timers.get(id.max(0) as usize).filter(|_| id >= 0) else {
        return LINUX_EINVAL;
    };
    if (spec.value.sec != 0 || spec.value.nsec != 0) && !state.worker_started {
        if let Err(error) = start_worker() {
            return linux_error(error);
        }
        state.worker_started = true;
    }

    let timer = state.timers[id as usize].as_mut().expect("timer checked");
    let previous = snapshot(timer);
    timer.interval = to_ticks(spec.interval);
    timer.next = to_ticks(spec.value);
    if timer.next != 0 && flags & 1 == 0 {
        timer.next += now(timer.clock);
    }
    if !old.is_null() {
        unsafe { ptr::write_unaligned(old.cast::<LSpec>(), previous) };
    }
    CHANGED.get().notify_all();
    0
}

#[no_mangle]
pub extern "C" fn lxrt_posix_timer_delete(id: c_int) -> c_long {
    ensure_fork_hooks();
    let mut state = lock_state();
    let Some(Some(timer)) = state.timers.get_mut(id.max(0) as usize).filter(|_| id >= 0) else {
        return LINUX_EINVAL;
    };
    timer.next = 0;
    CHANGED.get().notify_all();
    while state.timers[id as usize]
        .as_ref()
        .is_some_and(|timer| timer.delivering)
    {
        state = CHANGED
            .get()
            .wait(state)
            .unwrap_or_else(std::sync::PoisonError::into_inner);
    }
    state.timers[id as usize] = None;
    0
}
