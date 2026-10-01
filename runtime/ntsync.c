// /dev/ntsync -- Linux's NT synchronization driver (drivers/misc/ntsync.c,
// Linux 6.14), emulated across processes.
//
// Wine's server makes every Windows semaphore, mutex and event an ntsync
// object and hands its fd to the processes that use it (SCM_RIGHTS); each
// process then waits on and signals the objects itself, without a round trip
// to the server. Proton Experimental opens /dev/ntsync whenever it exists
// ("ntsync: up and running", PROTON_NO_NTSYNC turns it off), so it is offered
// only when the launcher asks for it: LXRT_NTSYNC=1 (Settings ->
// Sincronización -> NTSYNC). Without it, opening /dev/ntsync fails with ENOENT
// as before and Proton keeps fsync (futex_waitv.c).
//
// A device is a directory of the user's (/tmp/lxrt-ntsync-UID/d-PID-N):
//
//   device      the arena, mapped MAP_SHARED by every process that has the
//               device or one of its objects: a header with the device lock,
//               a table of objects (type, state, generation) and a table of
//               waiters (the objects each blocked thread waits for, and the
//               word it sleeps on). The device fd is an fd of this file.
//   o<slot>     one file per object, whose fd is the object's fd. Its first
//               bytes name the slot, the generation and the device.
//
// Every fd holds a shared flock on its file: flock locks belong to the open
// file description, which dup, fork and SCM_RIGHTS share, so a file is
// unlocked exactly when no process has an fd of it any more -- the kernel's
// "last fput", without tracking a single close. An object slot whose file
// can be locked exclusively is free again; a device directory whose files
// all can is removed.
//
// One lock per device (a futex-style word in the arena, Darwin shared
// ulocks) serializes every operation; the kernel driver locks per object and
// takes a device lock for wait-all, the semantics are the same. A signal
// hands the object to its waiters in queue order, as the driver's
// try_wake_any_* and try_wake_all do, before the ioctl returns: a semaphore
// released to a waiter reads 0 afterwards. A waiter sleeps on its own word
// in the arena. A lock held by a process that died is taken over after a
// 200 ms wait; a waiter of a process that died is dropped when a signal
// would have woken it.
//
// tests/elf/ntsync.c covers the ioctls across processes; the kernel's own
// selftests (tools/testing/selftests/drivers/ntsync) pass 13/13
// (benchmarks/stage45).
#include "lxrt.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LERR(e) (-(long)lxrt_errno_to_linux(e))

extern int __ulock_wait(uint32_t operation, void *addr, uint64_t value, uint32_t timeout_us);
extern int __ulock_wake(uint32_t operation, void *addr, uint64_t wake_value);
#define UL_COMPARE_AND_WAIT_SHARED 3
#define ULF_WAKE_ALL               0x00000100
#define ULF_NO_ERRNO               0x01000000

// include/uapi/linux/ntsync.h
struct nt_sem_args { uint32_t count, max; };
struct nt_mutex_args { uint32_t owner, count; };
struct nt_event_args { uint32_t manual, signaled; };
struct nt_wait_args {
    uint64_t timeout, objs;
    uint32_t count, index, flags, owner, alert, pad;
};
_Static_assert(sizeof(struct nt_wait_args) == 40, "struct ntsync_wait_args");
#define NT_WAIT_REALTIME 0x1
#define NT_MAX_WAIT      64
#define L_IOC(dir, nr, size) (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ('N' << 8) | (nr))
#define L_W 1u
#define L_R 2u
#define NT_CREATE_SEM   L_IOC(L_W, 0x80, 8)
#define NT_SEM_RELEASE  L_IOC(L_W | L_R, 0x81, 4)
#define NT_WAIT_ANY     L_IOC(L_W | L_R, 0x82, 40)
#define NT_WAIT_ALL     L_IOC(L_W | L_R, 0x83, 40)
#define NT_CREATE_MUTEX L_IOC(L_W, 0x84, 8)
#define NT_MUTEX_UNLOCK L_IOC(L_W | L_R, 0x85, 8)
#define NT_MUTEX_KILL   L_IOC(L_W, 0x86, 4)
#define NT_CREATE_EVENT L_IOC(L_W, 0x87, 8)
#define NT_EVENT_SET    L_IOC(L_R, 0x88, 4)
#define NT_EVENT_RESET  L_IOC(L_R, 0x89, 4)
#define NT_EVENT_PULSE  L_IOC(L_R, 0x8a, 4)
#define NT_SEM_READ     L_IOC(L_R, 0x8b, 8)
#define NT_MUTEX_READ   L_IOC(L_R, 0x8c, 8)
#define NT_EVENT_READ   L_IOC(L_R, 0x8d, 8)

