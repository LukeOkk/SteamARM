//! Keep read-only `MAP_SHARED` file ranges live when Darwin's larger host
//! page becomes a sub-page composite. A worker copies changed file bytes into
//! the composite so clients observe updates such as Wine's shared clock.

use std::ffi::{c_char, c_int, c_ulonglong, c_void};
use std::mem::MaybeUninit;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard, Once};
use std::thread;
use std::time::Duration;

include!(concat!(env!("OUT_DIR"), "/timer_constants.rs"));

const HOST_PAGE: u64 = 16_384;
const MIRROR_PERIOD_US: u64 = 2_000;
const NOTE_MAX: usize = 256;
const MIRROR_MAX: usize = 64;
const KERN_SUCCESS: c_int = 0;
const VM_FLAGS_ANYWHERE: c_int = 1;
const VM_INHERIT_NONE: c_int = 2;
const VM_PROT_READ: c_int = 1;
const VM_PROT_WRITE: c_int = 2;

type MachPort = u32;
type MachAddress = u64;
type MachSize = u64;

#[derive(Clone, Copy, Default)]
struct Note {
    start: u64,
    end: u64,
}

#[derive(Clone, Copy, Default)]
struct Mirror {
    hpage: u64,
    view: MachAddress,
    alias: MachAddress,
    start: u64,
    end: u64,
}

struct State {
    notes: [Note; NOTE_MAX],
    nnotes: usize,
    mirrors: [Mirror; MIRROR_MAX],
}

impl State {
    const fn new() -> Self {
        Self {
            notes: [Note { start: 0, end: 0 }; NOTE_MAX],
            nnotes: 0,
            mirrors: [Mirror {
                hpage: 0,
                view: 0,
                alias: 0,
                start: 0,
                end: 0,
            }; MIRROR_MAX],
        }
    }
}

static STATE: Mutex<State> = Mutex::new(State::new());
static MIRROR_COUNT: AtomicUsize = AtomicUsize::new(0);
static THREAD_STARTED: AtomicBool = AtomicBool::new(false);
static FORK_HOOKS: Once = Once::new();
type StateGuard = MutexGuard<'static, State>;
static mut EXTERNAL_FORK_GUARD: MaybeUninit<StateGuard> = MaybeUninit::uninit();

#[repr(transparent)]
struct DarwinSigSet(u32);

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: MachPort;
    fn mach_vm_deallocate(task: MachPort, address: MachAddress, size: MachSize) -> c_int;
    fn mach_vm_protect(
        task: MachPort,
        address: MachAddress,
        size: MachSize,
        set_maximum: c_int,
        new_protection: c_int,
    ) -> c_int;
    fn mach_vm_remap(
        target_task: MachPort,
        target_address: *mut MachAddress,
        size: MachSize,
        mask: MachAddress,
        flags: c_int,
        source_task: MachPort,
        source_address: MachAddress,
        copy: c_int,
        current_protection: *mut c_int,
        maximum_protection: *mut c_int,
        inheritance: c_int,
    ) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn sigfillset(set: *mut c_void) -> c_int;
    fn pthread_sigmask(how: c_int, set: *const c_void, old: *mut c_void) -> c_int;
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
}

