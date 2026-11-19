// Sockets.
//
// Three incompatibilities, in increasing order of how quietly they bite:
//
//   1. struct sockaddr has a different shape. Linux starts with a 16-bit
//      family; Darwin starts with a 1-byte length and a 1-byte family. A
//      pass-through makes AF_UNIX (1) look like sun_len = 1, and the connect
//      fails with something unrelated to the real problem.
//   2. sun_path is 108 bytes on Linux and 104 on Darwin.
//   3. Linux ORs SOCK_CLOEXEC and SOCK_NONBLOCK into the socket *type*. Darwin
//      has no such thing and rejects the type outright.
//
// A direct translation of the C: the same libSystem calls (snprintf, sscanf,
// getenv, malloc only where the C had it), the same statics, no std locks and
// no allocation of its own. Guest structures are read and written unaligned.

#![allow(clippy::missing_safety_doc)]

use std::cell::{Cell, UnsafeCell};
use std::ffi::{c_char, c_int, c_long, c_uint, c_void};
use std::mem::{size_of, MaybeUninit};
use std::ptr;
use std::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, AtomicU8, Ordering};

// Darwin errno values (host side; converted with lxrt_errno_to_linux).
const EFAULT: c_int = 14;
const EEXIST: c_int = 17;
const EINVAL: c_int = 22;
const EMFILE: c_int = 24;
const ENOPROTOOPT: c_int = 42;
const EAFNOSUPPORT: c_int = 47;
const ECONNRESET: c_int = 54;
const ENOBUFS: c_int = 55;
const ENAMETOOLONG: c_int = 63;

// Darwin constants (macOS SDK, arm64).
const AF_UNIX: c_int = 1;
const AF_INET: c_int = 2;
const AF_INET6: c_int = 30;
const SOCK_STREAM: c_int = 1;
const SOCK_DGRAM: c_int = 2;
const SOL_SOCKET: c_int = 0xffff;
const SOL_LOCAL: c_int = 0;
const LOCAL_PEERCRED: c_int = 1;
const LOCAL_PEERPID: c_int = 2;
const SCM_RIGHTS: c_int = 1;
const F_SETFD: c_int = 2;
const F_GETFL: c_int = 3;
const F_SETFL: c_int = 4;
const FD_CLOEXEC: c_int = 1;
const O_RDONLY: c_int = 0;
const O_WRONLY: c_int = 1;
const O_RDWR: c_int = 2;
const O_NONBLOCK: c_int = 4;
const O_CREAT: c_int = 0x200;
const O_TRUNC: c_int = 0x400;
const O_CLOEXEC: c_int = 0x100_0000;
const POLLIN: i16 = 1;
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const MAP_SHARED: c_int = 1;
const MAP_FAILED: *mut c_void = usize::MAX as *mut c_void;
const PROC_PIDLISTFDS: c_int = 1;
const PROC_PIDFDSOCKETINFO: c_int = 3;
const PROX_FDTYPE_SOCKET: u32 = 2;
const INET6_ADDRSTRLEN: usize = 46;
const S_IFMT: u16 = 0o170000;
const S_IFREG: u16 = 0o100000;
const S_IFIFO: u16 = 0o010000;

const SO_DEBUG: c_int = 0x0001;
const SO_ACCEPTCONN: c_int = 0x0002;
const SO_REUSEADDR: c_int = 0x0004;
const SO_KEEPALIVE: c_int = 0x0008;
const SO_DONTROUTE: c_int = 0x0010;
const SO_BROADCAST: c_int = 0x0020;
const SO_LINGER: c_int = 0x0080;
const SO_OOBINLINE: c_int = 0x0100;
const SO_REUSEPORT: c_int = 0x0200;
const SO_SNDBUF: c_int = 0x1001;
const SO_RCVBUF: c_int = 0x1002;
const SO_SNDLOWAT: c_int = 0x1003;
const SO_RCVLOWAT: c_int = 0x1004;
const SO_SNDTIMEO: c_int = 0x1005;
const SO_RCVTIMEO: c_int = 0x1006;
const SO_ERROR: c_int = 0x1007;
const SO_TYPE: c_int = 0x1008;

const IP_OPTIONS: c_int = 1;
const IP_HDRINCL: c_int = 2;
const IP_TOS: c_int = 3;
const IP_TTL: c_int = 4;
const IP_MULTICAST_IF: c_int = 9;
const IP_MULTICAST_TTL: c_int = 10;
const IP_MULTICAST_LOOP: c_int = 11;
const IP_ADD_MEMBERSHIP: c_int = 12;
const IP_DROP_MEMBERSHIP: c_int = 13;
const IP_RECVTTL: c_int = 24;
const IP_PKTINFO: c_int = 26;
const IP_RECVTOS: c_int = 27;

const IPV6_UNICAST_HOPS: c_int = 4;
const IPV6_MULTICAST_IF: c_int = 9;
const IPV6_MULTICAST_HOPS: c_int = 10;
const IPV6_MULTICAST_LOOP: c_int = 11;
const IPV6_JOIN_GROUP: c_int = 12;
const IPV6_LEAVE_GROUP: c_int = 13;
const IPV6_V6ONLY: c_int = 27;
const IPV6_TCLASS: c_int = 36;

const TCP_NODELAY: c_int = 0x01;
const TCP_MAXSEG: c_int = 0x02;
const TCP_KEEPALIVE: c_int = 0x10;
const TCP_KEEPINTVL: c_int = 0x101;
const TCP_KEEPCNT: c_int = 0x102;
const TCP_FASTOPEN: c_int = 0x105;

const L_SOCK_NONBLOCK: c_int = 0x0800;
const L_SOCK_CLOEXEC: c_int = 0x80000;

const L_AF_UNIX: c_int = 1;
const L_AF_INET: c_int = 2;
const L_AF_INET6: c_int = 10;

const L_SUN_PATH: usize = 108; // sizeof linux_sockaddr_un.sun_path
const D_SUN_PATH: usize = 104; // sizeof sockaddr_un.sun_path
const SUN_PATH_OFF: usize = 2; // offsetof(sockaddr_un, sun_path), both systems

#[repr(C)]
struct LinuxSockaddr {
    sa_family: u16,
    sa_data: [u8; 126],
}
#[repr(C)]
struct LinuxSockaddrUn {
    sun_family: u16,
    sun_path: [u8; L_SUN_PATH],
}
#[repr(C)]
struct LinuxSockaddrIn {
    sin_family: u16,
    sin_port: u16,
    sin_addr: u32,
    pad: [u8; 8],
}
#[repr(C)]
struct LinuxSockaddrIn6 {
    sin6_family: u16,
    sin6_port: u16,
    sin6_flowinfo: u32,
    sin6_addr: [u8; 16],
    sin6_scope_id: u32,
}

const _: [(); 128] = [(); size_of::<LinuxSockaddr>()];
const _: [(); 110] = [(); size_of::<LinuxSockaddrUn>()];
const _: [(); 16] = [(); size_of::<LinuxSockaddrIn>()];
const _: [(); 28] = [(); size_of::<LinuxSockaddrIn6>()];

// ---- Darwin structures ----

#[repr(C)]
struct SockaddrStorage {
    ss_len: u8,
    ss_family: u8,
    ss_pad1: [u8; 6],
    ss_align: i64,
    ss_pad2: [u8; 112],
}
#[repr(C)]
struct SockaddrUn {
    sun_len: u8,
    sun_family: u8,
    sun_path: [c_char; D_SUN_PATH],
}
#[repr(C)]
struct SockaddrIn {
    sin_len: u8,
    sin_family: u8,
    sin_port: u16,
    sin_addr: u32,
    sin_zero: [u8; 8],
}
#[repr(C)]
struct SockaddrIn6 {
    sin6_len: u8,
    sin6_family: u8,
    sin6_port: u16,
    sin6_flowinfo: u32,
    sin6_addr: [u8; 16],
    sin6_scope_id: u32,
}
#[repr(C)]
struct Linger {
    l_onoff: c_int,
    l_linger: c_int,
}
#[repr(C)]
struct PollFd {
    fd: c_int,
    events: i16,
    revents: i16,
}
#[repr(C)]
struct Stat {
    st_dev: i32,
    st_mode: u16,
    st_nlink: u16,
    st_ino: u64,
    rest: [u8; 128],
}
#[repr(C)]
pub struct Iovec {
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
struct Xucred {
    cr_version: u32,
    cr_uid: u32,
    cr_ngroups: i16,
    cr_groups: [u32; 16],
}
#[repr(C)]
struct Timeval {
    tv_sec: i64,
    tv_usec: i32,
}
// struct socket_fdinfo (sys/proc_info.h), 792 bytes; only three members are
// read: psi.soi_so (160), psi.soi_family (184) and
// psi.soi_proto.pri_un.unsi_conn_so (264).
#[repr(C)]
struct SocketFdInfo {
    words: [u64; 99],
}
const SI_SOI_SO: usize = 160;
const SI_SOI_FAMILY: usize = 184;
const SI_UNSI_CONN_SO: usize = 264;
#[repr(C)]
struct ProcFdInfo {
    proc_fd: i32,
    proc_fdtype: u32,
}
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}
const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;

const _: [(); 128] = [(); size_of::<SockaddrStorage>()];
const _: [(); 106] = [(); size_of::<SockaddrUn>()];
const _: [(); 16] = [(); size_of::<SockaddrIn>()];
const _: [(); 28] = [(); size_of::<SockaddrIn6>()];
const _: [(); 144] = [(); size_of::<Stat>()];
const _: [(); 48] = [(); size_of::<MsgHdr>()];
const _: [(); 12] = [(); size_of::<CmsgHdr>()];
const _: [(); 76] = [(); size_of::<Xucred>()];
const _: [(); 16] = [(); size_of::<Timeval>()];
const _: [(); 792] = [(); size_of::<SocketFdInfo>()];
const _: [(); 8] = [(); size_of::<ProcFdInfo>()];
const _: [(); 64] = [(); size_of::<PthreadMutex>()];

// runtime/android_ids.h
#[repr(C)]
struct LxrtAidsPeer {
    ruid: u32,
    euid: u32,
    rgid: u32,
    egid: u32,
    eff: u64,
    prm: u64,
    inh: u64,
}

