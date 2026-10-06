// DLSS -> MetalFX through SteamARM's nvngx.dll (tools/dlss-metalfx), the
// Vulkan side: the way a Vulkan game uses NVIDIA's NGX (NVSDK_NGX_VULKAN_*),
// end to end from a Windows program: vulkan-1.dll -> winevulkan -> FEX's
// Vulkan thunks -> the Vulkan shim -> KosmicKrisp (MetalFX) or MoltenVK.
//
//   NVSDK_NGX_VULKAN_RequiredExtensions (none), Init_with_ProjectID with the
//   instance, physical device, device and the loader's two GetProcAddr,
//   GetCapabilityParameters (SuperSampling.Available), AllocateParameters,
//   CreateFeature1 (SuperSampling, 1280x720 -> 1920x1080) on a recording
//   command buffer, EvaluateFeature with the inputs as NVSDK_NGX_Resource_VK
//   (colour RGBA16F, depth D32, motion RG16F, output RGBA16F storage) and the
//   picture, jitter and motion vectors of tests/win/d3d12_dlss.c for N
//   frames, the output read back (RMSE against the supersampled scene);
//   ReleaseFeature, Shutdown1. Then DLSS 1: Init as a 2019 game (SDK 0x12),
//   SuperSampling.Available = 1 on either driver, a feature evaluated with
//   Colour, Output and Sharpness only: the shim's SPATIAL (stats path 1,
//   status OK) and the upscaled picture on the output.
//
// Arguments: frames=N (default 40), expect=available|unavailable (DLSS 2+:
// 0 on MoltenVK and with KK_MFXT=0, where CreateFeature1 must refuse).
// Built and run by tests/win/run_d3d12_dlss.sh (modes vk, vkmvk).
// Prints "== vk dlss: ok", "...: FAIL (n)" or "...: FAIL (why)".
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "../../shim/mfx_temporal.h"

enum { IW = 1280, IH = 720, OW = 1920, OH = 1080, MEASURE = 4 };

#define NGX_OK 0x1u
#define NGX_FAIL_FeatureNotSupported 0xBAD00001u
#define NGX_Feature_SuperSampling 1
#define NGX_DLSS_MVLowRes 2
#define Q_MaxQuality 2

/* NVSDK_NGX_Resource_VK (nvsdk_ngx_defs_vk.h). */
struct ngx_image_view_info { VkImageView ImageView; VkImage Image; VkImageSubresourceRange SubresourceRange; VkFormat Format; unsigned Width, Height; };
struct ngx_resource_vk { union { struct ngx_image_view_info ImageViewInfo; struct { VkBuffer Buffer; unsigned SizeInBytes; } BufferInfo; } Resource; int Type; unsigned char ReadWrite; };

typedef unsigned (*pfn_req_ext)(unsigned *, const char ***, unsigned *, const char ***);
typedef unsigned (*pfn_init_pid)(const char *, int, const char *, const wchar_t *, VkInstance, VkPhysicalDevice, VkDevice,
                                 PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, const void *, int);
typedef unsigned (*pfn_init)(unsigned long long, const wchar_t *, VkInstance, VkPhysicalDevice, VkDevice,
                             PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, const void *, int);
typedef unsigned (*pfn_params)(void **);
typedef unsigned (*pfn_destroy_params)(void *);
typedef unsigned (*pfn_create1)(VkDevice, VkCommandBuffer, int, void *, void **);
typedef unsigned (*pfn_release)(void *);
typedef unsigned (*pfn_evaluate)(VkCommandBuffer, const void *, void *, void *);
typedef unsigned (*pfn_shutdown1)(VkDevice);
typedef const char *(*pfn_ident)(void);
typedef int (*pfn_devinfo)(unsigned *);
typedef int (*pfn_stats)(const void *, unsigned *);
typedef void (*pfn_c_set_vp)(void *, const char *, void *);
typedef void (*pfn_c_set_ui)(void *, const char *, unsigned);
typedef void (*pfn_c_set_i)(void *, const char *, int);
typedef void (*pfn_c_set_f)(void *, const char *, float);
typedef unsigned (*pfn_c_get_i)(void *, const char *, int *);

