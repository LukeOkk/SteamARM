// The guest address base for 32-bit guests.
//
// Darwin cannot map the low 4 GiB of a process at all -- not at link time
// (benchmarks/stage5-fex.txt, "sub-4 GiB wall") and not at run time:
// mach_vm_deallocate(0, 4 GiB) says KERN_SUCCESS and every mmap or
// mach_vm_map below 4 GiB then fails with ENOMEM / KERN_INVALID_ADDRESS
// (measured 2026-09-25, scratchpad pagezero_probe.c). A 32-bit guest's
// address space therefore lives at host = base + guest, with the base
// chosen by FEX at start-up and announced here through a private syscall.
//
// FEX itself translates where it knows it holds a guest pointer (its JIT,
// its compat_ptr, its allocator, its signal frames). What it does NOT
// translate are the pointer arguments of the ~240 syscalls it passes straight
// through to the kernel with the guest's 32-bit values. The kernel here is
// this runtime, and it knows which arguments are pointers, so it applies the
// base itself -- unambiguously: no host memory ever lives below 4 GiB on this
// platform (measured), so any pointer argument below 4 GiB is a guest
// pointer. Memory-management addresses (mmap, munmap, mprotect, madvise,
// mremap, brk, shmat, mlock, msync) are deliberately NOT in the table: FEX's
// allocator owns them and hands over host addresses. (madvise and msync are
// passed through by FEX with the guest's address; dispatch.c bases a low one
// itself.)

#include "lxrt.h"

#include <stdio.h>
#include <stdlib.h>

static uint64_t g_base;

bool lxrt_trace_on(void);

void lxrt_gbase_set(uint64_t base)
{
    g_base = base;
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] guest address base 0x%llx announced\n",
                (unsigned long long)base);
}

uint64_t lxrt_gbase(void) { return g_base; }

