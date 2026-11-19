// Linux and Darwin assign different values to open flags, *at flags, fcntl
// lock commands, and struct flock fields. Host constants come from the macOS
// SDK; Linux ABI values remain explicit here.

use std::ffi::c_void;

include!(concat!(env!("OUT_DIR"), "/fcntl_constants.rs"));

const L_O_CREAT: i32 = 0x000040;
const L_O_EXCL: i32 = 0x000080;
const L_O_NOCTTY: i32 = 0x000100;
const L_O_TRUNC: i32 = 0x000200;
const L_O_APPEND: i32 = 0x000400;
const L_O_NONBLOCK: i32 = 0x000800;
const L_O_DSYNC: i32 = 0x001000;
const L_O_DIRECTORY: i32 = 0x004000;
const L_O_NOFOLLOW: i32 = 0x008000;
const L_O_SYNC: i32 = 0x101000;
const L_O_CLOEXEC: i32 = 0x080000;
const L_O_PATH: i32 = 0x200000;

const L_AT_SYMLINK_NOFOLLOW: i32 = 0x100;
const L_AT_REMOVEDIR: i32 = 0x200;
const L_AT_SYMLINK_FOLLOW: i32 = 0x400;
const L_AT_EMPTY_PATH: i32 = 0x1000;

const L_F_GETLK: i32 = 5;
const L_F_SETLK: i32 = 6;
const L_F_SETLKW: i32 = 7;
const L_F_OFD_GETLK: i32 = 36;
const L_F_OFD_SETLK: i32 = 37;
const L_F_OFD_SETLKW: i32 = 38;

#[repr(C)]
struct LinuxFlock {
    l_type: i16,
    l_whence: i16,
    pad: i32,
    l_start: i64,
    l_len: i64,
    l_pid: i32,
    pad2: i32,
}

#[repr(C)]
struct DarwinFlock {
    l_start: i64,
    l_len: i64,
    l_pid: i32,
    l_type: i16,
    l_whence: i16,
}

#[no_mangle]
pub extern "C" fn lxrt_open_flags_to_darwin(lflags: i32) -> i32 {
    let mut darwin = lflags & 3;
    if lflags & L_O_CREAT != 0 {
        darwin |= D_O_CREAT;
    }
    if lflags & L_O_EXCL != 0 {
        darwin |= D_O_EXCL;
    }
    if lflags & L_O_NOCTTY != 0 {
        darwin |= D_O_NOCTTY;
    }
    if lflags & L_O_TRUNC != 0 {
        darwin |= D_O_TRUNC;
    }
    if lflags & L_O_APPEND != 0 {
        darwin |= D_O_APPEND;
    }
    if lflags & L_O_NONBLOCK != 0 {
        darwin |= D_O_NONBLOCK;
    }
    if lflags & L_O_DIRECTORY != 0 {
        darwin |= D_O_DIRECTORY;
    }
    if lflags & L_O_NOFOLLOW != 0 {
        darwin |= D_O_NOFOLLOW;
    }
    if lflags & L_O_CLOEXEC != 0 {
        darwin |= D_O_CLOEXEC;
    }
    if (lflags & L_O_SYNC) == L_O_SYNC {
        darwin |= D_O_SYNC;
    } else if lflags & L_O_DSYNC != 0 {
        darwin |= D_O_DSYNC;
    }
    if lflags & L_O_PATH != 0 {
        darwin =
            D_O_RDONLY | D_O_NONBLOCK | (darwin & (D_O_DIRECTORY | D_O_CLOEXEC | D_O_NOFOLLOW));
        if lflags & L_O_NOFOLLOW != 0 {
            darwin = (darwin & !D_O_NOFOLLOW) | D_O_SYMLINK;
        }
    }
    darwin
}

