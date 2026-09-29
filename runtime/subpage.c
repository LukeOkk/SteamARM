// 4 KiB guest mappings on 16 KiB host pages.
//
// This is the wall PERFORMANCE_BASELINE §5 predicted and stage2-elf-survey
// located: aarch64 Linux binaries are linked with p_align 0x10000, a clean
// multiple of Darwin's page, so Stage 2 never met it -- but x86-64 and i386
// Linux binaries use p_align 0x1000, and Steam's client is x86. FEX maps the
// guest's segments at 4 KiB granularity and Darwin's mmap rejects a MAP_FIXED
// address that is not 16 KiB aligned:
//
//   mmap(0x108259000, 0x1000, PROT_READ|PROT_EXEC) -> EINVAL
//
// The host page cannot be subdivided, so the runtime subdivides the bookkeeping
// instead: the enclosing host pages are backed by anonymous memory, file
// content is READ into place rather than mapped, and each host page carries the
// union of the protections of the guest pages inside it.
//
// Two costs, both stated rather than hidden:
//   * a file mapped this way is private and duplicated, not shared with the
//     page cache;
//   * a host page holding an executable guest page and a writable one is both,
//     which is weaker than the guest asked for. Executable is never granted to
//     a host page unless a guest page inside it asked for it, but write can
//     leak across the 4 KiB boundaries inside one 16 KiB page. That is inherent
//     to the hardware and is what a 16 KiB kernel would do too.
//
// Such a page is read-write or read-execute at any moment, flipped by the
// fault handler below. A store executed from it into it cannot complete
// either way, so the handler performs that store itself (storemu.c); and code
// stored into an executable 4 KiB guest page is rewritten before it runs.

#include "lxrt.h"
#include "storemu.h"

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/ucontext.h>
#include <libproc.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))
#define GUEST_PAGE 4096

// Every sub-page guest mapping the runtime is tracking.
struct sub {
    uint64_t start, end;
    int prot;
};

// Grown on demand. A fixed 4096-entry table overflowed under the Steam
// client (FEX hands every freed 4 KiB page back to its reservation with its
// own mmap) and dropped records silently: an mprotect to read/write was then
// missing from the union, the host page stayed PROT_NONE and three threads
// retried the same fault forever (runtime/memlog.c found it).
static struct sub *g_subs;
static int g_nsubs, g_cap;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(subpage_g_lock, g_lock)
// Set while this thread holds g_lock inside the mapping paths: a fault raised
// by this module's own stores must be reported, never waited on.
static _Thread_local bool g_in_subpage;

// Room for `extra` more records. Caller holds g_lock.
static bool subs_reserve(int extra)
{
    if (g_nsubs + extra <= g_cap)
        return true;
    int nc = g_cap ? g_cap * 2 : 4096;
    while (nc < g_nsubs + extra)
        nc *= 2;
    struct sub *p = realloc(g_subs, (size_t)nc * sizeof *g_subs);
    if (!p) {
        static bool said;
        if (!said) {
            said = true;
            fprintf(lxrt_trace_stream(), "[lxrt] subpage: cannot grow the table past %d entries; "
                            "sub-page protections will be wrong\n", g_cap);
        }
        return false;
    }
    g_subs = p;
    g_cap = nc;
    return true;
}

// Only the ADDRESS and the file OFFSET matter. A length that is not a multiple
// of the host page is ordinary -- the kernel rounds it up -- and treating it as
// a sub-page case routed every normal mapping through this bookkeeping and
// broke seven tests.
bool lxrt_subpage_needed(uint64_t addr, uint64_t len, uint64_t off)
{
    // A partial LENGTH matters as much as an unaligned address: Darwin rounds
    // a MAP_FIXED mmap of 4 KiB up to the whole 16 KiB page and zero-fills the
    // neighbours (measured, benchmarks/stage5-subpage.txt), and FEX's own
    // allocator maps 4 KiB at a time.
    (void)len;   // partial lengths are decided by the caller, which knows prot
    return (addr % LXRT_HOST_PAGE) != 0 || (off % LXRT_HOST_PAGE) != 0;
}

// Drop the tracked guest ranges inside [addr, addr+len): the guest unmapped
// them. The host pages stay mapped when a partial page still has neighbours;
// see do_munmap in dispatch.c.
void lxrt_subpage_forget(uint64_t addr, uint64_t len)
{
    lxrt_shmirror_forget(addr, len);
    pthread_mutex_lock(&g_lock);
    uint64_t end = addr + len;
    for (int i = 0; i < g_nsubs; i++) {
        if (g_subs[i].end <= addr || g_subs[i].start >= end)
            continue;
        if (g_subs[i].start < addr && g_subs[i].end > end) {
            if (subs_reserve(1)) {
                g_subs[g_nsubs].start = end;
                g_subs[g_nsubs].end = g_subs[i].end;
                g_subs[g_nsubs].prot = g_subs[i].prot;
                g_nsubs++;
            }
            g_subs[i].end = addr;
        } else if (g_subs[i].start < addr) {
            g_subs[i].end = addr;
        } else if (g_subs[i].end > end) {
            g_subs[i].start = end;
        } else {
            g_subs[i] = g_subs[--g_nsubs];
            i--;
        }
    }
    pthread_mutex_unlock(&g_lock);
}

