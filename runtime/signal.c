// Guest signal delivery.
//
// Three things differ between Linux and Darwin here, and all three are silent
// if got wrong:
//
//   1. The numbers. They agree up to SIGABRT and then diverge completely --
//      SIGBUS is 7 on Linux and 10 on Darwin, SIGUSR1 is 10 and 30, SIGCHLD is
//      17 and 20. A pass-through delivers a different signal, not a wrong one.
//   2. The frame. A Linux handler with SA_SIGINFO expects a Linux siginfo_t and
//      a Linux ucontext_t at a fixed layout, not Darwin's.
//   3. The return path. Linux enters a handler with lr pointing at a restorer
//      that issues rt_sigreturn. Since `svc` cannot be used here (Stage 1), lr
//      points into the runtime instead.
//
// The mechanism is the one the kernel itself uses: the host handler does not
// call the guest handler, it *rewrites the interrupted context* so that
// returning from it resumes inside the guest handler with a Linux frame on the
// guest stack.

#include "lxrt.h"
#include "ids.h"
static _Atomic int g_xsig_sender[65536];
#include "x18.h"

#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <unistd.h>

bool lxrt_trace_on(void);

#define LERR(e) (-lxrt_errno_to_linux(e))

#define LINUX_NSIG      64
#define LINUX_SIGRTMIN  32

// ---------------------------------------------------------------- numbers

// Indexed by Linux signal number. 0 means "no Darwin equivalent".
static const int k_linux_to_darwin[32] = {
    [1]  = SIGHUP,  [2]  = SIGINT,  [3]  = SIGQUIT, [4]  = SIGILL,
    [5]  = SIGTRAP, [6]  = SIGABRT, [7]  = SIGBUS,  [8]  = SIGFPE,
    [9]  = SIGKILL, [10] = SIGUSR1, [11] = SIGSEGV, [12] = SIGUSR2,
    [13] = SIGPIPE, [14] = SIGALRM, [15] = SIGTERM,
    [16] = 0,        // Linux SIGSTKFLT has no Darwin equivalent
    [17] = SIGCHLD, [18] = SIGCONT, [19] = SIGSTOP, [20] = SIGTSTP,
    [21] = SIGTTIN, [22] = SIGTTOU, [23] = SIGURG,  [24] = SIGXCPU,
    [25] = SIGXFSZ, [26] = SIGVTALRM, [27] = SIGPROF, [28] = SIGWINCH,
    [29] = SIGIO,
    [30] = 0,        // Linux SIGPWR has no Darwin equivalent
    [31] = SIGSYS,
};

int lxrt_signo_to_darwin(int linux_sig)
{
    if (linux_sig <= 0 || linux_sig >= 32)
        return 0;
    return k_linux_to_darwin[linux_sig];
}

int lxrt_signo_to_linux(int darwin_sig)
{
    for (int i = 1; i < 32; i++)
        if (k_linux_to_darwin[i] == darwin_sig)
            return i;
    return 0;
}

// Linux has 32 realtime signals (32..63) and Darwin has none. They are still
// deliverable here, because every send and every receive passes through this
// runtime: the signal number is queued against the target thread and a carrier
// signal wakes it.
//
// SIGEMT is the carrier precisely because no Linux signal maps onto it --
// Linux's SIGBUS (7) becomes Darwin's SIGBUS (10), so Darwin's SIGEMT (7) is
// left unclaimed and cannot collide with a guest's own handler.
#define LXRT_RT_CARRIER SIGEMT

static bool is_rt(int lsig) { return lsig >= LINUX_SIGRTMIN && lsig <= LINUX_NSIG; }
static bool xsig_send(int pid, int tid, int lsig);
static void xsig_drain(void);

// Realtime signals are carried, not raised (see LXRT_RT_CARRIER), so the host
// signal mask cannot block them: their Linux mask bits live here, per thread.
// Before this they were simply dropped from every mask -- a guest (FEX) that
// masked everything still took its SIG63 pause request, and the thread later
// hung on the pause's `hlt` return with SIGILL blocked (Steam's
// "IPC:CSteamEngine" thread, measured: 100 % CPU on one hlt, the client's
// 15-second main-loop watchdog then killed it).
#define RT_BITS (~0ull << (LINUX_SIGRTMIN - 1))
static _Thread_local uint64_t g_rt_blocked;
// Synchronous faults are never held off on the host. Linux force-delivers a
// SIGILL/SIGSEGV/SIGBUS/SIGFPE/SIGTRAP raised by the faulting instruction
// even when it is blocked (force_sig_fault unblocks it); Darwin leaves it
// pending and re-executes the instruction forever. FEX blocks everything
// around its pause handler and returns from it through a `hlt` it expects to
// see as SIGILL -- the Steam client's IPC thread spun on that `hlt` at 100 %
// CPU (measured). Their Linux-visible mask bits are kept here instead.
#define SYNC_BITS ((1ull << (4 - 1)) | (1ull << (5 - 1)) | (1ull << (7 - 1)) | \
                   (1ull << (8 - 1)) | (1ull << (11 - 1)))
static _Thread_local uint64_t g_sync_blocked;
static void linux_mask_to_darwin(uint64_t lmask, sigset_t *out);
static uint64_t darwin_mask_to_linux(const sigset_t *in);
// A new thread starts with its creator's mask (clone), realtime part included.
uint64_t lxrt_rt_mask_get(void) { return g_rt_blocked; }
void lxrt_rt_mask_set(uint64_t m) { g_rt_blocked = m & RT_BITS; }
static void rt_kick_if_pending(void)
{
    if (lxrt_rt_pending_unblocked(g_rt_blocked))
        pthread_kill(pthread_self(), LXRT_RT_CARRIER);
}

// si_code: the seventh silent divergence.
//
// The FAULT codes agree -- SEGV_MAPERR/ACCERR are 1/2 on both, BUS_ADRALN/
// ADRERR/OBJERR are 1/2/3 on both -- but the "who sent this" family does not:
//
//   origin        Linux   Darwin
//   SI_USER           0   0x10001
//   SI_QUEUE         -1   0x10002
//   SI_TIMER         -2   0x10003
//   SI_MESGQ         -3   0x10005
//   SI_ASYNCIO       -4   0x10004
//   SI_KERNEL      0x80   (none)
//   SI_TKILL         -6   (none)
//
// A handler that distinguishes "a real fault" from "someone raised this at me"
// reads si_code and nothing else. FEX does exactly that: it raises SIGBUS at
// itself to probe its self-modifying-code path, and with an untranslated code
// it cannot tell that probe from a genuine bus error.
static int si_code_to_linux(int dsig, int dcode)
{
    switch (dcode) {
    case 0x10001: return 0;     // SI_USER
    case 0x10002: return -1;    // SI_QUEUE
    case 0x10003: return -2;    // SI_TIMER
    case 0x10004: return -4;    // SI_ASYNCIO
    case 0x10005: return -3;    // SI_MESGQ
    default: break;
    }
    // Everything else is a per-signal fault code, and those numbers agree.
    // Guard the range anyway: a code this runtime does not recognise is
    // reported as SI_KERNEL rather than passed through as a number that means
    // something else on the other side.
    if (dcode >= 1 && dcode <= 8)
        return dcode;
    (void)dsig;
    return 0x80;                // SI_KERNEL
}

// KNOWN IMPRECISION, stated rather than papered over: a signal this runtime
// delivers through pthread_kill arrives with Darwin's SI_USER and is reported
// as Linux SI_USER (0). Linux would report SI_TKILL (-6) when it came from
// tgkill, which is what glibc's raise() uses. Distinguishing them would need
// the sender to tag the target thread before signalling, and that tag races
// with a second signal in flight. Both values are <= 0, so a handler asking
// "was this sent or was it a fault" -- the question every handler actually
// asks -- gets the right answer either way.

// ---------------------------------------------------------------- state

struct guest_sigaction {
    uint64_t handler;   // guest address, or SIG_DFL(0) / SIG_IGN(1)
    uint64_t flags;     // Linux SA_*
    uint64_t restorer;
    uint64_t mask;      // low 64 bits of the Linux sigset
};

#define LINUX_SA_SIGINFO  0x00000004u
#define LINUX_SA_ONSTACK  0x08000000u
#define LINUX_SA_RESTART  0x10000000u
_Thread_local int lxrt_sig_during_syscall;   // see dispatch.c, SA_RESTART
#define LINUX_SA_NODEFER  0x40000000u
#define LINUX_SA_RESETHAND 0x80000000u

static struct guest_sigaction g_actions[LINUX_NSIG + 1];
static pthread_mutex_t g_actions_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(signal_g_actions_lock, g_actions_lock)

// Per-thread alternate signal stack, as the guest declared it.
struct guest_altstack { uint64_t sp; uint64_t flags; uint64_t size; };
static _Thread_local struct guest_altstack g_altstack;
// W^X window state at each pending signal delivery, innermost last. Pushed
// when a guest handler is entered, popped by its sigreturn.

// ---------------------------------------------------------------- frame

// Linux aarch64 sigcontext, from the kernel's uapi headers. Offsets are ABI.
struct linux_sigcontext {
    uint64_t fault_address;   // 0
    uint64_t regs[31];        // 8    x0..x30
    uint64_t sp;              // 256
    uint64_t pc;              // 264
    uint64_t pstate;          // 272
    uint8_t  reserved[4096];  // 280  extension records; all-zero = end marker
};

// Linux's first extension record in sigcontext.__reserved: the FP/SIMD state.
#define LINUX_FPSIMD_MAGIC 0x46508001u
struct linux_fpsimd_context {
    uint32_t magic;           // 0
    uint32_t size;            // 4   528
    uint32_t fpsr;            // 8
    uint32_t fpcr;            // 12
    __uint128_t vregs[32];    // 16
};
_Static_assert(sizeof(struct linux_fpsimd_context) == 528, "fpsimd_context is 528 bytes");
struct linux_ucontext {
    uint64_t uc_flags;                 // 0
    uint64_t uc_link;                  // 8
    struct { uint64_t ss_sp; int32_t ss_flags; int32_t pad; uint64_t ss_size; }
             uc_stack;                 // 16
    uint8_t  uc_sigmask[128];          // 40   Linux sigset_t is 1024 bits
    uint8_t  pad_[8];                  // 168  align uc_mcontext to 176
    struct linux_sigcontext uc_mcontext; // 176 -- this offset is ABI
};

// Linux siginfo_t is 128 bytes. The first three fields are common to every
// signal; after them comes a union, and for the fault signals -- SIGSEGV,
// SIGBUS, SIGFPE, SIGILL, SIGTRAP -- its first member is si_addr at offset 16.
//
// That field is not optional. A handler that exists to FIX a fault reads it to
// learn which address faulted. Leaving it zero made FEX's SIGSEGV handler --
// which maps pages on demand and detects self-modifying code -- unable to fix
// anything, so it returned, faulted again at the same pc, and looped until the
// stack was gone.
struct linux_siginfo {
    int32_t si_signo;
    int32_t si_errno;
    int32_t si_code;
    int32_t pad_;
    union {
        uint64_t si_addr;   // fault signals
        struct { int32_t si_pid, si_uid; }; // sent signals and SIGCHLD
    };
    uint8_t  rest[104];
};

struct rt_sigframe {
    struct linux_siginfo info;
    struct linux_ucontext uc;
};

// rt_tgsigqueueinfo / rt_sigqueueinfo to a thread of this process: the
// caller's siginfo, kept here until the signal is delivered to that thread
// and then handed to its handler whole. FEX's seccomp emulation raises
// SIGSYS this way with si_code SYS_SECCOMP, si_call_addr, si_syscall and
// si_arch; delivered with tgkill's siginfo instead, Chromium's sandbox (a
// WebView renderer) found "sanity checks failing after receiving SIGSYS"
// and killed itself (MEASURED).
#define QINFO_SLOTS 64
static struct { _Atomic int state; int tid, lsig; struct linux_siginfo info; } g_qinfo[QINFO_SLOTS];
static bool qinfo_put(int tid, int lsig, const void *linfo)
{
    for (int i = 0; i < QINFO_SLOTS; i++) {
        int z = 0;
        if (atomic_compare_exchange_strong(&g_qinfo[i].state, &z, 1)) {
            g_qinfo[i].tid = tid;
            g_qinfo[i].lsig = lsig;
            memcpy(&g_qinfo[i].info, linfo, sizeof g_qinfo[i].info);
            atomic_store(&g_qinfo[i].state, 2);
            return true;
        }
    }
    return false;
}
static bool qinfo_take(int tid, int lsig, struct linux_siginfo *out)
{
    for (int i = 0; i < QINFO_SLOTS; i++) {
        if (atomic_load(&g_qinfo[i].state) != 2 || (g_qinfo[i].tid != tid && g_qinfo[i].tid != 0) ||
            g_qinfo[i].lsig != lsig)
            continue;          // tid 0: process-directed, whichever thread takes it
        int two = 2;
        if (!atomic_compare_exchange_strong(&g_qinfo[i].state, &two, 3))
            continue;
        memcpy(out, &g_qinfo[i].info, sizeof *out);
        atomic_store(&g_qinfo[i].state, 0);
        return true;
    }
    return false;
}

