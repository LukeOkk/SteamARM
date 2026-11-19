//! ELF executable section discovery and function-range indexing.
//!
//! Keep these exports ABI-compatible with lxrt.h: the C dispatcher and PE
//! loader pass the ranges across this boundary while the implementation lives
//! in the Rust runtime-support archive.

use std::ffi::{c_char, c_int, c_void, CStr};
use std::mem::MaybeUninit;
use std::ptr;
use std::sync::{Mutex, MutexGuard, Once};

mod generated {
    #[allow(dead_code)]
    pub mod pathfd {
        include!(concat!(env!("OUT_DIR"), "/pathfd_constants.rs"));
    }
    pub mod timer {
        include!(concat!(env!("OUT_DIR"), "/timer_constants.rs"));
    }
    pub mod tls {
        include!(concat!(env!("OUT_DIR"), "/tls_constants.rs"));
    }
}

const HOST_PAGE: u64 = 16_384;
const GUEST_PAGE: u64 = 4_096;
const EM_AARCH64: u16 = 183;
const SHF_EXECINSTR: u64 = 0x4;
const SHT_PROGBITS: u32 = 1;
const SHT_DYNSYM: u32 = 11;
const PT_LOAD: u32 = 1;
const PF_X: u32 = 1;
const PATH_MAX: usize = generated::pathfd::D_PATH_MAX as usize;

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct LxrtRange {
    pub start: u64,
    pub end: u64,
}

#[derive(Clone, Copy)]
struct ElfHeader {
    machine: u16,
    phoff: u64,
    shoff: u64,
    phentsize: u16,
    phnum: u16,
    shentsize: u16,
    shnum: u16,
    shstrndx: u16,
}

#[derive(Clone, Copy, Default)]
struct ProgramHeader {
    kind: u32,
    flags: u32,
    offset: u64,
    vaddr: u64,
    memsz: u64,
}

#[derive(Clone, Copy, Default)]
struct SectionHeader {
    name: u32,
    kind: u32,
    flags: u64,
    addr: u64,
    offset: u64,
    size: u64,
    link: u32,
    entsize: u64,
}

struct FunctionSet {
    lo: u64,
    hi: u64,
    ranges: Vec<LxrtRange>,
}

static FUNCTIONS: Mutex<Vec<FunctionSet>> = Mutex::new(Vec::new());
static FORK_HOOKS: Once = Once::new();
type FunctionGuard = MutexGuard<'static, Vec<FunctionSet>>;
static mut FORK_GUARD: MaybeUninit<FunctionGuard> = MaybeUninit::uninit();

#[repr(transparent)]
struct DarwinSigSet(u32);

#[link(name = "System")]
unsafe extern "C" {
    fn pread(fd: c_int, buffer: *mut c_void, count: usize, offset: i64) -> isize;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn free(pointer: *mut c_void);
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn sigfillset(set: *mut c_void) -> c_int;
    fn sigdelset(set: *mut c_void, signal: c_int) -> c_int;
    fn pthread_sigmask(how: c_int, set: *const c_void, old: *mut c_void) -> c_int;

    #[cfg(feature = "pe")]
    fn lxrt_pe_is(fd: c_int) -> bool;
    #[cfg(feature = "pe")]
    fn lxrt_pe_exec_sections(
        fd: c_int,
        file_off: u64,
        len: u64,
        map_base: u64,
        out: *mut LxrtRange,
        max: c_int,
    ) -> c_int;

    fn lxrt_tlskeep_enabled() -> bool;
    fn lxrt_tlskeep_add(start: u64, end: u64, why: *const c_char);
}

fn read_u16(bytes: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        bytes.get(offset..offset.checked_add(2)?)?.try_into().ok()?,
    ))
}

fn read_u32(bytes: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        bytes.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
    ))
}

fn read_u64(bytes: &[u8], offset: usize) -> Option<u64> {
    Some(u64::from_le_bytes(
        bytes.get(offset..offset.checked_add(8)?)?.try_into().ok()?,
    ))
}

