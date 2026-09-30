// memfd seals bind a process that received the descriptor. The parent
// creates a memfd, seals it F_SEAL_FUTURE_WRITE (what ASharedMemory_setProt
// does for a read-only region) and passes it with SCM_RIGHTS to a child that
// exec'd afresh; the child must read the seals back and fail to map it
// writable and shared (EPERM), while a read-only mapping works. Chromium's
// WebView renderer checks exactly that before it maps a read-only region.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef F_SEAL_FUTURE_WRITE
#define F_SEAL_FUTURE_WRITE 0x0010
#endif

static int child(void)
{
    char c;
    struct iovec io = { &c, 1 };
    char cb[CMSG_SPACE(sizeof(int))];
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
    if (recvmsg(3, &m, 0) != 1) { printf("  MAL  recvmsg\n"); return 1; }
    int fd;
    memcpy(&fd, CMSG_DATA(CMSG_FIRSTHDR(&m)), sizeof fd);
    int fails = 0;
    int seals = fcntl(fd, F_GET_SEALS);
    int ok = seals >= 0 && (seals & F_SEAL_FUTURE_WRITE);
    printf("  %s  the receiving process reads the seals: 0x%x\n", ok ? "OK " : "MAL", seals);
    fails += !ok;
    void *w = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ok = w == MAP_FAILED && errno == EPERM;
    printf("  %s  it cannot map the region writable and shared (EPERM)\n", ok ? "OK " : "MAL");
    fails += !ok;
    char *r = mmap(NULL, 4096, PROT_READ, MAP_SHARED, fd, 0);
    ok = r != MAP_FAILED && !memcmp(r, "sealed", 6);
    printf("  %s  a read-only mapping works and shows the contents\n", ok ? "OK " : "MAL");
    fails += !ok;
    return fails != 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "child"))
        return child();
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    pid_t p = fork();
    if (p == 0) {
        dup2(sp[1], 3);
        execl("/proc/self/exe", argv[0], "child", (char *)NULL);
        execl(argv[0], argv[0], "child", (char *)NULL);
        _exit(127);
    }
    int fd = memfd_create("region", MFD_ALLOW_SEALING);
    write(fd, "sealed", 6);
    ftruncate(fd, 4096);
    int r = fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE | F_SEAL_SHRINK | F_SEAL_GROW);
    char c = 'x';
    struct iovec io = { &c, 1 };
    char cb[CMSG_SPACE(sizeof(int))];
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof cb };
    struct cmsghdr *h = CMSG_FIRSTHDR(&m);
    h->cmsg_level = SOL_SOCKET;
    h->cmsg_type = SCM_RIGHTS;
    h->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(h), &fd, sizeof fd);
    sendmsg(sp[0], &m, 0);
    int st = 0;
    waitpid(p, &st, 0);
    int good = r == 0 && WIFEXITED(st) && WEXITSTATUS(st) == 0;
    printf("%s\n", good ? "PASS" : "FAIL");
    return !good;
}
