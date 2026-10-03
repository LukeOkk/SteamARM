// A game's FSR 1 upscale (EASU) replaced by MetalFX, as a native program on
// the Vulkan shim. The fragment shader here carries the two constants of
// EASU's approximations (0x7ef07ebb, 0x5f347d74) and samples set 1 binding
// 30 with the sampler at binding 14, as Counter-Strike 2's EASU does, but
// paints magenta. A 32x32 input (blue left half, red right half) is "upscaled"
// into a 64x64 colour attachment cleared green. SteamARM's KosmicKrisp
// (patches/kosmickrisp-14) draws that pass with MetalFX's spatial scaler: the
// result is the input enlarged (blue left, red right); any other driver, or
// KK_FSR_METALFX=0, runs the shader: magenta.
// Usage: vk_fsr metalfx|shader (the result expected)
// Prints "== vk_fsr: ok" or "== vk_fsr: FAIL".
//
// The shaders (vk_fsr_spv.h, glslangValidator -V --target-env vulkan1.3):
//   full.vert: full-screen triangle from gl_VertexIndex at z 0.5
//   easu.frag: uint a = 0x7ef07ebbu - c.con0.x, b = 0x5f347d74u - (c.con0.y >> 1);
//              o = a == b ? texture(sampler2D(t, s), gl_FragCoord.xy / 64) : magenta
//              (c: uniform block at set 1 binding 1, zero here; s: binding 14; t: binding 30)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "vk_fsr_spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s -> %d\n== vk_fsr: FAIL\n", #x, r_); exit(1); } } while (0)
enum { IW = 32, IH = 32, OW = 64, OH = 64 };

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

static void make_image(uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImage *img, VkImageView *view)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                              { w, h, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage };
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
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                               { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}

static VkShaderModule module(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo i = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, size, code };
    VkShaderModule m;
    CHECK(vkCreateShaderModule(dev, &i, 0, &m));
    return m;
}

int main(int argc, char **argv)
{
    int want_metalfx = argc > 1 && !strcmp(argv[1], "metalfx");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_fsr", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { printf("no device\n== vk_fsr: FAIL\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);
    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1, &q };
    CHECK(vkCreateDevice(pd, &dci, 0, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    // Set 1 as Counter-Strike 2's EASU has it; set 0 is empty.
    VkDescriptorSetLayoutBinding b[3] = {
        { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        { 14, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
        { 30, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    VkDescriptorSetLayoutCreateInfo lci0 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkDescriptorSetLayoutCreateInfo lci1 = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 3, b };
    VkDescriptorSetLayout sl[2];
    CHECK(vkCreateDescriptorSetLayout(dev, &lci0, 0, &sl[0]));
    CHECK(vkCreateDescriptorSetLayout(dev, &lci1, 0, &sl[1]));
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 2, sl };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, 0, &pl));

    VkImage in, out;
    VkImageView in_view, out_view;
    make_image(IW, IH, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &in, &in_view);
    make_image(OW, OH, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
               &out, &out_view);
    VkBuffer up, down, ubo;
    void *up_map, *down_map, *ubo_map;
    make_buffer(IW * IH * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up, &up_map);
    make_buffer(OW * OH * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &down, &down_map);
    make_buffer(256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubo, &ubo_map);
    memset(ubo_map, 0, 256);
    unsigned char *px = up_map;
    for (int y = 0; y < IH; y++)
        for (int x = 0; x < IW; x++) {
            unsigned char *p = px + 4 * (y * IW + x);
            p[0] = x < IW / 2 ? 0 : 255;
            p[1] = 0;
            p[2] = x < IW / 2 ? 255 : 0;
            p[3] = 255;
        }

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, 0, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR };
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler;
    CHECK(vkCreateSampler(dev, &sci, 0, &sampler));

    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 1 },
                                   { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 } };
    VkDescriptorPoolCreateInfo dpci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 2, 3, ps };
    VkDescriptorPool dp;
    CHECK(vkCreateDescriptorPool(dev, &dpci, 0, &dp));
    VkDescriptorSetAllocateInfo dsai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dp, 2, sl };
    VkDescriptorSet sets[2];
    CHECK(vkAllocateDescriptorSets(dev, &dsai, sets));
    VkDescriptorBufferInfo bi = { ubo, 0, 16 };
    VkDescriptorImageInfo si = { sampler }, ii = { 0, in_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w[3] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, sets[1], 1, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &bi },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, sets[1], 14, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &si },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, sets[1], 30, 0, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii },
    };
    vkUpdateDescriptorSets(dev, 3, w, 0, 0);

    VkShaderModule vs = module(vfs_full, sizeof vfs_full), fs = module(vfs_easu, sizeof vfs_easu);
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
                                                VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { 0 };
    ba.colorWriteMask = 0xf;
    VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dyn };
    VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo prc = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 1, &cf };
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prc, 0, 2, st, &vin, &ia, 0, &vps,
                                        &rs, &ms, 0, &cbs, &dsi, pl };
    VkPipeline pipe;
    CHECK(vkCreateGraphicsPipelines(dev, 0, 1, &gi, 0, &pipe));

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandPool cp;
    CHECK(vkCreateCommandPool(dev, &cpci, 0, &cp));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, cp,
                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cbai, &cb));
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    CHECK(vkBeginCommandBuffer(cb, &cbbi));
    barrier(cb, in, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy up_copy = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0 }, { IW, IH, 1 } };
    vkCmdCopyBufferToImage(cb, up, in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up_copy);
    barrier(cb, in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(cb, out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, out_view,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color.float32[1] = 1;
    ca.clearValue.color.float32[3] = 1;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, { { 0, 0 }, { OW, OH } }, 1, 0, 1, &ca };
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = { 0, 0, OW, OH, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { OW, OH } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 2, sets, 0, 0);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRendering(cb);
    barrier(cb, out, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy down_copy = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0 }, { OW, OH, 1 } };
    vkCmdCopyImageToBuffer(cb, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, down, 1, &down_copy);
    CHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si2 = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };
    CHECK(vkQueueSubmit(queue, 1, &si2, 0));
    CHECK(vkQueueWaitIdle(queue));

    // Away from the middle and the edges, where a filter blends.
    const unsigned char *o = down_map;
    const unsigned char *l = o + 4 * (OH / 2 * OW + 8), *r = o + 4 * (OH / 2 * OW + OW - 9);
    printf("left %u,%u,%u right %u,%u,%u\n", l[0], l[1], l[2], r[0], r[1], r[2]);
    int scaled = l[2] > 200 && l[0] < 60 && l[1] < 60 && r[0] > 200 && r[2] < 60 && r[1] < 60;
    int magenta = l[0] > 200 && l[2] > 200 && r[0] > 200 && r[2] > 200 && l[1] < 60 && r[1] < 60;
    printf("result: %s\n", scaled ? "metalfx (the input enlarged)" : magenta ? "shader (magenta)" : "neither");
    int ok = want_metalfx ? scaled : magenta;
    printf("== vk_fsr: %s\n", ok ? "ok" : "FAIL");
    return !ok;
}
