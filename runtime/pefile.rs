// The aarch64 code of a PE image mapped by the guest's own loader.
//
// Wine ARM64 maps its DLLs itself (ARM64X: ntdll, kernel32, DXVK; ARM64EC:
// libarm64ecfex) section by section from the file, and that code reads x18
// as the TEB pointer -- which Darwin clears. The runtime's x18 pass
// (rewrite.c x18_site) runs only where it knows instructions are: inside
// the code windows a mapping is told about (lxrt_elf_exec_sections) and
// inside a registered function table (elfsect.rs lxrt_elf_in_function). For
// an ELF both come from the file; a PE mapping got neither, its x18 sites
// stayed live, and Wine's ARM64X ntdll died on [x18 + 0x60] with x18 = 0
// (MEASURED 2026-10-06, benchmarks/stage57-wine-arm64-native.txt).
//
// Here a PE file behind a mapping is read the same way: its code ranges are
// the ARM64 / ARM64EC entries of the ARM64X CHPE code map (load config
// CHPEMetadataPointer -> IMAGE_ARM64EC_METADATA CodeMap; the x64 entries --
// type 2, the x64 side of an ARM64X DLL and the x64 thunks -- are NOT
// aarch64 and are never decoded as such), or every executable section of a
// plain ARM64 image; a pure x64 image has no aarch64 code at all. The
// windows of a mapping are those ranges translated through the section
// table to the file offsets the mapping covers. For now a window counts as
// one function for the x18 pass (the .pdata bounds would exclude data
// between functions; PE text keeps its constants in .rdata, so the risk is
// small, and the pass refuses what it cannot plan). Windows are remembered
// by address for the scans that have no file at hand (an mprotect to
// executable, a W^X flip: rewrite.c asks lxrt_pe_code_windows).
//
// Port note: the state is the C file's, kept as it was -- plain statics under
// one pthread mutex (no fork hooks: the C had none), malloc/realloc/free for
// what the C allocated, and the pe_file entries handed out by pointer after
// the lock is dropped, exactly as before.

use std::cell::Cell;
use std::ffi::{c_char, c_int, c_long, c_uint, c_ulonglong, c_void};
use std::mem::{size_of, MaybeUninit};
use std::ptr;

// Darwin constants this file needs (the C took them from the SDK headers).
const D_KERN_SUCCESS: c_int = 0;
const D_CLOCK_UPTIME_RAW: c_int = 8;
const D_S_IFMT: u16 = 0o170000;
const D_S_IFREG: u16 = 0o100000;
const D_PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;

/// `struct lxrt_range` (lxrt.h).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LxrtRange {
    start: u64,
    end: u64,
}

const _: [(); 16] = [(); size_of::<LxrtRange>()];

#[repr(C)]
#[derive(Clone, Copy)]
struct PeSect {
    va: u32,
    vsize: u32,
    raw: u32,
    rawsz: u32,
    ch: u32,
}

#[repr(C)]
struct PeFile {
    dev: i32, // dev_t
    ino: u64, // ino_t
    is_pe: bool,
    has_code: bool,
    machine: u16,
    chpe: bool,
    sect: *mut PeSect,
    nsect: c_int,
    code: *mut LxrtRange, // RVA ranges of aarch64 code, sorted
    ncode: c_int,
}

impl PeFile {
    const EMPTY: Self = Self {
        dev: 0,
        ino: 0,
        is_pe: false,
        has_code: false,
        machine: 0,
        chpe: false,
        sect: ptr::null_mut(),
        nsect: 0,
        code: ptr::null_mut(),
        ncode: 0,
    };
}

/// Darwin arm64 `pthread_mutex_t`.
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}

const _: [(); 64] = [(); size_of::<PthreadMutex>()];

/// Darwin arm64 `struct stat` (64-bit inodes).
#[repr(C)]
struct Stat {
    st_dev: i32,
    st_mode: u16,
    st_nlink: u16,
    st_ino: u64,
    st_uid: u32,
    st_gid: u32,
    st_rdev: i32,
    st_atimespec: [c_long; 2],
    st_mtimespec: [c_long; 2],
    st_ctimespec: [c_long; 2],
    st_birthtimespec: [c_long; 2],
    st_size: i64,
    st_blocks: i64,
    st_blksize: i32,
    st_flags: u32,
    st_gen: u32,
    st_lspare: i32,
    st_qspare: [i64; 2],
}

const _: [(); 144] = [(); size_of::<Stat>()];

const PE_CACHE: usize = 64;
static mut G_FILES: [PeFile; PE_CACHE] = [PeFile::EMPTY; PE_CACHE];
static mut G_NFILES: c_int = 0;
static mut G_LOCK: PthreadMutex = PthreadMutex {
    sig: D_PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
};

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: u32;

    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn pread(fd: c_int, buffer: *mut c_void, count: usize, offset: i64) -> isize;
    fn fstat(fd: c_int, status: *mut Stat) -> c_int;
    fn malloc(size: usize) -> *mut c_void;
    fn calloc(count: usize, size: usize) -> *mut c_void;
    fn realloc(pointer: *mut c_void, size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn qsort(
        base: *mut c_void,
        count: usize,
        width: usize,
        compare: Option<unsafe extern "C" fn(*const c_void, *const c_void) -> c_int>,
    );
    fn clock_gettime_nsec_np(clock: c_int) -> u64;
    fn mach_vm_read_overwrite(
        task: u32,
        address: u64,
        size: u64,
        data: u64,
        out_size: *mut u64,
    ) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;

    fn lxrt_trace_on() -> bool;
    fn lxrt_trace_stream() -> *mut c_void;
    fn lxrt_jit_contains(address: u64) -> bool;
    fn lxrt_fn_register(lo: u64, hi: u64, ranges: *mut LxrtRange, count: c_int);
}

