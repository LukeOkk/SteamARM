// A synthetic /proc.
//
// ARCHITECTURE_TARGET §3.3 listed this as needed and it is: FEXServer dies on
// `read_symlink("/proc/self/exe")` before doing anything else, and Steam and
// pressure-vessel read /proc heavily.
//
// It is materialised as real files in a directory the runtime owns, and
// absolute guest paths under /proc/self (or /proc/<our pid>) are redirected
// there. That means an ordinary open/read/readlink works with no special cases
// in the syscall paths -- only a prefix substitution.
//
// Files whose content changes are regenerated on each lookup; the rest are
// written once.
//
// This is a line-for-line port of procfs.c. It is reached from guest syscall
// paths, so it keeps the C's shape: fixed-size buffers, libc string and
// formatting routines, the same pthread mutex and once, no Rust allocation
// (the two malloc()s are the C's), no panics.

use std::cell::UnsafeCell;
use std::ffi::{c_char, c_int, c_long, c_uint, c_void};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU8, Ordering};
use std::thread::LocalKey;

// Darwin arm64 values (<sys/fcntl.h>, <sys/errno.h>, <sys/stat.h>,
// <sys/dirent.h>, <sys/syslimits.h>, <sys/proc_info.h>, <unistd.h>,
// <mach/vm_prot.h>, <mach/vm_region.h>, <pthread.h>).
const PATH_MAX: usize = 1024;
const D_ESRCH: c_int = 3;
const D_EEXIST: c_int = 17;
const D_O_RDONLY: c_int = 0x0000;
const D_O_WRONLY: c_int = 0x0001;
const D_O_CREAT: c_int = 0x0200;
const D_O_TRUNC: c_int = 0x0400;
const D_O_DIRECTORY: c_int = 0x0010_0000;
const D_O_CLOEXEC: c_int = 0x0100_0000;
const D_AT_FDCWD: c_int = -2;
const D_F_GETFD: c_int = 1;
const D_F_SETFD: c_int = 2;
const D_F_GETPATH: c_int = 50;
const D_F_DUPFD_CLOEXEC: c_int = 67;
const D_FD_CLOEXEC: c_int = 1;
const D_F_OK: c_int = 0;
const D_SC_OPEN_MAX: c_int = 5;
const D_DT_UNKNOWN: u8 = 0;
const D_DT_DIR: u8 = 4;
const D_S_IFMT: u16 = 0o170000;
const D_S_IFDIR: u16 = 0o040000;
const D_PROC_PIDLISTFDS: c_int = 1;
const D_KERN_SUCCESS: c_int = 0;
const D_VM_PROT_READ: u32 = 1;
const D_VM_PROT_WRITE: u32 = 2;
const D_VM_PROT_EXECUTE: u32 = 4;
const D_SM_SHARED: u32 = 4;
// sizeof(vm_region_submap_info_data_64_t) / sizeof(natural_t): 84 bytes,
// #pragma pack(4).
const VM_REGION_SUBMAP_INFO_COUNT_64: u32 = 21;
const PTHREAD_RECURSIVE_MUTEX_SIG_INIT: c_long = 0x32AA_ABA2;
const PTHREAD_ONCE_SIG_INIT: c_long = 0x30B1_BCBA;

const FD_LIMIT: c_int = 65536;
const FD_BYTES: usize = 65536 / 8;
const MAPS_CAP: usize = 256 * 1024;

type ModeT = u16;
type Dir = c_void;

// struct stat, Darwin arm64 (always the 64-bit inode layout): 144 bytes.
#[repr(C)]
struct Stat {
    st_dev: i32,
    st_mode: ModeT,
    st_nlink: u16,
    st_ino: u64,
    st_uid: u32,
    st_gid: u32,
    st_rdev: i32,
    st_atime: i64,
    st_atime_nsec: c_long,
    st_mtime: i64,
    st_mtime_nsec: c_long,
    rest: [u64; 10],
}

// struct dirent, Darwin 64-bit inodes: 1048 bytes.
#[repr(C)]
struct Dirent {
    d_ino: u64,
    d_seekoff: u64,
    d_reclen: u16,
    d_namlen: u16,
    d_type: u8,
    d_name: [c_char; 1024],
}

// struct proc_fdinfo.
#[repr(C)]
struct ProcFdInfo {
    proc_fd: i32,
    proc_fdtype: u32,
}

// pthread_mutex_t, Darwin LP64: signature + 56 opaque bytes.
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}

impl PthreadMutex {
    // PTHREAD_RECURSIVE_MUTEX_INITIALIZER
    const RECURSIVE: Self = Self {
        sig: PTHREAD_RECURSIVE_MUTEX_SIG_INIT,
        opaque: [0; 56],
    };
}

// pthread_once_t, Darwin LP64: signature + 8 opaque bytes.
#[repr(C)]
struct PthreadOnce {
    sig: c_long,
    opaque: [u8; 8],
}

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: u32;

    fn __error() -> *mut c_int;
    fn access(path: *const c_char, mode: c_int) -> c_int;
    fn atoi(text: *const c_char) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn closedir(dir: *mut Dir) -> c_int;
    fn dirfd(dir: *mut Dir) -> c_int;
    fn fclose(stream: *mut c_void) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn fdopendir(fd: c_int) -> *mut Dir;
    fn fopen(path: *const c_char, mode: *const c_char) -> *mut c_void;
    fn fread(buffer: *mut c_void, size: usize, count: usize, stream: *mut c_void) -> usize;
    fn free(pointer: *mut c_void);
    fn futimens(fd: c_int, times: *const c_void) -> c_int;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn getgid() -> c_uint;
    fn getpid() -> c_int;
    fn getppid() -> c_int;
    fn getuid() -> c_uint;
    fn kill(pid: c_int, signal: c_int) -> c_int;
    fn lstat(path: *const c_char, out: *mut Stat) -> c_int;
    fn mach_vm_region_recurse(
        task: u32,
        address: *mut u64,
        size: *mut u64,
        depth: *mut u32,
        info: *mut i32,
        count: *mut u32,
    ) -> c_int;
    fn malloc(size: usize) -> *mut c_void;
    fn memcmp(left: *const c_void, right: *const c_void, length: usize) -> c_int;
    fn memcpy(destination: *mut c_void, source: *const c_void, length: usize) -> *mut c_void;
    fn mkdir(path: *const c_char, mode: ModeT) -> c_int;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn openat(dirfd: c_int, path: *const c_char, flags: c_int, ...) -> c_int;
    fn proc_pidinfo(
        pid: c_int,
        flavor: c_int,
        arg: u64,
        buffer: *mut c_void,
        size: c_int,
    ) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_once(once: *mut PthreadOnce, routine: extern "C" fn()) -> c_int;
    fn read(fd: c_int, buffer: *mut c_void, length: usize) -> isize;
    // Same layout as the struct dirent other modules declare for it.
    #[allow(clashing_extern_declarations)]
    fn readdir(dir: *mut Dir) -> *mut Dirent;
    fn readlink(path: *const c_char, buffer: *mut c_char, length: usize) -> isize;
    fn rmdir(path: *const c_char) -> c_int;
    fn setenv(name: *const c_char, value: *const c_char, overwrite: c_int) -> c_int;
    fn snprintf(buffer: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn stat(path: *const c_char, out: *mut Stat) -> c_int;
    fn strchr(text: *const c_char, character: c_int) -> *mut c_char;
    fn strcmp(left: *const c_char, right: *const c_char) -> c_int;
    fn strcspn(text: *const c_char, reject: *const c_char) -> usize;
    fn strlen(text: *const c_char) -> usize;
    fn strncmp(left: *const c_char, right: *const c_char, length: usize) -> c_int;
    fn strrchr(text: *const c_char, character: c_int) -> *mut c_char;
    fn strstr(haystack: *const c_char, needle: *const c_char) -> *mut c_char;
    fn strtol(text: *const c_char, end: *mut *mut c_char, base: c_int) -> c_long;
    fn symlink(target: *const c_char, path: *const c_char) -> c_int;
    fn sysconf(name: c_int) -> c_long;
    fn time(out: *mut i64) -> i64;
    fn unlink(path: *const c_char) -> c_int;
    fn write(fd: c_int, buffer: *const c_void, length: usize) -> isize;

    // android_ids.h, ids.h, lxrt.h, proc_ext.h
    fn lxrt_aids_status_ids(u: *mut u32, g: *mut u32, groups: *mut c_char, groups_len: usize);
    fn lxrt_gettid() -> c_int;
    fn lxrt_ids_on() -> bool;
    fn lxrt_ids_pid() -> c_int;
    fn lxrt_ids_to_guest(host_pid: c_int, index: c_int) -> c_int;
    fn lxrt_pathfd_path(fd: c_int) -> *const c_char;
    fn lxrt_proc_ext_translate(path: *const c_char, dir: *const c_char) -> *const c_char;
    fn lxrt_proc_gen_task_stat(tid: c_int, b: *mut c_char, cap: usize) -> usize;
    fn lxrt_procnet_translate(sub: *const c_char, dir: *const c_char) -> *const c_char;
    fn lxrt_procpid_environ(pid: c_int, out_path: *const c_char) -> bool;
    fn lxrt_procpid_fd_target(pid: c_int, fd: c_int, out: *mut c_char, n: usize) -> bool;
    fn lxrt_procpid_list(dir: *const c_char);
    fn lxrt_procpid_translate(rest: *const c_char, dir: *const c_char) -> *const c_char;
    fn lxrt_thread_list(tids: *mut c_int, max: c_int) -> c_int;
    fn lxrt_thread_lookup(tid: c_int, out: *mut usize) -> bool;
}

