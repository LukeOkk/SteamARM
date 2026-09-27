// MIGRATION_PLAN Stage 1: what does it cost to intercept one Linux syscall on
// Darwin, and can it even be done reliably?
//
// A ZERO-VM runtime has to take a Linux ELF's `svc #0` and service it itself.
// Three things decide whether that is viable, and this program measures all
// three on the real machine rather than assuming them:
//
//   1. Does Darwin distinguish `svc #0` from its own `svc #0x80`? If it does
//      not, a Linux binary's syscall silently runs whatever Darwin syscall
//      number happens to be in x16 -- interception by trapping is impossible
//      and the loader must rewrite every svc site.
//   2. What does one signal-based interception cost (SIGSYS from an invalid
//      syscall number)?
//   3. What does one Mach-exception-based interception cost (EXC_BREAKPOINT
//      on a dedicated handler thread)?
//
// Compare the answers against the cost of the VM exit they would replace
// (benchmarks/vmexit_cost.md).
//
// Build:  clang -O2 -arch arm64 -o build/syscall_cost benchmarks/syscall_cost.c
// Run:    ./build/syscall_cost

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <sys/ucontext.h>

// ---------------------------------------------------------------- timing

static double g_ns_per_tick = 0.0;

static void timebase_init(void)
{
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    g_ns_per_tick = (double)tb.numer / (double)tb.denom;
}

static inline uint64_t ticks(void) { return mach_absolute_time(); }

static double ns_since(uint64_t start)
{
    return (double)(ticks() - start) * g_ns_per_tick;
}

struct result {
    const char *name;
    const char *note;
    double per_op_ns;
    uint64_t iterations;
    bool valid;
};

#define MAX_RESULTS 12
static struct result g_results[MAX_RESULTS];
static int g_result_count;

static void record(const char *name, double total_ns, uint64_t iters,
                   bool valid, const char *note)
{
    if (g_result_count >= MAX_RESULTS)
        return;
    g_results[g_result_count++] = (struct result){
        .name = name,
        .note = note,
        .per_op_ns = valid && iters ? total_ns / (double)iters : 0.0,
        .iterations = iters,
        .valid = valid,
    };
}

// ------------------------------------------------------- baseline: a call

__attribute__((noinline)) static uint64_t empty_call(uint64_t x) { return x + 1; }
static uint64_t (*volatile g_call)(uint64_t) = empty_call;

// ----------------------------------------------- Darwin's own syscall path

// getpid, BSD syscall 20. Raw svc so libc's cached pid does not hide the cost.
__attribute__((noinline)) static long darwin_getpid_svc80(void)
{
    register long x16 __asm__("x16") = 20;
    register long x0 __asm__("x0");
    __asm__ __volatile__("svc #0x80" : "=r"(x0) : "r"(x16) : "memory", "cc");
    return x0;
}

// The same call with the Linux immediate. If this returns our pid, Darwin
// ignores the SVC immediate entirely -- see finding (1) at the top.
__attribute__((noinline)) static long darwin_getpid_svc0(void)
{
    register long x16 __asm__("x16") = 20;
    register long x0 __asm__("x0");
    __asm__ __volatile__("svc #0" : "=r"(x0) : "r"(x16) : "memory", "cc");
    return x0;
}

// ------------------------------------------------------- SIGSYS path

static volatile sig_atomic_t g_signal_hits;
static volatile sig_atomic_t g_signal_seen;

static void trap_handler(int sig, siginfo_t *info, void *uap)
{
    (void)info;
    g_signal_hits++;
    g_signal_seen = sig;
    // Step over the faulting instruction, which is what a real runtime would
    // do after servicing the call. Not arm64e: __pc is a plain field here.
    ucontext_t *uc = (ucontext_t *)uap;
    uc->uc_mcontext->__ss.__pc += 4;
    // A real runtime would also write the Linux return value into x0.
    uc->uc_mcontext->__ss.__x[0] = 0;
}

// An out-of-range BSD syscall number. Which signal -- if any -- XNU raises for
// it is exactly what this measures; it is not the same across architectures.
#define BAD_SYSCALL_NR 0x00ffffff

__attribute__((noinline)) static void trigger_bad_syscall(void)
{
    register long x16 __asm__("x16") = BAD_SYSCALL_NR;
    __asm__ __volatile__("svc #0x80" : : "r"(x16) : "x0", "memory", "cc");
}

// ------------------------------------------- Mach exception path

