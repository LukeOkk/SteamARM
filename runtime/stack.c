// The Linux process start state.
//
// glibc's _start does not take arguments: it reads argc, argv, envp and the
// auxiliary vector straight off the stack at the layout the kernel left them
// in. Getting this wrong does not produce a clean error, it produces a crash
// somewhere inside __libc_start_main, so the layout is spelled out here rather
// than assembled implicitly.

#include "lxrt.h"



#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

// LXRT_GUEST_PAGE=4096: tell a native guest Linux's 4 KiB page instead of the
// host's 16 KiB. Valve's arm64 Steam client ships 4 KiB-aligned libraries
// that glibc refuses to dlopen on a 16 KiB system ("ELF load command
// address/offset not page-aligned"); subpage.c already keeps 4 KiB mappings
// on 16 KiB pages for FEX's guests, and it serves these the same way.
uint64_t lxrt_guest_page(void)
{
    static uint64_t pg;
    if (!pg) {
        const char *e = getenv("LXRT_GUEST_PAGE");
        pg = e && strcmp(e, "4096") == 0 ? 4096 : LXRT_HOST_PAGE;
    }
    return pg;
}

// Auxiliary vector types, from Linux's elf.h.
enum {
    AT_NULL = 0, AT_IGNORE = 1, AT_EXECFD = 2, AT_PHDR = 3, AT_PHENT = 4,
    AT_PHNUM = 5, AT_PAGESZ = 6, AT_BASE = 7, AT_FLAGS = 8, AT_ENTRY = 9,
    AT_UID = 11, AT_EUID = 12, AT_GID = 13, AT_EGID = 14, AT_HWCAP = 16,
    AT_CLKTCK = 17, AT_SECURE = 23, AT_RANDOM = 25, AT_HWCAP2 = 26,
};

// HWCAP bits a Linux aarch64 userspace expects on any Apple Silicon-class
// core. Deliberately conservative: advertising something the hardware lacks
// makes glibc pick an ifunc that faults.
#define HWCAP_FP     (1u << 0)
#define HWCAP_ASIMD  (1u << 1)
#define HWCAP_AES    (1u << 3)
#define HWCAP_PMULL  (1u << 4)
#define HWCAP_SHA1   (1u << 5)
#define HWCAP_SHA2   (1u << 6)
#define HWCAP_CRC32  (1u << 7)
#define HWCAP_ATOMICS (1u << 8)
// HWCAP_CPUID (1u << 11) is deliberately ABSENT. On Linux it means "the kernel
// emulates mrs reads of the ID_AA64* / MIDR_EL1 registers". Darwin does not,
// and advertising it made glibc's start-up execute `mrs x0, midr_el1`
// (0xd5380000) and take SIGILL before its first syscall. Claiming a capability
// the runtime does not provide fails later and less clearly than not claiming
// it.
#define HWCAP_ASIMDRDM (1u << 12)

#define LXRT_STACK_SIZE (8ull << 20)

static int fail(char **err, const char *fmt, ...)
{
    if (err) {
        char *buf = malloc(512);
        if (buf) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(buf, 512, fmt, ap);
            va_end(ap);
        }
        *err = buf;
    }
    return -1;
}

