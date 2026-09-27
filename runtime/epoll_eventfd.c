// epoll and eventfd on kqueue and pipes.
//
// benchmarks/stage6-steam-gap.txt put this at the head of the P0 list: 46 live
// eventpoll and 49 live eventfd descriptors in Steam's process tree, and
// epoll_pwait at 33334 calls per 10 s, the 4th most frequent syscall on the
// machine. It is the client's own event loop.
//
// kqueue is not epoll, and the gaps are not cosmetic:
//
//   * epoll carries a bitmask per descriptor; kqueue carries a separate knote
//     per (fd, filter). One readable-and-writable fd is two kevents and must be
//     coalesced back into one epoll_event, or the guest sees the same cookie
//     twice in one wait and double-services it.
//   * epoll is LEVEL-triggered unless EPOLLET is given. kqueue is also level
//     triggered unless EV_CLEAR is given -- measured, not assumed: registering
//     EVFILT_READ on a pipe with one unread byte returns the event on every
//     subsequent kevent() call. So the default maps straight across and EPOLLET
//     becomes EV_CLEAR. Getting this backwards is the classic port bug: an
//     event loop that reads a partial buffer would hang forever.
//   * kqueue has no event mask to return. EPOLLERR and EPOLLHUP, which Linux
//     reports whether or not they were requested, have to be reconstructed from
//     EV_EOF and EV_ERROR plus the *type* of the descriptor, because EV_EOF on
//     a pipe means EPOLLHUP while EV_EOF on a socket means EPOLLRDHUP.
//   * a kqueue knote is dropped when its descriptor closes and nothing says so,
//     whereas Linux's close() removes the fd from every epoll set. The
//     descriptor NUMBER is therefore not a safe key on its own; see
//     ep_entry_stale() and lxrt_epoll_fd_closed().
//   * eventfd has no substrate at all. The counter lives here and a pipe
//     carries readiness, so the descriptor stays pollable by kqueue, by the
//     ppoll() the runtime already has, and by select().
//
// Everything the guest receives is a real Darwin descriptor. A synthetic fd
// number would die the first time Steam handed its epoll fd to ppoll() or to
// close(), which it does constantly.

#include "lxrt.h"
#include "epoll_eventfd.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))

// ---------------------------------------------------------------- constants
//
// Linux epoll event bits, from include/uapi/linux/eventpoll.h. The first six
// are the poll(2) bits and they happen to agree with Darwin's:
//
//     bit            Linux    Darwin
//     POLLIN         0x0001   0x0001   agree
//     POLLPRI        0x0002   0x0002   agree
//     POLLOUT        0x0004   0x0004   agree
//     POLLERR        0x0008   0x0008   agree
//     POLLHUP        0x0010   0x0010   agree
//     POLLNVAL       0x0020   0x0020   agree
//     POLLRDNORM     0x0040   0x0040   agree
//     POLLRDBAND     0x0080   0x0080   agree
//     POLLWRNORM     0x0100   0x0004   DIVERGE -- Darwin aliases it to POLLOUT
//     POLLWRBAND     0x0200   0x0100   DIVERGE -- Darwin's value is Linux's
//                                      POLLWRNORM, so a pass-through turns a
//                                      band-write request into a normal one
//     POLLRDHUP      0x2000   absent   Darwin has no such bit at all
//
// That is the shape of the bug this project keeps meeting: the low range
// agrees, so the common case works, and the divergence starts exactly where
// the less-travelled code is. Nothing below is passed through; every bit is
// translated by name.
#define L_EPOLLIN        0x00000001u
#define L_EPOLLPRI       0x00000002u
#define L_EPOLLOUT       0x00000004u
#define L_EPOLLERR       0x00000008u
#define L_EPOLLHUP       0x00000010u
#define L_EPOLLNVAL      0x00000020u
#define L_EPOLLRDNORM    0x00000040u
#define L_EPOLLRDBAND    0x00000080u
#define L_EPOLLWRNORM    0x00000100u
#define L_EPOLLWRBAND    0x00000200u
#define L_EPOLLMSG       0x00000400u
#define L_EPOLLRDHUP     0x00002000u
#define L_EPOLLEXCLUSIVE 0x10000000u
#define L_EPOLLWAKEUP    0x20000000u
#define L_EPOLLONESHOT   0x40000000u
#define L_EPOLLET        0x80000000u

#define L_EPOLL_READ_BITS  (L_EPOLLIN | L_EPOLLRDNORM | L_EPOLLRDBAND)
#define L_EPOLL_WRITE_BITS (L_EPOLLOUT | L_EPOLLWRNORM | L_EPOLLWRBAND)

// fs/eventpoll.c, EPOLLEXCLUSIVE_OK_BITS: the only bits Linux lets an
// EPOLL_CTL_ADD carry alongside EPOLLEXCLUSIVE. Anything else is EINVAL --
// measured on the 6.17.1 aarch64 guest: EPOLLIN|EPOLLEXCLUSIVE alone returned
// 0, while adding EPOLLONESHOT or EPOLLRDBAND returned -EINVAL.
#define L_EPOLL_EXCLUSIVE_OK_BITS \
    (L_EPOLLIN | L_EPOLLOUT | L_EPOLLERR | L_EPOLLHUP | L_EPOLLWAKEUP | \
     L_EPOLLET | L_EPOLLEXCLUSIVE)

// op values for epoll_ctl. Identical on every Linux architecture.
#define L_EPOLL_CTL_ADD 1
#define L_EPOLL_CTL_DEL 2
#define L_EPOLL_CTL_MOD 3

// The only flag epoll_create1 accepts. Linux spells it O_CLOEXEC, which is
// 0x80000 (0o2000000) on aarch64; Darwin's O_CLOEXEC is 0x1000000. The two are
// never compared, but the guest value must not be handed to a Darwin open().
#define L_EPOLL_CLOEXEC 0x00080000

// eventfd2 flags. EFD_CLOEXEC and EFD_NONBLOCK are aliases of Linux's
// O_CLOEXEC and O_NONBLOCK, both of which differ from Darwin's:
//     O_CLOEXEC   Linux 0x80000   Darwin 0x1000000
//     O_NONBLOCK  Linux 0x00800   Darwin 0x0000004
// EFD_SEMAPHORE has no open() counterpart and is 1 on every architecture.
#define L_EFD_SEMAPHORE 0x00000001
#define L_EFD_CLOEXEC   0x00080000
#define L_EFD_NONBLOCK  0x00000800

// Linux SIG_SETMASK. Darwin's is 3 -- the whole how/1/2/3 triple is shifted by
// one. lxrt_rt_sigprocmask() takes the Linux numbering because it implements
// the Linux syscall, so 2 is correct here and would be SIG_UNBLOCK on Darwin.
#define L_SIG_SETMASK 2

// The guest's sigset_t as the kernel sees it: 64 bits on aarch64. Linux
// rejects any other sigsetsize with EINVAL, so the guest's glibc is entitled to
// rely on that check existing.
#define L_KERNEL_SIGSET_BYTES 8

// struct epoll_event, aarch64.
//
// include/uapi/linux/eventpoll.h applies __attribute__((packed)) under
// #ifdef __x86_64__ ONLY. Everywhere else, aarch64 included, the struct is
// natural-aligned, so the __u64 sits at offset 8 and the struct is 16 bytes:
//
//     offset 0  uint32_t events
//     offset 4  4 bytes of padding, uninitialised by the kernel
//     offset 8  uint64_t data      (the epoll_data_t union: ptr/fd/u32/u64)
//
// On x86-64 the same struct is 12 bytes with data at offset 4. Borrowing that
// layout here slides every returned cookie by four bytes and the guest
// dereferences half a pointer -- silent, total, and looks like heap corruption.
// The asserts below are the check, not the comment.
struct linux_epoll_event {
    uint32_t events;
    uint32_t pad_aarch64;
    uint64_t data;
};
_Static_assert(sizeof(struct linux_epoll_event) == 16,
               "aarch64 struct epoll_event is 16 bytes, not packed");
_Static_assert(offsetof(struct linux_epoll_event, data) == 8,
               "aarch64 epoll_data_t sits at offset 8");

struct linux_timespec { int64_t tv_sec; int64_t tv_nsec; };

// Sized from the measured census: 46 eventpoll and 49 eventfd descriptors
// across Steam's eleven processes. One runtime is one process, so these are
// generous by roughly 5x; exhaustion returns EMFILE, which is what Linux
// returns when /proc/sys/fs/epoll/max_user_instances is hit.
#define MAX_EPOLL   256
#define MAX_EVENTFD 256

// kevent() is asked for at most this many events per call regardless of the
// guest's maxevents. Returning fewer events than asked for is always legal --
// level-triggered readiness is still there on the next call -- and it bounds
// the transient allocation at 4096 * (32 + 24) bytes.
#define KEV_BATCH_MAX 4096

// The most kqueue filters one interest can ever occupy: read, write, except.
// The changelists below are sized 2 * this because EPOLL_CTL_MOD submits a
// delete list and an add list as one atomic changelist.
#define EP_MAX_CH 3

// ---------------------------------------------------------------- guest memory
//
// Rule: a bad guest pointer produces EFAULT, never a SIGSEGV in the host. There
// is no guest/host boundary to catch a fault at -- the guest runs in this
// process -- so the range is checked against the VM map before it is touched.
// One mach_vm_region() trap per epoll_pwait against 33334 calls per 10 s is
// ~3.3k traps/s, well under the cost of the kevent() the call is about to make.
// Measured on this machine: mach_vm_region costs 682 ns per call.
static bool range_ok(const void *p, size_t len, bool need_write)
{
    if (!p)
        return false;
    if (len == 0)
        return true;

    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)p;
    mach_vm_address_t end = a + len;
    if (end < a)                       // wrapped: not a range at all
        return false;

    while (a < end) {
        mach_vm_address_t ra = a;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;

        // mach_vm_region reports the first region at or AFTER `ra`, so a
        // returned start past the probe address means the probe address is in
        // a hole.
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
            return false;
        if (obj != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), obj);
        if (ra > a || rs == 0)
            return false;
        if (!(info.protection & VM_PROT_READ))
            return false;
        if (need_write && !(info.protection & VM_PROT_WRITE))
            return false;
        a = ra + rs;
    }
    return true;
}

