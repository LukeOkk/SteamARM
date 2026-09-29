// Read-write-execute memory for NATIVE aarch64 guests: a per-page W^X split.
//
// V8 (the Steam client's webhelper renderers, stage 22 E11/E17) reserves its
// code range PROT_NONE and then makes the whole of it read-write-execute with
// one mprotect (MEASURED, zygote trace: mprotect(0x...76940000, 0x10000000,
// 7) followed by madvise(DONTNEED) of the same range), writes code into it
// and runs it. Darwin refuses RWX on ordinary memory, and before this file
// the runtime granted such a request as read-write (right for x86 V8 under
// FEX, whose code the host never executes); a native renderer then executed a
// non-executable page and died on its first JIT'd call.
//
// Why not MAP_JIT and jit.c's per-thread switch. MAP_JIT cannot be applied to
// an existing range in place, but that is the small problem. The large one is
// the rule that no executable page may hold an unvalidated `svc`: jit.c
// rescans what a thread wrote when THAT thread flips back to execute, and an
// unpatched JIT tells it nothing about what was written. A thread in write
// mode can write any MAP_JIT page without faulting, and every other thread in
// execute mode can run that page at the same moment without faulting either --
// V8's background compiler and GC threads write code the main thread then
// runs, and those threads never flip back at all. Per-thread modes cannot see
// that. Page protections can: they are process-wide.
//
// So a range the guest made RWX is recorded here and each 16 KiB host page of
// it is, at any moment, either
//   read-write    (written since it was last checked; not executable), or
//   read-execute  (checked since it was last written; not writable).
// A store into a read-execute page faults: the page becomes read-write. An
// instruction fetch from a read-write page faults: the page is made
// READ-ONLY first (so no thread can store into it any more), scanned for the
// words the rewriter must replace (svc, TLS, CTR_EL0 and ID-register reads),
// rewritten if it holds any, re-scanned read-only until clean, and only then
// made read-execute. There is no window in which a page is executable with
// bytes nobody has scanned, whichever thread wrote them. A page nobody ever
// executes from never pays anything: it simply stays read-write, which is all
// the old grant ever gave.
//
// Costs, stated: one fault per write-after-execute and one per
// execute-after-write, per host page; a thread storing into the very page it
// is executing from (self-modifying code on one 16 KiB page) never makes
// progress -- the same known limit as the sub-page split (tests/elf/run.sh,
// subpage4k); and rewritten words are PC-relative branches, so JIT code that
// moves a rewritten site elsewhere (V8's code compaction) breaks it. V8 emits
// none of those words: x18 is excluded from its allocatable registers
// (UPSTREAM DOCUMENTED, v8 src/codegen/arm64/register-arm64.h) and generated
// code reaches the OS only through C++ -- the scan is the guarantee, not the
// expected workload. /proc/self/maps shows the host protection (rw-p or
// r-xp), not rwxp.
//
// FEX keeps the old read-write grant: the x86 code it hands to mprotect is
// data to the host, and executing it would be wrong. The main program's name
// decides (lxrt_wx_set_program); LXRT_WX_SPLIT=0/1 overrides.

#include "lxrt.h"
#include "x18.h"

#include <errno.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

bool lxrt_trace_on(void);
size_t lxrt_rewrite_count(uint64_t start, uint64_t end);

// ------------------------------------------------------------- the decision

static int g_enabled = -1;      // -1: not decided yet (treated as off)

void lxrt_wx_set_program(const char *path)
{
    const char *e = getenv("LXRT_WX_SPLIT");
    if (e && *e) {
        g_enabled = *e != '0';
        return;
    }
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path ? path : "";
    // The emulator itself: /usr/lib/lxrt-emu/FEX, FEX-gb, FEXInterpreter,
    // FEXLoader. FEXServer and the config tools are ordinary native programs.
    bool fex = !strncmp(base, "FEX", 3) && strncmp(base, "FEXServer", 9) &&
               strncmp(base, "FEXGetConfig", 12) && strncmp(base, "FEXConfig", 9) &&
               strncmp(base, "FEXRootFSFetcher", 16);
    g_enabled = !fex;
}

bool lxrt_wx_enabled(void) { return g_enabled > 0; }

// ------------------------------------------------------------- the table
//
// Guest ranges the guest asked to be read-write-execute, sorted, disjoint,
// adjacent ones merged: V8's single 256 MiB mprotect is one entry, and a JIT
// that commits thousands of separate chunks costs one entry per run of chunks.

