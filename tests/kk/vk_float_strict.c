// KK_FLOAT_STRICT in SteamARM's KosmicKrisp (patches/kosmickrisp-28-float-strict.patch):
// a native macOS program that loads the driver itself (dlopen,
// vk_icdGetInstanceProcAddr; no loader, no shim), as vk_geometry.c does, and
// goes through each mode -- KK_FLOAT_STRICT unset (full), nan, fast -- with
// shaders that declare SignedZeroInfNanPreserve the way DXVK and
// vkd3d-proton do for every DXBC shader (tests/kk/float_strict/):
//
//   results  a compute shader of min/max, compares, isnan/isinf, multiplies,
//            fma, sqrt, rsqrt, lerp, fract, pow and sin over ordinary and
//            special inputs: the ordinary ones right in every mode; NaN and
//            the infinities the IEEE way in full and nan for compares,
//            min/max and isnan/isinf; everything, the sign of zero too, in
//            full. Vertex+fragment and vertex+geometry+fragment pipelines
//            drawn into a float target, right in every mode.
//   MSL      what the driver compiled (MESA_KK_DEBUG=msl, read back from
//            stderr): the vertex shaders, everything the geometry pipeline
//            makes of its vertex and geometry shaders (compute kernels and
//            the vertex function that draws) and a fragment shader that
//            writes depth: the same text in every mode. The compute shaders
//            and the other fragment shaders: precise:: functions, isnan
//            guards on compares (KK_WORKAROUND_14), guarded multiplies,
//            relaxed blocks and signed-zero min/max in full; only precise
//            fmin/fmax, the compare guards and their relaxed blocks in nan;
//            none in fast.
//   caches   a pipeline compiled in one mode is not handed to another, by
//            Mesa's disk cache (a temporary MESA_SHADER_CACHE_DIR) or by an
//            application's saved VkPipelineCache (pipelineCacheUUID).
//   time     a lighting-style loop over a million threads, the fastest of
//            five submissions in each mode (reported only).
//
// Usage: vk_float_strict KOSMICKRISP_DYLIB (tests/kk/run.sh);
// VK_FLOAT_STRICT_DUMP=DIR also writes each shader's MSL there.
// Prints "== vk_float_strict: ok" or what failed.
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <fcntl.h>
#include <ftw.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "vk_float_strict_spv.h"

enum { W = 8, H = 8, NOPS = 16, BENCH_THREADS = 1 << 20, BENCH_ITERATIONS = 256 };

enum mode { FULL, NAN_, FAST, NMODES };
static const char *mode_env[] = { NULL, "nan", "fast" };
static const char *mode_name[] = { "full", "nan", "fast" };

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("  FAIL  %s -> %d\n", #x, r_); return 0; } } while (0)

#define IFNS(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceFeatures) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkDestroyInstance)
#define DFNS(X) \
    X(vkGetDeviceQueue) X(vkDestroyDevice) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkGetBufferDeviceAddress) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) \
    X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCmdPipelineBarrier) X(vkCmdCopyImageToBuffer) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) \
    X(vkCreateComputePipelines) X(vkDestroyPipeline) X(vkCmdBindPipeline) X(vkCmdDraw) X(vkCmdDispatch) \
    X(vkCmdPushConstants) X(vkCreatePipelineCache) X(vkGetPipelineCacheData) X(vkDestroyPipelineCache)

struct drv {
    void *lib;
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance inst;
    VkPhysicalDevice pd;
    VkPhysicalDeviceProperties props;
    int geometry;
    VkDevice dev;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkImage img;
    VkImageView view;
    VkBuffer in, out, down;
    void *in_map, *out_map, *down_map;
    VkPipelineLayout cs_layout, gfx_layout;
#define DECL(n) PFN_##n n;
    IFNS(DECL)
    DFNS(DECL)
};

/* ------------------------------------------------------------ the driver */

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
    VkMemoryAllocateFlagsInfo fi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, 0,
                                     VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi, mr.size,
                                 mem_type(d, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory mem;
    if (d->vkAllocateMemory(d->dev, &mai, 0, &mem) != VK_SUCCESS ||
        d->vkBindBufferMemory(d->dev, buf, mem, 0) != VK_SUCCESS ||
        d->vkMapMemory(d->dev, mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return buf;
}

static uint64_t
address(struct drv *d, VkBuffer b)
{
    VkBufferDeviceAddressInfo ai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, 0, b };
    return d->vkGetBufferDeviceAddress(d->dev, &ai);
}

static VkShaderModule
module(struct drv *d, const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, size, code };
    VkShaderModule m = VK_NULL_HANDLE;
    d->vkCreateShaderModule(d->dev, &ci, 0, &m);
    return m;
}

