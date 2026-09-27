// memfd_create, execveat and rt_sigtimedwait.
//
// These are the three calls FEX makes for itself rather than on the guest's
// behalf, so all three fail before a single x86 instruction is translated.
// None of them has a Darwin equivalent to forward to, and each fails in a
// different way:
//
//   * memfd_create asks for a file with no name. Darwin's nearest object,
//     POSIX shared memory, is not a file -- measured below.
//   * execveat asks the kernel to execute an open fd. Darwin has no execveat,
//     no fexecve, and -- measured below -- no /proc/self/fd/N equivalent that
//     can be walked through.
//   * rt_sigtimedwait asks to dequeue a pending signal with a deadline. Darwin
//     has sigwait, which has no deadline, and -- measured below -- a kqueue
//     signal filter that cannot see the only kind of signal this runtime ever
//     sends.
//
// Every number in here that could plausibly have been passed through has been
// checked against both systems, because the bugs this project keeps hitting
// are the ones where the low range agrees.

#include "lxrt.h"
#include "fex_support.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))

bool lxrt_trace_on(void);

// ------------------------------------------------------------ guest memory
//
// A guest pointer is a number the guest chose. Dereferencing one that is not
// mapped kills the host process, which from the guest's point of view is the
// runtime vanishing rather than a syscall returning -EFAULT. The Mach region
// map is the only way to ask "is this readable" without trying it.

static bool mem_ok(uint64_t addr, size_t len, vm_prot_t need)
{
    if (!addr || !len)
        return false;
    uint64_t end = addr + len;
    if (end < addr)
        return false;               // wrapped; no mapping can satisfy it

    uint64_t at = addr;
    while (at < end) {
        mach_vm_address_t ra = at;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
            return false;
        // mach_vm_region reports the first region at or AFTER the address, so a
        // region starting past `at` means `at` itself is in a hole.
        if (ra > at || rs == 0)
            return false;
        if ((info.protection & need) != need)
            return false;
        at = ra + rs;
    }
    return true;
}

// strnlen over guest memory. A plain strnlen walks off the end of a valid
// mapping into an unmapped one, so readability is rechecked at every host page
// boundary crossed. Returns false on fault; *out_len == max means "no NUL
// within max bytes", which every caller treats as too long rather than as a
// string.
static bool guest_strnlen(const char *p, size_t max, size_t *out_len)
{
    if (!p)
        return false;
    uint64_t a = (uint64_t)(uintptr_t)p;
    uint64_t checked = 0;
    for (size_t n = 0; n < max; n++) {
        uint64_t page = LXRT_ALIGN_DOWN(a + n, LXRT_HOST_PAGE);
        if (page != checked || n == 0) {
            if (!mem_ok(page, 1, VM_PROT_READ))
                return false;
            checked = page;
        }
        if (p[n] == '\0') {
            *out_len = n;
            return true;
        }
    }
    *out_len = max;
    return true;
}

// The timebase is published once, under pthread_once. The lazy form it replaces
// wrote tb.numer and tb.denom as two unsynchronised stores behind an
// `if (tb.denom == 0)` test, so a second thread arriving in that window could
// read denom set and numer still 0 and get 0 ns back -- which makes every
// deadline computed from it look already expired.
static mach_timebase_info_data_t g_tb;
static pthread_once_t            g_tb_once = PTHREAD_ONCE_INIT;

static void tb_init(void) { mach_timebase_info(&g_tb); }

static uint64_t mono_ns(void)
{
    pthread_once(&g_tb_once, tb_init);
    uint64_t t = mach_absolute_time();
    // numer/denom is 125/3 on this machine, so the plain t * numer overflows 64
    // bits at roughly 195 years of uptime. Not a live concern, but the split
    // multiply is exact and costs nothing; verified to agree with the plain
    // form bit for bit on the current timebase.
    return (t / g_tb.denom) * g_tb.numer +
           (t % g_tb.denom) * g_tb.numer / g_tb.denom;
}

// Linux aarch64 timespec is two signed 64-bit fields. Darwin's struct timespec
// is time_t + long, which on arm64 is also 8 + 8, but the types are written out
// rather than cast so a future 32-bit target does not silently reinterpret it.
struct linux_timespec { int64_t tv_sec; int64_t tv_nsec; };

// ============================================================================
// 279  memfd_create(const char *name, unsigned int flags)
// ============================================================================
//
// Linux hands back a file descriptor for an anonymous, resizable, mappable
// file living in tmpfs. Darwin has nothing that is all four things.
//
// shm_open + immediate shm_unlink was the obvious candidate and it does leave a
// usable descriptor, but it is not a file. Measured on macOS 15 / arm64:
//
//   ftruncate(fd, 4096)   -> 0        first call only
//   ftruncate(fd, 8192)   -> EINVAL   a POSIX shm object is sized exactly once
//   ftruncate(fd, 4096)   -> EINVAL
//   fstat  st_size        -> 16384    rounded up to the host page, not 4096
//   pread / pwrite        -> ESPIPE   it is not seekable, so it is not readable
//   fcntl(F_GETPATH)      -> EBADF
//
// Three of those are fatal for a memfd. Callers grow a memfd repeatedly (that
// is the point of F_SEAL_GROW existing), they stat it to learn its exact size,
// and they write() into it -- FEX writes its thunk images that way. So the
// fallback the brief allowed is the implementation: mkstemp in the runtime's
// temp directory, unlinked before anything else touches it. Measured on the
// same machine, all of ftruncate grow, ftruncate shrink, exact fstat size,
// pread/pwrite, mmap MAP_SHARED and reopen through /dev/fd/N work.
//
// What is lost against Linux: the file is backed by the real filesystem rather
// than by tmpfs, so a large memfd is written to disk instead of staying in page
// cache, and it is visible in the directory for the instant between mkstemp and
// unlink. Nothing correctness-bearing depends on either.

// Linux MFD_* (include/uapi/linux/memfd.h). Darwin has no counterpart to any
// of them, so these are matched here rather than translated.
#define L_MFD_CLOEXEC       0x0001u
#define L_MFD_ALLOW_SEALING 0x0002u
#define L_MFD_HUGETLB       0x0004u
#define L_MFD_NOEXEC_SEAL   0x0008u   // Linux 6.3
#define L_MFD_EXEC          0x0010u   // Linux 6.3
#define L_MFD_KNOWN (L_MFD_CLOEXEC | L_MFD_ALLOW_SEALING | L_MFD_HUGETLB | \
                     L_MFD_NOEXEC_SEAL | L_MFD_EXEC)

// The kernel rejects a name longer than NAME_MAX minus the "memfd:" prefix it
// prepends: 255 - 6 = 249.
#define L_MFD_NAME_MAX 249

// ---------------------------------------------------------------- the seals
//
// F_ADD_SEALS and F_GET_SEALS are Linux-only fcntl commands and there is no
// Darwin mechanism behind them -- not a weaker one, none. Darwin's own fcntl
// command space currently tops out at 116 (F_RDADVISEV), so Linux's 1033/1034
// cannot collide with anything and are safe to claim.
//
// They are implemented as bookkeeping in this runtime: the seal bits are
// recorded per object and the operations they forbid are refused here, before
// the Darwin call that would otherwise succeed.
//
// SAY THIS PLAINLY, because a caller that trusts seals is making a security
// decision with them. On Linux a seal is enforced by the kernel on the inode,
// which is why it is safe to receive a memfd over a socket from a process you
// do not trust and rely on F_SEAL_WRITE to mean the bytes cannot change under
// you. Here it is enforced by a table inside one process. Three consequences:
//
//   * A different process holding the same fd -- passed by SCM_RIGHTS, or
//     simply inherited across fork -- is not bound by these seals at all. It
//     holds an ordinary writable file.
//   * Even in this process, any path that reaches the fd without calling the
//     lxrt_memfd_check_* gates below writes straight through.
//   * The file has a real name for the instant before it is unlinked, so it is
//     not unreachable the way a Linux memfd is.
//
// A guest using seals as a local invariant gets what it expects. A guest using
// them as a trust boundary is getting a weaker promise than it thinks, and
// nothing in this runtime can close that gap without kernel support.

