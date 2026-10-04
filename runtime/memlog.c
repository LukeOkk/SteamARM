// A flight recorder for address-space changes. Every mmap, munmap, mprotect,
// madvise, mremap, shmat and shmdt the runtime serves lands in a fixed ring;
// when a thread keeps faulting on the same address (a guest that cannot make
// progress), the entries touching that host page are printed once.
//
// Why it exists: under a guest base, a page that should be read/write turned
// up PROT_NONE in the middle of the Steam client's run and three threads
// retried the same fault forever. A full syscall trace of that run is ~10 GB;
// the question "who last changed this page" needs a few hundred entries.
//
// Lock-free on the write side (one atomic increment); the dump is best effort
// and may show an entry that is being overwritten at that moment.
#include "lxrt.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <unistd.h>

#define MEMLOG_N 16384u

struct memlog_ent {
    uint64_t seq;
    uint64_t addr, len;
    long a, b, ret;
    uint64_t lr;
    int tid;
    char op;
};

uint64_t lxrt_main_image_base, lxrt_main_image_span;
uint64_t lxrt_start_stack;

static struct memlog_ent g_ring[MEMLOG_N];
static _Atomic uint64_t g_next;

uint64_t lxrt_last_guest_lr(void);

void lxrt_memlog(char op, uint64_t addr, uint64_t len, long a, long b, long ret)
{
    uint64_t s = atomic_fetch_add_explicit(&g_next, 1, memory_order_relaxed);
    struct memlog_ent *e = &g_ring[s % MEMLOG_N];
    e->seq = s + 1;
    e->addr = addr;
    e->len = len;
    e->a = a;
    e->b = b;
    e->ret = ret;
    e->lr = lxrt_last_guest_lr();
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    e->tid = (int)tid;
    e->op = op;
}

// op letters: m mmap(prot, flags)  u munmap  p mprotect(prot)  a madvise(advice)
//             r mremap(new_len, flags)  s shmat(shmid, flags)  d shmdt
// To a file descriptor (the fault report also keeps a copy in a file).
void lxrt_memlog_dump_fd(int fd, uint64_t fault_addr, const char *why)
{
    uint64_t lo = LXRT_ALIGN_DOWN(fault_addr, LXRT_HOST_PAGE);
    uint64_t hi = lo + LXRT_HOST_PAGE;
    uint64_t end = atomic_load_explicit(&g_next, memory_order_relaxed);
    uint64_t start = end > MEMLOG_N ? end - MEMLOG_N : 0;
    dprintf(fd, "[lxrt] memlog (pid %d): %s at 0x%llx; ops touching host page 0x%llx, oldest first:\n",
            (int)getpid(), why, (unsigned long long)fault_addr, (unsigned long long)lo);
    int shown = 0;
    for (uint64_t s = start; s < end; s++) {
        const struct memlog_ent *e = &g_ring[s % MEMLOG_N];
        if (e->seq != s + 1 || e->addr >= hi || e->addr + e->len <= lo)
            continue;
        dprintf(fd, "[lxrt]   #%llu tid %d %c 0x%llx+0x%llx a=0x%lx b=0x%lx -> %ld (lr 0x%llx)\n",
                (unsigned long long)e->seq, e->tid, e->op, (unsigned long long)e->addr,
                (unsigned long long)e->len, e->a, e->b, e->ret, (unsigned long long)e->lr);
        shown++;
    }
    dprintf(fd, "[lxrt] memlog: %d of the last %llu ops\n", shown,
            (unsigned long long)(end - start));
}

void lxrt_memlog_dump(uint64_t fault_addr, const char *why)
{
    FILE *f = lxrt_trace_stream();
    fflush(f);
    lxrt_memlog_dump_fd(fileno(f), fault_addr, why);
}

// File-backed mappings, kept only under LXRT_GUEST_FAULTS: a guest fault
// report can then name the file and offset of a guest code address (the
// runtime may have placed the content as a private copy, which the host
// region no longer names). Newest entry wins. An entry goes when the guest
// unmaps all of it (lxrt_memlog_file_forget): scripts/settings-env.py turns
// LXRT_GUEST_FAULTS on by default, and the Steam client's controller thread
// maps ld.so.cache and libusb (twice) in each of its dlopen/dlclose cycles,
// about four a second -- a path copy each, never freed (MEASURED on the live
// client).
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>

struct fmap_ent { uint64_t addr, len, off; char *path; };
static struct fmap_ent *g_fmaps;
static size_t g_nfmaps, g_capfmaps;
static pthread_mutex_t g_fmap_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(memlog_fmap_lock, g_fmap_lock)

