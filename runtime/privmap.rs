// Linux MAP_PRIVATE copy-on-write for memfd-backed shared-memory files.

use std::ffi::{c_int, c_long, c_void};
use std::mem::MaybeUninit;
use std::ptr;
use std::sync::{Mutex, MutexGuard, Once};

include!(concat!(env!("OUT_DIR"), "/privmap_constants.rs"));

const HOST_PAGE: u64 = 16_384;
const PM_MAX: usize = 1_024;

#[derive(Clone, Copy)]
struct Mapping {
    start: u64,
    end: u64,
    want: c_int,
}

impl Mapping {
    const EMPTY: Self = Self {
        start: 0,
        end: 0,
        want: 0,
    };
}

struct State {
    entries: [Mapping; PM_MAX],
    len: usize,
}

impl State {
    const fn new() -> Self {
        Self {
            entries: [Mapping::EMPTY; PM_MAX],
            len: 0,
        }
    }
}

static STATE: Mutex<State> = Mutex::new(State::new());
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
    fn __error() -> *mut c_int;
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(address: *mut c_void, length: usize) -> c_int;
    fn mprotect(address: *mut c_void, length: usize, protection: c_int) -> c_int;
    fn lxrt_memfd_path_is(fd: c_int) -> bool;
    fn lxrt_errno_to_linux(error: c_int) -> c_int;
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

extern "C" fn fork_release() {
    unsafe {
        ptr::drop_in_place(ptr::addr_of_mut!(FORK_GUARD).cast::<StateGuard>());
    }
}