// Bit i set: argument i is a pointer into guest memory. aarch64 Linux numbers.
#define A0 1u
#define A1 2u
#define A2 4u
#define A3 8u
#define A4 16u
#define A5 32u
static const struct { unsigned short nr; unsigned char mask; } k_ptr_args[] = {
    {5, A0 | A1 | A2}, {6, A0 | A1 | A2}, {7, A1 | A2},          // setxattr, lsetxattr, fsetxattr
    {8, A0 | A1 | A2}, {9, A0 | A1 | A2}, {10, A1 | A2},         // getxattr...
    {11, A0 | A1}, {12, A0 | A1}, {13, A1},                      // listxattr...
    {14, A0 | A1}, {15, A0 | A1}, {16, A1},                      // removexattr...
    {17, A0},                                                    // getcwd
    {21, A3},                                                    // epoll_ctl
    {22, A1 | A4},                                               // epoll_pwait
    {27, A1},                                                    // inotify_add_watch
    {33, A1}, {34, A1}, {35, A1},                                // mknodat, mkdirat, unlinkat
    {36, A0 | A2}, {37, A1 | A3}, {38, A1 | A3},                 // symlinkat, linkat, renameat
    {40, A0 | A1 | A2 | A4}, {41, A0},                           // mount, umount2
    {43, A0 | A1}, {44, A1}, {45, A0},                           // statfs, fstatfs, truncate
    {48, A1}, {49, A0}, {51, A0}, {53, A1}, {54, A1},            // faccessat, chdir, chroot, fchmodat, fchownat
    {56, A1}, {59, A0}, {61, A1},                                // openat, pipe2, getdents64
    {63, A1}, {64, A1}, {65, A1}, {66, A1}, {67, A1}, {68, A1},  // read, write, readv, writev, pread64, pwrite64
    {69, A1}, {70, A1}, {71, A2},                                // preadv, pwritev, sendfile
    {72, A1 | A2 | A3 | A4 | A5},                                // pselect6
    {73, A0 | A2 | A3},                                          // ppoll
    {74, A1},                                                    // signalfd4
    {78, A1 | A2}, {79, A1 | A2}, {80, A1},                      // readlinkat, newfstatat, fstat
    {86, A2 | A3}, {87, A1}, {88, A1 | A2},                      // timerfd_settime/gettime, utimensat
    {90, A1}, {91, A1},                                          // capget, capset
    {95, A2 | A4}, {96, A0},                                     // waitid, set_tid_address
    {99, A0}, {100, A1 | A2}, {101, A0 | A1},                    // set/get_robust_list, nanosleep
    {102, A1}, {103, A1 | A2},                                   // getitimer, setitimer
    {106, A1 | A2}, {107, A1}, {109, A2 | A3},                   // timer_create/gettime/settime
    {111, A1}, {112, A1}, {113, A1}, {114, A1}, {115, A2 | A3},  // clock_settime/gettime/getres/nanosleep
    {116, A1},                                                   // syslog
    {118, A1}, {119, A2}, {121, A1}, {122, A2}, {123, A2}, {127, A1},  // sched_*
    {132, A0 | A1}, {133, A0}, {134, A1 | A2}, {135, A1 | A2},   // sigaltstack, rt_sigsuspend, rt_sigaction, rt_sigprocmask
    {136, A0}, {137, A0 | A1 | A2}, {138, A2},                   // rt_sigpending, rt_sigtimedwait, rt_sigqueueinfo
    {158, A1}, {159, A1}, {160, A0}, {161, A0}, {162, A0},       // getgroups, setgroups, uname, sethostname, setdomainname
    {163, A1}, {164, A1}, {165, A1},                             // getrlimit, setrlimit, getrusage
    {168, A0 | A1 | A2}, {169, A0 | A1}, {170, A0 | A1}, {171, A0},  // getcpu, gettimeofday, settimeofday, adjtimex
    {179, A0},                                                   // sysinfo
    {187, A2}, {188, A1}, {189, A1},                             // msgctl, msgrcv, msgsnd
    {192, A1 | A3}, {193, A1}, {195, A2},                        // semtimedop, semop, shmctl
    {199, A3}, {200, A1}, {202, A1 | A2}, {203, A1},             // socketpair, bind, accept, connect
    {204, A1 | A2}, {205, A1 | A2}, {206, A1 | A4}, {207, A1 | A4 | A5},  // getsockname, getpeername, sendto, recvfrom
    {208, A3}, {209, A3 | A4}, {211, A1}, {212, A1},             // setsockopt, getsockopt, sendmsg, recvmsg
    {221, A0 | A1 | A2},                                         // execve
    {232, A2},                                                   // mincore (vec only; addr is host)
    {236, A0 | A1}, {237, A1},                                   // get/set_mempolicy
    {240, A3}, {242, A1 | A2}, {243, A1 | A4},                   // rt_tgsigqueueinfo, accept4, recvmmsg
    {260, A1 | A3}, {261, A2 | A3},                              // wait4, prlimit64
    {263, A4}, {264, A1 | A2 | A3}, {265, A1}, {266, A1},        // fanotify_mark, name_to_handle_at, open_by_handle_at, clock_adjtime
    {269, A1}, {270, A1 | A3}, {271, A1 | A3},                   // sendmmsg, process_vm_readv/writev
    {274, A1}, {275, A1}, {276, A1 | A3},                        // sched_setattr/getattr, renameat2
    {277, A2}, {278, A0}, {279, A0},                             // seccomp, getrandom, memfd_create
    {281, A1 | A2 | A3},                                         // execveat
    {285, A1 | A3}, {286, A1}, {287, A1},                        // copy_file_range, preadv2, pwritev2
    {291, A1 | A4},                                              // statx
    {424, A2}, {437, A1 | A2}, {439, A1}, {441, A1 | A2 | A4},   // pidfd_send_signal, openat2, faccessat2, epoll_pwait2
    {449, A0}, {452, A1}, {454, A0}, {455, A0 | A4}, {456, A0},  // futex_waitv, fchmodat2, futex_wake, futex_wait, futex_requeue
};

static inline bool guest_ptr(uint64_t v) { return v && v < (1ull << 32); }

static void translate(uint64_t *x, unsigned mask)
{
    for (int i = 0; i < 6; i++)
        if ((mask & (1u << i)) && guest_ptr(x[i]))
            x[i] += g_base;
}

