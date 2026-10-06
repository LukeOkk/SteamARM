// A D3D12 program that uses the FidelityFX API the way a game does, to test
// SteamARM's amd_fidelityfx_dx12.dll (tools/ffx-metalfx) against AMD's:
//
//   LoadLibrary("amd_fidelityfx_dx12.dll") (Wine's override decides whose),
//   ffxQuery(GET_VERSIONS), ffxCreateContext(upscale 1280x720 -> 1920x1080,
//   DX12 backend), the queries a game makes (render resolution, upscale
//   ratio, jitter phase count and offsets, provider version, GPU memory),
//   then ffxDispatch of a synthetic picture: tests/win/d3d12_mfxt_interop.c's
//   pattern with the jitter the DLL itself hands out (the game's jitter is
//   minus the sample offset, as AMD's sample does), still and then moving
//   (motion vectors from the current position to the previous one), the
//   output read back and compared with a supersampled reference (RMSE).
//
// With orig=<path> the same queries are also asked of that DLL (AMD's own
// copy under another name, loaded in the same process) and the answers
// compared: "queries: identical (N)" or each difference. With out=<path>
// the last output frame is written raw (RGBA16F rows, no padding), so two
// runs can be compared bit for bit (FORWARD mode against AMD's DLL).
//
// Arguments: frames=N (per case, default 24), out=PATH, orig=PATH,
// dll=NAME (default amd_fidelityfx_dx12.dll), still=0|1 moving=0|1 (cases,
// default both), maxerr=F (the RMSE both cases must stay under, default
// 0.06 still and 0.15 moving: a plain bilinear blit of the input, the
// DLL's answer with no upscaler at all, needs more). Built and run by
// tests/win/run_d3d12_ffx.sh.
// Prints "== d3d12 ffx: ok" or "== d3d12 ffx: FAIL (why)".
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

enum { IW = 1280, IH = 720, OW = 1920, OH = 1080, MEASURE = 4 };

/* ---- the FidelityFX API (ffx_api.h, ffx_upscale.h, dx12/ffx_api_dx12.h) ---- */
typedef uint32_t ffxReturnCode_t;
typedef void *ffxContext;
typedef struct ffxApiHeader { uint64_t type; struct ffxApiHeader *pNext; } ffxApiHeader;
typedef ffxReturnCode_t (*PfnCreate)(ffxContext *, ffxApiHeader *, const void *);
typedef ffxReturnCode_t (*PfnDestroy)(ffxContext *, const void *);
typedef ffxReturnCode_t (*PfnConfigure)(ffxContext *, const ffxApiHeader *);
typedef ffxReturnCode_t (*PfnQuery)(ffxContext *, ffxApiHeader *);
typedef ffxReturnCode_t (*PfnDispatch)(ffxContext *, const ffxApiHeader *);
struct ffxapi { HMODULE mod; PfnCreate create; PfnDestroy destroy; PfnConfigure configure; PfnQuery query; PfnDispatch dispatch; };

