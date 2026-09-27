// A synthetic /proc.
//
// ARCHITECTURE_TARGET §3.3 listed this as needed and it is: FEXServer dies on
// `read_symlink("/proc/self/exe")` before doing anything else, and Steam and
// pressure-vessel read /proc heavily.
//
// It is materialised as real files in a directory the runtime owns, and
// absolute guest paths under /proc/self (or /proc/<our pid>) are redirected
// there. That means an ordinary open/read/readlink works with no special cases
// in the syscall paths -- only a prefix substitution.
//
// Files whose content changes are regenerated on each lookup; the rest are
// written once.

#include "lxrt.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <ftw.h>
#include <signal.h>
#include <time.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

static char g_dir[512];
static void regenerate_fds(void);
static void regenerate_tasks(void);
static char g_exe[1024];
static bool g_ready;

// This procfs lives in real files, and every write is a filesystem event the
// system's fseventsd records. Rewriting unchanged files and re-creating whole
// fd directories on every lookup pushed fseventsd to 5.9 GB and, with other
// memory pressure, the Mac into a watchdog panic (MEASURED 2026-09-27). So:
// content and links are only written when they differ from what is there.
bool lxrt_file_same(const char *path, const void *data, size_t len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return false;
    char buf[8192];
    size_t off = 0;
    bool same = true;
    for (;;) {
        ssize_t r = read(fd, buf, sizeof buf);
        if (r < 0) { same = false; break; }
        if (r == 0) break;
        if (off + (size_t)r > len || memcmp(buf, (const char *)data + off, (size_t)r) != 0) { same = false; break; }
        off += (size_t)r;
    }
    close(fd);
    return same && off == len;
}

void lxrt_link_set(const char *target, const char *link)
{
    char cur[PATH_MAX];
    ssize_t n = readlink(link, cur, sizeof cur - 1);
    if (n >= 0) {
        cur[n] = '\0';
        if (strcmp(cur, target) == 0)
            return;
    }
    unlink(link);
    symlink(target, link);
}

static void path_write_if_changed(const char *path, const char *data, size_t len)
{
    if (lxrt_file_same(path, data, len))
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    ssize_t rc = write(fd, data, len);
    (void)rc;
    close(fd);
}

static void write_file(const char *name, const char *data, size_t len)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", g_dir, name);
    if (lxrt_file_same(path, data, len))
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return;
    ssize_t rc = write(fd, data, len);
    (void)rc;
    close(fd);
}

// /proc/self/maps, generated from the real address space. FEX parses this to
// find where it and its guest are mapped, so the addresses have to be true
// even though the permissions and names are approximations.
static void regenerate_maps(void)
{
    char *buf = malloc(256 * 1024);
    if (!buf)
        return;
    size_t used = 0;

    mach_vm_address_t addr = 0;
    for (;;) {
        mach_vm_size_t size = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        natural_t depth = 0;
        kern_return_t kr = mach_vm_region_recurse(mach_task_self(), &addr, &size,
                                                  &depth,
                                                  (vm_region_recurse_info_t)&info,
                                                  &count);
        if (kr != KERN_SUCCESS || used + 256 > 256 * 1024)
            break;
        used += (size_t)snprintf(buf + used, 256 * 1024 - used,
                                 "%012llx-%012llx %c%c%c%c 00000000 00:00 0\n",
                                 (unsigned long long)addr,
                                 (unsigned long long)(addr + size),
                                 (info.protection & VM_PROT_READ) ? 'r' : '-',
                                 (info.protection & VM_PROT_WRITE) ? 'w' : '-',
                                 (info.protection & VM_PROT_EXECUTE) ? 'x' : '-',
                                 (info.share_mode == SM_SHARED) ? 's' : 'p');
        addr += size;
    }
    write_file("maps", buf, used);
    free(buf);
}

// /proc/self/fd, as a directory of symlinks. Rebuilt on every lookup because
// the fd table changes constantly; F_GETPATH gives the target where Darwin
// knows one, and sockets and pipes get a descriptive name the way Linux does.
static void regenerate_fds(void)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/fd", g_dir);
    mkdir(dir, 0700);

    int maxfd = (int)sysconf(_SC_OPEN_MAX);
    if (maxfd < 0 || maxfd > 4096)
        maxfd = 4096;
    // Stale entries: only the descriptors that are closed now.
    DIR *existing = opendir(dir);
    if (existing) {
        struct dirent *e;
        while ((e = readdir(existing))) {
            if (e->d_name[0] == '.')
                continue;
            int n = atoi(e->d_name);
            if (n >= 0 && n < maxfd && fcntl(n, F_GETFD) >= 0)
                continue;
            char p[1200];
            snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
            unlink(p);
        }
        closedir(existing);
    }
    for (int fd = 0; fd < maxfd; fd++) {
        if (fcntl(fd, F_GETFD) < 0)
            continue;
        char target[1024];
        if (lxrt_pathfd_path(fd))
            snprintf(target, sizeof target, "%s", lxrt_pathfd_path(fd));
        else if (fcntl(fd, F_GETPATH, target) != 0 &&
            !lxrt_procpid_fd_target(getpid(), fd, target, sizeof target))
            snprintf(target, sizeof target, "anon_inode:[fd%d]", fd);
        char link[1200];
        snprintf(link, sizeof link, "%s/%d", dir, fd);
        lxrt_link_set(target, link);
    }
}