static mach_port_t g_exc_port = MACH_PORT_NULL;
static volatile uint64_t g_exc_hits;

#pragma pack(push, 4)
typedef struct {
    mach_msg_header_t Head;
    mach_msg_body_t msgh_body;
    mach_msg_port_descriptor_t thread;
    mach_msg_port_descriptor_t task;
    NDR_record_t NDR;
    exception_type_t exception;
    mach_msg_type_number_t codeCnt;
    int64_t code[2];
    char trailer[64];
} exc_request_t;

typedef struct {
    mach_msg_header_t Head;
    NDR_record_t NDR;
    kern_return_t RetCode;
} exc_reply_t;
#pragma pack(pop)

static void *exception_thread(void *arg)
{
    (void)arg;
    for (;;) {
        exc_request_t req;
        memset(&req, 0, sizeof(req));
        kern_return_t kr = mach_msg(&req.Head, MACH_RCV_MSG, 0, sizeof(req),
                                    g_exc_port, MACH_MSG_TIMEOUT_NONE,
                                    MACH_PORT_NULL);
        if (kr != KERN_SUCCESS)
            break;

        // Advance the faulting thread past the trap instruction, exactly as a
        // syscall emulator would after servicing the call.
        arm_thread_state64_t state;
        mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
        kern_return_t sk = thread_get_state(req.thread.name, ARM_THREAD_STATE64,
                                            (thread_state_t)&state, &count);
        if (sk == KERN_SUCCESS) {
            state.__pc += 4;
            state.__x[0] = 0;
            thread_set_state(req.thread.name, ARM_THREAD_STATE64,
                             (thread_state_t)&state, count);
        }
        g_exc_hits++;

        mach_port_deallocate(mach_task_self(), req.thread.name);
        mach_port_deallocate(mach_task_self(), req.task.name);

        exc_reply_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.Head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(req.Head.msgh_bits), 0);
        rep.Head.msgh_size = sizeof(rep);
        rep.Head.msgh_remote_port = req.Head.msgh_remote_port;
        rep.Head.msgh_local_port = MACH_PORT_NULL;
        rep.Head.msgh_id = req.Head.msgh_id + 100;
        rep.NDR = NDR_record;
        rep.RetCode = sk == KERN_SUCCESS ? KERN_SUCCESS : KERN_FAILURE;
        mach_msg(&rep.Head, MACH_SEND_MSG, sizeof(rep), 0, MACH_PORT_NULL,
                 MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
    }
    return NULL;
}

static bool exception_setup(void)
{
    mach_port_t self = mach_task_self();
    if (mach_port_allocate(self, MACH_PORT_RIGHT_RECEIVE, &g_exc_port) != KERN_SUCCESS)
        return false;
    if (mach_port_insert_right(self, g_exc_port, g_exc_port,
                               MACH_MSG_TYPE_MAKE_SEND) != KERN_SUCCESS)
        return false;
    // Thread-level, not task-level: a real runtime only wants to intercept the
    // threads running guest code, and leaving the task port alone keeps the
    // debugger usable.
    if (thread_set_exception_ports(mach_thread_self(), EXC_MASK_BREAKPOINT,
                                   g_exc_port,
                                   EXCEPTION_DEFAULT | MACH_EXCEPTION_CODES,
                                   ARM_THREAD_STATE64) != KERN_SUCCESS)
        return false;

    pthread_t th;
    if (pthread_create(&th, NULL, exception_thread, NULL) != 0)
        return false;
    pthread_detach(th);
    return true;
}

__attribute__((noinline)) static void trigger_breakpoint(void)
{
    __asm__ __volatile__("brk #0" : : : "x0", "memory", "cc");
}

// ------------------------------------------- rewritten-svc path

// Findings [1] and [2] leave only one mechanism: the loader rewrites each
// `svc #0` site into a branch to the runtime's dispatcher. This measures what
// that actually costs end to end -- the branch, reading the Linux arguments
// out of the guest register layout, a dispatch table lookup, and servicing the
// call with the real Darwin syscall underneath.

struct linux_regs {
    uint64_t x[8];   // x0..x5 are the Linux arguments, x8 is the number
};

typedef long (*linux_handler_t)(struct linux_regs *);

// Linux aarch64: getpid is 172.
#define LINUX_NR_GETPID 172
#define DISPATCH_SIZE 512

static linux_handler_t g_dispatch[DISPATCH_SIZE];

