// Sockets.
//
// Three incompatibilities, in increasing order of how quietly they bite:
//
//   1. struct sockaddr has a different shape. Linux starts with a 16-bit
//      family; Darwin starts with a 1-byte length and a 1-byte family. A
//      pass-through makes AF_UNIX (1) look like sun_len = 1, and the connect
//      fails with something unrelated to the real problem.
//   2. sun_path is 108 bytes on Linux and 104 on Darwin.
//   3. Linux ORs SOCK_CLOEXEC and SOCK_NONBLOCK into the socket *type*. Darwin
//      has no such thing and rejects the type outright.

#include "lxrt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/uio.h>
#include <sys/ucred.h>
#include <sys/un.h>
bool lxrt_trace_on(void);
#include <unistd.h>
#include <libproc.h>
#include <sys/mman.h>
#include <sys/proc_info.h>
#include <signal.h>

#define LERR(e) (-lxrt_errno_to_linux(e))

#define L_SOCK_NONBLOCK 0x0800
#define L_SOCK_CLOEXEC  0x80000

#define L_AF_UNIX  1
#define L_AF_INET  2
#define L_AF_INET6 10

struct linux_sockaddr { uint16_t sa_family; char sa_data[126]; };
struct linux_sockaddr_un { uint16_t sun_family; char sun_path[108]; };
struct linux_sockaddr_in { uint16_t sin_family; uint16_t sin_port;
                           uint32_t sin_addr; uint8_t pad[8]; };
struct linux_sockaddr_in6 { uint16_t sin6_family; uint16_t sin6_port;
                            uint32_t sin6_flowinfo; uint8_t sin6_addr[16];
                            uint32_t sin6_scope_id; };

// Where Linux abstract socket names are materialised. Created once, mode 0700.
static const char *abstract_dir(void)
{
    static char dir[256];
    static bool ready, tried;
    if (tried)
        return ready ? dir : NULL;
    tried = true;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    snprintf(dir, sizeof dir, "%s/lxrt-abstract-%u", tmp, (unsigned)getuid());
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return NULL;
    ready = true;
    return dir;
}

// Darwin's AF_* happen to match Linux's for UNIX and INET; INET6 does not
// (10 on Linux, 30 on Darwin), so the mapping is explicit.
static int family_to_darwin(int lf)
{
    switch (lf) {
    case L_AF_UNIX:  return AF_UNIX;
    case L_AF_INET:  return AF_INET;
    case L_AF_INET6: return AF_INET6;
    default:         return -1;
    }
}

static int family_to_linux(int df)
{
    switch (df) {
    case AF_UNIX:  return L_AF_UNIX;
    case AF_INET:  return L_AF_INET;
    case AF_INET6: return L_AF_INET6;
    default:       return 0;
    }
}

