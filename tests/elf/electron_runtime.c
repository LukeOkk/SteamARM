/* What Electron (Heroic Games Launcher, linux-arm64) needed from the runtime
 * (benchmarks/stage24-heroic.txt), one check per fix. Each block prints
 * "ok" or "MAL"; the last line is "== electron runtime: ok" or a count.
 *
 *   prlimit   prlimit64's old-limit buffer on a read-only page is EFAULT, not
 *             a fault in the runtime (Chromium's ProtectedMemory check).
 *   mprotect  a length that is not a page multiple covers the whole last page
 *             (Chromium's protected_memory section, 0x1001a bytes).
 *   shm       IPC_RMID of an attached SysV segment keeps it attachable until
 *             the last detach or exit (cairo's MIT-SHM pool and the X server).
 *   execve    a missing file fails with ENOENT and the caller goes on
 *             (execvp along PATH, "#!/usr/bin/env python3").
 *   epoll     a dup of an epoll descriptor is the same instance (tokio).
 *   seqpacket a peer's close ends a blocked recvmsg and ppoll with EOF
 *             (Chromium's zygotes).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int oks, mal;
static void check(int cond, const char *what)
{
    if (cond) { printf("  ok   %s\n", what); oks++; }
    else { printf("  MAL  %s (errno %d %s)\n", what, errno, strerror(errno)); mal++; }
}

static long prlimit_old(void *p)
{
    return syscall(SYS_prlimit64, 0, RLIMIT_NPROC, NULL, p);
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static void test_prlimit(void)
{
    struct rlimit rl;
    check(prlimit_old(&rl) == 0, "prlimit64 into a writable buffer");
    long pg = sysconf(_SC_PAGESIZE);
    char *p = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(p, 0x5a, 64);
    mprotect(p, pg, PROT_READ);
    errno = 0;
    long r = prlimit_old(p);
    check(r == -1 && errno == EFAULT, "prlimit64 into a read-only page: EFAULT");
    check(p[0] == 0x5a && p[15] == 0x5a, "the read-only page is unchanged");
    mprotect(p, pg, PROT_NONE);
    errno = 0;
    check(prlimit_old(p) == -1 && errno == EFAULT, "prlimit64 into a PROT_NONE page: EFAULT");
    mprotect(p, pg, PROT_READ | PROT_WRITE);
    check(prlimit_old(p) == 0, "writable again: prlimit64 succeeds");
    munmap(p, pg);
}

static void test_mprotect(void)
{
    long pg = sysconf(_SC_PAGESIZE);
    char *p = mmap(NULL, 3 * pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memset(p, 1, 3 * pg);
    /* Chromium: SetDataReadOnly(start, 0x1001a-like length). */
    check(mprotect(p, pg + 26, PROT_READ) == 0, "mprotect with a length one page + 26 bytes");
    errno = 0;
    check(prlimit_old(p + pg) == -1 && errno == EFAULT, "the partial last page is read-only too");
    check(prlimit_old(p + 2 * pg) == 0, "the page after it is still writable");
    mprotect(p, 3 * pg, PROT_READ | PROT_WRITE);
    munmap(p, 3 * pg);
    printf("       (page size %ld)\n", pg);
}

static void test_shm(void)
{
    int id = shmget(IPC_PRIVATE, 65536, IPC_CREAT | 0600);
    check(id >= 0, "shmget 64 KiB");
    if (id < 0) return;
    char *a = shmat(id, NULL, 0);
    check(a != (void *)-1, "shmat");
    strcpy(a, "hello");
    check(shmctl(id, IPC_RMID, NULL) == 0, "IPC_RMID while attached");
    struct shmid_ds ds;
    check(shmctl(id, IPC_STAT, &ds) == 0 && (ds.shm_perm.mode & 01000), "IPC_STAT after IPC_RMID: SHM_DEST set");
    /* Another process attaches after the IPC_RMID, as the X server does. */
    pid_t c = fork();
    if (c == 0) {
        char *b = shmat(id, NULL, 0);
        if (b == (void *)-1) _exit(2);
        int same = strcmp(b, "hello") == 0;
        strcpy(b, "world");
        shmdt(b);
        _exit(same ? 0 : 3);
    }
    int st = -1;
    waitpid(c, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "a second process attaches after IPC_RMID and sees the data");
    check(strcmp(a, "world") == 0, "its write is seen through the first attach");
    check(shmdt(a) == 0, "last detach");
    errno = 0;
    check(shmat(id, NULL, 0) == (void *)-1, "after the last detach the segment is gone");

    /* A process that exits with its IPC_RMID deferred: the segment goes then. */
    int pfd[2];
    pipe(pfd);
    c = fork();
    if (c == 0) {
        int cid = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);
        char *b = cid >= 0 ? shmat(cid, NULL, 0) : (void *)-1;
        if (b == (void *)-1 || shmctl(cid, IPC_RMID, NULL) != 0) _exit(2);
        write(pfd[1], &cid, sizeof cid);
        _exit(0);                       /* no shmdt */
    }
    int cid = -1;
    read(pfd[0], &cid, sizeof cid);
    waitpid(c, &st, 0);
    errno = 0;
    check(cid >= 0 && shmat(cid, NULL, 0) == (void *)-1, "exit with a deferred IPC_RMID removes the segment");
    close(pfd[0]);
    close(pfd[1]);
}

