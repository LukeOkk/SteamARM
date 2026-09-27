// A JIT that forks, with a SIGCHLD storm: the shape of FEX under bash.
// Parent: emits a block in a MAP_JIT-backed region (via the runtime's private
// W^X syscall), installs a SIGCHLD handler, then forks N children; each child
// executes the inherited block, emits its own block into the same region,
// executes both, and exits with a code the parent checks.
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static long wx(long enable, uint64_t addr, uint64_t len) {
    register long x8 asm("x8") = 0x4C580020; register long x0 asm("x0") = enable;
    register uint64_t x1 asm("x1") = addr; register uint64_t x2 asm("x2") = len;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc"); return x0; }
static volatile sig_atomic_t chld;
static void on_chld(int s) { (void)s; chld++; }
static void emit(uint32_t *at, uint32_t imm) {
    wx(0, 0, 0); at[0] = 0xd2800000 | (imm << 5); at[1] = 0xd65f03c0;
    __builtin___clear_cache((char *)at, (char *)(at + 2)); wx(1, (uint64_t)at, 8); }
int main(int argc, char **argv) {
    int N = argc > 1 ? atoi(argv[1]) : 40;
    uint32_t *code = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (code == MAP_FAILED) { perror("mmap"); return 2; }
    emit(code, 5);
    long (*fn)(void) = (long (*)(void))code;
    if (fn() != 5) { printf("parent block wrong\n"); return 2; }
    struct sigaction sa = { .sa_handler = on_chld, .sa_flags = SA_RESTART }; sigaction(SIGCHLD, &sa, NULL);
    int ok = 0, bad = 0;
    for (int i = 0; i < N; i++) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 2; }
        if (pid == 0) {
            long a = fn();
            uint32_t *mine = code + 64 + (i % 100) * 4;
            emit(mine, 42 + i);
            long b = ((long (*)(void))mine)();
            long c = fn();
            _exit(a == 5 && b == 42 + i && c == 5 ? 0 : 1);
        }
        // Keep the parent busy in the JIT too, like bash's own translated code.
        emit(code + 32, 7 + i); if (((long (*)(void))(code + 32))() != 7 + i) bad++;
        int st = 0; waitpid(pid, &st, 0);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) ok++; else { bad++; printf("child %d: %s %d\n", i, WIFEXITED(st) ? "exit" : "signal", WIFEXITED(st) ? WEXITSTATUS(st) : WTERMSIG(st)); }
    }
    printf("== %d ok, %d mal (SIGCHLD seen %d)\n", ok, bad, (int)chld);
    return bad ? 1 : 0;
}