static struct {
    HMODULE h;
    pfn_req_ext req_ext;
    pfn_init_pid init_pid;
    pfn_init init;
    pfn_params get_caps, alloc_params;
    pfn_destroy_params destroy_params;
    pfn_create1 create1;
    pfn_release release;
    pfn_evaluate evaluate;
    pfn_shutdown1 shutdown1;
    pfn_ident ident;
    pfn_devinfo devinfo;
    pfn_stats stats;
    pfn_c_set_vp set_vp;
    pfn_c_set_ui set_ui;
    pfn_c_set_i set_i;
    pfn_c_set_f set_f;
    pfn_c_get_i get_i;
} ngx;

static int failures;
#define FAILX(...) do { printf(__VA_ARGS__); printf("\n== vk dlss: FAIL\n"); fflush(stdout); ExitProcess(1); } while (0)
#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) FAILX(#call " -> %d", (int)r_); } while (0)
#define OK(...) do { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define BAD(...) do { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); failures++; } while (0)

static DWORD WINAPI watchdog(LPVOID arg)
{
    (void)arg;
    Sleep(55000);
    printf("== vk dlss: FAIL (watchdog: not done in 55 s)\n");
    fflush(stdout);
    ExitProcess(3);
    return 0;
}

static const char *status_name(uint32_t st)
{
    switch (st) {
    case SA_ST_UNSEEN: return "UNSEEN";
    case SA_ST_OK: return "OK";
    case SA_ST_NO_DRIVER: return "NO_DRIVER";
    case SA_ST_BAD_ARGS: return "BAD_ARGS";
    case SA_ST_UNSUPPORTED: return "UNSUPPORTED";
    case SA_ST_NO_SCALER: return "NO_SCALER";
    default: return "?";
    }
}

/* ---- the picture (tests/win/d3d12_dlss.c's) ---- */
static float dir[3][2];
static float sintab[4097];
static inline int ifloor(float v) { int i = (int)v; return (v < (float)i) ? i - 1 : i; }
static inline float sin2pi(float u)
{
    u -= (float)ifloor(u);
    float f = u * 4096.0f;
    int i = (int)f;
    float a = f - (float)i;
    return sintab[i] + a * (sintab[i + 1] - sintab[i]);
}
static void pattern_init(void)
{
    const float f[3] = { 0.31f, 0.58f, 0.72f }, a[3] = { 0.3f, 1.2f, 2.3f };
    for (int i = 0; i < 3; i++) { dir[i][0] = f[i] * cosf(a[i]); dir[i][1] = f[i] * sinf(a[i]); }
    for (int i = 0; i <= 4096; i++) sintab[i] = sinf(6.2831853f * (float)i / 4096.0f);
}
static inline float pattern(float x, float y)
{
    float v = 0.5f;
    v += 0.12f * sin2pi(x * dir[0][0] + y * dir[0][1]);
    v += 0.12f * sin2pi(x * dir[1][0] + y * dir[1][1]);
    v += 0.10f * sin2pi(x * dir[2][0] + y * dir[2][1]);
    float rx = (x * 0.9397f - y * 0.3420f) / 2.7f, ry = (x * 0.3420f + y * 0.9397f) / 2.7f;
    int c = (ifloor(rx) + ifloor(ry)) & 1;
    return v + ((float)c - 0.5f) * 0.12f;
}
static float halton(int i, int b)
{
    float f = 1, r = 0;
    while (i > 0) { f /= b; r += f * (i % b); i /= b; }
    return r;
}
static uint16_t f2h(float v)
{
    uint32_t x;
    memcpy(&x, &v, 4);
    uint32_t s = (x >> 16) & 0x8000;
    int e = (int)((x >> 23) & 0xff) - 127 + 15;
    uint32_t m = x & 0x7fffff;
    if (e <= 0) return (uint16_t)s;
    if (e >= 31) return (uint16_t)(s | 0x7c00);
    uint32_t h = s | ((uint32_t)e << 10) | (m >> 13);
    if (m & 0x1000) h++;
    return (uint16_t)h;
}
static float h2f(uint16_t h)
{
    uint32_t s = (uint32_t)(h & 0x8000) << 16, e = (h >> 10) & 0x1f, m = h & 0x3ff, x;
    if (e == 0) {
        if (!m) x = s;
        else { float f = ldexpf((float)m, -24); return s ? -f : f; }
    } else if (e == 31) x = s | 0x7f800000 | (m << 13);
    else x = s | ((e - 15 + 127) << 23) | (m << 13);
    float f;
    memcpy(&f, &x, 4);
    return f;
}

