/* Ruta de render completa sin VM: render pass, shaders SPIR-V que MoltenVK
   traduce a MSL, pipeline grafico, y un triangulo dibujado por frame. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "lxrt_host.h"
#include "tri_spv.h"

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

    void *layer = lxrt_host_window(1280, 720, "lxrt triangle");
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
        .imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR, .clipped = VK_TRUE };
    VkSwapchainKHR sc; CK(vkCreateSwapchainKHR(dev, &sci, NULL, &sc));
    uint32_t got = 0; vkGetSwapchainImagesKHR(dev, sc, &got, NULL);
    VkImage *imgs = calloc(got, sizeof *imgs);
    vkGetSwapchainImagesKHR(dev, sc, &got, imgs);
    printf("swapchain %ux%u, %u imagenes\n", ext.width, ext.height, got);

    /* ---- vistas, render pass, framebuffers ---- */
    VkImageView *views = calloc(got, sizeof *views);
    for (uint32_t i = 0; i < got; i++) {
        VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = imgs[i], .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = VK_FORMAT_B8G8R8A8_UNORM,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        CK(vkCreateImageView(dev, &vci, NULL, &views[i]));
    }
    VkAttachmentDescription att = { .format = VK_FORMAT_B8G8R8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkSubpassDependency dep = { .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att,
        .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 1, .pDependencies = &dep };
    VkRenderPass rp; CK(vkCreateRenderPass(dev, &rpci, NULL, &rp));

    VkFramebuffer *fbs = calloc(got, sizeof *fbs);
    for (uint32_t i = 0; i < got; i++) {
        VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = rp, .attachmentCount = 1, .pAttachments = &views[i],
            .width = ext.width, .height = ext.height, .layers = 1 };
        CK(vkCreateFramebuffer(dev, &fbi, NULL, &fbs[i]));
    }

    /* ---- shaders: SPIR-V que MoltenVK convierte a MSL ---- */
    VkShaderModuleCreateInfo smv = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof k_vert_spv, .pCode = k_vert_spv };
    VkShaderModuleCreateInfo smf = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof k_frag_spv, .pCode = k_frag_spv };
    VkShaderModule vs, fs;
    CK(vkCreateShaderModule(dev, &smv, NULL, &vs));
    CK(vkCreateShaderModule(dev, &smf, NULL, &fs));
    printf("modulos SPIR-V creados (%zu + %zu bytes)\n", sizeof k_vert_spv, sizeof k_frag_spv);

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" } };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, (float)ext.width, (float)ext.height, 0.0f, 1.0f };
    VkRect2D sc2 = { {0,0}, ext };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc2 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout pl; CK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
        .pInputAssemblyState = &ia, .pViewportState = &vps,
        .pRasterizationState = &rs, .pMultisampleState = &ms,
        .pColorBlendState = &cb, .layout = pl, .renderPass = rp, .subpass = 0 };
    VkPipeline pipe;
    double tpipe0 = now_ms();
    CK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe));
    printf("pipeline grafico compilado en %.1f ms (SPIR-V -> MSL -> Metal)\n", now_ms() - tpipe0);

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
        float t = (float)f / (float)frames;
        VkClearValue clear = { .color = { .float32 = { 0.05f, 0.05f + 0.15f * t, 0.12f, 1.0f } } };
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = rp, .framebuffer = fbs[idx],
            .renderArea = { {0,0}, ext }, .clearValueCount = 1, .pClearValues = &clear };
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);

        VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
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
    printf("triangulo dibujado %d veces, sin VM\n", frames);
    return 0;
}
