// The machine-description half of the synthetic /proc.
//
// procfs.c answers the questions only the runtime can answer -- where the
// executable is, what the address space looks like, which fds are open. This
// answers the questions the *kernel* would answer: how much memory the machine
// has, what the CPU is, how long it has been up, what the sysctl knobs are set
// to. Roughly 40 distinct /proc paths appear in the Steam binaries; the ones
// here are the subset that is both load-bearing and honestly generable from
// Darwin.
//
// Two rules govern everything below.
//
// The first is that the CONTENT must be Linux-shaped, not macOS-shaped. These
// files are read by Linux parsers that were never written to be defensive:
// glibc's sysconf(_SC_PHYS_PAGES) wants `MemTotal: N kB`, SDL and Steam's
// hardware survey want an aarch64 /proc/cpuinfo with `CPU implementer` and
// `Features`, and srt-bwrap wants an 11-field /proc/self/mountinfo. Emitting
// `sysctl -a` output in a /proc file is worse than emitting nothing, because
// nothing at least produces ENOENT.
//
// The second is that the NUMBERS must be measured. Every figure here that can
// be read out of Darwin is read out of Darwin -- hw.memsize, hw.logicalcpu,
// hw.tbfrequency, vm_statistics64, host_cpu_load_info, kern.boottime,
// vm.swapusage, task_info. Only the values Darwin genuinely does not have are
// constants, and each of those carries a comment saying so.
//
// Not provided here, deliberately:
//
//   /proc/self/maps       procfs.c already builds it from the real address
//                         space. Two generators for one file is how they drift.
//   /proc/self/ns/*       Linux namespaces. Darwin has none -- not a weaker
//                         version, none. A synthetic ns/ directory would make
//                         pressure-vessel believe unshare(CLONE_NEWNS) can
//                         work, and it takes the container path on exactly
//                         that evidence. ENOENT sends it down the fallback.
//   /proc/<other pid>/*   Reading another process needs task_for_pid(), which
//                         needs root or com.apple.security.cs.debugger, and
//                         what it would describe is a Darwin process anyway.
//   /proc/kallsyms,
//   /proc/modules         There is no kernel to enumerate.
//   /proc/self/smaps      Per-mapping Linux accounting (Pss, Private_Dirty)
//                         with no Darwin counterpart at any granularity.
//   /proc/self/oom_*      Writable control files. Everything this module
//                         publishes is a 0444 regular file, which is right for
//                         the files Linux also makes read-only to an
//                         unprivileged process, and wrong for these: a write
//                         to oom_score_adj is meant to SUCCEED, so it would
//                         fail with EACCES and read back the old value.
//
// This is a line-for-line port of the former proc_ext.c. Like the C, it
// formats with the C library's snprintf into caller-sized buffers (sbf() was a
// vsnprintf wrapper; here it is a macro around snprintf with the same
// accounting), takes its generation buffers from malloc, guards the published
// auxv with a pthread mutex registered with pthread_atfork, and hands back a
// pointer into a per-thread buffer.

use std::cell::UnsafeCell;
use std::ffi::{c_char, c_double, c_int, c_long, c_uint, c_ulonglong, c_void, CStr};
use std::mem::{size_of, MaybeUninit};
use std::ptr;
use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};

// Darwin (arm64) values of the SDK constants the C original used.
const EFAULT: c_int = 14;
const E2BIG: c_int = 7;
const O_WRONLY: c_int = 0x1;
const O_CREAT: c_int = 0x200;
const O_TRUNC: c_int = 0x400;
const S_IFMT: u16 = 0o170000;
const S_IFDIR: u16 = 0o040000;
const CTL_KERN: c_int = 1;
const KERN_PROC: c_int = 14;
const KERN_BOOTTIME: c_int = 21;
const KERN_PROC_ALL: c_int = 0;
const KERN_PROC_PID: c_int = 1;
const KERN_SUCCESS: c_int = 0;
const MACH_PORT_NULL: u32 = 0;
const HOST_CPU_LOAD_INFO: c_int = 3;
const HOST_VM_INFO64: c_int = 4;
const PROCESSOR_CPU_LOAD_INFO: c_int = 2;
const CPU_STATE_USER: usize = 0;
const CPU_STATE_SYSTEM: usize = 1;
const CPU_STATE_IDLE: usize = 2;
const CPU_STATE_NICE: usize = 3;
const TASK_EVENTS_INFO: u32 = 2;
const TASK_THREAD_TIMES_INFO: u32 = 3;
const MACH_TASK_BASIC_INFO: u32 = 20;
const SM_EMPTY: u8 = 3;
const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;

// time_value_t
#[repr(C)]
#[derive(Clone, Copy)]
struct TimeValue {
    seconds: i32,
    microseconds: i32,
}

// struct timeval, Darwin LP64: tv_usec is 32-bit and followed by padding.
#[repr(C)]
struct Timeval {
    tv_sec: i64,
    tv_usec: i32,
    pad: i32,
}
const _: [(); 16] = [(); size_of::<Timeval>()];

// vm_statistics64_data_t, as far as its fields are read here. The tail is the
// rest of the SDK's revision (REV6: 104 natural_t in all), which the kernel
// may fill and nothing here looks at.
#[repr(C)]
struct VmStatistics64 {
    free_count: u32,
    active_count: u32,
    inactive_count: u32,
    wire_count: u32,
    zero_fill_count: u64,
    reactivations: u64,
    pageins: u64,
    pageouts: u64,
    faults: u64,
    cow_faults: u64,
    lookups: u64,
    hits: u64,
    purges: u64,
    purgeable_count: u32,
    speculative_count: u32,
    decompressions: u64,
    compressions: u64,
    swapins: u64,
    swapouts: u64,
    compressor_page_count: u32,
    throttled_count: u32,
    external_page_count: u32,
    internal_page_count: u32,
    total_uncompressed_pages_in_compressor: u64,
    later_revisions: [u64; 33],
}
const _: [(); 416] = [(); size_of::<VmStatistics64>()];
const HOST_VM_INFO64_COUNT: u32 = (size_of::<VmStatistics64>() / size_of::<i32>()) as u32;

// host_cpu_load_info_data_t and processor_cpu_load_info: CPU_STATE_MAX ticks.
#[repr(C)]
struct CpuLoadInfo {
    cpu_ticks: [u32; 4],
}
const HOST_CPU_LOAD_INFO_COUNT: u32 = (size_of::<CpuLoadInfo>() / size_of::<i32>()) as u32;

// mach_task_basic_info_data_t (#pragma pack(4) in the SDK; no field moves).
#[repr(C, packed(4))]
struct MachTaskBasicInfo {
    virtual_size: u64,
    resident_size: u64,
    resident_size_max: u64,
    user_time: TimeValue,
    system_time: TimeValue,
    policy: i32,
    suspend_count: i32,
}
const _: [(); 48] = [(); size_of::<MachTaskBasicInfo>()];
const MACH_TASK_BASIC_INFO_COUNT: u32 = (size_of::<MachTaskBasicInfo>() / size_of::<u32>()) as u32;

// task_thread_times_info_data_t
#[repr(C)]
struct TaskThreadTimesInfo {
    user_time: TimeValue,
    system_time: TimeValue,
}
const TASK_THREAD_TIMES_INFO_COUNT: u32 =
    (size_of::<TaskThreadTimesInfo>() / size_of::<u32>()) as u32;

// task_events_info_data_t
#[repr(C)]
struct TaskEventsInfo {
    faults: i32,
    pageins: i32,
    cow_faults: i32,
    messages_sent: i32,
    messages_received: i32,
    syscalls_mach: i32,
    syscalls_unix: i32,
    csw: i32,
}
const TASK_EVENTS_INFO_COUNT: u32 = (size_of::<TaskEventsInfo>() / size_of::<u32>()) as u32;

// vm_region_submap_info_data_64_t is packed(4) and 84 bytes; only user_tag
// (offset 20, a 32-bit word) and share_mode (offset 47, one byte) are read.
#[repr(C)]
struct VmRegionSubmapInfo64 {
    words: [u32; 21],
}
const VM_REGION_SUBMAP_INFO_COUNT_64: u32 =
    (size_of::<VmRegionSubmapInfo64>() / size_of::<u32>()) as u32;
const SUBMAP_USER_TAG_WORD: usize = 20 / 4;
const SUBMAP_SHARE_MODE_OFFSET: usize = 47;

// struct xsw_usage
#[repr(C)]
struct XswUsage {
    xsu_total: u64,
    xsu_avail: u64,
    xsu_used: u64,
    xsu_pagesize: u32,
    xsu_encrypted: c_int,
}
const _: [(); 32] = [(); size_of::<XswUsage>()];

// struct kinfo_proc is 648 bytes. kp_proc.p_starttime is the first member (the
// p_un union at offset 0), which is the only part read here.
#[repr(C)]
struct KinfoProc {
    p_starttime: Timeval,
    rest: [u64; 79],
}
const KINFO_PROC_SIZE: usize = size_of::<KinfoProc>();
const _: [(); 648] = [(); KINFO_PROC_SIZE];

// struct stat (arm64: the 64-bit-inode layout). Only st_mode is read.
#[repr(C)]
struct Stat {
    st_dev: i32,
    st_mode: u16,
    st_nlink: u16,
    rest: [u64; 17],
}
const _: [(); 144] = [(); size_of::<Stat>()];

// mach_timebase_info_data_t
#[repr(C)]
struct MachTimebaseInfo {
    numer: u32,
    denom: u32,
}

