// Linux process entry stack and auxiliary vector.

use std::ffi::{c_char, c_int, c_void, CStr};
use std::mem::{offset_of, size_of};
use std::os::unix::ffi::OsStrExt;
use std::ptr;
use std::sync::atomic::{AtomicU64, Ordering};

include!(concat!(env!("OUT_DIR"), "/stack_constants.rs"));

const HOST_PAGE: usize = 16_384;
const STACK_SIZE: usize = 8 << 20;
const HWCAP_FP: u64 = 1 << 0;
const HWCAP_ASIMD: u64 = 1 << 1;
const HWCAP_AES: u64 = 1 << 3;
const HWCAP_PMULL: u64 = 1 << 4;
const HWCAP_SHA1: u64 = 1 << 5;
const HWCAP_SHA2: u64 = 1 << 6;
const HWCAP_CRC32: u64 = 1 << 7;
const HWCAP_ATOMICS: u64 = 1 << 8;
const HWCAP_CPUID: u64 = 1 << 11;
const HWCAP_ASIMDRDM: u64 = 1 << 12;

static GUEST_PAGE: AtomicU64 = AtomicU64::new(0);

#[repr(C)]
#[derive(Clone, Copy)]
struct LxrtRange {
    start: u64,
    end: u64,
}

#[repr(C)]
pub struct LxrtImage {
    base: *mut u8,
    load_bias: u64,
    entry: u64,
    phdr: u64,
    phentsize: u16,
    phnum: u16,
    span: usize,
    brk: u64,
    is_pie: bool,
    subpage: bool,
    interp: [c_char; 256],
    interp_base: u64,
    exec: [LxrtRange; 8],
    nexec: c_int,
    code: [LxrtRange; 16],
    ncode: c_int,
}

const _: [(); 320] = [(); offset_of!(LxrtImage, interp_base)];
const _: [(); 328] = [(); offset_of!(LxrtImage, exec)];
const _: [(); 456] = [(); offset_of!(LxrtImage, nexec)];
const _: [(); 464] = [(); offset_of!(LxrtImage, code)];
const _: [(); 720] = [(); offset_of!(LxrtImage, ncode)];
const _: [(); 728] = [(); size_of::<LxrtImage>()];

#[repr(C)]
#[derive(Clone, Copy)]
struct AuxEntry {
    kind: u64,
    value: u64,
}

#[link(name = "System")]
unsafe extern "C" {
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(address: *mut c_void, length: usize) -> c_int;
    fn arc4random_buf(buffer: *mut c_void, length: usize);
    fn malloc(size: usize) -> *mut c_void;
    fn __error() -> *mut c_int;
    fn strerror(error: c_int) -> *const c_char;
    fn getuid() -> u32;
    fn geteuid() -> u32;
    fn getgid() -> u32;
    fn getegid() -> u32;
    fn lxrt_program_is_fex() -> bool;
    fn lxrt_vdso_ehdr() -> u64;
}

fn set_error(error_out: *mut *mut c_char, parts: &[&[u8]]) {
    if error_out.is_null() {
        return;
    }
    let buffer = unsafe { malloc(512).cast::<u8>() };
    if buffer.is_null() {
        unsafe { error_out.write(ptr::null_mut()) };
        return;
    }
    let mut used = 0usize;
    for part in parts {
        let length = part.len().min(511 - used);
        unsafe { ptr::copy_nonoverlapping(part.as_ptr(), buffer.add(used), length) };
        used += length;
        if used == 511 {
            break;
        }
    }
    unsafe {
        buffer.add(used).write(0);
        error_out.write(buffer.cast());
    }
}

fn fail_with_stack(error_out: *mut *mut c_char, message: &[u8], region: *mut c_void) {
    if !region.is_null() {
        unsafe { munmap(region, STACK_SIZE) };
    }
    set_error(error_out, &[message]);
}

