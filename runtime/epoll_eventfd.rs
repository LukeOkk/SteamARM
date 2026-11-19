// epoll and eventfd on kqueue and pipes.
//
// benchmarks/stage6-steam-gap.txt put this at the head of the P0 list: 46 live
// eventpoll and 49 live eventfd descriptors in Steam's process tree, and
// epoll_pwait at 33334 calls per 10 s, the 4th most frequent syscall on the
// machine. It is the client's own event loop.
//
// kqueue is not epoll, and the gaps are not cosmetic:
//
//   * epoll carries a bitmask per descriptor; kqueue carries a separate knote
//     per (fd, filter). One readable-and-writable fd is two kevents and must be
//     coalesced back into one epoll_event, or the guest sees the same cookie
//     twice in one wait and double-services it.
//   * epoll is LEVEL-triggered unless EPOLLET is given. kqueue is also level
//     triggered unless EV_CLEAR is given -- measured, not assumed: registering
//     EVFILT_READ on a pipe with one unread byte returns the event on every
//     subsequent kevent() call. So the default maps straight across and EPOLLET
//     becomes EV_CLEAR. Getting this backwards is the classic port bug: an
//     event loop that reads a partial buffer would hang forever.
//   * kqueue has no event mask to return. EPOLLERR and EPOLLHUP, which Linux
//     reports whether or not they were requested, have to be reconstructed from
//     EV_EOF and EV_ERROR plus the *type* of the descriptor, because EV_EOF on
//     a pipe means EPOLLHUP while EV_EOF on a socket means EPOLLRDHUP.
//   * a kqueue knote is dropped when its descriptor closes and nothing says so,
//     whereas Linux's close() removes the fd from every epoll set. The
//     descriptor NUMBER is therefore not a safe key on its own; see
//     ep_entry_stale() and lxrt_epoll_fd_closed().
//   * eventfd has no substrate at all. The counter lives here and a pipe
//     carries readiness, so the descriptor stays pollable by kqueue, by the
//     ppoll() the runtime already has, and by select().
//
// Everything the guest receives is a real Darwin descriptor. A synthetic fd
// number would die the first time Steam handed its epoll fd to ppoll() or to
// close(), which it does constantly.
//
// Port notes (from runtime/epoll_eventfd.c): the tables stay static and
// zero-filled, the locks stay pthread mutexes (they are taken across fork by
// pthread_atfork handlers), the interest list stays malloc/realloc/free, and
// nothing here allocates through Rust or can unwind.

#![allow(dead_code)]

use std::cell::UnsafeCell;
use std::ffi::{c_char, c_int, c_long, c_uint, c_void};
use std::mem::{size_of, MaybeUninit};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU32, AtomicU64, Ordering};

// ---------------------------------------------------------------- Darwin ABI

// Darwin <errno.h> (host side; converted with lxrt_errno_to_linux).
const EPERM: c_int = 1;
const ENOENT: c_int = 2;
const EINTR: c_int = 4;
const EBADF: c_int = 9;
const ENOMEM: c_int = 12;
const EFAULT: c_int = 14;
const EEXIST: c_int = 17;
const EINVAL: c_int = 22;
const EMFILE: c_int = 24;
const EAGAIN: c_int = 35;

// <fcntl.h>
const F_GETFD: c_int = 1;
const F_SETFD: c_int = 2;
const F_GETFL: c_int = 3;
const F_SETFL: c_int = 4;
const FD_CLOEXEC: c_int = 1;
const O_RDWR: c_int = 0x0002;
const O_NONBLOCK: c_int = 0x0004;
const O_CREAT: c_int = 0x0200;
const O_EXCL: c_int = 0x0800;
const O_CLOEXEC: c_int = 0x0100_0000;

// <sys/mman.h>
const PROT_READ: c_int = 0x01;
const PROT_WRITE: c_int = 0x02;
const MAP_SHARED: c_int = 0x0001;
const MAP_ANON: c_int = 0x1000;
const MAP_FAILED: *mut c_void = !0usize as *mut c_void;

// <poll.h>
const POLLIN: i16 = 0x0001;
const POLLNVAL: i16 = 0x0020;

// <sys/stat.h>
const S_IFMT: u16 = 0o170000;
const S_IFDIR: u16 = 0o040000;
const S_IFREG: u16 = 0o100000;
const S_IFSOCK: u16 = 0o140000;

// <sys/event.h>
const EVFILT_READ: i16 = -1;
const EVFILT_WRITE: i16 = -2;
const EVFILT_EXCEPT: i16 = -15;
const EV_ADD: u16 = 0x0001;
const EV_DELETE: u16 = 0x0002;
const EV_ENABLE: u16 = 0x0004;
const EV_CLEAR: u16 = 0x0020;
const EV_RECEIPT: u16 = 0x0040;
const EV_ERROR: u16 = 0x4000;
const EV_EOF: u16 = 0x8000;
const NOTE_OOB: u32 = 0x0000_0002;
const KEVENT_FLAG_IMMEDIATE: c_uint = 0x0001;

// <time.h>
const CLOCK_MONOTONIC: c_int = 6;

// <mach/...>
type VmProt = c_int;
const VM_PROT_READ: VmProt = 0x01;
const VM_PROT_WRITE: VmProt = 0x02;
const VM_REGION_BASIC_INFO_64: c_int = 9;
const KERN_SUCCESS: c_int = 0;
const MACH_PORT_NULL: u32 = 0;

// vm_region_basic_info_data_64_t; the header declares it under
// #pragma pack(push, 4).
#[repr(C, packed(4))]
struct VmRegionBasicInfo64 {
    protection: VmProt,
    max_protection: VmProt,
    inheritance: u32,
    shared: c_int,
    reserved: c_int,
    offset: u64,
    behavior: c_int,
    user_wired_count: u16,
}
const _: [(); 36] = [(); size_of::<VmRegionBasicInfo64>()];
const VM_REGION_BASIC_INFO_COUNT_64: u32 =
    (size_of::<VmRegionBasicInfo64>() / size_of::<c_int>()) as u32;

// struct timespec, Darwin arm64: time_t and long are both 64-bit.
#[repr(C)]
#[derive(Clone, Copy)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: c_long,
}
const _: [(); 16] = [(); size_of::<Timespec>()];

// struct kevent, Darwin LP64.
#[repr(C)]
#[derive(Clone, Copy)]
struct KEvent {
    ident: usize,
    filter: i16,
    flags: u16,
    fflags: u32,
    data: isize,
    udata: *mut c_void,
}
const _: [(); 32] = [(); size_of::<KEvent>()];

// struct kevent64_s.
#[repr(C)]
#[derive(Clone, Copy)]
struct KEvent64 {
    ident: u64,
    filter: i16,
    flags: u16,
    fflags: u32,
    data: i64,
    udata: u64,
    ext: [u64; 2],
}
const _: [(); 48] = [(); size_of::<KEvent64>()];

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: i16,
    revents: i16,
}
const _: [(); 8] = [(); size_of::<PollFd>()];

// struct stat, Darwin arm64 (64-bit inodes are the only ABI there).
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

// Darwin pthread_mutex_t: a signature word plus 56 opaque bytes.
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}
const _: [(); 64] = [(); size_of::<PthreadMutex>()];
const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: u32;

    fn __error() -> *mut c_int;
    fn mach_vm_region(
        target_task: u32,
        address: *mut u64,
        size: *mut u64,
        flavor: c_int,
        info: *mut c_int,
        info_count: *mut u32,
        object_name: *mut u32,
    ) -> c_int;
    fn mach_port_deallocate(task: u32, name: u32) -> c_int;

    fn kqueue() -> c_int;
    fn kevent64(
        kq: c_int,
        changelist: *const KEvent64,
        nchanges: c_int,
        eventlist: *mut KEvent64,
        nevents: c_int,
        flags: c_uint,
        timeout: *const Timespec,
    ) -> c_int;
    fn poll(fds: *mut PollFd, count: c_uint, timeout: c_int) -> c_int;
    fn pipe(fds: *mut c_int) -> c_int;
    fn dup(fd: c_int) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn read(fd: c_int, buf: *mut c_void, count: usize) -> isize;
    fn write(fd: c_int, buf: *const c_void, count: usize) -> isize;
    fn fcntl(fd: c_int, cmd: c_int, ...) -> c_int;
    fn fstat(fd: c_int, st: *mut Stat) -> c_int;
    fn ftruncate(fd: c_int, length: i64) -> c_int;
    fn unlink(path: *const c_char) -> c_int;
    fn getpid() -> c_int;
    fn mmap(
        addr: *mut c_void,
        len: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(addr: *mut c_void, len: usize) -> c_int;
    fn malloc(size: usize) -> *mut c_void;
    fn realloc(p: *mut c_void, size: usize) -> *mut c_void;
    fn free(p: *mut c_void);
    fn sched_yield() -> c_int;
    fn clock_gettime(clock: c_int, ts: *mut Timespec) -> c_int;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn atoi(s: *const c_char) -> c_int;
    fn snprintf(buf: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;

    // The rest of the runtime (lxrt.h).
    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_rt_sigprocmask(
        how: c_int,
        uset: *const u64,
        uoldset: *mut u64,
        sigsetsize: usize,
    ) -> c_long;
    fn lxrt_fd_hide(fd: c_int, hide: bool);
    fn lxrt_fd_dup_private(fd: c_int) -> c_int;
    fn lxrt_fd_private(fd: c_int) -> c_int;
    fn lxrt_open_private(path: *const c_char, flags: c_int, mode: c_int) -> c_int;
    fn lxrt_close_private(fd: c_int);
    fn lxrt_host_tmpdir() -> *const c_char;
    fn lxrt_gettid() -> c_int;
    fn lxrt_guest_clock_ns(clock: c_long) -> u64;
    fn lxrt_trace_stream() -> *mut c_void;
}

#[inline(always)]
unsafe fn errno() -> c_int {
    *__error()
}

#[inline(always)]
unsafe fn set_errno(e: c_int) {
    *__error() = e;
}

// LERR(e): never return a raw Darwin errno to a guest.
#[inline(always)]
unsafe fn lerr(e: c_int) -> c_long {
    -lxrt_errno_to_linux(e) as c_long
}

struct Shared<T>(UnsafeCell<T>);
unsafe impl<T> Sync for Shared<T> {}

// ---------------------------------------------------------------- constants
//
// Linux epoll event bits, from include/uapi/linux/eventpoll.h. The first six
// are the poll(2) bits and they happen to agree with Darwin's:
//
//     bit            Linux    Darwin
//     POLLIN         0x0001   0x0001   agree
//     POLLPRI        0x0002   0x0002   agree
//     POLLOUT        0x0004   0x0004   agree
//     POLLERR        0x0008   0x0008   agree
//     POLLHUP        0x0010   0x0010   agree
//     POLLNVAL       0x0020   0x0020   agree
//     POLLRDNORM     0x0040   0x0040   agree
//     POLLRDBAND     0x0080   0x0080   agree
//     POLLWRNORM     0x0100   0x0004   DIVERGE -- Darwin aliases it to POLLOUT
//     POLLWRBAND     0x0200   0x0100   DIVERGE -- Darwin's value is Linux's
//                                      POLLWRNORM, so a pass-through turns a
//                                      band-write request into a normal one
//     POLLRDHUP      0x2000   absent   Darwin has no such bit at all
//
// That is the shape of the bug this project keeps meeting: the low range
// agrees, so the common case works, and the divergence starts exactly where
// the less-travelled code is. Nothing below is passed through; every bit is
// translated by name.
const L_EPOLLIN: u32 = 0x0000_0001;
const L_EPOLLPRI: u32 = 0x0000_0002;
const L_EPOLLOUT: u32 = 0x0000_0004;
const L_EPOLLERR: u32 = 0x0000_0008;
const L_EPOLLHUP: u32 = 0x0000_0010;
const L_EPOLLNVAL: u32 = 0x0000_0020;
const L_EPOLLRDNORM: u32 = 0x0000_0040;
const L_EPOLLRDBAND: u32 = 0x0000_0080;
const L_EPOLLWRNORM: u32 = 0x0000_0100;
const L_EPOLLWRBAND: u32 = 0x0000_0200;
const L_EPOLLMSG: u32 = 0x0000_0400;
const L_EPOLLRDHUP: u32 = 0x0000_2000;
const L_EPOLLEXCLUSIVE: u32 = 0x1000_0000;
const L_EPOLLWAKEUP: u32 = 0x2000_0000;
const L_EPOLLONESHOT: u32 = 0x4000_0000;
const L_EPOLLET: u32 = 0x8000_0000;

const L_EPOLL_READ_BITS: u32 = L_EPOLLIN | L_EPOLLRDNORM | L_EPOLLRDBAND;
const L_EPOLL_WRITE_BITS: u32 = L_EPOLLOUT | L_EPOLLWRNORM | L_EPOLLWRBAND;

// fs/eventpoll.c, EPOLLEXCLUSIVE_OK_BITS: the only bits Linux lets an
// EPOLL_CTL_ADD carry alongside EPOLLEXCLUSIVE. Anything else is EINVAL --
// measured on the 6.17.1 aarch64 guest: EPOLLIN|EPOLLEXCLUSIVE alone returned
// 0, while adding EPOLLONESHOT or EPOLLRDBAND returned -EINVAL.
const L_EPOLL_EXCLUSIVE_OK_BITS: u32 = L_EPOLLIN
    | L_EPOLLOUT
    | L_EPOLLERR
    | L_EPOLLHUP
    | L_EPOLLWAKEUP
    | L_EPOLLET
    | L_EPOLLEXCLUSIVE;

// op values for epoll_ctl. Identical on every Linux architecture.
const L_EPOLL_CTL_ADD: c_int = 1;
const L_EPOLL_CTL_DEL: c_int = 2;
const L_EPOLL_CTL_MOD: c_int = 3;

// The only flag epoll_create1 accepts. Linux spells it O_CLOEXEC, which is
// 0x80000 (0o2000000) on aarch64; Darwin's O_CLOEXEC is 0x1000000. The two are
// never compared, but the guest value must not be handed to a Darwin open().
const L_EPOLL_CLOEXEC: c_int = 0x0008_0000;

// eventfd2 flags. EFD_CLOEXEC and EFD_NONBLOCK are aliases of Linux's
// O_CLOEXEC and O_NONBLOCK, both of which differ from Darwin's:
//     O_CLOEXEC   Linux 0x80000   Darwin 0x1000000
//     O_NONBLOCK  Linux 0x00800   Darwin 0x0000004
// EFD_SEMAPHORE has no open() counterpart and is 1 on every architecture.
const L_EFD_SEMAPHORE: c_int = 0x0000_0001;
const L_EFD_CLOEXEC: c_int = 0x0008_0000;
const L_EFD_NONBLOCK: c_int = 0x0000_0800;

// Linux SIG_SETMASK. Darwin's is 3 -- the whole how/1/2/3 triple is shifted by
// one. lxrt_rt_sigprocmask() takes the Linux numbering because it implements
// the Linux syscall, so 2 is correct here and would be SIG_UNBLOCK on Darwin.
const L_SIG_SETMASK: c_int = 2;

// The guest's sigset_t as the kernel sees it: 64 bits on aarch64. Linux
// rejects any other sigsetsize with EINVAL, so the guest's glibc is entitled to
// rely on that check existing.
const L_KERNEL_SIGSET_BYTES: usize = 8;

// struct epoll_event, aarch64.
//
// include/uapi/linux/eventpoll.h applies __attribute__((packed)) under
// #ifdef __x86_64__ ONLY. Everywhere else, aarch64 included, the struct is
// natural-aligned, so the __u64 sits at offset 8 and the struct is 16 bytes:
//
//     offset 0  uint32_t events
//     offset 4  4 bytes of padding, uninitialised by the kernel
//     offset 8  uint64_t data      (the epoll_data_t union: ptr/fd/u32/u64)
//
// On x86-64 the same struct is 12 bytes with data at offset 4. Borrowing that
// layout here slides every returned cookie by four bytes and the guest
// dereferences half a pointer -- silent, total, and looks like heap corruption.
// The asserts below are the check, not the comment.
#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxEpollEvent {
    events: u32,
    pad_aarch64: u32,
    data: u64,
}
const _: [(); 16] = [(); size_of::<LinuxEpollEvent>()];
const _: [(); 8] = [(); std::mem::offset_of!(LinuxEpollEvent, data)];

