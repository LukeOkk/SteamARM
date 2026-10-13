/*
 * SteamARM's FidelityFX API DLL: a game's FSR 3.1 / FSR 4 upscale handed to
 * Apple's MetalFX temporal scaler, with the game's own DLL as the fallback.
 *
 * One source, three Wine builtins (build.sh): amd_fidelityfx_dx12.dll and
 * amd_fidelityfx_vk.dll (FidelityFX SDK 1.1.x, one DLL per backend) and
 * amd_fidelityfx_upscaler_dx12.dll (SDK 2.x: the game's own
 * amd_fidelityfx_loader_dx12.dll, never replaced, LoadLibraryA's it by name
 * and forwards every upscale call: it expects the same five exports). AMD's
 * exports at AMD's ordinals: 1 ffxConfigure, 2 ffxCreateContext,
 * 3 ffxDestroyContext, 4 ffxDispatch, 5 ffxQuery.
 *
 * What it does, in one paragraph. With STEAMARM_WIN_UPSCALER=metalfx an
 * upscale context (FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE, 0x10000) whose
 * descriptor chain this DLL understands (a backend descriptor, decoded per
 * SDK generation, see T_BACKEND_*; SDK 2.x's allocation callbacks, passed
 * through; a version override naming this DLL's version or none; SDK 2.x's
 * version header) is PROBEd through SteamARM's command channel
 * (shim/mfx_temporal.h: a vkCmdUpdateBuffer of 320 bytes in a command
 * buffer of a pool of its own, never submitted; vkd3d-proton's
 * ID3D12DXVKInteropDevice1 gives the VkDevice behind an ID3D12Device). PROBE
 * OK with the temporal cap: the context is this DLL's, and every
 * FFX_API_DISPATCH_DESC_TYPE_UPSCALE (0x10001) is recorded as an ENCODE in
 * the game's command list (BeginVkCommandBufferInterop for D3D12, the
 * VkCommandBuffer itself for Vulkan), its status read back in the same
 * call. Anything else -- PROBE not OK, a descriptor it does not know, a
 * refused ENCODE, a dispatch it has no MetalFX equivalent for (the reactive
 * mask generator, a debug view, a non-linear colour space, see
 * dispatch_upscale) -- and the game's own DLL takes over: its file
 * (GetModuleFileNameW of this module: Wine keeps the path of the file it
 * replaced) is copied to %LOCALAPPDATA%\SteamARM\ffx\<hash>.dll (a name the
 * override does not cover) and LoadLibrary'd; a refused ENCODE creates the
 * original context at once, from a copy of the creation descriptors, and
 * that context is used from then on, the refused frame included. Every
 * call into the game's DLL goes without an ffxOverrideVersion naming THIS
 * DLL's version (unlinked for the call: AMD's GetffxProvider has no
 * provider for a version it does not know and answers NO_PROVIDER; without
 * the override AMD picks its own default). With no game DLL to fall back
 * to, a refused ENCODE returns FFX_RUNTIME_ERROR. Quality is never silently
 * replaced with bilinear output. Contexts of
 * this DLL are kept in a table: a pointer not in it is forwarded, never
 * dereferenced; frame generation and swapchain contexts are always
 * forwarded. Without the variable (FORWARD mode) every call goes to the
 * original unchanged. Queries 0x10002..0x10005 (ratio, render resolution,
 * jitter phase count, jitter offset) are answered with AMD's formulas
 * (ffx_fsr3upscaler.cpp), 0x1000a (resource requirements) and 6 (the
 * provider's version) for this DLL's contexts; GPU memory (0x10008/9) with
 * the original's numbers; the version list (4) is this DLL's version first,
 * then the original's.
 *
 * KERNEL32 is the only import (no C runtime; build.sh) and
 * wine-builtin-mark.py writes Wine's builtin marker with prefer-native, so
 * Wine takes it over a game's copy only under the override "b,n"
 * (tools/steamarm-fex-proton sets it). SA_FFX_QUERY_IDENT (0x53410001)
 * says who answered (tests/win/ffx_load.c). Lines go to OutputDebugStringA
 * and stderr: [ffx-metalfx <dll>] ...; SA_FFX_DEBUG=1 adds one per dispatch
 * for the first frames of each context. SA_FFX_TEST_REFUSE=1 (tests only,
 * tests/win/run_d3d12_ffx.sh's refuse mode) sends every ENCODE malformed
 * (size + 1), which the shim refuses with BAD_ARGS: the fallback paths
 * without a driver that refuses.
 */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <stdint.h>
#include <stdarg.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "../../shim/mfx_temporal.h"

#ifndef FFX_IDENT
#define FFX_IDENT "steamarm-builtin"
#endif
#ifndef FFX_DLL_NAME
#define FFX_DLL_NAME "amd_fidelityfx"
#endif
/* The SDK generation this DLL stands in for: build.sh sets FFX_SDK2=1 for
 * the SDK 2.x names (amd_fidelityfx_upscaler_dx12, loaded by the game's
 * amd_fidelityfx_loader_dx12.dll), 0 for SDK 1.1.x's amd_fidelityfx_dx12
 * and _vk. The two decode the backend descriptor type 3 differently. */
#ifndef FFX_SDK2
#define FFX_SDK2 0
#endif

/* ------------------------------------------------------------------ API */
typedef uint32_t ffxReturnCode_t;
typedef void *ffxContext;
typedef struct ffxApiHeader { uint64_t type; struct ffxApiHeader *pNext; } ffxApiHeader;
typedef void (*ffxApiMessage)(uint32_t type, const wchar_t *message);
typedef void *(*ffxAlloc)(void *pUserData, uint64_t size);
typedef void (*ffxDealloc)(void *pUserData, void *pMem);
typedef struct ffxAllocationCallbacks { void *pUserData; ffxAlloc alloc; ffxDealloc dealloc; } ffxAllocationCallbacks;

enum {
    FFX_OK = 0, FFX_ERROR = 1, FFX_UNKNOWN_DESCTYPE = 2, FFX_RUNTIME_ERROR = 3, FFX_NO_PROVIDER = 4,
    FFX_ERROR_MEMORY = 5, FFX_ERROR_PARAMETER = 6, FFX_NO_SUPPORT_NEW_DESCTYPE = 7,
};

typedef ffxReturnCode_t (*PfnCreate)(ffxContext *, ffxApiHeader *, const ffxAllocationCallbacks *);
typedef ffxReturnCode_t (*PfnDestroy)(ffxContext *, const ffxAllocationCallbacks *);
typedef ffxReturnCode_t (*PfnConfigure)(ffxContext *, const ffxApiHeader *);
typedef ffxReturnCode_t (*PfnQuery)(ffxContext *, ffxApiHeader *);
typedef ffxReturnCode_t (*PfnDispatch)(ffxContext *, const ffxApiHeader *);

/* Descriptor types (ffx_api.h, ffx_upscale.h, dx12/ffx_api_dx12.h,
 * vk/ffx_api_vk.h; SDK 1.1.x values, which SDK 2.x keeps for these). */
#define FFX_EFFECT_MASK            0x00ff0000u
#define FFX_EFFECT_UPSCALE         0x00010000u
#define T_CREATE_UPSCALE           0x00010000u
#define T_DISPATCH_UPSCALE         0x00010001u
#define T_Q_RATIO                  0x00010002u
#define T_Q_RENDERRES              0x00010003u
#define T_Q_PHASECOUNT             0x00010004u
#define T_Q_JITTER                 0x00010005u
#define T_DISPATCH_REACTIVE        0x00010006u
#define T_CFG_KEYVALUE             0x00010007u
#define T_Q_GPUMEM                 0x00010008u
#define T_Q_GPUMEM2                0x00010009u
#define T_Q_RESREQ                 0x0001000au
#define T_CREATE_UPSCALE_VERSION   0x0001000bu
#define T_CFG_GLOBALDEBUG1         1u
#define T_Q_GET_VERSIONS           4u
#define T_OVERRIDE_VERSION         5u
#define T_Q_PROVIDER_VERSION       6u
#define T_CFG_GLOBALDEBUG          7u
/* Backend descriptor types, per SDK generation. SDK 1.1.x
 * (dx12/ffx_api_dx12.h, vk/ffx_api_vk.h): 2 = DX12 backend, 3 = Vulkan
 * backend. SDK 2.x (ffx_api.h's FFX_API_MAKE_BACKEND_SUB_ID(backend id,
 * sub id), DX12's id 0, VK's 0x02000000): 2 = DX12 backend, 3 = DX12
 * ALLOCATION_CALLBACKS (ffxCreateBackendDX12AllocationCallbacksDesc), the
 * Vulkan backend 0x02000003. An SDK 2.x DLL reading 3 as Vulkan would
 * refuse a game's allocation callbacks as "two backends". */