static void record(uint64_t start, uint64_t end, int prot)
{
    // Trim anything the new mapping covers, then append. Overlapping guest
    // mappings replace rather than merge, which is what mmap means.
    for (int i = 0; i < g_nsubs; i++) {
        if (g_subs[i].end <= start || g_subs[i].start >= end)
            continue;
        if (g_subs[i].start < start && g_subs[i].end > end) {
            // Split: keep the head, append the tail.
            if (subs_reserve(1)) {
                g_subs[g_nsubs].start = end;
                g_subs[g_nsubs].end = g_subs[i].end;
                g_subs[g_nsubs].prot = g_subs[i].prot;
                g_nsubs++;
            }
            g_subs[i].end = start;
        } else if (g_subs[i].start < start) {
            g_subs[i].end = start;
        } else if (g_subs[i].end > end) {
            g_subs[i].start = end;
        } else {
            g_subs[i] = g_subs[--g_nsubs];
            i--;
        }
    }
    // Coalesce with neighbours of the same protection: FEX re-reserves freed
    // memory one 4 KiB page at a time, and those runs would otherwise cost an
    // entry each.
    for (int i = 0; i < g_nsubs; i++) {
        if (g_subs[i].prot != prot)
            continue;
        if (g_subs[i].end == start) {
            start = g_subs[i].start;
        } else if (g_subs[i].start == end) {
            end = g_subs[i].end;
        } else {
            continue;
        }
        g_subs[i] = g_subs[--g_nsubs];
        i = -1;                     // the grown range may now touch another
    }
    if (subs_reserve(1)) {
        g_subs[g_nsubs].start = start;
        g_subs[g_nsubs].end = end;
        g_subs[g_nsubs].prot = prot;
        g_nsubs++;
    }
}

// Which access the guest most recently asked for on a host page that cannot
// have write and execute at once. Darwin refuses W+X on any ordinary page, so
// when the union of the guest mappings inside a host page wants both, one of
// them has to be withheld -- and the fault handler flips between them.
//
// Silently dropping WRITE (the first implementation) made FEX's
// self-modifying-code handler spin forever: it took the protection fault,
// called mprotect for write, got read-execute back, and faulted again. Thirteen
// million times in thirty seconds.
static int union_prot(uint64_t hpage);
static int apply_prot(uint64_t hpage, int prefer);
static void adopt_untracked(uint64_t hpage);
static bool page_tracked(uint64_t hpage);

static bool needs_wx_split(uint64_t hpage)
{
    int u = union_prot(hpage);
    return (u & PROT_WRITE) && (u & PROT_EXEC);
}

// The protection a host page must carry: the union over every guest mapping
// inside it.
static int union_prot(uint64_t hpage)
{
    int p = 0;
    for (int i = 0; i < g_nsubs; i++)
        if (g_subs[i].start < hpage + LXRT_HOST_PAGE && g_subs[i].end > hpage)
            p |= g_subs[i].prot;
    return p;
}

// Ensure [hstart, hend) is backed and writable so content can be placed.
// Is this host page a file mapping? Asked of the region that contains it
// (external pager): proc_regionfilename() answers for the NEXT file-backed
// region when this one is anonymous, so it cannot be used on its own.
static bool page_is_file_backed(uint64_t p)
{
    mach_vm_address_t ra = p;
    mach_vm_size_t rs = 0;
    vm_region_extended_info_data_t ri;
    mach_msg_type_number_t rc = VM_REGION_EXTENDED_INFO_COUNT;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_EXTENDED_INFO,
                       (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS || ra > p)
        return false;
    return ri.external_pager != 0;
}

