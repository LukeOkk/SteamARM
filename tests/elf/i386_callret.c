// i386 probe: unbalanced call/ret. FEX keeps a per-thread call-ret stack with
// 4 KiB guard pages; running off either end faults in a guard and FEX resets
// its stack pointer. On 16 KiB host pages a 4 KiB guard shares its page with
// the stack itself and is not enforced, so the stack walked into neighbouring
// memory: another thread's call-ret stack, whose {guest, host} pairs then
// "matched" and sent a ret into stale host code (the Steam client, after a few
// minutes of lazy PLT binds -- _dl_runtime_resolve returns with `ret`).
// Shapes: many rets without calls (push; ret), many calls without rets
// (call; pop), both in two threads at once, then ordinary calls must work.
#include <stddef.h>
#include <stdint.h>
extern int printf(const char *, ...);
extern void exit(int) __attribute__((noreturn));
extern int pthread_create(unsigned long *, const void *, void *(*)(void *), void *);
extern int pthread_join(unsigned long, void **);

static int __attribute__((noinline)) leaf(int x) { return x * 3 + 1; }

static int churn(int rounds)
{
    int sum = 0;
    // 1. Rets with no call drift the call-ret stack upward, well past its
    //    3 MiB of headroom (196k entries).
    for (int i = 0; i < rounds * 1000; i++) {
        __asm__ volatile("pushl $1f\n\tret\n1:" ::: "memory");
        if (i % 1000 == 0) sum += leaf(i & 0xff) - leaf(i & 0xff);
    }
    // 2. Calls with no ret drift it downward past its 1 MiB (65k entries).
    for (int i = 0; i < rounds * 1000; i++) {
        __asm__ volatile("call 1f\n1:\n\tpopl %%eax" ::: "eax", "memory");
        if (i % 1000 == 0) sum += leaf(i & 0xff) - leaf(i & 0xff);
    }
    // Ordinary nested calls must still land where they should.
    for (int r = 0; r < rounds; r++)
        for (int i = 0; i < 100; i++)
            sum += leaf(i);
    return sum;
}

static void *worker(void *arg)
{
    int rounds = (int)(intptr_t)arg;
    return (void *)(intptr_t)churn(rounds);
}

static int check(void)
{
    // 400 rounds: 400k unmatched rets (the stack has 3 MiB = 196k entries of
    // headroom upward) and 400k unmatched calls (1 MiB = 65k downward).
    int want = 0;
    for (int i = 0; i < 100; i++) want += i * 3 + 1;
    want *= 400;
    unsigned long t1, t2;
    pthread_create(&t1, 0, worker, (void *)400);
    pthread_create(&t2, 0, worker, (void *)400);
    int mine = churn(400);
    void *r1, *r2;
    pthread_join(t1, &r1);
    pthread_join(t2, &r2);
    printf("sums %d %d %d want %d\n", mine, (int)(intptr_t)r1, (int)(intptr_t)r2, want);
    int ok = mine == want && (int)(intptr_t)r1 == want && (int)(intptr_t)r2 == want;
    printf(ok ? "== callret: ok\n" : "== callret: MAL\n");
    return !ok;
}
void _start(void) { exit(check()); }
