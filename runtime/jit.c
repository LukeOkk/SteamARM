// Guest JIT memory.
//
// FEX writes instructions into its code buffer while that buffer is
// executable. Apple Silicon does not offer that to anyone: plain RWX is
// refused outright, and MAP_JIT gives a per-thread switch where the thread can
// write OR execute, never both (measured -- benchmarks/stage5-jit.txt).
//
// The first design drove the transition from the faults themselves: a write to
// an execute-mode region faults, the handler flips the thread to write mode and
// resumes. That design is DEAD -- benchmarks/stage5-jit.txt measured that a
// pthread_jit_write_protect_np() issued inside a signal handler does not
// survive the handler's return, so the same store faults again forever (5
// times at the same address before the test gave up). Outside a handler the
// same call works (3 clean cycles), so the flip has to be requested by the
// guest on its own stack, not synthesised behind its back.
//
// So the guest asks. lxrt_jit_set_write() is reachable from guest code as
// private syscall 0x4C580020 (x8), taking (enable, addr, len): enable=0 opens
// the thread for writing, enable=1 closes it for execution and rescans exactly
// the range the guest says it just emitted. FEX carries a three-line
// JitWriteScope around its emit and its two code-buffer memcpys; on a real
// Linux kernel that syscall number is far past the table, returns -ENOSYS, and
// every scope becomes a no-op -- one binary, both hosts.
//
// lxrt_jit_handle_fault() below is kept for an unpatched guest: it still
// reports what the memory actually is, which is how the dead design was
// diagnosed in the first place, and it still services the execute-side flip
// (the flip that DOES survive, because by then the thread is already in write
// mode and the handler only has to notice).
//
// The flip back to execute mode is also the only moment at which generated
// code can be inspected, so it is where `svc` sites in JIT output are
// rewritten. Stage 1 established that a live `svc` runs an arbitrary Darwin
// syscall, and JIT output is precisely the code no load-time pass can see.

#include "lxrt.h"

#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <sys/ucontext.h>

#define MAX_JIT_REGIONS 64

static struct {
    uint64_t start, end;
} g_jit[MAX_JIT_REGIONS];
static atomic_int g_jit_count;
static pthread_mutex_t g_jit_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(jit_g_jit_lock, g_jit_lock)

// The per-thread APRR state. Threads start able to execute, which is what a
// freshly mapped region is for.
static _Thread_local bool g_writable;

bool lxrt_trace_on(void);

bool lxrt_jit_register(uint64_t start, uint64_t len)
{
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt]    jit region 0x%llx+0x%llx registered\n",
                (unsigned long long)start, (unsigned long long)len);
    pthread_mutex_lock(&g_jit_lock);
    int n = atomic_load(&g_jit_count);
    // Reuse a slot freed by lxrt_jit_forget: FEX replaces its code buffers
    // over a long run, and 64 live slots were exhausted otherwise.
    int slot = -1;
    for (int i = 0; i < n; i++)
        if (g_jit[i].start == 0 && g_jit[i].end == 0) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_JIT_REGIONS) {
            pthread_mutex_unlock(&g_jit_lock);
            return false;
        }
        slot = n;
    }
    g_jit[slot].end = start + len;
    g_jit[slot].start = start;
    if (slot == n)
        atomic_store(&g_jit_count, n + 1);
    pthread_mutex_unlock(&g_jit_lock);
    return true;
}

// munmap / MAP_FIXED over a JIT region: forget every region entirely inside
// [start, start+len). A stale entry made the fault handler treat a fetch from
// an unmapped former code buffer as a mode transition, forever.
// The last few JIT regions the guest freed, to recognise a thread that later
// runs into one (FEX freeing a code buffer another thread still executes).
static struct { uint64_t start, end, lr, when; int tid; } g_freed[16];
static atomic_uint g_freed_next;
uint64_t lxrt_last_guest_lr(void);

void lxrt_jit_forget(uint64_t start, uint64_t len)
{
    uint64_t end = start + len;
    if (!atomic_load(&g_jit_count))
        return;
    pthread_mutex_lock(&g_jit_lock);
    int n = atomic_load(&g_jit_count);
    for (int i = 0; i < n; i++)
        if (g_jit[i].end && g_jit[i].start >= start && g_jit[i].end <= end) {
            unsigned k = atomic_fetch_add(&g_freed_next, 1) % 16;
            uint64_t tid = 0;
            pthread_threadid_np(NULL, &tid);
            g_freed[k].start = g_jit[i].start;
            g_freed[k].end = g_jit[i].end;
            g_freed[k].lr = lxrt_last_guest_lr();
            g_freed[k].when = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            g_freed[k].tid = (int)tid;
            g_jit[i].start = 0;
            g_jit[i].end = 0;
        }
    pthread_mutex_unlock(&g_jit_lock);
}

