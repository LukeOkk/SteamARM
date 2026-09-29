// Perform one AArch64 store instruction on behalf of the guest.
//
// Why this exists. A 16 KiB host page that holds both a guest code page and a
// writable guest data page (4 KiB-aligned images and libraries, subpage.c) is
// read-write or read-execute, never both: Darwin refuses write+execute
// outside MAP_JIT. A store executed from that page INTO that page cannot
// complete natively -- the store needs write, the fetch of the store itself
// needs execute -- and the fault handler flipped the page between the two at
// the same pc forever (MEASURED: tests/elf/subpage4k.S "selfwrite", an endless
// pair of write/exec flips at one pc). So the fault handler performs that one
// store itself: this file decodes it and carries it out while the thread is
// parked in the handler, with the page opened for writing by the caller and
// closed again before the thread resumes at pc + 4. The host page is never
// writable and executable at the same time.
//
// Covered: every A64 base and SIMD&FP store -- STR/STRB/STRH/STUR/STTR
// (unsigned offset, unscaled, pre/post-index, register offset), STP/STNP
// (X, W, S, D, Q), ST1-ST4 (multiple and single structure, post-index by
// immediate or register) -- the ordered stores (STLR, STLLR, STLUR), the LSE
// atomics (LDADD/LDCLR/LDEOR/LDSET/LD[SU]MAX/LD[SU]MIN, SWP, CAS, CASP), the
// exclusive stores (STXR/STLXR/STXP/STLXP, below) and DC ZVA. Anything else is
// refused, and the caller keeps its previous behaviour.
//
// Atomicity: atomics and CAS are performed with host atomics on the same
// memory, so they stay atomic against other threads that access it natively.
// A plain store keeps per-element single-copy atomicity (aligned 1/2/4/8/16
// bytes). Every emulated access is bracketed by full barriers, which is at
// least as strong as any ordering the guest instruction asked for.
//
// Exclusive stores. The local exclusive monitor does not survive the fault
// (MEASURED on an M4, benchmarks/stage23: 64 of 64 STXR fail when a page
// fault falls between them and their LDXR, 0 of 64 without), so a native
// retry of the STXR would fail and the guest's retry loop would fault again. The STXR is performed instead as a compare-and-swap against the
// value its load-exclusive returned: the nearest preceding LDXR/LDAXR (LDXP/
// LDAXP) with the same size and base register, provided nothing between the
// two can have overwritten that value or the base (checked conservatively,
// word by word). That is how QEMU's user mode emulates LL/SC; like it, this
// accepts ABA (a value changed and changed back in between still succeeds).
// No such load found: refused.

#include "storemu.h"

#include <mach/mach.h>
#include <mach/mach_vm.h>
#include <string.h>
#include <sys/ucontext.h>

typedef _STRUCT_ARM_THREAD_STATE64 tstate;

// X registers as the ucontext holds them. r == 31 is XZR here; the _sp
// variants treat it as SP (a base register).
static uint64_t xr(const tstate *ss, unsigned r)
{
    if (r == 31) return 0;
    if (r == 29) return ss->__fp;
    if (r == 30) return ss->__lr;
    return ss->__x[r];
}
static uint64_t xr_sp(const tstate *ss, unsigned r) { return r == 31 ? ss->__sp : xr(ss, r); }
static void set_xr(tstate *ss, unsigned r, uint64_t v)
{
    if (r == 31) return;
    if (r == 29) ss->__fp = v;
    else if (r == 30) ss->__lr = v;
    else ss->__x[r] = v;
}
static void set_xr_sp(tstate *ss, unsigned r, uint64_t v)
{
    if (r == 31) ss->__sp = v;
    else set_xr(ss, r, v);
}

static inline uint32_t fld(uint32_t w, int hi, int lo) { return (w >> lo) & ((1u << (hi - lo + 1)) - 1); }
static inline int64_t sext(uint64_t v, int nbits) { return (int64_t)(v << (64 - nbits)) >> (64 - nbits); }
static inline uint64_t mask_n(unsigned n) { return n >= 8 ? ~0ull : (1ull << (8 * n)) - 1; }

static void put_le(uint8_t *dst, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        dst[i] = (uint8_t)(v >> (8 * i));
}
// Element e of esize bytes of a vector register (little-endian lanes).
static void put_lane(uint8_t *dst, __uint128_t v, unsigned e, unsigned esize)
{
    uint8_t b[16];
    memcpy(b, &v, 16);
    memcpy(dst, b + e * esize, esize);
}

static int refuse(const char **why, const char *what) { *why = what; return -1; }

static bool read_word(uint64_t a, uint32_t *w)
{
    mach_vm_size_t got = 0;
    return mach_vm_read_overwrite(mach_task_self(), a, 4, (mach_vm_address_t)(uintptr_t)w,
                                  &got) == KERN_SUCCESS && got == 4;
}

// Can instruction w, found between a load-exclusive and its store-exclusive,
// change register r or break the straight line? Conservative: every field that
// can name a destination counts, and every control transfer but a conditional
// branch not taken (execution reached the store) gives up.
static bool clobbers(uint32_t w, unsigned r)
{
    if ((w & 0x1c000000u) == 0x14000000u) {              // branches, system
        if ((w & 0xff000010u) == 0x54000000u) return false;  // B.cond
        if ((w & 0x7e000000u) == 0x34000000u) return false;  // CBZ/CBNZ
        if ((w & 0x7e000000u) == 0x36000000u) return false;  // TBZ/TBNZ
        if (w == 0xd503201fu || w == 0xd503203fu) return false;  // NOP, YIELD
        uint32_t b = w & 0xfffff0ffu;
        if (b == 0xd503309fu || b == 0xd50330bfu || b == 0xd50330dfu)
            return false;                                     // DSB, DMB, ISB
        return true;                                          // CLREX, BL, MRS, ...
    }
    if ((w & 31) == r)
        return true;
    if ((w & 0x0a000000u) == 0x08000000u &&                   // loads and stores
        (fld(w, 9, 5) == r || fld(w, 14, 10) == r || fld(w, 20, 16) == r))
        return true;
    return false;
}