_Static_assert(__builtin_offsetof(struct linux_ucontext, uc_mcontext) == 176,
               "Linux aarch64 puts uc_mcontext at 176; the guest reads it there");

// Implemented in trampoline.S. ctx[0..30] = x0..x30, ctx[31] = sp, ctx[32] = pc.
void lxrt_restore_regs(const uint64_t *ctx33) __attribute__((noreturn));
// The address a guest handler returns to, also in trampoline.S.
void lxrt_sigreturn_entry(void);
// trampoline.S: switch sp to the guest frame, call handler(sig, info, uc), switch back.
void lxrt_call_guest_handler(uint64_t handler, uint64_t sig, uint64_t info,
                             uint64_t uc, uint64_t new_sp);

// Darwin and Linux disagree on which signal a PROTECTION violation is.
//
// Writing to a mapped-but-read-only page is SIGSEGV with SEGV_ACCERR on Linux.
// Darwin reports it as SIGBUS. That is not cosmetic: FEX installs a SIGSEGV
// handler precisely for this case -- it is how its self-modifying-code
// tracking works, mprotecting the page writable and retrying -- and a separate
// SIGBUS handler that gives up and re-raises with the default disposition.
// Measured: FEX died exactly there, on a store into its own guest text mapping
// at a region reported as prot=r-x max=rwx.
//
// So a SIGBUS whose address lies in a mapped region that COULD have permitted
// the access (max_protection allows more than protection) is reported to the
// guest as SIGSEGV/SEGV_ACCERR, which is what Linux would have reported. A
// SIGBUS on unmapped or truly unbacked memory stays a SIGBUS.
static bool protection_fault(uint64_t addr, int *out_lsig, int *out_code)
{
    if (!addr)
        return false;
    mach_vm_address_t ra = addr;
    mach_vm_size_t rs = 0;
    vm_region_basic_info_data_64_t ri;
    mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS)
        return false;
    if (addr < ra || addr >= ra + rs)
        return false;                       // the region found is past it
    if (ri.max_protection == ri.protection)
        return false;                       // nothing was withheld
    *out_lsig = 11;                         // SIGSEGV
    *out_code = 2;                          // SEGV_ACCERR, same number on both
    return true;
}

// ------------------------------------------------------- host alt stack
//
// The HOST handler must not run on the guest's stack. FEX's signal handler
// builds the x86 guest's signal frame on the guest stack below the
// interrupted sp -- legitimately, that memory is the guest's -- and that is
// exactly where Darwin's signal frame and this runtime's handler frames sat.
// Measured: with delivery made synchronous, the handler's saved x19/x29/x30
// were overwritten by FEX's frame and the return went to a data address.
// The kernel avoids this with sigaltstack; so does the runtime, one per
// guest thread, and every host sigaction carries SA_ONSTACK.
enum { HOST_ALTSTACK_SIZE = 512 * 1024 };
static _Thread_local void *g_host_altstack;
void lxrt_host_altstack_install(void)
{
    if (g_host_altstack)
        return;
    void *p = mmap(NULL, HOST_ALTSTACK_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return;
    stack_t st = { .ss_sp = p, .ss_size = HOST_ALTSTACK_SIZE, .ss_flags = 0 };
    if (sigaltstack(&st, NULL) == 0)
        g_host_altstack = p;
    else
        munmap(p, HOST_ALTSTACK_SIZE);
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] host altstack for thread %llx: %p (%s)\n",
                (unsigned long long)(uintptr_t)pthread_self(), g_host_altstack,
                g_host_altstack ? "installed" : strerror(errno));
}

// Darwin's fork() gives the child a thread with NO alternate stack (measured:
// sigaltstack(NULL, &st) in the child reports SS_DISABLE while the parent's
// is armed), but the child inherits this thread-local pointer, so the install
// above would believe the stack is there. The mapping itself survived the
// fork; re-arm it. Without this, every host signal in a forked FEX process
// landed on the guest thread's stack, FEX's handler wrote its context backup
// over the host frames, and dash's subshells died with SIGILL/SIGSEGV --
// visible as "Illegal instruction" from the parent shell and lost output.
void lxrt_host_altstack_after_fork(void)
{
    if (!g_host_altstack) {
        lxrt_host_altstack_install();
        return;
    }
    stack_t st = { .ss_sp = g_host_altstack, .ss_size = HOST_ALTSTACK_SIZE, .ss_flags = 0 };
    if (sigaltstack(&st, NULL) != 0 && lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] host altstack re-arm after fork failed: %s\n", strerror(errno));
}

// ---------------------------------------------------------------- delivery

// LXRT_SIGSTATS=1: count the host signals this process takes, and print the
// tally every 5 s -- which signal keeps interrupting sleeps, for instance.
static _Atomic uint32_t g_sigstats[33];
static _Atomic uint64_t g_sigstats_last;
static int sigstats_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("LXRT_SIGSTATS") ? 1 : 0;
    return on;
}
static void sigstats_print(const char *when)
{
    char buf[512]; int n = snprintf(buf, sizeof buf, "[lxrt] pid %d sigstats%s:", (int)getpid(), when);
    for (int i = 1; i <= 32 && n < (int)sizeof buf - 24; i++) {
        uint32_t c = atomic_exchange(&g_sigstats[i], 0);
        if (c) n += snprintf(buf + n, sizeof buf - n, " %d:%u", i, c);
    }
    snprintf(buf + n, sizeof buf - n, "\n");
    fputs(buf, lxrt_trace_stream());
}
static _Atomic uint64_t g_x18br_restarts, g_x18br_zero, g_x18br_signal;   // br x18, below
static int x18_stats_on(void);
// exit_group: whatever the last 5 s window collected.
void lxrt_sigstats_flush(void)
{
    lxrt_wx_stats_flush();
    if (x18_stats_on())
        fprintf(lxrt_trace_stream(), "[lxrt] pid %d x18 br at exit: %llu restarts after a fault, %llu branches "
                "to 0 recovered, %llu restarts at signal delivery\n", (int)getpid(),
                (unsigned long long)atomic_load(&g_x18br_restarts), (unsigned long long)atomic_load(&g_x18br_zero),
                (unsigned long long)atomic_load(&g_x18br_signal));
    if (sigstats_on())
        sigstats_print(" at exit");
}
static void sigstats_note(int dsig)
{
    if (!sigstats_on() || dsig <= 0 || dsig > 32)
        return;
    atomic_fetch_add(&g_sigstats[dsig], 1);
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t last = atomic_load(&g_sigstats_last);
    if (now - last > 5000000000ull && atomic_compare_exchange_strong(&g_sigstats_last, &last, now))
        sigstats_print("");
}

// The last few host signals this thread took, for fault reports: a signal
// landing between a FEX helper's return and the dispatcher's `br x0` would
// explain a jump to a garbage address.
static _Thread_local struct { int sig; uint64_t pc, x0, ns; } g_lastsig[4];
static _Thread_local unsigned g_lastsig_n;

// Was this SIGSEGV/SIGBUS/SIGTRAP raised by the instruction at pc (a fault,
// which Linux force-delivers) rather than sent with kill/tgkill? Darwin's
// si_code cannot say: a kill()ed SIGSEGV arrives as SEGV_ACCERR and a
// pthread_kill()ed SIGBUS as BUS_ADRALN (MEASURED, macOS 27 arm64). The
// exception state can: a sent signal carries the syndrome of the thread's
// last trap into the kernel -- the sending syscall's svc (EC 0x15) when it
// signalled itself -- and si_addr 0, where a data or instruction abort
// carries its own EC and si_addr == FAR.
static bool raised_by_instruction(int dsig, const siginfo_t *dinfo, void *uap)
{
    if (!dinfo || !uap)
        return false;
    const ucontext_t *u = (const ucontext_t *)uap;
    uint32_t ec = u->uc_mcontext->__es.__esr >> 26;
    uint64_t far = u->uc_mcontext->__es.__far;
    switch (ec) {
    case 0x20: case 0x21:                   // instruction abort
    case 0x24: case 0x25:                   // data abort
        return (dsig == SIGSEGV || dsig == SIGBUS) &&
               (uint64_t)(uintptr_t)dinfo->si_addr == far;
    case 0x22: case 0x26:                   // pc / sp alignment
        return dsig == SIGBUS;
    case 0x3c:                              // brk
        return dsig == SIGTRAP;
    default:
        return false;
    }
}

// ---------------------------------------------------------------- br x18
//
// The `br x18` trampoline (runtime/x18.c plan_br, x18.h) branches through the
// hardware x18, loaded from the virtual one in its last three instructions.
// Where the kernel zeroes x18 on an exception return (every process of the
// default build, and every forked child of the keep-x18 build: stage 28), an
// exception between those instructions leaves x18 = 0. Two outcomes, both
// recovered here, and one precaution at signal delivery:
//   * the `ldr x18, [x18, #slot]` faults on a page-zero address: restart the
//     trampoline from its first word (x16, x17 and sp are the guest's again
//     by then, so it is idempotent);
//   * the `br x18` jumps to 0: the trampoline left {its D, the target} at
//     sp-32 just before; when that D is a trampoline's and the target is
//     still the virtual x18, resume at the target;
//   * a guest signal handler interrupting A..D would build its frame over
//     that mark: the trampoline is restarted instead (host_handler).
// Every case has pc inside a trampoline pool (or 0) and the hardware x18 = 0,
// so the words are read only then: this chain also sees every W^X flip.
// (counters declared above lxrt_sigstats_flush)

static bool read_words(uint64_t a, void *out, unsigned bytes)
{
    mach_vm_size_t got = 0;
    return a >= 0x1000 && mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)a, bytes,
                                                 (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS &&
           got == bytes;
}

// pc on A..D of a br x18 trampoline: the trampoline's first word; else 0.
static uint64_t x18_br_restart_pc(uint64_t pc)
{
    uint32_t w[7] = {0};
    if (pc < 0x100000000ull || (pc & 3) || !lxrt_pool_maybe(pc) || !read_words(pc - 12, w, 16))
        return 0;
    (void)read_words(pc + 4, w + 4, 12);        // D may end a mapping: then zeros
    unsigned back = lxrt_x18_br_restart(w);
    return back ? pc - back : 0;
}

// The branch to 0: resume at the target the trampoline marked, or 0.
static uint64_t x18_br_zero_target(uint64_t sp)
{
    uint64_t m[2];
    uint32_t w[4];
    if (!read_words(sp - X18_BR_MARK_BELOW_SP, m, sizeof m) || m[0] < 0x100000000ull ||
        !read_words(m[0] - 12, w, sizeof w) || !lxrt_x18_br_tail(w) || m[1] != lxrt_x18_get())
        return 0;
    uint64_t zero = 0;   // used once: a later jump to 0 is the guest's own
    (void)mach_vm_write(mach_task_self(), (mach_vm_address_t)(sp - X18_BR_MARK_BELOW_SP),
                        (vm_offset_t)(uintptr_t)&zero, sizeof zero);
    return m[1];
}

static int x18_stats_on(void)
{
    static int on = -1;
    if (on < 0) on = getenv("LXRT_X18_STATS") ? 1 : 0;
    return on;
}

