// X18_JUMP_TABLE: a jump table dispatched through x18 keeps x16 and x17.
//
// V8's TurboFan (Electron 43, Heroic) keeps w16 live across
//     adr x18, table ; ldrb w0, [x5, x16] ; add x18, x18, x0, lsl #2 ; br x18
// and reads it again at the target. The runtime's `br x18` trampoline used to
// carry the target in x16 (fine for blr and ret, where x16 is dead by the
// procedure call standard, wrong here): TurboFan built broken graphs and hit
// a CHECK in CFGBuilder::ConnectBlocks (benchmarks/stage28-keep-x18.txt).
//
// THREADS threads (more than the Mac has cores, so they are preempted all the
// time) each run ITERS dispatches with x16 and x17 live across the branch and
// checked at the target; the table's four cases add different amounts, so the
// sum says every case ran as often as it should. Where the kernel zeroes x18,
// the trampoline's last instructions are hit by those exceptions too; the
// runtime restarts or resumes it (runtime/signal.c, "br x18"), and
// LXRT_X18_STATS=1 prints how often that happened.
//
// Then the two recoveries, forced: generated code (never rewritten) zeroes
// the hardware x18 and jumps into the trampoline of a rewritten `br x18`,
// once at its `and` (the `ldr x18, [x18, #slot]` then faults: the runtime
// restarts the trampoline) and once at its `br` (a branch to 0: the runtime
// resumes at the target the trampoline marked below sp). Each must arrive at
// the target the virtual x18 holds.
//
// Exit 0 and "== x18_jumptable: ok" when every check held.
#define _GNU_SOURCE
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

// A `br x18` the rewriter turns into a branch to its trampoline; never run
// here. Inside an FDE (.cfi_startproc), so the x18 pass sees it.
__asm__(".text\n.p2align 2\n.type x18_brsite_fn, %function\nx18_brsite_fn:\n.cfi_startproc\n"
        "  ret\n.global x18_brsite\nx18_brsite:\n  br x18\n.cfi_endproc\n"
        ".size x18_brsite_fn, .-x18_brsite_fn\n");
extern const uint32_t x18_brsite[];

// Enter the trampoline of x18_brsite at word `word` with the hardware x18 = 0
// and the virtual x18 = a label; 1 when control arrived at the label.
static int forced(const uint32_t *jit, unsigned word)
{
    uint32_t w = x18_brsite[0];
    if ((w & 0xfc000000u) != 0x14000000u)
        return -1;                                     // not rewritten
    int64_t off = (int64_t)((int32_t)(w << 6) >> 6) * 4;
    uint64_t entry = (uint64_t)(uintptr_t)x18_brsite + (uint64_t)off + 4u * word;
    uint64_t hit = 0;
    __asm__ volatile(
        "adr  x18, 5f\n"                 // the virtual x18: the target
        "adr  x10, 5f\n"
        "add  x9, %[entry], #0\n"
        "stp  x9, x10, [sp, #-32]\n"      // what the trampoline's words 0-8 leave
        "br   %[jit]\n"                   // mov x18, xzr ; br x9
        "5:   mov %[hit], #1\n"
        : [hit] "+r"(hit)
        : [entry] "r"(entry), [jit] "r"(jit)
        : "x9", "x10", "x16", "x17", "x18", "memory");
    return (int)hit;
}

#define THREADS 16
#define ITERS 3000000UL

static unsigned long bad[THREADS], sum[THREADS];

static void *worker(void *arg)
{
    int id = (int)(uintptr_t)arg;
    unsigned long errors = 0, acc = 0, n = ITERS + (unsigned long)id;
    __asm__ volatile(
        "1: and  x0, %[n], #3\n"
        "   adr  x18, 2f\n"
        "   add  x18, x18, x0, lsl #3\n"   // each case is two instructions
        "   mov  x16, %[n]\n"              // live across the branch
        "   eor  x17, %[n], #0xff\n"       // this one too
        "   br   x18\n"
        "2: add  %[acc], %[acc], #1\n"
        "   b    3f\n"
        "   add  %[acc], %[acc], #10\n"
        "   b    3f\n"
        "   add  %[acc], %[acc], #100\n"
        "   b    3f\n"
        "   add  %[acc], %[acc], #1000\n"
        "   b    3f\n"
        "3: cmp  x16, %[n]\n"
        "   cinc %[err], %[err], ne\n"
        "   eor  x0, %[n], #0xff\n"
        "   cmp  x17, x0\n"
        "   cinc %[err], %[err], ne\n"
        "   subs %[n], %[n], #1\n"
        "   b.ne 1b\n"
        : [err] "+r"(errors), [acc] "+r"(acc), [n] "+r"(n)
        :
        : "x0", "x16", "x17", "x18", "cc", "memory");
    bad[id] = errors;
    sum[id] = acc;
    return NULL;
}

// Cases hit by n, n-1, ..., 1: case k = n & 3, counted per residue.
static unsigned long expected(unsigned long n)
{
    static const unsigned long add[4] = {1, 10, 100, 1000};
    unsigned long s = 0;
    for (unsigned k = 0; k < 4; k++) {
        unsigned long c = n / 4 + (k && k <= n % 4 ? 1 : 0);   // values in 1..n with v % 4 == k
        s += c * add[k];
    }
    return s;
}

int main(void)
{
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, worker, (void *)(uintptr_t)i);
    int fails = 0;
    for (int i = 0; i < THREADS; i++) {
        pthread_join(t[i], NULL);
        unsigned long want = expected(ITERS + (unsigned long)i);
        if (bad[i] || sum[i] != want) {
            printf("  thread %d: %lu x16/x17 mismatches, sum %lu (want %lu)\n", i, bad[i], sum[i], want);
            fails++;
        }
    }
    uint32_t *jit = mmap(NULL, 16384, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (jit == MAP_FAILED) { perror("mmap"); return 2; }
    jit[0] = 0xaa1f03f2;   // mov x18, xzr
    jit[1] = 0xd61f0120;   // br x9
    __builtin___clear_cache((char *)jit, (char *)(jit + 2));
    int r1 = forced(jit, 10), r2 = forced(jit, 12);
    printf("  x18 zeroed before the trampoline's ldr: %s\n", r1 == 1 ? "restarted, arrived" : "FAIL");
    printf("  x18 zeroed before the trampoline's br:  %s\n", r2 == 1 ? "branch to 0 recovered, arrived" : "FAIL");
    if (r1 != 1 || r2 != 1) fails++;
    printf("== x18_jumptable: %s (%d threads x %lu dispatches)\n", fails ? "FAIL" : "ok", THREADS, ITERS);
    return fails ? 1 : 0;
}
