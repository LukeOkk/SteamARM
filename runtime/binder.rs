// /dev/binder, /dev/hwbinder and /dev/vndbinder for guest processes.
//
// The driver itself is the hub (runtime/binder_hub.c, a host process started
// on demand). This side is what a guest process's syscalls reach:
//
//   open    connects to the hub (starting it if needed) and says hello; the
//           guest's descriptor IS that connection, the "process channel",
//           so poll and epoll on it work unchanged: the hub makes it
//           readable (one doorbell byte) when work waits for a polling
//           thread, as binder_poll would report;
//   ioctl   BINDER_WRITE_READ gathers what the kernel would copy from the
//           sender (the write bytes, each transaction's data and offsets,
//           scatter-gather buffers, the file descriptors named by
//           BINDER_TYPE_FD/FDA objects) and sends it on the calling
//           thread's own channel; the result brings the BR_* bytes for the
//           read buffer, descriptors to install (SCM_RIGHTS) and where in
//           the receive buffer their numbers go;
//   mmap    creates the receive buffer as a file mapped read-only for the
//           guest (Linux forbids PROT_WRITE on binder) and read-write for
//           the hub, which copies incoming transactions into it;
//   fork    the child keeps nothing (Linux: VM_DONTCOPY on the mapping, and
//           a binder_proc belongs to the process that opened it).
//
// A guest thread blocked in BINDER_WRITE_READ is blocked in recv() on its
// channel. A signal interrupts it: the hub is told (BH_CANCEL) and answers
// with the real result if it had one, else -EINTR, which libbinder retries
// and the dispatcher restarts under SA_RESTART with the consumed counts
// advanced, as Linux's restart does.
//
// This is a line-for-line port of the former binder.c: the same pthread
// mutexes, the same malloc'd records, the same thread-local channel table and
// the same host calls in the same order. Nothing here may panic or unwind.
// The Linux UAPI and the wire protocol shared with the hub are binder.h's.

use std::cell::{Cell, UnsafeCell};
use std::ffi::{c_char, c_int, c_long, c_short, c_uint, c_ulong, c_ulonglong, c_void};
use std::mem::{size_of, zeroed, MaybeUninit};
use std::ptr;
use std::sync::atomic::{AtomicI32, Ordering};

// Darwin errno values (host side; converted with lxrt_errno_to_linux).
const EPERM: c_int = 1;
const ENOENT: c_int = 2;
const EINTR: c_int = 4;
const EIO: c_int = 5;
const EBADF: c_int = 9;
const ENOMEM: c_int = 12;
const EFAULT: c_int = 14;
const EBUSY: c_int = 16;
const EINVAL: c_int = 22;
const EMFILE: c_int = 24;
const EPIPE: c_int = 32;
const EAGAIN: c_int = 35;
const EWOULDBLOCK: c_int = EAGAIN;
const ENAMETOOLONG: c_int = 63;

// Darwin <fcntl.h>, <sys/socket.h>, <sys/file.h>, <poll.h>, <sys/stat.h>,
// <sys/mman.h>, <spawn.h>, <time.h>, <limits.h>.
const O_RDONLY: c_int = 0;
const O_WRONLY: c_int = 1;
const O_RDWR: c_int = 2;
const O_NONBLOCK: c_int = 0x4;
const O_APPEND: c_int = 0x8;
const O_CREAT: c_int = 0x200;
const O_CLOEXEC: c_int = 0x0100_0000;
const F_GETFD: c_int = 1;
const F_SETFD: c_int = 2;
const F_GETFL: c_int = 3;
const F_SETFL: c_int = 4;
const F_GETPATH: c_int = 50;
const F_DUPFD_CLOEXEC: c_int = 67;
const FD_CLOEXEC: c_int = 1;
const AF_UNIX: c_int = 1;
const SOCK_STREAM: c_int = 1;
const SOL_SOCKET: c_int = 0xffff;
const SCM_RIGHTS: c_int = 1;
const SO_SNDBUF: c_int = 0x1001;
const SO_RCVBUF: c_int = 0x1002;
const SO_RCVTIMEO: c_int = 0x1006;
const MSG_TRUNC: c_int = 0x10;
const MSG_CTRUNC: c_int = 0x20;
const MSG_DONTWAIT: c_int = 0x80;
const SHUT_RDWR: c_int = 2;
const LOCK_SH: c_int = 1;
const LOCK_EX: c_int = 2;
const LOCK_NB: c_int = 4;
const LOCK_UN: c_int = 8;
const POLLIN: c_short = 0x1;
const POLLOUT: c_short = 0x4;
const S_IFMT: u16 = 0o170000;
const S_IFCHR: u16 = 0o020000;
const S_IFDIR: u16 = 0o040000;
const S_IFSOCK: u16 = 0o140000;
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const MAP_SHARED: c_int = 1;
const MAP_FIXED: c_int = 0x10;
const MAP_FAILED: *mut c_void = !0usize as *mut c_void;
const POSIX_SPAWN_CLOEXEC_DEFAULT: c_short = 0x4000;
const CLOCK_REALTIME: c_int = 0;
const PATH_MAX: usize = 1024;
const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;
const PTHREAD_ONCE_SIG_INIT: c_long = 0x30B1_BCBA;

// Mach.
const KERN_SUCCESS: c_int = 0;
const VM_INHERIT_NONE: c_int = 2;
const VM_PROT_READ: c_int = 1;
const TRUE: c_int = 1;

// lxrt.h.
const LXRT_HOST_PAGE: u64 = 16384;

// ------------------------------------------------------------ Linux UAPI
// (binder.h: include/uapi/linux/android/binder.h, 64-bit layout, version 8)
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
const fn lx_iowr(t: u8, n: u32, s: usize) -> u32 {
    lx_ioc(3, t, n, s)
}
const fn lx_ioc_size(c: u32) -> u32 {
    (c >> 16) & 0x3fff
}

const B_TYPE_LARGE: u32 = 0x85;
const fn b_pack_chars(c1: u8, c2: u8, c3: u8, c4: u32) -> u32 {
    ((c1 as u32) << 24) | ((c2 as u32) << 16) | ((c3 as u32) << 8) | c4
}
const BINDER_TYPE_BINDER: u32 = b_pack_chars(b's', b'b', b'*', B_TYPE_LARGE);
const BINDER_TYPE_WEAK_BINDER: u32 = b_pack_chars(b'w', b'b', b'*', B_TYPE_LARGE);
const BINDER_TYPE_HANDLE: u32 = b_pack_chars(b's', b'h', b'*', B_TYPE_LARGE);
const BINDER_TYPE_WEAK_HANDLE: u32 = b_pack_chars(b'w', b'h', b'*', B_TYPE_LARGE);
const BINDER_TYPE_FD: u32 = b_pack_chars(b'f', b'd', b'*', B_TYPE_LARGE);
const BINDER_TYPE_FDA: u32 = b_pack_chars(b'f', b'd', b'a', B_TYPE_LARGE);
const BINDER_TYPE_PTR: u32 = b_pack_chars(b'p', b't', b'*', B_TYPE_LARGE);

// struct flat_binder_object is 24 bytes (BINDER_SET_CONTEXT_MGR_EXT's size).
const FLAT_BINDER_OBJECT_SIZE: usize = 24;

// struct binder_fd_object: { hdr, pad_flags, union { pad_binder; fd }, cookie }.
#[repr(C)]
#[derive(Clone, Copy)]
struct BinderFdObject {
    hdr_type: u32,
    pad_flags: u32,
    fd: u32,
    pad_binder_high: u32,
    cookie: u64,
}
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
#[repr(C)]
#[derive(Clone, Copy)]
struct BinderWriteRead {
    write_size: u64,
    write_consumed: u64,
    write_buffer: u64,
    read_size: u64,
    read_consumed: u64,
    read_buffer: u64,
}
// struct binder_transaction_data: `target` is union { u32 handle; u64 ptr },
// `data` is union { struct { u64 buffer; u64 offsets; } ptr; u8 buf[8]; }.
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
    data_ptr_buffer: u64,
    data_ptr_offsets: u64,
}
const BINDER_VERSION_SIZE: usize = 4; // struct binder_version
const BINDER_NODE_DEBUG_INFO_SIZE: usize = 24; // struct binder_node_debug_info
const BINDER_NODE_INFO_FOR_REF_SIZE: usize = 24; // struct binder_node_info_for_ref
const BINDER_TRANSACTION_DATA_SG_SIZE: usize = size_of::<BinderTransactionData>() + 8;

const _: () = assert!(size_of::<BinderFdObject>() == 24);
const _: () = assert!(size_of::<BinderBufferObject>() == 40);
const _: () = assert!(size_of::<BinderFdArrayObject>() == 32);
const _: () = assert!(size_of::<BinderTransactionData>() == 64);
const _: () = assert!(size_of::<BinderWriteRead>() == 48);

const BINDER_WRITE_READ: u32 = lx_iowr(b'b', 1, size_of::<BinderWriteRead>());
const BINDER_SET_MAX_THREADS: u32 = lx_iow(b'b', 5, 4);
const BINDER_SET_CONTEXT_MGR: u32 = lx_iow(b'b', 7, 4);
const BINDER_THREAD_EXIT: u32 = lx_iow(b'b', 8, 4);
const BINDER_VERSION: u32 = lx_iowr(b'b', 9, BINDER_VERSION_SIZE);
const BINDER_GET_NODE_DEBUG_INFO: u32 = lx_iowr(b'b', 11, BINDER_NODE_DEBUG_INFO_SIZE);
const BINDER_GET_NODE_INFO_FOR_REF: u32 = lx_iowr(b'b', 12, BINDER_NODE_INFO_FOR_REF_SIZE);
const BINDER_SET_CONTEXT_MGR_EXT: u32 = lx_iow(b'b', 13, FLAT_BINDER_OBJECT_SIZE);

const BR_SPAWN_LOOPER: u32 = lx_io(b'r', 13);

const BC_TRANSACTION: u32 = lx_iow(b'c', 0, size_of::<BinderTransactionData>());
const BC_REPLY: u32 = lx_iow(b'c', 1, size_of::<BinderTransactionData>());
const BC_TRANSACTION_SG: u32 = lx_iow(b'c', 17, BINDER_TRANSACTION_DATA_SG_SIZE);
const BC_REPLY_SG: u32 = lx_iow(b'c', 18, BINDER_TRANSACTION_DATA_SG_SIZE);

// ------------------------------------------------------------ wire protocol
// (binder.h; shared with binder_hub.c)
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
#[derive(Clone, Copy)]
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
    write_len: u64,     // write bytes that follow (from write_consumed on)
    read_avail: u64,    // read_size - read_consumed
    read_consumed: u64, // as the guest passed it (0: the read starts with BR_NOOP)
    flags: u32,         // BH_WR_*
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
    cmd: u32, // the Linux ioctl number, or BH_IOCTL_UNMAPPED
    pad: u32,
    arg: [u8; 64],
}
#[repr(C)]
#[derive(Clone, Copy)]
struct BhMmap {
    base: u64,
    size: u64,
}
// Result of any thread-channel request.
#[repr(C)]
#[derive(Clone, Copy)]
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

// ------------------------------------------------------------ Darwin types

type MachPort = u32;
type PosixSpawnAttr = *mut c_void;
type PosixSpawnFileActions = *mut c_void;

#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}
const _: [(); 64] = [(); size_of::<PthreadMutex>()];

#[repr(C)]
struct PthreadOnce {
    sig: c_long,
    opaque: [u8; 8],
}
const _: [(); 16] = [(); size_of::<PthreadOnce>()];

