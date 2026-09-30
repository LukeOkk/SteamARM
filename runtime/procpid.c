// Other processes in /proc, and the socket tables in /proc/net.
//
// procfs.c describes the calling process only. The Steam client needs more:
// every websocket its UI opens to it is checked with
//
//     lsof -P -F upnR -i TCP@127.0.0.1:<port>
//
// and rejected when lsof cannot name the process at the other end ("lsof is
// required to run steam"; without it the UI dies with "Unexpected Transport
// Error 0x3008"). Linux lsof finds that process by reading /proc/net/tcp for
// the socket's inode and then every /proc/<pid>/fd/* for a "socket:[inode]"
// link, and the client then reads /proc/<pid>/cmdline and stat of the pid it
// got. So those have to describe the other guest processes truthfully.
//
// Darwin's libproc answers all of it for processes of the same user without
// any entitlement (proc_pidinfo / proc_pidfdinfo; task_for_pid is not needed):
// the fd table, each socket's addresses, ports and TCP state, and a stable
// per-socket kernel handle (soi_so, permuted by the kernel but constant for
// the socket's life) from which an inode number is derived -- the same number
// in /proc/net/* and in every fd link that refers to that socket.
//
// "Guest processes" are the host processes of this uid running this very
// runtime executable. Their cmdline and exe come from the procfs directory
// each of them maintains (<tmp>/lxrt-proc-<pid>, where FEX's PR_SET_MM_MAP
// lands the guest's argv); everything else is read from the kernel here.
// Files are generated into <our procfs dir>/.p/<pid>/ on each lookup.

#include "lxrt.h"
#include "android_ids.h"
#include "ids.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/proc.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define MAXPIDS 4096

// A socket's inode: derived from the kernel's socket handle, so the tables
// and the fd links agree, and non-zero (lsof treats 0 as "none").
static uint64_t sock_inode(uint64_t so)
{
    uint64_t z = so + 0x9e3779b97f4a7c15ull;
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
    z ^= z >> 31;
    return (z & 0x7fffffffull) | 0x1000000ull;
}

static bool same_exe_as_us(int pid)
{
    static char self[PROC_PIDPATHINFO_MAXSIZE];
    if (!self[0] && proc_pidpath(getpid(), self, sizeof self) <= 0)
        return false;
    char p[PROC_PIDPATHINFO_MAXSIZE];
    return proc_pidpath(pid, p, sizeof p) > 0 && strcmp(p, self) == 0;
}

// Live guest processes, ourselves included, ascending.
static int guest_pids(int *out, int max)
{
    static int all[MAXPIDS];
    int n = proc_listallpids(all, sizeof all);
    if (n <= 0)
        return 0;
    uid_t me = getuid();
    int k = 0;
    for (int i = 0; i < n && k < max; i++) {
        if (all[i] <= 0)
            continue;
        struct proc_bsdinfo bi;
        if (proc_pidinfo(all[i], PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != sizeof bi)
            continue;
        if (bi.pbi_uid != me || (bi.pbi_status == SZOMB))
            continue;
        if (all[i] != getpid() && !same_exe_as_us(all[i]))
            continue;
        out[k++] = all[i];
    }
    for (int i = 1; i < k; i++)          // insertion sort: a few dozen at most
        for (int j = i; j > 0 && out[j - 1] > out[j]; j--) {
            int t = out[j]; out[j] = out[j - 1]; out[j - 1] = t;
        }
    return k;
}

bool lxrt_procpid_is_guest(int pid)
{
    if (pid <= 0)
        return false;
    if (pid == getpid())
        return true;
    struct proc_bsdinfo bi;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != sizeof bi)
        return false;
    return bi.pbi_uid == getuid() && bi.pbi_status != SZOMB && same_exe_as_us(pid);
}