// Returns the Darwin length, or -1. `out` must be at least sizeof(sockaddr_un).
static int addr_to_darwin(const void *lsa, unsigned llen, struct sockaddr_storage *out)
{
    if (!lsa || llen < 2)
        return -1;
    const struct linux_sockaddr *g = lsa;
    memset(out, 0, sizeof(*out));

    switch (g->sa_family) {
    case L_AF_UNIX: {
        const struct linux_sockaddr_un *lu = lsa;
        struct sockaddr_un *du = (struct sockaddr_un *)out;
        du->sun_family = AF_UNIX;
        size_t pathmax = llen > 2 ? llen - 2 : 0;
        if (pathmax > sizeof(lu->sun_path))
            pathmax = sizeof(lu->sun_path);
        // Abstract sockets -- a leading NUL -- exist only on Linux; Darwin has
        // no filesystem-free socket namespace. They are mapped into a directory
        // the runtime owns instead of refused, because both ends of an abstract
        // socket are guest code and both go through this same translation, so
        // they agree. What is lost is the auto-cleanup on last close, and
        // isolation from anything else using that directory.
        if (pathmax && lu->sun_path[0] == '\0') {
            const char *dir = abstract_dir();
            if (!dir)
                return -1;
            size_t n = strnlen(lu->sun_path + 1, pathmax - 1);
            char name[128];
            size_t k = 0;
            for (size_t i = 0; i < n && k + 1 < sizeof name; i++) {
                char c = lu->sun_path[1 + i];
                // Abstract names may hold anything, including '/'.
                name[k++] = (c == '/' || c == '\0') ? '_' : c;
            }
            name[k] = '\0';
            struct sockaddr_un *du = (struct sockaddr_un *)out;
            du->sun_family = AF_UNIX;
            int w = snprintf(du->sun_path, sizeof du->sun_path, "%s/%s", dir, name);
            if (w < 0 || (size_t)w >= sizeof du->sun_path)
                return -1;
            du->sun_len = (uint8_t)sizeof(*du);
            return (int)sizeof(*du);
        }
        size_t n = strnlen(lu->sun_path, pathmax);
        // A filesystem socket path is a guest path: it resolves like every
        // other one -- binds of a bwrap sandbox, then LXRT_ROOT (Xvnc's
        // /tmp/.X11-unix/X1 failed to bind with ENOENT until the root was
        // applied; Chromium's SingletonSocket under a sandbox's /tmp until the
        // binds were). Darwin's sun_path is 104 bytes.
        extern const char *lxrt_translate_guest_path(const char *, char *, size_t);
        char guest[sizeof lu->sun_path + 1], host[1024];
        memcpy(guest, lu->sun_path, n);
        guest[n] = '\0';
        const char *h = n && guest[0] == '/' ? lxrt_translate_guest_path(guest, host, sizeof host) : guest;
        size_t hl = strlen(h);
        if (hl >= sizeof(du->sun_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(du->sun_path, h, hl + 1);
        du->sun_len = (uint8_t)(sizeof(*du));
        return (int)sizeof(*du);
    }
    case L_AF_INET: {
        const struct linux_sockaddr_in *li = lsa;
        struct sockaddr_in *di = (struct sockaddr_in *)out;
        di->sin_len = sizeof(*di);
        di->sin_family = AF_INET;
        di->sin_port = li->sin_port;          // both already network order
        di->sin_addr.s_addr = li->sin_addr;
        return (int)sizeof(*di);
    }
    case L_AF_INET6: {
        const struct linux_sockaddr_in6 *l6 = lsa;
        struct sockaddr_in6 *d6 = (struct sockaddr_in6 *)out;
        d6->sin6_len = sizeof(*d6);
        d6->sin6_family = AF_INET6;
        d6->sin6_port = l6->sin6_port;
        d6->sin6_flowinfo = l6->sin6_flowinfo;
        memcpy(&d6->sin6_addr, l6->sin6_addr, 16);
        d6->sin6_scope_id = l6->sin6_scope_id;
        return (int)sizeof(*d6);
    }
    default:
        return -1;
    }
}

static void addr_to_linux(const struct sockaddr *dsa, socklen_t dlen,
                          void *lsa, uint32_t *llen)
{
    if (!lsa || !llen)
        return;
    uint32_t cap = *llen;
    struct linux_sockaddr out;
    memset(&out, 0, sizeof out);
    uint32_t used = 0;

    switch (dsa->sa_family) {
    case AF_UNIX: {
        const struct sockaddr_un *du = (const struct sockaddr_un *)dsa;
        struct linux_sockaddr_un lu;
        memset(&lu, 0, sizeof lu);
        lu.sun_family = L_AF_UNIX;
        size_t n = strnlen(du->sun_path, sizeof du->sun_path);
        const char *src = du->sun_path;
        const char *root = getenv("LXRT_ROOT");
        size_t rl = root ? strlen(root) : 0;
        char hostp[sizeof du->sun_path + 1], gview[1024];
        memcpy(hostp, src, n);
        hostp[n] = '\0';
        const char *u = n && src[0] == '/' ? lxrt_mounts_untranslate(hostp, gview, sizeof gview) : NULL;
        if (u && strlen(u) < sizeof lu.sun_path) {
            src = u;            // back to the guest's own path (binds, root)
            n = strlen(u);
        } else if (rl && n > rl && strncmp(src, root, rl) == 0 && src[rl] == '/') {
            src += rl;
            n -= rl;
        }
        memcpy(lu.sun_path, src, n);
        used = (uint32_t)(2 + n + 1);
        memcpy(&out, &lu, sizeof lu < sizeof out ? sizeof lu : sizeof out);
        break;
    }
    case AF_INET: {
        const struct sockaddr_in *di = (const struct sockaddr_in *)dsa;
        struct linux_sockaddr_in li;
        memset(&li, 0, sizeof li);
        li.sin_family = L_AF_INET;
        li.sin_port = di->sin_port;
        li.sin_addr = di->sin_addr.s_addr;
        used = sizeof li;
        memcpy(&out, &li, sizeof li);
        break;
    }
    case AF_INET6: {
        const struct sockaddr_in6 *d6 = (const struct sockaddr_in6 *)dsa;
        struct linux_sockaddr_in6 l6;
        memset(&l6, 0, sizeof l6);
        l6.sin6_family = L_AF_INET6;
        l6.sin6_port = d6->sin6_port;
        l6.sin6_flowinfo = d6->sin6_flowinfo;
        memcpy(l6.sin6_addr, &d6->sin6_addr, 16);
        l6.sin6_scope_id = d6->sin6_scope_id;
        used = sizeof l6;
        memcpy(&out, &l6, sizeof l6);
        break;
    }
    default:
        out.sa_family = (uint16_t)family_to_linux(dsa->sa_family);
        used = dlen;
        break;
    }

    if (used > cap)
        used = cap;
    memcpy(lsa, &out, used);
    // Linux reports the size the address WOULD have needed, not what fitted.
    *llen = used;
}

// Linux carries these in the type; Darwin needs them applied afterwards.
static void apply_type_flags(int fd, int ltype)
{
    if (fd < 0)
        return;
    if (ltype & L_SOCK_NONBLOCK)
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if (ltype & L_SOCK_CLOEXEC)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
}

// Linux SOCK_SEQPACKET (5) on AF_UNIX has no Darwin counterpart: Darwin's
// unix sockets are STREAM or DGRAM. A DGRAM pair keeps message boundaries
// and carries SCM_RIGHTS, which is what Chromium's IPC (steamwebhelper) and
// crashpad want from a seqpacket pair; what it lacks is end-of-file when the
// peer closes -- a read then blocks instead of returning 0. Measured need:
// socketpair(AF_UNIX, SOCK_SEQPACKET) -> EPROTONOSUPPORT was the last
// syscall before CEF gave up (benchmarks/stage5-fex.txt).
static int type_to_darwin(int ltype)
{
    int base = ltype & ~(L_SOCK_NONBLOCK | L_SOCK_CLOEXEC);
    if (base == 5)          // SOCK_SEQPACKET
        return SOCK_DGRAM;
    return base;
}

// The end of file, then. A thread already blocked on a Darwin AF_UNIX
// datagram socket is not woken when the peer goes away: poll(), kevent() and
// recv() stay blocked (MEASURED on this Mac, 2026-09-29: a poll with a
// 2000 ms timeout returned 0 although the peer was closed 300 ms in, and a
// recv never returned). A poll made AFTER the peer is gone reports POLLIN at
// once, and the recv then fails with ECONNRESET. Chromium's zygotes wait for
// the browser that way (ppoll, then recvmsg, on their seqpacket socket) and
// outlived Heroic's browser process by minutes (MEASURED,
// benchmarks/stage24-heroic.txt). So a blocking recvmsg/recvfrom or ppoll on
// such a socket waits in slices of SEQPKT_SLICE_MS, and ECONNRESET on it
// reads as Linux's end of file, 0.
//
// Which sockets: those created as SOCK_SEQPACKET. They carry SO_LINGER
// {1, SEQPKT_LINGER} as their mark (a linger time means nothing to a
// datagram socket, and Darwin keeps and reports it: MEASURED), so the mark
// travels with the socket through fork, exec and SCM_RIGHTS, where a table
// in this process would not. A guest reading SO_LINGER sees it off.
#define SEQPKT_LINGER   0x5e9
#define SEQPKT_SLICE_MS 250

static void seqpkt_mark(int fd, int ltype)
{
    if ((ltype & ~(L_SOCK_NONBLOCK | L_SOCK_CLOEXEC)) != 5 || fd < 0)
        return;
    struct linger l = { 1, SEQPKT_LINGER };
    setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
}

bool lxrt_is_seqpacket(int fd)
{
    struct linger l;
    socklen_t n = sizeof l;
    return fd >= 0 && getsockopt(fd, SOL_SOCKET, SO_LINGER, &l, &n) == 0 &&
           l.l_onoff && l.l_linger == SEQPKT_LINGER;
}

// Block until fd is readable, a slice at a time; 0, or -EINTR (a signal
// ended the wait, as it ends a blocking recvmsg without SA_RESTART).
static long seqpkt_wait(int fd)
{
    for (;;) {
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, SEQPKT_SLICE_MS);
        if (r > 0)
            return 0;
        if (r < 0)
            return LERR(errno);
    }
}

// A blocking receive on a seqpacket socket: wait in slices first.
static long seqpkt_before_recv(int fd, int lflags)
{
    if ((lflags & 0x40) /* MSG_DONTWAIT */ || (fcntl(fd, F_GETFL) & O_NONBLOCK) ||
        !lxrt_is_seqpacket(fd))
        return 0;
    return seqpkt_wait(fd);
}

// AF_NETLINK / NETLINK_KOBJECT_UEVENT: libudev's device monitor. Darwin has
// no netlink, and SDL3 (sdl2-compat in Steam's runtime) refuses to start its
// haptic subsystem -- and with it all controller support, so winebus saw no
// gamepads (MEASURED: "could not init SDL: Could not initialize UDEV") --
// when udev_monitor_new_from_netlink fails. The monitor is a datagram socket
// that stays silent: no uevents, since there is no kernel to send them
// (controller hotplug reaches SDL through inotify on /dev/input instead).
// Its peer end is kept so uevents could be injected later.
#define L_AF_NETLINK 16
#define L_NETLINK_KOBJECT_UEVENT 15
static _Atomic int g_nl_peer[65536];        // peer fd + 1, 0 = not netlink
static _Atomic uint32_t g_nl_groups[65536];
static _Atomic uint64_t g_nl_ino[65536];
// A number can outlive its socket here: close_range, dup2 over it, exec of
// a CLOEXEC descriptor and a forked child's mass close never pass through
// lxrt_socket_close. Trusting the number alone turned the reused descriptor
// -- Chromium's IPC sockets -- into a fake netlink socket whose bind and
// getsockname lied, and Steam's web helper hung before its first window
// (MEASURED). The entry counts only while it names the same socket.
static bool is_netlink(int fd)
{
    if (fd < 0 || fd >= 65536 || atomic_load(&g_nl_peer[fd]) == 0)
        return false;
    struct stat st;
    if (fstat(fd, &st) == 0 && (uint64_t)st.st_ino == atomic_load(&g_nl_ino[fd]))
        return true;
    atomic_store(&g_nl_peer[fd], 0);            // stale: forget it (the peer leaks, once)
    return false;
}

long lxrt_socket(int ldomain, int ltype, int proto)
{
    // Only inside a (fake) bwrap container -- where Proton's winebus and the
    // games' SDL live. The Steam client itself, outside, got stuck in its
    // own udev/controller threads once the monitor opened, and its window
    // never appeared (MEASURED: login window with the socket refused, none
    // in 5 minutes with it). LXRT_NO_NETLINK=1 refuses it everywhere.
    extern bool lxrt_mounts_active(void);
    int nl_off = getenv("LXRT_NO_NETLINK") != NULL || !lxrt_mounts_active();
    if (ldomain == L_AF_NETLINK && proto == L_NETLINK_KOBJECT_UEVENT && nl_off == 0) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0)
            return LERR(errno);
        if (sv[0] >= 65536) {
            close(sv[0]);
            close(sv[1]);
            return LERR(EMFILE);
        }
        fcntl(sv[1], F_SETFD, FD_CLOEXEC);
        apply_type_flags(sv[0], ltype);
        struct stat st;
        atomic_store(&g_nl_ino[sv[0]], fstat(sv[0], &st) == 0 ? (uint64_t)st.st_ino : 0);
        atomic_store(&g_nl_groups[sv[0]], 0);
        atomic_store(&g_nl_peer[sv[0]], sv[1] + 1);
        return sv[0];
    }
    int d = family_to_darwin(ldomain);
    if (d < 0)
        return LERR(EAFNOSUPPORT);
    int fd = socket(d, type_to_darwin(ltype), proto);
    if (fd < 0)
        return LERR(errno);
    apply_type_flags(fd, ltype);
    seqpkt_mark(fd, ltype);
    return fd;
}

