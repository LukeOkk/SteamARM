// ARM64_AUXV_LAYOUT: dynamic auxv, loader addresses, page size, and 4 KiB DSO loading.
#define _GNU_SOURCE
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <unistd.h>

extern char _start[];
static int failures;
static void check(const char *name, int yes)
{
    printf("  %s  %s\n", yes ? "ok  " : "FAIL", name);
    failures += !yes;
}
struct images { uintptr_t main_phdr, main_num, loader_base; int first, loader; };
static int image(struct dl_phdr_info *info, size_t size, void *data)
{
    (void)size;
    struct images *im = data;
    if (!im->first++) {
        im->main_phdr = (uintptr_t)info->dlpi_phdr;
        im->main_num = info->dlpi_phnum;
    }
    if (info->dlpi_name && strstr(info->dlpi_name, "ld-linux-aarch64.so.1")) {
        im->loader_base = info->dlpi_addr;
        im->loader = 1;
    }
    return 0;
}
static void *thread(void *arg) { return arg; }

int main(int argc, char **argv, char **envp)
{
    if (argc != 3) { puts("  FAIL  arguments"); puts("== auxv layout: FAIL"); return 1; }
    long expected = atol(argv[1]);
    int envc = 0;
    while (envc < 65536 && envp[envc]) envc++;
    Elf64_auxv_t *aux = (Elf64_auxv_t *)(envp + envc + 1);
    unsigned long vals[64] = {0};
    unsigned long types[64] = {0};
    int n = 0, duplicate = 0;
    for (; n < 64 && aux[n].a_type != AT_NULL; n++) {
        unsigned long type = aux[n].a_type;
        if (type != AT_IGNORE) {
            for (int i = 0; i < n; i++) if (types[i] == type) duplicate = 1;
            types[n] = type;
        }
        if (type < 64) vals[type] = aux[n].a_un.a_val;
    }
    check("AT_NULL within 64 pairs; unique types", n < 64 && !duplicate);
    struct images im = {0};
    dl_iterate_phdr(image, &im);
    check("AT_PHENT/PHDR/PHNUM", vals[AT_PHENT] == 56 && im.first &&
          vals[AT_PHDR] == im.main_phdr && vals[AT_PHNUM] == im.main_num);
    check("AT_ENTRY and AT_BASE", vals[AT_ENTRY] == (uintptr_t)_start &&
          im.loader && vals[AT_BASE] == im.loader_base);
    check("AT_PAGESZ agrees with libc", vals[AT_PAGESZ] == (unsigned long)expected &&
          expected == sysconf(_SC_PAGESIZE) && expected == getpagesize());
    FILE *f = fopen("/proc/self/auxv", "rb");
    unsigned long proc_page = 0, proc_hwcap = 0;
    if (f) {
        Elf64_auxv_t pair;
        while (fread(&pair, sizeof pair, 1, f) == 1 && pair.a_type != AT_NULL) {
            if (pair.a_type == AT_PAGESZ) proc_page = pair.a_un.a_val;
            if (pair.a_type == AT_HWCAP) proc_hwcap = pair.a_un.a_val;
        }
        fclose(f);
    }
    check("/proc/self/auxv agrees with getauxval", proc_page == getauxval(AT_PAGESZ) &&
          proc_hwcap == getauxval(AT_HWCAP) && proc_page && proc_hwcap);
    unsigned long hw = vals[AT_HWCAP];
    // HWCAP_CPUID (bit 11) is set: ID register reads are rewritten or
    // emulated from SIGILL (runtime/stack.c), and Wine ARM64 needs it.
    check("AT_HWCAP and HWCAP2", (hw & ((1UL << 0) | (1UL << 1) | (1UL << 8) | (1UL << 11))) ==
          ((1UL << 0) | (1UL << 1) | (1UL << 8) | (1UL << 11)) && vals[AT_HWCAP2] == 0);
    check("AT_CLKTCK and AT_SECURE", vals[AT_CLKTCK] == 100 &&
          sysconf(_SC_CLK_TCK) == 100 && vals[AT_SECURE] == 0);
    check("AT_UID/EUID/GID/EGID", vals[AT_UID] == getuid() && vals[AT_EUID] == geteuid() &&
          vals[AT_GID] == getgid() && vals[AT_EGID] == getegid());
    int random = vals[AT_RANDOM] != 0;
    unsigned char any = 0;
    if (random) for (int i = 0; i < 16; i++) any |= ((unsigned char *)vals[AT_RANDOM])[i];
    check("AT_RANDOM has entropy", random && any);
    int vdso = vals[AT_SYSINFO_EHDR] == 0 ||
               memcmp((void *)vals[AT_SYSINFO_EHDR], "\177ELF", 4) == 0;
    check("AT_SYSINFO_EHDR ELF when present", vdso);

    dlerror();
    void *so = dlopen(argv[2], RTLD_NOW);
    if (expected == 4096) {
        int (*answer)(void) = so ? dlsym(so, "pg4k_answer") : NULL;
        int (*bump)(void) = so ? dlsym(so, "pg4k_bump") : NULL;
        check("4 KiB DSO loads and executes separate data page", so && answer && bump &&
              answer() == 42 && bump() == 1 && bump() == 2);
        if (!so) printf("  dlerror  %s\n", dlerror());
    } else {
        const char *error = dlerror();
        printf("  dlerror  %s\n", error ? error : "(none)");
        check("16 KiB page rejects 4 KiB DSO", !so && error && strstr(error, "not page-aligned"));
    }
    if (so) dlclose(so);
    if (expected == 4096) {
        unsigned char *p = mmap(NULL, 65536, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        int mapped = p != MAP_FAILED;
        int protected = mapped && mprotect(p + 8192, 4096, PROT_READ) == 0;
        if (protected) { p[4096] = 0x42; p[12288] = 0x43; }
        check("4 KiB mprotect keeps neighbors writable", protected &&
              p[4096] == 0x42 && p[12288] == 0x43 && p[8192] == 0);
        if (mapped) munmap(p, 65536);
        pthread_t th;
        void *result = NULL;
        int created = pthread_create(&th, NULL, thread, (void *)0x42) == 0;
        int joined = created && pthread_join(th, &result) == 0;
        check("pthread_create and join", joined && result == (void *)0x42);
    }
    printf(failures ? "== auxv layout: FAIL (pagesz %ld)\n" :
                      "== auxv layout: ok (pagesz %ld)\n", expected);
    return failures != 0;
}
