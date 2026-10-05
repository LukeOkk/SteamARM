/*
 * Which amd_fidelityfx_dx12.dll does Wine/Proton give a game? Proof 2 of
 * the FSR 3.1 -> MetalFX design (tools/ffx-metalfx, tests/win/run_ffx_load.sh).
 *
 * Put next to this exe a "game" amd_fidelityfx_dx12.dll (the same stub built
 * with FFX_IDENT=game-native and FFX_MARK=0); SteamARM's builtin comes from
 * WINEDLLPATH. The program:
 *   1. loads amd_fidelityfx_dx12.dll -- by bare name (mode "name", what
 *      AMD's SDK and games do) or by its full path next to the exe ("path");
 *   2. calls ffxQuery (by name and by ordinal 5) with SteamARM's ident query
 *      and prints who answered and its return code;
 *   3. prints GetModuleFileNameW of the module, whether the mapped image
 *      carries the "Wine builtin DLL" marker, what LoadLibraryW of that same
 *      path returns, and what a copy of that file under another name answers
 *      (how the builtin would reach the game's original to forward to it);
 *   4. checks whether each further argument (a Windows path) exists, e.g.
 *      the builtin in WINEDLLPATH seen from inside a container.
 * Output goes to stdout and to ffx_load-result.txt next to the exe.
 *
 *   ffx_load.exe [name|path] [Z:\\path\\to\\check ...]
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct ffxApiHeader { uint64_t type; struct ffxApiHeader *pNext; } ffxApiHeader;
typedef uint32_t (*PfnFfxQuery)(void **context, ffxApiHeader *desc);

#define SA_FFX_QUERY_IDENT 0x53410001ull
struct sa_ffx_query_ident {
    ffxApiHeader header;
    char who[64];
    uint32_t marker;
};

static FILE *res;

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    if (res) {
        va_start(ap, fmt);
        vfprintf(res, fmt, ap);
        va_end(ap);
    }
    fflush(stdout);
}

static const char *u8(const WCHAR *w)
{
    static char buf[4][1024];
    static int i;
    char *b = buf[i++ & 3];
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof(buf[0]), NULL, NULL)) strcpy(b, "?");
    return b;
}

/* ident query through one export; returns the code, fills who */
static uint32_t ident(HMODULE h, LPCSTR exp, char *who, uint32_t *marker, void **fn)
{
    PfnFfxQuery q = (PfnFfxQuery)GetProcAddress(h, exp);
    struct sa_ffx_query_ident d;
    memset(&d, 0, sizeof(d));
    d.header.type = SA_FFX_QUERY_IDENT;
    strcpy(d.who, "(nobody)");
    *fn = (void *)q;
    if (!q) { strcpy(who, "(no export)"); return 0xffffffffu; }
    uint32_t rc = q(NULL, &d.header);
    strcpy(who, d.who);
    *marker = d.marker;
    return rc;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "name";
    WCHAR exe[MAX_PATH], dir[MAX_PATH], target[MAX_PATH], file[MAX_PATH], tmp[MAX_PATH], copy[MAX_PATH];
    char who[64], who2[64];
    uint32_t marker = 0, marker2 = 0;
    void *fn_name, *fn_ord;

    GetModuleFileNameW(NULL, exe, MAX_PATH);
    wcscpy(dir, exe);
    WCHAR *slash = wcsrchr(dir, L'\\');
    if (slash) slash[0] = 0;
    swprintf(tmp, MAX_PATH, L"%ls\\ffx_load-result.txt", dir);
    res = _wfopen(tmp, L"w");

    out("exe: %s\n", u8(exe));
    char env[2048];
    out("env WINEDLLPATH: %s\n", GetEnvironmentVariableA("WINEDLLPATH", env, sizeof env) ? env : "(not visible)");
    out("env WINEDLLOVERRIDES: %s\n", GetEnvironmentVariableA("WINEDLLOVERRIDES", env, sizeof env) ? env : "(not visible)");
    for (int i = 2; i < argc; i++)
        out("exists %s: %s\n", argv[i], GetFileAttributesA(argv[i]) != INVALID_FILE_ATTRIBUTES ? "yes" : "no");

    if (!strcmp(mode, "path")) swprintf(target, MAX_PATH, L"%ls\\amd_fidelityfx_dx12.dll", dir);
    else wcscpy(target, L"amd_fidelityfx_dx12.dll");
    HMODULE h = LoadLibraryW(target);
    DWORD err = GetLastError();
    out("LoadLibraryW(%s) [%s] -> %p%s\n", u8(target), mode, (void *)h, h ? "" : " (failed)");
    if (!h) { out("error %lu\n== ffx_load probe: done answered=none\n", err); return 1; }

    GetModuleFileNameW(h, file, MAX_PATH);
    out("GetModuleFileNameW: %s\n", u8(file));
    out("mapped image +0x40 builtin marker: %s\n",
        !memcmp((const char *)h + 0x40, "Wine builtin DLL", 17) ? "yes" : "no");

    uint32_t rc = ident(h, "ffxQuery", who, &marker, &fn_name);
    uint32_t rc5 = ident(h, MAKEINTRESOURCEA(5), who2, &marker2, &fn_ord);
    out("ffxQuery (by name %p) -> rc %u, answered by: %s (marker %u)\n", fn_name, rc, who, marker);
    out("ordinal 5 (%p) -> rc %u, answered by: %s; same function: %s\n", fn_ord, rc5, who2,
        fn_name == fn_ord ? "yes" : "no");

    /* the same path again: the override applies to full paths as well */
    HMODULE again = LoadLibraryW(file);
    out("LoadLibraryW(GetModuleFileNameW result) -> %p (%s module)\n", (void *)again,
        again == h ? "same" : again ? "another" : "no");
    if (again) FreeLibrary(again);

    /* a copy under a new name: what the builtin would load to forward */
    GetTempPathW(MAX_PATH, tmp);
    swprintf(copy, MAX_PATH, L"%lsffx_load_orig_%lu.dll", tmp, GetCurrentProcessId());
    if (GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) {
        out("copy: %s is not a file\n", u8(file));
    } else if (!CopyFileW(file, copy, FALSE)) {
        out("copy: CopyFileW failed %lu\n", GetLastError());
    } else {
        HMODULE c = LoadLibraryW(copy);
        if (c) {
            uint32_t crc = ident(c, "ffxQuery", who2, &marker2, &fn_ord);
            WCHAR cf[MAX_PATH];
            GetModuleFileNameW(c, cf, MAX_PATH);
            out("copy %s -> %p, ffxQuery rc %u, answered by: %s\n", u8(cf), (void *)c, crc, who2);
            FreeLibrary(c);
        } else {
            out("copy: LoadLibraryW(%s) failed %lu\n", u8(copy), GetLastError());
        }
        DeleteFileW(copy);
    }
    out("== ffx_load probe: done answered=%s\n", who);
    if (res) fclose(res);
    return 0;
}
