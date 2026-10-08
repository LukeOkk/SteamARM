// Geometry shaders in SteamARM's KosmicKrisp (patches/kosmickrisp-27-geometry-shaders.patch),
// checked against llvmpipe: a native macOS program that loads each driver
// itself (dlopen, vk_icdGetInstanceProcAddr; no loader, no shim), draws the
// same scenes with both and compares the pictures pixel by pixel.
//
// KosmicKrisp had no geometry shaders: a pipeline with one failed to build
// ("RasterizationEnabled is false but the vertex shader's return type is not
// void"), and vkd3d-proton's CreateGraphicsPipelineState failed with it --
// FINAL FANTASY VII REMAKE (Unreal Engine 4, one-pass point light shadows)
// showed an empty error box from its render thread and never drew a frame.
//
// Scenes (64x64 RGBA8, cleared to grey): pass-through triangles; points
// expanded to quads; two triangles per input triangle, the second only for
// even primitives (EndPrimitive, varying output counts); layered rendering,
// two invocations writing gl_Layer into a 2-layer image; the pass-through
// scene again from vkCmdDrawIndirect, from an indexed triangle strip with
// primitive restart, and from a strip with back faces culled (the winding
// of a strip's odd triangles). Pixels may differ along edges (rasterisation rules);
// a scene fails when more than 2 % of its pixels differ by more than 8/255.
//
// Usage: vk_geometry KOSMICKRISP_DYLIB LLVMPIPE_DYLIB (tests/kk/run.sh);
// VK_GEOMETRY_DUMP=DIR also writes both drivers' pictures there (PPM).
// Prints "== vk_geometry: ok", "== vk_geometry: skip (...)" or what failed.
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "vk_geometry_spv.h"

enum { W = 64, H = 64, LAYERS = 2 };

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s: %s -> %d\n", d->name, #x, r_); return 0; } } while (0)

#define IFNS(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DFNS(X) \
    X(vkGetDeviceQueue) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer) X(vkCmdBeginRendering) X(vkCmdEndRendering) \
    X(vkCreateShaderModule) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) \
    X(vkCmdBindPipeline) X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndirect) \
    X(vkCmdBindIndexBuffer) X(vkCmdPushConstants) X(vkResetCommandBuffer) \
    X(vkDestroyPipeline) X(vkDeviceWaitIdle)

struct drv {
    const char *name;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkImage img;
    VkImageView view;
    VkBuffer down, idx, ind;
    void *down_map, *idx_map, *ind_map;
    VkPipelineLayout layout;
    VkShaderModule vs, fs, gs[4];
#define DECL(n) PFN_##n n;
    IFNS(DECL)
    DFNS(DECL)
};

