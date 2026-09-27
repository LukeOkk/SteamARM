// Windows D3D12 probe for the Proton path: VKD3D-Proton (d3d12) -> Vulkan ->
// FEX thunks -> the runtime's Vulkan shim -> MoltenVK -> Metal.
// Device, direct queue, flip-model swapchain, clear + present FRAMES frames
// with a fence per frame; prints the adapter and the frame rate.
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>

#ifndef FRAMES
#define FRAMES 600
#endif
#define NBUF 2

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(h, m, w, l);
}

#define CHECK(what, expr) do { HRESULT hr_ = (expr); if (FAILED(hr_)) { \
    printf("%s -> 0x%08lx\n== d3d12 probe: FAIL\n", what, (unsigned long)hr_); fflush(stdout); return 2; } } while (0)

int main(void)
{
    HINSTANCE inst = GetModuleHandleA(NULL);
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = inst; wc.lpszClassName = "d3d12probe";
    RegisterClassA(&wc);
    HWND hwnd = CreateWindowA("d3d12probe", "SteamARM D3D12 probe", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                              150, 150, 800, 600, NULL, NULL, inst, NULL);
    if (!hwnd) { printf("CreateWindow failed\n"); return 1; }

    IDXGIFactory4 *fac = NULL;
    CHECK("CreateDXGIFactory2", CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&fac));
    ID3D12Device *dev = NULL;
    CHECK("D3D12CreateDevice", D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev));
    LUID luid = ID3D12Device_GetAdapterLuid(dev);
    IDXGIAdapter1 *ad = NULL;
    DXGI_ADAPTER_DESC1 desc = {0};
    if (SUCCEEDED(IDXGIFactory4_EnumAdapterByLuid(fac, luid, &IID_IDXGIAdapter1, (void **)&ad)))
        IDXGIAdapter1_GetDesc1(ad, &desc);
    printf("D3D12CreateDevice ok, adapter: %ls\n", desc.Description);

    D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    ID3D12CommandQueue *q = NULL;
    CHECK("CreateCommandQueue", ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q));

    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = 800; sd.Height = 600; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = NBUF; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc1 = NULL;
    CHECK("CreateSwapChainForHwnd", IDXGIFactory4_CreateSwapChainForHwnd(fac, (IUnknown *)q, hwnd, &sd, NULL, NULL, &sc1));
    IDXGISwapChain3 *sc = NULL;
    CHECK("QueryInterface(IDXGISwapChain3)", IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc));

    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, NBUF, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
    ID3D12DescriptorHeap *heap = NULL;
    CHECK("CreateDescriptorHeap", ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap));
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h0;
    h0 = ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(heap);
    ID3D12Resource *bufs[NBUF];
    for (int i = 0; i < NBUF; i++) {
        CHECK("GetBuffer", IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&bufs[i]));
        D3D12_CPU_DESCRIPTOR_HANDLE h = { h0.ptr + i * inc };
        ID3D12Device_CreateRenderTargetView(dev, bufs[i], NULL, h);
    }
    ID3D12CommandAllocator *ca = NULL;
    CHECK("CreateCommandAllocator", ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                                        &IID_ID3D12CommandAllocator, (void **)&ca));
    ID3D12GraphicsCommandList *cl = NULL;
    CHECK("CreateCommandList", ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, NULL,
                                                              &IID_ID3D12GraphicsCommandList, (void **)&cl));
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12Fence *fence = NULL;
    CHECK("CreateFence", ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    UINT64 fv = 0;

    LARGE_INTEGER f, t0, t1;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t0);
    for (int i = 0; i < FRAMES; i++) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageA(&msg); }
        UINT bi = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
        CHECK("Reset allocator", ID3D12CommandAllocator_Reset(ca));
        CHECK("Reset list", ID3D12GraphicsCommandList_Reset(cl, ca, NULL));
        D3D12_RESOURCE_BARRIER b = {0};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = bufs[bi];
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        float t = (float)i / FRAMES;
        float col[4] = { 0.2f, t, 1.0f - t, 1.0f };
        D3D12_CPU_DESCRIPTOR_HANDLE h = { h0.ptr + bi * inc };
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, h, col, 0, NULL);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        CHECK("Close", ID3D12GraphicsCommandList_Close(cl));
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
        CHECK("Present", IDXGISwapChain3_Present(sc, 0, 0));
        CHECK("Signal", ID3D12CommandQueue_Signal(q, fence, ++fv));
        if (ID3D12Fence_GetCompletedValue(fence) < fv) {
            ID3D12Fence_SetEventOnCompletion(fence, fv, ev);
            WaitForSingleObject(ev, 5000);
        }
    }
    QueryPerformanceCounter(&t1);
    double s = (double)(t1.QuadPart - t0.QuadPart) / f.QuadPart;
    printf("%d frames in %.2f s = %.1f fps\n== d3d12 probe: ok\n", FRAMES, s, FRAMES / s);
    // Also in probe-result.txt (the working directory): under Steam's
    // launch path the console output goes to Wine's console, not a log.
    FILE *rf = fopen("probe-result.txt", "a");
    if (rf) {
        fprintf(rf, "%d frames in %.2f s = %.1f fps\n== d3d12 probe: ok\n", FRAMES, s, FRAMES / s);
        fclose(rf);
    }
    fflush(stdout);
    return 0;
}
