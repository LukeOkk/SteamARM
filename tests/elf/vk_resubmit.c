// The same command buffer submitted again and again, as a native program on
// the Vulkan shim: what a game does with command buffers it records once.
// KosmicKrisp records a command buffer into Metal at its first submission and
// records it again at each later one (rerecord_cmd_buffer); since
// patches/kosmickrisp-12 the memory a recording uploads into (the root
// descriptor table of every draw) goes back to the device's pool when the GPU
// has finished, and another command buffer takes it. Here command buffer A
// (no ONE_TIME_SUBMIT): 4 render passes of 16 draws each, every draw with its
// own dynamic offset into a uniform buffer and its own push constant, each
// into its own 8x8 cell of a 64x64 target, then a copy out. Between A's
// submissions, command buffer B (64 passes of 32 draws on another target)
// churns the pool. A must draw the same 64 cells at its 1st, 2nd and 3rd
// submission. Also secondary command buffers (vkCmdExecuteCommands) carrying
// the draws of A's last pass.
// Prints "== vk_resubmit: ok" or the first cell that is wrong.
// tests/elf/run_vk_arm64.sh builds and runs it on each driver.
//
// The shaders (vk_resubmit_spv.h, glslangValidator -V):
//   full.vert: vec2 p = vec2((gl_VertexIndex<<1)&2, gl_VertexIndex&2);
//              gl_Position = vec4(p*2.0-1.0, 0.5, 1.0);
//   ubo.frag:  layout(set=0, binding=0) uniform U { vec4 color; } u;
//              layout(push_constant) uniform PC { vec4 tint; } pc;
//              o = u.color * pc.tint;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "vk_resubmit_spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s -> %d\n== vk_resubmit: FAIL\n", #x, r_); exit(1); } } while (0)
enum { W = 64, H = 64, CELL = 8, NCELL = (W / CELL) * (H / CELL), BW = 256, BH = 256 };

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

static void make_image(uint32_t w, uint32_t h, VkImage *img, VkImageView *view)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                              { w, h, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    CHECK(vkCreateImage(dev, &ici, 0, img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mem_type(mr.memoryTypeBits, 0) };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, 0, &mem));
    CHECK(vkBindImageMemory(dev, *img, mem, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, *img, VK_IMAGE_VIEW_TYPE_2D,
                                  VK_FORMAT_R8G8B8A8_UNORM, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
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

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT, from, to,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}

static VkPipeline pipe;
static VkPipelineLayout pl;
static VkDescriptorSet set;
static uint32_t ubo_stride;

// The colour of cell i (cells of A) or of churn draw i, RGBA8.
static void cell_color(int i, float c[4])
{
    c[0] = ((i * 37) % 251) / 255.0f;
    c[1] = ((i * 91 + 17) % 241) / 255.0f;
    c[2] = ((i * 53 + 101) % 239) / 255.0f;
    c[3] = 1.0f;
}

// Draws cells [first, first + n) of a W-wide grid; each draw a dynamic offset
// to its own colour in the uniform buffer and a tint push constant of 1.
static void draw_cells(VkCommandBuffer cb, int first, int n, uint32_t tw, uint32_t th, int ubo_base)
{
    VkViewport vp = { 0, 0, (float)tw, (float)th, 0, 1 };
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdSetViewport(cb, 0, 1, &vp);
    const float tint[4] = { 1, 1, 1, 1 };
    for (int k = 0; k < n; k++) {
        int i = first + k;
        uint32_t off = (uint32_t)((ubo_base + i) % 1024) * ubo_stride;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 1, &off);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, tint);
        uint32_t cols = tw / CELL;
        VkRect2D sc = { { (int32_t)((i % cols) * CELL), (int32_t)(((i / cols) % (th / CELL)) * CELL) }, { CELL, CELL } };
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
}

static void begin_pass(VkCommandBuffer cb, VkImageView v, uint32_t w, uint32_t h, int clear, VkRenderingFlags flags)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, v, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                                     0, 0, 0, clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                     VK_ATTACHMENT_STORE_OP_STORE };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, flags, { { 0, 0 }, { w, h } }, 1, 0, 1, &ca };
    vkCmdBeginRendering(cb, &ri);
}

