// The guest's fixed-address arena: 0x140000000 - 0x160000000 and what the
// image's slide adds around it.
//
// A 64-bit Windows program is linked to load at 0x140000000, and Wine maps
// it there with MAP_FIXED (its preloader reserves the range the same way).
// On Linux that address is free. Here it is in the one gap Darwin fills
// first: above the runtime's own image and the guest's (just over 4 GiB),
// below the dyld shared cache (0x180000000). The host's malloc put a 128 MiB
// region exactly there, the fixed mapping was refused to protect it
// (dispatch.c, host_heap_hit: "REFUSED mmap MAP_FIXED 0x140000000+0x17000:
// overlaps host malloc region 0x140000000+0x8000000 (tag 12)"), Wine ran a
// program that was never mapped, and every game started through Steam's
// container ended at once with FEX's "NoExec instruction in entry block: 0"
// (MEASURED, benchmarks/stage50; a program started outside the container
// happened to get there before the host's heap did).
//
// So the range belongs to the runtime's own image: a zero-fill segment with
// no access (arena_seg.s), which the kernel maps at exec, before libmalloc
// exists. Reserving it from a constructor was too late: by then the host's
// heap was already there in one start in eight (MEASURED). The segment
// slides with the image, so its bounds are read at start-up; 0x140000000 to
// 0x160000000 is inside it at any slide. It is kept for the guest:
//   - a guest mapping there replaces the reservation, with MAP_FIXED, with
//     MAP_FIXED_NOREPLACE or with a plain hint, as long as no guest mapping
//     is in the way (a bitmap of host pages the guest has mapped);
//   - a guest munmap there puts the reservation back instead of leaving a
//     hole for the host's heap to fall into.
// Nothing of the host's is ever inside it, so nothing of the host's can be
// replaced. The hole the guest's break grows into is kept the same way
// (lxrt_arena_reserve_heap). LXRT_NO_ARENA=1 leaves the segment reserved but
// stops treating either as free for the guest (bisecting aid).

#include "lxrt.h"

#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

#define SEG_MAX_PAGES (0x25000000ull / LXRT_HOST_PAGE)     // the segment's size, arena_seg.s
#define HEAP_SPAN (4ull << 30)                             // the guest's break, see lxrt_arena_reserve_heap
#define HEAP_PAGES (HEAP_SPAN / LXRT_HOST_PAGE)

extern const struct mach_header_64 __dso_handle;

// Two ranges: the image's segment (fixed mappings of Windows programs) and
// the hole the guest's program break grows into (dispatch.c). Each has a
// bitmap of the host pages a guest mapping covers.
struct arena {
    uint64_t lo, hi;
    _Atomic unsigned char *used;
};
static _Atomic unsigned char g_seg_used[SEG_MAX_PAGES / 8];
static _Atomic unsigned char g_heap_used[HEAP_PAGES / 8];
static struct arena g_arena[2] = {{0, 0, g_seg_used}, {0, 0, g_heap_used}};
static bool g_off;

static bool page_used(const struct arena *ar, uint64_t i) { return (atomic_load(&ar->used[i / 8]) >> (i % 8)) & 1; }

static void page_set(const struct arena *ar, uint64_t i, bool used)
{
    if (used)
        atomic_fetch_or(&ar->used[i / 8], (unsigned char)(1u << (i % 8)));
    else
        atomic_fetch_and(&ar->used[i / 8], (unsigned char)~(1u << (i % 8)));
}

