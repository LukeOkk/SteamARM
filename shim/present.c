// Present modes: what the game asks for, and what reaches the driver.
//
// The present mode is the application's choice in VkSwapchainCreateInfoKHR
// (DXVK: dxgi.syncInterval or the game's SyncInterval; VKD3D-Proton:
// VKD3D_SWAPCHAIN_PRESENT_MODE). No driver setting changes it: MoltenVK has no
// such MVK_CONFIG variable. Metal surfaces offer FIFO and IMMEDIATE on both
// MoltenVK and KosmicKrisp (MEASURED, vulkaninfo).
//
//   LXRT_VK_DEBUG=1              each swapchain logs the driver in use, the
//                                mode requested and the mode passed on.
//   LXRT_VK_PRESENT_MODE=FIFO|IMMEDIATE
//                                replaces the requested mode, but only if the
//                                surface lists it; otherwise the request goes
//                                through unchanged and the log says why.
//
// "Passed on" is what the driver is asked for. What Metal then does with it
// (CAMetalLayer.displaySyncEnabled) is not visible from here.
//
// Stretching. Wine's display-mode emulation ("Escala de resolución",
// EmulateModeset) keeps a game's swapchain at the mode it chose while its X
// window -- and so the layer -- has the screen's size. MoltenVK then answers
// every present with VK_SUBOPTIMAL_KHR and DXVK recreates the swapchain
// without end (4 frames in 2.5 s, MEASURED, benchmarks/stage43). Wine's
// win32u asks for stretching (VkSwapchainPresentScalingCreateInfoEXT) only
// when the instance enabled VK_EXT_surface_maintenance1, and DXVK enables the
// KHR one; so on MoltenVK a swapchain whose extent differs from the surface's
// gets STRETCH here, as Wine would have asked. Other filters than this
// bilinear stretch: shim/scaler.c (LXRT_VK_SCALER).
#include <stdint.h>
#include <stddef.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

extern char *getenv(const char *);   // the guest's libc (the shim is -nostdlib)
extern int dprintf(int, const char *, ...);
extern const char *lxrt_vk_driver;   // vulkan_shim.c (generated): the driver in use

VkResult lxrt_mvk_vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *,
                                       VkSwapchainKHR *);
VkResult lxrt_mvk_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *,
                                                           VkPresentModeKHR *);

// Device -> physical device, recorded by vkCreateDevice (features.c): the
// surface's present modes are a physical-device query. Few devices; a slot
// is reused when a handle comes back or the table is full.
static struct { VkDevice dev; VkPhysicalDevice pd; } g_dev[16];
static unsigned g_dev_next;
static volatile int g_dev_lock;

void lxrt_note_device(VkDevice dev, VkPhysicalDevice pd)
{
    while (__atomic_exchange_n(&g_dev_lock, 1, __ATOMIC_ACQUIRE))
        ;
    unsigned k = 16;
    for (unsigned i = 0; i < 16 && k == 16; i++)
        if (g_dev[i].dev == dev || !g_dev[i].dev)
            k = i;
    if (k == 16)
        k = g_dev_next++ % 16;
    g_dev[k].dev = dev;
    g_dev[k].pd = pd;
    __atomic_store_n(&g_dev_lock, 0, __ATOMIC_RELEASE);
}

static VkPhysicalDevice device_pd(VkDevice dev);

VkPhysicalDevice lxrt_device_pd(VkDevice dev) { return device_pd(dev); }   // scaler.c

static VkPhysicalDevice device_pd(VkDevice dev)
{
    VkPhysicalDevice pd = 0;
    while (__atomic_exchange_n(&g_dev_lock, 1, __ATOMIC_ACQUIRE))
        ;
    for (unsigned i = 0; i < 16 && !pd; i++)
        if (g_dev[i].dev == dev)
            pd = g_dev[i].pd;
    __atomic_store_n(&g_dev_lock, 0, __ATOMIC_RELEASE);
    return pd;
}

static const char *mode_name(VkPresentModeKHR m)
{
    switch ((int)m) {
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX";
    case VK_PRESENT_MODE_FIFO_KHR: return "FIFO";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED";
    case 1000361000: return "FIFO_LATEST_READY";
    case 1000111000: return "SHARED_DEMAND_REFRESH";
    case 1000111001: return "SHARED_CONTINUOUS_REFRESH";
    default: return "?";
    }
}

// Case-insensitive: "fifo" is as good as "FIFO" (VKD3D-Proton ignores the
// lowercase spelling of its own variable, MEASURED in the audit).
static int s_ieq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++)
        if ((*a | 0x20) != (*b | 0x20))
            return 0;
    return *a == *b;
}

