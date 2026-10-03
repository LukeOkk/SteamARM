/* Steam's web helper makes a Vulkan device, destroys it, and makes the one it
   draws with. The shim's mailbox presenter signals the game's semaphore for
   its first acquire (before any present) on a queue -- and took the first
   queue any device had ever handed out: the destroyed device's. KosmicKrisp
   then crashed in vk_queue_submit_alloc or mtl_residency_set_commit, 13-17 s
   into a Steam start, in most starts of 2026-10-02 (from 13:16, when the
   mailbox began). Here: device A with its queue, destroyed; device B, a FIFO
   swapchain on an X window (the mailbox takes it), and 60 frames whose first
   acquire comes before any present. Run with MallocScribble=1 so that the
   freed device reads as 0x55 bytes. Prints "== vk twodev: ok" or "MAL". */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    printf("FALLO %s -> %d\n== vk twodev: MAL\n", #x, _r); return 1; } } while (0)

static VkDevice make_device(VkPhysicalDevice phys, uint32_t qi, VkQueue *q)
{
    float prio = 1.0f;
    const char *dev_ext[] = { "VK_KHR_swapchain" };
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qi,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .enabledExtensionCount = 1, .ppEnabledExtensionNames = dev_ext };
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &dci, NULL, &dev) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    vkGetDeviceQueue(dev, qi, 0, q);
    return dev;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    if (xcb_connection_has_error(c)) { printf("sin X\n== vk twodev: MAL\n"); return 1; }
    xcb_screen_t *scr = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    uint32_t win = xcb_generate_id(c);
    xcb_create_window(c, 0, win, scr->root, 200, 150, 640, 360, 0, 1, scr->root_visual, 0, NULL);
    xcb_change_property(c, 0, win, 39 /* WM_NAME */, 31 /* STRING */, 8, 10, "vk_twodev ");
    xcb_map_window(c, win);
    xcb_flush(c);

    const char *inst_ext[] = { "VK_KHR_surface", "VK_KHR_xcb_surface" };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "vk_twodev", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
                                 .enabledExtensionCount = 2, .ppEnabledExtensionNames = inst_ext };
    VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));
    PFN_CreateXcbSurface createXcb = (PFN_CreateXcbSurface)vkGetInstanceProcAddr(inst, "vkCreateXcbSurfaceKHR");
    XcbSurfaceCreateInfo xsi = { .sType = 1000005000, .connection = c, .window = win };
    VkSurfaceKHR surf; CK(createXcb(inst, &xsi, NULL, &surf));
    uint32_t ndev = 1; VkPhysicalDevice phys;
    CK(vkEnumeratePhysicalDevices(inst, &ndev, &phys));
    uint32_t qi = 0;
    VkBool32 sup = 0;
    vkGetPhysicalDeviceSurfaceSupportKHR(phys, qi, surf, &sup);
    if (!sup) { printf("la familia 0 no presenta\n== vk twodev: MAL\n"); return 1; }

    /* Device A: a queue handed out, then the device destroyed. */
    VkQueue qa = VK_NULL_HANDLE;
    VkDevice a = make_device(phys, qi, &qa);
    if (!a) { printf("FALLO device A\n== vk twodev: MAL\n"); return 1; }
    vkDestroyDevice(a, NULL);
    printf("device A made and destroyed (queue %p)\n", (void *)qa);

    /* Device B: the one that draws. */
    VkQueue queue = VK_NULL_HANDLE;
    VkDevice dev = make_device(phys, qi, &queue);
    if (!dev) { printf("FALLO device B\n== vk twodev: MAL\n"); return 1; }
    VkSurfaceCapabilitiesKHR caps;
    CK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps));
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) { ext.width = 640; ext.height = 360; }
    uint32_t nimg = caps.minImageCount + 1;
    if (caps.maxImageCount && nimg > caps.maxImageCount) nimg = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = surf, .minImageCount = nimg, .imageFormat = VK_FORMAT_B8G8R8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR, .imageExtent = ext, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE };
    VkSwapchainKHR sc; CK(vkCreateSwapchainKHR(dev, &sci, NULL, &sc));
    uint32_t n = 0; CK(vkGetSwapchainImagesKHR(dev, sc, &n, NULL));
    VkImage imgs[16]; if (n > 16) n = 16; CK(vkGetSwapchainImagesKHR(dev, sc, &n, imgs));

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

    for (int f = 0; f < 60; f++) {
        uint32_t i = 0;
        CK(vkAcquireNextImageKHR(dev, sc, 2000000000ull, acquired, VK_NULL_HANDLE, &i));
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        CK(vkResetCommandBuffer(cmd, 0));
        CK(vkBeginCommandBuffer(cmd, &bi));
        VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = imgs[i],
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        VkClearColorValue col = { .float32 = { (f % 30) / 30.0f, 0.3f, 0.6f, 1.0f } };
        VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        vkCmdClearColorImage(cmd, imgs[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &rr);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        CK(vkEndCommandBuffer(cmd));
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
            .pWaitSemaphores = &acquired, .pWaitDstStageMask = &ws, .commandBufferCount = 1,
            .pCommandBuffers = &cmd, .signalSemaphoreCount = 1, .pSignalSemaphores = &rendered };
        CK(vkQueueSubmit(queue, 1, &si, fence));
        VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
            .pWaitSemaphores = &rendered, .swapchainCount = 1, .pSwapchains = &sc, .pImageIndices = &i };
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr != VK_SUCCESS && pr != VK_SUBOPTIMAL_KHR) { printf("FALLO present -> %d\n== vk twodev: MAL\n", pr); return 1; }
        if (vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull) != VK_SUCCESS) {
            printf("FALLO frame %d: the GPU never finished (the acquire's semaphore was signalled elsewhere)\n== vk twodev: MAL\n", f);
            return 1;
        }
        vkResetFences(dev, 1, &fence);
    }
    CK(vkDeviceWaitIdle(dev));
    vkDestroySwapchainKHR(dev, sc, NULL);
    vkDestroyFence(dev, fence, NULL);
    vkDestroySemaphore(dev, acquired, NULL);
    vkDestroySemaphore(dev, rendered, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    printf("60 frames on device B\n== vk twodev: ok\n");
    return 0;
}
