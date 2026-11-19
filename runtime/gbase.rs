//! Guest pointer translation and low-window fault repair for 32-bit guests.
//!
//! Darwin cannot map the low 4 GiB. FEX places a 32-bit guest at host
//! address base + guest address, but pointer arguments passed straight through
//! Linux syscalls still need rebasing here. Memory-management addresses remain
//! FEX-owned and are intentionally excluded from the syscall table.

use std::cell::UnsafeCell;
use std::ffi::{c_char, c_int, c_long, c_ulonglong, c_void};
use std::ptr;
use std::sync::atomic::{AtomicI32, AtomicU64, Ordering};

const A0: u8 = 1 << 0;
const A1: u8 = 1 << 1;
const A2: u8 = 1 << 2;
const A3: u8 = 1 << 3;
const A4: u8 = 1 << 4;
const A5: u8 = 1 << 5;
const GUEST_LIMIT: u64 = 1 << 32;
const GUEST_PTR_MIN: u64 = 0x10000;
const GB_MAX_IOV: usize = 1024;

const PTR_ARGS: &[(u16, u8)] = &[
    (5, A0 | A1 | A2),
    (6, A0 | A1 | A2),
    (7, A1 | A2),
    (8, A0 | A1 | A2),
    (9, A0 | A1 | A2),
    (10, A1 | A2),
    (11, A0 | A1),
    (12, A0 | A1),
    (13, A1),
    (14, A0 | A1),
    (15, A0 | A1),
    (16, A1),
    (17, A0),
    (21, A3),
    (22, A1 | A4),
    (27, A1),
    (33, A1),
    (34, A1),
    (35, A1),
    (36, A0 | A2),
    (37, A1 | A3),
    (38, A1 | A3),
    (40, A0 | A1 | A2 | A4),
    (41, A0),
    (43, A0 | A1),
    (44, A1),
    (45, A0),
    (48, A1),
    (49, A0),
    (51, A0),
    (53, A1),
    (54, A1),
    (56, A1),
    (59, A0),
    (61, A1),
    (63, A1),
    (64, A1),
    (65, A1),
    (66, A1),
    (67, A1),
    (68, A1),
    (69, A1),
    (70, A1),
    (71, A2),
    (72, A1 | A2 | A3 | A4 | A5),
    (73, A0 | A2 | A3),
    (74, A1),
    (76, A1 | A3),
    (78, A1 | A2),
    (79, A1 | A2),
    (80, A1),
    (86, A2 | A3),
    (87, A1),
    (88, A1 | A2),
    (90, A1),
    (91, A1),
    (95, A2 | A4),
    (96, A0),
    (99, A0),
    (100, A1 | A2),
    (101, A0 | A1),
    (102, A1),
    (103, A1 | A2),
    (107, A1 | A2),
    (108, A1),
    (110, A2 | A3),
    (111, A1),
    (112, A1),
    (113, A1),
    (114, A1),
    (115, A2 | A3),
    (116, A1),
    (118, A1),
    (119, A2),
    (121, A1),
    (122, A2),
    (123, A2),
    (127, A1),
    (132, A0 | A1),
    (133, A0),
    (134, A1 | A2),
    (135, A1 | A2),
    (136, A0),
    (137, A0 | A1 | A2),
    (138, A2),
    (158, A1),
    (159, A1),
    (160, A0),
    (161, A0),
    (162, A0),
    (163, A1),
    (164, A1),
    (165, A1),
    (168, A0 | A1 | A2),
    (169, A0 | A1),
    (170, A0 | A1),
    (171, A0),
    (179, A0),
    (187, A2),
    (188, A1),
    (189, A1),
    (192, A1 | A3),
    (193, A1),
    (195, A2),
    (199, A3),
    (200, A1),
    (202, A1 | A2),
    (203, A1),
    (204, A1 | A2),
    (205, A1 | A2),
    (206, A1 | A4),
    (207, A1 | A4 | A5),
    (208, A3),
    (209, A3 | A4),
    (211, A1),
    (212, A1),
    (221, A0 | A1 | A2),
    (232, A2),
    (236, A0 | A1),
    (237, A1),
    (240, A3),
    (242, A1 | A2),
    (243, A1 | A4),
    (260, A1 | A3),
    (261, A2 | A3),
    (263, A4),
    (264, A1 | A2 | A3),
    (265, A1),
    (266, A1),
    (269, A1),
    (270, A1 | A3),
    (271, A1 | A3),
    (274, A1),
    (275, A1),
    (276, A1 | A3),
    (277, A2),
    (278, A0),
    (279, A0),
    (281, A1 | A2 | A3),
    (285, A1 | A3),
    (286, A1),
    (287, A1),
    (291, A1 | A4),
    (424, A2),
    (437, A1 | A2),
    (439, A1),
    (441, A1 | A2 | A4),
    (449, A0 | A3),
    (452, A1),
    (454, A0),
    (455, A0 | A4),
    (456, A0),
];