static uint32_t
mem_type(struct drv *d, uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    d->vkGetPhysicalDeviceMemoryProperties(d->pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

static VkBuffer
make_buffer(struct drv *d, VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size, usage };
    VkBuffer buf = VK_NULL_HANDLE;
    if (d->vkCreateBuffer(d->dev, &bci, 0, &buf) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    VkMemoryRequirements mr;
    d->vkGetBufferMemoryRequirements(d->dev, buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
                                 mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory mem;
    if (d->vkAllocateMemory(d->dev, &mai, 0, &mem) != VK_SUCCESS ||
        d->vkBindBufferMemory(d->dev, buf, mem, 0) != VK_SUCCESS ||
        d->vkMapMemory(d->dev, mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return buf;
}

static VkShaderModule
module(struct drv *d, const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, size, code };
    VkShaderModule m = VK_NULL_HANDLE;
    d->vkCreateShaderModule(d->dev, &ci, 0, &m);
    return m;
}

/* 0 on failure (printed), 1 when ready; *skip set when the device has no
 * geometry shaders. */
static int
open_driver(struct drv *d, const char *path, int *skip)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("%s: dlopen: %s\n", d->name, dlerror());
        return 0;
    }
    d->gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)d->gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_geometry", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    CHECK(create(&ici, 0, &d->inst));
#define LOADI(n) d->n = (PFN_##n)d->gipa(d->inst, #n);
    IFNS(LOADI)
    uint32_t n = 1;
    d->vkEnumeratePhysicalDevices(d->inst, &n, &d->pd);
    if (!n) {
        printf("%s: no device\n", d->name);
        return 0;
    }
    VkPhysicalDeviceFeatures f;
    d->vkGetPhysicalDeviceFeatures(d->pd, &f);
    if (!f.geometryShader) {
        *skip = 1;
        return 0;
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceFeatures want = { .geometryShader = VK_TRUE };
    VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    v13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v13, 0, 1, &qci, 0, 0, 0, 0, &want };
    CHECK(d->vkCreateDevice(d->pd, &dci, 0, &d->dev));
#define LOADD(n) d->n = (PFN_##n)d->vkGetDeviceProcAddr(d->dev, #n);
    DFNS(LOADD)
    d->vkGetDeviceQueue(d->dev, 0, 0, &d->queue);

    VkImageCreateInfo imci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                               { W, H, 1 }, 1, LAYERS, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    CHECK(d->vkCreateImage(d->dev, &imci, 0, &d->img));
    VkMemoryRequirements mr;
    d->vkGetImageMemoryRequirements(d->dev, d->img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mem_type(d, mr.memoryTypeBits, 0) };
    VkDeviceMemory mem;
    CHECK(d->vkAllocateMemory(d->dev, &mai, 0, &mem));
    CHECK(d->vkBindImageMemory(d->dev, d->img, mem, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, d->img, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                  VK_FORMAT_R8G8B8A8_UNORM, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, LAYERS } };
    CHECK(d->vkCreateImageView(d->dev, &vci, 0, &d->view));

    d->down = make_buffer(d, W * H * 4 * LAYERS, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &d->down_map);
    d->idx = make_buffer(d, 64, VK_BUFFER_USAGE_INDEX_BUFFER_BIT, &d->idx_map);
    d->ind = make_buffer(d, 64, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, &d->ind_map);
    if (!d->down || !d->idx || !d->ind) {
        printf("%s: buffers\n", d->name);
        return 0;
    }
    /* A strip of six vertices, restart, a strip of four. */
    uint16_t idx[] = { 0, 1, 2, 3, 4, 5, 0xffff, 4, 5, 6, 7 };
    memcpy(d->idx_map, idx, sizeof(idx));
    VkDrawIndirectCommand ind = { 6, 1, 2, 0 };
    memcpy(d->ind_map, &ind, sizeof(ind));

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    CHECK(d->vkCreateCommandPool(d->dev, &pci, 0, &d->pool));
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, d->pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    CHECK(d->vkAllocateCommandBuffers(d->dev, &ai, &d->cb));

    VkPushConstantRange pc = { VK_SHADER_STAGE_VERTEX_BIT, 0, 32 };
    VkPipelineLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 0, 0, 1, &pc };
    CHECK(d->vkCreatePipelineLayout(d->dev, &lci, 0, &d->layout));
    d->vs = module(d, spv_tri_vert, sizeof(spv_tri_vert));
    d->fs = module(d, spv_color_frag, sizeof(spv_color_frag));
    d->gs[0] = module(d, spv_pass_geom, sizeof(spv_pass_geom));
    d->gs[1] = module(d, spv_quads_geom, sizeof(spv_quads_geom));
    d->gs[2] = module(d, spv_split_geom, sizeof(spv_split_geom));
    d->gs[3] = module(d, spv_layers_geom, sizeof(spv_layers_geom));
    return 1;
}

static VkPipeline
pipeline(struct drv *d, VkShaderModule gs, VkPrimitiveTopology topo, VkBool32 restart, VkCullModeFlags cull)
{
    VkPipelineShaderStageCreateInfo st[3] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, d->vs, "main" },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gs, "main" },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, d->fs, "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0,
                                                  topo, restart };
    VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.cullMode = cull;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 1, &fmt };
    VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, 3, st, &vi, &ia, 0,
                                         &vps, &rs, &ms, 0, &cb, 0, d->layout };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = d->vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gci, 0, &p);
    if (r != VK_SUCCESS)
        printf("%s: vkCreateGraphicsPipelines -> %d\n", d->name, r);
    return p;
}

enum scene { PASS, QUADS, SPLIT, LAYERS_, INDIRECT, RESTART, STRIP_CULL, NSCENES };
static const char *scene_name[] = { "pass-through", "points to quads", "two strips, varying count",
                                    "layered (gl_Layer, 2 invocations)", "pass-through, indirect",
                                    "pass-through, indexed strip with restart",
                                    "pass-through, triangle strip, back faces culled" };

static void
image_barrier(struct drv *d, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, src, dst, from, to,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, d->img,
                               { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, LAYERS } };
    d->vkCmdPipelineBarrier(d->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0,
                            0, 1, &b);
}