#define T_BACKEND_DX12             2u
#define T_BACKEND_VK_SDK1          3u
#define T_BACKEND_DX12_ALLOC_SDK2  3u
#define T_BACKEND_VK_SDK2          0x02000003u

/* ffxDispatchDescUpscale.flags */
#define DF_DEBUG_VIEW     (1u << 0)
#define DF_NON_LINEAR_SRGB (1u << 1)
#define DF_NON_LINEAR_PQ  (1u << 2)

/* ffxCreateContextDescUpscale flags */
#define UP_HDR            (1u << 0)
#define UP_MV_DISPLAY     (1u << 1)
#define UP_MV_JITTERED    (1u << 2)
#define UP_DEPTH_INVERTED (1u << 3)
#define UP_DEPTH_INFINITE (1u << 4)
#define UP_AUTO_EXPOSURE  (1u << 5)
#define UP_DYNAMIC_RES    (1u << 6)
#define UP_DEBUG_CHECKING (1u << 7)
#define UP_NON_LINEAR     (1u << 8)
#define UP_DEBUG_VIS      (1u << 9)

struct FfxApiDimensions2D { uint32_t width, height; };
struct FfxApiFloatCoords2D { float x, y; };
struct FfxApiResourceDescription { uint32_t type, format, width, height, depth, mipCount, flags, usage; };
struct FfxApiResource { void *resource; struct FfxApiResourceDescription description; uint32_t state; };
struct FfxApiEffectMemoryUsage { uint64_t totalUsageInBytes, aliasableUsageInBytes; };

struct ffxCreateContextDescUpscale {
    ffxApiHeader header; uint32_t flags; struct FfxApiDimensions2D maxRenderSize, maxUpscaleSize; ffxApiMessage fpMessage;
};
struct ffxDispatchDescUpscale {
    ffxApiHeader header; void *commandList;
    struct FfxApiResource color, depth, motionVectors, exposure, reactive, transparencyAndComposition, output;
    struct FfxApiFloatCoords2D jitterOffset, motionVectorScale;
    struct FfxApiDimensions2D renderSize, upscaleSize;
    BOOLEAN enableSharpening; float sharpness, frameTimeDelta, preExposure; BOOLEAN reset;
    float cameraNear, cameraFar, cameraFovAngleVertical, viewSpaceToMetersFactor; uint32_t flags;
};
struct ffxQueryDescGetVersions { ffxApiHeader header; uint64_t createDescType; void *device; uint64_t *outputCount; uint64_t *versionIds; const char **versionNames; };
struct ffxOverrideVersion { ffxApiHeader header; uint64_t versionId; };
struct ffxQueryGetProviderVersion { ffxApiHeader header; uint64_t versionId; const char *versionName; };
struct ffxQueryRatio { ffxApiHeader header; uint32_t qualityMode; float *pOutUpscaleRatio; };
struct ffxQueryRenderRes { ffxApiHeader header; uint32_t displayWidth, displayHeight, qualityMode; uint32_t *pOutRenderWidth, *pOutRenderHeight; };
struct ffxQueryPhaseCount { ffxApiHeader header; uint32_t renderWidth, displayWidth; int32_t *pOutPhaseCount; };
struct ffxQueryJitter { ffxApiHeader header; int32_t index, phaseCount; float *pOutX, *pOutY; };
struct ffxQueryGpuMem { ffxApiHeader header; struct FfxApiEffectMemoryUsage *usage; };
struct ffxQueryResReq { ffxApiHeader header; uint64_t required_resources, optional_resources; };
struct ffxCreateBackendDX12Desc { ffxApiHeader header; ID3D12Device *device; };
struct ffxCreateBackendVKDesc { ffxApiHeader header; VkDevice vkDevice; VkPhysicalDevice vkPhysicalDevice; PFN_vkGetDeviceProcAddr vkDeviceProcAddr; };
/* SDK 2.x: five callbacks (resource and heap allocation, their release, a
 * constant buffer allocator), each may be null; passed through as they are. */
struct ffxCreateBackendDX12AllocationCallbacksDesc { ffxApiHeader header; void *fn[5]; };
struct ffxCreateContextDescUpscaleVersion { ffxApiHeader header; uint32_t version; };
struct ffxConfigureDescGlobalDebug1 { ffxApiHeader header; ffxApiMessage fpMessage; uint32_t debugLevel; };

_Static_assert(sizeof(struct FfxApiResource) == 48, "FfxApiResource is 48 bytes");
_Static_assert(sizeof(struct ffxDispatchDescUpscale) == 432, "ffxDispatchDescUpscale is 432 bytes");
_Static_assert(sizeof(struct ffxCreateContextDescUpscale) == 48, "ffxCreateContextDescUpscale is 48 bytes");
_Static_assert(__builtin_offsetof(struct ffxDispatchDescUpscale, flags) == 428, "flags at 428");
_Static_assert(__builtin_offsetof(struct ffxDispatchDescUpscale, reset) == 408, "reset at 408");
_Static_assert(sizeof(struct ffxCreateBackendDX12AllocationCallbacksDesc) == 56, "the allocation callbacks desc is 56 bytes");

/* This DLL's version, as a provider id (AMD's form: a tag in the high
 * half, (major << 22 | minor << 12 | patch) in the low) and a name. */
#define SA_FFX_VERSION_ID 0x53414D4600400000ull   /* "SAMF" << 32 | 1.0.0 */
static const char SA_FFX_VERSION_NAME[] = "MetalFX temporal 1.0.0 (SteamARM)";

/* SteamARM's own query: who answered (tests/win/ffx_load.c). */
#define SA_FFX_QUERY_IDENT 0x53410001ull
struct sa_ffx_query_ident { ffxApiHeader header; char who[64]; uint32_t marker; };

/* ------------------------------------------------- no C runtime: helpers */
static HMODULE g_self;

void *memcpy(void *d, const void *s, size_t n)
{
    void *r = d;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(s), "+c"(n) : : "memory");
    return r;
}
void *memset(void *d, int c, size_t n)
{
    void *r = d;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return r;
}
static size_t wlen(const WCHAR *s) { size_t n = 0; while (s[n]) n++; return n; }
static int seq(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return *a == *b; }

static void *xalloc(size_t n) { return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n); }
static void xfree(void *p) { if (p) HeapFree(GetProcessHeap(), 0, p); }

/* A small printf: %s %d %u %x %llu %llx %p %f (4 decimals) %c %%. */
static char *put_str(char *p, char *end, const char *s) { while (*s && p < end) *p++ = *s++; return p; }
static char *put_u(char *p, char *end, uint64_t v, unsigned base, int upper)
{
    char tmp[24]; int n = 0;
    const char *dig = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { tmp[n++] = dig[v % base]; v /= base; } while (v);
    while (n && p < end) *p++ = tmp[--n];
    return p;
}
static char *put_f(char *p, char *end, double v)
{
    if (v < 0) { if (p < end) *p++ = '-'; v = -v; }
    if (v > 1e15) return put_str(p, end, "big");
    uint64_t ip = (uint64_t)v;
    uint64_t fp = (uint64_t)((v - (double)ip) * 10000.0 + 0.5);
    if (fp >= 10000) { ip++; fp -= 10000; }
    p = put_u(p, end, ip, 10, 0);
    if (p < end) *p++ = '.';
    char d[4] = { (char)('0' + fp / 1000 % 10), (char)('0' + fp / 100 % 10), (char)('0' + fp / 10 % 10), (char)('0' + fp % 10) };
    for (int i = 0; i < 4 && p < end; i++) *p++ = d[i];
    return p;
}
static int vfmt(char *buf, size_t size, const char *fmt, va_list ap)
{
    char *p = buf, *end = buf + size - 1;
    for (; *fmt && p < end; fmt++) {
        if (*fmt != '%') { *p++ = *fmt; continue; }
        fmt++;
        int ll = 0;
        while (*fmt == 'l') { ll++; fmt++; }
        switch (*fmt) {
        case 's': { const char *s = va_arg(ap, const char *); p = put_str(p, end, s ? s : "(null)"); break; }
        case 'c': { *p++ = (char)va_arg(ap, int); break; }
        case 'd': { int64_t v = ll >= 2 ? va_arg(ap, int64_t) : va_arg(ap, int);
                    if (v < 0) { *p++ = '-'; v = -v; } p = put_u(p, end, (uint64_t)v, 10, 0); break; }
        case 'u': { uint64_t v = ll >= 2 ? va_arg(ap, uint64_t) : va_arg(ap, unsigned); p = put_u(p, end, v, 10, 0); break; }
        case 'x': { uint64_t v = ll >= 2 ? va_arg(ap, uint64_t) : va_arg(ap, unsigned); p = put_u(p, end, v, 16, 0); break; }
        case 'p': { uint64_t v = (uint64_t)(uintptr_t)va_arg(ap, void *); p = put_str(p, end, "0x"); p = put_u(p, end, v, 16, 0); break; }
        case 'f': { double v = va_arg(ap, double); p = put_f(p, end, v); break; }
        case '%': *p++ = '%'; break;
        default: *p++ = '?'; break;
        }
    }
    *p = 0;
    return (int)(p - buf);
}

