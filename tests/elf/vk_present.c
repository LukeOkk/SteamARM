/* Swapchain end to end: ventana del runtime -> VkSurfaceKHR sobre CAMetalLayer
   -> swapchain -> acquire/clear/present. Sin VM y sin copia de framebuffer. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lxrt_host.h"

/* Declarada a mano: el vulkan_metal.h de Linux exige tipos de Metal. */
#define SType_MetalSurface 1000217000
struct MetalSurfaceCreateInfo {
    VkStructureType sType; uint32_t _p; const void *pNext;
    VkFlags flags; uint32_t _p2; const void *pLayer;
};
typedef VkResult (VKAPI_PTR *PFN_CreateMetalSurface)(VkInstance,
        const struct MetalSurfaceCreateInfo *, const VkAllocationCallbacks *, VkSurfaceKHR *);

#define CK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    printf("FALLO %s -> %d\n", #x, _r); return 1; } } while (0)

static double now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1000.0 + t.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int frames = argc > 1 ? atoi(argv[1]) : 240;

    void *layer = lxrt_host_window(1280, 720, "lxrt swapchain");
    if (!layer) { printf("sin ventana\n"); return 1; }
    uint32_t dw = 0, dh = 0;
    lxrt_host_drawable_size(&dw, &dh);
    printf("CAMetalLayer %p, drawable %ux%u\n", layer, dw, dh);

    const char *inst_ext[] = { "VK_KHR_surface", "VK_EXT_metal_surface" };
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "lxrt", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app,
                                 .enabledExtensionCount = 2,
                                 .ppEnabledExtensionNames = inst_ext };
    VkInstance inst; CK(vkCreateInstance(&ici, NULL, &inst));

    PFN_CreateMetalSurface createMetal =
        (PFN_CreateMetalSurface)vkGetInstanceProcAddr(inst, "vkCreateMetalSurfaceEXT");
    if (!createMetal) { printf("sin vkCreateMetalSurfaceEXT\n"); return 1; }
    struct MetalSurfaceCreateInfo msi;
    memset(&msi, 0, sizeof msi);
    msi.sType = SType_MetalSurface; msi.pLayer = layer;
    VkSurfaceKHR surf; CK(createMetal(inst, &msi, NULL, &surf));
    printf("VkSurfaceKHR creada sobre el CAMetalLayer\n");

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
    printf("presentacion completa, sin VM\n");
    return 0;
}
