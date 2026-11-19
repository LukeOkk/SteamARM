// netif.rs -- the Mac's network interfaces as a Linux guest asks for them:
// rtnetlink dumps (RTM_GETLINK, RTM_GETADDR) on an emulated NETLINK_ROUTE
// socket, and the SIOCGIF* interface ioctls.
//
// Darwin has no netlink. glibc's getifaddrs() and if_nameindex() are built on
// it, so with the socket refused (EAFNOSUPPORT) both failed and every Linux
// program saw no network interfaces at all. Wine's nsiproxy.sys does not
// check if_nameindex() for NULL: its first interface enumeration faulted on
// it while holding its interface list lock, and every later one -- each
// GetAdaptersInfo, GetIfTable, GetAdaptersAddresses of every Windows program
// in the prefix -- hung for good. Minecraft Dungeons II's protection takes
// the machine's adapter list as part of its fingerprint, waited on that call
// and killed the game 74 s later (exit code 300; MEASURED,
// benchmarks/stage62, 15).
//
// Everything comes from Darwin's getifaddrs(): an AF_LINK entry per interface
// (name, index, flags, hardware address, MTU, counters) and one entry per
// address. Interface names stay Darwin's (lo0, en0, ...) so the netlink
// answers, the ioctls and if_nametoindex() all agree.
#![allow(dead_code)]

use std::ffi::{c_char, c_int, c_long, c_uint, c_ulong, c_void};
use std::mem::{size_of, MaybeUninit};
use std::ptr;

// ---------------------------------------------------------------- Darwin ABI
//
// Values and layouts of the macOS arm64 SDK (<net/if.h>, <net/if_dl.h>,
// <net/route.h>, <ifaddrs.h>, <sys/socket.h>, <sys/stat.h>).

const IFNAMSIZ: usize = 16;
const IF_NAMESIZE: usize = 16;

const IFF_BROADCAST: c_uint = 0x2;
const IFF_LOOPBACK: c_uint = 0x8;
const IFF_POINTOPOINT: c_uint = 0x10;
const IFF_RUNNING: c_uint = 0x40;
const IFF_MULTICAST: c_uint = 0x8000;

const AF_INET: c_int = 2;
const AF_LINK: c_int = 18;
const AF_INET6: c_int = 30;
const IFT_ETHER: u8 = 0x6;
const MSG_DONTWAIT: c_int = 0x80;

const CTL_NET: c_int = 4;
const PF_ROUTE: c_int = 17;
const NET_RT_DUMP: c_int = 1;
const NET_RT_FLAGS: c_int = 2;

const RTF_UP: c_int = 0x1;
const RTF_GATEWAY: c_int = 0x2;
const RTF_HOST: c_int = 0x4;
const RTF_LLINFO: c_int = 0x400;
const RTF_STATIC: c_int = 0x800;
const RTF_WASCLONED: c_int = 0x20000;
const RTF_BROADCAST: c_int = 0x400000;
const RTF_MULTICAST: c_int = 0x800000;
const RTF_IFSCOPE: c_int = 0x1000000;

const RTAX_DST: usize = 0;
const RTAX_GATEWAY: usize = 1;
const RTAX_NETMASK: usize = 2;
const RTAX_MAX: usize = 8;

const EFAULT: c_int = 14;
const ENODEV: c_int = 19;
const EADDRNOTAVAIL: c_int = 49;

const S_IFMT: u16 = 0o170000;
const S_IFSOCK: u16 = 0o140000;
const STAT_SIZE: usize = 144;
const STAT_MODE_OFFSET: usize = 4;

#[repr(C)]
struct Sockaddr {
    sa_len: u8,
    sa_family: u8,
    sa_data: [u8; 14],
}

const SIN_ADDR_OFFSET: usize = 4; // offsetof(struct sockaddr_in, sin_addr)
const SIN6_ADDR_OFFSET: usize = 8; // offsetof(struct sockaddr_in6, sin6_addr)

#[repr(C)]
struct SockaddrDl {
    sdl_len: u8,
    sdl_family: u8,
    sdl_index: u16,
    sdl_type: u8,
    sdl_nlen: u8,
    sdl_alen: u8,
    sdl_slen: u8,
    sdl_data: [u8; 12],
}

const _: [(); 20] = [(); size_of::<SockaddrDl>()];

#[repr(C)]
struct Ifaddrs {
    ifa_next: *mut Ifaddrs,
    ifa_name: *mut c_char,
    ifa_flags: c_uint,
    ifa_addr: *mut Sockaddr,
    ifa_netmask: *mut Sockaddr,
    ifa_dstaddr: *mut Sockaddr, // also ifa_broadaddr
    ifa_data: *mut c_void,
}

const _: [(); 56] = [(); size_of::<Ifaddrs>()];

// The head of struct if_data, up to the last counter read here.
#[repr(C)]
struct IfData {
    ifi_type: u8,
    ifi_typelen: u8,
    ifi_physical: u8,
    ifi_addrlen: u8,
    ifi_hdrlen: u8,
    ifi_recvquota: u8,
    ifi_xmitquota: u8,
    ifi_unused1: u8,
    ifi_mtu: u32,
    ifi_metric: u32,
    ifi_baudrate: u32,
    ifi_ipackets: u32,
    ifi_ierrors: u32,
    ifi_opackets: u32,
    ifi_oerrors: u32,
    ifi_collisions: u32,
    ifi_ibytes: u32,
    ifi_obytes: u32,
}

const _: [(); 48] = [(); size_of::<IfData>()];

// struct rt_msghdr: only the head is read; the sockaddrs start at its full
// size.
#[repr(C)]
struct RtMsghdr {
    rtm_msglen: u16,
    rtm_version: u8,
    rtm_type: u8,
    rtm_index: u16,
    rtm_flags: c_int,
    rtm_addrs: c_int,
    rtm_rest: [u32; 19], // pid, seq, errno, use, inits, struct rt_metrics
}

const _: [(); 92] = [(); size_of::<RtMsghdr>()];

