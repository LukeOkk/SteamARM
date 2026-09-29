// What bionic (Android 11's libc and linker) needed from the runtime, stage 25
// (benchmarks/stage25-android-userspace.txt). Each check prints "ok" or "MAL";
// the last line is "== android bionic runtime: ok" when all pass.
//
//   tagged   prctl(PR_SET_TAGGED_ADDR_CTRL) is EINVAL: the runtime does not
//            untag pointers in syscalls, and bionic tags its heap when the
//            call succeeds (mksh's read(2) into a 0xb4... buffer got EFAULT)
//   msync    mapped -> 0, unmapped -> ENOMEM, the low 4 GiB (__PAGEZERO)
//            -> 0 (reserved), a misaligned address -> EINVAL; ART probes
//            free address space with msync
//   mremap   a 4 KiB-aligned MREMAP_FIXED move into a reservation, as
//            bionic's CFI shadow does (ShadowWrite): the bytes arrive, the
//            neighbours keep theirs, the source is gone; a 4 KiB-aligned
//            shrink and move without FIXED
//   clone    a thread cloned without CLONE_SETTLS starts with its parent's
//            thread pointer (bionic's crash handler relies on it), and one
//            without CLONE_FILES is refused with EINVAL (a Darwin thread
//            cannot have its own descriptor table)
//   sigqueue rt_tgsigqueueinfo to this thread runs the handler (bionic's
//            abort raises SIGABRT that way)
//   tlskeep  (argument "tlskeep") a TPIDR_EL0 read inside
//            [BORINGSSL_bcm_text_start, BORINGSSL_bcm_text_end) is left as
//            it is (the word is still `mrs x9, tpidr_el0`), and loads
//            through it return the same as a rewritten read, across sleeps,
//            yields and threads (runtime/tls.c, kept TLS reads)
//
// Built by tests/elf/run.sh as a glibc static-pie with --export-dynamic (the
// two symbols must be in .dynsym, as they are in Android's libcrypto.so).
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <signal.h>
#include <linux/futex.h>
#include <unistd.h>

static int bad;
static void check(int cond, const char *what)
{
    printf("  %s  %s\n", cond ? "ok " : "MAL", what);
    if (!cond)
        bad++;
}

// The kept region. A load through the thread pointer read here, and the same
// load through a read the runtime rewrote (outside the region), must agree.
__asm__(
    ".text\n"
    ".p2align 4\n"
    ".globl BORINGSSL_bcm_text_start\n"
    ".type BORINGSSL_bcm_text_start, %function\n"
    "BORINGSSL_bcm_text_start:\n"
    ".globl kept_tp_load\n"
    ".type kept_tp_load, %function\n"
    "kept_tp_load:\n"
    "    mrs x9, tpidr_el0\n"
    "    ldr x0, [x9, x0]\n"
    "    ret\n"
    ".size kept_tp_load, . - kept_tp_load\n"
    ".globl BORINGSSL_bcm_text_end\n"
    ".type BORINGSSL_bcm_text_end, %function\n"
    "BORINGSSL_bcm_text_end:\n"
    "    ret\n"
    ".size BORINGSSL_bcm_text_end, 4\n"
    ".globl rewritten_tp_load\n"
    ".type rewritten_tp_load, %function\n"
    "rewritten_tp_load:\n"
    "    mrs x9, tpidr_el0\n"
    "    ldr x0, [x9, x0]\n"
    "    ret\n"
    ".size rewritten_tp_load, . - rewritten_tp_load\n");
uint64_t kept_tp_load(uint64_t off);
uint64_t rewritten_tp_load(uint64_t off);
extern const uint32_t BORINGSSL_bcm_text_start[];

static _Atomic long mismatches;
static void *hammer(void *arg)
{
    long n = (long)(intptr_t)arg;
    for (long i = 0; i < n; i++) {
        if (kept_tp_load(0) != rewritten_tp_load(0))
            mismatches++;
        if (i % 1000 == 0)
            usleep(50);             // a context switch: Darwin replaces TPIDR_EL0
        else if (i % 97 == 0)
            sched_yield();
    }
    return NULL;
}

static int tlskeep(void)
{
    uint32_t w = BORINGSSL_bcm_text_start[0];
    printf("  word at BORINGSSL_bcm_text_start: 0x%08x\n", w);
    check(w == 0xd53bd049u, "the TPIDR_EL0 read in the kept range is untouched (mrs x9, tpidr_el0)");
    check(kept_tp_load(0) == rewritten_tp_load(0), "kept and rewritten reads load the same TCB word");
    pthread_t th[4];
    for (int i = 0; i < 4; i++)
        pthread_create(&th[i], NULL, hammer, (void *)(intptr_t)200000);
    hammer((void *)(intptr_t)200000);
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], NULL);
    char msg[160];
    snprintf(msg, sizeof msg, "5 threads x 200000 loads across usleep/sched_yield: %ld mismatches",
             (long)mismatches);
    check(mismatches == 0, msg);
    return bad;
}

static volatile uint64_t child_tp;
static int clone_child(void *arg)
{
    (void)arg;
    child_tp = (uint64_t)__builtin_thread_pointer();
    return 0;
}

