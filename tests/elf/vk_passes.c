// Render passes that follow one another on the same attachments, as a
// native program on the Vulkan shim: what patches/kosmickrisp-04-tile-gpu-
// passes.patch turns into one Metal encoder must still draw what separate
// passes draw. Source 2 ends and begins a pass between groups of draws on the
// same targets about 200 times a frame (benchmarks/stage51).
//
// One 64x64 colour target T (with a depth buffer whose contents nobody keeps)
// and a second target U:
//   A  T, CLEAR black                  red   into the left half
//   B  T, LOAD (continues A)           green into the right half
//   C  T, CLEAR blue, area 16x16+0+0   nothing drawn (a clear inside a pass)
//   D  U, CLEAR white                  (another target: ends the pass on T)
//   E  T, LOAD                         yellow into 16x16+40+40
//   then T and U are copied out (a copy after a pass ends it too).
// Pixels: (8,8) blue, (24,8) red, (8,24) red, (40,8) green, (44,44) yellow,
// (56,56) green; U white.
// Prints "== vk_passes: ok" or the first pixel that is wrong.
// tests/elf/run_vk_arm64.sh builds and runs it on each driver.
//
// The shaders (vk_passes_spv.h, glslangValidator -V):
//   vert: vec2 p = vec2((gl_VertexIndex<<1)&2, gl_VertexIndex&2);
//         gl_Position = vec4(p*2.0-1.0, 0.5, 1.0);
//   frag: layout(push_constant) uniform PC { vec4 color; } pc;
//         layout(location=0) out vec4 o;  void main(){ o = pc.color; }
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "vk_passes_spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s -> %d\n== vk_passes: FAIL\n", #x, r_); return 1; } } while (0)
enum { W = 64, H = 64 };

static VkDevice dev;
static VkPhysicalDevice pd;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

static int make_image(VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImage *img, VkImageView *view)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, fmt, { W, H, 1 }, 1, 1,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage };
    CHECK(vkCreateImage(dev, &ici, 0, img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mem_type(mr.memoryTypeBits, 0) };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, 0, &mem));
    CHECK(vkBindImageMemory(dev, *img, mem, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, *img, VK_IMAGE_VIEW_TYPE_2D, fmt, { 0 },
                                  { aspect, 0, 1, 0, 1 } };
    CHECK(vkCreateImageView(dev, &vci, 0, view));
    return 0;
}

static void transition(VkCommandBuffer cb, VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, from, to,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, { aspect, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}

// One pass on `color` (and `depth`, if any): begin, draw `rgba` into `draw`
// if it has a width, end.
static void pass(VkCommandBuffer cb, VkPipeline pipe, VkPipelineLayout pl, VkImageView color, VkImageView depth,
                 VkAttachmentLoadOp load, const float clear[4], VkRect2D area, VkRect2D draw, const float rgba[4])
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, color,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0, 0, load, VK_ATTACHMENT_STORE_OP_STORE };
    if (clear)
        memcpy(ca.clearValue.color.float32, clear, sizeof ca.clearValue.color.float32);
    // The depth buffer is loaded as it is and thrown away: a store operation
    // of DONT_CARE the driver may now honour.
    VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, depth,
                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, 0, 0,
                                     VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, area, 1, 0, 1, &ca, depth ? &da : 0, 0 };
    vkCmdBeginRendering(cb, &ri);
    if (draw.extent.width) {
        VkViewport vp = { 0, 0, W, H, 0, 1 };
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &draw);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, rgba);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cb);
}

static int pixel(const unsigned char *p, int x, int y, unsigned r, unsigned g, unsigned b, const char *what)
{
    const unsigned char *q = p + 4 * (y * W + x);
    if (q[0] == r && q[1] == g && q[2] == b)
        return 0;
    printf("(%d,%d) is %u,%u,%u, wanted %u,%u,%u: %s\n== vk_passes: FAIL\n", x, y, q[0], q[1], q[2], r, g, b, what);
    return 1;
}

