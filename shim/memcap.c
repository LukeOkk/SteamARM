// Video memory seen by games (launcher: Sistema -> VRAM, LXRT_VK_MAX_VRAM_MB).
//
// On Apple Silicon the GPU has no memory of its own: MoltenVK reports one
// device-local heap sized from Metal's recommended working set, which is most
// of the Mac's RAM. DXVK and VKD3D-Proton size their caches and the budget
// they report to games (DXGI VideoMemory, QueryVideoMemoryInfo) from the heap
// size and VK_EXT_memory_budget. Capping both here caps them all; DXVK also
// gets dxgi.maxDeviceMemory from scripts/settings-env.py.
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

static void cap_heaps(VkPhysicalDeviceMemoryProperties *p, VkDeviceSize *budget)
{
    VkDeviceSize cap = cap_bytes();
    if (!cap)
        return;
    for (uint32_t i = 0; i < p->memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; i++) {
        if (!(p->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT))
            continue;
        if (p->memoryHeaps[i].size > cap)
            p->memoryHeaps[i].size = cap;
        if (budget && budget[i] > cap)
            budget[i] = cap;
    }
}

static void cap_chain(VkPhysicalDeviceMemoryProperties2 *p)
{
    VkDeviceSize *budget = NULL;
    for (VkBaseOutStructure *s = (VkBaseOutStructure *)p->pNext; s; s = s->pNext)
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT)
            budget = ((VkPhysicalDeviceMemoryBudgetPropertiesEXT *)s)->heapBudget;
    cap_heaps(&p->memoryProperties, budget);
}

void lxrt_inner_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *p)
{
    lxrt_mvk_vkGetPhysicalDeviceMemoryProperties(pd, p);
    cap_heaps(p, NULL);
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
