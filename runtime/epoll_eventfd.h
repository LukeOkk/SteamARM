// epoll and eventfd. Darwin has neither.
//
// benchmarks/stage6-steam-gap.txt, measured on the live client: 46 eventpoll
// and 49 eventfd descriptors in Steam's process tree, 17 eventfd and 7 epoll
// held by the `steam` binary itself, and epoll_pwait at 33334 calls per 10 s --
// the 4th most frequent syscall on the machine. This is the client's own event
// loop, not the embedded browser, and without it nothing starts.
//
// The substrate is kqueue for epoll and a pipe for eventfd. Both hand the guest
// a REAL Darwin descriptor, because the guest also passes these fds to ppoll()
// and to close(), and a synthetic number would fail there. The runtime keeps a
// side table mapping the descriptor to its registered interest list.

#ifndef LXRT_EPOLL_EVENTFD_H
#define LXRT_EPOLL_EVENTFD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ----------------------------------------------------------- syscalls
//
// aarch64 asm-generic numbers, verified against
// include/uapi/asm-generic/unistd.h. aarch64 has no epoll_create(1),
// no epoll_wait(2) and no eventfd(1): glibc reaches epoll_pwait with a NULL
// sigmask for epoll_wait, so only the five below ever arrive.

// 20  epoll_create1(int flags) -> fd
long lxrt_epoll_create1(int lflags);

// 21  epoll_ctl(int epfd, int op, int fd, struct epoll_event *event) -> 0
long lxrt_epoll_ctl(int epfd, int op, int fd, const void *levent);

// 22  epoll_pwait(int epfd, struct epoll_event *events, int maxevents,
//                 int timeout, const sigset_t *sigmask, size_t sigsetsize)
long lxrt_epoll_pwait(int epfd, void *levents, int maxevents, int timeout_ms,
                      const uint64_t *lsigmask, size_t sigsetsize);

// 441 epoll_pwait2(int epfd, struct epoll_event *events, int maxevents,
//                  const struct __kernel_timespec *timeout,
//                  const sigset_t *sigmask, size_t sigsetsize)
long lxrt_epoll_pwait2(int epfd, void *levents, int maxevents,
                       const void *ltimespec, const uint64_t *lsigmask,
                       size_t sigsetsize);

// 19  eventfd2(unsigned int initval, int flags) -> fd
// The Linux prototype takes an unsigned *int*; the counter is 64-bit but the
// initial value that crosses the syscall boundary is not.
long lxrt_eventfd2(unsigned initval, int lflags);

// ----------------------------------------------------------- read/write hooks
//
// An eventfd is read and written through the ordinary read(63)/write(64)
// syscalls, and what crosses is a counter, not the pipe bytes underneath. The
// dispatcher must therefore ask before touching the descriptor:
//
//     case LNR_read:
//         if (lxrt_eventfd_is((int)a0))
//             ret = lxrt_eventfd_read((int)a0, (void *)a1, (size_t)a2);
//         else
//             ret = ret_of(read(...));
//
// lxrt_eventfd_is() is a lock-free bitmap probe so it can sit in front of every
// read and write in the process without being measurable.
bool lxrt_eventfd_is(int fd);
long lxrt_eventfd_read(int fd, void *buf, size_t n);
long lxrt_eventfd_write(int fd, const void *buf, size_t n);

