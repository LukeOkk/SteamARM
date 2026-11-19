// Linux inotify on Darwin kqueue. All failures return negative LINUX errno.
// See inotify.h for the Linux ABI, dispatch contract and Darwin divergences.
//
// This is a line-for-line port of the former inotify.c: the same pthread
// mutex, the same malloc'd lists, the same worker thread and the same host
// calls in the same order. Nothing here may panic or unwind.

use std::ffi::{c_char, c_int, c_long, c_short, c_uint, c_ulong, c_void};
use std::mem::{size_of, MaybeUninit};
use std::ptr;

// Darwin errno values (host side; converted with lxrt_errno_to_linux).
const EINTR: c_int = 4;
const ENOENT: c_int = 2;
const EBADF: c_int = 9;
const ENOMEM: c_int = 12;
const EFAULT: c_int = 14;
const ENOTDIR: c_int = 20;
const EINVAL: c_int = 22;
const EMFILE: c_int = 24;
const ENOSPC: c_int = 28;
const EAGAIN: c_int = 35;
const ENAMETOOLONG: c_int = 63;

// Darwin <fcntl.h>, <sys/ioctl.h>, <sys/event.h>, <poll.h>, <sys/stat.h>.
const F_DUPFD: c_int = 0;
const F_GETFD: c_int = 1;
const F_SETFD: c_int = 2;
const F_GETFL: c_int = 3;
const F_SETFL: c_int = 4;
const F_DUPFD_CLOEXEC: c_int = 67;
const FD_CLOEXEC: c_int = 1;
const O_NONBLOCK: c_int = 0x4;
const O_EVTONLY: c_int = 0x8000;
const O_SYMLINK: c_int = 0x0020_0000;
const O_CLOEXEC: c_int = 0x0100_0000;
const AT_SYMLINK_NOFOLLOW: c_int = 0x20;
const FIONREAD: c_ulong = 0x4004_667f;
const EVFILT_VNODE: i16 = -4;
const EVFILT_USER: i16 = -10;
const EV_ADD: u16 = 0x1;
const EV_CLEAR: u16 = 0x20;
const EV_ERROR: u16 = 0x4000;
const NOTE_TRIGGER: u32 = 0x0100_0000;
const NOTE_DELETE: u32 = 0x1;
const NOTE_WRITE: u32 = 0x2;
const NOTE_EXTEND: u32 = 0x4;
const NOTE_ATTRIB: u32 = 0x8;
const NOTE_LINK: u32 = 0x10;
const NOTE_RENAME: u32 = 0x20;
const POLLIN: c_short = 0x1;
const S_IFMT: u16 = 0o170000;
const S_IFDIR: u16 = 0o040000;
const PATH_MAX: usize = 1024;
const INT_MAX: c_long = c_int::MAX as c_long;

// Mach.
const KERN_SUCCESS: c_int = 0;
const VM_REGION_BASIC_INFO_64: c_int = 9;
const VM_REGION_BASIC_INFO_COUNT_64: u32 = (size_of::<VmRegionBasicInfo64>() / 4) as u32;
const VM_PROT_WRITE: c_int = 2;

// Linux inotify ABI.
const I_MODIFY: u32 = 0x2;
const I_ATTRIB: u32 = 0x4;
const I_FROM: u32 = 0x40;
const I_TO: u32 = 0x80;
const I_CREATE: u32 = 0x100;
const I_DELETE: u32 = 0x200;
const I_DELETE_SELF: u32 = 0x400;
const I_MOVE_SELF: u32 = 0x800;
const I_OVERFLOW: u32 = 0x4000;
const I_IGNORED: u32 = 0x8000;
const I_ONLYDIR: u32 = 0x0100_0000;
const I_NOFOLLOW: u32 = 0x0200_0000;
const I_MASK_ADD: u32 = 0x2000_0000;
const I_ISDIR: u32 = 0x4000_0000;
const I_ONESHOT: u32 = 0x8000_0000;
const I_EVENTS: u32 = 0xfff;
const I_NONBLOCK: c_int = 0x800;
const I_CLOEXEC: c_int = 0x80000;
const FD_LIMIT: usize = 1_048_576;
const QUEUE_LIMIT: c_uint = 16384;

type MachPort = u32;
type PthreadT = *mut c_void;

#[repr(C)]
#[derive(Clone, Copy)]
struct InoEvent {
    wd: i32,
    mask: u32,
    cookie: u32,
    len: u32,
}
const _: [(); 16] = [(); size_of::<InoEvent>()]; // Linux inotify ABI

#[repr(C)]
struct Record {
    next: *mut Record,
    event: InoEvent,
    name: [c_char; 256],
}

#[repr(C)]
struct DirEntry {
    next: *mut DirEntry,
    ino: u64,
    isdir: bool,
    matched: bool,
    name: [c_char; 256],
}

