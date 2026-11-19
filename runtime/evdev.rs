// /dev/input/eventN for guest programs: game controllers without a kernel.
//
// The Mac side (tools/inputd, steamarm-inputd) reads the real controllers and
// publishes each player as a Unix stream socket /tmp/lxrt-input/eventN plus a
// description in /tmp/lxrt-input/meta/eventN (tools/inputd/PROTOCOL.md). The
// guest's /dev/input is that directory. Opening an event node connects to the
// socket; the descriptor then behaves like a Linux evdev node: the EVIOC*
// ioctls answer from the description and from the state seen in the stream,
// read() returns whole struct input_event records, and force feedback
// (EVIOCSFF + write of EV_FF) becomes a rumble record to the daemon.
//
// SDL (winebus in Proton, native games) finds these the way it does inside
// any container: scanning /dev/input and watching it with inotify.
//
// LXRT_INPUT_DIR=<absolute host directory> gives a guest (and what it runs,
// which inherits the environment) a /dev/input of its own instead. Android's
// display composer (Waydroid's hwcomposer.waydroid) makes its input FIFOs
// there, /dev/input/wl_{pointer,keyboard,touch,tablet}_events, for
// InputFlinger's EventHub to read; in the shared directory they sat next to
// steamarm-inputd's controller sockets, and two Android roots would have met
// in one FIFO (benchmarks/stage28-android-reliability.txt). Unset, the
// directory is /tmp/lxrt-input as before: the Steam controller path does not
// change.

use std::ffi::{c_char, c_int, c_long, c_uint, c_ulong, c_void};
use std::mem::size_of;
use std::ptr;
use std::sync::atomic::{AtomicI32, AtomicPtr, Ordering};

// Darwin (arm64) values of the libc constants the C original used.
const PATH_MAX: usize = 1024;
const D_EINVAL: c_int = 22;
const D_ENOMEM: c_int = 12;
const D_EFAULT: c_int = 14;
const D_ENODEV: c_int = 19;
const D_ENOTTY: c_int = 25;
const D_ENOSPC: c_int = 28;
const D_ECONNREFUSED: c_int = 61;
const D_EOPNOTSUPP: c_int = 102;
const AF_UNIX: c_int = 1;
const SOCK_STREAM: c_int = 1;
const SOL_SOCKET: c_int = 0xffff;
const SO_NOSIGPIPE: c_int = 0x1022;
const MSG_WAITALL: c_int = 0x40;
const POLLIN: i16 = 0x0001;
const F_SETFD: c_int = 2;
const F_GETFL: c_int = 3;
const F_SETFL: c_int = 4;
const FD_CLOEXEC: c_int = 1;
const O_NONBLOCK: c_int = 0x0004;
const S_IFMT: u16 = 0o170000;
const S_IFSOCK: u16 = 0o140000;
const S_IFCHR: u16 = 0o020000;
const CLOCK_REALTIME: c_int = 0;
const CLOCK_MONOTONIC: c_int = 6;

const EVDEV_DIR_DEFAULT: &[u8] = b"/tmp/lxrt-input";

const EV_FDS: usize = 65536;
const KEY_BYTES: usize = 96; // KEY_MAX 0x2ff
const ABS_N: usize = 64; // ABS_MAX 0x3f
const FF_BYTES: usize = 16; // FF_MAX 0x7f
const MAX_EFFECTS: usize = 16;
const EVENT_SIZE: usize = 24;

#[repr(C)]
#[derive(Clone, Copy)]
struct AbsInfo {
    value: i32,
    min: i32,
    max: i32,
    fuzz: i32,
    flat: i32,
    res: i32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Fx {
    used: c_int,
    strong: u16,
    weak: u16,
    length: u16,
}

#[repr(C)]
struct Evdev {
    num: c_int, // N of eventN
    ino: u64,   // the socket's inode: the entry is stale once it differs
    name: [u8; 128],
    phys: [u8; 64],
    uniq: [u8; 64],
    id: [u16; 4], // bustype vendor product version
    prop: [u8; 8],
    keybits: [u8; KEY_BYTES],
    absbits: [u8; 8],
    ffbits: [u8; FF_BYTES],
    effects: c_int,
    abs: [AbsInfo; ABS_N],
    keystate: [u8; KEY_BYTES],
    clock: c_int, // EVIOCSCLOCKID: 0 realtime, 1 monotonic
    fx: [Fx; MAX_EFFECTS],
    gain: u32, // FF_GAIN, 0..0xffff
    grabbed: c_int,
}

#[repr(C)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: i64,
}

