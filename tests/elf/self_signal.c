// A SIGUSR2 a thread sends itself (tgkill), which the runtime delivers
// synchronously inside the syscall instead of through Darwin's signal path
// (runtime/signal.c sync_self_deliver), the way Wine returns from a syscall
// into a full context. The handler's view and the resume must be exactly what
// a kernel delivery gives:
//   plain     the handler runs once, on the alt stack it asked for, and the
//             thread carries on after the tgkill
//   x16       the handler moves the thread to a new pc with x16 == pc
//             (lxrt_restore_regs): every other register arrives as set
//   x0        a new pc whose first instruction overwrites x0, x16 != pc
//             (lxrt_restore_regs_x0): x16 arrives exact too
//   exact     a new pc where neither holds: every register arrives as set
//             (the resume goes through one real signal)
//   blocked   blocked, it waits, shows in sigpending, and runs on unblock
//   raise     blocked around the tgkill and unblocked right after, as glibc's
//             raise() did: it runs at the unblock, which returns 0
// Run under the runtime with and without LXRT_SYNC_SELF_SIGNAL=0; both PASS.
#define _GNU_SOURCE
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "OK " : "MAL", what);
    if (!ok)
        fails++;
}

static long self_kill(void)
{
    return syscall(SYS_tgkill, getpid(), gettid(), SIGUSR2);
}

// Entry points a handler sends the thread to. Each saves x0-x30 below sp and
// calls check_regs(saved) with sp pointing at the block.
void probe_any(void);
void probe_x0(void);
__asm__(
    ".text\n"
    ".globl probe_any\n"
    "probe_any:\n"
    "    cmp x0, #0\n"              // reads x0: not an x0-dead entry
    "    b 1f\n"
    ".globl probe_x0\n"
    "probe_x0:\n"
    "    mov x0, sp\n"              // overwrites x0 first
    "1:  sub sp, sp, #256\n"
    "    stp x0, x1, [sp, #0]\n"
    "    stp x2, x3, [sp, #16]\n"
    "    stp x4, x5, [sp, #32]\n"
    "    stp x6, x7, [sp, #48]\n"
    "    stp x8, x9, [sp, #64]\n"
    "    stp x10, x11, [sp, #80]\n"
    "    stp x12, x13, [sp, #96]\n"
    "    stp x14, x15, [sp, #112]\n"
    "    stp x16, x17, [sp, #128]\n"
    "    stp x18, x19, [sp, #144]\n"
    "    stp x20, x21, [sp, #160]\n"
    "    stp x22, x23, [sp, #176]\n"
    "    stp x24, x25, [sp, #192]\n"
    "    stp x26, x27, [sp, #208]\n"
    "    stp x28, x29, [sp, #224]\n"
    "    str x30, [sp, #240]\n"
    "    mov x0, sp\n"
    "    b check_regs\n");

static sigjmp_buf back;
static uint64_t seen[31];
static uint64_t magic(int i) { return 0x5100000000000000ull | ((uint64_t)i << 32) | (uint64_t)(i * 0x1111); }

void check_regs(const uint64_t *r)
{
    memcpy(seen, r, sizeof seen);
    siglongjmp(back, 1);
}

enum { MODE_PLAIN, MODE_X16, MODE_X0, MODE_EXACT };
static volatile int mode, hits;
static volatile uintptr_t handler_sp;
static char altstack[64 * 1024];

static void on_usr2(int sig, siginfo_t *si, void *uap)
{
    (void)sig; (void)si;
    char here;
    handler_sp = (uintptr_t)&here;
    hits++;
    if (mode == MODE_PLAIN)
        return;
    ucontext_t *uc = uap;
    for (int i = 0; i <= 30; i++)
        if (i != 18)
            uc->uc_mcontext.regs[i] = magic(i);
    uint64_t target = (uint64_t)(uintptr_t)(mode == MODE_X0 ? probe_x0 : probe_any);
    uc->uc_mcontext.pc = target;
    if (mode == MODE_X16)
        uc->uc_mcontext.regs[16] = target;
}

static void redirect(int m, const char *name)
{
    mode = m;
    memset(seen, 0, sizeof seen);
    if (sigsetjmp(back, 1) == 0) {
        self_kill();
        check(0, name);             // the handler moved the thread: never here
        return;
    }
    uint64_t target = (uint64_t)(uintptr_t)(m == MODE_X0 ? probe_x0 : probe_any);
    int ok = 1;
    for (int i = 1; i <= 30; i++) {
        if (i == 18)
            continue;
        uint64_t want = (i == 16 && m == MODE_X16) ? target : magic(i);
        if (seen[i] != want) {
            printf("    x%d = 0x%llx, wanted 0x%llx\n", i, (unsigned long long)seen[i], (unsigned long long)want);
            ok = 0;
        }
    }
    if (m != MODE_X0 && seen[0] != magic(0)) {
        printf("    x0 = 0x%llx, wanted 0x%llx\n", (unsigned long long)seen[0], (unsigned long long)magic(0));
        ok = 0;
    }
    check(ok, name);
}

int main(void)
{
    stack_t ss = { .ss_sp = altstack, .ss_size = sizeof altstack };
    sigaltstack(&ss, NULL);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_usr2;
    sa.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
    sigaction(SIGUSR2, &sa, NULL);

    mode = MODE_PLAIN;
    hits = 0;
    long r = self_kill();
    check(r == 0 && hits == 1, "plain: handler ran once, tgkill returned 0");
    check(handler_sp >= (uintptr_t)altstack && handler_sp < (uintptr_t)altstack + sizeof altstack,
          "plain: handler on the alt stack");

    redirect(MODE_X16, "x16: new pc with x16 == pc, registers as the handler set them");
    redirect(MODE_X0, "x0: new pc that overwrites x0 first, x16 and the rest exact");
    redirect(MODE_EXACT, "exact: new pc, every register as the handler set them");

    mode = MODE_PLAIN;
    hits = 0;
    sigset_t u2, old, pend;
    sigemptyset(&u2);
    sigaddset(&u2, SIGUSR2);
    sigprocmask(SIG_BLOCK, &u2, &old);
    self_kill();
    check(hits == 0, "blocked: not run while blocked");
    sigpending(&pend);
    check(sigismember(&pend, SIGUSR2), "blocked: pending");
    sigprocmask(SIG_SETMASK, &old, NULL);
    check(hits == 1, "blocked: ran on unblock");

    hits = 0;
    sigset_t all;
    sigfillset(&all);
    sigprocmask(SIG_BLOCK, &all, &old);
    self_kill();
    r = sigprocmask(SIG_SETMASK, &old, NULL);
    check(hits == 1 && r == 0, "raise: ran at the unblock, which returned 0");
    sigset_t now;
    sigprocmask(SIG_BLOCK, NULL, &now);
    check(!sigismember(&now, SIGUSR2) && !sigismember(&now, SIGUSR1), "raise: mask back as it was");

    // Many in a row: no state left behind between deliveries.
    hits = 0;
    for (int i = 0; i < 20000; i++)
        self_kill();
    check(hits == 20000, "20000 in a row, each run once");

    if (fails)
        return 1;
    printf("PASS\n");
    return 0;
}