// pthread_mutex_t, Darwin LP64: a signature word plus 56 opaque bytes.
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}
const _: [(); 64] = [(); size_of::<PthreadMutex>()];

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: u32;
    static vm_kernel_page_size: usize;

    fn snprintf(buf: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn strlen(s: *const c_char) -> usize;
    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
    fn strncmp(a: *const c_char, b: *const c_char, n: usize) -> c_int;
    fn malloc(size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn write(fd: c_int, buf: *const c_void, count: usize) -> isize;
    fn close(fd: c_int) -> c_int;
    fn unlink(path: *const c_char) -> c_int;
    fn rename(from: *const c_char, to: *const c_char) -> c_int;
    fn mkdir(path: *const c_char, mode: u16) -> c_int;
    fn stat(path: *const c_char, out: *mut Stat) -> c_int;
    fn getpid() -> c_int;
    fn getppid() -> c_int;
    fn getpgrp() -> c_int;
    fn getsid(pid: c_int) -> c_int;
    fn getuid() -> c_uint;
    fn geteuid() -> c_uint;
    fn getgid() -> c_uint;
    fn getegid() -> c_uint;
    fn getprogname() -> *const c_char;
    fn getloadavg(loadavg: *mut c_double, nelem: c_int) -> c_int;
    fn gettimeofday(tp: *mut Timeval, tzp: *mut c_void) -> c_int;
    fn arc4random_buf(buf: *mut c_void, nbytes: usize);
    fn sysctl(
        name: *mut c_int,
        namelen: c_uint,
        oldp: *mut c_void,
        oldlenp: *mut usize,
        newp: *mut c_void,
        newlen: usize,
    ) -> c_int;
    fn sysctlbyname(
        name: *const c_char,
        oldp: *mut c_void,
        oldlenp: *mut usize,
        newp: *mut c_void,
        newlen: usize,
    ) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;

    fn mach_host_self() -> u32;
    fn mach_port_deallocate(task: u32, name: u32) -> c_int;
    fn mach_absolute_time() -> u64;
    fn mach_timebase_info(info: *mut MachTimebaseInfo) -> c_int;
    fn host_statistics64(host: u32, flavor: c_int, info: *mut i32, count: *mut u32) -> c_int;
    fn host_statistics(host: u32, flavor: c_int, info: *mut i32, count: *mut u32) -> c_int;
    fn host_processor_info(
        host: u32,
        flavor: c_int,
        out_processor_count: *mut u32,
        out_processor_info: *mut *mut i32,
        out_processor_info_count: *mut u32,
    ) -> c_int;
    fn task_info(task: u32, flavor: u32, info: *mut i32, count: *mut u32) -> c_int;
    fn task_threads(task: u32, list: *mut *mut u32, count: *mut u32) -> c_int;
    fn vm_deallocate(task: u32, address: usize, size: usize) -> c_int;
    fn mach_vm_region_recurse(
        task: u32,
        address: *mut u64,
        size: *mut u64,
        nesting_depth: *mut u32,
        info: *mut i32,
        count: *mut u32,
    ) -> c_int;
    fn mach_vm_read_overwrite(
        task: u32,
        address: u64,
        size: u64,
        data: u64,
        outsize: *mut u64,
    ) -> c_int;
}

// The rest of the runtime (C or Rust), by its C names.
unsafe extern "C" {
    static mut lxrt_start_stack: u64;

    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_guest_page() -> u64; // stack.rs: AT_PAGESZ (LXRT_GUEST_PAGE)
    fn lxrt_hwcap_cpuid() -> bool; // stack.rs
    fn lxrt_synthetic_sysreg(instruction: u32) -> u64; // sysreg.rs
    fn lxrt_file_same(path: *const c_char, data: *const c_void, len: usize) -> bool; // procfs.c
    fn lxrt_thread_times(
        tid: c_int,
        user_us: *mut u64,
        sys_us: *mut u64,
        running: *mut bool,
    ) -> bool;
    fn lxrt_ids_on() -> bool; // ids.rs
    fn lxrt_ids_pid() -> c_int;
    fn lxrt_ids_to_guest(host_pid: c_int, index: c_int) -> c_int;
    // android_ids.h
    fn lxrt_aids_status_ids(u: *mut u32, g: *mut u32, groups: *mut c_char, groups_len: usize);
}

// #define LERR(e) (-lxrt_errno_to_linux(e))
#[inline(always)]
unsafe fn lerr(error: c_int) -> c_long {
    (-lxrt_errno_to_linux(error)) as c_long
}

// mach_task_self() is a macro for this cached special port.
#[inline(always)]
unsafe fn mach_task_self() -> u32 {
    mach_task_self_
}

// The C divided by the guest page size and by a timebase numerator without
// checking them. Neither is ever zero; if one were, arm64's udiv would have
// produced 0 rather than trapping, and this must not panic where the C did
// not, so it produces the same 0.
#[inline(always)]
fn udiv(a: u64, b: u64) -> u64 {
    a.checked_div(b).unwrap_or(0)
}

// One generated file, before it is written out. 10 cores of cpuinfo is 2.3 KB;
// a 128-core machine would be 30 KB. Heap, not static, because two guest
// threads can be in here at once.
const GEN_MAX: usize = 128 * 1024;

// Two different page sizes are in play here, and conflating them is the bug
// this pair of accessors exists to prevent.
//
// guest_page_bytes() is the unit the GUEST divides by. Linux's
// /proc/self/stat and /proc/self/statm report memory in PAGES, the guest
// converts with sysconf(_SC_PAGESIZE), that comes from AT_PAGESZ, and
// AT_PAGESZ is whatever stack.rs published -- LXRT_HOST_PAGE, lxrt.h:18. Every
// page COUNT in this file has to be in those units or the guest's own
// arithmetic turns it into the wrong number of bytes.
//
// host_page_bytes() is the unit Darwin's counters arrive in. vm_statistics64's
// fields and task_info's resident_size are measured in vm_kernel_page_size.
// Measured on Apple Silicon: getpagesize(), vm_kernel_page_size and
// host_page_size() all agree at 16384, and
// free+active+inactive+wire+speculative+compressor came to 1018436 against
// hw.memsize/16384 = 1048576, the shortfall being the firmware carveout.
//
// Today the two are the same number, so nothing observable depends on the
// distinction. They are split anyway because guest and host page sizes need
// not agree: x86 guests under FEX use 4 KiB pages inside the 16 KiB host pages
// (runtime/subpage.c), while the host page itself cannot be 4 KiB on Apple
// Silicon Darwin (LXRT_HOST_PAGE is 16384, lxrt.h:19; "the host page cannot be
// subdivided", subpage.c:12). Were the guest granule ever to differ from the
// host's, a single page_bytes() reading vm_kernel_page_size would put statm,
// stat's rss, vmstat's nr_* and the auxv fallback's AT_PAGESZ 4x away from the
// guest's own sysconf(_SC_PAGESIZE), silently. Converting instead of warning
// is the fix: the numbers stay right on either granule, so there is nothing
// left to warn about, and a per-read fprintf into the guest's stderr would be
// worse than the divergence it announced.
unsafe fn guest_page_bytes() -> u64 {
    lxrt_guest_page()
}

unsafe fn host_page_bytes() -> u64 {
    vm_kernel_page_size as u64
}

// A Darwin page count restated in the page unit the guest will divide by.
unsafe fn host_pages_to_guest(pages: u64) -> u64 {
    let gp = guest_page_bytes();
    if gp == 0 {
        return pages;
    }
    pages.wrapping_mul(host_page_bytes()) / gp
}

// ------------------------------------------------------------ string buffer

// The `trunc` flag is not decoration. sbf() caps at cap-1 and returns, which
// on a machine with enough cores used to hand /proc/cpuinfo out with its last
// record cut off mid-line -- a file no parser rejects and every parser
// misreads. Truncation is now recorded, sb_done() turns it into GEN_TRUNC, and
// materialise() grows the buffer and retries rather than publishing the stump.
struct Sb {
    p: *mut c_char,
    cap: usize,
    len: usize,
    trunc: bool,
}

impl Sb {
    #[inline(always)]
    fn new(p: *mut c_char, cap: usize) -> Self {
        Self {
            p,
            cap,
            len: 0,
            trunc: false,
        }
    }
}

// Returned by a generator instead of a length when the content did not fit.
const GEN_TRUNC: usize = usize::MAX;

// sbf(s, fmt, ...): the C was a printf-style function over vsnprintf. Rust
// cannot define one, so the call to snprintf is made at each use, with the
// same accounting before and after it.
macro_rules! sbf {
    ($s:expr, $fmt:expr $(, $arg:expr)* $(,)?) => {{
        let s: &mut Sb = $s;
        let room: usize = if s.cap > s.len { s.cap - s.len } else { 0 };
        if room < 2 {
            s.trunc = true;
        } else {
            let fmt: &CStr = $fmt;
            let n: c_int = snprintf(s.p.add(s.len), room, fmt.as_ptr() $(, $arg)*);
            if n < 0 {
                s.trunc = true;
            } else if (n as usize) < room {
                s.len += n as usize;
            } else {
                s.len += room - 1;
                s.trunc = true;
            }
        }
    }};
}

fn sb_done(s: &Sb) -> usize {
    if s.trunc {
        GEN_TRUNC
    } else {
        s.len
    }
}

// Linux prints meminfo as a 16-column label followed by an 8-wide count and
// the literal " kB". The kB is binary: the kernel shifts pages by
// (PAGE_SHIFT - 10), so it is 1024 bytes, not 1000. A parser that divides by
// 1000 to get megabytes is off by 2.4%, which is why the unit is spelled out
// here rather than left to the call sites.
unsafe fn mem_kb(s: &mut Sb, label: &CStr, bytes: u64) {
    sbf!(
        s,
        c"%-15s %8llu kB\n",
        label.as_ptr(),
        (bytes / 1024) as c_ulonglong
    );
}

// ------------------------------------------------------------ Darwin probes

unsafe fn sctl_u64(name: &CStr, dflt: u64) -> u64 {
    // Darwin sysctls are not all the same width: hw.memsize is 8 bytes,
    // kern.maxfilesperproc is 4 (both measured). Asking for 8 on a 4-byte node
    // succeeds, writes 4 and reports 4, so the buffer is zeroed first and the
    // returned length decides how much of it is real. Reading the low half is
    // correct only because arm64 macOS is little-endian, which it always is.
    let mut v: u64 = 0;
    let mut len: usize = size_of::<u64>();
    if sysctlbyname(
        name.as_ptr(),
        ptr::addr_of_mut!(v).cast(),
        &mut len,
        ptr::null_mut(),
        0,
    ) != 0
    {
        return dflt;
    }
    if len == 4 {
        return v & 0xffff_ffff;
    }
    if len != 8 {
        return dflt;
    }
    v
}

// mach_host_self() hands out a send right and every call adds a reference --
// unlike mach_task_self(), which is a cached special port and needs no
// deallocate. These generators run on every /proc read, so the port is taken
// once and kept rather than leaked a few times per guest open().
unsafe fn host_port() -> u32 {
    static CACHED: AtomicU32 = AtomicU32::new(MACH_PORT_NULL);
    let p = CACHED.load(Ordering::SeqCst);
    if p != MACH_PORT_NULL {
        return p;
    }
    let fresh = mach_host_self();
    match CACHED.compare_exchange(MACH_PORT_NULL, fresh, Ordering::SeqCst, Ordering::SeqCst) {
        Ok(_) => fresh,
        Err(expect) => {
            mach_port_deallocate(mach_task_self(), fresh);
            expect
        }
    }
}

// HOST_VM_INFO64_COUNT is the count of the revision the SDK was built against
// (REV6, 104 natural_t, on this one). host_statistics64 CLAMPS *count down to
// whatever the running kernel actually supports and leaves the rest of the
// caller's struct untouched, so on an older kernel the tail -- which is where
// compressor_page_count, external_page_count and internal_page_count live,
// all REV1 -- would be read uninitialised. Zeroing first turns that into a
// reported 0 instead of a stack leak into a guest-readable file. Measured on
// this machine: the kernel returns the full 104, so nothing in use is
// currently short.
unsafe fn host_vm_stats(out: *mut VmStatistics64) -> bool {
    ptr::write_bytes(out, 0, 1);
    let mut count: u32 = HOST_VM_INFO64_COUNT;
    host_statistics64(host_port(), HOST_VM_INFO64, out.cast(), &mut count) == KERN_SUCCESS
}

// Aggregate CPU time. Darwin's cpu_ticks are already in 100 Hz units, the same
// USER_HZ Linux uses in /proc/stat -- checked rather than assumed: on a
// 10-core machine up 228469 s the ticks summed to 227607055, and
// 227607055 / 10 / 100 = 227607 s, a 0.4% match. CPU_STATE_* ordering is
// USER, SYSTEM, IDLE, NICE, which is NOT Linux's field order
// (user, nice, system, idle), so the two are never emitted positionally.
unsafe fn cpu_load(out: *mut CpuLoadInfo) -> bool {
    let mut count: u32 = HOST_CPU_LOAD_INFO_COUNT;
    host_statistics(host_port(), HOST_CPU_LOAD_INFO, out.cast(), &mut count) == KERN_SUCCESS
}

unsafe fn boot_time_sec() -> u64 {
    let mut bt = MaybeUninit::<Timeval>::zeroed();
    let mut len: usize = size_of::<Timeval>();
    let mut mib: [c_int; 2] = [CTL_KERN, KERN_BOOTTIME];
    if sysctl(
        mib.as_mut_ptr(),
        2,
        bt.as_mut_ptr().cast(),
        &mut len,
        ptr::null_mut(),
        0,
    ) != 0
    {
        return 0;
    }
    (*bt.as_ptr()).tv_sec as u64
}

// Linux's /proc/uptime is CLOCK_BOOTTIME: it keeps counting across suspend.
// mach_absolute_time() does not -- it stops when the Mac sleeps -- so the lid
// being shut for an hour would make uptime jump backwards relative to wall
// clock. The kern.boottime delta is the one that behaves like Linux.
unsafe fn uptime_sec() -> f64 {
    let bt = boot_time_sec();
    if bt == 0 {
        return 0.0;
    }
    let mut now = MaybeUninit::<Timeval>::zeroed();
    gettimeofday(now.as_mut_ptr(), ptr::null_mut());
    let now = &*now.as_ptr();
    let s = now.tv_sec as f64 - bt as f64 + now.tv_usec as f64 / 1e6;
    if s > 0.0 {
        s
    } else {
        0.0
    }
}

unsafe fn task_basic(out: *mut MachTaskBasicInfo) -> bool {
    let mut count: u32 = MACH_TASK_BASIC_INFO_COUNT;
    task_info(
        mach_task_self(),
        MACH_TASK_BASIC_INFO,
        out.cast(),
        &mut count,
    ) == KERN_SUCCESS
}

// MACH_TASK_BASIC_INFO's user_time and system_time are NOT the process's CPU
// time. The SDK header says so on the line the fields are declared:
// "total user run time for TERMINATED threads" (mach/task_info.h). Live
// threads are accounted in TASK_THREAD_TIMES_INFO, and the process total is
// the sum of the two -- that is exactly how ps(1) and getrusage() arrive at
// theirs. Measured with this split: a process that burned 0.3745 s of user CPU
// (getrusage ground truth) reported basic=0 ticks, thread=37 ticks, sum=37,
// against the 37.45 ticks getrusage implies. Reading only the basic half is
// what made /proc/self/stat fields 14 and 15 permanently 0, which tells any
// guest profiler, times(3) caller or Steam overlay that this process has used
// no CPU since it started -- plausibly wrong rather than visibly wrong, the
// one failure mode this file exists to keep out.
unsafe fn task_thread_times(out: *mut TaskThreadTimesInfo) -> bool {
    let mut count: u32 = TASK_THREAD_TIMES_INFO_COUNT;
    task_info(
        mach_task_self(),
        TASK_THREAD_TIMES_INFO,
        out.cast(),
        &mut count,
    ) == KERN_SUCCESS
}

unsafe fn task_events(out: *mut TaskEventsInfo) -> bool {
    let mut count: u32 = TASK_EVENTS_INFO_COUNT;
    task_info(mach_task_self(), TASK_EVENTS_INFO, out.cast(), &mut count) == KERN_SUCCESS
}

// TASK_EVENTS_INFO counts faults, pageins and context switches in integer_t --
// SIGNED 32-bit -- while the Linux fields they feed (/proc/self/stat minflt
// and majflt, /proc/stat ctxt) are unsigned long. A process that outlives
// 2^31 faults, which Steam does inside a session, wraps the Darwin counter
// negative, and a straight cast to uint64_t turns -1 into 18446744073709551615
// in a file the guest is parsing. Clamping is the only honest option: the true
// count is unrecoverable once the kernel has wrapped it.
fn nonneg(v: i32) -> u64 {
    if v > 0 {
        v as u64
    } else {
        0
    }
}

unsafe fn thread_count() -> c_uint {
    let mut list: *mut u32 = ptr::null_mut();
    let mut n: u32 = 0;
    if task_threads(mach_task_self(), &mut list, &mut n) != KERN_SUCCESS {
        return 1;
    }
    let mut i: u32 = 0;
    while i < n {
        mach_port_deallocate(mach_task_self(), *list.add(i as usize));
        i += 1;
    }
    vm_deallocate(
        mach_task_self(),
        list as usize,
        (n as usize).wrapping_mul(size_of::<u32>()),
    );
    if n != 0 {
        n
    } else {
        1
    }
}

// Darwin's task virtual_size is not a Linux VmSize and reporting it raw is a
// trap. Measured on an idle process here: virtual_size was 500512030720
// (500.5 GB), because every Darwin process carries the shared-region
// reservation. Dropping the regions that are SM_EMPTY (address space reserved
// with nothing behind it) brings it to 61.3 GB, and dropping the ones tagged
// VM_MEMORY_SHARED_PMAP (32 -- the dyld shared cache window, also unbacked,
// pages_resident == 0) brings it to 0.85 GB, which is a figure a Linux tool
// can use. Anything comparing vsize against MemTotal gets a sane answer only
// after both exclusions.
const VM_TAG_SHARED_PMAP: u32 = 32;

unsafe fn virtual_size_linuxish() -> u64 {
    let mut addr: u64 = 0;
    let mut total: u64 = 0;
    let mut guard = 0;
    while guard < 65536 {
        let mut size: u64 = 0;
        let mut info = MaybeUninit::<VmRegionSubmapInfo64>::uninit();
        let mut count: u32 = VM_REGION_SUBMAP_INFO_COUNT_64;
        let mut depth: u32 = 0;
        if mach_vm_region_recurse(
            mach_task_self(),
            &mut addr,
            &mut size,
            &mut depth,
            info.as_mut_ptr().cast(),
            &mut count,
        ) != KERN_SUCCESS
        {
            break;
        }
        let words: *const u32 = info.as_ptr().cast();
        let user_tag = *words.add(SUBMAP_USER_TAG_WORD);
        let share_mode = *words.cast::<u8>().add(SUBMAP_SHARE_MODE_OFFSET);
        if share_mode != SM_EMPTY && user_tag != VM_TAG_SHARED_PMAP {
            total = total.wrapping_add(size);
        }
        addr = addr.wrapping_add(size);
        guard += 1;
    }
    total
}

// A guest pointer, or a runtime pointer built from one, is never dereferenced
// directly: mach_vm_read_overwrite reports a bad source address as
// KERN_INVALID_ADDRESS instead of raising SIGSEGV in the host. Verified: the
// call against address 0 returns 1 (KERN_INVALID_ADDRESS) rather than faulting.
unsafe fn safe_copy_in(dst: *mut c_void, src: *const c_void, n: usize) -> bool {
    if src.is_null() || n == 0 {
        return false;
    }
    let mut got: u64 = 0;
    if mach_vm_read_overwrite(
        mach_task_self(),
        src as usize as u64,
        n as u64,
        dst as usize as u64,
        &mut got,
    ) != KERN_SUCCESS
    {
        return false;
    }
    got == n as u64
}

// ------------------------------------------------------------ file plumbing

// Written through a temporary and renamed. /proc reads on Linux are atomic
// snapshots; a plain O_TRUNC rewrite would let a second guest thread open the
// file while it is half-written and parse a truncated meminfo. rename(2) on
// the same filesystem is atomic, so a reader sees either the previous
// generation or the new one.
//
// The return value carries the whole point of the no-caching design. This used
// to return void and every caller ignored it, so a failed generation -- EMFILE
// under fd pressure, ENOSPC on TMPDIR, a sandbox revoking write on it -- left
// the PREVIOUS generation on disk and translate() handed the guest a path to
// it with no error at all. Reproduced: generate /proc/uptime, chmod 0500 the
// directory, wait, ask again -- same path, same frozen "229691.11". That is
// precisely the first-answer-is-the-only-answer bug this module claims to have
// eliminated, so a failure now unlinks the stale final as well as the temp and
// says so, and translate() turns that into an honest ENOENT.
//
// Mode 0444, not 0644. On Linux /proc/meminfo is 0444 and the /proc/sys knobs
// are root-owned, so an unprivileged write gets EACCES. At 0644 the guest's
// open(O_WRONLY|O_TRUNC) SUCCEEDED on meminfo, self/stat, overcommit_memory,
// ptrace_scope and max_user_watches and then changed nothing -- silently faked
// success on control files, which is strictly worse than the wrong-errno
// outcome the header rejects for /proc/self/oom_*. Regeneration is unaffected:
// every write goes to a fresh temp file and renames over the old one rather
// than reopening it, and unlink/rename are governed by the directory's mode,
// not the file's.
unsafe fn write_atomic(
    dir: *const c_char,
    rel: *const c_char,
    data: *const c_void,
    len: usize,
) -> bool {
    static SEQ: AtomicU32 = AtomicU32::new(0);
    let mut final_ = MaybeUninit::<[c_char; 1024]>::uninit();
    let final_: *mut c_char = final_.as_mut_ptr().cast();
    let mut tmp = MaybeUninit::<[c_char; 1100]>::uninit();
    let tmp: *mut c_char = tmp.as_mut_ptr().cast();
    snprintf(final_, 1024, c"%s/%s".as_ptr(), dir, rel);
    if lxrt_file_same(final_, data, len) {
        return true; // unchanged: no filesystem event (procfs.c)
    }
    snprintf(
        tmp,
        1100,
        c"%s.tmp%u.%u".as_ptr(),
        final_ as *const c_char,
        getpid() as c_uint,
        SEQ.fetch_add(1, Ordering::SeqCst) as c_uint,
    );

    // Accepted limitation in the unlink-on-failure paths: if one thread's
    // generation fails while another's has just succeeded, the failing thread
    // removes the fresh file and the second thread's reader gets ENOENT on a
    // path that was valid a microsecond ago. That is a lost read under a
    // condition that is already an error (EMFILE, ENOSPC, a revoked TMPDIR),
    // and the next lookup regenerates. The alternative -- leaving the old
    // generation in place so a directory walker can still open it -- trades a
    // rare spurious ENOENT for a silent stale answer, which is the trade this
    // whole module refuses to make.
    let fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0o444 as c_int);
    if fd < 0 {
        unlink(final_);
        return false;
    }
    let mut p: *const u8 = data.cast();
    let mut left = len;
    while left != 0 {
        let w = write(fd, p.cast(), left);
        if w <= 0 {
            break;
        }
        p = p.add(w as usize);
        left -= w as usize;
    }
    let mut wrote_all = left == 0;
    if close(fd) != 0 {
        wrote_all = false; // a deferred ENOSPC lands on close()
    }
    if !wrote_all || rename(tmp, final_) != 0 {
        unlink(tmp);
        unlink(final_);
        return false;
    }
    true
}

