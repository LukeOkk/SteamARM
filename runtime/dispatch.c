// Linux syscall dispatcher.
//
// The set implemented here is not a guess: it is the head of the distribution
// measured on the running system (PERFORMANCE_BASELINE §2.3,
// benchmarks/stage1-syscall-mix.txt), where nine calls cover 92% of everything
// the guest makes. clock_gettime alone is 70%, and it is answered here without
// entering the kernel at all -- measured at 0.9 ns against 68 ns for a call
// that does reach it.
//
// Anything not implemented returns -ENOSYS *and is recorded*, so a run that
// fails reports which call it lacked rather than dying anonymously.

#include "lxrt.h"
#include "ids.h"
#include "android_ids.h"
#include "mounts.h"
#include "offmap.h"

#include <errno.h>
#include <dirent.h>
#include <dlfcn.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <fcntl.h>
#include <libkern/OSCacheControl.h>
#include <limits.h>
#include <mach/mach_time.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/param.h>
#include <libproc.h>
#include <time.h>
#include <sys/sysctl.h>
#include <mach/mach_host.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/xattr.h>
#include <pthread.h>
#include <unistd.h>

#include "binder.h"
#include "props.h"

// Private syscall numbers, far outside the Linux range (which ends around 463).
// These are the graphics bridge: a guest ELF asks the runtime for the address
// of a Mach-O symbol and then calls it with a plain `blr`. After the lookup
// there is no marshalling, no IPC and no serialisation -- AAPCS64 is the same
// calling convention on both sides, and both sides are in one address space.
// This is the whole reason ZERO-VM can be simpler than the VM-era GPU
// forwarding it replaced, so it is a first-class mechanism, not a hack.
enum {
    LNR_lxrt_dlopen = 0x4C580001,   // (path, flags)   -> host handle
    LNR_lxrt_dlsym  = 0x4C580002,   // (handle, name)  -> host function pointer
    LNR_lxrt_dlerror = 0x4C580003,  // (buf, len)      -> bytes written
    LNR_lxrt_window  = 0x4C580010,  // (w, h, title)   -> CAMetalLayer *
    LNR_lxrt_drawable = 0x4C580011, // (uint32*, uint32*) -> 0
    LNR_lxrt_rlayer_create = 0x4C580012,  // (w, h, uint32_t *ctx) -> CAMetalLayer * (remote_layer.m)
    LNR_lxrt_rlayer_resize = 0x4C580013,  // (layer, w, h) -> 0
    LNR_lxrt_rlayer_release = 0x4C580014, // (layer) -> 0
    LNR_lxrt_mfx_encode = 0x4C580015,     // (struct lxrt_mfx_run *) -> 0 or -errno (metalfx.m)
    LNR_lxrt_mfx_release = 0x4C580016,    // (scaler) -> 0
    LNR_lxrt_metal_attach = 0x4C580017,   // () -> 0: Metal's compiler for a forked child (process.c)
    LNR_lxrt_jit_wx  = 0x4C580020,  // (enable, addr, len) -> 0
    LNR_lxrt_guest_base = 0x4C580030, // (base) -> 0: 32-bit guest lives at host = base + guest
    LNR_lxrt_guest_base_get = 0x4C580031, // () -> base, 0 if none (the Vulkan shim's rebasing)
    LNR_lxrt_alias = 0x4C580032,          // (src, len, dst) -> 0: dst becomes a shared alias of src
};

long lxrt_metal_attach(void);   // process.c

// metalfx.m
struct lxrt_mfx_run;
long lxrt_mfx_encode(struct lxrt_mfx_run *r);
void lxrt_mfx_release(void *scaler);

// window.m
void *lxrt_window_create(int w, int h, const char *title);
void  lxrt_window_drawable_size(uint32_t *w, uint32_t *h);

// Linux aarch64 syscall numbers (asm-generic).
enum {
    LNR_ioctl = 29, LNR_openat = 56, LNR_close = 57, LNR_lseek = 62,
    LNR_read = 63, LNR_write = 64, LNR_writev = 66, LNR_readlinkat = 78,
    LNR_exit = 93, LNR_exit_group = 94, LNR_set_tid_address = 96,
    LNR_personality = 92, LNR_getcpu = 168, LNR_getdents64 = 61,
    // epoll and eventfd: 46 + 49 live descriptors in Steam's process tree, and
    // epoll_pwait is the 4th most frequent syscall on the machine.
    LNR_eventfd2_n = 19, LNR_epoll_create1 = 20, LNR_epoll_ctl = 21,
    LNR_epoll_pwait = 22, LNR_epoll_pwait2 = 441,
    // System V IPC: Valve's own libtier0_s.so imports semget/semop/semctl.
    LNR_semget = 190, LNR_semctl = 191, LNR_semtimedop = 192, LNR_semop = 193,
    LNR_shmget = 194, LNR_shmctl = 195, LNR_shmat = 196, LNR_shmdt = 197,
    // flock is where FEX currently blocks, guarding its code map file.
    LNR_flock = 32, LNR_waitid = 95, LNR_openat2 = 437, LNR_statx = 291,
    LNR_statfs = 43, LNR_fstatfs = 44, LNR_fallocate = 47, LNR_fdatasync = 83,
    LNR_linkat_n = 37, LNR_faccessat2 = 439, LNR_sendmmsg = 269,
    LNR_recvmmsg = 243, LNR_sched_get_priority_max = 125,
    LNR_sched_get_priority_min = 126, LNR_sched_setscheduler = 119,
    LNR_sched_getscheduler = 120, LNR_sched_getparam = 121,
    LNR_setpriority = 140, LNR_getpriority = 141,
    LNR_setregid = 143, LNR_setgid = 144, LNR_setreuid = 145, LNR_setuid = 146,
    LNR_setfsuid = 151, LNR_setfsgid = 152,
    LNR_setresuid = 147, LNR_getresuid = 148, LNR_setresgid = 149,
    LNR_getresgid = 150,
    // The three FEX calls directly.
    LNR_memfd_create = 279, LNR_execveat = 281, LNR_rt_sigtimedwait = 137, LNR_getcwd = 17, LNR_mkdirat = 34, LNR_unlinkat = 35, LNR_renameat = 38,
    LNR_renameat2 = 276, LNR_chdir = 49, LNR_fchdir = 50, LNR_fchmod = 52,
    LNR_fchmodat = 53, LNR_mknodat = 33, LNR_fchownat = 54, LNR_fchown = 55, LNR_symlinkat = 36,
    LNR_linkat = 37, LNR_ftruncate = 46, LNR_pread64 = 67, LNR_pwrite64 = 68,
    LNR_fcntl = 25, LNR_prctl = 167, LNR_umask = 166, LNR_fsync = 82,
    LNR_getuid = 174, LNR_geteuid = 175, LNR_getgid = 176, LNR_getegid = 177,
    LNR_socket = 198, LNR_socketpair = 199, LNR_bind = 200, LNR_listen = 201,
    LNR_accept = 202, LNR_connect = 203, LNR_getsockname = 204,
    LNR_getpeername = 205, LNR_sendto = 206, LNR_recvfrom = 207,
    LNR_setsockopt = 208, LNR_getsockopt = 209, LNR_shutdown = 210,
    LNR_sendmsg = 211, LNR_recvmsg = 212, LNR_accept4 = 242,
    LNR_pipe2 = 59, LNR_eventfd2 = 19,
    // Extended attributes: Darwin has them, but the namespaces and security
    // semantics differ. "user." names map to the same Darwin names
    // (do_xattr); the other namespaces are ENOTSUP, the answer a filesystem
    // without them gives on Linux, which every caller handles.
    LNR_setxattr = 5, LNR_lsetxattr = 6, LNR_fsetxattr = 7, LNR_getxattr = 8,
    LNR_lgetxattr = 9, LNR_fgetxattr = 10, LNR_listxattr = 11, LNR_llistxattr = 12,
    LNR_flistxattr = 13, LNR_removexattr = 14, LNR_lremovexattr = 15,
    LNR_fremovexattr = 16,
    // fd families that Steam's event loop lives on, and mremap for glibc's
    // large realloc (benchmarks/stage6-steam-gap.txt, P2).
    LNR_inotify_init1 = 26, LNR_inotify_add_watch = 27, LNR_inotify_rm_watch = 28,
    LNR_signalfd4 = 74, LNR_timerfd_create = 85, LNR_timerfd_settime = 86,
    LNR_timerfd_gettime = 87, LNR_timer_create = 107, LNR_timer_gettime = 108,
    LNR_timer_getoverrun = 109, LNR_timer_settime = 110, LNR_timer_delete = 111,
    LNR_mremap = 216,
    LNR_execve = 221, LNR_wait4 = 260,
    LNR_clone = 220, LNR_clone3 = 435, LNR_futex = 98, LNR_tgkill = 131,
    LNR_madvise = 233, LNR_mincore = 232, LNR_dup = 23, LNR_dup3 = 24,
    LNR_sched_getaffinity = 123, LNR_membarrier = 283,
    LNR_rt_sigaction = 134, LNR_sigaltstack = 132, LNR_getppid = 173,
    LNR_kill = 129, LNR_rt_sigreturn = 139, LNR_rt_sigsuspend = 133,
    LNR_getitimer = 102, LNR_setitimer = 103, LNR_ppoll = 73, LNR_pselect6 = 72, LNR_gettimeofday = 169,
    LNR_setpgid = 154, LNR_getpgid = 155, LNR_sysinfo = 179,
    LNR_faccessat = 48, LNR_nanosleep = 101, LNR_sched_yield = 124,
    LNR_clock_nanosleep = 115, LNR_set_robust_list = 99, LNR_get_robust_list = 100, LNR_clock_gettime = 113,
    LNR_rt_sigprocmask = 135, LNR_uname = 160, LNR_getpid = 172,
    LNR_gettid = 178, LNR_brk = 214, LNR_munmap = 215, LNR_mmap = 222,
    LNR_capget = 90, LNR_capset = 91,
    LNR_mprotect = 226, LNR_prlimit64 = 261, LNR_getrandom = 278,
    LNR_getrlimit = 163, LNR_setrlimit = 164, LNR_getgroups = 158, LNR_setgroups = 159,
    LNR_fstat = 80, LNR_fstatat = 79,
};

static bool g_trace;
static bool g_rewrite_mapped;

void lxrt_dispatch_set_rewrite_mapped(bool on) { g_rewrite_mapped = on; }
// Whether code the guest maps (or writes) is rewritten: subpage.c asks before
// rescanning code stored into an executable 4 KiB page.
bool lxrt_dispatch_rewrite_mapped(void) { return g_rewrite_mapped; }

// ------------------------------------------------- guest filesystem root
//
// A dynamic Linux binary asks for /lib/ld-linux-aarch64.so.1 and /lib64/libc.so.6
// by absolute path. Those do not exist on macOS, so LXRT_ROOT names a directory
// holding a Linux tree and every absolute path the guest opens is resolved
// inside it. Unset, paths pass through to the host untouched.

static const char *guest_root(void)
{
    static const char *root;
    static bool looked;
    if (!looked) {
        root = getenv("LXRT_ROOT");
        if (root && !*root)
            root = NULL;
        looked = true;
    }
    return root;
}

static const char *translate_one(const char *path, char *buf, size_t n);

// The canonical host spelling of a path that may not exist yet: realpath() of
// its longest existing prefix, with the rest appended. /tmp/lxrt-steamroot
// and ~/SteamARM-roots/steamroot (and /private/tmp) are one directory.
static bool host_canon(const char *p, char *out, size_t n)
{
    char cur[PATH_MAX];
    if (snprintf(cur, sizeof cur, "%s", p) >= (int)sizeof cur)
        return false;
    size_t len = strlen(cur);
    for (;;) {
        char real[PATH_MAX];
        if (realpath(cur, real)) {
            return snprintf(out, n, "%s%s", real, p + len) < (int)n;
        }
        char *slash = strrchr(cur, '/');
        if (!slash || slash == cur)
            return false;
        *slash = '\0';
        len = (size_t)(slash - cur);
    }
}

// This process's root, canonical, computed once.
static const char *guest_root_canon(void)
{
    static char canon[PATH_MAX];
    static int state;   // 0 unknown, 1 set, -1 none
    if (!state) {
        const char *r = guest_root();
        state = (r && realpath(r, canon)) ? 1 : -1;
    }
    return state > 0 ? canon : NULL;
}

// Absolute symlinks are guest paths, and the host kernel would resolve them
// against the host's "/". Inside a bwrap sandbox that is always wrong
// (pressure-vessel links /usr/lib64/ld-linux-x86-64.so.2 to
// /run/host/usr/lib/..., which exists only in the sandbox's table), so every
// component of an absolute guest path is checked here and an absolute target
// is followed in guest terms. A target created by symlinkat's host rewrite
// (below) is recognised by its root prefix and turned back into a guest path.
// The last component is followed only when the caller follows links.
// Relative targets are left to the kernel. /proc and /dev are not walked.
static const char *guest_resolve(const char *path, bool follow_last, char *out, size_t outn)
{
    const char *root = guest_root();
    if (!path || path[0] != '/' || (!root && !lxrt_mounts_active()) ||
        !strncmp(path, "/proc", 5) || !strncmp(path, "/dev", 4))
        return path;
    char cur[1024];
    if (snprintf(cur, sizeof cur, "%s", path) >= (int)sizeof cur)
        return path;
    size_t rl = root ? strlen(root) : 0;
    bool changed_any = false;
    for (int hops = 0; hops < 40; hops++) {
        bool changed = false;
        for (char *p = cur + 1; ; p++) {
            if (*p != '/' && *p != '\0')
                continue;
            char save = *p;
            if (!save && !follow_last)
                break;
            *p = '\0';
            char hb[1024], target[1024];
            const char *host = translate_one(cur, hb, sizeof hb);
            ssize_t tl = host ? readlink(host, target, sizeof target - 1) : -1;
            *p = save;
            if (tl > 0) {
                target[tl] = '\0';
                const char *g = NULL;
                char uv[1024];
                if (rl && !strncmp(target, root, rl) && (target[rl] == '/' || !target[rl]))
                    g = target[rl] ? target + rl : "/";
                else if (target[0] == '/') {
                    // symlinkat stores host paths, so a link made in an outer
                    // view (steam.sh's ~/.steam/sdk64 -> the Steam root) names
                    // a host path a sandbox view cannot see as such: map it
                    // back through the sandbox's binds (/tmp is /private/tmp
                    // on the host, and binds may carry either spelling).
                    const char *u = lxrt_mounts_active() ? lxrt_mounts_untranslate(target, uv, sizeof uv) : NULL;
                    if (!u && lxrt_mounts_active() && !strncmp(target, "/tmp/", 5)) {
                        char pv[1100];
                        snprintf(pv, sizeof pv, "/private%s", target);
                        u = lxrt_mounts_untranslate(pv, uv, sizeof uv);
                    }
                    // The same root under another spelling (the link was made
                    // by a process whose LXRT_ROOT was /tmp/lxrt-steamroot,
                    // this one has its realpath): strip it too.
                    const char *croot = u ? NULL : guest_root_canon();
                    char ct[PATH_MAX];
                    size_t cl = croot ? strlen(croot) : 0;
                    if (croot && host_canon(target, ct, sizeof ct) && !strncmp(ct, croot, cl) &&
                        (ct[cl] == '/' || !ct[cl])) {
                        snprintf(uv, sizeof uv, "%s", ct[cl] ? ct + cl : "/");
                        u = uv;
                    }
                    g = u ? u : target;
                } else if (lxrt_mounts_active()) {
                    // A relative link in a sandbox: bwrap's --symlink makes
                    // /etc/os-release -> ../usr/lib/os-release in the
                    // sandbox's own root, and /usr is a bind. The host would
                    // resolve it against the empty directory standing in for
                    // the bind ("cat /etc/os-release: No such file" in the
                    // sniper container; CS2's cs2.sh reads it and refused to
                    // start, MEASURED, benchmarks/stage51). In guest terms:
                    // the link's directory, then the target, "." and ".."
                    // folded -- every component before the link has already
                    // been found not to be a link.
                    char joined[1024];
                    char *dirend = p - 1;
                    while (dirend > cur && *dirend != '/') dirend--;
                    if (snprintf(joined, sizeof joined, "%.*s/%s", (int)(dirend - cur), cur, target) < (int)sizeof joined) {
                        size_t o = 0;
                        uv[0] = '\0';
                        for (char *c = joined; *c;) {
                            while (*c == '/') c++;
                            char *e = c;
                            while (*e && *e != '/') e++;
                            size_t l = (size_t)(e - c);
                            if (l == 2 && c[0] == '.' && c[1] == '.') {
                                while (o > 0 && uv[--o] != '/')
                                    ;
                            } else if (l && !(l == 1 && c[0] == '.') && o + l + 2 < sizeof uv) {
                                uv[o++] = '/';
                                memcpy(uv + o, c, l);
                                o += l;
                            }
                            uv[o] = '\0';
                            c = e;
                        }
                        if (!o) { uv[0] = '/'; uv[1] = '\0'; }
                        // Only where a bind decides the answer. A link of the
                        // host's own that no bind covers is the host's to
                        // follow: /tmp -> private/tmp became /private/tmp
                        // here, and a mount made at /tmp/... was no longer
                        // found (tests/elf android_ids: the tmpfs and bind
                        // checks).
                        char whole[1100], hb[PATH_MAX];
                        snprintf(whole, sizeof whole, "%s%s", uv, save ? p : "");
                        if (lxrt_mounts_translate(whole, hb, sizeof hb))
                            g = uv;
                    }
                }
                if (g) {
                    char next[1024];
                    // A target ending in '/' (Wine's dosdevices/z: -> "/")
                    // joined with the rest would give "//tmp/...", which no
                    // bind matches: the lookup fell into the sandbox's
                    // scratch root (MEASURED: steam.exe "Failed to create
                    // process" for a game under Z:\tmp\fexhome\...).
                    const char *rest = save ? p : "";
                    size_t gl = strlen(g);
                    if (gl && g[gl - 1] == '/' && rest[0] == '/')
                        rest++;
                    if (snprintf(next, sizeof next, "%s%s", g, rest) >= (int)sizeof next)
                        return path;
                    snprintf(cur, sizeof cur, "%s", next);
                    changed = changed_any = true;
                    break;
                }
            }
            if (!save)
                break;
        }
        if (!changed)
            break;
    }
    if (!changed_any)
        return path;
    snprintf(out, outn, "%s", cur);
    return out;
}

// Returns a pointer valid until the next call on this thread. Only absolute
// paths are redirected: a relative path is already relative to a cwd the guest
// chose.
static const char *translate_ex(const char *path, bool follow_last)
{
    static _Thread_local char gbuf[1024], buf[1024];
    path = guest_resolve(path, follow_last, gbuf, sizeof gbuf);
    return translate_one(path, buf, sizeof buf);
}
static const char *translate(const char *path) { return translate_ex(path, false); }

// A lookup relative to a directory descriptor has to see the fake bwrap's
// binds too. Resolved on the host it reads what the bind covers instead:
// FEX makes every guest lookup relative to its rootfs descriptor (openat2
// RESOLVE_IN_ROOT, fstatat), and pressure-vessel binds the real resolv.conf
// over the rootfs image's EMPTY one at /run/pressure-vessel/interpreter-root/
// etc/resolv.conf -- Chromium's DNS client read the empty file, fell back to
// 127.0.0.1 and every request failed with ERR_NAME_NOT_RESOLVED (MEASURED).
// Returns the host path the bind table gives <dirfd's guest path>/<path>, to
// use with AT_FDCWD, or NULL when no bind deeper than the descriptor's own
// place covers it (then (dirfd, path) is right as it is).
static const char *at_through_mounts(int ldirfd, const char *path, bool follow_last)
{
    if (!lxrt_mounts_active() || !path || !path[0] || path[0] == '/' || ldirfd == -100 || ldirfd < 0)
        return NULL;
    char host[PATH_MAX];
    if (fcntl(ldirfd, F_GETPATH, host) != 0)
        return NULL;
    char gbuf[PATH_MAX];
    const char *g = lxrt_mounts_untranslate(host, gbuf, sizeof gbuf);
    if (!g) {
        const char *croot = guest_root_canon();
        size_t cl = croot ? strlen(croot) : 0;
        char chost[PATH_MAX];
        if (!cl || !host_canon(host, chost, sizeof chost) || strncmp(chost, croot, cl) != 0 ||
            (chost[cl] != '/' && chost[cl] != '\0'))
            return NULL;
        snprintf(gbuf, sizeof gbuf, "%s", chost[cl] ? chost + cl : "/");
        g = gbuf;
    }
    char full[PATH_MAX];
    if (snprintf(full, sizeof full, "%s%s%s", g, g[strlen(g) - 1] == '/' ? "" : "/", path) >= (int)sizeof full)
        return NULL;
    // Links on the way (and the last one, for a caller that follows it) are
    // resolved in guest terms first, as for an absolute path.
    char rb[1024];
    const char *res = guest_resolve(full, follow_last, rb, sizeof rb);
    static _Thread_local char mb[PATH_MAX];
    if (!lxrt_mounts_translate(res, mb, sizeof mb))
        return NULL;
    // The descriptor's own mapping reaching the same place: nothing to change.
    char plain[PATH_MAX];
    snprintf(plain, sizeof plain, "%s/%s", host, path);
    char cp[PATH_MAX], cm[PATH_MAX];
    if (host_canon(plain, cp, sizeof cp) && host_canon(mb, cm, sizeof cm) && strcmp(cp, cm) == 0)
        return NULL;
    return mb;
}
// For other modules (AF_UNIX socket paths): the host path a guest path names,
// through the same binds, root and symlink rules as every file syscall.
const char *lxrt_translate_guest_path(const char *path, char *out, size_t n)
{
    char g[1024];
    path = guest_resolve(path, false, g, sizeof g);
    const char *h = translate_one(path, out, n);
    if (h != out) {
        snprintf(out, n, "%s", h);
        h = out;
    }
    return h;
}

// For the exec path (main.c resolves the program itself): guest path in,
// guest path out, every absolute link followed.
const char *lxrt_guest_resolve_follow(const char *path, char *out, size_t n)
{
    return guest_resolve(path, true, out, n);
}
static const char *translate_follow(const char *path) { return translate_ex(path, true); }

static const char *translate_one(const char *path, char *buf, size_t bufn)
{
    // /proc is synthesised, not taken from LXRT_ROOT: its contents describe
    // this process, and no filesystem image can supply them.
    const char *proc = lxrt_proc_translate(path);
    if (proc)
        return proc;
    // /dev/input: the controllers steamarm-inputd publishes (evdev.c), in
    // every process and every sandbox, whatever its /dev binds say.
    const char *input = lxrt_evdev_translate(path, buf, bufn);
    if (input)
        return input;
    // /dev/__properties__ and /dev/socket/property_service: Android's
    // system properties, served per root by the property service (props.c).
    const char *props = lxrt_props_translate(path, buf, bufn);
    if (props)
        return props;
    // A bind mount from a (fake) bwrap plan shadows the root prefix.
    const char *m = lxrt_mounts_translate(path, buf, bufn);
    if (m)
        return m;
    // /dev/shm is one directory per "kernel", not per root: Chromium's
    // processes share memory through names in it, and inside a bwrap sandbox
    // the root is a scratch directory with no dev/ (steamwebhelper died with
    // "Creating shared memory in /dev/shm/... failed"). One host directory
    // serves every process of this user.
    if (path && !strncmp(path, "/dev/shm", 8) && (path[8] == '\0' || path[8] == '/')) {
        static char shmdir[64];
        if (!shmdir[0]) {
            snprintf(shmdir, sizeof shmdir, "/tmp/lxrt-shm-%u", (unsigned)getuid());
            mkdir(shmdir, 01777);
        }
        int n = snprintf(buf, bufn, "%s%s", shmdir, path + 8);
        if (n > 0 && (size_t)n < bufn)
            return buf;
    }
    // A pseudo-terminal's slave: Linux's /dev/pts/N is Darwin's
    // /dev/ttysNNN (ioctl_tty.c hands out the numbers).
    if (path && strncmp(path, "/dev/pts/", 9) == 0 && path[9] >= '0' && path[9] <= '9') {
        char *end;
        long n = strtol(path + 9, &end, 10);
        if (*end == '\0' && n >= 0 && n < 1000) {
            int w = snprintf(buf, bufn, "/dev/ttys%03ld", n);
            if (w > 0 && (size_t)w < bufn)
                return buf;
        }
    }
    const char *root = guest_root();
    if (!root || !path || path[0] != '/')
        return path;
    // /dev is the host's: null, zero, urandom, tty, stdin... exist there and
    // nowhere under a Linux tree copied into LXRT_ROOT. CEF's subprocesses
    // died with "Failed to open /dev/null" until this. The one Linux-only
    // entry programs create files in, /dev/shm (Chromium's shared memory),
    // is a directory inside the root instead.
    if (strncmp(path, "/dev", 4) == 0 && (path[4] == '\0' || path[4] == '/')) {
        // Only the device files whose semantics are identical pass through.
        // Passing ALL of /dev through was measured to kill bash at start-up
        // (its /dev/tty open went from ENOENT to ENXIO and something down
        // that path jumped to a garbage RIP); the rest stays under the root,
        // where it does not exist, as before.
        static const char *const pass[] = { "/dev/null", "/dev/zero", "/dev/full",
                                            "/dev/random", "/dev/urandom", "/dev/ptmx" };
        for (size_t i = 0; i < sizeof pass / sizeof pass[0]; i++)
            if (strcmp(path, pass[i]) == 0)
                return path;
        // Guest fds are host fds, so the host's /dev/fd/N (and the /dev/std*
        // links onto it) mean the same thing. bash process substitution
        // (<(...), >(...)) hands out /dev/fd/63 paths.
        if (strncmp(path, "/dev/fd/", 8) == 0 || strcmp(path, "/dev/stdin") == 0 ||
            strcmp(path, "/dev/stdout") == 0 || strcmp(path, "/dev/stderr") == 0)
            return path;
    }
    int n = snprintf(buf, bufn, "%s%s", root, path);
    if (n <= 0 || (size_t)n >= bufn)
        return path;
    return buf;
}
uint64_t lxrt_last_guest_lr(void);
static atomic_ullong g_unimplemented;
static _Atomic long g_last_unimplemented = -1;

// The trace goes to a private copy of stderr, well above any descriptor the
// guest will use: a guest that closes or redirects fd 2 (FEX does both at
// exit) otherwise silences the runtime at exactly the moment of interest.
// The number is carried to the processes lxrt_execve starts in LXRT_TRACE_FD.
static FILE *g_trace_fp;
static int g_trace_raw_fd = -1;
FILE *lxrt_trace_stream(void) { return g_trace_fp ? g_trace_fp : stderr; }
// Where the runtime's informational lines go (what it loaded, what it
// rewrote, signals it cannot carry): the trace stream, or nowhere with
// LXRT_QUIET=1 and no trace. Linux prints nothing when a program starts,
// and a guest that captures a child's stderr -- Termux's `$(id -u 2>&1)` --
// got these lines as the answer (MEASURED). Android sessions run quiet.
static int discard_write(void *cookie, const char *buf, int n)
{
    (void)cookie; (void)buf;
    return n;
}

FILE *lxrt_info_stream(void)
{
    static FILE *null_fp;
    static int quiet = -1;
    if (quiet < 0)
        quiet = getenv("LXRT_QUIET") && strcmp(getenv("LXRT_QUIET"), "0") ? 1 : 0;
    if (!quiet || g_trace)
        return lxrt_trace_stream();
    // A stream with no descriptor behind it: an fd opened here would be one
    // more of the runtime's in a guest's fd table, and Android's zygote
    // aborts on one it cannot account for ("Unable to stat 40", MEASURED).
    if (!null_fp)
        null_fp = funopen(NULL, NULL, discard_write, NULL, NULL);
    return null_fp ? null_fp : lxrt_trace_stream();
}
int lxrt_trace_fd(void) { return g_trace_fp ? fileno(g_trace_fp) : -1; }
void lxrt_dispatch_set_trace(bool on)
{
    g_trace = on;
    if (!on || g_trace_fp)
        return;
    const char *e = getenv("LXRT_TRACE_FD");
    const char *file = getenv("LXRT_TRACE_FILE");   // survives fd-closing parents
    int fd = -1;
    if (file && *file) {
        int f = open(file, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        fd = f >= 0 ? fcntl(f, F_DUPFD_CLOEXEC, 900) : -1;
        if (f >= 0) close(f);
    } else if (e && *e && fcntl(atoi(e), F_GETFD) != -1)
        fd = atoi(e);
    else
        fd = fcntl(2, F_DUPFD, 900);   // inherited across exec on purpose
    if (fd < 0)
        return;
    g_trace_fp = fdopen(fd, "w");
    if (g_trace_fp)
        setvbuf(g_trace_fp, NULL, _IOLBF, 0);
    g_trace_raw_fd = fd;
}

// A fork child inherits the trace FILE's lock in whatever state another
// thread left it; Darwin resets that only for the standard streams (measured:
// a traced pressure-vessel child sat in flockfile forever). The child gets a
// fresh FILE on the same descriptor; the old one is abandoned, not closed.
void lxrt_trace_after_fork(void)
{
    if (!g_trace_fp || g_trace_raw_fd < 0)
        return;
    int fd = dup(g_trace_raw_fd);
    FILE *fp = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!fp)
        return;
    setvbuf(fp, NULL, _IOLBF, 0);
    g_trace_fp = fp;
    g_trace_raw_fd = fd;
}
bool lxrt_trace_on(void) { return g_trace; }
// The trace stream's FILE lock is NOT registered for fork: Darwin's libc
// resets stdio locks in the child itself, and an extra funlockfile there
// broke the recursive count -- the next fork in that child blocked forever
// in flockfile (measured: steam.sh's bash, a grandchild fork).
uint64_t lxrt_dispatch_unimplemented_count(void)
{
    return atomic_load(&g_unimplemented);
}
long lxrt_dispatch_last_unimplemented(void)
{
    return atomic_load(&g_last_unimplemented);
}

// Darwin reports failure as -1 plus errno; Linux returns -errno directly -- and
// the numbers themselves differ (see errno_map.c), so the value is translated,
// never passed through.
static long ret_of(long r) { return r < 0 ? -lxrt_errno_to_linux(errno) : r; }
// For paths that already hold an errno value rather than a -1 return.
#define LERR(e) (-lxrt_errno_to_linux(e))

// ---------------------------------------------------------------- clock

static double g_ns_per_tick;

