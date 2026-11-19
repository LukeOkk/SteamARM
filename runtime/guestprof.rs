// LXRT_GUESTPROF=1: sample where the first guest thread spends its time.

use std::ffi::{c_char, c_int, c_uint, c_void, CStr};
use std::mem::{size_of, MaybeUninit};
use std::os::unix::ffi::OsStrExt;
use std::sync::atomic::{AtomicU32, AtomicU64, AtomicUsize, Ordering};
use std::sync::{Mutex, MutexGuard};
use std::thread;

include!(concat!(env!("OUT_DIR"), "/guestprof_constants.rs"));

const KEY_COUNT: usize = 192;
const KEY_NAME_BYTES: usize = 72;
const REPORT_BYTES: usize = 1600;
const D_CLOCK_UPTIME_RAW: c_int = 8;
const ARM_THREAD_STATE64_COUNT: c_uint =
    (size_of::<ArmThreadState64>() / size_of::<u32>()) as c_uint;

#[repr(C)]
struct ArmThreadState64 {
    x: [u64; 29],
    fp: u64,
    lr: u64,
    sp: u64,
    pc: u64,
    cpsr: u32,
    pad: u32,
}

const _: [(); 272] = [(); size_of::<ArmThreadState64>()];
const _: [(); 68] = [(); ARM_THREAD_STATE64_COUNT as usize];

#[repr(C)]
struct ProcRegionInfo {
    protection: u32,
    max_protection: u32,
    inheritance: u32,
    flags: u32,
    offset: u64,
    behavior: u32,
    user_wired_count: u32,
    user_tag: u32,
    pages_resident: u32,
    pages_shared_now_private: u32,
    pages_swapped_out: u32,
    pages_dirtied: u32,
    ref_count: u32,
    shadow_depth: u32,
    share_mode: u32,
    private_pages_resident: u32,
    shared_pages_resident: u32,
    object_id: u32,
    depth: u32,
    address: u64,
    size: u64,
}

#[repr(C)]
struct ProcRegionWithPathInfo {
    region: ProcRegionInfo,
    vnode_info: [u64; 19],
    path: [c_char; D_MAXPATHLEN as usize],
}

const _: [(); 96] = [(); size_of::<ProcRegionInfo>()];
const _: [(); 96 + 152 + D_MAXPATHLEN as usize] = [(); size_of::<ProcRegionWithPathInfo>()];

#[repr(C)]
struct DlInfo {
    file_name: *const c_char,
    file_base: *mut c_void,
    symbol_name: *const c_char,
    symbol_address: *mut c_void,
}

#[derive(Clone, Copy)]
struct SampleKey {
    name: [u8; KEY_NAME_BYTES],
    name_len: usize,
    samples: u32,
}

impl SampleKey {
    const EMPTY: Self = Self {
        name: [0; KEY_NAME_BYTES],
        name_len: 0,
        samples: 0,
    };
}

struct ProfilerState {
    keys: [SampleKey; KEY_COUNT],
    key_count: usize,
    total: u32,
}

impl ProfilerState {
    const fn new() -> Self {
        Self {
            keys: [SampleKey::EMPTY; KEY_COUNT],
            key_count: 0,
            total: 0,
        }
    }
}

static PROFILER: Mutex<ProfilerState> = Mutex::new(ProfilerState::new());
static TARGET_THREAD: AtomicUsize = AtomicUsize::new(0);
static TARGET_PORT: AtomicU32 = AtomicU32::new(0);
static CPU_STATE: AtomicU64 = AtomicU64::new(0);

#[no_mangle]
pub static mut lxrt_guestprof_on: c_int = 0;

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: u32;

    fn pthread_self() -> *mut c_void;
    fn pthread_equal(left: *mut c_void, right: *mut c_void) -> c_int;
    fn pthread_mach_thread_np(thread: *mut c_void) -> u32;
    fn pthread_setname_np(name: *const c_char) -> c_int;
    fn usleep(microseconds: c_uint) -> c_int;
    fn clock_gettime_nsec_np(clock: c_int) -> u64;
    fn getpid() -> c_int;
    fn dladdr(address: *const c_void, info: *mut DlInfo) -> c_int;
    fn proc_pidinfo(
        pid: c_int,
        flavor: c_int,
        arg: u64,
        buffer: *mut c_void,
        buffer_size: c_int,
    ) -> c_int;
    fn thread_suspend(thread: u32) -> c_int;
    fn thread_get_state(
        thread: u32,
        flavor: c_int,
        state: *mut u32,
        state_count: *mut c_uint,
    ) -> c_int;
    fn thread_resume(thread: u32) -> c_int;
    fn mach_vm_read_overwrite(
        task: u32,
        address: u64,
        size: u64,
        data: u64,
        out_size: *mut u64,
    ) -> c_int;
    fn lxrt_trace_stream() -> *mut c_void;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
}

