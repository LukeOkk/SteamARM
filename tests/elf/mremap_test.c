// Linux: gcc -static-pie -O2 -o mremap_test tests/elf/mremap_test.c
// The isolated Darwin harness supplies HOST_TEST and a direct-call adapter.
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED 2
#endif
#ifndef MREMAP_DONTUNMAP
#define MREMAP_DONTUNMAP 4
#endif

static int good, bad;
static size_t page;
static void check(int passed, const char *what)
{
    printf("  %s  %s\n", passed ? "OK " : "MAL", what);
    if (passed) good++; else bad++;
}

#ifndef HOST_TEST
static void *remap(void *a, size_t old, size_t len, int flags, void *target)
{
    return (void *)syscall(SYS_mremap, a, old, len, flags, target);
}
#endif

static unsigned char *arena(void)
{
    void *p = mmap(NULL, 8 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) { perror("arena mmap"); exit(1); }
    return p;
}

static unsigned char *mapping(void *a, size_t n)
{
    void *p = mmap(a, n, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) { perror("fixed mmap"); exit(1); }
    return p;
}

static void pattern(unsigned char *a, size_t n)
{
    for (size_t i = 0; i < n; i++) a[i] = (unsigned char)(i * 17 + 23);
}

static int matches(const unsigned char *a, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (a[i] != (unsigned char)(i * 17 + 23)) return 0;
    return 1;
}

static int zeros(const unsigned char *a, size_t n)
{
    for (size_t i = 0; i < n; i++) if (a[i]) return 0;
    return 1;
}

// Kernel copyin probes avoid intentional SIGSEGV and signal-handler state.
static int inaccessible(void *a)
{
    int fds[2];
    if (pipe(fds)) return 0;
    errno = 0;
    ssize_t n = write(fds[1], a, 1);
    int e = errno;
    close(fds[0]); close(fds[1]);
    return n == -1 && e == EFAULT;
}

static int mapped(void *a)
{
#ifdef HOST_TEST
    return host_mapped(a);
#else
    unsigned char vec;
    return mincore(a, page, &vec) == 0;
#endif
}

static int unwritable(void *a)
{
    int fds[2];
    if (pipe(fds)) return 0;
    unsigned char byte = 0;
    if (write(fds[1], &byte, 1) != 1) {
        close(fds[0]); close(fds[1]); return 0;
    }
    errno = 0;
    ssize_t n = read(fds[0], a, 1);
    int e = errno;
    close(fds[0]); close(fds[1]);
    return n == -1 && e == EFAULT;
}

static int fails(void *a, size_t old, size_t len, int flags, void *to, int e)
{
    errno = 0;
    return remap(a, old, len, flags, to) == MAP_FAILED && errno == e;
}

