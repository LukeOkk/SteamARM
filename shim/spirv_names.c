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

// LXRT_VK_DUMP_SPIRV=<guest directory>: every shader module a program
// creates, once each, as <directory>/<FNV-1a of the words>.spv -- to find
// a game's own passes (Counter-Strike 2's FSR) by what they contain.
extern char *getenv(const char *);
extern int open(const char *, int, ...);
extern long write(int, const void *, size_t);
extern int close(int);
extern int snprintf(char *, size_t, const char *, ...);
void lxrt_spirv_dump(const uint32_t *code, size_t size)
{
    static const char *dir = (const char *)1;
    if (dir == (const char *)1) {
        dir = getenv("LXRT_VK_DUMP_SPIRV");
        if (dir && !*dir) dir = 0;
    }
    if (!dir || !code || size < 20)
        return;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size / 4; i++) {
        h ^= code[i];
        h *= 1099511628211ull;
    }
    char path[512];
    snprintf(path, sizeof path, "%s/%016llx.spv", dir, (unsigned long long)h);
    int fd = open(path, 01 | 0100 | 0200, 0644);       // O_WRONLY|O_CREAT|O_EXCL (Linux)
    static int said;
    if (!said++) {
        extern int dprintf(int, const char *, ...);
        dprintf(2, "[shim] spirv dump: first module (%zu bytes) -> %s: %s\n", size, path, fd >= 0 ? "written" : "open failed");
    }
    if (fd >= 0) {
        write(fd, code, size);
        close(fd);
    }
}

// Stages of a graphics pipeline (shim/features.c), code given inline.
void lxrt_spirv_dump_stages(const void *stages, uint32_t count)
{
    const VkPipelineShaderStageCreateInfo *st = stages;
    for (uint32_t i = 0; st && i < count; i++)
        for (const VkBaseInStructure *b = st[i].pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO)
                lxrt_spirv_dump(((const VkShaderModuleCreateInfo *)b)->pCode,
                                ((const VkShaderModuleCreateInfo *)b)->codeSize);
}

// shim/spirv_dref.c: shadow compares on their own variable (MoltenVK only)
uint32_t *lxrt_spirv_split_dref(const uint32_t *code, size_t size, size_t *out_size);
int lxrt_spirv_split_dref_wanted(void);
extern const char *lxrt_vk_driver;   // vulkan_shim.c (generated): the driver in use

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

VkResult lxrt_inner_vkCreateShaderModule(VkDevice dev, const VkShaderModuleCreateInfo *ci,
                                         const VkAllocationCallbacks *alloc, VkShaderModule *out)
{
    if (ci)
        lxrt_spirv_dump(ci->pCode, ci->codeSize);
    uint32_t *fixed = ci ? lxrt_spirv_fix_names(ci->pCode, ci->codeSize) : NULL;
    VkShaderModuleCreateInfo c = ci ? *ci : (VkShaderModuleCreateInfo){0};
    if (fixed)
        c.pCode = fixed;
    uint32_t *split = NULL;
    size_t split_size = 0;
    if (ci && on_moltenvk() && lxrt_spirv_split_dref_wanted() &&
        (split = lxrt_spirv_split_dref(c.pCode, c.codeSize, &split_size))) {
        c.pCode = split;
        c.codeSize = split_size;
    }
    if (!fixed && !split)
        return lxrt_mvk_vkCreateShaderModule(dev, ci, alloc, out);
    VkResult r = lxrt_mvk_vkCreateShaderModule(dev, &c, alloc, out);
    free(fixed);
    free(split);
    return r;
}
