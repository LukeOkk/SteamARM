// Shadow compares on their own variable, for MoltenVK's SPIRV-Cross.
//
// Counter-Strike 2's material shaders sample colours and compare shadow
// depths through one bindless Texture2D array (binding 46, 65536 images).
// SPIRV-Cross marks a variable that any Dref op uses as a depth image and
// declares the whole array depth2d; every colour sample from it is then cut
// to one component and copied to all four: the red channel only. In the
// game a yellow prop came out white and a red sign grey, on 1144 of its 2404
// fragment shaders (found 2026-10-04 by translating CS2's own SPIR-V with
// SPIRV-Cross at MoltenVK 1.4.2's revision; KosmicKrisp is not affected).
//
// This pass gives each such variable a twin with the same type, descriptor
// set and binding, and routes only the Dref ops through it (a new access
// chain, load and OpSampledImage just before each Dref op). SPIRV-Cross then
// types only the twin depth2d, as an overlapping alias of the same argument
// buffer slot (MSL 3 argument buffers), and the colour samples read RGBA
// again. MoltenVK only; LXRT_VK_SPLIT_DREF=0 turns it off.
#include <stdint.h>
#include <stddef.h>
extern void *memcpy(void *, const void *, size_t);
extern void *memset(void *, int, size_t);

extern void *malloc(size_t);
extern void *realloc(void *, size_t);
extern void free(void *);
extern char *getenv(const char *);
extern int dprintf(int, const char *, ...);

enum {
    OP_ENTRY_POINT = 15, OP_VARIABLE = 59, OP_LOAD = 61, OP_ACCESS_CHAIN = 65, OP_IN_BOUNDS_ACCESS_CHAIN = 66,
    OP_DECORATE = 71, OP_COPY_OBJECT = 83, OP_SAMPLED_IMAGE = 86, OP_IMAGE = 100, DEC_NON_UNIFORM = 5300,
};

static int is_dref(uint32_t op)
{
    switch (op) {
    case 89: case 90: case 93: case 94: case 97:                // Sample(Proj)Dref{Implicit,Explicit}Lod, DrefGather
    case 307: case 308: case 311: case 312: case 315:           // their sparse forms
        return 1;
    }
    return 0;
}

// Ops whose third word is a sampled image or an image (colour use).
static int is_plain(uint32_t op)
{
    switch (op) {
    case 87: case 88: case 91: case 92: case 95: case 96: case 98:
    case 103: case 104: case 105: case 106: case 107:
    case 305: case 306: case 309: case 310: case 313: case 314:
        return 1;
    }
    return 0;
}

typedef struct { uint32_t *w; size_t n, cap; } vec;

static int vpush(vec *v, const uint32_t *w, size_t n)
{
    if (v->n + n > v->cap) {
        size_t cap = v->cap ? v->cap * 2 : 1024;
        while (cap < v->n + n)
            cap *= 2;
        uint32_t *p = realloc(v->w, cap * 4);
        if (!p)
            return 0;
        v->w = p;
        v->cap = cap;
    }
    memcpy(v->w + v->n, w, n * 4);
    v->n += n;
    return 1;
}

typedef struct { uint32_t at; int before; uint32_t off, len; } ins;   // words of v_ins[off..off+len)

int lxrt_spirv_split_dref_wanted(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_VK_SPLIT_DREF");
        on = !(e && *e == '0');
    }
    return on;
}

