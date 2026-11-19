// Android's system property service as a host process: `lxrun
// --property-service <dir> <root> [--daemon]` (runtime/props.h).
//
// What init's property service does at boot and afterwards, ported from the
// sources of the image's own Android 11 (LineageOS 18.1, VERIFIED IN SOURCE
// against LineageOS/android_system_core lineage-18.1 9242067 and
// LineageOS/android_bionic lineage-18.1 b67b033):
//
//   property_info    the serialized trie of property_contexts that maps a
//                    name to its context (area file) and type:
//                    property_service/libpropertyinfoserializer
//                    (trie_builder.cpp, trie_serializer.cpp,
//                    property_info_file.cpp), read back as
//                    libpropertyinfoparser/property_info_parser.cpp does;
//   the areas        one 128 KiB file per context plus properties_serial,
//                    bionic's prop_area / prop_bt / prop_info layout
//                    (libc/system_properties/prop_area.cpp, prop_info.cpp),
//                    written with SystemProperties::Update/Add's serial
//                    protocol (system_properties.cpp): dirty bit, backup
//                    area, release fences, a futex wake on the property's
//                    serial and on the global one;
//   boot values      init/property_service.cpp PropertyInit: the "kernel"
//                    boot properties (LXRT_PROPERTY_BOOTARGS stands for the
//                    command line), PropertyLoadBootDefaults' files in its
//                    order with its import/filter/override rules, the derived
//                    ro.product.* and ro.build.fingerprint, then
//                    ro.property_service.version and the persistent ones;
//   setprop          handle_property_set_fd: PROP_MSG_SETPROP and
//                    PROP_MSG_SETPROP2 on <dir>/property_service, with
//                    IsLegalPropertyName/Value, the type check, ro.* written
//                    once, persist.* stored (init/persistent_properties.cpp's
//                    protobuf file, by default the guest's own
//                    /data/property/persistent_properties).
//
// Not done, and said so: no SELinux (the MAC checks of CheckPermissions and
// CanReadProperty are not applied; every area is readable), no init to
// receive ctl.* (refused with PROP_ERROR_HANDLE_CONTROL_MESSAGE and logged),
// no property triggers (nothing runs "on property:..."), no
// vendor_load_properties hook, no restorecon (selinux.restorecon_recursive is
// set at once). Every guest is the Mac user, so "ro.* is write-once" is the
// protocol's rule, not a security boundary.
//
// Guests map the areas read-only and MAP_SHARED; this process writes through
// its own MAP_SHARED mapping of the same files, so both see the same pages
// (the host page cache), and __ulock_wake with the process-shared flavour
// reaches the guests' FUTEX_WAIT on those words (runtime/futex_ops.c parks
// shared futexes in shared mappings on that flavour). MEASURED before this
// file was written: two unrelated processes mapping one file, the waiter
// read-only, woke on UL_COMPARE_AND_WAIT_SHARED (benchmarks/stage26-android-
// properties.txt).
//
// The state is the area files, not this process: a service that left
// (LXRT_PROPERTY_IDLE) and is started again adopts them as they are. The
// property state of an Android root is reset ("reboot") by removing the
// directory.
//
// Strings are byte strings throughout (property files, names and values are
// not required to be UTF-8); wherever the C original read a buffer as a C
// string, the bytes are cut at the first NUL here too.

use std::ffi::{c_char, c_int, c_long, c_void, CStr};
use std::io::Write as _;
use std::mem::{size_of, zeroed};
use std::ptr;
use std::sync::atomic::{fence, AtomicI32, AtomicU32, Ordering};

const UL_COMPARE_AND_WAIT_SHARED: u32 = 3;
const ULF_NO_ERRNO: u32 = 0x0100_0000;
const ULF_WAKE_ALL: u32 = 0x0000_0100;

// bionic libc/system_properties (prop_area.cpp, prop_info.h) and
// libc/include/sys/_system_properties.h
const PA_SIZE: usize = 128 * 1024;
const PROP_AREA_MAGIC: u32 = 0x504f_5250;
const PROP_AREA_VERSION: u32 = 0xfc6e_d0ab;
const PA_HDR: usize = 128; // sizeof(prop_area): 4 + 4 + 4 + 4 + 28 * 4
const PA_DATA_SIZE: u32 = (PA_SIZE - PA_HDR) as u32;
const PROP_BT_HDR: u32 = 20; // namelen, prop, left, right, children
const PROP_INFO_HDR: u32 = 96; // serial + value[PROP_VALUE_MAX]
const PROP_VALUE_MAX: u32 = 92;
const PROP_NAME_MAX: usize = 32;
const LONG_FLAG: u32 = 1 << 16;
const LONG_OFFSET_AT: usize = 4 + 56; // long_property.offset inside prop_info
const K_LONG_LEGACY_ERROR: &[u8] = b"Must use __system_property_read_callback() to read";

const PROP_MSG_SETPROP: u32 = 1;
const PROP_MSG_SETPROP2: u32 = 0x0002_0001;
const PROP_SUCCESS: u32 = 0;
const PROP_ERROR_READ_CMD: u32 = 0x0004;
const PROP_ERROR_READ_DATA: u32 = 0x0008;
const PROP_ERROR_READ_ONLY_PROPERTY: u32 = 0x000B;
const PROP_ERROR_INVALID_NAME: u32 = 0x0010;
const PROP_ERROR_INVALID_VALUE: u32 = 0x0014;
const PROP_ERROR_INVALID_CMD: u32 = 0x001B;
const PROP_ERROR_HANDLE_CONTROL_MESSAGE: u32 = 0x0020;
const PROP_ERROR_SET_FAILED: u32 = 0x0024;

// Darwin (arm64) constants and structure layouts.
const PATH_MAX: usize = 1024;
const NAME_MAX: usize = 255;
const O_RDONLY: c_int = 0;
const O_WRONLY: c_int = 1;
const O_RDWR: c_int = 2;
const O_NONBLOCK: c_int = 0x4;
const O_APPEND: c_int = 0x8;
const O_NOFOLLOW: c_int = 0x100;
const O_CREAT: c_int = 0x200;
const O_TRUNC: c_int = 0x400;
const O_EXCL: c_int = 0x800;
const O_CLOEXEC: c_int = 0x0100_0000;
const F_SETFD: c_int = 2;
const F_SETFL: c_int = 4;
const FD_CLOEXEC: c_int = 1;
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const MAP_SHARED: c_int = 1;
const AF_UNIX: u8 = 1;
const SOCK_STREAM: c_int = 1;
const SOCK_DGRAM: c_int = 2;
const MSG_DONTWAIT: c_int = 0x80;
const SOL_LOCAL: c_int = 0;
const LOCAL_PEERCRED: c_int = 0x001;
const LOCAL_PEERPID: c_int = 0x002;
const POLLIN: i16 = 0x0001;
const SIGINT: c_int = 2;
const SIGPIPE: c_int = 13;
const SIGTERM: c_int = 15;
const SIG_IGN: usize = 1;
const EINTR: c_int = 4;
const EAGAIN: c_int = 35;
const LOCK_EX: c_int = 2;
const LOCK_NB: c_int = 4;
const R_OK: c_int = 4;
const DT_REG: u8 = 8;
const S_IFMT: u16 = 0o170000;
const S_IFDIR: u16 = 0o040000;
const S_IFREG: u16 = 0o100000;
const S_IFSOCK: u16 = 0o140000;
const CLOCK_REALTIME: c_int = 0;
const CLOCK_MONOTONIC: c_int = 6;
const IOLBF: c_int = 1;

#[repr(C)]
#[derive(Clone, Copy)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: c_long,
}

