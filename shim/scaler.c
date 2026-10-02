// Scaling filters for a swapchain smaller than its window.
//
// A game whose picture is smaller than its window (Wine's display-mode
// emulation, Settings "Escala de resolución") is stretched by MoltenVK when
// shim/present.c asks for it: bilinear, no pass of ours. The other filters
// need the picture as an image of its own. The swapchain the game gets is
// MoltenVK's, created at the window's size, but vkGetSwapchainImagesKHR hands
// the game images of the size it asked for ("virtual" images, one per
// swapchain image), and vkQueuePresentKHR first draws virtual[i] into the
// swapchain's image i:
//
//   LXRT_VK_SCALER=linear    MoltenVK's stretch (the default; present.c)
//                  nearest   whole pixels (vkCmdBlitImage, VK_FILTER_NEAREST)
//                  fsr       AMD FidelityFX Super Resolution 1.0
//                            (third_party/fsr1): EASU to the window's size,
//                            then RCAS sharpening
//                  metalfx   Apple's MetalFX spatial scaler: the runtime
//                            encodes it (runtime/metalfx.m) between two of
//                            the shim's submissions, ordered by a timeline
//                            semaphore's MTLSharedEvent (VK_EXT_metal_objects)
//   LXRT_VK_FSR_SHARPNESS=0..100
//                            RCAS strength in percent (default 90): 100 is
//                            FSR's maximum (0 stops), 0 is 2 stops softer
//   LXRT_VK_SCALER_PROBE=1   tests: after a swapchain's first pass, log the
//                            pixels the pass wrote at the window's centre and
//                            corner (tests/win/run.sh modeset_*)
//
// MoltenVK only, and only to enlarge (a picture larger than its window keeps
// MoltenVK's stretch). Index i of vkAcquireNextImageKHR is the swapchain's
// and the game's at once: virtual[i] is free again whenever image i is, since
// the pass that reads it ends before image i is presented, and so before it
// can be acquired again. If anything here fails, the swapchain is created the
// plain way and MoltenVK stretches it.
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan_metal.h>
#include "lxrt_host.h"
#include "scaler_spv.h"

extern char *getenv(const char *);   // the guest's libc (the shim is -nostdlib)
extern int dprintf(int, const char *, ...);
extern void *calloc(size_t, size_t);
extern void free(void *);

// MoltenVK's entry points (vulkan_shim.S): called directly, not through the
// shim's own wrappers, since these are the shim's objects, not the game's.
VkResult lxrt_mvk_vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *,
                                       VkSwapchainKHR *);
void lxrt_mvk_vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
VkResult lxrt_mvk_vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR *);
VkResult lxrt_mvk_vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
void lxrt_mvk_vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);
void lxrt_mvk_vkGetDeviceQueue2(VkDevice, const VkDeviceQueueInfo2 *, VkQueue *);
void lxrt_mvk_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
VkResult lxrt_mvk_vkCreateImage(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *);
void lxrt_mvk_vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *);
void lxrt_mvk_vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements *);
VkResult lxrt_mvk_vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *,
                                   VkDeviceMemory *);
void lxrt_mvk_vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
VkResult lxrt_mvk_vkCreateImageView(VkDevice, const VkImageViewCreateInfo *, const VkAllocationCallbacks *,
                                    VkImageView *);
void lxrt_mvk_vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo *, const VkAllocationCallbacks *,
                                     VkRenderPass *);
void lxrt_mvk_vkDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo *, const VkAllocationCallbacks *,
                                      VkFramebuffer *);
void lxrt_mvk_vkDestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *, const VkAllocationCallbacks *,
                                       VkShaderModule *);
void lxrt_mvk_vkDestroyShaderModule(VkDevice, VkShaderModule, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo *,
                                              const VkAllocationCallbacks *, VkDescriptorSetLayout *);
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
void lxrt_mvk_vkUpdateDescriptorSets(VkDevice, uint32_t, const VkWriteDescriptorSet *, uint32_t,
                                     const VkCopyDescriptorSet *);
VkResult lxrt_mvk_vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *,
                                      VkCommandPool *);
void lxrt_mvk_vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
VkResult lxrt_mvk_vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
VkResult lxrt_mvk_vkEndCommandBuffer(VkCommandBuffer);
void lxrt_mvk_vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                                   uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *,
                                   uint32_t, const VkImageMemoryBarrier *);
void lxrt_mvk_vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo *, VkSubpassContents);
void lxrt_mvk_vkCmdEndRenderPass(VkCommandBuffer);
void lxrt_mvk_vkCmdBindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline);
void lxrt_mvk_vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                                      const VkDescriptorSet *, uint32_t, const uint32_t *);
void lxrt_mvk_vkCmdPushConstants(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t,
                                 const void *);
void lxrt_mvk_vkCmdDraw(VkCommandBuffer, uint32_t, uint32_t, uint32_t, uint32_t);
void lxrt_mvk_vkCmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t,
                             const VkImageBlit *, VkFilter);
VkResult lxrt_mvk_vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *,
                                    VkSemaphore *);
void lxrt_mvk_vkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateFence(VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *);
void lxrt_mvk_vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkWaitForFences(VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t);
VkResult lxrt_mvk_vkResetFences(VkDevice, uint32_t, const VkFence *);
VkResult lxrt_mvk_vkCreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *);
void lxrt_mvk_vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks *);
void lxrt_mvk_vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *);
VkResult lxrt_mvk_vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
void lxrt_mvk_vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t,
                                     const VkBufferImageCopy *);
VkResult lxrt_mvk_vkQueueWaitIdle(VkQueue);
VkResult lxrt_mvk_vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **);
void lxrt_mvk_vkUnmapMemory(VkDevice, VkDeviceMemory);
void lxrt_mvk_vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t, const VkCommandBuffer *);
void lxrt_mvk_vkCmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t,
                             const VkImageCopy *);
void lxrt_mvk_vkCmdClearColorImage(VkCommandBuffer, VkImage, VkImageLayout, const VkClearColorValue *, uint32_t,
                                   const VkImageSubresourceRange *);
void lxrt_mvk_vkExportMetalObjectsEXT(VkDevice, VkExportMetalObjectsInfoEXT *);

enum { F_LINEAR, F_NEAREST, F_FSR, F_METALFX, F_LINEAR_BLIT };
extern const char *lxrt_vk_driver;   // vulkan_shim.c: the driver in use