extern "C" fn register_fork_hooks() {
    FORK_HOOKS.call_once(|| unsafe {
        let _ = pthread_atfork(Some(fork_prepare), Some(fork_release), Some(fork_release));
    });
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FORK_HOOKS_INIT: extern "C" fn() = register_fork_hooks;

fn ensure_fork_hooks() {
    register_fork_hooks();
}

fn linux_error() -> c_long {
    unsafe { -(lxrt_errno_to_linux(*__error()) as c_long) }
}

fn cut(state: &mut State, start: u64, end: u64) -> bool {
    let mut ok = true;
    let mut index = 0;
    while index < state.len {
        let entry = state.entries[index];
        if entry.end <= start || entry.start >= end {
            index += 1;
            continue;
        }
        if entry.start < start && entry.end > end {
            if state.len < PM_MAX {
                state.entries[state.len] = Mapping {
                    start: end,
                    end: entry.end,
                    want: entry.want,
                };
                state.len += 1;
            } else {
                ok = false;
            }
            state.entries[index].end = start;
            index += 1;
        } else if entry.start < start {
            state.entries[index].end = start;
            index += 1;
        } else if entry.end > end {
            state.entries[index].start = end;
            index += 1;
        } else {
            state.len -= 1;
            state.entries[index] = state.entries[state.len];
        }
    }
    ok
}

#[no_mangle]
pub extern "C" fn lxrt_privmap_eligible(
    fd: c_int,
    address: u64,
    length: u64,
    offset: u64,
    protection: c_int,
) -> bool {
    ensure_fork_hooks();
    if fd < 0 || protection & D_PROT_EXEC != 0 || length == 0 {
        return false;
    }
    if address % HOST_PAGE != 0 || length % HOST_PAGE != 0 || offset % HOST_PAGE != 0 {
        return false;
    }
    unsafe { lxrt_memfd_path_is(fd) }
}

#[no_mangle]
pub extern "C" fn lxrt_privmap_forget(address: u64, length: u64) {
    ensure_fork_hooks();
    if length == 0 {
        return;
    }
    let mut state = lock_state();
    if state.len != 0 {
        cut(&mut state, address, address.wrapping_add(length));
    }
}

#[no_mangle]
pub extern "C" fn lxrt_privmap_mmap(
    address: u64,
    length: u64,
    protection: c_int,
    flags: c_int,
    fd: c_int,
    offset: u64,
) -> c_long {
    ensure_fork_hooks();
    let flags = (flags & !D_MAP_PRIVATE) | D_MAP_SHARED;
    let mapped = unsafe {
        mmap(
            address as usize as *mut c_void,
            length as usize,
            protection & !D_PROT_WRITE,
            flags,
            fd,
            offset as i64,
        )
    };
    if mapped as isize == -1 {
        return linux_error();
    }
    let start = mapped as usize as u64;
    let end = start.wrapping_add(length);
    let mut state = lock_state();
    cut(&mut state, start, end);
    let room = state.len < PM_MAX;
    if room {
        let index = state.len;
        state.entries[index] = Mapping {
            start,
            end,
            want: protection,
        };
        state.len += 1;
    }
    drop(state);

    if !room {
        // Tracking exhaustion falls back to Darwin's private copy semantics.
        let copied = unsafe {
            mmap(
                mapped,
                length as usize,
                protection,
                (flags & !D_MAP_SHARED) | D_MAP_PRIVATE | D_MAP_FIXED,
                fd,
                offset as i64,
            )
        };
        if copied as isize == -1 {
            let error = linux_error();
            unsafe { munmap(mapped, length as usize) };
            return error;
        }
    }
    start as c_long
}

#[no_mangle]
pub extern "C" fn lxrt_privmap_after_mprotect(address: u64, length: u64, protection: c_int) {
    ensure_fork_hooks();
    if length == 0 {
        return;
    }
    let end = address.wrapping_add(length);
    let mut state = lock_state();
    if state.len == 0 {
        return;
    }
    let mut overlapped = [Mapping::EMPTY; 16];
    let mut overlap_count = 0;
    for entry in &state.entries[..state.len] {
        if entry.end <= address || entry.start >= end || overlap_count == overlapped.len() {
            continue;
        }
        overlapped[overlap_count] = Mapping {
            start: entry.start.max(address),
            end: entry.end.min(end),
            want: protection,
        };
        overlap_count += 1;
    }
    cut(&mut state, address, end);
    for entry in overlapped.iter().take(overlap_count) {
        if state.len == PM_MAX {
            break;
        }
        let index = state.len;
        state.entries[index] = *entry;
        state.len += 1;
        if protection & D_PROT_WRITE != 0 {
            unsafe {
                let _ = mprotect(
                    entry.start as usize as *mut c_void,
                    entry.end.wrapping_sub(entry.start) as usize,
                    protection & !D_PROT_WRITE,
                );
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn lxrt_privmap_handle_fault(fault_address: u64, is_write: bool) -> bool {
    ensure_fork_hooks();
    if !is_write {
        return false;
    }
    let page = fault_address & !(HOST_PAGE - 1);
    let mut state = lock_state();
    if state.len == 0 {
        return false;
    }
    let Some(entry) = state.entries[..state.len]
        .iter()
        .find(|entry| fault_address >= entry.start && fault_address < entry.end)
        .copied()
    else {
        return false;
    };
    if entry.want & D_PROT_WRITE == 0 {
        return false;
    }

    let temporary = unsafe {
        mmap(
            ptr::null_mut(),
            HOST_PAGE as usize,
            D_PROT_READ | D_PROT_WRITE,
            D_MAP_PRIVATE | D_MAP_ANON,
            -1,
            0,
        )
    };
    if temporary as isize == -1 {
        return false;
    }
    unsafe {
        ptr::copy_nonoverlapping(
            page as usize as *const u8,
            temporary.cast::<u8>(),
            HOST_PAGE as usize,
        );
    }
    let private_page = unsafe {
        mmap(
            page as usize as *mut c_void,
            HOST_PAGE as usize,
            D_PROT_READ | D_PROT_WRITE,
            D_MAP_PRIVATE | D_MAP_ANON | D_MAP_FIXED,
            -1,
            0,
        )
    };
    let mut copied = false;
    if private_page as isize != -1 {
        unsafe {
            ptr::copy_nonoverlapping(
                temporary.cast::<u8>(),
                page as usize as *mut u8,
                HOST_PAGE as usize,
            );
            let _ = mprotect(page as usize as *mut c_void, HOST_PAGE as usize, entry.want);
        }
        cut(&mut state, page, page.wrapping_add(HOST_PAGE));
        copied = true;
    }
    unsafe { munmap(temporary, HOST_PAGE as usize) };
    copied
}
