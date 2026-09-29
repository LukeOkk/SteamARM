// The machine-description half of the synthetic /proc.
//
// procfs.c answers the questions only the runtime can answer -- where the
// executable is, what the address space looks like, which fds are open. This
// answers the questions the *kernel* would answer: how much memory the machine
// has, what the CPU is, how long it has been up, what the sysctl knobs are set
// to. Roughly 40 distinct /proc paths appear in the Steam binaries; the ones
// here are the subset that is both load-bearing and honestly generable from
// Darwin.
//
// Two rules govern everything below.
//
// The first is that the CONTENT must be Linux-shaped, not macOS-shaped. These
// files are read by Linux parsers that were never written to be defensive:
// glibc's sysconf(_SC_PHYS_PAGES) wants `MemTotal: N kB`, SDL and Steam's
// hardware survey want an aarch64 /proc/cpuinfo with `CPU implementer` and
// `Features`, and srt-bwrap wants an 11-field /proc/self/mountinfo. Emitting
// `sysctl -a` output in a /proc file is worse than emitting nothing, because
// nothing at least produces ENOENT.
//
// The second is that the NUMBERS must be measured. Every figure here that can
// be read out of Darwin is read out of Darwin -- hw.memsize, hw.logicalcpu,
// hw.tbfrequency, vm_statistics64, host_cpu_load_info, kern.boottime,
// vm.swapusage, task_info. Only the values Darwin genuinely does not have are
// constants, and each of those carries a comment saying so.
//
// Not provided here, deliberately:
//
//   /proc/self/maps       procfs.c already builds it from the real address
//                         space. Two generators for one file is how they drift.
//   /proc/self/ns/*       Linux namespaces. Darwin has none -- not a weaker
//                         version, none. A synthetic ns/ directory would make
//                         pressure-vessel believe unshare(CLONE_NEWNS) can
//                         work, and it takes the container path on exactly
//                         that evidence. ENOENT sends it down the fallback.
//   /proc/<other pid>/*   Reading another process needs task_for_pid(), which
//                         needs root or com.apple.security.cs.debugger, and
//                         what it would describe is a Darwin process anyway.
//   /proc/kallsyms,
//   /proc/modules         There is no kernel to enumerate.
//   /proc/self/smaps      Per-mapping Linux accounting (Pss, Private_Dirty)
//                         with no Darwin counterpart at any granularity.
//   /proc/self/oom_*      Writable control files. Everything this module
//                         publishes is a 0444 regular file, which is right for
//                         the files Linux also makes read-only to an
//                         unprivileged process, and wrong for these: a write
//                         to oom_score_adj is meant to SUCCEED, so it would
//                         fail with EACCES and read back the old value.

#include "lxrt.h"
#include "proc_ext.h"

#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <mach/mach_host.h>
#include <mach/mach_vm.h>
#include <mach/processor_info.h>
#include <mach/vm_page_size.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#define LERR(e) (-lxrt_errno_to_linux(e))

// One generated file, before it is written out. 10 cores of cpuinfo is 2.3 KB;
// a 128-core machine would be 30 KB. Heap, not static, because two guest
// threads can be in here at once.
#define GEN_MAX (128 * 1024)

// Two different page sizes are in play here, and conflating them is the bug
// this pair of accessors exists to prevent.
//
// guest_page_bytes() is the unit the GUEST divides by. Linux's
// /proc/self/stat and /proc/self/statm report memory in PAGES, the guest
// converts with sysconf(_SC_PAGESIZE), that comes from AT_PAGESZ, and
// AT_PAGESZ is whatever stack.c published -- LXRT_HOST_PAGE, lxrt.h:18. Every
// page COUNT in this file has to be in those units or the guest's own
// arithmetic turns it into the wrong number of bytes.
//
// host_page_bytes() is the unit Darwin's counters arrive in. vm_statistics64's
// fields and task_info's resident_size are measured in vm_kernel_page_size.
// Measured on Apple Silicon: getpagesize(), vm_kernel_page_size and
// host_page_size() all agree at 16384, and
// free+active+inactive+wire+speculative+compressor came to 1018436 against
// hw.memsize/16384 = 1048576, the shortfall being the firmware carveout.
//
// Today the two are the same number, so nothing observable depends on the
// distinction. They are split anyway because guest and host page sizes need
// not agree: x86 guests under FEX use 4 KiB pages inside the 16 KiB host pages
// (runtime/subpage.c), while the host page itself cannot be 4 KiB on Apple
// Silicon Darwin (LXRT_HOST_PAGE is 16384, lxrt.h:19; "the host page cannot be
// subdivided", subpage.c:12). Were the guest granule ever to differ from the
// host's, a single page_bytes() reading vm_kernel_page_size would put statm,
// stat's rss, vmstat's nr_* and the auxv fallback's AT_PAGESZ 4x away from the
// guest's own sysconf(_SC_PAGESIZE), silently. Converting instead of warning
// is the fix: the numbers stay right on either granule, so there is nothing
// left to warn about, and a per-read fprintf into the guest's stderr would be
// worse than the divergence it announced.
static uint64_t guest_page_bytes(void)
{
    extern uint64_t lxrt_guest_page(void);
    return lxrt_guest_page();
}

static uint64_t host_page_bytes(void)
{
    return (uint64_t)vm_kernel_page_size;
}

// A Darwin page count restated in the page unit the guest will divide by.
static uint64_t host_pages_to_guest(uint64_t pages)
{
    uint64_t gp = guest_page_bytes();
    if (!gp)
        return pages;
    return (pages * host_page_bytes()) / gp;
}

// ------------------------------------------------------------ string buffer

// The `trunc` flag is not decoration. sbf() caps at cap-1 and returns, which
// on a machine with enough cores used to hand /proc/cpuinfo out with its last
// record cut off mid-line -- a file no parser rejects and every parser
// misreads. Truncation is now recorded, sb_done() turns it into GEN_TRUNC, and
// materialise() grows the buffer and retries rather than publishing the stump.
struct sb { char *p; size_t cap, len; bool trunc; };

// Returned by a generator instead of a length when the content did not fit.
#define GEN_TRUNC ((size_t)-1)

static void sbf(struct sb *s, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

static void sbf(struct sb *s, const char *fmt, ...)
{
    size_t room = s->cap > s->len ? s->cap - s->len : 0;
    if (room < 2) {
        s->trunc = true;
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->p + s->len, room, fmt, ap);
    va_end(ap);
    if (n < 0) {
        s->trunc = true;
        return;
    }
    if ((size_t)n < room) {
        s->len += (size_t)n;
    } else {
        s->len += room - 1;
        s->trunc = true;
    }
}

static size_t sb_done(const struct sb *s)
{
    return s->trunc ? GEN_TRUNC : s->len;
}

// Linux prints meminfo as a 16-column label followed by an 8-wide count and
// the literal " kB". The kB is binary: the kernel shifts pages by
// (PAGE_SHIFT - 10), so it is 1024 bytes, not 1000. A parser that divides by
// 1000 to get megabytes is off by 2.4%, which is why the unit is spelled out
// here rather than left to the call sites.
static void mem_kb(struct sb *s, const char *label, uint64_t bytes)
{
    sbf(s, "%-15s %8llu kB\n", label, (unsigned long long)(bytes / 1024));
}

// ------------------------------------------------------------ Darwin probes

static uint64_t sctl_u64(const char *name, uint64_t dflt)
{
    // Darwin sysctls are not all the same width: hw.memsize is 8 bytes,
    // kern.maxfilesperproc is 4 (both measured). Asking for 8 on a 4-byte node
    // succeeds, writes 4 and reports 4, so the buffer is zeroed first and the
    // returned length decides how much of it is real. Reading the low half is
    // correct only because arm64 macOS is little-endian, which it always is.
    uint64_t v = 0;
    size_t len = sizeof v;
    if (sysctlbyname(name, &v, &len, NULL, 0) != 0)
        return dflt;
    if (len == 4)
        return v & 0xffffffffull;
    if (len != 8)
        return dflt;
    return v;
}

// mach_host_self() hands out a send right and every call adds a reference --
// unlike mach_task_self(), which is a cached special port and needs no
// deallocate. These generators run on every /proc read, so the port is taken
// once and kept rather than leaked a few times per guest open().
static mach_port_t host_port(void)
{
    static _Atomic mach_port_t cached;
    mach_port_t p = atomic_load(&cached);
    if (p != MACH_PORT_NULL)
        return p;
    mach_port_t fresh = mach_host_self();
    mach_port_t expect = MACH_PORT_NULL;
    if (atomic_compare_exchange_strong(&cached, &expect, fresh))
        return fresh;
    mach_port_deallocate(mach_task_self(), fresh);
    return expect;
}

// HOST_VM_INFO64_COUNT is the count of the revision the SDK was built against
// (REV6, 104 natural_t, on this one). host_statistics64 CLAMPS *count down to
// whatever the running kernel actually supports and leaves the rest of the
// caller's struct untouched, so on an older kernel the tail -- which is where
// compressor_page_count, external_page_count and internal_page_count live,
// all REV1 -- would be read uninitialised. Zeroing first turns that into a
// reported 0 instead of a stack leak into a guest-readable file. Measured on
// this machine: the kernel returns the full 104, so nothing in use is
// currently short.
static bool host_vm_stats(vm_statistics64_data_t *out)
{
    memset(out, 0, sizeof *out);
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    return host_statistics64(host_port(), HOST_VM_INFO64,
                             (host_info64_t)out, &count) == KERN_SUCCESS;
}

// Aggregate CPU time. Darwin's cpu_ticks are already in 100 Hz units, the same
// USER_HZ Linux uses in /proc/stat -- checked rather than assumed: on a
// 10-core machine up 228469 s the ticks summed to 227607055, and
// 227607055 / 10 / 100 = 227607 s, a 0.4% match. CPU_STATE_* ordering is
// USER, SYSTEM, IDLE, NICE, which is NOT Linux's field order
// (user, nice, system, idle), so the two are never emitted positionally.
static bool cpu_load(host_cpu_load_info_data_t *out)
{
    mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
    return host_statistics(host_port(), HOST_CPU_LOAD_INFO,
                           (host_info_t)out, &count) == KERN_SUCCESS;
}

static uint64_t boot_time_sec(void)
{
    struct timeval bt;
    size_t len = sizeof bt;
    int mib[2] = { CTL_KERN, KERN_BOOTTIME };
    if (sysctl(mib, 2, &bt, &len, NULL, 0) != 0)
        return 0;
    return (uint64_t)bt.tv_sec;
}

// Linux's /proc/uptime is CLOCK_BOOTTIME: it keeps counting across suspend.
// mach_absolute_time() does not -- it stops when the Mac sleeps -- so the lid
// being shut for an hour would make uptime jump backwards relative to wall
// clock. The kern.boottime delta is the one that behaves like Linux.
static double uptime_sec(void)
{
    uint64_t bt = boot_time_sec();
    if (!bt)
        return 0.0;
    struct timeval now;
    gettimeofday(&now, NULL);
    double s = (double)now.tv_sec - (double)bt + (double)now.tv_usec / 1e6;
    return s > 0.0 ? s : 0.0;
}

static bool task_basic(mach_task_basic_info_data_t *out)
{
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    return task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                     (task_info_t)out, &count) == KERN_SUCCESS;
}