enum { T_FREE, T_SEM, T_MUTEX, T_EVENT };

#define NT_MAGIC_DEV 0x5645444e59535444ull   // "DTSYNDEV"
#define NT_MAGIC_OBJ 0x4a424f4e59535444ull   // "DTSYNOBJ"
#define NT_OBJS  65536
#define NT_QS    1024
#define NT_ENT   (NT_MAX_WAIT + 1)            // the objects and the alert

struct nt_obj {
    uint32_t type, gen;
    uint32_t a, b;             // sem: count, max; mutex: owner, count; event: manual, signaled
    uint32_t ownerdead, pad[3];
};

struct nt_q {
    _Atomic uint32_t wake;     // the word the waiter sleeps on
    _Atomic int32_t signaled;  // -1, or the index of the entry that woke it
    uint32_t state;            // 0 free, 1 waiting
    uint32_t all, count, total, owner, ownerdead;
    uint64_t seq;              // queue order
    int32_t pid;
    uint32_t pad;
    uint32_t slot[NT_ENT], gen[NT_ENT];
};

struct nt_hdr {
    uint64_t magic;
    _Atomic uint32_t lock;     // 0 free, 1 held, 2 held and contended
    _Atomic int32_t lock_pid;
    uint64_t seq;
    uint32_t obj_next, gc_next, q_high, pad;
    char dir[512];
};

#define HDR_BYTES  4096
#define OBJ_OFF    HDR_BYTES
#define Q_OFF      (OBJ_OFF + NT_OBJS * sizeof(struct nt_obj))
#define ARENA_BYTES (Q_OFF + NT_QS * sizeof(struct nt_q))

struct nt_file {               // the first bytes of an object file
    uint64_t magic;
    uint32_t slot, gen;
    char dev[512];
};

struct arena {
    dev_t dev;
    ino_t ino;
    struct nt_hdr *h;
};

// This process's view: arenas it mapped, and fds it has identified.
static struct arena g_arenas[32];
static int g_narenas;
static pthread_mutex_t g_local = PTHREAD_MUTEX_INITIALIZER;

static struct nt_obj *objs(struct nt_hdr *h) { return (struct nt_obj *)((char *)h + OBJ_OFF); }
static struct nt_q *qs(struct nt_hdr *h) { return (struct nt_q *)((char *)h + Q_OFF); }

bool lxrt_ntsync_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_NTSYNC");
        on = e && *e == '1';
    }
    return on;
}

// -------------------------------------------------------------- the lock

static bool pid_dead(int32_t pid)
{
    return pid > 0 && kill(pid, 0) != 0 && errno == ESRCH;
}

static void nt_lock(struct nt_hdr *h)
{
    uint32_t z = 0;
    if (!atomic_compare_exchange_strong(&h->lock, &z, 1)) {
        while (atomic_exchange(&h->lock, 2) != 0) {
            int r = __ulock_wait(UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO, (void *)&h->lock, 2, 200000);
            if (r == -ETIMEDOUT && pid_dead(atomic_load(&h->lock_pid))) {
                // Its holder died with it: no one else will release it.
                atomic_store(&h->lock_pid, 0);
                atomic_store(&h->lock, 0);
            }
        }
    }
    atomic_store(&h->lock_pid, (int32_t)getpid());
}

static void nt_unlock(struct nt_hdr *h)
{
    atomic_store(&h->lock_pid, 0);
    if (atomic_exchange(&h->lock, 0) == 2)
        __ulock_wake(UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO, (void *)&h->lock, 0);
}