// ------------------------------------------------ the LL/SC loop body
//
// Enough of A64 to run the few instructions a compiler puts between a
// load-exclusive and its store-exclusive (clang and GCC's atomic expansions:
// add/sub/logical/move/compare/select/bitfield/shift/multiply and the
// conditional branches out of the loop). No memory access: anything else is
// refused. Registers are a private copy; nothing reaches the thread until the
// caller commits it.
struct ireg {
    uint64_t x[31];
    uint64_t sp;
    uint32_t nzcv;          // N 8, Z 4, C 2, V 1
};

static uint64_t ird(const struct ireg *r, unsigned n, bool sp) { return n == 31 ? (sp ? r->sp : 0) : r->x[n]; }
static void iwr(struct ireg *r, unsigned n, uint64_t v, bool sf, bool sp)
{
    if (!sf)
        v &= 0xffffffffu;
    if (n == 31) {
        if (sp) r->sp = v;
        return;
    }
    r->x[n] = v;
}

static bool cond_holds(unsigned cond, uint32_t f)
{
    bool n = f & 8, z = f & 4, c = f & 2, v = f & 1, res;
    switch (cond >> 1) {
    case 0: res = z; break;
    case 1: res = c; break;
    case 2: res = n; break;
    case 3: res = v; break;
    case 4: res = c && !z; break;
    case 5: res = n == v; break;
    case 6: res = n == v && !z; break;
    default: res = true; break;
    }
    return ((cond & 1) && cond != 15) ? !res : res;
}

static uint64_t add_carry(uint64_t x, uint64_t y, unsigned c, bool sf, uint32_t *f)
{
    if (sf) {
        unsigned __int128 us = (unsigned __int128)x + y + c;
        __int128 ss = (__int128)(int64_t)x + (int64_t)y + c;
        uint64_t res = (uint64_t)us;
        *f = (uint32_t)(((res >> 63) << 3) | ((uint64_t)(res == 0) << 2) |
                        ((uint64_t)((unsigned __int128)res != us) << 1) |
                        (uint64_t)((__int128)(int64_t)res != ss));
        return res;
    }
    uint64_t us = (uint64_t)(uint32_t)x + (uint32_t)y + c;
    int64_t ss = (int64_t)(int32_t)x + (int32_t)y + c;
    uint32_t res = (uint32_t)us;
    *f = ((res >> 31) << 3) | ((uint32_t)(res == 0) << 2) | ((uint32_t)((uint64_t)res != us) << 1) |
         (uint32_t)((int64_t)(int32_t)res != ss);
    return res;
}

static uint32_t nz_of(uint64_t res, bool sf)
{
    return (uint32_t)((((sf ? res >> 63 : res >> 31) & 1) << 3) | ((uint64_t)(res == 0) << 2));
}

static uint64_t shifted(uint64_t v, unsigned type, unsigned amt, bool sf)
{
    unsigned size = sf ? 64 : 32;
    if (!sf)
        v &= 0xffffffffu;
    if (amt) {
        switch (type) {
        case 0: v <<= amt; break;
        case 1: v >>= amt; break;
        case 2: v = sf ? (uint64_t)((int64_t)v >> amt) : (uint64_t)(uint32_t)((int32_t)(uint32_t)v >> amt); break;
        default: v = (v >> amt) | (v << (size - amt)); break;
        }
    }
    return sf ? v : v & 0xffffffffu;
}

static uint64_t extended(uint64_t v, unsigned option, unsigned amt, bool sf)
{
    unsigned len = 8u << (option & 3);
    if (len < 64) {
        v &= (1ull << len) - 1;
        if (option & 4)
            v = (uint64_t)sext(v, (int)len);
    }
    v <<= amt;
    return sf ? v : v & 0xffffffffu;
}

// DecodeBitMasks from the ARM ARM.
static bool bit_masks(unsigned N, unsigned imms, unsigned immr, bool immediate, unsigned datasize,
                      uint64_t *wmask, uint64_t *tmask)
{
    unsigned combined = (N << 6) | (~imms & 0x3f);
    if (!combined)
        return false;
    int len = 31 - __builtin_clz(combined);
    if (len < 1)
        return false;
    unsigned levels = (1u << len) - 1;
    if (immediate && (imms & levels) == levels)
        return false;
    unsigned S = imms & levels, R = immr & levels, esize = 1u << len;
    unsigned d = ((S - R) & 0x3f) & levels;
    uint64_t emask = esize == 64 ? ~0ull : (1ull << esize) - 1;
    uint64_t welem = S + 1 == 64 ? ~0ull : (1ull << (S + 1)) - 1;
    uint64_t telem = d + 1 == 64 ? ~0ull : (1ull << (d + 1)) - 1;
    if (R)
        welem = ((welem >> R) | (welem << (esize - R))) & emask;
    uint64_t w = 0, t = 0;
    for (unsigned i = 0; i < 64; i += esize) {
        w |= welem << i;
        t |= telem << i;
    }
    if (datasize == 32) {
        w &= 0xffffffffu;
        t &= 0xffffffffu;
    }
    *wmask = w;
    if (tmask)
        *tmask = t;
    return true;
}

