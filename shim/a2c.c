// Alpha-to-coverage with one sample: a cut at alpha 0.5.
//
// With one sample a pixel, alpha-to-coverage can only cover the pixel or
// not; GPUs cut at about alpha 0.5. KosmicKrisp left a dither instead in a
// depth prepass drawn that way, then an opaque pass with depth EQUAL
// (tests/elf/vk_alphamask "1x a2c gradient + EQ": dots of the wrong colour
// along a gradient; MoltenVK passed). (De_dust2's speckled palms, first
// taken for this, were the depth prepass rounding apart instead:
// patches/kosmickrisp-18; Counter-Strike 2 makes no one-sample
// alpha-to-coverage pipeline.)
//
// Every fragment shader gets, before each return of its entry point,
//     if (SPEC && out0.a < 0.5) discard;
// with SPEC a boolean specialization constant, false unless set: the
// drivers fold the test away. A pipeline with one sample and
// alpha-to-coverage gets the constant set and alpha-to-coverage turned off
// (lxrt_a2c_pipelines, from shim/features.c); with two samples or more
// nothing changes. LXRT_VK_A2C_1X=hw leaves the hardware's dither.
#include <stdint.h>
#include <stddef.h>
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern char *getenv(const char *);
extern int dprintf(int, const char *, ...);

#define SPEC_ID 0xA2C1u          // < 65536: Metal function constant indices

enum {
    OP_ENTRY_POINT = 15, OP_EXECUTION_MODE = 16, OP_TYPE_BOOL = 20, OP_TYPE_FLOAT = 22, OP_TYPE_VECTOR = 23,
    OP_TYPE_POINTER = 32, OP_CONSTANT = 43, OP_SPEC_CONSTANT_FALSE = 49, OP_FUNCTION = 54, OP_FUNCTION_END = 56,
    OP_VARIABLE = 59, OP_LOAD = 61, OP_DECORATE = 71, OP_COMPOSITE_EXTRACT = 81, OP_LOGICAL_AND = 167,
    OP_FORD_LESS_THAN = 184, OP_SELECTION_MERGE = 247, OP_LABEL = 248, OP_BRANCH_CONDITIONAL = 250, OP_KILL = 252,
    OP_RETURN = 253,
    DEC_SPEC_ID = 1, DEC_LOCATION = 30, DEC_COMPONENT = 31, DEC_INDEX = 32,
    EXEC_MODEL_FRAGMENT = 4, EXEC_MODE_EARLY_FRAGMENT_TESTS = 9, STORAGE_OUTPUT = 3,
};

int lxrt_a2c_wanted(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_VK_A2C_1X");
        on = !(e && e[0] == 'h' && e[1] == 'w');
    }
    return on;
}

static int is_annotation(uint32_t op)
{
    return (op >= 71 && op <= 75) || op == 332 || op == 5632 || op == 5633;
}

// Small id sets: a module has a handful of decorated outputs.
#define SET_MAX 32
struct idset { uint32_t n, id[SET_MAX]; int full; };
static void set_add(struct idset *s, uint32_t id)
{
    if (s->n < SET_MAX) s->id[s->n++] = id;
    else s->full = 1;
}
static int set_has(const struct idset *s, uint32_t id)
{
    for (uint32_t i = 0; i < s->n; i++)
        if (s->id[i] == id) return 1;
    return 0;
}