// /proc/self/fd/<n> alone: refresh that one link. pressure-vessel resolves
// thousands of descriptors this way (readlink of /proc/self/fd/N after every
// openat), and rebuilding the whole directory each time -- an fcntl on every
// possible descriptor plus an unlink and a symlink per open one -- was most
// of its minute and a half of start-up (MEASURED with sample(1)).
static void regenerate_one_fd(int fd)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/fd", g_dir);
    char link[1200];
    snprintf(link, sizeof link, "%s/%d", dir, fd);
    if (fcntl(fd, F_GETFD) < 0) {
        unlink(link);                   // closed: the lookup gets ENOENT
        return;
    }
    mkdir(dir, 0700);
    char target[1024];
    if (lxrt_pathfd_path(fd))
        snprintf(target, sizeof target, "%s", lxrt_pathfd_path(fd));
    else if (fcntl(fd, F_GETPATH, target) != 0 &&
        !lxrt_procpid_fd_target(getpid(), fd, target, sizeof target))
        snprintf(target, sizeof target, "anon_inode:[fd%d]", fd);
    lxrt_link_set(target, link);
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st; (void)ftw;
    if (flag == FTW_DP) rmdir(path);
    else unlink(path);
    return 0;
}

static void remove_tree(const char *dir)
{
    nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
}

// A process that exits removes its directory (lxrt_proc_cleanup); one that is
// killed cannot. Every so often -- at most once a minute, whoever starts
// then -- the directories of pids that no longer exist are swept, or /tmp
// fills with thousands of them over a long session.
static void sweep_dead(const char *tmp)
{
    char mark[600];
    snprintf(mark, sizeof mark, "%s/lxrt-proc-sweep", tmp);
    struct stat st;
    time_t now = time(NULL);
    if (stat(mark, &st) == 0 && now - st.st_mtime < 60)
        return;
    int fd = open(mark, O_WRONLY | O_CREAT, 0600);
    if (fd >= 0) { futimens(fd, NULL); close(fd); }
    DIR *d = opendir(tmp);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "lxrt-proc-", 10) != 0)
            continue;
        char *end;
        long pid = strtol(e->d_name + 10, &end, 10);
        if (*end || pid <= 0 || pid == getpid())
            continue;
        if (kill((pid_t)pid, 0) == 0 || errno != ESRCH)
            continue;
        char path[900];
        snprintf(path, sizeof path, "%s/%s", tmp, e->d_name);
        remove_tree(path);
    }
    closedir(d);
}

void lxrt_proc_cleanup(void)
{
    if (g_dir[0] && strstr(g_dir, "/lxrt-proc-"))
        remove_tree(g_dir);
}

void lxrt_proc_init(const char *exe_path, int argc, char **argv)
{
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    sweep_dead(tmp);
    snprintf(g_dir, sizeof g_dir, "%s/lxrt-proc-%d", tmp, getpid());
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST)
        return;

    // The guest's own view of its path, not the host's: a guest that re-execs
    // on this must get something the runtime can resolve again.
    // A `self` symlink inside the directory makes /proc/self resolve for
    // anything that walks the tree rather than opening the path directly.
    char selflink[600];
    snprintf(selflink, sizeof selflink, "%s/self", g_dir);
    unlink(selflink);
    // Linux: readlink("/proc/self") is "<pid>", and realpath() walks through
    // it -- "." made realpath("/proc/self/exe") come out as "/proc/exe", and
    // steam-runtime-tools could not find its own executable. "<pid>" -> "."
    // keeps the tree walkable.
    {
        char pidname[32], pidlink[640];
        snprintf(pidname, sizeof pidname, "%d", getpid());
        snprintf(pidlink, sizeof pidlink, "%s/%s", g_dir, pidname);
        unlink(pidlink);
        symlink(".", pidlink);
        symlink(pidname, selflink);
    }

    snprintf(g_exe, sizeof g_exe, "%s", exe_path ? exe_path : "/unknown");
    char link[1024];
    snprintf(link, sizeof link, "%s/exe", g_dir);
    unlink(link);
    symlink(g_exe, link);

    char cmdline[4096];
    size_t n = 0;
    for (int i = 0; i < argc && n + 1 < sizeof cmdline; i++) {
        size_t l = strlen(argv[i]);
        if (n + l + 1 >= sizeof cmdline)
            break;
        memcpy(cmdline + n, argv[i], l);
        n += l;
        cmdline[n++] = '\0';
    }
    write_file("cmdline", cmdline, n);

    char status[512];
    int sl = snprintf(status, sizeof status,
                      "Name:\t%s\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\n"
                      "PPid:\t%d\nUid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\n"
                      "Threads:\t1\n",
                      argc > 0 ? argv[0] : "lxrt", getpid(), getpid(), getppid(),
                      getuid(), getuid(), getuid(), getuid(),
                      getgid(), getgid(), getgid(), getgid());
    write_file("status", status, (size_t)sl);

    g_ready = true;
}