/* ---- Vulkan ---- */
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;
static VkCommandPool pool;
static VkCommandBuffer cb;
static VkFence fence;
static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkPhysicalDeviceMemoryProperties mp;

#define DEVFN(name) static PFN_##name name;
DEVFN(vkGetDeviceQueue) DEVFN(vkCreateBuffer) DEVFN(vkGetBufferMemoryRequirements) DEVFN(vkAllocateMemory)
DEVFN(vkBindBufferMemory) DEVFN(vkMapMemory) DEVFN(vkCreateCommandPool) DEVFN(vkAllocateCommandBuffers)
DEVFN(vkBeginCommandBuffer) DEVFN(vkEndCommandBuffer) DEVFN(vkCmdPipelineBarrier) DEVFN(vkCreateFence)
DEVFN(vkWaitForFences) DEVFN(vkResetFences) DEVFN(vkQueueSubmit) DEVFN(vkResetCommandBuffer) DEVFN(vkCreateImage)
DEVFN(vkGetImageMemoryRequirements) DEVFN(vkBindImageMemory) DEVFN(vkCreateImageView) DEVFN(vkCmdCopyBufferToImage)
DEVFN(vkCmdCopyImageToBuffer) DEVFN(vkDeviceWaitIdle)

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

struct tex {
    VkImage img;
    VkImageView view;
    VkDeviceMemory mem;
    VkFormat fmt;
    uint32_t w, h, bpp;
    VkImageAspectFlags aspect;
    struct ngx_resource_vk res;
};

static void make_tex(struct tex *t, VkFormat fmt, uint32_t w, uint32_t h, uint32_t bpp, VkImageUsageFlags usage, int depth)
{
    memset(t, 0, sizeof *t);
    t->fmt = fmt; t->w = w; t->h = h; t->bpp = bpp;
    t->aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
                             .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    CHECK(vkCreateImage(dev, &ii, NULL, &t->img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, t->img, &mr);
    uint32_t mt = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) mt = memory_type(mr.memoryTypeBits, 0);
    if (mt == UINT32_MAX) FAILX("no memory type for an image");
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    CHECK(vkAllocateMemory(dev, &mai, NULL, &t->mem));
    CHECK(vkBindImageMemory(dev, t->img, t->mem, 0));
    VkImageViewCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = t->img, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                 .format = fmt, .subresourceRange = { t->aspect, 0, 1, 0, 1 } };
    CHECK(vkCreateImageView(dev, &vi, NULL, &t->view));
    t->res.Type = 0;   /* NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW */
    t->res.ReadWrite = (usage & VK_IMAGE_USAGE_STORAGE_BIT) != 0;
    t->res.Resource.ImageViewInfo.ImageView = t->view;
    t->res.Resource.ImageViewInfo.Image = t->img;
    t->res.Resource.ImageViewInfo.SubresourceRange = vi.subresourceRange;
    t->res.Resource.ImageViewInfo.Format = fmt;
    t->res.Resource.ImageViewInfo.Width = w;
    t->res.Resource.ImageViewInfo.Height = h;
}

struct hostbuf { VkBuffer buf; VkDeviceMemory mem; unsigned char *map; VkDeviceSize size; };
static void make_hostbuf(struct hostbuf *b, VkDeviceSize size)
{
    b->size = size;
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    CHECK(vkCreateBuffer(dev, &bci, NULL, &b->buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, b->buf, &mr);
    uint32_t mt = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt == UINT32_MAX) FAILX("no host-visible memory");
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size, .memoryTypeIndex = mt };
    CHECK(vkAllocateMemory(dev, &mai, NULL, &b->mem));
    CHECK(vkBindBufferMemory(dev, b->buf, b->mem, 0));
    CHECK(vkMapMemory(dev, b->mem, 0, size, 0, (void **)&b->map));
}