// The guest's clocks, in one place. Every module that compares a guest-supplied
// absolute time against "now" (timerfd's TFD_TIMER_ABSTIME, futex timeouts)
// has to read the SAME counter the guest read, or the comparison is off by
// however much the two bases differ. Darwin's clock_gettime(CLOCK_MONOTONIC)
// and mach_absolute_time() are not the same base (measured 3.8 s apart on
// this machine, hours on a laptop that has slept); timerfd's ABSTIME arming
// never fired because of it.
uint64_t lxrt_guest_clock_ns(long clk)
{
    // Wall-clock ids: REALTIME(0), REALTIME_COARSE(5) -- which is what glibc's
    // time() reads, measured returning uptime before this --, REALTIME_ALARM(8)
    // and TAI(11, approximated by UTC).
    if (clk == 0 || clk == 5 || clk == 8 || clk == 11) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    // CPU-time ids: PROCESS_CPUTIME_ID(2), THREAD_CPUTIME_ID(3) and the
    // negative per-pid/per-tid encodings (clock_getcpuclockid); bit 2 of a
    // negative id set means a thread clock.
    if (clk == 2 || clk == 3 || clk < 0) {
        bool thread = clk == 3 || (clk < 0 && (clk & 4));
        struct timespec ts;
        clock_gettime(thread ? CLOCK_THREAD_CPUTIME_ID : CLOCK_PROCESS_CPUTIME_ID, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    // BOOTTIME(7)/BOOTTIME_ALARM(9) count suspended time too.
    if (clk == 7 || clk == 9) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
    }
    // The vDSO computes exactly this (vdso_map.c), so the two never disagree.
    (void)g_ns_per_tick;
    return lxrt_mono_ns();
}

// close(2) as the guest sees it.
static long guest_close(int fd)
{
    if (lxrt_binder_owns(fd))
        return LERR(EBADF);

    // Every module that keeps side state keyed by descriptor gets a chance
    // to drop it. Each is a cheap no-op for a descriptor it does not own.
    // On Linux, close() removes the descriptor from every epoll set it is
    // in. That is what makes fd-number recycling safe, and Steam recycles
    // descriptors constantly: without this, a reused number inherits a dead
    // registration and the guest's event loop goes silently deaf on it.
    // This runs BEFORE the eventfd release: once the last alias is gone
    // the private write end closes too, and the EV_DELETE must name it
    // while it still exists (epoll_eventfd.h contract).
    lxrt_epoll_fd_closed(fd);
    lxrt_dirents_close(fd);
    lxrt_epoll_close(fd);
    lxrt_eventfd_close(fd);
    lxrt_timerfd_close(fd);
    lxrt_signalfd_close(fd);
    lxrt_inotify_close(fd);
    lxrt_socket_close(fd);
    lxrt_pty_close(fd);
    lxrt_memfd_close(fd);
    lxrt_pathfd_close(fd);
    lxrt_evdev_close(fd);
    lxrt_binder_close(fd);
    lxrt_fd_hide(fd, false);
    return ret_of(close(fd));
}
// For modules that close a descriptor on the guest's behalf (binder.c: the
// FDA descriptors Linux closes when a buffer is freed).
long lxrt_guest_close_fd(int fd) { return guest_close(fd); }

static long do_clock_gettime(long clk, uint64_t user_ts)
{
    struct linux_timespec { int64_t tv_sec; int64_t tv_nsec; } *ts =
        (struct linux_timespec *)user_ts;
    if (!ts)
        return LERR(EFAULT);
    // CLOCK_REALTIME(0) needs wall time; everything else is satisfied by the
    // monotonic counter, which is a plain register read.
    uint64_t ns = lxrt_guest_clock_ns(clk);
    ts->tv_sec = (int64_t)(ns / 1000000000ull);
    ts->tv_nsec = (int64_t)(ns % 1000000000ull);
    return 0;
}

// ---------------------------------------------------------------- memory

// Linux and Darwin agree on PROT_* and on MAP_PRIVATE/MAP_SHARED/MAP_FIXED,
// but not on MAP_ANONYMOUS: 0x20 on Linux, 0x1000 on Darwin.
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FIXED     0x10
#define LINUX_MAP_PRIVATE   0x02
#define LINUX_MAP_SHARED    0x01
#define LINUX_MAP_NORESERVE 0x4000
#define LINUX_MAP_DENYWRITE 0x800
#define LINUX_MAP_STACK     0x20000
#define LINUX_MAP_GROWSDOWN 0x0100
// Reserve at exactly this address, and FAIL rather than replace what is there.
// Darwin's mmap has no equivalent; mach_vm_allocate(VM_FLAGS_FIXED) does.
#define LINUX_MAP_FIXED_NOREPLACE 0x100000

// Executable code that the guest's own dynamic loader maps never passes
// through runtime/elf.c, so it still contains raw `svc`. Map it writable,
// rewrite it, then seal it to what the guest asked for. Getting this wrong is
// silent (Stage 1 finding 1), so the sequence is deliberate rather than
// opportunistic.
static long rewrite_and_seal(void *p, size_t len, int want_prot, size_t scan_len,
                             int fd, uint64_t off)
{
    uint64_t start = LXRT_ALIGN_DOWN((uint64_t)p, LXRT_HOST_PAGE);
    uint64_t end = LXRT_ALIGN_UP((uint64_t)p + len, LXRT_HOST_PAGE);
    // A file mapping can be longer than the file: a loader rounds a segment up
    // to a page, and the tail past EOF is mapped but not backed. Reading it
    // raises SIGBUS, so the scan stops at the last byte the file actually
    // provides. Found by vulkaninfo dying in ld.so before its first syscall.
    uint64_t scan_end = LXRT_ALIGN_UP((uint64_t)p + scan_len, LXRT_HOST_PAGE);
    if (scan_end > end)
        scan_end = end;

    struct lxrt_rewrite_report rep;
    char *err = NULL;
    // The x18 pass needs to know which bytes are instructions: the file's
    // executable sections, translated to where this mapping put them.
    struct lxrt_range code[16];
    int ncode = fd >= 0 ? lxrt_elf_exec_sections(fd, off, len, (uint64_t)p, code, 16) : 0;
    if (lxrt_rewrite_range_code(start, scan_end, code, ncode, &rep, &err) != 0) {
        if (g_trace)
            fprintf(lxrt_trace_stream(), "[lxrt] rewrite of mapped code failed: %s\n",
                    err ? err : "?");
    } else if (g_trace && (rep.sites_found || rep.x18_found ||
                           rep.tls_read_found || rep.tls_write_found)) {
        fprintf(lxrt_trace_stream(), "[lxrt] mapped code at 0x%llx: %zu svc sites, "
                        "%zu rewritten, %zu poisoned | x18 %zu found, %zu rewritten, "
                        "%zu unsupported, %zu unreachable (%d code windows) | "
                        "tls %zu found, %zu rewritten, %zu poisoned, %zu kept\n",
                (unsigned long long)start, rep.sites_found,
                rep.sites_rewritten, rep.sites_unreachable, rep.x18_found,
                rep.x18_rewritten, rep.x18_unsupported, rep.x18_unreachable, ncode,
                rep.tls_read_found + rep.tls_write_found, rep.tls_rewritten,
                rep.tls_unreachable, rep.tls_kept);
    }

    if (mprotect((void *)start, (size_t)(end - start), want_prot) != 0)
        return LERR(errno);
    return 0;
}

// The host process's own heap lives in the same address space as everything a
// guest maps. A fixed mmap, munmap or MADV_DONTNEED that lands on a region
// libmalloc owns silently replaces live runtime/libSystem data (measured:
// objc's class_rw_t for OS_xpc_pipe read back as zeros and every forked child
// crashed in xpc_atfork_child). Nothing Linux-side can own such a range, so
// the request is refused and reported instead of executed.
#include <mach/vm_statistics.h>
static bool region_info_at(uint64_t *addr, uint64_t *size, unsigned *tag)
{
    // Top-level entries only: a nested walk (mach_vm_region_recurse) can hand
    // the same submap entry back forever, and spun a Steam thread at 100 %.
    mach_vm_address_t ra = *addr;
    mach_vm_size_t rs = 0;
    vm_region_extended_info_data_t ri;
    mach_msg_type_number_t rc = VM_REGION_EXTENDED_INFO_COUNT;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_EXTENDED_INFO,
                       (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS || rs == 0)
        return false;
    *addr = ra;
    *size = rs;
    *tag = ri.user_tag;
    return true;
}

static bool host_heap_hit(uint64_t addr, uint64_t len, const char *op)
{
    uint64_t end = addr + len, p = addr;
    while (p < end) {
        uint64_t ra = p, rs;
        unsigned t;
        if (!region_info_at(&ra, &rs, &t) || ra >= end)
            return false;
        if ((t >= VM_MEMORY_MALLOC && t <= VM_MEMORY_MALLOC_LARGE_REUSED &&
             t != VM_MEMORY_SBRK) ||
            t == VM_MEMORY_MALLOC_NANO || t == VM_MEMORY_MALLOC_MEDIUM ||
            t == VM_MEMORY_MALLOC_PROB_GUARD) {
            uint64_t alo, ahi;
            lxrt_arena_bounds(&alo, &ahi);
            fprintf(lxrt_trace_stream(), "[lxrt] REFUSED %s 0x%llx+0x%llx: overlaps host "
                    "malloc region 0x%llx+0x%llx (tag %u), guest lr 0x%llx [pid %d, arena 0x%llx-0x%llx]\n", op,
                    (unsigned long long)addr, (unsigned long long)len,
                    (unsigned long long)ra, (unsigned long long)rs, t,
                    (unsigned long long)lxrt_last_guest_lr(), (int)getpid(),
                    (unsigned long long)alo, (unsigned long long)ahi);
            return true;
        }
        p = ra + rs;
    }
    return false;
}

// Is the region holding addr backed by a file (a vnode pager)? Asked of that
// exact region: proc_regionfilename() answers for the next region that has a
// file when addr's own has none, which made anonymous memory look file-backed
// depending on the layout -- and MADV_DONTNEED then left FEX's lookup caches
// full of pointers into code buffers it had freed (MEASURED).
static bool region_is_file(uint64_t addr)
{
    mach_vm_address_t ra = addr;
    mach_vm_size_t rs = 0;
    vm_region_extended_info_data_t ri;
    mach_msg_type_number_t rc = VM_REGION_EXTENDED_INFO_COUNT;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_EXTENDED_INFO,
                       (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS || ra > addr)
        return false;
    return ri.external_pager != 0;
}

// The Mach user tag of the region holding addr (0 for plain anonymous memory,
// which is everything a Linux mmap creates here; libmalloc, stacks and dyld tag
// theirs). -1 when nothing is mapped there.
static int region_tag(uint64_t addr)
{
    uint64_t ra = addr, rs;
    unsigned t;
    if (!region_info_at(&ra, &rs, &t) || ra > addr)
        return -1;
    return (int)t;
}

// Is the region holding addr shared memory -- MAP_SHARED anonymous memory,
// shm, a MAP_SHARED file, pages shared with a fork child -- whose contents
// Linux keeps across MADV_DONTNEED (it only drops this process's page-table
// entries)? Darwin's mmap gives every MAP_SHARED mapping VM_INHERIT_SHARE;
// the share mode alone says SM_PRIVATE while this process is the object's
// only mapper (MEASURED: a 20 KiB MAP_SHARED|MAP_ANONYMOUS region was taken
// for private memory, replaced by zeros and detached from its object).
static bool region_shared(uint64_t addr)
{
    mach_vm_address_t ra = addr;
    mach_vm_size_t rs = 0;
    vm_region_basic_info_data_64_t ri;
    mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS || ra > addr)
        return false;
    if (ri.inheritance == VM_INHERIT_SHARE)
        return true;
    mach_vm_address_t xa = addr;
    mach_vm_size_t xs = 0;
    vm_region_extended_info_data_t xi;
    mach_msg_type_number_t xc = VM_REGION_EXTENDED_INFO_COUNT;
    if (mach_vm_region(mach_task_self(), &xa, &xs, VM_REGION_EXTENDED_INFO,
                       (vm_region_info_t)&xi, &xc, &obj) != KERN_SUCCESS || xa > addr)
        return false;
    return xi.share_mode == SM_TRUESHARED || xi.share_mode == SM_SHARED_ALIASED ||
           (xi.share_mode == SM_SHARED && ri.shared);
}

// madvise. Most advice is advisory and the constants differ, so unknown
// advice is accepted and ignored. Two are NOT advisory on Linux:
// MADV_DONTNEED (4) and MADV_REMOVE (9) on anonymous private memory make the
// range read back as ZEROS. FEX relies on that -- it clears its call/return
// stack and lookup-cache tables with madvise(MADV_DONTNEED) instead of
// memset -- and while the runtime ignored it, forked children (and, later,
// steamwebhelper) read stale entries back as pointers: SIGSEGV at 0xa9d8,
// 0x18c4, 0x100000021 (benchmarks/stage5-fex.txt). Darwin's MADV_FREE does
// not promise zeros on the next read, so the pages are replaced outright.
static long do_madvise(uint64_t addr, uint64_t len, int ladvice)
{
    static int knob = -1;                       // LXRT_NO_MADVISE=1: bisecting aid
    if (knob < 0) knob = getenv("LXRT_NO_MADVISE") ? 1 : 0;
    if (knob || (ladvice != 4 && ladvice != 9))
        return 0;
    if (addr % 4096 || !len)
        return LERR(EINVAL);
    uint64_t end = addr + len;
    // Partial host pages at either end: zero what is writable, leave the rest.
    uint64_t hstart = LXRT_ALIGN_UP(addr, LXRT_HOST_PAGE);
    uint64_t hend = LXRT_ALIGN_DOWN(end, LXRT_HOST_PAGE);
    if (hstart > hend) hstart = hend = addr;   // inside one host page
    uint64_t p = hstart;
    while (p < hend) {
        mach_vm_address_t ra = p;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t ri;
        mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS || ra >= hend)
            break;
        if (ra > p) { p = ra; continue; }            // a hole: nothing to clear
        uint64_t seg_end = ra + rs < hend ? ra + rs : hend;
        bool file = region_is_file(p);
        // Shared memory keeps its contents on Linux (region_shared).
        // Replacing it with fresh private memory would detach it from the
        // other processes and lose data. (MADV_REMOVE would punch a hole in
        // shmem, which reads back zeros; kept here too -- zeroing it would
        // commit every page of a range the guest asked to release.)
        bool shared = region_shared(p);
        if (shared) {
            static _Atomic int said;
            if (atomic_fetch_add(&said, 1) < 4)
                fprintf(lxrt_trace_stream(), "[lxrt] madvise(DONTNEED) on shared memory 0x%llx+0x%llx: "
                        "contents kept, as on Linux\n", (unsigned long long)p,
                        (unsigned long long)(seg_end - p));
            file = true;   // leave it alone
        }
        // Host-owned memory (malloc zones, thread stacks) is never the
        // guest's to zero, whatever range it passed: FEX's 32-bit allocator
        // hooks once madvised a 6 GiB span that libmalloc had since grown
        // into, and objc's class tables read back as zeros.
        bool foreign = region_tag(p) > 0;
        if (foreign && g_trace)
            fprintf(lxrt_trace_stream(), "[lxrt] madvise: skipping host region 0x%llx+0x%llx (tag %d)\n",
                    (unsigned long long)p, (unsigned long long)(seg_end - p), region_tag(p));
        if (!file && !foreign) {
            int prot = (ri.protection & VM_PROT_READ ? PROT_READ : 0)
                     | (ri.protection & VM_PROT_WRITE ? PROT_WRITE : 0)
                     | (ri.protection & VM_PROT_EXECUTE ? PROT_EXEC : 0);
            int flags = MAP_PRIVATE | MAP_ANON | MAP_FIXED;
            if (lxrt_jit_contains(p)) flags |= MAP_JIT;
            if (mmap((void *)p, (size_t)(seg_end - p), prot, flags, -1, 0) == MAP_FAILED)
                return LERR(errno);
        }
        // A file mapping reverts to file content on Linux; here it keeps what
        // it has. Accepted silently: nothing measured depends on it yet.
        p = seg_end;
    }
    // The unaligned ends, part of a host page the range shares with memory
    // outside it: zeroed in place when writable right now, and only where
    // Linux gives zeros. Not a file mapping: a private page reads back the
    // file on Linux (kept as it is, like the aligned part above; unwritten,
    // that IS the file) and a memset through a shared one would write zeros
    // into the file. Not shared memory under MADV_DONTNEED, which keeps its
    // contents. A 4 KiB-offset file mapping (offmap.c) always starts partway
    // into a host page, so both reach here with every such mapping.
    for (int side = 0; side < 2; side++) {
        uint64_t lo = side == 0 ? addr : hend, hi = side == 0 ? hstart : end;
        if (lo >= hi) continue;
        mach_vm_address_t ra = lo; mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t ri; mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&ri, &rc, &obj) == KERN_SUCCESS &&
            ra <= lo && (ri.protection & VM_PROT_WRITE) && !lxrt_jit_contains(lo) &&
            region_tag(lo) == 0 && !region_is_file(lo) &&
            !(ladvice == 4 && region_shared(lo)))
            memset((void *)lo, 0, (size_t)(hi - lo));
    }
    return 0;
}

// munmap at the guest's 4 KiB granularity on 16 KiB host pages. Darwin's
// munmap rounds a 4 KiB request up to the whole host page (measured:
// munmap(page+0x4000, 0x1000) left page+0x5000 UNMAPPED), which took live
// neighbours down with it -- FEX's allocator frees 4 KiB at a time and glibc
// frees mmap'd chunks at byte lengths; bash under FEX then dereferenced
// garbage (0x85c, 0x100000021) inside FEX's IR compiler. Only host pages
// wholly inside the range are unmapped; a partial page stays mapped, and its
// tracked guest ranges are forgotten so a later mmap can reuse them. The
// divergence: reading a freed 4 KiB slot that shares a host page with live
// memory does not fault here. Linux would SIGSEGV; nothing measured minds.
static long do_munmap_inner(uint64_t addr, uint64_t len);
static long do_munmap(uint64_t addr, uint64_t len)
{
    {
        long lr;
        if (lxrt_lowpage_munmap(addr, len, &lr))
            return lr;
    }
    lxrt_pe_forget(addr, len);      // code windows of a PE image unmapped here
    long r = do_munmap_inner(addr, len);
    if (r == 0)
        lxrt_arena_unmapped(addr, len);    // holes in the guest's arena get its reservation back
    return r;
}

static long do_munmap_inner(uint64_t addr, uint64_t len)
{
    static int knob = -1;                       // LXRT_PLAIN_MUNMAP=1: bisecting aid
    if (knob < 0) knob = getenv("LXRT_PLAIN_MUNMAP") ? 1 : 0;
    if (knob) {
        long r = ret_of(munmap((void *)addr, (size_t)len));
        if (r == 0)
            lxrt_offmap_disown(addr, len);
        return r;
    }
    if (addr % 4096 || !len)
        return LERR(EINVAL);
    if (host_heap_hit(addr, len, "munmap"))
        return LERR(EINVAL);
    uint64_t end = addr + len;
    uint64_t hstart = LXRT_ALIGN_UP(addr, LXRT_HOST_PAGE);
    uint64_t hend = LXRT_ALIGN_DOWN(end, LXRT_HOST_PAGE);
    lxrt_subpage_forget(addr, len);
    lxrt_jit_forget(addr, len);
    lxrt_wx_forget(addr, len);
    lxrt_privmap_forget(addr, len);
    // What was mapped there: the x18 pass's function tables and the
    // LXRT_GUEST_FAULTS file names, freed when wholly inside. Linux unmaps
    // whole 4 KiB pages, and glibc's dlclose passes an unrounded length
    // (l_map_end - l_map_start, 0x30188 for the libusb the Steam client
    // reloads four times a second) while the RW segment it mapped runs to
    // the next 4 KiB boundary: with the raw length that segment's file name
    // stayed, one per cycle (tests/elfsect_unmap_check.c --raw-len). Only
    // these two take the rounded length; the host-page decisions here keep
    // the guest's.
    uint64_t len4k = LXRT_ALIGN_UP(len, 4096);
    lxrt_elf_forget(addr, len4k);
    lxrt_memlog_file_forget(addr, len4k);
    if (hstart < hend && munmap((void *)hstart, (size_t)(hend - hstart)) != 0)
        return LERR(errno);
    if (hstart < hend)
        lxrt_mremap_forget_shared(hstart, hend - hstart);
    // A partly covered page at either end that belongs to a 4 KiB-offset file
    // mapping (offmap.c) holds nothing else: it goes with its last guest byte.
    uint64_t dead[2];
    int ndead = lxrt_offmap_unmapped(addr, len, dead);
    for (int i = 0; i < ndead; i++)
        lxrt_subpage_forget(dead[i], LXRT_HOST_PAGE);
    return 0;
}

// Every module that keeps state keyed by descriptor number. dup, dup3 and
// fcntl(F_DUPFD*) create aliases; dup3 and close destroy them. Keeping the
// two lists here, and only here, is what stops one path from being forgotten
// -- the F_DUPFD route was, and an eventfd aliased that way handed the guest
// the raw 1-byte pipe token instead of its counter.
static void alias_fd(int oldfd, int newfd)
{
    lxrt_memfd_track_dup(oldfd, newfd);    // seals follow the fd
    lxrt_eventfd_dup(oldfd, newfd);        // and so does the counter
    lxrt_epoll_dup(oldfd, newfd);          // a dup is the same epoll instance
    lxrt_timerfd_dup(oldfd, newfd);
    lxrt_signalfd_dup(oldfd, newfd);
    lxrt_inotify_dup(oldfd, newfd);
    lxrt_socket_dup(oldfd, newfd);
    lxrt_binder_dup(oldfd, newfd);
}

// What an implicit close (dup2 onto a live descriptor) must release. Same
// order as LNR_close: epoll bookkeeping before the eventfd's private end goes.
static void forget_fd(int fd)
{
    lxrt_epoll_fd_closed(fd);
    lxrt_dirents_close(fd);
    lxrt_epoll_close(fd);
    lxrt_eventfd_close(fd);
    lxrt_timerfd_close(fd);
    lxrt_signalfd_close(fd);
    lxrt_inotify_close(fd);
    lxrt_socket_close(fd);
    lxrt_pty_close(fd);
    lxrt_memfd_close(fd);
    lxrt_binder_close(fd);
    lxrt_fd_hide(fd, false);
}

// Linux places mmap(NULL, ...) top-down from just below the stack; Darwin
// fills upward from the bottom of the address space, next to its own images,
// the dyld shared cache and the malloc zones. A 64-bit guest then gets its
// libraries at ~5 GB, and V8, which wants its code range within 2 GB of the
// embedded builtins in libcef.so, found no free 512 MiB there and died
// ("Failed to reserve virtual memory for CodeRange", MEASURED). So a hint-less
// guest mapping is placed top-down under TOPDOWN_BASE, a region Darwin leaves
// empty; a guest hint is still passed through as Linux would honour it.
// LXRT_NO_TOPDOWN=1 restores Darwin's placement.
#define TOPDOWN_BASE 0x7f0000000000ull
static _Atomic uint64_t g_topdown = TOPDOWN_BASE;

static bool range_is_free(uint64_t addr, uint64_t len)
{
    mach_vm_address_t ra = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
    uint64_t end = LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE);
    mach_vm_size_t rs = 0;
    vm_region_basic_info_data_64_t ri;
    mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr = mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                                      (vm_region_info_t)&ri, &rc, &obj);
    return kr != KERN_SUCCESS || ra >= end;
}

static uint64_t topdown_candidate(uint64_t len)
{
    static int off = -1;
    if (off < 0) off = getenv("LXRT_NO_TOPDOWN") ? 1 : 0;
    if (off || !len)
        return 0;
    len = LXRT_ALIGN_UP(len, LXRT_HOST_PAGE);
    uint64_t top = atomic_load(&g_topdown);
    uint64_t cand = LXRT_ALIGN_DOWN(top - len, LXRT_HOST_PAGE);
    for (int tries = 0; tries < 64 && cand > (1ull << 40); tries++) {
        mach_vm_address_t ra = cand;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t ri;
        mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&ri, &rc, &obj);
        if (kr != KERN_SUCCESS || ra >= cand + len) {
            // Free: nothing mapped in [cand, cand+len). Move the cursor down
            // (racing threads just take slightly different gaps).
            uint64_t cur = top;
            while (cand < cur && !atomic_compare_exchange_weak(&g_topdown, &cur, cand))
                ;
            return cand;
        }
        cand = LXRT_ALIGN_DOWN(ra - len, LXRT_HOST_PAGE);   // below what is in the way
    }
    return 0;
}

// MAP_GROWSDOWN (Darwin has no such flag; the mapping is an ordinary one).
// The only user seen is FEX, for the x86 program's main-thread stack
// (ELFCodeLoader.h: a PROT_NONE reservation, then the 8 MiB read-write top
// of it). That stack, not the one the runtime built for FEX, is what the x86
// program means by "my stack": x86-64 bionic's pthread_getattr_np() of the
// main thread looks up /proc/self/stat's startstack in /proc/self/maps, and
// with 0 there ART aborted ("Stack not found in /proc/self/maps", MEASURED,
// benchmarks/stage25-art-x86-fex.txt). The first writable one wins; under a
// guest base (a 32-bit guest, or a 64-bit low window) the guest's address.
void lxrt_note_growsdown(uint64_t addr, uint64_t len, long prot)
{
    static _Atomic int noted;
    if (!(prot & PROT_WRITE) || !len || atomic_exchange(&noted, 1))
        return;
    uint64_t top = addr + len - 16;
    uint64_t gb = lxrt_gbase();
    if (gb && top >= gb && top < gb + (1ull << 32))
        top -= gb;
    lxrt_start_stack = top;
    if (g_trace)
        fprintf(lxrt_trace_stream(), "[lxrt] MAP_GROWSDOWN stack 0x%llx+0x%llx: startstack 0x%llx\n",
                (unsigned long long)addr, (unsigned long long)len, (unsigned long long)top);
}

static long do_mmap_inner(uint64_t addr, uint64_t len, long prot, long lflags,
                          long fd, long off);
static long do_mmap(uint64_t addr, uint64_t len, long prot, long lflags,
                    long fd, long off)
{
    // In the guest's arena (arena.c) a range no guest mapping touches holds
    // only the runtime's reservation: an address the guest asks for there --
    // MAP_FIXED_NOREPLACE or a hint -- is free as Linux would see it, and the
    // mapping replaces the reservation.
    if (addr && !(lflags & LINUX_MAP_FIXED) && lxrt_arena_free(addr, len))
        lflags = (lflags & ~(long)LINUX_MAP_FIXED_NOREPLACE) | LINUX_MAP_FIXED;
    long r = do_mmap_inner(addr, len, prot, lflags, fd, off);
    if (r >= 0)
        lxrt_arena_mapped((uint64_t)r, len);
    else if (addr >= 0x140000000ull && addr < 0x160000000ull && getenv("LXRT_ARENA_DEBUG"))
        fprintf(lxrt_trace_stream(), "[lxrt] arena: mmap 0x%llx+0x%llx prot %ld flags 0x%lx fd %ld -> %ld (arena %s)\n",
                (unsigned long long)addr, (unsigned long long)len, prot, lflags, fd, r,
                lxrt_arena_on() ? "on" : "OFF");
    return r;
}

