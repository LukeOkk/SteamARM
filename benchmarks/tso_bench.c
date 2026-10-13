// What FEX's x86 memory-ordering (TSO) emulation costs. An x86-64 Linux
// program (glibc, pthreads; built by benchmarks/tso_bench.sh against the
// thunk build's x86-64 sysroot) run through FEX under the runtime with
// scripts/run-fex.sh under each profile of Procesador -> Orden de memoria:
// FEX_TSOENABLED=0, FEX's defaults, and FEX_VECTORTSOENABLED=1
// FEX_MEMCPYSETTSOENABLED=1. No Proton, no window.
//
// Every phase prints nanoseconds per operation (what one operation is, below).
// FEX changes only how it translates loads and stores, so the phases are
// sorted by what they do to memory:
//   regs      xorshift64 kept in registers, no memory: TSO cannot cost here
//   scalar    one 8-byte load and one store per element, scalar code
//   vector    one 16-byte SSE2 load and one store per element (VectorTSOEnabled)
//   memcpy    glibc memcpy of 256 KiB (its SSE/AVX moves), per 64 bytes
//   repmovs   rep movsb of 256 KiB (MemcpySetTSOEnabled), per 64 bytes
//   atomic    4 threads, lock xadd on one shared counter (contended), per op
//   atomic1   4 threads, lock xadd each on its own cache line, per op
//   ring      2 producer/consumer pairs (4 threads) over 4096-slot rings:
//             plain stores publish, plain loads consume, as x86 lock-free
//             code does. The consumer checks the sequence; "violations" are
//             items read before the data store became visible, which x86
//             ordering forbids and ARM's does not.
// The whole run prints "== tso_bench: ok" or "== tso_bench: N violations".
// An optional argument scales every count (0.25 for a quick look).
#include <emmintrin.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ARRAY_BYTES (256u << 10)      // in L2: the instruction cost, not DRAM
#define ARRAY_U64 (ARRAY_BYTES / 8)
#define ARRAY_V128 (ARRAY_BYTES / 16)
#define LINE 64
#define RING_SLOTS 4096
#define THREADS 4

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
#define BARRIER() __asm__ volatile("" ::: "memory")
static void report(const char *phase, uint64_t ns, uint64_t ops, const char *unit) {
    printf("%-8s %8.2f ns/op   (%llu %s in %.1f ms)\n", phase, (double)ns / (double)ops,
           (unsigned long long)ops, unit, (double)ns / 1e6);
}

// ---------------------------------------------------------------- one thread
static uint64_t sink;

__attribute__((noinline)) static uint64_t regs(uint64_t n) {
    uint64_t x = 88172645463325252ull;
    for (uint64_t i = 0; i < n; i++) { x ^= x << 13; x ^= x >> 7; x ^= x << 17; }
    return x;
}

__attribute__((noinline)) static void scalar(uint64_t *a, uint64_t reps) {
    for (uint64_t r = 0; r < reps; r++) {
#pragma clang loop vectorize(disable) interleave(disable)
        for (uint64_t i = 0; i < ARRAY_U64; i++) a[i] = a[i] * 3 + 1;
    }
}

__attribute__((noinline)) static void vector(__m128i *a, uint64_t reps) {
    const __m128i k = _mm_set1_epi32(7);
    for (uint64_t r = 0; r < reps; r++)
        for (uint64_t i = 0; i < ARRAY_V128; i++)
            _mm_storeu_si128(a + i, _mm_add_epi32(_mm_loadu_si128(a + i), k));
}

__attribute__((noinline)) static void copy_memcpy(void *d, const void *s, uint64_t reps) {
    for (uint64_t r = 0; r < reps; r++) { memcpy(d, s, ARRAY_BYTES); BARRIER(); }
}

__attribute__((noinline)) static void copy_repmovs(void *d, const void *s, uint64_t reps) {
    for (uint64_t r = 0; r < reps; r++) {
        void *dd = d; const void *ss = s; uint64_t n = ARRAY_BYTES;
        __asm__ volatile("rep movsb" : "+D"(dd), "+S"(ss), "+c"(n) : : "memory");
    }
}

// ---------------------------------------------------------------- 4 threads
struct padded { volatile uint64_t v; char pad[LINE - 8]; } __attribute__((aligned(LINE)));
static struct padded shared_counter;
static struct padded own_counter[THREADS];
static uint64_t atomic_ops;

static void *atomic_shared(void *arg) {
    (void)arg;
    for (uint64_t i = 0; i < atomic_ops; i++) __atomic_fetch_add(&shared_counter.v, 1, __ATOMIC_SEQ_CST);
    return 0;
}
static void *atomic_own(void *arg) {
    volatile uint64_t *c = &own_counter[(long)arg].v;
    for (uint64_t i = 0; i < atomic_ops; i++) __atomic_fetch_add(c, 1, __ATOMIC_SEQ_CST);
    return 0;
}

