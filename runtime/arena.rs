//! Reserve Darwin address space for guest fixed mappings and the guest break.
//!
//! The linker-provided `__LXRT_ARENA` zero-fill segment reserves the Windows
//! image range before the host allocator starts. Guest mappings may replace
//! pages in that segment; after guest unmaps, empty host pages are reserved
//! again so Darwin's heap cannot claim them. `LXRT_NO_ARENA=1` disables guest
//! use of both arenas for bisecting.

use std::ffi::{c_char, c_int, c_void};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicU8, Ordering};

include!(concat!(env!("OUT_DIR"), "/arena_constants.rs"));

const HOST_PAGE: u64 = 16_384;
const SEG_MAX_PAGES: usize = 0x2500_0000usize / HOST_PAGE as usize;
const HEAP_SPAN: u64 = 4_u64 << 30;
const HEAP_PAGES: usize = (HEAP_SPAN / HOST_PAGE) as usize;

// Values from the public macOS Mach VM headers.
const VM_FLAGS_FIXED: c_int = 0;
const VM_PROT_NONE: c_int = 0;
const VM_PROT_ALL: c_int = 7;
const VM_INHERIT_COPY: c_int = 1;

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

struct Arena {
    lo: AtomicU64,
    hi: AtomicU64,
    used: &'static [AtomicU8],
}

static SEG_USED: [AtomicU8; SEG_MAX_PAGES / 8] = [const { AtomicU8::new(0) }; SEG_MAX_PAGES / 8];
static HEAP_USED: [AtomicU8; HEAP_PAGES / 8] = [const { AtomicU8::new(0) }; HEAP_PAGES / 8];
static ARENAS: [Arena; 2] = [
    Arena {
        lo: AtomicU64::new(0),
        hi: AtomicU64::new(0),
        used: &SEG_USED,
    },
    Arena {
        lo: AtomicU64::new(0),
        hi: AtomicU64::new(0),
        used: &HEAP_USED,
    },
];
static NO_ARENA: AtomicBool = AtomicBool::new(false);

#[link(name = "System")]
unsafe extern "C" {
    static __dso_handle: MachHeader64;
    static mach_task_self_: u32;

    fn getsegmentdata(
        image: *const MachHeader64,
        segment: *const c_char,
        size: *mut usize,
    ) -> *mut u8;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn mach_vm_map(
        target_task: u32,
        address: *mut u64,
        size: u64,
        mask: u64,
        flags: c_int,
        object: u32,
        offset: u64,
        copy: c_int,
        current_protection: c_int,
        maximum_protection: c_int,
        inheritance: c_int,
    ) -> c_int;
}

fn align_down(value: u64) -> u64 {
    value & !(HOST_PAGE - 1)
}

fn align_up(value: u64) -> Option<u64> {
    value.checked_add(HOST_PAGE - 1).map(align_down)
}

extern "C" fn arena_find() {
    let off = unsafe { getenv(b"LXRT_NO_ARENA\0".as_ptr().cast()) };
    NO_ARENA.store(
        !off.is_null() && unsafe { *off as u8 == b'1' },
        Ordering::SeqCst,
    );

    let mut size = 0usize;
    let segment = unsafe {
        getsegmentdata(
            ptr::addr_of!(__dso_handle),
            b"__LXRT_ARENA\0".as_ptr().cast(),
            &mut size,
        )
    };
    if segment.is_null() || size == 0 || NO_ARENA.load(Ordering::SeqCst) {
        return;
    }

    let Some(segment_end) = (segment as usize as u64).checked_add(size as u64) else {
        return;
    };
    let Some(lo) = align_up(segment as usize as u64) else {
        return;
    };
    let mut hi = align_down(segment_end);
    if hi < lo {
        return;
    }
    let max_span = SEG_MAX_PAGES as u64 * HOST_PAGE;
    if hi - lo > max_span {
        let Some(capped_hi) = lo.checked_add(max_span) else {
            return;
        };
        hi = capped_hi;
    }
    if hi <= lo {
        return;
    }

    // Replace the linker's no-access segment with an ordinary reservation so
    // later guest mappings can safely replace individual pages.
    let address = lo as usize as *mut c_void;
    let mapped = unsafe {
        mmap(
            address,
            (hi - lo) as usize,
            0,
            D_MAP_PRIVATE | D_MAP_ANON | D_MAP_FIXED,
            -1,
            0,
        )
    };
    if mapped != address {
        return;
    }
    ARENAS[0].lo.store(lo, Ordering::SeqCst);
    ARENAS[0].hi.store(hi, Ordering::SeqCst);
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static ARENA_INIT: extern "C" fn() = arena_find;

#[no_mangle]
pub extern "C" fn lxrt_arena_on() -> bool {
    ARENAS[0].hi.load(Ordering::SeqCst) != 0
}

#[no_mangle]
pub extern "C" fn lxrt_arena_bounds(lo: *mut u64, hi: *mut u64) {
    unsafe {
        if !lo.is_null() {
            lo.write(ARENAS[0].lo.load(Ordering::SeqCst));
        }
        if !hi.is_null() {
            hi.write(ARENAS[0].hi.load(Ordering::SeqCst));
        }
    }
}

#[no_mangle]
pub extern "C" fn lxrt_arena_reserve_heap() -> u64 {
    if NO_ARENA.load(Ordering::SeqCst) {
        return 0;
    }
    let mapped = unsafe {
        mmap(
            ptr::null_mut(),
            HEAP_SPAN as usize,
            0,
            D_MAP_PRIVATE | D_MAP_ANON,
            -1,
            0,
        )
    };
    if mapped as isize == -1 {
        return 0;
    }
    let lo = mapped as usize as u64;
    ARENAS[1].lo.store(lo, Ordering::SeqCst);
    ARENAS[1].hi.store(lo + HEAP_SPAN, Ordering::SeqCst);
    lo
}

fn arena_of(address: u64) -> Option<&'static Arena> {
    ARENAS.iter().find(|arena| {
        let lo = arena.lo.load(Ordering::SeqCst);
        let hi = arena.hi.load(Ordering::SeqCst);
        hi != 0 && address >= lo && address < hi
    })
}