// What /proc/<pid>/fd/<fd> points at, for descriptors F_GETPATH cannot name:
// "socket:[inode]", "pipe:[inode]", or a file's host path (readlink hands the
// guest its own view of that). False when the descriptor is not open.
bool lxrt_procpid_fd_target(int pid, int fd, char *out, size_t n)
{
    struct socket_fdinfo si;
    if (proc_pidfdinfo(pid, fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) == sizeof si) {
        snprintf(out, n, "socket:[%llu]", (unsigned long long)sock_inode(si.psi.soi_so));
        return true;
    }
    struct pipe_fdinfo pi;
    if (proc_pidfdinfo(pid, fd, PROC_PIDFDPIPEINFO, &pi, sizeof pi) == sizeof pi) {
        snprintf(out, n, "pipe:[%llu]", (unsigned long long)sock_inode(pi.pipeinfo.pipe_handle));
        return true;
    }
    struct vnode_fdinfowithpath vi;
    if (proc_pidfdinfo(pid, fd, PROC_PIDFDVNODEPATHINFO, &vi, sizeof vi) == sizeof vi &&
        vi.pvip.vip_path[0]) {
        snprintf(out, n, "%s", vi.pvip.vip_path);
        return true;
    }
    return false;
}

// ------------------------------------------------------------ /proc/net

enum { NET_TCP, NET_TCP6, NET_UDP, NET_UDP6 };

// Darwin tcpsi_state -> Linux TCP_* (include/net/tcp_states.h).
static int linux_tcp_state(int s)
{
    switch (s) {
    case 1:  return 0x0A;   // LISTEN
    case 2:  return 0x02;   // SYN_SENT
    case 3:  return 0x03;   // SYN_RECV
    case 4:  return 0x01;   // ESTABLISHED
    case 5:  return 0x08;   // CLOSE_WAIT
    case 6:  return 0x04;   // FIN_WAIT1
    case 7:  return 0x0B;   // CLOSING
    case 8:  return 0x09;   // LAST_ACK
    case 9:  return 0x05;   // FIN_WAIT2
    case 10: return 0x06;   // TIME_WAIT
    default: return 0x07;   // CLOSE
    }
}

// An IPv4 address as the kernel prints it: the network-order word read as a
// little-endian integer (127.0.0.1 -> 0100007F).
static void hex4(char *b, size_t n, const struct in_addr *a)
{
    uint32_t w;
    memcpy(&w, a, 4);
    snprintf(b, n, "%08X", w);
}

static void hex6(char *b, size_t n, const struct in6_addr *a)
{
    uint32_t w[4];
    memcpy(w, a, 16);
    snprintf(b, n, "%08X%08X%08X%08X", w[0], w[1], w[2], w[3]);
}