struct ring {
    uint64_t slot[RING_SLOTS];
    struct padded head;           // written by the producer, read by the consumer
    struct padded tail;           // written by the consumer, read by the producer
    uint64_t items, violations;
};
static struct ring rings[THREADS / 2] __attribute__((aligned(LINE)));

static void *producer(void *arg) {
    struct ring *r = arg;
    for (uint64_t seq = 1; seq <= r->items; seq++) {
        while (r->head.v - r->tail.v == RING_SLOTS) _mm_pause();
        uint64_t h = r->head.v;
        r->slot[h & (RING_SLOTS - 1)] = seq;      // data, then publish: x86 keeps the order
        BARRIER();
        r->head.v = h + 1;
    }
    return 0;
}
static void *consumer(void *arg) {
    struct ring *r = arg;
    uint64_t bad = 0;
    for (uint64_t seq = 1; seq <= r->items; seq++) {
        uint64_t t = r->tail.v;
        while (r->head.v == t) _mm_pause();
        BARRIER();
        uint64_t v = r->slot[t & (RING_SLOTS - 1)];
        if (v != seq) bad++;
        BARRIER();
        r->tail.v = t + 1;
    }
    r->violations = bad;
    return 0;
}

static uint64_t run4(void *(*fn[THREADS])(void *), void *arg[THREADS]) {
    pthread_t th[THREADS];
    uint64_t t0 = now_ns();
    for (int i = 0; i < THREADS; i++) pthread_create(&th[i], 0, fn[i], arg[i]);
    for (int i = 0; i < THREADS; i++) pthread_join(th[i], 0);
    return now_ns() - t0;
}

int main(int argc, char **argv) {
    double scale = argc > 1 ? atof(argv[1]) : 1.0;          // shorter or longer runs
    const uint64_t regs_n = (uint64_t)(128e6 * scale), scalar_reps = (uint64_t)(32768 * scale);
    const uint64_t vector_reps = (uint64_t)(65536 * scale), copy_reps = (uint64_t)(32768 * scale);
    const uint64_t ring_items = (uint64_t)(16e6 * scale);
    atomic_ops = (uint64_t)(8e6 * scale);
    uint64_t *a = aligned_alloc(LINE, ARRAY_BYTES), *b = aligned_alloc(LINE, ARRAY_BYTES);
    for (uint64_t i = 0; i < ARRAY_U64; i++) { a[i] = i; b[i] = ~i; }
    uint64_t t, ns;

    t = now_ns(); sink = regs(regs_n); ns = now_ns() - t;
    report("regs", ns, regs_n, "iterations");
    t = now_ns(); scalar(a, scalar_reps); ns = now_ns() - t;
    report("scalar", ns, scalar_reps * ARRAY_U64, "load+store of 8 bytes");
    t = now_ns(); vector((__m128i *)a, vector_reps); ns = now_ns() - t;
    report("vector", ns, vector_reps * ARRAY_V128, "load+store of 16 bytes");
    t = now_ns(); copy_memcpy(b, a, copy_reps); ns = now_ns() - t;
    report("memcpy", ns, copy_reps * ARRAY_BYTES / LINE, "x 64 bytes copied");
    t = now_ns(); copy_repmovs(b, a, copy_reps); ns = now_ns() - t;
    report("repmovs", ns, copy_reps * ARRAY_BYTES / LINE, "x 64 bytes copied");
    sink += a[ARRAY_U64 - 1] + b[ARRAY_U64 - 1];

    void *(*fn[THREADS])(void *); void *arg[THREADS];
    for (int i = 0; i < THREADS; i++) { fn[i] = atomic_shared; arg[i] = 0; }
    ns = run4(fn, arg);
    if (shared_counter.v != atomic_ops * THREADS) printf("atomic: counter %llu, expected %llu\n",
        (unsigned long long)shared_counter.v, (unsigned long long)(atomic_ops * THREADS));
    report("atomic", ns, atomic_ops, "lock xadd per thread, 4 threads on one line");
    for (int i = 0; i < THREADS; i++) { fn[i] = atomic_own; arg[i] = (void *)(long)i; }
    ns = run4(fn, arg);
    report("atomic1", ns, atomic_ops, "lock xadd per thread, each its own line");

    uint64_t violations = 0;
    for (int p = 0; p < THREADS / 2; p++) {
        memset(&rings[p], 0, sizeof rings[p]);
        rings[p].items = ring_items;
        fn[2 * p] = producer; arg[2 * p] = &rings[p];
        fn[2 * p + 1] = consumer; arg[2 * p + 1] = &rings[p];
    }
    ns = run4(fn, arg);
    for (int p = 0; p < THREADS / 2; p++) violations += rings[p].violations;
    report("ring", ns, ring_items, "items per pair, 2 producer/consumer pairs");
    printf("ring violations: %llu of %llu items\n", (unsigned long long)violations,
           (unsigned long long)(ring_items * (THREADS / 2)));
    if (sink == 42) printf("(%llu)\n", (unsigned long long)sink);
    if (violations) printf("== tso_bench: %llu violations\n", (unsigned long long)violations);
    else printf("== tso_bench: ok\n");
    return 0;
}