// mkdir -p, for the /proc/sys tree. Nothing here needs to fail loudly: a
// directory that cannot be created simply means the open() the guest is about
// to do returns ENOENT, which is the same answer it would get from a kernel
// built without that sysctl.
unsafe fn mkdirs(dir: *const c_char, rel: *const c_char) {
    let mut path = MaybeUninit::<[c_char; 1024]>::uninit();
    let path: *mut c_char = path.as_mut_ptr().cast();
    let n = snprintf(path, 1024, c"%s/%s".as_ptr(), dir, rel) as usize;
    if n >= 1024 {
        return;
    }
    let mut i = strlen(dir) + 1;
    while i < n {
        if *path.add(i) != b'/' as c_char {
            i += 1;
            continue;
        }
        *path.add(i) = 0;
        mkdir(path, 0o755);
        *path.add(i) = b'/' as c_char;
        i += 1;
    }
    mkdir(path, 0o755);
}

// ------------------------------------------------------------ /proc/meminfo

unsafe fn gen_meminfo(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let mut vm = MaybeUninit::<VmStatistics64>::uninit();
    let have = host_vm_stats(vm.as_mut_ptr());
    let vm = &*vm.as_ptr();
    // The vm_statistics64 counters are in the HOST granule; these figures are
    // emitted in bytes, so the host unit is the right one throughout.
    let ps = host_page_bytes();
    let total = sctl_u64(c"hw.memsize", 0);

    // Speculative pages are SUBTRACTED, not added. mach/vm_statistics.h:114
    // carries an explicit NB three lines above the field -- "speculative pages
    // are already accounted for in free_count" -- and vm_stat(1) and Activity
    // Monitor both print free_count minus speculative_count. Measured live:
    // free_count=14554, speculative=709, vm_stat printed "Pages free: 13775";
    // the old free+speculative came to 15263. glibc's
    // sysconf(_SC_AVPHYS_PAGES) reads MemFree, so the overstatement (21 MB
    // idle, hundreds of MB under file-cache pressure) lands in the guest's own
    // allocator sizing.
    let mut freep: u64 = 0;
    if have {
        freep = if vm.free_count as u64 > vm.speculative_count as u64 {
            vm.free_count as u64 - vm.speculative_count as u64
        } else {
            0
        };
    }
    let active: u64 = if have { vm.active_count as u64 } else { 0 };
    let inactive: u64 = if have { vm.inactive_count as u64 } else { 0 };
    let wired: u64 = if have { vm.wire_count as u64 } else { 0 };
    let purge: u64 = if have { vm.purgeable_count as u64 } else { 0 };
    let ext: u64 = if have {
        vm.external_page_count as u64
    } else {
        0
    };
    let internal: u64 = if have {
        vm.internal_page_count as u64
    } else {
        0
    };
    let compressed: u64 = if have {
        vm.compressor_page_count as u64
    } else {
        0
    };

    // MemAvailable is "what a new allocation could get without swapping". On
    // Darwin that is free plus the reclaimable half: inactive, purgeable and
    // the clean file cache. Clamped, because those three overlap -- external
    // pages can also be inactive -- and a MemAvailable above MemTotal makes
    // every consumer's percentage arithmetic nonsense.
    let mut avail = freep
        .wrapping_add(inactive)
        .wrapping_add(purge)
        .wrapping_add(ext)
        .wrapping_mul(ps);
    if avail > total {
        avail = total;
    }

    // vm.swapusage is a struct, not a scalar, and the encrypted-swap flag on
    // the end means the length check matters.
    let mut xsu = MaybeUninit::<XswUsage>::zeroed();
    let mut xl: usize = size_of::<XswUsage>();
    let xsu_failed = sysctlbyname(
        c"vm.swapusage".as_ptr(),
        xsu.as_mut_ptr().cast(),
        &mut xl,
        ptr::null_mut(),
        0,
    ) != 0
        || xl != size_of::<XswUsage>();
    let xsu = &mut *xsu.as_mut_ptr();
    if xsu_failed {
        xsu.xsu_total = 0;
        xsu.xsu_avail = 0;
    }

    mem_kb(s, c"MemTotal:", total);
    mem_kb(s, c"MemFree:", freep.wrapping_mul(ps));
    mem_kb(s, c"MemAvailable:", avail);
    // Darwin has one unified cache; there is no separate buffer cache to
    // report, and Linux itself has reported a near-zero Buffers since the
    // page cache was unified in 2.4.
    mem_kb(s, c"Buffers:", 0);
    mem_kb(s, c"Cached:", ext.wrapping_mul(ps));
    mem_kb(s, c"SwapCached:", 0);
    mem_kb(s, c"Active:", active.wrapping_mul(ps));
    mem_kb(s, c"Inactive:", inactive.wrapping_mul(ps));
    // Active(anon)/Active(file) and their inactive twins are omitted, not
    // zeroed. Darwin splits pages by age (active/inactive) and, separately, by
    // backing (internal/external), but never publishes the cross product.
    // Zeros would read as "no anonymous memory is active", which is a
    // statement; absence reads as "this kernel does not export that", which is
    // true, and Linux itself only grew those fields in 2.6.28 so parsers
    // tolerate their absence.
    mem_kb(s, c"Unevictable:", wired.wrapping_mul(ps));
    mem_kb(s, c"Mlocked:", 0);
    mem_kb(s, c"SwapTotal:", xsu.xsu_total);
    mem_kb(s, c"SwapFree:", xsu.xsu_avail);
    mem_kb(s, c"Dirty:", 0);
    mem_kb(s, c"Writeback:", 0);
    mem_kb(s, c"AnonPages:", internal.wrapping_mul(ps));
    mem_kb(s, c"Mapped:", ext.wrapping_mul(ps));
    mem_kb(s, c"Shmem:", 0);
    mem_kb(s, c"KReclaimable:", 0);
    mem_kb(s, c"Slab:", 0);
    mem_kb(s, c"SReclaimable:", 0);
    mem_kb(s, c"SUnreclaim:", 0);
    mem_kb(s, c"KernelStack:", 0);
    mem_kb(s, c"PageTables:", 0);
    // overcommit_memory is reported as 0 (heuristic), and under mode 0 Linux's
    // CommitLimit is advisory; RAM + swap is the figure that matches the
    // behaviour Darwin actually has, which is to overcommit freely.
    mem_kb(s, c"CommitLimit:", total.wrapping_add(xsu.xsu_total));
    // Compressed pages are charged here rather than being counted as free:
    // they are live anonymous memory that happens to be squeezed, and Linux
    // has no equivalent state to put them in.
    mem_kb(
        s,
        c"Committed_AS:",
        internal.wrapping_add(compressed).wrapping_mul(ps),
    );
    // A constant on Linux too -- the size of the vmalloc arena, not a
    // measurement, and nothing in Steam or pressure-vessel parses it. The
    // figure here is the one MEASURED on this project's own aarch64 guest
    // (6.17.1-300.fc43.aarch64, 48-bit VA): VmallocTotal: 135288315904 kB. The
    // value this line used to print, 34359738367, is the x86_64 answer and was
    // asserted in the comment as "what x86_64 and arm64 both print", which the
    // guest disproves. An arm64 /proc must not carry an x86_64 constant with a
    // verification claim attached to it.
    sbf!(
        s,
        c"%-15s %8llu kB\n",
        c"VmallocTotal:".as_ptr(),
        135_288_315_904u64 as c_ulonglong
    );
    mem_kb(s, c"VmallocUsed:", 0);
    mem_kb(s, c"VmallocChunk:", 0);
    // Hugepage counts print WITHOUT the kB suffix on Linux; only the size does.
    sbf!(s, c"HugePages_Total:   %5d\n", 0 as c_int);
    sbf!(s, c"HugePages_Free:    %5d\n", 0 as c_int);
    sbf!(s, c"HugePages_Rsvd:    %5d\n", 0 as c_int);
    sbf!(s, c"HugePages_Surp:    %5d\n", 0 as c_int);
    // 2048 kB is the x86_64 and 4K-page arm64 answer (confirmed on the
    // project's guest, which runs a 4 KiB granule and prints exactly that),
    // and it is wrong here. A level-2 block covers granule * (granule / 8)
    // bytes, because an 8-byte descriptor fills one table: 4096 * 512 = 2 MiB
    // on a 4 KiB granule, 16384 * 2048 = 32 MiB on the 16 KiB one Darwin uses.
    // Derived from the GUEST granule, not the host's, because the thing it
    // must not contradict is the AT_PAGESZ this same process published.
    let gps = guest_page_bytes();
    sbf!(
        s,
        c"%-15s %8llu kB\n",
        c"Hugepagesize:".as_ptr(),
        (gps.wrapping_mul(gps / 8) / 1024) as c_ulonglong
    );
    mem_kb(s, c"Hugetlb:", 0);
    sb_done(s)
}

