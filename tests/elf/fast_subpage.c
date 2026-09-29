// CEF_FILE_BACKED_MAPPING_FAST_PATH: a large offset file map is direct and private.
#define _GNU_SOURCE
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

static int failures;
static void check(const char *name, int yes)
{
    printf("  %s  %s\n", yes ? "ok  " : "FAIL", name);
    failures += !yes;
}
int main(void)
{
    enum { FILE_SIZE = 4 * 1024 * 1024, LENGTH = FILE_SIZE - 0x2000 };
    char path[128];
    snprintf(path, sizeof path, "/tmp/lxrt-fast-subpage-%ld.bin", (long)getpid());
    int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) { check("create file", 0); goto verdict; }
    unlink(path);
    uint32_t words[4096];
    int written = 1;
    for (unsigned block = 0; block < FILE_SIZE / sizeof words; block++) {
        for (unsigned i = 0; i < 4096; i++) words[i] = block * 4096 + i;
        if (pwrite(fd, words, sizeof words, (off_t)block * sizeof words) != sizeof words) written = 0;
    }
    check("4 MiB word-index file", written);
    void *reservation = mmap(NULL, 8 * 1024 * 1024, PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reservation == MAP_FAILED) { check("reserve 8 MiB", 0); close(fd); goto verdict; }
    uintptr_t base = ((uintptr_t)reservation + 0x3fff) & ~(uintptr_t)0x3fff;
    uint32_t *map = mmap((void *)(base + 0x1000), LENGTH, PROT_READ,
                         MAP_PRIVATE | MAP_FIXED, fd, 0x1000);
    int mapped = map != MAP_FAILED;
    check("4 KiB-offset MAP_FIXED file mapping", mapped);
    if (mapped) {
        int equal = 1;
        for (size_t pos = 0; pos < LENGTH; pos = pos ? pos + 0x4000 : 0x3000) {
            uint32_t file_word = 0;
            if (pread(fd, &file_word, sizeof file_word, 0x1000 + (off_t)pos) != sizeof file_word ||
                *(uint32_t *)((char *)map + pos) != file_word) equal = 0;
        }
        uint32_t last = 0;
        if (pread(fd, &last, sizeof last, FILE_SIZE - 0x1000 - 4) != sizeof last ||
            *(uint32_t *)((char *)map + LENGTH - 4) != last) equal = 0;
        check("first, last, and every 16 KiB boundary match file", equal);
        size_t middle = 2 * 1024 * 1024 - 0x1000;
        int protected = mprotect((char *)map + middle, 0x1000, PROT_READ | PROT_WRITE) == 0;
        uint32_t old = 0, after = 0;
        if (protected) {
            pread(fd, &old, sizeof old, 0x1000 + (off_t)middle);
            *(uint32_t *)((char *)map + middle) = 0xdeadbeef;
            pread(fd, &after, sizeof after, 0x1000 + (off_t)middle);
        }
        check("one private 4 KiB page writable, file unchanged", protected &&
              old == after && *(uint32_t *)((char *)map + middle) == 0xdeadbeef);
    }
    munmap(reservation, 8 * 1024 * 1024);
    close(fd);
verdict:
    puts(failures ? "== fast subpage: FAIL" : "== fast subpage: ok");
    return failures != 0;
}
