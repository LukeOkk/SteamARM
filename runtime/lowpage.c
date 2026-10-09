// Virtual pages below 4 GiB for native (aarch64) guests: KUSER_SHARED_DATA.
//
// macOS keeps the low 4 GiB of every arm64 task unmapped (__PAGEZERO), and a
// native guest cannot have memory there: nothing relocates its addresses the
// way FEX's guest base relocates an x86 program's (gbase.c). Wine's Windows
// side needs exactly one thing down there, KUSER_SHARED_DATA, the page Windows
// keeps at 0x7ffe0000 in every process: ntdll's user_shared_data, allocated
// fixed by virtual_alloc_first_teb (wine exited there: "failed to map the
// shared user data", MEASURED with Valve's Proton ARM64, 2026-10-05), later
// remapped from wineserver's section by virtual_map_user_shared_data, read by
// ntdll and kernel32 for the clock and the processor features.
//
// With LXRT_LOWPAGES=1 a fixed mapping inside [0x7ffe0000, 0x7fff0000) is
// accepted as a VIRTUAL page: its memory lives at a host address of the
// runtime's choosing, the guest's address stays unmapped, and every load or
// store that faults there is carried out by the fault handler against the
// backing -- the instruction decoded, the registers of the signal context
// updated, the pc advanced. A read costs a fault, microseconds: right for a
// page read a few thousand times a second, wrong for a heap. So nothing else
// below 4 GiB is accepted: a MAP_FIXED_NOREPLACE probe there answers EEXIST
// ("busy", which sends Wine's allocator upward: try_map_free_area doubles its
// step) and a plain MAP_FIXED answers ENOMEM, as the host would.
//
// Off (the default) nothing here runs: FEX guests keep their guest base and
// their ENOMEM probes (dispatch.c).
#include "lxrt.h"
#include <dlfcn.h>
#include <mach-o/loader.h>
#include <setjmp.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <mach/arm/_structs.h>

bool lxrt_trace_on(void);

#define LP_LO   0x7ffe0000ull
#define LP_HI   0x7fff0000ull
#define LP_PAGE 4096ull
#define LP_N    ((LP_HI - LP_LO) / LP_PAGE)

#define LINUX_PROT_READ  1
#define LINUX_PROT_WRITE 2
#define LINUX_MAP_SHARED 1
#define LINUX_MAP_FIXED 0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FIXED_NOREPLACE 0x100000

struct lp_page {
    _Atomic(uint8_t *) host;    // the backing (a host page), NULL when unmapped
    _Atomic int prot;           // the guest's PROT_* (Linux values)
};
static struct lp_page g_pages[LP_N];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned long g_faults;

bool lxrt_lowpage_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_LOWPAGES");
        on = e && e[0] == '1';
    }
    return on;
}

static bool in_window(uint64_t addr, uint64_t len)
{
    return addr >= LP_LO && len && addr + len <= LP_HI;
}

static void release(unsigned i)
{
    uint8_t *h = atomic_exchange(&g_pages[i].host, NULL);
    if (h)
        munmap(h, LXRT_HOST_PAGE);
}

