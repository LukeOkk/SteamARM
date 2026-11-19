// Maps the embedded Linux vDSO and refreshes its shared data page.

use std::ffi::{c_int, c_void};
use std::mem::{offset_of, size_of};
use std::ptr;
use std::sync::atomic::{AtomicPtr, AtomicU32, AtomicU64, Ordering};

#[allow(dead_code)]
mod generated {
    include!(concat!(env!("OUT_DIR"), "/privmap_constants.rs"));
    include!(concat!(env!("OUT_DIR"), "/timer_constants.rs"));
}

use generated::{D_MAP_ANON, D_MAP_PRIVATE, D_PROT_EXEC, D_PROT_READ, D_PROT_WRITE, D_SIG_BLOCK};

const HOST_PAGE: usize = 16_384;
const CLOCK_REALTIME: c_int = 0;
const REPORT_WORDS: usize = 32;

#[repr(C)]
struct Vvar {
    seq: AtomicU32,
    enabled: u32,
    numer: u32,
    denom: u32,
    mult: u64,
    rt_offset_ns: i64,
}

const _: [(); 32] = [(); size_of::<Vvar>()];
const _: [(); 16] = [(); offset_of!(Vvar, mult)];
const _: [(); 24] = [(); offset_of!(Vvar, rt_offset_ns)];

#[repr(C, align(16))]
struct RewriteReport([u64; REPORT_WORDS]);

#[repr(C)]
struct TimebaseInfo {
    numer: u32,
    denom: u32,
}

static VVAR: AtomicPtr<Vvar> = AtomicPtr::new(ptr::null_mut());
static EHDR: AtomicU64 = AtomicU64::new(0);
static MULT: AtomicU64 = AtomicU64::new(0);

#[link(name = "System")]
unsafe extern "C" {
    static lxrt_vdso_image: u8;
    static lxrt_vdso_image_end: u8;

    fn mach_absolute_time() -> u64;
    fn mach_timebase_info(info: *mut TimebaseInfo) -> c_int;
    fn clock_gettime(clock_id: c_int, time: *mut Timespec) -> c_int;
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(address: *mut c_void, length: usize) -> c_int;
    fn mprotect(address: *mut c_void, length: usize, protection: c_int) -> c_int;
    fn pthread_create(
        thread: *mut *mut c_void,
        attributes: *const c_void,
        start: unsafe extern "C" fn(*mut c_void) -> *mut c_void,
        argument: *mut c_void,
    ) -> c_int;
    fn pthread_detach(thread: *mut c_void) -> c_int;
    fn pthread_sigmask(how: c_int, set: *const c_void, old: *mut c_void) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn sigfillset(set: *mut c_void) -> c_int;
    fn usleep(microseconds: u32) -> c_int;
    fn lxrt_rewrite_range(start: u64, end: u64, report: *mut c_void, error: *mut *mut i8) -> c_int;
    fn free(pointer: *mut c_void);
}

#[repr(C)]
struct Timespec {
    seconds: i64,
    nanoseconds: i64,
}

fn image_range() -> (usize, usize) {
    let start = ptr::addr_of!(lxrt_vdso_image) as usize;
    let end = ptr::addr_of!(lxrt_vdso_image_end) as usize;
    (start, end - start)
}

fn mono_mult() -> u64 {
    let existing = MULT.load(Ordering::Relaxed);
    if existing != 0 {
        return existing;
    }
    let mut info = TimebaseInfo { numer: 0, denom: 0 };
    unsafe { mach_timebase_info(&mut info) };
    let computed = (((info.numer as u128) << 32) / info.denom as u128) as u64;
    match MULT.compare_exchange(0, computed, Ordering::Relaxed, Ordering::Relaxed) {
        Ok(_) => computed,
        Err(value) => value,
    }
}

#[no_mangle]
pub extern "C" fn lxrt_mono_ns() -> u64 {
    ((unsafe { mach_absolute_time() } as u128 * mono_mult() as u128) >> 32) as u64
}

#[no_mangle]
pub extern "C" fn lxrt_vdso_refresh() {
    let vvar = VVAR.load(Ordering::Acquire);
    if vvar.is_null() {
        return;
    }
    let mut realtime = Timespec {
        seconds: 0,
        nanoseconds: 0,
    };
    unsafe { clock_gettime(CLOCK_REALTIME, &mut realtime) };
    let monotonic = lxrt_mono_ns();
    let realtime_ns = (realtime.seconds as u64)
        .wrapping_mul(1_000_000_000)
        .wrapping_add(realtime.nanoseconds as u64);
    let offset = realtime_ns.wrapping_sub(monotonic) as i64;
    unsafe {
        (*vvar).seq.fetch_add(1, Ordering::AcqRel);
        ptr::write_volatile(ptr::addr_of_mut!((*vvar).rt_offset_ns), offset);
        (*vvar).seq.fetch_add(1, Ordering::Release);
    }
}