#[no_mangle]
pub extern "C" fn lxrt_open_flags_to_linux(dflags: i32) -> i32 {
    let mut linux = dflags & 3;
    if dflags & D_O_CREAT != 0 {
        linux |= L_O_CREAT;
    }
    if dflags & D_O_EXCL != 0 {
        linux |= L_O_EXCL;
    }
    if dflags & D_O_NOCTTY != 0 {
        linux |= L_O_NOCTTY;
    }
    if dflags & D_O_TRUNC != 0 {
        linux |= L_O_TRUNC;
    }
    if dflags & D_O_APPEND != 0 {
        linux |= L_O_APPEND;
    }
    if dflags & D_O_NONBLOCK != 0 {
        linux |= L_O_NONBLOCK;
    }
    if dflags & D_O_DIRECTORY != 0 {
        linux |= L_O_DIRECTORY;
    }
    if dflags & D_O_NOFOLLOW != 0 {
        linux |= L_O_NOFOLLOW;
    }
    if dflags & D_O_CLOEXEC != 0 {
        linux |= L_O_CLOEXEC;
    }
    linux
}

#[no_mangle]
pub extern "C" fn lxrt_at_flags_to_darwin(lflags: i32) -> i32 {
    let mut darwin = 0;
    if lflags & L_AT_SYMLINK_NOFOLLOW != 0 {
        darwin |= D_AT_SYMLINK_NOFOLLOW;
    }
    if lflags & L_AT_SYMLINK_FOLLOW != 0 {
        darwin |= D_AT_SYMLINK_FOLLOW;
    }
    darwin
}

#[no_mangle]
pub extern "C" fn lxrt_at_is_removedir(lflags: i32) -> bool {
    lflags & L_AT_REMOVEDIR != 0
}

#[no_mangle]
pub extern "C" fn lxrt_at_is_empty_path(lflags: i32) -> bool {
    lflags & L_AT_EMPTY_PATH != 0
}

#[no_mangle]
pub extern "C" fn lxrt_dirfd_to_darwin(dirfd: i32) -> i32 {
    if dirfd == -100 {
        D_AT_FDCWD
    } else {
        dirfd
    }
}

fn locktype_to_darwin(linux: i16) -> i16 {
    match linux {
        0 => D_F_RDLCK as i16,
        1 => D_F_WRLCK as i16,
        _ => D_F_UNLCK as i16,
    }
}

fn locktype_to_linux(darwin: i16) -> i16 {
    if darwin == D_F_RDLCK as i16 {
        0
    } else if darwin == D_F_WRLCK as i16 {
        1
    } else {
        2
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_fcntl_lock_cmd(lcmd: i32, is_lock: *mut bool) -> i32 {
    *is_lock = true;
    match lcmd {
        L_F_GETLK | L_F_OFD_GETLK => D_F_GETLK,
        L_F_SETLK | L_F_OFD_SETLK => D_F_SETLK,
        L_F_SETLKW | L_F_OFD_SETLKW => D_F_SETLKW,
        _ => {
            *is_lock = false;
            -1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_flock_to_darwin(lin: *const c_void, out: *mut c_void) {
    let linux = &*lin.cast::<LinuxFlock>();
    let darwin = &mut *out.cast::<DarwinFlock>();
    darwin.l_start = linux.l_start;
    darwin.l_len = linux.l_len;
    darwin.l_pid = linux.l_pid;
    darwin.l_type = locktype_to_darwin(linux.l_type);
    darwin.l_whence = linux.l_whence;
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_flock_to_linux(darwin: *const c_void, lout: *mut c_void) {
    let darwin = &*darwin.cast::<DarwinFlock>();
    let linux = &mut *lout.cast::<LinuxFlock>();
    linux.l_type = locktype_to_linux(darwin.l_type);
    linux.l_whence = darwin.l_whence;
    linux.pad = 0;
    linux.l_start = darwin.l_start;
    linux.l_len = darwin.l_len;
    linux.l_pid = darwin.l_pid;
    linux.pad2 = 0;
}