__attribute__((noinline)) static long handle_getpid(struct linux_regs *r)
{
    (void)r;
    register long x16 __asm__("x16") = 20;
    register long x0 __asm__("x0");
    __asm__ __volatile__("svc #0x80" : "=r"(x0) : "r"(x16) : "memory", "cc");
    return x0;
}

__attribute__((noinline)) static long handle_enosys(struct linux_regs *r)
{
    (void)r;
    return -38; // -ENOSYS
}

// What the rewritten site branches to.
__attribute__((noinline)) static long runtime_dispatch(struct linux_regs *regs)
{
    uint64_t nr = regs->x[7];
    linux_handler_t h = nr < DISPATCH_SIZE ? g_dispatch[nr] : NULL;
    return h ? h(regs) : handle_enosys(regs);
}

static long (*volatile g_dispatch_entry)(struct linux_regs *) = runtime_dispatch;

static void bench_rewritten_svc(void)
{
    for (int i = 0; i < DISPATCH_SIZE; i++)
        g_dispatch[i] = handle_enosys;
    g_dispatch[LINUX_NR_GETPID] = handle_getpid;

    struct linux_regs regs;
    memset(&regs, 0, sizeof(regs));
    regs.x[7] = LINUX_NR_GETPID;

    printf("\n[4] Rewritten `svc` site: branch into the runtime dispatcher\n");
    long pid = g_dispatch_entry(&regs);
    if (pid != (long)getpid()) {
        printf("    dispatcher returned %ld, expected %d\n", pid, getpid());
        record("rewritten svc -> dispatcher", 0, 0, false, "wrong result");
        return;
    }
    printf("    Serviced Linux getpid(172) through the dispatch table.\n");

    const uint64_t iters = 1000000;
    uint64_t t0 = ticks();
    for (uint64_t i = 0; i < iters; i++)
        pid = g_dispatch_entry(&regs);
    record("rewritten svc -> dispatcher", ns_since(t0), iters, true,
           "branch + table lookup + real Darwin syscall");

    // The same path for a call the runtime can answer without going to the
    // kernel at all (a cached value, a vDSO-style clock, a futex fast path).
    regs.x[7] = 999; // unmapped -> ENOSYS handler, no kernel involvement
    t0 = ticks();
    for (uint64_t i = 0; i < iters; i++)
        (void)g_dispatch_entry(&regs);
    record("rewritten svc, serviced in userspace", ns_since(t0), iters, true,
           "branch + table lookup only, no kernel");
}

// ---------------------------------------------------------------- main

static void run_baselines(void)
{
    const uint64_t iters = 2000000;
    uint64_t acc = 0;
    uint64_t t0 = ticks();
    for (uint64_t i = 0; i < iters; i++)
        acc += g_call(i);
    double total = ns_since(t0);
    record("indirect function call", total, iters, true, "cost floor");
    if (acc == 0)
        fputs("", stderr); // keep the accumulator alive

    const uint64_t sys_iters = 500000;
    long pid = 0;
    t0 = ticks();
    for (uint64_t i = 0; i < sys_iters; i++)
        pid = darwin_getpid_svc80();
    total = ns_since(t0);
    bool ok = pid == (long)getpid();
    record("Darwin syscall, svc #0x80", total, sys_iters, ok,
           ok ? "getpid via the native path" : "UNEXPECTED: wrong pid returned");
}

static void probe_svc_immediate(void)
{
    long pid = darwin_getpid_svc0();
    printf("\n[1] Does Darwin distinguish `svc #0` from `svc #0x80`?\n");
    if (pid == (long)getpid()) {
        printf("    NO. `svc #0` with x16=20 returned our pid (%ld).\n", pid);
        printf("    Darwin ignores the SVC immediate and dispatches on x16.\n");
        printf("    CONSEQUENCE: a Linux binary's `svc #0` does not trap. It\n");
        printf("    executes whatever Darwin syscall x16 happens to hold, with\n");
        printf("    Linux arguments. Interception by trapping is NOT AVAILABLE;\n");
        printf("    the loader must rewrite every svc site, and self-modifying\n");
        printf("    or JIT-generated code (FEX) needs a different mechanism.\n");
    } else {
        printf("    YES. `svc #0` returned %ld, not our pid (%d).\n", pid, getpid());
        printf("    A Linux `svc #0` can be trapped and serviced directly.\n");
    }

    const uint64_t iters = 500000;
    uint64_t t0 = ticks();
    for (uint64_t i = 0; i < iters; i++)
        (void)darwin_getpid_svc0();
    record("Darwin syscall, svc #0", ns_since(t0), iters, true,
           "same path as svc #0x80 if the immediate is ignored");
}