static int g_debug = -1;
static int debug_on(void)
{
    if (g_debug < 0) {
        char v[8];
        g_debug = GetEnvironmentVariableA("SA_FFX_DEBUG", v, sizeof v) > 0 && v[0] == '1';
    }
    return g_debug;
}

static void say(const char *fmt, ...)
{
    char line[512], *p = line;
    p = put_str(p, line + sizeof line - 2, "[ffx-metalfx " FFX_DLL_NAME "] ");
    va_list ap;
    va_start(ap, fmt);
    p += vfmt(p, (size_t)(line + sizeof line - 2 - p), fmt, ap);
    va_end(ap);
    *p++ = '\n';
    *p = 0;
    OutputDebugStringA(line);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    DWORD n;
    if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, line, (DWORD)(p - line), &n, NULL);
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

/* ---------------------------------------------------------- the mode */
static int g_mode = -1;   /* 0 FORWARD, 1 metalfx */
static int metalfx_mode(void)
{
    if (g_mode < 0) {
        char v[32];
        DWORD n = GetEnvironmentVariableA("STEAMARM_WIN_UPSCALER", v, sizeof v);
        g_mode = n > 0 && n < sizeof v && seq(v, "metalfx");
        say("%s", g_mode ? "MetalFX temporal mode (STEAMARM_WIN_UPSCALER=metalfx): upscale contexts go to MetalFX, the rest to the game's own DLL"
                         : "FORWARD mode (STEAMARM_WIN_UPSCALER not metalfx): every call goes to the game's own DLL");
    }
    return g_mode;
}

/* ---------------------------------------------------- the original DLL */
static CRITICAL_SECTION g_lock;
static struct {
    int state;   /* 0 untried, 1 loaded, 2 unavailable */
    HMODULE mod;
    PfnCreate create; PfnDestroy destroy; PfnConfigure configure; PfnQuery query; PfnDispatch dispatch;
    char path[520];
} g_orig;
/* A global debug configure seen before the original was loaded: replayed. */
static struct ffxConfigureDescGlobalDebug1 g_pending_debug;
static int g_pending_debug_set;

static uint64_t fnv1a(uint64_t h, const unsigned char *p, size_t n)
{
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}

/* The hash of a file's contents (FNV-1a 64); 0 on failure or when the
 * file is a Wine builtin (this DLL itself, found where a game copy should
 * be). */
static uint64_t hash_file(const WCHAR *path, uint64_t *size_out)
{
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    uint64_t h = 0xcbf29ce484222325ull, total = 0;
    unsigned char *buf = xalloc(1 << 20);
    int first = 1, builtin = 0;
    DWORD n;
    while (buf && ReadFile(f, buf, 1 << 20, &n, NULL) && n) {
        if (first) {
            static const char sig[] = "Wine builtin DLL";
            builtin = n > 0x40 + sizeof sig && !__builtin_memcmp(buf + 0x40, sig, sizeof sig);
            first = 0;
        }
        h = fnv1a(h, buf, n);
        total += n;
    }
    xfree(buf);
    CloseHandle(f);
    if (size_out) *size_out = total;
    return builtin || !total ? 0 : h;
}

static int wcat(WCHAR *d, size_t cap, const WCHAR *s)
{
    size_t n = wlen(d), m = wlen(s);
    if (n + m + 1 > cap) return 0;
    memcpy(d + n, s, (m + 1) * sizeof(WCHAR));
    return 1;
}

static void to_utf8(const WCHAR *w, char *out, int cap)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL)) { out[0] = '?'; out[1] = 0; }
}

static int orig_load_locked(void)
{
    if (g_orig.state) return g_orig.state == 1;
    g_orig.state = 2;
    WCHAR path[520], dest[520], hex[20];
    if (!GetModuleFileNameW(g_self, path, 520)) { say("original DLL: GetModuleFileNameW failed (%u)", (unsigned)GetLastError()); return 0; }
    to_utf8(path, g_orig.path, sizeof g_orig.path);
    uint64_t size = 0, h = hash_file(path, &size);
    if (!h) { say("original DLL: %s is not a game copy (missing, empty or this builtin): no fallback", g_orig.path); return 0; }
    /* %LOCALAPPDATA%\SteamARM\ffx\<hash>.dll */
    dest[0] = 0;
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dest, 520) || !dest[0]) {
        if (!GetTempPathW(520, dest)) return 0;
        wcat(dest, 520, L"SteamARM-ffx");
    } else {
        wcat(dest, 520, L"\\SteamARM");
        CreateDirectoryW(dest, NULL);
        wcat(dest, 520, L"\\ffx");
    }
    CreateDirectoryW(dest, NULL);
    for (int i = 0; i < 16; i++) hex[i] = (WCHAR)"0123456789abcdef"[(h >> (60 - 4 * i)) & 15];
    hex[16] = 0;
    wcat(dest, 520, L"\\");
    wcat(dest, 520, hex);
    wcat(dest, 520, L".dll");
    WIN32_FILE_ATTRIBUTE_DATA fa;
    int have = GetFileAttributesExW(dest, GetFileExInfoStandard, &fa) &&
               (((uint64_t)fa.nFileSizeHigh << 32) | fa.nFileSizeLow) == size;
    if (!have && !CopyFileW(path, dest, FALSE)) {
        say("original DLL: copying %s failed (%u): no fallback", g_orig.path, (unsigned)GetLastError());
        return 0;
    }
    HMODULE m = LoadLibraryW(dest);
    if (!m) {
        char d8[520];
        to_utf8(dest, d8, sizeof d8);
        say("original DLL: LoadLibraryW(%s) failed (%u): no fallback", d8, (unsigned)GetLastError());
        return 0;
    }
    g_orig.create = (PfnCreate)(void *)GetProcAddress(m, "ffxCreateContext");
    g_orig.destroy = (PfnDestroy)(void *)GetProcAddress(m, "ffxDestroyContext");
    g_orig.configure = (PfnConfigure)(void *)GetProcAddress(m, "ffxConfigure");
    g_orig.query = (PfnQuery)(void *)GetProcAddress(m, "ffxQuery");
    g_orig.dispatch = (PfnDispatch)(void *)GetProcAddress(m, "ffxDispatch");
    if (!g_orig.create || !g_orig.destroy || !g_orig.configure || !g_orig.query || !g_orig.dispatch) {
        say("original DLL: %s lacks the FFX API exports: no fallback", g_orig.path);
        FreeLibrary(m);
        return 0;
    }
    g_orig.mod = m;
    g_orig.state = 1;
    {
        char d8[520];
        to_utf8(dest, d8, sizeof d8);
        say("original DLL: %s (%llu bytes) %s %s", g_orig.path, (unsigned long long)size, have ? "already at" : "copied to", d8);
    }
    if (g_pending_debug_set) g_orig.configure(NULL, &g_pending_debug.header);
    return 1;
}

static int orig_ok(void)
{
    EnterCriticalSection(&g_lock);
    int r = orig_load_locked();
    LeaveCriticalSection(&g_lock);
    return r;
}

/* ------------------------------------------------------- AMD's formulas */
/* ffx_fsr3upscaler.cpp's halton(), as AMD's DLL computes it: a game that
 * asks AMD's DLL and this one must get the same jitter, bit for bit. AMD's
 * build turns "f /= (float)base" into a multiplication by the reciprocal
 * (MEASURED against amd_fidelityfx_dx12.dll 1.1.4: index 4 base 3 gives
 * 0xbd638e30, the division 0xbd638e38; tests/win/d3d12_ffx.c compares 40
 * offsets), and build.sh's -ffp-contract=off keeps the sum unfused. */
static float halton(int32_t index, int32_t base)
{
    const float inv = 1.0f / (float)base;
    float f = 1.0f, result = 0.0f;
    for (int32_t i = index; i > 0;) {
        f = f * inv;
        result = result + f * (float)(i % base);
        i = (int32_t)(uint32_t)__builtin_floorf((float)i / (float)base);
    }
    return result;
}
static float ratio_of_mode(uint32_t mode)
{
    switch (mode) { case 0: return 1.0f; case 1: return 1.5f; case 2: return 1.7f; case 3: return 2.0f; case 4: return 3.0f; default: return 0.0f; }
}
/* ffxFsr3UpscalerGetJitterPhaseCount: int32_t(8.0f * pow(float(displayWidth)
 * / renderWidth, 2.0f)), truncated. The square must come out as AMD's DLL
 * computes it, or the truncation can differ by one. AMD's SDK 1.1.4 DLL
 * imports no powf (its api-ms-win-crt-math imports are log2f, sinf, exp2f,
 * tanf, roundf, atanf: MSVC folded pow(x, 2.0f) into a multiplication),
 * MEASURED identical to r * r for 1280->1920, 1707->2560, 1280->3840 and
 * 960->1920 (tests/win/d3d12_ffx.c). AMD's SDK 2.3.0 upscaler DLL does
 * import powf, so the SDK 2.x build calls the same powf it would call under
 * Wine (ucrtbase's, the DLL behind api-ms-win-crt-math-l1-1-0), and the
 * multiplication only if that is not loadable. */