// One instruction. *next becomes the following pc. False: not interpreted.
static bool interp(uint32_t w, uint64_t pc, struct ireg *r, uint64_t *next)
{
    *next = pc + 4;
    bool sf = w >> 31;
    unsigned rd = w & 31, rn = fld(w, 9, 5), rm = fld(w, 20, 16);
    uint32_t f;

    if (w == 0xd503201fu || w == 0xd503203fu)                   // NOP, YIELD
        return true;
    uint32_t bar = w & 0xfffff0ffu;
    if (bar == 0xd503309fu || bar == 0xd50330bfu || bar == 0xd50330dfu)
        return true;                                            // DSB, DMB, ISB
    if ((w & 0xff000010u) == 0x54000000u) {                     // B.cond
        if (cond_holds(w & 15, r->nzcv))
            *next = pc + (uint64_t)(sext(fld(w, 23, 5), 19) * 4);
        return true;
    }
    if ((w & 0x7e000000u) == 0x34000000u) {                     // CBZ, CBNZ
        uint64_t v = ird(r, rd, false);
        if (!sf) v &= 0xffffffffu;
        if ((v != 0) == (bool)fld(w, 24, 24))
            *next = pc + (uint64_t)(sext(fld(w, 23, 5), 19) * 4);
        return true;
    }
    if ((w & 0x7e000000u) == 0x36000000u) {                     // TBZ, TBNZ
        unsigned bit = (fld(w, 31, 31) << 5) | fld(w, 23, 19);
        if ((bool)((ird(r, rd, false) >> bit) & 1) == (bool)fld(w, 24, 24))
            *next = pc + (uint64_t)(sext(fld(w, 18, 5), 14) * 4);
        return true;
    }
    if ((w & 0x1f800000u) == 0x11000000u) {                     // ADD/SUB(S) immediate
        bool op = fld(w, 30, 30), S = fld(w, 29, 29);
        uint64_t imm = (uint64_t)fld(w, 21, 10) << (fld(w, 22, 22) ? 12 : 0);
        uint64_t res = add_carry(ird(r, rn, true), op ? ~imm : imm, op, sf, &f);
        if (S) r->nzcv = f;
        iwr(r, rd, res, sf, !S);
        return true;
    }
    if ((w & 0x1f200000u) == 0x0b000000u) {                     // ADD/SUB(S) shifted register
        unsigned type = fld(w, 23, 22), amt = fld(w, 15, 10);
        if (type == 3 || (!sf && amt >= 32))
            return false;
        bool op = fld(w, 30, 30), S = fld(w, 29, 29);
        uint64_t y = shifted(ird(r, rm, false), type, amt, sf);
        uint64_t res = add_carry(ird(r, rn, false), op ? ~y : y, op, sf, &f);
        if (S) r->nzcv = f;
        iwr(r, rd, res, sf, false);
        return true;
    }
    if ((w & 0x1fe00000u) == 0x0b200000u) {                     // ADD/SUB(S) extended register
        unsigned amt = fld(w, 12, 10);
        if (amt > 4)
            return false;
        bool op = fld(w, 30, 30), S = fld(w, 29, 29);
        uint64_t y = extended(ird(r, rm, false), fld(w, 15, 13), amt, sf);
        uint64_t res = add_carry(ird(r, rn, true), op ? ~y : y, op, sf, &f);
        if (S) r->nzcv = f;
        iwr(r, rd, res, sf, !S);
        return true;
    }
    if ((w & 0x1fe0fc00u) == 0x1a000000u) {                     // ADC/SBC(S)
        bool op = fld(w, 30, 30), S = fld(w, 29, 29);
        uint64_t y = ird(r, rm, false);
        uint64_t res = add_carry(ird(r, rn, false), op ? ~y : y, (r->nzcv >> 1) & 1, sf, &f);
        if (S) r->nzcv = f;
        iwr(r, rd, res, sf, false);
        return true;
    }
    if ((w & 0x1f800000u) == 0x12000000u) {                     // logical immediate
        unsigned opc = fld(w, 30, 29);
        uint64_t imm, x = ird(r, rn, false), res;
        if ((!sf && fld(w, 22, 22)) ||
            !bit_masks(fld(w, 22, 22), fld(w, 15, 10), fld(w, 21, 16), true, sf ? 64 : 32, &imm, NULL))
            return false;
        res = opc == 1 ? x | imm : opc == 2 ? x ^ imm : x & imm;
        if (!sf) res &= 0xffffffffu;
        if (opc == 3) r->nzcv = nz_of(res, sf);
        iwr(r, rd, res, sf, opc != 3);
        return true;
    }
    if ((w & 0x1f000000u) == 0x0a000000u) {                     // logical shifted register
        unsigned opc = fld(w, 30, 29), amt = fld(w, 15, 10);
        if (!sf && amt >= 32)
            return false;
        uint64_t y = shifted(ird(r, rm, false), fld(w, 23, 22), amt, sf), x = ird(r, rn, false), res;
        if (fld(w, 21, 21)) y = ~y;
        res = opc == 1 ? x | y : opc == 2 ? x ^ y : x & y;
        if (!sf) res &= 0xffffffffu;
        if (opc == 3) r->nzcv = nz_of(res, sf);
        iwr(r, rd, res, sf, false);
        return true;
    }
    if ((w & 0x1f800000u) == 0x12800000u) {                     // MOVN, MOVZ, MOVK
        unsigned opc = fld(w, 30, 29), hw = fld(w, 22, 21);
        if (opc == 1 || (!sf && hw > 1))
            return false;
        uint64_t imm = (uint64_t)fld(w, 20, 5) << (16 * hw);
        uint64_t res = opc == 0 ? ~imm : opc == 2 ? imm
                     : (ird(r, rd, false) & ~(0xffffull << (16 * hw))) | imm;
        iwr(r, rd, res, sf, false);
        return true;
    }
    if ((w & 0x1f800000u) == 0x13000000u) {                     // SBFM, BFM, UBFM
        unsigned opc = fld(w, 30, 29), R = fld(w, 21, 16), S = fld(w, 15, 10);
        unsigned datasize = sf ? 64 : 32;
        uint64_t wmask, tmask;
        if (opc == 3 || fld(w, 22, 22) != sf || (!sf && (R >= 32 || S >= 32)) ||
            !bit_masks(fld(w, 22, 22), S, R, false, datasize, &wmask, &tmask))
            return false;
        uint64_t src = ird(r, rn, false), dst = ird(r, rd, false);
        if (!sf) { src &= 0xffffffffu; dst &= 0xffffffffu; }
        uint64_t rot = R ? (src >> R) | (src << (datasize - R)) : src;
        if (!sf) rot &= 0xffffffffu;
        uint64_t res;
        if (opc == 1) {
            uint64_t bot = (dst & ~wmask) | (rot & wmask);
            res = (dst & ~tmask) | (bot & tmask);
        } else if (opc == 2) {
            res = rot & wmask & tmask;
        } else {
            uint64_t top = ((src >> S) & 1) ? ~0ull : 0;
            res = (top & ~tmask) | (rot & wmask & tmask);
        }
        iwr(r, rd, res, sf, false);
        return true;
    }
    if ((w & 0x3fe00800u) == 0x1a800000u) {                     // CSEL, CSINC, CSINV, CSNEG
        uint64_t res;
        if (cond_holds(fld(w, 15, 12), r->nzcv)) {
            res = ird(r, rn, false);
        } else {
            res = ird(r, rm, false);
            if (fld(w, 30, 30)) res = ~res;
            if (fld(w, 10, 10)) res += 1;
        }
        iwr(r, rd, res, sf, false);
        return true;
    }
    if ((w & 0x3fe00410u) == 0x3a400000u) {                     // CCMN, CCMP
        if (cond_holds(fld(w, 15, 12), r->nzcv)) {
            bool op = fld(w, 30, 30);
            uint64_t y = fld(w, 11, 11) ? fld(w, 20, 16) : ird(r, rm, false);
            add_carry(ird(r, rn, false), op ? ~y : y, op, sf, &f);
            r->nzcv = f;
        } else {
            r->nzcv = w & 15;
        }
        return true;
    }
    if ((w & 0x7fe0f000u) == 0x1ac02000u) {                     // LSLV, LSRV, ASRV, RORV
        unsigned amt = (unsigned)(ird(r, rm, false) & (sf ? 63 : 31));
        iwr(r, rd, shifted(ird(r, rn, false), fld(w, 11, 10), amt, sf), sf, false);
        return true;
    }
    if ((w & 0x7fe00000u) == 0x1b000000u) {                     // MADD, MSUB
        uint64_t prod = ird(r, rn, false) * ird(r, rm, false), a = ird(r, fld(w, 14, 10), false);
        iwr(r, rd, fld(w, 15, 15) ? a - prod : a + prod, sf, false);
        return true;
    }
    return false;
}

