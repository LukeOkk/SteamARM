/*
 * SteamARM amd_fidelityfx_dx12.dll -- proof stub (phase 0 of the FSR 3.1 ->
 * MetalFX design). An x86_64 PE DLL with AMD's FidelityFX API exports at
 * AMD's ordinals (1 ffxConfigure, 2 ffxCreateContext, 3 ffxDestroyContext,
 * 4 ffxDispatch, 5 ffxQuery). Every call logs one line (stderr and
 * OutputDebugString) and returns FFX_API_RETURN_NO_PROVIDER (4).
 *
 * One query is answered: SA_FFX_QUERY_IDENT fills in who this DLL is, so a
 * caller (tests/win/ffx_load.c) can tell SteamARM's builtin from a game's
 * own copy. The same source builds both; FFX_IDENT names the build.
 *
 * Built by build.sh: KERNEL32 is the only import (no C runtime), and
 * wine-builtin-mark.py then writes the "Wine builtin DLL" marker at 0x40 and
 * sets Wine's prefer-native flag, as winebuild --builtin --prefer-native
 * would: Wine then takes it over a game's copy only under an override "b".
 */
#include <windows.h>
#include <stdint.h>

#ifndef FFX_IDENT
#define FFX_IDENT "steamarm-builtin"
#endif

typedef uint32_t ffxReturnCode_t;
typedef void *ffxContext;
typedef struct ffxApiHeader { uint64_t type; struct ffxApiHeader *pNext; } ffxApiHeader;

#define FFX_API_RETURN_NO_PROVIDER 4u

/* SteamARM's own query: not an AMD descriptor type (AMD uses 0x0000xxxx
 * general and 0x000Exxxx..0x0003xxxx effect ranges). */
#define SA_FFX_QUERY_IDENT 0x53410001ull
struct sa_ffx_query_ident {
    ffxApiHeader header;      /* type = SA_FFX_QUERY_IDENT */
    char who[64];             /* out: FFX_IDENT */
    uint32_t marker;          /* out: 1 if this image carries the Wine builtin marker */
};

static HMODULE self;

/* No C runtime and no USER32 (wsprintf): a few appenders. */
static char *put_str(char *p, const char *s) { while (*s) *p++ = *s++; return p; }
static char *put_hex(char *p, uint64_t v)
{
    char tmp[16];
    int n = 0;
    p = put_str(p, "0x");
    do { tmp[n++] = "0123456789abcdef"[v & 15]; v >>= 4; } while (v);
    while (n) *p++ = tmp[--n];
    return p;
}

static void say(const char *fn, const void *ctx, const ffxApiHeader *h, ffxReturnCode_t rc)
{
    char line[256], *p = line;
    DWORD n;
    p = put_str(p, "[ffx-stub " FFX_IDENT "] ");
    p = put_str(p, fn);
    p = put_str(p, "(ctx=");
    p = put_hex(p, (uintptr_t)ctx);
    if (h) { p = put_str(p, ", type="); p = put_hex(p, h->type); }
    p = put_str(p, ") -> ");
    *p++ = (char)('0' + rc % 10);
    *p++ = '\n';
    *p = 0;
    OutputDebugStringA(line);
    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err && err != INVALID_HANDLE_VALUE) WriteFile(err, line, (DWORD)(p - line), &n, NULL);
}

static uint32_t has_builtin_marker(void)
{
    static const char sig[] = "Wine builtin DLL";
    const char *p = (const char *)self + 0x40;   /* the image's own headers */
    for (unsigned i = 0; i < sizeof(sig); i++)
        if (p[i] != sig[i]) return 0;
    return 1;
}

ffxReturnCode_t ffxConfigure(ffxContext *context, const ffxApiHeader *desc)
{
    say("ffxConfigure", context, desc, FFX_API_RETURN_NO_PROVIDER);
    return FFX_API_RETURN_NO_PROVIDER;
}

ffxReturnCode_t ffxCreateContext(ffxContext *context, ffxApiHeader *desc, const void *memCb)
{
    (void)memCb;
    say("ffxCreateContext", context, desc, FFX_API_RETURN_NO_PROVIDER);
    return FFX_API_RETURN_NO_PROVIDER;
}

ffxReturnCode_t ffxDestroyContext(ffxContext *context, const void *memCb)
{
    (void)memCb;
    say("ffxDestroyContext", context, NULL, FFX_API_RETURN_NO_PROVIDER);
    return FFX_API_RETURN_NO_PROVIDER;
}

ffxReturnCode_t ffxDispatch(ffxContext *context, const ffxApiHeader *desc)
{
    say("ffxDispatch", context, desc, FFX_API_RETURN_NO_PROVIDER);
    return FFX_API_RETURN_NO_PROVIDER;
}

ffxReturnCode_t ffxQuery(ffxContext *context, ffxApiHeader *desc)
{
    if (desc && desc->type == SA_FFX_QUERY_IDENT) {
        struct sa_ffx_query_ident *q = (struct sa_ffx_query_ident *)desc;
        lstrcpynA(q->who, FFX_IDENT, sizeof(q->who));
        q->marker = has_builtin_marker();
    }
    say("ffxQuery", context, desc, FFX_API_RETURN_NO_PROVIDER);
    return FFX_API_RETURN_NO_PROVIDER;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
    (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
