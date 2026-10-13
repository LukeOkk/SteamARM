// The video memory a game sees through the shim (shim/memcap.c, the launcher's
// VRAM setting, LXRT_VK_MAX_VRAM_MB): the heaps of vkGetPhysicalDeviceMemory-
// Properties and ...2, and VK_EXT_memory_budget's budget and usage, before and
// after this program allocates device memory. tests/elf/run_vk_arm64.sh builds
// and runs it on MoltenVK and KosmicKrisp.
//
//   vk_memcap [alloc-MiB]          print, allocate (default 256 MiB), print again
//   vk_memcap hold N [MiB]         keep N allocations of MiB (default 1) each, with
//                                  an image bound to each, for 20 s: vmmap(1) on the
//                                  runtime process from the host counts the
//                                  "IOAccelerator" regions they cost
//
// With LXRT_VK_MAX_VRAM_MB=N set, every DEVICE_LOCAL heap must be at most N MiB
// in both queries, its budget at most N MiB and above its usage, and
// allocating must not widen the gap between budget and usage. Without the cap
// the driver's answers are printed and only checked for being there.
// Prints "== vk_memcap: ok" when every check holds.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define FAIL(...) do { printf(__VA_ARGS__); printf("\n== vk_memcap: FAIL\n"); return 1; } while (0)
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) FAIL("%s -> %d", #x, r_); } while (0)

static const char *mib(VkDeviceSize b)
{
    static char buf[4][32];
    static unsigned k;
    char *s = buf[k++ & 3];
    snprintf(s, 32, "%.1f MiB", (double)b / 1048576.0);
    return s;
}

static void query(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *p2, VkPhysicalDeviceMemoryBudgetPropertiesEXT *b)
{
    memset(b, 0, sizeof *b);
    b->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT;
    memset(p2, 0, sizeof *p2);
    p2->sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2;
    p2->pNext = b;
    vkGetPhysicalDeviceMemoryProperties2(pd, p2);
}

static void show(const char *when, const VkPhysicalDeviceMemoryProperties *p, const VkPhysicalDeviceMemoryBudgetPropertiesEXT *b)
{
    for (uint32_t i = 0; i < p->memoryHeapCount; i++)
        printf("%s heap[%u] size %s%s budget %s usage %s\n", when, i, mib(p->memoryHeaps[i].size),
               p->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT ? " device-local" : "",
               mib(b->heapBudget[i]), mib(b->heapUsage[i]));
}

int main(int argc, char **argv)
{
    setvbuf(stdout, 0, _IONBF, 0);
    int hold = argc > 1 && !strcmp(argv[1], "hold");
    unsigned count = hold ? (argc > 2 ? (unsigned)atoi(argv[2]) : 100) : 1;
    VkDeviceSize each = (VkDeviceSize)(hold ? (argc > 3 ? atoi(argv[3]) : 1) : (argc > 1 ? atoi(argv[1]) : 256)) << 20;
    if (!count || !each)
        FAIL("bad arguments");
    const char *e = getenv("LXRT_VK_MAX_VRAM_MB");
    VkDeviceSize cap = e && atol(e) > 0 ? (VkDeviceSize)atol(e) << 20 : 0;
    printf("LXRT_VK_MAX_VRAM_MB=%s (cap %s)\n", e ? e : "", cap ? mib(cap) : "none");

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "vk_memcap", 1, 0, 0, VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 0, 0, &app };
    VkInstance inst;
    CHECK(vkCreateInstance(&ici, 0, &inst));
    uint32_t n = 1;
    VkPhysicalDevice pd;
    vkEnumeratePhysicalDevices(inst, &n, &pd);
    if (!n)
        FAIL("no device");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device %s (type %d)\n", props.deviceName, (int)props.deviceType);

    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(pd, 0, &ne, 0);
    VkExtensionProperties *ext = calloc(ne, sizeof *ext);
    vkEnumerateDeviceExtensionProperties(pd, 0, &ne, ext);
    int has_budget = 0;
    for (uint32_t i = 0; i < ne; i++)
        if (!strcmp(ext[i].extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME))
            has_budget = 1;
    free(ext);
    printf("VK_EXT_memory_budget: %s\n", has_budget ? "yes" : "no");

    VkPhysicalDeviceMemoryProperties p1;
    vkGetPhysicalDeviceMemoryProperties(pd, &p1);
    VkPhysicalDeviceMemoryProperties2 p2;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT b0, b1;
    query(pd, &p2, &b0);
    const VkPhysicalDeviceMemoryProperties *p = &p2.memoryProperties;
    if (!p->memoryHeapCount || p->memoryHeapCount != p1.memoryHeapCount)
        FAIL("heap counts %u (v1) and %u (v2)", p1.memoryHeapCount, p->memoryHeapCount);
    for (uint32_t i = 0; i < p->memoryHeapCount; i++)
        if (p1.memoryHeaps[i].size != p->memoryHeaps[i].size)
            FAIL("heap[%u] size %s (v1) differs from %s (v2)", i, mib(p1.memoryHeaps[i].size), mib(p->memoryHeaps[i].size));
    show("before", p, &b0);

    // Device-local memory the host can write (KosmicKrisp's only type;
    // MoltenVK's shared type), else device-local alone, left untouched.
    uint32_t type = ~0u, heap = 0, mappable = 0;
    for (uint32_t i = 0; i < p->memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = p->memoryTypes[i].propertyFlags;
        if (!(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || (type != ~0u && mappable))
            continue;
        if (type == ~0u || (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
            type = i, heap = p->memoryTypes[i].heapIndex, mappable = !!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    }
    if (type == ~0u)
        FAIL("no device-local memory type");

    float prio = 1;
    VkDeviceQueueCreateInfo q = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 0, 0, 0, 1, &prio };
    const char *dext[] = { VK_EXT_MEMORY_BUDGET_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 0, 0, 1, &q, 0, 0, has_budget ? 1 : 0, dext };
    VkDevice dev;
    CHECK(vkCreateDevice(pd, &dci, 0, &dev));

    VkDeviceMemory *mem = calloc(count, sizeof *mem);
    VkImage *img = calloc(count, sizeof *img);
    for (unsigned i = 0; i < count; i++) {
        VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, each, type };
        CHECK(vkAllocateMemory(dev, &ai, 0, &mem[i]));
        if (mappable) {
            void *map = 0;
            CHECK(vkMapMemory(dev, mem[i], 0, each, 0, &map));
            memset(map, 1, (size_t)each);   // resident, as a game's uploads make it
            vkUnmapMemory(dev, mem[i]);
        }
        if (hold) {
            VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, 0, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
                                     { 128, 128, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
                                     VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE,
                                     0, 0, VK_IMAGE_LAYOUT_UNDEFINED };
            CHECK(vkCreateImage(dev, &ic, 0, &img[i]));
            VkMemoryRequirements mr;
            vkGetImageMemoryRequirements(dev, img[i], &mr);
            if (mr.size <= each && (mr.memoryTypeBits & (1u << type)))
                CHECK(vkBindImageMemory(dev, img[i], mem[i], 0));
        }
    }
    query(pd, &p2, &b1);
    show("after", p, &b1);
    printf("allocated %u x %s in heap %u\n", count, mib(each), heap);

    if (hold) {
        printf("holding %u allocations for 20 s (pid %d)\n", count, (int)getpid());
        sleep(20);
    }
    for (unsigned i = 0; i < count; i++) {
        if (img[i])
            vkDestroyImage(dev, img[i], 0);
        vkFreeMemory(dev, mem[i], 0);
    }
    vkDestroyDevice(dev, 0);

    for (uint32_t i = 0; i < p->memoryHeapCount; i++) {
        int local = p->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT;
        if (has_budget && !b0.heapBudget[i])
            FAIL("heap[%u]: no budget", i);
        if (!cap || !local)
            continue;
        if (p->memoryHeaps[i].size > cap)
            FAIL("heap[%u] size %s over the cap %s", i, mib(p->memoryHeaps[i].size), mib(cap));
        if (!has_budget)
            continue;
        if (b0.heapBudget[i] > cap || b1.heapBudget[i] > cap)
            FAIL("heap[%u] budget %s / %s over the cap %s", i, mib(b0.heapBudget[i]), mib(b1.heapBudget[i]), mib(cap));
        if (b0.heapUsage[i] < cap && b0.heapBudget[i] <= b0.heapUsage[i])
            FAIL("heap[%u] budget %s leaves no room over usage %s", i, mib(b0.heapBudget[i]), mib(b0.heapUsage[i]));
        if (i == heap && b1.heapUsage[i] < b0.heapUsage[i] + (VkDeviceSize)count * each)
            FAIL("heap[%u] usage %s -> %s does not show the %u x %s allocated", i, mib(b0.heapUsage[i]),
                 mib(b1.heapUsage[i]), count, mib(each));
        VkDeviceSize room0 = b0.heapBudget[i] > b0.heapUsage[i] ? b0.heapBudget[i] - b0.heapUsage[i] : 0;
        VkDeviceSize room1 = b1.heapBudget[i] > b1.heapUsage[i] ? b1.heapBudget[i] - b1.heapUsage[i] : 0;
        if (room1 > room0)
            FAIL("heap[%u] room grew from %s to %s after allocating", i, mib(room0), mib(room1));
    }
    vkDestroyInstance(inst, 0);
    printf("== vk_memcap: ok\n");
    return 0;
}