// Report (once per region) a fault inside a JIT region that was freed.
void lxrt_jit_report_freed(uint64_t pc, uint64_t addr, void *uap)
{
    for (unsigned k = 0; k < 16; k++) {
        if (!g_freed[k].end || !((pc >= g_freed[k].start && pc < g_freed[k].end) ||
                                 (addr >= g_freed[k].start && addr < g_freed[k].end)))
            continue;
        uint64_t ib = lxrt_main_image_base, ie = ib + lxrt_main_image_span;
        uint64_t lr = g_freed[k].lr;
        uint64_t tid = 0;
        pthread_threadid_np(NULL, &tid);
        fprintf(lxrt_trace_stream(), "[lxrt] pid %d tid %d: fault at pc 0x%llx addr 0x%llx inside JIT region "
                "0x%llx-0x%llx freed %.3f s ago by tid %d (munmap caller lr 0x%llx%s+0x%llx)\n",
                (int)getpid(), (int)tid, (unsigned long long)pc, (unsigned long long)addr,
                (unsigned long long)g_freed[k].start, (unsigned long long)g_freed[k].end,
                (double)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - g_freed[k].when) / 1e9,
                g_freed[k].tid, (unsigned long long)lr, lr >= ib && lr < ie ? " = image" : "",
                (unsigned long long)(lr >= ib && lr < ie ? lr - ib : 0));
        if (uap) {
            ucontext_t *u = uap;
            _STRUCT_ARM_THREAD_STATE64 *ss = &u->uc_mcontext->__ss;
            uint64_t flr = ss->__lr;
            fprintf(lxrt_trace_stream(), "[lxrt]   lr 0x%llx%s+0x%llx sp 0x%llx fp 0x%llx x28 0x%llx x0 0x%llx x1 0x%llx x16 0x%llx x17 0x%llx\n",
                    (unsigned long long)flr, flr >= ib && flr < ie ? " = image" : "",
                    (unsigned long long)(flr >= ib && flr < ie ? flr - ib : 0),
                    (unsigned long long)ss->__sp, (unsigned long long)ss->__fp,
                    (unsigned long long)ss->__x[28], (unsigned long long)ss->__x[0],
                    (unsigned long long)ss->__x[1], (unsigned long long)ss->__x[16],
                    (unsigned long long)ss->__x[17]);
            // Host stack words that point into the freed buffer or the image.
            uint64_t *sp = (uint64_t *)(uintptr_t)ss->__sp;
            for (int i = 0; i < 32; i++) {
                uint64_t v = 0; mach_vm_size_t got = 0;
                if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)(sp + i), 8,
                                           (mach_vm_address_t)(uintptr_t)&v, &got) != KERN_SUCCESS)
                    break;
                bool in_old = v >= g_freed[k].start && v < g_freed[k].end;
                bool in_img = v >= ib && v < ie;
                if (in_old || in_img)
                    fprintf(lxrt_trace_stream(), "[lxrt]   sp+%d: 0x%llx %s+0x%llx\n", i * 8, (unsigned long long)v,
                            in_old ? "freed-buffer" : "image",
                            (unsigned long long)(in_old ? v - g_freed[k].start : v - ib));
            }
        }
        g_freed[k].end = 0;   // once
        return;
    }
}

// Is this address mapped executable right now? (A fetch fault anywhere else is
// a real fault, not a W^X transition.)
static bool mapped_executable(uint64_t addr)
{
    mach_vm_address_t ra = addr;
    mach_vm_size_t rs = 0;
    vm_region_basic_info_data_64_t ri;
    mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS)
        return false;
    return ra <= addr && addr < ra + rs && (ri.protection & VM_PROT_EXECUTE);
}

static int region_of(uint64_t addr)
{
    int n = atomic_load(&g_jit_count);
    for (int i = 0; i < n; i++)
        if (addr >= g_jit[i].start && addr < g_jit[i].end)
            return i;
    return -1;
}

bool lxrt_jit_contains(uint64_t addr) { return region_of(addr) >= 0; }