#[link(name = "System")]
unsafe extern "C" {
    fn __error() -> *mut c_int;
    fn snprintf(buffer: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn sscanf(input: *const c_char, format: *const c_char, ...) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn strerror(error: c_int) -> *const c_char;
    fn strlen(s: *const c_char) -> usize;
    fn strnlen(s: *const c_char, max: usize) -> usize;
    fn strncmp(a: *const c_char, b: *const c_char, n: usize) -> c_int;
    fn getenv(name: *const c_char) -> *const c_char;
    fn malloc(size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn getuid() -> u32;
    fn getgid() -> u32;
    fn getpid() -> c_int;
    fn kill(pid: c_int, signal: c_int) -> c_int;
    fn mkdir(path: *const c_char, mode: u16) -> c_int;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn read(fd: c_int, buffer: *mut c_void, count: usize) -> isize;
    fn write(fd: c_int, buffer: *const c_void, count: usize) -> isize;
    fn readv(fd: c_int, iov: *const Iovec, count: c_int) -> isize;
    fn dup(fd: c_int) -> c_int;
    fn unlink(path: *const c_char) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn fstat(fd: c_int, st: *mut Stat) -> c_int;
    fn stat(path: *const c_char, st: *mut Stat) -> c_int;
    fn ftruncate(fd: c_int, length: i64) -> c_int;
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn poll(fds: *mut PollFd, count: c_uint, timeout: c_int) -> c_int;
    fn socket(domain: c_int, kind: c_int, protocol: c_int) -> c_int;
    fn socketpair(domain: c_int, kind: c_int, protocol: c_int, sv: *mut c_int) -> c_int;
    fn connect(fd: c_int, address: *const c_void, length: u32) -> c_int;
    fn bind(fd: c_int, address: *const c_void, length: u32) -> c_int;
    fn accept(fd: c_int, address: *mut c_void, length: *mut u32) -> c_int;
    fn getsockname(fd: c_int, address: *mut c_void, length: *mut u32) -> c_int;
    fn getpeername(fd: c_int, address: *mut c_void, length: *mut u32) -> c_int;
    fn getsockopt(
        fd: c_int,
        level: c_int,
        option: c_int,
        value: *mut c_void,
        length: *mut u32,
    ) -> c_int;
    fn setsockopt(
        fd: c_int,
        level: c_int,
        option: c_int,
        value: *const c_void,
        length: u32,
    ) -> c_int;
    fn send(fd: c_int, buffer: *const c_void, length: usize, flags: c_int) -> isize;
    fn sendto(
        fd: c_int,
        buffer: *const c_void,
        length: usize,
        flags: c_int,
        address: *const c_void,
        address_length: u32,
    ) -> isize;
    fn sendmsg(fd: c_int, message: *const MsgHdr, flags: c_int) -> isize;
    fn recvfrom(
        fd: c_int,
        buffer: *mut c_void,
        length: usize,
        flags: c_int,
        address: *mut c_void,
        address_length: *mut u32,
    ) -> isize;
    fn recvmsg(fd: c_int, message: *mut MsgHdr, flags: c_int) -> isize;
    fn inet_ntop(
        family: c_int,
        source: *const c_void,
        destination: *mut c_char,
        size: u32,
    ) -> *const c_char;
    fn proc_pidinfo(
        pid: c_int,
        flavor: c_int,
        arg: u64,
        buffer: *mut c_void,
        size: c_int,
    ) -> c_int;
    fn proc_pidfdinfo(
        pid: c_int,
        fd: c_int,
        flavor: c_int,
        buffer: *mut c_void,
        size: c_int,
    ) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
}

unsafe extern "C" {
    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
    fn lxrt_host_tmpdir() -> *const c_char;
    fn lxrt_open_private(path: *const c_char, flags: c_int, mode: c_int) -> c_int;
    fn lxrt_eventfd_is(fd: c_int) -> bool;
    fn lxrt_eventfd_companions(fd: c_int, shm_fd: *mut c_int, wfd: *mut c_int) -> c_int;
    fn lxrt_eventfd_adopt(guest_fd: c_int, shm_fd: c_int, wfd: c_int) -> bool;
    fn lxrt_rtnl_request(peer: c_int, buffer: *const c_void, length: usize);
    fn lxrt_mounts_active() -> bool;
    fn lxrt_mounts_untranslate(
        host: *const c_char,
        buffer: *mut c_char,
        capacity: usize,
    ) -> *const c_char;
    fn lxrt_props_is_socket(guest_path: *const c_char) -> bool;
    fn lxrt_props_start_service() -> bool;
    fn lxrt_ids_on() -> bool;
    fn lxrt_ids_pid() -> c_int;
    fn lxrt_ids_to_guest(host_pid: c_int, index: c_int) -> c_int;
    fn lxrt_aids_lookup(pid: c_int, out: *mut LxrtAidsPeer) -> bool;
    // The Rust path layer applies pressure-vessel mounts and aliases paths
    // that exceed Darwin's sockaddr_un.sun_path limit.
    fn lxrt_socket_path_for_darwin(
        guest_path: *const c_char,
        output: *mut c_char,
        capacity: usize,
    ) -> c_int;
    fn lxrt_socket_path_unalias(
        host_socket_path: *const c_char,
        output: *mut c_char,
        capacity: usize,
    ) -> bool;
}

#[inline]
unsafe fn errno() -> c_int {
    *__error()
}

#[inline]
unsafe fn set_errno(value: c_int) {
    *__error() = value;
}

// LERR(e): -lxrt_errno_to_linux(e)
#[inline]
unsafe fn lerr(error: c_int) -> c_long {
    (-lxrt_errno_to_linux(error)) as c_long
}

// A C static: plain memory, no lock, exactly as racy as the original.
struct Racy<T>(UnsafeCell<T>);
unsafe impl<T> Sync for Racy<T> {}
impl<T> Racy<T> {
    const fn new(value: T) -> Self {
        Self(UnsafeCell::new(value))
    }
    #[inline]
    fn get(&self) -> *mut T {
        self.0.get()
    }
}

// The last abstract name too long to materialise as itself (addr_to_darwin),
// for lxrt_bind to record beside its hash. Per thread: bind translates and
// binds on the calling thread.
thread_local! {
    static G_ABSTRACT_LONG: UnsafeCell<[u8; L_SUN_PATH]> =
        const { UnsafeCell::new([0; L_SUN_PATH]) };
    static G_ABSTRACT_LONG_LEN: Cell<usize> = const { Cell::new(0) };
}

static ABSTRACT_DIR: Racy<[c_char; 256]> = Racy::new([0; 256]);
static ABSTRACT_DIR_READY: Racy<bool> = Racy::new(false);
static ABSTRACT_DIR_TRIED: Racy<bool> = Racy::new(false);

// Where Linux abstract socket names are materialised. Created once, mode 0700.
unsafe fn abstract_dir() -> *const c_char {
    let dir = ABSTRACT_DIR.get().cast::<c_char>();
    if *ABSTRACT_DIR_TRIED.get() {
        return if *ABSTRACT_DIR_READY.get() {
            dir.cast_const()
        } else {
            ptr::null()
        };
    }
    *ABSTRACT_DIR_TRIED.get() = true;
    // The host temporary directory of this process tree (lxrt_host_tmpdir),
    // not the guest's TMPDIR: Termux points TMPDIR into its own prefix, the
    // directory could not be made there, and FEX could not reach its server
    // ("Couldn't connect to FEXServer socket ... 22", MEASURED). Nor one
    // directory for every tree: the trees are separate namespaces, as network
    // namespaces are on Linux -- with all of them in /tmp, the Android root's
    // FEXServer and Steam's met under FEX's default name (501.FEXServer.Socket),
    // and Steam's FEX ran against the Android root: no Steam window (MEASURED).
    let _ = snprintf(
        dir,
        256,
        c"%s/lxrt-abstract-%u".as_ptr(),
        lxrt_host_tmpdir(),
        getuid() as c_uint,
    );
    if mkdir(dir, 0o700) != 0 && errno() != EEXIST {
        return ptr::null();
    }
    *ABSTRACT_DIR_READY.get() = true;
    dir.cast_const()
}

// Darwin's AF_* happen to match Linux's for UNIX and INET; INET6 does not
// (10 on Linux, 30 on Darwin), so the mapping is explicit.
fn family_to_darwin(lf: c_int) -> c_int {
    match lf {
        L_AF_UNIX => AF_UNIX,
        L_AF_INET => AF_INET,
        L_AF_INET6 => AF_INET6,
        _ => -1,
    }
}

fn family_to_linux(df: c_int) -> c_int {
    match df {
        AF_UNIX => L_AF_UNIX,
        AF_INET => L_AF_INET,
        AF_INET6 => L_AF_INET6,
        _ => 0,
    }
}

// Returns the Darwin length, or -1. `out` must be at least sizeof(sockaddr_un).
unsafe fn addr_to_darwin(lsa: *const c_void, llen: c_uint, out: *mut SockaddrStorage) -> c_int {
    if lsa.is_null() || llen < 2 {
        return -1;
    }
    let g = lsa.cast::<u8>();
    ptr::write_bytes(out, 0, 1);

    match c_int::from(g.cast::<u16>().read_unaligned()) {
        L_AF_UNIX => {
            let lu_path = g.add(SUN_PATH_OFF); // lu->sun_path
            let du = out.cast::<SockaddrUn>();
            let du_path = out.cast::<c_char>().add(SUN_PATH_OFF); // du->sun_path
            (*du).sun_family = AF_UNIX as u8;
            let mut pathmax: usize = if llen > 2 { (llen - 2) as usize } else { 0 };
            if pathmax > L_SUN_PATH {
                pathmax = L_SUN_PATH;
            }
            // Abstract sockets -- a leading NUL -- exist only on Linux; Darwin has
            // no filesystem-free socket namespace. They are mapped into a directory
            // the runtime owns instead of refused, because both ends of an abstract
            // socket are guest code and both go through this same translation, so
            // they agree. What is lost is the auto-cleanup on last close, and
            // isolation from anything else using that directory.
            if pathmax != 0 && *lu_path == 0 {
                let dir = abstract_dir();
                if dir.is_null() {
                    return -1;
                }
                // The name is everything after the leading NUL, llen long (Linux
                // abstract names are not NUL-terminated and may hold NULs and
                // '/'). Encoded reversibly -- '/' as %2F, '%' as %25, NUL as %00
                // -- so getsockname can give the guest its abstract address back
                // (addr_to_linux): Android's zygote checks a child zygote's socket
                // by that name ("Socket name not whitelisted : /tmp/lxrt-abstract-
                // 501/com.android.internal.os.WebViewZygoteInit_...", MEASURED).
                // Trailing NULs are dropped: a program that binds with
                // sizeof(struct sockaddr_un) pads its name with them.
                let mut n = pathmax - 1;
                while n != 0 && *lu_path.add(n) == 0 {
                    n -= 1;
                }
                const NAME: usize = 330;
                let mut name_buf = MaybeUninit::<[c_char; NAME]>::uninit();
                let name = name_buf.as_mut_ptr().cast::<c_char>();
                let mut k: usize = 0;
                let mut i: usize = 0;
                while i < n && k + 4 < NAME {
                    let c = *lu_path.add(1 + i);
                    if c == b'/' || c == b'%' || c == 0 {
                        k = k.wrapping_add(snprintf(
                            name.add(k),
                            NAME - k,
                            c"%%%02X".as_ptr(),
                            c_uint::from(c),
                        ) as usize);
                    } else {
                        *name.add(k) = c as c_char;
                        k += 1;
                    }
                    i += 1;
                }
                *name.add(k) = 0;
                (*du).sun_family = AF_UNIX as u8;
                let mut w = snprintf(du_path, D_SUN_PATH, c"%s/%s".as_ptr(), dir, name.cast_const());
                if w < 0 || w as usize >= D_SUN_PATH {
                    // Too long for Darwin's 104 bytes: a hash names the socket,
                    // and <hash>.name beside it keeps the name for getsockname
                    // (written at bind, abstract_note_long_name).
                    let mut h: u64 = 1469598103934665603;
                    let mut i: usize = 0;
                    while i < n {
                        h ^= u64::from(*lu_path.add(1 + i));
                        h = h.wrapping_mul(1099511628211);
                        i += 1;
                    }
                    w = snprintf(du_path, D_SUN_PATH, c"%s/~%016llx".as_ptr(), dir, h);
                    if w < 0 || w as usize >= D_SUN_PATH {
                        return -1;
                    }
                    G_ABSTRACT_LONG_LEN.with(|len| len.set(n));
                    G_ABSTRACT_LONG.with(|buffer| {
                        ptr::copy_nonoverlapping(lu_path.add(1), buffer.get().cast::<u8>(), n);
                    });
                }
                (*du).sun_len = size_of::<SockaddrUn>() as u8;
                return size_of::<SockaddrUn>() as c_int;
            }
            let n = strnlen(lu_path.cast::<c_char>(), pathmax);
            let mut guest = [0 as c_char; L_SUN_PATH + 1];
            let mut socket_path = [0 as c_char; D_SUN_PATH];
            ptr::copy_nonoverlapping(lu_path.cast::<c_char>(), guest.as_mut_ptr(), n);
            *guest.as_mut_ptr().add(n) = 0;
            let error =
                lxrt_socket_path_for_darwin(guest.as_ptr(), socket_path.as_mut_ptr(), D_SUN_PATH);
            if error != 0 {
                set_errno(error);
                return -1;
            }
            let socket_path_len = strnlen(socket_path.as_ptr(), D_SUN_PATH);
            if socket_path_len >= D_SUN_PATH {
                set_errno(ENAMETOOLONG);
                return -1;
            }
            ptr::copy_nonoverlapping(socket_path.as_ptr(), du_path, socket_path_len + 1);
            (*du).sun_len = size_of::<SockaddrUn>() as u8;
            size_of::<SockaddrUn>() as c_int
        }
        L_AF_INET => {
            let li = lsa.cast::<LinuxSockaddrIn>();
            let di = out.cast::<SockaddrIn>();
            (*di).sin_len = size_of::<SockaddrIn>() as u8;
            (*di).sin_family = AF_INET as u8;
            // both already network order
            (*di).sin_port = ptr::addr_of!((*li).sin_port).read_unaligned();
            (*di).sin_addr = ptr::addr_of!((*li).sin_addr).read_unaligned();
            size_of::<SockaddrIn>() as c_int
        }
        L_AF_INET6 => {
            let l6 = lsa.cast::<LinuxSockaddrIn6>();
            let d6 = out.cast::<SockaddrIn6>();
            (*d6).sin6_len = size_of::<SockaddrIn6>() as u8;
            (*d6).sin6_family = AF_INET6 as u8;
            (*d6).sin6_port = ptr::addr_of!((*l6).sin6_port).read_unaligned();
            (*d6).sin6_flowinfo = ptr::addr_of!((*l6).sin6_flowinfo).read_unaligned();
            ptr::copy_nonoverlapping(
                ptr::addr_of!((*l6).sin6_addr).cast::<u8>(),
                ptr::addr_of_mut!((*d6).sin6_addr).cast::<u8>(),
                16,
            );
            (*d6).sin6_scope_id = ptr::addr_of!((*l6).sin6_scope_id).read_unaligned();
            size_of::<SockaddrIn6>() as c_int
        }
        _ => -1,
    }
}

unsafe fn addr_to_linux(dsa: *const SockaddrStorage, dlen: u32, lsa: *mut c_void, llen: *mut u32) {
    if lsa.is_null() || llen.is_null() {
        return;
    }
    let cap = llen.read_unaligned();
    let mut out: LinuxSockaddr = std::mem::zeroed();
    let outp = ptr::addr_of_mut!(out).cast::<u8>();
    let mut used: u32;

    match c_int::from((*dsa).ss_family) {
        AF_UNIX => 'unix: {
            let du_path = dsa.cast::<c_char>().add(SUN_PATH_OFF); // du->sun_path
            let mut lu: LinuxSockaddrUn = std::mem::zeroed();
            let lup = ptr::addr_of_mut!(lu).cast::<u8>();
            let lu_path = lup.add(SUN_PATH_OFF); // lu.sun_path
            lu.sun_family = L_AF_UNIX as u16;
            // The name is what the kernel returned, dlen bytes: Darwin does not
            // NUL-terminate a path that was bound without its terminator (Python
            // and most host programs bind that way), and whatever follows in the
            // caller's buffer is not part of it. Reading on to the next NUL
            // handed the zygote "/dev/socket/zygote@A..." for a socket its init
            // bound, and it refused to fork (MEASURED, stage 27).
            let off = SUN_PATH_OFF;
            let mut lim: usize = if dlen as usize > off { dlen as usize - off } else { 0 };
            if lim > D_SUN_PATH {
                lim = D_SUN_PATH;
            }
            let mut n = strnlen(du_path, lim);
            let mut src = du_path;
            let mut restored = [0 as c_char; L_SUN_PATH];
            if n != 0 && lxrt_socket_path_unalias(src, restored.as_mut_ptr(), L_SUN_PATH) {
                let restored_len = strnlen(restored.as_ptr(), L_SUN_PATH);
                // restored_len + 1 bytes, terminator included (the unalias
                // layer always leaves one inside its capacity).
                let copy = if restored_len < L_SUN_PATH {
                    restored_len + 1
                } else {
                    L_SUN_PATH
                };
                ptr::copy_nonoverlapping(restored.as_ptr().cast::<u8>(), lu_path, copy);
                used = (2 + restored_len + 1) as u32;
                ptr::copy_nonoverlapping(lup, outp, size_of::<LinuxSockaddrUn>());
                break 'unix;
            }
            // One of the runtime's materialised abstract names: the guest's
            // abstract address again (leading NUL, the decoded name, no
            // terminator; the length says where it ends).
            let adir = abstract_dir();
            let al: usize = if adir.is_null() { 0 } else { strlen(adir) };
            if al != 0
                && n > al + 1
                && strncmp(src, adir, al) == 0
                && *src.add(al) == b'/' as c_char
                && *src.add(al + 1) == b'~' as c_char
            {
                let mut side = [0 as c_char; 200];
                let mut nm = [0u8; L_SUN_PATH];
                let _ = snprintf(side.as_mut_ptr(), 200, c"%.*s.name".as_ptr(), n as c_int, src);
                let sf = open(side.as_ptr(), O_RDONLY | O_CLOEXEC);
                let got: isize = if sf >= 0 {
                    read(sf, nm.as_mut_ptr().cast::<c_void>(), L_SUN_PATH - 1)
                } else {
                    -1
                };
                if sf >= 0 {
                    close(sf);
                }
                if got > 0 {
                    *lu_path = 0;
                    ptr::copy_nonoverlapping(nm.as_ptr(), lu_path.add(1), got as usize);
                    used = (2 + 1 + got) as u32;
                    ptr::copy_nonoverlapping(lup, outp, size_of::<LinuxSockaddrUn>());
                    break 'unix;
                }
            }
            if al != 0
                && n > al + 1
                && strncmp(src, adir, al) == 0
                && *src.add(al) == b'/' as c_char
                && *src.add(al + 1) != b'~' as c_char
            {
                let mut k: usize = 1;
                let mut i = al + 1;
                while i < n && k < L_SUN_PATH {
                    let mut v: c_uint = 0;
                    if *src.add(i) == b'%' as c_char
                        && i + 2 < n
                        && sscanf(src.add(i + 1), c"%2X".as_ptr(), ptr::addr_of_mut!(v)) == 1
                    {
                        *lu_path.add(k) = v as u8;
                        k += 1;
                        i += 3;
                    } else {
                        *lu_path.add(k) = *src.add(i) as u8;
                        k += 1;
                        i += 1;
                    }
                }
                *lu_path = 0;
                used = (2 + k) as u32;
                ptr::copy_nonoverlapping(lup, outp, size_of::<LinuxSockaddrUn>());
                break 'unix;
            }
            let root = getenv(c"LXRT_ROOT".as_ptr());
            let rl: usize = if root.is_null() { 0 } else { strlen(root) };
            let mut hostp = [0 as c_char; D_SUN_PATH + 1];
            let mut gview = MaybeUninit::<[c_char; 1024]>::uninit();
            ptr::copy_nonoverlapping(src, hostp.as_mut_ptr(), n);
            *hostp.as_mut_ptr().add(n) = 0;
            let u: *const c_char = if n != 0 && *src == b'/' as c_char {
                lxrt_mounts_untranslate(hostp.as_ptr(), gview.as_mut_ptr().cast::<c_char>(), 1024)
            } else {
                ptr::null()
            };
            if !u.is_null() && strlen(u) < L_SUN_PATH {
                src = u; // back to the guest's own path (binds, root)
                n = strlen(u);
            } else if rl != 0
                && n > rl
                && strncmp(src, root, rl) == 0
                && *src.add(rl) == b'/' as c_char
            {
                src = src.add(rl);
                n -= rl;
            }
            ptr::copy_nonoverlapping(src.cast::<u8>(), lu_path, n);
            used = (2 + n + 1) as u32;
            ptr::copy_nonoverlapping(lup, outp, size_of::<LinuxSockaddrUn>());
        }
        AF_INET => {
            let di = dsa.cast::<SockaddrIn>();
            let mut li: LinuxSockaddrIn = std::mem::zeroed();
            li.sin_family = L_AF_INET as u16;
            li.sin_port = (*di).sin_port;
            li.sin_addr = (*di).sin_addr;
            used = size_of::<LinuxSockaddrIn>() as u32;
            ptr::copy_nonoverlapping(
                ptr::addr_of!(li).cast::<u8>(),
                outp,
                size_of::<LinuxSockaddrIn>(),
            );
        }
        AF_INET6 => {
            let d6 = dsa.cast::<SockaddrIn6>();
            let mut l6: LinuxSockaddrIn6 = std::mem::zeroed();
            l6.sin6_family = L_AF_INET6 as u16;
            l6.sin6_port = (*d6).sin6_port;
            l6.sin6_flowinfo = (*d6).sin6_flowinfo;
            l6.sin6_addr = (*d6).sin6_addr;
            l6.sin6_scope_id = (*d6).sin6_scope_id;
            used = size_of::<LinuxSockaddrIn6>() as u32;
            ptr::copy_nonoverlapping(
                ptr::addr_of!(l6).cast::<u8>(),
                outp,
                size_of::<LinuxSockaddrIn6>(),
            );
        }
        df => {
            out.sa_family = family_to_linux(df) as u16;
            used = dlen;
        }
    }

    if used > cap {
        used = cap;
    }
    ptr::copy_nonoverlapping(outp.cast_const(), lsa.cast::<u8>(), used as usize);
    // Linux reports the size the address WOULD have needed, not what fitted.
    llen.write_unaligned(used);
}

// Linux carries these in the type; Darwin needs them applied afterwards.
unsafe fn apply_type_flags(fd: c_int, ltype: c_int) {
    if fd < 0 {
        return;
    }
    if ltype & L_SOCK_NONBLOCK != 0 {
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    if ltype & L_SOCK_CLOEXEC != 0 {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
}

// Linux SOCK_SEQPACKET (5) on AF_UNIX has no Darwin counterpart: Darwin's
// unix sockets are STREAM or DGRAM. A DGRAM pair keeps message boundaries
// and carries SCM_RIGHTS, which is what Chromium's IPC (steamwebhelper) and
// crashpad want from a seqpacket pair; what it lacks is end-of-file when the
// peer closes -- a read then blocks instead of returning 0. Measured need:
// socketpair(AF_UNIX, SOCK_SEQPACKET) -> EPROTONOSUPPORT was the last
// syscall before CEF gave up (benchmarks/stage5-fex.txt).
fn type_to_darwin(ltype: c_int) -> c_int {
    let base = ltype & !(L_SOCK_NONBLOCK | L_SOCK_CLOEXEC);
    if base == 5 {
        // SOCK_SEQPACKET
        return SOCK_DGRAM;
    }
    base
}

// The end of file, then. A thread already blocked on a Darwin AF_UNIX
// datagram socket is not woken when the peer goes away: poll(), kevent() and
// recv() stay blocked (MEASURED on this Mac, 2026-09-29: a poll with a
// 2000 ms timeout returned 0 although the peer was closed 300 ms in, and a
// recv never returned). A poll made AFTER the peer is gone reports POLLIN at
// once, and the recv then fails with ECONNRESET. Chromium's zygotes wait for
// the browser that way (ppoll, then recvmsg, on their seqpacket socket) and
// outlived Heroic's browser process by minutes (MEASURED,
// benchmarks/stage24-heroic.txt). So a blocking recvmsg/recvfrom or ppoll on
// such a socket waits in slices of SEQPKT_SLICE_MS, and ECONNRESET on it
// reads as Linux's end of file, 0.
//
// Which sockets: those created as SOCK_SEQPACKET. They carry SO_LINGER
// {1, SEQPKT_LINGER} as their mark (a linger time means nothing to a
// datagram socket, and Darwin keeps and reports it: MEASURED), so the mark
// travels with the socket through fork, exec and SCM_RIGHTS, where a table
// in this process would not. A guest reading SO_LINGER sees it off.
const SEQPKT_LINGER: c_int = 0x5e9;
const SEQPKT_SLICE_MS: c_int = 250;

unsafe fn seqpkt_mark(fd: c_int, ltype: c_int) {
    if (ltype & !(L_SOCK_NONBLOCK | L_SOCK_CLOEXEC)) != 5 || fd < 0 {
        return;
    }
    let l = Linger {
        l_onoff: 1,
        l_linger: SEQPKT_LINGER,
    };
    setsockopt(
        fd,
        SOL_SOCKET,
        SO_LINGER,
        ptr::addr_of!(l).cast::<c_void>(),
        size_of::<Linger>() as u32,
    );
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_is_seqpacket(fd: c_int) -> bool {
    let mut l = MaybeUninit::<Linger>::uninit();
    let mut n = size_of::<Linger>() as u32;
    fd >= 0
        && getsockopt(
            fd,
            SOL_SOCKET,
            SO_LINGER,
            l.as_mut_ptr().cast::<c_void>(),
            &mut n,
        ) == 0
        && (*l.as_ptr()).l_onoff != 0
        && (*l.as_ptr()).l_linger == SEQPKT_LINGER
}

// Block until fd is readable, a slice at a time; 0, or -EINTR (a signal
// ended the wait, as it ends a blocking recvmsg without SA_RESTART).
unsafe fn seqpkt_wait(fd: c_int) -> c_long {
    loop {
        let mut p = PollFd {
            fd,
            events: POLLIN,
            revents: 0,
        };
        let r = poll(&mut p, 1, SEQPKT_SLICE_MS);
        if r > 0 {
            return 0;
        }
        if r < 0 {
            return lerr(errno());
        }
    }
}

// A blocking receive on a seqpacket socket: wait in slices first.
unsafe fn seqpkt_before_recv(fd: c_int, lflags: c_int) -> c_long {
    if (lflags & 0x40) != 0 /* MSG_DONTWAIT */
        || (fcntl(fd, F_GETFL) & O_NONBLOCK) != 0
        || !lxrt_is_seqpacket(fd)
    {
        return 0;
    }
    seqpkt_wait(fd)
}

// AF_NETLINK / NETLINK_KOBJECT_UEVENT: libudev's device monitor. Darwin has
// no netlink, and SDL3 (sdl2-compat in Steam's runtime) refuses to start its
// haptic subsystem -- and with it all controller support, so winebus saw no
// gamepads (MEASURED: "could not init SDL: Could not initialize UDEV") --
// when udev_monitor_new_from_netlink fails. The monitor is a datagram socket
// that stays silent: no uevents, since there is no kernel to send them
// (controller hotplug reaches SDL through inotify on /dev/input instead).
// Its peer end is kept so uevents could be injected later.
const L_AF_NETLINK: c_int = 16;
const L_NETLINK_KOBJECT_UEVENT: c_int = 15;
const L_NETLINK_ROUTE: c_int = 0;
const NL_FDS: usize = 65536;
static G_NL_PEER: [AtomicI32; NL_FDS] = [const { AtomicI32::new(0) }; NL_FDS]; // peer fd + 1, 0 = not netlink
static G_NL_ROUTE: [AtomicU8; NL_FDS] = [const { AtomicU8::new(0) }; NL_FDS]; // NETLINK_ROUTE: requests get answers (netif.c)
static G_NL_GROUPS: [AtomicU32; NL_FDS] = [const { AtomicU32::new(0) }; NL_FDS];
static G_NL_INO: [AtomicU64; NL_FDS] = [const { AtomicU64::new(0) }; NL_FDS];

// The table entry for a descriptor already known to be in 0..65536.
#[inline]
unsafe fn slot<T>(table: &'static [T; NL_FDS], fd: c_int) -> &'static T {
    &*table.as_ptr().add(fd as usize)
}

// A number can outlive its socket here: close_range, dup2 over it, exec of
// a CLOEXEC descriptor and a forked child's mass close never pass through
// lxrt_socket_close. Trusting the number alone turned the reused descriptor
// -- Chromium's IPC sockets -- into a fake netlink socket whose bind and
// getsockname lied, and Steam's web helper hung before its first window
// (MEASURED). The entry counts only while it names the same socket.
unsafe fn is_netlink(fd: c_int) -> bool {
    if !(0..NL_FDS as c_int).contains(&fd) || slot(&G_NL_PEER, fd).load(Ordering::SeqCst) == 0 {
        return false;
    }
    let mut st = MaybeUninit::<Stat>::uninit();
    if fstat(fd, st.as_mut_ptr()) == 0
        && (*st.as_ptr()).st_ino == slot(&G_NL_INO, fd).load(Ordering::SeqCst)
    {
        return true;
    }
    slot(&G_NL_PEER, fd).store(0, Ordering::SeqCst); // stale: forget it (the peer leaks, once)
    false
}

// Darwin caps an AF_UNIX datagram at the socket's send buffer, 2048 bytes by
// default (net.local.dgram.maxdgram), and queues 4096 bytes on the receiving
// side (net.local.dgram.recvspace); Linux takes datagrams up to its default
// buffer, 212992 bytes (net.core.wmem_default). Android's liblog sends each
// record, up to 4068 bytes, as one datagram to logd: every longer one --
// ART's abort message with its stack -- failed with EMSGSIZE and was lost
// (MEASURED, benchmarks/stage25-art-x86-fex.txt). Linux's sizes, then.
unsafe fn unix_dgram_buffers(fd: c_int, d: c_int, ltype: c_int) {
    if d != AF_UNIX || type_to_darwin(ltype) != SOCK_DGRAM {
        return;
    }
    let sz: c_int = 212992;
    let p = ptr::addr_of!(sz).cast::<c_void>();
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, p, size_of::<c_int>() as u32);
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, p, size_of::<c_int>() as u32);
}

// The netlink socket: one end of a datagram pair, the other end kept to write
// what "the kernel" sends.
unsafe fn nl_socket(ltype: c_int, route: bool) -> c_long {
    let mut sv = [0 as c_int; 2];
    if socketpair(AF_UNIX, SOCK_DGRAM, 0, sv.as_mut_ptr()) != 0 {
        return lerr(errno());
    }
    let [sv0, sv1] = sv;
    if sv0 >= NL_FDS as c_int {
        close(sv0);
        close(sv1);
        return lerr(EMFILE);
    }
    fcntl(sv1, F_SETFD, FD_CLOEXEC);
    apply_type_flags(sv0, ltype);
    // A whole dump is queued before the request's send returns.
    let sz: c_int = 212992;
    let p = ptr::addr_of!(sz).cast::<c_void>();
    setsockopt(sv1, SOL_SOCKET, SO_SNDBUF, p, size_of::<c_int>() as u32);
    setsockopt(sv0, SOL_SOCKET, SO_RCVBUF, p, size_of::<c_int>() as u32);
    let mut st = MaybeUninit::<Stat>::uninit();
    let ino = if fstat(sv0, st.as_mut_ptr()) == 0 {
        (*st.as_ptr()).st_ino
    } else {
        0
    };
    slot(&G_NL_INO, sv0).store(ino, Ordering::SeqCst);
    slot(&G_NL_GROUPS, sv0).store(0, Ordering::SeqCst);
    slot(&G_NL_ROUTE, sv0).store(u8::from(route), Ordering::SeqCst);
    slot(&G_NL_PEER, sv0).store(sv1 + 1, Ordering::SeqCst);
    c_long::from(sv0)
}

// A request to an emulated NETLINK_ROUTE socket; false: fd is not one.
unsafe fn nl_route_send(fd: c_int, buf: *const c_void, len: usize) -> bool {
    if !is_netlink(fd) || slot(&G_NL_ROUTE, fd).load(Ordering::SeqCst) == 0 {
        return false;
    }
    let peer = slot(&G_NL_PEER, fd).load(Ordering::SeqCst) - 1;
    if peer >= 0 && !buf.is_null() {
        lxrt_rtnl_request(peer, buf, len);
    }
    true
}

// What a netlink message's sender address reads: the kernel (port id 0).
unsafe fn nl_kernel_addr(lsa: *mut c_void, llen: *mut u32) {
    if lsa.is_null() || llen.is_null() {
        return;
    }
    let mut nl = [0u8; 12];
    let fam = L_AF_NETLINK as u16;
    ptr::copy_nonoverlapping(ptr::addr_of!(fam).cast::<u8>(), nl.as_mut_ptr(), 2);
    let have = llen.read_unaligned() as usize;
    ptr::copy_nonoverlapping(
        nl.as_ptr(),
        lsa.cast::<u8>(),
        if have < nl.len() { have } else { nl.len() },
    );
    llen.write_unaligned(nl.len() as u32);
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_socket(ldomain: c_int, ltype: c_int, proto: c_int) -> c_long {
    // Only inside a (fake) bwrap container -- where Proton's winebus and the
    // games' SDL live. The Steam client itself, outside, got stuck in its
    // own udev/controller threads once the monitor opened, and its window
    // never appeared (MEASURED: login window with the socket refused, none
    // in 5 minutes with it). LXRT_NO_NETLINK=1 refuses it everywhere.
    let nl_off = !getenv(c"LXRT_NO_NETLINK".as_ptr()).is_null() || !lxrt_mounts_active();
    if ldomain == L_AF_NETLINK && proto == L_NETLINK_KOBJECT_UEVENT && !nl_off {
        return nl_socket(ltype, false);
    }
    // NETLINK_ROUTE: the interface and address dumps behind glibc's
    // getifaddrs() and if_nameindex() (netif.c), everywhere: they only answer
    // what is asked, and no change notification ever arrives.
    if ldomain == L_AF_NETLINK
        && proto == L_NETLINK_ROUTE
        && getenv(c"LXRT_NO_NETLINK".as_ptr()).is_null()
    {
        return nl_socket(ltype, true);
    }
    let d = family_to_darwin(ldomain);
    if d < 0 {
        return lerr(EAFNOSUPPORT);
    }
    let fd = socket(d, type_to_darwin(ltype), proto);
    if fd < 0 {
        return lerr(errno());
    }
    apply_type_flags(fd, ltype);
    seqpkt_mark(fd, ltype);
    unix_dgram_buffers(fd, d, ltype);
    c_long::from(fd)
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_socketpair(
    ldomain: c_int,
    ltype: c_int,
    proto: c_int,
    sv: *mut c_int,
) -> c_long {
    let d = family_to_darwin(ldomain);
    if d < 0 || sv.is_null() {
        return lerr(EAFNOSUPPORT);
    }
    if socketpair(d, type_to_darwin(ltype), proto, sv) != 0 {
        return lerr(errno());
    }
    let sv0 = sv.read_unaligned();
    let sv1 = sv.add(1).read_unaligned();
    apply_type_flags(sv0, ltype);
    apply_type_flags(sv1, ltype);
    seqpkt_mark(sv0, ltype);
    seqpkt_mark(sv1, ltype);
    unix_dgram_buffers(sv0, d, ltype);
    unix_dgram_buffers(sv1, d, ltype);
    0
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_connect(fd: c_int, lsa: *const c_void, llen: c_uint) -> c_long {
    if is_netlink(fd) {
        return 0; // to the kernel: the only peer there is
    }
    let mut ss = MaybeUninit::<SockaddrStorage>::uninit();
    let ssp = ss.as_mut_ptr();
    set_errno(0);
    let n = addr_to_darwin(lsa, llen, ssp);
    if n < 0 {
        let e = errno();
        return lerr(if e != 0 { e } else { EINVAL });
    }
    let mut rc: c_long = if connect(fd, ssp.cast::<c_void>(), n as u32) != 0 {
        lerr(errno())
    } else {
        0
    };
    let family = c_int::from((*ssp).ss_family);
    // Android's property service is started on demand and leaves when idle
    // (props.rs, propsvc.c): a setprop that finds nobody listening starts it
    // and tries once more. Linux ENOENT 2, ECONNREFUSED 111.
    if (rc == -2 || rc == -111) && family == AF_UNIX && llen > 2 {
        let lu_path = lsa.cast::<c_char>().add(SUN_PATH_OFF);
        let cap: usize = if ((llen - 2) as usize) < L_SUN_PATH {
            (llen - 2) as usize
        } else {
            L_SUN_PATH
        };
        let mut g = [0 as c_char; L_SUN_PATH + 1];
        let gl = strnlen(lu_path, cap);
        ptr::copy_nonoverlapping(lu_path, g.as_mut_ptr(), gl);
        *g.as_mut_ptr().add(gl) = 0;
        if lxrt_props_is_socket(g.as_ptr()) && lxrt_props_start_service() {
            rc = if connect(fd, ssp.cast::<c_void>(), n as u32) != 0 {
                lerr(errno())
            } else {
                0
            };
        }
    }
    // The native X server (scripts/run-x11-native.sh) is a macOS process: its
    // socket is the host's /tmp/.X11-unix/X<n>, which no guest root contains.
    // A guest X client connecting to /tmp/.X11-unix/X<n> that finds nothing in
    // its own root reaches the host socket instead. Only connect does this --
    // an X server inside the guest (Xvnc) still binds in its root.
    if rc < 0 && family == AF_UNIX && llen > 2 {
        let lu_path = lsa.cast::<c_char>().add(SUN_PATH_OFF);
        static PRE: &std::ffi::CStr = c"/tmp/.X11-unix/X";
        let pl = PRE.to_bytes().len();
        if strncmp(lu_path, PRE.as_ptr(), pl) == 0 {
            let mut hu: SockaddrUn = std::mem::zeroed();
            hu.sun_family = AF_UNIX as u8;
            hu.sun_len = size_of::<SockaddrUn>() as u8;
            let _ = snprintf(
                ptr::addr_of_mut!(hu.sun_path).cast::<c_char>(),
                D_SUN_PATH,
                c"/private%.*s".as_ptr(),
                strnlen(lu_path, 90) as c_int,
                lu_path,
            );
            if connect(
                fd,
                ptr::addr_of!(hu).cast::<c_void>(),
                size_of::<SockaddrUn>() as u32,
            ) == 0
            {
                rc = 0;
            }
        }
    }
    if lxrt_trace_on() {
        let mut a = [0 as c_char; INET6_ADDRSTRLEN];
        *a.as_mut_ptr() = b'?' as c_char;
        let mut port: c_int = 0;
        if family == AF_INET {
            let di = ssp.cast::<SockaddrIn>();
            inet_ntop(
                AF_INET,
                ptr::addr_of!((*di).sin_addr).cast::<c_void>(),
                a.as_mut_ptr(),
                INET6_ADDRSTRLEN as u32,
            );
            port = c_int::from(u16::from_be((*di).sin_port));
        } else if family == AF_INET6 {
            let d6 = ssp.cast::<SockaddrIn6>();
            inet_ntop(
                AF_INET6,
                ptr::addr_of!((*d6).sin6_addr).cast::<c_void>(),
                a.as_mut_ptr(),
                INET6_ADDRSTRLEN as u32,
            );
            port = c_int::from(u16::from_be((*d6).sin6_port));
        }
        if family == AF_INET || family == AF_INET6 {
            fprintf(
                lxrt_trace_stream(),
                c"[lxrt]    connect fd %d %s port %d -> %ld\n".as_ptr(),
                fd,
                a.as_ptr(),
                port,
                rc,
            );
        }
    }
    rc
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_bind(fd: c_int, lsa: *const c_void, llen: c_uint) -> c_long {
    if is_netlink(fd) {
        // struct sockaddr_nl { u16 family; u16 pad; u32 pid; u32 groups; }
        if lsa.is_null() || llen < 12 {
            return lerr(EINVAL);
        }
        let groups = lsa.cast::<u8>().add(8).cast::<u32>().read_unaligned();
        slot(&G_NL_GROUPS, fd).store(groups, Ordering::SeqCst);
        return 0;
    }
    let mut ss = MaybeUninit::<SockaddrStorage>::uninit();
    let ssp = ss.as_mut_ptr();
    set_errno(0);
    let n = addr_to_darwin(lsa, llen, ssp);
    if n < 0 {
        let e = errno();
        return lerr(if e != 0 { e } else { EINVAL });
    }
    let family = c_int::from((*ssp).ss_family);
    let bp = ssp.cast::<c_char>().add(SUN_PATH_OFF).cast_const();
    // A stale socket file survives a crash, where an abstract socket would
    // not. Unlink before binding so a restart is not blocked by the last run.
    if family == AF_UNIX {
        unlink(bp);
    }
    if bind(fd, ssp.cast::<c_void>(), n as u32) != 0 {
        return lerr(errno());
    }
    let adir = abstract_dir();
    let al: usize = if adir.is_null() { 0 } else { strlen(adir) };
    let long_len = G_ABSTRACT_LONG_LEN.with(Cell::get);
    if family == AF_UNIX
        && al != 0
        && strncmp(bp, adir, al) == 0
        && *bp.add(al) == b'/' as c_char
        && *bp.add(al + 1) == b'~' as c_char
        && long_len != 0
    {
        let mut side = [0 as c_char; 200];
        let _ = snprintf(side.as_mut_ptr(), 200, c"%s.name".as_ptr(), bp);
        let sf = open(
            side.as_ptr(),
            O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
            0o600 as c_int,
        );
        if sf >= 0 {
            let _ = G_ABSTRACT_LONG
                .with(|buffer| write(sf, buffer.get().cast::<c_void>().cast_const(), long_len));
            close(sf);
        }
    }
    G_ABSTRACT_LONG_LEN.with(|len| len.set(0));
    0
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_accept4(
    fd: c_int,
    lsa: *mut c_void,
    llen: *mut u32,
    lflags: c_int,
) -> c_long {
    let mut ss = MaybeUninit::<SockaddrStorage>::uninit();
    let mut dlen = size_of::<SockaddrStorage>() as u32;
    let nfd = accept(fd, ss.as_mut_ptr().cast::<c_void>(), &mut dlen);
    if nfd < 0 {
        return lerr(errno());
    }
    if !lsa.is_null() && !llen.is_null() {
        addr_to_linux(ss.as_ptr(), dlen, lsa, llen);
    }
    apply_type_flags(nfd, lflags);
    c_long::from(nfd)
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_getsockname(
    fd: c_int,
    lsa: *mut c_void,
    llen: *mut u32,
    peer: bool,
) -> c_long {
    if is_netlink(fd) {
        if lsa.is_null() || llen.is_null() {
            return lerr(EFAULT);
        }
        let mut nl = [0u8; 12];
        let fam = L_AF_NETLINK as u16;
        let pid: u32 = if peer { 0 } else { lxrt_ids_pid() as u32 };
        let groups: u32 = if peer {
            0
        } else {
            slot(&G_NL_GROUPS, fd).load(Ordering::SeqCst)
        };
        ptr::copy_nonoverlapping(ptr::addr_of!(fam).cast::<u8>(), nl.as_mut_ptr(), 2);
        ptr::copy_nonoverlapping(ptr::addr_of!(pid).cast::<u8>(), nl.as_mut_ptr().add(4), 4);
        ptr::copy_nonoverlapping(ptr::addr_of!(groups).cast::<u8>(), nl.as_mut_ptr().add(8), 4);
        let have = llen.read_unaligned() as usize;
        ptr::copy_nonoverlapping(
            nl.as_ptr(),
            lsa.cast::<u8>(),
            if have < nl.len() { have } else { nl.len() },
        );
        llen.write_unaligned(nl.len() as u32);
        return 0;
    }
    let mut ss = MaybeUninit::<SockaddrStorage>::uninit();
    let ssp = ss.as_mut_ptr();
    let mut dlen = size_of::<SockaddrStorage>() as u32;
    let rc = if peer {
        getpeername(fd, ssp.cast::<c_void>(), &mut dlen)
    } else {
        getsockname(fd, ssp.cast::<c_void>(), &mut dlen)
    };
    if rc != 0 {
        return lerr(errno());
    }
    addr_to_linux(ssp, dlen, lsa, llen);
    if lxrt_trace_on()
        && c_int::from((*ssp).ss_family) == AF_UNIX
        && !lsa.is_null()
        && !llen.is_null()
    {
        let off = SUN_PATH_OFF;
        let hl: c_int = if dlen as usize > off {
            (dlen as usize - off) as c_int
        } else {
            0
        };
        let guest_len = llen.read_unaligned();
        fprintf(
            lxrt_trace_stream(),
            c"[lxrt]    %s fd %d: host \"%.*s\" -> guest len %u \"%.*s\"\n".as_ptr(),
            if peer {
                c"getpeername".as_ptr()
            } else {
                c"getsockname".as_ptr()
            },
            fd,
            hl,
            ssp.cast::<c_char>().add(SUN_PATH_OFF).cast_const(),
            guest_len,
            if guest_len > 2 {
                (guest_len - 2) as c_int
            } else {
                0
            },
            lsa.cast::<c_char>().add(2).cast_const(),
        );
    }
    0
}

// ---------------------------------------------------------------- msghdr
//
// The fifth number space, and the one that hangs rather than fails: struct
// msghdr and struct cmsghdr are laid out differently, and SOL_SOCKET is 1 on
// Linux and 0xffff on Darwin.
//
//   msghdr      Linux                      Darwin
//   msg_iovlen  size_t (8 bytes)           int (4 bytes)
//   msg_controllen size_t (8)              socklen_t (4)
//   -- and Linux pads after msg_namelen, Darwin does not.
//
//   cmsghdr     Linux                      Darwin
//   cmsg_len    size_t (8 bytes)           socklen_t (4 bytes)
//   alignment   8                          4
//
// Passing one through unchanged makes an SCM_RIGHTS fd transfer silently
// malformed. FEXServer hands the RootFS fd to FEX that way; the symptom was
// FEX blocking forever in a receive rather than reporting anything.

#[repr(C)]
struct LinuxMsghdr {
    msg_name: u64,
    msg_namelen: u32,
    pad_: u32,
    msg_iov: u64,
    msg_iovlen: u64,
    msg_control: u64,
    msg_controllen: u64,
    msg_flags: i32,
    pad2_: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxCmsghdr {
    cmsg_len: u64,
    cmsg_level: i32,
    cmsg_type: i32,
}

const _: [(); 56] = [(); size_of::<LinuxMsghdr>()];
const _: [(); 16] = [(); size_of::<LinuxCmsghdr>()];

// One member of a guest structure, read or written where it is used.
macro_rules! guest_get {
    ($pointer:expr, $field:ident) => {
        ptr::addr_of!((*$pointer).$field).read_unaligned()
    };
}
macro_rules! guest_set {
    ($pointer:expr, $field:ident, $value:expr) => {
        ptr::addr_of_mut!((*$pointer).$field).write_unaligned($value)
    };
}

const L_SOL_SOCKET: c_int = 1;
const L_SCM_RIGHTS: c_int = 1;

const L_SOL_SOCKET_OPT: c_int = 1;
// SO_PASSCRED / SO_PEERCRED / SCM_CREDENTIALS. Darwin's unix sockets cannot be
// asked to attach credentials to every message (there is no LOCAL_CREDS in
// the public SDK), but they can always be ASKED who the peer is: LOCAL_PEERPID
// and LOCAL_PEERCRED. So SO_PASSCRED is remembered per descriptor and
// recvmsg() synthesises the SCM_CREDENTIALS control message from those two,
// which for a socketpair or a connected unix socket is the same answer Linux
// would give. Chromium's Mojo channel (steamwebhelper) CHECKs the setsockopt
// and reads the peer pid from the first message.
const L_SO_PASSCRED: c_int = 16;
const L_SO_PEERCRED: c_int = 17;
const L_SCM_CREDENTIALS: c_int = 2;
#[repr(C)]
struct LinuxUcred {
    pid: i32,
    uid: u32,
    gid: u32,
}
static G_PASSCRED: Racy<[u8; 65536 / 8]> = Racy::new([0; 65536 / 8]);

unsafe fn passcred_get(fd: c_int) -> bool {
    (0..65536).contains(&fd)
        && (*G_PASSCRED.get().cast::<u8>().add((fd >> 3) as usize) >> (fd & 7)) & 1 != 0
}

unsafe fn passcred_set(fd: c_int, on: bool) {
    if !(0..65536).contains(&fd) {
        return;
    }
    let byte = G_PASSCRED.get().cast::<u8>().add((fd >> 3) as usize);
    if on {
        *byte |= (1u32 << (fd & 7)) as u8;
    } else {
        *byte &= !(1u32 << (fd & 7)) as u8;
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_socket_close(fd: c_int) {
    passcred_set(fd, false);
    if (0..NL_FDS as c_int).contains(&fd) {
        let peer = slot(&G_NL_PEER, fd).swap(0, Ordering::SeqCst);
        if peer != 0 {
            close(peer - 1);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_socket_dup(oldfd: c_int, newfd: c_int) {
    passcred_set(newfd, passcred_get(oldfd));
}

// Who sent the message? Linux answers per message (SCM_CREDENTIALS carries
// the SENDER's pid); Darwin's LOCAL_PEERPID answers who created or connected
// the peer socket, which for a socketpair inherited across fork is the
// parent. Chromium's zygote learns each child's pid exactly that way (the
// child pings the browser over a socketpair the browser made) and got the
// browser's own pid back: "Zygote could not fork ... child_pid -1".
//
// So senders record themselves. Each AF_UNIX send stores (the kernel identity
// of the sending socket, sender pid) in a table shared by every runtime
// process of this user; the receiver looks up its peer's identity. Socket
// identities come from proc_pidfdinfo and are the same in every process that
// holds the socket (measured: a forked child sees the parent's values).
// Last sender wins, which is what a Linux receiver would see for a message
// just sent; a peer outside the runtime falls back to LOCAL_PEERPID.
#[repr(C)]
struct CredEnt {
    so: AtomicU64,
    pid: AtomicI32,
}
const _: [(); 16] = [(); size_of::<CredEnt>()];
const CRED_SLOTS: usize = 8192;
static G_CREDS: Racy<*mut CredEnt> = Racy::new(ptr::null_mut());
static CRED_TABLE_STATE: AtomicI32 = AtomicI32::new(0); // 0 untried, 1 ready, 2 failed

unsafe fn cred_table() -> *mut CredEnt {
    let st = CRED_TABLE_STATE.load(Ordering::SeqCst);
    if st == 1 {
        return *G_CREDS.get();
    }
    if st == 2 {
        return ptr::null_mut();
    }
    let mut path = [0 as c_char; 96];
    let _ = snprintf(
        path.as_mut_ptr(),
        96,
        c"/tmp/lxrt-shm-%u".as_ptr(),
        getuid() as c_uint,
    );
    mkdir(path.as_ptr(), 0o1777);
    let at = strlen(path.as_ptr());
    let _ = snprintf(path.as_mut_ptr().add(at), 96 - at, c"/unix-creds".as_ptr());
    let fd = open(path.as_ptr(), O_RDWR | O_CREAT | O_CLOEXEC, 0o600 as c_int);
    let sz = size_of::<CredEnt>() * CRED_SLOTS;
    let mut p = MAP_FAILED;
    if fd >= 0 && ftruncate(fd, sz as i64) == 0 {
        p = mmap(ptr::null_mut(), sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    if fd >= 0 {
        close(fd);
    }
    if p == MAP_FAILED {
        CRED_TABLE_STATE.store(2, Ordering::SeqCst);
        return ptr::null_mut();
    }
    *G_CREDS.get() = p.cast::<CredEnt>();
    CRED_TABLE_STATE.store(1, Ordering::SeqCst);
    *G_CREDS.get()
}

#[inline]
unsafe fn socket_info_u64(si: *const SocketFdInfo, offset: usize) -> u64 {
    si.cast::<u8>().add(offset).cast::<u64>().read_unaligned()
}

unsafe fn unix_ids(fd: c_int, self_so: *mut u64, peer_so: *mut u64) -> bool {
    let mut si = MaybeUninit::<SocketFdInfo>::uninit();
    let sip = si.as_mut_ptr();
    if proc_pidfdinfo(
        getpid(),
        fd,
        PROC_PIDFDSOCKETINFO,
        sip.cast::<c_void>(),
        size_of::<SocketFdInfo>() as c_int,
    ) != size_of::<SocketFdInfo>() as c_int
        || sip
            .cast::<u8>()
            .add(SI_SOI_FAMILY)
            .cast::<c_int>()
            .read_unaligned()
            != AF_UNIX
    {
        return false;
    }
    *self_so = socket_info_u64(sip, SI_SOI_SO);
    *peer_so = socket_info_u64(sip, SI_UNSI_CONN_SO);
    true
}

#[inline]
fn cred_hash(so: u64) -> u32 {
    ((so.wrapping_mul(0x9E37_79B9_7F4A_7C15) >> 51) as u32) % CRED_SLOTS as u32
}

unsafe fn cred_note_send(fd: c_int) {
    let mut self_so: u64 = 0;
    let mut peer_so: u64 = 0;
    if !unix_ids(fd, &mut self_so, &mut peer_so) || self_so == 0 {
        return;
    }
    let t = cred_table();
    if t.is_null() {
        return;
    }
    let h = cred_hash(self_so);
    for k in 0..64u32 {
        let e = &*t.add((h.wrapping_add(k) % CRED_SLOTS as u32) as usize);
        let mut cur = e.so.load(Ordering::SeqCst);
        if cur == self_so
            || (cur == 0
                && match e
                    .so
                    .compare_exchange(cur, self_so, Ordering::SeqCst, Ordering::SeqCst)
                {
                    Ok(_) => true,
                    Err(seen) => {
                        cur = seen;
                        false
                    }
                })
            || cur == self_so
        /* lost the race to the same socket */
        {
            e.pid.store(getpid(), Ordering::SeqCst);
            return;
        }
    }
}

// Does process `pid` (this user's) have a descriptor on the socket whose
// kernel identity is `so`?
unsafe fn pid_holds_socket(pid: c_int, so: u64) -> bool {
    if pid == getpid() {
        return true;
    }
    let mut n = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, ptr::null_mut(), 0);
    if n <= 0 {
        return false;
    }
    let fds = malloc(n as usize).cast::<ProcFdInfo>();
    if fds.is_null() {
        return true; // cannot tell: trust the record
    }
    n = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.cast::<c_void>(), n);
    let mut found = false;
    let mut i: c_int = 0;
    while i < n / size_of::<ProcFdInfo>() as c_int && !found {
        let entry = fds.add(i as usize);
        i += 1;
        if (*entry).proc_fdtype != PROX_FDTYPE_SOCKET {
            continue;
        }
        let mut si = MaybeUninit::<SocketFdInfo>::uninit();
        if proc_pidfdinfo(
            pid,
            (*entry).proc_fd,
            PROC_PIDFDSOCKETINFO,
            si.as_mut_ptr().cast::<c_void>(),
            size_of::<SocketFdInfo>() as c_int,
        ) == size_of::<SocketFdInfo>() as c_int
            && socket_info_u64(si.as_ptr(), SI_SOI_SO) == so
        {
            found = true;
        }
    }
    free(fds.cast::<c_void>());
    found
}

unsafe fn cred_peer_pid(fd: c_int) -> c_int {
    let mut self_so: u64 = 0;
    let mut peer_so: u64 = 0;
    if !unix_ids(fd, &mut self_so, &mut peer_so) || peer_so == 0 {
        return 0;
    }
    let t = cred_table();
    if t.is_null() {
        return 0;
    }
    let h = cred_hash(peer_so);
    for k in 0..64u32 {
        let e = &*t.add((h.wrapping_add(k) % CRED_SLOTS as u32) as usize);
        let cur = e.so.load(Ordering::SeqCst);
        if cur == peer_so {
            let p = e.pid.load(Ordering::SeqCst);
            // A recorded sender that has since exited is stale; so is one
            // that no longer holds the socket (its kernel address has been
            // given to a new one since: a live FEXServer was reported as
            // the peer of a fresh socketpair, MEASURED).
            return if p > 0 && kill(p, 0) == 0 && pid_holds_socket(p, peer_so) {
                p
            } else {
                0
            };
        }
        if cur == 0 {
            break;
        }
    }
    0
}

// `real`: SCM_CREDENTIALS, which Linux fills with the sender's real ids;
// SO_PEERCRED reports the effective ones.
unsafe fn peer_ucred(fd: c_int, out: *mut LinuxUcred, real: bool) -> bool {
    // The recorded sender first (see cred_note_send); LOCAL_PEERPID and
    // LOCAL_PEERCRED only answer for connected stream sockets -- a DGRAM
    // socketpair (what SOCK_SEQPACKET becomes here) had no credentials at all.
    let sender = cred_peer_pid(fd);
    let mut pid: c_int = 0;
    let mut pl = size_of::<c_int>() as u32;
    let mut xu: Xucred = std::mem::zeroed();
    let mut xl = size_of::<Xucred>() as u32;
    let have_pid = getsockopt(
        fd,
        SOL_LOCAL,
        LOCAL_PEERPID,
        ptr::addr_of_mut!(pid).cast::<c_void>(),
        &mut pl,
    ) == 0;
    let have_cred = getsockopt(
        fd,
        SOL_LOCAL,
        LOCAL_PEERCRED,
        ptr::addr_of_mut!(xu).cast::<c_void>(),
        &mut xl,
    ) == 0;
    if sender == 0 && !have_pid {
        return false;
    }
    (*out).pid = if sender != 0 { sender } else { pid };
    // Every process here runs as this user; a peer outside the runtime says so.
    (*out).uid = if have_cred { xu.cr_uid } else { getuid() };
    (*out).gid = if have_cred {
        if xu.cr_ngroups > 0 {
            xu.cr_groups[0]
        } else {
            0
        }
    } else {
        getgid()
    };
    // A peer with Android ids (runtime/android_ids.h) is who it says it is:
    // its effective uid and gid, as Linux's SO_PEERCRED reports.
    let mut ap = MaybeUninit::<LxrtAidsPeer>::uninit();
    if lxrt_aids_lookup((*out).pid, ap.as_mut_ptr()) {
        let ap = &*ap.as_ptr();
        (*out).uid = if real { ap.ruid } else { ap.euid };
        (*out).gid = if real { ap.rgid } else { ap.egid };
    }
    if lxrt_ids_on() {
        (*out).pid = lxrt_ids_to_guest((*out).pid, 0);
        if (*out).pid == 0 {
            (*out).pid = 1;
        }
    }
    true
}

// LALIGN(n): (((n) + 7u) & ~7u), in 32-bit unsigned arithmetic.
#[inline]
fn lalign(n: u32) -> u32 {
    n.wrapping_add(7) & !7
}

// Darwin's CMSG_* (sys/socket.h): 4-byte alignment, a 12-byte header.
#[inline]
fn darwin_align32(n: usize) -> usize {
    n.wrapping_add(3) & !3
}
#[inline]
fn cmsg_len(payload: usize) -> usize {
    darwin_align32(size_of::<CmsgHdr>()).wrapping_add(payload)
}
#[inline]
fn cmsg_space(payload: usize) -> usize {
    darwin_align32(size_of::<CmsgHdr>()).wrapping_add(darwin_align32(payload))
}
#[inline]
unsafe fn cmsg_data(c: *mut CmsgHdr) -> *mut u8 {
    c.cast::<u8>().add(darwin_align32(size_of::<CmsgHdr>()))
}

fn cmsg_level_to_darwin(l: c_int) -> c_int {
    if l == L_SOL_SOCKET {
        SOL_SOCKET
    } else {
        l
    }
}
fn cmsg_level_to_linux(l: c_int) -> c_int {
    if l == SOL_SOCKET {
        L_SOL_SOCKET
    } else {
        l
    }
}

// Builds a Darwin control buffer from a Linux one. Returns bytes written, or
// -1. `dst` must be at least as large as the Linux buffer.
unsafe fn cmsg_to_darwin(lbuf: *const c_void, llen: usize, dst: *mut c_void, dcap: usize) -> c_long {
    let p = lbuf.cast::<u8>();
    let out = dst.cast::<u8>();
    let mut in_off: usize = 0;
    let mut out_off: usize = 0;
    const LHDR: usize = size_of::<LinuxCmsghdr>();

    while in_off.wrapping_add(LHDR) <= llen {
        let lcp = p.add(in_off).cast::<LinuxCmsghdr>();
        let lc = lcp.read_unaligned();
        // cmsg_len is the guest's 64-bit number: `in_off + cmsg_len` could
        // wrap past the check and send a huge payload into the copy below.
        if lc.cmsg_len < LHDR as u64 || lc.cmsg_len > (llen - in_off) as u64 {
            break;
        }
        let payload = lc.cmsg_len as usize - LHDR;
        // SCM_CREDENTIALS has no Darwin form: its type (2) is Darwin's
        // SCM_TIMESTAMP, and sendmsg failed. Linux's libpulse sends it with
        // its first packet, so every PulseAudio client lost its connection
        // (MEASURED). A receiver in this runtime gets the sender's
        // credentials from SO_PASSCRED anyway (lxrt_recvmsg below).
        if lc.cmsg_level == L_SOL_SOCKET && lc.cmsg_type == L_SCM_CREDENTIALS {
            in_off = in_off.wrapping_add(lalign(lc.cmsg_len as u32) as usize);
            continue;
        }
        // SCM_RIGHTS without a descriptor: Linux attaches nothing
        // (net/core/scm.c scm_fp_copy, num <= 0), Darwin delivers an empty
        // SCM_RIGHTS the receiver has to cope with. Android's LocalSocket
        // sends one whenever its outbound descriptor array is empty, and
        // libbase's ReceiveFileDescriptorVector aborts on it ("cmsg_len(16)
        // not long enough to hold any data": the zygote, MEASURED at stage 28).
        if lc.cmsg_level == L_SOL_SOCKET
            && lc.cmsg_type == L_SCM_RIGHTS
            && payload < size_of::<c_int>()
        {
            in_off = in_off.wrapping_add(lalign(lc.cmsg_len as u32) as usize);
            continue;
        }
        let need = cmsg_space(payload);
        if out_off.wrapping_add(need) > dcap {
            return -1;
        }

        let dc = out.add(out_off).cast::<CmsgHdr>();
        dc.write_unaligned(CmsgHdr {
            cmsg_len: cmsg_len(payload) as u32,
            cmsg_level: cmsg_level_to_darwin(lc.cmsg_level),
            cmsg_type: lc.cmsg_type, // SCM_RIGHTS is 1 on both
        });
        ptr::copy_nonoverlapping(lcp.cast::<u8>().add(LHDR), cmsg_data(dc), payload);

        out_off += need;
        in_off = in_off.wrapping_add(lalign(lc.cmsg_len as u32) as usize);
    }
    out_off as c_long
}

unsafe fn cmsg_to_linux(dbuf: *const c_void, dlen: usize, lout: *mut c_void, lcap: usize) -> c_long {
    let p = dbuf.cast::<u8>();
    let out = lout.cast::<u8>();
    let mut in_off: usize = 0;
    let mut out_off: usize = 0;
    const DHDR: usize = size_of::<CmsgHdr>();
    const LHDR: usize = size_of::<LinuxCmsghdr>();

    while in_off.wrapping_add(DHDR) <= dlen {
        let dcp = p.add(in_off).cast::<CmsgHdr>();
        let dc = dcp.read_unaligned();
        if (dc.cmsg_len as usize) < DHDR || in_off.wrapping_add(dc.cmsg_len as usize) > dlen {
            break;
        }
        let payload = dc.cmsg_len as usize - DHDR;
        if dc.cmsg_level == SOL_SOCKET && dc.cmsg_type == SCM_RIGHTS && payload < size_of::<c_int>()
        {
            // an empty SCM_RIGHTS: Linux never has one
            in_off = in_off.wrapping_add((dc.cmsg_len.wrapping_add(3) & !3) as usize);
            continue;
        }
        let need = lalign((LHDR + payload) as u32) as usize;
        if out_off.wrapping_add(need) > lcap {
            return -1;
        }

        let lc = out.add(out_off).cast::<LinuxCmsghdr>();
        lc.write_unaligned(LinuxCmsghdr {
            cmsg_len: (LHDR + payload) as u64,
            cmsg_level: cmsg_level_to_linux(dc.cmsg_level),
            cmsg_type: dc.cmsg_type,
        });
        ptr::copy_nonoverlapping(
            dcp.cast::<u8>().add(DHDR),
            lc.cast::<u8>().add(LHDR),
            payload,
        );

        out_off += need;
        // Darwin aligns to 4; CMSG_ALIGN does it for us via the next pointer.
        in_off = in_off.wrapping_add((dc.cmsg_len.wrapping_add(3) & !3) as usize);
    }
    out_off as c_long
}

const CTRL_MAX: usize = 4096;

// The Darwin control buffer of one sendmsg/recvmsg, on the stack as in C
// (uninitialised until the kernel or cmsg_to_darwin fills it).
#[repr(C, align(8))]
struct Ctrl([u8; CTRL_MAX]);

// MSG_* flags: another silent number space. Linux -> Darwin:
//   DONTWAIT 0x40 -> 0x80, EOR 0x80 -> 0x8, WAITALL 0x100 -> 0x40,
//   NOSIGNAL 0x4000 -> 0x80000; OOB/PEEK/DONTROUTE (1/2/4) agree;
//   CMSG_CLOEXEC has no meaning here.
// TRUNC (0x20) and CTRUNC (0x8) are NOT passed on: as recvmsg input Linux
// ignores CTRUNC and reads TRUNC only on datagram and packet sockets (return
// the datagram's real length), while XNU copies both into msg_flags on
// return whether or not anything was truncated (MEASURED, macOS 27: one byte
// into a 4-byte buffer, input MSG_TRUNC -> msg_flags MSG_TRUNC, on stream
// and datagram sockets alike). libbase's ReceiveFileDescriptorVector passes
// both and then refuses a message that reports either: i386 Android's
// zygote ("message was truncated when receiving file descriptors") and
// system_server talking to it. The real-length reading of input TRUNC on a
// datagram socket is not emulated.
#[no_mangle]
pub extern "C" fn lxrt_msgflags_to_darwin(lf: c_int) -> c_int {
    let mut d = lf & 0x7;
    if lf & 0x40 != 0 {
        d |= 0x80;
    }
    if lf & 0x80 != 0 {
        d |= 0x8;
    }
    if lf & 0x100 != 0 {
        d |= 0x40;
    }
    if lf & 0x4000 != 0 {
        d |= 0x80000;
    }
    d
}

#[no_mangle]
pub extern "C" fn lxrt_msgflags_to_linux(df: c_int) -> c_int {
    let mut l = df & 0x7;
    if df & 0x20 != 0 {
        l |= 0x8;
    }
    if df & 0x10 != 0 {
        l |= 0x20;
    }
    if df & 0x8 != 0 {
        l |= 0x80;
    }
    l
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_sendto(
    fd: c_int,
    buf: *const c_void,
    len: usize,
    lflags: c_int,
    laddr: *const c_void,
    lalen: c_uint,
) -> c_long {
    if nl_route_send(fd, buf, len) {
        return len as c_long;
    }
    cred_note_send(fd);
    let df = lxrt_msgflags_to_darwin(lflags);
    if laddr.is_null() {
        let r = send(fd, buf, len, df);
        return if r < 0 { lerr(errno()) } else { r as c_long };
    }
    let mut ss = MaybeUninit::<SockaddrStorage>::uninit();
    set_errno(0);
    let dlen = addr_to_darwin(laddr, lalen, ss.as_mut_ptr());
    if dlen < 0 {
        let e = errno();
        return lerr(if e != 0 { e } else { EINVAL });
    }
    let r = sendto(fd, buf, len, df, ss.as_ptr().cast::<c_void>(), dlen as u32);
    if r < 0 {
        lerr(errno())
    } else {
        r as c_long
    }
}

// read() and readv() on a seqpacket socket: the same slices and the same end
// of file as a blocking recvmsg. Steam's web helper waits for each zygote's
// hello with read(); a zygote that died before sending it left the browser
// blocked for good (MEASURED, no window), where Linux returns 0 and the
// browser gives up and is restarted. false: not a seqpacket socket.
#[no_mangle]
pub unsafe extern "C" fn lxrt_seqpkt_readv(
    fd: c_int,
    iov: *const Iovec,
    cnt: c_int,
    ret: *mut c_long,
) -> bool {
    if !lxrt_is_seqpacket(fd) {
        return false;
    }
    let w = seqpkt_before_recv(fd, 0);
    if w < 0 {
        *ret = w;
        return true;
    }
    let r = readv(fd, iov, cnt);
    if r < 0 && errno() == ECONNRESET {
        *ret = 0; // the peer is gone: end of file (see above)
    } else {
        *ret = if r < 0 { lerr(errno()) } else { r as c_long };
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_recvfrom(
    fd: c_int,
    buf: *mut c_void,
    len: usize,
    lflags: c_int,
    laddr: *mut c_void,
    lalen: *mut u32,
) -> c_long {
    let df = lxrt_msgflags_to_darwin(lflags);
    let mut ss = MaybeUninit::<SockaddrStorage>::uninit();
    let mut sl = size_of::<SockaddrStorage>() as u32;
    let w = seqpkt_before_recv(fd, lflags);
    if w < 0 {
        return w;
    }
    let r = recvfrom(
        fd,
        buf,
        len,
        df,
        if laddr.is_null() {
            ptr::null_mut()
        } else {
            ss.as_mut_ptr().cast::<c_void>()
        },
        if laddr.is_null() {
            ptr::null_mut()
        } else {
            &mut sl
        },
    );
    if r < 0 && errno() == ECONNRESET && lxrt_is_seqpacket(fd) {
        return 0; // the peer is gone: end of file (see above)
    }
    if r < 0 {
        return lerr(errno());
    }
    if !laddr.is_null() && !lalen.is_null() && is_netlink(fd) {
        nl_kernel_addr(laddr, lalen);
    } else if !laddr.is_null() && !lalen.is_null() {
        addr_to_linux(ss.as_ptr(), sl, laddr, lalen);
    }
    r as c_long
}

// ------------------------------------------- eventfds across SCM_RIGHTS
//
// An eventfd here is a pipe plus a counter the runtime keeps (epoll_eventfd.c);
// sent as is, the receiving process got a bare pipe: a read of 8 bytes saw a
// token byte, a write landed nowhere. Wine's esync is exactly that traffic --
// wineserver makes an eventfd per synchronization object and sends it to each
// client -- and failed (tests/elf/eventfd_scm.c, MEASURED). So a message that
// carries eventfds also carries, after the guest's descriptors, a marker
// descriptor and, per eventfd, the file behind its counter and its token
// pipe's write end. The receiving runtime takes them off before the guest sees
// the message and makes each received pipe the same eventfd
// (lxrt_eventfd_adopt).
static SCM_MARKER_FD: Racy<c_int> = Racy::new(-2);
static SCM_MARKER_MU: Racy<PthreadMutex> = Racy::new(PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
});

unsafe fn scm_marker_path(path: *mut c_char, capacity: usize) {
    let _ = snprintf(
        path,
        capacity,
        c"/tmp/lxrt-scm-marker-%u".as_ptr(),
        getuid() as c_uint,
    );
}

unsafe fn scm_marker_fd() -> c_int {
    pthread_mutex_lock(SCM_MARKER_MU.get());
    if *SCM_MARKER_FD.get() == -2 {
        let mut path = [0 as c_char; 64];
        scm_marker_path(path.as_mut_ptr(), 64);
        *SCM_MARKER_FD.get() =
            lxrt_open_private(path.as_ptr(), O_RDONLY | O_CREAT | O_CLOEXEC, 0o600);
    }
    let r = *SCM_MARKER_FD.get();
    pthread_mutex_unlock(SCM_MARKER_MU.get());
    r
}

unsafe fn is_scm_marker(fd: c_int) -> bool {
    let mut path = [0 as c_char; 64];
    scm_marker_path(path.as_mut_ptr(), 64);
    let mut a = MaybeUninit::<Stat>::uninit();
    let mut b = MaybeUninit::<Stat>::uninit();
    fstat(fd, a.as_mut_ptr()) == 0
        && (*a.as_ptr()).st_mode & S_IFMT == S_IFREG
        && stat(path.as_ptr(), b.as_mut_ptr()) == 0
        && (*a.as_ptr()).st_dev == (*b.as_ptr()).st_dev
        && (*a.as_ptr()).st_ino == (*b.as_ptr()).st_ino
}

#[inline]
unsafe fn fd_at(fds: *const c_int, index: c_int) -> c_int {
    fds.offset(index as isize).read_unaligned()
}

// Sender: append the companions to the (first) SCM_RIGHTS message in `ctrl`.
unsafe fn scm_add_eventfds(ctrl: *mut u8, len: u32, cap: usize) -> u32 {
    let c = ctrl.cast::<CmsgHdr>();
    if (len as usize) < size_of::<CmsgHdr>()
        || (*c).cmsg_level != SOL_SOCKET
        || (*c).cmsg_type != SCM_RIGHTS
    {
        return len;
    }
    let n = (((*c).cmsg_len as usize).wrapping_sub(cmsg_len(0)) / size_of::<c_int>()) as c_int;
    let fds = cmsg_data(c).cast::<c_int>();
    let mut extra = [0 as c_int; 2 * 64 + 1];
    let extrap = extra.as_mut_ptr();
    let mut ne: c_int = 0;
    let mut i: c_int = 0;
    while i < n && ne < 2 * 64 {
        let mut sfd: c_int = 0;
        let mut wfd: c_int = 0;
        if lxrt_eventfd_companions(fd_at(fds, i), &mut sfd, &mut wfd) != 0 {
            *extrap.add(1 + ne as usize) = sfd;
            ne += 1;
            *extrap.add(1 + ne as usize) = wfd;
            ne += 1;
        }
        i += 1;
    }
    if ne == 0 {
        return len;
    }
    let marker = scm_marker_fd();
    if marker < 0 {
        return len;
    }
    *extrap = marker;
    ne += 1;
    // Only when it is the sole control message, and it fits.
    if cmsg_space((n as usize).wrapping_mul(size_of::<c_int>())) as u32 != len
        && ((*c).cmsg_len.wrapping_add(3) & !3) != len
    {
        return len;
    }
    let newlen = cmsg_space(((n + ne) as usize).wrapping_mul(size_of::<c_int>()));
    if newlen > cap {
        return len;
    }
    ptr::copy_nonoverlapping(
        extrap.cast_const().cast::<u8>(),
        fds.offset(n as isize).cast::<u8>(),
        ne as usize * size_of::<c_int>(),
    );
    (*c).cmsg_len = cmsg_len(((n + ne) as usize).wrapping_mul(size_of::<c_int>())) as u32;
    newlen as u32
}

// Receiver: take the companions off and adopt; returns the new length.
unsafe fn scm_take_eventfds(ctrl: *mut u8, len: u32) -> u32 {
    let c = ctrl.cast::<CmsgHdr>();
    if (len as usize) < size_of::<CmsgHdr>()
        || (*c).cmsg_level != SOL_SOCKET
        || (*c).cmsg_type != SCM_RIGHTS
    {
        return len;
    }
    let n = (((*c).cmsg_len as usize).wrapping_sub(cmsg_len(0)) / size_of::<c_int>()) as c_int;
    let fds = cmsg_data(c).cast::<c_int>().cast_const();
    let mut m: c_int = -1;
    let mut i = n.wrapping_sub(3);
    // marker + at least one pair after it
    while i >= 0 && m < 0 {
        if is_scm_marker(fd_at(fds, i)) && (n - i - 1) % 2 == 0 {
            m = i;
        }
        i -= 1;
    }
    if m < 0 {
        return len;
    }
    close(fd_at(fds, m));
    let mut k = m + 1;
    while k + 1 < n {
        let sfd = fd_at(fds, k);
        let wfd = fd_at(fds, k + 1);
        let mut done = false;
        let mut g: c_int = 0;
        while g < m && !done {
            let guest_fd = fd_at(fds, g);
            g += 1;
            let mut gs = MaybeUninit::<Stat>::uninit();
            if fstat(guest_fd, gs.as_mut_ptr()) != 0
                || (*gs.as_ptr()).st_mode & S_IFMT != S_IFIFO
                || lxrt_eventfd_is(guest_fd)
            {
                continue;
            }
            let s2 = dup(sfd);
            let w2 = dup(wfd);
            if s2 < 0 || w2 < 0 {
                if s2 >= 0 {
                    close(s2);
                }
                if w2 >= 0 {
                    close(w2);
                }
                continue;
            }
            done = lxrt_eventfd_adopt(guest_fd, s2, w2); // takes s2 and w2
        }
        close(sfd);
        close(wfd);
        k += 2;
    }
    let keep = cmsg_space(m as usize * size_of::<c_int>()) as u32;
    (*c).cmsg_len = cmsg_len(m as usize * size_of::<c_int>()) as u32;
    // A message with nothing after its SCM_RIGHTS (all this ever adds to).
    if keep < len {
        keep
    } else {
        len
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_sendmsg(fd: c_int, lmsg: *const c_void, flags: c_int) -> c_long {
    if lmsg.is_null() {
        return lerr(EFAULT);
    }
    cred_note_send(fd);
    let lm = lmsg.cast::<LinuxMsghdr>();
    if is_netlink(fd) && slot(&G_NL_ROUTE, fd).load(Ordering::SeqCst) != 0 {
        // The request, gathered from its pieces.
        let iov = guest_get!(lm, msg_iov) as usize as *const Iovec;
        let mut total: usize = 0;
        let mut i: u64 = 0;
        while !iov.is_null() && i < guest_get!(lm, msg_iovlen) {
            total = total.wrapping_add(guest_get!(iov.add(i as usize), iov_len));
            i += 1;
        }
        let req: *mut u8 = if total != 0 && total <= 65536 {
            malloc(total).cast::<u8>()
        } else {
            ptr::null_mut()
        };
        let mut at: usize = 0;
        let mut i: u64 = 0;
        while !req.is_null() && i < guest_get!(lm, msg_iovlen) {
            let piece = iov.add(i as usize);
            let piece_len = guest_get!(piece, iov_len);
            ptr::copy_nonoverlapping(
                guest_get!(piece, iov_base).cast::<u8>().cast_const(),
                req.add(at),
                piece_len,
            );
            at = at.wrapping_add(piece_len);
            i += 1;
        }
        nl_route_send(fd, req.cast::<c_void>().cast_const(), at);
        free(req.cast::<c_void>());
        return total as c_long;
    }

    let mut dm: MsgHdr = std::mem::zeroed();
    let mut ss = MaybeUninit::<SockaddrStorage>::uninit();
    if guest_get!(lm, msg_name) != 0 && guest_get!(lm, msg_namelen) != 0 {
        set_errno(0);
        let n = addr_to_darwin(
            guest_get!(lm, msg_name) as usize as *const c_void,
            guest_get!(lm, msg_namelen),
            ss.as_mut_ptr(),
        );
        if n < 0 {
            let e = errno();
            return lerr(if e != 0 { e } else { EINVAL });
        }
        dm.msg_name = ss.as_mut_ptr().cast::<c_void>();
        dm.msg_namelen = n as u32;
    }
    dm.msg_iov = guest_get!(lm, msg_iov) as usize as *mut Iovec; // same layout
    dm.msg_iovlen = guest_get!(lm, msg_iovlen) as c_int;

    let mut ctrl = MaybeUninit::<Ctrl>::uninit();
    let ctrlp = ctrl.as_mut_ptr().cast::<u8>();
    if guest_get!(lm, msg_control) != 0 && guest_get!(lm, msg_controllen) != 0 {
        let n = cmsg_to_darwin(
            guest_get!(lm, msg_control) as usize as *const c_void,
            guest_get!(lm, msg_controllen) as usize,
            ctrlp.cast::<c_void>(),
            CTRL_MAX,
        );
        if n < 0 {
            return lerr(EINVAL);
        }
        if n > 0 {
            // nothing left (only credentials): no control at all
            dm.msg_control = ctrlp.cast::<c_void>();
            dm.msg_controllen = scm_add_eventfds(ctrlp, n as u32, CTRL_MAX);
        }
    }
    dm.msg_flags = guest_get!(lm, msg_flags);

    let r = sendmsg(fd, &dm, lxrt_msgflags_to_darwin(flags));
    if r < 0 {
        lerr(errno())
    } else {
        r as c_long
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_recvmsg(fd: c_int, lmsg: *mut c_void, flags: c_int) -> c_long {
    if lmsg.is_null() {
        return lerr(EFAULT);
    }
    let lm = lmsg.cast::<LinuxMsghdr>();

    let mut dm: MsgHdr = std::mem::zeroed();
    let mut ss = MaybeUninit::<SockaddrStorage>::uninit();
    if guest_get!(lm, msg_name) != 0 && guest_get!(lm, msg_namelen) != 0 {
        dm.msg_name = ss.as_mut_ptr().cast::<c_void>();
        dm.msg_namelen = size_of::<SockaddrStorage>() as u32;
    }
    dm.msg_iov = guest_get!(lm, msg_iov) as usize as *mut Iovec;
    dm.msg_iovlen = guest_get!(lm, msg_iovlen) as c_int;

    let mut ctrl = MaybeUninit::<Ctrl>::uninit();
    let ctrlp = ctrl.as_mut_ptr().cast::<u8>();
    if guest_get!(lm, msg_control) != 0 && guest_get!(lm, msg_controllen) != 0 {
        dm.msg_control = ctrlp.cast::<c_void>();
        dm.msg_controllen = CTRL_MAX as u32;
    }

    let w = seqpkt_before_recv(fd, flags);
    if w < 0 {
        return w;
    }
    let r = recvmsg(fd, &mut dm, lxrt_msgflags_to_darwin(flags)); // drops TRUNC/CTRUNC
    if r < 0 && errno() == ECONNRESET && lxrt_is_seqpacket(fd) {
        guest_set!(lm, msg_controllen, 0);
        guest_set!(lm, msg_flags, 0);
        return 0; // the peer is gone: end of file (see above)
    }
    if r < 0 {
        return lerr(errno());
    }

    if !dm.msg_name.is_null() && guest_get!(lm, msg_name) != 0 {
        let mut cap: u32 = guest_get!(lm, msg_namelen);
        if is_netlink(fd) {
            nl_kernel_addr(guest_get!(lm, msg_name) as usize as *mut c_void, &mut cap);
        } else {
            addr_to_linux(
                ss.as_ptr(),
                dm.msg_namelen,
                guest_get!(lm, msg_name) as usize as *mut c_void,
                &mut cap,
            );
        }
        guest_set!(lm, msg_namelen, cap);
    }
    if !dm.msg_control.is_null() && dm.msg_controllen != 0 {
        dm.msg_controllen = scm_take_eventfds(ctrlp, dm.msg_controllen);
    }
    if guest_get!(lm, msg_control) != 0 {
        let mut n = cmsg_to_linux(
            ctrlp.cast::<c_void>().cast_const(),
            dm.msg_controllen as usize,
            guest_get!(lm, msg_control) as usize as *mut c_void,
            guest_get!(lm, msg_controllen) as usize,
        );
        if n >= 0 && passcred_get(fd) {
            // SO_PASSCRED: every message carries the sender's credentials.
            let mut uc = MaybeUninit::<LinuxUcred>::uninit();
            let need =
                lalign((size_of::<LinuxCmsghdr>() + size_of::<LinuxUcred>()) as u32) as usize;
            if peer_ucred(fd, uc.as_mut_ptr(), true)
                && (n as usize).wrapping_add(need) <= guest_get!(lm, msg_controllen) as usize
            {
                let lc = (guest_get!(lm, msg_control) as usize as *mut u8)
                    .add(n as usize)
                    .cast::<LinuxCmsghdr>();
                lc.write_unaligned(LinuxCmsghdr {
                    cmsg_len: (size_of::<LinuxCmsghdr>() + size_of::<LinuxUcred>()) as u64,
                    cmsg_level: L_SOL_SOCKET_OPT,
                    cmsg_type: L_SCM_CREDENTIALS,
                });
                ptr::copy_nonoverlapping(
                    uc.as_ptr().cast::<u8>(),
                    lc.cast::<u8>().add(size_of::<LinuxCmsghdr>()),
                    size_of::<LinuxUcred>(),
                );
                n += need as c_long;
            }
        }
        guest_set!(lm, msg_controllen, if n < 0 { 0 } else { n as u64 });
    }
    guest_set!(lm, msg_flags, lxrt_msgflags_to_linux(dm.msg_flags));
    r as c_long
}

// ------------------------------------------------- socket options
//
// The sixth silent number space. SOL_SOCKET is 1 on Linux and 0xffff on
// Darwin, and the SO_* constants underneath it share almost nothing:
//
//   option          Linux   Darwin        option        Linux   Darwin
//   SO_REUSEADDR        2   0x0004        SO_SNDBUF         7   0x1001
//   SO_TYPE             3   0x1008        SO_RCVBUF         8   0x1002
//   SO_ERROR            4   0x1007        SO_KEEPALIVE      9   0x0008
//   SO_BROADCAST        6   0x0020        SO_LINGER        13   0x0080
//
// Passing them through does not fail -- it sets a DIFFERENT option, or the
// same one with a nonsense value. Linux's SO_ERROR (4) reaches Darwin as
// SO_REUSEADDR|... and a getsockopt for the pending error silently returns
// something else.

struct OptMap {
    lopt: c_int,
    dopt: c_int,
}

const fn om(lopt: c_int, dopt: c_int) -> OptMap {
    OptMap { lopt, dopt }
}

static K_SO: [OptMap; 19] = [
    om(1, SO_DEBUG),
    om(2, SO_REUSEADDR),
    om(3, SO_TYPE),
    om(4, SO_ERROR),
    om(5, SO_DONTROUTE),
    om(6, SO_BROADCAST),
    om(7, SO_SNDBUF),
    om(8, SO_RCVBUF),
    om(9, SO_KEEPALIVE),
    om(10, SO_OOBINLINE),
    om(13, SO_LINGER),
    om(15, SO_REUSEPORT),
    om(18, SO_RCVLOWAT),
    om(19, SO_SNDLOWAT),
    om(20, SO_RCVTIMEO),
    om(21, SO_SNDTIMEO),
    om(30, SO_ACCEPTCONN),
    // SO_RCVTIMEO_NEW / SO_SNDTIMEO_NEW (y2038 names): on a 64-bit ABI the
    // same 16-byte timeval as 20/21. FEX turns an i386 guest's _OLD
    // timeouts into these (i386 Android's zygote: ENOPROTOOPT on every
    // connection before this).
    om(66, SO_RCVTIMEO),
    om(67, SO_SNDTIMEO),
];

// Returns false when the option has no Darwin equivalent -- SO_PASSCRED and
// SO_PEERCRED above all, which Darwin only offers as LOCAL_PEERCRED on a
// different level with a different structure.
// IPPROTO_IP (0), IPPROTO_TCP (6) and IPPROTO_IPV6 (41) are the same LEVEL
// numbers on both systems, but most option numbers under them are not (the
// old comment here claimed they agreed; Xvnc's IPV6_V6ONLY -- Linux 26,
// Darwin 27 -- measured otherwise).
static K_IP: [OptMap; 12] = [
    om(1, IP_TOS),
    om(2, IP_TTL),
    om(3, IP_HDRINCL),
    om(4, IP_OPTIONS),
    om(8, IP_PKTINFO),
    om(12, IP_RECVTTL),
    om(13, IP_RECVTOS),
    om(32, IP_MULTICAST_IF),
    om(33, IP_MULTICAST_TTL),
    om(34, IP_MULTICAST_LOOP),
    om(35, IP_ADD_MEMBERSHIP),
    om(36, IP_DROP_MEMBERSHIP),
];
static K_IPV6: [OptMap; 10] = [
    om(16, IPV6_UNICAST_HOPS),
    om(17, IPV6_MULTICAST_IF),
    om(18, IPV6_MULTICAST_HOPS),
    om(19, IPV6_MULTICAST_LOOP),
    om(20, IPV6_JOIN_GROUP),
    om(21, IPV6_LEAVE_GROUP),
    om(26, IPV6_V6ONLY),
    om(49, 61 /* Darwin IPV6_RECVPKTINFO (RFC 3542) */),
    om(50, 46 /* IPV6_PKTINFO */),
    om(67, IPV6_TCLASS),
];
static K_TCP: [OptMap; 6] = [
    om(1, TCP_NODELAY),
    om(2, TCP_MAXSEG),
    om(4, TCP_KEEPALIVE /* KEEPIDLE */),
    om(5, TCP_KEEPINTVL),
    om(6, TCP_KEEPCNT),
    om(23, TCP_FASTOPEN),
];

// Linux-only knobs with no Darwin counterpart whose absence changes nothing a
// program can observe here; accepted and ignored rather than failed, since
// programs treat ENOPROTOOPT on them as fatal (Xvnc) or log noise.
#[no_mangle]
pub extern "C" fn lxrt_sockopt_ignorable(llevel: c_int, lopt: c_int) -> bool {
    if llevel == 0 {
        return lopt == 10 /* IP_MTU_DISCOVER */
            || lopt == 11 /* IP_RECVERR */
            || lopt == 15 /* IP_FREEBIND */
            || lopt == 49 /* IP_BIND_ADDRESS_NO_PORT */;
    }
    if llevel == 6 {
        return lopt == 3 /* TCP_CORK */
            || lopt == 9 /* TCP_DEFER_ACCEPT */
            || lopt == 12 /* TCP_QUICKACK */
            || lopt == 18 /* TCP_USER_TIMEOUT */
            || lopt == 13 /* TCP_CONGESTION */;
    }
    if llevel == 41 {
        return lopt == 23 /* IPV6_MTU_DISCOVER */ || lopt == 25 /* IPV6_RECVERR */;
    }
    if llevel == L_SOL_SOCKET_OPT {
        return lopt == 12 /* SO_PRIORITY */
            || lopt == 25 /* SO_BINDTODEVICE */
            || lopt == 26 /* SO_ATTACH_FILTER */
            || lopt == 29 /* SO_TIMESTAMP */
            || lopt == 32 /* SO_SNDBUFFORCE */
            || lopt == 33 /* SO_RCVBUFFORCE */
            || lopt == 46 /* SO_BUSY_POLL */
            || lopt == 36 /* SO_MARK */;
    }
    false
}

fn map_opt(m: &[OptMap], lopt: c_int, dopt: &mut c_int) -> bool {
    for entry in m {
        if entry.lopt == lopt {
            *dopt = entry.dopt;
            return true;
        }
    }
    false
}

fn sockopt_to_darwin(llevel: c_int, lopt: c_int, dlevel: &mut c_int, dopt: &mut c_int) -> bool {
    if llevel != L_SOL_SOCKET_OPT {
        *dlevel = llevel;
        return match llevel {
            0 => map_opt(&K_IP, lopt, dopt),
            6 => map_opt(&K_TCP, lopt, dopt),
            41 => map_opt(&K_IPV6, lopt, dopt),
            _ => {
                *dopt = lopt; // other protocols: pass through
                true
            }
        };
    }
    *dlevel = SOL_SOCKET;
    map_opt(&K_SO, lopt, dopt)
}

// SO_RCVTIMEO/SO_SNDTIMEO carry a timeval, and Linux's has a 64-bit tv_usec
// where Darwin's has 32 bits plus padding. Same size, different layout.
#[repr(C)]
struct LinuxTimevalOpt {
    tv_sec: i64,
    tv_usec: i64,
}
// SO_LINGER carries two ints on both, and they agree.

// Say which option, once each (lxrt_setsockopt).
static SAID: [AtomicU32; 64] = [const { AtomicU32::new(0) }; 64];

#[no_mangle]
pub unsafe extern "C" fn lxrt_setsockopt(
    fd: c_int,
    llevel: c_int,
    lopt: c_int,
    val: *const c_void,
    len: c_uint,
) -> c_long {
    if llevel == L_SOL_SOCKET_OPT && lopt == L_SO_PASSCRED {
        if val.is_null() || len < 4 {
            return lerr(EINVAL);
        }
        passcred_set(fd, val.cast::<i32>().read_unaligned() != 0);
        return 0;
    }
    // The uevent monitor's socket filter (SO_ATTACH_FILTER, a BPF program),
    // SO_RCVBUFFORCE, SOL_NETLINK memberships: nothing arrives to filter.
    if is_netlink(fd) {
        return 0;
    }
    let mut dlevel: c_int = 0;
    let mut dopt: c_int = 0;
    if !sockopt_to_darwin(llevel, lopt, &mut dlevel, &mut dopt) {
        return if lxrt_sockopt_ignorable(llevel, lopt) {
            0
        } else {
            lerr(ENOPROTOOPT)
        };
    }

    if dlevel == SOL_SOCKET && (dopt == SO_RCVTIMEO || dopt == SO_SNDTIMEO) {
        if val.is_null() || (len as usize) < size_of::<LinuxTimevalOpt>() {
            return lerr(EINVAL);
        }
        let lt = val.cast::<LinuxTimevalOpt>().read_unaligned();
        let tv = Timeval {
            tv_sec: lt.tv_sec,
            tv_usec: lt.tv_usec as i32,
        };
        return if setsockopt(
            fd,
            dlevel,
            dopt,
            ptr::addr_of!(tv).cast::<c_void>(),
            size_of::<Timeval>() as u32,
        ) != 0
        {
            lerr(errno())
        } else {
            0
        };
    }
    if setsockopt(fd, dlevel, dopt, val, len) == 0 {
        return 0;
    }
    let e = errno();
    // Linux clamps an oversized SO_SNDBUF/SO_RCVBUF to net.core.[wr]mem_max
    // and succeeds; Darwin refuses anything above kern.ipc.maxsockbuf. The
    // Steam client asserts on the refusal ("Unable to set default socket
    // options, error 22"). Clamp the same way: the largest size accepted.
    if dlevel == SOL_SOCKET
        && (dopt == SO_SNDBUF || dopt == SO_RCVBUF)
        && (e == EINVAL || e == ENOBUFS)
        && !val.is_null()
        && len >= 4
    {
        let want = val.cast::<c_int>().read_unaligned();
        let mut v = want / 2;
        while v >= 4096 {
            if setsockopt(
                fd,
                dlevel,
                dopt,
                ptr::addr_of!(v).cast::<c_void>(),
                size_of::<c_int>() as u32,
            ) == 0
            {
                return 0;
            }
            v /= 2;
        }
        // Below the minimum (the Steam client asks for 0): Linux rounds up
        // to SOCK_MIN_SNDBUF/SOCK_MIN_RCVBUF, a few KiB.
        if want < 4608 {
            let v: c_int = 4608;
            if setsockopt(
                fd,
                dlevel,
                dopt,
                ptr::addr_of!(v).cast::<c_void>(),
                size_of::<c_int>() as u32,
            ) == 0
            {
                return 0;
            }
        }
    }
    // Say which option, once each: "Unable to set default socket options,
    // error 22" from the Steam client names none.
    let key = ((llevel as u32) << 16) ^ (lopt as u32);
    let mut fresh = true;
    for said in &SAID {
        let cur = said.load(Ordering::SeqCst);
        if cur == key.wrapping_add(1) {
            fresh = false;
            break;
        }
        if cur == 0
            && said
                .compare_exchange(0, key.wrapping_add(1), Ordering::SeqCst, Ordering::SeqCst)
                .is_ok()
        {
            break;
        }
    }
    if fresh {
        fprintf(
            lxrt_trace_stream(),
            c"[lxrt] setsockopt(level %d, opt %d, len %u, int %d) -> darwin (%d, %d): %s\n".as_ptr(),
            llevel,
            lopt,
            len,
            if !val.is_null() && len >= 4 {
                val.cast::<c_int>().read_unaligned()
            } else {
                0
            },
            dlevel,
            dopt,
            strerror(e),
        );
    }
    lerr(e)
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_getsockopt(
    fd: c_int,
    llevel: c_int,
    lopt: c_int,
    val: *mut c_void,
    len: *mut c_uint,
) -> c_long {
    if llevel == L_SOL_SOCKET_OPT && (lopt == L_SO_PASSCRED || lopt == L_SO_PEERCRED) {
        if val.is_null() || len.is_null() {
            return lerr(EFAULT);
        }
        if lopt == L_SO_PASSCRED {
            if len.read_unaligned() < 4 {
                return lerr(EINVAL);
            }
            val.cast::<i32>().write_unaligned(i32::from(passcred_get(fd)));
            len.write_unaligned(4);
            return 0;
        }
        let mut uc = MaybeUninit::<LinuxUcred>::uninit();
        if !peer_ucred(fd, uc.as_mut_ptr(), false) {
            return lerr(errno());
        }
        if (len.read_unaligned() as usize) < size_of::<LinuxUcred>() {
            return lerr(EINVAL);
        }
        ptr::copy_nonoverlapping(
            uc.as_ptr().cast::<u8>(),
            val.cast::<u8>(),
            size_of::<LinuxUcred>(),
        );
        len.write_unaligned(size_of::<LinuxUcred>() as c_uint);
        return 0;
    }
    if llevel == L_SOL_SOCKET_OPT && (lopt == 38 || lopt == 39) {
        // SO_PROTOCOL (38) and SO_DOMAIN (39): read-only, Linux 2.6.32+.
        // Darwin has neither; the answer follows from the socket's address
        // family and type. Android's BlockGuardOs asks SO_DOMAIN of every
        // accepted socket (isInetSocket), and the zygote died on the
        // ENOPROTOOPT with system_server's first connection ("accept failed:
        // ENOPROTOOPT", MEASURED at stage 28).
        if val.is_null() || len.is_null() {
            return lerr(EFAULT);
        }
        if len.read_unaligned() < 4 {
            return lerr(EINVAL);
        }
        let v: i32;
        if is_netlink(fd) {
            v = if lopt == 39 { L_AF_NETLINK } else { 0 };
        } else {
            let mut ss = MaybeUninit::<SockaddrStorage>::uninit();
            let mut sl = size_of::<SockaddrStorage>() as u32;
            let mut kind: c_int = 0;
            let mut tl = size_of::<c_int>() as u32;
            if getsockname(fd, ss.as_mut_ptr().cast::<c_void>(), &mut sl) != 0
                || getsockopt(
                    fd,
                    SOL_SOCKET,
                    SO_TYPE,
                    ptr::addr_of_mut!(kind).cast::<c_void>(),
                    &mut tl,
                ) != 0
            {
                return lerr(errno());
            }
            let ss_family = c_int::from((*ss.as_ptr()).ss_family);
            let fam = if ss_family == AF_INET6 { 10 } else { ss_family }; // Darwin AF_INET6 is 30
            if lopt == 39 {
                v = fam;
            } else if fam == AF_UNIX {
                v = 0;
            } else {
                // IPPROTO_TCP/UDP
                v = if kind == SOCK_STREAM {
                    6
                } else if kind == SOCK_DGRAM {
                    17
                } else {
                    0
                };
            }
        }
        val.cast::<i32>().write_unaligned(v);
        len.write_unaligned(4);
        return 0;
    }
    let mut dlevel: c_int = 0;
    let mut dopt: c_int = 0;
    if !sockopt_to_darwin(llevel, lopt, &mut dlevel, &mut dopt) {
        if lxrt_sockopt_ignorable(llevel, lopt)
            && !val.is_null()
            && !len.is_null()
            && len.read_unaligned() >= 4
        {
            val.cast::<i32>().write_unaligned(0);
            len.write_unaligned(4);
            return 0;
        }
        return lerr(ENOPROTOOPT);
    }

    if dlevel == SOL_SOCKET && (dopt == SO_RCVTIMEO || dopt == SO_SNDTIMEO) {
        if val.is_null()
            || len.is_null()
            || (len.read_unaligned() as usize) < size_of::<LinuxTimevalOpt>()
        {
            return lerr(EINVAL);
        }
        let mut tv = MaybeUninit::<Timeval>::zeroed();
        let mut tl = size_of::<Timeval>() as u32;
        if getsockopt(fd, dlevel, dopt, tv.as_mut_ptr().cast::<c_void>(), &mut tl) != 0 {
            return lerr(errno());
        }
        let tv = &*tv.as_ptr();
        val.cast::<LinuxTimevalOpt>().write_unaligned(LinuxTimevalOpt {
            tv_sec: tv.tv_sec,
            tv_usec: i64::from(tv.tv_usec),
        });
        len.write_unaligned(size_of::<LinuxTimevalOpt>() as c_uint);
        return 0;
    }

    let mut dl: u32 = if len.is_null() { 0 } else { len.read_unaligned() };
    if getsockopt(fd, dlevel, dopt, val, &mut dl) != 0 {
        return lerr(errno());
    }
    if !len.is_null() {
        len.write_unaligned(dl);
    }
    // The seqpacket mark is the runtime's, not the guest's.
    if dlevel == SOL_SOCKET && dopt == SO_LINGER && !val.is_null() && dl as usize >= size_of::<Linger>()
    {
        let l = val.cast::<Linger>().read_unaligned();
        if l.l_onoff != 0 && l.l_linger == SEQPKT_LINGER {
            ptr::write_bytes(val.cast::<u8>(), 0, size_of::<Linger>());
        }
    }
    0
}
