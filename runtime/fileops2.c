// The remaining file, process and scheduling syscalls.
//
// Six number spaces have already bitten this runtime -- errno, open flags,
// sockaddr, fcntl locks, msghdr/cmsghdr and SO_*. Everything in this file sits
// in the same trap: constants whose low range agrees and which diverge above
// it, so the common case works and the wrong answer arrives later with no
// symptom attached to it. Three of them are here, and one is nastier than
// anything in the list above:
//
//   wait options   Linux  Darwin      sched policy   Linux  Darwin
//   WNOHANG            1       1      SCHED_OTHER        0       1
//   WUNTRACED          2       2      SCHED_FIFO         1       4
//   WEXITED            4       4      SCHED_RR           2       2
//   WSTOPPED           2       8
//   WCONTINUED         8    0x10      getpriority     20-nice  nice
//   WNOWAIT     0x1000000    0x20
//
// Linux WCONTINUED is 8 and Darwin WSTOPPED is 8. A pass-through does not fail;
// it asks a different question and gets a plausible answer to it. That is the
// exact shape of bug this file exists to prevent.
//
// flock is the one thing here that is NOT a translation problem: LOCK_SH/EX/
// NB/UN are 1/2/4/8 on both. It is in this file because FEX blocks on
// flock(LOCK_EX) over its code-map file and the runtime did not implement it.

#include "lxrt.h"
#include "ids.h"
bool lxrt_trace_on(void);
#include "fileops2.h"
#include "props.h"
#include "android_ids.h"

#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/attr.h>
#include <sched.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <stdio.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))

// EOPNOTSUPP IS NOT SPELLABLE HERE, and finding out why cost a debugging round.
//
// Linux makes EOPNOTSUPP and ENOTSUP the same number, 95. Darwin does not:
// ENOTSUP is 45 and EOPNOTSUPP is 102, two distinct values. errno_map.c carries
// only ENOTSUP -- its comment says Darwin aliases them, which is the mistake --
// so LERR(EOPNOTSUPP) falls off the end of that table and comes back as the
// default, Linux EIO. A guest told EIO where it expected EOPNOTSUPP treats a
// missing feature as a failing disk.
//
// Until errno_map.c grows the 102 case, ENOTSUP is the spelling that produces
// Linux 95. Every "this mode has no Darwin equivalent" path below uses it.

// ------------------------------------------------------------ guest pointers
//
// A guest pointer is an arbitrary 64-bit number. Dereferencing a bad one kills
// the whole runtime -- guest, graphics bridge and all -- where Linux would have
// returned EFAULT to the one syscall. A NULL check is not enough: FEX probes
// the address space by handing the kernel addresses it expects to be unmapped
// and reading the errno back.
//
// mach_vm_region is the only way to ask Darwin whether a range is mapped
// without touching it. It costs one mach trap per region crossed, which is
// nothing next to the statx/statfs work it guards, and is skipped entirely on
// the per-message path once the whole mmsghdr vector has been checked.
static bool mem_ok(const void *p, size_t n, bool need_write)
{
    if (!p || n == 0)
        return false;
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)p;
    mach_vm_address_t end = a + n;
    if (end < a)
        return false;   // the guest wrapped the address space

    while (a < end) {
        mach_vm_address_t ra = a;
        mach_vm_size_t rs = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t obj = MACH_PORT_NULL;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64,
                           (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS)
            return false;
        if (obj != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), obj);
        // mach_vm_region reports the first region AT OR AFTER the address, so a
        // start past `a` means `a` itself is in a hole.
        if (ra > a || rs == 0)
            return false;
        if (!(info.protection & VM_PROT_READ))
            return false;
        if (need_write && !(info.protection & VM_PROT_WRITE))
            return false;
        a = ra + rs;
    }
    return true;
}

// ------------------------------------------------------------ 32 flock
//
//   operation  Linux  Darwin
//   LOCK_SH        1       1
//   LOCK_EX        2       2
//   LOCK_NB        4       4
//   LOCK_UN        8       8
//
// Identical, verified against sys/fcntl.h on the SDK rather than assumed. The
// two behaviours that do differ are invisible in the constants:
//
//   * Linux rejects an operation that is not exactly one of SH/EX/UN with
//     EINVAL. Darwin's flock() accepts LOCK_SH|LOCK_EX and picks one. FEX asks
//     for LOCK_EX|LOCK_NB and retries, so a mis-parsed operation would show up
//     as a lock that never blocks, not as an error.
//   * a failed non-blocking lock is EWOULDBLOCK, which is EAGAIN: 35 on Darwin
//     and 11 on Linux. LERR does that swap; a raw return would tell the guest
//     EDEADLK, and FEX treats EDEADLK as fatal.
#define L_LOCK_SH 1
#define L_LOCK_EX 2
#define L_LOCK_NB 4
#define L_LOCK_UN 8

long lxrt_flock(int fd, int loperation)
{
    int mode = loperation & ~L_LOCK_NB;
    if (mode != L_LOCK_SH && mode != L_LOCK_EX && mode != L_LOCK_UN)
        return LERR(EINVAL);
    if (loperation & ~(L_LOCK_SH | L_LOCK_EX | L_LOCK_UN | L_LOCK_NB))
        return LERR(EINVAL);

    int d = 0;
    if (mode == L_LOCK_SH) d = LOCK_SH;
    if (mode == L_LOCK_EX) d = LOCK_EX;
    if (mode == L_LOCK_UN) d = LOCK_UN;
    if (loperation & L_LOCK_NB) d |= LOCK_NB;

    return flock(fd, d) != 0 ? LERR(errno) : 0;
}

// ------------------------------------------------------------ 95 waitid
//
// idtype: P_ALL=0, P_PID=1, P_PGID=2 on both -- Darwin's is an unnumbered enum
// in sys/wait.h whose declaration order fixes those values, and Linux's is a
// numbered enum. They agree. Linux adds P_PIDFD=3, which cannot work here
// because there are no pidfds.
//
// options: see the table at the top of the file. Nothing above WNOHANG agrees.
#define L_P_ALL  0
#define L_P_PID  1
#define L_P_PGID 2

#define L_WNOHANG    0x00000001
#define L_WSTOPPED   0x00000002   /* == Linux WUNTRACED */
#define L_WEXITED    0x00000004
#define L_WCONTINUED 0x00000008
#define L_WNOWAIT    0x01000000
#define L_WNOTHREAD  0x20000000
#define L_WALL       0x40000000
#define L_WCLONE     0x80000000

#define L_SIGCHLD 17              /* Darwin's is 20 */

// Linux siginfo_t is 128 bytes and a guest reads its fields by offset; see the
// same struct for the fault signals in signal.c, which fixes si_addr at 16.
// For SIGCHLD the union at 16 holds _sifields._sigchld instead.
struct linux_siginfo_chld {
    int32_t  si_signo;      //  0
    int32_t  si_errno;      //  4
    int32_t  si_code;       //  8
    int32_t  pad_;          // 12
    int32_t  si_pid;        // 16
    uint32_t si_uid;        // 20
    int32_t  si_status;     // 24
    int32_t  pad2_;         // 28
    int64_t  si_utime;      // 32
    int64_t  si_stime;      // 40
    uint8_t  rest[80];      // 48 .. 128
};
_Static_assert(sizeof(struct linux_siginfo_chld) == 128, "Linux siginfo_t is 128 bytes");
_Static_assert(__builtin_offsetof(struct linux_siginfo_chld, si_pid) == 16,
               "_sifields._sigchld starts at 16, where the fault signals keep si_addr");
_Static_assert(__builtin_offsetof(struct linux_siginfo_chld, si_status) == 24,
               "si_status offset is ABI");

// Linux struct rusage on aarch64. The first two fields are the only ones
// POSIX defines and the only ones Darwin documents as meaningful, but the
// layout has to be right for all of them or a guest reading ru_maxrss gets
// ru_stime's microseconds.
//
//   struct timeval   Linux                Darwin
//   tv_sec           __s64                long       (8, same)
//   tv_usec          __s64                __int32_t  (4 + 4 bytes of padding)
//
// So a memcpy of a Darwin timeval into a Linux one copies four bytes of
// uninitialised padding into the top half of tv_usec. Field by field, always.
//
// And the unit divergence, which is the quiet one: ru_maxrss is KILOBYTES on
// Linux and BYTES on Darwin. Passing it through overstates a child's peak
// resident set by 1024x, which is exactly the sort of number a memory watchdog
// acts on.
struct linux_rusage {
    int64_t ru_utime_sec,  ru_utime_usec;   //   0, 8
    int64_t ru_stime_sec,  ru_stime_usec;   //  16, 24
    int64_t ru_maxrss;                      //  32  (KiB on Linux)
    int64_t ru_ixrss;                       //  40
    int64_t ru_idrss;                       //  48
    int64_t ru_isrss;                       //  56
    int64_t ru_minflt;                      //  64
    int64_t ru_majflt;                      //  72
    int64_t ru_nswap;                       //  80
    int64_t ru_inblock;                     //  88
    int64_t ru_oublock;                     //  96
    int64_t ru_msgsnd;                      // 104
    int64_t ru_msgrcv;                      // 112
    int64_t ru_nsignals;                    // 120
    int64_t ru_nvcsw;                       // 128
    int64_t ru_nivcsw;                      // 136
};
_Static_assert(sizeof(struct linux_rusage) == 144, "Linux struct rusage is 144 bytes");

static void rusage_to_linux(const struct rusage *d, struct linux_rusage *l);
void lxrt_rusage_to_linux(const struct rusage *d, void *out)
{
    rusage_to_linux(d, (struct linux_rusage *)out);
}

static void rusage_to_linux(const struct rusage *d, struct linux_rusage *l)
{
    memset(l, 0, sizeof(*l));
    l->ru_utime_sec  = (int64_t)d->ru_utime.tv_sec;
    l->ru_utime_usec = (int64_t)d->ru_utime.tv_usec;
    l->ru_stime_sec  = (int64_t)d->ru_stime.tv_sec;
    l->ru_stime_usec = (int64_t)d->ru_stime.tv_usec;
    l->ru_maxrss     = (int64_t)d->ru_maxrss / 1024;   // bytes -> KiB
    l->ru_ixrss      = (int64_t)d->ru_ixrss;
    l->ru_idrss      = (int64_t)d->ru_idrss;
    l->ru_isrss      = (int64_t)d->ru_isrss;
    l->ru_minflt     = (int64_t)d->ru_minflt;
    l->ru_majflt     = (int64_t)d->ru_majflt;
    l->ru_nswap      = (int64_t)d->ru_nswap;
    l->ru_inblock    = (int64_t)d->ru_inblock;
    l->ru_oublock    = (int64_t)d->ru_oublock;
    l->ru_msgsnd     = (int64_t)d->ru_msgsnd;
    l->ru_msgrcv     = (int64_t)d->ru_msgrcv;
    l->ru_nsignals   = (int64_t)d->ru_nsignals;
    l->ru_nvcsw      = (int64_t)d->ru_nvcsw;
    l->ru_nivcsw     = (int64_t)d->ru_nivcsw;
}

// CLD_EXITED..CLD_CONTINUED are 1..6 on both, checked against Darwin's
// sys/signal.h. si_status, however, holds a SIGNAL NUMBER for everything except
// CLD_EXITED, and signal numbers do not agree (SIGCHLD alone is 17 vs 20), so
// it has to go through signal.c's map.
static int32_t chld_status_to_linux(int code, int dstatus)
{
    if (code == CLD_EXITED)
        return dstatus;
    int l = lxrt_signo_to_linux(dstatus);
    return l > 0 ? l : dstatus;
}

// The child's REAL uid, read from the kernel instead of guessed.
//
// si_uid for SIGCHLD is defined as the real uid of the CHILD, which differs
// from the caller's after any setuid child, and Darwin reports it in NEITHER
// wait path: measured, waitid() leaves si_uid 0 for a child whose real uid was
// 501, where Linux fills in the child's ruid (measured 1000 on the aarch64
// guest). sysctl(KERN_PROC_PID) does know it, and keeps knowing it while the
// process is a zombie -- measured answering 501 for an exited-but-unreaped
// child, for a stopped child, and failing with a short answer only once the
// child has actually been reaped.
//
// So it is read where the kernel will answer: before the call when the guest
// named a single pid, after the call otherwise. Zero is itself a uid, so a
// guest reading si_uid unconditionally would see "root" for a field left blank
// -- which is why this is read wherever it can be read rather than left at zero
// everywhere, and why the residual is spelled out at the one call site that
// still cannot fill it.
static bool proc_ruid(pid_t pid, uid_t *out)
{
    struct kinfo_proc kp;
    memset(&kp, 0, sizeof kp);
    size_t len = sizeof kp;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)pid };
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len < sizeof kp)
        return false;
    *out = kp.kp_eproc.e_pcred.p_ruid;
    return true;
}

// Fills si_uid from whichever of the two readings the kernel will still answer.
// `pre` is the uid read before the call, valid only when `pre_ok` and only for
// the pid the guest named.
static void fill_si_uid(struct linux_siginfo_chld *out, pid_t reaped,
                        uid_t pre, bool pre_ok, pid_t named)
{
    uid_t u;
    if (proc_ruid(reaped, &u))
        out->si_uid = (uint32_t)u;
    else if (pre_ok && reaped == named)
        out->si_uid = (uint32_t)pre;
    // Otherwise left at zero: waitid(P_ALL) or P_PGID cannot name the pid in
    // advance and the process is gone by the time it can. Not guessed from
    // getuid() -- that was measured reporting the CALLER's 501 for a field whose
    // whole purpose is to say when the child's uid differs, and this file
    // refuses invented values elsewhere for the same reason (si_utime/si_stime,
    // getresuid's saved id, faccessat2's AT_EMPTY_PATH).
}

// Builds the Linux siginfo from a wait STATUS word rather than from a Darwin
// siginfo. Used by the rusage path below, which has to go through wait4.
static void status_to_siginfo(struct linux_siginfo_chld *out, pid_t pid,
                              int status)
{
    out->si_signo = L_SIGCHLD;
    out->si_pid = (int32_t)pid;
    // si_uid is the caller's job: see fill_si_uid. It is not derivable from a
    // status word.
    if (WIFEXITED(status)) {
        out->si_code = CLD_EXITED;
        out->si_status = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        out->si_code = WCOREDUMP(status) ? CLD_DUMPED : CLD_KILLED;
        out->si_status = chld_status_to_linux(CLD_KILLED, WTERMSIG(status));
    } else if (WIFSTOPPED(status)) {
        out->si_code = CLD_STOPPED;
        out->si_status = chld_status_to_linux(CLD_STOPPED, WSTOPSIG(status));
    } else {
        out->si_code = CLD_CONTINUED;
        out->si_status = L_SIGCHLD;
    }
}

