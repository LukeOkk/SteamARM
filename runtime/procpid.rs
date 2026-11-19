// Other processes in /proc, and the socket tables in /proc/net.
//
// procfs.c describes the calling process only. The Steam client needs more:
// every websocket its UI opens to it is checked with
//
//     lsof -P -F upnR -i TCP@127.0.0.1:<port>
//
// and rejected when lsof cannot name the process at the other end ("lsof is
// required to run steam"; without it the UI dies with "Unexpected Transport
// Error 0x3008"). Linux lsof finds that process by reading /proc/net/tcp for
// the socket's inode and then every /proc/<pid>/fd/* for a "socket:[inode]"
// link, and the client then reads /proc/<pid>/cmdline and stat of the pid it
// got. So those have to describe the other guest processes truthfully.
//
// Darwin's libproc answers all of it for processes of the same user without
// any entitlement (proc_pidinfo / proc_pidfdinfo; task_for_pid is not needed):
// the fd table, each socket's addresses, ports and TCP state, and a stable
// per-socket kernel handle (soi_so, permuted by the kernel but constant for
// the socket's life) from which an inode number is derived -- the same number
// in /proc/net/* and in every fd link that refers to that socket.
//
// "Guest processes" are the host processes of this uid running this very
// runtime executable. Their cmdline and exe come from the procfs directory
// each of them maintains (<tmp>/lxrt-proc-<pid>, where FEX's PR_SET_MM_MAP
// lands the guest's argv); everything else is read from the kernel here.
// Files are generated into <our procfs dir>/.p/<pid>/ on each lookup.
//
// This is a line-for-line port of the former procpid.c. Like the C, it formats
// with the C library's snprintf into caller-sized buffers, keeps its scratch
// tables in unsynchronised statics (the callers serialise as they did before)
// and hands back pointers into per-thread buffers.

use std::cell::UnsafeCell;
use std::ffi::{c_char, c_int, c_long, c_uint, c_ulonglong, c_void, CStr};
use std::mem::{offset_of, size_of, MaybeUninit};
use std::ptr;
use std::thread::LocalKey;

use crate::ids::{
    lxrt_ids_on, lxrt_ids_pid, lxrt_ids_target_pid, lxrt_ids_threads, lxrt_ids_to_guest,
};

const MAXPIDS: usize = 4096;

// Darwin (arm64) values of the SDK constants the C original used.
const PATH_MAX: usize = 1024;
const MAXPATHLEN: usize = 1024;
const MAXCOMLEN: usize = 16;
const PROC_PIDPATHINFO_MAXSIZE: usize = 4 * MAXPATHLEN;
const PROC_PIDLISTFDS: c_int = 1;
const PROC_PIDTBSDINFO: c_int = 3;
const PROC_PIDTASKINFO: c_int = 4;
const PROC_PIDFDVNODEPATHINFO: c_int = 2;
const PROC_PIDFDSOCKETINFO: c_int = 3;
const PROC_PIDFDPIPEINFO: c_int = 6;
const PROX_FDTYPE_SOCKET: u32 = 2;
const SOCKINFO_IN: c_int = 1;
const SOCKINFO_TCP: c_int = 2;
const INI_IPV6: u8 = 0x2;
const SSTOP: u32 = 4;
const SZOMB: u32 = 5;
const CTL_KERN: c_int = 1;
const KERN_PROCARGS2: c_int = 49;
const O_RDONLY: c_int = 0;
const O_WRONLY: c_int = 0x1;
const O_CREAT: c_int = 0x200;
const O_TRUNC: c_int = 0x400;
const S_IFMT: u16 = 0o170000;
const S_IFIFO: u16 = 0o010000;
const S_IFDIR: u16 = 0o040000;
const S_IFSOCK: u16 = 0o140000;
const INT_MAX: c_long = c_int::MAX as c_long;

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
    pbi_comm: [c_char; MAXCOMLEN],
    pbi_name: [c_char; 2 * MAXCOMLEN],
    pbi_nfiles: u32,
    pbi_pgid: u32,
    pbi_pjobc: u32,
    e_tdev: u32,
    e_tpgid: u32,
    pbi_nice: i32,
    pbi_start_tvsec: u64,
    pbi_start_tvusec: u64,
}

#[repr(C)]
struct ProcTaskInfo {
    pti_virtual_size: u64,
    pti_resident_size: u64,
    pti_total_user: u64,
    pti_total_system: u64,
    pti_threads_user: u64,
    pti_threads_system: u64,
    pti_policy: i32,
    pti_faults: i32,
    pti_pageins: i32,
    pti_cow_faults: i32,
    pti_messages_sent: i32,
    pti_messages_received: i32,
    pti_syscalls_mach: i32,
    pti_syscalls_unix: i32,
    pti_csw: i32,
    pti_threadnum: i32,
    pti_numrunning: i32,
    pti_priority: i32,
}

#[repr(C)]
struct ProcFdInfo {
    proc_fd: i32,
    proc_fdtype: u32,
}

// struct proc_fileinfo and struct vinfo_stat: nothing in them is read here.
#[repr(C)]
struct ProcFileInfo {
    opaque: [u64; 3],
}

#[repr(C)]
struct VinfoStat {
    opaque: [u64; 17],
}

#[repr(C)]
struct SockbufInfo {
    sbi_cc: u32,
    sbi_hiwat: u32,
    sbi_mbcnt: u32,
    sbi_mbmax: u32,
    sbi_lowat: u32,
    sbi_flags: i16,
    sbi_timeo: i16,
}

#[repr(C)]
struct InsiV6 {
    in6_hlim: u8,
    in6_cksum: c_int,
    in6_ifindex: u16,
    in6_hops: i16,
}

// insi_faddr / insi_laddr are a union of struct in6_addr (ina_6) and struct
// in4in6_addr (ina_46: three pad words, then the IPv4 address). Both are
// four 32-bit words in network byte order; the IPv4 address is word 3.
#[repr(C)]
struct InSockInfo {
    insi_fport: c_int,
    insi_lport: c_int,
    insi_gencnt: u64,
    insi_flags: u32,
    insi_flow: u32,
    insi_vflag: u8,
    insi_ip_ttl: u8,
    rfu_1: u32,
    insi_faddr: [u32; 4],
    insi_laddr: [u32; 4],
    insi_v4: u8,
    insi_v6: InsiV6,
}

#[repr(C)]
struct TcpSockInfo {
    tcpsi_ini: InSockInfo,
    tcpsi_state: c_int,
    tcpsi_timer: [c_int; 4],
    tcpsi_mss: c_int,
    tcpsi_flags: u32,
    rfu_1: u32,
    tcpsi_tp: u64,
}

// The other members (un_sockinfo, ndrv_info, kern_event_info, kern_ctl_info,
// vsock_sockinfo) are not read; `whole` gives the union the SDK's size.
#[repr(C)]
union SoiProto {
    pri_in: std::mem::ManuallyDrop<InSockInfo>,
    pri_tcp: std::mem::ManuallyDrop<TcpSockInfo>,
    whole: [u64; 66],
}