struct FfxApiDimensions2D { uint32_t width, height; };
struct FfxApiFloatCoords2D { float x, y; };
struct FfxApiResourceDescription { uint32_t type, format, width, height, depth, mipCount, flags, usage; };
struct FfxApiResource { void *resource; struct FfxApiResourceDescription description; uint32_t state; };
struct FfxApiEffectMemoryUsage { uint64_t totalUsageInBytes, aliasableUsageInBytes; };
struct ffxCreateContextDescUpscale { ffxApiHeader header; uint32_t flags; struct FfxApiDimensions2D maxRenderSize, maxUpscaleSize; void *fpMessage; };
struct ffxCreateBackendDX12Desc { ffxApiHeader header; ID3D12Device *device; };
struct ffxDispatchDescUpscale {
    ffxApiHeader header; void *commandList;
    struct FfxApiResource color, depth, motionVectors, exposure, reactive, transparencyAndComposition, output;
    struct FfxApiFloatCoords2D jitterOffset, motionVectorScale;
    struct FfxApiDimensions2D renderSize, upscaleSize;
    BOOLEAN enableSharpening; float sharpness, frameTimeDelta, preExposure; BOOLEAN reset;
    float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor; uint32_t flags;
};
struct ffxQueryDescGetVersions { ffxApiHeader header; uint64_t createDescType; void *device; uint64_t *outputCount; uint64_t *versionIds; const char **versionNames; };
struct ffxQueryGetProviderVersion { ffxApiHeader header; uint64_t versionId; const char *versionName; };
struct ffxQueryRatio { ffxApiHeader header; uint32_t qualityMode; float *pOutUpscaleRatio; };
struct ffxQueryRenderRes { ffxApiHeader header; uint32_t displayWidth, displayHeight, qualityMode; uint32_t *pOutRenderWidth, *pOutRenderHeight; };
struct ffxQueryPhaseCount { ffxApiHeader header; uint32_t renderWidth, displayWidth; int32_t *pOutPhaseCount; };
struct ffxQueryJitter { ffxApiHeader header; int32_t index, phaseCount; float *pOutX, *pOutY; };
struct ffxQueryGpuMem { ffxApiHeader header; struct FfxApiEffectMemoryUsage *usage; };
struct sa_ffx_query_ident { ffxApiHeader header; char who[64]; uint32_t marker; };
#define T_CREATE_UPSCALE 0x00010000u
#define T_DISPATCH_UPSCALE 0x00010001u
#define T_Q_RATIO 0x00010002u
#define T_Q_RENDERRES 0x00010003u
#define T_Q_PHASECOUNT 0x00010004u
#define T_Q_JITTER 0x00010005u
#define T_Q_GPUMEM 0x00010008u
#define T_BACKEND_DX12 2u
#define T_Q_GET_VERSIONS 4u
#define T_Q_PROVIDER_VERSION 6u
#define SA_FFX_QUERY_IDENT 0x53410001ull
#define FMT_RGBA16F 4u
#define FMT_R32F 28u
#define FMT_RG16F 18u
#define ST_UAV 2u
#define ST_COMPUTE_READ 4u
_Static_assert(sizeof(struct ffxDispatchDescUpscale) == 432, "432");

/* ---- globals ---- */
static int failures;
#define FAILX(...) do { printf(__VA_ARGS__); printf("\n== d3d12 ffx: FAIL\n"); fflush(stdout); ExitProcess(1); } while (0)
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
static ID3D12Resource *color, *depth, *motion, *output, *upload, *readback;
static D3D12_RESOURCE_STATES color_st, motion_st;
static D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp_color, fp_motion, fp_depth, fp_out;
static unsigned char *upload_map, *readback_map;
static struct ffxapi ffx, orig;
static ffxContext ctx;

static DWORD WINAPI watchdog(LPVOID arg)
{
    (void)arg;
    Sleep(55000);
    printf("== d3d12 ffx: FAIL (watchdog: not done in 55 s)\n");
    fflush(stdout);
    ExitProcess(3);
    return 0;
}

/* ---- the picture (tests/win/d3d12_mfxt_interop.c) ---- */
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
static ID3D12Resource *make_tex(DXGI_FORMAT f, UINT w, UINT h, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES st, D3D12_RESOURCE_DESC *out_desc)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC d = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, w, h, 1, 1, f, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, flags };
    ID3D12Resource *r = NULL;
    HR("CreateCommittedResource(texture)", ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **)&r));
    if (out_desc) *out_desc = d;
    return r;
}
static ID3D12Resource *make_buf(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp = { type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC d = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, size, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    ID3D12Resource *r = NULL;
    HR("CreateCommittedResource(buffer)", ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, st, NULL, &IID_ID3D12Resource, (void **)&r));
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
static void copy_buf_tex(ID3D12GraphicsCommandList *l, ID3D12Resource *buf, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT *fp, ID3D12Resource *tex, int to_tex)
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
        if (WaitForSingleObject(fence_event, 10000) != WAIT_OBJECT_0) FAILX("GPU did not finish in 10 s");
    }
}
static void reset_list(void)
{
    HR("CommandAllocator::Reset", ID3D12CommandAllocator_Reset(alloc));
    HR("CommandList::Reset", ID3D12GraphicsCommandList_Reset(cl, alloc, NULL));
}