// ---------------------------------------------------------------- epoll state

struct ep_interest {
    int      fd;         // guest fd; the kqueue ident for the read/except knote
    int      wfd;        // ident for the write knote. Equal to fd except for
                         // our own eventfds -- see eventfd_write_end().
    uint32_t events;     // the Linux mask exactly as registered
    uint64_t data;       // epoll_data_t, returned to the guest verbatim
    bool     is_socket;  // fstat'd once at ADD. Decides whether EV_EOF means
                         // EPOLLHUP (pipe: writer gone) or EPOLLRDHUP (socket:
                         // peer sent FIN, the other half may still be alive).
    bool     oneshot_fired;  // EPOLLONESHOT and this interest has already
                             // reported. Linux disarms the whole INTEREST after
                             // one reported event, not the one filter that
                             // fired; see ep_changes() for the measurement.
    // Identity of the open file behind `fd` at ADD time. The descriptor NUMBER
    // alone is not a key: kqueue drops the knote when the guest closes the fd
    // and tells nobody, so a recycled number would keep matching a dead entry.
    // Darwin hands a pipe a random 64-bit st_ino and a socket a distinct one --
    // measured: two pipes created back to back got 802054470155456830 and
    // 8646670859292934534, and a dup() of the first kept its ino, which is
    // exactly the "same open file" answer wanted here.
    dev_t    st_dev;
    ino_t    st_ino;
};

// What each kqueue filter can actually produce once kev_to_epoll() has
// translated it. The arming decision in ep_changes() MUST agree with these
// sets: a LEVEL-triggered knote whose bits the translation then masks away to
// nothing fires on every kevent(), and pwait_common()'s honest "re-wait rather
// than fake a timeout" loop turns into a busy spin. Measured before the fix,
// deterministic over three runs: EPOLLRDBAND alone on a readable pipe burned
// 409 ms of CPU in 400 ms of wall clock inside one epoll_pwait(-1) and never
// returned; EPOLLWRBAND alone on a socketpair burned 300 ms of CPU inside a
// single epoll_pwait(300).
//
// The sets are measured, not assumed. On the 6.17.1 aarch64 guest:
//     pipe readable,        EPOLLRDBAND only -> epoll_wait returns 0 events
//     unix socket readable, EPOLLRDBAND only -> 0 events
//     pipe writable,        EPOLLWRBAND only -> 0 events
//     unix socket writable, EPOLLWRBAND only -> 1 event, 0x200
// So RDBAND is never set for ordinary readability anywhere, and WRBAND is set
// by unix_poll/tcp_poll but not by pipe_poll. Emitting RDBAND to stop the spin
// would invent an event Linux never delivers; the arming rule is the fix and
// the emitted WRBAND below is simply what Linux does.
#define L_EPOLL_READ_EMIT (L_EPOLLIN | L_EPOLLRDNORM)

static uint32_t write_emit_bits(const struct ep_interest *in)
{
    return L_EPOLLOUT | L_EPOLLWRNORM | (in->is_socket ? L_EPOLLWRBAND : 0u);
}

struct ep_inst {
    // `used` rather than a sentinel in `kq`: the table is static, so a free
    // slot is zero-filled, and zero is fd 0. Testing kq for a sentinel value
    // made every slot look occupied by stdin and epoll_create1 returned EMFILE
    // on the very first call.
    bool used;
    int  kq;             // guest-visible descriptor
    int  refs;           // threads inside kevent() on this instance
    bool dead;           // closed, waiting for refs to drain
    struct ep_interest *v;
    int  n, cap;
};

static struct ep_inst g_ep[MAX_EPOLL];
static atomic_int g_ep_live;
static pthread_mutex_t g_ep_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(epoll_eventfd_g_ep_lock, g_ep_lock)

// ---------------------------------------------------------------- eventfd state

// Guest descriptors that alias one eventfd. dup(), dup2(), dup3() and
// fcntl(F_DUPFD) all produce one, and on Linux every alias is the same eventfd:
// measured on the guest, dup(eventfd(5)) then read(dup, buf, 8) returns 8 bytes
// and the value 5. Before aliases were tracked, the same sequence here returned
// 1 byte holding 101 -- the ASCII 'e' readiness token -- and ate the token, so
// the descriptor became permanently unreadable to poll/select/kqueue while the
// counter still said 5.
//
// Sixteen is a documented cap, not a guess at infinity: it keeps the table
// allocation-free so lxrt_eventfd_dup() cannot fail under memory pressure while
// holding the global eventfd lock. A seventeenth alias is not tracked and falls
// back to the raw pipe behaviour described above; nothing in the measured Steam
// census comes close.
#define EV_MAX_ALIAS 16

struct ev_obj {
    bool     used;       // see struct ep_inst: a zero-filled free slot would
                         // otherwise claim to be descriptor 0
    int      pr;         // PRIVATE read end of the token pipe. Never handed to
                         // the guest, so no guest close() can take the drain
                         // path away -- every guest-visible fd below is a dup()
                         // of this one and therefore the same pipe.
    int      wfd;        // our write end. Never handed to the guest, and the
                         // ident used for EPOLLOUT interest -- measured:
                         // EVFILT_WRITE registers successfully on a pipe's READ
                         // end and then never fires, so watching the read end
                         // for writability would silently wedge any guest
                         // waiting on EPOLLOUT.
    int      fds[EV_MAX_ALIAS];   // guest-visible descriptors for this eventfd
    int      nfds;
    uint64_t count;
    bool     semaphore;
    bool     nonblock;
    bool     armed;      // exactly one token byte is sitting in the pipe.
                         // Invariant: armed == (count > 0). That is what makes
                         // the descriptor readable to kqueue, poll and select
                         // without them knowing anything about the counter.
    int      refs;
    bool     dead;
};

static struct ev_obj g_ev[MAX_EVENTFD];
static pthread_mutex_t g_ev_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(epoll_eventfd_g_ev_lock, g_ev_lock)

// lxrt_eventfd_is() sits in front of every read(63) and write(64) in the
// process, so it cannot take a lock. A bitmap over the low fd space answers
// almost every call with one relaxed load; descriptors above it fall back to a
// locked scan, which only happens if the guest has raised RLIMIT_NOFILE past
// 64k and then landed an eventfd up there. Every alias sets its own bit.
#define EVMAP_FDS 65536
static _Atomic uint64_t g_evmap[EVMAP_FDS / 64];
static atomic_int g_ev_live;

static void evmap_set(int fd, bool on)
{
    if (fd < 0 || fd >= EVMAP_FDS)
        return;
    uint64_t bit = (uint64_t)1 << (fd & 63);
    if (on)
        atomic_fetch_or_explicit(&g_evmap[fd >> 6], bit, memory_order_relaxed);
    else
        atomic_fetch_and_explicit(&g_evmap[fd >> 6], ~bit, memory_order_relaxed);
}

static bool evmap_test(int fd)
{
    if (fd < 0 || fd >= EVMAP_FDS)
        return false;
    uint64_t w = atomic_load_explicit(&g_evmap[fd >> 6], memory_order_relaxed);
    return (w >> (fd & 63)) & 1;
}

// Caller holds g_ev_lock.
static bool ev_has_fd(const struct ev_obj *o, int fd)
{
    for (int i = 0; i < o->nfds; i++)
        if (o->fds[i] == fd)
            return true;
    return false;
}

// Caller holds g_ev_lock. Keyed on any alias, not just the first descriptor
// handed out, so a dup'd eventfd still resolves to its counter.
static struct ev_obj *ev_find(int fd)
{
    for (int i = 0; i < MAX_EVENTFD; i++)
        if (g_ev[i].used && !g_ev[i].dead && ev_has_fd(&g_ev[i], fd))
            return &g_ev[i];
    return NULL;
}

// Caller holds g_ev_lock. A slot stays occupied while a thread is still
// blocked inside it, so its fd numbers cannot be handed to a new eventfd
// underneath that thread. The private pipe ends are closed here rather than in
// lxrt_eventfd_close() for the same reason: a thread parked in poll() on `pr`
// must still have a valid descriptor to be parked on.
static void ev_release(struct ev_obj *o)
{
    if (o->dead && o->refs == 0) {
        if (o->pr >= 0)
            close(o->pr);
        if (o->wfd >= 0)
            close(o->wfd);
        o->pr = o->wfd = -1;
        o->nfds = 0;
        o->dead = false;
        o->used = false;
    }
}

bool lxrt_eventfd_is(int fd)
{
    if (fd < 0)
        return false;
    if (evmap_test(fd))
        return true;
    if (fd < EVMAP_FDS || atomic_load_explicit(&g_ev_live, memory_order_relaxed) == 0)
        return false;
    pthread_mutex_lock(&g_ev_lock);
    bool found = ev_find(fd) != NULL;
    pthread_mutex_unlock(&g_ev_lock);
    return found;
}

// The fd that carries writability for an eventfd, or -1 if `fd` is not one.
static int eventfd_write_end(int fd)
{
    if (!lxrt_eventfd_is(fd))
        return -1;
    pthread_mutex_lock(&g_ev_lock);
    struct ev_obj *o = ev_find(fd);
    int w = o ? o->wfd : -1;
    pthread_mutex_unlock(&g_ev_lock);
    return w;
}

