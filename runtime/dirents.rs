// Linux getdents64 compatibility over Darwin's fdopendir/readdir API.

use std::ffi::{c_char, c_int, c_long, c_uint, c_void, CStr};
use std::mem::{offset_of, size_of, MaybeUninit};
use std::ptr;
use std::sync::{Mutex, MutexGuard, Once};

include!(concat!(env!("OUT_DIR"), "/dirent_constants.rs"));

const MAX_DIRS: usize = 128;
const MAX_PATH_BYTES: usize = 1024;
const LINUX_D_NAME_OFFSET: usize = 19;

#[repr(C)]
struct DarwinDirent {
    d_ino: u64,
    d_seekoff: u64,
    d_reclen: u16,
    d_namlen: u16,
    d_type: u8,
    d_name: [c_char; MAX_PATH_BYTES],
}

#[repr(C)]
struct LinuxDirent64 {
    d_ino: u64,
    d_off: i64,
    d_reclen: u16,
    d_type: u8,
    d_name: [c_char; 1],
}

const _: [(); 24] = [(); size_of::<LinuxDirent64>()];
const _: [(); LINUX_D_NAME_OFFSET] = [(); offset_of!(LinuxDirent64, d_name)];

#[derive(Clone, Copy)]
struct DirSlot {
    fd: c_int,
    dir: usize,
}

impl DirSlot {
    const EMPTY: Self = Self { fd: -1, dir: 0 };
}

static DIRS: Mutex<[DirSlot; MAX_DIRS]> = Mutex::new([DirSlot::EMPTY; MAX_DIRS]);
static FORK_HOOKS: Once = Once::new();
static mut FORK_GUARD: MaybeUninit<MutexGuard<'static, [DirSlot; MAX_DIRS]>> =
    MaybeUninit::uninit();

#[link(name = "System")]
unsafe extern "C" {
    fn fdopendir(fd: c_int) -> *mut c_void;
    fn readdir(dir: *mut c_void) -> *mut DarwinDirent;
    fn telldir(dir: *mut c_void) -> c_long;
    fn seekdir(dir: *mut c_void, location: c_long);
    fn close(fd: c_int) -> c_int;
    fn __error() -> *mut c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn lxrt_fd_dup_private(fd: c_int) -> c_int;
    fn lxrt_fd_hide(fd: c_int, hide: bool);
    fn lxrt_closedir_private(dir: *mut c_void);
}

fn lock_dirs() -> MutexGuard<'static, [DirSlot; MAX_DIRS]> {
    DIRS.lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

extern "C" fn fork_prepare() {
    let guard = lock_dirs();
    unsafe {
        ptr::addr_of_mut!(FORK_GUARD)
            .cast::<MutexGuard<'static, [DirSlot; MAX_DIRS]>>()
            .write(guard);
    }
}

extern "C" fn fork_release() {
    unsafe {
        ptr::drop_in_place(
            ptr::addr_of_mut!(FORK_GUARD).cast::<MutexGuard<'static, [DirSlot; MAX_DIRS]>>(),
        );
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

fn ensure_fork_safe() {
    register_fork_hooks();
}

fn dir_for(fd: c_int) -> *mut c_void {
    ensure_fork_safe();
    let mut slots = lock_dirs();
    for slot in slots.iter() {
        if slot.dir != 0 && slot.fd == fd {
            return slot.dir as *mut c_void;
        }
    }

    // fdopendir takes ownership; duplicate and hide the runtime descriptor.
    let duplicate = unsafe { lxrt_fd_dup_private(fd) };
    let directory = if duplicate < 0 {
        ptr::null_mut()
    } else {
        unsafe { fdopendir(duplicate) }
    };
    if directory.is_null() {
        if duplicate >= 0 {
            unsafe {
                lxrt_fd_hide(duplicate, false);
                close(duplicate);
            }
        }
        return ptr::null_mut();
    }

    if let Some(slot) = slots.iter_mut().find(|slot| slot.dir == 0) {
        slot.fd = fd;
        slot.dir = directory as usize;
        directory
    } else {
        unsafe { lxrt_closedir_private(directory) };
        ptr::null_mut()
    }
}

fn dtype_to_linux(dtype: u8) -> u8 {
    match dtype as c_int {
        D_DT_FIFO => 1,
        D_DT_CHR => 2,
        D_DT_DIR => 4,
        D_DT_BLK => 6,
        D_DT_REG => 8,
        D_DT_LNK => 10,
        D_DT_SOCK => 12,
        _ => 0,
    }
}

fn linux_error(host_errno: c_int) -> c_long {
    -(crate::errno_map::map_errno(host_errno) as c_long)
}

#[no_mangle]
pub extern "C" fn lxrt_dirents_close(fd: c_int) {
    ensure_fork_safe();
    let mut slots = lock_dirs();
    if let Some(slot) = slots.iter_mut().find(|slot| slot.dir != 0 && slot.fd == fd) {
        unsafe { lxrt_closedir_private(slot.dir as *mut c_void) };
        slot.dir = 0;
    }
}

#[no_mangle]
pub extern "C" fn lxrt_getdents64(fd: c_int, buf: *mut c_void, count: c_uint) -> c_long {
    if buf.is_null() {
        return linux_error(D_EFAULT);
    }
    let directory = dir_for(fd);
    if directory.is_null() {
        return linux_error(D_ENOTDIR);
    }

    let mut used = 0usize;
    loop {
        let position = unsafe { telldir(directory) };
        unsafe { *__error() = 0 };
        let entry = unsafe { readdir(directory) };
        if entry.is_null() {
            let error = unsafe { *__error() };
            return if error != 0 {
                linux_error(error)
            } else {
                used as c_long
            };
        }

        let name_ptr = unsafe { ptr::addr_of!((*entry).d_name).cast::<c_char>() };
        let name = unsafe { CStr::from_ptr(name_ptr) }.to_bytes();
        let record_len = (size_of::<LinuxDirent64>() + name.len() + 1 + 7) & !7;
        let next_used = used.saturating_add(record_len);
        if next_used > count as usize {
            unsafe { seekdir(directory, position) };
            return if used != 0 {
                used as c_long
            } else {
                linux_error(D_EINVAL)
            };
        }

        let record = unsafe { buf.cast::<u8>().add(used) };
        unsafe {
            ptr::write_unaligned(record.cast::<u64>(), (*entry).d_ino);
            ptr::write_unaligned(record.add(8).cast::<i64>(), telldir(directory) as i64);
            ptr::write_unaligned(record.add(16).cast::<u16>(), record_len as u16);
            record.add(18).write(dtype_to_linux((*entry).d_type));
            ptr::copy_nonoverlapping(name.as_ptr(), record.add(LINUX_D_NAME_OFFSET), name.len());
            record.add(LINUX_D_NAME_OFFSET + name.len()).write(0);
        }
        used = next_used;
    }
}