#[repr(C)]
struct Watch {
    next: *mut Watch,
    fd: c_int,
    wd: c_int,
    dev: i32,
    ino: u64,
    mask: u32,
    isdir: bool,
    dead: bool,
    entries: *mut DirEntry,
}

#[repr(C)]
struct Instance {
    kq: c_int,
    reader: c_int,
    writer: c_int,
    thread: PthreadT,
    refs: c_uint,
    aliases: c_uint,
    queued: c_uint,
    cookie: u32,
    next_wd: i64,
    stopping: bool,
    overflow_queued: bool,
    watches: *mut Watch,
    head: *mut Record,
    tail: *mut Record,
    overflow: Record,
}

// Darwin pthread_mutex_t: a signature word plus 56 opaque bytes.
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}
const _: [(); 64] = [(); size_of::<PthreadMutex>()];
const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;

#[repr(C)]
struct KEvent {
    ident: usize,
    filter: i16,
    flags: u16,
    fflags: u32,
    data: isize,
    udata: *mut c_void,
}
const _: [(); 32] = [(); size_of::<KEvent>()];

#[repr(C)]
struct PollFd {
    fd: c_int,
    events: c_short,
    revents: c_short,
}

#[repr(C)]
struct Timespec {
    tv_sec: c_long,
    tv_nsec: c_long,
}

// Darwin arm64 struct stat (64-bit inodes).
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

// Darwin arm64 struct dirent (64-bit inodes).
#[repr(C)]
struct Dirent {
    d_ino: u64,
    d_seekoff: u64,
    d_reclen: u16,
    d_namlen: u16,
    d_type: u8,
    d_name: [c_char; 1024],
}
const _: [(); 1048] = [(); size_of::<Dirent>()];

// vm_region_basic_info_data_64_t (declared under #pragma pack(4)).
#[repr(C, packed(4))]
struct VmRegionBasicInfo64 {
    protection: c_int,
    max_protection: c_int,
    inheritance: u32,
    shared: u32,
    reserved: u32,
    offset: u64,
    behavior: c_int,
    user_wired_count: u16,
}
const _: [(); 36] = [(); size_of::<VmRegionBasicInfo64>()];

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: MachPort;
    fn mach_vm_region(
        task: MachPort,
        address: *mut u64,
        size: *mut u64,
        flavor: c_int,
        info: *mut c_int,
        count: *mut u32,
        object: *mut MachPort,
    ) -> c_int;
    fn mach_vm_read_overwrite(
        task: MachPort,
        address: u64,
        size: u64,
        data: u64,
        out_size: *mut u64,
    ) -> c_int;
    fn mach_port_deallocate(task: MachPort, name: MachPort) -> c_int;
    fn __error() -> *mut c_int;
    fn calloc(count: usize, size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
    fn strlen(s: *const c_char) -> usize;
    fn strlcpy(dst: *mut c_char, src: *const c_char, size: usize) -> usize;
    fn close(fd: c_int) -> c_int;
    fn read(fd: c_int, buf: *mut c_void, count: usize) -> isize;
    fn write(fd: c_int, buf: *const c_void, count: usize) -> isize;
    fn pipe(fds: *mut c_int) -> c_int;
    // Variadic in C: Darwin arm64 passes variadic arguments on the stack, so
    // these must be declared variadic here too.
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn ioctl(fd: c_int, request: c_ulong, ...) -> c_int;
    fn fstat(fd: c_int, st: *mut Stat) -> c_int;
    fn fstatat(dirfd: c_int, path: *const c_char, st: *mut Stat, flags: c_int) -> c_int;
    fn dirfd(dir: *mut c_void) -> c_int;
    // Returns struct dirent *. dirents.rs declares it with its own private
    // dirent type, so the (ABI-identical) declarations cannot be spelled alike.
    #[allow(clashing_extern_declarations)]
    fn readdir(dir: *mut c_void) -> *mut c_void;
    fn kqueue() -> c_int;
    fn kevent(
        kq: c_int,
        changes: *const KEvent,
        nchanges: c_int,
        events: *mut KEvent,
        nevents: c_int,
        timeout: *const Timespec,
    ) -> c_int;
    fn poll(fds: *mut PollFd, count: c_uint, timeout: c_int) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_create(
        thread: *mut PthreadT,
        attributes: *const c_void,
        start: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
        argument: *mut c_void,
    ) -> c_int;
    fn pthread_join(thread: PthreadT, value: *mut *mut c_void) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;

    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_opendirat_private(dirfd: c_int, path: *const c_char) -> *mut c_void;
    fn lxrt_closedir_private(dir: *mut c_void);
    fn lxrt_translate_guest_path(
        path: *const c_char,
        out: *mut c_char,
        size: usize,
    ) -> *const c_char;
}

