// System V IPC: semaphores and shared memory.
//
// Not a corner case. Valve's own libtier0_s.so imports semget/semop/semctl, so
// this is the base library every process in the Steam stack links against, not
// one component. `ipcs` on the running guest shows five live semaphore arrays
// and a 10 805 760-byte shared segment created by steamwebhelper; semtimedop
// is measured at 131 calls per 10 s from the `steam` process itself.
//
// Wired: dispatch.c routes 190-197 here (LNR_* at dispatch.c:81-82, cases at
// dispatch.c:2859-2903), and this file is in the Makefile's LXRT_SRCS
// (Makefile:35-38). Still missing: the second semtimedop number, 420
// (semtimedop_time64), which has no case and returns -ENOSYS; see sysv_ipc.h.
//
// Darwin has SysV IPC, so most of this is translation rather than emulation.
// The translation is not thin, because five number spaces disagree and four of
// them agree on their low values first:
//
//  1. semctl commands. Linux GETPID=11 GETVAL=12 GETALL=13 GETNCNT=14
//     GETZCNT=15 SETVAL=16 SETALL=17; Darwin GETNCNT=3 GETPID=4 GETVAL=5
//     GETALL=6 GETZCNT=7 SETVAL=8 SETALL=9. IPC_RMID/IPC_SET/IPC_STAT are
//     0/1/2 on both, which is exactly what makes the rest dangerous: the
//     common commands work unmapped. The collision that actually bites is
//     Linux IPC_INFO (3), which is Darwin's GETNCNT (3) -- measured here,
//     semctl(id, 0, 3, arg) returns 0 rather than failing, so an unmapped
//     pass-through answers "describe the system limits" with a wait count.
//
//  2. glibc and musl both OR IPC_64 (0x0100) into the semctl and shmctl
//     command before issuing the syscall, because the kernel uses that bit to
//     pick the 64-bit struct. A guest IPC_STAT therefore arrives here as
//     0x102, and an implementation that switches on the raw value turns every
//     stat into EINVAL.
//
//  3. struct ipc_perm. Linux puts key FIRST and mode is 32-bit; Darwin puts
//     key LAST and mode is 16-bit. Field-by-field below.
//
//  4. semid_ds and shmid_ds: 88 and 112 bytes on Linux against 72 and 76 on
//     Darwin, and Darwin's are #pragma pack(4) so its 8-byte time_t fields sit
//     at 4-aligned offsets. Nothing about these can be memcpy'd.
//
//  5. ipc_perm.mode leaks kernel state on Darwin. Measured on this machine
//     (Darwin 27.0, xnu-13432): a segment created 0600 reads back as 04600
//     (XNU's SHMSEG_ALLOCATED, 0x800 == 04000) and a semaphore set created
//     0600 reads back as 01600 (XNU's SEM_ALLOC, 0x200 == 01000). Neither of
//     those bit positions is spare on Linux, and the earlier note here named
//     the wrong ones: in shm_perm.mode 04000 is SHM_HUGETLB and 01000 is
//     SHM_DEST (guest /usr/include/bits/shm.h:56-58, read rather than
//     remembered). So the extra bits do not read as noise, they read as a
//     different answer. Mode is masked to 0777 in both directions.
//
//     The mask erases one bit Linux really does set: SHM_DEST (01000) goes
//     into shm_perm.mode when a segment is RMID'd while still attached
//     (verified on the guest -- IPC_STAT after IPC_RMID with one attach
//     outstanding reports mode 01600 and nattch 1, and a further shmat still
//     succeeds). Darwin has nothing to restore it from: measured here,
//     shmctl(id, IPC_STAT) on a Darwin segment that has been RMID'd returns
//     EINVAL outright -- the id is rejected before any perm is read -- so
//     SHMSEG_REMOVED (0x400) is never observable through this interface. The
//     bit is therefore synthesised only for the POSIX-backed segments this
//     module keeps its own bookkeeping for; see pshm_stat(). For a
//     kernel-backed segment a guest polling for SHM_DEST after IPC_RMID gets
//     EINVAL from IPC_STAT instead, which is a divergence that cannot be
//     closed from user space.
//
// Two Darwin limits are walls rather than translation problems, and both are
// measured rather than assumed. They are handled where they are hit:
//
//   * kern.sysv.shmmax defaults to 4194304 (4 MiB). Steam asks for 10 805 760.
//     shmget of that size returns EINVAL. See shmmax_bytes() and the POSIX
//     shared memory fallback.
//   * Darwin's semop accepts at most 5 operations in one call. Measured:
//     nsops=5 succeeds, nsops=6 returns E2BIG. Linux's SEMOPM is 500.
//
// And one limit that turned out to be folklore: Darwin's semaphore counts are
// NOT 87 any more. This machine reports kern.sysv.semmni=87381,
// semmns=87381, semmsl=87381, and semget(200 sems) succeeds. What is still
// small is kern.sysv.semume=10, the number of SEM_UNDO entries ONE PROCESS may
// hold; a guest that leaves more than ten undo adjustments outstanding gets
// EINVAL from semop with nothing else to go on. Shared memory is where the
// small numbers live: shmmni=32 segments system-wide and shmseg=8 attaches per
// process, against Linux's 4096 and 4096.
//
// Rust port of the former runtime/sysv_ipc.c. The notes above are that file's,
// kept whole; line references to dispatch.c and the Makefile are as they were
// written there. Exported symbols and their C ABI are those of sysv_ipc.h.

use std::ffi::{c_char, c_int, c_long, c_longlong, c_uint, c_ulonglong, c_void};
use std::mem::{size_of, MaybeUninit};
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

// --------------------------------------------------------- Darwin constants
//
// Host-side values (macOS arm64 SDK), converted with lxrt_errno_to_linux
// before anything reaches the guest.
const EPERM: c_int = 1;
const ENOENT: c_int = 2;
const EINTR: c_int = 4;
const E2BIG: c_int = 7;
const ENOMEM: c_int = 12;
const EFAULT: c_int = 14;
const EEXIST: c_int = 17;
const EINVAL: c_int = 22;
const EMFILE: c_int = 24;
const ENOSPC: c_int = 28;
const EAGAIN: c_int = 35;
const ENOSYS: c_int = 78;

const O_WRONLY: c_int = 0x0001;
const O_RDWR: c_int = 0x0002;
const O_CREAT: c_int = 0x0200;
const O_TRUNC: c_int = 0x0400;
const O_EXCL: c_int = 0x0800;
const O_CLOEXEC: c_int = 0x0100_0000;

const PROT_NONE: c_int = 0;
const PROT_READ: c_int = 1;
const PROT_WRITE: c_int = 2;
const MAP_SHARED: c_int = 0x0001;
const MAP_PRIVATE: c_int = 0x0002;
const MAP_FIXED: c_int = 0x0010;
const MAP_NORESERVE: c_int = 0x0040;
const MAP_ANON: c_int = 0x1000;
const MAP_FAILED: *mut c_void = usize::MAX as *mut c_void;

const IPC_RMID: c_int = 0;
const IPC_SET: c_int = 1;
const IPC_STAT: c_int = 2;
const IPC_NOWAIT: c_int = 0o4000;
const GETNCNT: c_int = 3;
const GETPID: c_int = 4;
const GETVAL: c_int = 5;
const GETALL: c_int = 6;
const GETZCNT: c_int = 7;
const SETVAL: c_int = 8;
const SETALL: c_int = 9;
const SHMLBA: u64 = 16 * 1024; // sys/shm.h on Apple Silicon

const CLOCK_MONOTONIC: c_int = 6;

const KERN_SUCCESS: c_int = 0;
const MACH_PORT_NULL: MachPort = 0;
const VM_PROT_NONE: c_int = 0;
const VM_PROT_READ: c_int = 1;
const VM_PROT_WRITE: c_int = 2;
const VM_REGION_BASIC_INFO_64: c_int = 9;
const VM_REGION_BASIC_INFO_COUNT_64: u32 = 9;
const VM_REGION_EXTENDED_INFO: c_int = 13;
const VM_REGION_EXTENDED_INFO_COUNT: u32 = 9;

const INT32_MAX: c_long = 0x7fff_ffff;

type MachPort = u32;

// ------------------------------------------------------------ Linux constants
//
// IPC_CREAT/IPC_EXCL/IPC_NOWAIT were checked on both sides rather than
// assumed, because the rest of this file is a list of things that looked like
// they matched. They do match: 01000/02000/04000 on Linux (linux/ipc.h) and
// 01000/02000/04000 on Darwin (sys/ipc.h). So do IPC_RMID/IPC_SET/IPC_STAT
// (0/1/2), SEM_UNDO (0x1000 on Linux, 010000 on Darwin -- same number written
// two ways), SHM_RDONLY (010000) and SHM_RND (020000).
const L_IPC_CREAT: c_int = 0o1000;
const L_IPC_EXCL: c_int = 0o2000;
const L_IPC_NOWAIT: c_int = 0o4000;
const L_IPC_64: c_int = 0x0100; // glibc/musl OR this into semctl/shmctl cmd

const L_IPC_RMID: c_int = 0;
const L_IPC_SET: c_int = 1;
const L_IPC_STAT: c_int = 2;
const L_IPC_INFO: c_int = 3; // Darwin's 3 is GETNCNT; must never pass through

const L_GETPID: c_int = 11;
const L_GETVAL: c_int = 12;
const L_GETALL: c_int = 13;
const L_GETNCNT: c_int = 14;
const L_GETZCNT: c_int = 15;
const L_SETVAL: c_int = 16;
const L_SETALL: c_int = 17;
const L_SEM_STAT: c_int = 18;
const L_SEM_INFO: c_int = 19;
const L_SEM_STAT_ANY: c_int = 20;

const L_SHM_LOCK: c_int = 11;
const L_SHM_UNLOCK: c_int = 12;
const L_SHM_STAT: c_int = 13;
const L_SHM_INFO: c_int = 14;
const L_SHM_STAT_ANY: c_int = 15;

const L_SHM_DEST: u32 = 0o1000; // set by Linux in shm_perm.mode after RMID
const L_SHM_RDONLY: c_int = 0o10000;
const L_SHM_RND: c_int = 0o20000;
const L_SHM_REMAP: c_int = 0o40000; // Linux only
const L_SHM_EXEC: c_int = 0o100000; // Linux only

// Linux shmget flags with no Darwin meaning, and which collide with bits
// Darwin does use: SHM_HUGETLB is 04000, the same bit as IPC_NOWAIT, and
// SHM_NORESERVE is 010000, the same bit as Darwin's IPC_M. Passing a guest's
// shmflg through unmasked therefore sets unrelated Darwin flags. Both are
// dropped; macOS has no huge pages for SysV shm and no reservation policy to
// opt out of.
const L_SHM_HUGETLB: c_int = 0o4000;
const L_SHM_NORESERVE: c_int = 0o10000;

const L_SEMOPM: u64 = 500; // Linux max ops per semop; Darwin's is 5
const PERM_MASK: c_int = 0o777;

// ------------------------------------------------------------- Linux structs
//
//   struct ipc64_perm                  struct ipc_perm (Darwin, __ipc_perm_new)
//   off size field                     off size field
//     0    4 key      (int32)           -    -  (key is LAST here)
//     4    4 uid                        0    4 uid
//     8    4 gid                        4    4 gid
//    12    4 cuid                       8    4 cuid
//    16    4 cgid                      12    4 cgid
//    20    4 mode     (uint32)         16    2 mode (mode_t is 16-bit)
//    24    2 seq                       18    2 _seq
//    26    2 __pad2                    20    4 _key (int32)
//    32    8 __unused1
//    40    8 __unused2
//   size 48, align 8                   size 24, align 4
//
//   struct semid64_ds                  struct __semid_ds_new  (pack(4))
//     0   48 sem_perm                   0   24 sem_perm
//    48    8 sem_otime (long)           24    4 sem_base (kernel pointer)
//    56    8 sem_ctime (long)           28    2 sem_nsems (unsigned short!)
//    64    8 sem_nsems (unsigned long)  32    8 sem_otime (time_t)
//    72    8 __unused3                  40    4 sem_pad1
//    80    8 __unused4                  44    8 sem_ctime (time_t)
//                                       52    4 sem_pad2
//                                       56   16 sem_pad3[4]
//   size 88                            size 72
//
//   struct shmid64_ds                  struct __shmid_ds_new  (pack(4))
//     0   48 shm_perm                   0   24 shm_perm
//    48    8 shm_segsz (size_t)        24    8 shm_segsz
//    56    8 shm_atime                 32    4 shm_lpid
//    64    8 shm_dtime                 36    4 shm_cpid
//    72    8 shm_ctime                 40    2 shm_nattch (unsigned short!)
//    80    4 shm_cpid                  44    8 shm_atime
//    84    4 shm_lpid                  52    8 shm_dtime
//    88    8 shm_nattch (unsigned long) 60   8 shm_ctime
//    96    8 __unused4                 68    8 shm_internal (kernel pointer)
//   104    8 __unused5
//   size 112                           size 76
//
// Note the two narrowings that would go unnoticed: Darwin's sem_nsems and
// shm_nattch are 16-bit where Linux's are 64-bit. A set of more than 65535
// semaphores, or more than 65535 attaches, cannot be reported faithfully --
// neither is reachable under Darwin's own limits, so it is a note, not a bug.

