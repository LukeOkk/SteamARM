// SPIR-V debug names that MoltenVK turns into invalid Metal.
//
// SPIRV-Cross names a resource after its OpName. A combined image sampler
// named "sampler" -- WineD3D's GLSL blit shaders declare
// `uniform sampler2D sampler;`, and Mesa's Zink keeps the name -- becomes
//     texture2d<float> sampler [[id(0)]];
//     sampler samplerSmplr [[id(1)]];
// and Metal refuses it ("must use 'struct' tag to refer to type 'sampler'"):
// MoltenVK fails the pipeline with VK_ERROR_INITIALIZATION_FAILED, Zink
// drops the draw (MEASURED: tests/win d3d9 through WineD3D on Zink,
// benchmarks/stage38-opengl-zink.txt).
//
// The fix is in the names alone: an OpName or OpMemberName equal to a Metal
// type name gets its first letter upper-cased, in place (same length, so no
// instruction changes size). Debug names have no meaning to the program;
// SPIRV-Cross still de-duplicates any clash it sees.
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan_core.h>

// Built -nostdlib (Makefile): the guest's libc provides these at run time.
extern void *malloc(size_t);
extern void free(void *);
extern void *memcpy(void *, const void *, size_t);

#define SPV_MAGIC         0x07230203u
#define SPV_OP_NAME       5
#define SPV_OP_MEMBERNAME 6

static const char *const metal_types[] = { "sampler", "texture" };

// `s` is a NUL-padded literal of at most `max` bytes.
static int is_metal_type(const char *s, size_t max)
{
    for (size_t i = 0; i < sizeof metal_types / sizeof *metal_types; i++) {
        const char *t = metal_types[i];
        size_t k = 0;
        while (k < max && t[k] && s[k] == t[k])
            k++;
        if (!t[k] && k < max && !s[k])
            return 1;
    }
    return 0;
}

// A copy of `code` with the names fixed, or NULL when nothing needs fixing
// (then the caller passes the original on untouched). `size` is in bytes.
uint32_t *lxrt_spirv_fix_names(const uint32_t *code, size_t size)
{
    if (!code || size < 20 || (size & 3) || code[0] != SPV_MAGIC)
        return NULL;
    size_t words = size / 4;
    uint32_t *out = NULL;
    for (size_t i = 5; i < words;) {
        uint32_t wc = code[i] >> 16, op = code[i] & 0xffff;
        if (!wc || i + wc > words)
            break;
        // OpName: <id> "name"; OpMemberName: <id> <member> "name".
        size_t first = op == SPV_OP_NAME ? 2 : op == SPV_OP_MEMBERNAME ? 3 : 0;
        if (first && wc > first) {
            const char *s = (const char *)(code + i + first);
            size_t max = (wc - first) * 4;
            if (is_metal_type(s, max)) {
                if (!out) {
                    out = malloc(size);
                    if (!out)
                        return NULL;
                    memcpy(out, code, size);
                }
                char *d = (char *)(out + i + first);
                d[0] = (char)(d[0] - 'a' + 'A');
            }
        }
        // Names come before every function: stop at the first OpFunction.
        if (op == 54)
            break;
        i += wc;
    }
    return out;
}

VkResult lxrt_mvk_vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *, const VkAllocationCallbacks *,
                                       VkShaderModule *);

VkResult lxrt_inner_vkCreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo *ci,
                                         const VkAllocationCallbacks *alloc, VkShaderModule *out)
{
    uint32_t *fixed = ci ? lxrt_spirv_fix_names(ci->pCode, ci->codeSize) : NULL;
    if (!fixed)
        return lxrt_mvk_vkCreateShaderModule(dev, ci, alloc, out);
    VkShaderModuleCreateInfo c = *ci;
    c.pCode = fixed;
    VkResult r = lxrt_mvk_vkCreateShaderModule(dev, &c, alloc, out);
    free(fixed);
    return r;
}
