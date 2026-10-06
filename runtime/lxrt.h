// lxrt -- Linux runtime on Darwin. MIGRATION_PLAN Stage 2.
//
// Loads a Linux aarch64 ELF into this process, rewrites its syscall sites to
// branch into a dispatcher, and runs it. No kernel, no hypervisor, no guest.

#ifndef LXRT_H
#define LXRT_H

#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

// Darwin page size on Apple Silicon. Everything mapped or protected has to be
// a multiple of this; Linux aarch64 ELFs are linked with p_align 0x10000,
// which is a multiple of it, so segments land cleanly. See
// benchmarks/stage2-elf-alignment.txt for the survey that established that.
#define LXRT_HOST_PAGE 16384

#define LXRT_ALIGN_DOWN(v, a) ((v) & ~((uint64_t)(a) - 1))
#define LXRT_ALIGN_UP(v, a)   LXRT_ALIGN_DOWN((v) + (a) - 1, a)

// Guest register state as the trampoline saves it. The layout is fixed by
// runtime/trampoline.S and must not be reordered.
// Fixed by runtime/trampoline.S; do not reorder. x[30] is the link register.
struct lxrt_regs {
    uint64_t x[31];     // x0..x30
    uint64_t pc_after;  // guest address of the instruction after the svc
};

// A half-open address range.
struct lxrt_range { uint64_t start, end; };

// Executable-section windows of an aarch64 ELF (runtime/elfsect.c).
int lxrt_elf_exec_sections(int fd, uint64_t file_off, uint64_t len, uint64_t map_base,
                           struct lxrt_range *out, int max);
int lxrt_elf_exec_sections_path(const char *path, uint64_t load_bias,
                                struct lxrt_range *out, int max);
void lxrt_elf_forget(uint64_t addr, uint64_t len);
bool lxrt_elf_gap_before_exec(int fd, uint64_t file_off, uint64_t map_base,
                              struct lxrt_range *gap);

struct lxrt_image {
    uint8_t *base;          // where the image actually landed
    uint64_t load_bias;     // base - lowest p_vaddr; 0 for an EXEC file
    uint64_t entry;         // runtime address of e_entry
    uint64_t phdr;          // runtime address of the program headers
    uint16_t phentsize;
    uint16_t phnum;
    size_t   span;          // bytes reserved at base
    uint64_t brk;           // end of the image, where the heap starts
    bool     is_pie;
    bool     subpage;   // p_align < host page: permissions kept per 4 KiB (subpage.c)
    // PT_INTERP, if any. A dynamic executable names its loader here and the
    // runtime must start *that*, not the executable's own entry point.
    char     interp[256];
    uint64_t interp_base;   // filled in by main.c once the loader is mapped
    // Executable ranges, so the rewriting pass never scans data segments.
    struct { uint64_t start, end; } exec[8];
    int      nexec;
    // Executable SECTIONS (a subset of the segments): where the x18 pass runs.
    struct lxrt_range code[16];
    int      ncode;
};

// One rewritten syscall site.
struct lxrt_site {
    uint64_t addr;      // runtime address of the rewritten instruction
    uint64_t tramp;     // runtime address of its trampoline
};

struct lxrt_rewrite_report {
    size_t scanned_words;
    size_t sites_found;         // svc
    size_t sites_rewritten;
    size_t sites_unreachable;   // out of branch range; poisoned instead
    size_t hvc_or_smc_found;    // present in a userspace image means trouble
    size_t tls_read_found;      // mrs Xt, TPIDR_EL0
    size_t tls_write_found;     // msr TPIDR_EL0, Xt
    size_t tls_rewritten;
    size_t tls_unreachable;
    size_t tls_kept;            // reads left in place in a kept range (tls.c)
    size_t ctr_found;           // mrs Xt, CTR_EL0
    size_t ctr_rewritten;
    size_t sysreg_found;        // mrs Xt, ID_AA64* / MIDR_EL1
    size_t sysreg_rewritten;
    size_t x18_found;           // instructions naming x18 inside executable sections
    size_t x18_rewritten;
    size_t x18_unsupported;     // forms the planner refuses; poisoned
    size_t x18_unreachable;     // no pool in branch range; poisoned
};
// The x18 pass runs only inside executable-section windows (runtime/elfsect.c):
// unlike `svc`, an instruction naming register 18 is an ordinary data word.
int lxrt_rewrite_range_code(uint64_t start, uint64_t end, const struct lxrt_range *code,
                            int ncode, struct lxrt_rewrite_report *rep, char **err);