// A NUL-terminated string literal as a C pointer.
macro_rules! cs {
    ($text:expr) => {
        concat!($text, "\0").as_ptr().cast::<c_char>()
    };
}

// A character literal as the C `char` it is compared with.
#[inline(always)]
const fn ch(byte: u8) -> c_char {
    byte as c_char
}

#[inline(always)]
unsafe fn errno() -> c_int {
    *__error()
}

#[inline(always)]
unsafe fn set_errno(error: c_int) {
    *__error() = error;
}

#[inline(always)]
unsafe fn streq(left: *const c_char, right: *const c_char) -> bool {
    strcmp(left, right) == 0
}

// A file-scope `static char name[N]`: zero-initialised, written through raw
// pointers exactly as the C does, with whatever synchronisation the C had.
#[repr(transparent)]
struct CharBuf<const N: usize>(UnsafeCell<[c_char; N]>);

unsafe impl<const N: usize> Sync for CharBuf<N> {}

impl<const N: usize> CharBuf<N> {
    const fn new() -> Self {
        Self(UnsafeCell::new([0; N]))
    }

    #[inline(always)]
    fn ptr(&self) -> *mut c_char {
        self.0.get().cast::<c_char>()
    }
}

// A `static _Thread_local char name[N]`: neither has a destructor, so the
// access is a plain thread-local address and cannot fail.
type TlsBuf<const N: usize> = UnsafeCell<[c_char; N]>;

#[inline(always)]
fn tls_ptr<const N: usize>(key: &'static LocalKey<TlsBuf<N>>) -> *mut c_char {
    key.with(|buffer| buffer.get().cast::<c_char>())
}

thread_local! {
    static T_ROOT: TlsBuf<600> = const { UnsafeCell::new([0; 600]) };
    static T_TSBUF: TlsBuf<600> = const { UnsafeCell::new([0; 600]) };
    static T_SELFDIR: TlsBuf<600> = const { UnsafeCell::new([0; 600]) };
    static T_ROOTBUF: TlsBuf<1024> = const { UnsafeCell::new([0; 1024]) };
    static T_OUT: TlsBuf<1024> = const { UnsafeCell::new([0; 1024]) };
}

static G_DIR: CharBuf<512> = CharBuf::new();

// Descriptors that are the runtime's or the emulator's, not the guest's.
// Linux shows a process every descriptor it has, and so did this /proc; but
// here one process holds three parties' descriptors: the guest's, the
// runtime's (the private pipe ends behind an eventfd, epoll_eventfd.c) and,
// under FEX, the emulator's (its FEXServer connections, its rootfs and /proc
// directory descriptors). Android's zygote checks every descriptor it would
// hand to a child and aborts on any it does not know ("Unsupported st_mode
// for FD 35: FIFO", MEASURED), so /proc/self/fd leaves these out.
//   - the runtime marks its own (lxrt_fd_hide);
//   - FEX's are the ones it opened before the guest's first instruction:
//     what is open when FEX names the guest's argv (PR_SET_MM_MAP,
//     lxrt_proc_set_cmdline) and was not open when this runtime started
//     (inherited descriptors are the guest's: the exec'ing guest passed them).
// A hidden number that the guest closes, or dup2s over, is the guest's again
// (dispatch.c guest_close/forget_fd call lxrt_fd_hide(fd, false)).
static G_FD_HIDDEN: [AtomicU8; FD_BYTES] = [const { AtomicU8::new(0) }; FD_BYTES];
// Plain bytes in the C (`|=`, no atomics): a relaxed load and a relaxed store
// are the same instructions.
static G_FD_INHERITED: [AtomicU8; FD_BYTES] = [const { AtomicU8::new(0) }; FD_BYTES];

#[no_mangle]
pub extern "C" fn lxrt_fd_hide(fd: c_int, hide: bool) {
    if fd < 0 || fd >= FD_LIMIT {
        return;
    }
    let slot = unsafe { &*G_FD_HIDDEN.as_ptr().add((fd >> 3) as usize) };
    let bit = (1u32 << (fd & 7)) as u8;
    if hide {
        slot.fetch_or(bit, Ordering::Relaxed);
    } else {
        slot.fetch_and(!bit, Ordering::Relaxed);
    }
}

#[no_mangle]
pub extern "C" fn lxrt_fd_hidden(fd: c_int) -> bool {
    fd >= 0
        && fd < FD_LIMIT
        && (unsafe { &*G_FD_HIDDEN.as_ptr().add((fd >> 3) as usize) }.load(Ordering::Relaxed)
            >> (fd & 7))
            & 1
            != 0
}

// A descriptor the runtime keeps for itself inside the guest's table: moved
// above the numbers a guest expects next (Linux hands out the lowest free
// one), close-on-exec, and left out of /proc/self/fd. getdents64's duplicate
// of a directory the guest reads (dirents.rs) was not: Android's zygote, which
// checks every descriptor before it forks, found "Unsupported st_mode for FD
// 40: DIR" and aborted, one boot in about ten (MEASURED: zygote exit status
// 1 right at sys.boot_completed, the framework restarted).
//
// Opening one still passes through a low number for a moment, and a guest
// thread listing /proc/self/fd right then saw it (the zygote again: "Unable
// to stat 40" when it was gone by the fstat, "Unsupported st_mode for FD 40:
// DIR" when it was not, MEASURED). The listing and every open-then-move of
// a private descriptor take g_private_lock, so a listing sees each one
// either not yet open or already moved and hidden.
static mut G_PRIVATE_LOCK: PthreadMutex = PthreadMutex::RECURSIVE;
static mut G_PRIVATE_ONCE: PthreadOnce = PthreadOnce {
    sig: PTHREAD_ONCE_SIG_INIT,
    opaque: [0; 8],
};

extern "C" fn private_child() {
    // A fork from another thread during a listing: the child's only thread
    // must not find the lock held.
    unsafe { ptr::addr_of_mut!(G_PRIVATE_LOCK).write(PthreadMutex::RECURSIVE) };
}

extern "C" fn private_once() {
    unsafe { pthread_atfork(None, None, Some(private_child)) };
}

