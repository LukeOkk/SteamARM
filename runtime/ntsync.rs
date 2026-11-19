// /dev/ntsync -- Linux's NT synchronization driver (drivers/misc/ntsync.c,
// Linux 6.14), emulated across processes.
//
// Wine's server makes every Windows semaphore, mutex and event an ntsync
// object and hands its fd to the processes that use it (SCM_RIGHTS); each
// process then waits on and signals the objects itself, without a round trip
// to the server. Proton Experimental opens /dev/ntsync whenever it exists
// ("ntsync: up and running", PROTON_NO_NTSYNC turns it off), so it is offered
// only when the launcher asks for it: LXRT_NTSYNC=1 (Settings ->
// Sincronización -> NTSYNC). Without it, opening /dev/ntsync fails with ENOENT
// as before and Proton keeps fsync (futex_waitv.rs).
//
// A device is a directory of the user's (/tmp/lxrt-ntsync-UID/d-PID-N):
//
//   device      the arena, mapped MAP_SHARED by every process that has the
//               device or one of its objects: a header with the device lock,
//               a table of objects (type, state, generation) and a table of
//               waiters (the objects each blocked thread waits for, and the
//               word it sleeps on). The device fd is an fd of this file.
//   o<slot>     one file per object, whose fd is the object's fd. Its first
//               bytes name the slot, the generation and the device.
//
// Every fd holds a shared flock on its file: flock locks belong to the open
// file description, which dup, fork and SCM_RIGHTS share, so a file is
// unlocked exactly when no process has an fd of it any more -- the kernel's
// "last fput", without tracking a single close. An object slot whose file
// can be locked exclusively is free again; a device directory whose files
// all can is removed.
//
// One lock per device (a futex-style word in the arena, Darwin shared
// ulocks) serializes every operation; the kernel driver locks per object and
// takes a device lock for wait-all, the semantics are the same. A signal
// hands the object to its waiters in queue order, as the driver's
// try_wake_any_* and try_wake_all do, before the ioctl returns: a semaphore
// released to a waiter reads 0 afterwards. A waiter sleeps on its own word
// in the arena. A lock held by a process that died is taken over after a
// 200 ms wait; a waiter of a process that died is dropped when a signal
// would have woken it.
//
// tests/elf/ntsync.c covers the ioctls across processes; the kernel's own
// selftests (tools/testing/selftests/drivers/ntsync) pass 13/13
// (benchmarks/stage45).
//
// The arena is shared memory that other processes change under the device
// lock, and this code runs on guest syscall paths: everything goes through
// raw pointers and the C library, with no Rust locks, no allocation and no
// panicking paths, exactly as the C original did.

use std::ffi::{c_char, c_int, c_long, c_uint, c_ulong, c_void};
use std::mem::{size_of, MaybeUninit};
use std::ptr;
use std::sync::atomic::{AtomicI32, AtomicU32, Ordering};

// Darwin (arm64) values of <errno.h>, <fcntl.h>, <sys/file.h>, <sys/mman.h>
// and <sys/stat.h>.
const D_EPERM: c_int = 1;
const D_ENOENT: c_int = 2;
const D_ESRCH: c_int = 3;
const D_EINTR: c_int = 4;
const D_EBADF: c_int = 9;
const D_ENOMEM: c_int = 12;
const D_EEXIST: c_int = 17;
const D_EINVAL: c_int = 22;
const D_ENOTTY: c_int = 25;
const D_ETIMEDOUT: c_int = 60;
const D_EOVERFLOW: c_int = 84;
const D_EOWNERDEAD: c_int = 105;

const D_O_RDONLY: c_int = 0x0000;
const D_O_RDWR: c_int = 0x0002;
const D_O_CREAT: c_int = 0x0200;
const D_O_TRUNC: c_int = 0x0400;
const D_O_EXCL: c_int = 0x0800;
const D_O_CLOEXEC: c_int = 0x0100_0000;
const D_F_GETPATH: c_int = 50;
const D_LOCK_SH: c_int = 1;
const D_LOCK_EX: c_int = 2;
const D_LOCK_NB: c_int = 4;
const D_PROT_READ: c_int = 1;
const D_PROT_WRITE: c_int = 2;
const D_MAP_SHARED: c_int = 1;
const D_MAP_FAILED: *mut c_void = usize::MAX as *mut c_void;
const D_S_IFMT: u16 = 0o170000;
const D_S_IFREG: u16 = 0o100000;

#[inline]
fn lerr(error: c_int) -> c_long {
    -(crate::errno_map::map_errno(error) as c_long)
}

const UL_COMPARE_AND_WAIT_SHARED: u32 = 3;
const ULF_WAKE_ALL: u32 = 0x0000_0100;
const ULF_NO_ERRNO: u32 = 0x0100_0000;

