// offmap.c -- the bytes of host pages that no guest mapping covers.
//
// The guest maps and unmaps in 4 KiB pages, the host in 16 KiB ones, so a
// host page can hold 4 KiB slots that belong to no guest mapping:
//   - the head and tail of a mapping the runtime placed itself. A file
//     mapping at a 4 KiB offset that is not on a host page is mapped from the
//     host page below the offset (dispatch.c do_mmap; the native arm64
//     webhelper's shared-memory pool, grown 64 KiB at a time, stage22), and
//     any mapping whose length is not a multiple of 16 KiB ends partway into
//     its last host page: ld.so.cache, glibc's PROT_NONE reservation of a
//     library's span, glibc malloc's large blocks with 4 KiB guest pages;
//   - what the guest unmapped of a host page that stays mapped because other
//     guest pages are still in it (dispatch.c do_munmap_inner unmaps only the
//     host pages wholly inside the range).
// These slots are recorded here, as one set ("spare"). After a guest munmap,
// a partly covered host page at either end of the range whose every slot is
// spare holds nothing anyone can name, and it is unmapped too.
//
// Without that, every map/unmap cycle left up to two 16 KiB pages mapped for
// the life of the process -- MAP_SHARED ones holding the file or
// shared-memory object and its resident pages after the guest had closed and
// unlinked it (MEASURED, tests/elf/mmap_offset4k.c: 300 cycles of two 64 KiB
// mappings took /proc/self/maps from 91 to 1291 lines; with this file, 84 to
// 84). The Steam client's controller thread dlopens and dlcloses libusb
// about four times a second, and each cycle left three such pages: the tail
// of ld.so.cache, the tail of glibc's reservation of the library's span, and
// the RW segment's last page, which also held the end of the code mapping's
// last host page, past the guest's last byte (tests/elf/dlcycle.c).
//
// The set may only ever under-claim. Anything placed into a range (MAP_FIXED,
// MAP_FIXED_NOREPLACE, mremap, shmat, a binder mapping) takes its slots out
// of the set first (lxrt_offmap_disown, lxrt_offmap_disown_pages). A failure
// to record (no memory) keeps a page mapped, as before; a page is never
// unmapped on a guess, and never when the kernel says it is something other
// than a plain guest mapping (a tagged region: the host's heap, a stack), a
// trampoline pool (rewrite.c) or a JIT region (jit.c).
#include "lxrt.h"
#include "offmap.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

bool lxrt_trace_on(void);

#define SLOT 4096ull

// Sorted, disjoint half-open ranges; touching ones are merged.
struct rset { struct lxrt_range *r; int n, cap; };

static struct rset g_spare;
static _Atomic int g_live;          // g_spare.n, read without the lock
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
    atomic_store(&g_live, g_spare.n);
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

// Is all of [s, e) in the set? Touching ranges are merged, so one entry.
static bool rs_covers(const struct rset *t, uint64_t s, uint64_t e)
{
    int i = lower(t, s);
    return i < t->n && t->r[i].start <= s && t->r[i].end >= e;
}