long lxrt_socketpair(int ldomain, int ltype, int proto, int *sv)
{
    int d = family_to_darwin(ldomain);
    if (d < 0 || !sv)
        return LERR(EAFNOSUPPORT);
    if (socketpair(d, type_to_darwin(ltype), proto, sv) != 0)
        return LERR(errno);
    apply_type_flags(sv[0], ltype);
    apply_type_flags(sv[1], ltype);
    seqpkt_mark(sv[0], ltype);
    seqpkt_mark(sv[1], ltype);
    return 0;
}

long lxrt_connect(int fd, const void *lsa, unsigned llen)
{
    struct sockaddr_storage ss;
    int n = addr_to_darwin(lsa, llen, &ss);
    if (n < 0)
        return LERR(EINVAL);
    long rc = connect(fd, (struct sockaddr *)&ss, (socklen_t)n) != 0 ? LERR(errno) : 0;
    // The native X server (scripts/run-x11-native.sh) is a macOS process: its
    // socket is the host's /tmp/.X11-unix/X<n>, which no guest root contains.
    // A guest X client connecting to /tmp/.X11-unix/X<n> that finds nothing in
    // its own root reaches the host socket instead. Only connect does this --
    // an X server inside the guest (Xvnc) still binds in its root.
    if (rc < 0 && ss.ss_family == AF_UNIX && llen > 2) {
        const struct linux_sockaddr_un *lu = lsa;
        static const char pre[] = "/tmp/.X11-unix/X";
        size_t pl = sizeof pre - 1;
        if (strncmp(lu->sun_path, pre, pl) == 0) {
            struct sockaddr_un hu;
            memset(&hu, 0, sizeof hu);
            hu.sun_family = AF_UNIX;
            hu.sun_len = (uint8_t)sizeof hu;
            snprintf(hu.sun_path, sizeof hu.sun_path, "/private%.*s", (int)strnlen(lu->sun_path, 90), lu->sun_path);
            if (connect(fd, (struct sockaddr *)&hu, sizeof hu) == 0)
                rc = 0;
        }
    }
    if (lxrt_trace_on()) {
        char a[INET6_ADDRSTRLEN] = "?";
        int port = 0;
        if (ss.ss_family == AF_INET) {
            inet_ntop(AF_INET, &((struct sockaddr_in *)&ss)->sin_addr, a, sizeof a);
            port = ntohs(((struct sockaddr_in *)&ss)->sin_port);
        } else if (ss.ss_family == AF_INET6) {
            inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ss)->sin6_addr, a, sizeof a);
            port = ntohs(((struct sockaddr_in6 *)&ss)->sin6_port);
        }
        if (ss.ss_family == AF_INET || ss.ss_family == AF_INET6)
            fprintf(lxrt_trace_stream(), "[lxrt]    connect fd %d %s port %d -> %ld\n", fd, a, port, rc);
    }
    return rc;
}

