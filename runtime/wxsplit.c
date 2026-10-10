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
//
// Lazy execute. A read-execute request can be recorded here too, as R-X
// beside the RWX ranges: its host pages become READ-ONLY, and executable at
// their first instruction fetch, through the same flip as an RWX page (made
// read-only, scanned, rewritten until clean, then read-execute). A store into
// such a page is the guest's own fault and is delivered, never flipped. It is
// for Wine under FEX's ARM64EC JIT, which write-protects the x64 code it has
// translated with VirtualProtect(PAGE_EXECUTE_READ) and lifts that again with
// PAGE_EXECUTE_READWRITE on the first store (its self-modifying-code trap):
// that code is data to the host, never fetched natively, and every trap was
// a hand-over (dispatch.c wx_leave_whole) that scanned 16 KiB, invalidated
// the icache and made three mach_vm_region and three mprotect calls -- inside
// FEX's shared CodeInvalidationMutex hold, which its invalidations wait
// behind (MEASURED, tests/win smc_bench under `sample`, 2026-10-10: 99-125 of
// each writer thread's 2,631 samples in wx_leave_whole, and another 105-166
// in the untrap's region walk, lxrt_wx_protect). A trap is now one
// mprotect(PROT_READ) and a table update: the same trap and untrap from a
// native guest, nothing fetched in between, went from 90-95 to 200-207
// thousand rounds a second; with a call into the page after each R-X, from
// 82-88 to 77-83 thousand, the first fetch's fault (MEASURED, 2026-10-10,
// tests/elf/wx_lazy_exec.c "smc", eight runs each). The scan also matched x64
// bytes as aarch64 sites (an ID-register read fixes only 20 bits) and could
// rewrite them under FEX's feet.
//
// LXRT_LAZY_EXEC=2, the default in Wine's loaders: only ranges this table
// already holds (RWX made R-X, FEX's trap). =1: every host-page aligned R-X
// mprotect of private anonymous memory (tests/elf/wx_lazy_exec.c). =0 or any
// other program: off. Not 1 by default: it would move the scan of every PE
// ARM64 page into a fault taken wherever that code first runs -- inside a
// nested guest handler too -- and the scan takes pefile.c's lock and
// allocates; an RWX page's first fetch carries that exposure already, so 2
// adds nothing new. mmap(PROT_EXEC), file and shared memory and unaligned
// ranges stay eager (rewrite_and_seal). 4 KiB-tracked ranges follow the same
// rule in subpage.c (apply_prot_fresh): a host page waits for its fetch only
// when every executable guest page in it is one FEX trapped (R-X made from
// RWX) or one this table holds lazily -- code the loader maps R-X is scanned
// when mapped. /proc/self/maps shows such a page r--p until its first fetch.
//
// One deviation, stated. A first fetch whose scan finds a word to rewrite
// opens the page read-write while it rewrites (lxrt_wx_scan_for_exec's
// second round), and a store another thread makes into that R-X page in that
// moment lands instead of faulting -- a store the guest must see refused
// (FEX's trap, a write watch) goes through once, unseen. The svc rule still
// holds: the page goes back to read-only and is counted clean again before
// it becomes executable. Nothing measured meets it: FEX's x64 pages are
// never fetched natively, and of 1,083,296 execute flips in a Minecraft
// Dungeons II session, 2 found anything to rewrite (MEASURED, LXRT_WX_STATS,
// 2026-10-09). Closing it means rewriting through a second, private view of
// the page instead of opening the guest's; not done.
//
// One owner per host page. A 16 KiB host page can hold pages of this table
// AND 4 KiB guest pages subpage.c records (a 4 KiB mprotect or MAP_FIXED
// inside an RWX range, or an RWX commit that is not 16 KiB aligned). Such a
// page is subpage.c's: when it first sees the page it records every slot of
// this table with the guest's protection, RWX or R-X (adopt_untracked), its
// flips scan every RWX and every lazily executable guest page of the page,
// and the fault handler here leaves the page alone. A page it does
// not track is this file's. Both tables, and every protection change of a
// page either of them owns, sit under ONE lock (lxrt_pageprot_lock): with a
// lock each, a write flip here and an execute flip there changed one page at
// the same time, and code in a 4 KiB RWX page only subpage.c knew about ran
// unscanned after a fetch from a neighbour that was in this table (stage 23
// review; tests/elf/wx_owner.c, "mixed").

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
static int g_lazy;              // LXRT_LAZY_EXEC (header): 0 off, 1 every R-X request, 2 held ranges

static bool g_program_is_fex;
static bool g_program_is_wine_loader;

void lxrt_wx_set_program(const char *path)
{
    const char *base = path ? strrchr(path, '/') : NULL;
    base = base ? base + 1 : path ? path : "";
    // Wine's loaders are the processes that run PE code with the TEB in x18
    // (lxrt_no_x18 below).
    g_program_is_wine_loader = !strcmp(base, "wine") || !strcmp(base, "wine64") ||
                               !strcmp(base, "wine-preloader") || !strcmp(base, "wine64-preloader");
    // The emulator itself: /usr/lib/lxrt-emu/FEX, FEX-gb, FEXInterpreter,
    // FEXLoader. FEXServer and the config tools are ordinary native programs.
    g_program_is_fex = !strncmp(base, "FEX", 3) && strncmp(base, "FEXServer", 9) &&
                       strncmp(base, "FEXGetConfig", 12) && strncmp(base, "FEXConfig", 9) &&
                       strncmp(base, "FEXRootFSFetcher", 16);
    // Lazy execute (header): FEX's trap in Wine's loaders; anything more
    // only when asked.
    const char *lz = getenv("LXRT_LAZY_EXEC");
    int lv = lz && *lz ? atoi(lz) : g_program_is_wine_loader ? 2 : 0;
    g_lazy = lv == 1 || lv == 2 ? lv : 0;
    const char *e = getenv("LXRT_WX_SPLIT");
    if (e && *e) {
        g_enabled = *e != '0';
        return;
    }
    g_enabled = !g_program_is_fex;
}