fn read_all(fd: c_int, offset: u64, buffer: &mut [u8]) -> bool {
    let mut done = 0usize;
    while done < buffer.len() {
        let Some(at) = offset
            .checked_add(done as u64)
            .and_then(|n| i64::try_from(n).ok())
        else {
            return false;
        };
        let result = unsafe {
            pread(
                fd,
                buffer[done..].as_mut_ptr().cast(),
                buffer.len() - done,
                at,
            )
        };
        if result < 0 {
            if std::io::Error::last_os_error().raw_os_error() == Some(4) {
                continue;
            }
            return false;
        }
        if result == 0 {
            return false;
        }
        done += result as usize;
    }
    true
}

fn read_elf_header(fd: c_int) -> Option<ElfHeader> {
    let mut bytes = [0u8; 64];
    if !read_all(fd, 0, &mut bytes)
        || bytes.get(0..4)? != b"\x7fELF"
        || bytes[4] != 2
        || bytes[5] != 1
    {
        return None;
    }
    Some(ElfHeader {
        machine: read_u16(&bytes, 18)?,
        phoff: read_u64(&bytes, 32)?,
        shoff: read_u64(&bytes, 40)?,
        phentsize: read_u16(&bytes, 54)?,
        phnum: read_u16(&bytes, 56)?,
        shentsize: read_u16(&bytes, 58)?,
        shnum: read_u16(&bytes, 60)?,
        shstrndx: read_u16(&bytes, 62)?,
    })
}

fn read_program_headers(fd: c_int, header: ElfHeader) -> Option<Vec<ProgramHeader>> {
    if header.phentsize != 56 || header.phnum == 0 || header.phnum > 128 {
        return None;
    }
    let size = usize::from(header.phnum).checked_mul(56)?;
    let mut bytes = vec![0; size];
    if !read_all(fd, header.phoff, &mut bytes) {
        return None;
    }
    let mut result = Vec::with_capacity(usize::from(header.phnum));
    for entry in bytes.chunks_exact(56) {
        result.push(ProgramHeader {
            kind: read_u32(entry, 0)?,
            flags: read_u32(entry, 4)?,
            offset: read_u64(entry, 8)?,
            vaddr: read_u64(entry, 16)?,
            memsz: read_u64(entry, 40)?,
        });
    }
    Some(result)
}

fn read_section_headers(fd: c_int, header: ElfHeader) -> Option<Vec<SectionHeader>> {
    if header.shoff == 0 || header.shnum == 0 || header.shnum > 4096 || header.shentsize != 64 {
        return None;
    }
    let size = usize::from(header.shnum).checked_mul(64)?;
    let mut bytes = vec![0; size];
    if !read_all(fd, header.shoff, &mut bytes) {
        return None;
    }
    let mut result = Vec::with_capacity(usize::from(header.shnum));
    for entry in bytes.chunks_exact(64) {
        result.push(SectionHeader {
            name: read_u32(entry, 0)?,
            kind: read_u32(entry, 4)?,
            flags: read_u64(entry, 8)?,
            addr: read_u64(entry, 16)?,
            offset: read_u64(entry, 24)?,
            size: read_u64(entry, 32)?,
            link: read_u32(entry, 40)?,
            entsize: read_u64(entry, 56)?,
        });
    }
    Some(result)
}

fn block_async_signals() -> DarwinSigSet {
    let mut all = DarwinSigSet(0);
    let mut old = DarwinSigSet(0);
    unsafe {
        let _ = sigfillset((&mut all as *mut DarwinSigSet).cast());
        let _ = sigdelset(
            (&mut all as *mut DarwinSigSet).cast(),
            generated::tls::D_SIGBUS,
        );
        let _ = sigdelset(
            (&mut all as *mut DarwinSigSet).cast(),
            generated::tls::D_SIGSEGV,
        );
        let _ = sigdelset(
            (&mut all as *mut DarwinSigSet).cast(),
            generated::tls::D_SIGILL,
        );
        let _ = sigdelset(
            (&mut all as *mut DarwinSigSet).cast(),
            generated::tls::D_SIGTRAP,
        );
        let _ = pthread_sigmask(
            generated::timer::D_SIG_BLOCK,
            (&all as *const DarwinSigSet).cast(),
            (&mut old as *mut DarwinSigSet).cast(),
        );
    }
    old
}