// struct stat of macOS arm64 (64-bit inodes).
#[repr(C)]
pub struct DarwinStat {
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
struct SockaddrUn {
    sun_len: u8,
    sun_family: u8,
    sun_path: [u8; 104],
}

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: i16,
    revents: i16,
}

const _: [(); 24] = [(); size_of::<AbsInfo>()];
const _: [(); 12] = [(); size_of::<Fx>()];
const _: [(); 144] = [(); size_of::<DarwinStat>()];
const _: [(); 106] = [(); size_of::<SockaddrUn>()];
const _: [(); 8] = [(); size_of::<PollFd>()];

#[link(name = "System")]
unsafe extern "C" {
    fn __error() -> *mut c_int;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn strlen(s: *const c_char) -> usize;
    fn strncmp(a: *const c_char, b: *const c_char, n: usize) -> c_int;
    fn atoi(s: *const c_char) -> c_int;
    fn sscanf(s: *const c_char, format: *const c_char, ...) -> c_int;
    fn fopen(path: *const c_char, mode: *const c_char) -> *mut c_void;
    fn fgets(buf: *mut c_char, size: c_int, stream: *mut c_void) -> *mut c_char;
    fn fclose(stream: *mut c_void) -> c_int;
    fn calloc(count: usize, size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn socket(domain: c_int, kind: c_int, protocol: c_int) -> c_int;
    fn connect(fd: c_int, address: *const c_void, len: u32) -> c_int;
    fn setsockopt(
        fd: c_int,
        level: c_int,
        name: c_int,
        value: *const c_void,
        len: u32,
    ) -> c_int;
    fn recv(fd: c_int, buf: *mut c_void, len: usize, flags: c_int) -> isize;
    fn send(fd: c_int, buf: *const c_void, len: usize, flags: c_int) -> isize;
    fn poll(fds: *mut PollFd, count: c_uint, timeout: c_int) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn fstat(fd: c_int, st: *mut DarwinStat) -> c_int;
    fn clock_gettime(clock: c_int, ts: *mut Timespec) -> c_int;
}

#[inline]
fn lerr(error: c_int) -> c_long {
    -(crate::errno_map::map_errno(error) as c_long)
}

#[inline]
unsafe fn errno() -> c_int {
    *__error()
}

#[inline]
unsafe fn set_errno(value: c_int) {
    *__error() = value;
}

static mut DIR: [u8; PATH_MAX] = [0; PATH_MAX];
static DIR_READY: AtomicI32 = AtomicI32::new(0);

unsafe fn evdev_dir() -> *const u8 {
    let dir = ptr::addr_of_mut!(DIR).cast::<u8>();
    if DIR_READY.load(Ordering::Acquire) == 0 {
        let e = getenv(c"LXRT_INPUT_DIR".as_ptr()).cast::<u8>();
        let mut tmp = [0u8; PATH_MAX];
        let mut l;
        if !e.is_null() && *e == b'/' && strlen(e.cast()) < PATH_MAX - 32 {
            l = strlen(e.cast());
            ptr::copy_nonoverlapping(e, tmp.as_mut_ptr(), l);
        } else {
            l = EVDEV_DIR_DEFAULT.len();
            ptr::copy_nonoverlapping(EVDEV_DIR_DEFAULT.as_ptr(), tmp.as_mut_ptr(), l);
        }
        tmp[l] = 0;
        // The environment can change under us between the two strlen calls
        // only if another thread calls setenv; the C had the same window.
        while l > 1 && tmp[l - 1] == b'/' {
            l -= 1;
            tmp[l] = 0;
        }
        // every thread that races here writes the same bytes
        ptr::copy_nonoverlapping(tmp.as_ptr(), dir, l + 1);
        DIR_READY.store(1, Ordering::Release);
    }
    dir
}

static G_EV: [AtomicPtr<Evdev>; EV_FDS] = [const { AtomicPtr::new(ptr::null_mut()) }; EV_FDS];

#[inline]
fn set_bit(bits: &mut [u8], b: c_uint) {
    let index = (b / 8) as usize;
    if index < bits.len() {
        bits[index] |= (1u32 << (b % 8)) as u8;
    }
}

#[inline]
fn test_bit(bits: &[u8], b: c_uint) -> bool {
    let index = (b / 8) as usize;
    index < bits.len() && (bits[index] >> (b % 8)) & 1 != 0
}

// snprintf(dst, n, "%s", src): at most n - 1 bytes and a terminator.
unsafe fn copy_cstr(dst: *mut u8, n: usize, src: *const u8) {
    if n == 0 {
        return;
    }
    let mut i = 0;
    while i < n - 1 {
        let byte = *src.add(i);
        if byte == 0 {
            break;
        }
        *dst.add(i) = byte;
        i += 1;
    }
    *dst.add(i) = 0;
}

// The host path of the guest's /dev/input[/...]; NULL for other paths.
#[no_mangle]
pub unsafe extern "C" fn lxrt_evdev_translate(
    path: *const c_char,
    buf: *mut c_char,
    n: usize,
) -> *const c_char {
    if strncmp(path, c"/dev/input".as_ptr(), 10) != 0 {
        return ptr::null();
    }
    let after = *path.add(10) as u8;
    if after != 0 && after != b'/' {
        return ptr::null();
    }
    // snprintf(buf, n, "%s%s", evdev_dir(), path + 10)
    let dir = evdev_dir();
    let dir_len = strlen(dir.cast());
    let rest = path.add(10).cast::<u8>();
    let rest_len = strlen(rest.cast());
    let k = dir_len + rest_len;
    if n > 0 {
        let out = buf.cast::<u8>();
        let room = n - 1;
        let first = if dir_len < room { dir_len } else { room };
        ptr::copy_nonoverlapping(dir, out, first);
        let left = room - first;
        let second = if rest_len < left { rest_len } else { left };
        ptr::copy_nonoverlapping(rest, out.add(first), second);
        *out.add(first + second) = 0;
    }
    if k > 0 && k <= c_int::MAX as usize && k < n {
        buf
    } else {
        ptr::null()
    }
}

// "…/lxrt-input/eventN" -> N, else -1.
unsafe fn node_number(host: *const c_char) -> c_int {
    let d = evdev_dir();
    let l = strlen(d.cast());
    if strncmp(host, d.cast(), l) != 0 || strncmp(host.add(l), c"/event".as_ptr(), 6) != 0 {
        return -1;
    }
    let mut p = host.add(l + 6).cast::<u8>();
    if *p == 0 {
        return -1;
    }
    let mut n: c_int = 0;
    while *p != 0 {
        if *p < b'0' || *p > b'9' || n > 1000 {
            return -1;
        }
        n = n * 10 + (*p - b'0') as c_int;
        p = p.add(1);
    }
    n
}

unsafe fn keyword(line: *const u8, word: &[u8]) -> bool {
    for (i, byte) in word.iter().enumerate() {
        if *line.add(i) != *byte {
            return false;
        }
    }
    *line.add(word.len()) == 0
}

unsafe fn load_meta(e: *mut Evdev) -> c_int {
    // snprintf(path, sizeof path, "%s/meta/event%d", evdev_dir(), e->num)
    let mut path = [0u8; PATH_MAX];
    {
        let dir = evdev_dir();
        let mut at = strlen(dir.cast());
        if at > PATH_MAX - 32 {
            at = PATH_MAX - 32;
        }
        ptr::copy_nonoverlapping(dir, path.as_mut_ptr(), at);
        for byte in b"/meta/event" {
            path[at] = *byte;
            at += 1;
        }
        let mut digits = [0u8; 12];
        let mut count = 0;
        let mut value = (*e).num as i64;
        if value < 0 {
            path[at] = b'-';
            at += 1;
            value = -value;
        }
        loop {
            digits[count] = b'0' + (value % 10) as u8;
            count += 1;
            value /= 10;
            if value == 0 {
                break;
            }
        }
        while count > 0 {
            count -= 1;
            path[at] = digits[count];
            at += 1;
        }
        path[at] = 0;
    }
    let f = fopen(path.as_ptr().cast(), c"r".as_ptr());
    if f.is_null() {
        return -1;
    }
    let mut line = [0u8; 1024];
    while !fgets(line.as_mut_ptr().cast(), line.len() as c_int, f).is_null() {
        let base = line.as_mut_ptr();
        // line[strcspn(line, "\n")] = 0
        let mut i = 0;
        while *base.add(i) != 0 && *base.add(i) != b'\n' {
            i += 1;
        }
        *base.add(i) = 0;
        // rest = strchr(line, ' ')
        let mut rest: *mut u8 = ptr::null_mut();
        let mut j = 0;
        while *base.add(j) != 0 {
            if *base.add(j) == b' ' {
                rest = base.add(j);
                break;
            }
            j += 1;
        }
        let arg: *mut u8 = if rest.is_null() {
            base.add(i) // the terminator: an empty string
        } else {
            *rest = 0;
            rest.add(1)
        };
        if keyword(base, b"name") {
            copy_cstr((*e).name.as_mut_ptr(), 128, arg);
        } else if keyword(base, b"phys") {
            copy_cstr((*e).phys.as_mut_ptr(), 64, arg);
        } else if keyword(base, b"uniq") {
            copy_cstr((*e).uniq.as_mut_ptr(), 64, arg);
        } else if keyword(base, b"id") {
            let (mut a, mut b, mut c, mut d): (c_uint, c_uint, c_uint, c_uint) = (0, 0, 0, 0);
            if sscanf(
                arg.cast(),
                c"%x %x %x %x".as_ptr(),
                &mut a as *mut c_uint,
                &mut b as *mut c_uint,
                &mut c as *mut c_uint,
                &mut d as *mut c_uint,
            ) == 4
            {
                (*e).id[0] = a as u16;
                (*e).id[1] = b as u16;
                (*e).id[2] = c as u16;
                (*e).id[3] = d as u16;
            }
        } else if keyword(base, b"effects") {
            (*e).effects = atoi(arg.cast());
        } else if keyword(base, b"abs") {
            let (mut c, mut mn, mut mx, mut fz, mut fl, mut rs): (
                c_int,
                c_int,
                c_int,
                c_int,
                c_int,
                c_int,
            ) = (0, 0, 0, 0, 0, 0);
            if sscanf(
                arg.cast(),
                c"%d %d %d %d %d %d".as_ptr(),
                &mut c as *mut c_int,
                &mut mn as *mut c_int,
                &mut mx as *mut c_int,
                &mut fz as *mut c_int,
                &mut fl as *mut c_int,
                &mut rs as *mut c_int,
            ) == 6
                && c >= 0
                && (c as usize) < ABS_N
            {
                set_bit(&mut (*e).absbits, c as c_uint);
                (*e).abs[c as usize] = AbsInfo {
                    value: if mn <= 0 && mx >= 0 { 0 } else { mn },
                    min: mn,
                    max: mx,
                    fuzz: fz,
                    flat: fl,
                    res: rs,
                };
            }
        } else if keyword(base, b"key") || keyword(base, b"ff") || keyword(base, b"prop") {
            let bits: &mut [u8] = match *base {
                b'k' => &mut (*e).keybits,
                b'f' => &mut (*e).ffbits,
                _ => &mut (*e).prop,
            };
            // strtok(arg, " ") without its hidden static state: tokens are
            // the runs between spaces, each terminated in place for atoi.
            let mut t = arg;
            loop {
                while *t == b' ' {
                    t = t.add(1);
                }
                if *t == 0 {
                    break;
                }
                let token = t;
                while *t != 0 && *t != b' ' {
                    t = t.add(1);
                }
                let last = *t == 0;
                *t = 0;
                set_bit(bits, atoi(token.cast()) as c_uint);
                if last {
                    break;
                }
                t = t.add(1);
            }
        }
    }
    fclose(f);
    if (*e).effects <= 0 || (*e).effects > MAX_EFFECTS as c_int {
        (*e).effects = MAX_EFFECTS as c_int;
    }
    0
}

#[inline]
unsafe fn rec_u16(rec: *const u8, offset: usize) -> u16 {
    ptr::read_unaligned(rec.add(offset).cast::<u16>())
}

#[inline]
unsafe fn rec_i16(rec: *const u8, offset: usize) -> i16 {
    ptr::read_unaligned(rec.add(offset).cast::<i16>())
}

#[inline]
unsafe fn rec_i32(rec: *const u8, offset: usize) -> i32 {
    ptr::read_unaligned(rec.add(offset).cast::<i32>())
}

unsafe fn apply(e: *mut Evdev, rec: *const u8) {
    let kind = rec_u16(rec, 16);
    let code = rec_u16(rec, 18);
    let value = rec_i32(rec, 20);
    if kind == 1 && ((code / 8) as usize) < KEY_BYTES {
        if value != 0 {
            (*e).keystate[(code / 8) as usize] |= (1u32 << (code % 8)) as u8;
        } else {
            (*e).keystate[(code / 8) as usize] &= !(1u32 << (code % 8)) as u8;
        }
    } else if kind == 3 && (code as usize) < ABS_N {
        (*e).abs[code as usize].value = value;
    }
}

// The daemon's snapshot (state, then SYN_REPORT) right after connecting: it
// answers EVIOCGABS/EVIOCGKEY, and like a Linux open the guest gets no events.
unsafe fn take_snapshot(fd: c_int, e: *mut Evdev) {
    let mut rec = [0u8; EVENT_SIZE];
    let mut have: usize = 0;
    let mut spins = 0;
    while spins < 400 {
        let mut p = PollFd {
            fd,
            events: POLLIN,
            revents: 0,
        };
        if poll(&mut p, 1, 50) <= 0 {
            return;
        }
        let r = recv(
            fd,
            rec.as_mut_ptr().add(have).cast(),
            EVENT_SIZE - have,
            0,
        );
        if r <= 0 {
            return;
        }
        have += r as usize;
        if have < EVENT_SIZE {
            continue;
        }
        have = 0;
        spins += 1;
        let kind = rec_u16(rec.as_ptr(), 16);
        let code = rec_u16(rec.as_ptr(), 18);
        if kind == 0 && code == 0 {
            return;
        }
        apply(e, rec.as_ptr());
    }
}

// openat of a /dev/input node, after Darwin refused to open the socket.
// Returns a descriptor, or -1 with errno.
#[no_mangle]
pub unsafe extern "C" fn lxrt_evdev_open(host: *const c_char, lflags: c_int) -> c_int {
    let num = node_number(host);
    if num < 0 {
        set_errno(D_EOPNOTSUPP);
        return -1;
    }
    let e = calloc(1, size_of::<Evdev>()).cast::<Evdev>();
    if e.is_null() {
        set_errno(D_ENOMEM);
        return -1;
    }
    (*e).num = num;
    (*e).gain = 0xffff;
    if load_meta(e) != 0 {
        free(e.cast());
        set_errno(D_ENODEV);
        return -1;
    }
    let fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if fd < 0 {
        free(e.cast());
        return -1;
    }
    let mut a = SockaddrUn {
        sun_len: 0,
        sun_family: AF_UNIX as u8,
        sun_path: [0; 104],
    };
    copy_cstr(a.sun_path.as_mut_ptr(), 104, host.cast());
    if connect(
        fd,
        (&a as *const SockaddrUn).cast(),
        size_of::<SockaddrUn>() as u32,
    ) != 0
        || fd as usize >= EV_FDS
    {
        let err = if errno() == D_ECONNREFUSED {
            D_ENODEV
        } else {
            errno()
        };
        close(fd);
        free(e.cast());
        set_errno(err);
        return -1;
    }
    let one: c_int = 1;
    setsockopt(
        fd,
        SOL_SOCKET,
        SO_NOSIGPIPE,
        (&one as *const c_int).cast(),
        size_of::<c_int>() as u32,
    );
    let mut sst = std::mem::MaybeUninit::<DarwinStat>::uninit();
    (*e).ino = if fstat(fd, sst.as_mut_ptr()) == 0 {
        (*sst.as_ptr()).st_ino
    } else {
        0
    };
    take_snapshot(fd, e);
    if lflags & 0o2000000 != 0 {
        // O_CLOEXEC
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
    if lflags & 0o4000 != 0 {
        // O_NONBLOCK
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    let old = G_EV[fd as usize].swap(e, Ordering::SeqCst);
    free(old.cast());
    fd
}

// As in socket.c's netlink table: a number closed by close_range, dup2 or
// exec never reaches lxrt_evdev_close, and must not make an unrelated
// descriptor that later gets it behave like a controller.
unsafe fn get(fd: c_int) -> *mut Evdev {
    let e = if fd >= 0 && (fd as usize) < EV_FDS {
        G_EV[fd as usize].load(Ordering::SeqCst)
    } else {
        ptr::null_mut()
    };
    if e.is_null() {
        return ptr::null_mut();
    }
    let mut st = std::mem::MaybeUninit::<DarwinStat>::uninit();
    if fstat(fd, st.as_mut_ptr()) == 0 && (*st.as_ptr()).st_ino == (*e).ino {
        return e;
    }
    if G_EV[fd as usize]
        .compare_exchange(e, ptr::null_mut(), Ordering::SeqCst, Ordering::SeqCst)
        .is_ok()
    {
        free(e.cast());
    }
    ptr::null_mut()
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_evdev_is(fd: c_int) -> c_int {
    !get(fd).is_null() as c_int
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_evdev_close(fd: c_int) {
    if fd < 0 || fd as usize >= EV_FDS {
        return;
    }
    free(
        G_EV[fd as usize]
            .swap(ptr::null_mut(), Ordering::SeqCst)
            .cast(),
    );
}

// stat of a node (or of an open one): a character device, input major 13,
// minor 64+N, as on Linux. SDL tells devices apart by st_rdev.
#[no_mangle]
pub unsafe extern "C" fn lxrt_evdev_fix_stat(host: *const c_char, fd: c_int, st: *mut DarwinStat) {
    let mut num: c_int = -1;
    let e = get(fd);
    if !e.is_null() {
        num = (*e).num;
    } else if !host.is_null() && (*st).st_mode & S_IFMT == S_IFSOCK {
        num = node_number(host);
    }
    if num < 0 {
        return;
    }
    (*st).st_mode = S_IFCHR | 0o660;
    (*st).st_rdev = (13 << 24) | (64 + num); // makedev(13, 64 + num)
    (*st).st_size = 0;
}

unsafe fn stamp(e: *mut Evdev, rec: *mut u8) {
    let mut ts = Timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    clock_gettime(
        if (*e).clock == 1 {
            CLOCK_MONOTONIC
        } else {
            CLOCK_REALTIME
        },
        &mut ts,
    );
    let sec: i64 = ts.tv_sec;
    let usec: i64 = ts.tv_nsec / 1000;
    ptr::write_unaligned(rec.cast::<i64>(), sec);
    ptr::write_unaligned(rec.add(8).cast::<i64>(), usec);
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_evdev_read(fd: c_int, buf: *mut c_void, n: usize) -> c_long {
    let e = get(fd);
    if n < EVENT_SIZE {
        return lerr(D_EINVAL);
    }
    let n = n - n % EVENT_SIZE;
    let mut r = recv(fd, buf, n, 0);
    if r < 0 {
        return lerr(errno());
    }
    if r == 0 {
        return lerr(D_ENODEV); // the device went away
    }
    // A record is never split for the guest: wait for the rest of it.
    while r % EVENT_SIZE as isize != 0 {
        let k = recv(
            fd,
            buf.cast::<u8>().offset(r).cast(),
            EVENT_SIZE - (r % EVENT_SIZE as isize) as usize,
            MSG_WAITALL,
        );
        if k <= 0 {
            return lerr(D_ENODEV);
        }
        r += k;
    }
    let mut i: isize = 0;
    while i < r {
        apply(e, buf.cast::<u8>().offset(i));
        if (*e).clock == 1 {
            stamp(e, buf.cast::<u8>().offset(i));
        }
        i += EVENT_SIZE as isize;
    }
    r as c_long
}

unsafe fn send_rumble(fd: c_int, strong: u16, weak: u16, ms: u32) {
    let mut rec = [0u8; EVENT_SIZE];
    let magic: u32 = 0x4c42_4d52;
    ptr::write_unaligned(rec.as_mut_ptr().cast::<u32>(), magic);
    ptr::write_unaligned(rec.as_mut_ptr().add(4).cast::<u16>(), strong);
    ptr::write_unaligned(rec.as_mut_ptr().add(6).cast::<u16>(), weak);
    ptr::write_unaligned(rec.as_mut_ptr().add(8).cast::<u32>(), ms);
    let _ = send(fd, rec.as_ptr().cast(), EVENT_SIZE, 0);
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_evdev_write(fd: c_int, buf: *const c_void, n: usize) -> c_long {
    let e = get(fd);
    if n < EVENT_SIZE {
        return lerr(D_EINVAL);
    }
    let n = n - n % EVENT_SIZE;
    let mut i = 0;
    while i < n {
        let rec = buf.cast::<u8>().add(i);
        i += EVENT_SIZE;
        let kind = rec_u16(rec, 16);
        let code = rec_u16(rec, 18) as usize;
        let value = rec_i32(rec, 20);
        if kind != 0x15 {
            // EV_FF only; LEDs etc. are dropped
            continue;
        }
        if code == 96 {
            // FF_GAIN
            (*e).gain = value as u32 & 0xffff;
            continue;
        }
        if code >= MAX_EFFECTS || (*e).fx[code].used == 0 {
            continue;
        }
        if value > 0 {
            let fx = (*e).fx[code];
            let s = ((fx.strong as u32).wrapping_mul((*e).gain) / 0xffff) as u16;
            let w = ((fx.weak as u32).wrapping_mul((*e).gain) / 0xffff) as u16;
            send_rumble(fd, s, w, fx.length as u32);
        } else {
            send_rumble(fd, 0, 0, 0);
        }
    }
    n as c_long
}

unsafe fn copy_out(p: *mut c_void, len: usize, src: *const u8, have: usize) -> c_long {
    if p.is_null() {
        return lerr(D_EFAULT);
    }
    let k = if len < have { len } else { have };
    ptr::copy_nonoverlapping(src, p.cast::<u8>(), k);
    k as c_long
}

unsafe fn copy_str(p: *mut c_void, len: usize, s: *const u8) -> c_long {
    if len == 0 {
        return 0;
    }
    let have = strlen(s.cast()) + 1;
    let k = copy_out(p, len, s, have);
    if k > 0 && k as usize == len {
        *p.cast::<u8>().add(len - 1) = 0;
    }
    k
}

#[inline]
fn fault_or_zero(copied: c_long) -> c_long {
    if copied < 0 {
        lerr(D_EFAULT)
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_evdev_ioctl(fd: c_int, req: c_ulong, arg: u64) -> c_long {
    let e = get(fd);
    let p = arg as *mut c_void;
    let dir = (req >> 30) as c_uint & 3;
    let size = ((req >> 16) as c_uint & 0x3fff) as usize;
    let kind = (req >> 8) as c_uint & 0xff;
    let nr = req as c_uint & 0xff;
    if kind != b'E' as c_uint {
        return lerr(D_ENOTTY);
    }
    if dir == 2 {
        // _IOC_READ
        match nr {
            0x01 => {
                let v: c_int = 0x010001;
                return fault_or_zero(copy_out(p, size, (&v as *const c_int).cast(), 4));
            }
            0x02 => return fault_or_zero(copy_out(p, size, (*e).id.as_ptr().cast(), 8)),
            0x03 => {
                let rep: [u32; 2] = [250, 33];
                return fault_or_zero(copy_out(p, size, rep.as_ptr().cast(), 8));
            }
            0x06 => return copy_str(p, size, (*e).name.as_ptr()),
            0x07 => return copy_str(p, size, (*e).phys.as_ptr()),
            0x08 => return copy_str(p, size, (*e).uniq.as_ptr()),
            0x09 => return copy_out(p, size, (*e).prop.as_ptr(), 8),
            0x18 => return copy_out(p, size, (*e).keystate.as_ptr(), KEY_BYTES),
            0x19 | 0x1a | 0x1b => {
                // LED, SND, SW state: none
                let z = [0u8; 16];
                return copy_out(p, size, z.as_ptr(), if size < 16 { size } else { 16 });
            }
            0x84 => {
                return fault_or_zero(copy_out(
                    p,
                    size,
                    ptr::addr_of!((*e).effects).cast(),
                    4,
                ))
            }
            _ => {}
        }
        if (0x20..0x40).contains(&nr) {
            // EVIOCGBIT(ev, len)
            let ev = nr - 0x20;
            let mut bits = [0u8; KEY_BYTES];
            match ev {
                0 => {
                    // the event types
                    set_bit(&mut bits, 0);
                    let mut any: c_int = 0;
                    for i in 0..KEY_BYTES {
                        any |= (*e).keybits[i] as c_int;
                    }
                    if any != 0 {
                        set_bit(&mut bits, 1);
                    }
                    any = 0;
                    for i in 0..8 {
                        any |= (*e).absbits[i] as c_int;
                    }
                    if any != 0 {
                        set_bit(&mut bits, 3);
                    }
                    any = 0;
                    for i in 0..FF_BYTES {
                        any |= (*e).ffbits[i] as c_int;
                    }
                    if any != 0 {
                        set_bit(&mut bits, 0x15);
                    }
                    return copy_out(p, size, bits.as_ptr(), 4);
                }
                1 => return copy_out(p, size, (*e).keybits.as_ptr(), KEY_BYTES),
                3 => return copy_out(p, size, (*e).absbits.as_ptr(), 8),
                0x15 => return copy_out(p, size, (*e).ffbits.as_ptr(), FF_BYTES),
                // none of that type
                _ => return copy_out(p, size, bits.as_ptr(), if size < 8 { size } else { 8 }),
            }
        }
        if nr >= 0x40 && nr < 0x40 + ABS_N as c_uint {
            // EVIOCGABS(abs)
            let c = nr - 0x40;
            if !test_bit(&(*e).absbits, c) {
                return lerr(D_EINVAL);
            }
            return fault_or_zero(copy_out(
                p,
                size,
                ptr::addr_of!((*e).abs[c as usize]).cast(),
                size_of::<AbsInfo>(),
            ));
        }
        return lerr(D_EINVAL);
    }
    if dir == 1 {
        // _IOC_WRITE
        if nr >= 0xc0 && nr < 0xc0 + ABS_N as c_uint {
            // EVIOCSABS: accepted, ranges kept
            let c = nr - 0xc0;
            if p.is_null() {
                return lerr(D_EFAULT);
            }
            if test_bit(&(*e).absbits, c) {
                let a = ptr::read_unaligned(p.cast::<AbsInfo>());
                (*e).abs[c as usize].fuzz = a.fuzz;
                (*e).abs[c as usize].flat = a.flat;
            }
            return 0;
        }
        match nr {
            0x80 => {
                // EVIOCSFF(struct ff_effect)
                if p.is_null() {
                    return lerr(D_EFAULT);
                }
                let fx = p.cast::<u8>();
                let ftype = rec_u16(fx, 0);
                let mut id = rec_i16(fx, 2);
                // The union follows a pointer-aligned header: offset 16 in the
                // 64-bit struct (48 bytes), 12 in the i386 one (44 bytes).
                let u = fx.add(if size == 44 { 12 } else { 16 }).cast_const();
                let length = rec_u16(fx, 10); // replay.length
                let strong: u16;
                let weak: u16;
                if ftype == 80 {
                    // FF_RUMBLE
                    strong = rec_u16(u, 0);
                    weak = rec_u16(u, 2);
                } else if ftype == 81 {
                    // FF_PERIODIC: magnitude
                    let m = rec_i16(u, 4) as i32;
                    strong = ((if m < 0 { -m } else { m }) as u16).wrapping_mul(2);
                    weak = strong;
                } else if ftype == 82 {
                    // FF_CONSTANT: level
                    let l = rec_i16(u, 0) as i32;
                    strong = ((if l < 0 { -l } else { l }) as u16).wrapping_mul(2);
                    weak = strong;
                } else {
                    return lerr(D_EINVAL);
                }
                if id < 0 {
                    id = 0;
                    while (id as c_int) < (*e).effects && (*e).fx[id as usize].used != 0 {
                        id += 1;
                    }
                    if id as c_int >= (*e).effects {
                        return lerr(D_ENOSPC);
                    }
                    ptr::write_unaligned(fx.add(2).cast::<i16>(), id);
                } else if id as c_int >= (*e).effects {
                    return lerr(D_EINVAL);
                }
                let slot = ptr::addr_of_mut!((*e).fx[id as usize]);
                (*slot).used = 1;
                (*slot).strong = strong;
                (*slot).weak = weak;
                (*slot).length = length;
                return 0;
            }
            0x81 => {
                // EVIOCRMFF(int id)
                let id = arg as c_long;
                if id < 0 || id >= MAX_EFFECTS as c_long {
                    return lerr(D_EINVAL);
                }
                if (*e).fx[id as usize].used != 0 {
                    send_rumble(fd, 0, 0, 0);
                }
                (*e).fx[id as usize].used = 0;
                return 0;
            }
            0x90 => {
                // EVIOCGRAB: not exclusive here
                (*e).grabbed = (arg != 0) as c_int;
                return 0;
            }
            0x91 => return 0, // EVIOCREVOKE
            0x93 => return 0, // EVIOCSMASK
            0xa0 => {
                // EVIOCSCLOCKID
                if p.is_null() {
                    return lerr(D_EFAULT);
                }
                let id = ptr::read_unaligned(p.cast::<c_int>());
                if id != 0 && id != 1 && id != 7 {
                    // REALTIME, MONOTONIC, BOOTTIME
                    return lerr(D_EINVAL);
                }
                (*e).clock = if id == 0 { 0 } else { 1 };
                return 0;
            }
            _ => {}
        }
    }
    lerr(D_EINVAL)
}