#define L_F_ADD_SEALS 1033   // F_LINUX_SPECIFIC_BASE(1024) + 9
#define L_F_GET_SEALS 1034   // F_LINUX_SPECIFIC_BASE(1024) + 10

#define L_F_SEAL_SEAL         0x0001u
#define L_F_SEAL_SHRINK       0x0002u
#define L_F_SEAL_GROW         0x0004u
#define L_F_SEAL_WRITE        0x0008u
#define L_F_SEAL_FUTURE_WRITE 0x0010u  // Linux 5.1
#define L_F_SEAL_EXEC         0x0020u  // Linux 6.3
#define L_F_SEAL_KNOWN (L_F_SEAL_SEAL | L_F_SEAL_SHRINK | L_F_SEAL_GROW | \
                        L_F_SEAL_WRITE | L_F_SEAL_FUTURE_WRITE | L_F_SEAL_EXEC)

#define MEMFD_MAX 256

// One entry per fd; several fds can name one object, because dup() on Linux
// produces a second descriptor over the same inode and the seals are on the
// inode.
struct memfd_ent { bool used; int fd; int obj; };
struct memfd_obj { int refs; uint32_t seals; };

static struct memfd_ent g_memfd[MEMFD_MAX];
static struct memfd_obj g_memobj[MEMFD_MAX];
static pthread_mutex_t  g_memfd_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(fex_support_g_memfd_lock, g_memfd_lock)

// Caller holds g_memfd_lock.
static struct memfd_ent *find_ent(int fd)
{
    if (fd < 0)
        return NULL;
    for (int i = 0; i < MEMFD_MAX; i++)
        if (g_memfd[i].used && g_memfd[i].fd == fd)
            return &g_memfd[i];
    return NULL;
}

static bool add_ent(int fd, int obj)
{
    for (int i = 0; i < MEMFD_MAX; i++) {
        if (g_memfd[i].used)
            continue;
        g_memfd[i].used = true;
        g_memfd[i].fd = fd;
        g_memfd[i].obj = obj;
        g_memobj[obj].refs++;
        return true;
    }
    return false;
}

static char           g_memfd_dir[512];
static bool           g_memfd_dir_ok;
static pthread_once_t g_memfd_dir_once = PTHREAD_ONCE_INIT;

static void memfd_dir_init(void)
{
    // TMPDIR is per-user and per-session on macOS; /tmp is the documented
    // fallback and is what a sandboxed process without TMPDIR gets.
    const char *tmp = getenv("TMPDIR");
    if (!tmp || !*tmp)
        tmp = "/tmp";
    size_t n = strlen(tmp);
    int w = snprintf(g_memfd_dir, sizeof g_memfd_dir, "%s%s", tmp,
                     (n && tmp[n - 1] == '/') ? "" : "/");
    if (w > 0 && (size_t)w < sizeof g_memfd_dir)
        g_memfd_dir_ok = true;
}

// pthread_once rather than the `tried`/`ready` pair this replaces. That pair
// set `tried` BEFORE the buffer was filled, so a second thread entering the
// window took the fast path, read a not-yet-ready `ready`, got NULL, and
// lxrt_memfd_create returned -ENOENT for a perfectly valid call. Measured: 32
// threads racing lxrt_memfd_create produced two such -ENOENT returns in 4 of 12
// runs. Every other shared table in this file is already lock-protected.
static const char *memfd_dir(void)
{
    pthread_once(&g_memfd_dir_once, memfd_dir_init);
    return g_memfd_dir_ok ? g_memfd_dir : NULL;
}

long lxrt_memfd_create(const char *name, unsigned int lflags)
{
    if (lflags & ~L_MFD_KNOWN)
        return LERR(EINVAL);        // Linux rejects unknown bits outright
    if (lflags & L_MFD_HUGETLB) {
        // Darwin has no huge-page file backing and no way to approximate one.
        // Succeeding with 16 KiB pages would give the caller a file that is
        // silently the wrong thing; superpages here are an mmap hint, not a
        // property a descriptor can carry.
        return LERR(ENOSYS);
    }

    size_t nlen = 0;
    if (!guest_strnlen(name, L_MFD_NAME_MAX + 1, &nlen))
        return LERR(EFAULT);
    if (nlen > L_MFD_NAME_MAX)
        return LERR(EINVAL);

    const char *dir = memfd_dir();
    if (!dir)
        return LERR(ENOENT);

    // The name is a label on Linux, shown as "memfd:<name>", and need not be
    // unique. Here it has to become part of a real filename, so '/' and
    // anything unprintable is folded away and mkstemp supplies the uniqueness.
    char label[64];
    size_t k = 0;
    for (size_t i = 0; i < nlen && k + 1 < sizeof label; i++) {
        unsigned char c = (unsigned char)name[i];
        label[k++] = (c == '/' || c < 0x20 || c > 0x7e) ? '_' : (char)c;
    }
    label[k] = '\0';

    char path[1024];
    int w = snprintf(path, sizeof path, "%smemfd-%s-XXXXXX", dir, label);
    if (w < 0 || (size_t)w >= sizeof path)
        return LERR(ENAMETOOLONG);

    int fd = mkstemp(path);
    if (fd < 0)
        return LERR(errno);
    // Unlink first and check second: the window in which the name exists has to
    // be as short as possible, and a failure to unlink means this is not an
    // anonymous file and must not be handed back as one.
    if (unlink(path) != 0) {
        int e = errno;
        close(fd);
        return LERR(e);
    }
    // mkstemp always creates with O_RDWR and without O_CLOEXEC; Linux's memfd
    // is also always readable and writable, so only the CLOEXEC bit is a
    // decision. Note the flag values differ: Linux MFD_CLOEXEC is 1 and Linux
    // FD_CLOEXEC is 1, but Darwin FD_CLOEXEC is also 1 -- the coincidence holds
    // here and is used deliberately rather than by accident.
    if (lflags & L_MFD_CLOEXEC)
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    // mkstemp makes the file 0600; Linux memfds are 0777 & ~umask on tmpfs but
    // nothing can open them by name, so the mode is unobservable either way.

    pthread_mutex_lock(&g_memfd_lock);
    int obj = -1;
    for (int i = 0; i < MEMFD_MAX; i++)
        if (g_memobj[i].refs == 0) { obj = i; break; }
    if (obj < 0 || !add_ent(fd, obj)) {
        pthread_mutex_unlock(&g_memfd_lock);
        close(fd);
        return LERR(EMFILE);        // out of seal slots, not out of fds
    }
    // Linux: without MFD_ALLOW_SEALING the file starts with F_SEAL_SEAL set, so
    // F_ADD_SEALS fails with EPERM forever after. That is the default, not an
    // error, and callers depend on being able to read it back.
    g_memobj[obj].seals = (lflags & L_MFD_ALLOW_SEALING) ? 0u : L_F_SEAL_SEAL;
    // MFD_NOEXEC_SEAL means "create it non-executable and seal that in". The
    // exec seal is recorded so F_GET_SEALS round-trips; it is not enforced,
    // because nothing in this runtime execs a memfd directly -- lxrt_execveat
    // re-executes the runtime, which reads the file rather than executing it.
    if (lflags & L_MFD_NOEXEC_SEAL)
        g_memobj[obj].seals |= L_F_SEAL_EXEC;
    pthread_mutex_unlock(&g_memfd_lock);

    return fd;
}

bool lxrt_memfd_is(int fd)
{
    pthread_mutex_lock(&g_memfd_lock);
    bool yes = find_ent(fd) != NULL;
    pthread_mutex_unlock(&g_memfd_lock);
    return yes;
}

// A memfd of any process: an unlinked file whose name the runtime made
// "memfd-<label>-XXXXXX". A descriptor received over a socket is not in this
// process's table, and wineserver hands every section out that way. (The
// directory is the creator's TMPDIR, not necessarily this process's.)
bool lxrt_memfd_path_is(int fd)
{
    struct stat st;
    char path[PATH_MAX];
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_nlink != 0 ||
        fcntl(fd, F_GETPATH, path) != 0)
        return false;
    const char *base = strrchr(path, '/');
    return strncmp(base ? base + 1 : path, "memfd-", 6) == 0;
}

