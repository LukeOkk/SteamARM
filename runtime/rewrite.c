// Syscall-site rewriting.
//
// Stage 1 established that `svc` cannot be trapped on Darwin: XNU ignores the
// SVC immediate and dispatches on x16, so a Linux `svc #0` does not fault --
// it runs whatever Darwin syscall x16 happens to hold. A site that this pass
// misses is therefore *silent misbehaviour*, not a crash. That asymmetry
// drives every decision here: when in doubt, fail loudly.
//
// Two entry points, because code reaches the process two ways:
//   lxrt_rewrite_image  -- the image the runtime loaded itself;
//   lxrt_rewrite_range  -- code the guest's own dynamic loader mapped, which
//                          never passes through our ELF loader at all.
//
// LIMITATION, stated plainly because it is the main correctness risk in the
// whole design: this is a linear scan for the 32-bit pattern of `svc #0`. It
// cannot distinguish an instruction from a literal pool entry that happens to
// hold the same bits. Validated against a full disassembly of real glibc and
// ld-linux with exact agreement (benchmarks/stage2-rewrite-validation.txt),
// but validation is not proof: a disassembler-driven walk is the correct
// long-term implementation.

#include "lxrt.h"
#include "x18.h"
bool lxrt_trace_on(void);

#include <errno.h>
#include <libkern/OSCacheControl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <sys/mman.h>

// Instruction encodings, from the ARM ARM.
#define INSN_SVC0  0xD4000001u  // svc #0
#define INSN_HVC0  0xD4000002u  // hvc #0
#define INSN_SMC0  0xD4000003u  // smc #0
#define INSN_BRK1  0xD4000021u  // brk #1

// mrs Xt, TPIDR_EL0 / msr TPIDR_EL0, Xt -- Rt in bits [4:0].
#define INSN_MRS_TPIDR 0xD53BD040u
#define INSN_MSR_TPIDR 0xD51BD040u
// mrs Xt, CTR_EL0. Darwin traps this at EL0 even for its own code, and every
// icache flush does one -- see runtime/sysreg.c.
#define INSN_MRS_CTR   0xD53B0020u
// mrs Xt, <op0=3, op1=0, CRn=0, ...> -- the whole ID register space, MIDR_EL1
// included. Darwin traps every one of them at EL0.
#define INSN_MRS_ID      0xD5380000u
#define INSN_MRS_ID_MASK 0xFFFFF000u
#define SYSREG_MASK    0xFFFFFFE0u

// B <label>: 0b000101 || imm26, imm26 = offset >> 2, signed, +/-128 MiB.
#define B_RANGE (1LL << 27)

extern const uint32_t lxrt_tramp_template[];
extern const uint64_t lxrt_tramp_size;
extern const uint64_t lxrt_tramp_lit_off;
extern const uint64_t lxrt_tramp_br_off;
extern const uint64_t lxrt_tramp_ret_off;

extern const uint32_t lxrt_tlsrd_template[];
extern const uint64_t lxrt_tlsrd_size, lxrt_tlsrd_ldr_off, lxrt_tlsrd_br_off;
extern const uint32_t lxrt_tlswr_template[];
extern const uint64_t lxrt_tlswr_size, lxrt_tlswr_str_off, lxrt_tlswr_br_off;
extern const uint32_t lxrt_ctr_template[];
extern const uint64_t lxrt_ctr_size, lxrt_ctr_br_off;
extern const uint32_t lxrt_sysreg_template[];
extern const uint64_t lxrt_sysreg_size, lxrt_sysreg_br_off;

// Field patching. The templates are assembled with x0 as the operand, so the
// real register number is ORed in rather than masked and replaced.
static inline uint32_t set_rt(uint32_t insn, unsigned rt) { return insn | (rt & 31u); }
static inline uint32_t set_rn(uint32_t insn, unsigned rn) { return insn | ((rn & 31u) << 5); }
static inline uint32_t set_imm12(uint32_t insn, unsigned scaled)
{
    return (insn & ~(0xFFFu << 10)) | ((scaled & 0xFFFu) << 10);
}