long lxrt_waitid(int lidtype, int id, void *uinfop, int loptions, void *urusage)
{
    if (lxrt_ids_on() && lidtype == L_P_PID) {
        id = lxrt_ids_target_pid(id);
        if (id < 0) return LERR(ECHILD);
    }
    if (lidtype != L_P_ALL && lidtype != L_P_PID && lidtype != L_P_PGID)
        return LERR(EINVAL);   // P_PIDFD(3): no pidfds in this runtime
    // Linux insists the caller say which state transitions it wants to hear
    // about. Without this check, options==0 would silently become "report
    // nothing" and the guest would block forever.
    if (!(loptions & (L_WEXITED | L_WSTOPPED | L_WCONTINUED)))
        return LERR(EINVAL);
    if (loptions & ~(L_WNOHANG | L_WSTOPPED | L_WEXITED | L_WCONTINUED |
                     L_WNOWAIT | L_WNOTHREAD | L_WALL | L_WCLONE))
        return LERR(EINVAL);
    if (uinfop && !mem_ok(uinfop, sizeof(struct linux_siginfo_chld), true))
        return LERR(EFAULT);
    if (urusage && !mem_ok(urusage, sizeof(struct linux_rusage), true))
        return LERR(EFAULT);

    struct linux_siginfo_chld info;
    memset(&info, 0, sizeof info);

    // __WALL/__WCLONE/__WNOTHREAD select between clone-children and ordinary
    // children. Darwin has no such distinction -- every child is an ordinary
    // child -- so they are dropped rather than refused: the set they would
    // narrow is the set we already have.
    int dopt = 0;
    if (loptions & L_WNOHANG)    dopt |= WNOHANG;
    if (loptions & L_WSTOPPED)   dopt |= WSTOPPED;      // 2 -> 8
    if (loptions & L_WEXITED)    dopt |= WEXITED;       // 4 -> 4
    if (loptions & L_WCONTINUED) dopt |= WCONTINUED;    // 8 -> 0x10
    if (loptions & L_WNOWAIT)    dopt |= WNOWAIT;       // 0x1000000 -> 0x20

    if (urusage) {
        // Darwin's waitid() has no rusage out-parameter, and there is no second
        // call that can recover a child's usage after it has been reaped --
        // getrusage(RUSAGE_CHILDREN) is cumulative over every child ever reaped,
        // not this one. wait4() does report it, so the rusage form is routed
        // there instead of zero-filling a struct the guest will believe.
        //
        // What wait4 cannot express is WNOWAIT (peek without reaping): it always
        // reaps. Refusing is the only honest answer -- reaping a child the guest
        // asked to leave waitable loses the exit status for its real wait.
        if (loptions & L_WNOWAIT)
            return LERR(ENOSYS);

        // The same reasoning one bit over. wait4 has no WEXITED selector: it
        // ALWAYS reports and reaps an exited child. A guest that asked only
        // about stops -- waitid(P_ALL, 0, &info, WSTOPPED, &ru) -- would have an
        // exited child reaped here and reported as CLD_EXITED, and its real
        // waitid(WEXITED) would then get ECHILD with the status gone. Measured
        // on the aarch64 guest: Linux answers that call ECHILD and leaves the
        // zombie, and the follow-up WEXITED returns status 42. Refused rather
        // than silently reaped; the combination is rare and losing an exit
        // status is not recoverable.
        if (!(loptions & L_WEXITED))
            return LERR(ENOSYS);

        // wait4 reaps, and a reaped pid is gone from the process table, so the
        // child's real uid has to be read BEFORE the call when the guest named
        // one. See proc_ruid.
        uid_t pre_ruid = 0;
        bool pre_ok = (lidtype == L_P_PID) && proc_ruid((pid_t)id, &pre_ruid);

        pid_t target;
        switch (lidtype) {
        case L_P_ALL:  target = -1;         break;
        case L_P_PID:  target = (pid_t)id;  break;
        default:       target = -(pid_t)id; break;   // P_PGID
        }

        // wait4 has no WEXITED bit: exited children are always reported. Only
        // the stop/continue selectors carry over, and with wait4's numbering
        // (WUNTRACED, not waitid's WSTOPPED).
        int w4 = 0;
        if (loptions & L_WNOHANG)    w4 |= WNOHANG;
        if (loptions & L_WSTOPPED)   w4 |= WUNTRACED;
        if (loptions & L_WCONTINUED) w4 |= WCONTINUED;

        int status = 0;
        struct rusage du;
        memset(&du, 0, sizeof du);
        pid_t r = wait4(target, &status, w4, &du);
        if (r < 0)
            return LERR(errno);

        struct linux_rusage lru;
        rusage_to_linux(&du, &lru);
        memcpy(urusage, &lru, sizeof lru);
        if (r > 0) {
            status_to_siginfo(&info, r, status);
            fill_si_uid(&info, r, pre_ruid, pre_ok, (pid_t)id);
            info.si_pid = lxrt_ids_reaped_child(r, WIFEXITED(status) || WIFSIGNALED(status));
        }
        if (uinfop)
            memcpy(uinfop, &info, sizeof info);
        return 0;
    }

    uid_t pre_ruid = 0;
    bool pre_ok = (lidtype == L_P_PID) && proc_ruid((pid_t)id, &pre_ruid);

    siginfo_t dinfo;
    memset(&dinfo, 0, sizeof dinfo);
    if (waitid((idtype_t)lidtype, (id_t)id, &dinfo, dopt) != 0)
        return LERR(errno);

    // WNOHANG with nothing to report leaves si_pid at 0 and si_signo at 0 on
    // both systems; the zeroed struct is then the correct answer and is written
    // back as-is, which is what Linux >= 4.7 does.
    if (dinfo.si_pid != 0) {
        info.si_signo = L_SIGCHLD;
        info.si_code = dinfo.si_code;
        info.si_pid = (int32_t)dinfo.si_pid;
        // NOT dinfo.si_uid: Darwin leaves it zero (measured against a child
        // whose real uid was 501). Read from the kernel instead.
        fill_si_uid(&info, dinfo.si_pid, pre_ruid, pre_ok, (pid_t)id);
        bool reaped = !(loptions & L_WNOWAIT) &&
            (dinfo.si_code == CLD_EXITED || dinfo.si_code == CLD_KILLED ||
             dinfo.si_code == CLD_DUMPED);
        info.si_pid = lxrt_ids_reaped_child(dinfo.si_pid, reaped);
        info.si_status = chld_status_to_linux(dinfo.si_code, dinfo.si_status);
        // si_utime/si_stime stay zero: Darwin's siginfo_t has no field for
        // them. Left at zero rather than guessed -- a guest that wants child
        // CPU time has times(2) and getrusage(RUSAGE_CHILDREN).
    }
    if (uinfop)
        memcpy(uinfop, &info, sizeof info);
    return 0;
}

// ------------------------------------------------------------ 437 openat2
//
// struct open_how is { u64 flags; u64 mode; u64 resolve; } -- 24 bytes, which
// Linux calls OPEN_HOW_SIZE_VER0. `size` is the guest's declared size and is
// the versioning mechanism: smaller than VER0 is EINVAL, larger is E2BIG unless
// every extra byte is zero.
//
// The point of openat2 over openat is that it is STRICT. openat ignores flag
// bits it does not know; openat2 fails. Reproducing that strictness is the
// whole reason a guest would call it.
struct linux_open_how { uint64_t flags, mode, resolve; };
_Static_assert(sizeof(struct linux_open_how) == 24, "OPEN_HOW_SIZE_VER0 is 24");

// aarch64 Linux O_* -- note these are NOT the asm-generic defaults. arm64
// overrides four of them in arch/arm64/include/uapi/asm/fcntl.h:
//
//            asm-generic   aarch64     Darwin
//   O_DIRECTORY  0x10000    0x4000    0x100000
//   O_NOFOLLOW   0x20000    0x8000      0x0100
//   O_DIRECT      0x4000   0x10000     (none; F_NOCACHE after open)
//   O_LARGEFILE   0x8000   0x20000     (none; always 64-bit)
//
// fsflags.c already carries the aarch64 values for the flags openat uses; this
// mask exists only to decide which bits are LEGAL, which openat never has to.
//
// THREE BITS THE MASK ADMITS THAT fsflags.c DOES NOT KNOW. Declaring a flag
// legal and then dropping it is the silent-downgrade this call exists to avoid,
// so each is decided here by name instead of falling off the end of
// lxrt_open_flags_to_darwin's switch:
//
//   O_PATH 0x200000   REFUSED with EOPNOTSUPP. An O_PATH fd is a name handle
//                     with NO access: Linux fails read() on it with EBADF
//                     (measured on the aarch64 guest: read = -1, errno 9).
//                     Darwin has no such open mode, and the obvious emulation --
//                     O_RDONLY|O_NONBLOCK -- was measured here returning a fd on
//                     which read() of /etc/hosts succeeded and returned 4 bytes.
//                     A guest holding what it believes is an unreadable handle
//                     and getting a readable one is a worse answer than a
//                     refusal, and making read() fail would need an fd registry
//                     that read/write in dispatch.c would have to consult.
//   O_NOATIME 0x40000 accepted and dropped. It suppresses the atime update, and
//                     Darwin has no per-open equivalent. The cost is a written
//                     atime, never a wrong field. The divergence runs the safe
//                     way: Linux requires ownership and returns EPERM otherwise
//                     (measured on the guest against root-owned /etc/hosts:
//                     errno 1), where this succeeds -- more permissive, never
//                     less.
//   FASYNC 0x2000     accepted and dropped. open(2) cannot actually arm SIGIO;
//                     Linux stores the bit and delivers nothing until an owner
//                     is set with fcntl(F_SETOWN) (measured on the guest: the
//                     open just succeeds). Dropping it costs nothing, exactly
//                     like the MSG_CONFIRM/MSG_MORE/MSG_FASTOPEN precedent by
//                     msg_flags_to_darwin below.
#define L_O_VALID_MASK 0x7fffc3ull   /* access mode | O_CREAT .. __O_TMPFILE */
#define L_O_ACCMODE    0x000003
#define L_O_CREAT      0x000040
#define L_O_FASYNC     0x002000
#define L_O_DIRECTORY  0x004000
#define L_O_DIRECT     0x010000
#define L_O_NOATIME    0x040000
#define L_O_PATH       0x200000
// arm64's own value (arch/arm64/include/uapi/asm/fcntl.h), NOT asm-generic's
// 0x20000 -- which on arm64 is O_LARGEFILE. With 0x20000 every 32-bit guest
// open through FEX (O_LARGEFILE is always set there) became O_NOFOLLOW and
// failed with ELOOP on a final symlink: ld.so could not dlopen libGL.so.1.
#undef L_O_NOFOLLOW
#define L_O_NOFOLLOW   0x8000
#define L_O_TMPFILE_BIT 0x400000     /* __O_TMPFILE on its own */
#define L_O_TMPFILE    0x404000      /* __O_TMPFILE | O_DIRECTORY */

// RESOLVE_* are pure pathwalk constraints. Two of them have exact Darwin
// counterparts, which is luckier than this file usually gets:
//
//   RESOLVE_NO_SYMLINKS 0x04  ->  O_NOFOLLOW_ANY    0x20000000
//   RESOLVE_BENEATH     0x08  ->  O_RESOLVE_BENEATH 0x00001000
//
// The rest do not, and they are SECURITY constraints -- a caller passing
// RESOLVE_IN_ROOT is containing a path it does not trust. Dropping one and
// opening the file anyway hands back exactly the file the caller was guarding
// against, with no indication anything was ignored. Each unsupported bit is
// refused instead.
#define L_RESOLVE_NO_XDEV       0x01
#define L_RESOLVE_NO_MAGICLINKS 0x02
#define L_RESOLVE_NO_SYMLINKS   0x04
#define L_RESOLVE_BENEATH       0x08
#define L_RESOLVE_IN_ROOT       0x10
#define L_RESOLVE_CACHED        0x20

// RESOLVE_IN_ROOT by hand. Walks `path` relative to `root`, treating `root`
// as "/": absolute paths and ".." never leave it, and a symlink's target is
// re-rooted the same way. Writes the canonical, symlink-free relative path
// into `out` ("" for the root itself). Returns 0 or -errno.
//
// Symlink targets are read with readlinkat(root, canonical-so-far/comp), so
// the walk itself never opens anything outside the root either.
static int walk_in_root(int root, const char *path, bool nofollow_last,
                        char *out, size_t outsz)
{
    enum { MAXLINKS = 40 };
    char work[PATH_MAX];            // what is still to be walked
    char cur[PATH_MAX];             // canonical path so far, no leading '/'
    size_t curlen = 0;
    int links = 0;
    if (strlen(path) >= sizeof work)
        return -ENAMETOOLONG;
    strcpy(work, path);
    cur[0] = 0;

    char *rest = work;
    while (*rest) {
        // Skip separators; an absolute (re)start means "back to root".
        if (*rest == '/') {
            while (*rest == '/') rest++;
            continue;
        }
        char *comp = rest;
        while (*rest && *rest != '/') rest++;
        bool last = (*rest == 0);
        size_t clen = (size_t)(rest - comp);
        if (clen == 1 && comp[0] == '.')
            continue;
        if (clen == 2 && comp[0] == '.' && comp[1] == '.') {
            // Pop one component; at the root, ".." is the root.
            while (curlen > 0 && cur[curlen - 1] != '/') curlen--;
            if (curlen > 0) curlen--;
            cur[curlen] = 0;
            continue;
        }
        if (curlen + 1 + clen + 1 >= sizeof cur)
            return -ENAMETOOLONG;
        size_t saved = curlen;
        if (curlen) cur[curlen++] = '/';
        memcpy(cur + curlen, comp, clen);
        curlen += clen;
        cur[curlen] = 0;

        if (last && nofollow_last)
            break;
        char target[PATH_MAX];
        ssize_t tl = readlinkat(root, cur, target, sizeof target - 1);
        if (tl < 0) {
            if (errno == EINVAL || errno == ENOENT || errno == ENOTDIR)
                continue;           // not a symlink (or missing: let open say so)
            return -errno;
        }
        target[tl] = 0;
        if (++links > MAXLINKS)
            return -ELOOP;
        // Splice: the target replaces the component; the remainder follows.
        char next[PATH_MAX];
        int n = snprintf(next, sizeof next, "%s%s%s", target,
                         *rest ? "/" : "", *rest ? rest : "");
        if (n < 0 || (size_t)n >= sizeof next)
            return -ENAMETOOLONG;
        strcpy(work, next);
        rest = work;
        if (target[0] == '/') {
            curlen = 0;             // absolute target: re-rooted
        } else {
            curlen = saved;         // relative target: from the parent
        }
        cur[curlen] = 0;
    }
    if (curlen >= outsz)
        return -ENAMETOOLONG;
    memcpy(out, cur, curlen + 1);
    return 0;
}