void lxrt_memfd_track_dup(int oldfd, int newfd)
{
    if (oldfd == newfd)
        return;                     // dup2(fd, fd) is a no-op on Linux
    pthread_mutex_lock(&g_memfd_lock);
    struct memfd_ent *e = find_ent(oldfd);
    if (e) {
        // A recycled fd number that used to be a memfd would otherwise keep the
        // old object's seals.
        struct memfd_ent *stale = find_ent(newfd);
        if (stale) {
            // Clamped the way lxrt_memfd_close clamps. Without the clamp a refs
            // that had already gone negative stayed negative, and the free-slot
            // scan in lxrt_memfd_create looks for refs == 0 exactly -- so the
            // object would never be reclaimed and one of the 256 slots leaked
            // for the life of the process.
            if (--g_memobj[stale->obj].refs <= 0) {
                g_memobj[stale->obj].refs = 0;
                g_memobj[stale->obj].seals = 0;
            }
            stale->used = false;
            e = find_ent(oldfd);    // find_ent returns a table slot; re-look up
        }
        if (e)
            add_ent(newfd, e->obj);
    }
    pthread_mutex_unlock(&g_memfd_lock);
}

void lxrt_memfd_close(int fd)
{
    pthread_mutex_lock(&g_memfd_lock);
    struct memfd_ent *e = find_ent(fd);
    if (e) {
        if (--g_memobj[e->obj].refs <= 0) {
            g_memobj[e->obj].refs = 0;
            g_memobj[e->obj].seals = 0;
        }
        e->used = false;
    }
    pthread_mutex_unlock(&g_memfd_lock);
}

// Returns the seal word, or 0 for an fd that is not ours.
static uint32_t seals_of(int fd)
{
    pthread_mutex_lock(&g_memfd_lock);
    struct memfd_ent *e = find_ent(fd);
    uint32_t s = e ? g_memobj[e->obj].seals : 0u;
    pthread_mutex_unlock(&g_memfd_lock);
    return s;
}

long lxrt_memfd_fcntl(int fd, int lcmd, unsigned long arg, bool *handled)
{
    if (lcmd != L_F_ADD_SEALS && lcmd != L_F_GET_SEALS) {
        *handled = false;
        return 0;
    }
    *handled = true;

    pthread_mutex_lock(&g_memfd_lock);
    struct memfd_ent *e = find_ent(fd);
    if (!e) {
        pthread_mutex_unlock(&g_memfd_lock);
        // Linux returns EINVAL for a file whose filesystem does not support
        // sealing, which is every file here that is not a memfd.
        return LERR(EINVAL);
    }
    struct memfd_obj *o = &g_memobj[e->obj];

    if (lcmd == L_F_GET_SEALS) {
        long s = (long)o->seals;
        pthread_mutex_unlock(&g_memfd_lock);
        return s;
    }

    // Narrowing to 32 bits here is what Linux itself does: its F_ADD_SEALS
    // handler takes the argument as an unsigned int, so the high half is
    // discarded rather than rejected. Measured on the aarch64 guest (6.17.1):
    //   F_ADD_SEALS(0x100000000)        -> 0,  F_GET_SEALS 0x0
    //   F_ADD_SEALS(0xFFFFFFFF00000004) -> 0,  F_GET_SEALS 0x4
    //   F_ADD_SEALS(0x40)               -> EINVAL  (unknown bit in the low half)
    // So the truncation matches, and only an unknown bit in the low 32 is an
    // error. Rejecting wide arguments here would be a divergence, not a fix.
    uint32_t want = (uint32_t)arg;
    if (want & ~L_F_SEAL_KNOWN) {
        pthread_mutex_unlock(&g_memfd_lock);
        return LERR(EINVAL);
    }
    if (o->seals & L_F_SEAL_SEAL) {
        pthread_mutex_unlock(&g_memfd_lock);
        return LERR(EPERM);
    }
    // Linux also fails F_SEAL_WRITE with EBUSY when a shared writable mapping
    // of the file already exists, because it cannot revoke one. This runtime
    // does not track which guest mappings cover which fd, so that check is not
    // made: a guest that maps first and seals second gets a successful seal and
    // a mapping that can still write. Refusing every F_SEAL_WRITE would break
    // the common correct ordering, so the narrower gap is the one accepted.
    o->seals |= want;
    pthread_mutex_unlock(&g_memfd_lock);
    return 0;
}

long lxrt_memfd_check_ftruncate(int fd, int64_t length)
{
    uint32_t s = seals_of(fd);
    if (!s)
        return 0;
    if (s & L_F_SEAL_WRITE)
        return LERR(EPERM);         // any size change is a write
    if (!(s & (L_F_SEAL_SHRINK | L_F_SEAL_GROW)))
        return 0;
    struct stat st;
    if (fstat(fd, &st) != 0)
        return LERR(errno);
    if ((s & L_F_SEAL_SHRINK) && length < st.st_size)
        return LERR(EPERM);
    if ((s & L_F_SEAL_GROW) && length > st.st_size)
        return LERR(EPERM);
    return 0;
}

long lxrt_memfd_check_write(int fd, int64_t offset, uint64_t count)
{
    uint32_t s = seals_of(fd);
    if (!s)
        return 0;
    // F_SEAL_FUTURE_WRITE differs from F_SEAL_WRITE only in that mappings taken
    // out before the seal keep writing. Since the mapping-aware distinction is
    // not tracked here (see lxrt_memfd_fcntl), both stop a write() the same way.
    if (s & (L_F_SEAL_WRITE | L_F_SEAL_FUTURE_WRITE))
        return LERR(EPERM);
    if (!(s & L_F_SEAL_GROW) || count == 0)
        return 0;
    int64_t at = offset;
    if (at < 0) {
        off_t cur = lseek(fd, 0, SEEK_CUR);
        if (cur < 0)
            return LERR(errno);
        at = (int64_t)cur;
    }
    struct stat st;
    if (fstat(fd, &st) != 0)
        return LERR(errno);
    // Computed in uint64 rather than as `at + (int64_t)count`. That form was
    // signed overflow for a large count, and a count above INT64_MAX cast to a
    // negative int64, which made the comparison pass and let the write through
    // with the grow seal set -- the seal silently bypassed by the one argument
    // an attacker controls.
    uint64_t start = (uint64_t)at;
    uint64_t size  = st.st_size > 0 ? (uint64_t)st.st_size : 0u;
    // Linux under F_SEAL_GROW short-writes up to the existing size rather than
    // failing outright. Refusing the whole write is the conservative reading:
    // a caller that gets EPERM retries or reports, where a silent short write
    // that a caller does not check loses data.
    if (count > UINT64_MAX - start || start + count > size)
        return LERR(EPERM);
    return 0;
}

long lxrt_memfd_check_mmap(int fd, int prot, int lmap_flags)
{
    // PROT_READ/WRITE/EXEC are 1/2/4 on both Linux and Darwin, and MAP_SHARED
    // is 0x01 on both. MAP_ANONYMOUS is the one that differs (0x20 against
    // 0x1000) and is not consulted here.
    uint32_t s = seals_of(fd);
    if (!s)
        return 0;
    if ((s & (L_F_SEAL_WRITE | L_F_SEAL_FUTURE_WRITE)) &&
        (prot & PROT_WRITE) && (lmap_flags & MAP_SHARED))
        return LERR(EPERM);
    return 0;
}

// ============================================================================
// 281  execveat(int dirfd, const char *path, argv, envp, int flags)
// ============================================================================
//
// Darwin has no execveat and no fexecve. What it does have is lxrt_execve,
// which is the only thing that can start a Linux ELF here at all: a Linux ELF
// cannot be handed to Darwin's execve, so process.c re-executes the runtime
// with the guest binary as its first argument. execveat therefore reduces to
// "turn dirfd + path into a path string", and then it is the existing problem.
//
// Two Darwin facts shape the resolution, both measured:
//
//   * /dev/fd/N is a clone device, not a symlink into the namespace the way
//     Linux's /proc/self/fd/N is. open("/dev/fd/N") on a file fd works, but
//     open("/dev/fd/N/sub") on a DIRECTORY fd fails with ENOENT. So the Linux
//     trick of walking through a dirfd's /proc entry is unavailable and the
//     directory has to be named by its real path.
//   * fcntl(F_GETPATH) on an UNLINKED file still returns the path the file used
//     to have. It does not fail, it lies. Every use of F_GETPATH below is
//     therefore checked by stat'ing the answer back and comparing st_dev and
//     st_ino against the fd, which is the only way to tell a live path from a
//     stale one.