#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxTimespec {
    tv_sec: i64,
    tv_nsec: i64,
}

// Sized from the measured census: 46 eventpoll and 49 eventfd descriptors
// across Steam's eleven processes. One runtime is one process, so these are
// generous by roughly 5x; exhaustion returns EMFILE, which is what Linux
// returns when /proc/sys/fs/epoll/max_user_instances is hit.
const MAX_EPOLL: usize = 256;
const MAX_EVENTFD: usize = 256;

// kevent64() is asked for at most this many events per call regardless of the
// guest's maxevents. Returning fewer events than asked for is always legal --
// level-triggered readiness is still there on the next call -- and it bounds
// the transient allocation at 4096 * (48 + 24) bytes.
const KEV_BATCH_MAX: c_int = 4096;

// The most kqueue filters one interest can ever occupy: read, write, except.
// The changelists below are sized 2 * this because EPOLL_CTL_MOD submits a
// delete list and an add list as one atomic changelist.
const EP_MAX_CH: usize = 3;

// ---------------------------------------------------------------- kevent64
//
// Every call on an epoll kqueue is a kevent64(), never a kevent(), because of
// what a zero timeout costs. MEASURED on this machine (macOS 27, M4), 20000
// calls each, nothing ready, one or eight pipe knotes alike:
//
//     kevent(ts = {0, 0})                       median 12000 ns, p99 14.7 us
//     kevent(ts = {0, 1 ns})                    median 12000 ns
//     kevent64(flags = 0, ts = {0, 0})          median 12000 ns
//     kevent64(KEVENT_FLAG_IMMEDIATE, ts NULL)  median   250 ns, p99 334 ns
//     kevent(ts = {0, 0}), one knote READY      median   167 ns
//
// A timespec of zero is not "do not wait": the kernel turns it into a deadline
// of now and parks the thread until the timer fires, 12 us later on an idle
// machine and far longer on a busy one (about 90 us on the main thread of
// Counter-Strike 2, which polls its event queue with epoll_wait(0) every
// frame, benchmarks/stage52 section 4). Only KEVENT_FLAG_IMMEDIATE, which the
// legacy kevent() cannot pass, skips the sleep.
//
// And a kqueue is bound to the first interface used on it: MEASURED, kevent()
// on a queue that has seen a kevent64() returns EINVAL, and kevent64() on one
// that has seen a kevent() returns EINVAL too. Hence all of them, including
// the changelists, go through here; the EV_SET builders keep struct kevent
// and are converted at the call.
#[inline(always)]
unsafe fn kev_to64(k: *const KEvent, o: *mut KEvent64) {
    (*o).ident = (*k).ident as u64;
    (*o).filter = (*k).filter;
    (*o).flags = (*k).flags;
    (*o).fflags = (*k).fflags;
    (*o).data = (*k).data as i64;
    (*o).udata = (*k).udata as usize as u64;
    (*o).ext[0] = 0;
    (*o).ext[1] = 0;
}

// EV_SET().
#[inline(always)]
unsafe fn ev_set(
    k: *mut KEvent,
    ident: usize,
    filter: i16,
    flags: u16,
    fflags: u32,
    data: isize,
    udata: *mut c_void,
) {
    (*k).ident = ident;
    (*k).filter = filter;
    (*k).flags = flags;
    (*k).fflags = fflags;
    (*k).data = data;
    (*k).udata = udata;
}

#[inline(always)]
unsafe fn kev_from64(k: *const KEvent64, o: *mut KEvent) {
    ev_set(
        o,
        (*k).ident as usize,
        (*k).filter,
        (*k).flags,
        (*k).fflags,
        (*k).data as isize,
        (*k).udata as usize as *mut c_void,
    );
}

// Submit a changelist of at most 2 * EP_MAX_CH entries and collect up to
// `nout` receipts (EV_RECEIPT) without waiting. Returns what kevent64 did.
unsafe fn ep_kevent_now(
    kq: c_int,
    ch: *const KEvent,
    n: c_int,
    out: *mut KEvent,
    nout: c_int,
) -> c_int {
    let mut c = MaybeUninit::<[KEvent64; 2 * EP_MAX_CH]>::uninit();
    let mut o = MaybeUninit::<[KEvent64; 2 * EP_MAX_CH]>::uninit();
    let c = c.as_mut_ptr().cast::<KEvent64>();
    let o = o.as_mut_ptr().cast::<KEvent64>();
    if n < 0 || n > (2 * EP_MAX_CH) as c_int || nout < 0 || nout > (2 * EP_MAX_CH) as c_int {
        set_errno(EINVAL);
        return -1;
    }
    let mut i = 0;
    while i < n {
        kev_to64(ch.add(i as usize), c.add(i as usize));
        i += 1;
    }
    let r = kevent64(kq, c, n, o, nout, KEVENT_FLAG_IMMEDIATE, ptr::null());
    let mut i = 0;
    while i < r && i < nout {
        kev_from64(o.add(i as usize), out.add(i as usize));
        i += 1;
    }
    r
}

// ---------------------------------------------------------------- guest memory
//
// Rule: a bad guest pointer produces EFAULT, never a SIGSEGV in the host. There
// is no guest/host boundary to catch a fault at -- the guest runs in this
// process -- so the range is checked against the VM map before it is touched.
// One mach_vm_region() trap per epoll_pwait against 33334 calls per 10 s is
// ~3.3k traps/s, well under the cost of the kevent() the call is about to make.
// Measured on this machine: mach_vm_region costs 682 ns per call.
unsafe fn range_ok(p: *const c_void, len: usize, need_write: bool) -> bool {
    if p.is_null() {
        return false;
    }
    if len == 0 {
        return true;
    }

    let mut a = p as usize as u64;
    let end = a.wrapping_add(len as u64);
    if end < a {
        // wrapped: not a range at all
        return false;
    }

    while a < end {
        let mut ra = a;
        let mut rs: u64 = 0;
        let mut info = MaybeUninit::<VmRegionBasicInfo64>::uninit();
        let mut cnt = VM_REGION_BASIC_INFO_COUNT_64;
        let mut obj = MACH_PORT_NULL;

        // mach_vm_region reports the first region at or AFTER `ra`, so a
        // returned start past the probe address means the probe address is in
        // a hole.
        if mach_vm_region(
            mach_task_self_,
            &mut ra,
            &mut rs,
            VM_REGION_BASIC_INFO_64,
            info.as_mut_ptr().cast::<c_int>(),
            &mut cnt,
            &mut obj,
        ) != KERN_SUCCESS
        {
            return false;
        }
        if obj != MACH_PORT_NULL {
            mach_port_deallocate(mach_task_self_, obj);
        }
        if ra > a || rs == 0 {
            return false;
        }
        let protection = ptr::addr_of!((*info.as_ptr()).protection).read_unaligned();
        if protection & VM_PROT_READ == 0 {
            return false;
        }
        if need_write && protection & VM_PROT_WRITE == 0 {
            return false;
        }
        a = ra.wrapping_add(rs);
    }
    true
}

// ---------------------------------------------------------------- epoll state

#[repr(C)]
#[derive(Clone, Copy)]
struct EpInterest {
    fd: c_int, // guest fd; the kqueue ident for the read/except knote
    wfd: c_int, // ident for the write knote. Equal to fd except for
    // our own eventfds -- see eventfd_write_end().
    events: u32,     // the Linux mask exactly as registered
    data: u64,       // epoll_data_t, returned to the guest verbatim
    is_socket: bool, // fstat'd once at ADD. Decides whether EV_EOF means
    // EPOLLHUP (pipe: writer gone) or EPOLLRDHUP (socket:
    // peer sent FIN, the other half may still be alive).
    oneshot_fired: bool, // EPOLLONESHOT and this interest has already
    // reported. Linux disarms the whole INTEREST after
    // one reported event, not the one filter that
    // fired; see ep_changes() for the measurement.
    // Identity of the open file behind `fd` at ADD time. The descriptor NUMBER
    // alone is not a key: kqueue drops the knote when the guest closes the fd
    // and tells nobody, so a recycled number would keep matching a dead entry.
    // Darwin hands a pipe a random 64-bit st_ino and a socket a distinct one --
    // measured: two pipes created back to back got 802054470155456830 and
    // 8646670859292934534, and a dup() of the first kept its ino, which is
    // exactly the "same open file" answer wanted here.
    st_dev: i32,
    st_ino: u64,
}

// What each kqueue filter can actually produce once kev_to_epoll() has
// translated it. The arming decision in ep_changes() MUST agree with these
// sets: a LEVEL-triggered knote whose bits the translation then masks away to
// nothing fires on every kevent(), and pwait_common()'s honest "re-wait rather
// than fake a timeout" loop turns into a busy spin. Measured before the fix,
// deterministic over three runs: EPOLLRDBAND alone on a readable pipe burned
// 409 ms of CPU in 400 ms of wall clock inside one epoll_pwait(-1) and never
// returned; EPOLLWRBAND alone on a socketpair burned 300 ms of CPU inside a
// single epoll_pwait(300).
//
// The sets are measured, not assumed. On the 6.17.1 aarch64 guest:
//     pipe readable,        EPOLLRDBAND only -> epoll_wait returns 0 events
//     unix socket readable, EPOLLRDBAND only -> 0 events
//     pipe writable,        EPOLLWRBAND only -> 0 events
//     unix socket writable, EPOLLWRBAND only -> 1 event, 0x200
// So RDBAND is never set for ordinary readability anywhere, and WRBAND is set
// by unix_poll/tcp_poll but not by pipe_poll. Emitting RDBAND to stop the spin
// would invent an event Linux never delivers; the arming rule is the fix and
// the emitted WRBAND below is simply what Linux does.
const L_EPOLL_READ_EMIT: u32 = L_EPOLLIN | L_EPOLLRDNORM;

#[inline(always)]
unsafe fn write_emit_bits(i: *const EpInterest) -> u32 {
    L_EPOLLOUT | L_EPOLLWRNORM | if (*i).is_socket { L_EPOLLWRBAND } else { 0 }
}

const EP_ALIASES: usize = 8;

#[repr(C)]
struct EpInst {
    // `used` rather than a sentinel in `kq`: the table is static, so a free
    // slot is zero-filled, and zero is fd 0. Testing kq for a sentinel value
    // made every slot look occupied by stdin and epoll_create1 returned EMFILE
    // on the very first call.
    used: bool,
    kq: c_int,   // guest-visible descriptor
    refs: c_int, // threads inside kevent() on this instance
    dead: bool,  // closed, waiting for refs to drain
    v: *mut EpInterest,
    n: c_int,
    cap: c_int,
    // dup()s of kq (lxrt_epoll_dup): on Linux every one is the same epoll
    // instance, and each is also a descriptor of the same kqueue here.
    alias: [c_int; EP_ALIASES],
    nalias: c_int,
}

static G_EP: Shared<MaybeUninit<[EpInst; MAX_EPOLL]>> =
    Shared(UnsafeCell::new(MaybeUninit::zeroed()));
static G_EP_LIVE: AtomicI32 = AtomicI32::new(0);
static G_EP_LOCK: Shared<PthreadMutex> = Shared(UnsafeCell::new(PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
}));

#[inline(always)]
fn g_ep(i: usize) -> *mut EpInst {
    unsafe { G_EP.0.get().cast::<EpInst>().add(i) }
}

#[inline(always)]
unsafe fn ep_lock() {
    let _ = pthread_mutex_lock(G_EP_LOCK.0.get());
}

#[inline(always)]
unsafe fn ep_unlock() {
    let _ = pthread_mutex_unlock(G_EP_LOCK.0.get());
}

// LXRT_FORK_SAFE(epoll_eventfd_g_ep_lock, g_ep_lock): the mutex is taken before
// fork and released after it in both parent and child (the child's only thread
// is the one that took it).
extern "C" fn epoll_eventfd_g_ep_lock_fork_prepare() {
    unsafe { ep_lock() }
}

extern "C" fn epoll_eventfd_g_ep_lock_fork_release() {
    unsafe { ep_unlock() }
}

// ---------------------------------------------------------------- eventfd state

// Guest descriptors that alias one eventfd. dup(), dup2(), dup3() and
// fcntl(F_DUPFD) all produce one, and on Linux every alias is the same eventfd:
// measured on the guest, dup(eventfd(5)) then read(dup, buf, 8) returns 8 bytes
// and the value 5. Before aliases were tracked, the same sequence here returned
// 1 byte holding 101 -- the ASCII 'e' readiness token -- and ate the token, so
// the descriptor became permanently unreadable to poll/select/kqueue while the
// counter still said 5.
//
// Sixteen is a documented cap, not a guess at infinity: it keeps the table
// allocation-free so lxrt_eventfd_dup() cannot fail under memory pressure while
// holding the global eventfd lock. A seventeenth alias is not tracked and falls
// back to the raw pipe behaviour described above; nothing in the measured Steam
// census comes close.
const EV_MAX_ALIAS: usize = 16;