// 1: an override was asked for (*m set); 0: none; -1: a value not understood.
static int wanted_mode(const char **raw, VkPresentModeKHR *m)
{
    const char *e = getenv("LXRT_VK_PRESENT_MODE");
    *raw = e;
    if (!e || !*e)
        return 0;
    if (s_ieq(e, "FIFO")) { *m = VK_PRESENT_MODE_FIFO_KHR; return 1; }
    if (s_ieq(e, "IMMEDIATE")) { *m = VK_PRESENT_MODE_IMMEDIATE_KHR; return 1; }
    return -1;
}

static int surface_offers(VkPhysicalDevice pd, VkSurfaceKHR surface, VkPresentModeKHR m)
{
    VkPresentModeKHR modes[16];
    uint32_t n = 16;
    VkResult r = lxrt_mvk_vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surface, &n, modes);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
        return 0;
    for (uint32_t i = 0; i < n && i < 16; i++)
        if (modes[i] == m)
            return 1;
    return 0;
}

VkResult lxrt_inner_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, void *);   // wsi.c
int lxrt_wsi_layer_extent(VkSurfaceKHR, uint32_t *, uint32_t *);                                           // wsi.c
int lxrt_wsi_adaptive_sync(VkSurfaceKHR);   // wsi.c: native fullscreen + actual display range

// 1 when the swapchain must be enlarged over its surface (above); *window
// is then the surface's size. MoltenVK stretches it itself; any other
// driver (KosmicKrisp) gets the scaler's own pass (scaler.c): with a
// swapchain smaller than its layer KosmicKrisp made DXVK recreate it 315
// times in 2.5 s (MEASURED, benchmarks/stage47).
static int needs_stretch(VkDevice dev, const VkSwapchainCreateInfoKHR *ci, VkExtent2D *window)
{
    if (!ci->surface)
        return 0;
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT)
            return 0;
    VkPhysicalDevice pd = device_pd(dev);
    VkSurfaceCapabilitiesKHR caps;
    if (!pd || lxrt_inner_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, ci->surface, &caps) != VK_SUCCESS ||
        caps.currentExtent.width == 0xFFFFFFFFu)
        return 0;
    // The layer's size, which the game's own extents (wsi.c window_extents)
    // may not be: a game on its full-screen toplevel.
    uint32_t lw, lh;
    if (lxrt_wsi_layer_extent(ci->surface, &lw, &lh)) {
        caps.currentExtent.width = lw;
        caps.currentExtent.height = lh;
    }
    *window = caps.currentExtent;
    if (caps.currentExtent.width != ci->imageExtent.width || caps.currentExtent.height != ci->imageExtent.height)
        return 1;
    // LXRT_VK_SCALER_NATIVE=1 (the game only, tools/steamarm-fex-proton) with
    // the MetalFX temporal filter: the picture goes through the scaler at its
    // own size -- MetalFX's temporal pass as antialiasing at the native
    // resolution, the 100 % of "Calidad de MetalFX". Game-sized windows only.
    // 2, not 1: the sizes match, and a scaler that cannot be made falls back
    // to the mailbox like any window-sized swapchain.
    const char *nat = getenv("LXRT_VK_SCALER_NATIVE"), *flt = getenv("LXRT_VK_SCALER");
    return nat && *nat == '1' && flt && (s_ieq(flt, "metalfx-temporal") || s_ieq(flt, "metalfx_temporal")) &&
           caps.currentExtent.width >= 1024 ? 2 : 0;
}

// shim/scaler.c: the other filters (LXRT_VK_SCALER), or an error when MoltenVK
// is to stretch.
VkResult lxrt_scaler_create(VkDevice, VkPhysicalDevice, const VkSwapchainCreateInfoKHR *, VkExtent2D,
                            const VkAllocationCallbacks *, VkSwapchainKHR *);
// shim/mailbox.c: a swapchain that does not wait for the display (IMMEDIATE,
// MAILBOX; FIFO holds the game to the display's rate without the wait at
// every acquire).
int lxrt_mailbox_wanted(const VkSwapchainCreateInfoKHR *);
VkResult lxrt_mailbox_create(VkDevice, VkPhysicalDevice, const VkSwapchainCreateInfoKHR *,
                             const VkAllocationCallbacks *, VkSwapchainKHR *);
void lxrt_mailbox_retire(VkSwapchainKHR);
void lxrt_scaler_note_plain(VkDevice, VkSwapchainKHR, VkExtent2D, VkFormat);

