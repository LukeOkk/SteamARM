//! Load a PIE aarch64 Linux ELF into the current macOS process.
//!
//! Darwin reserves the low 4 GiB as `__PAGEZERO`, so non-PIE Linux images
//! cannot be placed at their link address. FEX handles x86 images separately.

use std::ffi::{c_char, c_int, c_long, c_void, CStr, OsStr};
use std::fs::File;
use std::os::unix::ffi::OsStrExt;
use std::os::unix::io::AsRawFd;
use std::path::Path;
use std::ptr;

mod generated {
    #[allow(dead_code)]
    pub mod privmap {
        include!(concat!(env!("OUT_DIR"), "/privmap_constants.rs"));
    }
}

const HOST_PAGE: u64 = 16_384;
const ELF_HEADER_SIZE: usize = 64;
const ELF_PROGRAM_HEADER_SIZE: usize = 56;
const ET_EXEC: u16 = 2;
const ET_DYN: u16 = 3;
const EM_AARCH64: u16 = 183;
const PT_LOAD: u32 = 1;
const PT_INTERP: u32 = 3;
const PF_X: u32 = 1;
const PF_W: u32 = 2;
const PF_R: u32 = 4;
const PROT_READ: c_int = generated::privmap::D_PROT_READ;
const PROT_WRITE: c_int = generated::privmap::D_PROT_WRITE;
const PROT_EXEC: c_int = generated::privmap::D_PROT_EXEC;
const MAP_PRIVATE: c_int = generated::privmap::D_MAP_PRIVATE;
const MAP_ANON: c_int = generated::privmap::D_MAP_ANON;

#[repr(C)]
#[derive(Clone, Copy, Default)]
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

impl LxrtImage {
    fn clear(&mut self) {
        unsafe { ptr::write_bytes(self as *mut Self, 0, 1) }
    }
}

#[derive(Clone, Copy)]
struct ProgramHeader {
    kind: u32,
    flags: u32,
    offset: u64,
    vaddr: u64,
    filesz: u64,
    memsz: u64,
    align: u64,
}

struct Mapping {
    base: *mut u8,
    len: usize,
    keep: bool,
}

impl Drop for Mapping {
    fn drop(&mut self) {
        if !self.keep && !self.base.is_null() {
            unsafe {
                let _ = munmap(self.base.cast(), self.len);
            }
        }
    }
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
    fn mprotect(address: *mut c_void, length: usize, protection: c_int) -> c_int;
    fn malloc(size: usize) -> *mut c_void;
    fn lxrt_subpage_mprotect(address: u64, length: u64, protection: c_int) -> c_long;
    fn lxrt_elf_exec_sections_path(
        path: *const c_char,
        load_bias: u64,
        out: *mut LxrtRange,
        max: c_int,
    ) -> c_int;
}

fn read_u16(bytes: &[u8], at: usize) -> Option<u16> {
    Some(u16::from_le_bytes(bytes.get(at..at + 2)?.try_into().ok()?))
}

fn read_u32(bytes: &[u8], at: usize) -> Option<u32> {
    Some(u32::from_le_bytes(bytes.get(at..at + 4)?.try_into().ok()?))
}

fn read_u64(bytes: &[u8], at: usize) -> Option<u64> {
    Some(u64::from_le_bytes(bytes.get(at..at + 8)?.try_into().ok()?))
}

fn program_header(bytes: &[u8], at: usize) -> Option<ProgramHeader> {
    Some(ProgramHeader {
        kind: read_u32(bytes, at)?,
        flags: read_u32(bytes, at + 4)?,
        offset: read_u64(bytes, at + 8)?,
        vaddr: read_u64(bytes, at + 16)?,
        filesz: read_u64(bytes, at + 32)?,
        memsz: read_u64(bytes, at + 40)?,
        align: read_u64(bytes, at + 48)?,
    })
}

fn align_down(value: u64, alignment: u64) -> u64 {
    value & !(alignment - 1)
}

fn align_up(value: u64, alignment: u64) -> Option<u64> {
    value
        .checked_add(alignment - 1)
        .map(|rounded| align_down(rounded, alignment))
}

