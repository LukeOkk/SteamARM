/* Ejercita las familias de syscalls que el runtime acaba de ganar. Cada bloque
   imprime OK o *** MAL ***; el runner cuenta los OK. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/file.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int oks, fails;
static void check(const char *what, int cond, const char *detail) {
    if (cond) { printf("  OK   %s\n", what); oks++; }
    else { printf("  *** MAL *** %s (%s)\n", what, detail ? detail : strerror(errno)); fails++; }
}

static void test_eventfd_epoll(void) {
    int efd = eventfd(0, EFD_CLOEXEC);
    check("eventfd creado", efd >= 0, NULL);
    if (efd < 0) return;

    uint64_t one = 1, got = 0;
    check("eventfd write", write(efd, &one, 8) == 8, NULL);
    check("eventfd read devuelve el contador", read(efd, &got, 8) == 8 && got == 1, "contador incorrecto");

    int ep = epoll_create1(EPOLL_CLOEXEC);
    check("epoll_create1", ep >= 0, NULL);
    if (ep < 0) { close(efd); return; }

    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0xCAFEBABEULL };
    check("epoll_ctl ADD", epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev) == 0, NULL);

    struct epoll_event out[4];
    check("epoll_pwait sin eventos expira", epoll_wait(ep, out, 4, 50) == 0, "deberia dar 0");

    write(efd, &one, 8);
    int n = epoll_wait(ep, out, 4, 500);
    check("epoll_pwait ve el eventfd listo", n == 1 && out[0].data.u64 == 0xCAFEBABEULL,
          "no llego el evento o se perdio el dato");
    read(efd, &got, 8);

    /* Reciclaje de descriptores: cerrar un fd vigilado debe sacarlo del set. */
    int p[2];
    if (pipe(p) == 0) {
        struct epoll_event pv = { .events = EPOLLIN, .data.u64 = 1 };
        epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &pv);
        int recycled = p[0];
        close(p[0]); close(p[1]);
        int q[2];
        if (pipe(q) == 0 && q[0] == recycled) {
            check("un fd reciclado se puede volver a anadir",
                  epoll_ctl(ep, EPOLL_CTL_ADD, q[0], &pv) == 0, "quedo envenenado");
            close(q[0]); close(q[1]);
        } else {
            printf("  --   reciclaje de fd no reproducible en esta corrida\n");
            if (q[0] >= 0) { close(q[0]); close(q[1]); }
        }
    }
    close(ep); close(efd);
}

static void test_flock_and_stat(void) {
    char path[] = "/tmp/lxrt_fam_XXXXXX";
    int fd = mkstemp(path);
    check("fichero temporal creado", fd >= 0, NULL);
    if (fd < 0) return;
    check("flock LOCK_EX", flock(fd, LOCK_EX) == 0, NULL);
    check("flock LOCK_UN", flock(fd, LOCK_UN) == 0, NULL);

    struct statfs sf;
    check("statfs", statfs("/tmp", &sf) == 0, NULL);
    check("fstatfs", fstatfs(fd, &sf) == 0, NULL);

    check("fdatasync", fdatasync(fd) == 0, NULL);

    /* statx por syscall directa: glibc puede no exponerlo. */
    unsigned char stx[256];
    memset(stx, 0, sizeof stx);
    long r = syscall(291 /* statx */, AT_FDCWD, path, 0, 0x7ff /* STATX_BASIC_STATS */, stx);
    uint32_t mask; memcpy(&mask, stx, 4);
    check("statx devuelve una mascara con campos", r == 0 && mask != 0, "mask vacia");

    close(fd); unlink(path);
}

static void test_sysv(void) {
    int sid = semget(IPC_PRIVATE, 1, IPC_CREAT | 0600);
    check("semget IPC_PRIVATE", sid >= 0, NULL);
    if (sid >= 0) {
        union { int val; void *p; } arg = { .val = 1 };
        check("semctl SETVAL", semctl(sid, 0, SETVAL, arg) == 0, NULL);
        check("semctl GETVAL devuelve 1", semctl(sid, 0, GETVAL) == 1, "valor incorrecto");
        struct sembuf op = { 0, -1, IPC_NOWAIT };
        check("semop baja el semaforo", semop(sid, &op, 1) == 0, NULL);
        semctl(sid, 0, IPC_RMID);
    }

    int mid = shmget(IPC_PRIVATE, 65536, IPC_CREAT | 0600);
    check("shmget 64 KiB", mid >= 0, NULL);
    if (mid >= 0) {
        void *at = shmat(mid, NULL, 0);
        check("shmat", at != (void *)-1, NULL);
        if (at != (void *)-1) {
            memcpy(at, "lxrt", 5);
            check("la memoria compartida conserva lo escrito", memcmp(at, "lxrt", 5) == 0, NULL);
            shmdt(at);
        }
        shmctl(mid, IPC_RMID, NULL);
    }
}

static void test_memfd(void) {
    long fd = syscall(279 /* memfd_create */, "lxrt-test", 1 /* MFD_CLOEXEC */);
    check("memfd_create", fd >= 0, NULL);
    if (fd < 0) return;
    check("memfd ftruncate", ftruncate((int)fd, 4096) == 0, NULL);
    void *m = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, (int)fd, 0);
    check("memfd mmap compartido", m != MAP_FAILED, NULL);
    if (m != MAP_FAILED) { memcpy(m, "ok", 3); munmap(m, 4096); }
    close((int)fd);
}

static volatile int cond_done;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static void *waiter(void *a) {
    (void)a;
    pthread_mutex_lock(&mu);
    while (!cond_done) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
    return NULL;
}

static void test_futex_requeue(void) {
    /* pthread_cond_broadcast usa FUTEX_CMP_REQUEUE en glibc. */
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, waiter, NULL);
    usleep(50000);
    pthread_mutex_lock(&mu);
    cond_done = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    int joined = 0;
    for (int i = 0; i < 4; i++) if (pthread_join(t[i], NULL) == 0) joined++;
    check("pthread_cond_broadcast despierta a los 4 (CMP_REQUEUE)", joined == 4, "se quedo alguno");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("== familias de syscalls\n");
    test_eventfd_epoll();
    test_flock_and_stat();
    test_sysv();
    test_memfd();
    test_futex_requeue();
    printf("== %d ok, %d mal\n", oks, fails);
    return fails ? 1 : 0;
}
