// Op SPATIAL of shim/mfx_temporal.h: a spatial upscale of the colour image
// onto the output, recorded by this shim into the sender's command buffer,
// for DLSS 1 (tools/dlss-metalfx: the per-game networks of 2018-2019, which
// MetalFX's temporal scaler cannot stand in for, since those games give NGX
// no depth or exposure as DLSS 2 does; the user wants them spatial).
//
// The pass is shim/scaler.c's FSR 1: EASU from the colour image into a
// private image of the output's size, RCAS (when the command asks for
// sharpening) into a second, then vkCmdCopyImage onto the output at its
// offset. EASU is drawn with the shader KosmicKrisp's FSR-1 hook recognises
// (shim/scaler/easu_set1.frag: the input at set 1 binding 30, the constants
// in a uniform buffer at set 1 binding 1, con0 first; patches/kosmickrisp-14,
// -16, -17 in the main checkout, kk_fsr_metalfx_draw): on KosmicKrisp that
// draw becomes MetalFX's spatial scaler ("kk: FSR by MetalFX: picture WxH ->
// WxH" on stderr), on MoltenVK it is FSR 1's EASU. The hook takes only a
// pass drawing the whole of a plain 2D image from its origin with no
// blending, scissor, depth or stencil, which is why the pass draws into a
// private image rather than the game's output (whose usage, offset and
// format are the game's) and copies after. The copy costs an output-sized
// blit a frame.
//
// Objects (the images, their memory, views, passes, pipelines, the uniform
// buffer, a descriptor set) are made on the first SPATIAL of a context and
// kept by (device, images, formats, sizes); a change makes a new variant,
// and a context keeps up to SPATIAL_VARIANTS of them, the oldest replaced
// (its objects destroyed: a game that changes images every frame and keeps
// four frames in flight would reuse objects a command buffer still names;
// no DLSS game is known to). DESTROY frees them; vkDestroyDevice frees what
// is left on the device (scaler.c calls lxrt_spatial_device_gone).
//
// The command buffer's device: recorded at vkAllocateCommandBuffers (a
// table keyed by the handle; a handle reused by the driver is updated when
// allocated again). The driver's own entry points are called
// (lxrt_mvk_...), as scaler.c does, not this shim's overrides.
//
// Image layouts: the colour image is moved to SHADER_READ_ONLY and back to
// GENERAL, the output to TRANSFER_DST and back to GENERAL, around the pass.
// What layout the game keeps them in is not known here; on the two drivers
// this shim serves layouts are not a thing (Metal has none: KosmicKrisp
// unified image layouts, MoltenVK ignores them), and the barriers' access
// masks are what matters.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <vulkan/vulkan_core.h>
#include "mfx_temporal.h"
#include "scaler_spv.h"

extern int dprintf(int, const char *, ...);
extern char *getenv(const char *);
void *memset(void *, int, size_t);
void *memcpy(void *, const void *, size_t);

VkPhysicalDevice lxrt_device_pd(VkDevice dev);   // present.c

VkResult lxrt_mvk_vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
VkResult lxrt_mvk_vkCreateImage(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *);
void lxrt_mvk_vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *);
void lxrt_mvk_vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements *);
VkResult lxrt_mvk_vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *, VkDeviceMemory *);
void lxrt_mvk_vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
VkResult lxrt_mvk_vkCreateImageView(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *, VkImageView *);
void lxrt_mvk_vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo *, const VkAllocationCallbacks *, VkRenderPass *);
void lxrt_mvk_vkDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo *, const VkAllocationCallbacks *, VkFramebuffer *);
void lxrt_mvk_vkDestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *, const VkAllocationCallbacks *, VkShaderModule *);
void lxrt_mvk_vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo *, const VkAllocationCallbacks *,
                                              VkDescriptorSetLayout *);
void lxrt_mvk_vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo *, const VkAllocationCallbacks *,
                                         VkPipelineLayout *);
void lxrt_mvk_vkDestroyPipelineLayout(VkDevice, VkPipelineLayout, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *,
                                            const VkAllocationCallbacks *, VkPipeline *);