// A fixed request below 4 GiB. True when answered here (*ret is the result).
bool lxrt_lowpage_mmap(uint64_t addr, uint64_t len, long prot, long lflags,
                       long fd, long off, long *ret)
{
    if (!lxrt_lowpage_on() || !addr || addr >= (1ull << 32))
        return false;
    bool fixed = (lflags & LINUX_MAP_FIXED) != 0;
    bool noreplace = (lflags & LINUX_MAP_FIXED_NOREPLACE) != 0;
    if (!fixed && !noreplace)
        return false;                    // a hint: the host places it (above 4 GiB)
    if (!in_window(addr, len) || (addr % LP_PAGE) || (len % LP_PAGE)) {
        *ret = -(noreplace && !fixed ? EEXIST : ENOMEM);
        return true;
    }
    unsigned first = (unsigned)((addr - LP_LO) / LP_PAGE), n = (unsigned)(len / LP_PAGE);
    pthread_mutex_lock(&g_lock);
    if (noreplace && !fixed) {
        for (unsigned i = first; i < first + n; i++)
            if (atomic_load(&g_pages[i].host)) {
                pthread_mutex_unlock(&g_lock);
                *ret = -EEXIST;
                return true;
            }
    }
    for (unsigned i = first; i < first + n; i++) {
        uint8_t *h;
        if (lflags & LINUX_MAP_ANONYMOUS) {
            h = mmap(NULL, LXRT_HOST_PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        } else {
            // A file page (wineserver's section): shared, so the server's
            // clock updates arrive. Mapped readable and, when the file
            // allows it, writable; the guest's protection is enforced by
            // the emulation, not the host.
            int hprot = PROT_READ | ((prot & LINUX_PROT_WRITE) ? PROT_WRITE : 0);
            int hflags = (lflags & LINUX_MAP_SHARED) ? MAP_SHARED : MAP_PRIVATE;
            h = mmap(NULL, LXRT_HOST_PAGE, hprot, hflags, (int)fd,
                     (off_t)(off + (long)((i - first) * LP_PAGE)));
        }
        if (h == MAP_FAILED) {
            int e = errno;
            pthread_mutex_unlock(&g_lock);
            *ret = -e;
            return true;
        }
        release(i);
        atomic_store(&g_pages[i].prot, (int)prot);
        atomic_store(&g_pages[i].host, h);
    }
    pthread_mutex_unlock(&g_lock);
    if (lxrt_trace_on())
        fprintf(lxrt_trace_stream(), "[lxrt] lowpage: 0x%llx+0x%llx prot %ld %s -> virtual page(s)\n",
                (unsigned long long)addr, (unsigned long long)len, prot,
                (lflags & LINUX_MAP_ANONYMOUS) ? "anonymous" : "file");
    *ret = (long)addr;
    return true;
}

bool lxrt_lowpage_munmap(uint64_t addr, uint64_t len, long *ret)
{
    if (!lxrt_lowpage_on() || !len || addr + len > (1ull << 32))
        return false;
    pthread_mutex_lock(&g_lock);
    for (unsigned i = 0; i < LP_N; i++) {
        uint64_t g = LP_LO + i * LP_PAGE;
        if (g >= addr && g + LP_PAGE <= addr + len)
            release(i);
    }
    pthread_mutex_unlock(&g_lock);
    *ret = 0;                           // Linux: unmapping nothing is fine
    return true;
}

bool lxrt_lowpage_mprotect(uint64_t addr, uint64_t len, long prot, long *ret)
{
    if (!lxrt_lowpage_on() || !len || addr + len > (1ull << 32))
        return false;
    bool any = false, hole = false;
    for (uint64_t g = addr & ~(LP_PAGE - 1); g < addr + len; g += LP_PAGE) {
        if (g < LP_LO || g >= LP_HI || !atomic_load(&g_pages[(g - LP_LO) / LP_PAGE].host)) {
            hole = true;
            continue;
        }
        atomic_store(&g_pages[(g - LP_LO) / LP_PAGE].prot, (int)prot);
        any = true;
    }
    *ret = any && !hole ? 0 : -ENOMEM;  // Linux: ENOMEM when a page is unmapped
    return true;
}

// ----------------------------------------------------------------- faults

// The bytes [guest, guest+n) through the backing; false when a byte is not in
// a virtual page the access may touch.
static bool lp_access(uint64_t guest, void *buf, unsigned n, bool write)
{
    uint8_t *b = buf;
    while (n) {
        if (guest < LP_LO || guest >= LP_HI)
            return false;
        unsigned i = (unsigned)((guest - LP_LO) / LP_PAGE);
        uint8_t *h = atomic_load(&g_pages[i].host);
        int prot = atomic_load(&g_pages[i].prot);
        if (!h || !(prot & (write ? LINUX_PROT_WRITE : LINUX_PROT_READ)))
            return false;
        unsigned in = (unsigned)(guest % LP_PAGE), k = LP_PAGE - in;
        if (k > n) k = n;
        if (write) memcpy(h + in, b, k);
        else memcpy(b, h + in, k);
        guest += k; b += k; n -= k;
    }
    return true;
}

static uint64_t get_x(const _STRUCT_ARM_THREAD_STATE64 *ss, unsigned r)
{
    if (r < 29) return ss->__x[r];
    if (r == 29) return __darwin_arm_thread_state64_get_fp(*ss);
    if (r == 30) return __darwin_arm_thread_state64_get_lr(*ss);
    return 0;                                           // xzr
}

static void set_x(_STRUCT_ARM_THREAD_STATE64 *ss, unsigned r, uint64_t v)
{
    if (r < 29) ss->__x[r] = v;
    else if (r == 29) __darwin_arm_thread_state64_set_fp(*ss, v);
    else if (r == 30) __darwin_arm_thread_state64_set_lr_fptr(*ss, (void *)(uintptr_t)v);
    // 31: xzr, discarded
}

static uint64_t get_base(const _STRUCT_ARM_THREAD_STATE64 *ss, unsigned rn)
{
    return rn == 31 ? __darwin_arm_thread_state64_get_sp(*ss) : get_x(ss, rn);
}

static void set_base(_STRUCT_ARM_THREAD_STATE64 *ss, unsigned rn, uint64_t v)
{
    if (rn == 31) __darwin_arm_thread_state64_set_sp(*ss, v);
    else set_x(ss, rn, v);
}

static int64_t sext(uint64_t v, unsigned bits)
{
    uint64_t m = 1ull << (bits - 1);
    return (int64_t)((v ^ m) - m);
}

// One integer or SIMD load/store of `bytes` at `addr`: register `rt`,
// `simd` for a V register, `sign` the load's sign extension width in bits
// (0: zero-extend; 32: to a 32-bit result; 64: to 64 bits).
static bool do_access(ucontext_t *uc, uint64_t addr, unsigned bytes, unsigned rt,
                      bool load, bool simd, unsigned sign)
{
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    _STRUCT_ARM_NEON_STATE64 *ns = &uc->uc_mcontext->__ns;
    uint8_t buf[16] = {0};
    if (load) {
        if (!lp_access(addr, buf, bytes, false))
            return false;
        if (simd) {
            __uint128_t v = 0;
            memcpy(&v, buf, bytes);
            ns->__v[rt] = v;
        } else {
            uint64_t v = 0;
            memcpy(&v, buf, bytes);
            if (sign == 64) v = (uint64_t)sext(v, bytes * 8);
            else if (sign == 32) v = (uint32_t)sext(v, bytes * 8);
            set_x(ss, rt, v);
        }
        return true;
    }
    if (simd) {
        __uint128_t v = ns->__v[rt];
        memcpy(buf, &v, bytes);
    } else {
        uint64_t v = get_x(ss, rt);
        memcpy(buf, &v, bytes);
    }
    return lp_access(addr, buf, bytes, true);
}

// The fault at `fa` with the instruction `insn` at the context's pc. True
// when it was carried out and the pc advanced.
static bool emulate(ucontext_t *uc, uint64_t fa, uint32_t insn, const char **why)
{
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    unsigned size = insn >> 30, rt = insn & 31, rn = (insn >> 5) & 31;
    bool simd = (insn & (1u << 26)) != 0;
    (void)fa;

    // Load/store pair: 101 V 0 opc2 L imm7 Rt2 Rn Rt (opc2: 001 post, 010 offset, 011 pre, 000 no-allocate)
    if ((insn & 0x3A000000) == 0x28000000) {
        unsigned opc = size, opc2 = (insn >> 23) & 7;
        bool load = (insn & (1u << 22)) != 0;
        unsigned rt2 = (insn >> 10) & 31;
        int64_t imm = sext((insn >> 15) & 0x7F, 7);
        unsigned bytes, sign = 0;
        if (simd) {
            if (opc == 3) { *why = "ldp/stp simd opc 3"; return false; }
            bytes = 4u << opc;
        } else if (opc == 0) bytes = 4;
        else if (opc == 2) bytes = 8;
        else if (opc == 1 && load) { bytes = 4; sign = 64; }   // ldpsw
        else { *why = "ldp/stp opc"; return false; }
        if (opc2 == 0 || opc2 == 1 || opc2 == 2 || opc2 == 3) {
            uint64_t base = get_base(ss, rn);
            uint64_t addr = (opc2 == 1) ? base : base + (uint64_t)(imm * (int64_t)bytes);
            if (!do_access(uc, addr, bytes, rt, load, simd, sign)) { *why = "pair access"; return false; }
            if (!do_access(uc, addr + bytes, bytes, rt2, load, simd, sign)) { *why = "pair access 2"; return false; }
            if (opc2 == 1 || opc2 == 3)
                set_base(ss, rn, base + (uint64_t)(imm * (int64_t)bytes));
            return true;
        }
        *why = "ldp/stp form";
        return false;
    }

    // Load/store register: x x 111 V 0 x ... (group A: [25:24]=01 unsigned imm;
    // group B/C: [25:24]=00 with [21]=0 imm9 forms, [21]=1 register offset)
    if ((insn & 0x0A000000) == 0x08000000 && ((insn >> 27) & 7) == 7) {
        unsigned opc = (insn >> 22) & 3;
        unsigned group = (insn >> 24) & 3;
        unsigned bytes, sign = 0;
        bool load;
        if (simd) {
            // size:opc -> b (00:00/01), h (01), s (10), d (11), q (00:10/11)
            if (size == 0 && (opc & 2)) bytes = 16;
            else bytes = 1u << size;
            load = (opc & 1) != 0;
        } else {
            bytes = 1u << size;
            if (size == 3 && opc == 2) {              // prfm: nothing to do
                __darwin_arm_thread_state64_set_pc_fptr(*ss, (void *)(uintptr_t)(__darwin_arm_thread_state64_get_pc(*ss) + 4));
                return true;
            }
            if (opc == 0) load = false;
            else if (opc == 1) load = true;
            else if (opc == 2) { load = true; sign = 64; if (size == 3) { *why = "opc 2 size 3"; return false; } }
            else { load = true; sign = 32; if (size >= 2) { *why = "opc 3 size"; return false; } }
        }
        unsigned scale = simd && bytes == 16 ? 4 : size;
        uint64_t base = get_base(ss, rn), addr;
        unsigned wb = 0;   // 1: post-index, 2: pre-index
        int64_t imm = 0;
        if (group == 1) {
            addr = base + (((insn >> 10) & 0xFFF) << scale);
        } else if (group == 0 && !(insn & (1u << 21))) {
            imm = sext((insn >> 12) & 0x1FF, 9);
            unsigned idx = (insn >> 10) & 3;
            if (idx == 1) { addr = base; wb = 1; }
            else if (idx == 3) { addr = base + (uint64_t)imm; wb = 2; }
            else addr = base + (uint64_t)imm;            // unscaled (0) and unprivileged (2)
        } else if (group == 0 && (insn & (1u << 21)) && ((insn >> 10) & 3) == 2) {
            unsigned rm = (insn >> 16) & 31, option = (insn >> 13) & 7, s = (insn >> 12) & 1;
            uint64_t m = get_x(ss, rm);
            uint64_t off;
            switch (option) {
            case 2: off = (uint32_t)m; break;                  // uxtw
            case 3: off = m; break;                            // lsl
            case 6: off = (uint64_t)(int64_t)(int32_t)m; break; // sxtw
            case 7: off = m; break;                            // sxtx
            default: *why = "register offset option"; return false;
            }
            addr = base + (s ? off << scale : off);
        } else {
            *why = "load/store form";
            return false;
        }
        if (!do_access(uc, addr, bytes, rt, load, simd, sign)) { *why = "access"; return false; }
        if (wb == 1) set_base(ss, rn, base + (uint64_t)imm);
        else if (wb == 2) set_base(ss, rn, addr);
        return true;
    }

    // Load-acquire / store-release: size 001000 1 L 0 1 11111 1 11111 Rn Rt
    if ((insn & 0x3FFFFC00) == 0x089FFC00 || (insn & 0x3FFFFC00) == 0x08DFFC00) {
        bool load = (insn & (1u << 22)) != 0;
        unsigned bytes = 1u << size;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (!do_access(uc, get_base(ss, rn), bytes, rt, load, false, 0)) { *why = "acquire/release access"; return false; }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return true;
    }
    // RCpc loads and stores (FEAT_LRCPC / FEAT_LRCPC2): LDAPR Rt, [Rn] and
    // LDAPUR*/STLUR Rt, [Rn, #imm9]. FEX emits them for x86 loads and stores
    // under TSO once the ID registers say the core has them (stack.c
    // HWCAP_CPUID): an x64 game reading KUSER_SHARED_DATA did
    // `ldapurb w6, [x6]` on this page, and it went to the guest as a fault.
    if ((insn & 0x3FFFFC00) == 0x38BFC000) {                    // LDAPR{B,H,,}
        unsigned bytes = 1u << size;
        if (!do_access(uc, get_base(ss, rn), bytes, rt, true, false, 0)) { *why = "ldapr access"; return false; }
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return true;
    }
    if ((insn & 0x3F200C00) == 0x19000000) {                    // STLUR / LDAPUR / LDAPURS
        unsigned opc = (insn >> 22) & 3, bytes = 1u << size, sign = 0;
        bool load = opc != 0;
        if (opc == 2) { if (size == 3) { *why = "ldapurs size 3"; return false; } sign = 64; }
        if (opc == 3) { if (size >= 2) { *why = "ldapurs size"; return false; } sign = 32; }
        uint64_t addr = get_base(ss, rn) + (uint64_t)sext((insn >> 12) & 0x1FF, 9);
        if (!load) __atomic_thread_fence(__ATOMIC_SEQ_CST);
        if (!do_access(uc, addr, bytes, rt, load, false, sign)) { *why = "rcpc access"; return false; }
        if (load) __atomic_thread_fence(__ATOMIC_SEQ_CST);
        return true;
    }
    *why = "instruction";
    return false;
}

// libsystem_platform's text, where Darwin's signal trampoline lives.
static uint64_t g_plat_lo, g_plat_hi;
__attribute__((constructor)) static void find_platform_text(void)
{
    Dl_info di;
    if (!dladdr((void *)(uintptr_t)longjmp, &di) || !di.dli_fbase || !di.dli_fname ||
        !strstr(di.dli_fname, "libsystem_platform"))
        return;
    const struct mach_header_64 *mh = di.dli_fbase;
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t k = 0; k < mh->ncmds; k++, lc = (const void *)((const char *)lc + lc->cmdsize))
        if (lc->cmd == LC_SEGMENT_64 && !strcmp(((const struct segment_command_64 *)lc)->segname, "__TEXT")) {
            g_plat_lo = (uint64_t)(uintptr_t)mh;
            g_plat_hi = g_plat_lo + ((const struct segment_command_64 *)lc)->vmsize;
        }
}