/* ---- the DLLs ---- */
static int load_api(struct ffxapi *a, const char *name)
{
    a->mod = LoadLibraryA(name);
    if (!a->mod) { printf("LoadLibraryA(%s) failed: %lu\n", name, GetLastError()); return 0; }
    a->create = (PfnCreate)(void *)GetProcAddress(a->mod, "ffxCreateContext");
    a->destroy = (PfnDestroy)(void *)GetProcAddress(a->mod, "ffxDestroyContext");
    a->configure = (PfnConfigure)(void *)GetProcAddress(a->mod, "ffxConfigure");
    a->query = (PfnQuery)(void *)GetProcAddress(a->mod, "ffxQuery");
    a->dispatch = (PfnDispatch)(void *)GetProcAddress(a->mod, "ffxDispatch");
    if (!a->create || !a->destroy || !a->configure || !a->query || !a->dispatch) { printf("%s: exports missing\n", name); return 0; }
    char path[MAX_PATH];
    GetModuleFileNameA(a->mod, path, MAX_PATH);
    struct sa_ffx_query_ident id;
    memset(&id, 0, sizeof id);
    id.header.type = SA_FFX_QUERY_IDENT;
    strcpy(id.who, "(no answer)");
    ffxReturnCode_t rc = a->query(NULL, &id.header);
    printf("dll %s: %s, ident query rc %u who %s marker %u\n", name, path, rc, id.who, id.marker);
    return 1;
}

/* ---- the queries, as one record so two DLLs can be compared ---- */
struct answers {
    uint32_t rr[3][5][2];     /* render resolution per display size and quality mode */
    float ratio[5];
    int32_t phase[4];
    float jitter[2][10][2];   /* phase 18 and 32, index 0..9 */
    uint32_t rc_bad;          /* any non-OK return */
};
static const uint32_t disp[3][2] = { { 1920, 1080 }, { 2560, 1440 }, { 3840, 2160 } };
static const uint32_t phase_q[4][2] = { { 1280, 1920 }, { 1707, 2560 }, { 1280, 3840 }, { 960, 1920 } };

static void ask(struct ffxapi *a, ffxContext *c, struct answers *o)
{
    memset(o, 0, sizeof *o);
    for (int d = 0; d < 3; d++)
        for (uint32_t m = 0; m < 5; m++) {
            struct ffxQueryRenderRes q = { { T_Q_RENDERRES, NULL }, disp[d][0], disp[d][1], m, &o->rr[d][m][0], &o->rr[d][m][1] };
            if (a->query(c, &q.header)) o->rc_bad++;
        }
    for (uint32_t m = 0; m < 5; m++) {
        struct ffxQueryRatio q = { { T_Q_RATIO, NULL }, m, &o->ratio[m] };
        if (a->query(c, &q.header)) o->rc_bad++;
    }
    for (int i = 0; i < 4; i++) {
        struct ffxQueryPhaseCount q = { { T_Q_PHASECOUNT, NULL }, phase_q[i][0], phase_q[i][1], &o->phase[i] };
        if (a->query(c, &q.header)) o->rc_bad++;
    }
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 10; i++) {
            struct ffxQueryJitter q = { { T_Q_JITTER, NULL }, i, p ? 32 : 18, &o->jitter[p][i][0], &o->jitter[p][i][1] };
            if (a->query(c, &q.header)) o->rc_bad++;
        }
}