#[inline(always)]
unsafe fn lock() {
    let _ = pthread_mutex_lock(ptr::addr_of_mut!(G_LOCK));
}

#[inline(always)]
unsafe fn unlock() {
    let _ = pthread_mutex_unlock(ptr::addr_of_mut!(G_LOCK));
}

unsafe fn pread_all(fd: c_int, buffer: *mut u8, mut count: usize, mut offset: u64) -> bool {
    let mut cursor = buffer;
    while count != 0 {
        let got = pread(fd, cursor.cast(), count, offset as i64);
        if got <= 0 {
            return false;
        }
        cursor = cursor.add(got as usize);
        offset = offset.wrapping_add(got as u64);
        count = count.wrapping_sub(got as usize);
    }
    true
}

#[inline(always)]
unsafe fn rd32(pointer: *const u8) -> u32 {
    pointer.cast::<u32>().read_unaligned()
}

#[inline(always)]
unsafe fn rd16(pointer: *const u8) -> u16 {
    pointer.cast::<u16>().read_unaligned()
}

#[inline(always)]
unsafe fn rd64(pointer: *const u8) -> u64 {
    pointer.cast::<u64>().read_unaligned()
}

// File offset of `need` bytes at `rva`, or -1.
unsafe fn rva_offset(file: *const PeFile, rva: u32, need: u32) -> i64 {
    let mut index: c_int = 0;
    while index < (*file).nsect {
        let section = (*file).sect.add(index as usize);
        let span = if (*section).vsize > (*section).rawsz {
            (*section).vsize
        } else {
            (*section).rawsz
        };
        if rva >= (*section).va && rva < (*section).va.wrapping_add(span) {
            let inside = rva.wrapping_sub((*section).va);
            if inside.wrapping_add(need) > (*section).rawsz {
                return -1;
            }
            return (*section).raw as i64 + inside as i64;
        }
        index += 1;
    }
    -1
}

unsafe extern "C" fn cmp_range(left: *const c_void, right: *const c_void) -> c_int {
    let left = left.cast::<LxrtRange>();
    let right = right.cast::<LxrtRange>();
    if (*left).start < (*right).start {
        -1
    } else {
        ((*left).start > (*right).start) as c_int
    }
}

unsafe fn add_code(file: *mut PeFile, rva: u32, length: u32) {
    if length == 0 {
        return;
    }
    let grown = realloc(
        (*file).code.cast(),
        ((*file).ncode.wrapping_add(1) as usize).wrapping_mul(size_of::<LxrtRange>()),
    )
    .cast::<LxrtRange>();
    if grown.is_null() {
        return;
    }
    (*file).code = grown;
    let slot = grown.add((*file).ncode as usize);
    (*slot).start = rva as u64;
    (*slot).end = rva as u64 + length as u64;
    (*file).ncode += 1;
}

unsafe fn parse(fd: c_int, file: *mut PeFile) {
    let mut dos = [0u8; 0x40];
    if !pread_all(fd, dos.as_mut_ptr(), 0x40, 0) || dos[0] != b'M' || dos[1] != b'Z' {
        return;
    }
    let pe = rd32(dos.as_ptr().add(0x3C));
    let mut hdr = [0u8; 24 + 240];
    if pe > (64u32 << 20) || !pread_all(fd, hdr.as_mut_ptr(), 24 + 240, pe as u64) {
        return;
    }
    if hdr[0] != b'P' || hdr[1] != b'E' || hdr[2] != 0 || hdr[3] != 0 {
        return;
    }
    (*file).is_pe = true;
    (*file).machine = rd16(hdr.as_ptr().add(4));
    let nsect = rd16(hdr.as_ptr().add(6)) as c_uint;
    let optsz = rd16(hdr.as_ptr().add(20)) as c_uint;
    let opt = hdr.as_ptr().add(24);
    if optsz < 112 + 16 * 8 || rd16(opt) != 0x20B {
        return; // PE32+ only
    }
    let image_base = rd64(opt.add(24));
    let ndirs = rd32(opt.add(108));
    // sections
    if nsect == 0 || nsect > 96 {
        return;
    }
    let slen = nsect as usize * 40;
    let sb = malloc(slen).cast::<u8>();
    if sb.is_null() || !pread_all(fd, sb, slen, pe as u64 + 24 + optsz as u64) {
        free(sb.cast());
        return;
    }
    (*file).sect = calloc(nsect as usize, size_of::<PeSect>()).cast::<PeSect>();
    if (*file).sect.is_null() {
        free(sb.cast());
        return;
    }
    (*file).nsect = nsect as c_int;
    let mut index: c_uint = 0;
    while index < nsect {
        let raw = sb.add(index as usize * 40);
        let section = (*file).sect.add(index as usize);
        (*section).vsize = rd32(raw.add(8));
        (*section).va = rd32(raw.add(12));
        (*section).rawsz = rd32(raw.add(16));
        (*section).raw = rd32(raw.add(20));
        (*section).ch = rd32(raw.add(36));
        index += 1;
    }
    free(sb.cast());
    // CHPE code map, when there is one (ARM64X and ARM64EC images)
    if ndirs > 10 {
        let lc_rva = rd32(opt.add(112 + 10 * 8));
        let lc_size = rd32(opt.add(112 + 10 * 8 + 4));
        let lc_off = if lc_rva != 0 && lc_size != 0 {
            rva_offset(file, lc_rva, 4)
        } else {
            -1
        };
        if lc_off >= 0 {
            let mut lc = [0u8; 0xD0];
            let mut have: u32 = 0;
            if pread_all(fd, lc.as_mut_ptr(), 4, lc_off as u64) {
                have = rd32(lc.as_ptr());
            }
            if have >= 0xD0
                && rva_offset(file, lc_rva, 0xD0) >= 0
                && pread_all(fd, lc.as_mut_ptr(), 0xD0, lc_off as u64)
            {
                let chpe_va = rd64(lc.as_ptr().add(0xC8));
                if chpe_va > image_base {
                    let chpe_rva = chpe_va.wrapping_sub(image_base) as u32;
                    let m_off = rva_offset(file, chpe_rva, 12);
                    let mut meta = [0u8; 12];
                    if m_off >= 0 && pread_all(fd, meta.as_mut_ptr(), 12, m_off as u64) {
                        let map_rva = rd32(meta.as_ptr().add(4));
                        let count = rd32(meta.as_ptr().add(8));
                        let map_off = if count != 0 && count < 65536 {
                            rva_offset(file, map_rva, count.wrapping_mul(8))
                        } else {
                            -1
                        };
                        if map_off >= 0 {
                            let entries = malloc(count as usize * 8).cast::<u8>();
                            if !entries.is_null()
                                && pread_all(fd, entries, count as usize * 8, map_off as u64)
                            {
                                (*file).chpe = true;
                                let mut entry: u32 = 0;
                                while entry < count {
                                    let start = rd32(entries.add(entry as usize * 8));
                                    let length = rd32(entries.add(entry as usize * 8 + 4));
                                    if (start & 3) <= 1 {
                                        // 0 ARM64, 1 ARM64EC; 2 is x64
                                        add_code(file, start & !3u32, length);
                                    }
                                    entry += 1;
                                }
                            }
                            free(entries.cast());
                        }
                    }
                }
            }
        }
    }
    if !(*file).chpe {
        if (*file).machine == 0xAA64 {
            let mut index: c_int = 0;
            while index < (*file).nsect {
                let section = (*file).sect.add(index as usize);
                if (*section).ch & 0x2000_0000u32 != 0 {
                    // IMAGE_SCN_MEM_EXECUTE
                    add_code(
                        file,
                        (*section).va,
                        if (*section).rawsz < (*section).vsize {
                            (*section).rawsz
                        } else {
                            (*section).vsize
                        },
                    );
                }
                index += 1;
            }
        }
        // 0x8664 without CHPE: x64 only, nothing for an aarch64 pass
    }
    if (*file).ncode > 1 {
        qsort(
            (*file).code.cast(),
            (*file).ncode as usize,
            size_of::<LxrtRange>(),
            Some(cmp_range),
        );
    }
    (*file).has_code = (*file).ncode > 0;
}

