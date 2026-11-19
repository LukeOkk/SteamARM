// The binder driver, as a host process (lxrun --binder-hub <dir>).
//
// Android's binder is a kernel driver. There is no Linux kernel here and no
// VM (AGENTS.md), and guest processes are separate macOS processes, so the
// driver's state has to live somewhere every process can reach: this hub.
// One per user, started on demand by the first guest that opens a binder
// device (runtime/binder.c), gone again once no process has one open.
//
// What it keeps is what drivers/android/binder.c keeps, with the same names
// where that helps a reader compare: procs (one per open of a device),
// threads, nodes (a process's own objects), refs (handles to another
// process's nodes), transactions with their stacks, work lists, death
// notifications, and each proc's receive buffer. The buffer is the guest's
// mmap of the device: a file the guest maps read-only and the hub maps
// read-write, so the hub copies each transaction straight into the
// receiver's buffer and hands it offsets into it, as the kernel does.
//
// It is single-threaded. Every message from a guest thread is processed to
// completion before the next, which gives the driver's locking for free. A
// guest thread that would sleep in BINDER_WRITE_READ is "parked": its result
// is simply not sent until work arrives for it.
//
// Deviations from the Linux driver, stated rather than hidden:
//   * no priorities or scheduler policy (min_priority is kept, not applied);
//   * no SELinux: a node that asks for the sender's security context
//     (FLAT_BINDER_FLAG_TXN_SECURITY_CTX) gets the label the sender's
//     runtime reports (LXRT_BINDER_SECCTX, default "u:r:unlabeled:s0");
//   * no freezer (BINDER_FREEZE is EINVAL, as on kernels before 5.11);
//   * poll readiness is per process, not per polling thread (see
//     proc_poll_wanted), which is exact for the one-poller processes
//     Android has (servicemanager, Looper-based services).
//
// This is a line-for-line port of the C driver: the state is the same
// intrusive lists of calloc'd objects, reached through raw pointers, and the
// log lines go through the C library's printf so their text is unchanged.

#![allow(clippy::missing_safety_doc)]

use std::ffi::{c_char, c_int, c_long, c_void};
use std::mem::{offset_of, size_of, zeroed};
use std::ptr::{self, addr_of_mut, null, null_mut};
use std::sync::atomic::{AtomicI32, Ordering};

// Linux errno values the protocol carries (the runtime returns them to the
// guest unchanged, so they are Linux numbers, never Darwin's).
const L_EPERM: c_int = 1;
const L_ESRCH: c_int = 3;
const L_EINTR: c_int = 4;
const L_EBADF: c_int = 9;
const L_EAGAIN: c_int = 11;
const L_ENOMEM: c_int = 12;
const L_EFAULT: c_int = 14;
const L_EBUSY: c_int = 16;
const L_EINVAL: c_int = 22;
const L_ENOSPC: c_int = 28;
const L_EPROTO: c_int = 71;

#[inline(always)]
const fn align8(x: u64) -> u64 {
    x.wrapping_add(7) & !7u64
}
const SZ_4M: u64 = 4 << 20;

// ------------------------------------------------------------ Darwin (arm64)

const EINTR: c_int = 4;
const ENOMEM: c_int = 12;
const EAGAIN: c_int = 35;
const SOL_SOCKET: c_int = 0xffff;
const SCM_RIGHTS: c_int = 1;
const MSG_PEEK: c_int = 0x2;
const MSG_DONTWAIT: c_int = 0x80;
const SO_SNDBUF: c_int = 0x1001;
const SO_RCVBUF: c_int = 0x1002;
const SO_SNDTIMEO: c_int = 0x1005;
const SO_RCVTIMEO: c_int = 0x1006;
const SOL_LOCAL: c_int = 0;
const LOCAL_PEERPID: c_int = 0x002;
const F_SETFD: c_int = 2;
const FD_CLOEXEC: c_int = 1;
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const MAP_SHARED: c_int = 1;
const MAP_FAILED: *mut c_void = !0usize as *mut c_void;
const SC_PAGESIZE: c_int = 29;
const O_WRONLY: c_int = 0x1;
const O_RDWR: c_int = 0x2;
const O_APPEND: c_int = 0x8;
const O_CREAT: c_int = 0x200;
const O_TRUNC: c_int = 0x400;
const O_CLOEXEC: c_int = 0x100_0000;
const LOCK_EX: c_int = 2;
const LOCK_NB: c_int = 4;
const AF_UNIX: c_int = 1;
const SOCK_STREAM: c_int = 1;
const SIGINT: c_int = 2;
const SIGPIPE: c_int = 13;
const SIGTERM: c_int = 15;
const SIGUSR1: c_int = 30;
const SIG_IGN: usize = 1;
const RLIMIT_NOFILE: c_int = 8;
const POLLIN: i16 = 0x1;
const IOLBF: c_int = 1;
const CLOCK_REALTIME: c_int = 0;
const S_IFMT: u16 = 0o170000;
const S_IFSOCK: u16 = 0o140000;

#[repr(C)]
#[derive(Clone, Copy)]
struct Timespec {
    tv_sec: c_long,
    tv_nsec: c_long,
}

#[repr(C)]
struct Timeval {
    tv_sec: c_long,
    tv_usec: i32,
}