#[repr(C)]
struct SocketInfo {
    soi_stat: VinfoStat,
    soi_so: u64,
    soi_pcb: u64,
    soi_type: c_int,
    soi_protocol: c_int,
    soi_family: c_int,
    soi_options: i16,
    soi_linger: i16,
    soi_state: i16,
    soi_qlen: i16,
    soi_incqlen: i16,
    soi_qlimit: i16,
    soi_timeo: i16,
    soi_error: u16,
    soi_oobmark: u32,
    soi_rcv: SockbufInfo,
    soi_snd: SockbufInfo,
    soi_kind: c_int,
    rfu_1: u32,
    soi_proto: SoiProto,
}

#[repr(C)]
struct SocketFdInfo {
    pfi: ProcFileInfo,
    psi: SocketInfo,
}

#[repr(C)]
struct PipeInfo {
    pipe_stat: VinfoStat,
    pipe_handle: u64,
    pipe_peerhandle: u64,
    pipe_status: c_int,
    rfu_1: c_int,
}

#[repr(C)]
struct PipeFdInfo {
    pfi: ProcFileInfo,
    pipeinfo: PipeInfo,
}

// struct vnode_info (vinfo_stat, type, pad, fsid): not read here.
#[repr(C)]
struct VnodeInfo {
    opaque: [u64; 19],
}

#[repr(C)]
struct VnodeInfoPath {
    vip_vi: VnodeInfo,
    vip_path: [c_char; MAXPATHLEN],
}

#[repr(C)]
struct VnodeFdInfoWithPath {
    pfi: ProcFileInfo,
    pvip: VnodeInfoPath,
}

#[repr(C)]
struct Timespec {
    tv_sec: i64,
    tv_nsec: i64,
}

