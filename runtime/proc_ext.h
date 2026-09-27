// proc_ext -- the second half of the synthetic /proc.
//
// procfs.c owns the per-process files that only the runtime can know (exe,
// cmdline, status, maps, fd/). This owns the machine-description and
// kernel-tunable files that Steam, pressure-vessel, srt-bwrap and glibc read
// but that procfs.c does not produce. The two are deliberately separate files
// so that neither has to be edited to extend the other; they share one
// runtime-owned directory and nothing else.
//
// The contract is the same as lxrt_proc_translate: hand it a guest path, get
// back a host path, or NULL if the path is not ours and the caller should
// carry on with whatever it would have done.

#ifndef LXRT_PROC_EXT_H
#define LXRT_PROC_EXT_H

#include <stddef.h>
#include <stdint.h>

// Materialise the /proc file `path` names, if it is one of ours, inside the
// runtime's procfs directory `dir` (the directory lxrt_proc_init created), and
// return the host path to it. Volatile files -- meminfo, stat, vmstat, uptime,
// loadavg, self/stat, self/statm, the random uuid -- are regenerated on every
// call, because the caller's next open() must not see the previous answer.
//
// Returns NULL when the path is not one this module provides, and ALSO when
// it is one of ours but could not be generated -- TMPDIR full, fd table
// exhausted, a sandbox that revoked write on the directory. Both cases mean
// the caller should carry on and let the open() fail with ENOENT. The one
// thing this never does is return a path whose content is a previous
// generation: a stale /proc/meminfo is indistinguishable from a fresh one to
// the guest, which makes it worse than an error.
//
// Call it AFTER lxrt_proc_translate: that one answers the paths procfs.c owns,
// and the two sets do not overlap.
//
// procfs.c exports no accessor for its directory, and does not need one:
// lxrt_proc_translate("/proc") already returns exactly that path, so the
// caller can obtain `dir` without either file being edited.
//
// The returned pointer is thread-local and valid until this thread calls
// again, which matches what dispatch.c's translate() already does with the
// value procfs.c returns.
const char *lxrt_proc_ext_translate(const char *path, const char *dir);

// Hand this module the real auxiliary vector so /proc/self/auxv can be the
// truth rather than a plausible reconstruction. `auxv` points at the array of
// {type, value} 64-bit pairs terminated by AT_NULL that lxrt_build_stack wrote
// onto the guest stack -- that is, at the first word after the NULL that ends
// envp. Safe to call from main.c once the stack is built, and safe never to
// call at all: without it the module emits a measured minimum set instead.
//
// Returns 0, or a negative *Linux* errno. The pointer is probed rather than
// trusted, so a bad one yields -EFAULT instead of killing the host process.
//
// All or nothing: the vector is staged in full before anything is published,
// so a call that fails part-way leaves whatever /proc/self/auxv held before it
// completely untouched. A caller that checks the return value and falls back
// to the measured minimum is therefore choosing between two coherent vectors,
// never between one and a splice of two.
long lxrt_proc_ext_set_auxv(const void *auxv);

#endif