fn lock_state() -> StateGuard {
    unsafe { &*ptr::addr_of!(STATE) }
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

extern "C" fn external_fork_lock() {
    let guard = lock_state();
    unsafe {
        ptr::addr_of_mut!(EXTERNAL_FORK_GUARD)
            .cast::<StateGuard>()
            .write(guard);
    }
}

extern "C" fn external_fork_unlock() {
    unsafe {
        ptr::drop_in_place(ptr::addr_of_mut!(EXTERNAL_FORK_GUARD).cast::<StateGuard>());
    }
}

extern "C" fn child_reset() {
    // VM_INHERIT_NONE leaves no remaps in the child. The wxsplit atfork hook
    // holds the state lock while the fork occurs. Atomics also make this
    // safe regardless of atfork child-handler ordering.
    MIRROR_COUNT.store(0, Ordering::SeqCst);
    THREAD_STARTED.store(false, Ordering::SeqCst);
}

extern "C" fn register_fork_hook() {
    FORK_HOOKS.call_once(|| unsafe {
        let _ = pthread_atfork(None, None, Some(child_reset));
    });
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FORK_HOOK_INIT: extern "C" fn() = register_fork_hook;

unsafe fn deallocate(address: MachAddress) {
    let _ = mach_vm_deallocate(mach_task_self_, address, HOST_PAGE);
}

fn drop_mirror(state: &mut State, index: usize) {
    unsafe {
        deallocate(state.mirrors[index].view);
        deallocate(state.mirrors[index].alias);
    }
    let count = MIRROR_COUNT.load(Ordering::Relaxed) - 1;
    MIRROR_COUNT.store(count, Ordering::Relaxed);
    state.mirrors[index] = state.mirrors[count];
}

fn mirror_thread() {
    loop {
        thread::sleep(Duration::from_micros(MIRROR_PERIOD_US));
        let state = lock_state();
        let count = MIRROR_COUNT.load(Ordering::Relaxed);
        for mirror in &state.mirrors[..count] {
            let offset = mirror.start - mirror.hpage;
            let len = (mirror.end - mirror.start) as usize;
            unsafe {
                let source = (mirror.view + offset) as *const u8;
                let target = (mirror.alias + offset) as *mut u8;
                if std::slice::from_raw_parts(target.cast_const(), len)
                    != std::slice::from_raw_parts(source, len)
                {
                    ptr::copy_nonoverlapping(source, target, len);
                }
            }
        }
    }
}

fn start_thread_locked() {
    if THREAD_STARTED.load(Ordering::Relaxed) {
        return;
    }

    // The worker inherits this blocked signal mask, so no guest signal is
    // ever delivered on it. Restore the caller's mask after creating it.
    let mut all = DarwinSigSet(0);
    let mut old = DarwinSigSet(0);
    unsafe {
        let _ = sigfillset((&mut all as *mut DarwinSigSet).cast());
        let _ = pthread_sigmask(
            D_SIG_BLOCK,
            (&all as *const DarwinSigSet).cast(),
            (&mut old as *mut DarwinSigSet).cast(),
        );
    }
    let spawned = thread::Builder::new()
        .stack_size(64 * 1024)
        .spawn(mirror_thread);
    unsafe {
        let _ = pthread_sigmask(
            D_SIG_SETMASK,
            (&old as *const DarwinSigSet).cast(),
            ptr::null_mut(),
        );
    }
    if let Ok(worker) = spawned {
        drop(worker); // detached, like PTHREAD_CREATE_DETACHED
        THREAD_STARTED.store(true, Ordering::Relaxed);
    }
}

/// Remember a read-only shared file range before a sub-page composite is
/// created for its host page.
#[no_mangle]
pub extern "C" fn lxrt_shmirror_note(addr: u64, len: u64) {
    register_fork_hook();
    let mut state = lock_state();
    if state.nnotes < NOTE_MAX {
        let index = state.nnotes;
        state.notes[index] = Note {
            start: addr,
            end: addr.wrapping_add(len),
        };
        state.nnotes += 1;
    }
}

/// Forget a guest range and release mirror views no longer mapped by it.
#[no_mangle]
pub extern "C" fn lxrt_shmirror_forget(addr: u64, len: u64) {
    register_fork_hook();
    let end = addr.wrapping_add(len);
    let mut state = lock_state();
    let mut index = 0;
    while index < state.nnotes {
        let note = state.notes[index];
        if note.start < end && note.end > addr {
            state.nnotes -= 1;
            state.notes[index] = state.notes[state.nnotes];
        } else {
            index += 1;
        }
    }
    index = 0;
    while index < MIRROR_COUNT.load(Ordering::Relaxed) {
        let mirror = state.mirrors[index];
        if mirror.start < end && mirror.end > addr {
            drop_mirror(&mut state, index);
        } else {
            index += 1;
        }
    }
}

/// Lock ordering companion for wxsplit's atfork prepare/release hooks.
#[no_mangle]
pub extern "C" fn lxrt_shmirror_fork_lock() {
    external_fork_lock();
}

#[no_mangle]
pub extern "C" fn lxrt_shmirror_fork_unlock() {
    external_fork_unlock();
}

/// Preserve a live mapping of the old file page while subpage replaces it.
#[no_mangle]
pub extern "C" fn lxrt_shmirror_take_view(hpage: u64) -> u64 {
    register_fork_hook();
    let state = lock_state();
    for note in &state.notes[..state.nnotes] {
        if note.end <= hpage || note.start >= hpage + HOST_PAGE {
            continue;
        }
        let mut view = 0;
        let mut current = 0;
        let mut maximum = 0;
        let result = unsafe {
            mach_vm_remap(
                mach_task_self_,
                &mut view,
                HOST_PAGE,
                0,
                VM_FLAGS_ANYWHERE,
                mach_task_self_,
                hpage,
                0,
                &mut current,
                &mut maximum,
                VM_INHERIT_NONE,
            )
        };
        if result == KERN_SUCCESS {
            return view;
        }
    }
    0
}

/// Start mirroring a file range into the sub-page composite.
#[no_mangle]
pub extern "C" fn lxrt_shmirror_adopt(hpage: u64, view: u64) {
    register_fork_hook();
    if view == 0 {
        return;
    }

    let mut alias = 0;
    let mut current = 0;
    let mut maximum = 0;
    let mut result = unsafe {
        mach_vm_remap(
            mach_task_self_,
            &mut alias,
            HOST_PAGE,
            0,
            VM_FLAGS_ANYWHERE,
            mach_task_self_,
            hpage,
            0,
            &mut current,
            &mut maximum,
            VM_INHERIT_NONE,
        )
    };
    if result == KERN_SUCCESS {
        result = unsafe {
            mach_vm_protect(
                mach_task_self_,
                alias,
                HOST_PAGE,
                0,
                VM_PROT_READ | VM_PROT_WRITE,
            )
        };
    }

    let mut state = lock_state();
    let mut added = 0;
    let page_end = hpage.wrapping_add(HOST_PAGE);
    for index in 0..state.nnotes {
        if result != KERN_SUCCESS {
            break;
        }
        let start = state.notes[index].start.max(hpage);
        let end = state.notes[index].end.min(page_end);
        let mirror_count = MIRROR_COUNT.load(Ordering::Relaxed);
        if start >= end || mirror_count >= MIRROR_MAX {
            continue;
        }

        let (view_copy, alias_copy) = if added == 0 {
            (view, alias)
        } else {
            let mut view_copy = 0;
            let mut alias_copy = 0;
            if unsafe {
                mach_vm_remap(
                    mach_task_self_,
                    &mut view_copy,
                    HOST_PAGE,
                    0,
                    VM_FLAGS_ANYWHERE,
                    mach_task_self_,
                    view,
                    0,
                    &mut current,
                    &mut maximum,
                    VM_INHERIT_NONE,
                )
            } != KERN_SUCCESS
                || unsafe {
                    mach_vm_remap(
                        mach_task_self_,
                        &mut alias_copy,
                        HOST_PAGE,
                        0,
                        VM_FLAGS_ANYWHERE,
                        mach_task_self_,
                        alias,
                        0,
                        &mut current,
                        &mut maximum,
                        VM_INHERIT_NONE,
                    )
                } != KERN_SUCCESS
            {
                continue;
            }
            (view_copy, alias_copy)
        };

        let mirror_index = mirror_count;
        state.mirrors[mirror_index] = Mirror {
            hpage,
            view: view_copy,
            alias: alias_copy,
            start,
            end,
        };
        MIRROR_COUNT.store(mirror_count + 1, Ordering::Relaxed);
        added += 1;
        if unsafe { lxrt_trace_on() } {
            unsafe {
                let _ = fprintf(
                    lxrt_trace_stream(),
                    b"[lxrt]    shmirror 0x%llx..0x%llx mirrored from a live file view\n\0"
                        .as_ptr()
                        .cast(),
                    start as c_ulonglong,
                    end as c_ulonglong,
                );
            }
        }
    }
    if added > 0 {
        start_thread_locked();
    }
    drop(state);

    if added == 0 {
        unsafe {
            deallocate(view);
            if alias != 0 {
                deallocate(alias);
            }
        }
    }
}
