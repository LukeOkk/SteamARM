// dlopen a 4 KiB-aligned library whose code stores into its own data from the
// same 16 KiB host page (tests/elf/libself4k.c), under LXRT_GUEST_PAGE=4096.
// Before runtime/storemu.c the first such store never completed (the W/X
// flip livelock). Also checks that emulated atomics stay atomic: four threads
// add through the LL/SC and the LSE variant at once, and the total is exact.
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>

#define THREADS 4
#define PER_THREAD 2000

static int (*bump)(void), (*llsc_add)(int), (*llsc_cas)(int, int);
static int (*lse_add)(int), (*lse_cas)(int, int);
static long (*fill)(long);
static int fails;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL " __VA_ARGS__); printf("\n"); fails++; } } while (0)

static void *adder(void *arg)
{
    (void)arg;
    for (int i = 0; i < PER_THREAD; i++) {
        llsc_add(1);
        lse_add(1);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: dlopen_self4k <libself4k.so>\n");
        return 2;
    }
    void *so = dlopen(argv[1], RTLD_NOW);
    if (!so) {
        printf("FAIL dlopen: %s\n", dlerror());
        return 1;
    }
    bump = (int (*)(void))dlsym(so, "self4k_bump");
    fill = (long (*)(long))dlsym(so, "self4k_fill");
    llsc_add = (int (*)(int))dlsym(so, "self4k_llsc_add");
    llsc_cas = (int (*)(int, int))dlsym(so, "self4k_llsc_cas");
    lse_add = (int (*)(int))dlsym(so, "self4k_lse_add");
    lse_cas = (int (*)(int, int))dlsym(so, "self4k_lse_cas");
    int *counter = (int *)dlsym(so, "self4k_counter");
    long *wide = (long *)dlsym(so, "self4k_wide");
    int *atomic = (int *)dlsym(so, "self4k_atomic");
    if (!bump || !fill || !llsc_add || !llsc_cas || !lse_add || !lse_cas ||
        !counter || !wide || !atomic) {
        printf("FAIL dlsym\n");
        return 1;
    }

    for (int i = 1; i <= 1000; i++) {
        int got = bump();
        if (got != i) {
            printf("FAIL bump %d returned %d\n", i, got);
            return 1;
        }
    }
    CHECK(*counter == 1000, "counter %d", *counter);
    CHECK(fill(100) == 103, "fill");
    for (int i = 0; i < 4; i++)
        CHECK(wide[i] == 100 + i, "wide[%d] = %ld", i, wide[i]);

    CHECK(lse_add(5) == 5, "lse_add");
    CHECK(llsc_add(7) == 12, "llsc_add");
    CHECK(llsc_cas(12, 40) == 12 && *atomic == 40, "llsc_cas success (%d)", *atomic);
    CHECK(llsc_cas(12, 50) == 40 && *atomic == 40, "llsc_cas failure (%d)", *atomic);
    CHECK(lse_cas(40, 0) == 40 && *atomic == 0, "lse_cas success (%d)", *atomic);
    CHECK(lse_cas(40, 9) == 0 && *atomic == 0, "lse_cas failure (%d)", *atomic);

    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, adder, NULL);
    for (int i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    CHECK(*atomic == THREADS * PER_THREAD * 2, "contended total %d, expected %d",
          *atomic, THREADS * PER_THREAD * 2);

    if (fails)
        return 1;
    printf("== self4k: ok (%d contended atomic adds)\n", THREADS * PER_THREAD * 2);
    return 0;
}
