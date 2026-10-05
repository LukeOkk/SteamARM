// The D3D12 side of the FSR -> MetalFX temporal path, end to end, before
// the FFX DLL exists: a Windows program that does with VKD3D-Proton's
// interop interface what the DLL will do (shim/mfx_temporal.h has the
// command; tests/win/vk_mfxt_channel.c proves the channel with plain Vulkan).
//
//   D3D12 device (12_0 first: AMD's samples need it, and KosmicKrisp gets it
//   only with VKD3D_FEATURE_LEVEL=12_0; else 11_0)
//   -> ID3D12DXVKInteropDevice1 (vkd3d_device_vkd3d_ext.idl)
//      GetVulkanHandles: the VkDevice for vulkan-1.dll's vkGetDeviceProcAddr
//      GetVulkanResourceInfo1: each texture's VkImage and VkFormat, and the
//        carrier buffer's VkBuffer and offset
//      GetVulkanImageLayout: the layout behind each D3D12 state used
//      BeginVkCommandBufferInterop: the command list's VkCommandBuffer
//        (render pass ended, state invalidated); EndVkCommandBufferInterop
//   -> vkCmdUpdateBuffer(the command) from winevulkan, between two memory
//      barriers, into the command list's own VkCommandBuffer
//   -> FEX thunks -> the Vulkan shim -> KosmicKrisp's
//      kk_steamarm_upscale_temporal -> MetalFX's temporal scaler.
//
// The textures are made the way a game makes FSR's: colour RGBA16F 960x540
// and motion vectors RG16F (render targets), depth D32 (depth-stencil),
// output RGBA16F 1920x1080 (a UAV); filled with CopyTextureRegion uploads
// (no shader compiler), kept in the states FFX's DX12 backend asks for
// (inputs NON_PIXEL_SHADER_RESOURCE, output UNORDERED_ACCESS). The picture
// is tests/kk/mfxt_bridge.m's: a pattern with a known sub-pixel jitter
// (Halton 2,3; the game's jitter is minus the sample offset) and, moving, a
// known motion (vectors from the current position to the previous one, in
// input pixels); the output is read back and compared with a supersampled
// reference over the last frames (RMSE; the pattern is about 0.25..0.75).
//
// Checks: PROBE in a never-submitted command buffer of a pool of our own on
// vkd3d's VkDevice, and in the command list; ENCODE status OK every frame;
// jitter and motion-vector signs (the right ones best); the command list
// executed a second time after the output was zeroed (vkd3d records without
// ONE_TIME_SUBMIT: KosmicKrisp records it again and replays the upscale);
// depth as R32_TYPELESS and R32G8X24_TYPELESS (D32S8).
//
// Arguments: frames=N (per case, default 40), noresubmit (with
// VKD3D_CONFIG=one_time_submit a second execution is not allowed).
// Built and run by tests/win/run_d3d12_mfxt_interop.sh.
// Prints "== d3d12 mfxt interop: ok", "...: FAIL (n)" or "...: FAIL (why)".
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "../../shim/mfx_temporal.h"

enum { IW = 960, IH = 540, OW = 1920, OH = 1080, MEASURE = 4 };

/* ---- vkd3d-proton's interop interface (vkd3d_device_vkd3d_ext.idl) ---- */
static const GUID IID_ID3D12DXVKInteropDevice =
    { 0x39da4e09, 0xbd1c, 0x4198, { 0x9f, 0xae, 0x86, 0xbb, 0xe3, 0xbe, 0x41, 0xfd } };
static const GUID IID_ID3D12DXVKInteropDevice1 =
    { 0x902d8115, 0x59eb, 0x4406, { 0x95, 0x18, 0xfe, 0x00, 0xf9, 0x91, 0xee, 0x65 } };
static const GUID IID_ID3D12DXVKInteropDevice2 =
    { 0x90ecf26e, 0xb212, 0x43f5, { 0xb6, 0x2a, 0x82, 0x5a, 0xd7, 0xb1, 0x38, 0x5e } };
static const GUID IID_ID3D12DXVKInteropDevice3 =
    { 0x22a70184, 0xa6a4, 0x4c24, { 0xbf, 0x97, 0x7d, 0x6d, 0xf9, 0xf1, 0x2d, 0x8a } };

