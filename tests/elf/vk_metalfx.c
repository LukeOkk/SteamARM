// MetalFX between two Vulkan submissions, as the shim's scaler does it
// (shim/scaler.c, LXRT_VK_SCALER=metalfx|auto), without a window: a native
// program on the Vulkan shim that
//   1. clears a 960x540 B8G8R8A8 picture red on the left, blue on the right,
//      and signals a timeline semaphore to 1;
//   2. exports the picture, a 1920x1080 output image and the semaphore's
//      MTLSharedEvent (VK_EXT_metal_objects: MoltenVK, and SteamARM's
//      KosmicKrisp with patches/kosmickrisp-10-metal-objects.patch) and asks
//      the runtime (runtime/metalfx.m) for Apple's spatial scaler, waiting
//      for 1 and signalling 2 -- on the driver's MTLCommandQueue where it
//      exports one (MoltenVK), on a queue of the runtime's own where it does
//      not (KosmicKrisp: its queue is an MTL4CommandQueue);
//   3. copies the output out in a submission that waits for 2.
// Pixels (1/4 and 3/4 across at mid height): red and blue. Then the same
// three steps 300 times, and the time per frame (the whole chain, CPU and
// GPU, as at a present), against one bilinear vkCmdBlitImage of the same
// sizes.
// VK_METALFX_TEMPORAL=1: the temporal scaler (runtime/metalfx.m), same checks.
// Prints "== vk_metalfx: ok", "== vk_metalfx: skip (...)" (a driver without
// VK_EXT_metal_objects) or the first thing that is wrong.
#define VK_USE_PLATFORM_METAL_EXT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <vulkan/vulkan.h>
#include "lxrt_host.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s -> %d\n== vk_metalfx: FAIL\n", #x, r_); return 1; } } while (0)
enum { IW = 960, IH = 540, OW = 1920, OH = 1080, RUNS = 300 };

struct lxrt_mfx_run {
    void *scaler, *queue, *in, *out, *event;
    uint64_t wait, signal;
    int mode;           // 0 spatial, 1 temporal (VK_METALFX_TEMPORAL=1)
};

static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue q;
static VkCommandPool pool;

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return 0;
}

static int make_image(uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImage *img)
{
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_B8G8R8A8_UNORM,
                              { w, h, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage };
    CHECK(vkCreateImage(dev, &ici, 0, img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, *img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
                                 mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, 0, &mem));
    CHECK(vkBindImageMemory(dev, *img, mem, 0));
    return 0;
}

static void barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, 0, VK_ACCESS_MEMORY_WRITE_BIT,
                               VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, from, to,
                               VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img,
                               { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0, 0, 1, &b);
}

static VkCommandBuffer new_cb(void)
{
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb = 0;
    vkAllocateCommandBuffers(dev, &ai, &cb);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cb, &bi);
    return cb;
}

// One submission of `cb` (may be 0), waiting for tl >= wait (0: nothing) and
// signalling tl = signal (0: nothing), with `fence`.
static VkResult submit(VkCommandBuffer cb, VkSemaphore tl, uint64_t wait, uint64_t signal, VkFence fence)
{
    VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo ts = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, 0, wait ? 1 : 0, &wait,
                                         signal ? 1 : 0, &signal };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, wait ? 1 : 0, &tl, &st, cb ? 1 : 0, &cb,
                        signal ? 1 : 0, &tl };
    return vkQueueSubmit(q, 1, &si, fence);
}

