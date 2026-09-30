// rt_tgsigqueueinfo to the calling thread: the handler gets the siginfo the
// caller queued (FEX's seccomp emulation raises SIGSYS so, with si_code
// SYS_SECCOMP, si_call_addr, si_syscall and si_arch), for SIGSYS and for a
// realtime signal; rt_sigqueueinfo to the process too.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile int got_code, got_errno, got_sys, got_arch, got_sig;
static volatile void *got_addr;

static void handler(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    got_sig = sig;
    got_code = si->si_code;
    got_errno = si->si_errno;
    got_addr = si->si_call_addr;
    got_sys = si->si_syscall;
    got_arch = (int)si->si_arch;
}

static int fails;

static void one(const char *what, int sig, int process)
{
    got_sig = got_code = got_errno = got_sys = got_arch = 0;
    got_addr = 0;
    siginfo_t si;
    memset(&si, 0, sizeof si);
    si.si_signo = sig;
    si.si_code = 1;                         // SYS_SECCOMP (a negative code for a queued one works too)
    si.si_errno = 0x42;
    si.si_call_addr = (void *)0x12345678;
    si.si_syscall = 39;
    si.si_arch = 0xc000003e;
    long r = process ? syscall(SYS_rt_sigqueueinfo, getpid(), sig, &si)
                     : syscall(SYS_rt_tgsigqueueinfo, getpid(), gettid(), sig, &si);
    int good = r == 0 && got_sig == sig && got_code == 1 && got_errno == 0x42 &&
               got_addr == (void *)0x12345678 && got_sys == 39 && (unsigned)got_arch == 0xc000003e;
    printf("  %s  %s: r=%ld code=%d errno=0x%x addr=%p syscall=%d arch=0x%x\n", good ? "OK " : "MAL", what, r,
           got_code, got_errno, (void *)got_addr, got_sys, (unsigned)got_arch);
    if (!good) fails++;
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSYS, &sa, NULL);
    sigaction(SIGRTMIN + 3, &sa, NULL);
    one("SIGSYS to this thread", SIGSYS, 0);
    one("SIGRTMIN+3 to this thread", SIGRTMIN + 3, 0);
    one("SIGSYS to this process", SIGSYS, 1);
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