long lxrt_openat2(int ldirfd, const char *tpath, const void *uhow,
                  uint64_t size)
{
    if (!tpath)
        return LERR(EFAULT);
    if (size < sizeof(struct linux_open_how))
        return LERR(EINVAL);
    // Linux's sys_openat2 bounds usize at PAGE_SIZE and returns E2BIG past it
    // (measured on the aarch64 guest: size 100000 -> errno 7). The guest
    // kernel's page size is not knowable from here, so the bound is 65536, the
    // largest page size aarch64 Linux is built with -- which only ever makes
    // this MORE permissive than the guest's own kernel. The errno has to match
    // the trailing-nonzero-bytes case eleven lines down, which is already
    // E2BIG: both are "this struct is bigger than I understand".
    if (size > 65536)
        return LERR(E2BIG);
    if (!mem_ok(uhow, (size_t)size, false))
        return LERR(EFAULT);

    struct linux_open_how how;
    memcpy(&how, uhow, sizeof how);
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt]    openat2 \"%s\" flags=0x%llx mode=0%llo resolve=0x%llx\n", tpath,
                (unsigned long long)how.flags, (unsigned long long)how.mode, (unsigned long long)how.resolve);

    // Anything past VER0 must be zero, or the guest is asking for a feature
    // built after this code and must be told so rather than silently downgraded.
    const uint8_t *extra = (const uint8_t *)uhow + sizeof how;
    for (uint64_t i = sizeof how; i < size; i++)
        if (extra[i - sizeof how] != 0)
            return LERR(E2BIG);

    if (how.flags & ~L_O_VALID_MASK)
        return LERR(EINVAL);
    // O_PATH: emulated as openat emulates it (fsflags.c: O_RDONLY, O_SYMLINK
    // with O_NOFOLLOW). The refusal described by L_O_VALID_MASK made openat
    // and openat2 disagree, and GNU tar 1.35 extracts through
    // openat2(O_PATH|O_DIRECTORY|O_NOFOLLOW, RESOLVE_BENEATH): with
    // EOPNOTSUPP it could not unpack anything, and on a fresh install
    // steam.sh failed to set up the Steam Runtime (MEASURED). The readable
    // handle is the divergence openat already accepts.
    // Linux's mode rule, with Linux's own WILL_CREATE(flags) -- which is
    // O_CREAT *or* __O_TMPFILE, because O_TMPFILE creates an inode too. Mode
    // must fit the permission bits (S_IALLUGO, 07777, the same on both) when
    // the open can create, and must be zero when it cannot.
    if (how.flags & (L_O_CREAT | L_O_TMPFILE_BIT)) {
        if (how.mode & ~07777ull)
            return LERR(EINVAL);
    } else if (how.mode != 0) {
        return LERR(EINVAL);
    }

    if (how.flags & L_O_TMPFILE_BIT) {
        // __O_TMPFILE ALONE IS NOT O_TMPFILE, and testing for the pair let the
        // bare bit through: bit 22 is unknown to lxrt_open_flags_to_darwin, got
        // dropped, and the openat below opened the named file (measured: fd 3
        // for flags (1<<22)|O_WRONLY on a real path). A guest that believes it
        // holds an unnamed inode would then write into a file it never meant to
        // touch. Linux's build_open_flags demands the exact triple -- O_TMPFILE
        // set, O_CREAT clear -- and write access, and returns EINVAL otherwise;
        // both measured on the aarch64 guest as errno 22.
        if ((how.flags & (L_O_TMPFILE | L_O_CREAT)) != L_O_TMPFILE)
            return LERR(EINVAL);
        if ((how.flags & L_O_ACCMODE) == 0)   /* O_RDONLY: no MAY_WRITE */
            return LERR(EINVAL);
        // O_TMPFILE creates an unnamed inode in a directory. Darwin has no
        // equivalent -- an unlinked O_CREAT file is not the same thing, because
        // it is visible in the directory for the window between the two calls,
        // which is the race O_TMPFILE exists to close.
        return LERR(ENOTSUP);
    }

    int dflags = lxrt_open_flags_to_darwin((int)how.flags);

    uint64_t res = how.resolve;
    if (res & ~(L_RESOLVE_NO_XDEV | L_RESOLVE_NO_MAGICLINKS |
                L_RESOLVE_NO_SYMLINKS | L_RESOLVE_BENEATH |
                L_RESOLVE_IN_ROOT | L_RESOLVE_CACHED))
        return LERR(EINVAL);

    if (res & L_RESOLVE_NO_SYMLINKS)
        dflags |= O_NOFOLLOW_ANY;
    if (res & L_RESOLVE_BENEATH) {
        // Linux refuses an absolute path under RESOLVE_BENEATH with EXDEV
        // before it walks anything. It has to be checked here too, because the
        // integrator's translate() turns a guest-absolute path into a
        // host-absolute one and O_RESOLVE_BENEATH would then be asked to
        // contain a path that already escaped.
        if (tpath[0] == '/')
            return LERR(EXDEV);
        dflags |= O_RESOLVE_BENEATH;
    }
    if (res & L_RESOLVE_NO_MAGICLINKS) {
        // Magic links are /proc/<pid>/fd entries, which resolve to the open
        // file rather than to a path. procfs.c materialises real files, so
        // nothing in this process behaves like one and the constraint is
        // satisfied by construction. Accepted, not dropped.
    }
    if (res & L_RESOLVE_NO_XDEV)
        // "do not cross a mount point during the walk". Darwin has no openat
        // flag for it and no way to ask the pathwalk to stop at a boundary;
        // checking st_dev after the fact would not close the race the flag
        // exists to close.
        return LERR(EINVAL);
    if (res & L_RESOLVE_IN_ROOT) {
        // Treat the dirfd as "/" for the whole walk, including for absolute
        // paths and for ".." escapes: a per-lookup chroot. Darwin has nothing
        // below a real chroot(2) that does it, so the walk is done here, one
        // component at a time, with every symlink read and re-rooted by hand.
        //
        // This is not optional. FEX's rootfs overlay is exactly this call --
        // openat2(RootFSFD, path, RESOLVE_IN_ROOT) -- and while it returned
        // EINVAL, FEX punted to the host path and the x86-64 ld.so could not
        // find libc.so.6 inside the rootfs it had itself been loaded from
        // (benchmarks/stage5-fex.txt, dynamic x86-64).
        if (res & L_RESOLVE_BENEATH)
            return LERR(EINVAL);            // Linux: mutually exclusive
        char rel[PATH_MAX];
        int r = walk_in_root(lxrt_dirfd_to_darwin(ldirfd), tpath,
                             (how.flags & L_O_NOFOLLOW) != 0, rel, sizeof rel);
        if (r < 0)
            return LERR(-r);
        // Every symlink was resolved above, so the canonical path has none
        // left: O_NOFOLLOW and O_RESOLVE_BENEATH cost nothing and close the
        // race where one appears between the walk and the open.
        int fd = openat(lxrt_dirfd_to_darwin(ldirfd), rel[0] ? rel : ".",
                        dflags | ((dflags & O_SYMLINK) ? 0 : O_NOFOLLOW) | O_RESOLVE_BENEATH,
                        (mode_t)(how.mode & 07777));
        if (fd < 0)
            return LERR(errno);
        if (how.flags & L_O_DIRECT)
            (void)fcntl(fd, F_NOCACHE, 1);
        return fd;
    }
    if (res & L_RESOLVE_CACHED)
        // "fail rather than do any I/O". We cannot promise a lookup stays in
        // cache, and EAGAIN is what Linux itself returns when it cannot -- the
        // caller's documented response is to retry without the bit. This is the
        // one unsupported RESOLVE_* that has a correct answer other than EINVAL.
        return LERR(EAGAIN);

    int fd = openat(lxrt_dirfd_to_darwin(ldirfd), tpath, dflags,
                    (mode_t)(how.mode & 07777));
    if (fd < 0 && errno == EOPNOTSUPP && (how.flags & L_O_PATH))
        fd = lxrt_pathfd_open(lxrt_dirfd_to_darwin(ldirfd), tpath, (dflags & O_CLOEXEC) != 0);
    if (fd < 0)
        return LERR(errno);

    // O_DIRECT is a hint on Linux, not a guarantee, and its Darwin spelling is
    // a post-open fcntl. Failure to apply it is not failure to open.
    if (how.flags & L_O_DIRECT)
        (void)fcntl(fd, F_NOCACHE, 1);
    return fd;
}

// ------------------------------------------------------------ 291 statx
//
// struct statx is a Linux invention with no Darwin counterpart at all. 256
// bytes, and every offset is ABI:
//
//    0  u32 stx_mask               which fields below are actually valid
//    4  u32 stx_blksize
//    8  u64 stx_attributes
//   16  u32 stx_nlink
//   20  u32 stx_uid
//   24  u32 stx_gid
//   28  u16 stx_mode
//   30  u16 __spare0[1]
//   32  u64 stx_ino
//   40  u64 stx_size
//   48  u64 stx_blocks
//   56  u64 stx_attributes_mask    which bits of stx_attributes were examined
//   64  struct statx_timestamp stx_atime    { s64 tv_sec; u32 tv_nsec; s32 rsv }
//   80  struct statx_timestamp stx_btime
//   96  struct statx_timestamp stx_ctime
//  112  struct statx_timestamp stx_mtime
//  128  u32 stx_rdev_major
//  132  u32 stx_rdev_minor
//  136  u32 stx_dev_major
//  140  u32 stx_dev_minor
//  144  u64 stx_mnt_id
//  152  u32 stx_dio_mem_align
//  156  u32 stx_dio_offset_align
//  160  u64 __spare3[12]
//
// stx_mask is the honest part of the interface and the reason statx exists:
// it says which of those the filesystem actually answered. Setting a bit for a
// field left at zero is worse than omitting the field, because the guest then
// believes the zero. Only the fields Darwin's stat really supplies are claimed.
struct linux_statx_timestamp {
    int64_t  tv_sec;
    uint32_t tv_nsec;
    int32_t  reserved_;
};

struct linux_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t spare0_[1];
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct linux_statx_timestamp stx_atime;
    struct linux_statx_timestamp stx_btime;
    struct linux_statx_timestamp stx_ctime;
    struct linux_statx_timestamp stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align;
    uint32_t stx_dio_offset_align;
    uint64_t spare3_[12];
};
_Static_assert(sizeof(struct linux_statx) == 256, "struct statx is 256 bytes");
_Static_assert(__builtin_offsetof(struct linux_statx, stx_ino) == 32, "ABI offset");
_Static_assert(__builtin_offsetof(struct linux_statx, stx_atime) == 64, "ABI offset");
_Static_assert(__builtin_offsetof(struct linux_statx, stx_rdev_major) == 128, "ABI offset");
_Static_assert(__builtin_offsetof(struct linux_statx, stx_dev_major) == 136, "ABI offset");

#define L_STATX_TYPE        0x0001
#define L_STATX_MODE        0x0002
#define L_STATX_NLINK       0x0004
#define L_STATX_UID         0x0008
#define L_STATX_GID         0x0010
#define L_STATX_ATIME       0x0020
#define L_STATX_MTIME       0x0040
#define L_STATX_CTIME       0x0080
#define L_STATX_INO         0x0100
#define L_STATX_SIZE        0x0200
#define L_STATX_BLOCKS      0x0400
#define L_STATX_BTIME       0x0800
/* not claimed: STATX_MNT_ID 0x1000, STATX_DIOALIGN 0x2000 */

// Linux file attribute bits against Darwin's st_flags. Four of the nine map
// exactly; the rest (ENCRYPTED, AUTOMOUNT, MOUNT_ROOT, VERITY, DAX) have no
// Darwin source, so they are left clear AND left out of stx_attributes_mask,
// which is the field whose whole job is to say "I did not look at this".
#define L_STATX_ATTR_COMPRESSED 0x000004
#define L_STATX_ATTR_IMMUTABLE  0x000010
#define L_STATX_ATTR_APPEND     0x000020
#define L_STATX_ATTR_NODUMP     0x000040

#define L_AT_EMPTY_PATH 0x1000
#define L_AT_FDCWD      (-100)

static void stat_to_statx(const struct stat *d, struct linux_statx *x)
{
    memset(x, 0, sizeof(*x));

    x->stx_mask = L_STATX_TYPE | L_STATX_MODE | L_STATX_NLINK | L_STATX_UID |
                  L_STATX_GID | L_STATX_ATIME | L_STATX_MTIME | L_STATX_CTIME |
                  L_STATX_INO | L_STATX_SIZE | L_STATX_BLOCKS | L_STATX_BTIME;

    x->stx_blksize = (uint32_t)d->st_blksize;
    x->stx_nlink   = (uint32_t)d->st_nlink;
    x->stx_uid     = (uint32_t)d->st_uid;
    x->stx_gid     = (uint32_t)d->st_gid;
    x->stx_mode    = (uint16_t)d->st_mode;   // S_IF* agree on both systems
    x->stx_ino     = (uint64_t)d->st_ino;
    x->stx_size    = (uint64_t)d->st_size;
    x->stx_blocks  = (uint64_t)d->st_blocks;

    x->stx_attributes_mask = L_STATX_ATTR_COMPRESSED | L_STATX_ATTR_IMMUTABLE |
                             L_STATX_ATTR_APPEND | L_STATX_ATTR_NODUMP;
    if (d->st_flags & UF_COMPRESSED)                 x->stx_attributes |= L_STATX_ATTR_COMPRESSED;
    if (d->st_flags & (UF_IMMUTABLE | SF_IMMUTABLE)) x->stx_attributes |= L_STATX_ATTR_IMMUTABLE;
    if (d->st_flags & (UF_APPEND | SF_APPEND))       x->stx_attributes |= L_STATX_ATTR_APPEND;
    if (d->st_flags & UF_NODUMP)                     x->stx_attributes |= L_STATX_ATTR_NODUMP;

    x->stx_atime.tv_sec  = d->st_atimespec.tv_sec;
    x->stx_atime.tv_nsec = (uint32_t)d->st_atimespec.tv_nsec;
    x->stx_mtime.tv_sec  = d->st_mtimespec.tv_sec;
    x->stx_mtime.tv_nsec = (uint32_t)d->st_mtimespec.tv_nsec;
    x->stx_ctime.tv_sec  = d->st_ctimespec.tv_sec;
    x->stx_ctime.tv_nsec = (uint32_t)d->st_ctimespec.tv_nsec;
    // Linux has no birth time in struct stat, which is why statx was added.
    // Darwin has had it since HFS+, so this is one field the guest gets a real
    // answer for.
    x->stx_btime.tv_sec  = d->st_birthtimespec.tv_sec;
    x->stx_btime.tv_nsec = (uint32_t)d->st_birthtimespec.tv_nsec;

    // dev_t is encoded differently on the two systems -- Linux splits major
    // across bits 8-19 and 32-63, Darwin puts it in bits 24-31 -- but statx
    // carries major and minor as separate integers, so the encoding never
    // crosses the boundary. major()/minor() are Darwin's decoders for a Darwin
    // dev_t, which is what these are.
    x->stx_rdev_major = (uint32_t)major(d->st_rdev);
    x->stx_rdev_minor = (uint32_t)minor(d->st_rdev);
    x->stx_dev_major  = (uint32_t)major(d->st_dev);
    x->stx_dev_minor  = (uint32_t)minor(d->st_dev);
}