fn lock_functions() -> FunctionGuard {
    FUNCTIONS
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

fn lock_nosig() -> (FunctionGuard, DarwinSigSet) {
    let old = block_async_signals();
    (lock_functions(), old)
}

fn unlock_nosig(guard: FunctionGuard, old: DarwinSigSet) {
    drop(guard);
    unsafe {
        let _ = pthread_sigmask(
            generated::timer::D_SIG_SETMASK,
            (&old as *const DarwinSigSet).cast(),
            ptr::null_mut(),
        );
    }
}

extern "C" fn fork_prepare() {
    let guard = lock_functions();
    unsafe {
        ptr::addr_of_mut!(FORK_GUARD)
            .cast::<FunctionGuard>()
            .write(guard);
    }
}

extern "C" fn fork_release() {
    unsafe {
        ptr::drop_in_place(ptr::addr_of_mut!(FORK_GUARD).cast::<FunctionGuard>());
    }
}

extern "C" fn register_fork_hooks() {
    FORK_HOOKS.call_once(|| unsafe {
        let _ = pthread_atfork(Some(fork_prepare), Some(fork_release), Some(fork_release));
    });
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static FORK_HOOKS_INIT: extern "C" fn() = register_fork_hooks;

fn register_ranges(lo: u64, hi: u64, mut ranges: Vec<LxrtRange>) {
    if hi <= lo {
        return;
    }
    ranges.sort_unstable_by_key(|range| range.start);
    let mut merged: Vec<LxrtRange> = Vec::with_capacity(ranges.len());
    for range in ranges {
        if let Some(last) = merged.last_mut() {
            if range.start <= last.end {
                last.end = last.end.max(range.end);
                continue;
            }
        }
        merged.push(range);
    }

    let (mut functions, old) = lock_nosig();
    functions.retain(|entry| {
        if entry.lo < hi && entry.hi > lo {
            false
        } else {
            true
        }
    });
    functions.push(FunctionSet {
        lo,
        hi,
        ranges: merged,
    });
    unlock_nosig(functions, old);
}

#[no_mangle]
pub extern "C" fn lxrt_fn_register(lo: u64, hi: u64, ranges: *mut LxrtRange, count: c_int) {
    if ranges.is_null() || count <= 0 {
        unsafe { free(ranges.cast()) };
        if hi > lo && count == 0 {
            register_ranges(lo, hi, Vec::new());
        }
        return;
    }
    let copied = unsafe { std::slice::from_raw_parts(ranges, count as usize).to_vec() };
    unsafe { free(ranges.cast()) };
    register_ranges(lo, hi, copied);
}

#[no_mangle]
pub extern "C" fn lxrt_elf_forget(address: u64, length: u64) {
    if length == 0 {
        return;
    }
    let end = address.wrapping_add(length);
    let (mut functions, old) = lock_nosig();
    functions.retain(|entry| !(entry.lo >= address && entry.hi <= end));
    unlock_nosig(functions, old);
}

#[no_mangle]
pub extern "C" fn lxrt_elf_in_function(address: u64) -> bool {
    let (functions, old) = lock_nosig();
    let mut result = true;
    if let Some(entry) = functions
        .iter()
        .find(|entry| address >= entry.lo && address < entry.hi)
    {
        let mut low = 0usize;
        let mut high = entry.ranges.len();
        result = false;
        while low < high {
            let middle = (low + high) / 2;
            let range = entry.ranges[middle];
            if address < range.start {
                high = middle;
            } else if address >= range.end {
                low = middle + 1;
            } else {
                result = true;
                break;
            }
        }
    }
    unlock_nosig(functions, old);
    result
}

fn read_uleb(bytes: &[u8], cursor: &mut usize, end: usize) -> Option<u64> {
    let mut value = 0u64;
    let mut shift = 0u32;
    while *cursor < end {
        let byte = *bytes.get(*cursor)?;
        *cursor += 1;
        if shift >= 64 && byte & 0x7f != 0 {
            return None;
        }
        value |= u64::from(byte & 0x7f).checked_shl(shift).unwrap_or(0);
        if byte & 0x80 == 0 {
            return Some(value);
        }
        shift = shift.checked_add(7)?;
    }
    None
}

fn read_eh_pointer(
    bytes: &[u8],
    cursor: &mut usize,
    end: usize,
    encoding: u8,
    field_address: u64,
) -> Option<u64> {
    let value = match encoding & 0x0f {
        0x00 => {
            let result = read_u64(bytes.get(..end)?, *cursor)?;
            *cursor = cursor.checked_add(8)?;
            result
        }
        0x03 => {
            let result = u64::from(read_u32(bytes.get(..end)?, *cursor)?);
            *cursor = cursor.checked_add(4)?;
            result
        }
        0x0b => {
            let result = read_u32(bytes.get(..end)?, *cursor)? as i32 as i64 as u64;
            *cursor = cursor.checked_add(4)?;
            result
        }
        0x04 | 0x0c => {
            let result = read_u64(bytes.get(..end)?, *cursor)?;
            *cursor = cursor.checked_add(8)?;
            result
        }
        _ => return None,
    };
    Some(if encoding & 0x70 == 0x10 {
        field_address.wrapping_add(value)
    } else {
        value
    })
}

fn parse_eh_frame(bytes: &[u8], section_address: u64) -> Vec<LxrtRange> {
    #[derive(Clone, Copy)]
    struct Cie {
        offset: usize,
        encoding: u8,
    }

    let mut ranges = Vec::new();
    let mut cies = Vec::with_capacity(64);
    let mut cursor = 0usize;
    while bytes.len().saturating_sub(cursor) >= 4 {
        let record_start = cursor;
        let Some(length) = read_u32(bytes, cursor) else {
            break;
        };
        cursor += 4;
        if length == 0 || length == u32::MAX {
            break;
        }
        let Some(record_end) = cursor.checked_add(length as usize) else {
            break;
        };
        if record_end > bytes.len() || record_end.saturating_sub(cursor) < 4 {
            break;
        }
        let id_offset = cursor;
        let Some(id) = read_u32(bytes, cursor) else {
            break;
        };
        cursor += 4;
        if id == 0 {
            let Some(&version) = bytes.get(cursor) else {
                break;
            };
            cursor += 1;
            let aug_start = cursor;
            let Some(aug_end) = bytes.get(cursor..record_end).and_then(|tail| {
                tail.iter()
                    .position(|byte| *byte == 0)
                    .map(|relative| cursor + relative)
            }) else {
                break;
            };
            let augmentation = &bytes[aug_start..aug_end];
            cursor = aug_end + 1;
            if read_uleb(bytes, &mut cursor, record_end).is_none()
                || read_uleb(bytes, &mut cursor, record_end).is_none()
            {
                break;
            }
            if version == 1 {
                cursor = match cursor.checked_add(1) {
                    Some(next) if next <= record_end => next,
                    _ => break,
                };
            } else if read_uleb(bytes, &mut cursor, record_end).is_none() {
                break;
            }
            let mut encoding = 0;
            if augmentation.first() == Some(&b'z') {
                let Some(augmentation_len) = read_uleb(bytes, &mut cursor, record_end) else {
                    break;
                };
                let Some(augmentation_end) = cursor.checked_add(augmentation_len as usize) else {
                    break;
                };
                if augmentation_end > record_end {
                    break;
                }
                for kind in augmentation.iter().skip(1).copied() {
                    if cursor >= augmentation_end {
                        break;
                    }
                    match kind {
                        b'R' => {
                            encoding = bytes[cursor];
                            cursor += 1;
                        }
                        b'L' => cursor += 1,
                        b'P' => {
                            let pointer_encoding = bytes[cursor];
                            cursor += 1;
                            if read_eh_pointer(
                                bytes,
                                &mut cursor,
                                augmentation_end,
                                pointer_encoding & 0x7f,
                                0,
                            )
                            .is_none()
                            {
                                break;
                            }
                        }
                        b'S' | b'B' => {}
                        _ => break,
                    }
                }
            }
            if cies.len() < 64 {
                cies.push(Cie {
                    offset: record_start,
                    encoding,
                });
            }
        } else {
            let Some(cie_offset) = id_offset.checked_sub(id as usize) else {
                cursor = record_end;
                continue;
            };
            let encoding = cies
                .iter()
                .find(|cie| cie.offset == cie_offset)
                .map(|cie| cie.encoding)
                .unwrap_or(0x1b);
            let mut fields = cursor;
            let field_address = section_address.wrapping_add(fields as u64);
            if let Some(begin) =
                read_eh_pointer(bytes, &mut fields, record_end, encoding, field_address)
            {
                if let Some(length) =
                    read_eh_pointer(bytes, &mut fields, record_end, encoding & 0x0f, 0)
                {
                    if length != 0 {
                        ranges.push(LxrtRange {
                            start: begin,
                            end: begin.wrapping_add(length),
                        });
                    }
                }
            }
        }
        cursor = record_end;
    }
    ranges
}

fn is_elf_aarch64(header: ElfHeader) -> bool {
    header.machine == EM_AARCH64
}

fn section_string<'a>(strings: &'a [u8], offset: usize) -> Option<&'a [u8]> {
    let tail = strings.get(offset..)?;
    let end = tail.iter().position(|byte| *byte == 0)?;
    Some(&tail[..end])
}