// ---------------------------------------------------------------- eventfd2

long lxrt_eventfd2(unsigned initval, int lflags)
{
    // Linux validates the flag word and returns EINVAL for anything it does not
    // know, so a guest probing for a newer flag gets a truthful answer.
    int known = L_EFD_SEMAPHORE | L_EFD_CLOEXEC | L_EFD_NONBLOCK;
    if (lflags & ~known)
        return LERR(EINVAL);

    if (atomic_load_explicit(&g_ev_live, memory_order_relaxed) >= MAX_EVENTFD)
        return LERR(EMFILE);

    int p[2];
    if (pipe(p) != 0)
        return LERR(errno);

    // The guest gets a dup of the read end, never p[0] itself. That costs one
    // descriptor per eventfd and buys the alias table above a drain path the
    // guest cannot close: if the guest dup()s its eventfd and then closes the
    // original, the counter and the token pipe are still reachable from here.
    int g = dup(p[0]);
    if (g < 0) {
        int e = errno;
        close(p[0]);
        close(p[1]);
        return LERR(e);
    }

    // Darwin has no pipe2(), so the flags are applied afterwards. The window in
    // between is not a fork hazard here: lxrt_fork() is the only thing in the
    // runtime that forks, and it is a guest syscall, not a background thread.
    if (lflags & L_EFD_CLOEXEC) {
        // FD_CLOEXEC is 1 on both systems; it is the O_CLOEXEC *open flag* that
        // differs (0x80000 vs 0x1000000), and that one is never used here.
        fcntl(g, F_SETFD, FD_CLOEXEC);
    }
    if (lflags & L_EFD_NONBLOCK) {
        // Kept for fidelity if the guest ever does F_GETFL on the descriptor.
        // The counter path does not consult it -- o->nonblock does. O_NONBLOCK
        // lives on the open file description, which the dup shares with p[0],
        // so this also makes our own drain non-blocking. That is harmless: the
        // drain is poll-gated regardless.
        int fl = fcntl(g, F_GETFL, 0);
        if (fl >= 0)
            fcntl(g, F_SETFL, fl | O_NONBLOCK);
    }
    // Our write end is always non-blocking, unconditionally and regardless of
    // what the guest asked for. It is never visible to the guest, so there is no
    // fidelity to lose, and it means the token write in ev_sync_token() can
    // never block with the table lock held -- which would wedge every eventfd
    // in the process, not just this one.
    {
        int fl = fcntl(p[1], F_GETFL, 0);
        if (fl >= 0)
            fcntl(p[1], F_SETFL, fl | O_NONBLOCK);
    }

    pthread_mutex_lock(&g_ev_lock);
    struct ev_obj *o = NULL;
    for (int i = 0; i < MAX_EVENTFD; i++) {
        if (!g_ev[i].used) {
            o = &g_ev[i];
            break;
        }
    }
    if (!o) {
        pthread_mutex_unlock(&g_ev_lock);
        close(g);
        close(p[0]);
        close(p[1]);
        return LERR(EMFILE);
    }
    o->used = true;
    o->pr = p[0];
    o->wfd = p[1];
    o->fds[0] = g;
    o->nfds = 1;
    o->count = initval;
    o->semaphore = (lflags & L_EFD_SEMAPHORE) != 0;
    o->nonblock = (lflags & L_EFD_NONBLOCK) != 0;
    o->armed = false;
    o->dead = false;
    o->refs = 0;
    if (o->count > 0) {
        // Restore the invariant immediately: a non-zero initval means the
        // descriptor is already readable before the guest touches it.
        if (write(o->wfd, "e", 1) == 1)
            o->armed = true;
    }
    evmap_set(g, true);
    atomic_fetch_add_explicit(&g_ev_live, 1, memory_order_relaxed);
    pthread_mutex_unlock(&g_ev_lock);
    return g;
}

void lxrt_eventfd_dup(int oldfd, int newfd)
{
    if (oldfd < 0 || newfd < 0 || oldfd == newfd)
        return;
    if (atomic_load_explicit(&g_ev_live, memory_order_relaxed) == 0)
        return;
    // Cheap reject for the overwhelming majority of dup()s in the process: a
    // descriptor this module owns always has its evmap bit set.
    if (oldfd < EVMAP_FDS && !evmap_test(oldfd))
        return;

    pthread_mutex_lock(&g_ev_lock);
    struct ev_obj *o = ev_find(oldfd);
    if (o && !ev_has_fd(o, newfd) && o->nfds < EV_MAX_ALIAS) {
        o->fds[o->nfds++] = newfd;
        evmap_set(newfd, true);
    }
    // Past EV_MAX_ALIAS the alias is not recorded and reads on it reach the raw
    // pipe. See the comment on EV_MAX_ALIAS: this is a stated cap, not a
    // silently swallowed failure, and 16 aliases of one eventfd is already far
    // outside anything the Steam census showed.
    pthread_mutex_unlock(&g_ev_lock);
}

// Caller holds g_ev_lock. Restores the invariant armed == (count > 0), which is
// the whole reason the descriptor is pollable: kqueue, poll() and select() see
// a pipe with a byte in it and know nothing about the counter.
static void ev_sync_token(struct ev_obj *o)
{
    if (o->count > 0 && !o->armed) {
        if (write(o->wfd, "e", 1) == 1)
            o->armed = true;
    } else if (o->count == 0 && o->armed) {
        // The drain reads from our PRIVATE read end, so it cannot be affected
        // by anything the guest does to its own aliases. It is still gated on a
        // zero-timeout poll first: under the invariant the byte is always
        // there, and the guard exists because the alternative -- if something
        // ever consumed the token behind our back -- is a blocking read holding
        // the global eventfd lock, and that is a deadlock rather than a wrong
        // answer.
        struct pollfd pfd = { .fd = o->pr, .events = POLLIN, .revents = 0 };
        if (poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN)) {
            char c;
            if (read(o->pr, &c, 1) == 1) {
                o->armed = false;
                return;
            }
        }
        o->armed = false;
    }
}

long lxrt_eventfd_read(int fd, void *buf, size_t n)
{
    // Linux: a read of fewer than 8 bytes is EINVAL, checked before anything
    // else, so a short buffer never consumes the counter.
    if (n < sizeof(uint64_t))
        return LERR(EINVAL);
    if (!range_ok(buf, sizeof(uint64_t), true))
        return LERR(EFAULT);

    for (;;) {
        pthread_mutex_lock(&g_ev_lock);
        struct ev_obj *o = ev_find(fd);
        if (!o) {
            pthread_mutex_unlock(&g_ev_lock);
            return LERR(EBADF);
        }
        if (o->count > 0) {
            uint64_t v;
            if (o->semaphore) {
                // EFD_SEMAPHORE: hand back 1 and decrement by 1.
                v = 1;
                o->count -= 1;
            } else {
                v = o->count;
                o->count = 0;
            }
            ev_sync_token(o);
            pthread_mutex_unlock(&g_ev_lock);
            // memcpy, not a store through a cast: the guest is only promised an
            // 8-byte buffer, never an 8-byte-aligned one, and aarch64 traps
            // unaligned accesses to device memory.
            memcpy(buf, &v, sizeof v);
            return (long)sizeof(uint64_t);
        }
        if (o->nonblock) {
            pthread_mutex_unlock(&g_ev_lock);
            return LERR(EAGAIN);
        }
        int pr = o->pr;
        o->refs++;
        pthread_mutex_unlock(&g_ev_lock);

        // Block on the pipe rather than on a condvar: poll() returns EINTR when
        // a guest signal lands, and a blocking eventfd read on Linux is
        // interruptible. pthread_cond_wait() is not, and swallowing the
        // interruption would hang a guest that expects its handler to break the
        // loop. A lost race here just costs one extra trip round the loop.
        // The private read end is used, not the guest's, so a close() of the
        // alias this call names cannot pull the descriptor out from under
        // poll(); lxrt_eventfd_close() writes a wake token instead.
        struct pollfd pfd = { .fd = pr, .events = POLLIN, .revents = 0 };
        int pr_rc = poll(&pfd, 1, -1);
        int perr = errno;

        pthread_mutex_lock(&g_ev_lock);
        o->refs--;
        bool gone = o->dead || !ev_has_fd(o, fd);
        ev_release(o);
        pthread_mutex_unlock(&g_ev_lock);

        if (gone)
            return LERR(EBADF);
        if (pr_rc < 0)
            return LERR(perr);
        if (pfd.revents & POLLNVAL)
            return LERR(EBADF);
    }
}

long lxrt_eventfd_write(int fd, const void *buf, size_t n)
{
    if (n < sizeof(uint64_t))
        return LERR(EINVAL);
    if (!range_ok(buf, sizeof(uint64_t), false))
        return LERR(EFAULT);

    uint64_t add;
    memcpy(&add, buf, sizeof add);
    // Linux rejects the all-ones value outright: it is the one value that could
    // never be added without saturating, so it is reserved as an error.
    if (add == UINT64_MAX)
        return LERR(EINVAL);

    for (;;) {
        pthread_mutex_lock(&g_ev_lock);
        struct ev_obj *o = ev_find(fd);
        if (!o) {
            pthread_mutex_unlock(&g_ev_lock);
            return LERR(EBADF);
        }
        // The counter saturates at UINT64_MAX-1, not UINT64_MAX.
        if (add <= (UINT64_MAX - 1) - o->count) {
            o->count += add;
            ev_sync_token(o);
            pthread_mutex_unlock(&g_ev_lock);
            return (long)sizeof(uint64_t);
        }
        if (o->nonblock) {
            pthread_mutex_unlock(&g_ev_lock);
            return LERR(EAGAIN);
        }
        int pr = o->pr;
        o->refs++;
        pthread_mutex_unlock(&g_ev_lock);

        // Overflow needs 2^64 outstanding wakeups, so this path is effectively
        // dead; it is written correctly rather than stubbed because "returns
        // EAGAIN on a blocking fd" would be a lie the guest could not diagnose.
        // Waiting for the descriptor to become readable is a proxy for waiting
        // for a reader: the reader drains the token on its way through.
        struct pollfd pfd = { .fd = pr, .events = POLLIN, .revents = 0 };
        int pr_rc = poll(&pfd, 1, 10 /* ms; no reader-side event to wait on */);
        int perr = errno;

        pthread_mutex_lock(&g_ev_lock);
        o->refs--;
        bool gone = o->dead || !ev_has_fd(o, fd);
        ev_release(o);
        pthread_mutex_unlock(&g_ev_lock);

        if (gone)
            return LERR(EBADF);
        if (pr_rc < 0 && perr == EINTR)
            return LERR(EINTR);
    }
}