#define L_AT_FDCWD            (-100)   // Darwin's AT_FDCWD is -2
#define L_AT_SYMLINK_NOFOLLOW 0x0100
#define L_AT_EMPTY_PATH       0x1000   // no Darwin counterpart at all
#define L_AT_EXECVEAT_KNOWN (L_AT_SYMLINK_NOFOLLOW | L_AT_EMPTY_PATH)

// Longest argv/envp this will walk before giving up. Linux's real ceiling is
// MAX_ARG_STRINGS (0x7FFFFFFF) bounded by a total byte limit; the point of the
// bound here is only to stop a corrupt guest pointer walking memory forever.
#define ARGV_MAX 65536

// True if `path` currently names the same file the fd is open on.
static bool path_matches_fd(int fd, const char *path)
{
    struct stat a, b;
    if (fstat(fd, &a) != 0 || stat(path, &b) != 0)
        return false;
    return a.st_dev == b.st_dev && a.st_ino == b.st_ino;
}

// Validate a guest argv/envp: a NULL-terminated array of readable C strings.
// Only the array spine and the reachability of each string are checked; the
// contents are the guest's business.
static bool argv_ok(char *const *v)
{
    if (!v)
        return true;                // NULL envp is legal; lxrt_execve substitutes
    for (int i = 0; i < ARGV_MAX; i++) {
        if (!mem_ok((uint64_t)(uintptr_t)&v[i], sizeof(char *), VM_PROT_READ))
            return false;
        if (!v[i])
            return true;
        size_t n;
        if (!guest_strnlen(v[i], 1u << 20, &n))
            return false;
    }
    return false;                   // no terminator within the bound
}

// lxrt_execve RETURNS on failure -- process.c reports ENOENT, ENOEXEC and
// EACCES as ordinary outcomes -- and Linux's execveat leaves the caller's
// descriptors untouched when it fails. Without putting FD_CLOEXEC back, the
// guest's memfd (created with MFD_CLOEXEC, which is the usual way) stays
// permanently inheritable and leaks into every later exec and posix_spawn.
static void restore_cloexec(int fd, int fl)
{
    if (fd >= 0)
        fcntl(fd, F_SETFD, fl);
}

long lxrt_execveat(int dirfd, const char *path, char *const argv[],
                   char *const envp[], int lflags)
{
    if (lflags & ~L_AT_EXECVEAT_KNOWN)
        return LERR(EINVAL);

    size_t plen = 0;
    if (!guest_strnlen(path, PATH_MAX, &plen))
        return LERR(EFAULT);
    if (plen >= PATH_MAX)
        return LERR(ENAMETOOLONG);
    if (!argv || !argv_ok(argv) || !argv_ok(envp))
        return LERR(EFAULT);

    char resolved[PATH_MAX];
    // Held open across the exec when the target can only be named through
    // /dev/fd. -1 when no such fd is involved.
    int keep_fd = -1;
    // The fd whose FD_CLOEXEC was cleared below, and the flag word to put back
    // on any path that returns instead of exec'ing. -1 when nothing was
    // cleared.
    int cloexec_fd = -1;
    int cloexec_fl = 0;

    if (plen == 0) {
        // AT_EMPTY_PATH: execute the descriptor itself. This is what fexecve
        // compiles down to and what a caller that built a binary in a memfd
        // uses; FEX is exactly that caller.
        if (!(lflags & L_AT_EMPTY_PATH))
            return LERR(ENOENT);
        if (dirfd == L_AT_FDCWD)
            // An empty path against the cwd names a directory, and executing a
            // directory is EACCES on Linux.
            return LERR(EACCES);
        // Explicit, rather than leaning on fcntl to reject it: see the note in
        // the relative-path branch for why a negative dirfd that is not Linux's
        // AT_FDCWD must never reach a Darwin *at() call.
        if (dirfd < 0)
            return LERR(EBADF);
        if (fcntl(dirfd, F_GETFD) < 0)
            return LERR(EBADF);

        char via[PATH_MAX];
        if (fcntl(dirfd, F_GETPATH, via) == 0 && path_matches_fd(dirfd, via)) {
            snprintf(resolved, sizeof resolved, "%s", via);
        } else {
            // Unlinked, or renamed since it was opened -- the memfd case. The
            // fd itself is the only name it has.
            //
            // This is where re-executing the runtime costs something Linux does
            // not charge: the kernel's execveat holds the file open across the
            // exec itself, whereas here a second process has to reopen
            // /dev/fd/N, so N must survive the exec. Clearing FD_CLOEXEC is not
            // cosmetic -- a memfd created with MFD_CLOEXEC, which is the usual
            // way, would otherwise be closed before the new runtime could look
            // at it, and the exec would fail with ENOENT for no visible reason.
            int fl = fcntl(dirfd, F_GETFD);
            if (fl >= 0 && (fl & FD_CLOEXEC) &&
                fcntl(dirfd, F_SETFD, fl & ~FD_CLOEXEC) == 0) {
                cloexec_fd = dirfd;
                cloexec_fl = fl;
            }
            snprintf(resolved, sizeof resolved, "/dev/fd/%d", dirfd);
        }
    } else if (path[0] == '/') {
        snprintf(resolved, sizeof resolved, "%s", path);
    } else if (dirfd == L_AT_FDCWD) {
        // The re-executed runtime inherits this process's cwd, so a relative
        // path resolves against the same directory it would have on Linux.
        snprintf(resolved, sizeof resolved, "%s", path);
    } else {
        // Darwin's AT_FDCWD is -2 and Linux's is -100, so every other negative
        // dirfd has to be rejected before it reaches a Darwin *at() call.
        // Measured on both sides: openat(-2, "marker.txt") succeeds here and
        // resolves against the cwd, while the aarch64 guest's
        // execveat(-2, "marker.txt") returns EBADF. Letting it through executes
        // a wrong file where Linux reports an error. Absolute paths are handled
        // in the branch above and keep Linux's behaviour of ignoring dirfd
        // entirely -- measured: execveat(-7, "/bin/true") execs on Linux.
        if (dirfd < 0)
            return LERR(EBADF);
        char dirpath[PATH_MAX];
        if (fcntl(dirfd, F_GETPATH, dirpath) == 0 &&
            path_matches_fd(dirfd, dirpath)) {
            int w = snprintf(resolved, sizeof resolved, "%s/%s", dirpath, path);
            if (w < 0 || (size_t)w >= sizeof resolved)
                return LERR(ENAMETOOLONG);
        } else {
            // The directory has no usable name -- it was unlinked, or it lives
            // somewhere F_GETPATH cannot describe. openat() still resolves
            // through the fd because that work happens in the kernel, so the
            // target is opened here and named by the resulting descriptor.
            int t = openat(dirfd, path, O_RDONLY);
            if (t < 0)
                return LERR(errno);
            keep_fd = t;
            snprintf(resolved, sizeof resolved, "/dev/fd/%d", t);
        }
    }

    if (lflags & L_AT_SYMLINK_NOFOLLOW) {
        // Linux fails execveat with ELOOP when the final component is a symlink
        // and this flag is set. Darwin's execve has no way to express that, so
        // the check is made here; it is a check rather than a guarantee, since
        // the path can change between the lstat and the exec.
        struct stat st;
        if (lstat(resolved, &st) == 0 && S_ISLNK(st.st_mode)) {
            restore_cloexec(cloexec_fd, cloexec_fl);
            if (keep_fd >= 0)
                close(keep_fd);
            return LERR(ELOOP);
        }
    }

    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] execveat(dirfd=%d, \"%s\", flags=0x%x) -> %s\n",
                dirfd, plen ? path : "", (unsigned)lflags, resolved);

    // lxrt_execve does not return on success. argv[0] is dropped there and
    // rebuilt from the path, which is the runtime's existing execve behaviour
    // and is inherited rather than re-decided here.
    long r = lxrt_execve(resolved, argv, envp);
    restore_cloexec(cloexec_fd, cloexec_fl);
    if (keep_fd >= 0)
        close(keep_fd);
    return r;
}