// movz/movk carry their immediate in bits [20:5].
static inline uint32_t set_imm16(uint32_t insn, unsigned imm)
{
    return (insn & ~(0xFFFFu << 5)) | ((imm & 0xFFFFu) << 5);
}

static struct lxrt_rewrite_report g_totals;

// Every pool handed out, so a later mapping over one can be recognised rather
// than silently corrupting live trampolines.
#define MAX_POOLS 1024
static struct { uint64_t start, end; } g_pools[MAX_POOLS];
static int g_npools;

#define MAX_ELF_GAPS 128
static struct { uint64_t next, end; } g_elf_gaps[MAX_ELF_GAPS];
static int g_nelf_gaps;

void lxrt_pool_offer_elf_gap(uint64_t start, uint64_t end)
{
    if (!start || end <= start || start % LXRT_HOST_PAGE ||
        end % LXRT_HOST_PAGE)
        return;
    for (int i = 0; i < g_nelf_gaps; i++)
        if (g_elf_gaps[i].next == start && g_elf_gaps[i].end == end)
            return;
    if (g_nelf_gaps < MAX_ELF_GAPS) {
        g_elf_gaps[g_nelf_gaps].next = start;
        g_elf_gaps[g_nelf_gaps].end = end;
        g_nelf_gaps++;
    }
}

bool lxrt_pool_contains(uint64_t addr)
{
    for (int i = 0; i < g_npools; i++)
        if (addr >= g_pools[i].start && addr < g_pools[i].end)
            return true;
    return false;
}

bool lxrt_pool_overlaps(uint64_t start, uint64_t end)
{
    for (int i = 0; i < g_npools; i++)
        if (start < g_pools[i].end && end > g_pools[i].start)
            return true;
    return false;
}

void lxrt_rewrite_totals(struct lxrt_rewrite_report *out) { *out = g_totals; }

static int fail(char **err, const char *fmt, ...)
{
    if (err) {
        char *buf = malloc(512);
        if (buf) {
            va_list ap;
            va_start(ap, fmt);
            vsnprintf(buf, 512, fmt, ap);
            va_end(ap);
        }
        *err = buf;
    }
    return -1;
}

static bool encode_b(uint64_t from, uint64_t to, uint32_t *out)
{
    int64_t delta = (int64_t)to - (int64_t)from;
    if (delta < -B_RANGE || delta >= B_RANGE || (delta & 3))
        return false;
    *out = 0x14000000u | (uint32_t)((delta >> 2) & 0x03FFFFFFu);
    return true;
}

static uint8_t *reserve_pool_at(uint64_t candidate, uint64_t range_start,
                                uint64_t range_end, size_t need, size_t *got)
{
    if (!candidate)
        return NULL;
    // VM_FLAGS_FIXED fails on an occupied range; MAP_FIXED would destroy guest
    // code or host data and cannot be used to probe for a free gap.
    mach_vm_address_t at = candidate;
    if (mach_vm_allocate(mach_task_self(), &at, need, VM_FLAGS_FIXED) != KERN_SUCCESS)
        return NULL;
    uint32_t tmp;
    if (!encode_b(range_start, at, &tmp) ||
        !encode_b(range_end, at + need, &tmp) ||
        mprotect((void *)at, need, PROT_READ | PROT_WRITE) != 0) {
        mach_vm_deallocate(mach_task_self(), at, need);
        return NULL;
    }
    if (g_npools < MAX_POOLS) {
        g_pools[g_npools].start = at;
        g_pools[g_npools].end = at + need;
        g_npools++;
    }
    *got = need;
    return (uint8_t *)at;
}