// One lock defines the ordering of events, watch removal and readiness.
// No blocking kevent/poll or pthread_join is performed while holding it.
static mut LOCK: PthreadMutex = PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
};
static mut INSTANCES: [*mut Instance; FD_LIMIT] = [ptr::null_mut(); FD_LIMIT];

// LXRT_FORK_SAFE(inotify_lock, lock): taken before fork, released after it in
// both parent and child (the child's only thread is the one that took it).
extern "C" fn inotify_lock_fork_prepare() {
    unsafe { lock() };
}
extern "C" fn inotify_lock_fork_release() {
    unsafe { unlock() };
}
extern "C" fn inotify_lock_fork_register() {
    unsafe {
        pthread_atfork(
            Some(inotify_lock_fork_prepare),
            Some(inotify_lock_fork_release),
            Some(inotify_lock_fork_release),
        );
    }
}
#[used]
#[link_section = "__DATA,__mod_init_func"]
static INOTIFY_LOCK_FORK_REGISTER: extern "C" fn() = inotify_lock_fork_register;

#[inline(always)]
unsafe fn lock() {
    pthread_mutex_lock(ptr::addr_of_mut!(LOCK));
}
#[inline(always)]
unsafe fn unlock() {
    pthread_mutex_unlock(ptr::addr_of_mut!(LOCK));
}
#[inline(always)]
unsafe fn errno() -> c_int {
    *__error()
}
#[inline(always)]
unsafe fn lerr(error: c_int) -> c_long {
    (-lxrt_errno_to_linux(error)) as c_long
}
#[inline(always)]
unsafe fn slot(fd: c_int) -> *mut *mut Instance {
    (ptr::addr_of_mut!(INSTANCES) as *mut *mut Instance).add(fd as usize)
}
#[inline(always)]
fn s_isdir(mode: u16) -> bool {
    mode & S_IFMT == S_IFDIR
}
// EV_SET
#[inline(always)]
fn ev_set(ident: usize, filter: i16, flags: u16, fflags: u32, udata: *mut c_void) -> KEvent {
    KEvent {
        ident,
        filter,
        flags,
        fflags,
        data: 0,
        udata,
    }
}

unsafe fn lookup(fd: c_int) -> *mut Instance {
    if fd >= 0 && (fd as usize) < FD_LIMIT {
        *slot(fd)
    } else {
        ptr::null_mut()
    }
}

unsafe fn range_ok(pointer: *mut c_void, len: usize) -> bool {
    let mut a = pointer as usize as u64;
    let end = a.wrapping_add(len as u64);
    if pointer.is_null() || end < a {
        return false;
    }
    while a < end {
        let mut start = a;
        let mut size = 0u64;
        let mut info = MaybeUninit::<VmRegionBasicInfo64>::zeroed();
        let mut count = VM_REGION_BASIC_INFO_COUNT_64;
        let mut object: MachPort = 0; // MACH_PORT_NULL
        let kr = mach_vm_region(
            mach_task_self_,
            &mut start,
            &mut size,
            VM_REGION_BASIC_INFO_64,
            info.as_mut_ptr().cast(),
            &mut count,
            &mut object,
        );
        if object != 0 {
            mach_port_deallocate(mach_task_self_, object);
        }
        if kr != KERN_SUCCESS
            || start > a
            || size == 0
            || (*info.as_ptr()).protection & VM_PROT_WRITE == 0
        {
            return false;
        }
        a = start.wrapping_add(size);
    }
    true
}

// Kernel-assisted copy-in makes even an invalid guest pathname an EFAULT.
unsafe fn copy_path(src: *const c_char, dst: *mut c_char) -> c_int {
    if src.is_null() {
        return EFAULT;
    }
    let mut n = 0usize;
    while n < PATH_MAX {
        let mut got = 0u64;
        if mach_vm_read_overwrite(
            mach_task_self_,
            (src as usize as u64).wrapping_add(n as u64),
            1,
            dst.add(n) as usize as u64,
            &mut got,
        ) != KERN_SUCCESS
            || got != 1
        {
            return EFAULT;
        }
        if *dst.add(n) == 0 {
            return 0;
        }
        n += 1;
    }
    ENAMETOOLONG
}

unsafe fn free_entries(mut e: *mut DirEntry) {
    while !e.is_null() {
        let next = (*e).next;
        free(e.cast());
        e = next;
    }
}