// MACH_TASK_BASIC_INFO's user_time and system_time are NOT the process's CPU
// time. The SDK header says so on the line the fields are declared:
// "total user run time for TERMINATED threads" (mach/task_info.h). Live
// threads are accounted in TASK_THREAD_TIMES_INFO, and the process total is
// the sum of the two -- that is exactly how ps(1) and getrusage() arrive at
// theirs. Measured with this split: a process that burned 0.3745 s of user CPU
// (getrusage ground truth) reported basic=0 ticks, thread=37 ticks, sum=37,
// against the 37.45 ticks getrusage implies. Reading only the basic half is
// what made /proc/self/stat fields 14 and 15 permanently 0, which tells any
// guest profiler, times(3) caller or Steam overlay that this process has used
// no CPU since it started -- plausibly wrong rather than visibly wrong, the
// one failure mode this file exists to keep out.
static bool task_thread_times(task_thread_times_info_data_t *out)
{
    mach_msg_type_number_t count = TASK_THREAD_TIMES_INFO_COUNT;
    return task_info(mach_task_self(), TASK_THREAD_TIMES_INFO,
                     (task_info_t)out, &count) == KERN_SUCCESS;
}

static bool task_events(task_events_info_data_t *out)
{
    mach_msg_type_number_t count = TASK_EVENTS_INFO_COUNT;
    return task_info(mach_task_self(), TASK_EVENTS_INFO,
                     (task_info_t)out, &count) == KERN_SUCCESS;
}

// TASK_EVENTS_INFO counts faults, pageins and context switches in integer_t --
// SIGNED 32-bit -- while the Linux fields they feed (/proc/self/stat minflt
// and majflt, /proc/stat ctxt) are unsigned long. A process that outlives
// 2^31 faults, which Steam does inside a session, wraps the Darwin counter
// negative, and a straight cast to uint64_t turns -1 into 18446744073709551615
// in a file the guest is parsing. Clamping is the only honest option: the true
// count is unrecoverable once the kernel has wrapped it.
static uint64_t nonneg(integer_t v)
{
    return v > 0 ? (uint64_t)v : 0;
}

static unsigned thread_count(void)
{
    thread_act_array_t list = NULL;
    mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS)
        return 1;
    for (mach_msg_type_number_t i = 0; i < n; i++)
        mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list,
                  n * sizeof(thread_act_t));
    return n ? (unsigned)n : 1u;
}

// Darwin's task virtual_size is not a Linux VmSize and reporting it raw is a
// trap. Measured on an idle process here: virtual_size was 500512030720
// (500.5 GB), because every Darwin process carries the shared-region
// reservation. Dropping the regions that are SM_EMPTY (address space reserved
// with nothing behind it) brings it to 61.3 GB, and dropping the ones tagged
// VM_MEMORY_SHARED_PMAP (32 -- the dyld shared cache window, also unbacked,
// pages_resident == 0) brings it to 0.85 GB, which is a figure a Linux tool
// can use. Anything comparing vsize against MemTotal gets a sane answer only
// after both exclusions.
#define VM_TAG_SHARED_PMAP 32

static uint64_t virtual_size_linuxish(void)
{
    mach_vm_address_t addr = 0;
    uint64_t total = 0;
    for (int guard = 0; guard < 65536; guard++) {
        mach_vm_size_t size = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
        natural_t depth = 0;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info,
                                   &count) != KERN_SUCCESS)
            break;
        if (info.share_mode != SM_EMPTY && info.user_tag != VM_TAG_SHARED_PMAP)
            total += (uint64_t)size;
        addr += size;
    }
    return total;
}

// A guest pointer, or a runtime pointer built from one, is never dereferenced
// directly: mach_vm_read_overwrite reports a bad source address as
// KERN_INVALID_ADDRESS instead of raising SIGSEGV in the host. Verified: the
// call against address 0 returns 1 (KERN_INVALID_ADDRESS) rather than faulting.
static bool safe_copy_in(void *dst, const void *src, size_t n)
{
    if (!src || !n)
        return false;
    mach_vm_size_t got = 0;
    if (mach_vm_read_overwrite(mach_task_self(), (mach_vm_address_t)src,
                               (mach_vm_size_t)n, (mach_vm_address_t)dst,
                               &got) != KERN_SUCCESS)
        return false;
    return got == n;
}

// ------------------------------------------------------------ file plumbing

// Written through a temporary and renamed. /proc reads on Linux are atomic
// snapshots; a plain O_TRUNC rewrite would let a second guest thread open the
// file while it is half-written and parse a truncated meminfo. rename(2) on
// the same filesystem is atomic, so a reader sees either the previous
// generation or the new one.
//
// The return value carries the whole point of the no-caching design. This used
// to return void and every caller ignored it, so a failed generation -- EMFILE
// under fd pressure, ENOSPC on TMPDIR, a sandbox revoking write on it -- left
// the PREVIOUS generation on disk and translate() handed the guest a path to
// it with no error at all. Reproduced: generate /proc/uptime, chmod 0500 the
// directory, wait, ask again -- same path, same frozen "229691.11". That is
// precisely the first-answer-is-the-only-answer bug this module claims to have
// eliminated, so a failure now unlinks the stale final as well as the temp and
// says so, and translate() turns that into an honest ENOENT.
//
// Mode 0444, not 0644. On Linux /proc/meminfo is 0444 and the /proc/sys knobs
// are root-owned, so an unprivileged write gets EACCES. At 0644 the guest's
// open(O_WRONLY|O_TRUNC) SUCCEEDED on meminfo, self/stat, overcommit_memory,
// ptrace_scope and max_user_watches and then changed nothing -- silently faked
// success on control files, which is strictly worse than the wrong-errno
// outcome the header rejects for /proc/self/oom_*. Regeneration is unaffected:
// every write goes to a fresh temp file and renames over the old one rather
// than reopening it, and unlink/rename are governed by the directory's mode,
// not the file's.
static bool write_atomic(const char *dir, const char *rel,
                         const void *data, size_t len)
{
    static atomic_uint seq;
    char final[1024], tmp[1100];
    snprintf(final, sizeof final, "%s/%s", dir, rel);
    extern bool lxrt_file_same(const char *, const void *, size_t);
    if (lxrt_file_same(final, data, len))
        return true;            // unchanged: no filesystem event (procfs.c)
    snprintf(tmp, sizeof tmp, "%s.tmp%u.%u", final, (unsigned)getpid(),
             atomic_fetch_add(&seq, 1u));

    // Accepted limitation in the unlink-on-failure paths: if one thread's
    // generation fails while another's has just succeeded, the failing thread
    // removes the fresh file and the second thread's reader gets ENOENT on a
    // path that was valid a microsecond ago. That is a lost read under a
    // condition that is already an error (EMFILE, ENOSPC, a revoked TMPDIR),
    // and the next lookup regenerates. The alternative -- leaving the old
    // generation in place so a directory walker can still open it -- trades a
    // rare spurious ENOENT for a silent stale answer, which is the trade this
    // whole module refuses to make.
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0444);
    if (fd < 0) {
        unlink(final);
        return false;
    }
    const char *p = data;
    size_t left = len;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w <= 0)
            break;
        p += w;
        left -= (size_t)w;
    }
    bool wrote_all = (left == 0);
    if (close(fd) != 0)
        wrote_all = false;              // a deferred ENOSPC lands on close()
    if (!wrote_all || rename(tmp, final) != 0) {
        unlink(tmp);
        unlink(final);
        return false;
    }
    return true;
}