static double now_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_metalfx", 1, 0, 0, VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { printf("no device\n== vk_metalfx: FAIL\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s\n", props.deviceName);
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, 0, &ne, 0);
    VkExtensionProperties ext[512];
    if (ne > 512) ne = 512;
    vkEnumerateDeviceExtensionProperties(pd, 0, &ne, ext);
    int have = 0;
    for (uint32_t i = 0; i < ne; i++)
        have |= !strcmp(ext[i].extensionName, VK_EXT_METAL_OBJECTS_EXTENSION_NAME);
    if (!have) { printf("== vk_metalfx: skip (no VK_EXT_metal_objects)\n"); return 0; }

    float prio = 1;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    f12.timelineSemaphore = VK_TRUE;
    const char *exts[] = { VK_EXT_METAL_OBJECTS_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f12, 0, 1, &qci, 0, 0, 1, exts };
    CHECK(vkCreateDevice(pd, &dci, 0, &dev));
    vkGetDeviceQueue(dev, 0, 0, &q);
    PFN_vkExportMetalObjectsEXT export_fn = (PFN_vkExportMetalObjectsEXT)vkGetDeviceProcAddr(dev, "vkExportMetalObjectsEXT");
    if (!export_fn) { printf("vkGetDeviceProcAddr(vkExportMetalObjectsEXT) = NULL\n== vk_metalfx: FAIL\n"); return 1; }
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, 0 };
    CHECK(vkCreateCommandPool(dev, &pci, 0, &pool));

    // The shim's usages: the game's image (+ SAMPLED, TRANSFER_SRC), and
    // the scaler's output (scaler.c setup_mfx).
    VkImage in, out;
    if (make_image(IW, IH, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &in) ||
        make_image(OW, OH, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &out))
        return 1;
    // Staging (blue, for the right half) and read-back buffer.
    VkBuffer buf;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, (VkDeviceSize)OW * OH * 4,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    CHECK(vkCreateBuffer(dev, &bci, 0, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size,
                                 mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory bmem;
    CHECK(vkAllocateMemory(dev, &mai, 0, &bmem));
    CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
    uint8_t *map;
    CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&map));
    for (uint32_t i = 0; i < IW / 2 * IH; i++) {
        map[4 * i + 0] = 255; map[4 * i + 1] = 0; map[4 * i + 2] = 0; map[4 * i + 3] = 255;   // B G R A: blue
    }

    VkExportMetalObjectCreateInfoEXT ex = { VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECT_CREATE_INFO_EXT, 0,
                                            VK_EXPORT_METAL_OBJECT_TYPE_METAL_SHARED_EVENT_BIT_EXT };
    VkSemaphoreTypeCreateInfo ty = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, &ex, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ty };
    VkSemaphore tl;
    CHECK(vkCreateSemaphore(dev, &sci, 0, &tl));

    VkExportMetalTextureInfoEXT t_in = { VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT, 0, in, 0, 0,
                                         VK_IMAGE_ASPECT_PLANE_0_BIT, 0 };
    VkExportMetalTextureInfoEXT t_out = { VK_STRUCTURE_TYPE_EXPORT_METAL_TEXTURE_INFO_EXT, &t_in, out, 0, 0,
                                          VK_IMAGE_ASPECT_PLANE_0_BIT, 0 };
    VkExportMetalSharedEventInfoEXT ev = { VK_STRUCTURE_TYPE_EXPORT_METAL_SHARED_EVENT_INFO_EXT, &t_out, tl, 0, 0 };
    VkExportMetalCommandQueueInfoEXT eq = { VK_STRUCTURE_TYPE_EXPORT_METAL_COMMAND_QUEUE_INFO_EXT, &ev, q, 0 };
    VkExportMetalDeviceInfoEXT ed = { VK_STRUCTURE_TYPE_EXPORT_METAL_DEVICE_INFO_EXT, &eq, 0 };
    VkExportMetalObjectsInfoEXT o = { VK_STRUCTURE_TYPE_EXPORT_METAL_OBJECTS_INFO_EXT, &ed };
    export_fn(dev, &o);
    printf("exported: device %p queue %p in %p out %p event %p\n", (void *)ed.mtlDevice, (void *)eq.mtlCommandQueue,
           (void *)t_in.mtlTexture, (void *)t_out.mtlTexture, (void *)ev.mtlSharedEvent);
    if (!ed.mtlDevice || !t_in.mtlTexture || !t_out.mtlTexture || !ev.mtlSharedEvent) {
        printf("an object was not exported\n== vk_metalfx: FAIL\n");
        return 1;
    }

    // 1. The picture: red, then blue into its right half.
    VkCommandBuffer a = new_cb();
    barrier(a, in, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue red = { { 1.0f, 0.0f, 0.0f, 1.0f } };
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    vkCmdClearColorImage(a, in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &red, 1, &all);
    barrier(a, in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy half = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { IW / 2, 0, 0 }, { IW / 2, IH, 1 } };
    vkCmdCopyBufferToImage(a, buf, in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &half);
    barrier(a, in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    barrier(a, out, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vkEndCommandBuffer(a);
    CHECK(submit(a, tl, 0, 1, VK_NULL_HANDLE));

    // 2. MetalFX: waits for 1, signals 2.
    struct lxrt_mfx_run run = { 0, (void *)eq.mtlCommandQueue, (void *)t_in.mtlTexture, (void *)t_out.mtlTexture,
                                (void *)ev.mtlSharedEvent, 1, 2,
                                getenv("VK_METALFX_TEMPORAL") && *getenv("VK_METALFX_TEMPORAL") == '1' };
    printf("mode: %s\n", run.mode ? "temporal" : "spatial");
    long e = lxrt_syscall2(LXRT_NR_MFX_ENCODE, (long)(uintptr_t)&run, 0);
    printf("runtime encode -> %ld (queue: %s)\n", e, eq.mtlCommandQueue ? "the driver's" : "the runtime's own");
    if (e) { printf("== vk_metalfx: FAIL\n"); return 1; }

    // 3. Out, after 2.
    VkCommandBuffer b = new_cb();
    barrier(b, out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy whole = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { OW, OH, 1 } };
    vkCmdCopyImageToBuffer(b, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &whole);
    barrier(b, out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
    vkEndCommandBuffer(b);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, 0, &fence));
    CHECK(submit(b, tl, 2, 0, fence));
    VkResult w = vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull);
    if (w != VK_SUCCESS) { printf("the copy did not finish (%d)\n== vk_metalfx: FAIL\n", w); return 1; }
    const uint8_t *l = map + 4 * ((size_t)OH / 2 * OW + OW / 4), *r = map + 4 * ((size_t)OH / 2 * OW + 3 * OW / 4);
    const uint8_t *edge = map + 4 * ((size_t)OH / 2 * OW + OW / 2);
    printf("pixels (B,G,R,A): left %u,%u,%u,%u right %u,%u,%u,%u at the edge %u,%u,%u,%u\n", l[0], l[1], l[2], l[3],
           r[0], r[1], r[2], r[3], edge[0], edge[1], edge[2], edge[3]);
    if (!(l[2] > 200 && l[0] < 40 && r[0] > 200 && r[2] < 40)) {
        printf("expected red on the left, blue on the right\n== vk_metalfx: FAIL\n");
        return 1;
    }

    // Time: the three steps per frame, RUNS times, as the shim's presents
    // run them: three frames in flight, each with command buffers of its own
    // that are reused only once its fence says the GPU is done with them
    // (the picture is not redrawn: steps 1 and 3 are one barrier each).
    // Then the same with a bilinear vkCmdBlitImage in place of MetalFX.
    enum { SLOTS = 3 };
    VkCommandBuffer ca[SLOTS], cbo[SLOTS], cbl[SLOTS];
    VkFence fs[SLOTS];
    int busy[SLOTS] = { 0 };
    VkImageBlit reg = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { IW, IH, 1 } },
                        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { OW, OH, 1 } } };
    for (int k = 0; k < SLOTS; k++) {
        ca[k] = new_cb();
        barrier(ca[k], in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        vkEndCommandBuffer(ca[k]);
        cbo[k] = new_cb();
        barrier(cbo[k], out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        vkEndCommandBuffer(cbo[k]);
        cbl[k] = new_cb();
        barrier(cbl[k], in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        barrier(cbl[k], out, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        vkCmdBlitImage(cbl[k], in, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &reg, VK_FILTER_LINEAR);
        barrier(cbl[k], in, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        barrier(cbl[k], out, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        vkEndCommandBuffer(cbl[k]);
        CHECK(vkCreateFence(dev, &fci, 0, &fs[k]));
    }
    uint64_t v = 2;
    double ms[2];
    for (int pass = 0; pass < 2; pass++) {
        double t0 = now_ms();
        for (int i = 0; i < RUNS; i++) {
            int k = i % SLOTS;
            if (busy[k]) {
                if (vkWaitForFences(dev, 1, &fs[k], VK_TRUE, 5000000000ull) != VK_SUCCESS) {
                    printf("frame %d of pass %d did not finish\n== vk_metalfx: FAIL\n", i, pass);
                    return 1;
                }
                vkResetFences(dev, 1, &fs[k]);
            }
            if (pass == 0) {
                CHECK(submit(ca[k], tl, 0, v + 1, VK_NULL_HANDLE));
                run.wait = v + 1;
                run.signal = v + 2;
                if (lxrt_syscall2(LXRT_NR_MFX_ENCODE, (long)(uintptr_t)&run, 0)) {
                    printf("== vk_metalfx: FAIL\n");
                    return 1;
                }
                CHECK(submit(cbo[k], tl, v + 2, 0, fs[k]));
            } else {
                CHECK(submit(ca[k], tl, 0, v + 1, VK_NULL_HANDLE));
                CHECK(submit(cbl[k], tl, v + 1, v + 2, fs[k]));
            }
            busy[k] = 1;
            v += 2;
        }
        for (int k = 0; k < SLOTS; k++)
            if (busy[k] && vkWaitForFences(dev, 1, &fs[k], VK_TRUE, 5000000000ull) != VK_SUCCESS) {
                printf("pass %d did not finish\n== vk_metalfx: FAIL\n", pass);
                return 1;
            }
        ms[pass] = (now_ms() - t0) / RUNS;
    }
    double mfx = ms[0], bl = ms[1];
    printf("per frame, %ux%u -> %ux%u: MetalFX %.3f ms, bilinear blit %.3f ms (%d frames each)\n", IW, IH, OW, OH, mfx,
           bl, RUNS);
    lxrt_syscall2(LXRT_NR_MFX_RELEASE, (long)(uintptr_t)run.scaler, 0);
    printf("== vk_metalfx: ok\n");
    return 0;
}