void lxrt_mvk_vkDestroyPipeline(VkDevice, VkPipeline, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateSampler(VkDevice, const VkSamplerCreateInfo *, const VkAllocationCallbacks *, VkSampler *);
void lxrt_mvk_vkDestroySampler(VkDevice, VkSampler, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo *, const VkAllocationCallbacks *,
                                         VkDescriptorPool *);
void lxrt_mvk_vkDestroyDescriptorPool(VkDevice, VkDescriptorPool, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *, VkDescriptorSet *);
void lxrt_mvk_vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet *, uint32_t, const VkCopyDescriptorSet *);
VkResult lxrt_mvk_vkCreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *);
void lxrt_mvk_vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks *);
void lxrt_mvk_vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *);
VkResult lxrt_mvk_vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
VkResult lxrt_mvk_vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **);
void lxrt_mvk_vkUnmapMemory(VkDevice, VkDeviceMemory);
void lxrt_mvk_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
void lxrt_mvk_vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                                   uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *,
                                   uint32_t, const VkImageMemoryBarrier *);
void lxrt_mvk_vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo *, VkSubpassContents);
void lxrt_mvk_vkCmdEndRenderPass(VkCommandBuffer);
void lxrt_mvk_vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
void lxrt_mvk_vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                                      const VkDescriptorSet *, uint32_t, const uint32_t *);
void lxrt_mvk_vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void *);
void lxrt_mvk_vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
void lxrt_mvk_vkCmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t,
                             const VkImageCopy *);

static int debug_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_VK_DEBUG");
        on = e && *e == '1';
    }
    return on;
}

// ------------------------------------------------- command buffer -> device
enum { CB_SLOTS = 4096, CB_PROBE = 16 };
static struct { VkCommandBuffer cb; VkDevice dev; } g_cb[CB_SLOTS];
static volatile int g_cb_lock;

static void lock(volatile int *l) { while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE)) ; }
static void unlock(volatile int *l) { __atomic_store_n(l, 0, __ATOMIC_RELEASE); }

static unsigned cb_hash(VkCommandBuffer cb)
{
    uint64_t h = (uint64_t)(uintptr_t)cb * 0x9E3779B97F4A7C15ull;
    return (unsigned)(h >> 40) % CB_SLOTS;
}

static void cb_note(VkCommandBuffer cb, VkDevice dev)
{
    unsigned h = cb_hash(cb), k = CB_SLOTS;
    lock(&g_cb_lock);
    for (unsigned i = 0; i < CB_PROBE && k == CB_SLOTS; i++) {
        unsigned s = (h + i) % CB_SLOTS;
        if (g_cb[s].cb == cb || !g_cb[s].cb)
            k = s;
    }
    if (k == CB_SLOTS)   // a full neighbourhood: the oldest-looking slot (lossy, never wrong for cb)
        k = h;
    g_cb[k].cb = cb;
    g_cb[k].dev = dev;
    unlock(&g_cb_lock);
}

static VkDevice cb_device(VkCommandBuffer cb)
{
    unsigned h = cb_hash(cb);
    VkDevice dev = 0;
    lock(&g_cb_lock);
    for (unsigned i = 0; i < CB_PROBE && !dev; i++) {
        unsigned s = (h + i) % CB_SLOTS;
        if (g_cb[s].cb == cb)
            dev = g_cb[s].dev;
    }
    unlock(&g_cb_lock);
    return dev;
}

__attribute__((visibility("hidden")))
VkResult lxrt_inner_vkAllocateCommandBuffers(VkDevice dev, const VkCommandBufferAllocateInfo *info, VkCommandBuffer *out)
{
    VkResult r = lxrt_mvk_vkAllocateCommandBuffers(dev, info, out);
    if (r == VK_SUCCESS && info && out)
        for (uint32_t i = 0; i < info->commandBufferCount; i++)
            cb_note(out[i], dev);
    return r;
}

// ------------------------------------------------------------ the objects
enum { SPATIAL_CONTEXTS = 16, SPATIAL_VARIANTS = 4 };

typedef struct {
    int used;
    unsigned serial;            // for the oldest-replaced rule
    VkDevice dev;
    VkImage color, output;
    VkFormat cfmt, ofmt;
    uint32_t rw, rh, uw, uh, ctw, cth;
    // the pass
    VkImage mid, res;
    VkDeviceMemory mmem, rmem;
    VkImageView cview, mview, rview;
    VkRenderPass rp;
    VkFramebuffer fb_mid, fb_res;
    VkDescriptorSetLayout dsl0, dsl1, dsl_rcas;
    VkPipelineLayout pl_easu, pl_rcas;
    VkPipeline p_easu, p_rcas;
    VkSampler sampler;
    VkDescriptorPool dpool;
    VkDescriptorSet ds_easu, ds_rcas;
    VkBuffer ubo;
    VkDeviceMemory umem;
} variant;