#[repr(C)]
struct Tm {
    tm_sec: c_int,
    tm_min: c_int,
    tm_hour: c_int,
    tm_mday: c_int,
    tm_mon: c_int,
    tm_year: c_int,
    tm_wday: c_int,
    tm_yday: c_int,
    tm_isdst: c_int,
    tm_gmtoff: c_long,
    tm_zone: *mut c_char,
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

#[repr(C)]
struct Dirent {
    d_ino: u64,
    d_seekoff: u64,
    d_reclen: u16,
    d_namlen: u16,
    d_type: u8,
    d_name: [c_char; 1024],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SockaddrUn {
    sun_len: u8,
    sun_family: u8,
    sun_path: [u8; 104],
}

#[repr(C)]
struct Xucred {
    cr_version: u32,
    cr_uid: u32,
    cr_ngroups: i16,
    cr_groups: [u32; 16],
}

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: i16,
    revents: i16,
}

const _: [(); 144] = [(); size_of::<Stat>()];
const _: [(); 56] = [(); size_of::<Tm>()];
const _: [(); 106] = [(); size_of::<SockaddrUn>()];
const _: [(); 76] = [(); size_of::<Xucred>()];
const _: [(); 8] = [(); size_of::<PollFd>()];

#[link(name = "System")]
unsafe extern "C" {
    #[allow(non_upper_case_globals)]
    static __stderrp: *mut c_void;

    fn __ulock_wake(operation: u32, addr: *mut c_void, wake_value: u64) -> c_int;
    fn __error() -> *mut c_int;
    fn _exit(status: c_int) -> !;
    fn accept(fd: c_int, addr: *mut c_void, len: *mut u32) -> c_int;
    fn access(path: *const c_char, mode: c_int) -> c_int;
    fn atoi(s: *const c_char) -> c_int;
    fn bind(fd: c_int, addr: *const SockaddrUn, len: u32) -> c_int;
    fn chmod(path: *const c_char, mode: u16) -> c_int;
    fn clock_gettime(clock: c_int, ts: *mut Timespec) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn closedir(dir: *mut c_void) -> c_int;
    fn dirfd(dir: *mut c_void) -> c_int;
    fn dup2(from: c_int, to: c_int) -> c_int;
    fn fchmod(fd: c_int, mode: u16) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn flock(fd: c_int, operation: c_int) -> c_int;
    fn fork() -> c_int;
    fn fstat(fd: c_int, st: *mut Stat) -> c_int;
    fn fsync(fd: c_int) -> c_int;
    fn ftruncate(fd: c_int, length: i64) -> c_int;
    fn fwrite(data: *const c_void, size: usize, count: usize, stream: *mut c_void) -> usize;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn getpid() -> c_int;
    fn getsockopt(
        fd: c_int,
        level: c_int,
        name: c_int,
        value: *mut c_void,
        len: *mut u32,
    ) -> c_int;
    fn getuid() -> u32;
    fn listen(fd: c_int, backlog: c_int) -> c_int;
    fn localtime_r(clock: *const i64, result: *mut Tm) -> *mut Tm;
    fn lstat(path: *const c_char, st: *mut Stat) -> c_int;
    fn mkdir(path: *const c_char, mode: u16) -> c_int;
    fn mmap(
        addr: *mut c_void,
        len: usize,
        prot: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(addr: *mut c_void, len: usize) -> c_int;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn openat(dirfd: c_int, path: *const c_char, flags: c_int, ...) -> c_int;
    fn opendir(path: *const c_char) -> *mut c_void;
    fn poll(fds: *mut PollFd, count: u32, timeout: c_int) -> c_int;
    fn read(fd: c_int, buf: *mut c_void, count: usize) -> isize;
    fn readdir(dir: *mut c_void) -> *mut Dirent;
    fn readlink(path: *const c_char, buf: *mut c_char, size: usize) -> isize;
    #[link_name = "realpath$DARWIN_EXTSN"]
    fn realpath(path: *const c_char, resolved: *mut c_char) -> *mut c_char;
    fn recv(fd: c_int, buf: *mut c_void, len: usize, flags: c_int) -> isize;
    fn rename(from: *const c_char, to: *const c_char) -> c_int;
    fn rmdir(path: *const c_char) -> c_int;
    fn send(fd: c_int, buf: *const c_void, len: usize, flags: c_int) -> isize;
    fn sendto(
        fd: c_int,
        buf: *const c_void,
        len: usize,
        flags: c_int,
        addr: *const SockaddrUn,
        addrlen: u32,
    ) -> isize;
    fn setsid() -> c_int;
    fn setvbuf(stream: *mut c_void, buf: *mut c_char, mode: c_int, size: usize) -> c_int;
    fn signal(sig: c_int, handler: usize) -> usize;
    fn socket(domain: c_int, kind: c_int, protocol: c_int) -> c_int;
    fn stat(path: *const c_char, st: *mut Stat) -> c_int;
    fn strerror(error: c_int) -> *mut c_char;
    fn strtod(s: *const c_char, end: *mut *mut c_char) -> f64;
    fn strtoll(s: *const c_char, end: *mut *mut c_char, base: c_int) -> i64;
    fn strtoull(s: *const c_char, end: *mut *mut c_char, base: c_int) -> u64;
    fn time(out: *mut i64) -> i64;
    fn unlink(path: *const c_char) -> c_int;
    fn unlinkat(dirfd: c_int, path: *const c_char, flags: c_int) -> c_int;
    fn usleep(micros: u32) -> c_int;
    fn write(fd: c_int, buf: *const c_void, count: usize) -> isize;
}

struct Area {
    ctx: Vec<u8>,
    base: *mut u8,
}

// The service's state: what the C original kept in file-scope statics. It
// lives in a static here too; the service is one thread.
struct Svc {
    dir: Vec<u8>,          // the service's directory
    init_sa: SockaddrUn,   // LXRT_PROPERTY_INIT: an init to tell (see init_notify)
    init_fd: c_int,
    root: Vec<u8>,         // the Android root (host path)
    persist: Vec<u8>,      // persistent property file (host path)
    persist_loaded: bool,
    vendor_api: c_int,
    pi: Vec<u8>,           // property_info bytes (a private copy)
    pi_size: u32,
    areas: Vec<Area>,      // one per context, property_info's order
    serial_area: *mut u8,  // properties_serial
}

static mut G: Svc = Svc {
    dir: Vec::new(),
    init_sa: SockaddrUn {
        sun_len: 0,
        sun_family: 0,
        sun_path: [0; 104],
    },
    init_fd: -1,
    root: Vec::new(),
    persist: Vec::new(),
    persist_loaded: false,
    vendor_api: 30,
    pi: Vec::new(),
    pi_size: 0,
    areas: Vec::new(),
    serial_area: ptr::null_mut(),
};
static G_STOP: AtomicI32 = AtomicI32::new(0);

// ---------------------------------------------------------- small helpers

fn errno() -> c_int {
    unsafe { *__error() }
}

fn set_errno(value: c_int) {
    unsafe { *__error() = value }
}

fn errstr(error: c_int) -> Vec<u8> {
    unsafe {
        let text = strerror(error);
        if text.is_null() {
            Vec::new()
        } else {
            CStr::from_ptr(text).to_bytes().to_vec()
        }
    }
}

// The C string a buffer holds: its bytes up to the first NUL.
fn cstr(bytes: &[u8]) -> &[u8] {
    match bytes.iter().position(|&c| c == 0) {
        Some(end) => &bytes[..end],
        None => bytes,
    }
}

unsafe fn cstr_ptr<'a>(p: *const u8) -> &'a [u8] {
    CStr::from_ptr(p.cast()).to_bytes()
}

// Call `f` with a NUL-terminated copy of `path`; errno is what `f` left.
fn with_path<R>(path: &[u8], f: impl FnOnce(*const c_char) -> R) -> R {
    let mut z = cstr(path).to_vec();
    z.push(0);
    let result = f(z.as_ptr().cast());
    let saved = errno();
    drop(z);
    set_errno(saved);
    result
}

fn isspace(c: u8) -> bool {
    c == b' ' || (9..=13).contains(&c)
}

// strncmp over byte strings: the end of a slice is its NUL.
fn strncmp(a: &[u8], b: &[u8], n: usize) -> c_int {
    for i in 0..n {
        let x = a.get(i).copied().unwrap_or(0);
        let y = b.get(i).copied().unwrap_or(0);
        if x != y {
            return x as c_int - y as c_int;
        }
        if x == 0 {
            return 0;
        }
    }
    0
}

fn find_byte(s: &[u8], c: u8) -> Option<usize> {
    s.iter().position(|&x| x == c)
}

fn find_sub(s: &[u8], needle: &[u8]) -> Option<usize> {
    if needle.is_empty() {
        return Some(0);
    }
    s.windows(needle.len()).position(|w| w == needle)
}

fn env(name: &[u8]) -> Option<Vec<u8>> {
    with_path(name, |n| unsafe {
        let value = getenv(n);
        if value.is_null() {
            None
        } else {
            Some(CStr::from_ptr(value).to_bytes().to_vec())
        }
    })
}

fn c_atoi(s: &[u8]) -> c_int {
    with_path(s, |p| unsafe { atoi(p) })
}

fn sys_open(path: &[u8], flags: c_int, mode: c_int) -> c_int {
    with_path(path, |p| unsafe { open(p, flags, mode) })
}
fn sys_unlink(path: &[u8]) -> c_int {
    with_path(path, |p| unsafe { unlink(p) })
}
fn sys_rename(from: &[u8], to: &[u8]) -> c_int {
    with_path(from, |f| with_path(to, |t| unsafe { rename(f, t) }))
}
fn sys_stat(path: &[u8], st: &mut Stat) -> c_int {
    with_path(path, |p| unsafe { stat(p, st) })
}
fn sys_lstat(path: &[u8], st: &mut Stat) -> c_int {
    with_path(path, |p| unsafe { lstat(p, st) })
}
fn sys_mkdir(path: &[u8], mode: u16) -> c_int {
    with_path(path, |p| unsafe { mkdir(p, mode) })
}
fn sys_chmod(path: &[u8], mode: u16) -> c_int {
    with_path(path, |p| unsafe { chmod(p, mode) })
}
fn sys_opendir(path: &[u8]) -> *mut c_void {
    with_path(path, |p| unsafe { opendir(p) })
}

fn new_stat() -> Stat {
    unsafe { zeroed() }
}
fn is_reg(st: &Stat) -> bool {
    st.st_mode & S_IFMT == S_IFREG
}
fn is_dir(st: &Stat) -> bool {
    st.st_mode & S_IFMT == S_IFDIR
}
fn is_sock(st: &Stat) -> bool {
    st.st_mode & S_IFMT == S_IFSOCK
}

fn monotonic() -> Timespec {
    let mut ts = Timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    unsafe {
        clock_gettime(CLOCK_MONOTONIC, &mut ts);
    }
    ts
}

// What snprintf leaves in a buffer of `cap` bytes, and the length it
// returns (the untruncated one).
fn snp(cap: usize, mut full: Vec<u8>) -> (Vec<u8>, usize) {
    let n = full.len();
    if n >= cap {
        full.truncate(cap.saturating_sub(1));
    }
    (full, n)
}

trait Piece {
    fn put(&self, out: &mut Vec<u8>);
}
impl Piece for str {
    fn put(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(self.as_bytes());
    }
}
impl Piece for [u8] {
    fn put(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(self);
    }
}
impl Piece for Vec<u8> {
    fn put(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(self);
    }
}
impl<const N: usize> Piece for [u8; N] {
    fn put(&self, out: &mut Vec<u8>) {
        out.extend_from_slice(self);
    }
}
impl Piece for i32 {
    fn put(&self, out: &mut Vec<u8>) {
        let _ = write!(out, "{}", self);
    }
}
impl Piece for u32 {
    fn put(&self, out: &mut Vec<u8>) {
        let _ = write!(out, "{}", self);
    }
}
// "%.1f"
struct Fixed1(f64);
impl Piece for Fixed1 {
    fn put(&self, out: &mut Vec<u8>) {
        let _ = write!(out, "{:.1}", self.0);
    }
}
impl<T: Piece + ?Sized> Piece for &T {
    fn put(&self, out: &mut Vec<u8>) {
        (**self).put(out);
    }
}

macro_rules! cat {
    ($($piece:expr),* $(,)?) => {{
        #[allow(unused_mut)]
        let mut bytes: Vec<u8> = Vec::new();
        $( Piece::put(&$piece, &mut bytes); )*
        bytes
    }};
}

// ------------------------------------------------------------------ logging

fn stderr_write(bytes: &[u8]) {
    unsafe {
        fwrite(bytes.as_ptr().cast(), 1, bytes.len(), __stderrp);
    }
}

fn plog_line(message: &[u8]) {
    let mut ts = Timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    let mut tm: Tm = unsafe { zeroed() };
    unsafe {
        clock_gettime(CLOCK_REALTIME, &mut ts);
        localtime_r(&ts.tv_sec, &mut tm);
    }
    let mut line: Vec<u8> = Vec::with_capacity(message.len() + 40);
    let _ = write!(
        line,
        "{:02}:{:02}:{:02}.{:03} [{}] ",
        tm.tm_hour,
        tm.tm_min,
        tm.tm_sec,
        ts.tv_nsec / 1_000_000,
        unsafe { getpid() }
    );
    line.extend_from_slice(message);
    line.push(b'\n');
    stderr_write(&line);
}

macro_rules! plog {
    ($($piece:expr),* $(,)?) => {
        plog_line(&cat!($($piece),*))
    };
}

fn set_error(error: &mut Vec<u8>, text: &str) {
    error.clear();
    error.extend_from_slice(text.as_bytes());
}

// ------------------------------------------------ property_contexts -> trie
//
// libpropertyinfoserializer. Entries carry their context and type strings;
// the serializer writes those strings' indexes in two sorted sets.

#[derive(Clone)]
struct PEntry {
    name: Vec<u8>,
    ctx: Option<Vec<u8>>,
    typ: Option<Vec<u8>>,
}

struct TNode {
    name: Vec<u8>,
    ctx: Option<Vec<u8>>, // the node's own match ("a.b." lines)
    typ: Option<Vec<u8>>,
    kids: Vec<TNode>,
    pre: Vec<PEntry>, // prefix matches ending at this node
    ex: Vec<PEntry>,  // exact matches ending at this node
}

impl TNode {
    fn new(name: &[u8]) -> TNode {
        TNode {
            name: name.to_vec(),
            ctx: None,
            typ: None,
            kids: Vec::new(),
            pre: Vec::new(),
            ex: Vec::new(),
        }
    }
}

fn strset_add(set: &mut Vec<Vec<u8>>, s: &[u8]) -> Vec<u8> {
    if !set.iter().any(|have| have.as_slice() == s) {
        set.push(s.to_vec());
    }
    s.to_vec()
}

struct Builder {
    root: TNode,
    ctxs: Vec<Vec<u8>>,
    types: Vec<Vec<u8>>,
}

fn tnode_child<'a>(n: &'a mut TNode, name: &[u8], add: bool) -> Option<&'a mut TNode> {
    if let Some(i) = n.kids.iter().position(|kid| kid.name.as_slice() == name) {
        return n.kids.get_mut(i);
    }
    if !add {
        return None;
    }
    n.kids.push(TNode::new(name));
    n.kids.last_mut()
}

// TrieBuilder::AddToTrie. Err (the message) on a duplicate, as init.
fn builder_add(
    b: &mut Builder,
    name: &[u8],
    ctx: &[u8],
    typ: &[u8],
    exact: bool,
) -> Result<(), Vec<u8>> {
    const ERRN: usize = 256;
    let c = strset_add(&mut b.ctxs, ctx);
    let t = strset_add(&mut b.types, typ);
    // android::base::Split(name, "."), a trailing "" dropped (ends_with_dot).
    let pieces: Vec<&[u8]> = name.split(|&ch| ch == b'.').take(256).collect();
    let mut np = pieces.len();
    let mut ends_with_dot = false;
    if np > 0 && pieces[np - 1].is_empty() {
        ends_with_dot = true;
        np -= 1;
    }
    if np == 0 {
        return Err(snp(ERRN, cat!("Empty property entry '", name, "'")).0);
    }
    let mut cur = &mut b.root;
    for piece in &pieces[..np - 1] {
        cur = match tnode_child(cur, piece, true) {
            Some(child) => child,
            None => return Err(Vec::new()),
        };
    }
    let last = pieces[np - 1];
    if exact {
        if cur.ex.iter().any(|e| e.name.as_slice() == last) {
            return Err(snp(ERRN, cat!("Duplicate exact match detected for '", name, "'")).0);
        }
        cur.ex.push(PEntry {
            name: last.to_vec(),
            ctx: Some(c),
            typ: Some(t),
        });
    } else if !ends_with_dot {
        if cur.pre.iter().any(|e| e.name.as_slice() == last) {
            return Err(snp(ERRN, cat!("Duplicate prefix match detected for '", name, "'")).0);
        }
        cur.pre.push(PEntry {
            name: last.to_vec(),
            ctx: Some(c),
            typ: Some(t),
        });
    } else {
        let child = match tnode_child(cur, last, true) {
            Some(child) => child,
            None => return Err(Vec::new()),
        };
        if child.ctx.is_some() || child.typ.is_some() {
            return Err(snp(ERRN, cat!("Duplicate prefix match detected for '", name, "'")).0);
        }
        child.ctx = Some(c);
        child.typ = Some(t);
    }
    Ok(())
}

// The arena (trie_node_arena.h): every allocation rounded up to 4 bytes.
// `d.len()` is the capacity; fresh bytes are zero.
struct Arena {
    d: Vec<u8>,
    len: u32,
}

fn arena_alloc(a: &mut Arena, size: usize) -> u32 {
    let al = ((size + 3) & !3usize) as u32;
    let cap = a.d.len() as u32;
    if a.len.wrapping_add(al) > cap {
        let nc = a.len.wrapping_add(al).wrapping_add(cap).wrapping_mul(2);
        a.d.resize(nc as usize, 0);
    }
    let off = a.len;
    a.len = a.len.wrapping_add(al);
    off
}

fn arena_put(a: &mut Arena, off: u32, index: usize, value: u32) {
    let at = off as usize + 4 * index;
    if let Some(slot) = a.d.get_mut(at..at + 4) {
        slot.copy_from_slice(&value.to_ne_bytes());
    }
}

fn arena_string(a: &mut Arena, s: &[u8]) -> u32 {
    let off = arena_alloc(a, s.len() + 1);
    let at = off as usize;
    if let Some(slot) = a.d.get_mut(at..at + s.len() + 1) {
        slot[..s.len()].copy_from_slice(s);
        slot[s.len()] = 0;
    }
    off
}

struct Serializer {
    a: Arena,
    ctxs: Vec<Vec<u8>>,
    types: Vec<Vec<u8>>,
}

fn index_of(set: &[Vec<u8>], s: Option<&[u8]>) -> u32 {
    let s = match s {
        Some(s) if !s.is_empty() => s,
        _ => return !0,
    };
    match set.binary_search_by(|have| have.as_slice().cmp(s)) {
        Ok(i) => i as u32,
        Err(_) => !0,
    }
}

fn serialize_strings(a: &mut Arena, s: &[Vec<u8>]) {
    let cnt = arena_alloc(a, 4);
    arena_put(a, cnt, 0, s.len() as u32);
    let arr = arena_alloc(a, 4 * s.len());
    for (i, text) in s.iter().enumerate() {
        let so = arena_string(a, text);
        arena_put(a, arr, i, so);
    }
}

fn write_entry(z: &mut Serializer, name: &[u8], ctx: Option<&[u8]>, typ: Option<&[u8]>) -> u32 {
    let ci = index_of(&z.ctxs, ctx);
    let ti = index_of(&z.types, typ);
    let off = arena_alloc(&mut z.a, 16);
    let no = arena_string(&mut z.a, name);
    arena_put(&mut z.a, off, 0, no);
    arena_put(&mut z.a, off, 1, name.len() as u32);
    arena_put(&mut z.a, off, 2, ci);
    arena_put(&mut z.a, off, 3, ti);
    off
}

// The serializer sorts with std::sort; ties cannot change what a lookup
// finds (two prefixes of equal length cannot both match one name), so a
// stable sort gives the same trie up to the order of equal-length prefixes.
fn write_node(z: &mut Serializer, n: &mut TNode) -> u32 {
    let t = arena_alloc(&mut z.a, 28);
    let pe = write_entry(z, &n.name, n.ctx.as_deref(), n.typ.as_deref());
    arena_put(&mut z.a, t, 0, pe);

    n.pre.sort_by(|x, y| y.name.len().cmp(&x.name.len()));
    arena_put(&mut z.a, t, 3, n.pre.len() as u32);
    let pa = arena_alloc(&mut z.a, 4 * n.pre.len());
    arena_put(&mut z.a, t, 4, pa);
    for (i, e) in n.pre.iter().enumerate() {
        let o = write_entry(z, &e.name, e.ctx.as_deref(), e.typ.as_deref());
        arena_put(&mut z.a, pa, i, o);
    }

    n.ex.sort_by(|x, y| x.name.cmp(&y.name));
    arena_put(&mut z.a, t, 5, n.ex.len() as u32);
    let ea = arena_alloc(&mut z.a, 4 * n.ex.len());
    arena_put(&mut z.a, t, 6, ea);
    for (i, e) in n.ex.iter().enumerate() {
        let o = write_entry(z, &e.name, e.ctx.as_deref(), e.typ.as_deref());
        arena_put(&mut z.a, ea, i, o);
    }

    n.kids.sort_by(|x, y| x.name.cmp(&y.name));
    arena_put(&mut z.a, t, 1, n.kids.len() as u32);
    let ca = arena_alloc(&mut z.a, 4 * n.kids.len());
    arena_put(&mut z.a, t, 2, ca);
    for i in 0..n.kids.len() {
        let o = write_node(z, &mut n.kids[i]);
        arena_put(&mut z.a, ca, i, o);
    }
    t
}