#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxIpc64Perm {
    key: i32,
    uid: u32,
    gid: u32,
    cuid: u32,
    cgid: u32,
    mode: u32,
    seq: u16,
    pad2: u16,
    unused1: u64,
    unused2: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxSemid64Ds {
    sem_perm: LinuxIpc64Perm,
    sem_otime: i64,
    sem_ctime: i64,
    sem_nsems: u64,
    unused3: u64,
    unused4: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxShmid64Ds {
    shm_perm: LinuxIpc64Perm,
    shm_segsz: u64,
    shm_atime: i64,
    shm_dtime: i64,
    shm_ctime: i64,
    shm_cpid: i32,
    shm_lpid: i32,
    shm_nattch: u64,
    unused4: u64,
    unused5: u64,
}

// The one structure that really is identical, verified rather than assumed:
// { unsigned short sem_num; short sem_op; short sem_flg; }, 6 bytes, align 2,
// offsets 0/2/4 on both. It is still copied field by field below, because the
// guest buffer must be snapshotted anyway -- validating a pointer and then
// letting the kernel read through it leaves the guest a window to rewrite it.
#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxSembuf {
    sem_num: u16,
    sem_op: i16,
    sem_flg: i16,
}

// Linux's __kernel_timespec: two signed 64-bit fields on aarch64. Darwin's
// struct timespec on arm64 is also { int64 tv_sec; long tv_nsec; }. Same size
// and same layout, but declared separately so the agreement is on the record.
#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxTimespec {
    tv_sec: i64,
    tv_nsec: i64,
}

// ------------------------------------------------------------ Darwin structs

// struct ipc_perm (__ipc_perm_new).
#[repr(C)]
#[derive(Clone, Copy)]
struct IpcPerm {
    uid: u32,
    gid: u32,
    cuid: u32,
    cgid: u32,
    mode: u16,
    seq: u16,
    key: i32,
}

// struct __semid_ds_new, #pragma pack(4).
#[repr(C, packed(4))]
#[derive(Clone, Copy)]
struct SemidDs {
    sem_perm: IpcPerm,
    sem_base: i32,
    sem_nsems: u16,
    sem_otime: i64,
    sem_pad1: i32,
    sem_ctime: i64,
    sem_pad2: i32,
    sem_pad3: [i32; 4],
}

// struct __shmid_ds_new, #pragma pack(4).
#[repr(C, packed(4))]
#[derive(Clone, Copy)]
struct ShmidDs {
    shm_perm: IpcPerm,
    shm_segsz: usize,
    shm_lpid: i32,
    shm_cpid: i32,
    shm_nattch: u16,
    shm_atime: i64,
    shm_dtime: i64,
    shm_ctime: i64,
    shm_internal: *mut c_void,
}

// struct sembuf.
#[repr(C)]
#[derive(Clone, Copy)]
struct Sembuf {
    sem_num: u16,
    sem_op: i16,
    sem_flg: i16,
}

// struct timespec (arm64): { time_t tv_sec; long tv_nsec; }.
#[repr(C)]
#[derive(Clone, Copy)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: c_long,
}

// struct stat (arm64, 64-bit inodes).
#[repr(C)]
struct Stat {
    st_dev: i32,
    st_mode: u16,
    st_nlink: u16,
    st_ino: u64,
    st_uid: u32,
    st_gid: u32,
    st_rdev: i32,
    st_atimespec: Timespec,
    st_mtimespec: Timespec,
    st_ctimespec: Timespec,
    st_birthtimespec: Timespec,
    st_size: i64,
    st_blocks: i64,
    st_blksize: i32,
    st_flags: u32,
    st_gen: u32,
    st_lspare: i32,
    st_qspare: [i64; 2],
}

// struct dirent (64-bit inodes).
#[repr(C)]
struct Dirent {
    d_ino: u64,
    d_seekoff: u64,
    d_reclen: u16,
    d_namlen: u16,
    d_type: u8,
    d_name: [c_char; 1024],
}

// vm_region_basic_info_data_64_t, #pragma pack(4).
#[repr(C, packed(4))]
struct VmRegionBasicInfo64 {
    protection: c_int,
    max_protection: c_int,
    inheritance: c_uint,
    shared: c_int,
    reserved: c_int,
    offset: u64,
    behavior: c_int,
    user_wired_count: u16,
}

// vm_region_extended_info_data_t.
#[repr(C)]
struct VmRegionExtendedInfo {
    protection: c_int,
    user_tag: c_uint,
    pages_resident: c_uint,
    pages_shared_now_private: c_uint,
    pages_swapped_out: c_uint,
    pages_dirtied: c_uint,
    ref_count: c_uint,
    shadow_depth: u16,
    external_pager: u8,
    share_mode: u8,
    pages_reusable: c_uint,
}

// Darwin's pthread_mutex_t and PTHREAD_MUTEX_INITIALIZER (arm64).
#[repr(C)]
struct PthreadMutex {
    sig: c_long,
    opaque: [u8; 56],
}
const PTHREAD_MUTEX_SIG_INIT: c_long = 0x32AA_ABA7;

// Darwin's pthread_once_t and PTHREAD_ONCE_INIT.
#[repr(C)]
struct PthreadOnce {
    sig: c_long,
    opaque: [u8; 8],
}
const PTHREAD_ONCE_SIG_INIT: c_long = 0x30B1_BCBA;

const _: [(); 48] = [(); size_of::<LinuxIpc64Perm>()]; // ipc64_perm is 48 bytes
const _: [(); 88] = [(); size_of::<LinuxSemid64Ds>()]; // semid64_ds is 88 bytes
const _: [(); 112] = [(); size_of::<LinuxShmid64Ds>()]; // shmid64_ds is 112 bytes
const _: [(); 6] = [(); size_of::<LinuxSembuf>()]; // sembuf is 6 bytes
// Darwin and Linux sembuf must stay the same width
const _: [(); size_of::<LinuxSembuf>()] = [(); size_of::<Sembuf>()];
const _: [(); 16] = [(); size_of::<LinuxTimespec>()]; // timespec is 16 bytes
const _: [(); 24] = [(); size_of::<IpcPerm>()];
const _: [(); 72] = [(); size_of::<SemidDs>()];
const _: [(); 76] = [(); size_of::<ShmidDs>()];
const _: [(); 16] = [(); size_of::<Timespec>()];
const _: [(); 144] = [(); size_of::<Stat>()];
const _: [(); 21] = [(); std::mem::offset_of!(Dirent, d_name)];
const _: [(); 36] = [(); size_of::<VmRegionBasicInfo64>()];
const _: [(); 36] = [(); size_of::<VmRegionExtendedInfo>()];
const _: [(); 64] = [(); size_of::<PthreadMutex>()];
const _: [(); 16] = [(); size_of::<PthreadOnce>()];

