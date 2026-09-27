/* Native-window stage W2: an X11 window -> vkCreateXcbSurfaceKHR (the shim's
   shim/wsi.c: a CAMetalLayer in a cross-process CAContext, id published as the
   window property _STEAMARM_LAYER) -> swapchain -> acquire/clear/present. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The few xcb calls needed, declared by hand (no xcb headers in the sysroot). */
typedef struct xcb_connection_t xcb_connection_t;
typedef struct { unsigned int sequence; } xcb_void_cookie_t;
typedef struct { uint32_t data[4]; } xcb_screen_iterator_raw;
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
    uint8_t response_type, format; uint16_t sequence; uint32_t length;
    uint32_t type, bytes_after, value_len; uint8_t pad0[12];
} xcb_get_property_reply_t;
xcb_connection_t *xcb_connect(const char *, int *);
int xcb_connection_has_error(xcb_connection_t *);
const void *xcb_get_setup(xcb_connection_t *);
xcb_screen_iterator_t xcb_setup_roots_iterator(const void *);
uint32_t xcb_generate_id(xcb_connection_t *);
xcb_void_cookie_t xcb_create_window(xcb_connection_t *, uint8_t, uint32_t, uint32_t, int16_t, int16_t,
                                    uint16_t, uint16_t, uint16_t, uint16_t, uint32_t, uint32_t, const void *);