#[repr(C)]
#[derive(Clone, Copy)]
struct Timespec {
    tv_sec: c_long,
    tv_nsec: c_long,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Timeval {
    tv_sec: c_long,
    tv_usec: i32,
}
const _: [(); 16] = [(); size_of::<Timeval>()];

// struct stat (arm64 Darwin, 64-bit inodes).
#[repr(C)]
pub struct Stat {
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

#[repr(C)]
struct SockaddrUn {
    sun_len: u8,
    sun_family: u8,
    sun_path: [c_char; 104],
}
const _: [(); 106] = [(); size_of::<SockaddrUn>()];

#[repr(C)]
#[derive(Clone, Copy)]
struct Iovec {
    iov_base: *mut c_void,
    iov_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Msghdr {
    msg_name: *mut c_void,
    msg_namelen: u32,
    msg_iov: *mut Iovec,
    msg_iovlen: c_int,
    msg_control: *mut c_void,
    msg_controllen: u32,
    msg_flags: c_int,
}
const _: [(); 48] = [(); size_of::<Msghdr>()];

#[repr(C)]
struct Cmsghdr {
    cmsg_len: u32,
    cmsg_level: c_int,
    cmsg_type: c_int,
}

#[repr(C)]
struct Pollfd {
    fd: c_int,
    events: c_short,
    revents: c_short,
}

// <sys/socket.h>'s CMSG_* macros (__DARWIN_ALIGN32: 4-byte alignment).
const fn darwin_align32(n: usize) -> usize {
    (n + 3) & !3
}
const fn cmsg_space(l: usize) -> usize {
    darwin_align32(size_of::<Cmsghdr>()) + darwin_align32(l)
}
const fn cmsg_len(l: usize) -> usize {
    darwin_align32(size_of::<Cmsghdr>()) + l
}
#[inline(always)]
unsafe fn cmsg_data(c: *mut Cmsghdr) -> *mut u8 {
    c.cast::<u8>().add(darwin_align32(size_of::<Cmsghdr>()))
}
#[inline(always)]
unsafe fn cmsg_firsthdr(m: *const Msghdr) -> *mut Cmsghdr {
    if (*m).msg_controllen as usize >= size_of::<Cmsghdr>() {
        (*m).msg_control.cast()
    } else {
        ptr::null_mut()
    }
}
#[inline(always)]
unsafe fn cmsg_nxthdr(m: *const Msghdr, c: *mut Cmsghdr) -> *mut Cmsghdr {
    if c.is_null() {
        return cmsg_firsthdr(m);
    }
    let next = (c as usize).wrapping_add(darwin_align32((*c).cmsg_len as usize));
    let end = ((*m).msg_control as usize).wrapping_add((*m).msg_controllen as usize);
    if next.wrapping_add(darwin_align32(size_of::<Cmsghdr>())) > end {
        ptr::null_mut()
    } else {
        next as *mut Cmsghdr
    }
}

// union { struct cmsghdr c; char b[CMSG_SPACE(sizeof(int) * BH_MAX_FDS)]; }
const CM_SIZE: usize = cmsg_space(size_of::<c_int>() * BH_MAX_FDS);
#[repr(C, align(4))]
struct CmsgBuf {
    b: [u8; CM_SIZE],
}

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: MachPort;
    static mut environ: *mut *mut c_char;