// Return the host-page indexes touched by the part of [address, address+len)
// that lies in this arena.
fn clip(arena: &Arena, address: u64, len: u64) -> Option<(u64, u64)> {
    let end = address.checked_add(len)?;
    let arena_lo = arena.lo.load(Ordering::SeqCst);
    let arena_hi = arena.hi.load(Ordering::SeqCst);
    if arena_hi == 0 || len == 0 {
        return None;
    }
    let lo = address.max(arena_lo);
    let hi = end.min(arena_hi);
    if lo >= hi {
        return None;
    }
    let first = (align_down(lo) - arena_lo) / HOST_PAGE;
    let last = (align_up(hi)? - arena_lo) / HOST_PAGE;
    Some((first, last))
}

fn page_used(arena: &Arena, index: u64) -> bool {
    let byte = arena.used[(index / 8) as usize].load(Ordering::SeqCst);
    byte & (1 << (index % 8)) != 0
}

fn page_set(arena: &Arena, index: u64, used: bool) {
    let mask = 1u8 << (index % 8);
    let byte = &arena.used[(index / 8) as usize];
    if used {
        byte.fetch_or(mask, Ordering::SeqCst);
    } else {
        byte.fetch_and(!mask, Ordering::SeqCst);
    }
}

#[no_mangle]
pub extern "C" fn lxrt_arena_free(address: u64, len: u64) -> bool {
    let Some(arena) = arena_of(address) else {
        return false;
    };
    let Some(end) = address.checked_add(len) else {
        return false;
    };
    if len == 0 || end > arena.hi.load(Ordering::SeqCst) {
        return false;
    }
    let Some((first, last)) = clip(arena, address, len) else {
        return false;
    };
    !(first..last).any(|index| page_used(arena, index))
}

#[no_mangle]
pub extern "C" fn lxrt_arena_mapped(address: u64, len: u64) {
    for arena in &ARENAS {
        if let Some((first, last)) = clip(arena, address, len) {
            for index in first..last {
                page_set(arena, index, true);
            }
        }
    }
}

#[no_mangle]
pub extern "C" fn lxrt_arena_unmapped(address: u64, len: u64) {
    let task = unsafe { mach_task_self_ };
    for arena in &ARENAS {
        let Some((first, last)) = clip(arena, address, len) else {
            continue;
        };
        let base = arena.lo.load(Ordering::SeqCst);
        let mut all = base + first * HOST_PAGE;
        let result = unsafe {
            mach_vm_map(
                task,
                &mut all,
                (last - first) * HOST_PAGE,
                0,
                VM_FLAGS_FIXED,
                0,
                0,
                0,
                VM_PROT_NONE,
                VM_PROT_ALL,
                VM_INHERIT_COPY,
            )
        };
        if result == 0 {
            for index in first..last {
                page_set(arena, index, false);
            }
            continue;
        }
        for index in first..last {
            let mut at = base + index * HOST_PAGE;
            let result = unsafe {
                mach_vm_map(
                    task,
                    &mut at,
                    HOST_PAGE,
                    0,
                    VM_FLAGS_FIXED,
                    0,
                    0,
                    0,
                    VM_PROT_NONE,
                    VM_PROT_ALL,
                    VM_INHERIT_COPY,
                )
            };
            if result == 0 {
                page_set(arena, index, false);
            }
        }
    }
}
