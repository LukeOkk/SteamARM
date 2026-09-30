// What booting Android's framework to an app needed from the runtime, stage
// 28 (benchmarks/stage28-android-apk.txt), checked with plain Linux calls:
//
//   futex      a FUTEX_WAIT_BITSET deadline past 64-bit nanoseconds waits
//              (bionic's Condition::waitRelative(INT64_MAX) asks {LONG_MAX, n};
//              audioserver's TimeCheck thread aborted when it came back at once)
//   timerfd    CLOCK_REALTIME_ALARM / CLOCK_BOOTTIME_ALARM (AlarmManagerService),
//              CAP_WAKE_ALARM required as on Linux; TFD_TIMER_CANCEL_ON_SET taken
//   sockopt    SO_DOMAIN and SO_PROTOCOL (BlockGuardOs asks SO_DOMAIN of every
//              accepted socket; the zygote died on ENOPROTOOPT)
//   scm        an SCM_RIGHTS with no descriptor attaches nothing, and
//              MSG_TRUNC|MSG_CTRUNC passed to recvmsg are not echoed back
//              (libbase's ReceiveFileDescriptorVector, every LocalSocket read)
//   xattr      "user." attributes (UserDataPreparer destroyed /data/user_de/0
//              when getxattr of user.serial failed); other namespaces ENOTSUP
//   pvm        process_vm_readv/writev on itself (ART's SafeCopy: without it
//              no NullPointerException from compiled code was caught)
//   boottime   an ABSTIME CLOCK_BOOTTIME timerfd measured against BOOTTIME
//              (AlarmManager's elapsed alarms fired seconds early against
//              MONOTONIC: "Too many (100000) false wakeups")
//   sendfile   sendfile and splice (FileUtils.copy: pm install's staging copy)
//
//   android_boot_rt          without Android ids: the alarm clocks are EPERM
//   android_boot_rt ids      with LXRT_ANDROID_IDS=root: the alarm clocks work
// Each check prints "ok" or "MAL"; the last line is "== android boot rt: ok".
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/futex.h>
#include <stdlib.h>
#include <sys/xattr.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/sendfile.h>
#include <sys/timerfd.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifndef SO_PROTOCOL
#define SO_PROTOCOL 38
#endif
#ifndef SO_DOMAIN
#define SO_DOMAIN 39
#endif
#ifndef CLOCK_REALTIME_ALARM
#define CLOCK_REALTIME_ALARM 8
#endif
#ifndef CLOCK_BOOTTIME_ALARM
#define CLOCK_BOOTTIME_ALARM 9
#endif
#ifndef TFD_TIMER_CANCEL_ON_SET
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)
#endif

static int bad;
static void check(int cond, const char *what)
{
    printf("  %s  %s\n", cond ? "ok " : "MAL", what);
    if (!cond) bad++;
}

static uint32_t word;
static void *waker(void *arg)
{
    (void)arg;
    struct timespec d = { 0, 150 * 1000 * 1000 };
    nanosleep(&d, NULL);
    __atomic_store_n(&word, 1, __ATOMIC_SEQ_CST);
    syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    return NULL;
}

static double now_s(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec / 1e9;
}

static int sockopt(int fd, int opt)
{
    int v = -1;
    socklen_t l = sizeof v;
    return getsockopt(fd, SOL_SOCKET, opt, &v, &l) == 0 && l == 4 ? v : -1000 - errno;
}