typedef struct {
    uint64_t context;
    int used;
    unsigned next_serial;
    variant v[SPATIAL_VARIANTS];
} spatial_ctx;

static spatial_ctx g_ctx[SPATIAL_CONTEXTS];
static volatile int g_ctx_lock;

static void variant_destroy(variant *v)
{
    VkDevice d = v->dev;
    if (!v->used)
        return;
    if (v->dpool) lxrt_mvk_vkDestroyDescriptorPool(d, v->dpool, 0);
    if (v->sampler) lxrt_mvk_vkDestroySampler(d, v->sampler, 0);
    if (v->p_easu) lxrt_mvk_vkDestroyPipeline(d, v->p_easu, 0);
    if (v->p_rcas) lxrt_mvk_vkDestroyPipeline(d, v->p_rcas, 0);
    if (v->pl_easu) lxrt_mvk_vkDestroyPipelineLayout(d, v->pl_easu, 0);
    if (v->pl_rcas) lxrt_mvk_vkDestroyPipelineLayout(d, v->pl_rcas, 0);
    if (v->dsl0) lxrt_mvk_vkDestroyDescriptorSetLayout(d, v->dsl0, 0);
    if (v->dsl1) lxrt_mvk_vkDestroyDescriptorSetLayout(d, v->dsl1, 0);
    if (v->dsl_rcas) lxrt_mvk_vkDestroyDescriptorSetLayout(d, v->dsl_rcas, 0);
    if (v->fb_mid) lxrt_mvk_vkDestroyFramebuffer(d, v->fb_mid, 0);
    if (v->fb_res) lxrt_mvk_vkDestroyFramebuffer(d, v->fb_res, 0);
    if (v->rp) lxrt_mvk_vkDestroyRenderPass(d, v->rp, 0);
    if (v->cview) lxrt_mvk_vkDestroyImageView(d, v->cview, 0);
    if (v->mview) lxrt_mvk_vkDestroyImageView(d, v->mview, 0);
    if (v->rview) lxrt_mvk_vkDestroyImageView(d, v->rview, 0);
    if (v->mid) lxrt_mvk_vkDestroyImage(d, v->mid, 0);
    if (v->res) lxrt_mvk_vkDestroyImage(d, v->res, 0);
    if (v->mmem) lxrt_mvk_vkFreeMemory(d, v->mmem, 0);
    if (v->rmem) lxrt_mvk_vkFreeMemory(d, v->rmem, 0);
    if (v->ubo) lxrt_mvk_vkDestroyBuffer(d, v->ubo, 0);
    if (v->umem) lxrt_mvk_vkFreeMemory(d, v->umem, 0);
    memset(v, 0, sizeof *v);
}

static uint32_t memory_type(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    lxrt_mvk_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t t = 0; t < mp.memoryTypeCount; t++)
        if ((bits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want)
            return t;
    return UINT32_MAX;
}

static VkResult make_image(variant *v, VkPhysicalDevice pd, VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = v->ofmt,
        .extent = { v->uw, v->uh, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult r = lxrt_mvk_vkCreateImage(v->dev, &ii, 0, img);
    if (r)
        return r;
    VkMemoryRequirements req;
    lxrt_mvk_vkGetImageMemoryRequirements(v->dev, *img, &req);
    uint32_t t = memory_type(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX)
        t = memory_type(pd, req.memoryTypeBits, 0);
    if (t == UINT32_MAX)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, req.size, t };
    if ((r = lxrt_mvk_vkAllocateMemory(v->dev, &ai, 0, mem)) || (r = lxrt_mvk_vkBindImageMemory(v->dev, *img, *mem, 0)))
        return r;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = v->ofmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    return lxrt_mvk_vkCreateImageView(v->dev, &vi, 0, view);
}

// One colour attachment, written whole, read afterwards by a fragment
// shader (RCAS) or a copy. GENERAL throughout: layouts do not exist on the
// drivers this shim serves, and one pass then serves both images.
static VkResult make_render_pass(variant *v)
{
    VkAttachmentDescription a = {
        .format = v->ofmt,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
    };
    VkSubpassDependency dep[2] = {
        { VK_SUBPASS_EXTERNAL, 0,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 },
        { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, 0 },
    };
    VkRenderPassCreateInfo ri = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &a,
        .subpassCount = 1,
        .pSubpasses = &sp,
        .dependencyCount = 2,
        .pDependencies = dep,
    };
    return lxrt_mvk_vkCreateRenderPass(v->dev, &ri, 0, &v->rp);
}

