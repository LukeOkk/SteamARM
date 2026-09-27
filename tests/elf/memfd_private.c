// A MAP_PRIVATE mapping of a memfd keeps showing the file's updates on every
// page the process has not written, as on Linux; a store makes that page (and
// only that page) private. Darwin alone copies at mmap time -- wine's
// read-only views of wineserver's session section saw stale window-class ids
// and every process started another explorer.exe (runtime/privmap.c).
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void)
{
    int ok = 1;
    const size_t sz = 0x10000;
    int fd = memfd_create("privtest", 0);
    if (fd < 0 || ftruncate(fd, sz) != 0) { perror("memfd"); return 1; }
    volatile int *sh = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    // Wine's way: private, read-write, then made read-only.
    volatile int *ro = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    mprotect((void *)ro, sz, PROT_READ);
    volatile int *pr = mmap(0, sz, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (sh == MAP_FAILED || ro == MAP_FAILED || pr == MAP_FAILED) { perror("mmap"); return 1; }

    sh[0] = 11; sh[0x2000] = 12;               // two different 16 KiB pages
    printf("read-only private sees %d %d (want 11 12)\n", ro[0], ro[0x2000]);
    if (ro[0] != 11 || ro[0x2000] != 12) ok = 0;
    int v = 13; pwrite(fd, &v, sizeof v, 0);
    printf("after pwrite: %d (want 13)\n", ro[0]);
    if (ro[0] != 13) ok = 0;

    pr[0] = 99;                                // copy on write, first page only
    sh[0] = 21; sh[0x2000] = 22;
    printf("written private: own %d, untouched page %d (want 99 22); shared %d (want 21)\n",
           pr[0], pr[0x2000], sh[0]);
    if (pr[0] != 99 || pr[0x2000] != 22 || sh[0] != 21) ok = 0;
    printf(ok ? "== memfd private: ok\n" : "== memfd private: FAIL\n");
    return !ok;
}