// The runtime's own synchronous faults, absorbed before anything is reported
// or delivered: the x18 trampolines' sp-alignment faults, MAP_JIT and W^X
// flips (jit.c, wxsplit.c, subpage.c), copy-on-write of private shared
// memory (privmap.c), unbased low pointers (gbase.c). True: resume the thread.
//
// ONE chain, shared by host_handler and main.c's fault_report (the handler
// until the guest installs its own action for the signal). They were two
// copies, and fault_report's had lost the alignment filter: a misaligned
// store-release into a W^X page, taken for a write flip, retried forever
// (stage 23 review; tests/elf/wx_owner.c, "misaligned").
bool lxrt_absorb_runtime_fault(int dsig, siginfo_t *dinfo, void *uap)
{
    if ((dsig == SIGSEGV || dsig == SIGBUS) && uap &&
        ((ucontext_t *)uap)->uc_mcontext->__ss.__x[18] == 0) {
        _STRUCT_ARM_THREAD_STATE64 *ts = &((ucontext_t *)uap)->uc_mcontext->__ss;
        uint64_t to = ts->__pc ? x18_br_restart_pc(ts->__pc) : x18_br_zero_target(ts->__sp);
        if (to) {
            atomic_fetch_add(ts->__pc ? &g_x18br_restarts : &g_x18br_zero, 1);
            ts->__pc = to;
            return true;
        }
    }
    // A thread pointer read the rewriter left in place (tls.c, kept ranges:
    // BoringSSL's FIPS module) returned Darwin's TPIDR_EL0 and the load
    // through it faulted: substitute the guest's and retry.
    if (lxrt_tlskeep_fixup(dsig, uap))
        return true;
    // SP-alignment fault (EC 0x26) on the x18 trampolines' register save and
    // restore: `stp Xa, Xb, [sp, #-16]!` / `ldp Xa, Xb, [sp], #16`. Code that
    // keeps sp misaligned between accesses is legal (pixman's hand-written
    // NEON did, under Xvnc); the trampoline's push through sp is what the
    // hardware refuses. Emulate those two forms here and resume -- Linux
    // would never execute them, only the rewriter emits them.
    if (dsig == SIGBUS && uap) {
        ucontext_t *u = (ucontext_t *)uap;
        uint32_t esr = u->uc_mcontext->__es.__esr;
        if ((esr >> 26) == 0x26) {
            _STRUCT_ARM_THREAD_STATE64 *ts = &u->uc_mcontext->__ss;
            uint32_t w = *(const uint32_t *)(uintptr_t)ts->__pc;
            unsigned rt = w & 31, rn = (w >> 5) & 31, rt2 = (w >> 10) & 31;
            int64_t imm = (int64_t)((int32_t)(((w >> 15) & 0x7f) << 25) >> 25) * 8;
            uint64_t *x = ts->__x;   // x0..x28; x29 = fp, x30 = lr
            #define REG(r) ((r) == 29 ? &ts->__fp : (r) == 30 ? &ts->__lr : &x[(r)])
            if (rn == 31 && (w & 0xFFC00000u) == 0xA9800000u) {         // STP pre-index
                uint64_t a = ts->__sp + (uint64_t)imm;
                memcpy((void *)(uintptr_t)a, REG(rt), 8);
                memcpy((void *)(uintptr_t)(a + 8), REG(rt2), 8);
                ts->__sp = a;
                ts->__pc += 4;
                return true;
            }
            if (rn == 31 && (w & 0xFFC00000u) == 0xA8C00000u) {         // LDP post-index
                uint64_t a = ts->__sp;
                memcpy(REG(rt), (void *)(uintptr_t)a, 8);
                memcpy(REG(rt2), (void *)(uintptr_t)(a + 8), 8);
                ts->__sp = a + (uint64_t)imm;
                ts->__pc += 4;
                return true;
            }
            #undef REG
        }
    }

    // An alignment fault (data abort, DFSC 0b100001) is never about page
    // protection. It has to reach the guest untouched -- FEX backpatches
    // unaligned atomics from SIGBUS -- so the protection handlers below must
    // not see it: on a 16 KiB page whose guest pages union to RWX the sub-page
    // handler "fixed" it with an mprotect and retried, forever (MEASURED: a
    // steamwebhelper renderer at ~440k SIGBUS/s on one unaligned lock op in
    // V8's code space, the Steam UI never loading).
    bool align_fault = false;
    if (dsig == SIGBUS && uap) {
        uint32_t esr = ((ucontext_t *)uap)->uc_mcontext->__es.__esr;
        uint32_t ec = esr >> 26;
        align_fault = (ec == 0x24 || ec == 0x25) && (esr & 0x3f) == 0x21;
    }
    if ((dsig == SIGBUS || dsig == SIGSEGV) && dinfo && uap && !align_fault) {
        ucontext_t *u = (ucontext_t *)uap;
        uint64_t fpc = u->uc_mcontext->__ss.__pc;
        uint64_t faddr = (uint64_t)(uintptr_t)dinfo->si_addr;
        // A thread faulting on the same address and pc over and over is not
        // making progress: say once who last changed that page (memlog.c).
        static _Thread_local uint64_t last_fa, last_pc;
        static _Thread_local unsigned repeats;
        // SIGSEGV only: FEX emulates split-lock atomics from SIGBUS, and a
        // guest spinning on one repeats the same pc/address legitimately.
        if (dsig == SIGSEGV && faddr == last_fa && fpc == last_pc) {
            if (++repeats == 1000) {
                lxrt_memlog_dump(faddr, "fault repeating 1000x");
                uint64_t ib = lxrt_main_image_base, ie = ib + lxrt_main_image_span;
                uint64_t flr = u->uc_mcontext->__ss.__lr;
                fprintf(lxrt_trace_stream(), "[lxrt]   faulting pc 0x%llx%s+0x%llx lr 0x%llx%s+0x%llx "
                        "x0 0x%llx x1 0x%llx x2 0x%llx x19 0x%llx x28 0x%llx\n",
                        (unsigned long long)fpc, fpc >= ib && fpc < ie ? " = image" : " (not image)",
                        (unsigned long long)(fpc >= ib && fpc < ie ? fpc - ib : 0),
                        (unsigned long long)flr, flr >= ib && flr < ie ? " = image" : " (not image)",
                        (unsigned long long)(flr >= ib && flr < ie ? flr - ib : 0),
                        (unsigned long long)u->uc_mcontext->__ss.__x[0], (unsigned long long)u->uc_mcontext->__ss.__x[1],
                        (unsigned long long)u->uc_mcontext->__ss.__x[2], (unsigned long long)u->uc_mcontext->__ss.__x[19],
                        (unsigned long long)u->uc_mcontext->__ss.__x[28]);
            }
        } else {
            last_fa = faddr;
            last_pc = fpc;
            repeats = 0;
        }
        if (lxrt_jit_handle_fault(fpc, faddr, uap))
            return true;
        // A native guest's RWX page (V8's code range): the W^X flip.
        if (lxrt_wx_handle_fault(fpc, faddr, u->uc_mcontext->__es.__esr))
            return true;
        lxrt_jit_report_freed(fpc, faddr, uap);
        // A store into a copy-on-write page of a private shared-memory
        // mapping (privmap.c). WnR, ESR bit 6, on a data abort.
        uint32_t fesr = u->uc_mcontext->__es.__esr;
        bool fwrite = ((fesr >> 26) == 0x24 || (fesr >> 26) == 0x25) && (fesr & (1u << 6));
        if (lxrt_privmap_handle_fault(faddr, fwrite))
            return true;
        if (lxrt_lowptr_fixup(uap, faddr))
            return true;
        // Ordinary pages get the same treatment when a 16 KiB host page holds
        // both a writable and an executable guest page.
        if (lxrt_subpage_handle_fault(fpc, faddr, uap))
            return true;
    }
    return false;
}

// ------------------------------------------------ signals no thread took
//
// XNU binds a process-directed signal (kill() of the process, a child's
// SIGCHLD) to ONE thread when it is posted: the first thread that does not
// block it or waits for it in sigwait -- and when every thread blocks it, the
// process's first thread, where it then stays (MEASURED on macOS 27,
// benchmarks/stage28-android-reliability.txt: main blocks everything, a
// second thread blocks SIGCHLD, a child exits, the second thread unblocks or
// sigsuspends -- nothing; sigpending() on main shows it). Linux keeps such a
// signal in the process's shared pending set and delivers it to the first
// thread that unblocks it. Here the first thread is the host main thread,
// which blocks every signal for good (main.c), so a SIGCHLD that arrived while
// the guest had it blocked -- or during one of the runtime's own all-signals-
// blocked sections (thread.c threads_lock, taken on every rt_sigprocmask) --
// was lost: a shell that blocks SIGCHLD around fork and waits in sigsuspend
// slept forever, 12 of 300 runs of x86-64 Android's `sh -c 'x=$(toybox ...)'`
// under FEX (stage 27's intermittent hang; tests/elf/sig_stranded.c).
//
// So the main thread looks at its own pending set on every turn of its loop
// (window.m, at most 20 ms apart), and takes each signal it finds there that
// has the runtime's handler by unblocking it for a moment: host_handler then
// runs on the main thread, sees it is not a guest thread, and hands the
// signal to a guest thread with pthread_kill -- one whose noted mask accepts
// it, else the first guest thread, where it stays pending until unblocked, as
// Linux would keep it. A signal at SIG_DFL (terminate, stop) is left where it
// is: taking it on the main thread would act on it even though the guest
// blocks it. The same hand-over serves any other non-guest thread that ends
// up with one (a thread Darwin's frameworks started with an open mask).
static bool is_sync_dsig(int d)
{
    return d == SIGSEGV || d == SIGBUS || d == SIGILL || d == SIGFPE || d == SIGTRAP || d == SIGSYS;
}
static _Atomic uint32_t g_forwarded;
static void forward_stray(int dsig)
{
    int lsig = dsig == LXRT_RT_CARRIER ? 0 : lxrt_signo_to_linux(dsig);
    pthread_t t;
    bool have = lsig ? lxrt_thread_signal_target(lsig, &t) : lxrt_main_guest_thread(&t);
    atomic_fetch_add(&g_forwarded, 1);
    if (have && !pthread_equal(t, pthread_self()))
        pthread_kill(t, dsig);
    if (lxrt_trace_on()) {
        char b[160];
        int n = snprintf(b, sizeof b, "[lxrt] pid %d: darwin signal %d reached a non-guest thread; %s\n",
                         (int)getpid(), dsig, have ? "handed to a guest thread" : "no guest thread to take it");
        write(2, b, (size_t)(n > 0 && n < (int)sizeof b ? n : 0));
    }
}
static void host_handler(int dsig, siginfo_t *dinfo, void *uap);
void lxrt_signal_rescue_stranded(void)
{
    sigset_t pend;
    if (sigpending(&pend) != 0)
        return;
    sigset_t take;
    sigemptyset(&take);
    bool any = false;
    for (int d = 1; d < NSIG; d++) {
        if (!sigismember(&pend, d) || d == SIGKILL || d == SIGSTOP || is_sync_dsig(d))
            continue;
        struct sigaction cur;
        if (sigaction(d, NULL, &cur) != 0 || !(cur.sa_flags & SA_SIGINFO) ||
            cur.sa_sigaction != host_handler)
            continue;
        sigaddset(&take, d);
        any = true;
    }
    if (!any)
        return;
    // Unblocked, each is delivered right here, before the call returns.
    pthread_sigmask(SIG_UNBLOCK, &take, NULL);
    pthread_sigmask(SIG_BLOCK, &take, NULL);
}