#[repr(C)]
struct Rlimit {
    rlim_cur: u64,
    rlim_max: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Iovec {
    iov_base: *mut c_void,
    iov_len: usize,
}

#[repr(C)]
struct MsgHdr {
    msg_name: *mut c_void,
    msg_namelen: u32,
    msg_iov: *mut Iovec,
    msg_iovlen: c_int,
    msg_control: *mut c_void,
    msg_controllen: u32,
    msg_flags: c_int,
}

#[repr(C)]
struct CmsgHdr {
    cmsg_len: u32,
    cmsg_level: c_int,
    cmsg_type: c_int,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct PollFd {
    fd: c_int,
    events: i16,
    revents: i16,
}

#[repr(C)]
struct SockaddrUn {
    sun_len: u8,
    sun_family: u8,
    sun_path: [c_char; 104],
}

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

const _: () = assert!(size_of::<Stat>() == 144);
const _: () = assert!(offset_of!(Stat, st_size) == 96);
const _: () = assert!(size_of::<MsgHdr>() == 48);
const _: () = assert!(size_of::<SockaddrUn>() == 106);
const _: () = assert!(size_of::<Timeval>() == 16);

// <sys/socket.h>: control messages are aligned to 4 bytes on Darwin.
const CMSG_HDR: usize = 12; // __DARWIN_ALIGN32(sizeof(struct cmsghdr))
#[inline(always)]
const fn darwin_align32(x: usize) -> usize {
    x.wrapping_add(3) & !3usize
}
#[inline(always)]
const fn cmsg_space(l: usize) -> usize {
    CMSG_HDR + darwin_align32(l)
}
#[inline(always)]
const fn cmsg_len(l: usize) -> usize {
    CMSG_HDR + l
}
#[inline(always)]
unsafe fn cmsg_data(c: *mut CmsgHdr) -> *mut u8 {
    (c as *mut u8).wrapping_add(CMSG_HDR)
}
#[inline(always)]
unsafe fn cmsg_firsthdr(m: *const MsgHdr) -> *mut CmsgHdr {
    if (*m).msg_controllen as usize >= size_of::<CmsgHdr>() {
        (*m).msg_control as *mut CmsgHdr
    } else {
        null_mut()
    }
}
#[inline(always)]
unsafe fn cmsg_nxthdr(m: *const MsgHdr, c: *mut CmsgHdr) -> *mut CmsgHdr {
    if c.is_null() {
        return cmsg_firsthdr(m);
    }
    let next = (c as *mut u8).wrapping_add(darwin_align32((*c).cmsg_len as usize));
    let end = ((*m).msg_control as *mut u8).wrapping_add((*m).msg_controllen as usize);
    if next.wrapping_add(CMSG_HDR) > end {
        null_mut()
    } else {
        next as *mut CmsgHdr
    }
}

#[link(name = "System")]
unsafe extern "C" {
    static mut __stderrp: *mut c_void;

    fn __error() -> *mut c_int;
    fn _exit(status: c_int) -> !;
    fn accept(fd: c_int, addr: *mut c_void, len: *mut u32) -> c_int;
    fn atoi(s: *const c_char) -> c_int;
    fn bind(fd: c_int, addr: *const c_void, len: u32) -> c_int;
    fn calloc(n: usize, s: usize) -> *mut c_void;
    fn chmod(path: *const c_char, mode: u16) -> c_int;
    fn clock_gettime(clock: c_int, ts: *mut Timespec) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn dprintf(fd: c_int, fmt: *const c_char, ...) -> c_int;
    fn dup2(from: c_int, to: c_int) -> c_int;
    fn fclose(f: *mut c_void) -> c_int;
    fn fcntl(fd: c_int, cmd: c_int, ...) -> c_int;
    fn flock(fd: c_int, op: c_int) -> c_int;
    fn fopen(path: *const c_char, mode: *const c_char) -> *mut c_void;
    fn fork() -> c_int;
    fn fprintf(f: *mut c_void, fmt: *const c_char, ...) -> c_int;
    fn fputc(c: c_int, f: *mut c_void) -> c_int;
    fn free(p: *mut c_void);
    fn fstat(fd: c_int, st: *mut Stat) -> c_int;
    fn ftruncate(fd: c_int, len: i64) -> c_int;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn getpid() -> c_int;
    fn getrlimit(what: c_int, rl: *mut Rlimit) -> c_int;
    fn getsockopt(fd: c_int, level: c_int, name: c_int, val: *mut c_void, len: *mut u32) -> c_int;
    fn listen(fd: c_int, backlog: c_int) -> c_int;
    fn malloc(n: usize) -> *mut c_void;
    fn mmap(addr: *mut c_void, len: usize, prot: c_int, flags: c_int, fd: c_int, off: i64) -> *mut c_void;
    fn munmap(addr: *mut c_void, len: usize) -> c_int;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn perror(s: *const c_char);
    fn poll(fds: *mut PollFd, n: u32, timeout: c_int) -> c_int;
    fn realloc(p: *mut c_void, n: usize) -> *mut c_void;
    fn recv(fd: c_int, buf: *mut c_void, len: usize, flags: c_int) -> isize;
    fn recvmsg(fd: c_int, m: *mut MsgHdr, flags: c_int) -> isize;
    fn send(fd: c_int, buf: *const c_void, len: usize, flags: c_int) -> isize;
    fn sendmsg(fd: c_int, m: *const MsgHdr, flags: c_int) -> isize;
    fn setrlimit(what: c_int, rl: *const Rlimit) -> c_int;
    fn setsid() -> c_int;
    fn setsockopt(fd: c_int, level: c_int, name: c_int, val: *const c_void, len: u32) -> c_int;
    fn setvbuf(f: *mut c_void, buf: *mut c_char, mode: c_int, size: usize) -> c_int;
    fn signal(sig: c_int, handler: usize) -> usize;
    fn snprintf(buf: *mut c_char, n: usize, fmt: *const c_char, ...) -> c_int;
    fn socket(domain: c_int, ty: c_int, proto: c_int) -> c_int;
    fn stat(path: *const c_char, st: *mut Stat) -> c_int;
    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
    fn strerror(e: c_int) -> *mut c_char;
    fn strlen(s: *const c_char) -> usize;
    fn sysconf(name: c_int) -> c_long;
    fn time(t: *mut c_long) -> c_long;
    fn unlink(path: *const c_char) -> c_int;
    fn usleep(us: u32) -> c_int;
}

#[inline(always)]
unsafe fn errno() -> c_int {
    *__error()
}

// ------------------------------------------------------------ Linux UAPI
// (runtime/binder.h: include/uapi/linux/android/binder.h, 64-bit layout,
// protocol version 8)
//
// Linux's _IOC encoding. Darwin's <sys/ioccom.h> puts IN and OUT the other
// way round (_IOW is 0x80000000 there, 0x40000000 on Linux), so the Linux
// macros are spelled out rather than borrowed.
const fn lx_ioc(dir: u32, ty: u8, nr: u32, size: usize) -> u32 {
    (dir << 30) | ((size as u32) << 16) | ((ty as u32) << 8) | nr
}
const fn lx_io(t: u8, n: u32) -> u32 {
    lx_ioc(0, t, n, 0)
}
const fn lx_iow(t: u8, n: u32, s: usize) -> u32 {
    lx_ioc(1, t, n, s)
}
const fn lx_ior(t: u8, n: u32, s: usize) -> u32 {
    lx_ioc(2, t, n, s)
}
const fn lx_iowr(t: u8, n: u32, s: usize) -> u32 {
    lx_ioc(3, t, n, s)
}
#[inline(always)]
const fn lx_ioc_size(c: u32) -> u32 {
    (c >> 16) & 0x3fff
}

const fn b_pack_chars(c1: u8, c2: u8, c3: u8, c4: u8) -> u32 {
    ((c1 as u32) << 24) | ((c2 as u32) << 16) | ((c3 as u32) << 8) | c4 as u32
}
const B_TYPE_LARGE: u8 = 0x85;
const BINDER_TYPE_BINDER: u32 = b_pack_chars(b's', b'b', b'*', B_TYPE_LARGE);
const BINDER_TYPE_WEAK_BINDER: u32 = b_pack_chars(b'w', b'b', b'*', B_TYPE_LARGE);
const BINDER_TYPE_HANDLE: u32 = b_pack_chars(b's', b'h', b'*', B_TYPE_LARGE);
const BINDER_TYPE_WEAK_HANDLE: u32 = b_pack_chars(b'w', b'h', b'*', B_TYPE_LARGE);
const BINDER_TYPE_FD: u32 = b_pack_chars(b'f', b'd', b'*', B_TYPE_LARGE);
const BINDER_TYPE_FDA: u32 = b_pack_chars(b'f', b'd', b'a', B_TYPE_LARGE);
const BINDER_TYPE_PTR: u32 = b_pack_chars(b'p', b't', b'*', B_TYPE_LARGE);

const FLAT_BINDER_FLAG_PRIORITY_MASK: u32 = 0xff;
const FLAT_BINDER_FLAG_ACCEPTS_FDS: u32 = 0x100;
const FLAT_BINDER_FLAG_TXN_SECURITY_CTX: u32 = 0x1000;
const BINDER_BUFFER_FLAG_HAS_PARENT: u32 = 0x01;

// struct flat_binder_object. `binder` is the C union { uint64_t binder;
// uint32_t handle; }: the handle is its low half (little endian).
#[repr(C)]
#[derive(Clone, Copy)]
struct FlatBinderObject {
    hdr_type: u32,
    flags: u32,
    binder: u64,
    cookie: u64,
}
impl FlatBinderObject {
    #[inline(always)]
    fn handle(&self) -> u32 {
        self.binder as u32
    }
    // `fp.handle = v` in C: only the low four bytes change.
    #[inline(always)]
    fn set_handle(&mut self, v: u32) {
        self.binder = (self.binder & !0xffff_ffffu64) | v as u64;
    }
}
// struct binder_fd_object: union { uint64_t pad_binder; uint32_t fd; }.
#[repr(C)]
#[derive(Clone, Copy)]
struct BinderFdObject {
    hdr_type: u32,
    pad_flags: u32,
    pad_binder: u64,
    cookie: u64,
}
const BINDER_FD_OBJECT_FD: u64 = 8; // offsetof(struct binder_fd_object, fd)
#[repr(C)]
#[derive(Clone, Copy)]
struct BinderBufferObject {
    hdr_type: u32,
    flags: u32,
    buffer: u64,
    length: u64,
    parent: u64,
    parent_offset: u64,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct BinderFdArrayObject {
    hdr_type: u32,
    pad: u32,
    num_fds: u64,
    parent: u64,
    parent_offset: u64,
}

const BINDER_CURRENT_PROTOCOL_VERSION: i32 = 8;

#[repr(C)]
#[derive(Clone, Copy)]
struct BinderNodeDebugInfo {
    ptr: u64,
    cookie: u64,
    has_strong_ref: u32,
    has_weak_ref: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct BinderNodeInfoForRef {
    handle: u32,
    strong_count: u32,
    weak_count: u32,
    reserved1: u32,
    reserved2: u32,
    reserved3: u32,
}

const TF_ONE_WAY: u32 = 0x01;
const TF_ACCEPT_FDS: u32 = 0x10;
const TF_CLEAR_BUF: u32 = 0x20;

// struct binder_transaction_data. `target` is the C union { uint32_t handle;
// uint64_t ptr; }; data_buffer/data_offsets are data.ptr.buffer/.offsets.
#[repr(C)]
#[derive(Clone, Copy)]
struct BinderTransactionData {
    target: u64,
    cookie: u64,
    code: u32,
    flags: u32,
    sender_pid: i32,
    sender_euid: u32,
    data_size: u64,
    offsets_size: u64,
    data_buffer: u64,
    data_offsets: u64,
}
impl BinderTransactionData {
    #[inline(always)]
    fn target_handle(&self) -> u32 {
        self.target as u32
    }
}
#[repr(C)]
#[derive(Clone, Copy)]
struct BinderTransactionDataSecctx {
    transaction_data: BinderTransactionData,
    secctx: u64,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct BinderPtrCookie {
    ptr: u64,
    cookie: u64,
}
// struct binder_handle_cookie is packed: u32 handle, then u64 cookie at 4.
const SIZEOF_BINDER_HANDLE_COOKIE: usize = 12;
const SIZEOF_BINDER_TRANSACTION_DATA_SG: usize = size_of::<BinderTransactionData>() + 8;

const _: () = assert!(size_of::<FlatBinderObject>() == 24);
const _: () = assert!(size_of::<BinderFdObject>() == 24);
const _: () = assert!(size_of::<BinderBufferObject>() == 40);
const _: () = assert!(size_of::<BinderFdArrayObject>() == 32);
const _: () = assert!(size_of::<BinderTransactionData>() == 64);
const _: () = assert!(size_of::<BinderTransactionDataSecctx>() == 72);

const BINDER_SET_MAX_THREADS: u32 = lx_iow(b'b', 5, 4);
const BINDER_SET_CONTEXT_MGR: u32 = lx_iow(b'b', 7, 4);
const BINDER_THREAD_EXIT: u32 = lx_iow(b'b', 8, 4);
const BINDER_VERSION: u32 = lx_iowr(b'b', 9, 4);
const BINDER_GET_NODE_DEBUG_INFO: u32 = lx_iowr(b'b', 11, size_of::<BinderNodeDebugInfo>());
const BINDER_GET_NODE_INFO_FOR_REF: u32 = lx_iowr(b'b', 12, size_of::<BinderNodeInfoForRef>());
const BINDER_SET_CONTEXT_MGR_EXT: u32 = lx_iow(b'b', 13, size_of::<FlatBinderObject>());

const BR_OK: u32 = lx_io(b'r', 1);
const BR_TRANSACTION_SEC_CTX: u32 = lx_ior(b'r', 2, size_of::<BinderTransactionDataSecctx>());
const BR_TRANSACTION: u32 = lx_ior(b'r', 2, size_of::<BinderTransactionData>());
const BR_REPLY: u32 = lx_ior(b'r', 3, size_of::<BinderTransactionData>());
const BR_DEAD_REPLY: u32 = lx_io(b'r', 5);
const BR_TRANSACTION_COMPLETE: u32 = lx_io(b'r', 6);
const BR_INCREFS: u32 = lx_ior(b'r', 7, size_of::<BinderPtrCookie>());
const BR_ACQUIRE: u32 = lx_ior(b'r', 8, size_of::<BinderPtrCookie>());
const BR_RELEASE: u32 = lx_ior(b'r', 9, size_of::<BinderPtrCookie>());
const BR_DECREFS: u32 = lx_ior(b'r', 10, size_of::<BinderPtrCookie>());
const BR_NOOP: u32 = lx_io(b'r', 12);
const BR_DEAD_BINDER: u32 = lx_ior(b'r', 15, 8);
const BR_CLEAR_DEATH_NOTIFICATION_DONE: u32 = lx_ior(b'r', 16, 8);
const BR_FAILED_REPLY: u32 = lx_io(b'r', 17);

const BC_TRANSACTION: u32 = lx_iow(b'c', 0, size_of::<BinderTransactionData>());
const BC_REPLY: u32 = lx_iow(b'c', 1, size_of::<BinderTransactionData>());
const BC_ACQUIRE_RESULT: u32 = lx_iow(b'c', 2, 4);
const BC_FREE_BUFFER: u32 = lx_iow(b'c', 3, 8);
const BC_INCREFS: u32 = lx_iow(b'c', 4, 4);
const BC_ACQUIRE: u32 = lx_iow(b'c', 5, 4);
const BC_RELEASE: u32 = lx_iow(b'c', 6, 4);
const BC_DECREFS: u32 = lx_iow(b'c', 7, 4);
const BC_INCREFS_DONE: u32 = lx_iow(b'c', 8, size_of::<BinderPtrCookie>());
const BC_ACQUIRE_DONE: u32 = lx_iow(b'c', 9, size_of::<BinderPtrCookie>());
const BC_ATTEMPT_ACQUIRE: u32 = lx_iow(b'c', 10, 8);
const BC_REGISTER_LOOPER: u32 = lx_io(b'c', 11);
const BC_ENTER_LOOPER: u32 = lx_io(b'c', 12);
const BC_EXIT_LOOPER: u32 = lx_io(b'c', 13);
const BC_REQUEST_DEATH_NOTIFICATION: u32 = lx_iow(b'c', 14, SIZEOF_BINDER_HANDLE_COOKIE);
const BC_CLEAR_DEATH_NOTIFICATION: u32 = lx_iow(b'c', 15, SIZEOF_BINDER_HANDLE_COOKIE);
const BC_DEAD_BINDER_DONE: u32 = lx_iow(b'c', 16, 8);
const BC_TRANSACTION_SG: u32 = lx_iow(b'c', 17, SIZEOF_BINDER_TRANSACTION_DATA_SG);
const BC_REPLY_SG: u32 = lx_iow(b'c', 18, SIZEOF_BINDER_TRANSACTION_DATA_SG);

// ------------------------------------------------------------ wire protocol
// (runtime/binder.h; shared with runtime/binder.c)
const BH_VERSION: u32 = 2; // 2: BH_THREAD_ACK
const BH_MAX_FDS: usize = 250; // file descriptors in one message (SCM_RIGHTS)
const BH_MAX_MSG: u64 = 8 << 20; // no request is larger than 2 receive buffers

const BH_HELLO: u32 = 1;
const BH_HELLO_ACK: u32 = 2;
const BH_THREAD: u32 = 3;
const BH_WRITE_READ: u32 = 4;
const BH_CANCEL: u32 = 5;
const BH_IOCTL: u32 = 6;
const BH_MMAP: u32 = 7;
const BH_POLL: u32 = 8;
const BH_RESULT: u32 = 9;
const BH_THREAD_ACK: u32 = 10;

#[repr(C)]
#[derive(Clone, Copy)]
struct BhHdr {
    type_: u32,
    nfds: u32, // descriptors carried with this message (SCM_RIGHTS)
    len: u64,  // payload bytes after this header
    seq: u64,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct BhHello {
    version: u32,
    pid: i32, // the guest's pid (the lxrun process)
    euid: u32,
    context: u32, // 0 binder, 1 hwbinder, 2 vndbinder
    secctx: [c_char; 64],
}
#[repr(C)]
struct BhHelloAck {
    ret: i32,
    proc_id: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct BhThread {
    tid: i32,
    pad: u32,
}
const BH_WR_NONBLOCK: u32 = 1;
#[repr(C)]
#[derive(Clone, Copy)]
struct BhWr {
    write_len: u64,
    read_avail: u64,
    read_consumed: u64,
    flags: u32,
    pad: u32,
}
#[repr(C)]
#[derive(Clone, Copy)]
struct BhTxnAtt {
    fault: i32,
    nfds: u32,
    data_size: u64,
    offsets_size: u64,
    sg_size: u64,
}
// Not a Linux ioctl: the runtime telling the hub the guest unmapped its
// receive buffer (Linux: binder_vma_close).
const BH_IOCTL_UNMAPPED: u32 = 0x0b1d_0001;
#[repr(C)]
#[derive(Clone, Copy)]
struct BhIoctl {
    cmd: u32,
    pad: u32,
    arg: [u8; 64],
}
#[repr(C)]
#[derive(Clone, Copy)]
struct BhMmap {
    base: u64,
    size: u64,
}
#[repr(C)]
struct BhResult {
    ret: i64,            // 0 or -errno (Linux numbering)
    write_consumed: u64, // bytes of this request's write data consumed
    read_len: u64,       // BR bytes that follow
    spawn_looper: u32,   // put BR_SPAWN_LOOPER at the read buffer's start
    nfixups: u32,        // u64 buffer offsets that follow the BR bytes
    ncloses: u32,        // i32 descriptors to close (freed FDA buffers)
    bell_pending: u32,   // a doorbell should remain unread
    bells: u64,          // doorbells written on the process channel so far
    arg: [u8; 64],       // BH_IOCTL's output
}

const _: () = assert!(size_of::<BhHdr>() == 24);
const _: () = assert!(size_of::<BhHello>() == 80);
const _: () = assert!(size_of::<BhWr>() == 32);
const _: () = assert!(size_of::<BhTxnAtt>() == 32);
const _: () = assert!(size_of::<BhIoctl>() == 72);
const _: () = assert!(size_of::<BhResult>() == 112);

// ------------------------------------------------------------------ lists

#[repr(C)]
struct List {
    prev: *mut List,
    next: *mut List,
}
#[inline(always)]
unsafe fn list_init(l: *mut List) {
    (*l).prev = l;
    (*l).next = l;
}
#[inline(always)]
unsafe fn list_empty(l: *const List) -> bool {
    (*l).next as *const List == l
}
#[inline(always)]
unsafe fn list_add_tail(n: *mut List, h: *mut List) {
    (*n).prev = (*h).prev;
    (*n).next = h;
    (*(*h).prev).next = n;
    (*h).prev = n;
}
#[inline(always)]
unsafe fn list_add_before(n: *mut List, at: *mut List) {
    list_add_tail(n, at);
}
#[inline(always)]
unsafe fn list_del_init(n: *mut List) {
    if (*n).next == n || (*n).next.is_null() {
        (*n).prev = n;
        (*n).next = n;
        return;
    }
    (*(*n).prev).next = (*n).next;
    (*(*n).next).prev = (*n).prev;
    (*n).prev = n;
    (*n).next = n;
}
macro_rules! container_of {
    ($p:expr, $T:ty, $($m:tt)+) => {
        (($p) as *mut u8).wrapping_sub(offset_of!($T, $($m)+)) as *mut $T
    };
}

// ------------------------------------------------------------------ state

const W_TRANSACTION: c_int = 1;
const W_TRANSACTION_COMPLETE: c_int = 2;
const W_RETURN_ERROR: c_int = 3;
const W_NODE: c_int = 4;
const W_DEAD_BINDER: c_int = 5;
const W_DEAD_BINDER_AND_CLEAR: c_int = 6;
const W_CLEAR_DEATH_NOTIFICATION: c_int = 7;

#[repr(C)]
struct Work {
    entry: List,
    type_: c_int,
}
#[repr(C)]
struct ErrorWork {
    work: Work,
    cmd: u32,
}

const LOOPER_REGISTERED: u32 = 0x01;
const LOOPER_ENTERED: u32 = 0x02;
const LOOPER_EXITED: u32 = 0x04;
const LOOPER_INVALID: u32 = 0x08;
const LOOPER_WAITING: u32 = 0x10;
const LOOPER_POLL: u32 = 0x20;

#[repr(C)]
struct Node {
    work: Work,        // W_NODE: BR_INCREFS/ACQUIRE/RELEASE/DECREFS
    proc_entry: List,  // proc->nodes, or g_dead_nodes
    proc_: *mut Proc,  // NULL once the owner died
    refs: List,        // struct ref.node_entry
    internal_strong_refs: c_int,
    local_strong_refs: c_int,
    local_weak_refs: c_int,
    ptr: u64,
    cookie: u64,
    has_strong_ref: bool,
    pending_strong_ref: bool,
    has_weak_ref: bool,
    pending_weak_ref: bool,
    has_async_transaction: bool,
    accept_fds: bool,
    txn_security_ctx: bool,
    min_priority: c_int,
    async_todo: List,
    debug_id: u32,
}

#[repr(C)]
struct RefDeath {
    work: Work,
    cookie: u64,
}

#[repr(C)]
struct Ref {
    proc_entry: List, // proc->refs, ascending desc
    node_entry: List, // node->refs
    proc_: *mut Proc,
    node: *mut Node,
    desc: u32,
    strong: c_int,
    weak: c_int,
    death: *mut RefDeath,
}

#[repr(C)]
struct Buffer {
    entry: List, // proc->buffers, ascending off
    off: u64,
    size: u64,
    data_size: u64,
    offsets_size: u64,
    extra_size: u64,
    allow_user_free: bool,
    async_: bool,
    clear_on_free: bool,
    transaction: *mut Transaction,
    target_node: *mut Node,
}

#[repr(C)]
struct Transaction {
    work: Work,
    debug_id: u32,
    from: *mut Thread,
    from_parent: *mut Transaction,
    to_parent: *mut Transaction,
    to_proc: *mut Proc,
    to_thread: *mut Thread,
    buffer: *mut Buffer,
    code: u32,
    flags: u32,
    sender_euid: u32,
    need_reply: bool,
    security_ctx: u64,  // guest address in the target, 0: none
    nfix: c_int,        // descriptors held for the receiver
    fix_fd: *mut c_int,
    fix_off: *mut u64,  // where each goes in the target buffer
}

#[repr(C)]
struct Thread {
    proc_entry: List,
    proc_: *mut Proc,
    fd: c_int, // thread channel
    tid: c_int,
    looper: u32,
    looper_need_return: bool,
    process_todo: bool,
    is_dead: bool,
    todo: List,
    transaction_stack: *mut Transaction,
    return_error: ErrorWork,
    reply_error: ErrorWork,
    // A BINDER_WRITE_READ waiting for work.
    parked: bool,
    waiting_entry: List, // proc->waiting_threads (proc work only)
    park_seq: u64,
    park_write_consumed: u64,
    park_read_avail: u64,
    park_read_consumed: u64,
    park_nonblock: bool,
    // FDA descriptors to close in this thread's process (buffers it freed),
    // carried by its next result.
    closes: *mut c_int,
    ncloses: c_int,
    ccloses: c_int,
    held: *mut c_int, // descriptors sent with the last result (send_result)
    nheld: c_int,
    cheld: c_int,
}

#[repr(C)]
struct Proc {
    entry: List, // g_procs
    fd: c_int,   // process channel
    hello: bool,
    id: u32,
    pid: c_int,
    host_pid: c_int, // LOCAL_PEERPID for channel verification
    euid: u32,
    context: c_int,
    secctx: [c_char; 64],
    threads: List,
    nodes: List,
    refs: List,
    todo: List,
    delivered_death: List,
    waiting_threads: List,
    max_threads: c_int,
    requested_threads: c_int,
    requested_threads_started: c_int,
    is_dead: bool,
    map: *mut u8,   // the receive buffer (hub's view)
    unmapped: bool, // the guest unmapped it: allocate nothing more
    map_size: u64,
    map_host_size: u64,
    user_base: u64,
    free_async: i64,
    buffers: List,
    bells: u64,
    bells_drained: u64,
}

static mut G_PROCS: List = List { prev: null_mut(), next: null_mut() };
static mut G_DEAD_NODES: List = List { prev: null_mut(), next: null_mut() };
static mut G_CTX_MGR: [*mut Node; 3] = [null_mut(); 3];
static mut G_CTX_MGR_UID: [u32; 3] = [0; 3];
static mut G_CTX_MGR_UID_SET: [bool; 3] = [false; 3];
static mut G_DEBUG_ID: u32 = 0;
static mut G_PROC_ID: u32 = 0;
static mut G_VERBOSE: bool = false;
static mut G_DIR: *const c_char = null();

// g_ctx_mgr[context] and friends, indexed as C does (no bounds check: the
// context is 0 until the handshake, which accepts only 0..2).
#[inline(always)]
unsafe fn ctx_mgr(context: c_int) -> *mut *mut Node {
    addr_of_mut!(G_CTX_MGR).cast::<*mut Node>().wrapping_offset(context as isize)
}
#[inline(always)]
unsafe fn ctx_mgr_uid(context: c_int) -> *mut u32 {
    addr_of_mut!(G_CTX_MGR_UID).cast::<u32>().wrapping_offset(context as isize)
}
#[inline(always)]
unsafe fn ctx_mgr_uid_set(context: c_int) -> *mut bool {
    addr_of_mut!(G_CTX_MGR_UID_SET).cast::<bool>().wrapping_offset(context as isize)
}

unsafe fn hub_log_begin() {
    let mut ts = Timespec { tv_sec: 0, tv_nsec: 0 };
    clock_gettime(CLOCK_REALTIME, &mut ts);
    fprintf(
        __stderrp,
        c"[binder-hub %ld.%03ld] ".as_ptr(),
        ts.tv_sec as c_long,
        (ts.tv_nsec / 1_000_000) as c_long,
    );
}

// hub_log(fmt, ...): a printf format and C-typed arguments (int, unsigned,
// long long, char *), exactly as the C driver logs. Arguments that read errno
// are computed by the caller before the line starts.
macro_rules! hub_log {
    ($fmt:literal $(, $arg:expr)* $(,)?) => {{
        hub_log_begin();
        fprintf(__stderrp, concat!($fmt, "\0").as_ptr().cast::<c_char>() $(, $arg)*);
        fputc(b'\n' as c_int, __stderrp);
    }};
}
macro_rules! user_error {
    ($fmt:literal $(, $arg:expr)* $(,)?) => {
        hub_log!(concat!("user error: ", $fmt) $(, $arg)*)
    };
}
macro_rules! debug {
    ($fmt:literal $(, $arg:expr)* $(,)?) => {
        if G_VERBOSE {
            hub_log!($fmt $(, $arg)*);
        }
    };
}

unsafe fn out_of_memory() -> ! {
    hub_log!("out of memory");
    _exit(70)
}

unsafe fn xcalloc(n: usize, s: usize) -> *mut c_void {
    let p = calloc(if n != 0 { n } else { 1 }, if s != 0 { s } else { 1 });
    if p.is_null() {
        out_of_memory();
    }
    p
}
#[inline(always)]
unsafe fn xnew<T>() -> *mut T {
    xcalloc(1, size_of::<T>()) as *mut T
}

// ------------------------------------------------------------------ work

#[inline(always)]
unsafe fn enqueue_work(w: *mut Work, target: *mut List) {
    list_add_tail(addr_of_mut!((*w).entry), target);
}
#[inline(always)]
unsafe fn dequeue_work(w: *mut Work) {
    list_del_init(addr_of_mut!((*w).entry));
}
unsafe fn dequeue_head(l: *mut List) -> *mut Work {
    if list_empty(l) {
        return null_mut();
    }
    let w = container_of!((*l).next, Work, entry);
    list_del_init(addr_of_mut!((*w).entry));
    w
}
#[inline(always)]
unsafe fn work_queued(w: *mut Work) -> bool {
    !(*w).entry.next.is_null() && (*w).entry.next != addr_of_mut!((*w).entry)
}

unsafe fn enqueue_thread_work(t: *mut Thread, w: *mut Work) {
    enqueue_work(w, addr_of_mut!((*t).todo));
    (*t).process_todo = true;
}
// Work the thread handles after the rest (a sync TRANSACTION_COMPLETE, a
// node's first references): the thread does not wake for it alone.
unsafe fn enqueue_deferred_thread_work(t: *mut Thread, w: *mut Work) {
    enqueue_work(w, addr_of_mut!((*t).todo));
}

unsafe fn available_for_proc_work(t: *mut Thread) -> bool {
    (*t).transaction_stack.is_null()
        && list_empty(addr_of_mut!((*t).todo))
        && ((*t).looper & (LOOPER_ENTERED | LOOPER_REGISTERED)) != 0
}
unsafe fn has_work(t: *mut Thread, do_proc_work: bool) -> bool {
    (*t).process_todo
        || (*t).looper_need_return
        || (do_proc_work && !list_empty(addr_of_mut!((*(*t).proc_).todo)))
}

// binder_select_thread_ilocked: a thread asleep waiting for process work.
unsafe fn select_thread(p: *mut Proc) -> *mut Thread {
    if list_empty(addr_of_mut!((*p).waiting_threads)) {
        return null_mut();
    }
    let t = container_of!((*p).waiting_threads.next, Thread, waiting_entry);
    list_del_init(addr_of_mut!((*t).waiting_entry));
    t
}

// ------------------------------------------------------------------ nodes

unsafe fn get_node(p: *mut Proc, ptr_: u64) -> *mut Node {
    let head = addr_of_mut!((*p).nodes);
    let mut e = (*head).next;
    while e != head {
        let n = container_of!(e, Node, proc_entry);
        if (*n).ptr == ptr_ {
            return n;
        }
        e = (*e).next;
    }
    null_mut()
}

unsafe fn new_node(p: *mut Proc, fp: *const FlatBinderObject) -> *mut Node {
    let n: *mut Node = xnew();
    G_DEBUG_ID = G_DEBUG_ID.wrapping_add(1);
    (*n).debug_id = G_DEBUG_ID;
    (*n).proc_ = p;
    (*n).ptr = if !fp.is_null() { (*fp).binder } else { 0 };
    (*n).cookie = if !fp.is_null() { (*fp).cookie } else { 0 };
    (*n).work.type_ = W_NODE;
    list_init(addr_of_mut!((*n).work.entry));
    list_init(addr_of_mut!((*n).refs));
    list_init(addr_of_mut!((*n).async_todo));
    let flags: u32 = if !fp.is_null() { (*fp).flags } else { 0 };
    (*n).min_priority = (flags & FLAT_BINDER_FLAG_PRIORITY_MASK) as c_int;
    (*n).accept_fds = (flags & FLAT_BINDER_FLAG_ACCEPTS_FDS) != 0;
    (*n).txn_security_ctx = (flags & FLAT_BINDER_FLAG_TXN_SECURITY_CTX) != 0;
    list_add_tail(addr_of_mut!((*n).proc_entry), addr_of_mut!((*p).nodes));
    debug!("%d new node %u u%016llx c%016llx", (*p).pid, (*n).debug_id, (*n).ptr, (*n).cookie);
    n
}

unsafe fn free_node(n: *mut Node) {
    list_del_init(addr_of_mut!((*n).proc_entry));
    dequeue_work(addr_of_mut!((*n).work));
    free(n as *mut c_void);
}

// binder_inc_node_nilocked
unsafe fn inc_node(n: *mut Node, strong: bool, internal: bool, target_list: *mut List) -> c_int {
    let p = (*n).proc_;
    if strong {
        if internal {
            if target_list.is_null()
                && (*n).internal_strong_refs == 0
                && !(!p.is_null() && n == *ctx_mgr((*p).context) && (*n).has_strong_ref)
            {
                hub_log!("invalid inc strong node for %u", (*n).debug_id);
                return -L_EINVAL;
            }
            (*n).internal_strong_refs = (*n).internal_strong_refs.wrapping_add(1);
        } else {
            (*n).local_strong_refs = (*n).local_strong_refs.wrapping_add(1);
        }
        if !(*n).has_strong_ref && !target_list.is_null() {
            let t = container_of!(target_list, Thread, todo);
            dequeue_work(addr_of_mut!((*n).work));
            enqueue_deferred_thread_work(t, addr_of_mut!((*n).work));
        }
    } else {
        if !internal {
            (*n).local_weak_refs = (*n).local_weak_refs.wrapping_add(1);
        }
        if !(*n).has_weak_ref && !work_queued(addr_of_mut!((*n).work)) {
            if target_list.is_null() {
                hub_log!("invalid inc weak node for %u", (*n).debug_id);
                return -L_EINVAL;
            }
            enqueue_work(addr_of_mut!((*n).work), target_list);
        }
    }
    0
}

// binder_dec_node_nilocked; true: the caller must free the node.
unsafe fn dec_node_nilocked(n: *mut Node, strong: bool, internal: bool) -> bool {
    let p = (*n).proc_;
    if strong {
        if internal {
            (*n).internal_strong_refs = (*n).internal_strong_refs.wrapping_sub(1);
        } else {
            (*n).local_strong_refs = (*n).local_strong_refs.wrapping_sub(1);
        }
        if (*n).local_strong_refs != 0 || (*n).internal_strong_refs != 0 {
            return false;
        }
    } else {
        if !internal {
            (*n).local_weak_refs = (*n).local_weak_refs.wrapping_sub(1);
        }
        if (*n).local_weak_refs != 0 || !list_empty(addr_of_mut!((*n).refs)) {
            return false;
        }
    }
    if !p.is_null() && ((*n).has_strong_ref || (*n).has_weak_ref) {
        if !work_queued(addr_of_mut!((*n).work)) {
            enqueue_work(addr_of_mut!((*n).work), addr_of_mut!((*p).todo));
        }
    } else if list_empty(addr_of_mut!((*n).refs))
        && (*n).local_strong_refs == 0
        && (*n).local_weak_refs == 0
    {
        debug!(
            "%s node %u deleted",
            if !p.is_null() { c"refless".as_ptr() } else { c"dead".as_ptr() },
            (*n).debug_id
        );
        return true;
    }
    false
}
unsafe fn dec_node(n: *mut Node, strong: bool, internal: bool) {
    if dec_node_nilocked(n, strong, internal) {
        free_node(n);
    }
}

// ------------------------------------------------------------------ refs

unsafe fn get_ref(p: *mut Proc, desc: u32, need_strong: bool) -> *mut Ref {
    let head = addr_of_mut!((*p).refs);
    let mut e = (*head).next;
    while e != head {
        let r = container_of!(e, Ref, proc_entry);
        if (*r).desc == desc {
            if need_strong && (*r).strong == 0 {
                user_error!("%d tried to use weak ref %u as strong ref", (*p).pid, desc);
                return null_mut();
            }
            return r;
        }
        if (*r).desc > desc {
            break;
        }
        e = (*e).next;
    }
    null_mut()
}

// binder_get_ref_for_node_olocked + creation
unsafe fn get_ref_for_node(p: *mut Proc, n: *mut Node) -> *mut Ref {
    let head = addr_of_mut!((*p).refs);
    let mut e = (*head).next;
    while e != head {
        let r = container_of!(e, Ref, proc_entry);
        if (*r).node == n {
            return r;
        }
        e = (*e).next;
    }
    let r: *mut Ref = xnew();
    (*r).proc_ = p;
    (*r).node = n;
    // The lowest free descriptor; 0 is the context manager's alone.
    let mut desc: u32 = if n == *ctx_mgr((*p).context) { 0 } else { 1 };
    let mut at = head;
    e = (*head).next;
    while e != head {
        let o = container_of!(e, Ref, proc_entry);
        if (*o).desc > desc {
            at = e;
            break;
        }
        desc = (*o).desc.wrapping_add(1);
        e = (*e).next;
    }
    (*r).desc = desc;
    list_add_before(addr_of_mut!((*r).proc_entry), at);
    list_add_tail(addr_of_mut!((*r).node_entry), addr_of_mut!((*n).refs));
    debug!("%d new ref desc %u for node %u", (*p).pid, desc, (*n).debug_id);
    r
}

unsafe fn inc_ref(r: *mut Ref, strong: bool, target_list: *mut List) -> c_int {
    if strong {
        if (*r).strong == 0 {
            let ret = inc_node((*r).node, true, true, target_list);
            if ret != 0 {
                return ret;
            }
        }
        (*r).strong = (*r).strong.wrapping_add(1);
    } else {
        if (*r).weak == 0 {
            let ret = inc_node((*r).node, false, true, target_list);
            if ret != 0 {
                return ret;
            }
        }
        (*r).weak = (*r).weak.wrapping_add(1);
    }
    0
}

// binder_cleanup_ref_olocked + binder_free_ref
unsafe fn cleanup_ref(r: *mut Ref) {
    let n = (*r).node;
    list_del_init(addr_of_mut!((*r).proc_entry));
    if (*r).strong != 0 {
        dec_node_nilocked(n, true, true); // the weak path below decides deletion
    }
    list_del_init(addr_of_mut!((*r).node_entry));
    let del = dec_node_nilocked(n, false, true);
    if !(*r).death.is_null() {
        dequeue_work(addr_of_mut!((*(*r).death).work));
        free((*r).death as *mut c_void);
    }
    free(r as *mut c_void);
    if del {
        free_node(n);
    }
}

// true: the ref was deleted
unsafe fn dec_ref(r: *mut Ref, strong: bool) -> bool {
    if strong {
        if (*r).strong == 0 {
            user_error!("%d invalid dec strong, ref %u", (*(*r).proc_).pid, (*r).desc);
            return false;
        }
        (*r).strong -= 1;
        if (*r).strong == 0 {
            dec_node((*r).node, true, true);
        }
    } else {
        if (*r).weak == 0 {
            user_error!("%d invalid dec weak, ref %u", (*(*r).proc_).pid, (*r).desc);
            return false;
        }
        (*r).weak -= 1;
    }
    if (*r).strong == 0 && (*r).weak == 0 {
        cleanup_ref(r);
        return true;
    }
    false
}

unsafe fn inc_ref_for_node(
    p: *mut Proc,
    n: *mut Node,
    strong: bool,
    target_list: *mut List,
    desc: *mut u32,
) -> c_int {
    let r = get_ref_for_node(p, n);
    let ret = inc_ref(r, strong, target_list);
    *desc = (*r).desc;
    if ret != 0 && (*r).strong == 0 && (*r).weak == 0 {
        cleanup_ref(r);
    }
    ret
}

unsafe fn update_ref_for_handle(p: *mut Proc, desc: u32, increment: bool, strong: bool) -> c_int {
    let r = get_ref(p, desc, strong);
    if r.is_null() {
        return -L_EINVAL;
    }
    if increment {
        return inc_ref(r, strong, null_mut());
    }
    dec_ref(r, strong);
    0
}

// ------------------------------------------------------------------ buffers

unsafe fn alloc_buf(p: *mut Proc, mut size: u64, async_: bool, err: *mut c_int) -> *mut Buffer {
    if (*p).map.is_null() || (*p).unmapped {
        // binder_alloc_new_buf_locked: "no vma"
        user_error!("%d: binder_alloc_buf, no vma", (*p).pid);
        *err = -L_ESRCH;
        return null_mut();
    }
    size = align8(if size < 8 { 8 } else { size });
    if async_ && (*p).free_async < size.wrapping_add(64) as i64 {
        user_error!("%d: no async space left", (*p).pid);
        *err = -L_ENOSPC;
        return null_mut();
    }
    let mut at: u64 = 0;
    let head = addr_of_mut!((*p).buffers);
    let mut before = head;
    let mut e = (*head).next;
    while e != head {
        let b = container_of!(e, Buffer, entry);
        if (*b).off.wrapping_sub(at) >= size {
            before = e;
            break;
        }
        at = (*b).off.wrapping_add((*b).size);
        e = (*e).next;
    }
    if at.wrapping_add(size) > (*p).map_size {
        user_error!("%d: binder_alloc_buf size %llu failed, no address space", (*p).pid, size);
        *err = -L_ENOSPC;
        return null_mut();
    }
    let b: *mut Buffer = xnew();
    (*b).off = at;
    (*b).size = size;
    (*b).async_ = async_;
    if async_ {
        (*p).free_async = (*p).free_async.wrapping_sub(size.wrapping_add(64) as i64);
    }
    list_add_before(addr_of_mut!((*b).entry), before);
    b
}

unsafe fn free_buf_mem(p: *mut Proc, b: *mut Buffer) {
    if (*b).async_ {
        (*p).free_async = (*p).free_async.wrapping_add((*b).size.wrapping_add(64) as i64);
    }
    list_del_init(addr_of_mut!((*b).entry));
    free(b as *mut c_void);
}

unsafe fn buffer_at_user(p: *mut Proc, user: u64) -> *mut Buffer {
    if (*p).map.is_null() || user < (*p).user_base {
        return null_mut();
    }
    let off = user - (*p).user_base;
    let head = addr_of_mut!((*p).buffers);
    let mut e = (*head).next;
    while e != head {
        let b = container_of!(e, Buffer, entry);
        if (*b).off == off {
            return b;
        }
        e = (*e).next;
    }
    null_mut()
}

// The hub's view of a buffer's bytes: p->map + b->off (+ off).
#[inline(always)]
unsafe fn buf_base(p: *mut Proc, b: *mut Buffer) -> *mut u8 {
    (*p).map.wrapping_add((*b).off as usize)
}

// binder_get_object: the size of a valid object at `off` in the buffer's data.
unsafe fn get_object(p: *mut Proc, b: *mut Buffer, off: u64, out: *mut u8) -> usize {
    let left: u64 = if (*b).data_size >= off { (*b).data_size - off } else { 0 };
    if off > (*b).data_size || left < 4 || (off & 3) != 0 {
        return 0;
    }
    let base = buf_base(p, b);
    let rd: usize = if left < 40 { left as usize } else { 40 };
    ptr::copy_nonoverlapping(base.wrapping_add(off as usize), out, rd);
    let sz: usize = match ptr::read_unaligned(out as *const u32) {
        BINDER_TYPE_BINDER | BINDER_TYPE_WEAK_BINDER | BINDER_TYPE_HANDLE | BINDER_TYPE_WEAK_HANDLE => {
            size_of::<FlatBinderObject>()
        }
        BINDER_TYPE_FD => size_of::<BinderFdObject>(),
        BINDER_TYPE_PTR => size_of::<BinderBufferObject>(),
        BINDER_TYPE_FDA => size_of::<BinderFdArrayObject>(),
        _ => return 0,
    };
    if (*b).data_size >= sz as u64 && off <= (*b).data_size - sz as u64 {
        sz
    } else {
        0
    }
}

#[inline(always)]
unsafe fn buf_u64(p: *mut Proc, b: *mut Buffer, off: u64) -> u64 {
    ptr::read_unaligned(buf_base(p, b).wrapping_add(off as usize) as *const u64)
}

// binder_validate_ptr
unsafe fn validate_ptr(
    p: *mut Proc,
    b: *mut Buffer,
    index: u64,
    start: u64,
    num_valid: u64,
    out: *mut BinderBufferObject,
    obj_off: *mut u64,
) -> bool {
    if index >= num_valid {
        return false;
    }
    let o = buf_u64(p, b, start.wrapping_add(index.wrapping_mul(8)));
    let mut obj = [0u8; 40];
    if get_object(p, b, o, obj.as_mut_ptr()) != size_of::<BinderBufferObject>() {
        return false;
    }
    *out = ptr::read_unaligned(obj.as_ptr() as *const BinderBufferObject);
    if (*out).hdr_type != BINDER_TYPE_PTR {
        return false;
    }
    if !obj_off.is_null() {
        *obj_off = o;
    }
    true
}

// binder_validate_fixup
unsafe fn validate_fixup(
    p: *mut Proc,
    b: *mut Buffer,
    start: u64,
    buffer_obj_off: u64,
    fixup_off: u64,
    mut last_obj_off: u64,
    mut last_min_off: u64,
) -> bool {
    if last_obj_off == 0 {
        return false;
    }
    while last_obj_off != buffer_obj_off {
        let mut obj = [0u8; 40];
        if get_object(p, b, last_obj_off, obj.as_mut_ptr()) != size_of::<BinderBufferObject>() {
            return false;
        }
        let bbo = ptr::read_unaligned(obj.as_ptr() as *const BinderBufferObject);
        if (bbo.flags & BINDER_BUFFER_FLAG_HAS_PARENT) == 0 {
            return false;
        }
        last_min_off = bbo.parent_offset.wrapping_add(8);
        last_obj_off = buf_u64(p, b, start.wrapping_add(bbo.parent.wrapping_mul(8)));
    }
    fixup_off >= last_min_off
}

unsafe fn thread_add_close(t: *mut Thread, fd: c_int) {
    if t.is_null() {
        return;
    }
    if (*t).ncloses == (*t).ccloses {
        (*t).ccloses = if (*t).ccloses != 0 { (*t).ccloses.wrapping_mul(2) } else { 16 };
        (*t).closes = realloc(
            (*t).closes as *mut c_void,
            ((*t).ccloses as usize).wrapping_mul(size_of::<c_int>()),
        ) as *mut c_int;
        if (*t).closes.is_null() {
            out_of_memory();
        }
    }
    *(*t).closes.offset((*t).ncloses as isize) = fd;
    (*t).ncloses += 1;
}

// binder_transaction_buffer_release: undo what a buffer's objects hold, in
// the process that owns the buffer. failed_at: offsets-array position the
// translation stopped at (0: all). FDA descriptors the receiver installed are
// closed there (the result to `t` carries them).
unsafe fn buffer_release(p: *mut Proc, t: *mut Thread, b: *mut Buffer, failed_at: u64, is_failure: bool) {
    let start = align8((*b).data_size);
    let end = if is_failure && failed_at != 0 {
        failed_at
    } else {
        start.wrapping_add((*b).offsets_size)
    };
    if !(*b).target_node.is_null() {
        dec_node((*b).target_node, true, false);
        (*b).target_node = null_mut();
    }
    let mut bo = start;
    while bo < end {
        let this_bo = bo;
        bo = bo.wrapping_add(8);
        let oo = buf_u64(p, b, this_bo);
        let mut obj = [0u8; 40];
        let sz = get_object(p, b, oo, obj.as_mut_ptr());
        if sz == 0 {
            hub_log!("transaction release: bad object at offset %llu", oo);
            continue;
        }
        let type_ = ptr::read_unaligned(obj.as_ptr() as *const u32);
        match type_ {
            BINDER_TYPE_BINDER | BINDER_TYPE_WEAK_BINDER => {
                let fp = ptr::read_unaligned(obj.as_ptr() as *const FlatBinderObject);
                let n = get_node(p, fp.binder);
                if n.is_null() {
                    hub_log!("transaction release: bad node %016llx", fp.binder);
                    continue;
                }
                dec_node(n, type_ == BINDER_TYPE_BINDER, false);
            }
            BINDER_TYPE_HANDLE | BINDER_TYPE_WEAK_HANDLE => {
                let fp = ptr::read_unaligned(obj.as_ptr() as *const FlatBinderObject);
                let r = get_ref(p, fp.handle(), type_ == BINDER_TYPE_HANDLE);
                if r.is_null() {
                    hub_log!("transaction release: bad handle %u", fp.handle());
                    continue;
                }
                dec_ref(r, type_ == BINDER_TYPE_HANDLE);
            }
            BINDER_TYPE_FDA => {
                if is_failure {
                    continue;
                }
                let fda = ptr::read_unaligned(obj.as_ptr() as *const BinderFdArrayObject);
                let mut parent: BinderBufferObject = zeroed();
                let num_valid = (this_bo - start) / 8;
                if !validate_ptr(p, b, fda.parent, start, num_valid, &mut parent, null_mut()) {
                    hub_log!("transaction release: bad FDA parent");
                    continue;
                }
                // binder_transaction_buffer_release's checks, before any offset
                // is followed: the number of fds against the parent's length,
                // and the array inside this buffer (computed without wrapping).
                let ubase = (*p).user_base.wrapping_add((*b).off);
                if fda.num_fds >= (1u64 << 60)
                    || fda.num_fds.wrapping_mul(4) > parent.length
                    || fda.parent_offset > parent.length.wrapping_sub(fda.num_fds.wrapping_mul(4))
                    || parent.buffer < ubase
                    || parent.buffer.wrapping_sub(ubase) > (*b).size
                    || fda.parent_offset > (*b).size.wrapping_sub(parent.buffer.wrapping_sub(ubase))
                    || fda.num_fds.wrapping_mul(4)
                        > (*b)
                            .size
                            .wrapping_sub(parent.buffer.wrapping_sub(ubase))
                            .wrapping_sub(fda.parent_offset)
                {
                    hub_log!("transaction release: bad FDA");
                    continue;
                }
                let fda_off = parent.buffer.wrapping_sub(ubase).wrapping_add(fda.parent_offset);
                let mut i: u64 = 0;
                while i < fda.num_fds {
                    let o = fda_off.wrapping_add(i.wrapping_mul(4));
                    let fd = ptr::read_unaligned(buf_base(p, b).wrapping_add(o as usize) as *const i32);
                    thread_add_close(t, fd);
                    if !t.is_null() {
                        (*t).looper_need_return = true;
                    }
                    i += 1;
                }
            }
            _ => {} // FD: userspace closes it; PTR: goes with the buffer
        }
    }
}

unsafe fn close_fixups(t: *mut Transaction) {
    let mut i: c_int = 0;
    while i < (*t).nfix {
        let fd = *(*t).fix_fd.offset(i as isize);
        if fd >= 0 {
            close(fd);
        }
        i += 1;
    }
    free((*t).fix_fd as *mut c_void);
    free((*t).fix_off as *mut c_void);
    (*t).fix_fd = null_mut();
    (*t).fix_off = null_mut();
    (*t).nfix = 0;
}

unsafe fn free_transaction(t: *mut Transaction) {
    if !(*t).buffer.is_null() {
        (*(*t).buffer).transaction = null_mut();
    }
    close_fixups(t);
    free(t as *mut c_void);
}

// ------------------------------------------------------------------ errors

unsafe fn pop_transaction(target: *mut Thread, t: *mut Transaction) {
    if (*target).transaction_stack != t || (*t).from != target {
        hub_log!("pop_transaction: stack mismatch");
        return;
    }
    (*target).transaction_stack = (*t).from_parent;
    (*t).from = null_mut();
}

// binder_send_failed_reply
unsafe fn send_failed_reply(mut t: *mut Transaction, error_code: u32) {
    loop {
        let target = (*t).from;
        if !target.is_null() {
            pop_transaction(target, t);
            if (*target).reply_error.cmd == BR_OK {
                (*target).reply_error.cmd = error_code;
                enqueue_thread_work(target, addr_of_mut!((*target).reply_error.work));
            } else {
                hub_log!("unexpected reply error: %u", (*target).reply_error.cmd);
            }
            free_transaction(t);
            return;
        }
        let next = (*t).from_parent;
        free_transaction(t);
        if next.is_null() {
            return;
        }
        t = next;
    }
}

unsafe fn cleanup_transaction(t: *mut Transaction, error_code: u32) {
    if !(*t).buffer.is_null()
        && !(*(*t).buffer).target_node.is_null()
        && ((*t).flags & TF_ONE_WAY) == 0
    {
        send_failed_reply(t, error_code);
    } else {
        free_transaction(t);
    }
}

// binder_release_work
unsafe fn release_work(l: *mut List) {
    loop {
        let w = dequeue_head(l);
        if w.is_null() {
            break;
        }
        match (*w).type_ {
            W_TRANSACTION => cleanup_transaction(container_of!(w, Transaction, work), BR_DEAD_REPLY),
            W_TRANSACTION_COMPLETE => free(w as *mut c_void),
            W_DEAD_BINDER_AND_CLEAR | W_CLEAR_DEATH_NOTIFICATION => {
                free(container_of!(w, RefDeath, work) as *mut c_void)
            }
            _ => {} // node work, return errors: embedded, nothing to free
        }
    }
}

// ------------------------------------------------------------------ transactions

// What the sender attached for one transaction (see struct bh_txn_att).
#[repr(C)]
struct Att {
    fault: c_int,
    data: *const u8,
    offsets: *const u8,
    sg: *const u8,
    data_size: u64,
    offsets_size: u64,
    sg_size: u64,
    fds: *mut c_int,
    nfds: c_int,
    used: c_int, // descriptors taken so far
}

unsafe fn set_return_error(th: *mut Thread, cmd: u32) {
    (*th).return_error.cmd = cmd;
    enqueue_thread_work(th, addr_of_mut!((*th).return_error.work));
}

unsafe fn add_fixup(t: *mut Transaction, a: *mut Att, off: u64, allowed: bool, th: *mut Thread) -> c_int {
    if !allowed {
        user_error!(
            "%d:%d got transaction with fd, but target does not allow fds",
            (*(*th).proc_).pid,
            (*th).tid
        );
        return -L_EPERM;
    }
    if (*a).used >= (*a).nfds {
        user_error!("%d:%d got transaction with invalid fd", (*(*th).proc_).pid, (*th).tid);
        return -L_EBADF;
    }
    let slot = (*a).fds.offset((*a).used as isize);
    let fd = *slot;
    *slot = -1; // the transaction holds it now
    (*a).used += 1;
    let want = ((*t).nfix.wrapping_add(1)) as usize;
    (*t).fix_fd =
        realloc((*t).fix_fd as *mut c_void, want.wrapping_mul(size_of::<c_int>())) as *mut c_int;
    (*t).fix_off =
        realloc((*t).fix_off as *mut c_void, want.wrapping_mul(size_of::<u64>())) as *mut u64;
    if (*t).fix_fd.is_null() || (*t).fix_off.is_null() {
        out_of_memory();
    }
    *(*t).fix_fd.offset((*t).nfix as isize) = fd;
    *(*t).fix_off.offset((*t).nfix as isize) = off;
    (*t).nfix += 1;
    0
}

// Where binder_transaction's failure paths join its cleanup chain (the C
// labels, each falling through to the next): err_dead_proc, err_translate,
// err_t, err_node, err_early.
#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum TxnFail {
    Early,
    Node,
    T,
    Translate,
    DeadProc,
}

// binder_transaction
unsafe fn transaction(
    th: *mut Thread,
    tr: *const BinderTransactionData,
    reply: bool,
    mut extra_buffers_size: u64,
    a: *mut Att,
) {
    let proc_ = (*th).proc_;
    let mut target_proc: *mut Proc = null_mut();
    let mut target_thread: *mut Thread = null_mut();
    let mut target_node: *mut Node = null_mut();
    let mut in_reply_to: *mut Transaction = null_mut();
    let mut return_error: u32 = BR_OK;
    let mut param: c_int = 0;
    let mut why: *const c_char = c"".as_ptr();

    let mut t: *mut Transaction = null_mut();
    let mut tcomplete: *mut Work = null_mut();
    let mut b: *mut Buffer = null_mut();
    let mut bo: u64 = 0;
    let mut off_end: u64 = 0;

    macro_rules! fail {
        ($label:lifetime, $to:expr, $err:expr, $param:expr) => {{
            return_error = $err;
            param = $param;
            break $label $to;
        }};
        ($label:lifetime, $to:expr, $err:expr, $param:expr, $why:literal) => {{
            why = $why.as_ptr();
            return_error = $err;
            param = $param;
            break $label $to;
        }};
    }

    let failed: TxnFail = 'body: {
        if reply {
            in_reply_to = (*th).transaction_stack;
            if in_reply_to.is_null() {
                fail!('body, TxnFail::Early, BR_FAILED_REPLY, -L_EPROTO,
                      c"got reply transaction with no transaction stack");
            }
            if (*in_reply_to).to_thread != th {
                in_reply_to = null_mut();
                fail!('body, TxnFail::Early, BR_FAILED_REPLY, -L_EPROTO,
                      c"got reply transaction with bad transaction stack");
            }
            (*th).transaction_stack = (*in_reply_to).to_parent;
            target_thread = (*in_reply_to).from;
            if target_thread.is_null() {
                return_error = BR_DEAD_REPLY;
                why = c"reply target died".as_ptr();
                break 'body TxnFail::Early;
            }
            if (*target_thread).transaction_stack != in_reply_to {
                in_reply_to = null_mut();
                target_thread = null_mut();
                fail!('body, TxnFail::Early, BR_FAILED_REPLY, -L_EPROTO,
                      c"got reply transaction with bad target transaction stack");
            }
            target_proc = (*target_thread).proc_;
        } else {
            if (*tr).target_handle() != 0 {
                let r = get_ref(proc_, (*tr).target_handle(), true);
                if !r.is_null() {
                    if !(*(*r).node).proc_.is_null() {
                        target_node = (*r).node;
                        inc_node(target_node, true, false, null_mut());
                        target_proc = (*target_node).proc_;
                    } else {
                        return_error = BR_DEAD_REPLY;
                    }
                } else {
                    user_error!(
                        "%d:%d got transaction to invalid handle %u",
                        (*proc_).pid,
                        (*th).tid,
                        (*tr).target_handle()
                    );
                    return_error = BR_FAILED_REPLY;
                }
            } else {
                target_node = *ctx_mgr((*proc_).context);
                if !target_node.is_null() && !(*target_node).proc_.is_null() {
                    inc_node(target_node, true, false, null_mut());
                    target_proc = (*target_node).proc_;
                } else {
                    target_node = null_mut();
                    return_error = BR_DEAD_REPLY;
                }
                if !target_node.is_null() && target_proc == proc_ {
                    fail!('body, TxnFail::Node, BR_FAILED_REPLY, -L_EINVAL,
                          c"got transaction to context manager from process owning it");
                }
            }
            if target_node.is_null() {
                param = -L_EINVAL;
                why = c"no target".as_ptr();
                break 'body TxnFail::Early;
            }
            if proc_ == target_proc {
                fail!('body, TxnFail::Node, BR_FAILED_REPLY, -L_EINVAL,
                      c"transaction to its own process");
            }
            if ((*tr).flags & TF_ONE_WAY) == 0 && !(*th).transaction_stack.is_null() {
                let mut tmp = (*th).transaction_stack;
                if (*tmp).to_thread != th {
                    fail!('body, TxnFail::Node, BR_FAILED_REPLY, -L_EPROTO,
                          c"got new transaction with bad transaction stack");
                }
                while !tmp.is_null() {
                    if !(*tmp).from.is_null() && (*(*tmp).from).proc_ == target_proc {
                        target_thread = (*tmp).from;
                    }
                    tmp = (*tmp).from_parent;
                }
            }
        }

        if (*a).fault != 0 {
            fail!('body, TxnFail::Node, BR_FAILED_REPLY, (*a).fault.wrapping_neg(),
                  c"got transaction with invalid data ptr");
        }
        if (*a).data_size != (*tr).data_size || (*a).offsets_size != (*tr).offsets_size {
            fail!('body, TxnFail::Node, BR_FAILED_REPLY, -L_EINVAL,
                  c"attachment does not match the transaction");
        }
        if (*tr).offsets_size % 8 != 0 {
            fail!('body, TxnFail::Node, BR_FAILED_REPLY, -L_EINVAL,
                  c"got transaction with invalid offsets size");
        }
        if extra_buffers_size % 8 != 0 {
            fail!('body, TxnFail::Node, BR_FAILED_REPLY, -L_EINVAL,
                  c"got transaction with unaligned buffers size");
        }

        t = xnew();
        G_DEBUG_ID = G_DEBUG_ID.wrapping_add(1);
        (*t).debug_id = G_DEBUG_ID;
        (*t).work.type_ = W_TRANSACTION;
        list_init(addr_of_mut!((*t).work.entry));
        tcomplete = xnew();
        (*tcomplete).type_ = W_TRANSACTION_COMPLETE;
        list_init(addr_of_mut!((*tcomplete).entry));

        (*t).from = if !reply && ((*tr).flags & TF_ONE_WAY) == 0 { th } else { null_mut() };
        (*t).sender_euid = (*proc_).euid;
        (*t).to_proc = target_proc;
        (*t).to_thread = target_thread;
        (*t).code = (*tr).code;
        (*t).flags = (*tr).flags;

        let mut secctx_sz: u64 = 0;
        let mut secctx: *const c_char = null();
        if !target_node.is_null() && (*target_node).txn_security_ctx {
            secctx = addr_of_mut!((*proc_).secctx) as *const c_char;
            secctx_sz = strlen(secctx) as u64 + 1;
            extra_buffers_size = extra_buffers_size.wrapping_add(align8(secctx_sz));
        }

        let mut aerr: c_int = 0;
        let total = align8((*tr).data_size)
            .wrapping_add(align8((*tr).offsets_size))
            .wrapping_add(align8(extra_buffers_size));
        if total < (*tr).data_size || total > (*target_proc).map_size.wrapping_add(8) {
            fail!('body, TxnFail::T, BR_FAILED_REPLY, -L_ENOSPC, c"transaction too large");
        }
        b = alloc_buf(target_proc, total, !reply && ((*t).flags & TF_ONE_WAY) != 0, &mut aerr);
        if b.is_null() {
            return_error = if aerr == -L_ESRCH { BR_DEAD_REPLY } else { BR_FAILED_REPLY };
            param = aerr;
            why = c"binder_alloc_buf failed".as_ptr();
            break 'body TxnFail::T;
        }
        (*b).data_size = (*tr).data_size;
        (*b).offsets_size = (*tr).offsets_size;
        (*b).extra_size = extra_buffers_size;
        (*b).transaction = t;
        (*b).target_node = target_node;
        (*b).clear_on_free = ((*t).flags & TF_CLEAR_BUF) != 0;
        (*t).buffer = b;
        let base = buf_base(target_proc, b);
        let user = (*target_proc).user_base.wrapping_add((*b).off);
        if !secctx.is_null() {
            let so = align8((*tr).data_size)
                .wrapping_add(align8((*tr).offsets_size))
                .wrapping_add(align8(extra_buffers_size))
                .wrapping_sub(align8(secctx_sz));
            ptr::copy_nonoverlapping(secctx as *const u8, base.wrapping_add(so as usize), secctx_sz as usize);
            (*t).security_ctx = user.wrapping_add(so);
        }
        ptr::copy_nonoverlapping((*a).data, base, (*tr).data_size as usize);
        ptr::copy_nonoverlapping(
            (*a).offsets,
            base.wrapping_add(align8((*tr).data_size) as usize),
            (*tr).offsets_size as usize,
        );

        let off_start = align8((*tr).data_size);
        off_end = off_start.wrapping_add((*tr).offsets_size);
        let mut sg_off = align8(off_end);
        let sg_end = sg_off.wrapping_add(extra_buffers_size).wrapping_sub(align8(secctx_sz));
        let mut sg_src: u64 = 0; // position in the sender's sg bytes
        let mut off_min: u64 = 0;
        let mut last_fixup_obj_off: u64 = 0;
        let mut last_fixup_min_off: u64 = 0;
        let fds_ok = if reply {
            ((*in_reply_to).flags & TF_ACCEPT_FDS) != 0
        } else {
            (*target_node).accept_fds
        };
        bo = off_start;
        while bo < off_end {
            let oo = buf_u64(target_proc, b, bo);
            let mut obj = [0u8; 40];
            let osz = get_object(target_proc, b, oo, obj.as_mut_ptr());
            if osz == 0 || oo < off_min {
                user_error!(
                    "%d:%d got transaction with invalid offset (%lld, min %lld max %lld) or object",
                    (*proc_).pid,
                    (*th).tid,
                    oo as i64,
                    off_min as i64,
                    (*b).data_size as i64
                );
                fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
            }
            off_min = oo.wrapping_add(osz as u64);
            let type_ = ptr::read_unaligned(obj.as_ptr() as *const u32);
            match type_ {
                BINDER_TYPE_BINDER | BINDER_TYPE_WEAK_BINDER => {
                    // binder_translate_binder
                    let mut fp = ptr::read_unaligned(obj.as_ptr() as *const FlatBinderObject);
                    let mut n = get_node(proc_, fp.binder);
                    if n.is_null() {
                        n = new_node(proc_, &fp);
                    }
                    if fp.cookie != (*n).cookie {
                        user_error!(
                            "%d:%d sending u%016llx node %u, cookie mismatch %016llx != %016llx",
                            (*proc_).pid,
                            (*th).tid,
                            fp.binder,
                            (*n).debug_id,
                            fp.cookie,
                            (*n).cookie
                        );
                        fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                    }
                    let mut desc: u32 = 0;
                    let r = inc_ref_for_node(
                        target_proc,
                        n,
                        type_ == BINDER_TYPE_BINDER,
                        addr_of_mut!((*th).todo),
                        &mut desc,
                    );
                    if r != 0 {
                        fail!('body, TxnFail::Translate, BR_FAILED_REPLY, r);
                    }
                    fp.hdr_type = if type_ == BINDER_TYPE_BINDER {
                        BINDER_TYPE_HANDLE
                    } else {
                        BINDER_TYPE_WEAK_HANDLE
                    };
                    fp.binder = 0;
                    fp.set_handle(desc);
                    fp.cookie = 0;
                    ptr::write_unaligned(base.wrapping_add(oo as usize) as *mut FlatBinderObject, fp);
                }
                BINDER_TYPE_HANDLE | BINDER_TYPE_WEAK_HANDLE => {
                    // binder_translate_handle
                    let mut fp = ptr::read_unaligned(obj.as_ptr() as *const FlatBinderObject);
                    let r = get_ref(proc_, fp.handle(), type_ == BINDER_TYPE_HANDLE);
                    if r.is_null() {
                        user_error!(
                            "%d:%d got transaction with invalid handle, %d",
                            (*proc_).pid,
                            (*th).tid,
                            fp.handle() as c_int
                        );
                        fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                    }
                    let n = (*r).node;
                    if (*n).proc_ == target_proc {
                        fp.hdr_type = if type_ == BINDER_TYPE_HANDLE {
                            BINDER_TYPE_BINDER
                        } else {
                            BINDER_TYPE_WEAK_BINDER
                        };
                        fp.binder = (*n).ptr;
                        fp.cookie = (*n).cookie;
                        inc_node(n, fp.hdr_type == BINDER_TYPE_BINDER, false, null_mut());
                    } else {
                        let mut desc: u32 = 0;
                        let rr = inc_ref_for_node(
                            target_proc,
                            n,
                            type_ == BINDER_TYPE_HANDLE,
                            null_mut(),
                            &mut desc,
                        );
                        if rr != 0 {
                            fail!('body, TxnFail::Translate, BR_FAILED_REPLY, rr);
                        }
                        fp.binder = 0;
                        fp.set_handle(desc);
                        fp.cookie = 0;
                    }
                    ptr::write_unaligned(base.wrapping_add(oo as usize) as *mut FlatBinderObject, fp);
                }
                BINDER_TYPE_FD => {
                    let mut fo = ptr::read_unaligned(obj.as_ptr() as *const BinderFdObject);
                    let r = add_fixup(t, a, oo.wrapping_add(BINDER_FD_OBJECT_FD), fds_ok, th);
                    if r != 0 {
                        fail!('body, TxnFail::Translate, BR_FAILED_REPLY, r);
                    }
                    // pad_binder = 0, then fd = 0xffffffff: the receiver's
                    // runtime writes the new fd here
                    fo.pad_binder = 0xffff_ffff;
                    ptr::write_unaligned(base.wrapping_add(oo as usize) as *mut BinderFdObject, fo);
                }
                BINDER_TYPE_FDA => {
                    let fda = ptr::read_unaligned(obj.as_ptr() as *const BinderFdArrayObject);
                    let num_valid = (bo - off_start) / 8;
                    let mut parent: BinderBufferObject = zeroed();
                    let mut parent_off: u64 = 0;
                    if !validate_ptr(target_proc, b, fda.parent, off_start, num_valid, &mut parent, &mut parent_off) {
                        user_error!(
                            "%d:%d got transaction with invalid parent offset or type",
                            (*proc_).pid,
                            (*th).tid
                        );
                        fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                    }
                    if !validate_fixup(
                        target_proc,
                        b,
                        off_start,
                        parent_off,
                        fda.parent_offset,
                        last_fixup_obj_off,
                        last_fixup_min_off,
                    ) {
                        user_error!(
                            "%d:%d got transaction with out-of-order buffer fixup",
                            (*proc_).pid,
                            (*th).tid
                        );
                        fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                    }
                    let fd_buf = fda.num_fds.wrapping_mul(4);
                    if fda.num_fds >= (1u64 << 60)
                        || fd_buf > parent.length
                        || fda.parent_offset > parent.length.wrapping_sub(fd_buf)
                    {
                        user_error!(
                            "%d:%d not enough space to store %lld fds in buffer",
                            (*proc_).pid,
                            (*th).tid,
                            fda.num_fds as i64
                        );
                        fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                    }
                    let fda_off = parent.buffer.wrapping_sub(user).wrapping_add(fda.parent_offset);
                    if (fda_off & 3) != 0 {
                        user_error!(
                            "%d:%d parent offset not aligned correctly",
                            (*proc_).pid,
                            (*th).tid
                        );
                        fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                    }
                    let mut i: u64 = 0;
                    while i < fda.num_fds {
                        let at = fda_off.wrapping_add(i.wrapping_mul(4));
                        let r = add_fixup(t, a, at, fds_ok, th);
                        if r != 0 {
                            fail!('body, TxnFail::Translate, BR_FAILED_REPLY, r);
                        }
                        let bad: u32 = 0xffff_ffff;
                        ptr::write_unaligned(base.wrapping_add(at as usize) as *mut u32, bad);
                        i += 1;
                    }
                    last_fixup_obj_off = parent_off;
                    last_fixup_min_off = fda.parent_offset.wrapping_add(fda.num_fds.wrapping_mul(4));
                }
                BINDER_TYPE_PTR => {
                    let mut bp = ptr::read_unaligned(obj.as_ptr() as *const BinderBufferObject);
                    let left = sg_end.wrapping_sub(sg_off);
                    if bp.length > left {
                        user_error!(
                            "%d:%d got transaction with too large buffer",
                            (*proc_).pid,
                            (*th).tid
                        );
                        fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                    }
                    if sg_src.wrapping_add(align8(bp.length)) > (*a).sg_size
                        || sg_src.wrapping_add(bp.length) < sg_src
                    {
                        user_error!(
                            "%d:%d got transaction with invalid offsets ptr",
                            (*proc_).pid,
                            (*th).tid
                        );
                        fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EFAULT);
                    }
                    ptr::copy_nonoverlapping(
                        (*a).sg.wrapping_add(sg_src as usize),
                        base.wrapping_add(sg_off as usize),
                        bp.length as usize,
                    );
                    sg_src = sg_src.wrapping_add(align8(bp.length));
                    bp.buffer = user.wrapping_add(sg_off);
                    sg_off = sg_off.wrapping_add(align8(bp.length));
                    if (bp.flags & BINDER_BUFFER_FLAG_HAS_PARENT) != 0 {
                        // binder_fixup_parent
                        let num_valid = (bo - off_start) / 8;
                        let mut parent: BinderBufferObject = zeroed();
                        let mut parent_off: u64 = 0;
                        if !validate_ptr(target_proc, b, bp.parent, off_start, num_valid, &mut parent, &mut parent_off) {
                            user_error!(
                                "%d:%d got transaction with invalid parent offset or type",
                                (*proc_).pid,
                                (*th).tid
                            );
                            fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                        }
                        if !validate_fixup(
                            target_proc,
                            b,
                            off_start,
                            parent_off,
                            bp.parent_offset,
                            last_fixup_obj_off,
                            last_fixup_min_off,
                        ) {
                            user_error!(
                                "%d:%d got transaction with out-of-order buffer fixup",
                                (*proc_).pid,
                                (*th).tid
                            );
                            fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                        }
                        if parent.length < 8 || bp.parent_offset > parent.length - 8 {
                            user_error!(
                                "%d:%d got transaction with invalid parent offset",
                                (*proc_).pid,
                                (*th).tid
                            );
                            fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                        }
                        let at = bp.parent_offset.wrapping_add(parent.buffer).wrapping_sub(user);
                        if at.wrapping_add(8) > (*b).size {
                            fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                        }
                        ptr::write_unaligned(base.wrapping_add(at as usize) as *mut u64, bp.buffer);
                    }
                    ptr::write_unaligned(base.wrapping_add(oo as usize) as *mut BinderBufferObject, bp);
                    last_fixup_obj_off = oo;
                    last_fixup_min_off = 0;
                }
                _ => {
                    user_error!(
                        "%d:%d got transaction with invalid object type, %x",
                        (*proc_).pid,
                        (*th).tid,
                        type_
                    );
                    fail!('body, TxnFail::Translate, BR_FAILED_REPLY, -L_EINVAL);
                }
            }
            bo = bo.wrapping_add(8);
        }

        (*t).work.type_ = W_TRANSACTION;
        debug!(
            "%d:%d %s %u -> pid %d: code %u flags 0x%x, %llu data, %llu offsets, %llu sg, %d fds",
            (*proc_).pid,
            (*th).tid,
            if reply { c"reply".as_ptr() } else { c"transaction".as_ptr() },
            (*t).debug_id,
            (*target_proc).pid,
            (*t).code,
            (*t).flags,
            (*tr).data_size,
            (*tr).offsets_size,
            extra_buffers_size,
            (*t).nfix
        );
        if reply {
            enqueue_thread_work(th, tcomplete);
            if (*target_thread).is_dead {
                return_error = BR_DEAD_REPLY;
                break 'body TxnFail::DeadProc;
            }
            pop_transaction(target_thread, in_reply_to);
            enqueue_thread_work(target_thread, addr_of_mut!((*t).work));
            free_transaction(in_reply_to);
        } else if ((*t).flags & TF_ONE_WAY) == 0 {
            (*t).need_reply = true;
            (*t).from_parent = (*th).transaction_stack;
            (*th).transaction_stack = t;
            enqueue_deferred_thread_work(th, tcomplete);
            if !proc_transaction(t, target_proc, target_thread) {
                (*th).transaction_stack = (*t).from_parent; // binder_pop_transaction_ilocked
                return_error = BR_DEAD_REPLY;
                break 'body TxnFail::DeadProc;
            }
        } else {
            enqueue_thread_work(th, tcomplete);
            if !proc_transaction(t, target_proc, null_mut()) {
                return_error = BR_DEAD_REPLY;
                break 'body TxnFail::DeadProc;
            }
        }
        return;
    };

    if failed >= TxnFail::DeadProc {
        // The transaction was complete but its target is gone: undo it whole.
        dequeue_work(tcomplete);
        free(tcomplete as *mut c_void);
        tcomplete = null_mut();
        bo = off_end;
        param = -L_ESRCH;
        why = c"target died".as_ptr();
    }
    if failed >= TxnFail::Translate {
        buffer_release(target_proc, null_mut(), b, bo, true);
        target_node = null_mut(); // released with the buffer
        (*t).buffer = null_mut();
        free_buf_mem(target_proc, b);
    }
    if failed >= TxnFail::T {
        if !tcomplete.is_null() {
            free(tcomplete as *mut c_void);
        }
        close_fixups(t);
        free(t as *mut c_void);
    }
    if failed >= TxnFail::Node && !target_node.is_null() {
        dec_node(target_node, true, false);
    }
    // err_early:
    if *why != 0 {
        debug!(
            "%d:%d transaction failed %s/%d: %s",
            (*proc_).pid,
            (*th).tid,
            if return_error == BR_DEAD_REPLY { c"BR_DEAD_REPLY".as_ptr() } else { c"BR_FAILED_REPLY".as_ptr() },
            param,
            why
        );
    }
    if *why != 0 && return_error == BR_FAILED_REPLY {
        user_error!("%d:%d transaction failed %d: %s", (*proc_).pid, (*th).tid, param, why);
    }
    if !in_reply_to.is_null() {
        (*th).return_error.cmd = BR_TRANSACTION_COMPLETE;
        enqueue_thread_work(th, addr_of_mut!((*th).return_error.work));
        send_failed_reply(in_reply_to, return_error);
    } else {
        set_return_error(th, return_error);
    }
}

// binder_proc_transaction
unsafe fn proc_transaction(t: *mut Transaction, p: *mut Proc, mut th: *mut Thread) -> bool {
    let node = (*(*t).buffer).target_node;
    let oneway = ((*t).flags & TF_ONE_WAY) != 0;
    let mut pending_async = false;
    if oneway {
        if (*node).has_async_transaction {
            pending_async = true;
        } else {
            (*node).has_async_transaction = true;
        }
    }
    if (*p).is_dead || (!th.is_null() && (*th).is_dead) {
        if oneway && !pending_async {
            (*node).has_async_transaction = false;
        }
        return false;
    }
    if th.is_null() && !pending_async {
        th = select_thread(p);
    }
    if !th.is_null() {
        enqueue_thread_work(th, addr_of_mut!((*t).work));
    } else if !pending_async {
        enqueue_work(addr_of_mut!((*t).work), addr_of_mut!((*p).todo));
    } else {
        enqueue_work(addr_of_mut!((*t).work), addr_of_mut!((*node).async_todo));
    }
    true
}

// binder_free_buf (BC_FREE_BUFFER)
unsafe fn free_buf(p: *mut Proc, th: *mut Thread, b: *mut Buffer) {
    if !(*b).transaction.is_null() {
        (*(*b).transaction).buffer = null_mut();
        (*b).transaction = null_mut();
    }
    if (*b).async_ && !(*b).target_node.is_null() {
        let n = (*b).target_node;
        let w = dequeue_head(addr_of_mut!((*n).async_todo));
        if w.is_null() {
            (*n).has_async_transaction = false;
        } else {
            enqueue_work(w, addr_of_mut!((*p).todo)); // the next oneway call to this node
        }
    }
    buffer_release(p, th, b, 0, false);
    if (*b).clear_on_free && !(*p).map.is_null() {
        // TF_CLEAR_BUF (Linux 5.11)
        ptr::write_bytes(buf_base(p, b), 0, (*b).size as usize);
    }
    free_buf_mem(p, b);
}

// ------------------------------------------------------------------ write

#[repr(C)]
struct WrCtx {
    att: *const u8, // attachments cursor
    att_end: *const u8,
    fds: *mut c_int, // descriptors of this request, -1 once taken
    nfds: c_int,
    fd_pos: c_int,
}

unsafe fn next_att(c: *mut WrCtx, a: *mut Att) -> bool {
    ptr::write_bytes(a, 0, 1);
    if (((*c).att_end as usize).wrapping_sub((*c).att as usize) as u64) < size_of::<BhTxnAtt>() as u64 {
        return false;
    }
    let h = ptr::read_unaligned((*c).att as *const BhTxnAtt);
    (*c).att = (*c).att.wrapping_add(size_of::<BhTxnAtt>());
    let need = align8(h.data_size)
        .wrapping_add(align8(h.offsets_size))
        .wrapping_add(h.sg_size);
    if need < h.data_size || (((*c).att_end as usize).wrapping_sub((*c).att as usize) as u64) < need {
        return false;
    }
    (*a).fault = h.fault;
    (*a).data = (*c).att;
    (*a).data_size = h.data_size;
    (*a).offsets = (*c).att.wrapping_add(align8(h.data_size) as usize);
    (*a).offsets_size = h.offsets_size;
    (*a).sg = (*a).offsets.wrapping_add(align8(h.offsets_size) as usize);
    (*a).sg_size = h.sg_size;
    (*c).att = (*c).att.wrapping_add(need as usize);
    if h.nfds > (*c).nfds.wrapping_sub((*c).fd_pos) as u32 {
        return false;
    }
    (*a).fds = (*c).fds.wrapping_offset((*c).fd_pos as isize);
    (*a).nfds = h.nfds as c_int;
    (*c).fd_pos = (*c).fd_pos.wrapping_add(h.nfds as c_int);
    true
}

// binder_thread_write. Returns 0 or -errno; *consumed advances per command.
unsafe fn thread_write(
    th: *mut Thread,
    buf: *const u8,
    size: u64,
    consumed: *mut u64,
    c: *mut WrCtx,
) -> c_int {
    let proc_ = (*th).proc_;
    let mut pos: u64 = 0;
    while pos < size && (*th).return_error.cmd == BR_OK {
        if size - pos < 4 {
            return -L_EFAULT;
        }
        let cmd = ptr::read_unaligned(buf.wrapping_add(pos as usize) as *const u32);
        let argsz = lx_ioc_size(cmd) as u64;
        if size - pos - 4 < argsz {
            return -L_EFAULT;
        }
        let arg = buf.wrapping_add(pos as usize).wrapping_add(4);
        'sw: {
            match cmd {
                BC_INCREFS | BC_ACQUIRE | BC_RELEASE | BC_DECREFS => {
                    let target = ptr::read_unaligned(arg as *const u32);
                    let strong = cmd == BC_ACQUIRE || cmd == BC_RELEASE;
                    let increment = cmd == BC_INCREFS || cmd == BC_ACQUIRE;
                    let mut ret: c_int = -1;
                    let mut desc: u32 = target;
                    if increment && target == 0 {
                        let cm = *ctx_mgr((*proc_).context);
                        if !cm.is_null() {
                            if (*cm).proc_ == proc_ {
                                user_error!(
                                    "%d:%d context manager tried to acquire desc 0",
                                    (*proc_).pid,
                                    (*th).tid
                                );
                                return -L_EINVAL;
                            }
                            ret = inc_ref_for_node(proc_, cm, strong, null_mut(), &mut desc);
                        }
                    }
                    if ret != 0 {
                        ret = update_ref_for_handle(proc_, target, increment, strong);
                    }
                    if ret == 0 && desc != target {
                        user_error!(
                            "%d:%d tried to acquire reference to desc %u, got %u instead",
                            (*proc_).pid,
                            (*th).tid,
                            target,
                            desc
                        );
                    }
                    if ret != 0 {
                        user_error!(
                            "%d:%d refcount change on invalid ref %u ret %d",
                            (*proc_).pid,
                            (*th).tid,
                            target,
                            ret
                        );
                    }
                }
                BC_INCREFS_DONE | BC_ACQUIRE_DONE => {
                    let pc = ptr::read_unaligned(arg as *const BinderPtrCookie);
                    let n = get_node(proc_, pc.ptr);
                    let name = if cmd == BC_INCREFS_DONE {
                        c"BC_INCREFS_DONE".as_ptr()
                    } else {
                        c"BC_ACQUIRE_DONE".as_ptr()
                    };
                    if n.is_null() {
                        user_error!("%d:%d %s u%016llx no match", (*proc_).pid, (*th).tid, name, pc.ptr);
                        break 'sw;
                    }
                    if pc.cookie != (*n).cookie {
                        user_error!(
                            "%d:%d %s u%016llx node %u cookie mismatch",
                            (*proc_).pid,
                            (*th).tid,
                            name,
                            pc.ptr,
                            (*n).debug_id
                        );
                        break 'sw;
                    }
                    if cmd == BC_ACQUIRE_DONE {
                        if !(*n).pending_strong_ref {
                            user_error!(
                                "%d:%d BC_ACQUIRE_DONE node %u has no pending acquire request",
                                (*proc_).pid,
                                (*th).tid,
                                (*n).debug_id
                            );
                            break 'sw;
                        }
                        (*n).pending_strong_ref = false;
                    } else {
                        if !(*n).pending_weak_ref {
                            user_error!(
                                "%d:%d BC_INCREFS_DONE node %u has no pending increfs request",
                                (*proc_).pid,
                                (*th).tid,
                                (*n).debug_id
                            );
                            break 'sw;
                        }
                        (*n).pending_weak_ref = false;
                    }
                    if dec_node_nilocked(n, cmd == BC_ACQUIRE_DONE, false) {
                        free_node(n); // the kernel WARNs here; it cannot happen
                    }
                }
                BC_ATTEMPT_ACQUIRE | BC_ACQUIRE_RESULT => {
                    hub_log!(
                        "%d:%d BC_ATTEMPT_ACQUIRE/BC_ACQUIRE_RESULT not supported",
                        (*proc_).pid,
                        (*th).tid
                    );
                    return -L_EINVAL;
                }
                BC_FREE_BUFFER => {
                    let ptr_ = ptr::read_unaligned(arg as *const u64);
                    let b = buffer_at_user(proc_, ptr_);
                    if b.is_null() {
                        user_error!(
                            "%d:%d BC_FREE_BUFFER u%016llx no match",
                            (*proc_).pid,
                            (*th).tid,
                            ptr_
                        );
                        break 'sw;
                    }
                    if !(*b).allow_user_free {
                        user_error!(
                            "%d:%d BC_FREE_BUFFER u%016llx matched unreturned or currently freeing buffer",
                            (*proc_).pid,
                            (*th).tid,
                            ptr_
                        );
                        break 'sw;
                    }
                    free_buf(proc_, th, b);
                }
                BC_TRANSACTION | BC_REPLY | BC_TRANSACTION_SG | BC_REPLY_SG => {
                    let tr = ptr::read_unaligned(arg as *const BinderTransactionData);
                    let mut extra: u64 = 0;
                    if cmd == BC_TRANSACTION_SG || cmd == BC_REPLY_SG {
                        extra = ptr::read_unaligned(
                            arg.wrapping_add(size_of::<BinderTransactionData>()) as *const u64,
                        );
                    }
                    let mut a: Att = zeroed();
                    if !next_att(c, &mut a) {
                        hub_log!("%d:%d transaction without its attachment", (*proc_).pid, (*th).tid);
                        return -L_EINVAL;
                    }
                    transaction(th, &tr, cmd == BC_REPLY || cmd == BC_REPLY_SG, extra, &mut a);
                    let mut i: c_int = 0;
                    while i < a.nfds {
                        // descriptors nothing took
                        let slot = a.fds.offset(i as isize);
                        if *slot >= 0 {
                            close(*slot);
                            *slot = -1;
                        }
                        i += 1;
                    }
                }
                BC_REGISTER_LOOPER => {
                    if ((*th).looper & LOOPER_ENTERED) != 0 {
                        user_error!(
                            "%d:%d ERROR: BC_REGISTER_LOOPER called after BC_ENTER_LOOPER",
                            (*proc_).pid,
                            (*th).tid
                        );
                        (*th).looper |= LOOPER_INVALID;
                    } else if (*proc_).requested_threads == 0 {
                        user_error!(
                            "%d:%d ERROR: BC_REGISTER_LOOPER called without request",
                            (*proc_).pid,
                            (*th).tid
                        );
                        (*th).looper |= LOOPER_INVALID;
                    } else {
                        (*proc_).requested_threads -= 1;
                        (*proc_).requested_threads_started =
                            (*proc_).requested_threads_started.wrapping_add(1);
                    }
                    (*th).looper |= LOOPER_REGISTERED;
                }
                BC_ENTER_LOOPER => {
                    if ((*th).looper & LOOPER_REGISTERED) != 0 {
                        user_error!(
                            "%d:%d ERROR: BC_ENTER_LOOPER called after BC_REGISTER_LOOPER",
                            (*proc_).pid,
                            (*th).tid
                        );
                        (*th).looper |= LOOPER_INVALID;
                    }
                    (*th).looper |= LOOPER_ENTERED;
                }
                BC_EXIT_LOOPER => {
                    (*th).looper |= LOOPER_EXITED;
                }
                BC_REQUEST_DEATH_NOTIFICATION | BC_CLEAR_DEATH_NOTIFICATION => {
                    // struct binder_handle_cookie (packed)
                    let hc_handle = ptr::read_unaligned(arg as *const u32);
                    let hc_cookie = ptr::read_unaligned(arg.wrapping_add(4) as *const u64);
                    let r = get_ref(proc_, hc_handle, false);
                    if r.is_null() {
                        user_error!(
                            "%d:%d %s invalid ref %u",
                            (*proc_).pid,
                            (*th).tid,
                            if cmd == BC_REQUEST_DEATH_NOTIFICATION {
                                c"BC_REQUEST_DEATH_NOTIFICATION".as_ptr()
                            } else {
                                c"BC_CLEAR_DEATH_NOTIFICATION".as_ptr()
                            },
                            hc_handle
                        );
                        break 'sw;
                    }
                    if cmd == BC_REQUEST_DEATH_NOTIFICATION {
                        if !(*r).death.is_null() {
                            user_error!(
                                "%d:%d BC_REQUEST_DEATH_NOTIFICATION death notification already set",
                                (*proc_).pid,
                                (*th).tid
                            );
                            break 'sw;
                        }
                        let d: *mut RefDeath = xnew();
                        list_init(addr_of_mut!((*d).work.entry));
                        (*d).cookie = hc_cookie;
                        (*r).death = d;
                        if (*(*r).node).proc_.is_null() {
                            (*d).work.type_ = W_DEAD_BINDER;
                            enqueue_work(addr_of_mut!((*d).work), addr_of_mut!((*proc_).todo));
                        }
                    } else {
                        if (*r).death.is_null() {
                            user_error!(
                                "%d:%d BC_CLEAR_DEATH_NOTIFICATION death notification not active",
                                (*proc_).pid,
                                (*th).tid
                            );
                            break 'sw;
                        }
                        let d = (*r).death;
                        if (*d).cookie != hc_cookie {
                            user_error!(
                                "%d:%d BC_CLEAR_DEATH_NOTIFICATION death notification cookie mismatch %016llx != %016llx",
                                (*proc_).pid,
                                (*th).tid,
                                (*d).cookie,
                                hc_cookie
                            );
                            break 'sw;
                        }
                        (*r).death = null_mut();
                        if !work_queued(addr_of_mut!((*d).work)) {
                            (*d).work.type_ = W_CLEAR_DEATH_NOTIFICATION;
                            if ((*th).looper & (LOOPER_REGISTERED | LOOPER_ENTERED)) != 0 {
                                enqueue_thread_work(th, addr_of_mut!((*d).work));
                            } else {
                                enqueue_work(addr_of_mut!((*d).work), addr_of_mut!((*proc_).todo));
                            }
                        } else {
                            (*d).work.type_ = W_DEAD_BINDER_AND_CLEAR;
                        }
                    }
                }
                BC_DEAD_BINDER_DONE => {
                    let cookie = ptr::read_unaligned(arg as *const u64);
                    let mut d: *mut RefDeath = null_mut();
                    let head = addr_of_mut!((*proc_).delivered_death);
                    let mut e = (*head).next;
                    while e != head {
                        let x = container_of!(container_of!(e, Work, entry), RefDeath, work);
                        if (*x).cookie == cookie {
                            d = x;
                            break;
                        }
                        e = (*e).next;
                    }
                    if d.is_null() {
                        user_error!(
                            "%d:%d BC_DEAD_BINDER_DONE %016llx not found",
                            (*proc_).pid,
                            (*th).tid,
                            cookie
                        );
                        break 'sw;
                    }
                    dequeue_work(addr_of_mut!((*d).work));
                    if (*d).work.type_ == W_DEAD_BINDER_AND_CLEAR {
                        (*d).work.type_ = W_CLEAR_DEATH_NOTIFICATION;
                        if ((*th).looper & (LOOPER_REGISTERED | LOOPER_ENTERED)) != 0 {
                            enqueue_thread_work(th, addr_of_mut!((*d).work));
                        } else {
                            enqueue_work(addr_of_mut!((*d).work), addr_of_mut!((*proc_).todo));
                        }
                    }
                    // W_DEAD_BINDER: the ref was cleared or never will be; the
                    // notification is done and stays owned by its ref.
                }
                _ => {
                    hub_log!("%d:%d unknown command %u (0x%x)", (*proc_).pid, (*th).tid, cmd, cmd);
                    return -L_EINVAL;
                }
            }
        }
        pos += 4 + argsz;
        *consumed = pos;
    }
    0
}

// ------------------------------------------------------------------ read

#[repr(C)]
struct Rd {
    out: *mut u8,
    avail: u64,
    pos: u64,
    consumed0: u64, // read_consumed when the call started
    spawn: bool,
    fds: *mut c_int,
    fix: *mut u64,
    nfix: c_int,
    ret: i64,
}

#[inline(always)]
unsafe fn put32(r: *mut Rd, v: u32) {
    ptr::write_unaligned((*r).out.wrapping_add((*r).pos as usize) as *mut u32, v);
    (*r).pos = (*r).pos.wrapping_add(4);
}
#[inline(always)]
unsafe fn put64(r: *mut Rd, v: u64) {
    ptr::write_unaligned((*r).out.wrapping_add((*r).pos as usize) as *mut u64, v);
    (*r).pos = (*r).pos.wrapping_add(8);
}

const RD_DONE: c_int = 0;
const RD_BLOCK: c_int = 1;

// binder_thread_read. RD_BLOCK: nothing to return yet (park the thread).
unsafe fn thread_read(th: *mut Thread, r: *mut Rd, nonblock: bool) -> c_int {
    let proc_ = (*th).proc_;
    'retry: loop {
        (*r).pos = 0;
        let wfpw = available_for_proc_work(th);
        if wfpw && ((*th).looper & (LOOPER_REGISTERED | LOOPER_ENTERED)) == 0 {
            user_error!(
                "%d:%d ERROR: Thread waiting for process work before calling BC_REGISTER_LOOPER or BC_ENTER_LOOPER (state %x)",
                (*proc_).pid,
                (*th).tid,
                (*th).looper
            );
        }
        if !has_work(th, wfpw) {
            if nonblock {
                (*r).ret = -(L_EAGAIN as i64);
                (*r).pos = 0;
                return RD_DONE;
            }
            return RD_BLOCK;
        }
        if (*r).consumed0 == 0 {
            if (*r).avail < 4 {
                (*r).ret = 0;
                return RD_DONE;
            }
            put32(r, BR_NOOP);
        }
        // `break 'work` is the C `goto done` (and the loop's own break).
        'work: loop {
            let list: *mut List;
            if !list_empty(addr_of_mut!((*th).todo)) {
                list = addr_of_mut!((*th).todo);
            } else if !list_empty(addr_of_mut!((*proc_).todo)) && wfpw {
                list = addr_of_mut!((*proc_).todo);
            } else {
                if (*r).consumed0.wrapping_add((*r).pos) == 4 && !(*th).looper_need_return {
                    continue 'retry;
                }
                break 'work;
            }
            if (*r).avail.wrapping_sub((*r).pos) < (size_of::<BinderTransactionDataSecctx>() + 4) as u64 {
                break 'work;
            }
            let w = dequeue_head(list);
            if list_empty(addr_of_mut!((*th).todo)) {
                (*th).process_todo = false;
            }
            let mut t: *mut Transaction = null_mut();
            match (*w).type_ {
                W_TRANSACTION => {
                    t = container_of!(w, Transaction, work);
                }
                W_RETURN_ERROR => {
                    let e = container_of!(w, ErrorWork, work);
                    put32(r, (*e).cmd);
                    (*e).cmd = BR_OK;
                }
                W_TRANSACTION_COMPLETE => {
                    free(w as *mut c_void);
                    put32(r, BR_TRANSACTION_COMPLETE);
                }
                W_NODE => {
                    let n = container_of!(w, Node, work);
                    let strong = (*n).internal_strong_refs != 0 || (*n).local_strong_refs != 0;
                    let weak = !list_empty(addr_of_mut!((*n).refs)) || (*n).local_weak_refs != 0 || strong;
                    let has_weak = (*n).has_weak_ref;
                    let has_strong = (*n).has_strong_ref;
                    let ptr_ = (*n).ptr;
                    let cookie = (*n).cookie;
                    if weak && !has_weak {
                        (*n).has_weak_ref = true;
                        (*n).pending_weak_ref = true;
                        (*n).local_weak_refs = (*n).local_weak_refs.wrapping_add(1);
                    }
                    if strong && !has_strong {
                        (*n).has_strong_ref = true;
                        (*n).pending_strong_ref = true;
                        (*n).local_strong_refs = (*n).local_strong_refs.wrapping_add(1);
                    }
                    if !strong && has_strong {
                        (*n).has_strong_ref = false;
                    }
                    if !weak && has_weak {
                        (*n).has_weak_ref = false;
                    }
                    if !weak && !strong {
                        debug!(
                            "%d:%d node %u u%016llx deleted",
                            (*proc_).pid,
                            (*th).tid,
                            (*n).debug_id,
                            ptr_
                        );
                        free_node(n);
                    }
                    if weak && !has_weak {
                        put32(r, BR_INCREFS);
                        put64(r, ptr_);
                        put64(r, cookie);
                    }
                    if strong && !has_strong {
                        put32(r, BR_ACQUIRE);
                        put64(r, ptr_);
                        put64(r, cookie);
                    }
                    if !strong && has_strong {
                        put32(r, BR_RELEASE);
                        put64(r, ptr_);
                        put64(r, cookie);
                    }
                    if !weak && has_weak {
                        put32(r, BR_DECREFS);
                        put64(r, ptr_);
                        put64(r, cookie);
                    }
                }
                W_DEAD_BINDER | W_DEAD_BINDER_AND_CLEAR | W_CLEAR_DEATH_NOTIFICATION => {
                    let d = container_of!(w, RefDeath, work);
                    let clear = (*w).type_ == W_CLEAR_DEATH_NOTIFICATION;
                    let cmd = if clear { BR_CLEAR_DEATH_NOTIFICATION_DONE } else { BR_DEAD_BINDER };
                    let cookie = (*d).cookie;
                    if clear {
                        free(d as *mut c_void);
                    } else {
                        enqueue_work(w, addr_of_mut!((*proc_).delivered_death));
                    }
                    put32(r, cmd);
                    put64(r, cookie);
                    if cmd == BR_DEAD_BINDER {
                        break 'work; // DEAD_BINDER notifications can cause transactions
                    }
                }
                _ => {
                    hub_log!("%d:%d bad work type %d", (*proc_).pid, (*th).tid, (*w).type_);
                }
            }
            if t.is_null() {
                continue 'work;
            }

            let mut tr: BinderTransactionDataSecctx = zeroed();
            let mut cmd: u32;
            if (*t).buffer.is_null() {
                // Its buffer went with a dying thread; nothing left to deliver.
                free_transaction(t);
                continue 'work;
            }
            if !(*(*t).buffer).target_node.is_null() {
                tr.transaction_data.target = (*(*(*t).buffer).target_node).ptr;
                tr.transaction_data.cookie = (*(*(*t).buffer).target_node).cookie;
                cmd = BR_TRANSACTION;
            } else {
                cmd = BR_REPLY;
            }
            tr.transaction_data.code = (*t).code;
            tr.transaction_data.flags = (*t).flags;
            tr.transaction_data.sender_euid = (*t).sender_euid;
            tr.transaction_data.sender_pid =
                if !(*t).from.is_null() { (*(*(*t).from).proc_).pid } else { 0 };
            // binder_apply_fd_fixups: the descriptors go with this result and the
            // receiver's runtime installs them (it knows its own fd numbers).
            let mut i: c_int = 0;
            while i < (*t).nfix {
                *(*r).fds.offset((*r).nfix as isize) = *(*t).fix_fd.offset(i as isize);
                *(*r).fix.offset((*r).nfix as isize) =
                    (*(*t).buffer).off.wrapping_add(*(*t).fix_off.offset(i as isize));
                (*r).nfix += 1;
                *(*t).fix_fd.offset(i as isize) = -1;
                i += 1;
            }
            close_fixups(t);
            tr.transaction_data.data_size = (*(*t).buffer).data_size;
            tr.transaction_data.offsets_size = (*(*t).buffer).offsets_size;
            tr.transaction_data.data_buffer = (*proc_).user_base.wrapping_add((*(*t).buffer).off);
            tr.transaction_data.data_offsets =
                tr.transaction_data.data_buffer.wrapping_add(align8((*(*t).buffer).data_size));
            let mut trsize = size_of::<BinderTransactionData>();
            tr.secctx = (*t).security_ctx;
            if (*t).security_ctx != 0 {
                cmd = BR_TRANSACTION_SEC_CTX;
                trsize = size_of::<BinderTransactionDataSecctx>();
            }
            put32(r, cmd);
            ptr::copy_nonoverlapping(
                &tr as *const BinderTransactionDataSecctx as *const u8,
                (*r).out.wrapping_add((*r).pos as usize),
                trsize,
            );
            (*r).pos = (*r).pos.wrapping_add(trsize as u64);
            (*(*t).buffer).allow_user_free = true;
            if cmd != BR_REPLY && ((*t).flags & TF_ONE_WAY) == 0 {
                (*t).to_parent = (*th).transaction_stack;
                (*t).to_thread = th;
                (*th).transaction_stack = t;
            } else {
                free_transaction(t);
            }
            break 'work;
        }
        break 'retry;
    }
    // done:
    if (*proc_).requested_threads == 0
        && list_empty(addr_of_mut!((*proc_).waiting_threads))
        && (*proc_).requested_threads_started < (*proc_).max_threads
        && ((*th).looper & (LOOPER_REGISTERED | LOOPER_ENTERED)) != 0
    {
        (*proc_).requested_threads += 1;
        (*r).spawn = true;
        debug!("%d:%d BR_SPAWN_LOOPER", (*proc_).pid, (*th).tid);
    }
    (*r).ret = 0;
    RD_DONE
}

// ------------------------------------------------------------------ release

// binder_thread_release
unsafe fn thread_release(th: *mut Thread) {
    let p = (*th).proc_;
    list_del_init(addr_of_mut!((*th).proc_entry));
    list_del_init(addr_of_mut!((*th).waiting_entry));
    (*th).parked = false;
    let mut t = (*th).transaction_stack;
    let mut send_reply: *mut Transaction = null_mut();
    if !t.is_null() && (*t).to_thread == th {
        send_reply = t;
    }
    (*th).is_dead = true;
    while !t.is_null() {
        if (*t).to_thread == th {
            (*t).to_proc = null_mut();
            (*t).to_thread = null_mut();
            if !(*t).buffer.is_null() {
                (*(*t).buffer).transaction = null_mut();
                (*t).buffer = null_mut();
            }
            t = (*t).to_parent;
        } else if (*t).from == th {
            (*t).from = null_mut();
            t = (*t).from_parent;
        } else {
            hub_log!("%d:%d thread_release: bad transaction stack", (*p).pid, (*th).tid);
            break;
        }
    }
    if !send_reply.is_null() {
        send_failed_reply(send_reply, BR_DEAD_REPLY);
    }
    release_work(addr_of_mut!((*th).todo));
    // FDA descriptors still owed to a gone thread stay open in its process
    // (they were installed there); nothing here can close them.
    free((*th).closes as *mut c_void);
    let mut i: c_int = 0;
    while i < (*th).nheld {
        close(*(*th).held.offset(i as isize));
        i += 1;
    }
    free((*th).held as *mut c_void);
    if (*th).fd >= 0 {
        close((*th).fd);
    }
    debug!("%d:%d thread released", (*p).pid, (*th).tid);
    free(th as *mut c_void);
}

unsafe fn node_release(n: *mut Node) {
    let p = (*n).proc_;
    release_work(addr_of_mut!((*n).async_todo));
    dequeue_work(addr_of_mut!((*n).work));
    list_del_init(addr_of_mut!((*n).proc_entry));
    if list_empty(addr_of_mut!((*n).refs)) {
        free(n as *mut c_void);
        return;
    }
    (*n).proc_ = null_mut();
    (*n).local_strong_refs = 0;
    (*n).local_weak_refs = 0;
    list_add_tail(addr_of_mut!((*n).proc_entry), addr_of_mut!(G_DEAD_NODES));
    let head = addr_of_mut!((*n).refs);
    let mut e = (*head).next;
    while e != head {
        let r = container_of!(e, Ref, node_entry);
        e = (*e).next;
        if (*r).death.is_null() {
            continue;
        }
        if work_queued(addr_of_mut!((*(*r).death).work)) {
            hub_log!("node_release: death work already queued");
            continue;
        }
        (*(*r).death).work.type_ = W_DEAD_BINDER;
        enqueue_work(addr_of_mut!((*(*r).death).work), addr_of_mut!((*(*r).proc_).todo));
    }
    debug!("%d node %u now dead", if !p.is_null() { (*p).pid } else { 0 }, (*n).debug_id);
}

// Why the last read_msg failed, for the log line of whoever drops the peer.
static mut G_READ_FAIL: *const c_char = c"".as_ptr();
static mut G_READ_ERRNO: c_int = 0;

// binder_deferred_release
unsafe fn proc_release(p: *mut Proc) {
    if (*p).hello {
        hub_log!(
            "proc %u (pid %d) released%s%s",
            (*p).id,
            (*p).pid,
            if *G_READ_FAIL != 0 { c": ".as_ptr() } else { c"".as_ptr() },
            G_READ_FAIL
        );
    }
    list_del_init(addr_of_mut!((*p).entry));
    let cm = ctx_mgr((*p).context);
    if !(*cm).is_null() && (*(*cm)).proc_ == p {
        *cm = null_mut();
    }
    (*p).is_dead = true;
    while !list_empty(addr_of_mut!((*p).threads)) {
        thread_release(container_of!((*p).threads.next, Thread, proc_entry));
    }
    while !list_empty(addr_of_mut!((*p).nodes)) {
        node_release(container_of!((*p).nodes.next, Node, proc_entry));
    }
    while !list_empty(addr_of_mut!((*p).refs)) {
        cleanup_ref(container_of!((*p).refs.next, Ref, proc_entry));
    }
    release_work(addr_of_mut!((*p).todo));
    release_work(addr_of_mut!((*p).delivered_death));
    while !list_empty(addr_of_mut!((*p).buffers)) {
        let b = container_of!((*p).buffers.next, Buffer, entry);
        if !(*b).transaction.is_null() {
            (*(*b).transaction).buffer = null_mut();
        }
        free_buf_mem(p, b);
    }
    if !(*p).map.is_null() {
        munmap((*p).map as *mut c_void, (*p).map_host_size as usize);
    }
    if (*p).fd >= 0 {
        close((*p).fd);
    }
    debug!("proc %d (pid %d) released", (*p).id, (*p).pid);
    free(p as *mut c_void);
}

// ------------------------------------------------------------------ channels

// union { struct cmsghdr h; char b[CMSG_SPACE(sizeof(int) * BH_MAX_FDS)]; }
const CM_BYTES: usize = cmsg_space(size_of::<c_int>() * BH_MAX_FDS);
#[repr(C, align(4))]
struct CmBuf {
    b: [u8; CM_BYTES],
}

unsafe fn send_all(fd: c_int, iov0: *const Iovec, niov: c_int, fds: *const c_int, nfds: c_int) -> c_int {
    let mut iov: [Iovec; 8] = [Iovec { iov_base: null_mut(), iov_len: 0 }; 8];
    if niov > 8 {
        return -1;
    }
    ptr::copy_nonoverlapping(iov0, iov.as_mut_ptr(), niov as usize);
    let iov = iov.as_mut_ptr();
    let mut cm: CmBuf = CmBuf { b: [0; CM_BYTES] };
    let mut m: MsgHdr = zeroed();
    m.msg_iov = iov;
    m.msg_iovlen = niov;
    if nfds > 0 {
        m.msg_control = cm.b.as_mut_ptr() as *mut c_void;
        m.msg_controllen = cmsg_space(size_of::<c_int>() * nfds as usize) as u32;
        let c = cmsg_firsthdr(&m);
        (*c).cmsg_level = SOL_SOCKET;
        (*c).cmsg_type = SCM_RIGHTS;
        (*c).cmsg_len = cmsg_len(size_of::<c_int>() * nfds as usize) as u32;
        ptr::copy_nonoverlapping(fds as *const u8, cmsg_data(c), size_of::<c_int>() * nfds as usize);
    }
    let mut idx: c_int = 0;
    while idx < niov {
        m.msg_iov = iov.offset(idx as isize);
        m.msg_iovlen = niov - idx;
        let mut n = sendmsg(fd, &m, 0);
        if n < 0 {
            if errno() == EINTR {
                continue;
            }
            return -1;
        }
        m.msg_control = null_mut();
        m.msg_controllen = 0;
        while n > 0 && idx < niov {
            let v = iov.offset(idx as isize);
            if n as usize >= (*v).iov_len {
                n -= (*v).iov_len as isize;
                idx += 1;
            } else {
                (*v).iov_base = ((*v).iov_base as *mut u8).wrapping_add(n as usize) as *mut c_void;
                (*v).iov_len -= n as usize;
                n = 0;
            }
        }
        while idx < niov && (*iov.offset(idx as isize)).iov_len == 0 {
            idx += 1;
        }
    }
    0
}

unsafe fn read_full(fd: c_int, buf: *mut c_void, len: usize) -> c_int {
    let mut got: usize = 0;
    while got < len {
        let n = recv(fd, (buf as *mut u8).wrapping_add(got) as *mut c_void, len - got, 0);
        if n < 0 && errno() == EINTR {
            continue;
        }
        if n <= 0 {
            G_READ_FAIL = if n == 0 {
                c"end of file in the payload".as_ptr()
            } else {
                c"payload read failed".as_ptr()
            };
            G_READ_ERRNO = if n == 0 { 0 } else { errno() };
            return -1;
        }
        got += n as usize;
    }
    0
}

// One whole message. -1: the peer is gone (or broke the protocol).
unsafe fn read_msg(
    fd: c_int,
    h: *mut BhHdr,
    payload: *mut *mut u8,
    fds: *mut c_int,
    nfds: *mut c_int,
) -> c_int {
    *nfds = 0;
    *payload = null_mut();
    let mut cm: CmBuf = CmBuf { b: [0; CM_BYTES] };
    let mut iov = Iovec { iov_base: h as *mut c_void, iov_len: size_of::<BhHdr>() };
    let mut m: MsgHdr = zeroed();
    m.msg_iov = &mut iov;
    m.msg_iovlen = 1;
    m.msg_control = cm.b.as_mut_ptr() as *mut c_void;
    m.msg_controllen = CM_BYTES as u32;
    let ok = 'fail: {
        let mut got: usize = 0;
        while got < size_of::<BhHdr>() {
            iov.iov_base = (h as *mut u8).wrapping_add(got) as *mut c_void;
            iov.iov_len = size_of::<BhHdr>() - got;
            m.msg_control = cm.b.as_mut_ptr() as *mut c_void;
            m.msg_controllen = CM_BYTES as u32;
            let n = recvmsg(fd, &mut m, 0);
            if n < 0 && errno() == EINTR {
                continue;
            }
            if n <= 0 {
                G_READ_FAIL = if n == 0 {
                    if got != 0 {
                        c"end of file in the header".as_ptr()
                    } else {
                        c"end of file".as_ptr()
                    }
                } else {
                    c"header read failed".as_ptr()
                };
                G_READ_ERRNO = if n == 0 { 0 } else { errno() };
                break 'fail false;
            }
            let mut c = cmsg_firsthdr(&m);
            while !c.is_null() {
                if (*c).cmsg_level == SOL_SOCKET && (*c).cmsg_type == SCM_RIGHTS {
                    let k = (((*c).cmsg_len as usize).wrapping_sub(cmsg_len(0)) / size_of::<c_int>()) as c_int;
                    let mut i: c_int = 0;
                    while i < k {
                        let f = ptr::read_unaligned(
                            cmsg_data(c).wrapping_add(i as usize * size_of::<c_int>()) as *const c_int,
                        );
                        if (*nfds as usize) < BH_MAX_FDS {
                            fcntl(f, F_SETFD, FD_CLOEXEC);
                            *fds.offset(*nfds as isize) = f;
                            *nfds += 1;
                        } else {
                            close(f);
                        }
                        i += 1;
                    }
                }
                c = cmsg_nxthdr(&m, c);
            }
            got += n as usize;
        }
        if (*h).len > BH_MAX_MSG {
            G_READ_FAIL = c"message too large".as_ptr();
            G_READ_ERRNO = 0;
            break 'fail false;
        }
        if (*h).len != 0 {
            *payload = malloc((*h).len as usize) as *mut u8;
            if (*payload).is_null() {
                G_READ_FAIL = c"out of memory".as_ptr();
                G_READ_ERRNO = ENOMEM;
                break 'fail false;
            }
            if read_full(fd, *payload as *mut c_void, (*h).len as usize) != 0 {
                break 'fail false;
            }
        }
        if (*h).nfds as c_int != *nfds {
            hub_log!("message carried %d descriptors, header says %u", *nfds, (*h).nfds);
            break 'fail false;
        }
        true
    };
    if ok {
        return 0;
    }
    free(*payload as *mut c_void);
    *payload = null_mut();
    let mut i: c_int = 0;
    while i < *nfds {
        close(*fds.offset(i as isize));
        i += 1;
    }
    *nfds = 0;
    -1
}

unsafe fn proc_poll_wanted(p: *mut Proc) -> bool {
    let head = addr_of_mut!((*p).threads);
    let mut e = (*head).next;
    while e != head {
        let t = container_of!(e, Thread, proc_entry);
        e = (*e).next;
        if ((*t).looper & LOOPER_POLL) == 0 || (*t).parked {
            continue;
        }
        if has_work(t, available_for_proc_work(t)) {
            return true;
        }
    }
    false
}

unsafe fn proc_ring(p: *mut Proc) {
    if !(*p).hello || (*p).fd < 0 {
        return;
    }
    if proc_poll_wanted(p) && (*p).bells == (*p).bells_drained {
        let c: c_char = b'D' as c_char;
        if send((*p).fd, &c as *const c_char as *const c_void, 1, MSG_DONTWAIT) == 1 {
            (*p).bells = (*p).bells.wrapping_add(1);
        }
    }
}

unsafe fn send_result(
    th: *mut Thread,
    seq: u64,
    res: *mut BhResult,
    rd: *const u8,
    fds: *const c_int,
    fix: *const u64,
) {
    let p = (*th).proc_;
    (*res).ncloses = (*th).ncloses as u32;
    // The doorbell's state as of this result: rung before it, if wanted.
    proc_ring(p);
    (*res).bells = (*p).bells;
    (*res).bell_pending = proc_poll_wanted(p) as u32;
    if (*res).bell_pending == 0 {
        (*p).bells_drained = (*p).bells;
    }
    let mut h = BhHdr { type_: BH_RESULT, nfds: (*res).nfixups, len: 0, seq };
    h.len = (size_of::<BhResult>() as u64)
        .wrapping_add((*res).read_len)
        .wrapping_add(8u64 * (*res).nfixups as u64)
        .wrapping_add(4u64 * (*res).ncloses as u64);
    let iov: [Iovec; 5] = [
        Iovec { iov_base: &mut h as *mut BhHdr as *mut c_void, iov_len: size_of::<BhHdr>() },
        Iovec { iov_base: res as *mut c_void, iov_len: size_of::<BhResult>() },
        Iovec { iov_base: rd as *mut c_void, iov_len: (*res).read_len as usize },
        Iovec { iov_base: fix as *mut c_void, iov_len: (8u64 * (*res).nfixups as u64) as usize },
        Iovec {
            iov_base: (*th).closes as *mut c_void,
            iov_len: (4u64).wrapping_mul((*th).ncloses as usize as u64) as usize,
        },
    ];
    if send_all((*th).fd, iov.as_ptr(), 5, fds, (*res).nfixups as c_int) != 0 {
        let why = strerror(errno());
        hub_log!("%d:%d result not delivered: %s", (*p).pid, (*th).tid, why);
    }
    // Not closed yet: a descriptor whose only reference is the message in
    // flight can be flushed by XNU's unix socket garbage collection before
    // the thread reads it (see runtime/binder.c tchan_get). The thread's
    // next message proves it has them (held_release).
    let mut i: u32 = 0;
    while i < (*res).nfixups {
        if (*th).nheld == (*th).cheld {
            (*th).cheld = if (*th).cheld != 0 { (*th).cheld.wrapping_mul(2) } else { 8 };
            (*th).held = realloc(
                (*th).held as *mut c_void,
                ((*th).cheld as usize).wrapping_mul(size_of::<c_int>()),
            ) as *mut c_int;
            if (*th).held.is_null() {
                out_of_memory();
            }
        }
        *(*th).held.offset((*th).nheld as isize) = *fds.add(i as usize);
        (*th).nheld += 1;
        i += 1;
    }
    (*th).ncloses = 0;
}

// Run the parked read of `th`; send its result when it has one.
unsafe fn try_complete_read(th: *mut Thread) {
    if !(*th).parked {
        return;
    }
    list_del_init(addr_of_mut!((*th).waiting_entry));
    let avail = (*th).park_read_avail;
    let mut r: Rd = zeroed();
    r.avail = avail;
    r.consumed0 = (*th).park_read_consumed;
    r.out = malloc(avail.wrapping_add(256) as usize) as *mut u8; // slack: node commands are not bounds-checked
    let mut fds = [0 as c_int; BH_MAX_FDS];
    let mut fix = [0u64; BH_MAX_FDS];
    r.fds = fds.as_mut_ptr();
    r.fix = fix.as_mut_ptr();
    if r.out.is_null() {
        out_of_memory();
    }
    let st = thread_read(th, &mut r, (*th).park_nonblock);
    if st == RD_BLOCK {
        if available_for_proc_work(th) {
            list_add_tail(
                addr_of_mut!((*th).waiting_entry),
                addr_of_mut!((*(*th).proc_).waiting_threads),
            );
        }
        free(r.out as *mut c_void);
        return;
    }
    (*th).parked = false;
    (*th).looper &= !LOOPER_WAITING;
    (*th).looper_need_return = false; // end of binder_ioctl
    let mut res: BhResult = zeroed();
    res.ret = r.ret;
    res.write_consumed = (*th).park_write_consumed;
    res.read_len = if r.ret == 0 { if r.pos > avail { avail } else { r.pos } } else { 0 };
    res.spawn_looper = r.spawn as u32;
    res.nfixups = r.nfix as u32;
    send_result(th, (*th).park_seq, &mut res, r.out, fds.as_ptr(), fix.as_ptr());
    free(r.out as *mut c_void);
}

unsafe fn simple_result(th: *mut Thread, seq: u64, ret: i64, arg: *const c_void, argn: usize) {
    let mut res: BhResult = zeroed();
    res.ret = ret;
    if !arg.is_null() && argn <= size_of::<[u8; 64]>() {
        ptr::copy_nonoverlapping(arg as *const u8, res.arg.as_mut_ptr(), argn);
    }
    (*th).looper_need_return = false;
    send_result(th, seq, &mut res, null(), null(), null());
}

unsafe fn handle_write_read(th: *mut Thread, h: *const BhHdr, pl: *const u8, fds: *mut c_int, nfds: c_int) {
    if (*h).len < size_of::<BhWr>() as u64 {
        simple_result(th, (*h).seq, -(L_EINVAL as i64), null(), 0);
        return;
    }
    let wr = ptr::read_unaligned(pl as *const BhWr);
    let wbuf = pl.wrapping_add(size_of::<BhWr>());
    let rest = (*h).len - size_of::<BhWr>() as u64;
    if wr.write_len > rest {
        simple_result(th, (*h).seq, -(L_EINVAL as i64), null(), 0);
        return;
    }
    let mut c = WrCtx {
        att: wbuf.wrapping_add(align8(wr.write_len) as usize),
        att_end: pl.wrapping_add((*h).len as usize),
        fds,
        nfds,
        fd_pos: 0,
    };
    if align8(wr.write_len) > rest {
        c.att = c.att_end;
    }
    let mut consumed: u64 = 0;
    let mut ret: c_int = 0;
    if wr.write_len > 0 {
        ret = thread_write(th, wbuf, wr.write_len, &mut consumed, &mut c);
    }
    let mut i: c_int = 0;
    while i < nfds {
        // descriptors of transactions not reached
        let slot = fds.offset(i as isize);
        if *slot >= 0 {
            close(*slot);
            *slot = -1;
        }
        i += 1;
    }
    if ret < 0 {
        let mut res: BhResult = zeroed();
        res.ret = ret as i64;
        res.write_consumed = consumed;
        res.arg[0] = 1; // the write failed: read_consumed = 0
        (*th).looper_need_return = false;
        send_result(th, (*h).seq, &mut res, null(), null(), null());
        return;
    }
    if wr.read_avail == 0 {
        let mut res: BhResult = zeroed();
        res.ret = 0;
        res.write_consumed = consumed;
        (*th).looper_need_return = false;
        send_result(th, (*h).seq, &mut res, null(), null(), null());
        return;
    }
    (*th).parked = true;
    (*th).looper |= LOOPER_WAITING;
    (*th).park_seq = (*h).seq;
    (*th).park_write_consumed = consumed;
    (*th).park_read_avail = if wr.read_avail > (1u64 << 20) { 1u64 << 20 } else { wr.read_avail };
    (*th).park_read_consumed = wr.read_consumed;
    (*th).park_nonblock = (wr.flags & BH_WR_NONBLOCK) != 0;
    try_complete_read(th);
}

unsafe fn handle_ioctl(th: *mut Thread, h: *const BhHdr, pl: *const u8) {
    let p = (*th).proc_;
    let seq = (*h).seq;
    if (*h).len < size_of::<BhIoctl>() as u64 {
        simple_result(th, seq, -(L_EINVAL as i64), null(), 0);
        return;
    }
    let io = ptr::read_unaligned(pl as *const BhIoctl);
    match io.cmd {
        BINDER_VERSION => {
            // struct binder_version
            let v: i32 = BINDER_CURRENT_PROTOCOL_VERSION;
            simple_result(th, seq, 0, &v as *const i32 as *const c_void, size_of::<i32>());
        }
        BINDER_SET_MAX_THREADS => {
            let n = ptr::read_unaligned(io.arg.as_ptr() as *const u32);
            (*p).max_threads = n as c_int;
            simple_result(th, seq, 0, null(), 0);
        }
        BINDER_SET_CONTEXT_MGR_EXT | BINDER_SET_CONTEXT_MGR => {
            let mut fbo: FlatBinderObject = zeroed();
            if io.cmd == BINDER_SET_CONTEXT_MGR_EXT {
                fbo = ptr::read_unaligned(io.arg.as_ptr() as *const FlatBinderObject);
            }
            if !(*ctx_mgr((*p).context)).is_null() {
                hub_log!("BINDER_SET_CONTEXT_MGR already set");
                simple_result(th, seq, -(L_EBUSY as i64), null(), 0);
                return;
            }
            if *ctx_mgr_uid_set((*p).context) {
                if *ctx_mgr_uid((*p).context) != (*p).euid {
                    hub_log!(
                        "BINDER_SET_CONTEXT_MGR bad uid %u != %u",
                        (*p).euid,
                        *ctx_mgr_uid((*p).context)
                    );
                    simple_result(th, seq, -(L_EPERM as i64), null(), 0);
                    return;
                }
            } else {
                *ctx_mgr_uid((*p).context) = (*p).euid;
                *ctx_mgr_uid_set((*p).context) = true;
            }
            let n = new_node(p, &fbo);
            (*n).local_weak_refs += 1;
            (*n).local_strong_refs += 1;
            (*n).has_strong_ref = true;
            (*n).has_weak_ref = true;
            *ctx_mgr((*p).context) = n;
            debug!("%d context manager of context %d", (*p).pid, (*p).context);
            simple_result(th, seq, 0, null(), 0);
        }
        BINDER_THREAD_EXIT => {
            // The runtime closes the channel after this result; the channel's
            // end is where the thread is released.
            simple_result(th, seq, 0, null(), 0);
        }
        BH_IOCTL_UNMAPPED => {
            (*p).unmapped = true;
            simple_result(th, seq, 0, null(), 0);
        }
        BINDER_GET_NODE_DEBUG_INFO => {
            let mut info = ptr::read_unaligned(io.arg.as_ptr() as *const BinderNodeDebugInfo);
            let want = info.ptr;
            info = zeroed();
            let mut best: *mut Node = null_mut();
            let head = addr_of_mut!((*p).nodes);
            let mut e = (*head).next;
            while e != head {
                let n = container_of!(e, Node, proc_entry);
                if (*n).ptr > want && (best.is_null() || (*n).ptr < (*best).ptr) {
                    best = n;
                }
                e = (*e).next;
            }
            if !best.is_null() {
                info.ptr = (*best).ptr;
                info.cookie = (*best).cookie;
                info.has_strong_ref = (*best).has_strong_ref as u32;
                info.has_weak_ref = (*best).has_weak_ref as u32;
            }
            simple_result(
                th,
                seq,
                0,
                &info as *const BinderNodeDebugInfo as *const c_void,
                size_of::<BinderNodeDebugInfo>(),
            );
        }
        BINDER_GET_NODE_INFO_FOR_REF => {
            let mut info = ptr::read_unaligned(io.arg.as_ptr() as *const BinderNodeInfoForRef);
            if info.strong_count != 0
                || info.weak_count != 0
                || info.reserved1 != 0
                || info.reserved2 != 0
                || info.reserved3 != 0
            {
                simple_result(th, seq, -(L_EINVAL as i64), null(), 0);
                return;
            }
            let cm = *ctx_mgr((*p).context);
            if cm.is_null() || (*cm).proc_ != p {
                simple_result(th, seq, -(L_EPERM as i64), null(), 0);
                return;
            }
            let r = get_ref(p, info.handle, true);
            if r.is_null() {
                simple_result(th, seq, -(L_EINVAL as i64), null(), 0);
                return;
            }
            info.strong_count =
                (*(*r).node).local_strong_refs.wrapping_add((*(*r).node).internal_strong_refs) as u32;
            info.weak_count = (*(*r).node).local_weak_refs as u32;
            simple_result(
                th,
                seq,
                0,
                &info as *const BinderNodeInfoForRef as *const c_void,
                size_of::<BinderNodeInfoForRef>(),
            );
        }
        _ => {
            simple_result(th, seq, -(L_EINVAL as i64), null(), 0);
        }
    }
}

unsafe fn handle_mmap(th: *mut Thread, h: *const BhHdr, pl: *const u8, fds: *mut c_int, nfds: c_int) {
    let p = (*th).proc_;
    if (*h).len < size_of::<BhMmap>() as u64 || nfds != 1 {
        let mut i: c_int = 0;
        while i < nfds {
            close(*fds.offset(i as isize));
            i += 1;
        }
        simple_result(th, (*h).seq, -(L_EINVAL as i64), null(), 0);
        return;
    }
    let mm = ptr::read_unaligned(pl as *const BhMmap);
    let fd = *fds;
    *fds = -1;
    if !(*p).map.is_null() {
        close(fd);
        hub_log!("%d binder_mmap: already mapped", (*p).pid);
        simple_result(th, (*h).seq, -(L_EBUSY as i64), null(), 0);
        return;
    }
    let size: u64 = if mm.size > SZ_4M { SZ_4M } else { mm.size };
    let pg = sysconf(SC_PAGESIZE);
    let hsize = size.wrapping_add(pg as u64).wrapping_sub(1) & !(pg as u64).wrapping_sub(1);
    let m = mmap(null_mut(), hsize as usize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if m == MAP_FAILED {
        let why = strerror(errno());
        hub_log!("%d binder_mmap: %s", (*p).pid, why);
        simple_result(th, (*h).seq, -(L_ENOMEM as i64), null(), 0);
        return;
    }
    (*p).map = m as *mut u8;
    (*p).map_size = size;
    (*p).map_host_size = hsize;
    (*p).user_base = mm.base;
    (*p).free_async = (size / 2) as i64;
    debug!("%d mapped %llu bytes at guest 0x%llx", (*p).pid, size, mm.base);
    simple_result(th, (*h).seq, 0, null(), 0);
}

// ------------------------------------------------------------------ loop

unsafe fn new_thread(p: *mut Proc, fd: c_int, tid: c_int) -> *mut Thread {
    let t: *mut Thread = xnew();
    (*t).proc_ = p;
    (*t).fd = fd;
    (*t).tid = tid;
    (*t).looper_need_return = true;
    list_init(addr_of_mut!((*t).todo));
    list_init(addr_of_mut!((*t).waiting_entry));
    (*t).return_error.work.type_ = W_RETURN_ERROR;
    (*t).return_error.cmd = BR_OK;
    list_init(addr_of_mut!((*t).return_error.work.entry));
    (*t).reply_error.work.type_ = W_RETURN_ERROR;
    (*t).reply_error.cmd = BR_OK;
    list_init(addr_of_mut!((*t).reply_error.work.entry));
    list_add_tail(addr_of_mut!((*t).proc_entry), addr_of_mut!((*p).threads));
    let tv = Timeval { tv_sec: 10, tv_usec: 0 };
    let tvp = &tv as *const Timeval as *const c_void;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, tvp, size_of::<Timeval>() as u32);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, tvp, size_of::<Timeval>() as u32);
    let sz: c_int = 1 << 20;
    let szp = &sz as *const c_int as *const c_void;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, szp, size_of::<c_int>() as u32);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, szp, size_of::<c_int>() as u32);
    debug!("%d:%d new thread", (*p).pid, tid);
    t
}

unsafe fn close_all(fds: *const c_int, nfds: c_int) {
    let mut i: c_int = 0;
    while i < nfds {
        close(*fds.offset(i as isize));
        i += 1;
    }
}

unsafe fn proc_message(p: *mut Proc) {
    let mut h: BhHdr = zeroed();
    let mut pl: *mut u8 = null_mut();
    let mut fds = [0 as c_int; BH_MAX_FDS];
    let mut nfds: c_int = 0;
    if read_msg((*p).fd, &mut h, &mut pl, fds.as_mut_ptr(), &mut nfds) != 0 {
        proc_release(p);
        return;
    }
    if !(*p).hello {
        let mut ack = BhHelloAck { ret: 0, proc_id: 0 };
        if h.type_ != BH_HELLO || h.len < size_of::<BhHello>() as u64 {
            free(pl as *mut c_void);
            close_all(fds.as_ptr(), nfds);
            proc_release(p);
            return;
        }
        let hello = ptr::read_unaligned(pl as *const BhHello);
        free(pl as *mut c_void);
        close_all(fds.as_ptr(), nfds);
        if hello.version != BH_VERSION || hello.context > 2 {
            ack.ret = -L_EINVAL;
        } else {
            (*p).hello = true;
            (*p).pid = hello.pid;
            let mut hp: c_int = 0;
            let mut hplen: u32 = size_of::<c_int>() as u32;
            (*p).host_pid = if getsockopt(
                (*p).fd,
                SOL_LOCAL,
                LOCAL_PEERPID,
                &mut hp as *mut c_int as *mut c_void,
                &mut hplen,
            ) == 0
            {
                hp
            } else {
                hello.pid
            };
            (*p).euid = hello.euid;
            (*p).context = hello.context as c_int;
            (*p).secctx = hello.secctx;
            (*p).secctx[63] = 0;
            ack.proc_id = (*p).id;
            debug!(
                "proc %u: pid %d euid %u context %d",
                (*p).id,
                (*p).pid,
                (*p).euid,
                (*p).context
            );
        }
        let mut ah = BhHdr { type_: BH_HELLO_ACK, nfds: 0, len: size_of::<BhHelloAck>() as u64, seq: h.seq };
        let iov: [Iovec; 2] = [
            Iovec { iov_base: &mut ah as *mut BhHdr as *mut c_void, iov_len: size_of::<BhHdr>() },
            Iovec { iov_base: &mut ack as *mut BhHelloAck as *mut c_void, iov_len: size_of::<BhHelloAck>() },
        ];
        if send_all((*p).fd, iov.as_ptr(), 2, null(), 0) != 0 || ack.ret != 0 {
            proc_release(p);
        }
        return;
    }
    if h.type_ == BH_THREAD && nfds == 1 && h.len >= size_of::<BhThread>() as u64 {
        let bt = ptr::read_unaligned(pl as *const BhThread);
        let mut c: c_char = 0;
        let pk = recv(fds[0], &mut c as *mut c_char as *mut c_void, 1, MSG_PEEK | MSG_DONTWAIT);
        let pe = errno();
        let mut fst: Stat = zeroed();
        let sock = fstat(fds[0], &mut fst) == 0 && (fst.st_mode & S_IFMT) == S_IFSOCK;
        let mut peer: c_int = 0;
        let mut pl2: u32 = size_of::<c_int>() as u32;
        let gp = getsockopt(fds[0], SOL_LOCAL, LOCAL_PEERPID, &mut peer as *mut c_int as *mut c_void, &mut pl2);
        if pk == 0 || !sock || (gp == 0 && peer != (*p).host_pid) {
            // The runtime waits for BH_THREAD_ACK and makes a new channel
            // when it does not come (runtime/binder.c tchan_get).
            hub_log!(
                "%d:%d thread channel arrived %s (socket %d, peer pid %d%s); refused",
                (*p).pid,
                bt.tid,
                if pk == 0 { c"closed at the other end".as_ptr() } else { c"odd".as_ptr() },
                sock as c_int,
                if gp == 0 { peer } else { -1 },
                if pk < 0 && pe != EAGAIN { c", peek failed".as_ptr() } else { c"".as_ptr() }
            );
            close(fds[0]);
        } else {
            let nt = new_thread(p, fds[0], bt.tid);
            let mut ack = BhHdr { type_: BH_THREAD_ACK, nfds: 0, len: 0, seq: 0 };
            let iov = Iovec { iov_base: &mut ack as *mut BhHdr as *mut c_void, iov_len: size_of::<BhHdr>() };
            if send_all((*nt).fd, &iov, 1, null(), 0) != 0 {
                let why = strerror(errno());
                hub_log!(
                    "%d:%d thread channel: acknowledgement not sent: %s; dropped",
                    (*p).pid,
                    bt.tid,
                    why
                );
                thread_release(nt);
            }
        }
    } else {
        hub_log!("proc %d: unexpected message %u", (*p).pid, h.type_);
        close_all(fds.as_ptr(), nfds);
    }
    free(pl as *mut c_void);
}

unsafe fn held_release(th: *mut Thread) {
    let mut i: c_int = 0;
    while i < (*th).nheld {
        close(*(*th).held.offset(i as isize));
        i += 1;
    }
    (*th).nheld = 0;
}

unsafe fn thread_message(th: *mut Thread) {
    let mut h: BhHdr = zeroed();
    let mut pl: *mut u8 = null_mut();
    let mut fds = [0 as c_int; BH_MAX_FDS];
    let mut nfds: c_int = 0;
    if read_msg((*th).fd, &mut h, &mut pl, fds.as_mut_ptr(), &mut nfds) != 0 {
        // A thread's channel ends when its thread or process does (end of
        // file, logged only when it was in the middle of something); any
        // other failure drops a live thread, whose next call then fails with
        // EBADF ("Bad file descriptor" in Java): always logged.
        if strcmp(G_READ_FAIL, c"end of file".as_ptr()) != 0
            || (*th).parked
            || !(*th).transaction_stack.is_null()
        {
            let es: *const c_char =
                if G_READ_ERRNO != 0 { strerror(G_READ_ERRNO) } else { c"".as_ptr() };
            hub_log!(
                "%d:%d thread dropped: %s%s%s%s",
                (*(*th).proc_).pid,
                (*th).tid,
                G_READ_FAIL,
                if G_READ_ERRNO != 0 { c" (".as_ptr() } else { c"".as_ptr() },
                es,
                if G_READ_ERRNO != 0 { c")".as_ptr() } else { c"".as_ptr() }
            );
        }
        thread_release(th);
        return;
    }
    // A request (not a cancel, which can cross the result) is sent after
    // the last result was read: the descriptors that came with it are the
    // thread's now.
    if h.type_ != BH_CANCEL {
        held_release(th);
    }
    if (*th).parked && h.type_ != BH_CANCEL {
        hub_log!("%d:%d request while blocked; dropping the thread", (*(*th).proc_).pid, (*th).tid);
        close_all(fds.as_ptr(), nfds);
        free(pl as *mut c_void);
        thread_release(th);
        return;
    }
    match h.type_ {
        BH_WRITE_READ => handle_write_read(th, &h, pl, fds.as_mut_ptr(), nfds),
        BH_CANCEL => {
            if (*th).parked && (*th).park_seq == h.seq {
                list_del_init(addr_of_mut!((*th).waiting_entry));
                (*th).parked = false;
                (*th).looper &= !LOOPER_WAITING;
                let mut res: BhResult = zeroed();
                res.ret = -(L_EINTR as i64);
                res.write_consumed = (*th).park_write_consumed;
                send_result(th, h.seq, &mut res, null(), null(), null());
            }
        }
        BH_IOCTL => handle_ioctl(th, &h, pl),
        BH_MMAP => {
            handle_mmap(th, &h, pl, fds.as_mut_ptr(), nfds);
            nfds = 0;
        }
        BH_POLL => {
            (*th).looper |= LOOPER_POLL;
            let mut res: BhResult = zeroed();
            res.ret = 0;
            send_result(th, h.seq, &mut res, null(), null(), null());
        }
        _ => {
            hub_log!("%d:%d unknown message %u", (*(*th).proc_).pid, (*th).tid, h.type_);
        }
    }
    if h.type_ != BH_WRITE_READ && h.type_ != BH_MMAP {
        close_all(fds.as_ptr(), nfds);
    }
    free(pl as *mut c_void);
}

// After every event: parked threads with work get their result, and every
// process's doorbell follows its state.
unsafe fn settle() {
    let phead = addr_of_mut!(G_PROCS);
    let mut pe = (*phead).next;
    let mut pt = (*pe).next;
    while pe != phead {
        let p = container_of!(pe, Proc, entry);
        let thead = addr_of_mut!((*p).threads);
        let mut te = (*thead).next;
        let mut tt = (*te).next;
        while te != thead {
            let th = container_of!(te, Thread, proc_entry);
            if (*th).parked && has_work(th, available_for_proc_work(th)) {
                try_complete_read(th);
            }
            te = tt;
            tt = (*te).next;
        }
        // Proc work nobody is waiting for: give it to a parked looper that
        // can now take it (a thread whose stack emptied), as binder_wakeup_proc.
        proc_ring(p);
        pe = pt;
        pt = (*pe).next;
    }
}

// volatile sig_atomic_t: written by the handlers, read by the loop.
static G_STOP: AtomicI32 = AtomicI32::new(0);
extern "C" fn on_term(_s: c_int) {
    G_STOP.store(1, Ordering::Relaxed);
}

// SIGUSR1: the driver's state, as /sys/kernel/debug/binder/state shows it on
// Linux, written to <dir>/hub-state.txt. A process stuck in a transaction
// (SurfaceFlinger's main thread waiting for a reply, every Android window
// gone with it) shows who it waits for and what that thread is doing.
static G_DUMP: AtomicI32 = AtomicI32::new(0);
extern "C" fn on_dump(_s: c_int) {
    G_DUMP.store(1, Ordering::Relaxed);
}

unsafe fn list_len(l: *mut List) -> c_int {
    let mut n: c_int = 0;
    let mut e = (*l).next;
    while e != l {
        n = n.wrapping_add(1);
        e = (*e).next;
    }
    n
}

unsafe fn dump_state() {
    let mut path = [0 as c_char; 1024];
    snprintf(path.as_mut_ptr(), 1024, c"%s/hub-state.txt".as_ptr(), G_DIR);
    let f = fopen(path.as_ptr(), c"w".as_ptr());
    if f.is_null() {
        return;
    }
    let phead = addr_of_mut!(G_PROCS);
    let mut pe = (*phead).next;
    while pe != phead {
        let p = container_of!(pe, Proc, entry);
        fprintf(
            f,
            c"proc %d host %d ctx %d euid %u%s todo %d waiting %d threads %d/%d started %d\n".as_ptr(),
            (*p).pid,
            (*p).host_pid,
            (*p).context,
            (*p).euid,
            if (*p).is_dead { c" DEAD".as_ptr() } else { c"".as_ptr() },
            list_len(addr_of_mut!((*p).todo)),
            list_len(addr_of_mut!((*p).waiting_threads)),
            list_len(addr_of_mut!((*p).threads)),
            (*p).max_threads,
            (*p).requested_threads_started,
        );
        let thead = addr_of_mut!((*p).threads);
        let mut te = (*thead).next;
        while te != thead {
            let t = container_of!(te, Thread, proc_entry);
            fprintf(
                f,
                c"  thread %d looper 0x%x%s%s todo %d\n".as_ptr(),
                (*t).tid,
                (*t).looper,
                if (*t).parked { c" parked".as_ptr() } else { c"".as_ptr() },
                if (*t).is_dead { c" DEAD".as_ptr() } else { c"".as_ptr() },
                list_len(addr_of_mut!((*t).todo)),
            );
            let mut x = (*t).transaction_stack;
            while !x.is_null() {
                fprintf(
                    f,
                    c"    txn %u code 0x%x flags 0x%x%s from %d:%d to %d:%d\n".as_ptr(),
                    (*x).debug_id,
                    (*x).code,
                    (*x).flags,
                    if (*x).need_reply { c" need_reply".as_ptr() } else { c"".as_ptr() },
                    if !(*x).from.is_null() { (*(*(*x).from).proc_).pid } else { -1 },
                    if !(*x).from.is_null() { (*(*x).from).tid } else { -1 },
                    if !(*x).to_proc.is_null() { (*(*x).to_proc).pid } else { -1 },
                    if !(*x).to_thread.is_null() { (*(*x).to_thread).tid } else { -1 },
                );
                x = if (*x).from == t { (*x).from_parent } else { (*x).to_parent };
            }
            te = (*te).next;
        }
        pe = (*pe).next;
    }
    fclose(f);
}

// The poll loop's rotation (a function-local static in C).
static mut ROT: usize = 0;

#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_hub_main(argc: c_int, argv: *mut *mut c_char) -> c_int {
    // argv: <lxrun> --binder-hub <dir> [--daemon]
    if argc < 3 {
        fprintf(__stderrp, c"usage: lxrun --binder-hub <dir> [--daemon]\n".as_ptr());
        return 2;
    }
    G_DIR = *argv.add(2);
    let daemonize = argc > 3 && strcmp(*argv.add(3), c"--daemon".as_ptr()) == 0;
    let log_env = getenv(c"LXRT_BINDER_LOG".as_ptr());
    G_VERBOSE = !log_env.is_null() && *log_env == b'1' as c_char;
    let idle_env = getenv(c"LXRT_BINDER_HUB_IDLE".as_ptr());
    let mut idle_s: c_int = if !idle_env.is_null() { atoi(idle_env) } else { 10 };
    if idle_s < 1 {
        idle_s = 1;
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, on_term as extern "C" fn(c_int) as usize);
    signal(SIGINT, on_term as extern "C" fn(c_int) as usize);
    signal(SIGUSR1, on_dump as extern "C" fn(c_int) as usize);
    let mut rl = Rlimit { rlim_cur: 0, rlim_max: 0 };
    if getrlimit(RLIMIT_NOFILE, &mut rl) == 0 {
        let want: u64 = if rl.rlim_max < 10240 { rl.rlim_max } else { 10240 };
        if rl.rlim_cur < want {
            rl.rlim_cur = want;
            setrlimit(RLIMIT_NOFILE, &rl);
        }
    }
    list_init(addr_of_mut!(G_PROCS));
    list_init(addr_of_mut!(G_DEAD_NODES));

