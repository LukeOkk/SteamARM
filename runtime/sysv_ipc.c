// System V IPC: semaphores and shared memory.
//
// Not a corner case. Valve's own libtier0_s.so imports semget/semop/semctl, so
// this is the base library every process in the Steam stack links against, not
// one component. `ipcs` on the running guest shows five live semaphore arrays
// and a 10 805 760-byte shared segment created by steamwebhelper; semtimedop
// is measured at 131 calls per 10 s from the `steam` process itself.
//
// Wired: dispatch.c routes 190-197 here (LNR_* at dispatch.c:81-82, cases at
// dispatch.c:2859-2903), and this file is in the Makefile's LXRT_SRCS
// (Makefile:35-38). Still missing: the second semtimedop number, 420
// (semtimedop_time64), which has no case and returns -ENOSYS; see sysv_ipc.h.
//
// Darwin has SysV IPC, so most of this is translation rather than emulation.
// The translation is not thin, because five number spaces disagree and four of
// them agree on their low values first:
//
//  1. semctl commands. Linux GETPID=11 GETVAL=12 GETALL=13 GETNCNT=14
//     GETZCNT=15 SETVAL=16 SETALL=17; Darwin GETNCNT=3 GETPID=4 GETVAL=5
//     GETALL=6 GETZCNT=7 SETVAL=8 SETALL=9. IPC_RMID/IPC_SET/IPC_STAT are
//     0/1/2 on both, which is exactly what makes the rest dangerous: the
//     common commands work unmapped. The collision that actually bites is
//     Linux IPC_INFO (3), which is Darwin's GETNCNT (3) -- measured here,
//     semctl(id, 0, 3, arg) returns 0 rather than failing, so an unmapped
//     pass-through answers "describe the system limits" with a wait count.
//
//  2. glibc and musl both OR IPC_64 (0x0100) into the semctl and shmctl
//     command before issuing the syscall, because the kernel uses that bit to
//     pick the 64-bit struct. A guest IPC_STAT therefore arrives here as
//     0x102, and an implementation that switches on the raw value turns every
//     stat into EINVAL.
//
//  3. struct ipc_perm. Linux puts key FIRST and mode is 32-bit; Darwin puts
//     key LAST and mode is 16-bit. Field-by-field below.
//
//  4. semid_ds and shmid_ds: 88 and 112 bytes on Linux against 72 and 76 on
//     Darwin, and Darwin's are #pragma pack(4) so its 8-byte time_t fields sit
//     at 4-aligned offsets. Nothing about these can be memcpy'd.
//
//  5. ipc_perm.mode leaks kernel state on Darwin. Measured on this machine
//     (Darwin 27.0, xnu-13432): a segment created 0600 reads back as 04600
//     (XNU's SHMSEG_ALLOCATED, 0x800 == 04000) and a semaphore set created
//     0600 reads back as 01600 (XNU's SEM_ALLOC, 0x200 == 01000). Neither of
//     those bit positions is spare on Linux, and the earlier note here named
//     the wrong ones: in shm_perm.mode 04000 is SHM_HUGETLB and 01000 is
//     SHM_DEST (guest /usr/include/bits/shm.h:56-58, read rather than
//     remembered). So the extra bits do not read as noise, they read as a
//     different answer. Mode is masked to 0777 in both directions.
//
//     The mask erases one bit Linux really does set: SHM_DEST (01000) goes
//     into shm_perm.mode when a segment is RMID'd while still attached
//     (verified on the guest -- IPC_STAT after IPC_RMID with one attach
//     outstanding reports mode 01600 and nattch 1, and a further shmat still
//     succeeds). Darwin has nothing to restore it from: measured here,
//     shmctl(id, IPC_STAT) on a Darwin segment that has been RMID'd returns
//     EINVAL outright -- the id is rejected before any perm is read -- so
//     SHMSEG_REMOVED (0x400) is never observable through this interface. The
//     bit is therefore synthesised only for the POSIX-backed segments this
//     module keeps its own bookkeeping for; see pshm_stat(). For a
//     kernel-backed segment a guest polling for SHM_DEST after IPC_RMID gets
//     EINVAL from IPC_STAT instead, which is a divergence that cannot be
//     closed from user space.
//
// Two Darwin limits are walls rather than translation problems, and both are
// measured rather than assumed. They are handled where they are hit:
//
//   * kern.sysv.shmmax defaults to 4194304 (4 MiB). Steam asks for 10 805 760.
//     shmget of that size returns EINVAL. See shmmax_bytes() and the POSIX
//     shared memory fallback.
//   * Darwin's semop accepts at most 5 operations in one call. Measured:
//     nsops=5 succeeds, nsops=6 returns E2BIG. Linux's SEMOPM is 500.
//
// And one limit that turned out to be folklore: Darwin's semaphore counts are
// NOT 87 any more. This machine reports kern.sysv.semmni=87381,
// semmns=87381, semmsl=87381, and semget(200 sems) succeeds. What is still
// small is kern.sysv.semume=10, the number of SEM_UNDO entries ONE PROCESS may
// hold; a guest that leaves more than ten undo adjustments outstanding gets
// EINVAL from semop with nothing else to go on. Shared memory is where the
// small numbers live: shmmni=32 segments system-wide and shmseg=8 attaches per
// process, against Linux's 4096 and 4096.

#include "lxrt.h"
#include "sysv_ipc.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))

// ------------------------------------------------------------ Linux constants
//
// IPC_CREAT/IPC_EXCL/IPC_NOWAIT were checked on both sides rather than
// assumed, because the rest of this file is a list of things that looked like
// they matched. They do match: 01000/02000/04000 on Linux (linux/ipc.h) and
// 01000/02000/04000 on Darwin (sys/ipc.h). So do IPC_RMID/IPC_SET/IPC_STAT
// (0/1/2), SEM_UNDO (0x1000 on Linux, 010000 on Darwin -- same number written
// two ways), SHM_RDONLY (010000) and SHM_RND (020000).
#define L_IPC_CREAT   01000
#define L_IPC_EXCL    02000
#define L_IPC_NOWAIT  04000
#define L_IPC_64      0x0100     // glibc/musl OR this into semctl/shmctl cmd

#define L_IPC_RMID  0
#define L_IPC_SET   1
#define L_IPC_STAT  2
#define L_IPC_INFO  3            // Darwin's 3 is GETNCNT; must never pass through

#define L_GETPID    11
#define L_GETVAL    12
#define L_GETALL    13
#define L_GETNCNT   14
#define L_GETZCNT   15
#define L_SETVAL    16
#define L_SETALL    17
#define L_SEM_STAT  18
#define L_SEM_INFO  19
#define L_SEM_STAT_ANY 20

#define L_SHM_LOCK     11
#define L_SHM_UNLOCK   12
#define L_SHM_STAT     13
#define L_SHM_INFO     14
#define L_SHM_STAT_ANY 15

#define L_SHM_DEST    01000      // set by Linux in shm_perm.mode after RMID
#define L_SHM_RDONLY  010000
#define L_SHM_RND     020000
#define L_SHM_REMAP   040000     // Linux only
#define L_SHM_EXEC   0100000     // Linux only

// Linux shmget flags with no Darwin meaning, and which collide with bits
// Darwin does use: SHM_HUGETLB is 04000, the same bit as IPC_NOWAIT, and
// SHM_NORESERVE is 010000, the same bit as Darwin's IPC_M. Passing a guest's
// shmflg through unmasked therefore sets unrelated Darwin flags. Both are
// dropped; macOS has no huge pages for SysV shm and no reservation policy to
// opt out of.
#define L_SHM_HUGETLB   04000
#define L_SHM_NORESERVE 010000

#define L_SEMOPM  500            // Linux max ops per semop; Darwin's is 5
#define PERM_MASK 0777

// ------------------------------------------------------------- Linux structs
//
//   struct ipc64_perm                  struct ipc_perm (Darwin, __ipc_perm_new)
//   off size field                     off size field
//     0    4 key      (int32)           -    -  (key is LAST here)
//     4    4 uid                        0    4 uid
//     8    4 gid                        4    4 gid
//    12    4 cuid                       8    4 cuid
//    16    4 cgid                      12    4 cgid
//    20    4 mode     (uint32)         16    2 mode (mode_t is 16-bit)
//    24    2 seq                       18    2 _seq
//    26    2 __pad2                    20    4 _key (int32)
//    32    8 __unused1
//    40    8 __unused2
//   size 48, align 8                   size 24, align 4
//
//   struct semid64_ds                  struct __semid_ds_new  (pack(4))
//     0   48 sem_perm                   0   24 sem_perm
//    48    8 sem_otime (long)           24    4 sem_base (kernel pointer)
//    56    8 sem_ctime (long)           28    2 sem_nsems (unsigned short!)
//    64    8 sem_nsems (unsigned long)  32    8 sem_otime (time_t)
//    72    8 __unused3                  40    4 sem_pad1
//    80    8 __unused4                  44    8 sem_ctime (time_t)
//                                       52    4 sem_pad2
//                                       56   16 sem_pad3[4]
//   size 88                            size 72
//
//   struct shmid64_ds                  struct __shmid_ds_new  (pack(4))
//     0   48 shm_perm                   0   24 shm_perm
//    48    8 shm_segsz (size_t)        24    8 shm_segsz
//    56    8 shm_atime                 32    4 shm_lpid
//    64    8 shm_dtime                 36    4 shm_cpid
//    72    8 shm_ctime                 40    2 shm_nattch (unsigned short!)
//    80    4 shm_cpid                  44    8 shm_atime
//    84    4 shm_lpid                  52    8 shm_dtime
//    88    8 shm_nattch (unsigned long) 60   8 shm_ctime
//    96    8 __unused4                 68    8 shm_internal (kernel pointer)
//   104    8 __unused5
//   size 112                           size 76
//
// Note the two narrowings that would go unnoticed: Darwin's sem_nsems and
// shm_nattch are 16-bit where Linux's are 64-bit. A set of more than 65535
// semaphores, or more than 65535 attaches, cannot be reported faithfully --
// neither is reachable under Darwin's own limits, so it is a note, not a bug.

