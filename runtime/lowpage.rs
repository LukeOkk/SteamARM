// Virtual pages below 4 GiB for native (aarch64) guests: KUSER_SHARED_DATA.
//
// macOS keeps the low 4 GiB of every arm64 task unmapped (__PAGEZERO), and a
// native guest cannot have memory there: nothing relocates its addresses the
// way FEX's guest base relocates an x86 program's (gbase.rs). Wine's Windows
// side needs exactly one thing down there, KUSER_SHARED_DATA, the page Windows
// keeps at 0x7ffe0000 in every process: ntdll's user_shared_data, allocated
// fixed by virtual_alloc_first_teb (wine exited there: "failed to map the
// shared user data", MEASURED with Valve's Proton ARM64, 2026-10-05), later
// remapped from wineserver's section by virtual_map_user_shared_data, read by
// ntdll and kernel32 for the clock and the processor features.
//
// With LXRT_LOWPAGES=1 a fixed mapping inside [0x7ffe0000, 0x7fff0000) is
// accepted as a VIRTUAL page: its memory lives at a host address of the
// runtime's choosing, the guest's address stays unmapped, and every load or
// store that faults there is carried out by the fault handler against the
// backing -- the instruction decoded, the registers of the signal context
// updated, the pc advanced. A read costs a fault, microseconds: right for a
// page read a few thousand times a second, wrong for a heap. So nothing else
// below 4 GiB is accepted: a MAP_FIXED_NOREPLACE probe there answers EEXIST
// ("busy", which sends Wine's allocator upward: try_map_free_area doubles its
// step) and a plain MAP_FIXED answers ENOMEM, as the host would.
//
// Off (the default) nothing here runs: FEX guests keep their guest base and
// their ENOMEM probes (dispatch.c).

use std::cell::UnsafeCell;
use std::ffi::{c_char, c_int, c_long, c_uint, c_ulong, c_ulonglong, c_void};
use std::ptr;
use std::sync::atomic::{fence, AtomicI32, AtomicPtr, AtomicU64, Ordering};

#[allow(dead_code)]
mod host {
    include!(concat!(env!("OUT_DIR"), "/privmap_constants.rs"));
}
use host::{D_MAP_ANON, D_MAP_PRIVATE, D_MAP_SHARED, D_PROT_READ, D_PROT_WRITE};

// Darwin's <errno.h> values: the C returned the host's numbers unconverted.
const EEXIST: c_long = 17;
const ENOMEM: c_long = 12;

const LXRT_HOST_PAGE: usize = 16_384;

const LP_LO: u64 = 0x7ffe_0000;
const LP_HI: u64 = 0x7fff_0000;
const LP_PAGE: u64 = 4_096;
const LP_N: usize = ((LP_HI - LP_LO) / LP_PAGE) as usize;

const LINUX_PROT_READ: c_int = 1;
const LINUX_PROT_WRITE: c_int = 2;
const LINUX_MAP_SHARED: c_long = 1;
const LINUX_MAP_FIXED: c_long = 0x10;
const LINUX_MAP_ANONYMOUS: c_long = 0x20;
const LINUX_MAP_FIXED_NOREPLACE: c_long = 0x10_0000;

const LC_SEGMENT_64: u32 = 0x19;

struct LpPage {
    host: AtomicPtr<u8>, // the backing (a host page), NULL when unmapped
    prot: AtomicI32,     // the guest's PROT_* (Linux values)
}

impl LpPage {
    #[allow(clippy::declare_interior_mutable_const)]
    const EMPTY: Self = Self {
        host: AtomicPtr::new(ptr::null_mut()),
        prot: AtomicI32::new(0),
    };
}

static G_PAGES: [LpPage; LP_N] = [LpPage::EMPTY; LP_N];

// Darwin's pthread_mutex_t and PTHREAD_MUTEX_INITIALIZER (arm64).
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}

struct Lock(UnsafeCell<PthreadMutex>);
unsafe impl Sync for Lock {}

static G_LOCK: Lock = Lock(UnsafeCell::new(PthreadMutex {
    sig: 0x32AA_ABA7,
    opaque: [0; 56],
}));

static G_FAULTS: AtomicU64 = AtomicU64::new(0);

// _STRUCT_ARM_THREAD_STATE64 (arm64, no pointer authentication: the
// __darwin_arm_thread_state64_* accessors are plain field accesses).
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

