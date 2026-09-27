// Guest-driven W^X: the mechanism FEX relies on, proven without FEX.
//
// 1. mmap an anonymous RWX region (the runtime turns that into MAP_JIT and
//    starts the thread in execute mode);
// 2. ask the runtime for write mode (private syscall 0x4C580020, enable=0),
//    emit code, ask for execute mode with the emitted range (enable=1);
// 3. run it.
//
// The emitted code deliberately contains a live `svc #0` with x8=getpid: on
// Darwin an unrewritten svc runs whatever Darwin syscall is in x16, so the
// only way this returns the guest's pid is if the runtime rescanned the range
// on the execute flip and rewrote the site. On a real Linux kernel the private
// syscall returns -ENOSYS, the region is plain RWX, and the same test passes
// for the boring reason -- which is the point of the probe.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static long lxrt_jit_wx(long enable, uint64_t addr, uint64_t len)
{
    register long x8 asm("x8") = 0x4C580020;
    register long x0 asm("x0") = enable;
    register uint64_t x1 asm("x1") = addr;
    register uint64_t x2 asm("x2") = len;
    asm volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
    return x0;
}

static int ok, bad;
#define CHECK(c, ...) do { if (c) { ok++; printf("  OK   " __VA_ARGS__); } \
                           else { bad++; printf("  MAL  " __VA_ARGS__); } putchar('\n'); } while (0)

int main(void)
{
    long probe = lxrt_jit_wx(1, 0, 0);
    int under_runtime = (probe == 0);
    printf("probe: %ld (%s)\n", probe, under_runtime ? "runtime" : "kernel Linux, -ENOSYS");

    uint32_t *code = mmap(NULL, 65536, PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(code != MAP_FAILED, "mmap RWX anonimo -> %p", (void *)code);
    if (code == MAP_FAILED) return 1;

    // Round 1: mov x0,#42; ret
    lxrt_jit_wx(0, 0, 0);
    code[0] = 0xd2800540; code[1] = 0xd65f03c0;
    __builtin___clear_cache((char *)code, (char *)(code + 2));
    lxrt_jit_wx(1, (uint64_t)code, 8);
    long (*fn)(void) = (long (*)(void))code;
    CHECK(fn() == 42, "ronda 1: codigo emitido devuelve %ld", fn());

    // Round 2: rewrite the same bytes -- mov x0,#7; ret
    lxrt_jit_wx(0, 0, 0);
    code[0] = 0xd28000e0;
    __builtin___clear_cache((char *)code, (char *)(code + 2));
    lxrt_jit_wx(1, (uint64_t)code, 8);
    CHECK(fn() == 7, "ronda 2: reescrito en sitio devuelve %ld", fn());

    // Round 3: JIT output that performs a syscall. mov x8,#172 (getpid);
    // svc #0; ret. Only a rescan-and-rewrite makes this the guest's pid.
    uint32_t *sc = code + 64;
    lxrt_jit_wx(0, 0, 0);
    sc[0] = 0xd2801588;   // mov x8, #172
    sc[1] = 0xd4000001;   // svc #0
    sc[2] = 0xd65f03c0;   // ret
    __builtin___clear_cache((char *)sc, (char *)(sc + 3));
    lxrt_jit_wx(1, (uint64_t)sc, 12);
    long (*pidfn)(void) = (long (*)(void))sc;
    long got = pidfn(), want = getpid();
    CHECK(got == want, "ronda 3: svc emitido por el JIT devuelve getpid()=%ld (esperado %ld)", got, want);

    // Round 4: nested-scope mode (enable=2) scans without closing the window.
    lxrt_jit_wx(0, 0, 0);
    sc[0] = 0xd2801588; sc[1] = 0xd4000001; sc[2] = 0xd65f03c0;
    __builtin___clear_cache((char *)sc, (char *)(sc + 3));
    long r2 = lxrt_jit_wx(2, (uint64_t)sc, 12);
    code[0] = 0xd28007c0;  // still writable: mov x0,#62
    __builtin___clear_cache((char *)code, (char *)(code + 2));
    lxrt_jit_wx(1, (uint64_t)code, 8);
    CHECK((r2 == 0 || !under_runtime) && fn() == 62 && pidfn() == want,
          "ronda 4: enable=2 escanea y sigue escribible (fn=%ld, pid ok=%d)", fn(), pidfn() == want);

    // Round 5: a 4 KiB guard page inside a 16 KiB host page. FEX's temporary
    // code buffer is mmap(12 KiB, rw) + mprotect(last 4 KiB, NONE); on 16 KiB
    // host pages the guard must not take its 8 KiB neighbours down with it.
    uint8_t *tb = mmap(NULL, 3 * 4096, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int guard_ok = 0;
    if (tb != MAP_FAILED && mprotect(tb + 2 * 4096, 4096, PROT_NONE) == 0) {
        tb[0] = 0x5a; tb[4096 + 7] = 0xa5;          // must not fault
        guard_ok = (tb[0] == 0x5a && tb[4096 + 7] == 0xa5);
    }
    CHECK(guard_ok, "ronda 5: guard de 4 KiB dentro de pagina de 16 KiB no oscurece a sus vecinos");

    printf("== %d ok, %d mal\n", ok, bad);
    return bad ? 1 : 0;
}
