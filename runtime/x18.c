#include "x18.h"
#include <string.h>

static const unsigned field_shift[4] = {0, 5, 10, 16};

static unsigned reg_at(uint32_t i, unsigned shift) { return (i >> shift) & 31; }
static bool match(uint32_t i, uint32_t mask, uint32_t value) {
    return (i & mask) == value;
}
static int decoded(int fields, int kind, int *cls) {
    *cls = kind;
    return fields;
}

int lxrt_x18_gpr_fields(uint32_t i, bool *sp_base, bool *sp_rd, int *cls) {
    *sp_base = false;
    *sp_rd = false;
    *cls = X18_CLS_OTHER;
    unsigned size = i >> 30, opc = (i >> 22) & 3;
    bool v = (i & (1u << 26)) != 0;

    /* PC-relative and immediate integer encodings. */
    if (match(i, 0x1f000000, 0x10000000))
        return decoded(1, (i >> 31) ? X18_CLS_ADRP : X18_CLS_ADR, cls);
    if (match(i, 0x1f800000, 0x11000000)) {
        *sp_base = true;
        *sp_rd = (i & (1u << 29)) == 0;
        return decoded(3, X18_CLS_ADDSUB_IMM, cls);
    }
    if (match(i, 0x1f800000, 0x12000000)) {
        /* DecodeBitMasks: len >= 1; all-ones element is reserved. */
        unsigned n = (i >> 22) & 1, s = (i >> 10) & 63;
        unsigned selector = (n << 6) | ((~s) & 63), len = 0;
        while (selector >>= 1) ++len;
        if (len == 0 || (!(i >> 31) && n) ||
            (s & ((1u << len) - 1)) == ((1u << len) - 1)) return -1;
        *sp_rd = ((i >> 29) & 3) != 3;
        return decoded(3, X18_CLS_DP_IMM, cls);
    }
    if (match(i, 0x1f800000, 0x12800000)) {
        if (((i >> 29) & 3) == 1 || (!(i >> 31) && (i & (1u << 22)))) return -1;
        return decoded(1, X18_CLS_DP_IMM, cls);
    }
    if (match(i, 0x1f800000, 0x13000000)) {
        if (((i >> 29) & 3) == 3 || ((i >> 22) & 1) != (i >> 31) ||
            (!(i >> 31) && (i & 0x00208000))) return -1;
        return decoded(3, X18_CLS_DP_IMM, cls);
    }
    if (match(i, 0x7fa00000, 0x13800000)) {
        if (((i >> 22) & 1) != (i >> 31) || (!(i >> 31) && (i & 0x8000))) return -1;
        return decoded(11, X18_CLS_DP_IMM, cls);
    }
    if (match(i, 0x7e000000, 0x34000000)) return decoded(1, X18_CLS_CBZ, cls);
    if (match(i, 0x7e000000, 0x36000000)) return decoded(1, X18_CLS_TBZ, cls);
    if (match(i, 0xfffffc1f, 0xd61f0000) ||
        match(i, 0xfffffc1f, 0xd63f0000) ||
        match(i, 0xfffffc1f, 0xd65f0000)) return decoded(2, X18_CLS_BR, cls);
    if (match(i, 0xffd00000, 0xd5100000)) return decoded(1, X18_CLS_SYSREG, cls);

    /* Load/store literal: SIMD destinations and PRFM have no GPR operand. */
    if (match(i, 0x3b000000, 0x18000000)) {
        if (v || size == 3) return -1;
        return decoded(1, X18_CLS_LDST_LITERAL, cls);
    }
    /* CAS(P) must precede the exclusive major group. */
    if (match(i, 0x3fa07c00, 0x08a07c00)) {
        *sp_base = true;
        return decoded(11, X18_CLS_CAS, cls);
    }
    if (match(i, 0xbfa07c00, 0x08207c00)) {
        if ((i & 1) || (i & (1u << 16))) return -1; /* pairs start even */
        *sp_base = true;
        return decoded(11, X18_CLS_CASP, cls);
    }
    if (match(i, 0x3f000000, 0x08000000)) {
        bool ordered = (i & (1u << 23)) != 0;
        bool load = (i & (1u << 22)) != 0;
        bool pair = (i & (1u << 21)) != 0;
        if ((ordered && (pair || !(i & 0x8000))) || (pair && size < 2) ||
            ((!pair) && reg_at(i, 10) != 31) ||
            ((load || ordered) && reg_at(i, 16) != 31)) return -1;
        *sp_base = true;
        return decoded(3 | (pair ? 4 : 0) | ((!load && !ordered) ? 8 : 0),
                       X18_CLS_LDST_EXCL, cls);
    }
    if (match(i, 0x3a000000, 0x28000000)) {
        if (size == 3 || (!v && size == 1 && (!(i & (1u << 22)) ||
            ((i >> 23) & 3) == 0))) return -1;
        *sp_base = true;
        return decoded(v ? 2 : 7, X18_CLS_LDST_PAIR, cls);
    }
    /* LDAPUR/STLUR have imm9 like LDUR/STUR, in the RCpc major group. */
    if (match(i, 0x3f200c00, 0x19000000)) {
        if ((size == 3 && opc >= 2) || (size == 2 && opc == 3)) return -1;
        *sp_base = true;
        return decoded(3, X18_CLS_LDST_UNSCALED, cls);
    }
    /* LSE read-modify-write. Bit 15 selects SWP; 14:12 must then be zero. */
    if (match(i, 0x3f200c00, 0x38200000)) {
        unsigned op = (i >> 12) & 15;
        if (op > 8) {
            /* LDAPR: Rs is fixed 31, not a register operand. */
            if (match(i, 0x3ffffc00, 0x38bfc000)) {
                *sp_base = true;
                return decoded(3, X18_CLS_ATOMIC, cls);
            }
            return -1;
        }
        *sp_base = true;
        return decoded(11, X18_CLS_ATOMIC, cls);
    }
    if (match(i, 0x3a000000, 0x38000000)) {
        bool prefetch = !v && size == 3 && opc == 2;
        if ((!v && ((size == 3 && opc == 3) || (size == 2 && opc == 3))) ||
            (v && opc >= 2 && size != 0)) return -1;
        int fields = (v || prefetch) ? 2 : 3, kind;
        if (i & (1u << 24)) kind = X18_CLS_LDST_UIMM;
        else if (i & (1u << 21)) {
            if (((i >> 10) & 3) != 2 || !(i & (1u << 14))) return -1;
            fields |= 8;
            kind = X18_CLS_LDST_REGOFF;
        } else {
            unsigned mode = (i >> 10) & 3;
            if ((prefetch && mode != 0) || (v && mode == 2)) return -1;
            kind = (mode & 1) ? X18_CLS_LDST_PRE_POST : X18_CLS_LDST_UNSCALED;
        }
        *sp_base = true;
        return decoded(fields, kind, cls);
    }
    /* AdvSIMD structure transfers: Rt is vector, Rm exists only with post-index. */
    if (match(i, 0xbf000000, 0x0c000000) || match(i, 0xbf000000, 0x0d000000)) {
        bool post = (i & (1u << 23)) != 0;
        if (!post && reg_at(i, 16) != 0) return -1;
        if (!(i & (1u << 24))) {
            unsigned op = (i >> 12) & 15;
            if ((i & (1u << 21)) || !(op == 0 || op == 2 || op == 4 ||
                op == 6 || op == 7 || op == 8 || op == 10)) return -1;
            if (((i >> 10) & 3) == 3 && !(i & (1u << 30)) &&
                !(op == 2 || op == 6 || op == 7 || op == 10)) return -1;
        } else {
            unsigned op = (i >> 13) & 7, sz = (i >> 10) & 3;
            if ((op >= 6 && (!(i & (1u << 22)) || (i & (1u << 12)))) ||
                ((op == 2 || op == 3) && (sz & 1)) ||
                ((op == 4 || op == 5) && (sz > 1 || (sz == 1 && (i & (1u << 12)))))) return -1;
        }
        *sp_base = true;
        return decoded(2 | ((post && reg_at(i, 16) != 31) ? 8 : 0), X18_CLS_SIMD_LDST, cls);
    }

    /* Scalar FP/integer conversion and FMOV general, never scalar FP arithmetic. */
    if (match(i, 0x7f000000, 0x1e000000)) {
        unsigned type = (i >> 22) & 3, mode = (i >> 19) & 3, op = (i >> 16) & 7;
        bool unscaled = (i & (1u << 21)) != 0;
        if (unscaled && (i & 0xfc00)) return -1;
        if (!unscaled) {
            if (type == 2 || (!(i >> 31) && !(i & 0x8000))) return -1;
            if (op <= 1 && mode == 3) return decoded(1, X18_CLS_FP_INT, cls);
            if ((op == 2 || op == 3) && mode == 0) return decoded(2, X18_CLS_FP_INT, cls);
        } else if (op >= 6) {
            if ((mode == 0 && ((type == 0 && !(i >> 31)) ||
                 (type == 1 && (i >> 31)) || type == 3)) ||
                (mode == 1 && type == 2 && (i >> 31)))
                return decoded(op == 6 ? 1 : 2, X18_CLS_FP_INT, cls);
        } else if (type != 2) {
            if (op <= 1 || ((op == 4 || op == 5) && mode == 0))
                return decoded(1, X18_CLS_FP_INT, cls);
            if ((op == 2 || op == 3) && mode == 0) return decoded(2, X18_CLS_FP_INT, cls);
        }
        return -1;
    }
    if (match(i, 0xbf208400, 0x0e000400)) {
        unsigned imm5 = (i >> 16) & 31, op = (i >> 11) & 15;
        unsigned sz = 0;
        if (!imm5) return -1;
        while (((imm5 >> sz) & 1) == 0) ++sz;
        bool q = (i & (1u << 30)) != 0;
        if (sz > 3) return -1;
        if (op == 1 && imm5 == (1u << sz) && (q || sz != 3))
            return decoded(2, X18_CLS_SIMD_COPY, cls);
        if (op == 3 && q) return decoded(2, X18_CLS_SIMD_COPY, cls);
        if (op == 5 && sz < (q ? 3u : 2u)) return decoded(1, X18_CLS_SIMD_COPY, cls);
        if (op == 7 && (q ? sz == 3 : sz < 3)) return decoded(1, X18_CLS_SIMD_COPY, cls);
        return -1;
    }

    /* Integer data processing: keep fixed opcode/Ra fields out of the mask. */
    if (match(i, 0x1f000000, 0x0a000000)) {
        if (!(i >> 31) && (i & 0x8000)) return -1;
        return decoded(11, X18_CLS_DP_REG, cls);
    }
    if (match(i, 0x1f000000, 0x0b000000)) {
        if (i & (1u << 21)) {
            if ((i & 0x00c00000) || ((i >> 10) & 7) > 4) return -1;
            *sp_base = true;
            *sp_rd = !(i & (1u << 29));
        } else if (((i >> 22) & 3) == 3 || (!(i >> 31) && (i & 0x8000))) return -1;
        return decoded(11, X18_CLS_DP_REG, cls);
    }
    if (match(i, 0x1fe0fc00, 0x1a000000)) return decoded(11, X18_CLS_DP_REG, cls);
    if (match(i, 0x1fe00410, 0x1a400000) && (i & (1u << 29)))
        return decoded((i & 0x800) ? 2 : 10, X18_CLS_DP_REG, cls);
    if (match(i, 0x3fe00800, 0x1a800000)) return decoded(11, X18_CLS_DP_REG, cls);
    if (match(i, 0x7fe00000, 0x5ac00000)) {
        unsigned op = (i >> 10) & 63;
        if (reg_at(i, 16) != 0 || op > 5 || (op == 3 && !(i >> 31))) return -1;
        return decoded(3, X18_CLS_DP_REG, cls);
    }
    if (match(i, 0x7fe00000, 0x1ac00000)) {
        unsigned op = (i >> 10) & 63;
        if (!(op == 2 || op == 3 || (op >= 8 && op <= 11) ||
            (op >= 16 && op <= 23))) return -1;
        if (op >= 16 && ((op & 3) == 3) != (bool)(i >> 31)) return -1;
        return decoded(11, X18_CLS_DP_REG, cls);
    }
    if (match(i, 0x7f000000, 0x1b000000)) {
        unsigned op = (i >> 21) & 7;
        if (op == 0) return decoded(15, X18_CLS_DP_REG, cls);
        if (!(i >> 31)) return -1;
        if (op == 1 || op == 5) return decoded(15, X18_CLS_DP_REG, cls);
        if ((op == 2 || op == 6) && !(i & 0x8000) && reg_at(i, 10) == 31)
            return decoded(11, X18_CLS_DP_REG, cls);
    }
    return -1;
}

