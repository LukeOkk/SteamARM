// The MetalFX temporal command channel, end to end from a Windows program:
// vulkan-1.dll -> winevulkan -> FEX's Vulkan thunks -> the runtime's shim
// (shim/mfx_temporal.c). A 320-byte vkCmdUpdateBuffer that carries a
// struct sa_mfxt_cmd (shim/mfx_temporal.h) must come back with the status
// the shim wrote into this program's own copy before the call returned, and
// must not reach the buffer; any other update must reach it unchanged.
//
//   1. probe (op PROBE, context 1) in a command buffer of its own pool that
//      is never submitted, as the FFX DLL will probe at ffxCreateContext;
//   2. the same command (context 2) in a command buffer that IS submitted,
//      at offset 0 of a 4 KiB buffer the host filled with 0xCD: the bytes
//      must stay 0xCD (the shim did not forward it);
//   3. control: 256 bytes of ordinary data at offset 1024 must arrive;
//   4. near miss: a command with a version the shim does not know, at
//      offset 2048, must arrive as it was, its status left at 0 (UNSEEN).
//
// Expected today: status NO_DRIVER (no KosmicKrisp entry
// kk_steamarm_upscale_temporal yet) from both probes; OK once the driver
// has the entry. UNSEEN means the command never met this shim.
//
// mingw has no vulkan-1 import library, and no other tests/win program
// calls Vulkan directly (they reach it through DXVK / VKD3D-Proton), so
// vulkan-1.dll (Wine's builtin, which forwards to winevulkan) is loaded at
// run time, as Vulkan's own loader-less samples do.
// Built and run by tests/win/run_mfxt_channel.sh.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "../../shim/mfx_temporal.h"

#define BUF_SIZE 4096
#define CTRL_OFF 1024
#define CTRL_SIZE 256
#define MISS_OFF 2048

static const char *status_name(uint32_t st)
{
    switch (st) {
    case SA_ST_UNSEEN: return "UNSEEN";
    case SA_ST_OK: return "OK";
    case SA_ST_NO_DRIVER: return "NO_DRIVER";
    case SA_ST_BAD_ARGS: return "BAD_ARGS";
    case SA_ST_UNSUPPORTED: return "UNSUPPORTED";
    case SA_ST_NO_SCALER: return "NO_SCALER";
    default: return "?";
    }
}

// Nothing here waits on a window, but a hung driver must not keep the
// probe alive: it ends itself well inside a minute.
static DWORD WINAPI watchdog(LPVOID arg)
{
    (void)arg;
    Sleep(45000);
    printf("== mfxt channel probe: FAIL (watchdog: no result in 45 s)\n");
    fflush(stdout);
    ExitProcess(3);
    return 0;
}

static void fill_cmd(struct sa_mfxt_cmd *c, uint64_t context)
{
    memset(c, 0, sizeof *c);
    c->magic = SA_MFXT_MAGIC;
    c->version = SA_MFXT_VERSION;
    c->size = sizeof *c;
    c->op = SA_OP_PROBE;
    c->status = SA_ST_UNSEEN;
    c->context = context;
    c->color.vk_format = VK_FORMAT_R16G16B16A16_SFLOAT;
    c->render_w = 1280; c->render_h = 720;
    c->upscale_w = 1920; c->upscale_h = 1080;
    c->mv_scale_x = 1.0f; c->mv_scale_y = 1.0f;
    c->pre_exposure = 1.0f;
}

#define FAIL(...) do { printf(__VA_ARGS__); printf("\n== mfxt channel probe: FAIL\n"); return 1; } while (0)
#define CHECK(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) FAIL(#call " -> %d", (int)r_); } while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);

    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (!lib) FAIL("LoadLibrary(vulkan-1.dll) failed: %lu", GetLastError());
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)(void *)GetProcAddress(lib, "vkGetInstanceProcAddr");
    if (!gipa) FAIL("no vkGetInstanceProcAddr in vulkan-1.dll");

#define G(name) PFN_##name name = (PFN_##name)gipa(inst, #name); if (!name) FAIL("no " #name)
    VkInstance inst = VK_NULL_HANDLE;
    G(vkCreateInstance);
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vk_mfxt_channel",
                              .apiVersion = VK_API_VERSION_1_2 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    CHECK(vkCreateInstance(&ici, NULL, &inst));
    G(vkEnumeratePhysicalDevices);
    G(vkGetPhysicalDeviceProperties);
    G(vkGetPhysicalDeviceProperties2);
    G(vkGetPhysicalDeviceQueueFamilyProperties);
    G(vkGetPhysicalDeviceMemoryProperties);
    G(vkCreateDevice);
    G(vkGetDeviceProcAddr);
    G(vkDestroyInstance);
#undef G

    uint32_t npd = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkResult er = vkEnumeratePhysicalDevices(inst, &npd, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !npd) FAIL("no physical device (%d)", (int)er);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    char driver[VK_MAX_DRIVER_NAME_SIZE] = "?";
    if (props.apiVersion >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceDriverProperties dp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &dp };
        vkGetPhysicalDeviceProperties2(pd, &p2);
        snprintf(driver, sizeof driver, "%s", dp.driverName);
    }
    printf("device: %s, driver %s, Vulkan %u.%u\n", props.deviceName, driver,
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion));

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qf[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t family = UINT32_MAX;
    for (uint32_t i = 0; i < nq && family == UINT32_MAX; i++)
        if (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
            family = i;
    if (family == UINT32_MAX) FAIL("no queue family that can transfer");
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = family,
                                    .queueCount = 1, .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));

    // Device functions the way the FFX DLL will take them: vkGetDeviceProcAddr.
