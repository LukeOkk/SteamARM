// An invariant position, for MoltenVK: one rounding in every pipeline.
//
// Counter-Strike 2 draws foliage and palm trunks in a depth prepass and then
// again with depth test EQUAL, each pass a pipeline of its own. Metal's fast
// math fuses multiplies and adds where it sees fit, and the same vertex code
// compiled in the two pipelines could round the position differently:
// wherever the depths came out a rounding apart EQUAL failed and the pixel
// stayed black -- clusters of black on de_dust2's palm trunks, dotted fronds
// (the user's screenshots, 2026-10-04, on KosmicKrisp and MoltenVK alike).
// KosmicKrisp compiles vertex shaders with "#pragma METAL fp contract(off)"
// (patches/kosmickrisp-18-vertex-no-fma-contraction.patch: the trunks whole,
// no frames lost). MoltenVK 1.4.2 has no such switch, but it compiles a
// shader with MTLCompileOptions.preserveInvariance when SPIRV-Cross reports
// an invariant position (SPIRVToMSLConversionResultInfo::isPositionInvariant,
// from Compiler::is_position_invariant(), MVKShaderModule.mm), and
// SPIRV-Cross reports it when the Position output the entry point writes
// is decorated Invariant. It then also declares the output
// [[position, invariant]] and computes everything the position depends on
// as temporaries, the same way in every shader.
//
// This pass decorates the Position output of every vertex, tessellation
// evaluation and geometry entry point Invariant: OpDecorate on a variable,
// OpMemberDecorate on a member of an output block. The new annotations go
// at the end of the annotation section; nothing else changes, not even the
// id bound. MoltenVK only, and off until it is measured in the game (the
// first runs were taken over by a player and shared the machine with
// builds): LXRT_VK_INVARIANT_POSITION=1 turns it on. Checked offline: 881
// of Counter-Strike 2's 5932 modules changed, all valid (spirv-val), all
// [[position, invariant]] in MoltenVK's SPIRV-Cross, all compile.
#include <stdint.h>
#include <stddef.h>
extern void *memcpy(void *, const void *, size_t);
extern void *malloc(size_t);
extern char *getenv(const char *);
extern int dprintf(int, const char *, ...);

enum {
    OP_ENTRY_POINT = 15, OP_TYPE_POINTER = 32, OP_FUNCTION = 54, OP_VARIABLE = 59,
    OP_DECORATE = 71, OP_MEMBER_DECORATE = 72,
    DEC_BUILTIN = 11, DEC_INVARIANT = 18, BUILTIN_POSITION = 0, STORAGE_OUTPUT = 3,
    MODEL_VERTEX = 0, MODEL_TESS_EVAL = 2, MODEL_GEOMETRY = 3,
};

int lxrt_invariant_wanted(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_VK_INVARIANT_POSITION");
        on = e && *e == '1';
    }
    return on;
}

static int is_annotation(uint32_t op)
{
    return (op >= 71 && op <= 75) || op == 332 || op == 5632 || op == 5633;
}

// Small sets: a module has one or two Position outputs.
#define SET_MAX 16
struct pairs { uint32_t n, a[SET_MAX], b[SET_MAX]; int full; };
static void add(struct pairs *s, uint32_t a, uint32_t b)
{
    for (uint32_t i = 0; i < s->n; i++)
        if (s->a[i] == a && s->b[i] == b) return;
    if (s->n < SET_MAX) { s->a[s->n] = a; s->b[s->n] = b; s->n++; }
    else s->full = 1;
}
static int has(const struct pairs *s, uint32_t a, uint32_t b)
{
    for (uint32_t i = 0; i < s->n; i++)
        if (s->a[i] == a && s->b[i] == b) return 1;
    return 0;
}

// Whether `id` is in the interface of a vertex, tessellation evaluation or
// geometry entry point (entry points come first, before every function).
static int in_stage_interface(const uint32_t *code, size_t n, uint32_t id)
{
    for (size_t i = 5; i < n; i += code[i] >> 16) {
        uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
        if (op == OP_FUNCTION)
            break;
        if (op != OP_ENTRY_POINT || wc < 4)
            continue;
        uint32_t model = code[i + 1];
        if (model != MODEL_VERTEX && model != MODEL_TESS_EVAL && model != MODEL_GEOMETRY)
            continue;
        // interface ids follow the name, a nul-terminated string from word 3
        uint32_t p = 3;
        while (p < wc) {
            uint32_t w = code[i + p++];
            if (!(w & 0xff000000u) || !(w & 0x00ff0000u) || !(w & 0x0000ff00u) || !(w & 0xffu))
                break;
        }
        for (; p < wc; p++)
            if (code[i + p] == id)
                return 1;
    }
    return 0;
}