// Darwin's struct stat (arm64: always the 64-bit inode layout).
#[repr(C)]
pub struct DarwinStat {
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

#[repr(C)]
struct Dirent {
    d_ino: u64,
    d_seekoff: u64,
    d_reclen: u16,
    d_namlen: u16,
    d_type: u8,
    d_name: [c_char; 1024],
}

// struct lxrt_aids_peer (android_ids.h).
#[repr(C)]
struct LxrtAidsPeer {
    ruid: u32,
    euid: u32,
    rgid: u32,
    egid: u32,
    eff: u64,
    prm: u64,
    inh: u64,
}

// Sizes and offsets as the macOS SDK's headers give them (arm64).
const _: [(); 136] = [(); size_of::<ProcBsdInfo>()];
const _: [(); 4] = [(); offset_of!(ProcBsdInfo, pbi_status)];
const _: [(); 16] = [(); offset_of!(ProcBsdInfo, pbi_ppid)];
const _: [(); 20] = [(); offset_of!(ProcBsdInfo, pbi_uid)];
const _: [(); 24] = [(); offset_of!(ProcBsdInfo, pbi_gid)];
const _: [(); 36] = [(); offset_of!(ProcBsdInfo, pbi_svuid)];
const _: [(); 40] = [(); offset_of!(ProcBsdInfo, pbi_svgid)];
const _: [(); 100] = [(); offset_of!(ProcBsdInfo, pbi_pgid)];
const _: [(); 96] = [(); size_of::<ProcTaskInfo>()];
const _: [(); 84] = [(); offset_of!(ProcTaskInfo, pti_threadnum)];
const _: [(); 8] = [(); size_of::<ProcFdInfo>()];
const _: [(); 24] = [(); size_of::<ProcFileInfo>()];
const _: [(); 136] = [(); size_of::<VinfoStat>()];
const _: [(); 24] = [(); size_of::<SockbufInfo>()];
const _: [(); 80] = [(); size_of::<InSockInfo>()];
const _: [(); 24] = [(); offset_of!(InSockInfo, insi_vflag)];
const _: [(); 32] = [(); offset_of!(InSockInfo, insi_faddr)];
const _: [(); 48] = [(); offset_of!(InSockInfo, insi_laddr)];
const _: [(); 64] = [(); offset_of!(InSockInfo, insi_v4)];
const _: [(); 68] = [(); offset_of!(InSockInfo, insi_v6)];
const _: [(); 120] = [(); size_of::<TcpSockInfo>()];
const _: [(); 80] = [(); offset_of!(TcpSockInfo, tcpsi_state)];
const _: [(); 528] = [(); size_of::<SoiProto>()];
const _: [(); 768] = [(); size_of::<SocketInfo>()];
const _: [(); 136] = [(); offset_of!(SocketInfo, soi_so)];
const _: [(); 184] = [(); offset_of!(SocketInfo, soi_rcv)];
const _: [(); 208] = [(); offset_of!(SocketInfo, soi_snd)];
const _: [(); 232] = [(); offset_of!(SocketInfo, soi_kind)];
const _: [(); 240] = [(); offset_of!(SocketInfo, soi_proto)];
const _: [(); 792] = [(); size_of::<SocketFdInfo>()];
const _: [(); 24] = [(); offset_of!(SocketFdInfo, psi)];
const _: [(); 160] = [(); size_of::<PipeInfo>()];
const _: [(); 136] = [(); offset_of!(PipeInfo, pipe_handle)];
const _: [(); 184] = [(); size_of::<PipeFdInfo>()];
const _: [(); 152] = [(); size_of::<VnodeInfo>()];
const _: [(); 1176] = [(); size_of::<VnodeInfoPath>()];
const _: [(); 152] = [(); offset_of!(VnodeInfoPath, vip_path)];
const _: [(); 1200] = [(); size_of::<VnodeFdInfoWithPath>()];
const _: [(); 144] = [(); size_of::<DarwinStat>()];
const _: [(); 4] = [(); offset_of!(DarwinStat, st_mode)];
const _: [(); 8] = [(); offset_of!(DarwinStat, st_ino)];
const _: [(); 112] = [(); offset_of!(DarwinStat, st_blksize)];
const _: [(); 1048] = [(); size_of::<Dirent>()];
const _: [(); 21] = [(); offset_of!(Dirent, d_name)];
const _: [(); 40] = [(); size_of::<LxrtAidsPeer>()];

#[link(name = "System")]
unsafe extern "C" {
    fn getpid() -> c_int;
    fn getuid() -> u32;
    fn getgid() -> u32;
    fn getenv(name: *const c_char) -> *mut c_char;
    fn malloc(size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn snprintf(buf: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
    fn sscanf(s: *const c_char, format: *const c_char, ...) -> c_int;
    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
    fn strncmp(a: *const c_char, b: *const c_char, n: usize) -> c_int;
    fn strnlen(s: *const c_char, max: usize) -> usize;
    fn strrchr(s: *const c_char, c: c_int) -> *mut c_char;
    fn strtol(s: *const c_char, end: *mut *mut c_char, base: c_int) -> c_long;
    fn atoi(s: *const c_char) -> c_int;
    fn open(path: *const c_char, flags: c_int, ...) -> c_int;
    fn close(fd: c_int) -> c_int;
    fn read(fd: c_int, buf: *mut c_void, count: usize) -> isize;
    fn write(fd: c_int, buf: *const c_void, count: usize) -> isize;
    fn rename(from: *const c_char, to: *const c_char) -> c_int;
    fn unlink(path: *const c_char) -> c_int;
    fn rmdir(path: *const c_char) -> c_int;
    fn mkdir(path: *const c_char, mode: u16) -> c_int;
    fn stat(path: *const c_char, st: *mut DarwinStat) -> c_int;
    fn readlink(path: *const c_char, buf: *mut c_char, size: usize) -> isize;
    fn readlinkat(fd: c_int, path: *const c_char, buf: *mut c_char, size: usize) -> isize;
    // dirents.rs declares the same function with its own struct dirent.
    #[allow(clashing_extern_declarations)]
    fn readdir(dir: *mut c_void) -> *mut Dirent;
    fn sysctl(
        name: *mut c_int,
        namelen: c_uint,
        oldp: *mut c_void,
        oldlenp: *mut usize,
        newp: *mut c_void,
        newlen: usize,
    ) -> c_int;
    fn proc_listallpids(buffer: *mut c_void, buffersize: c_int) -> c_int;
    fn proc_pidpath(pid: c_int, buffer: *mut c_void, buffersize: u32) -> c_int;
    fn proc_pidinfo(
        pid: c_int,
        flavor: c_int,
        arg: u64,
        buffer: *mut c_void,
        buffersize: c_int,
    ) -> c_int;
    fn proc_pidfdinfo(
        pid: c_int,
        fd: c_int,
        flavor: c_int,
        buffer: *mut c_void,
        buffersize: c_int,
    ) -> c_int;
}

// The rest of the runtime, still C.
unsafe extern "C" {
    fn lxrt_host_tmpdir() -> *const c_char; // procfs.c
    fn lxrt_opendir_private(path: *const c_char) -> *mut c_void; // DIR *
    fn lxrt_closedir_private(dir: *mut c_void);
    fn lxrt_file_same(path: *const c_char, data: *const c_void, len: usize) -> bool;
    fn lxrt_link_set(target: *const c_char, link: *const c_char);
    fn lxrt_netif_procnet(name: *const c_char, buf: *mut c_char, cap: usize) -> c_long; // netif.c
    fn lxrt_aids_lookup(pid: c_int, out: *mut LxrtAidsPeer) -> bool; // android_ids.c
}

// An uninitialised `char name[N]` on the stack, used through its pointer.
macro_rules! cbuf {
    ($name:ident, $size:expr) => {
        let mut $name = MaybeUninit::<[c_char; $size]>::uninit();
        let $name: *mut c_char = $name.as_mut_ptr().cast();
    };
}

// The address of a `static _Thread_local char buf[N]`. The buffers have no
// destructor, so the access neither allocates nor can fail.
fn tls_buf<const N: usize>(key: &'static LocalKey<UnsafeCell<[c_char; N]>>) -> *mut c_char {
    key.with(|cell| cell.get().cast())
}

// A socket's inode: derived from the kernel's socket handle, so the tables
// and the fd links agree, and non-zero (lsof treats 0 as "none").
fn sock_inode(so: u64) -> u64 {
    let mut z = so.wrapping_add(0x9e37_79b9_7f4a_7c15);
    z = (z ^ (z >> 30)).wrapping_mul(0xbf58_476d_1ce4_e5b9);
    z = (z ^ (z >> 27)).wrapping_mul(0x94d0_49bb_1331_11eb);
    z ^= z >> 31;
    (z & 0x7fff_ffff) | 0x100_0000
}

static mut SELF_EXE: [c_char; PROC_PIDPATHINFO_MAXSIZE] = [0; PROC_PIDPATHINFO_MAXSIZE];

unsafe fn same_exe_as_us(pid: c_int) -> bool {
    let me: *mut c_char = ptr::addr_of_mut!(SELF_EXE).cast();
    if *me == 0 && proc_pidpath(getpid(), me.cast(), PROC_PIDPATHINFO_MAXSIZE as u32) <= 0 {
        return false;
    }
    cbuf!(p, PROC_PIDPATHINFO_MAXSIZE);
    proc_pidpath(pid, p.cast(), PROC_PIDPATHINFO_MAXSIZE as u32) > 0 && strcmp(p, me) == 0
}

static mut ALL_PIDS: [c_int; MAXPIDS] = [0; MAXPIDS];

// Live guest processes, ourselves included, ascending.
unsafe fn guest_pids(out: *mut c_int, max: c_int) -> c_int {
    let all: *mut c_int = ptr::addr_of_mut!(ALL_PIDS).cast();
    let n = proc_listallpids(all.cast(), size_of::<[c_int; MAXPIDS]>() as c_int);
    if n <= 0 {
        return 0;
    }
    let me = getuid();
    let mut k: c_int = 0;
    let mut i: c_int = 0;
    while i < n && k < max {
        let pid = *all.add(i as usize);
        i += 1;
        if pid <= 0 {
            continue;
        }
        let mut bi = MaybeUninit::<ProcBsdInfo>::uninit();
        if proc_pidinfo(
            pid,
            PROC_PIDTBSDINFO,
            0,
            bi.as_mut_ptr().cast(),
            size_of::<ProcBsdInfo>() as c_int,
        ) != size_of::<ProcBsdInfo>() as c_int
        {
            continue;
        }
        let bi = bi.as_ptr();
        if (*bi).pbi_uid != me || (*bi).pbi_status == SZOMB {
            continue;
        }
        if pid != getpid() && !same_exe_as_us(pid) {
            continue;
        }
        *out.add(k as usize) = pid;
        k += 1;
    }
    // insertion sort: a few dozen at most
    let mut i: c_int = 1;
    while i < k {
        let mut j = i;
        while j > 0 && *out.add(j as usize - 1) > *out.add(j as usize) {
            let t = *out.add(j as usize);
            *out.add(j as usize) = *out.add(j as usize - 1);
            *out.add(j as usize - 1) = t;
            j -= 1;
        }
        i += 1;
    }
    k
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_procpid_is_guest(pid: c_int) -> bool {
    if pid <= 0 {
        return false;
    }
    if pid == getpid() {
        return true;
    }
    let mut bi = MaybeUninit::<ProcBsdInfo>::uninit();
    if proc_pidinfo(
        pid,
        PROC_PIDTBSDINFO,
        0,
        bi.as_mut_ptr().cast(),
        size_of::<ProcBsdInfo>() as c_int,
    ) != size_of::<ProcBsdInfo>() as c_int
    {
        return false;
    }
    let bi = bi.as_ptr();
    (*bi).pbi_uid == getuid() && (*bi).pbi_status != SZOMB && same_exe_as_us(pid)
}

// What /proc/<pid>/fd/<fd> points at, for descriptors F_GETPATH cannot name:
// "socket:[inode]", "pipe:[inode]", or a file's host path (readlink hands the
// guest its own view of that). False when the descriptor is not open.
#[no_mangle]
pub unsafe extern "C" fn lxrt_procpid_fd_target(
    pid: c_int,
    fd: c_int,
    out: *mut c_char,
    n: usize,
) -> bool {
    let mut si = MaybeUninit::<SocketFdInfo>::uninit();
    if proc_pidfdinfo(
        pid,
        fd,
        PROC_PIDFDSOCKETINFO,
        si.as_mut_ptr().cast(),
        size_of::<SocketFdInfo>() as c_int,
    ) == size_of::<SocketFdInfo>() as c_int
    {
        snprintf(
            out,
            n,
            c"socket:[%llu]".as_ptr(),
            sock_inode((*si.as_ptr()).psi.soi_so) as c_ulonglong,
        );
        return true;
    }
    let mut pi = MaybeUninit::<PipeFdInfo>::uninit();
    if proc_pidfdinfo(
        pid,
        fd,
        PROC_PIDFDPIPEINFO,
        pi.as_mut_ptr().cast(),
        size_of::<PipeFdInfo>() as c_int,
    ) == size_of::<PipeFdInfo>() as c_int
    {
        snprintf(
            out,
            n,
            c"pipe:[%llu]".as_ptr(),
            sock_inode((*pi.as_ptr()).pipeinfo.pipe_handle) as c_ulonglong,
        );
        return true;
    }
    let mut vi = MaybeUninit::<VnodeFdInfoWithPath>::uninit();
    if proc_pidfdinfo(
        pid,
        fd,
        PROC_PIDFDVNODEPATHINFO,
        vi.as_mut_ptr().cast(),
        size_of::<VnodeFdInfoWithPath>() as c_int,
    ) == size_of::<VnodeFdInfoWithPath>() as c_int
    {
        let path: *const c_char = ptr::addr_of!((*vi.as_ptr()).pvip.vip_path).cast();
        if *path != 0 {
            snprintf(out, n, c"%s".as_ptr(), path);
            return true;
        }
    }
    false
}

// ------------------------------------------------------------ /proc/net

const NET_TCP: c_int = 0;
const NET_TCP6: c_int = 1;
const NET_UDP: c_int = 2;
const NET_UDP6: c_int = 3;

// Darwin tcpsi_state -> Linux TCP_* (include/net/tcp_states.h).
fn linux_tcp_state(s: c_int) -> c_int {
    match s {
        1 => 0x0A,  // LISTEN
        2 => 0x02,  // SYN_SENT
        3 => 0x03,  // SYN_RECV
        4 => 0x01,  // ESTABLISHED
        5 => 0x08,  // CLOSE_WAIT
        6 => 0x04,  // FIN_WAIT1
        7 => 0x0B,  // CLOSING
        8 => 0x09,  // LAST_ACK
        9 => 0x05,  // FIN_WAIT2
        10 => 0x06, // TIME_WAIT
        _ => 0x07,  // CLOSE
    }
}

// An IPv4 address as the kernel prints it: the network-order word read as a
// little-endian integer (127.0.0.1 -> 0100007F). `a` is the in6_addr-sized
// union; the IPv4 address (ina_46.i46a_addr4) is its last word.
unsafe fn hex4(b: *mut c_char, n: usize, a: &[u32; 4]) {
    let w: u32 = a[3];
    snprintf(b, n, c"%08X".as_ptr(), w as c_uint);
}

unsafe fn hex6(b: *mut c_char, n: usize, a: &[u32; 4]) {
    let w: [u32; 4] = *a;
    snprintf(
        b,
        n,
        c"%08X%08X%08X%08X".as_ptr(),
        w[0] as c_uint,
        w[1] as c_uint,
        w[2] as c_uint,
        w[3] as c_uint,
    );
}

const NET_SEEN: usize = 16384;
static mut NET_PIDS: [c_int; MAXPIDS] = [0; MAXPIDS];
static mut NET_SEEN_SO: [u64; NET_SEEN] = [0; NET_SEEN];

unsafe fn gen_net(which: c_int, buf: *mut c_char, cap: usize) -> usize {
    let mut len: usize = 0;
    macro_rules! out {
        ($($arg:expr),+ $(,)?) => {{
            let k_ = snprintf(
                buf.wrapping_add(len),
                if len < cap { cap - len } else { 0 },
                $($arg),+
            );
            if k_ > 0 {
                len += k_ as usize;
            }
        }};
    }
    if which == NET_TCP || which == NET_UDP {
        out!(c"  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n".as_ptr());
    } else {
        out!(c"  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n".as_ptr());
    }

    let pids: *mut c_int = ptr::addr_of_mut!(NET_PIDS).cast();
    let np = guest_pids(pids, MAXPIDS as c_int);
    // One line per socket, however many processes hold it (fork, SCM_RIGHTS).
    let seen: *mut u64 = ptr::addr_of_mut!(NET_SEEN_SO).cast();
    let mut nseen: c_int = 0;
    let mut sl: c_int = 0;
    for i in 0..np {
        let pid = *pids.add(i as usize);
        let mut bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, ptr::null_mut(), 0);
        if bytes <= 0 {
            continue;
        }
        let fds: *mut ProcFdInfo = malloc(bytes as usize).cast();
        if fds.is_null() {
            continue;
        }
        bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.cast(), bytes);
        let nf: c_int = if bytes > 0 {
            bytes / size_of::<ProcFdInfo>() as c_int
        } else {
            0
        };
        for f in 0..nf {
            let fdi = fds.add(f as usize);
            if (*fdi).proc_fdtype != PROX_FDTYPE_SOCKET {
                continue;
            }
            let mut si = MaybeUninit::<SocketFdInfo>::uninit();
            if proc_pidfdinfo(
                pid,
                (*fdi).proc_fd,
                PROC_PIDFDSOCKETINFO,
                si.as_mut_ptr().cast(),
                size_of::<SocketFdInfo>() as c_int,
            ) != size_of::<SocketFdInfo>() as c_int
            {
                continue;
            }
            let psi: *const SocketInfo = ptr::addr_of!((*si.as_ptr()).psi);
            let kind = (*psi).soi_kind;
            let tcp = kind == SOCKINFO_TCP;
            if !tcp && kind != SOCKINFO_IN {
                continue;
            }
            if tcp != (which == NET_TCP || which == NET_TCP6) {
                continue;
            }
            // pri_tcp.tcpsi_ini is the first member of pri_tcp: both arms of
            // the C conditional are the same address, typed differently.
            let pri_tcp: *const TcpSockInfo = ptr::addr_of!((*psi).soi_proto.pri_tcp).cast();
            let inp: *const InSockInfo = if tcp {
                ptr::addr_of!((*pri_tcp).tcpsi_ini)
            } else {
                ptr::addr_of!((*psi).soi_proto.pri_in).cast()
            };
            let v6 = ((*inp).insi_vflag & INI_IPV6) != 0;
            if v6 != (which == NET_TCP6 || which == NET_UDP6) {
                continue;
            }
            let so = (*psi).soi_so;
            let mut dup = false;
            let mut s: c_int = 0;
            while s < nseen && !dup {
                dup = *seen.add(s as usize) == so;
                s += 1;
            }
            if dup {
                continue;
            }
            if nseen < NET_SEEN as c_int {
                *seen.add(nseen as usize) = so;
                nseen += 1;
            }
            cbuf!(la, 40);
            cbuf!(ra, 40);
            if v6 {
                hex6(la, 40, &(*inp).insi_laddr);
                hex6(ra, 40, &(*inp).insi_faddr);
            } else {
                hex4(la, 40, &(*inp).insi_laddr);
                hex4(ra, 40, &(*inp).insi_faddr);
            }
            let st: c_int = if tcp {
                linux_tcp_state((*pri_tcp).tcpsi_state)
            } else if (*inp).insi_fport != 0 {
                0x01
            } else {
                0x07
            };
            let line = sl;
            sl += 1;
            out!(
                c"%4d: %s:%04X %s:%04X %02X %08X:%08X 00:00000000 00000000 %5u        0 %llu 1 0000000000000000 100 0 0 10 0\n".as_ptr(),
                line,
                la as *const c_char,
                u16::from_be((*inp).insi_lport as u16) as c_int,
                ra as *const c_char,
                u16::from_be((*inp).insi_fport as u16) as c_int,
                st,
                (*psi).soi_snd.sbi_cc as c_uint,
                (*psi).soi_rcv.sbi_cc as c_uint,
                getuid() as c_uint,
                sock_inode(so) as c_ulonglong,
            );
        }
        free(fds.cast());
    }
    if len < cap {
        len
    } else {
        cap
    }
}

unsafe fn write_whole(path: *const c_char, data: *const c_char, len: usize) {
    if lxrt_file_same(path, data.cast(), len) {
        return;
    }
    cbuf!(tmp, 1200);
    snprintf(tmp, 1200, c"%s.%d".as_ptr(), path, getpid());
    let fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0o600 as c_int);
    if fd < 0 {
        return;
    }
    let mut off: usize = 0;
    while off < len {
        let w = write(fd, data.add(off).cast(), len - off);
        if w <= 0 {
            break;
        }
        off += w as usize;
    }
    close(fd);
    rename(tmp, path);
}

static NET_NAMES: [&CStr; 4] = [c"tcp", c"tcp6", c"udp", c"udp6"];

// route, ipv6_route, arp, dev: the host's routes and interfaces (netif.c).
static IF_NAMES: [&CStr; 4] = [c"route", c"ipv6_route", c"arp", c"dev"];

thread_local! {
    static NET_OUT: UnsafeCell<[c_char; 1024]> = const { UnsafeCell::new([0; 1024]) };
    static ROOT_OUT: UnsafeCell<[c_char; 1300]> = const { UnsafeCell::new([0; 1300]) };
    static PID_OUT: UnsafeCell<[c_char; 1300]> = const { UnsafeCell::new([0; 1300]) };
}

static mut NET_BUF: *mut c_char = ptr::null_mut();

// "net" or "net/<file>" (after /proc/ or /proc/<pid>/): host path, or NULL.
#[no_mangle]
pub unsafe extern "C" fn lxrt_procnet_translate(
    sub: *const c_char,
    dir: *const c_char,
) -> *const c_char {
    if strncmp(sub, c"net".as_ptr(), 3) != 0
        || (*sub.add(3) != 0 && *sub.add(3) as u8 != b'/')
    {
        return ptr::null();
    }
    let out = tls_buf(&NET_OUT);
    cbuf!(nd, 1024);
    snprintf(nd, 1024, c"%s/.net".as_ptr(), dir);
    mkdir(nd, 0o700);
    let file: *const c_char = if *sub.add(3) as u8 == b'/' {
        sub.add(4)
    } else {
        c"".as_ptr()
    };
    const CAP: usize = 4 << 20;
    let slot = ptr::addr_of_mut!(NET_BUF);
    if (*slot).is_null() {
        *slot = malloc(CAP).cast();
    }
    let buf = *slot;
    for name in IF_NAMES.iter() {
        if *file != 0 && strcmp(file, name.as_ptr()) != 0 {
            continue;
        }
        cbuf!(p, 1100);
        snprintf(p, 1100, c"%s/%s".as_ptr(), nd as *const c_char, name.as_ptr());
        let b: *mut c_char = malloc(256 << 10).cast();
        let n: c_long = if !b.is_null() {
            lxrt_netif_procnet(name.as_ptr(), b, 256 << 10)
        } else {
            -1
        };
        if n >= 0 {
            write_whole(p, b, n as usize);
        }
        free(b.cast());
        if *file != 0 {
            snprintf(out, 1024, c"%s".as_ptr(), p as *const c_char);
            return out;
        }
    }
    for (w, name) in NET_NAMES.iter().enumerate() {
        if *file != 0 && strcmp(file, name.as_ptr()) != 0 {
            continue;
        }
        cbuf!(p, 1100);
        snprintf(p, 1100, c"%s/%s".as_ptr(), nd as *const c_char, name.as_ptr());
        if !buf.is_null() {
            let n = gen_net(w as c_int, buf, CAP);
            write_whole(p, buf, n);
        }
        if *file != 0 {
            snprintf(out, 1024, c"%s".as_ptr(), p as *const c_char);
            return out;
        }
    }
    if *file != 0 {
        return ptr::null(); // unix, raw, ...: not provided
    }
    snprintf(out, 1024, c"%s".as_ptr(), nd as *const c_char);
    out
}

// ------------------------------------------------------------ /proc/<pid>

// The procfs directory another guest process maintains, if we can find it.
unsafe fn owner_dir(pid: c_int, out: *mut c_char, n: usize) -> bool {
    let cands: [*const c_char; 3] = [lxrt_host_tmpdir(), c"/tmp".as_ptr(), c"/private/tmp".as_ptr()];
    for cand in cands {
        if cand.is_null() || *cand == 0 {
            continue;
        }
        snprintf(out, n, c"%s/lxrt-proc-%d".as_ptr(), cand, pid);
        let mut st = MaybeUninit::<DarwinStat>::uninit();
        if stat(out, st.as_mut_ptr()) == 0 && ((*st.as_ptr()).st_mode & S_IFMT) == S_IFDIR {
            return true;
        }
    }
    false
}

unsafe fn copy_file(from: *const c_char, to: *const c_char) {
    cbuf!(buf, 65536);
    let fd = open(from, O_RDONLY);
    if fd < 0 {
        return;
    }
    let r = read(fd, buf.cast(), 65536);
    close(fd);
    if r >= 0 {
        write_whole(to, buf, r as usize);
    }
}

unsafe fn gen_pid_dir(pid: c_int, pd: *const c_char) {
    mkdir(pd, 0o700);
    let mut bi = MaybeUninit::<ProcBsdInfo>::uninit();
    if proc_pidinfo(
        pid,
        PROC_PIDTBSDINFO,
        0,
        bi.as_mut_ptr().cast(),
        size_of::<ProcBsdInfo>() as c_int,
    ) != size_of::<ProcBsdInfo>() as c_int
    {
        return;
    }
    let bi = bi.as_ptr();
    cbuf!(od, 1024);
    cbuf!(p, 1200);
    cbuf!(q, 1200);
    let have_owner = owner_dir(pid, od, 1024);

    // cmdline and exe: the guest's own, as that process published them.
    snprintf(p, 1200, c"%s/cmdline".as_ptr(), pd);
    let mut comm_buf: [c_char; 16] = [0; 16];
    comm_buf[0] = b'l' as c_char;
    comm_buf[1] = b'x' as c_char;
    comm_buf[2] = b'r' as c_char;
    comm_buf[3] = b't' as c_char;
    let comm: *mut c_char = comm_buf.as_mut_ptr();
    if have_owner {
        snprintf(q, 1200, c"%s/cmdline".as_ptr(), od as *const c_char);
        copy_file(q, p);
        let fd = open(q, O_RDONLY);
        if fd >= 0 {
            cbuf!(a0, PATH_MAX);
            let r = read(fd, a0.cast(), PATH_MAX - 1);
            close(fd);
            if r > 0 {
                *a0.add(r as usize) = 0;
                let base: *const c_char = strrchr(a0, b'/' as c_int);
                snprintf(
                    comm,
                    16,
                    c"%s".as_ptr(),
                    if !base.is_null() {
                        base.add(1)
                    } else {
                        a0 as *const c_char
                    },
                );
            }
        }
        snprintf(q, 1200, c"%s/exe".as_ptr(), od as *const c_char);
        cbuf!(tgt, PATH_MAX);
        let l = readlink(q, tgt, PATH_MAX - 1);
        snprintf(p, 1200, c"%s/exe".as_ptr(), pd);
        if l > 0 {
            *tgt.add(l as usize) = 0;
            lxrt_link_set(tgt, p);
        } else {
            unlink(p);
        }
    } else {
        write_whole(p, c"".as_ptr(), 0);
    }
    snprintf(p, 1200, c"%s/comm".as_ptr(), pd);
    cbuf!(line, 4096);
    let mut n: c_int = snprintf(line, 4096, c"%s\n".as_ptr(), comm as *const c_char);
    write_whole(p, line, n as usize);

    let state: c_char = if (*bi).pbi_status == SSTOP {
        b'T' as c_char
    } else if (*bi).pbi_status == SZOMB {
        b'Z' as c_char
    } else {
        b'S' as c_char
    };
    let guest_pid: c_int = if lxrt_ids_on() {
        lxrt_ids_to_guest(pid, 0)
    } else {
        pid
    };
    let mut guest_ppid: c_int = if lxrt_ids_on() {
        lxrt_ids_to_guest((*bi).pbi_ppid as c_int, 0)
    } else {
        (*bi).pbi_ppid as c_int
    };
    let mut guest_pgid: c_int = if lxrt_ids_on() {
        lxrt_ids_to_guest((*bi).pbi_pgid as c_int, 0)
    } else {
        (*bi).pbi_pgid as c_int
    };
    if guest_ppid == 0 {
        guest_ppid = 1;
    }
    if guest_pgid == 0 {
        guest_pgid = 1;
    }
    let mut ti = MaybeUninit::<ProcTaskInfo>::uninit();
    if proc_pidinfo(
        pid,
        PROC_PIDTASKINFO,
        0,
        ti.as_mut_ptr().cast(),
        size_of::<ProcTaskInfo>() as c_int,
    ) != size_of::<ProcTaskInfo>() as c_int
    {
        ptr::write_bytes(ti.as_mut_ptr(), 0, 1);
    }
    let ti = ti.as_ptr();
    let hz: c_long = 100;
    let ut: c_ulonglong = (*ti).pti_total_user / (1_000_000_000u64 / hz as u64);
    let stt: c_ulonglong = (*ti).pti_total_system / (1_000_000_000u64 / hz as u64);
    let threads: c_int = if (*ti).pti_threadnum > 0 {
        (*ti).pti_threadnum
    } else {
        1
    };
    n = snprintf(
        line,
        4096,
        c"%d (%s) %c %d %d %d 0 -1 4194304 0 0 0 0 %llu %llu 0 0 20 0 %d 0 0 %llu %llu 18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n".as_ptr(),
        guest_pid,
        comm as *const c_char,
        state as c_int,
        guest_ppid,
        guest_pgid,
        guest_pgid,
        ut,
        stt,
        threads,
        (*ti).pti_virtual_size as c_ulonglong,
        ((*ti).pti_resident_size / 4096) as c_ulonglong,
    );
    snprintf(p, 1200, c"%s/stat".as_ptr(), pd);
    write_whole(p, line, n as usize);

    // A guest with Android ids shows its virtual ones (android_ids.h): the
    // framework matches a pid to an app by the Uid: line here
    // (Process.getUidForPid, ActivityManagerService.isProcessAliveLocked).
    let mut ru: u32 = (*bi).pbi_uid;
    let mut eu: u32 = (*bi).pbi_uid;
    let mut su: u32 = (*bi).pbi_svuid;
    let mut rg: u32 = (*bi).pbi_gid;
    let mut eg: u32 = (*bi).pbi_gid;
    let mut sg: u32 = (*bi).pbi_svgid;
    let mut peer = MaybeUninit::<LxrtAidsPeer>::uninit();
    if lxrt_aids_lookup(pid, peer.as_mut_ptr()) {
        let peer = peer.as_ptr();
        ru = (*peer).ruid;
        su = (*peer).euid;
        eu = su;
        rg = (*peer).rgid;
        sg = (*peer).egid;
        eg = sg;
    }
    n = snprintf(
        line,
        4096,
        c"Name:\t%s\nUmask:\t0022\nState:\t%c (%s)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\nTracerPid:\t0\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nThreads:\t%d\n".as_ptr(),
        comm as *const c_char,
        state as c_int,
        if state as u8 == b'T' {
            c"stopped".as_ptr()
        } else {
            c"sleeping".as_ptr()
        },
        guest_pid,
        guest_pid,
        guest_ppid,
        ru as c_uint,
        eu as c_uint,
        su as c_uint,
        eu as c_uint,
        rg as c_uint,
        eg as c_uint,
        sg as c_uint,
        eg as c_uint,
        threads,
    );
    snprintf(p, 1200, c"%s/status".as_ptr(), pd);
    write_whole(p, line, n as usize);

    if lxrt_ids_on() {
        cbuf!(taskdir, 1200);
        snprintf(taskdir, 1200, c"%s/task".as_ptr(), pd);
        mkdir(taskdir, 0o700);
        // (The C left this uninitialised; lxrt_ids_threads fills what it counts.)
        let mut tids: [c_int; 512] = [0; 512];
        let mut count = lxrt_ids_threads(pid, tids.as_mut_ptr(), 512);
        if count > 512 {
            count = 512;
        }
        let tasks = lxrt_opendir_private(taskdir);
        if !tasks.is_null() {
            loop {
                let e = readdir(tasks);
                if e.is_null() {
                    break;
                }
                let name: *const c_char = ptr::addr_of!((*e).d_name).cast();
                let mut end: *mut c_char = ptr::null_mut();
                let old: c_long = strtol(name, &mut end, 10);
                if end as *const c_char == name || *end != 0 {
                    continue;
                }
                let mut live = false;
                let mut i: c_int = 0;
                while i < count {
                    if tids[i as usize] as c_long == old {
                        live = true;
                    }
                    i += 1;
                }
                if !live {
                    cbuf!(stale, 1300);
                    snprintf(stale, 1300, c"%s/%ld/status".as_ptr(), taskdir as *const c_char, old);
                    unlink(stale);
                    snprintf(stale, 1300, c"%s/%ld/comm".as_ptr(), taskdir as *const c_char, old);
                    unlink(stale);
                    snprintf(stale, 1300, c"%s/%ld".as_ptr(), taskdir as *const c_char, old);
                    rmdir(stale);
                }
            }
            lxrt_closedir_private(tasks);
        }
        let mut i: c_int = 0;
        while i < count {
            let tid = tids[i as usize];
            i += 1;
            cbuf!(td, 1300);
            cbuf!(file, 1400);
            snprintf(td, 1300, c"%s/%d".as_ptr(), taskdir as *const c_char, tid);
            mkdir(td, 0o700);
            snprintf(file, 1400, c"%s/comm".as_ptr(), td as *const c_char);
            n = snprintf(line, 4096, c"%s\n".as_ptr(), comm as *const c_char);
            write_whole(file, line, n as usize);
            snprintf(file, 1400, c"%s/status".as_ptr(), td as *const c_char);
            n = snprintf(
                line,
                4096,
                c"Name:\t%s\nState:\t%c (sleeping)\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\n".as_ptr(),
                comm as *const c_char,
                state as c_int,
                guest_pid,
                tid,
                guest_ppid,
            );
            write_whole(file, line, n as usize);
        }
    }
}

unsafe fn gen_pid_fds(pid: c_int, pd: *const c_char) {
    cbuf!(fdd, 1200);
    snprintf(fdd, 1200, c"%s/fd".as_ptr(), pd);
    mkdir(fdd, 0o700);
    let mut bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, ptr::null_mut(), 0);
    let fds: *mut ProcFdInfo = if bytes > 0 {
        malloc(bytes as usize).cast()
    } else {
        ptr::null_mut()
    };
    if !fds.is_null() {
        bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds.cast(), bytes);
    }
    let nf: c_int = if !fds.is_null() && bytes > 0 {
        bytes / size_of::<ProcFdInfo>() as c_int
    } else {
        0
    };
    // Only entries whose descriptor is gone are removed (see lxrt_link_set).
    let d = lxrt_opendir_private(fdd);
    if !d.is_null() {
        cbuf!(p, 1500);
        loop {
            let e = readdir(d);
            if e.is_null() {
                break;
            }
            let name: *const c_char = ptr::addr_of!((*e).d_name).cast();
            if *name as u8 == b'.' {
                continue;
            }
            let n = atoi(name);
            let mut live = false;
            let mut f: c_int = 0;
            while f < nf && !live {
                live = (*fds.add(f as usize)).proc_fd == n;
                f += 1;
            }
            if !live {
                snprintf(p, 1500, c"%s/%s".as_ptr(), fdd as *const c_char, name);
                unlink(p);
            }
        }
        lxrt_closedir_private(d);
    }
    if fds.is_null() {
        return;
    }
    for f in 0..nf {
        let fd = (*fds.add(f as usize)).proc_fd;
        cbuf!(tgt, PATH_MAX);
        cbuf!(p, 1500);
        if !lxrt_procpid_fd_target(pid, fd, tgt, PATH_MAX) {
            snprintf(tgt, PATH_MAX, c"anon_inode:[fd%d]".as_ptr(), fd);
        }
        snprintf(p, 1500, c"%s/%d".as_ptr(), fdd as *const c_char, fd);
        lxrt_link_set(tgt, p);
    }
    free(fds.cast());
}