// Virtual x18 (Darwin zeroes the real one on every exception return).
unsigned long lxrt_x18_slot_offset(void);
bool lxrt_x18_enabled(void);
uint64_t lxrt_x18_get(void);
void lxrt_x18_set(uint64_t v);

// elf.c
int  lxrt_load_elf(const char *path, struct lxrt_image *out, char **err);

// rewrite.c
int  lxrt_rewrite_image(struct lxrt_image *img, struct lxrt_rewrite_report *rep,
                        char **err);
// Rewrite an arbitrary executable range. Used for code the dynamic loader maps
// itself, which never passes through lxrt_load_elf: every shared library the
// guest's ld.so brings in arrives this way, full of unrewritten `svc`.
// The range must currently be writable; the caller seals it afterwards.
int  lxrt_rewrite_range(uint64_t start, uint64_t end,
                        struct lxrt_rewrite_report *rep, char **err);
// Could lxrt_rewrite_range find anything in [start, end)? A superset test of
// the same words, cheap enough to run on every W/X flip of a sub-page.
bool lxrt_rewrite_has_candidates(uint64_t start, uint64_t end);
void lxrt_rewrite_totals(struct lxrt_rewrite_report *out);
// Diagnostics: which trampoline pool, if any, an address falls in, and whether
// a guest mapping request would land on one. The guest's loader reserves large
// spans and fills them in, so a pool placed inside such a span gets mapped over
// -- a failure mode that otherwise looks like random corruption.
bool lxrt_pool_contains(uint64_t addr);
bool lxrt_pool_maybe(uint64_t addr);
bool lxrt_pool_overlaps(uint64_t start, uint64_t end);
void lxrt_pool_offer_elf_gap(uint64_t start, uint64_t end);

// dispatch.c -- the trampoline's target. Writes the result into regs->x[0].
void lxrt_dispatch(struct lxrt_regs *regs);
// Code the guest's dynamic loader maps arrives through mmap/mprotect, not
// through our ELF loader, so the dispatcher has to rewrite it on the way in.
void lxrt_dispatch_set_rewrite_mapped(bool on);
void lxrt_dispatch_set_trace(bool on);
// The guest's clocks (Linux clockid), nanoseconds. Compare guest-supplied
// absolute times against THIS, never against Darwin's clock_gettime.
uint64_t lxrt_guest_clock_ns(long clk);
// A blocking call Darwin cut short with EINTR although no guest handler ran
// during it (lxrt_sig_during_syscall still 0, see dispatch.c): the realtime
// carrier landing on a thread with nothing queued for it, for one. Linux
// never interrupts a call for a signal that runs no handler, so the call goes
// on -- with the time it has left.
static inline int lxrt_interrupted_internally(void)
{
    extern _Thread_local int lxrt_sig_during_syscall;
    return lxrt_sig_during_syscall == 0;
}
// Number of syscalls that hit the ENOSYS path, and the last one seen, so a run
// that dies reports *which* call it lacked rather than just failing.
uint64_t lxrt_dispatch_unimplemented_count(void);
long     lxrt_dispatch_last_unimplemented(void);

