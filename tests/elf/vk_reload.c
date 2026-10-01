// libvulkan.so.1 loaded, used and unloaded three times in one process, as
// Steam's i386 client does when it probes Vulkan and Mesa's Zink then loads
// it again: dlopen, vkCreateInstance, vkEnumeratePhysicalDevices through
// vkGetInstanceProcAddr, vkDestroyInstance, dlclose. Under FEX's Vulkan
// thunks the second load must not fault (tests/elf/run_vk_device.sh).
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct { int sType; const void *pNext; uint32_t flags; const void *app; uint32_t nl; const char *const *l;
                 uint32_t ne; const char *const *e; } InstanceInfo;
typedef void (*PFN)(void);
typedef int (*CreateInstance)(const InstanceInfo *, const void *, void **);
typedef PFN (*GetInstanceProcAddr)(void *, const char *);
typedef int (*EnumPD)(void *, uint32_t *, void **);
typedef void (*DestroyInstance)(void *, const void *);

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int ok = 0;
    for (int round = 1; round <= 3; round++) {
        void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!lib) { printf("round %d: dlopen: %s\n", round, dlerror()); break; }
        GetInstanceProcAddr gipa = (GetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
        CreateInstance create = (CreateInstance)gipa(NULL, "vkCreateInstance");
        InstanceInfo ci = { 1 /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO */, 0, 0, 0, 0, 0, 0, 0 };
        void *inst = NULL;
        int r = create ? create(&ci, NULL, &inst) : -1;
        EnumPD enumpd = inst ? (EnumPD)gipa(inst, "vkEnumeratePhysicalDevices") : NULL;
        DestroyInstance destroy = inst ? (DestroyInstance)gipa(inst, "vkDestroyInstance") : NULL;
        uint32_t n = 0;
        int r2 = enumpd ? enumpd(inst, &n, NULL) : -1;
        printf("round %d: vkCreateInstance %d, %u physical device(s) (%d)\n", round, r, n, r2);
        if (r == 0 && r2 == 0 && n > 0) ok++;
        if (destroy) destroy(inst, NULL);
        dlclose(lib);
    }
    printf("== vk_reload: %s\n", ok == 3 ? "ok" : "FAIL");
    return ok != 3;
}