struct linux_ipc64_perm {
    int32_t  key;
    uint32_t uid, gid, cuid, cgid;
    uint32_t mode;
    uint16_t seq;
    uint16_t pad2;
    uint64_t unused1, unused2;
};

struct linux_semid64_ds {
    struct linux_ipc64_perm sem_perm;
    int64_t  sem_otime;
    int64_t  sem_ctime;
    uint64_t sem_nsems;
    uint64_t unused3, unused4;
};

struct linux_shmid64_ds {
    struct linux_ipc64_perm shm_perm;
    uint64_t shm_segsz;
    int64_t  shm_atime, shm_dtime, shm_ctime;
    int32_t  shm_cpid, shm_lpid;
    uint64_t shm_nattch;
    uint64_t unused4, unused5;
};

// The one structure that really is identical, verified rather than assumed:
// { unsigned short sem_num; short sem_op; short sem_flg; }, 6 bytes, align 2,
// offsets 0/2/4 on both. It is still copied field by field below, because the
// guest buffer must be snapshotted anyway -- validating a pointer and then
// letting the kernel read through it leaves the guest a window to rewrite it.
struct linux_sembuf { uint16_t sem_num; int16_t sem_op; int16_t sem_flg; };

// Linux's __kernel_timespec: two signed 64-bit fields on aarch64. Darwin's
// struct timespec on arm64 is also { int64 tv_sec; long tv_nsec; }. Same size
// and same layout, but declared separately so the agreement is on the record.
struct linux_timespec { int64_t tv_sec; int64_t tv_nsec; };

_Static_assert(sizeof(struct linux_ipc64_perm) == 48, "ipc64_perm is 48 bytes");
_Static_assert(sizeof(struct linux_semid64_ds) == 88, "semid64_ds is 88 bytes");
_Static_assert(sizeof(struct linux_shmid64_ds) == 112, "shmid64_ds is 112 bytes");
_Static_assert(sizeof(struct linux_sembuf) == 6, "sembuf is 6 bytes");
_Static_assert(sizeof(struct sembuf) == sizeof(struct linux_sembuf),
               "Darwin and Linux sembuf must stay the same width");
_Static_assert(sizeof(struct linux_timespec) == 16, "timespec is 16 bytes");

// ------------------------------------------------------------- guest pointers
//
// A guest pointer is a register value, nothing more. Handing a bad one to the
// kernel is survivable; dereferencing it here is not, and a SIGBUS inside the
// runtime looks nothing like the EFAULT the guest was owed. Every guest buffer
// is checked against the VM map first. This costs one Mach trap per region
// crossed, which is irrelevant at the measured 13 semop/s and never on a path
// that runs per frame.
static bool mem_ok(uint64_t addr, uint64_t len, vm_prot_t need)
{
    if (!addr || !len || addr + len < addr)
        return false;
    uint64_t cur = addr, end = addr + len;
    while (cur < end) {
        mach_vm_address_t a = cur;
        mach_vm_size_t sz = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &a, &sz, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
            return false;
        if (obj != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), obj);
        // mach_vm_region reports the first region at or ABOVE the address, so
        // a > cur means cur itself is a hole.
        if (a > cur || sz == 0)
            return false;
        if ((info.protection & need) != need)
            return false;
        cur = a + sz;
    }
    return true;
}

static bool can_read(const void *p, uint64_t n)
{
    return mem_ok((uint64_t)(uintptr_t)p, n, VM_PROT_READ);
}

static bool can_write(const void *p, uint64_t n)
{
    return mem_ok((uint64_t)(uintptr_t)p, n, VM_PROT_READ | VM_PROT_WRITE);
}

// Diagnostics that must appear once and never again: these fire on a
// configuration problem the user has to fix outside the process, so repeating
// them per call would bury the run's real output.
__attribute__((format(printf, 2, 3)))
static void warn_once(_Atomic bool *said, const char *fmt, ...)
{
    if (atomic_exchange(said, true))
        return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(lxrt_trace_stream(), "lxrt: ");
    vfprintf(lxrt_trace_stream(), fmt, ap);
    va_end(ap);
    fflush(stderr);
}

// ------------------------------------------------------------ perm/ds mapping

static void perm_to_linux(const struct ipc_perm *d, struct linux_ipc64_perm *l)
{
    memset(l, 0, sizeof *l);
    l->key  = d->_key;
    l->uid  = d->uid;
    l->gid  = d->gid;
    l->cuid = d->cuid;
    l->cgid = d->cgid;
    // Masked, not copied: see note 5 at the top. Darwin ORs SHMSEG_ALLOCATED
    // (04000, Linux's SHM_HUGETLB position) into a segment's mode and SEM_ALLOC
    // (01000, Linux's SHM_DEST position) into a semaphore set's, so the bits
    // do not read as noise on the Linux side, they read as flags. Note 5 says
    // what this costs and where SHM_DEST is put back.
    l->mode = d->mode & PERM_MASK;
    l->seq  = d->_seq;
}

// IPC_SET only carries uid, gid and mode on Linux; everything else in the
// structure is ignored by the kernel. The Darwin side is read back first so
// the fields Darwin does care about are not zeroed out from under it.
static void perm_from_linux(const struct linux_ipc64_perm *l, struct ipc_perm *d)
{
    d->uid  = l->uid;
    d->gid  = l->gid;
    d->mode = (mode_t)(l->mode & PERM_MASK);
}

static void semid_to_linux(const struct semid_ds *d, struct linux_semid64_ds *l)
{
    memset(l, 0, sizeof *l);
    perm_to_linux(&d->sem_perm, &l->sem_perm);
    l->sem_otime = (int64_t)d->sem_otime;
    l->sem_ctime = (int64_t)d->sem_ctime;
    l->sem_nsems = d->sem_nsems;
}

static void shmid_to_linux(const struct shmid_ds *d, struct linux_shmid64_ds *l)
{
    memset(l, 0, sizeof *l);
    perm_to_linux(&d->shm_perm, &l->shm_perm);
    l->shm_segsz  = d->shm_segsz;
    l->shm_atime  = (int64_t)d->shm_atime;
    l->shm_dtime  = (int64_t)d->shm_dtime;
    l->shm_ctime  = (int64_t)d->shm_ctime;
    l->shm_cpid   = d->shm_cpid;
    l->shm_lpid   = d->shm_lpid;
    l->shm_nattch = d->shm_nattch;      // 16-bit on Darwin, 64-bit on Linux
}

// =========================================================== semaphores =====

// Darwin's semop rejects more than MAX_SOPS operations. XNU does not export
// the number; it was measured: 5 works, 6 returns E2BIG. Linux allows 500.
#define DARWIN_MAX_SOPS 5

static _Atomic bool g_said_sops;
static _Atomic bool g_said_semume;

static int semflg_to_darwin(int lflg)
{
    // Only the permission bits and IPC_CREAT/IPC_EXCL have meaning here, and
    // all three ranges are identical. Everything else is masked off rather
    // than forwarded, so a future Linux-only flag cannot silently land on a
    // Darwin bit.
    return lflg & (PERM_MASK | L_IPC_CREAT | L_IPC_EXCL);
}

long lxrt_semget(int32_t key, int nsems, int lsemflg)
{
    int id = semget((key_t)key, nsems, semflg_to_darwin(lsemflg));
    if (id < 0)
        return LERR(errno);
    return id;
}

// Copies and validates the guest's sembuf array. `extra_flg` is ORed into
// every entry, which is how semtimedop turns a blocking set into a polled one.
// `*all_nowait` comes back true when EVERY incoming entry already carried
// IPC_NOWAIT, which has to be recorded before the OR above destroys the
// distinction -- see do_semop. Returns 0, or a negated Linux errno.
static long copy_sops(const void *lsops, uint64_t nsops, struct sembuf *out,
                      int extra_flg, bool *all_nowait)
{
    if (!can_read(lsops, nsops * sizeof(struct linux_sembuf)))
        return LERR(EFAULT);
    const struct linux_sembuf *in = lsops;
    bool all = true;
    for (uint64_t i = 0; i < nsops; i++) {
        out[i].sem_num = in[i].sem_num;
        out[i].sem_op  = in[i].sem_op;
        if (!(in[i].sem_flg & L_IPC_NOWAIT))
            all = false;
        // SEM_UNDO is 0x1000 on Linux and 010000 on Darwin, which is the same
        // number; IPC_NOWAIT is 04000 on both. Nothing else is defined for
        // sem_flg on either side, so the field passes through whole.
        out[i].sem_flg = (short)(in[i].sem_flg | extra_flg);
    }
    *all_nowait = all;
    return 0;
}

