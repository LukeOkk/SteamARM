// offmap.c -- the host pages behind a file mapping at a 4 KiB offset.
//
// With LXRT_GUEST_PAGE=4096 a guest may mmap a file at an offset that is on a
// 4 KiB page but not on a 16 KiB host page; Darwin's mmap refuses such an
// offset. Without MAP_FIXED, dispatch.c maps from the host page below the
// offset and hands the guest the address of the offset it asked for (the
// native arm64 webhelper's shared-memory pool, grown 64 KiB at a time,
// stage22). The host range is then larger than the guest's: a head in the
// first host page, before the guest's first byte, and usually a tail after
// its last. The guest cannot name either.
//
// munmap works in whole host pages (dispatch.c do_munmap): it unmaps the
// pages wholly inside the guest's range and leaves a partly covered page
// mapped, because other guest memory may live in the rest of it. Here the
// rest of the first and last pages is that head and tail, which is nobody's,
// so every map/unmap cycle left up to two 16 KiB pages mapped for the life of
// the process -- MAP_SHARED ones, holding the file or shared-memory object
// and its resident pages after the guest had closed and unlinked it
// (MEASURED, tests/elf/mmap_offset4k.c: 300 cycles of two 64 KiB mappings
// took /proc/self/maps from 91 to 1291 lines; with this file, 84 to 84).
//
// So such mappings are recorded here as two sets:
//   owned  -- host pages this module created and nothing else lives in;
//   pieces -- the guest bytes of those mappings still mapped.
// After a guest munmap, a partly covered host page at either end of the range
// is unmapped when it is owned and no piece is left in it. The guest can also
// unmap a mapping piece by piece; the page goes with its last guest byte.
//
// Anything else placed into an owned page (MAP_FIXED, MAP_FIXED_NOREPLACE,
// mremap, shmat) disowns the pages it touches first: from then on they hold
// memory this module does not know about, and they are never unmapped here.
// A failure to record (no memory) errs the same way: a page not recorded
// stays mapped, as before; a page is never unmapped on a guess.
#include "lxrt.h"
#include "offmap.h"

#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

bool lxrt_trace_on(void);

// Sorted, disjoint half-open ranges; touching ones are merged.
struct rset { struct lxrt_range *r; int n, cap; };

static struct rset g_owned, g_pieces;
static _Atomic int g_live;          // g_owned.n + g_pieces.n, read without the lock
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(offmap_g_lock, g_lock)

// A guest signal handler runs nested in the host handler and may call munmap:
// the lock is held with the asynchronous signals blocked (as wxsplit.c does).
static void lock_nosig(sigset_t *old)
{
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGBUS);
    sigdelset(&all, SIGSEGV);
    sigdelset(&all, SIGILL);
    sigdelset(&all, SIGTRAP);
    pthread_sigmask(SIG_BLOCK, &all, old);
    pthread_mutex_lock(&g_lock);
}

static void unlock_nosig(const sigset_t *old)
{
    atomic_store(&g_live, g_owned.n + g_pieces.n);
    pthread_mutex_unlock(&g_lock);
    pthread_sigmask(SIG_SETMASK, old, NULL);
}

