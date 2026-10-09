// netif.c -- the Mac's network interfaces as a Linux guest asks for them:
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
#include "lxrt.h"
#include "ids.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stddef.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>

#define LERR(e) (-lxrt_errno_to_linux(e))

// ---------------------------------------------------------------- Linux ABI

#define L_AF_INET   2
#define L_AF_INET6  10
#define L_AF_PACKET 17

#define L_ARPHRD_ETHER    1
#define L_ARPHRD_LOOPBACK 772
#define L_ARPHRD_NONE     65534

#define L_IFF_UP          0x1
#define L_IFF_BROADCAST   0x2
#define L_IFF_LOOPBACK    0x8
#define L_IFF_POINTOPOINT 0x10
#define L_IFF_RUNNING     0x40
#define L_IFF_MULTICAST   0x1000
#define L_IFF_LOWER_UP    0x10000

#define NLMSG_NOOP_T  1
#define NLMSG_ERROR_T 2
#define NLMSG_DONE_T  3
#define L_NLM_F_REQUEST 0x1
#define L_NLM_F_MULTI   0x2
#define L_NLM_F_ACK     0x4
#define L_NLM_F_DUMP    0x300

#define L_RTM_NEWLINK 16
#define L_RTM_GETLINK 18
#define L_RTM_NEWADDR 20
#define L_RTM_GETADDR 22

#define L_IFLA_ADDRESS   1
#define L_IFLA_BROADCAST 2
#define L_IFLA_IFNAME    3
#define L_IFLA_MTU       4
#define L_IFLA_STATS     7
#define L_IFLA_TXQLEN    13
#define L_IFLA_OPERSTATE 16

#define L_IFA_ADDRESS   1
#define L_IFA_LOCAL     2
#define L_IFA_LABEL     3
#define L_IFA_BROADCAST 4
#define L_IFA_F_PERMANENT 0x80

#define L_RT_SCOPE_UNIVERSE 0
#define L_RT_SCOPE_LINK     253
#define L_RT_SCOPE_HOST     254

#define L_IF_OPER_UNKNOWN 0
#define L_IF_OPER_DOWN    2
#define L_IF_OPER_UP      6