unsafe extern "C" fn refresher(_argument: *mut c_void) -> *mut c_void {
    let mut all_signals = 0u32;
    unsafe {
        sigfillset((&mut all_signals as *mut u32).cast());
        pthread_sigmask(
            D_SIG_BLOCK,
            (&all_signals as *const u32).cast(),
            ptr::null_mut(),
        );
    }
    loop {
        unsafe { usleep(250_000) };
        lxrt_vdso_refresh();
    }
}

fn start_refresher() {
    let mut thread = ptr::null_mut();
    if unsafe { pthread_create(&mut thread, ptr::null(), refresher, ptr::null_mut()) } == 0 {
        unsafe { pthread_detach(thread) };
    }
}

extern "C" fn after_fork_child() {
    if !VVAR.load(Ordering::Acquire).is_null() {
        lxrt_vdso_refresh();
        start_refresher();
    }
}

#[no_mangle]
pub extern "C" fn lxrt_vdso_setup() -> u64 {
    let existing = EHDR.load(Ordering::Acquire);
    if existing != 0 || std::env::var_os("LXRT_NO_VDSO").is_some() {
        return existing;
    }

    let (image, image_size) = image_range();
    let Some(image_pages) = image_size
        .checked_add(HOST_PAGE - 1)
        .map(|n| n & !(HOST_PAGE - 1))
    else {
        return 0;
    };
    let Some(total_size) = HOST_PAGE.checked_add(image_pages) else {
        return 0;
    };
    let base = unsafe {
        mmap(
            ptr::null_mut(),
            total_size,
            D_PROT_READ | D_PROT_WRITE,
            D_MAP_PRIVATE | D_MAP_ANON,
            -1,
            0,
        )
    };
    if base as isize == -1 {
        return 0;
    }

    let image_mapping = unsafe { base.cast::<u8>().add(HOST_PAGE) };
    unsafe { ptr::copy_nonoverlapping(image as *const u8, image_mapping, image_size) };

    let mut report = RewriteReport([0; REPORT_WORDS]);
    let mut error = ptr::null_mut();
    if unsafe {
        lxrt_rewrite_range(
            image_mapping as u64,
            image_mapping as u64 + image_size as u64,
            (&mut report as *mut RewriteReport).cast(),
            &mut error,
        )
    } != 0
    {
        if !error.is_null() {
            unsafe { free(error.cast()) };
        }
        unsafe { munmap(base, total_size) };
        return 0;
    }
    if unsafe { mprotect(image_mapping.cast(), image_pages, D_PROT_READ | D_PROT_EXEC) } != 0 {
        unsafe { munmap(base, total_size) };
        return 0;
    }

    let vvar = base.cast::<Vvar>();
    let mut timebase = TimebaseInfo { numer: 0, denom: 0 };
    unsafe { mach_timebase_info(&mut timebase) };
    unsafe {
        ptr::write_volatile(ptr::addr_of_mut!((*vvar).numer), timebase.numer);
        ptr::write_volatile(ptr::addr_of_mut!((*vvar).denom), timebase.denom);
        ptr::write_volatile(ptr::addr_of_mut!((*vvar).mult), mono_mult());
    }
    VVAR.store(vvar, Ordering::Release);

    // The commpage timebase type is user-readable only for values 1 through 3.
    let commpage_type = unsafe { ptr::read_volatile(0x0000_000f_ffff_c090usize as *const u8) };
    lxrt_vdso_refresh();
    unsafe {
        ptr::write_volatile(
            ptr::addr_of_mut!((*vvar).enabled),
            u32::from((1..=3).contains(&commpage_type)),
        );
    }
    start_refresher();
    unsafe { pthread_atfork(None, None, Some(after_fork_child)) };
    let ehdr = image_mapping as u64;
    EHDR.store(ehdr, Ordering::Release);
    ehdr
}

#[no_mangle]
pub extern "C" fn lxrt_vdso_contains(address: u64) -> bool {
    let ehdr = EHDR.load(Ordering::Acquire);
    if ehdr == 0 {
        return false;
    }
    let (_, image_size) = image_range();
    let image_pages = image_size.saturating_add(HOST_PAGE - 1) & !(HOST_PAGE - 1);
    address >= ehdr - HOST_PAGE as u64 && address < ehdr + image_pages as u64
}

#[no_mangle]
pub extern "C" fn lxrt_vdso_ehdr() -> u64 {
    EHDR.load(Ordering::Acquire)
}
