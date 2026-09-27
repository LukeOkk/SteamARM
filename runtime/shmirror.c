// shmirror.c -- read-only MAP_SHARED file mappings inside a shared host page.
//
// A 4 KiB MAP_SHARED view of a file can be a real shared mapping only while it
// owns its whole 16 KiB host page (dispatch.c maps it rounded up when the rest
// of the page is free). When a guest later maps something else into the same
// host page, the page becomes a sub-page composite (subpage.c back_range):
// anonymous memory holding a COPY of the file bytes, which never changes
// again.
//
// Wine does exactly this with KUSER_SHARED_DATA: ntdll maps wineserver's
// section read-only at 0x7ffe0000 (4 KiB) and, in Wine 9 and later, its
// private "hypervisor shared data" page right after it, at 0x7ffe1000 -- the
// same 16 KiB page. wineserver updates the tick count and the system time in
// that section; every 64-bit client then read a frozen clock (MEASURED:
// GetTickCount advanced 0 ms across Sleep(1000), GetTickCount64 constant).
// 32-bit processes were unaffected: FEX's 32-bit allocator places the section
// differently.
//
// Here such ranges are remembered at mmap time (read-only only: a writable
// shared range cannot be merged this way). When subpage.c converts their host
// page, it hands the old file page over first: the runtime keeps a live view
// of it (mach_vm_remap without copy: the same VM object, the same page cache)
// and a writable alias of the new composite page, and a thread copies the
// file bytes into the guest range every MIRROR_PERIOD_US. That is polling,
// not sharing: a reader can see a value up to one period old, which is
// finer than wineserver's own update interval (16 ms).
#include "lxrt.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define MIRROR_PERIOD_US 2000
#define NOTE_MAX 256
#define MIRROR_MAX 64

struct note { uint64_t start, end; };                 // read-only shared ranges
struct mirror {
    uint64_t hpage;                                     // guest-visible host page
    mach_vm_address_t view;                             // live view of the file page
    mach_vm_address_t alias;                            // writable alias of hpage
    uint64_t start, end;                                // the guest range in it
};

static struct note g_notes[NOTE_MAX];
static int g_nnotes;
static struct mirror g_mir[MIRROR_MAX];
static int g_nmir;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(shmirror_lock, g_lock)
static bool g_thread_started;

bool lxrt_trace_on(void);

void lxrt_shmirror_note(uint64_t addr, uint64_t len)
{
    pthread_mutex_lock(&g_lock);
    if (g_nnotes < NOTE_MAX)
        g_notes[g_nnotes++] = (struct note){ addr, addr + len };
    pthread_mutex_unlock(&g_lock);
}

static void drop_mirror(int i)
{
    mach_vm_deallocate(mach_task_self(), g_mir[i].view, LXRT_HOST_PAGE);
    mach_vm_deallocate(mach_task_self(), g_mir[i].alias, LXRT_HOST_PAGE);
    g_mir[i] = g_mir[--g_nmir];
}

void lxrt_shmirror_forget(uint64_t addr, uint64_t len)
{
    uint64_t end = addr + len;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nnotes; )
        if (g_notes[i].start < end && g_notes[i].end > addr)
            g_notes[i] = g_notes[--g_nnotes];
        else
            i++;
    for (int i = 0; i < g_nmir; )
        if (g_mir[i].start < end && g_mir[i].end > addr)
            drop_mirror(i);
        else
            i++;
    pthread_mutex_unlock(&g_lock);
}

static void *mirror_thread(void *arg)
{
    (void)arg;
    for (;;) {
        usleep(MIRROR_PERIOD_US);
        pthread_mutex_lock(&g_lock);
        for (int i = 0; i < g_nmir; i++) {
            struct mirror *m = &g_mir[i];
            uint64_t off = m->start - m->hpage, n = m->end - m->start;
            if (memcmp((void *)(m->alias + off), (void *)(m->view + off), n))
                memcpy((void *)(m->alias + off), (void *)(m->view + off), n);
        }
        pthread_mutex_unlock(&g_lock);
    }
    return NULL;
}