static void test_execve(const char *self)
{
    char *argv[] = { "x", NULL };
    errno = 0;
    check(execve("/nonexistent-dir/prog", argv, environ) == -1 && errno == ENOENT,
          "execve of a missing file returns ENOENT");
    pid_t c = fork();
    if (c == 0) {
        const char *slash = strrchr(self, '/');
        char dir[1024];
        snprintf(dir, sizeof dir, "/nonexistent-dir:%.*s", (int)(slash - self), self);
        setenv("PATH", dir, 1);
        char *av[] = { (char *)(slash + 1), "--exit7", NULL };
        execvp(av[0], av);
        _exit(99);
    }
    int st = -1;
    waitpid(c, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 7, "execvp goes on to the next PATH entry");
}

static void test_epoll(void)
{
    int ep = epoll_create1(EPOLL_CLOEXEC);
    int d1 = fcntl(ep, F_DUPFD_CLOEXEC, 3);
    int d2 = dup(ep);
    int ev = eventfd(0, EFD_NONBLOCK);
    struct epoll_event e = { .events = EPOLLIN, .data.u64 = 42 };
    check(epoll_ctl(d1, EPOLL_CTL_ADD, ev, &e) == 0, "epoll_ctl ADD through an F_DUPFD_CLOEXEC copy");
    uint64_t one = 1;
    write(ev, &one, 8);
    struct epoll_event out;
    check(epoll_wait(ep, &out, 1, 1000) == 1 && out.data.u64 == 42, "the original descriptor sees the event");
    close(ep);
    check(epoll_wait(d2, &out, 1, 1000) == 1, "a dup still works after the original is closed");
    check(epoll_ctl(d2, EPOLL_CTL_DEL, ev, NULL) == 0, "epoll_ctl DEL through the dup");
    close(d1);
    close(d2);
    close(ev);
}

static int g_sv[2];
static long g_got = -2;
static double g_at;
static void *recv_thread(void *a)
{
    char buf[64];
    struct iovec iov = { buf, sizeof buf };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    g_got = recvmsg(g_sv[0], &m, 0);
    g_at = now();
    return a;
}

static void test_seqpacket(void)
{
    check(socketpair(AF_UNIX, SOCK_SEQPACKET, 0, g_sv) == 0, "socketpair SOCK_SEQPACKET");
    send(g_sv[1], "ab", 2, 0);
    send(g_sv[1], "cde", 3, 0);
    char buf[16];
    check(recv(g_sv[0], buf, sizeof buf, 0) == 2 && recv(g_sv[0], buf, sizeof buf, 0) == 3,
          "message boundaries kept");
    struct linger l = { 1, 1 };
    socklen_t ll = sizeof l;
    check(getsockopt(g_sv[0], SOL_SOCKET, SO_LINGER, &l, &ll) == 0 && !l.l_onoff, "SO_LINGER reads as off");
    pthread_t t;
    pthread_create(&t, NULL, recv_thread, NULL);
    usleep(300000);
    double closed = now();
    close(g_sv[1]);
    for (int i = 0; i < 40 && g_got == -2; i++)
        usleep(50000);
    check(g_got == 0 && g_at - closed < 1.5, "a recvmsg blocked before the peer closed returns 0 (EOF)");
    if (g_got == -2) pthread_kill(t, SIGUSR1); else pthread_join(t, NULL);
    close(g_sv[0]);

    /* ppoll, as the zygote waits. */
    int sv[2];
    socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv);
    pid_t c = fork();
    if (c == 0) {
        usleep(300000);
        _exit(0);                       /* its copy of sv[1] closes here */
    }
    close(sv[1]);
    struct pollfd p = { sv[0], POLLIN, 0 };
    double t0 = now();
    int r = ppoll(&p, 1, NULL, NULL);
    double dt = now() - t0;
    check(r == 1 && (p.revents & POLLIN) && dt < 2.0, "a ppoll blocked before the peer's exit returns");
    check(recv(sv[0], buf, sizeof buf, 0) == 0, "then recv reads EOF");
    waitpid(c, NULL, 0);
    close(sv[0]);
}

static void on_usr1(int s) { (void)s; }

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--exit7"))
        return 7;
    signal(SIGUSR1, on_usr1);
    setvbuf(stdout, NULL, _IOLBF, 0);   /* lines survive an exec or a hang */
    /* electron_runtime [section]: one section only (a control run on an older
     * runtime dies in the first one otherwise). */
    const char *only = argc > 1 ? argv[1] : NULL;
#define SECTION(name, call) if (!only || !strcmp(only, name)) { printf(name "\n"); call; }
    SECTION("prlimit", test_prlimit());
    SECTION("mprotect", test_mprotect());
    SECTION("shm", test_shm());
    SECTION("execve", test_execve(argv[0]));
    SECTION("epoll", test_epoll());
    SECTION("seqpacket", test_seqpacket());
    if (mal)
        printf("== electron runtime: %d ok, %d MAL\n", oks, mal);
    else
        printf("== electron runtime: ok (%d checks)\n", oks);
    return mal ? 1 : 0;
}
