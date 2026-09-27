// i386 probe: x86 memory ordering (TSO) must hold under FEX. Message passing:
// the writer stores data then flag; a reader that sees the flag must see the
// data. On ARM without FEX's ordering emulation the pair can be observed out
// of order. FEX turns that emulation off when the kernel claims hardware TSO
// (prctl PR_SET_MEM_MODEL); the runtime used to answer 0 to every prctl.
#include <stdint.h>
typedef unsigned long pthread_t;
extern int printf(const char *, ...);
extern int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_join(pthread_t, void **);
extern void exit(int) __attribute__((noreturn));

#define ROUNDS 200000
static volatile uint32_t data[64], flag[64];
static volatile uint32_t go;
static volatile long violations;

static void *writer(void *a)
{
    (void)a;
    for (uint32_t r = 1; r <= ROUNDS; r++) {
        uint32_t s = r & 63;
        data[s] = r;                 // plain stores: x86 keeps them in order
        flag[s] = r;
    }
    return 0;
}

static void *reader(void *a)
{
    (void)a;
    long bad = 0;
    for (uint32_t r = 1; r <= ROUNDS * 4; r++) {
        uint32_t s = r & 63;
        uint32_t f = flag[s];
        uint32_t d = data[s];        // must be >= f on x86
        if (d < f) bad++;
    }
    violations += bad;
    return 0;
}

static int check(void)
{
    pthread_t w, rd[3];
    pthread_create(&w, 0, writer, 0);
    for (int i = 0; i < 3; i++) pthread_create(&rd[i], 0, reader, 0);
    pthread_join(w, 0);
    for (int i = 0; i < 3; i++) pthread_join(rd[i], 0);
    printf("message-passing violations: %ld\n", violations);
    printf(violations ? "== tso: MAL\n" : "== tso: ok\n");
    return violations != 0;
}
void _start(void) { exit(check()); }
