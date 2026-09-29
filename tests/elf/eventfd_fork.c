// An eventfd is one object in the parent and in the children it forks, as
// on Linux: what one side writes, the other reads.
//
// Qt's QProcess starts every program through forkfd (qtbase
// src/3rdparty/forkfd/forkfd.c): the child blocks in eventfd_read() until the
// parent's eventfd_write() says it may go on to exec. lxrun kept the counter
// in process memory, so after fork the child's copy stayed 0 while the shared
// token pipe said "readable", and the child spun at 100% CPU for ever: Prism
// Launcher's updater and Java checks never started
// (benchmarks/stage24-minecraft-prism.txt).
//
// Linux prints "== eventfd fork: ok" too.
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "OK  " : "FAIL", what);
    fflush(stdout);
    if (!ok) fails++;
}

static double now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int child_status(pid_t pid)
{
    int st = 0;
    if (waitpid(pid, &st, 0) != pid) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
}

int main(void)
{
    // 1. forkfd's hand-shake: the child waits for the parent's all-clear.
    int efd = eventfd(0, EFD_CLOEXEC);
    check(efd >= 0, "eventfd(0, EFD_CLOEXEC)");
    double t0 = now();
    pid_t pid = fork();
    if (pid == 0) {
        alarm(5);                       // a child that never hears it dies here
        eventfd_t v = 0;
        int r = eventfd_read(efd, &v);
        _exit(r == 0 && v == 42 ? 0 : 1);
    }
    usleep(100000);
    check(eventfd_write(efd, 42) == 0, "parent writes 42");
    int st = child_status(pid);
    double dt = now() - t0;
    printf("  child exit %d after %.2f s\n", st, dt);
    check(st == 0 && dt < 3, "the blocked child reads the parent's 42 and exits");

    // 1b. Exactly forkfd's order: the parent writes and closes at once,
    // usually before the child has run at all.
    for (int round = 0; round < 20; round++) {
        int e2 = eventfd(0, EFD_CLOEXEC);
        pid = fork();
        if (pid == 0) {
            alarm(5);
            eventfd_t v = 0;
            int r = eventfd_read(e2, &v);
            _exit(r == 0 && v == 42 ? 0 : 1);
        }
        eventfd_write(e2, 42);
        close(e2);
        st = child_status(pid);
        if (st != 0) {
            printf("  round %d: child exit %d\n", round, st);
            break;
        }
        if (round == 19)
            st = 0;
    }
    check(st == 0, "20 x write-then-close in the parent: every child reads 42");

    // 2. A count written before fork is there once, for whichever reads first.
    int nb = eventfd(0, EFD_NONBLOCK);
    eventfd_write(nb, 5);
    pid = fork();
    if (pid == 0) {
        eventfd_t v = 0;
        _exit(eventfd_read(nb, &v) == 0 && v == 5 ? 0 : 1);
    }
    st = child_status(pid);
    check(st == 0, "the child reads the 5 written before fork");
    eventfd_t v = 0;
    errno = 0;
    int r = eventfd_read(nb, &v);
    check(r < 0 && errno == EAGAIN, "then the parent finds it consumed (EAGAIN)");

    // 3. The child writes, the parent polls and reads.
    pid = fork();
    if (pid == 0) {
        eventfd_write(nb, 7);
        _exit(0);
    }
    child_status(pid);
    struct pollfd p = {nb, POLLIN, 0};
    check(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN), "readable in the parent after the child's write");
    v = 0;
    check(eventfd_read(nb, &v) == 0 && v == 7, "the parent reads the child's 7");
    p.revents = 0;
    check(poll(&p, 1, 100) == 0, "and it is no longer readable");

    // 4. The child closing its copy leaves the parent's eventfd working.
    pid = fork();
    if (pid == 0) {
        close(nb);
        _exit(0);
    }
    child_status(pid);
    eventfd_write(nb, 1);
    v = 0;
    check(eventfd_read(nb, &v) == 0 && v == 1, "the parent's eventfd works after the child closed its copy");

    close(efd);
    close(nb);
    printf("== eventfd fork: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