// The load-exclusive a store-exclusive at pc pairs with: the nearest preceding
// LDXR/LDAXR (LDXP/LDAXP) of the same size and base, with no other exclusive,
// ordered or CAS instruction in between. The words between, in order.
static bool find_ldxr(uint32_t insn, uint64_t pc, uint64_t *lpc, uint32_t *lw,
                      uint32_t *between, int *nb, const char **why)
{
    unsigned size = insn >> 30, pair = fld(insn, 21, 21), rn = fld(insn, 9, 5);
    uint32_t back[16];
    for (int k = 1; k <= 16; k++) {
        uint32_t w;
        if (!read_word(pc - 4ull * (uint64_t)k, &w))
            return refuse(why, "store-exclusive: code before it unreadable"), false;
        if ((w & 0x3f000000u) != 0x08000000u) {
            back[k - 1] = w;
            continue;
        }
        bool is_casp = fld(w, 21, 21) && (w >> 30) < 2 && !fld(w, 23, 23);
        if (fld(w, 23, 23) || !fld(w, 22, 22) || is_casp || fld(w, 21, 21) != pair ||
            (w >> 30) != size || fld(w, 9, 5) != rn)
            return refuse(why, "store-exclusive: no matching load-exclusive"), false;
        unsigned lt = w & 31, lt2 = fld(w, 14, 10);
        if (lt == 31 || (pair && lt2 == 31))
            return refuse(why, "store-exclusive: load-exclusive discards its value"), false;
        *lpc = pc - 4ull * (uint64_t)k;
        *lw = w;
        *nb = k - 1;
        for (int i = 0; i < k - 1; i++)
            between[i] = back[k - 2 - i];
        return true;
    }
    return refuse(why, "store-exclusive: no load-exclusive within 16 instructions"), false;
}

// The expected value of a store-exclusive at pc. Two ways to know it:
//
//  1. The load-exclusive's destination still holds what it loaded (nothing in
//     between can have written it, nor the base): compare-and-swap against it.
//  2. The loop retries from the load-exclusive (`cbnz Ws, <ldxr>` right after
//     the store): then running it again from there with the registers as they
//     are is exactly the guest's own retry after a failed store-exclusive, so
//     the handler performs that retry -- the load as an atomic load, the body
//     interpreted, the store as a compare-and-swap, again until it succeeds
//     or the body branches out. Compilers emit this form and reuse the loaded
//     register (clang: ldaxr w8; add w8, w8, w0; stlxr w10, w8), which the
//     first way cannot use.
static bool exclusive_plan(uint32_t insn, uint64_t pc, const tstate *ss,
                           struct lxrt_storemu *m, const char **why)
{
    unsigned size = insn >> 30, pair = fld(insn, 21, 21), rn = fld(insn, 9, 5);
    uint64_t lpc;
    uint32_t lw;
    if (!find_ldxr(insn, pc, &lpc, &lw, m->between, &m->nbetween, why))
        return false;
    unsigned lt = lw & 31, lt2 = fld(lw, 14, 10);
    bool kept = true;
    for (int i = 0; i < m->nbetween; i++)
        if (clobbers(m->between[i], lt) || (pair && clobbers(m->between[i], lt2)) ||
            clobbers(m->between[i], rn))
            kept = false;
    if (kept) {
        if (!pair) {
            m->cmp[0] = xr(ss, lt) & mask_n(1u << size);
        } else if (size == 3) {
            m->cmp[0] = xr(ss, lt);
            m->cmp[1] = xr(ss, lt2);
        } else {
            m->cmp[0] = (xr(ss, lt) & 0xffffffffu) | (xr(ss, lt2) << 32);
        }
        return true;
    }
    uint32_t next;
    if (!read_word(pc + 4, &next) || (next & 0xff00001fu) != (0x35000000u | fld(insn, 20, 16)) ||
        pc + 4 + (uint64_t)(sext(fld(next, 23, 5), 19) * 4) != lpc)
        return refuse(why, "store-exclusive: loaded value overwritten, and no cbnz retry to the load"), false;
    struct ireg scratch;
    memset(&scratch, 0, sizeof scratch);
    for (int i = 0; i < m->nbetween; i++) {
        uint32_t w = m->between[i];
        uint64_t nx;
        bool branch = (w & 0x1c000000u) == 0x14000000u;
        if ((!branch && clobbers(w, rn)) ||
            !interp(w, lpc + 4 + 4ull * (uint64_t)i, &scratch, &nx))
            return refuse(why, "store-exclusive: loop body writes the base or is not interpretable"), false;
    }
    m->block = true;
    m->ldxr_pc = lpc;
    m->stxr_pc = pc;
    m->lt = lt;
    m->lt2 = lt2;
    return true;
}