// A trampoline pool must sit within a single `b` of the code it serves, in
// both directions. The kernel places a guest's mappings wherever it likes, so
// the pool is placed per range rather than once per process.
static uint8_t *alloc_pool_near(uint64_t range_start, uint64_t range_end,
                                size_t need, size_t *got)
{
    need = (size_t)LXRT_ALIGN_UP(need + LXRT_HOST_PAGE, LXRT_HOST_PAGE);

    // Order matters. The page immediately after a mapping is frequently still
    // inside a span the guest's loader reserved and has not filled in yet, so
    // a pool placed there gets mapped over later. Start far away and only fall
    // back to adjacency.
    uint64_t candidates[18];
    int n = 0;
    for (int mb = 96; mb >= 16; mb -= 16) {
        candidates[n++] = LXRT_ALIGN_UP(range_end + ((uint64_t)mb << 20),
                                        LXRT_HOST_PAGE);
        uint64_t below = (uint64_t)((int64_t)range_start - ((int64_t)mb << 20));
        if (below < range_start) // no wrap
            candidates[n++] = LXRT_ALIGN_DOWN(below, LXRT_HOST_PAGE);
    }
    candidates[n++] = LXRT_ALIGN_UP(range_end, LXRT_HOST_PAGE);

    for (int i = 0; i < n; i++) {
        uint8_t *pool = reserve_pool_at(candidates[i], range_start,
                                        range_end, need, got);
        if (pool)
            return pool;
    }

    // A very large ELF (libcef.so has a 162 MiB executable segment) may fill
    // every 16 MiB probe point while leaving a smaller linker gap in branch
    // range. Walk the VM map to find that gap rather than choosing a distant
    // pool that forces us to poison valid guest instructions.
    uint64_t lo = range_end > B_RANGE ? range_end - B_RANGE : 0;
    uint64_t hi = range_start <= UINT64_MAX - B_RANGE ? range_start + B_RANGE
                                                       : UINT64_MAX;
    uint64_t cursor = LXRT_ALIGN_UP(lo, LXRT_HOST_PAGE);
    for (int i = 0; i < 1024 && cursor < hi; i++) {
        mach_vm_address_t region = cursor;
        mach_vm_size_t size = 0;
        vm_region_basic_info_data_64_t info;
        mach_msg_type_number_t count = VM_REGION_BASIC_INFO_COUNT_64;
        mach_port_t object = MACH_PORT_NULL;
        kern_return_t kr = mach_vm_region(mach_task_self(), &region, &size,
                                          VM_REGION_BASIC_INFO_64,
                                          (vm_region_info_t)&info, &count,
                                          &object);
        if (object != MACH_PORT_NULL)
            mach_port_deallocate(mach_task_self(), object);
        uint64_t gap_end = kr == KERN_SUCCESS ? region : hi;
        if (gap_end > cursor && gap_end - cursor >= need) {
            uint8_t *pool = reserve_pool_at(cursor, range_start,
                                            range_end, need, got);
            if (pool)
                return pool;
        }
        if (kr != KERN_SUCCESS || region + size <= cursor || region + size >= hi)
            break;
        cursor = LXRT_ALIGN_UP(region + size, LXRT_HOST_PAGE);
    }

    // A large ELF can leave no *unmapped* space within branch range: ld.so
    // first reserves its full span, including holes between PT_LOADs. Those
    // holes contain no ELF data. The caller offers only host pages wholly
    // inside a program-header-proven hole, so borrowing one is safe.
    for (int i = 0; i < g_nelf_gaps; i++) {
        uint64_t at = g_elf_gaps[i].next;
        uint32_t tmp;
        if (g_elf_gaps[i].end - at < need || g_npools >= MAX_POOLS ||
            !encode_b(range_start, at, &tmp) ||
            !encode_b(range_end, at + need, &tmp) ||
            mprotect((void *)at, need, PROT_READ | PROT_WRITE) != 0)
            continue;
        g_elf_gaps[i].next += need;
        g_pools[g_npools].start = at;
        g_pools[g_npools].end = at + need;
        g_npools++;
        *got = need;
        if (lxrt_trace_on())
            fprintf(lxrt_trace_stream(),
                    "[lxrt] trampoline pool in ELF load gap 0x%llx+0x%zx\n",
                    (unsigned long long)at, need);
        return (uint8_t *)at;
    }

    // Nothing near enough was free. Let the kernel choose: encode_b at the call
    // site will poison any site it cannot reach from there.
    void *p = mmap(NULL, need, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON,
                   -1, 0);
    if (p == MAP_FAILED)
        return NULL;
    if (g_npools < MAX_POOLS) {
        g_pools[g_npools].start = (uint64_t)p;
        g_pools[g_npools].end = (uint64_t)p + need;
        g_npools++;
    }
    *got = need;
    return p;
}