static void host_handler(int dsig, siginfo_t *dinfo, void *uap)
{
    // The JIT execute-mode stub's brk (jit.c) is the runtime's own.
    if (dsig == SIGTRAP && lxrt_jit_stub_trap(uap))
        return;
    // Not a guest thread (the host main thread, see above): hand an
    // asynchronous signal on. Guest handlers never run on these threads.
    if (!is_sync_dsig(dsig) && !lxrt_thread_is_guest()) {
        forward_stray(dsig);
        return;
    }
    sigstats_note(dsig);
    if (uap) {
        unsigned k = g_lastsig_n++ % 4;
        g_lastsig[k].sig = dsig;
        g_lastsig[k].pc = ((ucontext_t *)uap)->uc_mcontext->__ss.__pc;
        g_lastsig[k].x0 = ((ucontext_t *)uap)->uc_mcontext->__ss.__x[0];
        g_lastsig[k].ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    }
    int lsig = lxrt_signo_to_linux(dsig);
    if (lxrt_trace_on()) {
        uint32_t w0 = 0, w1 = 0;
        uint64_t hpc = uap ? ((ucontext_t *)uap)->uc_mcontext->__ss.__pc : 0;
        if (hpc) {
            mach_vm_size_t got = 0;
            uint32_t buf[2] = {0, 0};
            if (mach_vm_read_overwrite(mach_task_self(), hpc, 8, (mach_vm_address_t)(uintptr_t)buf, &got) == KERN_SUCCESS)
                { w0 = buf[0]; w1 = buf[1]; }
        }
        fprintf(lxrt_trace_stream(), "[lxrt] pid %d host_handler: darwin %d -> linux %d (pc 0x%llx: %08x %08x, esr 0x%x)\n",
                (int)getpid(), dsig, lsig, (unsigned long long)hpc, w0, w1,
                uap ? ((ucontext_t *)uap)->uc_mcontext->__es.__esr : 0);
        // Under a guest base, a fault BELOW 4 GiB is a guest address that
        // missed the base somewhere: dump the code that computed it.
        uint64_t fa = dinfo ? (uint64_t)(uintptr_t)dinfo->si_addr : 0;
        static int dumped;
        if (hpc && fa && fa < (1ull << 32) && lxrt_gbase() && dumped < 3) {
            dumped++;
            uint32_t code[20];
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), hpc - 64, sizeof code,
                                       (mach_vm_address_t)(uintptr_t)code, &got) == KERN_SUCCESS) {
                fprintf(lxrt_trace_stream(), "[lxrt] unbased guest access at 0x%llx, code from pc-64:", (unsigned long long)fa);
                for (int i = 0; i < 20; i++)
                    fprintf(lxrt_trace_stream(), "%s%08x", i == 16 ? " |" : " ", code[i]);
                fprintf(lxrt_trace_stream(), "\n");
            }
        }
    }
    if (lsig == 0 && dsig != LXRT_RT_CARRIER)
        return;

    // A JIT write/execute transition is not a fault the guest should ever see:
    // it is the price Apple Silicon charges for a code buffer. Flip the mode
    // and retry the instruction.
    // The carrier signal is not itself a signal the guest asked for: it is a
    // realtime delivery, and the real number comes out of this thread's queue.
    if (dsig == LXRT_RT_CARRIER) {
        xsig_drain();                                      // from other processes
        lsig = lxrt_rt_dequeue_self_mask(g_rt_blocked);   // blocked ones wait
        if (lsig == 0)
            return;
    }

    // The runtime's own faults first (shared with main.c's fault_report).
    if (lxrt_absorb_runtime_fault(dsig, dinfo, uap))
        return;
    // A SIGSEGV the guest's handler returns from without fixing comes back
    // at once, at the same pc and address: a thread that took 10000 of them
    // in a row, with no other fault between, makes no progress and never
    // will. It dies of the fault, as the process would without a handler,
    // instead of spinning a core forever (MEASURED: an i386 Android daemon
    // that aborted under FEX, and the OMX store after its seccomp filter,
    // each at 100% CPU for as long as it was left, FEX faulting at 0x0 in
    // its own code). SIGBUS repeats legitimately (FEX's split-lock atomics).
    if (dsig == SIGSEGV && dinfo && uap) {
        static _Thread_local uint64_t spin_pc, spin_addr;
        static _Thread_local unsigned spin_n;
        uint64_t pc = ((ucontext_t *)uap)->uc_mcontext->__ss.__pc;
        uint64_t addr = (uint64_t)(uintptr_t)dinfo->si_addr;
        if (pc == spin_pc && addr == spin_addr) {
            if (++spin_n >= 10000) {
                char b[160];
                int n = snprintf(b, sizeof b, "[lxrt] pid %d: SIGSEGV at pc 0x%llx addr 0x%llx 10000 times "
                                 "in a row, unhandled: the process dies of it\n", (int)getpid(),
                                 (unsigned long long)pc, (unsigned long long)addr);
                write(2, b, (size_t)n);
                struct sigaction dfl;
                memset(&dfl, 0, sizeof dfl);
                dfl.sa_handler = SIG_DFL;
                sigaction(SIGSEGV, &dfl, NULL);
                return;     // the instruction faults again, now with the default action
            }
        } else {
            spin_pc = pc;
            spin_addr = addr;
            spin_n = 0;
        }
    }
    // LXRT_FAULT_LOG=1: a synchronous fault that goes on to the guest (its
    // handler or the default action), with pc, word, address and registers,
    // without a syscall trace (whose timing hid a stage 25 crash).
    {
        static int flog = -1;
        if (flog < 0) flog = getenv("LXRT_FAULT_LOG") ? 1 : 0;
        if (flog && uap && (dsig == SIGSEGV || dsig == SIGBUS || dsig == SIGILL || dsig == SIGTRAP)) {
            _STRUCT_ARM_THREAD_STATE64 *ts = &((ucontext_t *)uap)->uc_mcontext->__ss;
            uint32_t w = 0;
            mach_vm_size_t got = 0;
            mach_vm_read_overwrite(mach_task_self(), ts->__pc, 4, (mach_vm_address_t)(uintptr_t)&w, &got);
            char b[1400];
            int n = snprintf(b, sizeof b, "[lxrt] pid %d fault to guest: darwin %d pc 0x%llx insn 0x%08x addr %p sp 0x%llx lr 0x%llx\n[lxrt]  ",
                             (int)getpid(), dsig, (unsigned long long)ts->__pc, w,
                             dinfo ? dinfo->si_addr : NULL, (unsigned long long)ts->__sp,
                             (unsigned long long)ts->__lr);
            for (int i = 0; i < 29 && n < (int)sizeof b - 40; i++)
                n += snprintf(b + n, sizeof b - n, " x%d=%llx", i, (unsigned long long)ts->__x[i]);
            n += snprintf(b + n, sizeof b - n, " fp=%llx\n", (unsigned long long)ts->__fp);
            write(2, b, (size_t)n);
        }
    }

    int forced_code = -1;
    // An alignment fault (data abort, DFSC 0b100001) is a real SIGBUS/
    // BUS_ADRALN on Linux too, and FEX depends on getting it that way: its
    // SIGBUS handler backpatches unaligned atomics (lock cmpxchg8b -> CASP,
    // which needs 8-byte alignment). Classifying it by the region's
    // protections turned it into SIGSEGV and FEX's SMC path retried forever
    // (measured: the i386 Steam client, caspal on ...edc).
    bool alignment = false;
    if (dsig == SIGBUS && uap) {
        ucontext_t *u = (ucontext_t *)uap;
        uint32_t esr = u->uc_mcontext->__es.__esr;
        uint32_t ec = esr >> 26;
        if ((ec == 0x24 || ec == 0x25) && (esr & 0x3f) == 0x21) {
            alignment = true;
            forced_code = 1;        // BUS_ADRALN, same number on both
        }
    }
    if (dsig == SIGBUS && dinfo && !alignment) {
        int plsig = 0, pcode = 0;
        if (protection_fault((uint64_t)(uintptr_t)dinfo->si_addr, &plsig, &pcode)) {
            lsig = plsig;
            forced_code = pcode;
            if (lxrt_trace_on())
                fprintf(lxrt_trace_stream(), "[lxrt] protection fault at %p reported as "
                                "SIGSEGV/SEGV_ACCERR, not SIGBUS\n",
                        dinfo->si_addr);
        }
    }

    pthread_mutex_lock(&g_actions_lock);
    struct guest_sigaction act = g_actions[lsig];
    if (act.flags & LINUX_SA_RESETHAND)
        g_actions[lsig].handler = 0;
    pthread_mutex_unlock(&g_actions_lock);

    if (act.handler == 0 || act.handler == 1) {
        // Linux force_sig_fault: a SIGSEGV, SIGBUS or SIGTRAP raised by the
        // faulting instruction itself cannot be ignored -- the ignored
        // disposition is reset to SIG_DFL and the fault kills. Sent with
        // kill() instead, the same signal is ignored as asked. (Before this
        // host_handler was installed for SIG_IGN, the host ignored it too
        // and the thread re-ran the instruction forever.)
        bool forced = act.handler == 1 && (dsig == SIGSEGV || dsig == SIGBUS || dsig == SIGTRAP) &&
                      raised_by_instruction(dsig, dinfo, uap);
        if (forced) {
            pthread_mutex_lock(&g_actions_lock);
            if (g_actions[lsig].handler == 1)
                g_actions[lsig].handler = 0;
            pthread_mutex_unlock(&g_actions_lock);
            if (lxrt_trace_on())
                fprintf(lxrt_trace_stream(), "[lxrt] ignored guest fault forced: darwin %d addr %p\n",
                        dsig, dinfo ? dinfo->si_addr : NULL);
            if (dsig == SIGTRAP) {
                signal(SIGTRAP, SIG_DFL);   // the brk re-executes under SIG_DFL
                return;
            }
            int ds = lxrt_signo_to_darwin(lsig);   // SIGBUS reported as SIGSEGV: die of that
            if (!ds)
                ds = dsig;
            signal(ds, SIG_DFL);
            raise(ds);
            return;
        }
        // A fault the runtime did not absorb, on a signal the guest left at
        // its default: die of it, as Linux would. (SIGSEGV and SIGBUS keep
        // this handler even then -- see lxrt_rt_sigaction.)
        if (act.handler == 0 &&
            (dsig == SIGSEGV || dsig == SIGBUS ||
             (getenv("LXRT_DEBUG_DEFAULT_FAULTS") && (dsig == SIGILL || dsig == SIGFPE)))) {
            if (lxrt_trace_on() || getenv("LXRT_DEBUG_DEFAULT_FAULTS"))
                fprintf(lxrt_trace_stream(), "[lxrt] default guest fault: darwin %d addr %p\n",
                        dsig, dinfo ? dinfo->si_addr : NULL);
            signal(dsig, SIG_DFL);
            raise(dsig);
            return;
        }
        if (dsig == SIGTRAP && act.handler == 0) {
            // Kept installed for the stub (below); a real SIGTRAP with the
            // default action re-executes under SIG_DFL and terminates.
            signal(SIGTRAP, SIG_DFL);
            return;
        }
        if (lxrt_trace_on())
            fprintf(lxrt_trace_stream(), "[lxrt] host_handler: signal %d has no guest handler "
                            "(disposition %llu)\n", lsig,
                    (unsigned long long)act.handler);
        // SIG_DFL / SIG_IGN: Darwin's own disposition already applied -- or,
        // for SIGSEGV/SIGBUS/SIGTRAP under SIG_IGN (kept on this handler),
        // a sent signal ignored here.
        return;
    }

    // For the dispatcher's SA_RESTART decision (dispatch.c): one handler
    // without it makes the interrupted call report EINTR, as on Linux.
    if (act.flags & LINUX_SA_RESTART) {
        if (lxrt_sig_during_syscall == 0)
            lxrt_sig_during_syscall = 1;
    } else {
        lxrt_sig_during_syscall = 2;
    }

    ucontext_t *duc = (ucontext_t *)uap;
    _STRUCT_ARM_THREAD_STATE64 *ss = &duc->uc_mcontext->__ss;
    // A br x18 trampoline between its A and D keeps its mark just below sp,
    // where the guest frame is about to go: resume it from its first word.
    {
        uint64_t to = x18_br_restart_pc(ss->__pc);
        if (to) {
            ss->__pc = to;
            atomic_fetch_add(&g_x18br_signal, 1);
        }
    }

    // Snapshot the interrupted state BEFORE touching the stack. Darwin builds
    // its own signal frame -- this ucontext, the mcontext it points at and the
    // siginfo -- *below* the interrupted sp, so a Linux frame placed naively at
    // sp - sizeof(frame) lands on top of it. The first version of this code did
    // exactly that, zeroed Darwin's mcontext with its own memset, and then read
    // the resume pc back as 0.
    uint64_t saved_regs[31];
    for (int i = 0; i < 29; i++)
        saved_regs[i] = ss->__x[i];
    saved_regs[29] = ss->__fp;
    saved_regs[30] = ss->__lr;
    uint64_t saved_sp = ss->__sp;
    uint64_t saved_pc = ss->__pc;
    uint64_t saved_cpsr = ss->__cpsr;
    // The vector registers and their status words belong in the frame too.
    // FEX keeps the guest's XMM state in V16-V31 while translated code runs
    // (static register allocation) and its signal handler captures it from
    // the frame -- and on its sigreturn path (a `hlt` at a fixed address, see
    // benchmarks/stage5-fex.txt) writes the guest's state back INTO the frame
    // for the runtime to restore. A frame without an fpsimd record made every
    // guest signal silently corrupt vector state; bash died after its 4th
    // SIGCHLD with a bogus pointer.
    _STRUCT_ARM_NEON_STATE64 *ns = &duc->uc_mcontext->__ns;
    __uint128_t saved_v[32];
    memcpy(saved_v, ns->__v, sizeof saved_v);
    uint32_t saved_fpsr = ns->__fpsr, saved_fpcr = ns->__fpcr;
    uint64_t fault_addr = dinfo ? (uint64_t)(uintptr_t)dinfo->si_addr : 0;
    int si_code = forced_code >= 0 ? forced_code
                : dinfo ? si_code_to_linux(dsig, dinfo->si_code) : 0;

    // Where the frame goes. Linux uses the interrupted stack unless the handler
    // asked for the alternate one and we are not already on it.
    // "Already on the alternate stack" is a property of sp, not a flag: a
    // flag was cleared by the FIRST sigreturn of a nested pair (FEX's `hlt`
    // sigreturn inside a SIGCHLD handler), so the next signal put its frame at
    // the TOP of the alt stack, over the outer handler's live frames, and the
    // outer handler later returned into string data (pc 0x203a6e65706f2073,
    // "s open: "). Linux derives SS_ONSTACK from sp; so does this now.
    bool on_alt_now = g_altstack.sp &&
                      saved_sp >= g_altstack.sp && saved_sp < g_altstack.sp + g_altstack.size;
    bool use_alt = (act.flags & LINUX_SA_ONSTACK) && g_altstack.sp && !on_alt_now;
    uint64_t base;
    if (use_alt) {
        base = g_altstack.sp + g_altstack.size;
    } else {
        // Stay clear of everything Darwin put on this stack for us.
        // The host's own frames (Darwin's ucontext and this function) live on
        // the host alt stack, so the interrupted stack is free below sp bar
        // the 128-byte red zone Darwin's ABI grants leaf functions.
        base = saved_sp - 512;
        // ...unless Darwin put its frame on THIS stack after all: it does so
        // when its own on-alt-stack flag is set (a nested delivery), and that
        // flag is per thread and not always in step with sp. So stay clear of
        // everything Darwin handed us and of our own frames, every time.
        {
            uint64_t theirs[] = { (uint64_t)(uintptr_t)uap,
                                  (uint64_t)(uintptr_t)dinfo,
                                  (uint64_t)(uintptr_t)duc->uc_mcontext };
            for (size_t i = 0; i < sizeof(theirs) / sizeof(theirs[0]); i++)
                if (theirs[i] && theirs[i] < base && theirs[i] > saved_sp - (1u << 20))
                    base = theirs[i];
            uint64_t cur_sp;
            __asm__ volatile("mov %0, sp" : "=r"(cur_sp));
            if (cur_sp < base && cur_sp > saved_sp - (1u << 20))
                base = cur_sp;
            base -= 256;
        }
    }

    uint64_t frame_addr = (base - sizeof(struct rt_sigframe)) & ~15ull;
    struct rt_sigframe *f = (struct rt_sigframe *)frame_addr;
    memset(f, 0, sizeof(*f));

    f->info.si_signo = lsig;
    f->info.si_code = si_code;
    if (lxrt_ids_on() && lsig != 4 && lsig != 5 && lsig != 7 &&
        lsig != 8 && lsig != 11) {
        int sender = 0;
        int mytid = lxrt_gettid();
        if (mytid >= 2 && mytid < 65536)
            sender = atomic_exchange(&g_xsig_sender[mytid], 0);
        if (!sender && dinfo && dinfo->si_pid > 0)
            sender = lxrt_ids_reaped_child(dinfo->si_pid, false);
        f->info.si_pid = sender ? sender : 1;
    }
    // Only the fault signals carry an address; for the rest this union member
    // holds si_pid/si_uid and must stay zero rather than borrow the fault one.
    if (lsig == 4 /*ILL*/ || lsig == 5 /*TRAP*/ || lsig == 7 /*BUS*/ ||
        lsig == 8 /*FPE*/ || lsig == 11 /*SEGV*/)
        f->info.si_addr = fault_addr;
    // A siginfo the sender queued with it (rt_tgsigqueueinfo): that one.
    {
        struct linux_siginfo q;
        if (qinfo_take(lxrt_gettid(), lsig, &q)) {
            f->info = q;
            f->info.si_signo = lsig;
        }
    }

    f->uc.uc_stack.ss_sp = g_altstack.sp;
    f->uc.uc_stack.ss_size = g_altstack.size;
    f->uc.uc_stack.ss_flags = (on_alt_now || use_alt) ? 1 /* SS_ONSTACK */ : 0;

    struct linux_sigcontext *mc = &f->uc.uc_mcontext;
    memcpy(mc->regs, saved_regs, sizeof(saved_regs));
    // The guest's x18 is the virtual one (runtime/x18.c), not the register the
    // kernel just zeroed; the handler may change it and sigreturn restores it.
    // Without the x18 pass (LXRT_NO_X18) the hardware register is the only
    // x18 there is, and the frame carries it as it was.
    uint64_t hw_x18 = saved_regs[18];
    uint64_t virt_x18 = lxrt_x18_enabled() ? lxrt_x18_get() : hw_x18;
    mc->regs[18] = virt_x18;
    mc->sp = saved_sp;
    mc->pc = saved_pc;
    mc->pstate = saved_cpsr;
    mc->fault_address = fault_addr;
    // First extension record: fpsimd_context (magic 0x46508001, 528 bytes).
    // The zeroed bytes after it are the end-of-records marker.
    struct linux_fpsimd_context *fp = (struct linux_fpsimd_context *)mc->reserved;
    fp->magic = LINUX_FPSIMD_MAGIC;
    fp->size = sizeof *fp;
    fp->fpsr = saved_fpsr;
    fp->fpcr = saved_fpcr;
    memcpy(fp->vregs, saved_v, sizeof fp->vregs);

    // The guest handler runs translated code: it needs execute mode. Remember
    // whether the interrupted code had the write window open so it can be
    // reopened before the interrupted store resumes.
    bool was_writable = lxrt_jit_thread_writable();
    if (was_writable)
        lxrt_jit_set_write(1, 0, 0);

    // RUN THE GUEST HANDLER NOW, nested inside this host handler, on the frame
    // just built, and return through the kernel's own sigreturn afterwards.
    //
    // The first design resumed the guest handler by editing this ucontext's
    // pc and let the handler's `ret` land in a runtime routine that restored
    // the interrupted registers by hand. That routine could not restore x16
    // (the branch needs a register), and x16 is the syscall-number register
    // Darwin reloads when it RESTARTS a syscall a signal interrupted: pc is
    // rewound to the `svc`, and the resumed `svc` ran with x16 = the resume
    // address -- SIGSYS. Measured with tests/elf/jit_fork.c: the parent died
    // on its first SIGCHLD inside wait4 (rc 140), before this.
    //
    // Calling the handler synchronously and then returning normally hands
    // every register, the syscall restart and the FP state back to the
    // kernel, which is the only thing that knows how to resume correctly.
    // Whatever the guest handler wrote into the frame's mcontext (FEX swaps
    // whole contexts this way) is copied back into the Darwin context first.
    {
        // The guest's sa_mask, plus the signal itself unless SA_NODEFER, is
        // blocked while its handler runs -- Linux semantics, on the host mask.
        sigset_t during, before;
        sigemptyset(&during);
        for (int i = 1; i <= 64; i++)
            if ((act.mask >> (i - 1)) & 1 && !((SYNC_BITS >> (i - 1)) & 1)) {
                int d = lxrt_signo_to_darwin(i);
                if (d > 0) sigaddset(&during, d);
            }
        // Synchronous faults stay deliverable (see SYNC_BITS).
        if (!(act.flags & LINUX_SA_NODEFER) && !(lsig <= 64 && (SYNC_BITS >> (lsig - 1)) & 1))
            sigaddset(&during, dsig);
        // Linux: the frame's uc_sigmask is the mask at the interruption, and
        // sigreturn installs whatever the handler left there (FEX rewrites
        // it). The realtime part is ours to track.
        uint64_t interrupted = darwin_mask_to_linux(&duc->uc_sigmask) | g_rt_blocked | g_sync_blocked;
        memset(f->uc.uc_sigmask, 0, sizeof f->uc.uc_sigmask);
        memcpy(f->uc.uc_sigmask, &interrupted, sizeof interrupted);
        g_rt_blocked |= act.mask & RT_BITS;
        if (is_rt(lsig) && !(act.flags & LINUX_SA_NODEFER))
            g_rt_blocked |= 1ull << (lsig - 1);
        pthread_sigmask(SIG_BLOCK, &during, &before);
        if (lxrt_trace_on() && (lsig == 4 || lsig == 7 || lsig == 11)) {
            // A fault: the registers and the top of the interrupted stack are
            // the evidence. Symbolise offline against the guest images.
            fprintf(lxrt_trace_stream(), "[lxrt]    regs:");
            for (int i = 0; i < 31; i++)
                fprintf(lxrt_trace_stream(), "%s x%d=0x%llx", (i % 6) ? "" : "\n[lxrt]     ", i,
                        (unsigned long long)saved_regs[i]);
            {
                // Debugging aid for guests run by FEX: x28 holds FEX's
                // CpuStateFrame in translated code; FEXCore CPUState keeps the
                // guest RIP of the current block at +0x18 and gregs[16]
                // in x86 encoding order (RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI)
                // from +0x20 (FEXCore X86Enums.h).
                uint64_t st[4 + 16] = {0}; mach_vm_size_t got = 0;
                if (saved_regs[28] > 0x10000 &&
                    mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)saved_regs[28], sizeof st,
                                           (mach_vm_address_t)(uintptr_t)st, &got) == KERN_SUCCESS) {
                    fprintf(lxrt_trace_stream(), "\n[lxrt]    fex guest: rip=0x%llx", (unsigned long long)st[3]);
                    static const char *const nm[8] = {"rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi"};
                    for (int k = 0; k < 8; k++)
                        fprintf(lxrt_trace_stream(), " %s=0x%llx", nm[k], (unsigned long long)st[4 + k]);
                    // A 64-bit guest's stack is directly addressable: the
                    // words on it that look like code addresses are a rough
                    // guest backtrace (symbolise against the guest's images).
                    if (!lxrt_gbase() && st[4 + 4] > 0x10000) {
                        fprintf(lxrt_trace_stream(), "\n[lxrt]    guest stack words:");
                        for (int k = 0; k < 256; k++) {
                            uint64_t w = 0; mach_vm_size_t g2 = 0;
                            if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(st[4 + 4] + 8 * k), 8,
                                                       (mach_vm_address_t)&w, &g2) != KERN_SUCCESS)
                                break;
                            if (w >= 0x7e0000000000ull && w < 0x7f0000000000ull)
                                fprintf(lxrt_trace_stream(), " [%d]0x%llx", k, (unsigned long long)w);
                        }
                    }
                }
            }
            fprintf(lxrt_trace_stream(), "\n[lxrt]    stack @0x%llx:", (unsigned long long)saved_sp);
            for (int i = 0; i < 48; i++) {
                uint64_t w = 0; mach_vm_size_t got = 0;
                if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(saved_sp + 8 * i),
                                           8, (mach_vm_address_t)&w, &got) != KERN_SUCCESS)
                    break;
                fprintf(lxrt_trace_stream(), "%s +%03x 0x%llx", (i % 4) ? "" : "\n[lxrt]     ", i * 8,
                        (unsigned long long)w);
            }
            fprintf(lxrt_trace_stream(), "\n");
            // Anything that resolves to a host image, by name.
            for (int i = 0; i < 31 + 48; i++) {
                uint64_t v = 0;
                if (i < 31) v = saved_regs[i];
                else {
                    mach_vm_size_t got = 0;
                    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(saved_sp + 8 * (i - 31)),
                                               8, (mach_vm_address_t)&v, &got) != KERN_SUCCESS)
                        break;
                }
                Dl_info di;
                if (v > 0x1000 && dladdr((void *)(uintptr_t)v, &di) && di.dli_sname)
                    fprintf(lxrt_trace_stream(), "[lxrt]      %s%d = 0x%llx: %s+0x%llx (%s)\n",
                            i < 31 ? "x" : "stack+", i < 31 ? i : (i - 31) * 8,
                            (unsigned long long)v, di.dli_sname,
                            (unsigned long long)(v - (uint64_t)(uintptr_t)di.dli_saddr),
                            strrchr(di.dli_fname, '/') ? strrchr(di.dli_fname, '/') + 1 : di.dli_fname);
            }
        }
        if (lxrt_trace_on()) {
            uint64_t host_sp;
            __asm__ volatile("mov %0, sp" : "=r"(host_sp));
            stack_t kst = {0};
            sigaltstack(NULL, &kst);
            fprintf(lxrt_trace_stream(), "[lxrt] pid %d signal %d -> guest handler 0x%llx, frame 0x%llx, "
                            "interrupted pc 0x%llx sp 0x%llx%s [host sp 0x%llx, host altstack 0x%llx%s, kernel flags 0x%x, uc_onstack %d]\n", (int)getpid(), lsig,
                    (unsigned long long)act.handler, (unsigned long long)frame_addr,
                    (unsigned long long)saved_pc, (unsigned long long)saved_sp,
                    use_alt ? " (alt stack)" : "",
                    (unsigned long long)host_sp, (unsigned long long)(uintptr_t)g_host_altstack,
                    (g_host_altstack && host_sp >= (uint64_t)(uintptr_t)g_host_altstack &&
                     host_sp < (uint64_t)(uintptr_t)g_host_altstack + HOST_ALTSTACK_SIZE) ? " ON" : " OFF",
                    (unsigned)kst.ss_flags, duc->uc_onstack);
        }
        lxrt_call_guest_handler(act.handler, (uint64_t)lsig,
                                (uint64_t)(uintptr_t)&f->info,
                                (uint64_t)(uintptr_t)&f->uc, frame_addr);
        pthread_sigmask(SIG_SETMASK, &before, NULL);
        static int ctxlog = -1;
        if (ctxlog < 0) ctxlog = getenv("LXRT_GUEST_FAULTS") ? 1 : 0;
        if ((ctxlog || lxrt_trace_on()) && lsig == 11 && lxrt_gbase() &&
            (mc->pc != saved_pc || mc->sp != saved_sp)) {
            // FEX has just redirected the thread into the guest's handler: its
            // CPUState (x28 in translated code) holds the guest ESP of the
            // i386 rt_sigframe it built -- [ret][sig][&info][&uc], and in
            // uc_mcontext.gregs EIP is index 14. That is the faulting guest
            // instruction, the one thing the host context cannot say.
            uint64_t st[4 + 8] = {0}; mach_vm_size_t got = 0;
            uint32_t fr[4] = {0};
            if (mc->regs[28] > 0x10000 &&
                mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)mc->regs[28], sizeof st,
                                       (mach_vm_address_t)(uintptr_t)st, &got) == KERN_SUCCESS) {
                uint64_t gbase = lxrt_gbase();
                if (mach_vm_read_overwrite(mach_task_self(), gbase + (uint32_t)st[4 + 4], sizeof fr,
                                           (mach_vm_address_t)(uintptr_t)fr, &got) == KERN_SUCCESS) {
                    uint32_t g[19] = {0};
                    // i386 ucontext: uc_flags, uc_link, uc_stack (12), then mcontext gregs.
                    if (mach_vm_read_overwrite(mach_task_self(), gbase + fr[3] + 20, sizeof g,
                                               (mach_vm_address_t)(uintptr_t)g, &got) == KERN_SUCCESS) {
                        fprintf(lxrt_trace_stream(), "[lxrt]    guest fault context: eip=0x%x esp=0x%x ebp=0x%x "
                                "eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x esi=0x%x edi=0x%x trap=%u err=0x%x\n",
                                g[14], g[7], g[6], g[11], g[8], g[10], g[9], g[5], g[4], g[12], g[13]);
                        {
                            // si_addr (i386 siginfo: signo, errno, code, then the address)
                            // and the bytes at EIP.
                            uint32_t si[4] = {0}; uint8_t code[16] = {0};
                            mach_vm_read_overwrite(mach_task_self(), gbase + fr[2], sizeof si,
                                                   (mach_vm_address_t)(uintptr_t)si, &got);
                            mach_vm_read_overwrite(mach_task_self(), gbase + g[14], sizeof code,
                                                   (mach_vm_address_t)(uintptr_t)code, &got);
                            fprintf(lxrt_trace_stream(), "[lxrt]    guest si_addr 0x%x si_code %d; bytes at eip:", si[3], (int)si[2]);
                            for (int i = 0; i < 16; i++) fprintf(lxrt_trace_stream(), " %02x", code[i]);
                            fprintf(lxrt_trace_stream(), "\n");
                            // What the host page under si_addr is now, and who changed it.
                            mach_vm_address_t ra = gbase + si[3]; mach_vm_size_t rs = 0;
                            vm_region_basic_info_data_64_t rinfo; mach_msg_type_number_t rcnt = VM_REGION_BASIC_INFO_COUNT_64;
                            mach_port_t robj = MACH_PORT_NULL;
                            if (si[3] && mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                                                        (vm_region_info_t)&rinfo, &rcnt, &robj) == KERN_SUCCESS)
                                fprintf(lxrt_trace_stream(), "[lxrt]    host region at si_addr: 0x%llx+0x%llx prot %d max %d\n",
                                        (unsigned long long)ra, (unsigned long long)rs, rinfo.protection, rinfo.max_protection);
                            if (si[3])
                                lxrt_memlog_dump(gbase + si[3], "guest fault page history");
                        }
                        char fp[1024]; uint64_t fo = 0;
                        if (g[14] && lxrt_memlog_file_lookup(gbase + g[14], fp, sizeof fp, &fo))
                            fprintf(lxrt_trace_stream(), "[lxrt]    guest eip 0x%x = %s +0x%llx (file offset)\n",
                                    g[14], fp, (unsigned long long)fo);
                        // Return addresses on the guest stack, for a rough backtrace.
                        uint32_t stk[64] = {0};
                        if (mach_vm_read_overwrite(mach_task_self(), gbase + g[7], sizeof stk,
                                                   (mach_vm_address_t)(uintptr_t)stk, &got) == KERN_SUCCESS)
                            for (int i = 0; i < 64; i++)
                                if (stk[i] > 0x10000 && lxrt_memlog_file_lookup(gbase + stk[i], fp, sizeof fp, &fo) &&
                                    strstr(fp, ".so"))
                                    fprintf(lxrt_trace_stream(), "[lxrt]      stack[%d] 0x%x = %s +0x%llx\n",
                                            i, stk[i], strrchr(fp, '/') ? strrchr(fp, '/') + 1 : fp,
                                            (unsigned long long)fo);
                    }
                }
            }
            {
                uint64_t ib = lxrt_main_image_base, ie = ib + lxrt_main_image_span;
                uint64_t olr = saved_regs[30];
                Dl_info di;
                const char *sym = dladdr((void *)(uintptr_t)saved_pc, &di) && di.dli_sname ? di.dli_sname : "?";
                fprintf(lxrt_trace_stream(), "[lxrt]    host fault: pc 0x%llx%s+0x%llx (%s) lr 0x%llx%s+0x%llx sp 0x%llx\n",
                        (unsigned long long)saved_pc, saved_pc >= ib && saved_pc < ie ? " = image" : "",
                        (unsigned long long)(saved_pc >= ib && saved_pc < ie ? saved_pc - ib : 0), sym,
                        (unsigned long long)olr, olr >= ib && olr < ie ? " = image" : "",
                        (unsigned long long)(olr >= ib && olr < ie ? olr - ib : 0),
                        (unsigned long long)saved_sp);
            }
            {
                uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
                for (unsigned i = 0; i < 4 && i < g_lastsig_n; i++) {
                    unsigned k = (g_lastsig_n - 1 - i) % 4;
                    fprintf(lxrt_trace_stream(), "[lxrt]    recent host signal %u: sig %d at pc 0x%llx x0 0x%llx, %.3f ms ago\n",
                            i, g_lastsig[k].sig, (unsigned long long)g_lastsig[k].pc,
                            (unsigned long long)g_lastsig[k].x0, (double)(now - g_lastsig[k].ns) / 1e6);
                }
                fprintf(lxrt_trace_stream(), "[lxrt]    host regs at fault: x0 %llx x1 %llx x2 %llx x3 %llx x4 %llx x5 %llx x6 %llx x7 %llx\n",
                        (unsigned long long)saved_regs[0], (unsigned long long)saved_regs[1], (unsigned long long)saved_regs[2],
                        (unsigned long long)saved_regs[3], (unsigned long long)saved_regs[4], (unsigned long long)saved_regs[5],
                        (unsigned long long)saved_regs[6], (unsigned long long)saved_regs[7]);
            }
            fprintf(lxrt_trace_stream(), "[lxrt]    guest fault raw: x28 0x%llx state rip 0x%llx rsp 0x%llx | "
                    "frame %x %x %x %x | new pc 0x%llx sp 0x%llx x8 0x%llx | old x8 0x%llx fault 0x%llx\n",
                    (unsigned long long)mc->regs[28], (unsigned long long)st[3], (unsigned long long)st[8],
                    fr[0], fr[1], fr[2], fr[3], (unsigned long long)mc->pc, (unsigned long long)mc->sp,
                    (unsigned long long)mc->regs[8], (unsigned long long)saved_regs[8],
                    (unsigned long long)(uintptr_t)dinfo->si_addr);
            {
                char fp[1024]; uint64_t fo = 0;
                uint64_t rip_host = lxrt_gbase() + (uint32_t)st[3];
                if (st[3] && lxrt_memlog_file_lookup(rip_host, fp, sizeof fp, &fo))
                    fprintf(lxrt_trace_stream(), "[lxrt]    guest block rip 0x%llx = %s +0x%llx (file offset)\n",
                            (unsigned long long)st[3], fp, (unsigned long long)fo);
            }
        }
        uint64_t after;
        memcpy(&after, f->uc.uc_sigmask, sizeof after);
        sigset_t dm;
        linux_mask_to_darwin(after, &dm);
        duc->uc_sigmask = dm;           // Darwin's sigreturn installs this
        g_rt_blocked = after & RT_BITS;
        g_sync_blocked = after & SYNC_BITS;
        lxrt_thread_note_mask(after);
        rt_kick_if_pending();
    }
    if (lxrt_trace_on()) {
        uint32_t w[2] = {0, 0}; mach_vm_size_t got = 0;
        mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)mc->pc, sizeof w, (mach_vm_address_t)w, &got);
        fprintf(lxrt_trace_stream(), "[lxrt] pid %d sigreturn: frame 0x%llx -> pc 0x%llx (%08x %08x) sp 0x%llx x16 0x%llx x30 0x%llx%s\n",
                (int)getpid(), (unsigned long long)frame_addr, (unsigned long long)mc->pc, w[0], w[1],
                (unsigned long long)mc->sp, (unsigned long long)mc->regs[16],
                (unsigned long long)mc->regs[30],
                (mc->pc == saved_pc && mc->sp == saved_sp) ? " (unchanged)" : " (CHANGED by handler)");
    }
    // Put the W^X window back the way the interrupted code had it.
    if (was_writable != lxrt_jit_thread_writable())
        lxrt_jit_set_write(was_writable ? 0 : 1, 0, 0);
    // Frame -> Darwin context. The handler may have changed any of it.
    lxrt_x18_set(mc->regs[18]);
    for (int i = 0; i < 29; i++)
        ss->__x[i] = mc->regs[i];
    // An x18 the handler left alone goes back into the hardware register as
    // it was, not as the virtual one: where the kernel keeps x18 (the keep-x18
    // build), code the runtime never rewrote -- a JIT's -- may hold a live
    // value there, and HotSpot's safepoint polls and implicit null checks
    // take SIGSEGV in exactly such code (stage 28, x18_jit_signal).
    if (mc->regs[18] == virt_x18)
        ss->__x[18] = hw_x18;
    ss->__fp = mc->regs[29];
    ss->__lr = mc->regs[30];
    ss->__sp = mc->sp;
    ss->__pc = mc->pc;
    ss->__cpsr = (uint32_t)((ss->__cpsr & ~0xF0000000u) | (mc->pstate & 0xF0000000u));
    const struct linux_fpsimd_context *fpr = (const struct linux_fpsimd_context *)mc->reserved;
    if (fpr->magic == LINUX_FPSIMD_MAGIC && fpr->size == sizeof *fpr) {
        memcpy(ns->__v, fpr->vregs, sizeof ns->__v);
        ns->__fpsr = fpr->fpsr;
        ns->__fpcr = fpr->fpcr;
    }
    return;


    if (lxrt_trace_on()) {
        fprintf(lxrt_trace_stream(), "[lxrt] signal %d -> guest handler 0x%llx, frame 0x%llx, "
                        "resume 0x%llx\n", lsig,
                (unsigned long long)act.handler, (unsigned long long)frame_addr,
                (unsigned long long)mc->pc);
        // For a fault, the instruction is the evidence: dump the words at the
        // resume pc (which is the faulting pc for SIGILL/SIGSEGV/SIGBUS).
        // mach_vm_read_overwrite fails cleanly on unmapped memory.
        if (lsig == 4 || lsig == 7 || lsig == 11) {
            uint32_t w[4] = {0};
            mach_vm_size_t got = 0;
            if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)mc->pc,
                                       sizeof w, (mach_vm_address_t)w, &got) == KERN_SUCCESS)
                fprintf(lxrt_trace_stream(), "[lxrt]    at pc: %08x %08x %08x %08x  (jit=%d)\n",
                        w[0], w[1], w[2], w[3], (int)lxrt_jit_contains(mc->pc));
            else
                fprintf(lxrt_trace_stream(), "[lxrt]    at pc: unreadable\n");
        }
    }
}