static bool back_range(uint64_t hstart, uint64_t hend)
{
    for (uint64_t p = hstart; p < hend; p += LXRT_HOST_PAGE) {
        mach_vm_address_t at = p;
        kern_return_t kr = mach_vm_allocate(mach_task_self(), &at,
                                            LXRT_HOST_PAGE, VM_FLAGS_FIXED);
        if (kr != KERN_SUCCESS && kr != KERN_NO_SPACE)
            return false;           // genuinely unusable address
        // KERN_NO_SPACE means it is already mapped, which is fine and normal:
        // a second 4 KiB mapping inside a host page the first one created.
        //
        // Unless what is already there is a FILE mapping. The x86-64 ld.so maps
        // libc's data segment from the file with an ordinary, 16 KiB-aligned
        // mmap, then maps the bss anonymously 4 KiB later -- inside the same
        // host page, whose tail lies past the file's end. Writing zeros into
        // that tail is a store past EOF on a file mapping, which Darwin
        // answers with SIGBUS (benchmarks/stage5-fex.txt, dynamic x86-64).
        // So a file page is converted to an anonymous one first, keeping its
        // bytes: what the mapping showed becomes a private copy, which is what
        // MAP_PRIVATE meant anyway. mach_vm_read_overwrite fails, rather than
        // faults, on the part past EOF, and that part reads as zeros on Linux.
        if (kr == KERN_NO_SPACE && page_is_file_backed(p)) {
            static _Thread_local uint8_t keep[LXRT_HOST_PAGE];
            memset(keep, 0, sizeof keep);
            mach_vm_size_t got = 0;
            (void)mach_vm_read_overwrite(mach_task_self(), p, LXRT_HOST_PAGE,
                                         (mach_vm_address_t)keep, &got);
            // A read-only MAP_SHARED view in this page (KUSER_SHARED_DATA)
            // keeps following the file through shmirror.c.
            uint64_t view = lxrt_shmirror_take_view(p);
            if (mmap((void *)p, LXRT_HOST_PAGE, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
                return false;
            memcpy((void *)p, keep, LXRT_HOST_PAGE);
            lxrt_shmirror_adopt(p, view);
            continue;
        }
        if (mprotect((void *)p, LXRT_HOST_PAGE, PROT_READ | PROT_WRITE) != 0)
            return false;
    }
    return true;
}

// A file segment can start 4 KiB into a host page while its much larger
// interior is still host-page aligned. The edges need a private copy, but the
// interior can remain a lazy MAP_PRIVATE file mapping. libcef.so's 162 MiB
// text segment otherwise made thousands of mach_vm_region/allocate calls and
// pread copies before Steam's webhelper could reach CefInitialize.
static int copy_file_edge(int fd, uint64_t dst, size_t len, uint64_t off)
{
    uint8_t bounce[LXRT_HOST_PAGE] = {0};
    size_t done = 0;
    while (done < len) {
        ssize_t n = pread(fd, bounce + done, len - done, (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (!n) break;
        done += (size_t)n;
    }
    memcpy((void *)dst, bounce, len);
    return 0;
}

static bool fast_file_interior(uint64_t addr, uint64_t len, int prot,
                               int fd, uint64_t off, long *result)
{
    uint64_t end = addr + len;
    uint64_t inside = LXRT_ALIGN_UP(addr, LXRT_HOST_PAGE);
    uint64_t inside_end = LXRT_ALIGN_DOWN(end, LXRT_HOST_PAGE);
    struct stat st;
    if (end < addr || off > UINT64_MAX - len ||
        (addr & (LXRT_HOST_PAGE - 1)) != (off & (LXRT_HOST_PAGE - 1)) ||
        inside_end <= inside || inside_end - inside < (1u << 20) ||
        fstat(fd, &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size < off + len)
        return false;

    uint64_t hstart = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
    uint64_t hend = LXRT_ALIGN_UP(end, LXRT_HOST_PAGE);
    if (hstart < inside) adopt_untracked(hstart);
    if (inside_end < hend) adopt_untracked(inside_end);
    if (!back_range(hstart, inside) || !back_range(inside_end, hend)) {
        *result = LERR(ENOMEM);
        return true;
    }
    void *mapped = mmap((void *)inside, (size_t)(inside_end - inside),
                        PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_FIXED,
                        fd, (off_t)(off + inside - addr));
    if (mapped == MAP_FAILED) {
        *result = LERR(errno);
        return true;
    }
    if ((inside > addr && copy_file_edge(fd, addr, (size_t)(inside - addr), off) != 0) ||
        (inside_end < end && copy_file_edge(fd, inside_end, (size_t)(end - inside_end),
                                           off + inside_end - addr) != 0)) {
        *result = LERR(errno);
        return true;
    }
    record(addr, end, prot);
    if ((hstart < inside && apply_prot(hstart, prot & PROT_WRITE ? PROT_WRITE : PROT_EXEC) != 0) ||
        (inside_end < hend && apply_prot(inside_end, prot & PROT_WRITE ? PROT_WRITE : PROT_EXEC) != 0)) {
        *result = LERR(errno);
        return true;
    }
    int whole_prot = prot;
    if ((whole_prot & PROT_WRITE) && (whole_prot & PROT_EXEC))
        whole_prot &= ~PROT_EXEC;
    if (mprotect((void *)inside, (size_t)(inside_end - inside), whole_prot) != 0) {
        *result = LERR(errno);
        return true;
    }
    *result = (long)addr;
    return true;
}

bool lxrt_trace_on(void);
#define STEP(fmt, ...) do { if (lxrt_trace_on()) \
    fprintf(lxrt_trace_stream(), "[lxrt]    subpage " fmt "\n", ##__VA_ARGS__); } while (0)

static long subpage_mmap_locked(uint64_t addr, uint64_t len, int prot, bool anon,
                                int fd, uint64_t off);

long lxrt_subpage_mmap(uint64_t addr, uint64_t len, int prot, bool anon,
                       int fd, uint64_t off)
{
    if (!addr || !len)
        return LERR(EINVAL);
    STEP("enter 0x%llx+0x%llx prot=%d anon=%d fd=%d off=0x%llx",
         (unsigned long long)addr, (unsigned long long)len, prot, anon, fd,
         (unsigned long long)off);
    STEP("locking");
    pthread_mutex_lock(&g_lock);
    g_in_subpage = true;
    long r = subpage_mmap_locked(addr, len, prot, anon, fd, off);
    g_in_subpage = false;
    pthread_mutex_unlock(&g_lock);
    return r;
}

// Is [addr, addr+len) free at guest-page granularity, as far as this table
// can know? A host page it has never seen is a plain mapping and entirely in
// use; a page it has seen was adopted whole on first contact, so a slot of it
// without a record is free. Caller holds g_lock.
static bool range_free_locked(uint64_t addr, uint64_t len)
{
    uint64_t hstart = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
    uint64_t hend = LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE);
    for (uint64_t p = hstart; p < hend; p += LXRT_HOST_PAGE) {
        mach_vm_address_t ra = p;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t ri;
        mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS
            || ra > p)
            continue;                   // not mapped: free
        if (!page_tracked(p))
            return false;               // a plain mapping: in use
        uint64_t lo = p > addr ? p : addr;
        uint64_t hi = p + LXRT_HOST_PAGE < addr + len ? p + LXRT_HOST_PAGE : addr + len;
        for (int i = 0; i < g_nsubs; i++)
            if (g_subs[i].end > lo && g_subs[i].start < hi)
                return false;           // a guest page lives there
    }
    return true;
}

// MAP_FIXED_NOREPLACE that Darwin refused because a host page in the range is
// already mapped: succeed if every guest page asked for is free, which is what
// the guest -- linked and allocating in 4 KiB units -- can see.
long lxrt_subpage_mmap_noreplace(uint64_t addr, uint64_t len, int prot, bool anon,
                                 int fd, uint64_t off)
{
    if (!addr || !len)
        return LERR(EINVAL);
    pthread_mutex_lock(&g_lock);
    g_in_subpage = true;
    long r;
    if (!range_free_locked(addr, len)) {
        r = LERR(EEXIST);
    } else {
        STEP("noreplace 0x%llx+0x%llx: free at guest granularity, mapping",
             (unsigned long long)addr, (unsigned long long)len);
        r = subpage_mmap_locked(addr, len, prot, anon, fd, off);
    }
    g_in_subpage = false;
    pthread_mutex_unlock(&g_lock);
    return r;
}

static long subpage_mmap_locked(uint64_t addr, uint64_t len, int prot, bool anon,
                                int fd, uint64_t off)
{
    long fast_result;
    if (!getenv("LXRT_NO_FAST_SUBPAGE") && !anon && fd >= 0 &&
        fast_file_interior(addr, len, prot, fd, off, &fast_result)) {
        STEP("file interior mapped directly 0x%llx+0x%llx", (unsigned long long)addr,
             (unsigned long long)len);
        return fast_result;
    }
    uint64_t hstart = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
    uint64_t hend = LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE);
    // Whatever already lives in these host pages and was never tracked (a
    // plain 16 KiB-aligned mapping) must keep its protection: a 4 KiB
    // PROT_NONE guard mapped with MAP_FIXED into the middle of a live page
    // otherwise made the union NONE and took the live 12 KiB down with it
    // (SIGBUS in FEX's allocator arenas under bash's fork loop). Same rule as
    // lxrt_subpage_mprotect; it has to run before back_range() changes prot.
    for (uint64_t p = hstart; p < hend; p += LXRT_HOST_PAGE)
        adopt_untracked(p);
    STEP("backing 0x%llx..0x%llx", (unsigned long long)hstart, (unsigned long long)hend);
    if (!back_range(hstart, hend)) {
        STEP("back_range failed");
        return LERR(ENOMEM);
    }
    STEP("backed");

    if (anon) {
        memset((void *)addr, 0, (size_t)len);
    } else {
        STEP("pread fd=%d len=0x%llx off=0x%llx", fd, (unsigned long long)len,
             (unsigned long long)off);
        // Read rather than map. The content is identical; what is lost is
        // sharing with the page cache.
        //
        // THE READ GOES THROUGH A BOUNCE BUFFER, and that is not an
        // optimisation -- it is the difference between working and deadlocking.
        // An ELF's segments are 4 KiB apart, so the host page containing this
        // sub-page mapping very often ALSO holds a normal, page-aligned mapping
        // of the same file placed by the previous mmap. Reading from a file
        // straight into a private mapping of that same file makes the kernel
        // take the file's VM object lock for the read and then need it again to
        // fault in the copy-out destination. The process wedges in an
        // uninterruptible wait, and so does every later reader of that file --
        // which is exactly what FEX's first x86 segment did, every time.
        enum { BOUNCE = 64 * 1024 };
        uint8_t *bounce = malloc(BOUNCE);
        if (!bounce) {
                return LERR(ENOMEM);
        }
        uint64_t done = 0;
        while (done < len) {
            size_t want = (size_t)(len - done);
            if (want > BOUNCE)
                want = BOUNCE;
            ssize_t r = pread(fd, bounce, want, (off_t)(off + done));
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                int e = errno;
                free(bounce);
                        return LERR(e);
            }
            if (r == 0) {
                // Past end of file: Linux zero-fills the rest of the page.
                memset((void *)(addr + done), 0, (size_t)(len - done));
                break;
            }
            memcpy((void *)(addr + done), bounce, (size_t)r);
            done += (uint64_t)r;
        }
        free(bounce);
    }

    STEP("content placed, recording");
    record(addr, addr + len, prot);
    STEP("recorded, %d ranges tracked", g_nsubs);

    for (uint64_t p = hstart; p < hend; p += LXRT_HOST_PAGE)
        if (apply_prot(p, prot & PROT_WRITE ? PROT_WRITE : PROT_EXEC) != 0) {
                return LERR(errno);
        }
    STEP("done 0x%llx", (unsigned long long)addr);
    return (long)addr;
}

// A host page can hold guest pages the table has never seen: an ordinary
// 16 KiB-aligned mmap is not tracked, and the first sub-page mprotect inside
// it must not pretend the rest of the page is PROT_NONE. FEX's temporary code
// buffer is exactly that: mmap(12 KiB, rw) then mprotect(last 4 KiB, NONE) as
// a guard; with the neighbours unknown the union came out NONE, the whole
// host page went dark, and FEX's very next store into the buffer faulted
// (benchmarks/stage5-fex.txt). So before recording, every untracked 4 KiB slot
// of the page is recorded with the protection the page currently carries.
//
// The guard itself is then lost: union(rw, none) is rw. A guard page is a
// debugging aid, and a program that never touches it cannot tell.
// Caller holds g_lock.
static bool page_tracked(uint64_t hpage)
{
    for (int i = 0; i < g_nsubs; i++)
        if (g_subs[i].end > hpage && g_subs[i].start < hpage + LXRT_HOST_PAGE)
            return true;
    return false;
}

static void adopt_untracked(uint64_t hpage)
{
    // A page the table already knows was adopted whole the first time it was
    // seen; every slot of it without a record since then was CREATED by this
    // module (back_range) or freed by lxrt_subpage_forget, and is free. Adopting
    // it again would mark that free space as used and refuse the next
    // MAP_FIXED_NOREPLACE into it -- FEX's allocator asks for 4 KiB-granular
    // ranges right after a 4 KiB guard mprotect, and got EEXIST (measured,
    // benchmarks/stage5-fex.txt: `wc` under FEX died inside the IR compiler).
    if (page_tracked(hpage))
        return;
    mach_vm_address_t ra = hpage;
    mach_vm_size_t rs = 0;
    vm_region_basic_info_data_64_t ri;
    mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS
        || ra > hpage)
        return;                     // not mapped at all: nothing to adopt
    int cur = (ri.protection & VM_PROT_READ ? PROT_READ : 0)
            | (ri.protection & VM_PROT_WRITE ? PROT_WRITE : 0)
            | (ri.protection & VM_PROT_EXECUTE ? PROT_EXEC : 0);
    bool wx = lxrt_wx_intersects(hpage, LXRT_HOST_PAGE);
    for (uint64_t g = hpage; g < hpage + LXRT_HOST_PAGE; g += GUEST_PAGE) {
        bool covered = false;
        for (int i = 0; i < g_nsubs && !covered; i++)
            if (g_subs[i].start <= g && g_subs[i].end >= g + GUEST_PAGE)
                covered = true;
        // A page of a native guest's RWX range (wxsplit.c) is RWX to the
        // guest whatever the host page carries at this moment.
        if (!covered)
            record(g, g + GUEST_PAGE,
                   wx && lxrt_wx_contains(g) ? PROT_READ | PROT_WRITE | PROT_EXEC : cur);
    }
}

long lxrt_subpage_mprotect(uint64_t addr, uint64_t len, int prot)
{
    uint64_t hstart = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
    uint64_t hend = LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE);

    pthread_mutex_lock(&g_lock);
    for (uint64_t p = hstart; p < hend; p += LXRT_HOST_PAGE)
        adopt_untracked(p);
    record(addr, addr + len, prot);
    // Honour what the caller just asked for: a guest calling mprotect for write
    // is about to write, so write is what it gets. Execute comes back on the
    // next instruction fetch, through the fault handler.
    for (uint64_t p = hstart; p < hend; p += LXRT_HOST_PAGE)
        if (apply_prot(p, prot & PROT_WRITE ? PROT_WRITE : PROT_EXEC) != 0) {
            pthread_mutex_unlock(&g_lock);
            return LERR(errno);
        }
    pthread_mutex_unlock(&g_lock);
    return 0;
}