static void start_thread_locked(void)
{
    if (g_thread_started)
        return;
    // No guest signal may be delivered on this thread.
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    pthread_t t;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 64 * 1024);
    if (pthread_create(&t, &at, mirror_thread, NULL) == 0)
        g_thread_started = true;
    pthread_attr_destroy(&at);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

// subpage.c, before it replaces the file page at hpage: keep a live view of
// it if a noted range lives there. Returns the view (0 if none).
uint64_t lxrt_shmirror_take_view(uint64_t hpage)
{
    uint64_t view = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nnotes && !view; i++) {
        if (g_notes[i].end <= hpage || g_notes[i].start >= hpage + LXRT_HOST_PAGE)
            continue;
        mach_vm_address_t v = 0;
        vm_prot_t cur, max;
        if (mach_vm_remap(mach_task_self(), &v, LXRT_HOST_PAGE, 0, VM_FLAGS_ANYWHERE,
                          mach_task_self(), hpage, FALSE, &cur, &max, VM_INHERIT_NONE) == KERN_SUCCESS)
            view = v;
    }
    pthread_mutex_unlock(&g_lock);
    return view;
}

// subpage.c, after the composite page is in place: start mirroring.
void lxrt_shmirror_adopt(uint64_t hpage, uint64_t view)
{
    if (!view)
        return;
    mach_vm_address_t alias = 0;
    vm_prot_t cur, max;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &alias, LXRT_HOST_PAGE, 0, VM_FLAGS_ANYWHERE,
                                     mach_task_self(), hpage, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr == KERN_SUCCESS)
        kr = mach_vm_protect(mach_task_self(), alias, LXRT_HOST_PAGE, FALSE, VM_PROT_READ | VM_PROT_WRITE);
    pthread_mutex_lock(&g_lock);
    int added = 0;
    for (int i = 0; kr == KERN_SUCCESS && i < g_nnotes; i++) {
        uint64_t s = g_notes[i].start > hpage ? g_notes[i].start : hpage;
        uint64_t e = g_notes[i].end < hpage + LXRT_HOST_PAGE ? g_notes[i].end : hpage + LXRT_HOST_PAGE;
        if (s >= e || g_nmir >= MIRROR_MAX)
            continue;
        // Each mirror owns its view and alias; a second range in the same
        // page gets its own remaps.
        mach_vm_address_t v = view, a = alias;
        if (added) {
            v = a = 0;
            if (mach_vm_remap(mach_task_self(), &v, LXRT_HOST_PAGE, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                              view, FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS ||
                mach_vm_remap(mach_task_self(), &a, LXRT_HOST_PAGE, 0, VM_FLAGS_ANYWHERE, mach_task_self(),
                              alias, FALSE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS)
                continue;
        }
        g_mir[g_nmir++] = (struct mirror){ hpage, v, a, s, e };
        added++;
        if (lxrt_trace_on())
            fprintf(lxrt_trace_stream(), "[lxrt]    shmirror 0x%llx..0x%llx mirrored from a live file view\n",
                    (unsigned long long)s, (unsigned long long)e);
    }
    if (added)
        start_thread_locked();
    pthread_mutex_unlock(&g_lock);
    if (!added) {
        mach_vm_deallocate(mach_task_self(), view, LXRT_HOST_PAGE);
        if (alias)
            mach_vm_deallocate(mach_task_self(), alias, LXRT_HOST_PAGE);
    }
}

// A forked child has no mirror thread and its views were not inherited
// (VM_INHERIT_NONE): forget them. A child that execs starts over anyway.
static void child_reset(void)
{
    g_nmir = 0;
    g_thread_started = false;
}
__attribute__((constructor(201))) static void shmirror_register_fork(void)
{
    pthread_atfork(NULL, NULL, child_reset);
}
