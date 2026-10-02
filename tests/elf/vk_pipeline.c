// A Vulkan graphics pipeline compiled by a native program: the step where
// the driver hands MSL to Metal's shader compiler (MTLCompilerService, an XPC
// service). Chromium's GPU process in the Steam Frame root could not reach it
// ("Unable to reach MTLCompilerService ... lookup with error 3"), so every
// pipeline failed. tests/elf/run_vk_arm64.sh builds and runs it.
// Prints "== vk_pipeline: ok" when vkCreateGraphicsPipelines succeeds.
// "vk_pipeline fork": then fork and exec itself, as Chromium starts its GPU
// process from a browser that already used Vulkan. "vk_pipeline zygote":
// fork first and compile in the child without exec, as Chromium's
// unsandboxed zygote starts the GPU process.
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vulkan/vulkan.h>
#include "../../shim/scaler_spv.h"

// A copy of the fragment shader with its first float constant changed to a
// value from the clock: new MSL source each run, so Metal compiles it rather
// than finding it in its shader cache.
static uint32_t fresh_rcas[sizeof spv_rcas / 4];
static void make_fresh(void)
{
    memcpy(fresh_rcas, spv_rcas, sizeof spv_rcas);
    uint32_t floats[16], nf = 0, n = sizeof spv_rcas / 4;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    for (uint32_t i = 5; i < n;) {
        uint32_t wc = fresh_rcas[i] >> 16, op = fresh_rcas[i] & 0xffff;
        if (!wc)
            break;
        if (op == 22 && nf < 16 && fresh_rcas[i + 2] == 32)
            floats[nf++] = fresh_rcas[i + 1];
        if (op == 43)
            for (uint32_t k = 0; k < nf; k++)
                if (fresh_rcas[i + 1] == floats[k]) {
                    float v = 0.001f + (float)(ts.tv_nsec % 1000000) * 1e-9f;
                    memcpy(&fresh_rcas[i + 3], &v, 4);
                    return;
                }
        i += wc;
    }
}

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s -> %d\n== vk_pipeline: FAIL\n", #x, r_); return 1; } } while (0)

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "zygote")) {
        fflush(stdout);
        pid_t pid = fork();
        if (pid > 0) {
            int st = 0;
            waitpid(pid, &st, 0);
            return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
        }
        argc = 1;
    }
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_pipeline", 1, 0, 0, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n) { printf("no device\n== vk_pipeline: FAIL\n"); return 1; }
    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &q };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, 0, &dev));
    VkAttachmentDescription a = { 0, VK_FORMAT_B8G8R8A8_UNORM, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, 0, 0, 1, &ref };
    VkRenderPassCreateInfo rpi = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, 0, 0, 1, &a, 1, &sp };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(dev, &rpi, 0, &rp));
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, 0, 0, 1, &b };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(dev, &dli, 0, &dsl));
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, 0, 0, 1, &dsl, 1, &pr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &pli, 0, &pl));
    VkShaderModuleCreateInfo vi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof spv_fullscreen, spv_fullscreen };
    make_fresh();
    VkShaderModuleCreateInfo fi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, 0, 0, sizeof fresh_rcas, fresh_rcas };
    VkShaderModule vs, fs;
    CHECK(vkCreateShaderModule(dev, &vi, 0, &vs));
    CHECK(vkCreateShaderModule(dev, &fi, 0, &fs));
    VkPipelineShaderStageCreateInfo st[2] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, 0, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" },
    };
    VkPipelineVertexInputStateCreateInfo vin = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, 0, 0,
                                                  VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, 64, 64, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { 64, 64 } };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, 0, 0, 1, &vp, 1, &sc };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, 0, 0,
                                                VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, 0, 0, 0, 0, 1, &ba };
    VkGraphicsPipelineCreateInfo gi = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, 0, 0, 2, st, &vin, &ia, 0, &vps,
                                        &rs, &ms, 0, &cb, 0, pl, rp, 0 };
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(dev, 0, 1, &gi, 0, &p));
    printf("== vk_pipeline: ok%s\n", argc > 1 ? " (parent)" : "");
    fflush(stdout);
    if (argc > 1 && !strcmp(argv[1], "fork")) {
        pid_t pid = fork();
        if (pid == 0) {
            execl("/proc/self/exe", argv[0], (char *)0);
            _exit(127);
        }
        int st = 0;
        waitpid(pid, &st, 0);
        return WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st);
    }
    return 0;
}
