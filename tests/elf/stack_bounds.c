// ARM64_INITIAL_STACK_BOUNDS: entry layout, string order, padding, and usable main stack.
#define _GNU_SOURCE
#include <elf.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>

static int failures;
static void check(const char *name, int yes)
{
    printf("  %s  %s\n", yes ? "ok  " : "FAIL", name);
    failures += !yes;
}

__attribute__((noinline)) static unsigned long recurse(int depth)
{
    volatile unsigned char page[65536];
    unsigned long sum = 0;
    for (size_t i = 0; i < sizeof page; i += 4096) {
        page[i] = (unsigned char)depth;
        sum += page[i];
    }
    if (depth) sum += recurse(depth - 1);
    return sum + page[0];
}

int main(int argc, char **argv, char **envp)
{
    uint64_t *sp0 = (uint64_t *)argv - 1;
    check("entry stack 16-byte aligned and argc", ((uintptr_t)sp0 % 16) == 0 && *sp0 == (uint64_t)argc);
    int envc = 0;
    while (envc < 65536 && envp[envc]) envc++;
    Elf64_auxv_t *aux = (Elf64_auxv_t *)(envp + envc + 1);
    int null_at = -1;
    unsigned long pagesz = 0, random = 0;
    for (int i = 0; i < 64; i++) {
        if (aux[i].a_type == AT_NULL) { null_at = i; break; }
        if (aux[i].a_type == AT_PAGESZ) pagesz = aux[i].a_un.a_val;
        if (aux[i].a_type == AT_RANDOM) random = aux[i].a_un.a_val;
    }
    check("argv/envp/auxv adjacency and AT_PAGESZ", argv[argc] == NULL &&
          envp == argv + argc + 1 && envc < 65536 && null_at >= 0 &&
          pagesz == getauxval(AT_PAGESZ));
    uintptr_t aux_end = null_at >= 0 ? (uintptr_t)&aux[null_at + 1] : 0;
    uintptr_t lowest = UINTPTR_MAX, highest = 0;
    int strings_above = null_at >= 0;
    for (int i = 0; i < argc + envc; i++) {
        char *s = i < argc ? argv[i] : envp[i - argc];
        uintptr_t start = (uintptr_t)s, end = start + strlen(s) + 1;
        if (start < aux_end) strings_above = 0;
        if (start < lowest) lowest = start;
        if (end > highest) highest = end;
    }
    int random_ok = random >= aux_end && random + 16 <= lowest;
    unsigned char any = 0;
    if (random_ok)
        for (int i = 0; i < 16; i++) any |= ((unsigned char *)random)[i];
    check("strings above auxv; AT_RANDOM between auxv and strings", strings_above && random_ok && any);
    volatile unsigned char checksum = 0;
    if (highest)
        for (size_t i = 0; i < 4096; i++) checksum ^= ((volatile unsigned char *)highest)[i];
    (void)checksum;
    check("4096 readable pad bytes above strings", highest != 0);

    char local;
    pthread_attr_t attr;
    void *stack = NULL;
    size_t size = 0;
    int attr_ok = pthread_getattr_np(pthread_self(), &attr) == 0;
    if (attr_ok) {
        attr_ok = pthread_attr_getstack(&attr, &stack, &size) == 0;
        pthread_attr_destroy(&attr);
    }
    unsigned long map_end = 0;
    FILE *maps = fopen("/proc/self/maps", "r");
    char line[512];
    if (maps) {
        while (fgets(line, sizeof line, maps)) {
            unsigned long a, b;
            if (sscanf(line, "%lx-%lx", &a, &b) == 2 &&
                a <= (uintptr_t)&local && (uintptr_t)&local < b) {
                map_end = b;
                printf("  maps  %s", line);
                break;
            }
        }
        fclose(maps);
    }
    uintptr_t lo = (uintptr_t)stack, hi = lo + size;
    printf("  stack lo=%#lx hi=%#lx size=%zu\n", (unsigned long)lo, (unsigned long)hi, size);
    check("pthread main-stack bounds inside /proc/self/maps", attr_ok &&
          lo <= (uintptr_t)&local && (uintptr_t)&local < hi &&
          size >= 1024 * 1024 && map_end && hi <= map_end);
    unsigned long touched = recurse(95);
    check("6 MiB main-stack recursion", touched != 0);
    int ordered = argc > 0;
    for (int i = 0; i + 1 < argc; i++)
        ordered &= argv[i + 1] == argv[i] + strlen(argv[i]) + 1;
    if (argc && envc)
        ordered &= envp[0] == argv[argc - 1] + strlen(argv[argc - 1]) + 1;
    for (int i = 0; i + 1 < envc; i++)
        ordered &= envp[i + 1] == envp[i] + strlen(envp[i]) + 1;
    check("Linux contiguous argv then env strings", ordered);
    printf(failures ? "== stack bounds: FAIL\n" : "== stack bounds: ok\n");
    return failures != 0;
}