typedef float (*PfnPowf)(float, float);
static PfnPowf g_powf;
static int g_powf_tried;
static float square_as_amd(float r)
{
    if (FFX_SDK2) {
        if (!g_powf_tried) {
            HMODULE m = LoadLibraryA("ucrtbase.dll");
            g_powf = m ? (PfnPowf)(void *)GetProcAddress(m, "powf") : NULL;
            g_powf_tried = 1;
        }
        if (g_powf) return g_powf(r, 2.0f);
    }
    return r * r;
}
static int32_t phase_count(uint32_t render_w, uint32_t display_w)
{
    float r = (float)display_w / (float)render_w;
    return (int32_t)(8.0f * square_as_amd(r));
}

/* ----------------------------------------------------------- contexts */
enum { BK_DX12 = 1, BK_VK = 2 };
enum { MODE_METALFX = 1, MODE_FALLBACK = 2 };

struct vkfn {
    PFN_vkCreateCommandPool CreateCommandPool;
    PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
    PFN_vkBeginCommandBuffer BeginCommandBuffer;
    PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdUpdateBuffer CmdUpdateBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier;
    PFN_vkCreateBuffer CreateBuffer;
    PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements;
    PFN_vkAllocateMemory AllocateMemory;
    PFN_vkFreeMemory FreeMemory;
    PFN_vkBindBufferMemory BindBufferMemory;
};

/* vkd3d-proton's interop interface (vkd3d_device_vkd3d_ext.idl), the part used. */
static const GUID IID_ID3D12DXVKInteropDevice1 = { 0x902d8115, 0x59eb, 0x4406, { 0x95, 0x18, 0xfe, 0x00, 0xf9, 0x91, 0xee, 0x65 } };
static const GUID IID_ID3D12Resource_local = { 0x696442be, 0xa72e, 0x4059, { 0xbc, 0x79, 0x5b, 0x5c, 0x98, 0x04, 0x0f, 0xad } };
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

/* The creation descriptors, copied: the game's may be stack variables
 * (AMD's sample's are), and the original context is created from them
 * only when a frame is refused. Types not listed here are not copied: a
 * chain with one of them is forwarded at creation. */
struct desc_copy {
    struct ffxCreateContextDescUpscale up;
    struct ffxCreateBackendDX12Desc dx;
    struct ffxCreateBackendVKDesc vk;
    struct ffxCreateBackendDX12AllocationCallbacksDesc ac;   /* SDK 2.x */
    struct ffxOverrideVersion ov;
    struct ffxCreateContextDescUpscaleVersion ver;
    int has_dx, has_vk, has_ac, has_ov, has_ver;
    ffxAllocationCallbacks cb;
    int has_cb;
};

struct sa_ctx {
    uint32_t magic;            /* 'SAMF' */
    int backend, mode;
    uint64_t id;
    struct desc_copy desc;
    /* the Vulkan side */
    VkDevice vdev;
    VkPhysicalDevice vpd;
    struct vkfn fn;
    VkCommandPool pool;
    VkCommandBuffer own_cb;    /* of the pool, never submitted: PROBE and DESTROY */
    VkBuffer carrier;          /* a 4 KiB buffer the commands are "written" to */
    VkDeviceSize carrier_off;
    VkDeviceMemory carrier_mem; /* VK backend: its memory */
    /* D3D12 */
    ID3D12Device *dev;
    ID3D12DXVKInteropDevice1 *ix;
    ID3D12Resource *carrier_res;
    /* what PROBE said */
    uint32_t caps; float max_scale;
    /* the original's context, once needed */
    ffxContext orig_ctx;
    /* bookkeeping */
    uint32_t frames, logged, status_last;
    int probe_tried, tc_said, nofall_said;
};
#define SA_CTX_MAGIC 0x464D4153u

enum { MAX_CTX = 64 };
static struct sa_ctx *g_ctx[MAX_CTX];

static struct sa_ctx *ctx_find(ffxContext *context)
{
    if (!context || !*context) return NULL;
    void *p = *context;
    struct sa_ctx *r = NULL;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_CTX; i++) if (g_ctx[i] == p) { r = g_ctx[i]; break; }
    LeaveCriticalSection(&g_lock);
    return r;
}
static int ctx_register(struct sa_ctx *c)
{
    int ok = 0;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_CTX; i++) if (!g_ctx[i]) { g_ctx[i] = c; ok = 1; break; }
    LeaveCriticalSection(&g_lock);
    return ok;
}
static void ctx_unregister(struct sa_ctx *c)
{
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < MAX_CTX; i++) if (g_ctx[i] == c) g_ctx[i] = NULL;
    LeaveCriticalSection(&g_lock);
}

/* --------------------------------------------------- the Vulkan side */
static HMODULE g_vk1;
static PFN_vkGetDeviceProcAddr g_gdpa;
static PFN_vkGetPhysicalDeviceQueueFamilyProperties g_qfp;
static PFN_vkGetPhysicalDeviceMemoryProperties g_memprops;

static int vk1_load(void)
{
    if (g_vk1) return 1;
    HMODULE m = LoadLibraryA("vulkan-1.dll");
    if (!m) { say("vulkan-1.dll: not loadable (%u)", (unsigned)GetLastError()); return 0; }
    g_gdpa = (PFN_vkGetDeviceProcAddr)(void *)GetProcAddress(m, "vkGetDeviceProcAddr");
    g_qfp = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)(void *)GetProcAddress(m, "vkGetPhysicalDeviceQueueFamilyProperties");
    g_memprops = (PFN_vkGetPhysicalDeviceMemoryProperties)(void *)GetProcAddress(m, "vkGetPhysicalDeviceMemoryProperties");
    if (!g_gdpa || !g_qfp || !g_memprops) { say("vulkan-1.dll: exports missing"); return 0; }
    g_vk1 = m;
    return 1;
}

static int vkfn_load(struct sa_ctx *c, PFN_vkGetDeviceProcAddr gdpa)
{
#define F(name) c->fn.name = (PFN_vk##name)gdpa(c->vdev, "vk" #name); if (!c->fn.name) { say("vkGetDeviceProcAddr: no vk" #name); return 0; }
    F(CreateCommandPool) F(DestroyCommandPool) F(AllocateCommandBuffers) F(BeginCommandBuffer) F(EndCommandBuffer)
    F(CmdUpdateBuffer) F(CmdPipelineBarrier) F(CreateBuffer) F(DestroyBuffer) F(GetBufferMemoryRequirements)
    F(AllocateMemory) F(FreeMemory) F(BindBufferMemory)
#undef F
    return 1;
}

static uint32_t pick_family(VkPhysicalDevice pd)
{
    uint32_t n = 0;
    g_qfp(pd, &n, NULL);
    if (!n) return 0;
    VkQueueFamilyProperties *p = xalloc(n * sizeof *p);
    if (!p) return 0;
    g_qfp(pd, &n, p);
    uint32_t fam = 0;
    for (uint32_t i = 0; i < n; i++) if (p[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { fam = i; break; }
    xfree(p);
    return fam;
}

static int own_pool(struct sa_ctx *c)
{
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_TRANSIENT_BIT | VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    pick_family(c->vpd) };
    if (c->fn.CreateCommandPool(c->vdev, &pci, NULL, &c->pool) != VK_SUCCESS) { say("vkCreateCommandPool failed"); return 0; }
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, c->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    if (c->fn.AllocateCommandBuffers(c->vdev, &ai, &c->own_cb) != VK_SUCCESS) { say("vkAllocateCommandBuffers failed"); return 0; }
    return 1;
}

/* VK backend: the carrier buffer, with memory of its own. */
static int vk_carrier(struct sa_ctx *c)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (c->fn.CreateBuffer(c->vdev, &bi, NULL, &c->carrier) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    c->fn.GetBufferMemoryRequirements(c->vdev, c->carrier, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    g_memprops(c->vpd, &mp);
    uint32_t type = ~0u;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type = i; break; }
    for (uint32_t i = 0; type == ~0u && i < mp.memoryTypeCount; i++) if (mr.memoryTypeBits & (1u << i)) type = i;
    if (type == ~0u) return 0;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, type };
    if (c->fn.AllocateMemory(c->vdev, &mai, NULL, &c->carrier_mem) != VK_SUCCESS) return 0;
    if (c->fn.BindBufferMemory(c->vdev, c->carrier, c->carrier_mem, 0) != VK_SUCCESS) return 0;
    return 1;
}

