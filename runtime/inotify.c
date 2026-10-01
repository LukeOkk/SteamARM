// See inotify.h for the Linux ABI, dispatch contract and Darwin divergences.
#include "lxrt.h"
#include "inotify.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/event.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))
#define I_MODIFY 0x2u
#define I_ATTRIB 0x4u
#define I_FROM 0x40u
#define I_TO 0x80u
#define I_CREATE 0x100u
#define I_DELETE 0x200u
#define I_DELETE_SELF 0x400u
#define I_MOVE_SELF 0x800u
#define I_OVERFLOW 0x4000u
#define I_IGNORED 0x8000u
#define I_ONLYDIR 0x01000000u
#define I_NOFOLLOW 0x02000000u
#define I_MASK_ADD 0x20000000u
#define I_ISDIR 0x40000000u
#define I_ONESHOT 0x80000000u
#define I_EVENTS 0xfffu
#define I_NONBLOCK 0x800
#define I_CLOEXEC 0x80000
#define FD_LIMIT 1048576
#define QUEUE_LIMIT 16384

struct ino_event { int32_t wd; uint32_t mask, cookie, len; };
_Static_assert(sizeof(struct ino_event) == 16, "Linux inotify ABI");
struct record {
    struct record *next;
    struct ino_event event;
    char name[256];
};
struct dir_entry {
    struct dir_entry *next;
    ino_t ino;
    bool isdir, matched;
    char name[256];
};
struct watch {
    struct watch *next;
    int fd, wd;
    dev_t dev;
    ino_t ino;
    uint32_t mask;
    bool isdir, dead;
    struct dir_entry *entries;
};
struct instance {
    int kq, reader, writer;
    pthread_t thread;
    unsigned refs, aliases, queued;
    uint32_t cookie;
    int64_t next_wd;
    bool stopping, overflow_queued;
    struct watch *watches;
    struct record *head, *tail;
    struct record overflow;
};
// One lock defines the ordering of events, watch removal and readiness.
// No blocking kevent/poll or pthread_join is performed while holding it.
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(inotify_lock, lock)
static struct instance *instances[FD_LIMIT];