static int compare(const char *what, const struct answers *x, const struct answers *y)
{
    int diff = 0;
    for (int d = 0; d < 3; d++)
        for (int m = 0; m < 5; m++)
            if (x->rr[d][m][0] != y->rr[d][m][0] || x->rr[d][m][1] != y->rr[d][m][1]) {
                printf("    differ: render resolution %ux%u mode %d: %ux%u vs %ux%u\n", disp[d][0], disp[d][1], m, x->rr[d][m][0], x->rr[d][m][1], y->rr[d][m][0], y->rr[d][m][1]);
                diff++;
            }
    for (int m = 0; m < 5; m++)
        if (x->ratio[m] != y->ratio[m]) { printf("    differ: ratio mode %d: %g vs %g\n", m, x->ratio[m], y->ratio[m]); diff++; }
    for (int i = 0; i < 4; i++)
        if (x->phase[i] != y->phase[i]) { printf("    differ: phase count %u->%u: %d vs %d\n", phase_q[i][0], phase_q[i][1], x->phase[i], y->phase[i]); diff++; }
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < 10; i++)
            if (x->jitter[p][i][0] != y->jitter[p][i][0] || x->jitter[p][i][1] != y->jitter[p][i][1]) {
                printf("    differ: jitter phase %d index %d: (%.7g, %.7g) vs (%.7g, %.7g)\n", p ? 32 : 18, i, x->jitter[p][i][0], x->jitter[p][i][1], y->jitter[p][i][0], y->jitter[p][i][1]);
                diff++;
            }
    if (x->rc_bad || y->rc_bad) { printf("    non-OK query returns: %u vs %u\n", x->rc_bad, y->rc_bad); diff++; }
    if (diff) BAD("queries: %s: %d differences", what, diff);
    else OK("queries: %s: identical (%d answers)", what, 15 * 2 + 5 + 4 + 40);
    return diff == 0;
}

static void print_answers(const struct answers *o)
{
    printf("    render resolution at 1920x1080: ");
    for (int m = 0; m < 5; m++) printf("%ux%u ", o->rr[0][m][0], o->rr[0][m][1]);
    printf("\n    ratios: ");
    for (int m = 0; m < 5; m++) printf("%g ", o->ratio[m]);
    printf("\n    phase counts: ");
    for (int i = 0; i < 4; i++) printf("%u->%u:%d ", phase_q[i][0], phase_q[i][1], o->phase[i]);
    printf("\n    jitter (phase 18, 0..3): ");
    for (int i = 0; i < 4; i++) printf("(%.5f, %.5f) ", o->jitter[0][i][0], o->jitter[0][i][1]);
    printf("\n");
}

/* ---- one case ---- */
struct run {
    const char *name;
    float vx, vy;
    int frames;
    uint32_t rc_bad;
    double err, mean;
    uint64_t hash;
};

static struct FfxApiResource res(ID3D12Resource *r, uint32_t fmt, uint32_t w, uint32_t h, uint32_t state)
{
    struct FfxApiResource x;
    memset(&x, 0, sizeof x);
    x.resource = r;
    x.description.type = 2;   /* TEXTURE2D */
    x.description.format = fmt;
    x.description.width = w;
    x.description.height = h;
    x.description.depth = 1;
    x.description.mipCount = 1;
    x.state = state;
    return x;
}

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
    uint16_t mx = f2h(-r->vx), my = f2h(-r->vy);   /* current -> previous, in pixels */
    for (int y = 0; y < IH; y++) {
        uint16_t *o = (uint16_t *)(base + (size_t)y * fp_motion.Footprint.RowPitch);
        for (int x = 0; x < IW; x++) { o[x * 2] = mx; o[x * 2 + 1] = my; }
    }
}
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
                    ref += pattern(((float)x + (i + 0.5f) / 4) * k - r->vx * (float)t, ((float)y + (j + 0.5f) / 4) * k - r->vy * (float)t);
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
static uint64_t hash_readback(void)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (int y = 0; y < OH; y++) {
        const unsigned char *row = readback_map + fp_out.Offset + (size_t)y * fp_out.Footprint.RowPitch;
        for (int i = 0; i < OW * 8; i++) { h ^= row[i]; h *= 0x100000001b3ull; }
    }
    return h;
}
static void save_readback(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("    (cannot write %s)\n", path); return; }
    for (int y = 0; y < OH; y++) fwrite(readback_map + fp_out.Offset + (size_t)y * fp_out.Footprint.RowPitch, 1, OW * 8, f);
    fclose(f);
    printf("    output written: %s (%d x %d RGBA16F)\n", path, OW, OH);
}

