// Linux aarch64 reference: gcc -static-pie -O2 -o /tmp/inotify_test inotify_test.c
// INOTIFY_HOST lets the native harness run the identical event assertions.
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef INOTIFY_HOST
#include <sys/epoll.h>
#include <sys/syscall.h>
#endif

enum {
    T_MODIFY = 2, T_ATTRIB = 4, T_FROM = 0x40, T_TO = 0x80,
    T_CREATE = 0x100, T_DELETE = 0x200, T_DELETE_SELF = 0x400,
    T_MOVE_SELF = 0x800, T_IGNORED = 0x8000, T_ONLYDIR = 0x1000000,
    T_NOFOLLOW = 0x2000000, T_MASK_ADD = 0x20000000, T_ISDIR = 0x40000000,
    T_NONBLOCK = 0x800, T_CLOEXEC = 0x80000
};
#define T_ONESHOT UINT32_C(0x80000000)
struct test_event { int32_t wd; uint32_t mask, cookie, len; };
static int good, bad;
static void check(int truth, const char *name)
{
    printf("  %s  %s\n", truth ? "OK " : "MAL", name);
    if (truth) ++good; else ++bad;
}
#ifdef INOTIFY_HOST
#include "inotify.h"
static long host_result(long r)
{
    if (r >= 0) return r;
    errno = r == -11 ? EAGAIN : r == -40 ? ELOOP : (int)-r;
    return -1;
}
static long ino_init(int flags) { return host_result(lxrt_inotify_init1(flags)); }
static long ino_add(int fd, const char *p, uint32_t m)
{ return host_result(lxrt_inotify_add_watch(fd, p, m)); }
static long ino_rm(int fd, int wd) { return host_result(lxrt_inotify_rm_watch(fd, wd)); }
static long ino_read(int fd, void *p, size_t n)
{ return host_result(lxrt_inotify_read(fd, p, n)); }
static void ino_close(int fd) { lxrt_inotify_close(fd); close(fd); }
static void host_checks(const char *dir);
#else
// Linux aarch64 syscall ABI: init1=26, add_watch=27, rm_watch=28.
static long ino_init(int flags) { return syscall(26, flags); }
static long ino_add(int fd, const char *p, uint32_t m)
{ return syscall(27, fd, p, m); }
static long ino_rm(int fd, int wd) { return syscall(28, fd, wd); }
static long ino_read(int fd, void *p, size_t n) { return syscall(SYS_read, fd, p, n); }
static void ino_close(int fd) { syscall(SYS_close, fd); }
#endif
struct batch { unsigned char bytes[8192]; size_t len; };
static struct batch collect(int fd)
{
    struct batch b = {{0}, 0};
    usleep(100000);
    for (;;) {
        long n = ino_read(fd, b.bytes + b.len, sizeof b.bytes - b.len);
        if (n < 0 && errno == EAGAIN) break;
        if (n <= 0 || (size_t)n > sizeof b.bytes - b.len) {
            check(0, "read valid event batch"); break;
        }
        b.len += (size_t)n;
        if (b.len == sizeof b.bytes) break;
    }
    size_t off = 0;
    while (off < b.len) {
        struct test_event e;
        if (b.len - off < sizeof e) { check(0, "event header intact"); break; }
        memcpy(&e, b.bytes + off, sizeof e);
        if (e.len % 16 || e.len > b.len - off - sizeof e ||
            (e.len && !memchr(b.bytes + off + sizeof e, 0, e.len))) {
            check(0, "event name and padding intact"); break;
        }
        off += sizeof e + e.len;
    }
    return b;
}
static int event_in(const struct batch *b, int wd, uint32_t mask,
                    const char *name, uint32_t *cookie)
{
    for (size_t off = 0; off + sizeof(struct test_event) <= b->len;) {
        struct test_event e;
        memcpy(&e, b->bytes + off, sizeof e);
        if (e.len > b->len - off - sizeof e) return 0;
        const char *n = (const char *)b->bytes + off + sizeof e;
        if (e.wd == wd && (e.mask & mask) == mask &&
            (!name || (e.len && memchr(n, 0, e.len) && !strcmp(n, name)))) {
            if (cookie) *cookie = e.cookie;
            return 1;
        }
        off += sizeof e + e.len;
    }
    return 0;
}
static int create_file(const char *path)
{
    int f = open(path, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (f < 0) return 0;
    return close(f) == 0;
}
int main(void)
{
    char dir[] = "/tmp/lxrt-inotify-XXXXXX", a[512], b[512], sub[512], link[512];
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    snprintf(a, sizeof a, "%s/a", dir);
    snprintf(b, sizeof b, "%s/b", dir);
    snprintf(sub, sizeof sub, "%s/sub", dir);
    snprintf(link, sizeof link, "%s/link", dir);
    int fd = (int)ino_init(T_NONBLOCK | T_CLOEXEC);
    check(fd >= 0, "init1 NONBLOCK|CLOEXEC");
    if (fd < 0) { rmdir(dir); return 1; }
    check((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0, "CLOEXEC set");
    check((fcntl(fd, F_GETFL) & O_NONBLOCK) != 0, "NONBLOCK set");
    char tiny[8];
    check(ino_read(fd, tiny, sizeof tiny) == -1 && errno == EAGAIN,
          "NONBLOCK empty: EAGAIN");
    check(ino_init(1) == -1 && errno == EINVAL, "init1 invalid flags: EINVAL");
    check(ino_add(fd, a, T_MODIFY) == -1 && errno == ENOENT, "missing path: ENOENT");
    uint32_t dm = T_CREATE | T_DELETE | T_FROM | T_TO;
    int dw = (int)ino_add(fd, dir, dm);
    check(dw == 1, "first wd is 1");
    check(create_file(a), "create file");
    usleep(100000);
    struct pollfd p = {fd, POLLIN, 0};
    check(poll(&p, 1, 1000) == 1 && (p.revents & POLLIN), "fd readable via poll");
    check(ino_read(fd, tiny, sizeof tiny) == -1 && errno == EINVAL,
          "small buffer: EINVAL, event retained");
    struct batch batch = collect(fd);
    check(event_in(&batch, dw, T_CREATE, "a", NULL), "IN_CREATE name=a");
    p.revents = 0;
    check(poll(&p, 1, 0) == 0, "drained fd no longer readable");
    check(ino_add(fd, a, T_ONLYDIR | T_MODIFY) == -1 && errno == ENOTDIR,
          "ONLYDIR on file: ENOTDIR");
    int fw = (int)ino_add(fd, a, T_MODIFY | T_DELETE_SELF | T_MOVE_SELF);
    check(fw > dw, "watch descriptors increase");
    int file = open(a, O_WRONLY);
    check(file >= 0 && write(file, "x", 1) == 1, "write watched file");
    if (file >= 0) close(file);
    batch = collect(fd);
    check(event_in(&batch, fw, T_MODIFY, NULL, NULL), "IN_MODIFY");
    check(rename(a, b) == 0, "rename within directory");
    batch = collect(fd);
    uint32_t from = 0, to = 0;
    check(event_in(&batch, dw, T_FROM, "a", &from) &&
          event_in(&batch, dw, T_TO, "b", &to) && from && from == to,
          "MOVED_FROM/MOVED_TO names and shared nonzero cookie");
    check(event_in(&batch, fw, T_MOVE_SELF, NULL, NULL), "IN_MOVE_SELF");
    check(unlink(b) == 0, "unlink watched file");
    batch = collect(fd);
    check(event_in(&batch, dw, T_DELETE, "b", NULL), "IN_DELETE name=b");
    check(event_in(&batch, fw, T_DELETE_SELF, NULL, NULL) &&
          event_in(&batch, fw, T_IGNORED, NULL, NULL), "DELETE_SELF and IGNORED");
    check(mkdir(sub, 0700) == 0, "mkdir");
    batch = collect(fd);
    check(event_in(&batch, dw, T_CREATE | T_ISDIR, "sub", NULL), "CREATE|ISDIR");
    check(rmdir(sub) == 0, "remove child directory");
    batch = collect(fd);
    check(event_in(&batch, dw, T_DELETE | T_ISDIR, "sub", NULL),
          "IN_DELETE|IN_ISDIR name=sub");
    check(mkdir(sub, 0700) == 0, "recreate child directory");
    collect(fd);
    check(ino_add(fd, dir, T_CREATE) == dw, "same path replaces mask, same wd");
    rmdir(sub);
    batch = collect(fd);
    check(!event_in(&batch, dw, T_DELETE, "sub", NULL), "replacement removes DELETE interest");
    check(ino_add(fd, dir, T_DELETE | T_MASK_ADD) == dw, "MASK_ADD retains wd");
    create_file(a);
    batch = collect(fd);
    check(event_in(&batch, dw, T_CREATE, "a", NULL), "MASK_ADD preserves CREATE");
    unlink(a);
    batch = collect(fd);
    check(event_in(&batch, dw, T_DELETE, "a", NULL), "MASK_ADD adds DELETE");
#ifndef INOTIFY_HOST
    int ep = (int)syscall(SYS_epoll_create1, 0);
    struct epoll_event interest = {.events = EPOLLIN, .data.u64 = 123};
    int registered = ep >= 0 && syscall(SYS_epoll_ctl, ep, EPOLL_CTL_ADD, fd, &interest) == 0;
    create_file(a);
    usleep(100000);
    struct epoll_event ready = {0};
    check(registered && syscall(SYS_epoll_pwait, ep, &ready, 1, 1000, NULL, 8) == 1 &&
          (ready.events & EPOLLIN) && ready.data.u64 == 123, "epoll readable after event");
    if (ep >= 0) close(ep);
    collect(fd); unlink(a); collect(fd);
#endif
    check(ino_rm(fd, dw) == 0, "rm_watch succeeds");
    batch = collect(fd);
    check(event_in(&batch, dw, T_IGNORED, NULL, NULL), "rm_watch queues IGNORED");
    check(ino_rm(fd, dw) == -1 && errno == EINVAL, "removed wd: EINVAL");
    dw = (int)ino_add(fd, dir, T_CREATE | T_ONESHOT);
    create_file(a);
    batch = collect(fd);
    check(event_in(&batch, dw, T_CREATE, "a", NULL) &&
          event_in(&batch, dw, T_IGNORED, NULL, NULL), "ONESHOT event then IGNORED");
    check(ino_rm(fd, dw) == -1 && errno == EINVAL, "ONESHOT watch removed");
    unlink(a);
    // Symlink inode and target must remain distinct under DONT_FOLLOW.
    create_file(a);
    check(symlink(a, link) == 0, "create symlink");
    fw = (int)ino_add(fd, a, T_MODIFY);
    int sw = (int)ino_add(fd, link, T_DELETE_SELF | T_NOFOLLOW);
    check(sw > fw && fw > dw, "DONT_FOLLOW watches symlink inode");
    unlink(link);
    batch = collect(fd);
    check(event_in(&batch, sw, T_DELETE_SELF, NULL, NULL) &&
          event_in(&batch, sw, T_IGNORED, NULL, NULL), "symlink deletion and IGNORED");
    ino_rm(fd, fw); collect(fd); unlink(a);
    ino_close(fd);
#ifdef INOTIFY_HOST
    host_checks(dir);
#endif
    unlink(link); unlink(a); unlink(b); rmdir(sub); rmdir(dir);
    printf("== %d ok, %d mal\n", good, bad);
    return bad ? 1 : 0;
}