struct wxr { uint64_t start, end; };
static struct wxr *g_r;
static int g_cap;
static _Atomic int g_n;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(wxsplit_g_lock, g_lock)

// Every path that holds g_lock runs with the asynchronous signals blocked: the
// runtime runs a guest's handler nested inside the host handler, and a guest
// handler that ran JIT code on a page this thread was in the middle of
// flipping would wait for this thread's own lock forever.
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
    pthread_mutex_unlock(&g_lock);
    pthread_sigmask(SIG_SETMASK, old, NULL);
}

// First entry whose end is above addr. Caller holds g_lock.
static int lower(uint64_t addr)
{
    int lo = 0, hi = atomic_load(&g_n);
    while (lo < hi) {
        int mid = (lo + hi) / 2;
        if (g_r[mid].end <= addr) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

static bool find_locked(uint64_t addr)
{
    int i = lower(addr);
    return i < atomic_load(&g_n) && g_r[i].start <= addr;
}

static bool reserve(int extra)
{
    int n = atomic_load(&g_n);
    if (n + extra <= g_cap)
        return true;
    int nc = g_cap ? g_cap * 2 : 256;
    while (nc < n + extra)
        nc *= 2;
    struct wxr *p = realloc(g_r, (size_t)nc * sizeof *p);
    if (!p)
        return false;
    g_r = p;
    g_cap = nc;
    return true;
}

// Remove [start, end) from the table. Caller holds g_lock.
static void remove_locked(uint64_t start, uint64_t end)
{
    int n = atomic_load(&g_n);
    int i = lower(start);
    while (i < n && g_r[i].start < end) {
        if (g_r[i].start < start && g_r[i].end > end) {       // split in two
            if (!reserve(1))
                return;
            memmove(&g_r[i + 2], &g_r[i + 1], (size_t)(n - i - 1) * sizeof *g_r);
            g_r[i + 1].start = end;
            g_r[i + 1].end = g_r[i].end;
            g_r[i].end = start;
            atomic_store(&g_n, n + 1);
            return;
        }
        if (g_r[i].start < start) {                            // keep the head
            g_r[i].end = start;
            i++;
        } else if (g_r[i].end > end) {                         // keep the tail
            g_r[i].start = end;
            return;
        } else {                                               // wholly inside
            memmove(&g_r[i], &g_r[i + 1], (size_t)(n - i - 1) * sizeof *g_r);
            atomic_store(&g_n, --n);
        }
    }
}

// Add [start, end), merging with whatever it touches. Caller holds g_lock.
static bool add_locked(uint64_t start, uint64_t end)
{
    int n = atomic_load(&g_n);
    int i = lower(start);
    if (i > 0 && g_r[i - 1].end == start)       // touches the one below
        i--;
    int j = i;
    while (j < n && g_r[j].start <= end) {      // everything it overlaps or touches
        if (g_r[j].start < start) start = g_r[j].start;
        if (g_r[j].end > end) end = g_r[j].end;
        j++;
    }
    if (j > i) {                                // replace [i, j) by one entry
        g_r[i].start = start;
        g_r[i].end = end;
        memmove(&g_r[i + 1], &g_r[j], (size_t)(n - j) * sizeof *g_r);
        atomic_store(&g_n, n - (j - i - 1));
        return true;
    }
    if (!reserve(1))
        return false;
    memmove(&g_r[i + 1], &g_r[i], (size_t)(n - i) * sizeof *g_r);
    g_r[i].start = start;
    g_r[i].end = end;
    atomic_store(&g_n, n + 1);
    return true;
}

bool lxrt_wx_contains(uint64_t addr)
{
    if (!atomic_load(&g_n))
        return false;
    sigset_t old;
    lock_nosig(&old);
    bool r = find_locked(addr);
    unlock_nosig(&old);
    return r;
}

// Is every byte of [addr, addr+len) in the table?
bool lxrt_wx_covered(uint64_t addr, uint64_t len)
{
    if (!atomic_load(&g_n) || !len)
        return false;
    sigset_t old;
    lock_nosig(&old);
    int i = lower(addr);
    bool r = i < atomic_load(&g_n) && g_r[i].start <= addr && g_r[i].end >= addr + len;
    unlock_nosig(&old);
    return r;
}

int lxrt_wx_count(void) { return atomic_load(&g_n); }

void lxrt_wx_forget(uint64_t addr, uint64_t len)
{
    if (!atomic_load(&g_n) || !len)
        return;
    sigset_t old;
    lock_nosig(&old);
    remove_locked(addr, addr + len);
    unlock_nosig(&old);
}

// ------------------------------------------------------------- statistics
//
// LXRT_WX_STATS=1: the flips and scans this process made, every 5 s while it
// faults and at exit, on the runtime's stream. LXRT_WX_STATS=/host/path
// appends them to that file instead: a webhelper renderer's stderr goes
// nowhere anyone reads.
static _Atomic uint64_t st_write, st_exec, st_scan_sites, st_rewritten, st_x18w;
static _Atomic uint64_t st_last;
static int stats_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("LXRT_WX_STATS") ? 1 : 0;
    return on;
}
static void stats_print(const char *when)
{
    char line[320];
    int n = snprintf(line, sizeof line, "[lxrt] pid %d wxsplit%s: %d ranges, %llu write flips, %llu exec flips, "
                     "%llu scans with sites, %llu words rewritten, %llu x18-naming words seen\n",
                     (int)getpid(), when, atomic_load(&g_n),
                     (unsigned long long)atomic_load(&st_write), (unsigned long long)atomic_load(&st_exec),
                     (unsigned long long)atomic_load(&st_scan_sites), (unsigned long long)atomic_load(&st_rewritten),
                     (unsigned long long)atomic_load(&st_x18w));
    const char *e = getenv("LXRT_WX_STATS");
    int fd = e && *e == '/' ? open(e, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644) : -1;
    if (fd >= 0) {
        (void)!write(fd, line, (size_t)n);
        close(fd);
    } else {
        fputs(line, lxrt_trace_stream());
    }
}
// With LXRT_WX_STATS=/path, the first 64 x18-naming words a process scans,
// with 4 words either side: instruction or data is for a disassembler to say.
static void stats_word(uint64_t a, uint64_t lo, uint64_t hi)
{
    static _Atomic int said;
    const char *e = getenv("LXRT_WX_STATS");
    if (!e || *e != '/' || atomic_fetch_add(&said, 1) >= 64)
        return;
    char line[256];
    int n = snprintf(line, sizeof line, "[lxrt] pid %d x18 word at 0x%llx:", (int)getpid(),
                     (unsigned long long)a);
    for (int d = -4; d <= 4; d++) {
        uint64_t q = a + (uint64_t)(int64_t)(d * 4);
        if (q < lo || q + 4 > hi)
            continue;
        n += snprintf(line + n, sizeof line - (size_t)n, d ? " %08x" : " [%08x]",
                      *(const uint32_t *)(uintptr_t)q);
    }
    n += snprintf(line + n, sizeof line - (size_t)n, "\n");
    int fd = open(e, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
    if (fd >= 0) {
        (void)!write(fd, line, (size_t)n);
        close(fd);
    }
}

static void stats_tick(void)
{
    if (!stats_on())
        return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t last = atomic_load(&st_last);
    if (now - last > 5000000000ull && atomic_compare_exchange_strong(&st_last, &last, now))
        stats_print("");
}
void lxrt_wx_stats_flush(void)
{
    if (stats_on() && (atomic_load(&st_write) || atomic_load(&st_exec) || atomic_load(&g_n)))
        stats_print(" at exit");
}

// ------------------------------------------------------------- scanning

// Make the host page [hpage, hpage+LXRT_HOST_PAGE) read-only, and make the
// given guest ranges inside it free of anything the rewriter replaces. On
// return (true) the page is still read-only and clean: the caller applies the
// executable protection, and between the two nothing can have stored into it.
// Also used by subpage.c for host pages shared with other guest mappings.
bool lxrt_wx_scan_for_exec(uint64_t hpage, const struct lxrt_range *r, int nr)
{
    for (int round = 0; round < 64; round++) {
        if (mprotect((void *)(uintptr_t)hpage, LXRT_HOST_PAGE, PROT_READ) != 0)
            return false;
        size_t sites = 0;
        for (int k = 0; k < nr; k++)
            sites += lxrt_rewrite_count(r[k].start, r[k].end);
        if (stats_on() && round == 0)
            for (int k = 0; k < nr; k++)
                for (uint64_t a = r[k].start & ~3ull; a + 4 <= r[k].end; a += 4)
                    if (lxrt_x18_touches(*(const uint32_t *)(uintptr_t)a)) {
                        atomic_fetch_add(&st_x18w, 1);
                        stats_word(a, r[k].start, r[k].end);
                    }
        if (!sites)
            return true;
        // Something to replace: open the page, rewrite, and count again from
        // read-only -- another thread may have stored while it was open.
        if (mprotect((void *)(uintptr_t)hpage, LXRT_HOST_PAGE, PROT_READ | PROT_WRITE) != 0)
            return false;
        atomic_fetch_add(&st_scan_sites, 1);
        for (int k = 0; k < nr; k++) {
            struct lxrt_rewrite_report rep;
            char *err = NULL;
            if (lxrt_rewrite_range(r[k].start, r[k].end, &rep, &err) != 0) {
                fprintf(lxrt_trace_stream(), "[lxrt] JIT output at 0x%llx: rewrite failed: %s\n",
                        (unsigned long long)r[k].start, err ? err : "?");
                free(err);
                continue;
            }
            size_t done = rep.sites_rewritten + rep.tls_rewritten + rep.ctr_rewritten +
                          rep.sysreg_rewritten;
            atomic_fetch_add(&st_rewritten, done);
            // Same words as jit.c's rescan line, so one grep finds both.
            fprintf(lxrt_trace_stream(), "[lxrt] JIT output: %zu svc sites rewritten, %zu poisoned "
                    "(W^X page 0x%llx: tls %zu, ctr %zu, sysreg %zu)\n",
                    rep.sites_rewritten, rep.sites_unreachable + rep.tls_unreachable,
                    (unsigned long long)hpage, rep.tls_rewritten, rep.ctr_rewritten,
                    rep.sysreg_rewritten);
        }
    }
    return false;
}

static int host_prot(uint64_t addr)
{
    mach_vm_address_t ra = addr;
    mach_vm_size_t rs = 0;
    vm_region_basic_info_data_64_t ri;
    mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS || ra > addr)
        return -1;
    return (ri.protection & VM_PROT_READ ? PROT_READ : 0) |
           (ri.protection & VM_PROT_WRITE ? PROT_WRITE : 0) |
           (ri.protection & VM_PROT_EXECUTE ? PROT_EXEC : 0);
}

// Private anonymous memory the guest owns: not a file (rewriting would be a
// private copy at best), not shared with another process or view (a rewrite
// would show through), not host memory.
static bool anon_private(uint64_t addr, uint64_t len)
{
    uint64_t p = addr, end = addr + len;
    while (p < end) {
        mach_vm_address_t ra = p;
        mach_vm_size_t rs = 0;
        vm_region_extended_info_data_t xi;
        mach_msg_type_number_t xc = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_EXTENDED_INFO,
                           (vm_region_info_t)&xi, &xc, &obj) != KERN_SUCCESS || ra > p)
            return false;           // a hole: mprotect would fail anyway
        if (xi.external_pager || xi.user_tag != 0 ||
            xi.share_mode == SM_TRUESHARED || xi.share_mode == SM_SHARED_ALIASED ||
            xi.share_mode == SM_LARGE_PAGE)
            return false;
        if (xi.share_mode == SM_SHARED) {
            // SM_SHARED is also what a private region reports while a fork
            // child still shares its pages; only a MAP_SHARED region says so.
            mach_vm_address_t ba = p;
            mach_vm_size_t bs = 0;
            vm_region_basic_info_data_64_t bi;
            mach_msg_type_number_t bc = VM_REGION_BASIC_INFO_COUNT_64;
            if (mach_vm_region(mach_task_self(), &ba, &bs, VM_REGION_BASIC_INFO_64,
                               (vm_region_info_t)&bi, &bc, &obj) != KERN_SUCCESS || bi.shared)
                return false;
        }
        p = ra + rs;
    }
    return true;
}