int main(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_resubmit", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { printf("no device\n== vk_resubmit: FAIL\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    ubo_stride = (uint32_t)props.limits.minUniformBufferOffsetAlignment;
    if (ubo_stride < 16) ubo_stride = 16;
    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1, &q };
    CHECK(vkCreateDevice(pd, &dci, 0, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    VkImage ta, tb;
    VkImageView tav, tbv;
    make_image(W, H, &ta, &tav);
    make_image(BW, BH, &tb, &tbv);
    VkBuffer out, ubo;
    void *outmap, *ubomap;
    make_buffer(W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out, &outmap);
    make_buffer(1024 * (VkDeviceSize)ubo_stride, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubo, &ubomap);
    for (int i = 0; i < 1024; i++)
        cell_color(i, (float *)((char *)ubomap + (size_t)i * ubo_stride));

    VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &lb };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &lci, 0, &dsl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 1, &ps };
    VkDescriptorPool dpool;
    CHECK(vkCreateDescriptorPool(dev, &dpi, 0, &dpool));
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dpool, 1, &dsl };
    CHECK(vkAllocateDescriptorSets(dev, &dai, &set));
    VkDescriptorBufferInfo dbi = { ubo, 0, 16 };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 0, &dbi };
    vkUpdateDescriptorSets(dev, 1, &wr, 0, 0);
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 1, &pr };
    CHECK(vkCreatePipelineLayout(dev, &pli, 0, &pl));

    VkShaderModuleCreateInfo vi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof vrs_vert, vrs_vert };
    VkShaderModuleCreateInfo fi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof vrs_frag, vrs_frag };
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
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dyn };
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo prc = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 1, &cf };
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prc, 0, 2, st, &vin, &ia, 0, &vps,
                                        &rs, &ms, 0, &cbs, &dsi, pl };
    CHECK(vkCreateGraphicsPipelines(dev, 0, 1, &gi, 0, &pipe));

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpi, 0, &pool));
    VkCommandBuffer cbs_[2], sec;
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2 };
    CHECK(vkAllocateCommandBuffers(dev, &cai, cbs_));
    VkCommandBufferAllocateInfo sai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_SECONDARY, 1 };
    CHECK(vkAllocateCommandBuffers(dev, &sai, &sec));
    VkCommandBuffer a = cbs_[0], b = cbs_[1];

    // The secondary: the 16 draws of A's last pass, inside a render pass.
    VkCommandBufferInheritanceRenderingInfo inr = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO, 0, 0, 0, 1, &cf,
                                                    VK_FORMAT_UNDEFINED, VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT };
    VkCommandBufferInheritanceInfo inh = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO, &inr };
    VkCommandBufferBeginInfo sbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0,
                                     VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT | VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT, &inh };
    CHECK(vkBeginCommandBuffer(sec, &sbi));
    draw_cells(sec, 48, 16, W, H, 0);
    CHECK(vkEndCommandBuffer(sec));

    // A: recorded once, no ONE_TIME_SUBMIT.
    VkCommandBufferBeginInfo abi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(a, &abi));
    barrier(a, ta, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    for (int p = 0; p < 3; p++) {
        begin_pass(a, tav, W, H, p == 0, 0);
        draw_cells(a, p * 16, 16, W, H, 0);
        vkCmdEndRendering(a);
    }
    begin_pass(a, tav, W, H, 0, VK_RENDERING_CONTENTS_SECONDARY_COMMAND_BUFFERS_BIT);
    vkCmdExecuteCommands(a, 1, &sec);
    vkCmdEndRendering(a);
    barrier(a, ta, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c0 = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    vkCmdCopyImageToBuffer(a, ta, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1, &c0);
    CHECK(vkEndCommandBuffer(a));

    // B: the churn, re-recorded before each of its submissions.
    VkSubmitInfo sa = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &a };
    VkSubmitInfo sb = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &b };
    int bad = 0;
    for (int round = 0; round < 3 && !bad; round++) {
        memset(outmap, 0, W * H * 4);
        CHECK(vkQueueSubmit(queue, 1, &sa, 0));
        CHECK(vkQueueWaitIdle(queue));
        const unsigned char *px = outmap;
        for (int i = 0; i < NCELL && !bad; i++) {
            int cx = (i % (W / CELL)) * CELL + CELL / 2, cy = (i / (W / CELL)) * CELL + CELL / 2;
            const unsigned char *pp = px + 4 * (cy * W + cx);
            float c[4];
            cell_color(i, c);
            for (int k = 0; k < 3; k++) {
                int want = (int)(c[k] * 255.0f + 0.5f);
                if (abs((int)pp[k] - want) > 1) {
                    printf("submission %d, cell %d (%d,%d): %u,%u,%u, wanted %d,%d,%d\n== vk_resubmit: FAIL\n", round + 1, i,
                           cx, cy, pp[0], pp[1], pp[2], (int)(c[0] * 255 + .5f), (int)(c[1] * 255 + .5f), (int)(c[2] * 255 + .5f));
                    bad = 1;
                    break;
                }
            }
        }
        if (bad)
            break;
        printf("submission %d of A: %d cells right\n", round + 1, NCELL);
        for (int churn = 0; churn < 3; churn++) {
            CHECK(vkResetCommandBuffer(b, 0));
            VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            CHECK(vkBeginCommandBuffer(b, &bbi));
            barrier(b, tb, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            for (int p = 0; p < 64; p++) {
                begin_pass(b, tbv, BW, BH, p == 0, 0);
                draw_cells(b, p * 32, 32, BW, BH, 500 + churn * 7);
                vkCmdEndRendering(b);
            }
            CHECK(vkEndCommandBuffer(b));
            CHECK(vkQueueSubmit(queue, 1, &sb, 0));
        }
        CHECK(vkQueueWaitIdle(queue));
    }
    if (bad)
        return 1;
    printf("== vk_resubmit: ok\n");
    return 0;
}
