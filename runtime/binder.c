// /dev/binder, /dev/hwbinder and /dev/vndbinder for guest processes.
//
// The driver itself is the hub (runtime/binder_hub.c, a host process started
// on demand). This side is what a guest process's syscalls reach:
//
//   open    connects to the hub (starting it if needed) and says hello; the
//           guest's descriptor IS that connection, the "process channel",
//           so poll and epoll on it work unchanged: the hub makes it
//           readable (one doorbell byte) when work waits for a polling
//           thread, as binder_poll would report;
//   ioctl   BINDER_WRITE_READ gathers what the kernel would copy from the
//           sender (the write bytes, each transaction's data and offsets,
//           scatter-gather buffers, the file descriptors named by
//           BINDER_TYPE_FD/FDA objects) and sends it on the calling
//           thread's own channel; the result brings the BR_* bytes for the
//           read buffer, descriptors to install (SCM_RIGHTS) and where in
//           the receive buffer their numbers go;
//   mmap    creates the receive buffer as a file mapped read-only for the
//           guest (Linux forbids PROT_WRITE on binder) and read-write for
//           the hub, which copies incoming transactions into it;
//   fork    the child keeps nothing (Linux: VM_DONTCOPY on the mapping, and
//           a binder_proc belongs to the process that opened it).
//
// A guest thread blocked in BINDER_WRITE_READ is blocked in recv() on its
// channel. A signal interrupts it: the hub is told (BH_CANCEL) and answers
// with the real result if it had one, else -EINTR, which libbinder retries
// and the dispatcher restarts under SA_RESTART with the consumed counts
// advanced, as Linux's restart does.
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <mach-o/dyld.h>

#include "lxrt.h"
#include "android_ids.h"
#include "binder.h"

#define LERR(e) (-lxrt_errno_to_linux(e))
#define ALIGN8(x) (((x) + 7) & ~(uint64_t)7)
#define B_FDS 65536
#define MAX_TCHAN 8
#define SZ_4M (4u << 20)

bool lxrt_trace_on(void);
long lxrt_guest_close_fd(int fd);       // dispatch.c: close() as the guest's own

extern char **environ;

struct bfile {
    int naliases;               // guest descriptors naming it
    int refs;                   // aliases + mapping + thread channels + in-flight calls
    int chan;                   // our own descriptor of the process channel
    int context;
    bool forked;                // inherited over fork(): unusable here
    bool closed;                // the guest closed its last descriptor
    uint64_t drained;           // doorbells read so far
    pthread_mutex_t lock;       // process-channel writes, doorbell reads
    pthread_mutex_t map_lock;   // the mapping: fd fixups against munmap
    uint64_t map_base, map_len, map_host_len;
    uint8_t *alias;             // our read-write view of the receive buffer
};

struct tchan {
    struct bfile *f;
    int fd;
    int hub_end;        // our copy of the hub's end, until the hub has answered (tchan_get)
    bool polled;
    bool busy;
    uint64_t seq;
};

static struct bfile *g_files[B_FDS];
static _Atomic int g_nopened;           // files ever opened (fork: skip the scan if none)
static struct bfile *g_mapped[16];      // files with a receive buffer (munmap checks)
static _Atomic int g_nmapped;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(binder_g_lock, g_lock)

static _Thread_local struct tchan t_chans[MAX_TCHAN];
static _Thread_local int t_nchans;

// Every channel of this process to the hub -- each binder file's process
// channel and each thread's -- so a fork child can close the parent's (their
// threads do not exist in the child) and a guest close() cannot cut one
// (lxrt_binder_owns).
static int g_chan_fds[1024];
static int g_nchan_fds;

static void chan_register(int fd)
{
    pthread_mutex_lock(&g_lock);
    if (g_nchan_fds < (int)(sizeof g_chan_fds / sizeof g_chan_fds[0]))
        g_chan_fds[g_nchan_fds++] = fd;
    pthread_mutex_unlock(&g_lock);
}
static void chan_unregister(int fd)
{
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nchan_fds; i++)
        if (g_chan_fds[i] == fd) { g_chan_fds[i] = g_chan_fds[--g_nchan_fds]; break; }
    pthread_mutex_unlock(&g_lock);
}

// ------------------------------------------------------------ guest memory

// A guest pointer is the guest's choice; reading or writing through a bad
// one must be EFAULT, not the runtime dying. mach_vm_read_overwrite copies
// within this task and reports a bad range instead of faulting.
static bool gcopy(void *dst, const void *src, size_t n)
{
    if (!n) return true;
    if (!dst || !src) return false;
    mach_vm_size_t out = 0;
    kern_return_t kr = mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)src,
                                              n, (mach_vm_address_t)(uintptr_t)dst, &out);
    return kr == KERN_SUCCESS && out == n;
}

// A 32-bit guest (i386 Android under FEX's 32-bit mode) lives at host =
// guest base + guest address, and the addresses inside binder's structures
// -- binder_write_read's buffers, a transaction's data and offsets, a PTR
// object's buffer -- are its own, 32 bits in 64-bit fields (Android's binder
// ABI on a 64-bit kernel). Below 4 GiB an address is always a guest one on
// this host (gbase.c's rule), so the base is added where this file reads or
// writes through such an address. The other way, the hub is told the
// receive buffer's guest address (lxrt_binder_mmap), so every address it
// hands back (BR_TRANSACTION's data, PTR buffers in the target) is the
// guest's. Before: "Binder ioctl to obtain version failed: Bad address"
// and the 32-bit audio HAL ended there (benchmarks/stage28-android-
// reliability.txt).
static uint64_t gp(uint64_t a)
{
    uint64_t b = lxrt_gbase();
    return (b && a && a < (1ull << 32)) ? a + b : a;
}

// ------------------------------------------------------------ files

static struct bfile *file_get(int fd)
{
    if (fd < 0 || fd >= B_FDS) return NULL;
    pthread_mutex_lock(&g_lock);
    struct bfile *f = g_files[fd];
    if (f) f->refs++;
    pthread_mutex_unlock(&g_lock);
    return f;
}

static void file_put(struct bfile *f)
{
    pthread_mutex_lock(&g_lock);
    bool last = --f->refs == 0;
    pthread_mutex_unlock(&g_lock);
    if (!last) return;
    if (f->chan >= 0) { chan_unregister(f->chan); close(f->chan); }
    pthread_mutex_destroy(&f->lock);
    pthread_mutex_destroy(&f->map_lock);
    free(f);
}

bool lxrt_binder_any(void) { return atomic_load(&g_nopened) != 0; }

bool lxrt_binder_is(int fd)
{
    return fd >= 0 && fd < B_FDS && g_files[fd] != NULL;
}