// TrieSerializer::SerializeTrie. Returns the bytes of property_info.
fn serialize_trie(b: &mut Builder) -> Vec<u8> {
    let mut z = Serializer {
        a: Arena {
            d: Vec::new(),
            len: 0,
        },
        ctxs: std::mem::take(&mut b.ctxs),
        types: std::mem::take(&mut b.types),
    };
    z.ctxs.sort();
    z.types.sort();
    let h = arena_alloc(&mut z.a, 24);
    arena_put(&mut z.a, h, 0, 1); // current_version
    arena_put(&mut z.a, h, 1, 1); // minimum_supported_version
    let at = z.a.len;
    arena_put(&mut z.a, h, 3, at); // contexts_offset
    let ctxs = std::mem::take(&mut z.ctxs);
    serialize_strings(&mut z.a, &ctxs);
    z.ctxs = ctxs;
    let at = z.a.len;
    arena_put(&mut z.a, h, 4, at); // types_offset
    let types = std::mem::take(&mut z.types);
    serialize_strings(&mut z.a, &types);
    z.types = types;
    let at = z.a.len;
    arena_put(&mut z.a, h, 2, at); // size, for the index lookups
    let root = write_node(&mut z, &mut b.root);
    arena_put(&mut z.a, h, 5, root);
    let at = z.a.len;
    arena_put(&mut z.a, h, 2, at);
    let mut bytes = z.a.d;
    bytes.truncate(at as usize);
    bytes
}

// ------------------------------------------------------------- the areas

unsafe fn pa_bytes_used(b: *mut u8) -> *mut u32 {
    b.cast()
}
unsafe fn au32<'a>(p: *mut u8) -> &'a AtomicU32 {
    &*(p as *const AtomicU32)
}
unsafe fn pa_serial<'a>(b: *mut u8) -> &'a AtomicU32 {
    au32(b.add(4))
}
unsafe fn pa_data(b: *mut u8) -> *mut u8 {
    b.add(PA_HDR)
}
unsafe fn rd32(p: *const u8) -> u32 {
    ptr::read_unaligned(p.cast::<u32>())
}
unsafe fn wr32(p: *mut u8, value: u32) {
    ptr::write_unaligned(p.cast::<u32>(), value)
}

unsafe fn futex_wake_all(addr: *mut c_void) {
    // The guests' FUTEX_WAIT on a MAP_SHARED mapping parks on this flavour
    // (runtime/futex_ops.c); -ENOENT is "nobody waiting".
    __ulock_wake(
        UL_COMPARE_AND_WAIT_SHARED | ULF_WAKE_ALL | ULF_NO_ERRNO,
        addr,
        0,
    );
}

unsafe fn pa_obj(b: *mut u8, off: u32) -> *mut u8 {
    if off > PA_DATA_SIZE {
        return ptr::null_mut();
    }
    pa_data(b).add(off as usize)
}

unsafe fn pa_allocate(b: *mut u8, size: usize, off: &mut u32) -> *mut u8 {
    let al = ((size + 3) & !3usize) as u32;
    let used = *pa_bytes_used(b);
    if used as u64 + al as u64 > PA_DATA_SIZE as u64 {
        return ptr::null_mut();
    }
    *off = used;
    *pa_bytes_used(b) = used + al;
    pa_data(b).add(used as usize)
}

unsafe fn new_prop_bt(b: *mut u8, name: &[u8], off: &mut u32) -> *mut u8 {
    let len = name.len() as u32;
    let p = pa_allocate(b, PROP_BT_HDR as usize + name.len() + 1, off);
    if p.is_null() {
        return ptr::null_mut();
    }
    wr32(p, len);
    ptr::copy_nonoverlapping(name.as_ptr(), p.add(PROP_BT_HDR as usize), name.len());
    *p.add(PROP_BT_HDR as usize + name.len()) = 0;
    p
}

unsafe fn new_prop_info(b: *mut u8, name: &[u8], value: &[u8], off: &mut u32) -> *mut u8 {
    let namelen = name.len();
    let valuelen = value.len() as u32;
    let mut noff = 0u32;
    let p = pa_allocate(b, PROP_INFO_HDR as usize + namelen + 1, &mut noff);
    if p.is_null() {
        return ptr::null_mut();
    }
    ptr::copy_nonoverlapping(name.as_ptr(), p.add(PROP_INFO_HDR as usize), namelen);
    *p.add(PROP_INFO_HDR as usize + namelen) = 0;
    if valuelen >= PROP_VALUE_MAX {
        let mut loff = 0u32;
        let lv = pa_allocate(b, value.len() + 1, &mut loff);
        if lv.is_null() {
            return ptr::null_mut();
        }
        ptr::copy_nonoverlapping(value.as_ptr(), lv, value.len());
        *lv.add(value.len()) = 0;
        loff = loff.wrapping_sub(noff); // relative to the prop_info
        au32(p).store(
            ((K_LONG_LEGACY_ERROR.len() << 24) as u32) | LONG_FLAG,
            Ordering::Relaxed,
        );
        ptr::copy_nonoverlapping(
            K_LONG_LEGACY_ERROR.as_ptr(),
            p.add(4),
            K_LONG_LEGACY_ERROR.len(),
        );
        *p.add(4 + K_LONG_LEGACY_ERROR.len()) = 0;
        wr32(p.add(LONG_OFFSET_AT), loff);
    } else {
        au32(p).store(valuelen << 24, Ordering::Relaxed);
        ptr::copy_nonoverlapping(value.as_ptr(), p.add(4), value.len());
        *p.add(4 + value.len()) = 0;
    }
    *off = noff;
    p
}

fn cmp_prop_name(one: &[u8], two: &[u8], two_len: u32) -> c_int {
    let one_len = one.len() as u32;
    if one_len < two_len {
        return -1;
    }
    if one_len > two_len {
        return 1;
    }
    strncmp(one, two, one.len())
}

// prop_area::find_prop_bt: siblings are a binary tree ordered by length,
// then bytes.
unsafe fn find_prop_bt(b: *mut u8, bt: *mut u8, name: &[u8], alloc: bool) -> *mut u8 {
    let mut cur = bt;
    loop {
        if cur.is_null() {
            return ptr::null_mut();
        }
        let cl = rd32(cur);
        // Only names of the same length are compared byte by byte.
        let r = if cl == name.len() as u32 {
            let have = std::slice::from_raw_parts(cur.add(PROP_BT_HDR as usize), cl as usize);
            cmp_prop_name(name, have, cl)
        } else {
            cmp_prop_name(name, &[], cl)
        };
        if r == 0 {
            return cur;
        }
        let link = au32(cur.add(if r < 0 { 8 } else { 12 }));
        let o = link.load(Ordering::Relaxed);
        if o != 0 {
            cur = pa_obj(b, o);
            continue;
        }
        if !alloc {
            return ptr::null_mut();
        }
        let mut no = 0u32;
        let nb = new_prop_bt(b, name, &mut no);
        if !nb.is_null() {
            link.store(no, Ordering::Release);
        }
        return nb;
    }
}

// prop_area::find_property: the trie of '.'-separated pieces.
unsafe fn find_property(b: *mut u8, name: &[u8], value: &[u8], alloc: bool) -> *mut u8 {
    let mut rem = name;
    let mut cur = pa_obj(b, 0); // the root node
    loop {
        let sep = find_byte(rem, b'.');
        let sub = sep.is_some();
        let sl = sep.unwrap_or(rem.len());
        if sl == 0 {
            return ptr::null_mut();
        }
        let piece = &rem[..sl];
        let mut root = ptr::null_mut();
        let co = au32(cur.add(16)).load(Ordering::Relaxed);
        if co != 0 {
            root = pa_obj(b, co);
        } else if alloc {
            let mut no = 0u32;
            root = new_prop_bt(b, piece, &mut no);
            if !root.is_null() {
                au32(cur.add(16)).store(no, Ordering::Release);
            }
        }
        if root.is_null() {
            return ptr::null_mut();
        }
        cur = find_prop_bt(b, root, piece, alloc);
        if cur.is_null() {
            return ptr::null_mut();
        }
        if !sub {
            break;
        }
        rem = &rem[sl + 1..];
    }
    let po = au32(cur.add(4)).load(Ordering::Relaxed);
    if po != 0 {
        return pa_obj(b, po);
    }
    if !alloc {
        return ptr::null_mut();
    }
    let mut no = 0u32;
    let pi = new_prop_info(b, name, value, &mut no);
    if !pi.is_null() {
        au32(cur.add(4)).store(no, Ordering::Release);
    }
    pi
}

unsafe fn foreach_bt(b: *mut u8, bt: *mut u8, f: &mut dyn FnMut(&[u8], &[u8])) {
    if bt.is_null() {
        return;
    }
    let o = au32(bt.add(8)).load(Ordering::Relaxed);
    if o != 0 {
        foreach_bt(b, pa_obj(b, o), f);
    }
    let o = au32(bt.add(4)).load(Ordering::Relaxed);
    if o != 0 {
        let pi = pa_obj(b, o);
        if !pi.is_null() {
            let s = au32(pi).load(Ordering::Relaxed);
            let v: *const u8 = if s & LONG_FLAG != 0 {
                pi.add(rd32(pi.add(LONG_OFFSET_AT)) as usize)
            } else {
                pi.add(4)
            };
            f(cstr_ptr(pi.add(PROP_INFO_HDR as usize)), cstr_ptr(v));
        }
    }
    let o = au32(bt.add(16)).load(Ordering::Relaxed);
    if o != 0 {
        foreach_bt(b, pa_obj(b, o), f);
    }
    let o = au32(bt.add(12)).load(Ordering::Relaxed);
    if o != 0 {
        foreach_bt(b, pa_obj(b, o), f);
    }
}

unsafe fn prop_value(pi: *mut u8) -> *const u8 {
    let s = au32(pi).load(Ordering::Acquire);
    if s & LONG_FLAG != 0 {
        let lo = rd32(pi.add(LONG_OFFSET_AT));
        return pi.add(lo as usize);
    }
    pi.add(4)
}

// ------------------------------------------------------ PropertySet & co

// init/util.cpp IsLegalPropertyName.
fn legal_name(name: &[u8]) -> bool {
    let n = name.len();
    if n < 1 || name[0] == b'.' || name[n - 1] == b'.' {
        return false;
    }
    for i in 0..n {
        let c = name[i];
        if c == b'.' {
            if name[i - 1] == b'.' {
                return false;
            }
            continue;
        }
        if c == b'_' || c == b'-' || c == b'@' || c == b':' {
            continue;
        }
        if c.is_ascii_lowercase() || c.is_ascii_uppercase() || c.is_ascii_digit() {
            continue;
        }
        return false;
    }
    true
}

// mbstowcs(nullptr, value, 0) != -1 in bionic's UTF-8 C locale: well-formed
// UTF-8, no overlong forms, no surrogates, nothing past U+10FFFF.
fn valid_utf8(s: &[u8]) -> bool {
    let n = s.len();
    let mut i = 0usize;
    while i < n {
        let c = s[i] as u32;
        if c < 0x80 {
            i += 1;
            continue;
        }
        let k: usize;
        let mut cp: u32;
        let min: u32;
        if c & 0xe0 == 0xc0 {
            k = 1;
            cp = c & 0x1f;
            min = 0x80;
        } else if c & 0xf0 == 0xe0 {
            k = 2;
            cp = c & 0x0f;
            min = 0x800;
        } else if c & 0xf8 == 0xf0 {
            k = 3;
            cp = c & 0x07;
            min = 0x10000;
        } else {
            return false;
        }
        for j in 1..=k {
            if i + j >= n || (s[i + j] & 0xc0) != 0x80 {
                return false;
            }
            cp = (cp << 6) | (s[i + j] & 0x3f) as u32;
        }
        if cp < min || cp > 0x10ffff || (0xd800..=0xdfff).contains(&cp) {
            return false;
        }
        i += k + 1;
    }
    true
}

// init/property_type.cpp CheckType.
fn check_type(typ: Option<&[u8]>, value: &[u8]) -> bool {
    if value.is_empty() {
        return true;
    }
    let typ = match typ {
        Some(t) if !t.is_empty() => t,
        _ => return false,
    };
    let fl = find_byte(typ, b' ').unwrap_or(typ.len());
    if fl >= 64 {
        return false;
    }
    let first = &typ[..fl];
    if first == b"string" {
        return true;
    }
    if first == b"bool" {
        return value == b"true" || value == b"false" || value == b"1" || value == b"0";
    }
    if first == b"int" || first == b"uint" {
        // android::base::ParseInt/ParseUint (libbase parseint.h): leading
        // spaces skipped, base 16 after "0x", else 10, the whole string,
        // no '-' for uint.
        let is_uint = first == b"uint";
        if is_uint && value[0] == b'-' {
            return false;
        }
        let mut at = 0usize;
        while at < value.len() && isspace(value[at]) {
            at += 1;
        }
        let s = &value[at..];
        if is_uint && s.first() == Some(&b'-') {
            return false;
        }
        let base = if s.len() >= 2 && s[0] == b'0' && (s[1] == b'x' || s[1] == b'X') {
            16
        } else {
            10
        };
        return with_path(s, |p| unsafe {
            let mut end: *mut c_char = ptr::null_mut();
            set_errno(0);
            if is_uint {
                let _ = strtoull(p, &mut end, base);
            } else {
                let _ = strtoll(p, &mut end, base);
            }
            errno() == 0 && end.cast_const() != p && *end == 0
        });
    }
    if first == b"double" {
        // ParseDouble: strtod, the whole string, within [lowest, max].
        return with_path(value, |p| unsafe {
            let mut end: *mut c_char = ptr::null_mut();
            set_errno(0);
            let d = strtod(p, &mut end);
            errno() == 0 && end.cast_const() != p && *end == 0 && d >= -f64::MAX && d <= f64::MAX
        });
    }
    if first == b"size" {
        let mut p = 0usize;
        while p < value.len() && value[p].is_ascii_digit() {
            p += 1;
        }
        if p == 0 || p >= value.len() || (value[p] != b'g' && value[p] != b'k' && value[p] != b'm')
        {
            return false;
        }
        return p + 1 == value.len();
    }
    if first == b"enum" {
        let mut p = fl;
        while p < typ.len() {
            while p < typ.len() && typ[p] == b' ' {
                p += 1;
            }
            let l = find_byte(&typ[p..], b' ').unwrap_or(typ.len() - p);
            if l != 0 && value.len() == l && &typ[p..p + l] == value {
                return true;
            }
            p += l;
        }
    }
    false
}

// --------------------------------------------------- persistent properties
//
// init/persistent_properties.cpp's file: a PersistentProperties protobuf,
// `repeated PersistentPropertyRecord properties = 1` with `name = 1` and
// `value = 2`, rewritten whole through a .tmp file on every change.
struct Kv {
    k: Vec<u8>,
    v: Vec<u8>,
}

fn kv_set(l: &mut Vec<Kv>, k: &[u8], v: &[u8], warn: bool) {
    for item in l.iter_mut() {
        if item.k.as_slice() == k {
            if warn && item.v.as_slice() != v {
                plog!(
                    "Overriding previous property '",
                    k,
                    "':'",
                    item.v,
                    "' with new value '",
                    v,
                    "'"
                );
            }
            item.v = v.to_vec();
            return;
        }
    }
    l.push(Kv {
        k: k.to_vec(),
        v: v.to_vec(),
    });
}