/* An instance (whose physical device reads KK_FLOAT_STRICT as it is now) and
 * a device. With `full`, also what drawing and dispatching need. */
static int
open_driver(struct drv *d, int full)
{
    d->gipa = (PFN_vkGetInstanceProcAddr)dlsym(d->lib, "vk_icdGetInstanceProcAddr");
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)d->gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_float_strict", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    CHECK(create(&ici, 0, &d->inst));
#define LOADI(n) d->n = (PFN_##n)d->gipa(d->inst, #n);
    IFNS(LOADI)
    uint32_t n = 1;
    d->vkEnumeratePhysicalDevices(d->inst, &n, &d->pd);
    if (!n) {
        printf("  FAIL  no device\n");
        return 0;
    }
    d->vkGetPhysicalDeviceProperties(d->pd, &d->props);
    VkPhysicalDeviceFeatures have, want = { 0 };
    d->vkGetPhysicalDeviceFeatures(d->pd, &have);
    d->geometry = want.geometryShader = full && have.geometryShader;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan13Features v13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    v13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &v13 };
    v12.bufferDeviceAddress = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v12, 0, 1, &qci, 0, 0, 0, 0, &want };
    CHECK(d->vkCreateDevice(d->pd, &dci, 0, &d->dev));
#define LOADD(n) d->n = (PFN_##n)d->vkGetDeviceProcAddr(d->dev, #n);
    DFNS(LOADD)
    d->vkGetDeviceQueue(d->dev, 0, 0, &d->queue);

    VkPushConstantRange cs_pc = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 48 };
    VkPipelineLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 0, 0, 1, &cs_pc };
    CHECK(d->vkCreatePipelineLayout(d->dev, &lci, 0, &d->cs_layout));
    if (!full)
        return 1;
    VkPushConstantRange gfx_pc = { VK_SHADER_STAGE_ALL_GRAPHICS, 0, 16 };
    lci.pPushConstantRanges = &gfx_pc;
    CHECK(d->vkCreatePipelineLayout(d->dev, &lci, 0, &d->gfx_layout));

    VkImageCreateInfo imci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D,
                               VK_FORMAT_R32G32B32A32_SFLOAT, { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT,
                               VK_IMAGE_TILING_OPTIMAL,
                               VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT };
    CHECK(d->vkCreateImage(d->dev, &imci, 0, &d->img));
    VkMemoryRequirements mr;
    d->vkGetImageMemoryRequirements(d->dev, d->img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mem_type(d, mr.memoryTypeBits, 0) };
    VkDeviceMemory mem;
    CHECK(d->vkAllocateMemory(d->dev, &mai, 0, &mem));
    CHECK(d->vkBindImageMemory(d->dev, d->img, mem, 0));
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, d->img, VK_IMAGE_VIEW_TYPE_2D,
                                  VK_FORMAT_R32G32B32A32_SFLOAT, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CHECK(d->vkCreateImageView(d->dev, &vci, 0, &d->view));

    VkBufferUsageFlags sb = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    d->in = make_buffer(d, 4096, sb, &d->in_map);
    d->out = make_buffer(d, (VkDeviceSize)BENCH_THREADS * 16, sb, &d->out_map);
    d->down = make_buffer(d, W * H * 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &d->down_map);
    if (!d->in || !d->out || !d->down) {
        printf("  FAIL  buffers\n");
        return 0;
    }
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    CHECK(d->vkCreateCommandPool(d->dev, &pci, 0, &d->pool));
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, d->pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    CHECK(d->vkAllocateCommandBuffers(d->dev, &ai, &d->cb));
    return 1;
}

static VkPipeline
compute_pipeline(struct drv *d, const uint32_t *code, size_t size, VkPipelineCache cache)
{
    VkShaderModule m = module(d, code, size);
    VkComputePipelineCreateInfo ci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
    ci.stage = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0,
                                                  VK_SHADER_STAGE_COMPUTE_BIT, m, "main" };
    ci.layout = d->cs_layout;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = d->vkCreateComputePipelines(d->dev, cache, 1, &ci, 0, &p);
    if (r != VK_SUCCESS)
        printf("  FAIL  vkCreateComputePipelines -> %d\n", r);
    d->vkDestroyShaderModule(d->dev, m, 0);
    return p;
}

