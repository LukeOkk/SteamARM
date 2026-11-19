// Linux guest process/thread IDs backed by a process-shared Darwin PID table.
// Keep IdEntry and IdTable layout identical to the former C implementation:
// they are mapped by every lxrun process using this compatibility layer.

use std::env;
use std::ffi::{c_char, c_int, c_void};
use std::fs::{self, OpenOptions};
use std::io::ErrorKind;
use std::mem::{size_of, MaybeUninit};
use std::os::unix::ffi::OsStrExt;
use std::os::unix::fs::{MetadataExt, OpenOptionsExt};
use std::os::unix::io::AsRawFd;
use std::ptr;
use std::sync::atomic::{AtomicI32, AtomicPtr, AtomicU32, Ordering};
use std::time::Duration;

include!(concat!(env!("OUT_DIR"), "/ids_constants.rs"));

const IDS_COUNT: usize = 65_536;
const IDS_FIRST: i32 = 2;
const IDS_SLOTS: u32 = (IDS_COUNT as u32) - IDS_FIRST as u32;
const IDS_FILE_PREFIX: &str = "/tmp/lxrt-ids-";

#[repr(C)]
struct IdEntry {
    pid: AtomicI32,
    thread: AtomicI32,
    generation: AtomicU32,
}

#[repr(C)]
struct IdTable {
    cursor: AtomicU32,
    entries: [IdEntry; IDS_COUNT],
}

const _: [(); 12] = [(); size_of::<IdEntry>()];
const _: [(); 4 + IDS_COUNT * 12] = [(); size_of::<IdTable>()];

#[repr(C)]
struct ProcBsdInfo {
    pbi_flags: u32,
    pbi_status: u32,
    pbi_xstatus: u32,
    pbi_pid: u32,
    pbi_ppid: u32,
    pbi_uid: u32,
    pbi_gid: u32,
    pbi_ruid: u32,
    pbi_rgid: u32,
    pbi_svuid: u32,
    pbi_svgid: u32,
    rfu_1: u32,
    pbi_comm: [c_char; D_MAXCOMLEN as usize],
    pbi_name: [c_char; (D_MAXCOMLEN * 2) as usize],
    pbi_nfiles: u32,
    pbi_pgid: u32,
    pbi_pjobc: u32,
    e_tdev: u32,
    e_tpgid: u32,
    pbi_nice: i32,
    pbi_start_tvsec: u64,
    pbi_start_tvusec: u64,
}

#[link(name = "System")]
unsafe extern "C" {
    fn getuid() -> u32;
    fn kill(pid: c_int, signal: c_int) -> c_int;
    fn proc_pidinfo(pid: c_int, flavor: c_int, arg: u64, buffer: *mut c_void, size: c_int)
        -> c_int;
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn atexit(callback: extern "C" fn()) -> c_int;
}

static IDS: AtomicPtr<IdTable> = AtomicPtr::new(ptr::null_mut());
static STATE: AtomicI32 = AtomicI32::new(0); // 0 uninitialized, 1 ready, -1 failed, 2 initializing
static GUEST_PID: AtomicI32 = AtomicI32::new(0);
static CHILDREN: [AtomicI32; IDS_COUNT] = [const { AtomicI32::new(0) }; IDS_COUNT];
static IDS_ENABLED: AtomicI32 = AtomicI32::new(-1);

fn table() -> Option<&'static IdTable> {
    let pointer = IDS.load(Ordering::Acquire);
    if pointer.is_null() {
        None
    } else {
        // The MAP_SHARED mapping lives for the process lifetime, as it did in C.
        Some(unsafe { &*pointer })
    }
}

fn birth(host_pid: i32) -> u32 {
    if host_pid <= 0 {
        return 0;
    }
    let mut info = MaybeUninit::<ProcBsdInfo>::zeroed();
    let written = unsafe {
        proc_pidinfo(
            host_pid,
            D_PROC_PIDTBSDINFO,
            0,
            info.as_mut_ptr().cast(),
            size_of::<ProcBsdInfo>() as c_int,
        )
    };
    if written != size_of::<ProcBsdInfo>() as c_int {
        return 0;
    }
    let info = unsafe { info.assume_init() };
    let timestamp = info
        .pbi_start_tvsec
        .wrapping_mul(1_000_000)
        .wrapping_add(info.pbi_start_tvusec);
    let generation = (timestamp ^ (timestamp >> 32)) as u32;
    if generation == 0 {
        1
    } else {
        generation
    }
}