int main(int argc, char **argv)
{
    int ids = argc > 1 && !strcmp(argv[1], "ids");

    // futex: a deadline too far to represent is no deadline.
    pthread_t t;
    pthread_create(&t, NULL, waker, NULL);
    struct timespec far = { INT64_MAX, 0 };
    double t0 = now_s();
    long r = syscall(SYS_futex, &word, FUTEX_WAIT_BITSET_PRIVATE, 0, &far, NULL, FUTEX_BITSET_MATCH_ANY);
    int e = errno;
    double dt = now_s() - t0;
    pthread_join(t, NULL);
    char msg[160];
    snprintf(msg, sizeof msg, "FUTEX_WAIT_BITSET until {INT64_MAX, 0}: woken by the other thread after %.0f ms (r=%ld%s)",
             dt * 1000, r, r ? (e == ETIMEDOUT ? " ETIMEDOUT" : " error") : "");
    check((r == 0 || (r == -1 && e == EAGAIN)) && dt > 0.1, msg);
    struct timespec farn = { INT64_MAX / 1000000000 + 1, 999999999 };
    word = 0;
    pthread_create(&t, NULL, waker, NULL);
    t0 = now_s();
    r = syscall(SYS_futex, &word, FUTEX_WAIT_BITSET_PRIVATE | FUTEX_CLOCK_REALTIME, 0, &farn, NULL,
                FUTEX_BITSET_MATCH_ANY);
    dt = now_s() - t0;
    pthread_join(t, NULL);
    check(r == 0 && dt > 0.1, "... and on CLOCK_REALTIME with {INT64_MAX/1e9 + 1, 999999999}");

    // timerfd: the alarm clocks.
    int fr = timerfd_create(CLOCK_REALTIME_ALARM, TFD_NONBLOCK);
    int er = errno;
    int fb = timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_NONBLOCK);
    int eb = errno;
    if (ids) {
        check(fr >= 0 && fb >= 0, "timerfd_create(CLOCK_REALTIME_ALARM / CLOCK_BOOTTIME_ALARM) with CAP_WAKE_ALARM");
        struct itimerspec zero = { { 0, 0 }, { 0, 0 } };
        check(fr >= 0 && timerfd_settime(fr, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &zero, NULL) == 0,
              "timerfd_settime(TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET) as AlarmManagerService does");
        struct itimerspec soon = { { 0, 0 }, { 0, 20 * 1000 * 1000 } };
        uint64_t n = 0;
        struct timespec d = { 0, 60 * 1000 * 1000 };
        timerfd_settime(fb, 0, &soon, NULL);
        nanosleep(&d, NULL);
        check(fb >= 0 && read(fb, &n, 8) == 8 && n == 1, "a CLOCK_BOOTTIME_ALARM timer fires once");
    } else {
        check(fr < 0 && er == EPERM && fb < 0 && eb == EPERM,
              "timerfd_create of the alarm clocks without CAP_WAKE_ALARM: EPERM, as on Linux");
    }
    check(timerfd_settime(timerfd_create(CLOCK_MONOTONIC, 0), 4, &(struct itimerspec){ 0 }, NULL) == -1 &&
          errno == EINVAL, "timerfd_settime with an unknown flag: EINVAL");
    // An absolute BOOTTIME deadline is measured against BOOTTIME, which here
    // counts the Mac's sleep and so runs ahead of MONOTONIC (AlarmManager's
    // elapsed-realtime alarms came due as much later as the Mac had slept).
    {
        struct timespec bt, mt;
        clock_gettime(CLOCK_BOOTTIME, &bt);
        clock_gettime(CLOCK_MONOTONIC, &mt);
        double ahead = (bt.tv_sec - mt.tv_sec) + (bt.tv_nsec - mt.tv_nsec) / 1e9;
        int tb = timerfd_create(CLOCK_BOOTTIME, TFD_NONBLOCK);
        struct itimerspec at = { { 0, 0 }, bt };
        at.it_value.tv_nsec += 80 * 1000 * 1000;
        if (at.it_value.tv_nsec >= 1000000000) { at.it_value.tv_sec++; at.it_value.tv_nsec -= 1000000000; }
        double b0 = now_s();
        int sr = tb >= 0 ? timerfd_settime(tb, TFD_TIMER_ABSTIME, &at, NULL) : -1;
        uint64_t n = 0;
        ssize_t rr = -1;
        while (now_s() - b0 < 3 && (rr = read(tb, &n, 8)) != 8) {
            struct timespec d = { 0, 5 * 1000 * 1000 };
            nanosleep(&d, NULL);
        }
        double took = now_s() - b0;
        snprintf(msg, sizeof msg, "timerfd CLOCK_BOOTTIME, ABSTIME now+80 ms: fired after %.0f ms (BOOTTIME ahead of MONOTONIC by %.1f s)",
                 took * 1000, ahead);
        check(sr == 0 && rr == 8 && n == 1 && took >= 0.07 && took < 1.5, msg);
        if (tb >= 0) close(tb);
    }
    if (fr >= 0) close(fr);
    if (fb >= 0) close(fb);

    // SO_DOMAIN, SO_PROTOCOL.
    int us = socket(AF_UNIX, SOCK_STREAM, 0), ts = socket(AF_INET, SOCK_STREAM, 0);
    int ud = socket(AF_INET, SOCK_DGRAM, 0), t6 = socket(AF_INET6, SOCK_STREAM, 0);
    snprintf(msg, sizeof msg, "SO_DOMAIN/SO_PROTOCOL: unix stream %d/%d, inet stream %d/%d, inet dgram %d/%d, inet6 %d",
             sockopt(us, SO_DOMAIN), sockopt(us, SO_PROTOCOL), sockopt(ts, SO_DOMAIN), sockopt(ts, SO_PROTOCOL),
             sockopt(ud, SO_DOMAIN), sockopt(ud, SO_PROTOCOL), sockopt(t6, SO_DOMAIN));
    check(sockopt(us, SO_DOMAIN) == AF_UNIX && sockopt(us, SO_PROTOCOL) == 0 &&
          sockopt(ts, SO_DOMAIN) == AF_INET && sockopt(ts, SO_PROTOCOL) == IPPROTO_TCP &&
          sockopt(ud, SO_DOMAIN) == AF_INET && sockopt(ud, SO_PROTOCOL) == IPPROTO_UDP &&
          (t6 < 0 || sockopt(t6, SO_DOMAIN) == AF_INET6), msg);
    close(us); close(ts); close(ud); if (t6 >= 0) close(t6);

    // An empty SCM_RIGHTS, then a receive with MSG_TRUNC|MSG_CTRUNC.
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    union { struct cmsghdr c; char b[CMSG_SPACE(0)]; } cm;
    memset(&cm, 0, sizeof cm);
    struct iovec iov = { "zygote", 6 };
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cm.b, .msg_controllen = sizeof cm.b };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(0);
    ssize_t sn = sendmsg(sv[0], &m, MSG_NOSIGNAL);
    char buf[64] = { 0 };
    union { struct cmsghdr c; char b[CMSG_SPACE(sizeof(int) * 64)]; } rc;
    struct iovec riov = { buf, sizeof buf };
    struct msghdr rm = { .msg_iov = &riov, .msg_iovlen = 1, .msg_control = rc.b, .msg_controllen = sizeof rc.b };
    ssize_t rn = recvmsg(sv[1], &rm, MSG_TRUNC | MSG_CTRUNC | MSG_CMSG_CLOEXEC | MSG_NOSIGNAL);
    snprintf(msg, sizeof msg, "an SCM_RIGHTS with no descriptor: %zd bytes sent, %zd received, flags 0x%x, control %zu",
             sn, rn, rm.msg_flags, (size_t)rm.msg_controllen);
    check(sn == 6 && rn == 6 && !memcmp(buf, "zygote", 6) && rm.msg_flags == 0 && rm.msg_controllen == 0, msg);
    close(sv[0]); close(sv[1]);

    // user. extended attributes (UserDataPreparer's user.serial, installd's
    // user.default / user.inode_cache); other namespaces ENOTSUP.
    char dir[] = "/tmp/lxrt-xattr-XXXXXX";
    if (mkdtemp(dir)) {
        char v[32] = { 0 };
        int s1 = setxattr(dir, "user.serial", "0", 1, 0);
        ssize_t g1 = getxattr(dir, "user.serial", v, sizeof v);
        ssize_t gz = getxattr(dir, "user.serial", NULL, 0);
        check(s1 == 0 && g1 == 1 && v[0] == '0' && gz == 1, "setxattr/getxattr user.serial on a directory (and the size query)");
        errno = 0;
        check(getxattr(dir, "user.missing", v, sizeof v) == -1 && errno == ENODATA, "a missing attribute: ENODATA");
        check(setxattr(dir, "user.serial", "1", 1, XATTR_CREATE) == -1 && errno == EEXIST &&
              setxattr(dir, "user.other", "x", 1, XATTR_REPLACE) == -1 && errno == ENODATA,
              "XATTR_CREATE on an existing one: EEXIST; XATTR_REPLACE on a missing one: ENODATA");
        check(setxattr(dir, "security.selinux", "u:object_r:x:s0", 16, 0) == -1 && errno == ENOTSUP &&
              getxattr(dir, "trusted.x", v, sizeof v) == -1 && errno == ENOTSUP,
              "security. and trusted.: ENOTSUP");
        char list[256];
        ssize_t ln = listxattr(dir, list, sizeof list);
        ssize_t lz = listxattr(dir, NULL, 0);
        check(ln == 12 && lz == 12 && !strcmp(list, "user.serial"), "listxattr shows the user. names only");
        int dfd = open(dir, O_RDONLY | O_DIRECTORY);
        check(dfd >= 0 && fsetxattr(dfd, "user.inode_cache", "12345678", 8, 0) == 0 &&
              fgetxattr(dfd, "user.inode_cache", v, sizeof v) == 8 && fremovexattr(dfd, "user.inode_cache") == 0 &&
              fgetxattr(dfd, "user.inode_cache", v, sizeof v) == -1 && errno == ENODATA,
              "fsetxattr, fgetxattr, fremovexattr on a descriptor");
        if (dfd >= 0) close(dfd);
        check(removexattr(dir, "user.serial") == 0 && listxattr(dir, NULL, 0) == 0, "removexattr");
        rmdir(dir);
    }

    // sendfile and splice (Android's FileUtils.copy: pm install's staging copy).
    {
        char a[] = "/tmp/lxrt-sendfile-a-XXXXXX", b[] = "/tmp/lxrt-sendfile-b-XXXXXX";
        int fa = mkstemp(a), fb = mkstemp(b);
        static char big[300000];
        for (size_t i = 0; i < sizeof big; i++) big[i] = (char)(i * 7 + 3);
        int wr = fa >= 0 && write(fa, big, sizeof big) == (ssize_t)sizeof big;
        off_t off = 1000;
        ssize_t n = wr ? sendfile(fb, fa, &off, sizeof big - 1000) : -1;
        static char back[300000];
        ssize_t got = pread(fb, back, sizeof back, 0);
        off_t cur = lseek(fa, 0, SEEK_CUR);
        snprintf(msg, sizeof msg, "sendfile file -> file with an offset: %zd bytes, offset now %lld, input's own offset %lld",
                 n, (long long)off, (long long)cur);
        check(n == (ssize_t)sizeof big - 1000 && got == n && !memcmp(back, big + 1000, (size_t)n) &&
              off == (off_t)sizeof big && cur == (off_t)sizeof big, msg);
        lseek(fa, 0, SEEK_SET);
        ssize_t n2 = sendfile(fb, fa, NULL, 5000);
        check(n2 == 5000 && lseek(fa, 0, SEEK_CUR) == 5000, "sendfile without an offset advances the input");
        int p[2];
        pipe(p);
        loff_t so = 10;
        ssize_t s1 = splice(fa, &so, p[1], NULL, 4096, 0);
        char pb[4096];
        ssize_t s2 = read(p[0], pb, sizeof pb);
        check(s1 == 4096 && s2 == 4096 && so == 4106 && !memcmp(pb, big + 10, 4096), "splice file -> pipe with an offset");
        errno = 0;
        check(splice(fa, NULL, fb, NULL, 10, 0) == -1 && errno == EINVAL, "splice with no pipe: EINVAL");
        close(p[0]); close(p[1]);
        if (fa >= 0) { close(fa); unlink(a); }
        if (fb >= 0) { close(fb); unlink(b); }
    }

    // process_vm_readv on itself (ART's SafeCopy of the faulting instruction).
    {
        static const char src[] = "art fault handler";
        char dst[32] = { 0 };
        struct iovec lv = { dst, sizeof src }, rv = { (void *)src, sizeof src };
        ssize_t n = process_vm_readv(getpid(), &lv, 1, &rv, 1, 0);
        check(n == (ssize_t)sizeof src && !strcmp(dst, src), "process_vm_readv of its own memory");
        struct iovec bad = { (void *)8, 16 };
        errno = 0;
        check(process_vm_readv(getpid(), &lv, 1, &bad, 1, 0) == -1 && errno == EFAULT,
              "process_vm_readv of an unmapped address: EFAULT (no fault)");
        struct iovec two[2] = { { (void *)src, 4 }, { (void *)8, 4 } };
        check(process_vm_readv(getpid(), &lv, 1, two, 2, 0) == 4, "a readable element, then an unreadable one: the first is read");
        char w[8] = "xxxxxxx";
        struct iovec wl = { "written", 8 }, wr = { w, 8 };
        check(process_vm_writev(getpid(), &wl, 1, &wr, 1, 0) == 8 && !strcmp(w, "written"), "process_vm_writev to its own memory");
        check(process_vm_readv(1, &lv, 1, &rv, 1, 0) == -1 && errno == EPERM, "another process (launchd): EPERM");
    }

    printf("== android boot rt: %s\n", bad ? "MAL" : "ok");
    return bad ? 1 : 0;
}