VkResult lxrt_inner_vkCreateSwapchainKHR(VkDevice dev, const VkSwapchainCreateInfoKHR *ci,
                                         const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    const char *d = getenv("LXRT_VK_DEBUG"), *raw = 0;
    int dbg = d && *d == '1';
    VkPresentModeKHR want = VK_PRESENT_MODE_FIFO_KHR;
    int ov = wanted_mode(&raw, &want);
    VkExtent2D window = { 0, 0 };
    int stretch = ci && needs_stretch(dev, ci, &window);
    const char *adaptive = getenv("LXRT_VK_ADAPTIVE_SYNC");
    int requested = adaptive && adaptive[0] == '1';
    // Metal owns VRR timing. Keep display sync through direct FIFO only on an
    // eligible native fullscreen surface. Explicit mode overrides win.
    // Never make an unsupported monitor or a borderless X window "active".
    int vrr = requested && !ov && ci && !stretch && lxrt_wsi_adaptive_sync(ci->surface) &&
              device_pd(dev) && surface_offers(device_pd(dev), ci->surface, VK_PRESENT_MODE_FIFO_KHR);
    if (vrr) { want = VK_PRESENT_MODE_FIFO_KHR; ov = 1; raw = "FIFO (Adaptive Sync request)"; }
    if (dbg && requested)
        dprintf(2, "[shim] Adaptive Sync request: %s; physical VRR activation unverified\n",
                vrr ? "eligible native fullscreen, direct FIFO" : "original presentation retained");
    if (dbg && ci)
        dprintf(2, "[shim] vkCreateSwapchainKHR extent %ux%u usage 0x%x flags 0x%x images %u, surface %ux%u, format %d colorspace %d\n",
                ci->imageExtent.width, ci->imageExtent.height, (unsigned)ci->imageUsage, (unsigned)ci->flags,
                ci->minImageCount, window.width, window.height, (int)ci->imageFormat, (int)ci->imageColorSpace);
    // The swapchain this one replaces: its thread leaves the driver first.
    if (ci)
        lxrt_mailbox_retire(ci->oldSwapchain);
    if (stretch && lxrt_scaler_create(dev, device_pd(dev), ci, window, alloc, out) == VK_SUCCESS)
        return VK_SUCCESS;
    if (stretch == 2)
        stretch = 0;   // native MetalFX refused: the plain path, mailbox included
    if (ci && !stretch) {
        VkSwapchainCreateInfoKHR m = *ci;
        if (ov > 0 && device_pd(dev) && surface_offers(device_pd(dev), ci->surface, want))
            m.presentMode = want;
        if (!vrr && lxrt_mailbox_wanted(&m)) {
            VkResult mr = lxrt_mailbox_create(dev, device_pd(dev), &m, alloc, out);
            if (dbg)
                dprintf(2, "[shim] vkCreateSwapchainKHR driver=%s requested=%s: mailbox -> %d\n", lxrt_vk_driver,
                        mode_name(m.presentMode), (int)mr);
            if (mr == VK_SUCCESS)
                return VK_SUCCESS;
        }
    }
    const char *pe = getenv("LXRT_VK_SCALER_PROBE");
    int probe = pe && *pe == '1';
    if ((!dbg && !ov && !stretch && !probe) || !ci)
        return lxrt_mvk_vkCreateSwapchainKHR(dev, ci, alloc, out);
    // Present scaling is MoltenVK's to honour (VK_EXT_swapchain_maintenance1);
    // other drivers were given the scaler above, or keep the swapchain as is.
    if (!s_ieq(lxrt_vk_driver, "moltenvk"))
        stretch = 0;

    // A copy: the caller's structure is const and stays as it was.
    VkSwapchainCreateInfoKHR c = *ci;
    VkSwapchainPresentScalingCreateInfoEXT scaling = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_SCALING_CREATE_INFO_EXT,
        .pNext = ci->pNext,
        // Its aspect kept and centred, as Proton's fullscreen hack and the
        // scaler's own passes (scaler.c fit) show a game.
        .scalingBehavior = VK_PRESENT_SCALING_ASPECT_RATIO_STRETCH_BIT_EXT,
        .presentGravityX = VK_PRESENT_GRAVITY_CENTERED_BIT_EXT,
        .presentGravityY = VK_PRESENT_GRAVITY_CENTERED_BIT_EXT,
    };
    if (stretch)
        c.pNext = &scaling;
    if (probe)
        c.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;   // scaler.c reads it back once
    const char *note = "";
    if (ov < 0) {
        note = " (LXRT_VK_PRESENT_MODE not understood: FIFO or IMMEDIATE)";
    } else if (ov > 0 && want != ci->presentMode) {
        VkPhysicalDevice pd = device_pd(dev);
        if (!pd)
            note = " (override ignored: device not created through the shim)";
        else if (!surface_offers(pd, ci->surface, want))
            note = " (override ignored: the surface does not offer it)";
        else
            c.presentMode = want;
    }
    VkResult r = lxrt_mvk_vkCreateSwapchainKHR(dev, &c, alloc, out);
    if (probe && r == VK_SUCCESS)
        lxrt_scaler_note_plain(dev, *out, c.imageExtent, c.imageFormat);
    if (dbg || ov < 0 || *note)
        dprintf(2, "[shim] vkCreateSwapchainKHR driver=%s requested=%s passed=%s%s%s%s%s -> %d\n", lxrt_vk_driver,
                mode_name(ci->presentMode), mode_name(c.presentMode), ov ? " LXRT_VK_PRESENT_MODE=" : "",
                ov ? raw : "", note, stretch ? " stretched" : "", (int)r);
    return r;
}