// Whether the main program is the emulator (stack.c: its AT_HWCAP).
bool lxrt_program_is_fex(void) { return g_program_is_fex; }

// LXRT_NO_X18 (set by tools/steamarm-native-proton): no x18 virtualisation,
// so Wine's PE code and FEX's ARM64EC JIT output keep the hardware register,
// the TEB, untouched. Only for Wine's loaders: every other program under the
// tool is plain ELF code, and ELF code that forks without exec needs the
// virtualisation -- a fork child loses the kernel's x18 preservation, its
// first page faults zero the register, and glibc's ld.so keeps the length of
// the symbol-lookup scope in x18 (do_lookup_x). wineserver daemonises with
// fork and its first lazy binding, setsid, stopped at the Steam overlay that
// the client preloads and never reached libc: "undefined symbol: setsid,
// version GLIBC_2.17", the game "Descriptor invalido" (2026-10-08, every
// Steam launch on the native path). LXRT_NO_X18=0 is off.
bool lxrt_no_x18(void)
{
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("LXRT_NO_X18");
        v = e && *e && strcmp(e, "0") != 0 && g_program_is_wine_loader;
    }
    return v;
}

bool lxrt_wx_enabled(void) { return g_enabled > 0; }
static bool lazy_exec(void) { return g_lazy > 0 && g_enabled > 0; }
// For subpage.c: lazy execute is on (either mode). LXRT_LAZY_SUBPAGE=0 keeps
// that file eager on its own.
bool lxrt_wx_lazy_subpage(void)
{
    static int off = -1;
    if (off < 0) { const char *e = getenv("LXRT_LAZY_SUBPAGE"); off = e && e[0] == '0'; }
    return lazy_exec() && !off;
}

// ------------------------------------------------------------- the table
//
// Guest ranges the guest asked to be read-write-execute (or, lazy execute,
// read-execute), sorted, disjoint, adjacent ones of the same protection
// merged: V8's single 256 MiB mprotect is one entry, and a JIT that commits
// thousands of separate chunks costs one entry per run of chunks. Every
// entry is private anonymous memory the guest owns: whatever replaces that
// (munmap, MAP_FIXED, brk, mremap's destination, lxrt_alias, jit_adopt)
// takes the entries out, and lxrt_wx_protect relies on it.
//
// prot: what the guest asked for, WX_RWX (the split) or WX_RX (lazy execute;
// --X is recorded as R-X -- the scan has to read the page anyway).
// x: a host page of it may be read-execute -- a fetch flipped one, or an R-X
// grant left one so (lazy_host_locked). A hint for that grant, never for
// safety: wrongly false, the grant takes execute from a scanned page, which
// refaults and is scanned again; wrongly true, it asks the kernel first.
struct wxr { uint64_t start, end; int prot; bool x; };
#define WX_RWX (PROT_READ | PROT_WRITE | PROT_EXEC)
#define WX_RX  (PROT_READ | PROT_EXEC)
static struct wxr *g_r;
static int g_cap;
static _Atomic int g_n;
static _Atomic bool g_used;                 // a range was ever split here
// dispatch.c hands ranges back (wx_leave_whole): how many hand-overs run now,
// and a sequence bumped at each start and end, for stale faults (below).
static _Atomic int g_handover_active;
static _Atomic unsigned g_handover_seq;

// The page-protection lock: this table, subpage.c's records, and every
// protection change of a host page either one owns.
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
// Across fork(), as LXRT_FORK_SAFE, and with shmirror.c's lock, which is
// taken under this one (see lxrt_shmirror_fork_lock): in that order, and
// released the other way round.
void lxrt_shmirror_fork_lock(void);
void lxrt_shmirror_fork_unlock(void);
static void wxsplit_fork_prepare(void) { pthread_mutex_lock(&g_lock); lxrt_shmirror_fork_lock(); }
static void wxsplit_fork_release(void) { lxrt_shmirror_fork_unlock(); pthread_mutex_unlock(&g_lock); }
__attribute__((constructor(200))) static void wxsplit_fork_register(void)
{
    pthread_atfork(wxsplit_fork_prepare, wxsplit_fork_release, wxsplit_fork_release);
}
static _Thread_local int t_held;        // this thread holds g_lock

// Every path that holds g_lock runs with the asynchronous signals blocked: the
// runtime runs a guest's handler nested inside the host handler, and a guest
// handler that ran JIT code on a page this thread was in the middle of
// flipping would wait for this thread's own lock forever. Only the
// synchronous faults stay deliverable, and one raised while this thread holds
// the lock is the runtime's own: the fault handlers report it
// (lxrt_pageprot_held) instead of waiting.
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
    t_held++;
}

