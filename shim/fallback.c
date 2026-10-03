// Compute pipelines MoltenVK cannot compile.
//
// MoltenVK translates SPIR-V to Metal through SPIRV-Cross, and some valid
// shaders come out as MSL Metal rejects. VKD3D-Proton creates its
// DirectStorage GPU-decompression pipelines while creating the device, and
// one of them makes an atomic on a vector component
// (atomicAdd(region_buffer.region_count.x, 1)); SPIRV-Cross emits
// "&region_buffer->region_count[0u]", and Metal says "address of vector
// element requested" (MEASURED). vkCreateComputePipelines failed, and with it
// D3D12CreateDevice for every D3D12 program.
//
// A compute pipeline that does not compile is replaced by an empty one with
// the same layout, and the replacement is reported on stderr. The device is
// created; only the work of that one pipeline is missing (here: DirectStorage
// GPU decompression, which few games use). The proper fix is in SPIRV-Cross.
// LXRT_VK_NO_STUB_PIPELINES=1 turns this off.
#include <stdint.h>
#include <stddef.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

extern char *getenv(const char *);
extern int dprintf(int, const char *, ...);

static const uint32_t k_empty_comp_spv[] = {
	0x07230203,0x00010000,0x0008000b,0x0000000a,0x00000000,0x00020011,0x00000001,0x0006000b,
	0x00000001,0x4c534c47,0x6474732e,0x3035342e,0x00000000,0x0003000e,0x00000000,0x00000001,
	0x0005000f,0x00000005,0x00000004,0x6e69616d,0x00000000,0x00060010,0x00000004,0x00000011,
	0x00000001,0x00000001,0x00000001,0x00030003,0x00000002,0x000001c2,0x00040005,0x00000004,
	0x6e69616d,0x00000000,0x00040047,0x00000009,0x0000000b,0x00000019,0x00020013,0x00000002,
	0x00030021,0x00000003,0x00000002,0x00040015,0x00000006,0x00000020,0x00000000,0x00040017,
	0x00000007,0x00000006,0x00000003,0x0004002b,0x00000006,0x00000008,0x00000001,0x0006002c,
	0x00000007,0x00000009,0x00000008,0x00000008,0x00000008,0x00050036,0x00000002,0x00000004,
	0x00000000,0x00000003,0x000200f8,0x00000005,0x000100fd,0x00010038
};

VkResult lxrt_mvk_vkCreateComputePipelines(VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *,
                                           const VkAllocationCallbacks *, VkPipeline *);
VkResult lxrt_mvk_vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *, const VkAllocationCallbacks *,
                                       VkShaderModule *);

// One empty module per device (few devices; never freed before exit).
static struct { VkDevice dev; VkShaderModule mod; } g_stub[8];
static volatile int g_stub_lock;

static VkShaderModule stub_module(VkDevice dev)
{
    for (unsigned i = 0; i < 8; i++)
        if (g_stub[i].dev == dev)
            return g_stub[i].mod;
    while (__atomic_exchange_n(&g_stub_lock, 1, __ATOMIC_ACQUIRE))
        ;
    VkShaderModule m = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0,
                                    sizeof k_empty_comp_spv, k_empty_comp_spv };
    if (lxrt_mvk_vkCreateShaderModule(dev, &ci, NULL, &m) == VK_SUCCESS)
        for (unsigned i = 0; i < 8; i++)
            if (!g_stub[i].dev) {
                g_stub[i].mod = m;
                __atomic_store_n(&g_stub[i].dev, dev, __ATOMIC_RELEASE);
                break;
            }
    __atomic_store_n(&g_stub_lock, 0, __ATOMIC_RELEASE);
    return m;
}

void lxrt_spirv_dump(const uint32_t *code, size_t size);   // spirv_names.c (LXRT_VK_DUMP_SPIRV)

VkResult lxrt_inner_vkCreateComputePipelines(VkDevice dev, VkPipelineCache cache, uint32_t n,
                                             const VkComputePipelineCreateInfo *cis,
                                             const VkAllocationCallbacks *alloc, VkPipeline *out)
{
    // Shader code given inline (VK_KHR_maintenance5) never passes through
    // vkCreateShaderModule: dumped here too.
    for (uint32_t i = 0; cis && i < n; i++)
        for (const VkBaseInStructure *b = cis[i].stage.pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
                lxrt_spirv_dump(((const VkShaderModuleCreateInfo *)b)->pCode,
                                ((const VkShaderModuleCreateInfo *)b)->codeSize);
    VkResult r = lxrt_mvk_vkCreateComputePipelines(dev, cache, n, cis, alloc, out);
    if (r != VK_ERROR_INITIALIZATION_FAILED)
        return r;
    const char *e = getenv("LXRT_VK_NO_STUB_PIPELINES");
    if (e && *e == '1')
        return r;
    // Again one by one, so only the ones that really fail are replaced.
    r = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = VK_NULL_HANDLE;
        VkResult ri = lxrt_mvk_vkCreateComputePipelines(dev, cache, 1, &cis[i], alloc, &out[i]);
        if (ri == VK_ERROR_INITIALIZATION_FAILED) {
            VkComputePipelineCreateInfo c = cis[i];
            c.pNext = NULL;
            c.stage.pNext = NULL;           // an inline module or required subgroup size
            c.stage.flags = 0;
            c.stage.module = stub_module(dev);
            c.stage.pName = "main";
            c.stage.pSpecializationInfo = NULL;
            ri = lxrt_mvk_vkCreateComputePipelines(dev, cache, 1, &c, alloc, &out[i]);
            dprintf(2, "[shim] compute pipeline %u of %u: MoltenVK could not compile its shader; "
                       "replaced by an empty one (%d)\n", i, n, (int)ri);
        }
        if (ri < 0 && r == VK_SUCCESS)
            r = ri;
    }
    return r;
}