int lxrt_binder_context_of(const char *p)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("LXRT_BINDER");
        enabled = !(e && !strcmp(e, "0"));
    }
    if (!enabled || !p || strncmp(p, "/dev/", 5) != 0) return -1;
    p += 5;
    if (!strncmp(p, "binderfs/", 9)) p += 9;
    if (!strcmp(p, "binder")) return 0;
    if (!strcmp(p, "hwbinder")) return 1;
    if (!strcmp(p, "vndbinder")) return 2;
    return -1;
}

bool lxrt_binder_stat(const char *p, struct stat *st)
{
    if (lxrt_binder_context_of(p) < 0) return false;
    memset(st, 0, sizeof *st);
    // What a binderfs node reports: a character device, 0600 root... here
    // owned by the caller, since every process of the guest shares one uid.
    st->st_mode = S_IFCHR | 0666;
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_nlink = 1;
    st->st_rdev = (dev_t)((10 << 24) | (60 + lxrt_binder_context_of(p)));
    return true;
}

bool lxrt_binder_fstat(int fd, struct stat *st)
{
    struct bfile *f = file_get(fd);
    if (!f) return false;
    static const char *const names[] = { "/dev/binder", "/dev/hwbinder", "/dev/vndbinder" };
    bool ok = lxrt_binder_stat(names[f->context < 3 ? f->context : 0], st);
    file_put(f);
    return ok;
}

// Descriptors the runtime keeps for itself sit high, out of the guest's way
// (a guest that dup2()s onto a low number must not hit one).
static int hide_fd(int fd)
{
    static const int bases[] = { 800, 400, 200, 64 };
    for (size_t i = 0; i < sizeof bases / sizeof bases[0]; i++) {
        int n = fcntl(fd, F_DUPFD_CLOEXEC, bases[i]);
        if (n >= 0) { close(fd); return n; }
    }
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

static const char *binder_dir(void);

// Why a binder call failed in a way the guest cannot explain ("Bad file
// descriptor" from BinderProxy.transactNative with a driver that is there):
// one line per event in <binder dir>/client.log. Guests' stderr is often
// /dev/null (the zygote's children), so it goes to a file of the hub's
// directory. Costs nothing on the success paths.
static void binder_diag(const char *fmt, ...)
{
    const char *dir = binder_dir();
    if (!dir) return;
    char path[300], line[400];
    snprintf(path, sizeof path, "%s/client.log", dir);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(line, sizeof line, "[%ld.%03ld pid %d tid %ld] ", (long)ts.tv_sec, ts.tv_nsec / 1000000,
                     getpid(), (long)lxrt_gettid());
    if (n > 0 && n < (int)sizeof line) vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
    va_end(ap);
    size_t l = strnlen(line, sizeof line - 2);
    line[l++] = '\n';
    int fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0) {
        ssize_t w = write(fd, line, l);
        (void)w;
        close(fd);
    }
}

// The hub's directory. In /tmp by default, so it must be this user's own
// and closed to others: a directory someone else made (or can write to)
// would let them stand in for the driver. NULL: refuse to use it.
static const char *binder_dir(void)
{
    static char dir[128];
    static bool checked, ok;
    if (checked) return ok ? dir : NULL;
    checked = true;
    const char *e = getenv("LXRT_BINDER_DIR");
    if (e && *e) snprintf(dir, sizeof dir, "%s", e);
    else snprintf(dir, sizeof dir, "/tmp/lxrt-binder-%u", (unsigned)getuid());
    mkdir(dir, 0700);
    struct stat st;
    ok = lstat(dir, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == getuid() &&
         (st.st_mode & 077) == 0;
    if (!ok)
        fprintf(lxrt_trace_stream(), "[lxrt] binder: %s is not a private directory of this "
                                     "user; no binder driver\n", dir);
    return ok ? dir : NULL;
}

static const char *self_exe(void)
{
    static char path[4096];
    if (path[0]) return path;
    uint32_t sz = sizeof path;
    if (_NSGetExecutablePath(path, &sz) != 0) path[0] = 0;
    return path;
}

static int try_connect(const char *dir)
{
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    if (snprintf(sa.sun_path, sizeof sa.sun_path, "%s/hub.sock", dir) >= (int)sizeof sa.sun_path)
        return -ENAMETOOLONG;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    if (s < 0) return -errno;
    if (connect(s, (struct sockaddr *)&sa, sizeof sa) != 0) {
        int e = errno;
        close(s);
        return -e;
    }
    return s;
}

// The hub is `lxrun --binder-hub <dir> --daemon`: its first process binds the
// socket, forks the real hub and exits, so once waitpid returns the hub is
// listening and is nobody's child. No guest descriptor goes with it.
static int spawn_hub(const char *dir)
{
    const char *exe = self_exe();
    if (!exe[0]) return -ENOENT;
    posix_spawnattr_t at;
    posix_spawn_file_actions_t fa;
    posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_CLOEXEC_DEFAULT);
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *env[8];
    int ne = 0;
    for (char **e = environ; *e && ne < 6; e++)
        if (!strncmp(*e, "LXRT_BINDER", 11)) env[ne++] = *e;
    env[ne] = NULL;
    char *argv[] = { (char *)exe, "--binder-hub", (char *)dir, "--daemon", NULL };
    pid_t pid;
    int rc = posix_spawn(&pid, exe, &fa, &at, argv, env);
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (rc != 0) return -rc;
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

static int send_msg(int fd, const struct bh_hdr *h, const void *pl, size_t plen,
                    const int *fds, int nfds)
{
    struct iovec iov[2] = { { (void *)h, sizeof *h }, { (void *)pl, plen } };
    union { struct cmsghdr c; char b[CMSG_SPACE(sizeof(int) * BH_MAX_FDS)]; } cm;
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = plen ? 2 : 1 };
    if (nfds > 0) {
        m.msg_control = cm.b;
        m.msg_controllen = (socklen_t)CMSG_SPACE(sizeof(int) * (size_t)nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = (socklen_t)CMSG_LEN(sizeof(int) * (size_t)nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * (size_t)nfds);
    }
    // A message is never abandoned half-sent: the hub reads it whole.
    int idx = 0, niov = m.msg_iovlen;
    while (idx < niov) {
        struct msghdr mm = m;
        mm.msg_iov = iov + idx;
        mm.msg_iovlen = niov - idx;
        ssize_t n = sendmsg(fd, &mm, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {      // a guest's O_NONBLOCK on the shared socket
                struct pollfd p = { fd, POLLOUT, 0 };
                poll(&p, 1, 100);
                continue;
            }
            return -errno;
        }
        m.msg_control = NULL;
        m.msg_controllen = 0;
        while (n > 0 && idx < niov) {
            if ((size_t)n >= iov[idx].iov_len) { n -= (ssize_t)iov[idx].iov_len; idx++; }
            else { iov[idx].iov_base = (char *)iov[idx].iov_base + n; iov[idx].iov_len -= (size_t)n; n = 0; }
        }
        while (idx < niov && iov[idx].iov_len == 0) idx++;
    }
    return 0;
}

// Receive one message. *interrupted is set when a signal cut the wait short
// (the caller decides what that means); fds get FD_CLOEXEC, as binder
// installs them.
static _Thread_local const char *t_recv_fail;
static _Thread_local int t_recv_errno;

static int recv_msg(int fd, struct bh_hdr *h, uint8_t **pl, int *fds, int *nfds,
                    bool *interrupted)
{
    t_recv_fail = "";
    t_recv_errno = 0;
    *pl = NULL;
    *nfds = 0;
    union { struct cmsghdr c; char b[CMSG_SPACE(sizeof(int) * BH_MAX_FDS)]; } cm;
    size_t got = 0;
    while (got < sizeof *h) {
        struct iovec iov = { (char *)h + got, sizeof *h - got };
        struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cm.b,
                            .msg_controllen = sizeof cm.b };
        ssize_t n = recvmsg(fd, &m, 0);
        if (n < 0 && errno == EINTR) {
            if (interrupted) *interrupted = true;
            if (got == 0 && interrupted) return -EINTR;
            continue;
        }
        if (n <= 0) {
            t_recv_errno = n < 0 ? errno : 0;
            t_recv_fail = n < 0 ? "header recvmsg failed" : got ? "end of file in the header" : "end of file";
            goto fail;
        }
        if (m.msg_flags & (MSG_CTRUNC | MSG_TRUNC))
            binder_diag("channel %d: message flags 0x%x (truncated control data: descriptors lost)", fd, m.msg_flags);
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
            int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            for (int i = 0; i < k; i++) {
                int f;
                memcpy(&f, CMSG_DATA(c) + i * sizeof(int), sizeof f);
                fcntl(f, F_SETFD, FD_CLOEXEC);
                if (*nfds < BH_MAX_FDS) fds[(*nfds)++] = f;
                else close(f);
            }
        }
        got += (size_t)n;
    }
    if (h->len > BH_MAX_MSG) { t_recv_fail = "message too large"; goto fail; }
    if (h->len) {
        *pl = malloc(h->len);
        if (!*pl) { t_recv_fail = "out of memory"; goto fail; }
        size_t g2 = 0;
        while (g2 < h->len) {
            ssize_t n = recv(fd, *pl + g2, h->len - g2, 0);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                t_recv_errno = n < 0 ? errno : 0;
                t_recv_fail = n < 0 ? "payload recv failed" : "end of file in the payload";
                goto fail;
            }
            g2 += (size_t)n;
        }
    }
    return 0;