// ------------------------------------------------------------- mprotect

// The guest asked for read-write-execute on [addr, addr+len), host-page
// aligned. Returns 1 when handled here (*ret set), 0 when the caller should
// fall back to its old behaviour (not private anonymous memory).
int lxrt_wx_protect(uint64_t addr, uint64_t len, long *ret)
{
    if (!lxrt_wx_enabled() || !len || !anon_private(addr, len))
        return 0;
    sigset_t old;
    lock_nosig(&old);
    if (!add_locked(addr, addr + len)) {
        unlock_nosig(&old);
        *ret = -12;                 // ENOMEM
        return 1;
    }
    // Read-write first: a guest that asks for write is about to write, and a
    // page nobody executes never has to be scanned. Execute comes on the
    // first instruction fetch, through lxrt_wx_handle_fault.
    if (mprotect((void *)(uintptr_t)addr, (size_t)len, PROT_READ | PROT_WRITE) != 0) {
        int e = errno;
        remove_locked(addr, addr + len);
        unlock_nosig(&old);
        *ret = -lxrt_errno_to_linux(e);
        return 1;
    }
    unlock_nosig(&old);
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] mprotect 0x%llx+0x%llx RWX: W^X split (%d ranges)\n",
                (unsigned long long)addr, (unsigned long long)len, atomic_load(&g_n));
    *ret = 0;
    return 1;
}