static void unlock_nosig(const sigset_t *old)
{
    t_held--;
    pthread_mutex_unlock(&g_lock);
    pthread_sigmask(SIG_SETMASK, old, NULL);
}

// For subpage.c, which keeps its records under the same lock.
void lxrt_pageprot_lock(sigset_t *old) { lock_nosig(old); }
void lxrt_pageprot_unlock(const sigset_t *old) { unlock_nosig(old); }
bool lxrt_pageprot_held(void) { return t_held > 0; }

// subpage.c, caller holds g_lock: does it record any guest page in
// [addr, addr+len)? Such a host page is its to flip.
bool lxrt_subpage_tracked_locked(uint64_t addr, uint64_t len);

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

// Remove [start, end) from the table. Caller holds g_lock. Afterwards nothing
// in [start, end) is held, whatever happens: lxrt_wx_protect takes a held
// range for private anonymous memory without asking the kernel.
static void remove_locked(uint64_t start, uint64_t end)
{
    int n = atomic_load(&g_n);
    int i = lower(start);
    while (i < n && g_r[i].start < end) {
        if (g_r[i].start < start && g_r[i].end > end) {       // split in two
            if (!reserve(1)) {
                // No room for the tail: it goes too. Its pages keep their
                // host protection (read-write, read-only or scanned
                // read-execute, all safe); the guest faults there unclaimed.
                g_r[i].end = start;
                return;
            }
            memmove(&g_r[i + 2], &g_r[i + 1], (size_t)(n - i - 1) * sizeof *g_r);
            g_r[i + 1] = g_r[i];
            g_r[i + 1].start = end;
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

// Add [start, end) with protection prot (WX_RWX or WX_RX) and hint x: it
// replaces what it overlaps and merges with neighbours of the same
// protection (their hints joined). Caller holds g_lock and has run
// reserve(2) -- a split by the remove and an insert -- so nothing in here
// can fail half-way.
// Bumped by every add_locked: stale_fault_retry looks the page up again only
// when something was added since the handler found it missing.
static _Atomic unsigned g_add_gen;

static void add_locked(uint64_t start, uint64_t end, int prot, bool x)
{
    atomic_fetch_add(&g_add_gen, 1);
    remove_locked(start, end);
    int n = atomic_load(&g_n);
    int i = lower(start);       // nothing overlaps now: [i-1].end <= start, [i].start >= end
    bool left = i > 0 && g_r[i - 1].end == start && g_r[i - 1].prot == prot;
    bool right = i < n && g_r[i].start == end && g_r[i].prot == prot;
    if (left && right) {
        g_r[i - 1].end = g_r[i].end;
        g_r[i - 1].x |= g_r[i].x | x;
        memmove(&g_r[i], &g_r[i + 1], (size_t)(n - i - 1) * sizeof *g_r);
        atomic_store(&g_n, n - 1);
    } else if (left) {
        g_r[i - 1].end = end;
        g_r[i - 1].x |= x;
    } else if (right) {
        g_r[i].start = start;
        g_r[i].x |= x;
    } else {
        memmove(&g_r[i + 1], &g_r[i], (size_t)(n - i) * sizeof *g_r);
        g_r[i] = (struct wxr){ start, end, prot, x };
        atomic_store(&g_n, n + 1);
    }
    atomic_store(&g_used, true);
}

// Is every byte of [addr, addr+len) held, with protection prot throughout (0:
// either)? Caller holds g_lock.
static bool covered_locked(uint64_t addr, uint64_t len, int prot)
{
    int n = atomic_load(&g_n);
    uint64_t p = addr, end = addr + len;
    if (!len || end < addr)
        return false;
    for (int i = lower(addr); p < end; i++) {
        if (i >= n || g_r[i].start > p || (prot && g_r[i].prot != prot))
            return false;
        p = g_r[i].end;
    }
    return true;
}

// The queries below for a caller that already holds g_lock (subpage.c).

// The protection the guest gave the page at addr (WX_RWX or WX_RX), 0 when
// the table does not hold it.
int lxrt_wx_prot_locked(uint64_t addr)
{
    int i = lower(addr);
    return i < atomic_load(&g_n) && g_r[i].start <= addr ? g_r[i].prot : 0;
}

// Does the guest's protection of the page at addr allow the access (a fetch,
// or a store)? A store into an R-X page is the guest's fault, not a flip.
bool lxrt_wx_allows_locked(uint64_t addr, bool fetch)
{
    return (lxrt_wx_prot_locked(addr) & (fetch ? PROT_EXEC : PROT_WRITE)) != 0;
}

// Is the page at addr lazily executable -- read-execute to the guest, its
// bytes scanned only by the fetch flip that makes its host page executable?
bool lxrt_wx_lazy_locked(uint64_t addr)
{
    return lxrt_wx_prot_locked(addr) == WX_RX;
}

bool lxrt_wx_intersects_locked(uint64_t addr, uint64_t len)
{
    if (!atomic_load(&g_n) || !len)
        return false;
    int i = lower(addr);
    return i < atomic_load(&g_n) && g_r[i].start < addr + len;
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

// Is every byte of [addr, addr+len) in the table (with protection prot
// throughout, for the second)?
bool lxrt_wx_covered_prot(uint64_t addr, uint64_t len, int prot)
{
    if (!atomic_load(&g_n) || !len)
        return false;
    sigset_t old;
    lock_nosig(&old);
    bool r = covered_locked(addr, len, prot);
    unlock_nosig(&old);
    return r;
}

bool lxrt_wx_covered(uint64_t addr, uint64_t len) { return lxrt_wx_covered_prot(addr, len, 0); }

// The same two for dispatch.c's jit_adopt, which holds g_lock
// (lxrt_pageprot_lock) while it replaces the range: asked again there, and
// the range out of the table before anyone can fault on it as MAP_JIT.
bool lxrt_wx_covered_prot_locked(uint64_t addr, uint64_t len, int prot)
{
    return atomic_load(&g_n) && covered_locked(addr, len, prot);
}

void lxrt_wx_forget_locked(uint64_t addr, uint64_t len)
{
    if (atomic_load(&g_n) && len)
        remove_locked(addr, addr + len);
}

// Does any byte of [addr, addr+len) lie in the table?
bool lxrt_wx_intersects(uint64_t addr, uint64_t len)
{
    if (!atomic_load(&g_n) || !len)
        return false;
    sigset_t old;
    lock_nosig(&old);
    bool r = lxrt_wx_intersects_locked(addr, len);
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
// Lazy execute: R-X requests recorded, first fetches that made such a page
// executable, stores into one handed to the guest.
static _Atomic uint64_t st_lazy, st_lazy_fetch, st_lazy_store;
static _Atomic uint64_t st_last;
static int stats_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("LXRT_WX_STATS") ? 1 : 0;
    return on;
}
static void stats_print(const char *when)
{
    char line[512];
    int n = snprintf(line, sizeof line, "[lxrt] pid %d wxsplit%s: %d ranges, %llu write flips, %llu exec flips, "
                     "%llu scans with sites, %llu words rewritten, %llu x18-naming words seen; "
                     "lazy execute: %llu grants, %llu first fetches, %llu stores to the guest\n",
                     (int)getpid(), when, atomic_load(&g_n),
                     (unsigned long long)atomic_load(&st_write), (unsigned long long)atomic_load(&st_exec),
                     (unsigned long long)atomic_load(&st_scan_sites), (unsigned long long)atomic_load(&st_rewritten),
                     (unsigned long long)atomic_load(&st_x18w), (unsigned long long)atomic_load(&st_lazy),
                     (unsigned long long)atomic_load(&st_lazy_fetch), (unsigned long long)atomic_load(&st_lazy_store));
    if (n < 0)
        return;
    if (n >= (int)sizeof line)
        n = (int)sizeof line - 1;
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
    if (stats_on() && (atomic_load(&st_write) || atomic_load(&st_exec) || atomic_load(&g_n) ||
                       atomic_load(&st_lazy) || atomic_load(&st_lazy_fetch) || atomic_load(&st_lazy_store)))
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
            if (!done && !rep.sites_unreachable && !rep.tls_unreachable)
                continue;           // this range of the page had nothing
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

// An RWX request lxrt_wx_protect declines goes the ordinary way (dispatch.c):
// its host pages become read-write, or subpage.c takes the range. What the
// table holds there as R-X becomes RWX first -- what the guest asked for, and
// a kind those pages may carry. Left R-X over a read-write host page, the
// table and the host disagreed: the first fetch made the page read-execute
// and every store after it was refused as the guest's fault, and an R-X
// request found the range "already so" and left it writable (review of lazy
// execute: an RWX mprotect over a lazy page and a shared memfd page beside
// it, then R-X again -- a store into the guest's R-X page landed;
// tests/elf/wx_lazy_exec.c, "decline"). Caller holds g_lock.
static void rwx_declined_locked(uint64_t addr, uint64_t end)
{
    for (uint64_t p = addr;;) {
        int n = atomic_load(&g_n), i = lower(p);
        while (i < n && g_r[i].start < end && g_r[i].prot != WX_RX)
            i++;
        if (i >= n || g_r[i].start >= end)
            return;
        uint64_t s = g_r[i].start > p ? g_r[i].start : p;
        uint64_t e = g_r[i].end < end ? g_r[i].end : end;
        bool x = g_r[i].x;
        if (reserve(2))
            add_locked(s, e, WX_RWX, x);
        else
            remove_locked(s, e);    // no room: out of the table, which is never wrong
        p = e;
    }
}

// The host half of a lazy R-X grant on [addr, end), table entry already in
// place, g_lock held: read-only where a page can be written (or cannot be
// read). A page read-only or read-execute already stays as it is --
// read-execute was scanned when it became so and has not been writable
// since, which holds for every executable page of guest memory, whoever made
// it so. Read-only over all of it took execute from code that had run since
// the range was last made writable, and each such page refaulted and was
// scanned again with nothing changed (review; tests/elf/wx_lazy_exec.c,
// "keeprx": the page that ran stays r-xp). Called only where a page may be
// read-execute (the entries' x hint, or memory the table did not hold):
// FEX's trap, an R-X over pages nothing has fetched natively, stays one
// mprotect -- this walk's mach_vm_region on every trap took smc's no-call
// rounds from 200-202 to 157-183 thousand a second, and with the hint they
// are 200-207 again (MEASURED, tests/elf/wx_lazy_exec.c "smc", four and
// eight runs, 2026-10-10).
static bool lazy_host_locked(uint64_t addr, uint64_t end)
{
    for (uint64_t p = addr; p < end;) {
        mach_vm_address_t ra = p;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t ri;
        mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS || ra > p)
            // A hole: the kernel's own answer for it.
            return mprotect((void *)(uintptr_t)p, (size_t)(end - p), PROT_READ) == 0;
        uint64_t next = ra + rs < end ? ra + rs : end;
        if (((ri.protection & VM_PROT_WRITE) || !(ri.protection & VM_PROT_READ)) &&
            mprotect((void *)(uintptr_t)p, (size_t)(next - p), PROT_READ) != 0)
            return false;
        p = next;
    }
    return true;
}

// The guest asked for an executable protection prot on [addr, addr+len),
// host-page aligned: read-write-execute (the split), or read-execute without
// write (lazy execute, header). Returns 1 when handled here (*ret set), 0
// when the caller should fall back to its old behaviour (lazy execute off,
// not private anonymous memory, mode 2 and a range the table does not hold,
// or a host page subpage.c owns: dispatch.c then sends the request there --
// for RWX, with what the table holds there made RWX, rwx_declined_locked).
int lxrt_wx_protect(uint64_t addr, uint64_t len, int prot, long *ret)
{
    if (!lxrt_wx_enabled() || !len || !(prot & PROT_EXEC))
        return 0;
    bool lazy = !(prot & PROT_WRITE);
    if (lazy && !lazy_exec())
        return 0;
    int want = lazy ? WX_RX : WX_RWX;
    uint64_t end = addr + len;
    // A range that wraps the address space: Linux says ENOMEM, and an entry
    // ending below its start broke lower()'s binary search for every lookup
    // after it (review: an RWX or R-X mprotect wrapping the top made every
    // later fetch in a held range a guest SIGSEGV).
    if (end <= addr) {
        *ret = -12;                 // ENOMEM
        return 1;
    }
    sigset_t old;
    lock_nosig(&old);
    // A page subpage.c tracks is its, and stays its: asked under the lock its
    // records are kept under.
    if (lxrt_subpage_tracked_locked(addr, len)) {
        if (!lazy)
            rwx_declined_locked(addr, end);
        unlock_nosig(&old);
        return 0;
    }
    // Already so: every host page of it is in a state this protection allows
    // (RWX: read-write, read-only, or read-execute scanned; R-X: read-only, or
    // read-execute scanned). FEX traps pages it has trapped before.
    if (covered_locked(addr, len, want)) {
        unlock_nosig(&old);
        *ret = 0;
        return 1;
    }
    // A range the table holds is private anonymous memory (the table's
    // header): no region walk. FEX's untrap (RWX over its own trap) paid one
    // every time -- anon_private's mach_vm_region was 105-166 of each smc_bench
    // writer thread's 2,631 samples, nearly all of lxrt_wx_protect's
    // (MEASURED, 2026-10-10).
    bool held = covered_locked(addr, len, 0);
    if (!held && ((lazy && g_lazy == 2) || !anon_private(addr, len))) {
        if (lazy) {
            unlock_nosig(&old);
            return 0;
        }
        // RWX declined: what dispatch.c would grant -- read-write -- granted
        // here under the lock, so that the table's R-X pieces become RWX only
        // if the host really changed. Converted first, a read-only shared
        // view earlier in the range made the mprotect fail and change nothing
        // (Darwin and Linux both), the guest's page stayed R-X, and the
        // table's RWX let the next store into it land (review: RWX over [a
        // read-only shared file][a lazy page], EACCES, then a store landed).
        int r = mprotect((void *)(uintptr_t)addr, (size_t)len, PROT_READ | PROT_WRITE);
        int e = errno;
        if (r == 0)
            rwx_declined_locked(addr, end);
        unlock_nosig(&old);
        *ret = r == 0 ? 0 : -lxrt_errno_to_linux(e);
        return 1;
    }
    // What the table holds there now, to put back as it was if the host
    // refuses the change; and room first, so that nothing changes unless all
    // of it does.
    struct wxr was_buf[16], *was = was_buf;
    int nwas = 0;
    int n = atomic_load(&g_n), first = lower(addr), last = first;
    while (last < n && g_r[last].start < end)
        last++;
    if (last - first > (int)(sizeof was_buf / sizeof was_buf[0]))
        was = malloc((size_t)(last - first) * sizeof *was);
    if (!was || !reserve(2)) {
        if (was != was_buf)
            free(was);
        unlock_nosig(&old);
        *ret = -12;                 // ENOMEM
        return 1;
    }
    // A page of it may be read-execute on the host: one an entry's hint names,
    // or any where the table holds nothing (who knows).
    bool anyx = !held;
    for (int i = first; i < last; i++) {
        was[nwas++] = (struct wxr){ g_r[i].start > addr ? g_r[i].start : addr,
                                    g_r[i].end < end ? g_r[i].end : end, g_r[i].prot, g_r[i].x };
        anyx |= g_r[i].x;
    }
    // Table first, host second, both under the lock. The fault handler looks
    // at the table's size before it takes the lock: with the table empty --
    // an ELF program's, mode 0, after every R-X made the range leave it -- a
    // fetch that faulted on a page this call had already made read-write
    // found nothing to wait for and reached the guest as SIGSEGV (MEASURED:
    // tests/elf/wx_lazy_exec.c "race" in mode 0, six runs at a time, 29 of
    // 192 runs with the host first). With the entry in first, that handler
    // waits for the lock and finds the page held; a handler that sees the
    // entry while the host still has the old protection is behind this lock
    // too.
    //
    // RWX: read-write first -- a guest that asks for write is about to write,
    // and a page nobody executes never has to be scanned. R-X: read-only
    // where the page could be written (lazy_host_locked, when a page may be
    // read-execute; read-only throughout otherwise). Execute comes on the
    // first instruction fetch either way, through lxrt_wx_handle_fault, which
    // scans then; a store into an R-X page faults and is the guest's.
    add_locked(addr, end, want, lazy && anyx);
    bool ok = lazy && anyx ? lazy_host_locked(addr, end)
                           : mprotect((void *)(uintptr_t)addr, (size_t)len,
                                      lazy ? PROT_READ : PROT_READ | PROT_WRITE) == 0;
    if (!ok) {
        int e = errno;
        // Back as it was: the pieces it held merge again with what they
        // were cut from (the table never holds two touching entries of one
        // protection). Only lazy_host_locked can stop half-way, and what it
        // leaves is read-only, which either kind allows; the RWX change is
        // one mprotect, all or nothing, so its R-X pieces are still R-X.
        remove_locked(addr, end);
        for (int i = 0; i < nwas; i++)
            if (reserve(2))
                add_locked(was[i].start, was[i].end, was[i].prot, was[i].x);
        if (was != was_buf)
            free(was);
        unlock_nosig(&old);
        *ret = -lxrt_errno_to_linux(e);
        return 1;
    }
    if (was != was_buf)
        free(was);
    if (lazy)
        atomic_fetch_add(&st_lazy, 1);
    unlock_nosig(&old);
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] mprotect 0x%llx+0x%llx %s (%d ranges)\n",
                (unsigned long long)addr, (unsigned long long)len,
                lazy ? "R-X: read-only until its first fetch" : "RWX: W^X split", atomic_load(&g_n));
    *ret = 0;
    return 1;
}

// mremap.c is about to move [addr, addr+len), host-page aligned, and moves
// one protection, as Linux moves one VMA. A range this table holds whole,
// all with one guest protection, can show several on the host: read-only
// pages not fetched yet beside read-execute ones (lazy execute), read-write
// beside read-execute (the split) -- and mremap.c refused that with EFAULT
// (review: a lazy range with one of its pages run; tests/elf/wx_lazy_exec.c,
// "fetched"). Each page goes to its entry's base state, read-only for R-X
// and read-write for RWX: one protection, which the destination takes and
// the table carries there (lxrt_wx_moved); execute comes back on the next
// fetch, scanned where the page is then. A range of mixed guest protections
// is left as it is, and refused, as Linux refuses two VMAs. Returns whether
// it changed anything.
bool lxrt_wx_unify_prot(uint64_t addr, uint64_t len)
{
    if (!atomic_load(&g_n) || !len)
        return false;
    sigset_t old;
    lock_nosig(&old);
    int kind = covered_locked(addr, len, WX_RX) ? WX_RX : covered_locked(addr, len, WX_RWX) ? WX_RWX : 0;
    bool done = kind && !lxrt_subpage_tracked_locked(addr, len) &&
                mprotect((void *)(uintptr_t)addr, (size_t)len,
                         kind == WX_RX ? PROT_READ : PROT_READ | PROT_WRITE) == 0;
    unlock_nosig(&old);
    return done;
}

// mremap moved [old, old+olen) to [neu, neu+nlen): the new range keeps what
// the guest gave the old one, piece by piece (a grown tail takes the last
// piece's), and the pages keep their host protection -- a read-execute one
// was scanned where it was and holds the same bytes now. Whatever the table
// held at the destination goes first, whether the source was held or not:
// the move replaced it (MREMAP_FIXED), and a shared or file mapping moved
// over a held range was left in the table, where a fetch flipped, scanned and
// rewrote it like the guest's own private memory (MEASURED before this: a
// memfd's svc came back as a branch, 15800ffe, through its other view --
// tests/elf/wx_lazy_exec.c, "fixed").
void lxrt_wx_moved(uint64_t old, uint64_t olen, uint64_t neu, uint64_t nlen)
{
    if (!atomic_load(&g_n))
        return;
    sigset_t o;
    lock_nosig(&o);
    struct wxr *pc = NULL;
    int np = 0;
    if (nlen && covered_locked(old, olen, 0)) {
        int n = atomic_load(&g_n), first = lower(old), last = first;
        while (last < n && g_r[last].start < old + olen)
            last++;
        pc = malloc((size_t)(last - first) * sizeof *pc);
        for (int i = first; pc && i < last; i++) {
            uint64_t so = (g_r[i].start > old ? g_r[i].start : old) - old;
            uint64_t eo = (g_r[i].end < old + olen ? g_r[i].end : old + olen) - old;
            if (so >= nlen)
                break;
            if (i == last - 1 && nlen > olen)
                eo = nlen;          // grown: the tail takes this piece's protection
            else if (eo > nlen)
                eo = nlen;
            pc[np++] = (struct wxr){ neu + so, neu + eo, g_r[i].prot, g_r[i].x };
        }
    }
    remove_locked(old, old + olen);
    remove_locked(neu, neu + nlen);
    for (int i = 0; i < np; i++)
        if (reserve(2))
            add_locked(pc[i].start, pc[i].end, pc[i].prot, pc[i].x);
    unlock_nosig(&o);
    free(pc);
}

// ------------------------------------------------------------- faults

// Last flips of this thread, to name a page that never makes progress.
static _Thread_local uint64_t t_last_hp, t_last_pc;
static _Thread_local unsigned t_pingpong;

// Flip the host page hp, which this file owns (subpage.c does not track it):
// read-only, scanned and read-execute for a fetch, read-write for a store.
// Caller holds g_lock. Also subpage.c's, for a page that stopped being its
// between the two handlers.
bool lxrt_wx_flip_locked(uint64_t hp, bool fetch)
{
    int cur = host_prot(hp);
    int n = atomic_load(&g_n);
    bool ok = true;
    if (fetch) {
        if (cur < 0 || !(cur & PROT_EXEC)) {
            // The whole host page is scanned: all of it becomes executable.
            // The table's ranges are host-page aligned where they came from
            // an aligned mprotect, but a 4 KiB munmap trims them and leaves
            // the freed bytes in the page (dispatch.c do_munmap); scanning
            // only the held part made that slot executable unscanned, and a
            // svc in it ran live (review: tests/elf/wx_lazy_exec.c, "freed";
            // RWX alone did it too). Those bytes are no guest page of anyone
            // -- a mapping there would make the page subpage.c's -- so
            // rewriting them is harmless. A page subpage.c tracks never comes
            // here (both callers ask); if one did, only its held part, the
            // rest being other mappings.
            struct lxrt_range r[LXRT_HOST_PAGE / 4096];
            int nr = 0;
            bool lazy = false;
            for (int i = lower(hp); i < n && g_r[i].start < hp + LXRT_HOST_PAGE; i++) {
                lazy |= !(g_r[i].prot & PROT_WRITE);
                uint64_t s = g_r[i].start > hp ? g_r[i].start : hp;
                uint64_t e = g_r[i].end < hp + LXRT_HOST_PAGE ? g_r[i].end : hp + LXRT_HOST_PAGE;
                if (nr == (int)(sizeof r / sizeof r[0])) {
                    r[nr - 1].end = e;          // more pieces than slots: cover the rest
                    continue;
                }
                r[nr].start = s;
                r[nr].end = e;
                nr++;
            }
            if (!lxrt_subpage_tracked_locked(hp, LXRT_HOST_PAGE)) {
                r[0] = (struct lxrt_range){ hp, hp + LXRT_HOST_PAGE };
                nr = 1;
            }
            ok = lxrt_wx_scan_for_exec(hp, r, nr) &&
                 mprotect((void *)(uintptr_t)hp, LXRT_HOST_PAGE, PROT_READ | PROT_EXEC) == 0;
            if (ok) {
                sys_icache_invalidate((void *)(uintptr_t)hp, LXRT_HOST_PAGE);
                atomic_fetch_add(&st_exec, 1);
                if (lazy)
                    atomic_fetch_add(&st_lazy_fetch, 1);
            }
        }
        // Read-execute now: an R-X request over it looks before it takes
        // execute away (struct wxr's hint).
        if (ok)
            for (int i = lower(hp); i < n && g_r[i].start < hp + LXRT_HOST_PAGE; i++)
                g_r[i].x = true;
    } else if (cur < 0 || !(cur & PROT_WRITE)) {
        // Never a host page holding a page the guest did not make writable
        // (lazy execute): the callers ask about the faulting address
        // (lxrt_wx_allows_locked); this is the backstop for the rest of the
        // page. Writable, its R-X neighbour would take stores the guest must
        // see fault -- FEX's self-modifying-code trap.
        for (int i = lower(hp); i < n && g_r[i].start < hp + LXRT_HOST_PAGE; i++)
            if (!(g_r[i].prot & PROT_WRITE)) {
                errno = EACCES;
                return false;
            }
        ok = mprotect((void *)(uintptr_t)hp, LXRT_HOST_PAGE, PROT_READ | PROT_WRITE) == 0;
        if (ok)
            atomic_fetch_add(&st_write, 1);
    }
    return ok;
}

void lxrt_wx_handover(bool begin)
{
    atomic_fetch_add(&g_handover_seq, 1);
    atomic_fetch_add(&g_handover_active, begin ? 1 : -1);
}

// Would the access that faulted succeed now? mach_vm_region is a Mach trap,
// fine in a signal handler.
static bool access_now_allowed(uint64_t addr, bool fetch)
{
    mach_vm_address_t a = addr;
    mach_vm_size_t sz = 0;
    vm_region_basic_info_data_64_t ri;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&ri, &cnt, &obj) != KERN_SUCCESS || a > addr)
        return false;
    return fetch ? (ri.protection & VM_PROT_EXECUTE) != 0 : (ri.protection & VM_PROT_WRITE) != 0;
}