int main(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_passes", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { printf("no device\n== vk_passes: FAIL\n"); return 1; }
    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1, &q };
    CHECK(vkCreateDevice(pd, &dci, 0, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    VkImage t, u, d;
    VkImageView tv, uv, dv;
    VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (make_image(VK_FORMAT_R8G8B8A8_UNORM, cu, VK_IMAGE_ASPECT_COLOR_BIT, &t, &tv) ||
        make_image(VK_FORMAT_R8G8B8A8_UNORM, cu, VK_IMAGE_ASPECT_COLOR_BIT, &u, &uv) ||
        make_image(VK_FORMAT_D32_SFLOAT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT, &d, &dv))
        return 1;

    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, 2 * W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, 0, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
                                 mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory bmem;
    CHECK(vkAllocateMemory(dev, &mai, 0, &bmem));
    CHECK(vkBindBufferMemory(dev, buf, bmem, 0));

    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 0, 0, 1, &pr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
    VkShaderModuleCreateInfo vi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof vkp_vert, vkp_vert };
    VkShaderModuleCreateInfo fi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof vkp_frag, vkp_frag };
    VkShaderModule vs, fs;
    CHECK(vkCreateShaderModule(dev, &vi, 0, &vs));
    CHECK(vkCreateShaderModule(dev, &fi, 0, &fs));
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
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0, VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo dss = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dyn };
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    // Two pipelines: with the depth buffer (T) and without (U has none).
    VkPipeline pipe[2];
    for (int k = 0; k < 2; k++) {
        VkPipelineRenderingCreateInfo prc = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 1, &cf,
                                              k ? VK_FORMAT_UNDEFINED : VK_FORMAT_D32_SFLOAT };
        VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prc, 0, 2, st, &vin, &ia, 0, &vps,
                                            &rs, &ms, &dss, &cbs, &dsi, pl };
        CHECK(vkCreateGraphicsPipelines(dev, 0, 1, &gi, 0, &pipe[k]));
    }

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpi, 0, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(vkBeginCommandBuffer(cb, &cbi));
    transition(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cb, u, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    transition(cb, d, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    const float black[4] = { 0, 0, 0, 1 }, blue[4] = { 0, 0, 1, 1 }, white[4] = { 1, 1, 1, 1 };
    const float red[4] = { 1, 0, 0, 1 }, green[4] = { 0, 1, 0, 1 }, yellow[4] = { 1, 1, 0, 1 };
    const VkRect2D whole = { { 0, 0 }, { W, H } }, none = { { 0, 0 }, { 0, 0 } };
    pass(cb, pipe[0], pl, tv, dv, VK_ATTACHMENT_LOAD_OP_CLEAR, black, whole, (VkRect2D){ { 0, 0 }, { 32, H } }, red);       // A
    pass(cb, pipe[0], pl, tv, dv, VK_ATTACHMENT_LOAD_OP_LOAD, 0, whole, (VkRect2D){ { 32, 0 }, { 32, H } }, green);        // B
    pass(cb, pipe[0], pl, tv, dv, VK_ATTACHMENT_LOAD_OP_CLEAR, blue, (VkRect2D){ { 0, 0 }, { 16, 16 } }, none, 0);         // C
    pass(cb, pipe[1], pl, uv, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, white, whole, none, 0);                                      // D
    pass(cb, pipe[0], pl, tv, dv, VK_ATTACHMENT_LOAD_OP_LOAD, 0, whole, (VkRect2D){ { 40, 40 }, { 16, 16 } }, yellow);     // E

    transition(cb, t, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    transition(cb, u, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c0 = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    VkBufferImageCopy c1 = c0;
    c1.bufferOffset = W * H * 4;
    vkCmdCopyImageToBuffer(cb, t, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c0);
    vkCmdCopyImageToBuffer(cb, u, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &c1);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
    CHECK(vkQueueSubmit(queue, 1, &si, 0));
    CHECK(vkQueueWaitIdle(queue));

    void *map;
    CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &map));
    const unsigned char *pt = map, *pu = pt + W * H * 4;
    if (pixel(pt, 8, 8, 0, 0, 255, "C's clear inside the continued pass") ||
        pixel(pt, 24, 8, 255, 0, 0, "A's draw outside C's area") ||
        pixel(pt, 8, 24, 255, 0, 0, "A's draw below C's area") ||
        pixel(pt, 40, 8, 0, 255, 0, "B's draw, continuing A") ||
        pixel(pt, 44, 44, 255, 255, 0, "E's draw, back on T after another target") ||
        pixel(pt, 56, 56, 0, 255, 0, "B's draw outside E's") ||
        pixel(pu, 32, 32, 255, 255, 255, "D's clear of the other target"))
        return 1;
    printf("== vk_passes: ok\n");
    return 0;
}