// Called from lxrt_sigreturn_entry with the guest's sp, which Linux guarantees
// still points at the frame when the handler returns.
void lxrt_do_sigreturn(uint64_t sp)
{
    struct rt_sigframe *f = (struct rt_sigframe *)sp;
    struct linux_sigcontext *mc = &f->uc.uc_mcontext;

    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] sigreturn: frame 0x%llx -> pc 0x%llx sp 0x%llx\n",
                (unsigned long long)sp, (unsigned long long)mc->pc,
                (unsigned long long)mc->sp);

    // Thread-local, deliberately not on the stack: lxrt_restore_regs moves sp
    // before its last loads, and anything below the new sp is where a second
    // signal would build its frame. Layout is shared with trampoline.S:
    //   [0..30] x0..x30  [31] sp  [32] pc  [33] pstate  [34] fpsr  [35] fpcr
    //   +288: 32 x 16-byte vector registers.
    static _Thread_local _Alignas(16) uint64_t ctx[36 + 64];
    memcpy(ctx, mc->regs, sizeof(mc->regs));
    ctx[31] = mc->sp;
    ctx[32] = mc->pc;
    ctx[33] = mc->pstate;
    const struct linux_fpsimd_context *fp = (const struct linux_fpsimd_context *)mc->reserved;
    if (fp->magic == LINUX_FPSIMD_MAGIC && fp->size == sizeof *fp) {
        ctx[34] = fp->fpsr;
        ctx[35] = fp->fpcr;
        memcpy(ctx + 36, fp->vregs, sizeof fp->vregs);
    } else {
        // No record (a handler rewrote the frame without one): keep the
        // registers as the handler left them, restore only the status words
        // as zero would be wrong -- read the live ones.
        uint64_t fpsr, fpcr;
        __asm__ volatile("mrs %0, fpsr" : "=r"(fpsr));
        __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
        ctx[34] = fpsr; ctx[35] = fpcr;
        __asm__ volatile("stp q0, q1, [%0]" :: "r"(ctx + 36) : "memory");
        for (int i = 0; i < 32; i++) { /* keep whatever is live: copy via stp below */ }
        uint8_t *v = (uint8_t *)(ctx + 36);
        __asm__ volatile(
            "stp q0, q1, [%0, #0]\n stp q2, q3, [%0, #32]\n stp q4, q5, [%0, #64]\n stp q6, q7, [%0, #96]\n"
            "stp q8, q9, [%0, #128]\n stp q10, q11, [%0, #160]\n stp q12, q13, [%0, #192]\n stp q14, q15, [%0, #224]\n"
            "stp q16, q17, [%0, #256]\n stp q18, q19, [%0, #288]\n stp q20, q21, [%0, #320]\n stp q22, q23, [%0, #352]\n"
            "stp q24, q25, [%0, #384]\n stp q26, q27, [%0, #416]\n stp q28, q29, [%0, #448]\n stp q30, q31, [%0, #480]\n"
            :: "r"(v) : "memory");
    }
    lxrt_restore_regs(ctx);
}