// Apply the union to one host page, withholding whichever of write/execute the
// caller did not prefer when both are wanted and the platform refuses both.
// Caller holds g_lock.
static int apply_prot(uint64_t hpage, int prefer)
{
    int want = union_prot(hpage);
    if (!want)
        return mprotect((void *)hpage, LXRT_HOST_PAGE, PROT_NONE);
    if ((want & PROT_WRITE) && (want & PROT_EXEC))
        want &= (prefer == PROT_WRITE) ? ~PROT_EXEC : ~PROT_WRITE;
    return mprotect((void *)hpage, LXRT_HOST_PAGE, want);
}

// The protection the guest gave the 4 KiB page at g (0 when nothing is
// mapped there). Only meaningful in a tracked host page. Caller holds g_lock.
static int slot_prot(uint64_t g)
{
    int p = 0;
    for (int i = 0; i < g_nsubs; i++)
        if (g_subs[i].start < g + GUEST_PAGE && g_subs[i].end > g)
            p |= g_subs[i].prot;
    return p;
}

// LXRT_SUBPAGE_LOG=<n>: report the first n split events of this process
// (flips, emulated stores).
static bool subpage_log_one(void)
{
    static int logmax = -1;
    static _Atomic int logged;
    if (logmax < 0) { const char *e = getenv("LXRT_SUBPAGE_LOG"); logmax = e ? atoi(e) : 0; }
    return logged < logmax && atomic_fetch_add(&logged, 1) < logmax;
}

