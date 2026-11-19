// Linux futex_waitv backed by a cross-process board of shared ulocks.

use std::ffi::{c_char, c_int, c_long, c_void, CStr, CString};
use std::hint::spin_loop;
use std::mem::{offset_of, size_of};
use std::ptr;
use std::sync::atomic::{self, AtomicBool, AtomicPtr, AtomicU32, AtomicU8, Ordering};

#[allow(dead_code)]
mod generated {
    include!(concat!(env!("OUT_DIR"), "/futex_waitv_constants.rs"));
}

use generated::{
    D_EAGAIN, D_EINTR, D_EINVAL, D_ENOMEM, D_ETIMEDOUT, D_MAP_SHARED, D_O_CLOEXEC, D_O_CREAT,
    D_O_RDWR, D_PROT_READ, D_PROT_WRITE,
};

const BUCKETS: usize = 1_024;
const WAITV_MAX: usize = 128;
const BOARD_BYTES: usize = 16_384;
const FUTEX2_SIZE_U32: u32 = 0x02;
const FUTEX2_SIZE_MASK: u32 = 0x03;
const FUTEX2_PRIVATE: u32 = 128;
const UL_COMPARE_AND_WAIT_SHARED: u32 = 3;
const ULF_WAKE_ALL: u32 = 0x0000_0100;
const ULF_NO_ERRNO: u32 = 0x0100_0000;
const BOARD_UNINITIALIZED: u8 = 0;
const BOARD_INITIALIZING: u8 = 1;
const BOARD_READY: u8 = 2;
const BOARD_FAILED: u8 = 3;