// include/uapi/linux/ntsync.h
#[repr(C)]
#[derive(Clone, Copy)]
struct NtSemArgs {
    count: u32,
    max: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct NtMutexArgs {
    owner: u32,
    count: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct NtEventArgs {
    manual: u32,
    signaled: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct NtWaitArgs {
    timeout: u64,
    objs: u64,
    count: u32,
    index: u32,
    flags: u32,
    owner: u32,
    alert: u32,
    pad: u32,
}

const _: [(); 40] = [(); size_of::<NtWaitArgs>()]; // struct ntsync_wait_args

const NT_WAIT_REALTIME: u32 = 0x1;
const NT_MAX_WAIT: usize = 64;

const fn l_ioc(dir: u32, nr: u32, size: u32) -> u32 {
    (dir << 30) | (size << 16) | ((b'N' as u32) << 8) | nr
}
const L_W: u32 = 1;
const L_R: u32 = 2;
const NT_CREATE_SEM: u32 = l_ioc(L_W, 0x80, 8);
const NT_SEM_RELEASE: u32 = l_ioc(L_W | L_R, 0x81, 4);
const NT_WAIT_ANY: u32 = l_ioc(L_W | L_R, 0x82, 40);
const NT_WAIT_ALL: u32 = l_ioc(L_W | L_R, 0x83, 40);
const NT_CREATE_MUTEX: u32 = l_ioc(L_W, 0x84, 8);
const NT_MUTEX_UNLOCK: u32 = l_ioc(L_W | L_R, 0x85, 8);
const NT_MUTEX_KILL: u32 = l_ioc(L_W, 0x86, 4);
const NT_CREATE_EVENT: u32 = l_ioc(L_W, 0x87, 8);
const NT_EVENT_SET: u32 = l_ioc(L_R, 0x88, 4);
const NT_EVENT_RESET: u32 = l_ioc(L_R, 0x89, 4);
const NT_EVENT_PULSE: u32 = l_ioc(L_R, 0x8a, 4);
const NT_SEM_READ: u32 = l_ioc(L_R, 0x8b, 8);
const NT_MUTEX_READ: u32 = l_ioc(L_R, 0x8c, 8);
const NT_EVENT_READ: u32 = l_ioc(L_R, 0x8d, 8);

const T_FREE: u32 = 0;
const T_SEM: u32 = 1;
const T_MUTEX: u32 = 2;
const T_EVENT: u32 = 3;

const NT_MAGIC_DEV: u64 = 0x5645_444e_5953_5444; // "DTSYNDEV"
const NT_MAGIC_OBJ: u64 = 0x4a42_4f4e_5953_5444; // "DTSYNOBJ"
const NT_OBJS: u32 = 65536;
const NT_QS: u32 = 1024;
const NT_ENT: usize = NT_MAX_WAIT + 1; // the objects and the alert

#[repr(C)]
struct NtObj {
    type_: u32,
    gen: u32,
    a: u32, // sem: count, max; mutex: owner, count; event: manual, signaled
    b: u32,
    ownerdead: u32,
    pad: [u32; 3],
}

#[repr(C)]
struct NtQ {
    wake: AtomicU32,     // the word the waiter sleeps on
    signaled: AtomicI32, // -1, or the index of the entry that woke it
    state: u32,          // 0 free, 1 waiting
    all: u32,
    count: u32,
    total: u32,
    owner: u32,
    ownerdead: u32,
    seq: u64, // queue order
    pid: i32,
    pad: u32,
    slot: [u32; NT_ENT],
    gen: [u32; NT_ENT],
}

#[repr(C)]
struct NtHdr {
    magic: u64,
    lock: AtomicU32, // 0 free, 1 held, 2 held and contended
    lock_pid: AtomicI32,
    seq: u64,
    obj_next: u32,
    gc_next: u32,
    q_high: u32,
    pad: u32,
    dir: [c_char; 512],
}

const HDR_BYTES: usize = 4096;
const OBJ_OFF: usize = HDR_BYTES;
const Q_OFF: usize = OBJ_OFF + NT_OBJS as usize * size_of::<NtObj>();
const ARENA_BYTES: usize = Q_OFF + NT_QS as usize * size_of::<NtQ>();

// the first bytes of an object file
#[repr(C)]
struct NtFile {
    magic: u64,
    slot: u32,
    gen: u32,
    dev: [c_char; 512],
}

const _: [(); 32] = [(); size_of::<NtObj>()];
const _: [(); 568] = [(); size_of::<NtQ>()];
const _: [(); 552] = [(); size_of::<NtHdr>()];
const _: [(); 528] = [(); size_of::<NtFile>()];
const _: [(); 2_682_880] = [(); ARENA_BYTES];

#[repr(C)]
#[derive(Clone, Copy)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: i64,
}

// struct stat of macOS arm64 (64-bit inodes).
#[repr(C)]
#[derive(Clone, Copy)]
struct DarwinStat {
    st_dev: i32,
    st_mode: u16,
    st_nlink: u16,
    st_ino: u64,
    st_uid: u32,
    st_gid: u32,
    st_rdev: i32,
    st_atimespec: Timespec,
    st_mtimespec: Timespec,
    st_ctimespec: Timespec,
    st_birthtimespec: Timespec,
    st_size: i64,
    st_blocks: i64,
    st_blksize: i32,
    st_flags: u32,
    st_gen: u32,
    st_lspare: i32,
    st_qspare: [i64; 2],
}

const _: [(); 144] = [(); size_of::<DarwinStat>()];

// struct dirent of macOS arm64 (64-bit inodes).
#[repr(C)]
struct DarwinDirent {
    d_ino: u64,
    d_seekoff: u64,
    d_reclen: u16,
    d_namlen: u16,
    d_type: u8,
    d_name: [c_char; 1024],
}

const _: [(); 1048] = [(); size_of::<DarwinDirent>()];

// pthread_mutex_t of macOS arm64; PTHREAD_MUTEX_INITIALIZER.
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [c_char; 56],
}

const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;

#[repr(C)]
#[derive(Clone, Copy)]
struct Arena {
    dev: i32,
    ino: u64,
    h: *mut NtHdr,
}

const ARENAS: usize = 32;

// This process's view: arenas it mapped, and fds it has identified.
static mut G_ARENAS: [Arena; ARENAS] = [Arena {
    dev: 0,
    ino: 0,
    h: ptr::null_mut(),
}; ARENAS];
static mut G_NARENAS: c_int = 0;
static mut G_LOCAL: PthreadMutex = PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
};