typedef struct ID3D12DXVKInteropDevice1 ID3D12DXVKInteropDevice1;
typedef struct ID3D12DXVKInteropDevice1Vtbl {
    /* IUnknown */
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ID3D12DXVKInteropDevice1 *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(ID3D12DXVKInteropDevice1 *);
    ULONG (STDMETHODCALLTYPE *Release)(ID3D12DXVKInteropDevice1 *);
    /* ID3D12DXVKInteropDevice */
    HRESULT (STDMETHODCALLTYPE *GetDXGIAdapter)(ID3D12DXVKInteropDevice1 *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *GetInstanceExtensions)(ID3D12DXVKInteropDevice1 *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceExtensions)(ID3D12DXVKInteropDevice1 *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceFeatures)(ID3D12DXVKInteropDevice1 *, const VkPhysicalDeviceFeatures2 **);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHandles)(ID3D12DXVKInteropDevice1 *, VkInstance *, VkPhysicalDevice *, VkDevice *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanQueueInfo)(ID3D12DXVKInteropDevice1 *, ID3D12CommandQueue *, VkQueue *, UINT32 *);
    void (STDMETHODCALLTYPE *GetVulkanImageLayout)(ID3D12DXVKInteropDevice1 *, ID3D12Resource *, D3D12_RESOURCE_STATES,
                                                   VkImageLayout *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo)(ID3D12DXVKInteropDevice1 *, ID3D12Resource *, UINT64 *, UINT64 *);
    HRESULT (STDMETHODCALLTYPE *LockCommandQueue)(ID3D12DXVKInteropDevice1 *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *UnlockCommandQueue)(ID3D12DXVKInteropDevice1 *, ID3D12CommandQueue *);
    /* ID3D12DXVKInteropDevice1 */
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo1)(ID3D12DXVKInteropDevice1 *, ID3D12Resource *, UINT64 *, UINT64 *,
                                                        VkFormat *);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandQueue)(ID3D12DXVKInteropDevice1 *, const D3D12_COMMAND_QUEUE_DESC *,
                                                           UINT32, ID3D12CommandQueue **);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandAllocator)(ID3D12DXVKInteropDevice1 *, D3D12_COMMAND_LIST_TYPE, UINT32,
                                                               ID3D12CommandAllocator **);
    HRESULT (STDMETHODCALLTYPE *BeginVkCommandBufferInterop)(ID3D12DXVKInteropDevice1 *, ID3D12CommandList *,
                                                             VkCommandBuffer *);
    HRESULT (STDMETHODCALLTYPE *EndVkCommandBufferInterop)(ID3D12DXVKInteropDevice1 *, ID3D12CommandList *);
} ID3D12DXVKInteropDevice1Vtbl;
struct ID3D12DXVKInteropDevice1 { const ID3D12DXVKInteropDevice1Vtbl *lpVtbl; };

/* ---- globals ---- */
static int failures;
#define FAILX(...) do { printf(__VA_ARGS__); printf("\n== d3d12 mfxt interop: FAIL\n"); fflush(stdout); ExitProcess(1); } while (0)
#define HR(what, expr) do { HRESULT hr_ = (expr); if (FAILED(hr_)) FAILX("%s -> 0x%08lx", what, (unsigned long)hr_); } while (0)
#define OK(...) do { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define BAD(...) do { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); failures++; } while (0)

static ID3D12Device *dev;
static ID3D12DXVKInteropDevice1 *ix;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc, *alloc2;
static ID3D12GraphicsCommandList *cl, *cl2;
static ID3D12Fence *fence;
static UINT64 fence_value;
static HANDLE fence_event;
static PFN_vkCmdUpdateBuffer p_vkCmdUpdateBuffer;
static PFN_vkCmdPipelineBarrier p_vkCmdPipelineBarrier;

/* Resources. */
static ID3D12Resource *color, *depth, *motion, *output, *carrier, *upload, *zeros, *readback;
static D3D12_RESOURCE_STATES color_st, motion_st;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_color, fp_motion, fp_depth, fp_out;
static unsigned char *upload_map, *readback_map;
static UINT64 carrier_buf, carrier_off;
struct vkimg { UINT64 image; VkFormat format; };
static struct vkimg vk_color, vk_depth, vk_motion, vk_output;

static DWORD WINAPI watchdog(LPVOID arg)
{
    (void)arg;
    Sleep(55000);
    printf("== d3d12 mfxt interop: FAIL (watchdog: not done in 55 s)\n");
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

static const char *layout_name(VkImageLayout l)
{
    switch ((int)l) {
    case VK_IMAGE_LAYOUT_UNDEFINED: return "UNDEFINED";
    case VK_IMAGE_LAYOUT_GENERAL: return "GENERAL";
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return "COLOR_ATTACHMENT_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_STENCIL_ATTACHMENT_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL: return "DEPTH_STENCIL_READ_ONLY_OPTIMAL";
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return "SHADER_READ_ONLY_OPTIMAL";
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return "TRANSFER_SRC_OPTIMAL";
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return "TRANSFER_DST_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL: return "DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL: return "DEPTH_ATTACHMENT_STENCIL_READ_ONLY_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL: return "DEPTH_ATTACHMENT_OPTIMAL";
    case VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL: return "DEPTH_READ_ONLY_OPTIMAL";
    case VK_IMAGE_LAYOUT_READ_ONLY_OPTIMAL: return "READ_ONLY_OPTIMAL";
    case VK_IMAGE_LAYOUT_ATTACHMENT_OPTIMAL: return "ATTACHMENT_OPTIMAL";
    default: return "?";
    }
}

/* ---- the picture (tests/kk/mfxt_bridge.m's pattern, sine from a table) ---- */
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

/* ---- D3D12 helpers ---- */
static ID3D12Resource *make_tex(DXGI_FORMAT f, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st,
                                D3D12_RESOURCE_DESC *out_desc)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC d = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, f, { 1, 0 },
                              D3D12_TEXTURE_LAYOUT_UNKNOWN, flags };
    ID3D12Resource *r = NULL;
    HR("CreateCommittedResource(texture)",
       ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **)&r));
    if (out_desc) *out_desc = d;
    return r;
}

static ID3D12Resource *make_buf(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = { type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC d = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource *r = NULL;
    HR("CreateCommittedResource(buffer)",
       ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **)&r));
    return r;
}