#[repr(C)]
struct Board {
    generation: [AtomicU32; BUCKETS],
    single_waiters: [AtomicU32; BUCKETS],
    multi_waiters: [AtomicU32; BUCKETS],
    multi_generation: AtomicU32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxFutexWaitv {
    value: u64,
    address: u64,
    flags: u32,
    reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct WaitvTimespec {
    seconds: i64,
    nanoseconds: i64,
}

const _: [(); 12_292] = [(); size_of::<Board>()];
const _: [(); 16_384] = [(); BOARD_BYTES];
const _: [(); 24] = [(); size_of::<LinuxFutexWaitv>()];
const _: [(); 8] = [(); offset_of!(LinuxFutexWaitv, address)];
const _: [(); 16] = [(); offset_of!(LinuxFutexWaitv, flags)];
const _: [(); 16] = [(); size_of::<WaitvTimespec>()];

static BOARD: AtomicPtr<Board> = AtomicPtr::new(ptr::null_mut());
static BOARD_STATE: AtomicU8 = AtomicU8::new(BOARD_UNINITIALIZED);
static FORK_GATE: AtomicBool = AtomicBool::new(false);

#[link(name = "System")]
unsafe extern "C" {
    fn __ulock_wait(operation: u32, address: *mut c_void, value: u64, timeout_us: u32) -> c_int;
    fn __ulock_wake(operation: u32, address: *mut c_void, wake_value: u64) -> c_int;
    fn getuid() -> u32;
    fn lseek(fd: c_int, offset: i64, whence: c_int) -> i64;
    fn ftruncate(fd: c_int, length: i64) -> c_int;
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn lxrt_host_tmpdir() -> *const c_char;
    fn lxrt_open_private(path: *const c_char, flags: c_int, mode: c_int) -> c_int;
    fn lxrt_close_private(fd: c_int);
    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_gbase() -> u64;
    fn lxrt_guest_clock_ns(clock_id: c_long) -> u64;
    fn lxrt_interrupted_internally_ffi() -> bool;
}

fn linux_error(error: c_int) -> c_long {
    -(unsafe { lxrt_errno_to_linux(error) } as c_long)
}

fn map_board() -> *mut Board {
    let temporary_directory = unsafe { lxrt_host_tmpdir() };
    if temporary_directory.is_null() {
        return ptr::null_mut();
    }
    let mut path = unsafe { CStr::from_ptr(temporary_directory) }
        .to_bytes()
        .to_vec();
    path.extend_from_slice(format!("/lxrt-futexwaitv-{}", unsafe { getuid() }).as_bytes());
    if path.len() >= 600 {
        path.truncate(599);
    }
    let Ok(path) = CString::new(path) else {
        return ptr::null_mut();
    };
    let fd = unsafe { lxrt_open_private(path.as_ptr(), D_O_RDWR | D_O_CREAT | D_O_CLOEXEC, 0o600) };
    if fd < 0 {
        return ptr::null_mut();
    }

    let size = unsafe { lseek(fd, 0, 2) };
    if size >= 0 && size < BOARD_BYTES as i64 {
        unsafe { ftruncate(fd, BOARD_BYTES as i64) };
    }
    let mapping = unsafe {
        mmap(
            ptr::null_mut(),
            BOARD_BYTES,
            D_PROT_READ | D_PROT_WRITE,
            D_MAP_SHARED,
            fd,
            0,
        )
    };
    unsafe { lxrt_close_private(fd) };
    if mapping as isize == -1 {
        ptr::null_mut()
    } else {
        mapping.cast()
    }
}

fn board() -> *mut Board {
    loop {
        if FORK_GATE.load(Ordering::Acquire) {
            spin_loop();
            continue;
        }
        match BOARD_STATE.load(Ordering::Acquire) {
            BOARD_READY => return BOARD.load(Ordering::Acquire),
            BOARD_FAILED => return ptr::null_mut(),
            BOARD_UNINITIALIZED => {
                if BOARD_STATE
                    .compare_exchange(
                        BOARD_UNINITIALIZED,
                        BOARD_INITIALIZING,
                        Ordering::AcqRel,
                        Ordering::Acquire,
                    )
                    .is_ok()
                {
                    let mapping = map_board();
                    BOARD.store(mapping, Ordering::Release);
                    BOARD_STATE.store(
                        if mapping.is_null() {
                            BOARD_FAILED
                        } else {
                            BOARD_READY
                        },
                        Ordering::Release,
                    );
                    return mapping;
                }
            }
            BOARD_INITIALIZING => spin_loop(),
            _ => return ptr::null_mut(),
        }
    }
}

extern "C" fn fork_prepare() {
    while FORK_GATE
        .compare_exchange(false, true, Ordering::AcqRel, Ordering::Acquire)
        .is_err()
    {
        spin_loop();
    }
    while BOARD_STATE.load(Ordering::Acquire) == BOARD_INITIALIZING {
        spin_loop();
    }
}

extern "C" fn fork_release() {
    FORK_GATE.store(false, Ordering::Release);
}

extern "C" fn register_fork_hooks() {
    unsafe {
        pthread_atfork(Some(fork_prepare), Some(fork_release), Some(fork_release));
    }
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FORK_HOOKS_INIT: extern "C" fn() = register_fork_hooks;

fn bucket(address: *const c_void) -> usize {
    ((address as usize >> 2) & (BUCKETS - 1)) as usize
}

#[no_mangle]
pub extern "C" fn lxrt_futex_waitv_notify(address: *const c_void) {
    let shared = board();
    if shared.is_null() {
        return;
    }
    atomic::fence(Ordering::SeqCst);
    let index = bucket(address);
    unsafe {
        let shared = &*shared;
        if shared.single_waiters[index].load(Ordering::Relaxed) != 0 {
            shared.generation[index].fetch_add(1, Ordering::SeqCst);
            __ulock_wake(
                UL_COMPARE_AND_WAIT_SHARED | ULF_WAKE_ALL | ULF_NO_ERRNO,
                (&shared.generation[index] as *const AtomicU32)
                    .cast_mut()
                    .cast(),
                0,
            );
        }
        if shared.multi_waiters[index].load(Ordering::Relaxed) != 0 {
            shared.multi_generation.fetch_add(1, Ordering::SeqCst);
            __ulock_wake(
                UL_COMPARE_AND_WAIT_SHARED | ULF_WAKE_ALL | ULF_NO_ERRNO,
                (&shared.multi_generation as *const AtomicU32)
                    .cast_mut()
                    .cast(),
                0,
            );
        }
    }
}

fn clock_ns(clock_id: c_int) -> i64 {
    unsafe { lxrt_guest_clock_ns(if clock_id == 0 { 0 } else { 1 }) as i64 }
}

#[no_mangle]
pub extern "C" fn lxrt_futex_waitv(
    waiters: u64,
    count: u32,
    flags: u32,
    timeout: u64,
    clock_id: i32,
) -> c_long {
    if flags != 0 {
        return linux_error(D_EINVAL);
    }
    if count == 0 || count as usize > WAITV_MAX || waiters == 0 {
        return linux_error(D_EINVAL);
    }

    let timeout_ptr = timeout as usize as *const WaitvTimespec;
    let first_timeout = if timeout_ptr.is_null() {
        None
    } else {
        Some(unsafe { ptr::read_unaligned(timeout_ptr) })
    };
    if first_timeout.is_some() && clock_id != 0 && clock_id != 1 {
        return linux_error(D_EINVAL);
    }
    if first_timeout
        .is_some_and(|ts| ts.seconds < 0 || ts.nanoseconds < 0 || ts.nanoseconds >= 1_000_000_000)
    {
        return linux_error(D_EINVAL);
    }

    let input = waiters as usize as *const LinuxFutexWaitv;
    let mut addresses = [ptr::null_mut::<AtomicU32>(); WAITV_MAX];
    let mut values = [0u32; WAITV_MAX];
    let base = unsafe { lxrt_gbase() };
    for index in 0..count as usize {
        let waiter = unsafe { ptr::read_unaligned(input.add(index)) };
        if waiter.flags & !(FUTEX2_SIZE_MASK | FUTEX2_PRIVATE) != 0
            || waiter.flags & FUTEX2_SIZE_MASK != FUTEX2_SIZE_U32
            || waiter.reserved != 0
        {
            return linux_error(D_EINVAL);
        }
        if waiter.value >> 32 != 0 {
            return linux_error(D_EINVAL);
        }
        let mut address = waiter.address;
        if base != 0 && address != 0 && address < (1u64 << 32) {
            address = address.wrapping_add(base);
        }
        if address == 0 || address & 3 != 0 {
            return linux_error(D_EINVAL);
        }
        addresses[index] = address as usize as *mut AtomicU32;
        values[index] = waiter.value as u32;
    }

    let shared = board();
    if shared.is_null() {
        return linux_error(D_ENOMEM);
    }
    let shared = unsafe { &*shared };
    let first_bucket = bucket(addresses[0].cast());
    let one_bucket = addresses[..count as usize]
        .iter()
        .all(|address| bucket(address.cast()) == first_bucket);
    if one_bucket {
        shared.single_waiters[first_bucket].fetch_add(1, Ordering::SeqCst);
    } else {
        for address in addresses.iter().take(count as usize) {
            shared.multi_waiters[bucket(address.cast())].fetch_add(1, Ordering::SeqCst);
        }
    }
    let generation = if one_bucket {
        &shared.generation[first_bucket] as *const AtomicU32
    } else {
        &shared.multi_generation as *const AtomicU32
    };

    let deadline = if timeout_ptr.is_null() {
        0
    } else {
        let ts = unsafe { ptr::read_unaligned(timeout_ptr) };
        ts.seconds
            .wrapping_mul(1_000_000_000)
            .wrapping_add(ts.nanoseconds)
    };
    let mut result = -1;
    let mut first = true;
    loop {
        let current_generation = unsafe { &*generation }.load(Ordering::SeqCst);
        atomic::fence(Ordering::SeqCst);
        for index in 0..count as usize {
            if unsafe { &*addresses[index] }.load(Ordering::SeqCst) != values[index] {
                result = if first {
                    linux_error(D_EAGAIN)
                } else {
                    index as c_long
                };
                break;
            }
        }
        if result != -1 {
            break;
        }
        first = false;

        let mut timeout_us = 0u32;
        if !timeout_ptr.is_null() {
            let remaining = deadline.wrapping_sub(clock_ns(clock_id));
            if remaining <= 0 {
                result = linux_error(D_ETIMEDOUT);
                break;
            }
            let micros = remaining / 1_000;
            timeout_us = if micros <= 0 {
                1
            } else if micros >= u32::MAX as i64 {
                u32::MAX - 1
            } else {
                micros as u32
            };
        }
        let wait_result = unsafe {
            __ulock_wait(
                UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO,
                generation.cast_mut().cast(),
                current_generation as u64,
                timeout_us,
            )
        };
        if wait_result == -D_EINTR && !unsafe { lxrt_interrupted_internally_ffi() } {
            result = linux_error(D_EINTR);
            break;
        }
    }

    if one_bucket {
        shared.single_waiters[first_bucket].fetch_sub(1, Ordering::SeqCst);
    } else {
        for address in addresses.iter().take(count as usize) {
            shared.multi_waiters[bucket(address.cast())].fetch_sub(1, Ordering::SeqCst);
        }
    }
    result
}