static bool rs_add(struct rset *t, uint64_t s, uint64_t e)
{
    if (s >= e)
        return true;
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
// grow is removed whole: claiming fewer spare slots can only keep a page
// mapped, never unmap a live one.
static void rs_remove(struct rset *t, uint64_t s, uint64_t e)
{
    int i = lower(t, s);
    while (i < t->n && t->r[i].start < e) {
        struct lxrt_range *r = &t->r[i];
        if (r->start < s && r->end > e) {
            if (!reserve(t, 1)) {
                memmove(&t->r[i], &t->r[i + 1], (size_t)(t->n - i - 1) * sizeof *t->r);
                t->n--;
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

// Is the host page at p mapped, and an ordinary guest mapping as far as the
// kernel can tell (no tag: guest mmaps carry none; the host's heap, stacks
// and frameworks do)?
static bool plain_page(uint64_t p)
{
    mach_vm_address_t ra = p;
    mach_vm_size_t rs = 0;
    vm_region_extended_info_data_t ri;
    mach_msg_type_number_t rc = VM_REGION_EXTENDED_INFO_COUNT;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_EXTENDED_INFO,
                       (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS)
        return false;
    return ra <= p && ra + rs >= p + LXRT_HOST_PAGE && ri.user_tag == 0;
}

void lxrt_offmap_note(uint64_t hstart, uint64_t hend, uint64_t gstart, uint64_t glen)
{
    if (hstart >= hend || !glen)
        return;
    uint64_t gs = LXRT_ALIGN_DOWN(gstart, SLOT);
    uint64_t ge = LXRT_ALIGN_UP(gstart + glen, SLOT);
    if (gs <= hstart && ge >= hend)
        return;                                 // the guest's to the last byte
    sigset_t old;
    lock_nosig(&old);
    if (hstart < gs)
        rs_add(&g_spare, hstart, gs);
    if (ge < hend)
        rs_add(&g_spare, ge, hend);
    unlock_nosig(&old);
}

int lxrt_offmap_unmapped(uint64_t addr, uint64_t len, uint64_t dead[2])
{
    if (!len)
        return 0;
    uint64_t end = addr + len;
    uint64_t hs = LXRT_ALIGN_UP(addr, LXRT_HOST_PAGE);
    uint64_t he = LXRT_ALIGN_DOWN(end, LXRT_HOST_PAGE);
    // The partly covered host pages at either end (one when the range lies
    // inside a single host page), and the part of each the guest unmapped.
    uint64_t cand[2], lo[2], hi[2];
    int nc = 0;
    if (addr % LXRT_HOST_PAGE) {
        cand[nc] = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
        lo[nc] = addr;
        hi[nc] = end < hs ? end : hs;
        nc++;
    }
    if (end % LXRT_HOST_PAGE && (!nc || cand[0] != LXRT_ALIGN_DOWN(end, LXRT_HOST_PAGE))) {
        cand[nc] = LXRT_ALIGN_DOWN(end, LXRT_HOST_PAGE);
        lo[nc] = he > addr ? he : addr;
        hi[nc] = end;
        nc++;
    }
    if (!nc)
        return 0;                               // the whole pages went first (dispatch.c)

    sigset_t old;
    lock_nosig(&old);
    if (hs < he)
        rs_remove(&g_spare, hs, he);            // dispatch.c unmapped these (and said so before)
    int n = 0;
    for (int k = 0; k < nc; k++) {
        uint64_t p = cand[k];
        if (!plain_page(p))
            continue;                           // a hole, or not the guest's
        rs_add(&g_spare, lo[k], hi[k]);         // no guest mapping is there now
        if (!rs_covers(&g_spare, p, p + LXRT_HOST_PAGE) ||
            lxrt_pool_overlaps(p, p + LXRT_HOST_PAGE) || lxrt_jit_contains(p))
            continue;
        if (munmap((void *)(uintptr_t)p, LXRT_HOST_PAGE) != 0)
            continue;
        rs_remove(&g_spare, p, p + LXRT_HOST_PAGE);
        dead[n++] = p;
    }
    unlock_nosig(&old);
    if (n && lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] munmap 0x%llx+0x%llx: %d host page(s) with no guest "
                "page left freed (0x%llx%s)\n",
                (unsigned long long)addr, (unsigned long long)len, n,
                (unsigned long long)dead[0], n > 1 ? " and one more" : "");
    return n;
}

static void drop(uint64_t s, uint64_t e)
{
    if (!atomic_load(&g_live) || s >= e)
        return;
    sigset_t old;
    lock_nosig(&old);
    rs_remove(&g_spare, s, e);
    unlock_nosig(&old);
}

void lxrt_offmap_disown(uint64_t addr, uint64_t len)
{
    if (len)
        drop(LXRT_ALIGN_DOWN(addr, SLOT), LXRT_ALIGN_UP(addr + len, SLOT));
}

void lxrt_offmap_disown_pages(uint64_t addr, uint64_t len)
{
    if (len)
        drop(LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE), LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE));
}