// ============================================================================
// 137  rt_sigtimedwait(set, info, timeout, sigsetsize)
// ============================================================================
//
// Darwin has sigwait, which blocks without a deadline, and nothing else:
// sigwaitinfo, sigtimedwait and sigqueue are all absent from the SDK. So the
// deadline has to be built.
//
// The brief offered kqueue EVFILT_SIGNAL. It was measured and it does not work
// here. On macOS 15 / arm64, with the signal blocked:
//
//   kill(getpid(), SIGUSR2)         -> kevent wakes,      160 ms  (as expected)
//   pthread_kill(self, SIGUSR1)     -> kevent NEVER wakes, timed out at 2 s
//   signal raised before the kevent -> not reported; the filter is edge
//                                      triggered on delivery, so an already
//                                      pending signal is invisible to it
//
// EVFILT_SIGNAL hangs off the process, and a thread-directed signal never
// reaches it. Every signal this runtime delivers is thread-directed:
// lxrt_tgkill and lxrt_kill both end in pthread_kill, because the process also
// contains AppKit's main thread and libdispatch's workers and a signal must be
// aimed at a guest thread. A kqueue implementation would therefore have been
// invisible to exactly the signals FEX is waiting for, and would have looked
// like it worked in any test that used kill().
//
// So: sigwait, which does see thread-directed signals and returns immediately
// on an already-pending one (measured at 0.3 microseconds), plus a timer thread
// that pthread_kills the waiter at the deadline to break it out. Costs one
// shared thread for the process, not one per wait.
//
// ------------------------------------------- what sigwait gets wrong here
//
// Darwin's sigwait is not thread-scoped the way Linux's dequeue is. MEASURED:
// thread W blocks SIGINFO and pthread_kills ITSELF, so the signal is pending on
// W; thread V then calls sigwait() on a set containing SIGINFO and V's sigwait
// returns 29, while W's sigpending() clears. V took a signal that was aimed at
// W and W never ran. On Linux a thread-directed signal can only be dequeued by
// its target.
//
// For the runtime's own deadline wakeup this is handled: the timer re-sends
// WAKE_SIG until the owner disarms (see WAKE_RETRY_NS), so a swallowed wakeup
// costs a millisecond instead of hanging the thread.
//
// For the GUEST's signals it is a divergence that cannot be closed from here,
// and it is written down rather than papered over. If two guest threads are
// both inside rt_sigtimedwait with overlapping sets and the guest sends a
// signal to one of them specifically -- tgkill, which reaches pthread_kill --
// the other thread's sigwait may return it instead. Linux would deliver it only
// to the named thread. Nothing in userspace can tell which thread a signal
// arriving out of sigwait was aimed at, so there is no way to detect the theft,
// let alone re-post it to the right thread; the information is gone by the time
// sigwait returns. It needs a per-thread dequeue primitive Darwin does not
// have. The exposure is narrow -- it needs two guest threads waiting on the
// same signal number at the same time -- but it is real.

// The deadline wakeup. Darwin's SIGINFO is 29 and no Linux signal maps onto it
// -- the only free number besides SIGEMT(7), which signal.c already claimed as
// the realtime carrier. See the disposition note below for why it needs a
// handler installed.
#define WAKE_SIG SIGINFO
// signal.c's realtime carrier. Named again here rather than shared, because
// this file must not edit signal.c; if that choice ever moves, both change.
#define RT_CARRIER SIGEMT

#define LINUX_NSIG     64
#define LINUX_SIGRTMIN 32

// Linux siginfo_t, 128 bytes, matching the layout signal.c builds its frames
// with. The union at offset 16 is _sifields; for a signal that arrived from
// kill() or sigqueue() its members are si_pid, si_uid and si_value, which is
// the same 16 bytes signal.c uses as si_addr for the fault signals.
//
// The tag is _kill rather than plain linux_siginfo because signal.c:136 already
// defines `struct linux_siginfo` with a DIFFERENT member list (si_addr at 16,
// rest[104]). Two definitions of one tag with incompatible members in one
// program is undefined behaviour under C11 6.2.7, and the Makefile builds every
// source in a single clang invocation. Both are 128 bytes with the union at
// offset 16 so it is harmless today, but -flto -- or hoisting either definition
// into a header -- would turn it into silent field misinterpretation in the
// signal frames. Distinct tags cannot collide. The real fix is one shared
// definition with a named union in lxrt.h, which is not this file to make.
struct linux_siginfo_kill {
    int32_t  si_signo;   // 0
    int32_t  si_errno;   // 4
    int32_t  si_code;    // 8
    int32_t  pad_;       // 12
    int32_t  si_pid;     // 16
    uint32_t si_uid;     // 20
    uint64_t si_value;   // 24
    uint8_t  rest[96];   // 32
};
_Static_assert(sizeof(struct linux_siginfo_kill) == 128,
               "Linux siginfo_t is 128 bytes; the guest reads fields by offset");
_Static_assert(__builtin_offsetof(struct linux_siginfo_kill, si_pid) == 16,
               "si_pid shares offset 16 with si_addr, as the kernel union does");

// Linux si_code for a signal sent by kill() is SI_USER == 0. Darwin's SI_USER
// is 0x10001, and SI_QUEUE 0x10002 against Linux's -1. Nothing here forwards a
// Darwin si_code, precisely so that divergence cannot leak out.
#define L_SI_USER 0

// ------------------------------------------------ Darwin drops ignored signals
//
// Measured, and this one is load-bearing. Darwin discards a signal at post time
// when its effective disposition is "ignore", EVEN WHEN THE SIGNAL IS BLOCKED.
// Linux makes it pending instead, specifically so that sigwait and
// rt_sigtimedwait can pick it up. Blocking the signal and calling pthread_kill
// on macOS 15 / arm64:
//
//   SIG_DFL, ignore-by-default: 16 URG, 20 CHLD, 23 IO, 28 WINCH, 29 INFO
//                                                          -> DROPPED
//   SIG_IGN, any signal                                    -> DROPPED
//   handler installed, any signal                          -> PENDING
//
// Read back through the Linux-to-Darwin map, a guest that waits on Linux
// SIGCHLD(17 -> Darwin 20) or SIGWINCH(28 -> 28) or SIGURG(23 -> 16) or
// SIGIO(29 -> 23) with the default disposition would wait forever, and the
// failure would look like the signal was never sent. So for the duration of a
// wait, any signal in the set whose disposition would cause a drop gets a no-op
// handler, refcounted so that overlapping waits on other threads do not restore
// each other's saved disposition.

static void noop_handler(int s) { (void)s; }

static bool ignored_by_default(int dsig)
{
    return dsig == SIGURG || dsig == SIGCHLD || dsig == SIGIO ||
           dsig == SIGWINCH || dsig == SIGINFO;
}

static int              g_undrop_ref[NSIG];
static struct sigaction g_undrop_old[NSIG];
static bool             g_undrop_installed[NSIG];
static pthread_mutex_t  g_undrop_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(fex_support_g_undrop_lock, g_undrop_lock)