struct l_nlmsghdr { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct l_ifinfomsg { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; };
struct l_ifaddrmsg { uint8_t family, prefixlen, flags, scope; uint32_t index; };
struct l_rtattr { uint16_t len, type; };

#define NL_ALIGN(n) (((n) + 3u) & ~3u)

// ---------------------------------------------------------------- the host

struct netif {
    char name[IFNAMSIZ];
    uint32_t index;
    uint32_t lflags;            // Linux IFF_*
    uint16_t arphrd;
    uint8_t hw[6];
    bool has_hw;
    uint32_t mtu;
    uint32_t stats[6];          // rx/tx packets, rx/tx bytes, rx/tx errors
};

// Darwin's IFF_* share Linux's values up to IFF_ALLMULTI (0x200); above that
// they differ (Darwin's OACTIVE 0x400 is Linux's MASTER, its MULTICAST 0x8000
// is Linux's DYNAMIC).
static uint32_t flags_to_linux(unsigned df)
{
    uint32_t f = df & 0x3ff;
    if (df & IFF_MULTICAST)
        f |= L_IFF_MULTICAST;
    if (df & IFF_RUNNING)
        f |= L_IFF_LOWER_UP;
    return f;
}

static void fill_link(struct netif *n, const struct ifaddrs *a)
{
    const struct sockaddr_dl *dl = (const struct sockaddr_dl *)a->ifa_addr;
    if (dl->sdl_type == IFT_ETHER && dl->sdl_alen == 6) {
        n->arphrd = L_ARPHRD_ETHER;
        memcpy(n->hw, LLADDR(dl), 6);
        n->has_hw = true;
    }
    const struct if_data *d = a->ifa_data;
    if (d) {
        n->mtu = d->ifi_mtu;
        n->stats[0] = d->ifi_ipackets;
        n->stats[1] = d->ifi_opackets;
        n->stats[2] = d->ifi_ibytes;
        n->stats[3] = d->ifi_obytes;
        n->stats[4] = d->ifi_ierrors;
        n->stats[5] = d->ifi_oerrors;
    }
}

// The interfaces, in Darwin's index order; *ap keeps getifaddrs' list for
// the addresses (freeifaddrs by the caller). Returns the count.
static int host_netifs(struct netif *out, int cap, struct ifaddrs **ap)
{
    *ap = NULL;
    if (getifaddrs(ap) != 0)
        return 0;
    int n = 0;
    for (struct ifaddrs *a = *ap; a; a = a->ifa_next) {
        if (!a->ifa_name || strlen(a->ifa_name) >= IFNAMSIZ)
            continue;
        int i = 0;
        while (i < n && strcmp(out[i].name, a->ifa_name))
            i++;
        if (i == n) {
            if (n == cap)
                continue;
            memset(&out[n], 0, sizeof out[n]);
            strcpy(out[n].name, a->ifa_name);
            out[n].index = if_nametoindex(a->ifa_name);
            out[n].lflags = flags_to_linux(a->ifa_flags);
            out[n].arphrd = (a->ifa_flags & IFF_LOOPBACK) ? L_ARPHRD_LOOPBACK : L_ARPHRD_NONE;
            out[n].mtu = 1500;
            n++;
        }
        if (a->ifa_addr && a->ifa_addr->sa_family == AF_LINK)
            fill_link(&out[i], a);
    }
    for (int i = 1; i < n; i++)             // index order, as Linux dumps them
        for (int j = i; j > 0 && out[j - 1].index > out[j].index; j--) {
            struct netif t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return n;
}

// ---------------------------------------------------------------- replies

// Reply datagrams, at most 4096 bytes each: glibc's getifaddrs() and
// Chromium's address tracker receive into one page, and a longer datagram
// arrives truncated (MSG_TRUNC), which glibc treats as failure.
struct nlout {
    int peer;
    uint8_t buf[4096];
    size_t used;
};

static void nl_flush(struct nlout *o)
{
    if (o->used)
        (void)send(o->peer, o->buf, o->used, MSG_DONTWAIT);   // a full queue drops it, as ENOBUFS would
    o->used = 0;
}

// Starts a message of room `need` bytes, flushing first when it does not fit.
static struct l_nlmsghdr *nl_begin(struct nlout *o, size_t need, uint16_t type, uint16_t flags,
                                   uint32_t seq, uint32_t pid)
{
    if (need > sizeof o->buf)
        return NULL;
    if (o->used + need > sizeof o->buf)
        nl_flush(o);
    struct l_nlmsghdr *h = (struct l_nlmsghdr *)(o->buf + o->used);
    memset(h, 0, need);
    h->len = sizeof *h;
    h->type = type;
    h->flags = flags;
    h->seq = seq;
    h->pid = pid;
    return h;
}

static void *nl_put(struct l_nlmsghdr *h, const void *data, size_t len)
{
    uint8_t *p = (uint8_t *)h + h->len;
    memcpy(p, data, len);
    h->len += NL_ALIGN((uint32_t)len);
    return p;
}

static void nl_attr(struct l_nlmsghdr *h, uint16_t type, const void *data, size_t len)
{
    struct l_rtattr a = { (uint16_t)(sizeof a + len), type };
    uint8_t *p = (uint8_t *)h + h->len;
    memcpy(p, &a, sizeof a);
    memcpy(p + sizeof a, data, len);
    h->len += NL_ALIGN((uint32_t)(sizeof a + len));
}

static void nl_end(struct nlout *o, struct l_nlmsghdr *h)
{
    o->used += NL_ALIGN(h->len);
}

static void put_link(struct nlout *o, const struct netif *n, uint16_t flags, uint32_t seq, uint32_t pid)
{
    struct l_nlmsghdr *h = nl_begin(o, 512, L_RTM_NEWLINK, flags, seq, pid);
    if (!h)
        return;
    struct l_ifinfomsg ii = { 0, 0, n->arphrd, (int32_t)n->index, n->lflags, 0 };
    nl_put(h, &ii, sizeof ii);
    nl_attr(h, L_IFLA_IFNAME, n->name, strlen(n->name) + 1);
    if (n->has_hw || n->arphrd == L_ARPHRD_LOOPBACK) {
        static const uint8_t zero[6], bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        nl_attr(h, L_IFLA_ADDRESS, n->has_hw ? n->hw : zero, 6);
        nl_attr(h, L_IFLA_BROADCAST, n->has_hw ? bcast : zero, 6);
    }
    nl_attr(h, L_IFLA_MTU, &n->mtu, 4);
    uint32_t txq = n->arphrd == L_ARPHRD_LOOPBACK ? 0 : 1000;
    nl_attr(h, L_IFLA_TXQLEN, &txq, 4);
    uint8_t oper = n->arphrd == L_ARPHRD_LOOPBACK ? L_IF_OPER_UNKNOWN
                 : (n->lflags & L_IFF_RUNNING) ? L_IF_OPER_UP : L_IF_OPER_DOWN;
    nl_attr(h, L_IFLA_OPERSTATE, &oper, 1);
    uint32_t st[23] = {0};      // struct rtnl_link_stats
    st[0] = n->stats[0]; st[1] = n->stats[1]; st[2] = n->stats[2];
    st[3] = n->stats[3]; st[4] = n->stats[4]; st[5] = n->stats[5];
    nl_attr(h, L_IFLA_STATS, st, sizeof st);
    nl_end(o, h);
}

static int prefix_len(const uint8_t *mask, int bytes)
{
    int n = 0;
    for (int i = 0; i < bytes; i++)
        n += __builtin_popcount(mask[i]);
    return n;
}

static void put_addr(struct nlout *o, const struct netif *n, const struct ifaddrs *a,
                     uint32_t seq, uint32_t pid)
{
    struct l_ifaddrmsg am = { 0, 0, L_IFA_F_PERMANENT, L_RT_SCOPE_UNIVERSE, n->index };
    struct l_nlmsghdr *h = nl_begin(o, 256, L_RTM_NEWADDR, L_NLM_F_MULTI, seq, pid);
    if (!h)
        return;
    bool ptp = (a->ifa_flags & IFF_POINTOPOINT) && a->ifa_dstaddr &&
               a->ifa_dstaddr->sa_family == a->ifa_addr->sa_family;
    if (a->ifa_addr->sa_family == AF_INET) {
        const uint8_t *ip = (const uint8_t *)&((const struct sockaddr_in *)a->ifa_addr)->sin_addr;
        am.family = L_AF_INET;
        if (a->ifa_netmask)
            am.prefixlen = (uint8_t)prefix_len(
                (const uint8_t *)&((const struct sockaddr_in *)a->ifa_netmask)->sin_addr, 4);
        if (ip[0] == 127)
            am.scope = L_RT_SCOPE_HOST;
        else if (ip[0] == 169 && ip[1] == 254)
            am.scope = L_RT_SCOPE_LINK;
        nl_put(h, &am, sizeof am);
        const void *peer = ptp ? (const void *)&((const struct sockaddr_in *)a->ifa_dstaddr)->sin_addr : ip;
        nl_attr(h, L_IFA_ADDRESS, peer, 4);
        nl_attr(h, L_IFA_LOCAL, ip, 4);
        if (!ptp && (a->ifa_flags & IFF_BROADCAST) && a->ifa_broadaddr &&
            a->ifa_broadaddr->sa_family == AF_INET)
            nl_attr(h, L_IFA_BROADCAST, &((const struct sockaddr_in *)a->ifa_broadaddr)->sin_addr, 4);
        nl_attr(h, L_IFA_LABEL, n->name, strlen(n->name) + 1);
    } else {
        uint8_t ip[16];
        memcpy(ip, &((const struct sockaddr_in6 *)a->ifa_addr)->sin6_addr, 16);
        am.family = L_AF_INET6;
        if (a->ifa_netmask)
            am.prefixlen = (uint8_t)prefix_len(
                (const uint8_t *)&((const struct sockaddr_in6 *)a->ifa_netmask)->sin6_addr, 16);
        if (ip[0] == 0xfe && (ip[1] & 0xc0) == 0x80) {
            ip[2] = ip[3] = 0;  // KAME's embedded scope (the interface index): not on the wire, not in Linux
            am.scope = L_RT_SCOPE_LINK;
        } else if (!memcmp(ip, &in6addr_loopback, 16)) {
            am.scope = L_RT_SCOPE_HOST;
        }
        nl_put(h, &am, sizeof am);
        if (ptp) {
            nl_attr(h, L_IFA_LOCAL, ip, 16);
            nl_attr(h, L_IFA_ADDRESS, &((const struct sockaddr_in6 *)a->ifa_dstaddr)->sin6_addr, 16);
        } else {
            nl_attr(h, L_IFA_ADDRESS, ip, 16);
        }
    }
    nl_end(o, h);
}

static void put_done(struct nlout *o, uint32_t seq, uint32_t pid)
{
    struct l_nlmsghdr *h = nl_begin(o, 32, NLMSG_DONE_T, L_NLM_F_MULTI, seq, pid);
    if (!h)
        return;
    int32_t zero = 0;
    nl_put(h, &zero, 4);
    nl_end(o, h);
}

static void put_error(struct nlout *o, const struct l_nlmsghdr *req, int lerrno, uint32_t pid)
{
    struct l_nlmsghdr *h = nl_begin(o, 64, NLMSG_ERROR_T, 0, req->seq, pid);
    if (!h)
        return;
    int32_t e = -lerrno;
    nl_put(h, &e, 4);
    nl_put(h, req, sizeof *req);    // the request's header, as the kernel echoes it
    nl_end(o, h);
}

#define MAX_NETIFS 64

// One request message.
static void rtnl_one(struct nlout *o, const struct l_nlmsghdr *req, size_t len, uint32_t pid)
{
    if (!(req->flags & L_NLM_F_REQUEST) || req->type == NLMSG_NOOP_T)
        return;
    bool dump = (req->flags & L_NLM_F_DUMP) == L_NLM_F_DUMP;
    uint8_t family = len > sizeof *req ? *((const uint8_t *)req + sizeof *req) : 0;
    struct netif ifs[MAX_NETIFS];
    struct ifaddrs *list = NULL;

    if (req->type == L_RTM_GETLINK) {
        int n = host_netifs(ifs, MAX_NETIFS, &list);
        if (dump) {
            for (int i = 0; i < n; i++)
                put_link(o, &ifs[i], L_NLM_F_MULTI, req->seq, pid);
            put_done(o, req->seq, pid);
        } else {
            // One interface, by ifi_index or by IFLA_IFNAME.
            int32_t want = 0;
            const char *wname = NULL;
            if (len >= sizeof *req + sizeof(struct l_ifinfomsg)) {
                const struct l_ifinfomsg *ii = (const void *)((const uint8_t *)req + sizeof *req);
                want = ii->index;
                size_t off = sizeof *req + sizeof *ii;
                while (off + sizeof(struct l_rtattr) <= len) {
                    const struct l_rtattr *ra = (const void *)((const uint8_t *)req + off);
                    if (ra->len < sizeof *ra || off + ra->len > len)
                        break;
                    if (ra->type == L_IFLA_IFNAME)
                        wname = (const char *)(ra + 1);
                    off += NL_ALIGN(ra->len);
                }
            }
            int i = 0;
            while (i < n && !((want > 0 && (int32_t)ifs[i].index == want) ||
                              (want <= 0 && wname && !strncmp(ifs[i].name, wname, IFNAMSIZ))))
                i++;
            if (i < n) {
                put_link(o, &ifs[i], 0, req->seq, pid);
                if (req->flags & L_NLM_F_ACK)
                    put_error(o, req, 0, pid);
            } else {
                put_error(o, req, 19 /* ENODEV */, pid);
            }
        }
    } else if (req->type == L_RTM_GETADDR && dump) {
        int n = host_netifs(ifs, MAX_NETIFS, &list);
        for (int i = 0; i < n; i++)
            for (struct ifaddrs *a = list; a; a = a->ifa_next) {
                if (!a->ifa_addr || strcmp(a->ifa_name, ifs[i].name))
                    continue;
                int f = a->ifa_addr->sa_family;
                if ((f == AF_INET && (family == 0 || family == L_AF_INET)) ||
                    (f == AF_INET6 && (family == 0 || family == L_AF_INET6)))
                    put_addr(o, &ifs[i], a, req->seq, pid);
            }
        put_done(o, req->seq, pid);
    } else if (dump) {
        put_done(o, req->seq, pid);     // routes, neighbours, rules...: an empty table
    } else {
        put_error(o, req, 95 /* EOPNOTSUPP */, pid);
    }
    if (list)
        freeifaddrs(list);
}

void lxrt_rtnl_request(int peer, const void *buf, size_t len)
{
    static struct nlout zero;
    struct nlout *o = malloc(sizeof *o);
    if (!o)
        return;
    *o = zero;
    o->peer = peer;
    uint32_t pid = (uint32_t)lxrt_ids_pid();    // the port id getsockname reports
    size_t off = 0;
    while (off + sizeof(struct l_nlmsghdr) <= len) {
        const struct l_nlmsghdr *h = (const void *)((const uint8_t *)buf + off);
        if (h->len < sizeof *h || off + h->len > len)
            break;
        rtnl_one(o, h, h->len, pid);
        off += NL_ALIGN(h->len);
    }
    nl_flush(o);
    free(o);
}

// ---------------------------------------------------------------- SIOCGIF*

#define L_SIOCGIFNAME    0x8910
#define L_SIOCGIFFLAGS   0x8913
#define L_SIOCGIFADDR    0x8915
#define L_SIOCGIFBRDADDR 0x8919
#define L_SIOCGIFNETMASK 0x891b
#define L_SIOCGIFMTU     0x8921
#define L_SIOCGIFHWADDR  0x8927
#define L_SIOCGIFINDEX   0x8933

// struct ifreq: char ifr_name[16]; then a 24-byte union (sockaddr, short
// flags, int index/mtu).
struct l_ifreq { char name[16]; uint8_t u[24]; };

bool lxrt_netif_ioctl(int fd, unsigned long lreq, void *p, long *ret)
{
    switch ((uint32_t)lreq) {
    case L_SIOCGIFNAME: case L_SIOCGIFFLAGS: case L_SIOCGIFADDR: case L_SIOCGIFBRDADDR:
    case L_SIOCGIFNETMASK: case L_SIOCGIFMTU: case L_SIOCGIFHWADDR: case L_SIOCGIFINDEX:
        break;
    default:
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISSOCK(st.st_mode))
        return false;
    if (!p) {
        *ret = LERR(EFAULT);
        return true;
    }
    struct l_ifreq *r = p;
    struct netif ifs[MAX_NETIFS];
    struct ifaddrs *list = NULL;
    int n = host_netifs(ifs, MAX_NETIFS, &list);
    int i = 0;
    if ((uint32_t)lreq == L_SIOCGIFNAME) {
        int32_t want;
        memcpy(&want, r->u, 4);
        while (i < n && (int32_t)ifs[i].index != want)
            i++;
    } else {
        while (i < n && strncmp(ifs[i].name, r->name, sizeof r->name))
            i++;
    }
    if (i == n) {
        *ret = LERR(ENODEV);
        goto out;
    }
    const struct netif *f = &ifs[i];
    *ret = 0;
    switch ((uint32_t)lreq) {
    case L_SIOCGIFNAME:
        memset(r->name, 0, sizeof r->name);
        strncpy(r->name, f->name, sizeof r->name - 1);
        break;
    case L_SIOCGIFFLAGS: {
        uint16_t fl = (uint16_t)f->lflags;      // ifr_flags is a short
        memcpy(r->u, &fl, 2);
        break;
    }
    case L_SIOCGIFMTU:
        memcpy(r->u, &f->mtu, 4);
        break;
    case L_SIOCGIFINDEX:
        memcpy(r->u, &f->index, 4);
        break;
    case L_SIOCGIFHWADDR: {
        memset(r->u, 0, 16);
        memcpy(r->u, &f->arphrd, 2);            // sa_family carries the ARPHRD type
        if (f->has_hw)
            memcpy(r->u + 2, f->hw, 6);
        break;
    }
    default: {                                  // ADDR, BRDADDR, NETMASK: the first IPv4 address
        const struct sockaddr *sa = NULL;
        for (struct ifaddrs *a = list; a && !sa; a = a->ifa_next)
            if (a->ifa_addr && a->ifa_addr->sa_family == AF_INET && !strcmp(a->ifa_name, f->name))
                sa = (uint32_t)lreq == L_SIOCGIFADDR ? a->ifa_addr
                   : (uint32_t)lreq == L_SIOCGIFNETMASK ? a->ifa_netmask : a->ifa_broadaddr;
        if (!sa) {
            *ret = LERR(EADDRNOTAVAIL);
            break;
        }
        uint8_t lsin[16] = {0};                 // struct sockaddr_in, Linux layout
        uint16_t fam = L_AF_INET;
        memcpy(lsin, &fam, 2);
        memcpy(lsin + 4, &((const struct sockaddr_in *)sa)->sin_addr, 4);
        memcpy(r->u, lsin, 16);
        break;
    }
    }
out:
    if (list)
        freeifaddrs(list);
    return true;
}

// ---------------------------------------------------------------- /proc/net
//
// /proc/net/route, ipv6_route, arp and dev, from Darwin's routing table
// (sysctl NET_RT_DUMP / NET_RT_FLAGS) and interfaces. Wine's nsiproxy reads
// them for GetIpForwardTable/GetBestRoute, GetIpNetTable and the interface
// counters; without route, the IPv4 forward table answered
// STATUS_NOT_SUPPORTED and Minecraft Dungeons II's protection killed the game
// on that answer (benchmarks/stage62, 15).
#include <net/route.h>
#include <stdio.h>
#include <sys/sysctl.h>

#define RT_ROUNDUP(a) ((a) > 0 ? (1 + (((a) - 1) | (sizeof(uint32_t) - 1))) : sizeof(uint32_t))
#ifndef RTF_WASCLONED
#define RTF_WASCLONED 0x20000
#endif
#ifndef RTF_IFSCOPE
#define RTF_IFSCOPE 0x1000000
#endif

// The routing messages for family `af` (NET_RT_DUMP, or NET_RT_FLAGS with
// `flags`); malloc'd, *len its size.
static uint8_t *rt_dump(int af, int op, int flags, size_t *len)
{
    int mib[6] = { CTL_NET, PF_ROUTE, 0, af, op, flags };
    size_t n = 0;
    if (sysctl(mib, 6, NULL, &n, NULL, 0) != 0)
        return NULL;
    n += n / 4 + 4096;          // the table can grow between the two calls
    uint8_t *b = malloc(n);
    if (b && sysctl(mib, 6, b, &n, NULL, 0) != 0) {
        free(b);
        return NULL;
    }
    *len = n;
    return b;
}

// The sockaddrs after a routing message header, indexed by RTAX_*.
static void rt_addrs(const struct rt_msghdr *m, const struct sockaddr *sa[RTAX_MAX])
{
    const uint8_t *p = (const uint8_t *)(m + 1), *end = (const uint8_t *)m + m->rtm_msglen;
    for (int i = 0; i < RTAX_MAX; i++) {
        sa[i] = NULL;
        if (!(m->rtm_addrs & (1 << i)) || p >= end)
            continue;
        sa[i] = (const struct sockaddr *)p;
        p += RT_ROUNDUP(sa[i]->sa_len);
    }
}

// A (possibly truncated) routing-socket netmask as a full address.
static void rt_mask(const struct sockaddr *sa, void *out, size_t addr_off, size_t addr_len)
{
    memset(out, 0, addr_len);
    if (!sa || sa->sa_len <= addr_off)
        return;
    size_t have = sa->sa_len - addr_off;
    memcpy(out, (const uint8_t *)sa + addr_off, have < addr_len ? have : addr_len);
}

static bool rt_skip(const struct rt_msghdr *m, char *ifname)
{
    if (!(m->rtm_flags & RTF_UP) ||
        (m->rtm_flags & (RTF_LLINFO | RTF_WASCLONED | RTF_BROADCAST | RTF_MULTICAST | RTF_IFSCOPE)))
        return true;
    if (!if_indextoname(m->rtm_index, ifname))
        return true;
    return !strncmp(ifname, "lo", 2);   // loopback routes: Wine adds its own, Linux keeps them elsewhere
}

static size_t gen_route(char *buf, size_t cap)
{
    size_t o = (size_t)snprintf(buf, cap, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
    size_t len;
    uint8_t *d = rt_dump(AF_INET, NET_RT_DUMP, 0, &len);
    for (size_t at = 0; d && at + sizeof(struct rt_msghdr) <= len && o < cap; ) {
        const struct rt_msghdr *m = (const void *)(d + at);
        if (m->rtm_msglen == 0)
            break;
        at += m->rtm_msglen;
        const struct sockaddr *sa[RTAX_MAX];
        char ifname[IF_NAMESIZE];
        rt_addrs(m, sa);
        if (rt_skip(m, ifname) || !sa[RTAX_DST] || sa[RTAX_DST]->sa_family != AF_INET)
            continue;
        uint32_t dst = ((const struct sockaddr_in *)sa[RTAX_DST])->sin_addr.s_addr, gw = 0, mask;
        unsigned fl = 0x1;                                  // RTF_UP
        if (sa[RTAX_GATEWAY] && sa[RTAX_GATEWAY]->sa_family == AF_INET && (m->rtm_flags & RTF_GATEWAY)) {
            gw = ((const struct sockaddr_in *)sa[RTAX_GATEWAY])->sin_addr.s_addr;
            fl |= 0x2;                                      // RTF_GATEWAY
        }
        if (m->rtm_flags & RTF_HOST) {
            mask = 0xffffffffu;
            fl |= 0x4;                                      // RTF_HOST
        } else {
            rt_mask(sa[RTAX_NETMASK], &mask, offsetof(struct sockaddr_in, sin_addr), 4);
        }
        o += (size_t)snprintf(buf + o, cap - o, "%s\t%08X\t%08X\t%04X\t0\t0\t0\t%08X\t0\t0\t0\n",
                              ifname, dst, gw, fl, mask);
    }
    free(d);
    return o < cap ? o : cap;
}

static void hex6(char *out, const uint8_t *a)
{
    for (int i = 0; i < 16; i++)
        sprintf(out + 2 * i, "%02x", a[i]);
}

static size_t gen_ipv6_route(char *buf, size_t cap)
{
    size_t o = 0, len;
    uint8_t *d = rt_dump(AF_INET6, NET_RT_DUMP, 0, &len);
    for (size_t at = 0; d && at + sizeof(struct rt_msghdr) <= len && o < cap; ) {
        const struct rt_msghdr *m = (const void *)(d + at);
        if (m->rtm_msglen == 0)
            break;
        at += m->rtm_msglen;
        const struct sockaddr *sa[RTAX_MAX];
        char ifname[IF_NAMESIZE];
        rt_addrs(m, sa);
        if (rt_skip(m, ifname) || !sa[RTAX_DST] || sa[RTAX_DST]->sa_family != AF_INET6)
            continue;
        uint8_t dst[16], gw[16] = {0}, mask[16];
        memcpy(dst, &((const struct sockaddr_in6 *)sa[RTAX_DST])->sin6_addr, 16);
        if (dst[0] == 0xfe && (dst[1] & 0xc0) == 0x80)
            dst[2] = dst[3] = 0;                            // KAME's embedded scope
        unsigned fl = 0x1;
        if (sa[RTAX_GATEWAY] && sa[RTAX_GATEWAY]->sa_family == AF_INET6 && (m->rtm_flags & RTF_GATEWAY)) {
            memcpy(gw, &((const struct sockaddr_in6 *)sa[RTAX_GATEWAY])->sin6_addr, 16);
            if (gw[0] == 0xfe && (gw[1] & 0xc0) == 0x80)
                gw[2] = gw[3] = 0;
            fl |= 0x2;
        }
        int plen = 128;
        if (!(m->rtm_flags & RTF_HOST)) {
            rt_mask(sa[RTAX_NETMASK], mask, offsetof(struct sockaddr_in6, sin6_addr), 16);
            plen = sa[RTAX_NETMASK] ? prefix_len(mask, 16) : 0;
        } else {
            fl |= 0x4;
        }
        char hd[33], hz[33], hg[33];
        static const uint8_t zero[16];
        hex6(hd, dst); hex6(hz, zero); hex6(hg, gw);
        o += (size_t)snprintf(buf + o, cap - o, "%s %02x %s 00 %s %08x 00000000 00000000 %08x %8s\n",
                              hd, plen, hz, hg, 256, fl, ifname);
    }
    free(d);
    return o < cap ? o : cap;
}

static size_t gen_arp(char *buf, size_t cap)
{
    size_t o = (size_t)snprintf(buf, cap, "IP address       HW type     Flags       HW address            Mask     Device\n");
    size_t len;
    uint8_t *d = rt_dump(AF_INET, NET_RT_FLAGS, RTF_LLINFO, &len);
    for (size_t at = 0; d && at + sizeof(struct rt_msghdr) <= len && o < cap; ) {
        const struct rt_msghdr *m = (const void *)(d + at);
        if (m->rtm_msglen == 0)
            break;
        at += m->rtm_msglen;
        const struct sockaddr *sa[RTAX_MAX];
        char ifname[IF_NAMESIZE];
        rt_addrs(m, sa);
        if (!sa[RTAX_DST] || sa[RTAX_DST]->sa_family != AF_INET || !sa[RTAX_GATEWAY] ||
            sa[RTAX_GATEWAY]->sa_family != AF_LINK || !if_indextoname(m->rtm_index, ifname))
            continue;
        const struct sockaddr_dl *dl = (const struct sockaddr_dl *)sa[RTAX_GATEWAY];
        if (dl->sdl_alen != 6)
            continue;                                       // incomplete entry
        const uint8_t *hw = (const uint8_t *)LLADDR(dl);
        char ip[16], mac[18];
        inet_ntop(AF_INET, &((const struct sockaddr_in *)sa[RTAX_DST])->sin_addr, ip, sizeof ip);
        snprintf(mac, sizeof mac, "%02x:%02x:%02x:%02x:%02x:%02x", hw[0], hw[1], hw[2], hw[3], hw[4], hw[5]);
        unsigned fl = 0x2 | ((m->rtm_flags & RTF_STATIC) ? 0x4 : 0);   // ATF_COM, ATF_PERM
        o += (size_t)snprintf(buf + o, cap - o, "%-16s 0x1         0x%-9x %-21s *        %s\n", ip, fl, mac, ifname);
    }
    free(d);
    return o < cap ? o : cap;
}

static size_t gen_dev(char *buf, size_t cap)
{
    size_t o = (size_t)snprintf(buf, cap,
        "Inter-|   Receive                                                |  Transmit\n"
        " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
    struct netif ifs[MAX_NETIFS];
    struct ifaddrs *list = NULL;
    int n = host_netifs(ifs, MAX_NETIFS, &list);
    for (int i = 0; i < n && o < cap; i++)
        o += (size_t)snprintf(buf + o, cap - o, "%6s: %7u %7u %4u    0    0     0          0         0 %8u %7u %4u    0    0     0       0          0\n",
                              ifs[i].name, ifs[i].stats[2], ifs[i].stats[0], ifs[i].stats[4],
                              ifs[i].stats[3], ifs[i].stats[1], ifs[i].stats[5]);
    if (list)
        freeifaddrs(list);
    return o < cap ? o : cap;
}

// The contents of /proc/net/<name> for the files above into buf; -1 when
// `name` is not one of them.
long lxrt_netif_procnet(const char *name, char *buf, size_t cap)
{
    if (!strcmp(name, "route"))      return (long)gen_route(buf, cap);
    if (!strcmp(name, "ipv6_route")) return (long)gen_ipv6_route(buf, cap);
    if (!strcmp(name, "arp"))        return (long)gen_arp(buf, cap);
    if (!strcmp(name, "dev"))        return (long)gen_dev(buf, cap);
    return -1;
}