// First entry whose end is above a.
static int lower(const struct rset *t, uint64_t a)
{
    int lo = 0, hi = t->n;
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (t->r[mid].end <= a) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static bool reserve(struct rset *t, int extra)
{
    if (t->n + extra <= t->cap)
        return true;
    int nc = t->cap ? t->cap * 2 : 64;
    while (nc < t->n + extra)
        nc *= 2;
    struct lxrt_range *p = realloc(t->r, (size_t)nc * sizeof *p);
    if (!p)
        return false;
    t->r = p;
    t->cap = nc;
    return true;
}

static bool rs_intersects(const struct rset *t, uint64_t s, uint64_t e)
{
    int i = lower(t, s);
    return i < t->n && t->r[i].start < e;
}

static bool rs_add(struct rset *t, uint64_t s, uint64_t e)
{
    int i = lower(t, s);
    if (i > 0 && t->r[i - 1].end == s)
        i--;
    int j = i;
    while (j < t->n && t->r[j].start <= e) {
        if (t->r[j].start < s) s = t->r[j].start;
        if (t->r[j].end > e) e = t->r[j].end;
        j++;
    }
    if (j > i) {
        t->r[i].start = s;
        t->r[i].end = e;
        memmove(&t->r[i + 1], &t->r[j], (size_t)(t->n - j) * sizeof *t->r);
        t->n -= j - i - 1;
        return true;
    }
    if (!reserve(t, 1))
        return false;
    memmove(&t->r[i + 1], &t->r[i], (size_t)(t->n - i) * sizeof *t->r);
    t->r[i].start = s;
    t->r[i].end = e;
    t->n++;
    return true;
}

// Remove [s, e). An entry that has to split in two when the table cannot
// grow is removed whole if drop_whole, kept whole otherwise: callers pick
// the answer that can only leave a page mapped, never unmap a live one.
static void rs_remove(struct rset *t, uint64_t s, uint64_t e, bool drop_whole)
{
    int i = lower(t, s);
    while (i < t->n && t->r[i].start < e) {
        struct lxrt_range *r = &t->r[i];
        if (r->start < s && r->end > e) {
            if (!reserve(t, 1)) {
                if (drop_whole) {
                    memmove(&t->r[i], &t->r[i + 1], (size_t)(t->n - i - 1) * sizeof *t->r);
                    t->n--;
                }
                return;
            }
            r = &t->r[i];                       // reserve may have moved the table
            memmove(&t->r[i + 2], &t->r[i + 1], (size_t)(t->n - i - 1) * sizeof *t->r);
            t->r[i + 1].start = e;
            t->r[i + 1].end = r->end;
            r->end = s;
            t->n++;
            return;
        }
        if (r->start < s) {
            r->end = s;
            i++;
        } else if (r->end > e) {
            r->start = e;
            return;
        } else {
            memmove(&t->r[i], &t->r[i + 1], (size_t)(t->n - i - 1) * sizeof *t->r);
            t->n--;
        }
    }
}

void lxrt_offmap_note(uint64_t hstart, uint64_t hend, uint64_t gstart, uint64_t glen)
{
    if (hstart >= hend || !glen)
        return;
    sigset_t old;
    lock_nosig(&old);
    // Pieces first: an owned page without its pieces would look dead.
    if (rs_add(&g_pieces, gstart, gstart + glen) && !rs_add(&g_owned, hstart, hend))
        rs_remove(&g_pieces, gstart, gstart + glen, false);
    unlock_nosig(&old);
}

int lxrt_offmap_unmapped(uint64_t addr, uint64_t len, uint64_t dead[2])
{
    if (!atomic_load(&g_live) || !len)
        return 0;
    uint64_t end = addr + len;
    uint64_t hs = LXRT_ALIGN_UP(addr, LXRT_HOST_PAGE);
    uint64_t he = LXRT_ALIGN_DOWN(end, LXRT_HOST_PAGE);
    // The partly covered host pages at either end (one when the range lies
    // inside a single host page).
    uint64_t cand[2];
    int nc = 0;
    if (addr % LXRT_HOST_PAGE)
        cand[nc++] = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
    if (end % LXRT_HOST_PAGE && (!nc || cand[0] != LXRT_ALIGN_DOWN(end, LXRT_HOST_PAGE)))
        cand[nc++] = LXRT_ALIGN_DOWN(end, LXRT_HOST_PAGE);

    sigset_t old;
    lock_nosig(&old);
    rs_remove(&g_pieces, addr, end, false);
    if (hs < he)
        rs_remove(&g_owned, hs, he, true);      // dispatch.c unmapped these
    int n = 0;
    for (int k = 0; k < nc; k++) {
        uint64_t p = cand[k];
        if (!rs_intersects(&g_owned, p, p + LXRT_HOST_PAGE) ||
            rs_intersects(&g_pieces, p, p + LXRT_HOST_PAGE))
            continue;
        if (munmap((void *)(uintptr_t)p, LXRT_HOST_PAGE) != 0)
            continue;
        rs_remove(&g_owned, p, p + LXRT_HOST_PAGE, true);
        dead[n++] = p;
    }
    unlock_nosig(&old);
    if (n && lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] munmap 0x%llx+0x%llx: %d host page(s) of a 4 KiB-offset "
                "file mapping freed with their last guest page (0x%llx%s)\n",
                (unsigned long long)addr, (unsigned long long)len, n,
                (unsigned long long)dead[0], n > 1 ? " and one more" : "");
    return n;
}

void lxrt_offmap_disown(uint64_t addr, uint64_t len)
{
    if (!atomic_load(&g_live) || !len)
        return;
    sigset_t old;
    lock_nosig(&old);
    rs_remove(&g_pieces, addr, addr + len, false);
    rs_remove(&g_owned, LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE),
              LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE), true);
    unlock_nosig(&old);
}
