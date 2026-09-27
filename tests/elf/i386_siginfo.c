// i386 probe: a SA_SIGINFO SIGSEGV handler must receive real guest pointers.
// Under a guest base, FEX used to hand the handler its own host pointers
// truncated to 32 bits for siginfo/ucontext (and the host fault address), so
// crash handlers such as breakpad read garbage. Checks: siginfo and ucontext
// lie on the handler's stack, si_addr is the faulting guest address, and the
// saved EIP/ESP are plausible; then the handler longjmps out.
#include <stddef.h>
#include <stdint.h>
extern int printf(const char *, ...);
extern void exit(int) __attribute__((noreturn));

typedef struct { unsigned long sig[32]; } sigset_t_;   // glibc i386 sigset_t: 128 bytes
struct sigaction_ {
    void (*handler)(int, void *, void *);
    sigset_t_ mask;
    int flags;
    void (*restorer)(void);
};
extern int sigaction(int, const struct sigaction_ *, struct sigaction_ *);
typedef int jmp_buf_[39];
extern int __sigsetjmp(jmp_buf_, int);
extern void siglongjmp(jmp_buf_, int) __attribute__((noreturn));

#define SA_SIGINFO 4
#define SA_NODEFER 0x40000000

static jmp_buf_ env;
static volatile uint32_t got_addr, got_info, got_uc, got_eip, got_esp, got_signo, handler_esp;
static volatile int bad = 0;

static void handler(int sig, void *info, void *uc)
{
    uint32_t esp;
    __asm__ volatile("movl %%esp, %0" : "=r"(esp));
    handler_esp = esp;
    got_signo = sig;
    got_info = (uint32_t)(uintptr_t)info;
    got_uc = (uint32_t)(uintptr_t)uc;
    // siginfo: si_signo, si_errno, si_code, then si_addr.
    got_addr = ((volatile uint32_t *)info)[3];
    // ucontext: uc_flags, uc_link, uc_stack(12), then gregs; ESP 7, EIP 14.
    volatile uint32_t *g = (volatile uint32_t *)((char *)uc + 20);
    got_eip = g[14];
    got_esp = g[7];
    siglongjmp(env, 1);
}

static int check(void)
{
    struct sigaction_ sa = {0};
    sa.handler = handler;
    sa.flags = SA_SIGINFO | SA_NODEFER;
    sigaction(11, &sa, 0);
    volatile uint32_t *target = (volatile uint32_t *)(uintptr_t)0x10;
    if (!__sigsetjmp(env, 1)) {
        *target = 1;       // faults: the guest's page 0 is never mapped
        printf("no fault?\n");
        return 1;
    }
    printf("signo %u info 0x%x uc 0x%x handler esp 0x%x si_addr 0x%x eip 0x%x esp 0x%x\n",
           got_signo, got_info, got_uc, handler_esp, got_addr, got_eip, got_esp);
    int ok = 1;
    if (got_signo != 11) { printf("  signo wrong\n"); ok = 0; }
    // info/uc live in the frame just above the handler's stack pointer.
    if (got_info < handler_esp || got_info - handler_esp > 0x4000) { printf("  info not on the handler stack\n"); ok = 0; }
    if (got_uc < handler_esp || got_uc - handler_esp > 0x4000) { printf("  uc not on the handler stack\n"); ok = 0; }
    if (got_addr != 0x10) { printf("  si_addr wrong\n"); ok = 0; }
    if (got_eip < (uint32_t)(uintptr_t)check || got_eip > (uint32_t)(uintptr_t)check + 0x1000) { printf("  eip outside check()\n"); ok = 0; }
    if (got_esp < 0x1000 || got_esp == 0) { printf("  esp implausible\n"); ok = 0; }
    printf(ok ? "== siginfo: ok\n" : "== siginfo: MAL\n");
    return !ok;
}
void _start(void) { exit(check()); }
