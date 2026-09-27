// Linux aarch64 ELF calling Mach-O MoltenVK in the same address space.
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "lxrt_host.h"

typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef int32_t VkResult;

struct VkApplicationInfo {
    uint32_t sType; uint32_t _p0; const void *pNext;
    const char *pApplicationName; uint32_t applicationVersion; uint32_t _p1;
    const char *pEngineName; uint32_t engineVersion; uint32_t _p2;
    uint32_t apiVersion; uint32_t _p3;
};
struct VkInstanceCreateInfo {
    uint32_t sType; uint32_t _p0; const void *pNext;
    uint32_t flags; uint32_t _p1;
    const struct VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount; uint32_t _p2; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; uint32_t _p3; const char *const *ppEnabledExtensionNames;
};

typedef void *(*PFN_vkGetInstanceProcAddr)(VkInstance, const char *);
typedef VkResult (*PFN_vkCreateInstance)(const struct VkInstanceCreateInfo *, const void *, VkInstance *);
typedef VkResult (*PFN_vkEnumeratePhysicalDevices)(VkInstance, uint32_t *, VkPhysicalDevice *);
typedef void (*PFN_vkGetPhysicalDeviceProperties)(VkPhysicalDevice, void *);
typedef VkResult (*PFN_vkEnumerateInstanceVersion)(uint32_t *);
typedef int (*PFN_getpid)(void);

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *paths[] = {
        "/opt/homebrew/lib/libMoltenVK.dylib",
        "/usr/local/lib/libMoltenVK.dylib",
    };
    void *h = NULL;
    for (unsigned i = 0; i < sizeof(paths)/sizeof(paths[0]) && !h; i++)
        h = lxrt_host_dlopen(paths[i], 0);
    if (!h) { char e[256]; lxrt_host_dlerror(e, sizeof e); printf("dlopen fallo: %s\n", e); return 1; }
    printf("MoltenVK cargado, handle %p\n", h);

    PFN_vkGetInstanceProcAddr gipa = lxrt_host_dlsym(h, "vkGetInstanceProcAddr");
    if (!gipa) { printf("sin vkGetInstanceProcAddr\n"); return 1; }
    printf("vkGetInstanceProcAddr = %p  (puntero del anfitrion, llamado con blr)\n", (void*)gipa);

    PFN_vkEnumerateInstanceVersion eiv = gipa(NULL, "vkEnumerateInstanceVersion");
    uint32_t ver = 0;
    if (eiv && eiv(&ver) == 0)
        printf("Vulkan %u.%u.%u\n", ver >> 22, (ver >> 12) & 0x3ff, ver & 0xfff);

    PFN_vkCreateInstance createInstance = gipa(NULL, "vkCreateInstance");
    if (!createInstance) { printf("sin vkCreateInstance\n"); return 1; }

    struct VkApplicationInfo app; memset(&app, 0, sizeof app);
    app.sType = 0; app.pApplicationName = "lxrt"; app.pEngineName = "lxrt";
    app.apiVersion = (1u << 22) | (1u << 12);           /* 1.1.0 */
    struct VkInstanceCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = 1; ci.pApplicationInfo = &app;

    VkInstance inst = NULL;
    VkResult r = createInstance(&ci, NULL, &inst);
    printf("vkCreateInstance -> %d, instance %p\n", r, inst);
    if (r != 0) return 1;

    PFN_vkEnumeratePhysicalDevices epd = gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties gpdp = gipa(inst, "vkGetPhysicalDeviceProperties");
    VkPhysicalDevice dev0 = NULL;
    uint32_t n = 0;
    epd(inst, &n, NULL);
    printf("dispositivos fisicos: %u\n", n);
    if (n) {
        static VkPhysicalDevice dev[4]; if (n > 4) n = 4;
        epd(inst, &n, dev);
        dev0 = dev[0];
        for (uint32_t i = 0; i < n; i++) {
            unsigned char props[1024]; memset(props, 0, sizeof props);
            gpdp(dev[i], props);
            uint32_t api; memcpy(&api, props, 4);
            printf("  [%u] %s  (API %u.%u.%u)\n", i, (const char *)(props + 20),
                   api >> 22, (api >> 12) & 0x3ff, api & 0xfff);
        }
    }

    /* Coste de una llamada directa anfitrion<-huesped, contra una syscall.
       RTLD_DEFAULT no es NULL en Darwin, asi que se abre libSystem explicito. */
    void *libc = lxrt_host_dlopen("/usr/lib/libSystem.B.dylib", 0);
    PFN_getpid host_getpid = libc ? lxrt_host_dlsym(libc, "getpid") : NULL;
    printf("libSystem=%p getpid=%p\n", libc, (void *)host_getpid);
    if (host_getpid) {
        printf("llamando getpid del anfitrion... ");
        int gp = host_getpid();
        printf("-> %d\n", gp);
        const int N = 2000000;
        uint64_t t0 = now_ns();
        for (int i = 0; i < N; i++) host_getpid();
        double direct = (double)(now_ns() - t0) / N;
        /* Comparacion: la misma operacion como syscall Linux reescrita. */
        t0 = now_ns();
        for (int i = 0; i < N; i++) (void)lxrt_syscall2(172 /* getpid */, 0, 0);
        double viasys = (double)(now_ns() - t0) / N;
        /* Y una llamada Vulkan real, la que en la VM cruzaba un anillo compartido. */
        unsigned char props[1024];
        t0 = now_ns();
        for (int i = 0; i < 200000; i++) gpdp(dev0, props);
        double vkcall = (double)(now_ns() - t0) / 200000.0;
        printf("llamada directa al anfitrion: %.1f ns/op | syscall reescrita: %.1f ns/op\n",
               direct, viasys);
        printf("vkGetPhysicalDeviceProperties (MoltenVK real): %.1f ns/llamada\n", vkcall);
    }
    return 0;
}
