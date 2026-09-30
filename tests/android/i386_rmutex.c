// A recursive and an error-checking pthread mutex taken by a second thread
// of an i386 program linked against the x86_64 image's 32-bit bionic, run by
// FEX's 32-bit mode under lxrun. 32-bit bionic keeps a mutex owner's tid in
// 16 bits (pthread_mutex_t is 4 bytes there) and refuses to start at all
// when the main thread's tid is above 65535 ("Limited by the size of
// pthread_mutex_t, 32 bit bionic libc only accepts pid <= 65535"). Under
// lxrun the main thread's tid is the Mac's pid (up to 99999) and every
// other thread's is 200000 + pid * 10000 + n (runtime/thread.c, next_tid),
// so a second thread's own recursive mutex is not its own: MEASURED, relock
// EBUSY and unlock EPERM, and a printf from that thread (stdio's recursive
// lock) never returns (benchmarks/stage28-android-reliability.txt). An
// expected failure in tests/android/run.sh until the runtime gives 32-bit
// bionic guests tids below 65536.
//
// Last line: "== i386_rmutex: ok" or "== i386_rmutex: MAL".
#include <stddef.h>
#include <stdint.h>
typedef long pthread_t;
typedef struct { int32_t __private[1]; } pthread_mutex_t;
typedef long pthread_mutexattr_t;
int printf(const char *fmt, ...);
int pthread_create(pthread_t *t, const void *attr, void *(*fn)(void *), void *arg);
int pthread_join(pthread_t t, void **ret);
int pthread_mutexattr_init(pthread_mutexattr_t *a);
int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type);
int pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int pthread_mutex_lock(pthread_mutex_t *m);
int pthread_mutex_trylock(pthread_mutex_t *m);
int pthread_mutex_unlock(pthread_mutex_t *m);
int gettid(void);
int fflush(void *f);
#define PTHREAD_MUTEX_RECURSIVE 1
#define PTHREAD_MUTEX_ERRORCHECK 2

static pthread_mutex_t rec, chk;
static int R[8];
static void *body(void *arg)
{
    (void)arg;
    R[0] = gettid();
    R[1] = pthread_mutex_lock(&rec);
    R[2] = pthread_mutex_trylock(&rec);        // recursive, owned by this thread: 0
    R[3] = pthread_mutex_unlock(&rec);
    R[4] = R[2] == 0 ? pthread_mutex_unlock(&rec) : -1;
    R[5] = pthread_mutex_lock(&chk);
    R[6] = pthread_mutex_unlock(&chk);         // error-checking, by its owner: 0
    return 0;
}
int main(void)
{
    pthread_mutexattr_t ar, ac;
    pthread_mutexattr_init(&ar); pthread_mutexattr_settype(&ar, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutexattr_init(&ac); pthread_mutexattr_settype(&ac, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init(&rec, &ar); pthread_mutex_init(&chk, &ac);
    printf("main tid %d\n", gettid()); fflush(0);
    pthread_t t;
    pthread_create(&t, 0, body, 0);
    pthread_join(t, 0);
    printf("thread tid %d: recursive lock %d, relock (trylock) %d, unlock %d %d; errorcheck lock %d, unlock %d\n",
           R[0], R[1], R[2], R[3], R[4], R[5], R[6]);
    int ok = R[1] == 0 && R[2] == 0 && R[3] == 0 && R[4] == 0 && R[5] == 0 && R[6] == 0;
    printf("== i386_rmutex: %s\n", ok ? "ok" : "MAL");
    return ok ? 0 : 1;
}
// crtbegin's part, as tests/android/bionic_min.h does it for x86_64/aarch64.
typedef struct { void (**preinit_array)(void); void (**init_array)(void); void (**fini_array)(void); } structors_array_t;
__attribute__((noreturn)) void __libc_init(void *raw_args, void (*onexit)(void),
                                           int (*slingshot)(int, char **, char **),
                                           structors_array_t const *const structors);
static void (*fini[2])(void) = { (void (*)(void))-1, 0 };
static int slingshot(int argc, char **argv, char **envp) { (void)argc; (void)argv; (void)envp; return main(); }
__attribute__((used, regparm(0))) static void _start_main(void *raw_args)
{
    structors_array_t array = { 0, 0, fini };
    __libc_init(raw_args, 0, slingshot, &array);
}
__asm__(".globl _start\n_start:\n  mov %esp, %eax\n  and $-16, %esp\n  sub $12, %esp\n  push %eax\n  call _start_main\n  hlt\n");