// -------------------------------------------------------- files and fds

static const char *base_dir(void)
{
    static char dir[96];
    if (!dir[0]) {
        snprintf(dir, sizeof dir, "/tmp/lxrt-ntsync-%u", (unsigned)getuid());
        mkdir(dir, 0700);
    }
    return dir;
}

// A file nobody holds: an exclusive flock succeeds on a fresh open of it.
static bool file_unheld(const char *path)
{
    int fd = lxrt_open_private(path, O_RDONLY | O_CLOEXEC, 0);
    if (fd < 0)
        return errno == ENOENT;
    bool free_ = flock(fd, LOCK_EX | LOCK_NB) == 0;
    lxrt_close_private(fd);
    return free_;
}

static void obj_path(const struct nt_hdr *h, uint32_t slot, char *out, size_t n)
{
    snprintf(out, n, "%s/o%u", h->dir, slot);
}

static struct nt_hdr *map_arena(int fd, const struct stat *st)
{
    pthread_mutex_lock(&g_local);
    struct nt_hdr *h = NULL;
    for (int i = 0; i < g_narenas && !h; i++)
        if (g_arenas[i].dev == st->st_dev && g_arenas[i].ino == st->st_ino)
            h = g_arenas[i].h;
    if (!h && g_narenas < (int)(sizeof g_arenas / sizeof g_arenas[0])) {
        void *p = mmap(NULL, ARENA_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED && ((struct nt_hdr *)p)->magic == NT_MAGIC_DEV) {
            h = p;
            g_arenas[g_narenas++] = (struct arena){ st->st_dev, st->st_ino, h };
        } else if (p != MAP_FAILED) {
            munmap(p, ARENA_BYTES);
        }
    }
    pthread_mutex_unlock(&g_local);
    return h;
}

static struct nt_hdr *map_arena_path(const char *path)
{
    int fd = lxrt_open_private(path, O_RDWR | O_CLOEXEC, 0);
    if (fd < 0)
        return NULL;
    struct stat st;
    struct nt_hdr *h = fstat(fd, &st) == 0 ? map_arena(fd, &st) : NULL;
    lxrt_close_private(fd);
    return h;
}

// Files already identified by this process, by device, inode and birth
// time (an inode number comes back for a new file once an old one is
// removed; its birth time does not). Without it every ioctl opened and read
// the files again: 117 us per event round trip, against 15 us for fsync
// (tests/win/run.sh sync_*).
struct known {
    dev_t dev;
    ino_t ino;
    struct timespec born;
    struct nt_hdr *h;
    uint32_t slot, gen;
    int kind;
};
#define KNOWN 4096
static struct known g_known[KNOWN];

static struct known *known_slot(const struct stat *st)
{
    return &g_known[((uint64_t)st->st_ino * 0x9e3779b97f4a7c15ull) >> 52];
}

static int identify_slow(int fd, const struct stat *st, struct nt_hdr **h, uint32_t *slot, uint32_t *gen);

// What an fd is: 1 a device, 2 an object (*slot, *gen), 0 neither.
static int identify(int fd, struct nt_hdr **h, uint32_t *slot, uint32_t *gen)
{
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    pthread_mutex_lock(&g_local);
    struct known *k = known_slot(&st);
    if (k->kind && k->dev == st.st_dev && k->ino == st.st_ino &&
        k->born.tv_sec == st.st_birthtimespec.tv_sec && k->born.tv_nsec == st.st_birthtimespec.tv_nsec) {
        *h = k->h;
        *slot = k->slot;
        *gen = k->gen;
        int kind = k->kind;
        pthread_mutex_unlock(&g_local);
        return kind;
    }
    pthread_mutex_unlock(&g_local);
    int kind = identify_slow(fd, &st, h, slot, gen);
    if (kind) {
        pthread_mutex_lock(&g_local);
        *k = (struct known){ st.st_dev, st.st_ino, st.st_birthtimespec, *h, kind == 2 ? *slot : 0,
                             kind == 2 ? *gen : 0, kind };
        pthread_mutex_unlock(&g_local);
    }
    return kind;
}