fail:
    free(*pl);
    *pl = NULL;
    for (int i = 0; i < *nfds; i++) close(fds[i]);
    *nfds = 0;
    return -EPIPE;
}

// ------------------------------------------------------------ open

long lxrt_binder_open(int context, int lflags)
{
    const char *dir = binder_dir();
    if (!dir)
        return LERR(ENOENT);
    int s = -1;
    for (int attempt = 0; attempt < 4 && s < 0; attempt++) {
        s = try_connect(dir);
        if (s < 0) {
            char lp[256];
            snprintf(lp, sizeof lp, "%s/hub.lock", dir);
            int lk = open(lp, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
            if (lk >= 0) flock(lk, LOCK_EX);
            s = try_connect(dir);
            if (s < 0) {
                int r = spawn_hub(dir);
                if (r < 0 && lxrt_trace_on())
                    fprintf(lxrt_trace_stream(), "[lxrt] binder: cannot start the hub: %s\n", strerror(-r));
                s = try_connect(dir);
            }
            if (lk >= 0) { flock(lk, LOCK_UN); close(lk); }
            if (s < 0) continue;
        }
        // Hello. A hub that was just exiting closes on us: try again.
        // The uid binder peers see (getCallingUid) is the Mac user's unless
        // LXRT_BINDER_UID names an Android one: what init's `user` line gives
        // a service (surfaceflinger runs as system, 1000; bootanimation as
        // graphics, 1003). Every guest is the same Mac user, and the hub
        // trusts its peers' word already, so this grants nothing a guest could
        // not claim anyway; it lets Android's own uid checks pass the way they
        // do on a device (SurfaceFlinger admits AID_GRAPHICS and AID_SYSTEM
        // without asking system_server's permission service, which is not
        // running: benchmarks/stage27-android-display.txt). Without it, the
        // process's Android ids (runtime/android_ids.c) decide, as init's
        // `user` line does when scripts/android-boot.py starts a service.
        uint32_t euid = lxrt_aids_euid();
        const char *ue = getenv("LXRT_BINDER_UID");
        if (ue && *ue) {
            char *end = NULL;
            unsigned long v = strtoul(ue, &end, 10);
            if (end && !*end && v < 0x80000000ul)
                euid = (uint32_t)v;
        }
        struct bh_hello hello = { .version = BH_VERSION, .pid = getpid(), .euid = euid,
                                  .context = (uint32_t)context };
        const char *sc = getenv("LXRT_BINDER_SECCTX");
        snprintf(hello.secctx, sizeof hello.secctx, "%s", sc && *sc ? sc : "u:r:unlabeled:s0");
        struct bh_hdr h = { .type = BH_HELLO, .len = sizeof hello };
        struct timeval tv = { 10, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        struct bh_hdr ah;
        uint8_t *pl = NULL;
        int fds[1], nfds = 0;
        int hs = send_msg(s, &h, &hello, sizeof hello, NULL, 0);
        int hr = hs ? 0 : recv_msg(s, &ah, &pl, fds, &nfds, NULL);
        if (hs != 0 || hr != 0 || ah.type != BH_HELLO_ACK ||
            ah.len < sizeof(struct bh_hello_ack)) {
            binder_diag("open, attempt %d: hello %s (errno %d)", attempt + 1,
                        hs ? "not sent" : hr ? "not answered" : "answered wrongly", errno);
            free(pl);
            close(s);
            s = -1;
            continue;
        }
        struct bh_hello_ack ack;
        memcpy(&ack, pl, sizeof ack);
        free(pl);
        if (ack.ret) {
            close(s);
            return ack.ret;
        }
        struct timeval zero = { 0, 0 };
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &zero, sizeof zero);
    }
    if (s < 0) {
        binder_diag("open: no hub in %s after 4 attempts", dir);
        return LERR(ENOENT);        // no driver: what an absent /dev/binder says
    }
    if (s >= B_FDS) {
        close(s);
        return LERR(EMFILE);
    }
    struct bfile *f = calloc(1, sizeof *f);
    if (!f) { close(s); return LERR(ENOMEM); }
    int d = dup(s);
    if (d < 0) { free(f); close(s); return LERR(errno); }
    f->chan = hide_fd(d);
    chan_register(f->chan);
    f->context = context;
    f->naliases = 1;
    f->refs = 1;
    pthread_mutex_init(&f->lock, NULL);
    pthread_mutex_init(&f->map_lock, NULL);
    // Linux flags: O_CLOEXEC 0x80000, O_NONBLOCK 0x800.
    if (lflags & 0x80000) fcntl(s, F_SETFD, FD_CLOEXEC);
    if (lflags & 0x800) fcntl(s, F_SETFL, fcntl(s, F_GETFL) | O_NONBLOCK);
    pthread_mutex_lock(&g_lock);
    g_files[s] = f;
    pthread_mutex_unlock(&g_lock);
    atomic_fetch_add(&g_nopened, 1);
    return s;
}

// ------------------------------------------------------------ thread channels

static void tchan_drop(struct tchan *tc)
{
    chan_unregister(tc->fd);
    close(tc->fd);
    if (tc->hub_end >= 0) { chan_unregister(tc->hub_end); close(tc->hub_end); }
    struct bfile *f = tc->f;
    *tc = t_chans[--t_nchans];
    memset(&t_chans[t_nchans], 0, sizeof t_chans[0]);
    file_put(f);
}

static struct tchan *tchan_get(struct bfile *f, long *err)
{
    for (int i = 0; i < t_nchans; i++)
        if (t_chans[i].f == f) return &t_chans[i];
    // Channels of files closed since: gone with them. Not while a call of
    // this thread is in flight (a signal handler's): its entry must not move.
    bool busy = false;
    for (int i = 0; i < t_nchans; i++) busy |= t_chans[i].busy;
    for (int i = 0; !busy && i < t_nchans; )
        if (t_chans[i].f->closed) tchan_drop(&t_chans[i]);
        else i++;
    if (t_nchans >= MAX_TCHAN) { *err = LERR(ENOMEM); return NULL; }
    int mine = -1, hubs = -1;
    for (int attempt = 1; ; attempt++) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) { *err = LERR(errno); return NULL; }
        mine = hide_fd(sv[0]);
        // The hub's end goes to the hub by SCM_RIGHTS, and this process keeps
        // a copy of it until the hub has acknowledged the channel. Closed at
        // once, the end in flight had no reference but the message, and
        // XNU's unix-socket garbage collection (unp_gc) can flush such a
        // socket (HYPOTHESIS for the mechanism): the hub received a channel
        // already at end of file, dropped the thread, and the guest's call
        // failed with EBADF ("Bad file descriptor" from
        // BinderProxy.transactNative; system_server died of it several times
        // a boot, MEASURED at stage 28).
        hubs = hide_fd(sv[1]);
        int sz = 1 << 20;
        setsockopt(mine, SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
        setsockopt(mine, SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
        struct bh_thread bt = { .tid = lxrt_gettid() };
        struct bh_hdr h = { .type = BH_THREAD, .nfds = 1, .len = sizeof bt };
        pthread_mutex_lock(&f->lock);
        int r = send_msg(f->chan, &h, &bt, sizeof bt, &hubs, 1);
        pthread_mutex_unlock(&f->lock);
        if (r != 0) {
            binder_diag("new thread channel: send on the file's channel %d failed: %s", f->chan, strerror(-r));
            close(hubs);
            close(mine);
            *err = LERR(EBADF);
            return NULL;
        }
        // The hub acknowledges the channel on it, and refuses one that reaches
        // it already at end of file: a few per boot even with our copy held
        // (cause UNKNOWN, benchmarks/stage28-android-apk.txt); the thread's
        // first call then waited forever (am start -W for 300 s, MEASURED).
        // A new channel instead, a few times at most.
        bool acked = false;
        for (int waited = 0; waited < 5000 && !acked; ) {
            struct pollfd pfd = { mine, POLLIN, 0 };
            int pr = poll(&pfd, 1, 100);
            if (pr < 0 && errno == EINTR) continue;
            waited += 100;
            if (pr <= 0) continue;
            struct bh_hdr ah;
            uint8_t *apl = NULL;
            int afds[BH_MAX_FDS], anfds = 0;
            if (recv_msg(mine, &ah, &apl, afds, &anfds, NULL) != 0) break;
            free(apl);
            for (int i = 0; i < anfds; i++) close(afds[i]);
            acked = ah.type == BH_THREAD_ACK;
            if (!acked) break;
        }
        if (acked) break;
        close(hubs);
        close(mine);
        if (attempt >= 4) {
            binder_diag("new thread channel: not acknowledged by the hub after %d attempts", attempt);
            *err = LERR(EBADF);
            return NULL;
        }
        binder_diag("new thread channel: not acknowledged by the hub (attempt %d); another one", attempt);
    }
    close(hubs);                        // acknowledged: the hub has its end
    hubs = -1;
    chan_register(mine);
    pthread_mutex_lock(&g_lock);
    f->refs++;
    pthread_mutex_unlock(&g_lock);
    struct tchan *tc = &t_chans[t_nchans++];
    *tc = (struct tchan){ .f = f, .fd = mine, .hub_end = hubs };
    return tc;
}

// Doorbells the hub has written and no longer wants pending: read them, so
// the guest's descriptor stops being readable exactly when binder_poll would.
static void drain_to(struct bfile *f, uint64_t target)
{
    pthread_mutex_lock(&f->lock);
    while (f->drained < target) {
        char buf[64];
        uint64_t want = target - f->drained;
        ssize_t n = recv(f->chan, buf, want < sizeof buf ? (size_t)want : sizeof buf, MSG_DONTWAIT);
        if (n <= 0) break;
        f->drained += (uint64_t)n;
    }
    pthread_mutex_unlock(&f->lock);
}

struct result {
    struct bh_result r;
    uint8_t *pl;                // owns: payload
    const uint8_t *rd;          // BR bytes
    const uint64_t *fix;
    const int32_t *closes;
    int fds[BH_MAX_FDS];
    int nfds;
};

// One request, one result. A signal while waiting turns into BH_CANCEL; the
// result that follows is either the real one or -EINTR.
static long roundtrip(struct tchan *tc, uint32_t type, const void *pl, size_t plen,
                      const int *fds, int nfds, struct result *out)
{
    struct bfile *f = tc->f;
    uint64_t seq = ++tc->seq;
    struct bh_hdr h = { .type = type, .nfds = (uint32_t)nfds, .len = plen, .seq = seq };
    int sr = send_msg(tc->fd, &h, pl, plen, fds, nfds);
    if (sr != 0) {
        binder_diag("request type %u on thread channel %d: send failed: %s", type, tc->fd, strerror(-sr));
        return LERR(EBADF);
    }
    bool cancelled = false;
    while (1) {
        struct bh_hdr rh;
        bool intr = false;
        int r = recv_msg(tc->fd, &rh, &out->pl, out->fds, &out->nfds, &intr);
        if (r == -EINTR) {
            if (!cancelled) {
                struct bh_hdr ch = { .type = BH_CANCEL, .seq = seq };
                send_msg(tc->fd, &ch, NULL, 0, NULL, 0);
                cancelled = true;
            }
            continue;
        }
        if (r != 0) {
            binder_diag("request type %u on thread channel %d: no answer: %s%s%s", type, tc->fd, t_recv_fail,
                        t_recv_errno ? ", " : "", t_recv_errno ? strerror(t_recv_errno) : "");
            return LERR(EBADF);             // the hub is gone: no driver any more
        }
        if (rh.type != BH_RESULT || rh.seq != seq || rh.len < sizeof(struct bh_result)) {
            free(out->pl);
            for (int i = 0; i < out->nfds; i++) close(out->fds[i]);
            out->pl = NULL;
            out->nfds = 0;
            if (rh.seq != seq) continue;    // a stale answer: keep waiting
            return LERR(EIO);
        }
        if (tc->hub_end >= 0) {         // the hub has its end: ours can go
            chan_unregister(tc->hub_end);
            close(tc->hub_end);
            tc->hub_end = -1;
        }
        memcpy(&out->r, out->pl, sizeof out->r);
        uint64_t need = sizeof out->r + out->r.read_len + 8ull * out->r.nfixups + 4ull * out->r.ncloses;
        if (need > rh.len || out->r.nfixups != (uint32_t)out->nfds) {
            free(out->pl);
            for (int i = 0; i < out->nfds; i++) close(out->fds[i]);
            out->pl = NULL;
            out->nfds = 0;
            return LERR(EIO);
        }
        out->rd = out->pl + sizeof out->r;
        out->fix = (const uint64_t *)(void *)(out->rd + out->r.read_len);
        out->closes = (const int32_t *)(void *)((const uint8_t *)out->fix + 8ull * out->r.nfixups);
        break;
    }
    if (!out->r.bell_pending)
        drain_to(f, out->r.bells);
    return 0;
}

static void result_done(struct result *res)
{
    // Descriptors of FDA buffers the guest freed: Linux closes them in the
    // freeing process (binder_deferred_fd_close).
    if (res->pl)
        for (uint32_t i = 0; i < res->r.ncloses; i++) {
            int32_t fd;
            memcpy(&fd, res->closes + i, 4);
            if (fd >= 0 && fd < B_FDS && g_files[fd])
                binder_diag("FDA buffer freed: descriptor %d to close is a binder descriptor now", fd);
            struct stat cst;
            if (fd >= 0 && fstat(fd, &cst) == 0 && S_ISSOCK(cst.st_mode))
                binder_diag("FDA buffer freed: descriptor %d to close is a socket", fd);
            else if (fd >= 0 && fcntl(fd, F_GETFD) < 0)
                binder_diag("FDA buffer freed: descriptor %d to close is not open", fd);
            if (fd >= 0) lxrt_guest_close_fd(fd);
        }
    free(res->pl);
    res->pl = NULL;
}

// ------------------------------------------------------------ BINDER_WRITE_READ

struct gbuf { uint8_t *p; size_t len, cap; bool oom; };

static uint8_t *gb_grow(struct gbuf *b, size_t n)
{
    if (b->oom) return NULL;
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->len + n) nc *= 2;
        uint8_t *np = realloc(b->p, nc);
        if (!np) { b->oom = true; return NULL; }
        b->p = np;
        b->cap = nc;
    }
    uint8_t *at = b->p + b->len;
    b->len += n;
    return at;
}