// Returns a host path for a /proc path the runtime synthesises, or NULL if it
// does not provide that file. NULL means the caller should fall through to the
// normal translation, which will fail with ENOENT -- the honest answer.
// The guest's own executable path, as the guest names it. FEX re-execs
// itself as execve("/proc/self/exe", ...) to run the next x86 program; the
// child runtime must open the real image, not the literal magic path.
// /proc/self/task: one directory per guest thread, each with status and comm.
// Chromium's thread_helpers.cc opendir()s it and CHECKs the result, which is
// how steamwebhelper's subprocesses died before this existed.
void lxrt_proc_thread_gone(int tid)
{
    if (!g_ready || tid <= 0 || tid == getpid())
        return;
    char sub[700];
    snprintf(sub, sizeof sub, "%s/task/%d", g_dir, tid);
    remove_tree(sub);
}

static void regenerate_tasks(void)
{
    char dir[600];
    snprintf(dir, sizeof dir, "%s/task", g_dir);
    mkdir(dir, 0700);
    int tids[512];
    int n = lxrt_thread_list(tids, 512);
    if (n > 512) n = 512;
    // The main thread may not be registered yet; it is always present.
    bool have_main = false;
    for (int i = 0; i < n; i++) if (tids[i] == getpid()) have_main = true;
    if (!have_main && n < 512) tids[n++] = getpid();
    // Entries of threads that are gone go too.
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            char *end;
            long t = strtol(e->d_name, &end, 10);
            if (*end || t <= 0)
                continue;
            bool live = false;
            for (int i = 0; i < n && !live; i++) live = tids[i] == t;
            if (!live) {
                char sub[900];
                snprintf(sub, sizeof sub, "%s/%s", dir, e->d_name);
                remove_tree(sub);
            }
        }
        closedir(d);
    }
    for (int i = 0; i < n; i++) {
        char sub[700];
        snprintf(sub, sizeof sub, "%s/%d", dir, tids[i]);
        mkdir(sub, 0700);
        char f[800], b[1024];
        const char *nm = g_exe[0] ? (strrchr(g_exe, '/') ? strrchr(g_exe, '/') + 1 : g_exe) : "lxrt";
        int bl = snprintf(b, sizeof b, "%s\n", nm);
        snprintf(f, sizeof f, "%s/comm", sub);
        path_write_if_changed(f, b, (size_t)bl);
        bl = snprintf(b, sizeof b, "Name:\t%s\nState:\tS (sleeping)\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\n"
                      "Uid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\nThreads:\t%d\n",
                      strrchr(g_exe, '/') ? strrchr(g_exe, '/') + 1 : g_exe, getpid(), tids[i], getppid(),
                      getuid(), getuid(), getuid(), getuid(), getgid(), getgid(), getgid(), getgid(), n);
        snprintf(f, sizeof f, "%s/status", sub);
        path_write_if_changed(f, b, (size_t)bl);
    }
}