bool lxrt_x18_touches(uint32_t i) {
    bool sb, sd;
    int cls, fields = lxrt_x18_gpr_fields(i, &sb, &sd, &cls);
    if (fields < 0) return false;
    for (unsigned f = 0; f < 4; ++f)
        if ((fields & (1 << f)) && reg_at(i, field_shift[f]) == 18) return true;
    return false;
}

/* Sign extension without signed shifts or out-of-range unsigned-to-signed casts. */
static int64_t sext(uint32_t x, unsigned bits) {
    uint32_t sign = 1u << (bits - 1);
    return (int64_t)(x & (sign - 1)) - (int64_t)(x & sign);
}
static uint32_t replace(uint32_t i, unsigned shift, unsigned r) {
    return (i & ~(31u << shift)) | (r << shift);
}
static void emit(struct x18_plan *p, uint32_t word) { p->words[p->nwords++] = word; }
static void materialize(struct x18_plan *p, unsigned r, uint64_t value) {
    emit(p, 0xd2800000 | ((uint32_t)(value & 0xffff) << 5) | r);
    for (unsigned h = 1; h < 4; ++h) {
        unsigned part = (unsigned)((value >> (16 * h)) & 0xffff);
        if (part) emit(p, 0xf2800000 | (h << 21) | (part << 5) | r);
    }
}
static void reject(struct x18_plan *p, const char *why) {
    memset(p, 0, sizeof(*p));
    p->verdict = X18_UNSUPPORTED;
    p->why = why;
    p->back_idx = p->alt_idx = -1;
}
/* MRS/MSR of NZCV, FPCR or FPSR: plain EL0 state on Linux and Darwin alike,
 * and nothing the trampoline executes reads or writes any of them (its AND is
 * the non-flag-setting form). Bit 21 (L) and Rt are masked out. */