void lxrt_eventfd_close(int fd)
{
    if (fd < 0 || atomic_load_explicit(&g_ev_live, memory_order_relaxed) == 0)
        return;
    if (fd < EVMAP_FDS && !evmap_test(fd))
        return;

    pthread_mutex_lock(&g_ev_lock);
    struct ev_obj *o = ev_find(fd);
    if (o) {
        evmap_set(fd, false);
        // Drop just this alias. On Linux the eventfd outlives any one of its
        // descriptors and dies with the last; the object here does the same.
        for (int i = 0; i < o->nfds; i++) {
            if (o->fds[i] == fd) {
                o->fds[i] = o->fds[o->nfds - 1];
                o->nfds--;
                break;
            }
        }
        if (o->nfds == 0) {
            o->count = 0;
            o->armed = false;
            o->dead = true;
            atomic_fetch_sub_explicit(&g_ev_live, 1, memory_order_relaxed);
            if (o->refs > 0 && o->wfd >= 0) {
                // Wake anybody parked in poll() on the private read end. Both
                // pipe ends are still open -- ev_release() closes them once the
                // last reference drops -- so without a token nothing would ever
                // make that poll return. The sleeper sees `gone` and answers
                // EBADF, which is what Linux gives a read on a closed fd.
                (void)write(o->wfd, "e", 1);
            }
            ev_release(o);
        }
    }
    pthread_mutex_unlock(&g_ev_lock);
}

// ---------------------------------------------------------------- epoll table

// Caller holds g_ep_lock.
static struct ep_inst *ep_find(int kq)
{
    for (int i = 0; i < MAX_EPOLL; i++)
        if (g_ep[i].used && !g_ep[i].dead && g_ep[i].kq == kq)
            return &g_ep[i];
    return NULL;
}

// Detect if fd is a nested epoll instance (caller holds g_ep_lock).
static bool is_nested_epoll_locked(int fd)
{
    for (int i = 0; i < MAX_EPOLL; i++) {
        if (g_ep[i].used && !g_ep[i].dead && g_ep[i].kq == fd) {
            return true;
        }
    }
    return false;
}

static struct ep_interest *ep_lookup(struct ep_inst *ep, int fd)
{
    for (int i = 0; i < ep->n; i++)
        if (ep->v[i].fd == fd)
            return &ep->v[i];
    return NULL;
}

static void ep_release(struct ep_inst *ep)
{
    if (ep->dead && ep->refs == 0) {
        free(ep->v);
        ep->v = NULL;
        ep->n = ep->cap = 0;
        ep->kq = -1;
        ep->dead = false;
        ep->used = false;
    }
}

long lxrt_epoll_create1(int lflags)
{
    // EPOLL_CLOEXEC is the only flag Linux accepts; everything else is EINVAL,
    // including the old epoll_create()'s `size` argument, which is why aarch64
    // does not carry epoll_create at all.
    if (lflags & ~L_EPOLL_CLOEXEC)
        return LERR(EINVAL);

    if (atomic_load_explicit(&g_ep_live, memory_order_relaxed) >= MAX_EPOLL)
        return LERR(EMFILE);

    int kq = kqueue();
    if (kq < 0)
        return LERR(errno);

    // Divergence with no fix available: a Linux epoll fd survives fork() and
    // exec(), and the child sees the same interest list. A Darwin kqueue is
    // explicitly NOT inherited across fork -- the descriptor exists in the
    // child but its queue is empty. lxrt_fork() therefore produces a child
    // whose inherited epoll fds are inert. Documented rather than papered over:
    // re-creating the queue in the child would need a fork handler that also
    // re-registers every knote, and the interest list is not shared memory.
    if (lflags & L_EPOLL_CLOEXEC)
        fcntl(kq, F_SETFD, FD_CLOEXEC);

    pthread_mutex_lock(&g_ep_lock);
    struct ep_inst *ep = NULL;
    for (int i = 0; i < MAX_EPOLL; i++) {
        if (!g_ep[i].used) {
            ep = &g_ep[i];
            break;
        }
    }
    if (!ep) {
        pthread_mutex_unlock(&g_ep_lock);
        close(kq);
        return LERR(EMFILE);
    }
    ep->used = true;
    ep->kq = kq;
    ep->refs = 0;
    ep->dead = false;
    ep->v = NULL;
    ep->n = ep->cap = 0;
    atomic_fetch_add_explicit(&g_ep_live, 1, memory_order_relaxed);
    pthread_mutex_unlock(&g_ep_lock);
    return kq;
}

void lxrt_epoll_close(int fd)
{
    if (fd < 0 || atomic_load_explicit(&g_ep_live, memory_order_relaxed) == 0)
        return;
    pthread_mutex_lock(&g_ep_lock);
    struct ep_inst *ep = ep_find(fd);
    if (ep) {
        ep->dead = true;
        atomic_fetch_sub_explicit(&g_ep_live, 1, memory_order_relaxed);
        // The kqueue descriptor itself is the caller's to close; closing it is
        // what breaks any thread currently blocked in kevent() on it, which
        // then drops its reference and frees the slot.
        ep_release(ep);
    }
    pthread_mutex_unlock(&g_ep_lock);
}

// ---------------------------------------------------------------- epoll_ctl

// Push one interest's knotes at the queue. `mode` is EV_ADD to install and
// EV_DELETE to remove; a modify builds a delete list immediately followed by an
// add list in ONE changelist, because kevent() applies changes in order.
//
// EV_RECEIPT is used so each change reports its own status instead of kevent()
// stopping at the first failure with a bare errno.
static int ep_changes(const struct ep_interest *in, int mode,
                      struct kevent *ch, int max)
{
    int n = 0;
    uint32_t want = in->events;

    uint16_t base = (uint16_t)(mode | EV_RECEIPT);
    if (mode == EV_ADD) {
        // The default must be level-triggered, matching epoll. Measured on this
        // machine: a kqueue knote without EV_CLEAR re-reports a pipe with one
        // unread byte on every kevent() call, so plain EV_ADD already is level.
        if (want & L_EPOLLET)
            base |= EV_CLEAR;
        // EPOLLONESHOT is deliberately NOT mapped to EV_ONESHOT. The two agree
        // on intent but not on SCOPE: Linux disarms the whole interest after
        // one reported event, Darwin deletes the single knote that fired. An
        // interest registered EPOLLIN|EPOLLOUT|EPOLLONESHOT therefore installed
        // two independent one-shot knotes -- measured on a socketpair whose
        // send buffer was full, wait1 returned 0x1 (EPOLLIN) and wait2, after
        // the peer drained, returned 0x4 (EPOLLOUT), where the same sequence on
        // the 6.17.1 guest returns 0 for wait2. A guest that uses EPOLLONESHOT
        // precisely so that one descriptor is handled by one thread at a time
        // gets a second event and can double-service or free-then-use whatever
        // the cookie points at.
        //
        // The disarm is done in software instead, in pwait_common(): the whole
        // interest is EV_DELETEd the instant it reports, and oneshot_fired
        // keeps it out of the match loop until an EPOLL_CTL_MOD re-arms it --
        // which is what Linux does, measured: wait3 after MOD returned 0x5.
        // As a bonus, the read knote below survives firing with nothing to
        // report, which EV_ONESHOT would have destroyed.
    }

    // The read knote is installed for EVERY interest, not just those that asked
    // for readability. Linux owes EPOLLERR and EPOLLHUP on every interest
    // whether or not they were requested -- measured on the guest, a pipe whose
    // writer is gone reports 0x10 (EPOLLHUP) for an interest registered with
    // events=0, with EPOLLPRI alone, and with EPOLLRDBAND alone. kqueue has no
    // error-only filter, so EVFILT_READ stands in for all of them with one
    // rule.
    //
    // Before this, want_read was false unless the interest asked for read data,
    // EPOLLRDHUP, or nothing at all, which left EPOLLPRI-only registration
    // watching NOTHING: the sole change pushed was EVFILT_EXCEPT, that filter
    // returns EINVAL on a pipe (measured: EV_ERROR data=22) and ep_submit()
    // treats EXCEPT failures as soft, so epoll_ctl returned 0 having installed
    // zero knotes. Measured consequence: ADD EPOLLPRI on a pipe read end, close
    // the writer, epoll_pwait(120) -> 0 events after the full 120 ms, where
    // Linux reports EPOLLHUP. Silent success with nothing behind it is the one
    // outcome this runtime must never produce.
    bool want_write = (want & L_EPOLL_WRITE_BITS) != 0;