// Nested pointers. A 32-bit guest's iovecs reach us already rebuilt by FEX
// (compat layer, host pointers); a 64-bit low-window guest's pass straight
// through, so their iov_base fields may still be guest addresses. The guest's
// own array must not be modified: translate into a per-thread copy.
#define GB_MAX_IOV 1024
struct gb_iov { uint64_t base, len; };
static _Thread_local struct gb_iov g_iov_copy[GB_MAX_IOV];

static void translate_iov(uint64_t *arr, uint64_t count)
{
    if (!*arr || count == 0 || count > GB_MAX_IOV)
        return;
    const struct gb_iov *in = (const struct gb_iov *)(uintptr_t)*arr;
    bool any = false;
    for (uint64_t i = 0; i < count; i++)
        if (guest_ptr(in[i].base)) { any = true; break; }
    if (!any)
        return;
    for (uint64_t i = 0; i < count; i++) {
        g_iov_copy[i].base = guest_ptr(in[i].base) ? in[i].base + g_base : in[i].base;
        g_iov_copy[i].len = in[i].len;
    }
    *arr = (uint64_t)(uintptr_t)g_iov_copy;
}

void lxrt_gbase_apply(long nr, uint64_t *x)
{
    if (!g_base)
        return;
    switch (nr) {
    case 65: case 66: case 69: case 70: case 286: case 287:  // readv writev preadv pwritev preadv2 pwritev2
        translate(x, A1);
        translate_iov(&x[1], x[2]);
        return;
    case 25: {   // fcntl: the third argument is a pointer only for the lock commands
        unsigned cmd = (unsigned)x[1];
        if (cmd == 5 || cmd == 6 || cmd == 7 || cmd == 36 || cmd == 37 || cmd == 38 ||
            cmd == 15 || cmd == 16)
            translate(x, A2);
        return;
    }
    case 29: {   // ioctl: pointer requests in the set runtime/ioctl_tty.c serves
        unsigned req = (unsigned)x[1];
        switch (req) {
        case 0x5401: case 0x5402: case 0x5403: case 0x5404:            // TCGETS, TCSETS*
        case 0x802c542a: case 0x402c542b: case 0x402c542c: case 0x402c542d: // TCGETS2, TCSETS2*
        case 0x5413: case 0x5414: case 0x540f: case 0x5410: case 0x5429: // winsz, pgrp, sid
        case 0x5411: case 0x541b: case 0x5421:                          // TIOCOUTQ, FIONREAD, FIONBIO
            translate(x, A2);
        }
        return;
    }
    case 98: {   // futex: uaddr always; timeout only for the waiting ops; uaddr2 for the requeue ops
        unsigned op = (unsigned)x[1] & 0x7f;
        unsigned mask = A0;
        if (op == 0 /*WAIT*/ || op == 9 /*WAIT_BITSET*/ || op == 6 /*LOCK_PI*/ || op == 13 /*LOCK_PI2*/ ||
            op == 11 /*WAIT_REQUEUE_PI*/)
            mask |= A3;
        if (op == 3 /*REQUEUE*/ || op == 4 /*CMP_REQUEUE*/ || op == 5 /*WAKE_OP*/ ||
            op == 11 || op == 12 /*CMP_REQUEUE_PI*/)
            mask |= A4;
        translate(x, mask);
        return;
    }
    case 167: {  // prctl: PR_SET_NAME / PR_GET_NAME carry a buffer
        unsigned opt = (unsigned)x[0];
        if (opt == 15 || opt == 16)
            translate(x, A1);
        return;
    }
    case 191: {  // semctl: the union is a pointer except for SETVAL
        unsigned cmd = (unsigned)x[2] & 0xff;
        if (cmd != 16 /*SETVAL*/)
            translate(x, A3);
        return;
    }
    default:
        break;
    }
    for (size_t i = 0; i < sizeof k_ptr_args / sizeof k_ptr_args[0]; i++)
        if (k_ptr_args[i].nr == nr) {
            translate(x, k_ptr_args[i].mask);
            return;
        }
    // Not in the table. A pointer-looking argument here is either a syscall
    // this table forgot or a value that happens to be small; say so when
    // tracing rather than guess.
    // Memory-management syscalls carry lengths and flags that look like
    // small addresses; their address argument is FEX's allocator's business.
    if (nr == 214 || nr == 215 || nr == 216 || nr == 222 || (nr >= 226 && nr <= 234) || nr == 233 || nr == 284 || nr == 167)
        return;
    if (lxrt_trace_on())
        for (int i = 0; i < 6; i++)
            if (guest_ptr(x[i]) && x[i] >= 0x10000)
                fprintf(lxrt_trace_stream(), "[lxrt] guest-base: syscall %ld arg%d = 0x%llx below 4 GiB, "
                                "not in the pointer table\n", nr, i, (unsigned long long)x[i]);
}

