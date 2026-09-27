// Does a flip issued INSIDE a signal handler take effect for the handler's
// own stores, and does the flip back leave the thread executable on return?
// FEX's SIGBUS backpatcher (Arm64.cpp:2100-2165) is exactly this: the fault is
// an unaligned atomic, the handler rewrites the instruction, resumes at it.
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>

static uint32_t *code;
static volatile int hits, nested;

static void on_sig(int sig, siginfo_t *si, void *uc) {
    (void)si; (void)uc;
    if (sig == SIGBUS || sig == SIGSEGV) { nested++; if (nested > 3) _exit(3); return; }
    hits++;
    pthread_jit_write_protect_np(0);
    code[0] = 0xd2800540;                       // mov x0, #42
    int ok = (code[0] == 0xd2800540);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(code, 8);
    char m[64]; int n = snprintf(m, sizeof m, "handler: store %s, nested=%d\n", ok ? "landed" : "MISSED", nested);
    write(2, m, n);
}

int main(void) {
    code = mmap(NULL, 16384, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANON|MAP_JIT, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 1; }
    pthread_jit_write_protect_np(0);
    code[0] = 0xd28000a0; code[1] = 0xd65f03c0; // mov x0,#5 ; ret
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(code, 8);
    long (*fn)(void) = (long (*)(void))code;
    printf("before: fn() = %ld\n", fn());
    struct sigaction sa = { .sa_sigaction = on_sig, .sa_flags = SA_SIGINFO };
    sigaction(SIGUSR1, &sa, NULL); sigaction(SIGBUS, &sa, NULL); sigaction(SIGSEGV, &sa, NULL);
    for (int round = 1; round <= 3; round++) {
        kill(getpid(), SIGUSR1);
        printf("round %d: fn() = %ld (hits=%d nested=%d)\n", round, fn(), hits, nested);
    }
    // And the same from a *thread* that is mid-execution of JIT code? Not
    // reproducible here; the question is only whether the handler's flip is honoured.
    return 0;
}
