/*
 * SteamARM nvngx.dll / _nvngx.dll -- NVIDIA's NGX API for DLSS, answered by
 * Apple's MetalFX temporal scaler.
 *
 * A Windows game reaches DLSS through NGX: it loads nvngx.dll itself, or
 * links NVIDIA's NGX SDK statically, which loads _nvngx.dll (the NGX core,
 * normally the driver's) from the directory the registry names
 * (HKLM\Software\NVIDIA Corporation\Global\NGXCore FullPath, C:\Windows\
 * System32 in Proton's prefixes) and lets it load the model DLL the game
 * ships (nvngx_dlss.dll). On this Mac there is no NVIDIA driver and no DLSS:
 * this DLL stands in for the NGX core under both names (one source, two
 * builds; the Wine builtin marker and WINEDLLPATH/WINEDLLOVERRIDES put it
 * where Wine finds it: tools/dlss-metalfx/build.sh, tools/steamarm-fex-proton)
 * and turns the one feature games use for upscaling, SuperSampling (DLSS 2,
 * 3 and 4, SDK 310.x included: the same NGX surface), into the temporal
 * upscale command of shim/mfx_temporal.h, recorded as a vkCmdUpdateBuffer
 * into the game's own command buffer:
 *
 *   D3D12: the command list's VkCommandBuffer and each resource's VkImage
 *          through VKD3D-Proton's ID3D12DXVKInteropDevice1
 *          (tests/win/d3d12_mfxt_interop.c proves that path);
 *   Vulkan: the game's VkCommandBuffer and the VkImages in its
 *          NVSDK_NGX_Resource_VK descriptions.
 *
 * The command crosses winevulkan, FEX's thunks and the Vulkan shim, which
 * hands it to KosmicKrisp's kk_steamarm_upscale_temporal (MetalFX). The
 * status comes back in the same call. There is no DLSS to fall back to: a
 * refused encode (MoltenVK, a format or scale MetalFX does not take, an
 * encode inside a render pass) is answered with a vkCmdBlitImage of the
 * colour input stretched onto the output on the same command buffer, so
 * the game never shows black; the NGX call still succeeds. Whether DLSS is
 * offered at all is decided at NGX init: a PROBE of the channel on the
 * game's device (SuperSampling.Available = 1 only when MetalFX's temporal
 * scaler answered OK), so a game on MoltenVK, or with KK_MFXT=0, keeps its
 * own anti-aliasing.
 *
 * Not DLSS: frame generation (DLSSG) and ray reconstruction (DLSSD) are
 * reported unavailable. DLSS 1 (the per-game networks of 2018-2019: Metro
 * Exodus, Battlefield V, Shadow of the Tomb Raider of 2019) is MetalFX
 * SPATIAL instead: a game is taken for DLSS 1 when the NGX SDK version it
 * gives Init is below 0x13 (the first with NVSDK_NGX_FeatureCommonInfo:
 * [I] from the public headers' history, the 2018-2019 SDKs are not public)
 * or when the nvngx_dlss.dll it ships is a 1.x (its file version, read
 * through version.dll: STEAMARM_NGX_MODEL names it, else it is looked for
 * next to the game's program, in the current directory and next to this
 * DLL); STEAMARM_NGX_SPATIAL=1/0 forces either way. Its SuperSampling
 * features send op SPATIAL (protocol v2) instead of ENCODE: the shim
 * records its FSR 1 EASU (+ RCAS for a sharpness above 0) pass from the
 * colour input onto the output (shim/spatial.c), which KosmicKrisp's FSR-1
 * hook draws with MetalFX's spatial scaler (MoltenVK: FSR 1). SuperSampling
 * is then available when the shim's PROBE answer has SA_MFXT_CAP_SPATIAL,
 * on either driver. D3D11 games: the NGX D3D11 entry points exist so that
 * the game's init succeeds, but SuperSampling is unavailable (DXVK's D3D11
 * has no command-buffer interop).
 *
 * Built by build.sh as the FidelityFX DLLs are (tools/ffx-metalfx): KERNEL32
 * is the only import (no C runtime), vulkan-1.dll is loaded at run time;
 * wine-builtin-mark.py writes the "Wine builtin DLL" marker and Wine's
 * prefer-native flag.
 *
 * STEAMARM_NGX_DEBUG=1 logs every call that matters (stderr and
 * OutputDebugString); otherwise one line per init, feature and first
 * refusal.
 *
 * NVSDK_NGX_Parameter is a C++ object: its 17 virtual functions are called
 * through the vtable in the order MSVC gives them (MEASURED with clang's
 * x86_64-pc-windows-msvc target, benchmarks/stage60-dlss-ngx-metalfx.txt):
 * overloads of one name in reverse order of declaration, Set(void*) first,
 * Set(unsigned long long) last, then the Gets the same way, then Reset.
 */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <stddef.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "../../shim/mfx_temporal.h"
#include "ngx_abi.h"

#ifndef SA_NGX_IDENT
#define SA_NGX_IDENT "steamarm-nvngx-metalfx"
#endif
#ifndef SA_NGX_DLL_NAME
#define SA_NGX_DLL_NAME "nvngx"
#endif

/* ------------------------------------------------------------ freestanding */
void *memcpy(void *d, const void *s, size_t n)
{
    unsigned char *dp = d;
    const unsigned char *sp = s;
    while (n--) *dp++ = *sp++;
    return d;
}
void *memset(void *d, int c, size_t n)
{
    unsigned char *dp = d;
    while (n--) *dp++ = (unsigned char)c;
    return d;
}
int memcmp(const void *a, const void *b, size_t n)
{
    const unsigned char *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q) return *p - *q;
    return 0;
}
static size_t sa_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static int sa_streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* ---------------------------------------------------------------- logging */
static HMODULE g_self;
static int g_debug = -1;

static int debug_on(void)
{
    if (g_debug < 0) {
        char v[8];
        g_debug = GetEnvironmentVariableA("STEAMARM_NGX_DEBUG", v, sizeof v) && v[0] == '1';
    }
    return g_debug;
}

struct line { char b[640]; unsigned n; };
static void L_c(struct line *l, char c) { if (l->n < sizeof l->b - 2) l->b[l->n++] = c; }
static void L_s(struct line *l, const char *s) { while (*s) L_c(l, *s++); }
static void L_u(struct line *l, unsigned long long v)
{
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) L_c(l, t[--n]);
}
static void L_i(struct line *l, long long v) { if (v < 0) { L_c(l, '-'); v = -v; } L_u(l, (unsigned long long)v); }
static void L_x(struct line *l, unsigned long long v)
{
    char t[16];
    int n = 0;
    L_s(l, "0x");
    do { t[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    while (n) L_c(l, t[--n]);
}
static void L_f(struct line *l, double v, int dec)
{
    if (v < 0) { L_c(l, '-'); v = -v; }
    unsigned long long ip = (unsigned long long)v;
    double fr = v - (double)ip;
    L_u(l, ip);
    L_c(l, '.');
    for (int i = 0; i < dec; i++) { fr *= 10; int d = (int)fr; L_c(l, (char)('0' + d)); fr -= d; }
}
static void L_begin(struct line *l) { l->n = 0; L_s(l, "[" SA_NGX_DLL_NAME "-metalfx] "); }
static void L_end(struct line *l)
{
    DWORD n;
    L_c(l, '\n');
    l->b[l->n] = 0;
    OutputDebugStringA(l->b);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, l->b, l->n, &n, NULL);
}
static void say(const char *s) { struct line l; L_begin(&l); L_s(&l, s); L_end(&l); }

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

/* ------------------------------------------------------------------ heap */
static void *xalloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void *xrealloc(void *p, size_t n) { return p ? HeapReAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, p, n) : xalloc(n); }
static void xfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

static CRITICAL_SECTION g_lock;

/* ------------------------------------------------------ NVSDK_NGX_Parameter */
enum { T_NONE, T_ULL, T_F, T_D, T_UI, T_I, T_P };
struct kv {
    char key[80];
    unsigned type;
    union { unsigned long long ull; double d; float f; unsigned ui; int i; void *p; } v;
};
#define PARAM_MAGIC 0x4D524150u
struct param {
    const void *const *vt;
    unsigned magic;
    int persistent;
    CRITICAL_SECTION cs;
    struct kv *kv;
    unsigned n, cap;
};

static struct kv *P_find(struct param *p, const char *key)
{
    for (unsigned i = 0; i < p->n; i++)
        if (sa_streq(p->kv[i].key, key)) return &p->kv[i];
    return NULL;
}

static void P_put(struct param *p, const char *key, unsigned type, const void *val)
{
    if (!p || p->magic != PARAM_MAGIC || !key) return;
    EnterCriticalSection(&p->cs);
    struct kv *e = P_find(p, key);
    if (!e) {
        if (p->n == p->cap) {
            unsigned cap = p->cap ? p->cap * 2 : 64;
            struct kv *nk = xrealloc(p->kv, cap * sizeof *nk);
            if (!nk) { LeaveCriticalSection(&p->cs); return; }
            p->kv = nk;
            p->cap = cap;
        }
        e = &p->kv[p->n++];
        size_t n = sa_strlen(key);
        if (n >= sizeof e->key) n = sizeof e->key - 1;
        memcpy(e->key, key, n);
        e->key[n] = 0;
    }
    e->type = type;
    memset(&e->v, 0, sizeof e->v);
    switch (type) {
    case T_ULL: e->v.ull = *(const unsigned long long *)val; break;
    case T_F: e->v.f = *(const float *)val; break;
    case T_D: e->v.d = *(const double *)val; break;
    case T_UI: e->v.ui = *(const unsigned *)val; break;
    case T_I: e->v.i = *(const int *)val; break;
    case T_P: e->v.p = *(void *const *)val; break;
    }
    LeaveCriticalSection(&p->cs);
}

/* A numeric value as a double and as an integer; pointers count as integers. */
static int P_num(struct param *p, const char *key, double *d, long long *i, void **ptr)
{
    if (!p || p->magic != PARAM_MAGIC || !key) return 0;
    EnterCriticalSection(&p->cs);
    struct kv *e = P_find(p, key);
    int ok = 0;
    if (e) {
        ok = 1;
        if (ptr) *ptr = NULL;
        switch (e->type) {
        case T_ULL: *d = (double)e->v.ull; *i = (long long)e->v.ull; break;
        case T_F: *d = e->v.f; *i = (long long)e->v.f; break;
        case T_D: *d = e->v.d; *i = (long long)e->v.d; break;
        case T_UI: *d = e->v.ui; *i = e->v.ui; break;
        case T_I: *d = e->v.i; *i = e->v.i; break;
        case T_P: *d = (double)(uintptr_t)e->v.p; *i = (long long)(uintptr_t)e->v.p; if (ptr) *ptr = e->v.p; break;
        default: ok = 0;
        }
        if (ptr && e->type != T_P && e->type != T_ULL) *ptr = NULL;
        else if (ptr && e->type == T_ULL) *ptr = (void *)(uintptr_t)e->v.ull;
    }
    LeaveCriticalSection(&p->cs);
    return ok;
}

static int P_geti(struct param *p, const char *key, int dflt) { double d; long long i; return P_num(p, key, &d, &i, NULL) ? (int)i : dflt; }
static unsigned P_getu(struct param *p, const char *key, unsigned dflt) { double d; long long i; return P_num(p, key, &d, &i, NULL) ? (unsigned)i : dflt; }
static float P_getf(struct param *p, const char *key, float dflt) { double d; long long i; return P_num(p, key, &d, &i, NULL) ? (float)d : dflt; }
static void *P_getp(struct param *p, const char *key) { double d; long long i; void *ptr = NULL; return P_num(p, key, &d, &i, &ptr) ? ptr : NULL; }
static int P_has(struct param *p, const char *key) { double d; long long i; return P_num(p, key, &d, &i, NULL); }

