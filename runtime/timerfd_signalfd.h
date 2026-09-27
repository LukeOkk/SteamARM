#ifndef LXRT_TIMERFD_SIGNALFD_H
#define LXRT_TIMERFD_SIGNALFD_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Linux aarch64 ABI, independent of Darwin's siginfo_t. */
struct signalfd_siginfo {
    uint32_t ssi_signo;
    int32_t ssi_errno, ssi_code;
    uint32_t ssi_pid, ssi_uid;
    int32_t ssi_fd;
    uint32_t ssi_tid, ssi_band, ssi_overrun, ssi_trapno;
    int32_t ssi_status, ssi_int;
    uint64_t ssi_ptr, ssi_utime, ssi_stime, ssi_addr;
    uint16_t ssi_addr_lsb, __pad2;
    int32_t ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_arch;
    uint8_t __pad[28];
};
_Static_assert(sizeof(struct signalfd_siginfo) == 128, "signalfd ABI");

/* 85, 86, 87 and 74 respectively. All errors are negative Linux errno. */
long lxrt_timerfd_create(int clockid, int lflags);
long lxrt_timerfd_settime(int fd, int lflags, const void *new_lspec, void *old_lspec);
long lxrt_timerfd_gettime(int fd, void *cur_lspec);
bool lxrt_timerfd_is(int fd);
long lxrt_timerfd_read(int fd, void *buf, size_t len);
void lxrt_timerfd_close(int fd);
void lxrt_timerfd_dup(int oldfd, int newfd);
long lxrt_signalfd4(int fd, const uint64_t *lmask, size_t sizemask, int lflags);
bool lxrt_signalfd_is(int fd);
long lxrt_signalfd_read(int fd, void *buf, size_t len);
void lxrt_signalfd_close(int fd);
void lxrt_signalfd_dup(int oldfd, int newfd);

/* Optional signal.c hook: 1 = consumed one matching pending signal, 0 = none,
 * negative Linux errno = failure. Must not block; fills all 128 bytes.
 * Runtime-only queued signals also need a kqueue wakeup for epoll readiness.
 */
int lxrt_signal_dequeue_pending(uint64_t lmask, struct signalfd_siginfo *out)
    __attribute__((weak_import));

/* Integration: dispatch read before host read; close hooks BEFORE host close
 * (these hooks only release metadata); dup hooks AFTER successful dup/dup3/
 * F_DUPFD/F_DUPFD_CLOEXEC. On replacement release destination metadata too.
 * The guest fd is a kqueue, directly watchable with EVFILT_READ by epoll.
 * Limits: kqueues do not survive fork; realtime clock jumps after settime are
 * not tracked; BOOTTIME uses MONOTONIC. Native signal fallback lacks sender
 * metadata and requires blocked signals; runtime queues require the hook.
 * Darwin has no sigtimedwait: fallback sigpending+sigwait can race another
 * consumer of a process-pending signal. Use the runtime hook for coordinated
 * multithreaded consumption. A runtime-only queue must arrange EVFILT_USER
 * wakeups (including remaining queued records) for epoll/blocking reads.
 * Dup metadata allocation failure cannot be returned by the void dup API;
 * in that exceptional case the new descriptor is not tracked.
 */
#endif

// signal.c calls this after a thread-directed delivery so signalfd readers
// (and the epoll sets watching them) wake up: kqueue's signal filter does not
// see pthread_kill().
void lxrt_signalfd_notify(int lsig);