static void barrier(ID3D12GraphicsCommandList *l, ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(l, 1, &b);
}

/* buffer (placed footprint) -> texture subresource 0, or texture -> buffer */
static void copy_buf_tex(ID3D12GraphicsCommandList *l, ID3D12Resource *buf, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp,
                         ID3D12Resource *tex, int to_tex)
{
    D3D12_TEXTURE_COPY_LOCATION b, t;
    memset(&b, 0, sizeof b);
    memset(&t, 0, sizeof t);
    b.pResource = buf;
    b.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    b.PlacedFootprint = *fp;
    t.pResource = tex;
    t.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    t.SubresourceIndex = 0;
    if (to_tex) ID3D12GraphicsCommandList_CopyTextureRegion(l, &t, 0, 0, 0, &b, NULL);
    else ID3D12GraphicsCommandList_CopyTextureRegion(l, &b, 0, 0, 0, &t, NULL);
}

static void footprint(const D3D12_RESOURCE_DESC *d, UINT64 offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp, UINT64 *total)
{
    UINT rows;
    UINT64 row_size, bytes;
    ID3D12Device_GetCopyableFootprints(dev, d, 0, 1, offset, fp, &rows, &row_size, &bytes);
    if (total) *total = bytes;
}

static void execute_wait(ID3D12GraphicsCommandList *l)
{
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)l };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    HR("Signal", ID3D12CommandQueue_Signal(queue, fence, ++fence_value));
    if (ID3D12Fence_GetCompletedValue(fence) < fence_value) {
        HR("SetEventOnCompletion", ID3D12Fence_SetEventOnCompletion(fence, fence_value, fence_event));
        if (WaitForSingleObject(fence_event, 10000) != WAIT_OBJECT_0)
            FAILX("GPU did not finish in 10 s (fence %llu, completed %llu)", (unsigned long long)fence_value,
                  (unsigned long long)ID3D12Fence_GetCompletedValue(fence));
    }
}

static void reset_list(ID3D12CommandAllocator *a, ID3D12GraphicsCommandList *l)
{
    HR("CommandAllocator::Reset", ID3D12CommandAllocator_Reset(a));
    HR("CommandList::Reset", ID3D12GraphicsCommandList_Reset(l, a, NULL));
}

static struct vkimg vk_info(ID3D12Resource *r, const char *name)
{
    struct vkimg v = { 0, VK_FORMAT_UNDEFINED };
    UINT64 off = ~0ull;
    HR("GetVulkanResourceInfo1", ix->lpVtbl->GetVulkanResourceInfo1(ix, r, &v.image, &off, &v.format));
    if (!v.image) FAILX("GetVulkanResourceInfo1(%s): no VkImage", name);
    return v;
}

static void print_layouts(ID3D12Resource *r, const char *name, const D3D12_RESOURCE_STATES *st, const char *const *stn, int n)
{
    printf("layouts %s:", name);
    for (int i = 0; i < n; i++) {
        VkImageLayout l = (VkImageLayout)-1;
        ix->lpVtbl->GetVulkanImageLayout(ix, r, st[i], &l);
        printf(" %s=%s", stn[i], layout_name(l));
    }
    printf("\n");
}

/* ---- the command ---- */
static struct sa_mfxt_cmd make_cmd(uint32_t op, uint64_t context, float jx, float jy, int reset)
{
    struct sa_mfxt_cmd c;
    memset(&c, 0, sizeof c);
    c.magic = SA_MFXT_MAGIC;
    c.version = SA_MFXT_VERSION;
    c.size = sizeof c;
    c.op = op;
    c.status = SA_ST_UNSEEN;
    c.context = context;
    c.color = (struct sa_mfxt_tex){ vk_color.image, vk_color.format, IW, IH, 0, 0, 0 };
    c.depth = (struct sa_mfxt_tex){ vk_depth.image, vk_depth.format, IW, IH, 0, 0, 0 };
    c.motion = (struct sa_mfxt_tex){ vk_motion.image, vk_motion.format, IW, IH, 0, 0, 0 };
    c.output = (struct sa_mfxt_tex){ vk_output.image, vk_output.format, OW, OH, 0, 0, 0 };
    c.render_w = IW;
    c.render_h = IH;
    c.upscale_w = OW;
    c.upscale_h = OH;
    c.jitter_x = jx;
    c.jitter_y = jy;
    c.mv_scale_x = c.mv_scale_y = 1.0f;
    c.pre_exposure = 1.0f;
    c.flags = reset ? SA_MFXT_RESET : 0;
    return c;
}

/* What the DLL records around the command: all commands, writes, to all
 * commands, reads and writes. */
static void vk_barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL,
                           0, NULL);
}

static LARGE_INTEGER qpf;
static double us_since(LARGE_INTEGER t0)
{
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    return (double)(t1.QuadPart - t0.QuadPart) * 1e6 / (double)qpf.QuadPart;
}

