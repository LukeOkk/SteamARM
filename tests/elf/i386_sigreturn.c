// i386 probe: signal handlers that RETURN. Under a guest base FEX looked for
// its saved host state through the raw guest stack pointer on sigreturn, so
// every returning handler faulted inside FEX, forever. Three shapes:
//   1. SA_SIGINFO handler for an async signal (rt_sigreturn),
//   2. plain handler, no SA_SIGINFO (glibc still uses rt frames on i386,
//      but the handler signature differs),
//   3. SIGSEGV handler that repairs the fault (mprotect) and returns, the
//      pattern of every GC write barrier and JIT (Mono, Wine, Java).
#include <stddef.h>
#include <stdint.h>
extern int printf(const char *, ...);
extern void exit(int) __attribute__((noreturn));
extern int kill(int, int);
extern int getpid(void);
extern void *mmap(void *, size_t, int, int, int, long);
extern int mprotect(void *, size_t, int);

typedef struct { unsigned long sig[32]; } sigset_t_;
struct sigaction_ {
    void *handler;
    sigset_t_ mask;
    int flags;
    void (*restorer)(void);
};
extern int sigaction(int, const struct sigaction_ *, struct sigaction_ *);

#define SA_SIGINFO 4
#define SIGUSR1 10
#define SIGUSR2 12
#define SIGSEGV 11

static volatile int n_usr1, n_usr2, n_segv;
static volatile char *page;

static void on_usr1(int sig, void *info, void *uc) { (void)info; (void)uc; if (sig == SIGUSR1) n_usr1++; }
static void on_usr2(int sig) { if (sig == SIGUSR2) n_usr2++; }
static void on_segv(int sig, void *info, void *uc)
{
    (void)sig; (void)uc;
    uint32_t addr = ((volatile uint32_t *)info)[3];
    if (page && addr >= (uint32_t)(uintptr_t)page && addr < (uint32_t)(uintptr_t)page + 4096) {
        mprotect((void *)page, 4096, 3);
        n_segv++;
        return;
    }
    printf("unexpected SIGSEGV at 0x%x\n", addr);
    exit(2);
}

static int check(void)
{
    struct sigaction_ sa = {0};
    sa.handler = (void *)on_usr1; sa.flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, 0);
    sa.handler = (void *)on_usr2; sa.flags = 0;
    sigaction(SIGUSR2, &sa, 0);
    sa.handler = (void *)on_segv; sa.flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, 0);

    for (int i = 0; i < 100; i++) {
        kill(getpid(), SIGUSR1);
        kill(getpid(), SIGUSR2);
    }
    // A 4 KiB guest page shares its 16 KiB host page with its neighbours and
    // the host page carries the union of their protections (runtime/subpage.c):
    // a lone PROT_NONE page next to read/write ones is not enforced -- the
    // same as on a 16 KiB-page Linux kernel. Keep the neighbours PROT_NONE too
    // (a 64 KiB PROT_NONE region, test page on a 16 KiB boundary).
    uintptr_t base = (uintptr_t)mmap(0, 65536, 0, 0x22, -1, 0);   // PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS
    page = (volatile char *)((base + 16383) & ~(uintptr_t)16383);
    int sum = 0;
    for (int i = 0; i < 10; i++) {
        page[i * 8] = (char)(i + 1);               // first write faults, the handler repairs
        sum += page[i * 8];
        mprotect((void *)page, 4096, 0);           // and again next time round
    }
    printf("usr1 %d usr2 %d segv %d sum %d\n", n_usr1, n_usr2, n_segv, sum);
    int ok = n_usr1 == 100 && n_usr2 == 100 && n_segv == 10 && sum == 55;
    printf(ok ? "== sigreturn: ok\n" : "== sigreturn: MAL\n");
    return !ok;
}
void _start(void) { exit(check()); }