// Both entry points share this. `deadline` is NULL for a plain blocking semop.
static long do_semop(int semid, const void *lsops, uint64_t nsops,
                     const struct timespec *deadline)
{
    if (nsops == 0)
        return LERR(EINVAL);            // Linux: nsops < 1 is EINVAL
    if (nsops > L_SEMOPM)
        return LERR(E2BIG);             // Linux: SEMOPM is 500

    if (nsops > DARWIN_MAX_SOPS) {
        // No honest way to do this. semop is defined to apply the whole set
        // atomically or none of it, and Darwin will not accept a set this
        // size; splitting it into chunks of five would let another process
        // observe -- and act on -- a half-applied state, which is precisely
        // the guarantee the caller asked for. E2BIG is what Linux itself
        // returns when a set exceeds SEMOPM, so the guest at least sees an
        // errno from the right family, and the note below says why.
        warn_once(&g_said_sops,
                  "semop with %llu operations: Darwin accepts at most %d per "
                  "call (measured; XNU MAX_SOPS) and splitting the set would "
                  "break its atomicity. Returning E2BIG.\n",
                  (unsigned long long)nsops, DARWIN_MAX_SOPS);
        return LERR(E2BIG);
    }

    struct sembuf sops[DARWIN_MAX_SOPS];
    bool all_nowait = false;
    long rc = copy_sops(lsops, nsops, sops, deadline ? IPC_NOWAIT : 0,
                        &all_nowait);
    if (rc < 0)
        return rc;

    if (deadline && all_nowait) {
        // The poll loop below drives itself by ORing IPC_NOWAIT into every
        // operation, which makes a guest's OWN IPC_NOWAIT invisible: "the
        // caller asked not to block" and "the kernel says retry" become the
        // same EAGAIN, and a probe that Linux answers instantly sat in the
        // backoff for the whole timeout. Measured before this branch existed:
        // sembuf{sem_op=-1, sem_flg=IPC_NOWAIT} on a set at 0 with a 2 s
        // timeout returned -EAGAIN after 2.002 s; the same call on the Linux
        // guest returns -EAGAIN after 0.000 s. When every operation already
        // carries IPC_NOWAIT the answer needs no loop at all -- one semop,
        // whatever it says -- and the timeout is irrelevant because Linux
        // never waits either.
        //
        // A MIXED set (some operations with IPC_NOWAIT, some without) still
        // goes through the loop. Darwin's semop reports only that the set
        // could not proceed, never which operation blocked, so there is no way
        // to tell the "fail now" case from the "wait" case; looping keeps the
        // errno right and only the latency wrong, which is the smaller lie.
        if (semop(semid, sops, (size_t)nsops) != 0)
            return LERR(errno);
        return 0;
    }

    if (!deadline) {
        if (semop(semid, sops, (size_t)nsops) != 0) {
            int e = errno;
            if (e == EINVAL)
                warn_once(&g_said_semume,
                          "semop returned EINVAL. If the set used SEM_UNDO, "
                          "note kern.sysv.semume is 10 on macOS against "
                          "Linux's 500: only ten undo entries per process.\n");
            return LERR(e);
        }
        return 0;
    }

    // Darwin has no semtimedop. The emulation is semop with IPC_NOWAIT in a
    // retry loop against a monotonic deadline, so the wakeup latency is the
    // poll interval rather than immediate: a semaphore released while this
    // thread is asleep is not seen until the next probe. The backoff starts at
    // 100 us and doubles to 5 ms, which at the measured 131 semtimedop per
    // 10 s costs nothing and keeps a long wait off the CPU. It is a real
    // semantic loss and not a papered-over one: a Linux waiter is woken in
    // FIFO order the instant the operation can proceed, and this one is not,
    // so a heavily contended set can starve a waiter that a real kernel would
    // have served.
    //
    // Interruption survives, and that matters more than the latency: the sleep
    // is a nanosleep, so a signal delivered to this thread ends it with EINTR,
    // which is what Linux's semtimedop returns in the same situation.
    uint64_t poll_ns = 100000;          // 100 us
    const uint64_t poll_cap = 5000000;  // 5 ms
    for (;;) {
        if (semop(semid, sops, (size_t)nsops) == 0)
            return 0;
        int e = errno;
        if (e != EAGAIN)
            return LERR(e);             // a real failure, not "would block"

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        int64_t left = (int64_t)(deadline->tv_sec - now.tv_sec) * 1000000000 +
                       (int64_t)(deadline->tv_nsec - now.tv_nsec);
        if (left <= 0)
            return LERR(EAGAIN);        // Linux reports a timeout as EAGAIN

        uint64_t nap = poll_ns;
        if ((int64_t)nap > left)
            nap = (uint64_t)left;
        struct timespec ts = { (time_t)(nap / 1000000000),
                               (long)(nap % 1000000000) };
        if (nanosleep(&ts, NULL) != 0 && errno == EINTR)
            return LERR(EINTR);
        poll_ns = poll_ns * 2 > poll_cap ? poll_cap : poll_ns * 2;
    }
}

long lxrt_semop(int semid, const void *lsops, uint64_t nsops)
{
    return do_semop(semid, lsops, nsops, NULL);
}

long lxrt_semtimedop(int semid, const void *lsops, uint64_t nsops,
                     const void *ltimeout)
{
    if (!ltimeout)
        return do_semop(semid, lsops, nsops, NULL);
    if (!can_read(ltimeout, sizeof(struct linux_timespec)))
        return LERR(EFAULT);

    struct linux_timespec t;
    memcpy(&t, ltimeout, sizeof t);
    if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000)
        return LERR(EINVAL);

    // Linux measures the timeout against CLOCK_MONOTONIC, so this does too:
    // a wall-clock jump during a wait must not shorten or extend it.
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // t.tv_sec is a guest-controlled int64 and only its sign has been checked.
    // now.tv_sec + t.tv_sec is signed overflow -- undefined behaviour on a
    // value the guest picks -- and it does not merely wrap harmlessly:
    // measured before this clamp, tv_sec = INT64_MAX with tv_nsec = 999999999
    // landed the deadline in the PAST, so the first probe failed, `left <= 0`
    // fired, and a request for an effectively infinite wait came back -EAGAIN
    // after 0.000 s. Linux does not reject the value either -- verified on the
    // guest, semtimedop with tv_sec = INT64_MAX is accepted and simply waits
    // -- so the fix is to clamp, not to fail. LXRT_SEMOP_MAX_WAIT is ~3.2
    // years, longer than any process this runtime will host.
    const int64_t LXRT_SEMOP_MAX_WAIT = 100000000;
    if (t.tv_sec > LXRT_SEMOP_MAX_WAIT ||
        t.tv_sec > (int64_t)(INT64_MAX / 2) - (int64_t)now.tv_sec)
        t.tv_sec = LXRT_SEMOP_MAX_WAIT;

    struct timespec deadline;
    deadline.tv_sec = now.tv_sec + (time_t)t.tv_sec;
    deadline.tv_nsec = now.tv_nsec + (long)t.tv_nsec;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_nsec -= 1000000000;
        deadline.tv_sec += 1;
    }
    return do_semop(semid, lsops, nsops, &deadline);
}

// How many semaphores the set holds. Needed to bound GETALL/SETALL, which
// carry an unbounded guest array whose length is implied by the set, not by an
// argument.
static long sem_count(int semid, uint64_t *out)
{
    struct semid_ds ds;
    memset(&ds, 0, sizeof ds);
    semun_t u;
    u.buf = &ds;
    if (semctl(semid, 0, IPC_STAT, u) != 0)
        return LERR(errno);
    *out = ds.sem_nsems;
    return 0;
}

long lxrt_semctl(int semid, int semnum, int lcmd, uint64_t arg)
{
    const int cmd = lcmd & ~L_IPC_64;   // see note 2 at the top

    switch (cmd) {
    case L_IPC_RMID:
        return semctl(semid, semnum, IPC_RMID) != 0 ? LERR(errno) : 0;

    case L_IPC_STAT: {
        void *guest = (void *)(uintptr_t)arg;
        if (!can_write(guest, sizeof(struct linux_semid64_ds)))
            return LERR(EFAULT);
        struct semid_ds ds;
        memset(&ds, 0, sizeof ds);
        semun_t u;
        u.buf = &ds;
        if (semctl(semid, semnum, IPC_STAT, u) != 0)
            return LERR(errno);
        struct linux_semid64_ds l;
        semid_to_linux(&ds, &l);
        memcpy(guest, &l, sizeof l);
        return 0;
    }

    case L_IPC_SET: {
        const void *guest = (const void *)(uintptr_t)arg;
        if (!can_read(guest, sizeof(struct linux_semid64_ds)))
            return LERR(EFAULT);
        struct linux_semid64_ds l;
        memcpy(&l, guest, sizeof l);
        struct semid_ds ds;
        memset(&ds, 0, sizeof ds);
        semun_t u;
        u.buf = &ds;
        if (semctl(semid, semnum, IPC_STAT, u) != 0)
            return LERR(errno);
        perm_from_linux(&l.sem_perm, &ds.sem_perm);
        return semctl(semid, semnum, IPC_SET, u) != 0 ? LERR(errno) : 0;
    }

    // The scalar getters. The number changes, the meaning does not.
    // Each of these returns the value itself, so -1 is the only failure
    // signal and the call must be made exactly once -- a second call to read
    // the value would also reset errno.
    case L_GETPID:   { int v = semctl(semid, semnum, GETPID);
                       return v < 0 ? LERR(errno) : v; }
    case L_GETVAL:   { int v = semctl(semid, semnum, GETVAL);
                       return v < 0 ? LERR(errno) : v; }
    case L_GETNCNT:  { int v = semctl(semid, semnum, GETNCNT);
                       return v < 0 ? LERR(errno) : v; }
    case L_GETZCNT:  { int v = semctl(semid, semnum, GETZCNT);
                       return v < 0 ? LERR(errno) : v; }

    case L_SETVAL: {
        // The union is passed by value in one register. glibc stores an int
        // into it and hands the whole word to the kernel, so the value is the
        // low 32 bits and the top half is whatever was in the union before.
        semun_t u;
        u.val = (int)(uint32_t)arg;
        return semctl(semid, semnum, SETVAL, u) != 0 ? LERR(errno) : 0;
    }

    case L_GETALL:
    case L_SETALL: {
        uint64_t n = 0;
        long rc = sem_count(semid, &n);
        if (rc < 0)
            return rc;
        if (n == 0)
            return LERR(EINVAL);
        void *guest = (void *)(uintptr_t)arg;
        uint64_t bytes = n * sizeof(unsigned short);
        // unsigned short on both sides, so the element type needs no
        // conversion -- only the bounce through validated storage.
        unsigned short small[256];
        unsigned short *buf = small;
        if (n > sizeof small / sizeof small[0]) {
            buf = malloc((size_t)bytes);
            if (!buf)
                return LERR(ENOMEM);
        }
        semun_t u;
        u.array = buf;
        long out;
        if (cmd == L_GETALL) {
            if (!can_write(guest, bytes)) {
                out = LERR(EFAULT);
            } else if (semctl(semid, 0, GETALL, u) != 0) {
                out = LERR(errno);
            } else {
                memcpy(guest, buf, (size_t)bytes);
                out = 0;
            }
        } else {
            if (!can_read(guest, bytes)) {
                out = LERR(EFAULT);
            } else {
                memcpy(buf, guest, (size_t)bytes);
                out = semctl(semid, 0, SETALL, u) != 0 ? LERR(errno) : 0;
            }
        }
        if (buf != small)
            free(buf);
        return out;
    }

    case L_IPC_INFO:
    case L_SEM_STAT:
    case L_SEM_INFO:
    case L_SEM_STAT_ANY:
        // These exist so `ipcs` and `ipcrm` can enumerate the system. Darwin
        // has no equivalent -- its semctl has no command for them, and worse,
        // Linux's IPC_INFO (3) IS Darwin's GETNCNT (3), so forwarding the
        // number would return a plausible small integer instead of failing.
        // Nothing in the measured Steam stack calls these; a guest `ipcs`
        // will report nothing rather than report nonsense.
        return LERR(ENOSYS);

    default:
        return LERR(EINVAL);
    }
}