/* Into the command list (interop): [probe,] barrier, encode, barrier. */
static uint32_t record_upscale(struct sa_mfxt_cmd *c, struct sa_mfxt_cmd *probe, double *rec_us)
{
    VkCommandBuffer vcb = VK_NULL_HANDLE;
    HR("BeginVkCommandBufferInterop", ix->lpVtbl->BeginVkCommandBufferInterop(ix, (ID3D12CommandList *)cl, &vcb));
    if (!vcb) FAILX("BeginVkCommandBufferInterop: no VkCommandBuffer");
    if (probe)
        p_vkCmdUpdateBuffer(vcb, (VkBuffer)carrier_buf, carrier_off, sizeof *probe, probe);
    vk_barrier(vcb);
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    p_vkCmdUpdateBuffer(vcb, (VkBuffer)carrier_buf, carrier_off, sizeof *c, c);
    if (rec_us) *rec_us += us_since(t0);
    vk_barrier(vcb);
    HR("EndVkCommandBufferInterop", ix->lpVtbl->EndVkCommandBufferInterop(ix, (ID3D12CommandList *)cl));
    return c->status;
}

/* ---- one case ---- */
struct run {
    const char *name;
    float jitter_mul;   /* jitter given = jitter_mul * sample offset (the game's: -1) */
    float vx, vy;       /* the content moves by +v input pixels a frame */
    float mv_mul;       /* motion vectors written = mv_mul * v */
    uint64_t context;
    int frames, resubmit, probe;
    /* out */
    uint32_t status;
    double err, mean, err_again, mean_again, rec_us;
};

static void write_color(int t, float sx, float sy, const struct run *r)
{
    unsigned char *base = upload_map + fp_color.Offset;
    for (int y = 0; y < IH; y++) {
        uint16_t *o = (uint16_t *)(base + (size_t)y * fp_color.Footprint.RowPitch);
        float py = (float)y + 0.5f + sy - r->vy * (float)t;
        for (int x = 0; x < IW; x++) {
            float px = (float)x + 0.5f + sx - r->vx * (float)t;
            uint16_t v = f2h(pattern(px, py));
            o[x * 4] = o[x * 4 + 1] = o[x * 4 + 2] = v;
            o[x * 4 + 3] = 0x3c00;
        }
    }
}

static void write_motion(const struct run *r)
{
    unsigned char *base = upload_map + fp_motion.Offset;
    uint16_t mx = f2h(r->mv_mul * r->vx), my = f2h(r->mv_mul * r->vy);
    for (int y = 0; y < IH; y++) {
        uint16_t *o = (uint16_t *)(base + (size_t)y * fp_motion.Footprint.RowPitch);
        for (int x = 0; x < IW; x++) { o[x * 2] = mx; o[x * 2 + 1] = my; }
    }
}

/* RMSE of the read-back output against the supersampled scene at frame t,
 * every 4th pixel away from the borders. */