/* The vtable's functions: this, key, value (MS x64: rcx, rdx, r8 / xmm2). */
static void P_set_vp(struct param *p, const char *k, void *v) { P_put(p, k, T_P, &v); }
static void P_set_i(struct param *p, const char *k, int v) { P_put(p, k, T_I, &v); }
static void P_set_ui(struct param *p, const char *k, unsigned v) { P_put(p, k, T_UI, &v); }
static void P_set_d(struct param *p, const char *k, double v) { P_put(p, k, T_D, &v); }
static void P_set_f(struct param *p, const char *k, float v) { P_put(p, k, T_F, &v); }
static void P_set_ull(struct param *p, const char *k, unsigned long long v) { P_put(p, k, T_ULL, &v); }
static unsigned P_get_vp(struct param *p, const char *k, void **out)
{
    double d; long long i; void *ptr = NULL;
    if (!out || !P_num(p, k, &d, &i, &ptr)) return NGX_FAIL;
    *out = ptr;
    return NGX_OK;
}
static unsigned P_get_i(struct param *p, const char *k, int *out) { double d; long long i; if (!out || !P_num(p, k, &d, &i, NULL)) return NGX_FAIL; *out = (int)i; return NGX_OK; }
static unsigned P_get_ui(struct param *p, const char *k, unsigned *out) { double d; long long i; if (!out || !P_num(p, k, &d, &i, NULL)) return NGX_FAIL; *out = (unsigned)i; return NGX_OK; }
static unsigned P_get_d(struct param *p, const char *k, double *out) { double d; long long i; if (!out || !P_num(p, k, &d, &i, NULL)) return NGX_FAIL; *out = d; return NGX_OK; }
static unsigned P_get_f(struct param *p, const char *k, float *out) { double d; long long i; if (!out || !P_num(p, k, &d, &i, NULL)) return NGX_FAIL; *out = (float)d; return NGX_OK; }
static unsigned P_get_ull(struct param *p, const char *k, unsigned long long *out)
{
    double d; long long i;
    if (!out || !P_num(p, k, &d, &i, NULL)) return NGX_FAIL;
    *out = (unsigned long long)i;
    return NGX_OK;
}
static void P_reset(struct param *p)
{
    if (!p || p->magic != PARAM_MAGIC) return;
    EnterCriticalSection(&p->cs);
    p->n = 0;
    LeaveCriticalSection(&p->cs);
}

/* MSVC's order (see the top): Set(void*), Set(ID3D12Resource*),
 * Set(ID3D11Resource*), Set(int), Set(unsigned), Set(double), Set(float),
 * Set(unsigned long long); the Gets likewise; Reset. */
static const void *const g_param_vt[17] = {
    (const void *)P_set_vp, (const void *)P_set_vp, (const void *)P_set_vp, (const void *)P_set_i,
    (const void *)P_set_ui, (const void *)P_set_d, (const void *)P_set_f, (const void *)P_set_ull,
    (const void *)P_get_vp, (const void *)P_get_vp, (const void *)P_get_vp, (const void *)P_get_i,
    (const void *)P_get_ui, (const void *)P_get_d, (const void *)P_get_f, (const void *)P_get_ull,
    (const void *)P_reset,
};

static struct param *P_new(int persistent)
{
    struct param *p = xalloc(sizeof *p);
    if (!p) return NULL;
    p->vt = g_param_vt;
    p->magic = PARAM_MAGIC;
    p->persistent = persistent;
    InitializeCriticalSection(&p->cs);
    return p;
}

static void P_free(struct param *p)
{
    if (!p || p->magic != PARAM_MAGIC || p->persistent) return;
    p->magic = 0;
    DeleteCriticalSection(&p->cs);
    xfree(p->kv);
    xfree(p);
}

/* -------------------------------------------- VKD3D-Proton's interop interface */
DEFINE_GUID(IID_ID3D12DXVKInteropDevice1, 0x902d8115, 0x59eb, 0x4406, 0x95, 0x18, 0xfe, 0x00, 0xf9, 0x91, 0xee, 0x65);

typedef struct ID3D12DXVKInteropDevice1 ID3D12DXVKInteropDevice1;
typedef struct ID3D12DXVKInteropDevice1Vtbl {
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(ID3D12DXVKInteropDevice1 *, REFIID, void **);
    ULONG (STDMETHODCALLTYPE *AddRef)(ID3D12DXVKInteropDevice1 *);
    ULONG (STDMETHODCALLTYPE *Release)(ID3D12DXVKInteropDevice1 *);
    HRESULT (STDMETHODCALLTYPE *GetDXGIAdapter)(ID3D12DXVKInteropDevice1 *, REFIID, void **);
    HRESULT (STDMETHODCALLTYPE *GetInstanceExtensions)(ID3D12DXVKInteropDevice1 *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceExtensions)(ID3D12DXVKInteropDevice1 *, UINT *, const char **);
    HRESULT (STDMETHODCALLTYPE *GetDeviceFeatures)(ID3D12DXVKInteropDevice1 *, const VkPhysicalDeviceFeatures2 **);
    HRESULT (STDMETHODCALLTYPE *GetVulkanHandles)(ID3D12DXVKInteropDevice1 *, VkInstance *, VkPhysicalDevice *, VkDevice *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanQueueInfo)(ID3D12DXVKInteropDevice1 *, ID3D12CommandQueue *, VkQueue *, UINT32 *);
    void (STDMETHODCALLTYPE *GetVulkanImageLayout)(ID3D12DXVKInteropDevice1 *, ID3D12Resource *, D3D12_RESOURCE_STATES, VkImageLayout *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo)(ID3D12DXVKInteropDevice1 *, ID3D12Resource *, UINT64 *, UINT64 *);
    HRESULT (STDMETHODCALLTYPE *LockCommandQueue)(ID3D12DXVKInteropDevice1 *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *UnlockCommandQueue)(ID3D12DXVKInteropDevice1 *, ID3D12CommandQueue *);
    HRESULT (STDMETHODCALLTYPE *GetVulkanResourceInfo1)(ID3D12DXVKInteropDevice1 *, ID3D12Resource *, UINT64 *, UINT64 *, VkFormat *);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandQueue)(ID3D12DXVKInteropDevice1 *, const D3D12_COMMAND_QUEUE_DESC *, UINT32, ID3D12CommandQueue **);
    HRESULT (STDMETHODCALLTYPE *CreateInteropCommandAllocator)(ID3D12DXVKInteropDevice1 *, D3D12_COMMAND_LIST_TYPE, UINT32, ID3D12CommandAllocator **);
    HRESULT (STDMETHODCALLTYPE *BeginVkCommandBufferInterop)(ID3D12DXVKInteropDevice1 *, ID3D12CommandList *, VkCommandBuffer *);
    HRESULT (STDMETHODCALLTYPE *EndVkCommandBufferInterop)(ID3D12DXVKInteropDevice1 *, ID3D12CommandList *);
} ID3D12DXVKInteropDevice1Vtbl;
struct ID3D12DXVKInteropDevice1 { const ID3D12DXVKInteropDevice1Vtbl *lpVtbl; };

/* -------------------------------------------------------------- vulkan-1 */
static HMODULE g_vk1;
static PFN_vkGetInstanceProcAddr g_vk1_gipa;
static PFN_vkGetDeviceProcAddr g_vk1_gdpa;

static int vk1_load(void)
{
    if (g_vk1) return g_vk1_gipa && g_vk1_gdpa;
    g_vk1 = LoadLibraryA("vulkan-1.dll");
    if (!g_vk1) return 0;
    g_vk1_gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(g_vk1, "vkGetInstanceProcAddr");
    g_vk1_gdpa = (PFN_vkGetDeviceProcAddr)(void *)GetProcAddress(g_vk1, "vkGetDeviceProcAddr");
    return g_vk1_gipa && g_vk1_gdpa;
}

/* The device-level entry points the channel and the fallback need. */
struct vkprocs {
    PFN_vkCreateCommandPool create_pool;
    PFN_vkDestroyCommandPool destroy_pool;
    PFN_vkAllocateCommandBuffers alloc_cb;
    PFN_vkBeginCommandBuffer begin_cb;
    PFN_vkEndCommandBuffer end_cb;
    PFN_vkCmdUpdateBuffer cmd_update;
    PFN_vkCmdPipelineBarrier cmd_barrier;
    PFN_vkCmdBlitImage cmd_blit;
    PFN_vkCreateBuffer create_buffer;
    PFN_vkDestroyBuffer destroy_buffer;
    PFN_vkGetBufferMemoryRequirements buffer_reqs;
    PFN_vkAllocateMemory alloc_mem;
    PFN_vkFreeMemory free_mem;
    PFN_vkBindBufferMemory bind_buffer;
};

static int vkprocs_get(struct vkprocs *v, PFN_vkGetDeviceProcAddr gdpa, VkDevice dev)
{
#define G(f, n) v->f = (void *)gdpa(dev, n); if (!v->f) return 0
    G(create_pool, "vkCreateCommandPool");
    G(destroy_pool, "vkDestroyCommandPool");
    G(alloc_cb, "vkAllocateCommandBuffers");
    G(begin_cb, "vkBeginCommandBuffer");
    G(end_cb, "vkEndCommandBuffer");
    G(cmd_update, "vkCmdUpdateBuffer");
    G(cmd_barrier, "vkCmdPipelineBarrier");
    G(cmd_blit, "vkCmdBlitImage");
    G(create_buffer, "vkCreateBuffer");
    G(destroy_buffer, "vkDestroyBuffer");
    G(buffer_reqs, "vkGetBufferMemoryRequirements");
    G(alloc_mem, "vkAllocateMemory");
    G(free_mem, "vkFreeMemory");
    G(bind_buffer, "vkBindBufferMemory");
#undef G
    return 1;
}

/* --------------------------------------------------------- the command */
static uint64_t g_context_next = 0x4E47580000000001ull;   /* "NGX" + 1 */

/* The v1 ops go out as v1 (a v1 shim, an older SteamARM, still answers
 * them); SPATIAL is v2's. */
static void cmd_init(struct sa_mfxt_cmd *c, uint32_t op, uint64_t context)
{
    memset(c, 0, sizeof *c);
    c->magic = SA_MFXT_MAGIC;
    c->version = op == SA_OP_SPATIAL ? SA_MFXT_VERSION : SA_MFXT_VERSION_MIN;
    c->size = sizeof *c;
    c->op = op;
    c->context = context;
    c->mv_scale_x = c->mv_scale_y = 1.0f;
    c->pre_exposure = 1.0f;
}

/* A command in a command buffer of a private pool, never submitted: PROBE
 * at init, DESTROY at release (both answered when recorded, not forwarded). */
static uint32_t cmd_private(const struct vkprocs *v, VkDevice dev, uint32_t family, VkBuffer carrier,
                            VkDeviceSize off, struct sa_mfxt_cmd *c)
{
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, family };
    VkCommandPool pool;
    if (v->create_pool(dev, &pci, NULL, &pool) != VK_SUCCESS) return SA_ST_UNSEEN;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                       VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb;
    if (v->alloc_cb(dev, &ai, &cb) != VK_SUCCESS) { v->destroy_pool(dev, pool, NULL); return SA_ST_UNSEEN; }
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    v->begin_cb(cb, &bi);
    v->cmd_update(cb, carrier, off, sizeof *c, c);
    v->end_cb(cb);
    v->destroy_pool(dev, pool, NULL);
    return c->status;
}

