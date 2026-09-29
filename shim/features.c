// Device features MoltenVK lacks but DXVK refuses to run without.
//
// DXVK skips any adapter that does not report every feature Direct3D 11
// feature level 11_0 implies (MEASURED: "Skipping: Device does not support
// required feature 'geometryShader'" -- then "No adapters found"). Metal has
// no geometry shaders and no cull distances, so MoltenVK says false, and not
// one D3D11 program could start -- including the great majority that never
// use either.
//
// 1. Core features (geometryShader, shaderCullDistance): reported as
//    supported, and removed again from vkCreateDevice's request, so MoltenVK
//    is never asked for what it cannot do. A program that really uses a
//    geometry shader fails at pipeline creation instead of at start-up
//    (HYPOTHESIS: most D3D11 games do not; the real fix is geometry-shader
//    emulation in MoltenVK).
//
// 2. VK_EXT_depth_clip_enable (DXVK: "required feature 'depthClipEnable'").
//    Metal has the same switch (depth clip mode clip/clamp), which MoltenVK
//    exposes only as depthClampEnable. The extension is advertised, stripped
//    from vkCreateDevice, and each pipeline's
//    VkPipelineRasterizationDepthClipStateCreateInfoEXT becomes
//    depthClampEnable = !depthClipEnable -- what D3D means by "depth clip
//    off", and exactly what Metal's clamp mode does.
//
// The caller's structures are edited in place for the duration of the call
// and restored afterwards. LXRT_VK_NO_SPOOF=1 turns all of this off.
#include <stdint.h>
#include <stddef.h>

// glibc's, resolved at load time (the shim is linked -nostdlib).
extern char *getenv(const char *);

typedef int32_t VkResult;
typedef uint32_t VkBool32;
typedef void *VkPhysicalDevice;
typedef void *VkDevice;
typedef uint64_t VkPipelineCache;
typedef uint64_t VkPipeline;

#define VK_SUCCESS 0
#define VK_INCOMPLETE 5

#define N_FEATURES 55
enum { F_GEOMETRY_SHADER = 4, F_DEPTH_CLAMP = 11, F_SHADER_CULL_DISTANCE = 38 };
static const int k_spoof[] = { F_GEOMETRY_SHADER, F_SHADER_CULL_DISTANCE };
#define N_SPOOF (sizeof k_spoof / sizeof k_spoof[0])

#define STYPE_PHYSICAL_DEVICE_FEATURES_2                  1000059000
#define STYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES  1000102000
#define STYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE     1000102001
#define STYPE_GRAPHICS_PIPELINE_CREATE_INFO               28


typedef struct { VkBool32 f[N_FEATURES]; } VkPhysicalDeviceFeatures;
typedef struct VkBase { int32_t sType; struct VkBase *pNext; } VkBase;
typedef struct { int32_t sType; void *pNext; VkPhysicalDeviceFeatures features; } VkPhysicalDeviceFeatures2;
typedef struct { int32_t sType; void *pNext; VkBool32 depthClipEnable; } VkDepthClipFeatures;
typedef struct { int32_t sType; const void *pNext; uint32_t flags; VkBool32 depthClipEnable; } VkDepthClipState;
typedef struct { char extensionName[256]; uint32_t specVersion; } VkExtensionProperties;
typedef struct {
    int32_t sType; const void *pNext; uint32_t flags;
    uint32_t queueCreateInfoCount; const void *pQueueCreateInfos;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
    const VkPhysicalDeviceFeatures *pEnabledFeatures;
} VkDeviceCreateInfo;
typedef struct {
    int32_t sType; const void *pNext; uint32_t flags;
    VkBool32 depthClampEnable;
    // ... the rest is not touched
} VkRasterizationState;
typedef struct {
    int32_t sType; const void *pNext; uint32_t flags; uint32_t stageCount;
    const void *pStages, *pVertexInputState, *pInputAssemblyState, *pTessellationState, *pViewportState;
    const VkRasterizationState *pRasterizationState;
    const void *pMultisampleState, *pDepthStencilState, *pColorBlendState, *pDynamicState;
    uint64_t layout, renderPass; uint32_t subpass; uint64_t basePipelineHandle; int32_t basePipelineIndex;
} VkGraphicsPipelineCreateInfo;

