// Android ids (runtime/android_ids.h): the credentials, capabilities and
// namespace calls of a guest started with LXRT_ANDROID_IDS, checked against
// Linux's rules (kernel/sys.c, security/commoncap.c, kernel/capability.c).
// Each check prints "ok" or "MAL"; the last line is "== android ids: ok".
//
//   android_ids            run with LXRT_ANDROID_IDS=root (tests/elf/run.sh)
//   android_ids off        run WITHOUT it: the Mac's ids, nothing emulated
//   (android_ids after-exec <tmpdir> is the re-exec'd half of the run)
//
// What zygote does to become system_server, in order (com_android_internal_
// os_Zygote.cpp SpecializeCommon): unshare(CLONE_NEWNS) and a bind mount,
// PR_SET_KEEPCAPS, drop the bounding set, setgroups, setresgid, setresuid,
// capset; then what the process sees afterwards, across fork and execve,
// and what a peer sees of it over a socket (SO_PEERCRED).
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/capability.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static int bad;
static void check(int cond, const char *what)
{
    printf("  %s  %s\n", cond ? "ok " : "MAL", what);
    if (!cond) bad++;
}

static long sc(long n, long a, long b, long c) { long r = syscall(n, a, b, c); return r < 0 ? -errno : r; }
// The value of a "Key:\t..." line of a /proc status file.
static int status_line(const char *path, const char *key, char *out, size_t n)
{
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, strlen(key))) {
            char *v = line + strlen(key);
            while (*v == '\t' || *v == ' ') v++;
            v[strcspn(v, "\n")] = 0;
            snprintf(out, n, "%s", v);
            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}
static void caps(uint64_t *eff, uint64_t *prm, uint64_t *inh)
{
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct d[2];
    memset(d, 0, sizeof d);
    syscall(SYS_capget, &h, d);
    *eff = d[0].effective | (uint64_t)d[1].effective << 32;
    *prm = d[0].permitted | (uint64_t)d[1].permitted << 32;
    *inh = d[0].inheritable | (uint64_t)d[1].inheritable << 32;
}
static long setcaps(uint64_t eff, uint64_t prm, uint64_t inh)
{
    struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, 0 };
    struct __user_cap_data_struct d[2] = {
        { (uint32_t)eff, (uint32_t)prm, (uint32_t)inh },
        { (uint32_t)(eff >> 32), (uint32_t)(prm >> 32), (uint32_t)(inh >> 32) },
    };
    return syscall(SYS_capset, &h, d) == 0 ? 0 : -errno;
}
#define BIT(c) (1ull << (c))
#define ALL ((1ull << 41) - 1)

