// A vertex shader that reads what the render pass before it produced, as a
// native program on the Vulkan shim. A tile-based GPU runs the vertex stage
// of a pass while the fragments of the pass before are still being shaded,
// unless the driver makes it wait; patches/kosmickrisp-05-vertex-barrier.patch
// lets it run early except in exactly these two cases:
//   1  pass A draws a colour into T (2048x2048, a fragment shader that takes
//      long); pass B's vertex shader fetches a texel of T and hands it to the
//      fragment shader, which fills U with it;
//   2  pass A's fragment shader also stores the colour in a storage buffer;
//      pass B's vertex shader reads the buffer and U2 is filled with it.
// Several rounds with a different colour each: a vertex stage that did not
// wait reads the colour of the round before.
// Prints "== vk_vtxread: ok" or the first pixel that is wrong.
// tests/elf/run_vk_arm64.sh builds and runs it on each driver.
//
// The shaders (vk_vtxread_spv.h, glslangValidator -V):
//   full.vert:  vec2 p = vec2((gl_VertexIndex<<1)&2, gl_VertexIndex&2);
//               gl_Position = vec4(p*2.0-1.0, 0.5, 1.0);
//   heavy.frag: layout(push_constant) uniform PC { vec4 color; int iters; } pc;
//               float a = gl_FragCoord.x*0.001 + gl_FragCoord.y*0.002;
//               for (int i = 0; i < pc.iters; i++) a = fract(sin(a+float(i))*43758.5453);
//               o = pc.color + vec4(a*1e-6);
//   store.frag: heavy.frag, and
//               layout(set=0, binding=1) buffer S { vec4 v; } s;  s.v = pc.color;
//   rtex.vert:  full.vert, and layout(set=0, binding=0) uniform sampler2D t;
//               layout(location=0) flat out vec4 c;
//               c = texelFetch(t, ivec2(textureSize(t,0)) - ivec2(3,3), 0);
//   rbuf.vert:  full.vert, and layout(set=0, binding=1) readonly buffer S { vec4 v; } s;
//               c = s.v;
//   flat.frag:  layout(location=0) flat in vec4 c;  o = c;
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "vk_vtxread_spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s -> %d\n== vk_vtxread: FAIL\n", #x, r_); return 1; } } while (0)
enum { TW = 2048, TH = 2048, W = 64, H = 64, ROUNDS = 6 };

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

static int make_image(uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImage *img, VkImageView *view)
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
    return 0;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, void **map)
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
    return 0;
}

static void transition(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               from, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                               { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}

struct pc { float color[4]; int iters; };

// One pass on `color` of w x h: clear it to `clear` if given (else load),
// one triangle over all of it with `pipe`.
static void pass(VkCommandBuffer cb, VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSet set, VkImageView color,
                 uint32_t w, uint32_t h, const float clear[4], const struct pc *pc)
{
    VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, color,
                                     VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, 0, 0,
                                     clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD,
                                     VK_ATTACHMENT_STORE_OP_STORE };
    if (clear)
        memcpy(ca.clearValue.color.float32, clear, sizeof ca.clearValue.color.float32);
    VkRect2D area = { { 0, 0 }, { w, h } };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, area, 1, 0, 1, &ca };
    vkCmdBeginRendering(cb, &ri);
    if (pipe) {
        VkViewport vp = { 0, 0, w, h, 0, 1 };
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, 0);
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &area);
        vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof *pc, pc);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRendering(cb);
}