fn register_functions(
    fd: c_int,
    header: ElfHeader,
    sections: &[SectionHeader],
    window_lo: u64,
    window_hi: u64,
    delta: i64,
) {
    if let Some(filter) = std::env::var_os("LXRT_X18_ALL_TEXT") {
        use std::os::unix::ffi::OsStrExt;
        let pattern = filter.as_os_str().as_bytes();
        if !pattern.is_empty() {
            let mut path = [0i8; PATH_MAX];
            if unsafe { fcntl(fd, generated::pathfd::D_F_GETPATH, path.as_mut_ptr()) } == 0 {
                let bytes = unsafe { CStr::from_ptr(path.as_ptr()) }.to_bytes();
                if bytes.windows(pattern.len()).any(|part| part == pattern) {
                    return;
                }
            }
        }
    }
    let Some(string_section) = sections.get(usize::from(header.shstrndx)) else {
        return;
    };
    if string_section.size == 0 || string_section.size > (1 << 20) {
        return;
    }
    let Ok(string_size) = usize::try_from(string_section.size) else {
        return;
    };
    let mut names = vec![0; string_size];
    if !read_all(fd, string_section.offset, &mut names) {
        return;
    }
    for section in sections {
        if section_string(&names, section.name as usize) != Some(b".eh_frame") {
            continue;
        }
        if section.size == 0 || section.size > (64 << 20) {
            break;
        }
        let Ok(size) = usize::try_from(section.size) else {
            break;
        };
        let mut bytes = vec![0; size];
        if read_all(fd, section.offset, &mut bytes) {
            let mut ranges = parse_eh_frame(&bytes, section.addr);
            if !ranges.is_empty() {
                ranges.retain_mut(|range| {
                    range.start = range.start.wrapping_add(delta as u64);
                    range.end = range.end.wrapping_add(delta as u64);
                    range.start < window_hi && range.end > window_lo
                });
                register_ranges(window_lo, window_hi, ranges);
            }
        }
        break;
    }
}