#[repr(C)]
struct ArmExceptionState64 {
    far: u64,
    esr: u32,
    exception: u32,
}

#[repr(C)]
struct ArmNeonState64 {
    v: [u128; 32],
    fpsr: u32,
    fpcr: u32,
}

#[repr(C)]
struct MContext64 {
    es: ArmExceptionState64,
    ss: ArmThreadState64,
    ns: ArmNeonState64,
}

#[repr(C)]
struct StackT {
    ss_sp: *mut c_void,
    ss_size: usize,
    ss_flags: c_int,
}

#[repr(C)]
struct UContext {
    uc_onstack: c_int,
    uc_sigmask: u32,
    uc_stack: StackT,
    uc_link: *mut UContext,
    uc_mcsize: usize,
    uc_mcontext: *mut MContext64,
}

// siginfo_t (struct __siginfo), as far as si_addr.
#[repr(C)]
pub struct SigInfo {
    si_signo: c_int,
    si_errno: c_int,
    si_code: c_int,
    si_pid: c_int,
    si_uid: c_uint,
    si_status: c_int,
    si_addr: *mut c_void,
}

#[repr(C)]
struct DlInfo {
    dli_fname: *const c_char,
    dli_fbase: *mut c_void,
    dli_sname: *const c_char,
    dli_saddr: *mut c_void,
}

#[repr(C)]
struct MachHeader64 {
    magic: u32,
    cputype: i32,
    cpusubtype: i32,
    filetype: u32,
    ncmds: u32,
    sizeofcmds: u32,
    flags: u32,
    reserved: u32,
}

#[repr(C)]
struct LoadCommand {
    cmd: u32,
    cmdsize: u32,
}

#[repr(C)]
struct SegmentCommand64 {
    cmd: u32,
    cmdsize: u32,
    segname: [c_char; 16],
    vmaddr: u64,
    vmsize: u64,
    fileoff: u64,
    filesize: u64,
    maxprot: i32,
    initprot: i32,
    nsects: u32,
    flags: u32,
}

#[link(name = "System")]
unsafe extern "C" {
    fn __error() -> *mut c_int;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(address: *mut c_void, length: usize) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn dladdr(address: *const c_void, info: *mut DlInfo) -> c_int;
    fn strstr(haystack: *const c_char, needle: *const c_char) -> *mut c_char;
    fn strcmp(left: *const c_char, right: *const c_char) -> c_int;
    fn longjmp(environment: *mut c_int, value: c_int) -> !;
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
}

#[inline(always)]
fn page(index: usize) -> &'static LpPage {
    // Every caller has range-checked the index, as the C did.
    unsafe { G_PAGES.get_unchecked(index) }
}

#[inline(always)]
fn lock() {
    unsafe {
        let _ = pthread_mutex_lock(G_LOCK.0.get());
    }
}

#[inline(always)]
fn unlock() {
    unsafe {
        let _ = pthread_mutex_unlock(G_LOCK.0.get());
    }
}

#[no_mangle]
pub extern "C" fn lxrt_lowpage_on() -> bool {
    static ON: AtomicI32 = AtomicI32::new(-1);
    let mut on = ON.load(Ordering::Relaxed);
    if on < 0 {
        let e = unsafe { getenv(b"LXRT_LOWPAGES\0".as_ptr().cast()) };
        on = (!e.is_null() && unsafe { *e } as u8 == b'1') as i32;
        ON.store(on, Ordering::Relaxed);
    }
    on != 0
}

fn in_window(addr: u64, len: u64) -> bool {
    addr >= LP_LO && len != 0 && addr.wrapping_add(len) <= LP_HI
}

fn release(index: usize) {
    let h = page(index).host.swap(ptr::null_mut(), Ordering::SeqCst);
    if !h.is_null() {
        unsafe {
            let _ = munmap(h.cast(), LXRT_HOST_PAGE);
        }
    }
}

