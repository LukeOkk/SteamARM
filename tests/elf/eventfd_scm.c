// An eventfd handed to another process over SCM_RIGHTS, as Wine's esync
// works: wineserver makes one eventfd per synchronization object and sends it
// to each client, which read()s, write()s and poll()s it with 8-byte values.
// The child here exec's afresh (no fork-inherited state), receives the
// eventfd, and the two sides signal each other through it.
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what) { printf("  %s  %s\n", ok ? "OK " : "MAL", what); fails += !ok; }

static int recv_fd(int s)
{
    char c;
    struct iovec io = { &c, 1 };
    char cb[CMSG_SPACE(sizeof(int))];
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
    if (recvmsg(s, &m, 0) != 1 || !CMSG_FIRSTHDR(&m)) return -1;
    int fd;
    memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&m)), sizeof fd);
    return fd;
}

static void send_fd(int s, int fd)
{
    char c = 'x';
    struct iovec io = { &c, 1 };
    char cb[CMSG_SPACE(sizeof(int))];
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET; h->cmsg_type = SCM_RIGHTS; h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof fd);
    sendmsg(s, &m, 0);
}

static int child(void)
{
    int efd = recv_fd(3), sfd = recv_fd(3);
    int bad = 0;
    // The parent wrote 3 before sending: read gives 3, the counter is then 0.
    uint64_t v = 0;
    bad += read(efd, &v, 8) != 8 || v != 3;
    struct pollfd p = { efd, POLLIN, 0 };
    bad += poll(&p, 1, 0) != 0;                    // empty now
    // Wait for the parent's signal on it.
    p.revents = 0;
    bad += poll(&p, 1, 5000) != 1 || !(p.revents & POLLIN);
    v = 0;
    bad += read(efd, &v, 8) != 8 || v != 7;
    // Signal back on a second, semaphore-mode eventfd: two posts, two reads of 1.
    v = 2;
    bad += write(sfd, &v, 8) != 8;
    return bad;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child"))
        return child();
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int sfd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK | EFD_SEMAPHORE);
    uint64_t v = 3;
    write(efd, &v, 8);
    pid_t kid = fork();
    if (kid == 0) {
        dup2(sp[1], 3);
        execl("/proc/self/exe", argv[0], "child", (char *)NULL);
        _exit(127);
    }
    send_fd(sp[0], efd);
    send_fd(sp[0], sfd);
    usleep(200000);
    v = 7;
    check(write(efd, &v, 8) == 8, "the parent signals the eventfd it sent");
    struct pollfd p = { sfd, POLLIN, 0 };
    int r = poll(&p, 1, 5000);
    uint64_t a = 0, b = 0, c = 0;
    int ra = read(sfd, &a, 8), rb = read(sfd, &b, 8);
    errno = 0;
    int rc = read(sfd, &c, 8);
    check(r == 1 && ra == 8 && a == 1 && rb == 8 && b == 1 && rc == -1 && errno == EAGAIN,
          "the child's posts on a semaphore eventfd it received: two reads of 1, then EAGAIN");
    int st = 0;
    waitpid(kid, &st, 0);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 0,
          "the child (exec'd, the eventfd received over SCM_RIGHTS) reads 3, polls, is woken, reads 7");
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails != 0;
}