// After fork(), the child is a new pid with the parent's g_dir: every
// /proc/self regeneration it did went into the PARENT's directory (maps, fd,
// task, status all describing the child), and the parent read them back as
// its own. Give the child its own directory; the exe and cmdline stay.
void lxrt_proc_after_fork(void)
{
    if (!g_ready)
        return;
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    char cmdline_src[600];
    snprintf(cmdline_src, sizeof cmdline_src, "%s/cmdline", g_dir);
    snprintf(g_dir, sizeof g_dir, "%s/lxrt-proc-%d", tmp, getpid());
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST)
        return;
    char selflink[600];
    snprintf(selflink, sizeof selflink, "%s/self", g_dir);
    unlink(selflink);
    // Linux: readlink("/proc/self") is "<pid>", and realpath() walks through
    // it -- "." made realpath("/proc/self/exe") come out as "/proc/exe", and
    // steam-runtime-tools could not find its own executable. "<pid>" -> "."
    // keeps the tree walkable.
    {
        char pidname[32], pidlink[640];
        snprintf(pidname, sizeof pidname, "%d", getpid());
        snprintf(pidlink, sizeof pidlink, "%s/%s", g_dir, pidname);
        unlink(pidlink);
        symlink(".", pidlink);
        symlink(pidname, selflink);
    }
    char link[1024];
    snprintf(link, sizeof link, "%s/exe", g_dir);
    unlink(link);
    symlink(g_exe, link);
    // cmdline: copy the parent's.
    FILE *in = fopen(cmdline_src, "rb");
    if (in) {
        char buf[4096]; size_t n = fread(buf, 1, sizeof buf, in); fclose(in);
        write_file("cmdline", buf, n);
    }
    char status[512];
    const char *base = strrchr(g_exe, '/') ? strrchr(g_exe, '/') + 1 : g_exe;
    int sl = snprintf(status, sizeof status,
                      "Name:\t%s\nState:\tR (running)\nTgid:\t%d\nPid:\t%d\n"
                      "PPid:\t%d\nUid:\t%d\t%d\t%d\t%d\nGid:\t%d\t%d\t%d\t%d\n"
                      "Threads:\t1\n", base, getpid(), getpid(), getppid(),
                      getuid(), getuid(), getuid(), getuid(),
                      getgid(), getgid(), getgid(), getgid());
    write_file("status", status, (size_t)sl);
}

// prctl(PR_SET_MM, PR_SET_MM_MAP): the process moved its argv (FEX does,
// so /proc/<pid>/cmdline shows the x86 program rather than FEX itself).
void lxrt_proc_set_cmdline(const char *args, size_t len)
{
    if (g_ready && args && len)
        write_file("cmdline", args, len);
}

const char *lxrt_proc_exe_path(void)
{
    return g_ready ? g_exe : NULL;
}

// The reverse of lxrt_proc_translate, for readlink: a host path inside the
// materialised /proc (what F_GETPATH gives for /proc/self/fd links) becomes
// the /proc path Linux would show. FEX recognises its emulated files --
// /proc/cpuinfo above all -- by exactly this readlink after the open; with
// the host path it served the Apple cpuinfo to x86 guests, and the Steam
// client concluded it ran on arm64 (MEASURED: it installed the ARM64 Proton
// builds and Steam Linux Runtime 4.0 - Arm64).
const char *lxrt_proc_untranslate(const char *host, char *out, size_t n)
{
    if (!host)
        return NULL;
    // Any process's materialised /proc: <tmpdir>/lxrt-proc-<pid>[/...]. The
    // link may be read by another process than the one that made it (a
    // child that inherited the descriptor).
    const char *m = strstr(host, "/lxrt-proc-");
    if (!m)
        return NULL;
    char *end = NULL;
    long owner = strtol(m + 11, &end, 10);
    if (end == m + 11 || (*end != '/' && *end != '\0'))
        return NULL;
    const char *rest = *end ? end + 1 : "";
    if (!*rest) {
        snprintf(out, n, "/proc");
        return out;
    }
    // Other guest processes live under .p/<pid>/ (procpid.c).
    if (strncmp(rest, ".p/", 3) == 0) {
        snprintf(out, n, "/proc/%s", rest + 3);
        return out;
    }
    if (strncmp(rest, ".net", 4) == 0 && (rest[4] == '/' || !rest[4])) {
        snprintf(out, n, "/proc/net%s", rest + 4);
        return out;
    }
    static const char *const per_process[] = {
        "cmdline", "status", "maps", "environ", "exe", "fd", "task", "mountinfo",
        "statm", "comm", "auxv", "limits", "cgroup", "self",
    };
    size_t first = strcspn(rest, "/");
    for (unsigned i = 0; i < sizeof per_process / sizeof per_process[0]; i++)
        if (strlen(per_process[i]) == first && strncmp(rest, per_process[i], first) == 0) {
            if (strcmp(per_process[i], "self") == 0)
                snprintf(out, n, "/proc/%ld%s", owner, rest + first);
            else
                snprintf(out, n, "/proc/%ld/%s", owner, rest);
            return out;
        }
    snprintf(out, n, "/proc/%s", rest);
    return out;
}