static bool plain_sysreg(uint32_t i) {
    uint32_t r = i & ~0x0020001fu;
    return r == 0xd51b4200u || r == 0xd51b4400u || r == 0xd51b4420u;
}
static uint32_t tbz63(bool nz, int words, unsigned r) {
    return (nz ? 0xb7f80000u : 0xb6f80000u) | (((uint32_t)words & 0x3fff) << 5) | r;
}

/* An instruction that computes SP from x18 (Linux code does `mov sp, x18`
 * after a call: MEASURED in libgallium, steamclient.so and steamui.so). The
 * new value T is built in S1 like any other result; the hard part is
 * restoring S1/S2, saved at P = sp - 16, while SP moves to T. At no point may
 * the saved pair sit below the live SP, where a signal frame can land.
 *   T <= P:  SP = T, then reload the pair from P (now at or above SP).
 *   T >= P + 16: copy the pair up to T - 16 (above the live SP), SP = T - 16,
 *            reload from there, SP += 16.
 *   P < T < P + 16 exists only with a misaligned SP; neither order is safe
 *            with two registers, so it traps (brk #1) instead of corrupting.
 * No access through SP except the pre-index push, which signal.c emulates
 * when SP is misaligned. */
static void plan_writes_sp(struct x18_plan *p, uint32_t j, uint64_t site, bool sp_src,
                           unsigned s1, unsigned s2, unsigned slot_off) {
    emit(p, 0xa9bf0000 | (s2 << 10) | (31 << 5) | s1);   /* stp S1, S2, [sp, #-16]! */
    emit(p, 0xd53bd060 | s2);                            /* mrs S2, tpidrro_el0 */
    emit(p, 0x927df000 | (s2 << 5) | s2);                /* and S2, S2, #~7 */
    emit(p, 0xf9400000 | ((slot_off / 8) << 10) | (s2 << 5) | s1);
    if (sp_src) {
        emit(p, 0x910043e0 | s2);                        /* add S2, sp, #16 */
        j = replace(j, 5, s2);
    }
    emit(p, replace(j, 0, s1));                          /* S1 = T */
    emit(p, 0x910003e0 | s2);                            /* mov S2, sp: P */
    emit(p, 0xcb000000 | (s1 << 16) | (s2 << 5) | s2);   /* sub S2, S2, S1: P - T */
    int to_a = p->nwords;
    emit(p, 0);                                          /* tbz S2, #63, T <= P */
    emit(p, 0x91003c00 | (s2 << 5) | s2);                /* add S2, S2, #15 */
    int to_trap = p->nwords;
    emit(p, 0);                                          /* tbz S2, #63, T < P + 16 */
    emit(p, 0xd1004000 | (s1 << 5) | s1);                /* sub S1, S1, #16: X */
    emit(p, 0x910023e0 | s2);                            /* add S2, sp, #8 */
    emit(p, 0xf9400000 | (s2 << 5) | s2);                /* ldr S2, [S2] */
    emit(p, 0xf9000400 | (s1 << 5) | s2);                /* str S2, [S1, #8] */
    emit(p, 0x910003e0 | s2);                            /* mov S2, sp */
    emit(p, 0xf9400000 | (s2 << 5) | s2);                /* ldr S2, [S2] */
    emit(p, 0xf9000000 | (s1 << 5) | s2);                /* str S2, [S1] */
    emit(p, 0x9100001f | (s1 << 5));                     /* mov sp, S1 */
    emit(p, 0xa9400000 | (s2 << 10) | (s1 << 5) | s1);   /* ldp S1, S2, [S1] */
    emit(p, 0x910043ff);                                 /* add sp, sp, #16 */
    p->back_idx = p->nwords;
    emit(p, 0);
    p->words[to_a] = tbz63(false, p->nwords - to_a, s2);
    emit(p, 0x8b000000 | (s1 << 16) | (s2 << 5) | s2);   /* add S2, S2, S1: P */
    emit(p, 0x9100001f | (s1 << 5));                     /* mov sp, S1 */
    emit(p, 0xa9400000 | (s2 << 10) | (s2 << 5) | s1);   /* ldp S1, S2, [S2] */
    p->alt_idx = p->nwords;
    p->alt_target = site + 4;
    emit(p, 0);
    p->words[to_trap] = tbz63(false, p->nwords - to_trap, s2);
    emit(p, 0xd4200020);                                 /* brk #1 */
    p->verdict = X18_OK;
}

