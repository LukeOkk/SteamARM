// Per-process mount table: what a fake bwrap leaves behind.
//
// pressure-vessel builds a sandbox with bwrap: a fresh root, a few dozen bind
// mounts, some symlinks, a tmpfs or two, /proc, and then execs the game. On
// Darwin there are no mount namespaces, so the plan is interpreted instead:
// the directories, symlinks and data files are materialised in a fresh host
// directory that becomes the child's LXRT_ROOT, and the binds become entries
// in this table, consulted by the dispatcher's path translation before the
// root prefix. Longest destination prefix wins, which is bwrap's own
// later-mount-shadows-earlier order.
//
// The table travels to children in the environment (LXRT_MOUNTS), because
// every exec re-runs lxrun.
#ifndef LXRT_MOUNTS_H
#define LXRT_MOUNTS_H
#include <stdbool.h>
#include <stddef.h>

// Read LXRT_MOUNTS (if any) at start-up.
void lxrt_mounts_load_env(void);
// Translate a guest-absolute path through the table. Returns a host-absolute
// path in buf, or NULL when no entry covers it.
const char *lxrt_mounts_translate(const char *path, char *buf, size_t n);
bool lxrt_mounts_active(void);
bool lxrt_mounts_readonly(const char *path);
const char *lxrt_mounts_untranslate(const char *host, char *buf, size_t n);
// A bind mount made by the guest itself (Android ids: mount(MS_BIND) in the
// zygote's private "namespace", runtime/android_ids.h): dst is a guest path,
// src the host path it shows. A later bind on the same dst replaces it.
void lxrt_mounts_bind(const char *dst, const char *src, bool ro);
// umount2 of a bind made above: true when there was one.
bool lxrt_mounts_unbind(const char *dst);
// "LXRT_MOUNTS=..." for the next image (execve), or NULL when the table is
// empty. Valid until the next call.
const char *lxrt_mounts_exec_env(void);
// Interpret a bwrap command line (argv[0] is bwrap itself) and exec its
// command through `exec_guest`. Returns only on failure, with -errno (Linux).
long lxrt_bwrap_exec(char *const argv[], char *const envp[],
                     long (*exec_guest)(const char *path, char *const argv[],
                                        char *const envp[]));
#endif