int lxrt_storemu_decode(uint32_t insn, uint64_t pc, void *uap,
                        struct lxrt_storemu *m, const char **why)
{
    ucontext_t *u = uap;
    const tstate *ss = &u->uc_mcontext->__ss;
    const __uint128_t *v = u->uc_mcontext->__ns.__v;
    memset(m, 0, sizeof *m);
    m->wb = -1;
    *why = NULL;
    unsigned rt = insn & 31, rn = fld(insn, 9, 5);

    // DC ZVA, Xt: zero the naturally aligned block that holds Xt.
    if ((insn & 0xffffffe0u) == 0xd50b7420u) {
        uint64_t dczid;
        __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
        if (dczid & 16)
            return refuse(why, "DC ZVA prohibited");
        uint32_t block = 4u << (dczid & 15);
        m->kind = LXRT_SM_ZVA;
        m->len = block;
        m->addr = xr(ss, rt) & ~(uint64_t)(block - 1);
        return 0;
    }

    // Exclusive, ordered and compare-and-swap: size 001000 o2 L o1 Rs o0 Rt2 Rn Rt.
    if ((insn & 0x3f000000u) == 0x08000000u) {
        unsigned size = insn >> 30, o2 = fld(insn, 23, 23), L = fld(insn, 22, 22);
        unsigned o1 = fld(insn, 21, 21), rs = fld(insn, 20, 16), rt2 = fld(insn, 14, 10);
        uint64_t base = xr_sp(ss, rn);
        if (o2 && !o1) {                                  // STLR, STLLR (L=1: loads)
            if (L)
                return refuse(why, "load-acquire");
            unsigned n = 1u << size;
            m->kind = LXRT_SM_RELEASE;
            m->addr = base;
            m->len = m->elem = n;
            put_le(m->data, xr(ss, rt), n);
        } else if (o2 && o1) {                            // CAS{A}{L}{B,H}
            if (rt2 != 31)
                return refuse(why, "unallocated CAS form");
            unsigned n = 1u << size;
            m->kind = LXRT_SM_CAS;
            m->addr = base;
            m->len = n;
            m->rs = rs;
            m->cmp[0] = xr(ss, rs) & mask_n(n);
            m->val[0] = xr(ss, rt) & mask_n(n);
        } else if (o1 && size < 2) {                      // CASP{A}{L}
            if (rt2 != 31 || (rs & 1) || (rt & 1))
                return refuse(why, "unallocated CASP form");
            bool x = size == 1;
            m->kind = LXRT_SM_CASP;
            m->addr = base;
            m->len = x ? 16 : 8;
            m->rs = rs;
            if (x) {
                m->cmp[0] = xr(ss, rs);     m->cmp[1] = xr(ss, rs + 1);
                m->val[0] = xr(ss, rt);     m->val[1] = xr(ss, rt + 1);
            } else {
                m->pair32 = true;
                m->cmp[0] = (xr(ss, rs) & 0xffffffffu) | (xr(ss, rs + 1) << 32);
                m->val[0] = (xr(ss, rt) & 0xffffffffu) | (xr(ss, rt + 1) << 32);
            }
        } else if (!L) {                                  // STXR, STLXR, STXP, STLXP
            m->kind = LXRT_SM_EXCL;
            m->addr = base;
            m->rs = rs;
            if (!o1) {
                m->len = 1u << size;
                m->val[0] = xr(ss, rt) & mask_n(m->len);
            } else if (size == 3) {
                m->len = 16;
                m->val[0] = xr(ss, rt);
                m->val[1] = xr(ss, rt2);
            } else {
                m->len = 8;
                m->pair32 = true;
                m->val[0] = (xr(ss, rt) & 0xffffffffu) | (xr(ss, rt2) << 32);
            }
            m->st_rt = rt;
            m->st_rt2 = rt2;
            m->rn = rn;
            if (!exclusive_plan(insn, pc, ss, m, why))
                return -1;
        } else {
            return refuse(why, "load-exclusive");
        }
        // These fault on a misaligned address natively (alignment is checked
        // before permission); never perform one the hardware would refuse.
        if (m->addr & (m->len - 1))
            return refuse(why, "misaligned atomic or ordered access");
        return 0;
    }

    // STLUR{B,H} (FEAT_LRCPC2): size 011001 opc 0 imm9 00 Rn Rt, opc 00.
    if ((insn & 0x3f200c00u) == 0x19000000u) {
        if (fld(insn, 23, 22) != 0)
            return refuse(why, "LDAPUR");
        unsigned n = 1u << (insn >> 30);
        m->kind = LXRT_SM_RELEASE;
        m->addr = xr_sp(ss, rn) + (uint64_t)sext(fld(insn, 20, 12), 9);
        m->len = m->elem = n;
        put_le(m->data, xr(ss, rt), n);
        if (m->addr & (n - 1))
            return refuse(why, "misaligned STLUR");
        return 0;
    }

