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
//    emulation in the driver -- SteamARM's KosmicKrisp has it since patch
//    27 and reports the feature itself, so nothing is spoofed there).
//    fillModeNonSolid the same way, for KosmicKrisp (STEAMARM_VK_ICD), which
//    reports it false (MoltenVK reports it true, so nothing changes there).
//    DXVK refused the adapter, D3D11 and D3D12 (whose DXGI is DXVK's) alike:
//    MEASURED "Skipping: Device does not support required feature
//    'fillModeNonSolid'"; with it reported, both probes ran (stage22). A
//    wireframe (D3D11_FILL_WIREFRAME) pipeline on such a device is invalid
//    usage: what KosmicKrisp does with it is UNKNOWN (HYPOTHESIS: lines work,
//    Metal has a lines fill mode; points do not).
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
enum { F_GEOMETRY_SHADER = 4, F_DEPTH_CLAMP = 11, F_FILL_MODE_NON_SOLID = 13, F_SAMPLER_ANISOTROPY = 19,
       F_SHADER_CULL_DISTANCE = 38 };
static const int k_spoof[] = { F_GEOMETRY_SHADER, F_FILL_MODE_NON_SOLID, F_SHADER_CULL_DISTANCE };
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

// bufferImageGranularity no finer than 16 KiB, the alignment KosmicKrisp
// gives optimal images (Apple GPU pages). vkd3d-proton reports D3D12's tight
// alignment (tier 1) whenever the granularity is 4 KiB or less: then
// GetResourceAllocationInfo answers a texture with the driver's own 16 KiB,
// but CreatePlacedResource still demands 64 KiB unless the driver has
// VK_MESA_image_alignment_control, which neither KosmicKrisp nor MoltenVK
// has. Unreal Engine 5.6 placed textures where it had been told it could,
// vkd3d-proton refused ("Heap offset 0xfa8000 not a multiple of resource
// alignment 0x10000", E_INVALIDARG) and Minecraft Dungeons II stopped on a
// fatal D3D12 error (benchmarks/stage62, 15). LXRT_VK_GRANULARITY=<bytes>
// picks another floor; 0 reports the driver's value.
#define PROPS2_BUFFER_IMAGE_GRANULARITY 360     // offsetof(VkPhysicalDeviceProperties2, properties.limits.bufferImageGranularity)
static void granularity_floor(VkPhysicalDeviceProperties2Head *p)
{
    static long floor_ = -1;
    if (floor_ < 0) {
        const char *e = getenv("LXRT_VK_GRANULARITY");
        floor_ = 16384;
        if (e && *e) {                          // decimal only: no libc here
            floor_ = 0;
            for (; *e >= '0' && *e <= '9'; e++)
                floor_ = floor_ * 10 + (*e - '0');
        }
    }
    uint64_t *g = (uint64_t *)((char *)p + PROPS2_BUFFER_IMAGE_GRANULARITY);
    if (floor_ > 0 && *g < (uint64_t)floor_)
        *g = (uint64_t)floor_;
}

void lxrt_inner_vkGetPhysicalDeviceProperties2(VkPhysicalDevice pd, VkPhysicalDeviceProperties2Head *p)
{
    lxrt_mvk_vkGetPhysicalDeviceProperties2(pd, p);
    granularity_floor(p);
    spoof_props(pd, (VkBase *)p->pNext);
}

void lxrt_inner_vkGetPhysicalDeviceProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceProperties2Head *p)
{
    lxrt_mvk_vkGetPhysicalDeviceProperties2KHR(pd, p);
    granularity_floor(p);
    spoof_props(pd, (VkBase *)p->pNext);
}

