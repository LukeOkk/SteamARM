// What DXVK does first with a Vulkan device, without a window: instance,
// physical device with pNext property/feature chains, device, queue, host-
// visible memory mapped and written, a command buffer that fills a buffer,
// submit with a fence, and the result read back through the mapping.
//
// Built for x86-64 and i386 (tests/elf/run_vk_device.sh): the i386 build is
// the 32-bit Vulkan thunk path (handles, pointers and mapped memory of a
// 32-bit guest living at FEX's guest base).
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    printf("FAIL %s -> %d\n", #x, r_); return 1; } } while (0)

int main(void)
{
    setvbuf(stdout, 0, _IONBF, 0);
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_device", 1, 0, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, 0, &inst));
    printf("instance %p\n", (void *)inst);

    uint32_t n = 0;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, 0));
    VkPhysicalDevice pds[4];
    n = n > 4 ? 4 : n;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, pds));
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceVulkan12Properties p12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceVulkan11Properties p11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, &p12 };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p11 };
    vkGetPhysicalDeviceProperties2(pd, &p2);
    printf("device %s, driver %s, api %u.%u\n", p2.properties.deviceName, p12.driverName,
           VK_API_VERSION_MAJOR(p2.properties.apiVersion), VK_API_VERSION_MINOR(p2.properties.apiVersion));

    VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12 };
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    printf("features: timelineSemaphore %u, dynamicRendering %u, synchronization2 %u\n",
           f12.timelineSemaphore, f13.dynamicRendering, f13.synchronization2);

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, 0);
    VkQueueFamilyProperties qf[8];
    nq = nq > 8 ? 8 : nq;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t fam = 0;
    while (fam < nq && !(qf[fam].queueFlags & VK_QUEUE_GRAPHICS_BIT)) fam++;
    printf("queue families %u, graphics family %u\n", nq, fam);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, fam, 1, &prio };
    VkPhysicalDeviceVulkan13Features e13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    e13.synchronization2 = f13.synchronization2;
    VkPhysicalDeviceVulkan12Features e12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13 };
    e12.timelineSemaphore = f12.timelineSemaphore;
    VkPhysicalDeviceFeatures2 ef = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &e12 };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &ef, 0, 1, &qci };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, 0, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, fam, 0, &q);
    printf("device %p, queue %p\n", (void *)dev, (void *)q);

    const VkDeviceSize size = 1 << 20;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, 0, 0, size,
                               VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE };
    VkBuffer buf;
    CHECK(vkCreateBuffer(dev, &bci, 0, &buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, buf, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t h = 0; h < mp.memoryHeapCount; h++)
        if (mp.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
            printf("device-local heap %u: %llu MiB\n", h, (unsigned long long)(mp.memoryHeaps[h].size >> 20));
    uint32_t mt = 0;
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    while (mt < mp.memoryTypeCount && !((mr.memoryTypeBits >> mt & 1) && (mp.memoryTypes[mt].propertyFlags & want) == want))
        mt++;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, mt };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(dev, &mai, 0, &mem));
    CHECK(vkBindBufferMemory(dev, buf, mem, 0));
    uint32_t *map = 0;
    CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));
    printf("mapped %p (memory type %u)\n", (void *)map, mt);
    memset(map, 0x11, size);

    // Device-level commands the way DXVK gets them.
    PFN_vkCmdFillBuffer fill = (PFN_vkCmdFillBuffer)vkGetDeviceProcAddr(dev, "vkCmdFillBuffer");
    if (!fill) { printf("FAIL vkGetDeviceProcAddr(vkCmdFillBuffer)\n"); return 1; }

    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0, 0, fam };
    VkCommandPool pool;
    CHECK(vkCreateCommandPool(dev, &cpci, 0, &pool));
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, pool,
                                         VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2 };
    VkCommandBuffer cbs[2];
    CHECK(vkAllocateCommandBuffers(dev, &cbai, cbs));
    VkCommandBufferBeginInfo cbbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0,
                                      VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    CHECK(vkBeginCommandBuffer(cbs[0], &cbbi));
    fill(cbs[0], buf, 0, size / 2, 0xcafef00d);
    CHECK(vkEndCommandBuffer(cbs[0]));
    CHECK(vkBeginCommandBuffer(cbs[1], &cbbi));
    fill(cbs[1], buf, size / 2, size / 2, 0x0badc0de);
    CHECK(vkEndCommandBuffer(cbs[1]));

    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(dev, &fci, 0, &fence));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, 0, 0, 0, 0, 2, cbs };
    CHECK(vkQueueSubmit(q, 1, &si, fence));
    CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
    uint32_t a = map[0], b = map[size / 8], c = map[size / 4 - 1];
    printf("readback 0x%08x 0x%08x 0x%08x\n", a, b, c);

    vkDestroyFence(dev, fence, 0);
    vkFreeCommandBuffers(dev, pool, 2, cbs);
    vkDestroyCommandPool(dev, pool, 0);
    vkUnmapMemory(dev, mem);
    vkDestroyBuffer(dev, buf, 0);
    vkFreeMemory(dev, mem, 0);
    vkDestroyDevice(dev, 0);
    vkDestroyInstance(inst, 0);
    int ok = a == 0xcafef00d && b == 0x0badc0de && c == 0x0badc0de;
    printf(ok ? "== vk_device: ok\n" : "== vk_device: FAIL\n");
    return !ok;
}