// mkdir -p, for the /proc/sys tree. Nothing here needs to fail loudly: a
// directory that cannot be created simply means the open() the guest is about
// to do returns ENOENT, which is the same answer it would get from a kernel
// built without that sysctl.
static void mkdirs(const char *dir, const char *rel)
{
    char path[1024];
    size_t n = (size_t)snprintf(path, sizeof path, "%s/%s", dir, rel);
    if (n >= sizeof path)
        return;
    for (size_t i = strlen(dir) + 1; i < n; i++) {
        if (path[i] != '/')
            continue;
        path[i] = '\0';
        mkdir(path, 0755);
        path[i] = '/';
    }
    mkdir(path, 0755);
}

// ------------------------------------------------------------ /proc/meminfo

static size_t gen_meminfo(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    vm_statistics64_data_t vm;
    bool have = host_vm_stats(&vm);
    // The vm_statistics64 counters are in the HOST granule; these figures are
    // emitted in bytes, so the host unit is the right one throughout.
    uint64_t ps = host_page_bytes();
    uint64_t total = sctl_u64("hw.memsize", 0);

    // Speculative pages are SUBTRACTED, not added. mach/vm_statistics.h:114
    // carries an explicit NB three lines above the field -- "speculative pages
    // are already accounted for in free_count" -- and vm_stat(1) and Activity
    // Monitor both print free_count minus speculative_count. Measured live:
    // free_count=14554, speculative=709, vm_stat printed "Pages free: 13775";
    // the old free+speculative came to 15263. glibc's
    // sysconf(_SC_AVPHYS_PAGES) reads MemFree, so the overstatement (21 MB
    // idle, hundreds of MB under file-cache pressure) lands in the guest's own
    // allocator sizing.
    uint64_t freep = 0;
    if (have)
        freep = (uint64_t)vm.free_count > (uint64_t)vm.speculative_count
                    ? (uint64_t)vm.free_count - (uint64_t)vm.speculative_count
                    : 0;
    uint64_t active = have ? (uint64_t)vm.active_count : 0;
    uint64_t inactive = have ? (uint64_t)vm.inactive_count : 0;
    uint64_t wired = have ? (uint64_t)vm.wire_count : 0;
    uint64_t purge = have ? (uint64_t)vm.purgeable_count : 0;
    uint64_t ext = have ? (uint64_t)vm.external_page_count : 0;
    uint64_t internal = have ? (uint64_t)vm.internal_page_count : 0;
    uint64_t compressed = have ? (uint64_t)vm.compressor_page_count : 0;

    // MemAvailable is "what a new allocation could get without swapping". On
    // Darwin that is free plus the reclaimable half: inactive, purgeable and
    // the clean file cache. Clamped, because those three overlap -- external
    // pages can also be inactive -- and a MemAvailable above MemTotal makes
    // every consumer's percentage arithmetic nonsense.
    uint64_t avail = (freep + inactive + purge + ext) * ps;
    if (avail > total)
        avail = total;

    // vm.swapusage is a struct, not a scalar, and the encrypted-swap flag on
    // the end means the length check matters.
    struct xsw_usage xsu;
    memset(&xsu, 0, sizeof xsu);
    size_t xl = sizeof xsu;
    if (sysctlbyname("vm.swapusage", &xsu, &xl, NULL, 0) != 0 ||
        xl != sizeof xsu) {
        xsu.xsu_total = 0;
        xsu.xsu_avail = 0;
    }

    mem_kb(&s, "MemTotal:", total);
    mem_kb(&s, "MemFree:", freep * ps);
    mem_kb(&s, "MemAvailable:", avail);
    // Darwin has one unified cache; there is no separate buffer cache to
    // report, and Linux itself has reported a near-zero Buffers since the
    // page cache was unified in 2.4.
    mem_kb(&s, "Buffers:", 0);
    mem_kb(&s, "Cached:", ext * ps);
    mem_kb(&s, "SwapCached:", 0);
    mem_kb(&s, "Active:", active * ps);
    mem_kb(&s, "Inactive:", inactive * ps);
    // Active(anon)/Active(file) and their inactive twins are omitted, not
    // zeroed. Darwin splits pages by age (active/inactive) and, separately, by
    // backing (internal/external), but never publishes the cross product.
    // Zeros would read as "no anonymous memory is active", which is a
    // statement; absence reads as "this kernel does not export that", which is
    // true, and Linux itself only grew those fields in 2.6.28 so parsers
    // tolerate their absence.
    mem_kb(&s, "Unevictable:", wired * ps);
    mem_kb(&s, "Mlocked:", 0);
    mem_kb(&s, "SwapTotal:", (uint64_t)xsu.xsu_total);
    mem_kb(&s, "SwapFree:", (uint64_t)xsu.xsu_avail);
    mem_kb(&s, "Dirty:", 0);
    mem_kb(&s, "Writeback:", 0);
    mem_kb(&s, "AnonPages:", internal * ps);
    mem_kb(&s, "Mapped:", ext * ps);
    mem_kb(&s, "Shmem:", 0);
    mem_kb(&s, "KReclaimable:", 0);
    mem_kb(&s, "Slab:", 0);
    mem_kb(&s, "SReclaimable:", 0);
    mem_kb(&s, "SUnreclaim:", 0);
    mem_kb(&s, "KernelStack:", 0);
    mem_kb(&s, "PageTables:", 0);
    // overcommit_memory is reported as 0 (heuristic), and under mode 0 Linux's
    // CommitLimit is advisory; RAM + swap is the figure that matches the
    // behaviour Darwin actually has, which is to overcommit freely.
    mem_kb(&s, "CommitLimit:", total + (uint64_t)xsu.xsu_total);
    // Compressed pages are charged here rather than being counted as free:
    // they are live anonymous memory that happens to be squeezed, and Linux
    // has no equivalent state to put them in.
    mem_kb(&s, "Committed_AS:", (internal + compressed) * ps);
    // A constant on Linux too -- the size of the vmalloc arena, not a
    // measurement, and nothing in Steam or pressure-vessel parses it. The
    // figure here is the one MEASURED on this project's own aarch64 guest
    // (6.17.1-300.fc43.aarch64, 48-bit VA): VmallocTotal: 135288315904 kB. The
    // value this line used to print, 34359738367, is the x86_64 answer and was
    // asserted in the comment as "what x86_64 and arm64 both print", which the
    // guest disproves. An arm64 /proc must not carry an x86_64 constant with a
    // verification claim attached to it.
    sbf(&s, "%-15s %8llu kB\n", "VmallocTotal:", 135288315904ull);
    mem_kb(&s, "VmallocUsed:", 0);
    mem_kb(&s, "VmallocChunk:", 0);
    // Hugepage counts print WITHOUT the kB suffix on Linux; only the size does.
    sbf(&s, "HugePages_Total:   %5d\n", 0);
    sbf(&s, "HugePages_Free:    %5d\n", 0);
    sbf(&s, "HugePages_Rsvd:    %5d\n", 0);
    sbf(&s, "HugePages_Surp:    %5d\n", 0);
    // 2048 kB is the x86_64 and 4K-page arm64 answer (confirmed on the
    // project's guest, which runs a 4 KiB granule and prints exactly that),
    // and it is wrong here. A level-2 block covers granule * (granule / 8)
    // bytes, because an 8-byte descriptor fills one table: 4096 * 512 = 2 MiB
    // on a 4 KiB granule, 16384 * 2048 = 32 MiB on the 16 KiB one Darwin uses.
    // Derived from the GUEST granule, not the host's, because the thing it
    // must not contradict is the AT_PAGESZ this same process published.
    uint64_t gps = guest_page_bytes();
    sbf(&s, "%-15s %8llu kB\n", "Hugepagesize:",
        (unsigned long long)((gps * (gps / 8)) / 1024));
    mem_kb(&s, "Hugetlb:", 0);
    return sb_done(&s);
}