static void gb_pad8(struct gbuf *b)
{
    size_t pad = ALIGN8(b->len) - b->len;
    uint8_t *z = gb_grow(b, pad);
    if (z) memset(z, 0, pad);
}

// What the kernel copies from the sender for one transaction (binder_
// transaction's copy_from_user calls), appended to `b`; descriptors named by
// FD and FDA objects are appended to `fds`.
static void gather_txn(struct gbuf *b, const struct binder_transaction_data *tr,
                       uint64_t extra, int *fds, int *nfds)
{
    size_t hat = b->len;
    struct bh_txn_att a = { 0 };
    if (!gb_grow(b, sizeof a)) return;
    int fd0 = *nfds;
    if (tr->data_size > BH_MAX_MSG / 2 || tr->offsets_size > BH_MAX_MSG / 2 ||
        extra > BH_MAX_MSG / 2) {
        a.fault = 28;               // ENOSPC: larger than any receive buffer
        goto out;
    }
    size_t dat = b->len;
    uint8_t *d = gb_grow(b, (size_t)ALIGN8(tr->data_size));
    if (!d) { a.fault = 12; goto out; }
    memset(d + tr->data_size, 0, ALIGN8(tr->data_size) - tr->data_size);
    if (!gcopy(d, (const void *)(uintptr_t)gp(tr->data.ptr.buffer), tr->data_size)) {
        a.fault = 14;               // EFAULT
        b->len = dat;
        goto out;
    }
    size_t oat = b->len;
    uint8_t *o = gb_grow(b, (size_t)ALIGN8(tr->offsets_size));
    if (!o) { a.fault = 12; goto out; }
    memset(o + tr->offsets_size, 0, ALIGN8(tr->offsets_size) - tr->offsets_size);
    if (!gcopy(o, (const void *)(uintptr_t)gp(tr->data.ptr.offsets), tr->offsets_size)) {
        a.fault = 14;
        b->len = dat;
        goto out;
    }
    a.data_size = tr->data_size;
    a.offsets_size = tr->offsets_size;
    // Objects, in the order the hub will translate them. Only PTR contents and
    // descriptors are needed here; validity is the hub's to judge, so a bad
    // object just ends the walk (the hub then fails the transaction).
    size_t sgat = b->len;
    uint64_t nobj = tr->offsets_size / 8;
    struct { uint64_t off; size_t sg; uint64_t len; bool ptr; } *objs =
        nobj ? calloc((size_t)nobj, sizeof *objs) : NULL;
    if (nobj && !objs) { a.fault = 12; b->len = dat; goto out; }
    for (uint64_t i = 0; i < nobj; i++) {
        uint64_t off;
        memcpy(&off, b->p + oat + 8 * i, 8);
        if (off > tr->data_size || tr->data_size - off < 8 || (off & 3)) break;
        uint32_t type;
        memcpy(&type, b->p + dat + off, 4);
        objs[i].off = off;
        if (type == BINDER_TYPE_FD) {
            if (tr->data_size - off < sizeof(struct binder_fd_object)) break;
            struct binder_fd_object fo;
            memcpy(&fo, b->p + dat + off, sizeof fo);
            if (*nfds >= BH_MAX_FDS) { a.fault = 24; break; }        // EMFILE
            if (fcntl((int)fo.fd, F_GETFD) < 0) { a.fault = 9; break; }  // EBADF
            fds[(*nfds)++] = (int)fo.fd;
        } else if (type == BINDER_TYPE_PTR) {
            if (tr->data_size - off < sizeof(struct binder_buffer_object)) break;
            struct binder_buffer_object bp;
            memcpy(&bp, b->p + dat + off, sizeof bp);
            if (bp.length > extra) break;
            size_t at = b->len;
            uint8_t *s = gb_grow(b, (size_t)ALIGN8(bp.length));
            if (!s) { a.fault = 12; break; }
            memset(s + bp.length, 0, ALIGN8(bp.length) - bp.length);
            if (!gcopy(s, (const void *)(uintptr_t)gp(bp.buffer), bp.length)) { a.fault = 14; break; }
            objs[i].ptr = true;
            objs[i].sg = at;
            objs[i].len = bp.length;
        } else if (type == BINDER_TYPE_FDA) {
            if (tr->data_size - off < sizeof(struct binder_fd_array_object)) break;
            struct binder_fd_array_object fda;
            memcpy(&fda, b->p + dat + off, sizeof fda);
            if (fda.parent >= i || !objs[fda.parent].ptr) break;
            uint64_t plen = objs[fda.parent].len;
            if (fda.num_fds > plen / 4 || fda.parent_offset > plen - 4 * fda.num_fds) break;
            const uint8_t *arr = b->p + objs[fda.parent].sg + fda.parent_offset;
            bool bad = false;
            for (uint64_t k = 0; k < fda.num_fds; k++) {
                int32_t fd;
                memcpy(&fd, arr + 4 * k, 4);
                if (*nfds >= BH_MAX_FDS) { a.fault = 24; bad = true; break; }
                if (fcntl(fd, F_GETFD) < 0) { a.fault = 9; bad = true; break; }
                fds[(*nfds)++] = fd;
            }
            if (bad) break;
        } else if (type != BINDER_TYPE_BINDER && type != BINDER_TYPE_WEAK_BINDER &&
                   type != BINDER_TYPE_HANDLE && type != BINDER_TYPE_WEAK_HANDLE) {
            break;
        }
    }
    free(objs);
    if (a.fault) {
        b->len = dat;
        a.data_size = a.offsets_size = 0;
        *nfds = fd0;
        goto out;
    }
    a.sg_size = b->len - sgat;
    a.nfds = (uint32_t)(*nfds - fd0);
out:
    if (!b->oom)
        memcpy(b->p + hat, &a, sizeof a);
}

