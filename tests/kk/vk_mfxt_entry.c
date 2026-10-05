// kk_steamarm_upscale_temporal, the entry SteamARM's Vulkan shim calls in
// SteamARM's KosmicKrisp (patches/kosmickrisp-20-metalfx-temporal-entry.patch),
// called the way the shim calls it: a native macOS program that loads the
// driver itself (no loader, no shim), records ordinary Vulkan commands and
// hands it struct sa_mfxt_cmd (shim/mfx_temporal.h, the shim's own header).
//
// The picture: a horizontal ramp (0.25 .. 0.75) at 960x540, still, enlarged
// to 1920x1080 by MetalFX over 8 frames; the output read back must be the
// ramp at the output's size (RMSE < 0.02 away from the borders).
//
// Checks: PROBE (status, caps_out, driver_out, max_scale_out written into
// the caller's copy); ENCODE with the output read back in the same command
// buffer; ENCODE as the last command of a command buffer, read in the next
// one (kk_mfxt_end's tail); a command buffer without ONE_TIME_SUBMIT
// submitted twice (recorded again by the driver: the upscale replayed);
// DESTROY; and the refusals with their status: inside a render pass, a
// secondary command buffer, bad magic / version / size, more than 3x, motion
// vectors missing, a view format the image cannot have, a handle that is not
// an image.
//
// Usage: tests/kk/run.sh (builds it; the driver from STEAMARM_KK_OWN, default
// ${STEAMARM_BUILD:-$HOME/SteamARM-build}/mesa-kk/out-test).
// Prints "== vk_mfxt_entry: ok", "== vk_mfxt_entry: skip (...)" or what failed.
#define VK_NO_PROTOTYPES
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include "../../shim/mfx_temporal.h"

enum { IW = 960, IH = 540, OW = 1920, OH = 1080 };

static int failures;
#define FAIL(...) do { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); failures++; } while (0)
#define OK(...) do { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s -> %d\n== vk_mfxt_entry: FAIL\n", #x, r_); exit(1); } } while (0)

typedef uint32_t (*entry_fn)(VkCommandBuffer, const struct sa_mfxt_cmd *);
static entry_fn entry;

#define FNS(X) \
    X(vkCreateInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DFNS(X) \
    X(vkGetDeviceQueue) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdClearColorImage) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkResetCommandBuffer)
#define DECL(n) static PFN_##n n;
FNS(DECL)
DFNS(DECL)

static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue queue;
static VkCommandPool pool;
static VkImage color, depth, motion, output;
static VkImageView output_view;
static VkBuffer up, down;
static void *up_map, *down_map;

static uint32_t
mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

static VkImage
make_image(VkFormat f, uint32_t w, uint32_t h, VkImageUsageFlags usage)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, f, { w, h, 1 }, 1, 1,
                              VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage };
    VkImage img;
    CHECK(vkCreateImage(dev, &ici, 0, &img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mem_type(mr.memoryTypeBits, 0) };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, 0, &mem));
    CHECK(vkBindImageMemory(dev, img, mem, 0));
    return img;
}

static VkBuffer
make_buffer(VkDeviceSize size, void **map)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, 0, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
                                 mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, 0, &mem));
    CHECK(vkBindBufferMemory(dev, buf, mem, 0));
    CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, map));
    return buf;
}

/* What the FFX DLL records around the command: all commands, writes, to
 * all commands, reads and writes. */
static void
barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, 0, 0, 0);
}

static VkCommandBuffer
new_cb(VkCommandBufferLevel level)
{
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool, level, 1 };
    VkCommandBuffer cb;
    CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
    return cb;
}

static void
begin(VkCommandBuffer cb, VkCommandBufferUsageFlags flags)
{
    VkCommandBufferInheritanceInfo inh = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0, flags, &inh };
    CHECK(vkBeginCommandBuffer(cb, &bi));
}

static void
submit(VkCommandBuffer *cbs, uint32_t n)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, n, cbs };
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
}

static void
copy_out(VkCommandBuffer cb)
{
    VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { OW, OH, 1 } };
    vkCmdCopyImageToBuffer(cb, output, VK_IMAGE_LAYOUT_GENERAL, down, 1, &r);
}

static void
clear_out(VkCommandBuffer cb)
{
    VkClearColorValue zero = { { 0, 0, 0, 0 } };
    VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(cb, output, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &rr);
}