    // Pairs: opc 101 V type L imm7 Rt2 Rn Rt (type 00 STNP, 01 post, 10 offset, 11 pre).
    if ((insn & 0x3a000000u) == 0x28000000u) {
        unsigned opc = insn >> 30, V = fld(insn, 26, 26), type = fld(insn, 24, 23);
        unsigned rt2 = fld(insn, 14, 10);
        if (fld(insn, 22, 22))
            return refuse(why, "load pair");
        unsigned n;
        if (V) {
            if (opc == 3)
                return refuse(why, "unallocated pair");
            n = 4u << opc;
        } else {
            if (opc & 1)
                return refuse(why, opc == 1 ? "STGP (MTE)" : "unallocated pair");
            n = opc == 2 ? 8 : 4;
        }
        int64_t off = sext(fld(insn, 21, 15), 7) * (int64_t)n;
        uint64_t base = xr_sp(ss, rn);
        m->addr = type == 1 ? base : base + (uint64_t)off;
        if (type == 1 || type == 3) {
            m->wb = (int)rn;
            m->wb_val = base + (uint64_t)off;
        }
        if (V) {
            put_lane(m->data, v[rt], 0, n);
            put_lane(m->data + n, v[rt2], 0, n);
        } else {
            put_le(m->data, xr(ss, rt), n);
            put_le(m->data + n, xr(ss, rt2), n);
        }
        m->kind = LXRT_SM_STORE;
        m->len = 2 * n;
        m->elem = n;
        return 0;
    }

    // LSE atomics: size 111 0 00 A R 1 Rs o3 opc 00 Rn Rt.
    if ((insn & 0x3f200c00u) == 0x38200000u) {
        unsigned o3 = fld(insn, 15, 15), opc = fld(insn, 14, 12);
        unsigned n = 1u << (insn >> 30);
        if (o3 && opc)
            return refuse(why, opc == 4 ? "LDAPR" : "ST64B/LD64B family");
        m->kind = LXRT_SM_ATOMIC;
        m->op = o3 ? 8 : (int)opc;              // 0..7 LD<op>, 8 SWP
        m->addr = xr_sp(ss, rn);
        m->len = n;
        m->rt = rt;
        m->val[0] = xr(ss, fld(insn, 20, 16)) & mask_n(n);
        if (m->addr & (n - 1))
            return refuse(why, "misaligned atomic");
        return 0;
    }

    // Single register: size 111 V 0x opc ...
    if ((insn & 0x3a000000u) == 0x38000000u) {
        unsigned size = insn >> 30, V = fld(insn, 26, 26), opc = fld(insn, 23, 22);
        unsigned n;
        if (!V) {
            if (opc)
                return refuse(why, "load or prefetch");
            n = 1u << size;
        } else if (opc == 0) {
            n = 1u << size;
        } else if (opc == 2 && size == 0) {
            n = 16;
        } else {
            return refuse(why, "SIMD&FP load");
        }
        uint64_t base = xr_sp(ss, rn);
        if (fld(insn, 24, 24)) {                          // unsigned offset
            m->addr = base + (uint64_t)fld(insn, 21, 10) * n;
        } else if (!fld(insn, 21, 21)) {
            int64_t imm = sext(fld(insn, 20, 12), 9);
            switch (fld(insn, 11, 10)) {
            case 0:                                       // STUR
                m->addr = base + (uint64_t)imm;
                break;
            case 1:                                       // post-index
                m->addr = base;
                m->wb = (int)rn;
                m->wb_val = base + (uint64_t)imm;
                break;
            case 2:                                       // STTR
                if (V)
                    return refuse(why, "unallocated");
                m->addr = base + (uint64_t)imm;
                break;
            default:                                      // pre-index
                m->addr = base + (uint64_t)imm;
                m->wb = (int)rn;
                m->wb_val = m->addr;
                break;
            }
        } else if (fld(insn, 11, 10) == 2) {              // register offset
            unsigned option = fld(insn, 15, 13), S = fld(insn, 12, 12);
            if (!(option & 2))
                return refuse(why, "unallocated extend");
            uint64_t off = xr(ss, fld(insn, 20, 16));
            if (option == 2) off = (uint32_t)off;                           // UXTW
            else if (option == 6) off = (uint64_t)(int64_t)(int32_t)off;    // SXTW
            unsigned shift = 0;
            if (S)
                for (unsigned t = n; t > 1; t >>= 1) shift++;
            m->addr = base + (off << shift);
        } else {
            return refuse(why, "unallocated or pointer-authenticated load");
        }
        if (V)
            put_lane(m->data, v[rt], 0, n);
        else
            put_le(m->data, xr(ss, rt), n);
        m->kind = LXRT_SM_STORE;
        m->len = m->elem = n;
        return 0;
    }

    // ST1-ST4 (multiple structures): 0 Q 0011000 0 0 00000 opcode size Rn Rt,
    // post-index 0 Q 0011001 0 0 Rm opcode size Rn Rt.
    if ((insn & 0xbfff0000u) == 0x0c000000u || (insn & 0xbfe00000u) == 0x0c800000u) {
        bool post = fld(insn, 23, 23), Q = fld(insn, 30, 30);
        unsigned opcode = fld(insn, 15, 12), size = fld(insn, 11, 10), rm = fld(insn, 20, 16);
        unsigned rpt, selem;
        switch (opcode) {
        case 0x0: rpt = 1; selem = 4; break;
        case 0x2: rpt = 4; selem = 1; break;
        case 0x4: rpt = 1; selem = 3; break;
        case 0x6: rpt = 3; selem = 1; break;
        case 0x7: rpt = 1; selem = 1; break;
        case 0x8: rpt = 1; selem = 2; break;
        case 0xa: rpt = 2; selem = 1; break;
        default: return refuse(why, "unallocated ST multiple");
        }
        if (size == 3 && !Q && selem != 1)
            return refuse(why, "unallocated ST multiple");
        unsigned ebytes = 1u << size, elements = (Q ? 16u : 8u) / ebytes, offs = 0;
        for (unsigned r = 0; r < rpt; r++)
            for (unsigned e = 0; e < elements; e++) {
                unsigned tt = (rt + r) % 32;
                for (unsigned s = 0; s < selem; s++) {
                    put_lane(m->data + offs, v[tt], e, ebytes);
                    offs += ebytes;
                    tt = (tt + 1) % 32;
                }
            }
        uint64_t base = xr_sp(ss, rn);
        m->kind = LXRT_SM_STORE;
        m->addr = base;
        m->len = offs;
        m->elem = ebytes;
        if (post) {
            m->wb = (int)rn;
            m->wb_val = base + (rm == 31 ? offs : xr(ss, rm));
        }
        return 0;
    }

