// See android_ids.h. Linux references: kernel/sys.c (__sys_setresuid,
// __sys_setreuid, __sys_setuid and the gid twins, setfsuid), kernel/groups.c
// (setgroups: CAP_SETGID, sorted), kernel/capability.c (capget/capset),
// security/commoncap.c (cap_emulate_setxuid, cap_bprm_creds_from_file,
// cap_task_prctl), include/uapi/linux/capability.h and prctl.h.
#include "android_ids.h"
#include "lxrt.h"

#include <errno.h>
#include <fcntl.h>
#include <libproc.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/proc_info.h>
#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

#define LERR(e) (-(long)lxrt_errno_to_linux(e))

#define CAP_CHOWN     0
#define CAP_SETGID    6
#define CAP_SETUID    7
#define CAP_SETPCAP   8
#define CAP_LAST_CAP  40                       // Linux 5.9+: CAP_CHECKPOINT_RESTORE
#define CAP_ALL       ((1ull << (CAP_LAST_CAP + 1)) - 1)
#define SECURE_NOROOT                0
#define SECURE_NO_SETUID_FIXUP       2
#define SECURE_KEEP_CAPS             4
#define SECURE_NO_CAP_AMBIENT_RAISE  6
#define MAX_GROUPS    1024                     // Linux allows 65536; Android uses a few dozen

struct ids {
    uint32_t r, e, s, fs;
    uint32_t rg, eg, sg, fsg;
    int ngroups;
    uint32_t groups[MAX_GROUPS];
    uint64_t eff, prm, inh, bnd, amb;
    uint32_t securebits;
    bool nnp;                                  // PR_SET_NO_NEW_PRIVS: kept across fork and execve
};

static struct ids g;
static int g_on = -1;                          // -1 not read yet
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

static bool cap_has(uint64_t set, int cap) { return cap >= 0 && cap <= CAP_LAST_CAP && ((set >> cap) & 1); }
bool lxrt_aids_capable(int cap) { return lxrt_aids_on() && cap_has(g.eff, cap); }

// ---- the shared table: pid -> virtual ids, for SO_PEERCRED and binder ----
// One slot per pid. `seq` is odd while the owner rewrites the slot; a
// reader takes a copy between two equal even values (a seqlock), so it
// never mixes two updates.
struct ent {
    _Atomic uint32_t seq;
    _Atomic int32_t pid;
    _Atomic uint32_t r, e, rg, eg;
    _Atomic uint64_t eff, prm, inh, start;
};
#define TABLE_SLOTS 100000                     // Darwin's PID_MAX is 99999
static struct ent *g_table;

static struct ent *table(void)
{
    static _Atomic int state;                  // 0 untried, 1 ready, 2 failed
    int st = atomic_load(&state);
    if (st == 1) return g_table;
    if (st == 2) return NULL;
    char path[96];
    snprintf(path, sizeof path, "/tmp/lxrt-shm-%u", (unsigned)getuid());
    mkdir(path, 01777);
    struct stat ds;
    // The directory is shared with runtime/socket.c's table; it must be ours.
    if (lstat(path, &ds) != 0 || !S_ISDIR(ds.st_mode) || ds.st_uid != getuid()) {
        atomic_store(&state, 2);
        return NULL;
    }
    snprintf(path + strlen(path), sizeof path - strlen(path), "/android-ids.v2");
    int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    size_t sz = sizeof(struct ent) * TABLE_SLOTS;
    void *p = MAP_FAILED;
    struct stat fs;
    if (fd >= 0 && fstat(fd, &fs) == 0 && fs.st_uid == getuid() &&
        (fs.st_size == (off_t)sz || ftruncate(fd, (off_t)sz) == 0))
        p = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fd >= 0) close(fd);
    if (p == MAP_FAILED) { atomic_store(&state, 2); return NULL; }
    g_table = p;
    atomic_store(&state, 1);
    return g_table;
}

static uint64_t start_time(int pid)
{
    struct proc_bsdinfo bi;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi)
        return 0;
    return (uint64_t)bi.pbi_start_tvsec * 1000000ull + bi.pbi_start_tvusec;
}

static void publish(void)
{
    struct ent *t = table();
    int pid = getpid();
    if (!t || pid <= 0 || pid >= TABLE_SLOTS) return;
    struct ent *e = &t[pid];
    uint64_t st = start_time(pid);
    uint32_t s = atomic_load(&e->seq) & ~1u;   // one writer: this process, under g_mu
    atomic_store(&e->seq, s + 1);              // odd: being written
    atomic_store(&e->pid, pid);
    atomic_store(&e->r, g.r); atomic_store(&e->e, g.e);
    atomic_store(&e->rg, g.rg); atomic_store(&e->eg, g.eg);
    atomic_store(&e->eff, g.eff); atomic_store(&e->prm, g.prm); atomic_store(&e->inh, g.inh);
    atomic_store(&e->start, st);
    atomic_store(&e->seq, s + 2);              // even again, and different
}

