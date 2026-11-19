// memfd_create, execveat and rt_sigtimedwait.
//
// These are the three calls FEX makes for itself rather than on the guest's
// behalf, so all three fail before a single x86 instruction is translated.
// None of them has a Darwin equivalent to forward to, and each fails in a
// different way:
//
//   * memfd_create asks for a file with no name. Darwin's nearest object,
//     POSIX shared memory, is not a file -- measured below.
//   * execveat asks the kernel to execute an open fd. Darwin has no execveat,
//     no fexecve, and -- measured below -- no /proc/self/fd/N equivalent that
//     can be walked through.
//   * rt_sigtimedwait asks to dequeue a pending signal with a deadline. Darwin
//     has sigwait, which has no deadline, and -- measured below -- a kqueue
//     signal filter that cannot see the only kind of signal this runtime ever
//     sends.
//
// Every number in here that could plausibly have been passed through has been
// checked against both systems, because the bugs this project keeps hitting
// are the ones where the low range agrees.
//
// This is a line-for-line port of the C original. It deliberately uses the raw
// pthread mutexes, condition variable and once controls the C used (not std
// locks): the tables are touched from guest syscall paths and from the fork
// hooks, the timer needs pthread_cond_timedwait_relative_np, and the wait
// cleanup is a Darwin pthread cleanup handler record on the waiter's stack.
// Nothing in here allocates, and nothing can panic or unwind.

#![allow(static_mut_refs)]

use std::ffi::{c_char, c_int, c_long, c_uint, c_ulong, c_void};
use std::mem::size_of;
use std::ptr;

// ------------------------------------------------------------ Darwin ABI
//
// Written out rather than queried: these are the arm64 macOS SDK layouts and
// values the C original compiled against.

const EPERM: c_int = 1;
const ENOENT: c_int = 2;
const ESRCH: c_int = 3;
const EINTR: c_int = 4;
const EBADF: c_int = 9;
const ENOMEM: c_int = 12;
const EACCES: c_int = 13;
const EFAULT: c_int = 14;
const EINVAL: c_int = 22;
const EMFILE: c_int = 24;
const EAGAIN: c_int = 35;
const ELOOP: c_int = 62;
const ENAMETOOLONG: c_int = 63;
const ENOSYS: c_int = 78;

const PATH_MAX: usize = 1024;
const O_RDONLY: c_int = 0;
const F_GETFD: c_int = 1;
const F_SETFD: c_int = 2;
const F_GETPATH: c_int = 50;
const FD_CLOEXEC: c_int = 1;
const SEEK_CUR: c_int = 1;
const PROT_WRITE: c_int = 0x02;
const MAP_SHARED: c_int = 0x0001;

const S_IFMT: u16 = 0o170000;
const S_IFREG: u16 = 0o100000;
const S_IFLNK: u16 = 0o120000;

const VM_PROT_READ: c_int = 0x01;
const VM_PROT_WRITE: c_int = 0x02;
const VM_REGION_BASIC_INFO_64: c_int = 9;
// sizeof(vm_region_basic_info_data_64_t) / sizeof(int): the struct is 36 bytes
// under its #pragma pack(4).
const VM_REGION_BASIC_INFO_COUNT_64: u32 = 9;
const KERN_SUCCESS: c_int = 0;
const MACH_PORT_NULL: u32 = 0;

const NSIG: usize = 32;
const SIGILL: c_int = 4;
const SIGTRAP: c_int = 5;
const SIGEMT: c_int = 7;
const SIGFPE: c_int = 8;
const SIGKILL: c_int = 9;
const SIGBUS: c_int = 10;
const SIGSEGV: c_int = 11;
const SIGSYS: c_int = 12;
const SIGURG: c_int = 16;
const SIGSTOP: c_int = 17;
const SIGCHLD: c_int = 20;
const SIGIO: c_int = 23;
const SIGWINCH: c_int = 28;
const SIGINFO: c_int = 29;
const SIG_DFL: usize = 0;
const SIG_IGN: usize = 1;
const SIG_BLOCK: c_int = 1;
const SIG_SETMASK: c_int = 3;
const SA_RESTART: c_int = 0x0002;

const LXRT_HOST_PAGE: u64 = 16384;

// Darwin's sigset_t is a 32-bit word and the sigset operations are the
// <signal.h> inline forms the C original compiled to: bit n-1 is signal n, and
// a number above 32 selects nothing.
type SigSet = u32;

#[inline(always)]
fn sigbits(signo: c_int) -> u32 {
    if signo > 32 {
        0
    } else {
        1u32.wrapping_shl((signo - 1) as u32)
    }
}

#[inline(always)]
fn sigismember(set: SigSet, signo: c_int) -> bool {
    set & sigbits(signo) != 0
}

#[inline(always)]
fn sigaddset(set: &mut SigSet, signo: c_int) {
    *set |= sigbits(signo);
}