// runtime/metalfx.m
struct lxrt_mfx_run {
    void *scaler;
    void *queue, *in, *out, *event;
    uint64_t wait, signal;
};
#define MAXI 8          // swapchain images; MoltenVK gives 2 or 3
#define MID_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT

typedef struct {
    VkSwapchainKHR sc;  // MoltenVK's: the handle the game has
    VkDevice dev;
    int filter;
    VkFormat format;
    VkExtent2D in, out;
    VkRect2D dst;       // where the picture goes in the window: its aspect kept, centred
    uint32_t n;
    VkImage real[MAXI], virt[MAXI];
    VkDeviceMemory vmem[MAXI];
    VkImageView vview[MAXI], rview[MAXI];
    VkFramebuffer rfb[MAXI];
    // fsr: EASU into mid (window size), RCAS from mid into the swapchain image
    VkImage mid;
    VkDeviceMemory mmem;
    VkImageView mview;
    VkFramebuffer mfb;
    VkRenderPass rp_mid, rp_out;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl_easu, pl_rcas;
    VkPipeline p_easu, p_rcas;
    VkSampler sampler;
    VkDescriptorPool dpool;
    VkDescriptorSet ds_easu[MAXI], ds_rcas;
    uint32_t easu[16];
    float stops;
    // submission, made at the first present (the queue family is known then)
    uint32_t family;
    VkCommandPool pool;
    VkCommandBuffer cb[MAXI];
    VkSemaphore done[MAXI];
    VkFence fence[MAXI];
    int pending[MAXI];
    int probed;
    // metalfx: virt[i] -> MetalFX -> mids[i] (window size) -> copy -> real[i].
    // cb[i] hands virt[i] over and signals tl = tlv+1, the runtime's command
    // buffer scales and signals tlv+2, cb2[i] copies. cbf[i]: a bilinear blit
    // instead, should the runtime refuse.
    PFN_vkExportMetalObjectsEXT export_fn;
    VkImage mids[MAXI];
    VkDeviceMemory mmems[MAXI];
    void *tex_in[MAXI], *tex_out[MAXI];
    VkCommandBuffer cb2[MAXI], cbf[MAXI];
    VkSemaphore tl;
    uint64_t tlv;
    void *event, *mfx, *mtl_queue;
    VkQueue mtl_q;
    int mfx_broken;
} scaled;

static scaled *g_sc[16];
static struct { VkQueue q; uint32_t family; } g_q[32];
static unsigned g_q_next;
static volatile int g_lock;

static void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) ; }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

static int s_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20))
            return 0;
    return *a == *b;
}

static int filter_wanted(void)
{
    const char *e = getenv("LXRT_VK_SCALER");
    if (!e || !*e)
        return F_LINEAR;
    if (s_eq(e, "nearest"))
        return F_NEAREST;
    if (s_eq(e, "fsr"))
        return F_FSR;
    if (s_eq(e, "metalfx"))
        return F_METALFX;
    return F_LINEAR;
}

static int sharpness_percent(void)
{
    const char *e = getenv("LXRT_VK_FSR_SHARPNESS");
    int v = 0, any = 0;
    for (; e && *e >= '0' && *e <= '9'; e++, any = 1)
        v = v * 10 + (*e - '0');
    if (!any)
        return 90;
    return v > 100 ? 100 : v;
}

static int debug(void)
{
    const char *d = getenv("LXRT_VK_DEBUG");
    return d && *d == '1';
}

static int probe_wanted(void)
{
    const char *e = getenv("LXRT_VK_SCALER_PROBE");
    return e && *e == '1';
}

static const char *filter_name(int f)
{
    return f == F_NEAREST ? "nearest" : f == F_FSR ? "fsr" : f == F_METALFX ? "metalfx" : f == F_LINEAR_BLIT ? "linear-blit" : "linear";
}

static uint32_t fbits(float f)
{
    union { float f; uint32_t u; } x = { f };
    return x.u;
}

// FsrEasuCon() of ffx_fsr1.h, for the whole input image.
static void easu_con(uint32_t c[16], float iw, float ih, float ow, float oh)
{
    c[0] = fbits(iw / ow);
    c[1] = fbits(ih / oh);
    c[2] = fbits(0.5f * iw / ow - 0.5f);
    c[3] = fbits(0.5f * ih / oh - 0.5f);
    c[4] = fbits(1.0f / iw);
    c[5] = fbits(1.0f / ih);
    c[6] = fbits(1.0f / iw);
    c[7] = fbits(-1.0f / ih);
    c[8] = fbits(-1.0f / iw);
    c[9] = fbits(2.0f / ih);
    c[10] = fbits(1.0f / iw);
    c[11] = fbits(2.0f / ih);
    c[12] = fbits(0.0f);
    c[13] = fbits(4.0f / ih);
    c[14] = c[15] = 0;
}

// The game's picture in the window with its aspect kept, centred: black bars
// on the sides or above and below, as Proton's fullscreen hack drew them.
static VkRect2D fit(VkExtent2D in, VkExtent2D out)
{
    uint32_t w = out.width, h = out.height;
    if ((uint64_t)out.width * in.height > (uint64_t)out.height * in.width)
        w = (uint32_t)((uint64_t)in.width * out.height / in.height);
    else
        h = (uint32_t)((uint64_t)in.height * out.width / in.width);
    if (!w) w = 1;
    if (!h) h = 1;
    return (VkRect2D){ { (int32_t)(out.width - w) / 2, (int32_t)(out.height - h) / 2 }, { w, h } };
}

static scaled *find(VkSwapchainKHR sc)
{
    for (int i = 0; i < 16; i++)
        if (g_sc[i] && g_sc[i]->sc == sc)
            return g_sc[i];
    return 0;
}

static uint32_t queue_family(VkQueue q)
{
    for (int i = 0; i < 32; i++)
        if (g_q[i].q == q)
            return g_q[i].family;
    return 0;
}

static void note_queue(VkQueue q, uint32_t family)
{
    if (!q)
        return;
    lock();
    int k = -1;
    for (int i = 0; i < 32 && k < 0; i++)
        if (g_q[i].q == q || !g_q[i].q)
            k = i;
    if (k < 0)
        k = g_q_next++ % 32;
    g_q[k].q = q;
    g_q[k].family = family;
    unlock();
}

void lxrt_inner_vkGetDeviceQueue(VkDevice dev, uint32_t family, uint32_t index, VkQueue *q)
{
    lxrt_mvk_vkGetDeviceQueue(dev, family, index, q);
    note_queue(*q, family);
}