// Host code dereferencing a guest pointer that was never given the base.
//
// A guest's memory below 4 GiB lives at host base + x, but nothing stops a
// raw guest pointer from reaching HOST code that is not FEX: a library thunk
// hands the guest's arguments straight to the host library (Vulkan: every
// structure, pNext chain and array the application passes, down into the
// runtime's shim and MoltenVK). Wine's 64-bit processes keep their heaps and
// stacks in the low window, so DXVK's first vkCreateInstance faulted at
// 0x31eca8 forever inside the host thunk (MEASURED).
//
// Such an access faults (no host memory exists below 4 GiB, see above). The
// fix-up adds the base to the load/store's base register and retries. In the
// 64-bit low window, host base + x is also the GUEST address base + x -- the
// same memory under a second name -- so the corrected register stays valid if
// it flows back to the guest; later accesses through it no longer fault.
// FEX's own code and this runtime's are excluded: a fault there is a bug to
// see, not to paper over.
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <sys/ucontext.h>

extern const struct mach_header_64 __dso_handle;
static uint64_t g_self_lo, g_self_hi;

__attribute__((constructor)) static void self_text_range(void)
{
    unsigned long sz = 0;
    uint8_t *t = getsegmentdata(&__dso_handle, "__TEXT", &sz);
    g_self_lo = (uint64_t)(uintptr_t)t;
    g_self_hi = g_self_lo + sz;
}

static _Atomic unsigned long g_lowptr_fixups;

bool lxrt_lowptr_fixup(void *uap, uint64_t fault_addr)
{
    static int off = -1;                         // LXRT_NO_LOWPTR_FIXUP=1: bisecting aid
    if (off < 0) off = getenv("LXRT_NO_LOWPTR_FIXUP") ? 1 : 0;
    if (off || !g_base || !uap || fault_addr < 0x10000 || fault_addr >= (1ull << 32))
        return false;
    ucontext_t *u = (ucontext_t *)uap;
    _STRUCT_ARM_THREAD_STATE64 *ss = &u->uc_mcontext->__ss;
    uint64_t pc = ss->__pc;
    // A jump to an address below 4 GiB faults on the fetch: there is no
    // instruction to read, and reading it here faulted again inside the
    // handler (MEASURED: a truncated host function pointer, 0xb425a7cc).
    if (pc < (1ull << 32))
        return false;
    uint64_t ib = lxrt_main_image_base, ie = ib + lxrt_main_image_span;
    if ((pc >= ib && pc < ie) || (pc >= g_self_lo && pc < g_self_hi))
        return false;
    uint32_t insn = *(const uint32_t *)(uintptr_t)pc;
    if ((insn & 0x0a000000u) != 0x08000000u)      // not a load/store
        return false;
    unsigned rn = (insn >> 5) & 31;
    if (rn == 31)                                  // sp is never a guest pointer
        return false;
    uint64_t *reg = rn == 29 ? &ss->__fp : rn == 30 ? &ss->__lr : &ss->__x[rn];
    uint64_t v = *reg;
    if (v < 0x10000 || v >= (1ull << 32))
        return false;
    *reg = v + g_base;
    unsigned long n = ++g_lowptr_fixups;
    if (lxrt_trace_on() && (n <= 8 || (n & (n - 1)) == 0))
        fprintf(lxrt_trace_stream(), "[lxrt] low guest pointer fixed up in host code: x%u 0x%llx at pc 0x%llx (%lu so far)\n",
                rn, (unsigned long long)v, (unsigned long long)pc, n);
    return true;
}