// ------------------------------------------------------------ /proc/cpuinfo

// The CPU identity is taken from the same synthetic MIDR_EL1 that sysreg.c
// hands back for `mrs Xt, MIDR_EL1` (0xd5380000: op0=3 op1=0 CRn=0 CRm=0
// op2=0, which is the encoding that switch matches). Deriving it here instead
// of hardcoding means /proc/cpuinfo and the register the guest can read on any
// thread cannot disagree -- and they would, silently, the first time either is
// edited.
//
// A consequence worth stating: Apple Silicon is heterogeneous (measured here,
// hw.perflevel0.logicalcpu = 4 performance cores and
// hw.perflevel1.logicalcpu = 6 efficiency cores) and real Linux on such a
// machine prints two different `CPU part` values. This prints one, because
// there is only one synthetic MIDR and a guest that read the register on an
// E-core would otherwise catch the file lying.
#define MIDR_MRS_INSN 0xd5380000u

static size_t gen_cpuinfo(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    uint64_t midr = lxrt_synthetic_sysreg(MIDR_MRS_INSN);
    unsigned implementer = (unsigned)((midr >> 24) & 0xff);
    unsigned variant     = (unsigned)((midr >> 20) & 0xf);
    unsigned partnum     = (unsigned)((midr >> 4) & 0xfff);
    unsigned revision    = (unsigned)(midr & 0xf);

    // arm64 Linux derives BogoMIPS from the architected timer, not from a
    // delay loop: bogomips = arch_timer_rate / 500000. Apple's timer is
    // 24 MHz (hw.tbfrequency, measured), which gives the 48.00 that Asahi
    // prints on the same silicon.
    uint64_t tb = sctl_u64("hw.tbfrequency", 24000000);
    uint64_t bogo_int = tb / 500000;
    uint64_t bogo_frac = (tb % 500000) * 100 / 500000;

    // The clamp and the buffer used to disagree: each record below measures
    // ~203 bytes, so 1024 processors need ~208 KB against a GEN_MAX of 128 KB,
    // and above ~645 the file was published with its last record cut off
    // mid-line. The clamp stays at 1024 -- it is a sanity bound on a sysctl,
    // not a capacity statement -- and materialise() now grows the buffer until
    // the generator stops reporting truncation, so the two cannot drift apart
    // again.
    uint64_t ncpu = sctl_u64("hw.logicalcpu", sctl_u64("hw.ncpu", 1));
    if (ncpu < 1)
        ncpu = 1;
    if (ncpu > 1024)
        ncpu = 1024;

    for (uint64_t i = 0; i < ncpu; i++) {
        sbf(&s, "processor\t: %llu\n", (unsigned long long)i);
        sbf(&s, "BogoMIPS\t: %llu.%02llu\n", (unsigned long long)bogo_int,
            (unsigned long long)bogo_frac);
        // This list is not the hardware's full capability set: it is exactly
        // the HWCAP bits stack.c puts in AT_HWCAP. glibc reads one and Steam's
        // hardware survey reads the other, and a capability named here but
        // absent from AT_HWCAP is how an ifunc gets selected for an
        // instruction the runtime never agreed to support. In particular
        // `cpuid` is absent on purpose -- see the comment in stack.c: claiming
        // it made glibc execute `mrs x0, midr_el1` and take SIGILL.
        sbf(&s, "Features\t: fp asimd aes pmull sha1 sha2 crc32 atomics"
                " asimdrdm\n");
        sbf(&s, "CPU implementer\t: 0x%02x\n", implementer);
        // Printed with no tab and always 8: the arm64 kernel hardcodes this
        // line, because MIDR's architecture field reads 0xf ("consult the ID
        // registers") on every ARMv8 part and would be useless here.
        sbf(&s, "CPU architecture: 8\n");
        sbf(&s, "CPU variant\t: 0x%x\n", variant);
        sbf(&s, "CPU part\t: 0x%03x\n", partnum);
        sbf(&s, "CPU revision\t: %u\n", revision);
        sbf(&s, "\n");
    }
    return sb_done(&s);
}

// --------------------------------------------------------------- /proc/stat

static size_t gen_stat(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    host_cpu_load_info_data_t cl;
    bool have = cpu_load(&cl);
    unsigned long long u = have ? cl.cpu_ticks[CPU_STATE_USER] : 0;
    unsigned long long sy = have ? cl.cpu_ticks[CPU_STATE_SYSTEM] : 0;
    unsigned long long id = have ? cl.cpu_ticks[CPU_STATE_IDLE] : 0;
    unsigned long long ni = have ? cl.cpu_ticks[CPU_STATE_NICE] : 0;

    // Two spaces after the aggregate "cpu", one after "cpuN" -- Linux's own
    // format, and awk '$1=="cpu"' scripts depend on the field not running into
    // the first number. iowait, irq, softirq, steal, guest and guest_nice are
    // zero because Darwin folds all of them into system time; there is no
    // breakdown to recover.
    sbf(&s, "cpu  %llu %llu %llu %llu 0 0 0 0 0 0\n", u, ni, sy, id);

    natural_t ncpu = 0;
    processor_info_array_t pinfo = NULL;
    mach_msg_type_number_t pcount = 0;
    if (host_processor_info(host_port(), PROCESSOR_CPU_LOAD_INFO,
                            &ncpu, &pinfo, &pcount) == KERN_SUCCESS) {
        processor_cpu_load_info_t p = (processor_cpu_load_info_t)pinfo;
        for (natural_t i = 0; i < ncpu; i++)
            sbf(&s, "cpu%u %u %u %u %u 0 0 0 0 0 0\n", i,
                p[i].cpu_ticks[CPU_STATE_USER], p[i].cpu_ticks[CPU_STATE_NICE],
                p[i].cpu_ticks[CPU_STATE_SYSTEM], p[i].cpu_ticks[CPU_STATE_IDLE]);
        vm_deallocate(mach_task_self(), (vm_address_t)pinfo,
                      pcount * sizeof(integer_t));
    }

    sbf(&s, "intr 0\n");
    // Darwin publishes no system-wide context-switch counter to unprivileged
    // userspace; TASK_EVENTS_INFO gives this task's. Emitting that rather than
    // a frozen 0 keeps the value monotonic, which is all anything reading
    // /proc/stat's ctxt actually does with it (a rate). It is a lower bound on
    // the system figure, and labelled as such here so nobody later reads it as
    // machine-wide.
    task_events_info_data_t te;
    sbf(&s, "ctxt %llu\n",
        task_events(&te) ? (unsigned long long)nonneg(te.csw) : 0ull);
    sbf(&s, "btime %llu\n", (unsigned long long)boot_time_sec());
    // Forks since boot. No Darwin equivalent at all -- not approximable, so 0
    // rather than something invented. Anything computing a fork rate gets zero,
    // which is visibly wrong rather than plausibly wrong.
    sbf(&s, "processes 0\n");
    sbf(&s, "procs_running 1\n");
    sbf(&s, "procs_blocked 0\n");
    sbf(&s, "softirq 0\n");
    return sb_done(&s);
}

// ------------------------------------------------------------- /proc/vmstat

