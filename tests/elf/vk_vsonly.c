// A depth prepass made of a pipeline with no fragment shader, through a
// VkPipelineCache, as a native program on the Vulkan shim. KosmicKrisp keeps
// the Metal render pipeline (formats, samples, depth/stencil state) in the
// vertex shader object and hashed only the vertex state for a pipeline with
// no fragment shader, so two such pipelines that differ in their depth state
// shared one: here X (depth NEVER, no writes) is made first and Y (LESS,
// writes) was handed X's, the prepass wrote no depth and the colour pass with
// depth EQUAL drew nothing (patches/kosmickrisp-09). Shadow and depth
// prepasses of Source 2 are such pipelines.
// Usage: vk_vsonly [cache]. Prints "== vk_vsonly: ok" or "== vk_vsonly: FAIL".
// Shaders: vk_alphamask_spv.h (tests/elf/vk_alphamask.c).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "vk_alphamask_spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s -> %d\n== vk_alphamask: FAIL\n", #x, r_); exit(1); } } while (0)
enum { W = 64, H = 64 };

static VkDevice dev;
static VkPhysicalDevice pd;
static VkPipelineLayout pl;
static VkPipelineCache pcache; // with "cache" as the second argument, every pipeline goes through one

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

static void make_image(uint32_t w, uint32_t h, VkFormat fmt, VkSampleCountFlagBits s, VkImageUsageFlags usage,
                       VkImageAspectFlags aspect, VkImage *img, VkImageView *view)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, fmt,
                              { w, h, 1 }, 1, 1, s, VK_IMAGE_TILING_OPTIMAL, usage };
    CHECK(vkCreateImage(dev, &ici, 0, img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mem_type(mr.memoryTypeBits, 0) };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, 0, &mem));
    CHECK(vkBindImageMemory(dev, *img, mem, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, *img, VK_IMAGE_VIEW_TYPE_2D,
                                  fmt, { 0 }, { aspect, 0, 1, 0, 1 } };
    CHECK(vkCreateImageView(dev, &vci, 0, view));
}

static void make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, void **map)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, usage };
    CHECK(vkCreateBuffer(dev, &bci, 0, buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, *buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
                                 mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, 0, &mem));
    CHECK(vkBindBufferMemory(dev, *buf, mem, 0));
    CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, map));
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                               from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                               { aspect, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}

static VkShaderModule module(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo i = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, size, code };
    VkShaderModule m;
    CHECK(vkCreateShaderModule(dev, &i, 0, &m));
    return m;
}

struct pipe_desc {
    const uint32_t *fs; size_t fsn;
    int a2c;
    const VkSampleMask *mask;
    VkColorComponentFlags write;
    int depth_test, depth_write; VkCompareOp op;
    int blend;        // colour replaced, alpha one / one-minus-src-alpha: the blended alpha is not the source alpha
    int no_color;     // a depth-only pipeline (no colour attachment)
};
static VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM; static int vs_only_g;

static VkPipeline pipeline(const struct pipe_desc *d)
{
    VkShaderModule vs = module(vam_full, sizeof vam_full), fs = module(d->fs, d->fsn);
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" },
    };
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0,
                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, 0, 1, 0 };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0,
                                                VK_SAMPLE_COUNT_4_BIT, 0, 0, d->mask, d->a2c, 0 };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, 0, 0,
                                                 d->depth_test, d->depth_write, d->op };
    VkPipelineColorBlendAttachmentState ba = { d->blend, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO,
                                               VK_BLEND_OP_ADD, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
                                               VK_BLEND_OP_ADD, d->write };
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0,
                                                d->no_color ? 0 : 1, &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dyn };
    VkFormat cf = color_format;
    VkPipelineRenderingCreateInfo prc = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, d->no_color ? 0 : 1, &cf,
                                          VK_FORMAT_D32_SFLOAT };
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prc, 0, vs_only_g ? 1 : 2, st, &vin, &ia, 0, &vps,
                                        &rs, &ms, &ds, &cbs, &dsi, pl };
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(dev, pcache, 1, &gi, 0, &p));
    return p;
}

static float half(unsigned h)
{
    unsigned e = (h >> 10) & 31, m = h & 1023;
    float v = e ? (1.0f + m / 1024.0f) * (float)(1u << e) / 32768.0f : m / 16777216.0f;
    return h & 0x8000 ? -v : v;
}