long lxrt_bind(int fd, const void *lsa, unsigned llen)
{
    if (is_netlink(fd)) {
        // struct sockaddr_nl { u16 family; u16 pad; u32 pid; u32 groups; }
        if (!lsa || llen < 12)
            return LERR(EINVAL);
        uint32_t groups;
        memcpy(&groups, (const uint8_t *)lsa + 8, 4);
        atomic_store(&g_nl_groups[fd], groups);
        return 0;
    }
    struct sockaddr_storage ss;
    int n = addr_to_darwin(lsa, llen, &ss);
    if (n < 0)
        return LERR(EINVAL);
    // A stale socket file survives a crash, where an abstract socket would
    // not. Unlink before binding so a restart is not blocked by the last run.
    if (((struct sockaddr *)&ss)->sa_family == AF_UNIX)
        unlink(((struct sockaddr_un *)&ss)->sun_path);
    return bind(fd, (struct sockaddr *)&ss, (socklen_t)n) != 0 ? LERR(errno) : 0;
}

long lxrt_accept4(int fd, void *lsa, uint32_t *llen, int lflags)
{
    struct sockaddr_storage ss;
    socklen_t dlen = sizeof ss;
    int nfd = accept(fd, (struct sockaddr *)&ss, &dlen);
    if (nfd < 0)
        return LERR(errno);
    if (lsa && llen)
        addr_to_linux((struct sockaddr *)&ss, dlen, lsa, llen);
    apply_type_flags(nfd, lflags);
    return nfd;
}

long lxrt_getsockname(int fd, void *lsa, uint32_t *llen, bool peer)
{
    if (is_netlink(fd)) {
        if (!lsa || !llen)
            return LERR(EFAULT);
        uint8_t nl[12] = {0};
        uint16_t fam = L_AF_NETLINK;
        uint32_t pid = peer ? 0 : (uint32_t)getpid(), groups = peer ? 0 : atomic_load(&g_nl_groups[fd]);
        memcpy(nl, &fam, 2);
        memcpy(nl + 4, &pid, 4);
        memcpy(nl + 8, &groups, 4);
        memcpy(lsa, nl, *llen < sizeof nl ? *llen : sizeof nl);
        *llen = sizeof nl;
        return 0;
    }
    struct sockaddr_storage ss;
    socklen_t dlen = sizeof ss;
    int rc = peer ? getpeername(fd, (struct sockaddr *)&ss, &dlen)
                  : getsockname(fd, (struct sockaddr *)&ss, &dlen);
    if (rc != 0)
        return LERR(errno);
    addr_to_linux((struct sockaddr *)&ss, dlen, lsa, llen);
    return 0;
}

// ---------------------------------------------------------------- msghdr
//
// The fifth number space, and the one that hangs rather than fails: struct
// msghdr and struct cmsghdr are laid out differently, and SOL_SOCKET is 1 on
// Linux and 0xffff on Darwin.
//
//   msghdr      Linux                      Darwin
//   msg_iovlen  size_t (8 bytes)           int (4 bytes)
//   msg_controllen size_t (8)              socklen_t (4)
//   -- and Linux pads after msg_namelen, Darwin does not.
//
//   cmsghdr     Linux                      Darwin
//   cmsg_len    size_t (8 bytes)           socklen_t (4 bytes)
//   alignment   8                          4
//
// Passing one through unchanged makes an SCM_RIGHTS fd transfer silently
// malformed. FEXServer hands the RootFS fd to FEX that way; the symptom was
// FEX blocking forever in a receive rather than reporting anything.

struct linux_msghdr {
    uint64_t msg_name;
    uint32_t msg_namelen;
    uint32_t pad_;
    uint64_t msg_iov;
    uint64_t msg_iovlen;
    uint64_t msg_control;
    uint64_t msg_controllen;
    int32_t  msg_flags;
    int32_t  pad2_;
};

struct linux_cmsghdr {
    uint64_t cmsg_len;
    int32_t  cmsg_level;
    int32_t  cmsg_type;
};

#define L_SOL_SOCKET 1
#define L_SCM_RIGHTS 1

#ifndef L_SOL_SOCKET_OPT
#define L_SOL_SOCKET_OPT 1
#endif
// SO_PASSCRED / SO_PEERCRED / SCM_CREDENTIALS. Darwin's unix sockets cannot be
// asked to attach credentials to every message (there is no LOCAL_CREDS in
// the public SDK), but they can always be ASKED who the peer is: LOCAL_PEERPID
// and LOCAL_PEERCRED. So SO_PASSCRED is remembered per descriptor and
// recvmsg() synthesises the SCM_CREDENTIALS control message from those two,
// which for a socketpair or a connected unix socket is the same answer Linux
// would give. Chromium's Mojo channel (steamwebhelper) CHECKs the setsockopt
// and reads the peer pid from the first message.
#define L_SO_PASSCRED 16
#define L_SO_PEERCRED 17
#define L_SCM_CREDENTIALS 2
struct linux_ucred { int32_t pid; uint32_t uid, gid; };
static uint8_t g_passcred[65536 / 8];
static bool passcred_get(int fd) { return fd >= 0 && fd < 65536 && (g_passcred[fd >> 3] >> (fd & 7)) & 1; }
static void passcred_set(int fd, bool on)
{
    if (fd < 0 || fd >= 65536) return;
    if (on) g_passcred[fd >> 3] |= (uint8_t)(1u << (fd & 7));
    else    g_passcred[fd >> 3] &= (uint8_t)~(1u << (fd & 7));
}
void lxrt_socket_close(int fd)
{
    passcred_set(fd, false);
    if (fd >= 0 && fd < 65536) {
        int peer = atomic_exchange(&g_nl_peer[fd], 0);
        if (peer)
            close(peer - 1);
    }
}
void lxrt_socket_dup(int oldfd, int newfd) { passcred_set(newfd, passcred_get(oldfd)); }

// Who sent the message? Linux answers per message (SCM_CREDENTIALS carries
// the SENDER's pid); Darwin's LOCAL_PEERPID answers who created or connected
// the peer socket, which for a socketpair inherited across fork is the
// parent. Chromium's zygote learns each child's pid exactly that way (the
// child pings the browser over a socketpair the browser made) and got the
// browser's own pid back: "Zygote could not fork ... child_pid -1".
//
// So senders record themselves. Each AF_UNIX send stores (the kernel identity
// of the sending socket, sender pid) in a table shared by every runtime
// process of this user; the receiver looks up its peer's identity. Socket
// identities come from proc_pidfdinfo and are the same in every process that
// holds the socket (measured: a forked child sees the parent's values).
// Last sender wins, which is what a Linux receiver would see for a message
// just sent; a peer outside the runtime falls back to LOCAL_PEERPID.
struct cred_ent { _Atomic uint64_t so; _Atomic int32_t pid; };
#define CRED_SLOTS 8192
static struct cred_ent *g_creds;