static size_t gen_vmstat(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    vm_statistics64_data_t vm;
    host_vm_stats(&vm);                 // zeroes the struct on failure too
    uint64_t ps = host_page_bytes();

    // nr_* are page counts that the guest converts with sysconf(_SC_PAGESIZE),
    // i.e. AT_PAGESZ, so they are restated in the guest's granule rather than
    // Darwin's. Same speculative correction as meminfo: speculative pages are
    // already inside free_count (mach/vm_statistics.h:114), so adding them
    // overstated nr_free_pages by the whole speculative cache.
    uint64_t freep = (uint64_t)vm.free_count > (uint64_t)vm.speculative_count
                         ? (uint64_t)vm.free_count - (uint64_t)vm.speculative_count
                         : 0;
    sbf(&s, "nr_free_pages %llu\n",
        (unsigned long long)host_pages_to_guest(freep));
    sbf(&s, "nr_file_pages %llu\n",
        (unsigned long long)host_pages_to_guest(vm.external_page_count));
    sbf(&s, "nr_anon_pages %llu\n",
        (unsigned long long)host_pages_to_guest(vm.internal_page_count));
    sbf(&s, "nr_mapped %llu\n",
        (unsigned long long)host_pages_to_guest(vm.external_page_count));
    sbf(&s, "nr_unevictable %llu\n",
        (unsigned long long)host_pages_to_guest(vm.wire_count));
    sbf(&s, "nr_dirty 0\n");
    sbf(&s, "nr_writeback 0\n");
    sbf(&s, "nr_slab_reclaimable 0\n");
    sbf(&s, "nr_slab_unreclaimable 0\n");
    // pgpgin and pgpgout are the trap in this file: Linux counts them in
    // KILOBYTES while pswpin/pswpout and every nr_* right above are in pages.
    // The unit is MEASURED, not inferred from the kernel source -- the earlier
    // "the block layer feeds it sectors >> 1" gloss named a mechanism that
    // submit_bio does not obviously perform, and the reviewer was right to
    // distrust it. On the project's aarch64 guest, drop_caches then an
    // O_DIRECT read of 256 MiB off a real block device moved pgpgin by 263636.
    // 256 MiB is 262144 KiB and 524288 512-byte sectors, so the counter is in
    // KiB (0.6% over, which is the rest of the system's I/O) and definitively
    // not in sectors. Darwin's vm.pageins is a page count in the host granule,
    // so page_count * host_page / 1024 is the conversion; leaving it unscaled
    // would understate paging by the page size and look merely "low".
    sbf(&s, "pgpgin %llu\n", (unsigned long long)(vm.pageins * (ps / 1024)));
    sbf(&s, "pgpgout %llu\n", (unsigned long long)(vm.pageouts * (ps / 1024)));
    sbf(&s, "pswpin %llu\n", (unsigned long long)vm.swapins);
    sbf(&s, "pswpout %llu\n", (unsigned long long)vm.swapouts);
    sbf(&s, "pgfault %llu\n", (unsigned long long)vm.faults);
    // Darwin's "pageins" is a fault that had to touch storage, which is
    // precisely Linux's major fault.
    sbf(&s, "pgmajfault %llu\n", (unsigned long long)vm.pageins);
    sbf(&s, "pgactivate %llu\n", (unsigned long long)vm.reactivations);
    sbf(&s, "pgfree %llu\n", (unsigned long long)vm.zero_fill_count);
    return sb_done(&s);
}

// ------------------------------------------------------------ /proc/version

// Held to dispatch.c's do_uname(): release "6.6.0-lxrt", version "#1 SMP
// lxrt". Something that reads both and compares them -- and glibc's
// dl_discover_osversion does read /proc/version when uname's release fails to
// parse -- must not find two different kernels.
#define LXRT_OSRELEASE "6.6.0-lxrt"
#define LXRT_OSVERSION "#1 SMP lxrt"

static size_t gen_version(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    sbf(&s, "Linux version " LXRT_OSRELEASE " (lxrt@darwin) (clang) "
            LXRT_OSVERSION "\n");
    return sb_done(&s);
}

// -------------------------------------------- /proc/mounts, self/mountinfo

// Both files describe a Linux filesystem tree that does not exist, and that is
// the deliberate choice. getmntinfo(3) would give the real answer -- apfs on
// /System/Volumes/Data, firmlinks, /dev/disk3s5 -- and srt-bwrap reads
// mountinfo specifically to decide what to bind into its container. Feeding it
// Darwin volume paths makes it try to bind host paths that mean nothing inside
// the guest, and it fails with a message about the host's disk layout rather
// than about the real problem. A minimal, plausible Linux tree makes it take
// the ordinary path.
//
// Getting the shape right matters more than the contents: bwrap parses
// mountinfo positionally and the optional-field list before the "-" separator
// is variable-length, so the "-" must be there even when there are no optional
// fields.
static size_t gen_mounts(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    sbf(&s, "/dev/root / ext4 rw,relatime 0 0\n");
    sbf(&s, "devtmpfs /dev devtmpfs rw,nosuid,relatime 0 0\n");
    sbf(&s, "devpts /dev/pts devpts "
            "rw,nosuid,noexec,relatime,gid=5,mode=620,ptmxmode=666 0 0\n");
    sbf(&s, "tmpfs /dev/shm tmpfs rw,nosuid,nodev 0 0\n");
    sbf(&s, "proc /proc proc rw,nosuid,nodev,noexec,relatime 0 0\n");
    sbf(&s, "sysfs /sys sysfs rw,nosuid,nodev,noexec,relatime 0 0\n");
    sbf(&s, "tmpfs /tmp tmpfs rw,nosuid,nodev 0 0\n");
    return sb_done(&s);
}

static size_t gen_mountinfo(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    // id parent major:minor root mountpoint options [optional...] - fstype
    // source superopts. The root entry is parent 1 (itself, by convention for
    // the mount namespace root) and carries a peer group so that a caller
    // checking propagation does not conclude the tree is unshared already.
    sbf(&s, "23 1 254:0 / / rw,relatime shared:1 - ext4 /dev/root rw\n");
    sbf(&s, "24 23 0:5 / /dev rw,nosuid,relatime shared:2 - devtmpfs devtmpfs"
            " rw\n");
    sbf(&s, "25 24 0:6 / /dev/pts rw,nosuid,noexec,relatime shared:3 - devpts"
            " devpts rw,gid=5,mode=620,ptmxmode=666\n");
    sbf(&s, "26 24 0:7 / /dev/shm rw,nosuid,nodev shared:4 - tmpfs tmpfs rw\n");
    sbf(&s, "27 23 0:8 / /proc rw,nosuid,nodev,noexec,relatime shared:5 - proc"
            " proc rw\n");
    sbf(&s, "28 23 0:9 / /sys rw,nosuid,nodev,noexec,relatime shared:6 - sysfs"
            " sysfs rw\n");
    sbf(&s, "29 23 0:10 / /tmp rw,nosuid,nodev shared:7 - tmpfs tmpfs rw\n");
    return sb_done(&s);
}

// ------------------------------------------------- /proc/uptime, /proc/loadavg

static size_t gen_uptime(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    double up = uptime_sec();
    host_cpu_load_info_data_t cl;
    double idle = cpu_load(&cl) ? (double)cl.cpu_ticks[CPU_STATE_IDLE] / 100.0
                                : 0.0;
    // Linux's second figure is idle time summed over all CPUs, so on an
    // n-core machine it legitimately exceeds the first. Darwin's aggregate
    // host_cpu_load_info is summed the same way, so no division is wanted.
    sbf(&s, "%.2f %.2f\n", up, idle);
    return sb_done(&s);
}

static size_t gen_loadavg(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    double la[3] = { 0, 0, 0 };
    if (getloadavg(la, 3) != 3)
        la[0] = la[1] = la[2] = 0.0;

    // The "running/total" pair counts TASKS on Linux -- threads, not processes.
    // Darwin's KERN_PROC_ALL is a process count, so this understates the
    // denominator on any threaded system. The size-only sysctl probe is used
    // because it needs no buffer, and the kernel pads it, so the count is an
    // upper estimate of processes and a lower one of tasks.
    unsigned total = 1;
    int mib[3] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL };
    size_t sz = 0;
    if (sysctl(mib, 3, NULL, &sz, NULL, 0) == 0 && sz >= sizeof(struct kinfo_proc))
        total = (unsigned)(sz / sizeof(struct kinfo_proc));

    // The runnable count is 1: this thread. Darwin publishes no system-wide
    // run-queue depth, and counting our own threads would count sleeping ones.
    sbf(&s, "%.2f %.2f %.2f 1/%u %d\n", la[0], la[1], la[2], total,
        (int)getpid());
    return sb_done(&s);
}

// ------------------------------------------- /proc/self/stat, /proc/self/statm

// Darwin's time_value_t is seconds plus microseconds; Linux's stat fields are
// in clock ticks. sysconf(_SC_CLK_TCK) is 100 on both (measured), and it is
// also what stack.c publishes as AT_CLKTCK, so a guest calling
// sysconf(_SC_CLK_TCK) to divide these gets a consistent answer.
#define LXRT_CLK_TCK 100

static uint64_t tv_to_ticks(time_value_t t)
{
    return (uint64_t)t.seconds * LXRT_CLK_TCK +
           (uint64_t)t.microseconds / (1000000 / LXRT_CLK_TCK);
}

// Ticks since boot at which this process started. kinfo_proc carries the
// absolute start time as a timeval, so the subtraction against kern.boottime
// is the whole of it. A zero here makes `ps -o etime` style code inside the
// guest report an uptime-long lifetime, which is why it is measured.
static uint64_t start_ticks(void)
{
    struct kinfo_proc kp;
    memset(&kp, 0, sizeof kp);
    size_t len = sizeof kp;
    int mib[4] = { CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid() };
    if (sysctl(mib, 4, &kp, &len, NULL, 0) != 0 || len == 0)
        return 0;
    uint64_t bt = boot_time_sec();
    uint64_t st = (uint64_t)kp.kp_proc.p_starttime.tv_sec;
    if (!bt || st < bt)
        return 0;
    return (st - bt) * LXRT_CLK_TCK +
           (uint64_t)kp.kp_proc.p_starttime.tv_usec / (1000000 / LXRT_CLK_TCK);
}