static double rmse(const struct run *r, int t, double *mean)
{
    double se = 0, sum = 0;
    long n = 0;
    const float k = (float)IW / OW;
    for (int y = 16; y < OH - 16; y += 4) {
        const uint16_t *row = (const uint16_t *)(readback_map + fp_out.Offset + (size_t)y * fp_out.Footprint.RowPitch);
        for (int x = 16; x < OW - 16; x += 4) {
            float ref = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                    ref += pattern(((float)x + (i + 0.5f) / 4) * k - r->vx * (float)t,
                                   ((float)y + (j + 0.5f) / 4) * k - r->vy * (float)t);
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

static void readback_into(ID3D12GraphicsCommandList *l)
{
    barrier(l, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    copy_buf_tex(l, readback, &fp_out, output, 0);
    barrier(l, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

static void run_case(struct run *r)
{
    double err = 0, mean = 0;
    int nerr = 0;
    r->status = SA_ST_UNSEEN;
    r->rec_us = 0;
    write_motion(r);
    for (int t = 0; t < r->frames; t++) {
        float sx = halton(t % 32 + 1, 2) - 0.5f, sy = halton(t % 32 + 1, 3) - 0.5f;
        write_color(t, sx, sy, r);
        int measure = t >= r->frames - MEASURE;
        reset_list(alloc, cl);
        barrier(cl, color, color_st, D3D12_RESOURCE_STATE_COPY_DEST);
        copy_buf_tex(cl, upload, &fp_color, color, 1);
        color_st = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier(cl, color, D3D12_RESOURCE_STATE_COPY_DEST, color_st);
        if (t == 0) {
            barrier(cl, motion, motion_st, D3D12_RESOURCE_STATE_COPY_DEST);
            copy_buf_tex(cl, upload, &fp_motion, motion, 1);
            motion_st = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier(cl, motion, D3D12_RESOURCE_STATE_COPY_DEST, motion_st);
        }
        struct sa_mfxt_cmd c = make_cmd(SA_OP_ENCODE, r->context, r->jitter_mul * sx, r->jitter_mul * sy, t == 0);
        struct sa_mfxt_cmd p = make_cmd(SA_OP_PROBE, r->context, 0, 0, 0);
        uint32_t st = record_upscale(&c, (r->probe && t == 0) ? &p : NULL, &r->rec_us);
        if (r->probe && t == 0) {
            if (p.status != SA_ST_OK || !(p.caps_out & SA_MFXT_CAP_TEMPORAL))
                BAD("probe in the command list: status %s caps 0x%x driver %u max scale %.2f", status_name(p.status),
                    p.caps_out, p.driver_out, p.max_scale_out);
            else
                OK("probe in the command list: status OK, caps 0x%x, driver %u, max scale %.2f", p.caps_out,
                   p.driver_out, p.max_scale_out);
        }
        r->status = st;
        if (st != SA_ST_OK) {
            HR("Close", ID3D12GraphicsCommandList_Close(cl));
            execute_wait(cl);
            r->err = -1;
            return;
        }
        if (measure) readback_into(cl);
        HR("Close", ID3D12GraphicsCommandList_Close(cl));
        execute_wait(cl);
        if (measure) {
            double m;
            err += rmse(r, t, &m);
            mean += m;
            nerr++;
        }
    }
    r->err = err / nerr;
    r->mean = mean / nerr;
    r->rec_us /= r->frames;
    if (!r->resubmit) return;

    /* The last frame's command list once more, after the output is zeroed:
     * the same VkCommandBuffer submitted again. Without ONE_TIME_SUBMIT
     * KosmicKrisp records it again from its command queue, the upscale
     * included (kk_mfxt_replay); the output must be the upscale again, not
     * zero. */
    reset_list(alloc2, cl2);
    barrier(cl2, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
    copy_buf_tex(cl2, zeros, &fp_out, output, 1);
    barrier(cl2, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    HR("Close(zero)", ID3D12GraphicsCommandList_Close(cl2));
    execute_wait(cl2);
    memset(readback_map + fp_out.Offset, 0, (size_t)fp_out.Footprint.RowPitch * OH);
    execute_wait(cl);
    r->err_again = rmse(r, r->frames - 1, &r->mean_again);
}

static void check_still(const struct run *good, const struct run *opp)
{
    printf("    still, jitter as the game gives it %.5f, opposite sign %.5f (mean %.3f); record %.0f us/frame\n",
           good->err, opp->err, good->mean, good->rec_us);
    if (good->status != SA_ST_OK || opp->status != SA_ST_OK)
        BAD("still: encode status %s / %s", status_name(good->status), status_name(opp->status));
    else if (good->err > 0.04 || good->err * 2 > opp->err || fabs(good->mean - 0.5) > 0.05)
        BAD("jitter: the game's convention is not clearly the best, or the picture is wrong");
    else
        OK("encode through the D3D12 command list: status OK, RMSE %.4f (opposite jitter %.4f)", good->err, opp->err);
}

int main(int argc, char **argv)
{
    int frames = 40, resubmit = 1;
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "frames=", 7)) frames = atoi(argv[i] + 7);
        else if (!strcmp(argv[i], "noresubmit")) resubmit = 0;
    }
    if (frames < MEASURE + 1) frames = MEASURE + 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    QueryPerformanceFrequency(&qpf);
    LARGE_INTEGER t_start;
    QueryPerformanceCounter(&t_start);
    pattern_init();

    /* Device: 12_0 first, then 11_0. */
    const char *fl_env = getenv("VKD3D_FEATURE_LEVEL"), *cfg_env = getenv("VKD3D_CONFIG");
    printf("env: VKD3D_FEATURE_LEVEL=%s VKD3D_CONFIG=%s\n", fl_env ? fl_env : "(unset)", cfg_env ? cfg_env : "(unset)");
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev);
    printf("D3D12CreateDevice(12_0): 0x%08lx\n", (unsigned long)hr);
    const char *fl_used = "12_0";
    if (FAILED(hr)) {
        HR("D3D12CreateDevice(11_0)", D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
        printf("D3D12CreateDevice(11_0): ok\n");
        fl_used = "11_0";
    }
    {
        D3D_FEATURE_LEVEL want[4] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                      D3D_FEATURE_LEVEL_12_1 };
        D3D12_FEATURE_DATA_FEATURE_LEVELS fl = { 4, want, 0 };
        if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl)))
            printf("device: feature level %s, max supported 0x%x\n", fl_used, (unsigned)fl.MaxSupportedFeatureLevel);
    }

    /* The interop interface. */
    HR("QueryInterface(ID3D12DXVKInteropDevice1)",
       ID3D12Device_QueryInterface(dev, &IID_ID3D12DXVKInteropDevice1, (void **)&ix));
    {
        IUnknown *u;
        const GUID *g[3] = { &IID_ID3D12DXVKInteropDevice, &IID_ID3D12DXVKInteropDevice2, &IID_ID3D12DXVKInteropDevice3 };
        const char *gn[3] = { "Interop", "Interop2", "Interop3" };
        printf("interop: Interop1 ok");
        for (int i = 0; i < 3; i++) {
            HRESULT h = ID3D12Device_QueryInterface(dev, g[i], (void **)&u);
            printf(", %s %s", gn[i], SUCCEEDED(h) ? "ok" : "no");
            if (SUCCEEDED(h)) IUnknown_Release(u);
        }
        printf("\n");
    }
    VkInstance vinst;
    VkPhysicalDevice vpd;
    VkDevice vdev;
    HR("GetVulkanHandles", ix->lpVtbl->GetVulkanHandles(ix, &vinst, &vpd, &vdev));
    {
        UINT n = 0;
        HR("GetDeviceExtensions(count)", ix->lpVtbl->GetDeviceExtensions(ix, &n, NULL));
        const char **e = calloc(n + 1, sizeof *e);
        HR("GetDeviceExtensions", ix->lpVtbl->GetDeviceExtensions(ix, &n, e));
        int unified = 0;
        for (UINT i = 0; i < n; i++)
            if (!strcmp(e[i], "VK_KHR_unified_image_layouts")) unified = 1;
        printf("vkd3d's Vulkan device: %u extensions, VK_KHR_unified_image_layouts %s\n", n, unified ? "on" : "off");
    }

    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    if (!vk) FAILX("LoadLibrary(vulkan-1.dll): %lu", GetLastError());
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(vk, "vkGetInstanceProcAddr");
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)(void *)GetProcAddress(vk, "vkGetDeviceProcAddr");
    if (!gipa || !gdpa) FAILX("vulkan-1.dll: no vkGetInstanceProcAddr / vkGetDeviceProcAddr");
    {
        PFN_vkGetPhysicalDeviceProperties2 gpp2 =
            (PFN_vkGetPhysicalDeviceProperties2)gipa(vinst, "vkGetPhysicalDeviceProperties2");
        if (gpp2) {
            VkPhysicalDeviceDriverProperties dp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
            VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp };
            gpp2(vpd, &p2);
            printf("vulkan: %s, driver %s (id %u)\n", p2.properties.deviceName, dp.driverName, (unsigned)dp.driverID);
        }
    }