    // ST1-ST4 (single structure): 0 Q 0011010 0 R 00000 opcode S size Rn Rt,
    // post-index 0 Q 0011011 0 R Rm opcode S size Rn Rt.
    if ((insn & 0xbfdf0000u) == 0x0d000000u || (insn & 0xbfc00000u) == 0x0d800000u) {
        bool post = fld(insn, 23, 23);
        unsigned Q = fld(insn, 30, 30), R = fld(insn, 21, 21), rm = fld(insn, 20, 16);
        unsigned opcode = fld(insn, 15, 13), S = fld(insn, 12, 12), size = fld(insn, 11, 10);
        unsigned scale = opcode >> 1, selem = (((opcode & 1) << 1) | R) + 1, index;
        switch (scale) {
        case 0:
            index = (Q << 3) | (S << 2) | size;
            break;
        case 1:
            if (size & 1)
                return refuse(why, "unallocated ST single");
            index = (Q << 2) | (S << 1) | (size >> 1);
            break;
        case 2:
            if (size & 2)
                return refuse(why, "unallocated ST single");
            if (!(size & 1)) {
                index = (Q << 1) | S;
            } else {
                if (S)
                    return refuse(why, "unallocated ST single");
                index = Q;
                scale = 3;
            }
            break;
        default:
            return refuse(why, "unallocated ST single (replicate is load-only)");
        }
        unsigned ebytes = 1u << scale, offs = 0;
        for (unsigned s = 0; s < selem; s++) {
            put_lane(m->data + offs, v[(rt + s) % 32], index, ebytes);
            offs += ebytes;
        }
        uint64_t base = xr_sp(ss, rn);
        m->kind = LXRT_SM_STORE;
        m->addr = base;
        m->len = offs;
        m->elem = ebytes;
        if (post) {
            m->wb = (int)rn;
            m->wb_val = base + (rm == 31 ? offs : xr(ss, rm));
        }
        return 0;
    }

    return refuse(why, "not a store");
}

// ---------------------------------------------------------------- memory

static void store_unit(uint64_t a, const uint8_t *d, unsigned n, int order)
{
    uint64_t x = 0;
    if (n <= 8 && !(n & (n - 1)) && !(a & (n - 1))) {
        memcpy(&x, d, n);
        switch (n) {
        case 1: __atomic_store_n((uint8_t *)a, (uint8_t)x, order); return;
        case 2: __atomic_store_n((uint16_t *)a, (uint16_t)x, order); return;
        case 4: __atomic_store_n((uint32_t *)a, (uint32_t)x, order); return;
        default: __atomic_store_n((uint64_t *)a, x, order); return;
        }
    }
    if (n == 16 && !(a & 15)) {
        // An aligned 16-byte STP is single-copy atomic with FEAT_LSE2.
        uint64_t lo, hi;
        memcpy(&lo, d, 8);
        memcpy(&hi, d + 8, 8);
        __asm__ volatile("stp %0, %1, [%2]" :: "r"(lo), "r"(hi), "r"(a) : "memory");
        return;
    }
    volatile uint8_t *p = (volatile uint8_t *)a;
    for (unsigned i = 0; i < n; i++)
        p[i] = d[i];
}

static uint64_t load_n(uint64_t a, unsigned n)
{
    switch (n) {
    case 1: return __atomic_load_n((uint8_t *)a, __ATOMIC_SEQ_CST);
    case 2: return __atomic_load_n((uint16_t *)a, __ATOMIC_SEQ_CST);
    case 4: return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST);
    default: return __atomic_load_n((uint64_t *)a, __ATOMIC_SEQ_CST);
    }
}