// MoltenVK's own versions (vulkan_shim.S, hidden).
void lxrt_mvk_vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures *);
void lxrt_mvk_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
void lxrt_mvk_vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice, VkPhysicalDeviceFeatures2 *);
VkResult lxrt_mvk_vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const void *, VkDevice *);
VkResult lxrt_mvk_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *, uint32_t *, VkExtensionProperties *);
VkResult lxrt_mvk_vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *,
                                            const void *, VkPipeline *);
void *lxrt_mvk_vkGetDeviceProcAddr(VkDevice, const char *);

static int s_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int spoof_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_VK_NO_SPOOF");
        on = !(e && *e == '1');
    }
    return on;
}

static void spoof(VkPhysicalDeviceFeatures *f)
{
    for (unsigned i = 0; i < N_SPOOF; i++)
        f->f[k_spoof[i]] = 1;
}

// VK_EXT_robustness2 (DXVK: "required feature 'robustBufferAccess2'"):
// MoltenVK has the extension with robustImageAccess2 only. Buffer accesses
// out of bounds and null descriptors are reported as supported and asked of
// MoltenVK only if it really has them (HYPOTHESIS: games rarely depend on
// either; Metal returns zero for most out-of-range reads anyway).
typedef struct { int32_t sType; void *pNext; VkBool32 robustBufferAccess2, robustImageAccess2, nullDescriptor; } VkRobustness2Features;
#define STYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES 1000286000

// Extensions MoltenVK lacks that the shim claims, with the feature structure
// whose VkBool32 fields (after sType/pNext) it sets:
//   VK_EXT_depth_clip_enable  DXVK requires it; emulated with depth clamp
//                             (vkCreateGraphicsPipelines below).
//   VK_EXT_transform_feedback VKD3D-Proton refuses to create a device without
//                             it ("Lacking support for transform feedback").
//                             Metal has no stream output: the commands are
//                             no-ops and an indexed query with index 0 is the
//                             plain query. Games that really stream out draw
//                             wrong (HYPOTHESIS: few D3D12 games do).
//   VK_EXT_dynamic_rendering_unused_attachments  only relaxes validity rules
//                             VKD3D-Proton relies on ("required for correct
//                             operation"); Metal ignores unused attachments.
static const struct emu_ext {
    const char *name; int32_t feat_stype; unsigned nbools;
} k_emu[] = {
    { "VK_EXT_depth_clip_enable", STYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES, 1 },
    { "VK_EXT_transform_feedback", 1000028000, 2 },          // transformFeedback, geometryStreams
    { "VK_EXT_dynamic_rendering_unused_attachments", 1000499000, 1 },
};
#define N_EMU (sizeof k_emu / sizeof k_emu[0])
#define STYPE_TRANSFORM_FEEDBACK_PROPERTIES 1000028001

// Which of k_emu MoltenVK itself lacks, per physical device (bit i = emulate
// k_emu[i]). The real list has 130+ entries; computed once per device.
static struct { VkPhysicalDevice pd; unsigned mask; } g_emu_cache[8];
static VkExtensionProperties g_ext_buf[512];
static volatile int g_ext_lock;

static unsigned emu_mask(VkPhysicalDevice pd)
{
    for (unsigned i = 0; i < 8; i++)
        if (g_emu_cache[i].pd == pd)
            return g_emu_cache[i].mask;
    while (__atomic_exchange_n(&g_ext_lock, 1, __ATOMIC_ACQUIRE))
        ;
    unsigned mask = (1u << N_EMU) - 1;
    uint32_t n = 512;
    if (lxrt_mvk_vkEnumerateDeviceExtensionProperties(pd, 0, &n, g_ext_buf) >= 0)
        for (uint32_t i = 0; i < n; i++)
            for (unsigned k = 0; k < N_EMU; k++)
                if (s_eq(g_ext_buf[i].extensionName, k_emu[k].name))
                    mask &= ~(1u << k);
    for (unsigned i = 0; i < 8; i++)
        if (!g_emu_cache[i].pd) {
            g_emu_cache[i].mask = mask;
            __atomic_store_n(&g_emu_cache[i].pd, pd, __ATOMIC_RELEASE);
            break;
        }
    __atomic_store_n(&g_ext_lock, 0, __ATOMIC_RELEASE);
    return mask;
}