static GUEST_BASE: AtomicU64 = AtomicU64::new(0);
static SELF_TEXT_LO: AtomicU64 = AtomicU64::new(0);
static SELF_TEXT_HI: AtomicU64 = AtomicU64::new(0);
static LOWPTR_FIXUPS: AtomicU64 = AtomicU64::new(0);
static LOWPTR_DISABLED: AtomicI32 = AtomicI32::new(-1);

#[repr(C)]
#[derive(Clone, Copy)]
struct GuestIov {
    base: u64,
    len: u64,
}

impl GuestIov {
    const EMPTY: Self = Self { base: 0, len: 0 };
}

thread_local! {
    static IOV_COPY: UnsafeCell<[GuestIov; GB_MAX_IOV]> =
        const { UnsafeCell::new([GuestIov::EMPTY; GB_MAX_IOV]) };
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
struct ArmThreadState64 {
    x: [u64; 29],
    fp: u64,
    lr: u64,
    sp: u64,
    pc: u64,
    cpsr: u32,
    pad: u32,
}

const _: [(); 272] = [(); std::mem::size_of::<ArmThreadState64>()];

#[link(name = "System")]
unsafe extern "C" {
    static __dso_handle: MachHeader64;
    static mut lxrt_main_image_base: u64;
    static mut lxrt_main_image_span: u64;

    fn getsegmentdata(
        image: *const MachHeader64,
        segment: *const c_char,
        size: *mut usize,
    ) -> *mut u8;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
    fn lxrt_arm64_thread_state(ucontext: *mut c_void) -> *mut c_void;
}

fn guest_ptr(value: u64) -> bool {
    value != 0 && value < GUEST_LIMIT
}

unsafe fn translate(args: *mut u64, mask: u8, base: u64) {
    for index in 0..6 {
        if mask & (1 << index) != 0 {
            let slot = args.add(index);
            let value = slot.read();
            if guest_ptr(value) {
                slot.write(value.wrapping_add(base));
            }
        }
    }
}

unsafe fn translate_iov(array_slot: *mut u64, count: u64, base: u64) {
    let guest_array = array_slot.read();
    if guest_array == 0 || count == 0 || count > GB_MAX_IOV as u64 {
        return;
    }
    let input = guest_array as usize as *const GuestIov;
    let count = count as usize;
    let mut any_guest_pointer = false;
    for index in 0..count {
        if guest_ptr(input.add(index).read().base) {
            any_guest_pointer = true;
            break;
        }
    }
    if !any_guest_pointer {
        return;
    }
    IOV_COPY.with(|copy| {
        let output = (*copy.get()).as_mut_ptr();
        for index in 0..count {
            let mut entry = input.add(index).read();
            if guest_ptr(entry.base) {
                entry.base = entry.base.wrapping_add(base);
            }
            output.add(index).write(entry);
        }
        array_slot.write(output as usize as u64);
    });
}

#[no_mangle]
pub extern "C" fn lxrt_gbase_set(base: u64) {
    GUEST_BASE.store(base, Ordering::SeqCst);
    unsafe {
        if lxrt_trace_on() {
            fprintf(
                lxrt_trace_stream(),
                b"[lxrt] guest address base 0x%llx announced\n\0"
                    .as_ptr()
                    .cast(),
                base as c_ulonglong,
            );
        }
    }
}

#[no_mangle]
pub extern "C" fn lxrt_gbase() -> u64 {
    GUEST_BASE.load(Ordering::SeqCst)
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_gbase_apply(nr: c_long, args: *mut u64) {
    let base = GUEST_BASE.load(Ordering::SeqCst);
    if base == 0 || args.is_null() {
        return;
    }
    match nr {
        65 | 66 | 69 | 70 | 286 | 287 => {
            translate(args, A1, base);
            translate_iov(args.add(1), args.add(2).read(), base);
            return;
        }
        25 => {
            let cmd = args.add(1).read() as u32;
            if matches!(cmd, 5 | 6 | 7 | 36 | 37 | 38 | 15 | 16) {
                translate(args, A2, base);
            }
            return;
        }
        29 => {
            let request = args.add(1).read() as u32;
            if ((request >> 8) & 0xff) == b'b' as u32 && (request >> 30) != 0 {
                translate(args, A2, base);
                return;
            }
            if matches!(
                request,
                0x5401
                    | 0x5402
                    | 0x5403
                    | 0x5404
                    | 0x802c542a
                    | 0x402c542b
                    | 0x402c542c
                    | 0x402c542d
                    | 0x5413
                    | 0x5414
                    | 0x540f
                    | 0x5410
                    | 0x5429
                    | 0x5411
                    | 0x541b
                    | 0x5421
            ) {
                translate(args, A2, base);
            }
            return;
        }
        98 => {
            let op = (args.add(1).read() as u32) & 0x7f;
            let mut mask = A0;
            if matches!(op, 0 | 9 | 6 | 13 | 11) {
                mask |= A3;
            }
            if matches!(op, 3 | 4 | 5 | 11 | 12) {
                mask |= A4;
            }
            translate(args, mask, base);
            return;
        }
        167 => {
            let option = args.read() as u32;
            if option == 15 || option == 16 {
                translate(args, A1, base);
            }
            return;
        }
        191 => {
            let command = (args.add(2).read() as u32) & 0xff;
            if command != 16 {
                translate(args, A3, base);
            }
            return;
        }
        _ => {}
    }

    if let Some((_, mask)) = PTR_ARGS
        .iter()
        .find(|(syscall, _)| *syscall as c_long == nr)
    {
        translate(args, *mask, base);
        return;
    }
    // These addresses and pointer-like values are owned by FEX's allocator.
    if nr == 214
        || nr == 215
        || nr == 216
        || nr == 222
        || (226..=234).contains(&nr)
        || nr == 233
        || nr == 284
        || nr == 167
    {
        return;
    }
    unsafe {
        if lxrt_trace_on() {
            for index in 0..6 {
                let value = args.add(index).read();
                if guest_ptr(value) && value >= GUEST_PTR_MIN {
                    fprintf(
                        lxrt_trace_stream(),
                        b"[lxrt] guest-base: syscall %ld arg%d = 0x%llx below 4 GiB, not in the pointer table\n\0"
                            .as_ptr()
                            .cast(),
                        nr,
                        index as c_int,
                        value as c_ulonglong,
                    );
                }
            }
        }
    }
}

extern "C" fn self_text_range() {
    let mut size = 0usize;
    let text = unsafe {
        getsegmentdata(
            ptr::addr_of!(__dso_handle),
            b"__TEXT\0".as_ptr().cast(),
            &mut size,
        )
    };
    let lo = text as usize as u64;
    SELF_TEXT_LO.store(lo, Ordering::SeqCst);
    SELF_TEXT_HI.store(lo.wrapping_add(size as u64), Ordering::SeqCst);
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static SELF_TEXT_RANGE_INIT: extern "C" fn() = self_text_range;

fn lowptr_fixup_disabled() -> bool {
    let cached = LOWPTR_DISABLED.load(Ordering::Acquire);
    if cached >= 0 {
        return cached != 0;
    }
    let disabled = unsafe { !getenv(b"LXRT_NO_LOWPTR_FIXUP\0".as_ptr().cast()).is_null() };
    let value = i32::from(disabled);
    match LOWPTR_DISABLED.compare_exchange(-1, value, Ordering::SeqCst, Ordering::SeqCst) {
        Ok(_) => disabled,
        Err(existing) => existing != 0,
    }
}

#[no_mangle]
pub extern "C" fn lxrt_lowptr_fixup(ucontext: *mut c_void, fault_addr: u64) -> bool {
    let base = GUEST_BASE.load(Ordering::SeqCst);
    if lowptr_fixup_disabled()
        || base == 0
        || ucontext.is_null()
        || fault_addr < GUEST_PTR_MIN
        || fault_addr >= GUEST_LIMIT
    {
        return false;
    }
    let state = unsafe { lxrt_arm64_thread_state(ucontext).cast::<ArmThreadState64>() };
    if state.is_null() {
        return false;
    }
    let pc = unsafe { (*state).pc };
    // A low-PC fetch fault has no instruction to decode and reading it here
    // would fault again inside the signal handler.
    if pc < GUEST_LIMIT {
        return false;
    }
    let (image_base, image_span) = unsafe {
        (
            ptr::addr_of!(lxrt_main_image_base).read(),
            ptr::addr_of!(lxrt_main_image_span).read(),
        )
    };
    let image_end = image_base.wrapping_add(image_span);
    let own_lo = SELF_TEXT_LO.load(Ordering::SeqCst);
    let own_hi = SELF_TEXT_HI.load(Ordering::SeqCst);
    if (pc >= image_base && pc < image_end) || (pc >= own_lo && pc < own_hi) {
        return false;
    }
    let instruction = unsafe { (pc as usize as *const u32).read() };
    if (instruction & 0x0a00_0000) != 0x0800_0000 {
        return false;
    }
    let rn = (instruction >> 5) & 31;
    if rn == 31 {
        return false;
    }
    let register = unsafe {
        match rn {
            29 => ptr::addr_of_mut!((*state).fp),
            30 => ptr::addr_of_mut!((*state).lr),
            _ => (*state).x.as_mut_ptr().add(rn as usize),
        }
    };
    let value = unsafe { register.read() };
    if value < GUEST_PTR_MIN || value >= GUEST_LIMIT {
        return false;
    }
    unsafe { register.write(value.wrapping_add(base)) };
    let count = LOWPTR_FIXUPS.fetch_add(1, Ordering::SeqCst) + 1;
    if count <= 8 || count & (count - 1) == 0 {
        unsafe {
            if lxrt_trace_on() {
                fprintf(
                    lxrt_trace_stream(),
                    b"[lxrt] low guest pointer fixed up in host code: x%u 0x%llx at pc 0x%llx (%lu so far)\n\0"
                        .as_ptr()
                        .cast(),
                    rn,
                    value as c_ulonglong,
                    pc as c_ulonglong,
                    count as c_ulonglong,
                );
            }
        }
    }
    true
}