const char *lxrt_proc_translate(const char *path)
{
    if (!g_ready || !path)
        return NULL;
    if (strcmp(path, "/proc") != 0 && strncmp(path, "/proc/", 6) != 0)
        return NULL;

    // `/proc` itself, and `/proc/` -- FEX opens it to enumerate processes and
    // warns loudly if it cannot.
    if (path[5] == '\0' || (path[5] == '/' && path[6] == '\0')) {
        // A program that opens /proc itself will walk it with openat(fd,
        // "self/task") -- relative paths this translator never sees -- so
        // everything it might look for has to exist by then. Chromium's
        // sandbox helpers do exactly that (PCHECK on the result).
        regenerate_fds();
        regenerate_tasks();
        lxrt_procpid_list(g_dir);     // the other guest processes, /proc/net
        static _Thread_local char root[600];
        snprintf(root, sizeof root, "%s", g_dir);
        return root;
    }

    // Files proc_ext.c synthesises (meminfo, cpuinfo, mountinfo, sys/*, ...)
    // are handled there; this file keeps the per-process ones it already owns.
    const char *ext = lxrt_proc_ext_translate(path, g_dir);
    if (ext)
        return ext;

    const char *rest = path + 6;
    // /proc/net/...: the socket tables of every guest process (procpid.c).
    {
        const char *net = lxrt_procnet_translate(rest, g_dir);
        if (net)
            return net;
    }
    // The directory itself: /proc/self and /proc/<our pid>. A stat() of it
    // is how a program checks that /proc is mounted at all.
    {
        char mypid[32];
        snprintf(mypid, sizeof mypid, "%d", getpid());
        if (strcmp(rest, "self") == 0 || strcmp(rest, mypid) == 0) {
            regenerate_fds();
            regenerate_tasks();
            // /proc/self is the symlink (readlink gives "<pid>"); /proc/<pid>
            // is the directory itself, so glibc's realpath(), which calls
            // readlink on every component, sees EINVAL there and goes on.
            static _Thread_local char selfdir[600];
            if (strcmp(rest, "self") == 0)
                snprintf(selfdir, sizeof selfdir, "%s/self", g_dir);
            else
                snprintf(selfdir, sizeof selfdir, "%s", g_dir);
            return selfdir;
        }
    }
    if (strncmp(rest, "self/", 5) == 0) {
        rest += 5;
    } else {
        // /proc/<pid>/ for our own pid is the same thing.
        char mypid[32];
        int k = snprintf(mypid, sizeof mypid, "%d/", getpid());
        if (strncmp(rest, mypid, (size_t)k) != 0)
            return lxrt_procpid_translate(rest, g_dir);   // another guest process
        rest += k;
    }
    {
        const char *net = lxrt_procnet_translate(rest, g_dir);
        if (net)
            return net;
    }

    // /proc/self/root is the guest's "/": LXRT_ROOT (host "/" without one).
    // pressure-vessel-wrap opendir()s it before anything else.
    if (strncmp(rest, "root", 4) == 0 && (rest[4] == '\0' || rest[4] == '/')) {
        static _Thread_local char rootbuf[1024];
        const char *root = getenv("LXRT_ROOT");
        if (!root || !*root)
            root = "";
        int n = snprintf(rootbuf, sizeof rootbuf, "%s%s", root, rest[4] ? rest + 4 : "/");
        if (n <= 0 || (size_t)n >= sizeof rootbuf)
            return NULL;
        return rootbuf;
    }

    if (strcmp(rest, "maps") == 0) {
        regenerate_maps();
    } else if (strncmp(rest, "fd", 2) == 0 &&
               (rest[2] == '\0' || rest[2] == '/')) {
        char *end = NULL;
        long one = rest[2] == '/' && rest[3] >= '0' && rest[3] <= '9' ? strtol(rest + 3, &end, 10) : -1;
        if (one >= 0 && end && (*end == '\0' || *end == '/'))
            regenerate_one_fd((int)one);
        else
            regenerate_fds();
    } else if (strncmp(rest, "task", 4) == 0 &&
               (rest[4] == '\0' || rest[4] == '/')) {
        regenerate_tasks();
    } else if (strcmp(rest, "environ") == 0 && !getenv("LXRT_NO_ENVIRON")) {
        char ep[700];
        snprintf(ep, sizeof ep, "%s/environ", g_dir);
        lxrt_procpid_environ(getpid(), ep);
    } else if (strcmp(rest, "exe") != 0 && strcmp(rest, "cmdline") != 0 &&
               strcmp(rest, "status") != 0) {
        return NULL;
    }

    static _Thread_local char out[1024];
    snprintf(out, sizeof out, "%s/%s", g_dir, rest);
    return out;
}
