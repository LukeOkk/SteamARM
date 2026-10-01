// /dev/ntsync (runtime/ntsync.c, LXRT_NTSYNC=1) the way Wine uses it: one
// process makes the objects and hands their fds to another over a Unix
// socket (SCM_RIGHTS), which waits on them while the first signals. Also the
// semantics games depend on: a semaphore handed to its waiter before the
// release returns, wait-all taking every object or none, an abandoned mutex,
// auto-reset events, alerts and timeouts. Prints "  OK  name" per check and
// PASS at the end (tests/elf/run.sh).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

struct sem_args { uint32_t count, max; };
struct mutex_args { uint32_t owner, count; };
struct event_args { uint32_t manual, signaled; };
struct wait_args { uint64_t timeout, objs; uint32_t count, index, flags, owner, alert, pad; };
#define IOC(dir, nr, size) (((uint32_t)(dir) << 30) | ((uint32_t)(size) << 16) | ('N' << 8) | (nr))
#define CREATE_SEM   IOC(1, 0x80, 8)
#define SEM_RELEASE  IOC(3, 0x81, 4)
#undef WAIT_ANY                                 // <sys/wait.h> has one too
#define WAIT_ANY     IOC(3, 0x82, 40)
#define WAIT_ALL     IOC(3, 0x83, 40)
#define CREATE_MUTEX IOC(1, 0x84, 8)
#define MUTEX_UNLOCK IOC(3, 0x85, 8)
#define MUTEX_KILL   IOC(1, 0x86, 4)
#define CREATE_EVENT IOC(1, 0x87, 8)
#define EVENT_SET    IOC(2, 0x88, 4)
#define SEM_READ     IOC(2, 0x8b, 8)
#define MUTEX_READ   IOC(2, 0x8c, 8)
#define EVENT_READ   IOC(2, 0x8d, 8)

static int failures;
static void check(int ok, const char *name)
{
    printf("  %s  %s\n", ok ? "OK " : "MAL", name);
    if (!ok)
        failures++;
}

static uint64_t abs_ms(int ms)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec + (uint64_t)ms * 1000000ull;
}

static int wait_on(int dev, unsigned long req, const int *objs, uint32_t n, uint32_t owner, int alert,
                   int ms, uint32_t *index)
{
    struct wait_args a = { ms < 0 ? UINT64_MAX : abs_ms(ms), (uintptr_t)objs, n, 0xdeadbeef, 0, owner,
                           (uint32_t)alert, 0 };
    int r = ioctl(dev, req, &a);
    if (index)
        *index = a.index;
    return r < 0 ? -errno : r;
}

static uint32_t sem_count(int s)
{
    struct sem_args a = { 99, 99 };
    ioctl(s, SEM_READ, &a);
    return a.count;
}

static int send_fds(int sock, const int *fds, int n)
{
    char c = 'x';
    struct iovec iov = { &c, 1 };
    char buf[CMSG_SPACE(4 * sizeof(int))];
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = buf,
                        .msg_controllen = CMSG_SPACE(n * sizeof(int)) };
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(n * sizeof(int));
    memcpy(CMSG_DATA(cm), fds, n * sizeof(int));
    return sendmsg(sock, &m, 0) == 1 ? 0 : -1;
}

static int recv_fds(int sock, int *fds, int n)
{
    char c;
    struct iovec iov = { &c, 1 };
    char buf[CMSG_SPACE(4 * sizeof(int))];
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = buf, .msg_controllen = sizeof buf };
    if (recvmsg(sock, &m, 0) != 1)
        return -1;
    struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
    if (!cm || cm->cmsg_type != SCM_RIGHTS || cm->cmsg_len != CMSG_LEN(n * sizeof(int)))
        return -1;
    memcpy(fds, CMSG_DATA(cm), n * sizeof(int));
    return 0;
}