static long do_mmap_inner(uint64_t addr, uint64_t len, long prot, long lflags,
                          long fd, long off)
{
    // A native guest's fixed request below 4 GiB: KUSER_SHARED_DATA as a
    // virtual page, anything else refused (lowpage.c, LXRT_LOWPAGES=1).
    {
        long lr;
        if (lxrt_lowpage_mmap(addr, len, prot, lflags, fd, off, &lr))
            return lr;
    }
    int flags = 0;
    if (lflags & LINUX_MAP_SHARED)  flags |= MAP_SHARED;
    if (lflags & LINUX_MAP_PRIVATE) flags |= MAP_PRIVATE;
    if (lflags & LINUX_MAP_FIXED)   flags |= MAP_FIXED;
    if (lflags & LINUX_MAP_ANONYMOUS) { flags |= MAP_ANON; fd = -1; }
    if ((lflags & LINUX_MAP_FIXED) && host_heap_hit(addr, len, "mmap MAP_FIXED"))
        return LERR(ENOMEM);
    if (fd >= 0) {
        // F_SEAL_WRITE must stop a writable shared mapping of a memfd, not
        // only write(): the seal is on the inode, and mmap is a write path.
        long seal = lxrt_memfd_check_mmap((int)fd, (int)prot, (int)lflags);
        if (seal < 0) return seal;
    }
    // MAP_NORESERVE, MAP_DENYWRITE and MAP_STACK have no Darwin equivalent and
    // no effect on correctness here; dropping them is deliberate.
    long unknown = lflags & ~(LINUX_MAP_SHARED | LINUX_MAP_PRIVATE |
                              LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
                              LINUX_MAP_NORESERVE | LINUX_MAP_DENYWRITE |
                              LINUX_MAP_STACK | LINUX_MAP_FIXED_NOREPLACE |
                              LINUX_MAP_GROWSDOWN);
    if (unknown && g_trace)
        fprintf(lxrt_trace_stream(), "[lxrt] mmap: dropping unhandled Linux flags 0x%lx\n", unknown);

    // Whatever lands on a range ends its copy-on-write bookkeeping (privmap.c).
    if (flags & MAP_FIXED) {
        lxrt_privmap_forget(addr, len);
        lxrt_shmirror_forget(addr, len);
        lxrt_wx_forget(addr, len);     // a new mapping is not the old RWX range
        lxrt_mremap_forget_shared(addr, len);
        // Nor the PE image that was there: Wine releases a view by mapping
        // over it (PROT_NONE, MAP_FIXED), and a JIT buffer placed there later
        // must not be taken for that image's non-code (rewrite.c pe_foreign).
        lxrt_pe_forget(addr, len);
    }
    // Something placed into the host pages of a 4 KiB-offset file mapping
    // makes them no longer that mapping's alone (offmap.c).
    if ((flags & MAP_FIXED) || (lflags & LINUX_MAP_FIXED_NOREPLACE))
        lxrt_offmap_disown(addr, len);
    // A private mapping of a shared-memory file keeps seeing the file's
    // updates until the guest writes, as on Linux (privmap.c: wine's session
    // objects). Aligned ranges only; the whole host pages are replaced, so the
    // sub-page records for them go too.
    if (fd >= 0 && (flags & MAP_PRIVATE) &&
        lxrt_privmap_eligible((int)fd, addr, len, (uint64_t)off, (int)prot)) {
        if (flags & MAP_FIXED)
            lxrt_subpage_forget(addr, len);
        return lxrt_privmap_mmap(addr, len, (int)prot, flags, (int)fd, (uint64_t)off);
    }

    // A request for write AND execute is a JIT asking for a code buffer. Darwin
    // refuses that outright; MAP_JIT is the only thing that comes close, and
    // jit.c drives the write/execute switch from the faults.
    if ((prot & PROT_EXEC) && (prot & PROT_WRITE) &&
        (lflags & LINUX_MAP_ANONYMOUS)) {
        // MAP_JIT cannot be combined with MAP_FIXED (measured: EINVAL). A
        // fixed RWX request is a JIT committing code into a range it already
        // reserved (FEX does this for a 32-bit guest's code/VDSO pages). Drop
        // the reservation at that spot and place a fresh MAP_JIT there without
        // MAP_FIXED: the kernel honours a free hint (measured 8/8), and we
        // accept it only if it landed exactly where asked.
        void *jp;
        if ((flags & MAP_FIXED) && addr) {
            munmap((void *)addr, (size_t)len);
            jp = mmap((void *)addr, (size_t)len, prot,
                      (flags & ~MAP_FIXED) | MAP_JIT, -1, 0);
            if (jp != MAP_FAILED && jp != (void *)(uintptr_t)addr) {
                if (g_trace)
                    fprintf(lxrt_trace_stream(), "[lxrt] MAP_JIT fixed 0x%llx+0x%llx: kernel placed it "
                                    "at %p instead\n", (unsigned long long)addr,
                            (unsigned long long)len, jp);
                munmap(jp, (size_t)len);
                jp = MAP_FAILED;
            }
        } else {
            jp = mmap((void *)addr, (size_t)len, prot, flags | MAP_JIT, -1, 0);
        }
        if (jp == MAP_FAILED) {
            if (g_trace)
                fprintf(lxrt_trace_stream(), "[lxrt] MAP_JIT 0x%llx+0x%llx flags=0x%lx: %s\n",
                        (unsigned long long)addr, (unsigned long long)len, lflags, strerror(errno));
            return LERR(errno);
        }
        if (!lxrt_jit_register((uint64_t)jp, (uint64_t)len))
            fprintf(lxrt_trace_stream(), "[lxrt] too many JIT regions; this one will fault\n");
        return (long)(uintptr_t)jp;
    }

    // An executable mapping has to be brought in writable so its syscall sites
    // can be rewritten. macOS enforces W^X, so it cannot be both at once.
    bool exec_map = g_rewrite_mapped && (prot & PROT_EXEC) != 0;
    int use_prot = (int)prot;
    if (exec_map) {
        use_prot = (int)((prot & ~PROT_EXEC) | PROT_READ | PROT_WRITE);
        // Writing into a MAP_SHARED file mapping would modify the file on
        // disk. Code is always mapped MAP_PRIVATE by a sane loader; force it
        // rather than corrupt a library.
        if (flags & MAP_SHARED) {
            flags = (flags & ~MAP_SHARED) | MAP_PRIVATE;
            if (g_trace)
                fprintf(lxrt_trace_stream(), "[lxrt] mmap: forcing MAP_PRIVATE on an "
                                "executable MAP_SHARED mapping\n");
        }
    }

    // MAP_FIXED_NOREPLACE: place it exactly there, but do not destroy what is
    // already mapped. Darwin's mmap(MAP_FIXED) would overwrite -- the same trap
    // that silently deleted a page of libc from inside this runtime once.
    if ((lflags & LINUX_MAP_FIXED_NOREPLACE) && !(lflags & LINUX_MAP_FIXED)) {
        if (!addr)
            return LERR(EINVAL);
        mach_vm_address_t at = addr;
        kern_return_t kr = mach_vm_allocate(mach_task_self(), &at,
                                            (mach_vm_size_t)len, VM_FLAGS_FIXED);
        if (kr == KERN_NO_SPACE && addr + len > (1ull << 32)) {
            // A host page in the range is mapped. The guest allocates in
            // 4 KiB units and a 16 KiB host page can be only partly in use;
            // if every guest page asked for is free, this is not a conflict.
            // (Below 4 GiB the "mapping" is __PAGEZERO: ENOMEM, below.)
            long r = lxrt_subpage_mmap_noreplace(addr, len, (int)prot,
                                                 (lflags & LINUX_MAP_ANONYMOUS) != 0,
                                                 (int)fd, off);
            if (r >= 0)
                return r;
        }
        if (kr != KERN_SUCCESS) {
            if (g_trace) {
                // Say WHAT is in the way. "Taken" alone sends the reader
                // hunting through the whole address space.
                mach_vm_address_t ra = addr;
                mach_vm_size_t rs = 0;
                vm_region_basic_info_data_64_t ri;
                mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t obj = MACH_PORT_NULL;
                const char *who = "unknown";
                if (mach_vm_region(mach_task_self(), &ra, &rs,
                                   VM_REGION_BASIC_INFO_64,
                                   (vm_region_info_t)&ri, &rc, &obj)
                        == KERN_SUCCESS) {
                    if (lxrt_pool_contains(ra))
                        who = "a trampoline pool";
                    else
                        who = "another mapping";
                }
                uint64_t ta = addr, ts = 0;
                unsigned tag = 0;
                region_info_at(&ta, &ts, &tag);
                fprintf(lxrt_trace_stream(), "[lxrt] MAP_FIXED_NOREPLACE 0x%llx+0x%llx: %s "
                                "(%s at 0x%llx+0x%llx, tag %u) guestLR=0x%llx\n",
                        (unsigned long long)addr, (unsigned long long)len,
                        kr == KERN_INVALID_ADDRESS ? "outside the address space"
                                                   : "range busy",
                        who, (unsigned long long)ra, (unsigned long long)rs, tag,
                        (unsigned long long)lxrt_last_guest_lr());
            }
            // The distinction matters more than it looks. EEXIST means "busy,
            // try elsewhere"; ENOMEM means "that address does not exist here".
            // Returning EEXIST for both made FEX's host-VA-width probe --
            // which accepts EEXIST as proof that a width exists -- conclude
            // this machine had 57-bit addresses, and then try to reserve
            // [2^47, 2^48) to fence off what the guest must not see. Darwin
            // tops out at 2^47, so that reservation could never succeed, and
            // FEX asserted.
            //
            // Not every invalid address is past the top, though. Darwin's
            // page zero slides with the image: the first bytes above 4 GiB,
            // up to the runtime's own image, are below the map's minimum
            // and answer KERN_INVALID_ADDRESS too. Wine looks for room for a
            // 64-bit DLL upward from exactly 4 GiB with these probes, takes
            // anything but EEXIST as the end of the search ("mmap() error
            // Cannot allocate memory, range 0x100000000-0x100100000"), and
            // its services and DLLs failed to load (MEASURED, stage 50). On
            // Linux that address exists; here it is taken, by the host.
            // Only that gap is "taken": below 4 GiB and past the top of the
            // address space stay ENOMEM (FEX's probe again, which a wider
            // EEXIST broke: every exec under it failed with EFAULT).
            extern const struct mach_header_64 __dso_handle;
            bool below_image = addr >= (1ull << 32) && addr < (uint64_t)(uintptr_t)&__dso_handle;
            // The same at the other end: Darwin's address space stops 32 MiB
            // short of 2^47, and an x86-64 Wine allocates top-down from
            // 0x7fffffff0000 with these probes ("mmap() error Cannot
            // allocate memory, range 0x7ffffffd0000-0x7ffffffef000": its
            // services did not start). Taken, so it steps down -- except
            // for FEX's width probe itself, which is one inaccessible page.
            bool top_gap = addr >= MACH_VM_MAX_ADDRESS && addr + len <= (1ull << 47) &&
                           !(prot == PROT_NONE && len <= LXRT_HOST_PAGE);
            // Below 4 GiB is __PAGEZERO: a region that exists (KERN_NO_SPACE,
            // not KERN_INVALID_ADDRESS) and can never be mapped. "Busy"
            // (EEXIST) there sent native Wine stepping through every free
            // range it believes it has below 4 GiB, some 80 probes for every
            // allocation of a game: FINAL FANTASY VII REMAKE made 1.37
            // million failing mmaps a minute while loading (MEASURED,
            // 2026-10-08). ENOMEM ends each such range at its first probe.
            // The virtual pages lowpage.c provides there were handled above.
            if (addr + len <= (1ull << 32))
                return LERR(ENOMEM);
            return LERR(kr == KERN_INVALID_ADDRESS && !below_image && !top_gap ? ENOMEM : EEXIST);
        }
        if (mprotect((void *)at, (size_t)len, (int)prot) != 0) {
            int e = errno;
            mach_vm_deallocate(mach_task_self(), at, (mach_vm_size_t)len);
            return LERR(e);
        }
        if (!(lflags & LINUX_MAP_ANONYMOUS) && fd >= 0) {
            // A file mapping still has to come from the file; the reservation
            // only proved the address was free.
            mach_vm_deallocate(mach_task_self(), at, (mach_vm_size_t)len);
            void *fp = mmap((void *)addr, (size_t)len, (int)prot,
                            flags | MAP_FIXED, (int)fd, (off_t)off);
            if (fp == MAP_FAILED)
                return LERR(errno);
            if ((flags & MAP_SHARED) && !(prot & PROT_EXEC))
                lxrt_mremap_note_shared((uint64_t)(uintptr_t)fp, len, (int)fd, (uint64_t)off, (int)prot);
            return (long)(uintptr_t)fp;
        }
        // A reservation shorter than its host page: the page is the guest's
        // only up to addr+len. Recorded so, with the rest as a placeholder:
        // left untracked and read-write, the rest looked live to the next
        // mapping, and Wine -- which reserves a range this way and then maps
        // a file view over it -- got a private copy of every shared section
        // it mapped (its session shared memory: wineserver's updates never
        // reached the game, MEASURED with FINAL FANTASY VII REMAKE).
        if (len % LXRT_HOST_PAGE)
            lxrt_subpage_note_reservation(addr, len, (int)prot);
        return (long)at;
    }

    // A MAP_FIXED request that is not 16 KiB aligned cannot be given to
    // Darwin's mmap at all. x86 Linux images are linked for 4 KiB pages, so
    // this is the normal case for anything under FEX.
    // A partial LENGTH is routed the same way, but only for data: Darwin rounds
    // the tail up and zero-fills the neighbour (measured), which killed FEX's
    // 4 KiB allocator; an executable file segment mapped by ld.so is aarch64
    // code whose rounded tail is its own file bytes, and it must go through
    // the ordinary path so its syscall sites are rewritten.
    bool partial_data = (len % LXRT_HOST_PAGE) != 0 && !(prot & PROT_EXEC);
    bool shmirror_candidate = false;
    // A MAP_SHARED mapping on a host-page boundary with a partial length can
    // still be a real shared mapping when the rest of its last host page is
    // nobody's: FEX places shared mappings on 16 KiB boundaries and keeps the
    // tail reserved (PROT_NONE). Rounding up there is what Linux does to the
    // tail of a file page, and it keeps the memory shared -- the sub-page path
    // can only make a private copy (the Steam client's IPC objects).
    // The same holds for a private file mapping: a copy costs a read of the
    // whole range and its memory up front (Mesa's 154 MB driver, once per
    // mapping, MEASURED), where the file mapped on the host pages in lazily.
    if (partial_data && fd >= 0 && addr % LXRT_HOST_PAGE == 0 &&
        (uint64_t)off % LXRT_HOST_PAGE == 0) {
        uint64_t tail = addr + len, tend = LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE);
        bool tail_free = lxrt_subpage_only_placeholders(tail, tend - tail);
        // When the sub-page table covers the tail, it is the authority: the
        // host protection of that page is whatever an earlier sub-page
        // placement left there (wine re-mapping KUSER_SHARED_DATA over its
        // own reservation found the page read-only and got a private copy --
        // wineserver's clock updates never reached wineboot, which then spun
        // forever, MEASURED). Only untracked memory is probed on the host.
        bool tracked = lxrt_subpage_tracked(tail, tend - tail);
        for (uint64_t q = tail; tail_free && !tracked && q < tend; ) {
            mach_vm_address_t ra = q; mach_vm_size_t rs = 0;
            vm_region_basic_info_data_64_t ri;
            mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj = MACH_PORT_NULL;
            if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                               (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS || ra >= tend)
                break;                              // unmapped to the end: free
            if (ra > q) { q = ra; continue; }       // a hole: free
            if (ri.protection != VM_PROT_NONE) tail_free = false;   // live memory
            q = ra + rs;
        }
        if (g_trace)
            fprintf(lxrt_trace_stream(), "[lxrt] shared tail 0x%llx+0x%llx: tail 0x%llx-0x%llx free %d tracked %d\n",
                    (unsigned long long)addr, (unsigned long long)len, (unsigned long long)tail,
                    (unsigned long long)tend, (int)tail_free, (int)tracked);
        if (tail_free) {
            partial_data = false;
            // Read-only shared: if the host page is later shared with another
            // guest mapping, shmirror.c keeps the copy current.
            shmirror_candidate = (flags & MAP_SHARED) && !(prot & PROT_WRITE);
            // The whole host pages become this mapping: drop the placeholder
            // records so the sub-page bookkeeping does not route it back.
            lxrt_subpage_forget(addr, tend - addr);
        }
    }
    // A fixed mapping of whole host pages replaces everything in them, as on
    // Linux: whatever the sub-page table recorded there is gone, and the
    // mapping goes the ordinary way (a reservation recorded above must not
    // turn a whole-page shared mapping over it into a private copy).
    if ((lflags & LINUX_MAP_FIXED) && addr && addr % LXRT_HOST_PAGE == 0 &&
        len % LXRT_HOST_PAGE == 0 && !lxrt_subpage_needed(addr, len, (lflags & LINUX_MAP_ANONYMOUS) ? 0 : off) &&
        lxrt_subpage_tracked(addr, len))
        lxrt_subpage_forget(addr, len);
    if ((lflags & LINUX_MAP_FIXED) && addr &&
        (lxrt_subpage_needed(addr, len, (lflags & LINUX_MAP_ANONYMOUS) ? 0 : off) ||
         partial_data || lxrt_subpage_tracked(addr, len))) {
        if (g_trace && (flags & MAP_SHARED))
            fprintf(lxrt_trace_stream(), "[lxrt] shared 0x%llx+0x%llx to sub-page: needed %d partial %d tracked %d fd %ld off 0x%llx\n",
                    (unsigned long long)addr, (unsigned long long)len,
                    (int)lxrt_subpage_needed(addr, len, (lflags & LINUX_MAP_ANONYMOUS) ? 0 : off),
                    (int)partial_data, (int)lxrt_subpage_tracked(addr, len), fd, (unsigned long long)off);
        if (flags & MAP_SHARED) {
            // A sub-page placement is a private copy: writes stay in this
            // process. Say so -- it broke the Steam client's shared IPC object
            // until FEX learned to align shared mappings (see
            // patches/fex-lxrt-guest-base.patch, MemAllocator32Bit::Mmap).
            static _Atomic int said;
            if (atomic_fetch_add(&said, 1) < 8) {
                // Why the direct shared path was refused: what occupies the
                // rest of the last host page.
                uint64_t tail = addr + len, tend = LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE);
                mach_vm_address_t ra = tail; mach_vm_size_t rs = 0;
                vm_region_basic_info_data_64_t ri = {0};
                mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t obj = MACH_PORT_NULL;
                bool have = tail < tend && mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                                                          (vm_region_info_t)&ri, &rc, &obj) == KERN_SUCCESS;
                char path[PATH_MAX] = "?";
                if (fd >= 0) fcntl((int)fd, F_GETPATH, path);
                fprintf(lxrt_trace_stream(), "[lxrt] WARNING: pid %d MAP_SHARED 0x%llx+0x%llx off 0x%llx fd %ld (%s) "
                        "lflags 0x%lx is not 16 KiB aligned: mapped as a PRIVATE copy (not shared); "
                        "tail placeholders-only %d, tail region 0x%llx+0x%llx prot %d\n",
                        (int)getpid(), (unsigned long long)addr, (unsigned long long)len,
                        (unsigned long long)off, fd, path, (long)lflags,
                        tail < tend ? (int)lxrt_subpage_only_placeholders(tail, tend - tail) : -1,
                        have ? (unsigned long long)ra : 0ull, have ? (unsigned long long)rs : 0ull,
                        have ? ri.protection : -1);
            }
        }
        long r = lxrt_subpage_mmap(addr, len, (int)prot,
                                   (lflags & LINUX_MAP_ANONYMOUS) != 0,
                                   (int)fd, off);
        if (r >= 0 && exec_map) {
            if (fd >= 0 && len > (128ull << 20)) {
                struct lxrt_range gap;
                if (lxrt_elf_gap_before_exec((int)fd, off, addr, &gap))
                    lxrt_pool_offer_elf_gap(gap.start, gap.end);
            }
            // Code mapped at a 4 KiB file offset still has to be rewritten:
            // the runtime's own libvulkan.so.1 shim is linked with 4 KiB
            // segments, its text lands here, and its private syscalls ran
            // as live `svc`s -- SIGSYS in vulkaninfo before its first call
            // (tests/elf/run.sh #12-#16). Open the host pages, rewrite the
            // exact guest range, put the union protection back.
            uint64_t hs = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
            uint64_t he = LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE);
            if (mprotect((void *)hs, (size_t)(he - hs), PROT_READ | PROT_WRITE) == 0) {
                struct lxrt_rewrite_report rep;
                char *err = NULL;
                struct lxrt_range code[16];
                int ncode = (lflags & LINUX_MAP_ANONYMOUS) ? 0
                          : lxrt_elf_exec_sections((int)fd, off, len, addr, code, 16);
                if (lxrt_rewrite_range_code(addr, addr + len, code, ncode, &rep, &err) != 0 && g_trace)
                    fprintf(lxrt_trace_stream(), "[lxrt] rewrite of sub-page code failed: %s\n",
                            err ? err : "?");
                else if (g_trace && (rep.sites_found || rep.x18_found ||
                                     rep.tls_read_found || rep.tls_write_found))
                    fprintf(lxrt_trace_stream(), "[lxrt] sub-page code at 0x%llx: %zu svc sites, %zu "
                                    "rewritten, %zu poisoned | x18 %zu found, %zu rewritten, "
                                    "%zu unsupported, %zu unreachable (%d code windows) | "
                                    "tls %zu found, %zu rewritten, %zu poisoned, %zu kept\n",
                            (unsigned long long)addr, rep.sites_found, rep.sites_rewritten,
                            rep.sites_unreachable, rep.x18_found, rep.x18_rewritten,
                            rep.x18_unsupported, rep.x18_unreachable, ncode,
                            rep.tls_read_found + rep.tls_write_found, rep.tls_rewritten,
                            rep.tls_unreachable, rep.tls_kept);
            }
            lxrt_subpage_reapply(addr, len);
        }
        return r;
    }

    if ((flags & MAP_FIXED) && lxrt_pool_overlaps(addr, addr + len))
        fprintf(lxrt_trace_stream(), "[lxrt] WARNING: guest MAP_FIXED 0x%llx+0x%llx lands on a "
                        "trampoline pool; live trampolines are about to be "
                        "destroyed\n", (unsigned long long)addr,
                (unsigned long long)len);

    // A file offset on a 4 KiB guest page (LXRT_GUEST_PAGE=4096) but not on a
    // 16 KiB host page is EINVAL for Darwin's mmap. Without MAP_FIXED any
    // address will do: map from the host page below and return the address
    // of the offset asked for (the native arm64 webhelper's shared-memory
    // pool, grown 64 KiB at a time: its window never drew, stage22). The head
    // of the first host page and the tail of the last are nobody's; offmap.c
    // records them so munmap frees them with the guest's last byte there.
    uint64_t head = (uint64_t)off % LXRT_HOST_PAGE;
    if (head && fd >= 0 && !(flags & MAP_FIXED) && !exec_map) {
        void *hp = mmap(NULL, (size_t)(len + head), use_prot, flags, (int)fd, (off_t)(off - head));
        if (hp == MAP_FAILED)
            return LERR(errno);
        lxrt_offmap_note((uint64_t)(uintptr_t)hp,
                         (uint64_t)(uintptr_t)hp + LXRT_ALIGN_UP(len + head, LXRT_HOST_PAGE),
                         (uint64_t)(uintptr_t)hp + head, len);
        return (long)((uintptr_t)hp + head);
    }

    void *p = MAP_FAILED;
    // A hint Linux cannot honour (something is mapped there) is not searched
    // upward from, as Darwin does, but ignored: the mapping goes top-down like
    // an unhinted one. V8 walks hints just above libcef.so, which on Linux
    // either fit or land it below everything else, still within its 2 GB
    // code-range radius; Darwin's upward search put it 2.25 GB away.
    if (!(flags & MAP_FIXED) && addr != 0 && !range_is_free(addr, len))
        addr = 0;
    // FEX's 4 GiB guest-base window (PROT_NONE, hint-less) keeps Darwin's
    // placement, as it had before top-down placement existed: the window's
    // position is part of every 32-bit guest's address arithmetic.
    bool window_reservation = prot == PROT_NONE && len >= (1ull << 32);
    if (!(flags & MAP_FIXED) && addr == 0 && !window_reservation) {
        uint64_t cand = topdown_candidate(len);
        if (cand) {
            p = mmap((void *)(uintptr_t)cand, (size_t)len, use_prot, flags, (int)fd, (off_t)off);
            if (p != MAP_FAILED && p != (void *)(uintptr_t)cand) {
                munmap(p, (size_t)len);          // raced for the gap: let Darwin choose
                p = MAP_FAILED;
            }
        }
    }
    if (p == MAP_FAILED)
        p = mmap((void *)addr, (size_t)len, use_prot, flags, (int)fd, (off_t)off);
    if (p == MAP_FAILED)
        return LERR(errno);
    if (shmirror_candidate)
        lxrt_shmirror_note((uint64_t)(uintptr_t)p, len);
    // A shared file mapping can be grown later (mremap.c): remember the file.
    if ((flags & MAP_SHARED) && fd >= 0 && !exec_map)
        lxrt_mremap_note_shared((uint64_t)(uintptr_t)p, len, (int)fd, (uint64_t)off, use_prot);

    if (exec_map) {
        // Clamp the scan to what the file can actually supply.
        size_t scan_len = (size_t)len;
        if (!(lflags & LINUX_MAP_ANONYMOUS) && fd >= 0) {
            struct stat st;
            if (fstat((int)fd, &st) == 0) {
                off_t avail = st.st_size - (off_t)off;
                if (avail < 0)
                    avail = 0;
                if ((size_t)avail < scan_len)
                    scan_len = (size_t)avail;
            }
        }
        long rc = rewrite_and_seal(p, (size_t)len, (int)prot, scan_len,
                                 (lflags & LINUX_MAP_ANONYMOUS) ? -1 : (int)fd, off);
        if (rc < 0)
            return rc;
    }
    return (long)(uintptr_t)p;
}

static long do_mprotect_inner(uint64_t addr, uint64_t len, long prot);

// The host protection of the region holding p, and where that region ends
// (clamped to end). False for a hole (then *next is where mapping resumes).
static bool prot_run(uint64_t p, uint64_t end, vm_prot_t *prot, uint64_t *next)
{
    mach_vm_address_t ra = p;
    mach_vm_size_t rs = 0;
    vm_region_basic_info_data_64_t ri;
    mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                       (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS || ra >= end) {
        *next = end;
        return false;
    }
    if (ra > p) {
        *next = ra;
        return false;
    }
    *prot = ri.protection;
    *next = ra + rs < end ? ra + rs : end;
    return true;
}

// Hand a range the W^X split holds whole (wxsplit.c) over to protection
// prot, which is not read-write-execute. [addr, addr+len) is host-page
// aligned.
//
// The split's fault handler owns every page of the range until the range is
// forgotten; a fault it no longer claims reaches the guest as SIGSEGV. This
// used to forget the range first and then, for an executable request, open
// all of it read-write for rewrite_and_seal's scan: a thread running code in
// it or storing into a read-execute page of it died in between, although both
// the old protection and the new one allowed what it did (Linux lets such an
// access through). So, while the handler still owns the pages, each is taken
// to a state the new protection allows, through the handler itself -- under
// the split's lock, so no flip of another thread's interleaves with it:
//   executable request: every page not executable yet is flipped as a fetch
//     flips it (read-only, scanned, rewritten until clean, read-execute);
//     pages already read-execute were scanned when they became so. A thread
//     that fetches meanwhile is claimed and waits for the flip.
//   writable request: every page not writable is flipped as a store flips it.
//   read-only or none: nothing -- reads never fault, nothing else is allowed.
// Then the range is forgotten and the exact protection applied. A page that
// a racing access took back in between -- a store after its fetch flip, a
// fetch after its store flip, both forbidden by the new protection -- is put
// right after the forget: rescanned read-only before it becomes executable,
// as every executable page is.
//
// A thread that faulted while the split still held its page and is still
// waiting for the split's lock when the range is forgotten finds the range
// gone. The handler used to decline such a fault although the page allowed
// the access by then (tests/elf/wx_mprotect_race.c, stress mode: tens of
// faults in 300 rounds, against ~500000 with the range forgotten first); it
// now waits for the hand-over (lxrt_wx_handover) and rechecks the page.
static long wx_leave_whole_inner(uint64_t addr, uint64_t len, int prot);

// A fault handled while this runs finds the range gone: the handler waits for
// the hand-over and retries if the page allows the access (wxsplit.c,
// stale_fault_retry).
static long wx_leave_whole(uint64_t addr, uint64_t len, int prot)
{
    lxrt_wx_handover(true);
    long r = wx_leave_whole_inner(addr, len, prot);
    lxrt_wx_handover(false);
    return r;
}

static long wx_leave_whole_inner(uint64_t addr, uint64_t len, int prot)
{
    uint64_t end = addr + len, next;
    vm_prot_t cur;
    bool exec = prot & PROT_EXEC, write = prot & PROT_WRITE;
    if (exec || write) {
        vm_prot_t have = exec ? VM_PROT_EXECUTE : VM_PROT_WRITE;
        // The syndrome of the access the flip stands for: an instruction
        // abort, or a data abort with WnR (and pc != addr, which would read
        // as a fetch).
        uint32_t esr = exec ? 0x20u << 26 : (0x24u << 26) | (1u << 6);
        for (uint64_t p = addr; p < end; p = next) {
            if (!prot_run(p, end, &cur, &next) || (cur & have))
                continue;
            for (uint64_t q = p; q < next; q += LXRT_HOST_PAGE)
                lxrt_wx_handle_fault(exec ? q : 0, q, esr);
        }
    }
    lxrt_wx_forget(addr, len);
    if (exec) {
        for (uint64_t p = addr; p < end; p = next) {
            if (!prot_run(p, end, &cur, &next) ||
                ((cur & VM_PROT_EXECUTE) && !(cur & VM_PROT_WRITE)))
                continue;
            for (uint64_t q = p; q < next; q += LXRT_HOST_PAGE) {
                struct lxrt_range r = { q, q + LXRT_HOST_PAGE };
                if (lxrt_wx_scan_for_exec(q, &r, 1)) {
                    if (mprotect((void *)(uintptr_t)q, LXRT_HOST_PAGE, prot) != 0)
                        return LERR(errno);
                    sys_icache_invalidate((void *)(uintptr_t)q, LXRT_HOST_PAGE);
                    continue;
                }
                // The scan did not settle (or could not protect the page):
                // what the ordinary path does with it.
                if (mprotect((void *)(uintptr_t)q, LXRT_HOST_PAGE, PROT_READ | PROT_WRITE) != 0)
                    return LERR(errno);
                long rc = rewrite_and_seal((void *)(uintptr_t)q, LXRT_HOST_PAGE, prot,
                                           LXRT_HOST_PAGE, -1, 0);
                if (rc < 0)
                    return rc;
            }
        }
    }
    // Exactly what was asked (R-X becomes --X if so asked; RW- pages a racing
    // fetch made read-execute become RW- again). Nothing here makes a page
    // executable that was not scanned.
    return ret_of(mprotect((void *)(uintptr_t)addr, (size_t)len, prot));
}

// A non-RWX request over a range that intersects the W^X split: the pages the
// table holds are handed over by wx_leave_whole, the rest (and host pages the
// table holds only part of) go the ordinary way, run by run.
static long wx_leave(uint64_t addr, uint64_t len, int prot)
{
    if (lxrt_wx_covered(addr, len))
        return wx_leave_whole(addr, len, prot);
    uint64_t end = addr + len;
    for (uint64_t s = addr; s < end; ) {
        bool held = lxrt_wx_covered(s, LXRT_HOST_PAGE);
        uint64_t e = s + LXRT_HOST_PAGE;
        while (e < end && lxrt_wx_covered(e, LXRT_HOST_PAGE) == held)
            e += LXRT_HOST_PAGE;
        long r;
        if (held) {
            r = wx_leave_whole(s, e - s, prot);
        } else {
            lxrt_wx_forget(s, e - s);   // a partly held host page: ends here
            r = do_mprotect_inner(s, e - s, prot);
        }
        if (r != 0)
            return r;
        s = e;
    }
    return 0;
}

// Copy-on-write pages of private shared-memory mappings (privmap.c) stay
// unwritable on the host whatever the guest asks, until their first store.
static long do_mprotect(uint64_t addr, uint64_t len, long prot)
{
    {
        long lr;
        if (lxrt_lowpage_mprotect(addr, len, prot, &lr))
            return lr;
    }
    // Linux rounds the length up to whole pages (PAGE_ALIGN) before it looks
    // at anything; the paths below took it to the byte, so subpage.c recorded
    // a partial 4 KiB page and the tail of that page kept its old
    // protection. A length that is not a multiple of 4 KiB now covers the
    // page its last byte is in: for a native guest that sees the host's
    // 16 KiB page (AT_PAGESZ, no LXRT_GUEST_PAGE=4096), up to that 16 KiB
    // page's end, as a 16 KiB kernel does. Chromium makes its
    // protected_memory section (0x1001a bytes) read-only that way and CHECKs,
    // with prlimit64, that the page holding its last object is no longer
    // writable; that page kept read-write here and Electron's main process
    // died at start on a brk (MEASURED, benchmarks/stage24-heroic.txt).
    // Everyone else (FEX, whose x86 guests have 4 KiB pages, and native
    // guests told 4 KiB) gets 4 KiB. A multiple of 4 KiB is left as it is:
    // 4 KiB ranges inside a 16 KiB page are the sub-page extension
    // (subpage.c; tests/elf jit_rwx and wx_owner guards).
    if (len % 4096) {
        uint64_t pg = lxrt_wx_enabled() && lxrt_guest_page() == LXRT_HOST_PAGE
                      ? LXRT_HOST_PAGE : 4096;
        if (addr > UINT64_MAX - len || addr + len > UINT64_MAX - (pg - 1))
            return LERR(ENOMEM);
        len = LXRT_ALIGN_UP(addr + len, pg) - addr;
    }
    long r = do_mprotect_inner(addr, len, prot);
    if (r == 0)
        lxrt_privmap_after_mprotect(addr, len, (int)prot);
    return r;
}

