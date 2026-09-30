// Android identities for lxrun guests: per-process virtual credentials.
//
// Android's init starts each daemon as root and then drops it to the user,
// groups and capabilities its .rc file names; zygote (root) forks
// system_server and every app and calls setgroups, setresgid, setresuid and
// capset in the child, each fatal on failure (docs/ANDROID_ZERO_VM_
// FEASIBILITY.md 3.8). A Mac process cannot become another user. Lepton gets
// the same effect in its container with a seccomp profile that answers
// success and a bionic patch that fakes getuid (docs/LEPTON_REUSE_ANALYSIS.md
// 3.3); SteamARM does it in the runtime, for Android guests only.
//
// Switched on by LXRT_ANDROID_IDS in the environment (the boot script,
// scripts/android-boot.py, sets it per service from its .rc file):
//
//   LXRT_ANDROID_IDS=ruid,euid,suid:rgid,egid,sgid:g1,g2,...:eff,prm,inh,bnd,amb[:knb<hex>]
//
// (caps as hex masks; "k" = PR_SET_KEEPCAPS on, "n" = no_new_privs set,
// "b<hex>" = the securebits;
// "root" is uid/gid 0 with
// every capability). With it, the credential calls act on those numbers with
// Linux's rules (kernel/sys.c, security/commoncap.c): CAP_SETUID/CAP_SETGID
// decide what may change, a uid change drops capabilities unless keepcaps is
// set, execve recomputes them (ambient and root), and the state follows fork
// and execve. Nothing changes on the Mac: every file is still the Mac user's
// and every process can still signal every other one. What is emulated is
// what Android asks and checks; see docs/ANDROID_RUNTIME_ARCHITECTURE.md,
// "Identities".
//
// Other processes see the virtual ids too: SO_PEERCRED (effective ids) and
// SCM_CREDENTIALS (real ids, as Linux attaches them) in runtime/socket.c,
// capget of another pid, and the binder sender euid (runtime/binder.c), from
// a table shared by this user's lxrun processes (/tmp/lxrt-shm-<uid>/
// android-ids.v2), keyed by pid and checked against the process start time.
// SO_PEERCRED answers with the peer's ids now, not those it had when it
// connected, as Linux would.
#ifndef LXRT_ANDROID_IDS_H
#define LXRT_ANDROID_IDS_H

#include <stdbool.h>
#include <stdint.h>

// Reads LXRT_ANDROID_IDS once. True when this process runs with Android ids.
bool lxrt_aids_on(void);

// A credential syscall (aarch64 numbers: set/get[res]uid/gid, setfs*,
// get/setgroups, capget, capset, and prctl's capability options): true when
// handled, with the Linux return value in *ret.
bool lxrt_aids_syscall(long nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, long *ret);

uint32_t lxrt_aids_euid(void);   // host geteuid() when off
uint32_t lxrt_aids_egid(void);
// The Uid:/Gid:/Groups: lines of /proc/<pid>/status: real, effective, saved,
// filesystem ids, and the supplementary groups (space-separated). The host's
// ids when off. Android's Process.getUidForPid reads Uid: and
// ActivityManagerService.isProcessAliveLocked compares it with the process
// record's uid: with the Mac's 501 there, every provider looked like it was
// in a dying process (benchmarks/stage28-android-apk.txt).
void lxrt_aids_status_ids(uint32_t u[4], uint32_t g[4], char *groups, size_t groups_len);
// Linux's capable(): is `cap` in the effective set? (false when off)
bool lxrt_aids_capable(int cap);

// Virtual file ownership (stage 28). Every file is the Mac user's, but
// Android checks owners it has set: installd's fs_prepare_dir_strict refuses
// an app's existing data directory whose stat does not show the app's uid
// ("Expected path /data/data/<pkg> with owner 1000:1000 but found 501:20",
// benchmarks/stage28-android-apk.txt), so every boot after the first lost
// every app's data. A chown that lxrt_aids_chown allows records the owner in
// a Darwin extended attribute of the host file (LXRT_OWNER_XATTR, a name no
// guest can reach: guest xattr calls are ENOTSUP), and stat, fstat, fstatat
// and statx report it to guests with Android ids. A file never chowned shows
// the Mac's ids, as before; a chown back to the Mac's own ids removes the
// record. Without Android ids nothing is read or written.
#define LXRT_OWNER_XATTR "org.steamarm.lxrt.owner"

// chown in Android mode (only meaningful when lxrt_aids_on()): Linux's rules
// for these ids against the file's virtual owner (CAP_CHOWN, or the owner
// keeping its uid and giving the file to one of its groups: fs/attr.c
// chown_ok/chgrp_ok), then uid/gid (0xffffffff: keep that half) recorded
// for a host path (nofollow: the link itself) or, with host NULL, an open
// host descriptor. The file itself stays the Mac user's. Returns 0 or the
// Linux error.
long lxrt_aids_chown_file(const char *host, int fd, bool nofollow, uint32_t uid, uint32_t gid);

// Replace st_uid/st_gid with the recorded owner, if any (Android ids only).
struct stat;
void lxrt_aids_fix_stat(const char *host, int fd, bool nofollow, struct stat *st);

// The same two for a translated host path relative to a Darwin directory
// descriptor (AT_FDCWD: the working directory), as the *at calls give them.
long lxrt_aids_chown_at(int ddirfd, const char *host, bool nofollow, uint32_t uid, uint32_t gid);
void lxrt_aids_fix_stat_at(int ddirfd, const char *host, bool nofollow, struct stat *st);

// The string to put in the environment of the next image at execve, with
// Linux's execve capability rules applied ("LXRT_ANDROID_IDS=..."), or NULL
// when off.
const char *lxrt_aids_exec_env(void);

// The virtual ids of another lxrun process of this user, if it runs with
// Android ids (the shared table, a consistent snapshot). False otherwise.
struct lxrt_aids_peer { uint32_t ruid, euid, rgid, egid; uint64_t eff, prm, inh; };
bool lxrt_aids_lookup(int pid, struct lxrt_aids_peer *out);

#endif
