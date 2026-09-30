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
//   LXRT_ANDROID_IDS=ruid,euid,suid:rgid,egid,sgid:g1,g2,...:eff,prm,inh,bnd,amb[:kn]
//
// (caps as hex masks; "k" = PR_SET_KEEPCAPS on, "n" = no_new_privs set;
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
// Other processes see the virtual ids too: SO_PEERCRED and SCM_CREDENTIALS
// (runtime/socket.c) and the binder sender euid (runtime/binder.c) come from
// a table shared by this user's lxrun processes (/tmp/lxrt-shm-<uid>/
// android-ids), keyed by pid and checked against the process start time.
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
// Linux's capable(): is `cap` in the effective set? (false when off)
bool lxrt_aids_capable(int cap);

// chown in Android mode: 1 = do the real call (nothing to fake: the ids
// asked for are the host's own or -1), 0 = report success without changing
// the file (allowed by Linux's rules for these ids), negative = the Linux
// error. Only meaningful when lxrt_aids_on().
int lxrt_aids_chown(uint32_t uid, uint32_t gid);

// The string to put in the environment of the next image at execve, with
// Linux's execve capability rules applied ("LXRT_ANDROID_IDS=..."), or NULL
// when off.
const char *lxrt_aids_exec_env(void);

// The virtual uid/gid of another lxrun process of this user, if it runs with
// Android ids (the shared table). False otherwise.
bool lxrt_aids_lookup(int pid, uint32_t *uid, uint32_t *gid);

#endif