void lxrt_inner_vkGetDeviceQueue2(VkDevice dev, const VkDeviceQueueInfo2 *info, VkQueue *q)
{
    lxrt_mvk_vkGetDeviceQueue2(dev, info, q);
    note_queue(*q, info->queueFamilyIndex);
}

static void destroy(scaled *s)
{
    VkDevice d = s->dev;
    for (uint32_t i = 0; i < MAXI; i++) {
        if (s->pending[i])
            lxrt_mvk_vkWaitForFences(d, 1, &s->fence[i], VK_TRUE, UINT64_MAX);
        if (s->fence[i]) lxrt_mvk_vkDestroyFence(d, s->fence[i], 0);
        if (s->done[i]) lxrt_mvk_vkDestroySemaphore(d, s->done[i], 0);
        if (s->rfb[i]) lxrt_mvk_vkDestroyFramebuffer(d, s->rfb[i], 0);
        if (s->rview[i]) lxrt_mvk_vkDestroyImageView(d, s->rview[i], 0);
        if (s->vview[i]) lxrt_mvk_vkDestroyImageView(d, s->vview[i], 0);
        if (s->virt[i]) lxrt_mvk_vkDestroyImage(d, s->virt[i], 0);
        if (s->vmem[i]) lxrt_mvk_vkFreeMemory(d, s->vmem[i], 0);
        if (s->mids[i]) lxrt_mvk_vkDestroyImage(d, s->mids[i], 0);
        if (s->mmems[i]) lxrt_mvk_vkFreeMemory(d, s->mmems[i], 0);
    }
    if (s->tl) lxrt_mvk_vkDestroySemaphore(d, s->tl, 0);
    if (s->mfx) lxrt_syscall2(LXRT_NR_MFX_RELEASE, (long)(uintptr_t)s->mfx, 0);
    if (s->pool) lxrt_mvk_vkDestroyCommandPool(d, s->pool, 0);
    if (s->dpool) lxrt_mvk_vkDestroyDescriptorPool(d, s->dpool, 0);
    if (s->sampler) lxrt_mvk_vkDestroySampler(d, s->sampler, 0);
    if (s->p_easu) lxrt_mvk_vkDestroyPipeline(d, s->p_easu, 0);
    if (s->p_rcas) lxrt_mvk_vkDestroyPipeline(d, s->p_rcas, 0);
    if (s->pl_easu) lxrt_mvk_vkDestroyPipelineLayout(d, s->pl_easu, 0);
    if (s->pl_rcas) lxrt_mvk_vkDestroyPipelineLayout(d, s->pl_rcas, 0);
    if (s->dsl) lxrt_mvk_vkDestroyDescriptorSetLayout(d, s->dsl, 0);
    if (s->mfb) lxrt_mvk_vkDestroyFramebuffer(d, s->mfb, 0);
    if (s->mview) lxrt_mvk_vkDestroyImageView(d, s->mview, 0);
    if (s->mid) lxrt_mvk_vkDestroyImage(d, s->mid, 0);
    if (s->mmem) lxrt_mvk_vkFreeMemory(d, s->mmem, 0);
    if (s->rp_mid) lxrt_mvk_vkDestroyRenderPass(d, s->rp_mid, 0);
    if (s->rp_out) lxrt_mvk_vkDestroyRenderPass(d, s->rp_out, 0);
    free(s);
}

static VkResult image_memory(scaled *s, VkPhysicalDevice pd, VkImage img, VkDeviceMemory *mem)
{
    VkMemoryRequirements req;
    VkPhysicalDeviceMemoryProperties mp;
    lxrt_mvk_vkGetImageMemoryRequirements(s->dev, img, &req);
    lxrt_mvk_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++)
        if ((req.memoryTypeBits & (1u << t)) &&
            (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = t;
    if (type == UINT32_MAX)
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, req.size, type };
    VkResult r = lxrt_mvk_vkAllocateMemory(s->dev, &ai, 0, mem);
    return r ? r : lxrt_mvk_vkBindImageMemory(s->dev, img, *mem, 0);
}

static VkResult view(VkDevice d, VkImage img, VkFormat f, VkImageView *out)
{
    VkImageViewCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = f,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    return lxrt_mvk_vkCreateImageView(d, &vi, 0, out);
}

static VkResult render_pass(VkDevice d, VkFormat f, VkImageLayout final, bool clear, VkRenderPass *out)
{
    VkAttachmentDescription a = {
        .format = f,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = final,
    };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
    };
    // In: earlier reads of the image (mid: the last frame's RCAS) and writes
    // before the pass. Out: what the pass wrote, for the next pass's reads.
    VkSubpassDependency dep[2] = {
        { VK_SUBPASS_EXTERNAL, 0,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 },
        { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, 0 },
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
    return lxrt_mvk_vkCreateRenderPass(d, &ri, 0, out);
}

static VkResult framebuffer(VkDevice d, VkRenderPass rp, VkImageView v, VkExtent2D e, VkFramebuffer *out)
{
    VkFramebufferCreateInfo fi = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp,
        .attachmentCount = 1,
        .pAttachments = &v,
        .width = e.width,
        .height = e.height,
        .layers = 1,
    };
    return lxrt_mvk_vkCreateFramebuffer(d, &fi, 0, out);
}

static VkResult pipeline(scaled *s, const uint32_t *frag, size_t frag_size, uint32_t push, VkRenderPass rp,
                         VkRect2D area, VkPipelineLayout *pl, VkPipeline *out)
{
    VkDevice d = s->dev;
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, push };
    VkPipelineLayoutCreateInfo li = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &s->dsl,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pr,
    };
    VkResult r = lxrt_mvk_vkCreatePipelineLayout(d, &li, 0, pl);
    if (r)
        return r;
    VkShaderModule vs = 0, fs = 0;
    VkShaderModuleCreateInfo mi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof spv_fullscreen,
                                    spv_fullscreen };
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
    VkViewport vp = { (float)area.offset.x, (float)area.offset.y, (float)area.extent.width,
                      (float)area.extent.height, 0, 1 };
    VkRect2D sc = area;
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0,
                                              1, &vp, 1, &sc };
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
        .layout = *pl,
        .renderPass = rp,
    };
    r = lxrt_mvk_vkCreateGraphicsPipelines(d, 0, 1, &gi, 0, out);
    lxrt_mvk_vkDestroyShaderModule(d, vs, 0);
    lxrt_mvk_vkDestroyShaderModule(d, fs, 0);
    return r;
}