long lxrt_statx(int ldirfd, const char *tpath, int lflags, unsigned mask,
                void *ubuf)
{
    (void)mask;   // see below: the answer is the same for every request mask
    if (!tpath)
        return LERR(EFAULT);
    if (!mem_ok(ubuf, sizeof(struct linux_statx), true))
        return LERR(EFAULT);

    struct stat d;
    int rc;
    if ((lflags & L_AT_EMPTY_PATH) && tpath[0] == '\0') {
        // statx(AT_FDCWD, "", AT_EMPTY_PATH) means "stat the working
        // directory", and Linux answers it with 0 (measured on the aarch64
        // guest). lxrt_dirfd_to_darwin(-100) hands back Darwin's AT_FDCWD, -2,
        // which is not a descriptor: fstat(-2) is EBADF, and this call was
        // measured returning -9 before the split below existed.
        if (ldirfd == L_AT_FDCWD)
            rc = stat(".", &d);
        else if (lxrt_pathfd_path(lxrt_dirfd_to_darwin(ldirfd)))
            rc = stat(lxrt_pathfd_path(lxrt_dirfd_to_darwin(ldirfd)), &d);
        else
            rc = fstat(lxrt_dirfd_to_darwin(ldirfd), &d);
    } else {
        rc = fstatat(lxrt_dirfd_to_darwin(ldirfd), tpath, &d,
                     lxrt_at_flags_to_darwin(lflags));
        // /proc/<pid>/fd/<n> -> socket:[N] (procpid.c).
        if (rc != 0 && !(lflags & 0x100) &&
            lxrt_magic_link_stat(lxrt_dirfd_to_darwin(ldirfd), tpath, &d))
            rc = 0;
    }
    if (rc != 0)
        return LERR(errno);
    // Android's property areas look root-owned, as bionic requires.
    lxrt_props_fix_stat(tpath[0] ? tpath : NULL,
                        tpath[0] ? -1 : lxrt_dirfd_to_darwin(ldirfd), &d);
    // Guests with Android ids: the owner a chown recorded (android_ids.h).
    if (tpath[0]) {
        lxrt_aids_fix_stat_at(lxrt_dirfd_to_darwin(ldirfd), tpath, (lflags & 0x100) != 0, &d);
    } else if (ldirfd == L_AT_FDCWD) {
        lxrt_aids_fix_stat(".", -1, false, &d);
    } else {
        int dfd = lxrt_dirfd_to_darwin(ldirfd);
        const char *pf = lxrt_pathfd_path(dfd);
        lxrt_aids_fix_stat(pf, pf ? -1 : dfd, false, &d);
    }

    // AT_STATX_FORCE_SYNC (0x2000) and AT_STATX_DONT_SYNC (0x4000) ask a
    // network filesystem how hard to work for a fresh answer. Darwin's stat has
    // no such control and always behaves as AT_STATX_SYNC_AS_STAT, which is the
    // default the flags depart from -- so they are ignored rather than refused.
    // Ignoring them costs staleness, never a wrong field.
    //
    // `mask` is ignored for the opposite reason: a single fstat produces every
    // field this code can produce, so narrowing the request would save nothing
    // and the returned stx_mask -- not the requested one -- is what tells the
    // guest what it got.
    if ((lflags & L_AT_EMPTY_PATH) && tpath[0] == '\0')
        lxrt_evdev_fix_stat(NULL, lxrt_dirfd_to_darwin(ldirfd), &d);
    else
        lxrt_evdev_fix_stat(tpath[0] == '/' ? tpath : NULL, -1, &d);
    struct linux_statx x;
    stat_to_statx(&d, &x);
    memcpy(ubuf, &x, sizeof x);
    return 0;
}

// ------------------------------------------------------------ 43/44 statfs
//
// The two structs share nothing but intent.
//
//   Linux (aarch64, 120 bytes, every field a signed 64-bit word)
//     0 f_type   8 f_bsize  16 f_blocks  24 f_bfree  32 f_bavail  40 f_files
//    48 f_ffree  56 f_fsid (2x s32)  64 f_namelen  72 f_frsize  80 f_flags
//    88 f_spare[4]
//
//   Darwin (2168 bytes, mixed widths, and the tail is three strings)
//     u32 f_bsize, s32 f_iosize, u64 f_blocks/f_bfree/f_bavail/f_files/f_ffree,
//     fsid_t f_fsid, uid_t f_owner, u32 f_type, u32 f_flags, u32 f_fssubtype,
//     char f_fstypename[16], f_mntonname[1024], f_mntfromname[1024], ...
//
// Darwin HAS a field called f_type, which is the trap: it is a VFS table index,
// not a Linux superblock magic, and it is small (single digits), so copying it
// across produces a number that looks like a valid answer and is not one.
#define L_ST_RDONLY      0x0001
#define L_ST_NOSUID      0x0002
#define L_ST_NODEV       0x0004
#define L_ST_NOEXEC      0x0008
#define L_ST_SYNCHRONOUS 0x0010
#define L_ST_NOATIME     0x0400

struct linux_statfs {
    int64_t f_type;
    int64_t f_bsize;
    int64_t f_blocks;
    int64_t f_bfree;
    int64_t f_bavail;
    int64_t f_files;
    int64_t f_ffree;
    int32_t f_fsid[2];
    int64_t f_namelen;
    int64_t f_frsize;
    int64_t f_flags;
    int64_t f_spare[4];
};
_Static_assert(sizeof(struct linux_statfs) == 120, "Linux struct statfs is 120 bytes");
_Static_assert(__builtin_offsetof(struct linux_statfs, f_namelen) == 64, "ABI offset");

// Linux superblock magics, from include/uapi/linux/magic.h. Guests branch on
// f_type to decide behaviour -- "is this tmpfs, can I put a memfd here", "is
// this NFS, avoid flock" -- so the number has to be one they recognise.
#define L_EXT4_SUPER_MAGIC  0x0000EF53
#define L_TMPFS_MAGIC       0x01021994
#define L_MSDOS_SUPER_MAGIC 0x00004D44
#define L_EXFAT_SUPER_MAGIC 0x2011BAB0
#define L_NFS_SUPER_MAGIC   0x00006969
#define L_SMB2_MAGIC        0xFE534D42
#define L_HFSPLUS_MAGIC     0x0000482B
#define L_AUTOFS_MAGIC      0x00000187

// APFS, the filesystem essentially everything here will actually be on, has no
// Linux magic because Linux cannot mount it. Reporting ext4 is a deliberate
// synthesis, not an oversight: it is the value that makes a guest take the
// ordinary-local-disk path, which is the path that is correct for APFS. The
// alternative -- an unknown magic -- sends well-written guests down their
// "assume nothing works" branch for a filesystem that supports everything they
// were checking for.
static int64_t fstype_magic(const char *name)
{
    if (!strcmp(name, "devfs"))  return L_TMPFS_MAGIC;
    if (!strcmp(name, "msdos"))  return L_MSDOS_SUPER_MAGIC;
    if (!strcmp(name, "exfat"))  return L_EXFAT_SUPER_MAGIC;
    if (!strcmp(name, "nfs"))    return L_NFS_SUPER_MAGIC;
    if (!strcmp(name, "smbfs"))  return L_SMB2_MAGIC;
    if (!strcmp(name, "hfs"))    return L_HFSPLUS_MAGIC;
    if (!strcmp(name, "autofs")) return L_AUTOFS_MAGIC;
    return L_EXT4_SUPER_MAGIC;   /* apfs and everything else */
}

static void statfs_to_linux(const struct statfs *d, struct linux_statfs *l)
{
    memset(l, 0, sizeof(*l));
    l->f_type   = fstype_magic(d->f_fstypename);
    l->f_bsize  = (int64_t)d->f_bsize;
    l->f_blocks = (int64_t)d->f_blocks;
    l->f_bfree  = (int64_t)d->f_bfree;
    l->f_bavail = (int64_t)d->f_bavail;
    l->f_files  = (int64_t)d->f_files;
    l->f_ffree  = (int64_t)d->f_ffree;
    // fsid_t is { int32_t val[2] } on Darwin and __kernel_fsid_t is
    // { int val[2] } on Linux. Same two words, same order.
    l->f_fsid[0] = d->f_fsid.val[0];
    l->f_fsid[1] = d->f_fsid.val[1];
    // Darwin has no per-filesystem name limit in statfs. NAME_MAX is 255 on
    // APFS, HFS+ and every filesystem macOS mounts natively; pathconf() would
    // give a per-path answer but needs a path, which fstatfs does not have.
    l->f_namelen = 255;
    // Linux f_frsize is the fragment size; no filesystem macOS supports has
    // fragments distinct from blocks, so it equals f_bsize -- which is also
    // what Linux reports for ext4 with default options.
    l->f_frsize = (int64_t)d->f_bsize;

    // MNT_* against Linux ST_*. Nothing but RDONLY shares a value.
    if (d->f_flags & MNT_RDONLY)      l->f_flags |= L_ST_RDONLY;       /* 1 -> 1 */
    if (d->f_flags & MNT_NOSUID)      l->f_flags |= L_ST_NOSUID;       /* 8 -> 2 */
    if (d->f_flags & MNT_NODEV)       l->f_flags |= L_ST_NODEV;        /* 0x10 -> 4 */
    if (d->f_flags & MNT_NOEXEC)      l->f_flags |= L_ST_NOEXEC;       /* 4 -> 8 */
    if (d->f_flags & MNT_SYNCHRONOUS) l->f_flags |= L_ST_SYNCHRONOUS;  /* 2 -> 0x10 */
    if (d->f_flags & MNT_NOATIME)     l->f_flags |= L_ST_NOATIME;      /* 0x10000000 -> 0x400 */
    // No Darwin source for ST_MANDLOCK, ST_NODIRATIME or ST_RELATIME; left
    // clear, which is the truthful answer for a filesystem that does not have
    // the concept.
}

// Free space of a disk-image volume (the Steam Frame root's and Android's
// case-sensitive sparsebundles). Inside, APFS answers from the image's own
// capacity: Steam in the Steam Frame root saw 22 GB free of a 40 GB image
// while the Mac had 219 GB. The image can only grow as far as the volume that
// holds it, so its free space is the smaller of the two:
//
//   LXRT_STATFS_BACKING=MOUNT=PATH[;MOUNT=PATH...]
//     for a filesystem mounted at MOUNT, free blocks are capped by those of
//     the filesystem holding PATH (scripts/image-volume.sh backing writes it)
// Bytes a volume's own files take (ATTR_VOL_SPACEUSED), without the free
// space statfs leaves out of a sparse image's capacity.
static bool volume_used_bytes(const char *mount, uint64_t *out)
{
    struct attrlist al = { .bitmapcount = ATTR_BIT_MAP_COUNT, .volattr = ATTR_VOL_INFO | ATTR_VOL_SPACEUSED };
    struct { uint32_t len; off_t used; } __attribute__((packed)) buf;
    if (getattrlist(mount, &al, &buf, sizeof buf, 0) != 0 || buf.used < 0)
        return false;
    *out = (uint64_t)buf.used;
    return true;
}

static void cap_by_backing(const struct statfs *d, struct linux_statfs *l)
{
    const char *e = getenv("LXRT_STATFS_BACKING");
    if (!e || !*e || !d->f_bsize)
        return;
    size_t mlen = strlen(d->f_mntonname);
    while (*e) {
        const char *eq = strchr(e, '='), *end = strchr(e, ';');
        if (!end)
            end = e + strlen(e);
        if (eq && eq < end && (size_t)(eq - e) == mlen && !memcmp(e, d->f_mntonname, mlen)) {
            char path[1024];
            size_t n = (size_t)(end - eq - 1);
            struct statfs b;
            if (n && n < sizeof path) {
                memcpy(path, eq + 1, n);
                path[n] = 0;
                if (statfs(path, &b) == 0 && b.f_bsize &&
                    (b.f_fsid.val[0] != d->f_fsid.val[0] || b.f_fsid.val[1] != d->f_fsid.val[1])) {
                    uint64_t avail = (uint64_t)b.f_bavail * b.f_bsize / d->f_bsize;
                    uint64_t bfree = (uint64_t)b.f_bfree * b.f_bsize / d->f_bsize;
                    if ((uint64_t)l->f_bavail > avail)
                        l->f_bavail = (int64_t)avail;
                    if ((uint64_t)l->f_bfree > bfree)
                        l->f_bfree = (int64_t)bfree;
                    // The size: what the volume holds plus what is free. APFS
                    // in a sparse image already answers free space from the
                    // Mac's disk but keeps the image's capacity as its size,
                    // so blocks - free came out as 1.5 TB "used" for 18 GB of
                    // data (MEASURED, df in the Steam Frame root).
                    uint64_t used;
                    if (volume_used_bytes(d->f_mntonname, &used))
                        l->f_blocks = (int64_t)(used / d->f_bsize + (uint64_t)l->f_bfree);
                }
            }
            return;
        }
        e = *end ? end + 1 : end;
    }
}

long lxrt_statfs(const char *tpath, void *ubuf)
{
    if (!tpath)
        return LERR(EFAULT);
    if (!mem_ok(ubuf, sizeof(struct linux_statfs), true))
        return LERR(EFAULT);
    struct statfs d;
    if (statfs(tpath, &d) != 0)
        return LERR(errno);
    struct linux_statfs l;
    statfs_to_linux(&d, &l);
    cap_by_backing(&d, &l);
    memcpy(ubuf, &l, sizeof l);
    return 0;
}