// The counter and the token flag live in memory this process shares with the
// children it forks (MAP_SHARED|MAP_ANON stays shared across Darwin's fork()),
// because on Linux a forked child's eventfd IS the parent's eventfd. With the
// counter in process memory the two drifted apart while the token pipe was
// shared: Qt's QProcess (forkfd, qtbase src/3rdparty/forkfd/forkfd.c) makes
// the child wait in eventfd_read for the parent's "all clear" eventfd_write;
// the child's copy stayed 0 while the parent's token kept the pipe readable,
// and the child spun at 100% CPU for ever, so no QProcess ever started
// (Prism Launcher's updater and Java checks; benchmarks/stage24-minecraft-
// prism.txt, tests/elf/eventfd_fork.c).
//
// `lock` serialises count/armed and the token byte across processes. It is
// only ever taken with g_ev_lock held, so no thread of this process can hold
// it across fork() (g_ev_lock is held there, LXRT_FORK_SAFE), and it is held
// for one counter update plus at most one non-blocking token write or read.
// `procs` counts the processes that have the object (1, plus 1 per fork that
// inherited it); a process that execs or exits without closing leaves it
// high, so it only decides the courtesy wake-up in lxrt_eventfd_close().
//
// The layout is shared between processes (fork, SCM_RIGHTS) and must stay
// exactly what the C struct was: atomic_flag at 0, count at 8, armed at 16,
// procs at 20, magic at 24, semaphore at 28, pipe_dev at 32, pipe_ino at 40.
#[repr(C)]
struct EvShared {
    lock: AtomicBool, // atomic_flag
    count: u64,
    armed: bool, // exactly one token byte is sitting in the pipe.
    // Invariant: armed == (count > 0).
    procs: AtomicI32,
    // For a process that receives the eventfd over SCM_RIGHTS (socket.c):
    // what it is and which pipe is its token channel.
    magic: u32,
    semaphore: bool,
    pipe_dev: u64,
    pipe_ino: u64,
}
const _: [(); 48] = [(); size_of::<EvShared>()];
const _: [(); 8] = [(); std::mem::offset_of!(EvShared, count)];
const _: [(); 16] = [(); std::mem::offset_of!(EvShared, armed)];
const _: [(); 20] = [(); std::mem::offset_of!(EvShared, procs)];
const _: [(); 24] = [(); std::mem::offset_of!(EvShared, magic)];
const _: [(); 28] = [(); std::mem::offset_of!(EvShared, semaphore)];
const _: [(); 32] = [(); std::mem::offset_of!(EvShared, pipe_dev)];
const _: [(); 40] = [(); std::mem::offset_of!(EvShared, pipe_ino)];

const EV_MAGIC: u32 = 0x6576_6664; // "evfd"
const EV_SHARED_BYTES: usize = 16384; // one host page per eventfd

unsafe fn ev_sh_lock(s: *mut EvShared) {
    let mut spins: c_uint = 0;
    // atomic_flag_test_and_set_explicit(..., memory_order_acquire)
    while (*s).lock.swap(true, Ordering::Acquire) {
        if spins < 64 {
            // __builtin_arm_yield()
            core::arch::asm!("yield", options(nomem, nostack, preserves_flags));
        } else {
            sched_yield();
        }
        spins = spins.wrapping_add(1);
    }
}

#[inline(always)]
unsafe fn ev_sh_unlock(s: *mut EvShared) {
    (*s).lock.store(false, Ordering::Release);
}

#[repr(C)]
struct EvObj {
    used: bool, // see struct EpInst: a zero-filled free slot would
    // otherwise claim to be descriptor 0
    pr: c_int, // PRIVATE read end of the token pipe. Never handed to
    // the guest, so no guest close() can take the drain
    // path away -- every guest-visible fd below is a dup()
    // of this one and therefore the same pipe.
    wfd: c_int, // our write end. Never handed to the guest, and the
    // ident used for EPOLLOUT interest -- measured:
    // EVFILT_WRITE registers successfully on a pipe's READ
    // end and then never fires, so watching the read end
    // for writability would silently wedge any guest
    // waiting on EPOLLOUT.
    fds: [c_int; EV_MAX_ALIAS], // guest-visible descriptors for this eventfd
    nfds: c_int,
    shm_fd: c_int, // the file behind `sh` (private), -1 if anonymous: it is
    // what another process needs, with `wfd`, to share the
    // eventfd it receives over SCM_RIGHTS (Wine's esync)
    sh: *mut EvShared, // count and armed, shared with forked children (above).
    // armed == (count > 0) is what makes the descriptor
    // readable to kqueue, poll and select without them
    // knowing anything about the counter.
    semaphore: bool,
    nonblock: bool,
    refs: c_int,
    dead: bool,
}

static G_EV: Shared<MaybeUninit<[EvObj; MAX_EVENTFD]>> =
    Shared(UnsafeCell::new(MaybeUninit::zeroed()));
static G_EV_LOCK: Shared<PthreadMutex> = Shared(UnsafeCell::new(PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
}));

#[inline(always)]
fn g_ev(i: usize) -> *mut EvObj {
    unsafe { G_EV.0.get().cast::<EvObj>().add(i) }
}

#[inline(always)]
unsafe fn ev_lock() {
    let _ = pthread_mutex_lock(G_EV_LOCK.0.get());
}

#[inline(always)]
unsafe fn ev_unlock() {
    let _ = pthread_mutex_unlock(G_EV_LOCK.0.get());
}

// LXRT_FORK_SAFE(epoll_eventfd_g_ev_lock, g_ev_lock)
extern "C" fn epoll_eventfd_g_ev_lock_fork_prepare() {
    unsafe { ev_lock() }
}

extern "C" fn epoll_eventfd_g_ev_lock_fork_release() {
    unsafe { ev_unlock() }
}

// A forked child has every eventfd of its parent, counter included (the
// shared page is inherited shared): count it as one more owner BEFORE the
// fork, in the parent. Counted in the child instead, the parent could close
// its copy before the child had run at all and think itself the sole owner:
// forkfd's parent writes 42 and closes at once, and the reset that followed
// left the child reading 0 for ever (MEASURED in Prism Launcher's QProcess).
// This prepare handler runs before LXRT_FORK_SAFE's (handlers registered
// later prepare first). A fork that fails leaves the count one high, which
// only costs the courtesy reset in lxrt_eventfd_close().
extern "C" fn ev_fork_prepare() {
    unsafe {
        ev_lock();
        for i in 0..MAX_EVENTFD {
            let o = g_ev(i);
            if (*o).used && !(*o).dead && !(*o).sh.is_null() {
                (*(*o).sh).procs.fetch_add(1, Ordering::Relaxed);
            }
        }
        ev_unlock();
    }
}

// The C file had three constructors: the two LXRT_FORK_SAFE registrations at
// priority 200, in file order (g_ep_lock, then g_ev_lock), and
// ev_fork_register at priority 201. One initializer registers all three in
// that same order, which is what keeps ev_fork_prepare ahead of the g_ev_lock
// prepare handler at fork time.
extern "C" fn epoll_eventfd_fork_register() {
    unsafe {
        let _ = pthread_atfork(
            Some(epoll_eventfd_g_ep_lock_fork_prepare),
            Some(epoll_eventfd_g_ep_lock_fork_release),
            Some(epoll_eventfd_g_ep_lock_fork_release),
        );
        let _ = pthread_atfork(
            Some(epoll_eventfd_g_ev_lock_fork_prepare),
            Some(epoll_eventfd_g_ev_lock_fork_release),
            Some(epoll_eventfd_g_ev_lock_fork_release),
        );
        let _ = pthread_atfork(Some(ev_fork_prepare), None, None);
    }
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static EPOLL_EVENTFD_FORK_REGISTER: extern "C" fn() = epoll_eventfd_fork_register;

// lxrt_eventfd_is() sits in front of every read(63) and write(64) in the
// process, so it cannot take a lock. A bitmap over the low fd space answers
// almost every call with one relaxed load; descriptors above it fall back to a
// locked scan, which only happens if the guest has raised RLIMIT_NOFILE past
// 64k and then landed an eventfd up there. Every alias sets its own bit.
const EVMAP_FDS: c_int = 65536;
static G_EVMAP: [AtomicU64; (EVMAP_FDS / 64) as usize] =
    [const { AtomicU64::new(0) }; (EVMAP_FDS / 64) as usize];
static G_EV_LIVE: AtomicI32 = AtomicI32::new(0);

#[inline(always)]
unsafe fn evmap_word(fd: c_int) -> &'static AtomicU64 {
    // fd is already range-checked by the callers.
    &*G_EVMAP.as_ptr().add((fd >> 6) as usize)
}

unsafe fn evmap_set(fd: c_int, on: bool) {
    if fd < 0 || fd >= EVMAP_FDS {
        return;
    }
    let bit = 1u64 << (fd & 63);
    if on {
        evmap_word(fd).fetch_or(bit, Ordering::Relaxed);
    } else {
        evmap_word(fd).fetch_and(!bit, Ordering::Relaxed);
    }
}

#[inline(always)]
unsafe fn evmap_test(fd: c_int) -> bool {
    if fd < 0 || fd >= EVMAP_FDS {
        return false;
    }
    let w = evmap_word(fd).load(Ordering::Relaxed);
    (w >> (fd & 63)) & 1 != 0
}

// Caller holds g_ev_lock.
unsafe fn ev_has_fd(o: *const EvObj, fd: c_int) -> bool {
    let fds = ptr::addr_of!((*o).fds).cast::<c_int>();
    let mut i = 0;
    while i < (*o).nfds {
        if *fds.add(i as usize) == fd {
            return true;
        }
        i += 1;
    }
    false
}

// Caller holds g_ev_lock. Keyed on any alias, not just the first descriptor
// handed out, so a dup'd eventfd still resolves to its counter.
unsafe fn ev_find(fd: c_int) -> *mut EvObj {
    for i in 0..MAX_EVENTFD {
        let o = g_ev(i);
        if (*o).used && !(*o).dead && ev_has_fd(o, fd) {
            return o;
        }
    }
    ptr::null_mut()
}