enum gfx { COLOR, DEPTH, GEOMETRY };

/* tri.vert with color.frag; with depth.frag and a depth attachment; with
 * pass.geom and color.frag. */
static VkPipeline
graphics_pipeline(struct drv *d, enum gfx which)
{
    int depth = which == DEPTH;
    VkShaderModule vs = module(d, spv_tri_vert, sizeof(spv_tri_vert));
    VkShaderModule fs = depth ? module(d, spv_depth_frag, sizeof(spv_depth_frag))
                              : module(d, spv_color_frag, sizeof(spv_color_frag));
    VkShaderModule gs = which == GEOMETRY ? module(d, spv_pass_geom, sizeof(spv_pass_geom)) : VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo st[3] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_GEOMETRY_BIT, gs, "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0,
                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, W, H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkFormat fmt = VK_FORMAT_R32G32B32A32_SFLOAT;
    VkPipelineRenderingCreateInfo ri = { VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, 0, 0, 1, &fmt,
                                         depth ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_UNDEFINED };
    VkGraphicsPipelineCreateInfo gci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &ri, 0, gs ? 3 : 2, st, &vi,
                                         &ia, 0, &vps, &rs, &ms, depth ? &ds : 0, &cb, 0, d->gfx_layout };
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = d->vkCreateGraphicsPipelines(d->dev, VK_NULL_HANDLE, 1, &gci, 0, &p);
    if (r != VK_SUCCESS)
        printf("  FAIL  vkCreateGraphicsPipelines -> %d\n", r);
    d->vkDestroyShaderModule(d->dev, vs, 0);
    d->vkDestroyShaderModule(d->dev, fs, 0);
    if (gs)
        d->vkDestroyShaderModule(d->dev, gs, 0);
    return p;
}

static int
begin(struct drv *d)
{
    CHECK(d->vkResetCommandBuffer(d->cb, 0));
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(d->vkBeginCommandBuffer(d->cb, &bi));
    return 1;
}

static int
submit(struct drv *d)
{
    CHECK(d->vkEndCommandBuffer(d->cb));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 1, &d->cb };
    CHECK(d->vkQueueSubmit(d->queue, 1, &si, VK_NULL_HANDLE));
    CHECK(d->vkQueueWaitIdle(d->queue));
    return 1;
}

/* ----------------------------------------------------- MSL from stderr */

static char capture_path[512];
static int saved_stderr = -1;