// ======================================================== shared memory =====

static _Atomic bool g_said_shmmax;
static _Atomic bool g_said_shmmni;
static _Atomic bool g_said_shmflags;

// kern.sysv.shmmax is the per-segment cap and defaults to 4194304 on macOS.
// It is a CTLTYPE_QUAD, so it reads back as 8 bytes; read it once.
static uint64_t shmmax_bytes(void)
{
    static _Atomic uint64_t cached;
    uint64_t v = atomic_load(&cached);
    if (v)
        return v;
    // Declared CTLTYPE_QUAD in XNU and measured returning 8 bytes here, but
    // the buffer is zeroed first so a kernel that answers with 4 still lands
    // the value correctly on this little-endian target.
    uint64_t q = 0;
    size_t len = sizeof q;
    if (sysctlbyname("kern.sysv.shmmax", &q, &len, NULL, 0) != 0 || q == 0)
        q = 4194304;                    // the documented macOS default
    atomic_store(&cached, q);
    return q;
}

// ------------------------------------------------- POSIX shared memory path
//
// Steam's 10 805 760-byte segment cannot be served by Darwin's SysV shm at
// all: kern.sysv.shmmax is 4 MiB and raising it needs `sysctl -w` as root,
// applied at every boot, which is not something a runtime can arrange for
// itself. Refusing outright would mean steamwebhelper never starts on a stock
// machine, so oversized requests are served from POSIX shared memory instead,
// with the name derived from the guest's key so that independent processes
// asking for the same key land on the same object.
//
// Measured, on this machine: shm_open + one ftruncate(10 805 760) + mmap
// succeeds, a second process opening the same name sees the same bytes, and
// the object survives shm_unlink for anyone already mapped.
//
// What is given up, stated rather than hidden:
//
//   * Lifetime. Linux's IPC_RMID keeps a still-attached segment addressable,
//     marks it SHM_DEST and frees it at the last detach; that shape IS
//     reproduced here (see pshm_destroy). What is not reproduced is what
//     happens to a THIRD process: shm_unlink drops the name at once, so a
//     shmget for that key afterwards creates a new empty object, and a process
//     that still holds the old object open never learns it was removed.
//   * Permissions. SysV checks ipc_perm on every operation; POSIX shm checks
//     the file mode once, at open. Close, not identical. The mode is also
//     frozen at creation: Darwin's fchmod on a shm descriptor returns EINVAL
//     (measured), so IPC_SET records the new mode for IPC_STAT to report and
//     the object itself keeps the one it was born with. umask does NOT narrow
//     it -- measured, shm_open(..., 0666) under umask 077 still creates 0666 --
//     so a guest asking for 0666 gets 0666.
//   * shm_nattch counts only this runtime's attaches. There is no system-wide
//     attach count for a POSIX object, so a guest using nattch to decide when
//     to clean up will see a number that is too low.
//   * The identifier is process-local. A guest that creates a segment and
//     sends the raw shmid to an unrelated process leaves that process with a
//     number this table cannot resolve; it gets EINVAL, which is what Linux
//     returns for a stale id. Passing the KEY works, because the name is
//     derived from it, and that is how Steam's components find each other.
//   * Size. Darwin rounds the object up to a 16 KiB page -- measured,
//     ftruncate(10 805 760) fstats back as 10 813 440 -- and the exact request
//     is not recoverable from the object afterwards. So a segment CREATED here
//     reports what the guest asked for, and one ADOPTED by another process
//     reports the rounded size. That is a disagreement between processes as
//     well as with Linux, and it is the deliberate half of the trade: the
//     alternative, reporting the rounded size everywhere, would be consistent
//     but wrong for the single-process case as well. Nothing breaks either
//     way, since both numbers describe memory that is really mapped; a guest
//     that compares shm_segsz across processes for equality would notice.
//
// PSHMNAMLEN on Darwin is 31 characters including the leading slash; a 34
// character name was measured returning ENAMETOOLONG. The formats below are
// 21 and 22 characters.

// The table is sized to Linux's own defaults -- SHMMNI 4096 segments and
// SHMSEG 4096 attaches per process -- rather than to Darwin's kern.sysv
// numbers. The earlier caps of 32 and 64 were justified by shmmni=32, but a
// POSIX shm object is not a SysV segment and is not subject to shmmni at all;
// the real ceiling on this path is RLIMIT_NOFILE, because each live segment
// holds one descriptor. Both arrays are BSS, so the unused tail costs nothing
// until it is touched, and g_seg_high keeps the linear scans proportional to
// what is actually in use rather than to the size of the array.
#define MAX_PSHM   4096
#define MAX_ATTACH 4096

// The top of the shmid index space; see pshm_mint_id for why ids are built
// downwards from here.
#define PSHM_IX_TOP 0xffffu

struct pshm {
    bool     used;
    bool     private_key;       // created with IPC_PRIVATE: not findable by key
    bool     destroyed;         // IPC_RMID seen; alive only for its attaches
    int32_t  key;
    int      id;
    int      fd;
    uint64_t size;              // what the guest asked for, not the rounding
    uint32_t mode;
    int32_t  cpid, lpid;
    uint64_t nattch;            // this process only; see above
    int64_t  ctime, atime, dtime;
};

struct attach {
    bool     used;
    uint64_t addr, len;
    int      id;                // synthetic id this mapping belongs to
};

static struct pshm   g_seg[MAX_PSHM];
static struct attach g_att[MAX_ATTACH];
static int           g_seg_high;        // 1 + highest slot ever allocated
static int           g_att_high;
static uint32_t      g_pshm_gen = 1;    // sequence half of a synthetic id
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
// Fork safety is ipc_atfork_install below, registered on first use. Adding
// LXRT_FORK_SAFE here as well registered g_lock twice: the second prepare
// handler blocked on the lock the first had just taken, and the Steam client
// hung in fork() once X11 MIT-SHM had touched this module.

// Fork safety. The guest's fork() is a real Darwin fork() (runtime/process.c),
// so a child starts with exactly one thread and with whatever state g_lock was
// in at the moment of the call. Without a handler, a fork racing any thread
// that held g_lock left the child holding a locked mutex no one would ever
// release, and the child's first shm call would block forever.
//
// What the child inherits is deliberate rather than discarded: the descriptors
// in g_seg and the MAP_SHARED mappings in g_att both survive fork and are
// still valid in the child, and the child really does hold those attaches, so
// zeroing nattch or clearing the table would make its own bookkeeping wrong.
// The counts stay. What is knowingly left approximate is cpid/lpid, which
// still name the parent until the child's next attach, and the fact that
// parent and child now each count the same mapping in their own nattch -- both
// already follow from nattch being process-local, which the notes above say.
static void ipc_atfork_prepare(void) { pthread_mutex_lock(&g_lock); }
static void ipc_atfork_parent(void)  { pthread_mutex_unlock(&g_lock); }
static void ipc_atfork_child(void)
{
    // The one thread in the child is the owner of g_lock here -- the prepare
    // handler above took it in the parent -- so unlocking it is defined,
    // unlike re-initialising a held mutex.
    pthread_mutex_unlock(&g_lock);
}

static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;

static void ipc_atfork_install(void)
{
    pthread_atfork(ipc_atfork_prepare, ipc_atfork_parent, ipc_atfork_child);
}

static void ipc_lock(void)
{
    pthread_once(&g_atfork_once, ipc_atfork_install);
    pthread_mutex_lock(&g_lock);
}

static void ipc_unlock(void)
{
    pthread_mutex_unlock(&g_lock);
}

static void pshm_name(char *out, size_t cap, int32_t key, bool private_key)
{
    if (private_key) {
        // IPC_PRIVATE has no key to hash, so the name only has to be unique.
        // Only this process and its forks can reach it, which is what
        // IPC_PRIVATE means anyway.
        static _Atomic uint32_t seq;
        snprintf(out, cap, "/lxrtp%08x%08x", (unsigned)getpid(),
                 (unsigned)atomic_fetch_add(&seq, 1));
    } else {
        // The uid is folded in so two users on one machine do not collide on a
        // key, matching SysV's per-uid permission check closely enough.
        snprintf(out, cap, "/lxrt%08x%08x", (unsigned)(uint32_t)key,
                 (unsigned)getuid());
    }
}