// The guest root of another guest process: LXRT_ROOT in the environment its
// runtime was started with (KERN_PROCARGS2, as lxrt_procpid_environ), "" (the
// host's "/") without one.
unsafe fn pid_root(pid: c_int, out: *mut c_char, n: usize) -> bool {
    let mut mib: [c_int; 3] = [CTL_KERN, KERN_PROCARGS2, pid];
    let mut sz: usize = 0;
    if sysctl(mib.as_mut_ptr(), 3, ptr::null_mut(), &mut sz, ptr::null_mut(), 0) != 0
        || sz < size_of::<c_int>()
    {
        return false;
    }
    let b: *mut c_char = malloc(sz).cast();
    if b.is_null() {
        return false;
    }
    if sysctl(mib.as_mut_ptr(), 3, b.cast(), &mut sz, ptr::null_mut(), 0) != 0 {
        free(b.cast());
        return false;
    }
    let argc: c_int = ptr::read_unaligned(b.cast::<c_int>());
    let mut p: *const c_char = b.add(size_of::<c_int>());
    let end: *const c_char = b.add(sz);
    p = p.wrapping_add(strnlen(p, end as usize - p as usize));
    while p < end && *p == 0 {
        p = p.add(1);
    }
    let mut i: c_int = 0;
    while i < argc && p < end {
        p = p.wrapping_add(strnlen(p, end as usize - p as usize) + 1);
        i += 1;
    }
    *out = 0;
    while p < end && *p != 0 {
        let l = strnlen(p, end as usize - p as usize);
        if l > 10 && strncmp(p, c"LXRT_ROOT=".as_ptr(), 10) == 0 {
            snprintf(out, n, c"%.*s".as_ptr(), (l - 10) as c_int, p.add(10));
            break;
        }
        p = p.wrapping_add(l + 1);
    }
    free(b.cast());
    true
}