// ---------------------------------------------------------------- syscalls

struct linux_sigaction_arg {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask;
};

long lxrt_rt_sigaction(int lsig, const void *uact, void *uoldact, size_t sigsetsize)
{
    if (lsig <= 0 || lsig > LINUX_NSIG)
        return LERR(EINVAL);
    if (sigsetsize != 8)
        return LERR(EINVAL);   // Linux checks this; a mismatch means a bad caller

    pthread_mutex_lock(&g_actions_lock);
    struct guest_sigaction old = g_actions[lsig];
    if (uact) {
        const struct linux_sigaction_arg *a = uact;
        g_actions[lsig].handler = a->handler;
        g_actions[lsig].flags = a->flags;
        g_actions[lsig].restorer = a->restorer;
        g_actions[lsig].mask = a->mask;
    }
    pthread_mutex_unlock(&g_actions_lock);

    if (uoldact) {
        struct linux_sigaction_arg *o = uoldact;
        o->handler = old.handler;
        o->flags = old.flags;
        o->restorer = old.restorer;
        o->mask = old.mask;
    }
    if (!uact)
        return 0;

    int dsig = lxrt_signo_to_darwin(lsig);
    if (is_rt(lsig)) {
        // Install the carrier once; the handler reads the real number from the
        // per-thread queue.
        static atomic_flag carrier_installed;
        if (!atomic_flag_test_and_set(&carrier_installed)) {
            struct sigaction ca;
            memset(&ca, 0, sizeof ca);
            ca.sa_sigaction = host_handler;
            ca.sa_flags = SA_SIGINFO | SA_ONSTACK;
            sigemptyset(&ca.sa_mask);
            if (sigaction(LXRT_RT_CARRIER, &ca, NULL) != 0)
                return LERR(errno);
        }
        return 0;
    }
    if (dsig == 0) {
        // SIGSTKFLT and SIGPWR have no Darwin equivalent and no carrier is
        // warranted for them: nothing in this stack sends them. The disposition
        // is remembered so sigaction round-trips.
        static atomic_flag warned[LINUX_NSIG + 1];
        if (!atomic_flag_test_and_set(&warned[lsig]))
            fprintf(lxrt_trace_stream(), "[lxrt] signal %d has no Darwin equivalent and no "
                            "carrier: recorded but never delivered\n", lsig);
        return 0;
    }
    if (dsig == SIGKILL || dsig == SIGSTOP)
        return 0;   // Linux refuses to let these be caught; so does Darwin

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    // SIG_DFL or SIG_IGN, as the guest sees it.
    bool no_handler = g_actions[lsig].handler == 0 || g_actions[lsig].handler == 1;
    if (no_handler && dsig == SIGTRAP) {
        // The runtime's JIT execute-mode stub ends in brk (jit.c): keep a
        // handler that recognises it; host_handler applies the guest's
        // disposition to any other SIGTRAP. (Ignored on the host, the stub's
        // brk would be dropped and re-executed forever.)
        sa.sa_sigaction = host_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    } else if (no_handler && (dsig == SIGSEGV || dsig == SIGBUS)) {
        // The runtime's own faults arrive as SIGSEGV/SIGBUS whatever the
        // guest's disposition: MAP_JIT and W^X flips (jit.c, wxsplit.c,
        // subpage.c), copy-on-write of private shared memory (privmap.c).
        // With the host at SIG_DFL each of them killed the process instead:
        // every webhelper renderer (forked from the zygote, SIGBUS/SIGSEGV
        // at their default) died on its first JIT page, killed by the
        // kernel with no runtime report (stage 22 E11 "No lxrun report
        // precedes it"; MEASURED again here). With the host at SIG_IGN
        // Darwin dropped each of them and the thread re-ran the faulting
        // instruction forever (MEASURED: tests/elf/wx_owner.c, "sigign").
        // host_handler applies the guest's disposition itself to a fault
        // none of them absorbs.
        sa.sa_sigaction = host_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    } else if (g_actions[lsig].handler == 0 &&
               (!getenv("LXRT_DEBUG_DEFAULT_FAULTS") ||
                !(dsig == SIGILL || dsig == SIGFPE))) {
        sa.sa_handler = SIG_DFL;
    } else if (g_actions[lsig].handler == 1) {
        sa.sa_handler = SIG_IGN;
    } else {
        sa.sa_sigaction = host_handler;
        sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
        // The frame is built on the interrupted stack by hand, so Darwin's own
        // alternate stack is not used; SA_ONSTACK is honoured in host_handler.
        if (g_actions[lsig].flags & LINUX_SA_RESTART)
            sa.sa_flags |= SA_RESTART;
        if (g_actions[lsig].flags & LINUX_SA_NODEFER)
            sa.sa_flags |= SA_NODEFER;
        // Synchronous faults are never blocked on the host (see SYNC_BITS):
        // a fault raised INSIDE the handler of the same signal must be
        // delivered, not left pending. FEX's unaligned-atomic handler (itself
        // a SIGBUS handler) takes a spinlock that lives in the MAP_JIT code
        // buffer; in execute mode that store is a SIGBUS, and with SIGBUS
        // blocked Darwin re-executed the casal forever (MEASURED: i386 TSO
        // probe, one SIGBUS delivered, then a thread spinning at one pc).
        if (dsig == SIGSEGV || dsig == SIGBUS || dsig == SIGILL || dsig == SIGFPE || dsig == SIGTRAP)
            sa.sa_flags |= SA_NODEFER;
    }
    sigemptyset(&sa.sa_mask);
    return sigaction(dsig, &sa, NULL) != 0 ? LERR(errno) : 0;
}

