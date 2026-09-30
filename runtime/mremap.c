// Host-page mremap for anonymous memory. See mremap.h for guest limitations.
#include "lxrt.h"
#include "mremap.h"

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/fileport.h>
#include <sys/mman.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))
enum { L_MAYMOVE = 1, L_FIXED = 2, L_DONTUNMAP = 4 };

// ------------------------------------------------ shared file mappings
//
// Growing a MAP_SHARED file mapping has to map MORE OF THE FILE: anonymous
// pages would silently stop sharing. Linux finds the file from the VMA; here
// nothing in the address space leads back to it (an unlinked memfd has no
// path), so dispatch.c records each such mapping with a Mach fileport of its
// descriptor. A fileport keeps the open file alive without taking a
// descriptor number the guest could see, close or collide with.
//
// Why it matters (MEASURED, benchmarks/stage27-android-display.txt): every
// Wayland compositor built with libwayland-server grows a client's wl_shm
// pool with mremap(MREMAP_MAYMOVE) when the client sends wl_shm_pool.resize.
// Weston under lxrun killed its own desktop shell with "wl_shm_pool#3:
// error 2: failed mremap" before this.
enum { SHF_MAX = 512 };
struct shf { uint64_t start, end, off; int prot; mach_port_t port; };
static struct shf g_shf[SHF_MAX];
static int g_nshf;
static pthread_mutex_t g_shf_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(mremap_shf, g_shf_lock)
// Mach port rights are not inherited across fork: the child's records name
// nothing. Its inherited mappings are simply not growable (as before).
static void shf_child(void) { g_nshf = 0; }
__attribute__((constructor(201))) static void shf_register(void)
{
    pthread_atfork(NULL, NULL, shf_child);
}

static void shf_drop(int i)
{
    if (g_shf[i].port != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), g_shf[i].port);
    g_shf[i] = g_shf[--g_nshf];
}

// Caller holds g_shf_lock. Trims, splits or drops what [a, b) covers.
static void shf_forget_locked(uint64_t a, uint64_t b)
{
    for (int i = 0; i < g_nshf; ) {
        struct shf *e = &g_shf[i];
        if (b <= e->start || a >= e->end) { i++; continue; }
        if (a <= e->start && b >= e->end) { shf_drop(i); continue; }
        if (a > e->start && b < e->end) {
            // The middle goes: the tail becomes a record of its own, holding
            // its own reference to the file.
            if (g_nshf < SHF_MAX &&
                mach_port_mod_refs(mach_task_self(), e->port, MACH_PORT_RIGHT_SEND, 1) == KERN_SUCCESS) {
                g_shf[g_nshf++] = (struct shf){ b, e->end, e->off + (b - e->start), e->prot, e->port };
                e = &g_shf[i];
            }
            e->end = a;
        } else if (a <= e->start) {
            e->off += b - e->start;
            e->start = b;
        } else {
            e->end = a;
        }
        i++;
    }
}

void lxrt_mremap_note_shared(uint64_t addr, uint64_t len, int fd, uint64_t off, int prot)
{
    if (!len || fd < 0 || addr % LXRT_HOST_PAGE || off % LXRT_HOST_PAGE)
        return;
    uint64_t end = addr + LXRT_ALIGN_UP(len, LXRT_HOST_PAGE);
    mach_port_t port = MACH_PORT_NULL;
    if (fileport_makeport(fd, &port) != 0)
        return;
    pthread_mutex_lock(&g_shf_lock);
    shf_forget_locked(addr, end);
    if (g_nshf < SHF_MAX) {
        g_shf[g_nshf++] = (struct shf){ addr, end, off, prot, port };
        port = MACH_PORT_NULL;
    }
    pthread_mutex_unlock(&g_shf_lock);
    if (port != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), port);
}

void lxrt_mremap_forget_shared(uint64_t addr, uint64_t len)
{
    if (!len || !g_nshf)
        return;
    uint64_t a = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
    uint64_t b = LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE);
    pthread_mutex_lock(&g_shf_lock);
    shf_forget_locked(a, b);
    pthread_mutex_unlock(&g_shf_lock);
}

// Is [a, b) free (nothing mapped)? Probed by reserving it.
static bool range_free(uint64_t a, uint64_t len)
{
    mach_vm_address_t at = a;
    if (mach_vm_allocate(mach_task_self(), &at, len, VM_FLAGS_FIXED) != KERN_SUCCESS)
        return false;
    mach_vm_deallocate(mach_task_self(), at, len);
    return true;
}

