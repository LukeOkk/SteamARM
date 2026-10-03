// Cut-out foliage at 4x MSAA, as a native program on the Vulkan shim: a
// full-screen red triangle over a blue clear whose alpha is 0 on the left
// half (x < 32) and 1 on the right. Each case removes the transparent half a
// different way -- alpha-to-coverage (alpha from gl_FragCoord or from a
// texture sampled with implicit derivatives), discard, demote, a
// gl_SampleMask write, a static pSampleMask, and a depth prepass with discard
// or alpha-to-coverage followed by an opaque colour pass with depth EQUAL.
// The 4x target is resolved and read back: the left half must stay blue, the
// right half must be red. Also alpha 0.5 (half the samples), the colour
// target B10G11R11 (no alpha channel) and RGBA16F, colour writes of RGB only,
// blending, and a prepass in a render pass with no colour attachment.
// KosmicKrisp applied alpha-to-coverage to the colour after its in-shader
// blending, write mask and trimming (a palm card on de_dust2 drawn opaque);
// with "cache" as the second argument every pipeline goes through one
// VkPipelineCache, where KosmicKrisp handed an alpha-to-coverage pipeline
// the one compiled without it (patches/kosmickrisp-08).
// Usage: vk_alphamask [case-substring|""] [cache]
// Prints one line per case and "== vk_alphamask: ok" or "== vk_alphamask: FAIL".
//
// The shaders (vk_alphamask_spv.h, glslangValidator -V --target-env vulkan1.3):
//   full.vert:      full-screen triangle at z 0.5
//   coord.frag:     o = vec4(1,0,0, gl_FragCoord.x < 32 ? 0 : 1)
//   tex.frag:       a = texture(t, vec2(gl_FragCoord.x/64, 0.5)).a; o = vec4(1,0,0,a)
//                   (t: 2x1, alpha 0 then 1, nearest)
//   texopaque.frag: tex.frag with alpha 1
//   discard.frag:   tex.frag, and if (a < 0.5) discard
//   demote.frag:    tex.frag, and if (a < 0.5) demote; a second texture() after it
//   smask.frag:     tex.frag, and gl_SampleMask[0] = a < 0.5 ? 0 : -1
//   smaskc.frag:    coord.frag's test as gl_SampleMask[0], alpha 1
//   half.frag:      o = vec4(1,0,0,0.5)
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
static VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;

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
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &prc, 0, 2, st, &vin, &ia, 0, &vps,
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
    const char *only = argc > 1 && argv[1][0] ? argv[1] : 0;
    int use_cache = argc > 2 && !strcmp(argv[2], "cache");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_alphamask", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { printf("no device\n== vk_alphamask: FAIL\n"); return 1; }
    VkPhysicalDeviceVulkan13Features h13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 have = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &h13 };
    vkGetPhysicalDeviceFeatures2(pd, &have);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);
    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = h13.shaderDemoteToHelperInvocation;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1, &q };
    CHECK(vkCreateDevice(pd, &dci, 0, &dev));
    VkQueue queue;
    vkGetDeviceQueue(dev, 0, 0, &queue);
    if (use_cache) {
        VkPipelineCacheCreateInfo pci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO };
        CHECK(vkCreatePipelineCache(dev, &pci, 0, &pcache));
        printf("pipelines through a VkPipelineCache\n");
    }

    static const VkFormat formats[3] = { VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B10G11R11_UFLOAT_PACK32,
                                         VK_FORMAT_R16G16B16A16_SFLOAT };
    VkImage ms_cs[3], ress[3], ms_d, tex;
    VkImageView ms_cvs[3], resvs[3], ms_dv, texv;
    for (int f = 0; f < 3; f++) {
        make_image(W, H, formats[f], VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT, &ms_cs[f], &ms_cvs[f]);
        make_image(W, H, formats[f], VK_SAMPLE_COUNT_1_BIT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT,
                   &ress[f], &resvs[f]);
    }
    make_image(W, H, VK_FORMAT_D32_SFLOAT, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT, &ms_d, &ms_dv);
    make_image(2, 1, VK_FORMAT_R8G8B8A8_UNORM, VK_SAMPLE_COUNT_1_BIT,
               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &tex, &texv);
    VkBuffer out, up;
    void *outmap, *upmap;
    make_buffer(W * H * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &out, &outmap);
    make_buffer(256, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up, &upmap);
    const unsigned char texels[8] = { 255, 255, 255, 0, 255, 255, 255, 255 };
    memcpy(upmap, texels, sizeof texels);

    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSampler sampler;
    CHECK(vkCreateSampler(dev, &sci, 0, &sampler));
    VkDescriptorSetLayoutBinding lb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &lb };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &lci, 0, &dsl));
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, 0, 0, 1, 1, &ps };
    VkDescriptorPool dpool;
    CHECK(vkCreateDescriptorPool(dev, &dpi, 0, &dpool));
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, 0, dpool, 1, &dsl };
    VkDescriptorSet set;
    CHECK(vkAllocateDescriptorSets(dev, &dai, &set));
    VkDescriptorImageInfo dii = { sampler, texv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, 0, set, 0, 0, 1,
                                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii };
    vkUpdateDescriptorSets(dev, 1, &wr, 0, 0);
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl };
    CHECK(vkCreatePipelineLayout(dev, &pli, 0, &pl));

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpi, 0, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &cb };

    CHECK(vkBeginCommandBuffer(cb, &cbi));
    barrier(cb, tex, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy tc = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { 2, 1, 1 } };
    vkCmdCopyBufferToImage(cb, up, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &tc);
    barrier(cb, tex, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    CHECK(vkEndCommandBuffer(cb));
    CHECK(vkQueueSubmit(queue, 1, &si, 0));
    CHECK(vkQueueWaitIdle(queue));

    static const VkSampleMask all = 0xffffffffu, low4 = 0xfu;
    const VkColorComponentFlags rgba = 0xf, rgb = 0x7;
#define FS(x) vam_##x, sizeof vam_##x
#define LESS 1, 1, VK_COMPARE_OP_LESS
    struct pipe_desc opaque_eq = { FS(texopaque), 0, 0, rgba, 1, 0, VK_COMPARE_OP_EQUAL };
    struct { const char *name; int fmt; struct pipe_desc a, b; int two; int need_demote; int split; int opaque; } cases[] = {
        // The same shaders and state without alpha-to-coverage first: a
        // driver that caches the compiled pipeline without the flag in its
        // key hands the next case this one.
        { "no a2c coord (opaque)",   0, { FS(coord), 0, 0, rgba, LESS }, {0}, 0, 0, 0, 1 },
        { "no a2c tex (opaque)",     0, { FS(tex), 0, 0, rgba, LESS }, {0}, 0, 0, 0, 1 },
        { "a2c coord",               0, { FS(coord), 1, 0, rgba, LESS } },
        { "a2c tex",                 0, { FS(tex), 1, 0, rgba, LESS } },
        { "a2c tex mask=~0",         0, { FS(tex), 1, &all, rgba, LESS } },
        { "a2c tex mask=0xf",        0, { FS(tex), 1, &low4, rgba, LESS } },
        { "a2c coord mask=0xf",      0, { FS(coord), 1, &low4, rgba, LESS } },
        { "a2c tex write=rgb",       0, { FS(tex), 1, 0, rgb, LESS } },
        { "a2c tex blend",           0, { FS(tex), 1, 0, rgba, LESS, 1 } },
        { "a2c tex r11g11b10",       1, { FS(tex), 1, 0, rgba, LESS } },
        { "a2c tex rgba16f",         2, { FS(tex), 1, 0, rgba, LESS } },
        // Alpha 0.5 everywhere: half the samples, red and blue half each.
        { "a2c alpha 0.5",           0, { FS(half), 1, 0, rgba, LESS }, {0}, 0, 0, 0, 2 },
        { "a2c alpha 0.5 write=rgb", 0, { FS(half), 1, 0, rgb, LESS }, {0}, 0, 0, 0, 2 },
        { "a2c alpha 0.5 r11g11b10", 1, { FS(half), 1, 0, rgba, LESS }, {0}, 0, 0, 0, 2 },
        { "discard",                 0, { FS(discard), 0, 0, rgba, LESS } },
        { "discard+a2c",             0, { FS(discard), 1, 0, rgba, LESS } },
        { "demote",                  0, { FS(demote), 0, 0, rgba, LESS }, {0}, 0, 1 },
        { "samplemask tex",          0, { FS(smask), 0, 0, rgba, LESS } },
        { "samplemask coord",        0, { FS(smaskc), 0, 0, rgba, LESS } },
        { "samplemask+a2c write=0",  0, { FS(smask), 1, 0, 0, LESS }, opaque_eq, 1 },
        { "prepass discard + EQ",    0, { FS(discard), 0, 0, 0, LESS }, opaque_eq, 1 },
        { "prepass a2c + EQ",        0, { FS(tex), 1, 0, 0, LESS }, opaque_eq, 1 },
        { "prepass a2c coord + EQ",  0, { FS(coord), 1, 0, 0, LESS }, opaque_eq, 1 },
        { "prepass a2c mask=0xf + EQ", 0, { FS(tex), 1, &low4, 0, LESS }, opaque_eq, 1 },
        { "prepass a2c r11g11b10 + EQ", 1, { FS(tex), 1, 0, 0, LESS }, opaque_eq, 1 },
        { "depth-only a2c + EQ",     0, { FS(tex), 1, 0, 0, LESS, 0, 1 }, opaque_eq, 1, 0, 1 },
        { "depth-only discard + EQ", 0, { FS(discard), 0, 0, 0, LESS, 0, 1 }, opaque_eq, 1, 0, 1 },
    };
    int fails = 0;
    for (unsigned k = 0; k < sizeof cases / sizeof cases[0]; k++) {
        if (only && !strstr(cases[k].name, only))
            continue;
        if (cases[k].need_demote && !h13.shaderDemoteToHelperInvocation) {
            printf("  skip  %-26s (no demote)\n", cases[k].name);
            continue;
        }
        int f = cases[k].fmt;
        VkImage ms_c = ms_cs[f], res = ress[f];
        color_format = formats[f];
        VkPipeline pa = pipeline(&cases[k].a), pb = cases[k].two ? pipeline(&cases[k].b) : 0;
        CHECK(vkBeginCommandBuffer(cb, &cbi));
        barrier(cb, ms_c, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        barrier(cb, res, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        barrier(cb, ms_d, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo ca = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, ms_cvs[f],
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_RESOLVE_MODE_AVERAGE_BIT, resvs[f],
                                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                         VK_ATTACHMENT_STORE_OP_STORE };
        ca.clearValue.color.float32[2] = 1;
        ca.clearValue.color.float32[3] = 1;
        VkRenderingAttachmentInfo da = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, ms_dv,
                                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, 0, 0, 0, VK_ATTACHMENT_LOAD_OP_CLEAR,
                                         VK_ATTACHMENT_STORE_OP_STORE };
        da.clearValue.depthStencil.depth = 1;
        VkRect2D area = { { 0, 0 }, { W, H } };
        VkViewport vp = { 0, 0, W, H, 0, 1 };
        if (cases[k].split) {
            // The prepass in a render pass of its own with no colour attachment.
            VkRenderingInfo r0 = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, area, 1, 0, 0, 0, &da };
            vkCmdBeginRendering(cb, &r0);
            vkCmdSetViewport(cb, 0, 1, &vp);
            vkCmdSetScissor(cb, 0, 1, &area);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, 0);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pa);
            vkCmdDraw(cb, 3, 1, 0, 0);
            vkCmdEndRendering(cb);
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                                 0, 1, &mb, 0, 0, 0, 0);
            da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        }
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, area, 1, 0, 1, &ca, &da };
        vkCmdBeginRendering(cb, &ri);
        vkCmdSetViewport(cb, 0, 1, &vp);
        vkCmdSetScissor(cb, 0, 1, &area);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, 0);
        if (!cases[k].split) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pa);
            vkCmdDraw(cb, 3, 1, 0, 0);
        }
        if (pb) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pb);
            vkCmdDraw(cb, 3, 1, 0, 0);
        }
        vkCmdEndRendering(cb);
        barrier(cb, res, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy c0 = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
        vkCmdCopyImageToBuffer(cb, res, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, 1, &c0);
        CHECK(vkEndCommandBuffer(cb));
        CHECK(vkQueueSubmit(queue, 1, &si, 0));
        CHECK(vkQueueWaitIdle(queue));
        float l[3], r[3];
        texel(outmap, f, 16, 32, l);
        texel(outmap, f, 48, 32, r);
        int ok = cases[k].opaque == 2 ?
                 l[0] > 0.4f && l[0] < 0.6f && l[2] > 0.4f && l[2] < 0.6f && r[0] > 0.4f && r[0] < 0.6f :
                 (cases[k].opaque ? l[0] > 0.97f && l[2] < 0.03f : l[0] < 0.03f && l[2] > 0.97f) &&
                 r[0] > 0.97f && r[2] < 0.03f;
        printf("  %-4s  %-26s left %.2f,%.2f,%.2f  right %.2f,%.2f,%.2f%s\n", ok ? "ok" : "FAIL", cases[k].name,
               l[0], l[1], l[2], r[0], r[1], r[2],
               ok ? "" : cases[k].opaque ? "  <- wrong" : (l[0] > 0.03f ? "  <- transparent half drawn" : "  <- opaque half missing"));
        fails += !ok;
        vkDestroyPipeline(dev, pa, 0);
        if (pb)
            vkDestroyPipeline(dev, pb, 0);
    }
    printf(fails ? "== vk_alphamask: FAIL\n" : "== vk_alphamask: ok\n");
    return fails != 0;
}