// ------------------------------------------------------------ /proc/cpuinfo

// The CPU identity is taken from the same synthetic MIDR_EL1 that sysreg.rs
// hands back for `mrs Xt, MIDR_EL1` (0xd5380000: op0=3 op1=0 CRn=0 CRm=0
// op2=0, which is the encoding that switch matches). Deriving it here instead
// of hardcoding means /proc/cpuinfo and the register the guest can read on any
// thread cannot disagree -- and they would, silently, the first time either is
// edited.
//
// A consequence worth stating: Apple Silicon is heterogeneous (measured here,
// hw.perflevel0.logicalcpu = 4 performance cores and
// hw.perflevel1.logicalcpu = 6 efficiency cores) and real Linux on such a
// machine prints two different `CPU part` values. This prints one, because
// there is only one synthetic MIDR and a guest that read the register on an
// E-core would otherwise catch the file lying.
const MIDR_MRS_INSN: u32 = 0xd538_0000;

unsafe fn gen_cpuinfo(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let midr = lxrt_synthetic_sysreg(MIDR_MRS_INSN);
    let implementer = ((midr >> 24) & 0xff) as c_uint;
    let variant = ((midr >> 20) & 0xf) as c_uint;
    let partnum = ((midr >> 4) & 0xfff) as c_uint;
    let revision = (midr & 0xf) as c_uint;

    // arm64 Linux derives BogoMIPS from the architected timer, not from a
    // delay loop: bogomips = arch_timer_rate / 500000. Apple's timer is
    // 24 MHz (hw.tbfrequency, measured), which gives the 48.00 that Asahi
    // prints on the same silicon.
    let tb = sctl_u64(c"hw.tbfrequency", 24_000_000);
    let bogo_int = tb / 500_000;
    let bogo_frac = (tb % 500_000) * 100 / 500_000;

    // The clamp and the buffer used to disagree: each record below measures
    // ~203 bytes, so 1024 processors need ~208 KB against a GEN_MAX of 128 KB,
    // and above ~645 the file was published with its last record cut off
    // mid-line. The clamp stays at 1024 -- it is a sanity bound on a sysctl,
    // not a capacity statement -- and materialise() now grows the buffer until
    // the generator stops reporting truncation, so the two cannot drift apart
    // again.
    let mut ncpu = sctl_u64(c"hw.logicalcpu", sctl_u64(c"hw.ncpu", 1));
    if ncpu < 1 {
        ncpu = 1;
    }
    if ncpu > 1024 {
        ncpu = 1024;
    }

    let mut i: u64 = 0;
    while i < ncpu {
        sbf!(s, c"processor\t: %llu\n", i as c_ulonglong);
        sbf!(
            s,
            c"BogoMIPS\t: %llu.%02llu\n",
            bogo_int as c_ulonglong,
            bogo_frac as c_ulonglong
        );
        // This list is not the hardware's full capability set: it is exactly
        // the HWCAP bits stack.rs puts in AT_HWCAP. glibc reads one and Steam's
        // hardware survey reads the other, and a capability named here but
        // absent from AT_HWCAP is how an ifunc gets selected for an
        // instruction the runtime never agreed to support. `cpuid` follows
        // HWCAP_CPUID (stack.rs): ID register reads are answered, rewritten
        // or emulated from SIGILL.
        let cpuid: &CStr = if lxrt_hwcap_cpuid() { c" cpuid" } else { c"" };
        sbf!(
            s,
            c"Features\t: fp asimd aes pmull sha1 sha2 crc32 atomics%s asimdrdm\n",
            cpuid.as_ptr()
        );
        sbf!(s, c"CPU implementer\t: 0x%02x\n", implementer);
        // Printed with no tab and always 8: the arm64 kernel hardcodes this
        // line, because MIDR's architecture field reads 0xf ("consult the ID
        // registers") on every ARMv8 part and would be useless here.
        sbf!(s, c"CPU architecture: 8\n");
        sbf!(s, c"CPU variant\t: 0x%x\n", variant);
        sbf!(s, c"CPU part\t: 0x%03x\n", partnum);
        sbf!(s, c"CPU revision\t: %u\n", revision);
        sbf!(s, c"\n");
        i += 1;
    }
    sb_done(s)
}

// --------------------------------------------------------------- /proc/stat

unsafe fn gen_stat(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let mut cl = MaybeUninit::<CpuLoadInfo>::zeroed();
    let have = cpu_load(cl.as_mut_ptr());
    let cl = &*cl.as_ptr();
    let u: c_ulonglong = if have {
        cl.cpu_ticks[CPU_STATE_USER] as c_ulonglong
    } else {
        0
    };
    let sy: c_ulonglong = if have {
        cl.cpu_ticks[CPU_STATE_SYSTEM] as c_ulonglong
    } else {
        0
    };
    let id: c_ulonglong = if have {
        cl.cpu_ticks[CPU_STATE_IDLE] as c_ulonglong
    } else {
        0
    };
    let ni: c_ulonglong = if have {
        cl.cpu_ticks[CPU_STATE_NICE] as c_ulonglong
    } else {
        0
    };

    // Two spaces after the aggregate "cpu", one after "cpuN" -- Linux's own
    // format, and awk '$1=="cpu"' scripts depend on the field not running into
    // the first number. iowait, irq, softirq, steal, guest and guest_nice are
    // zero because Darwin folds all of them into system time; there is no
    // breakdown to recover.
    sbf!(s, c"cpu  %llu %llu %llu %llu 0 0 0 0 0 0\n", u, ni, sy, id);

    let mut ncpu: u32 = 0;
    let mut pinfo: *mut i32 = ptr::null_mut();
    let mut pcount: u32 = 0;
    if host_processor_info(
        host_port(),
        PROCESSOR_CPU_LOAD_INFO,
        &mut ncpu,
        &mut pinfo,
        &mut pcount,
    ) == KERN_SUCCESS
    {
        let p: *const CpuLoadInfo = pinfo.cast();
        let mut i: u32 = 0;
        while i < ncpu {
            let ticks = &(*p.add(i as usize)).cpu_ticks;
            sbf!(
                s,
                c"cpu%u %u %u %u %u 0 0 0 0 0 0\n",
                i as c_uint,
                ticks[CPU_STATE_USER] as c_uint,
                ticks[CPU_STATE_NICE] as c_uint,
                ticks[CPU_STATE_SYSTEM] as c_uint,
                ticks[CPU_STATE_IDLE] as c_uint
            );
            i += 1;
        }
        vm_deallocate(
            mach_task_self(),
            pinfo as usize,
            (pcount as usize).wrapping_mul(size_of::<i32>()),
        );
    }

    sbf!(s, c"intr 0\n");
    // Darwin publishes no system-wide context-switch counter to unprivileged
    // userspace; TASK_EVENTS_INFO gives this task's. Emitting that rather than
    // a frozen 0 keeps the value monotonic, which is all anything reading
    // /proc/stat's ctxt actually does with it (a rate). It is a lower bound on
    // the system figure, and labelled as such here so nobody later reads it as
    // machine-wide.
    let mut te = MaybeUninit::<TaskEventsInfo>::zeroed();
    let ctxt: c_ulonglong = if task_events(te.as_mut_ptr()) {
        nonneg((*te.as_ptr()).csw) as c_ulonglong
    } else {
        0
    };
    sbf!(s, c"ctxt %llu\n", ctxt);
    let btime = boot_time_sec() as c_ulonglong;
    sbf!(s, c"btime %llu\n", btime);
    // Forks since boot. No Darwin equivalent at all -- not approximable, so 0
    // rather than something invented. Anything computing a fork rate gets zero,
    // which is visibly wrong rather than plausibly wrong.
    sbf!(s, c"processes 0\n");
    sbf!(s, c"procs_running 1\n");
    sbf!(s, c"procs_blocked 0\n");
    sbf!(s, c"softirq 0\n");
    sb_done(s)
}

// ------------------------------------------------------------- /proc/vmstat

