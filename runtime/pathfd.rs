// Linux O_PATH descriptors for sockets Darwin refuses to open.

use std::cell::RefCell;
use std::ffi::{c_char, c_int, c_void, CStr, CString, OsStr};
use std::fs;
use std::io;
use std::mem::size_of;
use std::mem::MaybeUninit;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{FileTypeExt, MetadataExt};
use std::ptr;
use std::sync::atomic::{AtomicPtr, Ordering};
use std::sync::{Once, RwLock, RwLockWriteGuard};

include!(concat!(env!("OUT_DIR"), "/pathfd_constants.rs"));

const PF_MAX: usize = 65_536;

struct PathFd {
    path: CString,
    dev: u64,
    ino: u64,
}

static PATH_FDS: [AtomicPtr<PathFd>; PF_MAX] = [const { AtomicPtr::new(ptr::null_mut()) }; PF_MAX];
static PATH_FDS_LOCK: RwLock<()> = RwLock::new(());
static FORK_HOOKS: Once = Once::new();
static mut FORK_GUARD: MaybeUninit<RwLockWriteGuard<'static, ()>> = MaybeUninit::uninit();

thread_local! {
    static PATH_SNAPSHOT: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
}

#[link(name = "System")]
unsafe extern "C" {
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn malloc(size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn __error() -> *mut c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
}

extern "C" fn fork_prepare() {
    let guard = PATH_FDS_LOCK
        .write()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    unsafe {
        ptr::addr_of_mut!(FORK_GUARD)
            .cast::<RwLockWriteGuard<'static, ()>>()
            .write(guard);
    }
}

extern "C" fn fork_release() {
    unsafe {
        ptr::drop_in_place(ptr::addr_of_mut!(FORK_GUARD).cast::<RwLockWriteGuard<'static, ()>>());
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

fn set_errno(error: c_int) {
    unsafe { *__error() = error };
}

fn io_error(error: io::Error) -> c_int {
    error.raw_os_error().unwrap_or(5)
}

fn directory_path(dirfd: c_int) -> io::Result<Vec<u8>> {
    if dirfd == D_AT_FDCWD {
        return Ok(std::env::current_dir()?.as_os_str().as_bytes().to_vec());
    }

    let mut path = vec![0u8; D_MAXPATHLEN as usize];
    let result = unsafe { fcntl(dirfd, D_F_GETPATH, path.as_mut_ptr().cast::<c_void>()) };
    if result != 0 {
        return Err(io::Error::last_os_error());
    }
    let path = unsafe { CStr::from_ptr(path.as_ptr().cast::<c_char>()) };
    Ok(path.to_bytes().to_vec())
}

fn full_path(guest_path: &CStr, base: Option<&[u8]>) -> io::Result<CString> {
    let bytes = guest_path.to_bytes();
    let mut full = Vec::new();
    if bytes.first() == Some(&b'/') {
        full.try_reserve(bytes.len().saturating_add(1))
            .map_err(|_| io::Error::from_raw_os_error(D_ENOMEM))?;
        full.extend_from_slice(bytes);
    } else {
        let base = base.ok_or_else(|| io::Error::from_raw_os_error(D_EINVAL))?;
        full.try_reserve(base.len().saturating_add(bytes.len()).saturating_add(1))
            .map_err(|_| io::Error::from_raw_os_error(D_ENOMEM))?;
        full.extend_from_slice(&base);
        full.push(b'/');
        full.extend_from_slice(bytes);
        full.try_reserve(1)
            .map_err(|_| io::Error::from_raw_os_error(D_ENOMEM))?;
    }
    full.truncate((D_PATH_MAX as usize).saturating_sub(1));
    CString::new(full).map_err(|_| io::Error::from_raw_os_error(D_EINVAL))
}

fn guest_path_metadata(guest_path: &CStr, base: Option<&[u8]>) -> io::Result<fs::Metadata> {
    let bytes = guest_path.to_bytes();
    if bytes.is_empty() {
        return Err(io::Error::from_raw_os_error(2));
    }
    let path = if bytes.first() == Some(&b'/') {
        std::path::PathBuf::from(OsStr::from_bytes(bytes))
    } else {
        let base = base.ok_or_else(|| io::Error::from_raw_os_error(D_EINVAL))?;
        std::path::PathBuf::from(OsStr::from_bytes(base)).join(OsStr::from_bytes(bytes))
    };
    fs::metadata(path)
}

unsafe fn free_entry(entry: *mut PathFd) {
    if !entry.is_null() {
        ptr::drop_in_place(entry);
        free(entry.cast());
    }
}

#[no_mangle]
pub extern "C" fn lxrt_pathfd_open(
    dirfd: c_int,
    host_path: *const c_char,
    cloexec: c_int,
) -> c_int {
    ensure_fork_hooks();
    if host_path.is_null() {
        set_errno(D_EFAULT);
        return -1;
    }
    let guest_path = unsafe { CStr::from_ptr(host_path) };
    let base = if guest_path.to_bytes().first() == Some(&b'/') {
        None
    } else {
        match directory_path(dirfd) {
            Ok(base) => Some(base),
            Err(error) => {
                set_errno(io_error(error));
                return -1;
            }
        }
    };
    let metadata = match guest_path_metadata(guest_path, base.as_deref()) {
        Ok(metadata) => metadata,
        Err(error) => {
            set_errno(io_error(error));
            return -1;
        }
    };
    if !metadata.file_type().is_socket() {
        set_errno(D_EOPNOTSUPP);
        return -1;
    }

    let full = match full_path(guest_path, base.as_deref()) {
        Ok(path) => path,
        Err(error) => {
            set_errno(io_error(error));
            return -1;
        }
    };
    let flags = D_O_RDONLY | if cloexec != 0 { D_O_CLOEXEC } else { 0 };
    let fd = unsafe { open(b"/dev/null\0".as_ptr().cast(), flags) };
    if fd < 0 {
        return -1;
    }
    if fd as usize >= PF_MAX {
        unsafe { close(fd) };
        set_errno(D_EMFILE);
        return -1;
    }

    let null_metadata = match fs::metadata(format!("/dev/fd/{fd}")) {
        Ok(metadata) => metadata,
        Err(_) => {
            unsafe { close(fd) };
            set_errno(D_EMFILE);
            return -1;
        }
    };
    let replacement = unsafe { malloc(size_of::<PathFd>()).cast::<PathFd>() };
    if replacement.is_null() {
        unsafe { close(fd) };
        set_errno(D_ENOMEM);
        return -1;
    }
    unsafe {
        ptr::write(
            replacement,
            PathFd {
                path: full,
                dev: null_metadata.dev(),
                ino: null_metadata.ino(),
            },
        );
    }
    let _guard = PATH_FDS_LOCK
        .write()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let old = PATH_FDS[fd as usize].swap(replacement, Ordering::AcqRel);
    if !old.is_null() {
        unsafe { free_entry(old) };
    }
    fd
}

#[no_mangle]
pub extern "C" fn lxrt_pathfd_path(fd: c_int) -> *const c_char {
    ensure_fork_hooks();
    if fd < 0 || fd as usize >= PF_MAX {
        return ptr::null();
    }
    let guard = PATH_FDS_LOCK
        .read()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let entry = PATH_FDS[fd as usize].load(Ordering::Acquire);
    if entry.is_null() {
        return ptr::null();
    }
    let metadata = match fs::metadata(format!("/dev/fd/{fd}")) {
        Ok(metadata) => metadata,
        Err(error) => {
            set_errno(io_error(error));
            return ptr::null();
        }
    };
    let entry = unsafe { &*entry };
    if metadata.dev() != entry.dev || metadata.ino() != entry.ino {
        return ptr::null();
    }
    let result = PATH_SNAPSHOT.with(|snapshot| {
        let mut snapshot = snapshot.borrow_mut();
        snapshot.clear();
        snapshot.extend_from_slice(entry.path.as_bytes_with_nul());
        snapshot.as_ptr().cast::<c_char>()
    });
    drop(guard);
    result
}

#[no_mangle]
pub extern "C" fn lxrt_pathfd_close(fd: c_int) {
    ensure_fork_hooks();
    if fd < 0 || fd as usize >= PF_MAX {
        return;
    }
    let _guard = PATH_FDS_LOCK
        .write()
        .unwrap_or_else(std::sync::PoisonError::into_inner);
    let old = PATH_FDS[fd as usize].swap(ptr::null_mut(), Ordering::AcqRel);
    if !old.is_null() {
        unsafe { free_entry(old) };
    }
}
