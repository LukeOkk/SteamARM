// File mappings at a 4 KiB offset that is not on a 16 KiB host page, as the
// native arm64 webhelper makes them for its shared-memory pool (run with
// LXRT_GUEST_PAGE=4096; runtime/dispatch.c do_mmap, runtime/offmap.c).
//
// The runtime maps such a request from the host page below the offset, so the
// host range has a head before the guest's first byte and a tail after its
// last that the guest cannot name. Checked here:
//   - munmap of the whole mapping frees every host page of it, head and tail
//     included (mincore must report each unmapped), and 300
//     map/unmap cycles leave /proc/self/maps no longer;
//   - unmapping it piece by piece frees a host page with its last guest page
//     and not before (the other pieces stay readable);
//   - a mapping placed into the head's spare bytes is not taken down with it;
//   - madvise(MADV_DONTNEED) keeps a MAP_SHARED mapping's contents (file and
//     shared anonymous memory with a partial last host page), and a private
//     file mapping reads the file back, not zeros; private anonymous memory
//     still reads back zeros.
// Linux aarch64 with 4 KiB pages passes it as well: there the probes and the
// maps count only confirm what the kernel does anyway.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

#define KB 1024ul
#define HP (16 * KB)
#define FILE_LEN (1024 * KB)

static int ok, bad;
#define CHECK(c, ...) do { if (c) { ok++; printf("  OK   " __VA_ARGS__); } \
                           else { bad++; printf("  MAL  " __VA_ARGS__); } putchar('\n'); fflush(stdout); } while (0)

// The file's initial byte at each offset: never the same in two neighbouring
// 4 KiB pages, and 0 only where (off & 0xff) says so -- compared exactly.
static unsigned char pat(uint64_t off) { return (unsigned char)((off >> 12) * 29 + (off & 0xff) + 1); }

static int check_file_bytes(const unsigned char *p, uint64_t off, size_t len)
{
    for (size_t i = 0; i < len; i++)
        if (p[i] != pat(off + i))
            return (int)i + 1;
    return 0;
}

// Is every 16 KiB page of [a, a+len) unmapped? mincore fails with ENOMEM on
// an unmapped page, and changes nothing either way.
static int range_free(uint64_t a, size_t len)
{
    unsigned char vec[HP / 4096];
    for (uint64_t q = a; q < a + len; q += HP)
        if (mincore((void *)q, HP, vec) == 0 || errno != ENOMEM)
            return 0;
    return 1;
}