static int identify_slow(int fd, const struct stat *stp, struct nt_hdr **h, uint32_t *slot, uint32_t *gen)
{
    struct stat st = *stp;
    uint64_t magic = 0;
    if (pread(fd, &magic, sizeof magic, 0) != (ssize_t)sizeof magic)
        return 0;
    if (magic == NT_MAGIC_DEV) {
        *h = map_arena(fd, &st);
        return *h ? 1 : 0;
    }
    if (magic != NT_MAGIC_OBJ)
        return 0;
    struct nt_file f;
    if (pread(fd, &f, sizeof f, 0) != (ssize_t)sizeof f)
        return 0;
    f.dev[sizeof f.dev - 1] = 0;
    *h = map_arena_path(f.dev);
    if (!*h || f.slot >= NT_OBJS)
        return 0;
    *slot = f.slot;
    *gen = f.gen;
    return 2;
}

// ------------------------------------------------------------ the device

long lxrt_ntsync_open(int lflags)
{
    // Directories no process holds any more (their device and objects all
    // closed, or their processes gone) go first.
    const char *base = base_dir();
    DIR *d = opendir(base);
    if (d) {
        struct dirent *e;
        char path[1024];
        while ((e = readdir(d))) {
            if (strncmp(e->d_name, "d-", 2) != 0)
                continue;
            snprintf(path, sizeof path, "%s/%s/device", base, e->d_name);
            if (!file_unheld(path))
                continue;
            snprintf(path, sizeof path, "%s/%s", base, e->d_name);
            DIR *o = opendir(path);
            bool held = false;
            if (o) {
                struct dirent *f;
                char fp[1100];
                while (!held && (f = readdir(o)))
                    if (f->d_name[0] == 'o') {
                        snprintf(fp, sizeof fp, "%s/%s", path, f->d_name);
                        held = !file_unheld(fp);
                    }
                closedir(o);
            }
            if (held)
                continue;
            o = opendir(path);
            if (o) {
                struct dirent *f;
                char fp[1100];
                while ((f = readdir(o)))
                    if (f->d_name[0] != '.') {
                        snprintf(fp, sizeof fp, "%s/%s", path, f->d_name);
                        unlink(fp);
                    }
                closedir(o);
            }
            rmdir(path);
        }
        closedir(d);
    }

    static _Atomic uint32_t n;
    char dir[512], dev[600];
    snprintf(dir, sizeof dir, "%s/d-%d-%u", base, (int)getpid(), atomic_fetch_add(&n, 1));
    if (mkdir(dir, 0700) != 0 && errno != EEXIST)
        return LERR(errno);
    snprintf(dev, sizeof dev, "%s/device", dir);
    int fd = open(dev, O_RDWR | O_CREAT | O_TRUNC | ((lflags & 02000000) ? O_CLOEXEC : 0), 0600);
    if (fd < 0)
        return LERR(errno);
    if (ftruncate(fd, ARENA_BYTES) != 0 || flock(fd, LOCK_SH) != 0) {
        int e = errno;
        close(fd);
        return LERR(e);
    }
    struct nt_hdr hdr;
    memset(&hdr, 0, sizeof hdr);
    hdr.magic = NT_MAGIC_DEV;
    snprintf(hdr.dir, sizeof hdr.dir, "%s", dir);
    if (pwrite(fd, &hdr, sizeof hdr, 0) != (ssize_t)sizeof hdr) {
        int e = errno;
        close(fd);
        return LERR(e);
    }
    return fd;
}

// A slot for a new object, under the device lock: a free one, or one whose
// file nobody holds any more (checked a few at a time as objects are made,
// all of them when none is left).
static int alloc_slot(struct nt_hdr *h)
{
    struct nt_obj *o = objs(h);
    char path[700];
    for (int k = 0; k < 2; k++) {               // a little collection each time
        uint32_t s = h->gc_next++ % NT_OBJS;
        if (o[s].type != T_FREE) {
            obj_path(h, s, path, sizeof path);
            if (file_unheld(path)) {
                unlink(path);
                o[s].type = T_FREE;
            }
        }
    }
    for (uint32_t i = 0; i < NT_OBJS; i++) {
        uint32_t s = (h->obj_next + i) % NT_OBJS;
        if (o[s].type == T_FREE) {
            h->obj_next = s + 1;
            return (int)s;
        }
    }
    for (uint32_t s = 0; s < NT_OBJS; s++) {
        obj_path(h, s, path, sizeof path);
        if (file_unheld(path)) {
            unlink(path);
            o[s].type = T_FREE;
            return (int)s;
        }
    }
    return -1;
}