// Compare-and-swap of n (1/2/4/8) bytes; *expected becomes the old value.
static bool cas_n(uint64_t a, unsigned n, uint64_t *expected, uint64_t nv)
{
    bool ok;
    switch (n) {
    case 1: { uint8_t e = (uint8_t)*expected;
        ok = __atomic_compare_exchange_n((uint8_t *)a, &e, (uint8_t)nv, false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *expected = e; return ok; }
    case 2: { uint16_t e = (uint16_t)*expected;
        ok = __atomic_compare_exchange_n((uint16_t *)a, &e, (uint16_t)nv, false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *expected = e; return ok; }
    case 4: { uint32_t e = (uint32_t)*expected;
        ok = __atomic_compare_exchange_n((uint32_t *)a, &e, (uint32_t)nv, false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *expected = e; return ok; }
    default: { uint64_t e = *expected;
        ok = __atomic_compare_exchange_n((uint64_t *)a, &e, nv, false,
                                         __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        *expected = e; return ok; }
    }
}

// 16-byte compare-and-swap (CASPAL, as the guest's own CASP would be).
static bool cas_16(uint64_t a, uint64_t *lo, uint64_t *hi, uint64_t nlo, uint64_t nhi)
{
    register uint64_t x0 __asm__("x0") = *lo;
    register uint64_t x1 __asm__("x1") = *hi;
    register uint64_t x2 __asm__("x2") = nlo;
    register uint64_t x3 __asm__("x3") = nhi;
    uint64_t elo = *lo, ehi = *hi;
    __asm__ volatile(".arch_extension lse\n\tcaspal x0, x1, x2, x3, [%4]"
                     : "+r"(x0), "+r"(x1) : "r"(x2), "r"(x3), "r"(a) : "memory");
    *lo = x0;
    *hi = x1;
    return x0 == elo && x1 == ehi;
}

static uint64_t rmw_value(int op, uint64_t old, uint64_t arg, unsigned n)
{
    int64_t so = sext(old, (int)(8 * n)), sa = sext(arg, (int)(8 * n));
    switch (op) {
    case 0: return old + arg;                   // LDADD
    case 1: return old & ~arg;                  // LDCLR
    case 2: return old ^ arg;                   // LDEOR
    case 3: return old | arg;                   // LDSET
    case 4: return so > sa ? old : arg;         // LDSMAX
    case 5: return so < sa ? old : arg;         // LDSMIN
    case 6: return old > arg ? old : arg;       // LDUMAX
    case 7: return old < arg ? old : arg;       // LDUMIN
    default: return arg;                        // SWP
    }
}

// The retry of an LL/SC loop (exclusive_plan, way 2), from the load-exclusive
// with the thread's registers as they are: atomic load, body interpreted,
// compare-and-swap, until the swap succeeds or the body branches out of the
// loop. The result is committed as if the guest had run it: all registers,
// the flags, and pc after the store-exclusive (status 0) or at the branch
// target. After 1000 lost races the store-exclusive is reported failed (it
// may always fail) and the guest's own loop takes over.
static void perform_retry(const struct lxrt_storemu *m, tstate *ss)
{
    struct ireg base;
    for (unsigned i = 0; i < 31; i++)
        base.x[i] = xr(ss, i);
    base.sp = ss->__sp;
    base.nzcv = ss->__cpsr >> 28;
    unsigned n = m->len;
    for (int attempt = 0; ; attempt++) {
        struct ireg r = base;
        uint64_t lo, hi = 0;
        if (n == 16) {
            // An aligned 16-byte LDP is single-copy atomic with FEAT_LSE2.
            __asm__ volatile("ldp %0, %1, [%2]" : "=r"(lo), "=r"(hi) : "r"(m->addr) : "memory");
        } else {
            lo = load_n(m->addr, n);
        }
        if (m->pair32) {
            iwr(&r, m->lt, lo & 0xffffffffu, true, false);
            iwr(&r, m->lt2, lo >> 32, true, false);
        } else {
            iwr(&r, m->lt, lo, true, false);
            if (n == 16)
                iwr(&r, m->lt2, hi, true, false);
        }
        uint64_t pc = m->ldxr_pc + 4, next;
        int steps = 0;
        while (pc != m->stxr_pc) {
            if (pc <= m->ldxr_pc || pc > m->stxr_pc || ++steps > 64)
                break;                  // branched out of the loop
            interp(m->between[(pc - m->ldxr_pc) / 4 - 1], pc, &r, &next);
            pc = next;
        }
        bool at_store = pc == m->stxr_pc;
        bool ok = false;
        if (at_store) {
            uint64_t a = ird(&r, m->st_rt, false), b = ird(&r, m->st_rt2, false);
            if (n == 16) {
                uint64_t elo = lo, ehi = hi;
                ok = cas_16(m->addr, &elo, &ehi, a, b);
            } else {
                uint64_t e = lo;
                uint64_t nv = m->pair32 ? (a & 0xffffffffu) | (b << 32) : a & mask_n(n);
                ok = cas_n(m->addr, n, &e, nv);
            }
            if (!ok && attempt < 1000)
                continue;               // lost a race: the guest would retry too
            iwr(&r, m->rs, ok ? 0 : 1, false, false);
            pc = m->stxr_pc + 4;
        }
        for (unsigned i = 0; i < 31; i++)
            set_xr(ss, i, r.x[i]);
        ss->__sp = r.sp;
        ss->__cpsr = (ss->__cpsr & 0x0fffffffu) | (r.nzcv << 28);
        ss->__pc = pc;
        return;
    }
}

void lxrt_storemu_perform(const struct lxrt_storemu *m, void *uap)
{
    ucontext_t *u = uap;
    tstate *ss = &u->uc_mcontext->__ss;
    uint64_t a = m->addr;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    switch (m->kind) {
    case LXRT_SM_STORE:
    case LXRT_SM_RELEASE: {
        int order = m->kind == LXRT_SM_RELEASE ? __ATOMIC_RELEASE : __ATOMIC_RELAXED;
        unsigned unit = m->elem ? m->elem : m->len;
        if (m->len == 16 && !(a & 15))
            unit = 16;      // an aligned STP of X registers is one 128-bit access (LSE2)
        for (unsigned off = 0; off < m->len; off += unit)
            store_unit(a + off, m->data + off, unit, order);
        break;
    }
    case LXRT_SM_ZVA: {
        static const uint8_t zero[8];
        for (unsigned off = 0; off < m->len; off += m->len >= 8 ? 8 : m->len)
            store_unit(a + off, zero, m->len >= 8 ? 8 : m->len, __ATOMIC_RELAXED);
        break;
    }
    case LXRT_SM_ATOMIC: {
        unsigned n = m->len;
        uint64_t old = load_n(a, n);
        for (;;) {
            uint64_t e = old;
            if (cas_n(a, n, &e, rmw_value(m->op, old, m->val[0], n) & mask_n(n)))
                break;
            old = e;
        }
        set_xr(ss, m->rt, old & mask_n(n));
        break;
    }
    case LXRT_SM_CAS: {
        uint64_t e = m->cmp[0];
        cas_n(a, m->len, &e, m->val[0]);
        set_xr(ss, m->rs, e & mask_n(m->len));
        break;
    }
    case LXRT_SM_CASP: {
        if (m->pair32) {
            uint64_t e = m->cmp[0];
            cas_n(a, 8, &e, m->val[0]);
            set_xr(ss, m->rs, e & 0xffffffffu);
            set_xr(ss, m->rs + 1, e >> 32);
        } else {
            uint64_t lo = m->cmp[0], hi = m->cmp[1];
            cas_16(a, &lo, &hi, m->val[0], m->val[1]);
            set_xr(ss, m->rs, lo);
            set_xr(ss, m->rs + 1, hi);
        }
        break;
    }
    case LXRT_SM_EXCL: {
        if (m->block) {
            perform_retry(m, ss);
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            return;                     // registers and pc set by the retry
        }
        bool ok;
        if (m->len == 16) {
            uint64_t lo = m->cmp[0], hi = m->cmp[1];
            ok = cas_16(a, &lo, &hi, m->val[0], m->val[1]);
        } else {
            uint64_t e = m->cmp[0];
            ok = cas_n(a, m->len, &e, m->val[0]);
        }
        set_xr(ss, m->rs, ok ? 0 : 1);
        break;
    }
    }
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (m->wb >= 0)
        set_xr_sp(ss, (unsigned)m->wb, m->wb_val);
    ss->__pc += 4;
}