fn register_fips(
    fd: c_int,
    sections: &[SectionHeader],
    section_lo: u64,
    section_hi: u64,
    window_lo: u64,
    window_hi: u64,
    delta: i64,
) {
    if !unsafe { lxrt_tlskeep_enabled() } {
        return;
    }
    for symbols in sections {
        if symbols.kind != SHT_DYNSYM
            || symbols.entsize != 24
            || symbols.link as usize >= sections.len()
            || symbols.size == 0
            || symbols.size > (64 << 20)
        {
            continue;
        }
        let strings = &sections[symbols.link as usize];
        if strings.size == 0 || strings.size > (64 << 20) {
            return;
        }
        let (Ok(string_size), Some(capacity)) = (
            usize::try_from(strings.size),
            usize::try_from(strings.size)
                .ok()
                .and_then(|size| size.checked_add(1)),
        ) else {
            return;
        };
        let Ok(symbol_size) = usize::try_from(symbols.size) else {
            return;
        };
        let mut names = vec![0; capacity];
        if !read_all(fd, strings.offset, &mut names[..string_size])
            || !names[..string_size]
                .windows(b"BORINGSSL_bcm_text_".len())
                .any(|part| part == b"BORINGSSL_bcm_text_")
        {
            return;
        }
        let mut table = vec![0; symbol_size];
        let mut start = 0u64;
        let mut end = 0u64;
        if read_all(fd, symbols.offset, &mut table) {
            for symbol in table.chunks_exact(24) {
                let Some(name_offset) = read_u32(symbol, 0).map(|n| n as usize) else {
                    continue;
                };
                let Some(value) = read_u64(symbol, 8) else {
                    continue;
                };
                if name_offset >= string_size || value == 0 {
                    continue;
                }
                match section_string(&names[..string_size], name_offset) {
                    Some(b"BORINGSSL_bcm_text_start") => start = value,
                    Some(b"BORINGSSL_bcm_text_end") => end = value,
                    _ => {}
                }
            }
        }
        if start != 0 && end > start && start >= section_lo && end <= section_hi {
            let mapped_start = start.wrapping_add(delta as u64);
            let mapped_end = end.wrapping_add(delta as u64);
            if mapped_start >= window_lo && mapped_end <= window_hi {
                unsafe {
                    lxrt_tlskeep_add(
                        mapped_start,
                        mapped_end,
                        b"BoringSSL FIPS module\0".as_ptr().cast(),
                    )
                };
            }
        }
        return;
    }
}