// A copy with the Dref ops routed through twins, or NULL when nothing changes.
uint32_t *lxrt_spirv_split_dref(const uint32_t *code, size_t size, size_t *out_size)
{
    if (!code || size < 20 || (size & 3) || code[0] != 0x07230203u)
        return NULL;
    size_t nw = size / 4;
    uint32_t bound = code[3], obound = code[3];
    if (bound == 0 || bound > (1u << 22))
        return NULL;

    // Instruction starts.
    size_t ninst = 0;
    for (size_t i = 5; i < nw;) {
        uint32_t wc = code[i] >> 16;
        if (!wc || i + wc > nw)
            return NULL;
        ninst++;
        i += wc;
    }
    uint32_t *start = malloc(ninst * sizeof *start);
    int32_t *def = malloc((size_t)bound * sizeof *def);
    uint8_t *kind = malloc((size_t)bound);   // per variable: 1 Dref use, 2 colour use
    if (!start || !def || !kind) {
        free(start); free(def); free(kind);
        return NULL;
    }
    memset(def, 0xff, (size_t)bound * sizeof *def);
    memset(kind, 0, (size_t)bound);
    size_t k = 0;
    for (size_t i = 5; i < nw; i += code[i] >> 16) {
        start[k] = (uint32_t)i;
        uint32_t op = code[i] & 0xffff, wc = code[i] >> 16;
        if ((op == OP_VARIABLE || op == OP_LOAD || op == OP_ACCESS_CHAIN || op == OP_IN_BOUNDS_ACCESS_CHAIN ||
             op == OP_COPY_OBJECT || op == OP_SAMPLED_IMAGE || op == OP_IMAGE) && wc > 2 && code[i + 2] < bound)
            def[code[i + 2]] = (int32_t)k;
        k++;
    }

    // The variable an image operand comes from, or 0.
#define TRACE(id_, out_)                                                                                  \
    do {                                                                                                  \
        uint32_t t_ = (id_);                                                                              \
        (out_) = 0;                                                                                       \
        for (int d_ = 0; d_ < 16 && t_ < bound && def[t_] >= 0; d_++) {                                   \
            const uint32_t *x_ = code + start[def[t_]];                                                   \
            uint32_t o_ = x_[0] & 0xffff;                                                                 \
            if (o_ == OP_VARIABLE) { (out_) = t_; break; }                                                \
            if ((x_[0] >> 16) < 4) break;                                                                 \
            t_ = x_[3];                                                                                   \
        }                                                                                                 \
    } while (0)

    int any = 0;
    for (k = 0; k < ninst; k++) {
        const uint32_t *x = code + start[k];
        uint32_t op = x[0] & 0xffff;
        if ((x[0] >> 16) < 4 || !(is_dref(op) || is_plain(op)))
            continue;
        uint32_t v;
        TRACE(x[3], v);
        if (v)
            kind[v] |= is_dref(op) ? 1 : 2;
    }
    for (uint32_t i = 0; i < bound; i++)
        if (kind[i] == 3)
            any = 1;
    if (!any) {
        free(start); free(def); free(kind);
        return NULL;
    }

    uint32_t *twin = malloc((size_t)bound * sizeof *twin);
    uint8_t *nonuniform = malloc((size_t)bound);
    vec vins = {0}, out = {0};
    ins *list = NULL;
    size_t nlist = 0, caplist = 0;
    int rerouted = 0;
    if (!twin || !nonuniform)
        goto fail;
    memset(twin, 0, (size_t)bound * sizeof *twin);
    memset(nonuniform, 0, (size_t)bound);
    for (uint32_t i = 0; i < bound; i++)
        if (kind[i] == 3)
            twin[i] = bound++;

#define ADD(at_, before_, words_, n_)                                                                     \
    do {                                                                                                  \
        if (nlist == caplist) {                                                                           \
            caplist = caplist ? caplist * 2 : 64;                                                         \
            ins *p_ = realloc(list, caplist * sizeof *list);                                              \
            if (!p_) goto fail;                                                                           \
            list = p_;                                                                                    \
        }                                                                                                 \
        list[nlist].at = (uint32_t)(at_);                                                                 \
        list[nlist].before = (before_);                                                                   \
        list[nlist].off = (uint32_t)vins.n;                                                               \
        list[nlist].len = (uint32_t)(n_);                                                                 \
        if (!vpush(&vins, (words_), (n_))) goto fail;                                                     \
        nlist++;                                                                                          \
    } while (0)

    // Twin variables, their decorations; NonUniform targets.
    for (k = 0; k < ninst; k++) {
        const uint32_t *x = code + start[k];
        uint32_t op = x[0] & 0xffff, wc = x[0] >> 16;
        if (op == OP_VARIABLE && wc >= 4 && x[2] < obound && twin[x[2]]) {
            uint32_t w[16];
            if (wc > 16)
                goto fail;
            memcpy(w, x, wc * 4);
            w[2] = twin[x[2]];
            ADD(k, 0, w, wc);
        } else if (op == OP_DECORATE && wc >= 3 && x[1] < obound) {
            if (twin[x[1]]) {
                uint32_t w[16];
                if (wc > 16)
                    goto fail;
                memcpy(w, x, wc * 4);
                w[1] = twin[x[1]];
                ADD(k, 0, w, wc);
            }
            if (x[2] == DEC_NON_UNIFORM)
                nonuniform[x[1]] = 1;
        }
    }

    // Each Dref op: chain, load and sampled image of its own, just before it.
    uint32_t *newsi = malloc(ninst * sizeof *newsi);   // per instruction: replacement operand, 0 = none
    if (!newsi)
        goto fail;
    memset(newsi, 0, ninst * sizeof *newsi);
    uint32_t last_nonuniform_decor = 0;
    for (k = 0; k < ninst; k++) {
        const uint32_t *x = code + start[k];
        if ((x[0] & 0xffff) == OP_DECORATE && (x[0] >> 16) >= 3 && x[2] == DEC_NON_UNIFORM)
            last_nonuniform_decor = (uint32_t)k;
    }
    for (k = 0; k < ninst; k++) {
        const uint32_t *x = code + start[k];
        uint32_t op = x[0] & 0xffff;
        if ((x[0] >> 16) < 5 || !is_dref(op))
            continue;
        uint32_t si = x[3];
        if (si >= bound || def[si] < 0)
            continue;
        const uint32_t *s = code + start[def[si]];
        if ((s[0] & 0xffff) != OP_SAMPLED_IMAGE || (s[0] >> 16) != 5 || s[3] >= bound || def[s[3]] < 0)
            continue;
        const uint32_t *l = code + start[def[s[3]]];
        if ((l[0] & 0xffff) != OP_LOAD || (l[0] >> 16) < 4 || l[3] >= bound || def[l[3]] < 0)
            continue;
        uint32_t ptr = l[3], var = 0, chain_len = 0;
        const uint32_t *c = code + start[def[ptr]];
        uint32_t cop = c[0] & 0xffff;
        if (cop == OP_VARIABLE)
            var = ptr;
        else if ((cop == OP_ACCESS_CHAIN || cop == OP_IN_BOUNDS_ACCESS_CHAIN) && (c[0] >> 16) >= 4 && c[3] < bound &&
                 def[c[3]] >= 0 && (code[start[def[c[3]]]] & 0xffff) == OP_VARIABLE) {
            var = c[3];
            chain_len = c[0] >> 16;
        }
        if (!var || var >= obound || !twin[var] || chain_len > 32)
            continue;
        uint32_t w[40], new_ptr = twin[var];
        uint32_t ids[3] = {0};   // new chain, load, sampled image
        if (chain_len) {
            memcpy(w, c, chain_len * 4);
            w[2] = ids[0] = bound++;
            w[3] = twin[var];
            new_ptr = ids[0];
            ADD(k, 1, w, chain_len);
        }
        uint32_t ld[4] = {(4u << 16) | OP_LOAD, l[1], ids[1] = bound++, new_ptr};
        ADD(k, 1, ld, 4);
        uint32_t sm[5] = {(5u << 16) | OP_SAMPLED_IMAGE, s[1], ids[2] = bound++, ids[1], s[4]};
        ADD(k, 1, sm, 5);
        newsi[k] = ids[2];
        // NonUniform: the copies are decorated as their originals were.
        uint32_t orig[3] = {chain_len ? ptr : 0, s[3], si};
        for (int j = 0; j < 3; j++)
            if (ids[j] && orig[j] && nonuniform[orig[j]]) {
                uint32_t dw[3] = {(3u << 16) | OP_DECORATE, ids[j], DEC_NON_UNIFORM};
                ADD(last_nonuniform_decor, 0, dw, 3);
            }
        rerouted++;
    }
    if (!rerouted) {
        free(newsi);
        goto fail;
    }

    // Emit: header with the new bound, instructions with insertions; the
    // twins join every entry point's interface (SPIR-V 1.4+ lists all globals).
    uint32_t hdr[5] = {code[0], code[1], code[2], bound, code[4]};
    if (!vpush(&out, hdr, 5))
        goto fail2;
    // insertion sort of the small list by position (stable)
    for (size_t i = 1; i < nlist; i++) {
        ins t = list[i];
        size_t j = i;
        while (j > 0 && list[j - 1].at > t.at) {
            list[j] = list[j - 1];
            j--;
        }
        list[j] = t;
    }
    size_t li = 0;
    for (k = 0; k < ninst; k++) {
        const uint32_t *x = code + start[k];
        uint32_t op = x[0] & 0xffff, wc = x[0] >> 16;
        for (size_t j = li; j < nlist && list[j].at == k; j++)
            if (list[j].before && !vpush(&out, vins.w + list[j].off, list[j].len))
                goto fail2;
        if (op == OP_ENTRY_POINT) {
            size_t at = out.n;
            if (!vpush(&out, x, wc))
                goto fail2;
            // interface ids follow the name (a nul-terminated string from word 3)
            uint32_t p = 3;
            while (p < wc) {
                uint32_t word = x[p++];
                if (!(word & 0xff000000u) || !(word & 0x00ff0000u) || !(word & 0x0000ff00u) || !(word & 0xffu))
                    break;
            }
            uint32_t added = 0;
            for (uint32_t q = p; q < wc; q++)
                if (x[q] < obound && twin[x[q]]) {
                    if (!vpush(&out, &twin[x[q]], 1))
                        goto fail2;
                    added++;
                }
            out.w[at] = ((wc + added) << 16) | OP_ENTRY_POINT;
        } else if (newsi[k]) {
            if (!vpush(&out, x, wc))
                goto fail2;
            out.w[out.n - wc + 3] = newsi[k];
        } else if (!vpush(&out, x, wc))
            goto fail2;
        for (; li < nlist && list[li].at == k; li++)
            if (!list[li].before && !vpush(&out, vins.w + list[li].off, list[li].len))
                goto fail2;
    }
    free(newsi);
    free(start); free(def); free(kind); free(twin); free(nonuniform); free(vins.w); free(list);
    *out_size = out.n * 4;
    {
        static int said;
        const char *d = getenv("LXRT_VK_DEBUG");
        if (d && *d == '1' && said++ < 3)
            dprintf(2, "[shim] spirv: %d shadow compares moved to their own variable\n", rerouted);
    }
    return out.w;
fail2:
    free(newsi);
fail:
    free(start); free(def); free(kind); free(twin); free(nonuniform); free(vins.w); free(out.w); free(list);
    return NULL;
}