static volatile int got_usr1;
static void on_usr1(int sig, siginfo_t *si, void *uc)
{
    (void)sig; (void)si; (void)uc;
    got_usr1 = 1;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);       // every line out before a crash
    if (argc > 1 && !strcmp(argv[1], "tlskeep")) {
        int r = tlskeep();
        printf(r ? "== android tlskeep: FAIL\n" : "== android tlskeep: ok\n");
        return r ? 1 : 0;
    }

    // tagged
    errno = 0;
    int r = prctl(55 /* PR_SET_TAGGED_ADDR_CTRL */, 1, 0, 0, 0);
    check(r == -1 && errno == EINVAL, "prctl(PR_SET_TAGGED_ADDR_CTRL, ENABLE) is EINVAL (no tagged address ABI)");
    errno = 0;
    r = prctl(56 /* PR_GET_TAGGED_ADDR_CTRL */, 0, 0, 0, 0);
    check(r == -1 && errno == EINVAL, "prctl(PR_GET_TAGGED_ADDR_CTRL) is EINVAL");

    // msync
    const size_t pg = 4096;
    uint8_t *m = mmap(NULL, 16 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(m != MAP_FAILED, "mmap 64 KiB");
    check(msync(m, pg, 0) == 0, "msync of a mapped page, flags 0 -> 0");
    check(msync(m, pg, MS_ASYNC) == 0, "msync MS_ASYNC -> 0");
    errno = 0;
    check(msync(m + 1, pg, 0) == -1 && errno == EINVAL, "msync of a misaligned address -> EINVAL");
    errno = 0;
    check(msync(m, pg, MS_ASYNC | MS_SYNC) == -1 && errno == EINVAL, "msync MS_ASYNC|MS_SYNC -> EINVAL");
    check(msync((void *)0x70000000, pg, 0) == 0, "msync in the low 4 GiB (Darwin's __PAGEZERO) -> 0, reserved");
    uint8_t *gone = mmap(NULL, 16 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    munmap(gone, 16 * pg);
    errno = 0;
    check(msync(gone, pg, 0) == -1 && errno == ENOMEM, "msync of an unmapped page -> ENOMEM");

    // mremap, the CFI shadow's way: a reservation, a private copy, a move
    // over a 4 KiB-aligned slice of it.
    uint8_t *shadow = mmap(NULL, 64 * pg, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    uint8_t *tmp = mmap(NULL, 4 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(shadow != MAP_FAILED && tmp != MAP_FAILED, "reserve 256 KiB read-only, map a 16 KiB copy");
    for (size_t i = 0; i < 4 * pg; i++)
        tmp[i] = (uint8_t)(i * 7 + 3);
    uint8_t *dst = shadow + 5 * pg;         // 4 KiB-aligned, not 16 KiB-aligned
    void *got = mremap(tmp, 4 * pg, 4 * pg, MREMAP_MAYMOVE | MREMAP_FIXED, dst);
    check(got == dst, "mremap(MAYMOVE|FIXED) onto shadow+0x5000 returns that address");
    int same = got == dst;
    for (size_t i = 0; same && i < 4 * pg; i++)
        same = dst[i] == (uint8_t)(i * 7 + 3);
    check(same, "the moved 16 KiB hold the copy's bytes");
    int zero = 1;
    for (size_t i = 0; i < pg; i++)
        zero &= shadow[4 * pg + i] == 0 && shadow[9 * pg + i] == 0;
    check(zero, "the reservation's pages on either side are untouched (zero)");
    dst[0] = 0x5a;                          // the copy's protection: read-write
    check(dst[0] == 0x5a, "the moved pages keep the source's read-write protection");
    errno = 0;
    check(msync(tmp, pg, 0) == -1 && errno == ENOMEM, "the source range is unmapped after the move");

    // Shrink in place, and move without FIXED, on 4 KiB-aligned ranges.
    uint8_t *s4 = shadow + 20 * pg;
    void *p = mremap(dst, 4 * pg, 2 * pg, 0);
    check(p == dst, "mremap shrink of a 4 KiB-aligned range stays in place");
    p = mremap(dst, 2 * pg, 6 * pg, MREMAP_MAYMOVE);
    check(p != MAP_FAILED && ((uint8_t *)p)[1] == (uint8_t)(1 * 7 + 3) && ((uint8_t *)p)[0] == 0x5a,
          "mremap grow with MAYMOVE moves the bytes");
    (void)s4;

    // clone: no CLONE_SETTLS -> the parent's thread pointer; no CLONE_FILES
    // -> refused.
    static char cstack[64 * 1024] __attribute__((aligned(16)));
    pid_t ctid = 0;
    int cflags = CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
                 CLONE_SYSVSEM | CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID;
    int ct = clone(clone_child, cstack + sizeof cstack, cflags, NULL, NULL, NULL, &ctid);
    if (ct > 0)
        while (__atomic_load_n(&ctid, __ATOMIC_ACQUIRE) != 0)
            syscall(SYS_futex, &ctid, FUTEX_WAIT, ct, NULL, NULL, 0);
    check(ct > 0 && child_tp == (uint64_t)__builtin_thread_pointer(),
          "clone without CLONE_SETTLS: the thread starts with its parent's thread pointer");
    errno = 0;
    ct = clone(clone_child, cstack + sizeof cstack, cflags & ~CLONE_FILES, NULL, NULL, NULL, &ctid);
    check(ct == -1 && errno == EINVAL, "clone of a thread without CLONE_FILES -> EINVAL (no per-thread descriptor table)");

    // rt_tgsigqueueinfo to ourselves.
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_usr1;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGUSR1, &sa, NULL);
    siginfo_t qi;
    memset(&qi, 0, sizeof qi);
    qi.si_signo = SIGUSR1;
    qi.si_code = SI_QUEUE;
    qi.si_pid = getpid();
    long qr = syscall(SYS_rt_tgsigqueueinfo, getpid(), (pid_t)syscall(SYS_gettid), SIGUSR1, &qi);
    for (int i = 0; i < 100 && !got_usr1; i++)
        usleep(1000);
    check(qr == 0 && got_usr1, "rt_tgsigqueueinfo(self, SIGUSR1) runs the handler");

    printf(bad ? "== android bionic runtime: FAIL (%d)\n" : "== android bionic runtime: ok\n", bad);
    return bad ? 1 : 0;
}
