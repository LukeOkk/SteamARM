// Host-page mremap for anonymous memory. See mremap.h for guest limitations.

use std::cell::UnsafeCell;
use std::ffi::{c_char, c_int, c_long, c_uint, c_ulonglong, c_void};
use std::mem::{size_of, MaybeUninit};
use std::ptr;
use std::sync::atomic::{AtomicI32, Ordering};

#[allow(dead_code)]
mod sdk {
    include!(concat!(env!("OUT_DIR"), "/privmap_constants.rs"));
    include!(concat!(env!("OUT_DIR"), "/pathfd_constants.rs"));
}
use sdk::{
    D_EFAULT, D_EINVAL, D_ENOMEM, D_MAP_ANON, D_MAP_FIXED, D_MAP_PRIVATE, D_MAP_SHARED,
    D_PROT_EXEC, D_PROT_READ, D_PROT_WRITE,
};

const HOST_PAGE: u64 = 16_384;

// Linux MREMAP_* flags.
const L_MAYMOVE: c_int = 1;
const L_FIXED: c_int = 2;
const L_DONTUNMAP: c_int = 4;

const KERN_SUCCESS: c_int = 0;
const KERN_NO_SPACE: c_int = 3;
const MACH_PORT_NULL: MachPort = 0;
const MACH_PORT_RIGHT_SEND: c_uint = 0;
const VM_FLAGS_FIXED: c_int = 0;
const VM_FLAGS_ANYWHERE: c_int = 1;
const VM_FLAGS_OVERWRITE: c_int = 0x4000;
const VM_INHERIT_COPY: c_int = 1;
const VM_INHERIT_NONE: c_int = 2;
const VM_PROT_READ: c_int = 1;
const VM_PROT_WRITE: c_int = 2;
const VM_PROT_EXECUTE: c_int = 4;
const VM_REGION_BASIC_INFO_64: c_int = 9;
const PROC_PIDREGIONPATHINFO: c_int = 8;
const PROC_PIDPATHINFO_MAXSIZE: usize = 4 * 1024;
const F_SETFD: c_int = 2;
const FD_CLOEXEC: c_int = 1;
const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;
const MAP_FAILED: *mut c_void = !0usize as *mut c_void;

type MachPort = u32;
type MachAddress = u64;
type MachSize = u64;

// vm_region_basic_info_data_64_t (<mach/vm_region.h>, #pragma pack(4)).
#[repr(C, packed(4))]
#[derive(Clone, Copy)]
struct RegionBasicInfo64 {
    protection: c_int,
    max_protection: c_int,
    inheritance: c_uint,
    shared: c_uint,
    reserved: c_uint,
    offset: u64,
    behavior: c_int,
    user_wired_count: u16,
}
const VM_REGION_BASIC_INFO_COUNT_64: c_uint =
    (size_of::<RegionBasicInfo64>() / size_of::<c_int>()) as c_uint;
const _: () = assert!(size_of::<RegionBasicInfo64>() == 36);

// struct proc_regionwithpathinfo (<sys/proc_info.h>).
#[repr(C)]
struct ProcRegionInfo {
    pri_protection: u32,
    pri_max_protection: u32,
    pri_inheritance: u32,
    pri_flags: u32,
    pri_offset: u64,
    pri_behavior: u32,
    pri_user_wired_count: u32,
    pri_user_tag: u32,
    pri_pages_resident: u32,
    pri_pages_shared_now_private: u32,
    pri_pages_swapped_out: u32,
    pri_pages_dirtied: u32,
    pri_ref_count: u32,
    pri_shadow_depth: u32,
    pri_share_mode: u32,
    pri_private_pages_resident: u32,
    pri_shared_pages_resident: u32,
    pri_obj_id: u32,
    pri_depth: u32,
    pri_address: u64,
    pri_size: u64,
}

#[repr(C)]
struct VnodeInfoPath {
    vip_vi: [u64; 19], // struct vnode_info, opaque here
    vip_path: [c_char; 1024],
}