static void fence_all(void)
{
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static void to_general(struct tex *t)
{
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
                               .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = t->img, .subresourceRange = { t->aspect, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void upload(struct hostbuf *src, VkDeviceSize off, struct tex *t)
{
    VkBufferImageCopy r = { .bufferOffset = off, .imageSubresource = { t->aspect, 0, 0, 1 }, .imageExtent = { t->w, t->h, 1 } };
    vkCmdCopyBufferToImage(cb, src->buf, t->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    fence_all();
}
static void begin(void)
{
    CHECK(vkResetCommandBuffer(cb, 0));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(vkBeginCommandBuffer(cb, &bi));
}
static void end_submit(void)
{
    CHECK(vkEndCommandBuffer(cb));
    CHECK(vkResetFences(dev, 1, &fence));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000) != VK_SUCCESS) FAILX("GPU did not finish in 10 s");
}

static struct tex color, depth, motion, output;
static struct hostbuf stage, readback;

struct run {
    const char *name;
    float jitter_mul, vx, vy, mv_mul;
    int frames, dlss1;   /* dlss1: Colour, Output, Sharpness only, no jitter in the input */
    float pscale;        /* the pattern's coordinates scaled by this (0: 1); DLSS 1 is judged at a quarter rate (d3d12_dlss.c) */
    unsigned rc_create, rc_eval, rc_release, stats[6];
    double err, mean;
};

static float pscale_of(const struct run *r) { return r->pscale > 0 ? r->pscale : 1.0f; }
static void write_color(const struct run *r, int t, float sx, float sy)
{
    const float s = pscale_of(r);
    for (uint32_t y = 0; y < IH; y++) {
        uint16_t *o = (uint16_t *)(stage.map + (size_t)y * IW * 8);
        float py = (float)y + 0.5f + sy - r->vy * (float)t;
        for (uint32_t x = 0; x < IW; x++) {
            float px = (float)x + 0.5f + sx - r->vx * (float)t;
            uint16_t v = f2h(pattern(px * s, py * s));
            o[x * 4] = o[x * 4 + 1] = o[x * 4 + 2] = v;
            o[x * 4 + 3] = 0x3c00;
        }
    }
}
static double rmse(const struct run *r, int t, double *mean)
{
    double se = 0, sum = 0;
    long n = 0;
    const float k = (float)IW / OW, s = pscale_of(r);
    for (int y = 16; y < OH - 16; y += 4) {
        const uint16_t *row = (const uint16_t *)(readback.map + (size_t)y * OW * 8);
        for (int x = 16; x < OW - 16; x += 4) {
            float ref = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                    ref += pattern((((float)x + (i + 0.5f) / 4) * k - r->vx * (float)t) * s,
                                   (((float)y + (j + 0.5f) / 4) * k - r->vy * (float)t) * s);
            ref /= 16;
            double v = h2f(row[x * 4]);
            se += (v - ref) * (v - ref);
            sum += v;
            n++;
        }
    }
    *mean = sum / n;
    return sqrt(se / n);
}

static void run_case(struct run *r)
{
    void *p = NULL, *h = NULL;
    double err = 0, mean = 0;
    int nerr = 0;
    r->err = -1;
    memset(r->stats, 0, sizeof r->stats);
    if (ngx.alloc_params(&p) != NGX_OK || !p) FAILX("AllocateParameters");
    ngx.set_ui(p, "CreationNodeMask", 1);
    ngx.set_ui(p, "VisibilityNodeMask", 1);
    ngx.set_ui(p, "Width", IW);
    ngx.set_ui(p, "Height", IH);
    ngx.set_ui(p, "OutWidth", OW);
    ngx.set_ui(p, "OutHeight", OH);
    ngx.set_i(p, "PerfQualityValue", Q_MaxQuality);
    ngx.set_i(p, "DLSS.Feature.Create.Flags", NGX_DLSS_MVLowRes);
    ngx.set_i(p, "DLSS.Enable.Output.Subrects", 0);
    begin();
    r->rc_create = ngx.create1(dev, cb, NGX_Feature_SuperSampling, p, &h);
    end_submit();
    if (r->rc_create != NGX_OK || !h) {
        printf("    %s: CreateFeature1 -> 0x%08x (handle %p)\n", r->name, r->rc_create, h);
        ngx.destroy_params(p);
        return;
    }
    /* Motion vectors, once. */
    {
        uint16_t mx = f2h(r->mv_mul * r->vx), my = f2h(r->mv_mul * r->vy);
        for (uint32_t i = 0; i < IW * IH; i++) { ((uint16_t *)stage.map)[i * 2] = mx; ((uint16_t *)stage.map)[i * 2 + 1] = my; }
        begin();
        upload(&stage, 0, &motion);
        end_submit();
    }
    r->rc_eval = NGX_OK;
    for (int t = 0; t < r->frames; t++) {
        float sx = halton(t % 32 + 1, 2) - 0.5f, sy = halton(t % 32 + 1, 3) - 0.5f;
        if (r->dlss1) sx = sy = 0.0f;
        write_color(r, t, sx, sy);
        int measure = t >= r->frames - MEASURE;
        begin();
        upload(&stage, 0, &color);
        ngx.set_vp(p, "Color", &color.res);
        ngx.set_vp(p, "Output", &output.res);
        ngx.set_vp(p, "Depth", r->dlss1 ? NULL : &depth.res);
        ngx.set_vp(p, "MotionVectors", r->dlss1 ? NULL : &motion.res);
        ngx.set_f(p, "Jitter.Offset.X", r->jitter_mul * sx);
        ngx.set_f(p, "Jitter.Offset.Y", r->jitter_mul * sy);
        ngx.set_f(p, "Sharpness", r->dlss1 ? 0.3f : 0.0f);
        ngx.set_i(p, "Reset", t == 0);
        ngx.set_f(p, "MV.Scale.X", 1.0f);
        ngx.set_f(p, "MV.Scale.Y", 1.0f);
        ngx.set_vp(p, "ExposureTexture", NULL);
        ngx.set_vp(p, "DLSS.Input.Bias.Current.Color.Mask", NULL);
        ngx.set_ui(p, "DLSS.Render.Subrect.Dimensions.Width", IW);
        ngx.set_ui(p, "DLSS.Render.Subrect.Dimensions.Height", IH);
        ngx.set_f(p, "DLSS.Pre.Exposure", 1.0f);
        unsigned rc = ngx.evaluate(cb, h, p, NULL);
        if (rc != NGX_OK && r->rc_eval == NGX_OK) r->rc_eval = rc;
        if (measure) {
            fence_all();
            VkBufferImageCopy rg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { OW, OH, 1 } };
            vkCmdCopyImageToBuffer(cb, output.img, VK_IMAGE_LAYOUT_GENERAL, readback.buf, 1, &rg);
            VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
        }
        end_submit();
        if (measure && rc == NGX_OK) {
            double m;
            err += rmse(r, t, &m);
            mean += m;
            nerr++;
        }
    }
    if (nerr) { r->err = err / nerr; r->mean = mean / nerr; }
    ngx.stats(h, r->stats);
    r->rc_release = ngx.release(h);
    ngx.destroy_params(p);
    printf("    %s: %d frames %dx%d -> %dx%d, evaluate 0x%08x, evaluated %u, refused %u, last status %s, path %s, RMSE %.5f mean %.3f, release 0x%08x\n",
           r->name, r->frames, IW, IH, OW, OH, r->rc_eval, r->stats[0], r->stats[1], status_name(r->stats[2]),
           r->stats[4] ? "spatial" : "temporal", r->err, r->mean, r->rc_release);
}

static void *sym(const char *name)
{
    void *f = (void *)GetProcAddress(ngx.h, name);
    if (!f) FAILX("GetProcAddress(%s): missing", name);
    return f;
}

int main(int argc, char **argv)
{
    int frames = 40, expect_available = 1;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "frames=", 7)) frames = atoi(argv[i] + 7);
        else if (!strcmp(argv[i], "expect=unavailable")) expect_available = 0;
        else if (!strcmp(argv[i], "expect=available")) expect_available = 1;
    }
    if (frames < MEASURE + 1) frames = MEASURE + 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    pattern_init();

    /* 1. The DLL and the Vulkan loader. */
    ngx.h = LoadLibraryW(L"nvngx.dll");
    printf("LoadLibraryW(nvngx.dll) -> %p (error %lu)\n", (void *)ngx.h, ngx.h ? 0 : GetLastError());
    if (!ngx.h) FAILX("the NGX DLL did not load");
    ngx.ident = (pfn_ident)(void *)GetProcAddress(ngx.h, "SteamARM_NGX_Ident");
    printf("ident: %s\n", ngx.ident ? ngx.ident() : "(not SteamARM's)");
    if (!ngx.ident) FAILX("not SteamARM's nvngx.dll");
    ngx.req_ext = sym("NVSDK_NGX_VULKAN_RequiredExtensions");
    ngx.init_pid = sym("NVSDK_NGX_VULKAN_Init_with_ProjectID");
    ngx.init = sym("NVSDK_NGX_VULKAN_Init");
    ngx.get_caps = sym("NVSDK_NGX_VULKAN_GetCapabilityParameters");
    ngx.alloc_params = sym("NVSDK_NGX_VULKAN_AllocateParameters");
    ngx.destroy_params = sym("NVSDK_NGX_VULKAN_DestroyParameters");
    ngx.create1 = sym("NVSDK_NGX_VULKAN_CreateFeature1");
    ngx.release = sym("NVSDK_NGX_VULKAN_ReleaseFeature");
    ngx.evaluate = sym("NVSDK_NGX_VULKAN_EvaluateFeature");
    ngx.shutdown1 = sym("NVSDK_NGX_VULKAN_Shutdown1");
    ngx.devinfo = sym("SteamARM_NGX_DeviceInfo");
    ngx.stats = sym("SteamARM_NGX_FeatureStats");
    ngx.set_vp = sym("NVSDK_NGX_Parameter_SetVoidPointer");
    ngx.set_ui = sym("NVSDK_NGX_Parameter_SetUI");
    ngx.set_i = sym("NVSDK_NGX_Parameter_SetI");
    ngx.set_f = sym("NVSDK_NGX_Parameter_SetF");
    ngx.get_i = sym("NVSDK_NGX_Parameter_GetI");
    {
        unsigned ni = 99, nd = 99;
        const char **ie = NULL, **de = NULL;
        unsigned rc = ngx.req_ext(&ni, &ie, &nd, &de);
        printf("NVSDK_NGX_VULKAN_RequiredExtensions -> 0x%08x: %u instance, %u device extensions\n", rc, ni, nd);
        if (rc != NGX_OK || ni || nd) BAD("RequiredExtensions must answer none");
        else OK("NVSDK_NGX_VULKAN_RequiredExtensions: none required");
    }

    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) FAILX("LoadLibrary(vulkan-1.dll) failed: %lu", GetLastError());
    gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
    if (!gipa) FAILX("no vkGetInstanceProcAddr in vulkan-1.dll");