fn lock_profiler() -> MutexGuard<'static, ProfilerState> {
    PROFILER
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

fn count(name: &[u8]) {
    let mut state = lock_profiler();
    state.total = state.total.wrapping_add(1);
    let key_count = state.key_count;
    for key in &mut state.keys[..key_count] {
        if key.name_len == name.len() && key.name[..key.name_len] == *name {
            key.samples = key.samples.wrapping_add(1);
            return;
        }
    }
    if key_count < KEY_COUNT {
        let key = &mut state.keys[key_count];
        let length = name.len().min(KEY_NAME_BYTES - 1);
        key.name[..length].copy_from_slice(&name[..length]);
        key.name[length] = 0;
        key.name_len = length;
        key.samples = 1;
        state.key_count += 1;
    }
}

fn report() {
    let mut state = lock_profiler();
    let total = state.total;
    let mut line = format!(
        "[lxrt] guestprof pid {}, {} samples:",
        unsafe { getpid() },
        total
    )
    .into_bytes();
    for _ in 0..16 {
        if line.len() >= REPORT_BYTES - 96 || state.key_count == 0 {
            break;
        }
        let mut best = 0;
        for index in 1..state.key_count {
            if state.keys[index].samples > state.keys[best].samples {
                best = index;
            }
        }
        if state.keys[best].samples == 0 {
            break;
        }
        let key = &mut state.keys[best];
        line.push(b' ');
        line.extend_from_slice(&key.name[..key.name_len]);
        line.push(b'=');
        let percent = format!("{:.1}%", 100.0 * key.samples as f64 / total.max(1) as f64);
        line.extend_from_slice(percent.as_bytes());
        key.samples = 0;
    }
    line.truncate(REPORT_BYTES - 1);
    line.push(0);
    unsafe {
        let _ = fprintf(
            lxrt_trace_stream(),
            b"%s\n\0".as_ptr().cast(),
            line.as_ptr().cast::<c_char>(),
        );
    }
    state.key_count = 0;
    state.total = 0;
}

fn file_at(address: u64, path: &mut [u8]) -> bool {
    if path.is_empty() {
        return false;
    }
    path[0] = 0;
    let mut info = unsafe { MaybeUninit::<ProcRegionWithPathInfo>::zeroed().assume_init() };
    let result = unsafe {
        proc_pidinfo(
            getpid(),
            D_PROC_PIDREGIONPATHINFO,
            address,
            (&mut info as *mut ProcRegionWithPathInfo).cast(),
            size_of::<ProcRegionWithPathInfo>() as c_int,
        )
    };
    if result != size_of::<ProcRegionWithPathInfo>() as c_int
        || address < info.region.address
        || address.wrapping_sub(info.region.address) >= info.region.size
        || info.path[0] == 0
    {
        return false;
    }
    let bytes = unsafe { CStr::from_ptr(info.path.as_ptr()) }.to_bytes();
    let length = bytes.len().min(path.len() - 1);
    path[..length].copy_from_slice(&bytes[..length]);
    path[length] = 0;
    true
}

fn basename(path: &[u8]) -> &[u8] {
    path.iter()
        .rposition(|byte| *byte == b'/')
        .map(|index| &path[index + 1..])
        .unwrap_or(path)
}

fn prefixed(prefix: &[u8], value: &[u8]) -> Vec<u8> {
    let mut name = Vec::with_capacity(prefix.len() + value.len());
    name.extend_from_slice(prefix);
    name.extend_from_slice(value);
    name
}

