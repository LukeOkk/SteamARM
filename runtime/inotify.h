// Linux inotify on Darwin kqueue. All failures return negative LINUX errno.
#ifndef LXRT_INOTIFY_H
#define LXRT_INOTIFY_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

long lxrt_inotify_init1(int lflags);                           // syscall 26
long lxrt_inotify_add_watch(int fd, const char *guest_path, uint32_t mask); // 27
long lxrt_inotify_rm_watch(int fd, int wd);                    // 28
bool lxrt_inotify_is(int fd);
long lxrt_inotify_read(int fd, void *buf, size_t len);
void lxrt_inotify_close(int fd);
void lxrt_inotify_dup(int oldfd, int newfd);

// The public fd is a real pipe read end: poll/select and epoll's EVFILT_READ
// work without special handling. One readiness byte remains until the event
// queue is empty. ALWAYS route guest reads through lxrt_inotify_read; raw read
// would steal the readiness byte. Writes must return -Linux EINVAL.
// close drops bookkeeping only; caller closes the public fd afterwards.
// dup hooks run AFTER successful dup/dup2/fcntl duplication. The dup hook also
// drops any former newfd mapping (dup2 replacement), even for non-inotify
// oldfd. Failed duplication must not change bookkeeping. F_SETFL needs no
// hook: read consults the pipe's shared O_NONBLOCK flag. F_GETFL must report
// Linux O_RDONLY, plus O_NONBLOCK, rather than Darwin pipe-only flags.
// Internal descriptors are CLOEXEC. Aliases share watches, queue and flags.
//
// Darwin limitations:
// * IN_OPEN, IN_ACCESS, IN_CLOSE_WRITE and IN_CLOSE_NOWRITE are NOT generated.
// * Directory snapshots cannot observe transient entries between scans or
//   every intermediate rename. Same-directory inode matches get paired move
//   cookies; cross-directory moves appear as DELETE/CREATE. Hard links and
//   inode reuse can make rename inference ambiguous. Directory watches do not
//   synthesize MODIFY/ATTRIB for children (watch the file itself).
// * kqueue coalesces vnode changes. No exact Linux event ordering/counts.
// * No IN_UNMOUNT synthesis; no fork inheritance (worker threads do not survive
//   fork). Queue bound is 16384 records, then IN_Q_OVERFLOW (wd=-1).
// * Guest memory checks, as in epoll_eventfd.c, cannot prevent concurrent unmap
//   between validation and copying. Callers must serialize fd replacement with
//   dispatch hooks. At most 1048576 descriptor numbers can be tracked.
#endif