static void write_set(VkDevice d, VkDescriptorSet set, VkSampler smp, VkImageView v)
{
    VkDescriptorImageInfo ii = { smp, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = set,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &ii,
    };
    lxrt_mvk_vkUpdateDescriptorSets(d, 1, &w, 0, 0);
}

// The FSR passes' objects: render passes, pipelines, mid, descriptor sets.
static VkResult setup_fsr(scaled *s, VkPhysicalDevice pd)
{
    VkDevice d = s->dev;
    VkResult r;
    if ((r = render_pass(d, MID_FORMAT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, true, &s->rp_mid)) ||
        (r = render_pass(d, s->format, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, false, &s->rp_out)))
        return r;
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = MID_FORMAT,
        .extent = { s->out.width, s->out.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if ((r = lxrt_mvk_vkCreateImage(d, &ii, 0, &s->mid)) || (r = image_memory(s, pd, s->mid, &s->mmem)) ||
        (r = view(d, s->mid, MID_FORMAT, &s->mview)) || (r = framebuffer(d, s->rp_mid, s->mview, s->out, &s->mfb)))
        return r;
    for (uint32_t i = 0; i < s->n; i++)
        if ((r = view(d, s->real[i], s->format, &s->rview[i])) ||
            (r = framebuffer(d, s->rp_out, s->rview[i], s->out, &s->rfb[i])))
            return r;
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &b };
    if ((r = lxrt_mvk_vkCreateDescriptorSetLayout(d, &dli, 0, &s->dsl)))
        return r;
    VkRect2D whole = { { 0, 0 }, s->out };
    if ((r = pipeline(s, spv_easu, sizeof spv_easu, 64, s->rp_mid, s->dst, &s->pl_easu, &s->p_easu)) ||
        (r = pipeline(s, spv_rcas, sizeof spv_rcas, 4, s->rp_out, whole, &s->pl_rcas, &s->p_rcas)))
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
    if ((r = lxrt_mvk_vkCreateSampler(d, &si, 0, &s->sampler)))
        return r;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, s->n + 1 };
    VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, s->n + 1, 1, &ps };
    if ((r = lxrt_mvk_vkCreateDescriptorPool(d, &pi, 0, &s->dpool)))
        return r;
    VkDescriptorSetLayout layouts[MAXI + 1];
    VkDescriptorSet sets[MAXI + 1];
    for (uint32_t i = 0; i <= s->n; i++)
        layouts[i] = s->dsl;
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, s->dpool, s->n + 1, layouts };
    if ((r = lxrt_mvk_vkAllocateDescriptorSets(d, &ai, sets)))
        return r;
    for (uint32_t i = 0; i < s->n; i++) {
        s->ds_easu[i] = sets[i];
        write_set(d, sets[i], s->sampler, s->vview[i]);
    }
    s->ds_rcas = sets[s->n];
    write_set(d, s->ds_rcas, s->sampler, s->mview);
    // EASU at the picture's rectangle: FsrEasuCon for its size, and its
    // offset folded into con0.zw (pp = ip * con0.xy + con0.zw, with ip the
    // window's pixel), so the shader is the same.
    easu_con(s->easu, (float)s->in.width, (float)s->in.height, (float)s->dst.extent.width,
             (float)s->dst.extent.height);
    union { uint32_t u; float f; } c0 = { s->easu[0] }, c1 = { s->easu[1] }, c2 = { s->easu[2] }, c3 = { s->easu[3] };
    c2.f -= (float)s->dst.offset.x * c0.f;
    c3.f -= (float)s->dst.offset.y * c1.f;
    s->easu[2] = c2.u;
    s->easu[3] = c3.u;
    s->stops = 2.0f * (1.0f - (float)sharpness_percent() / 100.0f);
    return VK_SUCCESS;
}

static void *export_texture(scaled *s, VkImage img)
{
    VkExportMetalTextureInfoEXT t = { VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT, 0, img, 0, 0,
                                      VK_IMAGE_ASPECT_PLANE_0_BIT, 0 };
    VkExportMetalObjectsInfoEXT o = { VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &t };
    s->export_fn(s->dev, &o);
    return (void *)t.mtlTexture;
}

// The MetalFX pass's objects: output images, the Metal textures, the timeline
// semaphore and its MTLSharedEvent. The scaler itself is the runtime's, made
// at the first present.
static VkResult setup_mfx(scaled *s, VkPhysicalDevice pd)
{
    VkDevice d = s->dev;
    // MoltenVK's exported entry point: vkGetDeviceProcAddr answers NULL for
    // it unless the game enabled VK_EXT_metal_objects (none does), and
    // MoltenVK's export does not depend on that (MEASURED: setup failed with
    // -7 through vkGetDeviceProcAddr).
    s->export_fn = lxrt_mvk_vkExportMetalObjectsEXT;
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = s->format,
        .extent = { s->dst.extent.width, s->dst.extent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_SAMPLED_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkResult r;
    for (uint32_t i = 0; i < s->n; i++) {
        if ((r = lxrt_mvk_vkCreateImage(d, &ii, 0, &s->mids[i])) || (r = image_memory(s, pd, s->mids[i], &s->mmems[i])))
            return r;
        if (!(s->tex_in[i] = export_texture(s, s->virt[i])) || !(s->tex_out[i] = export_texture(s, s->mids[i])))
            return VK_ERROR_FEATURE_NOT_PRESENT;
    }
    VkExportMetalObjectCreateInfoEXT ex = { VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT, 0,
                                            VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT };
    VkSemaphoreTypeCreateInfo ty = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &ex, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
    VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ty };
    if ((r = lxrt_mvk_vkCreateSemaphore(d, &si, 0, &s->tl)))
        return r;
    VkExportMetalSharedEventInfoEXT ev = { VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT, 0, s->tl, 0, 0 };
    VkExportMetalObjectsInfoEXT o = { VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &ev };
    s->export_fn(d, &o);
    if (!(s->event = (void *)ev.mtlSharedEvent))
        return VK_ERROR_FEATURE_NOT_PRESENT;
    return VK_SUCCESS;
}

static const VkFormat *format_list(const VkSwapchainCreateInfoKHR *ci, uint32_t *n)
{
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            const VkImageFormatListCreateInfo *l = (const void *)p;
            *n = l->viewFormatCount;
            return l->pViewFormats;
        }
    *n = 0;
    return 0;
}

