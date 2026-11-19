// Guest thread-local storage in Darwin TSD slots.

use std::ffi::{c_char, c_int, c_ulong, c_ulonglong, c_void};
use std::hint::spin_loop;
use std::mem::size_of;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicI32, AtomicU64, AtomicUsize, Ordering};

#[allow(dead_code)]
mod generated {
    include!(concat!(env!("OUT_DIR"), "/tls_constants.rs"));
}

use generated::{D_SIGBUS, D_SIGSEGV};

const TLSKEEP_MAX: usize = 32;

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

struct KeepRange {
    start: AtomicU64,
    end: AtomicU64,
}

static TLS_KEY: AtomicI32 = AtomicI32::new(0);
static TLS_SLOT_OFFSET: AtomicUsize = AtomicUsize::new(0);
static TLS_READY: AtomicBool = AtomicBool::new(false);
static X18_KEY: AtomicI32 = AtomicI32::new(0);
static X18_SLOT_OFFSET: AtomicUsize = AtomicUsize::new(0);
static X18_READY: AtomicBool = AtomicBool::new(false);
static TLS_INIT_LOCK: AtomicBool = AtomicBool::new(false);
static KEEP_ENABLED: AtomicI32 = AtomicI32::new(-1);
static KEEP_COUNT: AtomicI32 = AtomicI32::new(0);
static KEEP_RANGES: [KeepRange; TLSKEEP_MAX] = [const {
    KeepRange {
        start: AtomicU64::new(0),
        end: AtomicU64::new(0),
    }
}; TLSKEEP_MAX];
static KEEP_FIXUPS: AtomicUsize = AtomicUsize::new(0);

#[link(name = "System")]
unsafe extern "C" {
    fn pthread_key_create(key: *mut c_int, destructor: Option<extern "C" fn(*mut c_void)>)
        -> c_int;
    fn pthread_getspecific(key: c_int) -> *mut c_void;
    fn pthread_setspecific(key: c_int, value: *const c_void) -> c_int;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn strcmp(left: *const c_char, right: *const c_char) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn lxrt_no_x18() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
    fn lxrt_arm64_thread_state(ucontext: *mut c_void) -> *mut c_void;
}

fn env_is_set(name: &'static [u8]) -> bool {
    unsafe { !getenv(name.as_ptr().cast()).is_null() }
}

fn init_tls() -> c_int {
    if TLS_READY.load(Ordering::Acquire) {
        return 0;
    }
    while TLS_INIT_LOCK
        .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        spin_loop();
    }
    if TLS_READY.load(Ordering::Acquire) {
        TLS_INIT_LOCK.store(false, Ordering::Release);
        return 0;
    }

    let result = if env_is_set(b"LXRT_NO_TLS_REWRITE\0") {
        unsafe {
            fprintf(
                lxrt_trace_stream(),
                b"[lxrt] LXRT_NO_TLS_REWRITE set: leaving TPIDR_EL0 in place. Guest TLS will break on the first context switch.\n\0"
                    .as_ptr()
                    .cast(),
            );
        }
        0
    } else {
        let mut key = 0;
        if unsafe { pthread_key_create(&mut key, None) } != 0 {
            -1
        } else {
            let slot_offset = key as usize * 8;
            TLS_KEY.store(key, Ordering::Relaxed);
            TLS_SLOT_OFFSET.store(slot_offset, Ordering::Relaxed);
            if slot_offset % 8 != 0 || slot_offset / 8 > 4095 {
                unsafe {
                    fprintf(
                        lxrt_trace_stream(),
                        b"[lxrt] TSD key %lu is out of `ldr` immediate range\n\0"
                            .as_ptr()
                            .cast(),
                        key as c_ulong,
                    );
                }
                -1
            } else {
                TLS_READY.store(true, Ordering::Release);
                if !unsafe { lxrt_no_x18() } {
                    let mut x18_key = 0;
                    if unsafe { pthread_key_create(&mut x18_key, None) } == 0 {
                        let x18_offset = x18_key as usize * 8;
                        X18_KEY.store(x18_key, Ordering::Relaxed);
                        X18_SLOT_OFFSET.store(x18_offset, Ordering::Relaxed);
                        if x18_offset % 8 == 0 && x18_offset / 8 <= 4095 {
                            X18_READY.store(true, Ordering::Release);
                        }
                    }
                }
                if !X18_READY.load(Ordering::Acquire) {
                    unsafe {
                        fprintf(
                            lxrt_trace_stream(),
                            b"[lxrt] x18 virtualisation OFF%s: guest code that keeps values in x18 will lose them on the next context switch\n\0"
                                .as_ptr()
                                .cast(),
                            if lxrt_no_x18() {
                                b" (LXRT_NO_X18)\0".as_ptr().cast::<c_char>()
                            } else {
                                b"\0".as_ptr().cast::<c_char>()
                            },
                        );
                    }
                }
                0
            }
        }
    };
    TLS_INIT_LOCK.store(false, Ordering::Release);
    result
}

#[no_mangle]
pub extern "C" fn lxrt_tls_init() -> c_int {
    init_tls()
}

#[no_mangle]
pub extern "C" fn lxrt_x18_slot_offset() -> c_ulong {
    X18_SLOT_OFFSET.load(Ordering::Acquire) as c_ulong
}

#[no_mangle]
pub extern "C" fn lxrt_x18_enabled() -> bool {
    X18_READY.load(Ordering::Acquire)
}

#[no_mangle]
pub extern "C" fn lxrt_x18_get() -> u64 {
    if !X18_READY.load(Ordering::Acquire) {
        return 0;
    }
    unsafe { pthread_getspecific(X18_KEY.load(Ordering::Relaxed)) as usize as u64 }
}

