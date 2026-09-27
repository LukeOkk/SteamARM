// System registers Darwin will not let userspace read.
//
// Linux emulates EL0 reads of the ID registers; Darwin traps them, and it traps
// them for its own native code too -- `mrs x0, ctr_el0` raises SIGILL in a
// plain Mach-O binary. Measured in benchmarks/stage5-sysreg.txt.
//
// CTR_EL0 matters because every icache flush reads it: __builtin___clear_cache,
// glibc's cacheflush, and any JIT that manages its own code cache. FEX does it
// constantly. So it is rewritten, exactly like TPIDR_EL0 and svc, into a
// constant the runtime supplies.
//
// The cache operations themselves -- dc cvau, ic ivau, dsb, isb -- ARE allowed
// at EL0 on Darwin, so only the register read needs replacing.

#include "lxrt.h"

#include <stdio.h>
#include <stdbool.h>
#include <sys/sysctl.h>

static uint32_t g_ctr;

// CTR_EL0 as the architecture defines it:
//   [3:0]   IminLine  log2 of the icache line in 32-bit words
//   [15:14] L1Ip      instruction cache policy; 0b11 = PIPT
//   [19:16] DminLine  log2 of the dcache line in words
//   [23:20] ERG       exclusives reservation granule
//   [27:24] CWG       cache writeback granule
//   [31]    RES1
//
// A SMALLER line size is always safe: it makes a flush loop do more, smaller
// operations over the same range. So when sysctl gives nothing, fall back to
// 16 bytes rather than guessing large and skipping cache lines.
uint32_t lxrt_synthetic_ctr_el0(void)
{
    if (g_ctr)
        return g_ctr;

    // hw.cachelinesize is 128 on Apple Silicon, and that is the L2 line. The
    // line the maintenance instructions work on is 64 bytes: Apple's own
    // sys_icache_invalidate (libsystem_platform, otool -tV) rounds down with
    // `and x9, x0, #~0x3f` and steps `add x9, x9, #0x40` between `ic ivau`.
    // Reporting 128 here made every icache flush stride 128 and leave every
    // other line stale; FEX's freshly emitted blocks then executed stale bytes
    // and died with SIGILL on a perfectly legal `ldrb` (benchmarks/
    // stage5-sysreg.txt, CORRECTION). A smaller stride is always correct, so
    // 64 bytes is what the guest is told, for icache and dcache alike.
    uint64_t line = 64;

    uint32_t words = (uint32_t)(line / 4);
    uint32_t log2w = 0;
    while ((1u << (log2w + 1)) <= words)
        log2w++;

    g_ctr = (1u << 31)          // RES1
          | (log2w << 24)       // CWG
          | (4u << 20)          // ERG, 16 words -- only used by exclusives
          | (log2w << 16)       // DminLine
          | (3u << 14)          // L1Ip = PIPT
          | log2w;              // IminLine
    return g_ctr;
}

// ---------------------------------------------------------- ID registers
//
// Darwin traps every ID_AA64* read at EL0. FEX reads eleven of them to decide
// which instruction encodings it may emit, so they cannot be left to fault and
// they cannot be answered with zero -- FEX would then emit only ARMv8.0 code on
// a core that has far more.
//
// macOS publishes the real feature set through sysctl (hw.optional.arm.FEAT_*),
// so the values are built from what the hardware actually has. The bias is
// deliberate and one-directional: a feature reported ABSENT costs performance,
// a feature reported PRESENT but missing is an illegal instruction at runtime.
// Anything not confirmed by sysctl is reported absent.

static bool feat(const char *name)
{
    char key[96];
    snprintf(key, sizeof key, "hw.optional.arm.%s", name);
    int v = 0;
    size_t sz = sizeof v;
    return sysctlbyname(key, &v, &sz, NULL, 0) == 0 && v != 0;
}

#define FIELD(v, shift) ((uint64_t)(v) << (shift))

static uint64_t id_aa64isar0(void)
{
    return FIELD(feat("FEAT_PMULL") ? 2 : feat("FEAT_AES") ? 1 : 0, 4)   // AES
         | FIELD(feat("FEAT_SHA1") ? 1 : 0, 8)                           // SHA1
         | FIELD(feat("FEAT_SHA512") ? 2 : feat("FEAT_SHA256") ? 1 : 0, 12)
         | FIELD(feat("FEAT_CRC32") ? 1 : 0, 16)
         | FIELD(feat("FEAT_LSE") ? 2 : 0, 20)                           // Atomic
         | FIELD(feat("FEAT_RDM") ? 1 : 0, 28)
         | FIELD(feat("FEAT_SHA3") ? 1 : 0, 32)
         | FIELD(feat("FEAT_DotProd") ? 1 : 0, 44)
         | FIELD(feat("FEAT_FHM") ? 1 : 0, 48)
         | FIELD(feat("FEAT_FlagM2") ? 2 : feat("FEAT_FlagM") ? 1 : 0, 52);
}