fn sampler() {
    unsafe {
        let _ = pthread_setname_np(b"lxrt-guestprof\0".as_ptr().cast());
    }
    let mut last = unsafe { clock_gettime_nsec_np(D_CLOCK_UPTIME_RAW) };
    loop {
        unsafe {
            let _ = usleep(1000);
        }
        let target_port = TARGET_PORT.load(Ordering::Relaxed);
        if unsafe { thread_suspend(target_port) } != 0 {
            return;
        }
        let mut thread_state = MaybeUninit::<ArmThreadState64>::uninit();
        let mut state_count = ARM_THREAD_STATE64_COUNT;
        let result = unsafe {
            thread_get_state(
                target_port,
                D_ARM_THREAD_STATE64,
                thread_state.as_mut_ptr().cast::<u32>(),
                &mut state_count,
            )
        };
        unsafe {
            let _ = thread_resume(target_port);
        }
        if result != 0 {
            return;
        }
        let pc = unsafe { thread_state.assume_init().pc };
        let mut info = MaybeUninit::<DlInfo>::zeroed();
        let mut name = None;
        if unsafe { dladdr(pc as usize as *const c_void, info.as_mut_ptr()) } != 0 {
            let info = unsafe { info.assume_init() };
            if !info.file_name.is_null() {
                let image = unsafe { CStr::from_ptr(info.file_name) }.to_bytes();
                let image_base = basename(image);
                if image_base == b"libsystem_kernel.dylib" && !info.symbol_name.is_null() {
                    let symbol = unsafe { CStr::from_ptr(info.symbol_name) }.to_bytes();
                    name = Some(prefixed(b"host:", symbol));
                } else {
                    name = Some(prefixed(b"host:", image_base));
                }
            }
        }

        let mut path = [0u8; (D_MAXPATHLEN as usize) * 4];
        if let Some(host_name) = name {
            count(&host_name);
        } else if file_at(pc, &mut path) {
            count(&prefixed(
                b"guest:",
                basename(&path[..path.iter().position(|v| *v == 0).unwrap_or(path.len())]),
            ));
        } else {
            let cpu_state = CPU_STATE.load(Ordering::Relaxed);
            let mut rip = 0u64;
            let mut got = 0u64;
            let translated = if cpu_state != 0
                && unsafe {
                    mach_vm_read_overwrite(
                        mach_task_self_,
                        cpu_state.wrapping_add(24),
                        8,
                        (&mut rip as *mut u64) as u64,
                        &mut got,
                    ) == 0
                }
                && rip != 0
                && file_at(rip, &mut path)
            {
                let end = path
                    .iter()
                    .position(|value| *value == 0)
                    .unwrap_or(path.len());
                Some(prefixed(b"x86:", basename(&path[..end])))
            } else {
                None
            };
            if let Some(translated_name) = translated {
                count(&translated_name);
            } else if cpu_state != 0 {
                count(b"x86:(anonymous)");
            } else {
                count(b"jit");
            }
        }

        let now = unsafe { clock_gettime_nsec_np(D_CLOCK_UPTIME_RAW) };
        if now.wrapping_sub(last) > 10_000_000_000 {
            report();
            last = now;
        }
    }
}

#[no_mangle]
pub extern "C" fn lxrt_guestprof_start() {
    let enabled = std::env::var_os("LXRT_GUESTPROF")
        .map(|value| value.as_os_str().as_bytes().first() == Some(&b'1'))
        .unwrap_or(false);
    if !enabled {
        return;
    }
    let target = unsafe { pthread_self() };
    TARGET_THREAD.store(target as usize, Ordering::Relaxed);
    TARGET_PORT.store(unsafe { pthread_mach_thread_np(target) }, Ordering::Relaxed);
    if let Ok(worker) = thread::Builder::new()
        .name("lxrt-guestprof".to_owned())
        .spawn(sampler)
    {
        drop(worker);
        unsafe { lxrt_guestprof_on = 1 };
    }
}

#[no_mangle]
pub extern "C" fn lxrt_guestprof_note(x28: u64) {
    let target = TARGET_THREAD.load(Ordering::Relaxed) as *mut c_void;
    if !target.is_null()
        && unsafe { pthread_equal(pthread_self(), target) } != 0
        && x28 > (1u64 << 32)
    {
        CPU_STATE.store(x28, Ordering::Relaxed);
    }
}