#[no_mangle]
pub extern "C" fn lxrt_guest_page() -> u64 {
    let cached = GUEST_PAGE.load(Ordering::Relaxed);
    if cached != 0 {
        return cached;
    }
    let page = if std::env::var_os("LXRT_GUEST_PAGE")
        .map(|value| value.as_os_str().as_bytes() == b"4096")
        .unwrap_or(false)
    {
        4_096
    } else {
        HOST_PAGE as u64
    };
    match GUEST_PAGE.compare_exchange(0, page, Ordering::Relaxed, Ordering::Relaxed) {
        Ok(_) => page,
        Err(existing) => existing,
    }
}

#[no_mangle]
pub extern "C" fn lxrt_hwcap_cpuid() -> bool {
    if let Some(value) = std::env::var_os("LXRT_HWCAP_CPUID") {
        let bytes = value.as_os_str().as_bytes();
        if let Some(first) = bytes.first() {
            return *first != b'0';
        }
    }
    if std::env::var_os("LXRT_NO_HWCAP_CPUID").is_some() {
        return false;
    }
    unsafe { !lxrt_program_is_fex() }
}

fn copy_guest_string(
    source: *const c_char,
    stack_start: usize,
    string_pointer: &mut usize,
) -> Option<u64> {
    if source.is_null() {
        return None;
    }
    let string = unsafe { CStr::from_ptr(source) }.to_bytes_with_nul();
    let current = *string_pointer;
    if string.len() > current.saturating_sub(stack_start) {
        return None;
    }
    let next = current - string.len();
    unsafe { ptr::copy_nonoverlapping(string.as_ptr(), next as *mut u8, string.len()) };
    *string_pointer = next;
    Some(next as u64)
}

fn alloc_ptrs(count: usize, error_out: *mut *mut c_char, region: *mut c_void) -> Option<Vec<u64>> {
    let Some(capacity) = count.checked_add(1) else {
        fail_with_stack(error_out, b"out of memory building the guest stack", region);
        return None;
    };
    let mut pointers = Vec::new();
    if pointers.try_reserve_exact(capacity).is_err() {
        fail_with_stack(error_out, b"out of memory building the guest stack", region);
        return None;
    }
    pointers.resize(capacity, 0);
    Some(pointers)
}