// mremap moved [old, old+olen) to [neu, neu+nlen): the new range keeps the
// guest's RWX, the pages keep their host protection.
void lxrt_wx_moved(uint64_t old, uint64_t olen, uint64_t neu, uint64_t nlen)
{
    if (!atomic_load(&g_n))
        return;
    bool was = lxrt_wx_covered(old, olen);
    lxrt_wx_forget(old, olen);
    if (!was || !nlen)
        return;
    // The moved pages carry their host protections with them: a read-execute
    // one was scanned where it was, and holds the same bytes now.
    sigset_t o;
    lock_nosig(&o);
    add_locked(neu, neu + nlen);
    unlock_nosig(&o);
}

// ------------------------------------------------------------- faults

// Last flips of this thread, to name a page that never makes progress.
static _Thread_local uint64_t t_last_hp, t_last_pc;
static _Thread_local unsigned t_pingpong;

bool lxrt_wx_handle_fault(uint64_t pc, uint64_t addr, uint32_t esr)
{
    if (!atomic_load(&g_n) || !addr)
        return false;
    uint32_t ec = esr >> 26;
    bool fetch = ec == 0x20 || ec == 0x21 || pc == addr;
    bool write = !fetch && (ec == 0x24 || ec == 0x25) && (esr & (1u << 6));
    if (!fetch && !write)
        return false;
    uint64_t hp = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);

    sigset_t old;
    lock_nosig(&old);
    if (!find_locked(addr)) {
        unlock_nosig(&old);
        return false;
    }
    int cur = host_prot(hp);
    bool ok = true;
    if (fetch) {
        if (cur < 0 || !(cur & PROT_EXEC)) {
            // Only the part of the host page the guest made RWX is scanned;
            // the table's ranges are host-page aligned where they came from
            // an aligned mprotect, but a partial munmap can trim them.
            struct lxrt_range r[4];
            int nr = 0;
            int n = atomic_load(&g_n);
            for (int i = lower(hp); i < n && g_r[i].start < hp + LXRT_HOST_PAGE && nr < 4; i++) {
                r[nr].start = g_r[i].start > hp ? g_r[i].start : hp;
                r[nr].end = g_r[i].end < hp + LXRT_HOST_PAGE ? g_r[i].end : hp + LXRT_HOST_PAGE;
                nr++;
            }
            ok = lxrt_wx_scan_for_exec(hp, r, nr) &&
                 mprotect((void *)(uintptr_t)hp, LXRT_HOST_PAGE, PROT_READ | PROT_EXEC) == 0;
            if (ok) {
                sys_icache_invalidate((void *)(uintptr_t)hp, LXRT_HOST_PAGE);
                atomic_fetch_add(&st_exec, 1);
            }
        }
    } else if (cur < 0 || !(cur & PROT_WRITE)) {
        ok = mprotect((void *)(uintptr_t)hp, LXRT_HOST_PAGE, PROT_READ | PROT_WRITE) == 0;
        if (ok)
            atomic_fetch_add(&st_write, 1);
    }
    unlock_nosig(&old);

    // A store executed from the page it stores into: each flip undoes the
    // other. Say so once; the thread will keep faulting (see the header).
    if (write && LXRT_ALIGN_DOWN(pc, LXRT_HOST_PAGE) == hp) {
        if (t_last_hp == hp && t_last_pc == pc) {
            if (++t_pingpong == 1000)
                fprintf(lxrt_trace_stream(), "[lxrt] pid %d: store at pc 0x%llx into its own "
                        "16 KiB page 0x%llx flips W^X forever (self-modifying code on one host page)\n",
                        (int)getpid(), (unsigned long long)pc, (unsigned long long)hp);
        } else {
            t_last_hp = hp;
            t_last_pc = pc;
            t_pingpong = 0;
        }
    }
    stats_tick();
    if (!ok && lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] W^X %s flip of 0x%llx failed: %s\n",
                fetch ? "execute" : "write", (unsigned long long)hp, strerror(errno));
    return ok;
}