// Which kind of site, if any, a word is. Rt is only meaningful for the TLS
// kinds.
enum site_kind { SITE_NONE, SITE_SVC, SITE_TLS_READ, SITE_TLS_WRITE, SITE_CTR,
                 SITE_SYSREG };

static enum site_kind classify(uint32_t insn, unsigned *rt)
{
    if (insn == INSN_SVC0)
        return SITE_SVC;
    if ((insn & SYSREG_MASK) == INSN_MRS_TPIDR) {
        *rt = insn & 31u;
        // `mrs xzr, ...` discards the value; rewriting it would turn register
        // 31 into SP in the load below. It is already a no-op, so leave it.
        return *rt == 31 ? SITE_NONE : SITE_TLS_READ;
    }
    if ((insn & SYSREG_MASK) == INSN_MSR_TPIDR) {
        *rt = insn & 31u;
        return SITE_TLS_WRITE;
    }
    if ((insn & SYSREG_MASK) == INSN_MRS_CTR) {
        *rt = insn & 31u;
        return *rt == 31 ? SITE_NONE : SITE_CTR;
    }
    if ((insn & INSN_MRS_ID_MASK) == INSN_MRS_ID) {
        *rt = insn & 31u;
        return *rt == 31 ? SITE_NONE : SITE_SYSREG;
    }
    return SITE_NONE;
}

// Returns the total trampoline bytes the range needs.
// Is this word inside one of the executable-section windows? The x18 pass
// runs nowhere else: outside .text a word naming register 18 is data.
static bool in_code(uint64_t addr, const struct lxrt_range *code, int ncode)
{
    for (int i = 0; i < ncode; i++)
        if (addr >= code[i].start && addr + 4 <= code[i].end)
            return true;
    return false;
}

static bool x18_site(uint64_t addr, uint32_t insn, const struct lxrt_range *code, int ncode)
{
    static int off = -1;                       // LXRT_NO_X18=1: diagnostic, no x18 pass
    if (off < 0) off = getenv("LXRT_NO_X18") != NULL;
    extern bool lxrt_elf_in_function(uint64_t addr);
    return !off && ncode > 0 && lxrt_x18_enabled() && in_code(addr, code, ncode) &&
           lxrt_x18_touches(insn) && lxrt_elf_in_function(addr);
}

static size_t count_sites(uint64_t start, uint64_t end,
                          struct lxrt_rewrite_report *rep,
                          const struct lxrt_range *code, int ncode)
{
    const uint32_t *w = (const uint32_t *)start;
    size_t words = (size_t)(end - start) / 4;
    size_t bytes = 0;
    rep->scanned_words += words;
    for (size_t i = 0; i < words; i++) {
        unsigned rt = 0;
        if (x18_site((uint64_t)&w[i], w[i], code, ncode)) {
            // Takes precedence over the TLS kinds: `mrs x18, TPIDR_EL0` is
            // planned as an x18 site that reads the TLS slot.
            rep->x18_found++;
            bytes += lxrt_x18_tramp_bytes(w[i]) + 8;
            continue;
        }
        switch (classify(w[i], &rt)) {
        case SITE_SVC:
            rep->sites_found++;
            bytes += (size_t)lxrt_tramp_size;
            break;
        case SITE_TLS_READ:
            rep->tls_read_found++;
            bytes += (size_t)lxrt_tlsrd_size;
            break;
        case SITE_TLS_WRITE:
            rep->tls_write_found++;
            bytes += (size_t)lxrt_tlswr_size;
            break;
        case SITE_CTR:
            rep->ctr_found++;
            bytes += (size_t)lxrt_ctr_size;
            break;
        case SITE_SYSREG:
            rep->sysreg_found++;
            bytes += (size_t)lxrt_sysreg_size;
            break;
        case SITE_NONE:
            if (w[i] == INSN_HVC0 || w[i] == INSN_SMC0)
                rep->hvc_or_smc_found++;
            break;
        }
    }
    return bytes;
}

