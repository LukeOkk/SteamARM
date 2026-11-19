// Android identities for lxrun guests: per-process virtual credentials.
// See android_ids.h. Linux references: kernel/sys.c (__sys_setresuid,
// __sys_setreuid, __sys_setuid and the gid twins, setfsuid), kernel/groups.c
// (setgroups: CAP_SETGID, sorted), kernel/capability.c (capget/capset),
// security/commoncap.c (cap_emulate_setxuid, cap_bprm_creds_from_file,
// cap_task_prctl), include/uapi/linux/capability.h and prctl.h.
//
// A direct translation of the C: the same libSystem calls (snprintf, sscanf,
// strtoul, qsort, the xattr calls), a real pthread mutex reinitialised in the
// fork child, no allocation anywhere.

use std::cell::UnsafeCell;
use std::ffi::{c_char, c_int, c_long, c_uint, c_ulong, c_ulonglong, c_void};
use std::mem::{size_of, MaybeUninit};
use std::ptr;
use std::sync::atomic::{AtomicI32, AtomicPtr, AtomicU32, AtomicU64, Ordering};

// Darwin errno values (host side; converted with lxrt_errno_to_linux).
const EPERM: c_int = 1;
const ESRCH: c_int = 3;
const ENOMEM: c_int = 12;
const EACCES: c_int = 13;
const EFAULT: c_int = 14;
const EINVAL: c_int = 22;
const ENOTSUP: c_int = 45;
const ENAMETOOLONG: c_int = 63;
const ENOATTR: c_int = 93;

// Darwin constants (arm64).
const O_RDWR: c_int = 0x0002;
const O_NOFOLLOW: c_int = 0x0100;
const O_CREAT: c_int = 0x0200;
const O_CLOEXEC: c_int = 0x0100_0000;
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const MAP_SHARED: c_int = 1;
const MAP_FAILED: *mut c_void = usize::MAX as *mut c_void;
const AT_FDCWD: c_int = -2;
const F_GETPATH: c_int = 50;
const PATH_MAX: usize = 1024;
const XATTR_NOFOLLOW: c_int = 1;
const PROC_PIDTBSDINFO: c_int = 3;
const S_IFMT: u16 = 0o170000;
const S_IFIFO: u16 = 0o010000;
const S_IFDIR: u16 = 0o040000;
const S_IFLNK: u16 = 0o120000;
const S_IFSOCK: u16 = 0o140000;
const S_IWUSR: c_int = 0o200;
const VM_PROT_READ: c_int = 1;
const VM_PROT_WRITE: c_int = 2;
const VM_REGION_BASIC_INFO_64: c_int = 9;
const KERN_SUCCESS: c_int = 0;
const MACH_PORT_NULL: u32 = 0;

const LXRT_OWNER_XATTR: *const c_char = c"org.steamarm.lxrt.owner".as_ptr();

const CAP_CHOWN: c_int = 0;
const CAP_SETGID: c_int = 6;
const CAP_SETUID: c_int = 7;
const CAP_SETPCAP: c_int = 8;
const CAP_LAST_CAP: c_int = 40; // Linux 5.9+: CAP_CHECKPOINT_RESTORE
const CAP_ALL: u64 = (1u64 << (CAP_LAST_CAP + 1)) - 1;
const SECURE_NOROOT: u32 = 0;
const SECURE_NO_SETUID_FIXUP: u32 = 2;
const SECURE_KEEP_CAPS: u32 = 4;
const SECURE_NO_CAP_AMBIENT_RAISE: u32 = 6;
const MAX_GROUPS: usize = 1024; // Linux allows 65536; Android uses a few dozen

const N: u32 = 0xffff_ffff;

#[repr(C)]
#[derive(Clone, Copy)]
struct Ids {
    r: u32,
    e: u32,
    s: u32,
    fs: u32,
    rg: u32,
    eg: u32,
    sg: u32,
    fsg: u32,
    ngroups: c_int,
    groups: [u32; MAX_GROUPS],
    eff: u64,
    prm: u64,
    inh: u64,
    bnd: u64,
    amb: u64,
    securebits: u32,
    nnp: bool, // PR_SET_NO_NEW_PRIVS: kept across fork and execve
}

// struct lxrt_aids_peer (android_ids.h).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct LxrtAidsPeer {
    ruid: u32,
    euid: u32,
    rgid: u32,
    egid: u32,
    eff: u64,
    prm: u64,
    inh: u64,
}

const _: [(); 40] = [(); size_of::<LxrtAidsPeer>()];

// Darwin's struct stat (arm64, 64-bit inodes).
#[repr(C)]
struct Stat {
    st_dev: i32,
    st_mode: u16,
    st_nlink: u16,
    st_ino: u64,
    st_uid: u32,
    st_gid: u32,
    st_rdev: i32,
    st_atimespec: [i64; 2],
    st_mtimespec: [i64; 2],
    st_ctimespec: [i64; 2],
    st_birthtimespec: [i64; 2],
    st_size: i64,
    st_blocks: i64,
    st_blksize: i32,
    st_flags: u32,
    st_gen: u32,
    st_lspare: i32,
    st_qspare: [i64; 2],
}

const _: [(); 144] = [(); size_of::<Stat>()];

// Darwin's struct proc_bsdinfo (sys/proc_info.h).
#[repr(C)]
struct ProcBsdInfo {
    pbi_flags: u32,
    pbi_status: u32,
    pbi_xstatus: u32,
    pbi_pid: u32,
    pbi_ppid: u32,
    pbi_uid: u32,
    pbi_gid: u32,
    pbi_ruid: u32,
    pbi_rgid: u32,
    pbi_svuid: u32,
    pbi_svgid: u32,
    rfu_1: u32,
    pbi_comm: [c_char; 16],
    pbi_name: [c_char; 32],
    pbi_nfiles: u32,
    pbi_pgid: u32,
    pbi_pjobc: u32,
    e_tdev: u32,
    e_tpgid: u32,
    pbi_nice: i32,
    pbi_start_tvsec: u64,
    pbi_start_tvusec: u64,
}

const _: [(); 136] = [(); size_of::<ProcBsdInfo>()];

// vm_region_basic_info_data_64_t (mach/vm_region.h, #pragma pack(4)).
#[repr(C, packed(4))]
struct VmRegionBasicInfo64 {
    protection: c_int,
    max_protection: c_int,
    inheritance: u32,
    shared: u32,
    reserved: u32,
    offset: u64,
    behavior: c_int,
    user_wired_count: u16,
}

const _: [(); 36] = [(); size_of::<VmRegionBasicInfo64>()];
const VM_REGION_BASIC_INFO_COUNT_64: u32 = (size_of::<VmRegionBasicInfo64>() / 4) as u32;

// Darwin's pthread_mutex_t and PTHREAD_MUTEX_INITIALIZER (arm64).
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}

const _: [(); 64] = [(); size_of::<PthreadMutex>()];

struct GlobalMutex(UnsafeCell<PthreadMutex>);
unsafe impl Sync for GlobalMutex {}