// Enumerate via the watched vnode, not a saved pathname: watching continues
// after the directory itself is renamed. openat gives an independent offset.
unsafe fn snapshot(fd: c_int, out: *mut *mut DirEntry) -> c_int {
    *out = ptr::null_mut();
    let dir = lxrt_opendirat_private(fd, c".".as_ptr());
    if dir.is_null() {
        return errno();
    }
    let scanfd = dirfd(dir);
    let error;
    loop {
        *__error() = 0;
        let de = readdir(dir) as *mut Dirent;
        if de.is_null() {
            error = errno();
            break;
        }
        let name = ptr::addr_of!((*de).d_name) as *const c_char;
        if strcmp(name, c".".as_ptr()) == 0 || strcmp(name, c"..".as_ptr()) == 0 {
            continue;
        }
        let mut st = MaybeUninit::<Stat>::uninit();
        if fstatat(scanfd, name, st.as_mut_ptr(), AT_SYMLINK_NOFOLLOW) < 0 {
            if errno() == ENOENT {
                continue; // raced an unlink
            }
            error = errno();
            break;
        }
        let st = st.as_ptr();
        let e = calloc(1, size_of::<DirEntry>()) as *mut DirEntry;
        if e.is_null() {
            error = ENOMEM;
            break;
        }
        (*e).ino = (*st).st_ino;
        (*e).isdir = s_isdir((*st).st_mode);
        strlcpy(ptr::addr_of_mut!((*e).name) as *mut c_char, name, 256);
        (*e).next = *out;
        *out = e;
    }
    lxrt_closedir_private(dir);
    if error != 0 {
        free_entries(*out);
        *out = ptr::null_mut();
    }
    error
}

// Readiness is level-triggered: the private pipe holds bytes exactly while
// events are queued. Drain every pending byte (not just one) whenever the
// queue is empty; a stray byte left behind made poll() report the pipe
// readable forever, and Chromium's inotify reader thread in Steam's web
// helper spun at 100% CPU in lxrt_inotify_read -- the Steam window never
// appeared (MEASURED with sample(1)).
unsafe fn drain_ready(i: *mut Instance) {
    let mut pending: c_int = 0;
    let mut sink = MaybeUninit::<[u8; 64]>::uninit();
    while ioctl((*i).reader, FIONREAD, &mut pending as *mut c_int) == 0 && pending > 0 {
        let want = if pending < 64 { pending as usize } else { 64 };
        let n = read((*i).reader, sink.as_mut_ptr().cast(), want);
        if n <= 0 && errno() != EINTR {
            break;
        }
    }
}

unsafe fn signal_ready(i: *mut Instance) {
    let byte: u8 = b'i';
    loop {
        let n = write((*i).writer, (&byte as *const u8).cast(), 1);
        if !(n < 0 && errno() == EINTR) {
            break;
        }
    }
    // The private reader stays alive until destruction. One outstanding byte
    // cannot fill a pipe; EAGAIN would only mean readiness is already present.
}

unsafe fn append(i: *mut Instance, r: *mut Record) {
    (*r).next = ptr::null_mut();
    if !(*i).tail.is_null() {
        (*(*i).tail).next = r;
    } else {
        (*i).head = r;
        signal_ready(i);
    }
    (*i).tail = r;
    (*i).queued = (*i).queued.wrapping_add(1);
}

unsafe fn overflow(i: *mut Instance) {
    if (*i).overflow_queued {
        return;
    }
    (*i).overflow_queued = true;
    let record = ptr::addr_of_mut!((*i).overflow);
    ptr::write_bytes(record, 0, 1);
    (*record).event.wd = -1;
    (*record).event.mask = I_OVERFLOW;
    append(i, record);
}

unsafe fn enqueue(i: *mut Instance, wd: c_int, mask: u32, cookie: u32, name: *const c_char) {
    if (*i).queued >= QUEUE_LIMIT {
        overflow(i);
        return;
    }
    let r = calloc(1, size_of::<Record>()) as *mut Record;
    if r.is_null() {
        overflow(i);
        return;
    }
    (*r).event = InoEvent {
        wd,
        mask,
        cookie,
        len: 0,
    };
    if !name.is_null() {
        let n = strlen(name).wrapping_add(1);
        (*r).event.len = (n.wrapping_add(15) & !15usize) as u32;
        ptr::copy_nonoverlapping(name, ptr::addr_of_mut!((*r).name) as *mut c_char, n);
    }
    append(i, r);
}

unsafe fn retire(i: *mut Instance, w: *mut Watch) {
    if (*w).dead {
        return;
    }
    (*w).dead = true;
    close((*w).fd); // removes EVFILT_VNODE
    (*w).fd = -1;
    free_entries((*w).entries);
    (*w).entries = ptr::null_mut();
    enqueue(i, (*w).wd, I_IGNORED, 0, ptr::null());
}

unsafe fn emit(
    i: *mut Instance,
    w: *mut Watch,
    mut mask: u32,
    cookie: u32,
    name: *const c_char,
    isdir: bool,
) {
    if (*w).dead || mask & (*w).mask & I_EVENTS == 0 {
        return;
    }
    mask &= (*w).mask & I_EVENTS;
    enqueue(
        i,
        (*w).wd,
        mask | if isdir { I_ISDIR } else { 0 },
        cookie,
        name,
    );
    if (*w).mask & I_ONESHOT != 0 {
        retire(i, w);
    }
}