// Add every field of one report into another. Written as a single function so
// a new counter cannot be added to the struct and silently dropped here -- an
// earlier version summed only the svc fields and reported zero TLS sites on a
// run that had rewritten hundreds of them.
static void report_add(struct lxrt_rewrite_report *dst,
                       const struct lxrt_rewrite_report *r)
{
    dst->scanned_words += r->scanned_words;
    dst->sites_found += r->sites_found;
    dst->sites_rewritten += r->sites_rewritten;
    dst->sites_unreachable += r->sites_unreachable;
    dst->hvc_or_smc_found += r->hvc_or_smc_found;
    dst->tls_read_found += r->tls_read_found;
    dst->tls_write_found += r->tls_write_found;
    dst->tls_rewritten += r->tls_rewritten;
    dst->tls_unreachable += r->tls_unreachable;
    dst->ctr_found += r->ctr_found;
    dst->ctr_rewritten += r->ctr_rewritten;
    dst->sysreg_found += r->sysreg_found;
    dst->sysreg_rewritten += r->sysreg_rewritten;
    dst->x18_found += r->x18_found;
    dst->x18_rewritten += r->x18_rewritten;
    dst->x18_unsupported += r->x18_unsupported;
    dst->x18_unreachable += r->x18_unreachable;
}

static void accumulate(const struct lxrt_rewrite_report *r)
{
    g_totals.scanned_words += r->scanned_words;
    g_totals.sites_found += r->sites_found;
    g_totals.sites_rewritten += r->sites_rewritten;
    g_totals.sites_unreachable += r->sites_unreachable;
    g_totals.hvc_or_smc_found += r->hvc_or_smc_found;
    g_totals.tls_read_found += r->tls_read_found;
    g_totals.tls_write_found += r->tls_write_found;
    g_totals.tls_rewritten += r->tls_rewritten;
    g_totals.tls_unreachable += r->tls_unreachable;
    g_totals.ctr_found += r->ctr_found;
    g_totals.ctr_rewritten += r->ctr_rewritten;
    g_totals.sysreg_found += r->sysreg_found;
    g_totals.sysreg_rewritten += r->sysreg_rewritten;
    g_totals.x18_found += r->x18_found;
    g_totals.x18_rewritten += r->x18_rewritten;
    g_totals.x18_unsupported += r->x18_unsupported;
    g_totals.x18_unreachable += r->x18_unreachable;
}

int lxrt_rewrite_range(uint64_t start, uint64_t end,
                       struct lxrt_rewrite_report *rep, char **err)
{
    return lxrt_rewrite_range_code(start, end, NULL, 0, rep, err);
}

static int rewrite_chunk_code(uint64_t start, uint64_t end,
                              const struct lxrt_range *code, int ncode,
                              struct lxrt_rewrite_report *rep, char **err)
{
    memset(rep, 0, sizeof(*rep));
    if (end <= start)
        return 0;

    size_t need = count_sites(start, end, rep, code, ncode);
    if (need == 0) {
        accumulate(rep);
        return 0;
    }

    size_t pool_size = 0;
    uint8_t *pool = alloc_pool_near(start, end, need, &pool_size);

    extern void lxrt_dispatch(struct lxrt_regs *);
    uint64_t dispatch_addr = (uint64_t)(void *)lxrt_dispatch;

    uint32_t *w = (uint32_t *)start;
    size_t words = (size_t)(end - start) / 4;
    size_t used = 0;

    unsigned tls_scaled = (unsigned)(lxrt_tls_slot_offset() / 8);

    unsigned x18_slot = (unsigned)lxrt_x18_slot_offset();
    unsigned tls_slot = (unsigned)lxrt_tls_slot_offset();