#[link(name = "System")]
unsafe extern "C" {
    fn __error() -> *mut c_int;
    fn __ulock_wait(operation: u32, addr: *mut c_void, value: u64, timeout_us: u32) -> c_int;
    fn __ulock_wake(operation: u32, addr: *mut c_void, wake_value: u64) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn getpid() -> c_int;
    fn getuid() -> c_uint;
    fn kill(pid: c_int, signal: c_int) -> c_int;
    fn snprintf(out: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
    fn strncmp(a: *const c_char, b: *const c_char, n: usize) -> c_int;
    // <stdlib.h> binds realpath to this variant, the one ntsync.c called.
    #[link_name = "realpath$DARWIN_EXTSN"]
    fn realpath(path: *const c_char, resolved: *mut c_char) -> *mut c_char;
    fn mkdir(path: *const c_char, mode: u16) -> c_int;
    fn rmdir(path: *const c_char) -> c_int;
    fn unlink(path: *const c_char) -> c_int;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn flock(fd: c_int, operation: c_int) -> c_int;
    fn fstat(fd: c_int, st: *mut DarwinStat) -> c_int;
    fn ftruncate(fd: c_int, length: i64) -> c_int;
    fn pread(fd: c_int, buf: *mut c_void, count: usize, offset: i64) -> isize;
    fn pwrite(fd: c_int, buf: *const c_void, count: usize, offset: i64) -> isize;
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(address: *mut c_void, length: usize) -> c_int;
    fn opendir(path: *const c_char) -> *mut c_void;
    // dirents.rs declares the same function with its own (identical) struct.
    #[allow(clashing_extern_declarations)]
    fn readdir(dir: *mut c_void) -> *mut DarwinDirent;
    fn closedir(dir: *mut c_void) -> c_int;

    fn lxrt_open_private(path: *const c_char, flags: c_int, mode: c_int) -> c_int; // procfs.c: high, hidden
    fn lxrt_close_private(fd: c_int);
    fn lxrt_guest_clock_ns(clock: c_long) -> u64;
    // lxrt.h's inline lxrt_interrupted_internally() reads the C thread-local
    // lxrt_sig_during_syscall; dispatch.c exports it for Rust.
    fn lxrt_interrupted_internally_ffi() -> bool;
}

#[inline]
unsafe fn errno() -> c_int {
    *__error()
}

#[inline]
unsafe fn local_lock() {
    pthread_mutex_lock(ptr::addr_of_mut!(G_LOCAL));
}

#[inline]
unsafe fn local_unlock() {
    pthread_mutex_unlock(ptr::addr_of_mut!(G_LOCAL));
}

#[inline]
unsafe fn objs(h: *mut NtHdr) -> *mut NtObj {
    h.cast::<u8>().add(OBJ_OFF).cast::<NtObj>()
}

#[inline]
unsafe fn qs(h: *mut NtHdr) -> *mut NtQ {
    h.cast::<u8>().add(Q_OFF).cast::<NtQ>()
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_ntsync_enabled() -> bool {
    static ON: AtomicI32 = AtomicI32::new(-1);
    let mut on = ON.load(Ordering::Relaxed);
    if on < 0 {
        let e = getenv(c"LXRT_NTSYNC".as_ptr());
        on = (!e.is_null() && *e == b'1' as c_char) as c_int;
        ON.store(on, Ordering::Relaxed);
    }
    on != 0
}

// -------------------------------------------------------------- the lock

unsafe fn pid_dead(pid: i32) -> bool {
    pid > 0 && kill(pid, 0) != 0 && errno() == D_ESRCH
}

unsafe fn nt_lock(h: *mut NtHdr) {
    let lock = &(*h).lock;
    let lock_pid = &(*h).lock_pid;
    if lock
        .compare_exchange(0, 1, Ordering::SeqCst, Ordering::SeqCst)
        .is_err()
    {
        while lock.swap(2, Ordering::SeqCst) != 0 {
            let r = __ulock_wait(
                UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO,
                lock.as_ptr().cast(),
                2,
                200_000,
            );
            if r == -D_ETIMEDOUT && pid_dead(lock_pid.load(Ordering::SeqCst)) {
                // Its holder died with it: no one else will release it.
                lock_pid.store(0, Ordering::SeqCst);
                lock.store(0, Ordering::SeqCst);
            }
        }
    }
    lock_pid.store(getpid(), Ordering::SeqCst);
}

unsafe fn nt_unlock(h: *mut NtHdr) {
    let lock = &(*h).lock;
    (*h).lock_pid.store(0, Ordering::SeqCst);
    if lock.swap(0, Ordering::SeqCst) == 2 {
        __ulock_wake(
            UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO,
            lock.as_ptr().cast(),
            0,
        );
    }
}

// -------------------------------------------------------- files and fds

unsafe fn base_dir() -> *const c_char {
    static mut DIR: [c_char; 96] = [0; 96];
    let dir = ptr::addr_of_mut!(DIR).cast::<c_char>();
    if *dir == 0 {
        snprintf(dir, 96, c"/tmp/lxrt-ntsync-%u".as_ptr(), getuid());
        mkdir(dir, 0o700);
    }
    dir
}

// A file nobody holds: an exclusive flock succeeds on a fresh open of it.
unsafe fn file_unheld(path: *const c_char) -> bool {
    let fd = lxrt_open_private(path, D_O_RDONLY | D_O_CLOEXEC, 0);
    if fd < 0 {
        return errno() == D_ENOENT;
    }
    let free = flock(fd, D_LOCK_EX | D_LOCK_NB) == 0;
    lxrt_close_private(fd);
    free
}

unsafe fn obj_path(h: *const NtHdr, slot: u32, out: *mut c_char, n: usize) {
    snprintf(
        out,
        n,
        c"%s/o%u".as_ptr(),
        ptr::addr_of!((*h).dir).cast::<c_char>(),
        slot,
    );
}

unsafe fn map_arena(fd: c_int, st: *const DarwinStat) -> *mut NtHdr {
    local_lock();
    let arenas = ptr::addr_of_mut!(G_ARENAS).cast::<Arena>();
    let narenas = ptr::addr_of_mut!(G_NARENAS);
    let mut h: *mut NtHdr = ptr::null_mut();
    let mut i = 0;
    while i < *narenas && h.is_null() {
        let a = arenas.add(i as usize);
        if (*a).dev == (*st).st_dev && (*a).ino == (*st).st_ino {
            h = (*a).h;
        }
        i += 1;
    }
    if h.is_null() && *narenas < ARENAS as c_int {
        let p = mmap(
            ptr::null_mut(),
            ARENA_BYTES,
            D_PROT_READ | D_PROT_WRITE,
            D_MAP_SHARED,
            fd,
            0,
        );
        if p != D_MAP_FAILED && (*p.cast::<NtHdr>()).magic == NT_MAGIC_DEV {
            h = p.cast();
            arenas.add(*narenas as usize).write(Arena {
                dev: (*st).st_dev,
                ino: (*st).st_ino,
                h,
            });
            *narenas += 1;
        } else if p != D_MAP_FAILED {
            munmap(p, ARENA_BYTES);
        }
    }
    local_unlock();
    h
}

unsafe fn map_arena_path(path: *const c_char) -> *mut NtHdr {
    let fd = lxrt_open_private(path, D_O_RDWR | D_O_CLOEXEC, 0);
    if fd < 0 {
        return ptr::null_mut();
    }
    let mut st = MaybeUninit::<DarwinStat>::uninit();
    let h = if fstat(fd, st.as_mut_ptr()) == 0 {
        map_arena(fd, st.as_ptr())
    } else {
        ptr::null_mut()
    };
    lxrt_close_private(fd);
    h
}

// Files already identified by this process, by device, inode and birth
// time (an inode number comes back for a new file once an old one is
// removed; its birth time does not). Without it every ioctl opened and read
// the files again: 117 us per event round trip, against 15 us for fsync
// (tests/win/run.sh sync_*).
#[repr(C)]
#[derive(Clone, Copy)]
struct Known {
    dev: i32,
    ino: u64,
    born: Timespec,
    h: *mut NtHdr,
    slot: u32,
    gen: u32,
    kind: c_int,
}

const KNOWN: usize = 4096;
static mut G_KNOWN: [Known; KNOWN] = [Known {
    dev: 0,
    ino: 0,
    born: Timespec {
        tv_sec: 0,
        tv_nsec: 0,
    },
    h: ptr::null_mut(),
    slot: 0,
    gen: 0,
    kind: 0,
}; KNOWN];

#[inline]
unsafe fn known_slot(st: *const DarwinStat) -> *mut Known {
    // The top 12 bits: always below KNOWN.
    let index = ((*st).st_ino.wrapping_mul(0x9e37_79b9_7f4a_7c15) >> 52) as usize;
    ptr::addr_of_mut!(G_KNOWN).cast::<Known>().add(index)
}

// What an fd is: 1 a device, 2 an object (*slot, *gen), 0 neither.
unsafe fn identify(fd: c_int, h: *mut *mut NtHdr, slot: *mut u32, gen: *mut u32) -> c_int {
    let mut st = MaybeUninit::<DarwinStat>::uninit();
    if fd < 0 || fstat(fd, st.as_mut_ptr()) != 0 {
        return 0;
    }
    let st = st.as_ptr();
    if (*st).st_mode & D_S_IFMT != D_S_IFREG {
        return 0;
    }
    local_lock();
    let k = known_slot(st);
    if (*k).kind != 0
        && (*k).dev == (*st).st_dev
        && (*k).ino == (*st).st_ino
        && (*k).born.tv_sec == (*st).st_birthtimespec.tv_sec
        && (*k).born.tv_nsec == (*st).st_birthtimespec.tv_nsec
    {
        *h = (*k).h;
        *slot = (*k).slot;
        *gen = (*k).gen;
        let kind = (*k).kind;
        local_unlock();
        return kind;
    }
    local_unlock();
    let kind = identify_slow(fd, st, h, slot, gen);
    if kind != 0 {
        local_lock();
        k.write(Known {
            dev: (*st).st_dev,
            ino: (*st).st_ino,
            born: (*st).st_birthtimespec,
            h: *h,
            slot: if kind == 2 { *slot } else { 0 },
            gen: if kind == 2 { *gen } else { 0 },
            kind,
        });
        local_unlock();
    }
    kind
}

unsafe fn identify_slow(
    fd: c_int,
    stp: *const DarwinStat,
    h: *mut *mut NtHdr,
    slot: *mut u32,
    gen: *mut u32,
) -> c_int {
    let st: DarwinStat = *stp;
    let mut magic: u64 = 0;
    if pread(fd, ptr::addr_of_mut!(magic).cast(), size_of::<u64>(), 0) != size_of::<u64>() as isize
    {
        return 0;
    }
    if magic == NT_MAGIC_DEV {
        *h = map_arena(fd, &st);
        return if (*h).is_null() { 0 } else { 1 };
    }
    if magic != NT_MAGIC_OBJ {
        return 0;
    }
    let mut f = MaybeUninit::<NtFile>::uninit();
    if pread(fd, f.as_mut_ptr().cast(), size_of::<NtFile>(), 0) != size_of::<NtFile>() as isize {
        return 0;
    }
    let f = f.as_mut_ptr();
    let dev = ptr::addr_of_mut!((*f).dev).cast::<c_char>();
    *dev.add(511) = 0;
    *h = map_arena_path(dev);
    if (*h).is_null() || (*f).slot >= NT_OBJS {
        return 0;
    }
    *slot = (*f).slot;
    *gen = (*f).gen;
    2
}

// ------------------------------------------------------------ the device

#[no_mangle]
pub unsafe extern "C" fn lxrt_ntsync_open(lflags: c_int) -> c_long {
    // Directories no process holds any more (their device and objects all
    // closed, or their processes gone) go first.
    let base = base_dir();
    let d = opendir(base);
    if !d.is_null() {
        let mut path = [0 as c_char; 1024];
        let path = path.as_mut_ptr();
        loop {
            let e = readdir(d);
            if e.is_null() {
                break;
            }
            let name = ptr::addr_of!((*e).d_name).cast::<c_char>();
            if strncmp(name, c"d-".as_ptr(), 2) != 0 {
                continue;
            }
            snprintf(path, 1024, c"%s/%s/device".as_ptr(), base, name);
            if !file_unheld(path) {
                continue;
            }
            snprintf(path, 1024, c"%s/%s".as_ptr(), base, name);
            let mut o = opendir(path);
            let mut held = false;
            if !o.is_null() {
                let mut fp = [0 as c_char; 1100];
                let fp = fp.as_mut_ptr();
                while !held {
                    let f = readdir(o);
                    if f.is_null() {
                        break;
                    }
                    let fname = ptr::addr_of!((*f).d_name).cast::<c_char>();
                    if *fname == b'o' as c_char {
                        snprintf(fp, 1100, c"%s/%s".as_ptr(), path as *const c_char, fname);
                        held = !file_unheld(fp);
                    }
                }
                closedir(o);
            }
            if held {
                continue;
            }
            o = opendir(path);
            if !o.is_null() {
                let mut fp = [0 as c_char; 1100];
                let fp = fp.as_mut_ptr();
                loop {
                    let f = readdir(o);
                    if f.is_null() {
                        break;
                    }
                    let fname = ptr::addr_of!((*f).d_name).cast::<c_char>();
                    if *fname != b'.' as c_char {
                        snprintf(fp, 1100, c"%s/%s".as_ptr(), path as *const c_char, fname);
                        unlink(fp);
                    }
                }
                closedir(o);
            }
            rmdir(path);
        }
        closedir(d);
    }

    static N: AtomicU32 = AtomicU32::new(0);
    let mut dir = [0 as c_char; 512];
    let mut dev = [0 as c_char; 600];
    let dir = dir.as_mut_ptr();
    let dev = dev.as_mut_ptr();
    snprintf(
        dir,
        512,
        c"%s/d-%d-%u".as_ptr(),
        base,
        getpid(),
        N.fetch_add(1, Ordering::SeqCst),
    );
    if mkdir(dir, 0o700) != 0 && errno() != D_EEXIST {
        return lerr(errno());
    }
    snprintf(dev, 600, c"%s/device".as_ptr(), dir as *const c_char);
    let cloexec = if lflags & 0o2000000 != 0 {
        D_O_CLOEXEC
    } else {
        0
    };
    let fd = open(dev, D_O_RDWR | D_O_CREAT | D_O_TRUNC | cloexec, 0o600 as c_int);
    if fd < 0 {
        return lerr(errno());
    }
    if ftruncate(fd, ARENA_BYTES as i64) != 0 || flock(fd, D_LOCK_SH) != 0 {
        let e = errno();
        close(fd);
        return lerr(e);
    }
    let mut hdr = MaybeUninit::<NtHdr>::zeroed();
    let hp = hdr.as_mut_ptr();
    (*hp).magic = NT_MAGIC_DEV;
    snprintf(
        ptr::addr_of_mut!((*hp).dir).cast::<c_char>(),
        512,
        c"%s".as_ptr(),
        dir as *const c_char,
    );
    if pwrite(fd, hp.cast(), size_of::<NtHdr>(), 0) != size_of::<NtHdr>() as isize {
        let e = errno();
        close(fd);
        return lerr(e);
    }
    fd as c_long
}

// A slot for a new object, under the device lock: a free one, or one whose
// file nobody holds any more (checked a few at a time as objects are made,
// all of them when none is left).
unsafe fn alloc_slot(h: *mut NtHdr) -> c_int {
    let o = objs(h);
    let mut path = [0 as c_char; 700];
    let path = path.as_mut_ptr();
    for _ in 0..2 {
        // a little collection each time
        let next = (*h).gc_next;
        (*h).gc_next = next.wrapping_add(1);
        let s = next % NT_OBJS;
        let os = o.add(s as usize);
        if (*os).type_ != T_FREE {
            obj_path(h, s, path, 700);
            if file_unheld(path) {
                unlink(path);
                (*os).type_ = T_FREE;
            }
        }
    }
    for i in 0..NT_OBJS {
        let s = (*h).obj_next.wrapping_add(i) % NT_OBJS;
        if (*o.add(s as usize)).type_ == T_FREE {
            (*h).obj_next = s + 1;
            return s as c_int;
        }
    }
    for s in 0..NT_OBJS {
        obj_path(h, s, path, 700);
        if file_unheld(path) {
            unlink(path);
            (*o.add(s as usize)).type_ = T_FREE;
            return s as c_int;
        }
    }
    -1
}

unsafe fn create(h: *mut NtHdr, type_: u32, a: u32, b: u32) -> c_long {
    nt_lock(h);
    let s = alloc_slot(h);
    if s < 0 {
        nt_unlock(h);
        return lerr(D_ENOMEM);
    }
    let mut path = [0 as c_char; 700];
    let path = path.as_mut_ptr();
    obj_path(h, s as u32, path, 700);
    unlink(path);
    let fd = open(
        path,
        D_O_RDWR | D_O_CREAT | D_O_EXCL | D_O_CLOEXEC,
        0o600 as c_int,
    );
    if fd < 0 || flock(fd, D_LOCK_SH) != 0 {
        let e = errno();
        if fd >= 0 {
            close(fd);
        }
        nt_unlock(h);
        return lerr(e);
    }
    let o = objs(h).add(s as usize);
    (*o).gen = (*o).gen.wrapping_add(1);
    let mut f = MaybeUninit::<NtFile>::zeroed();
    let fp = f.as_mut_ptr();
    (*fp).magic = NT_MAGIC_OBJ;
    (*fp).slot = s as u32;
    (*fp).gen = (*o).gen;
    snprintf(
        ptr::addr_of_mut!((*fp).dev).cast::<c_char>(),
        512,
        c"%s/device".as_ptr(),
        ptr::addr_of!((*h).dir).cast::<c_char>(),
    );
    if pwrite(fd, fp.cast(), size_of::<NtFile>(), 0) != size_of::<NtFile>() as isize {
        let e = errno();
        close(fd);
        unlink(path);
        nt_unlock(h);
        return lerr(e);
    }
    (*o).type_ = type_;
    (*o).a = a;
    (*o).b = b;
    (*o).ownerdead = 0;
    nt_unlock(h);
    fd as c_long
}

// --------------------------------------------------- the driver's logic

unsafe fn is_signaled(o: *const NtObj, owner: u32) -> bool {
    match (*o).type_ {
        T_SEM => (*o).a != 0,
        T_MUTEX => ((*o).a == 0 || (*o).a == owner) && (*o).b < u32::MAX,
        T_EVENT => (*o).b != 0,
        _ => false,
    }
}

unsafe fn wake(q: *mut NtQ) {
    let word = &(*q).wake;
    word.fetch_add(1, Ordering::SeqCst);
    __ulock_wake(
        UL_COMPARE_AND_WAIT_SHARED | ULF_WAKE_ALL | ULF_NO_ERRNO,
        word.as_ptr().cast(),
        0,
    );
}

// A queued waiter whose process is gone is dropped instead of handed
// anything.
unsafe fn q_alive(q: *mut NtQ) -> bool {
    if (*q).pid == getpid() || !pid_dead((*q).pid) {
        return true;
    }
    (*q).state = 0;
    false
}

#[inline]
unsafe fn q_slot(q: *const NtQ, i: u32) -> u32 {
    *ptr::addr_of!((*q).slot).cast::<u32>().add(i as usize)
}

#[inline]
unsafe fn q_gen(q: *const NtQ, i: u32) -> u32 {
    *ptr::addr_of!((*q).gen).cast::<u32>().add(i as usize)
}

unsafe fn entry_live(h: *mut NtHdr, q: *const NtQ, i: u32) -> bool {
    let o = objs(h).add(q_slot(q, i) as usize);
    (*o).type_ != T_FREE && (*o).gen == q_gen(q, i)
}

unsafe fn take(o: *mut NtObj, q: *mut NtQ) {
    match (*o).type_ {
        T_SEM => (*o).a = (*o).a.wrapping_sub(1),
        T_MUTEX => {
            if (*o).ownerdead != 0 {
                (*q).ownerdead = 1;
            }
            (*o).ownerdead = 0;
            (*o).b = (*o).b.wrapping_add(1);
            (*o).a = (*q).owner;
        }
        T_EVENT => {
            if (*o).a == 0 {
                (*o).b = 0;
            }
        }
        _ => {}
    }
}

unsafe fn try_wake_all(h: *mut NtHdr, q: *mut NtQ) {
    let o = objs(h);
    for i in 0..(*q).count {
        if !entry_live(h, q, i) || !is_signaled(o.add(q_slot(q, i) as usize), (*q).owner) {
            return;
        }
    }
    if !q_alive(q)
        || (*q)
            .signaled
            .compare_exchange(-1, 0, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
    {
        return;
    }
    for i in 0..(*q).count {
        take(o.add(q_slot(q, i) as usize), q);
    }
    wake(q);
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Cand {
    seq: u64,
    q: u32,
    index: u32,
}

const CANDS: usize = 256;

// The waiters queued on slot s, in queue order: wait-any entries, or
// wait-all waiters' alert entries (any = true); wait-all entries (false).
unsafe fn candidates(h: *mut NtHdr, s: u32, any: bool, out: *mut Cand, max: c_int) -> c_int {
    let qv = qs(h);
    let o = objs(h).add(s as usize);
    let mut n: c_int = 0;
    let mut k: u32 = 0;
    while k < (*h).q_high && k < NT_QS {
        let q = qv.add(k as usize);
        let index = k;
        k += 1;
        if (*q).state != 1 || (*q).signaled.load(Ordering::SeqCst) != -1 {
            continue;
        }
        let mut from = 0;
        let mut to = (*q).total;
        if (*q).all != 0 {
            if any {
                from = (*q).count; // the alert only
            } else {
                to = (*q).count;
            }
        } else if !any {
            continue;
        }
        let mut i = from;
        while i < to && n < max {
            let entry = i;
            i += 1;
            if q_slot(q, entry) != s || q_gen(q, entry) != (*o).gen {
                continue;
            }
            let mut j = n as usize;
            n += 1;
            while j > 0 {
                let prev = out.add(j - 1);
                if !((*prev).seq > (*q).seq || ((*prev).seq == (*q).seq && (*prev).index > entry)) {
                    break;
                }
                *out.add(j) = *prev;
                j -= 1;
            }
            out.add(j).write(Cand {
                seq: (*q).seq,
                q: index,
                index: entry,
            });
        }
    }
    n
}

unsafe fn try_wake_all_obj(h: *mut NtHdr, s: u32) {
    let mut c = MaybeUninit::<[Cand; CANDS]>::uninit();
    let c = c.as_mut_ptr().cast::<Cand>();
    let n = candidates(h, s, false, c, CANDS as c_int);
    for i in 0..n as usize {
        try_wake_all(h, qs(h).add((*c.add(i)).q as usize));
    }
}

unsafe fn try_wake_any_obj(h: *mut NtHdr, s: u32) {
    let o = objs(h).add(s as usize);
    let mut c = MaybeUninit::<[Cand; CANDS]>::uninit();
    let c = c.as_mut_ptr().cast::<Cand>();
    let n = candidates(h, s, true, c, CANDS as c_int);
    for i in 0..n as usize {
        let cand = c.add(i);
        let q = qs(h).add((*cand).q as usize);
        if (*o).type_ == T_SEM && (*o).a == 0 {
            break;
        }
        if (*o).type_ == T_EVENT && (*o).b == 0 {
            break;
        }
        if (*o).type_ == T_MUTEX {
            if (*o).b == u32::MAX {
                break;
            }
            if (*o).a != 0 && (*o).a != (*q).owner {
                continue;
            }
        }
        if !q_alive(q)
            || (*q)
                .signaled
                .compare_exchange(
                    -1,
                    (*cand).index as i32,
                    Ordering::SeqCst,
                    Ordering::SeqCst,
                )
                .is_err()
        {
            continue;
        }
        take(o, q);
        wake(q);
    }
}

unsafe fn signal_obj(h: *mut NtHdr, s: u32) {
    try_wake_all_obj(h, s);
    try_wake_any_obj(h, s);
}

// ------------------------------------------------------------ the ioctls

unsafe fn gptr(mut a: u64) -> *mut c_void {
    let base = crate::gbase::lxrt_gbase();
    if base != 0 && a != 0 && a < (1u64 << 32) {
        // FEX's low window (gbase.rs)
        a = a.wrapping_add(base);
    }
    a as usize as *mut c_void
}

unsafe fn obj_ioctl(h: *mut NtHdr, s: u32, gen: u32, req: u32, p: *mut c_void) -> c_long {
    nt_lock(h);
    let o = objs(h).add(s as usize);
    let mut ret: c_long = 0;
    if (*o).type_ == T_FREE || (*o).gen != gen {
        nt_unlock(h);
        return lerr(D_EBADF);
    }
    let p32 = p.cast::<u32>();
    match req {
        NT_SEM_RELEASE => 'case: {
            let count = p32.read_unaligned();
            let prev = (*o).a;
            if (*o).type_ != T_SEM {
                ret = lerr(D_EINVAL);
                break 'case;
            }
            let sum = match (*o).a.checked_add(count) {
                Some(sum) if sum <= (*o).b => sum,
                _ => {
                    ret = lerr(D_EOVERFLOW);
                    break 'case;
                }
            };
            (*o).a = sum;
            signal_obj(h, s);
            p32.write_unaligned(prev);
        }
        NT_MUTEX_UNLOCK => 'case: {
            // m->owner, m->count
            if p32.read_unaligned() == 0 || (*o).type_ != T_MUTEX {
                ret = lerr(D_EINVAL);
                break 'case;
            }
            let prev = (*o).b;
            if (*o).a != p32.read_unaligned() {
                ret = lerr(D_EPERM);
                break 'case;
            }
            (*o).b = (*o).b.wrapping_sub(1);
            if (*o).b == 0 {
                (*o).a = 0;
            }
            signal_obj(h, s);
            p32.add(1).write_unaligned(prev);
        }
        NT_MUTEX_KILL => 'case: {
            let owner = p32.read_unaligned();
            if owner == 0 || (*o).type_ != T_MUTEX {
                ret = lerr(D_EINVAL);
                break 'case;
            }
            if (*o).a != owner {
                ret = lerr(D_EPERM);
                break 'case;
            }
            (*o).ownerdead = 1;
            (*o).a = 0;
            (*o).b = 0;
            signal_obj(h, s);
        }
        NT_EVENT_SET | NT_EVENT_PULSE => 'case: {
            if (*o).type_ != T_EVENT {
                ret = lerr(D_EINVAL);
                break 'case;
            }
            let prev = (*o).b;
            (*o).b = 1;
            signal_obj(h, s);
            if req == NT_EVENT_PULSE {
                (*o).b = 0;
            }
            p32.write_unaligned(prev);
        }
        NT_EVENT_RESET => 'case: {
            if (*o).type_ != T_EVENT {
                ret = lerr(D_EINVAL);
                break 'case;
            }
            let prev = (*o).b;
            (*o).b = 0;
            p32.write_unaligned(prev);
        }
        NT_SEM_READ => 'case: {
            if (*o).type_ != T_SEM {
                ret = lerr(D_EINVAL);
                break 'case;
            }
            p.cast::<NtSemArgs>().write_unaligned(NtSemArgs {
                count: (*o).a,
                max: (*o).b,
            });
        }
        NT_MUTEX_READ => 'case: {
            if (*o).type_ != T_MUTEX {
                ret = lerr(D_EINVAL);
                break 'case;
            }
            p.cast::<NtMutexArgs>().write_unaligned(NtMutexArgs {
                owner: (*o).a,
                count: (*o).b,
            });
            if (*o).ownerdead != 0 {
                ret = lerr(D_EOWNERDEAD);
            }
        }
        NT_EVENT_READ => 'case: {
            if (*o).type_ != T_EVENT {
                ret = lerr(D_EINVAL);
                break 'case;
            }
            p.cast::<NtEventArgs>().write_unaligned(NtEventArgs {
                manual: (*o).a,
                signaled: (*o).b,
            });
        }
        _ => ret = lerr(D_ENOTTY),
    }
    nt_unlock(h);
    ret
}

