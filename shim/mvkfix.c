// Descriptor-set binding as MoltenVK needs it.
//
// Vulkan lets pDescriptorSets hold VK_NULL_HANDLE where the pipeline layout
// has a set no stage uses (graphics pipeline libraries, independent sets).
// Counter-Strike 2 binds such holes ("1 of 5 sets null, first 0, no
// dynamic offsets"). MoltenVK 1.4.2 reads every set it is given --
// bindDescriptorSets reads set->variableDescriptorCount at +0x3c -- and the
// game crashed in it at the first submit of a match (MEASURED 2026-10-04:
// LXRT_TRACE, then this shim's log). On MoltenVK a bind with holes is made
// as one bind per run of real sets; a hole binds nothing, which leaves that
// set number as it was, and no pipeline uses it. Dynamic offsets belong to
// the real sets in order; with holes and dynamic offsets the bind cannot be
// split without the sets' layouts, so it goes through as it came (logged).
#include <stdint.h>
#include <stddef.h>
#include <vulkan/vulkan_core.h>

extern int dprintf(int, const char *, ...);
extern char *getenv(const char *);
extern const char *lxrt_vk_driver;   // vulkan_shim.c (generated): the driver in use

void lxrt_mvk_vkCmdBindDescriptorSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                                      const VkDescriptorSet *, uint32_t, const uint32_t *);
void lxrt_mvk_vkCmdBindDescriptorSets2(VkCommandBuffer, const VkBindDescriptorSetsInfo *);
void lxrt_mvk_vkCmdBindDescriptorSets2KHR(VkCommandBuffer, const VkBindDescriptorSetsInfo *);

static int on_moltenvk(void)
{
    static int on = -1;
    if (on < 0) {
        const char *a = lxrt_vk_driver, *b = "moltenvk";
        while (*a && *a == *b) a++, b++;
        on = !*a && !*b;
    }
    return on;
}

static int debug_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_VK_DEBUG");
        on = e && *e == '1';
    }
    return on;
}

static int has_holes(const VkDescriptorSet *sets, uint32_t n)
{
    for (uint32_t i = 0; sets && i < n; i++)
        if (!sets[i])
            return 1;
    return 0;
}

static void note_dynamic(uint32_t n, uint32_t ndyn)
{
    static int said;
    if (debug_on() && said++ < 4)
        dprintf(2, "[shim] descriptor bind with holes and %u dynamic offsets (%u sets): not split\n", ndyn, n);
}

void lxrt_inner_vkCmdBindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp, VkPipelineLayout layout,
                                        uint32_t first, uint32_t n, const VkDescriptorSet *sets, uint32_t ndyn,
                                        const uint32_t *dyn)
{
    if (!on_moltenvk() || !has_holes(sets, n)) {
        lxrt_mvk_vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, ndyn, dyn);
        return;
    }
    if (ndyn) {
        note_dynamic(n, ndyn);
        lxrt_mvk_vkCmdBindDescriptorSets(cb, bp, layout, first, n, sets, ndyn, dyn);
        return;
    }
    for (uint32_t i = 0; i < n;) {
        if (!sets[i]) {
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < n && sets[j])
            j++;
        lxrt_mvk_vkCmdBindDescriptorSets(cb, bp, layout, first + i, j - i, sets + i, 0, 0);
        i = j;
    }
}

static void bind2(void (*fn)(VkCommandBuffer, const VkBindDescriptorSetsInfo *), VkCommandBuffer cb,
                  const VkBindDescriptorSetsInfo *info)
{
    if (!on_moltenvk() || !info || !has_holes(info->pDescriptorSets, info->descriptorSetCount)) {
        fn(cb, info);
        return;
    }
    if (info->dynamicOffsetCount) {
        note_dynamic(info->descriptorSetCount, info->dynamicOffsetCount);
        fn(cb, info);
        return;
    }
    VkBindDescriptorSetsInfo part = *info;
    for (uint32_t i = 0; i < info->descriptorSetCount;) {
        if (!info->pDescriptorSets[i]) {
            i++;
            continue;
        }
        uint32_t j = i;
        while (j < info->descriptorSetCount && info->pDescriptorSets[j])
            j++;
        part.firstSet = info->firstSet + i;
        part.descriptorSetCount = j - i;
        part.pDescriptorSets = info->pDescriptorSets + i;
        fn(cb, &part);
        i = j;
    }
}

void lxrt_inner_vkCmdBindDescriptorSets2(VkCommandBuffer cb, const VkBindDescriptorSetsInfo *info)
{
    bind2(lxrt_mvk_vkCmdBindDescriptorSets2, cb, info);
}

void lxrt_inner_vkCmdBindDescriptorSets2KHR(VkCommandBuffer cb, const VkBindDescriptorSetsInfo *info)
{
    bind2(lxrt_mvk_vkCmdBindDescriptorSets2KHR, cb, info);
}