// Code written into an executable guest page is scanned again before it can
// run, as mapped code is (dispatch.c) and JIT output is (jit.c): a `svc` that
// is not rewritten runs an arbitrary Darwin syscall. LXRT_NO_RESCAN=1 turns
// this off, as it does for JIT output (a diagnostic knob, and the negative
// control of the tests).
bool lxrt_dispatch_rewrite_mapped(void);
static bool rescan_enabled(void)
{
    static int no = -1;
    if (no < 0) no = getenv("LXRT_NO_RESCAN") ? 1 : 0;
    return !no && lxrt_dispatch_rewrite_mapped();
}

// Rewrite every guest page in [lo, hi) the guest mapped executable. The host
// pages must be readable (a split page always is: read-write or read-execute);
// they are made writable here, and only when there is something to rewrite.
// Returns how many were scanned. Caller holds g_lock.
static int rescan_exec_slots(uint64_t lo, uint64_t hi)
{
    int n = 0;
    for (uint64_t g = LXRT_ALIGN_DOWN(lo, GUEST_PAGE); g < hi; g += GUEST_PAGE) {
        if (!(slot_prot(g) & PROT_EXEC))
            continue;
        n++;
        if (!lxrt_rewrite_has_candidates(g, g + GUEST_PAGE))
            continue;                   // nothing a rewrite could change
        if (mprotect((void *)LXRT_ALIGN_DOWN(g, LXRT_HOST_PAGE), LXRT_HOST_PAGE,
                     PROT_READ | PROT_WRITE) != 0)
            continue;
        struct lxrt_rewrite_report rep;
        char *err = NULL;
        if (lxrt_rewrite_range(g, g + GUEST_PAGE, &rep, &err) == 0 &&
            (rep.sites_found || rep.tls_read_found || rep.tls_write_found ||
             rep.ctr_found || rep.sysreg_found))
            fprintf(lxrt_trace_stream(), "[lxrt] sub-page code written at run time 0x%llx: %zu svc "
                    "rewritten, %zu poisoned, %zu tls, %zu ctr/sysreg\n", (unsigned long long)g,
                    rep.sites_rewritten, rep.sites_unreachable + rep.tls_unreachable,
                    rep.tls_rewritten, rep.ctr_rewritten + rep.sysreg_rewritten);
    }
    return n;
}