static uint64_t id_aa64isar1(void)
{
    return FIELD(feat("FEAT_DPB2") ? 2 : feat("FEAT_DPB") ? 1 : 0, 0)
         | FIELD(feat("FEAT_JSCVT") ? 1 : 0, 12)
         | FIELD(feat("FEAT_FCMA") ? 1 : 0, 16)
         | FIELD(feat("FEAT_LRCPC2") ? 2 : feat("FEAT_LRCPC") ? 1 : 0, 20)
         | FIELD(feat("FEAT_FRINTTS") ? 1 : 0, 32)
         | FIELD(feat("FEAT_SB") ? 1 : 0, 36)
         | FIELD(feat("FEAT_BF16") ? 1 : 0, 44)
         | FIELD(feat("FEAT_I8MM") ? 1 : 0, 52);
    // Pointer authentication (APA/API/GPA/GPI) is deliberately reported absent.
    // Apple's implementation is enabled per-process by the kernel, and a guest
    // that started signing pointers because an ID register said it could would
    // fault on the first authenticate.
}

static uint64_t id_aa64isar2(void)
{
    return FIELD(feat("FEAT_WFxT") ? 2 : 0, 0)
         | FIELD(feat("FEAT_RPRES") ? 1 : 0, 4);
}

static uint64_t id_aa64pfr0(void)
{
    // EL0/EL1 AArch64-only. FP and AdvSIMD use 0 for "present" and 1 for
    // "present with half-precision", which is the one field where 0 does not
    // mean absent -- 0xF does.
    uint64_t fp16 = feat("FEAT_FP16") ? 1 : 0;
    return FIELD(1, 0) | FIELD(1, 4)
         | FIELD(fp16, 16) | FIELD(fp16, 20)
         | FIELD(feat("FEAT_DIT") ? 1 : 0, 48)
         | FIELD(feat("FEAT_CSV2") ? 1 : 0, 56)
         | FIELD(feat("FEAT_CSV3") ? 1 : 0, 60);
}

static uint64_t id_aa64pfr1(void)
{
    // SME is reported ABSENT even though this core has it: it needs the
    // streaming state enabled through SMCR_EL1, which no guest can do here.
    return FIELD(feat("FEAT_BTI") ? 1 : 0, 0);
}

static uint64_t id_aa64mmfr0(void)
{
    // PARange 2 = 40 bits, ASIDBits 2 = 16. TGran fields use 0 for "supported"
    // on 4K and 64K and 1 for "supported" on 16K, which is the granule Darwin
    // actually uses.
    return FIELD(2, 0) | FIELD(2, 4) | FIELD(1, 20);
}

static uint64_t id_aa64mmfr2(void)
{
    return FIELD(feat("FEAT_LSE2") ? 1 : 0, 32);   // AT
}

// MRS encoding for these: op0=3, op1=0, CRn=0. The rewriter matches on that,
// and this switch distinguishes them by CRm and op2.
uint64_t lxrt_synthetic_sysreg(uint32_t insn)
{
    unsigned crm = (insn >> 8) & 0xF;
    unsigned op2 = (insn >> 5) & 0x7;

    if (crm == 0 && op2 == 0) {
        // MIDR_EL1. Implementer 'A' (Apple), architecture 0xF (use the ID
        // registers), a plausible part number. Guests use it for errata
        // workarounds and logging.
        return FIELD(0x61, 24) | FIELD(0xF, 16) | FIELD(0x030, 4) | 1;
    }
    if (crm == 4) {
        switch (op2) {
        case 0: return id_aa64pfr0();
        case 1: return id_aa64pfr1();
        default: return 0;          // ZFR0 and friends: no SVE here
        }
    }
    if (crm == 6) {
        switch (op2) {
        case 0: return id_aa64isar0();
        case 1: return id_aa64isar1();
        case 2: return id_aa64isar2();
        default: return 0;
        }
    }
    if (crm == 7) {
        switch (op2) {
        case 0: return id_aa64mmfr0();
        case 2: return id_aa64mmfr2();
        default: return 0;
        }
    }
    return 0;
}