#define LINUX_SIG_BLOCK   0
#define LINUX_SIG_UNBLOCK 1
#define LINUX_SIG_SETMASK 2


static void linux_mask_to_darwin(uint64_t lmask, sigset_t *out)
{
    sigemptyset(out);
    lmask &= ~SYNC_BITS;
    for (int i = 1; i < 32; i++)
        if (lmask & (1ull << (i - 1))) {
            int d = lxrt_signo_to_darwin(i);
            if (d)
                sigaddset(out, d);
        }
}

static uint64_t darwin_mask_to_linux(const sigset_t *in)
{
    uint64_t m = 0;
    for (int i = 1; i < 32; i++) {
        int d = lxrt_signo_to_darwin(i);
        if (d && sigismember(in, d))
            m |= 1ull << (i - 1);
    }
    return m;
}

// rt_sigpending: what Darwin holds pending for this thread and the process,
// plus the realtime signals queued for this thread (which never reach
// Darwin's pending set: they travel as the carrier). It was ENOSYS, so
// sigpending() failed and reported nothing.
long lxrt_rt_sigpending(uint64_t *uset, size_t sigsetsize)
{
    if (sigsetsize != 8)
        return LERR(EINVAL);
    if (!uset)
        return LERR(EFAULT);
    sigset_t p;
    if (sigpending(&p) != 0)
        return LERR(errno);
    uint64_t m = darwin_mask_to_linux(&p) | lxrt_rt_queued_self();
    // Synchronous faults blocked by the guest are pending here, not on the
    // host (SYNC_BITS); nothing is ever left pending for them either way.
    *uset = m;
    return 0;
}

long lxrt_rt_sigprocmask(int how, const uint64_t *uset, uint64_t *uoldset,
                         size_t sigsetsize)
{
    if (sigsetsize != 8)
        return LERR(EINVAL);

    int dhow = how == LINUX_SIG_BLOCK   ? SIG_BLOCK
             : how == LINUX_SIG_UNBLOCK ? SIG_UNBLOCK
             : how == LINUX_SIG_SETMASK ? SIG_SETMASK : -1;
    if (uset && dhow < 0)
        return LERR(EINVAL);

    sigset_t set, old;
    if (uset)
        linux_mask_to_darwin(*uset, &set);
    uint64_t rt_old = g_rt_blocked;
    if (pthread_sigmask(uset ? dhow : SIG_BLOCK, uset ? &set : NULL, &old) != 0)
        return LERR(errno);
    uint64_t sync_old = g_sync_blocked;
    if (uset) {
        uint64_t r = *uset & RT_BITS, y = *uset & SYNC_BITS;
        g_rt_blocked = dhow == SIG_BLOCK ? (rt_old | r)
                     : dhow == SIG_UNBLOCK ? (rt_old & ~r) : r;
        g_sync_blocked = dhow == SIG_BLOCK ? (sync_old | y)
                       : dhow == SIG_UNBLOCK ? (sync_old & ~y) : y;
    }
    uint64_t old_l = darwin_mask_to_linux(&old) | rt_old | sync_old;
    if (uset) {
        // What other threads aim stray signals by (lxrt_thread_signal_target).
        uint64_t n = how == LINUX_SIG_BLOCK ? (old_l | *uset)
                   : how == LINUX_SIG_UNBLOCK ? (old_l & ~*uset) : *uset;
        lxrt_thread_note_mask(n & ~((1ull << (9 - 1)) | (1ull << (19 - 1))));   // never KILL, STOP
    }
    if (uoldset)
        *uoldset = old_l;
    if (uset)
        rt_kick_if_pending();
    return 0;
}