// Signals a bad syscall could plausibly arrive as. Installing all of them and
// reporting which one fires is cheaper than guessing from XNU source.
static const int kTrapSignals[] = { SIGSYS, SIGILL, SIGTRAP, SIGBUS, SIGSEGV };

static const char *signal_name(int sig)
{
    switch (sig) {
    case SIGSYS:  return "SIGSYS";
    case SIGILL:  return "SIGILL";
    case SIGTRAP: return "SIGTRAP";
    case SIGBUS:  return "SIGBUS";
    case SIGSEGV: return "SIGSEGV";
    default:      return "other";
    }
}

static void bench_signal_interception(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = trap_handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof(kTrapSignals) / sizeof(kTrapSignals[0]); i++) {
        if (sigaction(kTrapSignals[i], &sa, NULL) != 0) {
            record("signal interception", 0, 0, false, "sigaction failed");
            return;
        }
    }

    // One probe first: if an invalid syscall number raises nothing catchable,
    // this mechanism does not exist and there is nothing to time.
    g_signal_hits = 0;
    g_signal_seen = 0;
    trigger_bad_syscall();
    printf("\n[2] Does an invalid syscall number raise a catchable signal?\n");
    if (!g_signal_hits) {
        printf("    NO. svc with x16=0x%x returned without a signal.\n", BAD_SYSCALL_NR);
        printf("    Signal-based interception of a bad number is not available.\n");
        record("signal interception", 0, 0, false, "no signal raised");
        return;
    }
    printf("    YES, as %s. Handler ran and stepped the pc past the svc.\n",
           signal_name(g_signal_seen));

    const uint64_t iters = 100000;
    uint64_t t0 = ticks();
    for (uint64_t i = 0; i < iters; i++)
        trigger_bad_syscall();
    double total = ns_since(t0);
    static char note[96];
    snprintf(note, sizeof(note), "svc -> %s -> handler -> pc+=4 -> resume",
             signal_name(g_signal_seen));
    record("in-process signal interception", total, iters, true, note);
}

static void bench_mach_exception(void)
{
    printf("\n[3] Mach exception interception (EXC_BREAKPOINT, handler thread)\n");
    if (!exception_setup()) {
        printf("    setup failed (%s)\n", strerror(errno));
        record("Mach exception interception", 0, 0, false, "setup failed");
        return;
    }

    g_exc_hits = 0;
    trigger_breakpoint();
    if (!g_exc_hits) {
        printf("    no exception delivered\n");
        record("Mach exception interception", 0, 0, false, "no exception delivered");
        return;
    }
    printf("    Handler thread received the exception and advanced the pc.\n");

    const uint64_t iters = 50000;
    uint64_t t0 = ticks();
    for (uint64_t i = 0; i < iters; i++)
        trigger_breakpoint();
    double total = ns_since(t0);
    record("Mach exception interception", total, iters, true,
           "trap -> IPC to handler thread -> thread_set_state -> reply");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    timebase_init();

    printf("SteamARM Stage 1 -- cost of intercepting one Linux syscall on Darwin\n");
    printf("pid %d, timebase %.4f ns/tick\n", getpid(), g_ns_per_tick);

    run_baselines();
    probe_svc_immediate();
    bench_signal_interception();
    bench_mach_exception();
    bench_rewritten_svc();

    printf("\n%-32s %14s %12s  %s\n", "mechanism", "ns/op", "iterations", "note");
    printf("%-32s %14s %12s  %s\n", "--------------------------------",
           "--------------", "------------", "----");
    double floor_ns = 0.0;
    for (int i = 0; i < g_result_count; i++) {
        struct result *r = &g_results[i];
        if (i == 0)
            floor_ns = r->per_op_ns;
        if (r->valid)
            printf("%-32s %14.1f %12llu  %s\n", r->name, r->per_op_ns,
                   r->iterations, r->note ? r->note : "");
        else
            printf("%-32s %14s %12s  %s\n", r->name, "UNAVAILABLE", "-",
                   r->note ? r->note : "");
    }
    (void)floor_ns;

    printf("\nCompare against the VM exit these would replace: see\n");
    printf("benchmarks/vmexit_cost.md for how that number is obtained.\n");
    return 0;
}
