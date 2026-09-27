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
void lxrt_memlog_dump(uint64_t fault_addr, const char *why)
{
    uint64_t lo = LXRT_ALIGN_DOWN(fault_addr, LXRT_HOST_PAGE);
    uint64_t hi = lo + LXRT_HOST_PAGE;
    uint64_t end = atomic_load_explicit(&g_next, memory_order_relaxed);
    uint64_t start = end > MEMLOG_N ? end - MEMLOG_N : 0;
    FILE *f = lxrt_trace_stream();
    fprintf(f, "[lxrt] memlog (pid %d): %s at 0x%llx; ops touching host page 0x%llx, oldest first:\n",
            (int)getpid(), why, (unsigned long long)fault_addr, (unsigned long long)lo);
    int shown = 0;
    for (uint64_t s = start; s < end; s++) {
        const struct memlog_ent *e = &g_ring[s % MEMLOG_N];
        if (e->seq != s + 1 || e->addr >= hi || e->addr + e->len <= lo)
            continue;
        fprintf(f, "[lxrt]   #%llu tid %d %c 0x%llx+0x%llx a=0x%lx b=0x%lx -> %ld (lr 0x%llx)\n",
                (unsigned long long)e->seq, e->tid, e->op, (unsigned long long)e->addr,
                (unsigned long long)e->len, e->a, e->b, e->ret, (unsigned long long)e->lr);
        shown++;
    }
    fprintf(f, "[lxrt] memlog: %d of the last %llu ops\n", shown,
            (unsigned long long)(end - start));
    fflush(f);
}

// File-backed mappings, kept only under LXRT_GUEST_FAULTS: a guest fault
// report can then name the file and offset of a guest code address (the
// runtime may have placed the content as a private copy, which the host
// region no longer names). Newest entry wins; never pruned -- a debugging aid.
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct fmap_ent { uint64_t addr, len, off; char *path; };
static struct fmap_ent *g_fmaps;
static size_t g_nfmaps, g_capfmaps;
static pthread_mutex_t g_fmap_lock = PTHREAD_MUTEX_INITIALIZER;

void lxrt_memlog_file(uint64_t addr, uint64_t len, uint64_t off, int fd)
{
    static int on = -1;
    if (on < 0) on = getenv("LXRT_GUEST_FAULTS") ? 1 : 0;
    if (!on || fd < 0 || !len)
        return;
    char p[PATH_MAX];
    if (fcntl(fd, F_GETPATH, p) != 0)
        return;
    pthread_mutex_lock(&g_fmap_lock);
    if (g_nfmaps == g_capfmaps) {
        size_t n = g_capfmaps ? g_capfmaps * 2 : 1024;
        struct fmap_ent *m = realloc(g_fmaps, n * sizeof *m);
        if (!m) { pthread_mutex_unlock(&g_fmap_lock); return; }
        g_fmaps = m; g_capfmaps = n;
    }
    g_fmaps[g_nfmaps++] = (struct fmap_ent){ addr, len, off, strdup(p) };
    pthread_mutex_unlock(&g_fmap_lock);
}

bool lxrt_memlog_file_lookup(uint64_t addr, char *path, size_t n, uint64_t *off)
{
    bool found = false;
    if (pthread_mutex_trylock(&g_fmap_lock) != 0)
        return false;
    for (size_t i = g_nfmaps; i-- > 0;) {
        struct fmap_ent *e = &g_fmaps[i];
        if (addr >= e->addr && addr < e->addr + e->len) {
            snprintf(path, n, "%s", e->path ? e->path : "?");
            *off = e->off + (addr - e->addr);
            found = true;
            break;
        }
    }
    pthread_mutex_unlock(&g_fmap_lock);
    return found;
}