static int maps_lines(void)
{
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f)
        return -1;
    int n = 0, c;
    while ((c = fgetc(f)) != EOF)
        n += c == '\n';
    fclose(f);
    return n;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("page size %d\n", getpagesize());

    char path[] = "/tmp/lxrt-offmap-XXXXXX";
    int fd = mkstemp(path);
    CHECK(fd >= 0, "temporary file %s", path);
    if (fd < 0) return 1;
    unlink(path);
    static unsigned char buf[FILE_LEN];
    for (size_t i = 0; i < FILE_LEN; i++) buf[i] = pat(i);
    CHECK(pwrite(fd, buf, FILE_LEN, 0) == (ssize_t)FILE_LEN, "1 MiB of pattern written");

    // 1. MAP_SHARED at 0x1000: reads the file, writes reach it, and munmap
    //    frees every host page (head [hp, p) and tail included).
    unsigned char *p = mmap(NULL, 64 * KB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1000);
    CHECK(p != MAP_FAILED, "MAP_SHARED 64 KiB at offset 0x1000: %p (errno %d)", (void *)p, p == MAP_FAILED ? errno : 0);
    if (p == MAP_FAILED) return 1;
    uint64_t hp = (uint64_t)(uintptr_t)p - 0x1000;
    int hp_aligned = hp % HP == 0;          // how this runtime places it; Linux need not
    CHECK(!check_file_bytes(p, 0x1000, 64 * KB), "it reads the file from offset 0x1000");
    p[5] = 0xee;
    unsigned char back = 0;
    CHECK(pread(fd, &back, 1, 0x1005) == 1 && back == 0xee, "a store through it reaches the file");
    p[5] = pat(0x1005);
    CHECK(munmap(p, 64 * KB) == 0, "munmap the whole mapping");
    if (hp_aligned) {
        CHECK(range_free(hp, HP), "its first host page (head 0x%llx..%p) is free again", (unsigned long long)hp, (void *)p);
        CHECK(range_free(hp + 64 * KB, HP), "its last host page (tail after %p) is free again", (void *)(p + 64 * KB));
        CHECK(range_free(hp, 80 * KB), "all 80 KiB of host pages are free");
    }

    // 2. MAP_PRIVATE at 0x3000, ending on a host page: head only.
    p = mmap(NULL, 36 * KB, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0x3000);
    CHECK(p != MAP_FAILED && !check_file_bytes(p, 0x3000, 36 * KB), "MAP_PRIVATE 36 KiB at offset 0x3000 reads the file");
    hp = (uint64_t)(uintptr_t)p - 0x3000;
    p[0] = 1;                                   // a private copy of the head page
    CHECK(munmap(p, 36 * KB) == 0, "munmap it");
    if (hp_aligned)
        CHECK(range_free(hp, 48 * KB), "its 48 KiB of host pages are free again");

    // 3. Piece by piece: a host page goes with its last guest page.
    p = mmap(NULL, 64 * KB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1000);
    hp = (uint64_t)(uintptr_t)p - 0x1000;
    CHECK(p != MAP_FAILED, "MAP_SHARED 64 KiB at 0x1000 again");
    CHECK(munmap(p + 4 * KB, 4 * KB) == 0 && !check_file_bytes(p, 0x1000, 4 * KB) &&
          !check_file_bytes(p + 8 * KB, 0x3000, 4 * KB), "one 4 KiB page unmapped: its neighbours in the host page still read the file");
    CHECK(munmap(p, 4 * KB) == 0 && !check_file_bytes(p + 8 * KB, 0x3000, 4 * KB), "the first page unmapped: the third still reads");
    if (hp_aligned)
        CHECK(!range_free(hp, HP), "the first host page is still mapped while a guest page lives in it");
    CHECK(munmap(p + 8 * KB, 4 * KB) == 0, "the third page unmapped");
    if (hp_aligned)
        CHECK(range_free(hp, HP), "the first host page is free with its last guest page");
    CHECK(!check_file_bytes(p + 12 * KB, 0x4000, 48 * KB), "the rest still reads the file");
    CHECK(munmap(p + 12 * KB, 48 * KB) == 0 && !check_file_bytes(p + 60 * KB, 0x10000, 4 * KB),
          "the middle unmapped: the last guest page still reads");
    CHECK(munmap(p + 60 * KB, 4 * KB) == 0, "the last guest page unmapped");
    if (hp_aligned)
        CHECK(range_free(hp, 80 * KB), "every host page of it is free");

    // 4. Something mapped into the head's spare bytes lives on after the
    //    mapping around it is unmapped.
    p = mmap(NULL, 64 * KB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1000);
    hp = (uint64_t)(uintptr_t)p - 0x1000;
    if (p != MAP_FAILED && hp_aligned) {
        unsigned char *s = mmap((void *)hp, 4 * KB, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        CHECK(s == (void *)hp, "4 KiB anonymous MAP_FIXED into the head's spare bytes: %p", (void *)s);
        if (s == (void *)hp) {
            s[0] = 0x66;
            CHECK(munmap(p, 64 * KB) == 0 && s[0] == 0x66, "the mapping unmapped: the page placed in its head is intact");
            CHECK(munmap(s, 4 * KB) == 0, "and unmaps on its own");
        } else {
            munmap(p, 64 * KB);
        }
    } else if (p != MAP_FAILED) {
        munmap(p, 64 * KB);
    }

    // 5. The webhelper's pool: map, touch, unmap, 300 times at three offsets,
    //    two mappings alive at once.
    int before = maps_lines();
    int cyc_bad = 0;
    for (int i = 0; i < 300; i++) {
        uint64_t off = 0x1000 * (uint64_t)(1 + i % 3);
        unsigned char *a = mmap(NULL, 64 * KB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)off);
        unsigned char *b = mmap(NULL, 64 * KB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)(off + 64 * KB));
        if (a == MAP_FAILED || b == MAP_FAILED || a[0] != pat(off) || b[64 * KB - 1] != pat(off + 128 * KB - 1)) cyc_bad++;
        if ((a != MAP_FAILED && munmap(a, 64 * KB)) || (b != MAP_FAILED && munmap(b, 64 * KB))) cyc_bad++;
    }
    int after = maps_lines();
    CHECK(cyc_bad == 0, "300 cycles of two 64 KiB MAP_SHARED mappings at 4 KiB offsets: %d bad", cyc_bad);
    CHECK(before > 0 && after - before < 8, "/proc/self/maps: %d lines before, %d after", before, after);

    // 6. madvise(DONTNEED) on a MAP_SHARED file mapping at a 4 KiB offset keeps
    //    its contents (Linux drops only this process's page-table entries).
    p = mmap(NULL, 64 * KB, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0x1000);
    CHECK(p != MAP_FAILED, "MAP_SHARED 64 KiB at 0x1000 for madvise");
    for (size_t i = 0; i < 64 * KB; i++) p[i] = (unsigned char)(i * 7 + 3);
    CHECK(madvise(p, 64 * KB, MADV_DONTNEED) == 0, "madvise(DONTNEED) over all of it");
    int lost = 0;
    for (size_t i = 0; i < 64 * KB; i++) lost += p[i] != (unsigned char)(i * 7 + 3);
    static unsigned char fb[64 * KB];
    int flost = 0;
    if (pread(fd, fb, 64 * KB, 0x1000) != (ssize_t)(64 * KB)) flost = -1;
    else for (size_t i = 0; i < 64 * KB; i++) flost += fb[i] != (unsigned char)(i * 7 + 3);
    CHECK(lost == 0 && flost == 0, "shared contents kept: %d bytes changed in the mapping, %d in the file", lost, flost);
    for (size_t i = 0; i < 64 * KB; i++) p[i] = pat(0x1000 + i);   // the file as it was
    munmap(p, 64 * KB);

    // 7. A private file mapping at a 4 KiB offset reads the file after
    //    DONTNEED (Linux: the pages revert to the file), not zeros.
    p = mmap(NULL, 64 * KB, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0x1000);
    CHECK(p != MAP_FAILED && madvise(p, 64 * KB, MADV_DONTNEED) == 0,
          "MAP_PRIVATE 64 KiB at 0x1000, madvise(DONTNEED)");
    int zbad = check_file_bytes(p, 0x1000, 64 * KB);
    CHECK(zbad == 0, "private file mapping reads the file after DONTNEED (first difference at byte %d)", zbad - 1);
    munmap(p, 64 * KB);

    // 8. Shared anonymous memory whose last host page is partial: kept.
    unsigned char *sa = mmap(NULL, 20 * KB, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(sa != MAP_FAILED, "MAP_SHARED|MAP_ANONYMOUS 20 KiB");
    memset(sa, 0xa5, 20 * KB);
    CHECK(madvise(sa, 20 * KB, MADV_DONTNEED) == 0, "madvise(DONTNEED) over it");
    int kept = 0;
    for (size_t i = 0; i < 20 * KB; i++) kept += sa[i] == 0xa5;
    CHECK(kept == (int)(20 * KB), "shared anonymous contents kept: %d of %lu bytes", kept, 20 * KB);
    munmap(sa, 20 * KB);

    // 9. Private anonymous memory with a partial last host page still reads
    //    back zeros (FEX's caches, madvise_zero.c).
    unsigned char *pa = mmap(NULL, 20 * KB, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(pa, 0xa5, 20 * KB);
    CHECK(madvise(pa, 20 * KB, MADV_DONTNEED) == 0, "private anonymous 20 KiB, madvise(DONTNEED)");
    int nz = 0;
    for (size_t i = 0; i < 20 * KB; i++) nz += pa[i] != 0;
    CHECK(nz == 0, "private anonymous memory reads back zeros: %d non-zero bytes", nz);
    munmap(pa, 20 * KB);

    close(fd);
    printf("== mmap offset4k: %s (%d ok, %d mal)\n", bad ? "FAIL" : "ok", ok, bad);
    return bad ? 1 : 0;
}