// signal.c -- guest signal delivery. Linux and Darwin disagree on signal
// numbers, on the frame layout, and on how a handler returns.
int  lxrt_signo_to_darwin(int linux_sig);
int  lxrt_signo_to_linux(int darwin_sig);
long lxrt_rt_sigaction(int lsig, const void *uact, void *uoldact, size_t sigsetsize);
long lxrt_rt_sigprocmask(int how, const uint64_t *uset, uint64_t *uoldset,
                         size_t sigsetsize);
long lxrt_sigaltstack(const void *uss, void *uoss);
long lxrt_kill(int pid, int lsig);
long lxrt_tgkill(int tgid, int tid, int lsig);
// Give the calling thread a host alternate signal stack (once per thread).
void lxrt_host_altstack_install(void);

// errno_map.c -- Darwin and Linux errno numbers are different, including a
// swapped pair (EAGAIN/EDEADLK). Never return a raw Darwin errno to a guest.
int lxrt_errno_to_linux(int e);

// ntsync.c: /dev/ntsync (LXRT_NTSYNC=1)
bool lxrt_ntsync_enabled(void);
bool lxrt_ntsync_path(int dirfd, const char *path);
long lxrt_ntsync_open(int lflags);
bool lxrt_ntsync_request(unsigned long lreq);
bool lxrt_ntsync_ioctl(int fd, unsigned long lreq, uint64_t arg, long *ret);

// subpage.c -- 4 KiB guest mappings on 16 KiB host pages. x86 Linux binaries
// are linked with p_align 0x1000 and Darwin's mmap refuses a MAP_FIXED address
// that is not 16 KiB aligned.
uint64_t lxrt_guest_page(void);   // stack.c: AT_PAGESZ (LXRT_GUEST_PAGE)
bool lxrt_subpage_needed(uint64_t addr, uint64_t len, uint64_t off);
long lxrt_subpage_mmap(uint64_t addr, uint64_t len, int prot, bool anon,
                       int fd, uint64_t off);
long lxrt_subpage_mprotect(uint64_t addr, uint64_t len, int prot);
bool lxrt_subpage_only_placeholders(uint64_t addr, uint64_t len);
bool lxrt_subpage_tracked(uint64_t addr, uint64_t len);
bool lxrt_subpage_tracked_locked(uint64_t addr, uint64_t len);
int lxrt_subpage_prot_at(uint64_t addr);   // -1: no record (ask the kernel)
void lxrt_subpage_forget(uint64_t addr, uint64_t len);
void lxrt_subpage_reapply(uint64_t addr, uint64_t len);
long lxrt_subpage_mmap_noreplace(uint64_t addr, uint64_t len, int prot, bool anon, int fd, uint64_t off);
int lxrt_subpage_describe_last(char *buf, size_t n);   // fault report: the mapping in progress
// uap: the faulting ucontext_t (NULL: no store emulation, flip only).
bool lxrt_subpage_handle_fault(uint64_t pc, uint64_t fault_addr, void *uap);

// privmap.c -- Linux MAP_PRIVATE semantics (see the file's updates until the
// first store) for private mappings of shared-memory files.
bool lxrt_privmap_eligible(int fd, uint64_t addr, uint64_t len, uint64_t off, int prot);
long lxrt_privmap_mmap(uint64_t addr, uint64_t len, int prot, int flags, int fd, uint64_t off);
void lxrt_privmap_forget(uint64_t addr, uint64_t len);
void lxrt_privmap_after_mprotect(uint64_t addr, uint64_t len, int prot);
bool lxrt_privmap_handle_fault(uint64_t fault_addr, bool is_write);
// shmirror.c -- a read-only MAP_SHARED range that ends up in a sub-page
// composite host page keeps following its file (KUSER_SHARED_DATA).
void     lxrt_shmirror_note(uint64_t addr, uint64_t len);
void     lxrt_shmirror_forget(uint64_t addr, uint64_t len);
uint64_t lxrt_shmirror_take_view(uint64_t hpage);
void     lxrt_shmirror_adopt(uint64_t hpage, uint64_t view);