#[no_mangle]
pub extern "C" fn lxrt_build_stack(
    image: *const LxrtImage,
    argc: c_int,
    argv: *const *const c_char,
    envp: *const *const c_char,
    error_out: *mut *mut c_char,
) -> *mut c_void {
    if image.is_null() || argc < 0 || (argc > 0 && argv.is_null()) {
        set_error(error_out, &[b"invalid guest stack arguments"]);
        return ptr::null_mut();
    }
    let argc_usize = argc as usize;
    let mut envc = 0usize;
    unsafe {
        while !envp.is_null() && !(*envp.add(envc)).is_null() {
            envc += 1;
        }
    }

    let region = unsafe {
        mmap(
            ptr::null_mut(),
            STACK_SIZE,
            D_PROT_READ | D_PROT_WRITE,
            D_MAP_PRIVATE | D_MAP_ANON,
            -1,
            0,
        )
    };
    if region as isize == -1 {
        let error = unsafe { *__error() };
        let description = unsafe { CStr::from_ptr(strerror(error)) }.to_bytes();
        set_error(error_out, &[b"guest stack: ", description]);
        return ptr::null_mut();
    }
    let stack_start = region as usize;
    let mut string_pointer = stack_start + STACK_SIZE - HOST_PAGE;

    let mut argv_pointers = match alloc_ptrs(argc_usize, error_out, region) {
        Some(pointers) => pointers,
        None => return ptr::null_mut(),
    };
    let mut envp_pointers = match alloc_ptrs(envc, error_out, region) {
        Some(pointers) => pointers,
        None => return ptr::null_mut(),
    };

    for index in (0..envc).rev() {
        let source = unsafe { *envp.add(index) };
        let Some(pointer) = copy_guest_string(source, stack_start, &mut string_pointer) else {
            fail_with_stack(
                error_out,
                b"guest stack too small for argv/envp strings",
                region,
            );
            return ptr::null_mut();
        };
        envp_pointers[index] = pointer;
    }
    for index in (0..argc_usize).rev() {
        let source = unsafe { *argv.add(index) };
        let Some(pointer) = copy_guest_string(source, stack_start, &mut string_pointer) else {
            fail_with_stack(
                error_out,
                b"guest stack too small for argv/envp strings",
                region,
            );
            return ptr::null_mut();
        };
        argv_pointers[index] = pointer;
    }

    if string_pointer.saturating_sub(stack_start) < 16 {
        fail_with_stack(error_out, b"guest stack too small for AT_RANDOM", region);
        return ptr::null_mut();
    }
    string_pointer -= 16;
    let random = string_pointer as *mut u8;
    unsafe { arc4random_buf(random.cast(), 16) };

    let image = unsafe { &*image };
    let vdso = unsafe { lxrt_vdso_ehdr() };
    let hwcap = HWCAP_FP
        | HWCAP_ASIMD
        | HWCAP_AES
        | HWCAP_PMULL
        | HWCAP_SHA1
        | HWCAP_SHA2
        | HWCAP_CRC32
        | HWCAP_ATOMICS
        | HWCAP_ASIMDRDM
        | if lxrt_hwcap_cpuid() { HWCAP_CPUID } else { 0 };
    let auxiliary = [
        AuxEntry {
            kind: 3,
            value: image.phdr,
        },
        AuxEntry {
            kind: 4,
            value: image.phentsize as u64,
        },
        AuxEntry {
            kind: 5,
            value: image.phnum as u64,
        },
        AuxEntry {
            kind: 6,
            value: lxrt_guest_page(),
        },
        AuxEntry {
            kind: 7,
            value: image.interp_base,
        },
        AuxEntry { kind: 8, value: 0 },
        AuxEntry {
            kind: 9,
            value: image.entry,
        },
        AuxEntry {
            kind: 11,
            value: unsafe { getuid() as u64 },
        },
        AuxEntry {
            kind: 12,
            value: unsafe { geteuid() as u64 },
        },
        AuxEntry {
            kind: 13,
            value: unsafe { getgid() as u64 },
        },
        AuxEntry {
            kind: 14,
            value: unsafe { getegid() as u64 },
        },
        AuxEntry {
            kind: 16,
            value: hwcap,
        },
        AuxEntry { kind: 26, value: 0 },
        AuxEntry {
            kind: 17,
            value: 100,
        },
        AuxEntry { kind: 23, value: 0 },
        AuxEntry {
            kind: 25,
            value: random as usize as u64,
        },
        AuxEntry {
            kind: if vdso != 0 { 33 } else { 1 },
            value: vdso,
        },
        AuxEntry { kind: 0, value: 0 },
    ];
    let words = match 1usize
        .checked_add(argc_usize + 1)
        .and_then(|value| value.checked_add(envc + 1))
        .and_then(|value| value.checked_add(auxiliary.len() * 2))
    {
        Some(words) => words,
        None => {
            fail_with_stack(error_out, b"guest stack too small", region);
            return ptr::null_mut();
        }
    };
    let Some(stack_bytes) = words.checked_mul(size_of::<u64>()) else {
        fail_with_stack(error_out, b"guest stack too small", region);
        return ptr::null_mut();
    };
    let Some(unrounded_stack) = string_pointer.checked_sub(stack_bytes) else {
        fail_with_stack(error_out, b"guest stack too small", region);
        return ptr::null_mut();
    };
    let stack_pointer = unrounded_stack & !15usize;
    if stack_pointer < stack_start {
        let message = format!("guest stack too small for {} argv/envp bytes", stack_bytes);
        fail_with_stack(error_out, message.as_bytes(), region);
        return ptr::null_mut();
    }

    let mut output = stack_pointer as *mut u64;
    unsafe {
        output.write(argc_usize as u64);
        output = output.add(1);
        for pointer in argv_pointers.iter().take(argc_usize) {
            output.write(*pointer);
            output = output.add(1);
        }
        output.write(0);
        output = output.add(1);
        for pointer in envp_pointers.iter().take(envc) {
            output.write(*pointer);
            output = output.add(1);
        }
        output.write(0);
        output = output.add(1);
        for entry in auxiliary {
            output.write(entry.kind);
            output = output.add(1);
            output.write(entry.value);
            output = output.add(1);
        }
    }
    stack_pointer as *mut c_void
}