// kern.sysv.shmmni is not only the number of kernel segments, it is the size
// of the kernel's shmid INDEX space. XNU builds a shmid as
// IXSEQ_TO_IPCID(ix, perm) = (perm._seq << 16) | (ix & 0xffff), and
// shm_find_segment_by_shmid rejects up front any id whose ix falls outside
// 0..shmmni-1. Read as a quad: measured here returning 8 bytes, value 32, and
// holding 32 segments at once showed the index range the kernel actually used
// to be exactly 0..31.
static uint64_t shmmni_count(void)
{
    static _Atomic uint64_t cached;
    uint64_t v = atomic_load(&cached);
    if (v)
        return v;
    uint64_t q = 0;
    size_t len = sizeof q;
    if (sysctlbyname("kern.sysv.shmmni", &q, &len, NULL, 0) != 0 || q == 0)
        q = 32;                         // the documented macOS default
    if (q > PSHM_IX_TOP)
        q = PSHM_IX_TOP;                // keep pshm_mint_id's arithmetic sane
    atomic_store(&cached, q);
    return q;
}

// Synthetic ids for POSIX-backed segments used to be PSHM_TAG | counter, on
// the claim that a kernel shmid never carries bit 0x40000000. That claim was
// false in both directions and cost real memory. Bit 0x40000000 is simply bit
// 14 of the sequence half, and _seq climbs on every allocation of a slot and
// is sticky until reboot: on this machine the very FIRST shmget of a run
// returned 0x40020000 (seq 0x4002, index 0). Driving it the other way,
// priming the table with 31 POSIX segments so the next synthetic id was
// 0x4000001f and then churning shmget/IPC_RMID on kernel slot 31, the kernel
// reissued exactly 0x4000001f after 29 148 iterations -- 0.02 s -- and
// lxrt_shmat on that legitimate KERNEL id returned the POSIX segment's memory
// instead of the kernel segment's.
//
// So identity is no longer taken from a bit pattern. A synthetic id is built
// in the part of the id space the kernel provably cannot address: its index
// half counts DOWN from 0xffff, which puts it above shmmni, and
// shm_find_segment_by_shmid rejects every such id before it ever looks at a
// segment (measured: shmctl(0x0001ffff, IPC_STAT) returns EINVAL, as does
// every id in that range). The sequence half is a generation counter kept
// below 0x8000 so the id stays a positive int, which mirrors what the kernel
// does with _seq and keeps a reused slot from handing back an id a guest was
// still holding.
//
// The shmctl(IPC_STAT) probe below cannot fire given the index argument. It is
// kept anyway: it is one syscall on a path taken once per segment, and it is
// the only check that would survive a future XNU changing how an id is formed.
static int pshm_mint_id(int slot)
{
    uint64_t ix = PSHM_IX_TOP - (uint64_t)slot;
    if (ix < shmmni_count())
        return -1;                      // no index left outside the kernel's
    for (int tries = 0; tries < 64; tries++) {
        if (g_pshm_gen == 0 || g_pshm_gen >= 0x8000u)
            g_pshm_gen = 1;
        uint32_t gen = g_pshm_gen++;
        int id = (int)((gen << 16) | (uint32_t)ix);
        struct shmid_ds ds;
        if (shmctl(id, IPC_STAT, &ds) == 0)
            continue;                   // a live kernel segment owns it
        return id;
    }
    return -1;
}

static struct pshm *pshm_by_id(int id)
{
    // An id whose index half is inside the kernel's range belongs to the
    // kernel and must never be matched here, whatever the table happens to
    // hold. This is the check that closes the misrouting measured above, and
    // it also makes the kernel path free of the scan below.
    if (id <= 0 || (uint64_t)(uint32_t)(id & 0xffff) < shmmni_count())
        return NULL;
    for (int i = 0; i < g_seg_high; i++)
        if (g_seg[i].used && g_seg[i].id == id)
            return &g_seg[i];
    return NULL;
}

static struct pshm *pshm_by_key(int32_t key)
{
    if (key == 0)
        return NULL;                    // IPC_PRIVATE is not findable by key
    for (int i = 0; i < g_seg_high; i++)
        if (g_seg[i].used && !g_seg[i].private_key && !g_seg[i].destroyed &&
            g_seg[i].key == key)
            return &g_seg[i];
    return NULL;
}

static struct pshm *pshm_alloc(void)
{
    for (int i = 0; i < MAX_PSHM; i++)
        if (!g_seg[i].used) {
            int id = pshm_mint_id(i);
            if (id < 0)
                return NULL;
            memset(&g_seg[i], 0, sizeof g_seg[i]);
            g_seg[i].used = true;
            g_seg[i].fd = -1;
            g_seg[i].id = id;
            if (i >= g_seg_high)
                g_seg_high = i + 1;
            return &g_seg[i];
        }
    return NULL;
}

// Caller holds g_lock. Linux's IPC_RMID does not destroy a segment that still
// has attaches: it detaches the key, sets SHM_DEST in shm_perm.mode and keeps
// the id addressable until the last detach. Verified on the guest -- after
// IPC_RMID with one attach outstanding, IPC_STAT still succeeds and reports
// mode 01600 with nattch 1, and a further shmat still succeeds. Keeping the
// entry alive has a second effect that matters here: the id is not recycled
// while a guest is still using it.
static void pshm_destroy(struct pshm *s)
{
    if (!s->destroyed) {
        if (!s->private_key) {
            char name[40];
            pshm_name(name, sizeof name, s->key, false);
            shm_unlink(name);
        }
        s->destroyed = true;
        // Linux resets shm_perm.key to IPC_PRIVATE on RMID, which is what
        // frees the key for immediate reuse by a new shmget.
        s->key = 0;
        s->ctime = (int64_t)time(NULL);
    }
    if (s->nattch == 0) {
        if (s->fd >= 0)
            close(s->fd);
        s->fd = -1;
        s->used = false;
    }
}

// Caller holds g_lock. The creating process publishes the name with
// shm_open(O_CREAT|O_EXCL) and only sizes it with ftruncate a moment later, so
// an adopter that arrives in between fstats st_size == 0 and used to fail the
// `have < size` test with EINVAL -- reproduced deterministically, and again
// live with a creator that delayed its ftruncate by 30 ms.
//
// The obvious fix is not available. flock() on a Darwin POSIX shm descriptor
// returns ENOTSUP (measured: errno 45 for LOCK_EX, LOCK_SH and LOCK_UN alike),
// so the size cannot be published atomically with the name that way, and POSIX
// shm has no rename to build the object under a private name first. So the
// adopter waits for a size, briefly and boundedly. 50 ms is far longer than
// the window -- it is one ftruncate wide -- and an object that is still zero
// after it is treated as genuinely empty, which is the honest reading.
static long pshm_measure(int fd, uint64_t want, uint64_t *have, uint32_t *mode)
{
    const uint64_t step_ns = 200000;    // 200 us
    const uint64_t cap_ns  = 50000000;  // 50 ms
    uint64_t waited = 0;
    struct stat st;
    for (;;) {
        if (fstat(fd, &st) != 0)
            return LERR(errno);
        if (st.st_size > 0 || want == 0)
            break;
        if (waited >= cap_ns) {
            // Nobody ever sized it. That is not a race any more, it is an
            // object whose creator died between shm_open and ftruncate, and
            // leaving it that way would make the key permanently unusable --
            // the name exists, so every later shmget adopts it and every
            // adopter fails the size check. Size it here instead. macOS
            // allows exactly one ftruncate per object, so a racer that gets
            // in first simply wins and this one re-reads the result.
            if (ftruncate(fd, (off_t)want) != 0 && errno != EINVAL)
                return LERR(errno);
            if (fstat(fd, &st) != 0)
                return LERR(errno);
            break;
        }
        struct timespec ts = { 0, (long)step_ns };
        nanosleep(&ts, NULL);
        waited += step_ns;
    }
    *have = (uint64_t)st.st_size;
    *mode = (uint32_t)(st.st_mode & PERM_MASK);
    return 0;
}

// How many times a lost create race is retried before the errno is believed.
// The window is one ftruncate wide and only one racer can lose per round, so
// eight is already generous; it is bounded so a pathological loop cannot spin
// forever against a process that keeps unlinking the name.
#define PSHM_RACE_TRIES 8

