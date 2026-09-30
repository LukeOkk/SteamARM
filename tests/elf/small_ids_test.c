#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int failures;
static void check(int good, const char *name)
{
    printf("  %s  %s\n", good ? "OK " : "MAL", name);
    if (!good) failures++;
    fflush(stdout);
}

static atomic_int ready, stop_threads, signaled_tid;
static int tids[8];
static void usr2(int sig)
{
    (void)sig;
    atomic_store(&signaled_tid, (int)syscall(SYS_gettid));
}
static void *worker(void *arg)
{
    int n = (int)(long)arg;
    tids[n] = (int)syscall(SYS_gettid);
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&stop_threads)) usleep(1000);
    return NULL;
}
static volatile sig_atomic_t got_usr1;
static void usr1(int sig) { (void)sig; got_usr1 = 1; }

int main(void)
{
    setbuf(stdout, NULL);
    int pid = getpid(), tid = (int)syscall(SYS_gettid);
    check(pid >= 2 && pid <= 65535 && tid == pid, "main pid and tid small and equal");

    struct sigaction sa = { .sa_handler = usr2 };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
    pthread_t threads[8];
    for (long i = 0; i < 8; i++)
        if (pthread_create(&threads[i], NULL, worker, (void *)i)) return 2;
    while (atomic_load(&ready) != 8) usleep(1000);
    int unique = 1;
    for (int i = 0; i < 8; i++) {
        if (tids[i] < 2 || tids[i] > 65535 || tids[i] == pid) unique = 0;
        for (int j = 0; j < i; j++) if (tids[i] == tids[j]) unique = 0;
    }
    check(unique, "eight distinct small thread tids");
    check(syscall(SYS_tgkill, pid, tids[3], SIGUSR2) == 0, "tgkill accepts guest tid");
    for (int i = 0; i < 1000 && !atomic_load(&signaled_tid); i++) usleep(1000);
    check(atomic_load(&signaled_tid) == tids[3], "tgkill reaches selected thread");
    atomic_store(&stop_threads, 1);
    for (int i = 0; i < 8; i++) pthread_join(threads[i], NULL);

    FILE *st = fopen("/proc/self/status", "r");
    int p = -1, t = -1;
    char line[256];
    if (st) {
        while (fgets(line, sizeof line, st)) {
            if (sscanf(line, "Pid:%d", &p) == 1) continue;
            sscanf(line, "Tgid:%d", &t);
        }
        fclose(st);
    }
    if (p != pid || t != pid)
        fprintf(stderr, "status: pid=%d Pid=%d Tgid=%d\n", pid, p, t);
    check(p == pid && t == pid, "/proc/self/status Pid and Tgid");

    int s[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, s)) return 3;
    int one = 1;
    setsockopt(s[0], SOL_SOCKET, SO_PASSCRED, &one, sizeof one);
    pid_t child = fork();
    if (child == 0) {
        close(s[0]);
        struct sigaction a = { .sa_handler = usr1 };
        sigemptyset(&a.sa_mask);
        sigaction(SIGUSR1, &a, NULL);
        int good = getppid() == pid && getpid() >= 2 && getpid() <= 65535;
        char c = good ? 'Y' : 'N';
        send(s[1], &c, 1, 0);
        for (int i = 0; i < 5000 && !got_usr1; i++) usleep(1000);
        _exit(good && got_usr1 ? 42 : 43);
    }
    close(s[1]);
    check(child >= 2 && child <= 65535, "fork returns small child pid");
    char path[80];
    snprintf(path, sizeof path, "/proc/%d/stat", child);
    FILE *stat = fopen(path, "r");
    int stat_pid = -1;
    if (stat) { fscanf(stat, "%d", &stat_pid); fclose(stat); }
    check(stat_pid == child, "/proc/<child>/stat uses guest pid");
    char data = 0, control[CMSG_SPACE(sizeof(struct ucred))];
    struct iovec io = { &data, 1 };
    struct msghdr msg = { .msg_iov = &io, .msg_iovlen = 1,
                          .msg_control = control, .msg_controllen = sizeof control };
    int n = recvmsg(s[0], &msg, 0);
    int cred_pid = -1;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c))
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_CREDENTIALS)
            cred_pid = ((struct ucred *)CMSG_DATA(c))->pid;
    check(n == 1 && data == 'Y', "child sees parent guest pid");
    check(cred_pid == child, "SCM_CREDENTIALS carries guest pid");
    check(kill(child, SIGUSR1) == 0, "kill accepts guest pid");
    int status = 0;
    pid_t waited = waitpid(child, &status, 0);
    check(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 42,
          "waitpid returns guest pid and exit status");
    close(s[0]);

    int loop_good = 1;
    for (int i = 0; i < 200; i++) {
        pid_t c = fork();
        if (c == 0) _exit(0);
        if (c < 2 || c > 65535) { loop_good = 0; break; }
        if (waitpid(c, &status, 0) != c || !WIFEXITED(status)) { loop_good = 0; break; }
    }
    check(loop_good, "200 exiting children do not exhaust IDs");
    printf("== small ids: %d mal\n", failures);
    return failures ? 1 : 0;
}