fn prot_of(flags: u32) -> c_int {
    let mut protection = 0;
    if flags & PF_R != 0 {
        protection |= PROT_READ;
    }
    if flags & PF_W != 0 {
        protection |= PROT_WRITE;
    }
    if flags & PF_X != 0 {
        protection |= PROT_EXEC;
    }
    protection
}

fn path_text(path: &CStr) -> String {
    path.to_string_lossy().into_owned()
}

fn write_error(err: *mut *mut c_char, message: String) -> c_int {
    if !err.is_null() {
        unsafe {
            *err = ptr::null_mut();
            let buffer = malloc(512).cast::<u8>();
            if !buffer.is_null() {
                let bytes = message.as_bytes();
                let len = bytes.len().min(511);
                ptr::copy_nonoverlapping(bytes.as_ptr(), buffer, len);
                *buffer.add(len) = 0;
                *err = buffer.cast();
            }
        }
    }
    -1
}

fn load_elf(path: &CStr, out: &mut LxrtImage) -> Result<(), String> {
    let path_name = path_text(path);
    let file = File::open(Path::new(OsStr::from_bytes(path.to_bytes())))
        .map_err(|error| format!("open {path_name}: {error}"))?;
    let size = file
        .metadata()
        .map_err(|error| format!("fstat {path_name}: {error}"))?
        .len();
    let map_len = usize::try_from(size)
        .map_err(|_| format!("mmap {path_name}: file is too large for this process"))?;
    let mapped = unsafe {
        mmap(
            ptr::null_mut(),
            map_len,
            PROT_READ,
            MAP_PRIVATE,
            file.as_raw_fd(),
            0,
        )
    };
    if mapped as isize == -1 {
        return Err(format!(
            "mmap {path_name}: {}",
            std::io::Error::last_os_error()
        ));
    }
    drop(file);
    let file_map = Mapping {
        base: mapped.cast(),
        len: map_len,
        keep: false,
    };
    let bytes = unsafe { std::slice::from_raw_parts(file_map.base, file_map.len) };

    if bytes.len() < ELF_HEADER_SIZE {
        return Err(format!("{path_name}: too small to be an ELF"));
    }
    if bytes.get(0..4) != Some(b"\x7fELF") {
        return Err(format!("{path_name}: not an ELF"));
    }
    if bytes[4] != 2 || bytes[5] != 1 {
        return Err(format!("{path_name}: not a 64-bit little-endian ELF"));
    }
    let kind = read_u16(bytes, 16).ok_or_else(|| format!("{path_name}: truncated ELF header"))?;
    let machine =
        read_u16(bytes, 18).ok_or_else(|| format!("{path_name}: truncated ELF header"))?;
    let entry = read_u64(bytes, 24).ok_or_else(|| format!("{path_name}: truncated ELF header"))?;
    let phoff = read_u64(bytes, 32).ok_or_else(|| format!("{path_name}: truncated ELF header"))?;
    let phentsize =
        read_u16(bytes, 54).ok_or_else(|| format!("{path_name}: truncated ELF header"))?;
    let phnum = read_u16(bytes, 56).ok_or_else(|| format!("{path_name}: truncated ELF header"))?;
    if machine != EM_AARCH64 {
        return Err(format!(
            "{path_name}: e_machine {machine}, only aarch64 is implemented (x86-64 goes through FEX, Stage 5)"
        ));
    }
    if kind != ET_EXEC && kind != ET_DYN {
        return Err(format!("{path_name}: e_type {kind}, expected EXEC or DYN"));
    }
    if kind == ET_EXEC {
        return Err(format!(
            concat!(
                "{}: non-PIE (ET_EXEC) images cannot be loaded. It links at ",
                "0x{:x}, inside Darwin's 4 GiB __PAGEZERO, which cannot be ",
                "shrunk or mapped over on arm64, and an ET_EXEC image has no ",
                "relocations to move it. Rebuild it as PIE, or run it through ",
                "FEX where guest addresses are not host addresses."
            ),
            path_name, entry
        ));
    }
    if phentsize as usize != ELF_PROGRAM_HEADER_SIZE {
        return Err(format!(
            "{path_name}: program header size {phentsize}, expected {ELF_PROGRAM_HEADER_SIZE}"
        ));
    }
    let phoff = usize::try_from(phoff)
        .map_err(|_| format!("{path_name}: program headers are out of range"))?;
    let phbytes = (phnum as usize)
        .checked_mul(phentsize as usize)
        .ok_or_else(|| format!("{path_name}: program headers are out of range"))?;
    let phend = phoff
        .checked_add(phbytes)
        .ok_or_else(|| format!("{path_name}: program headers are out of range"))?;
    if phend > bytes.len() {
        return Err(format!("{path_name}: program headers are out of range"));
    }
    let headers = (0..phnum as usize)
        .map(|index| program_header(bytes, phoff + index * phentsize as usize))
        .collect::<Option<Vec<_>>>()
        .ok_or_else(|| format!("{path_name}: truncated program header"))?;

    for header in headers.iter().filter(|header| header.kind == PT_INTERP) {
        if header.filesz == 0 || header.filesz >= out.interp.len() as u64 {
            return Err(format!(
                "{path_name}: PT_INTERP is {} bytes, out of range",
                header.filesz
            ));
        }
        let start = usize::try_from(header.offset)
            .map_err(|_| format!("{path_name}: PT_INTERP is out of range"))?;
        let len = header.filesz as usize;
        let end = start
            .checked_add(len)
            .ok_or_else(|| format!("{path_name}: PT_INTERP is out of range"))?;
        let interp = bytes
            .get(start..end)
            .ok_or_else(|| format!("{path_name}: PT_INTERP is out of range"))?;
        for (dst, src) in out.interp.iter_mut().zip(interp) {
            *dst = *src as c_char;
        }
        out.interp[len] = 0;
    }

    let mut low = u64::MAX;
    let mut high = 0u64;
    let mut loads = 0usize;
    let mut subpage = false;
    for (index, header) in headers
        .iter()
        .enumerate()
        .filter(|(_, header)| header.kind == PT_LOAD)
    {
        loads += 1;
        low = low.min(header.vaddr);
        high = high.max(
            header
                .vaddr
                .checked_add(header.memsz)
                .ok_or_else(|| format!("{path_name}: PT_LOAD {index} address overflows"))?,
        );
        if header.filesz > header.memsz {
            return Err(format!("{path_name}: PT_LOAD {index} filesz exceeds memsz"));
        }
        let file_start = usize::try_from(header.offset)
            .map_err(|_| format!("{path_name}: PT_LOAD {index} is out of range"))?;
        let file_len = usize::try_from(header.filesz)
            .map_err(|_| format!("{path_name}: PT_LOAD {index} is out of range"))?;
        if file_start
            .checked_add(file_len)
            .is_none_or(|end| end > bytes.len())
        {
            return Err(format!(
                "{path_name}: PT_LOAD {index} file range is out of range"
            ));
        }
        if header.align % HOST_PAGE != 0 {
            if header.align % 4096 != 0 {
                return Err(format!(
                    "{path_name}: PT_LOAD {index} has p_align 0x{:x}, not a multiple of 4 KiB",
                    header.align
                ));
            }
            subpage = true;
        }
    }
    if loads == 0 {
        return Err(format!("{path_name}: no PT_LOAD segments"));
    }

    let low_page = align_down(low, HOST_PAGE);
    let high_page =
        align_up(high, HOST_PAGE).ok_or_else(|| format!("{path_name}: PT_LOAD span overflows"))?;
    let span = usize::try_from(high_page - low_page)
        .map_err(|_| format!("{path_name}: PT_LOAD span is too large"))?;
    let base = unsafe {
        mmap(
            ptr::null_mut(),
            span,
            PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANON,
            -1,
            0,
        )
    };
    if base as isize == -1 {
        return Err(format!(
            "reserve 0x{span:x} bytes at 0x{low_page:x}: {}",
            std::io::Error::last_os_error()
        ));
    }
    let mut arena = Mapping {
        base: base.cast(),
        len: span,
        keep: false,
    };
    let base_addr = base as u64;
    let bias = base_addr
        .checked_sub(low_page)
        .ok_or_else(|| format!("reserve returned an address below the ELF image"))?;

    for header in headers.iter().filter(|header| header.kind == PT_LOAD) {
        let address = header
            .vaddr
            .checked_add(bias)
            .ok_or_else(|| format!("{path_name}: PT_LOAD address overflows"))?;
        if header.filesz != 0 {
            let start = header.offset as usize;
            let len = header.filesz as usize;
            unsafe {
                ptr::copy_nonoverlapping(bytes.as_ptr().add(start), address as *mut u8, len);
            }
        }
    }

    for (index, header) in headers
        .iter()
        .enumerate()
        .filter(|(_, header)| header.kind == PT_LOAD)
    {
        let address = header
            .vaddr
            .checked_add(bias)
            .ok_or_else(|| format!("{path_name}: PT_LOAD address overflows"))?;
        let mem_end = address
            .checked_add(header.memsz)
            .ok_or_else(|| format!("{path_name}: PT_LOAD end overflows"))?;
        let mut protection = prot_of(header.flags);
        if subpage {
            let start4 = align_down(address, 4096);
            let end4 = align_up(mem_end, 4096)
                .ok_or_else(|| format!("{path_name}: PT_LOAD alignment overflows"))?;
            if protection & PROT_EXEC != 0 {
                if out.nexec < out.exec.len() as c_int {
                    out.exec[out.nexec as usize] = LxrtRange {
                        start: address,
                        end: address + header.filesz,
                    };
                    out.nexec += 1;
                }
                protection = (protection & !PROT_EXEC) | PROT_WRITE;
            }
            let result = unsafe { lxrt_subpage_mprotect(start4, end4 - start4, protection) };
            if result != 0 {
                return Err(format!(
                    "subpage mprotect 0x{start4:x}+0x{:x}: {result}",
                    end4 - start4
                ));
            }
            continue;
        }

        let start = align_down(address, HOST_PAGE);
        let end = align_up(mem_end, HOST_PAGE)
            .ok_or_else(|| format!("{path_name}: PT_LOAD alignment overflows"))?;
        if protection & PROT_EXEC != 0 {
            protection = (protection & !PROT_EXEC) | PROT_WRITE;
            if out.nexec < out.exec.len() as c_int {
                out.exec[out.nexec as usize] = LxrtRange {
                    start: address,
                    end: address + header.filesz,
                };
                out.nexec += 1;
            }
        }
        if unsafe { mprotect(start as *mut c_void, (end - start) as usize, protection) } != 0 {
            return Err(format!(
                "mprotect 0x{start:x}+0x{:x} to {protection}: {}",
                end - start,
                std::io::Error::last_os_error()
            ));
        }
        let _ = index;
    }

    out.base = arena.base;
    out.load_bias = bias;
    out.entry = entry
        .checked_add(bias)
        .ok_or_else(|| format!("{path_name}: entry address overflows"))?;
    out.phentsize = phentsize;
    out.phnum = phnum;
    out.span = span;
    out.brk = high_page
        .checked_add(bias)
        .ok_or_else(|| format!("{path_name}: brk address overflows"))?;
    out.is_pie = true;
    out.subpage = subpage;

    for header in headers.iter().filter(|header| header.kind == PT_LOAD) {
        let phend = phoff as u64 + phbytes as u64;
        let file_end = header.offset.saturating_add(header.filesz);
        if phoff as u64 >= header.offset && phend <= file_end {
            out.phdr = header.vaddr + bias + (phoff as u64 - header.offset);
            break;
        }
    }

    drop(file_map);
    out.ncode = unsafe {
        lxrt_elf_exec_sections_path(
            path.as_ptr(),
            bias,
            out.code.as_mut_ptr(),
            out.code.len() as c_int,
        )
    };
    arena.keep = true;
    Ok(())
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_load_elf(
    path: *const c_char,
    out: *mut LxrtImage,
    err: *mut *mut c_char,
) -> c_int {
    if out.is_null() || path.is_null() {
        return write_error(err, "invalid ELF loader argument".to_owned());
    }
    if !err.is_null() {
        *err = ptr::null_mut();
    }
    let image = &mut *out;
    image.clear();
    match load_elf(CStr::from_ptr(path), image) {
        Ok(()) => 0,
        Err(message) => {
            image.clear();
            write_error(err, message)
        }
    }
}
