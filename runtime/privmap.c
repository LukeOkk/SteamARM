// privmap.c -- Linux MAP_PRIVATE semantics for shared-memory files.
//
// On Linux a MAP_PRIVATE file mapping keeps showing the file's current
// contents on every page the process has not written yet; only a store makes
// a page private. Darwin copies at mmap time (MEASURED: a private mapping saw
// none of three later updates through a MAP_SHARED view of the same file).
//
// Wine relies on the Linux behaviour: a read-only view of a section is mapped
// MAP_PRIVATE on Linux (virtual.c map_file_into_view, "#ifdef __linux__").
// wineserver's session objects (window classes, desktops, ...) live in such a
// section; every client saw the ids of the moment it mapped it, found them
// stale ("Session object id doesn't match"), failed to register window
// classes, and each process needing a desktop started one more explorer.exe
// -- 45 of them in 12 s (MEASURED).
//
// For a memfd (the runtime's shared-memory files; wineserver makes every
// section one), a private mapping is therefore mapped MAP_SHARED without
// write permission. The first store into a host page faults and turns that
// page -- only it -- into a private anonymous copy with the protection the
// guest asked for: copy on write, the way Linux does it.
//
// Only host-page aligned mappings are handled; anything else keeps the old
// path (a copy). Mappings outside this table are not affected.
#include "lxrt.h"
#include "fex_support.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/param.h>

#define LERR(e) (-lxrt_errno_to_linux(e))

struct pm { uint64_t start, end; int want; };   // want: the guest's PROT_*

#define PM_MAX 1024
static struct pm g_pm[PM_MAX];
static int g_npm;
static pthread_mutex_t g_pm_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(privmap_lock, g_pm_lock)

bool lxrt_privmap_eligible(int fd, uint64_t addr, uint64_t len, uint64_t off, int prot)
{
    if (fd < 0 || (prot & PROT_EXEC) || !len)
        return false;
    if (addr % LXRT_HOST_PAGE || len % LXRT_HOST_PAGE || off % LXRT_HOST_PAGE)
        return false;
    return lxrt_memfd_path_is(fd);
}

// Drop [s, e) from the table, splitting records that straddle it. Caller
// holds the lock. Returns false when a split found the table full (the
// record is then cut short on the right, which only loses coherence there).
static bool cut(uint64_t s, uint64_t e)
{
    bool ok = true;
    for (int i = 0; i < g_npm; ) {
        struct pm *p = &g_pm[i];
        if (p->end <= s || p->start >= e) { i++; continue; }
        if (p->start < s && p->end > e) {
            if (g_npm < PM_MAX) {
                g_pm[g_npm++] = (struct pm){ e, p->end, p->want };
            } else {
                ok = false;
            }
            p->end = s;
            i++;
        } else if (p->start < s) {
            p->end = s;
            i++;
        } else if (p->end > e) {
            p->start = e;
            i++;
        } else {
            g_pm[i] = g_pm[--g_npm];
        }
    }
    return ok;
}

void lxrt_privmap_forget(uint64_t addr, uint64_t len)
{
    if (!len)
        return;
    pthread_mutex_lock(&g_pm_lock);
    if (g_npm)
        cut(addr, addr + len);
    pthread_mutex_unlock(&g_pm_lock);
}

// Map an eligible private mapping (see lxrt_privmap_eligible). `flags` are the
// Darwin flags the caller would have used. Returns the address or a Linux
// error, like do_mmap.
long lxrt_privmap_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off)
{
    flags = (flags & ~MAP_PRIVATE) | MAP_SHARED;
    void *p = mmap((void *)(uintptr_t)addr, (size_t)len, prot & ~PROT_WRITE, flags, fd, (off_t)off);
    if (p == MAP_FAILED)
        return LERR(errno);
    uint64_t a = (uint64_t)(uintptr_t)p;
    pthread_mutex_lock(&g_pm_lock);
    cut(a, a + len);
    bool room = g_npm < PM_MAX;
    if (room)
        g_pm[g_npm++] = (struct pm){ a, a + len, prot };
    pthread_mutex_unlock(&g_pm_lock);
    if (!room) {
        // No room to track copy-on-write: fall back to Darwin's own private
        // copy, which is at least never wrong about what the guest wrote.
        void *q = mmap(p, (size_t)len, prot, (flags & ~MAP_SHARED) | MAP_PRIVATE | MAP_FIXED, fd, (off_t)off);
        if (q == MAP_FAILED) {
            int e = errno;
            munmap(p, (size_t)len);
            return LERR(e);
        }
    }
    return (long)a;
}

// After a successful mprotect of [addr, addr+len) to `prot`: record the new
// wish for the still-shared pages and keep them unwritable on the host.
void lxrt_privmap_after_mprotect(uint64_t addr, uint64_t len, int prot)
{
    if (!len)
        return;
    uint64_t e = addr + len;
    pthread_mutex_lock(&g_pm_lock);
    if (!g_npm) {
        pthread_mutex_unlock(&g_pm_lock);
        return;
    }
    // Collect the overlapped pieces, cut them out, put them back with the
    // new wish (a record partly inside the range splits in two).
    struct pm got[16];
    int ngot = 0;
    for (int i = 0; i < g_npm && ngot < 16; i++) {
        struct pm *p = &g_pm[i];
        if (p->end <= addr || p->start >= e)
            continue;
        got[ngot++] = (struct pm){ MAX(p->start, addr), MIN(p->end, e), prot };
    }
    cut(addr, e);
    for (int i = 0; i < ngot && g_npm < PM_MAX; i++) {
        g_pm[g_npm++] = got[i];
        if (prot & PROT_WRITE)
            mprotect((void *)(uintptr_t)got[i].start, (size_t)(got[i].end - got[i].start), prot & ~PROT_WRITE);
    }
    pthread_mutex_unlock(&g_pm_lock);
}

// A store into a still-shared page of a private mapping: give this host page
// its own copy and let the store retry. False when the address is not ours
// or the guest never asked to write there (a real fault).
bool lxrt_privmap_handle_fault(uint64_t fault_addr, bool is_write)
{
    if (!is_write || !g_npm)
        return false;
    uint64_t page = LXRT_ALIGN_DOWN(fault_addr, LXRT_HOST_PAGE);
    pthread_mutex_lock(&g_pm_lock);
    int want = -1;
    for (int i = 0; i < g_npm; i++)
        if (fault_addr >= g_pm[i].start && fault_addr < g_pm[i].end) {
            want = g_pm[i].want;
            break;
        }
    if (want < 0 || !(want & PROT_WRITE)) {
        pthread_mutex_unlock(&g_pm_lock);
        return false;
    }
    bool ok = false;
    void *tmp = mmap(NULL, LXRT_HOST_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (tmp != MAP_FAILED) {
        memcpy(tmp, (const void *)(uintptr_t)page, LXRT_HOST_PAGE);
        if (mmap((void *)(uintptr_t)page, LXRT_HOST_PAGE, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) != MAP_FAILED) {
            memcpy((void *)(uintptr_t)page, tmp, LXRT_HOST_PAGE);
            mprotect((void *)(uintptr_t)page, LXRT_HOST_PAGE, want);
            cut(page, page + LXRT_HOST_PAGE);
            ok = true;
        }
        munmap(tmp, LXRT_HOST_PAGE);
    }
    pthread_mutex_unlock(&g_pm_lock);
    return ok;
}
