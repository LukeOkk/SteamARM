// Windows D3D9 probe (DXVK d3d9), usually built 32-bit: older games are
// D3D9 and i386. Device, Clear, Present FRAMES frames; prints the adapter
// and the frame rate, also into probe-result.txt next to the executable.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <stdio.h>

#ifndef FRAMES
#define FRAMES 600
#endif

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

static void report(const char *fmt, ...)
{
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    fputs(buf, stdout);
    fflush(stdout);
    FILE *f = fopen("probe-result.txt", "a");
    if (f) { fputs(buf, f); fclose(f); }
}

int main(void)
{
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = "d3d9probe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d9probe", "SteamARM D3D9 probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              200, 200, 800, 600, NULL, NULL, inst, NULL);
    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { report("Direct3DCreate9 failed\n== d3d9 probe: FAIL\n"); return 1; }
    D3DADAPTER_IDENTIFIER9 id = {0};
    IDirect3D9_GetAdapterIdentifier(d3d, D3DADAPTER_DEFAULT, 0, &id);
    D3DPRESENT_PARAMETERS pp = {0};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.BackBufferWidth = 800; pp.BackBufferHeight = 600;
    pp.hDeviceWindow = hwnd;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    IDirect3DDevice9 *dev = NULL;
    HRESULT hr = IDirect3D9_CreateDevice(d3d, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                         D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    report("CreateDevice -> 0x%08lx, adapter: %s (%d-bit process)\n", (unsigned long)hr, id.Description,
           (int)(sizeof(void *) * 8));
    if (FAILED(hr)) { report("== d3d9 probe: FAIL\n"); return 2; }
    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int bad = 0;
    for (int i = 0; i < FRAMES; i++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        int c = i * 255 / FRAMES;
        IDirect3DDevice9_Clear(dev, 0, NULL, D3DCLEAR_TARGET, D3DCOLOR_XRGB(c, 128, 255 - c), 1.0f, 0);
        IDirect3DDevice9_BeginScene(dev);
        IDirect3DDevice9_EndScene(dev);
        hr = IDirect3DDevice9_Present(dev, NULL, NULL, NULL, NULL);
        if (FAILED(hr)) { report("Present %d -> 0x%08lx\n", i, (unsigned long)hr); bad = 1; break; }
    }
    QueryPerformanceCounter(&t1);
    double s = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    report("%d frames in %.2f s = %.1f fps\n%s", FRAMES, s, FRAMES / s,
           bad ? "== d3d9 probe: FAIL\n" : "== d3d9 probe: ok\n");
    return bad;
}