#define D(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name); if (!name) FAIL("no " #name)
    D(vkGetDeviceQueue);
    D(vkCreateBuffer);
    D(vkGetBufferMemoryRequirements);
    D(vkAllocateMemory);
    D(vkBindBufferMemory);
    D(vkMapMemory);
    D(vkCreateCommandPool);
    D(vkAllocateCommandBuffers);
    D(vkBeginCommandBuffer);
    D(vkEndCommandBuffer);
    D(vkCmdUpdateBuffer);
    D(vkCmdPipelineBarrier);
    D(vkCreateFence);
    D(vkWaitForFences);
    D(vkQueueSubmit);
    D(vkDestroyFence);
    D(vkDestroyCommandPool);
    D(vkDestroyBuffer);
    D(vkFreeMemory);
    D(vkDestroyDevice);
#undef D
    VkQueue queue;
    vkGetDeviceQueue(dev, family, 0, &queue);

    // The 4 KiB carrier, host-visible so the result can be read back.
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BUF_SIZE,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            mt = i;
    if (mt == UINT32_MAX) FAIL("no host-visible coherent memory for the buffer");
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                 .memoryTypeIndex = mt };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
    CHECK(vkBindBufferMemory(dev, buf, mem, 0));
    unsigned char *map;
    CHECK(vkMapMemory(dev, mem, 0, BUF_SIZE, 0, (void **)&map));
    memset(map, 0xCD, BUF_SIZE);

    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = family };
    VkCommandPool pool, probe_pool;
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &probe_pool));
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb, probe_cb;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
    cai.commandPool = probe_pool;
    CHECK(vkAllocateCommandBuffers(dev, &cai, &probe_cb));
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };

    // 1. The probe, recorded and never submitted.
    struct sa_mfxt_cmd probe;
    fill_cmd(&probe, 1);
    CHECK(vkBeginCommandBuffer(probe_cb, &cbi));
    vkCmdUpdateBuffer(probe_cb, buf, 0, sizeof probe, &probe);
    CHECK(vkEndCommandBuffer(probe_cb));
    printf("probe (unsubmitted, ctx 1): status %u %s (data at %p)\n", probe.status, status_name(probe.status),
           (void *)&probe);
    if (probe.status == SA_ST_OK)
        printf("probe caps 0x%x driver %u max scale %.2f\n", probe.caps_out, probe.driver_out, probe.max_scale_out);

    // 2.-4. In one submitted command buffer, between the barriers the DLL uses.
    struct sa_mfxt_cmd inline_cmd, miss;
    fill_cmd(&inline_cmd, 2);
    fill_cmd(&miss, 3);
    miss.version = 99;
    uint32_t ctrl[CTRL_SIZE / 4];
    for (unsigned i = 0; i < CTRL_SIZE / 4; i++)
        ctrl[i] = 0xA5000000u ^ (i * 0x01010101u);
    VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                           .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    CHECK(vkBeginCommandBuffer(cb, &cbi));
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdUpdateBuffer(cb, buf, 0, sizeof inline_cmd, &inline_cmd);
    uint32_t inline_status = inline_cmd.status;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    vkCmdUpdateBuffer(cb, buf, CTRL_OFF, CTRL_SIZE, ctrl);
    vkCmdUpdateBuffer(cb, buf, MISS_OFF, sizeof miss, &miss);
    VkMemoryBarrier hb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                           .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &hb, 0, NULL, 0, NULL);
    CHECK(vkEndCommandBuffer(cb));
    printf("command (submitted, ctx 2): status %u %s\n", inline_status, status_name(inline_status));
    printf("near miss (version 99, ctx 3): status %u %s\n", miss.status, status_name(miss.status));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, NULL, &fence));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));

    // Read back.
    unsigned cmd_kept = 0;
    for (unsigned i = 0; i < sizeof inline_cmd; i++)
        cmd_kept += map[i] == 0xCD;
    int ctrl_ok = !memcmp(map + CTRL_OFF, ctrl, CTRL_SIZE);
    int miss_ok = !memcmp(map + MISS_OFF, &miss, sizeof miss);
    unsigned untouched = 0;
    for (unsigned i = 0; i < BUF_SIZE; i++)
        if ((i >= CTRL_OFF && i < CTRL_OFF + CTRL_SIZE) || (i >= MISS_OFF && i < MISS_OFF + sizeof miss))
            continue;
        else
            untouched += map[i] == 0xCD;
    unsigned expect_untouched = BUF_SIZE - CTRL_SIZE - (unsigned)sizeof miss;
    printf("buffer: command bytes at 0 still 0xCD: %u/%u (%s)\n", cmd_kept, (unsigned)sizeof inline_cmd,
           cmd_kept == sizeof inline_cmd ? "not forwarded" : "forwarded to the driver");
    printf("buffer: control 256 bytes at %d arrived: %s\n", CTRL_OFF, ctrl_ok ? "yes" : "NO");
    printf("buffer: near miss 320 bytes at %d arrived as sent: %s\n", MISS_OFF, miss_ok ? "yes" : "NO");
    printf("buffer: bytes nobody wrote still 0xCD: %u/%u\n", untouched, expect_untouched);

    vkDestroyFence(dev, fence, NULL);
    vkDestroyCommandPool(dev, probe_pool, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyBuffer(dev, buf, NULL);
    vkFreeMemory(dev, mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);

    int seen = (probe.status == SA_ST_NO_DRIVER || probe.status == SA_ST_OK) && inline_status == probe.status;
    int ok = seen && miss.status == SA_ST_UNSEEN && cmd_kept == sizeof inline_cmd && ctrl_ok && miss_ok &&
             untouched == expect_untouched;
    printf("== mfxt channel probe: %s (status %s)\n", ok ? "ok" : "FAIL", status_name(probe.status));
    return ok ? 0 : 1;
}