enum { EMU_PASS, EMU_DONE, EMU_FAULT };

// A store executed from a W/X-split host page into that same page: the page
// can never be writable and executable at once, so the store never completes
// natively (the flip below just alternated write and exec at the same pc,
// forever). Perform it here instead (storemu.c) and resume after it:
//
//   EMU_DONE  -- stored, pc advanced;
//   EMU_FAULT -- the guest page it writes is not writable in guest terms:
//                a real fault, exactly where Linux would raise it;
//   EMU_PASS  -- not this case, or not an instruction storemu.c performs:
//                the caller keeps the old flip.
//
// The page is opened for writing only while this thread is parked here with
// g_lock held; any other thread that runs into it waits on the lock in its
// own fault handler and finds it executable again. Caller holds g_lock.
static int emulate_store_locked(uint64_t pc, uint64_t ph, void *uap)
{
    uint32_t insn = *(const uint32_t *)(uintptr_t)pc;   // executing: readable
    struct lxrt_storemu m;
    const char *why = NULL;
    if (lxrt_storemu_decode(insn, pc, uap, &m, &why) != 0) {
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 8)
            fprintf(lxrt_trace_stream(), "[lxrt] subpage: store at pc 0x%llx (insn %08x) shares its "
                    "host page with its target and cannot be emulated (%s): W/X flip\n",
                    (unsigned long long)pc, insn, why ? why : "?");
        return EMU_PASS;
    }
    uint64_t lo = m.addr, hi = m.addr + m.len;
    if (hi <= lo || hi <= ph || lo >= ph + LXRT_HOST_PAGE)
        return EMU_PASS;                    // does not touch the page it runs from

    // Guest permission, 4 KiB page by 4 KiB page.
    for (uint64_t g = LXRT_ALIGN_DOWN(lo, GUEST_PAGE); g < hi; g += GUEST_PAGE) {
        uint64_t hp = LXRT_ALIGN_DOWN(g, LXRT_HOST_PAGE);
        if (page_tracked(hp)) {
            if (!(slot_prot(g) & PROT_WRITE))
                return EMU_FAULT;
            continue;
        }
        mach_vm_address_t ra = g;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t ri;
        mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS || ra > g)
            return EMU_FAULT;               // unmapped
        if (!(ri.protection & VM_PROT_WRITE))
            return EMU_PASS;                // someone else's (copy-on-write, JIT): not ours
    }

    // Open every split host page the store touches (at most three: DC ZVA
    // blocks are at most 2 KiB).
    uint64_t opened[3];
    int nopen = 0;
    bool code = false;
    for (uint64_t hp = LXRT_ALIGN_DOWN(lo, LXRT_HOST_PAGE); hp < hi && nopen < 3; hp += LXRT_HOST_PAGE) {
        if (!page_tracked(hp) || !needs_wx_split(hp))
            continue;
        if (mprotect((void *)hp, LXRT_HOST_PAGE, PROT_READ | PROT_WRITE) != 0) {
            for (int i = 0; i < nopen; i++)
                apply_prot(opened[i], opened[i] == ph ? PROT_EXEC : PROT_WRITE);
            return EMU_PASS;
        }
        opened[nopen++] = hp;
    }
    for (uint64_t g = LXRT_ALIGN_DOWN(lo, GUEST_PAGE); g < hi; g += GUEST_PAGE)
        if (page_tracked(LXRT_ALIGN_DOWN(g, LXRT_HOST_PAGE)) && (slot_prot(g) & PROT_EXEC))
            code = true;

    g_in_subpage = true;                    // a fault in here is ours: report it
    lxrt_storemu_perform(&m, uap);
    if (code) {
        // A store into an executable guest page (one the guest made writable
        // too, or the check above would have refused it): rescan that page
        // before it runs again.
        if (rescan_enabled())
            rescan_exec_slots(lo, hi);
        sys_icache_invalidate((void *)(uintptr_t)lo, (size_t)(hi - lo));
    }
    g_in_subpage = false;
    for (int i = 0; i < nopen; i++)
        apply_prot(opened[i], opened[i] == ph ? PROT_EXEC : PROT_WRITE);

    if (subpage_log_one())
        fprintf(lxrt_trace_stream(), "[lxrt] subpage emulate pid %d: pc 0x%llx insn %08x addr 0x%llx+%u%s\n",
                (int)getpid(), (unsigned long long)pc, insn, (unsigned long long)lo, m.len,
                code ? " (executable page: rescanned)" : "");
    return EMU_DONE;
}

