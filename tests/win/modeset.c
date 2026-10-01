// A game's lower full-screen resolution, the way Wine's display-mode
// emulation (HKCU\Software\Wine\X11 Driver, EmulateModeset) serves it: the
// modes listed, ChangeDisplaySettings to 1280x720, the screen size the
// program then sees, and a borderless D3D11 window of that size presenting
// for two seconds. tests/win/run.sh compares the X window and the Vulkan
// swapchain with it: the program's 1280x720 shown over the whole screen is
// the "Escala de resolución" of Settings. The left half is drawn red and the
// right half blue: the shim's probe (LXRT_VK_SCALER_PROBE) reads a pixel of
// each half of what reaches the window.
#define COBJMACROS
#include <windows.h>
#include <d3d11_1.h>
#include <dxgi.h>
#include <stdio.h>

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

int main(void)
{
    DEVMODEA dm = { .dmSize = sizeof dm };
    int n = 0, has720 = 0;
    while (EnumDisplaySettingsA(NULL, n, &dm)) {
        if (dm.dmPelsWidth == 1280 && dm.dmPelsHeight == 720) has720 = 1;
        n++;
    }
    EnumDisplaySettingsA(NULL, ENUM_CURRENT_SETTINGS, &dm);
    printf("modes %d (1280x720 %s), current %lux%lu\n", n, has720 ? "listed" : "not listed",
           dm.dmPelsWidth, dm.dmPelsHeight);
    DEVMODEA want = { .dmSize = sizeof want, .dmPelsWidth = 1280, .dmPelsHeight = 720,
                      .dmFields = DM_PELSWIDTH | DM_PELSHEIGHT };
    LONG r = ChangeDisplaySettingsA(&want, CDS_FULLSCREEN);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    printf("ChangeDisplaySettings 1280x720 -> %ld; screen now %dx%d\n", r, sw, sh);

    WNDCLASSA wc = { 0 };
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "modesetprobe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("modesetprobe", "SteamARM modeset probe", WS_POPUP | WS_VISIBLE,
                              0, 0, sw, sh, NULL, NULL, wc.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC sd = { 0 };
    sd.BufferCount = 2;
    sd.BufferDesc.Width = sw;
    sd.BufferDesc.Height = sh;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain *sc = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0,
                                               D3D11_SDK_VERSION, &sd, &sc, &dev, NULL, &ctx);
    if (FAILED(hr)) { printf("D3D11CreateDeviceAndSwapChain 0x%08lx\n== modeset probe: FAIL\n", hr); return 1; }
    ID3D11Texture2D *bb = NULL;
    ID3D11RenderTargetView *rtv = NULL;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);
    D3D11_TEXTURE2D_DESC td;
    ID3D11Texture2D_GetDesc(bb, &td);
    printf("backbuffer %ux%u\n", td.Width, td.Height);
    ID3D11DeviceContext1 *ctx1 = NULL;
    ID3D11DeviceContext_QueryInterface(ctx, &IID_ID3D11DeviceContext1, (void **)&ctx1);
    D3D11_RECT left = { 0, 0, (LONG)td.Width / 2, (LONG)td.Height };
    D3D11_RECT right = { (LONG)td.Width / 2, 0, (LONG)td.Width, (LONG)td.Height };
    DWORD t0 = GetTickCount();
    int frames = 0;
    while (GetTickCount() - t0 < 2500) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) DispatchMessageA(&msg);
        const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f }, blue[4] = { 0.0f, 0.0f, 1.0f, 1.0f };
        if (ctx1) {
            ID3D11DeviceContext1_ClearView(ctx1, (ID3D11View *)rtv, red, &left, 1);
            ID3D11DeviceContext1_ClearView(ctx1, (ID3D11View *)rtv, blue, &right, 1);
        } else {
            ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, red);
        }
        IDXGISwapChain_Present(sc, 1, 0);
        frames++;
    }
    RECT rc;
    GetWindowRect(hwnd, &rc);
    printf("window %ldx%ld, %d frames\n", rc.right - rc.left, rc.bottom - rc.top, frames);
    ChangeDisplaySettingsA(NULL, 0);
    printf("== modeset probe: %s\n", (r == DISP_CHANGE_SUCCESSFUL && frames > 10) ? "ok" : "FAIL");
    return 0;
}
