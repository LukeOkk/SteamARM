//! Flight recorder for guest address-space changes and file-backed maps.
//!
//! The fixed ring is best-effort and lock-free on writes. File mappings use a
//! signal-safe mutex because lookup can run inside a guest fault handler.

use std::ffi::{c_char, c_int, c_long, c_ulong, c_ulonglong, c_void, CStr, CString};
use std::fs::OpenOptions;
use std::os::unix::fs::OpenOptionsExt;
use std::os::unix::io::AsRawFd;
use std::ptr;
use std::sync::atomic::{AtomicI32, AtomicI64, AtomicU64, AtomicU8, Ordering};
use std::sync::{Mutex, MutexGuard, Once, OnceLock};

mod generated {
    pub mod timer {
        include!(concat!(env!("OUT_DIR"), "/timer_constants.rs"));
    }
    pub mod tls {
        include!(concat!(env!("OUT_DIR"), "/tls_constants.rs"));
    }
    #[allow(dead_code)]
    pub mod pathfd {
        include!(concat!(env!("OUT_DIR"), "/pathfd_constants.rs"));
    }
}

const MEMLOG_N: usize = 16_384;
const HOST_PAGE: u64 = 16_384;
const GUEST_PAGE: u64 = 4_096;
const PATH_MAX: usize = generated::pathfd::D_PATH_MAX as usize;

struct MemlogEntry {
    seq: AtomicU64,
    addr: AtomicU64,
    len: AtomicU64,
    a: AtomicI64,
    b: AtomicI64,
    ret: AtomicI64,
    lr: AtomicU64,
    tid: AtomicI32,
    op: AtomicU8,
}

impl MemlogEntry {
    const fn new() -> Self {
        Self {
            seq: AtomicU64::new(0),
            addr: AtomicU64::new(0),
            len: AtomicU64::new(0),
            a: AtomicI64::new(0),
            b: AtomicI64::new(0),
            ret: AtomicI64::new(0),
            lr: AtomicU64::new(0),
            tid: AtomicI32::new(0),
            op: AtomicU8::new(0),
        }
    }
}

static RING: [MemlogEntry; MEMLOG_N] = [const { MemlogEntry::new() }; MEMLOG_N];
static NEXT: AtomicU64 = AtomicU64::new(0);

#[no_mangle]
pub static mut lxrt_main_image_base: u64 = 0;
#[no_mangle]
pub static mut lxrt_main_image_span: u64 = 0;
#[no_mangle]
pub static mut lxrt_start_stack: u64 = 0;

struct FileMap {
    addr: u64,
    len: u64,
    off: u64,
    path: CString,
}

static FILE_MAPS: Mutex<Vec<FileMap>> = Mutex::new(Vec::new());
static FILE_MAPS_ON: OnceLock<bool> = OnceLock::new();
static FORK_HOOKS: Once = Once::new();
type FileMapGuard = MutexGuard<'static, Vec<FileMap>>;
static mut FORK_GUARD: std::mem::MaybeUninit<FileMapGuard> = std::mem::MaybeUninit::uninit();

#[repr(transparent)]
struct DarwinSigSet(u32);

#[link(name = "System")]
unsafe extern "C" {
    fn pthread_threadid_np(thread: *mut c_void, thread_id: *mut u64) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn sigfillset(set: *mut c_void) -> c_int;
    fn sigdelset(set: *mut c_void, signal: c_int) -> c_int;
    fn pthread_sigmask(how: c_int, set: *const c_void, old: *mut c_void) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn getpid() -> c_int;
    fn dprintf(fd: c_int, format: *const c_char, ...) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn fflush(stream: *mut c_void) -> c_int;
    fn fileno(stream: *mut c_void) -> c_int;
    fn _NSGetArgc() -> *mut c_int;
    fn _NSGetArgv() -> *mut *mut *mut c_char;
    fn lxrt_last_guest_lr() -> u64;
    fn lxrt_trace_stream() -> *mut c_void;
}