#[repr(C)]
struct ProcRegionWithPathInfo {
    prp_prinfo: ProcRegionInfo,
    prp_vip: VnodeInfoPath,
}
const _: () = assert!(size_of::<ProcRegionInfo>() == 96);
const _: () = assert!(size_of::<ProcRegionWithPathInfo>() == 1272);

// pthread_mutex_t (<sys/_pthread/_pthread_types.h>).
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}

#[link(name = "System")]
unsafe extern "C" {
    static mach_task_self_: MachPort;
    fn mach_port_deallocate(task: MachPort, name: MachPort) -> c_int;
    fn mach_port_mod_refs(task: MachPort, name: MachPort, right: c_uint, delta: c_int) -> c_int;
    fn mach_vm_allocate(
        task: MachPort,
        address: *mut MachAddress,
        size: MachSize,
        flags: c_int,
    ) -> c_int;
    fn mach_vm_deallocate(task: MachPort, address: MachAddress, size: MachSize) -> c_int;
    fn mach_vm_region(
        task: MachPort,
        address: *mut MachAddress,
        size: *mut MachSize,
        flavor: c_int,
        info: *mut c_int,
        count: *mut c_uint,
        object: *mut MachPort,
    ) -> c_int;
    fn mach_vm_remap(
        target_task: MachPort,
        target_address: *mut MachAddress,
        size: MachSize,
        mask: MachAddress,
        flags: c_int,
        source_task: MachPort,
        source_address: MachAddress,
        copy: c_int,
        current_protection: *mut c_int,
        maximum_protection: *mut c_int,
        inheritance: c_int,
    ) -> c_int;
    fn fileport_makeport(fd: c_int, port: *mut MachPort) -> c_int;
    fn fileport_makefd(port: MachPort) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn __error() -> *mut c_int;
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
    fn close(fd: c_int) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn getpid() -> c_int;
    fn proc_regionfilename(pid: c_int, address: u64, buffer: *mut c_void, size: u32) -> c_int;
    fn proc_pidinfo(pid: c_int, flavor: c_int, arg: u64, buffer: *mut c_void, size: c_int)
        -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_trace_stream() -> *mut c_void;
    fn lxrt_subpage_tracked(addr: u64, len: u64) -> bool;
    fn lxrt_wx_forget(addr: u64, len: u64);
    fn lxrt_wx_unify_prot(addr: u64, len: u64) -> bool;
}

#[inline(always)]
fn task() -> MachPort {
    unsafe { mach_task_self_ }
}

#[inline(always)]
fn align_down(value: u64) -> u64 {
    value & !(HOST_PAGE - 1)
}

#[inline(always)]
fn align_up(value: u64) -> u64 {
    align_down(value.wrapping_add(HOST_PAGE - 1))
}

// LERR(e)
#[inline(always)]
fn lerr(error: c_int) -> c_long {
    unsafe { lxrt_errno_to_linux(error) }.wrapping_neg() as c_long
}

#[inline(always)]
fn errno() -> c_int {
    unsafe { *__error() }
}

// ------------------------------------------------ shared file mappings
//
// Growing a MAP_SHARED file mapping has to map MORE OF THE FILE: anonymous
// pages would silently stop sharing. Linux finds the file from the VMA; here
// nothing in the address space leads back to it (an unlinked memfd has no
// path), so dispatch.c records each such mapping with a Mach fileport of its
// descriptor. A fileport keeps the open file alive without taking a
// descriptor number the guest could see, close or collide with.
//
// Why it matters (MEASURED, benchmarks/stage27-android-display.txt): every
// Wayland compositor built with libwayland-server grows a client's wl_shm
// pool with mremap(MREMAP_MAYMOVE) when the client sends wl_shm_pool.resize.
// Weston under lxrun killed its own desktop shell with "wl_shm_pool#3:
// error 2: failed mremap" before this.
const SHF_MAX: usize = 512;