fn pb_varint(d: &[u8], p: &mut usize, end: usize, v: &mut u64) -> bool {
    *v = 0;
    let mut sh = 0u32;
    while sh < 64 && *p < end {
        let c = d[*p];
        *p += 1;
        *v |= ((c & 0x7f) as u64) << sh;
        if c & 0x80 == 0 {
            return true;
        }
        sh += 7;
    }
    false
}

fn pb_skip(d: &[u8], p: &mut usize, end: usize, wt: c_int) -> bool {
    let mut v = 0u64;
    match wt {
        0 => pb_varint(d, p, end, &mut v),
        1 => {
            if end - *p < 8 {
                return false;
            }
            *p += 8;
            true
        }
        2 => {
            if !pb_varint(d, p, end, &mut v) || ((end - *p) as u64) < v {
                return false;
            }
            *p += v as usize;
            true
        }
        5 => {
            if end - *p < 4 {
                return false;
            }
            *p += 4;
            true
        }
        _ => false,
    }
}

fn pb_put_varint(out: &mut Vec<u8>, mut v: u64) {
    while v >= 0x80 {
        out.push((v | 0x80) as u8);
        v >>= 7;
    }
    out.push(v as u8);
}

fn varint_len(mut v: u64) -> usize {
    let mut n = 1;
    while v >= 0x80 {
        v >>= 7;
        n += 1;
    }
    n
}

// ----------------------------------------------------- loading .prop files

// C-string views of a buffer that is edited in place (LoadProperties): a
// read past the end is the terminating NUL.
fn b_at(d: &[u8], i: usize) -> u8 {
    d.get(i).copied().unwrap_or(0)
}
fn b_set(d: &mut [u8], i: usize, value: u8) {
    if let Some(slot) = d.get_mut(i) {
        *slot = value;
    }
}
fn b_strchr(d: &[u8], from: usize, c: u8) -> Option<usize> {
    let mut i = from;
    loop {
        let x = b_at(d, i);
        if x == c {
            return Some(i);
        }
        if x == 0 {
            return None;
        }
        i += 1;
    }
}
fn b_cstr(d: &[u8], from: usize) -> &[u8] {
    cstr(d.get(from..).unwrap_or(&[]))
}

// ------------------------------------------------------- the area files

fn map_area_rw(path: &[u8], create: bool) -> *mut u8 {
    let mut fd;
    if create {
        fd = sys_open(
            path,
            O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
            0o644,
        );
        if fd >= 0 && unsafe { ftruncate(fd, PA_SIZE as i64) } != 0 {
            unsafe { close(fd) };
            fd = -1;
        }
    } else {
        // 0444 on disk, as init makes them: this process writes through a
        // mapping taken while the owner could still open it for writing.
        sys_chmod(path, 0o644);
        fd = sys_open(path, O_RDWR | O_NOFOLLOW | O_CLOEXEC, 0);
    }
    if fd < 0 {
        let e = errno();
        plog!(path, ": ", errstr(e));
        return ptr::null_mut();
    }
    let mut st = new_stat();
    if unsafe { fstat(fd, &mut st) } != 0 || st.st_size != PA_SIZE as i64 {
        plog!(path, ": not a ", PA_SIZE as i32, "-byte property area");
        unsafe { close(fd) };
        return ptr::null_mut();
    }
    let m = unsafe {
        let m = mmap(
            ptr::null_mut(),
            PA_SIZE,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            0,
        );
        fchmod(fd, 0o444);
        close(fd);
        m
    };
    if m as isize == -1 {
        let e = errno();
        plog!(path, ": mmap: ", errstr(e));
        return ptr::null_mut();
    }
    let m = m.cast::<u8>();
    unsafe {
        if create {
            // The prop_area constructor: the root node and the dirty backup
            // area are reserved at the start of data_.
            *pa_bytes_used(m) = PROP_BT_HDR + ((PROP_VALUE_MAX + 3) & !3);
            pa_serial(m).store(0, Ordering::Relaxed);
            wr32(m.add(8), PROP_AREA_MAGIC);
            wr32(m.add(12), PROP_AREA_VERSION);
        } else {
            let mg = rd32(m.add(8));
            let vr = rd32(m.add(12));
            if mg != PROP_AREA_MAGIC || vr != PROP_AREA_VERSION || *pa_bytes_used(m) > PA_DATA_SIZE
            {
                plog!(path, ": bad magic or version");
                munmap(m.cast(), PA_SIZE);
                return ptr::null_mut();
            }
        }
    }
    m
}

fn write_file(path: &[u8], d: &[u8], mode: u16) -> bool {
    let fd = sys_open(
        path,
        O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
        0o600,
    );
    if fd < 0 {
        return false;
    }
    unsafe {
        let ok = write(fd, d.as_ptr().cast(), d.len()) == d.len() as isize;
        fchmod(fd, mode);
        close(fd);
        ok
    }
}

// A service that starts again: the areas are the state, adopt them.
//
// Only this process writes the areas, but they are files a same-user process
// could have changed while no service ran: every object reachable from an
// area's root must lie inside its used bytes before this process follows
// offsets in it again.
unsafe fn valid_prop_info(b: *mut u8, off: u32, used: u32) -> bool {
    if off % 4 != 0 || off as u64 + PROP_INFO_HDR as u64 + 1 > used as u64 {
        return false;
    }
    let pi = pa_data(b).add(off as usize);
    let name = std::slice::from_raw_parts(
        pi.add(PROP_INFO_HDR as usize),
        (used - off - PROP_INFO_HDR) as usize,
    );
    if !name.contains(&0) {
        return false;
    }
    let serial = au32(pi).load(Ordering::Relaxed);
    if serial & LONG_FLAG != 0 {
        let lo = rd32(pi.add(LONG_OFFSET_AT));
        if off as u64 + lo as u64 >= used as u64 {
            return false;
        }
        let value = std::slice::from_raw_parts(pi.add(lo as usize), (used - off - lo) as usize);
        if !value.contains(&0) {
            return false;
        }
    } else if (serial >> 24) >= PROP_VALUE_MAX || *pi.add(4 + (serial >> 24) as usize) != 0 {
        return false;
    }
    true
}

unsafe fn valid_bt(b: *mut u8, off: u32, used: u32, root: bool, budget: &mut u32) -> bool {
    if *budget == 0 {
        return false;
    }
    *budget -= 1;
    if off % 4 != 0 || off as u64 + PROP_BT_HDR as u64 > used as u64 {
        return false;
    }
    let bt = pa_data(b).add(off as usize);
    if !root {
        // The root node is never constructed: its "name" is the first byte
        // of the dirty backup area.
        let nl = rd32(bt);
        if off as u64 + PROP_BT_HDR as u64 + nl as u64 + 1 > used as u64
            || *bt.add(PROP_BT_HDR as usize + nl as usize) != 0
        {
            return false;
        }
    }
    let po = au32(bt.add(4)).load(Ordering::Relaxed);
    if po != 0 && !valid_prop_info(b, po, used) {
        return false;
    }
    let mut f = 8usize;
    while f <= 16 {
        let o = au32(bt.add(f)).load(Ordering::Relaxed);
        if o != 0 && !valid_bt(b, o, used, false, budget) {
            return false;
        }
        f += 4;
    }
    true
}

unsafe fn valid_area(b: *mut u8) -> bool {
    let used = *pa_bytes_used(b);
    let mut budget = PA_DATA_SIZE / PROP_BT_HDR; // more nodes cannot fit: a cycle
    used <= PA_DATA_SIZE
        && used >= PROP_BT_HDR + ((PROP_VALUE_MAX + 3) & !3)
        && valid_bt(b, 0, used, true, &mut budget)
}

// ------------------------------------------------------------ the socket