// A fault this thread took while the split still held the page, whose
// handler runs after dispatch.c handed the range back (wx_leave_whole): the
// table no longer claims it although the page allows the access by then, or
// will once the hand-over applies the new protection. Retry it then; decline
// once no hand-over runs and the page still forbids the access (a real fault).
// At most 64 retries in a row for the same pc and address: a fault that keeps
// coming back with the access allowed is not about protection.
//
// Retry it too when the table holds the page again by then: a request since
// put it back (its entry goes in before the host change, lxrt_wx_protect),
// and the fault, retried, comes back to the table and is flipped. Declined,
// it went on to subpage.c's fallback, which flips a page of the table --
// unless, the thread preempted in between, the next hand-over had already
// taken the page out again: then the guest took SIGSEGV for a fetch that
// every protection it had asked for allowed (MEASURED: tests/elf/
// wx_lazy_exec.c "race" in mode 0, six runs at a time: 10 of 2,880 runs,
// each one such a fetch, the page back in the table when this declined;
// none of 2,880 with the retry).
static _Thread_local uint64_t t_stale_pc, t_stale_addr;
static _Thread_local unsigned t_stale_n;
static bool stale_fault_retry(uint64_t pc, uint64_t addr, bool fetch, unsigned gen)
{
    if (!atomic_load(&g_used))
        return false;
    if (t_stale_pc == pc && t_stale_addr == addr) {
        if (++t_stale_n > 64)
            return false;
    } else {
        t_stale_pc = pc;
        t_stale_addr = addr;
        t_stale_n = 0;
    }
    for (int i = 0; i < 20000; i++) {                       // at most ~1 s
        unsigned s1 = atomic_load(&g_handover_seq);
        bool allowed = access_now_allowed(addr, fetch);
        int active = atomic_load(&g_handover_active);
        // Back in the table since the handler looked: a fetch flip waits for
        // it there. Asked only when something was added since -- the lookup is
        // a lock round trip, and every fault the table never held (Wine's write
        // watches, guard pages, copy-on-write) paid it: ~5 % on a store-fault
        // loop (review, 296-299 -> 280-285 thousand faults a second).
        bool held = !allowed && active == 0 && atomic_load(&g_add_gen) != gen && lxrt_wx_contains(addr);
        unsigned s2 = atomic_load(&g_handover_seq);
        if (allowed || held)
            return true;
        if (active == 0 && s1 == s2)
            return false;
        struct timespec ts = { 0, 50000 };
        nanosleep(&ts, NULL);
    }
    return false;
}