/* All commands before, all after: what the FFX path records around the
 * command too (tests/win/d3d12_mfxt_interop.c). */
static void cmd_fence(const struct vkprocs *v, VkCommandBuffer cb)
{
    VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    v->cmd_barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL,
                   0, NULL);
}

/* The fallback: the colour input stretched onto the output, on the same
 * command buffer, between layout transitions from and back to what the
 * game keeps the two images in (GENERAL on KosmicKrisp: VK_KHR_unified_
 * image_layouts; MoltenVK and Metal have no layouts to get wrong). */
static void cmd_blit_fallback(const struct vkprocs *v, VkCommandBuffer cb, const struct sa_mfxt_cmd *c,
                              VkImageLayout color_layout, VkImageLayout output_layout)
{
    VkImage src = (VkImage)(uintptr_t)c->color.image, dst = (VkImage)(uintptr_t)c->output.image;
    VkImageSubresourceRange all = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier in[2] = {
        { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
          color_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, src, all },
        { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
          VK_ACCESS_TRANSFER_WRITE_BIT, output_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED,
          VK_QUEUE_FAMILY_IGNORED, dst, all },
    };
    v->cmd_barrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, in);
    VkImageBlit r;
    memset(&r, 0, sizeof r);
    r.srcSubresource.aspectMask = r.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.srcSubresource.layerCount = r.dstSubresource.layerCount = 1;
    r.srcOffsets[0].x = (int32_t)c->color.x;
    r.srcOffsets[0].y = (int32_t)c->color.y;
    r.srcOffsets[1].x = (int32_t)(c->color.x + c->render_w);
    r.srcOffsets[1].y = (int32_t)(c->color.y + c->render_h);
    r.srcOffsets[1].z = 1;
    r.dstOffsets[0].x = (int32_t)c->output.x;
    r.dstOffsets[0].y = (int32_t)c->output.y;
    r.dstOffsets[1].x = (int32_t)(c->output.x + c->upscale_w);
    r.dstOffsets[1].y = (int32_t)(c->output.y + c->upscale_h);
    r.dstOffsets[1].z = 1;
    v->cmd_blit(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                VK_FILTER_LINEAR);
    VkImageMemoryBarrier out[2] = {
        { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color_layout, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, src, all },
        { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, output_layout,
          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, dst, all },
    };
    v->cmd_barrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 2, out);
}

/* ------------------------------------------------------------ features */
enum { API_D3D12 = 12, API_VK = 1 };
#define FEATURE_MAGIC 0x54414546u
struct feature {
    unsigned Id;                    /* NVSDK_NGX_Handle: the handle points here */
    unsigned magic;
    int api;
    uint64_t context;
    unsigned in_w, in_h, out_w, out_h;
    int flags, out_subrects, quality, spatial;   /* spatial: DLSS 1, op SPATIAL */
    unsigned frames, refused, blitted;
    uint32_t last_st;
    struct feature *next;
};
static struct feature *g_features;
static unsigned g_feature_id = 0x5A000000u;   /* far from the ids a real NGX hands out */

static struct feature *feature_of(const void *handle)
{
    const struct feature *h = handle;
    if (!h) return NULL;
    EnterCriticalSection(&g_lock);
    struct feature *f = g_features;
    while (f && f != h) f = f->next;
    LeaveCriticalSection(&g_lock);
    return f && f->magic == FEATURE_MAGIC ? f : NULL;
}

/* A key, or its enumerated form (the "#\xNN" names older games use). */
static unsigned P_getu2(struct param *p, const char *k, const char *e, unsigned dflt)
{
    return P_has(p, k) ? P_getu(p, k, dflt) : P_getu(p, e, dflt);
}
static int P_geti2(struct param *p, const char *k, const char *e, int dflt)
{
    return P_has(p, k) ? P_geti(p, k, dflt) : P_geti(p, e, dflt);
}
static float P_getf2(struct param *p, const char *k, const char *e, float dflt)
{
    return P_has(p, k) ? P_getf(p, k, dflt) : P_getf(p, e, dflt);
}
static void *P_getp2(struct param *p, const char *k, const char *e)
{
    void *v = P_getp(p, k);
    return v ? v : P_getp(p, e);
}

static struct feature *feature_new(int api, struct param *p, int spatial)
{
    struct feature *f = xalloc(sizeof *f);
    if (!f) return NULL;
    f->magic = FEATURE_MAGIC;
    f->api = api;
    f->spatial = spatial;
    f->in_w = P_getu2(p, K_Width, E_Width, 0);
    f->in_h = P_getu2(p, K_Height, E_Height, 0);
    f->out_w = P_getu2(p, K_OutWidth, E_OutWidth, 0);
    f->out_h = P_getu2(p, K_OutHeight, E_OutHeight, 0);
    f->flags = P_geti(p, K_Create_Flags, 0);
    f->out_subrects = P_geti(p, K_Enable_Output_Subrects, 0);
    f->quality = P_geti(p, K_PerfQualityValue, -1);
    EnterCriticalSection(&g_lock);
    f->Id = ++g_feature_id;
    f->context = g_context_next++;
    f->next = g_features;
    g_features = f;
    LeaveCriticalSection(&g_lock);
    return f;
}

static void feature_unlink(struct feature *f)
{
    EnterCriticalSection(&g_lock);
    struct feature **pp = &g_features;
    while (*pp && *pp != f) pp = &(*pp)->next;
    if (*pp) *pp = f->next;
    LeaveCriticalSection(&g_lock);
    f->magic = 0;
    xfree(f);
}

static void log_feature(const char *what, const struct feature *f, unsigned rc)
{
    struct line l;
    L_begin(&l);
    L_s(&l, what);
    L_s(&l, f->api == API_D3D12 ? " D3D12 feature " : " Vulkan feature ");
    L_x(&l, f->Id);
    L_s(&l, " ctx ");
    L_x(&l, f->context);
    L_s(&l, ": ");
    L_u(&l, f->in_w); L_c(&l, 'x'); L_u(&l, f->in_h); L_s(&l, " -> "); L_u(&l, f->out_w); L_c(&l, 'x'); L_u(&l, f->out_h);
    L_s(&l, " flags "); L_x(&l, (unsigned)f->flags);
    L_s(&l, " quality "); L_i(&l, f->quality);
    L_s(&l, f->spatial ? " path spatial (DLSS 1)" : " path temporal");
    L_s(&l, " -> "); L_x(&l, rc);
    L_end(&l);
}

/* What the game's DLSS flags say, as the command's flags. */
static uint32_t flags_of(const struct feature *f, int reset)
{
    uint32_t fl = reset ? SA_MFXT_RESET : 0;
    if (f->flags & NGX_DLSS_DepthInverted) fl |= SA_MFXT_DEPTH_INV;
    if (f->flags & NGX_DLSS_IsHDR) fl |= SA_MFXT_HDR;
    if (f->flags & NGX_DLSS_AutoExposure) fl |= SA_MFXT_AUTO_EXP;
    if (f->flags & NGX_DLSS_MVJittered) fl |= SA_MFXT_MV_JITTERED;
    if (!(f->flags & NGX_DLSS_MVLowRes)) fl |= SA_MFXT_MV_DISPLAY;
    if (f->flags & NGX_DLSS_DoSharpening) fl |= SA_MFXT_SHARPEN;
    return fl;
}

/* The per-frame inputs from the parameters into the command (sizes,
 * jitter, motion-vector scale, exposure, subrect origins); images apart. */
static void cmd_from_params(struct sa_mfxt_cmd *c, const struct feature *f, struct param *p)
{
    unsigned rw = P_getu(p, K_Render_Subrect_W, 0), rh = P_getu(p, K_Render_Subrect_H, 0);
    c->render_w = rw ? rw : f->in_w;
    c->render_h = rh ? rh : f->in_h;
    c->upscale_w = f->out_w;
    c->upscale_h = f->out_h;
    c->jitter_x = P_getf(p, K_Jitter_X, 0.0f);
    c->jitter_y = P_getf(p, K_Jitter_Y, 0.0f);
    float sx = P_getf2(p, K_MV_Scale_X, E_MV_Scale_X, 1.0f), sy = P_getf2(p, K_MV_Scale_Y, E_MV_Scale_Y, 1.0f);
    c->mv_scale_x = sx == 0.0f ? 1.0f : sx;
    c->mv_scale_y = sy == 0.0f ? 1.0f : sy;
    float pe = P_getf(p, K_Pre_Exposure, 1.0f);
    c->pre_exposure = pe == 0.0f ? 1.0f : pe;
    c->sharpness = P_getf2(p, K_Sharpness, E_Sharpness, 0.0f);
    c->flags = flags_of(f, P_geti2(p, K_Reset, E_Reset, 0) != 0);
    c->color.x = P_getu(p, K_Color_Subrect_X, 0);
    c->color.y = P_getu(p, K_Color_Subrect_Y, 0);
    c->depth.x = P_getu(p, K_Depth_Subrect_X, 0);
    c->depth.y = P_getu(p, K_Depth_Subrect_Y, 0);
    c->motion.x = P_getu(p, K_MV_Subrect_X, 0);
    c->motion.y = P_getu(p, K_MV_Subrect_Y, 0);
    c->reactive.x = P_getu(p, K_Bias_Subrect_X, 0);
    c->reactive.y = P_getu(p, K_Bias_Subrect_Y, 0);
    if (f->out_subrects) {
        c->output.x = P_getu(p, K_Output_Subrect_X, 0);
        c->output.y = P_getu(p, K_Output_Subrect_Y, 0);
    }
    /* The sizes as the sender knows them: the content, not the textures. */
    c->color.w = c->depth.w = c->reactive.w = c->render_w;
    c->color.h = c->depth.h = c->reactive.h = c->render_h;
    if (c->flags & SA_MFXT_MV_DISPLAY) { c->motion.w = c->upscale_w; c->motion.h = c->upscale_h; }
    else { c->motion.w = c->render_w; c->motion.h = c->render_h; }
    c->output.w = c->upscale_w;
    c->output.h = c->upscale_h;
    c->exposure.w = c->exposure.h = 1;
}

static void log_eval(const struct feature *f, const struct sa_mfxt_cmd *c, uint32_t st, int blitted)
{
    struct line l;
    L_begin(&l);
    L_s(&l, "evaluate feature "); L_x(&l, f->Id);
    L_s(&l, " frame "); L_u(&l, f->frames);
    L_s(&l, c->op == SA_OP_SPATIAL ? " SPATIAL (DLSS 1, the shim's FSR 1 pass): " : " ENCODE (MetalFX temporal): ");
    L_u(&l, c->render_w); L_c(&l, 'x'); L_u(&l, c->render_h);
    L_s(&l, " -> "); L_u(&l, c->upscale_w); L_c(&l, 'x'); L_u(&l, c->upscale_h);
    L_s(&l, " colour fmt "); L_u(&l, c->color.vk_format);
    L_s(&l, " depth "); L_s(&l, c->depth.image ? "yes" : "NO");
    L_s(&l, " mv "); L_s(&l, c->motion.image ? "yes" : "NO");
    L_s(&l, " exposure "); L_s(&l, c->exposure.image ? "yes" : "no");
    L_s(&l, " reactive "); L_s(&l, c->reactive.image ? "yes" : "no");
    L_s(&l, " jitter "); L_f(&l, c->jitter_x, 3); L_c(&l, ','); L_f(&l, c->jitter_y, 3);
    L_s(&l, " flags "); L_x(&l, c->flags);
    L_s(&l, " status "); L_s(&l, status_name(st));
    if (blitted) L_s(&l, " (blit fallback)");
    L_end(&l);
}