static size_t gen_net(int which, char *buf, size_t cap)
{
    size_t len = 0;
#define OUT(...) do { int k_ = snprintf(buf + len, len < cap ? cap - len : 0, __VA_ARGS__); \
                      if (k_ > 0) len += (size_t)k_; } while (0)
    if (which == NET_TCP || which == NET_UDP)
        OUT("  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
    else
        OUT("  sl  local_address                         remote_address                        st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");

    static int pids[MAXPIDS];
    int np = guest_pids(pids, MAXPIDS);
    // One line per socket, however many processes hold it (fork, SCM_RIGHTS).
    static uint64_t seen[16384];
    int nseen = 0, sl = 0;
    for (int i = 0; i < np; i++) {
        int bytes = proc_pidinfo(pids[i], PROC_PIDLISTFDS, 0, NULL, 0);
        if (bytes <= 0)
            continue;
        struct proc_fdinfo *fds = malloc((size_t)bytes);
        if (!fds)
            continue;
        bytes = proc_pidinfo(pids[i], PROC_PIDLISTFDS, 0, fds, bytes);
        int nf = bytes > 0 ? bytes / (int)sizeof *fds : 0;
        for (int f = 0; f < nf; f++) {
            if (fds[f].proc_fdtype != PROX_FDTYPE_SOCKET)
                continue;
            struct socket_fdinfo si;
            if (proc_pidfdinfo(pids[i], fds[f].proc_fd, PROC_PIDFDSOCKETINFO, &si, sizeof si) != sizeof si)
                continue;
            int kind = si.psi.soi_kind;
            bool tcp = kind == SOCKINFO_TCP;
            if (!tcp && kind != SOCKINFO_IN)
                continue;
            if (tcp != (which == NET_TCP || which == NET_TCP6))
                continue;
            const struct in_sockinfo *in = tcp ? &si.psi.soi_proto.pri_tcp.tcpsi_ini
                                               : &si.psi.soi_proto.pri_in;
            bool v6 = (in->insi_vflag & INI_IPV6) != 0;
            if (v6 != (which == NET_TCP6 || which == NET_UDP6))
                continue;
            bool dup = false;
            for (int s = 0; s < nseen && !dup; s++)
                dup = seen[s] == si.psi.soi_so;
            if (dup)
                continue;
            if (nseen < (int)(sizeof seen / sizeof *seen))
                seen[nseen++] = si.psi.soi_so;
            char la[40], ra[40];
            if (v6) {
                hex6(la, sizeof la, &in->insi_laddr.ina_6);
                hex6(ra, sizeof ra, &in->insi_faddr.ina_6);
            } else {
                hex4(la, sizeof la, &in->insi_laddr.ina_46.i46a_addr4);
                hex4(ra, sizeof ra, &in->insi_faddr.ina_46.i46a_addr4);
            }
            int st = tcp ? linux_tcp_state(si.psi.soi_proto.pri_tcp.tcpsi_state)
                         : (in->insi_fport ? 0x01 : 0x07);
            OUT("%4d: %s:%04X %s:%04X %02X %08X:%08X 00:00000000 00000000 %5u        0 %llu 1 0000000000000000 100 0 0 10 0\n",
                sl++, la, ntohs((uint16_t)in->insi_lport), ra, ntohs((uint16_t)in->insi_fport), st,
                (unsigned)si.psi.soi_snd.sbi_cc, (unsigned)si.psi.soi_rcv.sbi_cc,
                (unsigned)getuid(), (unsigned long long)sock_inode(si.psi.soi_so));
        }
        free(fds);
    }
#undef OUT
    return len < cap ? len : cap;
}

bool lxrt_file_same(const char *path, const void *data, size_t len);
void lxrt_link_set(const char *target, const char *link);

static void write_whole(const char *path, const char *data, size_t len)
{
    if (lxrt_file_same(path, data, len))
        return;
    char tmp[1200];
    snprintf(tmp, sizeof tmp, "%s.%d", path, (int)getpid());
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0)
            break;
        off += (size_t)w;
    }
    close(fd);
    rename(tmp, path);
}

static const char *net_names[] = { "tcp", "tcp6", "udp", "udp6" };

// "net" or "net/<file>" (after /proc/ or /proc/<pid>/): host path, or NULL.
const char *lxrt_procnet_translate(const char *sub, const char *dir)
{
    if (strncmp(sub, "net", 3) != 0 || (sub[3] != '\0' && sub[3] != '/'))
        return NULL;
    static _Thread_local char out[1024];
    char nd[1024];
    snprintf(nd, sizeof nd, "%s/.net", dir);
    mkdir(nd, 0700);
    const char *file = sub[3] == '/' ? sub + 4 : "";
    static char *buf;
    const size_t cap = 4 << 20;
    if (!buf)
        buf = malloc(cap);
    for (int w = 0; w < 4; w++) {
        if (*file && strcmp(file, net_names[w]) != 0)
            continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", nd, net_names[w]);
        if (buf)
            write_whole(p, buf, gen_net(w, buf, cap));
        if (*file) {
            snprintf(out, sizeof out, "%s", p);
            return out;
        }
    }
    if (*file)
        return NULL;                  // unix, raw, ...: not provided
    snprintf(out, sizeof out, "%s", nd);
    return out;
}

// ------------------------------------------------------------ /proc/<pid>

