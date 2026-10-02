/* A game changing its video mode on SteamARM's X server: the window goes
   fullscreen (_NET_WM_STATE_FULLSCREEN, as SDL does for "fullscreen windowed"),
   back to a 1280x720 window, borderless (_MOTIF_WM_HINTS) at the display's
   size, and fullscreen again; the swapchain is made again (oldSwapchain) at
   each new size and FIFO frames are presented. The picture must keep coming
   at the display's rate: KosmicKrisp kept a retain on every drawable it had
   presented or had been given, and after two swapchains with an acquired
   image in hand (the mailbox always has one) the layer had no drawable left
   -- a second's wait at every frame (Counter-Strike 2 after a change of its
   video settings: black; patches/kosmickrisp-07; benchmarks/stage54). A
   phase with a median frame over 100 ms or more than two frames over 200 ms
   fails. Prints "== vk x11 modes: ok" or "MAL". */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef struct xcb_connection_t xcb_connection_t;
typedef struct { unsigned int sequence; } xcb_void_cookie_t;
typedef struct {
    uint32_t root, default_colormap, white_pixel, black_pixel, current_input_masks;
    uint16_t width_in_pixels, height_in_pixels, width_in_millimeters, height_in_millimeters;
    uint16_t min_installed_maps, max_installed_maps;
    uint32_t root_visual;
    uint8_t backing_stores, save_unders, root_depth, allowed_depths_len;
} xcb_screen_t;
typedef struct { xcb_screen_t *data; int rem; int index; } xcb_screen_iterator_t;
typedef struct { unsigned int sequence; } xcb_cookie_t;
typedef struct { uint8_t response_type, pad0; uint16_t sequence; uint32_t length; uint32_t atom; } xcb_intern_atom_reply_t;
typedef struct {
    uint8_t response_type, depth; uint16_t sequence; uint32_t length;
    uint32_t root; int16_t x, y; uint16_t width, height, border_width; uint8_t pad1[2];
} xcb_get_geometry_reply_t;
typedef struct {
    uint8_t response_type, format; uint16_t sequence; uint32_t window, type;
    union { uint32_t data32[5]; uint8_t data8[20]; } data;
} xcb_client_message_event_t;
xcb_connection_t *xcb_connect(const char *, int *);
int xcb_connection_has_error(xcb_connection_t *);
const void *xcb_get_setup(xcb_connection_t *);
xcb_screen_iterator_t xcb_setup_roots_iterator(const void *);
uint32_t xcb_generate_id(xcb_connection_t *);
xcb_void_cookie_t xcb_create_window(xcb_connection_t *, uint8_t, uint32_t, uint32_t, int16_t, int16_t,
                                    uint16_t, uint16_t, uint16_t, uint16_t, uint32_t, uint32_t, const void *);
xcb_void_cookie_t xcb_map_window(xcb_connection_t *, uint32_t);
xcb_void_cookie_t xcb_configure_window(xcb_connection_t *, uint32_t, uint16_t, const void *);
xcb_void_cookie_t xcb_change_property(xcb_connection_t *, uint8_t, uint32_t, uint32_t, uint32_t, uint8_t, uint32_t, const void *);
xcb_void_cookie_t xcb_send_event(xcb_connection_t *, uint8_t, uint32_t, uint32_t, const char *);
int xcb_flush(xcb_connection_t *);
xcb_cookie_t xcb_intern_atom(xcb_connection_t *, uint8_t, uint16_t, const char *);
xcb_intern_atom_reply_t *xcb_intern_atom_reply(xcb_connection_t *, xcb_cookie_t, void **);
xcb_cookie_t xcb_get_geometry(xcb_connection_t *, uint32_t);
xcb_get_geometry_reply_t *xcb_get_geometry_reply(xcb_connection_t *, xcb_cookie_t, void **);

typedef struct { VkStructureType sType; const void *pNext; VkFlags flags;
                 xcb_connection_t *connection; uint32_t window; } XcbSurfaceCreateInfo;
typedef VkResult (VKAPI_PTR *PFN_CreateXcbSurface)(VkInstance, const XcbSurfaceCreateInfo *,
                                                   const VkAllocationCallbacks *, VkSurfaceKHR *);

#define CK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    printf("FALLO %s -> %d\n== vk x11 modes: MAL\n", #x, _r); return 1; } } while (0)

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