// The fragment shader with the cut added, or 0 (not a fragment shader, or
// one this pass does not understand: left as it is, and its pipelines keep
// alpha-to-coverage).
uint32_t *lxrt_spirv_a2c(const uint32_t *code, size_t size, size_t *out_size)
{
    if (!code || size < 20 || size % 4 || code[0] != 0x07230203u)
        return 0;
    const size_t n = size / 4;
    uint32_t bound = code[3];
    if (bound < 2 || bound > (1u << 22))
        return 0;
    unsigned char *kind = calloc(bound, 1);       // 1 float32, 2 vec4 of it, 3 Output pointer to that
    uint32_t *pointee = calloc(bound, 4);
    if (!kind || !pointee) {
        free(kind);
        free(pointee);
        return 0;
    }
    struct idset loc0 = {0}, other = {0};         // Location 0; Index or Component not 0
    uint32_t entry = 0, frags = 0, bool_t = 0, float_t = 0, out_var = 0, out_type = 0, returns = 0;
    size_t annot_end = 0, first_func = 0;
    int bad = 0, in_entry = 0;
    for (size_t i = 5; i < n && !bad;) {
        uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
        if (!wc || i + wc > n) { bad = 1; break; }
        const uint32_t *w = code + i;
        if (op == OP_ENTRY_POINT && wc >= 3 && w[1] == EXEC_MODEL_FRAGMENT) {
            frags++;
            entry = w[2];
        } else if (op == OP_EXECUTION_MODE && wc >= 3 && w[1] == entry && w[2] == EXEC_MODE_EARLY_FRAGMENT_TESTS) {
            bad = 1;     // the depth write would happen before the cut
        } else if (is_annotation(op) && !first_func) {
            annot_end = i + wc;
            if (op == OP_DECORATE && wc >= 4) {
                if (w[2] == DEC_LOCATION && w[3] == 0) set_add(&loc0, w[1]);
                if ((w[2] == DEC_INDEX || w[2] == DEC_COMPONENT) && w[3] != 0) set_add(&other, w[1]);
                if (w[2] == DEC_SPEC_ID && w[3] == SPEC_ID) bad = 1;
            }
        } else if (op == OP_TYPE_BOOL && wc == 2) {
            bool_t = w[1];
        } else if (op == OP_TYPE_FLOAT && wc == 3 && w[2] == 32 && w[1] < bound) {
            kind[w[1]] = 1;
            float_t = w[1];
        } else if (op == OP_TYPE_VECTOR && wc == 4 && w[1] < bound && w[2] < bound && kind[w[2]] == 1 && w[3] == 4) {
            kind[w[1]] = 2;
        } else if (op == OP_TYPE_POINTER && wc == 4 && w[1] < bound && w[3] < bound && w[2] == STORAGE_OUTPUT &&
                   kind[w[3]] == 2) {
            kind[w[1]] = 3;
            pointee[w[1]] = w[3];
        } else if (op == OP_VARIABLE && wc >= 4 && !first_func && w[3] == STORAGE_OUTPUT && w[1] < bound &&
                   kind[w[1]] == 3 && set_has(&loc0, w[2]) && !set_has(&other, w[2])) {
            if (out_var) bad = 1;          // two candidates: not understood
            out_var = w[2];
            out_type = pointee[w[1]];
        } else if (op == OP_FUNCTION && wc >= 5) {
            if (!first_func) first_func = i;
            in_entry = w[2] == entry;
        } else if (op == OP_FUNCTION_END) {
            in_entry = 0;
        } else if (op == OP_RETURN && in_entry) {
            returns++;
        }
        i += wc;
    }
    free(kind);
    free(pointee);
    if (bad || frags != 1 || !out_var || !float_t || !returns || !annot_end || !first_func ||
        annot_end > first_func || loc0.full || other.full)
        return 0;

    const int new_bool = !bool_t;
    if (new_bool) bool_t = bound++;
    const uint32_t spec = bound++, half = bound++;
    const size_t extra = 4 + (new_bool ? 2 : 0) + 3 + 4 + (size_t)returns * (4 + 5 + 5 + 5 + 3 + 4 + 2 + 1 + 2);
    uint32_t *o = malloc((n + extra) * 4);
    if (!o)
        return 0;
    size_t k = 5;
    memcpy(o, code, 5 * 4);
    o[3] = bound + (uint32_t)returns * 6;
    in_entry = 0;
    for (size_t i = 5; i < n;) {
        uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
        if (i == first_func) {
            if (new_bool) { o[k++] = (2u << 16) | OP_TYPE_BOOL; o[k++] = bool_t; }
            o[k++] = (3u << 16) | OP_SPEC_CONSTANT_FALSE; o[k++] = bool_t; o[k++] = spec;
            o[k++] = (4u << 16) | OP_CONSTANT; o[k++] = float_t; o[k++] = half; o[k++] = 0x3f000000u;   // 0.5
        }
        if (op == OP_FUNCTION) in_entry = code[i + 2] == entry;
        if (op == OP_FUNCTION_END) in_entry = 0;
        if (op == OP_RETURN && in_entry) {
            uint32_t v = bound++, a = bound++, lt = bound++, c = bound++, kill = bound++, merge = bound++;
            o[k++] = (4u << 16) | OP_LOAD; o[k++] = out_type; o[k++] = v; o[k++] = out_var;
            o[k++] = (5u << 16) | OP_COMPOSITE_EXTRACT; o[k++] = float_t; o[k++] = a; o[k++] = v; o[k++] = 3;
            o[k++] = (5u << 16) | OP_FORD_LESS_THAN; o[k++] = bool_t; o[k++] = lt; o[k++] = a; o[k++] = half;
            o[k++] = (5u << 16) | OP_LOGICAL_AND; o[k++] = bool_t; o[k++] = c; o[k++] = spec; o[k++] = lt;
            o[k++] = (3u << 16) | OP_SELECTION_MERGE; o[k++] = merge; o[k++] = 0;
            o[k++] = (4u << 16) | OP_BRANCH_CONDITIONAL; o[k++] = c; o[k++] = kill; o[k++] = merge;
            o[k++] = (2u << 16) | OP_LABEL; o[k++] = kill;
            o[k++] = (1u << 16) | OP_KILL;
            o[k++] = (2u << 16) | OP_LABEL; o[k++] = merge;
            o[k++] = (1u << 16) | OP_RETURN;
            i += wc;
            continue;
        }
        memcpy(o + k, code + i, wc * 4);
        k += wc;
        i += wc;
        if (i == annot_end) {
            o[k++] = (4u << 16) | OP_DECORATE; o[k++] = spec; o[k++] = DEC_SPEC_ID; o[k++] = SPEC_ID;
        }
    }
    *out_size = k * 4;
    return o;
}