#[inline(always)]
fn sigdelset(set: &mut SigSet, signo: c_int) {
    *set &= !sigbits(signo);
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SigAction {
    sa_handler: usize, // union __sigaction_u
    sa_mask: SigSet,
    sa_flags: c_int,
}

const _: [(); 16] = [(); size_of::<SigAction>()];

impl SigAction {
    const ZERO: Self = Self {
        sa_handler: 0,
        sa_mask: 0,
        sa_flags: 0,
    };
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Timespec {
    tv_sec: i64,  // __darwin_time_t
    tv_nsec: i64, // long
}

// struct stat, arm64 (always the 64-bit-inode layout there).
#[repr(C)]
struct Stat {
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

const _: [(); 144] = [(); size_of::<Stat>()];

impl Stat {
    const fn zeroed() -> Self {
        const Z: Timespec = Timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        Self {
            st_dev: 0,
            st_mode: 0,
            st_nlink: 0,
            st_ino: 0,
            st_uid: 0,
            st_gid: 0,
            st_rdev: 0,
            st_atimespec: Z,
            st_mtimespec: Z,
            st_ctimespec: Z,
            st_birthtimespec: Z,
            st_size: 0,
            st_blocks: 0,
            st_blksize: 0,
            st_flags: 0,
            st_gen: 0,
            st_lspare: 0,
            st_qspare: [0; 2],
        }
    }
}

#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}

// PTHREAD_MUTEX_INITIALIZER
const PTHREAD_MUTEX_INITIALIZER: PthreadMutex = PthreadMutex {
    sig: 0x32AA_ABA7,
    opaque: [0; 56],
};

#[repr(C)]
struct PthreadCond {
    sig: c_long,
    opaque: [u8; 40],
}

// PTHREAD_COND_INITIALIZER
const PTHREAD_COND_INITIALIZER: PthreadCond = PthreadCond {
    sig: 0x3CB0_B1BB,
    opaque: [0; 40],
};

#[repr(C)]
struct PthreadOnce {
    sig: c_long,
    opaque: [u8; 8],
}

// PTHREAD_ONCE_INIT
const PTHREAD_ONCE_INIT: PthreadOnce = PthreadOnce {
    sig: 0x30B1_BCBA,
    opaque: [0; 8],
};

// The public head of struct _opaque_pthread_t, and the handler record that
// pthread_cleanup_push/pop (macros in <pthread.h>) link through it.
#[repr(C)]
struct DarwinPthreadHandlerRec {
    routine: unsafe extern "C" fn(*mut c_void),
    arg: *mut c_void,
    next: *mut DarwinPthreadHandlerRec,
}

#[repr(C)]
struct OpaquePthread {
    sig: c_long,
    cleanup_stack: *mut DarwinPthreadHandlerRec,
    // char __opaque[__PTHREAD_SIZE__] follows; never touched here.
}

type PthreadT = *mut OpaquePthread;

#[repr(C)]
struct MachTimebaseInfo {
    numer: u32,
    denom: u32,
}

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: u32;

    fn __error() -> *mut c_int;
    fn close(fd: c_int) -> c_int;
    fn fcntl(fd: c_int, cmd: c_int, ...) -> c_int;
    fn fgetxattr(
        fd: c_int,
        name: *const c_char,
        value: *mut c_void,
        size: usize,
        position: u32,
        options: c_int,
    ) -> isize;
    fn fsetxattr(
        fd: c_int,
        name: *const c_char,
        value: *const c_void,
        size: usize,
        position: u32,
        options: c_int,
    ) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn snprintf(buffer: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn fstat(fd: c_int, st: *mut Stat) -> c_int;
    fn stat(path: *const c_char, st: *mut Stat) -> c_int;
    fn lstat(path: *const c_char, st: *mut Stat) -> c_int;
    fn getpid() -> c_int;
    fn getuid() -> c_uint;
    fn lseek(fd: c_int, offset: i64, whence: c_int) -> i64;
    fn mkstemp(template: *mut c_char) -> c_int;
    fn unlink(path: *const c_char) -> c_int;
    fn openat(fd: c_int, path: *const c_char, flags: c_int, ...) -> c_int;
    fn pause() -> c_int;
    fn nanosleep(request: *const Timespec, remaining: *mut Timespec) -> c_int;
    fn strlen(s: *const c_char) -> usize;
    fn strncmp(a: *const c_char, b: *const c_char, n: usize) -> c_int;
    fn strrchr(s: *const c_char, c: c_int) -> *mut c_char;

    fn mach_absolute_time() -> u64;
    fn mach_timebase_info(info: *mut MachTimebaseInfo) -> c_int;
    fn mach_vm_region(
        task: u32,
        address: *mut u64,
        size: *mut u64,
        flavor: c_int,
        info: *mut c_int,
        count: *mut u32,
        object: *mut u32,
    ) -> c_int;

    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_cond_signal(cond: *mut PthreadCond) -> c_int;
    fn pthread_cond_wait(cond: *mut PthreadCond, mutex: *mut PthreadMutex) -> c_int;
    fn pthread_cond_timedwait_relative_np(
        cond: *mut PthreadCond,
        mutex: *mut PthreadMutex,
        relative: *const Timespec,
    ) -> c_int;
    fn pthread_once(once: *mut PthreadOnce, init: extern "C" fn()) -> c_int;
    fn pthread_create(
        thread: *mut PthreadT,
        attr: *const c_void,
        start: extern "C" fn(*mut c_void) -> *mut c_void,
        arg: *mut c_void,
    ) -> c_int;
    fn pthread_detach(thread: PthreadT) -> c_int;
    fn pthread_kill(thread: PthreadT, signal: c_int) -> c_int;
    fn pthread_self() -> PthreadT;
    fn pthread_setname_np(name: *const c_char) -> c_int;
    fn pthread_sigmask(how: c_int, set: *const SigSet, oldset: *mut SigSet) -> c_int;
    fn sigaction(signal: c_int, action: *const SigAction, old: *mut SigAction) -> c_int;
    fn sigwait(set: *const SigSet, signal: *mut c_int) -> c_int;

    // The rest of the runtime.
    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void; // FILE *
    fn lxrt_host_tmpdir() -> *const c_char;
    fn lxrt_execve(
        path: *const c_char,
        argv: *const *mut c_char,
        envp: *const *mut c_char,
    ) -> c_long;
    fn lxrt_gettid() -> c_int;
    fn lxrt_rt_enqueue(tid: c_int, lsig: c_int) -> bool;
    fn lxrt_rt_dequeue_self() -> c_int;
    fn lxrt_signo_to_darwin(linux_sig: c_int) -> c_int;
    fn lxrt_signo_to_linux(darwin_sig: c_int) -> c_int;
}

#[inline(always)]
unsafe fn errno() -> c_int {
    *__error()
}

// LERR(e)
#[inline(always)]
unsafe fn lerr(e: c_int) -> c_long {
    -(lxrt_errno_to_linux(e) as c_long)
}

// NUL-terminated C string literal as a pointer.
macro_rules! cstr {
    ($s:literal) => {
        concat!($s, "\0").as_ptr().cast::<c_char>()
    };
}

// LXRT_FORK_SAFE(name, mutex): each module registers its mutex: taken before
// fork, released after it in both parent and child (the child's only thread is
// the one that took them).
macro_rules! fork_safe {
    ($prepare:ident, $release:ident, $mutex:ident) => {
        extern "C" fn $prepare() {
            unsafe {
                pthread_mutex_lock(ptr::addr_of_mut!($mutex));
            }
        }
        extern "C" fn $release() {
            unsafe {
                pthread_mutex_unlock(ptr::addr_of_mut!($mutex));
            }
        }
    };
}

// ------------------------------------------------------------ guest memory
//
// A guest pointer is a number the guest chose. Dereferencing one that is not
// mapped kills the host process, which from the guest's point of view is the
// runtime vanishing rather than a syscall returning -EFAULT. The Mach region
// map is the only way to ask "is this readable" without trying it.

unsafe fn mem_ok(addr: u64, len: usize, need: c_int) -> bool {
    if addr == 0 || len == 0 {
        return false;
    }
    let end = addr.wrapping_add(len as u64);
    if end < addr {
        return false; // wrapped; no mapping can satisfy it
    }

    let mut at = addr;
    while at < end {
        let mut ra: u64 = at;
        let mut rs: u64 = 0;
        // vm_region_basic_info_data_64_t; `protection` is its first member.
        let mut info = [0 as c_int; VM_REGION_BASIC_INFO_COUNT_64 as usize];
        let mut cnt: u32 = VM_REGION_BASIC_INFO_COUNT_64;
        let mut obj: u32 = MACH_PORT_NULL;
        if mach_vm_region(
            mach_task_self_,
            &mut ra,
            &mut rs,
            VM_REGION_BASIC_INFO_64,
            info.as_mut_ptr(),
            &mut cnt,
            &mut obj,
        ) != KERN_SUCCESS
        {
            return false;
        }
        // mach_vm_region reports the first region at or AFTER the address, so a
        // region starting past `at` means `at` itself is in a hole.
        if ra > at || rs == 0 {
            return false;
        }
        if (info[0] & need) != need {
            return false;
        }
        at = ra.wrapping_add(rs);
    }
    true
}

// strnlen over guest memory. A plain strnlen walks off the end of a valid
// mapping into an unmapped one, so readability is rechecked at every host page
// boundary crossed. Returns false on fault; *out_len == max means "no NUL
// within max bytes", which every caller treats as too long rather than as a
// string.
unsafe fn guest_strnlen(p: *const c_char, max: usize, out_len: &mut usize) -> bool {
    if p.is_null() {
        return false;
    }
    let a = p as usize as u64;
    let mut checked: u64 = 0;
    let mut n: usize = 0;
    while n < max {
        let page = a.wrapping_add(n as u64) & !(LXRT_HOST_PAGE - 1);
        if page != checked || n == 0 {
            if !mem_ok(page, 1, VM_PROT_READ) {
                return false;
            }
            checked = page;
        }
        if *p.add(n) == 0 {
            *out_len = n;
            return true;
        }
        n += 1;
    }
    *out_len = max;
    true
}

// The timebase is published once, under pthread_once. The lazy form it replaces
// wrote tb.numer and tb.denom as two unsynchronised stores behind an
// `if (tb.denom == 0)` test, so a second thread arriving in that window could
// read denom set and numer still 0 and get 0 ns back -- which makes every
// deadline computed from it look already expired.
static mut G_TB: MachTimebaseInfo = MachTimebaseInfo { numer: 0, denom: 0 };
static mut G_TB_ONCE: PthreadOnce = PTHREAD_ONCE_INIT;

extern "C" fn tb_init() {
    unsafe {
        mach_timebase_info(ptr::addr_of_mut!(G_TB));
    }
}

unsafe fn mono_ns() -> u64 {
    pthread_once(ptr::addr_of_mut!(G_TB_ONCE), tb_init);
    let t = mach_absolute_time();
    // numer/denom is 125/3 on this machine, so the plain t * numer overflows 64
    // bits at roughly 195 years of uptime. Not a live concern, but the split
    // multiply is exact and costs nothing; verified to agree with the plain
    // form bit for bit on the current timebase.
    let numer = (*ptr::addr_of!(G_TB)).numer as u64;
    let denom = (*ptr::addr_of!(G_TB)).denom as u64;
    // The C divides unguarded; a zero denom (mach_timebase_info failing, which
    // it does not) traps there. checked_* keeps that from becoming a Rust
    // panic path while leaving every real timebase bit-identical.
    let whole = t.checked_div(denom).unwrap_or(0);
    let part = t.checked_rem(denom).unwrap_or(0);
    whole
        .wrapping_mul(numer)
        .wrapping_add(part.wrapping_mul(numer).checked_div(denom).unwrap_or(0))
}

// Linux aarch64 timespec is two signed 64-bit fields. Darwin's struct timespec
// is time_t + long, which on arm64 is also 8 + 8, but the types are written out
// rather than cast so a future 32-bit target does not silently reinterpret it.
#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxTimespec {
    tv_sec: i64,
    tv_nsec: i64,
}

// ============================================================================
// 279  memfd_create(const char *name, unsigned int flags)
// ============================================================================
//
// Linux hands back a file descriptor for an anonymous, resizable, mappable
// file living in tmpfs. Darwin has nothing that is all four things.
//
// shm_open + immediate shm_unlink was the obvious candidate and it does leave a
// usable descriptor, but it is not a file. Measured on macOS 15 / arm64:
//
//   ftruncate(fd, 4096)   -> 0        first call only
//   ftruncate(fd, 8192)   -> EINVAL   a POSIX shm object is sized exactly once
//   ftruncate(fd, 4096)   -> EINVAL
//   fstat  st_size        -> 16384    rounded up to the host page, not 4096
//   pread / pwrite        -> ESPIPE   it is not seekable, so it is not readable
//   fcntl(F_GETPATH)      -> EBADF
//
// Three of those are fatal for a memfd. Callers grow a memfd repeatedly (that
// is the point of F_SEAL_GROW existing), they stat it to learn its exact size,
// and they write() into it -- FEX writes its thunk images that way. So the
// fallback the brief allowed is the implementation: mkstemp in the runtime's
// temp directory, unlinked before anything else touches it. Measured on the
// same machine, all of ftruncate grow, ftruncate shrink, exact fstat size,
// pread/pwrite, mmap MAP_SHARED and reopen through /dev/fd/N work.
//
// What is lost against Linux: the file is backed by the real filesystem rather
// than by tmpfs, so a large memfd is written to disk instead of staying in page
// cache, and it is visible in the directory for the instant between mkstemp and
// unlink. Nothing correctness-bearing depends on either.

// Linux MFD_* (include/uapi/linux/memfd.h). Darwin has no counterpart to any
// of them, so these are matched here rather than translated.
const L_MFD_CLOEXEC: c_uint = 0x0001;
const L_MFD_ALLOW_SEALING: c_uint = 0x0002;
const L_MFD_HUGETLB: c_uint = 0x0004;
const L_MFD_NOEXEC_SEAL: c_uint = 0x0008; // Linux 6.3
const L_MFD_EXEC: c_uint = 0x0010; // Linux 6.3
const L_MFD_KNOWN: c_uint =
    L_MFD_CLOEXEC | L_MFD_ALLOW_SEALING | L_MFD_HUGETLB | L_MFD_NOEXEC_SEAL | L_MFD_EXEC;

// The kernel rejects a name longer than NAME_MAX minus the "memfd:" prefix it
// prepends: 255 - 6 = 249.
const L_MFD_NAME_MAX: usize = 249;

// ---------------------------------------------------------------- the seals
//
// F_ADD_SEALS and F_GET_SEALS are Linux-only fcntl commands and there is no
// Darwin mechanism behind them -- not a weaker one, none. Darwin's own fcntl
// command space currently tops out at 116 (F_RDADVISEV), so Linux's 1033/1034
// cannot collide with anything and are safe to claim.
//
// They are implemented as bookkeeping in this runtime: the seal bits are
// recorded per object and the operations they forbid are refused here, before
// the Darwin call that would otherwise succeed.
//
// SAY THIS PLAINLY, because a caller that trusts seals is making a security
// decision with them. On Linux a seal is enforced by the kernel on the inode,
// which is why it is safe to receive a memfd over a socket from a process you
// do not trust and rely on F_SEAL_WRITE to mean the bytes cannot change under
// you. Here it is enforced by a table inside one process. Three consequences:
//
//   * A different process holding the same fd -- passed by SCM_RIGHTS, or
//     simply inherited across fork -- is not bound by these seals at all. It
//     holds an ordinary writable file.
//   * Even in this process, any path that reaches the fd without calling the
//     lxrt_memfd_check_* gates below writes straight through.
//   * The file has a real name for the instant before it is unlinked, so it is
//     not unreachable the way a Linux memfd is.
//
// A guest using seals as a local invariant gets what it expects. A guest using
// them as a trust boundary is getting a weaker promise than it thinks, and
// nothing in this runtime can close that gap without kernel support.

const L_F_ADD_SEALS: c_int = 1033; // F_LINUX_SPECIFIC_BASE(1024) + 9
const L_F_GET_SEALS: c_int = 1034; // F_LINUX_SPECIFIC_BASE(1024) + 10

const L_F_SEAL_SEAL: u32 = 0x0001;
const L_F_SEAL_SHRINK: u32 = 0x0002;
const L_F_SEAL_GROW: u32 = 0x0004;
const L_F_SEAL_WRITE: u32 = 0x0008;
const L_F_SEAL_FUTURE_WRITE: u32 = 0x0010; // Linux 5.1
const L_F_SEAL_EXEC: u32 = 0x0020; // Linux 6.3
const L_F_SEAL_KNOWN: u32 = L_F_SEAL_SEAL
    | L_F_SEAL_SHRINK
    | L_F_SEAL_GROW
    | L_F_SEAL_WRITE
    | L_F_SEAL_FUTURE_WRITE
    | L_F_SEAL_EXEC;

const MEMFD_MAX: usize = 256;

// One entry per fd; several fds can name one object, because dup() on Linux
// produces a second descriptor over the same inode and the seals are on the
// inode.
#[repr(C)]
#[derive(Clone, Copy)]
struct MemfdEnt {
    used: bool,
    fd: c_int,
    obj: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MemfdObj {
    refs: c_int,
    seals: u32,
}

static mut G_MEMFD: [MemfdEnt; MEMFD_MAX] = [MemfdEnt {
    used: false,
    fd: 0,
    obj: 0,
}; MEMFD_MAX];
static mut G_MEMOBJ: [MemfdObj; MEMFD_MAX] = [MemfdObj { refs: 0, seals: 0 }; MEMFD_MAX];
static mut G_MEMFD_LOCK: PthreadMutex = PTHREAD_MUTEX_INITIALIZER;
fork_safe!(
    fex_support_g_memfd_lock_fork_prepare,
    fex_support_g_memfd_lock_fork_release,
    G_MEMFD_LOCK
);

#[inline(always)]
unsafe fn memfd_ent(i: usize) -> *mut MemfdEnt {
    ptr::addr_of_mut!(G_MEMFD).cast::<MemfdEnt>().add(i)
}

// `obj` always comes out of the table or out of a 0..MEMFD_MAX scan.
#[inline(always)]
unsafe fn memfd_obj(obj: c_int) -> *mut MemfdObj {
    ptr::addr_of_mut!(G_MEMOBJ)
        .cast::<MemfdObj>()
        .add(obj as u32 as usize % MEMFD_MAX)
}

// Caller holds g_memfd_lock.
unsafe fn find_ent(fd: c_int) -> *mut MemfdEnt {
    if fd < 0 {
        return ptr::null_mut();
    }
    for i in 0..MEMFD_MAX {
        let e = memfd_ent(i);
        if (*e).used && (*e).fd == fd {
            return e;
        }
    }
    ptr::null_mut()
}

unsafe fn add_ent(fd: c_int, obj: c_int) -> bool {
    for i in 0..MEMFD_MAX {
        let e = memfd_ent(i);
        if (*e).used {
            continue;
        }
        (*e).used = true;
        (*e).fd = fd;
        (*e).obj = obj;
        let o = memfd_obj(obj);
        (*o).refs = (*o).refs.wrapping_add(1);
        return true;
    }
    false
}

// First object slot with no references, or -1.
unsafe fn free_obj() -> c_int {
    for i in 0..MEMFD_MAX {
        if (*memfd_obj(i as c_int)).refs == 0 {
            return i as c_int;
        }
    }
    -1
}

static mut G_MEMFD_DIR: [c_char; 512] = [0; 512];
static mut G_MEMFD_DIR_OK: bool = false;
static mut G_MEMFD_DIR_ONCE: PthreadOnce = PTHREAD_ONCE_INIT;

extern "C" fn memfd_dir_init() {
    unsafe {
        // The host's temporary directory (lxrt_host_tmpdir), not the guest's
        // TMPDIR, which may name a guest path.
        let tmp = lxrt_host_tmpdir();
        let n = strlen(tmp);
        let size = size_of::<[c_char; 512]>();
        let w = snprintf(
            ptr::addr_of_mut!(G_MEMFD_DIR).cast::<c_char>(),
            size,
            cstr!("%s%s"),
            tmp,
            if n != 0 && *tmp.add(n - 1) == b'/' as c_char {
                cstr!("")
            } else {
                cstr!("/")
            },
        );
        if w > 0 && (w as usize) < size {
            G_MEMFD_DIR_OK = true;
        }
    }
}

// pthread_once rather than the `tried`/`ready` pair this replaces. That pair
// set `tried` BEFORE the buffer was filled, so a second thread entering the
// window took the fast path, read a not-yet-ready `ready`, got NULL, and
// lxrt_memfd_create returned -ENOENT for a perfectly valid call. Measured: 32
// threads racing lxrt_memfd_create produced two such -ENOENT returns in 4 of 12
// runs. Every other shared table in this file is already lock-protected.
unsafe fn memfd_dir() -> *const c_char {
    pthread_once(ptr::addr_of_mut!(G_MEMFD_DIR_ONCE), memfd_dir_init);
    if G_MEMFD_DIR_OK {
        ptr::addr_of!(G_MEMFD_DIR).cast::<c_char>()
    } else {
        ptr::null()
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_memfd_create(name: *const c_char, lflags: c_uint) -> c_long {
    if lflags & !L_MFD_KNOWN != 0 {
        return lerr(EINVAL); // Linux rejects unknown bits outright
    }
    if lflags & L_MFD_HUGETLB != 0 {
        // Darwin has no huge-page file backing and no way to approximate one.
        // Succeeding with 16 KiB pages would give the caller a file that is
        // silently the wrong thing; superpages here are an mmap hint, not a
        // property a descriptor can carry.
        return lerr(ENOSYS);
    }

    let mut nlen: usize = 0;
    if !guest_strnlen(name, L_MFD_NAME_MAX + 1, &mut nlen) {
        return lerr(EFAULT);
    }
    if nlen > L_MFD_NAME_MAX {
        return lerr(EINVAL);
    }

    let dir = memfd_dir();
    if dir.is_null() {
        return lerr(ENOENT);
    }

    // The name is a label on Linux, shown as "memfd:<name>", and need not be
    // unique. Here it has to become part of a real filename, so '/' and
    // anything unprintable is folded away and mkstemp supplies the uniqueness.
    let mut label = [0 as c_char; 64];
    let label_p = label.as_mut_ptr();
    let mut k: usize = 0;
    let mut i: usize = 0;
    while i < nlen && k + 1 < 64 {
        let c = *name.add(i) as u8;
        *label_p.add(k) = if c == b'/' || c < 0x20 || c > 0x7e {
            b'_' as c_char
        } else {
            c as c_char
        };
        k += 1;
        i += 1;
    }
    *label_p.add(k) = 0;

    let mut path = [0 as c_char; 1024];
    let w = snprintf(
        path.as_mut_ptr(),
        1024,
        cstr!("%smemfd-%s-XXXXXX"),
        dir,
        label_p as *const c_char,
    );
    if w < 0 || w as usize >= 1024 {
        return lerr(ENAMETOOLONG);
    }

    let fd = mkstemp(path.as_mut_ptr());
    if fd < 0 {
        return lerr(errno());
    }
    // Unlink first and check second: the window in which the name exists has to
    // be as short as possible, and a failure to unlink means this is not an
    // anonymous file and must not be handed back as one.
    if unlink(path.as_ptr()) != 0 {
        let e = errno();
        close(fd);
        return lerr(e);
    }
    // mkstemp always creates with O_RDWR and without O_CLOEXEC; Linux's memfd
    // is also always readable and writable, so only the CLOEXEC bit is a
    // decision. Note the flag values differ: Linux MFD_CLOEXEC is 1 and Linux
    // FD_CLOEXEC is 1, but Darwin FD_CLOEXEC is also 1 -- the coincidence holds
    // here and is used deliberately rather than by accident.
    if lflags & L_MFD_CLOEXEC != 0 {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    // mkstemp makes the file 0600; Linux memfds are 0777 & ~umask on tmpfs but
    // nothing can open them by name, so the mode is unobservable either way.

    pthread_mutex_lock(ptr::addr_of_mut!(G_MEMFD_LOCK));
    let obj = free_obj();
    if obj < 0 || !add_ent(fd, obj) {
        pthread_mutex_unlock(ptr::addr_of_mut!(G_MEMFD_LOCK));
        close(fd);
        return lerr(EMFILE); // out of seal slots, not out of fds
    }
    let o = memfd_obj(obj);
    // Linux: without MFD_ALLOW_SEALING the file starts with F_SEAL_SEAL set, so
    // F_ADD_SEALS fails with EPERM forever after. That is the default, not an
    // error, and callers depend on being able to read it back.
    (*o).seals = if lflags & L_MFD_ALLOW_SEALING != 0 {
        0
    } else {
        L_F_SEAL_SEAL
    };
    // MFD_NOEXEC_SEAL means "create it non-executable and seal that in". The
    // exec seal is recorded so F_GET_SEALS round-trips; it is not enforced,
    // because nothing in this runtime execs a memfd directly -- lxrt_execveat
    // re-executes the runtime, which reads the file rather than executing it.
    if lflags & L_MFD_NOEXEC_SEAL != 0 {
        (*o).seals |= L_F_SEAL_EXEC;
    }
    let initial = (*o).seals;
    pthread_mutex_unlock(ptr::addr_of_mut!(G_MEMFD_LOCK));
    seals_store(fd, initial);

    fd as c_long
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_memfd_is(fd: c_int) -> bool {
    pthread_mutex_lock(ptr::addr_of_mut!(G_MEMFD_LOCK));
    let yes = !find_ent(fd).is_null();
    pthread_mutex_unlock(ptr::addr_of_mut!(G_MEMFD_LOCK));
    yes
}

// A memfd of any process: an unlinked file whose name the runtime made
// "memfd-<label>-XXXXXX". A descriptor received over a socket is not in this
// process's table, and wineserver hands every section out that way. (The
// directory is the creator's TMPDIR, not necessarily this process's.)
#[no_mangle]
pub unsafe extern "C" fn lxrt_memfd_path_is(fd: c_int) -> bool {
    let mut st = Stat::zeroed();
    let mut path = [0 as c_char; PATH_MAX];
    if fstat(fd, &mut st) != 0
        || (st.st_mode & S_IFMT) != S_IFREG
        || st.st_nlink != 0
        || fcntl(fd, F_GETPATH, path.as_mut_ptr()) != 0
    {
        return false;
    }
    let base = strrchr(path.as_ptr(), b'/' as c_int);
    let tail = if base.is_null() {
        path.as_ptr()
    } else {
        base.add(1) as *const c_char
    };
    strncmp(tail, cstr!("memfd-"), 6) == 0
}

// The reference drop shared by lxrt_memfd_track_dup and lxrt_memfd_close.
//
// Clamped. Without the clamp a refs that had already gone negative stayed
// negative, and the free-slot scan in lxrt_memfd_create looks for refs == 0
// exactly -- so the object would never be reclaimed and one of the 256 slots
// leaked for the life of the process.
#[inline(always)]
unsafe fn obj_unref(obj: c_int) {
    let o = memfd_obj(obj);
    (*o).refs = (*o).refs.wrapping_sub(1);
    if (*o).refs <= 0 {
        (*o).refs = 0;
        (*o).seals = 0;
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_memfd_track_dup(oldfd: c_int, newfd: c_int) {
    if oldfd == newfd {
        return; // dup2(fd, fd) is a no-op on Linux
    }
    pthread_mutex_lock(ptr::addr_of_mut!(G_MEMFD_LOCK));
    let mut e = find_ent(oldfd);
    if !e.is_null() {
        // A recycled fd number that used to be a memfd would otherwise keep the
        // old object's seals.
        let stale = find_ent(newfd);
        if !stale.is_null() {
            // Clamped the way lxrt_memfd_close clamps (see obj_unref).
            obj_unref((*stale).obj);
            (*stale).used = false;
            e = find_ent(oldfd); // find_ent returns a table slot; re-look up
        }
        if !e.is_null() {
            add_ent(newfd, (*e).obj);
        }
    }
    pthread_mutex_unlock(ptr::addr_of_mut!(G_MEMFD_LOCK));
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_memfd_close(fd: c_int) {
    pthread_mutex_lock(ptr::addr_of_mut!(G_MEMFD_LOCK));
    let e = find_ent(fd);
    if !e.is_null() {
        obj_unref((*e).obj);
        (*e).used = false;
    }
    pthread_mutex_unlock(ptr::addr_of_mut!(G_MEMFD_LOCK));
}

// Returns the seal word, or 0 for an fd that is not ours.
// The seals also live on the file, as an extended attribute, so that a
// process that received the descriptor (SCM_RIGHTS, binder, fork) is bound by
// them too: Chromium seals a read-only shared memory region
// (F_SEAL_FUTURE_WRITE) in the browser and its renderer checks the region
// cannot be mapped writable -- it could, the renderer's CHECK failed and every
// WebView renderer died (MEASURED). Only unlinked regular files are asked
// (the memfd's backing file), so an ordinary file costs one fstat.
const SEALS_XATTR: *const c_char = cstr!("org.steamarm.memfd.seals");

unsafe fn seals_store(fd: c_int, seals: u32) {
    let _ = fsetxattr(
        fd,
        SEALS_XATTR,
        (&seals as *const u32).cast(),
        size_of::<u32>(),
        0,
        0,
    );
}

unsafe fn seals_load(fd: c_int, out: &mut u32) -> bool {
    let mut st = Stat::zeroed();
    if fd < 0 || fstat(fd, &mut st) != 0 || (st.st_mode & S_IFMT) != S_IFREG || st.st_nlink != 0 {
        return false;
    }
    let mut v: u32 = 0;
    if fgetxattr(
        fd,
        SEALS_XATTR,
        (&mut v as *mut u32).cast(),
        size_of::<u32>(),
        0,
        0,
    ) != size_of::<u32>() as isize
    {
        return false;
    }
    *out = v;
    true
}

unsafe fn seals_of(fd: c_int) -> u32 {
    pthread_mutex_lock(ptr::addr_of_mut!(G_MEMFD_LOCK));
    let e = find_ent(fd);
    let mut s: u32 = if e.is_null() {
        0
    } else {
        (*memfd_obj((*e).obj)).seals
    };
    pthread_mutex_unlock(ptr::addr_of_mut!(G_MEMFD_LOCK));
    if e.is_null() {
        seals_load(fd, &mut s); // another process's memfd
    }
    s
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_memfd_fcntl(
    fd: c_int,
    lcmd: c_int,
    arg: c_ulong,
    handled: *mut bool,
) -> c_long {
    if lcmd != L_F_ADD_SEALS && lcmd != L_F_GET_SEALS {
        *handled = false;
        return 0;
    }
    *handled = true;

    pthread_mutex_lock(ptr::addr_of_mut!(G_MEMFD_LOCK));
    let mut e = find_ent(fd);
    let mut xs: u32 = 0;
    if e.is_null() && seals_load(fd, &mut xs) {
        // A memfd this process received: into the table, with its seals.
        let obj = free_obj();
        if obj >= 0 && add_ent(fd, obj) {
            (*memfd_obj(obj)).seals = xs;
            e = find_ent(fd);
        }
    }
    if e.is_null() {
        pthread_mutex_unlock(ptr::addr_of_mut!(G_MEMFD_LOCK));
        // Linux returns EINVAL for a file whose filesystem does not support
        // sealing, which is every file here that is not a memfd.
        return lerr(EINVAL);
    }
    let o = memfd_obj((*e).obj);

    if lcmd == L_F_GET_SEALS {
        let s = (*o).seals as c_long;
        pthread_mutex_unlock(ptr::addr_of_mut!(G_MEMFD_LOCK));
        return s;
    }

    // Narrowing to 32 bits here is what Linux itself does: its F_ADD_SEALS
    // handler takes the argument as an unsigned int, so the high half is
    // discarded rather than rejected. Measured on the aarch64 guest (6.17.1):
    //   F_ADD_SEALS(0x100000000)        -> 0,  F_GET_SEALS 0x0
    //   F_ADD_SEALS(0xFFFFFFFF00000004) -> 0,  F_GET_SEALS 0x4
    //   F_ADD_SEALS(0x40)               -> EINVAL  (unknown bit in the low half)
    // So the truncation matches, and only an unknown bit in the low 32 is an
    // error. Rejecting wide arguments here would be a divergence, not a fix.
    let want = arg as u32;
    if want & !L_F_SEAL_KNOWN != 0 {
        pthread_mutex_unlock(ptr::addr_of_mut!(G_MEMFD_LOCK));
        return lerr(EINVAL);
    }
    if (*o).seals & L_F_SEAL_SEAL != 0 {
        pthread_mutex_unlock(ptr::addr_of_mut!(G_MEMFD_LOCK));
        return lerr(EPERM);
    }
    // Linux also fails F_SEAL_WRITE with EBUSY when a shared writable mapping
    // of the file already exists, because it cannot revoke one. This runtime
    // does not track which guest mappings cover which fd, so that check is not
    // made: a guest that maps first and seals second gets a successful seal and
    // a mapping that can still write. Refusing every F_SEAL_WRITE would break
    // the common correct ordering, so the narrower gap is the one accepted.
    (*o).seals |= want;
    let now = (*o).seals;
    pthread_mutex_unlock(ptr::addr_of_mut!(G_MEMFD_LOCK));
    seals_store(fd, now);
    0
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_memfd_check_ftruncate(fd: c_int, length: i64) -> c_long {
    let s = seals_of(fd);
    if s == 0 {
        return 0;
    }
    if s & L_F_SEAL_WRITE != 0 {
        return lerr(EPERM); // any size change is a write
    }
    if s & (L_F_SEAL_SHRINK | L_F_SEAL_GROW) == 0 {
        return 0;
    }
    let mut st = Stat::zeroed();
    if fstat(fd, &mut st) != 0 {
        return lerr(errno());
    }
    if (s & L_F_SEAL_SHRINK) != 0 && length < st.st_size {
        return lerr(EPERM);
    }
    if (s & L_F_SEAL_GROW) != 0 && length > st.st_size {
        return lerr(EPERM);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_memfd_check_write(fd: c_int, offset: i64, count: u64) -> c_long {
    let s = seals_of(fd);
    if s == 0 {
        return 0;
    }
    // F_SEAL_FUTURE_WRITE differs from F_SEAL_WRITE only in that mappings taken
    // out before the seal keep writing. Since the mapping-aware distinction is
    // not tracked here (see lxrt_memfd_fcntl), both stop a write() the same way.
    if s & (L_F_SEAL_WRITE | L_F_SEAL_FUTURE_WRITE) != 0 {
        return lerr(EPERM);
    }
    if s & L_F_SEAL_GROW == 0 || count == 0 {
        return 0;
    }
    let mut at = offset;
    if at < 0 {
        let cur = lseek(fd, 0, SEEK_CUR);
        if cur < 0 {
            return lerr(errno());
        }
        at = cur;
    }
    let mut st = Stat::zeroed();
    if fstat(fd, &mut st) != 0 {
        return lerr(errno());
    }
    // Computed in uint64 rather than as `at + (int64_t)count`. That form was
    // signed overflow for a large count, and a count above INT64_MAX cast to a
    // negative int64, which made the comparison pass and let the write through
    // with the grow seal set -- the seal silently bypassed by the one argument
    // an attacker controls.
    let start = at as u64;
    let size: u64 = if st.st_size > 0 { st.st_size as u64 } else { 0 };
    // Linux under F_SEAL_GROW short-writes up to the existing size rather than
    // failing outright. Refusing the whole write is the conservative reading:
    // a caller that gets EPERM retries or reports, where a silent short write
    // that a caller does not check loses data.
    if count > u64::MAX - start || start.wrapping_add(count) > size {
        return lerr(EPERM);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_memfd_check_mmap(fd: c_int, prot: c_int, lmap_flags: c_int) -> c_long {
    // PROT_READ/WRITE/EXEC are 1/2/4 on both Linux and Darwin, and MAP_SHARED
    // is 0x01 on both. MAP_ANONYMOUS is the one that differs (0x20 against
    // 0x1000) and is not consulted here.
    let s = seals_of(fd);
    if s == 0 {
        return 0;
    }
    if (s & (L_F_SEAL_WRITE | L_F_SEAL_FUTURE_WRITE)) != 0
        && (prot & PROT_WRITE) != 0
        && (lmap_flags & MAP_SHARED) != 0
    {
        return lerr(EPERM);
    }
    0
}

// ============================================================================
// 281  execveat(int dirfd, const char *path, argv, envp, int flags)
// ============================================================================
//
// Darwin has no execveat and no fexecve. What it does have is lxrt_execve,
// which is the only thing that can start a Linux ELF here at all: a Linux ELF
// cannot be handed to Darwin's execve, so process.c re-executes the runtime
// with the guest binary as its first argument. execveat therefore reduces to
// "turn dirfd + path into a path string", and then it is the existing problem.
//
// Two Darwin facts shape the resolution, both measured:
//
//   * /dev/fd/N is a clone device, not a symlink into the namespace the way
//     Linux's /proc/self/fd/N is. open("/dev/fd/N") on a file fd works, but
//     open("/dev/fd/N/sub") on a DIRECTORY fd fails with ENOENT. So the Linux
//     trick of walking through a dirfd's /proc entry is unavailable and the
//     directory has to be named by its real path.
//   * fcntl(F_GETPATH) on an UNLINKED file still returns the path the file used
//     to have. It does not fail, it lies. Every use of F_GETPATH below is
//     therefore checked by stat'ing the answer back and comparing st_dev and
//     st_ino against the fd, which is the only way to tell a live path from a
//     stale one.

const L_AT_FDCWD: c_int = -100; // Darwin's AT_FDCWD is -2
const L_AT_SYMLINK_NOFOLLOW: c_int = 0x0100;
const L_AT_EMPTY_PATH: c_int = 0x1000; // no Darwin counterpart at all
const L_AT_EXECVEAT_KNOWN: c_int = L_AT_SYMLINK_NOFOLLOW | L_AT_EMPTY_PATH;

// Longest argv/envp this will walk before giving up. Linux's real ceiling is
// MAX_ARG_STRINGS (0x7FFFFFFF) bounded by a total byte limit; the point of the
// bound here is only to stop a corrupt guest pointer walking memory forever.
const ARGV_MAX: usize = 65536;

// True if `path` currently names the same file the fd is open on.
unsafe fn path_matches_fd(fd: c_int, path: *const c_char) -> bool {
    let mut a = Stat::zeroed();
    let mut b = Stat::zeroed();
    if fstat(fd, &mut a) != 0 || stat(path, &mut b) != 0 {
        return false;
    }
    a.st_dev == b.st_dev && a.st_ino == b.st_ino
}

// Validate a guest argv/envp: a NULL-terminated array of readable C strings.
// Only the array spine and the reachability of each string are checked; the
// contents are the guest's business.
unsafe fn argv_ok(v: *const *mut c_char) -> bool {
    if v.is_null() {
        return true; // NULL envp is legal; lxrt_execve substitutes
    }
    for i in 0..ARGV_MAX {
        let slot = v.add(i);
        if !mem_ok(slot as usize as u64, size_of::<*mut c_char>(), VM_PROT_READ) {
            return false;
        }
        let s = *slot;
        if s.is_null() {
            return true;
        }
        let mut n: usize = 0;
        if !guest_strnlen(s, 1usize << 20, &mut n) {
            return false;
        }
    }
    false // no terminator within the bound
}

// lxrt_execve RETURNS on failure -- process.c reports ENOENT, ENOEXEC and
// EACCES as ordinary outcomes -- and Linux's execveat leaves the caller's
// descriptors untouched when it fails. Without putting FD_CLOEXEC back, the
// guest's memfd (created with MFD_CLOEXEC, which is the usual way) stays
// permanently inheritable and leaks into every later exec and posix_spawn.
unsafe fn restore_cloexec(fd: c_int, fl: c_int) {
    if fd >= 0 {
        fcntl(fd, F_SETFD, fl);
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_execveat(
    dirfd: c_int,
    path: *const c_char,
    argv: *const *mut c_char,
    envp: *const *mut c_char,
    lflags: c_int,
) -> c_long {
    if lflags & !L_AT_EXECVEAT_KNOWN != 0 {
        return lerr(EINVAL);
    }

    let mut plen: usize = 0;
    if !guest_strnlen(path, PATH_MAX, &mut plen) {
        return lerr(EFAULT);
    }
    if plen >= PATH_MAX {
        return lerr(ENAMETOOLONG);
    }
    if argv.is_null() || !argv_ok(argv) || !argv_ok(envp) {
        return lerr(EFAULT);
    }

    let mut resolved_buf = [0 as c_char; PATH_MAX];
    let resolved = resolved_buf.as_mut_ptr();
    // Held open across the exec when the target can only be named through
    // /dev/fd. -1 when no such fd is involved.
    let mut keep_fd: c_int = -1;
    // The fd whose FD_CLOEXEC was cleared below, and the flag word to put back
    // on any path that returns instead of exec'ing. -1 when nothing was
    // cleared.
    let mut cloexec_fd: c_int = -1;
    let mut cloexec_fl: c_int = 0;

    if plen == 0 {
        // AT_EMPTY_PATH: execute the descriptor itself. This is what fexecve
        // compiles down to and what a caller that built a binary in a memfd
        // uses; FEX is exactly that caller.
        if lflags & L_AT_EMPTY_PATH == 0 {
            return lerr(ENOENT);
        }
        if dirfd == L_AT_FDCWD {
            // An empty path against the cwd names a directory, and executing a
            // directory is EACCES on Linux.
            return lerr(EACCES);
        }
        // Explicit, rather than leaning on fcntl to reject it: see the note in
        // the relative-path branch for why a negative dirfd that is not Linux's
        // AT_FDCWD must never reach a Darwin *at() call.
        if dirfd < 0 {
            return lerr(EBADF);
        }
        if fcntl(dirfd, F_GETFD) < 0 {
            return lerr(EBADF);
        }

        let mut via = [0 as c_char; PATH_MAX];
        if fcntl(dirfd, F_GETPATH, via.as_mut_ptr()) == 0 && path_matches_fd(dirfd, via.as_ptr()) {
            snprintf(resolved, PATH_MAX, cstr!("%s"), via.as_ptr());
        } else {
            // Unlinked, or renamed since it was opened -- the memfd case. The
            // fd itself is the only name it has.
            //
            // This is where re-executing the runtime costs something Linux does
            // not charge: the kernel's execveat holds the file open across the
            // exec itself, whereas here a second process has to reopen
            // /dev/fd/N, so N must survive the exec. Clearing FD_CLOEXEC is not
            // cosmetic -- a memfd created with MFD_CLOEXEC, which is the usual
            // way, would otherwise be closed before the new runtime could look
            // at it, and the exec would fail with ENOENT for no visible reason.
            let fl = fcntl(dirfd, F_GETFD);
            if fl >= 0 && (fl & FD_CLOEXEC) != 0 && fcntl(dirfd, F_SETFD, fl & !FD_CLOEXEC) == 0 {
                cloexec_fd = dirfd;
                cloexec_fl = fl;
            }
            snprintf(resolved, PATH_MAX, cstr!("/dev/fd/%d"), dirfd);
        }
    } else if *path == b'/' as c_char {
        snprintf(resolved, PATH_MAX, cstr!("%s"), path);
    } else if dirfd == L_AT_FDCWD {
        // The re-executed runtime inherits this process's cwd, so a relative
        // path resolves against the same directory it would have on Linux.
        snprintf(resolved, PATH_MAX, cstr!("%s"), path);
    } else {
        // Darwin's AT_FDCWD is -2 and Linux's is -100, so every other negative
        // dirfd has to be rejected before it reaches a Darwin *at() call.
        // Measured on both sides: openat(-2, "marker.txt") succeeds here and
        // resolves against the cwd, while the aarch64 guest's
        // execveat(-2, "marker.txt") returns EBADF. Letting it through executes
        // a wrong file where Linux reports an error. Absolute paths are handled
        // in the branch above and keep Linux's behaviour of ignoring dirfd
        // entirely -- measured: execveat(-7, "/bin/true") execs on Linux.
        if dirfd < 0 {
            return lerr(EBADF);
        }
        let mut dirpath = [0 as c_char; PATH_MAX];
        if fcntl(dirfd, F_GETPATH, dirpath.as_mut_ptr()) == 0
            && path_matches_fd(dirfd, dirpath.as_ptr())
        {
            let w = snprintf(resolved, PATH_MAX, cstr!("%s/%s"), dirpath.as_ptr(), path);
            if w < 0 || w as usize >= PATH_MAX {
                return lerr(ENAMETOOLONG);
            }
        } else {
            // The directory has no usable name -- it was unlinked, or it lives
            // somewhere F_GETPATH cannot describe. openat() still resolves
            // through the fd because that work happens in the kernel, so the
            // target is opened here and named by the resulting descriptor.
            let t = openat(dirfd, path, O_RDONLY);
            if t < 0 {
                return lerr(errno());
            }
            keep_fd = t;
            snprintf(resolved, PATH_MAX, cstr!("/dev/fd/%d"), t);
        }
    }

    if lflags & L_AT_SYMLINK_NOFOLLOW != 0 {
        // Linux fails execveat with ELOOP when the final component is a symlink
        // and this flag is set. Darwin's execve has no way to express that, so
        // the check is made here; it is a check rather than a guarantee, since
        // the path can change between the lstat and the exec.
        let mut st = Stat::zeroed();
        if lstat(resolved, &mut st) == 0 && (st.st_mode & S_IFMT) == S_IFLNK {
            restore_cloexec(cloexec_fd, cloexec_fl);
            if keep_fd >= 0 {
                close(keep_fd);
            }
            return lerr(ELOOP);
        }
    }

    if lxrt_trace_on() {
        fprintf(
            lxrt_trace_stream(),
            cstr!("[lxrt] execveat(dirfd=%d, \"%s\", flags=0x%x) -> %s\n"),
            dirfd,
            if plen != 0 { path } else { cstr!("") },
            lflags as c_uint,
            resolved as *const c_char,
        );
    }

    // lxrt_execve does not return on success. argv[0] is dropped there and
    // rebuilt from the path, which is the runtime's existing execve behaviour
    // and is inherited rather than re-decided here.
    let r = lxrt_execve(resolved, argv, envp);
    restore_cloexec(cloexec_fd, cloexec_fl);
    if keep_fd >= 0 {
        close(keep_fd);
    }
    r
}

// ============================================================================
// 137  rt_sigtimedwait(set, info, timeout, sigsetsize)
// ============================================================================
//
// Darwin has sigwait, which blocks without a deadline, and nothing else:
// sigwaitinfo, sigtimedwait and sigqueue are all absent from the SDK. So the
// deadline has to be built.
//
// The brief offered kqueue EVFILT_SIGNAL. It was measured and it does not work
// here. On macOS 15 / arm64, with the signal blocked:
//
//   kill(getpid(), SIGUSR2)         -> kevent wakes,      160 ms  (as expected)
//   pthread_kill(self, SIGUSR1)     -> kevent NEVER wakes, timed out at 2 s
//   signal raised before the kevent -> not reported; the filter is edge
//                                      triggered on delivery, so an already
//                                      pending signal is invisible to it
//
// EVFILT_SIGNAL hangs off the process, and a thread-directed signal never
// reaches it. Every signal this runtime delivers is thread-directed:
// lxrt_tgkill and lxrt_kill both end in pthread_kill, because the process also
// contains AppKit's main thread and libdispatch's workers and a signal must be
// aimed at a guest thread. A kqueue implementation would therefore have been
// invisible to exactly the signals FEX is waiting for, and would have looked
// like it worked in any test that used kill().
//
// So: sigwait, which does see thread-directed signals and returns immediately
// on an already-pending one (measured at 0.3 microseconds), plus a timer thread
// that pthread_kills the waiter at the deadline to break it out. Costs one
// shared thread for the process, not one per wait.
//
// ------------------------------------------- what sigwait gets wrong here
//
// Darwin's sigwait is not thread-scoped the way Linux's dequeue is. MEASURED:
// thread W blocks SIGINFO and pthread_kills ITSELF, so the signal is pending on
// W; thread V then calls sigwait() on a set containing SIGINFO and V's sigwait
// returns 29, while W's sigpending() clears. V took a signal that was aimed at
// W and W never ran. On Linux a thread-directed signal can only be dequeued by
// its target.
//
// For the runtime's own deadline wakeup this is handled: the timer re-sends
// WAKE_SIG until the owner disarms (see WAKE_RETRY_NS), so a swallowed wakeup
// costs a millisecond instead of hanging the thread.
//
// For the GUEST's signals it is a divergence that cannot be closed from here,
// and it is written down rather than papered over. If two guest threads are
// both inside rt_sigtimedwait with overlapping sets and the guest sends a
// signal to one of them specifically -- tgkill, which reaches pthread_kill --
// the other thread's sigwait may return it instead. Linux would deliver it only
// to the named thread. Nothing in userspace can tell which thread a signal
// arriving out of sigwait was aimed at, so there is no way to detect the theft,
// let alone re-post it to the right thread; the information is gone by the time
// sigwait returns. It needs a per-thread dequeue primitive Darwin does not
// have. The exposure is narrow -- it needs two guest threads waiting on the
// same signal number at the same time -- but it is real.

// The deadline wakeup. Darwin's SIGINFO is 29 and no Linux signal maps onto it
// -- the only free number besides SIGEMT(7), which signal.c already claimed as
// the realtime carrier. See the disposition note below for why it needs a
// handler installed.
const WAKE_SIG: c_int = SIGINFO;
// signal.c's realtime carrier. Named again here rather than shared, because
// this file must not edit signal.c; if that choice ever moves, both change.
const RT_CARRIER: c_int = SIGEMT;

const LINUX_NSIG: c_int = 64;
const LINUX_SIGRTMIN: c_int = 32;

// Linux siginfo_t, 128 bytes, matching the layout signal.c builds its frames
// with. The union at offset 16 is _sifields; for a signal that arrived from
// kill() or sigqueue() its members are si_pid, si_uid and si_value, which is
// the same 16 bytes signal.c uses as si_addr for the fault signals.
//
// The tag is _kill rather than plain linux_siginfo because signal.c:136 already
// defines `struct linux_siginfo` with a DIFFERENT member list (si_addr at 16,
// rest[104]). Two definitions of one tag with incompatible members in one
// program is undefined behaviour under C11 6.2.7, and the Makefile builds every
// source in a single clang invocation. Both are 128 bytes with the union at
// offset 16 so it is harmless today, but -flto -- or hoisting either definition
// into a header -- would turn it into silent field misinterpretation in the
// signal frames. Distinct tags cannot collide. The real fix is one shared
// definition with a named union in lxrt.h, which is not this file to make.
#[repr(C)]
struct LinuxSiginfoKill {
    si_signo: i32,  // 0
    si_errno: i32,  // 4
    si_code: i32,   // 8
    pad_: i32,      // 12
    si_pid: i32,    // 16
    si_uid: u32,    // 20
    si_value: u64,  // 24
    rest: [u8; 96], // 32
}

// Linux siginfo_t is 128 bytes; the guest reads fields by offset.
const _: [(); 128] = [(); size_of::<LinuxSiginfoKill>()];
// si_pid shares offset 16 with si_addr, as the kernel union does.
const _: [(); 16] = [(); std::mem::offset_of!(LinuxSiginfoKill, si_pid)];

// Linux si_code for a signal sent by kill() is SI_USER == 0. Darwin's SI_USER
// is 0x10001, and SI_QUEUE 0x10002 against Linux's -1. Nothing here forwards a
// Darwin si_code, precisely so that divergence cannot leak out.
const L_SI_USER: i32 = 0;

// ------------------------------------------------ Darwin drops ignored signals
//
// Measured, and this one is load-bearing. Darwin discards a signal at post time
// when its effective disposition is "ignore", EVEN WHEN THE SIGNAL IS BLOCKED.
// Linux makes it pending instead, specifically so that sigwait and
// rt_sigtimedwait can pick it up. Blocking the signal and calling pthread_kill
// on macOS 15 / arm64:
//
//   SIG_DFL, ignore-by-default: 16 URG, 20 CHLD, 23 IO, 28 WINCH, 29 INFO
//                                                          -> DROPPED
//   SIG_IGN, any signal                                    -> DROPPED
//   handler installed, any signal                          -> PENDING
//
// Read back through the Linux-to-Darwin map, a guest that waits on Linux
// SIGCHLD(17 -> Darwin 20) or SIGWINCH(28 -> 28) or SIGURG(23 -> 16) or
// SIGIO(29 -> 23) with the default disposition would wait forever, and the
// failure would look like the signal was never sent. So for the duration of a
// wait, any signal in the set whose disposition would cause a drop gets a no-op
// handler, refcounted so that overlapping waits on other threads do not restore
// each other's saved disposition.

extern "C" fn noop_handler(_s: c_int) {}

fn ignored_by_default(dsig: c_int) -> bool {
    dsig == SIGURG || dsig == SIGCHLD || dsig == SIGIO || dsig == SIGWINCH || dsig == SIGINFO
}

static mut G_UNDROP_REF: [c_int; NSIG] = [0; NSIG];
static mut G_UNDROP_OLD: [SigAction; NSIG] = [SigAction::ZERO; NSIG];
static mut G_UNDROP_INSTALLED: [bool; NSIG] = [false; NSIG];
static mut G_UNDROP_LOCK: PthreadMutex = PTHREAD_MUTEX_INITIALIZER;
fork_safe!(
    fex_support_g_undrop_lock_fork_prepare,
    fex_support_g_undrop_lock_fork_release,
    G_UNDROP_LOCK
);

unsafe fn undrop_begin(set: SigSet) {
    pthread_mutex_lock(ptr::addr_of_mut!(G_UNDROP_LOCK));
    let refs = ptr::addr_of_mut!(G_UNDROP_REF).cast::<c_int>();
    let olds = ptr::addr_of_mut!(G_UNDROP_OLD).cast::<SigAction>();
    let installed = ptr::addr_of_mut!(G_UNDROP_INSTALLED).cast::<bool>();
    for d in 1..NSIG {
        let sig = d as c_int;
        if !sigismember(set, sig) {
            continue;
        }
        let before = *refs.add(d);
        *refs.add(d) = before.wrapping_add(1);
        if before > 0 {
            continue;
        }
        let mut old = SigAction::ZERO;
        if sigaction(sig, ptr::null(), &mut old) != 0 {
            continue;
        }
        let drops = (old.sa_handler == SIG_IGN)
            || (old.sa_handler == SIG_DFL && ignored_by_default(sig));
        if !drops {
            continue;
        }
        // SIGCHLD under SIG_IGN is the one case where the no-op must NOT be
        // installed, and Linux agrees. Measured on the aarch64 guest:
        //   SIGCHLD SIG_IGN, blocked, child exits -> rt_sigtimedwait EAGAIN,
        //                                            waitpid ECHILD (auto-reaped)
        //   SIGCHLD SIG_DFL, blocked, child exits -> returns 17, child reapable
        // Linux never generates the signal at all when the parent ignores
        // SIGCHLD; it reaps instead. Darwin does the same. Replacing SIG_IGN
        // with a handler here would deliver a signal Linux would not AND switch
        // Darwin back to accumulating zombies for the duration -- a
        // process-wide change that any wait4 on another thread would see.
        //
        // The documented cost: a SIGCHLD sent EXPLICITLY with kill() while the
        // disposition is SIG_IGN is delivered on Linux (measured: raise() under
        // SIG_IGN + blocked returns 17) and is not seen here. That is narrow --
        // a guest that ignores SIGCHLD and then waits for a hand-delivered one
        // -- and it is the smaller divergence of the two. SIG_DFL SIGCHLD is
        // unaffected and still gets the no-op, which is the case that matters.
        if sig == SIGCHLD && old.sa_handler == SIG_IGN {
            continue;
        }
        let na = SigAction {
            sa_handler: noop_handler as extern "C" fn(c_int) as usize,
            sa_mask: 0, // sigemptyset
            // SA_RESTART so that a stray delivery on some other thread, where
            // this signal is not blocked, cannot turn one of its syscalls into
            // EINTR.
            sa_flags: SA_RESTART,
        };
        if sigaction(sig, &na, ptr::null_mut()) == 0 {
            *olds.add(d) = old;
            *installed.add(d) = true;
        }
    }
    pthread_mutex_unlock(ptr::addr_of_mut!(G_UNDROP_LOCK));
}

unsafe fn undrop_end(set: SigSet) {
    pthread_mutex_lock(ptr::addr_of_mut!(G_UNDROP_LOCK));
    let refs = ptr::addr_of_mut!(G_UNDROP_REF).cast::<c_int>();
    let olds = ptr::addr_of_mut!(G_UNDROP_OLD).cast::<SigAction>();
    let installed = ptr::addr_of_mut!(G_UNDROP_INSTALLED).cast::<bool>();
    for d in 1..NSIG {
        let sig = d as c_int;
        if !sigismember(set, sig) {
            continue;
        }
        if *refs.add(d) > 0 {
            *refs.add(d) -= 1;
            if *refs.add(d) == 0 && *installed.add(d) {
                sigaction(sig, olds.add(d), ptr::null_mut());
                *installed.add(d) = false;
            }
        }
    }
    pthread_mutex_unlock(ptr::addr_of_mut!(G_UNDROP_LOCK));
}

// ---------------------------------------------------------------- the timer

// One slot per wait, living on the waiting thread's own stack and linked into
// g_timer_list only while it is armed.
//
// This used to be a fixed array of 64 slots, and the cap did not merely limit
// concurrency -- it INVENTED A TIMEOUT. With every slot taken, timer_arm failed
// and the wait returned -EAGAIN, which is exactly what Linux returns on expiry,
// so a guest could not tell the two apart and a caller looping on EAGAIN spun
// at 100% CPU. Measured on the array version: 80 concurrent waits of 300 ms
// each, 16 of them (80 - 64, exactly the overflow) returned -EAGAIN after a
// wall time of 0.0000 s. FEX runs far more than 64 threads.
//
// A stack slot has no table to exhaust, so the cap is gone rather than raised.
// The lifetime is safe because linking, firing and unlinking all happen under
// g_timer_lock: once timer_disarm returns, the timer thread can no longer
// reach the slot. The wait cleanup disarms it on return and pthread_exit.
#[repr(C)]
struct TimerSlot {
    next: *mut TimerSlot, // g_timer_list; read/written under g_timer_lock
    linked: bool,         // read/written under g_timer_lock
    target: PthreadT,
    // When the next WAKE_SIG is due, monotonic. Set to the wait's deadline on
    // arm, then pushed forward by WAKE_RETRY_NS each time one is sent.
    deadline_ns: u64,
}

static mut G_TIMER_LIST: *mut TimerSlot = ptr::null_mut();
static mut G_TIMER_LOCK: PthreadMutex = PTHREAD_MUTEX_INITIALIZER;
fork_safe!(
    fex_support_g_timer_lock_fork_prepare,
    fex_support_g_timer_lock_fork_release,
    G_TIMER_LOCK
);
static mut G_TIMER_COND: PthreadCond = PTHREAD_COND_INITIALIZER;
static mut G_TIMER_RUNNING: bool = false;

// The three LXRT_FORK_SAFE registrations, in the order the C file declared
// them (memfd, undrop, timer), run as one image initialiser.
extern "C" fn fex_support_fork_register() {
    unsafe {
        pthread_atfork(
            Some(fex_support_g_memfd_lock_fork_prepare),
            Some(fex_support_g_memfd_lock_fork_release),
            Some(fex_support_g_memfd_lock_fork_release),
        );
        pthread_atfork(
            Some(fex_support_g_undrop_lock_fork_prepare),
            Some(fex_support_g_undrop_lock_fork_release),
            Some(fex_support_g_undrop_lock_fork_release),
        );
        pthread_atfork(
            Some(fex_support_g_timer_lock_fork_prepare),
            Some(fex_support_g_timer_lock_fork_release),
            Some(fex_support_g_timer_lock_fork_release),
        );
    }
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FEX_SUPPORT_FORK_INIT: extern "C" fn() = fex_support_fork_register;

// How often an expired slot re-sends WAKE_SIG until its owner disarms it.
//
// One pthread_kill is NOT a delivery guarantee on Darwin. Measured directly:
// thread V sitting in sigwait() on a set containing SIGINFO dequeued a SIGINFO
// that pthread_kill had aimed at thread W -- V's sigwait returned 29 and W's
// pending set cleared, without W ever running. Linux cannot do that; a
// thread-directed signal there is takeable only by its target. Since every
// waiter here has WAKE_SIG in its sigwait set, any of them can swallow another
// thread's wakeup.
//
// The old code marked the slot fired after one kill and never looked at it
// again, so a swallowed wakeup left the owner in sigwait forever with its
// deadline long gone -- reproduced as threads stuck in sigwait with the timer
// thread idle in pthread_cond_wait. Re-sending costs one kill per millisecond
// per not-yet-disarmed expired slot, and in practice the owner disarms on the
// first one.
const WAKE_RETRY_NS: u64 = 1_000_000;

extern "C" fn timer_main(_unused: *mut c_void) -> *mut c_void {
    unsafe {
        pthread_setname_np(cstr!("lxrt-sigtimer"));
        // The timer thread must never take a guest signal itself. The
        // synchronous faults are left unblocked deliberately: blocking one does
        // not prevent it, it makes the kernel force-deliver and kill, which
        // would turn a bug in this thread into an unexplained process death.
        let mut all: SigSet = !0; // sigfillset
        sigdelset(&mut all, SIGSEGV);
        sigdelset(&mut all, SIGBUS);
        sigdelset(&mut all, SIGILL);
        sigdelset(&mut all, SIGFPE);
        sigdelset(&mut all, SIGTRAP);
        sigdelset(&mut all, SIGSYS);
        pthread_sigmask(SIG_BLOCK, &all, ptr::null_mut());

        pthread_mutex_lock(ptr::addr_of_mut!(G_TIMER_LOCK));
        loop {
            let now = mono_ns();
            let mut next = u64::MAX;
            let mut pp: *mut *mut TimerSlot = ptr::addr_of_mut!(G_TIMER_LIST);
            while !(*pp).is_null() {
                let s = *pp;
                if (*s).deadline_ns <= now {
                    if pthread_kill((*s).target, WAKE_SIG) == ESRCH {
                        // Retire a missing target; the slot belongs to its
                        // waiter, so unlink it but never free it here.
                        *pp = (*s).next;
                        (*s).linked = false;
                        (*s).next = ptr::null_mut();
                        continue;
                    }
                    // Rescheduled rather than retired: see WAKE_RETRY_NS.
                    (*s).deadline_ns = now.wrapping_add(WAKE_RETRY_NS);
                }
                if (*s).deadline_ns < next {
                    next = (*s).deadline_ns;
                }
                pp = ptr::addr_of_mut!((*s).next);
            }
            if next == u64::MAX {
                pthread_cond_wait(
                    ptr::addr_of_mut!(G_TIMER_COND),
                    ptr::addr_of_mut!(G_TIMER_LOCK),
                );
                continue;
            }
            // The clock is re-read into n2 and the result guarded, because
            // `now` was sampled before a scan that does a pthread_kill per
            // expired slot and can be preempted anywhere inside it. When the
            // clock has passed `next` in that window, the `next - mono_ns()`
            // this replaces underflowed to ~1.8e19 ns -- and
            // pthread_cond_timedwait_relative_np does NOT reject that value, it
            // waits on it (tv_sec 18446744073). The armed deadline was then
            // never delivered and the guest thread sat in sigwait with no
            // timeout at all.
            //
            // Measured on the old code: 24 waiter threads doing sub-millisecond
            // timed waits under CPU contention, 0 of 24 finished in 60 s, with
            // the underflow window hit dozens of times per second (late by
            // 0.3 us to 18 us). Re-scanning instead of sleeping fires the slot
            // on the very next pass; it cannot spin, because that pass sends
            // the slot's WAKE_SIG and moves its deadline to
            // now + WAKE_RETRY_NS, which is strictly in the future.
            let n2 = mono_ns();
            if next <= n2 {
                continue;
            }
            let d = next - n2;
            // Relative, because Darwin's pthread_cond_timedwait is against
            // CLOCK_REALTIME and a wall-clock step would move every deadline.
            let rel = Timespec {
                tv_sec: (d / 1_000_000_000) as i64,
                tv_nsec: (d % 1_000_000_000) as i64,
            };
            pthread_cond_timedwait_relative_np(
                ptr::addr_of_mut!(G_TIMER_COND),
                ptr::addr_of_mut!(G_TIMER_LOCK),
                &rel,
            );
        }
    }
}

// Links `s` into the deadline list. The only way this fails is the timer thread
// itself failing to start, which is a genuine resource failure and is reported
// as one -- there is deliberately no "table full" outcome any more.
unsafe fn timer_arm(s: *mut TimerSlot, deadline_ns: u64) -> bool {
    pthread_mutex_lock(ptr::addr_of_mut!(G_TIMER_LOCK));
    if (*s).linked {
        pthread_mutex_unlock(ptr::addr_of_mut!(G_TIMER_LOCK));
        return true;
    }
    if !G_TIMER_RUNNING {
        let mut t: PthreadT = ptr::null_mut();
        if pthread_create(&mut t, ptr::null(), timer_main, ptr::null_mut()) != 0 {
            pthread_mutex_unlock(ptr::addr_of_mut!(G_TIMER_LOCK));
            return false;
        }
        pthread_detach(t);
        G_TIMER_RUNNING = true;
    }
    (*s).target = pthread_self();
    (*s).deadline_ns = deadline_ns;
    (*s).next = G_TIMER_LIST;
    G_TIMER_LIST = s;
    (*s).linked = true;
    pthread_cond_signal(ptr::addr_of_mut!(G_TIMER_COND));
    pthread_mutex_unlock(ptr::addr_of_mut!(G_TIMER_LOCK));
    true
}

// Unlinks the slot. Firing and unlinking both happen under g_timer_lock, so
// once this returns the timer can no longer reach the slot and no further
// WAKE_SIG can be sent for it -- which is what makes it safe for the slot to be
// a stack object that dies when the caller returns.
unsafe fn timer_disarm(s: *mut TimerSlot) {
    pthread_mutex_lock(ptr::addr_of_mut!(G_TIMER_LOCK));
    if !(*s).linked {
        pthread_mutex_unlock(ptr::addr_of_mut!(G_TIMER_LOCK));
        return;
    }
    let mut pp: *mut *mut TimerSlot = ptr::addr_of_mut!(G_TIMER_LIST);
    while !(*pp).is_null() {
        if *pp == s {
            *pp = (*s).next;
            break;
        }
        pp = ptr::addr_of_mut!((**pp).next);
    }
    (*s).linked = false;
    (*s).next = ptr::null_mut();
    pthread_mutex_unlock(ptr::addr_of_mut!(G_TIMER_LOCK));
}

// Shared by normal return and pthread_exit from a guest signal handler.
#[repr(C)]
struct SigwaitCleanup {
    slot: *mut TimerSlot,
    undrop: SigSet,
    oldmask: SigSet,
    undrop_active: bool,
    mask_active: bool,
}

unsafe extern "C" fn sigwait_cleanup(arg: *mut c_void) {
    let c = arg.cast::<SigwaitCleanup>();
    // Unlink before restoring dispositions or unblocking pending handlers.
    timer_disarm((*c).slot);
    if (*c).undrop_active {
        (*c).undrop_active = false;
        undrop_end((*c).undrop);
    }
    if (*c).mask_active {
        (*c).mask_active = false;
        pthread_sigmask(SIG_SETMASK, ptr::addr_of!((*c).oldmask), ptr::null_mut());
    }
}

// ------------------------------------------------------------ realtime half
//
// Linux realtime signals 32..63 have no Darwin numbers at all. signal.c carries
// them as a number queued against the target thread plus a SIGEMT to wake it,
// and that is what FEX waits on. Two things follow for this wait:
//
//   * SIGEMT must be blocked for its duration. Otherwise signal.c's host
//     handler runs, dequeues the number and dispatches it to the guest's
//     handler -- the signal is consumed by delivery and rt_sigtimedwait never
//     sees it, which is the opposite of what the call means.
//   * With SIGEMT blocked the carrier stays pending and the number stays in the
//     queue, so the queue can be read directly.

// lxrt_rt_dequeue_self pops the head unconditionally; there is no filtered
// take. Drain, keep the first number the set asked for, and put the rest back
// in the order they came out. A concurrent enqueue during the drain can end up
// behind a requeued signal; Linux's own ordering guarantee for realtime signals
// is per-signal-number, which this preserves.
// thread.c:71 sizes each thread's realtime queue at RT_QUEUE_DEPTH 32, and it
// is a ring that keeps one slot empty, so a full drain can yield at most 31
// entries. Named again here because this file must not edit thread.c or lxrt.h;
// sized above that so the hold-back buffer can always take everything the queue
// can hand over. It used to be sized by TIMER_SLOTS, a completely unrelated
// constant that happened to be large enough -- the coupling was accidental, and
// raising the queue depth would have started silently destroying signals.
const RT_HELD_MAX: usize = 64;

unsafe fn rt_take(rtmask: u64, leftover: &mut bool) -> c_int {
    let mut held = [0u8; RT_HELD_MAX];
    let held_p = held.as_mut_ptr();
    let mut n: usize = 0;
    let mut want: c_int = 0;

    for _guard in 0..256 {
        // Stop draining rather than pop something with nowhere to put it: a
        // signal dequeued here and not requeued is destroyed outright. Leaving
        // it in the queue keeps it deliverable.
        if n == RT_HELD_MAX {
            break;
        }
        let s = lxrt_rt_dequeue_self();
        if s == 0 {
            break;
        }
        if s < LINUX_SIGRTMIN || s > LINUX_NSIG {
            // Out of range names no Linux signal, so there is nothing to
            // requeue it as and nothing that could ever deliver it. It can only
            // be dropped -- but not in silence, because its presence means
            // something upstream wrote a bad value into the queue.
            if lxrt_trace_on() {
                fprintf(
                    lxrt_trace_stream(),
                    cstr!("[lxrt] rt_sigtimedwait: dropped queue entry %d (outside %d..%d)\n"),
                    s,
                    LINUX_SIGRTMIN,
                    LINUX_NSIG,
                );
            }
            continue;
        }
        if want == 0 && (rtmask & (1u64 << ((s - 1) as u32))) != 0 {
            want = s;
            continue;
        }
        *held_p.add(n) = s as u8;
        n += 1;
    }
    let me = lxrt_gettid();
    for i in 0..n {
        let sig = *held_p.add(i) as c_int;
        // A failed requeue means the queue filled from another thread during
        // the drain. Linux drops on a full queue too, so the behaviour is not
        // wrong -- but it is a real lost signal and the return value used to be
        // discarded, which made it invisible.
        if !lxrt_rt_enqueue(me, sig) && lxrt_trace_on() {
            fprintf(
                lxrt_trace_stream(),
                cstr!("[lxrt] rt_sigtimedwait: requeue of signal %d failed; it is lost\n"),
                sig,
            );
        }
    }
    *leftover = n > 0;
    want
}

// ---------------------------------------------------------------- the wait

unsafe fn write_info(uinfo: *mut c_void, lsig: c_int) {
    if uinfo.is_null() {
        return;
    }
    // Darwin's sigwait yields a number and nothing else, and Darwin has no
    // sigqueue, so there is no sender identity and no sigval to report. Linux
    // would fill in the real si_pid/si_uid and, for a sigqueue'd signal,
    // si_value. Reporting SI_USER from this process is the honest shape of what
    // is known: a caller reading si_value gets zero rather than a wrong value.
    let si = LinuxSiginfoKill {
        si_signo: lsig,
        si_errno: 0,
        si_code: L_SI_USER,
        pad_: 0,
        si_pid: getpid(),
        si_uid: getuid(),
        si_value: 0,
        rest: [0; 96],
    };
    ptr::copy_nonoverlapping(
        (&si as *const LinuxSiginfoKill).cast::<u8>(),
        uinfo.cast::<u8>(),
        size_of::<LinuxSiginfoKill>(),
    );
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_rt_sigtimedwait(
    uset: *const c_void,
    uinfo: *mut c_void,
    utimeout: *const c_void,
    sigsetsize: usize,
) -> c_long {
    // Linux checks this before anything else; aarch64 sigset_t is 8 bytes.
    if sigsetsize != 8 {
        return lerr(EINVAL);
    }
    if !mem_ok(uset as usize as u64, 8, VM_PROT_READ) {
        return lerr(EFAULT);
    }
    if !uinfo.is_null()
        && !mem_ok(
            uinfo as usize as u64,
            size_of::<LinuxSiginfoKill>(),
            VM_PROT_WRITE,
        )
    {
        return lerr(EFAULT);
    }

    let lmask: u64 = ptr::read_unaligned(uset.cast::<u64>());

    let mut timed = false;
    let mut deadline: u64 = 0;
    if !utimeout.is_null() {
        if !mem_ok(
            utimeout as usize as u64,
            size_of::<LinuxTimespec>(),
            VM_PROT_READ,
        ) {
            return lerr(EFAULT);
        }
        let ts: LinuxTimespec = ptr::read_unaligned(utimeout.cast::<LinuxTimespec>());
        if ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1_000_000_000 {
            return lerr(EINVAL);
        }
        timed = true;
        let now = mono_ns();
        let secs = ts.tv_sec as u64;
        let nsec = ts.tv_nsec as u64;
        // Saturate instead of wrapping. The plain sum overflowed uint64 above
        // tv_sec ~1.8e10 and put the deadline in the PAST, so the longest
        // possible wait became an instant -EAGAIN. Linux honours those
        // timeouts by blocking: measured on the aarch64 guest, rt_sigtimedwait
        // with tv_sec 2e10 and with 2^63-1 both waited until a SIGALRM
        // interrupted them (EINTR at 2.05 s) -- neither EINVAL nor EAGAIN.
        // UINT64_MAX is "effectively never", which is the honest translation.
        if secs > u64::MAX.wrapping_sub(now).wrapping_sub(nsec) / 1_000_000_000 {
            deadline = u64::MAX;
        } else {
            deadline = now
                .wrapping_add(secs.wrapping_mul(1_000_000_000))
                .wrapping_add(nsec);
        }
    }

    // Bit n-1 of the Linux sigset is signal n, so bits 0..30 are 1..31 and bits
    // 31..63 are the realtime range 32..64.
    let rtmask: u64 = lmask & !((1u64 << 31) - 1);

    let mut dset: SigSet = 0; // sigemptyset
    let mut any_classic = false;
    for l in 1..32 as c_int {
        if lmask & (1u64 << ((l - 1) as u32)) == 0 {
            continue;
        }
        let d = lxrt_signo_to_darwin(l);
        // Linux silently ignores SIGKILL and SIGSTOP in the set rather than
        // failing, and a Linux signal with no Darwin number (SIGSTKFLT 16,
        // SIGPWR 30) can never arrive here, so it is dropped from the set
        // rather than made to fail the whole call.
        if d == 0 || d == SIGKILL || d == SIGSTOP {
            continue;
        }
        sigaddset(&mut dset, d);
        any_classic = true;
    }
    if !any_classic && rtmask == 0 {
        // A set that can never produce anything: either it was empty, or every
        // Linux signal in it is one of the two with no Darwin number. Linux
        // still blocks, and is still interruptible, so reproduce that rather
        // than inventing an error -- pause() returns EINTR for exactly the
        // event that would end the wait on Linux.
        if !timed {
            pause();
            return lerr(EINTR);
        }
        let now = mono_ns();
        let d = if deadline > now { deadline - now } else { 0 };
        let ts = Timespec {
            tv_sec: (d / 1_000_000_000) as i64,
            tv_nsec: (d % 1_000_000_000) as i64,
        };
        // An interrupted sleep is not an expiry. Returning EAGAIN regardless
        // reported a timeout that did not happen, where Linux returns EINTR --
        // the untimed sibling above already gets this right with pause().
        if nanosleep(&ts, ptr::null_mut()) != 0 {
            return lerr(errno());
        }
        return lerr(EAGAIN);
    }

    // Block, for the whole wait:
    //   * the set itself, so a guest handler cannot consume the signal first.
    //     Linux requires the caller to have blocked it and returns EINTR if a
    //     handler runs instead; blocking it here makes the call work for a
    //     caller that forgot, at the cost of not reproducing that EINTR.
    //   * the realtime carrier, so signal.c's handler does not dispatch a
    //     queued realtime signal out from under this wait.
    //   * the deadline wakeup.
    let mut blockme: SigSet = dset;
    sigaddset(&mut blockme, RT_CARRIER);
    sigaddset(&mut blockme, WAKE_SIG);
    let mut oldmask: SigSet = 0;
    // pthread_sigmask RETURNS the error number and is not specified to set
    // errno. Reading errno here worked only by accident -- measured, Darwin's
    // implementation happens to set it too (pthread_sigmask(bad how) -> ret 22,
    // errno 22) -- and an errno left at 0 would have fallen through
    // lxrt_errno_to_linux's default arm and reported EIO for an EINVAL.
    let mrc = pthread_sigmask(SIG_BLOCK, &blockme, &mut oldmask);
    if mrc != 0 {
        return lerr(mrc);
    }

    // WAKE_SIG is ignored by default on Darwin and would be discarded rather
    // than made pending, so it is in the undrop set with everything else.
    let mut undrop: SigSet = dset;
    sigaddset(&mut undrop, WAKE_SIG);
    undrop_begin(undrop);

    // What sigwait may return: the wanted signals, plus the two wakeups.
    let mut waitset: SigSet = dset;
    sigaddset(&mut waitset, RT_CARRIER);
    sigaddset(&mut waitset, WAKE_SIG);

    let mut result: c_long = 0;
    let mut rt_leftover = false;
    // On this thread's stack, linked into the timer's list only while armed.
    let mut slot = TimerSlot {
        next: ptr::null_mut(),
        linked: false,
        target: ptr::null_mut(),
        deadline_ns: 0,
    };
    let slot_p: *mut TimerSlot = &mut slot;
    let mut cleanup = SigwaitCleanup {
        slot: slot_p,
        undrop,
        oldmask,
        undrop_active: true,
        mask_active: true,
    };
    let cleanup_p: *mut SigwaitCleanup = &mut cleanup;

    // pthread_cleanup_push(sigwait_cleanup, &cleanup), exactly as the Darwin
    // <pthread.h> macro expands: a handler record on this stack frame, linked
    // at the head of the thread's cleanup stack. pthread_exit from a guest
    // signal handler walks that stack and runs it.
    let self_thread = pthread_self();
    let mut handler = DarwinPthreadHandlerRec {
        routine: sigwait_cleanup,
        arg: cleanup_p.cast(),
        next: ptr::null_mut(),
    };
    let handler_p: *mut DarwinPthreadHandlerRec = &mut handler;
    (*handler_p).next = (*self_thread).cleanup_stack;
    ptr::write_volatile(ptr::addr_of_mut!((*self_thread).cleanup_stack), handler_p);

    loop {
        // Realtime first: it is the case FEX actually waits on, and it is read
        // from the queue rather than from Darwin's pending set.
        if rtmask != 0 {
            let mut left = false;
            let rs = rt_take(rtmask, &mut left);
            rt_leftover = rt_leftover || left;
            if rs != 0 {
                write_info(uinfo, rs);
                result = rs as c_long;
                break;
            }
        }

        // There used to be a sigpending() fast path here that called
        // sigwait() on just the signals sigpending() had reported. It is gone
        // rather than repaired. Two reasons, both measured:
        //
        //   * It could block with no deadline. Its set was a subset of dset and
        //     so never contained WAKE_SIG, which meant the timer's
        //     pthread_kill(WAKE_SIG) could not break it out even on an
        //     iteration where the slot WAS armed. If the signal sigpending()
        //     reported got consumed elsewhere between the two calls, that
        //     sigwait blocked forever and the guest's timeout never fired.
        //   * It bought nothing. The main sigwait below already returns
        //     immediately for an already-pending member: measured at 0.282 us
        //     mean over 2000 iterations on a three-signal set, against a
        //     sigpending() plus a set rebuild plus a second sigwait.
        //
        // An unbounded hang path in exchange for no measurable gain is not a
        // trade worth keeping.

        if timed {
            let now = mono_ns();
            if now >= deadline {
                result = lerr(EAGAIN); // Linux reports a timeout as EAGAIN
                break;
            }
            if !timer_arm(slot_p, deadline) {
                // The only remaining way to get here is the timer thread
                // failing to start, which is a real resource failure. It is NOT
                // reported as EAGAIN: EAGAIN is what Linux returns on expiry,
                // so a guest would read it as "your 300 ms elapsed" after no
                // time at all, and a caller that loops on EAGAIN would spin.
                // ENOMEM cannot be mistaken for a timeout.
                result = lerr(ENOMEM);
                break;
            }
        }

        let mut d: c_int = 0;
        let rc = sigwait(&waitset, &mut d);
        if rc != 0 {
            // Darwin returns EINTR here when a signal outside the set was
            // handled, which is exactly what Linux reports for the same event.
            result = lerr(if rc == -1 { errno() } else { rc });
            break;
        }
        if d == WAKE_SIG || d == RT_CARRIER {
            continue; // a wakeup; the loop re-evaluates
        }
        let l = lxrt_signo_to_linux(d);
        if l != 0 && sigismember(dset, d) {
            write_info(uinfo, l);
            result = l as c_long;
            break;
        }
        // Something in waitset that is not in dset and not a wakeup cannot
        // happen, but consuming it silently would lose it; go round again.
    }

    timer_disarm(slot_p);
    // A WAKE_SIG the timer sent may still be pending here, and it is left that
    // way deliberately. The code this replaces did sigpending() and then
    // committed to a blocking sigwait() on the strength of it -- and that
    // sigwait could block FOREVER, because between the two calls another
    // waiter's sigwait can dequeue this thread's pending WAKE_SIG (see
    // WAKE_RETRY_NS for the measurement). Reproduced: two threads parked in
    // exactly that sigwait with 2259 of 2259 samples in __sigwait, roughly one
    // run in six of a 48-thread soak.
    //
    // Leaving it pending is harmless, which is why the drain is not replaced
    // with anything. A timeout here is decided by comparing mono_ns() against
    // the deadline, never by the arrival of WAKE_SIG, so a stray one cannot
    // fabricate an early expiry: the loop above treats it as a wakeup, goes
    // round, re-reads the clock and carries on. And a standard signal has at
    // most one pending instance, so it cannot accumulate. On the way out it is
    // either delivered to the no-op handler, discarded because the disposition
    // has gone back to ignore, or left pending for the next wait's sigwait to
    // consume as one extra iteration.
    if rt_leftover {
        // Realtime signals were requeued that this wait did not want. The
        // carrier that would have announced them was consumed by sigwait, so it
        // is re-posted; it is still blocked at this point, so it becomes
        // pending again and is delivered when the guest unblocks it.
        // signal.c's handler already tolerates a carrier with an empty queue.
        //
        // Only when a handler is actually installed. SIGEMT's default action is
        // terminate-and-core, so re-posting one before signal.c has installed
        // the carrier handler would turn an unwanted realtime signal into a
        // crash the moment the mask is restored.
        let mut cur = SigAction::ZERO;
        if sigaction(RT_CARRIER, ptr::null(), &mut cur) == 0
            && cur.sa_handler != SIG_DFL
            && cur.sa_handler != SIG_IGN
        {
            pthread_kill(pthread_self(), RT_CARRIER);
        }
    }

    // pthread_cleanup_pop(1)
    ptr::write_volatile(
        ptr::addr_of_mut!((*self_thread).cleanup_stack),
        (*handler_p).next,
    );
    ((*handler_p).routine)((*handler_p).arg);

    if lxrt_trace_on() {
        fprintf(
            lxrt_trace_stream(),
            cstr!("[lxrt] rt_sigtimedwait(mask=0x%llx, %s) -> %ld\n"),
            lmask,
            if timed {
                cstr!("timed")
            } else {
                cstr!("blocking")
            },
            result,
        );
    }
    result
}