// The pointee of an Output OpTypePointer `ptr`, or 0.
static uint32_t output_pointee(const uint32_t *code, size_t n, uint32_t ptr)
{
    for (size_t i = 5; i < n; i += code[i] >> 16) {
        uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
        if (op == OP_FUNCTION)
            break;
        if (op == OP_TYPE_POINTER && wc == 4 && code[i + 1] == ptr)
            return code[i + 2] == STORAGE_OUTPUT ? code[i + 3] : 0;
    }
    return 0;
}

// A copy with the position outputs decorated Invariant, or NULL when
// nothing changes (no such stage, no Position output, already invariant).
uint32_t *lxrt_spirv_invariant(const uint32_t *code, size_t size, size_t *out_size)
{
    if (!code || size < 20 || (size & 3) || code[0] != 0x07230203u)
        return NULL;
    const size_t n = size / 4;

    struct pairs posvar = {0}, posmem = {0};      // BuiltIn Position: (variable, 0), (struct, member)
    struct pairs invvar = {0}, invmem = {0};      // already Invariant
    size_t annot_end = 0;
    int stages = 0, in_funcs = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
        if (!wc || i + wc > n)
            return NULL;
        const uint32_t *w = code + i;
        if (op == OP_ENTRY_POINT && wc >= 4 &&
            (w[1] == MODEL_VERTEX || w[1] == MODEL_TESS_EVAL || w[1] == MODEL_GEOMETRY))
            stages = 1;
        else if (op == OP_FUNCTION)
            in_funcs = 1;
        else if (is_annotation(op) && !in_funcs) {
            annot_end = i + wc;
            if (op == OP_DECORATE && wc >= 4 && w[2] == DEC_BUILTIN && w[3] == BUILTIN_POSITION)
                add(&posvar, w[1], 0);
            else if (op == OP_DECORATE && wc >= 3 && w[2] == DEC_INVARIANT)
                add(&invvar, w[1], 0);
            else if (op == OP_MEMBER_DECORATE && wc >= 5 && w[3] == DEC_BUILTIN && w[4] == BUILTIN_POSITION)
                add(&posmem, w[1], w[2]);
            else if (op == OP_MEMBER_DECORATE && wc >= 4 && w[3] == DEC_INVARIANT)
                add(&invmem, w[1], w[2]);
        }
        i += wc;
    }
    if (!stages || !annot_end || (!posvar.n && !posmem.n) || posvar.full || posmem.full || invvar.full ||
        invmem.full)
        return NULL;

    // The Output variables to decorate: a Position variable, or one whose
    // type is a block with a Position member.
    struct pairs dec = {0}, mdec = {0};
    for (size_t i = 5; i < n; i += code[i] >> 16) {
        uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
        if (op == OP_FUNCTION)
            break;
        if (op != OP_VARIABLE || wc < 4 || code[i + 3] != STORAGE_OUTPUT)
            continue;
        uint32_t type = code[i + 1], var = code[i + 2];
        if (has(&posvar, var, 0)) {
            if (!has(&invvar, var, 0) && in_stage_interface(code, n, var))
                add(&dec, var, 0);
            continue;
        }
        if (!posmem.n)
            continue;
        uint32_t s = output_pointee(code, n, type);
        for (uint32_t j = 0; s && j < posmem.n; j++)
            if (posmem.a[j] == s && !has(&invmem, s, posmem.b[j]) && in_stage_interface(code, n, var))
                add(&mdec, s, posmem.b[j]);
    }
    if ((!dec.n && !mdec.n) || dec.full || mdec.full)
        return NULL;

    const size_t extra = (size_t)dec.n * 3 + (size_t)mdec.n * 4;
    uint32_t *o = malloc((n + extra) * 4);
    if (!o)
        return NULL;
    memcpy(o, code, annot_end * 4);
    size_t k = annot_end;
    for (uint32_t j = 0; j < dec.n; j++) {
        o[k++] = (3u << 16) | OP_DECORATE; o[k++] = dec.a[j]; o[k++] = DEC_INVARIANT;
    }
    for (uint32_t j = 0; j < mdec.n; j++) {
        o[k++] = (4u << 16) | OP_MEMBER_DECORATE; o[k++] = mdec.a[j]; o[k++] = mdec.b[j]; o[k++] = DEC_INVARIANT;
    }
    memcpy(o + k, code + annot_end, (n - annot_end) * 4);
    k += n - annot_end;
    *out_size = k * 4;
    {
        static int said;
        const char *d = getenv("LXRT_VK_DEBUG");
        if (d && *d == '1' && said++ < 3)
            dprintf(2, "[shim] spirv: position made invariant (%u variables, %u block members)\n", dec.n, mdec.n);
    }
    return o;
}