static void spoof_chain(VkBase *p)
{
    for (; p; p = p->pNext) {
        for (unsigned k = 0; k < N_EMU; k++)
            if (p->sType == k_emu[k].feat_stype)
                for (unsigned b = 0; b < k_emu[k].nbools; b++)
                    ((VkBool32 *)((char *)p + 16))[b] = 1;
        if (p->sType == STYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES) {
            ((VkRobustness2Features *)p)->robustBufferAccess2 = 1;
            ((VkRobustness2Features *)p)->nullDescriptor = 1;
        }
    }
}

void lxrt_inner_vkGetPhysicalDeviceFeatures(VkPhysicalDevice pd, VkPhysicalDeviceFeatures *f)
{
    lxrt_mvk_vkGetPhysicalDeviceFeatures(pd, f);
    if (spoof_on()) spoof(f);
}

void lxrt_inner_vkGetPhysicalDeviceFeatures2(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    lxrt_mvk_vkGetPhysicalDeviceFeatures2(pd, f);
    if (spoof_on()) { spoof(&f->features); spoof_chain(f->pNext); }
}

void lxrt_inner_vkGetPhysicalDeviceFeatures2KHR(VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *f)
{
    lxrt_mvk_vkGetPhysicalDeviceFeatures2KHR(pd, f);
    if (spoof_on()) { spoof(&f->features); spoof_chain(f->pNext); }
}

// Transform-feedback limits for the emulated extension (D3D12's stream
// output: 4 streams, 4 buffers, 2048-byte strides).
typedef struct {
    int32_t sType; void *pNext;
    uint32_t maxTransformFeedbackStreams, maxTransformFeedbackBuffers;
    uint64_t maxTransformFeedbackBufferSize;
    uint32_t maxTransformFeedbackStreamDataSize, maxTransformFeedbackBufferDataSize, maxTransformFeedbackBufferDataStride;
    VkBool32 transformFeedbackQueries, transformFeedbackStreamsLinesTriangles,
             transformFeedbackRasterizationStreamSelect, transformFeedbackDraw;
} VkTransformFeedbackProperties;
typedef struct { int32_t sType; void *pNext; } VkPhysicalDeviceProperties2Head;   // + properties, not touched

void lxrt_mvk_vkGetPhysicalDeviceProperties2(VkPhysicalDevice, void *);
void lxrt_mvk_vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice, void *);

extern int dprintf(int, const char *, ...);
static void spoof_props(VkPhysicalDevice pd, VkBase *p)
{
    if (getenv("LXRT_VK_DEBUG")) {
        dprintf(2, "[shim] props2 mask 0x%x chain:", emu_mask(pd));
        for (VkBase *q = p; q; q = q->pNext) dprintf(2, " %d", q->sType);
        dprintf(2, "\n");
    }
    if (!spoof_on())
        return;
    // Single-texel alignment for texel buffer views (VKD3D-Proton: "Lacking
    // support for single texel alignment"). Metal's linear textures want 16
    // bytes; MoltenVK reports that honestly. HYPOTHESIS: most typed-buffer
    // views D3D12 games create are 16-byte aligned anyway; a misaligned one
    // may read wrong data.
    for (VkBase *q = p; q; q = q->pNext) {
        if (q->sType == 54) {                                   // VULKAN_1_3_PROPERTIES
            *(VkBool32 *)((char *)q + 184) = 1;                 // storageTexelBufferOffsetSingleTexelAlignment
            *(VkBool32 *)((char *)q + 200) = 1;                 // uniformTexelBufferOffsetSingleTexelAlignment
        } else if (q->sType == 1000281001) {                    // TEXEL_BUFFER_ALIGNMENT_PROPERTIES
            *(VkBool32 *)((char *)q + 24) = 1;
            *(VkBool32 *)((char *)q + 40) = 1;
        }
    }
    if (!(emu_mask(pd) & 2))
        return;
    for (; p; p = p->pNext)
        if (p->sType == STYPE_TRANSFORM_FEEDBACK_PROPERTIES) {
            VkTransformFeedbackProperties *t = (VkTransformFeedbackProperties *)p;
            t->maxTransformFeedbackStreams = 4;
            t->maxTransformFeedbackBuffers = 4;
            t->maxTransformFeedbackBufferSize = 1ull << 32;
            t->maxTransformFeedbackStreamDataSize = 512;
            t->maxTransformFeedbackBufferDataSize = 512;
            t->maxTransformFeedbackBufferDataStride = 2048;
            t->transformFeedbackQueries = 1;
            t->transformFeedbackStreamsLinesTriangles = 0;
            t->transformFeedbackRasterizationStreamSelect = 0;
            t->transformFeedbackDraw = 1;
        }
}