long lxrt_fstatfs(int fd, void *ubuf)
{
    if (!mem_ok(ubuf, sizeof(struct linux_statfs), true))
        return LERR(EFAULT);
    struct statfs d;
    if (fstatfs(fd, &d) != 0)
        return LERR(errno);
    struct linux_statfs l;
    statfs_to_linux(&d, &l);
    cap_by_backing(&d, &l);
    memcpy(ubuf, &l, sizeof l);
    return 0;
}

// ------------------------------------------------------------ 47 fallocate
//
// Linux fallocate guarantees the blocks exist, so a later write into the range
// cannot fail with ENOSPC. Darwin spells that fcntl(F_PREALLOCATE), whose
// F_PEOFPOSMODE form allocates a length PAST THE CURRENT EOF and ignores
// fst_offset entirely -- so the length asked for is (offset + len) minus the
// file's current size, not len.
//
//   FALLOC_FL_KEEP_SIZE      0x01   supported: skip the ftruncate
//   FALLOC_FL_PUNCH_HOLE     0x02   supported: fcntl(F_PUNCHHOLE)
//   FALLOC_FL_NO_HIDE_STALE  0x04   no Darwin equivalent
//   FALLOC_FL_COLLAPSE_RANGE 0x08   no Darwin equivalent
//   FALLOC_FL_ZERO_RANGE     0x10   no Darwin equivalent
//   FALLOC_FL_INSERT_RANGE   0x20   no Darwin equivalent
//   FALLOC_FL_UNSHARE_RANGE  0x40   no Darwin equivalent
//
// The four range-editing modes rewrite the file's extent map. Darwin exposes no
// fcntl for any of them and emulating them by copying would silently change the
// atomicity the guest is relying on, so they get Linux EOPNOTSUPP (spelled
// ENOTSUP -- see the note by LERR) -- which is what Linux itself returns on a
// filesystem that lacks them, so callers already have a fallback path for it.
#define L_FALLOC_FL_KEEP_SIZE  0x01
#define L_FALLOC_FL_PUNCH_HOLE 0x02

long lxrt_fallocate(int fd, int lmode, int64_t offset, int64_t len)
{
    if (offset < 0 || len <= 0)
        return LERR(EINVAL);
    if (offset > INT64_MAX - len)
        return LERR(EFBIG);

    if (lmode & L_FALLOC_FL_PUNCH_HOLE) {
        if (lmode & ~(L_FALLOC_FL_PUNCH_HOLE | L_FALLOC_FL_KEEP_SIZE))
            return LERR(ENOTSUP);
        // Linux requires KEEP_SIZE with PUNCH_HOLE and returns EINVAL without
        // it; punching a hole that also shortens the file is not expressible.
        if (!(lmode & L_FALLOC_FL_KEEP_SIZE))
            return LERR(EINVAL);
        fpunchhole_t fp;
        memset(&fp, 0, sizeof fp);
        fp.fp_offset = (off_t)offset;
        fp.fp_length = (off_t)len;
        return fcntl(fd, F_PUNCHHOLE, &fp) < 0 ? LERR(errno) : 0;
    }

    if (lmode & ~L_FALLOC_FL_KEEP_SIZE)
        return LERR(ENOTSUP);

    struct stat st;
    if (fstat(fd, &st) != 0)
        return LERR(errno);

    off_t want = (off_t)(offset + len);

    // THE PART OF THE RANGE THAT IS ALREADY INSIDE THE FILE.
    //
    // Darwin's F_PREALLOCATE only ever allocates PAST EOF (F_PEOFPOSMODE), and
    // macOS has no SEEK_HOLE, so a hole below the current size cannot be filled
    // and cannot even be located. Skipping straight to `return 0` for a range
    // inside the file therefore promised the guest something that was not true:
    // measured on a freshly ftruncate'd 1 MiB file, lxrt_fallocate(fd, 0, 0,
    // 1<<20) returned 0 with st_blocks 0 before AND 0 after. fallocate's whole
    // contract is that a later write into the range cannot take ENOSPC, and a
    // sparse file is precisely the case where the size is large and the blocks
    // are not there.
    //
    // The one thing that CAN be established cheaply is density: a file whose
    // allocated blocks already cover its size has no holes anywhere, so the
    // range is backed and 0 is the truth. Otherwise the request cannot be
    // honoured and EOPNOTSUPP is the answer (spelled ENOTSUP -- see the note by
    // LERR) -- what Linux itself returns on a filesystem that lacks fallocate,
    // so callers already carry a fallback for it, and the same refusal the
    // F_PREALLOCATE failure path below gives rather than softening to success.
    //
    // Residual: an APFS-compressed file reports st_blocks below st_size and is
    // refused even though it has no holes. A false ENOTSUP costs the guest its
    // fallback path; a false success costs it the ENOSPC it wrote code to avoid.
    if (offset < st.st_size) {
        off_t backed = (off_t)st.st_blocks * 512;
        if (backed < st.st_size)
            return LERR(ENOTSUP);
    }

    if (want > st.st_size) {
        fstore_t fst;
        memset(&fst, 0, sizeof fst);
        // F_ALLOCATEALL rather than F_ALLOCATECONTIG: fallocate promises the
        // space exists, not that it is one extent, and demanding contiguity
        // fails on a fragmented volume where the guest's requirement is met.
        fst.fst_flags = F_ALLOCATEALL;
        fst.fst_posmode = F_PEOFPOSMODE;
        fst.fst_offset = 0;                       // ignored in F_PEOFPOSMODE
        fst.fst_length = want - st.st_size;
        if (fcntl(fd, F_PREALLOCATE, &fst) < 0)
            // Not softened to success: a guest that called fallocate is about
            // to write into the range believing it cannot run out of space.
            return LERR(errno);
    }

    if (!(lmode & L_FALLOC_FL_KEEP_SIZE) && want > st.st_size)
        if (ftruncate(fd, want) != 0)
            return LERR(errno);
    return 0;
}

// ------------------------------------------------------------ 83 fdatasync
//
// Darwin has the syscall (SYS_fdatasync, 187) but declares it in no header, so
// calling it by name compiles only through an implicit declaration. The fcntl
// route is both declared and more accurate about the guarantee:
//
//   Linux fdatasync   data is on stable storage; metadata only as needed
//   Darwin fsync      data is handed to the drive, which may still cache it
//   F_BARRIERFSYNC    fsync plus a write barrier -- ordering is guaranteed
//   F_FULLFSYNC       fsync plus a full drive cache flush -- durable, and slow
//
// fsync (LNR_fsync) is answered with the plain Darwin fsync, and fdatasync
// now is too. It used F_BARRIERFSYNC, as the closest match to Linux's
// guarantee -- and on Linux fdatasync is the CHEAPER of the two, while here
// it was by far the dearer: on the disk image the guests' root lives on,
// with bulk writes going on (a Steam start), MEASURED 154 ms mean / 312 ms
// worst per F_BARRIERFSYNC against 8.7 / 17.7 ms per fsync (F_FULLFSYNC:
// 104 / 207 ms). Chromium's caches call fdatasync all through Steam's start,
// and the one web helper caught hung in the kernel (a start in about sixty
// with no main window, 2026-10-03) had a thread inside that fcntl and its
// main thread blocked in an openat. The residual gap is stated rather than
// hidden: data acknowledged here can still be lost to a power cut, where
// Linux's fdatasync would have kept it -- the same as this runtime's fsync.
// LXRT_FDATASYNC_BARRIER=1 brings the barrier back.
long lxrt_fdatasync(int fd)
{
    static int barrier = -1;
    if (barrier < 0)
        barrier = getenv("LXRT_FDATASYNC_BARRIER") && *getenv("LXRT_FDATASYNC_BARRIER") == '1';
    if (barrier) {
        if (fcntl(fd, F_BARRIERFSYNC) == 0)
            return 0;
        if (errno != ENOTTY && errno != ENOTSUP && errno != EINVAL)
            return LERR(errno);
    }
    return fsync(fd) != 0 ? LERR(errno) : 0;
}

// ------------------------------------------------------------ 37 linkat
//
//   flag                  Linux   Darwin
//   AT_SYMLINK_NOFOLLOW   0x0100  0x0020
//   AT_SYMLINK_FOLLOW     0x0400  0x0040
//   AT_EMPTY_PATH         0x1000  (none)
//
// fsflags.c's lxrt_at_flags_to_darwin already carries the first two. It is used
// here rather than repeating the mapping, so linkat cannot drift away from the
// other *at() calls.
long lxrt_linkat(int lolddirfd, const char *toldpath,
                 int lnewdirfd, const char *tnewpath, int lflags)
{
    if (!toldpath || !tnewpath)
        return LERR(EFAULT);
    // AT_SYMLINK_NOFOLLOW (0x0100) is NOT in the set. Nofollow is linkat's
    // default and is not expressible as a flag: Linux's sys_linkat accepts only
    // AT_SYMLINK_FOLLOW and AT_EMPTY_PATH and returns EINVAL for anything else
    // (measured on the aarch64 guest: errno 22 with both paths valid). Letting
    // it through here also handed lxrt_at_flags_to_darwin a bit it turns into
    // Darwin AT_SYMLINK_NOFOLLOW, which Darwin's linkat does not define either.
    if (lflags & ~(0x0400 | L_AT_EMPTY_PATH))
        return LERR(EINVAL);
    if (lflags & L_AT_EMPTY_PATH)
        // "link the file olddirfd refers to, ignoring the path". Darwin has no
        // link-by-descriptor. Linux itself needs CAP_DAC_READ_SEARCH for this
        // and returns ENOENT without it, so a guest that meets a refusal here
        // is on a path it already has to handle.
        return LERR(EPERM);

    return linkat(lxrt_dirfd_to_darwin(lolddirfd), toldpath,
                  lxrt_dirfd_to_darwin(lnewdirfd), tnewpath,
                  lxrt_at_flags_to_darwin(lflags)) != 0 ? LERR(errno) : 0;
}

// ------------------------------------------------------------ 439 faccessat2
//
// faccessat2 exists because Linux's faccessat never had a flags argument in the
// kernel -- glibc emulated AT_EACCESS in userspace. So this is the first call
// where the AT_* word actually reaches a kernel, and it is the one place where
// AT_EACCESS matters:
//
//   flag                  Linux   Darwin
//   AT_SYMLINK_NOFOLLOW   0x0100  0x0020
//   AT_EACCESS            0x0200  0x0010
//   AT_EMPTY_PATH         0x1000  (none)
//
// Linux AT_EACCESS is 0x200, which is also Linux AT_REMOVEDIR -- the two share
// a value and are told apart by which syscall they arrive at. That is why
// lxrt_at_flags_to_darwin does not translate it (it would have to guess) and
// why the mapping is done here instead.
//
//   mode: F_OK 0, X_OK 1, W_OK 2, R_OK 4 on both systems.
#define L_AT_SYMLINK_NOFOLLOW 0x0100
#define L_AT_EACCESS          0x0200

long lxrt_faccessat2(int ldirfd, const char *tpath, int mode, int lflags)
{
    if (!tpath)
        return LERR(EFAULT);
    if (mode & ~(R_OK | W_OK | X_OK))
        return LERR(EINVAL);
    if (lflags & ~(L_AT_SYMLINK_NOFOLLOW | L_AT_EACCESS | L_AT_EMPTY_PATH))
        return LERR(EINVAL);
    if (lflags & L_AT_EMPTY_PATH)
        // Test the dirfd itself. Darwin's faccessat has no equivalent, and
        // fstat cannot answer it: access() asks about the CALLER's permission,
        // which depends on the effective ids and on ACLs that st_mode does not
        // carry, so deriving it from a stat would be a guess dressed as a fact.
        return LERR(EINVAL);

    int dflags = 0;
    if (lflags & L_AT_SYMLINK_NOFOLLOW) dflags |= AT_SYMLINK_NOFOLLOW;
    if (lflags & L_AT_EACCESS)          dflags |= AT_EACCESS;

    return faccessat(lxrt_dirfd_to_darwin(ldirfd), tpath, mode, dflags) != 0
               ? LERR(errno) : 0;
}

// ------------------------------------------------------------ 269/243 mmsg
//
// struct mmsghdr is { struct msghdr msg_hdr; unsigned msg_len; }, where the
// msghdr is the LINUX one -- 56 bytes, laid out in socket.c. The element is
// therefore 64 bytes with msg_len at 56, and the per-message translation is
// socket.c's job: these loop over lxrt_sendmsg/lxrt_recvmsg one message at a
// time rather than growing a second copy of the msghdr/cmsghdr code.
//
// THE MSG_* NUMBERS DO NOT AGREE, and this is the seventh number space:
//
//   flag             Linux    Darwin
//   MSG_OOB           0x0001   0x0001
//   MSG_PEEK          0x0002   0x0002
//   MSG_DONTROUTE     0x0004   0x0004
//   MSG_CTRUNC        0x0008   0x0020
//   MSG_TRUNC         0x0020   0x0010
//   MSG_DONTWAIT      0x0040   0x0080
//   MSG_EOR           0x0080   0x0008
//   MSG_WAITALL       0x0100   0x0040
//   MSG_NOSIGNAL      0x4000   0x80000
//   MSG_WAITFORONE   0x10000   0x10000 is MSG_NEEDSA -- a different flag
//
// EOR/TRUNC/CTRUNC/WAITALL/DONTWAIT are five swapped pairs in the low byte, so
// a pass-through does not fail: MSG_DONTWAIT becomes MSG_WAITALL and a socket
// the guest asked not to block on blocks forever.
//
// lxrt_sendmsg/lxrt_recvmsg hand their flags word to Darwin unchanged, so what
// they want is a DARWIN flags word; the translation happens here and they get
// one. THE TABLE RUNS BOTH WAYS: recvmsg also REPORTS flags in msg_flags, and
// socket.c copies Darwin's straight back into the Linux msghdr, so the same
// five swapped pairs have to be undone on the way out -- see
// msg_flags_to_linux. (The single-message syscalls 211/212 go straight into
// socket.c without passing through here, so they still send Linux numbers to
// Darwin and still report Darwin numbers back. That is a bug in socket.c, not
// one to fix by translating twice -- noted here because this is where the table
// is written down.)
struct linux_mmsghdr {
    uint64_t msg_name;
    uint32_t msg_namelen;
    uint32_t pad_;
    uint64_t msg_iov;
    uint64_t msg_iovlen;
    uint64_t msg_control;
    uint64_t msg_controllen;
    int32_t  msg_flags;
    int32_t  pad2_;
    uint32_t msg_len;
    uint32_t pad3_;
};
_Static_assert(sizeof(struct linux_mmsghdr) == 64, "Linux struct mmsghdr is 64 bytes");
_Static_assert(__builtin_offsetof(struct linux_mmsghdr, msg_len) == 56,
               "msg_len sits right after the 56-byte Linux msghdr");