// The modules that have the cut, by handle.
#define MOD_SLOTS 65536
static uint64_t mods[MOD_SLOTS];

void lxrt_a2c_module_add(uint64_t module)
{
    for (uint32_t i = (uint32_t)(module >> 4) & (MOD_SLOTS - 1), t = 0; t < MOD_SLOTS; t++, i = (i + 1) & (MOD_SLOTS - 1)) {
        uint64_t cur = __atomic_load_n(&mods[i], __ATOMIC_ACQUIRE);
        if (cur == module) return;
        if (cur == 0 || cur == ~0ull) {
            uint64_t expect = cur;
            if (__atomic_compare_exchange_n(&mods[i], &expect, module, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
                return;
            if (expect == module) return;
        }
    }
}

static int module_has(uint64_t module)
{
    if (!module) return 0;
    for (uint32_t i = (uint32_t)(module >> 4) & (MOD_SLOTS - 1), t = 0; t < MOD_SLOTS; t++, i = (i + 1) & (MOD_SLOTS - 1)) {
        uint64_t cur = __atomic_load_n(&mods[i], __ATOMIC_ACQUIRE);
        if (cur == module) return 1;
        if (cur == 0) return 0;
    }
    return 0;
}

// A destroyed module's handle may come back for another one.
void lxrt_a2c_module_remove(uint64_t module)
{
    if (!module) return;
    for (uint32_t i = (uint32_t)(module >> 4) & (MOD_SLOTS - 1), t = 0; t < MOD_SLOTS; t++, i = (i + 1) & (MOD_SLOTS - 1)) {
        uint64_t cur = __atomic_load_n(&mods[i], __ATOMIC_ACQUIRE);
        if (cur == 0) return;
        if (cur == module) {
            __atomic_store_n(&mods[i], ~0ull, __ATOMIC_RELEASE);     // a tombstone
            return;
        }
    }
}

// The structures this touches (64-bit layouts).
typedef struct { int32_t sType; const void *pNext; } Base;
typedef struct { uint32_t constantID, offset; size_t size; } SpecEntry;
typedef struct { uint32_t mapEntryCount; const SpecEntry *pMapEntries; size_t dataSize; const void *pData; } SpecInfo;
typedef struct {
    int32_t sType; const void *pNext; uint32_t flags, stage; uint64_t module; const char *pName;
    const SpecInfo *pSpecializationInfo;
} Stage;
typedef struct {
    int32_t sType; const void *pNext; uint32_t flags, rasterizationSamples, sampleShadingEnable;
    float minSampleShading; const uint32_t *pSampleMask; uint32_t alphaToCoverageEnable, alphaToOneEnable;
} Multisample;
typedef struct { int32_t sType; const void *pNext; uint32_t flags, dynamicStateCount; const int32_t *pDynamicStates; } Dynamic;
typedef struct {
    int32_t sType; const void *pNext; uint32_t flags; uint32_t stageCount;
    const Stage *pStages; const void *pVertexInputState, *pInputAssemblyState, *pTessellationState, *pViewportState;
    const void *pRasterizationState; const Multisample *pMultisampleState;
    const void *pDepthStencilState, *pColorBlendState; const Dynamic *pDynamicState;
    uint64_t layout, renderPass; uint32_t subpass; uint64_t basePipelineHandle; int32_t basePipelineIndex;
} Pipeline;

static struct { unsigned long cut, kept_no_cut, kept_other; } stats;

static void tell(const char *what)
{
    unsigned long total = stats.cut + stats.kept_no_cut + stats.kept_other;
    if (total <= 4 || (total & (total - 1)) == 0)
        dprintf(2, "[shim] a2c 1x: %s (%lu cut at alpha 0.5, %lu kept: shader not changed, %lu kept: other)\n",
                what, stats.cut, stats.kept_no_cut, stats.kept_other);
}

// The pipelines to create instead of cis (the same array if none changes);
// *to_free gets what lxrt_a2c_free releases afterwards.
const void *lxrt_a2c_pipelines(uint32_t count, const void *cis_in, void **to_free)
{
    const Pipeline *cis = cis_in;
    *to_free = 0;
    if (!cis || !lxrt_a2c_wanted())
        return cis_in;
    Pipeline *copy = 0;
    void **blocks = 0;
    uint32_t nblocks = 0;
    for (uint32_t i = 0; i < count; i++) {
        const Multisample *ms = cis[i].pMultisampleState;
        if (!ms || !ms->alphaToCoverageEnable || ms->rasterizationSamples != 1)
            continue;
        int skip = (cis[i].flags & 0x800) != 0;        // a library: its parts meet later
        for (const Base *p = cis[i].pNext; p && !skip; p = p->pNext)
            skip = p->sType == 1000290000;              // VkPipelineLibraryCreateInfoKHR
        for (uint32_t d = 0; !skip && cis[i].pDynamicState && d < cis[i].pDynamicState->dynamicStateCount; d++) {
            int32_t s = cis[i].pDynamicState->pDynamicStates[d];
            skip = s == 1000455005 || s == 1000455007;   // rasterization samples, alpha-to-coverage
        }
        int fs = -1;
        for (uint32_t s = 0; cis[i].pStages && s < cis[i].stageCount; s++)
            if (cis[i].pStages[s].stage == 0x10)         // FRAGMENT
                fs = (int)s;
        if (skip || fs < 0) {
            stats.kept_other++;
            tell(skip ? "a pipeline library or dynamic state" : "no fragment shader");
            continue;
        }
        const Stage *st = &cis[i].pStages[fs];
        const SpecInfo *old = st->pSpecializationInfo;
        int clash = 0;
        for (uint32_t e = 0; old && e < old->mapEntryCount; e++)
            clash |= old->pMapEntries[e].constantID == SPEC_ID;
        if (!module_has(st->module) || clash) {
            stats.kept_no_cut++;
            tell(st->module ? "a fragment shader without the cut" : "fragment shader code given inline");
            continue;
        }
        uint32_t ne = old ? old->mapEntryCount : 0;
        size_t od = old ? old->dataSize : 0, off = (od + 3) & ~(size_t)3;
        size_t bytes = sizeof(Multisample) + sizeof(Stage) * cis[i].stageCount + sizeof(SpecInfo) +
                       sizeof(SpecEntry) * (ne + 1) + off + 4;
        if (!copy) {
            copy = malloc(sizeof(Pipeline) * count);
            blocks = calloc(count + 1, sizeof(void *));
            if (!copy || !blocks) {
                free(copy);
                free(blocks);
                return cis_in;
            }
            memcpy(copy, cis, sizeof(Pipeline) * count);
        }
        char *b = malloc(bytes);
        if (!b)
            continue;
        blocks[nblocks++] = b;
        Multisample *nms = (Multisample *)b;
        Stage *nst = (Stage *)(nms + 1);
        SpecInfo *nsi = (SpecInfo *)(nst + cis[i].stageCount);
        SpecEntry *nse = (SpecEntry *)(nsi + 1);
        unsigned char *data = (unsigned char *)(nse + ne + 1);
        *nms = *ms;
        nms->alphaToCoverageEnable = 0;
        memcpy(nst, cis[i].pStages, sizeof(Stage) * cis[i].stageCount);
        if (ne) memcpy(nse, old->pMapEntries, sizeof(SpecEntry) * ne);
        nse[ne] = (SpecEntry){ SPEC_ID, (uint32_t)off, 4 };
        memset(data, 0, off + 4);
        if (od) memcpy(data, old->pData, od);
        uint32_t on = 1;
        memcpy(data + off, &on, 4);
        *nsi = (SpecInfo){ ne + 1, nse, off + 4, data };
        nst[fs].pSpecializationInfo = nsi;
        copy[i].pStages = nst;
        copy[i].pMultisampleState = nms;
        stats.cut++;
        tell("foliage and other cut-outs drawn clean");
    }
    if (!copy)
        return cis_in;
    blocks[nblocks] = copy;
    *to_free = blocks;
    return copy;
}

void lxrt_a2c_free(void *to_free)
{
    void **blocks = to_free;
    if (!blocks) return;
    for (uint32_t i = 0; blocks[i]; i++)
        free(blocks[i]);
    free(blocks);
}