// mremap of 4 KiB guest pages that do not fill their 16 KiB host pages, which
// mremap.c refuses. bionic's linker needs it before main(): its CFI shadow
// (linker_cfi.cpp, ShadowWrite) fills a private copy and moves it with
// mremap(MREMAP_MAYMOVE | MREMAP_FIXED) over a 4 KiB-aligned slice of the
// shadow reservation, and CHECK-fails otherwise ("~ShadowWrite CHECK 'res !=
// MAP_FAILED' failed": every Android program, benchmarks/stage25-android-
// userspace.txt). Private anonymous memory, as that is: a new private
// mapping through do_mmap, the bytes copied, the protection set through
// do_mprotect, the old range unmapped through do_munmap -- the same paths,
// and the same 4 KiB bookkeeping, as the guest's own calls. A shared or
// file-backed source is not taken (it would lose its backing): ENOMEM, as
// before. Growing in place is not tried: without MREMAP_MAYMOVE that is
// ENOMEM, which Linux also answers when the next pages are taken.
static long do_mremap_subpage(uint64_t old, uint64_t olen, uint64_t nlen, long fl, uint64_t naddr)
{
    enum { MAYMOVE = 1, FIXED = 2, DONTUNMAP = 4 };
    if ((old & 4095) || !olen || !nlen || (fl & ~7L) ||
        ((fl & (FIXED | DONTUNMAP)) && !(fl & MAYMOVE)))
        return LERR(EINVAL);
    if (olen > UINT64_MAX - 4095 || nlen > UINT64_MAX - 4095)
        return LERR(ENOMEM);
    olen = LXRT_ALIGN_UP(olen, 4096);
    nlen = LXRT_ALIGN_UP(nlen, 4096);
    if (old > UINT64_MAX - olen)
        return LERR(EFAULT);
    if ((fl & DONTUNMAP) && olen != nlen)
        return LERR(EINVAL);
    if ((fl & FIXED) && ((naddr & 4095) || naddr > UINT64_MAX - nlen ||
                         (naddr < old + olen && old < naddr + nlen)))
        return LERR(EINVAL);
    if (!(fl & (FIXED | DONTUNMAP)) && nlen <= olen) {
        if (nlen < olen) {
            long r = do_munmap(old + nlen, olen - nlen);
            if (r != 0)
                return r;
        }
        return (long)old;
    }
    if (!(fl & MAYMOVE))
        return LERR(ENOMEM);
    // Every guest page of the source mapped, with one protection (Linux
    // moves one VMA; a range across several is EFAULT).
    int prot = -2;
    for (uint64_t g = old; g < old + olen; g += 4096) {
        bool shared = false;
        int hp = lxrt_mremap_host_prot(g, &shared);
        int p = lxrt_subpage_prot_at(g);
        if (p < 0)
            p = hp;
        if (hp < 0 || p < 0 || (prot != -2 && p != prot))
            return LERR(EFAULT);
        if (shared) {
            fprintf(lxrt_trace_stream(), "[lxrt] mremap: sub-page shared range 0x%llx+0x%llx: "
                            "not supported\n", (unsigned long long)old, (unsigned long long)olen);
            return LERR(ENOMEM);
        }
        prot = p;
    }
    long dst = do_mmap((fl & FIXED) ? naddr : 0, nlen, PROT_READ | PROT_WRITE,
                       LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | ((fl & FIXED) ? LINUX_MAP_FIXED : 0),
                       -1, 0);
    if (dst < 0)
        return dst;
    uint64_t n = olen < nlen ? olen : nlen;
    if (lxrt_mremap_copy_out((void *)(uintptr_t)dst, old, n) != 0) {
        do_munmap((uint64_t)dst, nlen);
        return LERR(EFAULT);
    }
    if (prot != (PROT_READ | PROT_WRITE)) {
        long r = do_mprotect((uint64_t)dst, nlen, prot);
        if (r != 0) {
            do_munmap((uint64_t)dst, nlen);
            return r;
        }
    }
    long r = (fl & DONTUNMAP)
        ? do_mmap(old, olen, prot, LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS | LINUX_MAP_FIXED, -1, 0)
        : do_munmap(old, olen);
    if (r < 0)
        return r;
    return dst;
}

static long do_mprotect_inner(uint64_t addr, uint64_t len, long prot)
{
    // A JIT region's protection is the per-thread switch, not the mapping's;
    // changing it would revoke MAP_JIT. Accept and ignore.
    if (lxrt_jit_contains(addr))
        return 0;

    // A native aarch64 guest's read-write-execute range is split W^X page by
    // page (wxsplit.c): V8 makes its whole code range RWX with one call. Any
    // other protection ends the split for that range (wx_leave: the pages are
    // handed over without a moment in which an access both protections allow
    // faults unclaimed); what the table does not hold goes the ordinary way
    // below. Sub-page ranges go through subpage.c, whose union flips scan the
    // RWX guest pages the same way.
    bool rwx = (prot & PROT_WRITE) && (prot & PROT_EXEC);
    if (lxrt_wx_enabled()) {
        if (!rwx) {
            if ((addr % LXRT_HOST_PAGE) == 0 && (len % LXRT_HOST_PAGE) == 0 &&
                lxrt_wx_intersects(addr, len) && !lxrt_subpage_tracked(addr, len))
                return wx_leave(addr, len, (int)prot);
            lxrt_wx_forget(addr, len);
        } else if ((addr % LXRT_HOST_PAGE) == 0 && (len % LXRT_HOST_PAGE) == 0 &&
                 !lxrt_subpage_tracked(addr, len)) {
            long wret;
            if (lxrt_wx_protect(addr, len, &wret))
                return wret;
        }
    }

    // Sub-page mappings have to go through the union bookkeeping, or one guest
    // page's mprotect silently changes its 16 KiB neighbours. A partial LENGTH
    // is the same hazard as an unaligned address: glibc's RELRO makes the
    // first 4 KiB of a 20 KiB rw mapping read-only, a plain mprotect rounded
    // that to the whole host page, and CEF's next store into the second 4 KiB
    // took SIGSEGV (benchmarks/stage5-fex.txt, steamwebhelper).
    if ((addr % LXRT_HOST_PAGE) != 0 || (len % LXRT_HOST_PAGE) != 0 ||
        lxrt_subpage_tracked(addr, len))
        return lxrt_subpage_mprotect(addr, len, (int)prot);

    // Read-write-execute outside MAP_JIT under FEX: Darwin refuses it
    // (EACCES), and a region cannot be turned into MAP_JIT in place. The
    // guest that asks is x86 V8, making its code range RWX -- x86 code the
    // host never executes (FEX translates it), so the host protection it
    // needs is read-write. Without this, every steamwebhelper renderer died
    // with "V8 process OOM (Failed to reserve virtual memory for CodeRange)"
    // (MEASURED). A native guest reaches this only for memory the W^X split
    // does not take (file-backed or shared); executing it faults.
    if (rwx) {
        static _Atomic int said;
        if (!atomic_fetch_add(&said, 1) && g_trace)
            fprintf(lxrt_trace_stream(), "[lxrt] mprotect 0x%llx+0x%llx RWX outside MAP_JIT: "
                    "granted as RW (%s)\n",
                    (unsigned long long)addr, (unsigned long long)len,
                    lxrt_wx_enabled() ? "native guest, not private anonymous memory"
                                      : "host never executes guest x86 code");
        return ret_of(mprotect((void *)addr, (size_t)len, (int)(prot & ~PROT_EXEC)));
    }

    // A loader commonly maps a segment read-write, relocates it, then adds
    // PROT_EXEC. That transition is the last chance to rewrite it.
    if (g_rewrite_mapped && (prot & PROT_EXEC)) {
        uint64_t start = LXRT_ALIGN_DOWN(addr, LXRT_HOST_PAGE);
        uint64_t end = LXRT_ALIGN_UP(addr + len, LXRT_HOST_PAGE);
        if (mprotect((void *)start, (size_t)(end - start),
                     PROT_READ | PROT_WRITE) != 0)
            return LERR(errno);
        // No fd here, so the whole range is assumed backed: an mprotect to
        // executable normally follows a private, already-faulted mapping.
        // mprotect to PROT_EXEC: no file behind it that we know of, so no
        // section windows -- the x18 pass does not run here (JIT regions
        // are FEX's own output; FEX itself no longer uses x18).
        return rewrite_and_seal((void *)addr, (size_t)len, (int)prot, (size_t)len, -1, 0);
    }
    return ret_of(mprotect((void *)addr, (size_t)len, (int)prot));
}

// brk(). On Linux, memory beyond the program break is NOT mapped, and that
// matters: FEX's allocator asks for the page immediately after the break with
// MAP_FIXED_NOREPLACE and gives up if it is taken. An implementation that
// pre-reserves a large heap looks correct until something probes past it --
// "Couldn't allocate page after SBRK" was the whole of FEX's complaint.
//
// So the break is grown and shrunk a page at a time against the real address
// space, exactly as the kernel does.

static uint64_t g_brk_base, g_brk_cur;

// Where the heap starts. Linux puts it just past the image and leaves a very
// large hole above it; Darwin's allocator packs mappings together, so the guest
// stack landed immediately after the image and FEX's "give me the page after
// the break" probe hit it.
//
// So the break is placed inside a hole the kernel confirms is free: reserve a
// large range ANYWHERE, note where it landed, and give it straight back. The
// hole is not owned afterwards -- that is the point, since anything the runtime
// keeps reserved is exactly what a probe like FEX's fails against. It is the
// same bargain Linux offers: the space above the break is free until something
// maps there.
#define LXRT_HEAP_HOLE (4ull << 30)

void lxrt_dispatch_init_brk(uint64_t fallback)
{
    // A hole of the guest's own (arena.c): the host's heap cannot open a
    // region in it, and the page after the break is still free to the guest.
    uint64_t heap = lxrt_arena_reserve_heap();
    if (heap) {
        g_brk_base = g_brk_cur = heap;
        return;
    }
    mach_vm_address_t at = 0;
    if (mach_vm_allocate(mach_task_self(), &at, LXRT_HEAP_HOLE,
                         VM_FLAGS_ANYWHERE) == KERN_SUCCESS) {
        mach_vm_deallocate(mach_task_self(), at, LXRT_HEAP_HOLE);
        g_brk_base = g_brk_cur = LXRT_ALIGN_UP((uint64_t)at, LXRT_HOST_PAGE);
        return;
    }
    g_brk_base = g_brk_cur = LXRT_ALIGN_UP(fallback, LXRT_HOST_PAGE);
}

static long do_brk(uint64_t want)
{
    if (!g_brk_base)
        return LERR(ENOMEM);
    if (want == 0 || want == g_brk_cur)
        return (long)g_brk_cur;
    if (want < g_brk_base)
        return (long)g_brk_cur;   // Linux returns the unchanged break on failure

    uint64_t cur_page = LXRT_ALIGN_UP(g_brk_cur, LXRT_HOST_PAGE);
    uint64_t want_page = LXRT_ALIGN_UP(want, LXRT_HOST_PAGE);

    if (want_page > cur_page) {
        mach_vm_address_t at = cur_page;
        if (lxrt_arena_free(cur_page, want_page - cur_page)) {
            // In the guest's hole: the new pages take the reservation's place.
            if (mmap((void *)(uintptr_t)cur_page, (size_t)(want_page - cur_page), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED)
                return (long)g_brk_cur;
            lxrt_arena_mapped(cur_page, want_page - cur_page);
            g_brk_cur = want;
            return (long)g_brk_cur;
        }
        if (mach_vm_allocate(mach_task_self(), &at,
                             (mach_vm_size_t)(want_page - cur_page),
                             VM_FLAGS_FIXED) != KERN_SUCCESS)
            return (long)g_brk_cur;
        if (mprotect((void *)at, (size_t)(want_page - cur_page),
                     PROT_READ | PROT_WRITE) != 0) {
            mach_vm_deallocate(mach_task_self(), at,
                               (mach_vm_size_t)(want_page - cur_page));
            return (long)g_brk_cur;
        }
    } else if (want_page < cur_page) {
        mach_vm_deallocate(mach_task_self(), (mach_vm_address_t)want_page,
                           (mach_vm_size_t)(cur_page - want_page));
        lxrt_arena_unmapped(want_page, cur_page - want_page);
    }

    g_brk_cur = want;
    return (long)g_brk_cur;
}

// ---------------------------------------------------------------- timers

struct linux_timespec { int64_t tv_sec; int64_t tv_nsec; };

// Linux aarch64 timeval is two 64-bit fields; Darwin's tv_usec is 32-bit with
// padding. Same size, different layout -- a cast would read the microseconds
// out of the padding.
struct linux_timeval { int64_t tv_sec; int64_t tv_usec; };
struct linux_itimerval { struct linux_timeval it_interval, it_value; };

static void itimerval_to_darwin(const struct linux_itimerval *l,
                                struct itimerval *d)
{
    d->it_interval.tv_sec  = (time_t)l->it_interval.tv_sec;
    d->it_interval.tv_usec = (suseconds_t)l->it_interval.tv_usec;
    d->it_value.tv_sec     = (time_t)l->it_value.tv_sec;
    d->it_value.tv_usec    = (suseconds_t)l->it_value.tv_usec;
}

static void itimerval_to_linux(const struct itimerval *d,
                               struct linux_itimerval *l)
{
    l->it_interval.tv_sec  = d->it_interval.tv_sec;
    l->it_interval.tv_usec = d->it_interval.tv_usec;
    l->it_value.tv_sec     = d->it_value.tv_sec;
    l->it_value.tv_usec    = d->it_value.tv_usec;
}

// ITIMER_REAL/VIRTUAL/PROF are 0/1/2 on both systems.
static long do_setitimer(int which, const void *newp, void *oldp)
{
    struct itimerval dnew, dold;
    if (newp)
        itimerval_to_darwin((const struct linux_itimerval *)newp, &dnew);
    if (setitimer(which, newp ? &dnew : NULL, oldp ? &dold : NULL) != 0)
        return LERR(errno);
    if (oldp)
        itimerval_to_linux(&dold, (struct linux_itimerval *)oldp);
    return 0;
}

static long do_getitimer(int which, void *oldp)
{
    struct itimerval dold;
    if (getitimer(which, &dold) != 0)
        return LERR(errno);
    if (oldp)
        itimerval_to_linux(&dold, (struct linux_itimerval *)oldp);
    return 0;
}

// Darwin has poll but not ppoll. The mask swap around it is not atomic, which
// is the whole reason ppoll exists; the window is documented rather than hidden
// because closing it needs kernel support we do not have.
// poll(2) with a zero timeout is not free of sleeping on this kernel. MEASURED
// on this machine (macOS 27, M4), 20000 calls each, nothing ready: poll(pf, 1,
// 0) median 1.6 us but mean 5.4 us, 37 % of the calls over 8 us (12 us: the
// next timer deadline, as a kqueue wait of zero -- see runtime/epoll_eventfd.c
// kev_to64); poll(NULL, 0, 0) the same. select() with a zero timeval never
// sleeps: 170 ns for one fd, 300 ns for eight. So "anything now?" is asked of
// select first, and poll only spells out the revents of what select found,
// which it does at once (1.1 us) because something IS ready. What select
// cannot express is left to poll: an fd of FD_SETSIZE or more, an interest in
// nothing but POLLHUP/POLLERR (events = 0), and a descriptor select refuses
// (EBADF is POLLNVAL). EOF and errors count as readable or writable for
// select, as for poll, so a zero from select is a zero from poll.
// LXRT_POLL0_SELECT=0 turns it off.
static bool poll0_select_empty(struct pollfd *pf, nfds_t nfds)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_POLL0_SELECT");
        on = !e || atoi(e) != 0;
    }
    if (!on || nfds > 1024)
        return false;
    fd_set rs, ws, es;
    FD_ZERO(&rs);
    FD_ZERO(&ws);
    FD_ZERO(&es);
    int maxfd = -1;
    for (nfds_t i = 0; i < nfds; i++) {
        int fd = pf[i].fd;
        if (fd < 0)
            continue;       // poll skips it and reports revents 0
        if (fd >= FD_SETSIZE)
            return false;
        short ev = pf[i].events;
        if (!(ev & (POLLIN | POLLRDNORM | POLLRDBAND | POLLPRI |
                    POLLOUT | POLLWRNORM | POLLWRBAND)))
            return false;
        if (ev & (POLLIN | POLLRDNORM | POLLRDBAND))
            FD_SET(fd, &rs);
        if (ev & (POLLOUT | POLLWRNORM | POLLWRBAND))
            FD_SET(fd, &ws);
        if (ev & POLLPRI)
            FD_SET(fd, &es);
        if (fd > maxfd)
            maxfd = fd;
    }
    struct timeval zero = { 0, 0 };
    if (select(maxfd + 1, &rs, &ws, &es, &zero) != 0)
        return false;
    for (nfds_t i = 0; i < nfds; i++)
        pf[i].revents = 0;
    return true;
}

static long do_ppoll(uint64_t fds, uint64_t nfds, uint64_t ts, uint64_t sigmask)
{
    // LXRT_FUTEX_DEBUG=1: the first polls of 50 ms to a second, with the x86
    // caller under FEX (a game sleeping in its event loop: Counter-Strike 2
    // waits 100 ms per frame while its window has no focus).
    if (ts && getenv("LXRT_FUTEX_DEBUG")) {
        extern void lxrt_debug_x86_stack(void);
        static _Atomic int said;
        const struct linux_timespec *t = (const struct linux_timespec *)(uintptr_t)ts;
        if (!t->tv_sec && t->tv_nsec >= 50000000 && atomic_fetch_add(&said, 1) < 6) {
            fprintf(lxrt_trace_stream(), "[lxrt] ppoll of %lld.%03lld s on %llu fds\n", (long long)t->tv_sec,
                    (long long)(t->tv_nsec / 1000000), (unsigned long long)nfds);
            lxrt_debug_x86_stack();
        }
    }
    int timeout_ms = -1;
    if (ts) {
        const struct linux_timespec *t = (const struct linux_timespec *)ts;
        int64_t ms = t->tv_sec * 1000 + t->tv_nsec / 1000000;
        timeout_ms = ms > INT_MAX ? INT_MAX : (int)ms;
    }

    // Linux installs the mask for the duration of the call only and puts the
    // caller's back on return; leaving the ppoll mask in place changed which
    // signals the thread would take for good.
    uint64_t oldmask = 0;
    bool swapped = false;
    if (sigmask) {
        long rc = lxrt_rt_sigprocmask(2 /* SIG_SETMASK */,
                                      (const uint64_t *)sigmask, &oldmask, 8);
        if (rc < 0)
            return rc;
        swapped = true;
    }

    // A seqpacket socket's peer going away does not wake a blocked poll
    // (runtime/socket.c, SEQPKT_LINGER): with one in the set, the wait is
    // made in slices, each a fresh poll that does see it.
    struct pollfd *pf = (struct pollfd *)fds;
    bool sliced = false;
    if (timeout_ms < 0 || timeout_ms > 250)
        for (uint64_t i = 0; i < nfds && i < 64 && !sliced; i++)
            sliced = (pf[i].events & POLLIN) && lxrt_is_seqpacket(pf[i].fd);
    int r;
    if (timeout_ms == 0 && poll0_select_empty(pf, (nfds_t)nfds)) {
        r = 0;
    } else if (!sliced) {
        r = poll(pf, (nfds_t)nfds, timeout_ms);
    } else {
        int left = timeout_ms;
        for (;;) {
            int slice = left < 0 || left > 250 ? 250 : left;
            r = poll(pf, (nfds_t)nfds, slice);
            if (r != 0 || left == slice)
                break;
            if (left > 0)
                left -= slice;
        }
    }
    long ret = r < 0 ? LERR(errno) : r;

    if (swapped)
        lxrt_rt_sigprocmask(2 /* SIG_SETMASK */, &oldmask, NULL, 8);
    return ret;
}

// pselect6 (glibc's select/pselect both end here). Darwin's select caps fds at
// FD_SETSIZE and uses a different fd_set word size, so the sets are turned
// into a pollfd array instead: same readiness rules, no fd limit. The Linux
// bit layout is an array of 64-bit words, bit (fd % 64) of word fd / 64.
static long do_pselect6(uint64_t nfds_, uint64_t rset, uint64_t wset, uint64_t eset,
                        uint64_t ts, uint64_t sigdata)
{
    int nfds = (int)nfds_;
    if (nfds < 0 || nfds > 65536)
        return LERR(EINVAL);
    uint64_t *r = (uint64_t *)rset, *w = (uint64_t *)wset, *e = (uint64_t *)eset;
    int words = (nfds + 63) / 64;
    struct pollfd *pfd = nfds ? calloc((size_t)nfds, sizeof *pfd) : NULL;
    if (nfds && !pfd)
        return LERR(ENOMEM);
    int n = 0;
    for (int fd = 0; fd < nfds; fd++) {
        uint64_t bit = 1ull << (fd % 64);
        short ev = 0;
        if (r && (r[fd / 64] & bit)) ev |= POLLIN;
        if (w && (w[fd / 64] & bit)) ev |= POLLOUT;
        if (e && (e[fd / 64] & bit)) ev |= POLLPRI;
        if (ev) { pfd[n].fd = fd; pfd[n].events = ev; n++; }
    }

    int timeout_ms = -1;
    struct timespec start;
    if (ts) {
        const struct linux_timespec *t = (const struct linux_timespec *)ts;
        if (t->tv_sec < 0 || t->tv_nsec < 0 || t->tv_nsec >= 1000000000L) { free(pfd); return LERR(EINVAL); }
        int64_t ms = t->tv_sec * 1000 + (t->tv_nsec + 999999) / 1000000;
        timeout_ms = ms > INT_MAX ? INT_MAX : (int)ms;
        clock_gettime(CLOCK_MONOTONIC, &start);
    }
    uint64_t ps_oldmask = 0;
    bool ps_swapped = false;
    if (sigdata) {
        // struct { const sigset_t *ss; size_t ss_len; }; ss is a guest pointer.
        const uint64_t *sd = (const uint64_t *)sigdata;
        uint64_t ss = sd[0];
        if (ss && ss < (1ull << 32) && lxrt_gbase())
            ss += lxrt_gbase();
        if (ss) {
            long rc = lxrt_rt_sigprocmask(2 /* SIG_SETMASK */, (const uint64_t *)ss, &ps_oldmask, 8);
            if (rc < 0) { free(pfd); return rc; }
            ps_swapped = true;
        }
    }

    int rc = poll(pfd, (nfds_t)n, timeout_ms);
    int poll_errno = errno;
    // As for ppoll: the mask holds only for the call.
    if (ps_swapped)
        lxrt_rt_sigprocmask(2 /* SIG_SETMASK */, &ps_oldmask, NULL, 8);
    if (rc < 0) { long err = LERR(poll_errno); free(pfd); return err; }

    if (r) memset(r, 0, (size_t)words * 8);
    if (w) memset(w, 0, (size_t)words * 8);
    if (e) memset(e, 0, (size_t)words * 8);
    long ready = 0;
    for (int i = 0; i < n; i++) {
        int fd = pfd[i].fd;
        uint64_t bit = 1ull << (fd % 64);
        short re = pfd[i].revents;
        if (re & POLLNVAL) { free(pfd); return LERR(EBADF); }
        if (r && (pfd[i].events & POLLIN) && (re & (POLLIN | POLLHUP | POLLERR))) { r[fd / 64] |= bit; ready++; }
        if (w && (pfd[i].events & POLLOUT) && (re & (POLLOUT | POLLERR))) { w[fd / 64] |= bit; ready++; }
        if (e && (pfd[i].events & POLLPRI) && (re & POLLPRI)) { e[fd / 64] |= bit; ready++; }
    }
    free(pfd);

    if (ts) {
        // Linux writes the time left back.
        struct linux_timespec *t = (struct linux_timespec *)ts;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t left = (t->tv_sec * 1000000000LL + t->tv_nsec)
                     - ((now.tv_sec - start.tv_sec) * 1000000000LL + (now.tv_nsec - start.tv_nsec));
        if (left < 0) left = 0;
        t->tv_sec = left / 1000000000LL;
        t->tv_nsec = left % 1000000000LL;
    }
    return ready;
}

// ---------------------------------------------------------------- sleep

// mount("tmpfs", dir, "tmpfs", flags, "uid=..,gid=..,mode=..") for Android
// ids: a new empty directory of the runtime's, bound on dir in this
// process's mount table (mounts.c), as a tmpfs in a private mount namespace
// is. Android's zygote isolates a process's view of other apps' data and
// JIT profiles that way before it forks WebView's zygote and isolated
// services ("Failed to mount tmpfs to /data/misc/profiles/cur: Operation not
// permitted" killed every WebView renderer, MEASURED). The directories live
// in /tmp/lxrt-tmpfs-<uid>/<pid>-*; those of processes that are gone are
// swept at the next such mount.
static void tmpfs_remove(const char *path)
{
    DIR *d = lxrt_opendir_private(path);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            char sub[PATH_MAX];
            snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
            struct stat st;
            if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) tmpfs_remove(sub);
            else unlink(sub);
        }
        lxrt_closedir_private(d);
    }
    rmdir(path);
}
static long tmpfs_mount(const char *dst, const char *data, bool ro)
{
    char base[64];
    snprintf(base, sizeof base, "/tmp/lxrt-tmpfs-%u", (unsigned)getuid());
    if (mkdir(base, 0700) != 0 && errno != EEXIST) return LERR(errno);
    static bool swept;
    if (!swept) {
        swept = true;
        DIR *d = lxrt_opendir_private(base);
        struct dirent *e;
        while (d && (e = readdir(d))) {
            char *end;
            long pid = strtol(e->d_name, &end, 10);
            if (end == e->d_name || *end != '-' || pid <= 0) continue;
            if (kill((pid_t)pid, 0) == 0 || errno != ESRCH) continue;
            char sub[PATH_MAX];
            snprintf(sub, sizeof sub, "%s/%s", base, e->d_name);
            tmpfs_remove(sub);
        }
        if (d) lxrt_closedir_private(d);
    }
    char dir[PATH_MAX];
    snprintf(dir, sizeof dir, "%s/%d-XXXXXX", base, (int)getpid());
    if (!mkdtemp(dir)) return LERR(errno);
    unsigned mode = 01777, uid = 0, gid = 0;
    for (const char *o = data; o && *o; ) {
        sscanf(o, "mode=%o", &mode);
        sscanf(o, "uid=%u", &uid);
        sscanf(o, "gid=%u", &gid);
        const char *c = strchr(o, ',');
        o = c ? c + 1 : NULL;
    }
    chmod(dir, (mode_t)(mode & 07777));
    lxrt_aids_chown_file(dir, -1, false, uid, gid);
    lxrt_mounts_bind(dst, dir, ro);
    if (getenv("LXRT_ANDROID_IDS_LOG"))
        fprintf(stderr, "[lxrt] pid %d: tmpfs %s (%s) on %s\n", (int)getpid(), data ? data : "", dir, dst);
    return 0;
}

static long do_nanosleep(uint64_t req, uint64_t rem)
{
    const struct linux_timespec *r = (const struct linux_timespec *)req;
    if (!r)
        return LERR(EFAULT);
    struct timespec ts = { (time_t)r->tv_sec, (long)r->tv_nsec };
    struct timespec left = { 0, 0 };
    for (;;) {
        if (nanosleep(&ts, &left) == 0)
            return 0;
        // Cut short by a signal that ran no guest handler: sleep the rest.
        // The Steam client's ThreadSleep is one nanosleep with no retry, and
        // its wait for the web helper (2400 x ThreadSleep(50), two minutes)
        // ended in under a second: "Timed out waiting for webhelper init",
        // and no window (MEASURED, about one x86 start in five).
        if (errno == EINTR && lxrt_interrupted_internally()) {
            ts = left;
            continue;
        }
        // Linux writes the remaining time back on EINTR; leaving it stale makes
        // a retry loop sleep for the full interval again.
        if (errno == EINTR && rem) {
            struct linux_timespec *o = (struct linux_timespec *)rem;
            o->tv_sec = left.tv_sec;
            o->tv_nsec = left.tv_nsec;
        }
        return LERR(errno);
    }
}

static long do_nanosleep_abs(long clk, uint64_t req)
{
    const struct linux_timespec *r = (const struct linux_timespec *)req;
    if (!r)
        return LERR(EFAULT);
    // Darwin has no absolute clock_nanosleep; convert to a relative wait,
    // against the clock the guest itself reads (lxrt_guest_clock_ns).
    for (;;) {
        int64_t now = (int64_t)lxrt_guest_clock_ns(clk);
        int64_t delta = r->tv_sec * 1000000000LL + r->tv_nsec - now;
        if (delta <= 0)
            return 0;
        struct timespec ts = { (time_t)(delta / 1000000000LL),
                               (long)(delta % 1000000000LL) };
        if (nanosleep(&ts, NULL) == 0)
            return 0;
        if (errno != EINTR || !lxrt_interrupted_internally())
            return LERR(errno);
        // no guest handler ran: the deadline stands (lxrt_interrupted_internally)
    }
}

// ---------------------------------------------------------------- stat

// Linux aarch64 `struct stat` (the asm-generic layout), 128 bytes. Darwin's is
// a different shape entirely, so this is a field-by-field translation, not a
// cast. stdio calls fstat on every stream to choose its buffering mode, so
// getting it wrong shows up as output appearing in the wrong order rather than
// as an obvious failure.
struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t __pad1;
    int64_t  st_size;
    int32_t  st_blksize;
    int32_t  __pad2;
    int64_t  st_blocks;
    int64_t  st_atime_sec,  st_atime_nsec;
    int64_t  st_mtime_sec,  st_mtime_nsec;
    int64_t  st_ctime_sec,  st_ctime_nsec;
    uint32_t __unused_[2];
};

// A dev_t in Linux's encoding (glibc makedev: minor low byte, major in bits
// 8-19, the rest above). Darwin packs major in bits 24-31. Passing Darwin's
// value through made stat() and statx() -- which carries major/minor apart and
// is recombined by the guest's glibc -- disagree about the same file: wine's
// client and wineserver named the server directory server-118-... and
// server-1000018-... from one prefix and never met (MEASURED).
static uint64_t dev_to_linux(dev_t dv)
{
    uint64_t ma = (uint64_t)major(dv), mi = (uint64_t)minor(dv);
    return ((ma & 0xfffull) << 8) | ((ma & ~0xfffull) << 32) | (mi & 0xffull) | ((mi & ~0xffull) << 12);
}

static void stat_to_linux(const struct stat *d, struct linux_stat *l)
{
    memset(l, 0, sizeof(*l));
    l->st_dev     = dev_to_linux(d->st_dev);
    l->st_ino     = (uint64_t)d->st_ino;
    l->st_mode    = (uint32_t)d->st_mode;   // S_IF* values agree on both
    l->st_nlink   = (uint32_t)d->st_nlink;
    l->st_uid     = (uint32_t)d->st_uid;
    l->st_gid     = (uint32_t)d->st_gid;
    l->st_rdev    = dev_to_linux(d->st_rdev);
    l->st_size    = (int64_t)d->st_size;
    l->st_blksize = (int32_t)d->st_blksize;
    l->st_blocks  = (int64_t)d->st_blocks;
    l->st_atime_sec  = d->st_atimespec.tv_sec;
    l->st_atime_nsec = d->st_atimespec.tv_nsec;
    l->st_mtime_sec  = d->st_mtimespec.tv_sec;
    l->st_mtime_nsec = d->st_mtimespec.tv_nsec;
    l->st_ctime_sec  = d->st_ctimespec.tv_sec;
    l->st_ctime_nsec = d->st_ctimespec.tv_nsec;
}

static long do_fstat(int fd, uint64_t out)
{
    if (!out)
        return LERR(EFAULT);
    struct stat d;
    if (lxrt_binder_fstat(fd, &d)) {        // a binder device, not the socket under it
        stat_to_linux(&d, (struct linux_stat *)out);
        return 0;
    }
    const char *pf = lxrt_pathfd_path(fd);
    if (pf ? stat(pf, &d) != 0 : fstat(fd, &d) != 0)
        return LERR(errno);
    lxrt_evdev_fix_stat(pf, fd, &d);
    lxrt_props_fix_stat(pf, fd, &d);
    lxrt_aids_fix_stat(pf, pf ? -1 : fd, false, &d);
    stat_to_linux(&d, (struct linux_stat *)out);
    return 0;
}

static const char *translate(const char *path);
static const char *translate_follow(const char *path);