// The procfs directory another guest process maintains, if we can find it.
static bool owner_dir(int pid, char *out, size_t n)
{
    const char *cands[3] = { getenv("TMPDIR"), "/tmp", "/private/tmp" };
    for (int i = 0; i < 3; i++) {
        if (!cands[i] || !*cands[i])
            continue;
        snprintf(out, n, "%s/lxrt-proc-%d", cands[i], pid);
        struct stat st;
        if (stat(out, &st) == 0 && S_ISDIR(st.st_mode))
            return true;
    }
    return false;
}

static void copy_file(const char *from, const char *to)
{
    char buf[65536];
    int fd = open(from, O_RDONLY);
    if (fd < 0)
        return;
    ssize_t r = read(fd, buf, sizeof buf);
    close(fd);
    if (r >= 0)
        write_whole(to, buf, (size_t)r);
}

static void gen_pid_dir(int pid, const char *pd)
{
    mkdir(pd, 0700);
    struct proc_bsdinfo bi;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != sizeof bi)
        return;
    char od[1024], p[1200], q[1200];
    bool have_owner = owner_dir(pid, od, sizeof od);

    // cmdline and exe: the guest's own, as that process published them.
    snprintf(p, sizeof p, "%s/cmdline", pd);
    char comm[16] = "lxrt";
    if (have_owner) {
        snprintf(q, sizeof q, "%s/cmdline", od);
        copy_file(q, p);
        int fd = open(q, O_RDONLY);
        if (fd >= 0) {
            char a0[PATH_MAX];
            ssize_t r = read(fd, a0, sizeof a0 - 1);
            close(fd);
            if (r > 0) {
                a0[r] = '\0';
                const char *base = strrchr(a0, '/');
                snprintf(comm, sizeof comm, "%s", base ? base + 1 : a0);
            }
        }
        snprintf(q, sizeof q, "%s/exe", od);
        char tgt[PATH_MAX];
        ssize_t l = readlink(q, tgt, sizeof tgt - 1);
        snprintf(p, sizeof p, "%s/exe", pd);
        if (l > 0) {
            tgt[l] = '\0';
            lxrt_link_set(tgt, p);
        } else {
            unlink(p);
        }
    } else {
        write_whole(p, "", 0);
    }
    snprintf(p, sizeof p, "%s/comm", pd);
    char line[4096];
    int n = snprintf(line, sizeof line, "%s\n", comm);
    write_whole(p, line, (size_t)n);

    char state = bi.pbi_status == SSTOP ? 'T' : bi.pbi_status == SZOMB ? 'Z' : 'S';
    int guest_pid = lxrt_ids_on() ? lxrt_ids_to_guest(pid, 0) : pid;
    int guest_ppid = lxrt_ids_on() ? lxrt_ids_to_guest(bi.pbi_ppid, 0) : (int)bi.pbi_ppid;
    int guest_pgid = lxrt_ids_on() ? lxrt_ids_to_guest(bi.pbi_pgid, 0) : (int)bi.pbi_pgid;
    if (!guest_ppid) guest_ppid = 1;
    if (!guest_pgid) guest_pgid = 1;
    struct proc_taskinfo ti;
    if (proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &ti, sizeof ti) != sizeof ti)
        memset(&ti, 0, sizeof ti);
    long hz = 100;
    unsigned long long ut = ti.pti_total_user / (1000000000ull / (unsigned long long)hz);
    unsigned long long stt = ti.pti_total_system / (1000000000ull / (unsigned long long)hz);
    n = snprintf(line, sizeof line,
                 "%d (%s) %c %d %d %d 0 -1 4194304 0 0 0 0 %llu %llu 0 0 20 0 %d 0 0 %llu %llu "
                 "18446744073709551615 0 0 0 0 0 0 0 0 0 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
                 guest_pid, comm, state, guest_ppid, guest_pgid, guest_pgid,
                 ut, stt, ti.pti_threadnum > 0 ? ti.pti_threadnum : 1,
                 (unsigned long long)ti.pti_virtual_size, (unsigned long long)(ti.pti_resident_size / 4096));
    snprintf(p, sizeof p, "%s/stat", pd);
    write_whole(p, line, (size_t)n);

    // A guest with Android ids shows its virtual ones (android_ids.h): the
    // framework matches a pid to an app by the Uid: line here
    // (Process.getUidForPid, ActivityManagerService.isProcessAliveLocked).
    uint32_t ru = bi.pbi_uid, eu = bi.pbi_uid, su = bi.pbi_svuid, rg = bi.pbi_gid, eg = bi.pbi_gid, sg = bi.pbi_svgid;
    struct lxrt_aids_peer peer;
    if (lxrt_aids_lookup(pid, &peer)) {
        ru = peer.ruid; eu = su = peer.euid;
        rg = peer.rgid; eg = sg = peer.egid;
    }
    n = snprintf(line, sizeof line,
                 "Name:\t%s\nUmask:\t0022\nState:\t%c (%s)\nTgid:\t%d\nNgid:\t0\nPid:\t%d\nPPid:\t%d\n"
                 "TracerPid:\t0\nUid:\t%u\t%u\t%u\t%u\nGid:\t%u\t%u\t%u\t%u\nThreads:\t%d\n",
                 comm, state, state == 'T' ? "stopped" : "sleeping", guest_pid, guest_pid, guest_ppid,
                 ru, eu, su, eu, rg, eg, sg, eg,
                 ti.pti_threadnum > 0 ? ti.pti_threadnum : 1);
    snprintf(p, sizeof p, "%s/status", pd);
    write_whole(p, line, (size_t)n);

    if (lxrt_ids_on()) {
        char taskdir[1200];
        snprintf(taskdir, sizeof taskdir, "%s/task", pd);
        mkdir(taskdir, 0700);
        int tids[512];
        int count = lxrt_ids_threads(pid, tids, 512);
        if (count > 512) count = 512;
        DIR *tasks = opendir(taskdir);
        if (tasks) {
            struct dirent *e;
            while ((e = readdir(tasks))) {
                char *end;
                long old = strtol(e->d_name, &end, 10);
                if (end == e->d_name || *end) continue;
                bool live = false;
                for (int i = 0; i < count; i++) if (tids[i] == old) live = true;
                if (!live) {
                    char stale[1300];
                    snprintf(stale, sizeof stale, "%s/%ld/status", taskdir, old); unlink(stale);
                    snprintf(stale, sizeof stale, "%s/%ld/comm", taskdir, old); unlink(stale);
                    snprintf(stale, sizeof stale, "%s/%ld", taskdir, old); rmdir(stale);
                }
            }
            closedir(tasks);
        }
        for (int i = 0; i < count; i++) {
            char td[1300], file[1400];
            snprintf(td, sizeof td, "%s/%d", taskdir, tids[i]);
            mkdir(td, 0700);
            snprintf(file, sizeof file, "%s/comm", td);
            n = snprintf(line, sizeof line, "%s\n", comm);
            write_whole(file, line, (size_t)n);
            snprintf(file, sizeof file, "%s/status", td);
            n = snprintf(line, sizeof line,
                         "Name:\t%s\nState:\t%c (sleeping)\nTgid:\t%d\nPid:\t%d\nPPid:\t%d\n",
                         comm, state, guest_pid, tids[i], guest_ppid);
            write_whole(file, line, (size_t)n);
        }
    }
}