// Image memory alignments as D3D12 places textures: 64 KiB for an image of
// 64 KiB or more, 4 KiB below that. KosmicKrisp asks 16 KiB of an optimal
// image (128 bytes of some), and vkd3d-proton hands that to a D3D12 app that
// uses tight alignment (GetResourceAllocationInfo with
// D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT) while CreatePlacedResource still
// wants 64 KiB -- 4 KiB for a small texture -- unless the driver has
// VK_MESA_image_alignment_control. Unreal Engine 5.6 placed its textures at
// the 16 KiB multiples it was given and vkd3d-proton refused them (E_INVALIDARG,
// a fatal D3D12 error in Minecraft Dungeons II; benchmarks/stage62, 15). A
// stricter alignment is always a valid answer. LXRT_VK_IMAGE_ALIGN=0 turns
// this off.
typedef struct { uint64_t size, alignment; uint32_t memoryTypeBits; } VkMemReq;
typedef struct { int32_t sType; void *pNext; VkMemReq memoryRequirements; } VkMemReq2;
void lxrt_mvk_vkGetImageMemoryRequirements(void *, uint64_t, VkMemReq *);
void lxrt_mvk_vkGetImageMemoryRequirements2(void *, const void *, VkMemReq2 *);
void lxrt_mvk_vkGetImageMemoryRequirements2KHR(void *, const void *, VkMemReq2 *);
void lxrt_mvk_vkGetDeviceImageMemoryRequirements(void *, const void *, VkMemReq2 *);
void lxrt_mvk_vkGetDeviceImageMemoryRequirementsKHR(void *, const void *, VkMemReq2 *);
static void d3d12_alignment(VkMemReq *r)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_VK_IMAGE_ALIGN");
        on = !(e && e[0] == '0' && !e[1]);
    }
    if (!on)
        return;
    uint64_t want = r->size >= 65536 ? 65536 : 4096;
    if (r->alignment < want)
        r->alignment = want;
    r->size = (r->size + r->alignment - 1) & ~(r->alignment - 1);
}
void lxrt_inner_vkGetImageMemoryRequirements(void *dev, uint64_t img, VkMemReq *r)
{
    lxrt_mvk_vkGetImageMemoryRequirements(dev, img, r);
    d3d12_alignment(r);
}
void lxrt_inner_vkGetImageMemoryRequirements2(void *dev, const void *info, VkMemReq2 *r)
{
    lxrt_mvk_vkGetImageMemoryRequirements2(dev, info, r);
    d3d12_alignment(&r->memoryRequirements);
}
void lxrt_inner_vkGetImageMemoryRequirements2KHR(void *dev, const void *info, VkMemReq2 *r)
{
    lxrt_mvk_vkGetImageMemoryRequirements2KHR(dev, info, r);
    d3d12_alignment(&r->memoryRequirements);
}
void lxrt_inner_vkGetDeviceImageMemoryRequirements(void *dev, const void *info, VkMemReq2 *r)
{
    lxrt_mvk_vkGetDeviceImageMemoryRequirements(dev, info, r);
    d3d12_alignment(&r->memoryRequirements);
}
void lxrt_inner_vkGetDeviceImageMemoryRequirementsKHR(void *dev, const void *info, VkMemReq2 *r)
{
    lxrt_mvk_vkGetDeviceImageMemoryRequirementsKHR(dev, info, r);
    d3d12_alignment(&r->memoryRequirements);
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

static VkResult enum_device_exts(VkPhysicalDevice pd, const char *layer, uint32_t *count,
                                 VkExtensionProperties *props);

// LXRT_VK_HIDE_EXTS=name,name: device extensions the program is not told
// about. A diagnostic: VK_EXT_shader_module_identifier hidden makes a game
// that creates pipelines from cached identifiers hand over its SPIR-V.
static int ext_hidden(const char *name)
{
    static const char *list = (const char *)1;
    if (list == (const char *)1)
        list = getenv("LXRT_VK_HIDE_EXTS");
    if (!list || !*list)
        return 0;
    for (const char *p = list; *p; ) {
        const char *q = p;
        while (*q && *q != ',') q++;
        size_t n = (size_t)(q - p), i = 0;
        while (i < n && name[i] == p[i]) i++;
        if (i == n && !name[n])
            return 1;
        p = *q ? q + 1 : q;
    }
    return 0;
}

VkResult lxrt_inner_vkEnumerateDeviceExtensionProperties(VkPhysicalDevice pd, const char *layer, uint32_t *count,
                                                         VkExtensionProperties *props)
{
    const char *list = getenv("LXRT_VK_HIDE_EXTS");
    if (!list || !*list || layer)
        return enum_device_exts(pd, layer, count, props);
    enum { MAXE = 512 };
    static VkExtensionProperties all[MAXE];
    uint32_t n = MAXE;
    VkResult r = enum_device_exts(pd, 0, &n, all);
    if (r < 0)
        return r;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++)
        if (!ext_hidden(all[i].extensionName))
            all[k++] = all[i];
    if (!props) {
        *count = k;
        return VK_SUCCESS;
    }
    uint32_t w = *count < k ? *count : k;
    for (uint32_t i = 0; i < w; i++)
        props[i] = all[i];
    *count = w;
    return w < k ? VK_INCOMPLETE : VK_SUCCESS;
}

static VkResult enum_device_exts(VkPhysicalDevice pd, const char *layer, uint32_t *count,
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
// LXRT_VK_ANISOTROPY=<2|4|8|16> (launcher "Filtrado anisotrópico"): samplers
// that filter linearly across mip levels get at least that much anisotropic
// filtering (lxrt_inner_vkCreateSampler). DXVK applies its own to Direct3D
// games; a native Vulkan game (Counter-Strike 2) got nothing from the setting
// (the user, 2026-10-04). A game asking for more keeps its own. It needs the
// device feature, turned on here when the driver has it.
static int aniso_level(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("LXRT_VK_ANISOTROPY");
        int v = 0;
        for (; e && *e >= '0' && *e <= '9'; e++)
            v = v * 10 + (*e - '0');
        n = (v == 2 || v == 4 || v == 8 || v == 16) ? v : 0;
    }
    return n;
}
#define ANISO_DEVICES 16
static VkDevice aniso_devices[ANISO_DEVICES];