static long write_read(struct tchan *tc, uint64_t ubwr, int guest_fd)
{
    struct binder_write_read bwr;
    if (!gcopy(&bwr, (const void *)(uintptr_t)ubwr, sizeof bwr))
        return LERR(EFAULT);
    uint64_t wlen = 0;
    if (bwr.write_size > 0 && bwr.write_consumed < bwr.write_size)
        wlen = bwr.write_size - bwr.write_consumed;
    if (wlen > BH_MAX_MSG / 2)
        return LERR(EINVAL);
    uint64_t ravail = 0;
    if (bwr.read_size > 0 && bwr.read_consumed < bwr.read_size)
        ravail = bwr.read_size - bwr.read_consumed;

    struct gbuf b = { 0 };
    struct bh_wr wr = { .write_len = wlen, .read_avail = ravail, .read_consumed = bwr.read_consumed };
    int fl = fcntl(guest_fd, F_GETFL);
    if (fl >= 0 && (fl & O_NONBLOCK)) wr.flags |= BH_WR_NONBLOCK;
    if (!gb_grow(&b, sizeof wr)) return LERR(ENOMEM);
    size_t wat = b.len;
    uint8_t *w = gb_grow(&b, (size_t)wlen);
    if (wlen && !w) { free(b.p); return LERR(ENOMEM); }
    if (!gcopy(w, (const void *)(uintptr_t)(gp(bwr.write_buffer) + bwr.write_consumed), wlen)) {
        free(b.p);
        return LERR(EFAULT);
    }
    gb_pad8(&b);
    // Attachments: one per transaction command, in order.
    int fds[BH_MAX_FDS], nfds = 0;
    for (uint64_t pos = 0; pos + 4 <= wlen; ) {
        uint32_t cmd;
        memcpy(&cmd, b.p + wat + pos, 4);
        uint64_t sz = LX_IOC_SIZE(cmd);
        if (pos + 4 + sz > wlen) break;
        if (cmd == BC_TRANSACTION || cmd == BC_REPLY || cmd == BC_TRANSACTION_SG || cmd == BC_REPLY_SG) {
            struct binder_transaction_data tr;
            memcpy(&tr, b.p + wat + pos + 4, sizeof tr);
            uint64_t extra = 0;
            if (cmd == BC_TRANSACTION_SG || cmd == BC_REPLY_SG)
                memcpy(&extra, b.p + wat + pos + 4 + sizeof tr, 8);
            gather_txn(&b, &tr, extra, fds, &nfds);
        }
        pos += 4 + sz;
    }
    if (b.oom) { free(b.p); return LERR(ENOMEM); }
    memcpy(b.p, &wr, sizeof wr);

    struct result res = { 0 };
    long rc = roundtrip(tc, BH_WRITE_READ, b.p, b.len, fds, nfds, &res);
    free(b.p);
    if (rc < 0)
        return rc;
    // Descriptors for the receive buffer: installed here, numbered here.
    // Under the map lock: another thread's munmap must not pull the view
    // out from under these writes.
    struct bfile *f = tc->f;
    pthread_mutex_lock(&f->map_lock);
    for (uint32_t i = 0; i < res.r.nfixups; i++) {
        uint64_t off = res.fix[i];
        int fd = res.fds[i];
        if (f->alias && off + 4 <= f->map_len) {
            uint32_t v = (uint32_t)fd;
            memcpy(f->alias + off, &v, 4);
        } else {
            close(fd);
        }
    }
    pthread_mutex_unlock(&f->map_lock);
    if (res.r.read_len)
        if (!gcopy((void *)(uintptr_t)(gp(bwr.read_buffer) + bwr.read_consumed), res.rd, res.r.read_len)) {
            result_done(&res);
            return LERR(EFAULT);
        }
    if (res.r.spawn_looper) {
        uint32_t sl = BR_SPAWN_LOOPER;
        gcopy((void *)(uintptr_t)gp(bwr.read_buffer), &sl, 4);
    }
    bwr.write_consumed += res.r.write_consumed;
    if (res.r.arg[0] == 1)          // the write failed: Linux zeroes read_consumed
        bwr.read_consumed = 0;
    else
        bwr.read_consumed += res.r.read_len;
    long ret = res.r.ret;
    result_done(&res);
    if (!gcopy((void *)(uintptr_t)ubwr, &bwr, sizeof bwr))
        return LERR(EFAULT);
    return ret;
}