    uint16_t rflags = base;
    if (mode == EV_ADD && !(want & L_EPOLL_READ_EMIT)) {
        // Forced to edge mode. A level-triggered read knote whose EPOLLIN the
        // translation is going to discard -- because the guest never asked for
        // it -- fires on every single kevent() and the wait loop then has to
        // re-wait, burning a core until the timeout and forever when the
        // timeout is -1. Edge costs a late EPOLLRDHUP if the guest ignores the
        // first report; level costs a spinning event loop. Edge wins.
        //
        // The cost is measured rather than assumed: an EV_CLEAR read knote
        // registered on a pipe whose writer had ALREADY gone still reports
        // EV_EOF on the first kevent() and nothing on any later one. So the
        // hangup reaches the guest exactly once instead of on every wait as
        // Linux would. An event loop reacts to EPOLLHUP by closing or DELing
        // the descriptor, so one report is enough in practice; a loop that
        // ignores the first one will not be told again. Accepted, because the
        // alternative is the 100%-CPU livelock above.
        rflags |= EV_CLEAR;
    }

    uint16_t wflags = base;
    if (mode == EV_ADD && !(want & write_emit_bits(in))) {
        // The same rule on the write side, which previously had no forced-edge
        // path at all. EPOLLWRBAND alone on a pipe is the case that mattered:
        // the write filter can only produce EPOLLOUT|EPOLLWRNORM there, the
        // guest asked for neither, and a level knote on an always-writable pipe
        // spun the wait loop at 100% of a core.
        wflags |= EV_CLEAR;
    }

    if (n < max)
        EV_SET(&ch[n++], (uintptr_t)in->fd, EVFILT_READ, rflags, 0, 0,
               (void *)(uintptr_t)in->data);
    if (want_write && n < max)
        EV_SET(&ch[n++], (uintptr_t)in->wfd, EVFILT_WRITE, wflags, 0, 0,
               (void *)(uintptr_t)in->data);
    if ((want & L_EPOLLPRI) && n < max)
        // EVFILT_EXCEPT/NOTE_OOB is the nearest thing Darwin has to EPOLLPRI.
        // It exists only for sockets: measured, registering it on a pipe
        // returns EINVAL. That failure is tolerated below rather than
        // propagated, because Linux accepts EPOLLPRI on a pipe too and simply
        // never reports it -- and the read knote above now carries the
        // ERR/HUP half of the contract, so a pipe interest is never empty.
        EV_SET(&ch[n++], (uintptr_t)in->fd, EVFILT_EXCEPT, base, NOTE_OOB, 0,
               (void *)(uintptr_t)in->data);
    return n;
}

// Applies a changelist and returns the first hard error, or 0. EVFILT_EXCEPT
// failures are always soft (see ep_changes). The first `nsoft` entries are
// removals whose ENOENT is soft: a knote whose descriptor has already been
// closed is gone from the queue with nobody told -- measured, EV_DELETE naming
// a closed fd returns ENOENT(2), not EBADF -- and an EPOLL_CTL_MOD's leading
// delete list is expected to find nothing when the previous mask installed
// fewer filters than this one removes.
static int ep_submit(int kq, struct kevent *ch, int n, int nsoft)
{
    if (n <= 0)
        return 0;
    struct kevent out[2 * EP_MAX_CH];
    if (n > (int)(sizeof out / sizeof out[0]))
        return EINVAL;   // unreachable: every caller builds at most 2*EP_MAX_CH
    // A zero timeout, not NULL. Every change carries EV_RECEIPT, so kevent()
    // fills the output array with one receipt per change and returns without
    // waiting -- but this runs with g_ep_lock held, and "cannot block" is not a
    // property worth betting every epoll instance in the process on.
    struct timespec zero = { 0, 0 };
    int r = kevent(kq, ch, n, out, n, &zero);
    if (r < 0)
        return errno;
    for (int i = 0; i < r; i++) {
        if (!(out[i].flags & EV_ERROR) || out[i].data == 0)
            continue;
        int e = (int)out[i].data;
        if (i < nsoft && e == ENOENT)
            continue;
        if (out[i].filter == EVFILT_EXCEPT)
            continue;   // see ep_changes(): EPOLLPRI is best-effort
        return e;
    }
    return 0;
}

// Is this entry still describing the file it was registered against? Nothing
// informs this module when the guest closes a watched descriptor unless the
// integrator wires lxrt_epoll_fd_closed() (see the contract in
// epoll_eventfd.h), and kqueue drops the knote silently when the fd goes. The
// stale entry then owns the fd NUMBER forever: measured, a pipe on fd 4 added
// to an epoll, closed, and the number recycled by a fresh pipe made
// EPOLL_CTL_ADD(4) return -EEXIST where Linux returns 0, and the epoll_pwait
// after it reported nothing although fd 4 was readable -- a silently dead event
// loop. Steam recycles descriptors constantly, so this is the cheap safety net
// that works even if the hook is never wired.
//
// `st` is the caller's already-taken fstat of the descriptor, or NULL when the
// fstat failed, which means the fd is closed outright.
//
// st_ino is a strong hint and not a proof, because Darwin does not keep a
// pipe's inode number unique over time: measured over twelve create/close
// cycles, the same six values came round again in order, so a recycled fd
// number lands on its old ino roughly one time in six. ep_knote_gone() below is
// the second opinion for the one path where getting it wrong is visible.
static bool ep_entry_stale(const struct ep_interest *in, const struct stat *st)
{
    if (!st)
        return true;
    return st->st_dev != in->st_dev || st->st_ino != in->st_ino;
}

// Does this entry still have a registration in the queue? kqueue knows what
// st_ino cannot tell us: it drops every knote of a descriptor when that
// descriptor is closed, so a knote that is still present proves the fd was
// never closed since ADD. EV_ENABLE names an existing knote without creating
// one -- measured on this machine, it returns ENOENT(2) for a filter that is
// not registered and 0 for one that is, and it leaves both EV_CLEAR and udata
// intact. The read knote is used because ep_changes() installs it for every
// interest.
//
// It is not free of side effects: also measured, EV_ENABLE re-activates an
// EV_CLEAR knote whose condition is still true, so the guest may see one
// spurious event afterwards. That is why this is called only on the duplicate
// EPOLL_CTL_ADD path, which is an error return in any case, and epoll is
// allowed to wake spuriously.
static bool ep_knote_gone(int kq, const struct ep_interest *in)
{
    struct kevent ch, out;
    struct timespec zero = { 0, 0 };
    EV_SET(&ch, (uintptr_t)in->fd, EVFILT_READ, EV_ENABLE | EV_RECEIPT, 0, 0,
           (void *)(uintptr_t)in->data);
    int r = kevent(kq, &ch, 1, &out, 1, &zero);
    if (r <= 0)
        return false;            // no answer: keep the entry rather than guess
    return (out.flags & EV_ERROR) != 0 && out.data == ENOENT;
}

// Caller holds g_ep_lock. Swap-removes ep->v[idx]. `alive` says whether the
// descriptor is still open: EV_DELETE can only name a live fd, and on a closed
// one kqueue has already dropped the knote.
static void ep_drop_at(struct ep_inst *ep, int idx, bool alive)
{
    if (alive) {
        struct kevent ch[EP_MAX_CH];
        int n = ep_changes(&ep->v[idx], EV_DELETE, ch, EP_MAX_CH);
        ep_submit(ep->kq, ch, n, n);
    } else if (!alive && ep->v[idx].wfd != ep->v[idx].fd) {
        // For eventfd: the EVFILT_WRITE knote names wfd (the private pipe end),
        // which stays open while another alias holds the eventfd. Must delete it.
        struct kevent ch[1];
        EV_SET(&ch[0], (uintptr_t)ep->v[idx].wfd, EVFILT_WRITE, EV_DELETE | EV_RECEIPT, 0, 0, 0);
        struct kevent out;
        struct timespec zero = { 0, 0 };
        kevent(ep->kq, ch, 1, &out, 1, &zero);
        // Tolerate ENOENT: wfd may have been closed alongside fd.
    }
    ep->v[idx] = ep->v[ep->n - 1];
    ep->n--;
}


void lxrt_epoll_fd_closed(int fd)
{
    if (fd < 0 || atomic_load_explicit(&g_ep_live, memory_order_relaxed) == 0)
        return;

    pthread_mutex_lock(&g_ep_lock);
    for (int i = 0; i < MAX_EPOLL; i++) {
        struct ep_inst *ep = &g_ep[i];
        if (!ep->used || ep->dead)
            continue;
        for (int j = 0; j < ep->n; ) {
            // wfd as well as fd: an eventfd's writability rides on the pipe's
            // other end, which lxrt_eventfd_close() is about to close.
            if (ep->v[j].fd == fd || ep->v[j].wfd == fd)
                ep_drop_at(ep, j, true);   // the caller has not closed it yet,
                                           // by contract, so EV_DELETE works
            else
                j++;
        }
    }
    pthread_mutex_unlock(&g_ep_lock);
}