static int after_exec(const char *tmp)
{
    uint64_t e, p, i;
    caps(&e, &p, &i);
    uid_t r, ef, s;
    getresuid(&r, &ef, &s);
    check(r == 1000 && ef == 1000 && s == 1000, "after execve: uid 1000 (real, effective, saved)");
    gid_t g[8];
    int n = getgroups(8, g);
    check(n == 2 && g[0] == 1001 && g[1] == 3003, "after execve: groups 1001,3003");
    check(e == BIT(CAP_NET_ADMIN) && p == BIT(CAP_NET_ADMIN),
          "after execve: permitted = effective = the ambient set (NET_ADMIN), as for a non-root exec");
    check(prctl(PR_GET_KEEPCAPS) == 0, "after execve: keepcaps cleared");
    check(prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1, "after execve: no_new_privs kept");
    check(prctl(PR_GET_SECUREBITS) == 0x3, "after execve: securebits NOROOT and its lock kept (0x3)");
    // The bind the "zygote" made before exec is still there.
    char p2[512], buf[16] = {0};
    snprintf(p2, sizeof p2, "%s/b/file", tmp);
    int fd = open(p2, O_RDONLY);
    check(fd >= 0 && read(fd, buf, sizeof buf - 1) == 4 && !strcmp(buf, "bind"),
          "after execve: the bind mount made before it is still in place");
    if (fd >= 0) close(fd);
    printf("== android ids after exec: %s\n", bad ? "MAL" : "ok");
    return bad ? 1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 2 && !strcmp(argv[1], "after-exec"))
        return after_exec(argv[2]);
    if (argc > 1 && !strcmp(argv[1], "off")) {
        check(getuid() != 0, "off: getuid is the Mac user's");
        check(sc(SYS_setresuid, 0, 0, 0) == -EPERM, "off: setresuid(0) is EPERM");
        check(sc(SYS_unshare, CLONE_NEWNS, 0, 0) == -ENOSYS, "off: unshare(CLONE_NEWNS) is ENOSYS as before");
        printf("== android ids off: %s\n", bad ? "MAL" : "ok");
        return bad ? 1 : 0;
    }

    uint64_t e, p, i;
    uid_t r, ef, s;
    check(getuid() == 0 && geteuid() == 0 && getgid() == 0, "root: uid, euid, gid 0");
    caps(&e, &p, &i);
    check(e == ALL && p == ALL && i == 0, "root: every capability effective and permitted");

    char tmp[] = "/tmp/lxrt-aids-XXXXXX";
    check(mkdtemp(tmp) != NULL, "mkdtemp");
    char a[512], b[512], f[512];
    snprintf(a, sizeof a, "%s/a", tmp);
    snprintf(b, sizeof b, "%s/b", tmp);
    snprintf(f, sizeof f, "%s/a/file", tmp);
    mkdir(a, 0755);
    mkdir(b, 0755);
    int fd = open(f, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd >= 0) { write(fd, "bind", 4); close(fd); }

    // zygote: UnmountStorageOnInit, then a child's MountEmulatedStorage.
    check(sc(SYS_unshare, CLONE_NEWNS, 0, 0) == 0, "unshare(CLONE_NEWNS) with CAP_SYS_ADMIN");
    check(mount("rootfs", "/", NULL, MS_SLAVE | MS_REC, NULL) == 0, "mount(\"/\", MS_SLAVE|MS_REC): a propagation change");
    // tmpfs (zygote's app data isolation: MountAppDataTmpFs): an empty,
    // writable directory over b, with the mode and owner the options name;
    // b's own contents come back once it is unmounted.
    char bt[512], bm[512];
    snprintf(bm, sizeof bm, "%s/mark", b);
    fd = open(bm, O_CREAT | O_WRONLY, 0644);
    if (fd >= 0) close(fd);
    check(mount("tmpfs", b, "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, "uid=1000,gid=1000,mode=0751") == 0,
          "mount(tmpfs, \"uid=1000,gid=1000,mode=0751\")");
    struct stat ts;
    snprintf(bt, sizeof bt, "%s/new", b);
    check(stat(b, &ts) == 0 && (ts.st_mode & 07777) == 0751 && ts.st_uid == 1000 && ts.st_gid == 1000 &&
          access(bm, F_OK) != 0 && mkdir(bt, 0700) == 0, "the tmpfs: empty, mode 0751, owner 1000:1000, writable");
    check(umount2(b, 0) == 0 && access(bm, F_OK) == 0 && access(bt, F_OK) != 0,
          "umount2: the directory's own contents again");
    check(mount(a, b, NULL, MS_BIND | MS_REC, NULL) == 0, "mount(MS_BIND) of a directory");
    char bf[512], buf[16] = {0};
    snprintf(bf, sizeof bf, "%s/file", b);
    fd = open(bf, O_RDONLY);
    check(fd >= 0 && read(fd, buf, sizeof buf - 1) == 4 && !strcmp(buf, "bind"), "the bind shows the source's file");
    if (fd >= 0) close(fd);

    // chown with CAP_CHOWN: done, and the owner is what stat reports from
    // then on (virtual file ownership: an xattr on the Mac's file, which
    // stays the Mac user's). installd's fs_prepare_dir_strict needs this for
    // an app's existing data directory (benchmarks/stage28-android-apk.txt).
    char o[512], ol[512];
    snprintf(o, sizeof o, "%s/owned", tmp);
    snprintf(ol, sizeof ol, "%s/owned-link", tmp);
    fd = open(o, O_CREAT | O_WRONLY | O_TRUNC, 0444);            // read-only: Darwin's xattr wants write access
    if (fd >= 0) close(fd);
    symlink("owned", ol);
    struct stat st0, st1;
    stat(o, &st0);
    check(chown(o, 1234, 5678) == 0, "chown to another uid with CAP_CHOWN: 0");
    stat(o, &st1);
    check(st1.st_uid == 1234 && st1.st_gid == 5678, "... stat reports the new owner, 1234:5678");
    check((st1.st_mode & 07777) == 0444, "... of a read-only file, whose mode is unchanged");
    fd = open(o, O_RDONLY);
    check(fd >= 0 && fstat(fd, &st1) == 0 && st1.st_uid == 1234, "... fstat too");
    check(fd >= 0 && fchown(fd, (uid_t)-1, 42) == 0 && fstat(fd, &st1) == 0 && st1.st_uid == 1234 && st1.st_gid == 42,
          "fchown(-1, 42): only the group changes (1234:42)");
    if (fd >= 0) close(fd);
    // A socket's owner, as netd's resolver sets one on each AI_ADDRCONFIG
    // probe socket (the asking app's uid): succeeds, as on Linux.
    int us = socket(AF_INET, SOCK_DGRAM, 0);
    check(us >= 0 && fchown(us, 10144, (gid_t)-1) == 0, "fchown of a socket to an app's uid (netd's resolver): 0");
    if (us >= 0) close(us);
    check(lstat(ol, &st1) == 0 && st1.st_uid == st0.st_uid, "the symlink to it keeps its own owner (lstat)");
    check(stat(ol, &st1) == 0 && st1.st_uid == 1234, "... and stat through it shows the file's");
    check(lchown(ol, 2000, 2000) == 0 && lstat(ol, &st1) == 0 && st1.st_uid == 2000 && stat(o, &st0) == 0 && st0.st_uid == 1234,
          "lchown records the link's owner, not the file's");
    int dfd = open(tmp, O_RDONLY | O_DIRECTORY);
    check(dfd >= 0 && fstatat(dfd, "owned", &st1, 0) == 0 && st1.st_uid == 1234 && st1.st_gid == 42,
          "fstatat relative to a directory descriptor");
    check(dfd >= 0 && fchownat(dfd, "owned", 1000, 1000, 0) == 0 && stat(o, &st1) == 0 && st1.st_uid == 1000,
          "fchownat relative to a directory descriptor");
    if (dfd >= 0) close(dfd);
    struct statx sx;
    check(statx(AT_FDCWD, o, 0, STATX_UID | STATX_GID, &sx) == 0 && sx.stx_uid == 1000 && sx.stx_gid == 1000,
          "statx reports it");
    check(chown(o, 1234, 5678) == 0, "back to 1234:5678 for the unprivileged checks below");

    // SO_PEERCRED: a peer that became uid 2000 is seen as 2000.
    int sp[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sp);
    pid_t kid = fork();
    if (kid == 0) {
        close(sp[0]);
        if (sc(SYS_setresgid, 2000, 2000, 2000) || sc(SYS_setresuid, 2000, 2000, 2000)) _exit(3);
        char c = 'x';
        write(sp[1], &c, 1);
        read(sp[1], &c, 1);
        _exit(getuid() == 2000 ? 0 : 4);
    }
    close(sp[1]);
    char c;
    read(sp[0], &c, 1);
    struct ucred uc = { 0 };
    socklen_t ul = sizeof uc;
    getsockopt(sp[0], SOL_SOCKET, SO_PEERCRED, &uc, &ul);
    printf("       (SO_PEERCRED: pid %d uid %u gid %u; child %d)\n", uc.pid, uc.uid, uc.gid, kid);
    check(uc.pid == kid && uc.uid == 2000 && uc.gid == 2000, "SO_PEERCRED of a peer that dropped to 2000: uid 2000, gid 2000");
    char sp_path[64], uidline[128];
    snprintf(sp_path, sizeof sp_path, "/proc/%d/status", (int)kid);
    check(status_line(sp_path, "Uid:", uidline, sizeof uidline) && !strcmp(uidline, "2000\t2000\t2000\t2000"),
          "/proc/<peer>/status Uid: its virtual ids (2000), as Process.getUidForPid reads them");
    write(sp[0], &c, 1);
    int ws = 0;
    waitpid(kid, &ws, 0);
    check(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "fork: the child has its own ids (2000), the parent keeps root");
    check(getuid() == 0, "parent still uid 0");

    // capget of another process: a child with ids (root: all) and launchd
    // (no ids: an unprivileged process's empty sets).
    int cp[2];
    pipe(cp);
    pid_t ck = fork();
    if (ck == 0) { char z; close(cp[1]); read(cp[0], &z, 1); _exit(0); }
    close(cp[0]);
    usleep(200000);
    {
        struct __user_cap_header_struct h = { _LINUX_CAPABILITY_VERSION_3, ck };
        struct __user_cap_data_struct d[2];
        memset(d, 0, sizeof d);
        long r1 = syscall(SYS_capget, &h, d);
        uint64_t ce = d[0].effective | (uint64_t)d[1].effective << 32;
        struct __user_cap_header_struct h1 = { _LINUX_CAPABILITY_VERSION_3, 1 };
        memset(d, 0xff, sizeof d);
        long r2 = syscall(SYS_capget, &h1, d);
        check(r1 == 0 && ce == ALL && r2 == 0 && d[0].effective == 0 && d[0].permitted == 0,
              "capget of a child with ids: every capability; of launchd (no ids): none");
    }
    close(cp[1]);
    waitpid(ck, NULL, 0);
    check(sc(SYS_unshare, CLONE_FILES, 0, 0) == -EINVAL, "unshare(CLONE_FILES): EINVAL (no per-thread descriptor tables here)");
    check(prctl(PR_SET_SECUREBITS, 0x3) == 0 && prctl(PR_SET_SECUREBITS, 0x2) == -1 && errno == EPERM,
          "PR_SET_SECUREBITS: NOROOT set and locked, then clearing the locked bit is EPERM");

    // PR_CAPBSET_*: 41 capabilities (0..40), then EINVAL, which is what the
    // zygote's DropCapabilitiesBoundingSet loop stops on.
    check(prctl(PR_CAPBSET_READ, 40) == 1 && prctl(PR_CAPBSET_READ, 41) == -1 && errno == EINVAL,
          "PR_CAPBSET_READ: 40 set, 41 EINVAL");
    check(prctl(PR_CAPBSET_DROP, CAP_SYS_BOOT) == 0 && prctl(PR_CAPBSET_READ, CAP_SYS_BOOT) == 0,
          "PR_CAPBSET_DROP (CAP_SETPCAP): dropped");

    // setgroups, then the drop to system (1000) with keepcaps.
    gid_t gs[2] = { 3003, 1001 };
    check(setgroups(2, gs) == 0, "setgroups with CAP_SETGID");
    gid_t g2[4];
    int n = getgroups(4, g2);
    check(n == 2 && g2[0] == 1001 && g2[1] == 3003, "getgroups: sorted, as Linux keeps them");
    check(prctl(PR_SET_KEEPCAPS, 1) == 0 && prctl(PR_GET_KEEPCAPS) == 1, "PR_SET_KEEPCAPS");
    check(sc(SYS_setresgid, 1000, 1000, 1000) == 0, "setresgid(1000)");
    check(sc(SYS_setresuid, 1000, 1000, 1000) == 0, "setresuid(1000)");
    getresuid(&r, &ef, &s);
    check(r == 1000 && ef == 1000 && s == 1000, "getresuid: 1000 1000 1000");
    {
        char l1[128], l2[128], self_by_pid[64];
        snprintf(self_by_pid, sizeof self_by_pid, "/proc/%d/status", (int)getpid());
        check(status_line("/proc/self/status", "Uid:", l1, sizeof l1) && !strcmp(l1, "1000\t1000\t1000\t1000") &&
              status_line(self_by_pid, "Uid:", l2, sizeof l2) && !strcmp(l2, l1),
              "/proc/self/status and /proc/<own pid>/status Uid: 1000 (the current virtual ids)");
    }
    {
        // A directory this uid makes is its own in stat, as Linux reports the
        // creator (git's and OpenSSH's ownership checks depend on it).
        char nd[96];
        snprintf(nd, sizeof nd, "/tmp/lxrt-aids-newdir-%d", (int)getpid());
        struct stat ns;
        check(mkdir(nd, 0700) == 0 && stat(nd, &ns) == 0 && ns.st_uid == 1000 && ns.st_gid == 1000,
              "a directory made as 1000 stats as 1000:1000");
        rmdir(nd);
    }
    caps(&e, &p, &i);
    check(e == 0 && p == ALL, "keepcaps: permitted kept, effective cleared (euid left 0)");
    check(sc(SYS_setresuid, 0, 0, 0) == -EPERM, "no CAP_SETUID effective: setresuid(0) EPERM");
    check(setcaps(BIT(CAP_NET_ADMIN), BIT(CAP_NET_ADMIN) | BIT(CAP_SYS_NICE), BIT(CAP_NET_ADMIN)) == 0,
          "capset down to NET_ADMIN|SYS_NICE (permitted), NET_ADMIN (effective, inheritable)");
    check(setcaps(BIT(CAP_SYS_ADMIN), BIT(CAP_SYS_ADMIN), 0) == -EPERM, "capset cannot add back what is not permitted");
    {
        // kernel/capability.c cap_validate_magic: capset of an unknown
        // version is EINVAL even with no data (capget of one is a probe).
        struct __user_cap_header_struct h = { 0x12345678, 0 };
        long r = syscall(SYS_capset, &h, NULL);
        check(r == -1 && errno == EINVAL && h.version == _LINUX_CAPABILITY_VERSION_3,
              "capset with an unknown version: EINVAL without data too, the kernel's version written back");
    }
    check(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_NET_ADMIN, 0, 0) == 0 &&
          prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_IS_SET, CAP_NET_ADMIN, 0, 0) == 1,
          "ambient NET_ADMIN raised");
    check(prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_RAISE, CAP_SYS_NICE, 0, 0) == -1 && errno == EPERM,
          "ambient SYS_NICE refused (not inheritable)");
    check(chown(f, 1000, 1000) == 0, "chown to its own uid and gid without CAP_CHOWN: 0");
    check(chown(f, 0, 0) == -1 && errno == EPERM, "chown to root without CAP_CHOWN: EPERM");
    check(chown(o, 1000, 1000) == -1 && errno == EPERM,
          "chown of a file owned by 1234 without CAP_CHOWN: EPERM (not its owner)");
    check(chown(o, (uid_t)-1, 1001) == -1 && errno == EPERM, "... nor its group");
    check(sc(SYS_unshare, CLONE_NEWNS, 0, 0) == -EPERM, "unshare(CLONE_NEWNS) without CAP_SYS_ADMIN: EPERM");
    check(prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 && prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0) == 1,
          "PR_SET_NO_NEW_PRIVS / PR_GET_NO_NEW_PRIVS");

    // execve: the ids, the ambient set and the bind carry over.
    fflush(stdout);
    pid_t ek = fork();
    if (ek == 0) {
        execl("/proc/self/exe", argv[0], "after-exec", tmp, (char *)NULL);
        _exit(9);
    }
    waitpid(ek, &ws, 0);
    check(WIFEXITED(ws) && WEXITSTATUS(ws) == 0, "execve'd image: ids, groups, caps, no_new_privs and the bind (above)");

    unlink(f);
    unlink(o);
    unlink(ol);
    rmdir(a);
    rmdir(b);
    rmdir(tmp);
    printf("== android ids: %s\n", bad ? "MAL" : "ok");
    return bad ? 1 : 0;
}