unsafe fn diff(i: *mut Instance, w: *mut Watch) {
    let mut fresh: *mut DirEntry = ptr::null_mut();
    if snapshot((*w).fd, &mut fresh) != 0 {
        overflow(i);
        return;
    }
    let old = (*w).entries;
    // Unchanged names/inodes first; a replaced name must be DELETE + CREATE.
    let mut a = old;
    while !a.is_null() {
        (*a).matched = false;
        let mut b = fresh;
        while !b.is_null() {
            if (*a).ino == (*b).ino
                && strcmp(
                    ptr::addr_of!((*a).name) as *const c_char,
                    ptr::addr_of!((*b).name) as *const c_char,
                ) == 0
            {
                (*a).matched = true;
                (*b).matched = true;
                break;
            }
            b = (*b).next;
        }
        a = (*a).next;
    }
    // Detach while diffing: ONESHOT may retire the watch during emit().
    (*w).entries = ptr::null_mut();
    a = old;
    while !a.is_null() {
        if !(*a).matched {
            let mut matching: *mut DirEntry = ptr::null_mut();
            let mut b = fresh;
            while !b.is_null() {
                if !(*b).matched && (*a).ino == (*b).ino && (*a).isdir == (*b).isdir {
                    matching = b;
                    break;
                }
                b = (*b).next;
            }
            let a_name = ptr::addr_of!((*a).name) as *const c_char;
            if !matching.is_null() {
                (*matching).matched = true;
                (*i).cookie = (*i).cookie.wrapping_add(1);
                if (*i).cookie == 0 {
                    (*i).cookie = (*i).cookie.wrapping_add(1);
                }
                emit(i, w, I_FROM, (*i).cookie, a_name, (*a).isdir);
                emit(
                    i,
                    w,
                    I_TO,
                    (*i).cookie,
                    ptr::addr_of!((*matching).name) as *const c_char,
                    (*matching).isdir,
                );
            } else {
                emit(i, w, I_DELETE, 0, a_name, (*a).isdir);
            }
        }
        a = (*a).next;
    }
    let mut b = fresh;
    while !b.is_null() {
        if !(*b).matched {
            emit(
                i,
                w,
                I_CREATE,
                0,
                ptr::addr_of!((*b).name) as *const c_char,
                (*b).isdir,
            );
        }
        b = (*b).next;
    }
    free_entries(old);
    if (*w).dead {
        free_entries(fresh);
    } else {
        (*w).entries = fresh;
    }
}

unsafe fn release(i: *mut Instance) {
    (*i).refs = (*i).refs.wrapping_sub(1);
    if (*i).refs != 0 {
        return;
    }
    while !(*i).watches.is_null() {
        let w = (*i).watches;
        (*i).watches = (*w).next;
        if (*w).fd >= 0 {
            close((*w).fd);
        }
        free_entries((*w).entries);
        free(w.cast());
    }
    while !(*i).head.is_null() {
        let r = (*i).head;
        (*i).head = (*r).next;
        if r != ptr::addr_of_mut!((*i).overflow) {
            free(r.cast());
        }
    }
    close((*i).reader);
    close((*i).writer);
    close((*i).kq);
    free(i.cast());
}

extern "C" fn worker(arg: *mut c_void) -> *mut c_void {
    unsafe {
        let i = arg as *mut Instance;
        loop {
            let mut event = MaybeUninit::<KEvent>::uninit();
            let n = kevent((*i).kq, ptr::null(), 0, event.as_mut_ptr(), 1, ptr::null());
            let error = errno();
            lock();
            if (*i).stopping {
                release(i);
                unlock();
                return ptr::null_mut();
            }
            if n < 0 {
                if error != EINTR {
                    overflow(i);
                }
            } else if n != 0 && (*event.as_ptr()).filter == EVFILT_VNODE {
                let event = event.as_ptr();
                // udata holds a monotonically increasing wd, never a freed pointer
                // or recycled host fd. rm_watch racing kevent is therefore harmless.
                let wd = (*event).udata as usize as c_int;
                let mut w = (*i).watches;
                while !w.is_null() && (*w).wd != wd {
                    w = (*w).next;
                }
                if !w.is_null() && !(*w).dead {
                    if (*event).flags & EV_ERROR != 0 {
                        overflow(i);
                    } else {
                        let fflags = (*event).fflags;
                        if (*w).isdir && fflags & NOTE_WRITE != 0 {
                            diff(i, w);
                        }
                        if !(*w).isdir && fflags & (NOTE_WRITE | NOTE_EXTEND) != 0 {
                            emit(i, w, I_MODIFY, 0, ptr::null(), false);
                        }
                        if fflags & (NOTE_ATTRIB | NOTE_LINK) != 0 {
                            emit(i, w, I_ATTRIB, 0, ptr::null(), (*w).isdir);
                        }
                        if fflags & NOTE_RENAME != 0 {
                            emit(i, w, I_MOVE_SELF, 0, ptr::null(), (*w).isdir);
                        }
                        if fflags & NOTE_DELETE != 0 {
                            emit(i, w, I_DELETE_SELF, 0, ptr::null(), (*w).isdir);
                            retire(i, w);
                        }
                    }
                }
                // Dead watches need not survive: kevent identifies only the wd.
                let mut p: *mut *mut Watch = ptr::addr_of_mut!((*i).watches);
                while !(*p).is_null() {
                    if (**p).dead {
                        w = *p;
                        *p = (*w).next;
                        free(w.cast());
                    } else {
                        p = ptr::addr_of_mut!((**p).next);
                    }
                }
            }
            unlock();
        }
    }
}