// jit.c -- guest JIT memory. Apple Silicon never lets a thread write and
// execute the same page. The guest asks for each flip itself, through private
// syscall 0x4C580020; a flip issued from a signal handler does not survive the
// handler's return (measured -- benchmarks/stage5-jit.txt).
bool lxrt_jit_register(uint64_t start, uint64_t len);
void lxrt_jit_forget(uint64_t start, uint64_t len);
void lxrt_jit_report_freed(uint64_t pc, uint64_t addr, void *uap);
bool lxrt_jit_contains(uint64_t addr);
bool lxrt_jit_handle_fault(uint64_t pc, uint64_t fault_addr, void *uap);
// brk from the execute-mode stub: restore the parked context (jit.c).
bool lxrt_jit_stub_trap(void *uap);
long lxrt_jit_set_write(int enable, uint64_t addr, uint64_t len);
void lxrt_jit_protect(int enable);   // pthread_jit_write_protect_np with signals blocked
bool lxrt_jit_thread_writable(void);

// wxsplit.c -- read-write-execute ranges of NATIVE aarch64 guests (V8's code
// range), split W^X per host page by the fault handler: a store makes a page
// read-write, a fetch scans it read-only and makes it read-execute. FEX keeps
// the read-write grant (its guest's RWX pages hold x86 code).
void lxrt_wx_set_program(const char *path);
bool lxrt_wx_enabled(void);
int  lxrt_wx_protect(uint64_t addr, uint64_t len, long *ret);
void lxrt_wx_forget(uint64_t addr, uint64_t len);
void lxrt_wx_moved(uint64_t old, uint64_t olen, uint64_t neu, uint64_t nlen);
bool lxrt_wx_contains(uint64_t addr);
bool lxrt_wx_covered(uint64_t addr, uint64_t len);
bool lxrt_wx_intersects(uint64_t addr, uint64_t len);
int  lxrt_wx_count(void);
bool lxrt_wx_handle_fault(uint64_t pc, uint64_t addr, uint32_t esr);
void lxrt_wx_handover(bool begin);   // dispatch.c: a range is being handed back
bool lxrt_wx_scan_for_exec(uint64_t hpage, const struct lxrt_range *r, int nr);
void lxrt_wx_stats_flush(void);
// rewrite.c: read-only count of the words lxrt_rewrite_range would replace.
size_t lxrt_rewrite_count(uint64_t start, uint64_t end);

// Syscall families that are large enough to own a file each. Their headers
// declare their own API; nothing here duplicates it.
#include "epoll_eventfd.h"
#include "fex_support.h"
#include "fileops2.h"
#include "futex_ops.h"
#include "inotify.h"
#include "ioctl_tty.h"
#include "mounts.h"
#include "mremap.h"
#include "proc_ext.h"
#include "sysv_ipc.h"
#include "timerfd_signalfd.h"

// posixtimer.c -- Linux per-process timer IDs, independent of file descriptors.
long lxrt_posix_timer_create(int clockid, const void *event, int *id_out);
long lxrt_posix_timer_gettime(int id, void *out);
long lxrt_posix_timer_getoverrun(int id);
long lxrt_posix_timer_settime(int id, int flags, const void *in, void *old);
long lxrt_posix_timer_delete(int id);

// fork() safety for the runtime's own locks. A fork on one guest thread while
// another holds one of these left the child blocked forever on its first use
// (measured: tests/elf/jit_fork_mt.c, the i386 Steam client). Each module
// registers its mutex: taken before fork, released after it in both parent
// and child (the child's only thread is the one that took them).
#define LXRT_FORK_SAFE(name, mutex)                                              \
    static void name##_fork_prepare(void) { pthread_mutex_lock(&(mutex)); }     \
    static void name##_fork_release(void) { pthread_mutex_unlock(&(mutex)); }   \
    __attribute__((constructor(200))) static void name##_fork_register(void) {  \
        pthread_atfork(name##_fork_prepare, name##_fork_release,                  \
                       name##_fork_release);                                      \
    }

