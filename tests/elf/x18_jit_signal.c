// X18_JIT: does code the runtime never rewrites keep x18?
//
// Generated code (an anonymous read-write-execute mapping, as a JIT makes it:
// the runtime scans it for svc and TPIDR_EL0, never for x18) holds a value in
// the hardware x18 and spins, checking it, while a 1 ms interval timer runs a
// guest signal handler over it. Linux HotSpot's C1/C2 and llvmpipe's LLVM
// JIT allocate x18 like that (benchmarks/stage24-minecraft-prism.txt), and
// HotSpot's safepoint polls and implicit null checks are SIGSEGV handlers
// taken inside such code.
//
//   parent       in the process lxrun was exec'd as
//   fork child   the same code in a child of fork() (no exec)
//
// Each line says "kept" or "lost after N of M checks". Whether x18 can be
// kept is the kernel's choice: xnu keeps it for a binary linked against an
// SDK older than macOS 13 (the keep-x18 build of lxrun), and only in the
// process it exec'd -- a forked child loses it (MEASURED on macOS 27,
// benchmarks/stage28-keep-x18.txt). The x18 rewriter does not depend on
// this; generated code does. tests/elf/run.sh decides what to expect from
// the lxrun build. Exit status: 0 parent kept, 1 parent lost.
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define CHECKS 400000UL

static volatile sig_atomic_t ticks;
static void on_alarm(int sig) { (void)sig; ticks++; }

// uint64_t spin(uint64_t magic /* x0 */, uint64_t checks /* x1 */):
// 0 when x18 held magic at every check, else the checks still to go.
static const uint32_t code[] = {
    0xaa0003f2,   //  0 mov  x18, x0
    0xd2807d02,   //  1 loop: mov x2, #1000
    0xf1000442,   //  2 inner: subs x2, x2, #1
    0x54ffffe1,   //  3 b.ne inner
    0xeb00025f,   //  4 cmp  x18, x0
    0x540000a1,   //  5 b.ne lost
    0xf1000421,   //  6 subs x1, x1, #1
    0x54ffff41,   //  7 b.ne loop
    0xd2800000,   //  8 mov  x0, #0
    0xd65f03c0,   //  9 ret
    0xaa0103e0,   // 10 lost: mov x0, x1
    0xd65f03c0,   // 11 ret
};
typedef uint64_t (*spin_t)(uint64_t, uint64_t);

static uint64_t run(spin_t f, const char *who)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alarm;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, 1000}, {0, 1000}};
    ticks = 0;
    setitimer(ITIMER_REAL, &it, NULL);
    uint64_t left = f(UINT64_C(0x5a18c0de12345678), CHECKS);
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &off, NULL);
    if (left)
        printf("  %-10s lost after %lu of %lu checks (%d signals so far)\n", who,
               (unsigned long)(CHECKS - left), (unsigned long)CHECKS, (int)ticks);
    else
        printf("  %-10s kept through %lu checks and %d signal handlers\n", who, (unsigned long)CHECKS, (int)ticks);
    fflush(stdout);
    return left;
}

int main(void)
{
    uint32_t *p = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("mmap"); return 2; }
    memcpy(p, code, sizeof code);
    __builtin___clear_cache((char *)p, (char *)p + sizeof code);
    spin_t f = (spin_t)(uintptr_t)p;
    uint64_t parent = run(f, "parent");
    pid_t pid = fork();
    if (pid == 0)
        _exit(run(f, "fork child") ? 1 : 0);
    int st = 0;
    waitpid(pid, &st, 0);
    printf("== x18_jit: parent %s, fork child %s\n", parent ? "lost" : "kept",
           WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "kept" : "lost");
    return parent ? 1 : 0;
}