// /proc/<pid>[/...] for a guest process other than the caller. `rest` is the
// part after "/proc/". NULL when <pid> is not one.
#[no_mangle]
pub unsafe extern "C" fn lxrt_procpid_translate(
    rest: *const c_char,
    dir: *const c_char,
) -> *const c_char {
    let mut end: *mut c_char = ptr::null_mut();
    let mut pid: c_long = strtol(rest, &mut end, 10);
    if end as *const c_char == rest
        || (*end != 0 && *end as u8 != b'/')
        || pid <= 0
        || pid > INT_MAX
    {
        return ptr::null();
    }
    if lxrt_ids_on() {
        pid = lxrt_ids_target_pid(pid as c_int) as c_long;
        if pid < 0 {
            return ptr::null();
        }
    }
    if pid == getpid() as c_long || !lxrt_procpid_is_guest(pid as c_int) {
        return ptr::null();
    }
    let sub: *const c_char = if *end as u8 == b'/' {
        end.add(1)
    } else {
        c"".as_ptr()
    };
    cbuf!(base, 1024);
    cbuf!(pd, 1100);
    snprintf(base, 1024, c"%s/.p".as_ptr(), dir);
    mkdir(base, 0o700);
    snprintf(pd, 1100, c"%s/%ld".as_ptr(), base as *const c_char, pid);
    let net = lxrt_procnet_translate(sub, dir);
    if !net.is_null() {
        return net;
    }
    // /proc/<pid>/root: that process's "/", as procfs.c gives /proc/self/root.
    // It was missing (ENOENT), and PipeWire takes a client whose
    // /proc/<pid>/root cannot be opened for a Flatpak app: every client,
    // WirePlumber included, then got only read and execute (SteamOS's access
    // rules), WirePlumber's own requests were refused (EACCES) and no stream
    // was linked to the Mac's sink unless it won a race (MEASURED,
    // pipewire.sec.flatpak = true on all clients).
    if strncmp(sub, c"root".as_ptr(), 4) == 0 && (*sub.add(4) == 0 || *sub.add(4) as u8 == b'/') {
        let rootbuf = tls_buf(&ROOT_OUT);
        cbuf!(root, 1024);
        if !pid_root(pid as c_int, root, 1024) {
            return ptr::null();
        }
        let n = snprintf(
            rootbuf,
            1300,
            c"%s%s".as_ptr(),
            root as *const c_char,
            if *sub.add(4) != 0 {
                sub.add(4)
            } else {
                c"/".as_ptr()
            },
        );
        if n <= 0 || n as usize >= 1300 {
            return ptr::null();
        }
        return rootbuf;
    }
    // Generate only what was asked for: lsof readlink()s and stat()s every
    // /proc/<pid>/fd/<n> of every process, and rebuilding the whole fd
    // directory (or the stat/status files) per lookup made one lsof run take
    // 17 s under FEX -- longer than the Steam UI waits for its websocket.
    mkdir(pd, 0o700);
    if strcmp(sub, c"environ".as_ptr()) == 0 && getenv(c"LXRT_NO_ENVIRON".as_ptr()).is_null() {
        cbuf!(ep, 1200);
        snprintf(ep, 1200, c"%s/environ".as_ptr(), pd as *const c_char);
        lxrt_procpid_environ(pid as c_int, ep);
    } else if strncmp(sub, c"fd/".as_ptr(), 3) == 0
        && *sub.add(3) as u8 >= b'0'
        && *sub.add(3) as u8 <= b'9'
    {
        let fd = atoi(sub.add(3));
        cbuf!(fdd, 1200);
        cbuf!(p, 1300);
        cbuf!(tgt, PATH_MAX);
        snprintf(fdd, 1200, c"%s/fd".as_ptr(), pd as *const c_char);
        mkdir(fdd, 0o700);
        snprintf(p, 1300, c"%s/%d".as_ptr(), fdd as *const c_char, fd);
        if lxrt_procpid_fd_target(pid as c_int, fd, tgt, PATH_MAX) {
            lxrt_link_set(tgt, p);
        } else {
            unlink(p);
        }
    } else if strcmp(sub, c"fd".as_ptr()) == 0 {
        gen_pid_fds(pid as c_int, pd);
    } else {
        gen_pid_dir(pid as c_int, pd);
    }
    let out = tls_buf(&PID_OUT);
    snprintf(
        out,
        1300,
        c"%s%s%s".as_ptr(),
        pd as *const c_char,
        if *sub != 0 {
            c"/".as_ptr()
        } else {
            c"".as_ptr()
        },
        sub,
    );
    out
}