static void aniso_note(VkDevice d)
{
    for (int i = 0; i < ANISO_DEVICES; i++) {
        VkDevice zero = 0;
        if (__atomic_compare_exchange_n(&aniso_devices[i], &zero, d, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) || zero == d)
            return;
    }
}

static int aniso_has(VkDevice d)
{
    for (int i = 0; i < ANISO_DEVICES; i++)
        if (__atomic_load_n(&aniso_devices[i], __ATOMIC_ACQUIRE) == d)
            return 1;
    return 0;
}

void lxrt_aniso_forget(VkDevice d)
{
    for (int i = 0; i < ANISO_DEVICES; i++) {
        VkDevice cur = d;
        __atomic_compare_exchange_n(&aniso_devices[i], &cur, (VkDevice)0, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
}

static VkResult create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *cci, const void *alloc, VkDevice *out)
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
    {
        const char *d = getenv("LXRT_VK_DEBUG");
        if (d && *d == '1')
            for (uint32_t i = 0; i < old_count; i++)
                dprintf(2, "[shim] device extension requested: %s\n", old_names[i]);
    }
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

    // Anisotropic filtering for the samplers (aniso_level): the feature on.
    VkBool32 aniso_saved[2] = { 0, 0 };
    VkPhysicalDeviceFeatures aniso_only;
    const VkPhysicalDeviceFeatures *old_enabled = ci->pEnabledFeatures;
    int aniso = aniso_level() && real.f[F_SAMPLER_ANISOTROPY];
    if (aniso) {
        for (int s = 0; s < nsets; s++) {
            aniso_saved[s] = sets[s]->f[F_SAMPLER_ANISOTROPY];
            sets[s]->f[F_SAMPLER_ANISOTROPY] = 1;
        }
        if (!nsets) {
            for (int i = 0; i < N_FEATURES; i++)
                aniso_only.f[i] = 0;
            aniso_only.f[F_SAMPLER_ANISOTROPY] = 1;
            ci->pEnabledFeatures = &aniso_only;
        }
    }

    VkResult r = lxrt_mvk_vkCreateDevice(pd, ci, alloc, out);

    if (aniso) {
        for (int s = 0; s < nsets; s++)
            sets[s]->f[F_SAMPLER_ANISOTROPY] = aniso_saved[s];
        ci->pEnabledFeatures = old_enabled;
        if (r == VK_SUCCESS && out)
            aniso_note(*out);
    }
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

void lxrt_note_device(VkDevice dev, VkPhysicalDevice pd);   // present.c

typedef struct {
    int32_t sType; const void *pNext; uint32_t flags;
    int32_t magFilter, minFilter, mipmapMode, addressModeU, addressModeV, addressModeW;
    float mipLodBias; VkBool32 anisotropyEnable; float maxAnisotropy;
    VkBool32 compareEnable; int32_t compareOp; float minLod, maxLod;
    int32_t borderColor; VkBool32 unnormalizedCoordinates;
} VkSamplerCreateInfo;
VkResult lxrt_mvk_vkCreateSampler(VkDevice, const VkSamplerCreateInfo *, const void *, uint64_t *);

// The samplers aniso_level() applies to: linear magnification and
// minification, mip levels to filter across (with either mip mode: a game's
// "bilinear" setting is nearest between levels, and anisotropy works with
// both), no depth compare (shadow lookups), normalized coordinates, and no
// extension structure (YCbCr conversions and reduction modes do not take
// anisotropy).
VkResult lxrt_inner_vkCreateSampler(VkDevice dev, const VkSamplerCreateInfo *ci, const void *alloc, uint64_t *out)
{
    int n = aniso_level();
    if (n && ci && !ci->pNext && ci->magFilter == 1 && ci->minFilter == 1 && !ci->compareEnable &&
        !ci->unnormalizedCoordinates && ci->maxLod > ci->minLod &&
        (!ci->anisotropyEnable || ci->maxAnisotropy < (float)n) && aniso_has(dev)) {
        VkSamplerCreateInfo c = *ci;
        c.anisotropyEnable = 1;
        c.maxAnisotropy = (float)n;
        return lxrt_mvk_vkCreateSampler(dev, &c, alloc, out);
    }
    return lxrt_mvk_vkCreateSampler(dev, ci, alloc, out);
}

VkResult lxrt_inner_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *cci, const void *alloc, VkDevice *out)
{
    VkResult r = create_device(pd, cci, alloc, out);
    if (r == VK_SUCCESS && out)
        lxrt_note_device(*out, pd);
    return r;
}