static VkResult make_framebuffer(variant *v, VkImageView view, VkFramebuffer *out)
{
    VkFramebufferCreateInfo fi = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = v->rp,
        .attachmentCount = 1,
        .pAttachments = &view,
        .width = v->uw,
        .height = v->uh,
        .layers = 1,
    };
    return lxrt_mvk_vkCreateFramebuffer(v->dev, &fi, 0, out);
}

// A full-screen pass over the whole output: one colour attachment, all
// four channels, no blending, viewport and scissor the whole image, no
// depth or stencil (what KosmicKrisp's hook requires of EASU's draw).
static VkResult make_pipeline(variant *v, const uint32_t *frag, size_t frag_size, VkPipelineLayout pl, VkPipeline *out)
{
    VkDevice d = v->dev;
    VkShaderModule vs = 0, fs = 0;
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof spv_fullscreen,
                                    spv_fullscreen };
    VkResult r;
    if ((r = lxrt_mvk_vkCreateShaderModule(d, &mi, 0, &vs)))
        return r;
    mi.codeSize = frag_size;
    mi.pCode = frag;
    if ((r = lxrt_mvk_vkCreateShaderModule(d, &mi, 0, &fs))) {
        lxrt_mvk_vkDestroyShaderModule(d, vs, 0);
        return r;
    }
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", 0 },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", 0 },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0,
                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
    VkViewport vp = { 0.0f, 0.0f, (float)v->uw, (float)v->uh, 0.0f, 1.0f };
    VkRect2D sc = { { 0, 0 }, { v->uw, v->uh } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0,
                                                VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0,
                                               VK_FALSE, VK_LOGIC_OP_COPY, 1, &ba };
    VkGraphicsPipelineCreateInfo gi = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = st,
        .pVertexInputState = &vi,
        .pInputAssemblyState = &ia,
        .pViewportState = &vps,
        .pRasterizationState = &rs,
        .pMultisampleState = &ms,
        .pColorBlendState = &cb,
        .layout = pl,
        .renderPass = v->rp,
    };
    r = lxrt_mvk_vkCreateGraphicsPipelines(d, 0, 1, &gi, 0, out);
    lxrt_mvk_vkDestroyShaderModule(d, vs, 0);
    lxrt_mvk_vkDestroyShaderModule(d, fs, 0);
    return r;
}

static uint32_t fbits(float f)
{
    union { float f; uint32_t u; } x = { f };
    return x.u;
}

// FsrEasuCon() of ffx_fsr1.h: the picture (viewport) of vw x vh at (x, y)
// in a texture of tw x th, upscaled to ow x oh. The offset is folded into
// con0.zw (pp = ip * con0.xy + con0.zw).
static void easu_con(uint32_t c[16], float vw, float vh, float tw, float th, float ow, float oh, float x, float y)
{
    c[0] = fbits(vw / ow);
    c[1] = fbits(vh / oh);
    c[2] = fbits(0.5f * vw / ow - 0.5f + x);
    c[3] = fbits(0.5f * vh / oh - 0.5f + y);
    c[4] = fbits(1.0f / tw);
    c[5] = fbits(1.0f / th);
    c[6] = fbits(1.0f / tw);
    c[7] = fbits(-1.0f / th);
    c[8] = fbits(-1.0f / tw);
    c[9] = fbits(2.0f / th);
    c[10] = fbits(1.0f / tw);
    c[11] = fbits(2.0f / th);
    c[12] = fbits(0.0f);
    c[13] = fbits(4.0f / th);
    c[14] = c[15] = 0;
}