// The fault-driven half of the W^X split, mirroring what jit.c does for
// MAP_JIT. A store to a page currently read-execute flips it to read-write; an
// instruction fetch from a page currently read-write flips it back. Returns
// true when the faulting instruction should simply be retried (or, for a
// store performed here, resumed after).
bool lxrt_subpage_handle_fault(uint64_t pc, uint64_t fault_addr, void *uap)
{
    if (!fault_addr)
        return false;
    // A fault raised by this module's own stores, with the lock held, is a
    // genuine fault and must be reported, not waited on: taking g_lock here
    // wedged the process forever the first time it happened.
    if (g_in_subpage)
        return false;
    uint64_t hpage = LXRT_ALIGN_DOWN(fault_addr, LXRT_HOST_PAGE);
    // Instruction abort or data abort, from the syndrome; without a context,
    // pc == fault_addr means the fault was the fetch itself. Only a write
    // (WnR, ESR bit 6: stores, atomics, DC ZVA) can be a store to perform.
    bool fetch = pc == fault_addr, store = false;
    if (uap) {
        uint32_t esr = ((ucontext_t *)uap)->uc_mcontext->__es.__esr, ec = esr >> 26;
        if (ec == 0x20 || ec == 0x21) {
            fetch = true;
        } else if (ec == 0x24 || ec == 0x25) {
            fetch = false;
            store = (esr >> 6) & 1;
        }
    }

    pthread_mutex_lock(&g_lock);
    if (store) {
        uint64_t ph = LXRT_ALIGN_DOWN(pc, LXRT_HOST_PAGE);
        if (needs_wx_split(ph)) {
            int r = emulate_store_locked(pc, ph, uap);
            if (r != EMU_PASS) {
                pthread_mutex_unlock(&g_lock);
                return r == EMU_DONE;
            }
        }
    }
    if (!needs_wx_split(hpage)) {
        pthread_mutex_unlock(&g_lock);
        return false;               // nothing withheld here; a real fault
    }
    int prefer = fetch ? PROT_EXEC : PROT_WRITE;
    // Going back to execute: the page was writable, so anything may have been
    // stored into its executable guest pages. Rescan them first (a scan of
    // unchanged code finds nothing and changes nothing).
    int rescanned = 0;
    if (prefer == PROT_EXEC && rescan_enabled()) {
        g_in_subpage = true;
        rescanned = rescan_exec_slots(hpage, hpage + LXRT_HOST_PAGE);
        g_in_subpage = false;
    }
    // LXRT_SUBPAGE_LOG=<n>: report the first n split flips of this process
    // with the guest ranges that make the host page want write and execute.
    if (subpage_log_one()) {
        fprintf(lxrt_trace_stream(), "[lxrt] subpage flip pid %d: pc 0x%llx addr 0x%llx -> %s (subs %d, "
                "rescanned %d):", (int)getpid(), (unsigned long long)pc,
                (unsigned long long)fault_addr, prefer == PROT_EXEC ? "exec" : "write", g_nsubs,
                rescanned);
        for (int i = 0; i < g_nsubs; i++)
            if (g_subs[i].start < hpage + LXRT_HOST_PAGE && g_subs[i].end > hpage)
                fprintf(lxrt_trace_stream(), " [0x%llx-0x%llx %d]", (unsigned long long)g_subs[i].start,
                        (unsigned long long)g_subs[i].end, g_subs[i].prot);
        fprintf(lxrt_trace_stream(), "\n");
    }
    // A native guest's RWX guest pages in this host page (a JIT's code at
    // 4 KiB granularity) are scanned before the page becomes executable, the
    // way wxsplit.c does it for whole host pages: read-only, count, rewrite,
    // count again, and only then execute. RX guest pages were scanned when
    // they became executable; RW ones are data and never scanned (a data word
    // that looks like svc is not an instruction, and rewriting it would
    // corrupt it). FEX's guest pages hold x86 code: no scan there.
    if (prefer == PROT_EXEC && lxrt_wx_enabled()) {
        struct lxrt_range r[LXRT_HOST_PAGE / GUEST_PAGE];
        int nr = 0;
        for (int i = 0; i < g_nsubs && nr < (int)(sizeof r / sizeof r[0]); i++) {
            if ((g_subs[i].prot & (PROT_WRITE | PROT_EXEC)) != (PROT_WRITE | PROT_EXEC) ||
                g_subs[i].start >= hpage + LXRT_HOST_PAGE || g_subs[i].end <= hpage)
                continue;
            r[nr].start = g_subs[i].start > hpage ? g_subs[i].start : hpage;
            r[nr].end = g_subs[i].end < hpage + LXRT_HOST_PAGE ? g_subs[i].end : hpage + LXRT_HOST_PAGE;
            nr++;
        }
        g_in_subpage = true;        // the scan's own faults are not flips
        bool clean = !nr || lxrt_wx_scan_for_exec(hpage, r, nr);
        g_in_subpage = false;
        if (!clean) {
            pthread_mutex_unlock(&g_lock);
            return false;
        }
    }
    int rc = apply_prot(hpage, prefer);
    pthread_mutex_unlock(&g_lock);
    return rc == 0;
}

