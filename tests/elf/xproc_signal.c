// Signals between processes, the way wineserver uses them: tgkill to ONE
// thread of another process (SIGUSR1 to a client thread) and realtime
// signals, which Darwin does not have (runtime/signal.c, mailbox + carrier).
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static _Atomic int got_usr1_tid, got_rt, got_rt_local;
static pid_t worker_tid;

static void on_usr1(int s) { (void)s; got_usr1_tid = (int)syscall(SYS_gettid); }
static void on_rt(int s) { if (s == SIGRTMIN + 3) got_rt++; }
static void on_rt_local(int s) { if (s == SIGRTMIN + 5) got_rt_local++; }

static void *worker(void *fdp)
{
    int fd = *(int *)fdp;
    worker_tid = (pid_t)syscall(SYS_gettid);
    if (write(fd, &worker_tid, sizeof worker_tid) != sizeof worker_tid) return NULL;
    for (int i = 0; i < 500 && !got_usr1_tid; i++) usleep(10000);
    return NULL;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int p[2], q[2];
    if (pipe(p) || pipe(q)) return 2;
    pid_t child = fork();
    if (child == 0) {
        signal(SIGUSR1, on_usr1);
        signal(SIGRTMIN + 3, on_rt);
        pthread_t t;
        pthread_create(&t, NULL, worker, &p[1]);
        pthread_join(t, NULL);
        for (int i = 0; i < 300 && !got_rt; i++) usleep(10000);
        int res[2] = { got_usr1_tid == worker_tid && worker_tid != getpid(), got_rt == 1 };
        if (write(q[1], res, sizeof res) != sizeof res) return 3;
        _exit(0);
    }
    pid_t tid = 0;
    if (read(p[0], &tid, sizeof tid) != sizeof tid) return 4;
    long r1 = syscall(SYS_tgkill, child, tid, SIGUSR1);
    usleep(200000);
    long r2 = kill(child, SIGRTMIN + 3);
    int res[2] = {0, 0};
    if (read(q[0], res, sizeof res) != sizeof res) return 5;
    waitpid(child, NULL, 0);

    signal(SIGRTMIN + 5, on_rt_local);
    long r3 = syscall(SYS_tgkill, getpid(), (pid_t)syscall(SYS_gettid), SIGRTMIN + 5);
    for (int i = 0; i < 100 && !got_rt_local; i++) usleep(10000);

    printf("tgkill(other process, thread %d, SIGUSR1) = %ld, handled on that thread: %d\n", tid, r1, res[0]);
    printf("kill(other process, SIGRTMIN+3) = %ld, handled: %d\n", r2, res[1]);
    printf("tgkill(self, SIGRTMIN+5) = %ld, handled: %d\n", r3, (int)got_rt_local);
    int ok = r1 == 0 && res[0] && r2 == 0 && res[1] && r3 == 0 && got_rt_local == 1;
    printf(ok ? "== xproc signal: ok\n" : "== xproc signal: FAIL\n");
    return !ok;
}