static mut LIST_PIDS: [c_int; MAXPIDS] = [0; MAXPIDS];

// Opening /proc itself: one entry per guest process, as symlinks into .p/
// (our own pid is the "<pid>" -> "." link procfs.c keeps).
#[no_mangle]
pub unsafe extern "C" fn lxrt_procpid_list(dir: *const c_char) {
    cbuf!(base, 1024);
    snprintf(base, 1024, c"%s/.p".as_ptr(), dir);
    mkdir(base, 0o700);
    let pids: *mut c_int = ptr::addr_of_mut!(LIST_PIDS).cast();
    let np = guest_pids(pids, MAXPIDS as c_int);
    // Drop entries of processes that are gone.
    let d = lxrt_opendir_private(dir);
    if !d.is_null() {
        loop {
            let e = readdir(d);
            if e.is_null() {
                break;
            }
            let name: *const c_char = ptr::addr_of!((*e).d_name).cast();
            let mut x: *mut c_char = ptr::null_mut();
            let v: c_long = strtol(name, &mut x, 10);
            if x as *const c_char == name || *x != 0 || v == lxrt_ids_pid() as c_long {
                continue;
            }
            let mut live = false;
            let mut i: c_int = 0;
            while i < np && !live {
                let host = *pids.add(i as usize);
                live = (if lxrt_ids_on() {
                    lxrt_ids_to_guest(host, 0)
                } else {
                    host
                }) as c_long
                    == v;
                i += 1;
            }
            if !live {
                cbuf!(p, 1300);
                snprintf(p, 1300, c"%s/%s".as_ptr(), dir, name);
                unlink(p);
            }
        }
        lxrt_closedir_private(d);
    }
    for i in 0..np {
        let host = *pids.add(i as usize);
        if host == getpid() {
            continue;
        }
        let guest_pid: c_int = if lxrt_ids_on() {
            lxrt_ids_to_guest(host, 0)
        } else {
            host
        };
        if guest_pid < 2 {
            continue;
        }
        cbuf!(pd, 1100);
        cbuf!(link, 1100);
        cbuf!(tgt, 64);
        snprintf(pd, 1100, c"%s/%d".as_ptr(), base as *const c_char, host);
        // Listing /proc must not rewrite every process's files: they are
        // generated when a path under /proc/<pid> is actually looked up.
        let mut st = MaybeUninit::<DarwinStat>::uninit();
        if stat(pd, st.as_mut_ptr()) != 0 {
            gen_pid_dir(host, pd);
        }
        snprintf(link, 1100, c"%s/%d".as_ptr(), dir, guest_pid);
        snprintf(tgt, 64, c".p/%d".as_ptr(), host);
        lxrt_link_set(tgt, link);
    }
    lxrt_procnet_translate(c"net".as_ptr(), dir);
    cbuf!(nl, 1100);
    snprintf(nl, 1100, c"%s/net".as_ptr(), dir);
    lxrt_link_set(c".net".as_ptr(), nl);
}