    let mut sa: SockaddrUn = zeroed();
    sa.sun_family = AF_UNIX as u8;
    let sun_path = addr_of_mut!(sa.sun_path) as *mut c_char;
    if snprintf(sun_path, 104, c"%s/hub.sock".as_ptr(), G_DIR) >= 104 {
        fprintf(__stderrp, c"binder hub: %s: path too long\n".as_ptr(), G_DIR);
        return 1;
    }
    // One hub per directory: hub.pid is held locked for the hub's whole life
    // (the daemon inherits this descriptor) and let go only after the socket
    // is gone, as the property service does with service.lock. A client
    // whose connect failed while a hub still held it (a listen backlog full
    // while the hub was busy, a hub leaving) used to start a second one,
    // which unlinked the live hub's socket and bound its own: two drivers,
    // and processes on the new one found no context manager. A hub that is
    // leaving holds the lock a moment longer: wait a little.
    let mut lockp = [0 as c_char; 1024];
    snprintf(lockp.as_mut_ptr(), 1024, c"%s/hub.pid".as_ptr(), G_DIR);
    let life = open(lockp.as_ptr(), O_RDWR | O_CREAT | O_CLOEXEC, 0o600 as c_int);
    let mut locked = false;
    let mut i: c_int = 0;
    while life >= 0 && i < 100 && !locked {
        // up to 10 s
        if flock(life, LOCK_EX | LOCK_NB) == 0 {
            locked = true;
        } else {
            usleep(100_000);
        }
        i += 1;
    }
    if !locked {
        if life < 0 {
            let why = strerror(errno());
            fprintf(__stderrp, c"binder hub: %s: %s\n".as_ptr(), lockp.as_ptr(), why);
        } else {
            fprintf(__stderrp, c"binder hub: another hub holds %s\n".as_ptr(), lockp.as_ptr());
        }
        return 1;
    }
    let ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if ls < 0 {
        perror(c"binder hub: socket".as_ptr());
        return 1;
    }
    fcntl(ls, F_SETFD, FD_CLOEXEC);
    unlink(sun_path);
    if bind(ls, &sa as *const SockaddrUn as *const c_void, size_of::<SockaddrUn>() as u32) != 0
        || listen(ls, 64) != 0
    {
        perror(c"binder hub: bind".as_ptr());
        return 1;
    }
    chmod(sun_path, 0o600);
    let mut sock_st: Stat = zeroed();
    stat(sun_path, &mut sock_st);