void lxrt_inner_vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2Head *p)
{
    lxrt_mvk_vkGetPhysicalDeviceProperties2(pd, p);
    spoof_props(pd, (VkBase *)p->pNext);
}

void lxrt_inner_vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceProperties2Head *p)
{
    lxrt_mvk_vkGetPhysicalDeviceProperties2KHR(pd, p);
    spoof_props(pd, (VkBase *)p->pNext);
}

// Transform-feedback commands: nothing to do without stream output, except
// that an indexed query on stream 0 is an ordinary query.
typedef void *VkCommandBuffer;
typedef uint64_t VkQueryPool, VkBuffer;
void lxrt_mvk_vkCmdBeginQuery(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t);
void lxrt_mvk_vkCmdEndQuery(VkCommandBuffer, VkQueryPool, uint32_t);
void lxrt_inner_vkCmdBindTransformFeedbackBuffersEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b,
                                                     const uint64_t *off, const uint64_t *sz)
{ (void)cb; (void)first; (void)n; (void)b; (void)off; (void)sz; }
void lxrt_inner_vkCmdBeginTransformFeedbackEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b,
                                               const uint64_t *off)
{ (void)cb; (void)first; (void)n; (void)b; (void)off; }
void lxrt_inner_vkCmdEndTransformFeedbackEXT(VkCommandBuffer cb, uint32_t first, uint32_t n, const VkBuffer *b,
                                             const uint64_t *off)
{ (void)cb; (void)first; (void)n; (void)b; (void)off; }
void lxrt_inner_vkCmdBeginQueryIndexedEXT(VkCommandBuffer cb, VkQueryPool pool, uint32_t q, uint32_t flags, uint32_t index)
{
    if (index == 0)
        lxrt_mvk_vkCmdBeginQuery(cb, pool, q, flags);
}
void lxrt_inner_vkCmdEndQueryIndexedEXT(VkCommandBuffer cb, VkQueryPool pool, uint32_t q, uint32_t index)
{
    if (index == 0)
        lxrt_mvk_vkCmdEndQuery(cb, pool, q);
}
void lxrt_inner_vkCmdDrawIndirectByteCountEXT(VkCommandBuffer cb, uint32_t inst, uint32_t first, VkBuffer b,
                                              uint64_t off, uint32_t coff, uint32_t stride)
{ (void)cb; (void)inst; (void)first; (void)b; (void)off; (void)coff; (void)stride; }

