// i386 probe: `lock cmpxchg8b` on a 64-bit word that straddles a 16-byte
// boundary, hammered by several threads (the Steam client's lock-free
// TSList heads sit at such addresses). ARMv8 cannot do that CAS in one
// instruction; FEX emulates it from SIGBUS (runtime/signal.c delivers the
// alignment fault as BUS_ADRALN). Every increment must survive: a torn CAS
// loses or duplicates counts.
#include <stddef.h>
#include <stdint.h>
typedef unsigned long pthread_t;
extern int printf(const char *, ...);
extern int pthread_create(pthread_t *, const void *, void *(*)(void *), void *);
extern int pthread_join(pthread_t, void **);
extern void exit(int) __attribute__((noreturn));

static unsigned char buf[64] __attribute__((aligned(64)));
#define THREADS 4
#define ITERS   20000

static uint64_t *word(void) { return (uint64_t *)(buf + 12); }   // crosses buf+16

static void add1(volatile uint64_t *p)
{
    uint32_t lo, hi;
    do {
        lo = ((volatile uint32_t *)p)[0];
        hi = ((volatile uint32_t *)p)[1];
        uint32_t nlo = lo + 1, nhi = hi + (nlo == 0);
        uint8_t ok;
        __asm__ volatile("lock cmpxchg8b %1; sete %0"
                         : "=q"(ok), "+m"(*p), "+a"(lo), "+d"(hi)
                         : "b"(nlo), "c"(nhi) : "memory", "cc");
        if (ok) return;
    } while (1);
}

static void *worker(void *arg)
{
    (void)arg;
    for (int i = 0; i < ITERS; i++)
        add1(word());
    return 0;
}

static int check(void)
{
    *word() = 0xfffff000u;       // the low half wraps partway through
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], 0, worker, 0);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], 0);
    uint64_t want = 0xfffff000u + (uint64_t)THREADS * ITERS;
    uint64_t got = *word();
    int bad = got != want;
    printf("cx8 mt: got 0x%llx want 0x%llx\n", (unsigned long long)got, (unsigned long long)want);
    printf(bad ? "== cx8 mt: MAL\n" : "== cx8 mt: ok\n");
    return bad;
}
void _start(void) { exit(check()); }