/* `br x18` without touching any general register. Up to 7492467 it went through
 * x16 like blr: V8's TurboFan keeps w16 live across a jump table dispatched
 * with `adr x18; add x18, x18, x0, lsl #2; br x18`, read it back as the
 * target address and built broken graphs (Heroic, the Node reproducer:
 * CHECK failure in CFGBuilder::ConnectBlocks, benchmarks/stage28-keep-x18.txt).
 * The branch now goes through the hardware x18 itself, loaded from the
 * virtual one as the last thing before the branch; x18.h says how the
 * runtime recovers when the kernel zeroes it in between (signal.c). */
static void plan_br(struct x18_plan *p, unsigned slot_off) {
    unsigned imm = slot_off / 8;
    emit(p, 0xa9bf47f0);                          /* 0 stp x16, x17, [sp, #-16]! */
    emit(p, 0xd53bd071);                          /* 1 mrs x17, tpidrro_el0 */
    emit(p, 0x927df231);                          /* 2 and x17, x17, #~7 */
    emit(p, 0xf9400000 | (imm << 10) | (17 << 5) | 16); /* 3 ldr x16, [x17, #slot]: target */
    emit(p, 0x10000000 | (8u << 5) | 17);         /* 4 adr x17, D (+32) */
    emit(p, 0xa9bf43f1);                          /* 5 stp x17, x16, [sp, #-16]!: {D, target} */
    emit(p, 0x910043f1);                          /* 6 add x17, sp, #16 */
    emit(p, 0xa9404630);                          /* 7 ldp x16, x17, [x17]: guest x16, x17 */
    emit(p, 0x910083ff);                          /* 8 add sp, sp, #32: the mark is below sp */
    emit(p, 0xd53bd072);                          /* 9  A mrs x18, tpidrro_el0 */
    emit(p, 0x927df252);                          /* 10 B and x18, x18, #~7 */
    emit(p, 0xf9400252 | (imm << 10));            /* 11 C ldr x18, [x18, #slot] */
    emit(p, 0xd61f0240);                          /* 12 D br x18 */
    p->terminal = true;
    p->verdict = X18_OK;
}

