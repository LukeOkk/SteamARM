// jit_fork, multithreaded: the JIT region is created and filled by one thread,
// the fork happens on another while a third thread keeps emitting code (in
// write mode) into a different region -- the shape of FEX inside the
// multithreaded i386 Steam client. Each child runs the inherited block.
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static long wx(long enable, uint64_t addr, uint64_t len) {
    register long x8 asm("x8") = 0x4C580020; register long x0 asm("x0") = enable;
    register uint64_t x1 asm("x1") = addr; register uint64_t x2 asm("x2") = len;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc"); return x0; }
static void emit(uint32_t *at, uint32_t imm) {
    wx(0, 0, 0); at[0] = 0xd2800000 | (imm << 5); at[1] = 0xd65f03c0;
    __builtin___clear_cache((char *)at, (char *)(at + 2)); wx(1, (uint64_t)at, 8); }
static uint32_t *code;
static volatile int stop;
static void *maker(void *a) { (void)a;
    code = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    emit(code, 7); return NULL; }
static void *churn(void *a) { (void)a;
    uint32_t *r = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    unsigned i = 0; while (!stop) { emit(r + (i++ % 512) * 2, i & 0xfff); } return NULL; }
static void *forker(void *a) { int N = (int)(intptr_t)a, ok = 0;
    for (int i = 0; i < N; i++) {
        if (getenv("JFMT_VERBOSE")) fprintf(stderr, "fork %d\n", i);
        pid_t p = fork();
        if (p == 0) { if (getenv("JFMT_VERBOSE")) write(2, "child\n", 6);
                      long v = ((long (*)(void))code)(); _exit(v == 7 ? 0 : 1); }
        int st = 0; pid_t w = waitpid(p, &st, 0);
        if (getenv("JFMT_VERBOSE")) fprintf(stderr, "waited %d -> %d st=0x%x\n", p, w, st);
        if (WIFEXITED(st) && WEXITSTATUS(st) == 0) ok++;
        else printf("  child %d: status 0x%x\n", i, st);
    }
    if (getenv("JFMT_VERBOSE")) fprintf(stderr, "forker done\n");
    return (void *)(intptr_t)ok; }
int main(int argc, char **argv) {
    int N = argc > 1 ? atoi(argv[1]) : 20;
    pthread_t m, c, f; void *ok;
    pthread_create(&m, NULL, maker, NULL); pthread_join(m, NULL);
    pthread_create(&c, NULL, churn, NULL);
    pthread_create(&f, NULL, forker, (void *)(intptr_t)N); pthread_join(f, &ok);
    if (getenv("JFMT_VERBOSE")) fprintf(stderr, "joined forker\n");
    stop = 1; pthread_join(c, NULL);
    if (getenv("JFMT_VERBOSE")) fprintf(stderr, "joined churn\n");
    printf("== jit_fork_mt: %d ok, %d mal\n", (int)(intptr_t)ok, N - (int)(intptr_t)ok);
    return (int)(intptr_t)ok == N ? 0 : 1;
}