// syscall 26
#[no_mangle]
pub unsafe extern "C" fn lxrt_inotify_init1(flags: c_int) -> c_long {
    if flags & !(I_NONBLOCK | I_CLOEXEC) != 0 {
        return lerr(EINVAL);
    }
    let i = calloc(1, size_of::<Instance>()) as *mut Instance;
    if i.is_null() {
        return lerr(ENOMEM);
    }
    (*i).kq = -1;
    (*i).reader = -1;
    (*i).writer = -1;
    (*i).refs = 1;
    let mut fds: [c_int; 2] = [0; 2];
    let mut fd: c_int = -1;
    let error: c_int;
    'fail: {
        if pipe(fds.as_mut_ptr()) < 0 {
            error = errno();
            break 'fail;
        }
        (*i).reader = fds[0];
        (*i).writer = fds[1];
        if fcntl((*i).reader, F_SETFD, FD_CLOEXEC) < 0
            || fcntl((*i).writer, F_SETFD, FD_CLOEXEC) < 0
            || fcntl((*i).writer, F_SETFL, O_NONBLOCK) < 0
            || fcntl(
                (*i).reader,
                F_SETFL,
                if flags & I_NONBLOCK != 0 { O_NONBLOCK } else { 0 },
            ) < 0
        {
            error = errno();
            break 'fail;
        }
        fd = fcntl(
            (*i).reader,
            if flags & I_CLOEXEC != 0 {
                F_DUPFD_CLOEXEC
            } else {
                F_DUPFD
            },
            0 as c_int,
        );
        if fd < 0 {
            error = errno();
            break 'fail;
        }
        if fd as usize >= FD_LIMIT {
            error = EMFILE;
            break 'fail;
        }
        (*i).kq = kqueue();
        if (*i).kq < 0 || fcntl((*i).kq, F_SETFD, FD_CLOEXEC) < 0 {
            error = errno();
            break 'fail;
        }
        let wake = ev_set(1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, ptr::null_mut());
        if kevent((*i).kq, &wake, 1, ptr::null_mut(), 0, ptr::null()) < 0 {
            error = errno();
            break 'fail;
        }
        (*i).next_wd = 1;
        (*i).aliases = 1;
        (*i).refs = 2; // alias + worker
        lock();
        error = pthread_create(
            ptr::addr_of_mut!((*i).thread),
            ptr::null(),
            worker,
            i.cast(),
        );
        if error == 0 {
            *slot(fd) = i;
        }
        unlock();
        if error != 0 {
            (*i).refs = 1;
            break 'fail;
        }
        return fd as c_long;
    }
    if fd >= 0 {
        close(fd);
    }
    release(i);
    lerr(error)
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_inotify_is(fd: c_int) -> bool {
    lock();
    let yes = !lookup(fd).is_null();
    unlock();
    yes
}