bool lxrt_aids_lookup(int pid, struct lxrt_aids_peer *out)
{
    struct ent *t = table();
    if (!t || pid <= 0 || pid >= TABLE_SLOTS) return false;
    struct ent *e = &t[pid];
    struct lxrt_aids_peer p;
    uint64_t st = 0;
    for (int tries = 0; ; tries++) {
        uint32_t s1 = atomic_load(&e->seq);
        if (s1 & 1) {                          // being written: try again shortly
            if (tries > 1000) return false;
            continue;
        }
        if (atomic_load(&e->pid) != pid) return false;
        p.ruid = atomic_load(&e->r); p.euid = atomic_load(&e->e);
        p.rgid = atomic_load(&e->rg); p.egid = atomic_load(&e->eg);
        p.eff = atomic_load(&e->eff); p.prm = atomic_load(&e->prm); p.inh = atomic_load(&e->inh);
        st = atomic_load(&e->start);
        if (atomic_load(&e->seq) == s1) break;
        if (tries > 1000) return false;
    }
    if (!st || st != start_time(pid)) return false;   // a later process with the same pid
    if (out) *out = p;
    return true;
}

static void after_fork_child(void)
{
    pthread_mutex_init(&g_mu, NULL);
    if (g_on == 1) publish();
}

// ---- LXRT_ANDROID_IDS ----
static bool parse_u32s(const char *s, const char *end, uint32_t *out, int max, int *n)
{
    *n = 0;
    while (s < end) {
        char *q;
        if (*n >= max) return false;
        unsigned long v = strtoul(s, &q, 10);
        if (q == s || v > 0xffffffffu) return false;
        out[(*n)++] = (uint32_t)v;
        s = q;
        if (s < end && *s == ',') s++;
        else if (s < end) return false;
    }
    return true;
}

static int cmp_u32(const void *a, const void *b)
{
    uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b;
    return x < y ? -1 : x > y;
}

static bool parse(const char *v)
{
    memset(&g, 0, sizeof g);
    g.bnd = CAP_ALL;
    if (!strcmp(v, "root")) {
        g.eff = g.prm = CAP_ALL;
        return true;
    }
    const char *f[5] = { 0 }, *fe[5] = { 0 };
    int nf = 0;
    const char *p = v;
    while (nf < 5) {
        f[nf] = p;
        const char *c = strchr(p, ':');
        fe[nf] = c ? c : p + strlen(p);
        nf++;
        if (!c) break;
        p = c + 1;
    }
    if (nf < 2) return false;
    uint32_t u[3], gg[3];
    int nu, ng;
    if (!parse_u32s(f[0], fe[0], u, 3, &nu) || (nu != 1 && nu != 3)) return false;
    if (!parse_u32s(f[1], fe[1], gg, 3, &ng) || (ng != 1 && ng != 3)) return false;
    g.r = u[0]; g.e = nu == 3 ? u[1] : u[0]; g.s = nu == 3 ? u[2] : u[0]; g.fs = g.e;
    g.rg = gg[0]; g.eg = ng == 3 ? gg[1] : gg[0]; g.sg = ng == 3 ? gg[2] : gg[0]; g.fsg = g.eg;
    if (nf > 2 && !parse_u32s(f[2], fe[2], g.groups, MAX_GROUPS, &g.ngroups)) return false;
    qsort(g.groups, (size_t)g.ngroups, sizeof g.groups[0], cmp_u32);
    if (nf > 3 && fe[3] > f[3]) {
        uint64_t c[5] = { 0 };
        const char *s = f[3];
        for (int i = 0; i < 5; i++) {
            char *q;
            c[i] = strtoull(s, &q, 16);
            if (q == s) return false;
            s = q;
            if (i < 4) { if (s >= fe[3] || *s != ',') return false; s++; }
        }
        g.eff = c[0] & CAP_ALL; g.prm = c[1] & CAP_ALL; g.inh = c[2] & CAP_ALL;
        g.bnd = c[3] & CAP_ALL; g.amb = c[4] & CAP_ALL;
        if ((g.eff & ~g.prm) || (g.amb & ~(g.prm & g.inh))) return false;
    } else if (g.e == 0) {
        g.eff = g.prm = CAP_ALL;               // a root process has every capability
    }
    for (const char *c = nf > 4 ? f[4] : fe[0]; nf > 4 && c < fe[4]; c++) {
        if (*c == 'k') g.securebits |= 1u << SECURE_KEEP_CAPS;
        else if (*c == 'n') g.nnp = true;
        else if (*c == 'b') {                  // b<hex>: the securebits
            char *q;
            g.securebits |= (uint32_t)strtoul(c + 1, &q, 16);
            c = q - 1;
        } else return false;
    }
    return true;
}