static struct cred_ent *cred_table(void)
{
    static _Atomic int state;              // 0 untried, 1 ready, 2 failed
    int st = atomic_load(&state);
    if (st == 1) return g_creds;
    if (st == 2) return NULL;
    char path[96];
    snprintf(path, sizeof path, "/tmp/lxrt-shm-%u", (unsigned)getuid());
    mkdir(path, 01777);
    snprintf(path + strlen(path), sizeof path - strlen(path), "/unix-creds");
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    size_t sz = sizeof(struct cred_ent) * CRED_SLOTS;
    void *p = MAP_FAILED;
    if (fd >= 0 && ftruncate(fd, (off_t)sz) == 0)
        p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fd >= 0) close(fd);
    if (p == MAP_FAILED) { atomic_store(&state, 2); return NULL; }
    g_creds = p;
    atomic_store(&state, 1);
    return g_creds;
}

static bool unix_ids(int fd, uint64_t *self, uint64_t *peer)
{
    struct socket_fdinfo si;
    if (proc_pidfdinfo(getpid(), fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) != sizeof si ||
        si.psi.soi_family != AF_UNIX)
        return false;
    *self = si.psi.soi_so;
    *peer = si.psi.soi_proto.pri_un.unsi_conn_so;
    return true;
}

static void cred_note_send(int fd)
{
    uint64_t self, peer;
    struct cred_ent *t;
    if (!unix_ids(fd, &self, &peer) || !self || !(t = cred_table()))
        return;
    uint32_t h = (uint32_t)((self * 0x9E3779B97F4A7C15ull) >> 51) % CRED_SLOTS;
    for (int k = 0; k < 64; k++) {
        struct cred_ent *e = &t[(h + (uint32_t)k) % CRED_SLOTS];
        uint64_t cur = atomic_load(&e->so);
        if (cur == self || (cur == 0 && atomic_compare_exchange_strong(&e->so, &cur, self)) ||
            cur == self /* lost the race to the same socket */) {
            atomic_store(&e->pid, (int32_t)getpid());
            return;
        }
    }
}

static pid_t cred_peer_pid(int fd)
{
    uint64_t self, peer;
    struct cred_ent *t;
    if (!unix_ids(fd, &self, &peer) || !peer || !(t = cred_table()))
        return 0;
    uint32_t h = (uint32_t)((peer * 0x9E3779B97F4A7C15ull) >> 51) % CRED_SLOTS;
    for (int k = 0; k < 64; k++) {
        struct cred_ent *e = &t[(h + (uint32_t)k) % CRED_SLOTS];
        uint64_t cur = atomic_load(&e->so);
        if (cur == peer) {
            pid_t p = atomic_load(&e->pid);
            // A recorded sender that has since exited is stale.
            return (p > 0 && kill(p, 0) == 0) ? p : 0;
        }
        if (cur == 0) break;
    }
    return 0;
}

static bool peer_ucred(int fd, struct linux_ucred *out)
{
    // The recorded sender first (see cred_note_send); LOCAL_PEERPID and
    // LOCAL_PEERCRED only answer for connected stream sockets -- a DGRAM
    // socketpair (what SOCK_SEQPACKET becomes here) had no credentials at all.
    pid_t sender = cred_peer_pid(fd);
    pid_t pid = 0; socklen_t pl = sizeof pid;
    struct xucred xu; socklen_t xl = sizeof xu;
    bool have_pid = getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &pl) == 0;
    bool have_cred = getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &xu, &xl) == 0;
    if (!sender && !have_pid)
        return false;
    out->pid = (int32_t)(sender ? sender : pid);
    // Every process here runs as this user; a peer outside the runtime says so.
    out->uid = have_cred ? xu.cr_uid : getuid();
    out->gid = have_cred ? (xu.cr_ngroups > 0 ? xu.cr_groups[0] : 0) : getgid();
    return true;
}


#define LALIGN(n) (((n) + 7u) & ~7u)

static int cmsg_level_to_darwin(int l) { return l == L_SOL_SOCKET ? SOL_SOCKET : l; }
static int cmsg_level_to_linux(int l)  { return l == SOL_SOCKET ? L_SOL_SOCKET : l; }

// Builds a Darwin control buffer from a Linux one. Returns bytes written, or
// -1. `dst` must be at least as large as the Linux buffer.
static long cmsg_to_darwin(const void *lbuf, size_t llen, void *dst, size_t dcap)
{
    const uint8_t *p = lbuf;
    uint8_t *out = dst;
    size_t in_off = 0, out_off = 0;

    while (in_off + sizeof(struct linux_cmsghdr) <= llen) {
        const struct linux_cmsghdr *lc = (const void *)(p + in_off);
        if (lc->cmsg_len < sizeof(*lc) || in_off + lc->cmsg_len > llen)
            break;
        size_t payload = (size_t)lc->cmsg_len - sizeof(*lc);
        // SCM_CREDENTIALS has no Darwin form: its type (2) is Darwin's
        // SCM_TIMESTAMP, and sendmsg failed. Linux's libpulse sends it with
        // its first packet, so every PulseAudio client lost its connection
        // (MEASURED). A receiver in this runtime gets the sender's
        // credentials from SO_PASSCRED anyway (lxrt_recvmsg below).
        if (lc->cmsg_level == L_SOL_SOCKET && lc->cmsg_type == L_SCM_CREDENTIALS) {
            in_off += LALIGN((uint32_t)lc->cmsg_len);
            continue;
        }
        size_t need = CMSG_SPACE(payload);
        if (out_off + need > dcap)
            return -1;

        struct cmsghdr *dc = (struct cmsghdr *)(out + out_off);
        dc->cmsg_len = (socklen_t)CMSG_LEN(payload);
        dc->cmsg_level = cmsg_level_to_darwin(lc->cmsg_level);
        dc->cmsg_type = lc->cmsg_type;       // SCM_RIGHTS is 1 on both
        memcpy(CMSG_DATA(dc), (const uint8_t *)lc + sizeof(*lc), payload);

        out_off += need;
        in_off += LALIGN((uint32_t)lc->cmsg_len);
    }
    return (long)out_off;
}

static long cmsg_to_linux(const void *dbuf, size_t dlen, void *lout, size_t lcap)
{
    const uint8_t *p = dbuf;
    uint8_t *out = lout;
    size_t in_off = 0, out_off = 0;

    while (in_off + sizeof(struct cmsghdr) <= dlen) {
        const struct cmsghdr *dc = (const void *)(p + in_off);
        if (dc->cmsg_len < sizeof(*dc) || in_off + dc->cmsg_len > dlen)
            break;
        size_t payload = (size_t)dc->cmsg_len - sizeof(*dc);
        size_t need = LALIGN((uint32_t)(sizeof(struct linux_cmsghdr) + payload));
        if (out_off + need > lcap)
            return -1;

        struct linux_cmsghdr *lc = (struct linux_cmsghdr *)(out + out_off);
        lc->cmsg_len = sizeof(*lc) + payload;
        lc->cmsg_level = cmsg_level_to_linux(dc->cmsg_level);
        lc->cmsg_type = dc->cmsg_type;
        memcpy((uint8_t *)lc + sizeof(*lc), (const uint8_t *)dc + sizeof(*dc), payload);

        out_off += need;
        // Darwin aligns to 4; CMSG_ALIGN does it for us via the next pointer.
        in_off += (size_t)((dc->cmsg_len + 3u) & ~3u);
    }
    return (long)out_off;
}

