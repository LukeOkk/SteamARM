#define _GNU_SOURCE
#include <sys/syscall.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>
#include <string.h>

/* Build on Linux aarch64: gcc -static-pie -O2 tfd_sfd_test.c -o tfd_sfd_test */
#ifdef __aarch64__
_Static_assert(SYS_timerfd_create == 85 && SYS_timerfd_settime == 86 &&
               SYS_timerfd_gettime == 87 && SYS_signalfd4 == 74, "syscall ABI");
#endif
static int ok, mal;
#define CHECK(name, expr) do { if (expr) { ++ok; printf("  OK   %s\n", name); } \
    else { ++mal; printf("  MAL  %s (errno=%d)\n", name, errno); } } while (0)
static long set(int fd, int flags, long first, long repeat)
{
    struct itimerspec t = {{0, repeat}, {0, first}};
    return syscall(SYS_timerfd_settime, fd, flags, &t, NULL);
}
static void pause_ms(long ms) { struct timespec t = {ms / 1000, ms % 1000 * 1000000}; nanosleep(&t, NULL); }
int main(void)
{
    alarm(10); /* A blocking regression must fail rather than hang CI. */
    uint64_t count = 0;
    int fd = syscall(SYS_timerfd_create, CLOCK_MONOTONIC, TFD_CLOEXEC);
    CHECK("create", fd >= 0);
    CHECK("one-shot 50ms arm", set(fd, 0, 50000000, 0) == 0);
    struct timespec a,b; clock_gettime(CLOCK_MONOTONIC, &a);
    CHECK("one-shot 50ms read", read(fd, &count, 8) == 8 && count == 1);
    clock_gettime(CLOCK_MONOTONIC, &b);
    CHECK("one-shot elapsed", (b.tv_sec-a.tv_sec)*1000000000LL+b.tv_nsec-a.tv_nsec >= 35000000);
    CHECK("periodic 10ms arm", set(fd, 0, 10000000, 10000000) == 0);
    pause_ms(65);
    CHECK("periodic accumulated", read(fd, &count, 8) == 8 && count >= 5);
    struct itimerspec t;
    CHECK("gettime", syscall(SYS_timerfd_gettime, fd, &t) == 0 && t.it_interval.tv_nsec == 10000000 && t.it_value.tv_sec == 0 && t.it_value.tv_nsec <= 10000000);
    CHECK("disarm", set(fd, 0, 0, 0) == 0 && syscall(SYS_timerfd_gettime, fd, &t) == 0 && !t.it_value.tv_sec && !t.it_value.tv_nsec);
    close(fd);
    fd = syscall(SYS_timerfd_create, CLOCK_MONOTONIC, TFD_NONBLOCK);
    CHECK("timer NONBLOCK", read(fd, &count, 8) == -1 && errno == EAGAIN);
    clock_gettime(CLOCK_MONOTONIC, &a);
    memset(&t,0,sizeof(t)); t.it_value = a; t.it_value.tv_nsec += 30000000;
    if (t.it_value.tv_nsec >= 1000000000) { ++t.it_value.tv_sec; t.it_value.tv_nsec -= 1000000000; }
    CHECK("ABSTIME", syscall(SYS_timerfd_settime, fd, TFD_TIMER_ABSTIME, &t, NULL) == 0);
    int ep = epoll_create1(0); struct epoll_event e = {.events=EPOLLIN,.data.u64=123}, got;
    CHECK("timer epoll add", epoll_ctl(ep, EPOLL_CTL_ADD, fd, &e) == 0);
    CHECK("timer epoll", epoll_wait(ep, &got, 1, 500) == 1 && got.data.u64 == 123 && (got.events & EPOLLIN));
    CHECK("ABSTIME read", read(fd,&count,8) == 8 && count == 1);
    CHECK("timer drained", epoll_wait(ep,&got,1,0) == 0);
    CHECK("invalid nsec", set(fd,0,1000000000,0) == -1 && errno == EINVAL);
    close(fd); close(ep);
    uint64_t mask = UINT64_C(1) << (SIGUSR1-1);
    CHECK("block SIGUSR1", syscall(SYS_rt_sigprocmask, SIG_BLOCK, &mask, NULL, 8) == 0);
    fd = syscall(SYS_signalfd4, -1, &mask, 8, SFD_NONBLOCK|SFD_CLOEXEC);
    struct signalfd_siginfo si;
    CHECK("signalfd NONBLOCK", fd >= 0 && read(fd,&si,sizeof(si)) == -1 && errno == EAGAIN);
    ep = epoll_create1(0); e.data.u64=456;
    CHECK("signal epoll add", epoll_ctl(ep,EPOLL_CTL_ADD,fd,&e) == 0);
    CHECK("send SIGUSR1", kill(getpid(),SIGUSR1) == 0);
    CHECK("signal epoll", epoll_wait(ep,&got,1,500) == 1 && got.data.u64 == 456);
    CHECK("signalfd SIGUSR1", read(fd,&si,sizeof(si)) == sizeof(si) && si.ssi_signo == SIGUSR1);
    CHECK("signal consumed", read(fd,&si,sizeof(si)) == -1 && errno == EAGAIN);
    CHECK("signal drained", epoll_wait(ep,&got,1,0) == 0);
    close(fd); close(ep);
    syscall(SYS_rt_sigprocmask,SIG_UNBLOCK,&mask,NULL,8);
    printf("== %d ok, %d mal\n",ok,mal); return mal ? 1 : 0;
}