// True if any sub-page mapping is being tracked in this range, meaning
// mprotect must go through the union rather than straight to the host.
// After the caller opened the host pages of [addr, addr+len) for writing
// (to rewrite code it just mapped), put the union protection back.
void lxrt_subpage_reapply(uint64_t addr, uint64_t len)
{
    uint64_t hstart = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
    uint64_t hend = LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE);
    pthread_mutex_lock(&g_lock);
    g_in_subpage = true;
    for (uint64_t p = hstart; p < hend; p += LXRT_HOST_PAGE)
        apply_prot(p, PROT_EXEC);
    g_in_subpage = false;
    pthread_mutex_unlock(&g_lock);
}

// True when every tracked guest range overlapping [addr, addr+len) is a
// PROT_NONE placeholder (FEX hands freed pages back to its reservation one
// 4 KiB mmap at a time, and those are tracked here) -- nothing live.
bool lxrt_subpage_only_placeholders(uint64_t addr, uint64_t len)
{
    pthread_mutex_lock(&g_lock);
    bool ok = true;
    for (int i = 0; i < g_nsubs && ok; i++)
        if (g_subs[i].start < addr + len && g_subs[i].end > addr && g_subs[i].prot != 0)
            ok = false;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

bool lxrt_subpage_tracked(uint64_t addr, uint64_t len)
{
    pthread_mutex_lock(&g_lock);
    bool found = false;
    for (int i = 0; i < g_nsubs && !found; i++)
        found = g_subs[i].start < addr + len && g_subs[i].end > addr;
    pthread_mutex_unlock(&g_lock);
    return found;
}