/* --------------------------------------------------------------- D3D12 */
struct dev12 {
    int inited, available;
    ID3D12Device *dev;
    ID3D12DXVKInteropDevice1 *ix;
    VkInstance vinst;
    VkPhysicalDevice vpd;
    VkDevice vdev;
    UINT32 family;
    struct vkprocs v;
    ID3D12Resource *carrier;
    UINT64 carrier_buf, carrier_off;
    uint32_t probe_st, caps, driver;
    float max_scale;
    int spatial;           /* DLSS 1: features send SPATIAL */
    const char *why;
};
static struct dev12 g12;

static void dev12_release(void)
{
    if (g12.carrier) ID3D12Resource_Release(g12.carrier);
    if (g12.ix) g12.ix->lpVtbl->Release(g12.ix);
    if (g12.dev) ID3D12Device_Release(g12.dev);
    memset(&g12, 0, sizeof g12);
}

static void log_init(const char *api, int available, int spatial, uint32_t st, uint32_t caps, uint32_t driver,
                     float max_scale, const char *why)
{
    struct line l;
    L_begin(&l);
    L_s(&l, api);
    L_s(&l, " init: ");
    if (why) { L_s(&l, why); L_s(&l, "; "); }
    L_s(&l, "probe "); L_s(&l, status_name(st));
    L_s(&l, " caps "); L_x(&l, caps);
    L_s(&l, " driver "); L_u(&l, driver);
    L_s(&l, " max scale "); L_f(&l, max_scale, 2);
    L_s(&l, " -> DLSS (SuperSampling) ");
    L_s(&l, !available ? "unavailable" : spatial ? "available: DLSS 1 as MetalFX spatial (the shim's FSR 1 pass)"
                                                 : "available: MetalFX temporal");
    L_end(&l);
}

/* ------------------------------------------------------ DLSS 1 or not */
/* The game's nvngx_dlss.dll file version (major; 0: not found). version.dll
 * is loaded when asked: KERNEL32 stays the only import. */
typedef DWORD (WINAPI *pfn_gfvis)(LPCWSTR, LPDWORD);
typedef BOOL (WINAPI *pfn_gfvi)(LPCWSTR, DWORD, DWORD, LPVOID);
typedef BOOL (WINAPI *pfn_vqv)(LPCVOID, LPCWSTR, LPVOID *, PUINT);

static unsigned file_version_major(const wchar_t *path, unsigned *rest)
{
    HMODULE v = LoadLibraryA("version.dll");
    if (!v) return 0;
    pfn_gfvis gfvis = (pfn_gfvis)(void *)GetProcAddress(v, "GetFileVersionInfoSizeW");
    pfn_gfvi gfvi = (pfn_gfvi)(void *)GetProcAddress(v, "GetFileVersionInfoW");
    pfn_vqv vqv = (pfn_vqv)(void *)GetProcAddress(v, "VerQueryValueW");
    unsigned major = 0;
    DWORD handle = 0, size = gfvis && gfvi && vqv ? gfvis(path, &handle) : 0;
    void *buf = size ? xalloc(size) : NULL;
    if (buf && gfvi(path, 0, size, buf)) {
        VS_FIXEDFILEINFO *ffi = NULL;
        UINT n = 0;
        if (vqv(buf, L"\\", (void **)&ffi, &n) && ffi && n >= sizeof *ffi) {
            major = ffi->dwFileVersionMS >> 16;
            if (rest) *rest = ((ffi->dwFileVersionMS & 0xffff) << 16) | (ffi->dwFileVersionLS >> 16);
        }
    }
    xfree(buf);
    FreeLibrary(v);
    return major;
}

/* Where the model DLL is: STEAMARM_NGX_MODEL (a Windows path, or a Unix
 * path of the guest, which Wine shows as Z:), else nvngx_dlss.dll next to
 * the game's program, in the current directory, next to this DLL. */
static int model_path(wchar_t *out, unsigned n)
{
    wchar_t env[1024];
    DWORD k = GetEnvironmentVariableW(L"STEAMARM_NGX_MODEL", env, 1024);
    if (k && k < 1024) {
        unsigned o = 0;
        if (env[0] == L'/') { out[o++] = L'Z'; out[o++] = L':'; }
        for (unsigned i = 0; env[i] && o + 1 < n; i++) out[o++] = env[i] == L'/' ? L'\\' : env[i];
        out[o] = 0;
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
        /* Not where the tool said (a path the guest does not see so): the search. */
    }
    static const wchar_t name[] = L"nvngx_dlss.dll";
    for (int where = 0; where < 3; where++) {
        wchar_t dir[1024];
        DWORD m = where == 0 ? GetModuleFileNameW(NULL, dir, 1024) :
                  where == 1 ? GetCurrentDirectoryW(1024, dir) : GetModuleFileNameW(g_self, dir, 1024);
        if (!m || m >= 1024) continue;
        if (where != 1) {   /* the file's directory */
            while (m && dir[m - 1] != L'\\' && dir[m - 1] != L'/') m--;
            dir[m] = 0;
        } else if (m && dir[m - 1] != L'\\') { dir[m++] = L'\\'; dir[m] = 0; }
        unsigned o = 0;
        for (unsigned i = 0; dir[i] && o + 1 < n; i++) out[o++] = dir[i];
        for (unsigned i = 0; name[i] && o + 1 < n; i++) out[o++] = name[i];
        out[o] = 0;
        if (GetFileAttributesW(out) != INVALID_FILE_ATTRIBUTES) return 1;
    }
    out[0] = 0;
    return 0;
}

/* Whether this game's DLSS is DLSS 1: decided at every init (a game inits
 * once; the test inits twice, with two SDK versions), logged when the
 * verdict or the SDK version changes. */
static int g_dlss1 = -1, g_dlss1_sdk = -1;

static int dlss1_mode(int sdk, const char *who)
{
    char v[8];
    const char *why = NULL;
    int mode = 0;
    unsigned major = 0, rest = 0;
    wchar_t model[1024];
    int have_model = model_path(model, 1024);
    if (have_model) major = file_version_major(model, &rest);
    if (GetEnvironmentVariableA("STEAMARM_NGX_SPATIAL", v, sizeof v) && (v[0] == '0' || v[0] == '1')) {
        mode = v[0] == '1';
        why = mode ? "STEAMARM_NGX_SPATIAL=1" : "STEAMARM_NGX_SPATIAL=0";
    } else if (sdk && sdk < 0x13) {
        mode = 1;
        why = "the NGX SDK version given to Init is below 0x13 (DLSS 1's)";
    } else if (major == 1) {
        mode = 1;
        why = "the game's nvngx_dlss.dll is a 1.x";
    } else {
        why = major ? "the game's nvngx_dlss.dll is a 2.x or later" : "no nvngx_dlss.dll found: DLSS 2 or later assumed";
    }
    if (g_dlss1 == mode && g_dlss1_sdk == sdk) return mode;
    g_dlss1 = mode;
    g_dlss1_sdk = sdk;
    struct line l;
    L_begin(&l);
    L_s(&l, who); L_s(&l, ": NGX SDK version "); L_x(&l, (unsigned)sdk);
    L_s(&l, ", model DLL ");
    if (have_model) {
        for (unsigned i = 0; model[i]; i++) L_c(&l, model[i] < 128 ? (char)model[i] : '?');
        L_s(&l, " version "); L_u(&l, major); L_c(&l, '.'); L_u(&l, rest >> 16); L_c(&l, '.'); L_u(&l, rest & 0xffff);
    } else L_s(&l, "not found");
    L_s(&l, " -> "); L_s(&l, mode ? "DLSS 1: MetalFX spatial" : "DLSS 2+: MetalFX temporal");
    L_s(&l, " ("); L_s(&l, why); L_c(&l, ')');
    L_end(&l);
    return mode;
}


/* Init on a device: the interop interface, vkd3d's VkDevice, a carrier
 * buffer of our own, a PROBE in a private command buffer. Caller holds
 * g_lock. The interop interface is the proof that this is VKD3D-Proton. */
static unsigned dev12_init(ID3D12Device *dev, int sdk, const char *who)
{
    if (!dev) return NGX_FAIL_InvalidParameter;
    if (g12.inited && g12.dev == dev) return NGX_OK;
    dev12_release();
    g12.inited = 1;
    g12.dev = dev;
    g12.spatial = dlss1_mode(sdk, who);
    ID3D12Device_AddRef(dev);
    if (FAILED(ID3D12Device_QueryInterface(dev, &IID_ID3D12DXVKInteropDevice1, (void **)&g12.ix)) || !g12.ix) {
        g12.ix = NULL;
        g12.why = "no VKD3D-Proton interop (ID3D12DXVKInteropDevice1) on this device";
        goto done;
    }
    if (FAILED(g12.ix->lpVtbl->GetVulkanHandles(g12.ix, &g12.vinst, &g12.vpd, &g12.vdev)) || !g12.vdev) {
        g12.why = "GetVulkanHandles failed";
        goto done;
    }
    if (!vk1_load()) { g12.why = "vulkan-1.dll not loadable"; goto done; }
    if (!vkprocs_get(&g12.v, g_vk1_gdpa, g12.vdev)) { g12.why = "vkGetDeviceProcAddr on vkd3d's device failed"; goto done; }
    {
        /* The queue family of a direct queue, for the private pool. */
        D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
        ID3D12CommandQueue *q = NULL;
        VkQueue vq = VK_NULL_HANDLE;
        if (FAILED(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q)) || !q) {
            g12.why = "CreateCommandQueue failed";
            goto done;
        }
        HRESULT hr = g12.ix->lpVtbl->GetVulkanQueueInfo(g12.ix, q, &vq, &g12.family);
        ID3D12CommandQueue_Release(q);
        if (FAILED(hr)) { g12.why = "GetVulkanQueueInfo failed"; goto done; }
    }
    {
        D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
        D3D12_RESOURCE_DESC d = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 4096, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                                  D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
        if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON,
                                                        NULL, &IID_ID3D12Resource, (void **)&g12.carrier)) || !g12.carrier) {
            g12.carrier = NULL;
            g12.why = "the carrier buffer could not be created";
            goto done;
        }
        VkFormat f;
        if (FAILED(g12.ix->lpVtbl->GetVulkanResourceInfo1(g12.ix, g12.carrier, &g12.carrier_buf, &g12.carrier_off, &f)) ||
            !g12.carrier_buf) {
            g12.why = "GetVulkanResourceInfo1(carrier) failed";
            goto done;
        }
    }
    {
        struct sa_mfxt_cmd c;
        cmd_init(&c, SA_OP_PROBE, 0);
        g12.probe_st = cmd_private(&g12.v, g12.vdev, g12.family, (VkBuffer)(uintptr_t)g12.carrier_buf, g12.carrier_off, &c);
        g12.caps = c.caps_out;
        g12.driver = c.driver_out;
        g12.max_scale = c.max_scale_out;
        if (g12.spatial) {
            g12.available = (g12.caps & SA_MFXT_CAP_SPATIAL) != 0;
            if (!g12.available) g12.why = "the channel's PROBE did not report the shim's spatial pass (an older shim, or none)";
        } else {
            g12.available = g12.probe_st == SA_ST_OK && (g12.caps & SA_MFXT_CAP_TEMPORAL);
            if (!g12.available) g12.why = "the channel's PROBE did not answer OK (no KosmicKrisp with MetalFX temporal, or KK_MFXT=0)";
        }
    }