int main(void)
{
    int dev = open("/dev/ntsync", O_RDONLY | O_CLOEXEC);
    if (dev < 0) {
        printf("no /dev/ntsync: %s (LXRT_NTSYNC=1?)\nFAIL\n", strerror(errno));
        return 1;
    }
    uint32_t idx, prev;

    // Semaphore: count, overflow, a wait takes one, an empty one times out.
    struct sem_args sa = { 1, 2 };
    int sem = ioctl(dev, CREATE_SEM, &sa);
    prev = 1;
    check(sem >= 0 && ioctl(sem, SEM_RELEASE, &prev) == 0 && prev == 1 && sem_count(sem) == 2, "semaphore release");
    prev = 1;
    check(ioctl(sem, SEM_RELEASE, &prev) < 0 && errno == EOVERFLOW && sem_count(sem) == 2, "semaphore over max");
    check(wait_on(dev, WAIT_ANY, &sem, 1, 1, 0, 0, &idx) == 0 && idx == 0 && sem_count(sem) == 1, "wait takes one");
    wait_on(dev, WAIT_ANY, &sem, 1, 1, 0, 0, NULL);
    check(wait_on(dev, WAIT_ANY, &sem, 1, 1, 0, 30, NULL) == -ETIMEDOUT, "empty semaphore times out");

    // Mutex: recursion, the wrong owner, an abandoned one.
    struct mutex_args ma = { 0, 0 };
    int mtx = ioctl(dev, CREATE_MUTEX, &ma);
    check(wait_on(dev, WAIT_ANY, &mtx, 1, 7, 0, 0, NULL) == 0 && wait_on(dev, WAIT_ANY, &mtx, 1, 7, 0, 0, NULL) == 0,
          "mutex taken twice by its owner");
    check(wait_on(dev, WAIT_ANY, &mtx, 1, 8, 0, 0, NULL) == -ETIMEDOUT, "mutex refused to another owner");
    struct mutex_args un = { 8, 0 };
    check(ioctl(mtx, MUTEX_UNLOCK, &un) < 0 && errno == EPERM, "unlock by another owner");
    uint32_t owner = 7;
    check(ioctl(mtx, MUTEX_KILL, &owner) == 0 && wait_on(dev, WAIT_ANY, &mtx, 1, 9, 0, 0, &idx) == -EOWNERDEAD &&
          idx == 0, "abandoned mutex");
    struct mutex_args mr;
    check(ioctl(mtx, MUTEX_READ, &mr) == 0 && mr.owner == 9 && mr.count == 1, "abandoned mutex now owned");

    // Wait-all takes every object or none.
    struct event_args ea = { 1, 0 };
    int ev = ioctl(dev, CREATE_EVENT, &ea);
    sa = (struct sem_args){ 1, 1 };
    int s2 = ioctl(dev, CREATE_SEM, &sa);
    int both[2] = { s2, ev };
    check(wait_on(dev, WAIT_ALL, both, 2, 1, 0, 30, NULL) == -ETIMEDOUT && sem_count(s2) == 1,
          "wait-all with one unsignaled takes nothing");
    ioctl(ev, EVENT_SET, &prev);
    check(wait_on(dev, WAIT_ALL, both, 2, 1, 0, 0, NULL) == 0 && sem_count(s2) == 0, "wait-all takes all");

    // Alert: an auto-reset event after the objects.
    ea = (struct event_args){ 0, 1 };
    int alert = ioctl(dev, CREATE_EVENT, &ea);
    struct event_args er;
    check(wait_on(dev, WAIT_ANY, &s2, 1, 1, alert, 0, &idx) == 0 && idx == 1 &&
          ioctl(alert, EVENT_READ, &er) == 0 && er.signaled == 0, "alert wakes, auto-reset");

    // Across processes, as Wine's server and its clients: the child gets the
    // device and two objects over SCM_RIGHTS and waits; the parent signals.
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    sa = (struct sem_args){ 0, 5 };
    int xsem = ioctl(dev, CREATE_SEM, &sa);
    ea = (struct event_args){ 0, 0 };
    int xev = ioctl(dev, CREATE_EVENT, &ea);
    pid_t pid = fork();
    if (pid == 0) {
        close(sv[0]);
        int fds[3];
        if (recv_fds(sv[1], fds, 3) != 0)
            _exit(10);
        char go = 'g';
        write(sv[1], &go, 1);
        uint32_t i2;
        int w = wait_on(fds[0], WAIT_ANY, &fds[1], 1, 42, 0, 5000, &i2);
        if (w != 0 || i2 != 0) {
            fprintf(stderr, "  child: wait on the semaphore: %d (%s), index %u\n", w, strerror(-w), i2);
            _exit(11);                           // the semaphore, released by the parent
        }
        write(sv[1], &go, 1);
        if (wait_on(fds[0], WAIT_ANY, &fds[2], 1, 42, 0, 5000, &i2) != 0 || i2 != 0)
            _exit(12);                           // the event, set by the parent
        _exit(0);
    }
    close(sv[1]);
    int out[3] = { dev, xsem, xev };
    char got;
    check(send_fds(sv[0], out, 3) == 0 && read(sv[0], &got, 1) == 1, "fds handed over SCM_RIGHTS");
    usleep(100000);                              // the child is waiting by now
    prev = 1;                                    // in: the count to add; out: the count before
    int rel = ioctl(xsem, SEM_RELEASE, &prev);
    // The kernel hands the count to the waiting child before the release
    // returns: it reads 0 here at once.
    check(rel == 0 && prev == 0 && sem_count(xsem) == 0, "release handed to the waiter in another process");
    check(read(sv[0], &got, 1) == 1, "child woken by the semaphore");
    usleep(50000);
    ioctl(xev, EVENT_SET, &prev);
    int st = 0;
    waitpid(pid, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child woken by the event");
    if (!WIFEXITED(st) || WEXITSTATUS(st))
        printf("  (child exit %d)\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    ioctl(xev, EVENT_READ, &er);
    check(er.signaled == 0, "auto-reset event taken by the child");

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