long lxrt_epoll_ctl(int epfd, int op, int fd, const void *levent)
{
    if (epfd < 0 || fd < 0)
        return LERR(EBADF);
    // Linux rejects an epoll fd watching itself before it even looks at `op`.
    if (epfd == fd)
        return LERR(EINVAL);

    struct linux_epoll_event ev = { 0, 0, 0 };
    if (op == L_EPOLL_CTL_ADD || op == L_EPOLL_CTL_MOD) {
        if (!range_ok(levent, sizeof ev, false))
            return LERR(EFAULT);
        memcpy(&ev, levent, sizeof ev);
    } else if (op != L_EPOLL_CTL_DEL) {
        return LERR(EINVAL);
    }
    // EPOLL_CTL_DEL has accepted a NULL event since 2.6.9 and ignores it.

    if (op == L_EPOLL_CTL_MOD && (ev.events & L_EPOLLEXCLUSIVE))
        return LERR(EINVAL);   // Linux does the same, for the same reason:
                               // exclusive wakeups cannot be changed in place.

    // One fstat answers everything the operation needs about the target: the
    // EBADF and EPERM checks Linux does on it, the socket-vs-pipe decision that
    // tells EPOLLHUP from EPOLLRDHUP at EV_EOF, and the identity comparison
    // that unmasks a stale entry left behind by a close this module was never
    // told about.
    struct stat tst;
    bool fd_alive = fstat(fd, &tst) == 0;

    pthread_mutex_lock(&g_ep_lock);
    struct ep_inst *ep = ep_find(epfd);
    if (!ep) {
        pthread_mutex_unlock(&g_ep_lock);
        // Not one of ours: either a real bad fd or an fd the guest obtained
        // without epoll_create1. EINVAL is what Linux returns for a valid fd
        // that is not an epoll instance.
        return fcntl(epfd, F_GETFD) < 0 ? LERR(EBADF) : LERR(EINVAL);
    }

    struct ep_interest *cur = ep_lookup(ep, fd);
    if (cur && ep_entry_stale(cur, fd_alive ? &tst : NULL)) {
        // The descriptor behind this entry is gone, and its number may already
        // belong to something else. Drop it before answering, so the guest sees
        // the fd it is actually holding rather than the ghost of the last one.
        ep_drop_at(ep, (int)(cur - ep->v), false);
        cur = ep_lookup(ep, fd);
    }

    long ret = 0;
    struct kevent ch[2 * EP_MAX_CH];

    switch (op) {
    case L_EPOLL_CTL_ADD: {
        if (!fd_alive) {
            ret = LERR(EBADF);
            break;
        }
        bool knote_was_alive = cur && !cur->oneshot_fired;   // cur may be NULL: a fresh ADD
        if (cur && knote_was_alive && ep_knote_gone(ep->kq, cur)) {
            // The number is live but our registration is not, so the
            // descriptor was closed and recycled onto an object that happened
            // to reuse the inode number. `false`: the knote is already gone, and
            // an EV_DELETE here would name in->wfd, which for an eventfd is a
            // private descriptor that may still belong to a live object.
            ep_drop_at(ep, (int)(cur - ep->v), false);
            cur = NULL;
        }
        if (cur) {
            ret = LERR(EEXIST);
            break;
        }
        // Linux returns EPERM when a regular file or a directory is added: an
        // inode with no poll method is always ready and epoll refuses to say
        // so. kqueue accepts both happily -- measured -- and would report the
        // file readable forever. libuv and libevent both use this EPERM to
        // detect "this fd needs blocking I/O, not the loop", so accepting it
        // would not be generosity, it would break their fallback.
        if (S_ISREG(tst.st_mode) || S_ISDIR(tst.st_mode)) {
            ret = LERR(EPERM);
            break;
        }
        if (ev.events & L_EPOLLEXCLUSIVE) {
            // fs/eventpoll.c, do_epoll_ctl: on ADD, EPOLLEXCLUSIVE is EINVAL
            // when combined with any bit outside EPOLLEXCLUSIVE_OK_BITS --
            // EPOLLONESHOT most importantly -- or when the target is itself an
            // epoll instance. Measured on the guest: IN|EXCLUSIVE -> 0,
            // IN|EXCLUSIVE|ONESHOT -> -EINVAL, IN|EXCLUSIVE|RDBAND -> -EINVAL,
            // and an epoll fd as the target -> -EINVAL. Half-validating this
            // gave a guest feature-probing for EPOLLEXCLUSIVE a wrong answer.
            if ((ev.events & ~L_EPOLL_EXCLUSIVE_OK_BITS) || ep_find(fd)) {
                ret = LERR(EINVAL);
                break;
            }
        }

        if (ep->n == ep->cap) {
            int ncap = ep->cap ? ep->cap * 2 : 8;
            struct ep_interest *nv = realloc(ep->v, (size_t)ncap * sizeof *nv);
            if (!nv) {
                ret = LERR(ENOMEM);
                break;
            }
            ep->v = nv;
            ep->cap = ncap;
        }
        struct ep_interest *in = &ep->v[ep->n];
        memset(in, 0, sizeof *in);
        in->fd = fd;
        // An eventfd's writability lives on the pipe's other end. Resolved once
        // here rather than on every wait.
        int wend = eventfd_write_end(fd);
        in->wfd = wend >= 0 ? wend : fd;
        in->events = ev.events;
        in->data = ev.data;
        in->st_dev = tst.st_dev;
        in->st_ino = tst.st_ino;
        // S_ISSOCK on a kqueue fd is false and S_ISFIFO is true -- Darwin
        // reports mode 010000 for one. That is the right answer for us: a
        // nested epoll behaves like a pipe, readable when it has work.
        in->is_socket = S_ISSOCK(tst.st_mode) != 0;
        // EPOLLEXCLUSIVE, once validated above, has no kqueue equivalent. It
        // exists to stop every epoll instance watching a shared listening
        // socket from waking on one connection. Ignoring its effect is safe in
        // the only direction that matters -- extra wakeups, never a missed one
        // -- and the guest's accept() already has to cope with EAGAIN.
        // EPOLLWAKEUP is an Android suspend-blocker and means nothing here; it
        // is ignored too.
        if (is_nested_epoll_locked(fd)) {
            ev.events &= ~(L_EPOLLOUT | L_EPOLLWRNORM | L_EPOLLWRBAND);
        }
        int nch = ep_changes(in, EV_ADD, ch, EP_MAX_CH);
        int e = ep_submit(ep->kq, ch, nch, 0);
        if (e) {
            // Roll back any knote that did install, so a failed ADD leaves no
            // trace -- Linux's ADD is all-or-nothing.
            int back = ep_changes(in, EV_DELETE, ch, EP_MAX_CH);
            ep_submit(ep->kq, ch, back, back);
            // EINVAL from an EVFILT_READ/EVFILT_WRITE registration is Darwin
            // saying this descriptor type has no kqueue filter, which is the
            // same condition Linux calls "no poll method" and answers with
            // EPERM. Measured on both sides: kqueue EV_ADD of either filter on
            // /dev/null, /dev/zero and /dev/urandom returns EINVAL(22), while
            // epoll_ctl(ADD) on the same three files on the 6.17.1 guest
            // returns EPERM(1) for both EPOLLIN and EPOLLOUT. libuv and
            // libevent branch on that EPERM to fall back to blocking I/O, so
            // handing them EINVAL instead would break the fallback in exactly
            // the way the S_ISREG check above exists to avoid. Pipes, FIFOs,
            // sockets, and listening sockets register both
            // filters cleanly. nested kqueues: EVFILT_WRITE returns EINVAL (measured). For nested epoll, the no-filter
            // case and not a malformed changelist.
            // For regular files: EINVAL->EPERM; for nested epoll: skip EVFILT_WRITE
            ret = (e == EINVAL) ? LERR(EPERM) : LERR(e);
            break;
        }
        ep->n++;
        break;
    }
    case L_EPOLL_CTL_MOD: {
        if (!cur) {
            // Measured on the guest: EPOLL_CTL_DEL/MOD naming a closed
            // descriptor returns EBADF, not ENOENT -- fget fails before the
            // interest tree is consulted. The stale-entry drop above is what
            // makes that reachable here at all.
            ret = fd_alive ? LERR(ENOENT) : LERR(EBADF);
            break;
        }
        // MOD replaces, it does not accumulate. Dropping EPOLLOUT from the mask
        // has to actually delete the write knote, so the old registration is
        // torn down and the new one built from scratch -- as ONE changelist,
        // because two separate kevent() calls leave a window in which another
        // thread's epoll_pwait sees the descriptor unregistered.
        struct ep_interest old = *cur;
        struct ep_interest upd = *cur;
        upd.events = ev.events;
        upd.data = ev.data;
        // EPOLL_CTL_MOD is how Linux re-arms an EPOLLONESHOT interest --
        // measured: after a oneshot report wait2 gave 0 and wait3, after MOD,
        // gave 0x5.
        upd.oneshot_fired = false;
        int ndel = ep_changes(&old, EV_DELETE, ch, EP_MAX_CH);
        int nadd = ep_changes(&upd, EV_ADD, ch + ndel, EP_MAX_CH);
        int e = ep_submit(ep->kq, ch, ndel + nadd, ndel);
        if (e) {
            // A partially applied changelist is the state that has to be
            // undone: an add that installed for a mask the interest will not
            // carry leaves a knote nothing reports, and before the arming rule
            // in ep_changes() that stale knote could spin the wait loop. Tear
            // the NEW registration down first -- the previous code restored
            // *cur and re-added the OLD knotes without ever deleting what the
            // failed attempt had installed, so it was not a rollback at all.
            int back = ep_changes(&upd, EV_DELETE, ch, EP_MAX_CH);
            ep_submit(ep->kq, ch, back, back);
            back = ep_changes(&old, EV_ADD, ch, EP_MAX_CH);
            ep_submit(ep->kq, ch, back, 0);
            ret = LERR(e);
        } else {
            *cur = upd;
        }
        break;
    }
    case L_EPOLL_CTL_DEL: {
        if (!cur) {
            ret = fd_alive ? LERR(ENOENT) : LERR(EBADF);
            break;
        }
        // ENOENT from the queue is expected whenever the descriptor is no
        // longer registered behind our back; ep_drop_at() treats it as soft.
        ep_drop_at(ep, (int)(cur - ep->v), fd_alive);
        break;
    }
    default:
        ret = LERR(EINVAL);
        break;
    }

    pthread_mutex_unlock(&g_ep_lock);
    return ret;
}

// ---------------------------------------------------------------- epoll_pwait