static void gen_pid_fds(int pid, const char *pd)
{
    char fdd[1200];
    snprintf(fdd, sizeof fdd, "%s/fd", pd);
    mkdir(fdd, 0700);
    int bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, NULL, 0);
    struct proc_fdinfo *fds = bytes > 0 ? malloc((size_t)bytes) : NULL;
    if (fds)
        bytes = proc_pidinfo(pid, PROC_PIDLISTFDS, 0, fds, bytes);
    int nf = fds && bytes > 0 ? bytes / (int)sizeof *fds : 0;
    // Only entries whose descriptor is gone are removed (see lxrt_link_set).
    DIR *d = opendir(fdd);
    if (d) {
        struct dirent *e;
        char p[1500];
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            int n = atoi(e->d_name), live = 0;
            for (int f = 0; f < nf && !live; f++)
                live = fds[f].proc_fd == n;
            if (!live) {
                snprintf(p, sizeof p, "%s/%s", fdd, e->d_name);
                unlink(p);
            }
        }
        closedir(d);
    }
    if (!fds)
        return;
    for (int f = 0; f < nf; f++) {
        char tgt[PATH_MAX], p[1500];
        if (!lxrt_procpid_fd_target(pid, fds[f].proc_fd, tgt, sizeof tgt))
            snprintf(tgt, sizeof tgt, "anon_inode:[fd%d]", fds[f].proc_fd);
        snprintf(p, sizeof p, "%s/%d", fdd, fds[f].proc_fd);
        lxrt_link_set(tgt, p);
    }
    free(fds);
}