// ------------------------------------------------------------ ioctl

long lxrt_binder_ioctl(int fd, unsigned long lreq, uint64_t arg)
{
    struct bfile *f = file_get(fd);
    if (!f) {
        binder_diag("ioctl 0x%lx on descriptor %d, which is not a binder file (any more)", lreq, fd);
        return LERR(EBADF);
    }
    long ret;
    if (f->forked || f->closed) { ret = LERR(EINVAL); goto out; }
    uint32_t req = (uint32_t)lreq;
    long err = 0;
    struct tchan *tc = tchan_get(f, &err);
    if (!tc) { ret = err; goto out; }
    if (tc->busy) {
        // A signal handler calling binder while this thread's own call is in
        // flight: its channel is mid-request. Refused rather than corrupted.
        ret = LERR(EAGAIN);
        goto out;
    }
    tc->busy = true;
    if (req == BINDER_WRITE_READ) {
        ret = write_read(tc, arg, fd);
    } else {
        switch (req) {
        case BINDER_VERSION: case BINDER_SET_MAX_THREADS: case BINDER_SET_CONTEXT_MGR:
        case BINDER_SET_CONTEXT_MGR_EXT: case BINDER_THREAD_EXIT:
        case BINDER_GET_NODE_DEBUG_INFO: case BINDER_GET_NODE_INFO_FOR_REF: {
            struct bh_ioctl io = { .cmd = req };
            uint32_t sz = LX_IOC_SIZE(req);
            uint32_t dir = req >> 30;
            if (sz > sizeof io.arg) { ret = LERR(EINVAL); break; }
            if ((dir & 1) && !gcopy(io.arg, (const void *)(uintptr_t)arg, sz)) { ret = LERR(EFAULT); break; }
            struct result res = { 0 };
            ret = roundtrip(tc, BH_IOCTL, &io, sizeof io, NULL, 0, &res);
            if (ret < 0) break;
            ret = res.r.ret;
            if (ret == 0 && (dir & 2) && !gcopy((void *)(uintptr_t)arg, res.r.arg, sz))
                ret = LERR(EFAULT);
            result_done(&res);
            break;
        }
        default:
            // BINDER_FREEZE and the rest: what a kernel without them says.
            ret = LERR(EINVAL);
            break;
        }
    }
    tc->busy = false;
    if (req == BINDER_THREAD_EXIT)
        tchan_drop(tc);             // the hub releases the thread at channel end
out:
    file_put(f);
    return ret;
}