// mremap of a recorded shared file mapping; lengths are host-page rounded.
// Returns the new address, a negative Linux errno, or 1 when the range is
// not (wholly) a recorded mapping.
static long remap_shared_file(uint64_t old_addr, uint64_t old_len, uint64_t new_len,
                              int lflags, uint64_t new_addr, int prot)
{
    pthread_mutex_lock(&g_shf_lock);
    struct shf rec = { 0 };
    bool found = false;
    for (int i = 0; i < g_nshf && !found; i++)
        if (g_shf[i].start <= old_addr && old_addr + old_len <= g_shf[i].end) {
            rec = g_shf[i];
            found = true;
        }
    if (!found || (lflags & L_DONTUNMAP)) {
        pthread_mutex_unlock(&g_shf_lock);
        return 1;
    }
    int fd = fileport_makefd(rec.port);
    pthread_mutex_unlock(&g_shf_lock);
    if (fd < 0)
        return 1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    uint64_t off = rec.off + (old_addr - rec.start);
    int mprot = prot & (PROT_READ | PROT_WRITE);
    long ret;
    // Linux tries to grow in place first, MAYMOVE or not.
    uint64_t tail = old_addr + old_len, delta = new_len - old_len;
    if (!(lflags & L_FIXED) && new_len > old_len && tail <= UINT64_MAX - delta &&
        range_free(tail, delta)) {
        void *p = mmap((void *)tail, delta, mprot, MAP_SHARED | MAP_FIXED, fd, (off_t)(off + old_len));
        if (p == (void *)tail) {
            lxrt_mremap_note_shared(old_addr, new_len, fd, off, mprot);
            close(fd);
            return (long)old_addr;
        }
        if (p != MAP_FAILED)
            munmap(p, delta);
    }
    if (!(lflags & L_MAYMOVE)) {
        close(fd);
        return LERR(ENOMEM);
    }
    void *dest = (lflags & L_FIXED)
        ? mmap((void *)new_addr, new_len, mprot, MAP_SHARED | MAP_FIXED, fd, (off_t)off)
        : mmap(NULL, new_len, mprot, MAP_SHARED, fd, (off_t)off);
    if (dest == MAP_FAILED) {
        ret = LERR(errno);
        close(fd);
        return ret;
    }
    if (munmap((void *)old_addr, old_len) != 0) {
        ret = LERR(errno);
        munmap(dest, new_len);
        close(fd);
        return ret;
    }
    lxrt_mremap_forget_shared(old_addr, old_len);
    lxrt_mremap_note_shared((uint64_t)(uintptr_t)dest, new_len, fd, off, mprot);
    close(fd);
    return (long)(uintptr_t)dest;
}

// mach_vm_region returns the NEXT region for a hole, not necessarily the
// region containing the address. Check containment and release its send right.
static bool region(uint64_t addr, uint64_t *end,
                   vm_region_basic_info_data_64_t *info)
{
    mach_vm_address_t base = addr;
    mach_vm_size_t size = 0;
    mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t object = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &base, &size,
        VM_REGION_BASIC_INFO_64, (vm_region_info_t)info, &count, &object);
    if (object != MACH_PORT_NULL)
        mach_port_deallocate(mach_task_self(), object);
    if (kr != KERN_SUCCESS || base > addr || size > UINT64_MAX - base ||
        addr >= base + size)
        return false;
    *end = base + size;
    return true;
}

static void *staging(uint64_t len, bool fixed, uint64_t target)
{
    // A free FIXED target can itself be selected by mmap(NULL). Hold colliding
    // allocations until we have a disjoint one. A target of length len can
    // intersect at most two disjoint allocations of that same length.
    void *held[3];
    int n = 0;
    void *result = MAP_FAILED;
    int e = ENOMEM;
    while (n < 3) {
        void *p = mmap(NULL, len, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANON, -1, 0);
        if (p == MAP_FAILED) { e = errno; break; }
        uint64_t at = (uint64_t)p;
        if (!fixed || at >= target + len || target >= at + len) {
            result = p;
            break;
        }
        held[n++] = p;
    }
    while (n) munmap(held[--n], len);
    if (result == MAP_FAILED) errno = e;
    return result;
}