static VkResult make_ubo(variant *v, VkPhysicalDevice pd, const struct sa_mfxt_cmd *c)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 64, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, 0 };
    VkResult r = lxrt_mvk_vkCreateBuffer(v->dev, &bi, 0, &v->ubo);
    if (r)
        return r;
    VkMemoryRequirements req;
    lxrt_mvk_vkGetBufferMemoryRequirements(v->dev, v->ubo, &req);
    uint32_t t = memory_type(pd, req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t == UINT32_MAX)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, req.size, t };
    if ((r = lxrt_mvk_vkAllocateMemory(v->dev, &ai, 0, &v->umem)) ||
        (r = lxrt_mvk_vkBindBufferMemory(v->dev, v->ubo, v->umem, 0)))
        return r;
    void *p = 0;
    if ((r = lxrt_mvk_vkMapMemory(v->dev, v->umem, 0, 64, 0, &p)))
        return r;
    uint32_t con[16];
    easu_con(con, (float)v->rw, (float)v->rh, (float)v->ctw, (float)v->cth, (float)v->uw, (float)v->uh,
             (float)c->color.x, (float)c->color.y);
    memcpy(p, con, sizeof con);
    lxrt_mvk_vkUnmapMemory(v->dev, v->umem);
    return VK_SUCCESS;
}

static VkResult make_descriptors(variant *v)
{
    VkDevice d = v->dev;
    VkResult r;
    // Set 0: nothing (the hook looks at set 1). Set 1: the constants at
    // binding 1, the input at binding 30. RCAS: its own set 0 binding 0.
    VkDescriptorSetLayoutCreateInfo d0 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 0, 0 };
    VkDescriptorSetLayoutBinding b1[2] = {
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 },
        { 30, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 },
    };
    VkDescriptorSetLayoutCreateInfo d1 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 2, b1 };
    VkDescriptorSetLayoutBinding br = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, 0 };
    VkDescriptorSetLayoutCreateInfo dr = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &br };
    if ((r = lxrt_mvk_vkCreateDescriptorSetLayout(d, &d0, 0, &v->dsl0)) ||
        (r = lxrt_mvk_vkCreateDescriptorSetLayout(d, &d1, 0, &v->dsl1)) ||
        (r = lxrt_mvk_vkCreateDescriptorSetLayout(d, &dr, 0, &v->dsl_rcas)))
        return r;
    VkDescriptorSetLayout easu_sets[2] = { v->dsl0, v->dsl1 };
    VkPipelineLayoutCreateInfo le = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 2, easu_sets, 0, 0 };
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo lr = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &v->dsl_rcas, 1, &pr };
    if ((r = lxrt_mvk_vkCreatePipelineLayout(d, &le, 0, &v->pl_easu)) ||
        (r = lxrt_mvk_vkCreatePipelineLayout(d, &lr, 0, &v->pl_rcas)))
        return r;
    VkSamplerCreateInfo si = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0.0f,
    };
    if ((r = lxrt_mvk_vkCreateSampler(d, &si, 0, &v->sampler)))
        return r;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 }, { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 2, 2, ps };
    if ((r = lxrt_mvk_vkCreateDescriptorPool(d, &pi, 0, &v->dpool)))
        return r;
    VkDescriptorSetLayout layouts[2] = { v->dsl1, v->dsl_rcas };
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, v->dpool, 2, layouts };
    if ((r = lxrt_mvk_vkAllocateDescriptorSets(d, &ai, sets)))
        return r;
    v->ds_easu = sets[0];
    v->ds_rcas = sets[1];
    VkDescriptorImageInfo in = { v->sampler, v->cview, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo mid = { v->sampler, v->mview, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorBufferInfo con = { v->ubo, 0, 64 };
    VkWriteDescriptorSet w[3] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, v->ds_easu, 30, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &in, 0, 0 },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, v->ds_easu, 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &con, 0 },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, v->ds_rcas, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &mid, 0, 0 },
    };
    lxrt_mvk_vkUpdateDescriptorSets(d, 3, w, 0, 0);
    return VK_SUCCESS;
}