// Caller holds g_lock. Returns the id, or a negated Linux errno.
static long pshm_get(int32_t key, uint64_t size, int lshmflg)
{
    const bool priv = (key == 0);       // IPC_PRIVATE is 0 on both systems
    const bool excl = (lshmflg & L_IPC_CREAT) && (lshmflg & L_IPC_EXCL);

    struct pshm *s = pshm_by_key(key);
    if (s) {
        if (excl)
            return LERR(EEXIST);
        if (size && size > s->size)
            return LERR(EINVAL);        // Linux: requested size exceeds segsz
        return s->id;
    }

    char name[40];
    pshm_name(name, sizeof name, key, priv);

    int fd = -1;
    uint64_t have = 0;
    uint32_t adopted_mode = 0;
    bool created = false;

    if (priv) {
        // IPC_PRIVATE is always a create, with or without IPC_CREAT: Linux's
        // ipcget() routes key == IPC_PRIVATE straight to ipcget_new() and
        // never looks at the flag. This path used to demand IPC_CREAT and
        // answered -ENOENT without it -- verified against the guest, where
        // shmget(IPC_PRIVATE, 10805760, 0600) with no IPC_CREAT returns an id,
        // and that is exactly the call Steam's 10 805 760-byte segment makes.
        if (size == 0)
            return LERR(EINVAL);        // Linux: size 0 on a create is EINVAL
        fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR,
                      (mode_t)(lshmflg & PERM_MASK));
        if (fd < 0)
            return LERR(errno);
        // macOS allows a POSIX shm object to be sized exactly once; a second
        // ftruncate on the same object returns EINVAL (measured). So this must
        // happen here, on the creating path, and nowhere else.
        if (ftruncate(fd, (off_t)size) != 0) {
            int e = errno;
            close(fd);
            shm_unlink(name);
            return LERR(e);
        }
        // An IPC_PRIVATE segment has no key anyone could look it up by, so the
        // name exists only to create the object. Unlinking it here keeps the
        // object alive through this fd and any mapping of it, makes it
        // unreachable to every other process, and -- the reason it is done at
        // once rather than at IPC_RMID -- leaves nothing behind if the runtime
        // is killed. A POSIX shm object outlives its creator until reboot.
        // fork() still works: the child inherits the descriptor.
        shm_unlink(name);
        have = size;
        created = true;
    } else {
        // Adopt an existing object before creating one, so a second guest
        // process asking for the same key attaches to the same memory -- and
        // go back and adopt when the create loses, rather than handing the
        // loser's EEXIST to the guest. Two processes calling
        // shmget(key, oversize, IPC_CREAT|0600) at once with no IPC_EXCL both
        // succeed on Linux; before this loop, measured with four children
        // released from a pipe, two of the four failed (-EEXIST and -EINVAL).
        // EEXIST is only an answer when the caller actually asked for
        // IPC_EXCL.
        for (int attempt = 0; ; attempt++) {
            fd = shm_open(name, O_RDWR);
            if (fd >= 0) {
                if (excl) {
                    close(fd);
                    return LERR(EEXIST);
                }
                long rc = pshm_measure(fd, size, &have, &adopted_mode);
                if (rc < 0) {
                    close(fd);
                    return rc;
                }
                break;
            }
            int e = errno;
            if (e != ENOENT)
                return LERR(e);
            if (!(lshmflg & L_IPC_CREAT))
                return LERR(ENOENT);
            if (size == 0)
                return LERR(EINVAL);
            fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR,
                          (mode_t)(lshmflg & PERM_MASK));
            if (fd >= 0) {
                have = size;
                if (ftruncate(fd, (off_t)size) != 0) {
                    e = errno;
                    // macOS allows exactly one ftruncate per object, so EINVAL
                    // here means somebody sized the name in the instant
                    // between the create and this call -- which can only be
                    // another runtime that adopted it and ran the recovery in
                    // pshm_measure. Take their size rather than destroying an
                    // object they are already using.
                    struct stat st;
                    if (e != EINVAL || fstat(fd, &st) != 0 ||
                        (uint64_t)st.st_size < size) {
                        close(fd);
                        shm_unlink(name);
                        return LERR(e);
                    }
                    have = (uint64_t)st.st_size;
                }
                created = true;
                break;
            }
            e = errno;
            if (e != EEXIST || attempt >= PSHM_RACE_TRIES)
                return LERR(e);
            // Someone else created the name between the two calls above. Go
            // round and adopt what they made.
        }
    }

    // Only reachable on the adopt path: every create above leaves have >= size.
    if (size && have < size) {
        close(fd);
        return LERR(EINVAL);
    }

    s = pshm_alloc();
    if (!s) {
        close(fd);
        if (created && !priv)
            shm_unlink(name);
        return LERR(ENOSPC);
    }
    s->private_key = priv;
    s->key  = priv ? 0 : key;
    s->fd   = fd;
    // A created segment reports what the guest asked for. An ADOPTED one
    // reports the object's own size, which is what Linux does -- shmget for a
    // key that already exists returns the existing segment and IPC_STAT
    // reports ITS segsz, not the size the caller happened to pass. The
    // divergence that remains is the rounding: Darwin rounds a POSIX shm
    // object up to a 16 KiB page, so a 10 805 760-byte segment adopted by a
    // second process reports 10 813 440 where Linux would report the exact
    // request.
    s->size = created ? size : (have ? have : size);
    // The recorded mode is what IPC_STAT reports. On a create it is what the
    // guest asked for, which is also what the object got: umask does not
    // narrow shm_open on Darwin (measured, 0666 under umask 077 still creates
    // 0666). On an adopt it is the object's own mode, which may have been set
    // by a process that asked for something else; see the IPC_SET note.
    s->mode = created ? (uint32_t)(lshmflg & PERM_MASK) : adopted_mode;
    s->cpid = getpid();
    s->ctime = (int64_t)time(NULL);
    return s->id;
}

static void kdefer_sweep(void);     // "deferred IPC_RMID", below

static int shmflg_to_darwin_get(int lshmflg)
{
    // SHM_HUGETLB (04000) and SHM_NORESERVE (010000) are Linux-only and sit on
    // bits Darwin reads as IPC_NOWAIT and IPC_M; see the constants above.
    return lshmflg & (PERM_MASK | L_IPC_CREAT | L_IPC_EXCL);
}

long lxrt_shmget(int32_t key, uint64_t size, int lshmflg)
{
    if (lshmflg & (L_SHM_HUGETLB | L_SHM_NORESERVE))
        warn_once(&g_said_shmflags,
                  "shmget: dropping SHM_HUGETLB/SHM_NORESERVE; macOS SysV "
                  "shared memory has neither, and both bits mean something "
                  "else to Darwin.\n");

    ipc_lock();

    // Which backing serves a key is decided by what ALREADY exists, in one
    // fixed order -- this table, then the POSIX name, then the kernel -- and
    // only a key nothing holds yet is decided by the requested size. The
    // comment that used to sit here claimed the decision depended "only on the
    // requested size, never on transient state", and it did not hold: the
    // POSIX name was consulted only after the kernel answered ENOENT, so a
    // process asking for an under-shmmax size got a brand new kernel segment
    // while another process was already serving that key from a POSIX object,
    // and the two silently shared nothing. Measured: with the object for the
    // key pre-created at 10 805 760 bytes, shmget(key, 4096, IPC_CREAT|0600)
    // returned a kernel id and a 4096-byte segment.
    if (key != 0) {
        if (pshm_by_key(key)) {
            long r = pshm_get(key, size, lshmflg);
            ipc_unlock();
            return r;
        }
        char probe[40];
        pshm_name(probe, sizeof probe, key, false);
        int pfd = shm_open(probe, O_RDWR);
        if (pfd >= 0) {
            close(pfd);
            long r = pshm_get(key, size, lshmflg);
            ipc_unlock();
            return r;
        }
        // A size-0, mode-0 shmget is a pure lookup on both systems (verified
        // on both: it returns the id for a live key and ENOENT otherwise). If
        // the kernel already owns this key, the kernel answers -- even for an
        // oversized request, because then the right answer is the one Linux
        // gives, EINVAL for a request larger than the existing segment, not a
        // second segment hidden behind the same key. Verified on both sides:
        // shmget(key, 10805760, IPC_CREAT|0600) against a live 4096-byte
        // segment returns EINVAL.
        int kid = shmget((key_t)key, 0, 0);
        if (kid >= 0) {
            int id = shmget((key_t)key, (size_t)size,
                            shmflg_to_darwin_get(lshmflg));
            long r = id >= 0 ? (long)id : LERR(errno);
            ipc_unlock();
            return r;
        }
        if (errno != ENOENT) {
            long r = LERR(errno);       // EACCES on someone else's key, say
            ipc_unlock();
            return r;
        }
    }

    const uint64_t cap = shmmax_bytes();
    bool oversize = size > cap;

    if (!oversize) {
        kdefer_sweep();
        int id = shmget((key_t)key, (size_t)size, shmflg_to_darwin_get(lshmflg));
        if (id >= 0) {
            ipc_unlock();
            return id;
        }
        int e = errno;
        if (e == ENOSPC) {
            warn_once(&g_said_shmmni,
                      "shmget: out of SysV segments. macOS defaults to "
                      "kern.sysv.shmmni=32 system-wide and kern.sysv.shmseg=8 "
                      "attaches per process, against Linux's 4096 of each. "
                      "Raise them with sysctl as root at boot.\n");
        }
        if (e != ENOENT) {
            ipc_unlock();
            return LERR(e);
        }
        // ENOENT means the kernel has no segment for this key and the caller
        // did not ask to create one, or another process created the POSIX
        // object in the window since the probe above. Fall through: pshm_get
        // answers ENOENT too if there is genuinely nothing there.
    } else {
        warn_once(&g_said_shmmax,
                  "shmget(%llu) exceeds kern.sysv.shmmax (%llu). Serving it "
                  "from POSIX shared memory instead; lifetime, permission and "
                  "nattch semantics are approximated. To use real SysV shared "
                  "memory, raise the limit as root at boot:\n"
                  "  sudo sysctl -w kern.sysv.shmmax=%llu kern.sysv.shmall=%llu\n",
                  (unsigned long long)size, (unsigned long long)cap,
                  (unsigned long long)(64ull << 20),
                  (unsigned long long)((64ull << 20) / (uint64_t)getpagesize()));
    }

    long r = pshm_get(key, size, lshmflg);
    ipc_unlock();
    return r;
}

static struct attach *attach_alloc(void)
{
    for (int i = 0; i < MAX_ATTACH; i++)
        if (!g_att[i].used) {
            if (i >= g_att_high)
                g_att_high = i + 1;
            return &g_att[i];
        }
    return NULL;
}

static struct attach *attach_find(uint64_t addr)
{
    for (int i = 0; i < g_att_high; i++)
        if (g_att[i].used && g_att[i].addr == addr)
            return &g_att[i];
    return NULL;
}