static float uf(unsigned v, int mbits) // unsigned 11- or 10-bit float
{
    unsigned e = v >> mbits, m = v & ((1u << mbits) - 1);
    return e ? (1.0f + (float)m / (1u << mbits)) * (float)(1u << e) / 32768.0f : (float)m / (1u << mbits) / 16384.0f;
}

// RGB of texel (x, y) of a W x H image of formats[f] read back into `map`.
static void texel(const void *map, int f, int x, int y, float rgb[3])
{
    if (f == 0) {
        const unsigned char *p = (const unsigned char *)map + 4 * (y * W + x);
        for (int i = 0; i < 3; i++)
            rgb[i] = p[i] / 255.0f;
    } else if (f == 1) {
        unsigned v = ((const unsigned *)map)[y * W + x];
        rgb[0] = uf(v & 0x7ff, 6);
        rgb[1] = uf((v >> 11) & 0x7ff, 6);
        rgb[2] = uf(v >> 22, 5);
    } else {
        const unsigned short *p = (const unsigned short *)map + 4 * (y * W + x);
        for (int i = 0; i < 3; i++)
            rgb[i] = half(p[i]);
    }
}

int main(int argc, char **argv)
{
    int use_cache = argc > 1 && !strcmp(argv[1], "cache");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_vsonly", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst; CHECK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1; vkEnumeratePhysicalDevices(inst, &n, &pd);
    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1, &q };
    CHECK(vkCreateDevice(pd, &dci, 0, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);
    if (use_cache) { VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO }; CHECK(vkCreatePipelineCache(dev, &pci, 0, &pcache)); }
    VkImage ms_c, res, ms_d; VkImageView ms_cv, resv, ms_dv;
    make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &ms_c, &ms_cv);
    make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &res, &resv);
    make_image(W, H, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &ms_d, &ms_dv);
    VkBuffer out; void *outmap; make_buffer(W * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out, &outmap);
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    CHECK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkCommandPool pool; CHECK(vkCreateCommandPool(dev, &cpi, 0, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb; CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
    /* X: depth-only, no fragment shader, depth test NEVER without writes (made first)
     * Y: the same but LESS with writes: the depth prepass
     * Z: colour, opaque red, EQUAL */
    struct pipe_desc dx = { vam_coord, sizeof vam_coord, 0, 0, 0xf, 1, 0, VK_COMPARE_OP_NEVER, 0, 1 };
    struct pipe_desc dy = { vam_coord, sizeof vam_coord, 0, 0, 0xf, 1, 1, VK_COMPARE_OP_LESS, 0, 1 };
    struct pipe_desc dz = { vam_coord, sizeof vam_coord, 0, 0, 0xf, 1, 0, VK_COMPARE_OP_EQUAL, 0, 0 };
    vs_only_g = 1;
    VkPipeline px = pipeline(&dx), py = pipeline(&dy);
    vs_only_g = 0;
    VkPipeline pz = pipeline(&dz);
    (void)px;
    CHECK(vkBeginCommandBuffer(cb, &cbi));
    barrier(cb, ms_c, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(cb, res, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    barrier(cb, ms_d, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, ms_cv, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_AVERAGE_BIT, resv, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE };
    ca.clearValue.color.float32[2] = 1; ca.clearValue.color.float32[3] = 1;
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, ms_dv, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 0, 0, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE };
    da.clearValue.depthStencil.depth = 1;
    VkRect2D area = { { 0, 0 }, { W, H } }; VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRenderingInfo r0 = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, area, 1, 0, 0, 0, &da };
    vkCmdBeginRendering(cb, &r0); vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, py); vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 1, &mb, 0, 0, 0, 0);
    da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, area, 1, 0, 1, &ca, &da };
    vkCmdBeginRendering(cb, &ri); vkCmdSetViewport(cb, 0, 1, &vp); vkCmdSetScissor(cb, 0, 1, &area);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pz); vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(cb, res, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c0 = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyImageToBuffer(cb, res, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1, &c0);
    CHECK(vkEndCommandBuffer(cb)); CHECK(vkQueueSubmit(queue, 1, &si, 0)); CHECK(vkQueueWaitIdle(queue));
    float c[3]; texel(outmap, 0, 32, 32, c);
    int ok = c[0] > 0.97f && c[2] < 0.03f;
    printf("%s vs-only prepass (cache=%d): %.2f,%.2f,%.2f\n== vk_vsonly: %s\n", ok ? "ok" : "FAIL", use_cache, c[0], c[1], c[2], ok ? "ok" : "FAIL");
    return !ok;
}
