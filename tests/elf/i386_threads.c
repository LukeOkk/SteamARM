// i386 probe: glibc pthread stacks (mmap PROT_NONE, then mprotect everything
// but a 4 KiB guard to read/write -- 4 KiB boundaries inside 16 KiB host
// pages), deep use of each stack, and a join. No crt1.o in the rootfs, so
// _start calls into libc directly.
#include <stddef.h>
typedef unsigned long pthread_t;
typedef struct { char opaque[36]; } pthread_attr_t;
extern int printf(const char *, ...);
extern int pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
extern int pthread_join(pthread_t, void **);
extern int pthread_attr_init(pthread_attr_t *);
extern int pthread_attr_setstacksize(pthread_attr_t *, size_t);
extern void exit(int) __attribute__((noreturn));

static void *worker(void *arg) {
    // Touch most of the stack, top to bottom, like a deep call chain would.
    size_t n = (size_t)arg;
    volatile char buf[n];
    for (size_t i = 0; i < n; i += 512)
        buf[n - 1 - i] = (char)i;
    return (void *)(size_t)(buf[0] + 1);
}

static int check(void) {
    int bad = 0;
    for (int round = 0; round < 3; round++) {
        pthread_t t[6];
        pthread_attr_t a;
        for (int i = 0; i < 6; i++) {
            pthread_attr_init(&a);
            size_t sz = (size_t)(64 + 48 * i) * 1024;   // uneven sizes on purpose
            pthread_attr_setstacksize(&a, sz);
            if (pthread_create(&t[i], &a, worker, (void *)(sz - 16 * 1024)) != 0) {
                printf("MAL pthread_create %d\n", i);
                bad++;
                t[i] = 0;
            }
        }
        for (int i = 0; i < 6; i++)
            if (t[i] && pthread_join(t[i], NULL) != 0) { printf("MAL join %d\n", i); bad++; }
    }
    printf(bad ? "== i386 threads: MAL\n" : "== i386 threads: ok\n");
    return bad;
}
void _start(void) { exit(check()); }