bool lxrt_binder_owns(int fd)
{
    // The channels sit at 64 and above (hide_fd); every close() comes here.
    if (fd < 64 || atomic_load(&g_nopened) == 0) return false;
    bool mine = false;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nchan_fds && !mine; i++) mine = g_chan_fds[i] == fd;
    pthread_mutex_unlock(&g_lock);
    if (mine) binder_diag("close of descriptor %d refused: it is the runtime's channel to the binder hub", fd);
    return mine;
}

void lxrt_binder_note_stray_ioctl(int fd, unsigned long req)
{
    if (atomic_load(&g_nopened) == 0 || ((req >> 8) & 0xff) != 'b') return;
    char path[PATH_MAX] = "";
    int fl = fcntl(fd, F_GETFD);
    if (fl >= 0) fcntl(fd, F_GETPATH, path);
    binder_diag("binder ioctl 0x%lx on descriptor %d, which is not a binder file: %s%s", req, fd,
                fl < 0 ? "closed" : "open", path[0] ? path : "");
}

void lxrt_binder_polled(int fd)
{
    struct bfile *f = file_get(fd);
    if (!f) return;
    if (!f->forked && !f->closed) {
        long err = 0;
        struct tchan *tc = tchan_get(f, &err);
        if (tc && !tc->polled && !tc->busy) {
            tc->busy = true;
            struct result res = { 0 };
            if (roundtrip(tc, BH_POLL, NULL, 0, NULL, 0, &res) == 0) {
                tc->polled = true;
                result_done(&res);
            }
            tc->busy = false;
        }
    }
    file_put(f);
}

// ------------------------------------------------------------ mmap

long lxrt_binder_mmap(uint64_t addr, uint64_t len, int prot, int lflags, int fd, uint64_t off)
{
    struct bfile *f = file_get(fd);
    if (!f) return LERR(EBADF);
    long ret;
    if (f->forked || f->closed) { ret = LERR(EINVAL); goto out; }
    if (prot & PROT_WRITE) { ret = LERR(EPERM); goto out; }   // FORBIDDEN_MMAP_FLAGS
    if (f->map_base) { ret = LERR(EBUSY); goto out; }
    if (!len) { ret = LERR(EINVAL); goto out; }
    if (atomic_load(&g_nmapped) >= (int)(sizeof g_mapped / sizeof g_mapped[0])) {
        ret = LERR(ENOMEM);         // more receive buffers than any process has
        goto out;
    }
    (void)off;
    uint64_t size = len > SZ_4M ? SZ_4M : len;
    uint64_t hlen = LXRT_ALIGN_UP(size, LXRT_HOST_PAGE);
    char path[256];
    snprintf(path, sizeof path, "%s/buf-%d-XXXXXX", binder_dir(), (int)getpid());   // open checked it
    int bfd = mkstemp(path);
    if (bfd < 0) { ret = LERR(errno); goto out; }
    unlink(path);
    fcntl(bfd, F_SETFD, FD_CLOEXEC);
    if (ftruncate(bfd, (off_t)hlen) != 0) { ret = LERR(errno); close(bfd); goto out; }
    int mflags = MAP_SHARED;
    if ((lflags & 0x10) && addr) mflags |= MAP_FIXED;     // Linux MAP_FIXED
    void *view = mmap((mflags & MAP_FIXED) ? (void *)(uintptr_t)addr : NULL, hlen,
                      PROT_READ, mflags, bfd, 0);
    if (view == MAP_FAILED) { ret = LERR(errno); close(bfd); goto out; }
    void *alias = mmap(NULL, hlen, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
    if (alias == MAP_FAILED) { ret = LERR(errno); munmap(view, hlen); close(bfd); goto out; }
    // VM_DONTCOPY: a fork child has no binder buffer.
    minherit(view, hlen, VM_INHERIT_NONE);
    minherit(alias, hlen, VM_INHERIT_NONE);
    long err = 0;
    struct tchan *tc = tchan_get(f, &err);
    if (!tc || tc->busy) {
        ret = tc ? LERR(EAGAIN) : err;
        munmap(view, hlen); munmap(alias, hlen); close(bfd);
        goto out;
    }
    tc->busy = true;
    // The hub computes the addresses it hands this process from the buffer's
    // base: give it the guest's view of it (a 32-bit guest's is below 4 GiB).
    uint64_t ubase = (uint64_t)(uintptr_t)view, gb = lxrt_gbase();
    if (gb && ubase >= gb && ubase - gb < (1ull << 32))
        ubase -= gb;
    struct bh_mmap mm = { .base = ubase, .size = size };
    struct result res = { 0 };
    ret = roundtrip(tc, BH_MMAP, &mm, sizeof mm, &bfd, 1, &res);
    tc->busy = false;
    close(bfd);
    if (ret == 0) {
        ret = res.r.ret;
        result_done(&res);
    }
    if (ret < 0) {
        munmap(view, hlen);
        munmap(alias, hlen);
        goto out;
    }
    pthread_mutex_lock(&g_lock);
    f->map_base = (uint64_t)(uintptr_t)view;
    f->map_len = size;
    f->map_host_len = hlen;
    f->alias = alias;
    f->refs++;                      // the mapping keeps the file (vm_file)
    int nm = atomic_load(&g_nmapped);
    g_mapped[nm] = f;               // room checked above; one mmap per file
    atomic_store(&g_nmapped, nm + 1);
    pthread_mutex_unlock(&g_lock);
    ret = (long)(uintptr_t)view;
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] binder: receive buffer %llu bytes at %p\n",
                (unsigned long long)size, view);
