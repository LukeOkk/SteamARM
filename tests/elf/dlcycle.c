// DLOPEN_CYCLE: dlopen/dlclose one library many times; the address space
// must stay flat. The Steam client's controller thread reloads libusb about
// four times a second, and every cycle used to leave 80 KiB of host address
// space behind, 48 KiB of it dirty (runtime/dispatch.c do_munmap_inner,
// runtime/offmap.c, runtime/rewrite.c):
//   - the last 16 KiB host page of a mapping the runtime placed with a length
//     that is not a multiple of it (ld.so.cache; glibc's PROT_NONE
//     reservation of the library's span): munmap kept it, because part of it
//     had never been the guest's;
//   - the RW segment's last host page, which also held the end of the code
//     mapping's last host page, past the guest's last byte;
//   - the trampoline pool of the library's code, never freed, until the
//     runtime's table of pools ran out after about a thousand loads.
// Each cycle also maps a file with an odd length the way ld.so maps
// ld.so.cache, and an anonymous mapping of an odd number of 4 KiB pages the
// way glibc's malloc maps a large block, and unmaps both; and it calls into
// the library each time (a raw syscall through its trampoline, its TLS, data
// and bss), so a pool or page freed too early, or a stale copy, shows.
//
// usage: dlcycle LIB CYCLES [wait]
// Counts the regions in /proc/self/maps (the host's, here) after a warm-up
// and after CYCLES more. With "wait" it also stops at both points until a
// line arrives on stdin, so the host can measure the process (tests/elf/
// run.sh: vmmap's region count and physical footprint).
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define WARMUP 50
#define ODD_FILE (5 * 4096 + 0x1fb)     // like ld.so.cache: a length with a tail
#define ODD_ANON (9 * 4096)             // not a multiple of 16 KiB

static int fails;
#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fflush(stdout); fails++; } } while (0)

// Regions, and those under 1 MiB: what a leaked page or pool is. The host's
// malloc adds and drops regions of 1 MiB and more on its own.
static void maps(long *lines, long *small)
{
    *lines = *small = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return;
    char buf[256];
    while (fgets(buf, sizeof buf, f)) {
        unsigned long lo, hi;
        if (sscanf(buf, "%lx-%lx", &lo, &hi) == 2) {
            (*lines)++;
            *small += hi - lo < (1ul << 20);
        }
    }
    fclose(f);
}

static void cycle(const char *lib, int fd, long i)
{
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        CHECK(0, "dlopen %s: %s", lib, dlerror());
        exit(1);
    }
    long (*gp)(void) = (long (*)(void))dlsym(h, "dlcycle_getpid");
    long (*touch)(long) = (long (*)(long))dlsym(h, "dlcycle_touch");
    if (!gp || !touch) {
        CHECK(0, "dlsym: %s", dlerror());
        exit(1);
    }
    long pid = gp();
    if (pid != getpid())
        CHECK(0, "cycle %ld: the library's raw getpid returned %ld, not %ld", i, pid, (long)getpid());
    long t = touch(3);
    if (t != 3 + 4 + 3)                     // fresh load: tls 0+3, data[3] 1+3, bss 0+3
        CHECK(0, "cycle %ld: the library's tls/data/bss read back %ld, not 10 "
              "(a stale copy from an earlier load?)", i, t);
    dlclose(h);

    unsigned char *p = mmap(NULL, ODD_FILE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED || p[ODD_FILE - 1] != (unsigned char)(ODD_FILE - 1))
        CHECK(0, "cycle %ld: odd-length file mapping", i);
    if (p != MAP_FAILED)
        munmap(p, ODD_FILE);
    unsigned char *a = mmap(NULL, ODD_ANON, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (a == MAP_FAILED || a[0] || a[ODD_ANON - 1])
        CHECK(0, "cycle %ld: odd-length anonymous mapping (fresh zeros)", i);
    if (a != MAP_FAILED) {
        memset(a, 0x5a, ODD_ANON);
        munmap(a, ODD_ANON);
    }
}

static void stop(const char *what, int wait)
{
    printf("== dlcycle: %s\n", what);
    if (wait) {
        char line[64];
        if (!fgets(line, sizeof line, stdin))
            CHECK(0, "no go-ahead on stdin after '%s'", what);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (argc < 3) {
        printf("usage: dlcycle LIB CYCLES [wait]\n");
        return 2;
    }
    long n = atol(argv[2]);
    int wait = argc > 3 && strcmp(argv[3], "wait") == 0;
    char path[] = "/tmp/lxrt-dlcycle-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "temporary file");
    if (fd < 0)
        return 1;
    unlink(path);
    static unsigned char buf[ODD_FILE];
    for (int i = 0; i < ODD_FILE; i++)
        buf[i] = (unsigned char)i;
    CHECK(pwrite(fd, buf, ODD_FILE, 0) == ODD_FILE, "write the odd-length file");

    printf("page size %d\n", getpagesize());
    for (long i = 0; i < WARMUP; i++)
        cycle(argv[1], fd, i);
    long l0, s0, l1, s1;
    maps(&l0, &s0);
    stop("warm", wait);
    for (long i = 0; i < n; i++)
        cycle(argv[1], fd, WARMUP + i);
    maps(&l1, &s1);
    printf("maps: %ld regions (%ld under 1 MiB) after %d cycles, %ld (%ld) after %ld more\n",
           l0, s0, WARMUP, l1, s1, n);
    // A cycle used to add five regions under 1 MiB. The host's malloc adds a
    // few of its own as it spreads over more CPUs.
    CHECK(s1 - s0 < 64 + n / 50, "%+ld regions under 1 MiB over %ld cycles", s1 - s0, n);
    stop("cycled", wait);
    printf("== dlcycle: %s (%ld cycles)\n", fails ? "FAIL" : "ok", n);
    return fails ? 1 : 0;
}