#[no_mangle]
pub extern "C" fn lxrt_elf_gap_before_exec(
    fd: c_int,
    file_offset: u64,
    map_base: u64,
    gap: *mut LxrtRange,
) -> bool {
    if fd < 0 || gap.is_null() {
        return false;
    }
    let Some(header) = read_elf_header(fd) else {
        return false;
    };
    if !is_elf_aarch64(header) {
        return false;
    }
    let Some(programs) = read_program_headers(fd, header) else {
        return false;
    };
    for executable in programs
        .iter()
        .filter(|ph| ph.kind == PT_LOAD && ph.flags & PF_X != 0)
    {
        if executable.offset & !(GUEST_PAGE - 1) != file_offset {
            continue;
        }
        let exec_va = executable.vaddr & !(GUEST_PAGE - 1);
        if map_base < exec_va {
            return false;
        }
        let bias = map_base - exec_va;
        let mut previous_end = 0u64;
        for ph in &programs {
            let Some(end) = ph.vaddr.checked_add(ph.memsz) else {
                continue;
            };
            if ph.kind == PT_LOAD && end <= exec_va && end > previous_end {
                previous_end = end;
            }
        }
        if previous_end == 0
            || bias.checked_add(exec_va).is_none()
            || bias.checked_add(previous_end).is_none()
        {
            return false;
        }
        let Some(lo_unaligned) = bias.checked_add(previous_end) else {
            return false;
        };
        let Some(hi_unaligned) = bias.checked_add(exec_va) else {
            return false;
        };
        let Some(lo) = lo_unaligned
            .checked_add(HOST_PAGE - 1)
            .map(|value| value & !(HOST_PAGE - 1))
        else {
            return false;
        };
        let hi = hi_unaligned & !(HOST_PAGE - 1);
        if hi <= lo {
            return false;
        }
        for ph in &programs {
            if ph.kind != PT_LOAD {
                continue;
            }
            let Some(start) = bias.checked_add(ph.vaddr) else {
                return false;
            };
            let Some(end) = start.checked_add(ph.memsz) else {
                return false;
            };
            if start < hi && end > lo {
                return false;
            }
        }
        unsafe { gap.write(LxrtRange { start: lo, end: hi }) };
        return true;
    }
    false
}