/* RMSE of the read back output against the ramp; *mean its mean. */
static double
ramp_rmse(double *mean)
{
    const __fp16 *o = down_map;
    double se = 0, sum = 0;
    long n = 0;
    for (int y = 16; y < OH - 16; y += 2)
        for (int x = 16; x < OW - 16; x += 2) {
            double want = 0.25 + 0.5 * (x + 0.5) / OW;
            double v = o[(y * OW + x) * 4];
            se += (v - want) * (v - want);
            sum += v;
            n++;
        }
    *mean = sum / n;
    return sqrt(se / n);
}

static struct sa_mfxt_cmd
cmd_for(uint64_t context, int frame)
{
    struct sa_mfxt_cmd c;
    memset(&c, 0, sizeof(c));
    c.magic = SA_MFXT_MAGIC;
    c.version = SA_MFXT_VERSION;
    c.size = sizeof(c);
    c.op = SA_OP_ENCODE;
    c.context = context;
    c.color = (struct sa_mfxt_tex){ (uint64_t)(uintptr_t)color, VK_FORMAT_R16G16B16A16_SFLOAT, IW, IH, 0, 0, 0 };
    c.depth = (struct sa_mfxt_tex){ (uint64_t)(uintptr_t)depth, VK_FORMAT_D32_SFLOAT, IW, IH, 0, 0, 0 };
    c.motion = (struct sa_mfxt_tex){ (uint64_t)(uintptr_t)motion, VK_FORMAT_R16G16_SFLOAT, IW, IH, 0, 0, 0 };
    c.output = (struct sa_mfxt_tex){ (uint64_t)(uintptr_t)output, VK_FORMAT_R16G16B16A16_SFLOAT, OW, OH, 0, 0, 0 };
    c.render_w = IW;
    c.render_h = IH;
    c.upscale_w = OW;
    c.upscale_h = OH;
    c.mv_scale_x = c.mv_scale_y = 1.0f;
    c.pre_exposure = 1.0f;
    c.flags = frame == 0 ? SA_MFXT_RESET : 0;
    return c;
}

/* Frames of the upscale, each in a one-time command buffer; the last reads
 * the output back in the same command buffer. */