unsafe fn lookup(fd: c_int) -> *mut PeFile {
    let mut status = MaybeUninit::<Stat>::uninit();
    if fstat(fd, status.as_mut_ptr()) != 0 {
        return ptr::null_mut();
    }
    let status = status.as_ptr();
    if (*status).st_mode & D_S_IFMT != D_S_IFREG {
        return ptr::null_mut();
    }
    let files = ptr::addr_of_mut!(G_FILES).cast::<PeFile>();
    lock();
    let mut index: c_int = 0;
    while index < G_NFILES {
        let known = files.add(index as usize);
        if (*known).dev == (*status).st_dev && (*known).ino == (*status).st_ino {
            unlock();
            return known;
        }
        index += 1;
    }
    let file;
    if G_NFILES < PE_CACHE as c_int {
        file = files.add(G_NFILES as usize);
        G_NFILES += 1;
    } else {
        file = files.add((G_NFILES - 1) as usize); // the last slot is recycled
        free((*file).sect.cast());
        free((*file).code.cast());
    }
    ptr::write_bytes(file, 0, 1);
    (*file).dev = (*status).st_dev;
    (*file).ino = (*status).st_ino;
    parse(fd, file);
    if (*file).is_pe && lxrt_trace_on() {
        let _ = fprintf(
            lxrt_trace_stream(),
            b"[lxrt] PE image on fd %d: machine 0x%x%s, %d sections, %d aarch64 code range(s)\n\0"
                .as_ptr()
                .cast(),
            fd,
            (*file).machine as c_int,
            chpe_note((*file).chpe),
            (*file).nsect,
            (*file).ncode,
        );
    }
    unlock();
    file
}

