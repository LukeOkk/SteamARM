//! Linux TTY ioctl translation for the macOS host.
//!
//! Linux request values below are the aarch64 asm-generic ABI. Darwin request,
//! termios, and fcntl values match the macOS SDK ABI used by this runtime.

use std::ffi::{c_char, c_int, c_long, c_ulong, c_void, CStr};
use std::ptr;
use std::sync::{Mutex, MutexGuard};

const L_TCGETS: u32 = 0x5401;
const L_TCSETS: u32 = 0x5402;
const L_TCSETSW: u32 = 0x5403;
const L_TCSETSF: u32 = 0x5404;
const L_TCFLSH: u32 = 0x540b;
const L_TIOCEXCL: u32 = 0x540c;
const L_TIOCNXCL: u32 = 0x540d;
const L_TIOCSCTTY: u32 = 0x540e;
const L_TIOCGPGRP: u32 = 0x540f;
const L_TIOCSPGRP: u32 = 0x5410;
const L_TIOCOUTQ: u32 = 0x5411;
const L_TIOCGWINSZ: u32 = 0x5413;
const L_TIOCSWINSZ: u32 = 0x5414;
const L_FIONREAD: u32 = 0x541b;
const L_FIONBIO: u32 = 0x5421;
const L_TIOCNOTTY: u32 = 0x5422;
const L_TIOCGSID: u32 = 0x5429;
const L_FIONCLEX: u32 = 0x5450;
const L_FIOCLEX: u32 = 0x5451;
const L_TCGETS2: u32 = 0x802c542a;
const L_TCSETS2: u32 = 0x402c542b;
const L_TCSETSW2: u32 = 0x402c542c;
const L_TCSETSF2: u32 = 0x402c542d;
const L_TIOCGPTN: u32 = 0x80045430;
const L_TIOCSPTLCK: u32 = 0x40045431;
const L_TIOCGPTPEER: u32 = 0x5441;

const L_TERMIOS_SIZE: usize = 36;
const L_TERMIOS2_SIZE: usize = 44;
const ENOTTY: c_int = 25;
const EFAULT: c_int = 14;

// Darwin termios is four 64-bit flags, twenty control bytes, then two
// 64-bit speeds. Its SDK layout is 72 bytes with speed fields at offsets 56/64.
#[repr(C)]
struct DarwinTermios {
    c_iflag: c_ulong,
    c_oflag: c_ulong,
    c_cflag: c_ulong,
    c_lflag: c_ulong,
    c_cc: [u8; 20],
    c_ispeed: c_ulong,
    c_ospeed: c_ulong,
}

