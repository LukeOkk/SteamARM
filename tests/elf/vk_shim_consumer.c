/* Un consumidor Vulkan corriente: enlaza -lvulkan y llama a la API. Ni una
   syscall privada. Esto es lo que hace DXVK. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

typedef void *VkInstance; typedef void *VkPhysicalDevice; typedef int32_t VkResult;
struct VkApplicationInfo {
    uint32_t sType; uint32_t _p0; const void *pNext;
    const char *pApplicationName; uint32_t applicationVersion; uint32_t _p1;
    const char *pEngineName; uint32_t engineVersion; uint32_t _p2;
    uint32_t apiVersion; uint32_t _p3;
};
struct VkInstanceCreateInfo {
    uint32_t sType; uint32_t _p0; const void *pNext; uint32_t flags; uint32_t _p1;
    const struct VkApplicationInfo *pApplicationInfo;
    uint32_t enabledLayerCount; uint32_t _p2; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; uint32_t _p3; const char *const *ppEnabledExtensionNames;
};
extern VkResult vkEnumerateInstanceVersion(uint32_t *);
extern VkResult vkCreateInstance(const struct VkInstanceCreateInfo *, const void *, VkInstance *);
extern VkResult vkEnumeratePhysicalDevices(VkInstance, uint32_t *, VkPhysicalDevice *);
extern void vkGetPhysicalDeviceProperties(VkPhysicalDevice, void *);
extern VkResult vkEnumerateInstanceExtensionProperties(const char *, uint32_t *, void *);
extern void vkDestroyInstance(VkInstance, const void *);

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    uint32_t ver = 0;
    if (vkEnumerateInstanceVersion(&ver) == 0)
        printf("vkEnumerateInstanceVersion -> %u.%u.%u\n", ver>>22, (ver>>12)&0x3ff, ver&0xfff);

    uint32_t nx = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &nx, NULL);
    printf("extensiones de instancia: %u\n", nx);

    struct VkApplicationInfo app; memset(&app, 0, sizeof app);
    app.sType = 0; app.pApplicationName = "shim"; app.apiVersion = (1u<<22)|(1u<<12);
    struct VkInstanceCreateInfo ci; memset(&ci, 0, sizeof ci);
    ci.sType = 1; ci.pApplicationInfo = &app;
    VkInstance inst = NULL;
    VkResult r = vkCreateInstance(&ci, NULL, &inst);
    printf("vkCreateInstance -> %d\n", r);
    if (r != 0) return 1;

    uint32_t n = 0; vkEnumeratePhysicalDevices(inst, &n, NULL);
    VkPhysicalDevice dev[4]; if (n > 4) n = 4;
    vkEnumeratePhysicalDevices(inst, &n, dev);
    for (uint32_t i = 0; i < n; i++) {
        unsigned char p[1024]; memset(p, 0, sizeof p);
        vkGetPhysicalDeviceProperties(dev[i], p);
        printf("  [%u] %s\n", i, (const char *)(p + 20));
    }
    if (n) {
        unsigned char p[1024];
        struct timespec t0, t1;
        const int N = 200000;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < N; i++) vkGetPhysicalDeviceProperties(dev[0], p);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double ns = ((double)(t1.tv_sec-t0.tv_sec)*1e9 + (t1.tv_nsec-t0.tv_nsec)) / N;
        printf("vkGetPhysicalDeviceProperties por el shim: %.1f ns/llamada\n", ns);
    }
    vkDestroyInstance(inst, NULL);
    printf("vkDestroyInstance ok\n");
    return 0;
}
