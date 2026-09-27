// Windows D3D11 probe for the Proton path: DXVK (d3d11/dxgi) -> Vulkan ->
// FEX thunks -> the runtime's Vulkan shim -> MoltenVK -> Metal.
// Opens a window, creates a device and a flip-model swapchain, clears to a
// changing colour and presents FRAMES frames, then prints the adapter and
// the frame rate. Exit code 0 only if every call succeeded.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>

#ifndef FRAMES
#define FRAMES 600
#endif

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int main(void)
{
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.lpszClassName = "d3d11probe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d11probe", "SteamARM D3D11 probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              100, 100, 800, 600, NULL, NULL, inst, NULL);
    if (!hwnd) { printf("CreateWindow failed %lu\n", GetLastError()); return 1; }

    DXGI_SWAP_CHAIN_DESC sd = {0};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 800;
    sd.BufferDesc.Height = 600;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGISwapChain *sc = NULL;
    D3D_FEATURE_LEVEL fl = 0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    printf("D3D11CreateDeviceAndSwapChain -> 0x%08lx, feature level 0x%x\n", (unsigned long)hr, (unsigned)fl);
    if (FAILED(hr)) return 2;

    IDXGIDevice *xdev = NULL;
    IDXGIAdapter *ad = NULL;
    DXGI_ADAPTER_DESC desc = {0};
    if (SUCCEEDED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xdev)) &&
        SUCCEEDED(IDXGIDevice_GetAdapter(xdev, &ad)) && SUCCEEDED(IDXGIAdapter_GetDesc(ad, &desc)))
        printf("adapter: %ls (vendor 0x%04x device 0x%04x)\n", desc.Description, desc.VendorId, desc.DeviceId);

    ID3D11Texture2D *bb = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    hr = IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    if (FAILED(hr)) { printf("render target -> 0x%08lx\n", (unsigned long)hr); return 3; }

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    int bad = 0;
    for (int i = 0; i < FRAMES; i++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        float t = (float)i / FRAMES;
        float col[4] = { t, 0.3f, 1.0f - t, 1.0f };
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, col);
        hr = IDXGISwapChain_Present(sc, 0, 0);
        if (FAILED(hr)) { printf("Present %d -> 0x%08lx\n", i, (unsigned long)hr); bad = 1; break; }
    }
    QueryPerformanceCounter(&t1);
    double s = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    printf("%d frames in %.2f s = %.1f fps\n", FRAMES, s, FRAMES / s);
    printf(bad ? "== d3d11 probe: FAIL\n" : "== d3d11 probe: ok\n");
    // Also in probe-result.txt (the working directory): under Steam's
    // launch path the console output goes to Wine's console, not a log.
    FILE *rf = fopen("probe-result.txt", "a");
    if (rf) {
        fprintf(rf, "%d frames in %.2f s = %.1f fps\n%s", FRAMES, s, FRAMES / s,
                bad ? "== d3d11 probe: FAIL\n" : "== d3d11 probe: ok\n");
        fclose(rf);
    }
    fflush(stdout);
    return bad;
}