#define CTRL_MAX 4096


// MSG_* flags: another silent number space. Linux -> Darwin:
//   CTRUNC 0x8 -> 0x20, TRUNC 0x20 -> 0x10, DONTWAIT 0x40 -> 0x80,
//   EOR 0x80 -> 0x8, WAITALL 0x100 -> 0x40, NOSIGNAL 0x4000 -> 0x80000;
//   OOB/PEEK/DONTROUTE (1/2/4) agree; CMSG_CLOEXEC has no meaning here.
int lxrt_msgflags_to_darwin(int lf)
{
    int d = lf & 0x7;
    if (lf & 0x8)     d |= 0x20;
    if (lf & 0x20)    d |= 0x10;
    if (lf & 0x40)    d |= 0x80;
    if (lf & 0x80)    d |= 0x8;
    if (lf & 0x100)   d |= 0x40;
    if (lf & 0x4000)  d |= 0x80000;
    return d;
}

int lxrt_msgflags_to_linux(int df)
{
    int l = df & 0x7;
    if (df & 0x20) l |= 0x8;
    if (df & 0x10) l |= 0x20;
    if (df & 0x8)  l |= 0x80;
    return l;
}

long lxrt_sendto(int fd, const void *buf, size_t len, int lflags, const void *laddr, unsigned lalen)
{
    cred_note_send(fd);
    int df = lxrt_msgflags_to_darwin(lflags);
    if (!laddr) {
        ssize_t r = send(fd, buf, len, df);
        return r < 0 ? LERR(errno) : (long)r;
    }
    struct sockaddr_storage ss;
    int dlen = addr_to_darwin(laddr, lalen, &ss);
    if (dlen < 0)
        return LERR(EINVAL);
    ssize_t r = sendto(fd, buf, len, df, (struct sockaddr *)&ss, (socklen_t)dlen);
    return r < 0 ? LERR(errno) : (long)r;
}

long lxrt_recvfrom(int fd, void *buf, size_t len, int lflags, void *laddr, uint32_t *lalen)
{
    int df = lxrt_msgflags_to_darwin(lflags);
    struct sockaddr_storage ss;
    socklen_t sl = sizeof ss;
    long w = seqpkt_before_recv(fd, lflags);
    if (w < 0)
        return w;
    ssize_t r = recvfrom(fd, buf, len, df, laddr ? (struct sockaddr *)&ss : NULL, laddr ? &sl : NULL);
    if (r < 0 && errno == ECONNRESET && lxrt_is_seqpacket(fd))
        return 0;               // the peer is gone: end of file (see above)
    if (r < 0)
        return LERR(errno);
    if (laddr && lalen)
        addr_to_linux((struct sockaddr *)&ss, sl, laddr, lalen);
    return (long)r;
}

long lxrt_sendmsg(int fd, const void *lmsg, int flags)
{
    if (!lmsg)
        return LERR(EFAULT);
    cred_note_send(fd);
    const struct linux_msghdr *lm = lmsg;

    struct msghdr dm;
    memset(&dm, 0, sizeof dm);
    struct sockaddr_storage ss;
    if (lm->msg_name && lm->msg_namelen) {
        int n = addr_to_darwin((const void *)(uintptr_t)lm->msg_name,
                               lm->msg_namelen, &ss);
        if (n < 0)
            return LERR(EINVAL);
        dm.msg_name = &ss;
        dm.msg_namelen = (socklen_t)n;
    }
    dm.msg_iov = (struct iovec *)(uintptr_t)lm->msg_iov;   // same layout
    dm.msg_iovlen = (int)lm->msg_iovlen;

    uint8_t ctrl[CTRL_MAX];
    if (lm->msg_control && lm->msg_controllen) {
        long n = cmsg_to_darwin((const void *)(uintptr_t)lm->msg_control,
                                (size_t)lm->msg_controllen, ctrl, sizeof ctrl);
        if (n < 0)
            return LERR(EINVAL);
        if (n > 0) {            // nothing left (only credentials): no control at all
            dm.msg_control = ctrl;
            dm.msg_controllen = (socklen_t)n;
        }
    }
    dm.msg_flags = lm->msg_flags;

    ssize_t r = sendmsg(fd, &dm, lxrt_msgflags_to_darwin(flags));
    return r < 0 ? LERR(errno) : (long)r;
}

long lxrt_recvmsg(int fd, void *lmsg, int flags)
{
    if (!lmsg)
        return LERR(EFAULT);
    struct linux_msghdr *lm = lmsg;

    struct msghdr dm;
    memset(&dm, 0, sizeof dm);
    struct sockaddr_storage ss;
    if (lm->msg_name && lm->msg_namelen) {
        dm.msg_name = &ss;
        dm.msg_namelen = sizeof ss;
    }
    dm.msg_iov = (struct iovec *)(uintptr_t)lm->msg_iov;
    dm.msg_iovlen = (int)lm->msg_iovlen;

    uint8_t ctrl[CTRL_MAX];
    if (lm->msg_control && lm->msg_controllen) {
        dm.msg_control = ctrl;
        dm.msg_controllen = sizeof ctrl;
    }

    long w = seqpkt_before_recv(fd, flags);
    if (w < 0)
        return w;
    ssize_t r = recvmsg(fd, &dm, lxrt_msgflags_to_darwin(flags));
    if (r < 0 && errno == ECONNRESET && lxrt_is_seqpacket(fd)) {
        lm->msg_controllen = 0;
        lm->msg_flags = 0;
        return 0;               // the peer is gone: end of file (see above)
    }
    if (r < 0)
        return LERR(errno);

    if (dm.msg_name && lm->msg_name) {
        uint32_t cap = lm->msg_namelen;
        addr_to_linux((struct sockaddr *)&ss, dm.msg_namelen,
                      (void *)(uintptr_t)lm->msg_name, &cap);
        lm->msg_namelen = cap;
    }
    if (lm->msg_control) {
        long n = cmsg_to_linux(ctrl, dm.msg_controllen,
                               (void *)(uintptr_t)lm->msg_control,
                               (size_t)lm->msg_controllen);
        if (n >= 0 && passcred_get(fd)) {
            // SO_PASSCRED: every message carries the sender's credentials.
            struct linux_ucred uc;
            size_t need = LALIGN((uint32_t)(sizeof(struct linux_cmsghdr) + sizeof uc));
            if (peer_ucred(fd, &uc) && (size_t)n + need <= (size_t)lm->msg_controllen) {
                struct linux_cmsghdr *lc = (struct linux_cmsghdr *)((uint8_t *)(uintptr_t)lm->msg_control + n);
                lc->cmsg_len = sizeof(*lc) + sizeof uc;
                lc->cmsg_level = L_SOL_SOCKET_OPT;
                lc->cmsg_type = L_SCM_CREDENTIALS;
                memcpy((uint8_t *)lc + sizeof(*lc), &uc, sizeof uc);
                n += (long)need;
            }
        }
        lm->msg_controllen = n < 0 ? 0 : (uint64_t)n;
    }
    lm->msg_flags = lxrt_msgflags_to_linux(dm.msg_flags);
    return (long)r;
}

