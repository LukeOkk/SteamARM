// FIONREAD on an inotify descriptor, the way Qt reads one.
//
// Qt's QInotifyFileSystemWatcherEngine::readFromInotify (qtbase
// src/corelib/io/qfilesystemwatcher_inotify.cpp, v6.10.2) asks FIONREAD how
// many bytes of events are queued and reads exactly that many, on a blocking
// descriptor (inotify_init1(IN_CLOEXEC)). On Linux FIONREAD is the size of
// the queued events. lxrun's inotify is a pipe carrying one readiness byte,
// and FIONREAD used to reach that pipe: 1 while events were queued, so Qt's
// read(fd, buf, 1) got EINVAL (no whole event fits), and a readiness byte
// left without an event made Qt block in read() on its GUI thread: Prism
// Launcher's window stayed blank (benchmarks/stage24-minecraft-prism.txt).
//
// Linux prints "== inotify fionread: ok" too.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails;
static void check(int ok, const char *what)
{
    printf("  %s  %s\n", ok ? "OK  " : "FAIL", what);
    if (!ok) fails++;
}

static void on_alarm(int s) { (void)s; }

int main(void)
{
    char dir[] = "/tmp/lxrt-ino-fionread-XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    struct sigaction sa = {0};
    sa.sa_handler = on_alarm;          // no SA_RESTART: a stuck read returns EINTR
    sigaction(SIGALRM, &sa, NULL);

    int fd = inotify_init1(IN_CLOEXEC);   // blocking, as Qt makes it
    check(fd >= 0, "inotify_init1(IN_CLOEXEC)");
    int wd = inotify_add_watch(fd, dir, IN_ATTRIB | IN_MOVE | IN_MOVE_SELF | IN_DELETE |
                                        IN_DELETE_SELF | IN_MODIFY | IN_CREATE);
    check(wd > 0, "add_watch on a directory");

    int n = -1;
    check(ioctl(fd, FIONREAD, &n) == 0 && n == 0, "FIONREAD is 0 with nothing queued");

    char path[256];
    snprintf(path, sizeof path, "%s/f", dir);
    int f = open(path, O_CREAT | O_WRONLY, 0644);
    close(f);
    struct pollfd p = {fd, POLLIN, 0};
    check(poll(&p, 1, 3000) == 1 && (p.revents & POLLIN), "readable after a create");

    n = -1;
    int r = ioctl(fd, FIONREAD, &n);
    // One IN_CREATE for "f": 16 bytes of event, the name padded to 16.
    printf("  FIONREAD after one create: %d (ioctl %d)\n", n, r);
    check(r == 0 && n >= 16 && n % 16 == 0, "FIONREAD is the size of the queued events");

    // Qt: read exactly FIONREAD bytes.
    char buf[4096];
    alarm(3);
    long got = n > 0 ? read(fd, buf, (size_t)n) : -2;
    int err = errno;
    alarm(0);
    printf("  read(fd, buf, %d) = %ld%s%s\n", n, got, got < 0 ? " errno " : "", got < 0 ? strerror(err) : "");
    check(got == n, "read(FIONREAD bytes) returns them, without blocking");
    if (got >= (long)sizeof(struct inotify_event)) {
        struct inotify_event *e = (struct inotify_event *)buf;
        check(e->wd == wd && (e->mask & IN_CREATE) && e->len && !strcmp(e->name, "f"),
              "the event is IN_CREATE \"f\"");
    }

    n = -1;
    check(ioctl(fd, FIONREAD, &n) == 0 && n == 0, "FIONREAD is 0 once everything is read");
    p.revents = 0;
    check(poll(&p, 1, 200) == 0, "not readable once everything is read");

    // Two events, then a buffer that holds only the first: the second stays
    // queued, and FIONREAD says how big it is.
    unlink(path);
    snprintf(path, sizeof path, "%s/g", dir);
    f = open(path, O_CREAT | O_WRONLY, 0644);
    close(f);
    usleep(300000);
    n = -1;
    ioctl(fd, FIONREAD, &n);
    printf("  FIONREAD after delete + create: %d\n", n);
    check(n >= 64 && n % 16 == 0, "FIONREAD counts every queued event");
    alarm(3);
    got = read(fd, buf, 32);
    alarm(0);
    check(got == 32, "a 32-byte read returns one whole event");
    int rest = -1;
    ioctl(fd, FIONREAD, &rest);
    check(rest == n - 32, "FIONREAD then counts what is left");
    alarm(3);
    got = read(fd, buf, sizeof buf);
    alarm(0);
    check(got == rest, "the rest reads in one go");

    close(fd);
    unlink(path);
    rmdir(dir);
    printf("== inotify fionread: %s\n", fails ? "FAIL" : "ok");
    return fails != 0;
}