#[repr(C)]
#[derive(Clone, Copy)]
struct Shf {
    start: u64,
    end: u64,
    off: u64,
    prot: c_int,
    port: MachPort,
}

impl Shf {
    const ZERO: Self = Self {
        start: 0,
        end: 0,
        off: 0,
        prot: 0,
        port: MACH_PORT_NULL,
    };
}

// The C statics, kept as plain memory: g_shf is only touched under
// g_shf_lock (a raw pthread mutex, as in C: no poisoning, no allocation,
// usable from the guest syscall path and the atfork handlers).
#[repr(transparent)]
struct Racy<T>(UnsafeCell<T>);
unsafe impl<T> Sync for Racy<T> {}

static G_SHF: Racy<[Shf; SHF_MAX]> = Racy(UnsafeCell::new([Shf::ZERO; SHF_MAX]));
// A plain int in C; lxrt_mremap_forget_shared peeks at it without the lock
// and the fork child zeroes it. Relaxed is the same plain load/store.
static G_NSHF: AtomicI32 = AtomicI32::new(0);
static G_SHF_LOCK: Racy<PthreadMutex> = Racy(UnsafeCell::new(PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
}));

#[inline(always)]
fn shf(index: c_int) -> *mut Shf {
    unsafe { G_SHF.0.get().cast::<Shf>().add(index as usize) }
}

#[inline(always)]
fn nshf() -> c_int {
    G_NSHF.load(Ordering::Relaxed)
}

#[inline(always)]
fn shf_lock() {
    unsafe {
        pthread_mutex_lock(G_SHF_LOCK.0.get());
    }
}

#[inline(always)]
fn shf_unlock() {
    unsafe {
        pthread_mutex_unlock(G_SHF_LOCK.0.get());
    }
}

// LXRT_FORK_SAFE(mremap_shf, g_shf_lock)
extern "C" fn mremap_shf_fork_prepare() {
    shf_lock();
}

extern "C" fn mremap_shf_fork_release() {
    shf_unlock();
}

// Mach port rights are not inherited across fork: the child's records name
// nothing. Its inherited mappings are simply not growable (as before).
extern "C" fn shf_child() {
    G_NSHF.store(0, Ordering::Relaxed);
}

// The C had two constructors, priority 200 (LXRT_FORK_SAFE) then 201
// (shf_register): one initializer here registers both in that order.
extern "C" fn shf_register() {
    unsafe {
        pthread_atfork(
            Some(mremap_shf_fork_prepare),
            Some(mremap_shf_fork_release),
            Some(mremap_shf_fork_release),
        );
        pthread_atfork(None, None, Some(shf_child));
    }
}

#[used]
#[link_section = "__DATA,__mod_init_func"]
static SHF_REGISTER_INIT: extern "C" fn() = shf_register;

unsafe fn shf_drop(index: c_int) {
    if (*shf(index)).port != MACH_PORT_NULL {
        mach_port_deallocate(task(), (*shf(index)).port);
    }
    let last = nshf() - 1;
    G_NSHF.store(last, Ordering::Relaxed);
    *shf(index) = *shf(last);
}