    for (size_t i = 0; i < words; i++) {
        unsigned rt = 0;
        uint64_t site = (uint64_t)&w[i];
        if (x18_site(site, w[i], code, ncode)) {
            uint32_t original = w[i];
            struct x18_plan plan;
            uint64_t tramp = pool ? (uint64_t)(pool + used) : 0;
            lxrt_x18_plan(w[i], site, tramp, x18_slot, tls_slot, &plan);
            uint32_t site_insn = 0, back_insn = 0, alt_insn = 0;
            bool fits = pool && plan.verdict == X18_OK &&
                        used + (size_t)plan.nwords * 4 <= pool_size &&
                        (plan.terminal ||
                         (plan.back_idx >= 0 && plan.back_idx < plan.nwords &&
                          encode_b(tramp + (uint64_t)plan.back_idx * 4,
                                   site + 4, &back_insn))) &&
                        encode_b(site, tramp, &site_insn) &&
                        (plan.alt_idx < 0 ||
                         encode_b(tramp + (uint64_t)plan.alt_idx * 4, plan.alt_target, &alt_insn));
            if (plan.verdict == X18_UNSUPPORTED) {
                // Refused forms are poisoned, never left live: a live x18 use
                // is silent corruption on the next context switch.
                w[i] = INSN_BRK1;
                rep->x18_unsupported++;
                if (lxrt_trace_on())
                    fprintf(lxrt_trace_stream(), "[lxrt]    x18 site 0x%llx (%08x) unsupported: %s\n",
                            (unsigned long long)site, original, plan.why ? plan.why : "?");
                continue;
            }
            if (!fits) {
                w[i] = INSN_BRK1;
                rep->x18_unreachable++;
                continue;
            }
            uint32_t *t = (uint32_t *)(pool + used);
            memcpy(t, plan.words, (size_t)plan.nwords * 4);
            if (plan.back_idx >= 0)
                t[plan.back_idx] = back_insn;
            if (plan.alt_idx >= 0)
                t[plan.alt_idx] = alt_insn;
            w[i] = site_insn;
            used += (size_t)plan.nwords * 4;
            rep->x18_rewritten++;
            continue;
        }
        enum site_kind kind = classify(w[i], &rt);
        if (kind == SITE_NONE)
            continue;
        if ((kind == SITE_TLS_READ || kind == SITE_TLS_WRITE) && !lxrt_tls_ready()) {
            // No TSD slot: leave the instruction alone. That is wrong -- the
            // guest will lose its thread pointer on the first context switch --
            // but it is what LXRT_NO_TLS_REWRITE exists to demonstrate, and
            // tls.c has already said so on stderr.
            continue;
        }

        size_t tsize = kind == SITE_SVC       ? (size_t)lxrt_tramp_size
                     : kind == SITE_TLS_READ  ? (size_t)lxrt_tlsrd_size
                     : kind == SITE_TLS_WRITE ? (size_t)lxrt_tlswr_size
                     : kind == SITE_CTR       ? (size_t)lxrt_ctr_size
                                              : (size_t)lxrt_sysreg_size;
        size_t br_off = kind == SITE_SVC      ? (size_t)lxrt_tramp_br_off
                      : kind == SITE_TLS_READ ? (size_t)lxrt_tlsrd_br_off
                      : kind == SITE_TLS_WRITE ? (size_t)lxrt_tlswr_br_off
                      : kind == SITE_CTR       ? (size_t)lxrt_ctr_br_off
                                              : (size_t)lxrt_sysreg_br_off;

        uint64_t tramp = pool ? (uint64_t)(pool + used) : 0;
        uint32_t site_insn, back_insn;

        if (!pool || used + tsize > pool_size ||
            !encode_b(site, tramp, &site_insn) ||
            !encode_b(tramp + br_off, site + 4, &back_insn)) {
            if (kind == SITE_SVC && lxrt_trace_on() && rep->sites_unreachable < 3)
                fprintf(lxrt_trace_stream(),
                        "[lxrt] svc pool unreachable: site 0x%llx pool 0x%llx used %zu/%zu size %zu\n",
                        (unsigned long long)site, (unsigned long long)tramp,
                        used, pool_size, tsize);
            // No reachable trampoline. Poison rather than leave the original:
            // per Stage 1 a live `svc` runs an arbitrary Darwin syscall, and a
            // live `mrs TPIDR_EL0` reads a register Darwin clobbers. Both are
            // silent. `brk #1` raises a catchable SIGTRAP instead.
            w[i] = INSN_BRK1;
            if (kind == SITE_SVC)
                rep->sites_unreachable++;
            else
                rep->tls_unreachable++;
            continue;
        }

        uint8_t *tr = pool + used;
        if (kind == SITE_SVC) {
            memcpy(tr, lxrt_tramp_template, tsize);
            *(uint64_t *)(tr + lxrt_tramp_lit_off) = dispatch_addr;
            *(uint64_t *)(tr + lxrt_tramp_ret_off) = site + 4;
            rep->sites_rewritten++;
        } else if (kind == SITE_TLS_READ) {
            memcpy(tr, lxrt_tlsrd_template, tsize);
            uint32_t *t = (uint32_t *)tr;
            t[0] = set_rt(t[0], rt);                       // mrs Xt, tpidrro_el0
            t[1] = set_rt(set_rn(t[1], rt), rt);           // and Xt, Xt, #~7
            uint32_t *ldr = (uint32_t *)(tr + lxrt_tlsrd_ldr_off);
            *ldr = set_imm12(set_rt(set_rn(*ldr, rt), rt), tls_scaled);
            rep->tls_rewritten++;
        } else if (kind == SITE_TLS_WRITE) {
            memcpy(tr, lxrt_tlswr_template, tsize);
            uint32_t *st = (uint32_t *)(tr + lxrt_tlswr_str_off);
            *st = set_imm12(set_rt(*st, rt), tls_scaled);  // base stays x16
            rep->tls_rewritten++;
        } else if (kind == SITE_CTR) {
            memcpy(tr, lxrt_ctr_template, tsize);
            uint32_t *t = (uint32_t *)tr;
            uint32_t ctr = lxrt_synthetic_ctr_el0();
            t[0] = set_imm16(set_rt(t[0], rt), ctr & 0xFFFFu);        // movz
            t[1] = set_imm16(set_rt(t[1], rt), (ctr >> 16) & 0xFFFFu); // movk
            rep->ctr_rewritten++;
        } else {
            memcpy(tr, lxrt_sysreg_template, tsize);
            uint32_t *t = (uint32_t *)tr;
            uint64_t v = lxrt_synthetic_sysreg(w[i]);
            for (int q = 0; q < 4; q++)
                t[q] = set_imm16(set_rt(t[q], rt),
                                 (unsigned)((v >> (16 * q)) & 0xFFFFu));
            rep->sysreg_rewritten++;
        }
        *(uint32_t *)(tr + br_off) = back_insn;
        w[i] = site_insn;

        used += tsize;
    }