static uint32_t
run_frames(uint64_t context, int frames, double *err, double *mean)
{
    uint32_t st = 0;
    for (int f = 0; f < frames; f++) {
        VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin(cb, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        barrier(cb);
        struct sa_mfxt_cmd c = cmd_for(context, f);
        st = entry(cb, &c);
        barrier(cb);
        if (f == frames - 1) copy_out(cb);
        CHECK(vkEndCommandBuffer(cb));
        submit(&cb, 1);
        if (st != SA_ST_OK) break;
    }
    *err = ramp_rmse(mean);
    return st;
}

static uint32_t
encode_once(struct sa_mfxt_cmd *c)
{
    VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
    begin(cb, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
    uint32_t st = entry(cb, c);
    CHECK(vkEndCommandBuffer(cb));
    submit(&cb, 1);
    return st;
}

int
main(void)
{
    const char *own = getenv("STEAMARM_KK_OWN");
    char path[1024];
    snprintf(path, sizeof(path), "%s/libvulkan_kosmickrisp.dylib", own ? own : ".");
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen %s: %s\n== vk_mfxt_entry: FAIL\n", path, dlerror()); return 1; }
    entry = (entry_fn)dlsym(h, SA_MFXT_KK_ENTRY);
    if (!entry) { printf("== vk_mfxt_entry: FAIL (no %s in %s)\n", SA_MFXT_KK_ENTRY, path); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    int32_t (*neg)(uint32_t *) = (int32_t (*)(uint32_t *))dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    uint32_t iv = 7;
    if (neg) neg(&iv);
    vkCreateInstance = (PFN_vkCreateInstance)gipa(0, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_mfxt_entry", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, 0, &inst));
#define LOADI(n) n = (PFN_##n)gipa(inst, #n);
    FNS(LOADI)
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("== vk_mfxt_entry: %s (%s)\n", props.deviceName, path);
    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f13, 0, 1, &q };
    CHECK(vkCreateDevice(pd, &dci, 0, &dev));
#define LOADD(n) n = (PFN_##n)vkGetDeviceProcAddr(dev, #n);
    DFNS(LOADD)
    vkGetDeviceQueue(dev, 0, 0, &queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, 0 };
    CHECK(vkCreateCommandPool(dev, &pci, 0, &pool));

    /* PROBE, as the DLL sends it at context creation. */
    {
        struct sa_mfxt_cmd c = cmd_for(1, 0);
        c.op = SA_OP_PROBE;
        VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin(cb, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        uint32_t st = entry(cb, &c);
        CHECK(vkEndCommandBuffer(cb));
        if (st == SA_ST_UNSUPPORTED) { printf("== vk_mfxt_entry: skip (no MetalFX temporal scaler)\n"); return 0; }
        if (st != SA_ST_OK || !(c.caps_out & 1) || c.driver_out != VK_DRIVER_ID_MESA_KOSMICKRISP || c.max_scale_out < 2.9f)
            FAIL("probe: status %u caps 0x%x driver %u max scale %.2f", st, c.caps_out, c.driver_out, c.max_scale_out);
        else
            OK("probe: status OK, caps 0x%x, driver %u, max scale %.2f", c.caps_out, c.driver_out, c.max_scale_out);
    }

    /* The images as vkd3d-proton makes FSR's: render targets that are
     * sampled, a UAV output (storage). */
    color = make_image(VK_FORMAT_R16G16B16A16_SFLOAT, IW, IH,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    depth = make_image(VK_FORMAT_D32_SFLOAT, IW, IH,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    motion = make_image(VK_FORMAT_R16G16_SFLOAT, IW, IH,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    output = make_image(VK_FORMAT_R16G16B16A16_SFLOAT, OW, OH,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, 0, 0, output, VK_IMAGE_VIEW_TYPE_2D,
                                  VK_FORMAT_R16G16B16A16_SFLOAT, { 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CHECK(vkCreateImageView(dev, &vci, 0, &output_view));
    up = make_buffer(IW * IH * 8, &up_map);
    down = make_buffer(OW * OH * 8, &down_map);
    {   /* The ramp, depth 0.5, motion 0, output 0. */
        VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        __fp16 *c = up_map;
        for (int y = 0; y < IH; y++)
            for (int x = 0; x < IW; x++) {
                __fp16 *p = &c[(y * IW + x) * 4];
                p[0] = p[1] = p[2] = 0.25f + 0.5f * (x + 0.5f) / IW;
                p[3] = 1;
            }
        begin(cb, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        VkBufferImageCopy r = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { IW, IH, 1 } };
        vkCmdCopyBufferToImage(cb, up, color, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        CHECK(vkEndCommandBuffer(cb));
        submit(&cb, 1);
        float *d = up_map;
        for (int i = 0; i < IW * IH; i++) d[i] = 0.5f;
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin(cb, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        vkCmdCopyBufferToImage(cb, up, depth, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        CHECK(vkEndCommandBuffer(cb));
        submit(&cb, 1);
        memset(up_map, 0, IW * IH * 4);
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin(cb, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        r.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vkCmdCopyBufferToImage(cb, up, motion, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
        clear_out(cb);
        CHECK(vkEndCommandBuffer(cb));
        submit(&cb, 1);
    }

    /* ENCODE, read back in the same command buffer. */
    double err, mean;
    uint32_t st = run_frames(1, 8, &err, &mean);
    if (st != SA_ST_OK || err > 0.02)
        FAIL("encode: status %u, ramp RMSE %.4f (mean %.3f)", st, err, mean);
    else
        OK("encode: 960x540 -> 1920x1080, ramp RMSE %.4f (mean %.3f)", err, mean);

    /* The upscale the last command of its command buffer; read in the next
     * command buffer of the same submission. */
    {
        memset(down_map, 0, OW * OH * 8);
        VkCommandBuffer cbs[2] = { new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY), new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY) };
        begin(cbs[0], VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        clear_out(cbs[0]);
        barrier(cbs[0]);
        struct sa_mfxt_cmd c = cmd_for(1, 1);
        st = entry(cbs[0], &c);
        CHECK(vkEndCommandBuffer(cbs[0]));
        begin(cbs[1], VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        barrier(cbs[1]);
        copy_out(cbs[1]);
        CHECK(vkEndCommandBuffer(cbs[1]));
        submit(cbs, 2);
        err = ramp_rmse(&mean);
        if (st != SA_ST_OK || err > 0.02)
            FAIL("upscale last in its command buffer: status %u, read in the next one RMSE %.4f", st, err);
        else
            OK("upscale last in its command buffer: read in the next one, RMSE %.4f", err);
    }

    /* A command buffer submitted twice: recorded again, the upscale too. */
    {
        VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin(cb, 0);
        clear_out(cb);
        barrier(cb);
        struct sa_mfxt_cmd c = cmd_for(1, 1);
        st = entry(cb, &c);
        barrier(cb);
        copy_out(cb);
        CHECK(vkEndCommandBuffer(cb));
        memset(down_map, 0, OW * OH * 8);
        submit(&cb, 1);
        double e1 = ramp_rmse(&mean);
        memset(down_map, 0, OW * OH * 8);
        submit(&cb, 1);
        double e2 = ramp_rmse(&mean);
        if (st != SA_ST_OK || e1 > 0.02 || e2 > 0.02)
            FAIL("submitted twice: status %u, RMSE %.4f then %.4f", st, e1, e2);
        else
            OK("submitted twice (recorded again by the driver): RMSE %.4f then %.4f", e1, e2);
    }

    /* Refusals. */
    {
        struct { const char *name; uint32_t want; struct sa_mfxt_cmd c; } r[8];
        unsigned k = 0;
        r[k].name = "bad magic"; r[k].want = SA_ST_BAD_ARGS; r[k].c = cmd_for(2, 0); r[k].c.magic ^= 1; k++;
        r[k].name = "bad version"; r[k].want = SA_ST_BAD_ARGS; r[k].c = cmd_for(2, 0); r[k].c.version = 2; k++;
        r[k].name = "bad size"; r[k].want = SA_ST_BAD_ARGS; r[k].c = cmd_for(2, 0); r[k].c.size = 316; k++;
        r[k].name = "motion vectors missing"; r[k].want = SA_ST_BAD_ARGS; r[k].c = cmd_for(2, 0); r[k].c.motion.image = 0; k++;
        r[k].name = "638x358 -> 1920x1080"; r[k].want = SA_ST_UNSUPPORTED; r[k].c = cmd_for(2, 0);
        r[k].c.render_w = 638; r[k].c.render_h = 358; k++;
        r[k].name = "a view format a non-mutable image cannot have"; r[k].want = SA_ST_UNSUPPORTED; r[k].c = cmd_for(2, 0);
        r[k].c.color.vk_format = VK_FORMAT_R16G16B16A16_UNORM; k++;
        r[k].name = "a buffer handle for the colour"; r[k].want = SA_ST_BAD_ARGS; r[k].c = cmd_for(2, 0);
        r[k].c.color.image = (uint64_t)(uintptr_t)down; k++;
        r[k].name = "unknown op"; r[k].want = SA_ST_BAD_ARGS; r[k].c = cmd_for(2, 0); r[k].c.op = 9; k++;
        for (unsigned i = 0; i < k; i++) {
            st = encode_once(&r[i].c);
            if (st != r[i].want) FAIL("%s: status %u, want %u", r[i].name, st, r[i].want);
            else OK("%s: status %u", r[i].name, st);
        }

        /* Inside a render pass. */
        VkCommandBuffer cb = new_cb(VK_COMMAND_BUFFER_LEVEL_PRIMARY);
        begin(cb, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT);
        VkRenderingAttachmentInfo att = { VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, 0, output_view, VK_IMAGE_LAYOUT_GENERAL };
        att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri = { VK_STRUCTURE_TYPE_RENDERING_INFO, 0, 0, { { 0, 0 }, { OW, OH } }, 1, 0, 1, &att };
        vkCmdBeginRendering(cb, &ri);
        struct sa_mfxt_cmd c = cmd_for(2, 0);
        st = entry(cb, &c);
        vkCmdEndRendering(cb);
        CHECK(vkEndCommandBuffer(cb));
        submit(&cb, 1);
        if (st != SA_ST_BAD_ARGS) FAIL("inside a render pass: status %u, want %u", st, SA_ST_BAD_ARGS);
        else OK("inside a render pass: status %u", st);

        /* A secondary command buffer. */
        cb = new_cb(VK_COMMAND_BUFFER_LEVEL_SECONDARY);
        begin(cb, 0);
        st = entry(cb, &c);
        CHECK(vkEndCommandBuffer(cb));
        if (st != SA_ST_UNSUPPORTED) FAIL("secondary command buffer: status %u, want %u", st, SA_ST_UNSUPPORTED);
        else OK("secondary command buffer: status %u", st);
    }

    /* DESTROY, and the context made again afterwards. */
    {
        struct sa_mfxt_cmd c = cmd_for(1, 0);
        c.op = SA_OP_DESTROY;
        st = encode_once(&c);
        double e;
        uint32_t st2 = run_frames(1, 8, &e, &mean);
        if (st != SA_ST_OK || st2 != SA_ST_OK || e > 0.02)
            FAIL("destroy: status %u, then encode %u RMSE %.4f", st, st2, e);
        else
            OK("destroy: status OK; the context made again: RMSE %.4f", e);
    }

    printf(failures ? "== vk_mfxt_entry: FAIL (%d)\n" : "== vk_mfxt_entry: ok\n", failures);
    return failures != 0;
}