VkResult lxrt_inner_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *count,
                                                         VkExtensionProperties *props)
{
    unsigned mask = spoof_on() && !layer ? emu_mask(pd) : 0;
    if (!mask)
        return lxrt_mvk_vkEnumerateDeviceExtensionProperties(pd, layer, count, props);
    const struct emu_ext *add[N_EMU];
    unsigned nadd = 0;
    for (unsigned k = 0; k < N_EMU; k++)
        if (mask & (1u << k))
            add[nadd++] = &k_emu[k];
    uint32_t n = 0;
    VkResult r = lxrt_mvk_vkEnumerateDeviceExtensionProperties(pd, 0, &n, 0);
    if (r < 0)
        return r;
    uint32_t total = n + nadd;
    if (!props) {
        *count = total;
        return VK_SUCCESS;
    }
    uint32_t want = *count, got = want < n ? want : n;
    r = lxrt_mvk_vkEnumerateDeviceExtensionProperties(pd, 0, &got, props);
    if (r < 0)
        return r;
    uint32_t k = got;
    for (unsigned a = 0; a < nadd && k < want; a++, k++) {
        const char *s = add[a]->name;
        unsigned i = 0;
        for (; s[i] && i < 255; i++)
            props[k].extensionName[i] = s[i];
        props[k].extensionName[i] = 0;
        props[k].specVersion = 1;
    }
    *count = k;
    return k < total ? VK_INCOMPLETE : VK_SUCCESS;
}

// Unlink every structure of type `st` from a pNext chain; `undo` records
// (link, removed) pairs so the chain can be put back.
struct unlink { VkBase **link; VkBase *node; };
static int chain_unlink(VkBase **head, int32_t st, struct unlink *undo, int max)
{
    int n = 0;
    for (VkBase **l = head; *l && n < max; ) {
        if ((*l)->sType == st) {
            undo[n].link = l;
            undo[n].node = *l;
            n++;
            *l = (*l)->pNext;
        } else {
            l = &(*l)->pNext;
        }
    }
    return n;
}
static void chain_relink(struct unlink *undo, int n)
{
    while (n-- > 0)
        *undo[n].link = undo[n].node;
}

// Clear, in the caller's own structures, every spoofed feature the device
// does not really have, drop the emulated extensions and their feature
// structures, call MoltenVK, and put everything back.
VkResult lxrt_inner_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *cci, const void *alloc, VkDevice *out)
{
    if (!spoof_on() || !cci)
        return lxrt_mvk_vkCreateDevice(pd, cci, alloc, out);
    VkDeviceCreateInfo *ci = (VkDeviceCreateInfo *)cci;
    VkPhysicalDeviceFeatures real;
    lxrt_mvk_vkGetPhysicalDeviceFeatures(pd, &real);

    VkPhysicalDeviceFeatures *sets[2] = { 0, 0 };
    int nsets = 0;
    if (ci->pEnabledFeatures)
        sets[nsets++] = (VkPhysicalDeviceFeatures *)ci->pEnabledFeatures;
    for (const VkBase *p = ci->pNext; p && nsets < 2; p = p->pNext)
        if (p->sType == STYPE_PHYSICAL_DEVICE_FEATURES_2)
            sets[nsets++] = &((VkPhysicalDeviceFeatures2 *)p)->features;

    VkBool32 saved[2][N_SPOOF];
    for (int s = 0; s < nsets; s++)
        for (unsigned i = 0; i < N_SPOOF; i++) {
            saved[s][i] = sets[s]->f[k_spoof[i]];
            if (!real.f[k_spoof[i]])
                sets[s]->f[k_spoof[i]] = 0;
        }

    unsigned mask = emu_mask(pd);
    struct unlink undo[16];
    int nundo = 0;
    const char *const *old_names = ci->ppEnabledExtensionNames;
    uint32_t old_count = ci->enabledExtensionCount;
    const char *names[512];
    if (mask) {
        for (unsigned k = 0; k < N_EMU; k++)
            if (mask & (1u << k))
                nundo += chain_unlink((VkBase **)&ci->pNext, k_emu[k].feat_stype, undo + nundo, 16 - nundo);
        uint32_t kept = 0;
        for (uint32_t i = 0; i < old_count && kept < 512; i++) {
            int drop = 0;
            for (unsigned k = 0; k < N_EMU; k++)
                if ((mask & (1u << k)) && s_eq(old_names[i], k_emu[k].name))
                    drop = 1;
            if (!drop)
                names[kept++] = old_names[i];
        }
        ci->ppEnabledExtensionNames = names;
        ci->enabledExtensionCount = kept;
    }

    // Robustness2: only what MoltenVK really has.
    VkRobustness2Features *rob = 0, rob_saved = {0}, rob_real = { STYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES, 0, 0, 0, 0 };
    for (VkBase *p = (VkBase *)ci->pNext; p; p = p->pNext)
        if (p->sType == STYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES)
            rob = (VkRobustness2Features *)p;
    if (rob) {
        VkPhysicalDeviceFeatures2 f2 = { STYPE_PHYSICAL_DEVICE_FEATURES_2, &rob_real, {{0}} };
        lxrt_mvk_vkGetPhysicalDeviceFeatures2(pd, &f2);
        rob_saved = *rob;
        rob->robustBufferAccess2 &= rob_real.robustBufferAccess2;
        rob->robustImageAccess2 &= rob_real.robustImageAccess2;
        rob->nullDescriptor &= rob_real.nullDescriptor;
    }

    VkResult r = lxrt_mvk_vkCreateDevice(pd, ci, alloc, out);

    if (rob) {
        rob->robustBufferAccess2 = rob_saved.robustBufferAccess2;
        rob->robustImageAccess2 = rob_saved.robustImageAccess2;
        rob->nullDescriptor = rob_saved.nullDescriptor;
    }
    if (mask) {
        ci->ppEnabledExtensionNames = old_names;
        ci->enabledExtensionCount = old_count;
        chain_relink(undo, nundo);
    }
    for (int s = 0; s < nsets; s++)
        for (unsigned i = 0; i < N_SPOOF; i++)
            sets[s]->f[k_spoof[i]] = saved[s][i];
    return r;
}