bool lxrt_aids_on(void)
{
    if (g_on >= 0) return g_on == 1;
    pthread_mutex_lock(&g_mu);
    if (g_on < 0) {
        const char *v = getenv("LXRT_ANDROID_IDS");
        int on = 0;
        if (v && *v) {
            if (parse(v)) on = 1;
            else fprintf(stderr, "[lxrt] LXRT_ANDROID_IDS=\"%s\" is not ruid[,euid,suid]:rgid[,egid,sgid]"
                                 "[:groups[:eff,prm,inh,bnd,amb[:k]]]; Android ids off\n", v);
        }
        if (on) {
            pthread_atfork(NULL, NULL, after_fork_child);
            g_on = 1;
            publish();
        } else {
            g_on = 0;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return g_on == 1;
}

uint32_t lxrt_aids_euid(void) { return lxrt_aids_on() ? g.e : (uint32_t)geteuid(); }
uint32_t lxrt_aids_egid(void) { return lxrt_aids_on() ? g.eg : (uint32_t)getegid(); }

void lxrt_aids_status_ids(uint32_t u[4], uint32_t gg[4], char *groups, size_t gn)
{
    if (groups && gn) groups[0] = '\0';
    if (!lxrt_aids_on()) {
        u[0] = u[1] = u[2] = u[3] = (uint32_t)getuid();
        gg[0] = gg[1] = gg[2] = gg[3] = (uint32_t)getgid();
        return;
    }
    pthread_mutex_lock(&g_mu);
    u[0] = g.r; u[1] = g.e; u[2] = g.s; u[3] = g.fs;
    gg[0] = g.rg; gg[1] = g.eg; gg[2] = g.sg; gg[3] = g.fsg;
    size_t o = 0;
    for (int i = 0; groups && i < g.ngroups && o + 12 < gn; i++)
        o += (size_t)snprintf(groups + o, gn - o, "%u ", g.groups[i]);
    pthread_mutex_unlock(&g_mu);
}

// ---- guest memory ----
static bool mem_ok(uint64_t addr, size_t n, bool need_write)
{
    if (!addr || !n) return false;
    mach_vm_address_t a = addr, end = addr + n;
    if (end < a) return false;
    while (a < end) {
        mach_vm_address_t ra = a;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
            return false;
        if (obj != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), obj);
        if (ra > a || rs == 0 || !(info.protection & VM_PROT_READ) ||
            (need_write && !(info.protection & VM_PROT_WRITE)))
            return false;
        a = ra + rs;
    }
    return true;
}

// ---- Linux's rules ----
// cap_emulate_setxuid: called after every uid change.
static void setxuid_fixup(uint32_t or_, uint32_t oe, uint32_t os)
{
    if (g.securebits & (1u << SECURE_NO_SETUID_FIXUP)) return;
    if ((or_ == 0 || oe == 0 || os == 0) && g.r != 0 && g.e != 0 && g.s != 0) {
        if (!(g.securebits & (1u << SECURE_KEEP_CAPS))) g.prm = g.eff = 0;
        g.amb = 0;
    }
    if (oe == 0 && g.e != 0) g.eff = 0;
    if (oe != 0 && g.e == 0) g.eff = g.prm;
}

static bool one_of(uint32_t v, uint32_t a, uint32_t b, uint32_t c) { return v == a || v == b || v == c; }

static long do_setresuid(uint32_t r, uint32_t e, uint32_t s)
{
    const uint32_t N = 0xffffffffu;
    if (!cap_has(g.eff, CAP_SETUID)) {
        if ((r != N && !one_of(r, g.r, g.e, g.s)) || (e != N && !one_of(e, g.r, g.e, g.s)) ||
            (s != N && !one_of(s, g.r, g.e, g.s)))
            return LERR(EPERM);
    }
    uint32_t or_ = g.r, oe = g.e, os = g.s;
    if (r != N) g.r = r;
    if (e != N) g.e = e;
    if (s != N) g.s = s;
    g.fs = g.e;
    setxuid_fixup(or_, oe, os);
    return 0;
}

static long do_setreuid(uint32_t r, uint32_t e)
{
    const uint32_t N = 0xffffffffu;
    bool cap = cap_has(g.eff, CAP_SETUID);
    if (r != N && r != g.r && r != g.e && !cap) return LERR(EPERM);
    if (e != N && !one_of(e, g.r, g.e, g.s) && !cap) return LERR(EPERM);
    uint32_t or_ = g.r, oe = g.e, os = g.s;
    if (r != N) g.r = r;
    if (e != N) g.e = e;
    if (r != N || (e != N && e != or_)) g.s = g.e;
    g.fs = g.e;
    setxuid_fixup(or_, oe, os);
    return 0;
}

static long do_setuid(uint32_t u)
{
    uint32_t or_ = g.r, oe = g.e, os = g.s;
    if (cap_has(g.eff, CAP_SETUID)) { g.r = g.s = u; }
    else if (u != g.r && u != g.s) return LERR(EPERM);
    g.e = g.fs = u;
    setxuid_fixup(or_, oe, os);
    return 0;
}

static long do_setresgid(uint32_t r, uint32_t e, uint32_t s)
{
    const uint32_t N = 0xffffffffu;
    if (!cap_has(g.eff, CAP_SETGID)) {
        if ((r != N && !one_of(r, g.rg, g.eg, g.sg)) || (e != N && !one_of(e, g.rg, g.eg, g.sg)) ||
            (s != N && !one_of(s, g.rg, g.eg, g.sg)))
            return LERR(EPERM);
    }
    if (r != N) g.rg = r;
    if (e != N) g.eg = e;
    if (s != N) g.sg = s;
    g.fsg = g.eg;
    return 0;
}

static long do_setregid(uint32_t r, uint32_t e)
{
    const uint32_t N = 0xffffffffu;
    bool cap = cap_has(g.eff, CAP_SETGID);
    if (r != N && r != g.rg && r != g.eg && !cap) return LERR(EPERM);
    if (e != N && !one_of(e, g.rg, g.eg, g.sg) && !cap) return LERR(EPERM);
    uint32_t org = g.rg;
    if (r != N) g.rg = r;
    if (e != N) g.eg = e;
    if (r != N || (e != N && e != org)) g.sg = g.eg;
    g.fsg = g.eg;
    return 0;
}

static long do_setgid(uint32_t v)
{
    if (cap_has(g.eff, CAP_SETGID)) { g.rg = g.sg = v; }
    else if (v != g.rg && v != g.sg) return LERR(EPERM);
    g.eg = g.fsg = v;
    return 0;
}

static long do_cap(bool set, uint64_t hdr, uint64_t data)
{
    if (!mem_ok(hdr, 8, set ? false : true)) return LERR(EFAULT);
    uint32_t *h = (uint32_t *)(uintptr_t)hdr;
    uint32_t ver = h[0];
    int32_t pid = (int32_t)h[1];
    int words = ver == 0x19980330 ? 1 : (ver == 0x20071026 || ver == 0x20080522) ? 2 : 0;
    if (!words) {
        if (!mem_ok(hdr, 8, true)) return LERR(EFAULT);
        h[0] = 0x20080522;                     // Linux answers with its preferred version
        // kernel/capability.c: capget with no data is a version probe (0);
        // capset returns cap_validate_magic's -EINVAL whatever data is.
        return (data || set) ? LERR(EINVAL) : 0;
    }
    if (pid < 0) return LERR(EINVAL);
    if (!set) {
        if (!data) return 0;                   // a version probe
        if (!mem_ok(data, (size_t)words * 12, true)) return LERR(EFAULT);
        uint32_t *d = (uint32_t *)(uintptr_t)data;
        uint64_t e = g.eff, p = g.prm, i = g.inh;
        if (pid != 0 && pid != getpid() && pid != lxrt_gettid()) {
            if (kill(pid, 0) != 0 && errno == ESRCH) return LERR(ESRCH);
            struct lxrt_aids_peer pe;
            if (lxrt_aids_lookup(pid, &pe)) { e = pe.eff; p = pe.prm; i = pe.inh; }
            else e = p = i = 0;                // no Android ids: an unprivileged process
        }
        for (int w = 0; w < words; w++) {
            d[w * 3 + 0] = (uint32_t)(e >> (32 * w));
            d[w * 3 + 1] = (uint32_t)(p >> (32 * w));
            d[w * 3 + 2] = (uint32_t)(i >> (32 * w));
        }
        return 0;
    }
    if (pid != 0 && pid != getpid() && pid != lxrt_gettid()) return LERR(EPERM);
    if (!mem_ok(data, (size_t)words * 12, false)) return LERR(EFAULT);
    const uint32_t *d = (const uint32_t *)(uintptr_t)data;
    uint64_t e = 0, p = 0, i = 0;
    for (int w = 0; w < words; w++) {
        e |= (uint64_t)d[w * 3 + 0] << (32 * w);
        p |= (uint64_t)d[w * 3 + 1] << (32 * w);
        i |= (uint64_t)d[w * 3 + 2] << (32 * w);
    }
    e &= CAP_ALL; p &= CAP_ALL; i &= CAP_ALL;
    if ((i & ~(g.inh | g.prm)) && !cap_has(g.eff, CAP_SETPCAP)) return LERR(EPERM);
    if (i & ~(g.inh | g.bnd)) return LERR(EPERM);
    if (p & ~g.prm) return LERR(EPERM);
    if (e & ~p) return LERR(EPERM);
    g.eff = e; g.prm = p; g.inh = i;
    g.amb &= p & i;
    return 0;
}

static bool do_prctl(uint64_t op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, long *ret)
{
    switch (op) {
    case 38:                                   // PR_SET_NO_NEW_PRIVS: one way only
        if (a1 != 1 || a2 || a3 || a4) *ret = LERR(EINVAL);
        else { g.nnp = true; *ret = 0; }
        return true;
    case 39:                                   // PR_GET_NO_NEW_PRIVS
        *ret = (a1 || a2 || a3 || a4) ? LERR(EINVAL) : (long)g.nnp;
        return true;
    case 7:                                    // PR_GET_KEEPCAPS
        *ret = (g.securebits >> SECURE_KEEP_CAPS) & 1;
        return true;
    case 8:                                    // PR_SET_KEEPCAPS
        if (a1 > 1) { *ret = LERR(EINVAL); return true; }
        if (g.securebits & (1u << (SECURE_KEEP_CAPS + 1))) { *ret = LERR(EPERM); return true; }  // locked
        if (a1) g.securebits |= 1u << SECURE_KEEP_CAPS;
        else g.securebits &= ~(1u << SECURE_KEEP_CAPS);
        *ret = 0;
        return true;
    case 23:                                   // PR_CAPBSET_READ
        *ret = a1 > CAP_LAST_CAP ? LERR(EINVAL) : (long)((g.bnd >> a1) & 1);
        return true;
    case 24:                                   // PR_CAPBSET_DROP
        if (!cap_has(g.eff, CAP_SETPCAP)) *ret = LERR(EPERM);
        else if (a1 > CAP_LAST_CAP) *ret = LERR(EINVAL);
        else { g.bnd &= ~(1ull << a1); *ret = 0; }
        return true;
    case 27:                                   // PR_GET_SECUREBITS
        *ret = g.securebits;
        return true;
    case 28: {                                 // PR_SET_SECUREBITS
        // security/commoncap.c: a locked bit cannot change, a lock cannot
        // be released, unknown bits and a missing CAP_SETPCAP are EPERM.
        const uint32_t locks = 0xaa, bits = 0x55;
        uint32_t old = g.securebits, nw = (uint32_t)a1;
        if ((((old & locks) >> 1) & (old ^ nw)) || (old & locks & ~nw) ||
            (a1 & ~(uint64_t)(locks | bits)) || !cap_has(g.eff, CAP_SETPCAP))
            *ret = LERR(EPERM);
        else { g.securebits = nw; *ret = 0; }
        return true;
    }
    case 47:                                   // PR_CAP_AMBIENT
        if (a1 == 4) {                         // CLEAR_ALL
            *ret = (a2 || a3 || a4) ? LERR(EINVAL) : (g.amb = 0, 0);
            return true;
        }
        if (a3 || a4 || a2 > CAP_LAST_CAP) { *ret = LERR(EINVAL); return true; }
        if (a1 == 1) *ret = (long)((g.amb >> a2) & 1);
        else if (a1 == 2) {                    // RAISE
            if (!cap_has(g.prm, (int)a2) || !cap_has(g.inh, (int)a2) ||
                (g.securebits & (1u << SECURE_NO_CAP_AMBIENT_RAISE)))
                *ret = LERR(EPERM);
            else { g.amb |= 1ull << a2; *ret = 0; }
        } else if (a1 == 3) { g.amb &= ~(1ull << a2); *ret = 0; }
        else *ret = LERR(EINVAL);
        return true;
    }
    return false;
}

bool lxrt_aids_syscall(long nr, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3,
                       uint64_t a4, long *ret)
{
    if (!lxrt_aids_on()) return false;
    bool handled = true;
    pthread_mutex_lock(&g_mu);
    uint32_t old_e = g.e, old_eg = g.eg, old_r = g.r, old_rg = g.rg;
    uint64_t old_eff = g.eff, old_prm = g.prm, old_inh = g.inh;
    switch (nr) {
    case 174: *ret = g.r; break;               // getuid
    case 175: *ret = g.e; break;               // geteuid
    case 176: *ret = g.rg; break;              // getgid
    case 177: *ret = g.eg; break;              // getegid
    case 148:                                  // getresuid
    case 150: {                                // getresgid
        uint32_t v[3] = { nr == 148 ? g.r : g.rg, nr == 148 ? g.e : g.eg, nr == 148 ? g.s : g.sg };
        uint64_t p[3] = { a0, a1, a2 };
        *ret = 0;
        for (int i = 0; i < 3; i++) {
            if (!mem_ok(p[i], 4, true)) { *ret = LERR(EFAULT); break; }
            memcpy((void *)(uintptr_t)p[i], &v[i], 4);
        }
        break;
    }
    case 147: *ret = do_setresuid((uint32_t)a0, (uint32_t)a1, (uint32_t)a2); break;
    case 145: *ret = do_setreuid((uint32_t)a0, (uint32_t)a1); break;
    case 146: *ret = do_setuid((uint32_t)a0); break;
    case 149: *ret = do_setresgid((uint32_t)a0, (uint32_t)a1, (uint32_t)a2); break;
    case 143: *ret = do_setregid((uint32_t)a0, (uint32_t)a1); break;
    case 144: *ret = do_setgid((uint32_t)a0); break;
    case 151: {                                // setfsuid: returns the old one
        uint32_t u = (uint32_t)a0;
        *ret = g.fs;
        if (u != 0xffffffffu && (cap_has(g.eff, CAP_SETUID) || u == g.r || u == g.e || u == g.s || u == g.fs))
            g.fs = u;
        break;
    }
    case 152: {
        uint32_t v = (uint32_t)a0;
        *ret = g.fsg;
        if (v != 0xffffffffu && (cap_has(g.eff, CAP_SETGID) || v == g.rg || v == g.eg || v == g.sg || v == g.fsg))
            g.fsg = v;
        break;
    }
    case 158: {                                // getgroups
        int n = (int)a0;
        if (n < 0) { *ret = LERR(EINVAL); break; }
        if (n == 0) { *ret = g.ngroups; break; }
        if (n < g.ngroups) { *ret = LERR(EINVAL); break; }
        if (g.ngroups && !mem_ok(a1, (size_t)g.ngroups * 4, true)) { *ret = LERR(EFAULT); break; }
        if (g.ngroups) memcpy((void *)(uintptr_t)a1, g.groups, (size_t)g.ngroups * 4);
        *ret = g.ngroups;
        break;
    }
    case 159: {                                // setgroups
        long n = (long)(int32_t)a0;
        if (!cap_has(g.eff, CAP_SETGID)) { *ret = LERR(EPERM); break; }
        if (n < 0 || n > 65536) { *ret = LERR(EINVAL); break; }
        if (n > MAX_GROUPS) { *ret = LERR(ENOMEM); break; }
        if (n && !mem_ok(a1, (size_t)n * 4, false)) { *ret = LERR(EFAULT); break; }
        if (n) memcpy(g.groups, (const void *)(uintptr_t)a1, (size_t)n * 4);
        g.ngroups = (int)n;
        qsort(g.groups, (size_t)n, sizeof g.groups[0], cmp_u32);
        *ret = 0;
        break;
    }
    case 90: *ret = do_cap(false, a0, a1); break;
    case 91: *ret = do_cap(true, a0, a1); break;
    case 167:                                  // prctl: the capability options
        handled = do_prctl(a0, a1, a2, a3, a4, ret);
        break;
    // Namespaces and mounts. There is one mount view and no other
    // namespaces; zygote makes a private mount namespace and marks "/" a
    // slave before any fork (com_android_internal_os_Zygote.cpp
    // UnmountStorageOnInit, each fatal). A process with CAP_SYS_ADMIN is
    // told that happened: a namespace identical to the one it had is what
    // it gets, and a propagation change has nothing to propagate. A mount
    // that would change what paths show (bind, tmpfs, a new filesystem) is
    // not emulated and fails; umount2 finds no mount point (EINVAL).
    case 97: {                                 // unshare
        const uint64_t ns = 0x00020000 | 0x02000000 | 0x04000000 | 0x08000000 |
                            0x10000000 | 0x20000000 | 0x40000000;   // NEWNS..NEWNET
        const uint64_t plain = 0x200 | 0x40000;                     // CLONE_FS, CLONE_SYSVSEM
        // CLONE_FILES would need a descriptor table of the thread's own,
        // which a Darwin thread cannot have (runtime/thread.c refuses a
        // thread without CLONE_FILES for the same reason).
        if (a0 & ~(ns | plain)) { *ret = LERR(EINVAL); break; }
        if ((a0 & ns & ~0x10000000ull) && !cap_has(g.eff, 21)) { *ret = LERR(EPERM); break; }
        *ret = 0;
        break;
    }
    case 40: {                                 // mount(src, target, type, flags, data)
        const uint64_t prop = (1u << 17) | (1u << 18) | (1u << 19) | (1u << 20);  // UNBINDABLE..SHARED
        if (!cap_has(g.eff, 21)) { *ret = LERR(EPERM); break; }
        if (a3 & (0x1000 /* MS_BIND */ | 0x20 /* MS_REMOUNT */)) {
            handled = false;                   // dispatch.c: lxrt_aids_mount
            break;
        }
        if ((a3 & prop) && !(a3 & ~(prop | 0x4000 /* MS_REC */ | 0x8000 /* MS_SILENT */)))
            *ret = 0;
        else {
            if (getenv("LXRT_ANDROID_IDS_LOG"))
                fprintf(stderr, "[lxrt] pid %d: mount(\"%s\", \"%s\", \"%s\", 0x%llx) not emulated -> EPERM\n",
                        (int)getpid(), a0 ? (const char *)(uintptr_t)a0 : "", a1 ? (const char *)(uintptr_t)a1 : "",
                        a2 ? (const char *)(uintptr_t)a2 : "", (unsigned long long)a3);
            *ret = LERR(EPERM);
        }
        break;
    }
    case 39:                                   // umount2: dispatch.c (binds made here can go)
        if (!cap_has(g.eff, 21)) *ret = LERR(EPERM);
        else handled = false;
        break;
    case 268:                                  // setns: there is no other namespace to join
        *ret = LERR(EINVAL);
        break;
    default:
        handled = false;
    }
    if (g.e != old_e || g.eg != old_eg || g.r != old_r || g.rg != old_rg ||
        g.eff != old_eff || g.prm != old_prm || g.inh != old_inh)
        publish();                             // under g_mu: one writer of the slot
    pthread_mutex_unlock(&g_mu);
    return handled;
}

// ---- virtual file ownership (android_ids.h) ----
static bool owner_read(const char *host, int fd, bool nofollow, uint32_t *uid, uint32_t *gid)
{
    char buf[32];
    ssize_t n = host ? getxattr(host, LXRT_OWNER_XATTR, buf, sizeof buf - 1, 0, nofollow ? XATTR_NOFOLLOW : 0)
                     : fgetxattr(fd, LXRT_OWNER_XATTR, buf, sizeof buf - 1, 0, 0);
    if (n <= 0) return false;
    buf[n] = 0;
    unsigned u, gg;
    if (sscanf(buf, "%u:%u", &u, &gg) != 2) return false;
    *uid = u;
    *gid = gg;
    return true;
}

void lxrt_aids_fix_stat(const char *host, int fd, bool nofollow, struct stat *st)
{
    if (!lxrt_aids_on() || (!host && fd < 0)) return;
    uint32_t u, gg;
    if (owner_read(host, fd, nofollow, &u, &gg)) {
        st->st_uid = u;
        st->st_gid = gg;
    }
}

// A path the xattr calls can take: relative paths joined to the directory
// descriptor's own path (Darwin has no getxattrat).
static const char *at_path(int ddirfd, const char *host, char *buf, size_t n)
{
    if (!host || host[0] == '/' || ddirfd == AT_FDCWD) return host;
    const char *pf = lxrt_pathfd_path(ddirfd);
    char dir[PATH_MAX];
    if (pf) snprintf(dir, sizeof dir, "%s", pf);
    else if (fcntl(ddirfd, F_GETPATH, dir) != 0) return NULL;
    if (snprintf(buf, n, "%s/%s", dir, host) >= (int)n) return NULL;
    return buf;
}

void lxrt_aids_fix_stat_at(int ddirfd, const char *host, bool nofollow, struct stat *st)
{
    if (!lxrt_aids_on()) return;
    char buf[PATH_MAX];
    const char *p = at_path(ddirfd, host, buf, sizeof buf);
    if (p) lxrt_aids_fix_stat(p, -1, nofollow, st);
}

static int owner_write(const char *host, int fd, bool nofollow, const char *val)
{
    int xo = host && nofollow ? XATTR_NOFOLLOW : 0;
    if (!val)
        return host ? removexattr(host, LXRT_OWNER_XATTR, xo) : fremovexattr(fd, LXRT_OWNER_XATTR, 0);
    return host ? setxattr(host, LXRT_OWNER_XATTR, val, strlen(val), 0, xo)
                : fsetxattr(fd, LXRT_OWNER_XATTR, val, strlen(val), 0, 0);
}

long lxrt_aids_chown_file(const char *host, int fd, bool nofollow, uint32_t uid, uint32_t gid)
{
    const uint32_t N = 0xffffffffu;
    struct stat st;
    if ((host ? (nofollow ? lstat(host, &st) : stat(host, &st)) : fstat(fd, &st)) != 0)
        return LERR(errno);
    uint32_t cu = (uint32_t)st.st_uid, cg = (uint32_t)st.st_gid;
    bool recorded = owner_read(host, fd, nofollow, &cu, &cg);
    pthread_mutex_lock(&g_mu);
    bool capchown = cap_has(g.eff, CAP_CHOWN);
    uint32_t fs = g.fs, fsg = g.fsg;
    bool member = gid == N || gid == fsg;
    for (int i = 0; i < g.ngroups && !member; i++) member = g.groups[i] == gid;
    pthread_mutex_unlock(&g_mu);
    if (!recorded) {
        // A file nobody chowned is the Mac user's, i.e. every guest's: as
        // before, the caller counts as its owner.
        cu = fs;
        cg = fsg;
    }
    if (!capchown) {
        if (uid != N && (uid != cu || fs != cu)) return LERR(EPERM);
        if (gid != N && gid != cg && (fs != cu || !member)) return LERR(EPERM);
    }
    uint32_t nu = uid == N ? cu : uid, ng = gid == N ? cg : gid;
    char val[32];
    const char *v = NULL;                      // back to the Mac's own ids: no record
    if (nu != (uint32_t)getuid() || ng != (uint32_t)getgid()) {
        snprintf(val, sizeof val, "%u:%u", nu, ng);
        v = val;
    }
    if (!v && !recorded) return 0;
    int r = owner_write(host, fd, nofollow, v);
    if (r != 0 && (errno == EACCES || errno == EPERM) && !S_ISLNK(st.st_mode)) {
        // Darwin wants write permission for an xattr; Linux's chown with
        // CAP_CHOWN does not. Lend the owner write permission for the call.
        int m = st.st_mode & 07777;
        if ((host ? chmod(host, (mode_t)(m | S_IWUSR)) : fchmod(fd, (mode_t)(m | S_IWUSR))) == 0) {
            r = owner_write(host, fd, nofollow, v);
            int e = errno;
            if (host) chmod(host, (mode_t)m); else fchmod(fd, (mode_t)m);
            errno = e;
        }
    }
    if (r != 0) {
        if (!v && errno == ENOATTR) return 0;
        // A filesystem without extended attributes: the old answer, success
        // with nothing recorded.
        if (errno == ENOTSUP) return 0;
        return LERR(errno);
    }
    return 0;
}

long lxrt_aids_chown_at(int ddirfd, const char *host, bool nofollow, uint32_t uid, uint32_t gid)
{
    char buf[PATH_MAX];
    const char *p = at_path(ddirfd, host, buf, sizeof buf);
    if (!p) return host ? LERR(ENAMETOOLONG) : LERR(EFAULT);
    return lxrt_aids_chown_file(p, -1, nofollow, uid, gid);
}

const char *lxrt_aids_exec_env(void)
{
    if (!lxrt_aids_on()) return NULL;
    static char buf[64 + MAX_GROUPS * 11 + 128];
    pthread_mutex_lock(&g_mu);
    struct ids n = g;
    pthread_mutex_unlock(&g_mu);
    // cap_bprm_creds_from_file for a file without capabilities or set-id bits.
    n.s = n.fs = n.e;
    n.sg = n.fsg = n.eg;
    n.securebits &= ~(1u << SECURE_KEEP_CAPS);
    uint64_t prm = 0;
    bool effective = false;
    if (!(n.securebits & (1u << SECURE_NOROOT)) && (n.e == 0 || n.r == 0)) {
        prm = n.bnd | n.inh;                   // handle_privileged_root
        effective = n.e == 0;
    }
    prm |= n.amb;
    n.prm = prm;
    n.eff = effective ? prm : n.amb;
    int o = snprintf(buf, sizeof buf, "LXRT_ANDROID_IDS=%u,%u,%u:%u,%u,%u:", n.r, n.e, n.s, n.rg, n.eg, n.sg);
    for (int i = 0; i < n.ngroups && o < (int)sizeof buf - 16; i++)
        o += snprintf(buf + o, sizeof buf - (size_t)o, "%s%u", i ? "," : "", n.groups[i]);
    o += snprintf(buf + o, sizeof buf - (size_t)o, ":%llx,%llx,%llx,%llx,%llx",
                  (unsigned long long)n.eff, (unsigned long long)n.prm, (unsigned long long)n.inh,
                  (unsigned long long)n.bnd, (unsigned long long)n.amb);
    if (n.nnp || n.securebits)
        snprintf(buf + o, sizeof buf - (size_t)o, ":%sb%x", n.nnp ? "n" : "", n.securebits);
    return buf;
}