/* D3D12 backend: the carrier is a committed buffer; vkd3d says its VkBuffer. */
static int dx_carrier(struct sa_ctx *c)
{
    D3D12_HEAP_PROPERTIES hp = { D3D12_HEAP_TYPE_DEFAULT, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
    D3D12_RESOURCE_DESC d = { D3D12_RESOURCE_DIMENSION_BUFFER, 0, 4096, 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 },
                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_FLAG_NONE };
    HRESULT hr = ID3D12Device_CreateCommittedResource(c->dev, &hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COMMON, NULL,
                                                      &IID_ID3D12Resource_local, (void **)&c->carrier_res);
    if (FAILED(hr)) { say("carrier buffer: CreateCommittedResource 0x%x", (unsigned)hr); return 0; }
    UINT64 buf = 0, off = 0;
    VkFormat f;
    hr = c->ix->lpVtbl->GetVulkanResourceInfo1(c->ix, c->carrier_res, &buf, &off, &f);
    if (FAILED(hr) || !buf) { say("carrier buffer: GetVulkanResourceInfo1 0x%x", (unsigned)hr); return 0; }
    c->carrier = (VkBuffer)buf;
    c->carrier_off = off;
    return 1;
}

static void vk_barrier(struct sa_ctx *c, VkCommandBuffer cb)
{
    VkMemoryBarrier b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT,
                          VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    c->fn.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL, 0, NULL);
}

static struct sa_mfxt_cmd base_cmd(uint32_t op, uint64_t ctx)
{
    struct sa_mfxt_cmd m;
    memset(&m, 0, sizeof m);
    m.magic = SA_MFXT_MAGIC;
    m.version = SA_MFXT_VERSION;
    m.size = sizeof m;
    m.op = op;
    m.status = SA_ST_UNSEEN;
    m.context = ctx;
    m.mv_scale_x = m.mv_scale_y = 1.0f;
    m.pre_exposure = 1.0f;
    return m;
}

/* PROBE or DESTROY, in the context's own command buffer: the status (and
 * PROBE's caps) comes back in the same call. */
static uint32_t own_command(struct sa_ctx *c, struct sa_mfxt_cmd *m)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    if (c->fn.BeginCommandBuffer(c->own_cb, &bi) != VK_SUCCESS) return SA_ST_BAD_ARGS;
    c->fn.CmdUpdateBuffer(c->own_cb, c->carrier, c->carrier_off, sizeof *m, m);
    c->fn.EndCommandBuffer(c->own_cb);
    return m->status;
}

/* ------------------------------------------------ descriptor handling */
static const ffxApiHeader *find_desc(const ffxApiHeader *h, uint64_t type)
{
    for (; h; h = h->pNext) if (h->type == type) return h;
    return NULL;
}

static uint64_t override_of(const ffxApiHeader *h)
{
    const struct ffxOverrideVersion *o = (const struct ffxOverrideVersion *)find_desc(h, T_OVERRIDE_VERSION);
    return o ? o->versionId : 0;
}

/* The chain of an upscale creation, copied; 0 with the type not taken.
 * Type 3 is the Vulkan backend in SDK 1.1.x and the DX12 allocation
 * callbacks in SDK 2.x (T_BACKEND_*): the DLL's name says which. */
static int copy_chain(const ffxApiHeader *h, struct desc_copy *d, uint64_t *bad)
{
    memset(d, 0, sizeof *d);
    memcpy(&d->up, h, sizeof d->up);
    for (h = h->pNext; h; h = h->pNext) {
        uint64_t t = h->type;
        if (t == T_BACKEND_DX12) { memcpy(&d->dx, h, sizeof d->dx); d->has_dx = 1; }
        else if (t == 3u && FFX_SDK2) { memcpy(&d->ac, h, sizeof d->ac); d->has_ac = 1; }
        else if ((t == T_BACKEND_VK_SDK1 && !FFX_SDK2) || t == T_BACKEND_VK_SDK2) { memcpy(&d->vk, h, sizeof d->vk); d->has_vk = 1; }
        else if (t == T_OVERRIDE_VERSION) { memcpy(&d->ov, h, sizeof d->ov); d->has_ov = 1; }
        else if (t == T_CREATE_UPSCALE_VERSION) { memcpy(&d->ver, h, sizeof d->ver); d->has_ver = 1; }
        else { *bad = t; return 0; }
    }
    return 1;
}

/* The copied chain linked again, for the original's ffxCreateContext: the
 * allocation callbacks pass through; an override naming this DLL's version
 * is left out (AMD's DLL has no provider of that id). */
static ffxApiHeader *link_chain(struct desc_copy *d)
{
    ffxApiHeader *tail = &d->up.header;
    tail->pNext = NULL;
    if (d->has_dx) { tail->pNext = &d->dx.header; tail = tail->pNext; tail->pNext = NULL; }
    if (d->has_ac) { tail->pNext = &d->ac.header; tail = tail->pNext; tail->pNext = NULL; }
    if (d->has_vk) { tail->pNext = &d->vk.header; tail = tail->pNext; tail->pNext = NULL; }
    if (d->has_ver) { tail->pNext = &d->ver.header; tail = tail->pNext; tail->pNext = NULL; }
    if (d->has_ov && d->ov.versionId != SA_FFX_VERSION_ID) { tail->pNext = &d->ov.header; tail = tail->pNext; tail->pNext = NULL; }
    return &d->up.header;
}

/* The game's own chain with an ffxOverrideVersion naming THIS DLL's version
 * unlinked, for the duration of a call into the game's DLL: AMD's
 * GetffxProvider (ffx_provider.cpp, SDK 1.1.x and 2.x alike) returns no
 * provider for an override id it does not have, and ffxCreateContext /
 * ffxQuery answer FFX_API_RETURN_NO_PROVIDER -- a game that chose this
 * DLL's version (first in the version list) and hit any forward path would
 * lose FSR altogether. Without the override AMD's DLL picks its own
 * default. The override is never the chain's head (the head is the create
 * or query descriptor). Returns the node unlinked (NULL: none) and its
 * predecessor, for relink_override(). */
static ffxApiHeader *unlink_our_override(ffxApiHeader *h, ffxApiHeader **pred)
{
    for (ffxApiHeader *p = h; p && p->pNext; p = p->pNext) {
        ffxApiHeader *n = p->pNext;
        if (n->type == T_OVERRIDE_VERSION && ((struct ffxOverrideVersion *)n)->versionId == SA_FFX_VERSION_ID) {
            p->pNext = n->pNext;
            *pred = p;
            return n;
        }
    }
    *pred = NULL;
    return NULL;
}
static void relink_override(ffxApiHeader *pred, ffxApiHeader *ov)
{
    if (ov) pred->pNext = ov;
}

/* FFX surface format -> VkFormat (vk/ffx_api_vk.h's table, the other way);
 * 0 = unknown, the driver then uses the image's own. */
