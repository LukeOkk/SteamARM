// Video memory seen by games (launcher: Sistema -> VRAM, LXRT_VK_MAX_VRAM_MB).
//
// Apple Silicon has one memory. Both drivers report one device-local heap
// the size of the Mac's RAM (MEASURED 2026-10-06, M4 16 GB: 16384 MiB on
// MoltenVK 1.4 and on KosmicKrisp 26.2.4; tests/elf/vk_memcap.c). DXVK makes
// DXGI's DedicatedVideoMemory of the largest device-local heap, capped again
// by dxgi.maxDeviceMemory (scripts/settings-env.py), and QueryVideoMemoryInfo's
// Budget of VK_EXT_memory_budget's heapBudget, which dxgi.maxDeviceMemory does
// not touch (dxvk/src/dxgi/dxgi_adapter.cpp); VKD3D-Proton and native Vulkan
// games read the same two. Capping both here caps what every one of them sees.
//
// The budget: MoltenVK answers Metal's recommendedMaxWorkingSetSize (12124 MiB
// here) whatever is in use. KosmicKrisp answers recommendedMaxWorkingSetSize
// minus all the Mac has in use (everything but free and inactive pages) plus
// its own allocations (kk_get_sysmem_heap_budget): with less than 4.2 GB free
// or inactive -- an idle desktop here -- that is its usage and nothing more
// (MEASURED: budget 0.1 MiB at usage 0.1 MiB, 257.0 at 257.0). DXVK then
// tells the game Budget == CurrentUsage for ever; on unified memory DXVK
// itself never refuses an allocation over budget (DxvkMemoryHeap::
// enforceBudget is for discrete GPUs), so the budget is only what the game
// is told. A budget that leaves no room over the usage is replaced by the
// cap, as is one above it.
#include <stdint.h>
#include <vulkan/vulkan.h>

extern char *getenv(const char *);   // the guest's libc (the shim is -nostdlib)

void lxrt_mvk_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
void lxrt_mvk_vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2 *);
void lxrt_mvk_vkGetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties2 *);

static VkDeviceSize cap_bytes(void)
{
    static int done;
    static VkDeviceSize cap;
    if (!done) {
        const char *e = getenv("LXRT_VK_MAX_VRAM_MB");
        long mb = 0;
        for (; e && *e >= '0' && *e <= '9'; e++)
            mb = mb * 10 + (*e - '0');
        cap = mb > 0 ? (VkDeviceSize)mb << 20 : 0;
        done = 1;
    }
    return cap;
}

// The cap on every device-local heap's size and, when `budget` is given
// (VK_EXT_memory_budget's heapBudget, with `usage` its heapUsage), on its
// budget: at most the cap, and the cap itself when the driver's leaves no
// room over the usage.
static void cap_heaps(VkPhysicalDeviceMemoryProperties *p, VkDeviceSize *budget, const VkDeviceSize *usage)
{
    VkDeviceSize cap = cap_bytes();
    if (!cap)
        return;
    for (uint32_t i = 0; i < p->memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++) {
        if (!(p->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
            continue;
        if (p->memoryHeaps[i].size > cap)
            p->memoryHeaps[i].size = cap;
        if (budget && (budget[i] > cap || budget[i] <= usage[i]))
            budget[i] = cap;
    }
}

static void cap_chain(VkPhysicalDeviceMemoryProperties2 *p)
{
    VkPhysicalDeviceMemoryBudgetPropertiesEXT *b = NULL;
    for (VkBaseOutStructure *s = (VkBaseOutStructure *)p->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
            b = (VkPhysicalDeviceMemoryBudgetPropertiesEXT *)s;
    cap_heaps(&p->memoryProperties, b ? b->heapBudget : NULL, b ? b->heapUsage : NULL);
}

void lxrt_inner_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *p)
{
    lxrt_mvk_vkGetPhysicalDeviceMemoryProperties(pd, p);
    cap_heaps(p, NULL, NULL);
}

void lxrt_inner_vkGetPhysicalDeviceMemoryProperties2(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *p)
{
    lxrt_mvk_vkGetPhysicalDeviceMemoryProperties2(pd, p);
    cap_chain(p);
}

void lxrt_inner_vkGetPhysicalDeviceMemoryProperties2KHR(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *p)
{
    lxrt_mvk_vkGetPhysicalDeviceMemoryProperties2KHR(pd, p);
    cap_chain(p);
}