static size_t gen_self_stat(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    mach_task_basic_info_data_t tb;
    if (!task_basic(&tb))
        memset(&tb, 0, sizeof tb);
    task_thread_times_info_data_t tt;
    if (!task_thread_times(&tt))
        memset(&tt, 0, sizeof tt);
    task_events_info_data_t te;
    if (!task_events(&te))
        memset(&te, 0, sizeof te);

    // Fields 14 and 15. tb carries only the threads that have EXITED and tt
    // only the ones still running, so the process total is the sum -- see the
    // comment on task_thread_times(). Either half alone is a lie, and the
    // terminated half alone is the lie that reads as zero.
    uint64_t utime = tv_to_ticks(tb.user_time) + tv_to_ticks(tt.user_time);
    uint64_t stime = tv_to_ticks(tb.system_time) + tv_to_ticks(tt.system_time);

    // Linux truncates comm to TASK_COMM_LEN-1 = 15 and wraps it in parens;
    // anything reading field 2 scans to the LAST ')' precisely because the
    // name may contain one, so a long name here is a compatibility hazard
    // rather than a cosmetic one.
    const char *nm = getprogname();
    char comm[16];
    snprintf(comm, sizeof comm, "%s", nm ? nm : "lxrt");

    // rss is a page count the guest scales by AT_PAGESZ; resident_size is
    // bytes, so the divisor is the guest granule.
    uint64_t ps = guest_page_bytes();
    uint64_t faults = nonneg(te.faults), major = nonneg(te.pageins);
    uint64_t minflt = faults > major ? faults - major : 0;

    // Field 25, rsslim, is unconditionally Linux's "unlimited".
    //
    // This used to be getrlimit(RLIMIT_RSS), which on Darwin is not an RSS
    // limit at all: sys/resource.h defines RLIMIT_RSS as RLIMIT_AS, literally
    // the same value 5, so the call returns the ADDRESS-SPACE ceiling. Under
    // `ulimit -v` that would publish a false RSS cap under an RSS label, and a
    // guest allocator that honours rsslim would throttle itself against a
    // number that means something else. Darwin cannot express an RSS limit, so
    // the honest answer is the one Linux gives when there is none.
    //
    // The value matters as much as the source: RLIM_INFINITY is NOT the same
    // number on the two systems -- Darwin's is INT64_MAX
    // (0x7fffffffffffffff, measured), Linux's is UINT64_MAX. Linux prints
    // 18446744073709551615 here, and a guest comparing the field against its
    // own RLIM_INFINITY would read the Darwin value as a finite limit of
    // roughly 9 exabytes.
    unsigned long long rsslim = ~0ull;

    // Field 38, exit_signal: Linux's SIGCHLD is 17, Darwin's is 20. The Linux
    // number goes in the Linux file. Field 41, policy: Linux SCHED_OTHER is 0,
    // Darwin's is 1 (measured), so again the Linux number.
    // Fields 26-30 (startcode, endcode, startstack, kstkesp, kstkeip) are 0:
    // the guest's text and stack extents are known to elf.c and stack.c, not
    // here, and Linux itself zeroes them for a process the reader may not
    // ptrace.
    sbf(&s,
        "%d (%s) R %d %d %d 0 -1 0 "          // 1-9
        "%llu 0 %llu 0 "                      // 10-13 minflt cminflt majflt cmajflt
        "%llu %llu 0 0 "                      // 14-17 utime stime cutime cstime
        "20 0 %u 0 %llu "                     // 18-22 prio nice threads itreal start
        "%llu %llu %llu "                     // 23-25 vsize rss rsslim
        "0 0 0 0 0 "                          // 26-30
        "0 0 0 0 0 "                          // 31-35 signal..wchan
        "0 0 17 0 0 0 "                       // 36-41 nswap cnswap exit_signal
                                              //       processor rt_prio policy
        "0 0 0 "                              // 42-44 blkio guest cguest
        "0 0 0 0 0 0 0 0\n",                  // 45-52
        (int)getpid(), comm, (int)getppid(), (int)getpgrp(), (int)getsid(0),
        (unsigned long long)minflt, (unsigned long long)major,
        (unsigned long long)utime, (unsigned long long)stime,
        thread_count(), (unsigned long long)start_ticks(),
        (unsigned long long)virtual_size_linuxish(),
        (unsigned long long)(tb.resident_size / ps), rsslim);
    return sb_done(&s);
}

static size_t gen_self_statm(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    mach_task_basic_info_data_t tb;
    if (!task_basic(&tb))
        memset(&tb, 0, sizeof tb);
    uint64_t ps = guest_page_bytes();
    uint64_t size = virtual_size_linuxish() / ps;
    uint64_t res = tb.resident_size / ps;
    // size resident shared text lib data dt, all in pages. `lib` has been
    // hardwired to 0 in Linux since 2.6 and `dt` since 2.6.22, so those two
    // zeroes are what a real kernel prints, not a gap. `shared` and `text`
    // would need per-mapping accounting Darwin does not expose; `data` is the
    // resident set minus a text figure we do not have, so it equals resident.
    sbf(&s, "%llu %llu 0 0 0 %llu 0\n", (unsigned long long)size,
        (unsigned long long)res, (unsigned long long)res);
    return sb_done(&s);
}

// -------------------------------------------------------- /proc/self/auxv

// Auxiliary vector types, matching stack.c's enum. Duplicated rather than
// shared because stack.c does not export them, and they are fixed ABI.
enum {
    AX_NULL = 0, AX_PAGESZ = 6, AX_UID = 11, AX_EUID = 12, AX_GID = 13,
    AX_EGID = 14, AX_HWCAP = 16, AX_CLKTCK = 17, AX_SECURE = 23, AX_HWCAP2 = 26,
};

// Must stay equal to the AT_HWCAP stack.c publishes. A guest that reads
// getauxval(AT_HWCAP) after a re-exec gets it from this file rather than from
// the stack, so a divergence between the two shows up only in re-executed
// children -- the late, symptomless failure this codebase keeps meeting.
#define LXRT_AUXV_HWCAP  ((1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | \
                          (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | \
                          (1u << 12))

// The real vector, if the integrator handed it over. 64 pairs is generous:
// stack.c writes 17.
//
// The copy is staged in a local and committed only once the whole vector has
// been read successfully. Writing straight into g_auxv[] meant a mid-vector
// EFAULT returned -14 having already clobbered the published vector while
// g_auxv_words still named the OLD length: reproduced, a failed 4-pair call
// left /proc/self/auxv reading 6/4096, 16/0xdead, 17/250, 25/0xaaaaaaaa, 0/0
// -- the failed vector's first three pairs spliced onto the previous vector's
// tail, AT_PAGESZ=4096 included, which contradicts the 16384 this runtime
// actually publishes. A caller that checks the return value and falls back
// deserves either the old truth or the measured fallback, never a splice of
// the two.
//
// The mutex covers readers as well as writers. An _Atomic length published
// last would order the length against the array for one writer, but two
// set_auxv calls can still interleave their memcpys under a reader; the lock
// is held only across a memcpy of at most 1 KB, on a path that already does
// file I/O.
#define AUXV_MAX_PAIRS 64
static uint64_t g_auxv[AUXV_MAX_PAIRS * 2];
static size_t g_auxv_words;
static pthread_mutex_t g_auxv_lock = PTHREAD_MUTEX_INITIALIZER;
LXRT_FORK_SAFE(proc_ext_g_auxv_lock, g_auxv_lock)

long lxrt_proc_ext_set_auxv(const void *auxv)
{
    if (!auxv)
        return LERR(EFAULT);

    uint64_t tmp[AUXV_MAX_PAIRS * 2];
    uint64_t pair[2];
    const uint64_t *src = auxv;
    size_t words = 0;
    for (size_t i = 0; i < AUXV_MAX_PAIRS; i++) {
        if (!safe_copy_in(pair, src + i * 2, sizeof pair))
            return LERR(EFAULT);
        tmp[words++] = pair[0];
        tmp[words++] = pair[1];
        if (pair[0] == AX_NULL) {
            pthread_mutex_lock(&g_auxv_lock);
            memcpy(g_auxv, tmp, words * sizeof(uint64_t));
            g_auxv_words = words;
            pthread_mutex_unlock(&g_auxv_lock);
            return 0;
        }
    }
    // No AT_NULL inside the bound. Refusing is the right answer: a vector that
    // long is a bad pointer that happens to be readable, and copying more of
    // it would leak whatever follows into a file the guest can read. The
    // previously published vector is left alone for the same reason the EFAULT
    // path leaves it alone -- the old truth beats a half-truth.
    return LERR(E2BIG);
}