static struct instance *lookup(int fd)
{
    return fd >= 0 && fd < FD_LIMIT ? instances[fd] : NULL;
}
static bool range_ok(void *ptr, size_t len)
{
    mach_vm_address_t a = (uintptr_t)ptr, end = a + len;
    if (!ptr || end < a) return false;
    while (a < end) {
        mach_vm_address_t start = a;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &start, &size,
            VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &count, &object);
        if (object) mach_port_deallocate(mach_task_self(), object);
        if (kr != KERN_SUCCESS || start > a || !size ||
            !(info.protection & VM_PROT_WRITE)) return false;
        a = start + size;
    }
    return true;
}
// Kernel-assisted copy-in makes even an invalid guest pathname an EFAULT.
static int copy_path(const char *src, char dst[PATH_MAX])
{
    if (!src) return EFAULT;
    for (size_t n = 0; n < PATH_MAX; ++n) {
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (uintptr_t)src + n, 1,
            (uintptr_t)(dst + n), &got) != KERN_SUCCESS || got != 1)
            return EFAULT;
        if (!dst[n]) return 0;
    }
    return ENAMETOOLONG;
}
static void free_entries(struct dir_entry *e)
{
    while (e) { struct dir_entry *next = e->next; free(e); e = next; }
}
// Enumerate via the watched vnode, not a saved pathname: watching continues
// after the directory itself is renamed. openat gives an independent offset.
static int snapshot(int fd, struct dir_entry **out)
{
    *out = NULL;
    DIR *dir = lxrt_opendirat_private(fd, ".");
    if (!dir) return errno;
    int scanfd = dirfd(dir);
    int error = 0;
    for (;;) {
        errno = 0;
        struct dirent *de = readdir(dir);
        if (!de) { error = errno; break; }
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        struct stat st;
        if (fstatat(scanfd, de->d_name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT) continue; // raced an unlink
            error = errno; break;
        }
        struct dir_entry *e = calloc(1, sizeof *e);
        if (!e) { error = ENOMEM; break; }
        e->ino = st.st_ino;
        e->isdir = S_ISDIR(st.st_mode);
        strlcpy(e->name, de->d_name, sizeof e->name);
        e->next = *out; *out = e;
    }
    lxrt_closedir_private(dir);
    if (error) { free_entries(*out); *out = NULL; }
    return error;
}
// Readiness is level-triggered: the private pipe holds bytes exactly while
// events are queued. Drain every pending byte (not just one) whenever the
// queue is empty; a stray byte left behind made poll() report the pipe
// readable forever, and Chromium's inotify reader thread in Steam's web
// helper spun at 100% CPU in lxrt_inotify_read -- the Steam window never
// appeared (MEASURED with sample(1)).
static void drain_ready(struct instance *i)
{
    int pending = 0;
    char sink[64];
    while (ioctl(i->reader, FIONREAD, &pending) == 0 && pending > 0) {
        ssize_t n = read(i->reader, sink, pending < (int)sizeof sink ? (size_t)pending : sizeof sink);
        if (n <= 0 && errno != EINTR) break;
    }
}
static void signal_ready(struct instance *i)
{
    char byte = 'i';
    ssize_t n;
    do { n = write(i->writer, &byte, 1); } while (n < 0 && errno == EINTR);
    // The private reader stays alive until destruction. One outstanding byte
    // cannot fill a pipe; EAGAIN would only mean readiness is already present.
}
static void append(struct instance *i, struct record *r)
{
    r->next = NULL;
    if (i->tail) i->tail->next = r;
    else { i->head = r; signal_ready(i); }
    i->tail = r;
    ++i->queued;
}
static void overflow(struct instance *i)
{
    if (i->overflow_queued) return;
    i->overflow_queued = true;
    memset(&i->overflow, 0, sizeof i->overflow);
    i->overflow.event.wd = -1;
    i->overflow.event.mask = I_OVERFLOW;
    append(i, &i->overflow);
}
static void enqueue(struct instance *i, int wd, uint32_t mask,
                    uint32_t cookie, const char *name)
{
    if (i->queued >= QUEUE_LIMIT) { overflow(i); return; }
    struct record *r = calloc(1, sizeof *r);
    if (!r) { overflow(i); return; }
    r->event = (struct ino_event){wd, mask, cookie, 0};
    if (name) {
        size_t n = strlen(name) + 1;
        r->event.len = (uint32_t)((n + 15) & ~(size_t)15);
        memcpy(r->name, name, n);
    }
    append(i, r);
}
static void retire(struct instance *i, struct watch *w)
{
    if (w->dead) return;
    w->dead = true;
    close(w->fd); // removes EVFILT_VNODE
    w->fd = -1;
    free_entries(w->entries); w->entries = NULL;
    enqueue(i, w->wd, I_IGNORED, 0, NULL);
}
static void emit(struct instance *i, struct watch *w, uint32_t mask,
                 uint32_t cookie, const char *name, bool isdir)
{
    if (w->dead || !(mask & w->mask & I_EVENTS)) return;
    mask &= w->mask & I_EVENTS;
    enqueue(i, w->wd, mask | (isdir ? I_ISDIR : 0), cookie, name);
    if (w->mask & I_ONESHOT) retire(i, w);
}
static void diff(struct instance *i, struct watch *w)
{
    struct dir_entry *fresh;
    if (snapshot(w->fd, &fresh)) { overflow(i); return; }
    struct dir_entry *old = w->entries;
    // Unchanged names/inodes first; a replaced name must be DELETE + CREATE.
    for (struct dir_entry *a = old; a; a = a->next) {
        a->matched = false;
        for (struct dir_entry *b = fresh; b; b = b->next)
            if (a->ino == b->ino && !strcmp(a->name, b->name)) {
                a->matched = b->matched = true; break;
            }
    }
    // Detach while diffing: ONESHOT may retire the watch during emit().
    w->entries = NULL;
    for (struct dir_entry *a = old; a; a = a->next) {
        if (a->matched) continue;
        struct dir_entry *match = NULL;
        for (struct dir_entry *b = fresh; b; b = b->next)
            if (!b->matched && a->ino == b->ino && a->isdir == b->isdir) {
                match = b; break;
            }
        if (match) {
            match->matched = true;
            if (++i->cookie == 0) ++i->cookie;
            emit(i, w, I_FROM, i->cookie, a->name, a->isdir);
            emit(i, w, I_TO, i->cookie, match->name, match->isdir);
        } else emit(i, w, I_DELETE, 0, a->name, a->isdir);
    }
    for (struct dir_entry *b = fresh; b; b = b->next)
        if (!b->matched) emit(i, w, I_CREATE, 0, b->name, b->isdir);
    free_entries(old);
    if (w->dead) free_entries(fresh);
    else w->entries = fresh;
}
static void release(struct instance *i)
{
    if (--i->refs) return;
    while (i->watches) {
        struct watch *w = i->watches; i->watches = w->next;
        if (w->fd >= 0) close(w->fd);
        free_entries(w->entries); free(w);
    }
    while (i->head) {
        struct record *r = i->head; i->head = r->next;
        if (r != &i->overflow) free(r);
    }
    close(i->reader); close(i->writer); close(i->kq); free(i);
}
static void *worker(void *arg)
{
    struct instance *i = arg;
    for (;;) {
        struct kevent event;
        int n = kevent(i->kq, NULL, 0, &event, 1, NULL);
        int error = errno;
        pthread_mutex_lock(&lock);
        if (i->stopping) { release(i); pthread_mutex_unlock(&lock); return NULL; }
        if (n < 0) {
            if (error != EINTR) overflow(i);
        } else if (n && event.filter == EVFILT_VNODE) {
            // udata holds a monotonically increasing wd, never a freed pointer
            // or recycled host fd. rm_watch racing kevent is therefore harmless.
            int wd = (int)(uintptr_t)event.udata;
            struct watch *w;
            for (w = i->watches; w && w->wd != wd; w = w->next) {}
            if (w && !w->dead) {
                if (event.flags & EV_ERROR) overflow(i);
                else {
                    if (w->isdir && (event.fflags & NOTE_WRITE)) diff(i, w);
                    if (!w->isdir && (event.fflags & (NOTE_WRITE | NOTE_EXTEND)))
                        emit(i, w, I_MODIFY, 0, NULL, false);
                    if (event.fflags & (NOTE_ATTRIB | NOTE_LINK))
                        emit(i, w, I_ATTRIB, 0, NULL, w->isdir);
                    if (event.fflags & NOTE_RENAME)
                        emit(i, w, I_MOVE_SELF, 0, NULL, w->isdir);
                    if (event.fflags & NOTE_DELETE) {
                        emit(i, w, I_DELETE_SELF, 0, NULL, w->isdir);
                        retire(i, w);
                    }
                }
            }
            // Dead watches need not survive: kevent identifies only the wd.
            struct watch **p = &i->watches;
            while (*p) {
                if ((*p)->dead) { w = *p; *p = w->next; free(w); }
                else p = &(*p)->next;
            }
        }
        pthread_mutex_unlock(&lock);
    }
}
long lxrt_inotify_init1(int flags)
{
    if (flags & ~(I_NONBLOCK | I_CLOEXEC)) return LERR(EINVAL);
    struct instance *i = calloc(1, sizeof *i);
    if (!i) return LERR(ENOMEM);
    i->kq = i->reader = i->writer = -1;
    i->refs = 1;
    int fds[2], fd = -1, error;
    if (pipe(fds) < 0) { error = errno; goto fail; }
    i->reader = fds[0]; i->writer = fds[1];
    if (fcntl(i->reader, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(i->writer, F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(i->writer, F_SETFL, O_NONBLOCK) < 0 ||
        fcntl(i->reader, F_SETFL, flags & I_NONBLOCK ? O_NONBLOCK : 0) < 0) {
        error = errno; goto fail;
    }
    fd = fcntl(i->reader, flags & I_CLOEXEC ? F_DUPFD_CLOEXEC : F_DUPFD, 0);
    if (fd < 0) { error = errno; goto fail; }
    if (fd >= FD_LIMIT) { error = EMFILE; goto fail; }
    i->kq = kqueue();
    if (i->kq < 0 || fcntl(i->kq, F_SETFD, FD_CLOEXEC) < 0) {
        error = errno; goto fail;
    }
    struct kevent wake;
    EV_SET(&wake, 1, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, NULL);
    if (kevent(i->kq, &wake, 1, NULL, 0, NULL) < 0) { error = errno; goto fail; }
    i->next_wd = 1; i->aliases = 1; i->refs = 2; // alias + worker
    pthread_mutex_lock(&lock);
    error = pthread_create(&i->thread, NULL, worker, i);
    if (!error) instances[fd] = i;
    pthread_mutex_unlock(&lock);
    if (error) { i->refs = 1; goto fail; }
    return fd;
fail:
    if (fd >= 0) close(fd);
    release(i);
    return LERR(error);
}
bool lxrt_inotify_is(int fd)
{
    pthread_mutex_lock(&lock);
    bool yes = lookup(fd) != NULL;
    pthread_mutex_unlock(&lock);
    return yes;
}
long lxrt_inotify_add_watch(int fd, const char *guest, uint32_t mask)
{
    pthread_mutex_lock(&lock);
    struct instance *i = lookup(fd);
    long result;
    if (!i) { result = LERR(fcntl(fd, F_GETFD) < 0 ? EBADF : EINVAL); goto done; }
    if (!(mask & I_EVENTS) || (mask & ~(I_EVENTS | I_ONLYDIR | I_NOFOLLOW |
                                      I_MASK_ADD | I_ONESHOT))) {
        result = LERR(EINVAL); goto done;
    }
    char path[PATH_MAX];
    int error = copy_path(guest, path);
    if (error) { result = LERR(error); goto done; }
    // A guest path: through the root, the sandbox binds and /proc, like
    // every file syscall (/dev/input is watched for controller hotplug).
    extern const char *lxrt_translate_guest_path(const char *, char *, size_t);
    char hostbuf[PATH_MAX];
    const char *translated = path[0] == '/' ? lxrt_translate_guest_path(path, hostbuf, sizeof hostbuf) : path;
    int watchfd = open(translated, O_EVTONLY | O_CLOEXEC |
        (mask & I_NOFOLLOW ? O_SYMLINK : 0));
    if (watchfd < 0) { result = LERR(errno); goto done; }
    struct stat st;
    if (fstat(watchfd, &st) < 0) {
        result = LERR(errno); close(watchfd); goto done;
    }
    if ((mask & I_ONLYDIR) && !S_ISDIR(st.st_mode)) {
        result = LERR(ENOTDIR); close(watchfd); goto done;
    }
    for (struct watch *w = i->watches; w; w = w->next) {
        if (!w->dead && w->ino == st.st_ino && w->dev == st.st_dev) {
            uint32_t interest = mask & (I_EVENTS | I_ONESHOT);
            w->mask = mask & I_MASK_ADD ? w->mask | interest : interest;
            result = w->wd; close(watchfd); goto done;
        }
    }
    if (i->next_wd > INT_MAX) { result = LERR(ENOSPC); close(watchfd); goto done; }
    struct watch *w = calloc(1, sizeof *w);
    if (!w) { result = LERR(ENOMEM); close(watchfd); goto done; }
    w->fd = watchfd; w->ino = st.st_ino; w->dev = st.st_dev;
    w->wd = (int)i->next_wd; w->mask = mask & (I_EVENTS | I_ONESHOT);
    w->isdir = S_ISDIR(st.st_mode);
    struct kevent change;
    EV_SET(&change, watchfd, EVFILT_VNODE, EV_ADD | EV_CLEAR,
        NOTE_WRITE | NOTE_DELETE | NOTE_RENAME | NOTE_ATTRIB | NOTE_EXTEND | NOTE_LINK,
        0, (void *)(uintptr_t)w->wd);
    // Register before taking the baseline so a subsequent change cannot fall
    // into a snapshot-to-registration gap. Pre-return changes may be coalesced.
    error = kevent(i->kq, &change, 1, NULL, 0, NULL) < 0 ? errno : 0;
    if (!error && w->isdir) error = snapshot(watchfd, &w->entries);
    if (error) { close(watchfd); free(w); result = LERR(error); goto done; }
    ++i->next_wd; w->next = i->watches; i->watches = w;
    result = w->wd;
done:
    pthread_mutex_unlock(&lock);
    return result;
}
long lxrt_inotify_rm_watch(int fd, int wd)
{
    pthread_mutex_lock(&lock);
    struct instance *i = lookup(fd);
    long result = LERR(i || fcntl(fd, F_GETFD) >= 0 ? EINVAL : EBADF);
    if (i) {
        struct watch **p = &i->watches;
        while (*p && (*p)->wd != wd) p = &(*p)->next;
        if (*p && !(*p)->dead) {
            struct watch *w = *p; retire(i, w); *p = w->next; free(w); result = 0;
        }
    }
    pthread_mutex_unlock(&lock);
    return result;
}
// FIONREAD, as Linux answers it: the size of the queued events. Qt reads
// exactly this many bytes on a blocking descriptor
// (QInotifyFileSystemWatcherEngine::readFromInotify); the pipe's own count
// (its one readiness byte) made that read(fd, buf, 1) fail with EINVAL.
// A readiness byte with nothing queued is drained here, so a poll that
// follows does not report the descriptor readable for nothing.
long lxrt_inotify_pending(int fd)
{
    pthread_mutex_lock(&lock);
    struct instance *i = lookup(fd);
    long total = 0;
    if (!i) {
        total = LERR(EBADF);
    } else {
        for (struct record *r = i->head; r; r = r->next)
            total += (long)(sizeof r->event + r->event.len);
        if (!i->head && !i->stopping) drain_ready(i);
        if (total > INT_MAX) total = INT_MAX;
    }
    pthread_mutex_unlock(&lock);
    return total;
}
long lxrt_inotify_read(int fd, void *buf, size_t len)
{
    pthread_mutex_lock(&lock);
    struct instance *i = lookup(fd);
    if (!i) { pthread_mutex_unlock(&lock); return LERR(EBADF); }
    ++i->refs;
    long result = 0;
    for (;;) {
        if (i->stopping) { result = LERR(EBADF); break; }
        if (i->head) break;
        if (fcntl(i->reader, F_GETFL) & O_NONBLOCK) { result = LERR(EAGAIN); break; }
        struct pollfd p = {i->reader, POLLIN, 0};
        pthread_mutex_unlock(&lock);
        int n = poll(&p, 1, -1), error = errno;
        pthread_mutex_lock(&lock);
        if (n < 0) { result = LERR(error); break; }
        if (!i->head && !i->stopping) drain_ready(i);   // readable but nothing queued
    }
    while (!result && i->head) {
        struct record *r = i->head;
        size_t size = sizeof r->event + r->event.len;
        if (size > len) { result = LERR(EINVAL); break; }
        // Return as many WHOLE events as fit; keep the next one queued.
        size_t used = 0;
        do {
            r = i->head; size = sizeof r->event + r->event.len;
            if (size > len - used) break;
            if (!range_ok((char *)buf + used, size)) {
                result = used ? (long)used : LERR(EFAULT); break;
            }
            memcpy((char *)buf + used, &r->event, sizeof r->event);
            memcpy((char *)buf + used + sizeof r->event, r->name, r->event.len);
            used += size;
            i->head = r->next; --i->queued;
            if (r == &i->overflow) i->overflow_queued = false;
            else free(r);
        } while (i->head);
        if (!i->head) {
            i->tail = NULL;
            drain_ready(i);
        }
        if (!result) result = (long)used;
    }
    release(i);
    pthread_mutex_unlock(&lock);
    return result;
}
// Drop one alias. Save thread id before dropping references; the worker can
// destroy the instance as soon as the lock is released.
static bool detach(int fd, pthread_t *thread)
{
    struct instance *i = lookup(fd);
    if (!i) return false;
    instances[fd] = NULL;
    bool last = --i->aliases == 0;
    if (last) {
        i->stopping = true; *thread = i->thread;
        if (!i->head) signal_ready(i); // wake any blocked reads
        struct kevent wake;
        EV_SET(&wake, 1, EVFILT_USER, 0, NOTE_TRIGGER, 0, NULL);
        kevent(i->kq, &wake, 1, NULL, 0, NULL);
    }
    release(i);
    return last;
}
void lxrt_inotify_close(int fd)
{
    pthread_t thread;
    pthread_mutex_lock(&lock);
    bool join = detach(fd, &thread);
    pthread_mutex_unlock(&lock);
    if (join) pthread_join(thread, NULL);
}
void lxrt_inotify_dup(int oldfd, int newfd)
{
    if (oldfd == newfd || newfd < 0 || newfd >= FD_LIMIT) return;
    pthread_t thread;
    pthread_mutex_lock(&lock);
    struct instance *i = lookup(oldfd);
    bool join = detach(newfd, &thread);
    if (i) { ++i->aliases; ++i->refs; instances[newfd] = i; }
    pthread_mutex_unlock(&lock);
    if (join) pthread_join(thread, NULL);
}