// The swapchain at the window's size and the game's images at its own.
// VK_SUCCESS with *out set, or an error and nothing left behind (the caller
// then creates the swapchain the plain way).
VkResult lxrt_scaler_create(VkDevice dev, VkPhysicalDevice pd, const VkSwapchainCreateInfoKHR *ci,
                            VkExtent2D window, const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    int f = filter_wanted();
    // MoltenVK stretches by itself (present.c); another driver gets a
    // bilinear blit here. MetalFX needs MoltenVK's Metal objects.
    int mvk = s_eq(lxrt_vk_driver, "moltenvk");
    // (KosmicKrisp has no vkExportMetalObjectsEXT: asking MoltenVK's thunk for
    // it there faulted, MEASURED.)
    if (!mvk && (f == F_LINEAR || f == F_METALFX))
        f = F_LINEAR_BLIT;
    if (f == F_LINEAR || ci->imageArrayLayers != 1 || ci->imageExtent.width > window.width ||
        ci->imageExtent.height > window.height || !ci->imageExtent.width || !ci->imageExtent.height)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    scaled *s = calloc(1, sizeof *s);
    if (!s)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->dev = dev;
    s->filter = f;
    s->format = ci->imageFormat;
    s->in = ci->imageExtent;
    s->out = window;
    s->dst = fit(s->in, s->out);

    VkSwapchainCreateInfoKHR c = *ci;
    c.imageExtent = window;
    c.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (probe_wanted())
        c.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkResult r = lxrt_mvk_vkCreateSwapchainKHR(dev, &c, alloc, &s->sc);
    if (r) {
        free(s);
        return r;
    }
    uint32_t n = MAXI;
    r = lxrt_mvk_vkGetSwapchainImagesKHR(dev, s->sc, &n, s->real);
    if (r != VK_SUCCESS)
        goto fail;
    s->n = n;

    uint32_t nfmt;
    const VkFormat *fmts = format_list(ci, &nfmt);
    VkImageFormatListCreateInfo fl = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, 0, nfmt, fmts };
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nfmt ? &fl : 0,
        .flags = (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = ci->imageFormat,
        .extent = { ci->imageExtent.width, ci->imageExtent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = ci->imageUsage | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = ci->imageSharingMode,
        .queueFamilyIndexCount = ci->queueFamilyIndexCount,
        .pQueueFamilyIndices = ci->pQueueFamilyIndices,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    for (uint32_t i = 0; i < n; i++)
        if ((r = lxrt_mvk_vkCreateImage(dev, &ii, 0, &s->virt[i])) ||
            (r = image_memory(s, pd, s->virt[i], &s->vmem[i])) ||
            (r = view(dev, s->virt[i], s->format, &s->vview[i])))
            goto fail;
    if ((f == F_FSR && (r = setup_fsr(s, pd))) || (f == F_METALFX && (r = setup_mfx(s, pd))))
        goto fail;

    lock();
    int k = -1;
    for (int i = 0; i < 16 && k < 0; i++)
        if (!g_sc[i])
            k = i;
    if (k >= 0)
        g_sc[k] = s;
    unlock();
    if (k < 0) {
        r = VK_ERROR_TOO_MANY_OBJECTS;
        goto fail;
    }
    if (debug())
        dprintf(2, "[shim] scaler %s %ux%u -> %ux%u, %u images, sharpness %d%%\n", filter_name(f), s->in.width,
                s->in.height, s->out.width, s->out.height, n, f == F_FSR ? sharpness_percent() : 0);
    *out = s->sc;
    return VK_SUCCESS;
fail:
    if (debug())
        dprintf(2, "[shim] scaler %s: setup failed (%d), MoltenVK stretches instead\n", filter_name(f), (int)r);
    lxrt_mvk_vkDestroySwapchainKHR(dev, s->sc, alloc);
    destroy(s);
    return r ? r : VK_ERROR_INITIALIZATION_FAILED;
}

VkResult lxrt_inner_vkGetSwapchainImagesKHR(VkDevice dev, VkSwapchainKHR sc, uint32_t *count, VkImage *images)
{
    lock();
    scaled *s = find(sc);
    unlock();
    if (!s)
        return lxrt_mvk_vkGetSwapchainImagesKHR(dev, sc, count, images);
    if (!images) {
        *count = s->n;
        return VK_SUCCESS;
    }
    uint32_t k = *count < s->n ? *count : s->n;
    for (uint32_t i = 0; i < k; i++)
        images[i] = s->virt[i];
    *count = k;
    return k < s->n ? VK_INCOMPLETE : VK_SUCCESS;
}

void lxrt_inner_vkDestroySwapchainKHR(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks *alloc)
{
    lock();
    scaled *s = find(sc);
    for (int i = 0; s && i < 16; i++)
        if (g_sc[i] == s)
            g_sc[i] = 0;
    unlock();
    if (s)
        destroy(s);
    lxrt_mvk_vkDestroySwapchainKHR(dev, sc, alloc);
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src, VkPipelineStageFlags dst)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    lxrt_mvk_vkCmdPipelineBarrier(cb, src, dst, 0, 0, 0, 0, 0, 1, &b);
}