// ------------------------------------------------- socket options
//
// The sixth silent number space. SOL_SOCKET is 1 on Linux and 0xffff on
// Darwin, and the SO_* constants underneath it share almost nothing:
//
//   option          Linux   Darwin        option        Linux   Darwin
//   SO_REUSEADDR        2   0x0004        SO_SNDBUF         7   0x1001
//   SO_TYPE             3   0x1008        SO_RCVBUF         8   0x1002
//   SO_ERROR            4   0x1007        SO_KEEPALIVE      9   0x0008
//   SO_BROADCAST        6   0x0020        SO_LINGER        13   0x0080
//
// Passing them through does not fail -- it sets a DIFFERENT option, or the
// same one with a nonsense value. Linux's SO_ERROR (4) reaches Darwin as
// SO_REUSEADDR|... and a getsockopt for the pending error silently returns
// something else.

#ifndef L_SOL_SOCKET_OPT
#define L_SOL_SOCKET_OPT 1
#endif

struct opt_map { int lopt, dopt; };

static const struct opt_map k_so[] = {
    { 1,  SO_DEBUG },      { 2,  SO_REUSEADDR },  { 3,  SO_TYPE },
    { 4,  SO_ERROR },      { 5,  SO_DONTROUTE },  { 6,  SO_BROADCAST },
    { 7,  SO_SNDBUF },     { 8,  SO_RCVBUF },     { 9,  SO_KEEPALIVE },
    { 10, SO_OOBINLINE },  { 13, SO_LINGER },     { 15, SO_REUSEPORT },
    { 18, SO_RCVLOWAT },   { 19, SO_SNDLOWAT },   { 20, SO_RCVTIMEO },
    { 21, SO_SNDTIMEO },   { 30, SO_ACCEPTCONN },
};

// Returns false when the option has no Darwin equivalent -- SO_PASSCRED and
// SO_PEERCRED above all, which Darwin only offers as LOCAL_PEERCRED on a
// different level with a different structure.
// IPPROTO_IP (0), IPPROTO_TCP (6) and IPPROTO_IPV6 (41) are the same LEVEL
// numbers on both systems, but most option numbers under them are not (the
// old comment here claimed they agreed; Xvnc's IPV6_V6ONLY -- Linux 26,
// Darwin 27 -- measured otherwise).
static const struct opt_map k_ip[] = {
    { 1, IP_TOS },            { 2, IP_TTL },            { 3, IP_HDRINCL },
    { 4, IP_OPTIONS },        { 8, IP_PKTINFO },        { 12, IP_RECVTTL },
    { 13, IP_RECVTOS },       { 32, IP_MULTICAST_IF },  { 33, IP_MULTICAST_TTL },
    { 34, IP_MULTICAST_LOOP }, { 35, IP_ADD_MEMBERSHIP }, { 36, IP_DROP_MEMBERSHIP },
};
static const struct opt_map k_ipv6[] = {
    { 16, IPV6_UNICAST_HOPS }, { 17, IPV6_MULTICAST_IF }, { 18, IPV6_MULTICAST_HOPS },
    { 19, IPV6_MULTICAST_LOOP }, { 20, IPV6_JOIN_GROUP }, { 21, IPV6_LEAVE_GROUP },
    { 26, IPV6_V6ONLY },       { 49, 61 /* Darwin IPV6_RECVPKTINFO (RFC 3542) */ }, { 50, 46 /* IPV6_PKTINFO */ },
    { 67, IPV6_TCLASS },
};
static const struct opt_map k_tcp[] = {
    { 1, TCP_NODELAY },        { 2, TCP_MAXSEG },         { 4, TCP_KEEPALIVE /* KEEPIDLE */ },
    { 5, TCP_KEEPINTVL },      { 6, TCP_KEEPCNT },        { 23, TCP_FASTOPEN },
};

// Linux-only knobs with no Darwin counterpart whose absence changes nothing a
// program can observe here; accepted and ignored rather than failed, since
// programs treat ENOPROTOOPT on them as fatal (Xvnc) or log noise.
bool lxrt_sockopt_ignorable(int llevel, int lopt)
{
    if (llevel == 0)  return lopt == 10 /* IP_MTU_DISCOVER */ || lopt == 11 /* IP_RECVERR */ ||
                             lopt == 15 /* IP_FREEBIND */ || lopt == 49 /* IP_BIND_ADDRESS_NO_PORT */;
    if (llevel == 6)  return lopt == 3 /* TCP_CORK */ || lopt == 9 /* TCP_DEFER_ACCEPT */ ||
                             lopt == 12 /* TCP_QUICKACK */ || lopt == 18 /* TCP_USER_TIMEOUT */ ||
                             lopt == 13 /* TCP_CONGESTION */;
    if (llevel == 41) return lopt == 23 /* IPV6_MTU_DISCOVER */ || lopt == 25 /* IPV6_RECVERR */;
    if (llevel == L_SOL_SOCKET_OPT)
        return lopt == 12 /* SO_PRIORITY */ || lopt == 25 /* SO_BINDTODEVICE */ ||
               lopt == 26 /* SO_ATTACH_FILTER */ || lopt == 29 /* SO_TIMESTAMP */ ||
               lopt == 32 /* SO_SNDBUFFORCE */ || lopt == 33 /* SO_RCVBUFFORCE */ ||
               lopt == 46 /* SO_BUSY_POLL */ || lopt == 36 /* SO_MARK */;
    return false;
}

static bool map_opt(const struct opt_map *m, size_t n, int lopt, int *dopt)
{
    for (size_t i = 0; i < n; i++)
        if (m[i].lopt == lopt) { *dopt = m[i].dopt; return true; }
    return false;
}