bool lxrt_procpid_environ(int pid, const char *out_path);

// /proc/<pid>[/...] for a guest process other than the caller. `rest` is the
// part after "/proc/". NULL when <pid> is not one.
const char *lxrt_procpid_translate(const char *rest, const char *dir)
{
    char *end;
    long pid = strtol(rest, &end, 10);
    if (end == rest || (*end != '\0' && *end != '/') || pid <= 0 || pid > INT_MAX)
        return NULL;
    if (lxrt_ids_on()) {
        pid = lxrt_ids_target_pid((int)pid);
        if (pid < 0) return NULL;
    }
    if (pid == getpid() || !lxrt_procpid_is_guest((int)pid))
        return NULL;
    const char *sub = *end == '/' ? end + 1 : "";
    char base[1024], pd[1100];
    snprintf(base, sizeof base, "%s/.p", dir);
    mkdir(base, 0700);
    snprintf(pd, sizeof pd, "%s/%ld", base, pid);
    const char *net = lxrt_procnet_translate(sub, dir);
    if (net)
        return net;
    // Generate only what was asked for: lsof readlink()s and stat()s every
    // /proc/<pid>/fd/<n> of every process, and rebuilding the whole fd
    // directory (or the stat/status files) per lookup made one lsof run take
    // 17 s under FEX -- longer than the Steam UI waits for its websocket.
    mkdir(pd, 0700);
    if (strcmp(sub, "environ") == 0 && !getenv("LXRT_NO_ENVIRON")) {
        char ep[1200];
        snprintf(ep, sizeof ep, "%s/environ", pd);
        lxrt_procpid_environ((int)pid, ep);
    } else if (strncmp(sub, "fd/", 3) == 0 && sub[3] >= '0' && sub[3] <= '9') {
        int fd = atoi(sub + 3);
        char fdd[1200], p[1300], tgt[PATH_MAX];
        snprintf(fdd, sizeof fdd, "%s/fd", pd);
        mkdir(fdd, 0700);
        snprintf(p, sizeof p, "%s/%d", fdd, fd);
        if (lxrt_procpid_fd_target((int)pid, fd, tgt, sizeof tgt))
            lxrt_link_set(tgt, p);
        else
            unlink(p);
    } else if (strcmp(sub, "fd") == 0) {
        gen_pid_fds((int)pid, pd);
    } else {
        gen_pid_dir((int)pid, pd);
    }
    static _Thread_local char out[1300];
    snprintf(out, sizeof out, "%s%s%s", pd, *sub ? "/" : "", sub);
    return out;
}