int lxrt_mremap_copy_out(void *dst, uint64_t src, uint64_t len)
{
    if (!len)
        return 0;
    // A private copy-on-write alias of the enclosing host pages, made
    // readable: the source may be PROT_NONE or execute-only to the guest.
    uint64_t hs = LXRT_ALIGN_DOWN(src, LXRT_HOST_PAGE);
    uint64_t he = LXRT_ALIGN_UP(src + len, LXRT_HOST_PAGE);
    mach_vm_address_t snap = 0;
    vm_prot_t cur, max;
    if (mach_vm_remap(mach_task_self(), &snap, he - hs, 0, VM_FLAGS_ANYWHERE,
                      mach_task_self(), hs, TRUE, &cur, &max, VM_INHERIT_NONE) != KERN_SUCCESS)
        return -1;
    int rc = -1;
    if (mprotect((void *)snap, he - hs, PROT_READ) == 0) {
        memcpy(dst, (void *)(snap + (src - hs)), len);
        rc = 0;
    }
    mach_vm_deallocate(mach_task_self(), snap, he - hs);
    return rc;
}

int lxrt_mremap_host_prot(uint64_t addr, bool *shared)
{
    vm_region_basic_info_data_64_t info;
    uint64_t end;
    if (!region(addr, &end, &info))
        return -1;
    if (shared)
        *shared = info.shared;
    return (info.protection & VM_PROT_READ ? PROT_READ : 0)
         | (info.protection & VM_PROT_WRITE ? PROT_WRITE : 0)
         | (info.protection & VM_PROT_EXECUTE ? PROT_EXEC : 0);
}