#define G(name) PFN_##name name = (PFN_##name)gipa(inst, #name); if (!name) FAILX("no " #name)
    G(vkCreateInstance);
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_dlss", .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    G(vkEnumeratePhysicalDevices);
    G(vkGetPhysicalDeviceProperties);
    G(vkGetPhysicalDeviceProperties2);
    G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkGetPhysicalDeviceMemoryProperties);
    G(vkCreateDevice);
    G(vkGetDeviceProcAddr);
#undef G
    gdpa = vkGetDeviceProcAddr;
    uint32_t npd = 1;
    VkResult er = vkEnumeratePhysicalDevices(inst, &npd, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !npd) FAILX("no physical device (%d)", (int)er);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    char driver[VK_MAX_DRIVER_NAME_SIZE] = "?";
    if (props.apiVersion >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceDriverProperties dp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp };
        vkGetPhysicalDeviceProperties2(pd, &p2);
        snprintf(driver, sizeof driver, "%s", dp.driverName);
    }
    printf("device: %s, driver %s, Vulkan %u.%u\n", props.deviceName, driver, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion));
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    uint32_t nq = 16;
    VkQueueFamilyProperties qf[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    family = UINT32_MAX;
    for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++)
        if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) family = i;
    if (family == UINT32_MAX) FAILX("no graphics queue family");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family, .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define D(name) name = (PFN_##name)gdpa(dev, #name); if (!name) FAILX("no " #name)
    D(vkGetDeviceQueue); D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkAllocateMemory); D(vkBindBufferMemory);
    D(vkMapMemory); D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer); D(vkEndCommandBuffer);
    D(vkCmdPipelineBarrier); D(vkCreateFence); D(vkWaitForFences); D(vkResetFences); D(vkQueueSubmit); D(vkResetCommandBuffer);
    D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkBindImageMemory); D(vkCreateImageView); D(vkCmdCopyBufferToImage);
    D(vkCmdCopyImageToBuffer); D(vkDeviceWaitIdle);