fn enabled() -> bool {
    let cached = IDS_ENABLED.load(Ordering::Relaxed);
    if cached >= 0 {
        return cached != 0;
    }
    let on =
        env::var_os("LXRT_SMALL_IDS").is_some_and(|value| value.as_os_str().as_bytes() == b"1");
    IDS_ENABLED.store(i32::from(on), Ordering::Relaxed);
    on
}

fn make_options() -> OpenOptions {
    let mut options = OpenOptions::new();
    options.read(true).write(true).custom_flags(D_O_NOFOLLOW);
    options
}

fn open_table() -> bool {
    let path = format!("{IDS_FILE_PREFIX}{}", unsafe { getuid() });
    let created = match make_options().create_new(true).mode(0o600).open(&path) {
        Ok(file) => Some(file),
        Err(error) if error.kind() == ErrorKind::AlreadyExists => None,
        Err(_) => return false,
    };
    let file = match created {
        Some(file) => {
            if file.set_len(size_of::<IdTable>() as u64).is_err() {
                drop(file);
                let _ = fs::remove_file(&path);
                return false;
            }
            file
        }
        None => match make_options().open(&path) {
            Ok(file) => file,
            Err(_) => return false,
        },
    };

    let expected_size = size_of::<IdTable>() as u64;
    let uid = unsafe { getuid() } as u64;
    let mut valid = false;
    for _ in 0..100 {
        if let Ok(metadata) = file.metadata() {
            if metadata.file_type().is_file()
                && metadata.uid() as u64 == uid
                && metadata.mode() & 0o077 == 0
                && metadata.len() == expected_size
            {
                valid = true;
                break;
            }
        }
        std::thread::sleep(Duration::from_millis(1));
    }
    if !valid {
        return false;
    }

    let pointer = unsafe {
        mmap(
            ptr::null_mut(),
            size_of::<IdTable>(),
            D_PROT_READ | D_PROT_WRITE,
            D_MAP_SHARED,
            file.as_raw_fd(),
            0,
        )
    };
    if pointer as isize == -1 {
        return false;
    }
    IDS.store(pointer.cast(), Ordering::Release);
    true
}

fn claim(host_pid: i32, thread_index: i32) -> i32 {
    let Some(ids) = table() else {
        return -1;
    };
    let generation = if host_pid > 0 { birth(host_pid) } else { 0 };
    if host_pid > 0 && generation == 0 {
        return -1;
    }
    let start = ids.cursor.fetch_add(1, Ordering::SeqCst);
    for pass in 0..2 {
        for offset in 0..IDS_SLOTS {
            let id = IDS_FIRST + (start.wrapping_add(offset) % IDS_SLOTS) as i32;
            let entry = &ids.entries[id as usize];
            let mut old = entry.pid.load(Ordering::Acquire);
            if old != 0 && pass != 0 && old > 0 {
                let stale = birth(old) != entry.generation.load(Ordering::SeqCst)
                    || (unsafe { kill(old, 0) } < 0
                        && std::io::Error::last_os_error().raw_os_error() == Some(D_ESRCH));
                if stale {
                    let _ = entry
                        .pid
                        .compare_exchange(old, 0, Ordering::SeqCst, Ordering::SeqCst);
                    old = entry.pid.load(Ordering::SeqCst);
                }
            }
            if old != 0 {
                continue;
            }
            if entry
                .pid
                .compare_exchange(0, -1, Ordering::SeqCst, Ordering::SeqCst)
                .is_err()
            {
                continue;
            }
            entry.thread.store(thread_index, Ordering::SeqCst);
            entry.generation.store(generation, Ordering::SeqCst);
            entry.pid.store(host_pid, Ordering::Release);
            return id;
        }
    }
    -1
}

extern "C" fn ids_atexit() {
    lxrt_ids_release_process();
}

#[no_mangle]
pub extern "C" fn lxrt_ids_on() -> bool {
    enabled()
}