out:
    file_put(f);
    return ret;
}

static void file_release_proc(struct bfile *f);

// The guest unmapping its receive buffer (ProcessState's destructor).
bool lxrt_binder_munmap(uint64_t addr, uint64_t len, long *ret)
{
    if (atomic_load(&g_nmapped) == 0) return false;
    struct bfile *hit = NULL;
    pthread_mutex_lock(&g_lock);
    int nm = atomic_load(&g_nmapped);
    for (int i = 0; i < nm; i++) {
        struct bfile *f = g_mapped[i];
        if (addr < f->map_base + f->map_host_len && f->map_base < addr + len) {
            hit = f;
            g_mapped[i] = g_mapped[nm - 1];
            atomic_store(&g_nmapped, nm - 1);
            break;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (!hit) return false;
    // binder_vma_close: from here on the hub allocates nothing more in this
    // proc's buffer (a transaction to it gets BR_DEAD_REPLY, as on Linux),
    // so nothing can be handed to the guest at an address it gave up.
    if (!hit->forked && !hit->closed) {
        long err = 0;
        struct tchan *tc = tchan_get(hit, &err);
        if (tc && !tc->busy) {
            tc->busy = true;
            struct bh_ioctl io = { .cmd = BH_IOCTL_UNMAPPED };
            struct result res = { 0 };
            if (roundtrip(tc, BH_IOCTL, &io, sizeof io, NULL, 0, &res) == 0)
                result_done(&res);
            tc->busy = false;
        }
    }
    pthread_mutex_lock(&hit->map_lock);
    munmap((void *)(uintptr_t)hit->map_base, hit->map_host_len);
    munmap(hit->alias, hit->map_host_len);
    pthread_mutex_lock(&g_lock);
    hit->map_base = 0;
    hit->alias = NULL;
    bool release = hit->naliases == 0;
    pthread_mutex_unlock(&g_lock);
    pthread_mutex_unlock(&hit->map_lock);
    if (release)
        file_release_proc(hit);
    file_put(hit);
    *ret = 0;
    return true;
}

// ------------------------------------------------------------ descriptors

void lxrt_binder_close(int fd)
{
    // Every close() of the guest comes here: stay off the lock unless this
    // process ever had a binder descriptor.
    if (fd < 0 || fd >= B_FDS || atomic_load(&g_nopened) == 0) return;
    pthread_mutex_lock(&g_lock);
    struct bfile *f = g_files[fd];
    g_files[fd] = NULL;
    bool last = f && --f->naliases == 0;
    if (last) f->closed = true;
    pthread_mutex_unlock(&g_lock);
    if (!f) return;
    binder_diag("binder descriptor %d closed%s", fd, last ? " (its last alias)" : "");
    if (last && !f->map_base)
        file_release_proc(f);
    file_put(f);
}

// binder_release: no descriptor and no mapping names the proc any more. Our
// own copy of the process channel goes, and the hub releases the proc (its
// threads' channels end with it; ours are dropped here or when their
// threads next look).
static void file_release_proc(struct bfile *f)
{
    if (f->forked) return;
    for (int i = 0; i < t_nchans; )
        if (t_chans[i].f == f && !t_chans[i].busy) tchan_drop(&t_chans[i]);
        else i++;
    pthread_mutex_lock(&f->lock);
    if (f->chan >= 0) { chan_unregister(f->chan); shutdown(f->chan, SHUT_RDWR); close(f->chan); f->chan = -1; }
    pthread_mutex_unlock(&f->lock);
}

void lxrt_binder_dup(int oldfd, int newfd)
{
    if (oldfd < 0 || oldfd >= B_FDS || newfd < 0 || newfd >= B_FDS || oldfd == newfd ||
        atomic_load(&g_nopened) == 0)
        return;
    pthread_mutex_lock(&g_lock);
    struct bfile *f = g_files[oldfd];
    if (f && !g_files[newfd]) {
        g_files[newfd] = f;
        f->naliases++;
        f->refs++;
    }
    pthread_mutex_unlock(&g_lock);
}

void lxrt_binder_thread_exit(void)
{
    while (t_nchans > 0)
        tchan_drop(&t_chans[t_nchans - 1]);
}

// fork(): the child is a new process. Its binder descriptors still exist (the
// guest may close them) but name its parent's procs, and its receive buffers
// are gone (VM_INHERIT_NONE); every call on them is refused, as Linux refuses
// an mmap or use of a binder_proc from another process. The parent's thread
// channels are closed here: their threads do not exist in the child.
static void binder_after_fork_child(void)
{
    if (atomic_load(&g_nopened) == 0)
        return;
    atomic_store(&g_nmapped, 0);
    for (int i = 0; i < g_nchan_fds; i++)
        close(g_chan_fds[i]);
    g_nchan_fds = 0;
    t_nchans = 0;
    memset(t_chans, 0, sizeof t_chans);
    for (int i = 0; i < B_FDS; i++) {
        struct bfile *f = g_files[i];
        if (!f || f->forked) continue;
        f->forked = true;
        pthread_mutex_init(&f->lock, NULL);
        pthread_mutex_init(&f->map_lock, NULL);
        f->chan = -1;                   // closed above with the other channels
        f->map_base = 0;
        f->alias = NULL;
    }
}
__attribute__((constructor(201))) static void binder_fork_register(void)
{
    pthread_atfork(NULL, NULL, binder_after_fork_child);
}