// dirents.c -- getdents64, which Darwin does not have.
long lxrt_getdents64(int fd, void *buf, unsigned count);
void lxrt_dirents_close(int fd);

// procfs.c -- a synthetic /proc, materialised as real files. FEXServer dies on
// /proc/self/exe before anything else; Steam and pressure-vessel read /proc
// heavily.
// exe_path: the guest's name of its image (what readlink(/proc/self/exe) and a
// re-exec of /proc/self/exe use); exe_link: the host path the procfs link
// points at, or NULL for exe_path itself.
void        lxrt_proc_init(const char *exe_path, const char *exe_link, int argc, char **argv);
const char *lxrt_proc_translate(const char *path);
// sysfs.c -- /sys/devices/system/cpu in the guest root (CPU counting).
void lxrt_sysfs_init(void);
const char *lxrt_proc_untranslate(const char *host, char *out, size_t n);
const char *lxrt_proc_exe_path(void);
const char *lxrt_proc_exe_link_file(char *out, size_t n);

// process.c -- the runtime gets a real process model, because FEX and Steam
// both need one. A second process is a second runtime; nothing the single
// address space buys applies across that boundary.
long lxrt_fork(void);
void lxrt_proc_after_fork(void);
void lxrt_proc_cleanup(void);
void lxrt_sigstats_flush(void);
// remote_layer.m (native windows W2)
void *lxrt_remote_layer_create(uint32_t w, uint32_t h, uint32_t *ctx_id);
int lxrt_remote_layer_resize(void *layer, uint32_t w, uint32_t h);
void lxrt_remote_layer_release(void *layer);
// vdso_map.c (M6)
uint64_t lxrt_vdso_setup(void);
uint64_t lxrt_vdso_ehdr(void);
void lxrt_vdso_refresh(void);
uint64_t lxrt_mono_ns(void);
bool lxrt_vdso_contains(uint64_t addr);
struct rusage;
void lxrt_rusage_to_linux(const struct rusage *d, void *out);
void lxrt_proc_set_cmdline(const char *args, size_t len);
// Descriptors /proc/self/fd does not show: the runtime's and the emulator's
// (procfs.c). A guest close or dup2 onto the number makes it the guest's.
void lxrt_fd_hide(int fd, bool hide);
bool lxrt_fd_hidden(int fd);
int lxrt_fd_dup_private(int fd);
int lxrt_open_private(const char *path, int flags, int mode);   // procfs.c: high, hidden
void lxrt_close_private(int fd);
// futex_waitv.c: futex_waitv(2), and the note every FUTEX_WAKE leaves for it
long lxrt_futex_waitv(uint64_t waiters, uint32_t nr, uint32_t flags, uint64_t timeout, int32_t clockid);
void lxrt_futex_waitv_notify(const void *addr);
// epoll_eventfd.c: an eventfd that crosses SCM_RIGHTS (socket.c)
int lxrt_eventfd_companions(int fd, int *shm_fd, int *wfd);
bool lxrt_eventfd_adopt(int guest_fd, int shm_fd, int wfd);       // a hidden high duplicate, no low number in between (procfs.c)
const char *lxrt_host_tmpdir(void);   // the host temp dir for the runtime's own files (procfs.c)
// The runtime's own descriptors in the guest's table: high numbers, hidden
// from /proc/self/fd (procfs.c).
#include <dirent.h>
int lxrt_fd_private(int fd);
DIR *lxrt_fdopendir_private(int fd);
DIR *lxrt_opendir_private(const char *path);
DIR *lxrt_opendirat_private(int dirfd, const char *path);
void lxrt_closedir_private(DIR *d);
// procpid.c: other guest processes in /proc, and /proc/net.
bool lxrt_procpid_is_guest(int pid);
bool lxrt_procpid_fd_target(int pid, int fd, char *out, size_t n);
const char *lxrt_procnet_translate(const char *sub, const char *dir);
const char *lxrt_procpid_translate(const char *rest, const char *dir);
void lxrt_procpid_list(const char *dir);
bool lxrt_procpid_environ(int pid, const char *out_path);
struct stat;
bool lxrt_magic_link_stat(int dfd, const char *hp, struct stat *d);
void lxrt_proc_thread_gone(int tid);   // drop /proc/self/task/<tid>   // exit_group: remove this pid's directory
FILE *lxrt_trace_stream(void);
FILE *lxrt_info_stream(void);   // informational lines: nowhere with LXRT_QUIET=1 (dispatch.c)
int lxrt_trace_fd(void);
void lxrt_host_altstack_after_fork(void);
// Guest address base for 32-bit guests (runtime/gbase.c).
void lxrt_gbase_set(uint64_t base);
uint64_t lxrt_gbase(void);