    fn __error() -> *mut c_int;
    fn malloc(size: usize) -> *mut c_void;
    fn calloc(count: usize, size: usize) -> *mut c_void;
    fn realloc(pointer: *mut c_void, size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    fn memset(dst: *mut c_void, value: c_int, n: usize) -> *mut c_void;
    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
    fn strncmp(a: *const c_char, b: *const c_char, n: usize) -> c_int;
    fn strnlen(s: *const c_char, max: usize) -> usize;
    fn strerror(error: c_int) -> *mut c_char;
    fn strtoul(s: *const c_char, end: *mut *mut c_char, base: c_int) -> c_ulong;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn snprintf(buffer: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn clock_gettime(clock: c_int, ts: *mut Timespec) -> c_int;
    fn usleep(usec: c_uint) -> c_int;

    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn dup(fd: c_int) -> c_int;
    fn write(fd: c_int, buffer: *const c_void, count: usize) -> isize;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn flock(fd: c_int, operation: c_int) -> c_int;
    fn mkdir(path: *const c_char, mode: u16) -> c_int;
    fn lstat(path: *const c_char, st: *mut Stat) -> c_int;
    fn fstat(fd: c_int, st: *mut Stat) -> c_int;
    fn unlink(path: *const c_char) -> c_int;
    fn mkstemp(template: *mut c_char) -> c_int;
    fn ftruncate(fd: c_int, length: i64) -> c_int;
    fn getuid() -> u32;
    fn getgid() -> u32;
    fn getpid() -> c_int;
    fn waitpid(pid: c_int, status: *mut c_int, options: c_int) -> c_int;

    fn socket(domain: c_int, kind: c_int, protocol: c_int) -> c_int;
    fn socketpair(domain: c_int, kind: c_int, protocol: c_int, sv: *mut c_int) -> c_int;
    fn connect(fd: c_int, address: *const c_void, len: u32) -> c_int;
    fn setsockopt(
        fd: c_int,
        level: c_int,
        name: c_int,
        value: *const c_void,
        len: u32,
    ) -> c_int;
    fn sendmsg(fd: c_int, message: *const Msghdr, flags: c_int) -> isize;
    fn recvmsg(fd: c_int, message: *mut Msghdr, flags: c_int) -> isize;
    fn recv(fd: c_int, buffer: *mut c_void, len: usize, flags: c_int) -> isize;
    fn shutdown(fd: c_int, how: c_int) -> c_int;
    fn poll(fds: *mut Pollfd, count: c_uint, timeout: c_int) -> c_int;

    fn mmap(
        address: *mut c_void,
        len: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(address: *mut c_void, len: usize) -> c_int;
    fn minherit(address: *mut c_void, len: usize, inherit: c_int) -> c_int;
    fn mach_vm_read_overwrite(
        task: MachPort,
        address: u64,
        size: u64,
        data: u64,
        out_size: *mut u64,
    ) -> c_int;
    fn mach_vm_protect(
        task: MachPort,
        address: u64,
        size: u64,
        set_maximum: c_int,
        new_protection: c_int,
    ) -> c_int;

    fn pthread_mutex_init(mutex: *mut PthreadMutex, attr: *const c_void) -> c_int;
    fn pthread_mutex_destroy(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_once(once: *mut PthreadOnce, init: extern "C" fn()) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;

    fn posix_spawn(
        pid: *mut c_int,
        path: *const c_char,
        file_actions: *const PosixSpawnFileActions,
        attr: *const PosixSpawnAttr,
        argv: *const *mut c_char,
        envp: *const *mut c_char,
    ) -> c_int;
    fn posix_spawnattr_init(attr: *mut PosixSpawnAttr) -> c_int;
    fn posix_spawnattr_destroy(attr: *mut PosixSpawnAttr) -> c_int;
    fn posix_spawnattr_setflags(attr: *mut PosixSpawnAttr, flags: c_short) -> c_int;
    fn posix_spawn_file_actions_init(actions: *mut PosixSpawnFileActions) -> c_int;
    fn posix_spawn_file_actions_destroy(actions: *mut PosixSpawnFileActions) -> c_int;
    fn posix_spawn_file_actions_addopen(
        actions: *mut PosixSpawnFileActions,
        fd: c_int,
        path: *const c_char,
        flags: c_int,
        mode: u16,
    ) -> c_int;
    fn _NSGetExecutablePath(buffer: *mut c_char, size: *mut u32) -> c_int;

    // The rest of the runtime.
    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
    fn lxrt_guest_close_fd(fd: c_int) -> c_long; // dispatch.c: close() as the guest's own
    fn lxrt_gbase() -> u64;
    fn lxrt_gettid() -> c_int;
    fn lxrt_aids_euid() -> u32;
    fn lxrt_ids_pid() -> c_int;
}

#[inline(always)]
unsafe fn errno() -> c_int {
    *__error()
}
#[inline(always)]
unsafe fn lerr(e: c_int) -> c_long {
    -(lxrt_errno_to_linux(e) as c_long)
}
#[inline(always)]
const fn align8(x: u64) -> u64 {
    x.wrapping_add(7) & !7u64
}
#[inline(always)]
fn task() -> MachPort {
    unsafe { mach_task_self_ }
}
#[inline(always)]
const fn cs(s: &std::ffi::CStr) -> *const c_char {
    s.as_ptr()
}

const B_FDS: usize = 65536;
const MAX_TCHAN: usize = 8;
const SZ_4M: u64 = 4 << 20;

#[repr(C)]
struct Bfile {
    naliases: c_int,         // guest descriptors naming it
    refs: c_int,             // aliases + mapping + thread channels + in-flight calls
    chan: c_int,             // our own descriptor of the process channel
    context: c_int,
    forked: bool,            // inherited over fork(): unusable here
    closed: bool,            // the guest closed its last descriptor
    map_pending: bool,       // an mmap of it is being set up (g_lock)
    drained: u64,            // doorbells read so far
    lock: PthreadMutex,      // process-channel writes, doorbell reads
    map_lock: PthreadMutex,  // the mapping: fd fixups against munmap
    map_base: u64,
    map_len: u64,
    map_host_len: u64,
    alias: *mut u8,          // our read-write view of the receive buffer
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Tchan {
    f: *mut Bfile,
    fd: c_int,
    hub_end: c_int, // our copy of the hub's end, until the hub has answered (tchan_get)
    polled: bool,
    busy: bool,
    seq: u64,
}

impl Tchan {
    const ZERO: Self = Self {
        f: ptr::null_mut(),
        fd: 0,
        hub_end: 0,
        polled: false,
        busy: false,
        seq: 0,
    };
}

static mut G_FILES: [*mut Bfile; B_FDS] = [ptr::null_mut(); B_FDS];
static G_NOPENED: AtomicI32 = AtomicI32::new(0); // files ever opened (fork: skip the scan if none)
static mut G_MAPPED: [*mut Bfile; 16] = [ptr::null_mut(); 16]; // files with a receive buffer (munmap checks)
static G_NMAPPED: AtomicI32 = AtomicI32::new(0);
static mut G_MAP_PENDING: c_int = 0; // g_mapped slots reserved by mmaps in progress (g_lock)
static mut G_LOCK: PthreadMutex = PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
};

#[inline(always)]
unsafe fn g_lock() {
    pthread_mutex_lock(ptr::addr_of_mut!(G_LOCK));
}
#[inline(always)]
unsafe fn g_unlock() {
    pthread_mutex_unlock(ptr::addr_of_mut!(G_LOCK));
}
#[inline(always)]
unsafe fn g_file(fd: c_int) -> *mut *mut Bfile {
    ptr::addr_of_mut!(G_FILES).cast::<*mut Bfile>().add(fd as usize)
}
#[inline(always)]
unsafe fn g_mapped(i: c_int) -> *mut *mut Bfile {
    ptr::addr_of_mut!(G_MAPPED).cast::<*mut Bfile>().add(i as usize)
}

// LXRT_FORK_SAFE(binder_g_lock, g_lock): taken before fork, released after it
// in both parent and child (the child's only thread is the one that took it).
extern "C" fn binder_g_lock_fork_prepare() {
    unsafe { g_lock() };
}
extern "C" fn binder_g_lock_fork_release() {
    unsafe { g_unlock() };
}

// static _Thread_local in the C. Const-initialized and without destructors:
// plain thread-local variables, nothing is allocated or registered on access.
thread_local! {
    static T_CHANS: UnsafeCell<[Tchan; MAX_TCHAN]> =
        const { UnsafeCell::new([Tchan::ZERO; MAX_TCHAN]) };
    static T_NCHANS: Cell<c_int> = const { Cell::new(0) };
    static T_RECV_FAIL: Cell<*const c_char> = const { Cell::new(ptr::null()) };
    static T_RECV_ERRNO: Cell<c_int> = const { Cell::new(0) };
}

#[inline(always)]
fn t_chans() -> *mut Tchan {
    T_CHANS.with(|c| c.get().cast::<Tchan>())
}
#[inline(always)]
fn t_nchans() -> c_int {
    T_NCHANS.with(|c| c.get())
}
#[inline(always)]
fn set_t_nchans(n: c_int) {
    T_NCHANS.with(|c| c.set(n));
}

// Every channel of this process to the hub -- each binder file's process
// channel and each thread's -- so a fork child can close the parent's (their
// threads do not exist in the child) and a guest close() cannot cut one
// (lxrt_binder_owns).
const CHAN_FDS_MAX: usize = 1024;
static mut G_CHAN_FDS: [c_int; CHAN_FDS_MAX] = [0; CHAN_FDS_MAX];
static mut G_NCHAN_FDS: c_int = 0;

#[inline(always)]
unsafe fn g_chan_fd(i: c_int) -> *mut c_int {
    ptr::addr_of_mut!(G_CHAN_FDS).cast::<c_int>().add(i as usize)
}

unsafe fn chan_register(fd: c_int) {
    g_lock();
    if G_NCHAN_FDS < CHAN_FDS_MAX as c_int {
        *g_chan_fd(G_NCHAN_FDS) = fd;
        G_NCHAN_FDS += 1;
    }
    g_unlock();
}
unsafe fn chan_unregister(fd: c_int) {
    g_lock();
    let mut i = 0;
    while i < G_NCHAN_FDS {
        if *g_chan_fd(i) == fd {
            G_NCHAN_FDS -= 1;
            *g_chan_fd(i) = *g_chan_fd(G_NCHAN_FDS);
            break;
        }
        i += 1;
    }
    g_unlock();
}

// ------------------------------------------------------------ guest memory

// A guest pointer is the guest's choice; reading or writing through a bad
// one must be EFAULT, not the runtime dying. mach_vm_read_overwrite copies
// within this task and reports a bad range instead of faulting.
unsafe fn gcopy(dst: *mut c_void, src: *const c_void, n: usize) -> bool {
    if n == 0 {
        return true;
    }
    if dst.is_null() || src.is_null() {
        return false;
    }
    let mut out: u64 = 0;
    let kr = mach_vm_read_overwrite(task(), src as usize as u64, n as u64, dst as usize as u64, &mut out);
    kr == KERN_SUCCESS && out == n as u64
}

// A 32-bit guest (i386 Android under FEX's 32-bit mode) lives at host =
// guest base + guest address, and the addresses inside binder's structures
// -- binder_write_read's buffers, a transaction's data and offsets, a PTR
// object's buffer -- are its own, 32 bits in 64-bit fields (Android's binder
// ABI on a 64-bit kernel). Below 4 GiB an address is always a guest one on
// this host (gbase.rs's rule), so the base is added where this file reads or
// writes through such an address. The other way, the hub is told the
// receive buffer's guest address (lxrt_binder_mmap), so every address it
// hands back (BR_TRANSACTION's data, PTR buffers in the target) is the
// guest's. Before: "Binder ioctl to obtain version failed: Bad address"
// and the 32-bit audio HAL ended there (benchmarks/stage28-android-
// reliability.txt).
unsafe fn gp(a: u64) -> u64 {
    let b = lxrt_gbase();
    if b != 0 && a != 0 && a < (1u64 << 32) {
        a.wrapping_add(b)
    } else {
        a
    }
}

// ------------------------------------------------------------ files

unsafe fn file_get(fd: c_int) -> *mut Bfile {
    if fd < 0 || fd >= B_FDS as c_int {
        return ptr::null_mut();
    }
    g_lock();
    let f = *g_file(fd);
    if !f.is_null() {
        (*f).refs += 1;
    }
    g_unlock();
    f
}

unsafe fn file_put(f: *mut Bfile) {
    g_lock();
    (*f).refs -= 1;
    let last = (*f).refs == 0;
    g_unlock();
    if !last {
        return;
    }
    if (*f).chan >= 0 {
        chan_unregister((*f).chan);
        close((*f).chan);
    }
    pthread_mutex_destroy(ptr::addr_of_mut!((*f).lock));
    pthread_mutex_destroy(ptr::addr_of_mut!((*f).map_lock));
    free(f.cast());
}

/// Has this process ever opened one?
#[no_mangle]
pub extern "C" fn lxrt_binder_any() -> bool {
    G_NOPENED.load(Ordering::SeqCst) != 0
}

#[no_mangle]
pub extern "C" fn lxrt_binder_is(fd: c_int) -> bool {
    fd >= 0 && fd < B_FDS as c_int && unsafe { !(*g_file(fd)).is_null() }
}

// static int enabled = -1 in lxrt_binder_context_of.
static CONTEXT_ENABLED: AtomicI32 = AtomicI32::new(-1);

/// -1: not a binder device.
#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_context_of(p: *const c_char) -> c_int {
    let mut enabled = CONTEXT_ENABLED.load(Ordering::Relaxed);
    if enabled < 0 {
        let e = getenv(cs(c"LXRT_BINDER"));
        enabled = !(!e.is_null() && strcmp(e, cs(c"0")) == 0) as c_int;
        CONTEXT_ENABLED.store(enabled, Ordering::Relaxed);
    }
    if enabled == 0 || p.is_null() || strncmp(p, cs(c"/dev/"), 5) != 0 {
        return -1;
    }
    let mut p = p.add(5);
    if strncmp(p, cs(c"binderfs/"), 9) == 0 {
        p = p.add(9);
    }
    if strcmp(p, cs(c"binder")) == 0 {
        return 0;
    }
    if strcmp(p, cs(c"hwbinder")) == 0 {
        return 1;
    }
    if strcmp(p, cs(c"vndbinder")) == 0 {
        return 2;
    }
    -1
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_stat(p: *const c_char, st: *mut Stat) -> bool {
    if lxrt_binder_context_of(p) < 0 {
        return false;
    }
    memset(st.cast(), 0, size_of::<Stat>());
    // What a binderfs node reports: a character device, 0600 root... here
    // owned by the caller, since every process of the guest shares one uid.
    (*st).st_mode = S_IFCHR | 0o666;
    (*st).st_uid = getuid();
    (*st).st_gid = getgid();
    (*st).st_nlink = 1;
    (*st).st_rdev = (10 << 24) | (60 + lxrt_binder_context_of(p));
    true
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_fstat(fd: c_int, st: *mut Stat) -> bool {
    let f = file_get(fd);
    if f.is_null() {
        return false;
    }
    // names[f->context < 3 ? f->context : 0]
    let name = match (*f).context {
        1 => cs(c"/dev/hwbinder"),
        2 => cs(c"/dev/vndbinder"),
        _ => cs(c"/dev/binder"),
    };
    let ok = lxrt_binder_stat(name, st);
    file_put(f);
    ok
}

// Descriptors the runtime keeps for itself sit high, out of the guest's way
// (a guest that dup2()s onto a low number must not hit one).
unsafe fn hide_fd(fd: c_int) -> c_int {
    const BASES: [c_int; 4] = [800, 400, 200, 64];
    let mut i = 0;
    while i < BASES.len() {
        let n = fcntl(fd, F_DUPFD_CLOEXEC, BASES[i]);
        if n >= 0 {
            close(fd);
            return n;
        }
        i += 1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fd
}

// Why a binder call failed in a way the guest cannot explain ("Bad file
// descriptor" from BinderProxy.transactNative with a driver that is there):
// one line per event in <binder dir>/client.log. Guests' stderr is often
// /dev/null (the zygote's children), so it goes to a file of the hub's
// directory. Costs nothing on the success paths.
//
// The C function was variadic (vsnprintf). Rust cannot define one, so it is
// split around the formatting call: diag_begin writes the path and the line's
// prefix, the macro formats the message with snprintf at the call site, and
// diag_end appends the line to the log.
const DIAG_LINE: usize = 400;
#[repr(C)]
struct Diag {
    path: [c_char; 300],
    line: [c_char; DIAG_LINE],
    n: c_int,
}

unsafe fn diag_begin(d: *mut Diag) -> bool {
    let dir = binder_dir();
    if dir.is_null() {
        return false;
    }
    let path = ptr::addr_of_mut!((*d).path).cast::<c_char>();
    let line = ptr::addr_of_mut!((*d).line).cast::<c_char>();
    snprintf(path, 300, cs(c"%s/client.log"), dir);
    let mut ts = Timespec { tv_sec: 0, tv_nsec: 0 };
    clock_gettime(CLOCK_REALTIME, &mut ts);
    (*d).n = snprintf(
        line,
        DIAG_LINE,
        cs(c"[%ld.%03ld pid %d tid %ld] "),
        ts.tv_sec,
        ts.tv_nsec / 1_000_000,
        getpid(),
        lxrt_gettid() as c_long,
    );
    true
}

unsafe fn diag_end(d: *mut Diag) {
    let path = ptr::addr_of_mut!((*d).path).cast::<c_char>();
    let line = ptr::addr_of_mut!((*d).line).cast::<c_char>();
    let mut l = strnlen(line, DIAG_LINE - 2);
    *line.add(l) = b'\n' as c_char;
    l += 1;
    let fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0o600 as c_int);
    if fd >= 0 {
        let _w = write(fd, line.cast(), l);
        close(fd);
    }
}

macro_rules! binder_diag {
    ($fmt:literal $(, $arg:expr)* $(,)?) => {{
        let mut d = MaybeUninit::<Diag>::uninit();
        let dp = d.as_mut_ptr();
        if diag_begin(dp) {
            let n = (*dp).n;
            if n > 0 && n < DIAG_LINE as c_int {
                snprintf(
                    ptr::addr_of_mut!((*dp).line).cast::<c_char>().add(n as usize),
                    DIAG_LINE - n as usize,
                    cs($fmt)
                    $(, $arg)*
                );
            }
            diag_end(dp);
        }
    }};
}

// The hub's directory. In /tmp by default, so it must be this user's own
// and closed to others: a directory someone else made (or can write to)
// would let them stand in for the driver. NULL: refuse to use it.
// Decided once per process: a second thread's first open used to read the
// answer before the first thread had computed it (ENOENT, and libbinder
// aborts on a driver it cannot open).
const G_DIR_SIZE: usize = 128;
static mut G_DIR: [c_char; G_DIR_SIZE] = [0; G_DIR_SIZE];
static mut G_DIR_OK: bool = false;
static mut G_DIR_ONCE: PthreadOnce = PthreadOnce {
    sig: PTHREAD_ONCE_SIG_INIT,
    opaque: [0; 8],
};

extern "C" fn binder_dir_check() {
    unsafe {
        let dir = ptr::addr_of_mut!(G_DIR).cast::<c_char>();
        let e = getenv(cs(c"LXRT_BINDER_DIR"));
        if !e.is_null() && *e != 0 {
            snprintf(dir, G_DIR_SIZE, cs(c"%s"), e);
        } else {
            snprintf(dir, G_DIR_SIZE, cs(c"/tmp/lxrt-binder-%u"), getuid() as c_uint);
        }
        mkdir(dir, 0o700);
        let mut st = MaybeUninit::<Stat>::uninit();
        let ok = lstat(dir, st.as_mut_ptr()) == 0 && {
            let st = st.as_ptr();
            ((*st).st_mode & S_IFMT) == S_IFDIR
                && (*st).st_uid == getuid()
                && ((*st).st_mode & 0o77) == 0
        };
        if !ok {
            fprintf(
                lxrt_trace_stream(),
                cs(c"[lxrt] binder: %s is not a private directory of this user; no binder driver\n"),
                dir,
            );
        }
        G_DIR_OK = ok;
    }
}

unsafe fn binder_dir() -> *const c_char {
    pthread_once(ptr::addr_of_mut!(G_DIR_ONCE), binder_dir_check);
    if G_DIR_OK {
        ptr::addr_of!(G_DIR).cast::<c_char>()
    } else {
        ptr::null()
    }
}

const SELF_EXE_SIZE: usize = 4096;
static mut SELF_EXE_PATH: [c_char; SELF_EXE_SIZE] = [0; SELF_EXE_SIZE];

unsafe fn self_exe() -> *const c_char {
    let path = ptr::addr_of_mut!(SELF_EXE_PATH).cast::<c_char>();
    if *path != 0 {
        return path;
    }
    let mut sz: u32 = SELF_EXE_SIZE as u32;
    if _NSGetExecutablePath(path, &mut sz) != 0 {
        *path = 0;
    }
    path
}

unsafe fn try_connect(dir: *const c_char) -> c_int {
    let mut sa: SockaddrUn = zeroed();
    sa.sun_family = AF_UNIX as u8;
    let cap = size_of::<[c_char; 104]>();
    if snprintf(sa.sun_path.as_mut_ptr(), cap, cs(c"%s/hub.sock"), dir) >= cap as c_int {
        return -ENAMETOOLONG;
    }
    let s = socket(AF_UNIX, SOCK_STREAM, 0);
    if s < 0 {
        return -errno();
    }
    if connect(s, (&sa as *const SockaddrUn).cast(), size_of::<SockaddrUn>() as u32) != 0 {
        let e = errno();
        close(s);
        return -e;
    }
    s
}

// Is a hub alive (serving, busy, or leaving)? It holds hub.pid locked for its
// whole life (binder_hub.c). Then a second one must not be started: it would
// take over the socket and split the driver in two.
unsafe fn hub_alive(dir: *const c_char) -> bool {
    let mut p = [0 as c_char; 256];
    snprintf(p.as_mut_ptr(), 256, cs(c"%s/hub.pid"), dir);
    let fd = open(p.as_ptr(), O_RDONLY | O_CLOEXEC);
    if fd < 0 {
        return false;
    }
    let held = flock(fd, LOCK_SH | LOCK_NB) != 0 && errno() == EWOULDBLOCK;
    close(fd);
    held
}

// The hub is `lxrun --binder-hub <dir> --daemon`: its first process binds the
// socket, forks the real hub and exits, so once waitpid returns the hub is
// listening and is nobody's child. No guest descriptor goes with it.
unsafe fn spawn_hub(dir: *const c_char) -> c_int {
    let exe = self_exe();
    if *exe == 0 {
        return -ENOENT;
    }
    let mut at: PosixSpawnAttr = ptr::null_mut();
    let mut fa: PosixSpawnFileActions = ptr::null_mut();
    posix_spawnattr_init(&mut at);
    posix_spawnattr_setflags(&mut at, POSIX_SPAWN_CLOEXEC_DEFAULT);
    posix_spawn_file_actions_init(&mut fa);
    posix_spawn_file_actions_addopen(&mut fa, 0, cs(c"/dev/null"), O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&mut fa, 1, cs(c"/dev/null"), O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&mut fa, 2, cs(c"/dev/null"), O_WRONLY, 0);
    let mut env: [*mut c_char; 8] = [ptr::null_mut(); 8];
    let mut ne = 0usize;
    // LXRT_SESSION too: the hub belongs to the session that started it
    // (scripts/run-app.sh and run-steam.sh leave an Android session's alone).
    let mut e = environ;
    while !(*e).is_null() && ne < 6 {
        if strncmp(*e, cs(c"LXRT_BINDER"), 11) == 0 || strncmp(*e, cs(c"LXRT_SESSION="), 13) == 0 {
            *env.as_mut_ptr().add(ne) = *e;
            ne += 1;
        }
        e = e.add(1);
    }
    *env.as_mut_ptr().add(ne) = ptr::null_mut();
    let argv: [*mut c_char; 5] = [
        exe.cast_mut(),
        cs(c"--binder-hub").cast_mut(),
        dir.cast_mut(),
        cs(c"--daemon").cast_mut(),
        ptr::null_mut(),
    ];
    let mut pid: c_int = 0;
    let rc = posix_spawn(&mut pid, exe, &fa, &at, argv.as_ptr(), env.as_ptr());
    posix_spawn_file_actions_destroy(&mut fa);
    posix_spawnattr_destroy(&mut at);
    if rc != 0 {
        return -rc;
    }
    let mut st: c_int = 0;
    while waitpid(pid, &mut st, 0) < 0 && errno() == EINTR {}
    0
}

unsafe fn send_msg(
    fd: c_int,
    h: *const BhHdr,
    pl: *const c_void,
    plen: usize,
    fds: *const c_int,
    nfds: c_int,
) -> c_int {
    let mut iov: [Iovec; 2] = [
        Iovec { iov_base: h.cast_mut().cast(), iov_len: size_of::<BhHdr>() },
        Iovec { iov_base: pl.cast_mut(), iov_len: plen },
    ];
    let iov = iov.as_mut_ptr();
    let mut cm = MaybeUninit::<CmsgBuf>::uninit();
    let mut m: Msghdr = zeroed();
    m.msg_iov = iov;
    m.msg_iovlen = if plen != 0 { 2 } else { 1 };
    if nfds > 0 {
        m.msg_control = cm.as_mut_ptr().cast();
        m.msg_controllen = cmsg_space(size_of::<c_int>() * nfds as usize) as u32;
        let c = cmsg_firsthdr(&m);
        (*c).cmsg_level = SOL_SOCKET;
        (*c).cmsg_type = SCM_RIGHTS;
        (*c).cmsg_len = cmsg_len(size_of::<c_int>() * nfds as usize) as u32;
        memcpy(cmsg_data(c).cast(), fds.cast(), size_of::<c_int>() * nfds as usize);
    }
    // A message is never abandoned half-sent: the hub reads it whole.
    let mut idx: c_int = 0;
    let niov: c_int = m.msg_iovlen;
    while idx < niov {
        let mut mm = m;
        mm.msg_iov = iov.add(idx as usize);
        mm.msg_iovlen = niov - idx;
        let mut n = sendmsg(fd, &mm, 0);
        if n < 0 {
            if errno() == EINTR {
                continue;
            }
            if errno() == EAGAIN {
                // a guest's O_NONBLOCK on the shared socket
                let mut p = Pollfd { fd, events: POLLOUT, revents: 0 };
                poll(&mut p, 1, 100);
                continue;
            }
            return -errno();
        }
        m.msg_control = ptr::null_mut();
        m.msg_controllen = 0;
        while n > 0 && idx < niov {
            let v = iov.add(idx as usize);
            if n as usize >= (*v).iov_len {
                n -= (*v).iov_len as isize;
                idx += 1;
            } else {
                (*v).iov_base = (*v).iov_base.cast::<u8>().add(n as usize).cast();
                (*v).iov_len -= n as usize;
                n = 0;
            }
        }
        while idx < niov && (*iov.add(idx as usize)).iov_len == 0 {
            idx += 1;
        }
    }
    0
}

// Receive one message. *interrupted is set when a signal cut the wait short
// (the caller decides what that means); fds get FD_CLOEXEC, as binder
// installs them.
// (t_recv_fail and t_recv_errno are the thread-locals T_RECV_FAIL and
// T_RECV_ERRNO above.)
#[inline(always)]
fn recv_fail(why: &'static std::ffi::CStr) {
    T_RECV_FAIL.with(|c| c.set(why.as_ptr()));
}

unsafe fn recv_msg(
    fd: c_int,
    h: *mut BhHdr,
    pl: *mut *mut u8,
    fds: *mut c_int,
    nfds: *mut c_int,
    interrupted: *mut bool,
) -> c_int {
    recv_fail(c"");
    T_RECV_ERRNO.with(|c| c.set(0));
    *pl = ptr::null_mut();
    *nfds = 0;
    let mut cm = MaybeUninit::<CmsgBuf>::uninit();
    let ok = 'fail: {
        let mut got: usize = 0;
        while got < size_of::<BhHdr>() {
            let mut iov = Iovec {
                iov_base: h.cast::<u8>().add(got).cast(),
                iov_len: size_of::<BhHdr>() - got,
            };
            let mut m = Msghdr {
                msg_name: ptr::null_mut(),
                msg_namelen: 0,
                msg_iov: &mut iov,
                msg_iovlen: 1,
                msg_control: cm.as_mut_ptr().cast(),
                msg_controllen: size_of::<CmsgBuf>() as u32,
                msg_flags: 0,
            };
            let n = recvmsg(fd, &mut m, 0);
            if n < 0 && errno() == EINTR {
                if !interrupted.is_null() {
                    *interrupted = true;
                }
                if got == 0 && !interrupted.is_null() {
                    return -EINTR;
                }
                continue;
            }
            if n <= 0 {
                T_RECV_ERRNO.with(|c| c.set(if n < 0 { errno() } else { 0 }));
                recv_fail(if n < 0 {
                    c"header recvmsg failed"
                } else if got != 0 {
                    c"end of file in the header"
                } else {
                    c"end of file"
                });
                break 'fail false;
            }
            if m.msg_flags & (MSG_CTRUNC | MSG_TRUNC) != 0 {
                binder_diag!(
                    c"channel %d: message flags 0x%x (truncated control data: descriptors lost)",
                    fd,
                    m.msg_flags
                );
            }
            let mut c = cmsg_firsthdr(&m);
            while !c.is_null() {
                if (*c).cmsg_level == SOL_SOCKET && (*c).cmsg_type == SCM_RIGHTS {
                    let k = (((*c).cmsg_len as usize).wrapping_sub(cmsg_len(0)) / size_of::<c_int>())
                        as c_int;
                    let mut i: c_int = 0;
                    while i < k {
                        let f: c_int = cmsg_data(c)
                            .add(i as usize * size_of::<c_int>())
                            .cast::<c_int>()
                            .read_unaligned();
                        fcntl(f, F_SETFD, FD_CLOEXEC);
                        if *nfds < BH_MAX_FDS as c_int {
                            *fds.add(*nfds as usize) = f;
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
            recv_fail(c"message too large");
            break 'fail false;
        }
        if (*h).len != 0 {
            *pl = malloc((*h).len as usize).cast();
            if (*pl).is_null() {
                recv_fail(c"out of memory");
                break 'fail false;
            }
            let mut g2: usize = 0;
            while (g2 as u64) < (*h).len {
                let n = recv(fd, (*pl).add(g2).cast(), (*h).len as usize - g2, 0);
                if n < 0 && errno() == EINTR {
                    continue;
                }
                if n <= 0 {
                    T_RECV_ERRNO.with(|c| c.set(if n < 0 { errno() } else { 0 }));
                    recv_fail(if n < 0 {
                        c"payload recv failed"
                    } else {
                        c"end of file in the payload"
                    });
                    break 'fail false;
                }
                g2 += n as usize;
            }
        }
        true
    };
    if ok {
        return 0;
    }
    free((*pl).cast());
    *pl = ptr::null_mut();
    let mut i: c_int = 0;
    while i < *nfds {
        close(*fds.add(i as usize));
        i += 1;
    }
    *nfds = 0;
    -EPIPE
}

// ------------------------------------------------------------ open

#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_open(context: c_int, lflags: c_int) -> c_long {
    let dir = binder_dir();
    if dir.is_null() {
        return lerr(ENOENT);
    }
    let mut s: c_int = -1;
    let mut waits: c_int = 0;
    let mut attempt: c_int = 0;
    while attempt < 4 && s < 0 {
        // The C loop body; `break 'next` is its `continue`.
        'next: {
            s = try_connect(dir);
            if s < 0 {
                let mut lp = [0 as c_char; 256];
                snprintf(lp.as_mut_ptr(), 256, cs(c"%s/hub.lock"), dir);
                let lk = open(lp.as_ptr(), O_RDWR | O_CREAT | O_CLOEXEC, 0o600 as c_int);
                if lk >= 0 {
                    flock(lk, LOCK_EX);
                }
                s = try_connect(dir);
                let mut alive = false;
                if s < 0 && {
                    alive = hub_alive(dir);
                    !alive
                } {
                    let r = spawn_hub(dir);
                    if r < 0 && lxrt_trace_on() {
                        fprintf(
                            lxrt_trace_stream(),
                            cs(c"[lxrt] binder: cannot start the hub: %s\n"),
                            strerror(-r),
                        );
                    }
                    s = try_connect(dir);
                }
                if lk >= 0 {
                    flock(lk, LOCK_UN);
                    close(lk);
                }
                if s < 0 {
                    // A hub that holds its lock but took no connection (its listen
                    // backlog full while it is busy, or leaving): wait for it, up
                    // to 10 s, rather than start a second one beside it.
                    if alive && {
                        let w = waits;
                        waits += 1;
                        w < 1000
                    } {
                        usleep(10000);
                        attempt -= 1;
                    }
                    break 'next;
                }
            }
            // Hello. A hub that was just exiting closes on us: try again.
            // The uid binder peers see (getCallingUid) is the Mac user's unless
            // LXRT_BINDER_UID names an Android one: what init's `user` line gives
            // a service (surfaceflinger runs as system, 1000; bootanimation as
            // graphics, 1003). Every guest is the same Mac user, and the hub
            // trusts its peers' word already, so this grants nothing a guest could
            // not claim anyway; it lets Android's own uid checks pass the way they
            // do on a device (SurfaceFlinger admits AID_GRAPHICS and AID_SYSTEM
            // without asking system_server's permission service, which is not
            // running: benchmarks/stage27-android-display.txt). Without it, the
            // process's Android ids (runtime/android_ids.c) decide, as init's
            // `user` line does when scripts/android-boot.py starts a service.
            let mut euid: u32 = lxrt_aids_euid();
            let ue = getenv(cs(c"LXRT_BINDER_UID"));
            if !ue.is_null() && *ue != 0 {
                let mut end: *mut c_char = ptr::null_mut();
                let v = strtoul(ue, &mut end, 10);
                if !end.is_null() && *end == 0 && v < 0x8000_0000 {
                    euid = v as u32;
                }
            }
            let mut hello: BhHello = zeroed();
            hello.version = BH_VERSION;
            hello.pid = lxrt_ids_pid();
            hello.euid = euid;
            hello.context = context as u32;
            let sc = getenv(cs(c"LXRT_BINDER_SECCTX"));
            snprintf(
                hello.secctx.as_mut_ptr(),
                size_of::<[c_char; 64]>(),
                cs(c"%s"),
                if !sc.is_null() && *sc != 0 { sc.cast_const() } else { cs(c"u:r:unlabeled:s0") },
            );
            let h = BhHdr { type_: BH_HELLO, nfds: 0, len: size_of::<BhHello>() as u64, seq: 0 };
            let tv = Timeval { tv_sec: 10, tv_usec: 0 };
            setsockopt(
                s,
                SOL_SOCKET,
                SO_RCVTIMEO,
                (&tv as *const Timeval).cast(),
                size_of::<Timeval>() as u32,
            );
            let mut ah: BhHdr = zeroed();
            let mut pl: *mut u8 = ptr::null_mut();
            // recv_msg may store up to BH_MAX_FDS
            let mut fds = MaybeUninit::<[c_int; BH_MAX_FDS]>::uninit();
            let fds = fds.as_mut_ptr().cast::<c_int>();
            let mut nfds: c_int = 0;
            let hs = send_msg(
                s,
                &h,
                (&hello as *const BhHello).cast(),
                size_of::<BhHello>(),
                ptr::null(),
                0,
            );
            let hr = if hs != 0 {
                0
            } else {
                recv_msg(s, &mut ah, &mut pl, fds, &mut nfds, ptr::null_mut())
            };
            let mut i: c_int = 0;
            while i < nfds {
                // an ack carries none
                close(*fds.add(i as usize));
                i += 1;
            }
            if hs != 0
                || hr != 0
                || ah.type_ != BH_HELLO_ACK
                || ah.len < size_of::<BhHelloAck>() as u64
            {
                let e = errno();
                binder_diag!(
                    c"open, attempt %d: hello %s (errno %d)",
                    attempt + 1,
                    if hs != 0 {
                        cs(c"not sent")
                    } else if hr != 0 {
                        cs(c"not answered")
                    } else {
                        cs(c"answered wrongly")
                    },
                    e
                );
                free(pl.cast());
                close(s);
                s = -1;
                break 'next;
            }
            let ack: BhHelloAck = pl.cast::<BhHelloAck>().read_unaligned();
            free(pl.cast());
            if ack.ret != 0 {
                close(s);
                return ack.ret as c_long;
            }
            let zero = Timeval { tv_sec: 0, tv_usec: 0 };
            setsockopt(
                s,
                SOL_SOCKET,
                SO_RCVTIMEO,
                (&zero as *const Timeval).cast(),
                size_of::<Timeval>() as u32,
            );
        }
        attempt += 1;
    }
    if s < 0 {
        binder_diag!(c"open: no hub in %s after 4 attempts", dir);
        return lerr(ENOENT); // no driver: what an absent /dev/binder says
    }
    if s >= B_FDS as c_int {
        close(s);
        return lerr(EMFILE);
    }
    let f: *mut Bfile = calloc(1, size_of::<Bfile>()).cast();
    if f.is_null() {
        close(s);
        return lerr(ENOMEM);
    }
    let d = dup(s);
    if d < 0 {
        free(f.cast());
        close(s);
        return lerr(errno());
    }
    (*f).chan = hide_fd(d);
    chan_register((*f).chan);
    (*f).context = context;
    (*f).naliases = 1;
    (*f).refs = 1;
    pthread_mutex_init(ptr::addr_of_mut!((*f).lock), ptr::null());
    pthread_mutex_init(ptr::addr_of_mut!((*f).map_lock), ptr::null());
    // Linux flags: O_CLOEXEC 0x80000, O_NONBLOCK 0x800.
    if lflags & 0x80000 != 0 {
        fcntl(s, F_SETFD, FD_CLOEXEC);
    }
    if lflags & 0x800 != 0 {
        fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    }
    g_lock();
    *g_file(s) = f;
    g_unlock();
    G_NOPENED.fetch_add(1, Ordering::SeqCst);
    s as c_long
}

// ------------------------------------------------------------ thread channels

// tchan_drop moves the last entry into the dropped one's slot. An entry whose
// call is in flight (this thread was interrupted inside it and a signal
// handler is running on top) must not move: its caller holds a pointer to it.
// Every drop is skipped while any entry is busy; the channel goes later, when
// tchan_get next looks, or at thread exit.
unsafe fn tchan_any_busy() -> bool {
    let chans = t_chans();
    let n = t_nchans();
    let mut i = 0;
    while i < n {
        if (*chans.add(i as usize)).busy {
            return true;
        }
        i += 1;
    }
    false
}

unsafe fn tchan_drop(tc: *mut Tchan) {
    chan_unregister((*tc).fd);
    close((*tc).fd);
    if (*tc).hub_end >= 0 {
        chan_unregister((*tc).hub_end);
        close((*tc).hub_end);
    }
    let f = (*tc).f;
    let chans = t_chans();
    let n = t_nchans() - 1;
    set_t_nchans(n);
    // *tc = t_chans[--t_nchans] (possibly onto itself)
    ptr::copy(chans.add(n as usize), tc, 1);
    memset(chans.add(n as usize).cast(), 0, size_of::<Tchan>());
    file_put(f);
}

unsafe fn tchan_get(f: *mut Bfile, err: *mut c_long) -> *mut Tchan {
    let chans = t_chans();
    let mut i: c_int = 0;
    while i < t_nchans() {
        if (*chans.add(i as usize)).f == f {
            return chans.add(i as usize);
        }
        i += 1;
    }
    // Channels of files closed since: gone with them. Not while a call of
    // this thread is in flight (a signal handler's): its entry must not move.
    let busy = tchan_any_busy();
    i = 0;
    while !busy && i < t_nchans() {
        if (*(*chans.add(i as usize)).f).closed {
            tchan_drop(chans.add(i as usize));
        } else {
            i += 1;
        }
    }
    if t_nchans() >= MAX_TCHAN as c_int {
        *err = lerr(ENOMEM);
        return ptr::null_mut();
    }
    let mut mine: c_int;
    let mut hubs: c_int;
    let mut attempt: c_int = 1;
    loop {
        let mut sv: [c_int; 2] = [0; 2];
        if socketpair(AF_UNIX, SOCK_STREAM, 0, sv.as_mut_ptr()) != 0 {
            *err = lerr(errno());
            return ptr::null_mut();
        }
        mine = hide_fd(sv[0]);
        // The hub's end goes to the hub by SCM_RIGHTS, and this process keeps
        // a copy of it until the hub has acknowledged the channel. Closed at
        // once, the end in flight had no reference but the message, and
        // XNU's unix-socket garbage collection (unp_gc) can flush such a
        // socket (HYPOTHESIS for the mechanism): the hub received a channel
        // already at end of file, dropped the thread, and the guest's call
        // failed with EBADF ("Bad file descriptor" from
        // BinderProxy.transactNative; system_server died of it several times
        // a boot, MEASURED at stage 28).
        hubs = hide_fd(sv[1]);
        let sz: c_int = 1 << 20;
        setsockopt(mine, SOL_SOCKET, SO_SNDBUF, (&sz as *const c_int).cast(), size_of::<c_int>() as u32);
        setsockopt(mine, SOL_SOCKET, SO_RCVBUF, (&sz as *const c_int).cast(), size_of::<c_int>() as u32);
        let bt = BhThread { tid: lxrt_gettid(), pad: 0 };
        let h = BhHdr { type_: BH_THREAD, nfds: 1, len: size_of::<BhThread>() as u64, seq: 0 };
        pthread_mutex_lock(ptr::addr_of_mut!((*f).lock));
        let r = send_msg(
            (*f).chan,
            &h,
            (&bt as *const BhThread).cast(),
            size_of::<BhThread>(),
            &hubs,
            1,
        );
        pthread_mutex_unlock(ptr::addr_of_mut!((*f).lock));
        if r != 0 {
            binder_diag!(
                c"new thread channel: send on the file's channel %d failed: %s",
                (*f).chan,
                strerror(-r)
            );
            close(hubs);
            close(mine);
            *err = lerr(EBADF);
            return ptr::null_mut();
        }
        // The hub acknowledges the channel on it, and refuses one that reaches
        // it already at end of file: a few per boot even with our copy held
        // (cause UNKNOWN, benchmarks/stage28-android-apk.txt); the thread's
        // first call then waited forever (am start -W for 300 s, MEASURED).
        // A new channel instead, a few times at most.
        let mut acked = false;
        let mut waited: c_int = 0;
        while waited < 5000 && !acked {
            let mut pfd = Pollfd { fd: mine, events: POLLIN, revents: 0 };
            let pr = poll(&mut pfd, 1, 100);
            if pr < 0 && errno() == EINTR {
                continue;
            }
            waited += 100;
            if pr <= 0 {
                continue;
            }
            let mut ah: BhHdr = zeroed();
            let mut apl: *mut u8 = ptr::null_mut();
            let mut afds = MaybeUninit::<[c_int; BH_MAX_FDS]>::uninit();
            let afds = afds.as_mut_ptr().cast::<c_int>();
            let mut anfds: c_int = 0;
            if recv_msg(mine, &mut ah, &mut apl, afds, &mut anfds, ptr::null_mut()) != 0 {
                break;
            }
            free(apl.cast());
            let mut k: c_int = 0;
            while k < anfds {
                close(*afds.add(k as usize));
                k += 1;
            }
            acked = ah.type_ == BH_THREAD_ACK;
            if !acked {
                break;
            }
        }
        if acked {
            break;
        }
        close(hubs);
        close(mine);
        if attempt >= 4 {
            binder_diag!(
                c"new thread channel: not acknowledged by the hub after %d attempts",
                attempt
            );
            *err = lerr(EBADF);
            return ptr::null_mut();
        }
        binder_diag!(
            c"new thread channel: not acknowledged by the hub (attempt %d); another one",
            attempt
        );
        attempt += 1;
    }
    close(hubs); // acknowledged: the hub has its end
    hubs = -1;
    chan_register(mine);
    g_lock();
    (*f).refs += 1;
    g_unlock();
    let n = t_nchans();
    set_t_nchans(n + 1);
    let tc = chans.add(n as usize);
    *tc = Tchan { f, fd: mine, hub_end: hubs, polled: false, busy: false, seq: 0 };
    tc
}

// Doorbells the hub has written and no longer wants pending: read them, so
// the guest's descriptor stops being readable exactly when binder_poll would.
unsafe fn drain_to(f: *mut Bfile, target: u64) {
    pthread_mutex_lock(ptr::addr_of_mut!((*f).lock));
    while (*f).drained < target {
        let mut buf = [0 as c_char; 64];
        let want = target - (*f).drained;
        let n = recv(
            (*f).chan,
            buf.as_mut_ptr().cast(),
            if want < 64 { want as usize } else { 64 },
            MSG_DONTWAIT,
        );
        if n <= 0 {
            break;
        }
        (*f).drained += n as u64;
    }
    pthread_mutex_unlock(ptr::addr_of_mut!((*f).lock));
}

// struct result
#[repr(C)]
struct HubResult {
    r: BhResult,
    pl: *mut u8,      // owns: payload
    rd: *const u8,    // BR bytes
    fix: *const u64,
    closes: *const i32,
    fds: [c_int; BH_MAX_FDS],
    nfds: c_int,
}

unsafe fn result_discard(out: *mut HubResult) {
    free((*out).pl.cast());
    let fds = ptr::addr_of_mut!((*out).fds).cast::<c_int>();
    let mut i: c_int = 0;
    while i < (*out).nfds {
        close(*fds.add(i as usize));
        i += 1;
    }
    (*out).pl = ptr::null_mut();
    (*out).nfds = 0;
}

// One request, one result. A signal while waiting turns into BH_CANCEL; the
// result that follows is either the real one or -EINTR.
unsafe fn roundtrip(
    tc: *mut Tchan,
    type_: u32,
    pl: *const c_void,
    plen: usize,
    fds: *const c_int,
    nfds: c_int,
    out: *mut HubResult,
) -> c_long {
    let f = (*tc).f;
    (*tc).seq = (*tc).seq.wrapping_add(1);
    let seq = (*tc).seq;
    let h = BhHdr { type_, nfds: nfds as u32, len: plen as u64, seq };
    let sr = send_msg((*tc).fd, &h, pl, plen, fds, nfds);
    if sr != 0 {
        binder_diag!(
            c"request type %u on thread channel %d: send failed: %s",
            type_,
            (*tc).fd,
            strerror(-sr)
        );
        return lerr(EBADF);
    }
    let mut cancelled = false;
    loop {
        let mut rh: BhHdr = zeroed();
        let mut intr = false;
        let r = recv_msg(
            (*tc).fd,
            &mut rh,
            ptr::addr_of_mut!((*out).pl),
            ptr::addr_of_mut!((*out).fds).cast(),
            ptr::addr_of_mut!((*out).nfds),
            &mut intr,
        );
        if r == -EINTR {
            if !cancelled {
                let ch = BhHdr { type_: BH_CANCEL, nfds: 0, len: 0, seq };
                send_msg((*tc).fd, &ch, ptr::null(), 0, ptr::null(), 0);
                cancelled = true;
            }
            continue;
        }
        if r != 0 {
            let re = T_RECV_ERRNO.with(|c| c.get());
            binder_diag!(
                c"request type %u on thread channel %d: no answer: %s%s%s",
                type_,
                (*tc).fd,
                T_RECV_FAIL.with(|c| c.get()),
                if re != 0 { cs(c", ") } else { cs(c"") },
                if re != 0 { strerror(re).cast_const() } else { cs(c"") }
            );
            return lerr(EBADF); // the hub is gone: no driver any more
        }
        if rh.type_ != BH_RESULT || rh.seq != seq || rh.len < size_of::<BhResult>() as u64 {
            result_discard(out);
            if rh.seq != seq {
                continue; // a stale answer: keep waiting
            }
            return lerr(EIO);
        }
        if (*tc).hub_end >= 0 {
            // the hub has its end: ours can go
            chan_unregister((*tc).hub_end);
            close((*tc).hub_end);
            (*tc).hub_end = -1;
        }
        memcpy(
            ptr::addr_of_mut!((*out).r).cast(),
            (*out).pl.cast(),
            size_of::<BhResult>(),
        );
        let need = (size_of::<BhResult>() as u64)
            .wrapping_add((*out).r.read_len)
            .wrapping_add(8u64.wrapping_mul((*out).r.nfixups as u64))
            .wrapping_add(4u64.wrapping_mul((*out).r.ncloses as u64));
        if need > rh.len || (*out).r.nfixups != (*out).nfds as u32 {
            result_discard(out);
            return lerr(EIO);
        }
        (*out).rd = (*out).pl.add(size_of::<BhResult>());
        (*out).fix = (*out).rd.wrapping_add((*out).r.read_len as usize).cast();
        (*out).closes = (*out)
            .fix
            .cast::<u8>()
            .wrapping_add(8 * (*out).r.nfixups as usize)
            .cast();
        break;
    }
    if (*out).r.bell_pending == 0 {
        drain_to(f, (*out).r.bells);
    }
    0
}

unsafe fn result_done(res: *mut HubResult) {
    // Descriptors of FDA buffers the guest freed: Linux closes them in the
    // freeing process (binder_deferred_fd_close).
    if !(*res).pl.is_null() {
        let mut i: u32 = 0;
        while i < (*res).r.ncloses {
            let fd: i32 = (*res).closes.add(i as usize).read_unaligned();
            if fd >= 0 && fd < B_FDS as c_int && !(*g_file(fd)).is_null() {
                binder_diag!(
                    c"FDA buffer freed: descriptor %d to close is a binder descriptor now",
                    fd
                );
            }
            let mut cst = MaybeUninit::<Stat>::uninit();
            if fd >= 0
                && fstat(fd, cst.as_mut_ptr()) == 0
                && ((*cst.as_ptr()).st_mode & S_IFMT) == S_IFSOCK
            {
                binder_diag!(c"FDA buffer freed: descriptor %d to close is a socket", fd);
            } else if fd >= 0 && fcntl(fd, F_GETFD) < 0 {
                binder_diag!(c"FDA buffer freed: descriptor %d to close is not open", fd);
            }
            if fd >= 0 {
                lxrt_guest_close_fd(fd);
            }
            i += 1;
        }
    }
    free((*res).pl.cast());
    (*res).pl = ptr::null_mut();
}

// ------------------------------------------------------------ BINDER_WRITE_READ

#[repr(C)]
struct Gbuf {
    p: *mut u8,
    len: usize,
    cap: usize,
    oom: bool,
}

unsafe fn gb_grow(b: *mut Gbuf, n: usize) -> *mut u8 {
    if (*b).oom {
        return ptr::null_mut();
    }
    if (*b).len.wrapping_add(n) > (*b).cap {
        let mut nc: usize = if (*b).cap != 0 { (*b).cap.wrapping_mul(2) } else { 4096 };
        while nc < (*b).len.wrapping_add(n) {
            nc = nc.wrapping_mul(2);
        }
        let np: *mut u8 = realloc((*b).p.cast(), nc).cast();
        if np.is_null() {
            (*b).oom = true;
            return ptr::null_mut();
        }
        (*b).p = np;
        (*b).cap = nc;
    }
    let at = (*b).p.wrapping_add((*b).len);
    (*b).len = (*b).len.wrapping_add(n);
    at
}

unsafe fn gb_pad8(b: *mut Gbuf) {
    let pad = (align8((*b).len as u64) - (*b).len as u64) as usize;
    let z = gb_grow(b, pad);
    if !z.is_null() {
        memset(z.cast(), 0, pad);
    }
}

// The unnamed per-object record of gather_txn.
#[repr(C)]
struct TxnObj {
    off: u64,
    sg: usize,
    len: u64,
    ptr: bool,
}

// What the kernel copies from the sender for one transaction (binder_
// transaction's copy_from_user calls), appended to `b`; descriptors named by
// FD and FDA objects are appended to `fds`.
unsafe fn gather_txn(
    b: *mut Gbuf,
    tr: *const BinderTransactionData,
    extra: u64,
    fds: *mut c_int,
    nfds: *mut c_int,
) {
    let hat = (*b).len;
    let mut a: BhTxnAtt = zeroed();
    if gb_grow(b, size_of::<BhTxnAtt>()).is_null() {
        return;
    }
    let fd0 = *nfds;
    let data_size = (*tr).data_size;
    let offsets_size = (*tr).offsets_size;
    // The C body up to its `out:` label.
    'out: {
        if data_size > BH_MAX_MSG / 2 || offsets_size > BH_MAX_MSG / 2 || extra > BH_MAX_MSG / 2 {
            a.fault = 28; // ENOSPC: larger than any receive buffer
            break 'out;
        }
        let dat = (*b).len;
        let d = gb_grow(b, align8(data_size) as usize);
        if d.is_null() {
            a.fault = 12;
            break 'out;
        }
        memset(
            d.add(data_size as usize).cast(),
            0,
            (align8(data_size) - data_size) as usize,
        );
        if !gcopy(d.cast(), gp((*tr).data_ptr_buffer) as usize as *const c_void, data_size as usize) {
            a.fault = 14; // EFAULT
            (*b).len = dat;
            break 'out;
        }
        let oat = (*b).len;
        let o = gb_grow(b, align8(offsets_size) as usize);
        if o.is_null() {
            a.fault = 12;
            break 'out;
        }
        memset(
            o.add(offsets_size as usize).cast(),
            0,
            (align8(offsets_size) - offsets_size) as usize,
        );
        if !gcopy(o.cast(), gp((*tr).data_ptr_offsets) as usize as *const c_void, offsets_size as usize)
        {
            a.fault = 14;
            (*b).len = dat;
            break 'out;
        }
        a.data_size = data_size;
        a.offsets_size = offsets_size;
        // Objects, in the order the hub will translate them. Only PTR contents and
        // descriptors are needed here; validity is the hub's to judge, so a bad
        // object just ends the walk (the hub then fails the transaction).
        let sgat = (*b).len;
        let nobj: u64 = offsets_size / 8;
        let objs: *mut TxnObj = if nobj != 0 {
            calloc(nobj as usize, size_of::<TxnObj>()).cast()
        } else {
            ptr::null_mut()
        };
        if nobj != 0 && objs.is_null() {
            a.fault = 12;
            (*b).len = dat;
            break 'out;
        }
        let mut i: u64 = 0;
        while i < nobj {
            let off: u64 = (*b).p.add(oat + 8 * i as usize).cast::<u64>().read_unaligned();
            if off > data_size || data_size - off < 8 || (off & 3) != 0 {
                break;
            }
            let obj_at = (*b).p.add(dat + off as usize);
            let type_: u32 = obj_at.cast::<u32>().read_unaligned();
            let oi = objs.add(i as usize);
            (*oi).off = off;
            if type_ == BINDER_TYPE_FD {
                if data_size - off < size_of::<BinderFdObject>() as u64 {
                    break;
                }
                let fo: BinderFdObject = obj_at.cast::<BinderFdObject>().read_unaligned();
                if *nfds >= BH_MAX_FDS as c_int {
                    a.fault = 24; // EMFILE
                    break;
                }
                if fcntl(fo.fd as c_int, F_GETFD) < 0 {
                    a.fault = 9; // EBADF
                    break;
                }
                *fds.add(*nfds as usize) = fo.fd as c_int;
                *nfds += 1;
            } else if type_ == BINDER_TYPE_PTR {
                if data_size - off < size_of::<BinderBufferObject>() as u64 {
                    break;
                }
                let bp: BinderBufferObject = obj_at.cast::<BinderBufferObject>().read_unaligned();
                if bp.length > extra {
                    break;
                }
                let at = (*b).len;
                let s = gb_grow(b, align8(bp.length) as usize);
                if s.is_null() {
                    a.fault = 12;
                    break;
                }
                memset(
                    s.add(bp.length as usize).cast(),
                    0,
                    (align8(bp.length) - bp.length) as usize,
                );
                if !gcopy(s.cast(), gp(bp.buffer) as usize as *const c_void, bp.length as usize) {
                    a.fault = 14;
                    break;
                }
                (*oi).ptr = true;
                (*oi).sg = at;
                (*oi).len = bp.length;
            } else if type_ == BINDER_TYPE_FDA {
                if data_size - off < size_of::<BinderFdArrayObject>() as u64 {
                    break;
                }
                let fda: BinderFdArrayObject = obj_at.cast::<BinderFdArrayObject>().read_unaligned();
                if fda.parent >= i || !(*objs.add(fda.parent as usize)).ptr {
                    break;
                }
                let parent = objs.add(fda.parent as usize);
                let plen: u64 = (*parent).len;
                if fda.num_fds > plen / 4 || fda.parent_offset > plen - 4 * fda.num_fds {
                    break;
                }
                let arr: *const u8 = (*b).p.add((*parent).sg).add(fda.parent_offset as usize);
                let mut bad = false;
                let mut k: u64 = 0;
                while k < fda.num_fds {
                    let fd: i32 = arr.add(4 * k as usize).cast::<i32>().read_unaligned();
                    if *nfds >= BH_MAX_FDS as c_int {
                        a.fault = 24;
                        bad = true;
                        break;
                    }
                    if fcntl(fd, F_GETFD) < 0 {
                        a.fault = 9;
                        bad = true;
                        break;
                    }
                    *fds.add(*nfds as usize) = fd;
                    *nfds += 1;
                    k += 1;
                }
                if bad {
                    break;
                }
            } else if type_ != BINDER_TYPE_BINDER
                && type_ != BINDER_TYPE_WEAK_BINDER
                && type_ != BINDER_TYPE_HANDLE
                && type_ != BINDER_TYPE_WEAK_HANDLE
            {
                break;
            }
            i += 1;
        }
        free(objs.cast());
        if a.fault != 0 {
            (*b).len = dat;
            a.data_size = 0;
            a.offsets_size = 0;
            *nfds = fd0;
            break 'out;
        }
        a.sg_size = ((*b).len - sgat) as u64;
        a.nfds = (*nfds - fd0) as u32;
    }
    if !(*b).oom {
        memcpy((*b).p.add(hat).cast(), (&a as *const BhTxnAtt).cast(), size_of::<BhTxnAtt>());
    }
}

unsafe fn write_read(tc: *mut Tchan, ubwr: u64, guest_fd: c_int) -> c_long {
    let mut bwr: BinderWriteRead = zeroed();
    if !gcopy(
        (&mut bwr as *mut BinderWriteRead).cast(),
        ubwr as usize as *const c_void,
        size_of::<BinderWriteRead>(),
    ) {
        return lerr(EFAULT);
    }
    let mut wlen: u64 = 0;
    if bwr.write_size > 0 && bwr.write_consumed < bwr.write_size {
        wlen = bwr.write_size - bwr.write_consumed;
    }
    if wlen > BH_MAX_MSG / 2 {
        return lerr(EINVAL);
    }
    let mut ravail: u64 = 0;
    if bwr.read_size > 0 && bwr.read_consumed < bwr.read_size {
        ravail = bwr.read_size - bwr.read_consumed;
    }

    let mut b = Gbuf { p: ptr::null_mut(), len: 0, cap: 0, oom: false };
    let mut wr = BhWr {
        write_len: wlen,
        read_avail: ravail,
        read_consumed: bwr.read_consumed,
        flags: 0,
        pad: 0,
    };
    let fl = fcntl(guest_fd, F_GETFL);
    if fl >= 0 && (fl & O_NONBLOCK) != 0 {
        wr.flags |= BH_WR_NONBLOCK;
    }
    if gb_grow(&mut b, size_of::<BhWr>()).is_null() {
        return lerr(ENOMEM);
    }
    let wat = b.len;
    let w = gb_grow(&mut b, wlen as usize);
    if wlen != 0 && w.is_null() {
        free(b.p.cast());
        return lerr(ENOMEM);
    }
    if !gcopy(
        w.cast(),
        gp(bwr.write_buffer).wrapping_add(bwr.write_consumed) as usize as *const c_void,
        wlen as usize,
    ) {
        free(b.p.cast());
        return lerr(EFAULT);
    }
    gb_pad8(&mut b);
    // Attachments: one per transaction command, in order.
    let mut fds = MaybeUninit::<[c_int; BH_MAX_FDS]>::uninit();
    let fds = fds.as_mut_ptr().cast::<c_int>();
    let mut nfds: c_int = 0;
    let mut pos: u64 = 0;
    while pos + 4 <= wlen {
        let cmd: u32 = b.p.add(wat + pos as usize).cast::<u32>().read_unaligned();
        let sz: u64 = lx_ioc_size(cmd) as u64;
        if pos + 4 + sz > wlen {
            break;
        }
        if cmd == BC_TRANSACTION || cmd == BC_REPLY || cmd == BC_TRANSACTION_SG || cmd == BC_REPLY_SG {
            let tr: BinderTransactionData = b
                .p
                .add(wat + pos as usize + 4)
                .cast::<BinderTransactionData>()
                .read_unaligned();
            let mut extra: u64 = 0;
            if cmd == BC_TRANSACTION_SG || cmd == BC_REPLY_SG {
                extra = b
                    .p
                    .add(wat + pos as usize + 4 + size_of::<BinderTransactionData>())
                    .cast::<u64>()
                    .read_unaligned();
            }
            gather_txn(&mut b, &tr, extra, fds, &mut nfds);
        }
        pos += 4 + sz;
    }
    if b.oom {
        free(b.p.cast());
        return lerr(ENOMEM);
    }
    memcpy(b.p.cast(), (&wr as *const BhWr).cast(), size_of::<BhWr>());

    let mut res: HubResult = zeroed();
    let rc = roundtrip(tc, BH_WRITE_READ, b.p.cast(), b.len, fds, nfds, &mut res);
    free(b.p.cast());
    if rc < 0 {
        return rc;
    }
    // Descriptors for the receive buffer: installed here, numbered here.
    // Under the map lock: another thread's munmap must not pull the view
    // out from under these writes.
    let f = (*tc).f;
    pthread_mutex_lock(ptr::addr_of_mut!((*f).map_lock));
    let mut i: u32 = 0;
    while i < res.r.nfixups {
        let off: u64 = res.fix.add(i as usize).read_unaligned();
        let fd: c_int = *ptr::addr_of!(res.fds).cast::<c_int>().add(i as usize);
        if !(*f).alias.is_null() && off.wrapping_add(4) <= (*f).map_len {
            let v: u32 = fd as u32;
            (*f).alias.add(off as usize).cast::<u32>().write_unaligned(v);
        } else {
            close(fd);
        }
        i += 1;
    }
    pthread_mutex_unlock(ptr::addr_of_mut!((*f).map_lock));
    if res.r.read_len != 0
        && !gcopy(
            gp(bwr.read_buffer).wrapping_add(bwr.read_consumed) as usize as *mut c_void,
            res.rd.cast(),
            res.r.read_len as usize,
        )
    {
        result_done(&mut res);
        return lerr(EFAULT);
    }
    if res.r.spawn_looper != 0 {
        let sl: u32 = BR_SPAWN_LOOPER;
        gcopy(gp(bwr.read_buffer) as usize as *mut c_void, (&sl as *const u32).cast(), 4);
    }
    bwr.write_consumed = bwr.write_consumed.wrapping_add(res.r.write_consumed);
    if res.r.arg[0] == 1 {
        // the write failed: Linux zeroes read_consumed
        bwr.read_consumed = 0;
    } else {
        bwr.read_consumed = bwr.read_consumed.wrapping_add(res.r.read_len);
    }
    let ret = res.r.ret as c_long;
    result_done(&mut res);
    if !gcopy(
        ubwr as usize as *mut c_void,
        (&bwr as *const BinderWriteRead).cast(),
        size_of::<BinderWriteRead>(),
    ) {
        return lerr(EFAULT);
    }
    ret
}

// ------------------------------------------------------------ ioctl

#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_ioctl(fd: c_int, lreq: c_ulong, arg: u64) -> c_long {
    let f = file_get(fd);
    if f.is_null() {
        binder_diag!(
            c"ioctl 0x%lx on descriptor %d, which is not a binder file (any more)",
            lreq,
            fd
        );
        return lerr(EBADF);
    }
    // The C body up to its `out:` label.
    let ret: c_long = 'out: {
        if (*f).forked || (*f).closed {
            break 'out lerr(EINVAL);
        }
        let req = lreq as u32;
        let mut err: c_long = 0;
        let tc = tchan_get(f, &mut err);
        if tc.is_null() {
            break 'out err;
        }
        if (*tc).busy {
            // A signal handler calling binder while this thread's own call is in
            // flight: its channel is mid-request. Refused rather than corrupted.
            break 'out lerr(EAGAIN);
        }
        (*tc).busy = true;
        let ret: c_long;
        if req == BINDER_WRITE_READ {
            ret = write_read(tc, arg, fd);
        } else {
            match req {
                BINDER_VERSION
                | BINDER_SET_MAX_THREADS
                | BINDER_SET_CONTEXT_MGR
                | BINDER_SET_CONTEXT_MGR_EXT
                | BINDER_THREAD_EXIT
                | BINDER_GET_NODE_DEBUG_INFO
                | BINDER_GET_NODE_INFO_FOR_REF => {
                    ret = 'sw: {
                        let mut io: BhIoctl = zeroed();
                        io.cmd = req;
                        let sz: u32 = lx_ioc_size(req);
                        let dir: u32 = req >> 30;
                        if sz as usize > size_of::<[u8; 64]>() {
                            break 'sw lerr(EINVAL);
                        }
                        if (dir & 1) != 0
                            && !gcopy(io.arg.as_mut_ptr().cast(), arg as usize as *const c_void, sz as usize)
                        {
                            break 'sw lerr(EFAULT);
                        }
                        let mut res: HubResult = zeroed();
                        let mut r = roundtrip(
                            tc,
                            BH_IOCTL,
                            (&io as *const BhIoctl).cast(),
                            size_of::<BhIoctl>(),
                            ptr::null(),
                            0,
                            &mut res,
                        );
                        if r < 0 {
                            break 'sw r;
                        }
                        r = res.r.ret as c_long;
                        if r == 0
                            && (dir & 2) != 0
                            && !gcopy(arg as usize as *mut c_void, res.r.arg.as_ptr().cast(), sz as usize)
                        {
                            r = lerr(EFAULT);
                        }
                        result_done(&mut res);
                        r
                    };
                }
                _ => {
                    // BINDER_FREEZE and the rest: what a kernel without them says.
                    ret = lerr(EINVAL);
                }
            }
        }
        (*tc).busy = false;
        // The hub releases the thread at channel end. Inside a signal handler
        // that interrupted another binder call of this thread, the channel stays
        // until that call is over (tchan_any_busy).
        if req == BINDER_THREAD_EXIT && !tchan_any_busy() {
            tchan_drop(tc);
        }
        ret
    };
    file_put(f);
    ret
}

/// Is `fd` one of the runtime's own channels to the hub?
#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_owns(fd: c_int) -> bool {
    // The channels sit at 64 and above (hide_fd); every close() comes here.
    if fd < 64 || G_NOPENED.load(Ordering::SeqCst) == 0 {
        return false;
    }
    let mut mine = false;
    g_lock();
    let mut i = 0;
    while i < G_NCHAN_FDS && !mine {
        mine = *g_chan_fd(i) == fd;
        i += 1;
    }
    g_unlock();
    if mine {
        binder_diag!(
            c"close of descriptor %d refused: it is the runtime's channel to the binder hub",
            fd
        );
    }
    mine
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_note_stray_ioctl(fd: c_int, req: c_ulong) {
    if G_NOPENED.load(Ordering::SeqCst) == 0 || ((req >> 8) & 0xff) != b'b' as c_ulong {
        return;
    }
    let mut path = [0 as c_char; PATH_MAX];
    let fl = fcntl(fd, F_GETFD);
    if fl >= 0 {
        fcntl(fd, F_GETPATH, path.as_mut_ptr());
    }
    binder_diag!(
        c"binder ioctl 0x%lx on descriptor %d, which is not a binder file: %s%s",
        req,
        fd,
        if fl < 0 { cs(c"closed") } else { cs(c"open") },
        if *path.as_ptr() != 0 { path.as_ptr() } else { cs(c"") }
    );
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_polled(fd: c_int) {
    let f = file_get(fd);
    if f.is_null() {
        return;
    }
    if !(*f).forked && !(*f).closed {
        let mut err: c_long = 0;
        let tc = tchan_get(f, &mut err);
        if !tc.is_null() && !(*tc).polled && !(*tc).busy {
            (*tc).busy = true;
            let mut res: HubResult = zeroed();
            if roundtrip(tc, BH_POLL, ptr::null(), 0, ptr::null(), 0, &mut res) == 0 {
                (*tc).polled = true;
                result_done(&mut res);
            }
            (*tc).busy = false;
        }
    }
    file_put(f);
}

// ------------------------------------------------------------ mmap

#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_mmap(
    addr: u64,
    len: u64,
    prot: c_int,
    lflags: c_int,
    fd: c_int,
    off: u64,
) -> c_long {
    let f = file_get(fd);
    if f.is_null() {
        return lerr(EBADF);
    }
    // The C body up to its `out:` label.
    let ret: c_long = 'out: {
        if (*f).forked || (*f).closed {
            break 'out lerr(EINVAL);
        }
        if prot & PROT_WRITE != 0 {
            break 'out lerr(EPERM); // FORBIDDEN_MMAP_FLAGS
        }
        if len == 0 {
            break 'out lerr(EINVAL);
        }
        // A slot in g_mapped and this file's one mapping, reserved under the lock:
        // two threads mapping at once both passed an unlocked check before, and
        // the seventeenth receive buffer was written past the end of g_mapped.
        g_lock();
        let busy = (*f).map_base != 0 || (*f).map_pending;
        let room = G_NMAPPED.load(Ordering::SeqCst) + G_MAP_PENDING < 16;
        if !busy && room {
            (*f).map_pending = true;
            G_MAP_PENDING += 1;
        }
        g_unlock();
        if busy {
            break 'out lerr(EBUSY);
        }
        if !room {
            break 'out lerr(ENOMEM); // more receive buffers than any process has
        }
        let _ = off;
        // The C body up to its `unreserve:` label; success leaves through 'out.
        let failed: c_long = 'unreserve: {
            let size: u64 = if len > SZ_4M { SZ_4M } else { len };
            // LXRT_ALIGN_UP(size, LXRT_HOST_PAGE)
            let hlen: u64 = (size + LXRT_HOST_PAGE - 1) & !(LXRT_HOST_PAGE - 1);
            let mut path = [0 as c_char; 256];
            // open checked it
            snprintf(path.as_mut_ptr(), 256, cs(c"%s/buf-%d-XXXXXX"), binder_dir(), getpid());
            let mut bfd = mkstemp(path.as_mut_ptr());
            if bfd < 0 {
                break 'unreserve lerr(errno());
            }
            unlink(path.as_ptr());
            fcntl(bfd, F_SETFD, FD_CLOEXEC);
            if ftruncate(bfd, hlen as i64) != 0 {
                let r = lerr(errno());
                close(bfd);
                break 'unreserve r;
            }
            let mut mflags = MAP_SHARED;
            if (lflags & 0x10) != 0 && addr != 0 {
                mflags |= MAP_FIXED; // Linux MAP_FIXED
            }
            let view = mmap(
                if mflags & MAP_FIXED != 0 { addr as usize as *mut c_void } else { ptr::null_mut() },
                hlen as usize,
                PROT_READ,
                mflags,
                bfd,
                0,
            );
            if view == MAP_FAILED {
                let r = lerr(errno());
                close(bfd);
                break 'unreserve r;
            }
            // binder_mmap clears VM_MAYWRITE: the guest can never make its receive
            // buffer writable (mprotect answers EACCES). The hub trusts what it wrote
            // there (offsets, objects) when the buffer is freed, as the kernel does.
            // The file is open read-write, so without this Darwin would let a
            // guest's mprotect add write.
            mach_vm_protect(task(), view as usize as u64, hlen, TRUE, VM_PROT_READ);
            let alias = mmap(ptr::null_mut(), hlen as usize, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
            if alias == MAP_FAILED {
                let r = lerr(errno());
                munmap(view, hlen as usize);
                close(bfd);
                break 'unreserve r;
            }
            // VM_DONTCOPY: a fork child has no binder buffer.
            minherit(view, hlen as usize, VM_INHERIT_NONE);
            minherit(alias, hlen as usize, VM_INHERIT_NONE);
            let mut err: c_long = 0;
            let tc = tchan_get(f, &mut err);
            if tc.is_null() || (*tc).busy {
                let r = if !tc.is_null() { lerr(EAGAIN) } else { err };
                munmap(view, hlen as usize);
                munmap(alias, hlen as usize);
                close(bfd);
                break 'unreserve r;
            }
            (*tc).busy = true;
            // The hub computes the addresses it hands this process from the buffer's
            // base: give it the guest's view of it (a 32-bit guest's is below 4 GiB).
            let mut ubase: u64 = view as usize as u64;
            let gb = lxrt_gbase();
            if gb != 0 && ubase >= gb && ubase - gb < (1u64 << 32) {
                ubase -= gb;
            }
            let mm = BhMmap { base: ubase, size };
            let mut res: HubResult = zeroed();
            let mut r = roundtrip(
                tc,
                BH_MMAP,
                (&mm as *const BhMmap).cast(),
                size_of::<BhMmap>(),
                &bfd,
                1,
                &mut res,
            );
            (*tc).busy = false;
            close(bfd);
            bfd = -1;
            let _ = bfd;
            if r == 0 {
                r = res.r.ret as c_long;
                result_done(&mut res);
            }
            if r < 0 {
                munmap(view, hlen as usize);
                munmap(alias, hlen as usize);
                break 'unreserve r;
            }
            g_lock();
            (*f).map_base = view as usize as u64;
            (*f).map_len = size;
            (*f).map_host_len = hlen;
            (*f).alias = alias.cast();
            (*f).refs += 1; // the mapping keeps the file (vm_file)
            let nm = G_NMAPPED.load(Ordering::SeqCst);
            *g_mapped(nm) = f; // the slot reserved above
            G_NMAPPED.store(nm + 1, Ordering::SeqCst);
            (*f).map_pending = false;
            G_MAP_PENDING -= 1;
            g_unlock();
            if lxrt_trace_on() {
                fprintf(
                    lxrt_trace_stream(),
                    cs(c"[lxrt] binder: receive buffer %llu bytes at %p\n"),
                    size as c_ulonglong,
                    view,
                );
            }
            break 'out view as usize as c_long;
        };
        g_lock();
        (*f).map_pending = false;
        G_MAP_PENDING -= 1;
        g_unlock();
        failed
    };
    file_put(f);
    ret
}

// The guest unmapping its receive buffer (ProcessState's destructor), or a
// range that covers some of it. Every receive buffer the range touches is
// closed and unmapped here (binder_vma_close); the rest of the range is then
// unmapped by the caller's ordinary munmap, with its bookkeeping. This used
// to unmap the first buffer alone and report the whole call done, leaving
// whatever else the range covered mapped.
// Always false: the caller goes on with its munmap.
#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_munmap(addr: u64, len: u64, ret: *mut c_long) -> bool {
    let _ = ret;
    // Linux refuses these before looking at any mapping (do_munmap: EINVAL).
    if G_NMAPPED.load(Ordering::SeqCst) == 0
        || (addr & 4095) != 0
        || len == 0
        || addr.wrapping_add(len) < addr
    {
        return false;
    }
    loop {
        let mut hit: *mut Bfile = ptr::null_mut();
        g_lock();
        let nm = G_NMAPPED.load(Ordering::SeqCst);
        let mut i = 0;
        while i < nm {
            let f = *g_mapped(i);
            if addr < (*f).map_base.wrapping_add((*f).map_host_len)
                && (*f).map_base < addr.wrapping_add(len)
            {
                hit = f;
                *g_mapped(i) = *g_mapped(nm - 1);
                G_NMAPPED.store(nm - 1, Ordering::SeqCst);
                break;
            }
            i += 1;
        }
        g_unlock();
        if hit.is_null() {
            return false;
        }
        unmap_one(hit);
    }
}

unsafe fn unmap_one(hit: *mut Bfile) {
    // binder_vma_close: from here on the hub allocates nothing more in this
    // proc's buffer (a transaction to it gets BR_DEAD_REPLY, as on Linux),
    // so nothing can be handed to the guest at an address it gave up.
    if !(*hit).forked && !(*hit).closed {
        let mut err: c_long = 0;
        let tc = tchan_get(hit, &mut err);
        if !tc.is_null() && !(*tc).busy {
            (*tc).busy = true;
            let mut io: BhIoctl = zeroed();
            io.cmd = BH_IOCTL_UNMAPPED;
            let mut res: HubResult = zeroed();
            if roundtrip(
                tc,
                BH_IOCTL,
                (&io as *const BhIoctl).cast(),
                size_of::<BhIoctl>(),
                ptr::null(),
                0,
                &mut res,
            ) == 0
            {
                result_done(&mut res);
            }
            (*tc).busy = false;
        }
    }
    // Both views go whole (the guest's is rounded up to host pages, which the
    // caller's munmap of the guest's length would not all reach); fd fixups
    // write through the alias under map_lock.
    pthread_mutex_lock(ptr::addr_of_mut!((*hit).map_lock));
    munmap((*hit).map_base as usize as *mut c_void, (*hit).map_host_len as usize);
    munmap((*hit).alias.cast(), (*hit).map_host_len as usize);
    g_lock();
    (*hit).map_base = 0;
    (*hit).alias = ptr::null_mut();
    let release = (*hit).naliases == 0;
    g_unlock();
    pthread_mutex_unlock(ptr::addr_of_mut!((*hit).map_lock));
    if release {
        file_release_proc(hit);
    }
    file_put(hit);
}

// ------------------------------------------------------------ descriptors

#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_close(fd: c_int) {
    // Every close() of the guest comes here: stay off the lock unless this
    // process ever had a binder descriptor.
    if fd < 0 || fd >= B_FDS as c_int || G_NOPENED.load(Ordering::SeqCst) == 0 {
        return;
    }
    g_lock();
    let f = *g_file(fd);
    *g_file(fd) = ptr::null_mut();
    let last = !f.is_null() && {
        (*f).naliases -= 1;
        (*f).naliases == 0
    };
    if last {
        (*f).closed = true;
    }
    g_unlock();
    if f.is_null() {
        return;
    }
    binder_diag!(
        c"binder descriptor %d closed%s",
        fd,
        if last { cs(c" (its last alias)") } else { cs(c"") }
    );
    if last && (*f).map_base == 0 {
        file_release_proc(f);
    }
    file_put(f);
}

// binder_release: no descriptor and no mapping names the proc any more. Our
// own copy of the process channel goes, and the hub releases the proc (its
// threads' channels end with it; ours are dropped here or when their
// threads next look).
unsafe fn file_release_proc(f: *mut Bfile) {
    if (*f).forked {
        return;
    }
    // A close() in a signal handler that interrupted a binder call of this
    // thread: nothing may move (tchan_any_busy); tchan_get drops these later.
    let chans = t_chans();
    let mut i: c_int = 0;
    while !tchan_any_busy() && i < t_nchans() {
        if (*chans.add(i as usize)).f == f {
            tchan_drop(chans.add(i as usize));
        } else {
            i += 1;
        }
    }
    pthread_mutex_lock(ptr::addr_of_mut!((*f).lock));
    if (*f).chan >= 0 {
        chan_unregister((*f).chan);
        shutdown((*f).chan, SHUT_RDWR);
        close((*f).chan);
        (*f).chan = -1;
    }
    pthread_mutex_unlock(ptr::addr_of_mut!((*f).lock));
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_dup(oldfd: c_int, newfd: c_int) {
    if oldfd < 0
        || oldfd >= B_FDS as c_int
        || newfd < 0
        || newfd >= B_FDS as c_int
        || oldfd == newfd
        || G_NOPENED.load(Ordering::SeqCst) == 0
    {
        return;
    }
    g_lock();
    let f = *g_file(oldfd);
    if !f.is_null() && (*g_file(newfd)).is_null() {
        *g_file(newfd) = f;
        (*f).naliases += 1;
        (*f).refs += 1;
    }
    g_unlock();
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_binder_thread_exit() {
    while t_nchans() > 0 {
        tchan_drop(t_chans().add(t_nchans() as usize - 1));
    }
}

// fork(): the child is a new process. Its binder descriptors still exist (the
// guest may close them) but name its parent's procs, and its receive buffers
// are gone (VM_INHERIT_NONE); every call on them is refused, as Linux refuses
// an mmap or use of a binder_proc from another process. The parent's thread
// channels are closed here: their threads do not exist in the child.
extern "C" fn binder_after_fork_child() {
    unsafe {
        if G_NOPENED.load(Ordering::SeqCst) == 0 {
            return;
        }
        G_NMAPPED.store(0, Ordering::SeqCst);
        G_MAP_PENDING = 0;
        let mut i: c_int = 0;
        while i < G_NCHAN_FDS {
            close(*g_chan_fd(i));
            i += 1;
        }
        G_NCHAN_FDS = 0;
        set_t_nchans(0);
        memset(t_chans().cast(), 0, size_of::<[Tchan; MAX_TCHAN]>());
        i = 0;
        while i < B_FDS as c_int {
            let f = *g_file(i);
            i += 1;
            if f.is_null() || (*f).forked {
                continue;
            }
            (*f).forked = true;
            (*f).map_pending = false;
            pthread_mutex_init(ptr::addr_of_mut!((*f).lock), ptr::null());
            pthread_mutex_init(ptr::addr_of_mut!((*f).map_lock), ptr::null());
            (*f).chan = -1; // closed above with the other channels
            (*f).map_base = 0;
            (*f).alias = ptr::null_mut();
        }
    }
}

// The C had two constructors, priority 200 (LXRT_FORK_SAFE) then 201
// (binder_fork_register): one initializer here registers both in that order.
extern "C" fn binder_fork_register() {
    unsafe {
        pthread_atfork(
            Some(binder_g_lock_fork_prepare),
            Some(binder_g_lock_fork_release),
            Some(binder_g_lock_fork_release),
        );
        pthread_atfork(None, None, Some(binder_after_fork_child));
    }
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static BINDER_FORK_REGISTER_INIT: extern "C" fn() = binder_fork_register;