#[link(name = "System")]
unsafe extern "C" {
    static in6addr_loopback: [u8; 16];

    fn getifaddrs(list: *mut *mut Ifaddrs) -> c_int;
    fn freeifaddrs(list: *mut Ifaddrs);
    fn if_nametoindex(name: *const c_char) -> c_uint;
    fn if_indextoname(index: c_uint, name: *mut c_char) -> *mut c_char;
    fn send(socket: c_int, buffer: *const c_void, length: usize, flags: c_int) -> isize;
    fn fstat(fd: c_int, st: *mut c_void) -> c_int;
    fn sysctl(
        name: *mut c_int,
        namelen: c_uint,
        oldp: *mut c_void,
        oldlenp: *mut usize,
        newp: *mut c_void,
        newlen: usize,
    ) -> c_int;
    fn inet_ntop(af: c_int, src: *const c_void, dst: *mut c_char, size: u32) -> *const c_char;
    fn malloc(size: usize) -> *mut c_void;
    fn free(pointer: *mut c_void);
    fn strlen(s: *const c_char) -> usize;
    fn strcmp(a: *const c_char, b: *const c_char) -> c_int;
    fn strncmp(a: *const c_char, b: *const c_char, n: usize) -> c_int;
    fn strcpy(dst: *mut c_char, src: *const c_char) -> *mut c_char;
    fn strncpy(dst: *mut c_char, src: *const c_char, n: usize) -> *mut c_char;
    fn memcmp(a: *const c_void, b: *const c_void, n: usize) -> c_int;
    fn snprintf(buffer: *mut c_char, size: usize, format: *const c_char, ...) -> c_int;
}

fn lerr(host_errno: c_int) -> c_long {
    -(crate::errno_map::map_errno(host_errno) as c_long)
}

// ---------------------------------------------------------------- Linux ABI

const L_AF_INET: u8 = 2;
const L_AF_INET6: u8 = 10;
const L_AF_PACKET: u8 = 17;

const L_ARPHRD_ETHER: u16 = 1;
const L_ARPHRD_LOOPBACK: u16 = 772;
const L_ARPHRD_NONE: u16 = 65534;

const L_IFF_UP: u32 = 0x1;
const L_IFF_BROADCAST: u32 = 0x2;
const L_IFF_LOOPBACK: u32 = 0x8;
const L_IFF_POINTOPOINT: u32 = 0x10;
const L_IFF_RUNNING: u32 = 0x40;
const L_IFF_MULTICAST: u32 = 0x1000;
const L_IFF_LOWER_UP: u32 = 0x10000;

const NLMSG_NOOP_T: u16 = 1;
const NLMSG_ERROR_T: u16 = 2;
const NLMSG_DONE_T: u16 = 3;
const L_NLM_F_REQUEST: u16 = 0x1;
const L_NLM_F_MULTI: u16 = 0x2;
const L_NLM_F_ACK: u16 = 0x4;
const L_NLM_F_DUMP: u16 = 0x300;

const L_RTM_NEWLINK: u16 = 16;
const L_RTM_GETLINK: u16 = 18;
const L_RTM_NEWADDR: u16 = 20;
const L_RTM_GETADDR: u16 = 22;

const L_IFLA_ADDRESS: u16 = 1;
const L_IFLA_BROADCAST: u16 = 2;
const L_IFLA_IFNAME: u16 = 3;
const L_IFLA_MTU: u16 = 4;
const L_IFLA_STATS: u16 = 7;
const L_IFLA_TXQLEN: u16 = 13;
const L_IFLA_OPERSTATE: u16 = 16;

const L_IFA_ADDRESS: u16 = 1;
const L_IFA_LOCAL: u16 = 2;
const L_IFA_LABEL: u16 = 3;
const L_IFA_BROADCAST: u16 = 4;
const L_IFA_F_PERMANENT: u8 = 0x80;

const L_RT_SCOPE_UNIVERSE: u8 = 0;
const L_RT_SCOPE_LINK: u8 = 253;
const L_RT_SCOPE_HOST: u8 = 254;

const L_IF_OPER_UNKNOWN: u8 = 0;
const L_IF_OPER_DOWN: u8 = 2;
const L_IF_OPER_UP: u8 = 6;