static VkFormat vk_format_of(uint32_t f)
{
    switch (f) {
    case 3: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 2: return VK_FORMAT_R32G32B32A32_UINT;
    case 4: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 5: return VK_FORMAT_R32G32B32_SFLOAT;
    case 6: return VK_FORMAT_R32G32_SFLOAT;
    case 7: return VK_FORMAT_R8_UINT;
    case 8: return VK_FORMAT_R32_UINT;
    case 10: return VK_FORMAT_R8G8B8A8_UNORM;
    case 11: return VK_FORMAT_R8G8B8A8_SNORM;
    case 12: return VK_FORMAT_R8G8B8A8_SRGB;
    case 14: return VK_FORMAT_B8G8R8A8_UNORM;
    case 15: return VK_FORMAT_B8G8R8A8_SRGB;
    case 16: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 17: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 18: return VK_FORMAT_R16G16_SFLOAT;
    case 19: return VK_FORMAT_R16G16_UINT;
    case 20: return VK_FORMAT_R16G16_SINT;
    case 21: return VK_FORMAT_R16_SFLOAT;
    case 22: return VK_FORMAT_R16_UINT;
    case 23: return VK_FORMAT_R16_UNORM;
    case 24: return VK_FORMAT_R16_SNORM;
    case 25: return VK_FORMAT_R8_UNORM;
    case 26: return VK_FORMAT_R8G8_UNORM;
    case 27: return VK_FORMAT_R8G8_UINT;
    case 28: return VK_FORMAT_R32_SFLOAT;
    case 29: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/* ------------------------------------------------------------ create */
static void ctx_free(struct sa_ctx *c)
{
    if (c->pool && c->fn.DestroyCommandPool) c->fn.DestroyCommandPool(c->vdev, c->pool, NULL);
    if (c->backend == BK_VK) {
        if (c->carrier && c->fn.DestroyBuffer) c->fn.DestroyBuffer(c->vdev, c->carrier, NULL);
        if (c->carrier_mem && c->fn.FreeMemory) c->fn.FreeMemory(c->vdev, c->carrier_mem, NULL);
    }
    if (c->carrier_res) ID3D12Resource_Release(c->carrier_res);
    if (c->ix) c->ix->lpVtbl->Release(c->ix);
    xfree(c);
}

static ffxReturnCode_t forward_create(ffxContext *context, ffxApiHeader *desc, const ffxAllocationCallbacks *cb, const char *why)
{
    if (!orig_ok()) { say("create 0x%llx: %s, and no original DLL: NO_PROVIDER", (unsigned long long)desc->type, why); return FFX_NO_PROVIDER; }
    ffxApiHeader *pred, *ov = unlink_our_override(desc, &pred);
    ffxReturnCode_t rc = g_orig.create(context, desc, cb);
    relink_override(pred, ov);
    say("create 0x%llx: %s: the game's DLL (rc %u, ctx %p%s)", (unsigned long long)desc->type, why, rc, context ? *context : NULL,
        ov ? "; the override naming this DLL's version left out" : "");
    return rc;
}

/* A query for the game's DLL (its override naming this DLL left out). */
static ffxReturnCode_t forward_query(ffxContext *context, ffxApiHeader *desc)
{
    if (!orig_ok()) return FFX_NO_PROVIDER;
    ffxApiHeader *pred, *ov = unlink_our_override(desc, &pred);
    ffxReturnCode_t rc = g_orig.query(context, desc);
    relink_override(pred, ov);
    return rc;
}

static ffxReturnCode_t create_upscale(ffxContext *context, ffxApiHeader *desc, const ffxAllocationCallbacks *cb)
{
    struct desc_copy d;
    uint64_t bad = 0;
    if (!copy_chain(desc, &d, &bad)) {
        char why[64];
        char *p = put_str(why, why + 60, "descriptor 0x");
        p = put_u(p, why + 60, bad, 16, 0);
        *p = 0;
        return forward_create(context, desc, cb, why);
    }
    if (d.has_ov && d.ov.versionId && d.ov.versionId != SA_FFX_VERSION_ID) return forward_create(context, desc, cb, "a version override naming AMD's");
    if (d.has_dx == d.has_vk) return forward_create(context, desc, cb, d.has_dx ? "two backends" : "no backend descriptor");
    if (d.has_ac && !d.has_dx) return forward_create(context, desc, cb, "allocation callbacks without the DX12 backend");
    const struct ffxCreateContextDescUpscale *up = (const struct ffxCreateContextDescUpscale *)desc;
    /* FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE: the colour input holds
     * gamma-encoded (perceptual) values. The channel (shim/mfx_temporal.h)
     * has no colour-space field and MetalFX's temporal scaler takes linear
     * colour: feeding it encoded values as linear would upscale the wrong
     * quantity (blending and exposure in the wrong space). The game's own
     * upscaler takes such a context; the choice is made here, at creation,
     * so the game never sees a context switch. */
    if (up->flags & UP_NON_LINEAR) return forward_create(context, desc, cb, "a non-linear (gamma-encoded) colour space, which the channel cannot tell MetalFX");
    if (!vk1_load()) return forward_create(context, desc, cb, "no vulkan-1.dll");

    struct sa_ctx *c = xalloc(sizeof *c);
    if (!c) return FFX_ERROR_MEMORY;
    c->magic = SA_CTX_MAGIC;
    c->desc = d;
    if (cb) { c->desc.cb = *cb; c->desc.has_cb = 1; }
    /* Unique across all live bridge contexts in this process, including
     * the other FidelityFX DLLs and both NGX names sharing this driver. */
    c->id = (uint64_t)(uintptr_t)c;
    const char *why = NULL;
    if (d.has_dx) {
        c->backend = BK_DX12;
        c->dev = d.dx.device;
        if (!c->dev) why = "a null ID3D12Device";
        else if (FAILED(ID3D12Device_QueryInterface(c->dev, &IID_ID3D12DXVKInteropDevice1, (void **)&c->ix)) || !c->ix)
            why = "no ID3D12DXVKInteropDevice1 (not vkd3d-proton)";
        else {
            VkInstance vinst;
            if (FAILED(c->ix->lpVtbl->GetVulkanHandles(c->ix, &vinst, &c->vpd, &c->vdev)) || !c->vdev) why = "GetVulkanHandles failed";
            else if (!vkfn_load(c, g_gdpa)) why = "Vulkan device functions missing";
            else if (!dx_carrier(c)) why = "no carrier buffer";
        }
    } else {
        c->backend = BK_VK;
        c->vdev = d.vk.vkDevice;
        c->vpd = d.vk.vkPhysicalDevice;
        if (!c->vdev || !c->vpd) why = "a null VkDevice or VkPhysicalDevice";
        else if (!vkfn_load(c, d.vk.vkDeviceProcAddr ? d.vk.vkDeviceProcAddr : g_gdpa)) why = "Vulkan device functions missing";
        else if (!vk_carrier(c)) why = "no carrier buffer";
    }
    if (!why && !own_pool(c)) why = "no command pool";
    if (!why) {
        struct sa_mfxt_cmd p = base_cmd(SA_OP_PROBE, c->id);
        p.render_w = up->maxRenderSize.width; p.render_h = up->maxRenderSize.height;
        p.upscale_w = up->maxUpscaleSize.width; p.upscale_h = up->maxUpscaleSize.height;
        uint32_t st = own_command(c, &p);
        c->caps = p.caps_out;
        c->max_scale = p.max_scale_out;
        if (st != SA_ST_OK) {
            static char buf[80];
            char *q = put_str(buf, buf + 78, "PROBE ");
            q = put_str(q, buf + 78, status_name(st));
            *q = 0;
            why = buf;
        } else if (!(c->caps & SA_MFXT_CAP_TEMPORAL)) why = "PROBE OK without the temporal cap";
        else if ((up->flags & UP_MV_DISPLAY) && !(c->caps & SA_MFXT_CAP_MV_DISPLAY)) why = "display-resolution motion vectors not in the caps";
        else if ((up->flags & UP_MV_JITTERED) && !(c->caps & SA_MFXT_CAP_MV_JITTERED)) why = "jittered motion vectors not in the caps";
    }
    if (why) {
        ctx_free(c);
        return forward_create(context, desc, cb, why);
    }
    if (!ctx_register(c)) { ctx_free(c); return forward_create(context, desc, cb, "the context table is full"); }
    c->mode = MODE_METALFX;
    *context = c;
    say("create upscale: %ux%u -> %ux%u flags 0x%x, %s backend: MetalFX temporal (ctx %llu, caps 0x%x, max scale %f)",
        up->maxRenderSize.width, up->maxRenderSize.height, up->maxUpscaleSize.width, up->maxUpscaleSize.height, up->flags,
        c->backend == BK_DX12 ? "D3D12" : "Vulkan", (unsigned long long)c->id, c->caps, (double)c->max_scale);
    return FFX_OK;
}

/* The original context, created now from the copied descriptors; the
 * context is the original's from here on. */
static int fall_back(struct sa_ctx *c, const char *why)
{
    if (c->mode == MODE_FALLBACK) return 1;
    if (!orig_ok()) {
        if (!c->nofall_said++) say("ctx %llu: %s, and no game DLL to fall back to", (unsigned long long)c->id, why);
        return 0;
    }
    ffxApiHeader *chain = link_chain(&c->desc);
    ffxReturnCode_t rc = g_orig.create(&c->orig_ctx, chain, c->desc.has_cb ? &c->desc.cb : NULL);
    if (rc != FFX_OK || !c->orig_ctx) {
        say("ctx %llu: %s, and the game's DLL refused its context (rc %u): the frame is lost", (unsigned long long)c->id, why, rc);
        c->orig_ctx = NULL;
        return 0;
    }
    c->mode = MODE_FALLBACK;
    say("ctx %llu: %s: the game's own upscaler from now on (its context %p)", (unsigned long long)c->id, why, c->orig_ctx);
    return 1;
}

/* ----------------------------------------------------------- dispatch */
static void tex_of(struct sa_ctx *c, const struct FfxApiResource *r, struct sa_mfxt_tex *t, int depth, int *bad)
{
    memset(t, 0, sizeof *t);
    if (!r->resource) return;
    if (c->backend == BK_DX12) {
        UINT64 img = 0, off = 0;
        VkFormat f = VK_FORMAT_UNDEFINED;
        if (FAILED(c->ix->lpVtbl->GetVulkanResourceInfo1(c->ix, (ID3D12Resource *)r->resource, &img, &off, &f)) || !img) { *bad = 1; return; }
        t->image = img;
        t->vk_format = depth ? 0 : (uint32_t)f;
    } else {
        t->image = (uint64_t)(uintptr_t)r->resource;
        t->vk_format = depth ? 0 : (uint32_t)vk_format_of(r->description.format);
    }
    t->w = r->description.width;
    t->h = r->description.height;
}

static int g_test_refuse = -1;
static int test_refuse(void)
{
    if (g_test_refuse < 0) {
        char v[8];
        g_test_refuse = GetEnvironmentVariableA("SA_FFX_TEST_REFUSE", v, sizeof v) > 0 && v[0] == '1';
        if (g_test_refuse) say("SA_FFX_TEST_REFUSE=1: every ENCODE is sent malformed, for the shim to refuse (tests only)");
    }
    return g_test_refuse;
}

/* A refused frame while the ENCODE's command buffer is open (D3D12: inside
 * Begin/EndVkCommandBufferInterop, cl the command list): the game's own
 * upscaler if there is one -- the interop is ended first, the original's
 * ffxDispatch records D3D12 commands of its own -- and the context is its
 * from now on; without a game DLL, report the dispatch failure. */
static ffxReturnCode_t refused_frame(struct sa_ctx *c, const char *why, ID3D12CommandList *cl,
                                     const struct ffxDispatchDescUpscale *d)
{
    if (cl) c->ix->lpVtbl->EndVkCommandBufferInterop(c->ix, cl);
    if (fall_back(c, why)) return g_orig.dispatch(&c->orig_ctx, &d->header);
    say("ctx %llu: refusing frame without an original upscaler; quality preserved (no bilinear substitute)",
        (unsigned long long)c->id);
    return FFX_RUNTIME_ERROR;
}

static ffxReturnCode_t dispatch_upscale(struct sa_ctx *c, const struct ffxDispatchDescUpscale *d)
{
    const struct ffxCreateContextDescUpscale *up = &c->desc.up;
    struct sa_mfxt_cmd m = base_cmd(SA_OP_ENCODE, c->id);
    int bad = 0;
    tex_of(c, &d->color, &m.color, 0, &bad);
    tex_of(c, &d->depth, &m.depth, 1, &bad);
    tex_of(c, &d->motionVectors, &m.motion, 0, &bad);
    tex_of(c, &d->exposure, &m.exposure, 0, &bad);
    tex_of(c, &d->reactive, &m.reactive, 0, &bad);
    tex_of(c, &d->output, &m.output, 0, &bad);
    m.render_w = d->renderSize.width; m.render_h = d->renderSize.height;
    m.upscale_w = d->upscaleSize.width ? d->upscaleSize.width : up->maxUpscaleSize.width;
    m.upscale_h = d->upscaleSize.height ? d->upscaleSize.height : up->maxUpscaleSize.height;
    m.jitter_x = d->jitterOffset.x; m.jitter_y = d->jitterOffset.y;
    m.mv_scale_x = d->motionVectorScale.x; m.mv_scale_y = d->motionVectorScale.y;
    m.pre_exposure = d->preExposure > 0.0f ? d->preExposure : 1.0f;
    m.sharpness = d->sharpness;
    m.flags = (d->reset ? SA_MFXT_RESET : 0) | ((up->flags & UP_DEPTH_INVERTED) ? SA_MFXT_DEPTH_INV : 0) |
              ((up->flags & UP_AUTO_EXPOSURE) ? SA_MFXT_AUTO_EXP : 0) | ((up->flags & UP_MV_DISPLAY) ? SA_MFXT_MV_DISPLAY : 0) |
              ((up->flags & UP_MV_JITTERED) ? SA_MFXT_MV_JITTERED : 0) | ((up->flags & UP_HDR) ? SA_MFXT_HDR : 0) |
              (d->enableSharpening ? SA_MFXT_SHARPEN : 0);
    /* Required colour/output resources for a temporal encode. */
    const char *why = NULL;
    if (bad) why = "a resource without a VkImage";
    else if (!d->commandList) why = "a null command list";
    else if (!m.color.image || !m.depth.image || !m.motion.image || !m.output.image) why = "colour, depth, motion vectors or output missing";
    else if (!m.render_w || !m.render_h || !m.upscale_w || !m.upscale_h) why = "a zero size";
    else if (m.upscale_w < m.render_w || m.upscale_h < m.render_h) why = "an upscale size below the render size";
    else if (c->max_scale > 0 && ((float)m.upscale_w / (float)m.render_w > c->max_scale + 1e-4f ||
                                  (float)m.upscale_h / (float)m.render_h > c->max_scale + 1e-4f)) why = "a scale above MetalFX's largest";
    /* Dispatch flags with no MetalFX equivalent. NON_LINEAR_COLOR_SRGB / PQ:
     * the colour input is gamma- or PQ-encoded for this frame and the
     * channel cannot say so (see create_upscale); DRAW_DEBUG_VIEW: FSR's
     * debug overlay. The game's own upscaler from here on. */
    else if (d->flags & (DF_NON_LINEAR_SRGB | DF_NON_LINEAR_PQ)) why = "a non-linear (sRGB or PQ) colour input this frame, which the channel cannot tell MetalFX";
    else if (d->flags & DF_DEBUG_VIEW) why = "FSR's debug view was asked for";
    else if (d->transparencyAndComposition.resource) why = "a transparency-and-composition mask without a MetalFX equivalent";
    else if (d->enableSharpening) why = "FSR sharpening without a MetalFX equivalent";
    if (why) {
        /* Before any command buffer is touched: the game's upscaler takes
         * the frame (the context is its from now on), or nothing can. */
        if (fall_back(c, why)) return g_orig.dispatch(&c->orig_ctx, &d->header);
        return FFX_ERROR_PARAMETER;
    }
    if (test_refuse()) m.size = sizeof m + 1;   /* the shim answers BAD_ARGS */

    /* The command buffer: vkd3d's behind the command list, or the game's. */
    VkCommandBuffer vcb;
    ID3D12CommandList *cl = NULL;
    if (c->backend == BK_DX12) {
        cl = (ID3D12CommandList *)d->commandList;
        vcb = VK_NULL_HANDLE;
        HRESULT hr = c->ix->lpVtbl->BeginVkCommandBufferInterop(c->ix, cl, &vcb);
        if (FAILED(hr) || !vcb) {
            say("ctx %llu: BeginVkCommandBufferInterop 0x%x", (unsigned long long)c->id, (unsigned)hr);
            if (fall_back(c, "no VkCommandBuffer behind the command list")) return g_orig.dispatch(&c->orig_ctx, &d->header);
            return FFX_RUNTIME_ERROR;
        }
    } else {
        vcb = (VkCommandBuffer)d->commandList;
    }
    c->frames++;
    if (why) {   /* known before ENCODE: report the refused dispatch */
        c->status_last = SA_ST_UNSEEN;
        return refused_frame(c, why, cl, d);
    }
    vk_barrier(c, vcb);
    c->fn.CmdUpdateBuffer(vcb, c->carrier, c->carrier_off, sizeof m, &m);
    uint32_t st = m.status;
    c->status_last = st;
    if (st != SA_ST_OK) {
        char buf[64];
        char *p = put_str(buf, buf + 62, "ENCODE ");
        p = put_str(p, buf + 62, status_name(st));
        *p = 0;
        return refused_frame(c, buf, cl, d);
    }
    vk_barrier(c, vcb);
    if (cl) c->ix->lpVtbl->EndVkCommandBufferInterop(c->ix, cl);
    if (debug_on() && c->logged < 3) {
        c->logged++;
        say("ctx %llu: ENCODE %ux%u -> %ux%u jitter (%f, %f) mv scale (%f, %f) flags 0x%x colour fmt %u depth fmt %u -> st=%s",
            (unsigned long long)c->id, m.render_w, m.render_h, m.upscale_w, m.upscale_h, (double)m.jitter_x, (double)m.jitter_y,
            (double)m.mv_scale_x, (double)m.mv_scale_y, m.flags, m.color.vk_format, m.depth.vk_format, status_name(st));
    }
    return FFX_OK;
}

/* ------------------------------------------------------------ exports */
ffxReturnCode_t ffxCreateContext(ffxContext *context, ffxApiHeader *desc, const ffxAllocationCallbacks *memCb)
{
    if (!context || !desc) return FFX_ERROR_PARAMETER;
    *context = NULL;
    if (!metalfx_mode() || (desc->type & FFX_EFFECT_MASK) != FFX_EFFECT_UPSCALE || desc->type != T_CREATE_UPSCALE)
        return forward_create(context, desc, memCb, metalfx_mode() ? "not an upscale context" : "FORWARD mode");
    return create_upscale(context, desc, memCb);
}

ffxReturnCode_t ffxDestroyContext(ffxContext *context, const ffxAllocationCallbacks *memCb)
{
    if (!context) return FFX_ERROR_PARAMETER;
    struct sa_ctx *c = ctx_find(context);
    if (!c) return orig_ok() ? g_orig.destroy(context, memCb) : FFX_NO_PROVIDER;
    ffxReturnCode_t rc = FFX_OK;
    int had_orig = c->orig_ctx != NULL;
    if (had_orig) rc = g_orig.destroy(&c->orig_ctx, memCb);
    struct sa_mfxt_cmd m = base_cmd(SA_OP_DESTROY, c->id);
    uint32_t st = own_command(c, &m);
    say("destroy ctx %llu after %u frames (%s; DESTROY %s%s)", (unsigned long long)c->id, c->frames,
        c->mode == MODE_FALLBACK ? "the game's upscaler" : "MetalFX", status_name(st), had_orig ? ", the game's context destroyed" : "");
    ctx_unregister(c);
    ctx_free(c);
    *context = NULL;
    return rc;
}

ffxReturnCode_t ffxConfigure(ffxContext *context, const ffxApiHeader *desc)
{
    if (!desc) return FFX_ERROR_PARAMETER;
    if (!context || !*context) {
        /* Global: the original's too, now or when it is loaded. */
        if (desc->type == T_CFG_GLOBALDEBUG1) {
            EnterCriticalSection(&g_lock);
            memcpy(&g_pending_debug, desc, sizeof g_pending_debug);
            g_pending_debug.header.pNext = NULL;
            g_pending_debug_set = 1;
            LeaveCriticalSection(&g_lock);
        }
        if (g_orig.state == 1 || (!metalfx_mode() && orig_ok())) return g_orig.configure(context, desc);
        return FFX_OK;
    }
    struct sa_ctx *c = ctx_find(context);
    if (!c) return orig_ok() ? g_orig.configure(context, desc) : FFX_NO_PROVIDER;
    if (c->mode == MODE_FALLBACK) return g_orig.configure(&c->orig_ctx, desc);
    switch (desc->type) {
    case T_CFG_GLOBALDEBUG1: case T_CFG_GLOBALDEBUG: case T_CFG_KEYVALUE: return FFX_OK;   /* FSR's tunables: no MetalFX equivalent */
    default: return FFX_UNKNOWN_DESCTYPE;
    }
}

/* Queries with AMD's formulas; 0 = not one of them. */
static int answer_formula(ffxApiHeader *h, ffxReturnCode_t *rc)
{
    switch (h->type) {
    case T_Q_RATIO: {
        struct ffxQueryRatio *q = (struct ffxQueryRatio *)h;
        if (q->pOutUpscaleRatio) *q->pOutUpscaleRatio = ratio_of_mode(q->qualityMode);
        *rc = FFX_OK;
        return 1;
    }
    case T_Q_RENDERRES: {
        struct ffxQueryRenderRes *q = (struct ffxQueryRenderRes *)h;
        if (q->qualityMode > 4) { *rc = FFX_RUNTIME_ERROR; return 1; }
        float r = ratio_of_mode(q->qualityMode);
        if (q->pOutRenderWidth) *q->pOutRenderWidth = (uint32_t)((float)q->displayWidth / r);
        if (q->pOutRenderHeight) *q->pOutRenderHeight = (uint32_t)((float)q->displayHeight / r);
        *rc = FFX_OK;
        return 1;
    }
    case T_Q_PHASECOUNT: {
        struct ffxQueryPhaseCount *q = (struct ffxQueryPhaseCount *)h;
        if (q->pOutPhaseCount) *q->pOutPhaseCount = phase_count(q->renderWidth, q->displayWidth);
        *rc = FFX_OK;
        return 1;
    }
    case T_Q_JITTER: {
        struct ffxQueryJitter *q = (struct ffxQueryJitter *)h;
        if (q->phaseCount <= 0 || !q->pOutX || !q->pOutY) { *rc = FFX_RUNTIME_ERROR; return 1; }
        *q->pOutX = halton((q->index % q->phaseCount) + 1, 2) - 0.5f;
        *q->pOutY = halton((q->index % q->phaseCount) + 1, 3) - 0.5f;
        *rc = FFX_OK;
        return 1;
    }
    case T_Q_RESREQ: {
        struct ffxQueryResReq *q = (struct ffxQueryResReq *)h;
        q->required_resources = 1 | 2 | 4;        /* colour, depth, motion vectors */
        q->optional_resources = 8 | 16;           /* exposure, reactive mask */
        *rc = FFX_OK;
        return 1;
    }
    default: return 0;
    }
}

ffxReturnCode_t ffxQuery(ffxContext *context, ffxApiHeader *desc)
{
    if (!desc) return FFX_ERROR_PARAMETER;
    if (desc->type == SA_FFX_QUERY_IDENT) {
        struct sa_ffx_query_ident *q = (struct sa_ffx_query_ident *)desc;
        static const char sig[] = "Wine builtin DLL";
        lstrcpynA(q->who, FFX_IDENT, sizeof q->who);
        q->marker = !__builtin_memcmp((const char *)g_self + 0x40, sig, sizeof sig);
        return FFX_OK;
    }
    ffxReturnCode_t rc;
    if (!context || !*context) {
        if (!metalfx_mode()) return forward_query(context, desc);
        if (desc->type == T_Q_GET_VERSIONS) {
            struct ffxQueryDescGetVersions *q = (struct ffxQueryDescGetVersions *)desc;
            if ((q->createDescType & FFX_EFFECT_MASK) != FFX_EFFECT_UPSCALE || !q->outputCount)
                return forward_query(context, desc);
            /* Ours first, then the original's. */
            uint64_t cap = *q->outputCount;
            int count_only = cap == 0 || (!q->versionIds && !q->versionNames);
            uint64_t theirs = 0;
            if (orig_ok()) {
                struct ffxQueryDescGetVersions t = *q;
                uint64_t n = count_only ? 0 : (cap > 1 ? cap - 1 : 0);
                t.outputCount = &n;
                t.versionIds = (!count_only && q->versionIds) ? q->versionIds + 1 : NULL;
                t.versionNames = (!count_only && q->versionNames) ? q->versionNames + 1 : NULL;
                if ((count_only || n) && g_orig.query(NULL, &t.header) == FFX_OK) theirs = n;
            }
            if (count_only) { *q->outputCount = 1 + theirs; return FFX_OK; }
            if (q->versionIds) q->versionIds[0] = SA_FFX_VERSION_ID;
            if (q->versionNames) q->versionNames[0] = SA_FFX_VERSION_NAME;
            *q->outputCount = 1 + theirs;
            return FFX_OK;
        }
        if ((desc->type & FFX_EFFECT_MASK) != FFX_EFFECT_UPSCALE) return forward_query(context, desc);
        uint64_t ov = override_of(desc);
        if (ov && ov != SA_FFX_VERSION_ID) return forward_query(context, desc);
        if (answer_formula(desc, &rc)) return rc;
        /* GPU memory (0x10008/9) and whatever else: the original's numbers
         * (an override naming this DLL left out of the chain for the call). */
        return forward_query(context, desc);
    }
    struct sa_ctx *c = ctx_find(context);
    if (!c) return forward_query(context, desc);
    if (c->mode == MODE_FALLBACK && desc->type != T_Q_PROVIDER_VERSION) return g_orig.query(&c->orig_ctx, desc);
    if (desc->type == T_Q_PROVIDER_VERSION) {
        struct ffxQueryGetProviderVersion *q = (struct ffxQueryGetProviderVersion *)desc;
        if (c->mode == MODE_FALLBACK) return g_orig.query(&c->orig_ctx, desc);
        q->versionId = SA_FFX_VERSION_ID;
        q->versionName = SA_FFX_VERSION_NAME;
        return FFX_OK;
    }
    if (answer_formula(desc, &rc)) return rc;
    if (desc->type == T_Q_GPUMEM) {
        struct ffxQueryGpuMem *q = (struct ffxQueryGpuMem *)desc;
        if (q->usage) { q->usage->totalUsageInBytes = 0; q->usage->aliasableUsageInBytes = 0; }
        return FFX_OK;
    }
    return FFX_UNKNOWN_DESCTYPE;
}

ffxReturnCode_t ffxDispatch(ffxContext *context, const ffxApiHeader *desc)
{
    if (!context || !desc) return FFX_ERROR_PARAMETER;
    struct sa_ctx *c = ctx_find(context);
    if (!c) return orig_ok() ? g_orig.dispatch(context, desc) : FFX_NO_PROVIDER;
    if (c->mode == MODE_FALLBACK) return g_orig.dispatch(&c->orig_ctx, desc);
    if (desc->type == T_DISPATCH_UPSCALE) return dispatch_upscale(c, (const struct ffxDispatchDescUpscale *)desc);
    /* The reactive-mask generator and anything else: FSR's own. */
    fall_back(c, desc->type == T_DISPATCH_REACTIVE ? "the reactive mask generator was asked for" : "a dispatch with no MetalFX equivalent");
    return c->mode == MODE_FALLBACK ? g_orig.dispatch(&c->orig_ctx, desc) : FFX_UNKNOWN_DESCTYPE;
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