// Caller holds g_shf_lock. Trims, splits or drops what [a, b) covers.
unsafe fn shf_forget_locked(a: u64, b: u64) {
    let mut i: c_int = 0;
    while i < nshf() {
        let e = shf(i);
        if b <= (*e).start || a >= (*e).end {
            i += 1;
            continue;
        }
        if a <= (*e).start && b >= (*e).end {
            shf_drop(i);
            continue;
        }
        if a > (*e).start && b < (*e).end {
            // The middle goes: the tail becomes a record of its own, holding
            // its own reference to the file.
            if (nshf() as usize) < SHF_MAX
                && mach_port_mod_refs(task(), (*e).port, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS
            {
                let n = nshf();
                *shf(n) = Shf {
                    start: b,
                    end: (*e).end,
                    off: (*e).off.wrapping_add(b.wrapping_sub((*e).start)),
                    prot: (*e).prot,
                    port: (*e).port,
                };
                G_NSHF.store(n + 1, Ordering::Relaxed);
            }
            (*e).end = a;
        } else if a <= (*e).start {
            (*e).off = (*e).off.wrapping_add(b.wrapping_sub((*e).start));
            (*e).start = b;
        } else {
            (*e).end = a;
        }
        i += 1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_mremap_note_shared(
    addr: u64,
    len: u64,
    fd: c_int,
    off: u64,
    prot: c_int,
) {
    if len == 0 || fd < 0 || addr % HOST_PAGE != 0 || off % HOST_PAGE != 0 {
        return;
    }
    let end = addr.wrapping_add(align_up(len));
    let mut port: MachPort = MACH_PORT_NULL;
    if fileport_makeport(fd, &mut port) != 0 {
        return;
    }
    shf_lock();
    shf_forget_locked(addr, end);
    if (nshf() as usize) < SHF_MAX {
        let n = nshf();
        *shf(n) = Shf {
            start: addr,
            end,
            off,
            prot,
            port,
        };
        G_NSHF.store(n + 1, Ordering::Relaxed);
        port = MACH_PORT_NULL;
    }
    shf_unlock();
    if port != MACH_PORT_NULL {
        mach_port_deallocate(task(), port);
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_mremap_forget_shared(addr: u64, len: u64) {
    if len == 0 || nshf() == 0 {
        return;
    }
    let a = align_down(addr);
    let b = align_up(addr.wrapping_add(len));
    shf_lock();
    shf_forget_locked(a, b);
    shf_unlock();
}

// Is [a, b) free (nothing mapped)? Probed by reserving it.
unsafe fn range_free(a: u64, len: u64) -> bool {
    let mut at: MachAddress = a;
    if mach_vm_allocate(task(), &mut at, len, VM_FLAGS_FIXED) != KERN_SUCCESS {
        return false;
    }
    mach_vm_deallocate(task(), at, len);
    true
}

// mremap of a recorded shared file mapping; lengths are host-page rounded.
// Returns the new address, a negative Linux errno, or 1 when the range is
// not (wholly) a recorded mapping.
unsafe fn remap_shared_file(
    old_addr: u64,
    old_len: u64,
    new_len: u64,
    lflags: c_int,
    new_addr: u64,
    prot: c_int,
) -> c_long {
    shf_lock();
    let mut rec = Shf::ZERO;
    let mut found = false;
    let mut i: c_int = 0;
    while i < nshf() && !found {
        let e = shf(i);
        if (*e).start <= old_addr && old_addr.wrapping_add(old_len) <= (*e).end {
            rec = *e;
            found = true;
        }
        i += 1;
    }
    if !found || (lflags & L_DONTUNMAP) != 0 {
        shf_unlock();
        return 1;
    }
    let fd = fileport_makefd(rec.port);
    shf_unlock();
    if fd < 0 {
        return 1;
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    let off = rec.off.wrapping_add(old_addr.wrapping_sub(rec.start));
    let mprot = prot & (D_PROT_READ | D_PROT_WRITE);
    let ret: c_long;
    // Linux tries to grow in place first, MAYMOVE or not.
    let tail = old_addr.wrapping_add(old_len);
    let delta = new_len.wrapping_sub(old_len);
    if (lflags & L_FIXED) == 0
        && new_len > old_len
        && tail <= u64::MAX - delta
        && range_free(tail, delta)
    {
        let p = mmap(
            tail as usize as *mut c_void,
            delta as usize,
            mprot,
            D_MAP_SHARED | D_MAP_FIXED,
            fd,
            off.wrapping_add(old_len) as i64,
        );
        if p == tail as usize as *mut c_void {
            lxrt_mremap_note_shared(old_addr, new_len, fd, off, mprot);
            close(fd);
            return old_addr as c_long;
        }
        if p != MAP_FAILED {
            munmap(p, delta as usize);
        }
    }
    if (lflags & L_MAYMOVE) == 0 {
        close(fd);
        return lerr(D_ENOMEM);
    }
    // A W^X range there is replaced (Linux unmaps the destination first):
    // out of the table before the file is, or a fetch in between flips,
    // scans and rewrites the file's page as the guest's private code.
    if (lflags & L_FIXED) != 0 {
        lxrt_wx_forget(new_addr, new_len);
    }
    let dest = if (lflags & L_FIXED) != 0 {
        mmap(
            new_addr as usize as *mut c_void,
            new_len as usize,
            mprot,
            D_MAP_SHARED | D_MAP_FIXED,
            fd,
            off as i64,
        )
    } else {
        mmap(
            ptr::null_mut(),
            new_len as usize,
            mprot,
            D_MAP_SHARED,
            fd,
            off as i64,
        )
    };
    if dest == MAP_FAILED {
        ret = lerr(errno());
        close(fd);
        return ret;
    }
    if munmap(old_addr as usize as *mut c_void, old_len as usize) != 0 {
        ret = lerr(errno());
        munmap(dest, new_len as usize);
        close(fd);
        return ret;
    }
    lxrt_mremap_forget_shared(old_addr, old_len);
    lxrt_mremap_note_shared(dest as usize as u64, new_len, fd, off, mprot);
    close(fd);
    dest as usize as c_long
}

// mach_vm_region returns the NEXT region for a hole, not necessarily the
// region containing the address. Check containment and release its send right.
unsafe fn region(addr: u64, end: *mut u64, info: *mut RegionBasicInfo64) -> bool {
    let mut base: MachAddress = addr;
    let mut size: MachSize = 0;
    let mut count: c_uint = VM_REGION_BASIC_INFO_COUNT_64;
    let mut object: MachPort = MACH_PORT_NULL;
    let kr = mach_vm_region(
        task(),
        &mut base,
        &mut size,
        VM_REGION_BASIC_INFO_64,
        info.cast::<c_int>(),
        &mut count,
        &mut object,
    );
    if object != MACH_PORT_NULL {
        mach_port_deallocate(task(), object);
    }
    if kr != KERN_SUCCESS
        || base > addr
        || size > u64::MAX - base
        || addr >= base.wrapping_add(size)
    {
        return false;
    }
    *end = base.wrapping_add(size);
    true
}

unsafe fn staging(len: u64, fixed: bool, target: u64) -> *mut c_void {
    // A free FIXED target can itself be selected by mmap(NULL). Hold colliding
    // allocations until we have a disjoint one. A target of length len can
    // intersect at most two disjoint allocations of that same length.
    let mut held: [*mut c_void; 3] = [ptr::null_mut(); 3];
    let mut n: usize = 0;
    let mut result = MAP_FAILED;
    let mut e = D_ENOMEM;
    while n < 3 {
        let p = mmap(
            ptr::null_mut(),
            len as usize,
            D_PROT_READ | D_PROT_WRITE,
            D_MAP_PRIVATE | D_MAP_ANON,
            -1,
            0,
        );
        if p == MAP_FAILED {
            e = errno();
            break;
        }
        let at = p as usize as u64;
        if !fixed || at >= target.wrapping_add(len) || target >= at.wrapping_add(len) {
            result = p;
            break;
        }
        held[n] = p;
        n += 1;
    }
    while n != 0 {
        n -= 1;
        munmap(held[n], len as usize);
    }
    if result == MAP_FAILED {
        *__error() = e;
    }
    result
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_mremap_copy_out(dst: *mut c_void, src: u64, len: u64) -> c_int {
    if len == 0 {
        return 0;
    }
    // A private copy-on-write alias of the enclosing host pages, made
    // readable: the source may be PROT_NONE or execute-only to the guest.
    let hs = align_down(src);
    let he = align_up(src.wrapping_add(len));
    let span = he.wrapping_sub(hs);
    let mut snap: MachAddress = 0;
    let mut cur: c_int = 0;
    let mut max: c_int = 0;
    if mach_vm_remap(
        task(),
        &mut snap,
        span,
        0,
        VM_FLAGS_ANYWHERE,
        task(),
        hs,
        1,
        &mut cur,
        &mut max,
        VM_INHERIT_NONE,
    ) != KERN_SUCCESS
    {
        return -1;
    }
    let mut rc = -1;
    if mprotect(snap as usize as *mut c_void, span as usize, D_PROT_READ) == 0 {
        ptr::copy_nonoverlapping(
            snap.wrapping_add(src.wrapping_sub(hs)) as usize as *const u8,
            dst.cast::<u8>(),
            len as usize,
        );
        rc = 0;
    }
    mach_vm_deallocate(task(), snap, span);
    rc
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_mremap_host_prot(addr: u64, shared: *mut bool) -> c_int {
    let mut info = MaybeUninit::<RegionBasicInfo64>::uninit();
    let mut end: u64 = 0;
    if !region(addr, &mut end, info.as_mut_ptr()) {
        return -1;
    }
    let info = info.assume_init();
    if !shared.is_null() {
        *shared = info.shared != 0;
    }
    let protection = info.protection;
    (if protection & VM_PROT_READ != 0 { D_PROT_READ } else { 0 })
        | (if protection & VM_PROT_WRITE != 0 { D_PROT_WRITE } else { 0 })
        | (if protection & VM_PROT_EXECUTE != 0 { D_PROT_EXEC } else { 0 })
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_mremap(
    old_addr: u64,
    old_len: u64,
    new_len: u64,
    lflags: c_int,
    new_addr: u64,
) -> c_long {
    let mut old_len = old_len;
    let mut new_len = new_len;
    if old_len == 0
        || new_len == 0
        || (lflags & !7) != 0
        || (old_addr % HOST_PAGE) != 0
        || ((lflags & (L_FIXED | L_DONTUNMAP)) != 0 && (lflags & L_MAYMOVE) == 0)
    {
        return lerr(D_EINVAL);
    }
    if old_len > u64::MAX - (HOST_PAGE - 1) || new_len > u64::MAX - (HOST_PAGE - 1) {
        return lerr(D_ENOMEM);
    }
    old_len = align_up(old_len);
    new_len = align_up(new_len);
    if old_addr > u64::MAX - old_len {
        return lerr(D_EFAULT);
    }
    if (lflags & L_DONTUNMAP) != 0 && old_len != new_len {
        return lerr(D_EINVAL);
    }
    if (lflags & L_FIXED) != 0
        && (new_addr % HOST_PAGE != 0
            || new_addr > u64::MAX - new_len
            || (new_addr < old_addr.wrapping_add(old_len)
                && old_addr < new_addr.wrapping_add(new_len)))
    {
        return lerr(D_EINVAL);
    }
    if lxrt_subpage_tracked(old_addr, old_len)
        || ((lflags & L_FIXED) != 0 && lxrt_subpage_tracked(new_addr, new_len))
    {
        fprintf(
            lxrt_trace_stream(),
            b"[lxrt] mremap: sub-page mapping not supported yet\n\0"
                .as_ptr()
                .cast::<c_char>(),
        );
        return lerr(D_ENOMEM);
    }

    // A range the W^X table holds whole has one protection to the guest but
    // per-page states on the host (read-only, read-write, read-execute):
    // one host protection first, which the check below wants and the
    // destination takes (wxsplit.c, lxrt_wx_unify_prot).
    lxrt_wx_unify_prot(old_addr, old_len);
    let mut first = MaybeUninit::<RegionBasicInfo64>::uninit();
    let mut info = MaybeUninit::<RegionBasicInfo64>::uninit();
    let mut end: u64 = 0;
    if !region(old_addr, &mut end, first.as_mut_ptr()) {
        return lerr(D_EFAULT);
    }
    let first_protection = (*first.as_ptr()).protection;
    let mut file_or_shared = false;
    // Validate the entire source before altering either mapping. Different
    // protections need per-region relocation and are not supported yet.
    let mut at = old_addr;
    while at < old_addr.wrapping_add(old_len) {
        if !region(at, &mut end, info.as_mut_ptr()) {
            return lerr(D_EFAULT);
        }
        let info_protection = (*info.as_ptr()).protection;
        if info_protection != first_protection {
            return lerr(D_EFAULT);
        }
        let mut path = MaybeUninit::<[c_char; PROC_PIDPATHINFO_MAXSIZE]>::uninit();
        // proc_regionfilename can return the NEXT file-backed region (e.g.
        // dyld for a preceding anonymous allocation). Confirm containment.
        let mut file = MaybeUninit::<ProcRegionWithPathInfo>::uninit();
        let mut backed = false;
        if proc_regionfilename(
            getpid(),
            at,
            path.as_mut_ptr().cast::<c_void>(),
            PROC_PIDPATHINFO_MAXSIZE as u32,
        ) > 0
            && proc_pidinfo(
                getpid(),
                PROC_PIDREGIONPATHINFO,
                at,
                file.as_mut_ptr().cast::<c_void>(),
                size_of::<ProcRegionWithPathInfo>() as c_int,
            ) == size_of::<ProcRegionWithPathInfo>() as c_int
        {
            let file = file.as_ptr();
            let base = (*file).prp_prinfo.pri_address;
            backed = base <= at
                && at.wrapping_sub(base) < (*file).prp_prinfo.pri_size
                && (*file).prp_vip.vip_path[0] != 0;
        }
        let info_shared = (*info.as_ptr()).shared;
        if info_shared != 0 || backed {
            file_or_shared = true;
        }
        at = end;
    }
    let prot: c_int = first_protection;
    if (lflags & (L_FIXED | L_DONTUNMAP)) == 0 {
        if new_len <= old_len {
            if new_len < old_len
                && munmap(
                    old_addr.wrapping_add(new_len) as usize as *mut c_void,
                    old_len.wrapping_sub(new_len) as usize,
                ) != 0
            {
                return lerr(errno());
            }
            if new_len < old_len {
                lxrt_mremap_forget_shared(
                    old_addr.wrapping_add(new_len),
                    old_len.wrapping_sub(new_len),
                );
            }
            return old_addr as c_long;
        }
        // Extending a file with anonymous pages would lose backing semantics.
        if !file_or_shared && old_addr <= u64::MAX - new_len {
            let mut tail: MachAddress = old_addr.wrapping_add(old_len);
            let delta = new_len.wrapping_sub(old_len);
            let kr = mach_vm_allocate(task(), &mut tail, delta, VM_FLAGS_FIXED);
            if kr == KERN_SUCCESS {
                let p = mmap(
                    tail as usize as *mut c_void,
                    delta as usize,
                    prot,
                    D_MAP_PRIVATE | D_MAP_ANON | D_MAP_FIXED,
                    -1,
                    0,
                );
                if p == MAP_FAILED {
                    let e = errno();
                    mach_vm_deallocate(task(), tail, delta);
                    return lerr(e);
                }
                return old_addr as c_long;
            }
            if kr != KERN_NO_SPACE {
                return lerr(D_ENOMEM);
            }
        }
        // A shared file mapping may still grow in place (remap_shared_file).
        if (lflags & L_MAYMOVE) == 0 && !file_or_shared {
            return lerr(D_ENOMEM);
        }
    }
    if file_or_shared {
        let r = remap_shared_file(old_addr, old_len, new_len, lflags, new_addr, prot);
        if r != 1 {
            return r;
        }
        fprintf(
            lxrt_trace_stream(),
            b"[lxrt] mremap: file-backed range 0x%llx..0x%llx: not supported yet\n\0"
                .as_ptr()
                .cast::<c_char>(),
            old_addr as c_ulonglong,
            old_addr.wrapping_add(old_len) as c_ulonglong,
        );
        return lerr(D_ENOMEM);
    }

    let mut dest = staging(new_len, (lflags & L_FIXED) != 0, new_addr);
    if dest == MAP_FAILED {
        return lerr(errno());
    }
    let copied = if old_len < new_len { old_len } else { new_len };
    // A private Mach alias lets memcpy read even PROT_NONE/execute-only
    // sources without temporarily changing the source's access permissions.
    let mut snapshot: MachAddress = 0;
    let mut current: c_int = 0;
    let mut maximum: c_int = 0;
    let mut kr = mach_vm_remap(
        task(),
        &mut snapshot,
        copied,
        0,
        VM_FLAGS_ANYWHERE,
        task(),
        old_addr,
        1,
        &mut current,
        &mut maximum,
        VM_INHERIT_NONE,
    );
    if kr != KERN_SUCCESS {
        munmap(dest, new_len as usize);
        return lerr(D_ENOMEM);
    }
    if mprotect(snapshot as usize as *mut c_void, copied as usize, D_PROT_READ) != 0 {
        let e = errno();
        mach_vm_deallocate(task(), snapshot, copied);
        munmap(dest, new_len as usize);
        return lerr(e);
    }
    ptr::copy_nonoverlapping(
        snapshot as usize as *const u8,
        dest.cast::<u8>(),
        copied as usize,
    );
    mach_vm_deallocate(task(), snapshot, copied);
    if mprotect(dest, new_len as usize, prot) != 0 {
        let e = errno();
        munmap(dest, new_len as usize);
        return lerr(e);
    }
    if (lflags & L_FIXED) != 0 {
        // Publish only after allocation/copy/protection succeed. Mach replaces
        // the destination just as MAP_FIXED does, with no unreserved gap --
        // and a W^X range there with it, so out of the table first
        // (dispatch.c's lxrt_wx_moved gives the source's pieces back).
        lxrt_wx_forget(new_addr, new_len);
        let mut target: MachAddress = new_addr;
        // As in C, this second remap overwrites `current`/`maximum`.
        kr = mach_vm_remap(
            task(),
            &mut target,
            new_len,
            0,
            VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
            task(),
            dest as usize as MachAddress,
            0,
            &mut current,
            &mut maximum,
            VM_INHERIT_COPY,
        );
        munmap(dest, new_len as usize);
        if kr != KERN_SUCCESS {
            return lerr(D_ENOMEM);
        }
        dest = target as usize as *mut c_void;
    }
    let rc: c_int;
    if (lflags & L_DONTUNMAP) != 0 {
        // Linux leaves the source mapped with its old protection and faults
        // it back in as zero pages (unless userfaultfd intervenes). A stricter
        // PROT_NONE here scored 21/22 against the Linux reference: a guest
        // that reads the purged source expects zeros, not SIGSEGV. `current`
        // is the protection the mach_vm_remap snapshot reported for the old
        // range, so the fresh anonymous pages get exactly that.
        let mut oldprot = (if current & VM_PROT_READ != 0 { D_PROT_READ } else { 0 })
            | (if current & VM_PROT_WRITE != 0 { D_PROT_WRITE } else { 0 })
            | (if current & VM_PROT_EXECUTE != 0 { D_PROT_EXEC } else { 0 });
        if (oldprot & D_PROT_WRITE) != 0 && (oldprot & D_PROT_EXEC) != 0 {
            oldprot &= !D_PROT_EXEC; // Darwin refuses rwx on a plain page
        }
        rc = if mmap(
            old_addr as usize as *mut c_void,
            old_len as usize,
            oldprot,
            D_MAP_PRIVATE | D_MAP_ANON | D_MAP_FIXED,
            -1,
            0,
        ) == MAP_FAILED
        {
            -1
        } else {
            0
        };
    } else {
        rc = munmap(old_addr as usize as *mut c_void, old_len as usize);
    }
    if rc != 0 {
        let e = errno();
        munmap(dest, new_len as usize);
        return lerr(e);
    }
    // Mach serializes individual VM operations. As with mmap/munmap, callers
    // must serialize concurrent changes to these same address ranges.
    dest as usize as c_long
}