// stat() through /proc/<pid>/fd/<n> when the link is "socket:[N]" or
// "pipe:[N]": on Linux the magic link reaches the socket's inode, and lsof
// takes the inode it matches against /proc/net/* from exactly that stat.
// Here the link dangles; answer as sockfs/pipefs would. `hp` is the host path
// of the link itself (not followed).
#[no_mangle]
pub unsafe extern "C" fn lxrt_magic_link_stat(
    dfd: c_int,
    hp: *const c_char,
    d: *mut DarwinStat,
) -> bool {
    cbuf!(t, 64);
    let n = readlinkat(dfd, hp, t, 64 - 1);
    if n <= 0 {
        return false;
    }
    *t.add(n as usize) = 0;
    let mut ino = MaybeUninit::<c_ulonglong>::uninit();
    let sock = sscanf(t, c"socket:[%llu]".as_ptr(), ino.as_mut_ptr()) == 1;
    if !sock && sscanf(t, c"pipe:[%llu]".as_ptr(), ino.as_mut_ptr()) != 1 {
        return false;
    }
    let ino = ino.assume_init();
    ptr::write_bytes(d, 0, 1);
    (*d).st_mode = if sock {
        S_IFSOCK | 0o777
    } else {
        S_IFIFO | 0o600
    };
    (*d).st_ino = ino;
    (*d).st_dev = if sock { 8 } else { 13 }; // makedev(0, n): Linux's sockfs / pipefs
    (*d).st_nlink = 1;
    (*d).st_uid = getuid();
    (*d).st_gid = getgid();
    (*d).st_blksize = 4096;
    true
}