static bool sockopt_to_darwin(int llevel, int lopt, int *dlevel, int *dopt)
{
    if (llevel != L_SOL_SOCKET_OPT) {
        *dlevel = llevel;
        switch (llevel) {
        case 0:  return map_opt(k_ip, sizeof k_ip / sizeof k_ip[0], lopt, dopt);
        case 6:  return map_opt(k_tcp, sizeof k_tcp / sizeof k_tcp[0], lopt, dopt);
        case 41: return map_opt(k_ipv6, sizeof k_ipv6 / sizeof k_ipv6[0], lopt, dopt);
        default: *dopt = lopt; return true;   // other protocols: pass through
        }
    }
    *dlevel = SOL_SOCKET;
    for (unsigned i = 0; i < sizeof k_so / sizeof k_so[0]; i++)
        if (k_so[i].lopt == lopt) {
            *dopt = k_so[i].dopt;
            return true;
        }
    return false;
}

// SO_RCVTIMEO/SO_SNDTIMEO carry a timeval, and Linux's has a 64-bit tv_usec
// where Darwin's has 32 bits plus padding. Same size, different layout.
struct linux_timeval_opt { int64_t tv_sec; int64_t tv_usec; };
// SO_LINGER carries two ints on both, and they agree.

long lxrt_setsockopt(int fd, int llevel, int lopt, const void *val, unsigned len)
{
    if (llevel == L_SOL_SOCKET_OPT && lopt == L_SO_PASSCRED) {
        if (!val || len < 4) return LERR(EINVAL);
        passcred_set(fd, *(const int32_t *)val != 0);
        return 0;
    }
    // The uevent monitor's socket filter (SO_ATTACH_FILTER, a BPF program),
    // SO_RCVBUFFORCE, SOL_NETLINK memberships: nothing arrives to filter.
    if (is_netlink(fd))
        return 0;
    int dlevel, dopt;
    if (!sockopt_to_darwin(llevel, lopt, &dlevel, &dopt))
        return lxrt_sockopt_ignorable(llevel, lopt) ? 0 : LERR(ENOPROTOOPT);

    if (dlevel == SOL_SOCKET && (dopt == SO_RCVTIMEO || dopt == SO_SNDTIMEO)) {
        if (!val || len < sizeof(struct linux_timeval_opt))
            return LERR(EINVAL);
        const struct linux_timeval_opt *lt = val;
        struct timeval tv = { (time_t)lt->tv_sec, (suseconds_t)lt->tv_usec };
        return setsockopt(fd, dlevel, dopt, &tv, sizeof tv) != 0 ? LERR(errno) : 0;
    }
    if (setsockopt(fd, dlevel, dopt, val, (socklen_t)len) == 0)
        return 0;
    int e = errno;
    // Linux clamps an oversized SO_SNDBUF/SO_RCVBUF to net.core.[wr]mem_max
    // and succeeds; Darwin refuses anything above kern.ipc.maxsockbuf. The
    // Steam client asserts on the refusal ("Unable to set default socket
    // options, error 22"). Clamp the same way: the largest size accepted.
    if (dlevel == SOL_SOCKET && (dopt == SO_SNDBUF || dopt == SO_RCVBUF) &&
        (e == EINVAL || e == ENOBUFS) && val && len >= 4) {
        int want = *(const int *)val;
        for (int v = want / 2; v >= 4096; v /= 2)
            if (setsockopt(fd, dlevel, dopt, &v, sizeof v) == 0)
                return 0;
        // Below the minimum (the Steam client asks for 0): Linux rounds up
        // to SOCK_MIN_SNDBUF/SOCK_MIN_RCVBUF, a few KiB.
        if (want < 4608) {
            int v = 4608;
            if (setsockopt(fd, dlevel, dopt, &v, sizeof v) == 0)
                return 0;
        }
    }
    // Say which option, once each: "Unable to set default socket options,
    // error 22" from the Steam client names none.
    static _Atomic uint32_t said[64];
    uint32_t key = ((uint32_t)llevel << 16) ^ (uint32_t)lopt;
    bool fresh = true;
    for (int i = 0; i < 64; i++) {
        uint32_t cur = atomic_load(&said[i]);
        if (cur == key + 1) { fresh = false; break; }
        uint32_t z = 0;
        if (cur == 0 && atomic_compare_exchange_strong(&said[i], &z, key + 1))
            break;
    }
    if (fresh)
        fprintf(lxrt_trace_stream(), "[lxrt] setsockopt(level %d, opt %d, len %u, int %d) -> darwin (%d, %d): %s\n",
                llevel, lopt, len, val && len >= 4 ? *(const int *)val : 0, dlevel, dopt, strerror(e));
    return LERR(e);
}

long lxrt_getsockopt(int fd, int llevel, int lopt, void *val, unsigned *len)
{
    if (llevel == L_SOL_SOCKET_OPT && (lopt == L_SO_PASSCRED || lopt == L_SO_PEERCRED)) {
        if (!val || !len) return LERR(EFAULT);
        if (lopt == L_SO_PASSCRED) {
            if (*len < 4) return LERR(EINVAL);
            *(int32_t *)val = passcred_get(fd);
            *len = 4;
            return 0;
        }
        struct linux_ucred uc;
        if (!peer_ucred(fd, &uc)) return LERR(errno);
        if (*len < sizeof uc) return LERR(EINVAL);
        memcpy(val, &uc, sizeof uc);
        *len = sizeof uc;
        return 0;
    }
    int dlevel, dopt;
    if (!sockopt_to_darwin(llevel, lopt, &dlevel, &dopt)) {
        if (lxrt_sockopt_ignorable(llevel, lopt) && val && len && *len >= 4) {
            *(int32_t *)val = 0;
            *len = 4;
            return 0;
        }
        return LERR(ENOPROTOOPT);
    }

    if (dlevel == SOL_SOCKET && (dopt == SO_RCVTIMEO || dopt == SO_SNDTIMEO)) {
        if (!val || !len || *len < sizeof(struct linux_timeval_opt))
            return LERR(EINVAL);
        struct timeval tv;
        socklen_t tl = sizeof tv;
        if (getsockopt(fd, dlevel, dopt, &tv, &tl) != 0)
            return LERR(errno);
        struct linux_timeval_opt *lt = val;
        lt->tv_sec = tv.tv_sec;
        lt->tv_usec = tv.tv_usec;
        *len = sizeof(*lt);
        return 0;
    }

    socklen_t dl = len ? (socklen_t)*len : 0;
    if (getsockopt(fd, dlevel, dopt, val, &dl) != 0)
        return LERR(errno);
    if (len)
        *len = dl;
    // The seqpacket mark is the runtime's, not the guest's.
    if (dlevel == SOL_SOCKET && dopt == SO_LINGER && val && dl >= sizeof(struct linger) &&
        ((struct linger *)val)->l_onoff && ((struct linger *)val)->l_linger == SEQPKT_LINGER)
        memset(val, 0, sizeof(struct linger));
    return 0;
}