unsafe fn private_lock() {
    pthread_once(ptr::addr_of_mut!(G_PRIVATE_ONCE), private_once);
    pthread_mutex_lock(ptr::addr_of_mut!(G_PRIVATE_LOCK));
}

unsafe fn private_unlock() {
    pthread_mutex_unlock(ptr::addr_of_mut!(G_PRIVATE_LOCK));
}

const PRIVATE_BASES: [c_int; 4] = [800, 400, 200, 64];

// A private duplicate of `fd` (which stays the guest's), made at a high
// number directly: no low number in between.
#[no_mangle]
pub unsafe extern "C" fn lxrt_fd_dup_private(fd: c_int) -> c_int {
    for base in PRIVATE_BASES {
        let n = fcntl(fd, D_F_DUPFD_CLOEXEC, base);
        if n >= 0 {
            lxrt_fd_hide(n, true);
            return n;
        }
    }
    -1
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_fd_private(mut fd: c_int) -> c_int {
    if fd < 0 {
        return fd;
    }
    for base in PRIVATE_BASES {
        let n = fcntl(fd, D_F_DUPFD_CLOEXEC, base);
        if n >= 0 {
            close(fd);
            fd = n;
            break;
        }
    }
    fcntl(fd, D_F_SETFD, D_FD_CLOEXEC);
    lxrt_fd_hide(fd, true);
    fd
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_fdopendir_private(fd: c_int) -> *mut Dir {
    let fd = lxrt_fd_private(fd);
    if fd < 0 {
        return ptr::null_mut();
    }
    let d = fdopendir(fd);
    if d.is_null() {
        let e = errno();
        lxrt_fd_hide(fd, false);
        close(fd);
        set_errno(e);
    }
    d
}

// A file the runtime opens for itself, under the same rule: never at a low
// number a /proc/self/fd listing could catch.
#[no_mangle]
pub unsafe extern "C" fn lxrt_open_private(
    path: *const c_char,
    flags: c_int,
    mode: c_int,
) -> c_int {
    private_lock();
    let mut fd = open(path, flags, mode);
    if fd >= 0 {
        fd = lxrt_fd_private(fd);
    }
    private_unlock();
    fd
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_close_private(fd: c_int) {
    if fd < 0 {
        return;
    }
    close(fd);
    lxrt_fd_hide(fd, false);
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_opendirat_private(dirfd: c_int, path: *const c_char) -> *mut Dir {
    private_lock();
    let fd = openat(dirfd, path, D_O_RDONLY | D_O_DIRECTORY | D_O_CLOEXEC);
    let d = if fd < 0 {
        ptr::null_mut()
    } else {
        lxrt_fdopendir_private(fd)
    };
    private_unlock();
    d
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_opendir_private(path: *const c_char) -> *mut Dir {
    lxrt_opendirat_private(D_AT_FDCWD, path)
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_closedir_private(d: *mut Dir) {
    if d.is_null() {
        return;
    }
    let fd = dirfd(d);
    closedir(d);
    lxrt_fd_hide(fd, false);
}

// The open descriptors, in one call (a probe of every number cost 4096
// fcntl calls at every start); `function` gets each.
unsafe fn each_open_fd(function: unsafe fn(c_int)) {
    let mut n = proc_pidinfo(getpid(), D_PROC_PIDLISTFDS, 0, ptr::null_mut(), 0);
    if n <= 0 {
        return;
    }
    let fds = malloc(n as usize).cast::<ProcFdInfo>();
    if fds.is_null() {
        return;
    }
    n = proc_pidinfo(getpid(), D_PROC_PIDLISTFDS, 0, fds.cast::<c_void>(), n);
    let count = n / size_of::<ProcFdInfo>() as c_int;
    let mut i: c_int = 0;
    while i < count {
        let fd = (*fds.add(i as usize)).proc_fd;
        if fd >= 0 && fd < FD_LIMIT {
            function(fd);
        }
        i += 1;
    }
    free(fds.cast::<c_void>());
}

#[inline(always)]
unsafe fn inherited_slot(fd: c_int) -> &'static AtomicU8 {
    &*G_FD_INHERITED.as_ptr().add((fd >> 3) as usize)
}

unsafe fn mark_inherited(fd: c_int) {
    let slot = inherited_slot(fd);
    slot.store(
        slot.load(Ordering::Relaxed) | (1u32 << (fd & 7)) as u8,
        Ordering::Relaxed,
    );
}

unsafe fn hide_if_new(fd: c_int) {
    if fd > 2 && (inherited_slot(fd).load(Ordering::Relaxed) >> (fd & 7)) & 1 == 0 {
        lxrt_fd_hide(fd, true);
    }
}

unsafe fn note_inherited_fds() {
    each_open_fd(mark_inherited);
}

static G_EXE: CharBuf<1024> = CharBuf::new();

// The part of g_exe after its last '/': strrchr(g_exe, '/') ? that + 1 : g_exe.
#[inline(always)]
unsafe fn exe_base() -> *const c_char {
    let slash = strrchr(G_EXE.ptr(), b'/' as c_int);
    if slash.is_null() {
        G_EXE.ptr()
    } else {
        slash.add(1)
    }
}

// `static bool done` of hide_emulator_fds: a plain flag in the C.
static HIDE_EMULATOR_FDS_DONE: AtomicBool = AtomicBool::new(false);

unsafe fn hide_emulator_fds() {
    if HIDE_EMULATOR_FDS_DONE.load(Ordering::Relaxed)
        || !getenv(cs!("LXRT_SHOW_ALL_FDS")).is_null()
    {
        return;
    }
    HIDE_EMULATOR_FDS_DONE.store(true, Ordering::Relaxed);
    // Only when this image is FEX: a native guest that moves its own argv
    // keeps every descriptor it opened.
    let b = exe_base();
    if strncmp(b, cs!("FEX"), 3) != 0 {
        return;
    }
    each_open_fd(hide_if_new);
}

// (g_exe, declared above: the guest's name of its image)
static G_EXE_LINK: CharBuf<1024> = CharBuf::new(); // what <procfs>/exe points at (a host path)
// A plain `static bool` in the C.
static G_READY: AtomicBool = AtomicBool::new(false);

#[inline(always)]
fn ready() -> bool {
    G_READY.load(Ordering::Relaxed)
}

// This procfs lives in real files, and every write is a filesystem event the
// system's fseventsd records. Rewriting unchanged files and re-creating whole
// fd directories on every lookup pushed fseventsd to 5.9 GB and, with other
// memory pressure, the Mac into a watchdog panic (MEASURED 2026-09-27). So:
// content and links are only written when they differ from what is there.
#[no_mangle]
pub unsafe extern "C" fn lxrt_file_same(
    path: *const c_char,
    data: *const c_void,
    len: usize,
) -> bool {
    let fd = open(path, D_O_RDONLY);
    if fd < 0 {
        return false;
    }
    let mut buf = [0u8; 8192];
    let mut off: usize = 0;
    let mut same = true;
    loop {
        let r = read(fd, buf.as_mut_ptr().cast::<c_void>(), 8192);
        if r < 0 {
            same = false;
            break;
        }
        if r == 0 {
            break;
        }
        if off.wrapping_add(r as usize) > len
            || memcmp(
                buf.as_ptr().cast::<c_void>(),
                data.cast::<u8>().add(off).cast::<c_void>(),
                r as usize,
            ) != 0
        {
            same = false;
            break;
        }
        off = off.wrapping_add(r as usize);
    }
    close(fd);
    same && off == len
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_link_set(target: *const c_char, link: *const c_char) {
    let mut cur = [0 as c_char; PATH_MAX];
    let n = readlink(link, cur.as_mut_ptr(), PATH_MAX - 1);
    if n >= 0 {
        *cur.as_mut_ptr().add(n as usize) = 0;
        if streq(cur.as_ptr(), target) {
            return;
        }
    }
    unlink(link);
    symlink(target, link);
}

unsafe fn path_write_if_changed(path: *const c_char, data: *const c_char, len: usize) {
    if lxrt_file_same(path, data.cast::<c_void>(), len) {
        return;
    }
    let fd = open(path, D_O_WRONLY | D_O_CREAT | D_O_TRUNC, 0o644 as c_int);
    if fd < 0 {
        return;
    }
    let _ = write(fd, data.cast::<c_void>(), len);
    close(fd);
}

unsafe fn write_file(name: *const c_char, data: *const c_char, len: usize) {
    let mut path = [0 as c_char; 1024];
    snprintf(path.as_mut_ptr(), 1024, cs!("%s/%s"), G_DIR.ptr(), name);
    if lxrt_file_same(path.as_ptr(), data.cast::<c_void>(), len) {
        return;
    }
    let fd = open(
        path.as_ptr(),
        D_O_WRONLY | D_O_CREAT | D_O_TRUNC,
        0o644 as c_int,
    );
    if fd < 0 {
        return;
    }
    let _ = write(fd, data.cast::<c_void>(), len);
    close(fd);
}

// /proc/self/maps, generated from the real address space. FEX parses this to
// find where it and its guest are mapped, so the addresses have to be true
// even though the permissions and names are approximations.
unsafe fn regenerate_maps() {
    let buf = malloc(MAPS_CAP).cast::<c_char>();
    if buf.is_null() {
        return;
    }
    let mut used: usize = 0;

    let mut addr: u64 = 0;
    loop {
        let mut size: u64 = 0;
        // vm_region_submap_info_data_64_t: protection is the first word,
        // share_mode the byte at offset 47.
        let mut info = [0u32; VM_REGION_SUBMAP_INFO_COUNT_64 as usize];
        let mut count: u32 = VM_REGION_SUBMAP_INFO_COUNT_64;
        let mut depth: u32 = 0;
        let kr = mach_vm_region_recurse(
            mach_task_self_,
            &mut addr,
            &mut size,
            &mut depth,
            info.as_mut_ptr().cast::<i32>(),
            &mut count,
        );
        if kr != D_KERN_SUCCESS || used.wrapping_add(256) > MAPS_CAP {
            break;
        }
        let protection = info[0];
        let share_mode = info[11] >> 24;
        used = used.wrapping_add(snprintf(
            buf.add(used),
            MAPS_CAP - used,
            cs!("%012llx-%012llx %c%c%c%c 00000000 00:00 0\n"),
            addr,
            addr.wrapping_add(size),
            (if protection & D_VM_PROT_READ != 0 { b'r' } else { b'-' }) as c_int,
            (if protection & D_VM_PROT_WRITE != 0 { b'w' } else { b'-' }) as c_int,
            (if protection & D_VM_PROT_EXECUTE != 0 { b'x' } else { b'-' }) as c_int,
            (if share_mode == D_SM_SHARED { b's' } else { b'p' }) as c_int,
        ) as usize);
        addr = addr.wrapping_add(size);
    }
    write_file(cs!("maps"), buf, used);
    free(buf.cast::<c_void>());
}

// A pseudo-terminal slave by its Linux name: Darwin's /dev/ttysNNN is
// /dev/pts/N to the guest (dispatch.c maps the name back), so ttyname()'s
// readlink-then-stat finds the terminal.
unsafe fn pts_name(target: *mut c_char, n: usize) {
    if strncmp(target, cs!("/dev/ttys"), 9) != 0
        || *target.add(9) < ch(b'0')
        || *target.add(9) > ch(b'9')
    {
        return;
    }
    let num = atoi(target.add(9));
    snprintf(target, n, cs!("/dev/pts/%d"), num);
}

// What /proc/self/fd/<fd> points at: the O_PATH emulation's name, Darwin's
// F_GETPATH, the socket/pipe description, or an anonymous inode.
#[inline(always)]
unsafe fn fd_target(fd: c_int, target: *mut c_char) {
    if !lxrt_pathfd_path(fd).is_null() {
        snprintf(target, 1024, cs!("%s"), lxrt_pathfd_path(fd));
    } else if fcntl(fd, D_F_GETPATH, target) != 0
        && !lxrt_procpid_fd_target(getpid(), fd, target, 1024)
    {
        snprintf(target, 1024, cs!("anon_inode:[fd%d]"), fd);
    }
    pts_name(target, 1024);
}

// /proc/self/fd, as a directory of symlinks. Rebuilt on every lookup because
// the fd table changes constantly; F_GETPATH gives the target where Darwin
// knows one, and sockets and pipes get a descriptive name the way Linux does.
unsafe fn regenerate_fds() {
    let mut dir = [0 as c_char; 600];
    snprintf(dir.as_mut_ptr(), 600, cs!("%s/fd"), G_DIR.ptr());
    mkdir(dir.as_ptr(), 0o700);

    let mut maxfd = sysconf(D_SC_OPEN_MAX) as c_int;
    if maxfd < 0 || maxfd > 4096 {
        maxfd = 4096;
    }
    // Stale entries: only the descriptors that are closed now.
    let existing = lxrt_opendir_private(dir.as_ptr());
    if !existing.is_null() {
        loop {
            let e = readdir(existing);
            if e.is_null() {
                break;
            }
            let name = ptr::addr_of!((*e).d_name).cast::<c_char>();
            if *name == ch(b'.') {
                continue;
            }
            let n = atoi(name);
            if n >= 0 && n < maxfd && fcntl(n, D_F_GETFD) >= 0 {
                continue;
            }
            let mut p = [0 as c_char; 1200];
            snprintf(p.as_mut_ptr(), 1200, cs!("%s/%s"), dir.as_ptr(), name);
            unlink(p.as_ptr());
        }
        lxrt_closedir_private(existing);
    }
    private_lock();
    let mut fd: c_int = 0;
    while fd < maxfd {
        if fcntl(fd, D_F_GETFD) < 0 {
            fd += 1;
            continue;
        }
        if lxrt_fd_hidden(fd) {
            let mut hl = [0 as c_char; 1200];
            snprintf(hl.as_mut_ptr(), 1200, cs!("%s/%d"), dir.as_ptr(), fd);
            unlink(hl.as_ptr());
            fd += 1;
            continue;
        }
        let mut target = [0 as c_char; 1024];
        fd_target(fd, target.as_mut_ptr());
        let mut link = [0 as c_char; 1200];
        snprintf(link.as_mut_ptr(), 1200, cs!("%s/%d"), dir.as_ptr(), fd);
        lxrt_link_set(target.as_ptr(), link.as_ptr());
        fd += 1;
    }
    private_unlock();
}

// /proc/self/fd/<n> alone: refresh that one link. pressure-vessel resolves
// thousands of descriptors this way (readlink of /proc/self/fd/N after every
// openat), and rebuilding the whole directory each time -- an fcntl on every
// possible descriptor plus an unlink and a symlink per open one -- was most
// of its minute and a half of start-up (MEASURED with sample(1)).
unsafe fn regenerate_one_fd(fd: c_int) {
    let mut dir = [0 as c_char; 600];
    snprintf(dir.as_mut_ptr(), 600, cs!("%s/fd"), G_DIR.ptr());
    let mut link = [0 as c_char; 1200];
    snprintf(link.as_mut_ptr(), 1200, cs!("%s/%d"), dir.as_ptr(), fd);
    private_lock();
    let gone = fcntl(fd, D_F_GETFD) < 0 || lxrt_fd_hidden(fd);
    private_unlock();
    if gone {
        unlink(link.as_ptr()); // closed (or not the guest's): ENOENT
        return;
    }
    mkdir(dir.as_ptr(), 0o700);
    let mut target = [0 as c_char; 1024];
    fd_target(fd, target.as_mut_ptr());
    lxrt_link_set(target.as_ptr(), link.as_ptr());
}

// Not nftw(): it opens each directory at the lowest free descriptor, and a
// guest thread that exits removes its /proc/self/task entry while the others
// run -- Android's zygote stops its daemon threads before a fork and then
// checks every descriptor, and one exiting thread's directory was there:
// "Unsupported st_mode for FD 39: DIR", the zygote aborted (MEASURED, about one
// boot in twenty). Private descriptors only (lxrt_opendir_private).
unsafe fn remove_tree(dir: *const c_char) {
    let d = lxrt_opendir_private(dir);
    if !d.is_null() {
        loop {
            let e = readdir(d);
            if e.is_null() {
                break;
            }
            let name = ptr::addr_of!((*e).d_name).cast::<c_char>();
            if streq(name, cs!(".")) || streq(name, cs!("..")) {
                continue;
            }
            let mut p = [0 as c_char; 1200];
            snprintf(p.as_mut_ptr(), 1200, cs!("%s/%s"), dir, name);
            let mut isdir = (*e).d_type == D_DT_DIR;
            if (*e).d_type == D_DT_UNKNOWN {
                let mut st: Stat = std::mem::zeroed();
                isdir = lstat(p.as_ptr(), &mut st) == 0 && st.st_mode & D_S_IFMT == D_S_IFDIR;
            }
            if isdir {
                remove_tree(p.as_ptr());
            } else {
                unlink(p.as_ptr());
            }
        }
        lxrt_closedir_private(d);
    }
    rmdir(dir);
}

// A process that exits removes its directory (lxrt_proc_cleanup); one that is
// killed cannot. Every so often -- at most once a minute, whoever starts
// then -- the directories of pids that no longer exist are swept, or /tmp
// fills with thousands of them over a long session.
unsafe fn sweep_dead(tmp: *const c_char) {
    let mut mark = [0 as c_char; 600];
    snprintf(mark.as_mut_ptr(), 600, cs!("%s/lxrt-proc-sweep"), tmp);
    let mut st: Stat = std::mem::zeroed();
    let now = time(ptr::null_mut());
    if stat(mark.as_ptr(), &mut st) == 0 && now.wrapping_sub(st.st_mtime) < 60 {
        return;
    }
    let fd = open(mark.as_ptr(), D_O_WRONLY | D_O_CREAT, 0o600 as c_int);
    if fd >= 0 {
        futimens(fd, ptr::null());
        close(fd);
    }
    let d = lxrt_opendir_private(tmp);
    if d.is_null() {
        return;
    }
    loop {
        let e = readdir(d);
        if e.is_null() {
            break;
        }
        let name = ptr::addr_of!((*e).d_name).cast::<c_char>();
        if strncmp(name, cs!("lxrt-proc-"), 10) != 0 {
            continue;
        }
        let mut end: *mut c_char = ptr::null_mut();
        let pid = strtol(name.add(10), &mut end, 10);
        if *end != 0 || pid <= 0 || pid == getpid() as c_long {
            continue;
        }
        if kill(pid as c_int, 0) == 0 || errno() != D_ESRCH {
            continue;
        }
        let mut path = [0 as c_char; 900];
        snprintf(path.as_mut_ptr(), 900, cs!("%s/%s"), tmp, name);
        remove_tree(path.as_ptr());
    }
    lxrt_closedir_private(d);
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_proc_cleanup() {
    if *G_DIR.ptr() != 0 && !strstr(G_DIR.ptr(), cs!("/lxrt-proc-")).is_null() {
        remove_tree(G_DIR.ptr());
    }
}

// getenv(name) when it is an absolute path of an existing directory.
#[inline(always)]
unsafe fn env_directory(name: *const c_char) -> *const c_char {
    let c = getenv(name).cast_const();
    let mut st: Stat = std::mem::zeroed();
    if !c.is_null()
        && *c == ch(b'/')
        && stat(c, &mut st) == 0
        && st.st_mode & D_S_IFMT == D_S_IFDIR
    {
        c
    } else {
        ptr::null()
    }
}

// The host's temporary directory for the runtime's own files (/proc,
// memfds, sandboxes): fixed once per process tree and handed down in
// LXRT_HOST_TMPDIR, because TMPDIR is the guest's -- Termux points it into
// its prefix, a guest path the host does not have, and the runtime's /proc
// went missing ("lxrun: open /proc/self/exe: No such file or directory",
// MEASURED). Processes that look for each other's /proc must also agree.
#[no_mangle]
pub unsafe extern "C" fn lxrt_host_tmpdir() -> *const c_char {
    static DIR: CharBuf<512> = CharBuf::new();
    let dir = DIR.ptr();
    if *dir != 0 {
        return dir;
    }
    let mut c = env_directory(cs!("LXRT_HOST_TMPDIR"));
    if c.is_null() {
        c = env_directory(cs!("TMPDIR"));
        if c.is_null() {
            c = cs!("/tmp");
        }
    }
    snprintf(dir, 512, cs!("%s"), c);
    let mut n = strlen(dir);
    while n > 1 && *dir.add(n - 1) == ch(b'/') {
        n -= 1;
        *dir.add(n) = 0;
    }
    setenv(cs!("LXRT_HOST_TMPDIR"), dir, 1);
    dir
}

// lxrt_ids_on() ? (lxrt_ids_to_guest(getppid(), 0) ?: 1) : getppid()
#[inline(always)]
unsafe fn guest_ppid() -> c_int {
    if lxrt_ids_on() {
        let guest = lxrt_ids_to_guest(getppid(), 0);
        if guest != 0 {
            guest
        } else {
            1
        }
    } else {
        getppid()
    }
}

// The `self` and `<pid>` links of the directory, and its `exe` link.
#[inline(always)]
unsafe fn make_self_and_exe_links() {
    // A `self` symlink inside the directory makes /proc/self resolve for
    // anything that walks the tree rather than opening the path directly.
    let mut selflink = [0 as c_char; 600];
    snprintf(selflink.as_mut_ptr(), 600, cs!("%s/self"), G_DIR.ptr());
    unlink(selflink.as_ptr());
    // Linux: readlink("/proc/self") is "<pid>", and realpath() walks through
    // it -- "." made realpath("/proc/self/exe") come out as "/proc/exe", and
    // steam-runtime-tools could not find its own executable. "<pid>" -> "."
    // keeps the tree walkable.
    {
        let mut pidname = [0 as c_char; 32];
        let mut pidlink = [0 as c_char; 640];
        snprintf(pidname.as_mut_ptr(), 32, cs!("%d"), lxrt_ids_pid());
        snprintf(
            pidlink.as_mut_ptr(),
            640,
            cs!("%s/%s"),
            G_DIR.ptr(),
            pidname.as_ptr(),
        );
        unlink(pidlink.as_ptr());
        symlink(cs!("."), pidlink.as_ptr());
        symlink(pidname.as_ptr(), selflink.as_ptr());
    }
}

#[inline(always)]
unsafe fn make_exe_link() {
    let mut link = [0 as c_char; 1024];
    snprintf(link.as_mut_ptr(), 1024, cs!("%s/exe"), G_DIR.ptr());
    unlink(link.as_ptr());
    symlink(G_EXE_LINK.ptr(), link.as_ptr());
}

// The process's own `status`, as it is first written.
#[inline(always)]
unsafe fn write_process_status(name: *const c_char) {
    let mut status = [0 as c_char; 512];
    let sl = snprintf(
        status.as_mut_ptr(),
        512,
        cs!(
            "Name:\t%s\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\n\
             PPid:\t%d\nUid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\n\
             Threads:\t1\n"
        ),
        name,
        lxrt_ids_pid(),
        lxrt_ids_pid(),
        guest_ppid(),
        getuid(),
        getuid(),
        getuid(),
        getuid(),
        getgid(),
        getgid(),
        getgid(),
        getgid(),
    );
    // As in the C, the length is snprintf's return value.
    write_file(cs!("status"), status.as_ptr(), sl as usize);
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_proc_init(
    exe_path: *const c_char,
    exe_link: *const c_char,
    argc: c_int,
    argv: *mut *mut c_char,
) {
    let tmp = lxrt_host_tmpdir();
    sweep_dead(tmp);
    snprintf(G_DIR.ptr(), 512, cs!("%s/lxrt-proc-%d"), tmp, getpid());
    note_inherited_fds();
    if mkdir(G_DIR.ptr(), 0o700) != 0 && errno() != D_EEXIST {
        return;
    }

    // The guest's own view of its path, not the host's: a guest that re-execs
    // on this must get something the runtime can resolve again.
    make_self_and_exe_links();

    snprintf(
        G_EXE.ptr(),
        1024,
        cs!("%s"),
        if exe_path.is_null() {
            cs!("/unknown")
        } else {
            exe_path
        },
    );
    // The link is followed by the host kernel (open) and read back through
    // readlinkat's guest view (readlink): a host path serves both. A relative
    // target would even be resolved against this directory, not the cwd.
    snprintf(
        G_EXE_LINK.ptr(),
        1024,
        cs!("%s"),
        if exe_link.is_null() {
            G_EXE.ptr().cast_const()
        } else {
            exe_link
        },
    );
    make_exe_link();

    let mut cmdline = [0 as c_char; 4096];
    let mut n: usize = 0;
    let mut i: c_int = 0;
    while i < argc && n + 1 < 4096 {
        let arg = *argv.add(i as usize);
        let l = strlen(arg);
        if n.wrapping_add(l).wrapping_add(1) >= 4096 {
            break;
        }
        memcpy(
            cmdline.as_mut_ptr().add(n).cast::<c_void>(),
            arg.cast::<c_void>(),
            l,
        );
        n += l;
        *cmdline.as_mut_ptr().add(n) = 0;
        n += 1;
        i += 1;
    }
    write_file(cs!("cmdline"), cmdline.as_ptr(), n);

    write_process_status(if argc > 0 {
        (*argv).cast_const()
    } else {
        cs!("lxrt")
    });

    G_READY.store(true, Ordering::Relaxed);
}

// Returns a host path for a /proc path the runtime synthesises, or NULL if it
// does not provide that file. NULL means the caller should fall through to the
// normal translation, which will fail with ENOENT -- the honest answer.
// The guest's own executable path, as the guest names it. FEX re-execs
// itself as execve("/proc/self/exe", ...) to run the next x86 program; the
// child runtime must open the real image, not the literal magic path.
// /proc/self/task: one directory per guest thread, each with status and comm.
// Chromium's thread_helpers.cc opendir()s it and CHECKs the result, which is
// how steamwebhelper's subprocesses died before this existed.
#[no_mangle]
pub unsafe extern "C" fn lxrt_proc_thread_gone(tid: c_int) {
    if !ready() || tid <= 0 || tid == lxrt_ids_pid() {
        return;
    }
    let mut sub = [0 as c_char; 700];
    snprintf(sub.as_mut_ptr(), 700, cs!("%s/task/%d"), G_DIR.ptr(), tid);
    remove_tree(sub.as_ptr());
}

unsafe fn regenerate_tasks() {
    let mut dir = [0 as c_char; 600];
    snprintf(dir.as_mut_ptr(), 600, cs!("%s/task"), G_DIR.ptr());
    mkdir(dir.as_ptr(), 0o700);
    let mut tids = [0 as c_int; 512];
    let tids = tids.as_mut_ptr();
    let mut n = lxrt_thread_list(tids, 512);
    if n > 512 {
        n = 512;
    }
    // The main thread may not be registered yet; it is always present.
    let mut have_main = false;
    let mut i: c_int = 0;
    while i < n {
        if *tids.add(i as usize) == lxrt_ids_pid() {
            have_main = true;
        }
        i += 1;
    }
    if !have_main && n < 512 {
        // (n is not negative here in practice; the C indexes with it as is.)
        *tids.offset(n as isize) = lxrt_ids_pid();
        n += 1;
    }
    // Entries of threads that are gone go too.
    let d = lxrt_opendir_private(dir.as_ptr());
    if !d.is_null() {
        loop {
            let e = readdir(d);
            if e.is_null() {
                break;
            }
            let name = ptr::addr_of!((*e).d_name).cast::<c_char>();
            let mut end: *mut c_char = ptr::null_mut();
            let t = strtol(name, &mut end, 10);
            if *end != 0 || t <= 0 {
                continue;
            }
            let mut live = false;
            let mut i: c_int = 0;
            while i < n && !live {
                live = *tids.add(i as usize) as c_long == t;
                i += 1;
            }
            if !live {
                let mut sub = [0 as c_char; 900];
                snprintf(sub.as_mut_ptr(), 900, cs!("%s/%s"), dir.as_ptr(), name);
                remove_tree(sub.as_ptr());
            }
        }
        lxrt_closedir_private(d);
    }
    let mut i: c_int = 0;
    while i < n {
        write_task_files(*tids.add(i as usize), n);
        i += 1;
    }
}

// The files of one thread's /proc/self/task/<tid>. stat is regenerated on
// every lookup: its CPU times are what a reader is after.
unsafe fn write_task_files(tid: c_int, nthreads: c_int) {
    let mut sub = [0 as c_char; 700];
    snprintf(sub.as_mut_ptr(), 700, cs!("%s/task/%d"), G_DIR.ptr(), tid);
    mkdir(sub.as_ptr(), 0o700);
    let mut f = [0 as c_char; 800];
    let mut b = [0 as c_char; 1024];
    let nm = if *G_EXE.ptr() != 0 {
        exe_base()
    } else {
        cs!("lxrt")
    };
    let mut bl = snprintf(b.as_mut_ptr(), 1024, cs!("%s\n"), nm);
    snprintf(f.as_mut_ptr(), 800, cs!("%s/comm"), sub.as_ptr());
    path_write_if_changed(f.as_ptr(), b.as_ptr(), bl as usize);
    let mut u = [0u32; 4];
    let mut g = [0u32; 4];
    lxrt_aids_status_ids(u.as_mut_ptr(), g.as_mut_ptr(), ptr::null_mut(), 0); // Android ids: the virtual ones
    bl = snprintf(
        b.as_mut_ptr(),
        1024,
        cs!(
            "Name:\t%s\nState:\tS (sleeping)\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\n\
             Uid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nThreads:\t%d\n"
        ),
        exe_base(),
        lxrt_ids_pid(),
        tid,
        guest_ppid(),
        u[0],
        u[1],
        u[2],
        u[3],
        g[0],
        g[1],
        g[2],
        g[3],
        nthreads,
    );
    snprintf(f.as_mut_ptr(), 800, cs!("%s/status"), sub.as_ptr());
    path_write_if_changed(f.as_ptr(), b.as_ptr(), bl as usize);
    let sl = lxrt_proc_gen_task_stat(tid, b.as_mut_ptr(), 1024);
    if sl != 0 {
        snprintf(f.as_mut_ptr(), 800, cs!("%s/stat"), sub.as_ptr());
        path_write_if_changed(f.as_ptr(), b.as_ptr(), sl);
    }
}

// /proc/self/task/<tid>/...: that thread's directory only. Wine reads
// task/<tid>/stat for every GetThreadTimes of another thread; rewriting every
// thread's files each time took a sixth of a busy thread's time (MEASURED).
unsafe fn regenerate_one_task(tid: c_int) {
    let mut th: usize = 0; // pthread_t
    if tid != lxrt_ids_pid() && !lxrt_thread_lookup(tid, &mut th) {
        lxrt_proc_thread_gone(tid);
        return;
    }
    let mut dir = [0 as c_char; 600];
    snprintf(dir.as_mut_ptr(), 600, cs!("%s/task"), G_DIR.ptr());
    mkdir(dir.as_ptr(), 0o700);
    // comm and status do not change; once the directory is there only stat
    // is rewritten.
    let mut f = [0 as c_char; 800];
    snprintf(f.as_mut_ptr(), 800, cs!("%s/%d/status"), dir.as_ptr(), tid);
    if access(f.as_ptr(), D_F_OK) != 0 {
        let mut tids = [0 as c_int; 1];
        let n = lxrt_thread_list(tids.as_mut_ptr(), 1);
        write_task_files(tid, if n > 0 { n } else { 1 });
        return;
    }
    let mut b = [0 as c_char; 1024];
    let sl = lxrt_proc_gen_task_stat(tid, b.as_mut_ptr(), 1024);
    if sl != 0 {
        snprintf(f.as_mut_ptr(), 800, cs!("%s/%d/stat"), dir.as_ptr(), tid);
        path_write_if_changed(f.as_ptr(), b.as_ptr(), sl);
    }
}

// After fork(), the child is a new pid with the parent's g_dir: every
// /proc/self regeneration it did went into the PARENT's directory (maps, fd,
// task, status all describing the child), and the parent read them back as
// its own. Give the child its own directory; the exe and cmdline stay.
#[no_mangle]
pub unsafe extern "C" fn lxrt_proc_after_fork() {
    if !ready() {
        return;
    }
    let tmp = lxrt_host_tmpdir();
    let mut cmdline_src = [0 as c_char; 600];
    snprintf(cmdline_src.as_mut_ptr(), 600, cs!("%s/cmdline"), G_DIR.ptr());
    snprintf(G_DIR.ptr(), 512, cs!("%s/lxrt-proc-%d"), tmp, getpid());
    if mkdir(G_DIR.ptr(), 0o700) != 0 && errno() != D_EEXIST {
        return;
    }
    make_self_and_exe_links();
    make_exe_link();
    // cmdline: copy the parent's.
    let input = fopen(cmdline_src.as_ptr(), cs!("rb"));
    if !input.is_null() {
        let mut buf = [0 as c_char; 4096];
        let n = fread(buf.as_mut_ptr().cast::<c_void>(), 1, 4096, input);
        fclose(input);
        write_file(cs!("cmdline"), buf.as_ptr(), n);
    }
    write_process_status(exe_base());
}

// prctl(PR_SET_MM, PR_SET_MM_MAP): the process moved its argv (FEX does,
// so /proc/<pid>/cmdline shows the x86 program rather than FEX itself).
#[no_mangle]
pub unsafe extern "C" fn lxrt_proc_set_cmdline(args: *const c_char, len: usize) {
    hide_emulator_fds();
    if ready() && !args.is_null() && len != 0 {
        write_file(cs!("cmdline"), args, len);
    }
}

#[no_mangle]
pub extern "C" fn lxrt_proc_exe_path() -> *const c_char {
    if ready() {
        G_EXE.ptr()
    } else {
        ptr::null()
    }
}

// The host path of this process's <procfs>/exe link file. readlink of it is
// answered with lxrt_proc_exe_path(), not with the link's host target.
#[no_mangle]
pub unsafe extern "C" fn lxrt_proc_exe_link_file(out: *mut c_char, n: usize) -> *const c_char {
    if !ready() || snprintf(out, n, cs!("%s/exe"), G_DIR.ptr()) >= n as c_int {
        return ptr::null();
    }
    out
}

// The reverse of lxrt_proc_translate, for readlink: a host path inside the
// materialised /proc (what F_GETPATH gives for /proc/self/fd links) becomes
// the /proc path Linux would show. FEX recognises its emulated files --
// /proc/cpuinfo above all -- by exactly this readlink after the open; with
// the host path it served the Apple cpuinfo to x86 guests, and the Steam
// client concluded it ran on arm64 (MEASURED: it installed the ARM64 Proton
// builds and Steam Linux Runtime 4.0 - Arm64).
#[no_mangle]
pub unsafe extern "C" fn lxrt_proc_untranslate(
    host: *const c_char,
    out: *mut c_char,
    n: usize,
) -> *const c_char {
    if host.is_null() {
        return ptr::null();
    }
    // Any process's materialised /proc: <tmpdir>/lxrt-proc-<pid>[/...]. The
    // link may be read by another process than the one that made it (a
    // child that inherited the descriptor).
    let m = strstr(host, cs!("/lxrt-proc-")).cast_const();
    if m.is_null() {
        return ptr::null();
    }
    let mut end: *mut c_char = ptr::null_mut();
    let owner = strtol(m.add(11), &mut end, 10);
    if end.cast_const() == m.add(11) || (*end != ch(b'/') && *end != 0) {
        return ptr::null();
    }
    let rest: *const c_char = if *end != 0 { end.add(1) } else { cs!("") };
    if *rest == 0 {
        snprintf(out, n, cs!("/proc"));
        return out;
    }
    // Other guest processes live under .p/<pid>/ (procpid.c).
    if strncmp(rest, cs!(".p/"), 3) == 0 {
        let mut endpid: *mut c_char = ptr::null_mut();
        let host = strtol(rest.add(3), &mut endpid, 10);
        let guest = if lxrt_ids_on() {
            lxrt_ids_to_guest(host as c_int, 0)
        } else {
            host as c_int
        };
        if host > 0 && guest > 0 && (*endpid == ch(b'/') || *endpid == 0) {
            snprintf(out, n, cs!("/proc/%d%s"), guest, endpid.cast_const());
        } else {
            snprintf(out, n, cs!("/proc/%s"), rest.add(3));
        }
        return out;
    }
    if strncmp(rest, cs!(".net"), 4) == 0 && (*rest.add(4) == ch(b'/') || *rest.add(4) == 0) {
        snprintf(out, n, cs!("/proc/net%s"), rest.add(4));
        return out;
    }
    const PER_PROCESS: [&[u8]; 14] = [
        b"cmdline\0",
        b"status\0",
        b"maps\0",
        b"environ\0",
        b"exe\0",
        b"fd\0",
        b"task\0",
        b"mountinfo\0",
        b"statm\0",
        b"comm\0",
        b"auxv\0",
        b"limits\0",
        b"cgroup\0",
        b"self\0",
    ];
    let first = strcspn(rest, cs!("/"));
    let mut owner_guest = if lxrt_ids_on() {
        lxrt_ids_to_guest(owner as c_int, 0)
    } else {
        owner as c_int
    };
    if owner_guest == 0 {
        owner_guest = 1;
    }
    for entry in PER_PROCESS {
        let name = entry.as_ptr().cast::<c_char>();
        if strlen(name) == first && strncmp(rest, name, first) == 0 {
            if streq(name, cs!("self")) {
                snprintf(out, n, cs!("/proc/%d%s"), owner_guest, rest.add(first));
            } else {
                snprintf(out, n, cs!("/proc/%d/%s"), owner_guest, rest);
            }
            return out;
        }
    }
    snprintf(out, n, cs!("/proc/%s"), rest);
    out
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_proc_translate(path: *const c_char) -> *const c_char {
    if !ready() || path.is_null() {
        return ptr::null();
    }
    if strcmp(path, cs!("/proc")) != 0 && strncmp(path, cs!("/proc/"), 6) != 0 {
        return ptr::null();
    }

    // `/proc` itself, and `/proc/` -- FEX opens it to enumerate processes and
    // warns loudly if it cannot.
    if *path.add(5) == 0 || (*path.add(5) == ch(b'/') && *path.add(6) == 0) {
        // A program that opens /proc itself will walk it with openat(fd,
        // "self/task") -- relative paths this translator never sees -- so
        // everything it might look for has to exist by then. Chromium's
        // sandbox helpers do exactly that (PCHECK on the result).
        regenerate_fds();
        regenerate_tasks();
        lxrt_procpid_list(G_DIR.ptr()); // the other guest processes, /proc/net
        let root = tls_ptr(&T_ROOT);
        snprintf(root, 600, cs!("%s"), G_DIR.ptr());
        return root;
    }

    // Files proc_ext.c synthesises (meminfo, cpuinfo, mountinfo, sys/*, ...)
    // are handled there; this file keeps the per-process ones it already owns.
    let ext = lxrt_proc_ext_translate(path, G_DIR.ptr());
    if !ext.is_null() {
        return ext;
    }

    let mut rest = path.add(6);
    // /proc/thread-self: the calling thread's /proc/self/task/<tid>, except
    // attr/ (below), which is per process here.
    if strncmp(rest, cs!("thread-self"), 11) == 0
        && (*rest.add(11) == 0 || *rest.add(11) == ch(b'/'))
    {
        let tsbuf = tls_ptr(&T_TSBUF);
        if strncmp(rest.add(11), cs!("/attr/"), 6) == 0 {
            snprintf(tsbuf, 600, cs!("self%s"), rest.add(11));
        } else {
            snprintf(
                tsbuf,
                600,
                cs!("self/task/%d%s"),
                lxrt_gettid(),
                rest.add(11),
            );
        }
        rest = tsbuf;
    }
    // /proc/net/...: the socket tables of every guest process (procpid.c).
    {
        let net = lxrt_procnet_translate(rest, G_DIR.ptr());
        if !net.is_null() {
            return net;
        }
    }
    // The directory itself: /proc/self and /proc/<our pid>. A stat() of it
    // is how a program checks that /proc is mounted at all.
    {
        let mut mypid = [0 as c_char; 32];
        snprintf(mypid.as_mut_ptr(), 32, cs!("%d"), lxrt_ids_pid());
        if streq(rest, cs!("self")) || streq(rest, mypid.as_ptr()) {
            regenerate_fds();
            regenerate_tasks();
            // /proc/self is the symlink (readlink gives "<pid>"); /proc/<pid>
            // is the directory itself, so glibc's realpath(), which calls
            // readlink on every component, sees EINVAL there and goes on.
            let selfdir = tls_ptr(&T_SELFDIR);
            if streq(rest, cs!("self")) {
                snprintf(selfdir, 600, cs!("%s/self"), G_DIR.ptr());
            } else {
                snprintf(selfdir, 600, cs!("%s"), G_DIR.ptr());
            }
            return selfdir;
        }
    }
    if strncmp(rest, cs!("self/"), 5) == 0 {
        rest = rest.add(5);
    } else {
        // /proc/<pid>/ for our own pid is the same thing.
        let mut mypid = [0 as c_char; 32];
        let k = snprintf(mypid.as_mut_ptr(), 32, cs!("%d/"), lxrt_ids_pid());
        if strncmp(rest, mypid.as_ptr(), k as usize) != 0 {
            return lxrt_procpid_translate(rest, G_DIR.ptr()); // another guest process
        }
        rest = rest.offset(k as isize);
    }
    {
        let net = lxrt_procnet_translate(rest, G_DIR.ptr());
        if !net.is_null() {
            return net;
        }
    }

    // /proc/self/root is the guest's "/": LXRT_ROOT (host "/" without one).
    // pressure-vessel-wrap opendir()s it before anything else.
    if strncmp(rest, cs!("root"), 4) == 0 && (*rest.add(4) == 0 || *rest.add(4) == ch(b'/')) {
        let rootbuf = tls_ptr(&T_ROOTBUF);
        let mut root = getenv(cs!("LXRT_ROOT")).cast_const();
        if root.is_null() || *root == 0 {
            root = cs!("");
        }
        let n = snprintf(
            rootbuf,
            1024,
            cs!("%s%s"),
            root,
            if *rest.add(4) != 0 {
                rest.add(4)
            } else {
                cs!("/")
            },
        );
        if n <= 0 || n as usize >= 1024 {
            return ptr::null();
        }
        return rootbuf;
    }

    if streq(rest, cs!("maps")) {
        regenerate_maps();
    } else if strncmp(rest, cs!("fd"), 2) == 0 && (*rest.add(2) == 0 || *rest.add(2) == ch(b'/')) {
        let mut end: *mut c_char = ptr::null_mut();
        let one = if *rest.add(2) == ch(b'/')
            && *rest.add(3) >= ch(b'0')
            && *rest.add(3) <= ch(b'9')
        {
            strtol(rest.add(3), &mut end, 10)
        } else {
            -1
        };
        if one >= 0 && !end.is_null() && (*end == 0 || *end == ch(b'/')) {
            regenerate_one_fd(one as c_int);
        } else {
            regenerate_fds();
        }
    } else if strncmp(rest, cs!("task"), 4) == 0
        && (*rest.add(4) == 0 || *rest.add(4) == ch(b'/'))
    {
        let mut end: *mut c_char = ptr::null_mut();
        let one = if *rest.add(4) == ch(b'/')
            && *rest.add(5) >= ch(b'1')
            && *rest.add(5) <= ch(b'9')
        {
            strtol(rest.add(5), &mut end, 10)
        } else {
            -1
        };
        if one > 0 && !end.is_null() && (*end == 0 || *end == ch(b'/')) {
            regenerate_one_task(one as c_int);
        } else {
            regenerate_tasks();
        }
    } else if strncmp(rest, cs!("attr/"), 5) == 0
        && strchr(rest.add(5), b'/' as c_int).is_null()
        && *rest.add(5) != 0
    {
        // The LSM's view of this process. There is no SELinux here, but
        // Android daemons ask for their own context first thing (keystore:
        // "SELinux: Could not acquire target context. Aborting keystore.",
        // MEASURED; libselinux getcon reads attr/current). On Waydroid's
        // hosts that answer comes from AppArmor. Here it is the label the
        // binder driver reports for this process too (LXRT_BINDER_SECCTX,
        // runtime/binder.c), NUL-terminated as SELinux writes it; the other
        // attr files (exec, fscreate, sockcreate, keycreate, prev) read
        // empty and take writes.
        let mut ad = [0 as c_char; 600];
        let mut af = [0 as c_char; 700];
        snprintf(ad.as_mut_ptr(), 600, cs!("%s/attr"), G_DIR.ptr());
        mkdir(ad.as_ptr(), 0o700);
        snprintf(
            af.as_mut_ptr(),
            700,
            cs!("%s/%s"),
            ad.as_ptr(),
            rest.add(5),
        );
        if streq(rest.add(5), cs!("current")) || streq(rest.add(5), cs!("prev")) {
            let sc = getenv(cs!("LXRT_BINDER_SECCTX")).cast_const();
            let mut lab = [0 as c_char; 256];
            let n = snprintf(
                lab.as_mut_ptr(),
                256,
                cs!("%s"),
                if !sc.is_null() && *sc != 0 {
                    sc
                } else {
                    cs!("u:r:unlabeled:s0")
                },
            );
            if n > 0 && n < 256 {
                snprintf(ad.as_mut_ptr(), 600, cs!("attr/%s"), rest.add(5));
                write_file(ad.as_ptr(), lab.as_ptr(), n as usize + 1);
            }
        } else if access(af.as_ptr(), D_F_OK) != 0 {
            let fd = open(
                af.as_ptr(),
                D_O_WRONLY | D_O_CREAT | D_O_CLOEXEC,
                0o600 as c_int,
            );
            if fd >= 0 {
                close(fd);
            }
        }
    } else if streq(rest, cs!("environ")) && getenv(cs!("LXRT_NO_ENVIRON")).is_null() {
        let mut ep = [0 as c_char; 700];
        snprintf(ep.as_mut_ptr(), 700, cs!("%s/environ"), G_DIR.ptr());
        lxrt_procpid_environ(getpid(), ep.as_ptr());
    } else if strcmp(rest, cs!("exe")) != 0
        && strcmp(rest, cs!("cmdline")) != 0
        && strcmp(rest, cs!("status")) != 0
    {
        return ptr::null();
    }

    let out = tls_ptr(&T_OUT);
    snprintf(out, 1024, cs!("%s/%s"), G_DIR.ptr(), rest);
    out
}