// A fault tells us one address and nothing about extent, so guess a page.
// The pages are this thread's own: write mode is per thread, and so is what
// it wrote. They used to go into the region's dirty window, shared by every
// thread, as a lo..hi union: one thread's page at the start of FEX's code
// buffer and another's near the end made the next rescan read the whole
// buffer -- Counter-Strike 2 froze for 1-2 s with one thread in
// lxrt_rewrite_range_code under rescan for every sample of the stall
// (benchmarks/stage61-cs2-stutter.txt, section 7), and one thread's rescan
// took the other's page while it was still being written.
#define WPAGES 32
static _Thread_local uint64_t g_wpages[WPAGES];
static _Thread_local int g_wpages_n;

static void rescan_exact(int idx, uint64_t lo, uint64_t hi);

static void rescan_written_pages(void)
{
    int n = g_wpages_n;
    g_wpages_n = 0;
    for (int i = 0; i < n; i++) {
        int idx = region_of(g_wpages[i]);
        if (idx >= 0)
            rescan_exact(idx, g_wpages[i], g_wpages[i] + 4096);
    }
}

static void note_written(int idx, uint64_t addr)
{
    (void)idx;
    uint64_t page = addr & ~4095ull;
    for (int i = 0; i < g_wpages_n; i++)
        if (g_wpages[i] == page)
            return;
    if (g_wpages_n == WPAGES)
        rescan_written_pages();     // still in write mode: scan what is noted so far
    g_wpages[g_wpages_n++] = page;
}

// Rewrite anything the JIT emitted that the runtime cannot let run as-is.
// Only the range that was actually written is scanned.
// How much JIT output the rescan has read, for the sigstats line (signal.c).
_Atomic uint64_t lxrt_rescan_calls, lxrt_rescan_words;

// Scan one range of a region, clipped to it, for svc / sysreg / x18 sites.
static void rescan_exact(int idx, uint64_t lo, uint64_t hi)
{
    if (lo < g_jit[idx].start) lo = g_jit[idx].start;
    if (hi > g_jit[idx].end)   hi = g_jit[idx].end;
    lo &= ~3ull;
    hi = (hi + 3) & ~3ull;
    if (hi > g_jit[idx].end)   hi = g_jit[idx].end;
    if (lo >= hi)
        return;
    atomic_fetch_add(&lxrt_rescan_calls, 1);
    atomic_fetch_add(&lxrt_rescan_words, (hi - lo) / 4);
    struct lxrt_rewrite_report rep;
    char *err = NULL;
    if (lxrt_rewrite_range(lo, hi, &rep, &err) == 0 && rep.sites_found)
        fprintf(lxrt_trace_stream(), "[lxrt] JIT output: %zu svc sites rewritten, %zu poisoned\n",
                rep.sites_rewritten, rep.sites_unreachable);
}

// The pages this thread noted by write faults (note_written).
static void rescan(int idx)
{
    (void)idx;
    rescan_written_pages();
}

// Guest-driven W^X. Called for private syscall 0x4C580020.
//
//   enable == 0  -> this thread may write JIT pages, not execute them
//   enable == 1  -> this thread may execute them, not write them; [addr,len)
//                   is scanned for instructions the runtime cannot let run
//   enable == 2  -> scan [addr,len) and STAY writable. This is what an inner
//                   scope does when an outer one still holds write mode: the
//                   scan has to happen while the range is still writable, but
//                   the flip belongs to whoever opened it.
//
// The range is the guest's own claim about what it just emitted. Passing len=0
// is legal and means "nothing new to scan" -- FEX's emit scope writes into a
// scratch buffer and only its two migration scopes have a final address.
// The per-thread W^X state, for signal delivery. A signal that lands inside a
// guest's write window (FEX's memcpy into its code buffer) must run the guest
// handler in execute mode -- the handler runs translated code -- and the
// sigreturn must put the window back, or the interrupted store resumes in
// execute mode and takes SIGBUS. Measured: bash's SIGCHLD storm under a fork
// loop died intermittently (8 forks: crash; 16: fine; 24: crash) until this.
bool lxrt_jit_thread_writable(void) { return g_writable; }

// Write scopes this thread has open. FEX as a Linux ELF tracks its own
// nesting (enable=2 for an inner scope); as an ARM64EC DLL it cannot keep a
// thread_local (it would take the game's TLS slot), so every scope there opens
// with 0 and closes with 1, and only the outermost close returns the thread to
// execute mode.
static _Thread_local int g_write_depth;