// Binary, not text: pairs of native 64-bit words ending in AT_NULL. When the
// hook has not been called the file still has to exist, because glibc's
// getauxval() falls back to reading it and returning ENOENT there turns into
// "this machine has no HWCAP" rather than into an error. The fallback set is
// the measurable minimum -- page size, the HWCAP stack.c agrees to, the clock
// tick, and the four credential entries -- and deliberately omits AT_PHDR,
// AT_BASE, AT_ENTRY and AT_RANDOM, which cannot be reconstructed from outside
// stack.c and would be actively harmful if guessed: AT_RANDOM in particular is
// the address of the stack canary seed.
static size_t gen_auxv(char *b, size_t cap)
{
    pthread_mutex_lock(&g_auxv_lock);
    size_t words = g_auxv_words;
    if (words) {
        size_t n = words * sizeof(uint64_t);
        if (n > cap) {
            pthread_mutex_unlock(&g_auxv_lock);
            return GEN_TRUNC;       // a half auxv is unparseable, not merely short
        }
        memcpy(b, g_auxv, n);
        pthread_mutex_unlock(&g_auxv_lock);
        return n;
    }
    pthread_mutex_unlock(&g_auxv_lock);

    uint64_t v[] = {
        AX_PAGESZ, guest_page_bytes(),
        AX_HWCAP,  LXRT_AUXV_HWCAP,
        AX_HWCAP2, 0,
        AX_CLKTCK, LXRT_CLK_TCK,
        AX_UID,    (uint64_t)getuid(),
        AX_EUID,   (uint64_t)geteuid(),
        AX_GID,    (uint64_t)getgid(),
        AX_EGID,   (uint64_t)getegid(),
        AX_SECURE, 0,
        AX_NULL,   0,
    };
    size_t n = sizeof v;
    if (n > cap)
        return GEN_TRUNC;
    memcpy(b, v, n);
    return n;
}

// ------------------------------------------------------------- /proc/sys/*

// Yama does not exist on Darwin, so this is a translation of Darwin's actual
// ptrace policy into the Linux scale, not a passthrough. 0 would mean "any
// process may trace any other", which is flatly untrue here: task_for_pid()
// needs root or com.apple.security.cs.debugger and fails for an unrelated
// process even as the same user. 1 -- "only descendants, unless the tracee
// opts in with PR_SET_PTRACER" -- is the closest honest rung, and it is also
// the value Ubuntu ships, so callers have a tested path for it. Reporting 0
// makes a crash handler attach out-of-process and then fail at the attach.
static size_t gen_ptrace_scope(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    sbf(&s, "1\n");
    return sb_done(&s);
}

// perf_event_open does not exist here at all. 2 is the Linux default ("no
// kernel or CPU-wide tracing for unprivileged users"), but 3 -- Debian and
// Ubuntu's extra rung, "no unprivileged perf at all" -- is the value that
// matches reality, and tools that special-case the paranoid level already
// understand it. The honest failure still happens at the syscall, which
// returns ENOSYS; this only stops callers from getting there.
static size_t gen_perf_paranoid(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    sbf(&s, "3\n");
    return sb_done(&s);
}

// The ceiling on RLIMIT_NOFILE. Linux's default is 1048576; Darwin's real
// ceiling is kern.maxfilesperproc, measured at 61440 on this machine. Printing
// Linux's number makes a guest raise its soft limit to something Darwin's
// setrlimit refuses, and the refusal arrives as EINVAL from a call the guest
// had every reason to expect would work.
// No namespaces exist here, and saying so is the honest answer: Steam's
// steamwebhelper.sh reads these two knobs and, seeing 0, starts CEF with
// --no-sandbox instead of trying a user-namespace sandbox that would fail.
static size_t gen_max_user_namespaces(char *b, size_t cap)
{
    return (size_t)snprintf(b, cap, "0\n");
}
static size_t gen_unprivileged_userns_clone(char *b, size_t cap)
{
    return (size_t)snprintf(b, cap, "0\n");
}

static size_t gen_nr_open(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    sbf(&s, "%llu\n", (unsigned long long)sctl_u64("kern.maxfilesperproc", 61440));
    return sb_done(&s);
}

// inotify is emulated over kqueue's EVFILT_VNODE (runtime/inotify.c), which
// needs an open descriptor per watched file and cannot watch a subtree; the
// limit is honest about that. 8192 is the stock Linux default. It is reported as
// the stock default rather than as a large tuned number precisely so that a
// caller which merely checks the ceiling proceeds and then meets the honest
// ENOSYS from inotify_init1, instead of one which believes it has 524288
// watches available and silently never receives an event.
static size_t gen_max_user_watches(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    sbf(&s, "8192\n");
    return sb_done(&s);
}

// 0 = heuristic overcommit, which is what Darwin does: it hands out address
// space freely and kills on real exhaustion. This one genuinely matches.
static size_t gen_overcommit(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    sbf(&s, "0\n");
    return sb_done(&s);
}

// Darwin recycles pids at 99999 (PID_MAX in the XNU sources); kern.maxproc,
// 4000 here, is the concurrent-process cap and a different number entirely.
// Linux's default pid_max is 32768, which is SMALLER than the largest pid this
// runtime will hand out -- so a guest that sizes a pid-indexed table from this
// file and then sees pid 40000 from getpid() writes out of bounds. That is
// exactly the late, quiet failure this file exists to avoid.
static size_t gen_pid_max(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    sbf(&s, "99999\n");
    return sb_done(&s);
}

static size_t gen_osrelease(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    sbf(&s, LXRT_OSRELEASE "\n");
    return sb_done(&s);
}