static int pixel(const unsigned char *p, int round, const float want[4], const char *what)
{
    const unsigned char *q = p + 4 * (32 * W + 32);
    unsigned r = want[0] * 255, g = want[1] * 255, b = want[2] * 255;
    if (q[0] == r && q[1] == g && q[2] == b)
        return 0;
    printf("round %d: %u,%u,%u, wanted %u,%u,%u: %s\n== vk_vtxread: FAIL\n", round, q[0], q[1], q[2], r, g, b, what);
    return 1;
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 200;
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_vtxread", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { printf("no device\n== vk_vtxread: FAIL\n"); return 1; }
    VkPhysicalDeviceFeatures have;
    vkGetPhysicalDeviceFeatures(pd, &have);
    // Without stores in fragment shaders there is no case 2.
    int stores = have.fragmentStoresAndAtomics;
    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures want = { .fragmentStoresAndAtomics = stores };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1, &q, 0, 0, 0, 0, &want };
    CHECK(vkCreateDevice(pd, &dci, 0, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);

    VkImage t, u, u2;
    VkImageView tv, uv, u2v;
    if (make_image(TW, TH, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &t, &tv) ||
        make_image(W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &u, &uv) ||
        make_image(W, H, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &u2, &u2v))
        return 1;
    VkBuffer out, sbuf;
    void *outmap, *smap;
    if (make_buffer(2 * W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out, &outmap) ||
        make_buffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &sbuf, &smap))
        return 1;
    const float blue[4] = { 0, 0, 1, 1 }, white[4] = { 1, 1, 1, 1 };
    memcpy(smap, blue, sizeof blue);

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    VkSampler sampler;
    CHECK(vkCreateSampler(dev, &sci, 0, &sampler));
    VkDescriptorSetLayoutBinding lb[2] = {
        { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT },
        { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
    };
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 2, lb };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &lci, 0, &dsl));
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 2, ps };
    VkDescriptorPool dpool;
    CHECK(vkCreateDescriptorPool(dev, &dpi, 0, &dpool));
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dpool, 1, &dsl };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(dev, &dai, &set));
    VkDescriptorImageInfo dii = { sampler, tv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorBufferInfo dbi = { sbuf, 0, 16 };
    VkWriteDescriptorSet wr[2] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, set, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, &dbi },
    };
    vkUpdateDescriptorSets(dev, 2, wr, 0, 0);

    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(struct pc) };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 1, &pr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &pli, 0, &pl));

    // heavy: full + heavy; store: full + store; rtex: rtex + flat; rbuf: rbuf + flat.
    const struct { const uint32_t *v; size_t vn; const uint32_t *f; size_t fn; } src[4] = {
        { vvr_full, sizeof vvr_full, vvr_heavy, sizeof vvr_heavy },
        { vvr_full, sizeof vvr_full, vvr_store, sizeof vvr_store },
        { vvr_rtex, sizeof vvr_rtex, vvr_flat, sizeof vvr_flat },
        { vvr_rbuf, sizeof vvr_rbuf, vvr_flat, sizeof vvr_flat },
    };
    VkPipeline pipe[4] = { 0 };
    for (int k = 0; k < 4; k++) {
        if (k == 1 && !stores)
            continue;
        VkShaderModuleCreateInfo vi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, src[k].vn, src[k].v };
        VkShaderModuleCreateInfo fi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, src[k].fn, src[k].f };
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
        VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0,
                                                    VK_SAMPLE_COUNT_1_BIT };
        VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
        VkPipelineColorBlendStateCreateInfo cbs = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &ba };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, 0, 0, 2, dyn };
        VkFormat cf = VK_FORMAT_R8G8B8A8_UNORM;
        VkPipelineRenderingCreateInfo prc = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 1, &cf };
        VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prc, 0, 2, st, &vin, &ia, 0, &vps,
                                            &rs, &ms, 0, &cbs, &dsi, pl };
        CHECK(vkCreateGraphicsPipelines(dev, 0, 1, &gi, 0, &pipe[k]));
    }

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpi, 0, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };

    // Before the first round T holds blue, as the buffer does.
    CHECK(vkBeginCommandBuffer(cb, &cbi));
    transition(cb, t, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    pass(cb, 0, pl, set, tv, TW, TH, blue, 0);
    CHECK(vkEndCommandBuffer(cb));
    CHECK(vkQueueSubmit(queue, 1, &si, 0));
    CHECK(vkQueueWaitIdle(queue));

    const float colors[4][4] = { { 0, 1, 0, 1 }, { 1, 0, 0, 1 }, { 1, 1, 0, 1 }, { 0, 1, 1, 1 } };
    for (int round = 0; round < ROUNDS; round++) {
        struct pc a = { { 0 }, iters }, b = { { 0 }, iters };
        memcpy(a.color, colors[round & 1], sizeof a.color);
        memcpy(b.color, colors[2 + (round & 1)], sizeof b.color);
        CHECK(vkBeginCommandBuffer(cb, &cbi));
        transition(cb, u, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        transition(cb, u2, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // 1: a colour into T, then a vertex shader that fetches it.
        pass(cb, pipe[0], pl, set, tv, TW, TH, 0, &a);
        transition(cb, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        pass(cb, pipe[2], pl, set, uv, W, H, white, &a);
        transition(cb, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        // 2: a colour into the buffer from a fragment shader, then a vertex
        // shader that reads the buffer.
        if (stores) {
            pass(cb, pipe[1], pl, set, tv, TW, TH, 0, &b);
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT, 0, 1, &mb,
                                 0, 0, 0, 0);
            pass(cb, pipe[3], pl, set, u2v, W, H, white, &b);
        }
        transition(cb, u, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        transition(cb, u2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c0 = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
        VkBufferImageCopy c1 = c0;
        c1.bufferOffset = W * H * 4;
        vkCmdCopyImageToBuffer(cb, u, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1, &c0);
        if (stores)
            vkCmdCopyImageToBuffer(cb, u2, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1, &c1);
        CHECK(vkEndCommandBuffer(cb));
        CHECK(vkQueueSubmit(queue, 1, &si, 0));
        CHECK(vkQueueWaitIdle(queue));
        const unsigned char *p = outmap;
        if (pixel(p, round, a.color, "the vertex shader fetched the target before the pass on it had been shaded") ||
            (stores && pixel(p + W * H * 4, round, b.color,
                             "the vertex shader read the buffer before the fragment shader had stored into it")))
            return 1;
    }
    if (!stores)
        printf("no fragmentStoresAndAtomics: the buffer case was not run\n");
    printf("== vk_vtxread: ok\n");
    return 0;
}