// The bars around the picture, in an image laid out for transfers.
static void clear_black(scaled *s, VkCommandBuffer cb, VkImage img)
{
    if (s->dst.extent.width == s->out.width && s->dst.extent.height == s->out.height)
        return;
    VkClearColorValue black = { { 0.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    lxrt_mvk_vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
}

// virt[i] onto real[i] with vkCmdBlitImage (nearest; and bilinear, the
// fallback should MetalFX be refused).
static void cmd_blit(scaled *s, VkCommandBuffer cb, uint32_t i, VkFilter filter)
{
    barrier(cb, s->virt[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cb, s->real[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    clear_black(s, cb, s->real[i]);
    VkImageBlit region = {
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { { 0, 0, 0 }, { (int32_t)s->in.width, (int32_t)s->in.height, 1 } },
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        { { s->dst.offset.x, s->dst.offset.y, 0 },
          { s->dst.offset.x + (int32_t)s->dst.extent.width, s->dst.offset.y + (int32_t)s->dst.extent.height, 1 } },
    };
    lxrt_mvk_vkCmdBlitImage(cb, s->virt[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->real[i],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, filter);
    barrier(cb, s->real[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    barrier(cb, s->virt[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

static void cmd_fsr(scaled *s, VkCommandBuffer cb, uint32_t i)
{
    barrier(cb, s->virt[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    VkClearValue black = { .color = { { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, 0, s->rp_mid, s->mfb,
                                 { { 0, 0 }, s->out }, 1, &black };
    lxrt_mvk_vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    lxrt_mvk_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->p_easu);
    lxrt_mvk_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pl_easu, 0, 1, &s->ds_easu[i], 0, 0);
    lxrt_mvk_vkCmdPushConstants(cb, s->pl_easu, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, s->easu);
    lxrt_mvk_vkCmdDraw(cb, 3, 1, 0, 0);
    lxrt_mvk_vkCmdEndRenderPass(cb);
    rb.renderPass = s->rp_out;
    rb.framebuffer = s->rfb[i];
    rb.clearValueCount = 0;
    rb.pClearValues = 0;
    lxrt_mvk_vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    lxrt_mvk_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->p_rcas);
    lxrt_mvk_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s->pl_rcas, 0, 1, &s->ds_rcas, 0, 0);
    lxrt_mvk_vkCmdPushConstants(cb, s->pl_rcas, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4, &s->stops);
    lxrt_mvk_vkCmdDraw(cb, 3, 1, 0, 0);
    lxrt_mvk_vkCmdEndRenderPass(cb);
    barrier(cb, s->virt[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

// MetalFX, before the runtime's command buffer: the game's writes to virt[i]
// done, mids[i] free to be written.
static void cmd_mfx_in(scaled *s, VkCommandBuffer cb, uint32_t i)
{
    barrier(cb, s->virt[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    barrier(cb, s->mids[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
}

// MetalFX, after: mids[i] (the scaled picture) copied into real[i].
static void cmd_mfx_out(scaled *s, VkCommandBuffer cb, uint32_t i)
{
    barrier(cb, s->mids[i], VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(cb, s->real[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    clear_black(s, cb, s->real[i]);
    VkImageCopy region = {
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 },
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { s->dst.offset.x, s->dst.offset.y, 0 },
        { s->dst.extent.width, s->dst.extent.height, 1 },
    };
    lxrt_mvk_vkCmdCopyImage(cb, s->mids[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->real[i],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    barrier(cb, s->real[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    barrier(cb, s->virt[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, 0, 0,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

enum { CB_MAIN, CB_MFX_OUT, CB_FALLBACK };

// Image i's command buffers, recorded once: the same images every time.
static VkResult record(scaled *s, VkCommandBuffer cb, uint32_t i, int which)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkResult r = lxrt_mvk_vkBeginCommandBuffer(cb, &bi);
    if (r)
        return r;
    if (which == CB_FALLBACK || s->filter == F_LINEAR_BLIT)
        cmd_blit(s, cb, i, VK_FILTER_LINEAR);
    else if (which == CB_MFX_OUT)
        cmd_mfx_out(s, cb, i);
    else if (s->filter == F_NEAREST)
        cmd_blit(s, cb, i, VK_FILTER_NEAREST);
    else if (s->filter == F_FSR)
        cmd_fsr(s, cb, i);
    else
        cmd_mfx_in(s, cb, i);
    return lxrt_mvk_vkEndCommandBuffer(cb);
}

// Command pool, buffers, semaphores and fences for the presenting queue's
// family (made again if a later present comes from another family).
static VkResult setup_submit(scaled *s, uint32_t family)
{
    if (s->pool && s->family == family)
        return VK_SUCCESS;
    VkDevice d = s->dev;
    if (s->pool) {
        for (uint32_t i = 0; i < s->n; i++)
            if (s->pending[i]) {
                lxrt_mvk_vkWaitForFences(d, 1, &s->fence[i], VK_TRUE, UINT64_MAX);
                s->pending[i] = 0;
            }
        lxrt_mvk_vkDestroyCommandPool(d, s->pool, 0);
        s->pool = 0;
    }
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, family };
    VkResult r = lxrt_mvk_vkCreateCommandPool(d, &pi, 0, &s->pool);
    if (r)
        return r;
    s->family = family;
    // metalfx: three per image (in, out, fallback); the others one.
    uint32_t per = s->filter == F_METALFX ? 3 : 1;
    VkCommandBuffer all[3 * MAXI];
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, s->pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, s->n * per };
    if ((r = lxrt_mvk_vkAllocateCommandBuffers(d, &ai, all)))
        return r;
    for (uint32_t i = 0; i < s->n; i++) {
        s->cb[i] = all[i];
        if ((r = record(s, s->cb[i], i, CB_MAIN)))
            return r;
        if (per == 3) {
            s->cb2[i] = all[s->n + i];
            s->cbf[i] = all[2 * s->n + i];
            if ((r = record(s, s->cb2[i], i, CB_MFX_OUT)) || (r = record(s, s->cbf[i], i, CB_FALLBACK)))
                return r;
        }
        VkSemaphoreCreateInfo si = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (!s->done[i] && (r = lxrt_mvk_vkCreateSemaphore(d, &si, 0, &s->done[i])))
            return r;
        if (!s->fence[i] && (r = lxrt_mvk_vkCreateFence(d, &fi, 0, &s->fence[i])))
            return r;
    }
    return VK_SUCCESS;
}

VkPhysicalDevice lxrt_device_pd(VkDevice dev);   // present.c

// LXRT_VK_SCALER_PROBE: the pixels an image of a swapchain holds when it is
// presented, read back once per swapchain: a quarter and three quarters of
// the way across at mid height, and the top-left corner (tests/win/run.sh
// modeset*, whose program draws its left half red and its right half blue).
// Waits for `waits` (consumed) and signals `signal` when given.
static void probe_image(VkDevice d, VkQueue q, uint32_t family, VkImage img, VkExtent2D ext, VkFormat fmt,
                        const VkSemaphore *waits, uint32_t nwaits, VkSemaphore signal, const char *what)
{
    VkPhysicalDevice pd = lxrt_device_pd(d);
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 64, VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf = 0;
    VkDeviceMemory mem = 0;
    VkCommandPool pool = 0;
    VkCommandBuffer cb = 0;
    void *map = 0;
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, family };
    if (!pd || lxrt_mvk_vkCreateBuffer(d, &bi, 0, &buf) || lxrt_mvk_vkCreateCommandPool(d, &pi, 0, &pool))
        goto out;
    VkMemoryRequirements req;
    VkPhysicalDeviceMemoryProperties mp;
    lxrt_mvk_vkGetBufferMemoryRequirements(d, buf, &req);
    lxrt_mvk_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t type = UINT32_MAX;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++)
        if ((req.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want)
            type = t;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, req.size, type };
    VkCommandBufferAllocateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0,
                                       VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (type == UINT32_MAX || lxrt_mvk_vkAllocateMemory(d, &ai, 0, &mem) ||
        lxrt_mvk_vkBindBufferMemory(d, buf, mem, 0) || lxrt_mvk_vkAllocateCommandBuffers(d, &ci, &cb) ||
        lxrt_mvk_vkBeginCommandBuffer(cb, &begin))
        goto out;
    barrier(cb, img, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    int32_t y = (int32_t)ext.height / 2;
    VkBufferImageCopy cp[3] = {
        { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { (int32_t)ext.width / 4, y, 0 }, { 1, 1, 1 } },
        { 16, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { (int32_t)ext.width * 3 / 4, y, 0 }, { 1, 1, 1 } },
        { 32, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { 1, 1, 1 } },
    };
    lxrt_mvk_vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 3, cp);
    barrier(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    VkPipelineStageFlags st[16];
    for (int k = 0; k < 16; k++)
        st[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = nwaits < 16 ? nwaits : 16,
                        .pWaitSemaphores = waits, .pWaitDstStageMask = st, .commandBufferCount = 1,
                        .pCommandBuffers = &cb, .signalSemaphoreCount = signal ? 1 : 0, .pSignalSemaphores = &signal };
    if (lxrt_mvk_vkEndCommandBuffer(cb) || lxrt_mvk_vkQueueSubmit(q, 1, &si, 0) || lxrt_mvk_vkQueueWaitIdle(q) ||
        lxrt_mvk_vkMapMemory(d, mem, 0, 64, 0, &map))
        goto out;
    const uint8_t *px = map;
    dprintf(2, "[shim] probe %s %ux%u format %d left %u,%u,%u,%u right %u,%u,%u,%u corner %u,%u,%u,%u\n", what,
            ext.width, ext.height, (int)fmt, px[0], px[1], px[2], px[3], px[16], px[17], px[18], px[19], px[32],
            px[33], px[34], px[35]);
    lxrt_mvk_vkUnmapMemory(d, mem);
out:
    if (pool)
        lxrt_mvk_vkDestroyCommandPool(d, pool, 0);
    if (buf)
        lxrt_mvk_vkDestroyBuffer(d, buf, 0);
    if (mem)
        lxrt_mvk_vkFreeMemory(d, mem, 0);
}

// Swapchains without a pass of the scaler, for the probe: their size and
// format (present.c notes them when LXRT_VK_SCALER_PROBE is set).
static struct { VkSwapchainKHR sc; VkDevice dev; VkExtent2D ext; VkFormat fmt; int probed; } g_plain[16];

void lxrt_scaler_note_plain(VkDevice dev, VkSwapchainKHR sc, VkExtent2D ext, VkFormat fmt)
{
    lock();
    for (int i = 0; i < 16; i++)
        if (!g_plain[i].sc || g_plain[i].sc == sc) {
            g_plain[i].sc = sc;
            g_plain[i].dev = dev;
            g_plain[i].ext = ext;
            g_plain[i].fmt = fmt;
            g_plain[i].probed = 0;
            break;
        }
    unlock();
}


// MetalFX: cb[i] (waits for the game, signals tlv+1), the runtime's command
// buffer (waits tlv+1, scales, signals tlv+2), cb2[i] (waits tlv+2, copies,
// signals done[i] and the fence). If the runtime refuses, cbf[i] blits
// instead, then and from then on.
static VkResult present_mfx(scaled *s, VkQueue q, uint32_t i, const VkSubmitInfo *game)
{
    if (s->mtl_q != q) {
        VkExportMetalCommandQueueInfoEXT eq = { VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT, 0, q, 0 };
        VkExportMetalObjectsInfoEXT o = { VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &eq };
        s->export_fn(s->dev, &o);
        s->mtl_queue = (void *)eq.mtlCommandQueue;
        s->mtl_q = q;
    }
    uint64_t v = s->tlv, in_done = v + 1, scaled_done = v + 2, none = 0;
    VkTimelineSemaphoreSubmitInfo ta = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, 0, 0, 0, 1, &in_done };
    VkSubmitInfo a = *game;
    a.pNext = &ta;
    a.signalSemaphoreCount = 1;
    a.pSignalSemaphores = &s->tl;
    VkResult r = lxrt_mvk_vkQueueSubmit(q, 1, &a, 0);
    if (r)
        return r;
    struct lxrt_mfx_run run = { s->mfx, s->mtl_queue, s->tex_in[i], s->tex_out[i], s->event, in_done, scaled_done };
    long e = s->mtl_queue ? lxrt_syscall2(LXRT_NR_MFX_ENCODE, (long)(uintptr_t)&run, 0) : -22;
    s->mfx = run.scaler;
    if (e) {
        // The runtime made nothing wait for tlv+1 or signal tlv+2: cbf[i]
        // waits for cb[i] itself (it left virt[i] for shader reads, which
        // MoltenVK, without image layouts, does not mind).
        dprintf(2, "[shim] scaler metalfx: the runtime refused (%ld), bilinear from now on\n", e);
        s->mfx_broken = 1;
        scaled_done = in_done;
    }
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo tb = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, 0, 1, &scaled_done, 1, &none };
    VkSubmitInfo b = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = &tb,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &s->tl,
        .pWaitDstStageMask = &st,
        .commandBufferCount = 1,
        .pCommandBuffers = e ? &s->cbf[i] : &s->cb2[i],
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &s->done[i],
    };
    s->tlv = scaled_done;
    return lxrt_mvk_vkQueueSubmit(q, 1, &b, s->fence[i]);
}

// LXRT_VK_TIMING=1: how long vkAcquireNextImageKHR and vkQueuePresentKHR
// take, summed over 2-second windows and printed per process (stderr).
struct lxrt_ts { long tv_sec, tv_nsec; };
extern int clock_gettime(int, struct lxrt_ts *);
static uint64_t now_ns(void)
{
    struct lxrt_ts t;
    clock_gettime(1 /* CLOCK_MONOTONIC */, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}
static int timing_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_VK_TIMING");
        on = e && *e == '1';
    }
    return on;
}
static struct { uint64_t start, acq_ns, acq_max, pres_ns, pres_max; unsigned acq, pres; } g_t;
static void timing_note(int present, uint64_t ns)
{
    uint64_t t = now_ns();
    lock();
    if (!g_t.start)
        g_t.start = t;
    if (present) { g_t.pres++; g_t.pres_ns += ns; if (ns > g_t.pres_max) g_t.pres_max = ns; }
    else { g_t.acq++; g_t.acq_ns += ns; if (ns > g_t.acq_max) g_t.acq_max = ns; }
    if (t - g_t.start > 2000000000ull) {
        dprintf(2, "[shim] timing %s: %u acquires avg %.2f ms max %.1f ms, %u presents avg %.2f ms max %.1f ms (2 s)\n",
                lxrt_vk_driver, g_t.acq, g_t.acq ? g_t.acq_ns / 1e6 / g_t.acq : 0.0, g_t.acq_max / 1e6, g_t.pres,
                g_t.pres ? g_t.pres_ns / 1e6 / g_t.pres : 0.0, g_t.pres_max / 1e6);
        g_t.start = t; g_t.acq = g_t.pres = 0; g_t.acq_ns = g_t.pres_ns = g_t.acq_max = g_t.pres_max = 0;
    }
    unlock();
}

VkResult lxrt_mvk_vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *);
VkResult lxrt_inner_vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore sem, VkFence f,
                                          uint32_t *index)
{
    if (!timing_on())
        return lxrt_mvk_vkAcquireNextImageKHR(d, sc, timeout, sem, f, index);
    uint64_t t0 = now_ns();
    VkResult r = lxrt_mvk_vkAcquireNextImageKHR(d, sc, timeout, sem, f, index);
    timing_note(0, now_ns() - t0);
    return r;
}

static VkResult present_inner(VkQueue q, const VkPresentInfoKHR *pi);
VkResult lxrt_inner_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi)
{
    if (!timing_on())
        return present_inner(q, pi);
    uint64_t t0 = now_ns();
    VkResult r = present_inner(q, pi);
    timing_note(1, now_ns() - t0);
    return r;
}

static VkResult present_inner(VkQueue q, const VkPresentInfoKHR *pi)
{
    scaled *ss[16];
    uint32_t k = 0, nsc = pi->swapchainCount < 16 ? pi->swapchainCount : 16;
    lock();
    for (uint32_t j = 0; j < nsc; j++)
        if ((ss[j] = find(pi->pSwapchains[j])))
            k++;
    unlock();
    if (!k && probe_wanted()) {
        // A swapchain the scaler leaves alone (MoltenVK's stretch, Proton's
        // own scaling): its image read once, between the game's semaphores
        // and the present.
        for (int j = 0; j < 16; j++) {
            lock();
            int hit = g_plain[j].sc == pi->pSwapchains[0] && !g_plain[j].probed;
            if (hit)
                g_plain[j].probed = 1;
            unlock();
            if (!hit)
                continue;
            VkImage imgs[8];
            uint32_t n = 8;
            VkSemaphore sem = 0;
            VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
            if (lxrt_mvk_vkGetSwapchainImagesKHR(g_plain[j].dev, g_plain[j].sc, &n, imgs) >= 0 &&
                pi->pImageIndices[0] < n && lxrt_mvk_vkCreateSemaphore(g_plain[j].dev, &sci, 0, &sem) == VK_SUCCESS) {
                probe_image(g_plain[j].dev, q, queue_family(q), imgs[pi->pImageIndices[0]], g_plain[j].ext,
                            g_plain[j].fmt, pi->pWaitSemaphores, pi->waitSemaphoreCount, sem, "plain");
                VkPresentInfoKHR p = *pi;
                p.waitSemaphoreCount = 1;
                p.pWaitSemaphores = &sem;
                VkResult r = lxrt_mvk_vkQueuePresentKHR(q, &p);
                lxrt_mvk_vkQueueWaitIdle(q);
                lxrt_mvk_vkDestroySemaphore(g_plain[j].dev, sem, 0);
                return r;
            }
            break;
        }
    }
    if (!k)
        return lxrt_mvk_vkQueuePresentKHR(q, pi);

    // Each scaled swapchain's pass waits for what the game's present waited
    // for (the first one does; the others follow it on the same queue), and
    // the present then waits for the passes.
    VkSemaphore waits[16];
    uint32_t nwait = 0;
    VkPipelineStageFlags stages[16];
    for (uint32_t j = 0; j < 16; j++)
        stages[j] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    const VkSemaphore *game = pi->pWaitSemaphores;
    uint32_t ngame = pi->waitSemaphoreCount;
    uint32_t family = queue_family(q);
    VkResult r = VK_SUCCESS;
    for (uint32_t j = 0; j < nsc; j++) {
        scaled *s = ss[j];
        if (!s)
            continue;
        uint32_t i = pi->pImageIndices[j];
        if (i >= s->n || (r = setup_submit(s, family)))
            break;
        if (s->pending[i]) {
            lxrt_mvk_vkWaitForFences(s->dev, 1, &s->fence[i], VK_TRUE, UINT64_MAX);
            lxrt_mvk_vkResetFences(s->dev, 1, &s->fence[i]);
            s->pending[i] = 0;
        }
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = ngame < 16 ? ngame : 16,
            .pWaitSemaphores = game,
            .pWaitDstStageMask = stages,
            .commandBufferCount = 1,
            .pCommandBuffers = &s->cb[i],
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &s->done[i],
        };
        if (s->filter == F_METALFX && s->mfx_broken) {
            si.pCommandBuffers = &s->cbf[i];
        } else if (s->filter == F_METALFX) {
            if ((r = present_mfx(s, q, i, &si)))
                break;
            goto submitted;
        }
        if ((r = lxrt_mvk_vkQueueSubmit(q, 1, &si, s->fence[i])))
            break;
    submitted:
        s->pending[i] = 1;
        if (!s->probed && probe_wanted()) {
            s->probed = 1;
            probe_image(s->dev, q, family, s->real[i], s->out, s->format, 0, 0, 0, filter_name(s->filter));
        }
        waits[nwait++] = s->done[i];
        game = 0;
        ngame = 0;
    }
    if (r)
        return r;
    VkPresentInfoKHR p = *pi;
    // Semaphores the game's present waited for and no pass consumed (none:
    // the first pass took them all) stay out; the passes' take their place.
    p.waitSemaphoreCount = nwait;
    p.pWaitSemaphores = waits;
    return lxrt_mvk_vkQueuePresentKHR(q, &p);
}