// Depth clip state -> depth clamp, per pipeline, then restore.
VkResult lxrt_inner_vkCreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                   const VkGraphicsPipelineCreateInfo *cis, const void *alloc, VkPipeline *out)
{
    enum { MAXP = 64 };
    if (!spoof_on() || n > MAXP)
        return lxrt_mvk_vkCreateGraphicsPipelines(dev, cache, n, cis, alloc, out);
    struct { VkRasterizationState *rs; VkBool32 clamp; struct unlink undo[2]; int nundo; } fix[MAXP];
    for (uint32_t i = 0; i < n; i++) {
        fix[i].rs = (VkRasterizationState *)cis[i].pRasterizationState;
        fix[i].nundo = 0;
        if (!fix[i].rs)
            continue;
        const VkDepthClipState *clip = 0;
        for (const VkBase *p = fix[i].rs->pNext; p; p = p->pNext)
            if (p->sType == STYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE)
                clip = (const VkDepthClipState *)p;
        fix[i].clamp = fix[i].rs->depthClampEnable;
        if (clip) {
            if (!clip->depthClipEnable)
                fix[i].rs->depthClampEnable = 1;
            fix[i].nundo = chain_unlink((VkBase **)&fix[i].rs->pNext,
                                        STYPE_PIPELINE_RASTERIZATION_DEPTH_CLIP_STATE, fix[i].undo, 2);
        }
    }
    VkResult r = lxrt_mvk_vkCreateGraphicsPipelines(dev, cache, n, cis, alloc, out);
    for (uint32_t i = 0; i < n; i++) {
        if (!fix[i].rs)
            continue;
        chain_relink(fix[i].undo, fix[i].nundo);
        fix[i].rs->depthClampEnable = fix[i].clamp;
    }
    return r;
}

void *lxrt_rebase_proc(const char *name);   // vk_rebase.c (generated)

void *lxrt_tramp32(void *fn);                // map32.c: 32-bit guests only
int lxrt_vk_missing(const char *name);       // vulkan_shim.c (generated): ICD mode only

void *lxrt_inner_vkGetDeviceProcAddr(VkDevice dev, const char *name)
{
    void *f = lxrt_rebase_proc(name);
    if (f && lxrt_vk_missing(name))
        return 0;
    return lxrt_tramp32(f ? f : lxrt_mvk_vkGetDeviceProcAddr(dev, name));
}