#[no_mangle]
pub extern "C" fn lxrt_ids_init() -> c_int {
    if !enabled() {
        return 0;
    }
    loop {
        match STATE.load(Ordering::Acquire) {
            1 => return GUEST_PID.load(Ordering::SeqCst),
            -1 => return -1,
            2 => std::thread::yield_now(),
            _ => {
                if STATE
                    .compare_exchange(0, 2, Ordering::SeqCst, Ordering::SeqCst)
                    .is_err()
                {
                    continue;
                }
                break;
            }
        }
    }

    let result = (|| {
        if !open_table() {
            return -1;
        }
        let host_pid = std::process::id() as i32;
        let generation = birth(host_pid);
        if generation == 0 {
            return -1;
        }
        let Some(ids) = table() else {
            return -1;
        };
        let mut guest_pid = 0;
        for id in IDS_FIRST..IDS_COUNT as i32 {
            let entry = &ids.entries[id as usize];
            if entry.pid.load(Ordering::Acquire) == host_pid
                && entry.thread.load(Ordering::SeqCst) == 0
                && entry.generation.load(Ordering::SeqCst) == generation
            {
                guest_pid = id;
                break;
            }
        }
        if guest_pid == 0 {
            guest_pid = claim(host_pid, 0);
        }
        if guest_pid < IDS_FIRST {
            return -1;
        }
        GUEST_PID.store(guest_pid, Ordering::SeqCst);
        unsafe {
            atexit(ids_atexit);
        }
        guest_pid
    })();
    STATE.store(if result >= IDS_FIRST { 1 } else { -1 }, Ordering::Release);
    result
}

#[no_mangle]
pub extern "C" fn lxrt_ids_pid() -> c_int {
    if !enabled() {
        return std::process::id() as i32;
    }
    let guest_pid = GUEST_PID.load(Ordering::SeqCst);
    if guest_pid > 1 {
        guest_pid
    } else {
        lxrt_ids_init()
    }
}