fn lock_file_maps() -> FileMapGuard {
    FILE_MAPS
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

extern "C" fn fork_prepare() {
    let guard = lock_file_maps();
    unsafe {
        ptr::addr_of_mut!(FORK_GUARD)
            .cast::<FileMapGuard>()
            .write(guard);
    }
}

extern "C" fn fork_release() {
    unsafe {
        ptr::drop_in_place(ptr::addr_of_mut!(FORK_GUARD).cast::<FileMapGuard>());
    }
}

extern "C" fn register_fork_hooks() {
    FORK_HOOKS.call_once(|| unsafe {
        let _ = pthread_atfork(Some(fork_prepare), Some(fork_release), Some(fork_release));
    });
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FORK_HOOKS_INIT: extern "C" fn() = register_fork_hooks;

fn fmaps_on() -> bool {
    *FILE_MAPS_ON.get_or_init(|| std::env::var_os("LXRT_GUEST_FAULTS").is_some())
}

fn block_async() -> DarwinSigSet {
    let mut all = DarwinSigSet(0);
    let mut old = DarwinSigSet(0);
    unsafe {
        let _ = sigfillset((&mut all as *mut DarwinSigSet).cast());
        let _ = sigdelset(
            (&mut all as *mut DarwinSigSet).cast(),
            generated::tls::D_SIGBUS,
        );
        let _ = sigdelset(
            (&mut all as *mut DarwinSigSet).cast(),
            generated::tls::D_SIGSEGV,
        );
        let _ = sigdelset(
            (&mut all as *mut DarwinSigSet).cast(),
            generated::tls::D_SIGILL,
        );
        let _ = sigdelset(
            (&mut all as *mut DarwinSigSet).cast(),
            generated::tls::D_SIGTRAP,
        );
        let _ = pthread_sigmask(
            generated::timer::D_SIG_BLOCK,
            (&all as *const DarwinSigSet).cast(),
            (&mut old as *mut DarwinSigSet).cast(),
        );
    }
    old
}

fn restore_signals(old: &DarwinSigSet) {
    unsafe {
        let _ = pthread_sigmask(
            generated::timer::D_SIG_SETMASK,
            (old as *const DarwinSigSet).cast(),
            ptr::null_mut(),
        );
    }
}

fn lock_nosig() -> (FileMapGuard, DarwinSigSet) {
    let old = block_async();
    (lock_file_maps(), old)
}

fn unlock_nosig(guard: FileMapGuard, old: &DarwinSigSet) {
    drop(guard);
    restore_signals(old);
}

fn drop_inside(entries: &mut Vec<FileMap>, low: u64, high: u64) {
    entries.retain(|entry| !(entry.addr >= low && entry.addr.wrapping_add(entry.len) <= high));
}

/// Record one mmap-like address-space operation in the fixed ring.
#[no_mangle]
pub extern "C" fn lxrt_memlog(op: c_char, addr: u64, len: u64, a: c_long, b: c_long, ret: c_long) {
    let sequence = NEXT.fetch_add(1, Ordering::Relaxed);
    let entry = &RING[sequence as usize % MEMLOG_N];
    entry.seq.store(sequence.wrapping_add(1), Ordering::Relaxed);
    entry.addr.store(addr, Ordering::Relaxed);
    entry.len.store(len, Ordering::Relaxed);
    entry.a.store(a as i64, Ordering::Relaxed);
    entry.b.store(b as i64, Ordering::Relaxed);
    entry.ret.store(ret as i64, Ordering::Relaxed);
    entry
        .lr
        .store(unsafe { lxrt_last_guest_lr() }, Ordering::Relaxed);
    let mut tid = 0;
    unsafe {
        let _ = pthread_threadid_np(ptr::null_mut(), &mut tid);
    }
    entry.tid.store(tid as i32, Ordering::Relaxed);
    entry.op.store(op as u8, Ordering::Relaxed);
}

/// Print the ring entries that overlap the host page containing a fault.
#[no_mangle]
pub unsafe extern "C" fn lxrt_memlog_dump_fd(fd: c_int, fault_addr: u64, why: *const c_char) {
    if why.is_null() {
        return;
    }
    let low = fault_addr & !(HOST_PAGE - 1);
    let high = low.wrapping_add(HOST_PAGE);
    let end = NEXT.load(Ordering::Relaxed);
    let start = if end > MEMLOG_N as u64 {
        end - MEMLOG_N as u64
    } else {
        0
    };
    let _ = dprintf(
        fd,
        b"[lxrt] memlog (pid %d): %s at 0x%llx; ops touching host page 0x%llx, oldest first:\n\0"
            .as_ptr()
            .cast(),
        getpid(),
        why,
        fault_addr as c_ulonglong,
        low as c_ulonglong,
    );
    let mut shown = 0;
    for sequence in start..end {
        let entry = &RING[sequence as usize % MEMLOG_N];
        let seq = entry.seq.load(Ordering::Relaxed);
        let addr = entry.addr.load(Ordering::Relaxed);
        let len = entry.len.load(Ordering::Relaxed);
        if seq != sequence.wrapping_add(1) || addr >= high || addr.wrapping_add(len) <= low {
            continue;
        }
        let op = entry.op.load(Ordering::Relaxed) as c_int;
        let _ = dprintf(
            fd,
            b"[lxrt]   #%llu tid %d %c 0x%llx+0x%llx a=0x%lx b=0x%lx -> %ld (lr 0x%llx)\n\0"
                .as_ptr()
                .cast(),
            seq as c_ulonglong,
            entry.tid.load(Ordering::Relaxed),
            op,
            addr as c_ulonglong,
            len as c_ulonglong,
            entry.a.load(Ordering::Relaxed) as u64 as c_ulong,
            entry.b.load(Ordering::Relaxed) as u64 as c_ulong,
            entry.ret.load(Ordering::Relaxed) as c_long,
            entry.lr.load(Ordering::Relaxed) as c_ulonglong,
        );
        shown += 1;
    }
    let _ = dprintf(
        fd,
        b"[lxrt] memlog: %d of the last %llu ops\n\0"
            .as_ptr()
            .cast(),
        shown,
        end - start,
    );
}

/// Dump the ring to the runtime trace stream.
#[no_mangle]
pub unsafe extern "C" fn lxrt_memlog_dump(fault_addr: u64, why: *const c_char) {
    let stream = lxrt_trace_stream();
    let _ = fflush(stream);
    lxrt_memlog_dump_fd(fileno(stream), fault_addr, why);
}

/// Record the file path and offset behind a guest file mapping.
#[no_mangle]
pub unsafe extern "C" fn lxrt_memlog_file(addr: u64, len: u64, off: u64, fd: c_int) {
    if !fmaps_on() || fd < 0 || len == 0 {
        return;
    }
    let mut path = vec![0 as c_char; PATH_MAX];
    if fcntl(fd, generated::pathfd::D_F_GETPATH, path.as_mut_ptr()) != 0 {
        return;
    }
    let path = CStr::from_ptr(path.as_ptr()).to_owned();
    register_fork_hooks();

    let rounded = len.wrapping_add(GUEST_PAGE - 1) & !(GUEST_PAGE - 1);
    let (mut entries, old) = lock_nosig();
    drop_inside(&mut entries, addr, addr.wrapping_add(rounded));
    if entries.len() == entries.capacity() {
        let additional = entries.capacity().max(1024);
        if entries.try_reserve_exact(additional).is_err() {
            unlock_nosig(entries, &old);
            return;
        }
    }
    entries.push(FileMap {
        addr,
        len,
        off,
        path,
    });
    unlock_nosig(entries, &old);
}

/// Forget mappings wholly covered by a guest munmap range.
#[no_mangle]
pub extern "C" fn lxrt_memlog_file_forget(addr: u64, len: u64) {
    if !fmaps_on() || len == 0 {
        return;
    }
    register_fork_hooks();
    let (mut entries, old) = lock_nosig();
    drop_inside(&mut entries, addr, addr.wrapping_add(len));
    unlock_nosig(entries, &old);
}

/// Find a file mapping without waiting; used from the runtime fault handler.
#[no_mangle]
pub unsafe extern "C" fn lxrt_memlog_file_lookup(
    addr: u64,
    path: *mut c_char,
    n: usize,
    off: *mut u64,
) -> bool {
    let old = block_async();
    let entries = match FILE_MAPS.try_lock() {
        Ok(entries) => entries,
        Err(std::sync::TryLockError::Poisoned(error)) => error.into_inner(),
        Err(std::sync::TryLockError::WouldBlock) => {
            restore_signals(&old);
            return false;
        }
    };
    let found = entries
        .iter()
        .rev()
        .find(|entry| addr >= entry.addr && addr < entry.addr.wrapping_add(entry.len));
    let Some(entry) = found else {
        drop(entries);
        restore_signals(&old);
        return false;
    };
    if !off.is_null() {
        *off = entry.off.wrapping_add(addr - entry.addr);
    }
    if !path.is_null() && n > 0 {
        let bytes = entry.path.as_bytes();
        let copied = bytes.len().min(n - 1);
        ptr::copy_nonoverlapping(bytes.as_ptr(), path.cast::<u8>(), copied);
        *path.add(copied) = 0;
    }
    drop(entries);
    restore_signals(&old);
    true
}

/// Log one already-formatted fault-repair message to the trace and fault log.
#[no_mangle]
pub unsafe extern "C" fn lxrt_fault_note(line: *const c_char) {
    if line.is_null() {
        return;
    }
    let _ = fprintf(lxrt_trace_stream(), b"[lxrt] %s\n\0".as_ptr().cast(), line);
    let file = OpenOptions::new()
        .write(true)
        .append(true)
        .create(true)
        .mode(0o600)
        .open("/tmp/lxrt-faults.log");
    let Ok(file) = file else {
        return;
    };
    let argc = _NSGetArgc();
    let argv = _NSGetArgv();
    let program_ptr = if !argc.is_null() && *argc > 1 && !argv.is_null() && !(*argv).is_null() {
        *(*argv).add(1) as *const c_char
    } else {
        b"?\0".as_ptr().cast()
    };
    let _ = dprintf(
        file.as_raw_fd(),
        b"[lxrt] pid %d %s: %s\n\0".as_ptr().cast(),
        getpid(),
        program_ptr,
        line,
    );
}