unsafe fn gen_vmstat(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let mut vm = MaybeUninit::<VmStatistics64>::uninit();
    host_vm_stats(vm.as_mut_ptr()); // zeroes the struct on failure too
    let vm = &*vm.as_ptr();
    let ps = host_page_bytes();

    // nr_* are page counts that the guest converts with sysconf(_SC_PAGESIZE),
    // i.e. AT_PAGESZ, so they are restated in the guest's granule rather than
    // Darwin's. Same speculative correction as meminfo: speculative pages are
    // already inside free_count (mach/vm_statistics.h:114), so adding them
    // overstated nr_free_pages by the whole speculative cache.
    let freep: u64 = if vm.free_count as u64 > vm.speculative_count as u64 {
        vm.free_count as u64 - vm.speculative_count as u64
    } else {
        0
    };
    let v = host_pages_to_guest(freep) as c_ulonglong;
    sbf!(s, c"nr_free_pages %llu\n", v);
    let v = host_pages_to_guest(vm.external_page_count as u64) as c_ulonglong;
    sbf!(s, c"nr_file_pages %llu\n", v);
    let v = host_pages_to_guest(vm.internal_page_count as u64) as c_ulonglong;
    sbf!(s, c"nr_anon_pages %llu\n", v);
    let v = host_pages_to_guest(vm.external_page_count as u64) as c_ulonglong;
    sbf!(s, c"nr_mapped %llu\n", v);
    let v = host_pages_to_guest(vm.wire_count as u64) as c_ulonglong;
    sbf!(s, c"nr_unevictable %llu\n", v);
    sbf!(s, c"nr_dirty 0\n");
    sbf!(s, c"nr_writeback 0\n");
    sbf!(s, c"nr_slab_reclaimable 0\n");
    sbf!(s, c"nr_slab_unreclaimable 0\n");
    // pgpgin and pgpgout are the trap in this file: Linux counts them in
    // KILOBYTES while pswpin/pswpout and every nr_* right above are in pages.
    // The unit is MEASURED, not inferred from the kernel source -- the earlier
    // "the block layer feeds it sectors >> 1" gloss named a mechanism that
    // submit_bio does not obviously perform, and the reviewer was right to
    // distrust it. On the project's aarch64 guest, drop_caches then an
    // O_DIRECT read of 256 MiB off a real block device moved pgpgin by 263636.
    // 256 MiB is 262144 KiB and 524288 512-byte sectors, so the counter is in
    // KiB (0.6% over, which is the rest of the system's I/O) and definitively
    // not in sectors. Darwin's vm.pageins is a page count in the host granule,
    // so page_count * host_page / 1024 is the conversion; leaving it unscaled
    // would understate paging by the page size and look merely "low".
    sbf!(
        s,
        c"pgpgin %llu\n",
        vm.pageins.wrapping_mul(ps / 1024) as c_ulonglong
    );
    sbf!(
        s,
        c"pgpgout %llu\n",
        vm.pageouts.wrapping_mul(ps / 1024) as c_ulonglong
    );
    sbf!(s, c"pswpin %llu\n", vm.swapins as c_ulonglong);
    sbf!(s, c"pswpout %llu\n", vm.swapouts as c_ulonglong);
    sbf!(s, c"pgfault %llu\n", vm.faults as c_ulonglong);
    // Darwin's "pageins" is a fault that had to touch storage, which is
    // precisely Linux's major fault.
    sbf!(s, c"pgmajfault %llu\n", vm.pageins as c_ulonglong);
    sbf!(s, c"pgactivate %llu\n", vm.reactivations as c_ulonglong);
    sbf!(s, c"pgfree %llu\n", vm.zero_fill_count as c_ulonglong);
    sb_done(s)
}

// ------------------------------------------------------------ /proc/version

// Held to dispatch.c's do_uname(): release "6.6.0-lxrt", version "#1 SMP
// lxrt". Something that reads both and compares them -- and glibc's
// dl_discover_osversion does read /proc/version when uname's release fails to
// parse -- must not find two different kernels.
const LXRT_OSRELEASE: &CStr = c"6.6.0-lxrt";
const LXRT_OSVERSION: &CStr = c"#1 SMP lxrt";

unsafe fn gen_version(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    // The C pasted the two macros into the format string; they contain no
    // conversion, so passing them as %s arguments yields the same bytes.
    sbf!(
        s,
        c"Linux version %s (lxrt@darwin) (clang) %s\n",
        LXRT_OSRELEASE.as_ptr(),
        LXRT_OSVERSION.as_ptr()
    );
    sb_done(s)
}

// -------------------------------------------- /proc/mounts, self/mountinfo

// Both files describe a Linux filesystem tree that does not exist, and that is
// the deliberate choice. getmntinfo(3) would give the real answer -- apfs on
// /System/Volumes/Data, firmlinks, /dev/disk3s5 -- and srt-bwrap reads
// mountinfo specifically to decide what to bind into its container. Feeding it
// Darwin volume paths makes it try to bind host paths that mean nothing inside
// the guest, and it fails with a message about the host's disk layout rather
// than about the real problem. A minimal, plausible Linux tree makes it take
// the ordinary path.
//
// Getting the shape right matters more than the contents: bwrap parses
// mountinfo positionally and the optional-field list before the "-" separator
// is variable-length, so the "-" must be there even when there are no optional
// fields.
unsafe fn gen_mounts(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    sbf!(s, c"/dev/root / ext4 rw,relatime 0 0\n");
    sbf!(s, c"devtmpfs /dev devtmpfs rw,nosuid,relatime 0 0\n");
    sbf!(
        s,
        c"devpts /dev/pts devpts rw,nosuid,noexec,relatime,gid=5,mode=620,ptmxmode=666 0 0\n"
    );
    sbf!(s, c"tmpfs /dev/shm tmpfs rw,nosuid,nodev 0 0\n");
    sbf!(s, c"proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n");
    sbf!(s, c"sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n");
    sbf!(s, c"tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n");
    sb_done(s)
}

unsafe fn gen_mountinfo(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    // id parent major:minor root mountpoint options [optional...] - fstype
    // source superopts. The root entry is parent 1 (itself, by convention for
    // the mount namespace root) and carries a peer group so that a caller
    // checking propagation does not conclude the tree is unshared already.
    sbf!(
        s,
        c"23 1 254:0 / / rw,relatime shared:1 - ext4 /dev/root rw\n"
    );
    sbf!(
        s,
        c"24 23 0:5 / /dev rw,nosuid,relatime shared:2 - devtmpfs devtmpfs rw\n"
    );
    sbf!(
        s,
        c"25 24 0:6 / /dev/pts rw,nosuid,noexec,relatime shared:3 - devpts devpts rw,gid=5,mode=620,ptmxmode=666\n"
    );
    sbf!(
        s,
        c"26 24 0:7 / /dev/shm rw,nosuid,nodev shared:4 - tmpfs tmpfs rw\n"
    );
    sbf!(
        s,
        c"27 23 0:8 / /proc rw,nosuid,nodev,noexec,relatime shared:5 - proc proc rw\n"
    );
    sbf!(
        s,
        c"28 23 0:9 / /sys rw,nosuid,nodev,noexec,relatime shared:6 - sysfs sysfs rw\n"
    );
    sbf!(
        s,
        c"29 23 0:10 / /tmp rw,nosuid,nodev shared:7 - tmpfs tmpfs rw\n"
    );
    sb_done(s)
}

// ------------------------------------------------- /proc/uptime, /proc/loadavg

unsafe fn gen_uptime(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let up: f64 = uptime_sec();
    let mut cl = MaybeUninit::<CpuLoadInfo>::zeroed();
    let idle: f64 = if cpu_load(cl.as_mut_ptr()) {
        (*cl.as_ptr()).cpu_ticks[CPU_STATE_IDLE] as f64 / 100.0
    } else {
        0.0
    };
    // Linux's second figure is idle time summed over all CPUs, so on an
    // n-core machine it legitimately exceeds the first. Darwin's aggregate
    // host_cpu_load_info is summed the same way, so no division is wanted.
    sbf!(s, c"%.2f %.2f\n", up as c_double, idle as c_double);
    sb_done(s)
}

unsafe fn gen_loadavg(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let mut la: [c_double; 3] = [0.0, 0.0, 0.0];
    if getloadavg(la.as_mut_ptr(), 3) != 3 {
        la = [0.0, 0.0, 0.0];
    }

    // The "running/total" pair counts TASKS on Linux -- threads, not processes.
    // Darwin's KERN_PROC_ALL is a process count, so this understates the
    // denominator on any threaded system. The size-only sysctl probe is used
    // because it needs no buffer, and the kernel pads it, so the count is an
    // upper estimate of processes and a lower one of tasks.
    let mut total: c_uint = 1;
    let mut mib: [c_int; 3] = [CTL_KERN, KERN_PROC, KERN_PROC_ALL];
    let mut sz: usize = 0;
    if sysctl(
        mib.as_mut_ptr(),
        3,
        ptr::null_mut(),
        &mut sz,
        ptr::null_mut(),
        0,
    ) == 0
        && sz >= KINFO_PROC_SIZE
    {
        total = (sz / KINFO_PROC_SIZE) as c_uint;
    }

    // The runnable count is 1: this thread. Darwin publishes no system-wide
    // run-queue depth, and counting our own threads would count sleeping ones.
    let pid: c_int = lxrt_ids_pid();
    sbf!(
        s,
        c"%.2f %.2f %.2f 1/%u %d\n",
        la[0],
        la[1],
        la[2],
        total,
        pid
    );
    sb_done(s)
}

// ------------------------------------------- /proc/self/stat, /proc/self/statm

// Darwin's time_value_t is seconds plus microseconds; Linux's stat fields are
// in clock ticks. sysconf(_SC_CLK_TCK) is 100 on both (measured), and it is
// also what stack.rs publishes as AT_CLKTCK, so a guest calling
// sysconf(_SC_CLK_TCK) to divide these gets a consistent answer.
const LXRT_CLK_TCK: u64 = 100;

fn tv_to_ticks(t: TimeValue) -> u64 {
    (t.seconds as u64)
        .wrapping_mul(LXRT_CLK_TCK)
        .wrapping_add(t.microseconds as u64 / (1_000_000 / LXRT_CLK_TCK))
}

// Ticks since boot at which this process started. kinfo_proc carries the
// absolute start time as a timeval, so the subtraction against kern.boottime
// is the whole of it. A zero here makes `ps -o etime` style code inside the
// guest report an uptime-long lifetime, which is why it is measured.
unsafe fn start_ticks() -> u64 {
    let mut kp = MaybeUninit::<KinfoProc>::zeroed();
    let mut len: usize = KINFO_PROC_SIZE;
    let mut mib: [c_int; 4] = [CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()];
    if sysctl(
        mib.as_mut_ptr(),
        4,
        kp.as_mut_ptr().cast(),
        &mut len,
        ptr::null_mut(),
        0,
    ) != 0
        || len == 0
    {
        return 0;
    }
    let kp = &*kp.as_ptr();
    let bt = boot_time_sec();
    let st = kp.p_starttime.tv_sec as u64;
    if bt == 0 || st < bt {
        return 0;
    }
    (st - bt)
        .wrapping_mul(LXRT_CLK_TCK)
        .wrapping_add(kp.p_starttime.tv_usec as u64 / (1_000_000 / LXRT_CLK_TCK))
}

// lxrt_ids_on() ? (lxrt_ids_to_guest(pid, 0) ?: 1) : (int)pid
unsafe fn guest_pid_or_one(host_pid: c_int) -> c_int {
    if lxrt_ids_on() {
        let guest = lxrt_ids_to_guest(host_pid, 0);
        if guest != 0 {
            guest
        } else {
            1
        }
    } else {
        host_pid
    }
}

// char comm[16]; snprintf(comm, sizeof comm, "%s", nm ? nm : "lxrt");
unsafe fn fill_comm(comm: *mut c_char) {
    let nm = getprogname();
    snprintf(
        comm,
        16,
        c"%s".as_ptr(),
        if !nm.is_null() { nm } else { c"lxrt".as_ptr() },
    );
}