// LXRT_VK_CALLS=1: what creating graphics pipelines cost and returned since
// the last report (shim/scaler.c prints it with the call counts): a game that
// compiles on its render thread shows here, and so does one that asks again
// every frame for pipelines it was refused.
struct lxrt_pipe_stats { unsigned ok, failed, cache_only; int last_error; unsigned long long ns; };
struct lxrt_pipe_stats lxrt_pipe_stats;
extern int lxrt_vk_calls_on;
struct lxrt_ts2 { long tv_sec, tv_nsec; };
extern int clock_gettime(int, struct lxrt_ts2 *);

int lxrt_pipe_log_on(void);                 // spirv_names.c
uint64_t lxrt_module_hash(uint64_t module);   // spirv_names.c
const void *lxrt_a2c_pipelines(uint32_t count, const void *cis, void **to_free);   // a2c.c
void lxrt_a2c_free(void *to_free);                                                 // a2c.c

static VkResult create_pipelines_raw(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                     const VkGraphicsPipelineCreateInfo *cis, const void *alloc, VkPipeline *out);

// One-sample alpha-to-coverage as a cut at alpha 0.5 (shim/a2c.c).
static VkResult create_pipelines_counted(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                         const VkGraphicsPipelineCreateInfo *cis, const void *alloc, VkPipeline *out)
{
    void *to_free = 0;
    const VkGraphicsPipelineCreateInfo *use = lxrt_a2c_pipelines(n, cis, &to_free);
    VkResult r = create_pipelines_raw(dev, cache, n, use, alloc, out);
    lxrt_a2c_free(to_free);
    return r;
}

static VkResult create_pipelines_raw(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                     const VkGraphicsPipelineCreateInfo *cis, const void *alloc, VkPipeline *out)
{
    if (lxrt_pipe_log_on())
        for (uint32_t i = 0; i < n; i++) {
            // VkPipelineShaderStageCreateInfo: sType, pNext, flags, stage, module, ...
            struct stage { int32_t sType; const void *pNext; uint32_t flags, stage; uint64_t module; const char *name; const void *spec; };
            const struct stage *st = cis[i].pStages;
            for (uint32_t j = 0; st && j < cis[i].stageCount; j++)
                if (st[j].stage == 0x10 /* FRAGMENT */)
                    dprintf(2, "[shim] pipeline fs %016llx\n", (unsigned long long)lxrt_module_hash(st[j].module));
        }
    if (!lxrt_vk_calls_on)
        return lxrt_mvk_vkCreateGraphicsPipelines(dev, cache, n, cis, alloc, out);
    struct lxrt_ts2 a, b;
    clock_gettime(1, &a);
    VkResult r = lxrt_mvk_vkCreateGraphicsPipelines(dev, cache, n, cis, alloc, out);
    clock_gettime(1, &b);
    __atomic_fetch_add(&lxrt_pipe_stats.ns, (unsigned long long)((b.tv_sec - a.tv_sec) * 1000000000ll + (b.tv_nsec - a.tv_nsec)),
                       __ATOMIC_RELAXED);
    __atomic_fetch_add(r == VK_SUCCESS ? &lxrt_pipe_stats.ok : &lxrt_pipe_stats.failed, n, __ATOMIC_RELAXED);
    if (r != VK_SUCCESS)
        lxrt_pipe_stats.last_error = r;
    for (uint32_t i = 0; i < n; i++)
        if (cis[i].flags & 0x100 /* FAIL_ON_PIPELINE_COMPILE_REQUIRED */)
            __atomic_fetch_add(&lxrt_pipe_stats.cache_only, 1, __ATOMIC_RELAXED);
    return r;
}

// Depth clip state -> depth clamp, per pipeline, then restore.
// LXRT_VK_DUMP_SPIRV (shim/spirv_names.c): shader code given inline to a
// pipeline (VK_KHR_maintenance5), which never passes through
// vkCreateShaderModule.
void lxrt_spirv_dump_stages(const void *stages, uint32_t count);

VkResult lxrt_inner_vkCreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                   const VkGraphicsPipelineCreateInfo *cis, const void *alloc, VkPipeline *out)
{
    for (uint32_t i = 0; cis && i < n; i++)
        lxrt_spirv_dump_stages(cis[i].pStages, cis[i].stageCount);
    enum { MAXP = 64 };
    if (!spoof_on() || n > MAXP)
        return create_pipelines_counted(dev, cache, n, cis, alloc, out);
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
    VkResult r = create_pipelines_counted(dev, cache, n, cis, alloc, out);
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