struct linux_cmsghdr { uint64_t cmsg_len; int32_t cmsg_level; int32_t cmsg_type; };

#define L_MSG_OOB          0x00000001
#define L_MSG_PEEK         0x00000002
#define L_MSG_DONTROUTE    0x00000004
#define L_MSG_CTRUNC       0x00000008
#define L_MSG_TRUNC        0x00000020
#define L_MSG_DONTWAIT     0x00000040
#define L_MSG_EOR          0x00000080
#define L_MSG_WAITALL      0x00000100
#define L_MSG_CONFIRM      0x00000800
#define L_MSG_ERRQUEUE     0x00002000
#define L_MSG_NOSIGNAL     0x00004000
#define L_MSG_MORE         0x00008000
#define L_MSG_WAITFORONE   0x00010000
#define L_MSG_FASTOPEN     0x20000000
#define L_MSG_CMSG_CLOEXEC 0x40000000

#define L_SOL_SOCKET 1
#define L_SCM_RIGHTS 1
#define L_UIO_MAXIOV 1024

// Returns the Darwin flags word, or -1 for a flag that cannot be honoured.
// MSG_WAITFORONE and MSG_CMSG_CLOEXEC are handled by the callers and must be
// stripped before getting here -- 0x10000 is MSG_NEEDSA on Darwin and 0x40000000
// is nothing at all.
static int msg_flags_to_darwin(int l)
{
    int d = 0;
    if (l & L_MSG_OOB)       d |= MSG_OOB;
    if (l & L_MSG_PEEK)      d |= MSG_PEEK;
    if (l & L_MSG_DONTROUTE) d |= MSG_DONTROUTE;
    if (l & L_MSG_CTRUNC)    d |= MSG_CTRUNC;
    if (l & L_MSG_TRUNC)     d |= MSG_TRUNC;
    if (l & L_MSG_DONTWAIT)  d |= MSG_DONTWAIT;
    if (l & L_MSG_EOR)       d |= MSG_EOR;
    if (l & L_MSG_WAITALL)   d |= MSG_WAITALL;
    if (l & L_MSG_NOSIGNAL)  d |= MSG_NOSIGNAL;

    // Dropped deliberately, and only these three. MSG_CONFIRM is an ARP
    // freshness hint, MSG_MORE is a packetisation hint (Linux's own doc calls
    // it advisory), and MSG_FASTOPEN only saves a round trip. Losing any of
    // them costs efficiency, never a different result.
    int benign = L_MSG_CONFIRM | L_MSG_MORE | L_MSG_FASTOPEN;
    int known = L_MSG_OOB | L_MSG_PEEK | L_MSG_DONTROUTE | L_MSG_CTRUNC |
                L_MSG_TRUNC | L_MSG_DONTWAIT | L_MSG_EOR | L_MSG_WAITALL |
                L_MSG_NOSIGNAL | benign;
    if (l & ~known)
        return -1;
    return d;
}

// THE OUTPUT DIRECTION OF THE SAME TABLE, and it was missing. recvmsg does not
// only take a flags word, it REPORTS one in msg_flags, and socket.c's
// lxrt_recvmsg copies Darwin's straight into the Linux msghdr
// (`lm->msg_flags = dm.msg_flags;`). Every swapped pair in the table above then
// runs backwards, and two of the three land on real Linux bits:
//
//   Darwin MSG_TRUNC  0x10 -> no Linux MSG_* bit at all: a truncated datagram
//                             reads as a clean receive and the guest silently
//                             loses the tail. Measured on an AF_UNIX SOCK_DGRAM
//                             socketpair: an 8-byte datagram into a 2-byte
//                             iovec came back msg_flags 0x10.
//   Darwin MSG_CTRUNC 0x20 -> Linux MSG_TRUNC 0x20: dropped SCM_RIGHTS
//                             descriptors reported as a truncated payload.
//   Darwin MSG_EOR     0x8 -> Linux MSG_CTRUNC 0x8: end-of-record reported as
//                             lost file descriptors.
//
// MSG_DONTWAIT/WAITALL/PEEK/OOB/DONTROUTE are never set on output by Darwin,
// but they are converted anyway so this stays the exact inverse of the function
// above and cannot drift away from it.
static int msg_flags_to_linux(int d)
{
    int l = 0;
    if (d & MSG_OOB)       l |= L_MSG_OOB;
    if (d & MSG_PEEK)      l |= L_MSG_PEEK;
    if (d & MSG_DONTROUTE) l |= L_MSG_DONTROUTE;
    if (d & MSG_CTRUNC)    l |= L_MSG_CTRUNC;
    if (d & MSG_TRUNC)     l |= L_MSG_TRUNC;
    if (d & MSG_DONTWAIT)  l |= L_MSG_DONTWAIT;
    if (d & MSG_EOR)       l |= L_MSG_EOR;
    if (d & MSG_WAITALL)   l |= L_MSG_WAITALL;
    if (d & MSG_NOSIGNAL)  l |= L_MSG_NOSIGNAL;
    return l;
}

// MEDIUM: the guest pointers INSIDE one element.
//
// mem_ok on the mmsghdr vector only says the 64-byte elements are mapped; it
// says nothing about the three guest pointers each one carries. socket.c then
// dereferences all of them IN USERSPACE -- cmsg_to_darwin reads msg_control,
// cmsg_to_linux WRITES to it, addr_to_linux writes to msg_name -- so a bad one
// is a host fault, not an EFAULT. Measured: a recvmmsg with msg_control set to
// an unmapped 0x4000000000 and a datagram actually carrying SCM_RIGHTS killed
// the harness with SIGBUS, which in the real runtime takes the guest and the
// graphics bridge down with it. That is exactly what mem_ok exists to prevent.
//
// `recv` selects write access for the two buffers the receive path writes back
// through, and also bounds msg_iovlen: socket.c truncates it into an `int` with
// no check, and Linux's import_iovec refuses more than UIO_MAXIOV segments with
// EMSGSIZE (90, checked on the aarch64 guest).
_Static_assert(sizeof(struct iovec) == 16, "Linux and Darwin iovec are both 16 bytes");

static long mmsg_elem_ok(const struct linux_mmsghdr *m, bool recv)
{
    if (m->msg_name && m->msg_namelen &&
        !mem_ok((const void *)(uintptr_t)m->msg_name, (size_t)m->msg_namelen,
                recv))
        return LERR(EFAULT);
    if (m->msg_iovlen > L_UIO_MAXIOV)
        return LERR(EMSGSIZE);
    if (m->msg_iov && m->msg_iovlen &&
        !mem_ok((const void *)(uintptr_t)m->msg_iov,
                (size_t)m->msg_iovlen * sizeof(struct iovec), false))
        return LERR(EFAULT);
    // Each iov_base is left to the kernel, which returns EFAULT for a bad one
    // without touching it -- unlike msg_control, which this process walks.
    if (m->msg_control && m->msg_controllen &&
        !mem_ok((const void *)(uintptr_t)m->msg_control,
                (size_t)m->msg_controllen, recv))
        return LERR(EFAULT);
    return 0;
}