#[repr(C)]
#[derive(Clone, Copy)]
struct LNlmsghdr {
    len: u32,
    type_: u16,
    flags: u16,
    seq: u32,
    pid: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct LIfinfomsg {
    family: u8,
    pad: u8,
    type_: u16,
    index: i32,
    flags: u32,
    change: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct LIfaddrmsg {
    family: u8,
    prefixlen: u8,
    flags: u8,
    scope: u8,
    index: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct LRtattr {
    len: u16,
    type_: u16,
}

const _: [(); 16] = [(); size_of::<LNlmsghdr>()];
const _: [(); 16] = [(); size_of::<LIfinfomsg>()];
const _: [(); 8] = [(); size_of::<LIfaddrmsg>()];
const _: [(); 4] = [(); size_of::<LRtattr>()];

const fn nl_align(n: u32) -> u32 {
    n.wrapping_add(3) & !3
}

// ---------------------------------------------------------------- the host

#[repr(C)]
#[derive(Clone, Copy)]
struct Netif {
    name: [c_char; IFNAMSIZ],
    index: u32,
    lflags: u32, // Linux IFF_*
    arphrd: u16,
    hw: [u8; 6],
    has_hw: bool,
    mtu: u32,
    stats: [u32; 6], // rx/tx packets, rx/tx bytes, rx/tx errors
}

// Darwin's IFF_* share Linux's values up to IFF_ALLMULTI (0x200); above that
// they differ (Darwin's OACTIVE 0x400 is Linux's MASTER, its MULTICAST 0x8000
// is Linux's DYNAMIC).
fn flags_to_linux(df: c_uint) -> u32 {
    let mut f = df & 0x3ff;
    if df & IFF_MULTICAST != 0 {
        f |= L_IFF_MULTICAST;
    }
    if df & IFF_RUNNING != 0 {
        f |= L_IFF_LOWER_UP;
    }
    f
}

// LLADDR(): the link-level address after the interface name.
unsafe fn lladdr(dl: *const SockaddrDl) -> *const u8 {
    ptr::addr_of!((*dl).sdl_data)
        .cast::<u8>()
        .add((*dl).sdl_nlen as usize)
}

unsafe fn fill_link(n: *mut Netif, a: *const Ifaddrs) {
    let dl = (*a).ifa_addr as *const SockaddrDl;
    if (*dl).sdl_type == IFT_ETHER && (*dl).sdl_alen == 6 {
        (*n).arphrd = L_ARPHRD_ETHER;
        ptr::copy_nonoverlapping(lladdr(dl), ptr::addr_of_mut!((*n).hw).cast::<u8>(), 6);
        (*n).has_hw = true;
    }
    let d = (*a).ifa_data as *const IfData;
    if !d.is_null() {
        (*n).mtu = ptr::addr_of!((*d).ifi_mtu).read_unaligned();
        (*n).stats[0] = ptr::addr_of!((*d).ifi_ipackets).read_unaligned();
        (*n).stats[1] = ptr::addr_of!((*d).ifi_opackets).read_unaligned();
        (*n).stats[2] = ptr::addr_of!((*d).ifi_ibytes).read_unaligned();
        (*n).stats[3] = ptr::addr_of!((*d).ifi_obytes).read_unaligned();
        (*n).stats[4] = ptr::addr_of!((*d).ifi_ierrors).read_unaligned();
        (*n).stats[5] = ptr::addr_of!((*d).ifi_oerrors).read_unaligned();
    }
}

// The interfaces, in Darwin's index order; *ap keeps getifaddrs' list for
// the addresses (freeifaddrs by the caller). Returns the count.
unsafe fn host_netifs(out: *mut Netif, cap: c_int, ap: *mut *mut Ifaddrs) -> c_int {
    *ap = ptr::null_mut();
    if getifaddrs(ap) != 0 {
        return 0;
    }
    let mut n: c_int = 0;
    let mut a = *ap;
    while !a.is_null() {
        let name = (*a).ifa_name;
        if name.is_null() || strlen(name) >= IFNAMSIZ {
            a = (*a).ifa_next;
            continue;
        }
        let mut i: c_int = 0;
        while i < n && strcmp(ptr::addr_of!((*out.add(i as usize)).name).cast(), name) != 0 {
            i += 1;
        }
        if i == n {
            if n == cap {
                a = (*a).ifa_next;
                continue;
            }
            let slot = out.add(n as usize);
            ptr::write_bytes(slot, 0, 1);
            strcpy(ptr::addr_of_mut!((*slot).name).cast(), name);
            (*slot).index = if_nametoindex(name);
            (*slot).lflags = flags_to_linux((*a).ifa_flags);
            (*slot).arphrd = if (*a).ifa_flags & IFF_LOOPBACK != 0 {
                L_ARPHRD_LOOPBACK
            } else {
                L_ARPHRD_NONE
            };
            (*slot).mtu = 1500;
            n += 1;
        }
        if !(*a).ifa_addr.is_null() && (*(*a).ifa_addr).sa_family as c_int == AF_LINK {
            fill_link(out.add(i as usize), a);
        }
        a = (*a).ifa_next;
    }
    // index order, as Linux dumps them
    let mut i: c_int = 1;
    while i < n {
        let mut j = i;
        while j > 0 && (*out.add(j as usize - 1)).index > (*out.add(j as usize)).index {
            ptr::swap(out.add(j as usize), out.add(j as usize - 1));
            j -= 1;
        }
        i += 1;
    }
    n
}

// ---------------------------------------------------------------- replies

// Reply datagrams, at most 4096 bytes each: glibc's getifaddrs() and
// Chromium's address tracker receive into one page, and a longer datagram
// arrives truncated (MSG_TRUNC), which glibc treats as failure.
const NLOUT_BUF: usize = 4096;

#[repr(C)]
struct Nlout {
    peer: c_int,
    buf: [u8; NLOUT_BUF],
    used: usize,
}

unsafe fn nl_flush(o: *mut Nlout) {
    if (*o).used != 0 {
        // a full queue drops it, as ENOBUFS would
        let _ = send(
            (*o).peer,
            ptr::addr_of!((*o).buf).cast(),
            (*o).used,
            MSG_DONTWAIT,
        );
    }
    (*o).used = 0;
}

// Starts a message of room `need` bytes, flushing first when it does not fit.
unsafe fn nl_begin(
    o: *mut Nlout,
    need: usize,
    type_: u16,
    flags: u16,
    seq: u32,
    pid: u32,
) -> *mut LNlmsghdr {
    if need > NLOUT_BUF {
        return ptr::null_mut();
    }
    if (*o).used.wrapping_add(need) > NLOUT_BUF {
        nl_flush(o);
    }
    let h = ptr::addr_of_mut!((*o).buf)
        .cast::<u8>()
        .add((*o).used)
        .cast::<LNlmsghdr>();
    ptr::write_bytes(h.cast::<u8>(), 0, need);
    (*h).len = size_of::<LNlmsghdr>() as u32;
    (*h).type_ = type_;
    (*h).flags = flags;
    (*h).seq = seq;
    (*h).pid = pid;
    h
}

unsafe fn nl_put(h: *mut LNlmsghdr, data: *const c_void, len: usize) -> *mut c_void {
    let p = h.cast::<u8>().add((*h).len as usize);
    ptr::copy_nonoverlapping(data.cast::<u8>(), p, len);
    (*h).len = (*h).len.wrapping_add(nl_align(len as u32));
    p.cast()
}

unsafe fn nl_attr(h: *mut LNlmsghdr, type_: u16, data: *const c_void, len: usize) {
    let a = LRtattr {
        len: (size_of::<LRtattr>() + len) as u16,
        type_,
    };
    let p = h.cast::<u8>().add((*h).len as usize);
    ptr::copy_nonoverlapping(ptr::addr_of!(a).cast::<u8>(), p, size_of::<LRtattr>());
    ptr::copy_nonoverlapping(data.cast::<u8>(), p.add(size_of::<LRtattr>()), len);
    (*h).len = (*h)
        .len
        .wrapping_add(nl_align((size_of::<LRtattr>() + len) as u32));
}

unsafe fn nl_end(o: *mut Nlout, h: *mut LNlmsghdr) {
    (*o).used = (*o).used.wrapping_add(nl_align((*h).len) as usize);
}

static HW_ZERO: [u8; 6] = [0; 6];
static HW_BCAST: [u8; 6] = [0xff; 6];

unsafe fn put_link(o: *mut Nlout, n: *const Netif, flags: u16, seq: u32, pid: u32) {
    let h = nl_begin(o, 512, L_RTM_NEWLINK, flags, seq, pid);
    if h.is_null() {
        return;
    }
    let ii = LIfinfomsg {
        family: 0,
        pad: 0,
        type_: (*n).arphrd,
        index: (*n).index as i32,
        flags: (*n).lflags,
        change: 0,
    };
    nl_put(h, ptr::addr_of!(ii).cast(), size_of::<LIfinfomsg>());
    let name = ptr::addr_of!((*n).name).cast::<c_char>();
    nl_attr(h, L_IFLA_IFNAME, name.cast(), strlen(name) + 1);
    if (*n).has_hw || (*n).arphrd == L_ARPHRD_LOOPBACK {
        let hw = ptr::addr_of!((*n).hw).cast::<c_void>();
        nl_attr(
            h,
            L_IFLA_ADDRESS,
            if (*n).has_hw { hw } else { HW_ZERO.as_ptr().cast() },
            6,
        );
        nl_attr(
            h,
            L_IFLA_BROADCAST,
            if (*n).has_hw {
                HW_BCAST.as_ptr().cast()
            } else {
                HW_ZERO.as_ptr().cast()
            },
            6,
        );
    }
    nl_attr(h, L_IFLA_MTU, ptr::addr_of!((*n).mtu).cast(), 4);
    let txq: u32 = if (*n).arphrd == L_ARPHRD_LOOPBACK { 0 } else { 1000 };
    nl_attr(h, L_IFLA_TXQLEN, ptr::addr_of!(txq).cast(), 4);
    let oper: u8 = if (*n).arphrd == L_ARPHRD_LOOPBACK {
        L_IF_OPER_UNKNOWN
    } else if (*n).lflags & L_IFF_RUNNING != 0 {
        L_IF_OPER_UP
    } else {
        L_IF_OPER_DOWN
    };
    nl_attr(h, L_IFLA_OPERSTATE, ptr::addr_of!(oper).cast(), 1);
    let mut st = [0u32; 23]; // struct rtnl_link_stats
    st[0] = (*n).stats[0];
    st[1] = (*n).stats[1];
    st[2] = (*n).stats[2];
    st[3] = (*n).stats[3];
    st[4] = (*n).stats[4];
    st[5] = (*n).stats[5];
    nl_attr(h, L_IFLA_STATS, st.as_ptr().cast(), size_of::<[u32; 23]>());
    nl_end(o, h);
}

unsafe fn prefix_len(mask: *const u8, bytes: c_int) -> c_int {
    let mut n: c_int = 0;
    let mut i: c_int = 0;
    while i < bytes {
        n += (*mask.add(i as usize)).count_ones() as c_int;
        i += 1;
    }
    n
}

unsafe fn put_addr(o: *mut Nlout, n: *const Netif, a: *const Ifaddrs, seq: u32, pid: u32) {
    let mut am = LIfaddrmsg {
        family: 0,
        prefixlen: 0,
        flags: L_IFA_F_PERMANENT,
        scope: L_RT_SCOPE_UNIVERSE,
        index: (*n).index,
    };
    let h = nl_begin(o, 256, L_RTM_NEWADDR, L_NLM_F_MULTI, seq, pid);
    if h.is_null() {
        return;
    }
    let addr = (*a).ifa_addr as *const u8;
    let dst = (*a).ifa_dstaddr as *const u8;
    let ptp = (*a).ifa_flags & IFF_POINTOPOINT != 0
        && !dst.is_null()
        && (*(*a).ifa_dstaddr).sa_family == (*(*a).ifa_addr).sa_family;
    if (*(*a).ifa_addr).sa_family as c_int == AF_INET {
        let ip = addr.add(SIN_ADDR_OFFSET);
        am.family = L_AF_INET;
        if !(*a).ifa_netmask.is_null() {
            am.prefixlen =
                prefix_len(((*a).ifa_netmask as *const u8).add(SIN_ADDR_OFFSET), 4) as u8;
        }
        if *ip == 127 {
            am.scope = L_RT_SCOPE_HOST;
        } else if *ip == 169 && *ip.add(1) == 254 {
            am.scope = L_RT_SCOPE_LINK;
        }
        nl_put(h, ptr::addr_of!(am).cast(), size_of::<LIfaddrmsg>());
        let peer = if ptp { dst.add(SIN_ADDR_OFFSET) } else { ip };
        nl_attr(h, L_IFA_ADDRESS, peer.cast(), 4);
        nl_attr(h, L_IFA_LOCAL, ip.cast(), 4);
        // ifa_broadaddr is ifa_dstaddr
        if !ptp
            && (*a).ifa_flags & IFF_BROADCAST != 0
            && !dst.is_null()
            && (*(*a).ifa_dstaddr).sa_family as c_int == AF_INET
        {
            nl_attr(h, L_IFA_BROADCAST, dst.add(SIN_ADDR_OFFSET).cast(), 4);
        }
        let name = ptr::addr_of!((*n).name).cast::<c_char>();
        nl_attr(h, L_IFA_LABEL, name.cast(), strlen(name) + 1);
    } else {
        let mut ip = [0u8; 16];
        ptr::copy_nonoverlapping(addr.add(SIN6_ADDR_OFFSET), ip.as_mut_ptr(), 16);
        am.family = L_AF_INET6;
        if !(*a).ifa_netmask.is_null() {
            am.prefixlen =
                prefix_len(((*a).ifa_netmask as *const u8).add(SIN6_ADDR_OFFSET), 16) as u8;
        }
        if ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80 {
            // KAME's embedded scope (the interface index): not on the wire, not in Linux
            ip[2] = 0;
            ip[3] = 0;
            am.scope = L_RT_SCOPE_LINK;
        } else if memcmp(
            ip.as_ptr().cast(),
            ptr::addr_of!(in6addr_loopback).cast(),
            16,
        ) == 0
        {
            am.scope = L_RT_SCOPE_HOST;
        }
        nl_put(h, ptr::addr_of!(am).cast(), size_of::<LIfaddrmsg>());
        if ptp {
            nl_attr(h, L_IFA_LOCAL, ip.as_ptr().cast(), 16);
            nl_attr(h, L_IFA_ADDRESS, dst.add(SIN6_ADDR_OFFSET).cast(), 16);
        } else {
            nl_attr(h, L_IFA_ADDRESS, ip.as_ptr().cast(), 16);
        }
    }
    nl_end(o, h);
}

unsafe fn put_done(o: *mut Nlout, seq: u32, pid: u32) {
    let h = nl_begin(o, 32, NLMSG_DONE_T, L_NLM_F_MULTI, seq, pid);
    if h.is_null() {
        return;
    }
    let zero: i32 = 0;
    nl_put(h, ptr::addr_of!(zero).cast(), 4);
    nl_end(o, h);
}

// `req` points at the request's header (possibly unaligned), `seq` is its
// sequence number.
unsafe fn put_error(o: *mut Nlout, req: *const u8, seq: u32, lerrno: c_int, pid: u32) {
    let h = nl_begin(o, 64, NLMSG_ERROR_T, 0, seq, pid);
    if h.is_null() {
        return;
    }
    let e: i32 = -lerrno;
    nl_put(h, ptr::addr_of!(e).cast(), 4);
    // the request's header, as the kernel echoes it
    nl_put(h, req.cast(), size_of::<LNlmsghdr>());
    nl_end(o, h);
}

const MAX_NETIFS: usize = 64;

// One request message. `reqp` may be unaligned inside the guest's buffer.
unsafe fn rtnl_one(o: *mut Nlout, reqp: *const u8, len: usize, pid: u32) {
    let req = reqp.cast::<LNlmsghdr>().read_unaligned();
    const HDR: usize = size_of::<LNlmsghdr>();
    if req.flags & L_NLM_F_REQUEST == 0 || req.type_ == NLMSG_NOOP_T {
        return;
    }
    let dump = (req.flags & L_NLM_F_DUMP) == L_NLM_F_DUMP;
    let family: u8 = if len > HDR { *reqp.add(HDR) } else { 0 };
    let mut ifs_storage = MaybeUninit::<[Netif; MAX_NETIFS]>::uninit();
    let ifs = ifs_storage.as_mut_ptr().cast::<Netif>();
    let mut list: *mut Ifaddrs = ptr::null_mut();

    if req.type_ == L_RTM_GETLINK {
        let n = host_netifs(ifs, MAX_NETIFS as c_int, &mut list);
        if dump {
            let mut i: c_int = 0;
            while i < n {
                put_link(o, ifs.add(i as usize), L_NLM_F_MULTI, req.seq, pid);
                i += 1;
            }
            put_done(o, req.seq, pid);
        } else {
            // One interface, by ifi_index or by IFLA_IFNAME.
            let mut want: i32 = 0;
            let mut wname: *const c_char = ptr::null();
            if len >= HDR + size_of::<LIfinfomsg>() {
                let ii = reqp.add(HDR).cast::<LIfinfomsg>().read_unaligned();
                want = ii.index;
                let mut off = HDR + size_of::<LIfinfomsg>();
                while off + size_of::<LRtattr>() <= len {
                    let rap = reqp.add(off);
                    let ra = rap.cast::<LRtattr>().read_unaligned();
                    if (ra.len as usize) < size_of::<LRtattr>() || off + ra.len as usize > len {
                        break;
                    }
                    if ra.type_ == L_IFLA_IFNAME {
                        wname = rap.add(size_of::<LRtattr>()).cast();
                    }
                    off += nl_align(ra.len as u32) as usize;
                }
            }
            let mut i: c_int = 0;
            while i < n {
                let f = ifs.add(i as usize);
                if (want > 0 && (*f).index as i32 == want)
                    || (want <= 0
                        && !wname.is_null()
                        && strncmp(ptr::addr_of!((*f).name).cast(), wname, IFNAMSIZ) == 0)
                {
                    break;
                }
                i += 1;
            }
            if i < n {
                put_link(o, ifs.add(i as usize), 0, req.seq, pid);
                if req.flags & L_NLM_F_ACK != 0 {
                    put_error(o, reqp, req.seq, 0, pid);
                }
            } else {
                put_error(o, reqp, req.seq, 19 /* ENODEV */, pid);
            }
        }
    } else if req.type_ == L_RTM_GETADDR && dump {
        let n = host_netifs(ifs, MAX_NETIFS as c_int, &mut list);
        let mut i: c_int = 0;
        while i < n {
            let f = ifs.add(i as usize);
            let mut a = list;
            while !a.is_null() {
                if (*a).ifa_addr.is_null()
                    || strcmp((*a).ifa_name, ptr::addr_of!((*f).name).cast()) != 0
                {
                    a = (*a).ifa_next;
                    continue;
                }
                let fam = (*(*a).ifa_addr).sa_family as c_int;
                if (fam == AF_INET && (family == 0 || family == L_AF_INET))
                    || (fam == AF_INET6 && (family == 0 || family == L_AF_INET6))
                {
                    put_addr(o, f, a, req.seq, pid);
                }
                a = (*a).ifa_next;
            }
            i += 1;
        }
        put_done(o, req.seq, pid);
    } else if dump {
        put_done(o, req.seq, pid); // routes, neighbours, rules...: an empty table
    } else {
        put_error(o, reqp, req.seq, 95 /* EOPNOTSUPP */, pid);
    }
    if !list.is_null() {
        freeifaddrs(list);
    }
}

static NLOUT_ZERO: Nlout = Nlout {
    peer: 0,
    buf: [0; NLOUT_BUF],
    used: 0,
};

#[no_mangle]
pub unsafe extern "C" fn lxrt_rtnl_request(peer: c_int, buf: *const c_void, len: usize) {
    let o = malloc(size_of::<Nlout>()).cast::<Nlout>();
    if o.is_null() {
        return;
    }
    ptr::copy_nonoverlapping(ptr::addr_of!(NLOUT_ZERO), o, 1);
    (*o).peer = peer;
    let pid = crate::ids::lxrt_ids_pid() as u32; // the port id getsockname reports
    let mut off: usize = 0;
    while off.wrapping_add(size_of::<LNlmsghdr>()) <= len {
        let hp = buf.cast::<u8>().add(off);
        let hlen = hp.cast::<u32>().read_unaligned();
        if (hlen as usize) < size_of::<LNlmsghdr>() || off.wrapping_add(hlen as usize) > len {
            break;
        }
        rtnl_one(o, hp, hlen as usize, pid);
        off = off.wrapping_add(nl_align(hlen) as usize);
    }
    nl_flush(o);
    free(o.cast());
}

// ---------------------------------------------------------------- SIOCGIF*

const L_SIOCGIFNAME: u32 = 0x8910;
const L_SIOCGIFFLAGS: u32 = 0x8913;
const L_SIOCGIFADDR: u32 = 0x8915;
const L_SIOCGIFBRDADDR: u32 = 0x8919;
const L_SIOCGIFNETMASK: u32 = 0x891b;
const L_SIOCGIFMTU: u32 = 0x8921;
const L_SIOCGIFHWADDR: u32 = 0x8927;
const L_SIOCGIFINDEX: u32 = 0x8933;

// struct ifreq: char ifr_name[16]; then a 24-byte union (sockaddr, short
// flags, int index/mtu).
#[repr(C)]
struct LIfreq {
    name: [c_char; 16],
    u: [u8; 24],
}

#[no_mangle]
pub unsafe extern "C" fn lxrt_netif_ioctl(
    fd: c_int,
    lreq: c_ulong,
    p: *mut c_void,
    ret: *mut c_long,
) -> bool {
    let lreq = lreq as u32;
    match lreq {
        L_SIOCGIFNAME | L_SIOCGIFFLAGS | L_SIOCGIFADDR | L_SIOCGIFBRDADDR | L_SIOCGIFNETMASK
        | L_SIOCGIFMTU | L_SIOCGIFHWADDR | L_SIOCGIFINDEX => {}
        _ => return false,
    }
    let mut st = MaybeUninit::<[u64; STAT_SIZE / 8]>::uninit();
    if fstat(fd, st.as_mut_ptr().cast()) != 0 {
        return false;
    }
    let mode = st
        .as_ptr()
        .cast::<u8>()
        .add(STAT_MODE_OFFSET)
        .cast::<u16>()
        .read_unaligned();
    if mode & S_IFMT != S_IFSOCK {
        return false;
    }
    if p.is_null() {
        *ret = lerr(EFAULT);
        return true;
    }
    let r = p.cast::<LIfreq>();
    let rname = ptr::addr_of_mut!((*r).name).cast::<c_char>();
    let ru = ptr::addr_of_mut!((*r).u).cast::<u8>();
    let mut ifs_storage = MaybeUninit::<[Netif; MAX_NETIFS]>::uninit();
    let ifs = ifs_storage.as_mut_ptr().cast::<Netif>();
    let mut list: *mut Ifaddrs = ptr::null_mut();
    let n = host_netifs(ifs, MAX_NETIFS as c_int, &mut list);
    let mut i: c_int = 0;
    if lreq == L_SIOCGIFNAME {
        let want = ru.cast::<i32>().read_unaligned();
        while i < n && (*ifs.add(i as usize)).index as i32 != want {
            i += 1;
        }
    } else {
        while i < n
            && strncmp(
                ptr::addr_of!((*ifs.add(i as usize)).name).cast(),
                rname,
                16,
            ) != 0
        {
            i += 1;
        }
    }
    'out: {
        if i == n {
            *ret = lerr(ENODEV);
            break 'out;
        }
        let f = ifs.add(i as usize) as *const Netif;
        let fname = ptr::addr_of!((*f).name).cast::<c_char>();
        *ret = 0;
        match lreq {
            L_SIOCGIFNAME => {
                ptr::write_bytes(rname, 0, 16);
                strncpy(rname, fname, 16 - 1);
            }
            L_SIOCGIFFLAGS => {
                let fl = (*f).lflags as u16; // ifr_flags is a short
                ru.cast::<u16>().write_unaligned(fl);
            }
            L_SIOCGIFMTU => ru.cast::<u32>().write_unaligned((*f).mtu),
            L_SIOCGIFINDEX => ru.cast::<u32>().write_unaligned((*f).index),
            L_SIOCGIFHWADDR => {
                ptr::write_bytes(ru, 0, 16);
                ru.cast::<u16>().write_unaligned((*f).arphrd); // sa_family carries the ARPHRD type
                if (*f).has_hw {
                    ptr::copy_nonoverlapping(ptr::addr_of!((*f).hw).cast::<u8>(), ru.add(2), 6);
                }
            }
            _ => {
                // ADDR, BRDADDR, NETMASK: the first IPv4 address
                let mut sa: *const Sockaddr = ptr::null();
                let mut a = list;
                while !a.is_null() && sa.is_null() {
                    if !(*a).ifa_addr.is_null()
                        && (*(*a).ifa_addr).sa_family as c_int == AF_INET
                        && strcmp((*a).ifa_name, fname) == 0
                    {
                        sa = if lreq == L_SIOCGIFADDR {
                            (*a).ifa_addr
                        } else if lreq == L_SIOCGIFNETMASK {
                            (*a).ifa_netmask
                        } else {
                            (*a).ifa_dstaddr // ifa_broadaddr
                        };
                    }
                    a = (*a).ifa_next;
                }
                if sa.is_null() {
                    *ret = lerr(EADDRNOTAVAIL);
                    break 'out;
                }
                let mut lsin = [0u8; 16]; // struct sockaddr_in, Linux layout
                let fam: u16 = L_AF_INET as u16;
                ptr::copy_nonoverlapping(ptr::addr_of!(fam).cast::<u8>(), lsin.as_mut_ptr(), 2);
                ptr::copy_nonoverlapping(
                    sa.cast::<u8>().add(SIN_ADDR_OFFSET),
                    lsin.as_mut_ptr().add(4),
                    4,
                );
                ptr::copy_nonoverlapping(lsin.as_ptr(), ru, 16);
            }
        }
    }
    if !list.is_null() {
        freeifaddrs(list);
    }
    true
}

// ---------------------------------------------------------------- /proc/net
//
// /proc/net/route, ipv6_route, arp and dev, from Darwin's routing table
// (sysctl NET_RT_DUMP / NET_RT_FLAGS) and interfaces. Wine's nsiproxy reads
// them for GetIpForwardTable/GetBestRoute, GetIpNetTable and the interface
// counters; without route, the IPv4 forward table answered
// STATUS_NOT_SUPPORTED and Minecraft Dungeons II's protection killed the game
// on that answer (benchmarks/stage62, 15).

fn rt_roundup(a: usize) -> usize {
    if a > 0 {
        1 + ((a - 1) | (size_of::<u32>() - 1))
    } else {
        size_of::<u32>()
    }
}

// The head of a routing message, read from wherever it sits in the dump.
#[derive(Clone, Copy)]
struct RtHead {
    msglen: u16,
    index: u16,
    flags: c_int,
    addrs: c_int,
}

unsafe fn rt_head(m: *const RtMsghdr) -> RtHead {
    RtHead {
        msglen: ptr::addr_of!((*m).rtm_msglen).read_unaligned(),
        index: ptr::addr_of!((*m).rtm_index).read_unaligned(),
        flags: ptr::addr_of!((*m).rtm_flags).read_unaligned(),
        addrs: ptr::addr_of!((*m).rtm_addrs).read_unaligned(),
    }
}

// The routing messages for family `af` (NET_RT_DUMP, or NET_RT_FLAGS with
// `flags`); malloc'd, *len its size.
unsafe fn rt_dump(af: c_int, op: c_int, flags: c_int, len: *mut usize) -> *mut u8 {
    let mut mib: [c_int; 6] = [CTL_NET, PF_ROUTE, 0, af, op, flags];
    let mut n: usize = 0;
    if sysctl(
        mib.as_mut_ptr(),
        6,
        ptr::null_mut(),
        &mut n,
        ptr::null_mut(),
        0,
    ) != 0
    {
        return ptr::null_mut();
    }
    n = n.wrapping_add(n / 4 + 4096); // the table can grow between the two calls
    let b = malloc(n).cast::<u8>();
    if !b.is_null() && sysctl(mib.as_mut_ptr(), 6, b.cast(), &mut n, ptr::null_mut(), 0) != 0 {
        free(b.cast());
        return ptr::null_mut();
    }
    *len = n;
    b
}

// The sockaddrs after a routing message header, indexed by RTAX_*.
unsafe fn rt_addrs(m: *const RtMsghdr, head: &RtHead, sa: *mut *const Sockaddr) {
    let mut p = m.cast::<u8>().add(size_of::<RtMsghdr>());
    let end = m.cast::<u8>().add(head.msglen as usize);
    let mut i = 0usize;
    while i < RTAX_MAX {
        *sa.add(i) = ptr::null();
        if head.addrs & (1 << i) != 0 && p < end {
            let s = p.cast::<Sockaddr>();
            *sa.add(i) = s;
            p = p.add(rt_roundup((*s).sa_len as usize));
        }
        i += 1;
    }
}

// A (possibly truncated) routing-socket netmask as a full address.
unsafe fn rt_mask(sa: *const Sockaddr, out: *mut u8, addr_off: usize, addr_len: usize) {
    ptr::write_bytes(out, 0, addr_len);
    if sa.is_null() || (*sa).sa_len as usize <= addr_off {
        return;
    }
    let have = (*sa).sa_len as usize - addr_off;
    ptr::copy_nonoverlapping(
        sa.cast::<u8>().add(addr_off),
        out,
        if have < addr_len { have } else { addr_len },
    );
}

unsafe fn rt_skip(head: &RtHead, ifname: *mut c_char) -> bool {
    if head.flags & RTF_UP == 0
        || head.flags & (RTF_LLINFO | RTF_WASCLONED | RTF_BROADCAST | RTF_MULTICAST | RTF_IFSCOPE)
            != 0
    {
        return true;
    }
    if if_indextoname(head.index as c_uint, ifname).is_null() {
        return true;
    }
    // loopback routes: Wine adds its own, Linux keeps them elsewhere
    strncmp(ifname, c"lo".as_ptr(), 2) == 0
}

unsafe fn family_of(sa: *const Sockaddr) -> c_int {
    (*sa).sa_family as c_int
}

unsafe fn gen_route(buf: *mut c_char, cap: usize) -> usize {
    let mut o = snprintf(
        buf,
        cap,
        c"Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
            .as_ptr(),
    ) as usize;
    let mut len: usize = 0;
    let d = rt_dump(AF_INET, NET_RT_DUMP, 0, &mut len);
    let mut at: usize = 0;
    while !d.is_null() && at.wrapping_add(size_of::<RtMsghdr>()) <= len && o < cap {
        let m = d.add(at).cast::<RtMsghdr>();
        let head = rt_head(m);
        if head.msglen == 0 {
            break;
        }
        at = at.wrapping_add(head.msglen as usize);
        let mut sa: [*const Sockaddr; RTAX_MAX] = [ptr::null(); RTAX_MAX];
        let mut ifname: [c_char; IF_NAMESIZE] = [0; IF_NAMESIZE];
        rt_addrs(m, &head, sa.as_mut_ptr());
        if rt_skip(&head, ifname.as_mut_ptr())
            || sa[RTAX_DST].is_null()
            || family_of(sa[RTAX_DST]) != AF_INET
        {
            continue;
        }
        let dst: u32 = sa[RTAX_DST]
            .cast::<u8>()
            .add(SIN_ADDR_OFFSET)
            .cast::<u32>()
            .read_unaligned();
        let mut gw: u32 = 0;
        let mut mask: u32 = 0;
        let mut fl: c_uint = 0x1; // RTF_UP
        if !sa[RTAX_GATEWAY].is_null()
            && family_of(sa[RTAX_GATEWAY]) == AF_INET
            && head.flags & RTF_GATEWAY != 0
        {
            gw = sa[RTAX_GATEWAY]
                .cast::<u8>()
                .add(SIN_ADDR_OFFSET)
                .cast::<u32>()
                .read_unaligned();
            fl |= 0x2; // RTF_GATEWAY
        }
        if head.flags & RTF_HOST != 0 {
            mask = 0xffff_ffff;
            fl |= 0x4; // RTF_HOST
        } else {
            rt_mask(
                sa[RTAX_NETMASK],
                ptr::addr_of_mut!(mask).cast(),
                SIN_ADDR_OFFSET,
                4,
            );
        }
        o = o.wrapping_add(snprintf(
            buf.add(o),
            cap - o,
            c"%s\t%08X\t%08X\t%04X\t0\t0\t0\t%08X\t0\t0\t0\n".as_ptr(),
            ifname.as_ptr(),
            dst as c_uint,
            gw as c_uint,
            fl,
            mask as c_uint,
        ) as usize);
    }
    free(d.cast());
    if o < cap {
        o
    } else {
        cap
    }
}

// 32 hex digits and a terminator, as sprintf("%02x") per byte leaves them.
unsafe fn hex6(out: *mut c_char, a: *const u8) {
    const DIGITS: &[u8; 16] = b"0123456789abcdef";
    let mut i = 0usize;
    while i < 16 {
        let b = *a.add(i);
        *out.add(2 * i) = DIGITS[(b >> 4) as usize] as c_char;
        *out.add(2 * i + 1) = DIGITS[(b & 0xf) as usize] as c_char;
        i += 1;
    }
    *out.add(32) = 0;
}

static IN6_ZERO: [u8; 16] = [0; 16];

unsafe fn gen_ipv6_route(buf: *mut c_char, cap: usize) -> usize {
    let mut o: usize = 0;
    let mut len: usize = 0;
    let d = rt_dump(AF_INET6, NET_RT_DUMP, 0, &mut len);
    let mut at: usize = 0;
    while !d.is_null() && at.wrapping_add(size_of::<RtMsghdr>()) <= len && o < cap {
        let m = d.add(at).cast::<RtMsghdr>();
        let head = rt_head(m);
        if head.msglen == 0 {
            break;
        }
        at = at.wrapping_add(head.msglen as usize);
        let mut sa: [*const Sockaddr; RTAX_MAX] = [ptr::null(); RTAX_MAX];
        let mut ifname: [c_char; IF_NAMESIZE] = [0; IF_NAMESIZE];
        rt_addrs(m, &head, sa.as_mut_ptr());
        if rt_skip(&head, ifname.as_mut_ptr())
            || sa[RTAX_DST].is_null()
            || family_of(sa[RTAX_DST]) != AF_INET6
        {
            continue;
        }
        let mut dst = [0u8; 16];
        let mut gw = [0u8; 16];
        let mut mask = [0u8; 16];
        ptr::copy_nonoverlapping(
            sa[RTAX_DST].cast::<u8>().add(SIN6_ADDR_OFFSET),
            dst.as_mut_ptr(),
            16,
        );
        if dst[0] == 0xfe && (dst[1] & 0xc0) == 0x80 {
            // KAME's embedded scope
            dst[2] = 0;
            dst[3] = 0;
        }
        let mut fl: c_uint = 0x1;
        if !sa[RTAX_GATEWAY].is_null()
            && family_of(sa[RTAX_GATEWAY]) == AF_INET6
            && head.flags & RTF_GATEWAY != 0
        {
            ptr::copy_nonoverlapping(
                sa[RTAX_GATEWAY].cast::<u8>().add(SIN6_ADDR_OFFSET),
                gw.as_mut_ptr(),
                16,
            );
            if gw[0] == 0xfe && (gw[1] & 0xc0) == 0x80 {
                gw[2] = 0;
                gw[3] = 0;
            }
            fl |= 0x2;
        }
        let mut plen: c_int = 128;
        if head.flags & RTF_HOST == 0 {
            rt_mask(sa[RTAX_NETMASK], mask.as_mut_ptr(), SIN6_ADDR_OFFSET, 16);
            plen = if !sa[RTAX_NETMASK].is_null() {
                prefix_len(mask.as_ptr(), 16)
            } else {
                0
            };
        } else {
            fl |= 0x4;
        }
        let mut hd: [c_char; 33] = [0; 33];
        let mut hz: [c_char; 33] = [0; 33];
        let mut hg: [c_char; 33] = [0; 33];
        hex6(hd.as_mut_ptr(), dst.as_ptr());
        hex6(hz.as_mut_ptr(), IN6_ZERO.as_ptr());
        hex6(hg.as_mut_ptr(), gw.as_ptr());
        o = o.wrapping_add(snprintf(
            buf.add(o),
            cap - o,
            c"%s %02x %s 00 %s %08x 00000000 00000000 %08x %8s\n".as_ptr(),
            hd.as_ptr(),
            plen,
            hz.as_ptr(),
            hg.as_ptr(),
            256 as c_int,
            fl,
            ifname.as_ptr(),
        ) as usize);
    }
    free(d.cast());
    if o < cap {
        o
    } else {
        cap
    }
}

unsafe fn gen_arp(buf: *mut c_char, cap: usize) -> usize {
    let mut o = snprintf(
        buf,
        cap,
        c"IP address       HW type     Flags       HW address            Mask     Device\n".as_ptr(),
    ) as usize;
    let mut len: usize = 0;
    let d = rt_dump(AF_INET, NET_RT_FLAGS, RTF_LLINFO, &mut len);
    let mut at: usize = 0;
    while !d.is_null() && at.wrapping_add(size_of::<RtMsghdr>()) <= len && o < cap {
        let m = d.add(at).cast::<RtMsghdr>();
        let head = rt_head(m);
        if head.msglen == 0 {
            break;
        }
        at = at.wrapping_add(head.msglen as usize);
        let mut sa: [*const Sockaddr; RTAX_MAX] = [ptr::null(); RTAX_MAX];
        let mut ifname: [c_char; IF_NAMESIZE] = [0; IF_NAMESIZE];
        rt_addrs(m, &head, sa.as_mut_ptr());
        if sa[RTAX_DST].is_null()
            || family_of(sa[RTAX_DST]) != AF_INET
            || sa[RTAX_GATEWAY].is_null()
            || family_of(sa[RTAX_GATEWAY]) != AF_LINK
            || if_indextoname(head.index as c_uint, ifname.as_mut_ptr()).is_null()
        {
            continue;
        }
        let dl = sa[RTAX_GATEWAY].cast::<SockaddrDl>();
        if (*dl).sdl_alen != 6 {
            continue; // incomplete entry
        }
        let hw = lladdr(dl);
        let mut ip: [c_char; 16] = [0; 16];
        let mut mac: [c_char; 18] = [0; 18];
        inet_ntop(
            AF_INET,
            sa[RTAX_DST].cast::<u8>().add(SIN_ADDR_OFFSET).cast(),
            ip.as_mut_ptr(),
            16,
        );
        snprintf(
            mac.as_mut_ptr(),
            18,
            c"%02x:%02x:%02x:%02x:%02x:%02x".as_ptr(),
            *hw as c_uint,
            *hw.add(1) as c_uint,
            *hw.add(2) as c_uint,
            *hw.add(3) as c_uint,
            *hw.add(4) as c_uint,
            *hw.add(5) as c_uint,
        );
        // ATF_COM, ATF_PERM
        let fl: c_uint = 0x2 | if head.flags & RTF_STATIC != 0 { 0x4 } else { 0 };
        o = o.wrapping_add(snprintf(
            buf.add(o),
            cap - o,
            c"%-16s 0x1         0x%-9x %-21s *        %s\n".as_ptr(),
            ip.as_ptr(),
            fl,
            mac.as_ptr(),
            ifname.as_ptr(),
        ) as usize);
    }
    free(d.cast());
    if o < cap {
        o
    } else {
        cap
    }
}

unsafe fn gen_dev(buf: *mut c_char, cap: usize) -> usize {
    let mut o = snprintf(
        buf,
        cap,
        c"Inter-|   Receive                                                |  Transmit\n face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
            .as_ptr(),
    ) as usize;
    let mut ifs_storage = MaybeUninit::<[Netif; MAX_NETIFS]>::uninit();
    let ifs = ifs_storage.as_mut_ptr().cast::<Netif>();
    let mut list: *mut Ifaddrs = ptr::null_mut();
    let n = host_netifs(ifs, MAX_NETIFS as c_int, &mut list);
    let mut i: c_int = 0;
    while i < n && o < cap {
        let f = ifs.add(i as usize);
        o = o.wrapping_add(snprintf(
            buf.add(o),
            cap - o,
            c"%6s: %7u %7u %4u    0    0     0          0         0 %8u %7u %4u    0    0     0       0          0\n"
                .as_ptr(),
            ptr::addr_of!((*f).name).cast::<c_char>(),
            (*f).stats[2] as c_uint,
            (*f).stats[0] as c_uint,
            (*f).stats[4] as c_uint,
            (*f).stats[3] as c_uint,
            (*f).stats[1] as c_uint,
            (*f).stats[5] as c_uint,
        ) as usize);
        i += 1;
    }
    if !list.is_null() {
        freeifaddrs(list);
    }
    if o < cap {
        o
    } else {
        cap
    }
}

// The contents of /proc/net/<name> for the files above into buf; -1 when
// `name` is not one of them.
#[no_mangle]
pub unsafe extern "C" fn lxrt_netif_procnet(
    name: *const c_char,
    buf: *mut c_char,
    cap: usize,
) -> c_long {
    if strcmp(name, c"route".as_ptr()) == 0 {
        return gen_route(buf, cap) as c_long;
    }
    if strcmp(name, c"ipv6_route".as_ptr()) == 0 {
        return gen_ipv6_route(buf, cap) as c_long;
    }
    if strcmp(name, c"arp".as_ptr()) == 0 {
        return gen_arp(buf, cap) as c_long;
    }
    if strcmp(name, c"dev".as_ptr()) == 0 {
        return gen_dev(buf, cap) as c_long;
    }
    -1
}