// syscall 27
#[no_mangle]
pub unsafe extern "C" fn lxrt_inotify_add_watch(
    fd: c_int,
    guest: *const c_char,
    mask: u32,
) -> c_long {
    lock();
    let i = lookup(fd);
    let result: c_long;
    'done: {
        if i.is_null() {
            result = lerr(if fcntl(fd, F_GETFD) < 0 { EBADF } else { EINVAL });
            break 'done;
        }
        if mask & I_EVENTS == 0
            || mask & !(I_EVENTS | I_ONLYDIR | I_NOFOLLOW | I_MASK_ADD | I_ONESHOT) != 0
        {
            result = lerr(EINVAL);
            break 'done;
        }
        let mut path = MaybeUninit::<[c_char; PATH_MAX]>::uninit();
        let path = path.as_mut_ptr() as *mut c_char;
        let mut error = copy_path(guest, path);
        if error != 0 {
            result = lerr(error);
            break 'done;
        }
        // A guest path: through the root, the sandbox binds and /proc, like
        // every file syscall (/dev/input is watched for controller hotplug).
        let mut hostbuf = MaybeUninit::<[c_char; PATH_MAX]>::uninit();
        let translated: *const c_char = if *path == b'/' as c_char {
            lxrt_translate_guest_path(path, hostbuf.as_mut_ptr() as *mut c_char, PATH_MAX)
        } else {
            path
        };
        let watchfd = open(
            translated,
            O_EVTONLY | O_CLOEXEC | if mask & I_NOFOLLOW != 0 { O_SYMLINK } else { 0 },
        );
        if watchfd < 0 {
            result = lerr(errno());
            break 'done;
        }
        let mut st = MaybeUninit::<Stat>::uninit();
        if fstat(watchfd, st.as_mut_ptr()) < 0 {
            result = lerr(errno());
            close(watchfd);
            break 'done;
        }
        let st = st.as_ptr();
        if mask & I_ONLYDIR != 0 && !s_isdir((*st).st_mode) {
            result = lerr(ENOTDIR);
            close(watchfd);
            break 'done;
        }
        let mut w = (*i).watches;
        while !w.is_null() {
            if !(*w).dead && (*w).ino == (*st).st_ino && (*w).dev == (*st).st_dev {
                let interest = mask & (I_EVENTS | I_ONESHOT);
                (*w).mask = if mask & I_MASK_ADD != 0 {
                    (*w).mask | interest
                } else {
                    interest
                };
                result = (*w).wd as c_long;
                close(watchfd);
                break 'done;
            }
            w = (*w).next;
        }
        if (*i).next_wd > INT_MAX as i64 {
            result = lerr(ENOSPC);
            close(watchfd);
            break 'done;
        }
        let w = calloc(1, size_of::<Watch>()) as *mut Watch;
        if w.is_null() {
            result = lerr(ENOMEM);
            close(watchfd);
            break 'done;
        }
        (*w).fd = watchfd;
        (*w).ino = (*st).st_ino;
        (*w).dev = (*st).st_dev;
        (*w).wd = (*i).next_wd as c_int;
        (*w).mask = mask & (I_EVENTS | I_ONESHOT);
        (*w).isdir = s_isdir((*st).st_mode);
        let change = ev_set(
            watchfd as isize as usize,
            EVFILT_VNODE,
            EV_ADD | EV_CLEAR,
            NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB | NOTE_EXTEND | NOTE_LINK,
            (*w).wd as isize as usize as *mut c_void,
        );
        // Register before taking the baseline so a subsequent change cannot fall
        // into a snapshot-to-registration gap. Pre-return changes may be coalesced.
        error = if kevent((*i).kq, &change, 1, ptr::null_mut(), 0, ptr::null()) < 0 {
            errno()
        } else {
            0
        };
        if error == 0 && (*w).isdir {
            error = snapshot(watchfd, ptr::addr_of_mut!((*w).entries));
        }
        if error != 0 {
            close(watchfd);
            free(w.cast());
            result = lerr(error);
            break 'done;
        }
        (*i).next_wd = (*i).next_wd.wrapping_add(1);
        (*w).next = (*i).watches;
        (*i).watches = w;
        result = (*w).wd as c_long;
    }
    unlock();
    result
}

// syscall 28
#[no_mangle]
pub unsafe extern "C" fn lxrt_inotify_rm_watch(fd: c_int, wd: c_int) -> c_long {
    lock();
    let i = lookup(fd);
    let mut result = lerr(if !i.is_null() || fcntl(fd, F_GETFD) >= 0 {
        EINVAL
    } else {
        EBADF
    });
    if !i.is_null() {
        let mut p: *mut *mut Watch = ptr::addr_of_mut!((*i).watches);
        while !(*p).is_null() && (**p).wd != wd {
            p = ptr::addr_of_mut!((**p).next);
        }
        if !(*p).is_null() && !(**p).dead {
            let w = *p;
            retire(i, w);
            *p = (*w).next;
            free(w.cast());
            result = 0;
        }
    }
    unlock();
    result
}