// ---------------------------------------------------------------- xattrs
//
// Linux's "user." namespace is the one ordinary programs use, and Darwin
// keeps arbitrary names on the same files: a "user.X" attribute is stored as
// the Darwin attribute "user.X". Android's framework needs it: UserDataPreparer
// marks every user directory with user.serial and, when getxattr failed,
// destroyed /data/user_de/0 and /data/system_ce/0 at every boot and
// system_server died right after (MEASURED at stage 28); installd marks app
// data with user.default and user.inode_cache. The other namespaces
// (security., trusted., system.) stay ENOTSUP -- what a filesystem without
// them answers -- and a listing shows only user. names, so Darwin's own
// com.apple.* attributes and the runtime's (android_ids.h) stay out of sight.
static bool xattr_user(const char *name)
{
    return name && !strncmp(name, "user.", 5) && name[5];
}

static long xattr_err(int e)
{
    return e == ENOATTR ? -61 : LERR(e);        // Linux ENODATA
}

// nr: the aarch64 number (5 setxattr .. 16 fremovexattr). a0 is a path or a
// descriptor; the l* forms do not follow a final symlink.
static long do_xattr(long nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4)
{
    int kind = (int)((nr - 5) % 3);             // 0 path, 1 lpath, 2 fd
    int op = (int)((nr - 5) / 3);               // 0 set, 1 get, 2 list, 3 remove
    const char *path = NULL;
    int fd = -1;
    char full[PATH_MAX];
    if (kind == 2) {
        fd = (int)a0;
        const char *pf = lxrt_pathfd_path(fd);
        if (pf) { snprintf(full, sizeof full, "%s", pf); path = full; kind = 0; }
    } else {
        if (!a0) return LERR(EFAULT);
        snprintf(full, sizeof full, "%s", kind == 1 ? translate((const char *)a0) : translate_follow((const char *)a0));
        path = full;
    }
    int opt = kind == 1 ? XATTR_NOFOLLOW : 0;
    const char *name = (const char *)a1;
    if (op != 2) {
        if (!name) return LERR(EFAULT);
        if (strlen(name) > 255) return LERR(ERANGE);
        if (!xattr_user(name)) return LERR(ENOTSUP);
    }
    ssize_t r;
    switch (op) {
    case 0: {                                   // set: value a2, size a3, flags a4
        int lf = (int)a4;
        if (lf & ~3) return LERR(EINVAL);
        if (a3 > 65536) return LERR(E2BIG);
        int o = opt | ((lf & 1) ? XATTR_CREATE : 0) | ((lf & 2) ? XATTR_REPLACE : 0);
        int rc = path ? setxattr(path, name, (const void *)a2, (size_t)a3, 0, o)
                      : fsetxattr(fd, name, (const void *)a2, (size_t)a3, 0, o);
        return rc != 0 ? xattr_err(errno) : 0;
    }
    case 1:                                     // get: buffer a2, size a3
        r = path ? getxattr(path, name, a3 ? (void *)a2 : NULL, (size_t)a3, 0, opt)
                 : fgetxattr(fd, name, a3 ? (void *)a2 : NULL, (size_t)a3, 0, 0);
        return r < 0 ? xattr_err(errno) : (long)r;
    case 2: {                                   // list: buffer a1, size a2
        ssize_t n = path ? listxattr(path, NULL, 0, opt) : flistxattr(fd, NULL, 0, 0);
        if (n < 0) return xattr_err(errno);
        char *all = malloc((size_t)n + 1);
        if (!all) return LERR(ENOMEM);
        n = path ? listxattr(path, all, (size_t)n, opt) : flistxattr(fd, all, (size_t)n, 0);
        if (n < 0) { int e = errno; free(all); return xattr_err(e); }
        size_t out = 0;
        for (ssize_t i = 0; i < n; ) {
            size_t l = strlen(all + i) + 1;
            if (xattr_user(all + i)) {
                if (a2 && out + l > a2) { free(all); return LERR(ERANGE); }
                if (a2) memcpy((char *)a1 + out, all + i, l);
                out += l;
            }
            i += (ssize_t)l;
        }
        free(all);
        return (long)out;
    }
    default: {                                  // remove
        int rc = path ? removexattr(path, name, opt) : fremovexattr(fd, name, 0);
        return rc != 0 ? xattr_err(errno) : 0;
    }
    }
}
// process_vm_readv / process_vm_writev (270 / 271) on the calling process.
// ART reads the faulting instruction this way before it turns a fault into a
// NullPointerException (art::SafeCopy, GetInstructionSize in its x86-64 fault
// handler); the ENOSYS it got made SafeCopy return 0 bytes, the handler gave
// up, and every NullPointerException in compiled code killed the process with
// SIGSEGV -- system_server included (MEASURED at stage 28). Mach's
// vm_read_overwrite copies within the task and fails on unmapped or
// unreadable memory instead of faulting, which is what these calls promise.
// Another process's memory would need its task port: EPERM, as for a
// process one may not ptrace. Low-window guest addresses (below 4 GiB under
// a guest base) inside the iovecs are translated as the table does for the
// arrays themselves (gbase.c).
static long do_process_vm(bool write, uint64_t pid, uint64_t liov, uint64_t liovcnt,
                          uint64_t riov, uint64_t riovcnt, uint64_t flags)
{
    if (flags) return LERR(EINVAL);
    if ((int)pid != lxrt_ids_pid()) return LERR(EPERM);
    if (liovcnt > 1024 || riovcnt > 1024) return LERR(EINVAL);
    struct iov64 { uint64_t base, len; };
    const struct iov64 *l = (const struct iov64 *)(uintptr_t)liov;
    const struct iov64 *r = (const struct iov64 *)(uintptr_t)riov;
    if ((liovcnt && !l) || (riovcnt && !r)) return LERR(EFAULT);
    uint64_t gb = lxrt_gbase();
#define PVM_ADDR(a) ((gb && (a) && (a) < (1ull << 32)) ? (a) + gb : (a))
    uint64_t total = 0, li = 0, loff = 0;
    for (uint64_t ri = 0; ri < riovcnt && li < liovcnt; ri++) {
        uint64_t rbase = PVM_ADDR(r[ri].base), rlen = r[ri].len, roff = 0;
        while (roff < rlen && li < liovcnt) {
            uint64_t lbase = PVM_ADDR(l[li].base), llen = l[li].len;
            if (loff >= llen) { li++; loff = 0; continue; }
            uint64_t n = rlen - roff < llen - loff ? rlen - roff : llen - loff;
            mach_vm_size_t got = 0;
            kern_return_t kr = write
                ? mach_vm_read_overwrite(mach_task_self(), lbase + loff, n, rbase + roff, &got)
                : mach_vm_read_overwrite(mach_task_self(), rbase + roff, n, lbase + loff, &got);
            if (kr != KERN_SUCCESS || got != n)
                // Linux transfers whole remote elements or none of one: stop here.
                return total ? (long)total : LERR(EFAULT);
            roff += n;
            loff += n;
            total += n;
        }
    }
#undef PVM_ADDR
    return (long)total;
}

// sendfile (71) and splice (76): the kernel copies between two descriptors.
// Darwin's sendfile only sends a file to a socket and it has no splice, so
// both are a read/write loop here, with the offsets as Linux keeps them: a
// given offset is read (or written) with pread/pwrite and advanced, the
// descriptor's own offset left alone. Android's FileUtils.copy picks
// sendfile for file to file and splice when a pipe is involved; `pm install`
// failed with "sendfile failed: ENOSYS" (PackageInstallerSession writing the
// APK into its staging file, MEASURED at stage 28). Neither blocks longer
// than a read or write would; bytes read but not written (a non-blocking
// output that filled) are given back with lseek where the input can seek.
static long copy_fds(int in, int64_t *off_in, int out, int64_t *off_out, uint64_t count)
{
    if (off_in && *off_in < 0) return LERR(EINVAL);
    if (off_out && *off_out < 0) return LERR(EINVAL);
    size_t cap = count < (1u << 20) ? (size_t)count : (1u << 20);
    if (!cap) return 0;
    char *buf = malloc(cap);
    if (!buf) return LERR(ENOMEM);
    uint64_t done = 0;
    long err = 0;
    while (done < count) {
        size_t want = count - done < cap ? (size_t)(count - done) : cap;
        ssize_t n = off_in ? pread(in, buf, want, (off_t)*off_in) : read(in, buf, want);
        if (n < 0) { err = LERR(errno); break; }
        if (n == 0) break;
        long seal = lxrt_memfd_check_write(out, off_out ? *off_out : -1, (uint64_t)n);
        if (seal < 0) { err = seal; break; }
        ssize_t w = 0;
        while (w < n) {
            ssize_t k = off_out ? pwrite(out, buf + w, (size_t)(n - w), (off_t)(*off_out + w))
                                : write(out, buf + w, (size_t)(n - w));
            if (k < 0) { if (errno == EINTR) continue; err = LERR(errno); break; }
            w += k;
        }
        if (off_in) *off_in += w;
        else if (w < n) lseek(in, -(off_t)(n - w), SEEK_CUR);
        if (off_out) *off_out += w;
        done += (uint64_t)w;
        if (w < n || (size_t)n < want) break;
    }
    free(buf);
    return done ? (long)done : err;
}

static long do_sendfile(int out, int in, uint64_t offp, uint64_t count)
{
    int64_t off = 0;
    if (offp) memcpy(&off, (const void *)(uintptr_t)offp, sizeof off);
    long r = copy_fds(in, offp ? &off : NULL, out, NULL, count);
    if (offp && r >= 0) memcpy((void *)(uintptr_t)offp, &off, sizeof off);
    return r;
}

static long do_splice(int in, uint64_t offinp, int out, uint64_t offoutp, uint64_t len, unsigned flags)
{
    struct stat si, so;
    if (fstat(in, &si) != 0 || fstat(out, &so) != 0) return LERR(EBADF);
    if (!S_ISFIFO(si.st_mode) && !S_ISFIFO(so.st_mode)) return LERR(EINVAL);   // one end must be a pipe
    if ((offinp && S_ISFIFO(si.st_mode)) || (offoutp && S_ISFIFO(so.st_mode))) return LERR(ESPIPE);
    (void)flags;                                // SPLICE_F_MOVE / MORE / NONBLOCK / GIFT: hints here
    int64_t oi = 0, oo = 0;
    if (offinp) memcpy(&oi, (const void *)(uintptr_t)offinp, sizeof oi);
    if (offoutp) memcpy(&oo, (const void *)(uintptr_t)offoutp, sizeof oo);
    long r = copy_fds(in, offinp ? &oi : NULL, out, offoutp ? &oo : NULL, len);
    if (r >= 0) {
        if (offinp) memcpy((void *)(uintptr_t)offinp, &oi, sizeof oi);
        if (offoutp) memcpy((void *)(uintptr_t)offoutp, &oo, sizeof oo);
    }
    return r;
}

static long do_nanosleep(uint64_t req, uint64_t rem);
static long do_nanosleep_abs(long clk, uint64_t req);

static long do_fstatat(int dirfd, const char *path, uint64_t out, int flags)
{
    if (!out || !path)
        return LERR(EFAULT);
    struct stat d;
    // Linux AT_EMPTY_PATH (0x1000) has no Darwin equivalent; an empty path
    // with it set means "stat the dirfd itself", which is fstat.
    if (lxrt_at_is_empty_path(flags) && path[0] == '\0')
        return do_fstat(lxrt_dirfd_to_darwin(dirfd), out);
    if (lxrt_binder_stat(path, &d)) {
        stat_to_linux(&d, (struct linux_stat *)out);
        return 0;
    }
    const char *mp = at_through_mounts(dirfd, path, !(flags & 0x100));
    if (mp)
        dirfd = -100;
    const char *hp = mp ? mp : (flags & 0x100) ? translate(path) : translate_follow(path);  // AT_SYMLINK_NOFOLLOW
    if (fstatat(lxrt_dirfd_to_darwin(dirfd), hp, &d,
                lxrt_at_flags_to_darwin(flags)) != 0) {
        int e = errno;
        if (!(flags & 0x100) && lxrt_magic_link_stat(lxrt_dirfd_to_darwin(dirfd), translate(path), &d)) {
            stat_to_linux(&d, (struct linux_stat *)out);
            return 0;
        }
        return LERR(e);
    }
    lxrt_evdev_fix_stat(hp, -1, &d);
    lxrt_props_fix_stat(hp, -1, &d);
    lxrt_aids_fix_stat_at(lxrt_dirfd_to_darwin(dirfd), hp, (flags & 0x100) != 0, &d);
    stat_to_linux(&d, (struct linux_stat *)out);
    return 0;
}

// ---------------------------------------------------------------- rlimit

// Linux and Darwin agree on RLIMIT_CPU/FSIZE/DATA/STACK/CORE and diverge after
// that, so the mapping is explicit rather than a pass-through.

// The highest soft RLIMIT_NOFILE Darwin accepts: kern.maxfilesperproc (61440
// here), or the hard limit when that is lower.
rlim_t lxrt_nofile_ceiling(void)
{
    int perproc = 0;
    size_t sz = sizeof perproc;
    rlim_t ceil = sysctlbyname("kern.maxfilesperproc", &perproc, &sz, NULL, 0) == 0 && perproc > 0
                      ? (rlim_t)perproc : 10240;
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_max != RLIM_INFINITY && rl.rlim_max < ceil)
        ceil = rl.rlim_max;
    return ceil;
}

// Every process the launcher starts through Launch Services (the X server,
// and since Game Mode Steam and its games through it) gets launchd's soft
// open-files limit, 256: Steam's client died with 28679 "shared memfd open()
// failed: Too many open files" and the runtime faults after them (MEASURED
// 2026-10-08, after a restart reset launchctl's maxfiles). Linux gives a
// process 1024 and Steam raises it further; the runtime starts every guest
// at the ceiling instead.
void lxrt_raise_nofile(void)
{
    struct rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) != 0)
        return;
    rlim_t want = lxrt_nofile_ceiling();
    if (rl.rlim_cur < want) {
        rl.rlim_cur = want;
        setrlimit(RLIMIT_NOFILE, &rl);
    }
}

static int rlimit_to_darwin(long linux_res)
{
    switch (linux_res) {
    case 0: return RLIMIT_CPU;
    case 1: return RLIMIT_FSIZE;
    case 2: return RLIMIT_DATA;
    case 3: return RLIMIT_STACK;
    case 4: return RLIMIT_CORE;
    case 5: return RLIMIT_AS;       // Linux RLIMIT_RSS, closest Darwin has
    case 6: return RLIMIT_NPROC;
    case 7: return RLIMIT_NOFILE;
    case 8: return RLIMIT_MEMLOCK;
    case 9: return RLIMIT_AS;
    default: return -1;
    }
}

struct linux_rlimit { uint64_t rlim_cur, rlim_max; };

static long do_prlimit64(long pid, long res, uint64_t newp, uint64_t oldp)
{
    if (pid != 0 && pid != lxrt_ids_pid())
        return LERR(EPERM);  // cross-process limits are not something we can honour
    int dres = rlimit_to_darwin(res);
    if (dres < 0)
        return LERR(EINVAL);

    struct rlimit cur;
    if (getrlimit(dres, &cur) != 0)
        return LERR(errno);
    // ...and a buffer in a 4 KiB guest page the guest made read-only inside a
    // host page that stays writable (subpage.c keeps the guest's protection):
    // EFAULT too. Chromium's WebView renderer (under FEX, 4 KiB pages)
    // checked its protected section so and died when the write went through
    // (MEASURED).
    if (oldp) {
        for (uint64_t pg = LXRT_ALIGN_DOWN(oldp, 4096); pg < oldp + sizeof(struct rlimit); pg += 4096) {
            int sp = lxrt_subpage_prot_at(pg);
            if (sp >= 0 && !(sp & PROT_WRITE))
                return LERR(EFAULT);
        }
    }
    // The old limit is written by Darwin's getrlimit straight into the
    // guest's buffer (Darwin's struct rlimit is two 64-bit words, as Linux's
    // struct rlimit64), so a buffer the guest cannot write gives EFAULT, as
    // Linux's copy_to_user does, instead of a fault inside the runtime.
    // Chromium relies on it: base::ProtectedMemory checks that its section
    // is read-only with prlimit64(0, RLIMIT_NPROC, NULL, p) == -EFAULT, and
    // Electron's main process died at start on the runtime's own store
    // (MEASURED, benchmarks/stage24-heroic.txt). The first getrlimit above
    // only checks the resource; the values are the same.
    // The new limit is read before the old one is written: the two may be
    // the same buffer.
    struct rlimit set = { 0, 0 };
    if (newp) {
        const struct linux_rlimit *n = (const struct linux_rlimit *)newp;
        set.rlim_cur = n->rlim_cur >= (uint64_t)RLIM_INFINITY ? RLIM_INFINITY : (rlim_t)n->rlim_cur;
        set.rlim_max = n->rlim_max >= (uint64_t)RLIM_INFINITY ? RLIM_INFINITY : (rlim_t)n->rlim_max;
    }
    bool old_fault = false;
    if (oldp && getrlimit(dres, (struct rlimit *)oldp) != 0) {
        if (errno != EFAULT)
            return LERR(errno);
        old_fault = true;
    }
    // "No limit" is a different number on the two systems: Darwin's
    // RLIM_INFINITY is INT64_MAX, Linux's is UINT64_MAX (see proc_ext.c,
    // rsslim). Passed through, Darwin's reads as a finite limit of 2^63-1:
    // bionic's fdsan sizes its overflow table from RLIMIT_NOFILE's rlim_max
    // unless it is RLIM_INFINITY, the size wrapped to 0 and every Android
    // program that used an fd above 127 aborted in "fdsan: mmap failed:
    // Invalid argument" (ART, MEASURED, benchmarks/stage25-art-x86-fex.txt).
    // The buffer is writable here (getrlimit just wrote it).
    if (oldp && !old_fault) {
        struct linux_rlimit *o = (struct linux_rlimit *)oldp;
        if (o->rlim_cur == (uint64_t)RLIM_INFINITY) o->rlim_cur = ~0ull;
        if (o->rlim_max == (uint64_t)RLIM_INFINITY) o->rlim_max = ~0ull;
    }
    if (newp && setrlimit(dres, &set) != 0) {
        // Darwin can refuse a soft open-files limit above
        // kern.maxfilesperproc (EINVAL) where Linux grants anything up to
        // the hard limit: raising the soft limit to the hard one
        // ("unlimited" here) is what Chromium and Proton do, and it failed
        // and left them at launchd's 256. The highest limit Darwin takes
        // instead.
        rlim_t ceil = dres == RLIMIT_NOFILE ? lxrt_nofile_ceiling() : 0;
        if (errno != EINVAL || !ceil || set.rlim_cur <= ceil || set.rlim_cur > set.rlim_max)
            return LERR(errno);
        set.rlim_cur = ceil;
        if (setrlimit(dres, &set) != 0)
            return LERR(errno);
    }
    // Linux sets the new limit first and reports the old one's EFAULT after.
    return old_fault ? LERR(EFAULT) : 0;
}

// ---------------------------------------------------------------- uname

static long do_uname(uint64_t buf)
{
    // Linux `struct utsname` is six fixed 65-byte fields.
    if (!buf)
        return LERR(EFAULT);
    char *u = (char *)buf;
    memset(u, 0, 6 * 65);
    strcpy(u + 0 * 65, "Linux");
    strcpy(u + 1 * 65, "lxrt");
    strcpy(u + 2 * 65, "6.6.0-lxrt");
    strcpy(u + 3 * 65, "#1 SMP lxrt");
    strcpy(u + 4 * 65, "aarch64");
    strcpy(u + 5 * 65, "(none)");
    return 0;
}

// ---------------------------------------------------------------- dispatch

static _Thread_local uint64_t g_guest_lr;
static _Thread_local uint64_t g_robust_head, g_robust_len;
uint64_t lxrt_last_guest_lr(void) { return g_guest_lr; }
static _Thread_local uint64_t g_guest_x28;
uint64_t lxrt_last_guest_x28(void) { return g_guest_x28; }   // FEX's CPU state, for diagnostics
// A process sending itself SIGABRT is almost always glibc's abort() in the
// guest's own (aarch64) code -- in FEX, that is the emulator's heap, which no
// guest-level handler sees ("corrupted double-linked list" from wineserver's
// FEX, stage 16). The frame-pointer chain, as offsets into the main image
// (FEX), is enough to symbolise it offline.
extern uint64_t lxrt_main_image_base, lxrt_main_image_span;
static void abort_backtrace(const struct lxrt_regs *r)
{
    uint64_t ib = lxrt_main_image_base, ie = ib + lxrt_main_image_span;
    extern char ***_NSGetArgv(void);
    extern int *_NSGetArgc(void);
    char **av = *_NSGetArgv();
    int ac = *_NSGetArgc();
    fprintf(stderr, "[lxrt] pid %d (parent %d, %s %s): SIGABRT to itself; main image 0x%llx, frames (image offsets):",
            (int)getpid(), (int)getppid(), ac > 2 ? av[2] : "?", ac > 3 ? av[3] : "",
            (unsigned long long)ib);
    uint64_t fp = r->x[29], lr = r->x[30];
    for (int i = 0; i < 32; i++) {
        if (lr >= ib && lr < ie)
            fprintf(stderr, " +0x%llx", (unsigned long long)(lr - ib));
        else
            fprintf(stderr, " 0x%llx", (unsigned long long)lr);
        uint64_t fr[2];
        mach_vm_size_t got = 0;
        if (!fp || fp & 7 ||
            mach_vm_read_overwrite(mach_task_self(), fp, sizeof fr, (mach_vm_address_t)(uintptr_t)fr, &got) != KERN_SUCCESS)
            break;
        if (fr[0] <= fp)
            break;
        fp = fr[0];
        lr = fr[1];
    }
    // glibc has no frame pointers: also every word on the stack above the
    // saved registers that points into the image (a heuristic backtrace).
    fprintf(stderr, "\n[lxrt]   stack words into the image:");
    const uint64_t *sp = (const uint64_t *)(r + 1);
    for (int i = 0, shown = 0; i < 4096 && shown < 48; i++) {
        uint64_t w;
        mach_vm_size_t got = 0;
        if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)(uintptr_t)(sp + i), sizeof w,
                                   (mach_vm_address_t)(uintptr_t)&w, &got) != KERN_SUCCESS)
            break;
        if (w >= ib && w < ie) {
            fprintf(stderr, " +0x%llx", (unsigned long long)(w - ib));
            shown++;
        }
    }
    fprintf(stderr, "\n");
}

// membarrier(2). MEMBARRIER_CMD_QUERY answered 0 before -- "no command
// supported" -- and every other command 0 as well, so Wine ARM64 logged
// "membarrier not supported for NtFlushProcessWriteBuffers" and flushed
// other threads' write buffers by interrupting each one with SIGUSR2 instead:
// a signal in the middle of FEX's ARM64EC JIT code, whose context went
// through NtGetContextThread and back. Oodle's Huffman decoder came out of
// it with RAX, RDI and XMM2 replaced by host values, deterministically
// (MEASURED: FINAL FANTASY VII REMAKE and a probe of the game's
// oo2core_7_win64.dll, 2026-10-08). The barrier itself is the one .NET uses
// on macOS: thread_get_register_pointer_values on every thread of the task
// makes each one stop and come back through an exception return, which
// drains its write buffer and is context-synchronising (the SYNC_CORE
// variants too).
#include <mach/thread_act.h>
static _Atomic unsigned g_membarrier_registered;
static void process_wide_barrier(void)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    thread_act_array_t th = NULL;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &th, &n) != KERN_SUCCESS)
        return;
    thread_act_t self = mach_thread_self();
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        if (th[i] != self) {
            uintptr_t sp = 0, regs[128];
            size_t cnt = 128;
            (void)thread_get_register_pointer_values(th[i], &sp, &cnt, regs);
        }
        mach_port_deallocate(mach_task_self(), th[i]);
    }
    mach_port_deallocate(mach_task_self(), self);
    vm_deallocate(mach_task_self(), (vm_address_t)th, n * sizeof *th);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

long lxrt_membarrier(int cmd, unsigned flags)
{
    enum { QUERY = 0, GLOBAL = 1, PRIV = 8, REG_PRIV = 16, PRIV_SYNC = 32, REG_PRIV_SYNC = 64 };
    if (flags)
        return LERR(EINVAL);
    switch (cmd) {
    case QUERY:
        return GLOBAL | PRIV | REG_PRIV | PRIV_SYNC | REG_PRIV_SYNC;
    case GLOBAL:
        process_wide_barrier();        // every process: ours is what we can reach
        return 0;
    case REG_PRIV:
    case REG_PRIV_SYNC:
        atomic_fetch_or(&g_membarrier_registered, (unsigned)cmd);
        return 0;
    case PRIV:
    case PRIV_SYNC:
        // As Linux: a process must register for the expedited command first.
        if (!(atomic_load(&g_membarrier_registered) & (unsigned)(cmd << 1)))
            return LERR(EPERM);
        process_wide_barrier();
        return 0;
    default:
        return LERR(EINVAL);
    }
}