// One coalesced output slot. kqueue delivers read and write readiness for the
// same descriptor as two kevents; epoll owes the guest one. `data` is copied in
// under g_ep_lock so that the store into guest memory can happen with the lock
// dropped -- see the copy-out in pwait_common().
struct out_slot {
    int      idx;      // index into ep->v
    uint32_t mask;
    uint64_t data;
    bool     eof_read;
    bool     eof_write;
};

// Translate one kevent into Linux event bits for an interest that is already
// known. Returns 0 when the event carries nothing the guest asked for, which
// happens for an edge-armed stand-in knote and must not reach the guest: Linux
// never reports an epoll_event with an empty mask.
//
// The bits produced here and the level-vs-edge decision in ep_changes() are two
// halves of one invariant: a LEVEL-triggered knote always produces at least one
// bit that survives `allowed` below, so it can never drive the caller's re-wait
// loop round a second time with nothing to show. Breaking that invariant is
// what the measured 100%-CPU livelock was.
static uint32_t kev_to_epoll(const struct kevent *k, const struct ep_interest *in,
                             bool *eof_read, bool *eof_write)
{
    uint32_t m = 0;
    uint32_t want = in->events;

    if (k->flags & EV_ERROR) {
        // The changelist is empty during a wait, so EV_ERROR here is a
        // condition on the knote rather than a rejected registration.
        return L_EPOLLERR;
    }

    switch (k->filter) {
    case EVFILT_READ:
        // Report EPOLLIN even at EOF. Linux does, and it is what lets the guest
        // call read() and see the 0 that tells it the peer is gone; withholding
        // it turns a clean shutdown into a stall.
        //
        // EPOLLRDBAND is deliberately absent: measured on the 6.17.1 guest, a
        // readable pipe and a readable unix socket both report 0 events for an
        // EPOLLRDBAND-only interest, so Linux never sets it for ordinary
        // readability and emitting it here would invent an event.
        m |= L_EPOLL_READ_EMIT;
        if (k->flags & EV_EOF) {
            *eof_read = true;
            if (in->is_socket) {
                // Socket EOF is a received FIN: the read half is done, the
                // write half may still be usable. That is exactly EPOLLRDHUP,
                // and Linux only reports it when asked. EPOLLHUP waits until
                // the write side is also gone -- decided after coalescing.
                if (want & L_EPOLLRDHUP)
                    m |= L_EPOLLRDHUP;
                // For sockets kqueue parks SO_ERROR in fflags at EOF.
                if (k->fflags != 0)
                    m |= L_EPOLLERR;
            } else {
                // A pipe or FIFO whose writer closed is a full hangup; there is
                // no half-open state to be in. Linux gives EPOLLHUP|EPOLLIN.
                m |= L_EPOLLHUP;
            }
        }
        break;

    case EVFILT_WRITE:
        // EPOLLWRBAND rides along on a socket and not on a pipe, because that
        // is what Linux does: unix_poll and tcp_poll set
        // EPOLLOUT|EPOLLWRNORM|EPOLLWRBAND while pipe_poll sets only
        // EPOLLOUT|EPOLLWRNORM. Measured on the guest: a writable unix socket
        // with an EPOLLWRBAND-only interest reports 0x200, a writable pipe with
        // the same interest reports nothing.
        m |= write_emit_bits(in);
        if (k->flags & EV_EOF) {
            *eof_write = true;
            if (in->is_socket)
                m |= L_EPOLLHUP;
            else
                // A pipe whose reader closed: the next write() gets EPIPE.
                // poll() on Linux reports POLLERR for this, and it keeps
                // POLLOUT set because the write will not block, it will fail.
                m |= L_EPOLLERR;
        }
        if (k->fflags != 0 && in->is_socket)
            m |= L_EPOLLERR;
        break;

    case EVFILT_EXCEPT:
        m |= L_EPOLLPRI;
        break;

    default:
        return 0;
    }

    // Mask down to what was requested. EPOLLERR and EPOLLHUP are the two
    // exceptions: Linux reports them unconditionally, which is why an event
    // loop that registers only EPOLLIN still learns that a connection died.
    uint32_t allowed = (want & (L_EPOLL_READ_BITS | L_EPOLL_WRITE_BITS |
                                L_EPOLLPRI | L_EPOLLRDHUP)) |
                       L_EPOLLERR | L_EPOLLHUP;
    return m & allowed;
}