unsafe fn gen_self_stat(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let mut tb = MaybeUninit::<MachTaskBasicInfo>::uninit();
    if !task_basic(tb.as_mut_ptr()) {
        ptr::write_bytes(tb.as_mut_ptr(), 0, 1);
    }
    let tb = &*tb.as_ptr();
    let mut tt = MaybeUninit::<TaskThreadTimesInfo>::uninit();
    if !task_thread_times(tt.as_mut_ptr()) {
        ptr::write_bytes(tt.as_mut_ptr(), 0, 1);
    }
    let tt = &*tt.as_ptr();
    let mut te = MaybeUninit::<TaskEventsInfo>::uninit();
    if !task_events(te.as_mut_ptr()) {
        ptr::write_bytes(te.as_mut_ptr(), 0, 1);
    }
    let te = &*te.as_ptr();

    // Fields 14 and 15. tb carries only the threads that have EXITED and tt
    // only the ones still running, so the process total is the sum -- see the
    // comment on task_thread_times(). Either half alone is a lie, and the
    // terminated half alone is the lie that reads as zero.
    let utime = tv_to_ticks(tb.user_time).wrapping_add(tv_to_ticks(tt.user_time));
    let stime = tv_to_ticks(tb.system_time).wrapping_add(tv_to_ticks(tt.system_time));

    // Linux truncates comm to TASK_COMM_LEN-1 = 15 and wraps it in parens;
    // anything reading field 2 scans to the LAST ')' precisely because the
    // name may contain one, so a long name here is a compatibility hazard
    // rather than a cosmetic one.
    let mut comm: [c_char; 16] = [0; 16];
    fill_comm(comm.as_mut_ptr());

    // rss is a page count the guest scales by AT_PAGESZ; resident_size is
    // bytes, so the divisor is the guest granule.
    let ps = guest_page_bytes();
    let faults = nonneg(te.faults);
    let major = nonneg(te.pageins);
    let minflt = if faults > major { faults - major } else { 0 };

    // Field 25, rsslim, is unconditionally Linux's "unlimited".
    //
    // This used to be getrlimit(RLIMIT_RSS), which on Darwin is not an RSS
    // limit at all: sys/resource.h defines RLIMIT_RSS as RLIMIT_AS, literally
    // the same value 5, so the call returns the ADDRESS-SPACE ceiling. Under
    // `ulimit -v` that would publish a false RSS cap under an RSS label, and a
    // guest allocator that honours rsslim would throttle itself against a
    // number that means something else. Darwin cannot express an RSS limit, so
    // the honest answer is the one Linux gives when there is none.
    //
    // The value matters as much as the source: RLIM_INFINITY is NOT the same
    // number on the two systems -- Darwin's is INT64_MAX
    // (0x7fffffffffffffff, measured), Linux's is UINT64_MAX. Linux prints
    // 18446744073709551615 here, and a guest comparing the field against its
    // own RLIM_INFINITY would read the Darwin value as a finite limit of
    // roughly 9 exabytes.
    let rsslim: c_ulonglong = !0;

    // Field 38, exit_signal: Linux's SIGCHLD is 17, Darwin's is 20. The Linux
    // number goes in the Linux file. Field 41, policy: Linux SCHED_OTHER is 0,
    // Darwin's is 1 (measured), so again the Linux number.
    // Fields 26, 27, 29 and 30 (startcode, endcode, kstkesp, kstkeip) are 0,
    // as Linux shows them to a reader that may not ptrace the process.
    // Field 28, startstack, is not: bionic's pthread_getattr_np() of the main
    // thread finds its stack by looking this address up in /proc/self/maps
    // and aborts when nothing contains it ("Stack not found in
    // /proc/self/maps": ART, benchmarks/stage25-art-x86-fex.txt). Linux shows
    // it to the process itself: the initial stack pointer (main.c), or, for
    // an x86 program under FEX, an address in FEX's stack for it (dispatch.c,
    // lxrt_note_growsdown).
    let pid: c_int = lxrt_ids_pid();
    let ppid: c_int = guest_pid_or_one(getppid());
    let pgrp: c_int = guest_pid_or_one(getpgrp());
    let sid: c_int = guest_pid_or_one(getsid(0));
    let threads: c_uint = thread_count();
    let start = start_ticks() as c_ulonglong;
    let vsize = virtual_size_linuxish() as c_ulonglong;
    let rss = udiv(tb.resident_size, ps) as c_ulonglong;
    let start_stack = lxrt_start_stack as c_ulonglong;
    sbf!(
        s,
        // 1-9
        // 10-13 minflt cminflt majflt cmajflt
        // 14-17 utime stime cutime cstime
        // 18-22 prio nice threads itreal start
        // 23-25 vsize rss rsslim
        // 26-30 (28 startstack)
        // 31-35 signal..wchan
        // 36-41 nswap cnswap exit_signal processor rt_prio policy
        // 42-44 blkio guest cguest
        // 45-52
        c"%d (%s) R %d %d %d 0 -1 0 \
          %llu 0 %llu 0 \
          %llu %llu 0 0 \
          20 0 %u 0 %llu \
          %llu %llu %llu \
          0 0 %llu 0 0 \
          0 0 0 0 0 \
          0 0 17 0 0 0 \
          0 0 0 \
          0 0 0 0 0 0 0 0\n",
        pid,
        comm.as_ptr(),
        ppid,
        pgrp,
        sid,
        minflt as c_ulonglong,
        major as c_ulonglong,
        utime as c_ulonglong,
        stime as c_ulonglong,
        threads,
        start,
        vsize,
        rss,
        rsslim,
        start_stack
    );
    sb_done(s)
}

// /proc/self/task/<tid>/stat: the process's line with that thread's id, state
// and CPU times. Wine's GetThreadTimes reads it for every thread but the
// caller (and SystemProcessInformation for every thread); without it a thread
// had used no CPU, ever. Minecraft Dungeons II's protection reads its worker
// threads' times from a watchdog thread about two minutes in, and closed the
// game -- or corrupted its own state -- when they had not moved (MEASURED;
// benchmarks/stage62, 15 m). 0 when the thread is not (or no longer) there.
//
// It has to be cheap: the same protection asks several thousand times a
// second. The process-wide fields (vsize walks every VM region -- tens of
// thousands in a game -- threads, rss, start time) are refreshed at most once
// a second; the per-thread CPU times are read every time (MEASURED: computing
// vsize per read slowed the game's start fivefold and it never drew a frame).
#[no_mangle]
pub unsafe extern "C" fn lxrt_proc_gen_task_stat(tid: c_int, b: *mut c_char, cap: usize) -> usize {
    let mut user_us: u64 = 0;
    let mut sys_us: u64 = 0;
    let mut running: bool = false;
    if !lxrt_thread_times(tid, &mut user_us, &mut sys_us, &mut running) {
        return 0;
    }
    static C_WHEN: AtomicU64 = AtomicU64::new(0);
    static C_VSIZE: AtomicU64 = AtomicU64::new(0);
    static C_RSS: AtomicU64 = AtomicU64::new(0);
    static C_START: AtomicU64 = AtomicU64::new(0);
    static C_THREADS: AtomicU32 = AtomicU32::new(0);
    let now = mach_absolute_time();
    let mut tbi = MachTimebaseInfo { numer: 0, denom: 0 };
    mach_timebase_info(&mut tbi);
    let sec = udiv(
        1_000_000_000u64.wrapping_mul(tbi.denom as u64),
        tbi.numer as u64,
    );
    let when = C_WHEN.load(Ordering::SeqCst);
    if when == 0 || now.wrapping_sub(when) > sec {
        C_WHEN.store(now, Ordering::SeqCst);
        let mut tb = MaybeUninit::<MachTaskBasicInfo>::uninit();
        if !task_basic(tb.as_mut_ptr()) {
            ptr::write_bytes(tb.as_mut_ptr(), 0, 1);
        }
        let resident_size = (*tb.as_ptr()).resident_size;
        C_RSS.store(udiv(resident_size, guest_page_bytes()), Ordering::SeqCst);
        C_VSIZE.store(virtual_size_linuxish(), Ordering::SeqCst);
        C_THREADS.store(thread_count(), Ordering::SeqCst);
        if C_START.load(Ordering::SeqCst) == 0 {
            C_START.store(start_ticks(), Ordering::SeqCst);
        }
    }
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let mut comm: [c_char; 16] = [0; 16];
    fill_comm(comm.as_mut_ptr());
    let ppid: c_int = guest_pid_or_one(getppid());
    let pgrp: c_int = guest_pid_or_one(getpgrp());
    let sid: c_int = guest_pid_or_one(getsid(0));
    let threads: c_uint = C_THREADS.load(Ordering::SeqCst);
    let start = C_START.load(Ordering::SeqCst) as c_ulonglong;
    let vsize = C_VSIZE.load(Ordering::SeqCst) as c_ulonglong;
    let rss = C_RSS.load(Ordering::SeqCst) as c_ulonglong;
    let start_stack = lxrt_start_stack as c_ulonglong;
    sbf!(
        s,
        // 1-9
        // 10-13
        // 14-17 utime stime cutime cstime
        // 18-22
        // 23-25
        // 26-30
        // 31-35
        // 36-41
        // 42-44
        // 45-52
        c"%d (%s) %c %d %d %d 0 -1 0 \
          0 0 0 0 \
          %llu %llu 0 0 \
          20 0 %u 0 %llu \
          %llu %llu %llu \
          0 0 %llu 0 0 \
          0 0 0 0 0 \
          0 0 17 0 0 0 \
          0 0 0 \
          0 0 0 0 0 0 0 0\n",
        tid,
        comm.as_ptr(),
        (if running { b'R' } else { b'S' }) as c_int,
        ppid,
        pgrp,
        sid,
        (user_us / (1_000_000 / LXRT_CLK_TCK)) as c_ulonglong,
        (sys_us / (1_000_000 / LXRT_CLK_TCK)) as c_ulonglong,
        threads,
        start,
        vsize,
        rss,
        !0 as c_ulonglong,
        start_stack
    );
    sb_done(s)
}

// /proc/self/status, regenerated on each lookup so its ids are the current
// ones: with Android ids (android_ids.h) those are the virtual ids, which
// change as a zygote child becomes an app. procfs.c's copy, written at start,
// can only hold the ids of that moment.
unsafe fn gen_self_status(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let mut u = MaybeUninit::<[u32; 4]>::uninit();
    let u: *mut u32 = u.as_mut_ptr().cast();
    let mut g = MaybeUninit::<[u32; 4]>::uninit();
    let g: *mut u32 = g.as_mut_ptr().cast();
    let mut groups = MaybeUninit::<[c_char; 1024]>::uninit();
    let groups: *mut c_char = groups.as_mut_ptr().cast();
    lxrt_aids_status_ids(u, g, groups, 1024);
    let mut comm: [c_char; 16] = [0; 16];
    fill_comm(comm.as_mut_ptr());
    let tgid: c_int = lxrt_ids_pid();
    let pid: c_int = lxrt_ids_pid();
    let ppid: c_int = guest_pid_or_one(getppid());
    let threads: c_uint = thread_count();
    sbf!(
        s,
        c"Name:\t%s\nUmask:\t0022\nState:\tR (running)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\n\
          TracerPid:\t0\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nFDSize:\t256\nGroups:\t%s\n\
          Threads:\t%u\n",
        comm.as_ptr(),
        tgid,
        pid,
        ppid,
        *u.add(0) as c_uint,
        *u.add(1) as c_uint,
        *u.add(2) as c_uint,
        *u.add(3) as c_uint,
        *g.add(0) as c_uint,
        *g.add(1) as c_uint,
        *g.add(2) as c_uint,
        *g.add(3) as c_uint,
        groups as *const c_char,
        threads
    );
    sb_done(s)
}

unsafe fn gen_self_statm(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let mut tb = MaybeUninit::<MachTaskBasicInfo>::uninit();
    if !task_basic(tb.as_mut_ptr()) {
        ptr::write_bytes(tb.as_mut_ptr(), 0, 1);
    }
    let tb = &*tb.as_ptr();
    let ps = guest_page_bytes();
    let size = udiv(virtual_size_linuxish(), ps);
    let res = udiv(tb.resident_size, ps);
    // size resident shared text lib data dt, all in pages. `lib` has been
    // hardwired to 0 in Linux since 2.6 and `dt` since 2.6.22, so those two
    // zeroes are what a real kernel prints, not a gap. `shared` and `text`
    // would need per-mapping accounting Darwin does not expose; `data` is the
    // resident set minus a text figure we do not have, so it equals resident.
    sbf!(
        s,
        c"%llu %llu 0 0 0 %llu 0\n",
        size as c_ulonglong,
        res as c_ulonglong,
        res as c_ulonglong
    );
    sb_done(s)
}

// -------------------------------------------------------- /proc/self/auxv

// Auxiliary vector types, matching stack.rs's enum. Duplicated rather than
// shared because stack.rs does not export them, and they are fixed ABI.
const AX_NULL: u64 = 0;
const AX_PAGESZ: u64 = 6;
const AX_UID: u64 = 11;
const AX_EUID: u64 = 12;
const AX_GID: u64 = 13;
const AX_EGID: u64 = 14;
const AX_HWCAP: u64 = 16;
const AX_CLKTCK: u64 = 17;
const AX_SECURE: u64 = 23;
const AX_HWCAP2: u64 = 26;