static VkResult variant_make(variant *v, VkDevice dev, const struct sa_mfxt_cmd *c)
{
    VkPhysicalDevice pd = lxrt_device_pd(dev);
    if (!pd)
        return VK_ERROR_UNKNOWN;
    memset(v, 0, sizeof *v);
    v->used = 1;
    v->dev = dev;
    v->color = (VkImage)(uintptr_t)c->color.image;
    v->output = (VkImage)(uintptr_t)c->output.image;
    v->cfmt = (VkFormat)c->color.vk_format;
    v->ofmt = (VkFormat)c->output.vk_format;
    v->rw = c->render_w;
    v->rh = c->render_h;
    v->uw = c->upscale_w;
    v->uh = c->upscale_h;
    v->ctw = c->color_tex_w ? c->color_tex_w : c->color.w ? c->color.w : c->render_w;
    v->cth = c->color_tex_h ? c->color_tex_h : c->color.h ? c->color.h : c->render_h;
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = v->color,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = v->cfmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkResult r;
    if ((r = lxrt_mvk_vkCreateImageView(dev, &vi, 0, &v->cview)) ||
        (r = make_image(v, pd, &v->mid, &v->mmem, &v->mview)) ||
        (r = make_image(v, pd, &v->res, &v->rmem, &v->rview)) ||
        (r = make_render_pass(v)) ||
        (r = make_framebuffer(v, v->mview, &v->fb_mid)) ||
        (r = make_framebuffer(v, v->rview, &v->fb_res)) ||
        (r = make_ubo(v, pd, c)) ||
        (r = make_descriptors(v)) ||
        (r = make_pipeline(v, spv_easu_set1, sizeof spv_easu_set1, v->pl_easu, &v->p_easu)) ||
        (r = make_pipeline(v, spv_rcas, sizeof spv_rcas, v->pl_rcas, &v->p_rcas)))
        return r;
    return VK_SUCCESS;
}

static int variant_fits(const variant *v, VkDevice dev, const struct sa_mfxt_cmd *c)
{
    uint32_t ctw = c->color_tex_w ? c->color_tex_w : c->color.w ? c->color.w : c->render_w;
    uint32_t cth = c->color_tex_h ? c->color_tex_h : c->color.h ? c->color.h : c->render_h;
    return v->used && v->dev == dev && v->color == (VkImage)(uintptr_t)c->color.image &&
           v->output == (VkImage)(uintptr_t)c->output.image && v->cfmt == (VkFormat)c->color.vk_format &&
           v->ofmt == (VkFormat)c->output.vk_format && v->rw == c->render_w && v->rh == c->render_h &&
           v->uw == c->upscale_w && v->uh == c->upscale_h && v->ctw == ctw && v->cth == cth;
}

// Caller holds g_ctx_lock. The context's slot, made if new (0: table full).
static spatial_ctx *ctx_get(uint64_t context)
{
    spatial_ctx *free_slot = 0;
    for (int i = 0; i < SPATIAL_CONTEXTS; i++) {
        if (g_ctx[i].used && g_ctx[i].context == context)
            return &g_ctx[i];
        if (!g_ctx[i].used && !free_slot)
            free_slot = &g_ctx[i];
    }
    if (free_slot) {
        memset(free_slot, 0, sizeof *free_slot);
        free_slot->used = 1;
        free_slot->context = context;
    }
    return free_slot;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                    VkAccessFlags dst, VkPipelineStageFlags sstage, VkPipelineStageFlags dstage)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src,
        .dstAccessMask = dst,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    lxrt_mvk_vkCmdPipelineBarrier(cb, sstage, dstage, 0, 0, 0, 0, 0, 1, &b);
}