unsafe fn nt_wait(h: *mut NtHdr, all: bool, uargs: *mut NtWaitArgs) -> c_long {
    let args: NtWaitArgs = uargs.read_unaligned();
    if args.pad != 0 || (args.flags & !NT_WAIT_REALTIME) != 0 {
        return lerr(D_EINVAL);
    }
    if args.owner == 0 {
        return lerr(D_EINVAL);
    }
    if args.count > NT_MAX_WAIT as u32 {
        return lerr(D_EINVAL);
    }
    let total = args.count + if args.alert != 0 { 1 } else { 0 };
    let mut fds = MaybeUninit::<[i32; NT_ENT]>::uninit();
    let fds = fds.as_mut_ptr().cast::<i32>();
    if args.count != 0 {
        ptr::copy_nonoverlapping(
            gptr(args.objs).cast::<u8>(),
            fds.cast::<u8>(),
            args.count as usize * size_of::<i32>(),
        );
    }
    if args.alert != 0 {
        *fds.add(args.count as usize) = args.alert as i32;
    }
    let mut slot = MaybeUninit::<[u32; NT_ENT]>::uninit();
    let mut gen = MaybeUninit::<[u32; NT_ENT]>::uninit();
    let slot = slot.as_mut_ptr().cast::<u32>();
    let gen = gen.as_mut_ptr().cast::<u32>();
    for i in 0..total as usize {
        let mut oh: *mut NtHdr = ptr::null_mut();
        if identify(*fds.add(i), &mut oh, slot.add(i), gen.add(i)) != 2 || oh != h {
            return lerr(D_EINVAL);
        }
        if all && i < args.count as usize {
            for j in 0..i {
                if *slot.add(j) == *slot.add(i) {
                    return lerr(D_EINVAL);
                }
            }
        }
    }

    nt_lock(h);
    let qv = qs(h);
    let mut q: *mut NtQ = ptr::null_mut();
    let mut k: u32 = 0;
    while k < NT_QS {
        let qk = qv.add(k as usize);
        if (*qk).state == 1 && k < (*h).q_high && pid_dead((*qk).pid) {
            (*qk).state = 0; // a waiter whose process died
        }
        if (*qk).state == 0 {
            q = qk;
            break;
        }
        k += 1;
    }
    if q.is_null() {
        nt_unlock(h);
        return lerr(D_ENOMEM);
    }
    if k + 1 > (*h).q_high {
        (*h).q_high = k + 1;
    }
    for i in 0..total as usize {
        let o = objs(h).add(*slot.add(i) as usize);
        if (*o).type_ == T_FREE || (*o).gen != *gen.add(i) {
            nt_unlock(h);
            return lerr(D_EINVAL);
        }
    }
    let signaled = &(*q).signaled;
    (*q).state = 1;
    signaled.store(-1, Ordering::SeqCst);
    (*q).all = all as u32;
    (*q).count = args.count;
    (*q).total = total;
    (*q).owner = args.owner;
    (*q).ownerdead = 0;
    (*h).seq = (*h).seq.wrapping_add(1);
    (*q).seq = (*h).seq;
    (*q).pid = getpid();
    ptr::copy_nonoverlapping(
        slot,
        ptr::addr_of_mut!((*q).slot).cast::<u32>(),
        total as usize,
    );
    ptr::copy_nonoverlapping(
        gen,
        ptr::addr_of_mut!((*q).gen).cast::<u32>(),
        total as usize,
    );
    if all {
        try_wake_all(h, q);
        if args.alert != 0 && signaled.load(Ordering::SeqCst) == -1 {
            try_wake_any_obj(h, *slot.add(args.count as usize));
        }
    } else {
        let mut i = 0;
        while i < total && signaled.load(Ordering::SeqCst) == -1 {
            try_wake_any_obj(h, *slot.add(i as usize));
            i += 1;
        }
    }
    nt_unlock(h);

    let mut intr = false;
    let clk: c_long = if args.flags & NT_WAIT_REALTIME != 0 {
        0
    } else {
        1
    };
    loop {
        let w = (*q).wake.load(Ordering::SeqCst);
        if signaled.load(Ordering::SeqCst) != -1 {
            break;
        }
        let mut us: u32 = 0;
        if args.timeout != u64::MAX {
            let now = lxrt_guest_clock_ns(clk);
            if now >= args.timeout {
                break; // timed out
            }
            let u = (args.timeout - now) / 1000;
            us = if u == 0 {
                1
            } else if u >= u32::MAX as u64 {
                u32::MAX - 1
            } else {
                u as u32
            };
        }
        let r = __ulock_wait(
            UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO,
            (*q).wake.as_ptr().cast(),
            w as u64,
            us,
        );
        if r == -D_EINTR && !lxrt_interrupted_internally_ffi() {
            intr = true;
            break;
        }
    }

    nt_lock(h);
    let sig = signaled.load(Ordering::SeqCst);
    let dead = (*q).ownerdead != 0;
    (*q).state = 0;
    // q_high bounds signal_obj's scans. Retired tail slots must not keep
    // every later signal scanning the peak number of concurrent waiters
    // (a signaled waiter stays occupied until it reaches this cleanup).
    while (*h).q_high != 0 && (*qv.add((*h).q_high as usize - 1)).state == 0 {
        (*h).q_high -= 1;
    }
    nt_unlock(h);
    if sig != -1 {
        // Even if a signal or the timeout came too, the objects were taken.
        ptr::addr_of_mut!((*uargs).index).write_unaligned(sig as u32);
        return if dead { lerr(D_EOWNERDEAD) } else { 0 };
    }
    if intr {
        lerr(D_EINTR)
    } else {
        lerr(D_ETIMEDOUT)
    }
}