#define D(name) PFN_##name p_##name = (PFN_##name)gdpa(vdev, #name); if (!p_##name) FAILX("vkGetDeviceProcAddr: no " #name)
    D(vkCreateCommandPool);
    D(vkAllocateCommandBuffers);
    D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer);
    D(vkDestroyCommandPool);
#undef D
    p_vkCmdUpdateBuffer = (PFN_vkCmdUpdateBuffer)gdpa(vdev, "vkCmdUpdateBuffer");
    p_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)gdpa(vdev, "vkCmdPipelineBarrier");
    if (!p_vkCmdUpdateBuffer || !p_vkCmdPipelineBarrier) FAILX("vkGetDeviceProcAddr: no vkCmdUpdateBuffer / vkCmdPipelineBarrier");

    /* Queue, lists, fence. */
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    HR("CreateCommandQueue", ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    VkQueue vq;
    UINT32 vfam = 0;
    HR("GetVulkanQueueInfo", ix->lpVtbl->GetVulkanQueueInfo(ix, queue, &vq, &vfam));
    HR("CreateCommandAllocator", ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                     &IID_ID3D12CommandAllocator, (void **)&alloc));
    HR("CreateCommandAllocator", ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                     &IID_ID3D12CommandAllocator, (void **)&alloc2));
    HR("CreateCommandList", ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                                           &IID_ID3D12GraphicsCommandList, (void **)&cl));
    HR("CreateCommandList", ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc2, NULL,
                                                           &IID_ID3D12GraphicsCommandList, (void **)&cl2));
    ID3D12GraphicsCommandList_Close(cl2);
    HR("CreateFence", ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    /* Resources: the textures as a game makes FSR's. */
    D3D12_RESOURCE_DESC dc, dd, dm, dout;
    color = make_tex(DXGI_FORMAT_R16G16B16A16_FLOAT, IW, IH, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                     D3D12_RESOURCE_STATE_COPY_DEST, &dc);
    depth = make_tex(DXGI_FORMAT_D32_FLOAT, IW, IH, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_COPY_DEST,
                     &dd);
    motion = make_tex(DXGI_FORMAT_R16G16_FLOAT, IW, IH, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                      D3D12_RESOURCE_STATE_COPY_DEST, &dm);
    output = make_tex(DXGI_FORMAT_R16G16B16A16_FLOAT, OW, OH, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &dout);
    color_st = motion_st = D3D12_RESOURCE_STATE_COPY_DEST;
    /* The DLL's carrier: a small buffer of its own; the command lands in it
     * only if nothing on the way knows it (status UNSEEN). */
    carrier = make_buf(D3D12_HEAP_TYPE_DEFAULT, 4096, D3D12_RESOURCE_STATE_COMMON);
    {
        UINT64 total, off = 0;
        footprint(&dc, off, &fp_color, &total);
        off = (off + total + 511) & ~511ull;
        footprint(&dm, off, &fp_motion, &total);
        off = (off + total + 511) & ~511ull;
        footprint(&dd, off, &fp_depth, &total);
        off = (off + total + 511) & ~511ull;
        upload = make_buf(D3D12_HEAP_TYPE_UPLOAD, off, D3D12_RESOURCE_STATE_GENERIC_READ);
        footprint(&dout, 0, &fp_out, &total);
        readback = make_buf(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        zeros = make_buf(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
        D3D12_RANGE none = { 0, 0 };
        void *m;
        HR("Map(zeros)", ID3D12Resource_Map(zeros, 0, &none, &m));
        memset(m, 0, total);
        ID3D12Resource_Unmap(zeros, 0, NULL);
        HR("Map(upload)", ID3D12Resource_Map(upload, 0, &none, (void **)&upload_map));
        HR("Map(readback)", ID3D12Resource_Map(readback, 0, NULL, (void **)&readback_map));
        printf("footprints: colour %s pitch %u, depth fmt %d pitch %u, motion pitch %u, output pitch %u\n",
               "RGBA16F", fp_color.Footprint.RowPitch, (int)fp_depth.Footprint.Format, fp_depth.Footprint.RowPitch,
               fp_motion.Footprint.RowPitch, fp_out.Footprint.RowPitch);
    }

    /* The Vulkan side of each resource. */
    vk_color = vk_info(color, "colour");
    vk_depth = vk_info(depth, "depth");
    vk_motion = vk_info(motion, "motion");
    vk_output = vk_info(output, "output");
    {
        VkFormat f;
        HR("GetVulkanResourceInfo1(carrier)",
           ix->lpVtbl->GetVulkanResourceInfo1(ix, carrier, &carrier_buf, &carrier_off, &f));
        if (!carrier_buf) FAILX("GetVulkanResourceInfo1(carrier): no VkBuffer");
    }
    printf("vk: colour image 0x%llx fmt %d, depth 0x%llx fmt %d, motion 0x%llx fmt %d, output 0x%llx fmt %d; "
           "carrier buffer 0x%llx + %llu; queue family %u; VkDevice %p\n",
           (unsigned long long)vk_color.image, vk_color.format, (unsigned long long)vk_depth.image, vk_depth.format,
           (unsigned long long)vk_motion.image, vk_motion.format, (unsigned long long)vk_output.image, vk_output.format,
           (unsigned long long)carrier_buf, (unsigned long long)carrier_off, vfam, (void *)vdev);
    {
        const D3D12_RESOURCE_STATES in_st[3] = { D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                 D3D12_RESOURCE_STATE_COMMON };
        const char *const in_n[3] = { "COPY_DEST", "NON_PIXEL_SHADER_RESOURCE", "COMMON" };
        print_layouts(color, "colour", in_st, in_n, 3);
        print_layouts(motion, "motion", in_st, in_n, 3);
        const D3D12_RESOURCE_STATES d_st[4] = { D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_DEPTH_WRITE };
        const char *const d_n[4] = { "COPY_DEST", "NON_PIXEL_SHADER_RESOURCE", "DEPTH_READ|NPSR", "DEPTH_WRITE" };
        print_layouts(depth, "depth", d_st, d_n, 4);
        const D3D12_RESOURCE_STATES o_st[3] = { D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                                D3D12_RESOURCE_STATE_COPY_DEST };
        const char *const o_n[3] = { "UNORDERED_ACCESS", "COPY_SOURCE", "COPY_DEST" };
        print_layouts(output, "output", o_st, o_n, 3);
    }

    /* PROBE the way the DLL does at ffxCreateContext: a command buffer of a
     * pool of its own on vkd3d's VkDevice, never submitted. */
    {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, vfam };
        VkCommandPool pool;
        if (p_vkCreateCommandPool(vdev, &pci, NULL, &pool) != VK_SUCCESS) FAILX("vkCreateCommandPool on vkd3d's device");
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                           VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
        VkCommandBuffer pcb;
        if (p_vkAllocateCommandBuffers(vdev, &ai, &pcb) != VK_SUCCESS) FAILX("vkAllocateCommandBuffers");
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
        p_vkBeginCommandBuffer(pcb, &bi);
        struct sa_mfxt_cmd p = make_cmd(SA_OP_PROBE, 100, 0, 0, 0);
        p_vkCmdUpdateBuffer(pcb, (VkBuffer)carrier_buf, carrier_off, sizeof p, &p);
        p_vkEndCommandBuffer(pcb);
        p_vkDestroyCommandPool(vdev, pool, NULL);
        if (p.status != SA_ST_OK || !(p.caps_out & SA_MFXT_CAP_TEMPORAL))
            BAD("probe (own pool on vkd3d's VkDevice): status %s caps 0x%x driver %u max scale %.2f", status_name(p.status),
                p.caps_out, p.driver_out, p.max_scale_out);
        else
            OK("probe (own pool on vkd3d's VkDevice): status OK, caps 0x%x, driver %u, max scale %.2f", p.caps_out,
               p.driver_out, p.max_scale_out);
        if (p.status != SA_ST_OK) {
            printf("== d3d12 mfxt interop: FAIL (probe %s: nothing to upscale with)\n", status_name(p.status));
            return 1;
        }
    }

    /* Depth 0.5, uploaded once. */
    {
        unsigned char *base = upload_map + fp_depth.Offset;
        for (int y = 0; y < IH; y++) {
            float *o = (float *)(base + (size_t)y * fp_depth.Footprint.RowPitch);
            for (int x = 0; x < IW; x++) o[x] = 0.5f;
        }
        reset_list(alloc, cl);
        copy_buf_tex(cl, upload, &fp_depth, depth, 1);
        barrier(cl, depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        HR("Close", ID3D12GraphicsCommandList_Close(cl));
        execute_wait(cl);
    }
    printf("setup: %.1f s since start\n", us_since(t_start) / 1e6);

    /* 1. Still picture: the game's jitter, and the opposite sign. */
    struct run good = { "still", -1, 0, 0, 0, 1, frames, resubmit, 1 };
    run_case(&good);
    if (good.status != SA_ST_OK) {
        BAD("encode through the D3D12 command list: status %s", status_name(good.status));
        printf("== d3d12 mfxt interop: FAIL (%d)\n", failures);
        return 1;
    }
    struct run opp = good;
    opp.jitter_mul = 1; opp.context = 2; opp.resubmit = 0; opp.probe = 0;
    run_case(&opp);
    check_still(&good, &opp);
    if (resubmit) {
        printf("    executed again after the output was zeroed: RMSE %.5f (mean %.3f), first time %.5f\n", good.err_again,
               good.mean_again, good.err);
        if (good.err_again < 0 || good.err_again > 0.05 || fabs(good.mean_again - 0.5) > 0.05)
            BAD("command list executed twice: the upscale did not run again (RMSE %.4f, mean %.3f)", good.err_again,
                good.mean_again);
        else
            OK("command list executed twice (recorded again by KosmicKrisp): RMSE %.4f", good.err_again);
    }
    printf("cases 1: %.1f s since start\n", us_since(t_start) / 1e6);

    /* 2. Moving: motion vectors previous minus current, and the opposite. */
    struct run mv = { "moving", -1, 1.37f, 0.61f, -1, 3, frames, 0, 0 };
    run_case(&mv);
    struct run mvp = mv;
    mvp.mv_mul = 1; mvp.context = 4;
    run_case(&mvp);
    printf("    moving (1.37, 0.61)/frame: motion vectors -v %.5f, +v %.5f (mean %.3f)\n", mv.err, mvp.err, mv.mean);
    if (mv.status != SA_ST_OK || mvp.status != SA_ST_OK)
        BAD("moving: encode status %s / %s", status_name(mv.status), status_name(mvp.status));
    else if (mv.err > 0.15 || mv.err >= mvp.err)
        BAD("motion vectors: the game's convention is not the best");
    else
        OK("motion vectors through the D3D12 path: RMSE %.4f (+v %.4f)", mv.err, mvp.err);
    printf("cases 2: %.1f s since start\n", us_since(t_start) / 1e6);

    /* 3. Depth as games often make it: typeless R32 and R32G8X24 (D32S8). */
    {
        const DXGI_FORMAT df[2] = { DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_R32G8X24_TYPELESS };
        const char *dn[2] = { "R32_TYPELESS", "R32G8X24_TYPELESS" };
        ID3D12Resource *depth0 = depth;
        struct vkimg vk_depth0 = vk_depth;
        for (int i = 0; i < 2; i++) {
            D3D12_RESOURCE_DESC d2;
            ID3D12Resource *dr = make_tex(df[i], IW, IH, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                                          D3D12_RESOURCE_STATE_COPY_DEST, &d2);
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp2;
            UINT64 total;
            footprint(&d2, fp_depth.Offset, &fp2, &total);
            reset_list(alloc, cl);
            copy_buf_tex(cl, upload, &fp2, dr, 1);
            barrier(cl, dr, D3D12_RESOURCE_STATE_COPY_DEST,
                    D3D12_RESOURCE_STATE_DEPTH_READ | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            HR("Close", ID3D12GraphicsCommandList_Close(cl));
            execute_wait(cl);
            depth = dr;
            vk_depth = vk_info(dr, dn[i]);
            struct run r = { dn[i], -1, 0, 0, 0, 5 + i, frames / 2 > MEASURE ? frames / 2 : MEASURE + 1, 0, 0 };
            run_case(&r);
            printf("    depth %s: VkImage 0x%llx fmt %d, copy footprint fmt %d, status %s, RMSE %.5f\n", dn[i],
                   (unsigned long long)vk_depth.image, vk_depth.format, (int)fp2.Footprint.Format, status_name(r.status),
                   r.err);
            if (r.status == SA_ST_OK && r.err >= 0 && r.err < 0.05)
                OK("depth %s (Vulkan format %d): status OK, RMSE %.4f", dn[i], vk_depth.format, r.err);
            else
                BAD("depth %s (Vulkan format %d): status %s, RMSE %.4f", dn[i], vk_depth.format, status_name(r.status), r.err);
            depth = depth0;
            vk_depth = vk_depth0;
            ID3D12Resource_Release(dr);
        }
    }

    /* DESTROY every context we made. */
    {
        reset_list(alloc, cl);
        VkCommandBuffer vcb;
        HR("BeginVkCommandBufferInterop", ix->lpVtbl->BeginVkCommandBufferInterop(ix, (ID3D12CommandList *)cl, &vcb));
        uint32_t worst = SA_ST_OK;
        for (uint64_t c = 1; c <= 6; c++) {
            struct sa_mfxt_cmd d = make_cmd(SA_OP_DESTROY, c, 0, 0, 0);
            p_vkCmdUpdateBuffer(vcb, (VkBuffer)carrier_buf, carrier_off, sizeof d, &d);
            if (d.status != SA_ST_OK) worst = d.status;
        }
        HR("EndVkCommandBufferInterop", ix->lpVtbl->EndVkCommandBufferInterop(ix, (ID3D12CommandList *)cl));
        HR("Close", ID3D12GraphicsCommandList_Close(cl));
        execute_wait(cl);
        if (worst != SA_ST_OK) BAD("destroy: status %s", status_name(worst));
        else OK("destroy: status OK");
    }
    printf("total: %.1f s\n", us_since(t_start) / 1e6);
    printf(failures ? "== d3d12 mfxt interop: FAIL (%d)\n" : "== d3d12 mfxt interop: ok\n", failures);
    fflush(stdout);
    ExitProcess(failures != 0);
    return failures != 0;
}
