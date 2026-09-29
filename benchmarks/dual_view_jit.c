// Can a macOS process hold the "dual view" JIT code cache Android's ART uses?
//
// ART maps one memfd twice: read-execute for running code, and a separate
// never-executable view it makes writable to update code
// (art/runtime/jit/jit_memory_region.cc, docs/ANDROID_ZERO_VM_FEASIBILITY.md
// section 3.2). lxrun's memfd is an unlinked temporary file. This native
// probe (not a Linux guest) tries that shape on Darwin two ways:
//   1. an unlinked temporary file mapped read-write MAP_SHARED and
//      read-execute MAP_SHARED (and, for information, MAP_PRIVATE);
//   2. anonymous memory, aliased with mach_vm_remap, the alias made
//      read-execute and the original left read-write.
// For each working view pair it writes `mov w0, #N; ret` through the
// writable view, runs it through the executable one, rewrites it and runs
// it again.
//
// Build and run (benchmarks/README.md; stage25-android-research.txt):
//   clang -O1 -Wall -o build/dual_view_jit benchmarks/dual_view_jit.c
//   ./build/dual_view_jit
//   codesign -f -s - --entitlements resources/lxrt.entitlements build/dual_view_jit
//   ./build/dual_view_jit        # signed as build/lxrun is
#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static sigjmp_buf jb;
static void on_sig(int s) { siglongjmp(jb, s); }

static int run(void *rx, int expect, const char *what)
{
    int (*fn)(void) = (int (*)(void))rx;
    int s = sigsetjmp(jb, 1);
    if (s) {
        printf("%-44s signal %d (%s)\n", what, s, strsignal(s));
        return 1;
    }
    int r = fn();
    printf("%-44s returned %d (%s)\n", what, r, r == expect ? "ok" : "WRONG");
    return r != expect;
}

static void emit(uint32_t *w, int value)
{
    w[0] = 0x52800000u | ((uint32_t)value << 5); // movz w0, #value
    w[1] = 0xd65f03c0u;                          // ret
}

int main(void)
{
    signal(SIGBUS, on_sig);
    signal(SIGSEGV, on_sig);
    signal(SIGILL, on_sig);
    const size_t len = 16384;
    int file_fails = 0, anon_fails = 0;

    // 1. An unlinked temporary file, as lxrun's memfd is.
    char path[] = "/tmp/dual_view_jit.XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); return 2; }
    unlink(path);
    if (ftruncate(fd, (off_t)len) != 0) { perror("ftruncate"); return 2; }
    void *rw = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    void *rx = mmap(NULL, len, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0);
    printf("file: RW MAP_SHARED view %s, RX MAP_SHARED view %s\n",
           rw == MAP_FAILED ? strerror(errno) : "mapped",
           rx == MAP_FAILED ? strerror(errno) : "mapped");
    if (rw != MAP_FAILED && rx != MAP_FAILED) {
        emit(rw, 42);
        sys_icache_invalidate(rx, 8);
        file_fails += run(rx, 42, "file: written via RW view, run via RX");
        emit(rw, 7);
        sys_icache_invalidate(rx, 8);
        file_fails += run(rx, 7, "file: rewritten via RW view, run again");
    } else {
        file_fails++;
    }
    void *px = mmap(NULL, len, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
    printf("file: RX MAP_PRIVATE mapping %s (information only)\n",
           px == MAP_FAILED ? strerror(errno) : "mapped");

    // 2. Anonymous memory and a mach_vm_remap alias of it.
    mach_vm_address_t a = 0, b = 0;
    vm_prot_t cur = 0, max = 0;
    kern_return_t kr = mach_vm_allocate(mach_task_self(), &a, len, VM_FLAGS_ANYWHERE);
    if (kr == KERN_SUCCESS)
        kr = mach_vm_remap(mach_task_self(), &b, len, 0, VM_FLAGS_ANYWHERE,
                           mach_task_self(), a, FALSE, &cur, &max, VM_INHERIT_NONE);
    printf("anon: mach_vm_remap %s (cur %d, max %d)\n", mach_error_string(kr), cur, max);
    if (kr == KERN_SUCCESS) {
        int pr = mprotect((void *)b, len, PROT_READ | PROT_EXEC);
        printf("anon: alias made RX: %s\n", pr ? strerror(errno) : "ok");
        if (!pr) {
            emit((uint32_t *)a, 99);
            sys_icache_invalidate((void *)b, 8);
            anon_fails += run((void *)b, 99, "anon: written via RW, run via RX alias");
            emit((uint32_t *)a, 5);
            sys_icache_invalidate((void *)b, 8);
            anon_fails += run((void *)b, 5, "anon: rewritten via RW, run again");
        } else {
            anon_fails++;
        }
    } else {
        anon_fails++;
    }

    printf("RESULT file dual view %s; anonymous alias dual view %s\n",
           file_fails ? "NOT AVAILABLE" : "works", anon_fails ? "NOT AVAILABLE" : "works");
    return anon_fails ? 1 : 0;
}