// MSG_CMSG_CLOEXEC asks that descriptors arriving over SCM_RIGHTS be created
// with FD_CLOEXEC. Darwin has no such recvmsg flag, and dropping it is not
// harmless -- the point of the flag is that there is no window between the
// recvmsg and an exec in another thread. Setting it afterwards narrows the
// window instead of closing it, which is strictly better than ignoring the
// request, and the residual race is named here rather than hidden.
//
// Takes the buffer and the length mmsg_elem_ok validated, rather than re-reading
// msg_controllen: lxrt_recvmsg overwrites that field with the length it wrote,
// and walking to a length this function never probed is the same fault as not
// probing at all.
static void apply_cmsg_cloexec(uint64_t control, size_t len)
{
    if (!control || !len)
        return;
    const uint8_t *p = (const uint8_t *)(uintptr_t)control;
    size_t off = 0;
    while (off + sizeof(struct linux_cmsghdr) <= len) {
        struct linux_cmsghdr c;
        memcpy(&c, p + off, sizeof c);
        if (c.cmsg_len < sizeof c || c.cmsg_len > len - off)
            break;
        if (c.cmsg_level == L_SOL_SOCKET && c.cmsg_type == L_SCM_RIGHTS) {
            size_t payload = (size_t)c.cmsg_len - sizeof c;
            for (size_t i = 0; i + sizeof(int) <= payload; i += sizeof(int)) {
                int fd;
                memcpy(&fd, p + off + sizeof c + i, sizeof fd);
                if (fd >= 0)
                    (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
        }
        off += ((size_t)c.cmsg_len + 7u) & ~(size_t)7u;   // Linux aligns to 8
    }
}

long lxrt_sendmmsg(int fd, void *umsgvec, unsigned vlen, int lflags)
{
    if (vlen == 0)
        return 0;
    if (vlen > L_UIO_MAXIOV)
        vlen = L_UIO_MAXIOV;   // Linux clamps rather than failing
    if (!mem_ok(umsgvec, (size_t)vlen * sizeof(struct linux_mmsghdr), true))
        return LERR(EFAULT);
    // MSG_WAITFORONE is receive-only; Linux rejects it on send.
    if (lflags & (L_MSG_WAITFORONE | L_MSG_ERRQUEUE | L_MSG_CMSG_CLOEXEC))
        return LERR(EINVAL);
    int dflags = msg_flags_to_darwin(lflags);
    if (dflags < 0)
        return LERR(EINVAL);

    struct linux_mmsghdr *vec = umsgvec;
    unsigned sent = 0;
    for (unsigned i = 0; i < vlen; i++) {
        long bad = mmsg_elem_ok(&vec[i], false);
        if (bad != 0) {
            if (sent == 0)
                return bad;
            break;
        }
        long r = lxrt_sendmsg(fd, &vec[i], dflags);
        if (r < 0) {
            // Linux reports an error only when nothing went out; otherwise the
            // count is the answer and the error surfaces on the next call.
            if (sent == 0)
                return r;
            break;
        }
        vec[i].msg_len = (uint32_t)r;
        sent++;
    }
    return (long)sent;
}

// Linux __kernel_timespec: two signed 64-bit words on every architecture.
// Darwin's struct timespec is also { long tv_sec; long tv_nsec } on arm64, so
// the two agree here -- unlike struct timeval, whose tv_usec does not.
struct linux_timespec64 { int64_t tv_sec, tv_nsec; };

long lxrt_recvmmsg(int fd, void *umsgvec, unsigned vlen, int lflags,
                   const void *utimeout)
{
    if (vlen == 0)
        return 0;
    if (vlen > L_UIO_MAXIOV)
        vlen = L_UIO_MAXIOV;
    if (!mem_ok(umsgvec, (size_t)vlen * sizeof(struct linux_mmsghdr), true))
        return LERR(EFAULT);

    if (lflags & L_MSG_ERRQUEUE)
        // Read from the socket's error queue instead of its data queue. Darwin
        // has no error queue, so there is nothing in it -- and EAGAIN, "empty
        // for now", is precisely what Linux returns for an empty one. The
        // caller's existing handling for that is the right handling here.
        return LERR(EAGAIN);

    bool waitforone = (lflags & L_MSG_WAITFORONE) != 0;
    bool cloexec = (lflags & L_MSG_CMSG_CLOEXEC) != 0;
    int dflags = msg_flags_to_darwin(lflags & ~(L_MSG_WAITFORONE |
                                                L_MSG_CMSG_CLOEXEC));
    if (dflags < 0)
        return LERR(EINVAL);

    struct timespec deadline;
    bool have_deadline = false;
    if (utimeout) {
        if (!mem_ok(utimeout, sizeof(struct linux_timespec64), false))
            return LERR(EFAULT);
        struct linux_timespec64 t;
        memcpy(&t, utimeout, sizeof t);
        if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= 1000000000)
            return LERR(EINVAL);
        // The addition below is signed time_t arithmetic on a number the guest
        // chose. tv_sec near INT64_MAX overflows it -- undefined behaviour, and
        // in practice a deadline in the PAST, which turns a long timeout into
        // "stop after the first datagram": measured, tv_sec 0x7ffffffffffffff0
        // returned 1 of 2 queued datagrams. Clamped to a decade, which no
        // caller waiting on a socket can tell apart from infinite.
        const int64_t DECADE_SEC = 10 * 365 * 24 * 3600;
        if (t.tv_sec > DECADE_SEC)
            t.tv_sec = DECADE_SEC;
        // CLOCK_MONOTONIC, not the wall clock: a clock step during a receive
        // would otherwise expire or extend the timeout.
        if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
            return LERR(errno);
        deadline.tv_sec += (time_t)t.tv_sec;
        deadline.tv_nsec += (long)t.tv_nsec;
        if (deadline.tv_nsec >= 1000000000) {
            deadline.tv_nsec -= 1000000000;
            deadline.tv_sec += 1;
        }
        have_deadline = true;
    }

    struct linux_mmsghdr *vec = umsgvec;
    unsigned got = 0;
    for (unsigned i = 0; i < vlen; i++) {
        int f = dflags;
        // MSG_WAITFORONE means "block for the first, poll for the rest". The
        // bit itself must never reach Darwin: 0x10000 there is MSG_NEEDSA,
        // which fails a receive when an address cannot be allocated.
        if (waitforone && got > 0)
            f |= MSG_DONTWAIT;

        long bad = mmsg_elem_ok(&vec[i], true);
        if (bad != 0) {
            if (got == 0)
                return bad;
            break;
        }
        uint64_t ctl = vec[i].msg_control;
        uint64_t ctlcap = vec[i].msg_controllen;   // the length just probed

        long r = lxrt_recvmsg(fd, &vec[i], f);
        if (r < 0) {
            if (got == 0)
                return r;
            break;
        }
        // socket.c wrote Darwin's reported flags into the Linux msghdr. Put
        // them back into the guest's number space before it reads them.
        vec[i].msg_flags = msg_flags_to_linux(vec[i].msg_flags);
        vec[i].msg_len = (uint32_t)r;
        if (cloexec) {
            uint64_t n = vec[i].msg_controllen;    // narrowed by lxrt_recvmsg
            if (n > ctlcap)
                n = ctlcap;
            apply_cmsg_cloexec(ctl, (size_t)n);
        }
        got++;

        // Linux checks the timeout BETWEEN datagrams and never interrupts one
        // in progress; a blocking receive that outlasts the timeout still
        // returns its message. Matching that exactly, rather than trying to
        // bound each receive, is why the deadline is only tested here.
        if (have_deadline) {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
                break;
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
                break;
        }
    }
    return (long)got;
}

// ------------------------------------------------------------ scheduling
//
//   policy         Linux  Darwin
//   SCHED_OTHER        0       1
//   SCHED_FIFO         1       4
//   SCHED_RR           2       2
//   SCHED_BATCH        3   (none)
//   SCHED_IDLE         5   (none)
//   SCHED_DEADLINE     6   (none)
//
// SCHED_OTHER is 0 on Linux and 1 on Darwin, and 1 on Linux is SCHED_FIFO. A
// pass-through of the most common policy in existence therefore asks for
// realtime FIFO scheduling, which either fails with EPERM or -- worse, if the
// process has the entitlement -- succeeds and starves everything else in the
// process, including AppKit's main thread.
//
//   priority       Linux            Darwin (measured on this machine)
//   SCHED_OTHER    must be 0        15..47, default 31
//   SCHED_FIFO/RR  1..99            15..47
//
// The guest is kept entirely in Linux's number space: the get_priority_max/min
// calls answer with LINUX's range, because whatever they return is what the
// guest will hand back to sched_setscheduler, and the two have to agree.
#define L_SCHED_OTHER    0
#define L_SCHED_FIFO     1
#define L_SCHED_RR       2
#define L_SCHED_BATCH    3
#define L_SCHED_IDLE     5
#define L_SCHED_DEADLINE 6
// Linux ORs this into the policy argument. Darwin has nothing like it, but the
// guarantee is met anyway: process.c's fork() carries only the calling thread
// and its execve() re-executes the runtime, so no thread's scheduling
// parameters survive into a child in the first place.
#define L_SCHED_RESET_ON_FORK 0x40000000

struct linux_sched_param { int32_t sched_priority; };

// ---- where a guest nice value lives, and why it has to live here ----------
//
// Darwin has nowhere to keep one. setpriority(PRIO_DARWIN_THREAD, 0, n) was
// measured accepting -5, 0, 5 and 10, returning 0 every time, while
// getpriority(PRIO_DARWIN_THREAD, 0) read back 0 for all four: it is a boolean
// "throttled or not" (PRIO_DARWIN_BG is 4096), not a nice scale. Using it would
// be the silent success this file refuses everywhere else.
//
// The one per-thread knob Darwin does store and hand back exactly is the
// pthread scheduling priority -- measured writing 20, 28, 36 and 44 into the
// timesharing band and reading each back unchanged. But that band is 15..47,
// 33 slots for 40 nice values, so it cannot round-trip a nice on its own, and
// a realtime thread has no room for a nice in it at all.
//
// So the table is the authority and the band is the effect: getpriority returns
// what the guest set, and the value is pushed into the band whenever the thread
// is on a timesharing policy. That is also what Linux does -- nice is stored on
// a realtime task too and only starts to matter when it returns to SCHED_OTHER,
// which is why lxrt_sched_setscheduler re-applies it below.
//
// Keyed by pthread_t rather than by tid so that every spelling of the same
// thread -- 0, its guest tid, the guest's getpid() for the group leader --
// lands in one slot. thread.c evicts the entry when unregistering the thread,
// before its pthread_t can be recycled.
#define NICE_SLOTS 256
static struct { pthread_t th; int nice; bool used; } g_nice[NICE_SLOTS];
static pthread_mutex_t g_nice_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(fileops2_g_nice_lock, g_nice_lock)

void lxrt_fileops2_thread_exited(pthread_t th)
{
    pthread_mutex_lock(&g_nice_lock);
    for (int i = 0; i < NICE_SLOTS; i++)
        if (g_nice[i].used && pthread_equal(g_nice[i].th, th)) {
            g_nice[i].used = false;
            break;
        }
    pthread_mutex_unlock(&g_nice_lock);
}

static void nice_store(pthread_t th, int nice)
{
    pthread_mutex_lock(&g_nice_lock);
    int spare = -1;
    for (int i = 0; i < NICE_SLOTS; i++) {
        if (g_nice[i].used && pthread_equal(g_nice[i].th, th)) {
            g_nice[i].nice = nice;
            pthread_mutex_unlock(&g_nice_lock);
            return;
        }
        if (!g_nice[i].used && spare < 0)
            spare = i;
    }
    // A full table is not an error: the value still reaches the band below, and
    // getpriority falls back to reading the band. It costs rounding, not truth.
    if (spare >= 0) {
        g_nice[spare].th = th;
        g_nice[spare].nice = nice;
        g_nice[spare].used = true;
    }
    pthread_mutex_unlock(&g_nice_lock);
}

static bool nice_load(pthread_t th, int *out)
{
    pthread_mutex_lock(&g_nice_lock);
    bool found = false;
    for (int i = 0; i < NICE_SLOTS; i++)
        if (g_nice[i].used && pthread_equal(g_nice[i].th, th)) {
            *out = g_nice[i].nice;
            found = true;
            break;
        }
    pthread_mutex_unlock(&g_nice_lock);
    return found;
}

// Darwin's timesharing band, read rather than assumed: 15..47 with a nominal
// default of 31 on this machine. Two slots of headroom are required so neither
// half of the nice mapping divides by zero.
static bool timeshare_band(int *dmin, int *dmax)
{
    int lo = sched_get_priority_min(SCHED_OTHER);
    int hi = sched_get_priority_max(SCHED_OTHER);
    if (lo < 0 || hi < lo + 2)
        return false;
    *dmin = lo;
    *dmax = hi;
    return true;
}

// nice -20..19, where LOWER is stronger, onto dmin..dmax, where HIGHER is.
// nice 0 lands on the band's midpoint, which is the Darwin default.
static int nice_to_band(int nice, int dmin, int dmax)
{
    int ddef = dmin + (dmax - dmin) / 2;
    if (nice < 0)
        return ddef + ((-nice) * (dmax - ddef)) / 20;
    return ddef - (nice * (ddef - dmin)) / 19;
}

static int band_to_nice(int d, int dmin, int dmax)
{
    int ddef = dmin + (dmax - dmin) / 2;
    int n = d > ddef ? -(((d - ddef) * 20) / (dmax - ddef))
                     :  (((ddef - d) * 19) / (ddef - dmin));
    if (n < -20) n = -20;
    if (n > 19)  n = 19;
    return n;
}

// Returns the Darwin policy, or -1.
static int policy_to_darwin(int lpolicy)
{
    switch (lpolicy) {
    case L_SCHED_OTHER: return SCHED_OTHER;   // 0 -> 1
    case L_SCHED_FIFO:  return SCHED_FIFO;    // 1 -> 4
    case L_SCHED_RR:    return SCHED_RR;      // 2 -> 2
    // SCHED_BATCH is "CPU-bound, penalise wakeup latency" and SCHED_IDLE is
    // "run only when nothing else wants the CPU". Darwin has no scheduling
    // class for either; both collapse onto SCHED_OTHER, and SCHED_IDLE takes
    // the bottom of the band so the intent survives even though the class does
    // not. Stated because a guest setting SCHED_IDLE on a background thread
    // will get a thread that competes with its foreground ones.
    case L_SCHED_BATCH: return SCHED_OTHER;
    case L_SCHED_IDLE:  return SCHED_OTHER;
    default:            return -1;            // SCHED_DEADLINE and anything new
    }
}

static int policy_to_linux(int dpolicy)
{
    switch (dpolicy) {
    case SCHED_FIFO: return L_SCHED_FIFO;
    case SCHED_RR:   return L_SCHED_RR;
    default:         return L_SCHED_OTHER;
    }
}

// Linux 1..99 onto whatever Darwin reports for the policy, which is 15..47 on
// Apple Silicon but is read rather than assumed. `nice` is the thread's stored
// nice value and is used only for the timesharing policies, where Linux's
// sched_priority must be 0 and nice is the only thing that can order threads --
// so a thread moved back to SCHED_OTHER lands where its nice says, not on a
// fixed midpoint that would discard a setpriority the guest already made.
static int prio_to_darwin(int lpolicy, int lprio, int nice)
{
    int dpolicy = policy_to_darwin(lpolicy);
    int dmin = sched_get_priority_min(dpolicy);
    int dmax = sched_get_priority_max(dpolicy);
    if (dmin < 0 || dmax < dmin)
        return dmin < 0 ? 0 : dmin;

    if (lpolicy == L_SCHED_IDLE)
        return dmin;
    if (lpolicy != L_SCHED_FIFO && lpolicy != L_SCHED_RR) {
        if (dmax < dmin + 2)
            return dmin + (dmax - dmin) / 2;
        return nice_to_band(nice, dmin, dmax);
    }
    return dmin + ((lprio - 1) * (dmax - dmin)) / 98;
}

static int prio_to_linux(int dpolicy, int dprio)
{
    if (dpolicy != SCHED_FIFO && dpolicy != SCHED_RR)
        return 0;   // Linux requires sched_priority == 0 for SCHED_OTHER
    int dmin = sched_get_priority_min(dpolicy);
    int dmax = sched_get_priority_max(dpolicy);
    if (dmax <= dmin)
        return 1;
    int l = 1 + ((dprio - dmin) * 98) / (dmax - dmin);
    if (l < 1)  l = 1;
    if (l > 99) l = 99;
    return l;
}

// Linux's sched_* calls take a TID, not a PID, and 0 means the caller. The
// thread registry in thread.c is what makes targeting another guest thread
// possible; without it this would have to refuse everything but 0.
//
// ACCEPTED LIMITATION -- a window this file cannot close. lxrt_thread_lookup
// reads the pthread_t under thread.c's g_threads_lock and then releases it, so
// every pthread_setschedparam/pthread_getschedparam below runs against a handle
// that is only known to have been live a moment ago. A guest thread that exits
// in that window leaves a stale pthread_t going into a pthread_* call. Closing
// it needs thread.c to either perform the operation while holding the lock or
// hand back a reference the caller releases, and thread.c's API does neither;
// adding either one means editing thread.c, which this module must not do. The
// same window is why the nice table above is keyed by a handle that can in
// principle be recycled. Named here for the same reason the MSG_CMSG_CLOEXEC
// and F_BARRIERFSYNC residuals are named: a documented gap can be closed later,
// an undocumented one cannot.
static bool resolve_thread(int pid, pthread_t *out)
{
    if (pid == 0 || pid == lxrt_gettid()) {
        *out = pthread_self();
        return true;
    }
    if (pid < 0)
        return false;
    return lxrt_thread_lookup(pid, out);
}

long lxrt_sched_get_priority_max(int lpolicy)
{
    switch (lpolicy) {
    case L_SCHED_FIFO:
    case L_SCHED_RR:
        return 99;
    case L_SCHED_OTHER:
    case L_SCHED_BATCH:
    case L_SCHED_IDLE:
        return 0;
    default:
        return LERR(EINVAL);
    }
}

long lxrt_sched_get_priority_min(int lpolicy)
{
    switch (lpolicy) {
    case L_SCHED_FIFO:
    case L_SCHED_RR:
        return 1;
    case L_SCHED_OTHER:
    case L_SCHED_BATCH:
    case L_SCHED_IDLE:
        return 0;
    default:
        return LERR(EINVAL);
    }
}

long lxrt_sched_setscheduler(int pid, int lpolicy, const void *uparam)
{
    // Linux's do_sched_setscheduler runs `if (!param || pid < 0) return
    // -EINVAL;` BEFORE the copy_from_user and before the task lookup, so both
    // of these outrank the EFAULT and the ESRCH below. Measured on the aarch64
    // guest: sched_setscheduler(0, SCHED_OTHER, NULL) and
    // sched_setscheduler(-5, SCHED_OTHER, &param) are both errno 22, where this
    // returned -14 and -3.
    if (!uparam || pid < 0)
        return LERR(EINVAL);

    int policy = lpolicy & ~L_SCHED_RESET_ON_FORK;
    if (policy == L_SCHED_DEADLINE)
        // SCHED_DEADLINE needs sched_setattr and a runtime/deadline/period
        // triple; Darwin's nearest concept is a thread time-constraint policy
        // set through thread_policy_set, which pthread_setschedparam cannot
        // reach. Refused rather than downgraded to FIFO, because a deadline
        // task quietly becoming a FIFO task is a hang waiting to happen.
        return LERR(EINVAL);
    int dpolicy = policy_to_darwin(policy);
    if (dpolicy < 0)
        return LERR(EINVAL);
    if (!mem_ok(uparam, sizeof(struct linux_sched_param), false))
        return LERR(EFAULT);

    struct linux_sched_param p;
    memcpy(&p, uparam, sizeof p);

    // Order matters: Linux's do_sched_setscheduler looks the task up first and
    // returns ESRCH, then validates the priority. Checking the priority first
    // reports EINVAL for a thread that does not exist, which sends a caller
    // hunting through its priority arithmetic for a bug that is not there.
    pthread_t t;
    if (!resolve_thread(pid, &t))
        return LERR(ESRCH);

    long lo = lxrt_sched_get_priority_min(policy);
    long hi = lxrt_sched_get_priority_max(policy);
    if (p.sched_priority < lo || p.sched_priority > hi)
        return LERR(EINVAL);

    // Linux keeps a task's nice across a policy change and starts honouring it
    // again the moment the task is timesharing. Carry that: the nice the guest
    // set through setpriority is what decides where in Darwin's band this
    // thread lands, instead of a fixed midpoint that would throw it away.
    int nice = 0;
    (void)nice_load(t, &nice);

    struct sched_param dp;
    memset(&dp, 0, sizeof dp);
    dp.sched_priority = prio_to_darwin(policy, p.sched_priority, nice);

    // pthread_setschedparam returns an errno rather than setting one.
    int rc = pthread_setschedparam(t, dpolicy, &dp);
    return rc != 0 ? LERR(rc) : 0;
}

long lxrt_sched_getscheduler(int pid)
{
    // Linux: `if (pid < 0) return -EINVAL;` before the lookup, so a negative
    // pid is EINVAL and not the ESRCH resolve_thread would otherwise produce.
    if (pid < 0)
        return LERR(EINVAL);
    pthread_t t;
    if (!resolve_thread(pid, &t))
        return LERR(ESRCH);
    int dpolicy = 0;
    struct sched_param dp;
    memset(&dp, 0, sizeof dp);
    int rc = pthread_getschedparam(t, &dpolicy, &dp);
    if (rc != 0)
        return LERR(rc);
    return policy_to_linux(dpolicy);
}

long lxrt_sched_getparam(int pid, void *uparam)
{
    // Linux's sys_sched_getparam checks !param and pid < 0 first, then looks
    // the task up (ESRCH), and only faults on the copy_to_user at the very end
    // -- the opposite order from sched_setscheduler, whose copy_from_user comes
    // before the lookup. Both are reproduced as the kernel has them.
    if (!uparam || pid < 0)
        return LERR(EINVAL);
    pthread_t t;
    if (!resolve_thread(pid, &t))
        return LERR(ESRCH);
    if (!mem_ok(uparam, sizeof(struct linux_sched_param), true))
        return LERR(EFAULT);
    int dpolicy = 0;
    struct sched_param dp;
    memset(&dp, 0, sizeof dp);
    int rc = pthread_getschedparam(t, &dpolicy, &dp);
    if (rc != 0)
        return LERR(rc);
    struct linux_sched_param p = { prio_to_linux(dpolicy, dp.sched_priority) };
    memcpy(uparam, &p, sizeof p);
    return 0;
}

// ------------------------------------------------------------ 140/141 priority
//
// PRIO_PROCESS/PRIO_PGRP/PRIO_USER are 0/1/2 on both. Darwin adds
// PRIO_DARWIN_THREAD(3) and PRIO_DARWIN_PROCESS(4); a Linux guest passing 3 or
// 4 means nothing by it, so they are refused rather than passed through into a
// Darwin-only behaviour.
//
// THE DIVERGENCE IS IN THE RETURN VALUE, not the arguments. getpriority(2) can
// legitimately return -1, which a syscall return cannot distinguish from an
// error, so Linux's SYSCALL returns 20 - nice -- a positive number in 1..40 --
// and glibc subtracts the bias back off. Darwin's kernel returns the raw nice
// value and relies on errno. Passing Darwin's answer through means every nice
// level is reported 20 too low: a thread at nice 0 comes back as nice 20.
//
// setpriority takes the raw nice value on both; only the read is biased.
//
// AND `who` IS A GUEST NUMBER, which is the part that was wrong and the part
// that escaped the sandbox. Under PRIO_PROCESS a Linux guest passes a TID --
// nice is per-task on Linux, and every sched_* entry point above already reads
// the argument that way. thread.c hands out guest tids as getpid()+1, +2, ...,
// synthetic numbers that are not this process's threads but very often ARE live
// pids of unrelated processes owned by the same user. Handing one to Darwin's
// setpriority/getpriority reads and writes THAT process instead.
//
// Measured, and it is not theoretical: with an unrelated child at pid 96357
// niced to 7, lxrt_getpriority(PRIO_PROCESS, 96357) returned 13 -- 20 minus the
// stranger's nice, reported to the guest as its own thread's -- and
// lxrt_setpriority(PRIO_PROCESS, 96357, 19) reniced that process to 19. Same
// uid, so Darwin permits it, and a guest calling setpriority on one of its own
// tids deprioritises a random process on the user's Mac.
//
// So PRIO_PROCESS now goes through the same registry as the rest of the file
// and a tid that is not a registered guest thread is ESRCH. Only the two
// `which` values whose namespace really is shared with the host still reach
// Darwin's setpriority.
#define L_PRIO_PROCESS 0
#define L_PRIO_PGRP    1
#define L_PRIO_USER    2

// PRIO_PROCESS `who`: 0 is the caller, the guest's own getpid() is the thread
// group -- Linux's PRIO_PROCESS with a tgid addresses the leader's task, and
// dispatch.c answers the guest's getpid() with this process's -- and anything
// else is a guest tid that must be in thread.c's registry. Never a host pid.
//
// (thread.c starts tids at getpid()+1, so the group leader's tid is not equal
// to the pid the way it is on Linux. Both spellings resolve to the same
// pthread_t here, which is why the nice table is keyed by the handle.)
static bool resolve_prio_thread(int who, pthread_t *out)
{
    (void)lxrt_gettid();   // Register the caller before resolving the leader.
    if (who == 0) {
        *out = pthread_self();
        return true;
    }
    if (who == lxrt_ids_pid())
        return lxrt_main_guest_thread(out);
    if (who < 0)
        return false;
    return lxrt_thread_lookup(who, out);
}

// Pushes a nice value into the thread's Darwin band. The value itself has
// already been recorded by the caller; this is only the effect.
static long apply_nice(pthread_t t, int nice)
{
    int pol = 0;
    struct sched_param dp;
    memset(&dp, 0, sizeof dp);
    int rc = pthread_getschedparam(t, &pol, &dp);
    if (rc != 0)
        return LERR(rc);
    if (pol == SCHED_FIFO || pol == SCHED_RR)
        // Not a refusal and not a pretence: Linux stores a nice on a realtime
        // task and it changes nothing until the task returns to a timesharing
        // policy, at which point lxrt_sched_setscheduler reads it back out of
        // the table and applies it. Overwriting the realtime priority with a
        // nice-derived one here would silently demote the thread instead.
        return 0;
    int dmin, dmax;
    if (!timeshare_band(&dmin, &dmax))
        return LERR(ENOTSUP);
    dp.sched_priority = nice_to_band(nice, dmin, dmax);
    // pthread_setschedparam returns an errno rather than setting one.
    rc = pthread_setschedparam(t, pol, &dp);
    return rc != 0 ? LERR(rc) : 0;
}

long lxrt_setpriority(int which, int who, int lprio)
{
    // Linux clamps out-of-range values instead of failing.
    if (lprio < -20) lprio = -20;
    if (lprio > 19)  lprio = 19;

    switch (which) {
    case L_PRIO_PROCESS: {
        pthread_t t;
        if (!resolve_prio_thread(who, &t))
            return LERR(ESRCH);
        long rc = apply_nice(t, lprio);
        if (rc != 0)
            return rc;   // record nothing getpriority would then report back
        nice_store(t, lprio);
        return 0;
    }
    case L_PRIO_USER:
        // The one namespace that really is shared: the guest runs as this
        // process's uid, so a uid means the same thing on both sides and
        // Darwin's setpriority is the right call with the guest's own number.
        return setpriority(PRIO_USER, (id_t)who, lprio) != 0 ? LERR(errno) : 0;
    case L_PRIO_PGRP:
        // A guest pgid is not a host pgid. Only this process's own group -- and
        // 0, which names it -- has any meaning; any other number addresses an
        // unrelated job on the user's Mac and is refused. Residual, stated
        // because it cannot be fixed from here: the host group may also hold
        // processes the guest never created, such as the shell that launched
        // the runtime, and they are reniced along with it.
        if (who != 0 && who != (int)getpgrp())
            return LERR(ESRCH);
        return setpriority(PRIO_PGRP, (id_t)who, lprio) != 0 ? LERR(errno) : 0;
    default:
        // PRIO_DARWIN_THREAD(3) and PRIO_DARWIN_PROCESS(4) mean nothing to a
        // Linux guest, so they are refused rather than passed through.
        return LERR(EINVAL);
    }
}

long lxrt_getpriority(int which, int who)
{
    switch (which) {
    case L_PRIO_PROCESS: {
        pthread_t t;
        if (!resolve_prio_thread(who, &t))
            return LERR(ESRCH);
        int n;
        if (!nice_load(t, &n)) {
            // Never niced through this interface. Read the band back instead of
            // inventing a number: a thread placed by sched_setscheduler still
            // has a real position, and a realtime thread has no nice at all --
            // Linux's default of 0 is the only true statement about it.
            int pol = 0;
            struct sched_param dp;
            memset(&dp, 0, sizeof dp);
            int rc = pthread_getschedparam(t, &pol, &dp);
            if (rc != 0)
                return LERR(rc);
            int dmin, dmax;
            if (pol == SCHED_FIFO || pol == SCHED_RR ||
                !timeshare_band(&dmin, &dmax))
                n = 0;
            else
                n = band_to_nice(dp.sched_priority, dmin, dmax);
        }
        return 20 - n;
    }
    case L_PRIO_USER:
    case L_PRIO_PGRP: {
        if (which == L_PRIO_PGRP && who != 0 && who != (int)getpgrp())
            return LERR(ESRCH);
        // -1 is a valid nice value, so errno is the only way to tell a failure
        // apart from a legitimate answer; it has to be cleared first.
        errno = 0;
        int n = getpriority(which == L_PRIO_USER ? PRIO_USER : PRIO_PGRP,
                            (id_t)who);
        if (n == -1 && errno != 0)
            return LERR(errno);
        return 20 - n;
    }
    default:
        return LERR(EINVAL);
    }
}

// ------------------------------------------------------------ 147..150 creds
//
// Numbers checked against include/uapi/asm-generic/unistd.h: setresuid is 147,
// getresuid 148, setresgid 149, getresgid 150. (151 is setfsuid.)
//
// Darwin has no setresuid/getresuid at any level -- not a syscall, not a libc
// function. What it has is the BSD triple behind setreuid(): a real, an
// effective and a saved id, where the saved one is not directly settable and
// follows the effective one whenever the real one changes.
//
// The saved id IS readable, through sysctl(KERN_PROC), which is how getresuid
// gives a real answer instead of repeating the effective id and hoping.

// Reads the process's real and saved ids from the kernel. Returns false if the
// sysctl fails, in which case the caller must not invent a saved id.
static bool read_saved_ids(uid_t *svuid, gid_t *svgid)
{
    struct kinfo_proc kp;
    memset(&kp, 0, sizeof kp);
    size_t len = sizeof kp;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid() };
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0)
        return false;
    if (len < sizeof kp) {
        errno = EIO;    // sysctl succeeded but answered short; do not reuse a
        return false;   // stale errno from whatever the caller did last
    }
    if (svuid) *svuid = kp.kp_eproc.e_pcred.p_svuid;
    if (svgid) *svgid = kp.kp_eproc.e_pcred.p_svgid;
    return true;
}

