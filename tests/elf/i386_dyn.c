// Dynamic i386 probe: real ld-linux.so.2 + libc.so.6 from the rootfs.
// No crt1.o in the rootfs, so a minimal _start calls into libc directly;
// ld.so has already run libc's initialisers by then.
#include <stddef.h>
extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void free(void *);
extern char *strcpy(char *, const char *);
extern int getpid(void);
extern void exit(int) __attribute__((noreturn));
static int check(void) {
    int bad = 0;
    char *p = malloc(64);
    if (!p) { printf("MAL malloc\n"); return 1; }
    strcpy(p, "libc heap ok");
    printf("i386 dyn: %s, pid %d\n", p, getpid());
    free(p);
    double d = 1.5; d *= 3.0;                 // x87/SSE through the JIT
    if ((int)(d * 2) != 9) { printf("MAL fp\n"); bad++; }
    printf(bad ? "== i386 dyn: MAL\n" : "== i386 dyn: ok\n");
    return bad;
}
void _start(void) { exit(check()); }
