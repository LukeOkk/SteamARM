// DLSS -> MetalFX temporal through SteamARM's nvngx.dll (tools/dlss-metalfx),
// the way a Unity / Unreal game uses NVIDIA's NGX on Direct3D 12:
//
//   LoadLibrary(nvngx.dll) [or _nvngx.dll by its System32 path, as the
//   static NGX SDK does], NVSDK_NGX_D3D12_Init_with_ProjectID,
//   GetCapabilityParameters (SuperSampling.Available, the optimal-settings
//   callback for every quality mode), AllocateParameters, CreateFeature
//   (SuperSampling, 1280x720 -> 1920x1080) on an open command list,
//   EvaluateFeature with Colour/Depth/MotionVectors/Output, the jitter
//   and motion vectors of a known picture for N frames, ReleaseFeature,
//   DestroyParameters, Shutdown1.
//
// The parameters go through NVSDK_NGX_Parameter's C++ vtable in MSVC's
// order (Set(void*) first ... Set(unsigned long long), the Gets likewise,
// Reset: tools/dlss-metalfx/nvngx.c) AND through the C exports
// (NVSDK_NGX_Parameter_SetUI...), checked against each other.
//
// The picture, textures and checks are tests/win/d3d12_mfxt_interop.c's:
// colour RGBA16F, depth D32, motion RG16F at the render size, output RGBA16F
// (a UAV); a pattern with a known sub-pixel jitter (Halton 2,3; the jitter
// given to DLSS is minus the sample offset, as a game's projection jitter)
// and, moving, known motion vectors (previous minus current position, in
// render pixels); the output read back and compared with a supersampled
// reference over the last frames (RMSE).
//
// Checks: every NGX call's result; SuperSampling.Available = 1 (0 with
// expect=unavailable: MoltenVK, KK_MFXT=0, where CreateFeature must then
// refuse); the render size per quality mode (Quality 1280x720 of
// 1920x1080); the still picture's RMSE, better with the game's jitter sign
// than the opposite; motion vectors likewise; the fallback: a feature of
// 480x270 -> 1920x1080 (4x, more than MetalFX's largest scale) is refused
// by the driver and the DLL blits the colour input onto the output instead
// (an image, not black), the NGX call still succeeding.
//
// Then DLSS 1, in every mode (the path is the shim's, on either driver):
// Shutdown1, NVSDK_NGX_D3D12_Init as a 2019 game (SDK version 0x12, below
// 0x13), SuperSampling.Available must be 1, a feature 1280x720 ->
// 1920x1080 evaluated for N frames with Colour, Output and Sharpness only:
// the DLL must have sent the shim's SPATIAL (stats: path 1, no refusal,
// status OK) and the output must be the upscaled picture (RMSE against the
// supersampled scene, a mean of 0.5). STEAMARM_NGX_SPATIAL=0 and a
// nvngx_dlss.dll next to the program would override the SDK version's
// verdict: neither is set here.
//
// Arguments: frames=N (per case, default 40), load=name|core|sys32 (how
// the DLL is loaded: "nvngx.dll" by name, "_nvngx.dll" by name, or
// C:\windows\system32\_nvngx.dll by path), expect=available|unavailable.
// Built and run by tests/win/run_d3d12_dlss.sh.
// Prints "== d3d12 dlss: ok", "...: FAIL (n)" or "...: FAIL (why)".
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

enum { OW = 1920, OH = 1080, MEASURE = 4 };

/* ---- NGX values (public DLSS SDK headers) ---- */
#define NGX_OK 0x1u
#define NGX_FAIL_FeatureNotSupported 0xBAD00001u
#define NGX_Feature_SuperSampling 1
#define NGX_Feature_FrameGeneration 11
#define NGX_DLSS_MVLowRes 2
enum { Q_MaxPerf, Q_Balanced, Q_MaxQuality, Q_UltraPerformance, Q_UltraQuality, Q_DLAA };

typedef unsigned (*pfn_init_pid)(const char *, int, const char *, const wchar_t *, ID3D12Device *, const void *, int);
typedef unsigned (*pfn_init)(unsigned long long, const wchar_t *, ID3D12Device *, const void *, int);
typedef unsigned (*pfn_params)(void **);
typedef unsigned (*pfn_destroy_params)(void *);
typedef unsigned (*pfn_create)(ID3D12GraphicsCommandList *, int, void *, void **);
typedef unsigned (*pfn_release)(void *);
typedef unsigned (*pfn_evaluate)(ID3D12GraphicsCommandList *, const void *, void *, void *);
typedef unsigned (*pfn_shutdown1)(ID3D12Device *);
typedef unsigned (*pfn_cb)(void *);
typedef const char *(*pfn_ident)(void);
typedef int (*pfn_devinfo)(unsigned *);
typedef int (*pfn_stats)(const void *, unsigned *);
typedef void (*pfn_c_set_ui)(void *, const char *, unsigned);
typedef void (*pfn_c_set_f)(void *, const char *, float);
typedef unsigned (*pfn_c_get_ui)(void *, const char *, unsigned *);
typedef unsigned (*pfn_c_get_f)(void *, const char *, float *);
typedef unsigned (*pfn_c_get_i)(void *, const char *, int *);