// A fixed request below 4 GiB. True when answered here (*ret is the result).
#[no_mangle]
pub unsafe extern "C" fn lxrt_lowpage_mmap(
    addr: u64,
    len: u64,
    prot: c_long,
    lflags: c_long,
    fd: c_long,
    off: c_long,
    ret: *mut c_long,
) -> bool {
    if !lxrt_lowpage_on() || addr == 0 || addr >= (1u64 << 32) {
        return false;
    }
    let fixed = lflags & LINUX_MAP_FIXED != 0;
    let noreplace = lflags & LINUX_MAP_FIXED_NOREPLACE != 0;
    if !fixed && !noreplace {
        return false; // a hint: the host places it (above 4 GiB)
    }
    if !in_window(addr, len) || addr % LP_PAGE != 0 || len % LP_PAGE != 0 {
        *ret = -(if noreplace && !fixed { EEXIST } else { ENOMEM });
        return true;
    }
    let first = ((addr - LP_LO) / LP_PAGE) as u32;
    let n = (len / LP_PAGE) as u32;
    lock();
    if noreplace && !fixed {
        let mut i = first;
        while i < first + n {
            if !page(i as usize).host.load(Ordering::SeqCst).is_null() {
                unlock();
                *ret = -EEXIST;
                return true;
            }
            i += 1;
        }
    }
    let mut i = first;
    while i < first + n {
        let h = if lflags & LINUX_MAP_ANONYMOUS != 0 {
            mmap(
                ptr::null_mut(),
                LXRT_HOST_PAGE,
                D_PROT_READ | D_PROT_WRITE,
                D_MAP_PRIVATE | D_MAP_ANON,
                -1,
                0,
            )
        } else {
            // A file page (wineserver's section): shared, so the server's
            // clock updates arrive. Mapped readable and, when the file
            // allows it, writable; the guest's protection is enforced by
            // the emulation, not the host.
            let hprot = D_PROT_READ
                | if prot & LINUX_PROT_WRITE as c_long != 0 {
                    D_PROT_WRITE
                } else {
                    0
                };
            let hflags = if lflags & LINUX_MAP_SHARED != 0 {
                D_MAP_SHARED
            } else {
                D_MAP_PRIVATE
            };
            mmap(
                ptr::null_mut(),
                LXRT_HOST_PAGE,
                hprot,
                hflags,
                fd as c_int,
                off.wrapping_add((u64::from(i - first) * LP_PAGE) as c_long),
            )
        };
        if h as isize == -1 {
            let e = *__error();
            unlock();
            *ret = -(e as c_long);
            return true;
        }
        release(i as usize);
        page(i as usize).prot.store(prot as c_int, Ordering::SeqCst);
        page(i as usize).host.store(h.cast(), Ordering::SeqCst);
        i += 1;
    }
    unlock();
    if lxrt_trace_on() {
        let kind: *const c_char = if lflags & LINUX_MAP_ANONYMOUS != 0 {
            b"anonymous\0".as_ptr().cast()
        } else {
            b"file\0".as_ptr().cast()
        };
        let _ = fprintf(
            lxrt_trace_stream(),
            b"[lxrt] lowpage: 0x%llx+0x%llx prot %ld %s -> virtual page(s)\n\0"
                .as_ptr()
                .cast(),
            addr as c_ulonglong,
            len as c_ulonglong,
            prot,
            kind,
        );
    }
    *ret = addr as c_long;
    true
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_lowpage_munmap(addr: u64, len: u64, ret: *mut c_long) -> bool {
    if !lxrt_lowpage_on() || len == 0 || addr.wrapping_add(len) > (1u64 << 32) {
        return false;
    }
    lock();
    for i in 0..LP_N {
        let g = LP_LO + i as u64 * LP_PAGE;
        if g >= addr && g + LP_PAGE <= addr.wrapping_add(len) {
            release(i);
        }
    }
    unlock();
    *ret = 0; // Linux: unmapping nothing is fine
    true
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_lowpage_mprotect(
    addr: u64,
    len: u64,
    prot: c_long,
    ret: *mut c_long,
) -> bool {
    if !lxrt_lowpage_on() || len == 0 || addr.wrapping_add(len) > (1u64 << 32) {
        return false;
    }
    let mut any = false;
    let mut hole = false;
    let mut g = addr & !(LP_PAGE - 1);
    while g < addr.wrapping_add(len) {
        if g < LP_LO
            || g >= LP_HI
            || page(((g - LP_LO) / LP_PAGE) as usize)
                .host
                .load(Ordering::SeqCst)
                .is_null()
        {
            hole = true;
            g = g.wrapping_add(LP_PAGE);
            continue;
        }
        page(((g - LP_LO) / LP_PAGE) as usize)
            .prot
            .store(prot as c_int, Ordering::SeqCst);
        any = true;
        g = g.wrapping_add(LP_PAGE);
    }
    *ret = if any && !hole { 0 } else { -ENOMEM }; // Linux: ENOMEM when a page is unmapped
    true
}

// ----------------------------------------------------------------- faults

// The bytes [guest, guest+n) through the backing; false when a byte is not in
// a virtual page the access may touch.
unsafe fn lp_access(mut guest: u64, buf: *mut u8, mut n: u32, write: bool) -> bool {
    let mut b = buf;
    while n != 0 {
        if guest < LP_LO || guest >= LP_HI {
            return false;
        }
        let i = ((guest - LP_LO) / LP_PAGE) as usize;
        let h = page(i).host.load(Ordering::SeqCst);
        let prot = page(i).prot.load(Ordering::SeqCst);
        let need = if write {
            LINUX_PROT_WRITE
        } else {
            LINUX_PROT_READ
        };
        if h.is_null() || prot & need == 0 {
            return false;
        }
        let within = (guest % LP_PAGE) as u32;
        let mut k = LP_PAGE as u32 - within;
        if k > n {
            k = n;
        }
        if write {
            ptr::copy_nonoverlapping(b as *const u8, h.add(within as usize), k as usize);
        } else {
            ptr::copy_nonoverlapping(h.add(within as usize) as *const u8, b, k as usize);
        }
        guest = guest.wrapping_add(u64::from(k));
        b = b.add(k as usize);
        n -= k;
    }
    true
}

unsafe fn get_x(ss: *const ArmThreadState64, r: u32) -> u64 {
    if r < 29 {
        return *(*ss).x.get_unchecked(r as usize);
    }
    if r == 29 {
        return (*ss).fp;
    }
    if r == 30 {
        return (*ss).lr;
    }
    0 // xzr
}

unsafe fn set_x(ss: *mut ArmThreadState64, r: u32, v: u64) {
    if r < 29 {
        *(*ss).x.get_unchecked_mut(r as usize) = v;
    } else if r == 29 {
        (*ss).fp = v;
    } else if r == 30 {
        (*ss).lr = v;
    }
    // 31: xzr, discarded
}

unsafe fn get_base(ss: *const ArmThreadState64, rn: u32) -> u64 {
    if rn == 31 {
        (*ss).sp
    } else {
        get_x(ss, rn)
    }
}

unsafe fn set_base(ss: *mut ArmThreadState64, rn: u32, v: u64) {
    if rn == 31 {
        (*ss).sp = v;
    } else {
        set_x(ss, rn, v);
    }
}

fn sext(v: u64, bits: u32) -> i64 {
    let m = 1u64 << (bits - 1);
    (v ^ m).wrapping_sub(m) as i64
}

// One integer or SIMD load/store of `bytes` at `addr`: register `rt`,
// `simd` for a V register, `sign` the load's sign extension width in bits
// (0: zero-extend; 32: to a 32-bit result; 64: to 64 bits).
unsafe fn do_access(
    uc: *mut UContext,
    addr: u64,
    bytes: u32,
    rt: u32,
    load: bool,
    simd: bool,
    sign: u32,
) -> bool {
    let ss: *mut ArmThreadState64 = ptr::addr_of_mut!((*(*uc).uc_mcontext).ss);
    let ns: *mut ArmNeonState64 = ptr::addr_of_mut!((*(*uc).uc_mcontext).ns);
    let mut buf = [0u8; 16];
    if load {
        if !lp_access(addr, buf.as_mut_ptr(), bytes, false) {
            return false;
        }
        if simd {
            let mut v: u128 = 0;
            ptr::copy_nonoverlapping(
                buf.as_ptr(),
                ptr::addr_of_mut!(v).cast::<u8>(),
                bytes as usize,
            );
            *(*ns).v.get_unchecked_mut((rt & 31) as usize) = v;
        } else {
            let mut v: u64 = 0;
            ptr::copy_nonoverlapping(
                buf.as_ptr(),
                ptr::addr_of_mut!(v).cast::<u8>(),
                bytes as usize,
            );
            if sign == 64 {
                v = sext(v, bytes * 8) as u64;
            } else if sign == 32 {
                v = u64::from(sext(v, bytes * 8) as u32);
            }
            set_x(ss, rt, v);
        }
        return true;
    }
    if simd {
        let v: u128 = *(*ns).v.get_unchecked((rt & 31) as usize);
        ptr::copy_nonoverlapping(
            ptr::addr_of!(v).cast::<u8>(),
            buf.as_mut_ptr(),
            bytes as usize,
        );
    } else {
        let v: u64 = get_x(ss, rt);
        ptr::copy_nonoverlapping(
            ptr::addr_of!(v).cast::<u8>(),
            buf.as_mut_ptr(),
            bytes as usize,
        );
    }
    lp_access(addr, buf.as_mut_ptr(), bytes, true)
}

type Why = *const c_char;

macro_rules! why {
    ($text:literal) => {
        Err(concat!($text, "\0").as_ptr().cast::<c_char>())
    };
}

// The fault at `fa` with the instruction `insn` at the context's pc. Ok when
// it was carried out and the pc advanced; Err names why not.
unsafe fn emulate(uc: *mut UContext, _fa: u64, insn: u32) -> Result<(), Why> {
    let ss: *mut ArmThreadState64 = ptr::addr_of_mut!((*(*uc).uc_mcontext).ss);
    let size = insn >> 30;
    let rt = insn & 31;
    let rn = (insn >> 5) & 31;
    let simd = insn & (1u32 << 26) != 0;

    // Load/store pair: 101 V 0 opc2 L imm7 Rt2 Rn Rt (opc2: 001 post, 010 offset, 011 pre, 000 no-allocate)
    if insn & 0x3A00_0000 == 0x2800_0000 {
        let opc = size;
        let opc2 = (insn >> 23) & 7;
        let load = insn & (1u32 << 22) != 0;
        let rt2 = (insn >> 10) & 31;
        let imm = sext(u64::from((insn >> 15) & 0x7F), 7);
        let bytes: u32;
        let mut sign = 0u32;
        if simd {
            if opc == 3 {
                return why!("ldp/stp simd opc 3");
            }
            bytes = 4u32 << opc;
        } else if opc == 0 {
            bytes = 4;
        } else if opc == 2 {
            bytes = 8;
        } else if opc == 1 && load {
            // ldpsw
            bytes = 4;
            sign = 64;
        } else {
            return why!("ldp/stp opc");
        }
        if opc2 == 0 || opc2 == 1 || opc2 == 2 || opc2 == 3 {
            let base = get_base(ss, rn);
            let scaled = imm.wrapping_mul(i64::from(bytes)) as u64;
            let addr = if opc2 == 1 {
                base
            } else {
                base.wrapping_add(scaled)
            };
            if !do_access(uc, addr, bytes, rt, load, simd, sign) {
                return why!("pair access");
            }
            if !do_access(
                uc,
                addr.wrapping_add(u64::from(bytes)),
                bytes,
                rt2,
                load,
                simd,
                sign,
            ) {
                return why!("pair access 2");
            }
            if opc2 == 1 || opc2 == 3 {
                set_base(ss, rn, base.wrapping_add(scaled));
            }
            return Ok(());
        }
        return why!("ldp/stp form");
    }

    // Load/store register: x x 111 V 0 x ... (group A: [25:24]=01 unsigned imm;
    // group B/C: [25:24]=00 with [21]=0 imm9 forms, [21]=1 register offset)
    if insn & 0x0A00_0000 == 0x0800_0000 && (insn >> 27) & 7 == 7 {
        let opc = (insn >> 22) & 3;
        let group = (insn >> 24) & 3;
        let bytes: u32;
        let mut sign = 0u32;
        let load: bool;
        if simd {
            // size:opc -> b (00:00/01), h (01), s (10), d (11), q (00:10/11)
            if size == 0 && opc & 2 != 0 {
                bytes = 16;
            } else {
                bytes = 1u32 << size;
            }
            load = opc & 1 != 0;
        } else {
            bytes = 1u32 << size;
            if size == 3 && opc == 2 {
                // prfm: nothing to do
                (*ss).pc = (*ss).pc.wrapping_add(4);
                return Ok(());
            }
            if opc == 0 {
                load = false;
            } else if opc == 1 {
                load = true;
            } else if opc == 2 {
                load = true;
                sign = 64;
                if size == 3 {
                    return why!("opc 2 size 3");
                }
            } else {
                load = true;
                sign = 32;
                if size >= 2 {
                    return why!("opc 3 size");
                }
            }
        }
        let scale = if simd && bytes == 16 { 4 } else { size };
        let base = get_base(ss, rn);
        let addr: u64;
        let mut wb = 0u32; // 1: post-index, 2: pre-index
        let mut imm = 0i64;
        if group == 1 {
            // An unsigned (32-bit) shift in the C: 12 bits << at most 4.
            addr = base.wrapping_add(u64::from(((insn >> 10) & 0xFFF) << scale));
        } else if group == 0 && insn & (1u32 << 21) == 0 {
            imm = sext(u64::from((insn >> 12) & 0x1FF), 9);
            let idx = (insn >> 10) & 3;
            if idx == 1 {
                addr = base;
                wb = 1;
            } else if idx == 3 {
                addr = base.wrapping_add(imm as u64);
                wb = 2;
            } else {
                addr = base.wrapping_add(imm as u64); // unscaled (0) and unprivileged (2)
            }
        } else if group == 0 && insn & (1u32 << 21) != 0 && (insn >> 10) & 3 == 2 {
            let rm = (insn >> 16) & 31;
            let option = (insn >> 13) & 7;
            let s = (insn >> 12) & 1;
            let m = get_x(ss, rm);
            let off: u64 = match option {
                2 => u64::from(m as u32),       // uxtw
                3 => m,                         // lsl
                6 => i64::from(m as i32) as u64, // sxtw
                7 => m,                         // sxtx
                _ => return why!("register offset option"),
            };
            addr = base.wrapping_add(if s != 0 { off << scale } else { off });
        } else {
            return why!("load/store form");
        }
        if !do_access(uc, addr, bytes, rt, load, simd, sign) {
            return why!("access");
        }
        if wb == 1 {
            set_base(ss, rn, base.wrapping_add(imm as u64));
        } else if wb == 2 {
            set_base(ss, rn, addr);
        }
        return Ok(());
    }

    // Load-acquire / store-release: size 001000 1 L 0 1 11111 1 11111 Rn Rt
    if insn & 0x3FFF_FC00 == 0x089F_FC00 || insn & 0x3FFF_FC00 == 0x08DF_FC00 {
        let load = insn & (1u32 << 22) != 0;
        let bytes = 1u32 << size;
        fence(Ordering::SeqCst);
        if !do_access(uc, get_base(ss, rn), bytes, rt, load, false, 0) {
            return why!("acquire/release access");
        }
        fence(Ordering::SeqCst);
        return Ok(());
    }
    // RCpc loads and stores (FEAT_LRCPC / FEAT_LRCPC2): LDAPR Rt, [Rn] and
    // LDAPUR*/STLUR Rt, [Rn, #imm9]. FEX emits them for x86 loads and stores
    // under TSO once the ID registers say the core has them (stack.rs
    // HWCAP_CPUID): an x64 game reading KUSER_SHARED_DATA did
    // `ldapurb w6, [x6]` on this page, and it went to the guest as a fault.
    if insn & 0x3FFF_FC00 == 0x38BF_C000 {
        // LDAPR{B,H,,}
        let bytes = 1u32 << size;
        if !do_access(uc, get_base(ss, rn), bytes, rt, true, false, 0) {
            return why!("ldapr access");
        }
        fence(Ordering::SeqCst);
        return Ok(());
    }
    if insn & 0x3F20_0C00 == 0x1900_0000 {
        // STLUR / LDAPUR / LDAPURS
        let opc = (insn >> 22) & 3;
        let bytes = 1u32 << size;
        let mut sign = 0u32;
        let load = opc != 0;
        if opc == 2 {
            if size == 3 {
                return why!("ldapurs size 3");
            }
            sign = 64;
        }
        if opc == 3 {
            if size >= 2 {
                return why!("ldapurs size");
            }
            sign = 32;
        }
        let addr = get_base(ss, rn).wrapping_add(sext(u64::from((insn >> 12) & 0x1FF), 9) as u64);
        if !load {
            fence(Ordering::SeqCst);
        }
        if !do_access(uc, addr, bytes, rt, load, false, sign) {
            return why!("rcpc access");
        }
        if load {
            fence(Ordering::SeqCst);
        }
        return Ok(());
    }
    why!("instruction")
}

// libsystem_platform's text, where Darwin's signal trampoline lives.
static G_PLAT_LO: AtomicU64 = AtomicU64::new(0);
static G_PLAT_HI: AtomicU64 = AtomicU64::new(0);

extern "C" fn find_platform_text() {
    unsafe {
        let mut di = DlInfo {
            dli_fname: ptr::null(),
            dli_fbase: ptr::null_mut(),
            dli_sname: ptr::null(),
            dli_saddr: ptr::null_mut(),
        };
        if dladdr(longjmp as *const () as *const c_void, &mut di) == 0
            || di.dli_fbase.is_null()
            || di.dli_fname.is_null()
            || strstr(di.dli_fname, b"libsystem_platform\0".as_ptr().cast()).is_null()
        {
            return;
        }
        let mh = di.dli_fbase as *const MachHeader64;
        let mut lc = mh.add(1) as *const LoadCommand;
        let mut k = 0u32;
        while k < (*mh).ncmds {
            if (*lc).cmd == LC_SEGMENT_64 {
                let seg = lc as *const SegmentCommand64;
                if strcmp(
                    ptr::addr_of!((*seg).segname).cast(),
                    b"__TEXT\0".as_ptr().cast(),
                ) == 0
                {
                    let lo = mh as usize as u64;
                    G_PLAT_LO.store(lo, Ordering::Relaxed);
                    G_PLAT_HI.store(lo.wrapping_add((*seg).vmsize), Ordering::Relaxed);
                }
            }
            k += 1;
            lc = (lc as *const u8).add((*lc).cmdsize as usize) as *const LoadCommand;
        }
    }
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FIND_PLATFORM_TEXT_INIT: extern "C" fn() = find_platform_text;

#[no_mangle]
pub unsafe extern "C" fn lxrt_lowpage_fault(si: *const SigInfo, uap: *mut c_void) -> bool {
    if !lxrt_lowpage_on() || si.is_null() || uap.is_null() {
        return false;
    }
    let fa = (*si).si_addr as usize as u64;
    if fa < LP_LO || fa >= LP_HI {
        return false;
    }
    if page(((fa - LP_LO) / LP_PAGE) as usize)
        .host
        .load(Ordering::SeqCst)
        .is_null()
    {
        return false; // not mapped: a real fault
    }
    let uc = uap as *mut UContext;
    let ss: *mut ArmThreadState64 = ptr::addr_of_mut!((*(*uc).uc_mcontext).ss);
    let pc = (*ss).pc;
    let insn = ptr::read_unaligned(pc as usize as *const u32);
    // The fault arrived with the thread already redirected into Darwin's
    // signal trampoline: an asynchronous signal (Wine suspends threads with
    // SIGUSR1, and Minecraft Dungeons II's integrity scanner suspends every
    // thread of the game over and over) was set up first, and the context
    // handed here is that frame's, not the faulting instruction's. Nothing
    // to emulate at the trampoline: let it run; its sigreturn goes back to
    // the access, which faults again with its own pc and is carried out
    // then. Passed to the guest instead, the game died with a fault in no
    // code of its own (MEASURED, once in a 7-minute run; benchmarks/stage62, 15).
    {
        // _sigtramp is not exported: the image is the test. Darwin's
        // libsystem_platform holds the trampoline, and no guest load ever
        // runs from it (range found at load time, above: dladdr is no call
        // for a signal handler).
        if pc >= G_PLAT_LO.load(Ordering::Relaxed) && pc < G_PLAT_HI.load(Ordering::Relaxed) {
            return true;
        }
    }
    if let Err(why) = emulate(uc, fa, insn) {
        static SAID: AtomicI32 = AtomicI32::new(0);
        if SAID.fetch_add(1, Ordering::SeqCst) < 8 {
            let _ = fprintf(
                lxrt_trace_stream(),
                b"[lxrt] lowpage: cannot carry out %08x at pc 0x%llx for 0x%llx (%s): the fault goes to the guest\n\0"
                    .as_ptr()
                    .cast(),
                insn as c_uint,
                pc as c_ulonglong,
                fa as c_ulonglong,
                why,
            );
        }
        return false;
    }
    // prfm advanced the pc itself
    if (*ss).pc == pc {
        (*ss).pc = pc.wrapping_add(4);
    }
    let n: c_ulong = G_FAULTS.fetch_add(1, Ordering::SeqCst).wrapping_add(1);
    if lxrt_trace_on() && (n <= 4 || n % 100_000 == 0) {
        let _ = fprintf(
            lxrt_trace_stream(),
            b"[lxrt] lowpage: access #%lu %08x at pc 0x%llx for 0x%llx carried out\n\0"
                .as_ptr()
                .cast(),
            n,
            insn as c_uint,
            pc as c_ulonglong,
            fa as c_ulonglong,
        );
    }
    true
}

#[no_mangle]
pub extern "C" fn lxrt_lowpage_faults() -> c_ulong {
    G_FAULTS.load(Ordering::SeqCst)
}