// ----------------------------------------------------------- close hooks
//
// All three take the guest-visible descriptor and drop the runtime-side state
// for it. None of them closes that descriptor -- the caller still does, exactly
// as it already does for lxrt_dirents_close() -- and all three must be called
// BEFORE the close(), while the descriptor is still open, because
// lxrt_epoll_fd_closed() has to name it in an EV_DELETE:
//
//     case LNR_close:
//         lxrt_dirents_close(fd);
//         lxrt_epoll_fd_closed(fd);   // fd leaving every epoll interest list
//         lxrt_epoll_close(fd);       // fd IS an epoll instance
//         lxrt_eventfd_close(fd);     // fd IS an eventfd
//         ret = ret_of(close(fd));
//
// All three are cheap no-ops for a descriptor this module does not own, which
// is almost all of them.
//
// lxrt_epoll_fd_closed() is the one that is NOT yet wired and must be. On Linux
// close() removes the descriptor from every epoll set it sits in -- that is
// what makes fd-number recycling safe for an event loop. kqueue drops the knote
// when the fd goes but tells this module nothing, so without the hook the
// interest entry outlives the descriptor and poisons its NUMBER. Measured: a
// pipe on fd 4 added to an epoll, closed, then a fresh pipe recycling fd 4 ->
// EPOLL_CTL_ADD(4) returned -EEXIST where Linux returns 0, and the epoll_pwait
// after it reported no events although fd 4 was readable -- a silently dead
// event loop on that descriptor. Steam recycles descriptors constantly.
//
// (The module also carries an st_dev/st_ino identity check as a safety net that
// works without the hook, but it can only repair state lazily, on the next
// epoll_ctl naming that fd. A waiter already blocked in epoll_pwait is not
// repaired by it. Wire the hook.)
void lxrt_epoll_fd_closed(int fd);
void lxrt_epoll_close(int fd);
void lxrt_eventfd_close(int fd);

// ----------------------------------------------------------- dup hooks
//
// An eventfd is identified here by its descriptor NUMBER, so every alias the
// guest creates has to be announced or it falls through to the host read(),
// which hands back the single 'e' readiness token byte instead of the 8-byte
// counter. Measured before the hook existed: lxrt_eventfd2(5,0) -> fd 3,
// dup(3) -> fd 5, and a raw read(5, buf, 8) returned 1 byte holding 101 -- the
// ASCII 'e' -- and ate the token, so the descriptor became permanently
// unreadable to kqueue/poll/select while its counter still said 5. On the
// 6.17.1 aarch64 guest the same read returns 8 bytes and the value 5.
//
//     case LNR_dup:
//         ret = ret_of(dup(oldfd));
//         if (ret >= 0) lxrt_eventfd_dup(oldfd, (int)ret);
//
// fcntl(F_DUPFD) and F_DUPFD_CLOEXEC need the same call.
//
// dup3 is the case with two halves, because runtime/dispatch.c implements it as
// dup2(oldfd, newfd) and dup2 implicitly CLOSES newfd. Both the close hooks and
// the dup hook are owed:
//
//     case LNR_dup3:
//         lxrt_epoll_fd_closed(newfd);   // newfd is about to be closed by dup2
//         lxrt_epoll_close(newfd);
//         lxrt_eventfd_close(newfd);
//         ret = ret_of(dup2(oldfd, newfd));
//         if (ret >= 0) lxrt_eventfd_dup(oldfd, newfd);
//
// Aliasing a descriptor this module does not own is a cheap no-op. An eventfd
// tracks at most 16 aliases; a seventeenth is not recorded and reverts to the
// raw-pipe behaviour above. That cap is stated rather than grown dynamically so
// that the call cannot fail under memory pressure while holding the global
// eventfd lock, and nothing in the measured Steam census comes near it.
void lxrt_eventfd_dup(int oldfd, int newfd);

// ----------------------------------------------------------- stated limits
//
// Divergences this module does not fix, listed so that the next reader does not
// have to rediscover them:
//
//   * fork(). A Linux epoll fd survives fork() and the child sees the same
//     interest list; a Darwin kqueue is explicitly not inherited, so the
//     descriptor exists in the child with an empty queue. Fixing it needs a
//     fork handler that re-registers every knote, and the interest list is not
//     shared memory.
//   * The sigmask swap in epoll_pwait is not atomic with the wait, which is the
//     one guarantee epoll_pwait exists to give. There is no kevent() that takes
//     a signal mask. dispatch.c's do_ppoll() has the same hole.
//   * An interest that asks for no readable data -- events=0, EPOLLPRI only,
//     EPOLLRDHUP only -- gets its EPOLLERR/EPOLLHUP report exactly once rather
//     than on every wait as Linux would, because the stand-in read knote has to
//     be edge-armed or it spins the wait loop at 100% of a core. See the
//     forced-EV_CLEAR comment in ep_changes().
//
#endif