static long pwait_common(int epfd, void *uevents, int maxevents,
                         const struct timespec *rel, bool infinite,
                         const uint64_t *lsigmask, size_t sigsetsize)
{
    // Linux's EP_MAX_EVENTS is INT_MAX / sizeof(struct epoll_event), so the
    // multiplication below cannot overflow and a guest cannot ask the runtime
    // to probe a nonsense range. The divisor is the aarch64 16-byte struct, not
    // the x86 12-byte one -- the cap differs between the two architectures and
    // this is the aarch64 runtime.
    if (maxevents <= 0 ||
        maxevents > (int)(INT_MAX / sizeof(struct linux_epoll_event)))
        return LERR(EINVAL);

    // One event is probed here, not the whole maxevents span. Linux's
    // do_epoll_wait does access_ok() over the full range, but access_ok() only
    // asks "is this address user space", never "is it mapped" -- the real fault
    // check is the per-event __put_user in ep_send_events. Probing
    // maxevents * 16 up front therefore rejected with EFAULT a guest that Linux
    // accepts: one that passes a maxevents larger than its buffer and relies on
    // fewer events being ready. glibc always sizes the two to match, so this
    // was unlikely to bite, but it was a divergence in the breaking direction.
    // Every entry is re-probed for real immediately before it is written.
    if (!range_ok(uevents, sizeof(struct linux_epoll_event), true))
        return LERR(EFAULT);

    // Linux checks sigsetsize only when a mask is actually supplied
    // (set_user_sigmask returns 0 for a NULL pointer before it validates).
    uint64_t oldmask = 0;
    bool swapped = false;
    if (lsigmask) {
        if (sigsetsize != L_KERNEL_SIGSET_BYTES)
            return LERR(EINVAL);
        if (!range_ok(lsigmask, sizeof(uint64_t), false))
            return LERR(EFAULT);
        long rc = lxrt_rt_sigprocmask(L_SIG_SETMASK, lsigmask, &oldmask,
                                      L_KERNEL_SIGSET_BYTES);
        if (rc < 0)
            return rc;
        swapped = true;
    }

    int kq;
    struct ep_inst *ep;
    pthread_mutex_lock(&g_ep_lock);
    ep = ep_find(epfd);
    if (!ep) {
        pthread_mutex_unlock(&g_ep_lock);
        if (swapped)
            lxrt_rt_sigprocmask(L_SIG_SETMASK, &oldmask, NULL,
                                L_KERNEL_SIGSET_BYTES);
        return fcntl(epfd, F_GETFD) < 0 ? LERR(EBADF) : LERR(EINVAL);
    }
    // Hold a reference across the wait. The instance must not be freed while
    // this thread sits in kevent() with the table unlocked, and the table must
    // be unlocked or a concurrent epoll_ctl on the same instance -- which is
    // the normal shape of a multi-threaded event loop -- would deadlock.
    ep->refs++;
    kq = ep->kq;
    pthread_mutex_unlock(&g_ep_lock);

    // 64 covers the maxevents the measured loops actually pass, so the hot path
    // makes no allocation at all: 64 * (32 + 24) bytes of stack.
    int kn = maxevents > KEV_BATCH_MAX ? KEV_BATCH_MAX : maxevents;
    struct kevent kstack[64];
    struct out_slot ostack[64];
    struct kevent *kv = kstack;
    struct out_slot *os = ostack;
    if (kn > (int)(sizeof kstack / sizeof kstack[0])) {
        kv = malloc((size_t)kn * sizeof *kv);
        os = malloc((size_t)kn * sizeof *os);
        if (!kv || !os) {
            free(kv);
            free(os);
            pthread_mutex_lock(&g_ep_lock);
            ep->refs--;
            ep_release(ep);
            pthread_mutex_unlock(&g_ep_lock);
            if (swapped)
                lxrt_rt_sigprocmask(L_SIG_SETMASK, &oldmask, NULL,
                                    L_KERNEL_SIGSET_BYTES);
            return LERR(ENOMEM);
        }
    }

    // A deadline, not a per-iteration timeout. Coalescing and the edge-armed
    // stand-in knote can both turn a non-empty kevent() return into zero
    // reportable events, and returning 0 early would look like a timeout to the
    // guest and fire whatever its idle path is. Re-waiting on the remaining
    // time is the only honest answer.
    struct timespec deadline = { 0, 0 };
    if (!infinite) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        // epoll_pwait2 forwards whatever non-negative tv_sec the guest hands
        // it, and glibc does not clamp, so now.tv_sec + rel->tv_sec was signed
        // overflow: UB, and in practice a deadline in the PAST, which returned
        // 0 immediately and looked to the guest like a spurious timeout in its
        // idle path. A wait that long is a wait forever -- measured on the
        // guest, a real epoll_pwait2 with tv_sec = INT64_MAX-1 never returned.
        if (rel->tv_sec > (time_t)INT64_MAX - now.tv_sec - 1) {
            infinite = true;
        } else {
            deadline.tv_sec = now.tv_sec + rel->tv_sec;
            deadline.tv_nsec = now.tv_nsec + rel->tv_nsec;
            if (deadline.tv_nsec >= 1000000000L) {
                deadline.tv_nsec -= 1000000000L;
                deadline.tv_sec += 1;
            }
        }
    }

    long ret = 0;
    for (;;) {
        struct timespec left, *tsp = NULL;
        if (!infinite) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            left.tv_sec = deadline.tv_sec - now.tv_sec;
            left.tv_nsec = deadline.tv_nsec - now.tv_nsec;
            if (left.tv_nsec < 0) {
                left.tv_nsec += 1000000000L;
                left.tv_sec -= 1;
            }
            if (left.tv_sec < 0)
                left.tv_sec = left.tv_nsec = 0;
            tsp = &left;
        }

        int n = kevent(kq, NULL, 0, kv, kn, tsp);
        if (n < 0) {
            // EINTR is not restarted: epoll_pwait exists precisely so the guest
            // can see the signal, and Linux does not set SA_RESTART semantics
            // on it.
            ret = LERR(errno);
            break;
        }

        int nout = 0;
        pthread_mutex_lock(&g_ep_lock);
        for (int i = 0; i < n; i++) {
            // Match by (ident, filter). The write knote of an eventfd carries
            // the pipe's write end as its ident, which is a descriptor the
            // guest never sees, so the two idents cannot collide.
            int idx = -1;
            for (int j = 0; j < ep->n; j++) {
                if (ep->v[j].oneshot_fired)
                    continue;   // EPOLLONESHOT: disarmed until EPOLL_CTL_MOD
                bool hit = (kv[i].filter == EVFILT_WRITE)
                               ? (uintptr_t)ep->v[j].wfd == kv[i].ident
                               : (uintptr_t)ep->v[j].fd == kv[i].ident;
                if (hit) {
                    idx = j;
                    break;
                }
            }
            if (idx < 0)
                continue;   // EPOLL_CTL_DEL raced this wait; the knote is stale

            bool eof_r = false, eof_w = false;
            uint32_t m = kev_to_epoll(&kv[i], &ep->v[idx], &eof_r, &eof_w);

            int slot = -1;
            for (int s = 0; s < nout; s++) {
                if (os[s].idx == idx) {
                    slot = s;
                    break;
                }
            }
            if (slot < 0) {
                if (nout >= kn)
                    continue;
                slot = nout++;
                os[slot].idx = idx;
                os[slot].mask = 0;
                os[slot].data = 0;
                os[slot].eof_read = os[slot].eof_write = false;
            }
            os[slot].mask |= m;
            os[slot].eof_read |= eof_r;
            os[slot].eof_write |= eof_w;
        }

        // Finalise every slot while the lock is held -- it reads ep->v, and the
        // EPOLLONESHOT disarm must be atomic with the decision to report, so a
        // second thread's epoll_pwait cannot also claim this interest. Empty
        // masks are dropped and the survivors compacted to the front, carrying
        // their cookie with them, so that nothing below needs ep->v again.
        int nready = 0;
        for (int s = 0; s < nout; s++) {
            struct ep_interest *in = &ep->v[os[s].idx];
            uint32_t m = os[s].mask;
            // A socket with both halves down is a real hangup. Linux only
            // reports EPOLLHUP once neither direction can make progress, which
            // is what "EOF on the read filter AND on the write filter" says.
            if (in->is_socket && os[s].eof_read && os[s].eof_write)
                m |= L_EPOLLHUP;
            if (m == 0)
                continue;
            if (in->events & L_EPOLLONESHOT) {
                // Linux disarms the whole INTEREST after one reported event,
                // not the filter that fired. Every knote of this interest goes,
                // and oneshot_fired keeps the entry out of the match loop until
                // an EPOLL_CTL_MOD re-arms it.
                in->oneshot_fired = true;
                struct kevent dch[EP_MAX_CH];
                int dn = ep_changes(in, EV_DELETE, dch, EP_MAX_CH);
                ep_submit(kq, dch, dn, dn);
            }
            uint64_t cookie = in->data;
            os[nready].mask = m;
            os[nready].data = cookie;
            nready++;
        }
        pthread_mutex_unlock(&g_ep_lock);

        // Belt and braces: nready <= nout <= kn <= maxevents already, so this
        // never bites. It is here because an interest that is counted but not
        // written would have been disarmed by the EPOLLONESHOT branch above
        // without ever being reported.
        if (nready > maxevents)
            nready = maxevents;

        if (nready > 0) {
            // The copy into guest memory happens with g_ep_lock DROPPED and the
            // destination proven writable again, immediately before the store.
            // The probe at entry is a check-then-use across an unbounded wait:
            // a guest thread that munmap()s or mprotect()s the buffer while the
            // waiter is blocked -- ordinary behaviour when a guest tears down a
            // thread or an arena -- used to fault the host. Measured,
            // deterministic over three runs: validate, wait(-1) on another
            // thread, munmap, fire the event -> SIGSEGV, exit 139, with
            // g_ep_lock still held, so even a recovering fault handler would
            // have wedged every epoll instance in the process.
            //
            // What remains is a window of a few hundred nanoseconds between the
            // probe and the memcpy, rather than the whole wait, and no lock is
            // held across it. Closing it completely needs a store that reports
            // a fault instead of taking one; mach_vm_write() is not it --
            // measured on this machine, mach_vm_write() into a freshly
            // munmap()ed page of our own task returns KERN_SUCCESS -- so that
            // would take a recovery landing pad, which belongs in the signal
            // module and not here.
            size_t esz = sizeof(struct linux_epoll_event);
            char *dst = (char *)uevents;
            int written = 0;
            if (range_ok(dst, (size_t)nready * esz, true)) {
                written = nready;
            } else {
                // Part of the buffer went away. Linux's ep_send_events faults
                // one event at a time and keeps whatever it already copied,
                // returning EFAULT only when it copied nothing at all.
                while (written < nready &&
                       range_ok(dst + (size_t)written * esz, esz, true))
                    written++;
            }
            for (int s = 0; s < written; s++) {
                // memcpy into a local, not a member store through a
                // struct linux_epoll_event * aimed at the guest's pointer. The
                // guest chose this address and is promised 16 bytes per entry,
                // never 8-byte alignment, and aarch64 traps unaligned accesses
                // to device memory; a member store is also strict-aliasing UB
                // on a void * the guest owns. Same discipline as the eventfd
                // counter store in lxrt_eventfd_read().
                struct linux_epoll_event e;
                e.events = os[s].mask;
                e.pad_aarch64 = 0;
                e.data = os[s].data;
                memcpy(dst + (size_t)s * esz, &e, sizeof e);
            }
            // Stated divergence on the EFAULT path: an EPOLLONESHOT interest
            // that was disarmed above is not re-armed when the copy-out fails,
            // whereas Linux's ep_send_events puts the event back on the ready
            // list. Reaching it means the guest unmapped the very buffer it
            // asked the kernel to fill while the call was in flight, which is
            // not a state it recovers from, and re-arming would mean holding
            // g_ep_lock across the guest-memory store again -- the exact thing
            // this block exists to avoid.
            ret = written > 0 ? (long)written : LERR(EFAULT);
            break;
        }
        if (n == 0) {
            ret = 0;        // kevent hit the deadline
            break;
        }
        if (infinite)
            continue;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            ret = 0;
            break;
        }
    }

    if (kv != kstack)
        free(kv);
    if (os != ostack)
        free(os);

    pthread_mutex_lock(&g_ep_lock);
    ep->refs--;
    ep_release(ep);
    pthread_mutex_unlock(&g_ep_lock);

    if (swapped) {
        // The swap is not atomic with the wait, which is the one thing
        // ppoll/epoll_pwait exist to provide. Closing the window needs a
        // kevent() that takes a mask and there is none, so the race is stated
        // here rather than hidden: a signal delivered between the unmask and
        // the kevent() runs its handler and is not turned into an EINTR return.
        // dispatch.c's do_ppoll() carries the same hole.
        lxrt_rt_sigprocmask(L_SIG_SETMASK, &oldmask, NULL, L_KERNEL_SIGSET_BYTES);
    }
    return ret;
}

long lxrt_epoll_pwait(int epfd, void *levents, int maxevents, int timeout_ms,
                      const uint64_t *lsigmask, size_t sigsetsize)
{
    struct timespec rel = { 0, 0 };
    bool infinite = timeout_ms < 0;    // any negative value, not just -1
    if (!infinite) {
        rel.tv_sec = timeout_ms / 1000;
        rel.tv_nsec = (long)(timeout_ms % 1000) * 1000000L;
    }
    return pwait_common(epfd, levents, maxevents, &rel, infinite,
                        lsigmask, sigsetsize);
}

long lxrt_epoll_pwait2(int epfd, void *levents, int maxevents,
                       const void *ltimespec, const uint64_t *lsigmask,
                       size_t sigsetsize)
{
    // epoll_pwait2 takes struct __kernel_timespec: two signed 64-bit fields on
    // every architecture, so it matches Darwin's struct timespec on arm64
    // (time_t is 64-bit, tv_nsec is a 64-bit long) field for field. The copy is
    // still explicit -- this is the same shape as the timeval trap in
    // dispatch.c, where Darwin's 32-bit tv_usec sits in what Linux uses as the
    // high half.
    struct timespec rel = { 0, 0 };
    bool infinite = (ltimespec == NULL);   // NULL means block forever
    if (!infinite) {
        if (!range_ok(ltimespec, sizeof(struct linux_timespec), false))
            return LERR(EFAULT);
        struct linux_timespec lts;
        memcpy(&lts, ltimespec, sizeof lts);
        if (lts.tv_sec < 0 || lts.tv_nsec < 0 || lts.tv_nsec >= 1000000000LL)
            return LERR(EINVAL);
        // tv_sec is forwarded unclamped, exactly as Linux forwards it;
        // pwait_common() is where an absurd value becomes an infinite wait
        // rather than a signed-overflow deadline in the past.
        rel.tv_sec = (time_t)lts.tv_sec;
        rel.tv_nsec = (long)lts.tv_nsec;
    }
    return pwait_common(epfd, levents, maxevents, &rel, infinite,
                        lsigmask, sigsetsize);
}
