// The remaining file, process and scheduling syscalls.
//
// Split out of dispatch.c rather than added to it because every one of these
// carries a Linux/Darwin number or layout divergence that needs its own
// comparison written down; see fileops2.c for the tables.
//
// TWO CALLING CONVENTIONS THE INTEGRATOR MUST HONOUR:
//
//   1. PATHS ARE ALREADY TRANSLATED. dispatch.c's translate() is static and
//      returns a thread-local buffer, so it cannot be called from here. Every
//      `tpath` argument below must be the OUTPUT of translate(), not the raw
//      guest pointer. A guest path that is NULL must be rejected by the caller
//      before translation -- translate() dereferences it.
//
//      The thread-local buffer also means that a two-path call cannot hold two
//      translations at once: lxrt_linkat() takes both paths, and the caller has
//      to copy the first into its own storage before translating the second.
//      dispatch.c already does exactly that for renameat.
//
//   2. FDs AND FLAGS ARE THE GUEST'S. Linux dirfd/AT_*/O_*/MSG_* values arrive
//      unconverted; this module converts them. Do not pre-translate them.
//
// Every entry point returns a Linux return value: >= 0 on success, or the
// NEGATED LINUX errno on failure. Never a Darwin errno.

#ifndef LXRT_FILEOPS2_H
#define LXRT_FILEOPS2_H

#include <stdint.h>
#include <pthread.h>

// Internal lifecycle hook: discard nice state before a thread handle is reused.
void lxrt_fileops2_thread_exited(pthread_t th);

// ---------------------------------------------------------------- locking

// 32 flock. FEX takes LOCK_EX on its code-map file before it will run at all,
// so this one is load-bearing rather than completeness.
long lxrt_flock(int fd, int loperation);

// ---------------------------------------------------------------- process

// 95 waitid. `uinfop` receives a 128-byte Linux siginfo_t; `urusage`, if not
// NULL, a 144-byte Linux struct rusage. Both may be NULL.
//
// The rusage form has to go through wait4, which always reaps an exited child,
// so it refuses WNOWAIT and refuses options without WEXITED with ENOSYS rather
// than reaping a child the guest did not ask about. It also cannot fill si_uid.
long lxrt_waitid(int lidtype, int id, void *uinfop, int loptions,
                 void *urusage);

// ---------------------------------------------------------------- files

// 437 openat2. `uhow` points at the guest's struct open_how; `size` is its
// declared size, which the guest uses for versioning and which is checked.
// Strict, as Linux is: unknown flag bits, a bad mode, a malformed O_TMPFILE and
// an unsupportable RESOLVE_* bit all fail instead of being dropped. O_PATH and
// O_TMPFILE have no Darwin equivalent and return EOPNOTSUPP; see fileops2.c.
long lxrt_openat2(int ldirfd, const char *tpath, const void *uhow,
                  uint64_t size);

// 291 statx. `ubuf` receives the 256-byte Linux struct statx. `tpath` may be
// "" when lflags carries AT_EMPTY_PATH, in which case ldirfd is stat'd.
long lxrt_statx(int ldirfd, const char *tpath, int lflags, unsigned mask,
                void *ubuf);

// 43 statfs / 44 fstatfs. `ubuf` receives the 120-byte Linux struct statfs.
long lxrt_statfs(const char *tpath, void *ubuf);
long lxrt_fstatfs(int fd, void *ubuf);

// 47 fallocate. Only mode 0, FALLOC_FL_KEEP_SIZE and
// FALLOC_FL_PUNCH_HOLE|FALLOC_FL_KEEP_SIZE are supported; see fileops2.c.
// A range reaching into the existing file is honoured only when the file is
// provably dense: Darwin's F_PREALLOCATE allocates past EOF only, so a hole
// below the current size cannot be filled and the call returns EOPNOTSUPP
// instead of a success the guest would write into.
long lxrt_fallocate(int fd, int lmode, int64_t offset, int64_t len);

// 83 fdatasync.
long lxrt_fdatasync(int fd);

// 37 linkat. Both paths already translated, and both must remain valid for the
// duration of the call -- see convention 1 above.
long lxrt_linkat(int lolddirfd, const char *toldpath,
                 int lnewdirfd, const char *tnewpath, int lflags);

// 439 faccessat2. Unlike faccessat, this one takes a real flags word.
long lxrt_faccessat2(int ldirfd, const char *tpath, int mode, int lflags);

// ---------------------------------------------------------------- sockets

// 269 sendmmsg / 243 recvmmsg. `umsgvec` points at the guest's array of Linux
// struct mmsghdr (64 bytes each: a 56-byte Linux msghdr then msg_len).
// `utimeout` is a Linux struct __kernel_timespec, or NULL.
long lxrt_sendmmsg(int fd, void *umsgvec, unsigned vlen, int lflags);
long lxrt_recvmmsg(int fd, void *umsgvec, unsigned vlen, int lflags,
                   const void *utimeout);

// ---------------------------------------------------------------- scheduling
//
// Linux's sched_* calls address THREADS: the `pid` argument is a tid, and 0
// means the caller. These resolve it through thread.c's registry, so a guest
// thread other than the caller can be targeted.
//
// Priorities crossing this boundary are always LINUX priorities. Darwin's
// usable band is 15..47 on this machine and Linux's is 1..99 for the realtime
// policies; the mapping happens inside, so a guest never sees a Darwin number.

// 125 sched_get_priority_max / 126 sched_get_priority_min.
long lxrt_sched_get_priority_max(int lpolicy);
long lxrt_sched_get_priority_min(int lpolicy);

// 119 sched_setscheduler. `uparam` is a Linux struct sched_param, which is a
// single int sched_priority.
long lxrt_sched_setscheduler(int pid, int lpolicy, const void *uparam);

// 120 sched_getscheduler. Returns the Linux policy number.
long lxrt_sched_getscheduler(int pid);

// 121 sched_getparam.
long lxrt_sched_getparam(int pid, void *uparam);

// 140 setpriority / 141 getpriority. getpriority returns Linux's BIASED form
// (20 - nice), not the nice value; see fileops2.c.
//
// `who` IS THE GUEST'S NUMBER AND IS NOT A HOST PID. Under PRIO_PROCESS it is a
// guest tid (0 = caller, the guest's getpid() = the thread group) and is
// resolved through thread.c's registry; an unregistered value is ESRCH. Under
// PRIO_PGRP only this process's own group is addressable. Only PRIO_USER, whose
// namespace really is shared with the host, reaches Darwin unchanged.
long lxrt_setpriority(int which, int who, int lprio);
long lxrt_getpriority(int which, int who);

// ---------------------------------------------------------------- credentials
//
// Numbers are the aarch64 asm-generic ones, which are NOT the pairing the task
// sheet gave: 147 setresuid, 148 getresuid, 149 setresgid, 150 getresgid.
// (151 is setfsuid, not setresgid.) Verified against
// include/uapi/asm-generic/unistd.h.
long lxrt_setresuid(int32_t ruid, int32_t euid, int32_t suid);
long lxrt_getresuid(void *uruid, void *ueuid, void *usuid);
long lxrt_setresgid(int32_t rgid, int32_t egid, int32_t sgid);
long lxrt_getresgid(void *urgid, void *uegid, void *usgid);

#endif