#[repr(C, packed)]
#[derive(Clone, Copy)]
struct LinuxTermios {
    c_iflag: u32,
    c_oflag: u32,
    c_cflag: u32,
    c_lflag: u32,
    c_line: u8,
    c_cc: [u8; 19],
    c_ispeed: u32,
    c_ospeed: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct WinSize {
    ws_row: u16,
    ws_col: u16,
    ws_xpixel: u16,
    ws_ypixel: u16,
}

#[derive(Clone, Copy)]
struct HeldPty {
    master: c_int,
    number: c_int,
    slave: c_int,
}

const EMPTY_HELD_PTY: HeldPty = HeldPty {
    master: 0,
    number: 0,
    slave: 0,
};
static HELD_PTYS: Mutex<[HeldPty; 32]> = Mutex::new([EMPTY_HELD_PTY; 32]);

const DARWIN_IOCTL_TIOCEXCL: c_ulong = 536_900_621;
const DARWIN_IOCTL_TIOCNXCL: c_ulong = 536_900_622;
const DARWIN_IOCTL_TIOCOUTQ: c_ulong = 1_074_033_779;
const DARWIN_IOCTL_TIOCGWINSZ: c_ulong = 1_074_295_912;
const DARWIN_IOCTL_TIOCSWINSZ: c_ulong = 2_148_037_735;
const DARWIN_IOCTL_TIOCNOTTY: c_ulong = 536_900_721;
const DARWIN_IOCTL_TIOCSCTTY: c_ulong = 536_900_705;
const DARWIN_IOCTL_TIOCPTYGRANT: c_ulong = 536_900_692;
const DARWIN_IOCTL_TIOCPTYGNAME: c_ulong = 1_082_160_211;
const DARWIN_IOCTL_TIOCPTYUNLK: c_ulong = 536_900_690;
const DARWIN_IOCTL_FIONREAD: c_ulong = 1_074_030_207;

const O_RDWR: c_int = 2;
const O_NOCTTY: c_int = 0x0002_0000;
const O_CLOEXEC: c_int = 0x0100_0000;
const O_NONBLOCK: c_int = 4;
const F_GETFL: c_int = 3;
const F_SETFL: c_int = 4;
const F_SETFD: c_int = 2;
const FD_CLOEXEC: c_int = 1;
const TCSANOW: c_int = 0;
const TCSADRAIN: c_int = 1;
const TCSAFLUSH: c_int = 2;
const TCIFLUSH: c_int = 1;
const TCOFLUSH: c_int = 2;
const TCIOFLUSH: c_int = 3;

#[link(name = "System")]
unsafe extern "C" {
    fn __error() -> *mut c_int;
    fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn tcgetattr(fd: c_int, termios: *mut DarwinTermios) -> c_int;
    fn tcsetattr(fd: c_int, action: c_int, termios: *const DarwinTermios) -> c_int;
    fn tcgetpgrp(fd: c_int) -> c_int;
    fn tcsetpgrp(fd: c_int, group: c_int) -> c_int;
    fn tcgetsid(fd: c_int) -> c_int;
    fn tcflush(fd: c_int, queue: c_int) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;

    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_evdev_is(fd: c_int) -> bool;
    fn lxrt_evdev_ioctl(fd: c_int, request: c_ulong, arg: u64) -> c_long;
    fn lxrt_binder_is(fd: c_int) -> bool;
    fn lxrt_binder_ioctl(fd: c_int, request: c_ulong, arg: u64) -> c_long;
    fn lxrt_binder_note_stray_ioctl(fd: c_int, request: c_ulong);
    fn lxrt_ntsync_request(request: c_ulong) -> bool;
    fn lxrt_ntsync_ioctl(fd: c_int, request: c_ulong, arg: u64, result: *mut c_long) -> bool;
    fn lxrt_inotify_is(fd: c_int) -> bool;
    fn lxrt_inotify_pending(fd: c_int) -> c_long;
    fn lxrt_netif_ioctl(fd: c_int, request: c_ulong, arg: *mut c_void, result: *mut c_long)
        -> bool;
    fn lxrt_fd_private(fd: c_int) -> c_int;
    fn lxrt_fd_hide(fd: c_int, hide: bool);
    fn lxrt_open_flags_to_darwin(flags: c_int) -> c_int;
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
}

#[inline]
fn errno() -> c_int {
    unsafe { *__error() }
}

#[inline]
fn linux_error(error: c_int) -> c_long {
    unsafe { -(lxrt_errno_to_linux(error) as c_long) }
}

fn held_ptys() -> MutexGuard<'static, [HeldPty; 32]> {
    HELD_PTYS
        .lock()
        .unwrap_or_else(|poisoned| poisoned.into_inner())
}

fn pty_number_from_name(name: &CStr) -> Option<c_int> {
    let bytes = name.to_bytes();
    let suffix = bytes.strip_prefix(b"/dev/ttys")?;
    let digits = suffix.iter().take_while(|byte| byte.is_ascii_digit());
    let mut value: c_int = 0;
    let mut found = false;
    for digit in digits {
        found = true;
        value = value
            .checked_mul(10)?
            .checked_add((digit - b'0') as c_int)?;
    }
    found.then_some(value)
}

unsafe fn pty_name(fd: c_int) -> Option<[c_char; 128]> {
    let mut name = [0 as c_char; 128];
    if ioctl(fd, DARWIN_IOCTL_TIOCPTYGNAME, name.as_mut_ptr()) != 0 {
        return None;
    }
    Some(name)
}

unsafe fn pty_number(fd: c_int) -> Option<c_int> {
    let name = pty_name(fd)?;
    pty_number_from_name(CStr::from_ptr(name.as_ptr()))
}