#[no_mangle]
pub extern "C" fn lxrt_elf_exec_sections(
    fd: c_int,
    file_offset: u64,
    length: u64,
    map_base: u64,
    out: *mut LxrtRange,
    max: c_int,
) -> c_int {
    if max <= 0 || out.is_null() {
        return 0;
    }
    #[cfg(feature = "pe")]
    if unsafe { lxrt_pe_is(fd) } {
        return unsafe { lxrt_pe_exec_sections(fd, file_offset, length, map_base, out, max) };
    }
    if fd < 0 {
        return 0;
    }
    let Some(header) = read_elf_header(fd) else {
        return 0;
    };
    if !is_elf_aarch64(header) {
        return 0;
    }
    let Some(sections) = read_section_headers(fd, header) else {
        return 0;
    };
    let file_end = file_offset.wrapping_add(length);
    let mut count = 0usize;
    for section in &sections {
        if section.flags & SHF_EXECINSTR == 0
            || section.kind != SHT_PROGBITS
            || section.size == 0
            || count >= max as usize
        {
            continue;
        }
        let Some(section_end) = section.offset.checked_add(section.size) else {
            continue;
        };
        let lo = section.offset.max(file_offset);
        let hi = section_end.min(file_end);
        if lo >= hi {
            continue;
        }
        unsafe {
            out.add(count).write(LxrtRange {
                start: map_base.wrapping_add(lo - file_offset),
                end: map_base.wrapping_add(hi - file_offset),
            })
        };
        count += 1;
    }
    for window in 0..count {
        let range = unsafe { *out.add(window) };
        for section in &sections {
            if section.flags & SHF_EXECINSTR == 0 || section.kind != SHT_PROGBITS {
                continue;
            }
            let section_start = map_base
                .wrapping_add(section.offset)
                .wrapping_sub(file_offset);
            let clipped_start = map_base
                .wrapping_add(section.offset.max(file_offset))
                .wrapping_sub(file_offset);
            if range.start != clipped_start {
                continue;
            }
            let delta = (section_start as i64).wrapping_sub(section.addr as i64);
            register_functions(fd, header, &sections, range.start, range.end, delta);
            register_fips(
                fd,
                &sections,
                section.addr,
                section.addr.wrapping_add(section.size),
                range.start,
                range.end,
                delta,
            );
            break;
        }
    }
    count as c_int
}

#[no_mangle]
pub extern "C" fn lxrt_elf_exec_sections_path(
    path: *const c_char,
    load_bias: u64,
    out: *mut LxrtRange,
    max: c_int,
) -> c_int {
    if path.is_null() || out.is_null() || max <= 0 {
        return 0;
    }
    let fd = unsafe { open(path, generated::pathfd::D_O_RDONLY) };
    if fd < 0 {
        return 0;
    }
    let mut count = 0usize;
    if let Some(header) = read_elf_header(fd) {
        if is_elf_aarch64(header) {
            if let Some(sections) = read_section_headers(fd, header) {
                for section in &sections {
                    if section.flags & SHF_EXECINSTR == 0
                        || section.kind != SHT_PROGBITS
                        || section.size == 0
                        || section.addr == 0
                        || count >= max as usize
                    {
                        continue;
                    }
                    let start = load_bias.wrapping_add(section.addr);
                    let end = load_bias
                        .wrapping_add(section.addr)
                        .wrapping_add(section.size);
                    unsafe {
                        out.add(count).write(LxrtRange { start, end });
                    }
                    register_functions(fd, header, &sections, start, end, load_bias as i64);
                    register_fips(
                        fd,
                        &sections,
                        section.addr,
                        section.addr.wrapping_add(section.size),
                        start,
                        end,
                        load_bias as i64,
                    );
                    count += 1;
                }
            }
        }
    }
    unsafe {
        let _ = close(fd);
    }
    count as c_int
}