#[no_mangle]
pub extern "C" fn lxrt_ids_alloc_thread(index: c_int) -> c_int {
    if enabled() {
        claim(std::process::id() as i32, index)
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn lxrt_ids_reserve_child() -> c_int {
    if enabled() {
        claim(-1, 0)
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn lxrt_ids_cancel_child(id: c_int) {
    if !enabled() || id < IDS_FIRST || id >= IDS_COUNT as i32 {
        return;
    }
    let Some(ids) = table() else {
        return;
    };
    let _ =
        ids.entries[id as usize]
            .pid
            .compare_exchange(-1, 0, Ordering::SeqCst, Ordering::SeqCst);
}

#[no_mangle]
pub extern "C" fn lxrt_ids_publish_child(id: c_int, host_pid: c_int) {
    if !enabled() || id < IDS_FIRST || id >= IDS_COUNT as i32 {
        return;
    }
    let Some(ids) = table() else {
        return;
    };
    let entry = &ids.entries[id as usize];
    entry.generation.store(birth(host_pid), Ordering::SeqCst);
    entry.pid.store(host_pid, Ordering::Release);
}

#[no_mangle]
pub extern "C" fn lxrt_ids_child_after_fork(id: c_int) {
    if !enabled() {
        return;
    }
    GUEST_PID.store(id, Ordering::SeqCst);
    for child in &CHILDREN {
        child.store(0, Ordering::SeqCst);
    }
    lxrt_ids_publish_child(id, std::process::id() as i32);
}

#[no_mangle]
pub extern "C" fn lxrt_ids_release(id: c_int) {
    if !enabled() || id < IDS_FIRST || id >= IDS_COUNT as i32 {
        return;
    }
    let Some(ids) = table() else {
        return;
    };
    let _ = ids.entries[id as usize].pid.compare_exchange(
        std::process::id() as i32,
        0,
        Ordering::SeqCst,
        Ordering::SeqCst,
    );
}

#[no_mangle]
pub extern "C" fn lxrt_ids_release_process() {
    if !enabled() {
        return;
    }
    let Some(ids) = table() else {
        return;
    };
    let host_pid = std::process::id() as i32;
    for id in IDS_FIRST..IDS_COUNT as i32 {
        let _ = ids.entries[id as usize].pid.compare_exchange(
            host_pid,
            0,
            Ordering::SeqCst,
            Ordering::SeqCst,
        );
    }
}

#[no_mangle]
pub extern "C" fn lxrt_ids_to_guest(host_pid: c_int, index: c_int) -> c_int {
    if !enabled() || host_pid <= 0 {
        return host_pid;
    }
    if host_pid == std::process::id() as i32 && index == 0 {
        return lxrt_ids_pid();
    }
    let Some(ids) = table() else {
        return 0;
    };
    let generation = birth(host_pid);
    if generation == 0 {
        return 0;
    }
    for id in IDS_FIRST..IDS_COUNT as i32 {
        let entry = &ids.entries[id as usize];
        if entry.pid.load(Ordering::Acquire) == host_pid
            && entry.thread.load(Ordering::SeqCst) == index
            && entry.generation.load(Ordering::SeqCst) == generation
        {
            return id;
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn lxrt_ids_threads(host_pid: c_int, out: *mut c_int, max: c_int) -> c_int {
    if !enabled() {
        return 0;
    }
    let Some(ids) = table() else {
        return 0;
    };
    let generation = birth(host_pid);
    if generation == 0 {
        return 0;
    }
    let mut count = 0;
    for id in IDS_FIRST..IDS_COUNT as i32 {
        let entry = &ids.entries[id as usize];
        if entry.pid.load(Ordering::Acquire) == host_pid
            && entry.generation.load(Ordering::SeqCst) == generation
        {
            if count < max && !out.is_null() {
                unsafe {
                    out.add(count as usize).write(id);
                }
            }
            count += 1;
        }
    }
    count
}

#[no_mangle]
pub extern "C" fn lxrt_ids_to_host(
    id: c_int,
    host_pid_out: *mut c_int,
    index_out: *mut c_int,
) -> bool {
    if !enabled() {
        if !host_pid_out.is_null() {
            unsafe { host_pid_out.write(id) };
        }
        if !index_out.is_null() {
            unsafe { index_out.write(0) };
        }
        return id > 0;
    }
    if id < IDS_FIRST || id >= IDS_COUNT as i32 {
        return false;
    }
    let Some(ids) = table() else {
        return false;
    };
    let entry = &ids.entries[id as usize];
    let host_pid = entry.pid.load(Ordering::Acquire);
    if host_pid <= 0 || entry.generation.load(Ordering::SeqCst) != birth(host_pid) {
        return false;
    }
    if !host_pid_out.is_null() {
        unsafe { host_pid_out.write(host_pid) };
    }
    if !index_out.is_null() {
        unsafe { index_out.write(entry.thread.load(Ordering::SeqCst)) };
    }
    true
}

#[no_mangle]
pub extern "C" fn lxrt_ids_target_pid(guest_pid: c_int) -> c_int {
    if !enabled() || guest_pid <= 0 {
        return guest_pid;
    }
    if (IDS_FIRST..IDS_COUNT as i32).contains(&guest_pid) {
        let child = CHILDREN[guest_pid as usize].load(Ordering::SeqCst);
        if child > 0 {
            return child;
        }
    }
    let mut host_pid = 0;
    let mut index = 0;
    if lxrt_ids_to_host(guest_pid, &mut host_pid, &mut index) && index == 0 {
        host_pid
    } else {
        -1
    }
}

#[no_mangle]
pub extern "C" fn lxrt_ids_note_child(host_pid: c_int, guest_pid: c_int) {
    if !enabled() || guest_pid < IDS_FIRST || guest_pid >= IDS_COUNT as i32 {
        return;
    }
    CHILDREN[guest_pid as usize].store(host_pid, Ordering::SeqCst);
}

#[no_mangle]
pub extern "C" fn lxrt_ids_reaped_child(host_pid: c_int, reap: bool) -> c_int {
    if !enabled() {
        return host_pid;
    }
    for (id, child) in CHILDREN.iter().enumerate().skip(IDS_FIRST as usize) {
        if child.load(Ordering::SeqCst) == host_pid {
            if reap {
                child.store(0, Ordering::SeqCst);
            }
            return id as i32;
        }
    }
    lxrt_ids_to_guest(host_pid, 0)
}