// A Darwin pty master does not retain terminal state until its slave opens.
// Keep a private slave open so the guest can configure the master first.
unsafe fn pty_hold_slave(master: c_int) -> bool {
    let Some(name) = pty_name(master) else {
        return false;
    };
    let Some(number) = pty_number_from_name(CStr::from_ptr(name.as_ptr())) else {
        return false;
    };

    let mut held = held_ptys();
    if held
        .iter()
        .any(|entry| entry.slave > 0 && entry.number == number)
    {
        return true;
    }
    let Some(slot) = held.iter_mut().find(|entry| entry.slave <= 0) else {
        return false;
    };
    let slave = open(name.as_ptr(), O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if slave < 0 {
        return false;
    }
    slot.master = master;
    slot.number = number;
    slot.slave = lxrt_fd_private(slave);
    true
}

unsafe fn held_release(slot: &mut HeldPty) {
    let slave = slot.slave;
    slot.slave = 0;
    lxrt_fd_hide(slave, false);
    close(slave);
}

unsafe fn pty_retry(fd: c_int, mut operation: impl FnMut() -> c_int) -> c_int {
    if operation() == 0 {
        return 0;
    }
    if errno() == ENOTTY && pty_hold_slave(fd) {
        operation()
    } else {
        -1
    }
}

#[derive(Clone, Copy)]
struct FlagBit {
    linux: u32,
    darwin: c_ulong,
}

const IFLAG_BITS: &[FlagBit] = &[
    FlagBit {
        linux: 0x0001,
        darwin: 1,
    },
    FlagBit {
        linux: 0x0002,
        darwin: 2,
    },
    FlagBit {
        linux: 0x0004,
        darwin: 4,
    },
    FlagBit {
        linux: 0x0008,
        darwin: 8,
    },
    FlagBit {
        linux: 0x0010,
        darwin: 16,
    },
    FlagBit {
        linux: 0x0020,
        darwin: 32,
    },
    FlagBit {
        linux: 0x0040,
        darwin: 64,
    },
    FlagBit {
        linux: 0x0080,
        darwin: 128,
    },
    FlagBit {
        linux: 0x0100,
        darwin: 256,
    },
    FlagBit {
        linux: 0x0400,
        darwin: 512,
    },
    FlagBit {
        linux: 0x0800,
        darwin: 2048,
    },
    FlagBit {
        linux: 0x1000,
        darwin: 1024,
    },
    FlagBit {
        linux: 0x2000,
        darwin: 8192,
    },
    FlagBit {
        linux: 0x4000,
        darwin: 16384,
    },
];
const OFLAG_BITS: &[FlagBit] = &[
    FlagBit {
        linux: 0x0001,
        darwin: 1,
    },
    FlagBit {
        linux: 0x0004,
        darwin: 2,
    },
    FlagBit {
        linux: 0x0008,
        darwin: 16,
    },
    FlagBit {
        linux: 0x0010,
        darwin: 32,
    },
    FlagBit {
        linux: 0x0020,
        darwin: 64,
    },
];
const CFLAG_BITS: &[FlagBit] = &[
    FlagBit {
        linux: 0x0080,
        darwin: 2048,
    },
    FlagBit {
        linux: 0x0100,
        darwin: 4096,
    },
    FlagBit {
        linux: 0x0200,
        darwin: 8192,
    },
    FlagBit {
        linux: 0x0400,
        darwin: 16384,
    },
    FlagBit {
        linux: 0x0800,
        darwin: 32768,
    },
    FlagBit {
        linux: 0x0040,
        darwin: 1024,
    },
];
const LFLAG_BITS: &[FlagBit] = &[
    FlagBit {
        linux: 0x0001,
        darwin: 128,
    },
    FlagBit {
        linux: 0x0002,
        darwin: 256,
    },
    FlagBit {
        linux: 0x0008,
        darwin: 8,
    },
    FlagBit {
        linux: 0x0010,
        darwin: 2,
    },
    FlagBit {
        linux: 0x0020,
        darwin: 4,
    },
    FlagBit {
        linux: 0x0040,
        darwin: 16,
    },
    FlagBit {
        linux: 0x0080,
        darwin: 2_147_483_648,
    },
    FlagBit {
        linux: 0x0100,
        darwin: 4_194_304,
    },
    FlagBit {
        linux: 0x0200,
        darwin: 64,
    },
    FlagBit {
        linux: 0x0400,
        darwin: 32,
    },
    FlagBit {
        linux: 0x0800,
        darwin: 1,
    },
    FlagBit {
        linux: 0x4000,
        darwin: 536_870_912,
    },
    FlagBit {
        linux: 0x8000,
        darwin: 1024,
    },
];
const CC_MAP: [i8; 19] = [
    8, 9, 3, 5, 0, 17, 16, -1, 12, 13, 10, 1, 6, 15, 4, 14, 2, -1, -1,
];

fn to_darwin(value: u32, table: &[FlagBit]) -> c_ulong {
    table.iter().fold(0, |flags, bit| {
        if value & bit.linux != 0 {
            flags | bit.darwin
        } else {
            flags
        }
    })
}

fn to_linux(value: c_ulong, table: &[FlagBit]) -> u32 {
    table.iter().fold(0, |flags, bit| {
        if value & bit.darwin != 0 {
            flags | bit.linux
        } else {
            flags
        }
    })
}

fn speed_to_linux(speed: c_ulong) -> u32 {
    const SPEEDS: &[(c_ulong, u32)] = &[
        (0, 1),
        (50, 1),
        (75, 2),
        (110, 3),
        (134, 4),
        (150, 5),
        (200, 6),
        (300, 7),
        (600, 8),
        (1200, 9),
        (1800, 10),
        (2400, 11),
        (4800, 12),
        (9600, 13),
        (19200, 14),
        (38400, 15),
        (57600, 0x1001),
        (115200, 0x1002),
        (230400, 0x1003),
    ];
    SPEEDS
        .iter()
        .find(|entry| entry.0 == speed)
        .map_or(15, |entry| entry.1)
}

fn speed_to_darwin(cbaud: u32) -> c_ulong {
    const SPEEDS: &[(u32, c_ulong)] = &[
        (0, 0),
        (1, 50),
        (2, 75),
        (3, 110),
        (4, 134),
        (5, 150),
        (6, 200),
        (7, 300),
        (8, 600),
        (9, 1200),
        (10, 1800),
        (11, 2400),
        (12, 4800),
        (13, 9600),
        (14, 19200),
        (15, 38400),
        (0x1001, 57600),
        (0x1002, 115200),
        (0x1003, 230400),
    ];
    SPEEDS
        .iter()
        .find(|entry| entry.0 == cbaud)
        .map_or(38400, |entry| entry.1)
}

unsafe fn termios_to_linux(darwin: &DarwinTermios) -> LinuxTermios {
    let mut linux: LinuxTermios = std::mem::zeroed();
    linux.c_iflag = to_linux(darwin.c_iflag, IFLAG_BITS);
    linux.c_oflag = to_linux(darwin.c_oflag, OFLAG_BITS);
    linux.c_cflag = to_linux(darwin.c_cflag, CFLAG_BITS);
    linux.c_cflag |= match darwin.c_cflag & 0x300 {
        0x000 => 0x00,
        0x100 => 0x10,
        0x200 => 0x20,
        _ => 0x30,
    };
    linux.c_cflag |= speed_to_linux(darwin.c_ospeed);
    linux.c_lflag = to_linux(darwin.c_lflag, LFLAG_BITS);
    for (linux_index, darwin_index) in CC_MAP.iter().enumerate() {
        if *darwin_index >= 0 {
            linux.c_cc[linux_index] = darwin.c_cc[*darwin_index as usize];
        }
    }
    linux.c_ispeed = darwin.c_ispeed as u32;
    linux.c_ospeed = darwin.c_ospeed as u32;
    linux
}

unsafe fn termios_to_darwin(linux: &LinuxTermios, darwin: &mut DarwinTermios) {
    darwin.c_iflag = to_darwin(linux.c_iflag, IFLAG_BITS);
    darwin.c_oflag = to_darwin(linux.c_oflag, OFLAG_BITS);
    darwin.c_cflag = to_darwin(linux.c_cflag, CFLAG_BITS);
    darwin.c_cflag |= match linux.c_cflag & 0x30 {
        0x00 => 0,
        0x10 => 0x100,
        0x20 => 0x200,
        _ => 0x300,
    };
    darwin.c_lflag = to_darwin(linux.c_lflag, LFLAG_BITS);
    for (linux_index, darwin_index) in CC_MAP.iter().enumerate() {
        if *darwin_index >= 0 {
            darwin.c_cc[*darwin_index as usize] = linux.c_cc[linux_index];
        }
    }
    let speed = speed_to_darwin(linux.c_cflag & 0x100f);
    darwin.c_ispeed = speed;
    darwin.c_ospeed = speed;
}

/// Release the private slave when the guest opens its `/dev/pts/N` path.
#[no_mangle]
pub unsafe extern "C" fn lxrt_pty_slave_opened(host_path: *const c_char) {
    if host_path.is_null() {
        return;
    }
    let path = CStr::from_ptr(host_path).to_bytes();
    let Some(suffix) = path.strip_prefix(b"/dev/ttys") else {
        return;
    };
    let number = suffix
        .iter()
        .take_while(|byte| byte.is_ascii_digit())
        .try_fold(0i32, |number, digit| {
            number.checked_mul(10)?.checked_add((digit - b'0') as i32)
        });
    let Some(number) = number else { return };
    let mut held = held_ptys();
    for slot in held.iter_mut() {
        if slot.slave > 0 && slot.number == number {
            held_release(slot);
        }
    }
}

/// Release private pty state when its master descriptor closes.
#[no_mangle]
pub unsafe extern "C" fn lxrt_pty_close(fd: c_int) {
    let mut held = held_ptys();
    for slot in held.iter_mut() {
        if slot.slave > 0 && slot.master == fd {
            held_release(slot);
        }
    }
}

/// Translate Linux ioctl requests handled by TTYs and generic file descriptors.
#[no_mangle]
pub unsafe extern "C" fn lxrt_ioctl(fd: c_int, request: c_ulong, arg: u64) -> c_long {
    let pointer = arg as usize as *mut c_void;
    if lxrt_evdev_is(fd) {
        return lxrt_evdev_ioctl(fd, request, arg);
    }
    if lxrt_binder_is(fd) {
        return lxrt_binder_ioctl(fd, request, arg);
    }
    if lxrt_ntsync_request(request) {
        let mut result = 0;
        if lxrt_ntsync_ioctl(fd, request, arg, &mut result) {
            return result;
        }
    }
    lxrt_binder_note_stray_ioctl(fd, request);

    match request as u32 {
        L_TCGETS | L_TCGETS2 => {
            let mut darwin: DarwinTermios = std::mem::zeroed();
            if pty_retry(fd, || tcgetattr(fd, &mut darwin)) != 0 {
                return linux_error(errno());
            }
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            let linux = termios_to_linux(&darwin);
            let length = if request as u32 == L_TCGETS2 {
                L_TERMIOS2_SIZE
            } else {
                L_TERMIOS_SIZE
            };
            ptr::copy_nonoverlapping(
                (&linux as *const LinuxTermios).cast::<u8>(),
                pointer.cast::<u8>(),
                length,
            );
            0
        }
        L_TCSETS | L_TCSETSW | L_TCSETSF | L_TCSETS2 | L_TCSETSW2 | L_TCSETSF2 => {
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            let mut darwin: DarwinTermios = std::mem::zeroed();
            if pty_retry(fd, || tcgetattr(fd, &mut darwin)) != 0 {
                return linux_error(errno());
            }
            let mut linux: LinuxTermios = std::mem::zeroed();
            let two = matches!(request as u32, L_TCSETS2 | L_TCSETSW2 | L_TCSETSF2);
            ptr::copy_nonoverlapping(
                pointer.cast::<u8>(),
                (&mut linux as *mut LinuxTermios).cast::<u8>(),
                if two { L_TERMIOS2_SIZE } else { L_TERMIOS_SIZE },
            );
            termios_to_darwin(&linux, &mut darwin);
            let action = match request as u32 {
                L_TCSETS | L_TCSETS2 => TCSANOW,
                L_TCSETSW | L_TCSETSW2 => TCSADRAIN,
                _ => TCSAFLUSH,
            };
            if tcsetattr(fd, action, &darwin) == 0 {
                0
            } else {
                linux_error(errno())
            }
        }
        L_TIOCGWINSZ => {
            let mut size = WinSize {
                ws_row: 0,
                ws_col: 0,
                ws_xpixel: 0,
                ws_ypixel: 0,
            };
            if pty_retry(fd, || ioctl(fd, DARWIN_IOCTL_TIOCGWINSZ, &mut size)) != 0 {
                return linux_error(errno());
            }
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            ptr::copy_nonoverlapping(
                (&size as *const WinSize).cast::<u8>(),
                pointer.cast::<u8>(),
                std::mem::size_of::<WinSize>(),
            );
            0
        }
        L_TIOCSWINSZ => {
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            let size = ptr::read_unaligned(pointer.cast::<WinSize>());
            if pty_retry(fd, || ioctl(fd, DARWIN_IOCTL_TIOCSWINSZ, &size)) == 0 {
                0
            } else {
                linux_error(errno())
            }
        }
        L_TIOCGPGRP => {
            let group = tcgetpgrp(fd);
            if group < 0 {
                return linux_error(errno());
            }
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            ptr::write_unaligned(pointer.cast::<i32>(), group);
            0
        }
        L_TIOCSPGRP => {
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            let group = ptr::read_unaligned(pointer.cast::<i32>());
            if tcsetpgrp(fd, group) == 0 {
                0
            } else {
                linux_error(errno())
            }
        }
        L_TIOCGSID => {
            let session = tcgetsid(fd);
            if session < 0 {
                return linux_error(errno());
            }
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            ptr::write_unaligned(pointer.cast::<i32>(), session);
            0
        }
        L_TIOCSCTTY => ioctl_result(ioctl(fd, DARWIN_IOCTL_TIOCSCTTY, 0 as c_int)),
        L_TIOCNOTTY => ioctl_result(ioctl(fd, DARWIN_IOCTL_TIOCNOTTY, 0 as c_int)),
        L_TIOCEXCL => ioctl_result(ioctl(fd, DARWIN_IOCTL_TIOCEXCL, 0 as c_int)),
        L_TIOCNXCL => ioctl_result(ioctl(fd, DARWIN_IOCTL_TIOCNXCL, 0 as c_int)),
        L_TIOCOUTQ => {
            let mut count = 0;
            if ioctl(fd, DARWIN_IOCTL_TIOCOUTQ, &mut count) != 0 {
                return linux_error(errno());
            }
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            ptr::write_unaligned(pointer.cast::<i32>(), count);
            0
        }
        L_TCFLSH => {
            let queue = match arg as c_int {
                0 => TCIFLUSH,
                1 => TCOFLUSH,
                _ => TCIOFLUSH,
            };
            if tcflush(fd, queue) == 0 {
                0
            } else {
                linux_error(errno())
            }
        }
        L_FIONREAD => {
            let mut count = 0;
            if lxrt_inotify_is(fd) {
                let queued = lxrt_inotify_pending(fd);
                if queued < 0 {
                    return queued;
                }
                count = queued as c_int;
            } else if ioctl(fd, DARWIN_IOCTL_FIONREAD, &mut count) != 0 {
                return linux_error(errno());
            }
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            ptr::write_unaligned(pointer.cast::<i32>(), count);
            0
        }
        L_FIONBIO => {
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            let enabled = ptr::read_unaligned(pointer.cast::<i32>()) != 0;
            let flags = fcntl(fd, F_GETFL);
            if flags < 0 {
                return linux_error(errno());
            }
            let flags = if enabled {
                flags | O_NONBLOCK
            } else {
                flags & !O_NONBLOCK
            };
            if fcntl(fd, F_SETFL, flags) == 0 {
                0
            } else {
                linux_error(errno())
            }
        }
        L_FIOCLEX => {
            if fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 {
                0
            } else {
                linux_error(errno())
            }
        }
        L_FIONCLEX => {
            if fcntl(fd, F_SETFD, 0) == 0 {
                0
            } else {
                linux_error(errno())
            }
        }
        L_TIOCGPTN => {
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            let Some(number) = pty_number(fd) else {
                return linux_error(25);
            };
            ptr::write_unaligned(pointer.cast::<u32>(), number as u32);
            0
        }
        L_TIOCSPTLCK => {
            if pointer.is_null() {
                return linux_error(EFAULT);
            }
            if ptr::read_unaligned(pointer.cast::<i32>()) != 0 {
                return if pty_number(fd).is_some() {
                    0
                } else {
                    linux_error(25)
                };
            }
            if ioctl(fd, DARWIN_IOCTL_TIOCPTYGRANT, 0 as c_int) != 0
                || ioctl(fd, DARWIN_IOCTL_TIOCPTYUNLK, 0 as c_int) != 0
            {
                linux_error(errno())
            } else {
                0
            }
        }
        L_TIOCGPTPEER => {
            let Some(name) = pty_name(fd) else {
                return linux_error(25);
            };
            let flags = lxrt_open_flags_to_darwin(arg as c_int);
            let slave = open(name.as_ptr(), flags);
            if slave >= 0 {
                slave as c_long
            } else {
                linux_error(errno())
            }
        }
        _ => {
            let mut result = 0;
            if lxrt_netif_ioctl(fd, request, pointer, &mut result) {
                return result;
            }
            if lxrt_trace_on() {
                let _ = fprintf(
                    lxrt_trace_stream(),
                    b"[lxrt] ioctl: unknown request 0x%lx on fd %d -> ENOTTY\n\0"
                        .as_ptr()
                        .cast(),
                    request,
                    fd,
                );
            }
            linux_error(25)
        }
    }
}

#[inline]
unsafe fn ioctl_result(result: c_int) -> c_long {
    if result == 0 {
        0
    } else {
        linux_error(errno())
    }
}