#[no_mangle]
pub extern "C" fn lxrt_ntsync_request(lreq: c_ulong) -> bool {
    ((lreq >> 8) & 0xff) == b'N' as c_ulong && (lreq & 0xff) >= 0x80 && (lreq & 0xff) <= 0x8d
}

// *ret set and true when fd is a device or an object of this emulation.
#[no_mangle]
pub unsafe extern "C" fn lxrt_ntsync_ioctl(
    fd: c_int,
    lreq: c_ulong,
    arg: u64,
    ret: *mut c_long,
) -> bool {
    let mut h: *mut NtHdr = ptr::null_mut();
    let mut slot: u32 = 0;
    let mut gen: u32 = 0;
    let kind = identify(fd, &mut h, &mut slot, &mut gen);
    if kind == 0 {
        return false;
    }
    let p = gptr(arg);
    let req = lreq as u32;
    if kind == 1 {
        match req {
            NT_CREATE_SEM => {
                let a = p.cast::<NtSemArgs>().read_unaligned();
                *ret = if a.count > a.max {
                    lerr(D_EINVAL)
                } else {
                    create(h, T_SEM, a.count, a.max)
                };
            }
            NT_CREATE_MUTEX => {
                let a = p.cast::<NtMutexArgs>().read_unaligned();
                *ret = if (a.owner == 0) != (a.count == 0) {
                    lerr(D_EINVAL)
                } else {
                    create(h, T_MUTEX, a.owner, a.count)
                };
            }
            NT_CREATE_EVENT => {
                let a = p.cast::<NtEventArgs>().read_unaligned();
                *ret = create(h, T_EVENT, a.manual, a.signaled);
            }
            NT_WAIT_ANY | NT_WAIT_ALL => {
                *ret = nt_wait(h, req == NT_WAIT_ALL, p.cast());
            }
            _ => *ret = lerr(D_ENOTTY),
        }
        return true;
    }
    *ret = obj_ioctl(h, slot, gen, req, p);
    true
}

// open("/dev/ntsync"), or "dev/ntsync" against a directory fd of the guest's
// root (FEX opens everything relative to its rootfs fd).
#[no_mangle]
pub unsafe extern "C" fn lxrt_ntsync_path(dirfd: c_int, path: *const c_char) -> bool {
    if path.is_null() || !lxrt_ntsync_enabled() {
        return false;
    }
    if strcmp(path, c"/dev/ntsync".as_ptr()) == 0 {
        return true;
    }
    if strcmp(path, c"dev/ntsync".as_ptr()) != 0 || dirfd < 0 {
        return false;
    }
    let mut dp = [0 as c_char; 1024];
    let mut rp = [0 as c_char; 1024];
    let root = getenv(c"LXRT_ROOT".as_ptr());
    if fcntl(dirfd, D_F_GETPATH, dp.as_mut_ptr()) != 0 {
        return false;
    }
    if root.is_null() || *root == 0 {
        return strcmp(dp.as_ptr(), c"/".as_ptr()) == 0;
    }
    !realpath(root, rp.as_mut_ptr()).is_null() && strcmp(dp.as_ptr(), rp.as_ptr()) == 0
}