static void run_case(struct run *r, const char *out_path)
{
    double err = 0, mean = 0;
    int nerr = 0;
    write_motion(r);
    int32_t phase = 0;
    {
        struct ffxQueryPhaseCount q = { { T_Q_PHASECOUNT, NULL }, IW, OW, &phase };
        if (ffx.query(&ctx, &q.header) || phase <= 0) FAILX("jitter phase count query failed (%d)", phase);
    }
    for (int t = 0; t < r->frames; t++) {
        float jx = 0, jy = 0;
        struct ffxQueryJitter q = { { T_Q_JITTER, NULL }, t, phase, &jx, &jy };
        if (ffx.query(&ctx, &q.header)) FAILX("jitter offset query failed at %d", t);
        write_color(t, jx, jy, r);   /* the sample offset is the jitter; the game gives FSR minus it */
        int measure = t >= r->frames - MEASURE;
        reset_list();
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
        struct ffxDispatchDescUpscale d;
        memset(&d, 0, sizeof d);
        d.header.type = T_DISPATCH_UPSCALE;
        d.commandList = cl;
        d.color = res(color, FMT_RGBA16F, IW, IH, ST_COMPUTE_READ);
        d.depth = res(depth, FMT_R32F, IW, IH, ST_COMPUTE_READ);
        d.motionVectors = res(motion, FMT_RG16F, IW, IH, ST_COMPUTE_READ);
        d.output = res(output, FMT_RGBA16F, OW, OH, ST_UAV);
        d.jitterOffset.x = -jx;
        d.jitterOffset.y = -jy;
        d.motionVectorScale.x = d.motionVectorScale.y = 1.0f;
        d.renderSize.width = IW; d.renderSize.height = IH;
        d.upscaleSize.width = OW; d.upscaleSize.height = OH;
        d.frameTimeDelta = 16.6f;
        d.preExposure = 1.0f;
        d.reset = t == 0;
        d.cameraNear = 0.1f; d.cameraFar = 1000.0f; d.cameraFovAngleVertical = 1.0f; d.viewSpaceToMetersFactor = 1.0f;
        ffxReturnCode_t rc = ffx.dispatch(&ctx, &d.header);
        if (rc) { r->rc_bad++; if (r->rc_bad == 1) printf("    dispatch at frame %d: rc %u\n", t, rc); }
        if (measure) {
            barrier(cl, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            copy_buf_tex(cl, readback, &fp_out, output, 0);
            barrier(cl, output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        }
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
    r->hash = hash_readback();
    printf("    %s: %d frames (phase count %d), dispatch rc bad %u, RMSE %.5f, mean %.3f, last frame hash %016llx\n",
           r->name, r->frames, phase, r->rc_bad, r->err, r->mean, (unsigned long long)r->hash);
    if (out_path) save_readback(out_path);
}

int main(int argc, char **argv)
{
    int frames = 24, do_still = 1, do_moving = 1;
    double still_max = 0.06, moving_max = 0.15;
    const char *out_path = NULL, *orig_path = NULL, *dll = "amd_fidelityfx_dx12.dll";
    for (int i = 1; i < argc; i++) {
        if (!strncmp(argv[i], "frames=", 7)) frames = atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "maxerr=", 7)) still_max = moving_max = atof(argv[i] + 7);
        else if (!strncmp(argv[i], "out=", 4)) out_path = argv[i] + 4;
        else if (!strncmp(argv[i], "orig=", 5)) orig_path = argv[i] + 5;
        else if (!strncmp(argv[i], "dll=", 4)) dll = argv[i] + 4;
        else if (!strncmp(argv[i], "still=", 6)) do_still = atoi(argv[i] + 6);
        else if (!strncmp(argv[i], "moving=", 7)) do_moving = atoi(argv[i] + 7);
    }
    if (frames < MEASURE + 1) frames = MEASURE + 1;
    setvbuf(stdout, NULL, _IONBF, 0);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);
    pattern_init();
    char env[64];
    printf("env: STEAMARM_WIN_UPSCALER=%s VKD3D_FEATURE_LEVEL=%s\n",
           GetEnvironmentVariableA("STEAMARM_WIN_UPSCALER", env, sizeof env) ? env : "(unset)",
           getenv("VKD3D_FEATURE_LEVEL") ? getenv("VKD3D_FEATURE_LEVEL") : "(unset)");

    HRESULT hr = D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device, (void **)&dev);
    printf("D3D12CreateDevice(12_0): 0x%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) HR("D3D12CreateDevice(11_0)", D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    HR("CreateCommandQueue", ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
    HR("CreateCommandAllocator", ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc));
    HR("CreateCommandList", ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl));
    ID3D12GraphicsCommandList_Close(cl);
    HR("CreateFence", ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    D3D12_RESOURCE_DESC dc, dd, dm, dout;
    color = make_tex(DXGI_FORMAT_R16G16B16A16_FLOAT, IW, IH, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST, &dc);
    depth = make_tex(DXGI_FORMAT_D32_FLOAT, IW, IH, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_COPY_DEST, &dd);
    motion = make_tex(DXGI_FORMAT_R16G16_FLOAT, IW, IH, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST, &dm);
    output = make_tex(DXGI_FORMAT_R16G16B16A16_FLOAT, OW, OH, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, &dout);
    color_st = motion_st = D3D12_RESOURCE_STATE_COPY_DEST;
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
        D3D12_RANGE none = { 0, 0 };
        HR("Map(upload)", ID3D12Resource_Map(upload, 0, &none, (void **)&upload_map));
        HR("Map(readback)", ID3D12Resource_Map(readback, 0, NULL, (void **)&readback_map));
    }
    /* Depth 0.5, uploaded once. */
    {
        unsigned char *base = upload_map + fp_depth.Offset;
        for (int y = 0; y < IH; y++) {
            float *o = (float *)(base + (size_t)y * fp_depth.Footprint.RowPitch);
            for (int x = 0; x < IW; x++) o[x] = 0.5f;
        }
        reset_list();
        copy_buf_tex(cl, upload, &fp_depth, depth, 1);
        barrier(cl, depth, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        HR("Close", ID3D12GraphicsCommandList_Close(cl));
        execute_wait(cl);
    }

    /* The DLL, as a game loads it. */
    if (!load_api(&ffx, dll)) FAILX("no FidelityFX API DLL");
    {
        struct ffxQueryDescGetVersions q;
        uint64_t n = 0, ids[8] = { 0 };
        const char *names[8] = { 0 };
        memset(&q, 0, sizeof q);
        q.header.type = T_Q_GET_VERSIONS;
        q.createDescType = T_CREATE_UPSCALE;
        q.device = dev;
        q.outputCount = &n;
        ffxReturnCode_t rc = ffx.query(NULL, &q.header);
        printf("versions: count query rc %u, %llu versions\n", rc, (unsigned long long)n);
        if (n > 8) n = 8;
        q.versionIds = ids;
        q.versionNames = names;
        rc = ffx.query(NULL, &q.header);
        for (uint64_t i = 0; i < n; i++) printf("    version[%llu] 0x%016llx %s\n", (unsigned long long)i, (unsigned long long)ids[i], names[i] ? names[i] : "(null)");
        if (rc || !n) BAD("version list: rc %u, %llu entries", rc, (unsigned long long)n);
        else OK("version list: %llu entries, first %s", (unsigned long long)n, names[0] ? names[0] : "?");
    }
    struct ffxCreateContextDescUpscale cd;
    struct ffxCreateBackendDX12Desc bd;
    memset(&cd, 0, sizeof cd);
    memset(&bd, 0, sizeof bd);
    cd.header.type = T_CREATE_UPSCALE;
    cd.header.pNext = &bd.header;
    cd.maxRenderSize.width = IW; cd.maxRenderSize.height = IH;
    cd.maxUpscaleSize.width = OW; cd.maxUpscaleSize.height = OH;
    bd.header.type = T_BACKEND_DX12;
    bd.device = dev;
    LARGE_INTEGER qpf, t0, t1;
    QueryPerformanceFrequency(&qpf);
    QueryPerformanceCounter(&t0);
    ffxReturnCode_t rc = ffx.create(&ctx, &cd.header, NULL);
    QueryPerformanceCounter(&t1);
    printf("ffxCreateContext(upscale %dx%d -> %dx%d): rc %u, ctx %p, %.1f ms\n", IW, IH, OW, OH, rc, ctx, (double)(t1.QuadPart - t0.QuadPart) * 1e3 / qpf.QuadPart);
    if (rc || !ctx) FAILX("no upscale context");
    {
        struct ffxQueryGetProviderVersion q = { { T_Q_PROVIDER_VERSION, NULL }, 0, NULL };
        rc = ffx.query(&ctx, &q.header);
        printf("provider version: rc %u, 0x%016llx %s\n", rc, (unsigned long long)q.versionId, q.versionName ? q.versionName : "(null)");
        struct FfxApiEffectMemoryUsage u = { 0, 0 };
        struct ffxQueryGpuMem m = { { T_Q_GPUMEM, NULL }, &u };
        rc = ffx.query(&ctx, &m.header);
        printf("gpu memory: rc %u, %.1f MB total, %.1f MB aliasable\n", rc, u.totalUsageInBytes / 1048576.0, u.aliasableUsageInBytes / 1048576.0);
    }
    struct answers a_ctx, a_null, a_orig;
    ask(&ffx, &ctx, &a_ctx);
    ask(&ffx, NULL, &a_null);
    print_answers(&a_ctx);
    compare("context vs null context", &a_ctx, &a_null);
    if (orig_path) {
        if (!load_api(&orig, orig_path)) BAD("orig DLL not loadable");
        else {
            ask(&orig, NULL, &a_orig);
            compare("ours vs AMD's DLL", &a_ctx, &a_orig);
        }
    }

    struct run still = { "still", 0, 0, frames, 0, 0, 0, 0 };
    struct run moving = { "moving (1.37, 0.61)/frame", 1.37f, 0.61f, frames, 0, 0, 0, 0 };
    if (do_still) {
        run_case(&still, do_moving ? NULL : out_path);
        if (still.rc_bad) BAD("still: %u dispatches returned an error", still.rc_bad);
        else if (still.err > still_max || fabs(still.mean - 0.5) > 0.05) BAD("still: RMSE %.4f (limit %.3f), mean %.3f: the picture is wrong", still.err, still_max, still.mean);
        else OK("still: dispatch OK every frame, RMSE %.4f (limit %.3f)", still.err, still_max);
    }
    if (do_moving) {
        run_case(&moving, out_path);
        if (moving.rc_bad) BAD("moving: %u dispatches returned an error", moving.rc_bad);
        else if (moving.err > moving_max || fabs(moving.mean - 0.5) > 0.05) BAD("moving: RMSE %.4f (limit %.3f), mean %.3f: the picture is wrong", moving.err, moving_max, moving.mean);
        else OK("moving: dispatch OK every frame, RMSE %.4f (limit %.3f)", moving.err, moving_max);
    }
    rc = ffx.destroy(&ctx, NULL);
    printf("ffxDestroyContext: rc %u\n", rc);
    if (rc) BAD("destroy: rc %u", rc);
    printf(failures ? "== d3d12 ffx: FAIL (%d)\n" : "== d3d12 ffx: ok\n", failures);
    fflush(stdout);
    ExitProcess(failures != 0);
    return failures != 0;
}