done:
    log_init("D3D12", g12.available, g12.spatial, g12.probe_st, g12.caps, g12.driver, g12.max_scale, g12.why);
    return NGX_OK;
}

/* The VkImage and format behind a D3D12 resource (0: none); tw/th: the
 * texture's own size, when asked. */
static void tex12(struct sa_mfxt_tex *t, ID3D12Resource *r, uint32_t *tw, uint32_t *th)
{
    UINT64 img = 0, off = 0;
    VkFormat f = VK_FORMAT_UNDEFINED;
    if (!r || !g12.ix) return;
    if (FAILED(g12.ix->lpVtbl->GetVulkanResourceInfo1(g12.ix, r, &img, &off, &f))) return;
    t->image = img;
    t->vk_format = (uint32_t)f;
    if (tw && th) {
        D3D12_RESOURCE_DESC d = ID3D12Resource_GetDesc(r);
        *tw = (uint32_t)d.Width;
        *th = d.Height;
    }
}

static VkImageLayout layout12(ID3D12Resource *r, D3D12_RESOURCE_STATES st)
{
    VkImageLayout l = VK_IMAGE_LAYOUT_GENERAL;
    if (r && g12.ix) g12.ix->lpVtbl->GetVulkanImageLayout(g12.ix, r, st, &l);
    return l;
}

static unsigned eval12(ID3D12GraphicsCommandList *cl, struct feature *f, struct param *p)
{
    if (!cl || !p || p->magic != PARAM_MAGIC) return NGX_FAIL_InvalidParameter;
    if (!g12.inited || !g12.ix) return NGX_FAIL_NotInitialized;
    ID3D12Resource *color = P_getp2(p, K_Color, E_Color), *output = P_getp2(p, K_Output, E_Output),
                   *depth = P_getp2(p, K_Depth, E_Depth), *motion = P_getp2(p, K_MotionVectors, E_MotionVectors),
                   *exposure = P_getp(p, K_ExposureTexture), *bias = P_getp(p, K_BiasCurrentColorMask);
    if (!color || !output) return NGX_FAIL_MissingInput;
    struct sa_mfxt_cmd c;
    cmd_init(&c, f->spatial ? SA_OP_SPATIAL : SA_OP_ENCODE, f->context);
    cmd_from_params(&c, f, p);
    tex12(&c.color, color, &c.color_tex_w, &c.color_tex_h);
    tex12(&c.output, output, &c.output_tex_w, &c.output_tex_h);
    tex12(&c.depth, depth, NULL, NULL);
    tex12(&c.motion, motion, NULL, NULL);
    tex12(&c.exposure, exposure, NULL, NULL);
    tex12(&c.reactive, bias, NULL, NULL);
    if (!c.color.image || !c.output.image) return NGX_FAIL_InvalidParameter;

    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (FAILED(g12.ix->lpVtbl->BeginVkCommandBufferInterop(g12.ix, (ID3D12CommandList *)cl, &cb)) || !cb)
        return NGX_FAIL_PlatformError;
    cmd_fence(&g12.v, cb);
    g12.v.cmd_update(cb, (VkBuffer)(uintptr_t)g12.carrier_buf, g12.carrier_off, sizeof c, &c);
    uint32_t st = c.status;
    int blit = st != SA_ST_OK;
    if (blit) {
        VkImageLayout lc = layout12(color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        VkImageLayout lo = layout12(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        cmd_blit_fallback(&g12.v, cb, &c, lc, lo);
    }
    cmd_fence(&g12.v, cb);
    g12.ix->lpVtbl->EndVkCommandBufferInterop(g12.ix, (ID3D12CommandList *)cl);

    f->frames++;
    f->last_st = st;
    if (blit) { f->refused++; f->blitted++; }
    if (debug_on() ? (f->frames <= 4 || (blit && f->refused <= 4)) : (f->frames == 1 || (blit && f->refused == 1)))
        log_eval(f, &c, st, blit);
    return NGX_OK;
}

/* Release: DESTROY in a private command buffer (answered when recorded). */
static void destroy_context12(struct feature *f)
{
    if (!g12.inited || !g12.ix || !g12.carrier_buf) return;
    struct sa_mfxt_cmd c;
    cmd_init(&c, SA_OP_DESTROY, f->context);
    cmd_private(&g12.v, g12.vdev, g12.family, (VkBuffer)(uintptr_t)g12.carrier_buf, g12.carrier_off, &c);
}

/* -------------------------------------------------------------- Vulkan */
struct devvk {
    int inited, available;
    VkInstance inst;
    VkPhysicalDevice pd;
    VkDevice dev;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    struct vkprocs v;
    VkBuffer carrier;
    VkDeviceMemory mem;
    uint32_t family;
    uint32_t probe_st, caps, driver;
    float max_scale;
    int spatial;
    const char *why;
};
static struct devvk gvk;

static void devvk_release(void)
{
    if (gvk.dev && gvk.carrier && gvk.v.destroy_buffer) gvk.v.destroy_buffer(gvk.dev, gvk.carrier, NULL);
    if (gvk.dev && gvk.mem && gvk.v.free_mem) gvk.v.free_mem(gvk.dev, gvk.mem, NULL);
    memset(&gvk, 0, sizeof gvk);
}

/* Caller holds g_lock. */
static unsigned devvk_init(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa,
                           PFN_vkGetDeviceProcAddr gdpa, int sdk, const char *who)
{
    if (!dev || !pd) return NGX_FAIL_InvalidParameter;
    if (gvk.inited && gvk.dev == dev) return NGX_OK;
    devvk_release();
    gvk.inited = 1;
    gvk.spatial = dlss1_mode(sdk, who);
    gvk.inst = inst;
    gvk.pd = pd;
    gvk.dev = dev;
    if (!gipa || !gdpa) {
        if (!vk1_load()) { gvk.why = "no vkGetInstanceProcAddr/vkGetDeviceProcAddr given and vulkan-1.dll not loadable"; goto done; }
        gipa = g_vk1_gipa;
        gdpa = g_vk1_gdpa;
    }
    gvk.gipa = gipa;
    gvk.gdpa = gdpa;
    if (!vkprocs_get(&gvk.v, gdpa, dev)) { gvk.why = "vkGetDeviceProcAddr failed"; goto done; }
    PFN_vkGetPhysicalDeviceMemoryProperties mem_props = (void *)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkGetPhysicalDeviceQueueFamilyProperties qf_props = (void *)gipa(inst, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (!mem_props || !qf_props) { gvk.why = "vkGetInstanceProcAddr failed"; goto done; }
    {
        VkQueueFamilyProperties qf[16];
        uint32_t n = 16;
        qf_props(pd, &n, qf);
        gvk.family = 0;
        for (uint32_t i = 0; i < n; i++)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gvk.family = i; break; }
    }
    {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096,
                                   VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        if (gvk.v.create_buffer(dev, &bci, NULL, &gvk.carrier) != VK_SUCCESS) { gvk.why = "the carrier buffer could not be created"; goto done; }
        VkMemoryRequirements mr;
        gvk.v.buffer_reqs(dev, gvk.carrier, &mr);
        VkPhysicalDeviceMemoryProperties mp;
        mem_props(pd, &mp);
        uint32_t mt = UINT32_MAX;
        for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
            if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) mt = i;
        for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
            if (mr.memoryTypeBits & (1u << i)) mt = i;
        if (mt == UINT32_MAX) { gvk.why = "no memory type for the carrier buffer"; goto done; }
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt };
        if (gvk.v.alloc_mem(dev, &mai, NULL, &gvk.mem) != VK_SUCCESS ||
            gvk.v.bind_buffer(dev, gvk.carrier, gvk.mem, 0) != VK_SUCCESS) {
            gvk.why = "the carrier buffer's memory could not be bound";
            goto done;
        }
    }
    {
        struct sa_mfxt_cmd c;
        cmd_init(&c, SA_OP_PROBE, 0);
        gvk.probe_st = cmd_private(&gvk.v, dev, gvk.family, gvk.carrier, 0, &c);
        gvk.caps = c.caps_out;
        gvk.driver = c.driver_out;
        gvk.max_scale = c.max_scale_out;
        if (gvk.spatial) {
            gvk.available = (gvk.caps & SA_MFXT_CAP_SPATIAL) != 0;
            if (!gvk.available) gvk.why = "the channel's PROBE did not report the shim's spatial pass (an older shim, or none)";
        } else {
            gvk.available = gvk.probe_st == SA_ST_OK && (gvk.caps & SA_MFXT_CAP_TEMPORAL);
            if (!gvk.available) gvk.why = "the channel's PROBE did not answer OK (no KosmicKrisp with MetalFX temporal, or KK_MFXT=0)";
        }
    }
done:
    log_init("Vulkan", gvk.available, gvk.spatial, gvk.probe_st, gvk.caps, gvk.driver, gvk.max_scale, gvk.why);
    return NGX_OK;
}

/* The VkImage and format of an NVSDK_NGX_Resource_VK (0: none, or a buffer);
 * tw/th: the image's size as the description gives it, when asked. */
static void texvk(struct sa_mfxt_tex *t, const struct ngx_resource_vk *r, uint32_t *tw, uint32_t *th)
{
    if (!r || r->Type != NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW || !r->Resource.ImageViewInfo.Image) return;
    t->image = r->Resource.ImageViewInfo.Image;
    t->vk_format = r->Resource.ImageViewInfo.Format;
    if (tw && th) {
        *tw = r->Resource.ImageViewInfo.Width;
        *th = r->Resource.ImageViewInfo.Height;
    }
}