static void undrop_begin(const sigset_t *set)
{
    pthread_mutex_lock(&g_undrop_lock);
    for (int d = 1; d < NSIG; d++) {
        if (!sigismember(set, d))
            continue;
        if (g_undrop_ref[d]++ > 0)
            continue;
        struct sigaction old;
        if (sigaction(d, NULL, &old) != 0)
            continue;
        bool drops = (old.sa_handler == SIG_IGN) ||
                     (old.sa_handler == SIG_DFL && ignored_by_default(d));
        if (!drops)
            continue;
        // SIGCHLD under SIG_IGN is the one case where the no-op must NOT be
        // installed, and Linux agrees. Measured on the aarch64 guest:
        //   SIGCHLD SIG_IGN, blocked, child exits -> rt_sigtimedwait EAGAIN,
        //                                            waitpid ECHILD (auto-reaped)
        //   SIGCHLD SIG_DFL, blocked, child exits -> returns 17, child reapable
        // Linux never generates the signal at all when the parent ignores
        // SIGCHLD; it reaps instead. Darwin does the same. Replacing SIG_IGN
        // with a handler here would deliver a signal Linux would not AND switch
        // Darwin back to accumulating zombies for the duration -- a
        // process-wide change that any wait4 on another thread would see.
        //
        // The documented cost: a SIGCHLD sent EXPLICITLY with kill() while the
        // disposition is SIG_IGN is delivered on Linux (measured: raise() under
        // SIG_IGN + blocked returns 17) and is not seen here. That is narrow --
        // a guest that ignores SIGCHLD and then waits for a hand-delivered one
        // -- and it is the smaller divergence of the two. SIG_DFL SIGCHLD is
        // unaffected and still gets the no-op, which is the case that matters.
        if (d == SIGCHLD && old.sa_handler == SIG_IGN)
            continue;
        struct sigaction na;
        memset(&na, 0, sizeof na);
        na.sa_handler = noop_handler;
        sigemptyset(&na.sa_mask);
        // SA_RESTART so that a stray delivery on some other thread, where this
        // signal is not blocked, cannot turn one of its syscalls into EINTR.
        na.sa_flags = SA_RESTART;
        if (sigaction(d, &na, NULL) == 0) {
            g_undrop_old[d] = old;
            g_undrop_installed[d] = true;
        }
    }
    pthread_mutex_unlock(&g_undrop_lock);
}

static void undrop_end(const sigset_t *set)
{
    pthread_mutex_lock(&g_undrop_lock);
    for (int d = 1; d < NSIG; d++) {
        if (!sigismember(set, d))
            continue;
        if (g_undrop_ref[d] > 0 && --g_undrop_ref[d] == 0 &&
            g_undrop_installed[d]) {
            sigaction(d, &g_undrop_old[d], NULL);
            g_undrop_installed[d] = false;
        }
    }
    pthread_mutex_unlock(&g_undrop_lock);
}

// ---------------------------------------------------------------- the timer

// One slot per wait, living on the waiting thread's own stack and linked into
// g_timer_list only while it is armed.
//
// This used to be a fixed array of 64 slots, and the cap did not merely limit
// concurrency -- it INVENTED A TIMEOUT. With every slot taken, timer_arm failed
// and the wait returned -EAGAIN, which is exactly what Linux returns on expiry,
// so a guest could not tell the two apart and a caller looping on EAGAIN spun
// at 100% CPU. Measured on the array version: 80 concurrent waits of 300 ms
// each, 16 of them (80 - 64, exactly the overflow) returned -EAGAIN after a
// wall time of 0.0000 s. FEX runs far more than 64 threads.
//
// A stack slot has no table to exhaust, so the cap is gone rather than raised.
// The lifetime is safe because linking, firing and unlinking all happen under
// g_timer_lock: once timer_disarm returns, the timer thread can no longer
// reach the slot. The wait cleanup disarms it on return and pthread_exit.
struct timer_slot {
    struct timer_slot *next;     // g_timer_list; read/written under g_timer_lock
    bool      linked;            // read/written under g_timer_lock
    pthread_t target;
    // When the next WAKE_SIG is due, monotonic. Set to the wait's deadline on
    // arm, then pushed forward by WAKE_RETRY_NS each time one is sent.
    uint64_t  deadline_ns;
};

static struct timer_slot *g_timer_list;
static pthread_mutex_t    g_timer_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(fex_support_g_timer_lock, g_timer_lock)
static pthread_cond_t     g_timer_cond = PTHREAD_COND_INITIALIZER;
static bool               g_timer_running;

// How often an expired slot re-sends WAKE_SIG until its owner disarms it.
//
// One pthread_kill is NOT a delivery guarantee on Darwin. Measured directly:
// thread V sitting in sigwait() on a set containing SIGINFO dequeued a SIGINFO
// that pthread_kill had aimed at thread W -- V's sigwait returned 29 and W's
// pending set cleared, without W ever running. Linux cannot do that; a
// thread-directed signal there is takeable only by its target. Since every
// waiter here has WAKE_SIG in its sigwait set, any of them can swallow another
// thread's wakeup.
//
// The old code marked the slot fired after one kill and never looked at it
// again, so a swallowed wakeup left the owner in sigwait forever with its
// deadline long gone -- reproduced as threads stuck in sigwait with the timer
// thread idle in pthread_cond_wait. Re-sending costs one kill per millisecond
// per not-yet-disarmed expired slot, and in practice the owner disarms on the
// first one.
#define WAKE_RETRY_NS 1000000ull

static void *timer_main(void *unused)
{
    (void)unused;
    pthread_setname_np("lxrt-sigtimer");
    // The timer thread must never take a guest signal itself. The synchronous
    // faults are left unblocked deliberately: blocking one does not prevent it,
    // it makes the kernel force-deliver and kill, which would turn a bug in
    // this thread into an unexplained process death.
    sigset_t all;
    sigfillset(&all);
    sigdelset(&all, SIGSEGV); sigdelset(&all, SIGBUS);
    sigdelset(&all, SIGILL);  sigdelset(&all, SIGFPE);
    sigdelset(&all, SIGTRAP); sigdelset(&all, SIGSYS);
    pthread_sigmask(SIG_BLOCK, &all, NULL);

    pthread_mutex_lock(&g_timer_lock);
    for (;;) {
        uint64_t now = mono_ns();
        uint64_t next = UINT64_MAX;
        for (struct timer_slot **pp = &g_timer_list; *pp;) {
            struct timer_slot *s = *pp;
            if (s->deadline_ns <= now) {
                if (pthread_kill(s->target, WAKE_SIG) == ESRCH) {
                    // Retire a missing target; the slot belongs to its waiter,
                    // so unlink it but never free it here.
                    *pp = s->next;
                    s->linked = false;
                    s->next = NULL;
                    continue;
                }
                // Rescheduled rather than retired: see WAKE_RETRY_NS.
                s->deadline_ns = now + WAKE_RETRY_NS;
            }
            if (s->deadline_ns < next)
                next = s->deadline_ns;
            pp = &s->next;
        }
        if (next == UINT64_MAX) {
            pthread_cond_wait(&g_timer_cond, &g_timer_lock);
            continue;
        }
        // The clock is re-read into n2 and the result guarded, because `now`
        // was sampled before a scan that does a pthread_kill per expired slot
        // and can be preempted anywhere inside it. When the clock has passed
        // `next` in that window, the `next - mono_ns()` this replaces underflowed
        // to ~1.8e19 ns -- and pthread_cond_timedwait_relative_np does NOT
        // reject that value, it waits on it (tv_sec 18446744073). The armed
        // deadline was then never delivered and the guest thread sat in sigwait
        // with no timeout at all.
        //
        // Measured on the old code: 24 waiter threads doing sub-millisecond
        // timed waits under CPU contention, 0 of 24 finished in 60 s, with the
        // underflow window hit dozens of times per second (late by 0.3 us to
        // 18 us). Re-scanning instead of sleeping fires the slot on the very
        // next pass; it cannot spin, because that pass sends the slot's
        // WAKE_SIG and moves its deadline to now + WAKE_RETRY_NS, which is
        // strictly in the future.
        uint64_t n2 = mono_ns();
        if (next <= n2)
            continue;
        uint64_t d = next - n2;
        // Relative, because Darwin's pthread_cond_timedwait is against
        // CLOCK_REALTIME and a wall-clock step would move every deadline.
        struct timespec rel = { (time_t)(d / 1000000000ull),
                                (long)(d % 1000000000ull) };
        pthread_cond_timedwait_relative_np(&g_timer_cond, &g_timer_lock, &rel);
    }
}

// Links `s` into the deadline list. The only way this fails is the timer thread
// itself failing to start, which is a genuine resource failure and is reported
// as one -- there is deliberately no "table full" outcome any more.
static bool timer_arm(struct timer_slot *s, uint64_t deadline_ns)
{
    pthread_mutex_lock(&g_timer_lock);
    if (s->linked) {
        pthread_mutex_unlock(&g_timer_lock);
        return true;
    }
    if (!g_timer_running) {
        pthread_t t;
        if (pthread_create(&t, NULL, timer_main, NULL) != 0) {
            pthread_mutex_unlock(&g_timer_lock);
            return false;
        }
        pthread_detach(t);
        g_timer_running = true;
    }
    s->target = pthread_self();
    s->deadline_ns = deadline_ns;
    s->next = g_timer_list;
    g_timer_list = s;
    s->linked = true;
    pthread_cond_signal(&g_timer_cond);
    pthread_mutex_unlock(&g_timer_lock);
    return true;
}