// munmap takes this lock (lxrt_memlog_file_forget), and a guest signal
// handler runs nested in the host handler and may call munmap: every holder
// blocks the asynchronous signals (as offmap.c and wxsplit.c do).
static void block_async(sigset_t *old)
{
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGBUS);
    sigdelset(&all, SIGSEGV);
    sigdelset(&all, SIGILL);
    sigdelset(&all, SIGTRAP);
    pthread_sigmask(SIG_BLOCK, &all, old);
}

static void lock_nosig(sigset_t *old)
{
    block_async(old);
    pthread_mutex_lock(&g_fmap_lock);
}

static void unlock_nosig(const sigset_t *old)
{
    pthread_mutex_unlock(&g_fmap_lock);
    pthread_sigmask(SIG_SETMASK, old, NULL);
}

static bool fmaps_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("LXRT_GUEST_FAULTS") ? 1 : 0;
    return on;
}

// Drop the entries wholly inside [lo, hi), keeping the order of the rest
// (the lookup prefers the newest). Caller holds the lock.
static void drop_inside(uint64_t lo, uint64_t hi)
{
    size_t j = 0;
    for (size_t i = 0; i < g_nfmaps; i++) {
        if (g_fmaps[i].addr >= lo && g_fmaps[i].addr + g_fmaps[i].len <= hi)
            free(g_fmaps[i].path);
        else
            g_fmaps[j++] = g_fmaps[i];
    }
    g_nfmaps = j;
}

void lxrt_memlog_file(uint64_t addr, uint64_t len, uint64_t off, int fd)
{
    if (!fmaps_on() || fd < 0 || !len)
        return;
    char p[PATH_MAX];
    if (fcntl(fd, F_GETPATH, p) != 0)
        return;
    char *copy = strdup(p);
    sigset_t old;
    lock_nosig(&old);
    drop_inside(addr, addr + LXRT_ALIGN_UP(len, 4096));   // mapped over: replaced
    if (g_nfmaps == g_capfmaps) {
        size_t n = g_capfmaps ? g_capfmaps * 2 : 1024;
        struct fmap_ent *m = realloc(g_fmaps, n * sizeof *m);
        if (!m) { unlock_nosig(&old); free(copy); return; }
        g_fmaps = m; g_capfmaps = n;
    }
    g_fmaps[g_nfmaps++] = (struct fmap_ent){ addr, len, off, copy };
    unlock_nosig(&old);
}

// The guest unmapped [addr, addr+len) (dispatch.c passes the length rounded
// up to 4 KiB, as Linux does: glibc's dlclose unmaps l_map_end - l_map_start,
// 0x30188 for libusb, and the RW segment it mapped ends at the next 4 KiB).
// An entry only partly unmapped stays.
void lxrt_memlog_file_forget(uint64_t addr, uint64_t len)
{
    if (!fmaps_on() || !len)
        return;
    sigset_t old;
    lock_nosig(&old);
    drop_inside(addr, addr + len);
    unlock_nosig(&old);
}

// From the fault reports, which may run inside a fault handler: never waits.
bool lxrt_memlog_file_lookup(uint64_t addr, char *path, size_t n, uint64_t *off)
{
    bool found = false;
    sigset_t old;
    block_async(&old);
    if (pthread_mutex_trylock(&g_fmap_lock) != 0) {
        pthread_sigmask(SIG_SETMASK, &old, NULL);
        return false;
    }
    for (size_t i = g_nfmaps; i-- > 0;) {
        struct fmap_ent *e = &g_fmaps[i];
        if (addr >= e->addr && addr < e->addr + e->len) {
            snprintf(path, n, "%s", e->path ? e->path : "?");
            *off = e->off + (addr - e->addr);
            found = true;
            break;
        }
    }
    unlock_nosig(&old);
    return found;
}

// One line in /tmp/lxrt-faults.log (and the trace stream): something the
// runtime caught and repaired that would otherwise have been a crash, from a
// process whose stderr may go nowhere. The guest program's path, never its
// arguments.
#include <crt_externs.h>
#include <fcntl.h>
#include <stdarg.h>
void lxrt_fault_note(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(lxrt_trace_stream(), "[lxrt] %s\n", line);
    int ff = open("/tmp/lxrt-faults.log", O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (ff >= 0) {
        char **av = *_NSGetArgv();
        dprintf(ff, "[lxrt] pid %d %s: %s\n", (int)getpid(), *_NSGetArgc() > 1 && av[1] ? av[1] : "?", line);
        close(ff);
    }
}