long lxrt_mremap(uint64_t old_addr, uint64_t old_len,
                 uint64_t new_len, int lflags, uint64_t new_addr)
{
    if (!old_len || !new_len || (lflags & ~7) ||
        (old_addr % LXRT_HOST_PAGE) ||
        ((lflags & (L_FIXED | L_DONTUNMAP)) && !(lflags & L_MAYMOVE)))
        return LERR(EINVAL);
    if (old_len > UINT64_MAX - (LXRT_HOST_PAGE - 1) ||
        new_len > UINT64_MAX - (LXRT_HOST_PAGE - 1))
        return LERR(ENOMEM);
    old_len = LXRT_ALIGN_UP(old_len, LXRT_HOST_PAGE);
    new_len = LXRT_ALIGN_UP(new_len, LXRT_HOST_PAGE);
    if (old_addr > UINT64_MAX - old_len)
        return LERR(EFAULT);
    if ((lflags & L_DONTUNMAP) && old_len != new_len)
        return LERR(EINVAL);
    if (lflags & L_FIXED) {
        if (new_addr % LXRT_HOST_PAGE || new_addr > UINT64_MAX - new_len ||
            (new_addr < old_addr + old_len && old_addr < new_addr + new_len))
            return LERR(EINVAL);
    }
    if (lxrt_subpage_tracked(old_addr, old_len) ||
        ((lflags & L_FIXED) && lxrt_subpage_tracked(new_addr, new_len))) {
        fprintf(lxrt_trace_stream(), "[lxrt] mremap: sub-page mapping not supported yet\n");
        return LERR(ENOMEM);
    }

    vm_region_basic_info_data_64_t first, info;
    uint64_t end;
    if (!region(old_addr, &end, &first))
        return LERR(EFAULT);
    bool file_or_shared = false;
    // Validate the entire source before altering either mapping. Different
    // protections need per-region relocation and are not supported yet.
    for (uint64_t at = old_addr; at < old_addr + old_len; at = end) {
        if (!region(at, &end, &info))
            return LERR(EFAULT);
        if (info.protection != first.protection)
            return LERR(EFAULT);
        char path[PROC_PIDPATHINFO_MAXSIZE];
        // proc_regionfilename can return the NEXT file-backed region (e.g.
        // dyld for a preceding anonymous allocation). Confirm containment.
        struct proc_regionwithpathinfo file;
        bool backed = false;
        if (proc_regionfilename(getpid(), at, path, sizeof path) > 0 &&
            proc_pidinfo(getpid(), PROC_PIDREGIONPATHINFO, at, &file,
                         sizeof file) == sizeof file) {
            uint64_t base = file.prp_prinfo.pri_address;
            backed = base <= at && at - base < file.prp_prinfo.pri_size &&
                     file.prp_vip.vip_path[0] != '\0';
        }
        if (info.shared || backed)
            file_or_shared = true;
    }
    int prot = first.protection;
    if (!(lflags & (L_FIXED | L_DONTUNMAP))) {
        if (new_len <= old_len) {
            if (new_len < old_len &&
                munmap((void *)(old_addr + new_len), old_len - new_len) != 0)
                return LERR(errno);
            if (new_len < old_len)
                lxrt_mremap_forget_shared(old_addr + new_len, old_len - new_len);
            return (long)old_addr;
        }
        // Extending a file with anonymous pages would lose backing semantics.
        if (!file_or_shared && old_addr <= UINT64_MAX - new_len) {
            mach_vm_address_t tail = old_addr + old_len;
            uint64_t delta = new_len - old_len;
            kern_return_t kr = mach_vm_allocate(mach_task_self(), &tail, delta,
                                                VM_FLAGS_FIXED);
            if (kr == KERN_SUCCESS) {
                void *p = mmap((void *)tail, delta, prot,
                              MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
                if (p == MAP_FAILED) {
                    int e = errno;
                    mach_vm_deallocate(mach_task_self(), tail, delta);
                    return LERR(e);
                }
                return (long)old_addr;
            }
            if (kr != KERN_NO_SPACE)
                return LERR(ENOMEM);
        }
        // A shared file mapping may still grow in place (remap_shared_file).
        if (!(lflags & L_MAYMOVE) && !file_or_shared)
            return LERR(ENOMEM);
    }
    if (file_or_shared) {
        long r = remap_shared_file(old_addr, old_len, new_len, lflags, new_addr, prot);
        if (r != 1)
            return r;
        fprintf(lxrt_trace_stream(), "[lxrt] mremap: file-backed range 0x%llx..0x%llx: not supported yet\n",
                (unsigned long long)old_addr,
                (unsigned long long)(old_addr + old_len));
        return LERR(ENOMEM);
    }

    void *dest = staging(new_len, (lflags & L_FIXED) != 0, new_addr);
    if (dest == MAP_FAILED)
        return LERR(errno);
    uint64_t copied = old_len < new_len ? old_len : new_len;
    // A private Mach alias lets memcpy read even PROT_NONE/execute-only
    // sources without temporarily changing the source's access permissions.
    mach_vm_address_t snapshot = 0;
    vm_prot_t current, maximum;
    kern_return_t kr = mach_vm_remap(mach_task_self(), &snapshot, copied, 0,
        VM_FLAGS_ANYWHERE, mach_task_self(), old_addr, TRUE,
        &current, &maximum, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS) {
        munmap(dest, new_len);
        return LERR(ENOMEM);
    }
    if (mprotect((void *)snapshot, copied, PROT_READ) != 0) {
        int e = errno;
        mach_vm_deallocate(mach_task_self(), snapshot, copied);
        munmap(dest, new_len);
        return LERR(e);
    }
    memcpy(dest, (void *)snapshot, copied);
    mach_vm_deallocate(mach_task_self(), snapshot, copied);
    if (mprotect(dest, new_len, prot) != 0) {
        int e = errno;
        munmap(dest, new_len);
        return LERR(e);
    }
    if (lflags & L_FIXED) {
        // Publish only after allocation/copy/protection succeed. Mach replaces
        // the destination just as MAP_FIXED does, with no unreserved gap.
        mach_vm_address_t target = new_addr;
        kr = mach_vm_remap(mach_task_self(), &target, new_len, 0,
            VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(),
            (mach_vm_address_t)dest, FALSE, &current, &maximum, VM_INHERIT_COPY);
        munmap(dest, new_len);
        if (kr != KERN_SUCCESS)
            return LERR(ENOMEM);
        dest = (void *)target;
    }
    int rc;
    if (lflags & L_DONTUNMAP) {
        // Linux leaves the source mapped with its old protection and faults
        // it back in as zero pages (unless userfaultfd intervenes). A stricter
        // PROT_NONE here scored 21/22 against the Linux reference: a guest
        // that reads the purged source expects zeros, not SIGSEGV. `current`
        // is the protection the mach_vm_remap snapshot reported for the old
        // range, so the fresh anonymous pages get exactly that.
        int oldprot = (current & VM_PROT_READ ? PROT_READ : 0)
                    | (current & VM_PROT_WRITE ? PROT_WRITE : 0)
                    | (current & VM_PROT_EXECUTE ? PROT_EXEC : 0);
        if ((oldprot & PROT_WRITE) && (oldprot & PROT_EXEC))
            oldprot &= ~PROT_EXEC;      // Darwin refuses rwx on a plain page
        rc = mmap((void *)old_addr, old_len, oldprot,
                  MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED ? -1 : 0;
    } else {
        rc = munmap((void *)old_addr, old_len);
    }
    if (rc != 0) {
        int e = errno;
        munmap(dest, new_len);
        return LERR(e);
    }
    // Mach serializes individual VM operations. As with mmap/munmap, callers
    // must serialize concurrent changes to these same address ranges.
    return (long)(uintptr_t)dest;
}