// FIONREAD, as Linux answers it: the size of the queued events. Qt reads
// exactly this many bytes on a blocking descriptor
// (QInotifyFileSystemWatcherEngine::readFromInotify); the pipe's own count
// (its one readiness byte) made that read(fd, buf, 1) fail with EINVAL.
// A readiness byte with nothing queued is drained here, so a poll that
// follows does not report the descriptor readable for nothing.
#[no_mangle]
pub unsafe extern "C" fn lxrt_inotify_pending(fd: c_int) -> c_long {
    lock();
    let i = lookup(fd);
    let mut total: c_long = 0;
    if i.is_null() {
        total = lerr(EBADF);
    } else {
        let mut r = (*i).head;
        while !r.is_null() {
            total = total
                .wrapping_add((size_of::<InoEvent>() + (*r).event.len as usize) as c_long);
            r = (*r).next;
        }
        if (*i).head.is_null() && !(*i).stopping {
            drain_ready(i);
        }
        if total > INT_MAX {
            total = INT_MAX;
        }
    }
    unlock();
    total
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_inotify_read(fd: c_int, buf: *mut c_void, len: usize) -> c_long {
    lock();
    let i = lookup(fd);
    if i.is_null() {
        unlock();
        return lerr(EBADF);
    }
    (*i).refs = (*i).refs.wrapping_add(1);
    let mut result: c_long = 0;
    loop {
        if (*i).stopping {
            result = lerr(EBADF);
            break;
        }
        if !(*i).head.is_null() {
            break;
        }
        if fcntl((*i).reader, F_GETFL) & O_NONBLOCK != 0 {
            result = lerr(EAGAIN);
            break;
        }
        let mut p = PollFd {
            fd: (*i).reader,
            events: POLLIN,
            revents: 0,
        };
        unlock();
        let n = poll(&mut p, 1, -1);
        let error = errno();
        lock();
        if n < 0 {
            result = lerr(error);
            break;
        }
        if (*i).head.is_null() && !(*i).stopping {
            drain_ready(i); // readable but nothing queued
        }
    }
    let buf = buf as *mut u8;
    while result == 0 && !(*i).head.is_null() {
        let mut r = (*i).head;
        let mut size = size_of::<InoEvent>() + (*r).event.len as usize;
        if size > len {
            result = lerr(EINVAL);
            break;
        }
        // Return as many WHOLE events as fit; keep the next one queued.
        let mut used = 0usize;
        loop {
            r = (*i).head;
            size = size_of::<InoEvent>() + (*r).event.len as usize;
            if size > len.wrapping_sub(used) {
                break;
            }
            if !range_ok(buf.wrapping_add(used).cast(), size) {
                result = if used != 0 {
                    used as c_long
                } else {
                    lerr(EFAULT)
                };
                break;
            }
            ptr::copy_nonoverlapping(
                ptr::addr_of!((*r).event) as *const u8,
                buf.add(used),
                size_of::<InoEvent>(),
            );
            ptr::copy_nonoverlapping(
                ptr::addr_of!((*r).name) as *const u8,
                buf.add(used + size_of::<InoEvent>()),
                (*r).event.len as usize,
            );
            used += size;
            (*i).head = (*r).next;
            (*i).queued = (*i).queued.wrapping_sub(1);
            if r == ptr::addr_of_mut!((*i).overflow) {
                (*i).overflow_queued = false;
            } else {
                free(r.cast());
            }
            if (*i).head.is_null() {
                break;
            }
        }
        if (*i).head.is_null() {
            (*i).tail = ptr::null_mut();
            drain_ready(i);
        }
        if result == 0 {
            result = used as c_long;
        }
    }
    release(i);
    unlock();
    result
}

// Drop one alias. Save thread id before dropping references; the worker can
// destroy the instance as soon as the lock is released.
unsafe fn detach(fd: c_int, thread: *mut PthreadT) -> bool {
    let i = lookup(fd);
    if i.is_null() {
        return false;
    }
    *slot(fd) = ptr::null_mut();
    (*i).aliases = (*i).aliases.wrapping_sub(1);
    let last = (*i).aliases == 0;
    if last {
        (*i).stopping = true;
        *thread = (*i).thread;
        if (*i).head.is_null() {
            signal_ready(i); // wake any blocked reads
        }
        let wake = ev_set(1, EVFILT_USER, 0, NOTE_TRIGGER, ptr::null_mut());
        kevent((*i).kq, &wake, 1, ptr::null_mut(), 0, ptr::null());
    }
    release(i);
    last
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_inotify_close(fd: c_int) {
    let mut thread: PthreadT = ptr::null_mut();
    lock();
    let join = detach(fd, &mut thread);
    unlock();
    if join {
        pthread_join(thread, ptr::null_mut());
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_inotify_dup(oldfd: c_int, newfd: c_int) {
    if oldfd == newfd || newfd < 0 || newfd as usize >= FD_LIMIT {
        return;
    }
    let mut thread: PthreadT = ptr::null_mut();
    lock();
    let i = lookup(oldfd);
    let join = detach(newfd, &mut thread);
    if !i.is_null() {
        (*i).aliases = (*i).aliases.wrapping_add(1);
        (*i).refs = (*i).refs.wrapping_add(1);
        *slot(newfd) = i;
    }
    unlock();
    if join {
        pthread_join(thread, ptr::null_mut());
    }
}