    if (pool) {
        if (mprotect(pool, pool_size, PROT_READ | PROT_EXEC) != 0)
            return fail(err, "seal trampoline pool: %s", strerror(errno));
        sys_icache_invalidate(pool, pool_size);
    }
    sys_icache_invalidate((void *)start, (size_t)(end - start));
    accumulate(rep);
    return 0;
}

int lxrt_rewrite_range_code(uint64_t start, uint64_t end,
                            const struct lxrt_range *code, int ncode,
                            struct lxrt_rewrite_report *rep, char **err)
{
    // AArch64's direct branch reaches only +/-128 MiB. CEF has a single
    // executable PT_LOAD larger than that; one trampoline pool cannot serve
    // both ends, so the old whole-segment scan poisoned its TLS/syscall sites.
    // Give each 32 MiB slice its own nearby pool. Code-section ranges remain
    // in absolute guest addresses and can be shared by all slices.
    const uint64_t chunk_size = 32ull << 20;
    memset(rep, 0, sizeof(*rep));
    for (uint64_t pos = start; pos < end;) {
        uint64_t next = end - pos > chunk_size ? pos + chunk_size : end;
        struct lxrt_rewrite_report one;
        if (rewrite_chunk_code(pos, next, code, ncode, &one, err) != 0)
            return -1;
        report_add(rep, &one);
        pos = next;
    }
    return 0;
}

int lxrt_rewrite_image(struct lxrt_image *img, struct lxrt_rewrite_report *rep,
                       char **err)
{
    memset(rep, 0, sizeof(*rep));
    for (int s = 0; s < img->nexec; s++) {
        struct lxrt_rewrite_report one;
        if (lxrt_rewrite_range_code(img->exec[s].start, img->exec[s].end,
                                    img->code, img->ncode, &one, err) != 0)
            return -1;
        report_add(rep, &one);
    }
    return 0;
}