void lxrt_dispatch(struct lxrt_regs *r)
{
    long nr = (long)r->x[8];
    g_guest_lr = r->x[30];
    g_guest_x28 = r->x[28];
    if (__builtin_expect(lxrt_guestprof_on, 0))
        lxrt_guestprof_note(r->x[28]);
    // A 32-bit guest's pointer arguments arrive as guest addresses; the
    // kernel is us, so we add the base (runtime/gbase.c).
    // On a COPY: the argument registers are the caller's live registers, and
    // FEX issues syscalls inline from translated code with the guest's own
    // registers (EBX, ECX, ...) in x0-x5. Translating in place handed the
    // guest back host pointers in those registers, which it then truncated
    // to 32 bits (measured: the i386 Steam client wrote to low32(host) - 8
    // and faulted forever).
    uint64_t av[6] = { r->x[0], r->x[1], r->x[2], r->x[3], r->x[4], r->x[5] };
    lxrt_gbase_apply(nr, av);
    uint64_t a0 = av[0], a1 = av[1], a2 = av[2];
    uint64_t a3 = av[3], a4 = av[4], a5 = av[5];
    long ret;

    // Entry trace. Without it a syscall that never returns leaves no record at
    // all, and "the last line is the one before the problem" is a guess.
    if (g_trace && nr != LNR_clock_gettime)
        fprintf(lxrt_trace_stream(), "[lxrt] %d/%d %.3f -> %ld(0x%llx, 0x%llx, 0x%llx)\n", (int)getpid(),
                lxrt_gettid(), (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9, nr,
                (unsigned long long)a0, (unsigned long long)a1,
                (unsigned long long)a2);

    extern _Thread_local int lxrt_sig_during_syscall;
    // A guest handler runs on top of the call it interrupted, and its own
    // syscalls come through here: the mark belongs to the interrupted call
    // and is put back on the way out. Cleared, it read as "no handler ran",
    // and the interrupted call restarted instead of reporting EINTR
    // (tests/elf binder_ipc.c's pool test: a handler that closes a
    // descriptor during a blocked BINDER_WRITE_READ).
    const int outer_sig_during_syscall = lxrt_sig_during_syscall;
    // Android ids (runtime/android_ids.h): the credential, capability and
    // namespace calls of a guest started with LXRT_ANDROID_IDS.
    if (lxrt_aids_on() && lxrt_aids_syscall(nr, a0, a1, a2, a3, a4, &ret))
        goto aids_done;
restart:
    lxrt_sig_during_syscall = 0;
    switch (nr) {
    case LNR_lxrt_dlopen: {
        if (!a0) { ret = LERR(EFAULT); break; }
        // RTLD_NOW|RTLD_LOCAL regardless of what the guest asked: the flag
        // values differ between the two systems and lazy binding across the
        // boundary would resolve at an unpredictable moment.
        void *h = dlopen((const char *)a0, RTLD_NOW | RTLD_LOCAL);
        if (!h && g_trace)
            fprintf(lxrt_trace_stream(), "[lxrt] dlopen(%s): %s\n", (const char *)a0, dlerror());
        ret = (long)(uintptr_t)h;
        break;
    }
    case LNR_lxrt_dlsym: {
        // dyld dereferences the name without checking; a null from a guest
        // would fault inside the host rather than return an error.
        if (!a1) { ret = LERR(EFAULT); break; }
        void *f = dlsym((void *)a0, (const char *)a1);
        if (!f && g_trace)
            fprintf(lxrt_trace_stream(), "[lxrt] dlsym(%s): %s\n", (const char *)a1, dlerror());
        ret = (long)(uintptr_t)f;
        break;
    }
    case LNR_lxrt_dlerror: {
        const char *e = dlerror();
        if (!a0 || !a1) { ret = 0; break; }
        size_t n = e ? strlen(e) : 0;
        if (n >= a1) n = (size_t)a1 - 1;
        memcpy((void *)a0, e ? e : "", n);
        ((char *)a0)[n] = 0;
        ret = (long)n;
        break;
    }
    case LNR_lxrt_window:
        // A Vulkan surface on Metal is a CAMetalLayer. The runtime owns the
        // window; the guest gets the layer pointer and hands it straight to
        // vkCreateMetalSurfaceEXT. No framebuffer ever crosses a boundary.
        ret = (long)(uintptr_t)lxrt_window_create((int)a0, (int)a1,
                                                  (const char *)a2);
        break;
    case LNR_lxrt_rlayer_create:
        ret = (long)(uintptr_t)lxrt_remote_layer_create((uint32_t)a0, (uint32_t)a1, (uint32_t *)a2);
        break;
    case LNR_lxrt_rlayer_resize:
        ret = lxrt_remote_layer_resize((void *)a0, (uint32_t)a1, (uint32_t)a2);
        break;
    case LNR_lxrt_rlayer_release:
        lxrt_remote_layer_release((void *)a0);
        ret = 0;
        break;
    case LNR_lxrt_mfx_encode:
        ret = lxrt_mfx_encode((struct lxrt_mfx_run *)a0);
        break;
    case LNR_lxrt_mfx_release:
        lxrt_mfx_release((void *)a0);
        ret = 0;
        break;
    case LNR_lxrt_metal_attach:
        ret = lxrt_metal_attach();
        break;
    case LNR_lxrt_drawable:
        lxrt_window_drawable_size((uint32_t *)a0, (uint32_t *)a1);
        ret = 0;
        break;
    case LNR_lxrt_guest_base:
        lxrt_gbase_set(a0);
        ret = 0;
        break;
    case LNR_lxrt_guest_base_get:
        ret = (long)lxrt_gbase();
        break;
    case LNR_lxrt_alias: {
        // The same pages at a second address, shared (not a copy): memory a
        // host library mapped (MoltenVK's vkMapMemory) made visible inside a
        // 32-bit guest's window. Host pages only; dst is replaced.
        uint64_t src = a0, len = a1, dst = a2;
        if (!len || src % LXRT_HOST_PAGE || dst % LXRT_HOST_PAGE || len % LXRT_HOST_PAGE) {
            ret = LERR(EINVAL);
            break;
        }
        lxrt_subpage_forget(dst, len);
        lxrt_privmap_forget(dst, len);
        mach_vm_address_t at = dst;
        vm_prot_t cur = 0, max = 0;
        kern_return_t kr = mach_vm_remap(mach_task_self(), &at, len, 0,
                                         VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), src,
                                         FALSE, &cur, &max, VM_INHERIT_SHARE);
        ret = kr == KERN_SUCCESS && at == dst ? 0 : LERR(ENOMEM);
        break;
    }
    case LNR_lxrt_jit_wx:
        // A JIT asking for the W^X flip on its own stack. Doing it from the
        // fault handler instead does not survive the handler's return
        // (benchmarks/stage5-jit.txt), which is why this syscall exists.
        ret = lxrt_jit_set_write((int)a0, a1, a2);
        break;
    case LNR_clock_gettime:
        ret = do_clock_gettime((long)a0, a1);
        break;
    case LNR_write:
        // An eventfd is a counter, not a stream: a write adds to it. The guest
        // reaches it through the ordinary write syscall, so the routing has to
        // happen here rather than inside the descriptor.
        if (lxrt_eventfd_is((int)a0)) {
            ret = lxrt_eventfd_write((int)a0, (const void *)a1, (size_t)a2);
            break;
        }
        if (lxrt_evdev_is((int)a0)) {
            ret = lxrt_evdev_write((int)a0, (const void *)a1, (size_t)a2);
            break;
        }
        if (lxrt_binder_is((int)a0)) {       // binder has no write(): EINVAL
            ret = LERR(EINVAL);
            break;
        }
        {
            long seal = lxrt_memfd_check_write((int)a0, -1, a2);
            if (seal < 0) { ret = seal; break; }
        }
        ret = ret_of((long)write((int)a0, (const void *)a1, (size_t)a2));
        break;
    case LNR_read:
        // Synthetic descriptors first: each carries its own record format and
        // a raw host read() on its backing fd would hand the guest garbage.
        if (lxrt_eventfd_is((int)a0)) {
            ret = lxrt_eventfd_read((int)a0, (void *)a1, (size_t)a2);
            break;
        }
        if (lxrt_evdev_is((int)a0)) {
            ret = lxrt_evdev_read((int)a0, (void *)a1, (size_t)a2);
            break;
        }
        if (lxrt_binder_is((int)a0)) {       // nor read(): the socket's bytes are doorbells
            ret = LERR(EINVAL);
            break;
        }
        if (lxrt_timerfd_is((int)a0)) {
            ret = lxrt_timerfd_read((int)a0, (void *)a1, (size_t)a2);
            break;
        }
        if (lxrt_signalfd_is((int)a0)) {
            ret = lxrt_signalfd_read((int)a0, (void *)a1, (size_t)a2);
            break;
        }
        if (lxrt_inotify_is((int)a0)) {
            ret = lxrt_inotify_read((int)a0, (void *)a1, (size_t)a2);
            break;
        }
        {
            struct iovec one = { (void *)a1, (size_t)a2 };
            if (lxrt_seqpkt_readv((int)a0, &one, 1, &ret))
                break;
        }
        ret = ret_of((long)read((int)a0, (void *)a1, (size_t)a2));
        break;
    case 65:                                            // readv
    case 69:                                            // preadv
    case 286: {                                         // preadv2
        // All three were ENOSYS. struct iovec has one layout on both systems.
        // preadv2's offset -1 is "the file position", as readv; its flags
        // are hints Darwin has no use for, except RWF_NOWAIT, which it
        // cannot honour (EOPNOTSUPP, as Linux answers where it cannot).
        const struct iovec *iov = (const struct iovec *)a1;
        int cnt = (int)a2;
        if (cnt < 0 || cnt > 1024) { ret = LERR(EINVAL); break; }
        if (nr == 286 && (a5 & ~0x7ull)) { ret = LERR(EOPNOTSUPP); break; }
        bool positional = nr == 69 || (nr == 286 && (int64_t)a3 != -1);
        if (positional && (int64_t)a3 < 0) { ret = LERR(EINVAL); break; }
        if (!positional) {
            // A descriptor the runtime synthesizes has its own record
            // format: read it through read()'s path, into the first buffer.
            int fd = (int)a0;
            if (lxrt_eventfd_is(fd) || lxrt_evdev_is(fd) || lxrt_timerfd_is(fd) ||
                lxrt_signalfd_is(fd) || lxrt_inotify_is(fd) || lxrt_binder_is(fd)) {
                int k = 0;
                while (k < cnt && iov[k].iov_len == 0) k++;
                if (k == cnt) { ret = 0; break; }
                ret = lxrt_eventfd_is(fd)   ? lxrt_eventfd_read(fd, iov[k].iov_base, iov[k].iov_len)
                    : lxrt_evdev_is(fd)     ? lxrt_evdev_read(fd, iov[k].iov_base, iov[k].iov_len)
                    : lxrt_timerfd_is(fd)   ? lxrt_timerfd_read(fd, iov[k].iov_base, iov[k].iov_len)
                    : lxrt_signalfd_is(fd)  ? lxrt_signalfd_read(fd, iov[k].iov_base, iov[k].iov_len)
                    : lxrt_inotify_is(fd)   ? lxrt_inotify_read(fd, iov[k].iov_base, iov[k].iov_len)
                    : LERR(EINVAL);
                break;
            }
            if (lxrt_seqpkt_readv(fd, iov, cnt, &ret))
                break;
            ret = ret_of((long)readv(fd, iov, cnt));
        } else {
            ret = ret_of((long)preadv((int)a0, iov, cnt, (off_t)a3));
        }
        break;
    }
    case 70:                                            // pwritev
    case 287: {                                         // pwritev2
        const struct iovec *iov = (const struct iovec *)a1;
        int cnt = (int)a2;
        if (cnt < 0 || cnt > 1024) { ret = LERR(EINVAL); break; }
        // RWF_HIPRI 1, RWF_DSYNC 2, RWF_SYNC 4; RWF_NOWAIT and RWF_APPEND
        // are not offered.
        if (nr == 287 && (a5 & ~0x7ull)) { ret = LERR(EOPNOTSUPP); break; }
        bool positional = nr == 70 || (int64_t)a3 != -1;
        if (positional && (int64_t)a3 < 0) { ret = LERR(EINVAL); break; }
        uint64_t total = 0;
        for (int i = 0; iov && i < cnt; i++) total += iov[i].iov_len;
        long seal = lxrt_memfd_check_write((int)a0, positional ? (int64_t)a3 : -1, total);
        if (seal < 0) { ret = seal; break; }
        ret = ret_of(positional ? (long)pwritev((int)a0, iov, cnt, (off_t)a3)
                                : (long)writev((int)a0, iov, cnt));
        if (ret >= 0 && nr == 287 && (a5 & 0x6) && fsync((int)a0) != 0)
            ret = LERR(errno);          // RWF_DSYNC / RWF_SYNC: this write, durable
        break;
    }
    case LNR_writev: {
        // struct iovec has the same layout on both systems. F_SEAL_WRITE has
        // to stop this path too, so the gate sums the vector first.
        uint64_t total = 0;
        const struct iovec *iov = (const struct iovec *)a1;
        for (int i = 0; iov && i < (int)a2; i++) total += iov[i].iov_len;
        long seal = lxrt_memfd_check_write((int)a0, -1, total);
        if (seal < 0) { ret = seal; break; }
        ret = ret_of((long)writev((int)a0, iov, (int)a2));
        break;
    }
    case LNR_close:
        ret = guest_close((int)a0);
        break;
    case LNR_getdents64:
        ret = lxrt_getdents64((int)a0, (void *)a1, (unsigned)a2);
        break;
    case LNR_lseek:
        ret = ret_of((long)lseek((int)a0, (off_t)a1, (int)a2));
        break;
    case LNR_openat:
        // /dev/ntsync: Wine's NT synchronization objects (ntsync.c), offered
        // only with LXRT_NTSYNC=1.
        if (lxrt_ntsync_path((int)a0 == -100 ? -1 : lxrt_dirfd_to_darwin((int)a0), (const char *)a1)) {
            ret = lxrt_ntsync_open((int)a2);
            break;
        }
        // /dev/binder, /dev/hwbinder, /dev/vndbinder: the userspace binder
        // driver (binder.c, binder_hub.c), whatever the root has under /dev.
        {
            int bctx = a1 ? lxrt_binder_context_of((const char *)a1) : -1;
            if (bctx >= 0) {
                ret = lxrt_binder_open(bctx, (int)a2);
                break;
            }
        }
        // A read-only bind (fake bwrap) refuses every write, as the mount
        // itself would on Linux.
        if (lxrt_mounts_readonly((const char *)a1) &&
            (((int)a2 & 3) != 0 || ((int)a2 & (0x40 | 0x200)))) {   // O_WRONLY/O_RDWR, O_CREAT, O_TRUNC
            ret = LERR(EROFS);
            break;
        }
        if (g_trace)
            fprintf(lxrt_trace_stream(), "[lxrt]    openat \"%s\"\n",
                    a1 ? (const char *)a1 : "(null)");
        // AT_FDCWD is -100 on Linux and -2 on Darwin.
        // Flag values differ; see fsflags.c. Passing Linux O_APPEND straight
        // through would ask Darwin for O_TRUNC|O_EXCL.
        {
            const char *mp = at_through_mounts((int)a0, (const char *)a1, !((int)a2 & 0x8000));
            int ddir = mp ? AT_FDCWD : lxrt_dirfd_to_darwin((int)a0);
            const char *hp = mp ? mp : ((int)a2 & 0x8000) ? translate((const char *)a1)
                                                          : translate_follow((const char *)a1);
            int dfl = lxrt_open_flags_to_darwin((int)a2);
            int fd = openat(ddir, hp, dfl, (int)a3);
            // O_PATH of a socket: Darwin refuses to open one (pathfd.c).
            // /dev/input/eventN is a socket too: connect to it (evdev.c).
            if (fd < 0 && errno == EOPNOTSUPP && ((int)a2 & 0x200000))
                fd = lxrt_pathfd_open(ddir, hp, (dfl & O_CLOEXEC) != 0);
            else if (fd < 0 && errno == EOPNOTSUPP && hp[0] == '/')
                fd = lxrt_evdev_open(hp, (int)a2);
            if (fd >= 0 && hp[0] == '/' && hp[1] == 'd')
                lxrt_pty_slave_opened(hp);      // a held slave can go (ioctl_tty.c)
            ret = ret_of((long)fd);
        }
        {
            // LXRT_LOG_OPEN_FAIL=<text>: report failed opens of paths
            // containing <text>, with the host path tried (debugging aid for
            // intermittent loader failures).
            static const char *watch = (const char *)1;
            if (watch == (const char *)1) watch = getenv("LXRT_LOG_OPEN_FAIL");
            if (watch && ret < 0 && a1 && strstr((const char *)a1, watch))
                fprintf(lxrt_trace_stream(), "[lxrt] open failed: dirfd %d \"%s\" -> host \"%s\" flags 0x%lx: %ld\n",
                        (int)a0, (const char *)a1, translate_follow((const char *)a1), (long)a2, ret);
        }
        break;
    case LNR_faccessat:
        // Linux passes three arguments here; faccessat2 (439) is the one that
        // carries flags.
        if (lxrt_binder_context_of((const char *)a1) >= 0) {
            // The binder devices exist (binder.c) whatever the root holds:
            // libbinder's initWithDriver falls back to /dev/binder when
            // access() fails, which would put vndservicemanager on the wrong
            // context. crw-rw-rw-: execute is refused.
            ret = ((int)a2 & 1) ? LERR(EACCES) : 0;
            break;
        }
        {
            const char *mp = at_through_mounts((int)a0, (const char *)a1, true);
            ret = ret_of(faccessat(mp ? AT_FDCWD : lxrt_dirfd_to_darwin((int)a0),
                                   mp ? mp : translate_follow((const char *)a1), (int)a2, 0));
        }
        break;
    case LNR_readlinkat: {
        char tmp[2048];
        ssize_t n;
        char self[MAXPATHLEN];
        const char *looked = NULL;          // the host path whose link is read
        if (a1 && ((const char *)a1)[0] == '\0') {
            // Linux: an empty path reads the link the descriptor itself names
            // (an O_PATH|O_NOFOLLOW fd, opened here with O_SYMLINK).
            n = fcntl((int)a0, F_GETPATH, self) == 0 ? readlink(self, tmp, sizeof tmp) : -1;
            if (n < 0 && errno == EINVAL) errno = ENOENT;
            if (n >= 0) looked = self;
        } else {
            const char *mp = at_through_mounts((int)a0, (const char *)a1, false);
            looked = mp ? mp : translate((const char *)a1);
            n = readlinkat(mp ? AT_FDCWD : lxrt_dirfd_to_darwin((int)a0), looked, tmp, sizeof tmp);
        }
        if (g_trace)
            fprintf(lxrt_trace_stream(), "[lxrt]    readlinkat %d \"%s\" -> %zd%s%.*s\n", (int)a0,
                    (const char *)a1, n, n > 0 ? " " : "", n > 0 ? (int)n : 0, tmp);
        if (n < 0) { ret = LERR(errno); break; }
        {
            // /proc/self/exe: the link points at the image's host path (open
            // must reach the file); its reader gets the guest's name for it
            // (main.c: absolute, in guest terms).
            char ef[1100];
            const char *exe = lxrt_proc_exe_path(), *file = lxrt_proc_exe_link_file(ef, sizeof ef);
            if (exe && file && looked && looked[0] == '/' && !strcmp(looked, file)) {
                size_t el = strlen(exe);
                if (el > (size_t)a3)
                    el = (size_t)a3;
                memcpy((char *)a2, exe, el);
                ret = (long)el;
                break;
            }
        }
        // Undo symlinkat's rewrite: a target inside the guest root is shown
        // as the guest path it was created from.
        const char *out = tmp;
        const char *root = guest_root();
        size_t rl = root ? strlen(root) : 0;
        char gview[2048];
        tmp[n < (ssize_t)sizeof tmp ? n : (ssize_t)sizeof tmp - 1] = '\0';
        // Binds and the root in their canonical host form too: /proc/self/fd
        // links carry F_GETPATH's /private/tmp/..., which leaked host paths
        // into pressure-vessel's capsule-capture-libs ("private/tmp/...").
        const char *u = tmp[0] == '/' ? lxrt_proc_untranslate(tmp, gview, sizeof gview) : NULL;
        if (!u && tmp[0] == '/')
            u = lxrt_mounts_untranslate(tmp, gview, sizeof gview);
        if (!u && tmp[0] == '/' && lxrt_mounts_active()) {
            // A link made outside the sandbox under another spelling of the
            // same directory: steam.sh's ~/.steam/sdk64 names
            // /tmp/lxrt-steamroot/..., the binds carry its realpath
            // (~/SteamARM-roots/steamroot/...). MEASURED: Proton's
            // lsteamclient could not load sdk64/steamclient.so in the
            // container, and steam.exe stopped on an assertion.
            char ct[PATH_MAX];
            if (host_canon(tmp, ct, sizeof ct) && strcmp(ct, tmp))
                u = lxrt_mounts_untranslate(ct, gview, sizeof gview);
        }
        if (u) {
            out = u;
            n = (ssize_t)strlen(u);
        } else if (rl && (size_t)n > rl && strncmp(tmp, root, rl) == 0 && tmp[rl] == '/') {
            out = tmp + rl;
            n -= (ssize_t)rl;
        }
        if ((size_t)n > (size_t)a3)
            n = (ssize_t)a3;
        memcpy((char *)a2, out, (size_t)n);
        ret = n;
        break;
    }
    case LNR_mmap:
        if (!((long)a3 & LINUX_MAP_ANONYMOUS) && lxrt_binder_is((int)a4)) {
            ret = lxrt_binder_mmap(a0, a1, (int)a2, (int)a3, (int)a4, a5);
            break;
        }
        ret = do_mmap(a0, a1, (long)a2, (long)a3, (long)a4, (long)a5);
        if (ret >= 0 && ((long)a3 & LINUX_MAP_GROWSDOWN))
            lxrt_note_growsdown((uint64_t)ret, a1, (long)a2);
        lxrt_memlog('m', ret >= 0 && !a0 ? (uint64_t)ret : a0, a1, (long)a2, (long)a3, ret);
        if (ret >= 0 && !((long)a3 & 0x20))     // file-backed (not MAP_ANONYMOUS)
            lxrt_memlog_file((uint64_t)ret, a1, (uint64_t)a5, (int)a4);
        if (ret >= 0 && ((long)a3 & LINUX_MAP_SHARED))
            lxrt_futex_shared_add((uint64_t)ret, a1);
        break;
    case LNR_munmap:
        if (lxrt_pool_overlaps(a0, a0 + a1))
            fprintf(lxrt_trace_stream(), "[lxrt] WARNING: guest munmap 0x%llx+0x%llx covers a "
                            "trampoline pool\n", (unsigned long long)a0,
                    (unsigned long long)a1);
        if (lxrt_binder_munmap(a0, a1, &ret))
            break;
        ret = do_munmap(a0, a1);
        lxrt_memlog('u', a0, a1, 0, 0, ret);
        if (ret == 0)
            lxrt_futex_shared_remove(a0, a1);
        break;
    case 227: { // msync(addr, len, flags)
        // Unimplemented until stage 25: ART probes for free address space
        // page by page with msync(addr, 4096, 0), which Linux answers ENOMEM
        // for an unmapped page; ENOSYS read as "mapped" sent it through all
        // of the low 4 GiB one call at a time (benchmarks/stage25-android-
        // userspace.txt). Linux: addr aligned to its page (4 KiB here),
        // MS_ASYNC 1 | MS_INVALIDATE 2 | MS_SYNC 4, not ASYNC and SYNC
        // together; ENOMEM if any page of the range is unmapped.
        if ((a0 & 4095) || (a2 & ~7ull) || ((a2 & 1) && (a2 & 4))) { ret = LERR(EINVAL); break; }
        // FEX passes msync straight through (its allocator never sees it),
        // so a 32-bit guest's address arrives as the guest's: below 4 GiB it
        // is base + address, as for madvise (gbase.c's rule). i386 dex2oat's
        // msync(MS_SYNC) of its vdex failed with ENOMEM on __PAGEZERO and
        // the compile was thrown away ("Failed to Sync() dex2dex output").
        if (lxrt_gbase() && a0 && a0 < (1ull << 32))
            a0 += lxrt_gbase();
        uint64_t end = a0 + LXRT_ALIGN_UP(a1, 4096);
        if (end < a0) { ret = LERR(ENOMEM); break; }
        ret = 0;
        for (uint64_t at = a0; at < end;) {
            // The low 4 GiB is Darwin's __PAGEZERO: reserved, never free
            // (benchmarks/stage2-pagezero.txt). Linux answers 0 for a
            // reserved PROT_NONE range, and so does this; "free" made ART
            // place a 64 MiB mapping at each of a million hints there, each
            // landing above 4 GiB and unmapped again (stage 25).
            if (at < (1ull << 32)) {
                at = (1ull << 32);
                continue;
            }
            mach_vm_address_t ra = at;
            mach_vm_size_t rs = 0;
            vm_region_basic_info_data_64_t ri;
            mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj = MACH_PORT_NULL;
            if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                               (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS || ra > at) {
                ret = LERR(ENOMEM);
                break;
            }
            if (obj != MACH_PORT_NULL)
                mach_port_deallocate(mach_task_self(), obj);
            at = ra + rs;
        }
        // Writing back a file mapping is the only part with an effect here.
        if (ret == 0 && (a2 & 4) && a1) {
            uint64_t hs = LXRT_ALIGN_DOWN(a0, LXRT_HOST_PAGE);
            if (msync((void *)hs, (size_t)(LXRT_ALIGN_UP(end, LXRT_HOST_PAGE) - hs), MS_SYNC) != 0 &&
                errno != EINVAL)
                ret = LERR(errno);
        }
        break;
    }
    case LNR_mprotect:
        ret = do_mprotect(a0, a1, (long)a2);
        lxrt_memlog('p', a0, a1, (long)a2, 0, ret);
        break;
    case LNR_brk:
        ret = do_brk(a0);
        break;
    case LNR_capget:
    case LNR_capset: {
        // Capabilities: an unprivileged Linux process has none, and that is
        // exactly the truth here (no root, no namespaces). capget reports an
        // empty set; capset succeeds when it asks for nothing (dropping what
        // is already absent -- Chromium's zygote does this even with
        // --no-sandbox and CHECKs the result) and fails EPERM otherwise.
        // Header: {u32 version, i32 pid}; data: 1 (v1) or 2 (v2/v3) x
        // {effective, permitted, inheritable}.
        const uint32_t *hdr = (const uint32_t *)a0;
        if (!hdr) { ret = LERR(EFAULT); break; }
        uint32_t ver = hdr[0];
        int words = ver == 0x19980330 ? 1 : (ver == 0x20071026 || ver == 0x20080522) ? 2 : 0;
        if (!words) {
            ((uint32_t *)a0)[0] = 0x20080522;        // Linux reports its preferred version
            ret = a1 ? LERR(EINVAL) : 0;
            break;
        }
        if (nr == LNR_capget) {
            if (a1) memset((void *)a1, 0, (size_t)words * 12);
            ret = 0;
        } else {
            const uint32_t *d = (const uint32_t *)a1;
            bool any = false;
            for (int k = 0; d && k < words * 3; k++) any |= d[k] != 0;
            ret = !d ? LERR(EFAULT) : any ? LERR(EPERM) : 0;
        }
        break;
    }
    case LNR_uname:
        ret = do_uname(a0);
        break;
    case LNR_fstat:
        ret = do_fstat((int)a0, a1);
        break;
    case LNR_fstatat:
        ret = do_fstatat((int)a0, (const char *)a1, a2, (int)a3);
        if (g_trace)
            fprintf(lxrt_trace_stream(), "[lxrt]    fstatat %d \"%s\" flags 0x%x -> %ld\n",
                    (int)a0, a1 ? (const char *)a1 : "(null)", (int)a3, ret);
        break;
    case LNR_prlimit64:
        ret = do_prlimit64((long)a0, (long)a1, a2, a3);
        break;
    // The asm-generic getrlimit/setrlimit. aarch64 glibc and bionic use
    // prlimit64, but FEX passes an x86-64 guest's getrlimit (97) and
    // setrlimit (160) straight through as these: x86-64 bionic's
    // pthread_getattr_np() of the main thread calls getrlimit(RLIMIT_STACK),
    // and ART aborted on the ENOSYS ("pthread_getattr_np failed for
    // GetThreadStack", MEASURED, benchmarks/stage25-art-x86-fex.txt).
    case LNR_getrlimit:
        ret = do_prlimit64(0, (long)a0, 0, a1);
        break;
    case LNR_setrlimit:
        ret = do_prlimit64(0, (long)a0, a1, 0);
        break;
    case LNR_getrandom:
        if (!a0)
            ret = LERR(EFAULT);
        else {
            arc4random_buf((void *)a0, (size_t)a1);
            ret = (long)a1;
        }
        break;
    case LNR_set_robust_list:
        // Remembered per thread and handed back by get_robust_list: Steam's
        // shared-memory IPC checks that glibc registered one ("Fatal error:
        // futex robust_list not initialized by pthreads" in the webhelper).
        // NOT implemented: the kernel's walk of the list when a thread dies
        // holding a robust mutex (FUTEX_OWNER_DIED). Linux's struct is
        // 24 bytes (head, futex_offset, pending) -- EINVAL otherwise, as there.
        if (a1 != 24) { ret = LERR(EINVAL); break; }
        g_robust_head = a0;
        g_robust_len = a1;
        ret = 0;
        break;
    case LNR_get_robust_list: {
        // (pid, head_ptr*, len_ptr*). This thread's own list; another
        // thread's or process's is not tracked here.
        long self = lxrt_gettid();
        if (a0 != 0 && (long)a0 != self) { ret = LERR(EPERM); break; }
        if (!a1 || !a2) { ret = LERR(EFAULT); break; }
        *(uint64_t *)a1 = g_robust_head;
        *(uint64_t *)a2 = g_robust_len;
        ret = 0;
        break;
    }
    case LNR_nanosleep:
        ret = do_nanosleep(a0, a1);
        break;
    case LNR_clock_nanosleep:
        // (clockid, flags, req, rem). TIMER_ABSTIME is flag bit 0; the
        // relative case is the one glibc's sleep paths use.
        if ((long)a1 & 1)
            ret = do_nanosleep_abs((long)a0, a2);
        else
            ret = do_nanosleep(a2, a3);
        break;
    case LNR_sched_yield:
        ret = ret_of(sched_yield());
        break;
    case LNR_personality:
        // FEX sets ADDR_NO_RANDOMIZE so the guest's layout is reproducible.
        // Nothing here randomises anything, so the request is already
        // satisfied; Linux returns the previous personality.
        ret = 0;
        break;
    case 165: { // getrusage(who, struct rusage *)
        // Linux: two 16-byte timevals (long, long) then 14 longs; Darwin's
        // timeval carries a 32-bit tv_usec plus padding, and ru_maxrss is in
        // bytes where Linux counts KiB. RUSAGE_THREAD (1) has no Darwin
        // counterpart: this thread's times come from thread_info, the rest
        // from the process. (Unimplemented, bash's `time` printed garbage.)
        if (!a1) { ret = LERR(EFAULT); break; }
        long who = (long)a0;
        if (who != 0 && who != -1 && who != 1) { ret = LERR(EINVAL); break; }
        struct rusage ru;
        if (getrusage(who == -1 ? RUSAGE_CHILDREN : RUSAGE_SELF, &ru) != 0) { ret = LERR(errno); break; }
        int64_t *o = (int64_t *)a1;
        o[0] = ru.ru_utime.tv_sec; o[1] = ru.ru_utime.tv_usec;
        o[2] = ru.ru_stime.tv_sec; o[3] = ru.ru_stime.tv_usec;
        if (who == 1) {
            thread_basic_info_data_t ti;
            mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;
            mach_port_t th = mach_thread_self();
            if (thread_info(th, THREAD_BASIC_INFO, (thread_info_t)&ti, &cnt) == KERN_SUCCESS) {
                o[0] = ti.user_time.seconds; o[1] = ti.user_time.microseconds;
                o[2] = ti.system_time.seconds; o[3] = ti.system_time.microseconds;
            }
            mach_port_deallocate(mach_task_self(), th);
        }
        o[4] = ru.ru_maxrss / 1024;
        o[5] = ru.ru_ixrss; o[6] = ru.ru_idrss; o[7] = ru.ru_isrss;
        o[8] = ru.ru_minflt; o[9] = ru.ru_majflt; o[10] = ru.ru_nswap;
        o[11] = ru.ru_inblock; o[12] = ru.ru_oublock; o[13] = ru.ru_msgsnd;
        o[14] = ru.ru_msgrcv; o[15] = ru.ru_nsignals; o[16] = ru.ru_nvcsw;
        o[17] = ru.ru_nivcsw;
        ret = 0;
        break;
    }
    case 153: { // times(struct tms *): clock ticks at USER_HZ = 100
        struct rusage self, kids;
        getrusage(RUSAGE_SELF, &self);
        getrusage(RUSAGE_CHILDREN, &kids);
        #define TICKS(tv) ((int64_t)(tv).tv_sec * 100 + (int64_t)(tv).tv_usec / 10000)
        if (a0) {
            int64_t *t = (int64_t *)a0;
            t[0] = TICKS(self.ru_utime); t[1] = TICKS(self.ru_stime);
            t[2] = TICKS(kids.ru_utime); t[3] = TICKS(kids.ru_stime);
        }
        #undef TICKS
        ret = (long)(clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) / 10000000ull);
        break;
    }
    case LNR_getcpu:
        // (cpu, node, tcache). Reporting CPU 0 is allowed and is what a caller
        // that only wants a hint can act on.
        if (a0) *(uint32_t *)a0 = 0;
        if (a1) *(uint32_t *)a1 = 0;
        ret = 0;
        break;
    case LNR_getcwd: {
        // Linux returns the length INCLUDING the terminator; getcwd(3) returns
        // the buffer. A guest that reads the return value as a pointer is
        // reading a length, so this must not be passed through.
        if (!a0 || !a1) { ret = LERR(EFAULT); break; }
        char hostcwd[PATH_MAX], guestcwd[PATH_MAX];
        if (!getcwd(hostcwd, sizeof hostcwd)) { ret = LERR(errno); break; }
        // The cwd is a host path; the guest must see its own view of it
        // (a bind destination or a path inside its root).
        const char *g = lxrt_mounts_untranslate(hostcwd, guestcwd, sizeof guestcwd);
        if (!g) g = hostcwd;
        if (strlen(g) + 1 > (size_t)a1) { ret = LERR(ERANGE); break; }
        memcpy((void *)a0, g, strlen(g) + 1);
        ret = (long)strlen(g) + 1;
        break;
    }
    case LNR_mkdirat:
        lxrt_mounts_symlinks_changed();
        if (lxrt_mounts_readonly((const char *)a1)) { ret = LERR(EROFS); break; }
        ret = ret_of(mkdirat(lxrt_dirfd_to_darwin((int)a0),
                             translate((const char *)a1), (mode_t)a2));
        break;
    case LNR_unlinkat:
        lxrt_mounts_symlinks_changed();
        if (lxrt_mounts_readonly((const char *)a1)) { ret = LERR(EROFS); break; }
        {
            // LXRT_KEEP_DUMPS=1 (debugging aid): crash reporters delete their
            // minidumps after uploading; keep them so the guest's own crash
            // context can be read offline. The guest is told it succeeded.
            static int keep = -1;
            if (keep < 0) keep = getenv("LXRT_KEEP_DUMPS") ? 1 : 0;
            const char *pth = (const char *)a1;
            size_t pl = pth ? strlen(pth) : 0;
            if (keep && pl > 4 && !strcmp(pth + pl - 4, ".dmp")) { ret = 0; break; }
        }
        if (lxrt_at_is_removedir((int)a2))
            ret = ret_of(unlinkat(lxrt_dirfd_to_darwin((int)a0),
                                  translate((const char *)a1), AT_REMOVEDIR));
        else {
            int dfd = lxrt_dirfd_to_darwin((int)a0);
            const char *hp = translate((const char *)a1);
            ret = ret_of(unlinkat(dfd, hp, 0));
            // Linux answers unlink of a directory with EISDIR, Darwin with
            // EPERM. bionic's and glibc's remove() try unlink first and
            // rmdir only on EISDIR, so with EPERM no Java File.delete() of a
            // directory ever succeeded: Organic Maps could not remove its
            // own test directory and called its storage unusable (MEASURED).
            struct stat st;
            if (ret == LERR(EPERM) && fstatat(dfd, hp, &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISDIR(st.st_mode))
                ret = LERR(EISDIR);
        }
        break;
    case LNR_renameat:
    case LNR_renameat2: {
        lxrt_mounts_symlinks_changed();
        if (lxrt_mounts_readonly((const char *)a1) || lxrt_mounts_readonly((const char *)a3)) {
            ret = LERR(EROFS);
            break;
        }
        // renameat2's flags (RENAME_NOREPLACE and friends) have no Darwin
        // equivalent; refuse rather than silently ignore them.
        const char *from = translate((const char *)a1);
        // translate() returns a thread-local buffer, so the second path has to
        // be resolved after the first has been copied out of it.
        static _Thread_local char fromcopy[1024];
        snprintf(fromcopy, sizeof fromcopy, "%s", from);
        if (nr == LNR_renameat2 && a4 != 0) {
            // RENAME_NOREPLACE (1) and RENAME_EXCHANGE (2) are Darwin's
            // RENAME_EXCL and RENAME_SWAP; RENAME_WHITEOUT (4) is overlayfs-only.
            if ((a4 & ~3ull) || a4 == 3) { ret = LERR(EINVAL); break; }
            unsigned int dflags = (a4 & 1) ? RENAME_EXCL : RENAME_SWAP;
            ret = ret_of(renameatx_np(lxrt_dirfd_to_darwin((int)a0), fromcopy,
                                      lxrt_dirfd_to_darwin((int)a2),
                                      translate((const char *)a3), dflags));
            break;
        }
        ret = ret_of(renameat(lxrt_dirfd_to_darwin((int)a0), fromcopy,
                              lxrt_dirfd_to_darwin((int)a2),
                              translate((const char *)a3)));
        break;
    }
    case LNR_chdir:
        ret = ret_of(chdir(translate_follow((const char *)a0)));
        break;
    case LNR_fchdir:
        ret = ret_of(fchdir((int)a0));
        break;
    case LNR_fchmod:
        ret = ret_of(fchmod((int)a0, (mode_t)a1));
        break;
    case LNR_fchmodat:
        // Linux fchmodat takes THREE arguments -- there is no flags word, and
        // x3 is whatever the caller left in it. Passing it on made every
        // chmod() fail EINVAL at random (ldconfig in pressure-vessel, Steam's
        // minidump directory). fchmodat2 (452) is the one with flags.
        ret = ret_of(fchmodat(lxrt_dirfd_to_darwin((int)a0),
                              translate((const char *)a1), (mode_t)a2, 0));
        break;
    case 449:   // futex_waitv(waiters, nr, flags, timeout, clockid): futex_waitv.c
        ret = lxrt_futex_waitv(a0, (uint32_t)a1, (uint32_t)a2, a3, (int32_t)a4);
        break;
    case 452:   // fchmodat2(dirfd, path, mode, flags)
        ret = ret_of(fchmodat(lxrt_dirfd_to_darwin((int)a0),
                              translate((const char *)a1), (mode_t)a2,
                              lxrt_at_flags_to_darwin((int)a3)));
        break;
    case 40:    // mount: only here for Android ids (android_ids.c sends MS_BIND/MS_REMOUNT on)
    case 39:    // umount2
        if (!lxrt_aids_on()) { ret = LERR(ENOSYS); break; }
        if (nr == 39) {
            // A bind this process (or its zygote) made goes; anything else
            // is not a mount point here (one mount view).
            const char *t = (const char *)a0;
            ret = t && lxrt_mounts_unbind(t) ? 0 : LERR(EINVAL);
            break;
        }
        {
            // mount(src, dst, type, flags, data) with MS_BIND: an entry in
            // the per-process bind table (mounts.c), what zygote's
            // MountEmulatedStorage does for every child (/mnt/user/<n> on
            // /storage). The table follows fork and exec, as a mount
            // namespace follows its processes. MS_REMOUNT (flags or
            // read-only on an existing bind): accepted, read-only honoured.
            const char *src = (const char *)a0, *dst = (const char *)a1;
            uint64_t fl = a3;
            if (!dst) { ret = LERR(EFAULT); break; }
            struct stat ds, ss;
            char hdst[PATH_MAX];
            snprintf(hdst, sizeof hdst, "%s", translate_follow(dst));
            if (stat(hdst, &ds) != 0) { ret = LERR(errno); break; }
            if (a2 && !strcmp((const char *)a2, "tmpfs") && !(fl & (0x1000 | 0x20))) {
                if (!S_ISDIR(ds.st_mode)) { ret = LERR(ENOTDIR); break; }
                ret = tmpfs_mount(dst, (const char *)a4, (fl & 1) != 0);
                break;
            }
            if (fl & 0x20) {                          // MS_REMOUNT
                if (fl & 0x1000) {
                    char cur[PATH_MAX];
                    const char *h = lxrt_mounts_translate(dst, cur, sizeof cur);
                    if (h) lxrt_mounts_bind(dst, h, (fl & 1) != 0);   // MS_RDONLY
                }
                ret = 0;
                break;
            }
            if (!src) { ret = LERR(EFAULT); break; }
            char hsrc[PATH_MAX];
            snprintf(hsrc, sizeof hsrc, "%s", translate_follow(src));
            if (stat(hsrc, &ss) != 0) { ret = LERR(errno); break; }
            if (S_ISDIR(ss.st_mode) != S_ISDIR(ds.st_mode)) { ret = LERR(S_ISDIR(ss.st_mode) ? ENOTDIR : EISDIR); break; }
            lxrt_mounts_bind(dst, hsrc, (fl & 1) != 0);
            if (getenv("LXRT_ANDROID_IDS_LOG"))
                fprintf(stderr, "[lxrt] pid %d: bind %s (%s) on %s\n", (int)getpid(), src, hsrc, dst);
            ret = 0;
        }
        break;
    case LNR_fchown:
        if (lxrt_aids_on()) {
            // Android ids: a chown Linux would allow these ids succeeds; the
            // file stays the Mac user's and the owner is recorded for stat
            // (android_ids.h, virtual file ownership).
            const char *pf = lxrt_pathfd_path((int)a0);
            ret = lxrt_aids_chown_file(pf, pf ? -1 : (int)a0, false, (uint32_t)a1, (uint32_t)a2);
            break;
        }
        ret = ret_of(fchown((int)a0, (uid_t)a1, (gid_t)a2));
        if (ret == LERR(EINVAL)) {
            // Darwin refuses an owner for a socket or a pipe; Linux keeps one
            // (android_ids.c, the same case).
            struct stat st;
            if (fstat((int)a0, &st) == 0 && (S_ISSOCK(st.st_mode) || S_ISFIFO(st.st_mode)))
                ret = ((uid_t)a1 == (uid_t)-1 || (uid_t)a1 == getuid()) &&
                      ((gid_t)a2 == (gid_t)-1 || (gid_t)a2 == getgid()) ? 0 : LERR(EPERM);
        }
        break;
    case 88: {  // utimensat(dirfd, path, times[2], flags)
        // FEX turns every x86 variant (utime, utimes, futimesat, the i386
        // time32/time64 pairs) into this one. The special nanosecond values
        // differ: Linux UTIME_NOW/OMIT are (1<<30)-1 / (1<<30)-2.
        struct timespec ts[2], *tp = NULL;
        bool bad = false;
        if (a2) {
            const struct timespec *lt = (const struct timespec *)a2;
            for (int i = 0; i < 2; i++) {
                ts[i] = lt[i];
                if (lt[i].tv_nsec == 0x3fffffff) ts[i].tv_nsec = UTIME_NOW;
                else if (lt[i].tv_nsec == 0x3ffffffe) ts[i].tv_nsec = UTIME_OMIT;
                else if (lt[i].tv_nsec < 0 || lt[i].tv_nsec > 999999999) bad = true;
            }
            tp = ts;
        }
        if (bad)
            ret = LERR(EINVAL);
        else if (!a1 || (((int)a3 & 0x1000) && !*(const char *)a1))   // NULL path / AT_EMPTY_PATH
            ret = ret_of(futimens((int)a0, tp));
        else
            ret = ret_of(utimensat(lxrt_dirfd_to_darwin((int)a0),
                                   ((int)a3 & 0x100) ? translate((const char *)a1)
                                                     : translate_follow((const char *)a1),
                                   tp, lxrt_at_flags_to_darwin((int)a3)));
        break;
    }
    case LNR_fchownat:
        if (lxrt_aids_on()) {
            const char *cp = (const char *)a1;
            if (!cp) { ret = LERR(EFAULT); break; }
            if (!*cp && ((int)a4 & 0x1000)) {                 // AT_EMPTY_PATH: the descriptor
                int dfd = lxrt_dirfd_to_darwin((int)a0);
                const char *pf = lxrt_pathfd_path(dfd);
                ret = lxrt_aids_chown_file(pf, pf ? -1 : dfd, false, (uint32_t)a2, (uint32_t)a3);
                break;
            }
            bool nof = ((int)a4 & 0x100) != 0;              // AT_SYMLINK_NOFOLLOW (lchown)
            ret = lxrt_aids_chown_at(lxrt_dirfd_to_darwin((int)a0), nof ? translate(cp) : translate_follow(cp),
                                     nof, (uint32_t)a2, (uint32_t)a3);
            break;
        }
        ret = ret_of(fchownat(lxrt_dirfd_to_darwin((int)a0),
                              translate((const char *)a1), (uid_t)a2, (gid_t)a3,
                              lxrt_at_flags_to_darwin((int)a4)));
        break;
    case LNR_mknodat: {
        // FIFOs (srt-logger, pressure-vessel) and plain files; device nodes
        // need privileges a Linux guest would not have either.
        unsigned mode = (unsigned)a2;
        const char *path = translate((const char *)a1);
        int dfd = lxrt_dirfd_to_darwin((int)a0);
        switch (mode & 0170000) {
        case 0010000:   // S_IFIFO
            ret = ret_of(mkfifoat(dfd, path, (mode_t)(mode & 07777)));
            break;
        case 0:
        case 0100000: { // S_IFREG
            int fd = openat(dfd, path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, (mode_t)(mode & 07777));
            if (fd < 0) { ret = LERR(errno); break; }
            close(fd);
            ret = 0;
            break;
        }
        case 0140000:   // S_IFSOCK: bind() creates these; mknod of one is rare
            ret = LERR(EOPNOTSUPP);
            break;
        default:
            ret = LERR(EPERM);
        }
        break;
    }
    case LNR_symlinkat: {
        lxrt_mounts_symlinks_changed();
        // An absolute target is a guest path; the host kernel resolves it
        // against the host's "/". Store the host path of the target instead
        // (readlinkat strips it back), so walking the link lands inside the
        // guest root. /proc and /dev targets stay as written: the first is
        // synthesised per process, the second passes through.
        const char *target = (const char *)a0;
        char host_target[1024];
        const char *root = guest_root();
        // Only when the target resolves in THIS view: pressure-vessel creates
        // links for the sandbox it is about to enter (/run/host/...), which
        // must stay guest paths for guest_resolve() to follow in there.
        char probe[1024];
        if (root && target && target[0] == '/' &&
            strncmp(target, "/proc", 5) != 0 && strncmp(target, "/dev", 4) != 0 &&
            strncmp(target, root, strlen(root)) != 0 &&
            access(translate_one(target, probe, sizeof probe), F_OK) == 0) {
            if (lxrt_mounts_active()) {
                // Inside a bwrap sandbox the root is a scratch directory that
                // disappears with it, and the binds exist only in this
                // process's table: a host path through the root pointed into
                // nothing. Wine's dosdevices/z: -> "/" became
                // "/tmp/lxrt-sandbox-XXXX/", so Z:\ found no file (MEASURED:
                // "Failed to create process ... d3d11_clear.exe: 2") and the
                // prefix kept the dead link. A target inside a bind is stored
                // as the bind's host path; anything else stays the guest path,
                // which guest_resolve() follows in guest terms.
                const char *h = translate_one(target, probe, sizeof probe);
                size_t rl = strlen(root);
                if (h && strncmp(h, root, rl) != 0 && snprintf(host_target, sizeof host_target, "%s", h) < (int)sizeof host_target)
                    target = host_target;
            } else {
                int n = snprintf(host_target, sizeof host_target, "%s%s", root, target);
                if (n > 0 && (size_t)n < sizeof host_target)
                    target = host_target;
            }
        }
        ret = ret_of(symlinkat(target, lxrt_dirfd_to_darwin((int)a1),
                               translate((const char *)a2)));
        break;
    }
    case LNR_ftruncate: {
        // memfd seals are enforced here, by the runtime, because Darwin has no
        // equivalent -- see fex_support.c for what that promise is and is not.
        long seal = lxrt_memfd_check_ftruncate((int)a0, (int64_t)a1);
        if (seal < 0) { ret = seal; break; }
        ret = ret_of(ftruncate((int)a0, (off_t)a1));
        break;
    }
    case LNR_fsync:
        ret = ret_of(fsync((int)a0));
        break;
    case LNR_pread64:
        if (g_trace) {
            // pread on a regular file cannot block, so if it ever does, what
            // the descriptor actually IS becomes the whole question.
            struct stat st;
            char path[1024] = "?";
            fcntl((int)a0, F_GETPATH, path);
            if (fstat((int)a0, &st) == 0)
                fprintf(lxrt_trace_stream(), "[lxrt]    pread fd=%d mode=0%o size=%lld off=%lld "
                                "count=%llu path=%s\n",
                        (int)a0, st.st_mode, (long long)st.st_size,
                        (long long)a3, (unsigned long long)a2, path);
            else
                fprintf(lxrt_trace_stream(), "[lxrt]    pread fd=%d fstat failed: %s\n",
                        (int)a0, strerror(errno));
            // And what the destination buffer is: a copyout into a page with
            // the wrong protection is the only way this call misbehaves.
            mach_vm_address_t ra = a1;
            mach_vm_size_t rs = 0;
            vm_region_basic_info_data_64_t ri;
            mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj = MACH_PORT_NULL;
            if (mach_vm_region(mach_task_self(), &ra, &rs,
                               VM_REGION_BASIC_INFO_64, (vm_region_info_t)&ri,
                               &rc, &obj) == KERN_SUCCESS)
                fprintf(lxrt_trace_stream(), "[lxrt]    buf 0x%llx in region 0x%llx+0x%llx "
                                "prot=%c%c%c%s\n",
                        (unsigned long long)a1, (unsigned long long)ra,
                        (unsigned long long)rs,
                        (ri.protection & VM_PROT_READ) ? 'r' : '-',
                        (ri.protection & VM_PROT_WRITE) ? 'w' : '-',
                        (ri.protection & VM_PROT_EXECUTE) ? 'x' : '-',
                        lxrt_subpage_tracked(a1, 1) ? " [subpage]" : "");
        }
        if (g_trace) {
            // Separate "the memory is unusable" from "the file is unusable":
            // touching the buffer here either faults, hangs, or proves the
            // destination is fine and the problem is elsewhere.
            *(volatile char *)a1 = 0;
            // Read a KNOWN-GOOD control file at the same instant. If the
            // control read also blocks, the process is in a bad state; if only
            // the guest's own file blocks, the file is.
            const char *ctl = getenv("LXRT_CONTROL_FILE");
            if (ctl) {
                int cfd = open(ctl, O_RDONLY);
                if (cfd >= 0) {
                    char cb[64];
                    fprintf(lxrt_trace_stream(), "[lxrt]    control read of %s... ", ctl);
                    fflush(stderr);
                    ssize_t cr = pread(cfd, cb, sizeof cb, 0);
                    fprintf(lxrt_trace_stream(), "%zd\n", cr);
                    close(cfd);
                }
            }
            fprintf(lxrt_trace_stream(), "[lxrt]    buffer writable, probing with a host buffer\n");
            char probe[512];
            ssize_t pr = pread((int)a0, probe, a2 > sizeof probe ? sizeof probe : (size_t)a2,
                               (off_t)a3);
            fprintf(lxrt_trace_stream(), "[lxrt]    host-buffer pread -> %zd (%s)\n", pr,
                    pr < 0 ? strerror(errno) : "ok");
        }
        ret = ret_of((long)pread((int)a0, (void *)a1, (size_t)a2, (off_t)a3));
        if (g_trace)
            fprintf(lxrt_trace_stream(), "[lxrt]    pread returned %ld\n", ret);
        break;
    case LNR_pwrite64: {
        long seal = lxrt_memfd_check_write((int)a0, (int64_t)a3, a2);
        if (seal < 0) { ret = seal; break; }
        ret = ret_of((long)pwrite((int)a0, (const void *)a1, (size_t)a2, (off_t)a3));
        break;
    }
    case LNR_umask:
        ret = (long)umask((mode_t)a0);
        break;
    case LNR_fcntl: {
        // memfd sealing (F_ADD_SEALS / F_GET_SEALS) has no Darwin counterpart
        // and is answered from runtime state.
        bool memfd_handled = false;
        long mres = lxrt_memfd_fcntl((int)a0, (int)a1, (unsigned long)a2,
                                     &memfd_handled);
        if (memfd_handled) { ret = mres; break; }
        }
        // F_DUPFD/F_GETFD/F_SETFD/F_GETFL/F_SETFL agree in number; the *flags*
        // carried by F_GETFL/F_SETFL do not.
        switch ((int)a1) {
        case F_GETFL: {
            int r = fcntl((int)a0, F_GETFL);
            ret = r < 0 ? LERR(errno) : lxrt_open_flags_to_linux(r);
            break;
        }
        case F_SETFL:
            ret = ret_of(fcntl((int)a0, F_SETFL,
                               lxrt_open_flags_to_darwin((int)a2)));
            break;
        case F_DUPFD:
        case 1030: { // Linux F_DUPFD_CLOEXEC
            int nfd = fcntl((int)a0, (int)a1 == F_DUPFD ? F_DUPFD : F_DUPFD_CLOEXEC,
                            (int)a2);
            if (nfd < 0) { ret = LERR(errno); break; }
            // An alias made here is an alias like any other: every module
            // that keys state by descriptor has to learn about it.
            alias_fd((int)a0, nfd);
            ret = nfd;
            break;
        }
        default: {
            bool is_lock = false;
            int dcmd = lxrt_fcntl_lock_cmd((int)a1, &is_lock);
            if (is_lock) {
                if (!a2) { ret = LERR(EFAULT); break; }
                struct flock fl;
                memset(&fl, 0, sizeof fl);
                lxrt_flock_to_darwin((const void *)a2, &fl);
                if (fcntl((int)a0, dcmd, &fl) != 0) { ret = LERR(errno); break; }
                if (dcmd == F_GETLK)
                    lxrt_flock_to_linux(&fl, (void *)a2);
                ret = 0;
                break;
            }
            ret = ret_of(fcntl((int)a0, (int)a1, (long)a2));
            break;
        }
        }
        break;
    case LNR_prctl:
        // Only the options with a Darwin meaning are honoured. The rest return
        // 0 rather than -EINVAL because callers commonly treat a failure as
        // fatal for options that are advisory on Linux too.
        switch ((int)a0) {
        case 15: // PR_SET_NAME
            pthread_setname_np((const char *)a1);
            ret = 0;
            break;
        case 16: // PR_GET_NAME
            if (a1)
                pthread_getname_np(pthread_self(), (char *)a1, 16);
            ret = 0;
            break;
        case 35: // PR_SET_MM
            // PR_SET_MM_MAP (14): FEX points arg_start/arg_end at the x86
            // program's argv so /proc/<pid>/cmdline names the guest program
            // (the Steam client checks its UI's peer process by it). Only the
            // cmdline is taken; the rest of the map has no Darwin meaning.
            if (a1 == 14 && a2 && a3 >= 11 * 8 && !getenv("LXRT_NO_SETMM")) {
                const uint64_t *m = (const uint64_t *)a2;
                uint64_t as = m[7], ae = m[8];
                if (as && ae > as && ae - as < (1u << 20))
                    lxrt_proc_set_cmdline((const char *)as, (size_t)(ae - as));
            }
            ret = 0;
            break;
        case 55: // PR_SET_TAGGED_ADDR_CTRL
        case 56: // PR_GET_TAGGED_ADDR_CTRL
            // NOT supported: the tagged address ABI means the kernel accepts
            // pointers with a tag in the top byte in every syscall, and the
            // runtime hands guest pointers to Darwin as they are. bionic
            // (Android 11) turns heap pointer tagging on when the SET call
            // succeeds: malloc then returns 0xb4... pointers, the CPU's
            // top-byte-ignore loads and stores through them fine, and the
            // first read(2) into one got EFAULT from Darwin -- mksh read no
            // script and exited 0 in silence (MEASURED,
            // benchmarks/stage25-android-userspace.txt). EINVAL is a kernel
            // without the ABI (Linux < 5.4); bionic then keeps its pointers
            // untagged.
            ret = LERR(EINVAL);
            break;
        case 0x6d4d444c: // PR_GET_MEM_MODEL (Asahi kernels)
        case 0x4d4d444c: // PR_SET_MEM_MODEL
            // NOT supported, and it matters: FEX asks for PR_SET_MEM_MODEL_TSO
            // and, when that "succeeds", turns off all of its x86 memory-order
            // emulation (Context.h SetHardwareTSOSupport). The old blanket 0
            // made every multithreaded x86 guest run with ARM's weak ordering
            // -- the Steam client's lock-free lists then corrupted themselves
            // (MEASURED: jumps to 0x814c7309b93f9500-style garbage). Apple
            // silicon's TSO bit is not reachable from user space.
            ret = LERR(EINVAL);
            break;
        case 38: // PR_SET_NO_NEW_PRIVS: one way, and remembered
        case 39: // PR_GET_NO_NEW_PRIVS
            // FEX's seccomp emulation installs a filter only for a task with
            // no_new_privs (or CAP_SYS_ADMIN), and asks PR_GET: answered 0
            // after a successful PR_SET, every sandbox's filter was refused
            // with EACCES (Android ids keep their own bit, android_ids.c).
            {
                static _Atomic int nnp = -1;
                if (atomic_load(&nnp) < 0)
                    atomic_store(&nnp, getenv("LXRT_NO_NEW_PRIVS") ? 1 : 0);
                if (a2 || a3 || a4 || (a0 == 38 && a1 != 1) || (a0 == 39 && a1)) { ret = LERR(EINVAL); break; }
                if (a0 == 38) { atomic_store(&nnp, 1); setenv("LXRT_NO_NEW_PRIVS", "1", 1); ret = 0; }
                else ret = atomic_load(&nnp);
            }
            break;
        default:
            ret = 0;
            break;
        }
        break;
    case LNR_getuid:
        ret = getuid();
        break;
    // Supplementary groups: the same call and the same 32-bit gid_t array
    // on both systems (0 asks for the count; too small a buffer is EINVAL).
    // toybox id (x86-64 Android under FEX) failed on the ENOSYS.
    case LNR_getgroups: {
        int n = getgroups((int)a0, a0 ? (gid_t *)a1 : NULL);
        ret = n < 0 ? LERR(errno) : n;
        break;
    }
    case LNR_setgroups:
        ret = setgroups((int)a0, (const gid_t *)a1) != 0 ? LERR(errno) : 0;
        break;
    case LNR_geteuid:
        ret = geteuid();
        break;
    case LNR_getgid:
        ret = getgid();
        break;
    case LNR_getegid:
        ret = getegid();
        break;
    case LNR_socket:
        ret = lxrt_socket((int)a0, (int)a1, (int)a2);
        break;
    case LNR_socketpair:
        ret = lxrt_socketpair((int)a0, (int)a1, (int)a2, (int *)a3);
        break;
    case LNR_connect:
        ret = lxrt_connect((int)a0, (const void *)a1, (unsigned)a2);
        break;
    case LNR_bind:
        ret = lxrt_bind((int)a0, (const void *)a1, (unsigned)a2);
        break;
    case LNR_listen:
        ret = ret_of(listen((int)a0, (int)a1));
        break;
    case LNR_accept:
        ret = lxrt_accept4((int)a0, (void *)a1, (uint32_t *)a2, 0);
        break;
    case LNR_accept4:
        ret = lxrt_accept4((int)a0, (void *)a1, (uint32_t *)a2, (int)a3);
        break;
    case LNR_getsockname:
        ret = lxrt_getsockname((int)a0, (void *)a1, (uint32_t *)a2, false);
        break;
    case LNR_getpeername:
        ret = lxrt_getsockname((int)a0, (void *)a1, (uint32_t *)a2, true);
        break;
    case LNR_shutdown:
        ret = ret_of(shutdown((int)a0, (int)a1));
        break;
    case LNR_sendto:
        ret = lxrt_sendto((int)a0, (const void *)a1, (size_t)a2, (int)a3, (const void *)a4, (unsigned)a5);
        break;
    case LNR_recvfrom:
        ret = lxrt_recvfrom((int)a0, (void *)a1, (size_t)a2, (int)a3, (void *)a4, (uint32_t *)a5);
        break;
    case LNR_sendmsg:
        ret = lxrt_sendmsg((int)a0, (const void *)a1, (int)a2);
        break;
    case LNR_recvmsg:
        ret = lxrt_recvmsg((int)a0, (void *)a1, (int)a2);
        break;
    case LNR_setsockopt:
        ret = lxrt_setsockopt((int)a0, (int)a1, (int)a2, (const void *)a3,
                              (unsigned)a4);
        break;
    case LNR_getsockopt:
        ret = lxrt_getsockopt((int)a0, (int)a1, (int)a2, (void *)a3,
                              (unsigned *)a4);
        break;
    case LNR_pipe2: {
        int fds[2];
        if (pipe(fds) != 0) { ret = LERR(errno); break; }
        // Linux carries O_CLOEXEC/O_NONBLOCK in flags; Darwin's pipe takes none.
        int lf = (int)a1;
        for (int i = 0; i < 2; i++) {
            if (lf & 0x80000) fcntl(fds[i], F_SETFD, FD_CLOEXEC);
            if (lf & 0x800) fcntl(fds[i], F_SETFL, fcntl(fds[i], F_GETFL) | O_NONBLOCK);
        }
        ((int *)a0)[0] = fds[0];
        ((int *)a0)[1] = fds[1];
        ret = 0;
        break;
    }
    /* ---------------- epoll and eventfd ---------------- */
    case LNR_epoll_create1:
        ret = lxrt_epoll_create1((int)a0);
        break;
    case LNR_epoll_ctl:
        ret = lxrt_epoll_ctl((int)a0, (int)a1, (int)a2, (const void *)a3);
        // binder_poll marks the calling thread as a poller (LOOPER_STATE_POLL).
        if (ret == 0 && ((int)a1 == 1 || (int)a1 == 3) && lxrt_binder_is((int)a2))
            lxrt_binder_polled((int)a2);
        break;
    case LNR_epoll_pwait:
        ret = lxrt_epoll_pwait((int)a0, (void *)a1, (int)a2, (int)a3,
                               (const uint64_t *)a4, (size_t)a5);
        break;
    case LNR_epoll_pwait2:
        ret = lxrt_epoll_pwait2((int)a0, (void *)a1, (int)a2,
                                (const void *)a3, (const uint64_t *)a4,
                                (size_t)a5);
        break;
    case LNR_eventfd2_n:
        ret = lxrt_eventfd2((unsigned)a0, (int)a1);
        break;
    case LNR_ioctl:
        ret = lxrt_ioctl((int)a0, (unsigned long)a1, a2);
        break;
    case LNR_sysinfo: {
        // struct sysinfo, 112 bytes on 64-bit Linux. Chromium reads totalram
        // for its memory-pressure heuristics; everything else is best effort.
        struct {
            int64_t uptime; uint64_t loads[3];
            uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
            uint16_t procs, pad; uint64_t totalhigh, freehigh; uint32_t mem_unit;
            uint8_t _f[20 - 2 * sizeof(long) - sizeof(int)];
        } si;
        memset(&si, 0, sizeof si);
        _Static_assert(sizeof si == 112, "Linux struct sysinfo is 112 bytes");
        uint64_t memsize = 0; size_t sz = sizeof memsize;
        sysctlbyname("hw.memsize", &memsize, &sz, NULL, 0);
        struct timeval boot; sz = sizeof boot;
        if (sysctlbyname("kern.boottime", &boot, &sz, NULL, 0) == 0)
            si.uptime = (int64_t)(time(NULL) - boot.tv_sec);
        double la[3];
        if (getloadavg(la, 3) == 3)
            for (int i = 0; i < 3; i++) si.loads[i] = (uint64_t)(la[i] * 65536.0);
        vm_statistics64_data_t vs; mach_msg_type_number_t cnt = HOST_VM_INFO64_COUNT;
        uint64_t pagesz = (uint64_t)getpagesize();
        if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vs, &cnt) == KERN_SUCCESS)
            si.freeram = ((uint64_t)vs.free_count + (uint64_t)vs.inactive_count) * pagesz;
        si.totalram = memsize;
        si.procs = 64;
        si.mem_unit = 1;
        if (!a0) { ret = LERR(EFAULT); break; }
        memcpy((void *)a0, &si, sizeof si);
        ret = 0;
        break;
    }
    case LNR_getpgid:
        if (lxrt_ids_on() && a0 && lxrt_ids_target_pid((int)a0) < 0) {
            ret = LERR(ESRCH); break;
        }
        ret = ret_of(getpgid((pid_t)(a0 ? lxrt_ids_target_pid((int)a0) : 0)));
        if (ret > 0 && lxrt_ids_on()) {
            ret = lxrt_ids_to_guest((int)ret, 0);
            if (!ret) ret = 1;
        }
        break;
    case LNR_setpgid:
        if (lxrt_ids_on() &&
            ((a0 && lxrt_ids_target_pid((int)a0) < 0) ||
             (a1 && lxrt_ids_target_pid((int)a1) < 0))) {
            ret = LERR(ESRCH); break;
        }
        ret = ret_of(setpgid((pid_t)(a0 ? lxrt_ids_target_pid((int)a0) : 0),
                             (pid_t)(a1 ? lxrt_ids_target_pid((int)a1) : 0)));
        break;
    case 156: { // getsid
        int host = a0 ? lxrt_ids_target_pid((int)a0) : 0;
        if (lxrt_ids_on() && a0 && host < 0) { ret = LERR(ESRCH); break; }
        ret = ret_of(getsid((pid_t)host));
        if (ret > 0 && lxrt_ids_on()) {
            ret = lxrt_ids_to_guest((int)ret, 0);
            if (!ret) ret = 1;
        }
        break;
    }
    case LNR_gettimeofday: {
        // (struct timeval *tv, struct timezone *tz). Linux's timeval is two
        // 64-bit fields; Darwin's tv_usec is a 32-bit int followed by padding,
        // so a memcpy handed the guest stack garbage in the upper half of
        // tv_usec (MEASURED: Chromium log stamps like ".4295097544").
        // tz is obsolete and left zeroed.
        struct timeval tv;
        if (gettimeofday(&tv, NULL) != 0) { ret = LERR(errno); break; }
        if (a0) {
            ((int64_t *)a0)[0] = (int64_t)tv.tv_sec;
            ((int64_t *)a0)[1] = (int64_t)tv.tv_usec;
        }
        if (a1) memset((void *)a1, 0, 8);
        ret = 0;
        break;
    }
    case LNR_setxattr: case LNR_lsetxattr: case LNR_fsetxattr:
    case LNR_getxattr: case LNR_lgetxattr: case LNR_fgetxattr:
    case LNR_listxattr: case LNR_llistxattr: case LNR_flistxattr:
    case LNR_removexattr: case LNR_lremovexattr: case LNR_fremovexattr:
        ret = do_xattr(nr, a0, a1, a2, a3, a4);
        break;
    case 71:                // sendfile
        ret = do_sendfile((int)a0, (int)a1, a2, a3);
        break;
    case 76:                // splice
        ret = do_splice((int)a0, a1, (int)a2, a3, a4, (unsigned)a5);
        break;
    case 270: case 271:     // process_vm_readv, process_vm_writev
        ret = do_process_vm(nr == 271, a0, a1, a2, a3, a4, a5);
        break;
    case LNR_timerfd_create:
        ret = lxrt_timerfd_create((int)a0, (int)a1);
        break;
    case LNR_timerfd_settime:
        ret = lxrt_timerfd_settime((int)a0, (int)a1, (const void *)a2, (void *)a3);
        break;
    case LNR_timerfd_gettime:
        ret = lxrt_timerfd_gettime((int)a0, (void *)a1);
        break;
    case LNR_timer_create:
        ret = lxrt_posix_timer_create((int)a0, (const void *)a1, (int *)a2);
        break;
    case LNR_timer_gettime:
        ret = lxrt_posix_timer_gettime((int)a0, (void *)a1);
        break;
    case LNR_timer_getoverrun:
        ret = lxrt_posix_timer_getoverrun((int)a0);
        break;
    case LNR_timer_settime:
        ret = lxrt_posix_timer_settime((int)a0, (int)a1, (const void *)a2, (void *)a3);
        break;
    case LNR_timer_delete:
        ret = lxrt_posix_timer_delete((int)a0);
        break;
    case LNR_signalfd4:
        ret = lxrt_signalfd4((int)a0, (const uint64_t *)a1, (size_t)a2, (int)a3);
        break;
    case LNR_inotify_init1:
        ret = lxrt_inotify_init1((int)a0);
        break;
    case LNR_inotify_add_watch:
        if (!a1) { ret = LERR(EFAULT); break; }
        ret = lxrt_inotify_add_watch((int)a0, (const char *)a1, (uint32_t)a2);
        break;
    case LNR_inotify_rm_watch:
        ret = lxrt_inotify_rm_watch((int)a0, (int)a1);
        break;
    case LNR_mremap:
        // (old_addr, old_len, new_len, flags, new_addr). Darwin has no mremap;
        // mremap.c grows in place when the next pages are free and moves
        // otherwise.
        if (host_heap_hit(a0, a1, "mremap") ||
            ((a3 & 2) && host_heap_hit(a4, a2, "mremap MREMAP_FIXED"))) {
            ret = LERR(EINVAL);
            break;
        }
        lxrt_privmap_forget(a0, a1);
        // 4 KiB guest pages that do not fill a host page: do_mremap_subpage.
        if ((a0 % LXRT_HOST_PAGE) || ((a3 & 2) && (a4 % LXRT_HOST_PAGE)) ||
            lxrt_subpage_tracked(a0, a1) || ((a3 & 2) && lxrt_subpage_tracked(a4, a2)))
            ret = do_mremap_subpage(a0, a1, a2, (long)a3, a4);
        else
            ret = lxrt_mremap(a0, a1, a2, (int)a3, a4);
        if (ret >= 0) {
            lxrt_wx_moved(a0, a1, (uint64_t)ret, a2);
            // The host pages moved, shrank or were replaced: none of them is
            // a 4 KiB-offset mapping's alone any more (offmap.c).
            lxrt_offmap_disown(a0, a1);
            lxrt_offmap_disown((uint64_t)ret, a2);
        }
        lxrt_memlog('r', a0, a1, (long)a2, (long)a3, ret);
        if (ret >= 0)
            lxrt_memlog('r', (uint64_t)ret, a2, (long)a2, (long)a3, ret);
        break;

    /* ---------------- System V IPC ---------------- */
    case LNR_semget:
        ret = lxrt_semget((int32_t)a0, (int)a1, (int)a2);
        break;
    case LNR_semctl:
        ret = lxrt_semctl((int)a0, (int)a1, (int)a2, a3);
        break;
    case LNR_semop:
        ret = lxrt_semop((int)a0, (const void *)a1, a2);
        break;
    case LNR_semtimedop:
        ret = lxrt_semtimedop((int)a0, (const void *)a1, a2, (const void *)a3);
        break;
    case LNR_shmget:
        ret = lxrt_shmget((int32_t)a0, a1, (int)a2);
        break;
    case LNR_shmat:
        if (a1) {
            // Check exactly the bytes the segment will cover: a fixed 16 KiB
            // probe ran past the end of a guest window into the malloc
            // region that follows it and refused a legal attach, which the
            // guest then retried forever.
            struct shmid_ds ds;
            uint64_t sz = shmctl((int)a0, IPC_STAT, &ds) == 0 ? ds.shm_segsz : 4096;
            if (host_heap_hit(a1, sz, "shmat")) {
                ret = LERR(EINVAL);
                break;
            }
        }
        ret = lxrt_shmat((int)a0, a1, (int)a2);
        lxrt_memlog('s', ret >= 0 ? (uint64_t)ret : a1, LXRT_HOST_PAGE, (long)a0, (long)a2, ret);
        if (ret >= 0) {
            struct shmid_ds ds;
            uint64_t sz = shmctl((int)a0, IPC_STAT, &ds) == 0 ? ds.shm_segsz : LXRT_HOST_PAGE;
            lxrt_futex_shared_add((uint64_t)ret, sz);
            lxrt_offmap_disown((uint64_t)ret, sz);
        }
        break;
    case LNR_shmdt:
        ret = lxrt_shmdt(a0);
        lxrt_memlog('d', a0, LXRT_HOST_PAGE, 0, 0, ret);
        break;
    case LNR_shmctl:
        ret = lxrt_shmctl((int)a0, (int)a1, (void *)a2);
        break;

    /* ---------------- files, processes, scheduling ---------------- */
    case LNR_flock:
        ret = lxrt_flock((int)a0, (int)a1);
        break;
    case LNR_waitid:
        ret = lxrt_waitid((int)a0, (int)a1, (void *)a2, (int)a3, (void *)a4);
        break;
    case LNR_openat2:
        if (lxrt_mounts_readonly((const char *)a1)) {
            // Only writes are refused; the how struct is checked inside.
            struct { uint64_t flags, mode, resolve; } how_ro;
            mach_vm_size_t got = 0;
            if (a2 && mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)a2,
                                             sizeof how_ro, (mach_vm_address_t)&how_ro,
                                             &got) == KERN_SUCCESS && got == sizeof how_ro) {
                if ((how_ro.flags & 3) != 0 || (how_ro.flags & (0x40 | 0x200))) { ret = LERR(EROFS); break; }
            }
        }
        {
            const char *mp = at_through_mounts((int)a0, (const char *)a1,
                                               !(a2 && a3 >= 8 && (*(const uint64_t *)a2 & 0x8000)));
            if (mp && a2 && a3 >= 24) {
                // The bind already placed it; the walk inside the descriptor's
                // tree that RESOLVE_IN_ROOT/BENEATH ask for does not apply.
                uint64_t how2[3];
                memcpy(how2, (const void *)a2, sizeof how2);
                how2[2] &= ~(uint64_t)(0x08 | 0x10);
                ret = lxrt_openat2(-100, mp, how2, sizeof how2);
            } else {
                ret = lxrt_openat2((int)a0, translate((const char *)a1),
                                   (const void *)a2, (size_t)a3);
            }
        }
        break;
    case LNR_statx:
        {
            const char *mp = at_through_mounts((int)a0, (const char *)a1, !((int)a2 & 0x100));
            ret = lxrt_statx(mp ? -100 : (int)a0, mp ? mp : translate((const char *)a1), (int)a2,
                             (unsigned)a3, (void *)a4);
        }
        break;
    case LNR_statfs:
        ret = lxrt_statfs(translate((const char *)a0), (void *)a1);
        break;
    case LNR_fstatfs:
        ret = lxrt_fstatfs((int)a0, (void *)a1);
        break;
    case LNR_fallocate:
        ret = lxrt_fallocate((int)a0, (int)a1, (int64_t)a2, (int64_t)a3);
        break;
    case LNR_fdatasync:
        ret = lxrt_fdatasync((int)a0);
        break;
    case LNR_linkat_n: {
        lxrt_mounts_symlinks_changed();
        // translate() hands back a thread-local buffer, so the first path has
        // to be copied out before the second one overwrites it.
        static _Thread_local char oldcopy[1024];
        snprintf(oldcopy, sizeof oldcopy, "%s", translate((const char *)a1));
        ret = lxrt_linkat((int)a0, oldcopy, (int)a2,
                          translate((const char *)a3), (int)a4);
        break;
    }
    case LNR_faccessat2:
        if (lxrt_binder_context_of((const char *)a1) >= 0) {
            ret = ((int)a2 & 1) ? LERR(EACCES) : 0;
            break;
        }
        {
            const char *mp = at_through_mounts((int)a0, (const char *)a1, !((int)a3 & 0x100));
            ret = lxrt_faccessat2(mp ? -100 : (int)a0, mp ? mp : translate((const char *)a1), (int)a2,
                                  (int)a3);
        }
        break;
    case LNR_sendmmsg:
        ret = lxrt_sendmmsg((int)a0, (void *)a1, (unsigned)a2, (int)a3);
        break;
    case LNR_recvmmsg:
        ret = lxrt_recvmmsg((int)a0, (void *)a1, (unsigned)a2, (int)a3,
                            (void *)a4);
        break;
    case LNR_sched_get_priority_max:
        ret = lxrt_sched_get_priority_max((int)a0);
        break;
    case LNR_sched_get_priority_min:
        ret = lxrt_sched_get_priority_min((int)a0);
        break;
    case LNR_sched_setscheduler:
        ret = lxrt_sched_setscheduler((int)a0, (int)a1, (const void *)a2);
        break;
    case LNR_sched_getscheduler:
        ret = lxrt_sched_getscheduler((int)a0);
        break;
    case LNR_sched_getparam:
        ret = lxrt_sched_getparam((int)a0, (void *)a1);
        break;
    case LNR_setpriority:
        ret = lxrt_setpriority((int)a0, (int)a1, (int)a2);
        break;
    case LNR_getpriority:
        ret = lxrt_getpriority((int)a0, (int)a1);
        break;
    case LNR_setresuid:
        ret = lxrt_setresuid((int32_t)a0, (int32_t)a1, (int32_t)a2);
        break;
    // The two-argument and one-argument forms, as the three-argument ones
    // (an unprivileged process: setting the id it already has succeeds, any
    // other fails with EPERM -- lxrt_setres* decides). X servers drop to
    // getuid()/getgid() in every popen() child.
    case LNR_setuid:
        ret = lxrt_setresuid(-1, (int32_t)a0, -1);
        break;
    case LNR_setreuid:
        ret = lxrt_setresuid((int32_t)a0, (int32_t)a1, -1);
        break;
    case LNR_setgid:
        ret = lxrt_setresgid(-1, (int32_t)a0, -1);
        break;
    case LNR_setregid:
        ret = lxrt_setresgid((int32_t)a0, (int32_t)a1, -1);
        break;
    case LNR_setfsuid:     // returns the previous fsuid; it never changes here
        ret = (long)getuid();
        break;
    case LNR_setfsgid:
        ret = (long)getgid();
        break;
    case LNR_getresuid:
        ret = lxrt_getresuid((void *)a0, (void *)a1, (void *)a2);
        break;
    case LNR_setresgid:
        ret = lxrt_setresgid((int32_t)a0, (int32_t)a1, (int32_t)a2);
        break;
    case LNR_getresgid:
        ret = lxrt_getresgid((void *)a0, (void *)a1, (void *)a2);
        break;

    /* ---------------- what FEX calls directly ---------------- */
    case LNR_memfd_create:
        ret = lxrt_memfd_create((const char *)a0, (unsigned)a1);
        break;
    case LNR_execveat:
        ret = lxrt_execveat((int)a0, (const char *)a1, (char *const *)a2,
                            (char *const *)a3, (int)a4);
        break;
    case LNR_rt_sigtimedwait:
        ret = lxrt_rt_sigtimedwait((const void *)a0, (void *)a1,
                                   (const void *)a2, (size_t)a3);
        break;

    case LNR_getpid:
        ret = lxrt_ids_pid();
        break;
    case LNR_gettid:
        ret = lxrt_gettid();
        break;
    case LNR_getppid:
        ret = lxrt_ids_on() ? lxrt_ids_to_guest(getppid(), 0) : getppid();
        if (lxrt_ids_on() && ret == 0) ret = 1;
        break;
    case LNR_set_tid_address:
        ret = lxrt_set_tid_address((uint32_t *)a0);
        break;
    case LNR_clone:
        // aarch64 orders the arguments (flags, stack, ptid, tls, ctid) --
        // note tls before ctid, unlike x86-64.
        ret = lxrt_clone(a0, a1, (uint32_t *)a2, a3, (uint32_t *)a4, r);
        break;
    case LNR_execve: {
        // Linux fails an execve of a missing or non-executable file with
        // ENOENT/EACCES and the caller carries on: execvp tries the next PATH
        // entry. The runtime replaced itself first and only the new image
        // found the file missing ("lxrun: open ...: No such file or
        // directory"), which ended the process: coreutils' env, running a
        // "#!/usr/bin/env python3" script, died on /usr/local/bin/python3
        // before it reached /usr/bin/python3 (MEASURED, Heroic's legendary,
        // benchmarks/stage24-heroic.txt). So the file is checked here, before
        // the exec. Not the names lxrt_execve answers itself: bwrap (there
        // is none; its plan is interpreted) and /proc/<self>/exe.
        const char *gp = (const char *)a0;
        const char *gb = gp ? strrchr(gp, '/') : NULL;
        gb = gb ? gb + 1 : gp;
        if (gp && *gp && strcmp(gb, "bwrap") && strcmp(gb, "srt-bwrap") &&
            strncmp(gp, "/proc/", 6)) {
            const char *hp = translate_follow(gp);
            struct stat est;
            if (stat(hp, &est) != 0) {
                // Not in this root. The new image still takes a program
                // that exists only on the host, by its host path (main.c,
                // resolve_program): tools/steamarm-fex-proton execs SteamARM's
                // FEX that way from the ARM64 client's root, and this check
                // answered ENOENT for it ("FEX-gb: No such file or
                // directory", every game and d3ddriverquery64.exe; MEASURED,
                // benchmarks/stage50). Only an ELF: macOS's own
                // /usr/local/bin/python3 is nothing a guest can run, and
                // execvp has to go on to the next PATH entry.
                int e = errno;
                unsigned char magic[4] = {0};
                int hfd = gp[0] == '/' ? open(gp, O_RDONLY | O_CLOEXEC) : -1;
                ssize_t got = hfd >= 0 ? read(hfd, magic, 4) : -1;
                if (hfd >= 0) close(hfd);
                if (got != 4 || memcmp(magic, "\x7f" "ELF", 4) != 0 || stat(gp, &est) != 0) {
                    if (g_trace)
                        fprintf(lxrt_trace_stream(), "[lxrt]    execve \"%s\" (host \"%s\"): %s\n",
                                gp, hp, strerror(e));
                    ret = LERR(e);
                    break;
                }
                hp = gp;
            }
            if (S_ISDIR(est.st_mode) || access(hp, X_OK) != 0) { ret = LERR(EACCES); break; }
        }
        ret = lxrt_execve(gp, (char *const *)a1, (char *const *)a2);
        break;
    }
    case LNR_wait4:
        ret = lxrt_wait4((int)a0, (int *)a1, (int)a2, (void *)a3);
        break;
    case LNR_clone3:
        ret = lxrt_clone3((const void *)a0, a1, r);
        break;
    case LNR_madvise:
        // FEX hands a guest's madvise straight through with the guest's
        // address (its handler is excluded from the guest-base argument
        // translation, GuestBaseThunk.h, as if its allocator owned it; it
        // does not). Below 4 GiB that is a guest address -- no host memory
        // lives there -- so it is the window's: base + address, as for the
        // pointer arguments in gbase.c. Without this MADV_DONTNEED on a
        // 32-bit guest's or a 64-bit low-window mapping was a silent no-op,
        // and ART, which clears heap regions that way, read old objects back
        // where Linux gives zeros (MEASURED with tests/android/x86_lowwin.c,
        // benchmarks/stage25-art-x86-fex.txt).
        // A range that runs past 4 GiB (64-bit window) is two host ranges:
        // the window's part, then the identity part from 4 GiB on.
        if (lxrt_gbase() && a0 && a0 < (1ull << 32)) {
            uint64_t low = a1 <= (1ull << 32) - a0 ? a1 : (1ull << 32) - a0;
            ret = do_madvise(a0 + lxrt_gbase(), low, (int)a2);
            if (ret == 0 && a1 > low)
                ret = do_madvise(1ull << 32, a1 - low, (int)a2);
            lxrt_memlog('a', a0 + lxrt_gbase(), a1, (long)a2, 0, ret);
            break;
        }
        ret = do_madvise(a0, a1, (int)a2);
        lxrt_memlog('a', a0, a1, (long)a2, 0, ret);
        break;
    case LNR_mincore: {
        // (addr, len, vec): one byte per guest page, bit 0 = resident. Linux
        // fails with ENOMEM if any page in the range is unmapped; Darwin's
        // mincore answers 0 for unmapped memory (measured, scratchpad
        // mincore_probe.c), so the mapped-ness has to be checked here. The
        // guest's page is the host's (AT_PAGESZ = LXRT_HOST_PAGE), so the
        // vector is per host page.
        if (a0 % LXRT_HOST_PAGE) { ret = LERR(EINVAL); break; }
        if (!a2) { ret = LERR(EFAULT); break; }
        uint64_t end = LXRT_ALIGN_UP(a0 + a1, LXRT_HOST_PAGE);
        unsigned char *vec = (unsigned char *)a2;
        ret = 0;
        for (uint64_t p = a0, i = 0; p < end; p += LXRT_HOST_PAGE, i++) {
            mach_vm_address_t ra = p;
            mach_vm_size_t rs = 0;
            vm_region_basic_info_data_64_t ri;
            mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
            mach_port_t obj = MACH_PORT_NULL;
            if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                               (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS
                || ra > p) {
                ret = LERR(ENOMEM);
                break;
            }
            char v = 0;
            if (mincore((void *)p, LXRT_HOST_PAGE, &v) != 0) v = 0;
            vec[i] = (v & MINCORE_INCORE) ? 1 : 0;
        }
        break;
    }
    case LNR_dup: {
        long nfd = ret_of(dup((int)a0));
        if (nfd >= 0)
            alias_fd((int)a0, (int)nfd);
        ret = nfd;
        break;
    }
    case LNR_dup3:
        // (oldfd, newfd, flags). Linux errors if old == new, unlike dup2.
        if ((int)a0 == (int)a1 || (a2 & ~0x80000ull)) {     // only O_CLOEXEC is a flag
            ret = LERR(EINVAL);
        } else if (fcntl((int)a0, F_GETFD) < 0) {
            // A bad oldfd leaves newfd alone: its module state must survive.
            ret = LERR(EBADF);
        } else {
            // dup2 closes newfd implicitly: whatever it used to be must be
            // released exactly as an explicit close() would, or a dead epoll
            // registration or eventfd survives on the recycled number.
            forget_fd((int)a1);
            ret = ret_of(dup2((int)a0, (int)a1));
            if (ret >= 0) {
                alias_fd((int)a0, (int)a1);
                // Linux dup3(O_CLOEXEC); Darwin's dup2 always clears it.
                if (a2 & 0x80000)
                    fcntl((int)a1, F_SETFD, FD_CLOEXEC);
            }
        }
        break;
    case LNR_futex:
        // REQUEUE / CMP_REQUEUE / WAKE_OP and the PI operations live in
        // futex_ops.c; everything else stays in thread.c.
        if (lxrt_futex_ext_handles((int)a1)) {
            ret = lxrt_futex_ext((uint32_t *)a0, (int)a1, (uint32_t)a2, a3,
                                 (uint32_t *)a4, (uint32_t)a5);
            if (ret != LXRT_FUTEX_NOT_HANDLED)
                break;
        }
        ret = lxrt_futex((uint32_t *)a0, (int)a1, (uint32_t)a2, a3,
                         (uint32_t *)a4, (uint32_t)a5);
        break;
    case LNR_sched_getaffinity: {
        if (lxrt_ids_on() && a0 &&
            lxrt_ids_target_pid((int)a0) < 0) { ret = LERR(ESRCH); break; }
        // (pid, cpusetsize, mask). Report every core as available; Darwin does
        // not expose affinity and glibc only uses this to size thread pools.
        unsigned long *mask = (unsigned long *)a2;
        size_t sz = (size_t)a1;
        if (!mask || sz < sizeof(unsigned long)) { ret = LERR(EINVAL); break; }
        memset(mask, 0, sz);
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        for (long c = 0; c < ncpu && (size_t)(c / 8) < sz; c++)
            mask[c / (8 * sizeof(unsigned long))] |=
                1UL << (c % (8 * sizeof(unsigned long)));
        ret = (long)sizeof(unsigned long);
        break;
    }
    case 122:   // sched_setaffinity: Darwin has no affinity; accept, as a
        ret = lxrt_ids_on() && a0 && lxrt_ids_target_pid((int)a0) < 0
            ? LERR(ESRCH) : 0;
        break;
    case 114: { // clock_getres(clk, res): every clock here is the nanosecond
        // counter (coarse ones included), so 1 ns is the true resolution.
        struct { int64_t s, ns; } *r = (void *)a1;
        if ((long)a0 > 11 && (long)a0 >= 0) { ret = LERR(EINVAL); break; }
        if (r) { r->s = 0; r->ns = 1; }
        ret = 0;
        break;
    }
    case 223:   // fadvise64(fd, off, len, advice): advice only
        ret = fcntl((int)a0, F_GETFD) < 0 ? LERR(EBADF) : 0;
        break;
    case 264:   // name_to_handle_at: no file handles on Darwin; Linux gives
        ret = LERR(EOPNOTSUPP);   // EOPNOTSUPP for filesystems without them
        break;
    case 157:   // setsid
        ret = ret_of(setsid());
        if (ret > 0 && lxrt_ids_on()) ret = lxrt_ids_pid();
        break;
    case 293:   // rseq: glibc registers it opportunistically; ENOSYS is its
        ret = LERR(ENOSYS);   // documented "not available" answer
        break;
    case 436: { // close_range(first, last, flags)
        // CLOSE_RANGE_CLOEXEC (4) marks, otherwise each descriptor is closed
        // the way close() does it, so the emulated kinds drop their state.
        // CLOSE_RANGE_UNSHARE (2) is moot: descriptor tables are per process.
        unsigned first = (unsigned)a0, last = (unsigned)a1, fl = (unsigned)a2;
        if (first > last || (fl & ~6u)) { ret = LERR(EINVAL); break; }
        long max = sysconf(_SC_OPEN_MAX);
        if (max < 0 || max > 65536) max = 65536;
        if (last >= (unsigned long)max) last = (unsigned)max - 1;
        for (unsigned fd = first; fd <= last && fd < (unsigned)max; fd++) {
            if ((int)fd == g_trace_raw_fd || fcntl((int)fd, F_GETFD) < 0)
                continue;
            if (fl & 4) fcntl((int)fd, F_SETFD, FD_CLOEXEC);
            else guest_close((int)fd);
        }
        ret = 0;
        break;
    }
    case LNR_membarrier:
        ret = lxrt_membarrier((int)a0, (unsigned)a1);
        break;
    case LNR_tgkill:
        if (a2 == 6 && (int)a0 == lxrt_ids_pid())
            abort_backtrace(r);
        ret = lxrt_tgkill((int)a0, (int)a1, (int)a2);
        break;
    case 240: // rt_tgsigqueueinfo(tgid, tid, sig, siginfo)
    case 138: // rt_sigqueueinfo(pid, sig, siginfo)
        // Sent like tgkill/kill; the caller's siginfo (si_code, si_value)
        // is not carried, the handler sees what tgkill gives. bionic's
        // crash handler (debuggerd_handler.cpp, resend_signal) re-raises a
        // fatal signal this way after resetting it to SIG_DFL; ENOSYS sent
        // it into async_safe_fatal and every Android abort ended as
        // _exit(127) instead of SIGABRT (stage 25).
        // The caller's siginfo travels with a signal to this process
        // (lxrt_sigqueueinfo): FEX's seccomp emulation sends SIGSYS so.
        if (!a2 && nr == 240) { ret = LERR(EFAULT); break; }
        ret = nr == 240 ? lxrt_sigqueueinfo((int)a0, (int)a1, (int)a2, (const void *)a3)
                        : lxrt_sigqueueinfo((int)a0, 0, (int)a1, (const void *)a2);
        break;
    case LNR_rt_sigaction:
        ret = lxrt_rt_sigaction((int)a0, (const void *)a1, (void *)a2, (size_t)a3);
        break;
    case LNR_sigaltstack:
        ret = lxrt_sigaltstack((const void *)a0, (void *)a1);
        break;
    case LNR_setitimer:
        ret = do_setitimer((int)a0, (const void *)a1, (void *)a2);
        break;
    case LNR_getitimer:
        ret = do_getitimer((int)a0, (void *)a1);
        break;
    case LNR_ppoll:
        if (a0 && lxrt_binder_any()) {
            const struct { int fd; short ev, rev; } *pf = (const void *)a0;
            for (uint64_t i = 0; i < a1 && i < 4096; i++)
                if (lxrt_binder_is(pf[i].fd))
                    lxrt_binder_polled(pf[i].fd);
        }
        ret = do_ppoll(a0, a1, a2, a3);
        break;
    case LNR_pselect6:
        ret = do_pselect6(a0, a1, a2, a3, a4, a5);
        break;
    case LNR_rt_sigsuspend: {
        sigset_t set;
        sigemptyset(&set);
        if (a0) {
            uint64_t lmask = *(const uint64_t *)a0;
            for (int i = 1; i < 32; i++)
                if (lmask & (1ull << (i - 1))) {
                    int d = lxrt_signo_to_darwin(i);
                    if (d)
                        sigaddset(&set, d);
                }
        }
        // For the duration, this thread accepts what the suspend mask lets
        // through (signal.c aims stray process-directed signals by it).
        uint64_t noted = lxrt_thread_noted_mask();
        uint64_t suspend_mask = a0 ? *(const uint64_t *)a0 : 0;
        lxrt_thread_note_mask(suspend_mask);
        // What a signalfd's shared queue holds of the signals this mask lets
        // through is this thread's now (signal.c): pending here, the suspend
        // returns for it at once.
        lxrt_signal_raise_unqueued(noted & ~suspend_mask);
        sigsuspend(&set);
        lxrt_thread_note_mask(noted);
        ret = LERR(EINTR);   // sigsuspend always returns -1/EINTR
        break;
    }
    case LNR_kill:
        ret = lxrt_kill((int)a0, (int)a1);
        break;
    case LNR_rt_sigreturn:
        // Never reached: a guest handler returns to lxrt_sigreturn_entry, which
        // restores the interrupted context directly. Reaching here means a
        // guest issued the syscall by hand.
        ret = LERR(ENOSYS);
        break;
    case 136:                                           // rt_sigpending
        ret = lxrt_rt_sigpending((uint64_t *)a0, (size_t)a1);
        break;
    case LNR_rt_sigprocmask:
        ret = lxrt_rt_sigprocmask((int)a0, (const uint64_t *)a1,
                                  (uint64_t *)a2, (size_t)a3);
        break;
    case LNR_exit:
        // A thread exiting, not the process. glibc's pthread_join blocks on
        // the CLONE_CHILD_CLEARTID write this performs.
        lxrt_thread_exit((int)a0);
    case LNR_exit_group:
        if (getenv("LXRT_TLS_KEEP_LOG") && lxrt_tlskeep_fixups())
            fprintf(lxrt_trace_stream(), "[lxrt] pid %d: %lu kept-TLS faults fixed (runtime/tls.c)\n",
                    (int)getpid(), lxrt_tlskeep_fixups());
        lxrt_sigstats_flush();
        lxrt_sysv_exit();
        lxrt_proc_cleanup();
        lxrt_ids_release_process();
        _exit((int)a0);
    default:
        atomic_fetch_add(&g_unimplemented, 1);
        atomic_store(&g_last_unimplemented, nr);
        {
            // LXRT_REPORT_ENOSYS=1: name each missing call once per process,
            // without the cost of a full trace.
            static int report = -1;
            static _Atomic uint64_t seen[8];
            if (report < 0) report = getenv("LXRT_REPORT_ENOSYS") ? 1 : 0;
            if (report && !g_trace && nr >= 0 && nr < 512 &&
                !(atomic_fetch_or(&seen[nr / 64], 1ull << (nr % 64)) & (1ull << (nr % 64))))
                fprintf(stderr, "[lxrt] pid %d: unimplemented syscall %ld (0x%llx, 0x%llx, 0x%llx) -> -ENOSYS\n",
                        (int)getpid(), nr, (unsigned long long)a0, (unsigned long long)a1,
                        (unsigned long long)a2);
        }
        if (g_trace)
            fprintf(lxrt_trace_stream(), "[lxrt] unimplemented syscall %ld"
                            "(0x%llx, 0x%llx, 0x%llx) -> -ENOSYS\n",
                    nr, (unsigned long long)a0, (unsigned long long)a1,
                    (unsigned long long)a2);
        ret = LERR(ENOSYS);
        break;
    }

    // SA_RESTART. A blocking syscall the runtime makes on the guest's behalf
    // (flock, read, wait4, ...) comes back EINTR from Darwin when a signal
    // interrupts it, and the guest's handler has already run by then. Linux
    // would have restarted the call if every handler that ran asked for
    // SA_RESTART -- Steam's webhelper gave up its shared-memory lock ("Error 4
    // locking shared memory file") after one such interruption. Only the
    // calls Linux restarts: never poll/select/epoll_wait/nanosleep/sigsuspend
    // and friends, which report EINTR whatever the handler asked for.
    // A signal that ran no guest handler at all (lxrt_sig_during_syscall 0:
    // the realtime carrier landing on a thread with nothing queued for it)
    // interrupts nothing on Linux, so the same calls go on as if SA_RESTART
    // were set. The timed waits continue inside their own implementations
    // (nanosleep, futex), with what is left of their time.
    if (ret == LERR(EINTR) && lxrt_sig_during_syscall <= 1) {
        bool restartable = false;
        switch (nr) {
        case 63: case 64: case 65: case 66: case 67: case 68:   // read/write/v, pread/pwrite
        case 32: case 25: case 29:                              // flock, fcntl, ioctl
        case 260: case 95:                                      // wait4, waitid
        case 56: case 202: case 242:                            // openat, accept, accept4
        case 206: case 207: case 211: case 212:                 // sendto, recvfrom, sendmsg, recvmsg
            restartable = true;
            break;
        case 98:                                                // futex WAIT without a timeout
            restartable = ((a1 & 0x7f) == 0 || (a1 & 0x7f) == 9) && a3 == 0;
            break;
        }
        if (restartable) {
            if (g_trace)
                fprintf(lxrt_trace_stream(), "[lxrt] %d syscall %ld interrupted, restarting (%s)\n",
                        (int)getpid(), nr, lxrt_sig_during_syscall ? "SA_RESTART" : "no guest handler ran");
            goto restart;
        }
    }

aids_done:
    if (g_trace && nr != LNR_clock_gettime)
        fprintf(lxrt_trace_stream(), "[lxrt] %d/%d syscall %ld -> %ld\n", (int)getpid(), lxrt_gettid(), nr, ret);

    lxrt_sig_during_syscall = outer_sig_during_syscall;
    r->x[0] = (uint64_t)ret;
}