// Unlinks the slot. Firing and unlinking both happen under g_timer_lock, so
// once this returns the timer can no longer reach the slot and no further
// WAKE_SIG can be sent for it -- which is what makes it safe for the slot to be
// a stack object that dies when the caller returns.
static void timer_disarm(struct timer_slot *s)
{
    pthread_mutex_lock(&g_timer_lock);
    if (!s->linked) {
        pthread_mutex_unlock(&g_timer_lock);
        return;
    }
    for (struct timer_slot **pp = &g_timer_list; *pp; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    s->linked = false;
    s->next = NULL;
    pthread_mutex_unlock(&g_timer_lock);
}

// Shared by normal return and pthread_exit from a guest signal handler.
struct sigwait_cleanup {
    struct timer_slot *slot;
    sigset_t undrop;
    sigset_t oldmask;
    bool undrop_active;
    bool mask_active;
};

static void sigwait_cleanup(void *arg)
{
    struct sigwait_cleanup *c = arg;
    // Unlink before restoring dispositions or unblocking pending handlers.
    timer_disarm(c->slot);
    if (c->undrop_active) {
        c->undrop_active = false;
        undrop_end(&c->undrop);
    }
    if (c->mask_active) {
        c->mask_active = false;
        pthread_sigmask(SIG_SETMASK, &c->oldmask, NULL);
    }
}

// ------------------------------------------------------------ realtime half
//
// Linux realtime signals 32..63 have no Darwin numbers at all. signal.c carries
// them as a number queued against the target thread plus a SIGEMT to wake it,
// and that is what FEX waits on. Two things follow for this wait:
//
//   * SIGEMT must be blocked for its duration. Otherwise signal.c's host
//     handler runs, dequeues the number and dispatches it to the guest's
//     handler -- the signal is consumed by delivery and rt_sigtimedwait never
//     sees it, which is the opposite of what the call means.
//   * With SIGEMT blocked the carrier stays pending and the number stays in the
//     queue, so the queue can be read directly.

// lxrt_rt_dequeue_self pops the head unconditionally; there is no filtered
// take. Drain, keep the first number the set asked for, and put the rest back
// in the order they came out. A concurrent enqueue during the drain can end up
// behind a requeued signal; Linux's own ordering guarantee for realtime signals
// is per-signal-number, which this preserves.
// thread.c:71 sizes each thread's realtime queue at RT_QUEUE_DEPTH 32, and it
// is a ring that keeps one slot empty, so a full drain can yield at most 31
// entries. Named again here because this file must not edit thread.c or lxrt.h;
// sized above that so the hold-back buffer can always take everything the queue
// can hand over. It used to be sized by TIMER_SLOTS, a completely unrelated
// constant that happened to be large enough -- the coupling was accidental, and
// raising the queue depth would have started silently destroying signals.
#define RT_HELD_MAX 64

static int rt_take(uint64_t rtmask, bool *leftover)
{
    uint8_t held[RT_HELD_MAX];
    int n = 0, want = 0;

    for (int guard = 0; guard < 256; guard++) {
        // Stop draining rather than pop something with nowhere to put it: a
        // signal dequeued here and not requeued is destroyed outright. Leaving
        // it in the queue keeps it deliverable.
        if (n == RT_HELD_MAX)
            break;
        int s = lxrt_rt_dequeue_self();
        if (s == 0)
            break;
        if (s < LINUX_SIGRTMIN || s > LINUX_NSIG) {
            // Out of range names no Linux signal, so there is nothing to
            // requeue it as and nothing that could ever deliver it. It can only
            // be dropped -- but not in silence, because its presence means
            // something upstream wrote a bad value into the queue.
            if (lxrt_trace_on())
                fprintf(lxrt_trace_stream(), "[lxrt] rt_sigtimedwait: dropped queue entry %d "
                                "(outside %d..%d)\n",
                        s, LINUX_SIGRTMIN, LINUX_NSIG);
            continue;
        }
        if (!want && (rtmask & (1ull << (s - 1)))) {
            want = s;
            continue;
        }
        held[n++] = (uint8_t)s;
    }
    int me = lxrt_gettid();
    for (int i = 0; i < n; i++) {
        // A failed requeue means the queue filled from another thread during
        // the drain. Linux drops on a full queue too, so the behaviour is not
        // wrong -- but it is a real lost signal and the return value used to be
        // discarded, which made it invisible.
        if (!lxrt_rt_enqueue(me, held[i]) && lxrt_trace_on())
            fprintf(lxrt_trace_stream(), "[lxrt] rt_sigtimedwait: requeue of signal %d "
                            "failed; it is lost\n", held[i]);
    }
    *leftover = n > 0;
    return want;
}

// ---------------------------------------------------------------- the wait

static void write_info(void *uinfo, int lsig)
{
    if (!uinfo)
        return;
    struct linux_siginfo_kill si;
    memset(&si, 0, sizeof si);
    si.si_signo = lsig;
    si.si_errno = 0;
    // Darwin's sigwait yields a number and nothing else, and Darwin has no
    // sigqueue, so there is no sender identity and no sigval to report. Linux
    // would fill in the real si_pid/si_uid and, for a sigqueue'd signal,
    // si_value. Reporting SI_USER from this process is the honest shape of what
    // is known: a caller reading si_value gets zero rather than a wrong value.
    si.si_code = L_SI_USER;
    si.si_pid = (int32_t)getpid();
    si.si_uid = (uint32_t)getuid();
    si.si_value = 0;
    memcpy(uinfo, &si, sizeof si);
}

long lxrt_rt_sigtimedwait(const void *uset, void *uinfo, const void *utimeout,
                          size_t sigsetsize)
{
    // Linux checks this before anything else; aarch64 sigset_t is 8 bytes.
    if (sigsetsize != 8)
        return LERR(EINVAL);
    if (!mem_ok((uint64_t)(uintptr_t)uset, 8, VM_PROT_READ))
        return LERR(EFAULT);
    if (uinfo && !mem_ok((uint64_t)(uintptr_t)uinfo,
                         sizeof(struct linux_siginfo_kill),
                         VM_PROT_WRITE))
        return LERR(EFAULT);

    uint64_t lmask;
    memcpy(&lmask, uset, sizeof lmask);

    bool timed = false;
    uint64_t deadline = 0;
    if (utimeout) {
        if (!mem_ok((uint64_t)(uintptr_t)utimeout, sizeof(struct linux_timespec),
                    VM_PROT_READ))
            return LERR(EFAULT);
        struct linux_timespec ts;
        memcpy(&ts, utimeout, sizeof ts);
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000)
            return LERR(EINVAL);
        timed = true;
        uint64_t now  = mono_ns();
        uint64_t secs = (uint64_t)ts.tv_sec;
        uint64_t nsec = (uint64_t)ts.tv_nsec;
        // Saturate instead of wrapping. The plain sum overflowed uint64 above
        // tv_sec ~1.8e10 and put the deadline in the PAST, so the longest
        // possible wait became an instant -EAGAIN. Linux honours those
        // timeouts by blocking: measured on the aarch64 guest, rt_sigtimedwait
        // with tv_sec 2e10 and with 2^63-1 both waited until a SIGALRM
        // interrupted them (EINTR at 2.05 s) -- neither EINVAL nor EAGAIN.
        // UINT64_MAX is "effectively never", which is the honest translation.
        if (secs > (UINT64_MAX - now - nsec) / 1000000000ull)
            deadline = UINT64_MAX;
        else
            deadline = now + secs * 1000000000ull + nsec;
    }

    // Bit n-1 of the Linux sigset is signal n, so bits 0..30 are 1..31 and bits
    // 31..63 are the realtime range 32..64.
    uint64_t rtmask = lmask & ~(((uint64_t)1 << 31) - 1);

    sigset_t dset;
    sigemptyset(&dset);
    bool any_classic = false;
    for (int l = 1; l < 32; l++) {
        if (!(lmask & ((uint64_t)1 << (l - 1))))
            continue;
        int d = lxrt_signo_to_darwin(l);
        // Linux silently ignores SIGKILL and SIGSTOP in the set rather than
        // failing, and a Linux signal with no Darwin number (SIGSTKFLT 16,
        // SIGPWR 30) can never arrive here, so it is dropped from the set
        // rather than made to fail the whole call.
        if (d == 0 || d == SIGKILL || d == SIGSTOP)
            continue;
        sigaddset(&dset, d);
        any_classic = true;
    }
    if (!any_classic && !rtmask) {
        // A set that can never produce anything: either it was empty, or every
        // Linux signal in it is one of the two with no Darwin number. Linux
        // still blocks, and is still interruptible, so reproduce that rather
        // than inventing an error -- pause() returns EINTR for exactly the
        // event that would end the wait on Linux.
        if (!timed) {
            pause();
            return LERR(EINTR);
        }
        struct timespec ts;
        uint64_t now = mono_ns();
        uint64_t d = deadline > now ? deadline - now : 0;
        ts.tv_sec = (time_t)(d / 1000000000ull);
        ts.tv_nsec = (long)(d % 1000000000ull);
        // An interrupted sleep is not an expiry. Returning EAGAIN regardless
        // reported a timeout that did not happen, where Linux returns EINTR --
        // the untimed sibling above already gets this right with pause().
        if (nanosleep(&ts, NULL) != 0)
            return LERR(errno);
        return LERR(EAGAIN);
    }

    // Block, for the whole wait:
    //   * the set itself, so a guest handler cannot consume the signal first.
    //     Linux requires the caller to have blocked it and returns EINTR if a
    //     handler runs instead; blocking it here makes the call work for a
    //     caller that forgot, at the cost of not reproducing that EINTR.
    //   * the realtime carrier, so signal.c's handler does not dispatch a
    //     queued realtime signal out from under this wait.
    //   * the deadline wakeup.
    sigset_t blockme = dset;
    sigaddset(&blockme, RT_CARRIER);
    sigaddset(&blockme, WAKE_SIG);
    sigset_t oldmask;
    // pthread_sigmask RETURNS the error number and is not specified to set
    // errno. Reading errno here worked only by accident -- measured, Darwin's
    // implementation happens to set it too (pthread_sigmask(bad how) -> ret 22,
    // errno 22) -- and an errno left at 0 would have fallen through
    // lxrt_errno_to_linux's default arm and reported EIO for an EINVAL.
    int mrc = pthread_sigmask(SIG_BLOCK, &blockme, &oldmask);
    if (mrc != 0)
        return LERR(mrc);

    // WAKE_SIG is ignored by default on Darwin and would be discarded rather
    // than made pending, so it is in the undrop set with everything else.
    sigset_t undrop = dset;
    sigaddset(&undrop, WAKE_SIG);
    undrop_begin(&undrop);

    // What sigwait may return: the wanted signals, plus the two wakeups.
    sigset_t waitset = dset;
    sigaddset(&waitset, RT_CARRIER);
    sigaddset(&waitset, WAKE_SIG);

    long result = 0;
    bool rt_leftover = false;
    // On this thread's stack, linked into the timer's list only while armed.
    struct timer_slot slot;
    memset(&slot, 0, sizeof slot);
    struct sigwait_cleanup cleanup = {
        .slot = &slot, .undrop = undrop, .oldmask = oldmask,
        .undrop_active = true, .mask_active = true,
    };

    pthread_cleanup_push(sigwait_cleanup, &cleanup);
    for (;;) {
        // Realtime first: it is the case FEX actually waits on, and it is read
        // from the queue rather than from Darwin's pending set.
        if (rtmask) {
            bool left = false;
            int rs = rt_take(rtmask, &left);
            rt_leftover = rt_leftover || left;
            if (rs) {
                write_info(uinfo, rs);
                result = rs;
                break;
            }
        }

        // There used to be a sigpending() fast path here that called
        // sigwait() on just the signals sigpending() had reported. It is gone
        // rather than repaired. Two reasons, both measured:
        //
        //   * It could block with no deadline. Its set was a subset of dset and
        //     so never contained WAKE_SIG, which meant the timer's
        //     pthread_kill(WAKE_SIG) could not break it out even on an
        //     iteration where the slot WAS armed. If the signal sigpending()
        //     reported got consumed elsewhere between the two calls, that
        //     sigwait blocked forever and the guest's timeout never fired.
        //   * It bought nothing. The main sigwait below already returns
        //     immediately for an already-pending member: measured at 0.282 us
        //     mean over 2000 iterations on a three-signal set, against a
        //     sigpending() plus a set rebuild plus a second sigwait.
        //
        // An unbounded hang path in exchange for no measurable gain is not a
        // trade worth keeping.

        if (timed) {
            uint64_t now = mono_ns();
            if (now >= deadline) {
                result = LERR(EAGAIN);   // Linux reports a timeout as EAGAIN
                break;
            }
            if (!timer_arm(&slot, deadline)) {
                // The only remaining way to get here is the timer thread
                // failing to start, which is a real resource failure. It is NOT
                // reported as EAGAIN: EAGAIN is what Linux returns on expiry,
                // so a guest would read it as "your 300 ms elapsed" after no
                // time at all, and a caller that loops on EAGAIN would spin.
                // ENOMEM cannot be mistaken for a timeout.
                result = LERR(ENOMEM);
                break;
            }
        }

        int d = 0;
        int rc = sigwait(&waitset, &d);
        if (rc != 0) {
            // Darwin returns EINTR here when a signal outside the set was
            // handled, which is exactly what Linux reports for the same event.
            result = LERR(rc == -1 ? errno : rc);
            break;
        }
        if (d == WAKE_SIG || d == RT_CARRIER)
            continue;               // a wakeup; the loop re-evaluates
        int l = lxrt_signo_to_linux(d);
        if (l && sigismember(&dset, d)) {
            write_info(uinfo, l);
            result = l;
            break;
        }
        // Something in waitset that is not in dset and not a wakeup cannot
        // happen, but consuming it silently would lose it; go round again.
    }

    timer_disarm(&slot);
    // A WAKE_SIG the timer sent may still be pending here, and it is left that
    // way deliberately. The code this replaces did sigpending() and then
    // committed to a blocking sigwait() on the strength of it -- and that
    // sigwait could block FOREVER, because between the two calls another
    // waiter's sigwait can dequeue this thread's pending WAKE_SIG (see
    // WAKE_RETRY_NS for the measurement). Reproduced: two threads parked in
    // exactly that sigwait with 2259 of 2259 samples in __sigwait, roughly one
    // run in six of a 48-thread soak.
    //
    // Leaving it pending is harmless, which is why the drain is not replaced
    // with anything. A timeout here is decided by comparing mono_ns() against
    // the deadline, never by the arrival of WAKE_SIG, so a stray one cannot
    // fabricate an early expiry: the loop above treats it as a wakeup, goes
    // round, re-reads the clock and carries on. And a standard signal has at
    // most one pending instance, so it cannot accumulate. On the way out it is
    // either delivered to the no-op handler, discarded because the disposition
    // has gone back to ignore, or left pending for the next wait's sigwait to
    // consume as one extra iteration.
    if (rt_leftover) {
        // Realtime signals were requeued that this wait did not want. The
        // carrier that would have announced them was consumed by sigwait, so it
        // is re-posted; it is still blocked at this point, so it becomes
        // pending again and is delivered when the guest unblocks it.
        // signal.c's handler already tolerates a carrier with an empty queue.
        //
        // Only when a handler is actually installed. SIGEMT's default action is
        // terminate-and-core, so re-posting one before signal.c has installed
        // the carrier handler would turn an unwanted realtime signal into a
        // crash the moment the mask is restored.
        struct sigaction cur;
        if (sigaction(RT_CARRIER, NULL, &cur) == 0 &&
            cur.sa_handler != SIG_DFL && cur.sa_handler != SIG_IGN)
            pthread_kill(pthread_self(), RT_CARRIER);
    }

    pthread_cleanup_pop(1);

    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] rt_sigtimedwait(mask=0x%llx, %s) -> %ld\n",
                (unsigned long long)lmask, timed ? "timed" : "blocking", result);
    return result;
}
