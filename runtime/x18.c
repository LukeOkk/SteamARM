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

void lxrt_x18_plan(uint32_t i, uint64_t site, uint64_t tramp,
                   unsigned slot_off, unsigned tls_off, struct x18_plan *p) {
    memset(p, 0, sizeof(*p));
    p->back_idx = p->alt_idx = -1;
    if (!lxrt_x18_touches(i)) return;
    bool sb, sd;
    int cls, fields = lxrt_x18_gpr_fields(i, &sb, &sd, &cls);
    if (cls == X18_CLS_LDST_EXCL) { reject(p, "exclusive"); return; }
    if (cls == X18_CLS_CASP) { reject(p, "casp pair"); return; }
    if (sd && reg_at(i, 0) == 31) { reject(p, "writes sp"); return; }
    /* NZCV (mrs x18, nzcv / msr nzcv, x18: libcef's multiprecision code
     * keeps the carry in x18) goes through the generic path unchanged: the
     * stp/mrs/and/ldr before it and the str/ldp after it leave the flags
     * alone. Other system registers stay refused. */
    bool nzcv = i == 0xd53b4212 || i == 0xd51b4212;
    if (cls == X18_CLS_SYSREG && i != 0xd53bd052 && i != 0xd51bd052 && !nzcv) {
        reject(p, "sysreg"); return;
    }
    if ((slot_off & 7) || slot_off >= 32768 || (tls_off & 7) || tls_off >= 32768 ||
        (site & 3) || (tramp & 3)) { reject(p, "imm overflow"); return; }
    if (cls == X18_CLS_BR) {
        /* x18 belongs to Darwin. Use the ABI's intra-procedure-call scratch
         * x16 for the guest target, leaving the host's physical x18 intact.
         * A BLR must expose the original guest return PC in x30. */
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
        if (nzcv) { emit(p, j); break; }
        emit(p, (i == 0xd53bd052 ? 0xf9400000 : 0xf9000000) |
                ((tls_off / 8) << 10) | (s2 << 5) | s1);
        break;
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
    if (cls == X18_CLS_BR) return 44; /* stack, TSD, four MOVs, branch */
    if (cls == X18_CLS_LDST_LITERAL) return 48; /* four MOVs + load + seven */
    if (cls == X18_CLS_ADR || cls == X18_CLS_CBZ || cls == X18_CLS_TBZ) return 44;
    if (sb && reg_at(i, 5) == 31) return 44; /* temporary original-SP base */
    return 32;
}
