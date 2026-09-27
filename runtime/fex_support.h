// The three syscalls FEX issues on its own behalf, rather than on the guest's.
//
// They are on the critical path for the x86 story: Steam's client is x86, FEX
// is what runs it, and until these three answer something other than -ENOSYS
// FEX does not get far enough to load anything. None of the three has a Darwin
// equivalent that can simply be called.

#ifndef LXRT_FEX_SUPPORT_H
#define LXRT_FEX_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// ------------------------------------------------------- 279 memfd_create
//
// Built out of an unlinked file in the runtime's temp directory. shm_open was
// measured and rejected; fex_support.c records the numbers.
//
// The seals are runtime state, not kernel state. Everything below that says
// "check" is a gate the dispatcher has to call *before* performing the Darwin
// operation, because Darwin will happily do it otherwise. See the seal section
// of fex_support.c for what that weakens.

long lxrt_memfd_create(const char *name, unsigned int lflags);

// True if this fd number is one of ours. The dispatcher needs it to decide
// whether fcntl(F_ADD_SEALS) is a memfd operation or a bad command.
bool lxrt_memfd_is(int fd);
bool lxrt_memfd_path_is(int fd);   // any memfd file, also one received from another process

// fcntl(F_ADD_SEALS)/fcntl(F_GET_SEALS), which are Linux-only commands.
// *handled is set false when lcmd is neither, and the caller should carry on
// with its normal fcntl path.
long lxrt_memfd_fcntl(int fd, int lcmd, unsigned long arg, bool *handled);

// Seal gates. Each returns 0 when the operation is permitted and a negative
// Linux errno when a seal forbids it. A non-memfd fd is always permitted, so
// these are safe to call unconditionally on the hot path.
//
// WHICH OF THESE ARE ACTUALLY WIRED, because a gate nobody calls is a seal that
// only looks enforced. Audited against dispatch.c:
//
//   lxrt_memfd_check_ftruncate  WIRED    (ftruncate)
//   lxrt_memfd_check_write      PARTIAL  (write only -- pwrite, writev and
//                                         pwritev reach the file with no gate)
//   lxrt_memfd_check_mmap       NOT WIRED -- no caller anywhere in the tree
//   lxrt_memfd_is               NOT WIRED -- no caller anywhere in the tree
//
// So today F_SEAL_WRITE stops write() and ftruncate() and does NOT stop
// mmap(PROT_WRITE, MAP_SHARED), which is precisely the path the security note
// above says a caller may be trusting, nor the other three write syscalls.
// These are dispatcher changes, not changes to this module. Until they are
// made, treat F_SEAL_WRITE here as advisory against write()/ftruncate() only.
long lxrt_memfd_check_ftruncate(int fd, int64_t length);
// offset < 0 means "wherever the fd is positioned"; count is the byte count.
long lxrt_memfd_check_write(int fd, int64_t offset, uint64_t count);
// prot and map_flags are the Linux values, which for PROT_* and MAP_SHARED
// happen to equal Darwin's. Call this before the Darwin mmap.
long lxrt_memfd_check_mmap(int fd, int prot, int lmap_flags);

// Seals live on the Linux inode, so every dup of a memfd shares them. fd
// numbers are also recycled, so an untracked close makes an unrelated later fd
// inherit a memfd's seals -- both of these must be wired up.
void lxrt_memfd_track_dup(int oldfd, int newfd);
void lxrt_memfd_close(int fd);

// ----------------------------------------------------------- 281 execveat
//
// Resolves dirfd + path to something Darwin can name and hands it to
// lxrt_execve, which re-executes the runtime on it.
long lxrt_execveat(int dirfd, const char *path, char *const argv[],
                   char *const envp[], int lflags);

// ---------------------------------------------------- 137 rt_sigtimedwait
//
// uset and utimeout are Linux layouts read from guest memory; uinfo is a
// 128-byte Linux siginfo_t written back, or NULL. sigsetsize must be 8.
// Returns the Linux signal number, or a negative Linux errno (-EAGAIN on
// timeout, as Linux does).
//
// -EAGAIN means the deadline really elapsed and nothing else. If the deadline
// machinery itself cannot be started this returns -ENOMEM, which Linux never
// returns here -- deliberately, because a guest that reads a fabricated EAGAIN
// as expiry will loop on it and spin.
long lxrt_rt_sigtimedwait(const void *uset, void *uinfo, const void *utimeout,
                          size_t sigsetsize);

#endif