    if daemonize {
        // The spawner waits for this first process: once it exits, the socket
        // is listening, and the hub itself is no child of any guest (a guest's
        // wait(-1) must never see it).
        let pid = fork();
        if pid < 0 {
            return 1;
        }
        if pid > 0 {
            _exit(0);
        }
        setsid();
        let mut logp = [0 as c_char; 1024];
        snprintf(logp.as_mut_ptr(), 1024, c"%s/hub.log".as_ptr(), G_DIR);
        let mut lst: Stat = zeroed();
        let mut lfl = O_WRONLY | O_CREAT | O_APPEND;
        if stat(logp.as_ptr(), &mut lst) == 0 && lst.st_size > (1 << 20) {
            lfl |= O_TRUNC;
        }
        let lfd = open(logp.as_ptr(), lfl, 0o600 as c_int);
        if lfd >= 0 {
            dup2(lfd, 2);
            if lfd != 2 {
                close(lfd);
            }
        }
        setvbuf(__stderrp, null_mut(), IOLBF, 0);
    }
    if ftruncate(life, 0) == 0 {
        dprintf(life, c"%d\n".as_ptr(), getpid());
    }
    hub_log!("started, pid %d, %s", getpid(), sun_path);

    let mut idle_since = time(null_mut());
    let mut pfd: *mut PollFd = null_mut();
    let mut who: *mut *mut c_void = null_mut();
    let mut cap: usize = 0;
    let phead = addr_of_mut!(G_PROCS);
    while G_STOP.load(Ordering::Relaxed) == 0 {
        let mut n: usize = 1;
        let mut pe = (*phead).next;
        while pe != phead {
            let p = container_of!(pe, Proc, entry);
            n += 1;
            let thead = addr_of_mut!((*p).threads);
            let mut te = (*thead).next;
            while te != thead {
                n += 1;
                te = (*te).next;
            }
            pe = (*pe).next;
        }
        if n > cap {
            cap = n * 2;
            pfd = realloc(pfd as *mut c_void, cap.wrapping_mul(size_of::<PollFd>())) as *mut PollFd;
            who = realloc(who as *mut c_void, cap.wrapping_mul(size_of::<*mut c_void>())) as *mut *mut c_void;
            if pfd.is_null() || who.is_null() {
                hub_log!("out of memory");
                return 70;
            }
        }
        let mut k: usize = 0;
        *pfd.add(k) = PollFd { fd: ls, events: POLLIN, revents: 0 };
        *who.add(k) = null_mut();
        k += 1;
        pe = (*phead).next;
        while pe != phead {
            let p = container_of!(pe, Proc, entry);
            *pfd.add(k) = PollFd { fd: (*p).fd, events: POLLIN, revents: 0 };
            *who.add(k) = p as *mut c_void;
            k += 1;
            let thead = addr_of_mut!((*p).threads);
            let mut te = (*thead).next;
            while te != thead {
                let t = container_of!(te, Thread, proc_entry);
                *pfd.add(k) = PollFd { fd: (*t).fd, events: POLLIN, revents: 0 };
                *who.add(k) = t as *mut c_void;
                k += 1;
                te = (*te).next;
            }
            pe = (*pe).next;
        }
        let empty = list_empty(phead);
        let timeout: c_int = if empty { 1000 } else { -1 };
        let r = poll(pfd, k as u32, timeout);
        if r < 0 {
            if errno() == EINTR {
                if G_DUMP.load(Ordering::Relaxed) != 0 {
                    G_DUMP.store(0, Ordering::Relaxed);
                    dump_state();
                }
                continue;
            }
            let why = strerror(errno());
            hub_log!("poll: %s", why);
            break;
        }
        if r == 0 {
            if empty && time(null_mut()).wrapping_sub(idle_since) >= idle_s as c_long {
                break;
            }
            continue;
        }
        // Handle one ready descriptor at a time, re-polling after each: any
        // event can free the procs and threads the array names.
        let mut i: usize = k;
        let mut j: usize = 0;
        while j < k {
            let x = ROT.wrapping_add(j) % k;
            if (*pfd.add(x)).revents != 0 {
                i = x;
                break;
            }
            j += 1;
        }
        if i == k {
            continue;
        }
        ROT = i + 1;
        if i == 0 {
            let c = accept(ls, null_mut(), null_mut());
            if c >= 0 {
                fcntl(c, F_SETFD, FD_CLOEXEC);
                let tv = Timeval { tv_sec: 10, tv_usec: 0 };
                let tvp = &tv as *const Timeval as *const c_void;
                setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, tvp, size_of::<Timeval>() as u32);
                setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, tvp, size_of::<Timeval>() as u32);
                let p: *mut Proc = xnew();
                (*p).fd = c;
                G_PROC_ID = G_PROC_ID.wrapping_add(1);
                (*p).id = G_PROC_ID;
                list_init(addr_of_mut!((*p).threads));
                list_init(addr_of_mut!((*p).nodes));
                list_init(addr_of_mut!((*p).refs));
                list_init(addr_of_mut!((*p).todo));
                list_init(addr_of_mut!((*p).delivered_death));
                list_init(addr_of_mut!((*p).waiting_threads));
                list_init(addr_of_mut!((*p).buffers));
                list_add_tail(addr_of_mut!((*p).entry), phead);
            }
        } else {
            // Find the object again (the arrays are rebuilt every round).
            let obj = *who.add(i);
            let mut is_proc = false;
            pe = (*phead).next;
            while pe != phead {
                if container_of!(pe, Proc, entry) as *mut c_void == obj {
                    is_proc = true;
                    break;
                }
                pe = (*pe).next;
            }
            if is_proc {
                proc_message(obj as *mut Proc);
            } else {
                thread_message(obj as *mut Thread);
            }
        }
        settle();
        if !list_empty(phead) || !empty {
            idle_since = time(null_mut());
        }
    }
    // Leave the name to a successor only if it is still ours.
    let mut now: Stat = zeroed();
    if stat(sun_path, &mut now) == 0 && now.st_ino == sock_st.st_ino {
        unlink(sun_path);
    }
    close(ls);
    hub_log!(
        "exiting (%s)",
        if G_STOP.load(Ordering::Relaxed) != 0 { c"signal".as_ptr() } else { c"idle".as_ptr() }
    );
    0
}