static xcb_connection_t *c;
static uint32_t win, root, screen_w, screen_h;
static uint32_t atom(const char *name) {
    xcb_intern_atom_reply_t *r = xcb_intern_atom_reply(c, xcb_intern_atom(c, 0, strlen(name), name), NULL);
    uint32_t a = r ? r->atom : 0;
    free(r);
    return a;
}

static void net_wm_fullscreen(int on) {
    xcb_client_message_event_t ev = { .response_type = 33, .format = 32, .window = win, .type = atom("_NET_WM_STATE") };
    ev.data.data32[0] = on ? 1 : 0;
    ev.data.data32[1] = atom("_NET_WM_STATE_FULLSCREEN");
    ev.data.data32[3] = 1;
    xcb_send_event(c, 0, root, (1 << 20) | (1 << 19) /* SubstructureRedirect|SubstructureNotify */, (const char *)&ev);
    xcb_flush(c);
}

static void borderless(int on) {
    uint32_t hints[5] = { 2, 0, on ? 0u : 1u, 0, 0 };    /* MWM_HINTS_DECORATIONS */
    uint32_t a = atom("_MOTIF_WM_HINTS");
    xcb_change_property(c, 0, win, a, a, 32, 5, hints);
    xcb_flush(c);
}

static void move_resize(int x, int y, uint32_t w, uint32_t h) {
    uint32_t v[4] = { (uint32_t)x, (uint32_t)y, w, h };
    xcb_configure_window(c, win, 1 | 2 | 4 | 8, v);
    xcb_flush(c);
}