int main(void)
{
    page = (size_t)sysconf(_SC_PAGESIZE); // 0x1000 Linux, 0x4000 Darwin arm64.
    unsigned char *base = arena(), *a = mapping(base + 2 * page, 2 * page);
    pattern(a, 2 * page);
    void *r = remap(a, 2 * page, page, 0, NULL);
    check(r == a && matches(a, page) && !mapped(a + page), "encoger: direccion, contenido y cola desmontada");
    munmap(base, 8 * page);

    base = arena(); a = mapping(base + 2 * page, page); pattern(a, page);
    munmap(a + page, page);
    r = remap(a, page, 2 * page, 0, NULL);
    check(r == a && matches(a, page) && zeros(a + page, page), "crecer en sitio: contenido y extension cero");
    munmap(base, 8 * page);

    base = arena(); a = mapping(base + 2 * page, page); pattern(a, page);
    unsigned char *b = mapping(a + page, page); memset(b, 0x5a, page);
    r = remap(a, page, 2 * page, MREMAP_MAYMOVE, NULL);
    check(r != MAP_FAILED && r != a && matches(r, page) && zeros((unsigned char *)r + page, page), "crecer bloqueado: mover y copiar cada byte");
    check(r != MAP_FAILED && inaccessible(a) && !mapped(a), "direccion antigua inaccesible y desmontada");
    check(b[0] == 0x5a && b[page - 1] == 0x5a, "bloqueador B intacto");
    if (r != MAP_FAILED && r != a) munmap(r, 2 * page);
    munmap(base, 8 * page);

    base = arena(); a = mapping(base + 2 * page, page); pattern(a, page);
    check(fails(a, page, 2 * page, 0, NULL, ENOMEM) && matches(a, page), "sin MAYMOVE bloqueado: ENOMEM, origen intacto");
    unsigned char *target = mapping(base + 5 * page, 2 * page); memset(target, 0xcc, 2 * page);
    r = remap(a, page, 2 * page, MREMAP_MAYMOVE | MREMAP_FIXED, target);
    check(r == target && matches(target, page) && zeros(target + page, page) && !mapped(a), "FIXED: destino ocupado sobrescrito");
    munmap(base, 8 * page);

    base = arena(); a = mapping(base + 2 * page, 2 * page); pattern(a, 2 * page);
    target = mapping(base + 5 * page, page);
    r = remap(a, 2 * page, page, MREMAP_MAYMOVE | MREMAP_FIXED, target);
    check(r == target && matches(target, page) && !mapped(a) && !mapped(a + page), "FIXED al encoger: mover y desmontar origen entero");
    munmap(base, 8 * page);

    base = arena(); a = mapping(base + 2 * page, page); pattern(a, page);
    r = remap(a, page, page, MREMAP_MAYMOVE | MREMAP_DONTUNMAP, NULL);
    check(r != MAP_FAILED && r != a && matches(r, page), "DONTUNMAP: mover contenido");
#ifdef HOST_TEST
    check(r != MAP_FAILED && mapped(a) && inaccessible(a), "DONTUNMAP host: origen mapeado PROT_NONE");
    int readable = mprotect(a, page, PROT_READ) == 0;
    check(r != MAP_FAILED && readable && zeros(a, page), "DONTUNMAP host: ceros tras mprotect");
#else
    check(r != MAP_FAILED && mapped(a) && zeros(a, page), "DONTUNMAP Linux: origen mapeado, lectura cero");
#endif
    if (r != MAP_FAILED && r != a) munmap(r, page);
    munmap(base, 8 * page);

    base = arena(); a = mapping(base + 2 * page, page);
    check(fails(a, 0, page, MREMAP_MAYMOVE, NULL, EINVAL), "old_len cero: EINVAL");
    check(fails(a, page, 0, 0, NULL, EINVAL), "new_len cero: EINVAL");
    munmap(a, page);
    check(fails(a, page, 2 * page, MREMAP_MAYMOVE, NULL, EFAULT), "origen no mapeado: EFAULT");
    a = mapping(a, page);
    check(fails(a + 1, page, 2 * page, MREMAP_MAYMOVE, NULL, EINVAL), "origen desalineado: EINVAL");
    check(fails(a, page, page, 8, NULL, EINVAL), "flags desconocidos: EINVAL");
    check(fails(a, page, page, MREMAP_FIXED, base + 5 * page, EINVAL), "FIXED sin MAYMOVE: EINVAL");
    check(fails(a, page, 2 * page, MREMAP_MAYMOVE | MREMAP_FIXED, a, EINVAL), "FIXED solapado: EINVAL");
    check(fails(a, page, page, MREMAP_MAYMOVE | MREMAP_FIXED, base + 5 * page + 1, EINVAL), "destino desalineado: EINVAL");
    check(fails(a, page, page, MREMAP_DONTUNMAP, NULL, EINVAL), "DONTUNMAP sin MAYMOVE: EINVAL");
    check(fails(a, page, 2 * page, MREMAP_MAYMOVE | MREMAP_DONTUNMAP, NULL, EINVAL), "DONTUNMAP tamanos distintos: EINVAL");
    munmap(base, 8 * page);

    for (int none = 0; none < 2; none++) {
        base = arena(); a = mapping(base + 2 * page, page); pattern(a, page);
        mprotect(a, page, none ? PROT_NONE : PROT_READ);
        r = remap(a, page, 2 * page, MREMAP_MAYMOVE, NULL);
        int valid = r != MAP_FAILED;
        if (valid && none) valid = inaccessible(r) && mprotect(r, 2 * page, PROT_READ) == 0;
        check(valid && unwritable(r) && matches(r, page) && zeros((unsigned char *)r + page, page),
              none ? "mover PROT_NONE: contenido recuperable" : "mover solo lectura: contenido intacto");
        if (r != MAP_FAILED && r != a) munmap(r, 2 * page);
        munmap(base, 8 * page);
    }
#ifdef HOST_TEST
    host_extra();
#endif
    printf("== %d ok, %d mal\n", good, bad);
    return bad ? 1 : 0;
}