// Opening /proc itself: one entry per guest process, as symlinks into .p/
// (our own pid is the "<pid>" -> "." link procfs.c keeps).
void lxrt_procpid_list(const char *dir)
{
    char base[1024];
    snprintf(base, sizeof base, "%s/.p", dir);
    mkdir(base, 0700);
    static int pids[MAXPIDS];
    int np = guest_pids(pids, MAXPIDS);
    // Drop entries of processes that are gone.
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            char *x;
            long v = strtol(e->d_name, &x, 10);
            if (x == e->d_name || *x || v == lxrt_ids_pid())
                continue;
            bool live = false;
            for (int i = 0; i < np && !live; i++)
                live = (lxrt_ids_on() ? lxrt_ids_to_guest(pids[i], 0) : pids[i]) == v;
            if (!live) {
                char p[1300];
                snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
                unlink(p);
            }
        }
        closedir(d);
    }
    for (int i = 0; i < np; i++) {
        if (pids[i] == getpid())
            continue;
        int guest_pid = lxrt_ids_on() ? lxrt_ids_to_guest(pids[i], 0) : pids[i];
        if (guest_pid < 2) continue;
        char pd[1100], link[1100], tgt[64];
        snprintf(pd, sizeof pd, "%s/%d", base, pids[i]);
        // Listing /proc must not rewrite every process's files: they are
        // generated when a path under /proc/<pid> is actually looked up.
        struct stat st;
        if (stat(pd, &st) != 0)
            gen_pid_dir(pids[i], pd);
        snprintf(link, sizeof link, "%s/%d", dir, guest_pid);
        snprintf(tgt, sizeof tgt, ".p/%d", pids[i]);
        lxrt_link_set(tgt, link);
    }
    lxrt_procnet_translate("net", dir);
    char nl[1100];
    snprintf(nl, sizeof nl, "%s/net", dir);
    lxrt_link_set(".net", nl);
}

// stat() through /proc/<pid>/fd/<n> when the link is "socket:[N]" or
// "pipe:[N]": on Linux the magic link reaches the socket's inode, and lsof
// takes the inode it matches against /proc/net/* from exactly that stat.
// Here the link dangles; answer as sockfs/pipefs would. `hp` is the host path
// of the link itself (not followed).
bool lxrt_magic_link_stat(int dfd, const char *hp, struct stat *d)
{
    char t[64];
    ssize_t n = readlinkat(dfd, hp, t, sizeof t - 1);
    if (n <= 0)
        return false;
    t[n] = '\0';
    unsigned long long ino;
    bool sock = sscanf(t, "socket:[%llu]", &ino) == 1;
    if (!sock && sscanf(t, "pipe:[%llu]", &ino) != 1)
        return false;
    memset(d, 0, sizeof *d);
    d->st_mode = sock ? (S_IFSOCK | 0777) : (S_IFIFO | 0600);
    d->st_ino = (ino_t)ino;
    d->st_dev = makedev(0, sock ? 8 : 13);   // Linux's sockfs / pipefs
    d->st_nlink = 1;
    d->st_uid = getuid();
    d->st_gid = getgid();
    d->st_blksize = 4096;
    return true;
}

// /proc/<pid>/environ: the environment the process was started with, which is
// what Linux shows too. For a guest process that is the host environment of
// its runtime process: the runtime execs with the guest's envp as is.
// KERN_PROCARGS2 = int argc, exec path, NUL padding, argv[], envp[].
bool lxrt_procpid_environ(int pid, const char *out_path)
{
    int mib[3] = { CTL_KERN, KERN_PROCARGS2, pid };
    size_t sz = 0;
    if (sysctl(mib, 3, NULL, &sz, NULL, 0) != 0 || sz < sizeof(int))
        return false;
    char *b = malloc(sz);
    if (!b)
        return false;
    if (sysctl(mib, 3, b, &sz, NULL, 0) != 0) {
        free(b);
        return false;
    }
    int argc;
    memcpy(&argc, b, sizeof argc);
    char *p = b + sizeof argc, *end = b + sz;
    p += strnlen(p, (size_t)(end - p));          // exec path
    while (p < end && *p == '\0')
        p++;
    for (int i = 0; i < argc && p < end; i++)    // argv
        p += strnlen(p, (size_t)(end - p)) + 1;
    char *env = p;
    char *q = env;
    while (q < end && *q) {                       // envp, up to the empty string
        size_t l = strnlen(q, (size_t)(end - q));
        q += l + 1;
    }
    if (q > end)
        q = end;
    write_whole(out_path, env, (size_t)(q - env));
    free(b);
    return true;
}