static unsigned evalvk(VkCommandBuffer cb, struct feature *f, struct param *p)
{
    if (!cb || !p || p->magic != PARAM_MAGIC) return NGX_FAIL_InvalidParameter;
    if (!gvk.inited || !gvk.carrier) return NGX_FAIL_NotInitialized;
    const struct ngx_resource_vk *color = P_getp2(p, K_Color, E_Color), *output = P_getp2(p, K_Output, E_Output),
                                 *depth = P_getp2(p, K_Depth, E_Depth), *motion = P_getp2(p, K_MotionVectors, E_MotionVectors),
                                 *exposure = P_getp(p, K_ExposureTexture), *bias = P_getp(p, K_BiasCurrentColorMask);
    if (!color || !output) return NGX_FAIL_MissingInput;
    struct sa_mfxt_cmd c;
    cmd_init(&c, f->spatial ? SA_OP_SPATIAL : SA_OP_ENCODE, f->context);
    cmd_from_params(&c, f, p);
    texvk(&c.color, color, &c.color_tex_w, &c.color_tex_h);
    texvk(&c.output, output, &c.output_tex_w, &c.output_tex_h);
    texvk(&c.depth, depth, NULL, NULL);
    texvk(&c.motion, motion, NULL, NULL);
    texvk(&c.exposure, exposure, NULL, NULL);
    texvk(&c.reactive, bias, NULL, NULL);
    if (!c.color.image || !c.output.image) return NGX_FAIL_InvalidParameter;

    cmd_fence(&gvk.v, cb);
    gvk.v.cmd_update(cb, gvk.carrier, 0, sizeof c, &c);
    uint32_t st = c.status;
    int blit = st != SA_ST_OK;
    /* DLSS's Vulkan inputs are in GENERAL or SHADER_READ_ONLY_OPTIMAL and
     * its output in GENERAL; on Metal a layout is not a thing, and the
     * blit is only ever reached on this Mac. */
    if (blit) cmd_blit_fallback(&gvk.v, cb, &c, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
    cmd_fence(&gvk.v, cb);

    f->frames++;
    f->last_st = st;
    if (blit) { f->refused++; f->blitted++; }
    if (debug_on() ? (f->frames <= 4 || (blit && f->refused <= 4)) : (f->frames == 1 || (blit && f->refused == 1)))
        log_eval(f, &c, st, blit);
    return NGX_OK;
}

static void destroy_contextvk(struct feature *f)
{
    if (!gvk.inited || !gvk.carrier) return;
    struct sa_mfxt_cmd c;
    cmd_init(&c, SA_OP_DESTROY, f->context);
    cmd_private(&gvk.v, gvk.dev, gvk.family, gvk.carrier, 0, &c);
}

/* -------------------------------------------- the capability parameters */
static unsigned optimal_settings_cb(struct param *p);
static unsigned get_stats_cb(struct param *p);

/* DLSS's render size per quality mode, as its programming guide lists them. */
static float quality_ratio(int q)
{
    switch (q) {
    case NGX_PerfQuality_MaxPerf: return 0.5f;
    case NGX_PerfQuality_Balanced: return 0.58f;
    case NGX_PerfQuality_MaxQuality: return 2.0f / 3.0f;
    case NGX_PerfQuality_UltraPerformance: return 1.0f / 3.0f;
    case NGX_PerfQuality_UltraQuality: return 0.77f;
    case NGX_PerfQuality_DLAA: return 1.0f;
    default: return 2.0f / 3.0f;
    }
}

static unsigned optimal_settings_cb(struct param *p)
{
    if (!p || p->magic != PARAM_MAGIC) return NGX_FAIL_InvalidParameter;
    unsigned w = P_getu(p, K_Width, 0), h = P_getu(p, K_Height, 0);
    if (!w) w = P_getu(p, E_Width, 0);
    if (!h) h = P_getu(p, E_Height, 0);
    int q = P_has(p, K_PerfQualityValue) ? P_geti(p, K_PerfQualityValue, 0) : P_geti(p, E_PerfQualityValue, 0);
    if (!w || !h) return NGX_FAIL_InvalidParameter;
    float r = quality_ratio(q);
    unsigned rw = (unsigned)((float)w * r + 0.5f), rh = (unsigned)((float)h * r + 0.5f);
    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    if (rw > w) rw = w;
    if (rh > h) rh = h;
    float scale = (float)rw / (float)w;
    /* Dynamic resolution: MetalFX takes a smaller content each frame, down
     * to the largest scale the probe reported (3x), and up to the output. */
    float max_scale = g12.max_scale > 1.0f ? g12.max_scale : gvk.max_scale > 1.0f ? gvk.max_scale : 3.0f;
    unsigned min_w = (unsigned)((float)w / max_scale + 0.999f), min_h = (unsigned)((float)h / max_scale + 0.999f);
    if (q == NGX_PerfQuality_DLAA) { min_w = rw; min_h = rh; }
    if (min_w > rw) min_w = rw;
    if (min_h > rh) min_h = rh;
    P_set_ui(p, K_OutWidth, rw);
    P_set_ui(p, K_OutHeight, rh);
    P_set_f(p, K_Scale, scale);
    P_set_f(p, K_SuperSampling_ScaleFactor, scale);
    P_set_ui(p, K_Dyn_Max_W, w);
    P_set_ui(p, K_Dyn_Max_H, h);
    P_set_ui(p, K_Dyn_Min_W, min_w);
    P_set_ui(p, K_Dyn_Min_H, min_h);
    P_set_f(p, K_Sharpness, 0.0f);
    P_set_ull(p, K_SizeInBytes, (unsigned long long)w * h * 31);
    P_set_i(p, K_DLSSMode, 1);
    P_set_ui(p, E_OutWidth, rw);
    P_set_ui(p, E_OutHeight, rh);
    P_set_f(p, E_Scale, scale);
    P_set_f(p, E_Sharpness, 0.0f);
    P_set_ull(p, E_SizeInBytes, (unsigned long long)w * h * 31);
    P_set_i(p, E_DLSSMode, 1);
    if (debug_on()) {
        struct line l;
        L_begin(&l);
        L_s(&l, "optimal settings: "); L_u(&l, w); L_c(&l, 'x'); L_u(&l, h);
        L_s(&l, " quality "); L_i(&l, q);
        L_s(&l, " -> render "); L_u(&l, rw); L_c(&l, 'x'); L_u(&l, rh);
        L_s(&l, " (dynamic "); L_u(&l, min_w); L_c(&l, 'x'); L_u(&l, min_h); L_s(&l, " .. "); L_u(&l, w); L_c(&l, 'x'); L_u(&l, h); L_c(&l, ')');
        L_end(&l);
    }
    return NGX_OK;
}

static unsigned get_stats_cb(struct param *p)
{
    if (!p || p->magic != PARAM_MAGIC) return NGX_FAIL_InvalidParameter;
    unsigned w = P_getu(p, K_Width, 1920), h = P_getu(p, K_Height, 1080);
    P_set_ull(p, K_SizeInBytes, (unsigned long long)w * h * 31);
    P_set_ui(p, K_OptLevel, 0);
    P_set_ui(p, K_IsDevSnippetBranch, 0);
    return NGX_OK;
}

/* What a game reads before it offers DLSS. available: the device's PROBE
 * (-1: no device yet, assumed available until Init says). */
static void fill_caps(struct param *p, int available)
{
    int avail = available != 0;
    P_set_i(p, K_SuperSampling_Available, avail);
    P_set_i(p, E_SuperSampling_Available, avail);
    P_set_i(p, K_SuperSampling_NeedsUpdatedDriver, 0);
    P_set_ui(p, K_SuperSampling_MinDriverMajor, 0);
    P_set_ui(p, K_SuperSampling_MinDriverMinor, 0);
    P_set_i(p, K_SuperSampling_FeatureInitResult, avail ? (int)NGX_OK : (int)NGX_FAIL_FeatureNotSupported);
    P_set_i(p, K_SSD_Available, 0);
    P_set_i(p, K_SSD_NeedsUpdatedDriver, 0);
    P_set_i(p, K_SSD_FeatureInitResult, (int)NGX_FAIL_FeatureNotSupported);
    P_set_i(p, K_FG_Available, 0);
    P_set_i(p, K_FG_NeedsUpdatedDriver, 0);
    P_set_i(p, K_FG_FeatureInitResult, (int)NGX_FAIL_FeatureNotSupported);
    P_set_i(p, K_FI_Available, 0);
    P_set_i(p, K_FI_NeedsUpdatedDriver, 0);
    P_set_i(p, K_FI_FeatureInitResult, (int)NGX_FAIL_FeatureNotSupported);
    P_set_ui(p, K_OptLevel, 0);
    P_set_ui(p, K_IsDevSnippetBranch, 0);
    P_set_ui(p, E_OptLevel, 0);
    P_set_ui(p, E_IsDevSnippetBranch, 0);
    P_set_vp(p, K_OptimalSettingsCallback, (void *)optimal_settings_cb);
    P_set_vp(p, E_OptimalSettingsCallback, (void *)optimal_settings_cb);
    P_set_vp(p, K_GetStatsCallback, (void *)get_stats_cb);
    P_set_f(p, K_Sharpness, 0.0f);
    P_set_f(p, E_Sharpness, 0.0f);
    P_set_f(p, K_MV_Scale_X, 1.0f);
    P_set_f(p, K_MV_Scale_Y, 1.0f);
    P_set_f(p, E_MV_Scale_X, 1.0f);
    P_set_f(p, E_MV_Scale_Y, 1.0f);
    P_set_f(p, K_MV_Offset_X, 0.0f);
    P_set_f(p, K_MV_Offset_Y, 0.0f);
    P_set_f(p, E_MV_Offset_X, 0.0f);
    P_set_f(p, E_MV_Offset_Y, 0.0f);
    P_set_f(p, K_Exposure_Scale, 1.0f);
    P_set_i(p, K_PerfQualityValue, 0);
    P_set_ull(p, K_SizeInBytes, 1920ull * 1080 * 31);
    P_set_i(p, K_FreeMemOnReleaseFeature, 0);
    P_set_ui(p, K_CreationNodeMask, 1);
    P_set_ui(p, K_VisibilityNodeMask, 1);
    P_set_i(p, K_Enable_Output_Subrects, 1);
    P_set_i(p, K_RTXValue, 0);
    P_set_ui(p, K_Preset_DLAA, 0);
    P_set_ui(p, K_Preset_Quality, 0);
    P_set_ui(p, K_Preset_Balanced, 0);
    P_set_ui(p, K_Preset_Performance, 0);
    P_set_ui(p, K_Preset_UltraPerformance, 0);
    P_set_ui(p, K_Preset_UltraQuality, 0);
}

static int available12(void) { return g12.inited ? g12.available : 1; }
static int availablevk(void) { return gvk.inited ? gvk.available : 1; }

static unsigned caps_params(struct param **out, int available)
{
    if (!out) return NGX_FAIL_InvalidParameter;
    struct param *p = P_new(0);
    if (!p) return NGX_FAIL_PlatformError;
    fill_caps(p, available);
    *out = p;
    return NGX_OK;
}

static struct param *g_persist12, *g_persistvk, *g_persist11;

static unsigned persistent_params(struct param **slot, struct param **out, int available)
{
    if (!out) return NGX_FAIL_InvalidParameter;
    EnterCriticalSection(&g_lock);
    if (!*slot) *slot = P_new(1);
    LeaveCriticalSection(&g_lock);
    if (!*slot) return NGX_FAIL_PlatformError;
    fill_caps(*slot, available);
    *out = *slot;
    return NGX_OK;
}

static unsigned feature_requirements(const struct ngx_feature_discovery_info *info, struct ngx_feature_requirement *out,
                                     int api_supported)
{
    if (!info || !out) return NGX_FAIL_InvalidParameter;
    memset(out, 0, sizeof *out);
    static const char os[] = "10.0.10240.16384";
    memcpy(out->MinOSVersion, os, sizeof os);
    if (info->FeatureID == NGX_Feature_SuperSampling && api_supported) {
        out->FeatureSupported = NGX_FeatureSupportResult_Supported;
        return NGX_OK;
    }
    out->FeatureSupported = NGX_FeatureSupportResult_AdapterUnsupported;
    return NGX_FAIL_FeatureNotSupported;
}

static unsigned create_feature(int api, int feature_id, struct param *p, void **out_handle, int available, int spatial)
{
    if (!out_handle || !p || p->magic != PARAM_MAGIC) return NGX_FAIL_InvalidParameter;
    *out_handle = NULL;
    if (feature_id != NGX_Feature_SuperSampling) {
        struct line l;
        L_begin(&l);
        L_s(&l, "create feature "); L_i(&l, feature_id);
        L_s(&l, feature_id == NGX_Feature_FrameGeneration ? " (frame generation)" :
                feature_id == NGX_Feature_RayReconstruction ? " (ray reconstruction)" : "");
        L_s(&l, ": not supported here");
        L_end(&l);
        return NGX_FAIL_FeatureNotSupported;
    }
    if (!available) {
        say("create feature SuperSampling: refused, the probe did not answer for this path (see init)");
        return NGX_FAIL_FeatureNotSupported;
    }
    struct feature *f = feature_new(api, p, spatial);
    if (!f) return NGX_FAIL_PlatformError;
    if (debug_on()) {
        /* DLSS 4's render preset hints (J..M), read for the record only. */
        struct line l;
        L_begin(&l);
        L_s(&l, "create feature: preset hints DLAA "); L_u(&l, P_getu(p, K_Preset_DLAA, 0));
        L_s(&l, " Quality "); L_u(&l, P_getu(p, K_Preset_Quality, 0));
        L_s(&l, " Balanced "); L_u(&l, P_getu(p, K_Preset_Balanced, 0));
        L_s(&l, " Performance "); L_u(&l, P_getu(p, K_Preset_Performance, 0));
        L_s(&l, " UltraPerformance "); L_u(&l, P_getu(p, K_Preset_UltraPerformance, 0));
        L_s(&l, " (ignored: MetalFX has no presets); alpha upscaling ");
        L_s(&l, (f->flags & NGX_DLSS_AlphaUpscaling) ? "asked (MetalFX scales all four channels)" : "no");
        L_end(&l);
    }
    unsigned rc = NGX_OK;
    if (!f->in_w || !f->in_h || !f->out_w || !f->out_h) rc = NGX_FAIL_InvalidParameter;
    else if (f->out_w < f->in_w || f->out_h < f->in_h) rc = NGX_FAIL_UnsupportedParameter;
    log_feature("create", f, rc);
    if (rc != NGX_OK) { feature_unlink(f); return rc; }
    *out_handle = f;
    return NGX_OK;
}

static unsigned release_feature(void *handle)
{
    struct feature *f = feature_of(handle);
    if (!f) return NGX_FAIL_FeatureNotFound;
    EnterCriticalSection(&g_lock);
    if (f->api == API_D3D12) destroy_context12(f);
    else destroy_contextvk(f);
    LeaveCriticalSection(&g_lock);
    {
        struct line l;
        L_begin(&l);
        L_s(&l, "release feature "); L_x(&l, f->Id);
        L_s(&l, ": "); L_u(&l, f->frames); L_s(&l, " frames, "); L_u(&l, f->refused); L_s(&l, " refused (blit fallback)");
        L_end(&l);
    }
    feature_unlink(f);
    return NGX_OK;
}

/* ================================================================ exports */
/* NGX's calling convention is cdecl, the MS x64 convention here. The
 * "Init" names are the DLL's (what NVIDIA's static SDK calls in the NGX
 * core): NVSDK_NGX_<api>_Init(app id, data path, device, feature info,
 * sdk version), _Init_Ext(app id, data path, device, sdk version, feature
 * info), _Init_ProjectID(project id, engine, engine version, data path,
 * device, sdk version, feature info), and the public SDK's
 * _Init_with_ProjectID(project id, engine, engine version, data path,
 * device, feature info, sdk version), for games that call it directly. */

static unsigned init12(ID3D12Device *dev, int sdk_version, const char *who)
{
    EnterCriticalSection(&g_lock);
    unsigned rc = dev12_init(dev, sdk_version, who);
    LeaveCriticalSection(&g_lock);
    return rc;
}

unsigned NVSDK_NGX_D3D12_Init(unsigned long long app_id, const wchar_t *path, ID3D12Device *dev, const void *info, int sdk)
{
    (void)app_id; (void)path; (void)info;
    return init12(dev, sdk, "NVSDK_NGX_D3D12_Init");
}
unsigned NVSDK_NGX_D3D12_Init_Ext(unsigned long long app_id, const wchar_t *path, ID3D12Device *dev, int sdk, const void *info)
{
    (void)app_id; (void)path; (void)info;
    return init12(dev, sdk, "NVSDK_NGX_D3D12_Init_Ext");
}
unsigned NVSDK_NGX_D3D12_Init_ProjectID(const char *project, int engine, const char *engine_version, const wchar_t *path,
                                        ID3D12Device *dev, int sdk, const void *info)
{
    (void)project; (void)engine; (void)engine_version; (void)path; (void)info;
    return init12(dev, sdk, "NVSDK_NGX_D3D12_Init_ProjectID");
}
unsigned NVSDK_NGX_D3D12_Init_with_ProjectID(const char *project, int engine, const char *engine_version, const wchar_t *path,
                                             ID3D12Device *dev, const void *info, int sdk)
{
    (void)project; (void)engine; (void)engine_version; (void)path; (void)info;
    return init12(dev, sdk, "NVSDK_NGX_D3D12_Init_with_ProjectID");
}
unsigned NVSDK_NGX_D3D12_Shutdown(void)
{
    EnterCriticalSection(&g_lock);
    dev12_release();
    LeaveCriticalSection(&g_lock);
    return NGX_OK;
}
unsigned NVSDK_NGX_D3D12_Shutdown1(ID3D12Device *dev) { (void)dev; return NVSDK_NGX_D3D12_Shutdown(); }
unsigned NVSDK_NGX_D3D12_GetParameters(struct param **out) { return persistent_params(&g_persist12, out, available12()); }
unsigned NVSDK_NGX_D3D12_GetCapabilityParameters(struct param **out) { return caps_params(out, available12()); }
unsigned NVSDK_NGX_D3D12_AllocateParameters(struct param **out)
{
    if (!out) return NGX_FAIL_InvalidParameter;
    *out = P_new(0);
    return *out ? NGX_OK : NGX_FAIL_PlatformError;
}
unsigned NVSDK_NGX_D3D12_PopulateParameters_Impl(struct param *p)
{
    if (!p || p->magic != PARAM_MAGIC) return NGX_FAIL_InvalidParameter;
    fill_caps(p, available12());
    return NGX_OK;
}
unsigned NVSDK_NGX_D3D12_DestroyParameters(struct param *p)
{
    if (!p || p->magic != PARAM_MAGIC) return NGX_FAIL_InvalidParameter;
    P_free(p);
    return NGX_OK;
}
unsigned NVSDK_NGX_D3D12_GetScratchBufferSize(int feature, const struct param *p, size_t *out)
{
    (void)feature; (void)p;
    if (!out) return NGX_FAIL_InvalidParameter;
    *out = 0;
    return NGX_OK;
}
unsigned NVSDK_NGX_D3D12_CreateFeature(ID3D12GraphicsCommandList *cl, int feature, struct param *p, void **out)
{
    if (!cl) return NGX_FAIL_InvalidParameter;
    EnterCriticalSection(&g_lock);
    if (!g12.inited) {
        /* Created without an Init: the device is the command list's. */
        ID3D12Device *dev = NULL;
        if (SUCCEEDED(ID3D12GraphicsCommandList_GetDevice(cl, &IID_ID3D12Device, (void **)&dev)) && dev) {
            dev12_init(dev, 0, "NVSDK_NGX_D3D12_CreateFeature (no Init before it)");
            ID3D12Device_Release(dev);
        }
    }
    int avail = g12.inited && g12.available, spatial = g12.spatial;
    LeaveCriticalSection(&g_lock);
    return create_feature(API_D3D12, feature, p, out, avail, spatial);
}
unsigned NVSDK_NGX_D3D12_ReleaseFeature(void *handle) { return release_feature(handle); }
unsigned NVSDK_NGX_D3D12_EvaluateFeature(ID3D12GraphicsCommandList *cl, const void *handle, struct param *p, void *cb)
{
    (void)cb;
    struct feature *f = feature_of(handle);
    if (!f || f->api != API_D3D12) return NGX_FAIL_FeatureNotFound;
    return eval12(cl, f, p);
}
unsigned NVSDK_NGX_D3D12_EvaluateFeature_C(ID3D12GraphicsCommandList *cl, const void *handle, struct param *p, void *cb)
{
    return NVSDK_NGX_D3D12_EvaluateFeature(cl, handle, p, cb);
}
unsigned NVSDK_NGX_D3D12_GetFeatureRequirements(void *adapter, const struct ngx_feature_discovery_info *info,
                                                struct ngx_feature_requirement *out)
{
    (void)adapter;
    return feature_requirements(info, out, 1);
}

/* ---- Vulkan ---- */
static unsigned initvk(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa,
                       PFN_vkGetDeviceProcAddr gdpa, int sdk, const char *who)
{
    EnterCriticalSection(&g_lock);
    unsigned rc = devvk_init(inst, pd, dev, gipa, gdpa, sdk, who);
    LeaveCriticalSection(&g_lock);
    return rc;
}

unsigned NVSDK_NGX_VULKAN_RequiredExtensions(unsigned *inst_n, const char ***inst_ext, unsigned *dev_n, const char ***dev_ext)
{
    static const char *none[1] = { NULL };
    if (inst_n) *inst_n = 0;
    if (dev_n) *dev_n = 0;
    if (inst_ext) *inst_ext = none;
    if (dev_ext) *dev_ext = none;
    return NGX_OK;
}
unsigned NVSDK_NGX_VULKAN_Init(unsigned long long app_id, const wchar_t *path, VkInstance inst, VkPhysicalDevice pd, VkDevice dev,
                               PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, const void *info, int sdk)
{
    (void)app_id; (void)path; (void)info;
    return initvk(inst, pd, dev, gipa, gdpa, sdk, "NVSDK_NGX_VULKAN_Init");
}
unsigned NVSDK_NGX_VULKAN_Init_Ext(unsigned long long app_id, const wchar_t *path, VkInstance inst, VkPhysicalDevice pd,
                                   VkDevice dev, int sdk, const void *info)
{
    (void)app_id; (void)path; (void)info;
    return initvk(inst, pd, dev, NULL, NULL, sdk, "NVSDK_NGX_VULKAN_Init_Ext");
}
unsigned NVSDK_NGX_VULKAN_Init_Ext2(unsigned long long app_id, const wchar_t *path, VkInstance inst, VkPhysicalDevice pd,
                                    VkDevice dev, PFN_vkGetInstanceProcAddr gipa, PFN_vkGetDeviceProcAddr gdpa, int sdk,
                                    const void *info)
{
    (void)app_id; (void)path; (void)info;
    return initvk(inst, pd, dev, gipa, gdpa, sdk, "NVSDK_NGX_VULKAN_Init_Ext2");
}
unsigned NVSDK_NGX_VULKAN_Init_ProjectID(const char *project, int engine, const char *engine_version, const wchar_t *path,
                                         VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa,
                                         PFN_vkGetDeviceProcAddr gdpa, int sdk, const void *info)
{
    (void)project; (void)engine; (void)engine_version; (void)path; (void)info;
    return initvk(inst, pd, dev, gipa, gdpa, sdk, "NVSDK_NGX_VULKAN_Init_ProjectID");
}
unsigned NVSDK_NGX_VULKAN_Init_ProjectID_Ext(const char *project, int engine, const char *engine_version, const wchar_t *path,
                                             VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa,
                                             PFN_vkGetDeviceProcAddr gdpa, int sdk, const void *info)
{
    (void)project; (void)engine; (void)engine_version; (void)path; (void)info;
    return initvk(inst, pd, dev, gipa, gdpa, sdk, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext");
}
unsigned NVSDK_NGX_VULKAN_Init_with_ProjectID(const char *project, int engine, const char *engine_version, const wchar_t *path,
                                              VkInstance inst, VkPhysicalDevice pd, VkDevice dev, PFN_vkGetInstanceProcAddr gipa,
                                              PFN_vkGetDeviceProcAddr gdpa, const void *info, int sdk)
{
    (void)project; (void)engine; (void)engine_version; (void)path; (void)info;
    return initvk(inst, pd, dev, gipa, gdpa, sdk, "NVSDK_NGX_VULKAN_Init_with_ProjectID");
}
unsigned NVSDK_NGX_VULKAN_Shutdown(void)
{
    EnterCriticalSection(&g_lock);
    devvk_release();
    LeaveCriticalSection(&g_lock);
    return NGX_OK;
}
unsigned NVSDK_NGX_VULKAN_Shutdown1(VkDevice dev) { (void)dev; return NVSDK_NGX_VULKAN_Shutdown(); }
unsigned NVSDK_NGX_VULKAN_GetParameters(struct param **out) { return persistent_params(&g_persistvk, out, availablevk()); }
unsigned NVSDK_NGX_VULKAN_GetCapabilityParameters(struct param **out) { return caps_params(out, availablevk()); }
unsigned NVSDK_NGX_VULKAN_AllocateParameters(struct param **out) { return NVSDK_NGX_D3D12_AllocateParameters(out); }
unsigned NVSDK_NGX_VULKAN_PopulateParameters_Impl(struct param *p)
{
    if (!p || p->magic != PARAM_MAGIC) return NGX_FAIL_InvalidParameter;
    fill_caps(p, availablevk());
    return NGX_OK;
}
unsigned NVSDK_NGX_VULKAN_DestroyParameters(struct param *p) { return NVSDK_NGX_D3D12_DestroyParameters(p); }
unsigned NVSDK_NGX_VULKAN_GetScratchBufferSize(int feature, const struct param *p, size_t *out)
{
    return NVSDK_NGX_D3D12_GetScratchBufferSize(feature, p, out);
}
unsigned NVSDK_NGX_VULKAN_CreateFeature1(VkDevice dev, VkCommandBuffer cb, int feature, struct param *p, void **out)
{
    (void)dev; (void)cb;
    EnterCriticalSection(&g_lock);
    int avail = gvk.inited && gvk.available, spatial = gvk.spatial;
    LeaveCriticalSection(&g_lock);
    if (!gvk.inited) { say("create Vulkan feature before NVSDK_NGX_VULKAN_Init: not initialized"); return NGX_FAIL_NotInitialized; }
    return create_feature(API_VK, feature, p, out, avail, spatial);
}
unsigned NVSDK_NGX_VULKAN_CreateFeature(VkCommandBuffer cb, int feature, struct param *p, void **out)
{
    return NVSDK_NGX_VULKAN_CreateFeature1(gvk.dev, cb, feature, p, out);
}
unsigned NVSDK_NGX_VULKAN_ReleaseFeature(void *handle) { return release_feature(handle); }
unsigned NVSDK_NGX_VULKAN_EvaluateFeature(VkCommandBuffer cb, const void *handle, struct param *p, void *callback)
{
    (void)callback;
    struct feature *f = feature_of(handle);
    if (!f || f->api != API_VK) return NGX_FAIL_FeatureNotFound;
    return evalvk(cb, f, p);
}
unsigned NVSDK_NGX_VULKAN_EvaluateFeature_C(VkCommandBuffer cb, const void *handle, struct param *p, void *callback)
{
    return NVSDK_NGX_VULKAN_EvaluateFeature(cb, handle, p, callback);
}
unsigned NVSDK_NGX_VULKAN_GetFeatureRequirements(VkInstance inst, VkPhysicalDevice pd, const struct ngx_feature_discovery_info *info,
                                                 struct ngx_feature_requirement *out)
{
    (void)inst; (void)pd;
    return feature_requirements(info, out, 1);
}
unsigned NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(const struct ngx_feature_discovery_info *info, uint32_t *n,
                                                                  VkExtensionProperties **ext)
{
    (void)info;
    if (n) *n = 0;
    if (ext) *ext = NULL;
    return NGX_OK;
}
unsigned NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(VkInstance inst, VkPhysicalDevice pd,
                                                                const struct ngx_feature_discovery_info *info, uint32_t *n,
                                                                VkExtensionProperties **ext)
{
    (void)inst; (void)pd; (void)info;
    if (n) *n = 0;
    if (ext) *ext = NULL;
    return NGX_OK;
}

/* ---- D3D11: the entry points, no feature (DXVK's D3D11 has no command
 * buffer interop to record into). The game's init succeeds and its
 * capability query says no. ---- */
unsigned NVSDK_NGX_D3D11_Init(unsigned long long app_id, const wchar_t *path, void *dev, const void *info, int sdk)
{
    (void)app_id; (void)path; (void)dev; (void)info; (void)sdk;
    say("D3D11 init: SuperSampling unavailable on D3D11 (no command-buffer interop in DXVK)");
    return NGX_OK;
}
unsigned NVSDK_NGX_D3D11_Init_Ext(unsigned long long app_id, const wchar_t *path, void *dev, int sdk, const void *info)
{
    return NVSDK_NGX_D3D11_Init(app_id, path, dev, info, sdk);
}
unsigned NVSDK_NGX_D3D11_Init_ProjectID(const char *project, int engine, const char *engine_version, const wchar_t *path,
                                        void *dev, int sdk, const void *info)
{
    (void)project; (void)engine; (void)engine_version;
    return NVSDK_NGX_D3D11_Init(0, path, dev, info, sdk);
}
unsigned NVSDK_NGX_D3D11_Init_with_ProjectID(const char *project, int engine, const char *engine_version, const wchar_t *path,
                                             void *dev, const void *info, int sdk)
{
    (void)project; (void)engine; (void)engine_version;
    return NVSDK_NGX_D3D11_Init(0, path, dev, info, sdk);
}
unsigned NVSDK_NGX_D3D11_Shutdown(void) { return NGX_OK; }
unsigned NVSDK_NGX_D3D11_Shutdown1(void *dev) { (void)dev; return NGX_OK; }
unsigned NVSDK_NGX_D3D11_GetParameters(struct param **out) { return persistent_params(&g_persist11, out, 0); }
unsigned NVSDK_NGX_D3D11_GetCapabilityParameters(struct param **out) { return caps_params(out, 0); }
unsigned NVSDK_NGX_D3D11_AllocateParameters(struct param **out) { return NVSDK_NGX_D3D12_AllocateParameters(out); }
unsigned NVSDK_NGX_D3D11_PopulateParameters_Impl(struct param *p)
{
    if (!p || p->magic != PARAM_MAGIC) return NGX_FAIL_InvalidParameter;
    fill_caps(p, 0);
    return NGX_OK;
}
unsigned NVSDK_NGX_D3D11_DestroyParameters(struct param *p) { return NVSDK_NGX_D3D12_DestroyParameters(p); }
unsigned NVSDK_NGX_D3D11_GetScratchBufferSize(int feature, const struct param *p, size_t *out)
{
    return NVSDK_NGX_D3D12_GetScratchBufferSize(feature, p, out);
}
unsigned NVSDK_NGX_D3D11_CreateFeature(void *ctx, int feature, struct param *p, void **out)
{
    (void)ctx; (void)feature; (void)p;
    if (out) *out = NULL;
    return NGX_FAIL_FeatureNotSupported;
}
unsigned NVSDK_NGX_D3D11_ReleaseFeature(void *handle) { (void)handle; return NGX_FAIL_FeatureNotFound; }
unsigned NVSDK_NGX_D3D11_EvaluateFeature(void *ctx, const void *handle, struct param *p, void *cb)
{
    (void)ctx; (void)handle; (void)p; (void)cb;
    return NGX_FAIL_FeatureNotFound;
}
unsigned NVSDK_NGX_D3D11_EvaluateFeature_C(void *ctx, const void *handle, struct param *p, void *cb)
{
    return NVSDK_NGX_D3D11_EvaluateFeature(ctx, handle, p, cb);
}
unsigned NVSDK_NGX_D3D11_GetFeatureRequirements(void *adapter, const struct ngx_feature_discovery_info *info,
                                                struct ngx_feature_requirement *out)
{
    (void)adapter;
    return feature_requirements(info, out, 0);
}

/* ---- common ---- */
unsigned NVSDK_NGX_UpdateFeature(const struct ngx_application_identifier *id, int feature)
{
    (void)id; (void)feature;
    return NGX_OK;
}
unsigned NVSDK_NGX_GetSnippetVersion(void) { return (3u << 16) | (5u << 8); }

/* The C form of the parameter interface (nvsdk_ngx_params.h). */
void NVSDK_NGX_Parameter_SetULL(struct param *p, const char *k, unsigned long long v) { P_set_ull(p, k, v); }
void NVSDK_NGX_Parameter_SetF(struct param *p, const char *k, float v) { P_set_f(p, k, v); }
void NVSDK_NGX_Parameter_SetD(struct param *p, const char *k, double v) { P_set_d(p, k, v); }
void NVSDK_NGX_Parameter_SetUI(struct param *p, const char *k, unsigned v) { P_set_ui(p, k, v); }
void NVSDK_NGX_Parameter_SetI(struct param *p, const char *k, int v) { P_set_i(p, k, v); }
void NVSDK_NGX_Parameter_SetD3d11Resource(struct param *p, const char *k, void *v) { P_set_vp(p, k, v); }
void NVSDK_NGX_Parameter_SetD3d12Resource(struct param *p, const char *k, void *v) { P_set_vp(p, k, v); }
void NVSDK_NGX_Parameter_SetVoidPointer(struct param *p, const char *k, void *v) { P_set_vp(p, k, v); }
unsigned NVSDK_NGX_Parameter_GetULL(struct param *p, const char *k, unsigned long long *v) { return P_get_ull(p, k, v); }
unsigned NVSDK_NGX_Parameter_GetF(struct param *p, const char *k, float *v) { return P_get_f(p, k, v); }
unsigned NVSDK_NGX_Parameter_GetD(struct param *p, const char *k, double *v) { return P_get_d(p, k, v); }
unsigned NVSDK_NGX_Parameter_GetUI(struct param *p, const char *k, unsigned *v) { return P_get_ui(p, k, v); }
unsigned NVSDK_NGX_Parameter_GetI(struct param *p, const char *k, int *v) { return P_get_i(p, k, v); }
unsigned NVSDK_NGX_Parameter_GetD3d11Resource(struct param *p, const char *k, void **v) { return P_get_vp(p, k, v); }
unsigned NVSDK_NGX_Parameter_GetD3d12Resource(struct param *p, const char *k, void **v) { return P_get_vp(p, k, v); }
unsigned NVSDK_NGX_Parameter_GetVoidPointer(struct param *p, const char *k, void **v) { return P_get_vp(p, k, v); }

/* ---- SteamARM's own, for its tests (tests/win/d3d12_dlss.c) ---- */
const char *SteamARM_NGX_Ident(void) { return SA_NGX_IDENT; }
/* out[0..3]: the device's probe status, caps, driver id, max scale x 100
 * (D3D12 when inited, else Vulkan); returns 1 with a device, 0 without. */
int SteamARM_NGX_DeviceInfo(unsigned out[4])
{
    if (!out) return 0;
    const int d12 = g12.inited;
    if (!d12 && !gvk.inited) { out[0] = out[1] = out[2] = out[3] = 0; return 0; }
    out[0] = d12 ? g12.probe_st : gvk.probe_st;
    out[1] = d12 ? g12.caps : gvk.caps;
    out[2] = d12 ? g12.driver : gvk.driver;
    out[3] = (unsigned)((d12 ? g12.max_scale : gvk.max_scale) * 100.0f + 0.5f);
    return 1;
}
/* out[0..5]: frames evaluated, refused (blit fallback), the last status,
 * the context id's low bits, the path (0 temporal ENCODE, 1 spatial: DLSS
 * 1), 0. */
int SteamARM_NGX_FeatureStats(const void *handle, unsigned out[6])
{
    struct feature *f = feature_of(handle);
    if (!f || !out) return 0;
    out[0] = f->frames;
    out[1] = f->refused;
    out[2] = f->last_st;
    out[3] = (unsigned)f->context;
    out[4] = f->spatial ? 1u : 0u;
    out[5] = 0;
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = inst;
        InitializeCriticalSection(&g_lock);
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