/* Draws the scene; the picture (both layers) in d->down_map. */
static int
draw(struct drv *d, enum scene s)
{
    static const VkPrimitiveTopology topo[] = { VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
                                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    static const int gs[] = { 0, 1, 2, 3, 0, 0, 0 };
    /* A strip's odd triangles reach the geometry shader with their first two
     * vertices swapped, so that every triangle keeps the strip's winding. */
    VkPipeline p = pipeline(d, d->gs[gs[s]], topo[s], s == RESTART,
                            s == STRIP_CULL ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    if (!p)
        return 0;
    CHECK(d->vkResetCommandBuffer(d->cb, 0));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(d->vkBeginCommandBuffer(d->cb, &bi));
    image_barrier(d, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, d->view,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.clearValue.color = (VkClearColorValue){ { 0.25f, 0.25f, 0.25f, 1.0f } };
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, { { 0, 0 }, { W, H } }, LAYERS, 0, 1, &att };
    d->vkCmdBeginRendering(d->cb, &ri);
    d->vkCmdBindPipeline(d->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    float pc[8] = { 0 };
    d->vkCmdPushConstants(d->cb, d->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
    switch (s) {
    case QUADS:
        d->vkCmdDraw(d->cb, 8, 1, 0, 0);
        break;
    case INDIRECT:
        d->vkCmdDrawIndirect(d->cb, d->ind, 0, 1, sizeof(VkDrawIndirectCommand));
        break;
    case STRIP_CULL:
        d->vkCmdDraw(d->cb, 8, 1, 0, 0);
        break;
    case RESTART:
        d->vkCmdBindIndexBuffer(d->cb, d->idx, 0, VK_INDEX_TYPE_UINT16);
        d->vkCmdDrawIndexed(d->cb, 11, 1, 0, 0, 0);
        break;
    default:
        d->vkCmdDraw(d->cb, 6, 1, 0, 0);
        /* A second instance and draw: per-draw state must not leak. */
        pc[0] = 0.1f;
        d->vkCmdPushConstants(d->cb, d->layout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), pc);
        d->vkCmdDraw(d->cb, 3, 2, 3, 0);
        break;
    }
    d->vkCmdEndRendering(d->cb);
    image_barrier(d, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, LAYERS }, { 0, 0, 0 }, { W, H, 1 } };
    d->vkCmdCopyImageToBuffer(d->cb, d->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->down, 1, &r);
    CHECK(d->vkEndCommandBuffer(d->cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &d->cb };
    CHECK(d->vkQueueSubmit(d->queue, 1, &si, VK_NULL_HANDLE));
    CHECK(d->vkQueueWaitIdle(d->queue));
    d->vkDestroyPipeline(d->dev, p, 0);
    return 1;
}

int
main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: vk_geometry KOSMICKRISP_DYLIB LLVMPIPE_DYLIB\n");
        return 2;
    }
    struct drv kk = { .name = "kosmickrisp" }, ref = { .name = "llvmpipe" };
    int skip = 0;
    if (!open_driver(&kk, argv[1], &skip)) {
        printf(skip ? "== vk_geometry: skip (no geometryShader)\n" : "== vk_geometry: FAIL\n");
        return skip ? 0 : 1;
    }
    if (!open_driver(&ref, argv[2], &skip)) {
        printf("== vk_geometry: skip (reference driver unusable)\n");
        return 0;
    }
    int failures = 0;
    static uint8_t want[W * H * 4 * LAYERS];
    for (int s = 0; s < NSCENES; s++) {
        if (!draw(&ref, s)) {
            printf("  skip  %s (reference)\n", scene_name[s]);
            continue;
        }
        memcpy(want, ref.down_map, sizeof(want));
        if (!draw(&kk, s)) {
            printf("  FAIL  %s: not drawn\n", scene_name[s]);
            failures++;
            continue;
        }
        const uint8_t *got = kk.down_map;
        int layers = s == LAYERS_ ? LAYERS : 1, bad = 0, covered = 0;
        for (int i = 0; i < W * H * 4 * layers; i += 4) {
            int diff = 0;
            for (int c = 0; c < 4; c++) {
                int e = abs((int)got[i + c] - (int)want[i + c]);
                if (e > diff)
                    diff = e;
            }
            bad += diff > 8;
            covered += want[i] != 64 || want[i + 1] != 64 || want[i + 2] != 64;
        }
        const char *dump = getenv("VK_GEOMETRY_DUMP");
        if (dump) {
            /* PPM pictures of both drivers' layers, for a closer look. */
            for (int l = 0; l < layers; l++)
                for (int k = 0; k < 2; k++) {
                    char path[512];
                    snprintf(path, sizeof(path), "%s/scene%d_layer%d_%s.ppm", dump, s, l, k ? "llvmpipe" : "kk");
                    FILE *f = fopen(path, "wb");
                    if (!f)
                        continue;
                    fprintf(f, "P6\n%d %d\n255\n", W, H);
                    const uint8_t *px = (k ? want : got) + l * W * H * 4;
                    for (int i = 0; i < W * H; i++)
                        fwrite(px + i * 4, 1, 3, f);
                    fclose(f);
                }
        }
        int n = W * H * layers;
        if (bad * 50 > n || covered == 0) {
            printf("  FAIL  %s: %d of %d pixels differ (%d covered in the reference)\n", scene_name[s], bad, n, covered);
            failures++;
        } else {
            printf("  ok    %s: %d of %d pixels covered, %d differ\n", scene_name[s], covered, n, bad);
        }
    }
    printf("== vk_geometry: %s\n", failures ? "FAIL" : "ok");
    return failures != 0;
}