// arena.c: the range kept for the guest's fixed mappings (Windows programs
// load at 0x140000000), reserved before the host's heap can take it.
bool lxrt_arena_on(void);
uint64_t lxrt_arena_reserve_heap(void);
void lxrt_arena_bounds(uint64_t *lo, uint64_t *hi);
bool lxrt_arena_free(uint64_t addr, uint64_t len);
void lxrt_arena_mapped(uint64_t addr, uint64_t len);
void lxrt_arena_unmapped(uint64_t addr, uint64_t len);
// Host (non-FEX) code faulting on a guest pointer below 4 GiB: rebase the
// load/store's base register and retry (gbase.c).
bool lxrt_lowptr_fixup(void *uap, uint64_t fault_addr);
// Flight recorder of address-space changes (runtime/memlog.c).
void lxrt_memlog(char op, uint64_t addr, uint64_t len, long a, long b, long ret);
void lxrt_memlog_dump(uint64_t fault_addr, const char *why);
void lxrt_memlog_dump_fd(int fd, uint64_t fault_addr, const char *why);
void lxrt_fault_note(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Where the main ELF image (FEX, under the Steam work) was loaded: a fault
// report can then say "FEX+0x..." for an offline symboliser.
extern uint64_t lxrt_main_image_base, lxrt_main_image_span;
// /proc/self/stat field 28, startstack (proc_ext.c): the initial stack
// pointer the runtime built (main.c), or, under FEX, an address in the stack
// FEX mapped for the x86 program with MAP_GROWSDOWN (dispatch.c).
extern uint64_t lxrt_start_stack;
void lxrt_note_growsdown(uint64_t addr, uint64_t len, long prot);
void lxrt_memlog_file(uint64_t addr, uint64_t len, uint64_t off, int fd);
bool lxrt_memlog_file_lookup(uint64_t addr, char *path, size_t n, uint64_t *off);
void lxrt_memlog_file_forget(uint64_t addr, uint64_t len);
void lxrt_futex_shared_add(uint64_t addr, uint64_t len);
long lxrt_sendto(int fd, const void *buf, size_t len, int lflags, const void *laddr, unsigned lalen);
long lxrt_recvfrom(int fd, void *buf, size_t len, int lflags, void *laddr, uint32_t *lalen);
void lxrt_futex_shared_remove(uint64_t addr, uint64_t len);
void lxrt_gbase_apply(long nr, uint64_t *x);
void lxrt_thread_after_fork(void);
long lxrt_execve(const char *path, char *const argv[], char *const envp[]);
// socket.c: a socket the guest created as SOCK_SEQPACKET (a Darwin datagram
// socket carrying the runtime's mark); ppoll waits on one in slices.
bool lxrt_is_seqpacket(int fd);
long lxrt_wait4(int pid, int *status, int loptions, void *rusage);

// socket.c -- sockaddr layout, address families and the SOCK_* flags Linux
// packs into the socket type all differ from Darwin's.
long lxrt_socket(int ldomain, int ltype, int proto);
void lxrt_socket_close(int fd);
// pathfd.c: O_PATH descriptors for sockets (Darwin cannot open them)
int lxrt_pathfd_open(int ddirfd, const char *hpath, int cloexec);
const char *lxrt_pathfd_path(int fd);
void lxrt_pathfd_close(int fd);
// evdev.c: /dev/input/eventN backed by steamarm-inputd (tools/inputd/PROTOCOL.md)
struct stat;
const char *lxrt_evdev_translate(const char *path, char *buf, size_t n);
int lxrt_evdev_open(const char *host, int lflags);
int lxrt_evdev_is(int fd);
void lxrt_evdev_close(int fd);
void lxrt_evdev_fix_stat(const char *host, int fd, struct stat *st);
long lxrt_evdev_read(int fd, void *buf, size_t n);
long lxrt_evdev_write(int fd, const void *buf, size_t n);
long lxrt_evdev_ioctl(int fd, unsigned long req, uint64_t arg);          // SO_PASSCRED state follows the fd
void lxrt_socket_dup(int oldfd, int newfd);
long lxrt_socketpair(int ldomain, int ltype, int proto, int *sv);
long lxrt_connect(int fd, const void *lsa, unsigned llen);
long lxrt_bind(int fd, const void *lsa, unsigned llen);
long lxrt_accept4(int fd, void *lsa, uint32_t *llen, int lflags);
long lxrt_getsockname(int fd, void *lsa, uint32_t *llen, bool peer);
long lxrt_sendmsg(int fd, const void *lmsg, int flags);
long lxrt_recvmsg(int fd, void *lmsg, int flags);
long lxrt_setsockopt(int fd, int llevel, int lopt, const void *val, unsigned len);
long lxrt_getsockopt(int fd, int llevel, int lopt, void *val, unsigned *len);

// fsflags.c -- open() and *at() flag values differ between Linux and Darwin.
int  lxrt_open_flags_to_darwin(int lflags);
int  lxrt_open_flags_to_linux(int dflags);
int  lxrt_at_flags_to_darwin(int lflags);
bool lxrt_at_is_removedir(int lflags);
bool lxrt_at_is_empty_path(int lflags);
int  lxrt_dirfd_to_darwin(int dirfd);
struct flock;
int  lxrt_fcntl_lock_cmd(int lcmd, bool *is_lock);
void lxrt_flock_to_darwin(const void *lin, struct flock *out);
void lxrt_flock_to_linux(const struct flock *d, void *lout);

// sysreg.c -- system registers Darwin traps even for its own code.
uint32_t lxrt_synthetic_ctr_el0(void);
uint64_t lxrt_synthetic_sysreg(uint32_t insn);

// tls.c -- guest thread pointer, kept in a Darwin TSD slot because TPIDR_EL0
// is not preserved across a context switch on Darwin.
int           lxrt_tls_init(void);
unsigned long lxrt_tls_slot_offset(void);
bool          lxrt_tls_ready(void);
void          lxrt_tls_set(uint64_t v);
uint64_t      lxrt_tls_get(void);
// Ranges whose `mrs Xt, TPIDR_EL0` reads are left as they are, because the
// program hashes its own code (BoringSSL's FIPS module; elfsect.c finds it):
// the hardware register is loaded with the guest's thread pointer when a
// read there returned Darwin's value and the load through it faulted.
void          lxrt_tlskeep_add(uint64_t start, uint64_t end, const char *why);
bool          lxrt_tlskeep_contains(uint64_t addr);
bool          lxrt_tlskeep_fixup(int sig, void *uap);
bool          lxrt_tlskeep_enabled(void);
unsigned long lxrt_tlskeep_fixups(void);

// thread.c -- guest threads and futexes.
int  lxrt_gettid(void);
void lxrt_qos_apply(void);            // LXRT_QOS=interactive (thread.c)
void lxrt_futex_park_enter(const void *addr);   // a thread about to park on a futex word (thread.c)
void lxrt_futex_park_leave(const void *addr);
void lxrt_futex_park_reset(void);               // in a fork child
extern int lxrt_guestprof_on;                   // LXRT_GUESTPROF=1 (guestprof.c)
void lxrt_guestprof_start(void);
void lxrt_guestprof_note(uint64_t x28);
long lxrt_set_tid_address(uint32_t *ctid);
long lxrt_futex(uint32_t *uaddr, int op, uint32_t val, uint64_t timeout_or_val2,
                uint32_t *uaddr2, uint32_t val3);
long lxrt_clone(uint64_t flags, uint64_t child_stack, uint32_t *ptid,
                uint64_t tls, uint32_t *ctid, const struct lxrt_regs *parent);
long lxrt_clone3(const void *uargs, uint64_t size,
                 const struct lxrt_regs *parent);
void lxrt_thread_exit(int code) __attribute__((noreturn));
// Signal targeting: the process contains AppKit's main thread and libdispatch
// workers as well as guest threads, so a signal must always be aimed at a
// specific guest thread rather than at the process.
bool lxrt_thread_lookup(int tid, pthread_t *out);
int  lxrt_thread_list(int *tids, int max);
bool lxrt_main_guest_thread(pthread_t *out);
// A process-directed signal that no guest thread accepted when it was posted
// lands on the host main thread (XNU binds it to a thread then); signal.c
// hands it on to a guest thread chosen by the masks noted here.
bool lxrt_thread_is_guest(void);
void lxrt_thread_note_mask(uint64_t lmask);
uint64_t lxrt_thread_noted_mask(void);
bool lxrt_thread_signal_target(int lsig, pthread_t *out);
// The first guest thread whose noted mask does not block `lsig`; false when
// every guest thread blocks it.
bool lxrt_thread_accepting(int lsig, pthread_t *out);
void lxrt_signal_rescue_stranded(void);
void lxrt_signal_raise_unqueued(uint64_t unblocked);
bool lxrt_rt_enqueue(int tid, int lsig);
int  lxrt_rt_dequeue_self(void);
int  lxrt_rt_dequeue_self_mask(uint64_t blocked);
bool lxrt_rt_pending_unblocked(uint64_t blocked);
uint64_t lxrt_rt_queued_self(void);
long lxrt_sigqueueinfo(int tgid, int tid, int lsig, const void *linfo);
struct iovec;
bool lxrt_seqpkt_readv(int fd, const struct iovec *iov, int cnt, long *ret);
long lxrt_rt_sigpending(uint64_t *uset, size_t sigsetsize);
uint64_t lxrt_rt_mask_get(void);
void lxrt_rt_mask_set(uint64_t m);

// stack.c
// Builds the Linux process start state: argc/argv/envp and the auxiliary
// vector, laid out as glibc's _start expects to find it at sp.
void *lxrt_build_stack(const struct lxrt_image *img, int argc, char **argv,
                       char **envp, char **err);

// main.c
// Transfers control. Does not return.
// lowpage.c: virtual pages below 4 GiB for native guests (LXRT_LOWPAGES=1).
bool lxrt_lowpage_on(void);
bool lxrt_lowpage_mmap(uint64_t addr, uint64_t len, long prot, long lflags,
                       long fd, long off, long *ret);
bool lxrt_lowpage_munmap(uint64_t addr, uint64_t len, long *ret);
bool lxrt_lowpage_mprotect(uint64_t addr, uint64_t len, long prot, long *ret);
struct __siginfo;
bool lxrt_lowpage_fault(const struct __siginfo *si, void *uap);
unsigned long lxrt_lowpage_faults(void);

void lxrt_enter(uint64_t entry, void *sp) __attribute__((noreturn));
void lxrt_thread_enter(const struct lxrt_regs *saved, uint64_t sp)
    __attribute__((noreturn));

#endif