// Set when lxrt_wx_handle_fault declined a store into a page the guest made
// read-execute: the guest's own fault, which no later handler of the chain
// can own either (signal.c delivers it at once).
static _Thread_local bool t_guest_fault;
bool lxrt_wx_take_guest_fault(void)
{
    bool r = t_guest_fault;
    t_guest_fault = false;
    return r;
}

bool lxrt_wx_handle_fault(uint64_t pc, uint64_t addr, uint32_t esr)
{
    t_guest_fault = false;
    if (!addr || !atomic_load(&g_used))
        return false;
    uint32_t ec = esr >> 26;
    // An alignment fault (data abort, DFSC 0b100001) is never about page
    // protection: a misaligned store-release into a read-write page of the
    // table is WnR=1, and taking it for a write flip retried the instruction
    // forever (MEASURED with lxrun's first handler, main.c's fault_report,
    // which had no filter: tests/elf/wx_owner.c, "misaligned stlr").
    if ((ec == 0x24 || ec == 0x25) && (esr & 0x3f) == 0x21)
        return false;
    // A fetch is an instruction abort. pc == addr says so only without a
    // syndrome: a branch to a misaligned address has si_addr == pc too (a PC
    // alignment fault, EC 0x22), and taken for a fetch it was "granted" and
    // came back forever (review of lazy execute: a bad function pointer hung
    // at 100 % instead of crashing).
    bool fetch = ec == 0x20 || ec == 0x21 || (esr == 0 && pc == addr);
    bool write = !fetch && (ec == 0x24 || ec == 0x25) && (esr & (1u << 6));
    if (!fetch && !write)
        return false;
    // Raised while this thread holds the lock: the runtime's own fault, not a
    // flip. Waiting would be waiting for itself.
    if (t_held)
        return false;
    uint64_t hp = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
    unsigned gen = atomic_load(&g_add_gen);
    if (!atomic_load(&g_n))
        return stale_fault_retry(pc, addr, fetch, gen);

    sigset_t old;
    lock_nosig(&old);
    // A page subpage.c tracks is subpage.c's (see the header): its handler,
    // later in the same chain, scans every RWX guest page in it.
    if (lxrt_subpage_tracked_locked(hp, LXRT_HOST_PAGE)) {
        unlock_nosig(&old);
        return false;
    }
    int gp = lxrt_wx_prot_locked(addr);
    if (!gp) {
        gen = atomic_load(&g_add_gen);          // as of this lookup, under the lock
        unlock_nosig(&old);
        return stale_fault_retry(pc, addr, fetch, gen);
    }
    if (!(gp & (fetch ? PROT_EXEC : PROT_WRITE))) {
        // A store into a page the guest made read-execute (lazy execute):
        // its own fault -- FEX's self-modifying-code trap, a Wine write
        // watch -- delivered, never flipped. Every entry allows a fetch.
        // (pc 0: dispatch.c's hand-over bringing the page to a state a
        // writable protection allows; this one never allowed a store.)
        unlock_nosig(&old);
        if (pc) {
            atomic_fetch_add(&st_lazy_store, 1);
            t_guest_fault = true;
            stats_tick();
        }
        return false;
    }
    bool ok = lxrt_wx_flip_locked(hp, fetch);
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