__attribute__((constructor)) static void arena_find(void)
{
    unsigned long size = 0;
    uint8_t *seg = getsegmentdata(&__dso_handle, "__LXRT_ARENA", &size);
    const char *off = getenv("LXRT_NO_ARENA");
    g_off = off && *off == '1';
    if (!seg || !size || g_off)
        return;
    uint64_t lo = LXRT_ALIGN_UP((uint64_t)(uintptr_t)seg, LXRT_HOST_PAGE);
    uint64_t hi = LXRT_ALIGN_DOWN((uint64_t)(uintptr_t)seg + size, LXRT_HOST_PAGE);
    if (hi - lo > SEG_MAX_PAGES * LXRT_HOST_PAGE)
        hi = lo + SEG_MAX_PAGES * LXRT_HOST_PAGE;
    if (hi <= lo)
        return;
    // The segment's maximum protection is none (the linker gives a segment
    // one protection on arm64), and a page of it can then never be given
    // access with mprotect -- which is what the sub-page path does to a host
    // page it shares (subpage.c; an x86 guest's 4 KiB mappings: "mmap
    // 0x140000000+0x2a000 ... -> -12", MEASURED). The range is the image's
    // own, so an ordinary reservation can take the segment's place.
    if (mmap((void *)(uintptr_t)lo, (size_t)(hi - lo), PROT_NONE,
             MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != (void *)(uintptr_t)lo)
        return;
    g_arena[0].lo = lo;
    g_arena[0].hi = hi;
}

bool lxrt_arena_on(void) { return g_arena[0].hi != 0; }

// For a refusal's report: where the image's arena is in this process.
void lxrt_arena_bounds(uint64_t *lo, uint64_t *hi)
{
    *lo = g_arena[0].lo;
    *hi = g_arena[0].hi;
}

// The guest's program break. Linux leaves a very large hole above the break,
// and FEX asks for the page right after it with MAP_FIXED_NOREPLACE: the
// break has to sit where that page is free. A hole the runtime found and
// gave back (the first design) is free for the host too, and the host's
// malloc opened a 128 MiB region exactly at its start: "Couldn't allocate
// page after SBRK." in 15 of 40 starts of FEX (MEASURED, benchmarks/stage50).
// So the hole is reserved, and free only for the guest. Returns its start,
// 0 if there is no such hole.
uint64_t lxrt_arena_reserve_heap(void)
{
    if (g_off)
        return 0;
    void *p = mmap(NULL, (size_t)HEAP_SPAN, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return 0;
    g_arena[1].lo = (uint64_t)(uintptr_t)p;
    g_arena[1].hi = g_arena[1].lo + HEAP_SPAN;
    return g_arena[1].lo;
}

static const struct arena *arena_of(uint64_t addr)
{
    for (int k = 0; k < 2; k++)
        if (g_arena[k].hi && addr >= g_arena[k].lo && addr < g_arena[k].hi)
            return &g_arena[k];
    return NULL;
}

// The part of [addr, addr+len) inside one arena, as its host pages [*a, *b).
static bool clip(const struct arena *ar, uint64_t addr, uint64_t len, uint64_t *a, uint64_t *b)
{
    if (!ar->hi || !len || addr + len < addr)
        return false;
    uint64_t lo = addr < ar->lo ? ar->lo : addr;
    uint64_t hi = addr + len > ar->hi ? ar->hi : addr + len;
    if (lo >= hi)
        return false;
    *a = (LXRT_ALIGN_DOWN(lo, LXRT_HOST_PAGE) - ar->lo) / LXRT_HOST_PAGE;
    *b = (LXRT_ALIGN_UP(hi, LXRT_HOST_PAGE) - ar->lo) / LXRT_HOST_PAGE;
    return true;
}

// Is the whole range inside an arena with no guest mapping on any of its
// host pages? Then only the reservation is there, and a guest mapping may
// take its place.
bool lxrt_arena_free(uint64_t addr, uint64_t len)
{
    const struct arena *ar = arena_of(addr);
    uint64_t a, b;
    if (!ar || !len || addr + len < addr || addr + len > ar->hi || !clip(ar, addr, len, &a, &b))
        return false;
    for (uint64_t i = a; i < b; i++)
        if (page_used(ar, i))
            return false;
    return true;
}

// A guest mapping now covers the range.
void lxrt_arena_mapped(uint64_t addr, uint64_t len)
{
    for (int k = 0; k < 2; k++) {
        uint64_t a, b;
        if (!clip(&g_arena[k], addr, len, &a, &b))
            continue;
        for (uint64_t i = a; i < b; i++)
            page_set(&g_arena[k], i, true);
    }
}

// The guest unmapped (part of) the range: every host page of it that is now
// a hole gets the reservation back. A page still mapped (a 4 KiB neighbour
// keeps it, subpage.c) stays the guest's.
void lxrt_arena_unmapped(uint64_t addr, uint64_t len)
{
    for (int k = 0; k < 2; k++) {
        const struct arena *ar = &g_arena[k];
        uint64_t a, b;
        if (!clip(ar, addr, len, &a, &b))
            continue;
        // VM_FLAGS_FIXED without overwrite: succeeds only on a hole. The
        // whole range at once when it is one (the usual munmap of a
        // mapping), else page by page.
        mach_vm_address_t all = ar->lo + a * LXRT_HOST_PAGE;
        if (mach_vm_map(mach_task_self(), &all, (b - a) * LXRT_HOST_PAGE, 0, VM_FLAGS_FIXED, MACH_PORT_NULL, 0,
                        FALSE, VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_COPY) == KERN_SUCCESS) {
            for (uint64_t i = a; i < b; i++)
                page_set(ar, i, false);
            continue;
        }
        for (uint64_t i = a; i < b; i++) {
            mach_vm_address_t at = ar->lo + i * LXRT_HOST_PAGE;
            if (mach_vm_map(mach_task_self(), &at, LXRT_HOST_PAGE, 0, VM_FLAGS_FIXED, MACH_PORT_NULL, 0, FALSE,
                            VM_PROT_NONE, VM_PROT_ALL, VM_INHERIT_COPY) == KERN_SUCCESS)
                page_set(ar, i, false);
        }
    }
}