static void geometry(const char *what) {
    xcb_get_geometry_reply_t *g = xcb_get_geometry_reply(c, xcb_get_geometry(c, win), NULL);
    if (g) printf("  %s: window %ux%u+%d+%d\n", what, g->width, g->height, g->x, g->y);
    free(g);
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int frames = argc > 1 ? atoi(argv[1]) : 120;
    c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) { printf("sin X\n== vk x11 modes: MAL\n"); return 1; }
    xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    root = scr->root; screen_w = scr->width_in_pixels; screen_h = scr->height_in_pixels;
    win = xcb_generate_id(c);
    uint32_t mask = 1 << 11;       /* CW_EVENT_MASK */
    uint32_t ev = 1 << 17;         /* StructureNotify */
    xcb_create_window(c, 0, win, root, 320, 180, 1280, 720, 0, 1, scr->root_visual, mask, &ev);
    xcb_change_property(c, 0, win, 39 /* WM_NAME */, 31 /* STRING */, 8, 13, "vk_x11_modes ");
    xcb_map_window(c, win);
    xcb_flush(c);
    printf("X window 0x%x, screen %ux%u\n", win, screen_w, screen_h);

    const char *inst_ext[] = { "VK_KHR_surface", "VK_KHR_xcb_surface" };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "vk_x11_modes", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                 .enabledExtensionCount = 2, .ppEnabledExtensionNames = inst_ext };
    VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
    PFN_CreateXcbSurface createXcb = (PFN_CreateXcbSurface)vkGetInstanceProcAddr(inst, "vkCreateXcbSurfaceKHR");
    XcbSurfaceCreateInfo xsi = { .sType = 1000005000, .connection = c, .window = win };
    VkSurfaceKHR surf; CK(createXcb(inst, &xsi, NULL, &surf));
    uint32_t ndev = 1; VkPhysicalDevice phys;
    CK(vkEnumeratePhysicalDevices(inst, &ndev, &phys));
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    uint32_t qi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 sup = 0;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surf, &sup);
        if (sup && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { qi = i; break; }
    }
    if (qi == UINT32_MAX) { printf("ninguna cola presenta\n== vk x11 modes: MAL\n"); return 1; }
    float prio = 1.0f;
    const char *dev_ext[] = { "VK_KHR_swapchain" };
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_ext };
    VkDevice dev; CK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, qi, 0, &queue);
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = qi };
    VkCommandPool pool; CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev, &cai, &cmd));
    VkSemaphoreCreateInfo semi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore acquired, rendered;
    CK(vkCreateSemaphore(dev, &semi, NULL, &acquired));
    CK(vkCreateSemaphore(dev, &semi, NULL, &rendered));
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence; CK(vkCreateFence(dev, &fci, NULL, &fence));

    /* The sequence of changes (argv[2]): w window 1280x720 at 320,180;
       f fullscreen on; u fullscreen off; b borderless at the display's size;
       d decorated again; r window 1600x900; s the same size again (no
       change, a new swapchain only). Default: wfubf. */
    const char *seq = argc > 2 ? argv[2] : "wfubf";
    int nphases = strlen(seq);
    VkSwapchainKHR sc = VK_NULL_HANDLE;
    VkImage imgs[16];
    int bad = 0;
    double *ft = calloc(frames, sizeof *ft);
    for (int phase = 0; phase < nphases; phase++) {
        char phase_name[64];
        switch (seq[phase]) {
        case 'w': move_resize(320, 180, 1280, 720); strcpy(phase_name, "window 1280x720"); break;
        case 'f': net_wm_fullscreen(1); strcpy(phase_name, "fullscreen on"); break;
        case 'u': net_wm_fullscreen(0); strcpy(phase_name, "fullscreen off"); break;
        case 'b': borderless(1); move_resize(0, 0, screen_w, screen_h); strcpy(phase_name, "borderless, display size"); break;
        case 'd': borderless(0); strcpy(phase_name, "decorated again"); break;
        case 'r': move_resize(160, 90, 1600, 900); strcpy(phase_name, "window 1600x900"); break;
        default: strcpy(phase_name, "same size, new swapchain"); break;
        }
        usleep(400000);     /* the window manager's turn */
        geometry(phase_name);
        VkSurfaceCapabilitiesKHR caps;
        CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps));
        VkExtent2D ext = caps.currentExtent;
        if (ext.width == UINT32_MAX) { ext.width = 1280; ext.height = 720; }
        uint32_t nimg = caps.minImageCount + 1;
        if (caps.maxImageCount && nimg > caps.maxImageCount) nimg = caps.maxImageCount;
        VkSwapchainCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface = surf, .minImageCount = nimg, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
            .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = ext, .imageArrayLayers = 1,
            .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
            .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
            .clipped = VK_TRUE, .oldSwapchain = sc };
        VkSwapchainKHR nsc; CK(vkCreateSwapchainKHR(dev, &sci, NULL, &nsc));
        if (sc) vkDestroySwapchainKHR(dev, sc, NULL);
        sc = nsc;
        uint32_t got = 16; vkGetSwapchainImagesKHR(dev, sc, &got, imgs);
        double prev = now_ms();
        int slow = 0;
        for (int f = 0; f < frames; f++) {
            uint32_t idx = 0;
            VkResult ar = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx);
            if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { printf("acquire -> %d\n", ar); bad++; break; }
            vkResetCommandBuffer(cmd, 0);
            VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
            vkBeginCommandBuffer(cmd, &bi);
            VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            VkImageMemoryBarrier to_dst = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = imgs[idx], .subresourceRange = rng, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, NULL, 0, NULL, 1, &to_dst);
            float t = (float)f / (float)frames;
            VkClearColorValue col = { .float32 = { t, phase * 0.2f, 1.0f - t, 1.0f } };
            vkCmdClearColorImage(cmd, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rng);
            VkImageMemoryBarrier to_present = to_dst;
            to_present.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_present.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            to_present.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_present.dstAccessMask = 0;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                                 0, 0, NULL, 0, NULL, 1, &to_present);
            vkEndCommandBuffer(cmd);
            VkPipelineStageFlags wait = VK_PIPELINE_STAGE_TRANSFER_BIT;
            VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                .pWaitSemaphores = &acquired, .pWaitDstStageMask = &wait, .commandBufferCount = 1,
                .pCommandBuffers = &cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered };
            CK(vkQueueSubmit(queue, 1, &si, fence));
            VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                .pWaitSemaphores = &rendered, .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx };
            VkResult pr = vkQueuePresentKHR(queue, &pi);
            if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) { printf("present -> %d\n", pr); bad++; break; }
            vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
            vkResetFences(dev, 1, &fence);
            double n = now_ms(); ft[f] = n - prev; prev = n;
            if (ft[f] > 200) slow++;
        }
        int n = frames;
        for (int i = 1; i < n; i++) { double k = ft[i]; int j = i - 1;
            while (j >= 0 && ft[j] > k) { ft[j+1] = ft[j]; j--; } ft[j+1] = k; }
        printf("  %s: swapchain %ux%u, %d frames, median %.2f ms, max %.1f ms, %d over 200 ms%s\n", phase_name,
               ext.width, ext.height, n, ft[n/2], ft[n-1], slow, ft[n/2] > 100 || slow > 2 ? "  <-- the picture stopped" : "");
        if (ft[n/2] > 100 || slow > 2) bad++;
    }
    vkDeviceWaitIdle(dev);
    printf("== vk x11 modes: %s\n", bad ? "MAL" : "ok");
    return bad != 0;
}