static long create(struct nt_hdr *h, uint32_t type, uint32_t a, uint32_t b)
{
    nt_lock(h);
    int s = alloc_slot(h);
    if (s < 0) {
        nt_unlock(h);
        return LERR(ENOMEM);
    }
    char path[700];
    obj_path(h, (uint32_t)s, path, sizeof path);
    unlink(path);
    int fd = open(path, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (fd < 0 || flock(fd, LOCK_SH) != 0) {
        int e = errno;
        if (fd >= 0)
            close(fd);
        nt_unlock(h);
        return LERR(e);
    }
    struct nt_obj *o = &objs(h)[s];
    o->gen++;
    struct nt_file f;
    memset(&f, 0, sizeof f);
    f.magic = NT_MAGIC_OBJ;
    f.slot = (uint32_t)s;
    f.gen = o->gen;
    snprintf(f.dev, sizeof f.dev, "%s/device", h->dir);
    if (pwrite(fd, &f, sizeof f, 0) != (ssize_t)sizeof f) {
        int e = errno;
        close(fd);
        unlink(path);
        nt_unlock(h);
        return LERR(e);
    }
    o->type = type;
    o->a = a;
    o->b = b;
    o->ownerdead = 0;
    nt_unlock(h);
    return fd;
}

// --------------------------------------------------- the driver's logic

static bool is_signaled(const struct nt_obj *o, uint32_t owner)
{
    switch (o->type) {
    case T_SEM: return o->a != 0;
    case T_MUTEX: return (!o->a || o->a == owner) && o->b < UINT32_MAX;
    case T_EVENT: return o->b != 0;
    }
    return false;
}

static void wake(struct nt_q *q)
{
    atomic_fetch_add(&q->wake, 1);
    __ulock_wake(UL_COMPARE_AND_WAIT_SHARED | ULF_WAKE_ALL | ULF_NO_ERRNO, (void *)&q->wake, 0);
}

// A queued waiter whose process is gone is dropped instead of handed
// anything.
static bool q_alive(struct nt_q *q)
{
    if (q->pid == (int32_t)getpid() || !pid_dead(q->pid))
        return true;
    q->state = 0;
    return false;
}

static bool entry_live(struct nt_hdr *h, const struct nt_q *q, uint32_t i)
{
    const struct nt_obj *o = &objs(h)[q->slot[i]];
    return o->type != T_FREE && o->gen == q->gen[i];
}

static void take(struct nt_obj *o, struct nt_q *q)
{
    switch (o->type) {
    case T_SEM:
        o->a--;
        break;
    case T_MUTEX:
        if (o->ownerdead)
            q->ownerdead = 1;
        o->ownerdead = 0;
        o->b++;
        o->a = q->owner;
        break;
    case T_EVENT:
        if (!o->a)
            o->b = 0;
        break;
    }
}

static void try_wake_all(struct nt_hdr *h, struct nt_q *q)
{
    struct nt_obj *o = objs(h);
    for (uint32_t i = 0; i < q->count; i++)
        if (!entry_live(h, q, i) || !is_signaled(&o[q->slot[i]], q->owner))
            return;
    int32_t m1 = -1;
    if (!q_alive(q) || !atomic_compare_exchange_strong(&q->signaled, &m1, 0))
        return;
    for (uint32_t i = 0; i < q->count; i++)
        take(&o[q->slot[i]], q);
    wake(q);
}

struct cand { uint64_t seq; uint32_t q, index; };

// The waiters queued on slot s, in queue order: wait-any entries, or
// wait-all waiters' alert entries (any = true); wait-all entries (false).
static int candidates(struct nt_hdr *h, uint32_t s, bool any, struct cand *out, int max)
{
    struct nt_q *qv = qs(h);
    const struct nt_obj *o = &objs(h)[s];
    int n = 0;
    for (uint32_t k = 0; k < h->q_high && k < NT_QS; k++) {
        struct nt_q *q = &qv[k];
        if (q->state != 1 || atomic_load(&q->signaled) != -1)
            continue;
        uint32_t from = 0, to = q->total;
        if (q->all) {
            if (any) from = q->count;            // the alert only
            else to = q->count;
        } else if (!any) {
            continue;
        }
        for (uint32_t i = from; i < to && n < max; i++) {
            if (q->slot[i] != s || q->gen[i] != o->gen)
                continue;
            int j = n++;
            while (j > 0 && (out[j - 1].seq > q->seq ||
                             (out[j - 1].seq == q->seq && out[j - 1].index > i))) {
                out[j] = out[j - 1];
                j--;
            }
            out[j] = (struct cand){ q->seq, k, i };
        }
    }
    return n;
}

static void try_wake_all_obj(struct nt_hdr *h, uint32_t s)
{
    struct cand c[256];
    int n = candidates(h, s, false, c, 256);
    for (int i = 0; i < n; i++)
        try_wake_all(h, &qs(h)[c[i].q]);
}

static void try_wake_any_obj(struct nt_hdr *h, uint32_t s)
{
    struct nt_obj *o = &objs(h)[s];
    struct cand c[256];
    int n = candidates(h, s, true, c, 256);
    for (int i = 0; i < n; i++) {
        struct nt_q *q = &qs(h)[c[i].q];
        if (o->type == T_SEM && !o->a) break;
        if (o->type == T_EVENT && !o->b) break;
        if (o->type == T_MUTEX) {
            if (o->b == UINT32_MAX) break;
            if (o->a && o->a != q->owner) continue;
        }
        int32_t m1 = -1;
        if (!q_alive(q) || !atomic_compare_exchange_strong(&q->signaled, &m1, (int32_t)c[i].index))
            continue;
        take(o, q);
        wake(q);
    }
}

static void signal_obj(struct nt_hdr *h, uint32_t s)
{
    try_wake_all_obj(h, s);
    try_wake_any_obj(h, s);
}

// ------------------------------------------------------------ the ioctls

static void *gptr(uint64_t a)
{
    uint64_t base = lxrt_gbase();
    if (base && a && a < (1ull << 32))          // FEX's low window (gbase.c)
        a += base;
    return (void *)(uintptr_t)a;
}

static long obj_ioctl(struct nt_hdr *h, uint32_t s, uint32_t gen, uint32_t req, void *p)
{
    nt_lock(h);
    struct nt_obj *o = &objs(h)[s];
    long ret = 0;
    if (o->type == T_FREE || o->gen != gen) {
        nt_unlock(h);
        return LERR(EBADF);
    }
    switch (req) {
    case NT_SEM_RELEASE: {
        uint32_t count = *(uint32_t *)p, prev = o->a, sum;
        if (o->type != T_SEM) { ret = LERR(EINVAL); break; }
        if (__builtin_add_overflow(o->a, count, &sum) || sum > o->b) { ret = LERR(EOVERFLOW); break; }
        o->a = sum;
        signal_obj(h, s);
        *(uint32_t *)p = prev;
        break;
    }
    case NT_MUTEX_UNLOCK: {
        struct nt_mutex_args *m = p;
        if (!m->owner || o->type != T_MUTEX) { ret = LERR(EINVAL); break; }
        uint32_t prev = o->b;
        if (o->a != m->owner) { ret = LERR(EPERM); break; }
        if (!--o->b)
            o->a = 0;
        signal_obj(h, s);
        m->count = prev;
        break;
    }
    case NT_MUTEX_KILL: {
        uint32_t owner = *(uint32_t *)p;
        if (!owner || o->type != T_MUTEX) { ret = LERR(EINVAL); break; }
        if (o->a != owner) { ret = LERR(EPERM); break; }
        o->ownerdead = 1;
        o->a = 0;
        o->b = 0;
        signal_obj(h, s);
        break;
    }
    case NT_EVENT_SET:
    case NT_EVENT_PULSE: {
        if (o->type != T_EVENT) { ret = LERR(EINVAL); break; }
        uint32_t prev = o->b;
        o->b = 1;
        signal_obj(h, s);
        if (req == NT_EVENT_PULSE)
            o->b = 0;
        *(uint32_t *)p = prev;
        break;
    }
    case NT_EVENT_RESET: {
        if (o->type != T_EVENT) { ret = LERR(EINVAL); break; }
        uint32_t prev = o->b;
        o->b = 0;
        *(uint32_t *)p = prev;
        break;
    }
    case NT_SEM_READ:
        if (o->type != T_SEM) { ret = LERR(EINVAL); break; }
        *(struct nt_sem_args *)p = (struct nt_sem_args){ o->a, o->b };
        break;
    case NT_MUTEX_READ:
        if (o->type != T_MUTEX) { ret = LERR(EINVAL); break; }
        *(struct nt_mutex_args *)p = (struct nt_mutex_args){ o->a, o->b };
        if (o->ownerdead)
            ret = LERR(EOWNERDEAD);
        break;
    case NT_EVENT_READ:
        if (o->type != T_EVENT) { ret = LERR(EINVAL); break; }
        *(struct nt_event_args *)p = (struct nt_event_args){ o->a, o->b };
        break;
    default:
        ret = LERR(ENOTTY);
    }
    nt_unlock(h);
    return ret;
}

static long nt_wait(struct nt_hdr *h, bool all, struct nt_wait_args *uargs)
{
    struct nt_wait_args args = *uargs;
    if (args.pad || (args.flags & ~NT_WAIT_REALTIME))
        return LERR(EINVAL);
    if (!args.owner)
        return LERR(EINVAL);
    if (args.count > NT_MAX_WAIT)
        return LERR(EINVAL);
    uint32_t total = args.count + (args.alert ? 1 : 0);
    int32_t fds[NT_ENT];
    if (args.count)
        memcpy(fds, gptr(args.objs), args.count * sizeof(int32_t));
    if (args.alert)
        fds[args.count] = (int32_t)args.alert;
    uint32_t slot[NT_ENT], gen[NT_ENT];
    for (uint32_t i = 0; i < total; i++) {
        struct nt_hdr *oh = NULL;
        if (identify(fds[i], &oh, &slot[i], &gen[i]) != 2 || oh != h)
            return LERR(EINVAL);
        if (all && i < args.count)
            for (uint32_t j = 0; j < i; j++)
                if (slot[j] == slot[i])
                    return LERR(EINVAL);
    }

    nt_lock(h);
    struct nt_q *qv = qs(h), *q = NULL;
    uint32_t k;
    for (k = 0; k < NT_QS; k++) {
        if (qv[k].state == 1 && k < h->q_high && pid_dead(qv[k].pid))
            qv[k].state = 0;                    // a waiter whose process died
        if (qv[k].state == 0) {
            q = &qv[k];
            break;
        }
    }
    if (!q) {
        nt_unlock(h);
        return LERR(ENOMEM);
    }
    if (k + 1 > h->q_high)
        h->q_high = k + 1;
    for (uint32_t i = 0; i < total; i++) {
        const struct nt_obj *o = &objs(h)[slot[i]];
        if (o->type == T_FREE || o->gen != gen[i]) {
            nt_unlock(h);
            return LERR(EINVAL);
        }
    }
    q->state = 1;
    atomic_store(&q->signaled, -1);
    q->all = all;
    q->count = args.count;
    q->total = total;
    q->owner = args.owner;
    q->ownerdead = 0;
    q->seq = ++h->seq;
    q->pid = (int32_t)getpid();
    memcpy(q->slot, slot, total * sizeof slot[0]);
    memcpy(q->gen, gen, total * sizeof gen[0]);
    if (all) {
        try_wake_all(h, q);
        if (args.alert && atomic_load(&q->signaled) == -1)
            try_wake_any_obj(h, slot[args.count]);
    } else {
        for (uint32_t i = 0; i < total && atomic_load(&q->signaled) == -1; i++)
            try_wake_any_obj(h, slot[i]);
    }
    nt_unlock(h);

    bool timed_out = false, intr = false;
    int clk = (args.flags & NT_WAIT_REALTIME) ? 0 : 1;
    for (;;) {
        uint32_t w = atomic_load(&q->wake);
        if (atomic_load(&q->signaled) != -1)
            break;
        uint32_t us = 0;
        if (args.timeout != UINT64_MAX) {
            uint64_t now = lxrt_guest_clock_ns(clk);
            if (now >= args.timeout) { timed_out = true; break; }
            uint64_t u = (args.timeout - now) / 1000;
            us = u == 0 ? 1 : (u >= UINT32_MAX ? UINT32_MAX - 1 : (uint32_t)u);
        }
        int r = __ulock_wait(UL_COMPARE_AND_WAIT_SHARED | ULF_NO_ERRNO, (void *)&q->wake, w, us);
        if (r == -EINTR && !lxrt_interrupted_internally()) {
            intr = true;
            break;
        }
    }

    nt_lock(h);
    int32_t sig = atomic_load(&q->signaled);
    bool dead = q->ownerdead;
    q->state = 0;
    nt_unlock(h);
    if (sig != -1) {
        // Even if a signal or the timeout came too, the objects were taken.
        uargs->index = (uint32_t)sig;
        return dead ? LERR(EOWNERDEAD) : 0;
    }
    (void)timed_out;
    return intr ? LERR(EINTR) : LERR(ETIMEDOUT);
}

bool lxrt_ntsync_request(unsigned long lreq)
{
    return ((lreq >> 8) & 0xff) == 'N' && (lreq & 0xff) >= 0x80 && (lreq & 0xff) <= 0x8d;
}

// *ret set and true when fd is a device or an object of this emulation.
bool lxrt_ntsync_ioctl(int fd, unsigned long lreq, uint64_t arg, long *ret)
{
    struct nt_hdr *h = NULL;
    uint32_t slot = 0, gen = 0;
    int kind = identify(fd, &h, &slot, &gen);
    if (!kind)
        return false;
    void *p = gptr(arg);
    uint32_t req = (uint32_t)lreq;
    if (kind == 1) {
        switch (req) {
        case NT_CREATE_SEM: {
            const struct nt_sem_args *a = p;
            *ret = a->count > a->max ? LERR(EINVAL) : create(h, T_SEM, a->count, a->max);
            break;
        }
        case NT_CREATE_MUTEX: {
            const struct nt_mutex_args *a = p;
            *ret = (!a->owner != !a->count) ? LERR(EINVAL) : create(h, T_MUTEX, a->owner, a->count);
            break;
        }
        case NT_CREATE_EVENT: {
            const struct nt_event_args *a = p;
            *ret = create(h, T_EVENT, a->manual, a->signaled);
            break;
        }
        case NT_WAIT_ANY:
        case NT_WAIT_ALL:
            *ret = nt_wait(h, req == NT_WAIT_ALL, p);
            break;
        default:
            *ret = LERR(ENOTTY);
        }
        return true;
    }
    *ret = obj_ioctl(h, slot, gen, req, p);
    return true;
}

// open("/dev/ntsync"), or "dev/ntsync" against a directory fd of the guest's
// root (FEX opens everything relative to its rootfs fd).
bool lxrt_ntsync_path(int dirfd, const char *path)
{
    if (!path || !lxrt_ntsync_enabled())
        return false;
    if (strcmp(path, "/dev/ntsync") == 0)
        return true;
    if (strcmp(path, "dev/ntsync") != 0 || dirfd < 0)
        return false;
    char dp[1024], rp[1024];
    const char *root = getenv("LXRT_ROOT");
    if (fcntl(dirfd, F_GETPATH, dp) != 0)
        return false;
    if (!root || !*root)
        return strcmp(dp, "/") == 0;
    return realpath(root, rp) && strcmp(dp, rp) == 0;
}
