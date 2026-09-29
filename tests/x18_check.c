/* Host-only decoder audit; never executes a guest instruction. */
#include "x18.h"
#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SP_BIT (UINT64_C(1) << 31)
#define ZR_BIT (UINT64_C(1) << 32)
#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
static const unsigned shifts[] = {0, 5, 10, 16};
static int failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "self-test line %d: %s\n", __LINE__, #x); ++failures; \
} } while (0)

static void expect_fields(uint32_t i, int fields, int cls, bool sb, bool sd) {
    bool b, d;
    int c, f = lxrt_x18_gpr_fields(i, &b, &d, &c);
    CHECK(f == fields);
    if (f >= 0) { CHECK(c == cls); CHECK(b == sb); CHECK(d == sd); }
}
static struct x18_plan plan(uint32_t i) {
    struct x18_plan p;
    lxrt_x18_plan(i, 0x10000000, 0x20000000, 0x1f8, 0x1f0, &p);
    CHECK(p.nwords >= 0 && p.nwords <= 32);
    CHECK((size_t)p.nwords * 4 <= lxrt_x18_tramp_bytes(i));
    if (p.verdict == X18_OK) {
        if (p.terminal) {
            CHECK(p.back_idx == -1 && p.alt_idx == -1);
            CHECK(p.words[p.nwords - 1] == 0xd61f0200);
        } else {
            CHECK(p.back_idx >= 0 && p.back_idx < p.nwords);
            CHECK(p.words[p.back_idx] == 0);
        }
        if (p.alt_idx >= 0) CHECK(p.words[p.alt_idx] == 0);
    } else { CHECK(p.nwords == 0); CHECK(p.back_idx == -1 && p.alt_idx == -1); }
    return p;
}
static void unsupported(uint32_t i, const char *why) {
    struct x18_plan p = plan(i);
    CHECK(p.verdict == X18_UNSUPPORTED);
    CHECK(p.why && strcmp(p.why, why) == 0);
}
static void fixture_tests(void);
static void selftest(void) {
    fixture_tests();
    expect_fields(0x0c40ae40, 2, X18_CLS_SIMD_LDST, true, false);
    expect_fields(0x0c406e40, 2, X18_CLS_SIMD_LDST, true, false);
    expect_fields(0x0c402e40, 2, X18_CLS_SIMD_LDST, true, false);
    expect_fields(0x0c00ae40, 2, X18_CLS_SIMD_LDST, true, false);
    expect_fields(0x3c400a40, -1, 0, false, false); /* no SIMD LDTR */
    expect_fields(0xf94003f2, 3, X18_CLS_LDST_UIMM, true, false);
    expect_fields(0x3dc00240, 2, X18_CLS_LDST_UIMM, true, false);
    expect_fields(0x3ce26840, 10, X18_CLS_LDST_REGOFF, true, false);
    expect_fields(0x910003f2, 3, X18_CLS_ADDSUB_IMM, true, true);
    expect_fields(0xb10003f2, 3, X18_CLS_ADDSUB_IMM, true, false);
    expect_fields(0x9ac22032, 11, X18_CLS_DP_REG, false, false);
    expect_fields(0xdac01032, 3, X18_CLS_DP_REG, false, false);
    expect_fields(0xfa520820, 2, X18_CLS_DP_REG, false, false); /* immediate #18 */
    CHECK(!lxrt_x18_touches(0xfa520820));
    expect_fields(0x9e660252, 1, X18_CLS_FP_INT, false, false);
    expect_fields(0x9e670252, 2, X18_CLS_FP_INT, false, false);
    expect_fields(0x4e080e40, 2, X18_CLS_SIMD_COPY, false, false);
    expect_fields(0x4e183e52, 1, X18_CLS_SIMD_COPY, false, false);
    CHECK(!lxrt_x18_touches(0x4e120652)); /* dup element, all fields can spell 18 */
    CHECK(!lxrt_x18_touches(0x1e722a52)); /* FP-only fadd */
    CHECK(!lxrt_x18_touches(0x00000252));
    CHECK(!lxrt_x18_touches(0xffffffff));
    expect_fields(0xc8f2fc20, 11, X18_CLS_CAS, true, false);
    expect_fields(0x4872fc20, 11, X18_CLS_CASP, true, false);
    expect_fields(0xc8127c20, 11, X18_CLS_LDST_EXCL, true, false);
    expect_fields(0xc85f7e40, 3, X18_CLS_LDST_EXCL, true, false);
    expect_fields(0xc8720640, -1, 0, false, false); /* LDXP has fixed Rs=31 */
    expect_fields(0xc87f0640, 7, X18_CLS_LDST_EXCL, true, false);
    expect_fields(0xf8320040, 11, X18_CLS_ATOMIC, true, false);
    expect_fields(0xd53bd052, 1, X18_CLS_SYSREG, false, false);
    unsupported(0xc8127c20, "exclusive");
    unsupported(0x4872fc20, "casp pair");
    struct x18_plan branch = plan(0xd61f0240);
    CHECK(branch.verdict == X18_OK && branch.terminal && branch.nwords == 7);
    branch = plan(0xd63f0240);
    CHECK(branch.verdict == X18_OK && branch.terminal && branch.nwords == 9);
    CHECK(branch.words[6] == (0xd2800000u | (4u << 5) | 30u));
    CHECK(branch.words[7] == (0xf2800000u | (1u << 21) | (0x1000u << 5) | 30u));
    branch = plan(0xd65f0240);
    CHECK(branch.verdict == X18_OK && branch.terminal && branch.nwords == 7);
    /* Exclusives stay refused (LL/SC monitor); LDAR/STLR are ordered only. */
    struct x18_plan q;
    unsupported(0x88127fd4, "exclusive"); /* stxr w18, w20, [x30] */
    unsupported(0xc8320e65, "exclusive"); /* stxp w18, x5, x3, [x19] */
    q = plan(0x88dffe52); /* ldar w18, [x18]: libcef */
    CHECK(q.verdict == X18_OK && q.nwords == 8 && q.words[4] == 0x88dffc00);
    CHECK(q.words[3] == 0xf940fc20 && q.words[5] == 0xf900fc20);
    q = plan(0x88dffe12); /* ldar w18, [x16] */
    CHECK(q.verdict == X18_OK && q.words[4] == 0x88dffe00);
    q = plan(0xc89ffff2); /* stlr x18, [sp]: original SP through S2 */
    CHECK(q.verdict == X18_OK && q.words[4] == 0x910043e1 && q.words[5] == 0xc89ffc20);
    /* NZCV/FPCR/FPSR are plain EL0 state; other system registers are not. */
    unsupported(0xd53be052, "sysreg"); /* mrs x18, cntvct_el0 */
    unsupported(0xd53b0032, "sysreg"); /* mrs x18, ctr_el0 */
    q = plan(0xd53b4212); CHECK(q.verdict == X18_OK && q.nwords == 8 && q.words[4] == 0xd53b4200);
    q = plan(0xd51b4212); CHECK(q.verdict == X18_OK && q.nwords == 8 && q.words[4] == 0xd51b4200);
    q = plan(0xd53b4412); CHECK(q.verdict == X18_OK && q.words[4] == 0xd53b4400); /* fpcr */
    q = plan(0xd51b4432); CHECK(q.verdict == X18_OK && q.words[4] == 0xd51b4420); /* fpsr */
    /* Writes SP: S1 = T, then the direction-dependent restore. */
    q = plan(0x9100025f); /* mov sp, x18: libgallium, steamclient.so */
    CHECK(q.verdict == X18_OK && !q.terminal && q.nwords == 26);
    CHECK(q.words[0] == 0xa9bf07e0 && q.words[3] == 0xf940fc20 && q.words[4] == 0x91000000);
    CHECK(q.words[5] == 0x910003e1 && q.words[6] == 0xcb000021);
    CHECK(q.words[7] == 0xb6f801c1 && q.words[9] == 0xb6f80201); /* +14, +16 */
    CHECK(q.words[17] == 0x9100001f && q.words[18] == 0xa9400400 && q.words[19] == 0x910043ff);
    CHECK(q.back_idx == 20 && q.alt_idx == 24 && q.alt_target == 0x10000004);
    CHECK(q.words[21] == 0x8b000021 && q.words[23] == 0xa9400420 && q.words[25] == 0xd4200020);
    q = plan(0x8b3263ff); /* add sp, sp, x18: SP source via S2 */
    CHECK(q.verdict == X18_OK && q.nwords == 27);
    CHECK(q.words[4] == 0x910043e1 && q.words[5] == 0x8b206020);
    q = plan(0xd100825f); CHECK(q.verdict == X18_OK && q.words[4] == 0xd1008000);
    q = plan(0xb240025f); CHECK(q.verdict == X18_OK && q.words[4] == 0xb2400000);
    /* Steam's stp x18, x17, [sp, #0x1f8]: 7-bit field full, SP through S2. */
    q = plan(0xa91fc7f2);
    CHECK(q.verdict == X18_OK && q.nwords == 11);
    CHECK(q.words[4] == 0x910043e1 && q.words[5] == 0xa91fc420);
    CHECK(q.words[6] == 0xd53bd061 && q.words[7] == 0x927df021);
    unsupported(0xf8408ff2, "sp writeback");
    unsupported(0xa8c14bf2, "sp writeback");
    CHECK(plan(0xf97ffff2).verdict == X18_OK);
    CHECK(plan(0xf84f0bf2).verdict == X18_OK);
    CHECK(plan(0x914003f2).verdict == X18_OK);
    struct x18_plan shifted = plan(0x914077f2); /* add x18, sp, #0x1d000 */
    CHECK(shifted.verdict == X18_OK && shifted.words[4] == 0x910043e1);
    struct x18_plan p = plan(0xf94003f2);
    CHECK(p.verdict == X18_OK && p.nwords == 8);
    CHECK(p.words[0] == 0xa9bf07e0 && p.words[1] == 0xd53bd061);
    CHECK(p.words[2] == 0x927df021 && p.words[3] == 0xf940fc20);
    CHECK(p.words[4] == 0xf9400be0 && p.words[5] == 0xf900fc20);
    CHECK(p.words[6] == 0xa8c107e0 && p.back_idx == 7);
    p = plan(0x910083f2); CHECK(p.words[4] == 0x9100c3e0);
    p = plan(0xd10083f2); CHECK(p.words[4] == 0xd10043e0);
    p = plan(0x8b000252); /* x0 occupied: choose x1, x2 */
    CHECK(p.words[0] == 0xa9bf0be1 && p.words[4] == 0x8b000021);
    p = plan(0xb4ffffd2); /* cbz x18, .-8 */
    CHECK(p.nwords == 11 && p.back_idx == 7 && p.alt_idx == 10);
    CHECK(p.alt_target == 0x0ffffff8 && p.words[4] == 0xb4000080);
    CHECK(p.words[5] == p.words[8] && p.words[6] == p.words[9]);
    p = plan(0x37fffff2); CHECK(p.alt_target == 0x0ffffffc && p.alt_idx == 10);
    p = plan(0x90000012); CHECK(p.words[4] == 0x90f80000);
    p = plan(0x10000012); CHECK(p.nwords == 9 && p.words[4] == 0xd2800000);
    p = plan(0x58000012); CHECK(p.nwords == 10 && p.words[6] == 0xf9400000);
    p = plan(0x18000012); CHECK(p.words[6] == 0xb9400000);
    p = plan(0x98000012); CHECK(p.words[6] == 0xb9800000);
    p = plan(0xd53bd052); CHECK(p.words[4] == 0xf940f820);
    p = plan(0xd51bd052); CHECK(p.words[4] == 0xf900f820);
    p = plan(0xf8726be3); CHECK(p.nwords == 11 && p.words[4] == 0x910043e1);
    CHECK(p.words[6] == 0xd53bd061 && p.words[7] == 0x927df021);
    lxrt_x18_plan(0x90000012, 0, UINT64_C(0x200000000), 0, 8, &p);
    CHECK(p.verdict == X18_UNSUPPORTED && !strcmp(p.why, "imm overflow"));
    /* ADRP uses the page of the emitted word, including a page crossing. */
    lxrt_x18_plan(0x90000012, 0x1000, 0xff0, 0, 8, &p);
    CHECK(p.verdict == X18_OK && p.words[4] == 0x90000000);
    lxrt_x18_plan(0x91000252, 0, 0, 7, 8, &p);
    CHECK(p.verdict == X18_UNSUPPORTED);
    /* Full-width absolute targets exercise all four materialization words. */
    lxrt_x18_plan(0x10000012, UINT64_C(0x123456789abcdef0), 0, 0, 8, &p);
    CHECK(p.verdict == X18_OK && p.nwords == 11);
    CHECK(p.words[4] == (0xd2800000u | (0xdef0u << 5)));
    CHECK(p.words[7] == (0xf2e00000u | (0x1234u << 5)));
    lxrt_x18_plan(0x58000012, UINT64_C(0x123456789abcdef0), 0, 0, 8, &p);
    CHECK(p.verdict == X18_OK && p.nwords == 12 && p.words[8] == 0xf9400000);
    lxrt_x18_plan(0x90000012, UINT64_C(0x100000000), 0, 0, 8, &p);
    CHECK(p.verdict == X18_UNSUPPORTED); /* +4 GiB is one page too far */
    lxrt_x18_plan(0x90000012, UINT64_C(0xfffff000), 0, 0, 8, &p);
    CHECK(p.verdict == X18_OK && p.words[4] == 0xf07fffe0);
    lxrt_x18_plan(0x90000012, 0, UINT64_C(0x100000000), 0, 8, &p);
    CHECK(p.verdict == X18_OK && p.words[4] == 0x90800000);
    p = plan(0xd503201f); CHECK(p.verdict == X18_NOT_A_SITE);
    /* Both edges of each immediate's signed/unsigned range. */
    for (unsigned imm = 0; imm < 4096; ++imm) {
        p = plan(0xf94003f2 | (imm << 10));
        CHECK(p.verdict == X18_OK);
        if (imm <= 4093) CHECK(((p.words[4] >> 10) & 4095) == imm + 2);
        else CHECK(p.words[4] == 0x910043e1 &&
                   ((p.words[5] >> 10) & 4095) == imm);
    }
    for (unsigned imm = 0; imm < 512; ++imm) {
        int signed_imm = imm < 256 ? (int)imm : (int)imm - 512;
        p = plan(0xf84003f2 | (imm << 12));
        CHECK(p.verdict == X18_OK);
        if (signed_imm > 239) CHECK(p.words[4] == 0x910043e1);
    }
    for (unsigned imm = 0; imm < 128; ++imm) {
        int signed_imm = imm < 64 ? (int)imm : (int)imm - 128;
        p = plan(0xa9400ff2 | (imm << 15));
        CHECK(p.verdict == X18_OK);
        if (signed_imm <= 61) CHECK(((p.words[4] >> 15) & 127) == ((imm + 2) & 127));
        else CHECK(p.words[4] == 0x910043e1);
        p = plan(0x28400ff2 | (imm << 15));
        CHECK(p.verdict == X18_OK);
    }
    fprintf(stderr, "self-tests: %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
}

static uint64_t text_regs(const char *s) {
    uint64_t regs = 0;
    for (size_t j = 0; s[j] && !(s[j] == '/' && s[j + 1] == '/'); ++j) {
        if (j && (isalnum((unsigned char)s[j - 1]) || s[j - 1] == '_')) continue;
        size_t n = 0;
        unsigned r = 33;
        if ((s[j] == 'x' || s[j] == 'w') && isdigit((unsigned char)s[j + 1])) {
            size_t k = j + 1;
            r = 0;
            while (isdigit((unsigned char)s[k])) { r = r * 10 + (unsigned)(s[k++] - '0'); }
            n = k - j;
            if (r > 30) r = 33;
        } else if (!strncmp(s + j, "xzr", 3) || !strncmp(s + j, "wzr", 3)) { n = 3; r = 32; }
        else if (!strncmp(s + j, "wsp", 3)) { n = 3; r = 31; }
        else if (!strncmp(s + j, "sp", 2)) { n = 2; r = 31; }
        if (n && !isalnum((unsigned char)s[j + n]) && s[j + n] != '_' && r <= 32)
            regs |= UINT64_C(1) << r;
    }
    return regs;
}
static uint64_t implied_regs(uint32_t i, int fields, bool sb, bool sd) {
    uint64_t regs = 0;
    if (fields < 0) return 0;
    for (unsigned f = 0; f < 4; ++f) if (fields & (1 << f)) {
        unsigned r = (i >> shifts[f]) & 31;
        if (r == 31 && !((f == 0 && sd) || (f == 1 && sb))) r = 32;
        regs |= UINT64_C(1) << r;
    }
    return regs;
}
/* Independently assembled with llvm-mc (armv8.5-a+lse+fp16), with
 * register sets read from llvm-objdump. CASP's implicit odd registers are
 * excluded here because the API describes only encoded five-bit fields. */
static void fixture_tests(void) {
    static const struct {
        uint32_t word;
        uint64_t regs;
        enum x18_verdict verdict;
    } cases[] = {
        {0xf97ffff2, UINT64_C(0x80040000), X18_OK}, /* ldr x18, [sp, #0x7ff8] */
        {0xf85003f2, UINT64_C(0x80040000), X18_OK}, /* ldur x18, [sp, #-0x100] */
        {0xf84efbf2, UINT64_C(0x80040000), X18_OK}, /* ldtr x18, [sp, #0xef] */
        {0x28600ff2, UINT64_C(0x80040008), X18_OK}, /* ldnp w18, w3, [sp, #-0x100] */
        {0x695f8ff2, UINT64_C(0x80040008), X18_OK}, /* ldpsw x18, x3, [sp, #0xfc] */
        {0xacc10e52, UINT64_C(0x40000), X18_OK}, /* ldp q18, q3, [x18], #0x20 */
        {0x2d410e52, UINT64_C(0x40000), X18_OK}, /* ldp s18, s3, [x18, #0x8] */
        {0x6d410e52, UINT64_C(0x40000), X18_OK}, /* ldp d18, d3, [x18, #0x10] */
        {0x3cf26852, UINT64_C(0x40004), X18_OK}, /* ldr q18, [x2, x18] */
        {0xf8636bf2, UINT64_C(0x80040008), X18_OK}, /* ldr x18, [sp, x3] */
        {0x8b2343f2, UINT64_C(0x80040008), X18_OK}, /* add x18, sp, w3, uxtw */
        {0xd10043f2, UINT64_C(0x80040000), X18_OK}, /* sub x18, sp, #0x10 */
        {0xc85f7c52, UINT64_C(0x40004), X18_UNSUPPORTED}, /* ldxr x18, [x2] */
        {0x885ffff2, UINT64_C(0x80040000), X18_UNSUPPORTED}, /* ldaxr w18, [sp] */
        {0xc812ffe3, UINT64_C(0x80040008), X18_UNSUPPORTED}, /* stlxr w18, x3, [sp] */
        {0xc87f0ff2, UINT64_C(0x80040008), X18_UNSUPPORTED}, /* ldxp x18, x3, [sp] */
        {0xc82313f2, UINT64_C(0x80040018), X18_UNSUPPORTED}, /* stxp w3, x18, x4, [sp] */
        {0xc8dffff2, UINT64_C(0x80040000), X18_OK}, /* ldar x18, [sp] */
        {0xc89ffff2, UINT64_C(0x80040000), X18_OK}, /* stlr x18, [sp] */
        {0x4872ffe2, UINT64_C(0x80040004), X18_UNSUPPORTED}, /* caspal x18, x19, x2, x3, [sp] */
        {0xc8f2ffe3, UINT64_C(0x80040008), X18_OK}, /* casal x18, x3, [sp] */
        {0xf83203e3, UINT64_C(0x80040008), X18_OK}, /* ldadd x18, x3, [sp] */
        {0x382383f2, UINT64_C(0x80040008), X18_OK}, /* swpb w3, w18, [sp] */
        {0xd95ff3f2, UINT64_C(0x80040000), X18_OK}, /* ldapur x18, [sp, #-0x1] */
        {0xf8bfc3f2, UINT64_C(0x80040000), X18_OK}, /* ldapr x18, [sp] */
        {0x10ffffd2, UINT64_C(0x40000), X18_OK}, /* adr x18, 0x5c <.text+0x5c> */
        {0x90000012, UINT64_C(0x40000), X18_OK}, /* adrp x18, 0x0 <.text> */
        {0x18000052, UINT64_C(0x40000), X18_OK}, /* ldr w18, 0x74 <.text+0x74> */
        {0x98ffffd2, UINT64_C(0x40000), X18_OK}, /* ldrsw x18, 0x68 <.text+0x68> */
        {0xd8000040, UINT64_C(0x0), X18_NOT_A_SITE}, /* prfm pldl1keep, 0x7c <.text+0x7c> */
        {0xd61f0240, UINT64_C(0x40000), X18_OK}, /* br x18 */
        {0xd63f0240, UINT64_C(0x40000), X18_OK}, /* blr x18 */
        {0xd65f0240, UINT64_C(0x40000), X18_OK}, /* ret x18 */
        {0xd53bd052, UINT64_C(0x40000), X18_OK}, /* mrs x18, TPIDR_EL0 */
        {0xd51bd052, UINT64_C(0x40000), X18_OK}, /* msr TPIDR_EL0, x18 */
        {0xd53b4212, UINT64_C(0x40000), X18_OK}, /* mrs x18, NZCV */
        {0x4c407252, UINT64_C(0x40000), X18_OK}, /* ld1 { v18.16b }, [x18] */
        {0x4cd27072, UINT64_C(0x40008), X18_OK}, /* ld1 { v18.16b }, [x3], x18 */
        {0x4cdf7243, UINT64_C(0x40000), X18_OK}, /* ld1 { v3.16b }, [x18], #16 */
        {0x4cd273e3, UINT64_C(0x80040000), X18_UNSUPPORTED}, /* ld1 { v3.16b }, [sp], x18 */
        {0x0d400243, UINT64_C(0x40000), X18_OK}, /* ld1 { v3.b }[0], [x18] */
        {0x0dd20043, UINT64_C(0x40004), X18_OK}, /* ld1 { v3.b }[0], [x2], x18 */
        {0x4d40c243, UINT64_C(0x40000), X18_OK}, /* ld1r { v3.16b }, [x18] */
        {0x4df2c043, UINT64_C(0x40004), X18_OK}, /* ld2r { v3.16b, v4.16b }, [x2], x18 */
        {0x4cdf0e41, UINT64_C(0x40000), X18_OK}, /* ld4 { v1.2d, v2.2d, v3.2d, v4.2d }, [x18], #64 */
        {0x9e660072, UINT64_C(0x40000), X18_OK}, /* fmov x18, d3 */
        {0x9e670243, UINT64_C(0x40000), X18_OK}, /* fmov d3, x18 */
        {0x1e260072, UINT64_C(0x40000), X18_OK}, /* fmov w18, s3 */
        {0x1e270243, UINT64_C(0x40000), X18_OK}, /* fmov s3, w18 */
        {0x9eae0072, UINT64_C(0x40000), X18_OK}, /* fmov x18, v3.d[1] */
        {0x9eaf0243, UINT64_C(0x40000), X18_OK}, /* fmov v3.d[1], x18 */
        {0x1ee60072, UINT64_C(0x40000), X18_OK}, /* fmov w18, h3 */
        {0x1ee70243, UINT64_C(0x40000), X18_OK}, /* fmov h3, w18 */
        {0x9e780072, UINT64_C(0x40000), X18_OK}, /* fcvtzs x18, d3 */
        {0x1ef90072, UINT64_C(0x40000), X18_OK}, /* fcvtzu w18, h3 */
        {0x9e240072, UINT64_C(0x40000), X18_OK}, /* fcvtas x18, s3 */
        {0x1e650072, UINT64_C(0x40000), X18_OK}, /* fcvtau w18, d3 */
        {0x9e300072, UINT64_C(0x40000), X18_OK}, /* fcvtms x18, s3 */
        {0x1e610072, UINT64_C(0x40000), X18_OK}, /* fcvtnu w18, d3 */
        {0x9ee80072, UINT64_C(0x40000), X18_OK}, /* fcvtps x18, h3 */
        {0x1e690072, UINT64_C(0x40000), X18_OK}, /* fcvtpu w18, d3 */
        {0x9e58e072, UINT64_C(0x40000), X18_OK}, /* fcvtzs x18, d3, #0x8 */
        {0x1e19e072, UINT64_C(0x40000), X18_OK}, /* fcvtzu w18, s3, #0x8 */
        {0x9e620243, UINT64_C(0x40000), X18_OK}, /* scvtf d3, x18 */
        {0x1e230243, UINT64_C(0x40000), X18_OK}, /* ucvtf s3, w18 */
        {0x9e42e243, UINT64_C(0x40000), X18_OK}, /* scvtf d3, x18, #0x8 */
        {0x1ec3e243, UINT64_C(0x40000), X18_OK}, /* ucvtf h3, w18, #0x8 */
        {0x4e080e43, UINT64_C(0x40000), X18_OK}, /* dup v3.2d, x18 */
        {0x4e181e43, UINT64_C(0x40000), X18_OK}, /* mov v3.d[1], x18 */
        {0x4e032c72, UINT64_C(0x40000), X18_OK}, /* smov x18, v3.b[1] */
        {0x0e0c3c72, UINT64_C(0x40000), X18_OK}, /* mov w18, v3.s[1] */
        {0x4e030652, UINT64_C(0x0), X18_NOT_A_SITE}, /* dup v18.16b, v18.b[1] */
        {0x6e030e52, UINT64_C(0x0), X18_NOT_A_SITE}, /* mov v18.b[1], v18.b[1] */
        {0x5e180652, UINT64_C(0x0), X18_NOT_A_SITE}, /* mov d18, v18.d[1] */
        {0xdac01052, UINT64_C(0x40004), X18_OK}, /* clz x18, x2 */
        {0xfa521a40, UINT64_C(0x40000), X18_OK}, /* ccmp x18, #0x12, #0x0, ne */
        {0xfa521060, UINT64_C(0x40008), X18_OK}, /* ccmp x3, x18, #0x0, ne */
        {0x9b054883, UINT64_C(0x40038), X18_OK}, /* madd x3, x4, x5, x18 */
        {0x9bc47c72, UINT64_C(0x40018), X18_OK}, /* umulh x18, x3, x4 */
        {0x93c44872, UINT64_C(0x40018), X18_OK}, /* extr x18, x3, x4, #0x12 */
        {0xb240025f, UINT64_C(0x80040000), X18_OK}, /* orr sp, x18, #0x1 */
    };
    for (size_t j = 0; j < ARRAY_LEN(cases); ++j) {
        bool sb, sd;
        int cls, fields = lxrt_x18_gpr_fields(cases[j].word, &sb, &sd, &cls);
        CHECK(implied_regs(cases[j].word, fields, sb, sd) == cases[j].regs);
        CHECK(lxrt_x18_touches(cases[j].word) == ((cases[j].regs & (UINT64_C(1) << 18)) != 0));
        struct x18_plan p = plan(cases[j].word);
        CHECK(p.verdict == cases[j].verdict);
    }
}

static void print_regs(uint64_t set) {
    putchar('{');
    bool first = true;
    for (unsigned r = 0; r <= 32; ++r) if (set & (UINT64_C(1) << r)) {
        if (!first) putchar(',');
        if (r < 31) printf("x%u", r); else fputs(r == 31 ? "sp" : "zr", stdout);
        first = false;
    }
    putchar('}');
}
struct count { char name[80]; unsigned long n; };
static void bump(struct count *c, size_t cap, const char *name) {
    for (size_t j = 0; j < cap; ++j) if (!c[j].name[0] || !strcmp(c[j].name, name)) {
        snprintf(c[j].name, sizeof(c[j].name), "%s", name); ++c[j].n; return;
    }
    fputs("counter capacity exceeded\n", stderr); exit(2);
}
static unsigned long count_of(struct count *c, size_t cap, const char *name) {
    for (size_t j = 0; j < cap; ++j) if (!strcmp(c[j].name, name)) return c[j].n;
    return 0;
}
static void dump_plan(const struct x18_plan *p, const char *dir, const char *mnemonic,
                      unsigned long n, uint32_t original, const char *line, FILE *manifest) {
    char path[1024];
    if (snprintf(path, sizeof(path), "%s/%s-%lu.bin", dir, mnemonic, n) >= (int)sizeof(path)) exit(2);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(2); }
    for (int j = 0; j < p->nwords; ++j) {
        unsigned char bytes[4];
        for (unsigned b = 0; b < 4; ++b) bytes[b] = (unsigned char)(p->words[j] >> (8 * b));
        if (fwrite(bytes, 1, 4, f) != 4) { perror(path); exit(2); }
    }
    if (fclose(f)) { perror(path); exit(2); }
    fprintf(manifest, "%s\t%08" PRIx32 "\tback=%d alt=%d target=0x%" PRIx64 "\t%s",
            path, original, p->back_idx, p->alt_idx, p->alt_target, line);
}
int main(int argc, char **argv) {
    unsigned dump_n = 0;
    const char *dir = "build/x18-dumps", *input = NULL;
    for (int j = 1; j < argc; ++j) {
        if (!strcmp(argv[j], "--dump") && j + 1 < argc) dump_n = (unsigned)strtoul(argv[++j], NULL, 10);
        else if (!strcmp(argv[j], "--dump-dir") && j + 1 < argc) dir = argv[++j];
        else if (!strcmp(argv[j], "--self-test")) { selftest(); return failures != 0; }
        else if (!input) input = argv[j];
        else { fprintf(stderr, "usage: %s [--dump N] [--dump-dir DIR] INPUT\n", argv[0]); return 2; }
    }
    if (!input) { selftest(); return failures != 0; }
    FILE *f = fopen(input, "r"), *manifest = NULL;
    if (!f) { perror(input); return 2; }
    if (dump_n) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/manifest.tsv", dir);
        manifest = fopen(path, "w");
        if (!manifest) { perror(path); return 2; }
    }
    struct count why[32] = {{0}}, mnemonic[256] = {{0}}, dumps[256] = {{0}}, gap_forms[256] = {{0}};
    unsigned long sizes[33] = {0}, lines = 0, recognised = 0, mismatch = 0, gaps = 0;
    unsigned long fp = 0, fn = 0, sites = 0, ok = 0, bad = 0, skipped = 0;
    unsigned long hidden_zr = 0, implicit_ret = 0, implicit_casp = 0, other_mismatch = 0;
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        uint32_t i;
        uint64_t address;
        char image[256], mn[80];
        int pos = 0;
        if (sscanf(line, "%255s %" SCNx64 ": %" SCNx32 " %79s %n", image, &address, &i, mn, &pos) != 4) {
            ++skipped; continue;
        }
        (void)address;
        ++lines;
        bool sb, sd;
        int cls, fields = lxrt_x18_gpr_fields(i, &sb, &sd, &cls);
        uint64_t actual = text_regs(line + pos), implied = implied_regs(i, fields, sb, sd);
        if (fields >= 0) {
            ++recognised;
            if (actual != implied) {
                ++mismatch;
                if ((implied ^ actual) == ZR_BIT && (implied & ZR_BIT)) ++hidden_zr;
                else if (!strcmp(mn, "ret") && !actual && implied == (UINT64_C(1) << 30)) ++implicit_ret;
                else if (cls == X18_CLS_CASP && (actual & implied) == implied) ++implicit_casp;
                else ++other_mismatch;
                if (mismatch <= 40) {
                    fputs("MISMATCH decoder=", stdout); print_regs(implied);
                    fputs(" text=", stdout); print_regs(actual); printf(" %s", line);
                }
            }
        } else if (actual) {
            bump(gap_forms, ARRAY_LEN(gap_forms), mn);
            if (++gaps <= 40) printf("GAP %s", line);
        }
        bool want = (actual & (UINT64_C(1) << 18)) != 0, got = lxrt_x18_touches(i);
        if (got && !want) { if (++fp <= 40) printf("FALSE_POSITIVE %s", line); }
        if (!got && want) { if (++fn <= 40) printf("FALSE_NEGATIVE %s", line); }
        if (want) {
            ++sites;
            bump(mnemonic, ARRAY_LEN(mnemonic), mn);
            struct x18_plan p = plan(i);
            if (p.verdict == X18_OK) {
                ++ok; ++sizes[p.nwords];
                unsigned long n = count_of(dumps, ARRAY_LEN(dumps), mn);
                if (n < dump_n) {
                    bump(dumps, ARRAY_LEN(dumps), mn);
                    dump_plan(&p, dir, mn, n + 1, i, line, manifest);
                }
            } else if (p.verdict == X18_UNSUPPORTED) {
                ++bad; bump(why, ARRAY_LEN(why), p.why);
            }
        }
    }
    if (ferror(f)) { perror(input); return 2; }
    fclose(f);
    if (manifest) fclose(manifest);
    printf("INPUT %s\n", input);
    printf("ALL lines=%lu skipped=%lu recognised=%lu mismatches=%lu gaps=%lu\n", lines, skipped, recognised, mismatch, gaps);
    printf("MISMATCH_REASONS hidden_zr=%lu implicit_ret=%lu implicit_casp=%lu other=%lu\n", hidden_zr, implicit_ret, implicit_casp, other_mismatch);
    for (size_t j = 0; j < ARRAY_LEN(gap_forms) && gap_forms[j].name[0]; ++j)
        printf("GAP_FORM %s=%lu\n", gap_forms[j].name, gap_forms[j].n);
    printf("TOUCHES false_positives=%lu false_negatives=%lu\n", fp, fn);
    printf("X18 sites=%lu ok=%lu unsupported=%lu not_a_site=%lu\n", sites, ok, bad, sites - ok - bad);
    for (size_t j = 0; j < ARRAY_LEN(why) && why[j].name[0]; ++j) printf("UNSUPPORTED %s=%lu\n", why[j].name, why[j].n);
    for (size_t j = 0; j < ARRAY_LEN(sizes); ++j) if (sizes[j]) printf("SIZE %zu bytes=%lu\n", j * 4, sizes[j]);
    unsigned distinct = 0, dumped = 0;
    for (size_t j = 0; j < ARRAY_LEN(mnemonic) && mnemonic[j].name[0]; ++j) ++distinct;
    for (size_t j = 0; j < ARRAY_LEN(dumps) && dumps[j].name[0]; ++j) ++dumped;
    printf("FORMS x18_mnemonics=%u dumped_mnemonics=%u\n", distinct, dumped);
    /* Alias/implicit-register mismatches are deliberately NOT hidden. They
     * do not make detection fail; all other mismatches and gaps do. */
    return fp || fn || other_mismatch || gaps || failures ? 1 : 0;
}