static void uuid_format(struct sb *s, const uint8_t u[16])
{
    sbf(s, "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
           "%02x%02x%02x%02x%02x%02x\n",
        u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
        u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

// boot_id must be the SAME value in every process for as long as the machine
// stays up, and a DIFFERENT one after a reboot. That is the whole contract:
// dbus, systemd-shaped code and Steam's own crash reporter use it to decide
// whether two observations came from the same boot. A per-process
// arc4random_buf would satisfy the format and break the meaning, and the break
// only shows when a second guest process starts -- so it is derived
// deterministically from kern.uuid (stable per machine) and kern.boottime
// (stable per boot) instead, mixed with splitmix64.
static size_t gen_boot_id(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    char host[64] = "";
    size_t hl = sizeof host;
    if (sysctlbyname("kern.uuid", host, &hl, NULL, 0) != 0)
        snprintf(host, sizeof host, "lxrt");

    uint64_t h = 1469598103934665603ull;            // FNV-1a offset basis
    for (const char *p = host; *p; p++) {
        h ^= (uint64_t)(unsigned char)*p;
        h *= 1099511628211ull;
    }
    uint64_t bt = boot_time_sec();
    h ^= bt + 0x9e3779b97f4a7c15ull;
    h *= 1099511628211ull;

    uint8_t u[16];
    uint64_t x = h;
    for (int i = 0; i < 16; i++) {
        x += 0x9e3779b97f4a7c15ull;                 // splitmix64
        uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
        u[i] = (uint8_t)((z ^ (z >> 31)) & 0xff);
    }
    u[6] = (uint8_t)((u[6] & 0x0f) | 0x40);         // RFC 4122 version 4
    u[8] = (uint8_t)((u[8] & 0x3f) | 0x80);         // variant 10x
    uuid_format(&s, u);
    return sb_done(&s);
}

// Linux hands out a fresh UUID on every read(2) of this file. Here it is a
// real file, so the best available granularity is a fresh one per lookup --
// that is, per open(). A guest that opens once and reads twice gets the same
// value where Linux would give two; a guest that opens each time, which is
// what every user of this file actually does, sees no difference.
static size_t gen_random_uuid(char *b, size_t cap)
{
    struct sb s = { b, cap, 0, false };
    uint8_t u[16];
    arc4random_buf(u, sizeof u);
    u[6] = (uint8_t)((u[6] & 0x0f) | 0x40);
    u[8] = (uint8_t)((u[8] & 0x3f) | 0x80);
    uuid_format(&s, u);
    return sb_done(&s);
}

// ------------------------------------------------------------------ table

// Scope, because /proc/stat and /proc/self/stat are different files with the
// same last component. procfs.c's directory carries a `self -> .` symlink, so
// both land on one host file if they share a name, and one of them has to be
// renamed.
//
// The per-process file keeps the name "stat" and the system-wide one becomes
// "stat.sys". That is the direction that closes the hole rather than leaving
// it: a caller which WALKS into <dir>/self/ and opens "stat" now gets the
// per-process file, which is the correct resolution, and nothing on Linux
// resolves /proc/<something>/stat to the system-wide file, so the opposite
// naming had a wrong answer available and this one does not. The whole-string
// path /proc/stat still maps explicitly to "stat.sys" through the table below.
// Verified before the swap: reading <dir>/self/stat returned
// "cpu  24113451 0 9331725 ...".
enum scope { SC_SYS, SC_SELF };

struct ext_file {
    enum scope scope;
    const char *guest;      // path under /proc, with any self/ prefix removed
    const char *host;       // path under the runtime's procfs directory
    size_t (*gen)(char *, size_t);
};

static const struct ext_file g_files[] = {
    { SC_SYS,  "meminfo",  "meminfo",  gen_meminfo  },
    { SC_SYS,  "cpuinfo",  "cpuinfo",  gen_cpuinfo  },
    { SC_SYS,  "stat",     "stat.sys", gen_stat     },
    { SC_SYS,  "vmstat",   "vmstat",   gen_vmstat   },
    { SC_SYS,  "version",  "version",  gen_version  },
    { SC_SYS,  "mounts",   "mounts",   gen_mounts   },
    { SC_SYS,  "uptime",   "uptime",   gen_uptime   },
    { SC_SYS,  "loadavg",  "loadavg",  gen_loadavg  },

    { SC_SELF, "stat",      "stat",      gen_self_stat  },
    { SC_SELF, "statm",     "statm",     gen_self_statm },
    { SC_SELF, "mountinfo", "mountinfo", gen_mountinfo  },
    { SC_SELF, "auxv",      "auxv",      gen_auxv       },
    // /proc/self/mounts is a real file on Linux -- verified on the project's
    // aarch64 guest, `-r--r--r--. 1 user user 0 /proc/self/mounts` -- and it
    // is the spelling glibc's setmntent/getmntent callers and libmount reach
    // for at least as often as /proc/mounts, Steam's free-space checks
    // included. Same host file as the system-wide one: the content is
    // identical on Linux too, both being views of the same mount table.
    { SC_SELF, "mounts",    "mounts",    gen_mounts     },

    { SC_SYS, "sys/user/max_user_namespaces",
              "sys/user/max_user_namespaces",       gen_max_user_namespaces },
    { SC_SYS, "sys/kernel/unprivileged_userns_clone",
              "sys/kernel/unprivileged_userns_clone", gen_unprivileged_userns_clone },
    { SC_SYS, "sys/kernel/yama/ptrace_scope",
              "sys/kernel/yama/ptrace_scope",       gen_ptrace_scope    },
    { SC_SYS, "sys/kernel/perf_event_paranoid",
              "sys/kernel/perf_event_paranoid",     gen_perf_paranoid   },
    { SC_SYS, "sys/kernel/pid_max",
              "sys/kernel/pid_max",                 gen_pid_max         },
    { SC_SYS, "sys/kernel/osrelease",
              "sys/kernel/osrelease",               gen_osrelease       },
    { SC_SYS, "sys/kernel/random/boot_id",
              "sys/kernel/random/boot_id",          gen_boot_id         },
    { SC_SYS, "sys/kernel/random/uuid",
              "sys/kernel/random/uuid",             gen_random_uuid     },
    { SC_SYS, "sys/fs/nr_open",
              "sys/fs/nr_open",                     gen_nr_open         },
    { SC_SYS, "sys/fs/inotify/max_user_watches",
              "sys/fs/inotify/max_user_watches",    gen_max_user_watches},
    { SC_SYS, "sys/vm/overcommit_memory",
              "sys/vm/overcommit_memory",           gen_overcommit      },
};

#define NFILES (sizeof g_files / sizeof g_files[0])

// Directories inside the /proc/sys tree, so that an opendir() of any of them
// succeeds and enumerates. Opening the directory materialises everything
// underneath it, which is what a reader walking the tree is about to need.
static const char *const g_dirs[] = {
    "sys", "sys/kernel", "sys/kernel/yama", "sys/kernel/random",
    "sys/fs", "sys/fs/inotify", "sys/vm",
};

#define NDIRS (sizeof g_dirs / sizeof g_dirs[0])

// Every file is regenerated on the lookup that asks for it, including the ones
// whose content never changes. Caching them would save a handful of sysctls
// against the open/read/close the caller is about to perform anyway, and would
// buy back the entire class of bug where the first answer is the only answer
// -- which is how a stale /proc/self/stat outlives the thread it describes.
//
// Returns false when the file could not be produced, for either reason a file
// can fail to be produced: the content did not fit (the buffer is grown and
// retried up to GEN_MAX_HARD, and a truncated file is never published), or the
// write failed (write_atomic has already removed the stale copy). The caller
// turns false into NULL, and NULL into ENOENT.
#define GEN_MAX_HARD (8 * 1024 * 1024)

static bool materialise(const struct ext_file *f, const char *dir)
{
    const char *slash = strrchr(f->host, '/');
    if (slash) {
        char sub[512];
        size_t n = (size_t)(slash - f->host);
        if (n < sizeof sub) {
            memcpy(sub, f->host, n);
            sub[n] = '\0';
            mkdirs(dir, sub);
        }
    }

    for (size_t cap = GEN_MAX; ; cap *= 2) {
        char *buf = malloc(cap);
        if (!buf)
            return false;
        size_t len = f->gen(buf, cap);
        if (len != GEN_TRUNC) {
            bool ok = write_atomic(dir, f->host, buf, len);
            free(buf);
            return ok;
        }
        free(buf);
        if (cap >= GEN_MAX_HARD)
            return false;
    }
}

// --------------------------------------------------------------- translate

const char *lxrt_proc_ext_translate(const char *path, const char *dir)
{
    if (!path || !dir || !*dir)
        return NULL;
    if (strncmp(path, "/proc/", 6) != 0)
        return NULL;

    const char *rest = path + 6;
    enum scope scope = SC_SYS;

    // /proc/self/... and /proc/<our pid>/... name the same thing. A numeric
    // first component that is NOT our pid is another process: not ours, and
    // not synthesisable (task_for_pid is entitled), so it falls through to
    // ENOENT rather than to a fabricated answer.
    if (strncmp(rest, "self/", 5) == 0) {
        scope = SC_SELF;
        rest += 5;
    } else if (rest[0] >= '0' && rest[0] <= '9') {
        char mypid[32];
        int k = snprintf(mypid, sizeof mypid, "%d/", (int)getpid());
        if (k <= 0 || strncmp(rest, mypid, (size_t)k) != 0)
            return NULL;
        scope = SC_SELF;
        rest += k;
    }

    // Trailing slash on a directory lookup: "/proc/sys/" and "/proc/sys" are
    // the same request.
    char trimmed[512];
    size_t rl = strlen(rest);
    while (rl && rest[rl - 1] == '/')
        rl--;
    if (rl == 0 || rl >= sizeof trimmed)
        return NULL;
    memcpy(trimmed, rest, rl);
    trimmed[rl] = '\0';

    // The procfs directory is normally already there; creating it here means a
    // lookup that arrives before lxrt_proc_init finished still works rather
    // than silently producing nothing.
    mkdir(dir, 0700);

    static _Thread_local char out[1024];

    if (scope == SC_SYS) {
        for (size_t i = 0; i < NDIRS; i++) {
            if (strcmp(trimmed, g_dirs[i]) != 0)
                continue;
            size_t dlen = strlen(g_dirs[i]);
            mkdirs(dir, g_dirs[i]);
            // A member that fails to generate is not a reason to fail the
            // DIRECTORY lookup: write_atomic has already unlinked its stale
            // copy, so it is simply absent from the listing and the open() of
            // it fails on its own with ENOENT. What would be wrong is
            // reporting a directory that does not exist, which the stat below
            // is there to rule out.
            for (size_t j = 0; j < NFILES; j++)
                if (g_files[j].scope == SC_SYS &&
                    strncmp(g_files[j].guest, g_dirs[i], dlen) == 0 &&
                    g_files[j].guest[dlen] == '/')
                    materialise(&g_files[j], dir);
            snprintf(out, sizeof out, "%s/%s", dir, g_dirs[i]);
            struct stat st;
            if (stat(out, &st) != 0 || !S_ISDIR(st.st_mode))
                return NULL;
            return out;
        }
    }

    for (size_t i = 0; i < NFILES; i++) {
        if (g_files[i].scope != scope || strcmp(g_files[i].guest, trimmed) != 0)
            continue;
        // NULL on a generation failure, never the path. Handing back a path
        // whose content is a previous generation -- or nothing at all -- is
        // the one outcome worse than ENOENT, because the guest has no way to
        // tell it apart from a fresh answer. procfs.c's caller falls through
        // to the ordinary translation, which produces ENOENT.
        if (!materialise(&g_files[i], dir))
            return NULL;
        snprintf(out, sizeof out, "%s/%s", dir, g_files[i].host);
        return out;
    }
    return NULL;
}