/* NVSDK_NGX_Parameter's vtable, MSVC's order. */
typedef void (*vt_set_vp)(void *, const char *, void *);
typedef void (*vt_set_i)(void *, const char *, int);
typedef void (*vt_set_ui)(void *, const char *, unsigned);
typedef void (*vt_set_f)(void *, const char *, float);
typedef unsigned (*vt_get_vp)(void *, const char *, void **);
typedef unsigned (*vt_get_i)(void *, const char *, int *);
typedef unsigned (*vt_get_ui)(void *, const char *, unsigned *);
typedef unsigned (*vt_get_f)(void *, const char *, float *);
typedef void (*vt_reset)(void *);
#define VT(p) (*(void ***)(p))
static void SETVP(void *p, const char *k, void *v) { ((vt_set_vp)VT(p)[0])(p, k, v); }
static void SETRES(void *p, const char *k, ID3D12Resource *v) { ((vt_set_vp)VT(p)[1])(p, k, v); }
static void SETI(void *p, const char *k, int v) { ((vt_set_i)VT(p)[3])(p, k, v); }
static void SETUI(void *p, const char *k, unsigned v) { ((vt_set_ui)VT(p)[4])(p, k, v); }
static void SETF(void *p, const char *k, float v) { ((vt_set_f)VT(p)[6])(p, k, v); }
static unsigned GETVP(void *p, const char *k, void **v) { return ((vt_get_vp)VT(p)[8])(p, k, v); }
static unsigned GETI(void *p, const char *k, int *v) { return ((vt_get_i)VT(p)[11])(p, k, v); }
static unsigned GETUI(void *p, const char *k, unsigned *v) { return ((vt_get_ui)VT(p)[12])(p, k, v); }
static unsigned GETF(void *p, const char *k, float *v) { return ((vt_get_f)VT(p)[14])(p, k, v); }

static struct {
    HMODULE h;
    pfn_init_pid init_pid;
    pfn_init init;
    pfn_params get_caps, alloc_params;
    pfn_destroy_params destroy_params;
    pfn_create create;
    pfn_release release;
    pfn_evaluate evaluate_c;
    pfn_shutdown1 shutdown1;
    pfn_ident ident;
    pfn_devinfo devinfo;
    pfn_stats stats;
    pfn_c_set_ui c_set_ui;
    pfn_c_set_f c_set_f;
    pfn_c_get_ui c_get_ui;
    pfn_c_get_f c_get_f;
    pfn_c_get_i c_get_i;
} ngx;

/* ---- globals ---- */
static int failures;
#define FAILX(...) do { printf(__VA_ARGS__); printf("\n== d3d12 dlss: FAIL\n"); fflush(stdout); ExitProcess(1); } while (0)
#define HR(what, expr) do { HRESULT hr_ = (expr); if (FAILED(hr_)) FAILX("%s -> 0x%08lx", what, (unsigned long)hr_); } while (0)
#define OK(...) do { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } while (0)
#define BAD(...) do { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); failures++; } while (0)

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *cl;
static ID3D12Fence *fence;
static UINT64 fence_value;
static HANDLE fence_event;

/* A set of render-size inputs. */
struct inputs {
    int w, h;
    ID3D12Resource *color, *depth, *motion;
    D3D12_RESOURCE_STATES color_st, motion_st;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_color, fp_motion, fp_depth;
    ID3D12Resource *upload;
    unsigned char *upload_map;
};
static ID3D12Resource *output, *readback;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_out;
static unsigned char *readback_map;

static DWORD WINAPI watchdog(LPVOID arg)
{
    (void)arg;
    Sleep(55000);
    printf("== d3d12 dlss: FAIL (watchdog: not done in 55 s)\n");
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

/* ---- the picture (tests/win/d3d12_mfxt_interop.c's) ---- */
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
static void barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if (before == after) return;
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
}
static void copy_buf_tex(ID3D12Resource *buf, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp, ID3D12Resource *tex, int to_tex)
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
    if (to_tex) ID3D12GraphicsCommandList_CopyTextureRegion(cl, &t, 0, 0, 0, &b, NULL);
    else ID3D12GraphicsCommandList_CopyTextureRegion(cl, &b, 0, 0, 0, &t, NULL);
}
static void footprint(const D3D12_RESOURCE_DESC *d, UINT64 offset, D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp, UINT64 *total)
{
    UINT rows;
    UINT64 row_size, bytes;
    ID3D12Device_GetCopyableFootprints(dev, d, 0, 1, offset, fp, &rows, &row_size, &bytes);
    if (total) *total = bytes;
}
static void execute_wait(void)
{
    ID3D12CommandList *lists[1] = { (ID3D12CommandList *)cl };
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    HR("Signal", ID3D12CommandQueue_Signal(queue, fence, ++fence_value));
    if (ID3D12Fence_GetCompletedValue(fence) < fence_value) {
        HR("SetEventOnCompletion", ID3D12Fence_SetEventOnCompletion(fence, fence_value, fence_event));
        if (WaitForSingleObject(fence_event, 10000) != WAIT_OBJECT_0)
            FAILX("GPU did not finish in 10 s (fence %llu)", (unsigned long long)fence_value);
    }
}
static void reset_list(void)
{
    HR("CommandAllocator::Reset", ID3D12CommandAllocator_Reset(alloc));
    HR("CommandList::Reset", ID3D12GraphicsCommandList_Reset(cl, alloc, NULL));
}
static void close_execute(void)
{
    HR("Close", ID3D12GraphicsCommandList_Close(cl));
    execute_wait();
}