void *lxrt_build_stack(const struct lxrt_image *img, int argc, char **argv,
                       char **envp, char **err)
{
    int envc = 0;
    while (envp && envp[envc])
        envc++;

    uint8_t *region = mmap(NULL, LXRT_STACK_SIZE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANON, -1, 0);
    if (region == MAP_FAILED) {
        fail(err, "guest stack: %s", strerror(errno));
        return NULL;
    }

    // Leave one mapped host page above the initial strings. Linux's initial
    // stack normally has mapped slack above argv/envp; a guest may read a few
    // bytes past an env string while comparing it. Placing the last string at
    // the very end of our mmap made that harmless read fault at the next page.
    // The padding stays anonymous and zero-filled, just like the rest of the
    // stack; argv/envp and auxv still have the kernel's usual layout.
    uint8_t *top = region + LXRT_STACK_SIZE - LXRT_HOST_PAGE;
    uint8_t *strp = top;

    uint64_t *argv_ptr = calloc((size_t)argc + 1, sizeof(uint64_t));
    uint64_t *envp_ptr = calloc((size_t)envc + 1, sizeof(uint64_t));
    if (!argv_ptr || !envp_ptr) {
        fail(err, "out of memory building the guest stack");
        return NULL;
    }

    // Linux's order, upward: argv strings, then env strings, contiguous
    // (fs/exec.c copies them top-down, env first). setproctitle-style code
    // that reuses the argv area for a title expects env to follow it; up to
    // 0.3.4 env sat below argv here (tests/elf/stack_bounds.c checks it).
    for (int i = envc - 1; i >= 0; i--) {
        size_t n = strlen(envp[i]) + 1;
        strp -= n;
        memcpy(strp, envp[i], n);
        envp_ptr[i] = (uint64_t)strp;
    }
    for (int i = argc - 1; i >= 0; i--) {
        size_t n = strlen(argv[i]) + 1;
        strp -= n;
        memcpy(strp, argv[i], n);
        argv_ptr[i] = (uint64_t)strp;
    }

    // AT_RANDOM points at 16 bytes the guest reads for its stack canary.
    strp -= 16;
    uint8_t *random16 = strp;
    arc4random_buf(random16, 16);

    struct { uint64_t type, val; } aux[] = {
        { AT_PHDR,   img->phdr },
        { AT_PHENT,  img->phentsize },
        { AT_PHNUM,  img->phnum },
        // The truth, not Linux's 4096: glibc uses this to align its own mmap
        // requests, and lying here produces EINVAL from Darwin later.
        { AT_PAGESZ, lxrt_guest_page() },
        // Where the dynamic loader was mapped. glibc's ld.so relocates itself
        // against this and will crash obscurely if it is wrong; 0 is correct
        // only for a static image.
        { AT_BASE,   img->interp_base },
        { AT_FLAGS,  0 },
        { AT_ENTRY,  img->entry },
        { AT_UID,    (uint64_t)getuid() },
        { AT_EUID,   (uint64_t)geteuid() },
        { AT_GID,    (uint64_t)getgid() },
        { AT_EGID,   (uint64_t)getegid() },
        { AT_HWCAP,  HWCAP_FP | HWCAP_ASIMD | HWCAP_AES | HWCAP_PMULL |
                     HWCAP_SHA1 | HWCAP_SHA2 | HWCAP_CRC32 | HWCAP_ATOMICS |
                     HWCAP_ASIMDRDM },
        { AT_HWCAP2, 0 },
        { AT_CLKTCK, 100 },
        { AT_SECURE, 0 },
        { AT_RANDOM, (uint64_t)random16 },
        // The runtime's vDSO (vdso_map.c), or AT_IGNORE when it is off.
        { lxrt_vdso_ehdr() ? 33 /* AT_SYSINFO_EHDR */ : AT_IGNORE, lxrt_vdso_ehdr() },
        { AT_NULL,   0 },
    };
    size_t naux = sizeof(aux) / sizeof(aux[0]);

    size_t words = 1                      // argc
                 + (size_t)argc + 1       // argv + NULL
                 + (size_t)envc + 1       // envp + NULL
                 + naux * 2;              // auxv

    uint64_t *sp = (uint64_t *)((uint64_t)(strp - words * 8) & ~15ull);
    if ((uint8_t *)sp < region) {
        fail(err, "guest stack too small for %zu argv/envp bytes", words * 8);
        return NULL;
    }

    uint64_t *p = sp;
    *p++ = (uint64_t)argc;
    for (int i = 0; i < argc; i++)
        *p++ = argv_ptr[i];
    *p++ = 0;
    for (int i = 0; i < envc; i++)
        *p++ = envp_ptr[i];
    *p++ = 0;
    for (size_t i = 0; i < naux; i++) {
        *p++ = aux[i].type;
        *p++ = aux[i].val;
    }

    free(argv_ptr);
    free(envp_ptr);
    return sp;
}