// Is [addr, addr+len) nothing but an untagged PROT_NONE reservation? That is
// how FEX's 32-bit allocator holds the unused part of a guest window
// (GUESTBASE): the guest's view of those pages is "free", so an attach there
// may replace the placeholder, where anything else in the way must still
// make shmat fail as Linux does.
static bool only_reservation(uint64_t addr, uint64_t len)
{
    uint64_t p = addr, end = addr + len;
    while (p < end) {
        mach_vm_address_t ra = p;
        mach_vm_size_t rs = 0;
        vm_region_extended_info_data_t ri;
        mach_msg_type_number_t rc = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_EXTENDED_INFO,
                           (vm_region_info_t)&ri, &rc, &obj) != KERN_SUCCESS ||
            rs == 0 || ra > p)
            return false;               // a hole or the end: not a reservation
        if (ri.user_tag != 0 || ri.protection != VM_PROT_NONE)
            return false;
        p = ra + rs;
    }
    return true;
}

// ------------------------------------------ kernel segments: deferred IPC_RMID
//
// Linux keeps a segment that was IPC_RMID'd while attached alive AND
// attachable until its last detach (shm_perm.mode shows SHM_DEST meanwhile).
// Darwin's shmctl(IPC_RMID) refuses every later shmat at once. X clients rely
// on the Linux rule: cairo's xlib SHM pool sends XShmAttach, which the X
// server carries out later, and marks the segment for removal right after it,
// before the server has attached (a Linux cairo does not XSync there:
// IPC_RMID_DEFERRED_RELEASE). On the Mac the server's shmat then failed, the
// client got BadAccess (MIT-SHM ShmAttach), and GDK's X error handler ended
// the program: Heroic's (Electron's) main process exited that way at start
// (MEASURED, benchmarks/stage24-heroic.txt).
//
// So an IPC_RMID of a kernel segment this process has attached is deferred:
// the segment stays attachable, and the real IPC_RMID is issued when this
// process detaches its last attach of it, execs or exits. A process killed
// before that leaves a record in RMID_DIR/<id> (its pid and the segment's
// ctime); a later shmget in any runtime process removes the segment of a
// record whose process is gone, as Linux would have at that process's exit.
// Only kernel segments: the POSIX-backed ones (pshm_destroy) already follow
// the Linux rule inside the runtime, and no other process can attach them.

#define RMID_DIR   "/tmp/lxrt-shm-rmid"
#define MAX_KATT   64
#define MAX_KDEFER 64

struct katt { bool used; uint64_t addr; int id; };
static struct katt g_katt[MAX_KATT];        // this process's kernel attaches
static int g_kdefer[MAX_KDEFER];            // ids whose IPC_RMID is deferred
static int g_nkdefer;

static void katt_add(uint64_t addr, int id)
{
    for (int i = 0; i < MAX_KATT; i++)
        if (!g_katt[i].used) {
            g_katt[i] = (struct katt){ true, addr, id };
            return;
        }
    // Full: this attach is not tracked, and an IPC_RMID of its segment is not
    // deferred unless another attach of it is (kdefer_try).
}

static int katt_take(uint64_t addr)
{
    for (int i = 0; i < MAX_KATT; i++)
        if (g_katt[i].used && g_katt[i].addr == addr) {
            g_katt[i].used = false;
            return g_katt[i].id;
        }
    return -1;
}

static bool katt_holds(int id)
{
    for (int i = 0; i < MAX_KATT; i++)
        if (g_katt[i].used && g_katt[i].id == id)
            return true;
    return false;
}

static int kdefer_index(int id)
{
    for (int i = 0; i < g_nkdefer; i++)
        if (g_kdefer[i] == id)
            return i;
    return -1;
}

static void kdefer_path(char *out, size_t cap, int id)
{
    snprintf(out, cap, "%s/%d", RMID_DIR, id);
}

// Defer the IPC_RMID of kernel segment `id`: true when it was deferred (the
// caller reports success), false when the caller should remove it now.
// Caller holds the lock.
static bool kdefer_try(int id)
{
    if (kdefer_index(id) >= 0)
        return true;                    // a second IPC_RMID: already marked
    if (!katt_holds(id) || g_nkdefer >= MAX_KDEFER)
        return false;
    struct shmid_ds ds;
    if (shmctl(id, IPC_STAT, &ds) != 0)
        return false;
    g_kdefer[g_nkdefer++] = id;
    char p[64];
    mkdir(RMID_DIR, 0700);
    kdefer_path(p, sizeof p, id);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd >= 0) {
        dprintf(fd, "%d %lld\n", (int)getpid(), (long long)ds.shm_ctime);
        close(fd);
    }
    return true;
}

// The deferred IPC_RMID of `id`, now. Caller holds the lock.
static void kdefer_release(int id)
{
    int i = kdefer_index(id);
    if (i < 0)
        return;
    g_kdefer[i] = g_kdefer[--g_nkdefer];
    shmctl(id, IPC_RMID, NULL);
    char p[64];
    kdefer_path(p, sizeof p, id);
    unlink(p);
}

// Records left by processes that died before their deferred IPC_RMID. The
// segment is removed only if it is still the one recorded (same ctime), so a
// reused id is left alone; a record whose pid is alive (or was reused) waits.
static void kdefer_sweep(void)
{
    DIR *d = lxrt_opendir_private(RMID_DIR);     // not in the guest's fd table
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char *end;
        long id = strtol(e->d_name, &end, 10);
        if (e->d_name[0] < '0' || e->d_name[0] > '9' || *end || id < 0 || id > INT32_MAX)
            continue;
        char p[64];
        kdefer_path(p, sizeof p, (int)id);
        FILE *f = fopen(p, "r");
        if (!f)
            continue;
        int pid = 0;
        long long ctime = 0;
        int n = fscanf(f, "%d %lld", &pid, &ctime);
        fclose(f);
        if (n != 2 || pid <= 0)
            continue;
        if (kill(pid, 0) == 0 || errno == EPERM)
            continue;                   // its process is still there
        struct shmid_ds ds;
        if (shmctl((int)id, IPC_STAT, &ds) == 0 && (long long)ds.shm_ctime == ctime)
            shmctl((int)id, IPC_RMID, NULL);
        unlink(p);
    }
    lxrt_closedir_private(d);
}

// This process is exiting or exec'ing: every deferred IPC_RMID happens now.
// Linux frees such a segment once the last process detaches, which exit and
// exec both do.
void lxrt_sysv_exit(void)
{
    if (!g_nkdefer)
        return;
    ipc_lock();
    while (g_nkdefer)
        kdefer_release(g_kdefer[g_nkdefer - 1]);
    ipc_unlock();
}

long lxrt_shmat(int shmid, uint64_t shmaddr, int lshmflg)
{
    if (lshmflg & L_SHM_REMAP)
        // Linux's SHM_REMAP replaces whatever is already mapped at the
        // address. Darwin's shmat has no such flag and no way to express it
        // without unmapping first, which is not atomic and would leave a hole
        // another thread could take. Refused rather than approximated.
        return LERR(EINVAL);

    if (lshmflg & L_SHM_EXEC)
        // Linux's SHM_EXEC asks for an executable shared mapping. Apple
        // Silicon will not give a thread write and execute on the same page
        // (see jit.c), and a SysV segment is writable by definition, so this
        // cannot be honoured rather than merely being unimplemented.
        return LERR(ENOSYS);

    // SHM_RND rounds the address down to SHMLBA. SHMLBA is the page size on
    // both, but the page sizes differ: 4096 (or 65536) on Linux aarch64
    // against 16384 on Apple Silicon, where sys/shm.h defines SHMLBA as
    // 16*1024 explicitly. The rounding is therefore done here, to Darwin's
    // value, and SHM_RND is not forwarded -- letting Darwin round again would
    // be harmless but letting the guest assume Linux's granularity is not.
    uint64_t want = shmaddr;
    if (want && (lshmflg & L_SHM_RND))
        want = LXRT_ALIGN_DOWN(want, SHMLBA);

    ipc_lock();
    struct pshm *s = pshm_by_id(shmid);

    if (!s) {
        ipc_unlock();
        // SHM_RND is not forwarded: the rounding above already used
        // Darwin's SHMLBA, and only SHM_RDONLY is left to carry.
        struct shmid_ds ds;
        if (want && shmctl(shmid, IPC_STAT, &ds) == 0 &&
            only_reservation(want, LXRT_ALIGN_UP(ds.shm_segsz, SHMLBA)))
            munmap((void *)(uintptr_t)want, LXRT_ALIGN_UP(ds.shm_segsz, SHMLBA));
        void *p = shmat(shmid, (const void *)(uintptr_t)want,
                        lshmflg & L_SHM_RDONLY);
        if (p == (void *)-1)
            return LERR(errno);
        ipc_lock();
        katt_add((uint64_t)(uintptr_t)p, shmid);
        ipc_unlock();
        return (long)(uintptr_t)p;
    }

    // POSIX-backed segment: an ordinary MAP_SHARED mapping of the object.
    // A segment that has been IPC_RMID'd is still attachable while it lives,
    // which is what Linux does: verified on the guest, shmat after IPC_RMID
    // with an attach outstanding still succeeds. pshm_destroy only frees the
    // entry once the last attach is gone, so reaching one here means it is
    // genuinely still alive.
    int prot = (lshmflg & L_SHM_RDONLY) ? PROT_READ : (PROT_READ | PROT_WRITE);
    struct attach *a = attach_alloc();
    if (!a) {
        ipc_unlock();
        // Linux reports a process that has attached too many segments as
        // EMFILE, not ENOMEM: this is a table-full condition, not an
        // out-of-memory one, and a guest that distinguishes them acts on the
        // difference.
        return LERR(EMFILE);
    }
    int fixed = want && only_reservation(want, LXRT_ALIGN_UP(s->size, SHMLBA)) ? MAP_FIXED : 0;
    void *p = mmap((void *)(uintptr_t)want, (size_t)s->size, prot, MAP_SHARED | fixed,
                   s->fd, 0);
    if (p == MAP_FAILED) {
        int e = errno;
        ipc_unlock();
        return LERR(e);
    }
    // MAP_FIXED is used only over a bare reservation (above): anywhere else
    // it would silently destroy whatever the guest already had at that address. Without it the hint is advisory,
    // so an address the kernel could not honour has to be reported rather than
    // quietly relocated, which is what Linux does when the range is occupied.
    if (want && (uint64_t)(uintptr_t)p != want) {
        munmap(p, (size_t)s->size);
        ipc_unlock();
        return LERR(EINVAL);
    }
    a->used = true;
    a->addr = (uint64_t)(uintptr_t)p;
    a->len  = s->size;
    a->id   = s->id;
    s->nattch++;
    s->lpid = getpid();
    s->atime = (int64_t)time(NULL);
    ipc_unlock();
    return (long)(uintptr_t)p;
}