// /proc/<pid>/environ: the environment the process was started with, which is
// what Linux shows too. For a guest process that is the host environment of
// its runtime process: the runtime execs with the guest's envp as is.
// KERN_PROCARGS2 = int argc, exec path, NUL padding, argv[], envp[].
#[no_mangle]
pub unsafe extern "C" fn lxrt_procpid_environ(pid: c_int, out_path: *const c_char) -> bool {
    let mut mib: [c_int; 3] = [CTL_KERN, KERN_PROCARGS2, pid];
    let mut sz: usize = 0;
    if sysctl(mib.as_mut_ptr(), 3, ptr::null_mut(), &mut sz, ptr::null_mut(), 0) != 0
        || sz < size_of::<c_int>()
    {
        return false;
    }
    let b: *mut c_char = malloc(sz).cast();
    if b.is_null() {
        return false;
    }
    if sysctl(mib.as_mut_ptr(), 3, b.cast(), &mut sz, ptr::null_mut(), 0) != 0 {
        free(b.cast());
        return false;
    }
    let argc: c_int = ptr::read_unaligned(b.cast::<c_int>());
    let mut p: *const c_char = b.add(size_of::<c_int>());
    let end: *const c_char = b.add(sz);
    p = p.wrapping_add(strnlen(p, end as usize - p as usize)); // exec path
    while p < end && *p == 0 {
        p = p.add(1);
    }
    let mut i: c_int = 0;
    while i < argc && p < end {
        // argv
        p = p.wrapping_add(strnlen(p, end as usize - p as usize) + 1);
        i += 1;
    }
    let env = p;
    let mut q = env;
    while q < end && *q != 0 {
        // envp, up to the empty string
        let l = strnlen(q, end as usize - q as usize);
        q = q.wrapping_add(l + 1);
    }
    if q > end {
        q = end;
    }
    write_whole(out_path, env, q as usize - env as usize);
    free(b.cast());
    true
}