// Must stay equal to the AT_HWCAP stack.rs publishes. A guest that reads
// getauxval(AT_HWCAP) after a re-exec gets it from this file rather than from
// the stack, so a divergence between the two shows up only in re-executed
// children -- the late, symptomless failure this codebase keeps meeting.
unsafe fn lxrt_auxv_hwcap() -> u32 {
    (1u32 << 0)
        | (1u32 << 1)
        | (1u32 << 3)
        | (1u32 << 4)
        | (1u32 << 5)
        | (1u32 << 6)
        | (1u32 << 7)
        | (1u32 << 8)
        | (1u32 << 12)
        | (if lxrt_hwcap_cpuid() { 1u32 << 11 } else { 0 })
}

// The real vector, if the integrator handed it over. 64 pairs is generous:
// stack.rs writes 17.
//
// The copy is staged in a local and committed only once the whole vector has
// been read successfully. Writing straight into g_auxv[] meant a mid-vector
// EFAULT returned -14 having already clobbered the published vector while
// g_auxv_words still named the OLD length: reproduced, a failed 4-pair call
// left /proc/self/auxv reading 6/4096, 16/0xdead, 17/250, 25/0xaaaaaaaa, 0/0
// -- the failed vector's first three pairs spliced onto the previous vector's
// tail, AT_PAGESZ=4096 included, which contradicts the 16384 this runtime
// actually publishes. A caller that checks the return value and falls back
// deserves either the old truth or the measured fallback, never a splice of
// the two.
//
// The mutex covers readers as well as writers. An _Atomic length published
// last would order the length against the array for one writer, but two
// set_auxv calls can still interleave their memcpys under a reader; the lock
// is held only across a memcpy of at most 1 KB, on a path that already does
// file I/O.
const AUXV_MAX_PAIRS: usize = 64;
static mut G_AUXV: [u64; AUXV_MAX_PAIRS * 2] = [0; AUXV_MAX_PAIRS * 2];
static mut G_AUXV_WORDS: usize = 0;
static mut G_AUXV_LOCK: PthreadMutex = PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
};

// LXRT_FORK_SAFE(proc_ext_g_auxv_lock, g_auxv_lock): taken before fork,
// released after it in both parent and child (the child's only thread is the
// one that took it).
extern "C" fn proc_ext_g_auxv_lock_fork_prepare() {
    unsafe {
        pthread_mutex_lock(ptr::addr_of_mut!(G_AUXV_LOCK));
    }
}
extern "C" fn proc_ext_g_auxv_lock_fork_release() {
    unsafe {
        pthread_mutex_unlock(ptr::addr_of_mut!(G_AUXV_LOCK));
    }
}
extern "C" fn proc_ext_g_auxv_lock_fork_register() {
    unsafe {
        pthread_atfork(
            Some(proc_ext_g_auxv_lock_fork_prepare),
            Some(proc_ext_g_auxv_lock_fork_release),
            Some(proc_ext_g_auxv_lock_fork_release),
        );
    }
}
#[used]
#[link_section = "__DATA,__mod_init_func"]
static PROC_EXT_G_AUXV_LOCK_FORK_REGISTER: extern "C" fn() = proc_ext_g_auxv_lock_fork_register;

#[no_mangle]
pub unsafe extern "C" fn lxrt_proc_ext_set_auxv(auxv: *const c_void) -> c_long {
    if auxv.is_null() {
        return lerr(EFAULT);
    }

    let mut tmp = MaybeUninit::<[u64; AUXV_MAX_PAIRS * 2]>::uninit();
    let tmp: *mut u64 = tmp.as_mut_ptr().cast();
    let mut pair: [u64; 2] = [0; 2];
    let src: *const u64 = auxv.cast();
    let mut words: usize = 0;
    let mut i: usize = 0;
    while i < AUXV_MAX_PAIRS {
        if !safe_copy_in(
            pair.as_mut_ptr().cast(),
            src.wrapping_add(i * 2).cast(),
            size_of::<[u64; 2]>(),
        ) {
            return lerr(EFAULT);
        }
        *tmp.add(words) = pair[0];
        words += 1;
        *tmp.add(words) = pair[1];
        words += 1;
        if pair[0] == AX_NULL {
            pthread_mutex_lock(ptr::addr_of_mut!(G_AUXV_LOCK));
            ptr::copy_nonoverlapping(
                tmp as *const u64,
                ptr::addr_of_mut!(G_AUXV).cast::<u64>(),
                words,
            );
            *ptr::addr_of_mut!(G_AUXV_WORDS) = words;
            pthread_mutex_unlock(ptr::addr_of_mut!(G_AUXV_LOCK));
            return 0;
        }
        i += 1;
    }
    // No AT_NULL inside the bound. Refusing is the right answer: a vector that
    // long is a bad pointer that happens to be readable, and copying more of
    // it would leak whatever follows into a file the guest can read. The
    // previously published vector is left alone for the same reason the EFAULT
    // path leaves it alone -- the old truth beats a half-truth.
    lerr(E2BIG)
}

// Binary, not text: pairs of native 64-bit words ending in AT_NULL. When the
// hook has not been called the file still has to exist, because glibc's
// getauxval() falls back to reading it and returning ENOENT there turns into
// "this machine has no HWCAP" rather than into an error. The fallback set is
// the measurable minimum -- page size, the HWCAP stack.rs agrees to, the clock
// tick, and the four credential entries -- and deliberately omits AT_PHDR,
// AT_BASE, AT_ENTRY and AT_RANDOM, which cannot be reconstructed from outside
// stack.rs and would be actively harmful if guessed: AT_RANDOM in particular is
// the address of the stack canary seed.
unsafe fn gen_auxv(b: *mut c_char, cap: usize) -> usize {
    pthread_mutex_lock(ptr::addr_of_mut!(G_AUXV_LOCK));
    let words = *ptr::addr_of!(G_AUXV_WORDS);
    if words != 0 {
        let n = words * size_of::<u64>();
        if n > cap {
            pthread_mutex_unlock(ptr::addr_of_mut!(G_AUXV_LOCK));
            return GEN_TRUNC; // a half auxv is unparseable, not merely short
        }
        ptr::copy_nonoverlapping(ptr::addr_of!(G_AUXV).cast::<u8>(), b.cast::<u8>(), n);
        pthread_mutex_unlock(ptr::addr_of_mut!(G_AUXV_LOCK));
        return n;
    }
    pthread_mutex_unlock(ptr::addr_of_mut!(G_AUXV_LOCK));

    let v: [u64; 20] = [
        AX_PAGESZ,
        guest_page_bytes(),
        AX_HWCAP,
        lxrt_auxv_hwcap() as u64,
        AX_HWCAP2,
        0,
        AX_CLKTCK,
        LXRT_CLK_TCK,
        AX_UID,
        getuid() as u64,
        AX_EUID,
        geteuid() as u64,
        AX_GID,
        getgid() as u64,
        AX_EGID,
        getegid() as u64,
        AX_SECURE,
        0,
        AX_NULL,
        0,
    ];
    let n = size_of::<[u64; 20]>();
    if n > cap {
        return GEN_TRUNC;
    }
    ptr::copy_nonoverlapping(v.as_ptr().cast::<u8>(), b.cast::<u8>(), n);
    n
}

// ------------------------------------------------------------- /proc/sys/*

// Yama does not exist on Darwin, so this is a translation of Darwin's actual
// ptrace policy into the Linux scale, not a passthrough. 0 would mean "any
// process may trace any other", which is flatly untrue here: task_for_pid()
// needs root or com.apple.security.cs.debugger and fails for an unrelated
// process even as the same user. 1 -- "only descendants, unless the tracee
// opts in with PR_SET_PTRACER" -- is the closest honest rung, and it is also
// the value Ubuntu ships, so callers have a tested path for it. Reporting 0
// makes a crash handler attach out-of-process and then fail at the attach.
unsafe fn gen_ptrace_scope(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    sbf!(s, c"1\n");
    sb_done(s)
}

// perf_event_open does not exist here at all. 2 is the Linux default ("no
// kernel or CPU-wide tracing for unprivileged users"), but 3 -- Debian and
// Ubuntu's extra rung, "no unprivileged perf at all" -- is the value that
// matches reality, and tools that special-case the paranoid level already
// understand it. The honest failure still happens at the syscall, which
// returns ENOSYS; this only stops callers from getting there.
unsafe fn gen_perf_paranoid(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    sbf!(s, c"3\n");
    sb_done(s)
}

// The ceiling on RLIMIT_NOFILE. Linux's default is 1048576; Darwin's real
// ceiling is kern.maxfilesperproc, measured at 61440 on this machine. Printing
// Linux's number makes a guest raise its soft limit to something Darwin's
// setrlimit refuses, and the refusal arrives as EINVAL from a call the guest
// had every reason to expect would work.
// No namespaces exist here, and saying so is the honest answer: Steam's
// steamwebhelper.sh reads these two knobs and, seeing 0, starts CEF with
// --no-sandbox instead of trying a user-namespace sandbox that would fail.
unsafe fn gen_max_user_namespaces(b: *mut c_char, cap: usize) -> usize {
    snprintf(b, cap, c"0\n".as_ptr()) as usize
}
unsafe fn gen_unprivileged_userns_clone(b: *mut c_char, cap: usize) -> usize {
    snprintf(b, cap, c"0\n".as_ptr()) as usize
}

unsafe fn gen_nr_open(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let v = sctl_u64(c"kern.maxfilesperproc", 61440) as c_ulonglong;
    sbf!(s, c"%llu\n", v);
    sb_done(s)
}

// inotify is emulated over kqueue's EVFILT_VNODE (runtime/inotify.c), which
// needs an open descriptor per watched file and cannot watch a subtree; the
// limit is honest about that. 8192 is the stock Linux default. It is reported as
// the stock default rather than as a large tuned number precisely so that a
// caller which merely checks the ceiling proceeds and then meets the honest
// ENOSYS from inotify_init1, instead of one which believes it has 524288
// watches available and silently never receives an event.
unsafe fn gen_max_user_watches(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    sbf!(s, c"8192\n");
    sb_done(s)
}

// 0 = heuristic overcommit, which is what Darwin does: it hands out address
// space freely and kills on real exhaustion. This one genuinely matches.
unsafe fn gen_overcommit(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    sbf!(s, c"0\n");
    sb_done(s)
}

// Darwin recycles pids at 99999 (PID_MAX in the XNU sources); kern.maxproc,
// 4000 here, is the concurrent-process cap and a different number entirely.
// Linux's default pid_max is 32768, which is SMALLER than the largest pid this
// runtime will hand out -- so a guest that sizes a pid-indexed table from this
// file and then sees pid 40000 from getpid() writes out of bounds. That is
// exactly the late, quiet failure this file exists to avoid.
unsafe fn gen_pid_max(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    sbf!(s, c"99999\n");
    sb_done(s)
}

unsafe fn gen_osrelease(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    // LXRT_OSRELEASE "\n" in the C; the macro holds no conversion.
    sbf!(s, c"%s\n", LXRT_OSRELEASE.as_ptr());
    sb_done(s)
}

unsafe fn uuid_format(s: &mut Sb, u: &[u8; 16]) {
    sbf!(
        s,
        c"%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n",
        u[0] as c_int,
        u[1] as c_int,
        u[2] as c_int,
        u[3] as c_int,
        u[4] as c_int,
        u[5] as c_int,
        u[6] as c_int,
        u[7] as c_int,
        u[8] as c_int,
        u[9] as c_int,
        u[10] as c_int,
        u[11] as c_int,
        u[12] as c_int,
        u[13] as c_int,
        u[14] as c_int,
        u[15] as c_int
    );
}

// boot_id must be the SAME value in every process for as long as the machine
// stays up, and a DIFFERENT one after a reboot. That is the whole contract:
// dbus, systemd-shaped code and Steam's own crash reporter use it to decide
// whether two observations came from the same boot. A per-process
// arc4random_buf would satisfy the format and break the meaning, and the break
// only shows when a second guest process starts -- so it is derived
// deterministically from kern.uuid (stable per machine) and kern.boottime
// (stable per boot) instead, mixed with splitmix64.
unsafe fn gen_boot_id(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let mut host: [c_char; 64] = [0; 64];
    let mut hl: usize = 64;
    if sysctlbyname(
        c"kern.uuid".as_ptr(),
        host.as_mut_ptr().cast(),
        &mut hl,
        ptr::null_mut(),
        0,
    ) != 0
    {
        snprintf(host.as_mut_ptr(), 64, c"lxrt".as_ptr());
    }

    let mut h: u64 = 1_469_598_103_934_665_603; // FNV-1a offset basis
    let mut p: *const c_char = host.as_ptr();
    while *p != 0 {
        h ^= *p as u8 as u64;
        h = h.wrapping_mul(1_099_511_628_211);
        p = p.add(1);
    }
    let bt = boot_time_sec();
    h ^= bt.wrapping_add(0x9e37_79b9_7f4a_7c15);
    h = h.wrapping_mul(1_099_511_628_211);

    let mut u: [u8; 16] = [0; 16];
    let mut x = h;
    for byte in u.iter_mut() {
        x = x.wrapping_add(0x9e37_79b9_7f4a_7c15); // splitmix64
        let mut z = x;
        z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
        z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
        *byte = ((z ^ (z >> 31)) & 0xff) as u8;
    }
    u[6] = (u[6] & 0x0f) | 0x40; // RFC 4122 version 4
    u[8] = (u[8] & 0x3f) | 0x80; // variant 10x
    uuid_format(s, &u);
    sb_done(s)
}

