// Do the vector registers, FPSR/FPCR and NZCV survive a signal handler?
// The handler clobbers them all; the interrupted code checks afterwards.
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static void clobber(int s) { (void)s;
    asm volatile("movi v0.16b, #0xff\n movi v7.16b, #0xff\n movi v8.16b, #0xff\n movi v15.16b, #0xff\n movi v16.16b, #0xff\n movi v31.16b, #0xff\n msr fpcr, xzr\n cmp xzr, xzr" ::: "v0","v7","v8","v15","v16","v31","memory","cc"); }
static int check(const char *what) {
    __uint128_t v[32]; uint64_t fpcr, nzcv; __uint128_t *vp = v;
    asm volatile("stp q0, q1, [%2, #0]\n stp q7, q8, [%2, #32]\n stp q15, q16, [%2, #64]\n stp q31, q31, [%2, #96]\n mrs %0, fpcr\n mrs %1, nzcv" : "=r"(fpcr), "=r"(nzcv) : "r"(vp) : "memory");
    int ok = 1;
    for (int i = 0; i < 8; i++) { uint64_t lo = (uint64_t)v[i], hi = (uint64_t)(v[i] >> 64); if (lo != 0x1111111111111111ull * (i + 1) || hi != 0x2222222222222222ull * (i + 1)) ok = 0; }
    if ((fpcr & 0x1000000) == 0) ok = 0;          // FZ bit we set
    if ((nzcv >> 28) != 0x8) ok = 0;              // N set by our cmp
    printf("  %s   %s\n", ok ? "OK " : "MAL", what); return ok; }
static void load(void) {
    __uint128_t v[8]; for (int i = 0; i < 8; i++) v[i] = ((__uint128_t)(0x2222222222222222ull * (i + 1)) << 64) | (0x1111111111111111ull * (i + 1));
    uint64_t fpcr = 0x1000000, one = 1, zero = 0;
    asm volatile("ldp q0, q1, [%0, #0]\n ldp q7, q8, [%0, #32]\n ldp q15, q16, [%0, #64]\n ldr q31, [%0, #96]\n msr fpcr, %1\n cmp %3, %2" :: "r"(v), "r"(fpcr), "r"(one), "r"(zero) : "v0","v1","v7","v8","v15","v16","v31","cc"); }
int main(void) {
    int ok = 0, n = 0;
    signal(SIGUSR1, clobber);
    load(); raise(SIGUSR1); n++; ok += check("vregs/fpcr/nzcv survive a synchronous signal");
    load(); kill(getpid(), SIGUSR1); n++; ok += check("...and a kill() to self");
    printf("== %d ok, %d mal\n", ok, n - ok); return ok == n ? 0 : 1; }