#[no_mangle]
pub extern "C" fn lxrt_x18_set(value: u64) {
    if X18_READY.load(Ordering::Acquire) {
        unsafe {
            pthread_setspecific(
                X18_KEY.load(Ordering::Relaxed),
                value as usize as *const c_void,
            );
        }
    }
}

#[no_mangle]
pub extern "C" fn lxrt_tls_slot_offset() -> c_ulong {
    TLS_SLOT_OFFSET.load(Ordering::Acquire) as c_ulong
}

#[no_mangle]
pub extern "C" fn lxrt_tls_ready() -> bool {
    TLS_READY.load(Ordering::Acquire)
}

#[no_mangle]
pub extern "C" fn lxrt_tls_set(value: u64) {
    unsafe {
        pthread_setspecific(
            TLS_KEY.load(Ordering::Relaxed),
            value as usize as *const c_void,
        );
    }
}

#[no_mangle]
pub extern "C" fn lxrt_tls_get() -> u64 {
    unsafe { pthread_getspecific(TLS_KEY.load(Ordering::Relaxed)) as usize as u64 }
}

#[no_mangle]
pub extern "C" fn lxrt_tlskeep_enabled() -> bool {
    let mut enabled = KEEP_ENABLED.load(Ordering::Relaxed);
    if enabled < 0 {
        let value = unsafe { getenv(b"LXRT_TLS_KEEP\0".as_ptr().cast()) };
        enabled =
            i32::from(value.is_null() || unsafe { strcmp(value, b"0\0".as_ptr().cast()) != 0 });
        KEEP_ENABLED.store(enabled, Ordering::Relaxed);
    }
    enabled != 0 && TLS_READY.load(Ordering::Acquire)
}

#[no_mangle]
pub extern "C" fn lxrt_tlskeep_add(start: u64, end: u64, why: *const c_char) {
    if !lxrt_tlskeep_enabled() || end <= start {
        return;
    }
    let count = KEEP_COUNT.load(Ordering::SeqCst).max(0) as usize;
    for range in KEEP_RANGES.iter().take(count.min(TLSKEEP_MAX)) {
        if range.start.load(Ordering::SeqCst) == start && range.end.load(Ordering::SeqCst) == end {
            return;
        }
    }
    let index = KEEP_COUNT.fetch_add(1, Ordering::SeqCst);
    if index < 0 || index as usize >= TLSKEEP_MAX {
        KEEP_COUNT.store(TLSKEEP_MAX as i32, Ordering::SeqCst);
        unsafe {
            fprintf(
                lxrt_trace_stream(),
                b"[lxrt] tls keep: table full, %s at 0x%llx is rewritten\n\0"
                    .as_ptr()
                    .cast(),
                if why.is_null() {
                    b"range\0".as_ptr().cast::<c_char>()
                } else {
                    why
                },
                start as c_ulonglong,
            );
        }
        return;
    }
    let range = &KEEP_RANGES[index as usize];
    range.end.store(end, Ordering::SeqCst);
    range.start.store(start, Ordering::SeqCst);
    if env_is_set(b"LXRT_TRACE\0") || env_is_set(b"LXRT_TLS_KEEP_LOG\0") {
        unsafe {
            fprintf(
                lxrt_trace_stream(),
                b"[lxrt] tls keep: %s 0x%llx-0x%llx: TLS reads left in place\n\0"
                    .as_ptr()
                    .cast(),
                if why.is_null() {
                    b"range\0".as_ptr().cast::<c_char>()
                } else {
                    why
                },
                start as c_ulonglong,
                end as c_ulonglong,
            );
        }
    }
}

#[no_mangle]
pub extern "C" fn lxrt_tlskeep_contains(address: u64) -> bool {
    let count = KEEP_COUNT.load(Ordering::SeqCst).max(0) as usize;
    for range in KEEP_RANGES.iter().take(count.min(TLSKEEP_MAX)) {
        let start = range.start.load(Ordering::SeqCst);
        if start != 0 && address >= start && address < range.end.load(Ordering::SeqCst) {
            return true;
        }
    }
    false
}

#[no_mangle]
pub extern "C" fn lxrt_tlskeep_fixups() -> c_ulong {
    KEEP_FIXUPS.load(Ordering::SeqCst) as c_ulong
}

#[no_mangle]
pub extern "C" fn lxrt_tlskeep_fixup(signal: c_int, ucontext: *mut c_void) -> bool {
    if (signal != D_SIGSEGV && signal != D_SIGBUS)
        || ucontext.is_null()
        || KEEP_COUNT.load(Ordering::SeqCst) == 0
    {
        return false;
    }
    let state = unsafe { lxrt_arm64_thread_state(ucontext) }.cast::<ArmThreadState64>();
    if state.is_null() {
        return false;
    }
    let state = unsafe { &mut *state };
    let pc = state.pc;
    if !lxrt_tlskeep_contains(pc) {
        return false;
    }
    let instruction = unsafe { ptr::read(pc as usize as *const u32) };
    // Load/store encoding group; Rn is bits 9:5, and 31 is sp.
    if instruction & 0x0a00_0000 != 0x0800_0000 {
        return false;
    }
    let rn = ((instruction >> 5) & 31) as usize;
    if rn == 31 {
        return false;
    }
    let value = match rn {
        29 => state.fp,
        30 => state.lr,
        _ => state.x[rn],
    };
    if (1u64 << 32..1u64 << 48).contains(&value) {
        return false;
    }
    let thread_pointer = lxrt_tls_get();
    if thread_pointer == 0 || value == thread_pointer {
        return false;
    }
    match rn {
        29 => state.fp = thread_pointer,
        30 => state.lr = thread_pointer,
        _ => state.x[rn] = thread_pointer,
    }
    KEEP_FIXUPS.fetch_add(1, Ordering::SeqCst);
    true
}