long lxrt_jit_set_write(int enable, uint64_t addr, uint64_t len)
{
    if (enable) {
        // Experiment knob: LXRT_NO_RESCAN=1 skips the rewrite of JIT output.
        static int no_rescan = -1;
        if (no_rescan < 0) no_rescan = getenv("LXRT_NO_RESCAN") ? 1 : 0;
        if (len && !no_rescan) {
            int idx = region_of(addr);
            if (idx >= 0) {
                uint64_t end = addr + len;
                if (end > g_jit[idx].end) end = g_jit[idx].end;
                // Still in write mode here: the rewriter needs to store into
                // the range it is fixing up. Exactly this scope's range: the
                // region's dirty window is shared by every thread, and one
                // thread's 4-byte branch patch far from another's new block
                // made the union of the two cover the distance between them
                // -- Counter-Strike 2 loading scanned 1 GB of JIT output per
                // 5 s for a few MB written (sigstats rescan counters,
                // benchmarks/stage61-cs2-stutter.txt). A range noted while a
                // thread was still writing it could also be taken by another
                // thread's rescan before the write was done.
                rescan_exact(idx, addr, end);
                rescan(idx);                // pages noted by write faults
            } else if (lxrt_trace_on()) {
                fprintf(lxrt_trace_stream(), "[lxrt]    jit_set_write(1) range 0x%llx+0x%llx "
                                "outside every registered region\n",
                        (unsigned long long)addr, (unsigned long long)len);
            }
        }
        if (enable == 2)
            return 0;           // inner scope: scanned, still writable
        if (g_write_depth > 0 && --g_write_depth > 0)
            return 0;           // an outer scope is still open
        lxrt_jit_protect(1);
        g_writable = false;
    } else {
        if (g_write_depth++ > 0 && g_writable)
            return 0;           // nested: already writable
        lxrt_jit_protect(0);
        g_writable = true;
    }
    return 0;
}

// The runtime's own switch around a guest signal handler (signal.c): the mode
// is put back as it was, and the guest's write scopes (g_write_depth) are not
// the runtime's to count. Through lxrt_jit_set_write, the switches before and
// after a handler moved the count; it drifted, scopes closed without returning
// the thread to execute mode, and the next instruction in JIT code faulted:
// ~25,000 faults a second in Minecraft Dungeons II (MEASURED, gone with this).
void lxrt_jit_mode(bool writable)
{
    lxrt_jit_protect(writable ? 0 : 1);
    g_writable = writable;
}

// pthread_jit_write_protect_np writes the thread's JIT permission register
// (S3_6_C15_C1_5), reads it back and traps (brk #1) when the two differ
// (disassembled on macOS 27: msr, isb, mrs, cmp, b.ne to a brk). A signal
// taken between the write and the read whose handler changes the thread's
// JIT mode -- the runtime's own fault handlers below do -- made them differ:
// the intermittent SIGTRAP at pthread_jit_write_protect_np+388 that ended
// one x86 Steam start in a few (MEASURED: lxrt_jit_set_write, called from a
// guest's JitWriteScope syscall). Every call runs with signals blocked.
void lxrt_jit_protect(int enable)
{
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    pthread_jit_write_protect_np(enable);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
}

// Execute-mode stub (trampoline.S): the fetch-fault handler parks the faulting
// context here and sends the thread through the stub, whose brk comes back
// to lxrt_jit_stub_trap().
extern char lxrt_jit_exec_stub[], lxrt_jit_exec_stub_trap[];
static _Thread_local struct {
    bool active;
    _STRUCT_ARM_THREAD_STATE64 ss;
    _STRUCT_ARM_NEON_STATE64 ns;
} g_stub;
// Last pc the stub resumed, when, and how many immediate re-faults followed.
static _Thread_local uint64_t g_stub_last_pc, g_stub_last_ns;
static _Thread_local int g_stub_repeats;

static bool redirect_to_exec_stub(ucontext_t *u)
{
    if (!u || g_stub.active)
        return false;
    g_stub.ss = u->uc_mcontext->__ss;
    g_stub.ns = u->uc_mcontext->__ns;
    g_stub.active = true;
    // Below the red zone; the stub calls one libSystem function.
    u->uc_mcontext->__ss.__sp = (u->uc_mcontext->__ss.__sp - 1024) & ~15ull;
    u->uc_mcontext->__ss.__pc = (uint64_t)(uintptr_t)lxrt_jit_exec_stub;
    return true;
}