unsafe fn recv_fully(fd: c_int, p: *mut u8, mut n: usize, timeout_ms: &mut c_int) -> bool {
    let mut d = p;
    while n > 0 {
        if *timeout_ms <= 0 {
            return false;
        }
        let mut pf = PollFd {
            fd,
            events: POLLIN,
            revents: 0,
        };
        let t0 = monotonic();
        let r = poll(&mut pf, 1, *timeout_ms);
        let t1 = monotonic();
        let el = ((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1_000_000) as c_int;
        *timeout_ms = (*timeout_ms).wrapping_sub(if el > 0 {
            el
        } else if r < 0 && errno() == EINTR {
            1
        } else {
            0
        });
        if r < 0 && errno() == EINTR {
            continue;
        }
        if r <= 0 {
            return false;
        }
        let k = recv(fd, d.cast(), n, MSG_DONTWAIT);
        if k < 0 && (errno() == EINTR || errno() == EAGAIN) {
            continue;
        }
        if k <= 0 {
            return false;
        }
        d = d.add(k as usize);
        n -= k as usize;
    }
    true
}

unsafe fn recv_string(fd: c_int, timeout_ms: &mut c_int) -> Option<Vec<u8>> {
    let mut len = 0u32;
    if !recv_fully(fd, (&mut len as *mut u32).cast(), 4, timeout_ms) {
        return None;
    }
    if len > 0xffff {
        plog!("sys_prop: RecvString asked to read huge string: ", len);
        return None;
    }
    let mut s = vec![0u8; len as usize];
    if len != 0 && !recv_fully(fd, s.as_mut_ptr(), len as usize, timeout_ms) {
        return None;
    }
    if s.contains(&0) {
        // An embedded NUL: init keeps the std::string whole and the name
        // then fails IsLegalPropertyName; the value would be cut here, so
        // refuse it outright.
        return None;
    }
    Some(s)
}

unsafe fn send_u32(fd: c_int, v: u32) {
    loop {
        let r = send(fd, (&v as *const u32).cast(), 4, 0);
        if !(r < 0 && errno() == EINTR) {
            break;
        }
    }
}

extern "C" fn on_term(_sig: c_int) {
    G_STOP.store(1, Ordering::Relaxed);
}

impl Svc {
    // ------------------------------------------------------------ root paths
    //
    // The image's symlinks are guest paths (/system_ext -> /system/system_ext,
    // /odm/etc -> /vendor/odm/etc). A guest path is resolved here the way a
    // kernel resolves one inside a chroot: component by component, ".", ".."
    // and links (absolute ones from the root, relative ones from their
    // directory) handled here, never above the root. The host path that comes
    // out has no link left in it except, possibly, a last component that does
    // not exist yet.
    fn root_path(&self, guest: &[u8]) -> Option<Vec<u8>> {
        if guest.first() != Some(&b'/') || guest.len() >= PATH_MAX {
            return None;
        }
        let mut rest: Vec<u8> = guest.to_vec(); // what is still to walk
        let mut done: Vec<u8> = Vec::new(); // the resolved guest path ("" is "/")
        let mut links = 0;
        let mut p = 0usize;
        while p < rest.len() {
            while p < rest.len() && rest[p] == b'/' {
                p += 1;
            }
            if p >= rest.len() {
                break;
            }
            let cl = find_byte(&rest[p..], b'/').unwrap_or(rest.len() - p);
            if cl > NAME_MAX {
                return None;
            }
            let comp = rest[p..p + cl].to_vec();
            p += cl;
            if comp == b"." {
                continue;
            }
            if comp == b".." {
                if let Some(s) = done.iter().rposition(|&c| c == b'/') {
                    done.truncate(s);
                }
                continue;
            }
            let cand = cat!(done, "/", comp);
            if cand.len() >= PATH_MAX {
                return None;
            }
            let host = cat!(self.root, cand);
            if host.len() >= PATH_MAX {
                return None;
            }
            let mut tgt = [0u8; PATH_MAX];
            let tl = with_path(&host, |h| unsafe {
                readlink(h, tgt.as_mut_ptr().cast(), PATH_MAX - 1)
            });
            if tl > 0 {
                links += 1;
                if links > 40 {
                    return None;
                }
                let tgt = cstr(&tgt[..tl as usize]);
                let next = cat!(tgt, "/", &rest[p..]);
                if next.len() >= PATH_MAX {
                    return None;
                }
                let absolute = tgt.first() == Some(&b'/');
                rest = next;
                p = 0;
                if absolute {
                    done.clear();
                }
                continue;
            }
            done = cand;
        }
        let out = if done.is_empty() {
            cat!(self.root, "/")
        } else {
            cat!(self.root, done)
        };
        if out.len() < PATH_MAX {
            Some(out)
        } else {
            None
        }
    }

    // The file's bytes (what was read of it).
    fn read_guest_file(&self, guest: &[u8]) -> Option<Vec<u8>> {
        let host = self.root_path(guest)?;
        let fd = sys_open(&host, O_RDONLY | O_CLOEXEC, 0);
        if fd < 0 {
            return None;
        }
        let mut st = new_stat();
        if unsafe { fstat(fd, &mut st) } != 0 || !is_reg(&st) || st.st_size > (64 << 20) {
            unsafe { close(fd) };
            return None;
        }
        let size = st.st_size as usize;
        let mut d = vec![0u8; size];
        let mut got = 0usize;
        while got < size {
            let r = unsafe { read(fd, d.as_mut_ptr().add(got).cast(), size - got) };
            if r < 0 && errno() == EINTR {
                continue;
            }
            if r <= 0 {
                break;
            }
            got += r as usize;
        }
        unsafe { close(fd) };
        d.truncate(got);
        Some(d)
    }

    fn guest_readable(&self, guest: &[u8]) -> bool {
        match self.root_path(guest) {
            Some(host) => with_path(&host, |h| unsafe { access(h, R_OK) }) == 0,
            None => false,
        }
    }

    // ParsePropertyInfoFile: one "name context [prefix|exact] [type...]" per line.
    fn parse_contexts(
        &self,
        b: &mut Builder,
        guest: &[u8],
        require_prefix_or_exact: bool,
        build_ok: &mut bool,
    ) {
        let data = match self.read_guest_file(guest) {
            Some(data) => data,
            None => {
                plog!("Could not read properties from '", guest, "'");
                return;
            }
        };
        for raw in cstr(&data).split(|&c| c == b'\n') {
            let mut line = raw;
            while let Some((&c, tail)) = line.split_first() {
                if !isspace(c) {
                    break;
                }
                line = tail;
            }
            while let Some((&c, head)) = line.split_last() {
                if !isspace(c) {
                    break;
                }
                line = head;
            }
            if line.is_empty() || line[0] == b'#' {
                continue;
            }
            let tok: Vec<&[u8]> = line
                .split(|&c| isspace(c))
                .filter(|t| !t.is_empty())
                .take(64)
                .collect();
            let nt = tok.len();
            if nt < 2 {
                plog!(
                    "Could not read line from '",
                    guest,
                    "': no context entry in '",
                    line,
                    "'"
                );
                continue;
            }
            let matched: &[u8] = if nt > 2 { tok[2] } else { b"" };
            let mut exact = false;
            if matched == b"exact" {
                exact = true;
            } else if matched != b"prefix" && !matched.is_empty() && require_prefix_or_exact {
                plog!(
                    "Could not read line from '",
                    guest,
                    "': Match operation '",
                    matched,
                    "' is not valid"
                );
                continue;
            }
            // Join(type_strings, " ").
            let mut typ: Vec<u8> = Vec::new();
            for (i, t) in tok.iter().enumerate().skip(3) {
                if typ.len() >= 512 {
                    break;
                }
                if i > 3 {
                    typ.push(b' ');
                }
                typ.extend_from_slice(t);
            }
            if typ.len() >= 512 {
                plog!("Could not read line from '", guest, "': type too long");
                continue;
            }
            if nt > 3 {
                // IsTypeValid
                let valid = if tok[3] == b"enum" {
                    nt > 4
                } else {
                    const SIMPLE: [&[u8]; 6] =
                        [b"string", b"bool", b"int", b"uint", b"double", b"size"];
                    nt == 4 && SIMPLE.iter().any(|s| *s == tok[3])
                };
                if !valid {
                    plog!(
                        "Could not read line from '",
                        guest,
                        "': Type '",
                        typ,
                        "' is not valid"
                    );
                    continue;
                }
            }
            if let Err(err) = builder_add(b, tok[0], tok[1], &typ, exact) {
                plog!("Unable to serialize property contexts: ", err);
                *build_ok = false;
            }
        }
    }

    // -------------------------------------------------- property_info lookups
    //
    // property_info_parser.cpp GetPropertyInfoIndexes, bounds-checked.
    fn pi_u32(&self, off: u32) -> u32 {
        if off > self.pi_size || self.pi_size - off < 4 {
            return !0;
        }
        match self.pi.get(off as usize..off as usize + 4) {
            Some(b) => u32::from_ne_bytes([b[0], b[1], b[2], b[3]]),
            None => !0,
        }
    }
    fn pi_str(&self, off: u32) -> &[u8] {
        if off >= self.pi_size {
            return b"";
        }
        let tail = match self.pi.get(off as usize..self.pi_size as usize) {
            Some(tail) => tail,
            None => return b"",
        };
        match find_byte(tail, 0) {
            Some(end) => &tail[..end],
            None => b"",
        }
    }
    fn pi_num_contexts(&self) -> u32 {
        self.pi_u32(self.pi_u32(12))
    }
    fn pi_context(&self, i: u32) -> &[u8] {
        self.pi_str(self.pi_u32(
            self.pi_u32(12).wrapping_add(4).wrapping_add(4u32.wrapping_mul(i)),
        ))
    }
    fn pi_type(&self, i: u32) -> &[u8] {
        self.pi_str(self.pi_u32(
            self.pi_u32(16).wrapping_add(4).wrapping_add(4u32.wrapping_mul(i)),
        ))
    }

    // A trie node's fields: 0 property_entry, 1 num_child_nodes, 2 child_nodes,
    // 3 num_prefixes, 4 prefix_entries, 5 num_exact_matches, 6 exact_match_entries.
    fn node_f(&self, node: u32, f: u32) -> u32 {
        self.pi_u32(node.wrapping_add(4 * f))
    }
    fn entry_f(&self, entry: u32, f: u32) -> u32 {
        self.pi_u32(entry.wrapping_add(4 * f))
    }

    fn check_prefix(&self, rem: &[u8], node: u32, ci: &mut u32, ti: &mut u32) {
        let rl = rem.len();
        let n = self.node_f(node, 3);
        let arr = self.node_f(node, 4);
        let mut i = 0u32;
        while i < n && i < 100000 {
            let e = self.pi_u32(arr.wrapping_add(4 * i));
            i += 1;
            let pl = self.entry_f(e, 1);
            if pl as usize > rl {
                continue;
            }
            if strncmp(self.pi_str(self.entry_f(e, 0)), rem, pl as usize) == 0 {
                if self.entry_f(e, 2) != !0 {
                    *ci = self.entry_f(e, 2);
                }
                if self.entry_f(e, 3) != !0 {
                    *ti = self.entry_f(e, 3);
                }
                return;
            }
        }
    }

    fn find_child(&self, node: u32, name: &[u8], len: u32, out: &mut u32) -> bool {
        let n = self.node_f(node, 1);
        let arr = self.node_f(node, 2);
        let mut lo: c_int = 0;
        let mut hi: c_int = (n as c_int).wrapping_sub(1);
        while hi >= lo {
            let mid = (lo + hi) / 2;
            let child = self.pi_u32(arr.wrapping_add(4u32.wrapping_mul(mid as u32)));
            let cn = self.pi_str(self.entry_f(self.node_f(child, 0), 0));
            let mut c = strncmp(cn, name, len as usize);
            if c == 0 && cn.len() > len as usize {
                c = 1;
            }
            if c == 0 {
                *out = child;
                return true;
            }
            if c < 0 {
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        false
    }

    fn pi_lookup(&self, name: &[u8], context_index: &mut u32, type_index: &mut u32) {
        let mut rci = !0u32;
        let mut rti = !0u32;
        let mut rem = name;
        let mut node = self.pi_u32(20); // root_offset
        loop {
            let sep = find_byte(rem, b'.');
            let e = self.node_f(node, 0);
            if self.entry_f(e, 2) != !0 {
                rci = self.entry_f(e, 2);
            }
            if self.entry_f(e, 3) != !0 {
                rti = self.entry_f(e, 3);
            }
            self.check_prefix(rem, node, &mut rci, &mut rti);
            let sep = match sep {
                Some(sep) => sep,
                None => break,
            };
            let mut child = 0u32;
            if !self.find_child(node, rem, sep as u32, &mut child) {
                break;
            }
            node = child;
            rem = &rem[sep + 1..];
        }
        let n = self.node_f(node, 5);
        let arr = self.node_f(node, 6);
        let mut i = 0u32;
        while i < n && i < 100000 {
            let e = self.pi_u32(arr.wrapping_add(4 * i));
            i += 1;
            if self.pi_str(self.entry_f(e, 0)) == rem {
                *context_index = if self.entry_f(e, 2) != !0 {
                    self.entry_f(e, 2)
                } else {
                    rci
                };
                *type_index = if self.entry_f(e, 3) != !0 {
                    self.entry_f(e, 3)
                } else {
                    rti
                };
                return;
            }
        }
        self.check_prefix(rem, node, &mut rci, &mut rti);
        *context_index = rci;
        *type_index = rti;
    }

    // ------------------------------------------------------------- the areas

    fn prop_foreach(&self, f: &mut dyn FnMut(&[u8], &[u8])) {
        for area in &self.areas {
            if !area.base.is_null() {
                unsafe { foreach_bt(area.base, pa_obj(area.base, 0), f) };
            }
        }
    }

    fn area_for(&self, name: &[u8]) -> *mut u8 {
        let mut ci = 0u32;
        let mut ti = 0u32;
        self.pi_lookup(name, &mut ci, &mut ti);
        match self.areas.get(ci as usize) {
            Some(area) if ci != !0 && !area.base.is_null() => area.base,
            _ => {
                plog!("Could not find context for property \"", name, "\"");
                ptr::null_mut()
            }
        }
    }

    fn prop_find(&self, name: &[u8]) -> *mut u8 {
        let b = self.area_for(name);
        if b.is_null() {
            ptr::null_mut()
        } else {
            unsafe { find_property(b, name, &[], false) }
        }
    }

    fn bump_global_serial(&self) {
        unsafe {
            let s = pa_serial(self.serial_area);
            s.store(s.load(Ordering::Relaxed).wrapping_add(1), Ordering::Release);
            futex_wake_all(s.as_ptr().cast());
        }
    }

    // SystemProperties::Update, step for step.
    fn prop_update(&self, pi: *mut u8, value: &[u8]) -> c_int {
        let len = value.len() as u32;
        if len >= PROP_VALUE_MAX {
            return -1;
        }
        unsafe {
            let b = self.area_for(cstr_ptr(pi.add(PROP_INFO_HDR as usize)));
            if b.is_null() {
                return -1;
            }
            let mut serial = au32(pi).load(Ordering::Relaxed);
            let old_len = serial >> 24;
            // the dirty backup area
            ptr::copy_nonoverlapping(
                pi.add(4).cast_const(),
                pa_data(b).add(PROP_BT_HDR as usize),
                old_len as usize + 1,
            );
            fence(Ordering::Release);
            serial |= 1;
            au32(pi).store(serial, Ordering::Relaxed);
            ptr::copy_nonoverlapping(value.as_ptr(), pi.add(4), value.len());
            *pi.add(4 + value.len()) = 0;
            fence(Ordering::Release);
            au32(pi).store(
                (len << 24) | (serial.wrapping_add(1) & 0xffffff),
                Ordering::Relaxed,
            );
            futex_wake_all(pi.cast());
        }
        self.bump_global_serial();
        0
    }

    // SystemProperties::Add.
    fn prop_add(&self, name: &[u8], value: &[u8]) -> c_int {
        if value.len() as u32 >= PROP_VALUE_MAX && !name.starts_with(b"ro.") {
            return -1;
        }
        if name.is_empty() {
            return -1;
        }
        let b = self.area_for(name);
        if b.is_null() {
            return -1;
        }
        if unsafe { find_property(b, name, value, true) }.is_null() {
            return -1;
        }
        self.bump_global_serial();
        0
    }

    // The value as it is now (a copy), or None when the property is not set.
    fn get_prop_opt(&self, name: &[u8]) -> Option<Vec<u8>> {
        let pi = self.prop_find(name);
        if pi.is_null() {
            None
        } else {
            Some(unsafe { cstr_ptr(prop_value(pi)).to_vec() })
        }
    }

    fn get_prop(&self, name: &[u8], def: &[u8]) -> Vec<u8> {
        self.get_prop_opt(name).unwrap_or_else(|| def.to_vec())
    }

    // An init to hand control messages to and to tell about every property that
    // changes: LXRT_PROPERTY_INIT names a Unix datagram socket of this user
    // (scripts/android-boot.py binds one). Android's init is both the property
    // service and the one that acts on ctl.* and "on property:" triggers; here
    // they are separate processes, and this is the line between them. One
    // datagram per event, sent without waiting: "ctl\0<cmd>\0<value>\0<pid>" or
    // "set\0<name>\0<value>\0<pid>". An init that is not reading loses them.
    fn init_notify(&self, kind: &[u8], a: &[u8], b: &[u8], pid: c_int) -> bool {
        if self.init_fd < 0 {
            return false;
        }
        let (buf, full) = snp(2048, cat!(kind, [0u8], a, [0u8], b, [0u8], pid));
        let n = if full >= 2048 { 2047 } else { full };
        unsafe {
            sendto(
                self.init_fd,
                buf.as_ptr().cast(),
                n,
                MSG_DONTWAIT,
                &self.init_sa,
                size_of::<SockaddrUn>() as u32,
            ) == n as isize
        }
    }

    fn init_channel_open(&mut self) {
        let p = match env(b"LXRT_PROPERTY_INIT") {
            Some(p) if !p.is_empty() => p,
            _ => return,
        };
        let mut st = new_stat();
        // The socket must be this user's own: nothing else is told about
        // properties or asked to start services.
        if sys_lstat(&p, &mut st) != 0 || !is_sock(&st) || st.st_uid != unsafe { getuid() } {
            plog!(
                "LXRT_PROPERTY_INIT=",
                p,
                ": not a socket of this user; ctl.* stays refused"
            );
            return;
        }
        self.init_sa.sun_family = AF_UNIX;
        let (path, full) = snp(self.init_sa.sun_path.len(), p.clone());
        self.init_sa.sun_path[..path.len()].copy_from_slice(&path);
        self.init_sa.sun_path[path.len()] = 0;
        if full >= self.init_sa.sun_path.len() {
            plog!("LXRT_PROPERTY_INIT=", p, ": path too long");
            return;
        }
        self.init_fd = unsafe { socket(AF_UNIX as c_int, SOCK_DGRAM, 0) };
        if self.init_fd >= 0 {
            unsafe { fcntl(self.init_fd, F_SETFD, FD_CLOEXEC) };
            plog!(
                "control messages and property changes go to the init at ",
                p
            );
        }
    }

    // init's PropertySet.
    fn property_set(&mut self, name: &[u8], value: &[u8], error: &mut Vec<u8>) -> u32 {
        let vl = value.len();
        if !legal_name(name) {
            set_error(error, "Illegal property name");
            return PROP_ERROR_INVALID_NAME;
        }
        if vl as u32 >= PROP_VALUE_MAX && !name.starts_with(b"ro.") {
            set_error(error, "Property value too long");
            return PROP_ERROR_INVALID_VALUE;
        }
        if !valid_utf8(value) {
            set_error(error, "Value is not a UTF8 encoded string");
            return PROP_ERROR_INVALID_VALUE;
        }
        let pi = self.prop_find(name);
        if !pi.is_null() {
            if name.starts_with(b"ro.") {
                set_error(error, "Read-only property was already set");
                return PROP_ERROR_READ_ONLY_PROPERTY;
            }
            self.prop_update(pi, value);
        } else if self.prop_add(name, value) < 0 {
            set_error(error, "__system_property_add failed");
            return PROP_ERROR_SET_FAILED;
        }
        if self.persist_loaded && name.starts_with(b"persist.") {
            self.write_persistent(name, value);
        }
        self.init_notify(b"set", name, value, 0);
        PROP_SUCCESS
    }

    // CheckPermissions without the SELinux check: the name, and the value
    // against the type property_contexts gives the name.
    fn check_permissions(&self, name: &[u8], value: &[u8], error: &mut Vec<u8>) -> u32 {
        if !legal_name(name) {
            set_error(error, "Illegal property name");
            return PROP_ERROR_INVALID_NAME;
        }
        if name.starts_with(b"ctl.") {
            return PROP_SUCCESS;
        }
        let mut ci = 0u32;
        let mut ti = 0u32;
        self.pi_lookup(name, &mut ci, &mut ti);
        let typ: Option<&[u8]> = if ti == !0 {
            None
        } else {
            Some(self.pi_type(ti))
        };
        if !check_type(typ, value) {
            // A 128-byte message buffer in the original.
            *error = snp(
                128,
                cat!(
                    "Property type check failed, value doesn't match expected type '",
                    typ.unwrap_or(b"(null)"),
                    "'"
                ),
            )
            .0;
            return PROP_ERROR_INVALID_VALUE;
        }
        PROP_SUCCESS
    }

    // HandlePropertySet. `pid` is the caller's (0 for the service itself).
    fn handle_set(&mut self, name: &[u8], value: &[u8], pid: c_int, error: &mut Vec<u8>) -> u32 {
        let r = self.check_permissions(name, value, error);
        if r != PROP_SUCCESS {
            return r;
        }
        if name.starts_with(b"ctl.") {
            // Android's init queues the message and answers success; whether the
            // service exists is its business (HandleControlMessage logs it).
            if self.init_notify(b"ctl", &name[4..], value, pid) {
                plog!(
                    "Received control message '",
                    &name[4..],
                    "' for '",
                    value,
                    "' from pid ",
                    pid,
                    ": passed to init"
                );
                return PROP_SUCCESS;
            }
            // No init here to start, stop or restart a service.
            plog!(
                "Received control message '",
                &name[4..],
                "' for '",
                value,
                "' from pid ",
                pid,
                ": refused, no init to act on it"
            );
            set_error(error, "no init to handle control messages");
            return PROP_ERROR_HANDLE_CONTROL_MESSAGE;
        }
        if name == b"sys.powerctl" {
            plog!(
                "Received sys.powerctl='",
                value,
                "' from pid: ",
                pid,
                " (no reboot or shutdown is done here)"
            );
        }
        if name == b"selinux.restorecon_recursive" && pid != 0 && !value.is_empty() {
            plog!(
                "restorecon of '",
                value,
                "' requested by pid ",
                pid,
                ": no SELinux labels here, done"
            );
        }
        self.property_set(name, value, error)
    }

    fn init_set(&mut self, name: &[u8], value: &[u8]) {
        let mut err = Vec::new();
        if self.handle_set(name, value, 0, &mut err) != PROP_SUCCESS {
            plog!("Init cannot set '", name, "' to '", value, "': ", err);
        }
    }

    // LoadPersistentPropertyFile: false (and *out untouched) when the file is
    // missing or unreadable; a file that does not parse is deleted, as init
    // does, so the next write starts clean. A stale .tmp is removed first.
    fn load_persistent_file(&self, out: &mut Vec<Kv>) -> bool {
        let (tmpf, _) = snp(PATH_MAX + 8, cat!(self.persist, ".tmp"));
        if sys_unlink(&tmpf) == 0 {
            plog!(
                "Found temporary property file while attempting to persistent system properties",
                " a previous persistent property write may have failed"
            );
        }
        let fd = sys_open(&self.persist, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
        if fd < 0 {
            return false;
        }
        let mut st = new_stat();
        if unsafe { fstat(fd, &mut st) } != 0 || !is_reg(&st) || st.st_size > (16 << 20) {
            unsafe { close(fd) };
            return false;
        }
        let mut d = vec![0u8; st.st_size as usize];
        let got = unsafe { read(fd, d.as_mut_ptr().cast(), d.len()) };
        unsafe { close(fd) };
        if got as i64 != st.st_size {
            return false;
        }
        let mut tmp: Vec<Kv> = Vec::new();
        let mut p = 0usize;
        let end = d.len();
        let mut ok = true;
        while ok && p < end {
            let mut key = 0u64;
            let mut len = 0u64;
            if !pb_varint(&d, &mut p, end, &mut key) {
                ok = false;
                break;
            }
            if key != ((1 << 3) | 2) {
                ok = pb_skip(&d, &mut p, end, (key & 7) as c_int);
                continue;
            }
            if !pb_varint(&d, &mut p, end, &mut len) || ((end - p) as u64) < len {
                ok = false;
                break;
            }
            let mut r = p;
            let rend = p + len as usize;
            p = rend;
            let mut name: Option<Vec<u8>> = None;
            let mut value: Option<Vec<u8>> = None;
            while ok && r < rend {
                let mut k2 = 0u64;
                let mut l2 = 0u64;
                if !pb_varint(&d, &mut r, rend, &mut k2) {
                    ok = false;
                    break;
                }
                if (k2 & 7) != 2 || ((k2 >> 3) != 1 && (k2 >> 3) != 2) {
                    ok = pb_skip(&d, &mut r, rend, (k2 & 7) as c_int);
                    continue;
                }
                if !pb_varint(&d, &mut r, rend, &mut l2) || ((rend - r) as u64) < l2 {
                    ok = false;
                    break;
                }
                // The record's bytes as a C string.
                let s = cstr(&d[r..r + l2 as usize]).to_vec();
                r += l2 as usize;
                if (k2 >> 3) == 1 {
                    name = Some(s);
                } else {
                    value = Some(s);
                }
            }
            if ok {
                kv_set(
                    &mut tmp,
                    name.as_deref().unwrap_or(b""),
                    value.as_deref().unwrap_or(b""),
                    false,
                );
            }
        }
        drop(d);
        if !ok {
            plog!(
                "Unable to parse persistent property file ",
                self.persist,
                ": Could not parse protobuf (deleted)"
            );
            sys_unlink(&self.persist);
            return false;
        }
        for item in &tmp {
            kv_set(out, &item.k, &item.v, false);
        }
        true
    }

    fn write_persistent_file(&self, l: &[Kv]) -> bool {
        let mut cap = 16usize;
        for item in l {
            cap += item.k.len() + item.v.len() + 32;
        }
        let mut buf: Vec<u8> = Vec::with_capacity(cap);
        for item in l {
            let kl = item.k.len();
            let vl = item.v.len();
            let rl = 1 + varint_len(kl as u64) + kl + 1 + varint_len(vl as u64) + vl;
            buf.push((1 << 3) | 2);
            pb_put_varint(&mut buf, rl as u64);
            buf.push((1 << 3) | 2);
            pb_put_varint(&mut buf, kl as u64);
            buf.extend_from_slice(&item.k);
            buf.push((2 << 3) | 2);
            pb_put_varint(&mut buf, vl as u64);
            buf.extend_from_slice(&item.v);
        }
        let (tmp, _) = snp(PATH_MAX + 8, cat!(self.persist, ".tmp"));
        let fd = sys_open(
            &tmp,
            O_WRONLY | O_CREAT | O_NOFOLLOW | O_TRUNC | O_CLOEXEC,
            0o600,
        );
        let mut ok =
            fd >= 0 && unsafe { write(fd, buf.as_ptr().cast(), buf.len()) } == buf.len() as isize;
        if fd >= 0 {
            unsafe {
                fsync(fd);
                close(fd);
            }
        }
        drop(buf);
        if ok && sys_rename(&tmp, &self.persist) != 0 {
            ok = false;
        }
        if !ok {
            let e = errno();
            plog!(
                "Could not store persistent properties in ",
                self.persist,
                ": ",
                errstr(e)
            );
            sys_unlink(&tmp);
            return false;
        }
        let mut dir = self.persist.clone();
        if let Some(sl) = dir.iter().rposition(|&c| c == b'/') {
            dir.truncate(sl);
            let dfd = sys_open(&dir, O_RDONLY | O_CLOEXEC, 0);
            if dfd >= 0 {
                unsafe {
                    fsync(dfd);
                    close(dfd);
                }
            }
        }
        true
    }

    fn write_persistent(&self, name: &[u8], value: &[u8]) {
        let mut l: Vec<Kv> = Vec::new();
        if !self.load_persistent_file(&mut l) {
            plog!("Recovering persistent properties from memory");
            l.clear();
            self.prop_foreach(&mut |n: &[u8], v: &[u8]| {
                if n.starts_with(b"persist.") {
                    kv_set(&mut l, n, v, false);
                }
            });
        }
        kv_set(&mut l, name, value, false);
        self.write_persistent_file(&l);
    }

    fn expand_props(&self, input: &[u8]) -> Option<Vec<u8>> {
        // init's ExpandProps: $$, ${name} and ${name:-default}, looked up in
        // what is set so far.
        let mut out: Vec<u8> = Vec::with_capacity(input.len() + 256);
        let mut p = 0usize;
        while p < input.len() {
            let next = b_at(input, p + 1);
            if input[p] == b'$' && next == b'$' {
                out.push(b'$');
                p += 2;
            } else if input[p] == b'$' && next == b'{' {
                let e = match find_byte(&input[p..], b'}') {
                    Some(e) => p + e,
                    None => {
                        plog!("unterminated {: ", input);
                        return None;
                    }
                };
                let nl = e - p - 2;
                if nl >= 256 {
                    return None;
                }
                let mut name = &input[p + 2..e];
                let mut def: Option<&[u8]> = None;
                if let Some(dd) = find_sub(name, b":-") {
                    def = Some(&name[dd + 2..]);
                    name = &name[..dd];
                }
                let mut v = self.get_prop_opt(name);
                if v.as_ref().map_or(true, |v| v.is_empty()) {
                    v = def.map(|d| d.to_vec());
                }
                let v = match v {
                    Some(v) => v,
                    None => {
                        plog!(
                            "property '",
                            name,
                            "' doesn't exist while expanding '",
                            input,
                            "'"
                        );
                        return None;
                    }
                };
                out.extend_from_slice(&v);
                p = e + 1;
            } else if input[p] == b'$' {
                plog!(
                    "unexpected end of string in '",
                    input,
                    "', looking for }"
                );
                return None;
            } else {
                out.push(input[p]);
                p += 1;
            }
        }
        Some(out)
    }

    // init's LoadProperties, including its in-place trimming. `d` ends with
    // a NUL.
    fn load_properties(
        &self,
        d: &mut [u8],
        filter: Option<&[u8]>,
        fname: &[u8],
        props: &mut Vec<Kv>,
    ) {
        let filter = filter.unwrap_or(b"");
        let flen = filter.len();
        let mut sol = 0usize;
        while let Some(nl) = b_strchr(d, sol, b'\n') {
            let mut key = sol;
            b_set(d, nl, 0);
            let eol = nl + 1;
            sol = eol;
            while isspace(b_at(d, key)) {
                key += 1;
            }
            if b_at(d, key) == b'#' {
                continue;
            }
            let mut tmp = eol as isize - 2;
            while tmp > key as isize && isspace(b_at(d, tmp as usize)) {
                b_set(d, tmp as usize, 0);
                tmp -= 1;
            }
            if strncmp(b_cstr(d, key), b"import ", 7) == 0 && flen == 0 {
                let mut name_at = key + 7;
                while isspace(b_at(d, name_at)) {
                    name_at += 1;
                }
                let mut sub_filter: Option<Vec<u8>> = None;
                if let Some(space) = b_strchr(d, name_at, b' ') {
                    b_set(d, space, 0);
                    let mut k = space + 1;
                    while isspace(b_at(d, k)) {
                        k += 1;
                    }
                    sub_filter = Some(b_cstr(d, k).to_vec());
                }
                let file = b_cstr(d, name_at).to_vec();
                let exp = match self.expand_props(&file) {
                    Some(exp) => exp,
                    None => {
                        plog!("Could not expand filename '", file, "'");
                        continue;
                    }
                };
                self.load_properties_from_file(&exp, sub_filter.as_deref(), props);
            } else {
                let mut value = match b_strchr(d, key, b'=') {
                    Some(eq) => eq,
                    None => continue,
                };
                b_set(d, value, 0);
                value += 1;
                tmp = value as isize - 2;
                while tmp > key as isize && isspace(b_at(d, tmp as usize)) {
                    b_set(d, tmp as usize, 0);
                    tmp -= 1;
                }
                while isspace(b_at(d, value)) {
                    value += 1;
                }
                let key = b_cstr(d, key);
                let value = b_cstr(d, value);
                if flen > 0 {
                    if filter[flen - 1] == b'*' {
                        if strncmp(key, filter, flen - 1) != 0 {
                            continue;
                        }
                    } else if key != filter {
                        continue;
                    }
                }
                if key.starts_with(b"ctl.")
                    || key == b"sys.powerctl"
                    || key == b"selinux.restorecon_recursive"
                {
                    plog!(
                        "Ignoring disallowed property '",
                        key,
                        "' with special meaning in prop file '",
                        fname,
                        "'"
                    );
                    continue;
                }
                let mut err = Vec::new();
                if self.check_permissions(key, value, &mut err) == PROP_SUCCESS {
                    kv_set(props, key, value, true);
                } else {
                    plog!(
                        "Do not have permissions to set '",
                        key,
                        "' to '",
                        value,
                        "' in property file '",
                        fname,
                        "': ",
                        err
                    );
                }
            }
        }
    }

    fn load_properties_from_file(
        &self,
        fname: &[u8],
        filter: Option<&[u8]>,
        props: &mut Vec<Kv>,
    ) -> bool {
        let mut d = match self.read_guest_file(fname) {
            Some(d) => d,
            None => {
                plog!("Couldn't load property file '", fname, "'");
                return false;
            }
        };
        d.push(b'\n');
        d.push(0);
        self.load_properties(&mut d, filter, fname, props);
        true
    }

    // property_initialize_ro_product_props.
    fn derive_ro_product(&mut self) {
        const PROPS: [&[u8]; 5] = [b"brand", b"device", b"manufacturer", b"model", b"name"];
        const ALLOWED: [&[u8]; 5] = [b"odm", b"product", b"system_ext", b"system", b"vendor"];
        const DEFLT: &[u8] = b"product,odm,vendor,system_ext,system";
        let mut order = self.get_prop(b"ro.product.property_source_order", b"");
        if !order.is_empty() {
            // android::base::Split: empty pieces count (and are not allowed).
            let bad = order
                .split(|&c| c == b',')
                .any(|s| !ALLOWED.iter().any(|a| *a == s));
            if bad {
                plog!("Found unexpected source in ro.product.property_source_order; using the default property source order");
                order = DEFLT.to_vec();
            }
        } else {
            order = DEFLT.to_vec();
        }
        for prop in PROPS {
            let (base, _) = snp(64, cat!("ro.product.", prop));
            if !self.get_prop(&base, b"").is_empty() {
                continue;
            }
            for s in order.split(|&c| c == b',') {
                let (target, _) = snp(96, cat!("ro.product.", s, ".", prop));
                // A copy of any length: ro.* values may be long ones.
                let val = self.get_prop(&target, b"");
                if !val.is_empty() {
                    plog!(
                        "Setting product property ",
                        base,
                        " to '",
                        val,
                        "' (from ",
                        target,
                        ")"
                    );
                    let mut err = Vec::new();
                    let r = self.property_set(&base, &val, &mut err);
                    if r != PROP_SUCCESS {
                        plog!(
                            "Error setting product property ",
                            base,
                            ": err=",
                            r,
                            " (",
                            err,
                            ")"
                        );
                    }
                    break;
                }
            }
        }
    }

    // property_derive_build_fingerprint: only when the image does not set one,
    // from the image's own values.
    fn derive_fingerprint(&mut self) {
        if !self.get_prop(b"ro.build.fingerprint", b"").is_empty() {
            return;
        }
        let unknown: &[u8] = b"unknown";
        let fp = cat!(
            self.get_prop(b"ro.product.brand", unknown),
            "/",
            self.get_prop(b"ro.product.name", unknown),
            "/",
            self.get_prop(b"ro.product.device", unknown),
            ":",
            self.get_prop(b"ro.build.version.release", unknown),
            "/",
            self.get_prop(b"ro.build.id", unknown),
            "/",
            self.get_prop(b"ro.build.version.incremental", unknown),
            ":",
            self.get_prop(b"ro.build.type", unknown),
            "/",
            self.get_prop(b"ro.build.tags", unknown)
        );
        plog!("Setting property 'ro.build.fingerprint' to '", fp, "'");
        let mut err = Vec::new();
        let r = self.property_set(b"ro.build.fingerprint", &fp, &mut err);
        if r != PROP_SUCCESS {
            plog!(
                "Error setting property 'ro.build.fingerprint': err=",
                r,
                " (",
                err,
                ")"
            );
        }
    }

    // ProcessKernelCmdline + ExportKernelBootProps. There is no kernel command
    // line: LXRT_PROPERTY_BOOTARGS stands for it ("androidboot.x=y ..."), empty
    // by default, and "qemu" in it is not honoured (SteamARM is not the
    // emulator).
    fn boot_props(&mut self) {
        if let Some(args) = env(b"LXRT_PROPERTY_BOOTARGS") {
            for t in args
                .split(|&c| c == b' ' || c == b'\t')
                .filter(|t| !t.is_empty())
            {
                let eq = match find_byte(t, b'=') {
                    Some(eq) => eq,
                    None => continue,
                };
                if !t.starts_with(b"androidboot.") {
                    continue;
                }
                let (name, _) = snp(128, cat!("ro.boot.", &t[12..eq]));
                self.init_set(&name, &t[eq + 1..]);
            }
        }
        const MAP: [(&[u8], &[u8], &[u8]); 6] = [
            (b"ro.boot.serialno", b"ro.serialno", b""),
            (b"ro.boot.mode", b"ro.bootmode", b"unknown"),
            (b"ro.boot.baseband", b"ro.baseband", b"unknown"),
            (b"ro.boot.bootloader", b"ro.bootloader", b"unknown"),
            (b"ro.boot.hardware", b"ro.hardware", b"unknown"),
            (b"ro.boot.revision", b"ro.revision", b"0"),
        ];
        for (src, dst, def) in MAP {
            let v = self.get_prop(src, def);
            if !v.is_empty() {
                self.init_set(dst, &v);
            }
        }
    }

    // PropertyLoadBootDefaults.
    fn load_boot_defaults(&mut self) {
        let mut props: Vec<Kv> = Vec::new();
        if !self.load_properties_from_file(b"/system/etc/prop.default", None, &mut props)
            && !self.load_properties_from_file(b"/prop.default", None, &mut props)
        {
            self.load_properties_from_file(b"/default.prop", None, &mut props);
        }
        self.load_properties_from_file(b"/system/build.prop", None, &mut props);
        self.load_properties_from_file(b"/system_ext/build.prop", None, &mut props);
        self.load_properties_from_file(b"/vendor/default.prop", None, &mut props);
        self.load_properties_from_file(b"/vendor/build.prop", None, &mut props);
        if self.vendor_api >= 29 {
            self.load_properties_from_file(b"/odm/etc/build.prop", None, &mut props);
        } else {
            self.load_properties_from_file(b"/odm/default.prop", None, &mut props);
            self.load_properties_from_file(b"/odm/build.prop", None, &mut props);
        }
        self.load_properties_from_file(b"/product/build.prop", None, &mut props);
        self.load_properties_from_file(b"/factory/factory.prop", Some(b"ro.*"), &mut props);
        if self.guest_readable(b"/debug_ramdisk/adb_debug.prop") {
            self.load_properties_from_file(b"/debug_ramdisk/adb_debug.prop", None, &mut props);
        }
        // LXRT_PROPERTY_HOST: the host's own .prop file (a host path), loaded
        // last among the image's files and with their rules -- what Waydroid's
        // container manager does with the waydroid.prop it writes for each boot.
        // scripts/android-boot.py puts there what this host can do that the
        // image cannot know, e.g. that it runs no 32-bit code:
        // ro.product.cpu.abilist32 empty (the framework otherwise waited for, and
        // tried to start processes in, a 32-bit zygote: benchmarks/stage28-
        // android-apk.txt).
        if let Some(hostprop) = env(b"LXRT_PROPERTY_HOST").filter(|h| !h.is_empty()) {
            let fd = sys_open(&hostprop, O_RDONLY | O_CLOEXEC, 0);
            let mut hs = new_stat();
            if fd >= 0
                && unsafe { fstat(fd, &mut hs) } == 0
                && is_reg(&hs)
                && hs.st_uid == unsafe { getuid() }
                && hs.st_size < (1 << 20)
            {
                let size = hs.st_size as usize;
                let mut d = vec![0u8; size + 2];
                let mut got = unsafe { read(fd, d.as_mut_ptr().cast(), size) };
                if got < 0 {
                    got = 0;
                }
                d[got as usize] = b'\n';
                d[got as usize + 1] = 0;
                self.load_properties(&mut d, None, &hostprop, &mut props);
                plog!("host properties from ", hostprop);
            } else {
                plog!(
                    "LXRT_PROPERTY_HOST=",
                    hostprop,
                    ": not a regular file of this user; ignored"
                );
            }
            if fd >= 0 {
                unsafe { close(fd) };
            }
        }
        // std::map order.
        props.sort_by(|a, b| a.k.cmp(&b.k));
        let mut set: c_int = 0;
        for item in &props {
            let mut err = Vec::new();
            if self.property_set(&item.k, &item.v, &mut err) != PROP_SUCCESS {
                plog!(
                    "Could not set '",
                    item.k,
                    "' to '",
                    item.v,
                    "' while loading .prop files: ",
                    err
                );
            } else {
                set += 1;
            }
        }
        plog!(
            "loaded ",
            set,
            " properties from the image's .prop files (",
            props.len() as c_int,
            " entries)"
        );
        drop(props);
        self.derive_ro_product();
        self.derive_fingerprint();
    }

    // init's legacy format, one file per property in the persistent file's
    // directory (/data/property/persist.*), read when the protobuf file cannot
    // be. init requires each to be root:root, closed to group and others and
    // not a hard link; here the owner is the Mac user every file has.
    fn legacy_dir(&self) -> Vec<u8> {
        let (mut dir, _) = snp(PATH_MAX, self.persist.clone());
        if let Some(sl) = dir.iter().rposition(|&c| c == b'/') {
            dir.truncate(sl);
        }
        dir
    }

    fn load_legacy_persistent(&self, out: &mut Vec<Kv>) -> bool {
        let dir = self.legacy_dir();
        let d = sys_opendir(&dir);
        if d.is_null() {
            plog!(
                "Unable to open persistent property directory \"",
                dir,
                "\""
            );
            return false;
        }
        unsafe {
            loop {
                let de = readdir(d);
                if de.is_null() {
                    break;
                }
                let d_name = ptr::addr_of!((*de).d_name).cast::<c_char>();
                let name = CStr::from_ptr(d_name).to_bytes().to_vec();
                if !name.starts_with(b"persist.") || (*de).d_type != DT_REG {
                    continue;
                }
                let fd = openat(dirfd(d), d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
                if fd < 0 {
                    continue;
                }
                let mut sb = new_stat();
                if fstat(fd, &mut sb) != 0
                    || (sb.st_mode & 0o077) != 0
                    || sb.st_uid != getuid()
                    || sb.st_nlink != 1
                    || sb.st_size > (1 << 20)
                {
                    plog!("skipping insecure property file ", name);
                    close(fd);
                    continue;
                }
                let mut v = vec![0u8; sb.st_size as usize + 1];
                let got = read(fd, v.as_mut_ptr().cast(), sb.st_size as usize);
                close(fd);
                if got >= 0 {
                    v[got as usize] = 0;
                    kv_set(out, &name, cstr(&v), false);
                }
            }
            closedir(d);
        }
        true
    }

    fn remove_legacy_persistent(&self) {
        let dir = self.legacy_dir();
        let d = sys_opendir(&dir);
        if d.is_null() {
            return;
        }
        unsafe {
            loop {
                let de = readdir(d);
                if de.is_null() {
                    break;
                }
                let d_name = ptr::addr_of!((*de).d_name).cast::<c_char>();
                if CStr::from_ptr(d_name).to_bytes().starts_with(b"persist.")
                    && (*de).d_type == DT_REG
                {
                    unlinkat(dirfd(d), d_name, 0);
                }
            }
            closedir(d);
        }
    }

    // The persistent part of init's boot: /data/local.prop on a debuggable
    // build (load_override_properties), then the stored persist.* values, then
    // ro.persistent_properties.ready.
    fn load_persistent(&mut self) {
        if self.get_prop(b"ro.debuggable", b"0") == b"1" {
            let mut l: Vec<Kv> = Vec::new();
            if self.guest_readable(b"/data/local.prop") {
                self.load_properties_from_file(b"/data/local.prop", None, &mut l);
                l.sort_by(|a, b| a.k.cmp(&b.k));
                for item in &l {
                    let mut err = Vec::new();
                    if self.property_set(&item.k, &item.v, &mut err) != PROP_SUCCESS {
                        plog!(
                            "Could not set '",
                            item.k,
                            "' to '",
                            item.v,
                            "' in /data/local.prop: ",
                            err
                        );
                    }
                }
            }
        }
        let mut l: Vec<Kv> = Vec::new();
        if !self.load_persistent_file(&mut l) {
            plog!("Could not load single persistent property file, trying legacy directory");
            if self.load_legacy_persistent(&mut l) {
                // Migrated as init migrates them: one protobuf file, then the
                // old per-property files go.
                if self.write_persistent_file(&l) {
                    self.remove_legacy_persistent();
                }
            }
        }
        for item in &l {
            self.init_set(&item.k, &item.v);
        }
        plog!(
            l.len() as c_int,
            " persistent properties from ",
            self.persist
        );
        drop(l);
        self.init_set(b"ro.persistent_properties.ready", b"true");
        self.persist_loaded = true;
    }

    // Set up the areas from property_info's contexts: the files are created
    // (fresh boot) or opened as they are (a service that starts again).
    fn open_areas(&mut self, pdir: &[u8], create: bool) -> bool {
        let nareas = self.pi_num_contexts();
        if nareas == !0 || nareas > 4096 {
            plog!("property_info: bad context count");
            return false;
        }
        self.areas = (0..nareas)
            .map(|_| Area {
                ctx: Vec::new(),
                base: ptr::null_mut(),
            })
            .collect();
        for i in 0..nareas {
            let ctx = self.pi_context(i).to_vec();
            if ctx.is_empty() || ctx.contains(&b'/') || ctx == b"." || ctx == b".." {
                plog!("property_info: bad context name '", ctx, "'");
                return false;
            }
            let (path, _) = snp(PATH_MAX, cat!(pdir, "/", ctx));
            let base = map_area_rw(&path, create);
            if let Some(area) = self.areas.get_mut(i as usize) {
                area.ctx = ctx;
                area.base = base;
            }
            if base.is_null() {
                return false;
            }
        }
        let (path, _) = snp(PATH_MAX, cat!(pdir, "/properties_serial"));
        self.serial_area = map_area_rw(&path, create);
        !self.serial_area.is_null()
    }

    fn load_property_info_file(&mut self, path: &[u8]) -> bool {
        let fd = sys_open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW, 0);
        if fd < 0 {
            return false;
        }
        let mut st = new_stat();
        if unsafe { fstat(fd, &mut st) } != 0 || st.st_size < 24 || st.st_size > (8 << 20) {
            unsafe { close(fd) };
            return false;
        }
        let len = st.st_size as usize;
        self.pi = vec![0u8; len];
        let ok = unsafe { read(fd, self.pi.as_mut_ptr().cast(), len) } == len as isize;
        unsafe { close(fd) };
        self.pi_size = len as u32;
        // PropertyInfoAreaFile::LoadPath's checks.
        ok && self.pi_u32(4) <= 1 && self.pi_u32(8) == self.pi_size
    }

    fn vendor_api_level(&self) -> c_int {
        // SelinuxGetVendorAndroidVersion: plat_sepolicy_vers.txt, "30.0".
        let v = match self.read_guest_file(b"/vendor/etc/selinux/plat_sepolicy_vers.txt") {
            Some(d) => c_atoi(&d),
            None => 0,
        };
        if v > 0 {
            v
        } else {
            10000
        }
    }

    // Boot: property_info, the areas, and every value init would set before
    // its first service starts. Built in __properties__.new and renamed into
    // place, so a guest never sees half an area set.
    fn boot(&mut self) -> bool {
        let (pdir, _) = snp(PATH_MAX, cat!(self.dir, "/__properties__"));
        let (tmpdir, _) = snp(PATH_MAX, cat!(self.dir, "/__properties__.new"));
        // A half-built directory from a service that died building it.
        let old = sys_opendir(&tmpdir);
        if !old.is_null() {
            unsafe {
                loop {
                    let de = readdir(old);
                    if de.is_null() {
                        break;
                    }
                    let name = CStr::from_ptr(ptr::addr_of!((*de).d_name).cast::<c_char>())
                        .to_bytes()
                        .to_vec();
                    if name.first() == Some(&b'.') {
                        continue;
                    }
                    let (path, _) = snp(PATH_MAX, cat!(tmpdir, "/", name));
                    sys_unlink(&path);
                }
                closedir(old);
            }
            with_path(&tmpdir, |p| unsafe { rmdir(p) });
        }
        if sys_mkdir(&tmpdir, 0o711) != 0 {
            let e = errno();
            plog!(tmpdir, ": ", errstr(e));
            return false;
        }

        // CreateSerializedPropertyInfo.
        let mut b = Builder {
            root: TNode::new(b"root"),
            ctxs: Vec::new(),
            types: Vec::new(),
        };
        b.root.ctx = Some(strset_add(&mut b.ctxs, b"u:object_r:default_prop:s0"));
        b.root.typ = Some(strset_add(&mut b.types, b"string"));
        let req = self.vendor_api >= 30;
        let mut ok = true;
        if !self.guest_readable(b"/system/etc/selinux/plat_property_contexts") {
            plog!(
                "no /system/etc/selinux/plat_property_contexts in ",
                self.root
            );
            return false;
        }
        self.parse_contexts(
            &mut b,
            b"/system/etc/selinux/plat_property_contexts",
            req,
            &mut ok,
        );
        if self.guest_readable(b"/system_ext/etc/selinux/system_ext_property_contexts") {
            self.parse_contexts(
                &mut b,
                b"/system_ext/etc/selinux/system_ext_property_contexts",
                req,
                &mut ok,
            );
        }
        if self.guest_readable(b"/vendor/etc/selinux/vendor_property_contexts") {
            self.parse_contexts(
                &mut b,
                b"/vendor/etc/selinux/vendor_property_contexts",
                req,
                &mut ok,
            );
        } else {
            self.parse_contexts(
                &mut b,
                b"/vendor/etc/selinux/nonplat_property_contexts",
                req,
                &mut ok,
            );
        }
        if self.guest_readable(b"/product/etc/selinux/product_property_contexts") {
            self.parse_contexts(
                &mut b,
                b"/product/etc/selinux/product_property_contexts",
                req,
                &mut ok,
            );
        }
        if self.guest_readable(b"/odm/etc/selinux/odm_property_contexts") {
            self.parse_contexts(
                &mut b,
                b"/odm/etc/selinux/odm_property_contexts",
                req,
                &mut ok,
            );
        }
        if !ok {
            return false;
        }
        let ser = serialize_trie(&mut b);
        let (path, _) = snp(PATH_MAX, cat!(tmpdir, "/property_info"));
        if !write_file(&path, &ser, 0o444) {
            let e = errno();
            plog!(path, ": ", errstr(e));
            return false;
        }
        drop(ser);
        if !self.load_property_info_file(&path) {
            plog!("cannot read back ", path);
            return false;
        }
        plog!(
            "property_info: ",
            self.pi_size,
            " bytes, ",
            self.pi_num_contexts(),
            " contexts"
        );

        // __system_property_area_init: every context's area and the serial one.
        if !self.open_areas(&tmpdir, true) {
            return false;
        }

        self.boot_props();
        self.load_boot_defaults();
        self.init_set(b"ro.property_service.version", b"2");
        self.load_persistent();

        if sys_rename(&tmpdir, &pdir) != 0 {
            let e = errno();
            plog!("rename ", tmpdir, ": ", errstr(e));
            return false;
        }
        true
    }

    fn close_areas(&mut self) {
        for area in &self.areas {
            if !area.base.is_null() {
                unsafe { munmap(area.base.cast(), PA_SIZE) };
            }
        }
        self.areas = Vec::new();
        if !self.serial_area.is_null() {
            unsafe { munmap(self.serial_area.cast(), PA_SIZE) };
        }
        self.serial_area = ptr::null_mut();
        self.pi = Vec::new();
        self.pi_size = 0;
    }

    fn adopt(&mut self) -> bool {
        let (pdir, _) = snp(PATH_MAX, cat!(self.dir, "/__properties__"));
        let (path, _) = snp(PATH_MAX, cat!(pdir, "/property_info"));
        if !self.load_property_info_file(&path) {
            return false;
        }
        if !self.open_areas(&pdir, false) {
            return false;
        }
        for area in &self.areas {
            if !unsafe { valid_area(area.base) } {
                plog!(
                    pdir,
                    "/",
                    area.ctx,
                    ": an offset points outside the area"
                );
                return false;
            }
        }
        self.persist_loaded = true;
        true
    }

    // handle_property_set_fd.
    fn handle_connection(&mut self, s: c_int, mut timeout: c_int) {
        unsafe {
            let mut cr: Xucred = zeroed();
            let mut cl = size_of::<Xucred>() as u32;
            let mut pid: c_int = 0;
            let mut pl = size_of::<c_int>() as u32;
            if getsockopt(
                s,
                SOL_LOCAL,
                LOCAL_PEERCRED,
                (&mut cr as *mut Xucred).cast(),
                &mut cl,
            ) != 0
                || cr.cr_uid != getuid()
            {
                plog!("sys_prop: connection from another user refused");
                return;
            }
            getsockopt(
                s,
                SOL_LOCAL,
                LOCAL_PEERPID,
                (&mut pid as *mut c_int).cast(),
                &mut pl,
            );
            // init: 2 s (kDefaultSocketTimeout) for the whole message.
            let mut cmd = 0u32;
            if !recv_fully(s, (&mut cmd as *mut u32).cast(), 4, &mut timeout) {
                plog!("sys_prop: error while reading command from the socket");
                send_u32(s, PROP_ERROR_READ_CMD);
                return;
            }
            if cmd == PROP_MSG_SETPROP {
                let mut name = [0u8; PROP_NAME_MAX];
                let mut value = [0u8; PROP_VALUE_MAX as usize];
                if !recv_fully(s, name.as_mut_ptr(), name.len(), &mut timeout)
                    || !recv_fully(s, value.as_mut_ptr(), value.len(), &mut timeout)
                {
                    plog!(
                        "sys_prop(PROP_MSG_SETPROP): error while reading name/value from the socket"
                    );
                    return;
                }
                name[PROP_NAME_MAX - 1] = 0;
                value[PROP_VALUE_MAX as usize - 1] = 0;
                let name = cstr(&name);
                let value = cstr(&value);
                let mut err = Vec::new();
                let r = self.handle_set(name, value, pid, &mut err);
                if r != PROP_SUCCESS {
                    plog!(
                        "Unable to set property '",
                        name,
                        "' from pid:",
                        pid,
                        ": ",
                        err
                    );
                } else {
                    plog!(
                        "set ",
                        name,
                        "=",
                        value,
                        " (pid ",
                        pid,
                        ", legacy protocol)"
                    );
                }
                return; // the reply is the close (POLLHUP)
            }
            if cmd == PROP_MSG_SETPROP2 {
                let name = recv_string(s, &mut timeout);
                let value = if name.is_some() {
                    recv_string(s, &mut timeout)
                } else {
                    None
                };
                let (name, value) = match (name, value) {
                    (Some(name), Some(value)) => (name, value),
                    _ => {
                        plog!(
                            "sys_prop(PROP_MSG_SETPROP2): error while reading name/value from the socket"
                        );
                        send_u32(s, PROP_ERROR_READ_DATA);
                        return;
                    }
                };
                let mut err = Vec::new();
                let r = self.handle_set(&name, &value, pid, &mut err);
                if r != PROP_SUCCESS {
                    plog!(
                        "Unable to set property '",
                        name,
                        "' from pid:",
                        pid,
                        ": ",
                        err
                    );
                } else {
                    plog!("set ", name, "=", value, " (pid ", pid, ")");
                }
                send_u32(s, r);
                return;
            }
            plog!("sys_prop: invalid command ", cmd);
            send_u32(s, PROP_ERROR_INVALID_CMD);
        }
    }

    unsafe fn main(&mut self, argc: c_int, argv: *mut *mut c_char) -> c_int {
        // argv: <lxrun> --property-service <dir> <root> [--daemon]
        if argc < 4 {
            stderr_write(b"usage: lxrun --property-service <dir> <android-root> [--daemon]\n");
            return 2;
        }
        let arg = |i: usize| -> Vec<u8> { CStr::from_ptr(*argv.add(i)).to_bytes().to_vec() };
        self.dir = arg(2);
        let daemonize = argc > 4 && arg(4) == b"--daemon";
        let mut resolved = [0u8; PATH_MAX];
        if realpath(*argv.add(3), resolved.as_mut_ptr().cast()).is_null() {
            let e = errno();
            stderr_write(&cat!("property service: ", arg(3), ": ", errstr(e), "\n"));
            return 1;
        }
        self.root = cstr(&resolved).to_vec();
        let mut dst = new_stat();
        if sys_lstat(&self.dir, &mut dst) != 0
            || !is_dir(&dst)
            || dst.st_uid != getuid()
            || (dst.st_mode & 0o077) != 0
        {
            stderr_write(&cat!(
                "property service: ",
                self.dir,
                " is not a private directory of this user\n"
            ));
            return 1;
        }
        let idle_s = match env(b"LXRT_PROPERTY_IDLE") {
            Some(idle) if !idle.is_empty() => c_atoi(&idle),
            _ => 60, // 0: never leave
        };
        let mut persist_fallback = false;
        match env(b"LXRT_PROPERTY_PERSIST") {
            Some(pe) if !pe.is_empty() => self.persist = snp(PATH_MAX, pe).0,
            _ => {
                // The root's /data is the guests' to change: a path that does not
                // resolve (a link loop, one too long) must not leave `d` unset. The
                // file then lives in the service's own directory, and the log says so.
                match self.root_path(b"/data/property") {
                    Some(d) => {
                        sys_mkdir(&d, 0o700);
                        self.persist = snp(PATH_MAX, cat!(d, "/persistent_properties")).0;
                    }
                    None => {
                        self.persist = snp(PATH_MAX, cat!(self.dir, "/persistent_properties")).0;
                        persist_fallback = true;
                    }
                }
            }
        }
        signal(SIGPIPE, SIG_IGN);
        signal(SIGTERM, on_term as extern "C" fn(c_int) as usize);
        signal(SIGINT, on_term as extern "C" fn(c_int) as usize);

        if daemonize {
            // The spawner gave us /dev/null: the log is where anything goes.
            let (logp, _) = snp(PATH_MAX, cat!(self.dir, "/service.log"));
            let mut lst = new_stat();
            let mut lfl = O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC;
            if sys_stat(&logp, &mut lst) == 0 && lst.st_size > (1 << 20) {
                lfl |= O_TRUNC;
            }
            let lfd = sys_open(&logp, lfl, 0o600);
            if lfd >= 0 {
                dup2(lfd, 2);
                close(lfd);
            }
        }
        setvbuf(__stderrp, ptr::null_mut(), IOLBF, 0);
        if persist_fallback {
            plog!(
                "the root's /data/property does not resolve; persistent properties in ",
                self.persist
            );
        }

        // One service per directory: the lock is held for the service's life. A
        // service that is leaving still holds it for a moment: wait a little.
        let (lockp, _) = snp(PATH_MAX, cat!(self.dir, "/service.lock"));
        let lk = sys_open(&lockp, O_RDWR | O_CREAT | O_CLOEXEC, 0o600);
        if lk < 0 {
            let e = errno();
            plog!(lockp, ": ", errstr(e));
            return 1;
        }
        let mut locked = false;
        let mut i = 0;
        while i < 100 && !locked {
            // up to 10 s
            if flock(lk, LOCK_EX | LOCK_NB) == 0 {
                locked = true;
            } else {
                usleep(100000);
            }
            i += 1;
        }
        if !locked {
            plog!("another property service holds ", lockp);
            return 1;
        }

        self.vendor_api = self.vendor_api_level();
        self.init_channel_open();
        let t0 = monotonic();
        let mut adopted = false;
        let (pdir, _) = snp(PATH_MAX, cat!(self.dir, "/__properties__"));
        let mut pst = new_stat();
        if sys_lstat(&pdir, &mut pst) == 0 {
            adopted = is_dir(&pst) && self.adopt();
            if !adopted {
                // Unusable state (no property_info, a bad area, not a
                // directory): set it aside -- guests that map it keep their
                // copy -- and boot afresh.
                let (aside, _) = snp(
                    PATH_MAX,
                    cat!(self.dir, "/__properties__.bad-", getpid()),
                );
                plog!(
                    "cannot adopt ",
                    pdir,
                    "; moved to ",
                    aside,
                    ", booting again"
                );
                self.close_areas();
                if sys_rename(&pdir, &aside) != 0 {
                    let e = errno();
                    plog!("rename ", pdir, ": ", errstr(e));
                    return 1;
                }
            }
        }
        if !adopted && !self.boot() {
            plog!("boot failed: no properties");
            return 1;
        }
        let t1 = monotonic();
        plog!(
            if adopted { "adopted" } else { "booted" },
            " ",
            self.dir,
            " in ",
            Fixed1(
                (t1.tv_sec - t0.tv_sec) as f64 * 1e3 + (t1.tv_nsec - t0.tv_nsec) as f64 / 1e6
            ),
            " ms (root ",
            self.root,
            ", persistent file ",
            self.persist,
            ")"
        );

        let mut sa = SockaddrUn {
            sun_len: 0,
            sun_family: AF_UNIX,
            sun_path: [0; 104],
        };
        let (sock_path, full) = snp(sa.sun_path.len(), cat!(self.dir, "/property_service"));
        if full >= sa.sun_path.len() {
            plog!(self.dir, ": socket path too long");
            return 1;
        }
        sa.sun_path[..sock_path.len()].copy_from_slice(&sock_path);
        let ls = socket(AF_UNIX as c_int, SOCK_STREAM, 0);
        if ls < 0 {
            let e = errno();
            plog!("socket: ", errstr(e));
            return 1;
        }
        fcntl(ls, F_SETFD, FD_CLOEXEC);
        sys_unlink(&sock_path);
        if bind(ls, &sa, size_of::<SockaddrUn>() as u32) != 0 || listen(ls, 64) != 0 {
            let e = errno();
            plog!("bind ", sock_path, ": ", errstr(e));
            return 1;
        }
        sys_chmod(&sock_path, 0o600);

        if daemonize {
            // The spawner waits for this first process: once it exits, the areas
            // exist and the socket listens, and the service is nobody's child.
            let pid = fork();
            if pid < 0 {
                return 1;
            }
            if pid > 0 {
                _exit(0);
            }
            setsid();
        }
        let (pidp, _) = snp(PATH_MAX, cat!(self.dir, "/service.pid"));
        let (pidv, _) = snp(32, cat!(getpid(), "\n"));
        write_file(&pidp, &pidv, 0o600);
        plog!(
            "serving ",
            sock_path,
            " (pid ",
            getpid(),
            ", leaves after ",
            idle_s,
            " s idle)"
        );

        let mut last = time(ptr::null_mut());
        while G_STOP.load(Ordering::Relaxed) == 0 {
            let mut pf = PollFd {
                fd: ls,
                events: POLLIN,
                revents: 0,
            };
            let r = poll(&mut pf, 1, 1000);
            if r < 0 {
                if errno() == EINTR {
                    continue;
                }
                let e = errno();
                plog!("poll: ", errstr(e));
                break;
            }
            if r == 0 {
                if idle_s > 0 && time(ptr::null_mut()) - last >= idle_s as i64 {
                    plog!(
                        "idle for ",
                        idle_s,
                        " s: leaving (the areas stay; the next setprop starts a service again)"
                    );
                    break;
                }
                // The directory went away (a "reboot" by removing it): leave.
                let mut st = new_stat();
                if sys_stat(&self.dir, &mut st) != 0 {
                    break;
                }
                continue;
            }
            let c = accept(ls, ptr::null_mut(), ptr::null_mut());
            if c < 0 {
                continue;
            }
            fcntl(c, F_SETFD, FD_CLOEXEC);
            self.handle_connection(c, 2000);
            close(c);
            last = time(ptr::null_mut());
        }
        // Nobody can connect once the name is gone; whoever did in the meantime
        // is still answered, within 2 s in all (a client that finds no socket
        // starts a new service, which waits up to 10 s for this one's lock, and
        // the runtime waits as long for this one to leave: props.rs).
        sys_unlink(&sock_path);
        fcntl(ls, F_SETFL, O_NONBLOCK);
        let d0 = monotonic();
        loop {
            let c = accept(ls, ptr::null_mut(), ptr::null_mut());
            if c < 0 {
                break;
            }
            fcntl(c, F_SETFL, 0);
            let d1 = monotonic();
            let left = 2000
                - ((d1.tv_sec - d0.tv_sec) * 1000 + (d1.tv_nsec - d0.tv_nsec) / 1_000_000) as c_int;
            if left > 0 {
                // else closed unanswered: its setprop fails
                self.handle_connection(c, left);
            }
            close(c);
        }
        close(ls);
        sys_unlink(&pidp);
        plog!("stopped");
        0
    }
}

/// propsvc: `lxrun --property-service <dir> <root> [--daemon]`
///
/// # Safety
/// `argv` must hold `argc` valid C strings, as `main` receives them.
#[no_mangle]
pub unsafe extern "C" fn lxrt_property_service_main(argc: c_int, argv: *mut *mut c_char) -> c_int {
    let svc = &mut *ptr::addr_of_mut!(G);
    svc.main(argc, argv)
}