static void record(variant *v, VkCommandBuffer cb, const struct sa_mfxt_cmd *c, int rcas)
{
    barrier(cb, v->color, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, v->rp, v->fb_mid,
                                 { { 0, 0 }, { v->uw, v->uh } }, 0, 0 };
    lxrt_mvk_vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    lxrt_mvk_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->p_easu);
    lxrt_mvk_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pl_easu, 1, 1, &v->ds_easu, 0, 0);
    lxrt_mvk_vkCmdDraw(cb, 3, 1, 0, 0);
    lxrt_mvk_vkCmdEndRenderPass(cb);
    VkImage src = v->mid;
    if (rcas) {
        // stops: 0 = the most sharpening (FsrRcasCon); DLSS's sharpness
        // 0..1 is none..most.
        float s = c->sharpness > 1.0f ? 1.0f : c->sharpness;
        float stops = 2.0f * (1.0f - s);
        rb.framebuffer = v->fb_res;
        lxrt_mvk_vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
        lxrt_mvk_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->p_rcas);
        lxrt_mvk_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v->pl_rcas, 0, 1, &v->ds_rcas, 0, 0);
        lxrt_mvk_vkCmdPushConstants(cb, v->pl_rcas, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &stops);
        lxrt_mvk_vkCmdDraw(cb, 3, 1, 0, 0);
        lxrt_mvk_vkCmdEndRenderPass(cb);
        src = v->res;
    }
    barrier(cb, src, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cb, v->output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkImageCopy region = {
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { (int32_t)c->output.x, (int32_t)c->output.y, 0 },
        { v->uw, v->uh, 1 },
    };
    lxrt_mvk_vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, v->output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cb, v->output, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier(cb, v->color, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}

// mfx_temporal.c: an SA_ST_*; *path names what ran, for the log.
__attribute__((visibility("hidden")))
uint32_t lxrt_spatial_record(VkCommandBuffer cb, const struct sa_mfxt_cmd *c, const char **path)
{
    *path = "";
    if (!c->color.image || !c->output.image || !c->render_w || !c->render_h || !c->upscale_w || !c->upscale_h ||
        c->color.image == c->output.image) {
        *path = "colour or output missing, or the same image";
        return SA_ST_BAD_ARGS;
    }
    VkDevice dev = cb_device(cb);
    if (!dev) {
        *path = "a command buffer this shim did not see allocated";
        return SA_ST_BAD_ARGS;
    }
    int rcas = c->sharpness > 0.0f;
    lock(&g_ctx_lock);
    spatial_ctx *x = ctx_get(c->context);
    if (!x) {
        unlock(&g_ctx_lock);
        *path = "too many contexts";
        return SA_ST_UNSUPPORTED;
    }
    variant *v = 0;
    for (int i = 0; i < SPATIAL_VARIANTS && !v; i++)
        if (variant_fits(&x->v[i], dev, c))
            v = &x->v[i];
    if (!v) {
        // A free slot, else the oldest.
        variant *oldest = &x->v[0];
        for (int i = 0; i < SPATIAL_VARIANTS && !v; i++) {
            if (!x->v[i].used)
                v = &x->v[i];
            else if ((int)(x->v[i].serial - oldest->serial) < 0)
                oldest = &x->v[i];
        }
        if (!v) {
            variant_destroy(oldest);
            v = oldest;
        }
        VkResult r = variant_make(v, dev, c);
        v->serial = x->next_serial++;
        if (r != VK_SUCCESS) {
            if (debug_on())
                dprintf(2, "[shim] spatial ctx %llu: objects for %ux%u (texture %ux%u, format %u) -> %ux%u (format %u) failed: %d\n",
                        (unsigned long long)c->context, c->render_w, c->render_h, v->ctw, v->cth, c->color.vk_format,
                        c->upscale_w, c->upscale_h, c->output.vk_format, (int)r);
            variant_destroy(v);
            unlock(&g_ctx_lock);
            *path = "the pass's objects could not be made";
            return SA_ST_UNSUPPORTED;
        }
        if (debug_on())
            dprintf(2, "[shim] spatial ctx %llu: FSR 1 EASU pass %ux%u (texture %ux%u, format %u) -> %ux%u (format %u) made\n",
                    (unsigned long long)c->context, c->render_w, c->render_h, v->ctw, v->cth, c->color.vk_format,
                    c->upscale_w, c->upscale_h, c->output.vk_format);
    }
    record(v, cb, c, rcas);
    unlock(&g_ctx_lock);
    *path = rcas ? "easu+rcas" : "easu";
    return SA_ST_OK;
}

__attribute__((visibility("hidden")))
void lxrt_spatial_destroy(uint64_t context)
{
    lock(&g_ctx_lock);
    for (int i = 0; i < SPATIAL_CONTEXTS; i++)
        if (g_ctx[i].used && g_ctx[i].context == context) {
            for (int j = 0; j < SPATIAL_VARIANTS; j++)
                variant_destroy(&g_ctx[i].v[j]);
            memset(&g_ctx[i], 0, sizeof g_ctx[i]);
        }
    unlock(&g_ctx_lock);
}

// scaler.c, before the device goes: its objects. Its command buffers'
// entries stay (a handle the driver gives out again is noted again).
__attribute__((visibility("hidden")))
void lxrt_spatial_device_gone(VkDevice dev)
{
    lock(&g_ctx_lock);
    for (int i = 0; i < SPATIAL_CONTEXTS; i++) {
        if (!g_ctx[i].used)
            continue;
        int left = 0;
        for (int j = 0; j < SPATIAL_VARIANTS; j++) {
            if (g_ctx[i].v[j].used && g_ctx[i].v[j].dev == dev)
                variant_destroy(&g_ctx[i].v[j]);
            left |= g_ctx[i].v[j].used;
        }
        if (!left)
            memset(&g_ctx[i], 0, sizeof g_ctx[i]);
    }
    unlock(&g_ctx_lock);
}