long lxrt_getresuid(void *uruid, void *ueuid, void *usuid)
{
    if (!mem_ok(uruid, sizeof(uint32_t), true) ||
        !mem_ok(ueuid, sizeof(uint32_t), true) ||
        !mem_ok(usuid, sizeof(uint32_t), true))
        return LERR(EFAULT);
    uid_t sv = 0;
    if (!read_saved_ids(&sv, NULL))
        // No fallback to geteuid(): a wrong saved-uid is what a setuid helper
        // uses to decide whether it can regain privilege, and a confident wrong
        // answer there is worse than a failed call.
        return LERR(errno ? errno : EIO);
    uint32_t r = (uint32_t)getuid(), e = (uint32_t)geteuid(), s = (uint32_t)sv;
    memcpy(uruid, &r, sizeof r);
    memcpy(ueuid, &e, sizeof e);
    memcpy(usuid, &s, sizeof s);
    return 0;
}

long lxrt_getresgid(void *urgid, void *uegid, void *usgid)
{
    if (!mem_ok(urgid, sizeof(uint32_t), true) ||
        !mem_ok(uegid, sizeof(uint32_t), true) ||
        !mem_ok(usgid, sizeof(uint32_t), true))
        return LERR(EFAULT);
    gid_t sv = 0;
    if (!read_saved_ids(NULL, &sv))
        return LERR(errno ? errno : EIO);
    uint32_t r = (uint32_t)getgid(), e = (uint32_t)getegid(), s = (uint32_t)sv;
    memcpy(urgid, &r, sizeof r);
    memcpy(uegid, &e, sizeof e);
    memcpy(usgid, &s, sizeof s);
    return 0;
}

// setresuid is expressible only when setreuid happens to land the saved id
// where the guest asked for it, so the precheck has to model XNU's rule EXACTLY
// -- it is the difference between refusing up front and discovering afterwards
// that the credentials have changed and cannot be put back.
//
// XNU's setreuid (bsd/kern/kern_prot.c):
//
//     if (my_pcred->cr_svuid != uap->ruid &&
//         my_pcred->cr_svuid != uap->euid)
//             svuid = new_euid;
//
// Three things in that the old guard got wrong. The comparison is against the
// RAW arguments, (uid_t)-1 included, not against the resolved ids. The saved id
// is overwritten unless it already equals one of them. And "leave the saved id
// alone" is therefore NOT always expressible -- which is precisely the case the
// old test `want_s != want_e && want_s != cur_sv` waved through.
//
// The canonical privilege drop is the one it let through: a root process with
// ruid = euid = svuid = 0 calling setresuid(1000, 1000, 0) to drop to 1000
// while keeping 0 in the saved slot so it can come back. want_s 0 == cur_sv 0,
// so the old guard allowed it; then svuid != 1000 and svuid != 1000, so
// setreuid(1000, 1000) set svuid to 1000 and the guest's later setresuid(0,0,0)
// fails forever. That outcome is exactly what this function exists to prevent,
// so it is predicted with the kernel's own arithmetic and refused.
long lxrt_setresuid(int32_t ruid, int32_t euid, int32_t suid)
{
    if (ruid == -1 && euid == -1 && suid == -1)
        return 0;
    uid_t cur_sv = 0;
    if (!read_saved_ids(&cur_sv, NULL))
        return LERR(errno ? errno : EIO);
    uid_t raw_r = (uid_t)ruid;   /* -1 stays (uid_t)-1, as the kernel sees it */
    uid_t raw_e = (uid_t)euid;
    uid_t want_e = euid == -1 ? geteuid() : (uid_t)euid;
    uid_t want_s = suid == -1 ? cur_sv : (uid_t)suid;
    uid_t after_sv = (cur_sv != raw_r && cur_sv != raw_e) ? want_e : cur_sv;
    if (after_sv != want_s)
        return LERR(EPERM);
    if (setreuid(ruid == -1 ? (uid_t)-1 : (uid_t)ruid,
                 euid == -1 ? (uid_t)-1 : (uid_t)euid) != 0)
        return LERR(errno);
    return 0;
}

// setregid carries the identical rule over cr_svgid; the two kernel functions
// are the same code with the id type changed, so the model is too.
long lxrt_setresgid(int32_t rgid, int32_t egid, int32_t sgid)
{
    if (rgid == -1 && egid == -1 && sgid == -1)
        return 0;
    gid_t cur_sv = 0;
    if (!read_saved_ids(NULL, &cur_sv))
        return LERR(errno ? errno : EIO);
    gid_t raw_r = (gid_t)rgid;
    gid_t raw_e = (gid_t)egid;
    gid_t want_e = egid == -1 ? getegid() : (gid_t)egid;
    gid_t want_s = sgid == -1 ? cur_sv : (gid_t)sgid;
    gid_t after_sv = (cur_sv != raw_r && cur_sv != raw_e) ? want_e : cur_sv;
    if (after_sv != want_s)
        return LERR(EPERM);
    if (setregid(rgid == -1 ? (gid_t)-1 : (gid_t)rgid,
                 egid == -1 ? (gid_t)-1 : (gid_t)egid) != 0)
        return LERR(errno);
    return 0;
}