xcb_void_cookie_t xcb_map_window(xcb_connection_t *, uint32_t);
int xcb_flush(xcb_connection_t *);
xcb_cookie_t xcb_intern_atom(xcb_connection_t *, uint8_t, uint16_t, const char *);
xcb_intern_atom_reply_t *xcb_intern_atom_reply(xcb_connection_t *, xcb_cookie_t, void **);
xcb_cookie_t xcb_get_property(xcb_connection_t *, uint8_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
xcb_get_property_reply_t *xcb_get_property_reply(xcb_connection_t *, xcb_cookie_t, void **);
void *xcb_get_property_value(const xcb_get_property_reply_t *);

typedef struct { VkStructureType sType; const void *pNext; VkFlags flags;
                 xcb_connection_t *connection; uint32_t window; } XcbSurfaceCreateInfo;
typedef VkResult (VKAPI_PTR *PFN_CreateXcbSurface)(VkInstance, const XcbSurfaceCreateInfo *,
                                                   const VkAllocationCallbacks *, VkSurfaceKHR *);

#define CK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    printf("FALLO %s -> %d\n", #x, _r); return 1; } } while (0)

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int frames = argc > 1 ? atoi(argv[1]) : 240;
    uint32_t dw = 800, dh = 600;

    xcb_connection_t *c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) {
        printf("xcb_connect(%s) failed: %d\n", getenv("DISPLAY"), xcb_connection_has_error(c));
        printf("== vk x11 surface: MAL (sin X)\n");
        return 1;
    }
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(c));
    xcb_screen_t *scr = it.data;
    uint32_t win = xcb_generate_id(c);
    xcb_create_window(c, 0 /* copy depth */, win, scr->root, 100, 100, dw, dh, 0,
                      1 /* InputOutput */, scr->root_visual, 0, NULL);
    xcb_map_window(c, win);
    xcb_flush(c);
    printf("X window 0x%x on root 0x%x\n", win, scr->root);

    const char *inst_ext[] = { "VK_KHR_surface", "VK_KHR_xcb_surface" };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "lxrt-x11", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app,
                                 .enabledExtensionCount = 2,
                                 .ppEnabledExtensionNames = inst_ext };
    VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
    PFN_CreateXcbSurface createXcb =
        (PFN_CreateXcbSurface)vkGetInstanceProcAddr(inst, "vkCreateXcbSurfaceKHR");
    if (!createXcb) { printf("sin vkCreateXcbSurfaceKHR\n"); return 1; }
    XcbSurfaceCreateInfo xsi = { .sType = 1000005000, .connection = c, .window = win };
    VkSurfaceKHR surf; CK(createXcb(inst, &xsi, NULL, &surf));
    xcb_intern_atom_reply_t *a = xcb_intern_atom_reply(c, xcb_intern_atom(c, 1, 15, "_STEAMARM_LAYER"), NULL);
    uint32_t ctx = 0;
    if (a) {
        xcb_get_property_reply_t *p = xcb_get_property_reply(c, xcb_get_property(c, 0, win, a->atom, 6, 0, 1), NULL);
        if (p && p->value_len == 1) ctx = *(uint32_t *)xcb_get_property_value(p);
        free(p); free(a);
    }
    printf("VkSurfaceKHR sobre la ventana X; _STEAMARM_LAYER = %u\n", ctx);
    if (!ctx) { printf("== vk x11 surface: MAL (sin propiedad)\n"); return 1; }

    uint32_t ndev = 1; VkPhysicalDevice phys;
    CK(vkEnumeratePhysicalDevices(inst, &ndev, &phys));
    VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(phys, &props);
    printf("GPU: %s\n", props.deviceName);

    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    uint32_t qi = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) {
        VkBool32 sup = 0;
        vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surf, &sup);
        if (sup && (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) { qi = i; break; }
    }
    if (qi == UINT32_MAX) { printf("ninguna cola presenta\n"); return 1; }
    printf("familia de colas %u (grafica + present)\n", qi);

    float prio = 1.0f;
    const char *dev_ext[] = { "VK_KHR_swapchain" };
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = qi, .queueCount = 1,
                                    .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci,
                               .enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_ext };
    VkDevice dev; CK(vkCreateDevice(phys, &dci, NULL, &dev));
    VkQueue queue; vkGetDeviceQueue(dev, qi, 0, &queue);

    VkSurfaceCapabilitiesKHR caps;
    CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps));
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) { ext.width = dw; ext.height = dh; }
    uint32_t nimg = caps.minImageCount + 1;
    if (caps.maxImageCount && nimg > caps.maxImageCount) nimg = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surf, .minImageCount = nimg,
        .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    VkSwapchainKHR sc; CK(vkCreateSwapchainKHR(dev, &sci, NULL, &sc));
    uint32_t got = 0; vkGetSwapchainImagesKHR(dev, sc, &got, NULL);
    VkImage *imgs = calloc(got, sizeof *imgs);
    vkGetSwapchainImagesKHR(dev, sc, &got, imgs);
    printf("swapchain %ux%u, %u imagenes\n", ext.width, ext.height, got);

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

    double *ft = calloc(frames, sizeof *ft);
    double prev = now_ms();
    for (int f = 0; f < frames; f++) {
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acquired, VK_NULL_HANDLE, &idx);
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) { printf("acquire -> %d\n", ar); return 1; }

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
        VkClearColorValue col = { .float32 = { t, 0.25f, 1.0f - t, 1.0f } };
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
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &acquired, .pWaitDstStageMask = &wait,
            .commandBufferCount = 1, .pCommandBuffers = &cmd,
            .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered };
        CK(vkQueueSubmit(queue, 1, &si, fence));

        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .waitSemaphoreCount = 1, .pWaitSemaphores = &rendered,
            .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &idx };
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) { printf("present -> %d\n", pr); return 1; }

        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        vkResetFences(dev, 1, &fence);
        double n = now_ms(); ft[f] = n - prev; prev = n;
    }

    /* Descartar el calentamiento y ordenar para las colas. */
    int skip = frames > 60 ? 30 : 0, n = frames - skip;
    double *v = ft + skip;
    for (int i = 1; i < n; i++) { double k = v[i]; int j = i - 1;
        while (j >= 0 && v[j] > k) { v[j+1] = v[j]; j--; } v[j+1] = k; }
    double sum = 0; for (int i = 0; i < n; i++) sum += v[i];
    int w1 = n / 100 ? n / 100 : 1;
    double s1 = 0; for (int i = n - w1; i < n; i++) s1 += v[i];
    printf("%d frames: mediana %.3f ms (%.1f fps), media %.3f, 1%% low %.3f ms, max %.3f\n",
           n, v[n/2], 1000.0 / v[n/2], sum / n, s1 / w1, v[n-1]);

    vkDeviceWaitIdle(dev);
    printf("presentacion completa, sin VM\n== vk x11 surface: ok\n");
    return 0;
}