// Caller holds g_ev_lock. A slot stays occupied while a thread is still
// blocked inside it, so its fd numbers cannot be handed to a new eventfd
// underneath that thread. The private pipe ends are closed here rather than in
// lxrt_eventfd_close() for the same reason: a thread parked in poll() on `pr`
// must still have a valid descriptor to be parked on.
unsafe fn ev_release(o: *mut EvObj) {
    if (*o).dead && (*o).refs == 0 {
        if (*o).pr >= 0 {
            lxrt_fd_hide((*o).pr, false);
            close((*o).pr);
        }
        if (*o).wfd >= 0 {
            lxrt_fd_hide((*o).wfd, false);
            close((*o).wfd);
        }
        (*o).pr = -1;
        (*o).wfd = -1;
        if (*o).shm_fd >= 0 {
            lxrt_close_private((*o).shm_fd);
        }
        (*o).shm_fd = -1;
        if !(*o).sh.is_null() {
            munmap((*o).sh.cast::<c_void>(), EV_SHARED_BYTES); // this process's view only
        }
        (*o).sh = ptr::null_mut();
        (*o).nfds = 0;
        (*o).dead = false;
        (*o).used = false;
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_eventfd_is(fd: c_int) -> bool {
    if fd < 0 {
        return false;
    }
    if evmap_test(fd) {
        return true;
    }
    if fd < EVMAP_FDS || G_EV_LIVE.load(Ordering::Relaxed) == 0 {
        return false;
    }
    ev_lock();
    let found = !ev_find(fd).is_null();
    ev_unlock();
    found
}

// The fd that carries writability for an eventfd, or -1 if `fd` is not one.
unsafe fn eventfd_write_end(fd: c_int) -> c_int {
    if !lxrt_eventfd_is(fd) {
        return -1;
    }
    ev_lock();
    let o = ev_find(fd);
    let w = if !o.is_null() { (*o).wfd } else { -1 };
    ev_unlock();
    w
}

// ---------------------------------------------------------------- eventfd2

// `static atomic_uint seq` of lxrt_eventfd2().
static EVENTFD2_SEQ: AtomicU32 = AtomicU32::new(0);

#[no_mangle]
pub unsafe extern "C" fn lxrt_eventfd2(initval: c_uint, lflags: c_int) -> c_long {
    // Linux validates the flag word and returns EINVAL for anything it does not
    // know, so a guest probing for a newer flag gets a truthful answer.
    let known = L_EFD_SEMAPHORE | L_EFD_CLOEXEC | L_EFD_NONBLOCK;
    if lflags & !known != 0 {
        return lerr(EINVAL);
    }

    if G_EV_LIVE.load(Ordering::Relaxed) >= MAX_EVENTFD as c_int {
        return lerr(EMFILE);
    }

    let mut p: [c_int; 2] = [0; 2];
    if pipe(p.as_mut_ptr()) != 0 {
        return lerr(errno());
    }

    // The guest gets a dup of the read end, never p[0] itself. That costs one
    // descriptor per eventfd and buys the alias table above a drain path the
    // guest cannot close: if the guest dup()s its eventfd and then closes the
    // original, the counter and the token pipe are still reachable from here.
    let g = dup(p[0]);
    if g < 0 {
        let e = errno();
        close(p[0]);
        close(p[1]);
        return lerr(e);
    }

    // Darwin has no pipe2(), so the flags are applied afterwards. The window in
    // between is not a fork hazard here: lxrt_fork() is the only thing in the
    // runtime that forks, and it is a guest syscall, not a background thread.
    if lflags & L_EFD_CLOEXEC != 0 {
        // FD_CLOEXEC is 1 on both systems; it is the O_CLOEXEC *open flag* that
        // differs (0x80000 vs 0x1000000), and that one is never used here.
        fcntl(g, F_SETFD, FD_CLOEXEC);
    }
    if lflags & L_EFD_NONBLOCK != 0 {
        // Kept for fidelity if the guest ever does F_GETFL on the descriptor.
        // The counter path does not consult it -- o->nonblock does. O_NONBLOCK
        // lives on the open file description, which the dup shares with p[0],
        // so this also makes our own drain non-blocking. That is harmless: the
        // drain is poll-gated regardless.
        let fl = fcntl(g, F_GETFL, 0 as c_int);
        if fl >= 0 {
            fcntl(g, F_SETFL, fl | O_NONBLOCK);
        }
    }
    // Our write end is always non-blocking, unconditionally and regardless of
    // what the guest asked for. It is never visible to the guest, so there is no
    // fidelity to lose, and it means the token write in ev_sync_token() can
    // never block with the table lock held -- which would wedge every eventfd
    // in the process, not just this one.
    {
        let fl = fcntl(p[1], F_GETFL, 0 as c_int);
        if fl >= 0 {
            fcntl(p[1], F_SETFL, fl | O_NONBLOCK);
        }
    }

    // Zero-filled: an unlocked lock, count 0, not armed. Behind an unlinked
    // file, so another process can map it too (lxrt_eventfd_adopt); behind
    // anonymous memory, shared with forked children only, if no file can be
    // made.
    let mut shm_fd: c_int;
    {
        let mut path = MaybeUninit::<[c_char; 600]>::uninit();
        let path = path.as_mut_ptr().cast::<c_char>();
        snprintf(
            path,
            600,
            c"%s/lxrt-ev-%d-%u".as_ptr(),
            lxrt_host_tmpdir(),
            getpid(),
            EVENTFD2_SEQ.fetch_add(1, Ordering::SeqCst) as c_uint,
        );
        shm_fd = lxrt_open_private(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0o600);
        if shm_fd >= 0 {
            unlink(path);
            if ftruncate(shm_fd, EV_SHARED_BYTES as i64) != 0 {
                lxrt_close_private(shm_fd);
                shm_fd = -1;
            }
        }
    }
    let sh = if shm_fd >= 0 {
        mmap(
            ptr::null_mut(),
            EV_SHARED_BYTES,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            shm_fd,
            0,
        )
    } else {
        mmap(
            ptr::null_mut(),
            EV_SHARED_BYTES,
            PROT_READ | PROT_WRITE,
            MAP_SHARED | MAP_ANON,
            -1,
            0,
        )
    };
    if sh == MAP_FAILED {
        if shm_fd >= 0 {
            lxrt_close_private(shm_fd);
        }
        // Read after the close, as the C did.
        let e = errno();
        close(g);
        close(p[0]);
        close(p[1]);
        return lerr(e);
    }
    let sh = sh.cast::<EvShared>();
    (*sh).procs.store(1, Ordering::Relaxed); // atomic_init
    {
        let mut ps = MaybeUninit::<Stat>::uninit();
        if fstat(p[0], ps.as_mut_ptr()) == 0 {
            let ps = ps.as_ptr();
            (*sh).pipe_dev = (*ps).st_dev as u64;
            (*sh).pipe_ino = (*ps).st_ino;
        }
        (*sh).semaphore = (lflags & L_EFD_SEMAPHORE) != 0;
        (*sh).magic = EV_MAGIC;
    }

    ev_lock();
    let mut o: *mut EvObj = ptr::null_mut();
    for i in 0..MAX_EVENTFD {
        if !(*g_ev(i)).used {
            o = g_ev(i);
            break;
        }
    }
    if o.is_null() {
        ev_unlock();
        munmap(sh.cast::<c_void>(), EV_SHARED_BYTES);
        if shm_fd >= 0 {
            lxrt_close_private(shm_fd);
        }
        close(g);
        close(p[0]);
        close(p[1]);
        return lerr(EMFILE);
    }
    (*o).used = true;
    (*o).pr = p[0];
    (*o).wfd = p[1];
    // The pipe ends are the runtime's: /proc/self/fd shows only `g`.
    lxrt_fd_hide(p[0], true);
    lxrt_fd_hide(p[1], true);
    (*o).fds[0] = g;
    (*o).nfds = 1;
    (*o).shm_fd = shm_fd;
    (*o).sh = sh;
    (*sh).count = initval as u64;
    (*o).semaphore = (lflags & L_EFD_SEMAPHORE) != 0;
    (*o).nonblock = (lflags & L_EFD_NONBLOCK) != 0;
    (*sh).armed = false;
    (*o).dead = false;
    (*o).refs = 0;
    if (*sh).count > 0 {
        // Restore the invariant immediately: a non-zero initval means the
        // descriptor is already readable before the guest touches it.
        if write((*o).wfd, c"e".as_ptr().cast::<c_void>(), 1) == 1 {
            (*sh).armed = true;
        }
    }
    evmap_set(g, true);
    G_EV_LIVE.fetch_add(1, Ordering::Relaxed);
    ev_unlock();
    g as c_long
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_eventfd_dup(oldfd: c_int, newfd: c_int) {
    if oldfd < 0 || newfd < 0 || oldfd == newfd {
        return;
    }
    if G_EV_LIVE.load(Ordering::Relaxed) == 0 {
        return;
    }
    // Cheap reject for the overwhelming majority of dup()s in the process: a
    // descriptor this module owns always has its evmap bit set.
    if oldfd < EVMAP_FDS && !evmap_test(oldfd) {
        return;
    }

    ev_lock();
    let o = ev_find(oldfd);
    if !o.is_null() && !ev_has_fd(o, newfd) && (*o).nfds < EV_MAX_ALIAS as c_int {
        let fds = ptr::addr_of_mut!((*o).fds).cast::<c_int>();
        *fds.add((*o).nfds as usize) = newfd;
        (*o).nfds += 1;
        evmap_set(newfd, true);
    }
    // Past EV_MAX_ALIAS the alias is not recorded and reads on it reach the raw
    // pipe. See the comment on EV_MAX_ALIAS: this is a stated cap, not a
    // silently swallowed failure, and 16 aliases of one eventfd is already far
    // outside anything the Steam census showed.
    ev_unlock();
}

// What travels with an eventfd sent over SCM_RIGHTS (socket.c): the file
// behind its counter and the write end of its token pipe, both private
// descriptors of this process. 0 if `fd` is not an eventfd that can be shared.
#[no_mangle]
pub unsafe extern "C" fn lxrt_eventfd_companions(
    fd: c_int,
    shm_fd: *mut c_int,
    wfd: *mut c_int,
) -> c_int {
    if !lxrt_eventfd_is(fd) {
        return 0;
    }
    ev_lock();
    let o = ev_find(fd);
    let ok = !o.is_null() && (*o).shm_fd >= 0 && (*o).wfd >= 0;
    if ok {
        *shm_fd = (*o).shm_fd;
        *wfd = (*o).wfd;
    }
    ev_unlock();
    ok as c_int
}

// An eventfd received over SCM_RIGHTS with its companions: `guest_fd` is the
// token pipe's read end the guest got, `shm_fd` and `wfd` the sender's
// companions (now this process's). Makes `guest_fd` this process's view of
// the same eventfd -- the same counter, the same pipe -- so read, write and
// poll on it behave as on Linux, where it simply is the same eventfd. Takes
// `shm_fd` and `wfd` either way. Wine's esync: wineserver makes the eventfd
// of every synchronization object and sends it to each client.
#[no_mangle]
pub unsafe extern "C" fn lxrt_eventfd_adopt(guest_fd: c_int, shm_fd: c_int, wfd: c_int) -> bool {
    let map = mmap(
        ptr::null_mut(),
        EV_SHARED_BYTES,
        PROT_READ | PROT_WRITE,
        MAP_SHARED,
        shm_fd,
        0,
    );
    let sh = map.cast::<EvShared>();
    let mut gs = MaybeUninit::<Stat>::uninit();
    // Short-circuit order as in C: mapping, magic, fstat, identity.
    if map == MAP_FAILED
        || (*sh).magic != EV_MAGIC
        || fstat(guest_fd, gs.as_mut_ptr()) != 0
        || (*gs.as_ptr()).st_dev as u64 != (*sh).pipe_dev
        || (*gs.as_ptr()).st_ino != (*sh).pipe_ino
    {
        if map != MAP_FAILED {
            munmap(map, EV_SHARED_BYTES);
        }
        close(shm_fd);
        close(wfd);
        return false;
    }
    let pr = lxrt_fd_dup_private(guest_fd); // the drain path the guest cannot close
    let w = lxrt_fd_private(wfd);
    let m = lxrt_fd_private(shm_fd);
    ev_lock();
    let mut o: *mut EvObj = ptr::null_mut();
    let mut i = 0;
    while i < MAX_EVENTFD && o.is_null() {
        if !(*g_ev(i)).used {
            o = g_ev(i);
        }
        i += 1;
    }
    if o.is_null() || pr < 0 {
        ev_unlock();
        munmap(map, EV_SHARED_BYTES);
        if pr >= 0 {
            lxrt_close_private(pr);
        }
        lxrt_close_private(w);
        lxrt_close_private(m);
        return false;
    }
    (*o).used = true;
    (*o).pr = pr;
    (*o).wfd = w;
    (*o).shm_fd = m;
    (*o).sh = sh;
    (*o).fds[0] = guest_fd;
    (*o).nfds = 1;
    (*o).semaphore = (*sh).semaphore;
    let fl = fcntl(guest_fd, F_GETFL, 0 as c_int);
    (*o).nonblock = fl >= 0 && (fl & O_NONBLOCK) != 0;
    (*o).dead = false;
    (*o).refs = 0;
    (*sh).procs.fetch_add(1, Ordering::Relaxed);
    evmap_set(guest_fd, true);
    G_EV_LIVE.fetch_add(1, Ordering::Relaxed);
    ev_unlock();
    true
}

// Caller holds g_ev_lock and o->sh's lock. Restores the invariant armed ==
// (count > 0), which is the whole reason the descriptor is pollable: kqueue,
// poll() and select() see a pipe with a byte in it and know nothing about the
// counter.
unsafe fn ev_sync_token(o: *mut EvObj) {
    let s = (*o).sh;
    if (*s).count > 0 && !(*s).armed {
        if write((*o).wfd, c"e".as_ptr().cast::<c_void>(), 1) == 1 {
            (*s).armed = true;
        }
    } else if (*s).count == 0 && (*s).armed {
        // The drain reads from our PRIVATE read end, so it cannot be affected
        // by anything the guest does to its own aliases. It is still gated on a
        // zero-timeout poll first: under the invariant the byte is always
        // there, and the guard exists because the alternative -- if something
        // ever consumed the token behind our back -- is a blocking read holding
        // the global eventfd lock, and that is a deadlock rather than a wrong
        // answer.
        let mut pfd = PollFd {
            fd: (*o).pr,
            events: POLLIN,
            revents: 0,
        };
        if poll(&mut pfd, 1, 0) == 1 && (pfd.revents & POLLIN) != 0 {
            let mut c: c_char = 0;
            if read((*o).pr, ptr::addr_of_mut!(c).cast::<c_void>(), 1) == 1 {
                (*s).armed = false;
                return;
            }
        }
        (*s).armed = false;
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_eventfd_read(fd: c_int, buf: *mut c_void, n: usize) -> c_long {
    // Linux: a read of fewer than 8 bytes is EINVAL, checked before anything
    // else, so a short buffer never consumes the counter.
    if n < size_of::<u64>() {
        return lerr(EINVAL);
    }
    if !range_ok(buf, size_of::<u64>(), true) {
        return lerr(EFAULT);
    }

    loop {
        ev_lock();
        let o = ev_find(fd);
        if o.is_null() {
            ev_unlock();
            return lerr(EBADF);
        }
        let sh = (*o).sh;
        ev_sh_lock(sh);
        if (*sh).count > 0 {
            let v: u64;
            if (*o).semaphore {
                // EFD_SEMAPHORE: hand back 1 and decrement by 1.
                v = 1;
                (*sh).count -= 1;
            } else {
                v = (*sh).count;
                (*sh).count = 0;
            }
            ev_sync_token(o);
            ev_sh_unlock(sh);
            ev_unlock();
            // A byte copy, not a store through a cast: the guest is only
            // promised an 8-byte buffer, never an 8-byte-aligned one, and
            // aarch64 traps unaligned accesses to device memory.
            ptr::copy_nonoverlapping(
                ptr::addr_of!(v).cast::<u8>(),
                buf.cast::<u8>(),
                size_of::<u64>(),
            );
            return size_of::<u64>() as c_long;
        }
        ev_sh_unlock(sh);
        if (*o).nonblock {
            ev_unlock();
            return lerr(EAGAIN);
        }
        let pr = (*o).pr;
        (*o).refs += 1;
        ev_unlock();

        // Block on the pipe rather than on a condvar: poll() returns EINTR when
        // a guest signal lands, and a blocking eventfd read on Linux is
        // interruptible. pthread_cond_wait() is not, and swallowing the
        // interruption would hang a guest that expects its handler to break the
        // loop. A lost race here just costs one extra trip round the loop.
        // The private read end is used, not the guest's, so a close() of the
        // alias this call names cannot pull the descriptor out from under
        // poll(); lxrt_eventfd_close() writes a wake token instead.
        let mut pfd = PollFd {
            fd: pr,
            events: POLLIN,
            revents: 0,
        };
        let pr_rc = poll(&mut pfd, 1, -1);
        let perr = errno();

        ev_lock();
        (*o).refs -= 1;
        let gone = (*o).dead || !ev_has_fd(o, fd);
        ev_release(o);
        ev_unlock();

        if gone {
            return lerr(EBADF);
        }
        if pr_rc < 0 {
            return lerr(perr);
        }
        if (pfd.revents & POLLNVAL) != 0 {
            return lerr(EBADF);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_eventfd_write(fd: c_int, buf: *const c_void, n: usize) -> c_long {
    if n < size_of::<u64>() {
        return lerr(EINVAL);
    }
    if !range_ok(buf, size_of::<u64>(), false) {
        return lerr(EFAULT);
    }

    let add = buf.cast::<u64>().read_unaligned();
    // Linux rejects the all-ones value outright: it is the one value that could
    // never be added without saturating, so it is reserved as an error.
    if add == u64::MAX {
        return lerr(EINVAL);
    }

    loop {
        ev_lock();
        let o = ev_find(fd);
        if o.is_null() {
            ev_unlock();
            return lerr(EBADF);
        }
        // The counter saturates at UINT64_MAX-1, not UINT64_MAX.
        let sh = (*o).sh;
        ev_sh_lock(sh);
        if add <= (u64::MAX - 1).wrapping_sub((*sh).count) {
            (*sh).count = (*sh).count.wrapping_add(add);
            ev_sync_token(o);
            ev_sh_unlock(sh);
            ev_unlock();
            return size_of::<u64>() as c_long;
        }
        ev_sh_unlock(sh);
        if (*o).nonblock {
            ev_unlock();
            return lerr(EAGAIN);
        }
        let pr = (*o).pr;
        (*o).refs += 1;
        ev_unlock();

        // Overflow needs 2^64 outstanding wakeups, so this path is effectively
        // dead; it is written correctly rather than stubbed because "returns
        // EAGAIN on a blocking fd" would be a lie the guest could not diagnose.
        // Waiting for the descriptor to become readable is a proxy for waiting
        // for a reader: the reader drains the token on its way through.
        let mut pfd = PollFd {
            fd: pr,
            events: POLLIN,
            revents: 0,
        };
        let pr_rc = poll(&mut pfd, 1, 10 /* ms; no reader-side event to wait on */);
        let perr = errno();

        ev_lock();
        (*o).refs -= 1;
        let gone = (*o).dead || !ev_has_fd(o, fd);
        ev_release(o);
        ev_unlock();

        if gone {
            return lerr(EBADF);
        }
        if pr_rc < 0 && perr == EINTR {
            return lerr(EINTR);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_eventfd_close(fd: c_int) {
    if fd < 0 || G_EV_LIVE.load(Ordering::Relaxed) == 0 {
        return;
    }
    if fd < EVMAP_FDS && !evmap_test(fd) {
        return;
    }

    ev_lock();
    let o = ev_find(fd);
    if !o.is_null() {
        evmap_set(fd, false);
        // Drop just this alias. On Linux the eventfd outlives any one of its
        // descriptors and dies with the last; the object here does the same.
        let fds = ptr::addr_of_mut!((*o).fds).cast::<c_int>();
        let mut i = 0;
        while i < (*o).nfds {
            if *fds.add(i as usize) == fd {
                *fds.add(i as usize) = *fds.add(((*o).nfds - 1) as usize);
                (*o).nfds -= 1;
                break;
            }
            i += 1;
        }
        if (*o).nfds == 0 {
            // Another process may still have this eventfd (a fork): then its
            // counter and token stay as they are, and a thread of ours parked
            // in poll() stays parked until that process writes, as a Linux
            // read blocked on a descriptor another thread closed keeps
            // waiting. Only a sole owner resets it and wakes its sleepers.
            let sh = (*o).sh;
            let sole = (*sh).procs.fetch_sub(1, Ordering::AcqRel) <= 1;
            if sole {
                ev_sh_lock(sh);
                (*sh).count = 0;
                ev_sync_token(o); // drains the token: no stray byte
                ev_sh_unlock(sh);
            }
            (*o).dead = true;
            G_EV_LIVE.fetch_sub(1, Ordering::Relaxed);
            if sole && (*o).refs > 0 && (*o).wfd >= 0 {
                // Wake anybody parked in poll() on the private read end. Both
                // pipe ends are still open -- ev_release() closes them once the
                // last reference drops -- so without a token nothing would ever
                // make that poll return. The sleeper sees `gone` and answers
                // EBADF, which is what Linux gives a read on a closed fd.
                let _ = write((*o).wfd, c"e".as_ptr().cast::<c_void>(), 1);
            }
            ev_release(o);
        }
    }
    ev_unlock();
}

// ---------------------------------------------------------------- epoll table

unsafe fn ep_names(ep: *const EpInst, fd: c_int) -> bool {
    if (*ep).kq == fd {
        return true;
    }
    let alias = ptr::addr_of!((*ep).alias).cast::<c_int>();
    let mut k = 0;
    while k < (*ep).nalias {
        if *alias.add(k as usize) == fd {
            return true;
        }
        k += 1;
    }
    false
}

// The instance `kq` names: its own descriptor or a dup of it. Caller holds
// g_ep_lock.
unsafe fn ep_find(kq: c_int) -> *mut EpInst {
    for i in 0..MAX_EPOLL {
        let ep = g_ep(i);
        if (*ep).used && !(*ep).dead && ep_names(ep, kq) {
            return ep;
        }
    }
    ptr::null_mut()
}

// Detect if fd is a nested epoll instance (caller holds g_ep_lock).
#[inline(always)]
unsafe fn is_nested_epoll_locked(fd: c_int) -> bool {
    !ep_find(fd).is_null()
}

unsafe fn ep_lookup(ep: *mut EpInst, fd: c_int) -> *mut EpInterest {
    let mut i = 0;
    while i < (*ep).n {
        let e = (*ep).v.add(i as usize);
        if (*e).fd == fd {
            return e;
        }
        i += 1;
    }
    ptr::null_mut()
}

unsafe fn ep_release(ep: *mut EpInst) {
    if (*ep).dead && (*ep).refs == 0 {
        free((*ep).v.cast::<c_void>());
        (*ep).v = ptr::null_mut();
        (*ep).n = 0;
        (*ep).cap = 0;
        (*ep).kq = -1;
        (*ep).nalias = 0;
        (*ep).dead = false;
        (*ep).used = false;
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_epoll_create1(lflags: c_int) -> c_long {
    // EPOLL_CLOEXEC is the only flag Linux accepts; everything else is EINVAL,
    // including the old epoll_create()'s `size` argument, which is why aarch64
    // does not carry epoll_create at all.
    if lflags & !L_EPOLL_CLOEXEC != 0 {
        return lerr(EINVAL);
    }

    if G_EP_LIVE.load(Ordering::Relaxed) >= MAX_EPOLL as c_int {
        return lerr(EMFILE);
    }

    let kq = kqueue();
    if kq < 0 {
        return lerr(errno());
    }

    // Divergence with no fix available: a Linux epoll fd survives fork() and
    // exec(), and the child sees the same interest list. A Darwin kqueue is
    // explicitly NOT inherited across fork -- the descriptor exists in the
    // child but its queue is empty. lxrt_fork() therefore produces a child
    // whose inherited epoll fds are inert. Documented rather than papered over:
    // re-creating the queue in the child would need a fork handler that also
    // re-registers every knote, and the interest list is not shared memory.
    if lflags & L_EPOLL_CLOEXEC != 0 {
        fcntl(kq, F_SETFD, FD_CLOEXEC);
    }

    ep_lock();
    let mut ep: *mut EpInst = ptr::null_mut();
    for i in 0..MAX_EPOLL {
        if !(*g_ep(i)).used {
            ep = g_ep(i);
            break;
        }
    }
    if ep.is_null() {
        ep_unlock();
        close(kq);
        return lerr(EMFILE);
    }
    (*ep).used = true;
    (*ep).kq = kq;
    (*ep).nalias = 0;
    (*ep).refs = 0;
    (*ep).dead = false;
    (*ep).v = ptr::null_mut();
    (*ep).n = 0;
    (*ep).cap = 0;
    G_EP_LIVE.fetch_add(1, Ordering::Relaxed);
    ep_unlock();
    kq as c_long
}

// A dup of an epoll descriptor is the same epoll instance on Linux. It was
// not one here: tokio (Rust) registers its signal pipe through a
// try_clone()d epoll descriptor (fcntl F_DUPFD_CLOEXEC), epoll_ctl on it
// answered EINVAL, and the runtime could not be built: Heroic's comet
// panicked at start (MEASURED, benchmarks/stage24-heroic.txt). The dup is
// also a descriptor of the same kqueue, so recording it as an alias is all
// it takes. At most EP_ALIASES per instance; a further dup is not recorded.
#[no_mangle]
pub unsafe extern "C" fn lxrt_epoll_dup(oldfd: c_int, newfd: c_int) {
    if oldfd < 0 || newfd < 0 || oldfd == newfd || G_EP_LIVE.load(Ordering::Relaxed) == 0 {
        return;
    }
    ep_lock();
    let ep = ep_find(oldfd);
    if !ep.is_null() && !ep_names(ep, newfd) && (*ep).nalias < EP_ALIASES as c_int {
        let alias = ptr::addr_of_mut!((*ep).alias).cast::<c_int>();
        *alias.add((*ep).nalias as usize) = newfd;
        (*ep).nalias += 1;
    }
    ep_unlock();
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_epoll_close(fd: c_int) {
    if fd < 0 || G_EP_LIVE.load(Ordering::Relaxed) == 0 {
        return;
    }
    ep_lock();
    let ep = ep_find(fd);
    if !ep.is_null() && (*ep).nalias > 0 {
        // One descriptor of several: the instance lives on. When the one
        // closing is kq itself, an alias (the same kqueue) takes its place.
        let alias = ptr::addr_of_mut!((*ep).alias).cast::<c_int>();
        if (*ep).kq == fd {
            (*ep).nalias -= 1;
            (*ep).kq = *alias.add((*ep).nalias as usize);
        } else {
            let mut k = 0;
            while k < (*ep).nalias {
                if *alias.add(k as usize) == fd {
                    (*ep).nalias -= 1;
                    *alias.add(k as usize) = *alias.add((*ep).nalias as usize);
                    break;
                }
                k += 1;
            }
        }
        ep_unlock();
        return;
    }
    if !ep.is_null() {
        (*ep).dead = true;
        G_EP_LIVE.fetch_sub(1, Ordering::Relaxed);
        // The kqueue descriptor itself is the caller's to close; closing it is
        // what breaks any thread currently blocked in kevent() on it, which
        // then drops its reference and frees the slot.
        ep_release(ep);
    }
    ep_unlock();
}

// ---------------------------------------------------------------- epoll_ctl

// Push one interest's knotes at the queue. `mode` is EV_ADD to install and
// EV_DELETE to remove; a modify builds a delete list immediately followed by an
// add list in ONE changelist, because kevent() applies changes in order.
//
// EV_RECEIPT is used so each change reports its own status instead of kevent()
// stopping at the first failure with a bare errno.
unsafe fn ep_changes(i: *const EpInterest, mode: u16, ch: *mut KEvent, max: c_int) -> c_int {
    let mut n: c_int = 0;
    let want = (*i).events;

    let mut base = mode | EV_RECEIPT;
    if mode == EV_ADD {
        // The default must be level-triggered, matching epoll. Measured on this
        // machine: a kqueue knote without EV_CLEAR re-reports a pipe with one
        // unread byte on every kevent() call, so plain EV_ADD already is level.
        if want & L_EPOLLET != 0 {
            base |= EV_CLEAR;
        }
        // EPOLLONESHOT is deliberately NOT mapped to EV_ONESHOT. The two agree
        // on intent but not on SCOPE: Linux disarms the whole interest after
        // one reported event, Darwin deletes the single knote that fired. An
        // interest registered EPOLLIN|EPOLLOUT|EPOLLONESHOT therefore installed
        // two independent one-shot knotes -- measured on a socketpair whose
        // send buffer was full, wait1 returned 0x1 (EPOLLIN) and wait2, after
        // the peer drained, returned 0x4 (EPOLLOUT), where the same sequence on
        // the 6.17.1 guest returns 0 for wait2. A guest that uses EPOLLONESHOT
        // precisely so that one descriptor is handled by one thread at a time
        // gets a second event and can double-service or free-then-use whatever
        // the cookie points at.
        //
        // The disarm is done in software instead, in pwait_common(): the whole
        // interest is EV_DELETEd the instant it reports, and oneshot_fired
        // keeps it out of the match loop until an EPOLL_CTL_MOD re-arms it --
        // which is what Linux does, measured: wait3 after MOD returned 0x5.
        // As a bonus, the read knote below survives firing with nothing to
        // report, which EV_ONESHOT would have destroyed.
    }

    // The read knote is installed for EVERY interest, not just those that asked
    // for readability. Linux owes EPOLLERR and EPOLLHUP on every interest
    // whether or not they were requested -- measured on the guest, a pipe whose
    // writer is gone reports 0x10 (EPOLLHUP) for an interest registered with
    // events=0, with EPOLLPRI alone, and with EPOLLRDBAND alone. kqueue has no
    // error-only filter, so EVFILT_READ stands in for all of them with one
    // rule.
    //
    // Before this, want_read was false unless the interest asked for read data,
    // EPOLLRDHUP, or nothing at all, which left EPOLLPRI-only registration
    // watching NOTHING: the sole change pushed was EVFILT_EXCEPT, that filter
    // returns EINVAL on a pipe (measured: EV_ERROR data=22) and ep_submit()
    // treats EXCEPT failures as soft, so epoll_ctl returned 0 having installed
    // zero knotes. Measured consequence: ADD EPOLLPRI on a pipe read end, close
    // the writer, epoll_pwait(120) -> 0 events after the full 120 ms, where
    // Linux reports EPOLLHUP. Silent success with nothing behind it is the one
    // outcome this runtime must never produce.
    let want_write = (want & L_EPOLL_WRITE_BITS) != 0;

    let mut rflags = base;
    if mode == EV_ADD && (want & L_EPOLL_READ_EMIT) == 0 {
        // Forced to edge mode. A level-triggered read knote whose EPOLLIN the
        // translation is going to discard -- because the guest never asked for
        // it -- fires on every single kevent() and the wait loop then has to
        // re-wait, burning a core until the timeout and forever when the
        // timeout is -1. Edge costs a late EPOLLRDHUP if the guest ignores the
        // first report; level costs a spinning event loop. Edge wins.
        //
        // The cost is measured rather than assumed: an EV_CLEAR read knote
        // registered on a pipe whose writer had ALREADY gone still reports
        // EV_EOF on the first kevent() and nothing on any later one. So the
        // hangup reaches the guest exactly once instead of on every wait as
        // Linux would. An event loop reacts to EPOLLHUP by closing or DELing
        // the descriptor, so one report is enough in practice; a loop that
        // ignores the first one will not be told again. Accepted, because the
        // alternative is the 100%-CPU livelock above.
        rflags |= EV_CLEAR;
    }

    let mut wflags = base;
    if mode == EV_ADD && (want & write_emit_bits(i)) == 0 {
        // The same rule on the write side, which previously had no forced-edge
        // path at all. EPOLLWRBAND alone on a pipe is the case that mattered:
        // the write filter can only produce EPOLLOUT|EPOLLWRNORM there, the
        // guest asked for neither, and a level knote on an always-writable pipe
        // spun the wait loop at 100% of a core.
        wflags |= EV_CLEAR;
    }

    let udata = (*i).data as usize as *mut c_void;
    if n < max {
        ev_set(
            ch.add(n as usize),
            (*i).fd as isize as usize,
            EVFILT_READ,
            rflags,
            0,
            0,
            udata,
        );
        n += 1;
    }
    if want_write && n < max {
        ev_set(
            ch.add(n as usize),
            (*i).wfd as isize as usize,
            EVFILT_WRITE,
            wflags,
            0,
            0,
            udata,
        );
        n += 1;
    }
    if (want & L_EPOLLPRI) != 0 && n < max {
        // EVFILT_EXCEPT/NOTE_OOB is the nearest thing Darwin has to EPOLLPRI.
        // It exists only for sockets: measured, registering it on a pipe
        // returns EINVAL. That failure is tolerated below rather than
        // propagated, because Linux accepts EPOLLPRI on a pipe too and simply
        // never reports it -- and the read knote above now carries the
        // ERR/HUP half of the contract, so a pipe interest is never empty.
        ev_set(
            ch.add(n as usize),
            (*i).fd as isize as usize,
            EVFILT_EXCEPT,
            base,
            NOTE_OOB,
            0,
            udata,
        );
        n += 1;
    }
    n
}

// Applies a changelist and returns the first hard error, or 0. EVFILT_EXCEPT
// failures are always soft (see ep_changes). The first `nsoft` entries are
// removals whose ENOENT is soft: a knote whose descriptor has already been
// closed is gone from the queue with nobody told -- measured, EV_DELETE naming
// a closed fd returns ENOENT(2), not EBADF -- and an EPOLL_CTL_MOD's leading
// delete list is expected to find nothing when the previous mask installed
// fewer filters than this one removes.
unsafe fn ep_submit(kq: c_int, ch: *mut KEvent, n: c_int, nsoft: c_int) -> c_int {
    if n <= 0 {
        return 0;
    }
    let mut out = MaybeUninit::<[KEvent; 2 * EP_MAX_CH]>::uninit();
    let out = out.as_mut_ptr().cast::<KEvent>();
    if n > (2 * EP_MAX_CH) as c_int {
        return EINVAL; // unreachable: every caller builds at most 2*EP_MAX_CH
    }
    // KEVENT_FLAG_IMMEDIATE, not a NULL timeout. Every change carries
    // EV_RECEIPT, so the call fills the output array with one receipt per
    // change and returns without waiting -- but this runs with g_ep_lock held,
    // and "cannot block" is not a property worth betting every epoll instance
    // in the process on.
    let r = ep_kevent_now(kq, ch, n, out, n);
    if r < 0 {
        return errno();
    }
    let mut i = 0;
    while i < r {
        let o = out.add(i as usize);
        i += 1;
        if ((*o).flags & EV_ERROR) == 0 || (*o).data == 0 {
            continue;
        }
        let e = (*o).data as c_int;
        if (i - 1) < nsoft && e == ENOENT {
            continue;
        }
        if (*o).filter == EVFILT_EXCEPT {
            continue; // see ep_changes(): EPOLLPRI is best-effort
        }
        return e;
    }
    0
}

// Is this entry still describing the file it was registered against? Nothing
// informs this module when the guest closes a watched descriptor unless the
// integrator wires lxrt_epoll_fd_closed() (see the contract in
// epoll_eventfd.h), and kqueue drops the knote silently when the fd goes. The
// stale entry then owns the fd NUMBER forever: measured, a pipe on fd 4 added
// to an epoll, closed, and the number recycled by a fresh pipe made
// EPOLL_CTL_ADD(4) return -EEXIST where Linux returns 0, and the epoll_pwait
// after it reported nothing although fd 4 was readable -- a silently dead event
// loop. Steam recycles descriptors constantly, so this is the cheap safety net
// that works even if the hook is never wired.
//
// `st` is the caller's already-taken fstat of the descriptor, or NULL when the
// fstat failed, which means the fd is closed outright.
//
// st_ino is a strong hint and not a proof, because Darwin does not keep a
// pipe's inode number unique over time: measured over twelve create/close
// cycles, the same six values came round again in order, so a recycled fd
// number lands on its old ino roughly one time in six. ep_knote_gone() below is
// the second opinion for the one path where getting it wrong is visible.
unsafe fn ep_entry_stale(i: *const EpInterest, st: *const Stat) -> bool {
    if st.is_null() {
        return true;
    }
    (*st).st_dev != (*i).st_dev || (*st).st_ino != (*i).st_ino
}

// Does this entry still have a registration in the queue? kqueue knows what
// st_ino cannot tell us: it drops every knote of a descriptor when that
// descriptor is closed, so a knote that is still present proves the fd was
// never closed since ADD. EV_ENABLE names an existing knote without creating
// one -- measured on this machine, it returns ENOENT(2) for a filter that is
// not registered and 0 for one that is, and it leaves both EV_CLEAR and udata
// intact. The read knote is used because ep_changes() installs it for every
// interest.
//
// It is not free of side effects: also measured, EV_ENABLE re-activates an
// EV_CLEAR knote whose condition is still true, so the guest may see one
// spurious event afterwards. That is why this is called only on the duplicate
// EPOLL_CTL_ADD path, which is an error return in any case, and epoll is
// allowed to wake spuriously.
unsafe fn ep_knote_gone(kq: c_int, i: *const EpInterest) -> bool {
    let mut ch = MaybeUninit::<KEvent>::uninit();
    let mut out = MaybeUninit::<KEvent>::uninit();
    ev_set(
        ch.as_mut_ptr(),
        (*i).fd as isize as usize,
        EVFILT_READ,
        EV_ENABLE | EV_RECEIPT,
        0,
        0,
        (*i).data as usize as *mut c_void,
    );
    let r = ep_kevent_now(kq, ch.as_ptr(), 1, out.as_mut_ptr(), 1);
    if r <= 0 {
        return false; // no answer: keep the entry rather than guess
    }
    let out = out.as_ptr();
    ((*out).flags & EV_ERROR) != 0 && (*out).data == ENOENT as isize
}

// Caller holds g_ep_lock. Swap-removes ep->v[idx]. `alive` says whether the
// descriptor is still open: EV_DELETE can only name a live fd, and on a closed
// one kqueue has already dropped the knote.
unsafe fn ep_drop_at(ep: *mut EpInst, idx: c_int, alive: bool) {
    let e = (*ep).v.offset(idx as isize);
    if alive {
        let mut ch = MaybeUninit::<[KEvent; EP_MAX_CH]>::uninit();
        let ch = ch.as_mut_ptr().cast::<KEvent>();
        let n = ep_changes(e, EV_DELETE, ch, EP_MAX_CH as c_int);
        ep_submit((*ep).kq, ch, n, n);
    } else if (*e).wfd != (*e).fd {
        // For eventfd: the EVFILT_WRITE knote names wfd (the private pipe end),
        // which stays open while another alias holds the eventfd. Must delete it.
        let mut ch = MaybeUninit::<KEvent>::uninit();
        ev_set(
            ch.as_mut_ptr(),
            (*e).wfd as isize as usize,
            EVFILT_WRITE,
            EV_DELETE | EV_RECEIPT,
            0,
            0,
            ptr::null_mut(),
        );
        let mut out = MaybeUninit::<KEvent>::uninit();
        ep_kevent_now((*ep).kq, ch.as_ptr(), 1, out.as_mut_ptr(), 1);
        // Tolerate ENOENT: wfd may have been closed alongside fd.
    }
    // ep->v[idx] = ep->v[ep->n - 1]; the two may be the same slot.
    ptr::copy((*ep).v.offset(((*ep).n - 1) as isize), e, 1);
    (*ep).n -= 1;
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_epoll_fd_closed(fd: c_int) {
    if fd < 0 || G_EP_LIVE.load(Ordering::Relaxed) == 0 {
        return;
    }

    ep_lock();
    for i in 0..MAX_EPOLL {
        let ep = g_ep(i);
        if !(*ep).used || (*ep).dead {
            continue;
        }
        let mut j: c_int = 0;
        while j < (*ep).n {
            // wfd as well as fd: an eventfd's writability rides on the pipe's
            // other end, which lxrt_eventfd_close() is about to close.
            let e = (*ep).v.offset(j as isize);
            if (*e).fd == fd || (*e).wfd == fd {
                // the caller has not closed it yet, by contract, so EV_DELETE
                // works
                ep_drop_at(ep, j, true);
            } else {
                j += 1;
            }
        }
    }
    ep_unlock();
}

#[inline(always)]
fn s_isreg(mode: u16) -> bool {
    mode & S_IFMT == S_IFREG
}

#[inline(always)]
fn s_isdir(mode: u16) -> bool {
    mode & S_IFMT == S_IFDIR
}

#[inline(always)]
fn s_issock(mode: u16) -> bool {
    mode & S_IFMT == S_IFSOCK
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_epoll_ctl(
    epfd: c_int,
    op: c_int,
    fd: c_int,
    levent: *const c_void,
) -> c_long {
    if epfd < 0 || fd < 0 {
        return lerr(EBADF);
    }
    // Linux rejects an epoll fd watching itself before it even looks at `op`.
    if epfd == fd {
        return lerr(EINVAL);
    }

    let mut ev = LinuxEpollEvent {
        events: 0,
        pad_aarch64: 0,
        data: 0,
    };
    if op == L_EPOLL_CTL_ADD || op == L_EPOLL_CTL_MOD {
        if !range_ok(levent, size_of::<LinuxEpollEvent>(), false) {
            return lerr(EFAULT);
        }
        ev = levent.cast::<LinuxEpollEvent>().read_unaligned();
    } else if op != L_EPOLL_CTL_DEL {
        return lerr(EINVAL);
    }
    // EPOLL_CTL_DEL has accepted a NULL event since 2.6.9 and ignores it.

    if op == L_EPOLL_CTL_MOD && (ev.events & L_EPOLLEXCLUSIVE) != 0 {
        // Linux does the same, for the same reason: exclusive wakeups cannot be
        // changed in place.
        return lerr(EINVAL);
    }

    // One fstat answers everything the operation needs about the target: the
    // EBADF and EPERM checks Linux does on it, the socket-vs-pipe decision that
    // tells EPOLLHUP from EPOLLRDHUP at EV_EOF, and the identity comparison
    // that unmasks a stale entry left behind by a close this module was never
    // told about.
    let mut tst = MaybeUninit::<Stat>::uninit();
    let fd_alive = fstat(fd, tst.as_mut_ptr()) == 0;
    // Only read where fd_alive is true, as in the C.
    let tst = tst.as_ptr();

    ep_lock();
    let ep = ep_find(epfd);
    if ep.is_null() {
        ep_unlock();
        // Not one of ours: either a real bad fd or an fd the guest obtained
        // without epoll_create1. EINVAL is what Linux returns for a valid fd
        // that is not an epoll instance.
        return if fcntl(epfd, F_GETFD) < 0 {
            lerr(EBADF)
        } else {
            lerr(EINVAL)
        };
    }

    let mut cur = ep_lookup(ep, fd);
    if !cur.is_null() && ep_entry_stale(cur, if fd_alive { tst } else { ptr::null() }) {
        // The descriptor behind this entry is gone, and its number may already
        // belong to something else. Drop it before answering, so the guest sees
        // the fd it is actually holding rather than the ghost of the last one.
        ep_drop_at(ep, cur.offset_from((*ep).v) as c_int, false);
        cur = ep_lookup(ep, fd);
    }

    let mut ch = MaybeUninit::<[KEvent; 2 * EP_MAX_CH]>::uninit();
    let ch = ch.as_mut_ptr().cast::<KEvent>();

    let ret: c_long = 'op: {
        match op {
            L_EPOLL_CTL_ADD => {
                if !fd_alive {
                    break 'op lerr(EBADF);
                }
                // cur may be NULL: a fresh ADD
                let knote_was_alive = !cur.is_null() && !(*cur).oneshot_fired;
                if !cur.is_null() && knote_was_alive && ep_knote_gone((*ep).kq, cur) {
                    // The number is live but our registration is not, so the
                    // descriptor was closed and recycled onto an object that happened
                    // to reuse the inode number. `false`: the knote is already gone, and
                    // an EV_DELETE here would name in->wfd, which for an eventfd is a
                    // private descriptor that may still belong to a live object.
                    ep_drop_at(ep, cur.offset_from((*ep).v) as c_int, false);
                    cur = ptr::null_mut();
                }
                if !cur.is_null() {
                    break 'op lerr(EEXIST);
                }
                // Linux returns EPERM when a regular file or a directory is added: an
                // inode with no poll method is always ready and epoll refuses to say
                // so. kqueue accepts both happily -- measured -- and would report the
                // file readable forever. libuv and libevent both use this EPERM to
                // detect "this fd needs blocking I/O, not the loop", so accepting it
                // would not be generosity, it would break their fallback.
                if s_isreg((*tst).st_mode) || s_isdir((*tst).st_mode) {
                    break 'op lerr(EPERM);
                }
                if (ev.events & L_EPOLLEXCLUSIVE) != 0 {
                    // fs/eventpoll.c, do_epoll_ctl: on ADD, EPOLLEXCLUSIVE is EINVAL
                    // when combined with any bit outside EPOLLEXCLUSIVE_OK_BITS --
                    // EPOLLONESHOT most importantly -- or when the target is itself an
                    // epoll instance. Measured on the guest: IN|EXCLUSIVE -> 0,
                    // IN|EXCLUSIVE|ONESHOT -> -EINVAL, IN|EXCLUSIVE|RDBAND -> -EINVAL,
                    // and an epoll fd as the target -> -EINVAL. Half-validating this
                    // gave a guest feature-probing for EPOLLEXCLUSIVE a wrong answer.
                    if (ev.events & !L_EPOLL_EXCLUSIVE_OK_BITS) != 0 || !ep_find(fd).is_null() {
                        break 'op lerr(EINVAL);
                    }
                }

                if (*ep).n == (*ep).cap {
                    let ncap: c_int = if (*ep).cap != 0 {
                        (*ep).cap.wrapping_mul(2)
                    } else {
                        8
                    };
                    let nv = realloc(
                        (*ep).v.cast::<c_void>(),
                        (ncap as usize).wrapping_mul(size_of::<EpInterest>()),
                    )
                    .cast::<EpInterest>();
                    if nv.is_null() {
                        break 'op lerr(ENOMEM);
                    }
                    (*ep).v = nv;
                    (*ep).cap = ncap;
                }
                let inp = (*ep).v.offset((*ep).n as isize);
                ptr::write_bytes(inp, 0, 1);
                (*inp).fd = fd;
                // An eventfd's writability lives on the pipe's other end. Resolved once
                // here rather than on every wait.
                let wend = eventfd_write_end(fd);
                (*inp).wfd = if wend >= 0 { wend } else { fd };
                (*inp).events = ev.events;
                (*inp).data = ev.data;
                (*inp).st_dev = (*tst).st_dev;
                (*inp).st_ino = (*tst).st_ino;
                // S_ISSOCK on a kqueue fd is false and S_ISFIFO is true -- Darwin
                // reports mode 010000 for one. That is the right answer for us: a
                // nested epoll behaves like a pipe, readable when it has work.
                (*inp).is_socket = s_issock((*tst).st_mode);
                // EPOLLEXCLUSIVE, once validated above, has no kqueue equivalent. It
                // exists to stop every epoll instance watching a shared listening
                // socket from waking on one connection. Ignoring its effect is safe in
                // the only direction that matters -- extra wakeups, never a missed one
                // -- and the guest's accept() already has to cope with EAGAIN.
                // EPOLLWAKEUP is an Android suspend-blocker and means nothing here; it
                // is ignored too.
                //
                // The C masked the write bits out of its local copy of the event
                // here for a nested epoll, AFTER in->events had been stored, so the
                // registered mask is unchanged; the lookup is kept and the dead
                // store is not reproduced.
                let _ = is_nested_epoll_locked(fd);
                let nch = ep_changes(inp, EV_ADD, ch, EP_MAX_CH as c_int);
                let e = ep_submit((*ep).kq, ch, nch, 0);
                if e != 0 {
                    // Roll back any knote that did install, so a failed ADD leaves no
                    // trace -- Linux's ADD is all-or-nothing.
                    let back = ep_changes(inp, EV_DELETE, ch, EP_MAX_CH as c_int);
                    ep_submit((*ep).kq, ch, back, back);
                    // EINVAL from an EVFILT_READ/EVFILT_WRITE registration is Darwin
                    // saying this descriptor type has no kqueue filter, which is the
                    // same condition Linux calls "no poll method" and answers with
                    // EPERM. Measured on both sides: kqueue EV_ADD of either filter on
                    // /dev/null, /dev/zero and /dev/urandom returns EINVAL(22), while
                    // epoll_ctl(ADD) on the same three files on the 6.17.1 guest
                    // returns EPERM(1) for both EPOLLIN and EPOLLOUT. libuv and
                    // libevent branch on that EPERM to fall back to blocking I/O, so
                    // handing them EINVAL instead would break the fallback in exactly
                    // the way the S_ISREG check above exists to avoid. Pipes, FIFOs,
                    // sockets, and listening sockets register both filters cleanly.
                    // Nested kqueues: EVFILT_WRITE returns EINVAL (measured), which is
                    // the no-filter case and not a malformed changelist.
                    break 'op if e == EINVAL { lerr(EPERM) } else { lerr(e) };
                }
                (*ep).n += 1;
                0
            }
            L_EPOLL_CTL_MOD => {
                if cur.is_null() {
                    // Measured on the guest: EPOLL_CTL_DEL/MOD naming a closed
                    // descriptor returns EBADF, not ENOENT -- fget fails before the
                    // interest tree is consulted. The stale-entry drop above is what
                    // makes that reachable here at all.
                    break 'op if fd_alive { lerr(ENOENT) } else { lerr(EBADF) };
                }
                // MOD replaces, it does not accumulate. Dropping EPOLLOUT from the mask
                // has to actually delete the write knote, so the old registration is
                // torn down and the new one built from scratch -- as ONE changelist,
                // because two separate kevent() calls leave a window in which another
                // thread's epoll_pwait sees the descriptor unregistered.
                let old: EpInterest = *cur;
                let mut upd: EpInterest = *cur;
                upd.events = ev.events;
                upd.data = ev.data;
                // EPOLL_CTL_MOD is how Linux re-arms an EPOLLONESHOT interest --
                // measured: after a oneshot report wait2 gave 0 and wait3, after MOD,
                // gave 0x5.
                upd.oneshot_fired = false;
                let ndel = ep_changes(&old, EV_DELETE, ch, EP_MAX_CH as c_int);
                let nadd = ep_changes(&upd, EV_ADD, ch.add(ndel as usize), EP_MAX_CH as c_int);
                let e = ep_submit((*ep).kq, ch, ndel + nadd, ndel);
                if e != 0 {
                    // A partially applied changelist is the state that has to be
                    // undone: an add that installed for a mask the interest will not
                    // carry leaves a knote nothing reports, and before the arming rule
                    // in ep_changes() that stale knote could spin the wait loop. Tear
                    // the NEW registration down first -- the previous code restored
                    // *cur and re-added the OLD knotes without ever deleting what the
                    // failed attempt had installed, so it was not a rollback at all.
                    let mut back = ep_changes(&upd, EV_DELETE, ch, EP_MAX_CH as c_int);
                    ep_submit((*ep).kq, ch, back, back);
                    back = ep_changes(&old, EV_ADD, ch, EP_MAX_CH as c_int);
                    ep_submit((*ep).kq, ch, back, 0);
                    lerr(e)
                } else {
                    *cur = upd;
                    0
                }
            }
            L_EPOLL_CTL_DEL => {
                if cur.is_null() {
                    break 'op if fd_alive { lerr(ENOENT) } else { lerr(EBADF) };
                }
                // ENOENT from the queue is expected whenever the descriptor is no
                // longer registered behind our back; ep_drop_at() treats it as soft.
                ep_drop_at(ep, cur.offset_from((*ep).v) as c_int, fd_alive);
                0
            }
            _ => lerr(EINVAL),
        }
    };

    ep_unlock();
    ret
}

// ---------------------------------------------------------------- epoll_pwait

// One coalesced output slot. kqueue delivers read and write readiness for the
// same descriptor as two kevents; epoll owes the guest one. `data` is copied in
// under g_ep_lock so that the store into guest memory can happen with the lock
// dropped -- see the copy-out in pwait_common().
#[repr(C)]
struct OutSlot {
    idx: c_int, // index into ep->v
    mask: u32,
    data: u64,
    eof_read: bool,
    eof_write: bool,
}
const _: [(); 24] = [(); size_of::<OutSlot>()];

// Translate one kevent into Linux event bits for an interest that is already
// known. Returns 0 when the event carries nothing the guest asked for, which
// happens for an edge-armed stand-in knote and must not reach the guest: Linux
// never reports an epoll_event with an empty mask.
//
// The bits produced here and the level-vs-edge decision in ep_changes() are two
// halves of one invariant: a LEVEL-triggered knote always produces at least one
// bit that survives `allowed` below, so it can never drive the caller's re-wait
// loop round a second time with nothing to show. Breaking that invariant is
// what the measured 100%-CPU livelock was.
unsafe fn kev_to_epoll(
    k: *const KEvent64,
    i: *const EpInterest,
    eof_read: *mut bool,
    eof_write: *mut bool,
) -> u32 {
    let mut m: u32 = 0;
    let want = (*i).events;

    if ((*k).flags & EV_ERROR) != 0 {
        // The changelist is empty during a wait, so EV_ERROR here is a
        // condition on the knote rather than a rejected registration.
        return L_EPOLLERR;
    }

    match (*k).filter {
        EVFILT_READ => {
            // Report EPOLLIN even at EOF. Linux does, and it is what lets the guest
            // call read() and see the 0 that tells it the peer is gone; withholding
            // it turns a clean shutdown into a stall.
            //
            // EPOLLRDBAND is deliberately absent: measured on the 6.17.1 guest, a
            // readable pipe and a readable unix socket both report 0 events for an
            // EPOLLRDBAND-only interest, so Linux never sets it for ordinary
            // readability and emitting it here would invent an event.
            m |= L_EPOLL_READ_EMIT;
            if ((*k).flags & EV_EOF) != 0 {
                *eof_read = true;
                if (*i).is_socket {
                    // Socket EOF is a received FIN: the read half is done, the
                    // write half may still be usable. That is exactly EPOLLRDHUP,
                    // and Linux only reports it when asked. EPOLLHUP waits until
                    // the write side is also gone -- decided after coalescing.
                    if (want & L_EPOLLRDHUP) != 0 {
                        m |= L_EPOLLRDHUP;
                    }
                    // For sockets kqueue parks SO_ERROR in fflags at EOF.
                    if (*k).fflags != 0 {
                        m |= L_EPOLLERR;
                    }
                } else {
                    // A pipe or FIFO whose writer closed is a full hangup; there is
                    // no half-open state to be in. Linux gives EPOLLHUP|EPOLLIN.
                    m |= L_EPOLLHUP;
                }
            }
        }
        EVFILT_WRITE => {
            // EPOLLWRBAND rides along on a socket and not on a pipe, because that
            // is what Linux does: unix_poll and tcp_poll set
            // EPOLLOUT|EPOLLWRNORM|EPOLLWRBAND while pipe_poll sets only
            // EPOLLOUT|EPOLLWRNORM. Measured on the guest: a writable unix socket
            // with an EPOLLWRBAND-only interest reports 0x200, a writable pipe with
            // the same interest reports nothing.
            m |= write_emit_bits(i);
            if ((*k).flags & EV_EOF) != 0 {
                *eof_write = true;
                if (*i).is_socket {
                    m |= L_EPOLLHUP;
                } else {
                    // A pipe whose reader closed: the next write() gets EPIPE.
                    // poll() on Linux reports POLLERR for this, and it keeps
                    // POLLOUT set because the write will not block, it will fail.
                    m |= L_EPOLLERR;
                }
            }
            if (*k).fflags != 0 && (*i).is_socket {
                m |= L_EPOLLERR;
            }
        }
        EVFILT_EXCEPT => {
            m |= L_EPOLLPRI;
        }
        _ => return 0,
    }

    // Mask down to what was requested. EPOLLERR and EPOLLHUP are the two
    // exceptions: Linux reports them unconditionally, which is why an event
    // loop that registers only EPOLLIN still learns that a connection died.
    let allowed = (want & (L_EPOLL_READ_BITS | L_EPOLL_WRITE_BITS | L_EPOLLPRI | L_EPOLLRDHUP))
        | L_EPOLLERR
        | L_EPOLLHUP;
    m & allowed
}

// `static int immediate = -1` of pwait_common(). A plain int in C, written
// with the same value by whichever thread gets there first.
static PWAIT_IMMEDIATE: AtomicI32 = AtomicI32::new(-1);

const KSTACK_N: usize = 64;

unsafe fn pwait_common(
    epfd: c_int,
    uevents: *mut c_void,
    maxevents: c_int,
    rel: *const Timespec,
    mut infinite: bool,
    lsigmask: *const u64,
    sigsetsize: usize,
) -> c_long {
    // Linux's EP_MAX_EVENTS is INT_MAX / sizeof(struct epoll_event), so the
    // multiplication below cannot overflow and a guest cannot ask the runtime
    // to probe a nonsense range. The divisor is the aarch64 16-byte struct, not
    // the x86 12-byte one -- the cap differs between the two architectures and
    // this is the aarch64 runtime.
    if maxevents <= 0 || maxevents > (c_int::MAX as usize / size_of::<LinuxEpollEvent>()) as c_int {
        return lerr(EINVAL);
    }

    // One event is probed here, not the whole maxevents span. Linux's
    // do_epoll_wait does access_ok() over the full range, but access_ok() only
    // asks "is this address user space", never "is it mapped" -- the real fault
    // check is the per-event __put_user in ep_send_events. Probing
    // maxevents * 16 up front therefore rejected with EFAULT a guest that Linux
    // accepts: one that passes a maxevents larger than its buffer and relies on
    // fewer events being ready. glibc always sizes the two to match, so this
    // was unlikely to bite, but it was a divergence in the breaking direction.
    // Every entry is re-probed for real immediately before it is written.
    if !range_ok(uevents, size_of::<LinuxEpollEvent>(), true) {
        return lerr(EFAULT);
    }

    // Linux checks sigsetsize only when a mask is actually supplied
    // (set_user_sigmask returns 0 for a NULL pointer before it validates).
    let mut oldmask: u64 = 0;
    let mut swapped = false;
    if !lsigmask.is_null() {
        if sigsetsize != L_KERNEL_SIGSET_BYTES {
            return lerr(EINVAL);
        }
        if !range_ok(lsigmask.cast::<c_void>(), size_of::<u64>(), false) {
            return lerr(EFAULT);
        }
        let rc = lxrt_rt_sigprocmask(L_SIG_SETMASK, lsigmask, &mut oldmask, L_KERNEL_SIGSET_BYTES);
        if rc < 0 {
            return rc;
        }
        swapped = true;
    }

    ep_lock();
    let ep = ep_find(epfd);
    if ep.is_null() {
        ep_unlock();
        if swapped {
            lxrt_rt_sigprocmask(
                L_SIG_SETMASK,
                &oldmask,
                ptr::null_mut(),
                L_KERNEL_SIGSET_BYTES,
            );
        }
        return if fcntl(epfd, F_GETFD) < 0 {
            lerr(EBADF)
        } else {
            lerr(EINVAL)
        };
    }
    // Hold a reference across the wait. The instance must not be freed while
    // this thread sits in kevent() with the table unlocked, and the table must
    // be unlocked or a concurrent epoll_ctl on the same instance -- which is
    // the normal shape of a multi-threaded event loop -- would deadlock.
    (*ep).refs += 1;
    let kq = (*ep).kq;
    ep_unlock();

    // 64 covers the maxevents the measured loops actually pass, so the hot path
    // makes no allocation at all: 64 * (48 + 24) bytes of stack.
    let kn: c_int = if maxevents > KEV_BATCH_MAX {
        KEV_BATCH_MAX
    } else {
        maxevents
    };
    let mut kstack = MaybeUninit::<[KEvent64; KSTACK_N]>::uninit();
    let mut ostack = MaybeUninit::<[OutSlot; KSTACK_N]>::uninit();
    let kstack = kstack.as_mut_ptr().cast::<KEvent64>();
    let ostack = ostack.as_mut_ptr().cast::<OutSlot>();
    let mut kv = kstack;
    let mut os = ostack;
    if kn > KSTACK_N as c_int {
        kv = malloc(kn as usize * size_of::<KEvent64>()).cast::<KEvent64>();
        os = malloc(kn as usize * size_of::<OutSlot>()).cast::<OutSlot>();
        if kv.is_null() || os.is_null() {
            free(kv.cast::<c_void>());
            free(os.cast::<c_void>());
            ep_lock();
            (*ep).refs -= 1;
            ep_release(ep);
            ep_unlock();
            if swapped {
                lxrt_rt_sigprocmask(
                    L_SIG_SETMASK,
                    &oldmask,
                    ptr::null_mut(),
                    L_KERNEL_SIGSET_BYTES,
                );
            }
            return lerr(ENOMEM);
        }
    }

    // A deadline, not a per-iteration timeout. Coalescing and the edge-armed
    // stand-in knote can both turn a non-empty kevent() return into zero
    // reportable events, and returning 0 early would look like a timeout to the
    // guest and fire whatever its idle path is. Re-waiting on the remaining
    // time is the only honest answer.
    let mut deadline = Timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    if !infinite {
        let mut now = Timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        clock_gettime(CLOCK_MONOTONIC, &mut now);
        // epoll_pwait2 forwards whatever non-negative tv_sec the guest hands
        // it, and glibc does not clamp, so now.tv_sec + rel->tv_sec was signed
        // overflow: UB, and in practice a deadline in the PAST, which returned
        // 0 immediately and looked to the guest like a spurious timeout in its
        // idle path. A wait that long is a wait forever -- measured on the
        // guest, a real epoll_pwait2 with tv_sec = INT64_MAX-1 never returned.
        if (*rel).tv_sec > i64::MAX.wrapping_sub(now.tv_sec).wrapping_sub(1) {
            infinite = true;
        } else {
            deadline.tv_sec = now.tv_sec.wrapping_add((*rel).tv_sec);
            deadline.tv_nsec = now.tv_nsec.wrapping_add((*rel).tv_nsec);
            if deadline.tv_nsec >= 1_000_000_000 {
                deadline.tv_nsec -= 1_000_000_000;
                deadline.tv_sec = deadline.tv_sec.wrapping_add(1);
            }
        }
    }

    let ret: c_long;
    loop {
        let mut left = Timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        let mut tsp: *const Timespec = ptr::null();
        if !infinite {
            let mut now = Timespec {
                tv_sec: 0,
                tv_nsec: 0,
            };
            clock_gettime(CLOCK_MONOTONIC, &mut now);
            left.tv_sec = deadline.tv_sec.wrapping_sub(now.tv_sec);
            left.tv_nsec = deadline.tv_nsec.wrapping_sub(now.tv_nsec);
            if left.tv_nsec < 0 {
                left.tv_nsec += 1_000_000_000;
                left.tv_sec = left.tv_sec.wrapping_sub(1);
            }
            if left.tv_sec < 0 {
                left.tv_sec = 0;
                left.tv_nsec = 0;
            }
            tsp = &left;
        }

        // No time left (a poll, epoll_wait(0): the commonest call of a game's
        // main loop) is KEVENT_FLAG_IMMEDIATE, never a zero timespec, which
        // sleeps until the next timer deadline -- 12 us idle, ~90 us under a
        // game: see kev_to64() for the measurements.
        // LXRT_EPOLL0_IMMEDIATE=0: the zero timespec again (the kernel's
        // sleep), to compare.
        let mut immediate = PWAIT_IMMEDIATE.load(Ordering::Relaxed);
        if immediate < 0 {
            let e = getenv(c"LXRT_EPOLL0_IMMEDIATE".as_ptr());
            immediate = (e.is_null() || atoi(e) != 0) as c_int;
            PWAIT_IMMEDIATE.store(immediate, Ordering::Relaxed);
        }
        let n: c_int = if immediate != 0 && !tsp.is_null() && left.tv_sec == 0 && left.tv_nsec == 0
        {
            kevent64(kq, ptr::null(), 0, kv, kn, KEVENT_FLAG_IMMEDIATE, ptr::null())
        } else {
            kevent64(kq, ptr::null(), 0, kv, kn, 0, tsp)
        };
        if n < 0 {
            // EINTR is not restarted: epoll_pwait exists precisely so the guest
            // can see the signal, and Linux does not set SA_RESTART semantics
            // on it.
            ret = lerr(errno());
            break;
        }

        let mut nout: c_int = 0;
        ep_lock();
        let mut i: c_int = 0;
        while i < n {
            let k = kv.add(i as usize);
            i += 1;
            // Match by (ident, filter). The write knote of an eventfd carries
            // the pipe's write end as its ident, which is a descriptor the
            // guest never sees, so the two idents cannot collide.
            let mut idx: c_int = -1;
            let mut j: c_int = 0;
            while j < (*ep).n {
                let e = (*ep).v.add(j as usize);
                if (*e).oneshot_fired {
                    // EPOLLONESHOT: disarmed until EPOLL_CTL_MOD
                    j += 1;
                    continue;
                }
                let hit = if (*k).filter == EVFILT_WRITE {
                    (*e).wfd as isize as usize as u64 == (*k).ident
                } else {
                    (*e).fd as isize as usize as u64 == (*k).ident
                };
                if hit {
                    idx = j;
                    break;
                }
                j += 1;
            }
            if idx < 0 {
                continue; // EPOLL_CTL_DEL raced this wait; the knote is stale
            }

            let mut eof_r = false;
            let mut eof_w = false;
            let m = kev_to_epoll(k, (*ep).v.add(idx as usize), &mut eof_r, &mut eof_w);

            let mut slot: c_int = -1;
            let mut s: c_int = 0;
            while s < nout {
                if (*os.add(s as usize)).idx == idx {
                    slot = s;
                    break;
                }
                s += 1;
            }
            if slot < 0 {
                if nout >= kn {
                    continue;
                }
                slot = nout;
                nout += 1;
                let o = os.add(slot as usize);
                (*o).idx = idx;
                (*o).mask = 0;
                (*o).data = 0;
                (*o).eof_read = false;
                (*o).eof_write = false;
            }
            let o = os.add(slot as usize);
            (*o).mask |= m;
            (*o).eof_read |= eof_r;
            (*o).eof_write |= eof_w;
        }

        // Finalise every slot while the lock is held -- it reads ep->v, and the
        // EPOLLONESHOT disarm must be atomic with the decision to report, so a
        // second thread's epoll_pwait cannot also claim this interest. Empty
        // masks are dropped and the survivors compacted to the front, carrying
        // their cookie with them, so that nothing below needs ep->v again.
        let mut nready: c_int = 0;
        let mut s: c_int = 0;
        while s < nout {
            let o = os.add(s as usize);
            s += 1;
            let inp = (*ep).v.offset((*o).idx as isize);
            let mut m = (*o).mask;
            // A socket with both halves down is a real hangup. Linux only
            // reports EPOLLHUP once neither direction can make progress, which
            // is what "EOF on the read filter AND on the write filter" says.
            if (*inp).is_socket && (*o).eof_read && (*o).eof_write {
                m |= L_EPOLLHUP;
            }
            if m == 0 {
                continue;
            }
            if ((*inp).events & L_EPOLLONESHOT) != 0 {
                // Linux disarms the whole INTEREST after one reported event,
                // not the filter that fired. Every knote of this interest goes,
                // and oneshot_fired keeps the entry out of the match loop until
                // an EPOLL_CTL_MOD re-arms it.
                (*inp).oneshot_fired = true;
                let mut dch = MaybeUninit::<[KEvent; EP_MAX_CH]>::uninit();
                let dch = dch.as_mut_ptr().cast::<KEvent>();
                let dn = ep_changes(inp, EV_DELETE, dch, EP_MAX_CH as c_int);
                ep_submit(kq, dch, dn, dn);
            }
            let cookie = (*inp).data;
            let r = os.add(nready as usize);
            (*r).mask = m;
            (*r).data = cookie;
            nready += 1;
        }
        ep_unlock();

        // Belt and braces: nready <= nout <= kn <= maxevents already, so this
        // never bites. It is here because an interest that is counted but not
        // written would have been disarmed by the EPOLLONESHOT branch above
        // without ever being reported.
        if nready > maxevents {
            nready = maxevents;
        }

        if nready > 0 {
            // The copy into guest memory happens with g_ep_lock DROPPED and the
            // destination proven writable again, immediately before the store.
            // The probe at entry is a check-then-use across an unbounded wait:
            // a guest thread that munmap()s or mprotect()s the buffer while the
            // waiter is blocked -- ordinary behaviour when a guest tears down a
            // thread or an arena -- used to fault the host. Measured,
            // deterministic over three runs: validate, wait(-1) on another
            // thread, munmap, fire the event -> SIGSEGV, exit 139, with
            // g_ep_lock still held, so even a recovering fault handler would
            // have wedged every epoll instance in the process.
            //
            // What remains is a window of a few hundred nanoseconds between the
            // probe and the copy, rather than the whole wait, and no lock is
            // held across it. Closing it completely needs a store that reports
            // a fault instead of taking one; mach_vm_write() is not it --
            // measured on this machine, mach_vm_write() into a freshly
            // munmap()ed page of our own task returns KERN_SUCCESS -- so that
            // would take a recovery landing pad, which belongs in the signal
            // module and not here.
            let esz = size_of::<LinuxEpollEvent>();
            let dst = uevents.cast::<u8>();
            let mut written: c_int = 0;
            if range_ok(dst.cast::<c_void>(), nready as usize * esz, true) {
                written = nready;
            } else {
                // Part of the buffer went away. Linux's ep_send_events faults
                // one event at a time and keeps whatever it already copied,
                // returning EFAULT only when it copied nothing at all.
                while written < nready
                    && range_ok(
                        dst.wrapping_add(written as usize * esz).cast::<c_void>(),
                        esz,
                        true,
                    )
                {
                    written += 1;
                }
            }
            let mut s: c_int = 0;
            while s < written {
                // A byte copy from a local, not a member store through a
                // struct pointer aimed at the guest's address. The guest chose
                // this address and is promised 16 bytes per entry, never 8-byte
                // alignment, and aarch64 traps unaligned accesses to device
                // memory. Same discipline as the eventfd counter store in
                // lxrt_eventfd_read().
                let o = os.add(s as usize);
                let e = LinuxEpollEvent {
                    events: (*o).mask,
                    pad_aarch64: 0,
                    data: (*o).data,
                };
                ptr::copy_nonoverlapping(
                    ptr::addr_of!(e).cast::<u8>(),
                    dst.wrapping_add(s as usize * esz),
                    esz,
                );
                s += 1;
            }
            // Stated divergence on the EFAULT path: an EPOLLONESHOT interest
            // that was disarmed above is not re-armed when the copy-out fails,
            // whereas Linux's ep_send_events puts the event back on the ready
            // list. Reaching it means the guest unmapped the very buffer it
            // asked the kernel to fill while the call was in flight, which is
            // not a state it recovers from, and re-arming would mean holding
            // g_ep_lock across the guest-memory store again -- the exact thing
            // this block exists to avoid.
            ret = if written > 0 {
                written as c_long
            } else {
                lerr(EFAULT)
            };
            break;
        }
        if n == 0 {
            ret = 0; // kevent hit the deadline
            break;
        }
        if infinite {
            continue;
        }
        let mut now = Timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        clock_gettime(CLOCK_MONOTONIC, &mut now);
        if now.tv_sec > deadline.tv_sec
            || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)
        {
            ret = 0;
            break;
        }
    }

    if kv != kstack {
        free(kv.cast::<c_void>());
    }
    if os != ostack {
        free(os.cast::<c_void>());
    }

    ep_lock();
    (*ep).refs -= 1;
    ep_release(ep);
    ep_unlock();

    if swapped {
        // The swap is not atomic with the wait, which is the one thing
        // ppoll/epoll_pwait exist to provide. Closing the window needs a
        // kevent() that takes a mask and there is none, so the race is stated
        // here rather than hidden: a signal delivered between the unmask and
        // the kevent() runs its handler and is not turned into an EINTR return.
        // dispatch.c's do_ppoll() carries the same hole.
        lxrt_rt_sigprocmask(
            L_SIG_SETMASK,
            &oldmask,
            ptr::null_mut(),
            L_KERNEL_SIGSET_BYTES,
        );
    }
    ret
}

// The function-local statics of lxrt_epoll_pwait(): `dbg`, the per-timeout
// statistics and the time of the last report. The statistics are only touched
// by the process's first thread (me == getpid()), as in the C.
static PWAIT_DBG: AtomicI32 = AtomicI32::new(-1);

#[repr(C)]
struct PwaitStat {
    n: c_uint,
    timed_out: c_uint,
    ns: u64,
    worst: u64,
}

// 0 ms, 1 ms, 2-15 ms, more or forever
static PWAIT_ST: Shared<MaybeUninit<[PwaitStat; 4]>> =
    Shared(UnsafeCell::new(MaybeUninit::zeroed()));
static PWAIT_LAST_REPORT: Shared<u64> = Shared(UnsafeCell::new(0));

#[no_mangle]
pub unsafe extern "C" fn lxrt_epoll_pwait(
    epfd: c_int,
    levents: *mut c_void,
    maxevents: c_int,
    timeout_ms: c_int,
    lsigmask: *const u64,
    sigsetsize: usize,
) -> c_long {
    let mut rel = Timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    let infinite = timeout_ms < 0; // any negative value, not just -1
    if !infinite {
        rel.tv_sec = (timeout_ms / 1000) as i64;
        rel.tv_nsec = (timeout_ms % 1000) as c_long * 1_000_000;
    }
    // LXRT_FUTEX_DEBUG=1: the waits of the process's first thread, by
    // timeout asked: how many, how many timed out, and how long they took
    // (a game's main loop sleeping in its event pump).
    let mut dbg = PWAIT_DBG.load(Ordering::Relaxed);
    if dbg < 0 {
        dbg = !getenv(c"LXRT_FUTEX_DEBUG".as_ptr()).is_null() as c_int;
        PWAIT_DBG.store(dbg, Ordering::Relaxed);
    }
    if dbg == 0 {
        return pwait_common(
            epfd, levents, maxevents, &rel, infinite, lsigmask, sigsetsize,
        );
    }
    let st = PWAIT_ST.0.get().cast::<PwaitStat>();
    let last_report = PWAIT_LAST_REPORT.0.get();
    let me = lxrt_gettid();
    let t0 = lxrt_guest_clock_ns(1);
    let r = pwait_common(
        epfd, levents, maxevents, &rel, infinite, lsigmask, sigsetsize,
    );
    let t1 = lxrt_guest_clock_ns(1);
    if me == getpid() {
        let k: usize = if timeout_ms == 0 {
            0
        } else if timeout_ms == 1 {
            1
        } else if timeout_ms > 0 && timeout_ms < 16 {
            2
        } else {
            3
        };
        let s = st.add(k);
        let dt = t1.wrapping_sub(t0);
        (*s).n = (*s).n.wrapping_add(1);
        (*s).timed_out = (*s).timed_out.wrapping_add((r == 0) as c_uint);
        (*s).ns = (*s).ns.wrapping_add(dt);
        if dt > (*s).worst {
            (*s).worst = dt;
        }
        if t1.wrapping_sub(*last_report) > 4_000_000_000 {
            let what: [*const c_char; 4] = [
                c"0 ms".as_ptr(),
                c"1 ms".as_ptr(),
                c"2-15 ms".as_ptr(),
                c"longer".as_ptr(),
            ];
            for k in 0..4usize {
                let s = st.add(k);
                if (*s).n != 0 {
                    fprintf(
                        lxrt_trace_stream(),
                        c"[lxrt] epoll_pwait tid %d, timeout %s: %u calls, %u timed out, avg %.2f ms, worst %.1f ms\n"
                            .as_ptr(),
                        me,
                        *what.as_ptr().add(k),
                        (*s).n,
                        (*s).timed_out,
                        (*s).ns as f64 / (*s).n as f64 / 1e6,
                        (*s).worst as f64 / 1e6,
                    );
                }
            }
            ptr::write_bytes(st, 0, 4);
            *last_report = t1;
        }
    }
    r
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_epoll_pwait2(
    epfd: c_int,
    levents: *mut c_void,
    maxevents: c_int,
    ltimespec: *const c_void,
    lsigmask: *const u64,
    sigsetsize: usize,
) -> c_long {
    // epoll_pwait2 takes struct __kernel_timespec: two signed 64-bit fields on
    // every architecture, so it matches Darwin's struct timespec on arm64
    // (time_t is 64-bit, tv_nsec is a 64-bit long) field for field. The copy is
    // still explicit -- this is the same shape as the timeval trap in
    // dispatch.c, where Darwin's 32-bit tv_usec sits in what Linux uses as the
    // high half.
    let mut rel = Timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    let infinite = ltimespec.is_null(); // NULL means block forever
    if !infinite {
        if !range_ok(ltimespec, size_of::<LinuxTimespec>(), false) {
            return lerr(EFAULT);
        }
        let lts = ltimespec.cast::<LinuxTimespec>().read_unaligned();
        if lts.tv_sec < 0 || lts.tv_nsec < 0 || lts.tv_nsec >= 1_000_000_000 {
            return lerr(EINVAL);
        }
        // tv_sec is forwarded unclamped, exactly as Linux forwards it;
        // pwait_common() is where an absurd value becomes an infinite wait
        // rather than a signed-overflow deadline in the past.
        rel.tv_sec = lts.tv_sec;
        rel.tv_nsec = lts.tv_nsec as c_long;
    }
    pwait_common(
        epfd, levents, maxevents, &rel, infinite, lsigmask, sigsetsize,
    )
}