long lxrt_sigaltstack(const void *uss, void *uoss)
{
    // On the alt stack now? Derived from sp: the dispatcher runs on the guest
    // thread's own stack, so ours is (near) the guest's.
    uint64_t cur_sp;
    __asm__ volatile("mov %0, sp" : "=r"(cur_sp));
    bool on_alt = g_altstack.sp && cur_sp >= g_altstack.sp && cur_sp < g_altstack.sp + g_altstack.size;
    struct linux_stack { uint64_t ss_sp; int32_t ss_flags; int32_t pad; uint64_t ss_size; };
    if (uoss) {
        struct linux_stack *o = uoss;
        o->ss_sp = g_altstack.sp;
        o->ss_size = g_altstack.size;
        o->ss_flags = on_alt ? 1 : (g_altstack.sp ? 0 : 2 /* SS_DISABLE */);
        o->pad = 0;
    }
    if (uss) {
        const struct linux_stack *s = uss;
        if (on_alt)
            return LERR(EPERM);   // Linux refuses to change it while in use
        if (s->ss_flags & 2) {    // SS_DISABLE
            g_altstack.sp = g_altstack.size = 0;
        } else {
            g_altstack.sp = s->ss_sp;
            g_altstack.size = s->ss_size;
        }
        g_altstack.flags = (uint64_t)s->ss_flags;
    }
    return 0;
}

// Process-directed kill. Aimed at the first guest thread rather than at the
// process, because the process also contains AppKit's main thread and
// libdispatch's workers -- a signal landing there is lost at best, and at worst
// has its context rewritten into guest code.
// A SIGABRT sent to ANOTHER process leaves no crash report on either side
// (ReportCrash only sees the process's own abort), so the sender says so.
// tests/win/run.sh saw "Abort trap: 6" twice after a probe had passed.
static void note_foreign_abort(int pid, int tid)
{
    extern char ***_NSGetArgv(void);
    extern int *_NSGetArgc(void);
    char **av = *_NSGetArgv();
    int ac = *_NSGetArgc();
    fprintf(lxrt_trace_stream(), "[lxrt] pid %d sends SIGABRT to pid %d tid %d (sender:", getpid(), pid, tid);
    for (int i = 0; i < ac && i < 4; i++)
        fprintf(lxrt_trace_stream(), " %s", av[i]);
    fprintf(lxrt_trace_stream(), ")\n");
}

long lxrt_kill(int pid, int lsig)
{
    if (lxrt_ids_on() && pid > 0) {
        pid = lxrt_ids_target_pid(pid);
        if (pid < 0) return LERR(ESRCH);
    }
    if (lsig == 0)
        return kill(pid, 0) != 0 ? LERR(errno) : 0;
    int d = is_rt(lsig) ? LXRT_RT_CARRIER : lxrt_signo_to_darwin(lsig);
    if (!d)
        return LERR(EINVAL);
    if (pid > 0 && pid != getpid() && is_rt(lsig))
        return xsig_send(pid, 0, lsig) ? 0 : LERR(ESRCH);   // the number travels in the mailbox
    if (pid != 0 && pid != getpid()) {
        if (lsig == 6)
            note_foreign_abort(pid, 0);
        return kill(pid, d) != 0 ? LERR(errno) : 0;
    }

    pthread_t target;
    if (lxrt_main_guest_thread(&target)) {
        if (is_rt(lsig)) {
            // Process-directed realtime signals go to the first guest thread,
            // the same simplification the non-realtime path makes.
            extern int lxrt_gettid(void);
            (void)lxrt_gettid();
            return lxrt_tgkill(0, lxrt_gettid(), lsig);
        }
        int rc = pthread_kill(target, d);
        if (rc == 0)
            lxrt_signalfd_notify(lsig);   // kqueue cannot see pthread_kill
        return rc != 0 ? LERR(rc) : 0;
    }
    return kill(pid, d) != 0 ? LERR(errno) : 0;
}

// ------------------------------------------- signals to another process
//
// Darwin can signal a process but not a thread of another process, and it has
// no realtime signals. Both ends of such a delivery are this runtime, though:
// the sender writes (tid, signal) into the target process's mailbox -- a small
// shared file named after its pid -- and wakes it with the realtime carrier;
// the target's carrier handler (installed in every process) takes the entries
// out and delivers each to its own thread. Before this, every thread-directed
// signal to another process failed with ESRCH: wineserver's SIGUSR1 to a
// client thread (system APCs, suspend) found "no thread", freed the APC on
// the spot, and a named pipe's async was then used after free when its
// handle closed -- wineserver died saving the registry at the end of every
// session with a window (MEASURED, stage 16). Process-directed realtime
// signals to another process sent the bare carrier and lost the number.
#define XSIG_DIR "/tmp/lxrt-sig"
#define XSIG_SLOTS 64
struct xsig_slot { _Atomic uint32_t state; int32_t tid, sig, sender; }; // 0 free, 1 filling, 2 ready
#define XSIG_BYTES (XSIG_SLOTS * sizeof(struct xsig_slot))
static struct xsig_slot *g_xsig;

static void xsig_open_self(void)
{
    mkdir(XSIG_DIR, 0700);
    char p[64];
    snprintf(p, sizeof p, XSIG_DIR "/%d", (int)getpid());
    int fd = open(p, O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0)
        return;
    void *m = ftruncate(fd, (off_t)XSIG_BYTES) == 0
            ? mmap(NULL, XSIG_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
    close(fd);
    g_xsig = m == MAP_FAILED ? NULL : m;
}

static void xsig_unlink_self(void)
{
    char p[64];
    snprintf(p, sizeof p, XSIG_DIR "/%d", (int)getpid());
    unlink(p);
}

static void host_handler(int dsig, siginfo_t *dinfo, void *uap);
static void xsig_child(void)
{
    // The inherited mapping is the PARENT's mailbox.
    if (g_xsig)
        munmap(g_xsig, XSIG_BYTES);
    g_xsig = NULL;
    xsig_open_self();
}

// Mailboxes of processes that died without atexit (SIGKILL) stay behind;
// one process start in 16 sweeps the ones whose pid is gone.
static void xsig_sweep(void)
{
    DIR *d = opendir(XSIG_DIR);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end || pid <= 0)
            continue;
        if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) {
            char p[64];
            snprintf(p, sizeof p, XSIG_DIR "/%ld", pid);
            unlink(p);
        }
    }
    closedir(d);
}

__attribute__((constructor(202))) static void xsig_init(void)
{
    if (getpid() % 16 == 0)
        xsig_sweep();
    xsig_open_self();
    atexit(xsig_unlink_self);
    pthread_atfork(NULL, NULL, xsig_child);
    // The carrier may now arrive from another process at any time; without
    // a handler its default action would kill this one.
    struct sigaction ca;
    memset(&ca, 0, sizeof ca);
    ca.sa_sigaction = host_handler;
    ca.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&ca.sa_mask);
    sigaction(LXRT_RT_CARRIER, &ca, NULL);
}

// tid 0: process-directed.
static bool xsig_send(int pid, int tid, int lsig)
{
    char p[64];
    snprintf(p, sizeof p, XSIG_DIR "/%d", pid);
    int fd = open(p, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return false;           // not a process of this runtime
    struct xsig_slot *m = mmap(NULL, XSIG_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (m == MAP_FAILED)
        return false;
    bool queued = false;
    for (int i = 0; i < XSIG_SLOTS && !queued; i++) {
        uint32_t z = 0;
        if (atomic_compare_exchange_strong(&m[i].state, &z, 1)) {
            m[i].tid = tid;
            m[i].sig = lsig;
            m[i].sender = lxrt_ids_on() ? lxrt_ids_pid() : 0;
            atomic_store_explicit(&m[i].state, 2, memory_order_release);
            queued = true;
        }
    }
    munmap(m, XSIG_BYTES);
    return queued && kill(pid, LXRT_RT_CARRIER) == 0;
}

long lxrt_tgkill(int tgid, int tid, int lsig);
// In the carrier handler: deliver what other processes left in the mailbox.
static void xsig_drain(void)
{
    if (!g_xsig)
        return;
    for (int i = 0; i < XSIG_SLOTS; i++) {
        if (atomic_load_explicit(&g_xsig[i].state, memory_order_acquire) != 2)
            continue;
        int tid = g_xsig[i].tid, sig = g_xsig[i].sig;
        int sender = g_xsig[i].sender;
        atomic_store_explicit(&g_xsig[i].state, 0, memory_order_release);
        if (lxrt_ids_on() && sender > 0) {
            int target = tid ? tid : lxrt_ids_pid();
            if (target >= 2 && target < 65536)
                atomic_store(&g_xsig_sender[target], sender);
        }
        if (tid)
            lxrt_tgkill(0, tid, sig);
        else
            lxrt_kill(lxrt_ids_pid(), sig);
    }
}

// Thread-directed. This is what glibc's raise() and pthread_kill() use.
// rt_tgsigqueueinfo(tgid, tid, sig, info) and rt_sigqueueinfo(pid, sig,
// info): to this process, the siginfo travels with the signal (qinfo_put);
// to another, it is sent as tgkill/kill would send it.
long lxrt_sigqueueinfo(int tgid, int tid, int lsig, const void *linfo)
{
    if (!linfo)
        return LERR(EFAULT);
    bool self = tgid == (lxrt_ids_on() ? lxrt_ids_pid() : getpid()) || tgid == getpid();
    if (self && lsig > 0 && lsig < 65) {
        int target = tid > 0 ? tid : 0;
        bool queued = qinfo_put(target, lsig, linfo);
        long r = tid > 0 ? lxrt_tgkill(tgid, tid, lsig) : lxrt_kill(tgid, lsig);
        if (r != 0 && queued) {
            struct linux_siginfo drop;
            qinfo_take(target, lsig, &drop);
        }
        return r;
    }
    return tid > 0 ? lxrt_tgkill(tgid, tid, lsig) : lxrt_kill(tgid, lsig);
}

long lxrt_tgkill(int tgid, int tid, int lsig)
{
    if (lxrt_ids_on() && tgid > 0) {
        int host, index;
        if (!lxrt_ids_to_host(tid, &host, &index) ||
            (tgid = lxrt_ids_target_pid(tgid)) < 0 || host != tgid)
            return LERR(ESRCH);
    }
    if (tgid > 0 && tgid != getpid()) {
        if (lsig == 0)
            return kill(tgid, 0) != 0 ? LERR(errno) : 0;
        if (lsig < 0 || lsig > LINUX_NSIG)
            return LERR(EINVAL);
        if (lsig == 6)
            note_foreign_abort(tgid, tid);
        if (xsig_send(tgid, tid, lsig))
            return 0;
        // Not one of ours: the best Darwin can do is the process.
        int d = is_rt(lsig) ? 0 : lxrt_signo_to_darwin(lsig);
        if (!d)
            return LERR(ESRCH);
        return kill(tgid, d) != 0 ? LERR(errno) : 0;
    }
    pthread_t target;
    if (!lxrt_thread_lookup(tid, &target))
        return LERR(ESRCH);
    if (lsig == 0)
        return 0;
    if (is_rt(lsig)) {
        // Queued against the thread; the carrier wakes it (see
        // LXRT_RT_CARRIER). A full queue drops, as Linux does -- EAGAIN.
        if (!lxrt_rt_enqueue(tid, lsig))
            return LERR(EAGAIN);
        int rc = pthread_kill(target, LXRT_RT_CARRIER);
        return rc != 0 ? LERR(rc) : 0;
    }
    int d = lxrt_signo_to_darwin(lsig);
    if (!d)
        return LERR(EINVAL);
    int rc = pthread_kill(target, d);
    if (rc == 0)
        lxrt_signalfd_notify(lsig);       // kqueue cannot see pthread_kill
    return rc != 0 ? LERR(rc) : 0;
}

_Static_assert(sizeof(struct linux_siginfo) == 128,
               "Linux siginfo_t is 128 bytes; a guest reads fields by offset");
_Static_assert(__builtin_offsetof(struct linux_siginfo, si_addr) == 16,
               "si_addr sits at offset 16 for the fault signals");