bool lxrt_x18_br_tail(const uint32_t w[4]) {
    return w[0] == 0xd53bd072 && w[1] == 0x927df252 &&
           (w[2] & 0xffc003ffu) == 0xf9400252 && w[3] == 0xd61f0240;
}

unsigned lxrt_x18_br_restart(const uint32_t w[7]) {
    for (unsigned k = 0; k < 4; ++k)             /* pc = A + 4k */
        if (lxrt_x18_br_tail(w + 3 - k))
            return 36 + 4 * k;                   /* A is word 9 */
    return 0;
}

void lxrt_x18_plan(uint32_t i, uint64_t site, uint64_t tramp,
                   unsigned slot_off, unsigned tls_off, struct x18_plan *p) {
    memset(p, 0, sizeof(*p));
    p->back_idx = p->alt_idx = -1;
    if (!lxrt_x18_touches(i)) return;
    bool sb, sd;
    int cls, fields = lxrt_x18_gpr_fields(i, &sb, &sd, &cls);
    /* A trampoline between a load-exclusive and its store-exclusive puts
     * stack and TSD accesses inside the LL/SC sequence, which may clear the
     * exclusive monitor every time and livelock the retry loop. LDAR/STLR
     * (bit 23) are ordered, not exclusive: planned like any load/store. */
    if (cls == X18_CLS_LDST_EXCL && !(i & (1u << 23))) { reject(p, "exclusive"); return; }
    if (cls == X18_CLS_CASP) { reject(p, "casp pair"); return; }
    if (cls == X18_CLS_SYSREG && i != 0xd53bd052 && i != 0xd51bd052 && !plain_sysreg(i)) {
        reject(p, "sysreg"); return;
    }
    if ((slot_off & 7) || slot_off >= 32768 || (tls_off & 7) || tls_off >= 32768 ||
        (site & 3) || (tramp & 3)) { reject(p, "imm overflow"); return; }
    if (i == 0xd61f0240) { /* br x18 */
        plan_br(p, slot_off);
        return;
    }
    if (cls == X18_CLS_BR) {
        /* blr x18 / ret x18: a call or a return, where x16 (IP0) is dead by
         * the procedure call standard, so it carries the guest target. A BLR
         * must expose the original guest return PC in x30. A plain `br x18`
         * is not a call (a jump table inside a function) and keeps x16:
         * plan_br. */
        emit(p, 0xa9bf47f0); /* stp x16, x17, [sp, #-16]! */
        emit(p, 0xd53bd071); /* mrs x17, tpidrro_el0 */
        emit(p, 0x927df231); /* and x17, x17, #~7 */
        emit(p, 0xf9400000 | ((slot_off / 8) << 10) | (17 << 5) | 16);
        emit(p, 0xf94007f1); /* ldr x17, [sp, #8] */
        emit(p, 0x910043ff); /* add sp, sp, #16 */
        if (i == 0xd63f0240) materialize(p, 30, site + 4);
        emit(p, 0xd61f0200); /* br x16 */
        p->terminal = true;
        p->verdict = X18_OK;
        return;
    }
    unsigned used = 0, s1 = 0, s2;
    for (unsigned f = 0; f < 4; ++f)
        if (fields & (1 << f)) used |= 1u << reg_at(i, field_shift[f]);
    while (used & (1u << s1)) ++s1;
    s2 = s1 + 1;
    while (used & (1u << s2)) ++s2;
    uint32_t j = i;
    for (unsigned f = 0; f < 4; ++f)
        if ((fields & (1 << f)) && reg_at(i, field_shift[f]) == 18)
            j = replace(j, field_shift[f], s1);
    if (sd && reg_at(i, 0) == 31) {
        plan_writes_sp(p, j, site, sb && reg_at(i, 5) == 31, s1, s2, slot_off);
        return;
    }

    bool sp = sb && reg_at(i, 5) == 31;
    bool sp_via_s2 = false;
    if (sp) {
        int64_t imm;
        unsigned scale;
        switch (cls) {
        case X18_CLS_LDST_PRE_POST:
            reject(p, "sp writeback"); return;
        case X18_CLS_LDST_PAIR:
            if ((i >> 23) & 1) { reject(p, "sp writeback"); return; }
            scale = (i & (1u << 26)) ? (4u << (i >> 30)) : ((i >> 31) ? 8 : 4);
            imm = sext((i >> 15) & 127, 7) + 16 / scale;
            /* No room to compensate in the 7-bit field (Steam's arm64
             * client: stp x18, x17, [sp, #0x1f8]): address through S2 =
             * the original SP instead, immediate unchanged. */
            if (imm > 63) { sp_via_s2 = true; break; }
            j = (j & ~0x003f8000u) | (((uint32_t)imm & 127) << 15);
            break;
        case X18_CLS_LDST_UIMM:
            scale = ((i & (1u << 26)) && (i & (1u << 23))) ? 16 : (1u << (i >> 30));
            imm = ((i >> 10) & 4095) + 16 / scale;
            if (imm > 4095) { sp_via_s2 = true; break; }
            j = (j & ~0x003ffc00u) | ((uint32_t)imm << 10);
            break;
        case X18_CLS_LDST_UNSCALED:
            imm = sext((i >> 12) & 511, 9) + 16;
            if (imm > 255) { sp_via_s2 = true; break; }
            j = (j & ~0x001ff000u) | (((uint32_t)imm & 511) << 12);
            break;
        case X18_CLS_ADDSUB_IMM:
            if (i & (1u << 22)) { sp_via_s2 = true; break; }
            /* SUB's sign is opposite ADD's: preserve the original SP value. */
            imm = (int64_t)((i >> 10) & 4095) + ((i & (1u << 30)) ? -16 : 16);
            if (imm < 0 || imm > 4095) { reject(p, "imm overflow"); return; }
            j = (j & ~0x003ffc00u) | ((uint32_t)imm << 10);
            break;
        case X18_CLS_SIMD_LDST:
            if (i & (1u << 23)) { reject(p, "sp writeback"); return; }
            sp_via_s2 = true;
            break;
        default:
            /* No immediate to compensate: use saved scratch S2 as original SP,
             * then reload the TSD base before the unconditional writeback. */
            sp_via_s2 = true;
            break;
        }
    }
    uint32_t mrs = 0xd53bd060 | s2;
    uint32_t mask = 0x927df000 | (s2 << 5) | s2;
    uint32_t str = 0xf9000000 | ((slot_off / 8) << 10) | (s2 << 5) | s1;
    uint32_t ldp = 0xa8c10000 | (s2 << 10) | (31 << 5) | s1;
    emit(p, 0xa9bf0000 | (s2 << 10) | (31 << 5) | s1);
    emit(p, mrs);
    emit(p, mask);
    emit(p, 0xf9400000 | ((slot_off / 8) << 10) | (s2 << 5) | s1);
    if (sp_via_s2) {
        emit(p, 0x910043e0 | s2); /* add S2, sp, #16 */
        j = replace(j, 5, s2);
    }
    switch (cls) {
    case X18_CLS_CBZ:
    case X18_CLS_TBZ: {
        unsigned bits = cls == X18_CLS_CBZ ? 19 : 14;
        uint32_t mask_imm = ((1u << bits) - 1) << 5;
        p->alt_target = site + (uint64_t)(sext((i & mask_imm) >> 5, bits) * 4);
        emit(p, (j & ~mask_imm) | (4u << 5)); /* word 4 -> word 8 */
        break;
    }
    case X18_CLS_ADR:
    case X18_CLS_ADRP: {
        uint32_t raw = (((i >> 5) & 0x7ffff) << 2) | ((i >> 29) & 3);
        int64_t offset = sext(raw, 21);
        if (cls == X18_CLS_ADR) materialize(p, s1, site + (uint64_t)offset);
        else {
            uint64_t target = (site & ~UINT64_C(4095)) + (uint64_t)(offset * 4096);
            uint64_t here = (tramp + (uint64_t)p->nwords * 4) & ~UINT64_C(4095);
            uint64_t delta = target - here;
            if (delta > UINT64_C(0xfffff000) && delta < UINT64_C(0xffffffff00000000)) {
                reject(p, "imm overflow"); return;
            }
            uint32_t pages = (uint32_t)(delta >> 12) & 0x1fffff;
            emit(p, 0x90000000 | ((pages & 3) << 29) | ((pages >> 2) << 5) | s1);
        }
        break;
    }
    case X18_CLS_LDST_LITERAL: {
        uint64_t target = site + (uint64_t)(sext((i >> 5) & 0x7ffff, 19) * 4);
        materialize(p, s1, target);
        unsigned op = i >> 30;
        emit(p, (op == 0 ? 0xb9400000 : op == 1 ? 0xf9400000 : 0xb9800000) | (s1 << 5) | s1);
        break;
    }
    case X18_CLS_SYSREG:
        if (plain_sysreg(i)) emit(p, j);
        else emit(p, (i == 0xd53bd052 ? 0xf9400000 : 0xf9000000) |
                     ((tls_off / 8) << 10) | (s2 << 5) | s1);
        break;
    case X18_CLS_LDST_EXCL: /* LDAR/STLR only; exclusives were refused above */
    case X18_CLS_LDST_UIMM: case X18_CLS_LDST_UNSCALED:
    case X18_CLS_LDST_PRE_POST: case X18_CLS_LDST_REGOFF: case X18_CLS_LDST_PAIR:
    case X18_CLS_CAS: case X18_CLS_ATOMIC: case X18_CLS_ADDSUB_IMM:
    case X18_CLS_SIMD_LDST: case X18_CLS_FP_INT: case X18_CLS_SIMD_COPY:
    case X18_CLS_DP_REG: case X18_CLS_DP_IMM:
        emit(p, j); break;
    default: reject(p, "unknown"); return;
    }
    if (sp_via_s2) { emit(p, mrs); emit(p, mask); }
    emit(p, str);
    emit(p, ldp);
    p->back_idx = p->nwords;
    emit(p, 0);
    if (cls == X18_CLS_CBZ || cls == X18_CLS_TBZ) {
        emit(p, str);
        emit(p, ldp);
        p->alt_idx = p->nwords;
        emit(p, 0);
    }
    p->verdict = X18_OK;
}

size_t lxrt_x18_tramp_bytes(uint32_t i) {
    if (!lxrt_x18_touches(i)) return 0;
    bool sb, sd;
    int cls;
    (void)lxrt_x18_gpr_fields(i, &sb, &sd, &cls);
    if (sd && reg_at(i, 0) == 31) return 108; /* plan_writes_sp: 27 words */
    if (i == 0xd61f0240) return 4 * X18_BR_TRAMP_WORDS; /* plan_br */
    if (cls == X18_CLS_BR) return 44; /* stack, TSD, four MOVs, branch */
    if (cls == X18_CLS_LDST_LITERAL) return 48; /* four MOVs + load + seven */
    if (cls == X18_CLS_ADR || cls == X18_CLS_CBZ || cls == X18_CLS_TBZ) return 44;
    if (sb && reg_at(i, 5) == 31) return 44; /* temporary original-SP base */
    return 32;
}