#undef D
    vkGetDeviceQueue(dev, family, 0, &queue);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = family };
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));

    /* 2. The textures, in GENERAL, the depth at 0.5. */
    const VkImageUsageFlags in_usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    make_tex(&color, VK_FORMAT_R16G16B16A16_SFLOAT, IW, IH, 8, in_usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0);
    make_tex(&depth, VK_FORMAT_D32_SFLOAT, IW, IH, 4, in_usage | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 1);
    make_tex(&motion, VK_FORMAT_R16G16_SFLOAT, IW, IH, 4, in_usage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0);
    make_tex(&output, VK_FORMAT_R16G16B16A16_SFLOAT, OW, OH, 8, in_usage | VK_IMAGE_USAGE_STORAGE_BIT, 0);
    make_hostbuf(&stage, (VkDeviceSize)IW * IH * 8);
    make_hostbuf(&readback, (VkDeviceSize)OW * OH * 8);
    for (uint32_t i = 0; i < IW * IH; i++) ((float *)stage.map)[i] = 0.5f;
    begin();
    to_general(&color); to_general(&depth); to_general(&motion); to_general(&output);
    upload(&stage, 0, &depth);
    end_submit();
    printf("vk: colour image %p fmt %d, depth %p fmt %d, motion %p fmt %d, output %p fmt %d\n", (void *)color.img, color.fmt,
           (void *)depth.img, depth.fmt, (void *)motion.img, motion.fmt, (void *)output.img, output.fmt);

    /* 3. NGX init as a Vulkan game (Unreal's plugin: Init_with_ProjectID). */
    void *caps = NULL;
    {
        unsigned rc = ngx.init_pid("steamarm-dlss-test", 1, "5.4.0", L"C:\\", inst, pd, dev, gipa, gdpa, NULL, 0x15);
        printf("NVSDK_NGX_VULKAN_Init_with_ProjectID -> 0x%08x\n", rc);
        if (rc != NGX_OK) BAD("Init_with_ProjectID -> 0x%08x", rc);
        else OK("NVSDK_NGX_VULKAN_Init_with_ProjectID: success");
        unsigned di[4] = { 0, 0, 0, 0 };
        ngx.devinfo(di);
        printf("device: probe %s, caps 0x%x, driver %u, max scale %.2f\n", status_name(di[0]), di[1], di[2], di[3] / 100.0);
        if (ngx.get_caps(&caps) != NGX_OK || !caps) FAILX("GetCapabilityParameters");
        int a = -1, fg = -1;
        ngx.get_i(caps, "SuperSampling.Available", &a);
        ngx.get_i(caps, "FrameGeneration.Available", &fg);
        printf("caps: SuperSampling.Available %d, FrameGeneration.Available %d\n", a, fg);
        if (a != expect_available) BAD("SuperSampling.Available = %d, expected %d", a, expect_available);
        else OK("SuperSampling.Available = %d as expected", a);
    }

    if (!expect_available) {
        struct run r = { "unavailable", -1, 0, 0, 0, MEASURE + 1, 0 };
        run_case(&r);
        if (r.rc_create == NGX_FAIL_FeatureNotSupported) OK("CreateFeature1 refused with FeatureNotSupported, as SuperSampling.Available = 0 says");
        else BAD("CreateFeature1 -> 0x%08x, expected FeatureNotSupported", r.rc_create);
    } else {
        /* 4. Still picture: the game's jitter sign and the opposite. */
        struct run good = { "still", -1, 0, 0, 0, frames, 0 };
        struct run opp = { "still, opposite jitter", 1, 0, 0, 0, frames, 0 };
        run_case(&good);
        run_case(&opp);
        if (good.rc_create != NGX_OK || good.rc_eval != NGX_OK || good.rc_release != NGX_OK)
            BAD("still: create 0x%08x evaluate 0x%08x release 0x%08x", good.rc_create, good.rc_eval, good.rc_release);
        else if (good.stats[0] != (unsigned)frames || good.stats[1] != 0 || good.stats[2] != SA_ST_OK)
            BAD("still: %u evaluated, %u refused, last status %s: not every frame went to MetalFX", good.stats[0], good.stats[1], status_name(good.stats[2]));
        else if (good.err < 0 || good.err > 0.04 || good.err * 2 > opp.err || fabs(good.mean - 0.5) > 0.05)
            BAD("still: RMSE %.4f (opposite jitter %.4f, mean %.3f)", good.err, opp.err, good.mean);
        else
            OK("DLSS evaluate on the game's VkCommandBuffer -> MetalFX temporal, every frame OK: RMSE %.4f (opposite jitter %.4f)", good.err, opp.err);
        /* 5. Moving. */
        struct run mv = { "moving", -1, 1.37f, 0.61f, -1, frames, 0 };
        struct run mvp = { "moving, opposite vectors", -1, 1.37f, 0.61f, 1, frames, 0 };
        run_case(&mv);
        run_case(&mvp);
        if (mv.rc_eval != NGX_OK || mvp.rc_eval != NGX_OK || mv.stats[1] || mvp.stats[1])
            BAD("moving: evaluate 0x%08x / 0x%08x, refused %u / %u", mv.rc_eval, mvp.rc_eval, mv.stats[1], mvp.stats[1]);
        else if (mv.err < 0 || mv.err > 0.15 || mv.err >= mvp.err)
            BAD("motion vectors: RMSE %.4f vs opposite %.4f", mv.err, mvp.err);
        else
            OK("motion vectors as DLSS defines them: RMSE %.4f (opposite %.4f)", mv.err, mvp.err);
    }

    /* 6. DLSS 1: NVSDK_NGX_VULKAN_Init with SDK 0x12, the spatial path on either driver. */
    {
        unsigned rc = ngx.destroy_params(caps), rs = ngx.shutdown1(dev);
        if (rc != NGX_OK || rs != NGX_OK) BAD("DestroyParameters 0x%08x, Shutdown1 0x%08x", rc, rs);
        caps = NULL;
        rc = ngx.init(0x2019ull, L"C:\\", inst, pd, dev, gipa, gdpa, NULL, 0x12);
        printf("NVSDK_NGX_VULKAN_Init (SDK 0x12, DLSS 1) -> 0x%08x\n", rc);
        if (rc != NGX_OK) BAD("DLSS 1 init -> 0x%08x", rc);
        unsigned di[4] = { 0, 0, 0, 0 };
        ngx.devinfo(di);
        printf("device: probe %s, caps 0x%x, driver %u, max scale %.2f\n", status_name(di[0]), di[1], di[2], di[3] / 100.0);
        int a = -1;
        if (ngx.get_caps(&caps) != NGX_OK || !caps) FAILX("GetCapabilityParameters (DLSS 1)");
        ngx.get_i(caps, "SuperSampling.Available", &a);
        if (a != 1) BAD("DLSS 1: SuperSampling.Available = %d, expected 1", a);
        else {
            struct run d1 = { "dlss1 spatial (quarter-rate pattern)", 0, 0, 0, 0, frames < 12 ? frames : 12, 1, 0.25f };
            struct run d1f = { "dlss1 spatial (full-rate pattern)", 0, 0, 0, 0, MEASURE + 2, 1, 1.0f };
            run_case(&d1);
            run_case(&d1f);
            if (d1.rc_create != NGX_OK || d1.rc_eval != NGX_OK || d1.rc_release != NGX_OK)
                BAD("dlss1: create 0x%08x evaluate 0x%08x release 0x%08x", d1.rc_create, d1.rc_eval, d1.rc_release);
            else if (d1.stats[4] != 1 || d1.stats[1] != 0 || d1.stats[2] != SA_ST_OK || d1.stats[0] != (unsigned)d1.frames)
                BAD("dlss1: path %s, %u evaluated, %u refused, last status %s", d1.stats[4] ? "spatial" : "temporal", d1.stats[0], d1.stats[1], status_name(d1.stats[2]));
            else if (d1.err < 0 || d1.err > 0.06 || fabs(d1.mean - 0.5) > 0.05)
                BAD("dlss1: RMSE %.4f mean %.3f: the spatial pass did not leave the upscaled picture on the output", d1.err, d1.mean);
            else
                OK("DLSS 1 -> the shim's spatial pass on the game's VkCommandBuffer, every frame OK: RMSE %.4f mean %.3f (full-rate pattern %.4f)", d1.err, d1.mean, d1f.err);
        }
        rc = caps ? ngx.destroy_params(caps) : NGX_OK;
        rs = ngx.shutdown1(dev);
        if (rc != NGX_OK || rs != NGX_OK) BAD("DestroyParameters 0x%08x, Shutdown1 0x%08x", rc, rs);
        else OK("DestroyParameters, Shutdown1: success");
    }
    vkDeviceWaitIdle(dev);
    printf(failures ? "== vk dlss: FAIL (%d)\n" : "== vk dlss: ok\n", failures);
    fflush(stdout);
    ExitProcess(failures != 0);
    return failures != 0;
}