#[link(name = "System")]
unsafe extern "C" {
    static __stderrp: *mut c_void;
    static mach_task_self_: u32;

    fn __error() -> *mut c_int;
    fn snprintf(buffer: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn sscanf(input: *const c_char, format: *const c_char, ...) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn strtoul(input: *const c_char, end: *mut *mut c_char, base: c_int) -> c_ulong;
    fn strtoull(input: *const c_char, end: *mut *mut c_char, base: c_int) -> c_ulonglong;
    fn strlen(string: *const c_char) -> usize;
    fn strcmp(left: *const c_char, right: *const c_char) -> c_int;
    fn strchr(string: *const c_char, character: c_int) -> *mut c_char;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn qsort(
        base: *mut c_void,
        count: usize,
        width: usize,
        compare: extern "C" fn(*const c_void, *const c_void) -> c_int,
    );
    fn getuid() -> u32;
    fn geteuid() -> u32;
    fn getgid() -> u32;
    fn getegid() -> u32;
    fn getpid() -> c_int;
    fn kill(pid: c_int, signal: c_int) -> c_int;
    fn mkdir(path: *const c_char, mode: u16) -> c_int;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn fcntl(fd: c_int, command: c_int, ...) -> c_int;
    fn ftruncate(fd: c_int, length: i64) -> c_int;
    fn stat(path: *const c_char, out: *mut Stat) -> c_int;
    fn lstat(path: *const c_char, out: *mut Stat) -> c_int;
    fn fstat(fd: c_int, out: *mut Stat) -> c_int;
    fn chmod(path: *const c_char, mode: u16) -> c_int;
    fn fchmod(fd: c_int, mode: u16) -> c_int;
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn getxattr(
        path: *const c_char,
        name: *const c_char,
        value: *mut c_void,
        size: usize,
        position: u32,
        options: c_int,
    ) -> isize;
    fn fgetxattr(
        fd: c_int,
        name: *const c_char,
        value: *mut c_void,
        size: usize,
        position: u32,
        options: c_int,
    ) -> isize;
    fn setxattr(
        path: *const c_char,
        name: *const c_char,
        value: *const c_void,
        size: usize,
        position: u32,
        options: c_int,
    ) -> c_int;
    fn fsetxattr(
        fd: c_int,
        name: *const c_char,
        value: *const c_void,
        size: usize,
        position: u32,
        options: c_int,
    ) -> c_int;
    fn removexattr(path: *const c_char, name: *const c_char, options: c_int) -> c_int;
    fn fremovexattr(fd: c_int, name: *const c_char, options: c_int) -> c_int;
    fn proc_pidinfo(
        pid: c_int,
        flavor: c_int,
        arg: u64,
        buffer: *mut c_void,
        buffer_size: c_int,
    ) -> c_int;
    fn mach_vm_region(
        task: u32,
        address: *mut u64,
        size: *mut u64,
        flavor: c_int,
        info: *mut c_int,
        count: *mut u32,
        object: *mut u32,
    ) -> c_int;
    fn mach_port_deallocate(task: u32, name: u32) -> c_int;
    fn pthread_mutex_init(mutex: *mut PthreadMutex, attributes: *const c_void) -> c_int;
    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;

    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_ids_pid() -> c_int;
    fn lxrt_ids_target_pid(guest_pid: c_int) -> c_int;
    fn lxrt_gettid() -> c_int;
    fn lxrt_pathfd_path(fd: c_int) -> *const c_char;
}

static mut G: MaybeUninit<Ids> = MaybeUninit::zeroed();
static G_ON: AtomicI32 = AtomicI32::new(-1); // -1 not read yet
static G_MU: GlobalMutex = GlobalMutex(UnsafeCell::new(PthreadMutex {
    sig: 0x32AA_ABA7,
    opaque: [0; 56],
}));

#[inline(always)]
fn g() -> *mut Ids {
    ptr::addr_of_mut!(G).cast::<Ids>()
}

#[inline(always)]
unsafe fn lock() {
    let _ = pthread_mutex_lock(G_MU.0.get());
}

#[inline(always)]
unsafe fn unlock() {
    let _ = pthread_mutex_unlock(G_MU.0.get());
}

// LERR(e): -(long)lxrt_errno_to_linux(e)
#[inline(always)]
unsafe fn lerr(error: c_int) -> c_long {
    -(lxrt_errno_to_linux(error) as c_long)
}

#[inline(always)]
unsafe fn errno() -> c_int {
    *__error()
}

#[inline(always)]
fn cap_has(set: u64, cap: c_int) -> bool {
    cap >= 0 && cap <= CAP_LAST_CAP && ((set >> cap) & 1) != 0
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_capable(cap: c_int) -> bool {
    lxrt_aids_on() && cap_has((*g()).eff, cap)
}

// ---- the shared table: pid -> virtual ids, for SO_PEERCRED and binder ----
// One slot per pid. `seq` is odd while the owner rewrites the slot; a
// reader takes a copy between two equal even values (a seqlock), so it
// never mixes two updates.
#[repr(C)]
struct Ent {
    seq: AtomicU32,
    pid: AtomicI32,
    r: AtomicU32,
    e: AtomicU32,
    rg: AtomicU32,
    eg: AtomicU32,
    eff: AtomicU64,
    prm: AtomicU64,
    inh: AtomicU64,
    start: AtomicU64,
}

const _: [(); 56] = [(); size_of::<Ent>()];

const TABLE_SLOTS: c_int = 100_000; // Darwin's PID_MAX is 99999
static G_TABLE: AtomicPtr<Ent> = AtomicPtr::new(ptr::null_mut());

unsafe fn table() -> *mut Ent {
    static STATE: AtomicI32 = AtomicI32::new(0); // 0 untried, 1 ready, 2 failed
    let st = STATE.load(Ordering::SeqCst);
    if st == 1 {
        return G_TABLE.load(Ordering::SeqCst);
    }
    if st == 2 {
        return ptr::null_mut();
    }
    let mut path = [0 as c_char; 96];
    let path_ptr = path.as_mut_ptr();
    let _ = snprintf(
        path_ptr,
        path.len(),
        c"/tmp/lxrt-shm-%u".as_ptr(),
        getuid() as c_uint,
    );
    let _ = mkdir(path_ptr, 0o1777);
    let mut ds = MaybeUninit::<Stat>::uninit();
    // The directory is shared with runtime/socket.c's table; it must be ours.
    if lstat(path_ptr, ds.as_mut_ptr()) != 0
        || ((*ds.as_ptr()).st_mode & S_IFMT) != S_IFDIR
        || (*ds.as_ptr()).st_uid != getuid()
    {
        STATE.store(2, Ordering::SeqCst);
        return ptr::null_mut();
    }
    let used = strlen(path_ptr);
    let _ = snprintf(
        path_ptr.add(used),
        path.len() - used,
        c"/android-ids.v2".as_ptr(),
    );
    let fd = open(
        path_ptr,
        O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
        0o600 as c_int,
    );
    let sz = size_of::<Ent>() * TABLE_SLOTS as usize;
    let mut p = MAP_FAILED;
    let mut fs = MaybeUninit::<Stat>::uninit();
    if fd >= 0
        && fstat(fd, fs.as_mut_ptr()) == 0
        && (*fs.as_ptr()).st_uid == getuid()
        && ((*fs.as_ptr()).st_size == sz as i64 || ftruncate(fd, sz as i64) == 0)
    {
        p = mmap(
            ptr::null_mut(),
            sz,
            PROT_READ | PROT_WRITE,
            MAP_SHARED,
            fd,
            0,
        );
    }
    if fd >= 0 {
        let _ = close(fd);
    }
    if p == MAP_FAILED {
        STATE.store(2, Ordering::SeqCst);
        return ptr::null_mut();
    }
    G_TABLE.store(p.cast::<Ent>(), Ordering::SeqCst);
    STATE.store(1, Ordering::SeqCst);
    p.cast::<Ent>()
}

unsafe fn start_time(pid: c_int) -> u64 {
    let mut bi = MaybeUninit::<ProcBsdInfo>::uninit();
    if proc_pidinfo(
        pid,
        PROC_PIDTBSDINFO,
        0,
        bi.as_mut_ptr().cast(),
        size_of::<ProcBsdInfo>() as c_int,
    ) != size_of::<ProcBsdInfo>() as c_int
    {
        return 0;
    }
    let bi = bi.as_ptr();
    (*bi)
        .pbi_start_tvsec
        .wrapping_mul(1_000_000)
        .wrapping_add((*bi).pbi_start_tvusec)
}

unsafe fn publish() {
    let t = table();
    let pid = getpid();
    if t.is_null() || pid <= 0 || pid >= TABLE_SLOTS {
        return;
    }
    let e = &*t.add(pid as usize);
    let st = start_time(pid);
    let g = g();
    let s = e.seq.load(Ordering::SeqCst) & !1u32; // one writer: this process, under g_mu
    e.seq.store(s.wrapping_add(1), Ordering::SeqCst); // odd: being written
    e.pid.store(pid, Ordering::SeqCst);
    e.r.store((*g).r, Ordering::SeqCst);
    e.e.store((*g).e, Ordering::SeqCst);
    e.rg.store((*g).rg, Ordering::SeqCst);
    e.eg.store((*g).eg, Ordering::SeqCst);
    e.eff.store((*g).eff, Ordering::SeqCst);
    e.prm.store((*g).prm, Ordering::SeqCst);
    e.inh.store((*g).inh, Ordering::SeqCst);
    e.start.store(st, Ordering::SeqCst);
    e.seq.store(s.wrapping_add(2), Ordering::SeqCst); // even again, and different
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_lookup(pid: c_int, out: *mut LxrtAidsPeer) -> bool {
    let t = table();
    if t.is_null() || pid <= 0 || pid >= TABLE_SLOTS {
        return false;
    }
    let e = &*t.add(pid as usize);
    let mut p = LxrtAidsPeer {
        ruid: 0,
        euid: 0,
        rgid: 0,
        egid: 0,
        eff: 0,
        prm: 0,
        inh: 0,
    };
    let st;
    let mut tries: c_int = 0;
    loop {
        let s1 = e.seq.load(Ordering::SeqCst);
        if s1 & 1 != 0 {
            // being written: try again shortly
            if tries > 1000 {
                return false;
            }
            tries += 1;
            continue;
        }
        if e.pid.load(Ordering::SeqCst) != pid {
            return false;
        }
        p.ruid = e.r.load(Ordering::SeqCst);
        p.euid = e.e.load(Ordering::SeqCst);
        p.rgid = e.rg.load(Ordering::SeqCst);
        p.egid = e.eg.load(Ordering::SeqCst);
        p.eff = e.eff.load(Ordering::SeqCst);
        p.prm = e.prm.load(Ordering::SeqCst);
        p.inh = e.inh.load(Ordering::SeqCst);
        let started = e.start.load(Ordering::SeqCst);
        if e.seq.load(Ordering::SeqCst) == s1 {
            st = started;
            break;
        }
        if tries > 1000 {
            return false;
        }
        tries += 1;
    }
    if st == 0 || st != start_time(pid) {
        return false; // a later process with the same pid
    }
    if !out.is_null() {
        out.write(p);
    }
    true
}

extern "C" fn after_fork_child() {
    unsafe {
        let _ = pthread_mutex_init(G_MU.0.get(), ptr::null());
        if G_ON.load(Ordering::Relaxed) == 1 {
            publish();
        }
    }
}

// ---- LXRT_ANDROID_IDS ----
unsafe fn parse_u32s(
    mut s: *const c_char,
    end: *const c_char,
    out: *mut u32,
    max: c_int,
    n: *mut c_int,
) -> bool {
    *n = 0;
    while s < end {
        let mut q: *mut c_char = ptr::null_mut();
        if *n >= max {
            return false;
        }
        let v = strtoul(s, &mut q, 10);
        if q.cast_const() == s || v > 0xffff_ffff {
            return false;
        }
        *out.add(*n as usize) = v as u32;
        *n += 1;
        s = q;
        if s < end && *s as u8 == b',' {
            s = s.add(1);
        } else if s < end {
            return false;
        }
    }
    true
}

extern "C" fn cmp_u32(a: *const c_void, b: *const c_void) -> c_int {
    let (x, y) = unsafe { (*a.cast::<u32>(), *b.cast::<u32>()) };
    if x < y {
        -1
    } else {
        (x > y) as c_int
    }
}

unsafe fn parse(v: *const c_char) -> bool {
    let g = g();
    ptr::write_bytes(g, 0, 1);
    (*g).bnd = CAP_ALL;
    if strcmp(v, c"root".as_ptr()) == 0 {
        (*g).eff = CAP_ALL;
        (*g).prm = CAP_ALL;
        return true;
    }
    let mut f: [*const c_char; 5] = [ptr::null(); 5];
    let mut fe: [*const c_char; 5] = [ptr::null(); 5];
    let mut nf: usize = 0;
    let mut p = v;
    while nf < 5 {
        f[nf] = p;
        let c = strchr(p, b':' as c_int).cast_const();
        fe[nf] = if !c.is_null() { c } else { p.add(strlen(p)) };
        nf += 1;
        if c.is_null() {
            break;
        }
        p = c.add(1);
    }
    if nf < 2 {
        return false;
    }
    let mut u = [0u32; 3];
    let mut gg = [0u32; 3];
    let mut nu: c_int = 0;
    let mut ng: c_int = 0;
    if !parse_u32s(f[0], fe[0], u.as_mut_ptr(), 3, &mut nu) || (nu != 1 && nu != 3) {
        return false;
    }
    if !parse_u32s(f[1], fe[1], gg.as_mut_ptr(), 3, &mut ng) || (ng != 1 && ng != 3) {
        return false;
    }
    (*g).r = u[0];
    (*g).e = if nu == 3 { u[1] } else { u[0] };
    (*g).s = if nu == 3 { u[2] } else { u[0] };
    (*g).fs = (*g).e;
    (*g).rg = gg[0];
    (*g).eg = if ng == 3 { gg[1] } else { gg[0] };
    (*g).sg = if ng == 3 { gg[2] } else { gg[0] };
    (*g).fsg = (*g).eg;
    if nf > 2
        && !parse_u32s(
            f[2],
            fe[2],
            ptr::addr_of_mut!((*g).groups).cast::<u32>(),
            MAX_GROUPS as c_int,
            ptr::addr_of_mut!((*g).ngroups),
        )
    {
        return false;
    }
    qsort(
        ptr::addr_of_mut!((*g).groups).cast(),
        (*g).ngroups as usize,
        size_of::<u32>(),
        cmp_u32,
    );
    if nf > 3 && fe[3] > f[3] {
        let mut c = [0u64; 5];
        let mut s = f[3];
        for i in 0..5 {
            let mut q: *mut c_char = ptr::null_mut();
            c[i] = strtoull(s, &mut q, 16);
            if q.cast_const() == s {
                return false;
            }
            s = q;
            if i < 4 {
                if s >= fe[3] || *s as u8 != b',' {
                    return false;
                }
                s = s.add(1);
            }
        }
        (*g).eff = c[0] & CAP_ALL;
        (*g).prm = c[1] & CAP_ALL;
        (*g).inh = c[2] & CAP_ALL;
        (*g).bnd = c[3] & CAP_ALL;
        (*g).amb = c[4] & CAP_ALL;
        if ((*g).eff & !(*g).prm) != 0 || ((*g).amb & !((*g).prm & (*g).inh)) != 0 {
            return false;
        }
    } else if (*g).e == 0 {
        // a root process has every capability
        (*g).eff = CAP_ALL;
        (*g).prm = CAP_ALL;
    }
    if nf > 4 {
        let mut c = f[4];
        while c < fe[4] {
            match *c as u8 {
                b'k' => (*g).securebits |= 1u32 << SECURE_KEEP_CAPS,
                b'n' => (*g).nnp = true,
                b'b' => {
                    // b<hex>: the securebits
                    let mut q: *mut c_char = ptr::null_mut();
                    (*g).securebits |= strtoul(c.add(1), &mut q, 16) as u32;
                    // C: c = q - 1, then the loop's c++.
                    c = q.cast_const();
                    continue;
                }
                _ => return false,
            }
            c = c.add(1);
        }
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_on() -> bool {
    let on = G_ON.load(Ordering::Relaxed);
    if on >= 0 {
        return on == 1;
    }
    lock();
    if G_ON.load(Ordering::Relaxed) < 0 {
        let v = getenv(c"LXRT_ANDROID_IDS".as_ptr()).cast_const();
        let mut on = false;
        if !v.is_null() && *v != 0 {
            if parse(v) {
                on = true;
            } else {
                let _ = fprintf(
                    __stderrp,
                    c"[lxrt] LXRT_ANDROID_IDS=\"%s\" is not ruid[,euid,suid]:rgid[,egid,sgid][:groups[:eff,prm,inh,bnd,amb[:k]]]; Android ids off\n"
                        .as_ptr(),
                    v,
                );
            }
        }
        if on {
            let _ = pthread_atfork(None, None, Some(after_fork_child));
            G_ON.store(1, Ordering::Relaxed);
            publish();
        } else {
            G_ON.store(0, Ordering::Relaxed);
        }
    }
    unlock();
    G_ON.load(Ordering::Relaxed) == 1
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_euid() -> u32 {
    if lxrt_aids_on() {
        (*g()).e
    } else {
        geteuid()
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_egid() -> u32 {
    if lxrt_aids_on() {
        (*g()).eg
    } else {
        getegid()
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_status_ids(
    u: *mut u32,
    gg: *mut u32,
    groups: *mut c_char,
    gn: usize,
) {
    if !groups.is_null() && gn != 0 {
        *groups = 0;
    }
    if !lxrt_aids_on() {
        let uid = getuid();
        *u.add(3) = uid;
        *u.add(2) = uid;
        *u.add(1) = uid;
        *u = uid;
        let gid = getgid();
        *gg.add(3) = gid;
        *gg.add(2) = gid;
        *gg.add(1) = gid;
        *gg = gid;
        return;
    }
    lock();
    let g = g();
    *u = (*g).r;
    *u.add(1) = (*g).e;
    *u.add(2) = (*g).s;
    *u.add(3) = (*g).fs;
    *gg = (*g).rg;
    *gg.add(1) = (*g).eg;
    *gg.add(2) = (*g).sg;
    *gg.add(3) = (*g).fsg;
    let mut o: usize = 0;
    let mut i: c_int = 0;
    while !groups.is_null() && i < (*g).ngroups && o.wrapping_add(12) < gn {
        o = o.wrapping_add(snprintf(
            groups.add(o),
            gn - o,
            c"%u ".as_ptr(),
            ptr::addr_of!((*g).groups).cast::<u32>().add(i as usize).read() as c_uint,
        ) as usize);
        i += 1;
    }
    unlock();
}

// ---- guest memory ----
unsafe fn mem_ok(addr: u64, n: usize, need_write: bool) -> bool {
    if addr == 0 || n == 0 {
        return false;
    }
    let mut a: u64 = addr;
    let end: u64 = addr.wrapping_add(n as u64);
    if end < a {
        return false;
    }
    while a < end {
        let mut ra: u64 = a;
        let mut rs: u64 = 0;
        let mut info = MaybeUninit::<VmRegionBasicInfo64>::uninit();
        let mut cnt: u32 = VM_REGION_BASIC_INFO_COUNT_64;
        let mut obj: u32 = MACH_PORT_NULL;
        if mach_vm_region(
            mach_task_self_,
            &mut ra,
            &mut rs,
            VM_REGION_BASIC_INFO_64,
            info.as_mut_ptr().cast(),
            &mut cnt,
            &mut obj,
        ) != KERN_SUCCESS
        {
            return false;
        }
        if obj != MACH_PORT_NULL {
            let _ = mach_port_deallocate(mach_task_self_, obj);
        }
        let protection = ptr::addr_of!((*info.as_ptr()).protection).read_unaligned();
        if ra > a
            || rs == 0
            || (protection & VM_PROT_READ) == 0
            || (need_write && (protection & VM_PROT_WRITE) == 0)
        {
            return false;
        }
        a = ra.wrapping_add(rs);
    }
    true
}

// ---- Linux's rules ----
// cap_emulate_setxuid: called after every uid change.
unsafe fn setxuid_fixup(or_: u32, oe: u32, os: u32) {
    let g = g();
    if (*g).securebits & (1u32 << SECURE_NO_SETUID_FIXUP) != 0 {
        return;
    }
    if (or_ == 0 || oe == 0 || os == 0) && (*g).r != 0 && (*g).e != 0 && (*g).s != 0 {
        if (*g).securebits & (1u32 << SECURE_KEEP_CAPS) == 0 {
            (*g).eff = 0;
            (*g).prm = 0;
        }
        (*g).amb = 0;
    }
    if oe == 0 && (*g).e != 0 {
        (*g).eff = 0;
    }
    if oe != 0 && (*g).e == 0 {
        (*g).eff = (*g).prm;
    }
}

#[inline(always)]
fn one_of(v: u32, a: u32, b: u32, c: u32) -> bool {
    v == a || v == b || v == c
}

unsafe fn do_setresuid(r: u32, e: u32, s: u32) -> c_long {
    let g = g();
    if !cap_has((*g).eff, CAP_SETUID) {
        if (r != N && !one_of(r, (*g).r, (*g).e, (*g).s))
            || (e != N && !one_of(e, (*g).r, (*g).e, (*g).s))
            || (s != N && !one_of(s, (*g).r, (*g).e, (*g).s))
        {
            return lerr(EPERM);
        }
    }
    let (or_, oe, os) = ((*g).r, (*g).e, (*g).s);
    if r != N {
        (*g).r = r;
    }
    if e != N {
        (*g).e = e;
    }
    if s != N {
        (*g).s = s;
    }
    (*g).fs = (*g).e;
    setxuid_fixup(or_, oe, os);
    0
}

unsafe fn do_setreuid(r: u32, e: u32) -> c_long {
    let g = g();
    let cap = cap_has((*g).eff, CAP_SETUID);
    if r != N && r != (*g).r && r != (*g).e && !cap {
        return lerr(EPERM);
    }
    if e != N && !one_of(e, (*g).r, (*g).e, (*g).s) && !cap {
        return lerr(EPERM);
    }
    let (or_, oe, os) = ((*g).r, (*g).e, (*g).s);
    if r != N {
        (*g).r = r;
    }
    if e != N {
        (*g).e = e;
    }
    if r != N || (e != N && e != or_) {
        (*g).s = (*g).e;
    }
    (*g).fs = (*g).e;
    setxuid_fixup(or_, oe, os);
    0
}

unsafe fn do_setuid(u: u32) -> c_long {
    let g = g();
    let (or_, oe, os) = ((*g).r, (*g).e, (*g).s);
    if cap_has((*g).eff, CAP_SETUID) {
        (*g).s = u;
        (*g).r = u;
    } else if u != (*g).r && u != (*g).s {
        return lerr(EPERM);
    }
    (*g).fs = u;
    (*g).e = u;
    setxuid_fixup(or_, oe, os);
    0
}

unsafe fn do_setresgid(r: u32, e: u32, s: u32) -> c_long {
    let g = g();
    if !cap_has((*g).eff, CAP_SETGID) {
        if (r != N && !one_of(r, (*g).rg, (*g).eg, (*g).sg))
            || (e != N && !one_of(e, (*g).rg, (*g).eg, (*g).sg))
            || (s != N && !one_of(s, (*g).rg, (*g).eg, (*g).sg))
        {
            return lerr(EPERM);
        }
    }
    if r != N {
        (*g).rg = r;
    }
    if e != N {
        (*g).eg = e;
    }
    if s != N {
        (*g).sg = s;
    }
    (*g).fsg = (*g).eg;
    0
}

unsafe fn do_setregid(r: u32, e: u32) -> c_long {
    let g = g();
    let cap = cap_has((*g).eff, CAP_SETGID);
    if r != N && r != (*g).rg && r != (*g).eg && !cap {
        return lerr(EPERM);
    }
    if e != N && !one_of(e, (*g).rg, (*g).eg, (*g).sg) && !cap {
        return lerr(EPERM);
    }
    let org = (*g).rg;
    if r != N {
        (*g).rg = r;
    }
    if e != N {
        (*g).eg = e;
    }
    if r != N || (e != N && e != org) {
        (*g).sg = (*g).eg;
    }
    (*g).fsg = (*g).eg;
    0
}

unsafe fn do_setgid(v: u32) -> c_long {
    let g = g();
    if cap_has((*g).eff, CAP_SETGID) {
        (*g).sg = v;
        (*g).rg = v;
    } else if v != (*g).rg && v != (*g).sg {
        return lerr(EPERM);
    }
    (*g).fsg = v;
    (*g).eg = v;
    0
}

unsafe fn do_cap(set: bool, hdr: u64, data: u64) -> c_long {
    let g = g();
    if !mem_ok(hdr, 8, !set) {
        return lerr(EFAULT);
    }
    let h = hdr as usize as *mut u32;
    let ver = h.read_unaligned();
    let pid = h.add(1).read_unaligned() as i32;
    let words: usize = if ver == 0x1998_0330 {
        1
    } else if ver == 0x2007_1026 || ver == 0x2008_0522 {
        2
    } else {
        0
    };
    if words == 0 {
        if !mem_ok(hdr, 8, true) {
            return lerr(EFAULT);
        }
        h.write_unaligned(0x2008_0522); // Linux answers with its preferred version
        // kernel/capability.c: capget with no data is a version probe (0);
        // capset returns cap_validate_magic's -EINVAL whatever data is.
        return if data != 0 || set { lerr(EINVAL) } else { 0 };
    }
    if pid < 0 {
        return lerr(EINVAL);
    }
    if !set {
        if data == 0 {
            return 0; // a version probe
        }
        if !mem_ok(data, words * 12, true) {
            return lerr(EFAULT);
        }
        let d = data as usize as *mut u32;
        let (mut e, mut p, mut i) = ((*g).eff, (*g).prm, (*g).inh);
        if pid != 0 && pid != lxrt_ids_pid() && pid != lxrt_gettid() {
            let host = lxrt_ids_target_pid(pid);
            if host < 0 || (kill(host, 0) != 0 && errno() == ESRCH) {
                return lerr(ESRCH);
            }
            let mut pe = MaybeUninit::<LxrtAidsPeer>::uninit();
            if lxrt_aids_lookup(host, pe.as_mut_ptr()) {
                let pe = pe.assume_init();
                e = pe.eff;
                p = pe.prm;
                i = pe.inh;
            } else {
                // no Android ids: an unprivileged process
                e = 0;
                p = 0;
                i = 0;
            }
        }
        for w in 0..words {
            d.add(w * 3).write_unaligned((e >> (32 * w)) as u32);
            d.add(w * 3 + 1).write_unaligned((p >> (32 * w)) as u32);
            d.add(w * 3 + 2).write_unaligned((i >> (32 * w)) as u32);
        }
        return 0;
    }
    if pid != 0 && pid != lxrt_ids_pid() && pid != lxrt_gettid() {
        return lerr(EPERM);
    }
    if !mem_ok(data, words * 12, false) {
        return lerr(EFAULT);
    }
    let d = data as usize as *const u32;
    let (mut e, mut p, mut i) = (0u64, 0u64, 0u64);
    for w in 0..words {
        e |= (d.add(w * 3).read_unaligned() as u64) << (32 * w);
        p |= (d.add(w * 3 + 1).read_unaligned() as u64) << (32 * w);
        i |= (d.add(w * 3 + 2).read_unaligned() as u64) << (32 * w);
    }
    e &= CAP_ALL;
    p &= CAP_ALL;
    i &= CAP_ALL;
    if (i & !((*g).inh | (*g).prm)) != 0 && !cap_has((*g).eff, CAP_SETPCAP) {
        return lerr(EPERM);
    }
    if (i & !((*g).inh | (*g).bnd)) != 0 {
        return lerr(EPERM);
    }
    if (p & !(*g).prm) != 0 {
        return lerr(EPERM);
    }
    if (e & !p) != 0 {
        return lerr(EPERM);
    }
    (*g).eff = e;
    (*g).prm = p;
    (*g).inh = i;
    (*g).amb &= p & i;
    0
}

unsafe fn do_prctl(op: u64, a1: u64, a2: u64, a3: u64, a4: u64, ret: *mut c_long) -> bool {
    let g = g();
    let last = CAP_LAST_CAP as u64;
    match op {
        38 => {
            // PR_SET_NO_NEW_PRIVS: one way only
            if a1 != 1 || a2 != 0 || a3 != 0 || a4 != 0 {
                *ret = lerr(EINVAL);
            } else {
                (*g).nnp = true;
                *ret = 0;
            }
            true
        }
        39 => {
            // PR_GET_NO_NEW_PRIVS
            *ret = if a1 != 0 || a2 != 0 || a3 != 0 || a4 != 0 {
                lerr(EINVAL)
            } else {
                (*g).nnp as c_long
            };
            true
        }
        7 => {
            // PR_GET_KEEPCAPS
            *ret = (((*g).securebits >> SECURE_KEEP_CAPS) & 1) as c_long;
            true
        }
        8 => {
            // PR_SET_KEEPCAPS
            if a1 > 1 {
                *ret = lerr(EINVAL);
                return true;
            }
            if (*g).securebits & (1u32 << (SECURE_KEEP_CAPS + 1)) != 0 {
                *ret = lerr(EPERM); // locked
                return true;
            }
            if a1 != 0 {
                (*g).securebits |= 1u32 << SECURE_KEEP_CAPS;
            } else {
                (*g).securebits &= !(1u32 << SECURE_KEEP_CAPS);
            }
            *ret = 0;
            true
        }
        23 => {
            // PR_CAPBSET_READ
            *ret = if a1 > last {
                lerr(EINVAL)
            } else {
                (((*g).bnd >> a1) & 1) as c_long
            };
            true
        }
        24 => {
            // PR_CAPBSET_DROP
            if !cap_has((*g).eff, CAP_SETPCAP) {
                *ret = lerr(EPERM);
            } else if a1 > last {
                *ret = lerr(EINVAL);
            } else {
                (*g).bnd &= !(1u64 << a1);
                *ret = 0;
            }
            true
        }
        27 => {
            // PR_GET_SECUREBITS
            *ret = (*g).securebits as c_long;
            true
        }
        28 => {
            // PR_SET_SECUREBITS
            // security/commoncap.c: a locked bit cannot change, a lock cannot
            // be released, unknown bits and a missing CAP_SETPCAP are EPERM.
            const LOCKS: u32 = 0xaa;
            const BITS: u32 = 0x55;
            let old = (*g).securebits;
            let nw = a1 as u32;
            if (((old & LOCKS) >> 1) & (old ^ nw)) != 0
                || (old & LOCKS & !nw) != 0
                || (a1 & !((LOCKS | BITS) as u64)) != 0
                || !cap_has((*g).eff, CAP_SETPCAP)
            {
                *ret = lerr(EPERM);
            } else {
                (*g).securebits = nw;
                *ret = 0;
            }
            true
        }
        47 => {
            // PR_CAP_AMBIENT
            if a1 == 4 {
                // CLEAR_ALL
                *ret = if a2 != 0 || a3 != 0 || a4 != 0 {
                    lerr(EINVAL)
                } else {
                    (*g).amb = 0;
                    0
                };
                return true;
            }
            if a3 != 0 || a4 != 0 || a2 > last {
                *ret = lerr(EINVAL);
                return true;
            }
            if a1 == 1 {
                *ret = (((*g).amb >> a2) & 1) as c_long;
            } else if a1 == 2 {
                // RAISE
                if !cap_has((*g).prm, a2 as c_int)
                    || !cap_has((*g).inh, a2 as c_int)
                    || ((*g).securebits & (1u32 << SECURE_NO_CAP_AMBIENT_RAISE)) != 0
                {
                    *ret = lerr(EPERM);
                } else {
                    (*g).amb |= 1u64 << a2;
                    *ret = 0;
                }
            } else if a1 == 3 {
                (*g).amb &= !(1u64 << a2);
                *ret = 0;
            } else {
                *ret = lerr(EINVAL);
            }
            true
        }
        _ => false,
    }
}

#[inline(always)]
unsafe fn cstr_or_empty(address: u64) -> *const c_char {
    if address != 0 {
        address as usize as *const c_char
    } else {
        c"".as_ptr()
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_syscall(
    nr: c_long,
    a0: u64,
    a1: u64,
    a2: u64,
    a3: u64,
    a4: u64,
    ret: *mut c_long,
) -> bool {
    if !lxrt_aids_on() {
        return false;
    }
    let mut handled = true;
    lock();
    let g = g();
    let (old_e, old_eg, old_r, old_rg) = ((*g).e, (*g).eg, (*g).r, (*g).rg);
    let (old_eff, old_prm, old_inh) = ((*g).eff, (*g).prm, (*g).inh);
    match nr {
        174 => *ret = (*g).r as c_long,  // getuid
        175 => *ret = (*g).e as c_long,  // geteuid
        176 => *ret = (*g).rg as c_long, // getgid
        177 => *ret = (*g).eg as c_long, // getegid
        148 | 150 => {
            // getresuid, getresgid
            let v: [u32; 3] = if nr == 148 {
                [(*g).r, (*g).e, (*g).s]
            } else {
                [(*g).rg, (*g).eg, (*g).sg]
            };
            let p: [u64; 3] = [a0, a1, a2];
            *ret = 0;
            for i in 0..3 {
                if !mem_ok(p[i], 4, true) {
                    *ret = lerr(EFAULT);
                    break;
                }
                (p[i] as usize as *mut u32).write_unaligned(v[i]);
            }
        }
        147 => *ret = do_setresuid(a0 as u32, a1 as u32, a2 as u32),
        145 => *ret = do_setreuid(a0 as u32, a1 as u32),
        146 => *ret = do_setuid(a0 as u32),
        149 => *ret = do_setresgid(a0 as u32, a1 as u32, a2 as u32),
        143 => *ret = do_setregid(a0 as u32, a1 as u32),
        144 => *ret = do_setgid(a0 as u32),
        151 => {
            // setfsuid: returns the old one
            let u = a0 as u32;
            *ret = (*g).fs as c_long;
            if u != N
                && (cap_has((*g).eff, CAP_SETUID)
                    || u == (*g).r
                    || u == (*g).e
                    || u == (*g).s
                    || u == (*g).fs)
            {
                (*g).fs = u;
            }
        }
        152 => {
            let v = a0 as u32;
            *ret = (*g).fsg as c_long;
            if v != N
                && (cap_has((*g).eff, CAP_SETGID)
                    || v == (*g).rg
                    || v == (*g).eg
                    || v == (*g).sg
                    || v == (*g).fsg)
            {
                (*g).fsg = v;
            }
        }
        158 => 'getgroups: {
            let n = a0 as c_int;
            let have = (*g).ngroups;
            if n < 0 {
                *ret = lerr(EINVAL);
                break 'getgroups;
            }
            if n == 0 {
                *ret = have as c_long;
                break 'getgroups;
            }
            if n < have {
                *ret = lerr(EINVAL);
                break 'getgroups;
            }
            if have != 0 && !mem_ok(a1, (have as usize).wrapping_mul(4), true) {
                *ret = lerr(EFAULT);
                break 'getgroups;
            }
            if have != 0 {
                ptr::copy_nonoverlapping(
                    ptr::addr_of!((*g).groups).cast::<u8>(),
                    a1 as usize as *mut u8,
                    (have as usize).wrapping_mul(4),
                );
            }
            *ret = have as c_long;
        }
        159 => 'setgroups: {
            let n = a0 as i32 as c_long;
            if !cap_has((*g).eff, CAP_SETGID) {
                *ret = lerr(EPERM);
                break 'setgroups;
            }
            if !(0..=65536).contains(&n) {
                *ret = lerr(EINVAL);
                break 'setgroups;
            }
            if n > MAX_GROUPS as c_long {
                *ret = lerr(ENOMEM);
                break 'setgroups;
            }
            if n != 0 && !mem_ok(a1, n as usize * 4, false) {
                *ret = lerr(EFAULT);
                break 'setgroups;
            }
            if n != 0 {
                ptr::copy_nonoverlapping(
                    a1 as usize as *const u8,
                    ptr::addr_of_mut!((*g).groups).cast::<u8>(),
                    n as usize * 4,
                );
            }
            (*g).ngroups = n as c_int;
            qsort(
                ptr::addr_of_mut!((*g).groups).cast(),
                n as usize,
                size_of::<u32>(),
                cmp_u32,
            );
            *ret = 0;
        }
        90 => *ret = do_cap(false, a0, a1),
        91 => *ret = do_cap(true, a0, a1),
        // prctl: the capability options
        167 => handled = do_prctl(a0, a1, a2, a3, a4, ret),
        // Namespaces and mounts. There is one mount view and no other
        // namespaces; zygote makes a private mount namespace and marks "/" a
        // slave before any fork (com_android_internal_os_Zygote.cpp
        // UnmountStorageOnInit, each fatal). A process with CAP_SYS_ADMIN is
        // told that happened: a namespace identical to the one it had is what
        // it gets, and a propagation change has nothing to propagate. A mount
        // that would change what paths show (bind, tmpfs, a new filesystem) is
        // not emulated and fails; umount2 finds no mount point (EINVAL).
        97 => {
            // unshare
            const NS: u64 = 0x0002_0000
                | 0x0200_0000
                | 0x0400_0000
                | 0x0800_0000
                | 0x1000_0000
                | 0x2000_0000
                | 0x4000_0000; // NEWNS..NEWNET
            const PLAIN: u64 = 0x200 | 0x40000; // CLONE_FS, CLONE_SYSVSEM
            // CLONE_FILES would need a descriptor table of the thread's own,
            // which a Darwin thread cannot have (runtime/thread.c refuses a
            // thread without CLONE_FILES for the same reason).
            if a0 & !(NS | PLAIN) != 0 {
                *ret = lerr(EINVAL);
            } else if (a0 & NS & !0x1000_0000u64) != 0 && !cap_has((*g).eff, 21) {
                *ret = lerr(EPERM);
            } else {
                *ret = 0;
            }
        }
        40 => {
            // mount(src, target, type, flags, data)
            const PROP: u64 = (1 << 17) | (1 << 18) | (1 << 19) | (1 << 20); // UNBINDABLE..SHARED
            if !cap_has((*g).eff, 21) {
                *ret = lerr(EPERM);
            } else if a3 & (0x1000 /* MS_BIND */ | 0x20/* MS_REMOUNT */) != 0 {
                handled = false; // dispatch.c: lxrt_aids_mount
            } else if a2 != 0 && strcmp(a2 as usize as *const c_char, c"tmpfs".as_ptr()) == 0 {
                handled = false; // dispatch.c: an empty directory bound there
            } else if (a3 & PROP) != 0
                && (a3 & !(PROP | 0x4000 /* MS_REC */ | 0x8000/* MS_SILENT */)) == 0
            {
                *ret = 0;
            } else {
                if !getenv(c"LXRT_ANDROID_IDS_LOG".as_ptr()).is_null() {
                    let _ = fprintf(
                        __stderrp,
                        c"[lxrt] pid %d: mount(\"%s\", \"%s\", \"%s\", 0x%llx) not emulated -> EPERM\n"
                            .as_ptr(),
                        getpid(),
                        cstr_or_empty(a0),
                        cstr_or_empty(a1),
                        cstr_or_empty(a2),
                        a3 as c_ulonglong,
                    );
                }
                *ret = lerr(EPERM);
            }
        }
        39 => {
            // umount2: dispatch.c (binds made here can go)
            if !cap_has((*g).eff, 21) {
                *ret = lerr(EPERM);
            } else {
                handled = false;
            }
        }
        // setns: there is no other namespace to join
        268 => *ret = lerr(EINVAL),
        _ => handled = false,
    }
    if (*g).e != old_e
        || (*g).eg != old_eg
        || (*g).r != old_r
        || (*g).rg != old_rg
        || (*g).eff != old_eff
        || (*g).prm != old_prm
        || (*g).inh != old_inh
    {
        publish(); // under g_mu: one writer of the slot
    }
    unlock();
    handled
}

// ---- virtual file ownership (android_ids.h) ----
unsafe fn owner_read(
    host: *const c_char,
    fd: c_int,
    nofollow: bool,
    uid: *mut u32,
    gid: *mut u32,
) -> bool {
    let mut buf = [0 as c_char; 32];
    let n = if !host.is_null() {
        getxattr(
            host,
            LXRT_OWNER_XATTR,
            buf.as_mut_ptr().cast(),
            buf.len() - 1,
            0,
            if nofollow { XATTR_NOFOLLOW } else { 0 },
        )
    } else {
        fgetxattr(
            fd,
            LXRT_OWNER_XATTR,
            buf.as_mut_ptr().cast(),
            buf.len() - 1,
            0,
            0,
        )
    };
    if n <= 0 {
        return false;
    }
    *buf.as_mut_ptr().add(n as usize) = 0;
    let mut u: c_uint = 0;
    let mut gg: c_uint = 0;
    if sscanf(
        buf.as_ptr(),
        c"%u:%u".as_ptr(),
        &mut u as *mut c_uint,
        &mut gg as *mut c_uint,
    ) != 2
    {
        return false;
    }
    *uid = u;
    *gid = gg;
    true
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_fix_stat(
    host: *const c_char,
    fd: c_int,
    nofollow: bool,
    st: *mut c_void,
) {
    let st = st.cast::<Stat>();
    if !lxrt_aids_on() || (host.is_null() && fd < 0) {
        return;
    }
    let mut u: u32 = 0;
    let mut gg: u32 = 0;
    if owner_read(host, fd, nofollow, &mut u, &mut gg) {
        (*st).st_uid = u;
        (*st).st_gid = gg;
    } else if (*st).st_uid == getuid() {
        // A file nobody chowned is the Mac user's, i.e. every guest's: the
        // permission checks already count the caller as its owner
        // (lxrt_aids_chown_file), and stat now says so too. It said 501, so a
        // directory an app had just made was not the app's: git's
        // safe.directory check, OpenSSH's key-file check and the like refuse
        // that ("dubious ownership"); Linux reports the creator.
        lock();
        (*st).st_uid = (*g()).fs;
        (*st).st_gid = (*g()).fsg;
        unlock();
    }
}

// A path the xattr calls can take: relative paths joined to the directory
// descriptor's own path (Darwin has no getxattrat).
unsafe fn at_path(
    ddirfd: c_int,
    host: *const c_char,
    buf: *mut c_char,
    n: usize,
) -> *const c_char {
    if host.is_null() || *host as u8 == b'/' || ddirfd == AT_FDCWD {
        return host;
    }
    let pf = lxrt_pathfd_path(ddirfd);
    let mut dir = MaybeUninit::<[c_char; PATH_MAX]>::uninit();
    let dir = dir.as_mut_ptr().cast::<c_char>();
    if !pf.is_null() {
        let _ = snprintf(dir, PATH_MAX, c"%s".as_ptr(), pf);
    } else if fcntl(ddirfd, F_GETPATH, dir) != 0 {
        return ptr::null();
    }
    if snprintf(buf, n, c"%s/%s".as_ptr(), dir.cast_const(), host) >= n as c_int {
        return ptr::null();
    }
    buf
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_fix_stat_at(
    ddirfd: c_int,
    host: *const c_char,
    nofollow: bool,
    st: *mut c_void,
) {
    if !lxrt_aids_on() {
        return;
    }
    let mut buf = MaybeUninit::<[c_char; PATH_MAX]>::uninit();
    let p = at_path(ddirfd, host, buf.as_mut_ptr().cast(), PATH_MAX);
    if !p.is_null() {
        lxrt_aids_fix_stat(p, -1, nofollow, st);
    }
}

unsafe fn owner_write(host: *const c_char, fd: c_int, nofollow: bool, val: *const c_char) -> c_int {
    let xo = if !host.is_null() && nofollow {
        XATTR_NOFOLLOW
    } else {
        0
    };
    if val.is_null() {
        return if !host.is_null() {
            removexattr(host, LXRT_OWNER_XATTR, xo)
        } else {
            fremovexattr(fd, LXRT_OWNER_XATTR, 0)
        };
    }
    if !host.is_null() {
        setxattr(host, LXRT_OWNER_XATTR, val.cast(), strlen(val), 0, xo)
    } else {
        fsetxattr(fd, LXRT_OWNER_XATTR, val.cast(), strlen(val), 0, 0)
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_chown_file(
    host: *const c_char,
    fd: c_int,
    nofollow: bool,
    uid: u32,
    gid: u32,
) -> c_long {
    let mut st = MaybeUninit::<Stat>::uninit();
    let status = if !host.is_null() {
        if nofollow {
            lstat(host, st.as_mut_ptr())
        } else {
            stat(host, st.as_mut_ptr())
        }
    } else {
        fstat(fd, st.as_mut_ptr())
    };
    if status != 0 {
        return lerr(errno());
    }
    let st = st.as_ptr();
    let mode = (*st).st_mode;
    let mut cu = (*st).st_uid;
    let mut cg = (*st).st_gid;
    let recorded = owner_read(host, fd, nofollow, &mut cu, &mut cg);
    lock();
    let g = g();
    let capchown = cap_has((*g).eff, CAP_CHOWN);
    let (fs, fsg) = ((*g).fs, (*g).fsg);
    let mut member = gid == N || gid == fsg;
    let mut i: c_int = 0;
    while i < (*g).ngroups && !member {
        member = ptr::addr_of!((*g).groups).cast::<u32>().add(i as usize).read() == gid;
        i += 1;
    }
    unlock();
    if !recorded {
        // A file nobody chowned is the Mac user's, i.e. every guest's: as
        // before, the caller counts as its owner.
        cu = fs;
        cg = fsg;
    }
    if !capchown {
        if uid != N && (uid != cu || fs != cu) {
            return lerr(EPERM);
        }
        if gid != N && gid != cg && (fs != cu || !member) {
            return lerr(EPERM);
        }
    }
    // A socket or a pipe has an owner on Linux (its sockfs/pipefs inode) and
    // nowhere to record one here: no extended attributes on either. netd's
    // resolver gives every AI_ADDRCONFIG probe socket the asking app's uid
    // (bionic _find_src_addr), and the failure there read as "no address":
    // apt in Termux, "No address associated with hostname" (MEASURED).
    if host.is_null() && ((mode & S_IFMT) == S_IFSOCK || (mode & S_IFMT) == S_IFIFO) {
        return 0;
    }
    let nu = if uid == N { cu } else { uid };
    let ng = if gid == N { cg } else { gid };
    let mut val = [0 as c_char; 32];
    let mut v: *const c_char = ptr::null(); // back to the Mac's own ids: no record
    if nu != getuid() || ng != getgid() {
        let _ = snprintf(
            val.as_mut_ptr(),
            val.len(),
            c"%u:%u".as_ptr(),
            nu as c_uint,
            ng as c_uint,
        );
        v = val.as_ptr();
    }
    if v.is_null() && !recorded {
        return 0;
    }
    let mut r = owner_write(host, fd, nofollow, v);
    if r != 0 && (errno() == EACCES || errno() == EPERM) && (mode & S_IFMT) != S_IFLNK {
        // Darwin wants write permission for an xattr; Linux's chown with
        // CAP_CHOWN does not. Lend the owner write permission for the call.
        let m = (mode & 0o7777) as c_int;
        let lent = if !host.is_null() {
            chmod(host, (m | S_IWUSR) as u16)
        } else {
            fchmod(fd, (m | S_IWUSR) as u16)
        };
        if lent == 0 {
            r = owner_write(host, fd, nofollow, v);
            let e = errno();
            if !host.is_null() {
                let _ = chmod(host, m as u16);
            } else {
                let _ = fchmod(fd, m as u16);
            }
            *__error() = e;
        }
    }
    if r != 0 {
        if v.is_null() && errno() == ENOATTR {
            return 0;
        }
        // A filesystem without extended attributes: the old answer, success
        // with nothing recorded.
        if errno() == ENOTSUP {
            return 0;
        }
        return lerr(errno());
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_chown_at(
    ddirfd: c_int,
    host: *const c_char,
    nofollow: bool,
    uid: u32,
    gid: u32,
) -> c_long {
    let mut buf = MaybeUninit::<[c_char; PATH_MAX]>::uninit();
    let p = at_path(ddirfd, host, buf.as_mut_ptr().cast(), PATH_MAX);
    if p.is_null() {
        return if !host.is_null() {
            lerr(ENAMETOOLONG)
        } else {
            lerr(EFAULT)
        };
    }
    lxrt_aids_chown_file(p, -1, nofollow, uid, gid)
}

const EXEC_ENV_LEN: usize = 64 + MAX_GROUPS * 11 + 128;
static mut EXEC_ENV: [c_char; EXEC_ENV_LEN] = [0; EXEC_ENV_LEN];

#[no_mangle]
pub unsafe extern "C" fn lxrt_aids_exec_env() -> *const c_char {
    if !lxrt_aids_on() {
        return ptr::null();
    }
    let buf = ptr::addr_of_mut!(EXEC_ENV).cast::<c_char>();
    lock();
    let mut n: Ids = g().read();
    unlock();
    // cap_bprm_creds_from_file for a file without capabilities or set-id bits.
    n.fs = n.e;
    n.s = n.e;
    n.fsg = n.eg;
    n.sg = n.eg;
    n.securebits &= !(1u32 << SECURE_KEEP_CAPS);
    let mut prm: u64 = 0;
    let mut effective = false;
    if (n.securebits & (1u32 << SECURE_NOROOT)) == 0 && (n.e == 0 || n.r == 0) {
        prm = n.bnd | n.inh; // handle_privileged_root
        effective = n.e == 0;
    }
    prm |= n.amb;
    n.prm = prm;
    n.eff = if effective { prm } else { n.amb };
    let mut o: c_int = snprintf(
        buf,
        EXEC_ENV_LEN,
        c"LXRT_ANDROID_IDS=%u,%u,%u:%u,%u,%u:".as_ptr(),
        n.r as c_uint,
        n.e as c_uint,
        n.s as c_uint,
        n.rg as c_uint,
        n.eg as c_uint,
        n.sg as c_uint,
    );
    let mut i: c_int = 0;
    while i < n.ngroups && o < EXEC_ENV_LEN as c_int - 16 {
        o += snprintf(
            buf.add(o as usize),
            EXEC_ENV_LEN - o as usize,
            c"%s%u".as_ptr(),
            if i != 0 { c",".as_ptr() } else { c"".as_ptr() },
            n.groups.as_ptr().add(i as usize).read() as c_uint,
        );
        i += 1;
    }
    o += snprintf(
        buf.add(o as usize),
        EXEC_ENV_LEN - o as usize,
        c":%llx,%llx,%llx,%llx,%llx".as_ptr(),
        n.eff as c_ulonglong,
        n.prm as c_ulonglong,
        n.inh as c_ulonglong,
        n.bnd as c_ulonglong,
        n.amb as c_ulonglong,
    );
    if n.nnp || n.securebits != 0 {
        let _ = snprintf(
            buf.add(o as usize),
            EXEC_ENV_LEN - o as usize,
            c":%sb%x".as_ptr(),
            if n.nnp { c"n".as_ptr() } else { c"".as_ptr() },
            n.securebits as c_uint,
        );
    }
    buf
}