bool lxrt_jit_stub_trap(void *uap)
{
    ucontext_t *u = uap;
    if (!u || !g_stub.active ||
        u->uc_mcontext->__ss.__pc != (uint64_t)(uintptr_t)lxrt_jit_exec_stub_trap)
        return false;
    u->uc_mcontext->__ss = g_stub.ss;
    u->uc_mcontext->__ns = g_stub.ns;
    g_stub.active = false;
    g_writable = false;
    g_stub_last_pc = g_stub.ss.__pc;
    g_stub_last_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    return true;
}

// Called from the runtime's fault handler before anything is delivered to the
// guest. Returns true if the fault was a JIT mode transition and the faulting
// instruction should simply be retried.
bool lxrt_jit_handle_fault(uint64_t pc, uint64_t fault_addr, void *uap)
{
    int idx = region_of(fault_addr);
    if (lxrt_trace_on()) {
        // A fault outside every registered region is the interesting case:
        // say what that memory actually IS rather than only that it is not
        // ours.
        mach_vm_address_t ra = fault_addr;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t ri;
        mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        char what[128] = "unmapped";
        if (fault_addr && mach_vm_region(mach_task_self(), &ra, &rs,
                                         VM_REGION_BASIC_INFO_64,
                                         (vm_region_info_t)&ri, &rc, &obj)
                == KERN_SUCCESS)
            snprintf(what, sizeof what, "region 0x%llx+0x%llx prot=%c%c%c max=%c%c%c %s",
                     (unsigned long long)ra, (unsigned long long)rs,
                     (ri.protection & VM_PROT_READ) ? 'r' : '-',
                     (ri.protection & VM_PROT_WRITE) ? 'w' : '-',
                     (ri.protection & VM_PROT_EXECUTE) ? 'x' : '-',
                     (ri.max_protection & VM_PROT_READ) ? 'r' : '-',
                     (ri.max_protection & VM_PROT_WRITE) ? 'w' : '-',
                     (ri.max_protection & VM_PROT_EXECUTE) ? 'x' : '-',
                     ri.shared ? "shared" : "private");
        Dl_info di;
        const char *sym = (dladdr((void *)pc, &di) && di.dli_sname) ? di.dli_sname : "?";
        fprintf(lxrt_trace_stream(), "[lxrt]    jit fault pid=%d pc=0x%llx (%s) addr=0x%llx region=%d "
                        "writable=%d [%s]\n", (int)getpid(), (unsigned long long)pc, sym,
                (unsigned long long)fault_addr, idx, (int)g_writable, what);
    }
    if (idx < 0)
        return false;

    if (pc == fault_addr) {
        // Instruction fetch from a JIT page while the thread is in write mode.
        // Either the flag says execute mode and the hardware disagrees (a
        // signal handler redirected a thread that was interrupted in write
        // mode: sigreturn put write mode back), or the flag says write mode.
        // A flip made here would not survive sigreturn (measured, see the
        // stub in trampoline.S), so the flip is made by the thread itself in
        // the stub, and the retry happens from there.
        // Unmapped or not executable (a code buffer another thread already
        // freed): a real fault. And a fault at the same pc straight after
        // the stub ran means execute mode did not help either.
        if (!mapped_executable(pc))
            return false;
        // The same pc faulting again within 10 ms of the stub restoring it,
        // three times running, is not a mode problem -- unless the thread
        // really is in write mode again: then something wrote JIT memory in
        // between (FEX writing a guest trampoline outside its write scopes,
        // three thunk calls in a row returning to the same block: MEASURED,
        // 32-bit DXVK died of a SIGBUS there), and the flip is the fix.
        uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (!g_writable && g_stub_last_pc == pc && now - g_stub_last_ns < 10000000ull) {
            if (++g_stub_repeats >= 3) {
                g_stub_repeats = 0;
                return false;
            }
        } else {
            g_stub_repeats = 0;
        }
        if (g_writable)
            rescan(idx);
        if (redirect_to_exec_stub(uap))
            return true;
        lxrt_jit_protect(1);   // no context to redirect: best effort
        g_writable = false;
        return true;
    }

    // A store into a JIT page while the thread can execute it.
    if (g_writable)
        return false;           // writable already: a genuine fault
    lxrt_jit_protect(0);
    g_writable = true;
    note_written(idx, fault_addr);
    return true;
}