static void make_inputs(struct inputs *in, int w, int h)
{
    D3D12_RESOURCE_DESC dc, dd, dm;
    memset(in, 0, sizeof *in);
    in->w = w;
    in->h = h;
    in->color = make_tex(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                         D3D12_RESOURCE_STATE_COPY_DEST, &dc);
    in->depth = make_tex(DXGI_FORMAT_D32_FLOAT, w, h, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
                         D3D12_RESOURCE_STATE_COPY_DEST, &dd);
    in->motion = make_tex(DXGI_FORMAT_R16G16_FLOAT, w, h, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                          D3D12_RESOURCE_STATE_COPY_DEST, &dm);
    in->color_st = in->motion_st = D3D12_RESOURCE_STATE_COPY_DEST;
    UINT64 total, off = 0;
    footprint(&dc, off, &in->fp_color, &total);
    off = (off + total + 511) & ~511ull;
    footprint(&dm, off, &in->fp_motion, &total);
    off = (off + total + 511) & ~511ull;
    footprint(&dd, off, &in->fp_depth, &total);
    off = (off + total + 511) & ~511ull;
    in->upload = make_buf(D3D12_HEAP_TYPE_UPLOAD, off, D3D12_RESOURCE_STATE_GENERIC_READ);
    D3D12_RANGE none = { 0, 0 };
    HR("Map(upload)", ID3D12Resource_Map(in->upload, 0, &none, (void **)&in->upload_map));
    /* Depth 0.5, uploaded once. */
    unsigned char *base = in->upload_map + in->fp_depth.Offset;
    for (int y = 0; y < h; y++) {
        float *o = (float *)(base + (size_t)y * in->fp_depth.Footprint.RowPitch);
        for (int x = 0; x < w; x++) o[x] = 0.5f;
    }
    reset_list();
    copy_buf_tex(in->upload, &in->fp_depth, in->depth, 1);
    barrier(in->depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    close_execute();
}

/* ---- one case ---- */
struct run {
    const char *name;
    struct inputs *in;
    float jitter_mul;   /* jitter given = jitter_mul * sample offset (the game's: -1) */
    float vx, vy;       /* the content moves by +v render pixels a frame */
    float mv_mul;       /* motion vectors written = mv_mul * v */
    int frames;
    int dlss1;          /* DLSS 1 style: Colour, Output, Sharpness, no depth or motion vectors, no jitter in the input */
    float pscale;       /* the pattern's coordinates scaled by this (0: 1). A spatial upscaler cannot undo the
                           aliasing of the full-rate pattern (0.58 and 0.72 cycles a render pixel are past Nyquist;
                           the temporal path accumulates 40 jittered frames): DLSS 1 is checked on the pattern at a
                           quarter of its rate, which one frame holds. */
    /* out */
    unsigned rc_create, rc_eval, rc_release;
    unsigned stats[6];
    double err, mean;
};

static float pscale_of(const struct run *r) { return r->pscale > 0 ? r->pscale : 1.0f; }
static void write_color(const struct run *r, int t, float sx, float sy)
{
    const struct inputs *in = r->in;
    unsigned char *base = in->upload_map + in->fp_color.Offset;
    const float s = pscale_of(r);
    for (int y = 0; y < in->h; y++) {
        uint16_t *o = (uint16_t *)(base + (size_t)y * in->fp_color.Footprint.RowPitch);
        float py = (float)y + 0.5f + sy - r->vy * (float)t;
        for (int x = 0; x < in->w; x++) {
            float px = (float)x + 0.5f + sx - r->vx * (float)t;
            uint16_t v = f2h(pattern(px * s, py * s));
            o[x * 4] = o[x * 4 + 1] = o[x * 4 + 2] = v;
            o[x * 4 + 3] = 0x3c00;
        }
    }
}
static void write_motion(const struct run *r)
{
    const struct inputs *in = r->in;
    unsigned char *base = in->upload_map + in->fp_motion.Offset;
    uint16_t mx = f2h(r->mv_mul * r->vx), my = f2h(r->mv_mul * r->vy);
    for (int y = 0; y < in->h; y++) {
        uint16_t *o = (uint16_t *)(base + (size_t)y * in->fp_motion.Footprint.RowPitch);
        for (int x = 0; x < in->w; x++) { o[x * 2] = mx; o[x * 2 + 1] = my; }
    }
}
/* RMSE of the read-back output against the supersampled scene at frame t. */
static double rmse(const struct run *r, int t, double *mean)
{
    double se = 0, sum = 0;
    long n = 0;
    const float k = (float)r->in->w / OW, s = pscale_of(r);
    for (int y = 16; y < OH - 16; y += 4) {
        const uint16_t *row = (const uint16_t *)(readback_map + fp_out.Offset + (size_t)y * fp_out.Footprint.RowPitch);
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

/* A feature for this case's inputs, N evaluations, the stats, release. */
static void run_case(struct run *r)
{
    struct inputs *in = r->in;
    void *p = NULL, *h = NULL;
    double err = 0, mean = 0;
    int nerr = 0;
    r->err = -1;
    memset(r->stats, 0, sizeof r->stats);
    if (ngx.alloc_params(&p) != NGX_OK || !p) FAILX("AllocateParameters");
    SETUI(p, "CreationNodeMask", 1);
    SETUI(p, "VisibilityNodeMask", 1);
    SETUI(p, "Width", (unsigned)in->w);
    SETUI(p, "Height", (unsigned)in->h);
    SETUI(p, "OutWidth", OW);
    SETUI(p, "OutHeight", OH);
    SETI(p, "PerfQualityValue", Q_MaxQuality);
    SETI(p, "DLSS.Feature.Create.Flags", NGX_DLSS_MVLowRes);
    SETI(p, "DLSS.Enable.Output.Subrects", 0);
    reset_list();
    r->rc_create = ngx.create(cl, NGX_Feature_SuperSampling, p, &h);
    close_execute();
    if (r->rc_create != NGX_OK || !h) {
        printf("    %s: CreateFeature -> 0x%08x (handle %p)\n", r->name, r->rc_create, h);
        ngx.destroy_params(p);
        return;
    }
    write_motion(r);
    r->rc_eval = NGX_OK;
    for (int t = 0; t < r->frames; t++) {
        float sx = halton(t % 32 + 1, 2) - 0.5f, sy = halton(t % 32 + 1, 3) - 0.5f;
        if (r->dlss1) sx = sy = 0.0f;   /* a spatial upscaler knows no jitter */
        write_color(r, t, sx, sy);
        int measure = t >= r->frames - MEASURE;
        reset_list();
        barrier(in->color, in->color_st, D3D12_RESOURCE_STATE_COPY_DEST);
        copy_buf_tex(in->upload, &in->fp_color, in->color, 1);
        in->color_st = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier(in->color, D3D12_RESOURCE_STATE_COPY_DEST, in->color_st);
        if (t == 0) {
            barrier(in->motion, in->motion_st, D3D12_RESOURCE_STATE_COPY_DEST);
            copy_buf_tex(in->upload, &in->fp_motion, in->motion, 1);
            in->motion_st = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier(in->motion, D3D12_RESOURCE_STATE_COPY_DEST, in->motion_st);
        }
        /* The evaluation parameters, as NGX_D3D12_EVALUATE_DLSS_EXT sets them. */
        SETRES(p, "Color", in->color);
        SETRES(p, "Output", output);
        SETRES(p, "Depth", r->dlss1 ? NULL : in->depth);
        SETRES(p, "MotionVectors", r->dlss1 ? NULL : in->motion);
        SETF(p, "Jitter.Offset.X", r->jitter_mul * sx);
        SETF(p, "Jitter.Offset.Y", r->jitter_mul * sy);
        SETF(p, "Sharpness", r->dlss1 ? 0.3f : 0.0f);
        SETI(p, "Reset", t == 0);
        SETF(p, "MV.Scale.X", 1.0f);
        SETF(p, "MV.Scale.Y", 1.0f);
        SETRES(p, "TransparencyMask", NULL);
        SETRES(p, "ExposureTexture", NULL);
        SETVP(p, "DLSS.Input.Bias.Current.Color.Mask", NULL);   /* Set(void*): slot 0 */
        SETUI(p, "DLSS.Input.Color.Subrect.Base.X", 0);
        SETUI(p, "DLSS.Input.Color.Subrect.Base.Y", 0);
        SETUI(p, "DLSS.Input.Depth.Subrect.Base.X", 0);
        SETUI(p, "DLSS.Input.Depth.Subrect.Base.Y", 0);
        SETUI(p, "DLSS.Input.MV.Subrect.Base.X", 0);
        SETUI(p, "DLSS.Input.MV.Subrect.Base.Y", 0);
        SETUI(p, "DLSS.Output.Subrect.Base.X", 0);
        SETUI(p, "DLSS.Output.Subrect.Base.Y", 0);
        ngx.c_set_ui(p, "DLSS.Render.Subrect.Dimensions.Width", (unsigned)in->w);   /* the C exports, too */
        ngx.c_set_ui(p, "DLSS.Render.Subrect.Dimensions.Height", (unsigned)in->h);
        ngx.c_set_f(p, "DLSS.Pre.Exposure", 1.0f);
        ngx.c_set_f(p, "DLSS.Exposure.Scale", 1.0f);
        unsigned rc = ngx.evaluate_c(cl, h, p, NULL);
        if (rc != NGX_OK && r->rc_eval == NGX_OK) r->rc_eval = rc;
        if (measure) {
            barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            copy_buf_tex(readback, &fp_out, output, 0);
            barrier(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
        close_execute();
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
           r->name, r->frames, in->w, in->h, OW, OH, r->rc_eval, r->stats[0], r->stats[1], status_name(r->stats[2]),
           r->stats[4] ? "spatial" : "temporal", r->err, r->mean, r->rc_release);
}

static void *sym(const char *name, int required)
{
    void *f = (void *)GetProcAddress(ngx.h, name);
    if (!f && required) FAILX("GetProcAddress(%s): missing", name);
    return f;
}

int main(int argc, char **argv)
{
    int frames = 40, expect_available = 1;
    const char *load = "name";
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "frames=", 7)) frames = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "load=", 5)) load = argv[i] + 5;
        else if (!strcmp(argv[i], "expect=unavailable")) expect_available = 0;
        else if (!strcmp(argv[i], "expect=available")) expect_available = 1;
    }
    if (frames < MEASURE + 1) frames = MEASURE + 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    pattern_init();

    /* 1. The DLL, as a game gets it. */
    {
        const WCHAR *target = L"nvngx.dll";
        if (!strcmp(load, "core")) target = L"_nvngx.dll";
        else if (!strcmp(load, "sys32")) target = L"C:\\windows\\system32\\_nvngx.dll";
        char env[1024];
        printf("env: WINEDLLPATH=%s", GetEnvironmentVariableA("WINEDLLPATH", env, sizeof env) ? env : "(unset)");
        printf(" WINEDLLOVERRIDES=%s\n", GetEnvironmentVariableA("WINEDLLOVERRIDES", env, sizeof env) ? env : "(unset)");
        ngx.h = LoadLibraryW(target);
        printf("LoadLibraryW(%ls) [%s] -> %p (error %lu)\n", target, load, (void *)ngx.h, ngx.h ? 0 : GetLastError());
        if (!ngx.h) FAILX("the NGX DLL did not load");
        WCHAR file[MAX_PATH];
        GetModuleFileNameW(ngx.h, file, MAX_PATH);
        printf("module: %ls, builtin marker %s\n", file,
               !memcmp((const char *)ngx.h + 0x40, "Wine builtin DLL", 17) ? "yes" : "no");
    }
    ngx.ident = sym("SteamARM_NGX_Ident", 0);
    printf("ident: %s\n", ngx.ident ? ngx.ident() : "(not SteamARM's: no SteamARM_NGX_Ident)");
    if (!ngx.ident) FAILX("not SteamARM's nvngx.dll");
    ngx.init_pid = sym("NVSDK_NGX_D3D12_Init_with_ProjectID", 1);
    ngx.init = sym("NVSDK_NGX_D3D12_Init", 1);
    ngx.get_caps = sym("NVSDK_NGX_D3D12_GetCapabilityParameters", 1);
    ngx.alloc_params = sym("NVSDK_NGX_D3D12_AllocateParameters", 1);
    ngx.destroy_params = sym("NVSDK_NGX_D3D12_DestroyParameters", 1);
    ngx.create = sym("NVSDK_NGX_D3D12_CreateFeature", 1);
    ngx.release = sym("NVSDK_NGX_D3D12_ReleaseFeature", 1);
    ngx.evaluate_c = sym("NVSDK_NGX_D3D12_EvaluateFeature_C", 1);
    ngx.shutdown1 = sym("NVSDK_NGX_D3D12_Shutdown1", 1);
    ngx.devinfo = sym("SteamARM_NGX_DeviceInfo", 1);
    ngx.stats = sym("SteamARM_NGX_FeatureStats", 1);
    ngx.c_set_ui = sym("NVSDK_NGX_Parameter_SetUI", 1);
    ngx.c_set_f = sym("NVSDK_NGX_Parameter_SetF", 1);
    ngx.c_get_ui = sym("NVSDK_NGX_Parameter_GetUI", 1);
    ngx.c_get_f = sym("NVSDK_NGX_Parameter_GetF", 1);
    ngx.c_get_i = sym("NVSDK_NGX_Parameter_GetI", 1);
    {
        static const char *const names[] = {
            "NVSDK_NGX_D3D12_Init", "NVSDK_NGX_D3D12_Init_Ext", "NVSDK_NGX_D3D12_Init_ProjectID", "NVSDK_NGX_D3D12_Shutdown",
            "NVSDK_NGX_D3D12_GetParameters", "NVSDK_NGX_D3D12_PopulateParameters_Impl", "NVSDK_NGX_D3D12_GetScratchBufferSize",
            "NVSDK_NGX_D3D12_EvaluateFeature", "NVSDK_NGX_D3D12_GetFeatureRequirements", "NVSDK_NGX_UpdateFeature",
            "NVSDK_NGX_GetSnippetVersion", "NVSDK_NGX_VULKAN_Init", "NVSDK_NGX_VULKAN_Init_ProjectID",
            "NVSDK_NGX_VULKAN_RequiredExtensions", "NVSDK_NGX_VULKAN_CreateFeature1", "NVSDK_NGX_VULKAN_EvaluateFeature",
            "NVSDK_NGX_VULKAN_GetFeatureRequirements", "NVSDK_NGX_D3D11_Init", "NVSDK_NGX_D3D11_CreateFeature",
            "NVSDK_NGX_Parameter_SetD3d12Resource", "NVSDK_NGX_Parameter_GetVoidPointer", "NVSDK_NGX_Parameter_SetULL",
        };
        int missing = 0;
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
            if (!GetProcAddress(ngx.h, names[i])) { printf("    missing export %s\n", names[i]); missing++; }
        if (missing) BAD("%d NGX exports missing", missing);
        else OK("the NGX exports a game or the static SDK looks up are there (%u checked)", (unsigned)(sizeof names / sizeof names[0]) + 16);
    }

    /* 2. Device, queue, list. */
    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev);
    printf("D3D12CreateDevice(12_0): 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) HR("D3D12CreateDevice(11_0)", D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    HR("CreateCommandQueue", ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    HR("CreateCommandAllocator", ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                     &IID_ID3D12CommandAllocator, (void **)&alloc));
    HR("CreateCommandList", ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL,
                                                           &IID_ID3D12GraphicsCommandList, (void **)&cl));
    ID3D12GraphicsCommandList_Close(cl);
    HR("CreateFence", ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    /* 3. NGX init, the way Unreal's DLSS plugin does it. */
    {
        unsigned rc = ngx.init_pid("steamarm-dlss-test", 1 /* Unreal */, "5.4.0", L"C:\\", dev, NULL, 0x15);
        printf("NVSDK_NGX_D3D12_Init_with_ProjectID -> 0x%08x\n", rc);
        if (rc != NGX_OK) BAD("Init_with_ProjectID -> 0x%08x", rc);
        else OK("NVSDK_NGX_D3D12_Init_with_ProjectID: success");
        unsigned di[4] = { 0, 0, 0, 0 };
        ngx.devinfo(di);
        printf("device: probe %s, caps 0x%x, driver %u, max scale %.2f\n", status_name(di[0]), di[1], di[2], di[3] / 100.0);
        if (!(di[1] & SA_MFXT_CAP_SPATIAL)) BAD("the probe's caps lack SA_MFXT_CAP_SPATIAL: not a v2 shim");
        else OK("the probe reports the shim's spatial pass (caps 0x%x)", di[1]);
    }

    /* 4. Capability parameters: availability through the vtable and the C exports. */
    void *caps = NULL;
    int available = -1;
    {
        if (ngx.get_caps(&caps) != NGX_OK || !caps) FAILX("GetCapabilityParameters");
        int a_vt = -1, a_c = -1, needs = -1, init_res = 0;
        unsigned rc1 = GETI(caps, "SuperSampling.Available", &a_vt), rc2 = ngx.c_get_i(caps, "SuperSampling.Available", &a_c);
        GETI(caps, "SuperSampling.NeedsUpdatedDriver", &needs);
        GETI(caps, "SuperSampling.FeatureInitResult", &init_res);
        int fg = -1, rr = -1;
        GETI(caps, "FrameGeneration.Available", &fg);
        GETI(caps, "SuperSamplingDenoising.Available", &rr);
        printf("caps: SuperSampling.Available vtable %d (0x%x) / C %d (0x%x), NeedsUpdatedDriver %d, FeatureInitResult 0x%x, FrameGeneration %d, RayReconstruction %d\n",
               a_vt, rc1, a_c, rc2, needs, (unsigned)init_res, fg, rr);
        if (rc1 != NGX_OK || rc2 != NGX_OK || a_vt != a_c) BAD("the vtable and the C exports disagree on SuperSampling.Available");
        available = a_vt;
        if (available != expect_available)
            BAD("SuperSampling.Available = %d, expected %d", available, expect_available);
        else
            OK("SuperSampling.Available = %d as expected; frame generation %d, ray reconstruction %d", available, fg, rr);
        if (fg != 0 || rr != 0) BAD("DLSSG / DLSSD must be unavailable");
    }

    /* 5. Optimal settings per quality mode, through the callback. */
    {
        void *cb = NULL;
        if (GETVP(caps, "DLSSOptimalSettingsCallback", &cb) != NGX_OK || !cb) BAD("no DLSSOptimalSettingsCallback");
        else {
            static const struct { int q; const char *name; unsigned w, h; } want[6] = {
                { Q_MaxQuality, "Quality", 1280, 720 }, { Q_Balanced, "Balanced", 1114, 626 },
                { Q_MaxPerf, "Performance", 960, 540 }, { Q_UltraPerformance, "UltraPerformance", 640, 360 },
                { Q_DLAA, "DLAA", 1920, 1080 }, { Q_UltraQuality, "UltraQuality", 1478, 832 },
            };
            int bad = 0;
            for (int i = 0; i < 6; i++) {
                SETUI(caps, "Width", 1920);
                SETUI(caps, "Height", 1080);
                SETI(caps, "PerfQualityValue", want[i].q);
                SETI(caps, "RTXValue", 0);
                unsigned rc = ((pfn_cb)cb)(caps);
                unsigned ow = 0, oh = 0, minw = 0, minh = 0, maxw = 0, maxh = 0, ow_c = 0;
                float sharp = -1, scale = 0;
                GETUI(caps, "OutWidth", &ow);
                GETUI(caps, "OutHeight", &oh);
                ngx.c_get_ui(caps, "OutWidth", &ow_c);
                GETUI(caps, "DLSS.Get.Dynamic.Min.Render.Width", &minw);
                GETUI(caps, "DLSS.Get.Dynamic.Min.Render.Height", &minh);
                GETUI(caps, "DLSS.Get.Dynamic.Max.Render.Width", &maxw);
                GETUI(caps, "DLSS.Get.Dynamic.Max.Render.Height", &maxh);
                GETF(caps, "Sharpness", &sharp);
                ngx.c_get_f(caps, "Scale", &scale);
                printf("    optimal %-16s 1920x1080 -> %ux%u (C %u), dynamic %ux%u..%ux%u, scale %.4f, sharpness %.1f (rc 0x%x)\n",
                       want[i].name, ow, oh, ow_c, minw, minh, maxw, maxh, scale, sharp, rc);
                if (rc != NGX_OK || ow != want[i].w || oh != want[i].h || ow_c != ow || maxw != 1920 || minw > ow || minw == 0) bad++;
            }
            if (bad) BAD("optimal settings: %d quality modes off", bad);
            else OK("optimal settings: Quality 1280x720, Balanced 1114x626, Performance 960x540, UltraPerformance 640x360, DLAA, UltraQuality");
        }
    }

    /* 6. Output and the render-size inputs. */
    D3D12_RESOURCE_DESC dout;
    output = make_tex(DXGI_FORMAT_R16G16B16A16_FLOAT, OW, OH, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                      D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &dout);
    {
        UINT64 total;
        footprint(&dout, 0, &fp_out, &total);
        readback = make_buf(D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
        HR("Map(readback)", ID3D12Resource_Map(readback, 0, NULL, (void **)&readback_map));
    }
    struct inputs quality, small;
    make_inputs(&quality, 1280, 720);

    if (!expect_available) {
        /* Unavailable: a feature must be refused, FeatureNotSupported. */
        struct run r = { "unavailable", &quality, -1, 0, 0, 0, MEASURE + 1, 0 };
        run_case(&r);
        if (r.rc_create == NGX_FAIL_FeatureNotSupported) OK("CreateFeature refused with FeatureNotSupported, as SuperSampling.Available = 0 says");
        else BAD("CreateFeature -> 0x%08x, expected FeatureNotSupported (0x%08x)", r.rc_create, NGX_FAIL_FeatureNotSupported);
        goto dlss1;
    }

    /* 7. Still picture: the game's jitter and the opposite sign. */
    {
        struct run good = { "still", &quality, -1, 0, 0, 0, frames, 0 };
        struct run opp = { "still, opposite jitter", &quality, 1, 0, 0, 0, frames, 0 };
        run_case(&good);
        run_case(&opp);
        if (good.rc_create != NGX_OK || good.rc_eval != NGX_OK || good.rc_release != NGX_OK)
            BAD("still: create 0x%08x evaluate 0x%08x release 0x%08x", good.rc_create, good.rc_eval, good.rc_release);
        else if (good.stats[0] != (unsigned)frames || good.stats[1] != 0 || good.stats[2] != SA_ST_OK)
            BAD("still: %u evaluated, %u refused, last status %s: not every frame went to MetalFX", good.stats[0], good.stats[1],
                status_name(good.stats[2]));
        else if (good.err < 0 || good.err > 0.04 || good.err * 2 > opp.err || fabs(good.mean - 0.5) > 0.05)
            BAD("still: RMSE %.4f (opposite jitter %.4f, mean %.3f): the picture is wrong or the jitter convention is not DLSS's",
                good.err, opp.err, good.mean);
        else
            OK("DLSS evaluate -> MetalFX temporal, every frame OK: RMSE %.4f (opposite jitter %.4f)", good.err, opp.err);
    }

    /* 8. Moving: motion vectors previous minus current, and the opposite. */
    {
        struct run mv = { "moving", &quality, -1, 1.37f, 0.61f, -1, frames, 0 };
        struct run mvp = { "moving, opposite vectors", &quality, -1, 1.37f, 0.61f, 1, frames, 0 };
        run_case(&mv);
        run_case(&mvp);
        if (mv.rc_eval != NGX_OK || mvp.rc_eval != NGX_OK || mv.stats[1] || mvp.stats[1])
            BAD("moving: evaluate 0x%08x / 0x%08x, refused %u / %u", mv.rc_eval, mvp.rc_eval, mv.stats[1], mvp.stats[1]);
        else if (mv.err < 0 || mv.err > 0.15 || mv.err >= mvp.err)
            BAD("motion vectors: RMSE %.4f vs opposite %.4f: DLSS's convention is not the best", mv.err, mvp.err);
        else
            OK("motion vectors as DLSS defines them: RMSE %.4f (opposite %.4f)", mv.err, mvp.err);
    }

    /* 9. The fallback: 4x is more than MetalFX takes; the output must still be an image. */
    {
        make_inputs(&small, 480, 270);
        struct run fb = { "fallback 4x", &small, -1, 0, 0, 0, MEASURE + 4, 0 };
        run_case(&fb);
        if (fb.rc_create != NGX_OK || fb.rc_eval != NGX_OK)
            BAD("fallback: create 0x%08x evaluate 0x%08x (the NGX calls must still succeed)", fb.rc_create, fb.rc_eval);
        else if (fb.stats[1] != fb.stats[0] || fb.stats[2] == SA_ST_OK)
            BAD("fallback: %u of %u refused, last status %s: 4x was not refused by the driver as expected", fb.stats[1],
                fb.stats[0], status_name(fb.stats[2]));
        else if (fb.err < 0 || fabs(fb.mean - 0.5) > 0.08 || fb.err > 0.3)
            BAD("fallback: RMSE %.4f mean %.3f: the blit did not leave an image on the output", fb.err, fb.mean);
        else
            OK("refused encode (%s): the colour input blitted onto the output, NGX success, RMSE %.4f mean %.3f",
               status_name(fb.stats[2]), fb.err, fb.mean);
    }

dlss1:
    /* 10. DLSS 1: a 2019 game's init (SDK version 0x12), its SuperSampling
     * feature goes to the shim's spatial pass (MetalFX spatial on
     * KosmicKrisp, FSR 1 on MoltenVK), on either driver. */
    {
        unsigned rc = ngx.destroy_params(caps);
        unsigned rs = ngx.shutdown1(dev);
        if (rc != NGX_OK || rs != NGX_OK) BAD("DestroyParameters 0x%08x, Shutdown1 0x%08x", rc, rs);
        caps = NULL;
        rc = ngx.init(0x2019ull, L"C:\\", dev, NULL, 0x12);
        printf("NVSDK_NGX_D3D12_Init (SDK 0x12, DLSS 1) -> 0x%08x\n", rc);
        if (rc != NGX_OK) BAD("DLSS 1 init -> 0x%08x", rc);
        unsigned di[4] = { 0, 0, 0, 0 };
        ngx.devinfo(di);
        printf("device: probe %s, caps 0x%x, driver %u, max scale %.2f\n", status_name(di[0]), di[1], di[2], di[3] / 100.0);
        int a = -1;
        if (ngx.get_caps(&caps) != NGX_OK || !caps) FAILX("GetCapabilityParameters (DLSS 1)");
        GETI(caps, "SuperSampling.Available", &a);
        if (a != 1) BAD("DLSS 1: SuperSampling.Available = %d, expected 1 (the shim's spatial pass is there on any driver)", a);
        else {
            /* The pattern at a quarter of its rate, which one frame holds (the
             * full-rate pattern is past Nyquist at the render size: the
             * temporal path's 40 jittered frames resolve it, one frame
             * cannot, whatever the scaler); for comparison the full-rate one
             * too, reported, not judged. */
            struct run d1 = { "dlss1 spatial (quarter-rate pattern)", &quality, 0, 0, 0, 0, frames < 12 ? frames : 12, 1, 0.25f };
            struct run d1f = { "dlss1 spatial (full-rate pattern)", &quality, 0, 0, 0, 0, MEASURE + 2, 1, 1.0f };
            run_case(&d1);
            run_case(&d1f);
            if (d1.rc_create != NGX_OK || d1.rc_eval != NGX_OK || d1.rc_release != NGX_OK)
                BAD("dlss1: create 0x%08x evaluate 0x%08x release 0x%08x", d1.rc_create, d1.rc_eval, d1.rc_release);
            else if (d1.stats[4] != 1 || d1.stats[1] != 0 || d1.stats[2] != SA_ST_OK || d1.stats[0] != (unsigned)d1.frames)
                BAD("dlss1: path %s, %u evaluated, %u refused, last status %s: not every frame went to the shim's spatial pass",
                    d1.stats[4] ? "spatial" : "temporal", d1.stats[0], d1.stats[1], status_name(d1.stats[2]));
            else if (d1.err < 0 || d1.err > 0.06 || fabs(d1.mean - 0.5) > 0.05)
                BAD("dlss1: RMSE %.4f mean %.3f: the spatial pass did not leave the upscaled picture on the output", d1.err, d1.mean);
            else
                OK("DLSS 1 -> the shim's spatial pass (EASU+RCAS, MetalFX spatial on KosmicKrisp), every frame OK: RMSE %.4f mean %.3f (full-rate pattern %.4f)",
                   d1.err, d1.mean, d1f.err);
        }
    }

    {
        unsigned rc = caps ? ngx.destroy_params(caps) : NGX_OK;
        unsigned rs = ngx.shutdown1(dev);
        if (rc != NGX_OK || rs != NGX_OK) BAD("DestroyParameters 0x%08x, Shutdown1 0x%08x", rc, rs);
        else OK("DestroyParameters, Shutdown1: success");
    }
    printf(failures ? "== d3d12 dlss: FAIL (%d)\n" : "== d3d12 dlss: ok\n", failures);
    fflush(stdout);
    ExitProcess(failures != 0);
    return failures != 0;
}