bool lxrt_lowpage_fault(const siginfo_t *si, void *uap)
{
    if (!lxrt_lowpage_on() || !si || !uap)
        return false;
    uint64_t fa = (uint64_t)(uintptr_t)si->si_addr;
    if (fa < LP_LO || fa >= LP_HI)
        return false;
    if (!atomic_load(&g_pages[(fa - LP_LO) / LP_PAGE].host))
        return false;                                   // not mapped: a real fault
    ucontext_t *uc = uap;
    _STRUCT_ARM_THREAD_STATE64 *ss = &uc->uc_mcontext->__ss;
    uint64_t pc = __darwin_arm_thread_state64_get_pc(*ss);
    uint32_t insn;
    memcpy(&insn, (const void *)(uintptr_t)pc, 4);
    const char *why = "";
    // The fault arrived with the thread already redirected into Darwin's
    // signal trampoline: an asynchronous signal (Wine suspends threads with
    // SIGUSR1, and Minecraft Dungeons II's integrity scanner suspends every
    // thread of the game over and over) was set up first, and the context
    // handed here is that frame's, not the faulting instruction's. Nothing
    // to emulate at the trampoline: let it run; its sigreturn goes back to
    // the access, which faults again with its own pc and is carried out
    // then. Passed to the guest instead, the game died with a fault in no
    // code of its own (MEASURED, once in a 7-minute run; benchmarks/stage62, 15).
    {
        // _sigtramp is not exported: the image is the test. Darwin's
        // libsystem_platform holds the trampoline, and no guest load ever
        // runs from it (range found at load time, below: dladdr is no call
        // for a signal handler).
        if (pc >= g_plat_lo && pc < g_plat_hi)
            return true;
    }
    if (!emulate(uc, fa, insn, &why)) {
        static _Atomic int said;
        if (atomic_fetch_add(&said, 1) < 8)
            fprintf(lxrt_trace_stream(), "[lxrt] lowpage: cannot carry out %08x at pc 0x%llx for 0x%llx (%s): the fault goes to the guest\n",
                    insn, (unsigned long long)pc, (unsigned long long)fa, why);
        return false;
    }
    // prfm advanced the pc itself
    if (__darwin_arm_thread_state64_get_pc(*ss) == pc)
        __darwin_arm_thread_state64_set_pc_fptr(*ss, (void *)(uintptr_t)(pc + 4));
    unsigned long n = atomic_fetch_add(&g_faults, 1) + 1;
    if (lxrt_trace_on() && (n <= 4 || (n % 100000) == 0))
        fprintf(lxrt_trace_stream(), "[lxrt] lowpage: access #%lu %08x at pc 0x%llx for 0x%llx carried out\n",
                n, insn, (unsigned long long)pc, (unsigned long long)fa);
    return true;
}

unsigned long lxrt_lowpage_faults(void) { return atomic_load(&g_faults); }