// Inside a guest window (GUESTBASE) a detached range goes back to being a
// PROT_NONE placeholder, as FEX's allocator does for munmap: a hole there
// would let the host's own allocations move into guest address space, where
// the next guest MAP_FIXED lands on them.
static void rereserve_if_window(uint64_t addr, uint64_t len)
{
    uint64_t base = lxrt_gbase();
    if (!base || addr < base || addr + len > base + (1ull << 32))
        return;
    mmap((void *)(uintptr_t)addr, (size_t)len, PROT_NONE,
         MAP_PRIVATE | MAP_ANON | MAP_FIXED | MAP_NORESERVE, -1, 0);
}

long lxrt_shmdt(uint64_t shmaddr)
{
    if (!shmaddr)
        return LERR(EINVAL);

    ipc_lock();
    struct attach *a = attach_find(shmaddr);
    if (!a) {
        ipc_unlock();
        // Not one of ours, so it is a kernel attach. Darwin's shmdt reports an
        // address that was never attached as EINVAL, same as Linux.
        mach_vm_address_t ra = shmaddr;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t ri;
        mach_msg_type_number_t rc = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        bool sized = mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                                    (vm_region_info_t)&ri, &rc, &obj) == KERN_SUCCESS &&
                     ra == shmaddr;
        if (shmdt((const void *)(uintptr_t)shmaddr) != 0)
            return LERR(errno);
        if (sized)
            rereserve_if_window(shmaddr, rs);
        ipc_lock();
        int kid = katt_take(shmaddr);
        if (kid >= 0 && !katt_holds(kid))
            kdefer_release(kid);        // its last attach here: see kdefer_try
        ipc_unlock();
        return 0;
    }
    size_t len = (size_t)a->len;
    struct pshm *s = pshm_by_id(a->id);
    a->used = false;
    if (s) {
        if (s->nattch)
            s->nattch--;
        s->lpid = getpid();
        s->dtime = (int64_t)time(NULL);
        // Last detach of a segment the guest already RMID'd: this is where
        // Linux actually frees it, and so is this.
        if (s->destroyed && s->nattch == 0)
            pshm_destroy(s);
    }
    ipc_unlock();
    if (munmap((void *)(uintptr_t)shmaddr, len) != 0)
        return LERR(errno);
    rereserve_if_window(shmaddr, LXRT_ALIGN_UP(len, SHMLBA));
    return 0;
}

// Fills a Linux shmid64_ds for a POSIX-backed segment. Everything here is
// synthesised from this process's own bookkeeping, which is why nattch is
// local-only; see the fallback note above.
static void pshm_stat(const struct pshm *s, struct linux_shmid64_ds *l)
{
    memset(l, 0, sizeof *l);
    l->shm_perm.key  = s->private_key ? 0 : s->key;
    l->shm_perm.uid  = getuid();
    l->shm_perm.gid  = getgid();
    l->shm_perm.cuid = getuid();
    l->shm_perm.cgid = getgid();
    // SHM_DEST is synthesised rather than masked away. Linux sets it in
    // shm_perm.mode once a segment has been RMID'd and still has attaches
    // (verified on the guest: mode 01600, nattch 1), and a guest that polls
    // for it is asking a question this module can answer for its own
    // segments -- unlike a kernel-backed one, where Darwin fails IPC_STAT
    // outright after RMID. See note 5 at the top.
    l->shm_perm.mode = (s->mode & PERM_MASK) | (s->destroyed ? L_SHM_DEST : 0);
    l->shm_segsz  = s->size;
    l->shm_cpid   = s->cpid;
    l->shm_lpid   = s->lpid;
    l->shm_nattch = s->nattch;
    l->shm_atime  = s->atime;
    l->shm_dtime  = s->dtime;
    l->shm_ctime  = s->ctime;
}

long lxrt_shmctl(int shmid, int lcmd, void *lbuf)
{
    const int cmd = lcmd & ~L_IPC_64;

    ipc_lock();
    struct pshm *s = pshm_by_id(shmid);

    if (s) {
        long out;
        switch (cmd) {
        case L_IPC_STAT: {
            if (!can_write(lbuf, sizeof(struct linux_shmid64_ds))) {
                out = LERR(EFAULT);
                break;
            }
            struct linux_shmid64_ds l;
            pshm_stat(s, &l);
            memcpy(lbuf, &l, sizeof l);
            out = 0;
            break;
        }
        case L_IPC_SET: {
            if (!can_read(lbuf, sizeof(struct linux_shmid64_ds))) {
                out = LERR(EFAULT);
                break;
            }
            struct linux_shmid64_ds l;
            memcpy(&l, lbuf, sizeof l);
            // Only the mode can be carried across. Changing the owner of a
            // POSIX object means fchown, which needs privileges this process
            // does not have, so a uid or gid change is refused instead of
            // being accepted and ignored.
            if (l.shm_perm.uid != getuid() || l.shm_perm.gid != getgid()) {
                out = LERR(EPERM);
                break;
            }
            // Darwin fixes a POSIX shm object's mode at shm_open time and
            // gives no way to change it afterwards. Measured on this machine:
            // shm_open("/probe", O_CREAT|O_EXCL|O_RDWR, 0600) followed by
            // fchmod(fd, 0644) returns -1 with errno 22, and fchown likewise.
            // So the fchmod that used to be here could never succeed, and
            // every guest chmod of a POSIX-backed segment came back -EINVAL
            // where Linux returns 0 -- steamwebhelper widening a segment to
            // 0666 for its renderer children is exactly that call. The mode is
            // recorded instead: IPC_STAT reports what the guest set and the
            // object keeps the mode it was created with. That is the same
            // approximation this path already makes for permissions (POSIX shm
            // checks its mode once at open, SysV checks ipc_perm on every
            // operation), and it costs nothing for the case that matters,
            // because every process in the Steam stack runs as the same uid
            // and reaches the object through its owner bits either way.
            s->mode = l.shm_perm.mode & PERM_MASK;
            s->ctime = (int64_t)time(NULL);
            out = 0;
            break;
        }
        case L_IPC_RMID: {
            // pshm_destroy drops the name (a keyed one is a pure function of
            // the key and the uid, so it can be rebuilt there; a private one
            // was already unlinked at creation), then either frees the entry
            // or keeps it alive for the attaches that remain, exactly as Linux
            // does. Existing mappings stay valid either way.
            //
            // What still differs, and cannot be fixed from user space: a later
            // shmget for this key creates a fresh empty object instead of
            // failing, and another PROCESS still holding this object open
            // never learns it was removed -- there is no system-wide refcount
            // on a POSIX shm object to consult.
            pshm_destroy(s);
            out = 0;
            break;
        }
        default:
            out = LERR(ENOSYS);
            break;
        }
        ipc_unlock();
        return out;
    }
    ipc_unlock();

    switch (cmd) {
    case L_IPC_RMID: {
        // Deferred while this process has it attached (see kdefer_try).
        ipc_lock();
        bool deferred = kdefer_try(shmid);
        ipc_unlock();
        if (deferred)
            return 0;
        return shmctl(shmid, IPC_RMID, NULL) != 0 ? LERR(errno) : 0;
    }

    case L_IPC_STAT: {
        if (!can_write(lbuf, sizeof(struct linux_shmid64_ds)))
            return LERR(EFAULT);
        struct shmid_ds ds;
        memset(&ds, 0, sizeof ds);
        if (shmctl(shmid, IPC_STAT, &ds) != 0)
            return LERR(errno);
        struct linux_shmid64_ds l;
        shmid_to_linux(&ds, &l);
        // A deferred IPC_RMID reads as Linux's removed-but-attached segment.
        ipc_lock();
        if (kdefer_index(shmid) >= 0)
            l.shm_perm.mode |= L_SHM_DEST;
        ipc_unlock();
        memcpy(lbuf, &l, sizeof l);
        return 0;
    }

    case L_IPC_SET: {
        if (!can_read(lbuf, sizeof(struct linux_shmid64_ds)))
            return LERR(EFAULT);
        struct linux_shmid64_ds l;
        memcpy(&l, lbuf, sizeof l);
        struct shmid_ds ds;
        memset(&ds, 0, sizeof ds);
        if (shmctl(shmid, IPC_STAT, &ds) != 0)
            return LERR(errno);
        perm_from_linux(&l.shm_perm, &ds.shm_perm);
        return shmctl(shmid, IPC_SET, &ds) != 0 ? LERR(errno) : 0;
    }

    case L_SHM_LOCK:
    case L_SHM_UNLOCK:
        // Linux pins a segment in RAM. Darwin's shmctl has no such command,
        // and mlock cannot stand in: the caller holds an id, not a mapping,
        // and the segment may not be attached in this process at all.
        return LERR(ENOSYS);

    case L_IPC_INFO:
    case L_SHM_STAT:
    case L_SHM_INFO:
    case L_SHM_STAT_ANY:
        // Enumeration commands for `ipcs`. Darwin exposes the same data only
        // through sysctl kern.sysv.*, in a different shape. Nothing in the
        // Steam stack asks for them.
        return LERR(ENOSYS);

    default:
        return LERR(EINVAL);
    }
}
