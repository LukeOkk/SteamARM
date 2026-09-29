#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    X18_CLS_OTHER = 0,
    X18_CLS_LDST_UIMM,
    X18_CLS_LDST_UNSCALED,
    X18_CLS_LDST_PRE_POST,
    X18_CLS_LDST_REGOFF,
    X18_CLS_LDST_PAIR,
    X18_CLS_LDST_EXCL,
    X18_CLS_CAS,
    X18_CLS_CASP,
    X18_CLS_ATOMIC,
    X18_CLS_LDST_LITERAL,
    X18_CLS_ADDSUB_IMM,
    X18_CLS_ADR,
    X18_CLS_ADRP,
    X18_CLS_CBZ,
    X18_CLS_TBZ,
    X18_CLS_BR,
    X18_CLS_SYSREG,
    X18_CLS_SIMD_LDST,
    X18_CLS_FP_INT,
    X18_CLS_SIMD_COPY,
    X18_CLS_DP_REG,
    X18_CLS_DP_IMM,
};

/* Bits 0..3 identify GPR fields at instruction bits 0, 5, 10, 16.
 * A value of 31 is SP only in the fields marked by sp_base/sp_rd; otherwise
 * it is ZR. Fixed opcode bits are never operands. Unknown encodings return -1.
 * Encoded ZR operands remain present even when disassembly aliases hide them.
 * CASP's implicit second registers cannot be represented by this field mask.
 */
int lxrt_x18_gpr_fields(uint32_t insn, bool *sp_base, bool *sp_rd, int *cls);
bool lxrt_x18_touches(uint32_t insn);

enum x18_verdict { X18_NOT_A_SITE = 0, X18_OK = 1, X18_UNSUPPORTED = 2 };

struct x18_plan {
    enum x18_verdict verdict;
    /* UNSUPPORTED: exclusive, casp pair, sp writeback, sysreg, imm overflow,
     * or unknown. Otherwise NULL. */
    const char *why;
    uint32_t words[32];
    int nwords;
    int back_idx;       /* zero placeholder for branch to site+4 */
    bool terminal;      /* br/blr/ret x18: branches to guest target, no back edge */
    int alt_idx;        /* zero placeholder for a second exit, or -1 */
    uint64_t alt_target; /* its absolute destination: the conditional branch's
                          * target, or site+4 for a writes-sp plan */
};

/* site and tramp are instruction addresses. Offsets are byte offsets from
 * masked TPIDRRO_EL0, aligned to 8 and below 32768. Branch placeholders are
 * zero words; the caller must encode them before publishing the trampoline.
 * Unsupported/non-site plans contain no words and have both indices -1.
 */
void lxrt_x18_plan(uint32_t insn, uint64_t site, uint64_t tramp,
                   unsigned slot_off, unsigned tls_off, struct x18_plan *out);
/* Conservative allocation bound, including both branch placeholders. */
size_t lxrt_x18_tramp_bytes(uint32_t insn);