#[link(name = "System")]
#[allow(clashing_extern_declarations)]
unsafe extern "C" {
    static mach_task_self_: MachPort;
    static __stderrp: *mut c_void;

    fn mach_vm_region(
        task: MachPort,
        address: *mut u64,
        size: *mut u64,
        flavor: c_int,
        info: *mut c_int,
        count: *mut u32,
        object_name: *mut MachPort,
    ) -> c_int;
    fn mach_port_deallocate(task: MachPort, name: MachPort) -> c_int;

    fn __error() -> *mut c_int;
    fn malloc(size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn strtol(s: *const c_char, end: *mut *mut c_char, base: c_int) -> c_long;
    fn time(out: *mut i64) -> i64;
    fn clock_gettime(clock: c_int, ts: *mut Timespec) -> c_int;
    fn nanosleep(request: *const Timespec, remaining: *mut Timespec) -> c_int;
    fn sysctlbyname(
        name: *const c_char,
        oldp: *mut c_void,
        oldlenp: *mut usize,
        newp: *mut c_void,
        newlen: usize,
    ) -> c_int;
    fn getpagesize() -> c_int;
    fn getpid() -> c_int;
    fn getuid() -> u32;
    fn getgid() -> u32;
    fn kill(pid: c_int, signal: c_int) -> c_int;

    // Variadic in C: Darwin arm64 passes variadic arguments on the stack, so
    // these must be declared variadic here too.
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn shm_open(name: *const c_char, flags: c_int, ...) -> c_int;
    fn semctl(semid: c_int, semnum: c_int, cmd: c_int, ...) -> c_int;
    fn snprintf(buffer: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn dprintf(fd: c_int, format: *const c_char, ...) -> c_int;
    fn fprintf(stream: *mut c_void, format: *const c_char, ...) -> c_int;
    fn fscanf(stream: *mut c_void, format: *const c_char, ...) -> c_int;

    fn fflush(stream: *mut c_void) -> c_int;
    fn fopen(path: *const c_char, mode: *const c_char) -> *mut c_void;
    fn fclose(stream: *mut c_void) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn unlink(path: *const c_char) -> c_int;
    fn mkdir(path: *const c_char, mode: u16) -> c_int;
    fn shm_unlink(name: *const c_char) -> c_int;
    fn ftruncate(fd: c_int, length: i64) -> c_int;
    fn fstat(fd: c_int, st: *mut Stat) -> c_int;
    fn readdir(dir: *mut c_void) -> *mut c_void;
    fn mmap(
        address: *mut c_void,
        length: usize,
        protection: c_int,
        flags: c_int,
        fd: c_int,
        offset: i64,
    ) -> *mut c_void;
    fn munmap(address: *mut c_void, length: usize) -> c_int;

    fn semget(key: i32, nsems: c_int, semflg: c_int) -> c_int;
    fn semop(semid: c_int, sops: *mut Sembuf, nsops: usize) -> c_int;
    fn shmget(key: i32, size: usize, shmflg: c_int) -> c_int;
    fn shmat(shmid: c_int, shmaddr: *const c_void, shmflg: c_int) -> *mut c_void;
    fn shmdt(shmaddr: *const c_void) -> c_int;
    fn shmctl(shmid: c_int, cmd: c_int, buf: *mut ShmidDs) -> c_int;

    fn pthread_mutex_lock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_mutex_unlock(mutex: *mut PthreadMutex) -> c_int;
    fn pthread_once(once: *mut PthreadOnce, init: extern "C" fn()) -> c_int;
    fn pthread_atfork(
        prepare: Option<extern "C" fn()>,
        parent: Option<extern "C" fn()>,
        child: Option<extern "C" fn()>,
    ) -> c_int;

    fn lxrt_errno_to_linux(error: c_int) -> c_int;
    fn lxrt_trace_stream() -> *mut c_void;
    fn lxrt_opendir_private(path: *const c_char) -> *mut c_void;
    fn lxrt_closedir_private(dir: *mut c_void);
    fn lxrt_gbase() -> u64;
}

#[inline(always)]
unsafe fn errno() -> c_int {
    *__error()
}

// LERR(e): (-lxrt_errno_to_linux(e))
#[inline(always)]
unsafe fn lerr(error: c_int) -> c_long {
    (-lxrt_errno_to_linux(error)) as c_long
}

// LXRT_ALIGN_DOWN / LXRT_ALIGN_UP from lxrt.h.
#[inline(always)]
const fn align_down(v: u64, a: u64) -> u64 {
    v & !(a.wrapping_sub(1))
}
#[inline(always)]
const fn align_up(v: u64, a: u64) -> u64 {
    align_down(v.wrapping_add(a).wrapping_sub(1), a)
}

// ------------------------------------------------------------- guest pointers
//
// A guest pointer is a register value, nothing more. Handing a bad one to the
// kernel is survivable; dereferencing it here is not, and a SIGBUS inside the
// runtime looks nothing like the EFAULT the guest was owed. Every guest buffer
// is checked against the VM map first. This costs one Mach trap per region
// crossed, which is irrelevant at the measured 13 semop/s and never on a path
// that runs per frame.
unsafe fn mem_ok(addr: u64, len: u64, need: c_int) -> bool {
    if addr == 0 || len == 0 || addr.wrapping_add(len) < addr {
        return false;
    }
    let mut cur = addr;
    let end = addr.wrapping_add(len);
    while cur < end {
        let mut a: u64 = cur;
        let mut sz: u64 = 0;
        let mut info = MaybeUninit::<VmRegionBasicInfo64>::uninit();
        let mut cnt: u32 = VM_REGION_BASIC_INFO_COUNT_64;
        let mut obj: MachPort = MACH_PORT_NULL;
        if mach_vm_region(
            mach_task_self_,
            &mut a,
            &mut sz,
            VM_REGION_BASIC_INFO_64,
            info.as_mut_ptr().cast::<c_int>(),
            &mut cnt,
            &mut obj,
        ) != KERN_SUCCESS
        {
            return false;
        }
        if obj != MACH_PORT_NULL {
            mach_port_deallocate(mach_task_self_, obj);
        }
        // mach_vm_region reports the first region at or ABOVE the address, so
        // a > cur means cur itself is a hole.
        if a > cur || sz == 0 {
            return false;
        }
        let protection = (*info.as_ptr()).protection;
        if (protection & need) != need {
            return false;
        }
        cur = a.wrapping_add(sz);
    }
    true
}

unsafe fn can_read(p: *const c_void, n: u64) -> bool {
    mem_ok(p as usize as u64, n, VM_PROT_READ)
}

unsafe fn can_write(p: *const c_void, n: u64) -> bool {
    mem_ok(p as usize as u64, n, VM_PROT_READ | VM_PROT_WRITE)
}

// Diagnostics that must appear once and never again: these fire on a
// configuration problem the user has to fix outside the process, so repeating
// them per call would bury the run's real output.
//
// The C original was a printf-style variadic function; a macro stands in for
// it here. The arguments are C-variadic values and the format a C string.
macro_rules! warn_once {
    ($said:expr, $fmt:expr $(, $arg:expr)* $(,)?) => {
        if !$said.swap(true, Ordering::SeqCst) {
            fprintf(lxrt_trace_stream(), c"lxrt: ".as_ptr());
            fprintf(lxrt_trace_stream(), $fmt.as_ptr() $(, $arg)*);
            fflush(__stderrp);
        }
    };
}

// ------------------------------------------------------------ perm/ds mapping
//
// The Linux-side structures are filled in place through a pointer to zeroed
// storage (the C memset), never moved afterwards, so their padding reaches the
// guest as zeroes.

unsafe fn perm_to_linux(d: *const IpcPerm, l: *mut LinuxIpc64Perm) {
    ptr::write_bytes(l, 0, 1);
    (*l).key = (*d).key;
    (*l).uid = (*d).uid;
    (*l).gid = (*d).gid;
    (*l).cuid = (*d).cuid;
    (*l).cgid = (*d).cgid;
    // Masked, not copied: see note 5 at the top. Darwin ORs SHMSEG_ALLOCATED
    // (04000, Linux's SHM_HUGETLB position) into a segment's mode and SEM_ALLOC
    // (01000, Linux's SHM_DEST position) into a semaphore set's, so the bits
    // do not read as noise on the Linux side, they read as flags. Note 5 says
    // what this costs and where SHM_DEST is put back.
    (*l).mode = ((*d).mode as c_int & PERM_MASK) as u32;
    (*l).seq = (*d).seq;
}

// IPC_SET only carries uid, gid and mode on Linux; everything else in the
// structure is ignored by the kernel. The Darwin side is read back first so
// the fields Darwin does care about are not zeroed out from under it.
unsafe fn perm_from_linux(l: *const LinuxIpc64Perm, d: *mut IpcPerm) {
    (*d).uid = (*l).uid;
    (*d).gid = (*l).gid;
    (*d).mode = ((*l).mode & PERM_MASK as u32) as u16;
}

unsafe fn semid_to_linux(d: *const SemidDs, l: *mut LinuxSemid64Ds) {
    ptr::write_bytes(l, 0, 1);
    perm_to_linux(ptr::addr_of!((*d).sem_perm), ptr::addr_of_mut!((*l).sem_perm));
    (*l).sem_otime = (*d).sem_otime;
    (*l).sem_ctime = (*d).sem_ctime;
    (*l).sem_nsems = (*d).sem_nsems as u64;
}

unsafe fn shmid_to_linux(d: *const ShmidDs, l: *mut LinuxShmid64Ds) {
    ptr::write_bytes(l, 0, 1);
    perm_to_linux(ptr::addr_of!((*d).shm_perm), ptr::addr_of_mut!((*l).shm_perm));
    (*l).shm_segsz = (*d).shm_segsz as u64;
    (*l).shm_atime = (*d).shm_atime;
    (*l).shm_dtime = (*d).shm_dtime;
    (*l).shm_ctime = (*d).shm_ctime;
    (*l).shm_cpid = (*d).shm_cpid;
    (*l).shm_lpid = (*d).shm_lpid;
    (*l).shm_nattch = (*d).shm_nattch as u64; // 16-bit on Darwin, 64-bit on Linux
}

// =========================================================== semaphores =====

// Darwin's semop rejects more than MAX_SOPS operations. XNU does not export
// the number; it was measured: 5 works, 6 returns E2BIG. Linux allows 500.
const DARWIN_MAX_SOPS: usize = 5;

static G_SAID_SOPS: AtomicBool = AtomicBool::new(false);
static G_SAID_SEMUME: AtomicBool = AtomicBool::new(false);

fn semflg_to_darwin(lflg: c_int) -> c_int {
    // Only the permission bits and IPC_CREAT/IPC_EXCL have meaning here, and
    // all three ranges are identical. Everything else is masked off rather
    // than forwarded, so a future Linux-only flag cannot silently land on a
    // Darwin bit.
    lflg & (PERM_MASK | L_IPC_CREAT | L_IPC_EXCL)
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_semget(key: i32, nsems: c_int, lsemflg: c_int) -> c_long {
    let id = semget(key, nsems, semflg_to_darwin(lsemflg));
    if id < 0 {
        return lerr(errno());
    }
    id as c_long
}

// Copies and validates the guest's sembuf array. `extra_flg` is ORed into
// every entry, which is how semtimedop turns a blocking set into a polled one.
// `*all_nowait` comes back true when EVERY incoming entry already carried
// IPC_NOWAIT, which has to be recorded before the OR above destroys the
// distinction -- see do_semop. Returns 0, or a negated Linux errno.
unsafe fn copy_sops(
    lsops: *const c_void,
    nsops: u64,
    out: *mut Sembuf,
    extra_flg: c_int,
    all_nowait: *mut bool,
) -> c_long {
    if !can_read(lsops, nsops.wrapping_mul(size_of::<LinuxSembuf>() as u64)) {
        return lerr(EFAULT);
    }
    let input = lsops as *const LinuxSembuf;
    let mut all = true;
    let mut i: u64 = 0;
    while i < nsops {
        let entry = ptr::read_unaligned(input.add(i as usize));
        let o = out.add(i as usize);
        (*o).sem_num = entry.sem_num;
        (*o).sem_op = entry.sem_op;
        if (entry.sem_flg as c_int & L_IPC_NOWAIT) == 0 {
            all = false;
        }
        // SEM_UNDO is 0x1000 on Linux and 010000 on Darwin, which is the same
        // number; IPC_NOWAIT is 04000 on both. Nothing else is defined for
        // sem_flg on either side, so the field passes through whole.
        (*o).sem_flg = (entry.sem_flg as c_int | extra_flg) as i16;
        i += 1;
    }
    *all_nowait = all;
    0
}

// Both entry points share this. `deadline` is NULL for a plain blocking semop.
unsafe fn do_semop(
    semid: c_int,
    lsops: *const c_void,
    nsops: u64,
    deadline: *const Timespec,
) -> c_long {
    if nsops == 0 {
        return lerr(EINVAL); // Linux: nsops < 1 is EINVAL
    }
    if nsops > L_SEMOPM {
        return lerr(E2BIG); // Linux: SEMOPM is 500
    }

    if nsops > DARWIN_MAX_SOPS as u64 {
        // No honest way to do this. semop is defined to apply the whole set
        // atomically or none of it, and Darwin will not accept a set this
        // size; splitting it into chunks of five would let another process
        // observe -- and act on -- a half-applied state, which is precisely
        // the guarantee the caller asked for. E2BIG is what Linux itself
        // returns when a set exceeds SEMOPM, so the guest at least sees an
        // errno from the right family, and the note below says why.
        warn_once!(
            G_SAID_SOPS,
            c"semop with %llu operations: Darwin accepts at most %d per call (measured; XNU MAX_SOPS) and splitting the set would break its atomicity. Returning E2BIG.\n",
            nsops as c_ulonglong,
            DARWIN_MAX_SOPS as c_int,
        );
        return lerr(E2BIG);
    }

    let mut sops = MaybeUninit::<[Sembuf; DARWIN_MAX_SOPS]>::uninit();
    let sops = sops.as_mut_ptr().cast::<Sembuf>();
    let mut all_nowait = false;
    let rc = copy_sops(
        lsops,
        nsops,
        sops,
        if !deadline.is_null() { IPC_NOWAIT } else { 0 },
        &mut all_nowait,
    );
    if rc < 0 {
        return rc;
    }

    if !deadline.is_null() && all_nowait {
        // The poll loop below drives itself by ORing IPC_NOWAIT into every
        // operation, which makes a guest's OWN IPC_NOWAIT invisible: "the
        // caller asked not to block" and "the kernel says retry" become the
        // same EAGAIN, and a probe that Linux answers instantly sat in the
        // backoff for the whole timeout. Measured before this branch existed:
        // sembuf{sem_op=-1, sem_flg=IPC_NOWAIT} on a set at 0 with a 2 s
        // timeout returned -EAGAIN after 2.002 s; the same call on the Linux
        // guest returns -EAGAIN after 0.000 s. When every operation already
        // carries IPC_NOWAIT the answer needs no loop at all -- one semop,
        // whatever it says -- and the timeout is irrelevant because Linux
        // never waits either.
        //
        // A MIXED set (some operations with IPC_NOWAIT, some without) still
        // goes through the loop. Darwin's semop reports only that the set
        // could not proceed, never which operation blocked, so there is no way
        // to tell the "fail now" case from the "wait" case; looping keeps the
        // errno right and only the latency wrong, which is the smaller lie.
        if semop(semid, sops, nsops as usize) != 0 {
            return lerr(errno());
        }
        return 0;
    }

    if deadline.is_null() {
        if semop(semid, sops, nsops as usize) != 0 {
            let e = errno();
            if e == EINVAL {
                warn_once!(
                    G_SAID_SEMUME,
                    c"semop returned EINVAL. If the set used SEM_UNDO, note kern.sysv.semume is 10 on macOS against Linux's 500: only ten undo entries per process.\n",
                );
            }
            return lerr(e);
        }
        return 0;
    }

    // Darwin has no semtimedop. The emulation is semop with IPC_NOWAIT in a
    // retry loop against a monotonic deadline, so the wakeup latency is the
    // poll interval rather than immediate: a semaphore released while this
    // thread is asleep is not seen until the next probe. The backoff starts at
    // 100 us and doubles to 5 ms, which at the measured 131 semtimedop per
    // 10 s costs nothing and keeps a long wait off the CPU. It is a real
    // semantic loss and not a papered-over one: a Linux waiter is woken in
    // FIFO order the instant the operation can proceed, and this one is not,
    // so a heavily contended set can starve a waiter that a real kernel would
    // have served.
    //
    // Interruption survives, and that matters more than the latency: the sleep
    // is a nanosleep, so a signal delivered to this thread ends it with EINTR,
    // which is what Linux's semtimedop returns in the same situation.
    let mut poll_ns: u64 = 100_000; // 100 us
    const POLL_CAP: u64 = 5_000_000; // 5 ms
    loop {
        if semop(semid, sops, nsops as usize) == 0 {
            return 0;
        }
        let e = errno();
        if e != EAGAIN {
            return lerr(e); // a real failure, not "would block"
        }

        let mut now = Timespec { tv_sec: 0, tv_nsec: 0 };
        clock_gettime(CLOCK_MONOTONIC, &mut now);
        let left: i64 = (*deadline)
            .tv_sec
            .wrapping_sub(now.tv_sec)
            .wrapping_mul(1_000_000_000)
            .wrapping_add((*deadline).tv_nsec.wrapping_sub(now.tv_nsec));
        if left <= 0 {
            return lerr(EAGAIN); // Linux reports a timeout as EAGAIN
        }

        let mut nap = poll_ns;
        if nap as i64 > left {
            nap = left as u64;
        }
        let ts = Timespec {
            tv_sec: (nap / 1_000_000_000) as i64,
            tv_nsec: (nap % 1_000_000_000) as c_long,
        };
        if nanosleep(&ts, ptr::null_mut()) != 0 && errno() == EINTR {
            return lerr(EINTR);
        }
        poll_ns = if poll_ns * 2 > POLL_CAP { POLL_CAP } else { poll_ns * 2 };
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_semop(semid: c_int, lsops: *const c_void, nsops: u64) -> c_long {
    do_semop(semid, lsops, nsops, ptr::null())
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_semtimedop(
    semid: c_int,
    lsops: *const c_void,
    nsops: u64,
    ltimeout: *const c_void,
) -> c_long {
    if ltimeout.is_null() {
        return do_semop(semid, lsops, nsops, ptr::null());
    }
    if !can_read(ltimeout, size_of::<LinuxTimespec>() as u64) {
        return lerr(EFAULT);
    }

    let mut t = ptr::read_unaligned(ltimeout as *const LinuxTimespec);
    if t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1_000_000_000 {
        return lerr(EINVAL);
    }

    // Linux measures the timeout against CLOCK_MONOTONIC, so this does too:
    // a wall-clock jump during a wait must not shorten or extend it.
    let mut now = Timespec { tv_sec: 0, tv_nsec: 0 };
    clock_gettime(CLOCK_MONOTONIC, &mut now);

    // t.tv_sec is a guest-controlled int64 and only its sign has been checked.
    // now.tv_sec + t.tv_sec is signed overflow -- undefined behaviour on a
    // value the guest picks -- and it does not merely wrap harmlessly:
    // measured before this clamp, tv_sec = INT64_MAX with tv_nsec = 999999999
    // landed the deadline in the PAST, so the first probe failed, `left <= 0`
    // fired, and a request for an effectively infinite wait came back -EAGAIN
    // after 0.000 s. Linux does not reject the value either -- verified on the
    // guest, semtimedop with tv_sec = INT64_MAX is accepted and simply waits
    // -- so the fix is to clamp, not to fail. LXRT_SEMOP_MAX_WAIT is ~3.2
    // years, longer than any process this runtime will host.
    const LXRT_SEMOP_MAX_WAIT: i64 = 100_000_000;
    if t.tv_sec > LXRT_SEMOP_MAX_WAIT || t.tv_sec > (i64::MAX / 2).wrapping_sub(now.tv_sec) {
        t.tv_sec = LXRT_SEMOP_MAX_WAIT;
    }

    let mut deadline = Timespec {
        tv_sec: now.tv_sec.wrapping_add(t.tv_sec),
        tv_nsec: now.tv_nsec.wrapping_add(t.tv_nsec as c_long),
    };
    if deadline.tv_nsec >= 1_000_000_000 {
        deadline.tv_nsec -= 1_000_000_000;
        deadline.tv_sec = deadline.tv_sec.wrapping_add(1);
    }
    do_semop(semid, lsops, nsops, &deadline)
}

// How many semaphores the set holds. Needed to bound GETALL/SETALL, which
// carry an unbounded guest array whose length is implied by the set, not by an
// argument.
unsafe fn sem_count(semid: c_int, out: *mut u64) -> c_long {
    let mut ds = MaybeUninit::<SemidDs>::zeroed();
    // semun_t u; u.buf = &ds;
    if semctl(semid, 0, IPC_STAT, ds.as_mut_ptr()) != 0 {
        return lerr(errno());
    }
    *out = (*ds.as_ptr()).sem_nsems as u64;
    0
}

// One of the scalar getters below: the value itself, or -1 with errno.
#[inline(always)]
unsafe fn sem_scalar(semid: c_int, semnum: c_int, dcmd: c_int) -> c_long {
    let v = semctl(semid, semnum, dcmd);
    if v < 0 {
        lerr(errno())
    } else {
        v as c_long
    }
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_semctl(
    semid: c_int,
    semnum: c_int,
    lcmd: c_int,
    arg: u64,
) -> c_long {
    let cmd = lcmd & !L_IPC_64; // see note 2 at the top

    match cmd {
        L_IPC_RMID => {
            if semctl(semid, semnum, IPC_RMID) != 0 {
                lerr(errno())
            } else {
                0
            }
        }

        L_IPC_STAT => {
            let guest = arg as usize as *mut c_void;
            if !can_write(guest, size_of::<LinuxSemid64Ds>() as u64) {
                return lerr(EFAULT);
            }
            let mut ds = MaybeUninit::<SemidDs>::zeroed();
            // semun_t u; u.buf = &ds;
            if semctl(semid, semnum, IPC_STAT, ds.as_mut_ptr()) != 0 {
                return lerr(errno());
            }
            let mut l = MaybeUninit::<LinuxSemid64Ds>::uninit();
            semid_to_linux(ds.as_ptr(), l.as_mut_ptr());
            ptr::copy_nonoverlapping(
                l.as_ptr().cast::<u8>(),
                guest.cast::<u8>(),
                size_of::<LinuxSemid64Ds>(),
            );
            0
        }

        L_IPC_SET => {
            let guest = arg as usize as *const c_void;
            if !can_read(guest, size_of::<LinuxSemid64Ds>() as u64) {
                return lerr(EFAULT);
            }
            let mut l = MaybeUninit::<LinuxSemid64Ds>::uninit();
            ptr::copy_nonoverlapping(
                guest.cast::<u8>(),
                l.as_mut_ptr().cast::<u8>(),
                size_of::<LinuxSemid64Ds>(),
            );
            let mut ds = MaybeUninit::<SemidDs>::zeroed();
            // semun_t u; u.buf = &ds;
            if semctl(semid, semnum, IPC_STAT, ds.as_mut_ptr()) != 0 {
                return lerr(errno());
            }
            perm_from_linux(
                ptr::addr_of!((*l.as_ptr()).sem_perm),
                ptr::addr_of_mut!((*ds.as_mut_ptr()).sem_perm),
            );
            if semctl(semid, semnum, IPC_SET, ds.as_mut_ptr()) != 0 {
                lerr(errno())
            } else {
                0
            }
        }

        // The scalar getters. The number changes, the meaning does not.
        // Each of these returns the value itself, so -1 is the only failure
        // signal and the call must be made exactly once -- a second call to read
        // the value would also reset errno.
        L_GETPID => sem_scalar(semid, semnum, GETPID),
        L_GETVAL => sem_scalar(semid, semnum, GETVAL),
        L_GETNCNT => sem_scalar(semid, semnum, GETNCNT),
        L_GETZCNT => sem_scalar(semid, semnum, GETZCNT),

        L_SETVAL => {
            // The union is passed by value in one register. glibc stores an int
            // into it and hands the whole word to the kernel, so the value is the
            // low 32 bits and the top half is whatever was in the union before.
            //
            // semun_t u; u.val = (int)(uint32_t)arg; the union travels as one
            // 8-byte variadic slot.
            let u: u64 = arg as u32 as u64;
            if semctl(semid, semnum, SETVAL, u) != 0 {
                lerr(errno())
            } else {
                0
            }
        }

        L_GETALL | L_SETALL => {
            let mut n: u64 = 0;
            let rc = sem_count(semid, &mut n);
            if rc < 0 {
                return rc;
            }
            if n == 0 {
                return lerr(EINVAL);
            }
            let guest = arg as usize as *mut c_void;
            let bytes: u64 = n * size_of::<u16>() as u64;
            // unsigned short on both sides, so the element type needs no
            // conversion -- only the bounce through validated storage.
            let mut small = MaybeUninit::<[u16; 256]>::uninit();
            let small_ptr = small.as_mut_ptr().cast::<u16>();
            let mut buf = small_ptr;
            if n > 256 {
                buf = malloc(bytes as usize).cast::<u16>();
                if buf.is_null() {
                    return lerr(ENOMEM);
                }
            }
            // semun_t u; u.array = buf;
            let out: c_long;
            if cmd == L_GETALL {
                if !can_write(guest, bytes) {
                    out = lerr(EFAULT);
                } else if semctl(semid, 0, GETALL, buf) != 0 {
                    out = lerr(errno());
                } else {
                    ptr::copy_nonoverlapping(buf.cast::<u8>(), guest.cast::<u8>(), bytes as usize);
                    out = 0;
                }
            } else if !can_read(guest, bytes) {
                out = lerr(EFAULT);
            } else {
                ptr::copy_nonoverlapping(guest.cast::<u8>(), buf.cast::<u8>(), bytes as usize);
                out = if semctl(semid, 0, SETALL, buf) != 0 {
                    lerr(errno())
                } else {
                    0
                };
            }
            if buf != small_ptr {
                free(buf.cast::<c_void>());
            }
            out
        }

        L_IPC_INFO | L_SEM_STAT | L_SEM_INFO | L_SEM_STAT_ANY => {
            // These exist so `ipcs` and `ipcrm` can enumerate the system. Darwin
            // has no equivalent -- its semctl has no command for them, and worse,
            // Linux's IPC_INFO (3) IS Darwin's GETNCNT (3), so forwarding the
            // number would return a plausible small integer instead of failing.
            // Nothing in the measured Steam stack calls these; a guest `ipcs`
            // will report nothing rather than report nonsense.
            lerr(ENOSYS)
        }

        _ => lerr(EINVAL),
    }
}

// ======================================================== shared memory =====

static G_SAID_SHMMAX: AtomicBool = AtomicBool::new(false);
static G_SAID_SHMMNI: AtomicBool = AtomicBool::new(false);
static G_SAID_SHMFLAGS: AtomicBool = AtomicBool::new(false);

// kern.sysv.shmmax is the per-segment cap and defaults to 4194304 on macOS.
// It is a CTLTYPE_QUAD, so it reads back as 8 bytes; read it once.
unsafe fn shmmax_bytes() -> u64 {
    static CACHED: AtomicU64 = AtomicU64::new(0);
    let v = CACHED.load(Ordering::SeqCst);
    if v != 0 {
        return v;
    }
    // Declared CTLTYPE_QUAD in XNU and measured returning 8 bytes here, but
    // the buffer is zeroed first so a kernel that answers with 4 still lands
    // the value correctly on this little-endian target.
    let mut q: u64 = 0;
    let mut len: usize = size_of::<u64>();
    if sysctlbyname(
        c"kern.sysv.shmmax".as_ptr(),
        ptr::addr_of_mut!(q).cast::<c_void>(),
        &mut len,
        ptr::null_mut(),
        0,
    ) != 0
        || q == 0
    {
        q = 4_194_304; // the documented macOS default
    }
    CACHED.store(q, Ordering::SeqCst);
    q
}

// ------------------------------------------------- POSIX shared memory path
//
// Steam's 10 805 760-byte segment cannot be served by Darwin's SysV shm at
// all: kern.sysv.shmmax is 4 MiB and raising it needs `sysctl -w` as root,
// applied at every boot, which is not something a runtime can arrange for
// itself. Refusing outright would mean steamwebhelper never starts on a stock
// machine, so oversized requests are served from POSIX shared memory instead,
// with the name derived from the guest's key so that independent processes
// asking for the same key land on the same object.
//
// Measured, on this machine: shm_open + one ftruncate(10 805 760) + mmap
// succeeds, a second process opening the same name sees the same bytes, and
// the object survives shm_unlink for anyone already mapped.
//
// What is given up, stated rather than hidden:
//
//   * Lifetime. Linux's IPC_RMID keeps a still-attached segment addressable,
//     marks it SHM_DEST and frees it at the last detach; that shape IS
//     reproduced here (see pshm_destroy). What is not reproduced is what
//     happens to a THIRD process: shm_unlink drops the name at once, so a
//     shmget for that key afterwards creates a new empty object, and a process
//     that still holds the old object open never learns it was removed.
//   * Permissions. SysV checks ipc_perm on every operation; POSIX shm checks
//     the file mode once, at open. Close, not identical. The mode is also
//     frozen at creation: Darwin's fchmod on a shm descriptor returns EINVAL
//     (measured), so IPC_SET records the new mode for IPC_STAT to report and
//     the object itself keeps the one it was born with. umask does NOT narrow
//     it -- measured, shm_open(..., 0666) under umask 077 still creates 0666 --
//     so a guest asking for 0666 gets 0666.
//   * shm_nattch counts only this runtime's attaches. There is no system-wide
//     attach count for a POSIX object, so a guest using nattch to decide when
//     to clean up will see a number that is too low.
//   * The identifier is process-local. A guest that creates a segment and
//     sends the raw shmid to an unrelated process leaves that process with a
//     number this table cannot resolve; it gets EINVAL, which is what Linux
//     returns for a stale id. Passing the KEY works, because the name is
//     derived from it, and that is how Steam's components find each other.
//   * Size. Darwin rounds the object up to a 16 KiB page -- measured,
//     ftruncate(10 805 760) fstats back as 10 813 440 -- and the exact request
//     is not recoverable from the object afterwards. So a segment CREATED here
//     reports what the guest asked for, and one ADOPTED by another process
//     reports the rounded size. That is a disagreement between processes as
//     well as with Linux, and it is the deliberate half of the trade: the
//     alternative, reporting the rounded size everywhere, would be consistent
//     but wrong for the single-process case as well. Nothing breaks either
//     way, since both numbers describe memory that is really mapped; a guest
//     that compares shm_segsz across processes for equality would notice.
//
// PSHMNAMLEN on Darwin is 31 characters including the leading slash; a 34
// character name was measured returning ENAMETOOLONG. The formats below are
// 21 and 22 characters.

// The table is sized to Linux's own defaults -- SHMMNI 4096 segments and
// SHMSEG 4096 attaches per process -- rather than to Darwin's kern.sysv
// numbers. The earlier caps of 32 and 64 were justified by shmmni=32, but a
// POSIX shm object is not a SysV segment and is not subject to shmmni at all;
// the real ceiling on this path is RLIMIT_NOFILE, because each live segment
// holds one descriptor. Both arrays are BSS, so the unused tail costs nothing
// until it is touched, and g_seg_high keeps the linear scans proportional to
// what is actually in use rather than to the size of the array.
const MAX_PSHM: usize = 4096;
const MAX_ATTACH: usize = 4096;

// The top of the shmid index space; see pshm_mint_id for why ids are built
// downwards from here.
const PSHM_IX_TOP: u64 = 0xffff;

#[repr(C)]
#[derive(Clone, Copy)]
struct Pshm {
    used: bool,
    private_key: bool, // created with IPC_PRIVATE: not findable by key
    destroyed: bool,   // IPC_RMID seen; alive only for its attaches
    key: i32,
    id: c_int,
    fd: c_int,
    size: u64, // what the guest asked for, not the rounding
    mode: u32,
    cpid: i32,
    lpid: i32,
    nattch: u64, // this process only; see above
    ctime: i64,
    atime: i64,
    dtime: i64,
}

impl Pshm {
    const ZERO: Pshm = Pshm {
        used: false,
        private_key: false,
        destroyed: false,
        key: 0,
        id: 0,
        fd: 0,
        size: 0,
        mode: 0,
        cpid: 0,
        lpid: 0,
        nattch: 0,
        ctime: 0,
        atime: 0,
        dtime: 0,
    };
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Attach {
    used: bool,
    addr: u64,
    len: u64,
    id: c_int, // synthetic id this mapping belongs to
}

impl Attach {
    const ZERO: Attach = Attach {
        used: false,
        addr: 0,
        len: 0,
        id: 0,
    };
}

static mut G_SEG: [Pshm; MAX_PSHM] = [Pshm::ZERO; MAX_PSHM];
static mut G_ATT: [Attach; MAX_ATTACH] = [Attach::ZERO; MAX_ATTACH];
static mut G_SEG_HIGH: c_int = 0; // 1 + highest slot ever allocated
static mut G_ATT_HIGH: c_int = 0;
static mut G_PSHM_GEN: u32 = 1; // sequence half of a synthetic id
static mut G_LOCK: PthreadMutex = PthreadMutex {
    sig: PTHREAD_MUTEX_SIG_INIT,
    opaque: [0; 56],
};
// Fork safety is ipc_atfork_install below, registered on first use. Adding
// LXRT_FORK_SAFE here as well registered g_lock twice: the second prepare
// handler blocked on the lock the first had just taken, and the Steam client
// hung in fork() once X11 MIT-SHM had touched this module.

#[inline(always)]
unsafe fn seg(i: usize) -> *mut Pshm {
    ptr::addr_of_mut!(G_SEG).cast::<Pshm>().add(i)
}

#[inline(always)]
unsafe fn att(i: usize) -> *mut Attach {
    ptr::addr_of_mut!(G_ATT).cast::<Attach>().add(i)
}

// Fork safety. The guest's fork() is a real Darwin fork() (runtime/process.c),
// so a child starts with exactly one thread and with whatever state g_lock was
// in at the moment of the call. Without a handler, a fork racing any thread
// that held g_lock left the child holding a locked mutex no one would ever
// release, and the child's first shm call would block forever.
//
// What the child inherits is deliberate rather than discarded: the descriptors
// in g_seg and the MAP_SHARED mappings in g_att both survive fork and are
// still valid in the child, and the child really does hold those attaches, so
// zeroing nattch or clearing the table would make its own bookkeeping wrong.
// The counts stay. What is knowingly left approximate is cpid/lpid, which
// still name the parent until the child's next attach, and the fact that
// parent and child now each count the same mapping in their own nattch -- both
// already follow from nattch being process-local, which the notes above say.
extern "C" fn ipc_atfork_prepare() {
    unsafe {
        pthread_mutex_lock(ptr::addr_of_mut!(G_LOCK));
    }
}
extern "C" fn ipc_atfork_parent() {
    unsafe {
        pthread_mutex_unlock(ptr::addr_of_mut!(G_LOCK));
    }
}
extern "C" fn ipc_atfork_child() {
    // The one thread in the child is the owner of g_lock here -- the prepare
    // handler above took it in the parent -- so unlocking it is defined,
    // unlike re-initialising a held mutex.
    unsafe {
        pthread_mutex_unlock(ptr::addr_of_mut!(G_LOCK));
    }
}

static mut G_ATFORK_ONCE: PthreadOnce = PthreadOnce {
    sig: PTHREAD_ONCE_SIG_INIT,
    opaque: [0; 8],
};

extern "C" fn ipc_atfork_install() {
    unsafe {
        pthread_atfork(
            Some(ipc_atfork_prepare),
            Some(ipc_atfork_parent),
            Some(ipc_atfork_child),
        );
    }
}

unsafe fn ipc_lock() {
    pthread_once(ptr::addr_of_mut!(G_ATFORK_ONCE), ipc_atfork_install);
    pthread_mutex_lock(ptr::addr_of_mut!(G_LOCK));
}

unsafe fn ipc_unlock() {
    pthread_mutex_unlock(ptr::addr_of_mut!(G_LOCK));
}

unsafe fn pshm_name(out: *mut c_char, cap: usize, key: i32, private_key: bool) {
    if private_key {
        // IPC_PRIVATE has no key to hash, so the name only has to be unique.
        // Only this process and its forks can reach it, which is what
        // IPC_PRIVATE means anyway.
        static SEQ: AtomicU32 = AtomicU32::new(0);
        snprintf(
            out,
            cap,
            c"/lxrtp%08x%08x".as_ptr(),
            getpid() as c_uint,
            SEQ.fetch_add(1, Ordering::SeqCst) as c_uint,
        );
    } else {
        // The uid is folded in so two users on one machine do not collide on a
        // key, matching SysV's per-uid permission check closely enough.
        snprintf(
            out,
            cap,
            c"/lxrt%08x%08x".as_ptr(),
            key as u32 as c_uint,
            getuid() as c_uint,
        );
    }
}

// kern.sysv.shmmni is not only the number of kernel segments, it is the size
// of the kernel's shmid INDEX space. XNU builds a shmid as
// IXSEQ_TO_IPCID(ix, perm) = (perm._seq << 16) | (ix & 0xffff), and
// shm_find_segment_by_shmid rejects up front any id whose ix falls outside
// 0..shmmni-1. Read as a quad: measured here returning 8 bytes, value 32, and
// holding 32 segments at once showed the index range the kernel actually used
// to be exactly 0..31.
unsafe fn shmmni_count() -> u64 {
    static CACHED: AtomicU64 = AtomicU64::new(0);
    let v = CACHED.load(Ordering::SeqCst);
    if v != 0 {
        return v;
    }
    let mut q: u64 = 0;
    let mut len: usize = size_of::<u64>();
    if sysctlbyname(
        c"kern.sysv.shmmni".as_ptr(),
        ptr::addr_of_mut!(q).cast::<c_void>(),
        &mut len,
        ptr::null_mut(),
        0,
    ) != 0
        || q == 0
    {
        q = 32; // the documented macOS default
    }
    if q > PSHM_IX_TOP {
        q = PSHM_IX_TOP; // keep pshm_mint_id's arithmetic sane
    }
    CACHED.store(q, Ordering::SeqCst);
    q
}

// Synthetic ids for POSIX-backed segments used to be PSHM_TAG | counter, on
// the claim that a kernel shmid never carries bit 0x40000000. That claim was
// false in both directions and cost real memory. Bit 0x40000000 is simply bit
// 14 of the sequence half, and _seq climbs on every allocation of a slot and
// is sticky until reboot: on this machine the very FIRST shmget of a run
// returned 0x40020000 (seq 0x4002, index 0). Driving it the other way,
// priming the table with 31 POSIX segments so the next synthetic id was
// 0x4000001f and then churning shmget/IPC_RMID on kernel slot 31, the kernel
// reissued exactly 0x4000001f after 29 148 iterations -- 0.02 s -- and
// lxrt_shmat on that legitimate KERNEL id returned the POSIX segment's memory
// instead of the kernel segment's.
//
// So identity is no longer taken from a bit pattern. A synthetic id is built
// in the part of the id space the kernel provably cannot address: its index
// half counts DOWN from 0xffff, which puts it above shmmni, and
// shm_find_segment_by_shmid rejects every such id before it ever looks at a
// segment (measured: shmctl(0x0001ffff, IPC_STAT) returns EINVAL, as does
// every id in that range). The sequence half is a generation counter kept
// below 0x8000 so the id stays a positive int, which mirrors what the kernel
// does with _seq and keeps a reused slot from handing back an id a guest was
// still holding.
//
// The shmctl(IPC_STAT) probe below cannot fire given the index argument. It is
// kept anyway: it is one syscall on a path taken once per segment, and it is
// the only check that would survive a future XNU changing how an id is formed.
unsafe fn pshm_mint_id(slot: c_int) -> c_int {
    let ix: u64 = PSHM_IX_TOP.wrapping_sub(slot as u64);
    if ix < shmmni_count() {
        return -1; // no index left outside the kernel's
    }
    let mut tries = 0;
    while tries < 64 {
        tries += 1;
        if G_PSHM_GEN == 0 || G_PSHM_GEN >= 0x8000 {
            G_PSHM_GEN = 1;
        }
        let gen: u32 = G_PSHM_GEN;
        G_PSHM_GEN = gen.wrapping_add(1);
        let id = ((gen << 16) | ix as u32) as c_int;
        let mut ds = MaybeUninit::<ShmidDs>::uninit();
        if shmctl(id, IPC_STAT, ds.as_mut_ptr()) == 0 {
            continue; // a live kernel segment owns it
        }
        return id;
    }
    -1
}

unsafe fn pshm_by_id(id: c_int) -> *mut Pshm {
    // An id whose index half is inside the kernel's range belongs to the
    // kernel and must never be matched here, whatever the table happens to
    // hold. This is the check that closes the misrouting measured above, and
    // it also makes the kernel path free of the scan below.
    if id <= 0 || ((id & 0xffff) as u32 as u64) < shmmni_count() {
        return ptr::null_mut();
    }
    let mut i = 0;
    while i < G_SEG_HIGH {
        let s = seg(i as usize);
        if (*s).used && (*s).id == id {
            return s;
        }
        i += 1;
    }
    ptr::null_mut()
}

unsafe fn pshm_by_key(key: i32) -> *mut Pshm {
    if key == 0 {
        return ptr::null_mut(); // IPC_PRIVATE is not findable by key
    }
    let mut i = 0;
    while i < G_SEG_HIGH {
        let s = seg(i as usize);
        if (*s).used && !(*s).private_key && !(*s).destroyed && (*s).key == key {
            return s;
        }
        i += 1;
    }
    ptr::null_mut()
}

unsafe fn pshm_alloc() -> *mut Pshm {
    let mut i: usize = 0;
    while i < MAX_PSHM {
        let s = seg(i);
        if !(*s).used {
            let id = pshm_mint_id(i as c_int);
            if id < 0 {
                return ptr::null_mut();
            }
            ptr::write_bytes(s, 0, 1);
            (*s).used = true;
            (*s).fd = -1;
            (*s).id = id;
            if i as c_int >= G_SEG_HIGH {
                G_SEG_HIGH = i as c_int + 1;
            }
            return s;
        }
        i += 1;
    }
    ptr::null_mut()
}

// Caller holds g_lock. Linux's IPC_RMID does not destroy a segment that still
// has attaches: it detaches the key, sets SHM_DEST in shm_perm.mode and keeps
// the id addressable until the last detach. Verified on the guest -- after
// IPC_RMID with one attach outstanding, IPC_STAT still succeeds and reports
// mode 01600 with nattch 1, and a further shmat still succeeds. Keeping the
// entry alive has a second effect that matters here: the id is not recycled
// while a guest is still using it.
unsafe fn pshm_destroy(s: *mut Pshm) {
    if !(*s).destroyed {
        if !(*s).private_key {
            let mut name = [0 as c_char; 40];
            pshm_name(name.as_mut_ptr(), name.len(), (*s).key, false);
            shm_unlink(name.as_ptr());
        }
        (*s).destroyed = true;
        // Linux resets shm_perm.key to IPC_PRIVATE on RMID, which is what
        // frees the key for immediate reuse by a new shmget.
        (*s).key = 0;
        (*s).ctime = time(ptr::null_mut());
    }
    if (*s).nattch == 0 {
        if (*s).fd >= 0 {
            close((*s).fd);
        }
        (*s).fd = -1;
        (*s).used = false;
    }
}

// Caller holds g_lock. The creating process publishes the name with
// shm_open(O_CREAT|O_EXCL) and only sizes it with ftruncate a moment later, so
// an adopter that arrives in between fstats st_size == 0 and used to fail the
// `have < size` test with EINVAL -- reproduced deterministically, and again
// live with a creator that delayed its ftruncate by 30 ms.
//
// The obvious fix is not available. flock() on a Darwin POSIX shm descriptor
// returns ENOTSUP (measured: errno 45 for LOCK_EX, LOCK_SH and LOCK_UN alike),
// so the size cannot be published atomically with the name that way, and POSIX
// shm has no rename to build the object under a private name first. So the
// adopter waits for a size, briefly and boundedly. 50 ms is far longer than
// the window -- it is one ftruncate wide -- and an object that is still zero
// after it is treated as genuinely empty, which is the honest reading.
unsafe fn pshm_measure(fd: c_int, want: u64, have: *mut u64, mode: *mut u32) -> c_long {
    const STEP_NS: u64 = 200_000; // 200 us
    const CAP_NS: u64 = 50_000_000; // 50 ms
    let mut waited: u64 = 0;
    let mut st = MaybeUninit::<Stat>::uninit();
    let st = st.as_mut_ptr();
    loop {
        if fstat(fd, st) != 0 {
            return lerr(errno());
        }
        if (*st).st_size > 0 || want == 0 {
            break;
        }
        if waited >= CAP_NS {
            // Nobody ever sized it. That is not a race any more, it is an
            // object whose creator died between shm_open and ftruncate, and
            // leaving it that way would make the key permanently unusable --
            // the name exists, so every later shmget adopts it and every
            // adopter fails the size check. Size it here instead. macOS
            // allows exactly one ftruncate per object, so a racer that gets
            // in first simply wins and this one re-reads the result.
            if ftruncate(fd, want as i64) != 0 && errno() != EINVAL {
                return lerr(errno());
            }
            if fstat(fd, st) != 0 {
                return lerr(errno());
            }
            break;
        }
        let ts = Timespec {
            tv_sec: 0,
            tv_nsec: STEP_NS as c_long,
        };
        nanosleep(&ts, ptr::null_mut());
        waited += STEP_NS;
    }
    *have = (*st).st_size as u64;
    *mode = ((*st).st_mode as c_int & PERM_MASK) as u32;
    0
}

// How many times a lost create race is retried before the errno is believed.
// The window is one ftruncate wide and only one racer can lose per round, so
// eight is already generous; it is bounded so a pathological loop cannot spin
// forever against a process that keeps unlinking the name.
const PSHM_RACE_TRIES: c_int = 8;

// Caller holds g_lock. Returns the id, or a negated Linux errno.
unsafe fn pshm_get(key: i32, size: u64, lshmflg: c_int) -> c_long {
    let private = key == 0; // IPC_PRIVATE is 0 on both systems
    let excl = (lshmflg & L_IPC_CREAT) != 0 && (lshmflg & L_IPC_EXCL) != 0;

    let mut s = pshm_by_key(key);
    if !s.is_null() {
        if excl {
            return lerr(EEXIST);
        }
        if size != 0 && size > (*s).size {
            return lerr(EINVAL); // Linux: requested size exceeds segsz
        }
        return (*s).id as c_long;
    }

    let mut name = [0 as c_char; 40];
    pshm_name(name.as_mut_ptr(), name.len(), key, private);
    let name = name.as_ptr();

    let mut fd: c_int;
    let mut have: u64 = 0;
    let mut adopted_mode: u32 = 0;
    let created: bool;

    if private {
        // IPC_PRIVATE is always a create, with or without IPC_CREAT: Linux's
        // ipcget() routes key == IPC_PRIVATE straight to ipcget_new() and
        // never looks at the flag. This path used to demand IPC_CREAT and
        // answered -ENOENT without it -- verified against the guest, where
        // shmget(IPC_PRIVATE, 10805760, 0600) with no IPC_CREAT returns an id,
        // and that is exactly the call Steam's 10 805 760-byte segment makes.
        if size == 0 {
            return lerr(EINVAL); // Linux: size 0 on a create is EINVAL
        }
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, lshmflg & PERM_MASK);
        if fd < 0 {
            return lerr(errno());
        }
        // macOS allows a POSIX shm object to be sized exactly once; a second
        // ftruncate on the same object returns EINVAL (measured). So this must
        // happen here, on the creating path, and nowhere else.
        if ftruncate(fd, size as i64) != 0 {
            let e = errno();
            close(fd);
            shm_unlink(name);
            return lerr(e);
        }
        // An IPC_PRIVATE segment has no key anyone could look it up by, so the
        // name exists only to create the object. Unlinking it here keeps the
        // object alive through this fd and any mapping of it, makes it
        // unreachable to every other process, and -- the reason it is done at
        // once rather than at IPC_RMID -- leaves nothing behind if the runtime
        // is killed. A POSIX shm object outlives its creator until reboot.
        // fork() still works: the child inherits the descriptor.
        shm_unlink(name);
        have = size;
        created = true;
    } else {
        // Adopt an existing object before creating one, so a second guest
        // process asking for the same key attaches to the same memory -- and
        // go back and adopt when the create loses, rather than handing the
        // loser's EEXIST to the guest. Two processes calling
        // shmget(key, oversize, IPC_CREAT|0600) at once with no IPC_EXCL both
        // succeed on Linux; before this loop, measured with four children
        // released from a pipe, two of the four failed (-EEXIST and -EINVAL).
        // EEXIST is only an answer when the caller actually asked for
        // IPC_EXCL.
        let mut attempt: c_int = 0;
        loop {
            fd = shm_open(name, O_RDWR);
            if fd >= 0 {
                if excl {
                    close(fd);
                    return lerr(EEXIST);
                }
                let rc = pshm_measure(fd, size, &mut have, &mut adopted_mode);
                if rc < 0 {
                    close(fd);
                    return rc;
                }
                created = false;
                break;
            }
            let mut e = errno();
            if e != ENOENT {
                return lerr(e);
            }
            if (lshmflg & L_IPC_CREAT) == 0 {
                return lerr(ENOENT);
            }
            if size == 0 {
                return lerr(EINVAL);
            }
            fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, lshmflg & PERM_MASK);
            if fd >= 0 {
                have = size;
                if ftruncate(fd, size as i64) != 0 {
                    e = errno();
                    // macOS allows exactly one ftruncate per object, so EINVAL
                    // here means somebody sized the name in the instant
                    // between the create and this call -- which can only be
                    // another runtime that adopted it and ran the recovery in
                    // pshm_measure. Take their size rather than destroying an
                    // object they are already using.
                    let mut st = MaybeUninit::<Stat>::uninit();
                    if e != EINVAL
                        || fstat(fd, st.as_mut_ptr()) != 0
                        || ((*st.as_ptr()).st_size as u64) < size
                    {
                        close(fd);
                        shm_unlink(name);
                        return lerr(e);
                    }
                    have = (*st.as_ptr()).st_size as u64;
                }
                created = true;
                break;
            }
            e = errno();
            if e != EEXIST || attempt >= PSHM_RACE_TRIES {
                return lerr(e);
            }
            // Someone else created the name between the two calls above. Go
            // round and adopt what they made.
            attempt += 1;
        }
    }

    // Only reachable on the adopt path: every create above leaves have >= size.
    if size != 0 && have < size {
        close(fd);
        return lerr(EINVAL);
    }

    s = pshm_alloc();
    if s.is_null() {
        close(fd);
        if created && !private {
            shm_unlink(name);
        }
        return lerr(ENOSPC);
    }
    (*s).private_key = private;
    (*s).key = if private { 0 } else { key };
    (*s).fd = fd;
    // A created segment reports what the guest asked for. An ADOPTED one
    // reports the object's own size, which is what Linux does -- shmget for a
    // key that already exists returns the existing segment and IPC_STAT
    // reports ITS segsz, not the size the caller happened to pass. The
    // divergence that remains is the rounding: Darwin rounds a POSIX shm
    // object up to a 16 KiB page, so a 10 805 760-byte segment adopted by a
    // second process reports 10 813 440 where Linux would report the exact
    // request.
    (*s).size = if created {
        size
    } else if have != 0 {
        have
    } else {
        size
    };
    // The recorded mode is what IPC_STAT reports. On a create it is what the
    // guest asked for, which is also what the object got: umask does not
    // narrow shm_open on Darwin (measured, 0666 under umask 077 still creates
    // 0666). On an adopt it is the object's own mode, which may have been set
    // by a process that asked for something else; see the IPC_SET note.
    (*s).mode = if created {
        (lshmflg & PERM_MASK) as u32
    } else {
        adopted_mode
    };
    (*s).cpid = getpid();
    (*s).ctime = time(ptr::null_mut());
    (*s).id as c_long
}

fn shmflg_to_darwin_get(lshmflg: c_int) -> c_int {
    // SHM_HUGETLB (04000) and SHM_NORESERVE (010000) are Linux-only and sit on
    // bits Darwin reads as IPC_NOWAIT and IPC_M; see the constants above.
    lshmflg & (PERM_MASK | L_IPC_CREAT | L_IPC_EXCL)
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_shmget(key: i32, size: u64, lshmflg: c_int) -> c_long {
    if (lshmflg & (L_SHM_HUGETLB | L_SHM_NORESERVE)) != 0 {
        warn_once!(
            G_SAID_SHMFLAGS,
            c"shmget: dropping SHM_HUGETLB/SHM_NORESERVE; macOS SysV shared memory has neither, and both bits mean something else to Darwin.\n",
        );
    }

    ipc_lock();

    // Which backing serves a key is decided by what ALREADY exists, in one
    // fixed order -- this table, then the POSIX name, then the kernel -- and
    // only a key nothing holds yet is decided by the requested size. The
    // comment that used to sit here claimed the decision depended "only on the
    // requested size, never on transient state", and it did not hold: the
    // POSIX name was consulted only after the kernel answered ENOENT, so a
    // process asking for an under-shmmax size got a brand new kernel segment
    // while another process was already serving that key from a POSIX object,
    // and the two silently shared nothing. Measured: with the object for the
    // key pre-created at 10 805 760 bytes, shmget(key, 4096, IPC_CREAT|0600)
    // returned a kernel id and a 4096-byte segment.
    if key != 0 {
        if !pshm_by_key(key).is_null() {
            let r = pshm_get(key, size, lshmflg);
            ipc_unlock();
            return r;
        }
        let mut probe = [0 as c_char; 40];
        pshm_name(probe.as_mut_ptr(), probe.len(), key, false);
        let pfd = shm_open(probe.as_ptr(), O_RDWR);
        if pfd >= 0 {
            close(pfd);
            let r = pshm_get(key, size, lshmflg);
            ipc_unlock();
            return r;
        }
        // A size-0, mode-0 shmget is a pure lookup on both systems (verified
        // on both: it returns the id for a live key and ENOENT otherwise). If
        // the kernel already owns this key, the kernel answers -- even for an
        // oversized request, because then the right answer is the one Linux
        // gives, EINVAL for a request larger than the existing segment, not a
        // second segment hidden behind the same key. Verified on both sides:
        // shmget(key, 10805760, IPC_CREAT|0600) against a live 4096-byte
        // segment returns EINVAL.
        let kid = shmget(key, 0, 0);
        if kid >= 0 {
            let id = shmget(key, size as usize, shmflg_to_darwin_get(lshmflg));
            let r = if id >= 0 { id as c_long } else { lerr(errno()) };
            ipc_unlock();
            return r;
        }
        if errno() != ENOENT {
            let r = lerr(errno()); // EACCES on someone else's key, say
            ipc_unlock();
            return r;
        }
    }

    let cap = shmmax_bytes();
    let oversize = size > cap;

    if !oversize {
        kdefer_sweep();
        let id = shmget(key, size as usize, shmflg_to_darwin_get(lshmflg));
        if id >= 0 {
            ipc_unlock();
            return id as c_long;
        }
        let e = errno();
        if e == ENOSPC {
            warn_once!(
                G_SAID_SHMMNI,
                c"shmget: out of SysV segments. macOS defaults to kern.sysv.shmmni=32 system-wide and kern.sysv.shmseg=8 attaches per process, against Linux's 4096 of each. Raise them with sysctl as root at boot.\n",
            );
        }
        if e != ENOENT {
            ipc_unlock();
            return lerr(e);
        }
        // ENOENT means the kernel has no segment for this key and the caller
        // did not ask to create one, or another process created the POSIX
        // object in the window since the probe above. Fall through: pshm_get
        // answers ENOENT too if there is genuinely nothing there.
    } else {
        warn_once!(
            G_SAID_SHMMAX,
            c"shmget(%llu) exceeds kern.sysv.shmmax (%llu). Serving it from POSIX shared memory instead; lifetime, permission and nattch semantics are approximated. To use real SysV shared memory, raise the limit as root at boot:\n  sudo sysctl -w kern.sysv.shmmax=%llu kern.sysv.shmall=%llu\n",
            size as c_ulonglong,
            cap as c_ulonglong,
            (64u64 << 20) as c_ulonglong,
            (64u64 << 20).checked_div(getpagesize() as u64).unwrap_or(0) as c_ulonglong,
        );
    }

    let r = pshm_get(key, size, lshmflg);
    ipc_unlock();
    r
}

unsafe fn attach_alloc() -> *mut Attach {
    let mut i: usize = 0;
    while i < MAX_ATTACH {
        let a = att(i);
        if !(*a).used {
            if i as c_int >= G_ATT_HIGH {
                G_ATT_HIGH = i as c_int + 1;
            }
            return a;
        }
        i += 1;
    }
    ptr::null_mut()
}

unsafe fn attach_find(addr: u64) -> *mut Attach {
    let mut i = 0;
    while i < G_ATT_HIGH {
        let a = att(i as usize);
        if (*a).used && (*a).addr == addr {
            return a;
        }
        i += 1;
    }
    ptr::null_mut()
}

// Is [addr, addr+len) nothing but an untagged PROT_NONE reservation? That is
// how FEX's 32-bit allocator holds the unused part of a guest window
// (GUESTBASE): the guest's view of those pages is "free", so an attach there
// may replace the placeholder, where anything else in the way must still
// make shmat fail as Linux does.
unsafe fn only_reservation(addr: u64, len: u64) -> bool {
    let mut p = addr;
    let end = addr.wrapping_add(len);
    while p < end {
        let mut ra: u64 = p;
        let mut rs: u64 = 0;
        let mut ri = MaybeUninit::<VmRegionExtendedInfo>::uninit();
        let mut rc: u32 = VM_REGION_EXTENDED_INFO_COUNT;
        let mut obj: MachPort = MACH_PORT_NULL;
        if mach_vm_region(
            mach_task_self_,
            &mut ra,
            &mut rs,
            VM_REGION_EXTENDED_INFO,
            ri.as_mut_ptr().cast::<c_int>(),
            &mut rc,
            &mut obj,
        ) != KERN_SUCCESS
            || rs == 0
            || ra > p
        {
            return false; // a hole or the end: not a reservation
        }
        let ri = ri.as_ptr();
        if (*ri).user_tag != 0 || (*ri).protection != VM_PROT_NONE {
            return false;
        }
        p = ra.wrapping_add(rs);
    }
    true
}

// ------------------------------------------ kernel segments: deferred IPC_RMID
//
// Linux keeps a segment that was IPC_RMID'd while attached alive AND
// attachable until its last detach (shm_perm.mode shows SHM_DEST meanwhile).
// Darwin's shmctl(IPC_RMID) refuses every later shmat at once. X clients rely
// on the Linux rule: cairo's xlib SHM pool sends XShmAttach, which the X
// server carries out later, and marks the segment for removal right after it,
// before the server has attached (a Linux cairo does not XSync there:
// IPC_RMID_DEFERRED_RELEASE). On the Mac the server's shmat then failed, the
// client got BadAccess (MIT-SHM ShmAttach), and GDK's X error handler ended
// the program: Heroic's (Electron's) main process exited that way at start
// (MEASURED, benchmarks/stage24-heroic.txt).
//
// So an IPC_RMID of a kernel segment this process has attached is deferred:
// the segment stays attachable, and the real IPC_RMID is issued when this
// process detaches its last attach of it, execs or exits. A process killed
// before that leaves a record in RMID_DIR/<id> (its pid and the segment's
// ctime); a later shmget in any runtime process removes the segment of a
// record whose process is gone, as Linux would have at that process's exit.
// Only kernel segments: the POSIX-backed ones (pshm_destroy) already follow
// the Linux rule inside the runtime, and no other process can attach them.

const RMID_DIR: &std::ffi::CStr = c"/tmp/lxrt-shm-rmid";
const MAX_KATT: usize = 64;
const MAX_KDEFER: usize = 64;

#[repr(C)]
#[derive(Clone, Copy)]
struct Katt {
    used: bool,
    addr: u64,
    id: c_int,
}

// this process's kernel attaches
static mut G_KATT: [Katt; MAX_KATT] = [Katt {
    used: false,
    addr: 0,
    id: 0,
}; MAX_KATT];
// ids whose IPC_RMID is deferred
static mut G_KDEFER: [c_int; MAX_KDEFER] = [0; MAX_KDEFER];
static mut G_NKDEFER: c_int = 0;

#[inline(always)]
unsafe fn katt(i: usize) -> *mut Katt {
    ptr::addr_of_mut!(G_KATT).cast::<Katt>().add(i)
}

#[inline(always)]
unsafe fn kdefer(i: usize) -> *mut c_int {
    ptr::addr_of_mut!(G_KDEFER).cast::<c_int>().add(i)
}

unsafe fn katt_add(addr: u64, id: c_int) {
    let mut i: usize = 0;
    while i < MAX_KATT {
        let k = katt(i);
        if !(*k).used {
            *k = Katt {
                used: true,
                addr,
                id,
            };
            return;
        }
        i += 1;
    }
    // Full: this attach is not tracked, and an IPC_RMID of its segment is not
    // deferred unless another attach of it is (kdefer_try).
}

unsafe fn katt_take(addr: u64) -> c_int {
    let mut i: usize = 0;
    while i < MAX_KATT {
        let k = katt(i);
        if (*k).used && (*k).addr == addr {
            (*k).used = false;
            return (*k).id;
        }
        i += 1;
    }
    -1
}

unsafe fn katt_holds(id: c_int) -> bool {
    let mut i: usize = 0;
    while i < MAX_KATT {
        let k = katt(i);
        if (*k).used && (*k).id == id {
            return true;
        }
        i += 1;
    }
    false
}

unsafe fn kdefer_index(id: c_int) -> c_int {
    let mut i = 0;
    while i < G_NKDEFER {
        if *kdefer(i as usize) == id {
            return i;
        }
        i += 1;
    }
    -1
}

unsafe fn kdefer_path(out: *mut c_char, cap: usize, id: c_int) {
    snprintf(out, cap, c"%s/%d".as_ptr(), RMID_DIR.as_ptr(), id);
}

// Defer the IPC_RMID of kernel segment `id`: true when it was deferred (the
// caller reports success), false when the caller should remove it now.
// Caller holds the lock.
unsafe fn kdefer_try(id: c_int) -> bool {
    if kdefer_index(id) >= 0 {
        return true; // a second IPC_RMID: already marked
    }
    if !katt_holds(id) || G_NKDEFER >= MAX_KDEFER as c_int {
        return false;
    }
    let mut ds = MaybeUninit::<ShmidDs>::uninit();
    if shmctl(id, IPC_STAT, ds.as_mut_ptr()) != 0 {
        return false;
    }
    *kdefer(G_NKDEFER as usize) = id;
    G_NKDEFER += 1;
    let mut p = [0 as c_char; 64];
    mkdir(RMID_DIR.as_ptr(), 0o700);
    kdefer_path(p.as_mut_ptr(), p.len(), id);
    let fd = open(
        p.as_ptr(),
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        0o600 as c_int,
    );
    if fd >= 0 {
        let ctime: i64 = (*ds.as_ptr()).shm_ctime;
        dprintf(
            fd,
            c"%d %lld\n".as_ptr(),
            getpid() as c_int,
            ctime as c_longlong,
        );
        close(fd);
    }
    true
}

// The deferred IPC_RMID of `id`, now. Caller holds the lock.
unsafe fn kdefer_release(id: c_int) {
    let i = kdefer_index(id);
    if i < 0 {
        return;
    }
    G_NKDEFER -= 1;
    *kdefer(i as usize) = *kdefer(G_NKDEFER as usize);
    shmctl(id, IPC_RMID, ptr::null_mut());
    let mut p = [0 as c_char; 64];
    kdefer_path(p.as_mut_ptr(), p.len(), id);
    unlink(p.as_ptr());
}

// Records left by processes that died before their deferred IPC_RMID. The
// segment is removed only if it is still the one recorded (same ctime), so a
// reused id is left alone; a record whose pid is alive (or was reused) waits.
unsafe fn kdefer_sweep() {
    let d = lxrt_opendir_private(RMID_DIR.as_ptr()); // not in the guest's fd table
    if d.is_null() {
        return;
    }
    loop {
        let e = readdir(d) as *const Dirent;
        if e.is_null() {
            break;
        }
        let d_name = ptr::addr_of!((*e).d_name).cast::<c_char>();
        let mut end: *mut c_char = ptr::null_mut();
        let id = strtol(d_name, &mut end, 10);
        let first = *d_name as u8;
        if first < b'0' || first > b'9' || *end != 0 || id < 0 || id > INT32_MAX {
            continue;
        }
        let mut p = [0 as c_char; 64];
        kdefer_path(p.as_mut_ptr(), p.len(), id as c_int);
        let f = fopen(p.as_ptr(), c"r".as_ptr());
        if f.is_null() {
            continue;
        }
        let mut pid: c_int = 0;
        let mut ctime: c_longlong = 0;
        let n = fscanf(
            f,
            c"%d %lld".as_ptr(),
            &mut pid as *mut c_int,
            &mut ctime as *mut c_longlong,
        );
        fclose(f);
        if n != 2 || pid <= 0 {
            continue;
        }
        if kill(pid, 0) == 0 || errno() == EPERM {
            continue; // its process is still there
        }
        let mut ds = MaybeUninit::<ShmidDs>::uninit();
        if shmctl(id as c_int, IPC_STAT, ds.as_mut_ptr()) == 0
            && { (*ds.as_ptr()).shm_ctime } as c_longlong == ctime
        {
            shmctl(id as c_int, IPC_RMID, ptr::null_mut());
        }
        unlink(p.as_ptr());
    }
    lxrt_closedir_private(d);
}

// This process is exiting or exec'ing: every deferred IPC_RMID happens now.
// Linux frees such a segment once the last process detaches, which exit and
// exec both do.
#[no_mangle]
pub unsafe extern "C" fn lxrt_sysv_exit() {
    if G_NKDEFER == 0 {
        return;
    }
    ipc_lock();
    while G_NKDEFER != 0 {
        kdefer_release(*kdefer((G_NKDEFER - 1) as usize));
    }
    ipc_unlock();
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_shmat(shmid: c_int, shmaddr: u64, lshmflg: c_int) -> c_long {
    if (lshmflg & L_SHM_REMAP) != 0 {
        // Linux's SHM_REMAP replaces whatever is already mapped at the
        // address. Darwin's shmat has no such flag and no way to express it
        // without unmapping first, which is not atomic and would leave a hole
        // another thread could take. Refused rather than approximated.
        return lerr(EINVAL);
    }

    if (lshmflg & L_SHM_EXEC) != 0 {
        // Linux's SHM_EXEC asks for an executable shared mapping. Apple
        // Silicon will not give a thread write and execute on the same page
        // (see jit.c), and a SysV segment is writable by definition, so this
        // cannot be honoured rather than merely being unimplemented.
        return lerr(ENOSYS);
    }

    // SHM_RND rounds the address down to SHMLBA. SHMLBA is the page size on
    // both, but the page sizes differ: 4096 (or 65536) on Linux aarch64
    // against 16384 on Apple Silicon, where sys/shm.h defines SHMLBA as
    // 16*1024 explicitly. The rounding is therefore done here, to Darwin's
    // value, and SHM_RND is not forwarded -- letting Darwin round again would
    // be harmless but letting the guest assume Linux's granularity is not.
    let mut want = shmaddr;
    if want != 0 && (lshmflg & L_SHM_RND) != 0 {
        want = align_down(want, SHMLBA);
    }

    ipc_lock();
    let s = pshm_by_id(shmid);

    if s.is_null() {
        ipc_unlock();
        // SHM_RND is not forwarded: the rounding above already used
        // Darwin's SHMLBA, and only SHM_RDONLY is left to carry.
        let mut ds = MaybeUninit::<ShmidDs>::uninit();
        if want != 0
            && shmctl(shmid, IPC_STAT, ds.as_mut_ptr()) == 0
            && only_reservation(want, align_up((*ds.as_ptr()).shm_segsz as u64, SHMLBA))
        {
            munmap(
                want as usize as *mut c_void,
                align_up((*ds.as_ptr()).shm_segsz as u64, SHMLBA) as usize,
            );
        }
        let p = shmat(
            shmid,
            want as usize as *const c_void,
            lshmflg & L_SHM_RDONLY,
        );
        if p == MAP_FAILED {
            // (void *)-1
            return lerr(errno());
        }
        ipc_lock();
        katt_add(p as usize as u64, shmid);
        ipc_unlock();
        return p as usize as c_long;
    }

    // POSIX-backed segment: an ordinary MAP_SHARED mapping of the object.
    // A segment that has been IPC_RMID'd is still attachable while it lives,
    // which is what Linux does: verified on the guest, shmat after IPC_RMID
    // with an attach outstanding still succeeds. pshm_destroy only frees the
    // entry once the last attach is gone, so reaching one here means it is
    // genuinely still alive.
    let prot = if (lshmflg & L_SHM_RDONLY) != 0 {
        PROT_READ
    } else {
        PROT_READ | PROT_WRITE
    };
    let a = attach_alloc();
    if a.is_null() {
        ipc_unlock();
        // Linux reports a process that has attached too many segments as
        // EMFILE, not ENOMEM: this is a table-full condition, not an
        // out-of-memory one, and a guest that distinguishes them acts on the
        // difference.
        return lerr(EMFILE);
    }
    let fixed = if want != 0 && only_reservation(want, align_up((*s).size, SHMLBA)) {
        MAP_FIXED
    } else {
        0
    };
    let p = mmap(
        want as usize as *mut c_void,
        (*s).size as usize,
        prot,
        MAP_SHARED | fixed,
        (*s).fd,
        0,
    );
    if p == MAP_FAILED {
        let e = errno();
        ipc_unlock();
        return lerr(e);
    }
    // MAP_FIXED is used only over a bare reservation (above): anywhere else
    // it would silently destroy whatever the guest already had at that address. Without it the hint is advisory,
    // so an address the kernel could not honour has to be reported rather than
    // quietly relocated, which is what Linux does when the range is occupied.
    if want != 0 && p as usize as u64 != want {
        munmap(p, (*s).size as usize);
        ipc_unlock();
        return lerr(EINVAL);
    }
    (*a).used = true;
    (*a).addr = p as usize as u64;
    (*a).len = (*s).size;
    (*a).id = (*s).id;
    (*s).nattch += 1;
    (*s).lpid = getpid();
    (*s).atime = time(ptr::null_mut());
    ipc_unlock();
    p as usize as c_long
}

// Inside a guest window (GUESTBASE) a detached range goes back to being a
// PROT_NONE placeholder, as FEX's allocator does for munmap: a hole there
// would let the host's own allocations move into guest address space, where
// the next guest MAP_FIXED lands on them.
unsafe fn rereserve_if_window(addr: u64, len: u64) {
    let base = lxrt_gbase();
    if base == 0 || addr < base || addr.wrapping_add(len) > base.wrapping_add(1u64 << 32) {
        return;
    }
    mmap(
        addr as usize as *mut c_void,
        len as usize,
        PROT_NONE,
        MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE,
        -1,
        0,
    );
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_shmdt(shmaddr: u64) -> c_long {
    if shmaddr == 0 {
        return lerr(EINVAL);
    }

    ipc_lock();
    let a = attach_find(shmaddr);
    if a.is_null() {
        ipc_unlock();
        // Not one of ours, so it is a kernel attach. Darwin's shmdt reports an
        // address that was never attached as EINVAL, same as Linux.
        let mut ra: u64 = shmaddr;
        let mut rs: u64 = 0;
        let mut ri = MaybeUninit::<VmRegionBasicInfo64>::uninit();
        let mut rc: u32 = VM_REGION_BASIC_INFO_COUNT_64;
        let mut obj: MachPort = MACH_PORT_NULL;
        let sized = mach_vm_region(
            mach_task_self_,
            &mut ra,
            &mut rs,
            VM_REGION_BASIC_INFO_64,
            ri.as_mut_ptr().cast::<c_int>(),
            &mut rc,
            &mut obj,
        ) == KERN_SUCCESS
            && ra == shmaddr;
        if shmdt(shmaddr as usize as *const c_void) != 0 {
            return lerr(errno());
        }
        if sized {
            rereserve_if_window(shmaddr, rs);
        }
        ipc_lock();
        let kid = katt_take(shmaddr);
        if kid >= 0 && !katt_holds(kid) {
            kdefer_release(kid); // its last attach here: see kdefer_try
        }
        ipc_unlock();
        return 0;
    }
    let len: usize = (*a).len as usize;
    let s = pshm_by_id((*a).id);
    (*a).used = false;
    if !s.is_null() {
        if (*s).nattch != 0 {
            (*s).nattch -= 1;
        }
        (*s).lpid = getpid();
        (*s).dtime = time(ptr::null_mut());
        // Last detach of a segment the guest already RMID'd: this is where
        // Linux actually frees it, and so is this.
        if (*s).destroyed && (*s).nattch == 0 {
            pshm_destroy(s);
        }
    }
    ipc_unlock();
    if munmap(shmaddr as usize as *mut c_void, len) != 0 {
        return lerr(errno());
    }
    rereserve_if_window(shmaddr, align_up(len as u64, SHMLBA));
    0
}

// Fills a Linux shmid64_ds for a POSIX-backed segment. Everything here is
// synthesised from this process's own bookkeeping, which is why nattch is
// local-only; see the fallback note above.
unsafe fn pshm_stat(s: *const Pshm, l: *mut LinuxShmid64Ds) {
    ptr::write_bytes(l, 0, 1);
    (*l).shm_perm.key = if (*s).private_key { 0 } else { (*s).key };
    (*l).shm_perm.uid = getuid();
    (*l).shm_perm.gid = getgid();
    (*l).shm_perm.cuid = getuid();
    (*l).shm_perm.cgid = getgid();
    // SHM_DEST is synthesised rather than masked away. Linux sets it in
    // shm_perm.mode once a segment has been RMID'd and still has attaches
    // (verified on the guest: mode 01600, nattch 1), and a guest that polls
    // for it is asking a question this module can answer for its own
    // segments -- unlike a kernel-backed one, where Darwin fails IPC_STAT
    // outright after RMID. See note 5 at the top.
    (*l).shm_perm.mode =
        ((*s).mode & PERM_MASK as u32) | if (*s).destroyed { L_SHM_DEST } else { 0 };
    (*l).shm_segsz = (*s).size;
    (*l).shm_cpid = (*s).cpid;
    (*l).shm_lpid = (*s).lpid;
    (*l).shm_nattch = (*s).nattch;
    (*l).shm_atime = (*s).atime;
    (*l).shm_dtime = (*s).dtime;
    (*l).shm_ctime = (*s).ctime;
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_shmctl(shmid: c_int, lcmd: c_int, lbuf: *mut c_void) -> c_long {
    let cmd = lcmd & !L_IPC_64;
    const DS_SIZE: usize = size_of::<LinuxShmid64Ds>();

    ipc_lock();
    let s = pshm_by_id(shmid);

    if !s.is_null() {
        let out: c_long;
        match cmd {
            L_IPC_STAT => {
                if !can_write(lbuf, DS_SIZE as u64) {
                    out = lerr(EFAULT);
                } else {
                    let mut l = MaybeUninit::<LinuxShmid64Ds>::uninit();
                    pshm_stat(s, l.as_mut_ptr());
                    ptr::copy_nonoverlapping(l.as_ptr().cast::<u8>(), lbuf.cast::<u8>(), DS_SIZE);
                    out = 0;
                }
            }
            L_IPC_SET => {
                if !can_read(lbuf, DS_SIZE as u64) {
                    out = lerr(EFAULT);
                } else {
                    let mut l = MaybeUninit::<LinuxShmid64Ds>::uninit();
                    ptr::copy_nonoverlapping(
                        lbuf.cast::<u8>(),
                        l.as_mut_ptr().cast::<u8>(),
                        DS_SIZE,
                    );
                    let l = l.as_ptr();
                    // Only the mode can be carried across. Changing the owner of a
                    // POSIX object means fchown, which needs privileges this process
                    // does not have, so a uid or gid change is refused instead of
                    // being accepted and ignored.
                    if (*l).shm_perm.uid != getuid() || (*l).shm_perm.gid != getgid() {
                        out = lerr(EPERM);
                    } else {
                        // Darwin fixes a POSIX shm object's mode at shm_open time and
                        // gives no way to change it afterwards. Measured on this machine:
                        // shm_open("/probe", O_CREAT|O_EXCL|O_RDWR, 0600) followed by
                        // fchmod(fd, 0644) returns -1 with errno 22, and fchown likewise.
                        // So the fchmod that used to be here could never succeed, and
                        // every guest chmod of a POSIX-backed segment came back -EINVAL
                        // where Linux returns 0 -- steamwebhelper widening a segment to
                        // 0666 for its renderer children is exactly that call. The mode is
                        // recorded instead: IPC_STAT reports what the guest set and the
                        // object keeps the mode it was created with. That is the same
                        // approximation this path already makes for permissions (POSIX shm
                        // checks its mode once at open, SysV checks ipc_perm on every
                        // operation), and it costs nothing for the case that matters,
                        // because every process in the Steam stack runs as the same uid
                        // and reaches the object through its owner bits either way.
                        (*s).mode = (*l).shm_perm.mode & PERM_MASK as u32;
                        (*s).ctime = time(ptr::null_mut());
                        out = 0;
                    }
                }
            }
            L_IPC_RMID => {
                // pshm_destroy drops the name (a keyed one is a pure function of
                // the key and the uid, so it can be rebuilt there; a private one
                // was already unlinked at creation), then either frees the entry
                // or keeps it alive for the attaches that remain, exactly as Linux
                // does. Existing mappings stay valid either way.
                //
                // What still differs, and cannot be fixed from user space: a later
                // shmget for this key creates a fresh empty object instead of
                // failing, and another PROCESS still holding this object open
                // never learns it was removed -- there is no system-wide refcount
                // on a POSIX shm object to consult.
                pshm_destroy(s);
                out = 0;
            }
            _ => {
                out = lerr(ENOSYS);
            }
        }
        ipc_unlock();
        return out;
    }
    ipc_unlock();

    match cmd {
        L_IPC_RMID => {
            // Deferred while this process has it attached (see kdefer_try).
            ipc_lock();
            let deferred = kdefer_try(shmid);
            ipc_unlock();
            if deferred {
                return 0;
            }
            if shmctl(shmid, IPC_RMID, ptr::null_mut()) != 0 {
                lerr(errno())
            } else {
                0
            }
        }

        L_IPC_STAT => {
            if !can_write(lbuf, DS_SIZE as u64) {
                return lerr(EFAULT);
            }
            let mut ds = MaybeUninit::<ShmidDs>::zeroed();
            if shmctl(shmid, IPC_STAT, ds.as_mut_ptr()) != 0 {
                return lerr(errno());
            }
            let mut l = MaybeUninit::<LinuxShmid64Ds>::uninit();
            shmid_to_linux(ds.as_ptr(), l.as_mut_ptr());
            // A deferred IPC_RMID reads as Linux's removed-but-attached segment.
            ipc_lock();
            if kdefer_index(shmid) >= 0 {
                (*l.as_mut_ptr()).shm_perm.mode |= L_SHM_DEST;
            }
            ipc_unlock();
            ptr::copy_nonoverlapping(l.as_ptr().cast::<u8>(), lbuf.cast::<u8>(), DS_SIZE);
            0
        }

        L_IPC_SET => {
            if !can_read(lbuf, DS_SIZE as u64) {
                return lerr(EFAULT);
            }
            let mut l = MaybeUninit::<LinuxShmid64Ds>::uninit();
            ptr::copy_nonoverlapping(lbuf.cast::<u8>(), l.as_mut_ptr().cast::<u8>(), DS_SIZE);
            let mut ds = MaybeUninit::<ShmidDs>::zeroed();
            if shmctl(shmid, IPC_STAT, ds.as_mut_ptr()) != 0 {
                return lerr(errno());
            }
            perm_from_linux(
                ptr::addr_of!((*l.as_ptr()).shm_perm),
                ptr::addr_of_mut!((*ds.as_mut_ptr()).shm_perm),
            );
            if shmctl(shmid, IPC_SET, ds.as_mut_ptr()) != 0 {
                lerr(errno())
            } else {
                0
            }
        }

        L_SHM_LOCK | L_SHM_UNLOCK => {
            // Linux pins a segment in RAM. Darwin's shmctl has no such command,
            // and mlock cannot stand in: the caller holds an id, not a mapping,
            // and the segment may not be attached in this process at all.
            lerr(ENOSYS)
        }

        L_IPC_INFO | L_SHM_STAT | L_SHM_INFO | L_SHM_STAT_ANY => {
            // Enumeration commands for `ipcs`. Darwin exposes the same data only
            // through sysctl kern.sysv.*, in a different shape. Nothing in the
            // Steam stack asks for them.
            lerr(ENOSYS)
        }

        _ => lerr(EINVAL),
    }
}