// Linux hands out a fresh UUID on every read(2) of this file. Here it is a
// real file, so the best available granularity is a fresh one per lookup --
// that is, per open(). A guest that opens once and reads twice gets the same
// value where Linux would give two; a guest that opens each time, which is
// what every user of this file actually does, sees no difference.
unsafe fn gen_random_uuid(b: *mut c_char, cap: usize) -> usize {
    let mut s = Sb::new(b, cap);
    let s = &mut s;
    let mut u: [u8; 16] = [0; 16];
    arc4random_buf(u.as_mut_ptr().cast(), 16);
    u[6] = (u[6] & 0x0f) | 0x40;
    u[8] = (u[8] & 0x3f) | 0x80;
    uuid_format(s, &u);
    sb_done(s)
}

// ------------------------------------------------------------------ table

// Scope, because /proc/stat and /proc/self/stat are different files with the
// same last component. procfs.c's directory carries a `self -> .` symlink, so
// both land on one host file if they share a name, and one of them has to be
// renamed.
//
// The per-process file keeps the name "stat" and the system-wide one becomes
// "stat.sys". That is the direction that closes the hole rather than leaving
// it: a caller which WALKS into <dir>/self/ and opens "stat" now gets the
// per-process file, which is the correct resolution, and nothing on Linux
// resolves /proc/<something>/stat to the system-wide file, so the opposite
// naming had a wrong answer available and this one does not. The whole-string
// path /proc/stat still maps explicitly to "stat.sys" through the table below.
// Verified before the swap: reading <dir>/self/stat returned
// "cpu  24113451 0 9331725 ...".
#[derive(Clone, Copy, PartialEq, Eq)]
enum Scope {
    Sys,
    SelfProc,
}
use Scope::{SelfProc as SC_SELF, Sys as SC_SYS};

type Generator = unsafe fn(*mut c_char, usize) -> usize;

struct ExtFile {
    scope: Scope,
    guest: &'static CStr, // path under /proc, with any self/ prefix removed
    host: &'static CStr,  // path under the runtime's procfs directory
    gen: Generator,
}

const fn ext(scope: Scope, guest: &'static CStr, host: &'static CStr, gen: Generator) -> ExtFile {
    ExtFile {
        scope,
        guest,
        host,
        gen,
    }
}

#[rustfmt::skip]
static G_FILES: [ExtFile; 25] = [
    ext(SC_SYS,  c"meminfo",  c"meminfo",  gen_meminfo),
    ext(SC_SYS,  c"cpuinfo",  c"cpuinfo",  gen_cpuinfo),
    ext(SC_SYS,  c"stat",     c"stat.sys", gen_stat),
    ext(SC_SYS,  c"vmstat",   c"vmstat",   gen_vmstat),
    ext(SC_SYS,  c"version",  c"version",  gen_version),
    ext(SC_SYS,  c"mounts",   c"mounts",   gen_mounts),
    ext(SC_SYS,  c"uptime",   c"uptime",   gen_uptime),
    ext(SC_SYS,  c"loadavg",  c"loadavg",  gen_loadavg),

    ext(SC_SELF, c"stat",      c"stat",      gen_self_stat),
    ext(SC_SELF, c"statm",     c"statm",     gen_self_statm),
    ext(SC_SELF, c"status",    c"status",    gen_self_status),
    ext(SC_SELF, c"mountinfo", c"mountinfo", gen_mountinfo),
    ext(SC_SELF, c"auxv",      c"auxv",      gen_auxv),
    // /proc/self/mounts is a real file on Linux -- verified on the project's
    // aarch64 guest, `-r--r--r--. 1 user user 0 /proc/self/mounts` -- and it
    // is the spelling glibc's setmntent/getmntent callers and libmount reach
    // for at least as often as /proc/mounts, Steam's free-space checks
    // included. Same host file as the system-wide one: the content is
    // identical on Linux too, both being views of the same mount table.
    ext(SC_SELF, c"mounts",    c"mounts",    gen_mounts),

    ext(SC_SYS, c"sys/user/max_user_namespaces",
                c"sys/user/max_user_namespaces",         gen_max_user_namespaces),
    ext(SC_SYS, c"sys/kernel/unprivileged_userns_clone",
                c"sys/kernel/unprivileged_userns_clone", gen_unprivileged_userns_clone),
    ext(SC_SYS, c"sys/kernel/yama/ptrace_scope",
                c"sys/kernel/yama/ptrace_scope",         gen_ptrace_scope),
    ext(SC_SYS, c"sys/kernel/perf_event_paranoid",
                c"sys/kernel/perf_event_paranoid",       gen_perf_paranoid),
    ext(SC_SYS, c"sys/kernel/pid_max",
                c"sys/kernel/pid_max",                   gen_pid_max),
    ext(SC_SYS, c"sys/kernel/osrelease",
                c"sys/kernel/osrelease",                 gen_osrelease),
    ext(SC_SYS, c"sys/kernel/random/boot_id",
                c"sys/kernel/random/boot_id",            gen_boot_id),
    ext(SC_SYS, c"sys/kernel/random/uuid",
                c"sys/kernel/random/uuid",               gen_random_uuid),
    ext(SC_SYS, c"sys/fs/nr_open",
                c"sys/fs/nr_open",                       gen_nr_open),
    ext(SC_SYS, c"sys/fs/inotify/max_user_watches",
                c"sys/fs/inotify/max_user_watches",      gen_max_user_watches),
    ext(SC_SYS, c"sys/vm/overcommit_memory",
                c"sys/vm/overcommit_memory",             gen_overcommit),
];

// Directories inside the /proc/sys tree, so that an opendir() of any of them
// succeeds and enumerates. Opening the directory materialises everything
// underneath it, which is what a reader walking the tree is about to need.
static G_DIRS: [&CStr; 7] = [
    c"sys",
    c"sys/kernel",
    c"sys/kernel/yama",
    c"sys/kernel/random",
    c"sys/fs",
    c"sys/fs/inotify",
    c"sys/vm",
];

// Every file is regenerated on the lookup that asks for it, including the ones
// whose content never changes. Caching them would save a handful of sysctls
// against the open/read/close the caller is about to perform anyway, and would
// buy back the entire class of bug where the first answer is the only answer
// -- which is how a stale /proc/self/stat outlives the thread it describes.
//
// Returns false when the file could not be produced, for either reason a file
// can fail to be produced: the content did not fit (the buffer is grown and
// retried up to GEN_MAX_HARD, and a truncated file is never published), or the
// write failed (write_atomic has already removed the stale copy). The caller
// turns false into NULL, and NULL into ENOENT.
const GEN_MAX_HARD: usize = 8 * 1024 * 1024;

unsafe fn materialise(f: &ExtFile, dir: *const c_char) -> bool {
    let host = f.host.to_bytes();
    // strrchr(f->host, '/')
    if let Some(n) = host.iter().rposition(|&byte| byte == b'/') {
        let mut sub = MaybeUninit::<[c_char; 512]>::uninit();
        let sub: *mut c_char = sub.as_mut_ptr().cast();
        if n < 512 {
            ptr::copy_nonoverlapping(f.host.as_ptr(), sub, n);
            *sub.add(n) = 0;
            mkdirs(dir, sub);
        }
    }

    let mut cap: usize = GEN_MAX;
    loop {
        let buf: *mut c_char = malloc(cap).cast();
        if buf.is_null() {
            return false;
        }
        let len = (f.gen)(buf, cap);
        if len != GEN_TRUNC {
            let ok = write_atomic(dir, f.host.as_ptr(), buf.cast(), len);
            free(buf.cast());
            return ok;
        }
        free(buf.cast());
        if cap >= GEN_MAX_HARD {
            return false;
        }
        cap *= 2;
    }
}

// --------------------------------------------------------------- translate

// static _Thread_local char out[1024]. The buffer has no destructor, so the
// access neither allocates nor can fail.
thread_local! {
    static OUT: UnsafeCell<[c_char; 1024]> = const { UnsafeCell::new([0; 1024]) };
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_proc_ext_translate(
    path: *const c_char,
    dir: *const c_char,
) -> *const c_char {
    if path.is_null() || dir.is_null() || *dir == 0 {
        return ptr::null();
    }
    if strncmp(path, c"/proc/".as_ptr(), 6) != 0 {
        return ptr::null();
    }

    let mut rest: *const c_char = path.add(6);
    let mut scope = SC_SYS;

    // /proc/self/... and /proc/<our pid>/... name the same thing. A numeric
    // first component that is NOT our pid is another process: not ours, and
    // not synthesisable (task_for_pid is entitled), so it falls through to
    // ENOENT rather than to a fabricated answer.
    if strncmp(rest, c"self/".as_ptr(), 5) == 0 {
        scope = SC_SELF;
        rest = rest.add(5);
    } else if *rest as u8 >= b'0' && *rest as u8 <= b'9' {
        let mut mypid: [c_char; 32] = [0; 32];
        let k = snprintf(mypid.as_mut_ptr(), 32, c"%d/".as_ptr(), lxrt_ids_pid());
        if k <= 0 || strncmp(rest, mypid.as_ptr(), k as usize) != 0 {
            return ptr::null();
        }
        scope = SC_SELF;
        rest = rest.add(k as usize);
    }

    // Trailing slash on a directory lookup: "/proc/sys/" and "/proc/sys" are
    // the same request.
    let mut trimmed = MaybeUninit::<[c_char; 512]>::uninit();
    let trimmed: *mut c_char = trimmed.as_mut_ptr().cast();
    let mut rl = strlen(rest);
    while rl != 0 && *rest.add(rl - 1) == b'/' as c_char {
        rl -= 1;
    }
    if rl == 0 || rl >= 512 {
        return ptr::null();
    }
    ptr::copy_nonoverlapping(rest, trimmed, rl);
    *trimmed.add(rl) = 0;

    // The procfs directory is normally already there; creating it here means a
    // lookup that arrives before lxrt_proc_init finished still works rather
    // than silently producing nothing.
    mkdir(dir, 0o700);

    let out: *mut c_char = OUT.with(|cell| cell.get().cast());

    if scope == SC_SYS {
        for g_dir in G_DIRS.iter() {
            if strcmp(trimmed, g_dir.as_ptr()) != 0 {
                continue;
            }
            let dlen = strlen(g_dir.as_ptr());
            mkdirs(dir, g_dir.as_ptr());
            // A member that fails to generate is not a reason to fail the
            // DIRECTORY lookup: write_atomic has already unlinked its stale
            // copy, so it is simply absent from the listing and the open() of
            // it fails on its own with ENOENT. What would be wrong is
            // reporting a directory that does not exist, which the stat below
            // is there to rule out.
            for file in G_FILES.iter() {
                if file.scope == SC_SYS
                    && strncmp(file.guest.as_ptr(), g_dir.as_ptr(), dlen) == 0
                    && *file.guest.as_ptr().add(dlen) == b'/' as c_char
                {
                    materialise(file, dir);
                }
            }
            snprintf(out, 1024, c"%s/%s".as_ptr(), dir, g_dir.as_ptr());
            let mut st = MaybeUninit::<Stat>::uninit();
            if stat(out, st.as_mut_ptr()) != 0 || (*st.as_ptr()).st_mode & S_IFMT != S_IFDIR {
                return ptr::null();
            }
            return out;
        }
    }

    for file in G_FILES.iter() {
        if file.scope != scope || strcmp(file.guest.as_ptr(), trimmed) != 0 {
            continue;
        }
        // NULL on a generation failure, never the path. Handing back a path
        // whose content is a previous generation -- or nothing at all -- is
        // the one outcome worse than ENOENT, because the guest has no way to
        // tell it apart from a fresh answer. procfs.c's caller falls through
        // to the ordinary translation, which produces ENOENT.
        if !materialise(file, dir) {
            return ptr::null();
        }
        snprintf(out, 1024, c"%s/%s".as_ptr(), dir, file.host.as_ptr());
        return out;
    }
    ptr::null()
}