/* The driver prints each shader's MSL (MESA_KK_DEBUG=msl) to stderr. */
static void
capture_start(void)
{
    fflush(stderr);
    snprintf(capture_path, sizeof(capture_path), "%s/vk_float_strict.XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    int fd = mkstemp(capture_path);
    saved_stderr = dup(2);
    dup2(fd, 2);
    close(fd);
}

/* What was printed since capture_start (malloc'd). */
static char *
capture_end(void)
{
    fflush(stderr);
    dup2(saved_stderr, 2);
    close(saved_stderr);
    FILE *f = fopen(capture_path, "rb");
    char *text = NULL;
    size_t n = 0;
    if (f) {
        fseek(f, 0, SEEK_END);
        n = (size_t)ftell(f);
        fseek(f, 0, SEEK_SET);
        text = malloc(n + 1);
        n = fread(text, 1, n, f);
        text[n] = 0;
        fclose(f);
    }
    unlink(capture_path);
    return text ? text : strdup("");
}

static const char marker[] = "// Generated by Mesa compiler";

static int
count(const char *s, const char *what)
{
    int k = 0;
    for (const char *p = s; (p = strstr(p, what)); p += strlen(what))
        k++;
    return k;
}

/* The MSL units in a capture, each from its first line to the closing brace
 * of its function (the last "}" at the start of a line before the next
 * unit): whatever else was printed in between is left out. */
static int
split(char *text, char **unit, int max)
{
    int k = 0;
    for (char *p = strstr(text, marker); p && k < max; p = strstr(p + 1, marker))
        unit[k++] = p;
    for (int i = k - 1; i >= 0; i--) {
        char *end = NULL;
        for (char *p = unit[i]; (p = strstr(p, "\n}\n")); p += 2)
            end = p;
        if (end)
            end[2] = 0;
    }
    return k;
}

struct tally {
    int precise, precise_minmax, cmp_guard, mul_guard, relaxed, signed_zero;
};

/* What keeps SignedZeroInfNanPreserve in a unit, as nir_to_msl.c writes it:
 * precise:: functions; KK_WORKAROUND_14's isnan guards on a compare
 * ("!isnan(x) && !isnan(y) && " or "isnan(x) || isnan(y) || ") and on a
 * multiply ("!&buf0.contents[0] ? 1 : "); relaxed math blocks; fmin/fmax
 * with signed zeros (the bits of both operands or'ed or and'ed). */
static struct tally
tally(const char *u)
{
    struct tally t;
    t.precise = count(u, "precise::");
    t.precise_minmax = count(u, "precise::fmin(") + count(u, "precise::fmax(");
    t.cmp_guard = count(u, ") && !isnan(") + count(u, ") || isnan(");
    t.mul_guard = count(u, "!&buf0.contents[0]");
    t.relaxed = count(u, "math_mode(relaxed)");
    t.signed_zero = count(u, ") | as_type<uint>(") + count(u, ") & as_type<uint>(");
    return t;
}

/* ------------------------------------------------------------- results */

/* Pairs (a, b) for ops.comp: ordinary ones first. */
static const float pairs[][2] = {
    { 1.5f, -2.25f }, { 0.3f, 0.7f }, { -4.0f, 3.0f }, { 100.0f, 0.01f }, { 2.0f, 2.0f }, { -0.75f, -0.5f },
    { NAN, 1.0f }, { 1.0f, NAN }, { NAN, NAN }, { INFINITY, 1.0f }, { -INFINITY, 1.0f }, { 1.0f, INFINITY },
    { 0.0f, -0.0f }, { -0.0f, 0.0f }, { INFINITY, 0.0f },
};
enum { NPAIRS = sizeof(pairs) / sizeof(pairs[0]), NORDINARY = 6 };
static const char *op_name[NOPS] = { "min", "max", "a<b", "a>=b", "a==b", "a!=b", "isnan", "isinf",
                                     "a*b", "fma", "sqrt", "rsqrt", "mix", "fract", "pow", "sin" };
/* Kept for NaN and infinity in nan as well as in full. */
static const int nan_kept[NOPS] = { 1, 1, 1, 1, 1, 1, 1, 1 };

/* IEEE 754 minNum/maxNum, with -0 < +0 as SignedZeroInfNanPreserve wants. */
static float
ieee_minmax(float a, float b, int max)
{
    if (isnan(a))
        return b;
    if (isnan(b))
        return a;
    if (a == 0.0f && b == 0.0f)
        return (signbit(a) && signbit(b)) || (!max && (signbit(a) || signbit(b))) ? -0.0f : 0.0f;
    return max ? (a > b ? a : b) : (a < b ? a : b);
}

static float
expected(int op, float a, float b)
{
    switch (op) {
    case 0: return ieee_minmax(a, b, 0);
    case 1: return ieee_minmax(a, b, 1);
    case 2: return a < b;
    case 3: return a >= b;
    case 4: return a == b;
    case 5: return a != b;
    case 6: return isnan(a);
    case 7: return isinf(a);
    case 8: return a * b;
    case 9: return fmaf(a, b, 1.0f);
    case 10: return sqrtf(fabsf(a));
    case 11: return 1.0f / sqrtf(fabsf(a) + 1.0f);
    case 12: return a + (b - a) * 0.25f;
    case 13: return a - floorf(a);
    case 14: return powf(fabsf(a) + 1.0f, b);
    default: return sinf(a);
    }
}

static int
same(float got, float want, int sign_of_zero)
{
    if (isnan(want))
        return isnan(got);
    if (isinf(want) || want == 0.0f)
        return got == want && (!sign_of_zero || want != 0.0f || signbit(got) == signbit(want));
    return fabsf(got - want) <= 2e-4f * fmaxf(1.0f, fabsf(want));
}

/* ops.comp over the pairs: what each mode promises. */
static int
check_ops(struct drv *d, VkPipeline p, enum mode m)
{
    memcpy(d->in_map, pairs, sizeof(pairs));
    struct { uint64_t in, out; uint32_t count; } pc = { address(d, d->in), address(d, d->out), NPAIRS };
    if (!begin(d))
        return 0;
    d->vkCmdBindPipeline(d->cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    d->vkCmdPushConstants(d->cb, d->cs_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    d->vkCmdDispatch(d->cb, 1, 1, 1);
    if (!submit(d))
        return 0;
    const float *r = d->out_map;
    int bad = 0, checked = 0;
    for (int i = 0; i < NPAIRS; i++)
        for (int op = 0; op < NOPS; op++) {
            int ordinary = i < NORDINARY;
            if (!ordinary && !(m == FULL || (m == NAN_ && nan_kept[op])))
                continue;
            float a = pairs[i][0], b = pairs[i][1], got = r[i * NOPS + op], want = expected(op, a, b);
            checked++;
            if (!same(got, want, m == FULL)) {
                if (bad++ < 12)
                    printf("        %s: %s(%g, %g) = %g, wanted %g\n", mode_name[m], op_name[op], a, b, got, want);
            }
        }
    printf("  %s  %s: compute results, %d of %d wrong\n", bad ? "FAIL" : "ok  ", mode_name[m], bad, checked);
    return !bad;
}

/* color.frag drawn with tri.vert (and pass.geom) into the float target. */
static int
check_draw(struct drv *d, VkPipeline p, enum mode m, const char *what)
{
    const float k[4] = { 3.0f, 5.0f, 0.0f, 0.0f };
    if (!begin(d))
        return 0;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, 0, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, d->img,
                               { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    d->vkCmdPipelineBarrier(d->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0,
                            0, 0, 1, &b);
    VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, d->view,
                                      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, { { 0, 0 }, { W, H } }, 1, 0, 1, &att };
    d->vkCmdBeginRendering(d->cb, &ri);
    d->vkCmdBindPipeline(d->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    d->vkCmdPushConstants(d->cb, d->gfx_layout, VK_SHADER_STAGE_ALL_GRAPHICS, 0, sizeof(k), k);
    d->vkCmdDraw(d->cb, 3, 1, 0, 0);
    d->vkCmdEndRendering(d->cb);
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    d->vkCmdPipelineBarrier(d->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0,
                            0, 0, 1, &b);
    VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    d->vkCmdCopyImageToBuffer(d->cb, d->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, d->down, 1, &r);
    if (!submit(d))
        return 0;
    const float *px = d->down_map;
    int bad = 0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            float a = (x + 0.5f) / W * k[0], bb = (y + 0.5f) / H * k[1], f = fmodf(a + bb, 1.0f);
            float want[4] = { fminf(a, bb), fmaxf(sqrtf(a), bb * 0.5f), a < bb ? 1.0f : 0.25f, f };
            for (int c = 0; c < 4; c++) {
                /* Where a rounding of the interpolated input decides. */
                if ((c == 2 && fabsf(a - bb) < 1e-3f) || (c == 3 && (f < 1e-3f || f > 1.0f - 1e-3f)))
                    continue;
                if (fabsf(px[(y * W + x) * 4 + c] - want[c]) > 1e-3f) {
                    if (bad++ < 4)
                        printf("        %s: pixel %d,%d channel %d = %g, wanted %g\n", mode_name[m], x, y, c,
                               px[(y * W + x) * 4 + c], want[c]);
                }
            }
        }
    printf("  %s  %s: %s drawn, %d values wrong\n", bad ? "FAIL" : "ok  ", mode_name[m], what, bad);
    return !bad;
}

/* bench.comp over a million threads: the fastest of five runs, in ms. */
static double
bench(struct drv *d, VkPipeline p)
{
    struct { uint64_t out, pad; float k[4]; uint32_t iterations; } pc = { address(d, d->out), 0,
                                                                          { 0.1f, 0.2f, 0.3f, 0.4f },
                                                                          BENCH_ITERATIONS };
    double best = 1e9;
    for (int run = 0; run < 6; run++) {
        if (!begin(d))
            return -1;
        d->vkCmdBindPipeline(d->cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
        d->vkCmdPushConstants(d->cb, d->cs_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        d->vkCmdDispatch(d->cb, BENCH_THREADS / 256, 1, 1);
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        if (!submit(d))
            return -1;
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
        if (run > 0 && ms < best)
            best = ms;
    }
    return best;
}

/* ------------------------------------------------------------- the modes */

static void
set_mode(enum mode m)
{
    if (mode_env[m])
        setenv("KK_FLOAT_STRICT", mode_env[m], 1);
    else
        unsetenv("KK_FLOAT_STRICT");
}

static void
dump(const char *what, enum mode m, const char *unit)
{
    const char *dir = getenv("VK_FLOAT_STRICT_DUMP");
    if (!dir)
        return;
    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%s.metal", dir, what, mode_name[m]);
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(unit, f);
        fclose(f);
    }
}

/* The pipelines of each mode and the shaders the driver printed for them, in
 * the order it compiled them: a kernel for a compute shader; a vertex and a
 * fragment function; for the geometry pipeline, the kernel that runs the
 * vertex shader, the vertex function that draws (where poly re-runs the
 * geometry shader for each vertex), the geometry shader's own kernels (which
 * only count and index) and the fragment function. */
enum pipe { P_OPS, P_COLOR, P_DEPTH, P_GEOM, P_BENCH, NPIPES };
static const char *pipe_name[NPIPES] = { "ops.comp", "color", "depth", "geometry", "bench.comp" };
enum { MAXUNITS = 8 };
struct printed {
    int n;
    char *unit[MAXUNITS];
};
static struct printed printed[NMODES][NPIPES];
static int geometry = 1;

static const char *
kind(const char *unit)
{
    return strstr(unit, "kernel void") ? "kernel" : strstr(unit, "vertex VertexOut") ? "vertex"
         : strstr(unit, "fragment FragmentOut") ? "fragment" : "?";
}

/* Relaxed below full: the application's compute shaders, and fragment
 * shaders that write no depth. */
static int
relaxed_unit(enum pipe p, const char *unit)
{
    return p == P_OPS || p == P_BENCH || ((p == P_COLOR || p == P_GEOM) && !strcmp(kind(unit), "fragment"));
}

/* Whether a unit of the default mode must show what it keeps: all but the
 * geometry shader's own kernels. */
static int
keeps_something(enum pipe p, int i, const char *unit)
{
    return !(p == P_GEOM && i >= 2 && !strcmp(kind(unit), "kernel"));
}

/* What pipeline `p` printed while it was made, for mode `m`. */
static int
take(enum mode m, enum pipe p, char *text)
{
    struct printed *pr = &printed[m][p];
    char *unit[MAXUNITS];
    pr->n = split(text, unit, MAXUNITS);
    static const char *want[NPIPES] = { "kernel", "vertex fragment", "vertex fragment",
                                        "kernel vertex kernel* fragment", "kernel" };
    char got[128] = "";
    for (int i = 0; i < pr->n; i++) {
        pr->unit[i] = strdup(unit[i]);
        snprintf(got + strlen(got), sizeof(got) - strlen(got), "%s%s", i ? " " : "", kind(unit[i]));
        char what[64];
        snprintf(what, sizeof(what), "%s_%d_%s", pipe_name[p], i, kind(unit[i]));
        for (char *c = what; *c; c++)
            if (*c == '.')
                *c = '_';
        dump(what, m, unit[i]);
    }
    free(text);
    /* "kernel*": any number of kernels there. */
    int ok = !strcmp(got, want[p]);
    if (p == P_GEOM) {
        ok = pr->n >= 4 && !strcmp(kind(pr->unit[0]), "kernel") && !strcmp(kind(pr->unit[1]), "vertex") &&
             !strcmp(kind(pr->unit[pr->n - 1]), "fragment");
        for (int i = 2; ok && i < pr->n - 1; i++)
            ok = !strcmp(kind(pr->unit[i]), "kernel");
    }
    if (!ok)
        printf("  FAIL  %s: %s printed \"%s\", wanted \"%s\" (MESA_KK_DEBUG=msl)\n", mode_name[m], pipe_name[p],
               got, want[p]);
    return ok;
}

static int
run_mode(void *lib, enum mode m, double *ms)
{
    set_mode(m);
    struct drv d = { .lib = lib };
    if (!open_driver(&d, 1))
        return 0;
    geometry &= d.geometry;
    VkPipeline pipe[NPIPES] = { 0 };
    int ok = 1;
    for (int p = 0; p < NPIPES; p++) {
        if (p == P_GEOM && !geometry)
            continue;
        capture_start();
        pipe[p] = p == P_OPS     ? compute_pipeline(&d, spv_ops_comp, sizeof(spv_ops_comp), VK_NULL_HANDLE)
                : p == P_BENCH   ? compute_pipeline(&d, spv_bench_comp, sizeof(spv_bench_comp), VK_NULL_HANDLE)
                : p == P_COLOR   ? graphics_pipeline(&d, COLOR)
                : p == P_DEPTH   ? graphics_pipeline(&d, DEPTH)
                                 : graphics_pipeline(&d, GEOMETRY);
        char *text = capture_end();
        if (!pipe[p])
            return 0;
        ok &= take(m, p, text);
    }
    ok &= check_ops(&d, pipe[P_OPS], m);
    ok &= check_draw(&d, pipe[P_COLOR], m, "vertex+fragment pipeline");
    if (geometry)
        ok &= check_draw(&d, pipe[P_GEOM], m, "vertex+geometry+fragment pipeline");
    *ms = bench(&d, pipe[P_BENCH]);
    /* The device and instance are left as they are: nothing here depends on
     * their caches being written out. */
    return ok;
}

/* The MSL of each mode against what it promises. */
static int
check_msl(void)
{
    int bad = 0;
    for (int m = 0; m < NMODES; m++)
        for (int p = 0; p < NPIPES; p++)
            for (int i = 0; i < printed[m][p].n; i++) {
                const char *u = printed[m][p].unit[i];
                struct tally t = tally(u);
                int relaxed = relaxed_unit(p, u), fail = 0;
                if (!relaxed || m == FULL) {
                    /* As the default compiles it, the sanity of the counts below. */
                    if (m != FULL && (i >= printed[FULL][p].n || strcmp(u, printed[FULL][p].unit[i]) != 0))
                        fail = 1;
                    if (m == FULL && keeps_something(p, i, u) &&
                        !(t.precise && t.cmp_guard && t.mul_guard && t.relaxed))
                        fail = 1;
                } else if (m == NAN_) {
                    fail = t.precise != t.precise_minmax || !t.precise_minmax || t.mul_guard || t.signed_zero ||
                           (p == P_OPS && !(t.cmp_guard && t.relaxed));
                } else {
                    fail = t.precise || t.cmp_guard || t.mul_guard || t.relaxed || t.signed_zero;
                }
                char name[48];
                snprintf(name, sizeof(name), "%s %s", pipe_name[p], kind(u));
                printf("  %s  %s: %-19s precise:: %3d (fmin/fmax %2d), compare guards %2d, multiply guards %2d, "
                       "relaxed %3d, signed-zero min/max %2d%s\n",
                       fail ? "FAIL" : "ok  ", mode_name[m], name, t.precise, t.precise_minmax, t.cmp_guard,
                       t.mul_guard, t.relaxed, t.signed_zero, relaxed || m == FULL ? "" : ", same MSL as full");
                bad += fail;
            }
    if (!geometry)
        printf("  skip  geometry pipeline (no geometryShader)\n");
    return !bad;
}

/* How many shaders ops.comp's pipeline compiled in a new instance and device
 * of mode `m` (0: it came from a cache). With `app_cache`, through a
 * VkPipelineCache made from `data` (which may be empty), whose contents are
 * then returned in *saved when `saved` is given. */
static int
compiles(void *lib, enum mode m, int app_cache, const void *data, size_t size, void **saved, size_t *saved_size,
         uint8_t uuid[VK_UUID_SIZE])
{
    set_mode(m);
    struct drv d = { .lib = lib };
    if (!open_driver(&d, 0))
        return -1;
    memcpy(uuid, d.props.pipelineCacheUUID, VK_UUID_SIZE);
    VkPipelineCache pc = VK_NULL_HANDLE;
    if (app_cache) {
        VkPipelineCacheCreateInfo ci = { VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO, 0, 0, size, data };
        if (d.vkCreatePipelineCache(d.dev, &ci, 0, &pc) != VK_SUCCESS)
            return -1;
    }
    capture_start();
    VkPipeline p = compute_pipeline(&d, spv_ops_comp, sizeof(spv_ops_comp), pc);
    char *text = capture_end();
    int n = p ? count(text, marker) : -1;
    free(text);
    if (pc && saved) {
        d.vkGetPipelineCacheData(d.dev, pc, saved_size, NULL);
        *saved = malloc(*saved_size);
        d.vkGetPipelineCacheData(d.dev, pc, saved_size, *saved);
    }
    d.vkDestroyPipeline(d.dev, p, 0);
    if (pc)
        d.vkDestroyPipelineCache(d.dev, pc, 0);
    d.vkDestroyPipelineLayout(d.dev, d.cs_layout, 0);
    d.vkDestroyDevice(d.dev, 0);
    /* Writes the disk cache out. */
    d.vkDestroyInstance(d.inst, 0);
    return n;
}

static int
rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st, (void)flag, (void)ftw;
    return remove(path);
}

static int
check_caches(void *lib)
{
    int bad = 0;
    uint8_t uuid[NMODES][VK_UUID_SIZE], u[VK_UUID_SIZE];

    /* Mesa's disk cache, in a directory of its own. */
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/vk_float_strict_cache.XXXXXX", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    if (!mkdtemp(dir)) {
        printf("  FAIL  mkdtemp\n");
        return 0;
    }
    setenv("MESA_SHADER_CACHE_DIR", dir, 1);
    unsetenv("MESA_SHADER_CACHE_DISABLE");
    static const enum mode seq[] = { FULL, FAST, FAST, NAN_, NAN_, FULL };
    static const int want[] = { 1, 1, 0, 1, 0, 0 };
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++) {
        int n = compiles(lib, seq[i], 0, NULL, 0, NULL, NULL, u);
        int fail = n != want[i];
        printf("  %s  disk cache, %s: %d shader(s) compiled%s\n", fail ? "FAIL" : "ok  ", mode_name[seq[i]], n,
               want[i] ? "" : " (cached in this mode before)");
        bad += fail;
    }
    nftw(dir, rm_entry, 16, FTW_DEPTH | FTW_PHYS);
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);

    /* An application's VkPipelineCache saved in one mode, loaded in the
     * others: only the mode it was saved in may use it. */
    void *data = NULL;
    size_t size = 0;
    int n = compiles(lib, FULL, 1, NULL, 0, &data, &size, uuid[FULL]);
    printf("  %s  pipeline cache saved in full: %d shader(s) compiled, %zu bytes\n", n == 1 ? "ok  " : "FAIL", n,
           size);
    bad += n != 1;
    for (int m = NAN_; m < NMODES; m++) {
        n = compiles(lib, m, 1, data, size, NULL, NULL, uuid[m]);
        int fail = n != 1 || !memcmp(uuid[m], uuid[FULL], VK_UUID_SIZE);
        printf("  %s  pipeline cache loaded in %s: %d shader(s) compiled, pipelineCacheUUID %s\n",
               fail ? "FAIL" : "ok  ", mode_name[m], n,
               memcmp(uuid[m], uuid[FULL], VK_UUID_SIZE) ? "its own" : "the same as full's");
        bad += fail;
    }
    n = compiles(lib, FULL, 1, data, size, NULL, NULL, u);
    printf("  %s  pipeline cache loaded in full: %d shader(s) compiled\n", n == 0 ? "ok  " : "FAIL", n);
    bad += n != 0;
    printf("        pipelineCacheUUID in full:");
    for (unsigned i = 0; i < VK_UUID_SIZE; i++)
        printf(" %02x", uuid[FULL][i]);
    printf("\n");
    free(data);
    return !bad;
}

int
main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: vk_float_strict KOSMICKRISP_DYLIB\n");
        return 2;
    }
    /* Every shader compiled (no disk cache) and printed. */
    setenv("MESA_KK_DEBUG", "msl", 1);
    setenv("MESA_SHADER_CACHE_DISABLE", "true", 1);
    void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        printf("dlopen: %s\n== vk_float_strict: FAIL\n", dlerror());
        return 1;
    }
    int ok = 1;
    double ms[NMODES];
    for (int m = 0; m < NMODES; m++)
        if (!run_mode(lib, m, &ms[m])) {
            printf("== vk_float_strict: FAIL\n");
            return 1;
        }
    ok &= check_msl();
    ok &= check_caches(lib);
    printf("        bench.comp, %d threads x %d iterations: full %.2f ms, nan %.2f ms, fast %.2f ms\n",
           BENCH_THREADS, BENCH_ITERATIONS, ms[FULL], ms[NAN_], ms[FAST]);
    printf("== vk_float_strict: %s\n", ok ? "ok" : "FAIL");
    return !ok;
}