#[inline(always)]
fn chpe_note(chpe: bool) -> *const c_char {
    if chpe {
        b" (CHPE code map)\0".as_ptr().cast()
    } else {
        b"\0".as_ptr().cast()
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_pe_is(fd: c_int) -> bool {
    let file = lookup(fd);
    !file.is_null() && (*file).is_pe
}

// ------------------------------------------------- windows known by address

// Grown on demand. A fixed 512 filled up in a game's Wine process (every
// section mapping of every DLL is a window), and the windows remembered after
// that were dropped without a word: FEX's DLL, loaded late, then had no code
// window at all, each of its words counted as "not aarch64", and a scan
// passed a page of it with live svc sites (MEASURED: SIGSYS with x8 =
// 0x4C580020 in Minecraft Dungeons II, under lazy execute).
static mut G_WIN: *mut LxrtRange = ptr::null_mut();
static mut G_NWIN: c_int = 0;
static mut G_CAPWIN: c_int = 0;
// Images found in memory: their whole extent (code or not), so the JIT-output
// x18 pass never takes the x64 side of an ARM64X DLL, or a pure x64 image,
// for aarch64 code.
const PE_IMAGES: usize = 256;

#[repr(C)]
#[derive(Clone, Copy)]
struct Image {
    base: u64,
    end: u64,
}

static mut G_IMG: [Image; PE_IMAGES] = [Image { base: 0, end: 0 }; PE_IMAGES];
static mut G_NIMG: c_int = 0;

// `static bool said` of remember().
static mut REMEMBER_SAID: bool = false;

#[inline(always)]
unsafe fn images() -> *mut Image {
    ptr::addr_of_mut!(G_IMG).cast::<Image>()
}

unsafe fn remember(lo: u64, hi: u64) {
    lock();
    let mut index: c_int = 0;
    while index < G_NWIN {
        let window = G_WIN.add(index as usize);
        if (*window).start == lo && (*window).end == hi {
            unlock();
            return;
        }
        index += 1;
    }
    if G_NWIN == G_CAPWIN {
        let capacity: c_int = if G_CAPWIN != 0 {
            G_CAPWIN.wrapping_mul(2)
        } else {
            512
        };
        let grown = realloc(
            G_WIN.cast(),
            (capacity as usize).wrapping_mul(size_of::<LxrtRange>()),
        )
        .cast::<LxrtRange>();
        if !grown.is_null() {
            G_WIN = grown;
            G_CAPWIN = capacity;
        }
    }
    if G_NWIN < G_CAPWIN {
        let window = G_WIN.add(G_NWIN as usize);
        (*window).start = lo;
        (*window).end = hi;
        G_NWIN += 1;
    } else if !REMEMBER_SAID {
        REMEMBER_SAID = true;
        let _ = fprintf(
            lxrt_trace_stream(),
            b"[lxrt] pefile: cannot remember more code windows (%d)\n\0"
                .as_ptr()
                .cast(),
            G_NWIN,
        );
    }
    unlock();
}

// The 64 KiB chunks known to lie in no image, as a set (open addressing,
// linear probing; under g_lock). Every executable flip of anonymous memory
// asks discover() about its address -- FEX ARM64EC's code buffer is ordinary
// RWX memory to Wine, so that is every block FEX compiles -- and a miss walks
// down up to 1024 chunks with a mach_vm_read each. With 256 remembered chunks
// and a buffer of thousands, nearly every flip missed: Minecraft Dungeons II's
// protection spent its start-up there, 2 ms per flip under the W^X lock, and
// its 74 s deadline ran out (MEASURED, benchmarks/stage62, 15).
const NEG_SLOTS: usize = 16384; // power of two
const NEG_TOMB: u64 = 1; // never a 64 KiB-aligned address
static mut G_NEGH: [u64; NEG_SLOTS] = [0; NEG_SLOTS]; // chunk address, 0 empty, NEG_TOMB deleted
static mut G_NEG_USED: c_uint = 0; // live + deleted slots

#[inline(always)]
unsafe fn negh() -> *mut u64 {
    ptr::addr_of_mut!(G_NEGH).cast::<u64>()
}

#[inline(always)]
fn neg_slot(chunk: u64) -> usize {
    (((chunk >> 16).wrapping_mul(0x9E37_79B9_7F4A_7C15u64) >> 50) as c_uint as usize)
        & (NEG_SLOTS - 1)
}

unsafe fn neg_has(chunk: u64) -> bool {
    let table = negh();
    let mut slot = neg_slot(chunk);
    let mut probes = 0usize;
    while probes < NEG_SLOTS {
        let value = *table.add(slot);
        if value == chunk {
            return true;
        }
        if value == 0 {
            return false;
        }
        slot = (slot + 1) & (NEG_SLOTS - 1);
        probes += 1;
    }
    false
}

unsafe fn neg_add(chunk: u64) {
    if neg_has(chunk) {
        return;
    }
    let table = negh();
    if G_NEG_USED as usize >= NEG_SLOTS * 3 / 4 {
        // full of entries or tombstones: start over
        ptr::write_bytes(table, 0, NEG_SLOTS);
        G_NEG_USED = 0;
    }
    let mut slot = neg_slot(chunk);
    loop {
        let value = *table.add(slot);
        if value == 0 || value == NEG_TOMB {
            if value == 0 {
                G_NEG_USED += 1;
            }
            *table.add(slot) = chunk;
            return;
        }
        slot = (slot + 1) & (NEG_SLOTS - 1);
    }
}

// Forget the chunks that [addr, addr+len) touches.
unsafe fn neg_forget(address: u64, length: u64) {
    let table = negh();
    let lo = address & !0xFFFFu64;
    let hi = address.wrapping_add(length);
    if hi.wrapping_sub(lo) >> 16 <= 64 {
        let mut chunk = lo;
        while chunk < hi {
            let mut slot = neg_slot(chunk);
            let mut probes = 0usize;
            while probes < NEG_SLOTS && *table.add(slot) != 0 {
                if *table.add(slot) == chunk {
                    *table.add(slot) = NEG_TOMB;
                    break;
                }
                slot = (slot + 1) & (NEG_SLOTS - 1);
                probes += 1;
            }
            chunk = chunk.wrapping_add(0x10000);
        }
    } else {
        let mut slot = 0usize;
        while slot < NEG_SLOTS {
            let value = *table.add(slot);
            if value > NEG_TOMB && value >= lo && value < hi {
                *table.add(slot) = NEG_TOMB;
            }
            slot += 1;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_pe_forget(address: u64, length: u64) {
    if length == 0 {
        return;
    }
    let limit = address.wrapping_add(length);
    lock();
    let image = images();
    let mut index: c_int = 0;
    while index < G_NIMG {
        let entry = image.add(index as usize);
        if (*entry).base < limit && (*entry).end > address {
            G_NIMG -= 1;
            *entry = *image.add(G_NIMG as usize);
        } else {
            index += 1;
        }
    }
    let mut index: c_int = 0;
    while index < G_NWIN {
        let window = G_WIN.add(index as usize);
        if (*window).start < limit && (*window).end > address {
            G_NWIN -= 1;
            *window = *G_WIN.add(G_NWIN as usize);
        } else {
            index += 1;
        }
    }
    // What was found empty there may hold an image next (a reused view).
    neg_forget(address, length);
    unlock();
}

// ------------------------------------------------- images found in memory
//
// Wine's loader does not mmap a DLL's sections when their file alignment is
// finer than the host page (4 KiB sections, 16 KiB pages): it maps anonymous
// memory at the image base and reads the file into it, then makes the code
// executable with mprotect (MEASURED: ntdll.dll, 2026-10-06). No file is at
// hand then, but the whole image is in memory: the MZ/PE headers at a 64 KiB
// boundary below the code, the load config, the CHPE code map, all by RVA.

unsafe fn mem_read(address: u64, buffer: *mut u8, count: usize) -> bool {
    let mut got: u64 = 0;
    address >= 0x10000
        && mach_vm_read_overwrite(
            mach_task_self_,
            address,
            count as u64,
            buffer as usize as u64,
            &mut got,
        ) == D_KERN_SUCCESS
        && got == count as u64
}

// Images found in memory: their whole extent, aarch64 code or not. The
// JIT-output x18 pass (rewrite.c, LXRT_X18_JIT=1) must never touch the x64
// side of an ARM64X DLL or a pure x64 image decoded as aarch64 by chance.

#[no_mangle]
pub unsafe extern "C" fn lxrt_pe_in_image_not_code(start: u64, end: u64) -> bool {
    let mut inside = false;
    lock();
    let image = images();
    let mut index: c_int = 0;
    while index < G_NIMG {
        let entry = image.add(index as usize);
        if (*entry).base < end && (*entry).end > start {
            inside = true;
            break;
        }
        index += 1;
    }
    unlock();
    if !inside {
        return false;
    }
    let mut range = LxrtRange { start: 0, end: 0 };
    lxrt_pe_code_windows(start, end, &mut range, 1) == 0
}

// Negative cache (g_negh, above): 64 KiB chunks that were looked at and lie
// in no image.

// Parse the image at `base` (headers readable there) and remember its
// aarch64 code windows. True when it was a PE image with such code.
// Set by parse_in_memory when the image's metadata is not readable yet.
thread_local! {
    static T_PE_UNREADABLE: Cell<bool> = const { Cell::new(false) };
}

// Images whose metadata was unreadable, asked again at most every 20 ms: a
// scan meanwhile treats the address as in no known image (nothing filtered,
// which never hides an svc). Retried on every scan, an image whose .rdata
// stays unreadable (a short raw mapping, a guard page) made each scan above
// it walk down 1,024 chunks again, under the page lock (review).
const UNREAD_SLOTS: usize = 16;

#[repr(C)]
#[derive(Clone, Copy)]
struct Unread {
    base: u64,
    end: u64,
    next: u64,
}

static mut G_UNREAD: [Unread; UNREAD_SLOTS] = [Unread {
    base: 0,
    end: 0,
    next: 0,
}; UNREAD_SLOTS];

#[inline(always)]
unsafe fn unread() -> *mut Unread {
    ptr::addr_of_mut!(G_UNREAD).cast::<Unread>()
}

unsafe fn unread_note(base: u64, size: u32) {
    let now = clock_gettime_nsec_np(D_CLOCK_UPTIME_RAW);
    let next = now.wrapping_add(20_000_000u64);
    lock();
    let table = unread();
    let mut slot = 0usize;
    let mut index = 0usize;
    while index < UNREAD_SLOTS {
        if (*table.add(index)).base == base {
            slot = index;
            break;
        }
        if (*table.add(index)).next < (*table.add(slot)).next {
            slot = index;
        }
        index += 1;
    }
    let entry = table.add(slot);
    (*entry).base = base;
    (*entry).end = base.wrapping_add(size as u64);
    (*entry).next = next;
    unlock();
}

// Caller holds g_lock.
unsafe fn unread_wait(address: u64) -> bool {
    let now = clock_gettime_nsec_np(D_CLOCK_UPTIME_RAW);
    let table = unread();
    let mut index = 0usize;
    while index < UNREAD_SLOTS {
        let entry = table.add(index);
        if (*entry).next > now && address >= (*entry).base && address < (*entry).end {
            return true;
        }
        index += 1;
    }
    false
}

// Remember the image [base, end) by its base, when there is room. Takes g_lock.
#[inline(always)]
unsafe fn image_note(base: u64, end: u64) {
    lock();
    let image = images();
    let mut known = false;
    let mut index: c_int = 0;
    while index < G_NIMG {
        if (*image.add(index as usize)).base == base {
            known = true;
            break;
        }
        index += 1;
    }
    if !known && G_NIMG < PE_IMAGES as c_int {
        let entry = image.add(G_NIMG as usize);
        (*entry).base = base;
        (*entry).end = end;
        G_NIMG += 1;
    }
    unlock();
}

unsafe fn parse_in_memory(base: u64) -> bool {
    let mut dos = [0u8; 0x40];
    if !mem_read(base, dos.as_mut_ptr(), 0x40) || dos[0] != b'M' || dos[1] != b'Z' {
        return false;
    }
    let pe = rd32(dos.as_ptr().add(0x3C));
    let mut hdr = [0u8; 24 + 240];
    if pe < 0x40
        || pe > (1u32 << 20)
        || !mem_read(base.wrapping_add(pe as u64), hdr.as_mut_ptr(), 24 + 240)
        || hdr[0] != b'P'
        || hdr[1] != b'E'
        || hdr[2] != 0
        || hdr[3] != 0
    {
        return false;
    }
    let machine = rd16(hdr.as_ptr().add(4));
    let nsect = rd16(hdr.as_ptr().add(6)) as c_uint;
    let optsz = rd16(hdr.as_ptr().add(20)) as c_uint;
    let opt = hdr.as_ptr().add(24);
    // PE32 (a .NET assembly built for any CPU, a 32-bit image): an image
    // with no aarch64 code, remembered only so that nothing in it is ever
    // rewritten (rewrite.c pe_foreign: wine-mono's mscorlib.dll).
    if optsz >= 96 && rd16(opt) == 0x10B {
        let soi = rd32(opt.add(56));
        if soi == 0 || soi > (512u32 << 20) || nsect == 0 || nsect > 96 {
            return false;
        }
        if lxrt_trace_on() {
            let _ = fprintf(
                lxrt_trace_stream(),
                b"[lxrt] PE image in memory at 0x%llx: PE32, machine 0x%x, size 0x%x, no aarch64 code\n\0"
                    .as_ptr()
                    .cast(),
                base as c_ulonglong,
                machine as c_int,
                soi as c_uint,
            );
        }
        image_note(base, base.wrapping_add(soi as u64));
        return false;
    }
    if optsz < 112 + 16 * 8 || rd16(opt) != 0x20B {
        return false;
    }
    let image_base = rd64(opt.add(24));
    let size_of_image = rd32(opt.add(56));
    let ndirs = rd32(opt.add(108));
    if size_of_image == 0 || size_of_image > (512u32 << 20) || nsect == 0 || nsect > 96 {
        return false;
    }
    let mut code_buf = [LxrtRange { start: 0, end: 0 }; 64];
    let code_stack = code_buf.as_mut_ptr();
    let mut code = code_stack;
    let mut ncode: c_int = 0;
    let mut capcode: c_int = 64;
    let mut chpe = false;
    // The load config, the CHPE metadata and its code map live in .rdata,
    // which Wine may not have mapped or made readable yet when the first
    // scan in the image comes. Unreadable, the image is NOT remembered (nor
    // the chunk taken for empty, discover): remembered without its code
    // map, an ARM64EC image counted as pure x64, every word of it "not
    // aarch64", and a scan that came after this -- lazy execute's, at the
    // first fetch -- passed FEX's own DLL with live svc sites in it (MEASURED:
    // SIGSYS with x8 = 0x4C580020 in Minecraft Dungeons II). The next scan
    // asks again.
    let mut unreadable = false;
    if ndirs > 10 {
        let lc_rva = rd32(opt.add(112 + 10 * 8));
        let lc_size = rd32(opt.add(112 + 10 * 8 + 4));
        let mut lc = [0u8; 0xD0];
        let lc_ok = lc_rva != 0 && lc_size >= 0xD0 && lc_rva as u64 + 0xD0 <= size_of_image as u64;
        // Readable but zero: .rdata mapped and not yet filled in -- as good
        // as unreadable.
        if lc_ok
            && (!mem_read(base.wrapping_add(lc_rva as u64), lc.as_mut_ptr(), 0xD0)
                || rd32(lc.as_ptr()) == 0)
        {
            unreadable = true;
        } else if lc_ok && rd32(lc.as_ptr()) >= 0xD0 {
            // CHPEMetadataPointer is a VA, and in memory Wine has relocated
            // it to where the image is, while the header's ImageBase may still
            // say where it was linked for: FEX's DLL at 0x7ffffdbf0000 linked
            // for 0x180000000. Taken against ImageBase only, the pointer fell
            // outside the image, the code map was never read and the DLL
            // counted as pure x64 (MEASURED with a trace of this function: "0 aarch64 code
            // range(s)"). Either base, whichever it lies in.
            let chpe_va = rd64(lc.as_ptr().add(0xC8));
            let mut chpe_rva = u64::MAX;
            if chpe_va >= base && chpe_va.wrapping_sub(base).wrapping_add(12) <= size_of_image as u64
            {
                chpe_rva = chpe_va.wrapping_sub(base);
            } else if chpe_va > image_base
                && chpe_va.wrapping_sub(image_base).wrapping_add(12) <= size_of_image as u64
            {
                chpe_rva = chpe_va.wrapping_sub(image_base);
            }
            if chpe_rva != u64::MAX {
                let mut meta = [0u8; 12];
                if !mem_read(base.wrapping_add(chpe_rva), meta.as_mut_ptr(), 12) {
                    unreadable = true;
                } else {
                    let map_rva = rd32(meta.as_ptr().add(4));
                    let count = rd32(meta.as_ptr().add(8));
                    // An image with a CHPE pointer always has a code map:
                    // none, or one that cannot be read whole, is not ready
                    // (or not sane) -- and never "pure x64", which would hide
                    // its aarch64 code from the scans.
                    let entries = if count != 0
                        && count < 65536
                        && map_rva as u64 + count as u64 * 8 <= size_of_image as u64
                    {
                        malloc(count as usize * 8).cast::<u8>()
                    } else {
                        ptr::null_mut()
                    };
                    if count > 64 {
                        code = malloc((count as usize).wrapping_mul(size_of::<LxrtRange>()))
                            .cast::<LxrtRange>();
                        capcode = count as c_int;
                    }
                    if entries.is_null()
                        || code.is_null()
                        || !mem_read(
                            base.wrapping_add(map_rva as u64),
                            entries,
                            count as usize * 8,
                        )
                    {
                        unreadable = true;
                    } else {
                        chpe = true;
                        let mut entry: u32 = 0;
                        while entry < count && ncode < capcode {
                            let start = rd32(entries.add(entry as usize * 8));
                            let length = rd32(entries.add(entry as usize * 8 + 4));
                            if (start & 3) <= 1 && length != 0 {
                                let slot = code.add(ncode as usize);
                                (*slot).start = base.wrapping_add((start & !3u32) as u64);
                                (*slot).end = base
                                    .wrapping_add((start & !3u32) as u64)
                                    .wrapping_add(length as u64);
                                ncode += 1;
                            }
                            entry += 1;
                        }
                    }
                    free(entries.cast());
                }
            }
        }
    }
    if unreadable {
        if code != code_stack {
            free(code.cast());
        }
        T_PE_UNREADABLE.with(|flag| flag.set(true));
        unread_note(base, size_of_image);
        if lxrt_trace_on() {
            let _ = fprintf(
                lxrt_trace_stream(),
                b"[lxrt] PE image in memory at 0x%llx: load config or code map not readable yet\n\0"
                    .as_ptr()
                    .cast(),
                base as c_ulonglong,
            );
        }
        return false;
    }
    if !chpe && machine == 0xAA64 {
        let slen = nsect as usize * 40;
        let sb = malloc(slen).cast::<u8>();
        if !sb.is_null()
            && mem_read(
                base.wrapping_add(pe as u64)
                    .wrapping_add(24)
                    .wrapping_add(optsz as u64),
                sb,
                slen,
            )
        {
            let mut index: c_uint = 0;
            while index < nsect && ncode < 64 {
                let raw = sb.add(index as usize * 40);
                let vsize = rd32(raw.add(8));
                let va = rd32(raw.add(12));
                let ch = rd32(raw.add(36));
                if ch & 0x2000_0000u32 != 0 && vsize != 0 {
                    let slot = code.add(ncode as usize);
                    (*slot).start = base.wrapping_add(va as u64);
                    (*slot).end = base.wrapping_add(va as u64).wrapping_add(vsize as u64);
                    ncode += 1;
                }
                index += 1;
            }
        }
        free(sb.cast());
    }
    if lxrt_trace_on() {
        let _ = fprintf(
            lxrt_trace_stream(),
            b"[lxrt] PE image in memory at 0x%llx: machine 0x%x%s, size 0x%x, %d aarch64 code range(s)\n\0"
                .as_ptr()
                .cast(),
            base as c_ulonglong,
            machine as c_int,
            chpe_note(chpe),
            size_of_image as c_uint,
            ncode,
        );
    }
    let image_end = base.wrapping_add(size_of_image as u64);
    image_note(base, image_end);
    let mut index: c_int = 0;
    while index < ncode {
        let range = code.add(index as usize);
        index += 1;
        let lo = (*range).start.wrapping_add(3) & !3u64;
        let hi = (*range).end & !3u64;
        if lo >= hi || hi > image_end {
            continue;
        }
        let function = malloc(size_of::<LxrtRange>()).cast::<LxrtRange>();
        if !function.is_null() {
            (*function).start = lo;
            (*function).end = hi;
            lxrt_fn_register(lo, hi, function, 1);
        }
        remember(lo, hi);
    }
    if code != code_stack {
        free(code.cast());
    }
    ncode > 0
}

// An image whose code could hold `addr`: its headers at a 64 KiB boundary
// at or below addr (Wine places images at 64 KiB granularity), within 64 MiB.
unsafe fn discover(address: u64) {
    if lxrt_jit_contains(address) {
        return; // a JIT buffer, never an image
    }
    let top = address & !0xFFFFu64;
    let mut at = top;
    let mut bottom = top;
    lock();
    let mut known_empty = neg_has(top) || unread_wait(address);
    unlock();
    if known_empty {
        return;
    }
    let mut saw_mz = false;
    let mut step: c_uint = 0;
    while step < 1024 && at >= 0x10000 {
        let mut word = [0u8; 4];
        if at != top {
            // A chunk known to lie in no image: an image holding addr with
            // its headers further down would hold that chunk too; stop.
            lock();
            known_empty = neg_has(at);
            unlock();
            if known_empty {
                break;
            }
        }
        if !mem_read(at, word.as_mut_ptr(), 4) {
            // Unmapped: no image spans it (images are contiguous); stop.
            break;
        }
        bottom = at;
        if word[0] == b'M' && word[1] == b'Z' {
            saw_mz = true;
            T_PE_UNREADABLE.with(|flag| flag.set(false));
            parse_in_memory(at);
            if T_PE_UNREADABLE.with(|flag| flag.get()) {
                return; // not readable yet: ask again next time, nothing cached
            }
            // Found: the image holding addr, with aarch64 code or not (a
            // PE32 or pure x64 image is remembered with none).
            let mut holds = false;
            lock();
            let image = images();
            let mut index: c_int = 0;
            while index < G_NIMG {
                let entry = image.add(index as usize);
                if (*entry).base <= address && (*entry).end > address {
                    holds = true;
                    break;
                }
                index += 1;
            }
            unlock();
            if holds {
                return;
            }
        }
        step += 1;
        at = at.wrapping_sub(0x10000);
    }
    // Without headers anywhere on the way down, no chunk walked lies in an
    // image either (one would hold the chunk where the walk stopped).
    lock();
    if saw_mz {
        neg_add(top);
    } else {
        let mut chunk = bottom;
        while chunk <= top {
            neg_add(chunk);
            chunk = chunk.wrapping_add(0x10000);
        }
    }
    unlock();
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_pe_code_windows(
    start: u64,
    end: u64,
    out: *mut LxrtRange,
    max: c_int,
) -> c_int {
    let mut count: c_int = 0;
    lock();
    let mut index: c_int = 0;
    while index < G_NWIN && count < max {
        let window = G_WIN.add(index as usize);
        let lo = if (*window).start > start {
            (*window).start
        } else {
            start
        };
        let hi = if (*window).end < end {
            (*window).end
        } else {
            end
        };
        if lo < hi {
            let slot = out.add(count as usize);
            (*slot).start = lo;
            (*slot).end = hi;
            count += 1;
        }
        index += 1;
    }
    unlock();
    count
}

// The images (their whole extent, aarch64 code or not) that [start, end)
// intersects, found first if need be: the one holding start, and any whose
// headers sit at a 64 KiB boundary inside the range. Returns how many.
#[no_mangle]
pub unsafe extern "C" fn lxrt_pe_images(
    start: u64,
    end: u64,
    out: *mut LxrtRange,
    max: c_int,
) -> c_int {
    let mut range = LxrtRange { start: 0, end: 0 };
    if lxrt_pe_code_windows(start, end, &mut range, 1) == 0 {
        discover(start);
    }
    let image = images();
    let mut at = start.wrapping_add(0xFFFF) & !0xFFFFu64;
    while at < end {
        let mut known = false;
        lock();
        let mut index: c_int = 0;
        while index < G_NIMG {
            if (*image.add(index as usize)).base == at {
                known = true;
                break;
            }
            index += 1;
        }
        unlock();
        let mut word = [0u8; 2];
        if !known && mem_read(at, word.as_mut_ptr(), 2) && word[0] == b'M' && word[1] == b'Z' {
            parse_in_memory(at);
        }
        at = at.wrapping_add(0x10000);
    }
    let mut count: c_int = 0;
    lock();
    let mut index: c_int = 0;
    while index < G_NIMG && count < max {
        let entry = image.add(index as usize);
        if (*entry).base < end && (*entry).end > start {
            let slot = out.add(count as usize);
            (*slot).start = (*entry).base;
            (*slot).end = (*entry).end;
            count += 1;
        }
        index += 1;
    }
    unlock();
    count
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_pe_intersects(start: u64, end: u64) -> bool {
    let mut range = LxrtRange { start: 0, end: 0 };
    if lxrt_pe_code_windows(start, end, &mut range, 1) > 0 {
        return true;
    }
    discover(start);
    lxrt_pe_code_windows(start, end, &mut range, 1) > 0
}

// The aarch64 code windows of the mapping [file_off, file_off + len) of the
// PE on `fd`, placed at `addr` (what lxrt_elf_exec_sections gives for an
// ELF), registered as function tables and remembered by address. 0 for a
// file that is not a PE or has no aarch64 code.
#[no_mangle]
pub unsafe extern "C" fn lxrt_pe_exec_sections(
    fd: c_int,
    file_off: u64,
    len: u64,
    addr: u64,
    code: *mut LxrtRange,
    max: c_int,
) -> c_int {
    let file = lookup(fd);
    if file.is_null() || !(*file).is_pe || !(*file).has_code || len == 0 {
        return 0;
    }
    let file_end = file_off.wrapping_add(len);
    let mut count: c_int = 0;
    let mut index: c_int = 0;
    while index < (*file).ncode && count < max {
        let range = (*file).code.add(index as usize);
        index += 1;
        let rva = (*range).start as u32;
        let mut clen = (*range).end.wrapping_sub((*range).start) as u32;
        // the range may span sections only in theory; it is placed by its first byte's section
        let raw = rva_offset(file, rva, 0);
        if raw < 0 {
            continue;
        }
        // clip to the section's raw bytes
        let mut section_index: c_int = 0;
        while section_index < (*file).nsect {
            let section = (*file).sect.add(section_index as usize);
            let span = if (*section).vsize > (*section).rawsz {
                (*section).vsize
            } else {
                (*section).rawsz
            };
            if rva >= (*section).va && rva < (*section).va.wrapping_add(span) {
                let inside = rva.wrapping_sub((*section).va);
                let avail = if (*section).rawsz > inside {
                    (*section).rawsz - inside
                } else {
                    0
                };
                if clen > avail {
                    clen = avail;
                }
                break;
            }
            section_index += 1;
        }
        let raw_start = raw as u64;
        let raw_end = raw_start.wrapping_add(clen as u64);
        let lo = if raw_start > file_off {
            raw_start
        } else {
            file_off
        };
        let hi = if raw_end < file_end { raw_end } else { file_end };
        if lo >= hi {
            continue;
        }
        let mut wlo = addr.wrapping_add(lo.wrapping_sub(file_off));
        let mut whi = addr.wrapping_add(hi.wrapping_sub(file_off));
        wlo = wlo.wrapping_add(3) & !3u64;
        whi &= !3u64;
        if wlo >= whi {
            continue;
        }
        let slot = code.add(count as usize);
        (*slot).start = wlo;
        (*slot).end = whi;
        count += 1;
        let function = malloc(size_of::<LxrtRange>()).cast::<LxrtRange>();
        if !function.is_null() {
            (*function).start = wlo;
            (*function).end = whi;
            lxrt_fn_register(wlo, whi, function, 1);
        }
        remember(wlo, whi);
    }
    if count != 0 && lxrt_trace_on() {
        let _ = fprintf(
            lxrt_trace_stream(),
            b"[lxrt] PE code mapped at 0x%llx+0x%llx (file offset 0x%llx): %d window(s), first 0x%llx-0x%llx\n\0"
                .as_ptr()
                .cast(),
            addr as c_ulonglong,
            len as c_ulonglong,
            file_off as c_ulonglong,
            count,
            (*code).start as c_ulonglong,
            (*code).end as c_ulonglong,
        );
    }
    count
}
