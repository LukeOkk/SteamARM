// X11 window-system integration for the Vulkan shim (native-window stage W2).
//
// MoltenVK only presents to Metal surfaces. Linux Vulkan programs -- DXVK and
// VKD3D inside Wine above all -- ask for VK_KHR_xlib_surface / VK_KHR_xcb_surface
// and hand over an X window. This file answers those requests itself:
//
//   * the X11 surface extensions are advertised, and swapped for
//     VK_EXT_metal_surface when the instance is created;
//   * vkCreate{Xlib,Xcb}SurfaceKHR asks the runtime for a CAMetalLayer inside a
//     cross-process Core Animation context (runtime/remote_layer.m), creates a
//     Metal surface on it, and publishes the context id on the X window as the
//     property _STEAMARM_LAYER (CARDINAL/32). The native X server shows that
//     context inside the window's own NSView with a CALayerHost: the game's
//     frames reach the screen with no copy and no second window;
//   * the surface-capabilities queries first resize the layer to the X
//     window's current size, so a resized window gets a matching swapchain.
//
// Every other entry point still tail-calls MoltenVK (vulkan_shim.S); the ones
// this file wraps are listed in overrides.txt and reach MoltenVK through the
// hidden lxrt_mvk_* thunks.
#include "lxrt_host.h"
#include <stdint.h>
#include <stddef.h>

typedef int32_t VkResult;
typedef uint32_t VkBool32;
typedef uint64_t VkSurfaceKHR;
typedef void *VkInstance;
typedef void *VkPhysicalDevice;
typedef void (*PFN_vkVoidFunction)(void);

#define VK_SUCCESS 0
#define VK_INCOMPLETE 5
#define VK_ERROR_OUT_OF_HOST_MEMORY (-1)
#define VK_ERROR_INITIALIZATION_FAILED (-3)
#define VK_ERROR_SURFACE_LOST_KHR (-1000000000)
#define VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT 1000217000

typedef struct {
    int32_t sType; const void *pNext; uint32_t flags;
    const void *pApplicationInfo;
    uint32_t enabledLayerCount; const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount; const char *const *ppEnabledExtensionNames;
} VkInstanceCreateInfo;

typedef struct { char extensionName[256]; uint32_t specVersion; } VkExtensionProperties;

typedef struct {
    int32_t sType; const void *pNext; uint32_t flags;
    void *dpy; unsigned long window;
} VkXlibSurfaceCreateInfoKHR;

typedef struct {
    int32_t sType; const void *pNext; uint32_t flags;
    void *connection; uint32_t window;
} VkXcbSurfaceCreateInfoKHR;

typedef struct {
    int32_t sType; const void *pNext; uint32_t flags;
    const void *pLayer;
} VkMetalSurfaceCreateInfoEXT;

typedef struct { int32_t sType; const void *pNext; VkSurfaceKHR surface; } VkPhysicalDeviceSurfaceInfo2KHR;

// MoltenVK's own versions (vulkan_shim.S, hidden).
VkResult lxrt_mvk_vkCreateInstance(const VkInstanceCreateInfo *, const void *, VkInstance *);
VkResult lxrt_mvk_vkEnumerateInstanceExtensionProperties(const char *, uint32_t *, VkExtensionProperties *);
PFN_vkVoidFunction lxrt_mvk_vkGetInstanceProcAddr(VkInstance, const char *);
void *lxrt_rebase_proc(const char *name);   // vk_rebase.c (generated)
void *lxrt_tramp32(void *fn);                // map32.c: 32-bit guests only
PFN_vkVoidFunction lxrt_mvk_vk_icdGetInstanceProcAddr(VkInstance, const char *);
void lxrt_mvk_vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR, const void *);
VkResult lxrt_mvk_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR, void *);
VkResult lxrt_mvk_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR *, void *);
VkResult vkCreateMetalSurfaceEXT(VkInstance, const VkMetalSurfaceCreateInfoEXT *, const void *, VkSurfaceKHR *);

// glibc, resolved from the process when the shim loads (no DT_NEEDED: -nostdlib).
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern void *malloc(size_t);
extern void free(void *);
#define RTLD_NOW 2

// ---------------------------------------------------------------- strings
static int s_eq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void s_copy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    for (; i + 1 < n && s[i]; i++) d[i] = s[i];
    d[i] = 0;
}

// ---------------------------------------------------------------- xcb
typedef struct { uint32_t sequence; } xcb_cookie_t;
typedef struct {
    uint8_t response_type, depth; uint16_t sequence; uint32_t length;
    uint32_t root; int16_t x, y; uint16_t width, height, border_width; uint8_t pad[2];
} xcb_get_geometry_reply_t;
typedef struct {
    uint8_t response_type, pad0; uint16_t sequence; uint32_t length; uint32_t atom;
} xcb_intern_atom_reply_t;

static struct {
    void *(*get_xcb)(void *dpy);
    xcb_cookie_t (*get_geometry)(void *c, uint32_t w);
    xcb_get_geometry_reply_t *(*get_geometry_reply)(void *c, xcb_cookie_t, void **e);
    xcb_cookie_t (*intern_atom)(void *c, uint8_t only_if_exists, uint16_t len, const char *name);
    xcb_intern_atom_reply_t *(*intern_atom_reply)(void *c, xcb_cookie_t, void **e);
    xcb_cookie_t (*change_property)(void *c, uint8_t mode, uint32_t w, uint32_t prop,
                                    uint32_t type, uint8_t format, uint32_t len, const void *data);
    int (*flush)(void *c);
    int loaded;
} X;

static int load_xcb(void)
{
    if (X.loaded)
        return X.loaded > 0;
    void *xcb = dlopen("libxcb.so.1", RTLD_NOW);
    void *xx = dlopen("libX11-xcb.so.1", RTLD_NOW);
    if (xcb) {
        X.get_geometry = dlsym(xcb, "xcb_get_geometry");
        X.get_geometry_reply = dlsym(xcb, "xcb_get_geometry_reply");
        X.intern_atom = dlsym(xcb, "xcb_intern_atom");
        X.intern_atom_reply = dlsym(xcb, "xcb_intern_atom_reply");
        X.change_property = dlsym(xcb, "xcb_change_property");
        X.flush = dlsym(xcb, "xcb_flush");
    }
    if (xx)
        X.get_xcb = dlsym(xx, "XGetXCBConnection");
    X.loaded = (X.get_geometry && X.get_geometry_reply && X.intern_atom && X.intern_atom_reply &&
                X.change_property && X.flush) ? 1 : -1;
    return X.loaded > 0;
}

static int window_size(void *conn, uint32_t win, uint32_t *w, uint32_t *h)
{
    xcb_get_geometry_reply_t *r = X.get_geometry_reply(conn, X.get_geometry(conn, win), 0);
    if (!r)
        return 0;
    *w = r->width ? r->width : 1;
    *h = r->height ? r->height : 1;
    free(r);
    return 1;
}

// ---------------------------------------------------------------- surfaces
typedef struct wsi_surface {
    VkSurfaceKHR surface;
    void *layer, *conn;
    uint32_t window, w, h;
    struct wsi_surface *next;
} wsi_surface;
static wsi_surface *g_surfaces;   // few, long-lived: a list is enough

static wsi_surface *find(VkSurfaceKHR s)
{
    for (wsi_surface *p = g_surfaces; p; p = p->next)
        if (p->surface == s)
            return p;
    return 0;
}

static VkResult create_x11_surface(VkInstance inst, void *conn, uint32_t window,
                                   const void *alloc, VkSurfaceKHR *out)
{
    if (!conn || !window || !load_xcb())
        return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t w = 1, h = 1, ctx = 0;
    window_size(conn, window, &w, &h);
    void *layer = (void *)lxrt_syscall3(LXRT_NR_RLAYER_CREATE, w, h, (long)(uintptr_t)&ctx);
    if (!layer)
        return VK_ERROR_INITIALIZATION_FAILED;
    VkMetalSurfaceCreateInfoEXT mi = { VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, 0, 0, layer };
    VkResult r = vkCreateMetalSurfaceEXT(inst, &mi, alloc, out);
    if (r != VK_SUCCESS) {
        lxrt_syscall2(LXRT_NR_RLAYER_RELEASE, (long)(uintptr_t)layer, 0);
        return r;
    }
    // Tell the X server which Core Animation context shows this window.
    static const char name[] = "_STEAMARM_LAYER";
    xcb_intern_atom_reply_t *a = X.intern_atom_reply(conn, X.intern_atom(conn, 0, sizeof name - 1, name), 0);
    if (a) {
        X.change_property(conn, 0 /* Replace */, window, a->atom, 6 /* CARDINAL */, 32, 1, &ctx);
        X.flush(conn);
        free(a);
    }
    wsi_surface *s = malloc(sizeof *s);
    if (s) {
        s->surface = *out; s->layer = layer; s->conn = conn; s->window = window; s->w = w; s->h = h;
        s->next = g_surfaces; g_surfaces = s;
    }
    return VK_SUCCESS;
}

VkResult lxrt_inner_vkCreateXcbSurfaceKHR(VkInstance inst, const VkXcbSurfaceCreateInfoKHR *ci,
                               const void *alloc, VkSurfaceKHR *out)
{
    return create_x11_surface(inst, ci->connection, ci->window, alloc, out);
}

VkResult lxrt_inner_vkCreateXlibSurfaceKHR(VkInstance inst, const VkXlibSurfaceCreateInfoKHR *ci,
                                const void *alloc, VkSurfaceKHR *out)
{
    if (!load_xcb() || !X.get_xcb)
        return VK_ERROR_INITIALIZATION_FAILED;
    return create_x11_surface(inst, X.get_xcb(ci->dpy), (uint32_t)ci->window, alloc, out);
}

VkBool32 lxrt_inner_vkGetPhysicalDeviceXcbPresentationSupportKHR(VkPhysicalDevice pd, uint32_t qf, void *c, uint32_t vis)
{
    return 1;
}

VkBool32 lxrt_inner_vkGetPhysicalDeviceXlibPresentationSupportKHR(VkPhysicalDevice pd, uint32_t qf, void *d, unsigned long vis)
{
    return 1;
}

void lxrt_inner_vkDestroySurfaceKHR(VkInstance inst, VkSurfaceKHR surface, const void *alloc)
{
    lxrt_mvk_vkDestroySurfaceKHR(inst, surface, alloc);
    for (wsi_surface **pp = &g_surfaces; *pp; pp = &(*pp)->next) {
        if ((*pp)->surface == surface) {
            wsi_surface *s = *pp;
            *pp = s->next;
            lxrt_syscall2(LXRT_NR_RLAYER_RELEASE, (long)(uintptr_t)s->layer, 0);
            free(s);
            break;
        }
    }
}

// The X window may have been resized since the last swapchain: follow it
// before MoltenVK reports currentExtent from the layer.
static void follow_window(VkSurfaceKHR surface)
{
    wsi_surface *s = find(surface);
    uint32_t w, h;
    if (!s || !window_size(s->conn, s->window, &w, &h) || (w == s->w && h == s->h))
        return;
    if (lxrt_syscall3(LXRT_NR_RLAYER_RESIZE, (long)(uintptr_t)s->layer, w, h) == 0) {
        s->w = w;
        s->h = h;
    }
}

VkResult lxrt_inner_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice pd, VkSurfaceKHR surface, void *caps)
{
    follow_window(surface);
    return lxrt_mvk_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surface, caps);
}

VkResult lxrt_inner_vkGetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice pd, const VkPhysicalDeviceSurfaceInfo2KHR *info,
                                                    void *caps)
{
    if (info)
        follow_window(info->surface);
    return lxrt_mvk_vkGetPhysicalDeviceSurfaceCapabilities2KHR(pd, info, caps);
}

// ---------------------------------------------------------------- instance
static const char *const k_x11_exts[] = { "VK_KHR_xlib_surface", "VK_KHR_xcb_surface" };

VkResult lxrt_inner_vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count, VkExtensionProperties *props)
{
    if (layer)
        return lxrt_mvk_vkEnumerateInstanceExtensionProperties(layer, count, props);
    uint32_t n = 0;
    VkResult r = lxrt_mvk_vkEnumerateInstanceExtensionProperties(0, &n, 0);
    if (r != VK_SUCCESS)
        return r;
    uint32_t total = n + 2;
    if (!props) {
        *count = total;
        return VK_SUCCESS;
    }
    uint32_t cap = *count, got = cap < n ? cap : n;
    r = lxrt_mvk_vkEnumerateInstanceExtensionProperties(0, &got, props);
    if (r != VK_SUCCESS && r != VK_INCOMPLETE)
        return r;
    uint32_t k = got;
    for (int i = 0; i < 2 && k < cap; i++, k++) {
        s_copy(props[k].extensionName, k_x11_exts[i], sizeof props[k].extensionName);
        props[k].specVersion = 6;
    }
    *count = k;
    return k < total ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult lxrt_inner_vkCreateInstance(const VkInstanceCreateInfo *ci, const void *alloc, VkInstance *out)
{
    // Drop the X11 surface extensions MoltenVK does not know; make sure the
    // two it needs to back them are on.
    uint32_t n = ci->enabledExtensionCount;
    const char **ext = malloc(sizeof(char *) * (n + 2));
    if (!ext)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t k = 0;
    int metal = 0, surface = 0, x11 = 0;
    for (uint32_t i = 0; i < n; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (s_eq(e, k_x11_exts[0]) || s_eq(e, k_x11_exts[1])) { x11 = 1; continue; }
        if (s_eq(e, "VK_EXT_metal_surface")) metal = 1;
        if (s_eq(e, "VK_KHR_surface")) surface = 1;
        ext[k++] = e;
    }
    if (x11 && !metal) ext[k++] = "VK_EXT_metal_surface";
    if (x11 && !surface) ext[k++] = "VK_KHR_surface";
    VkInstanceCreateInfo c = *ci;
    c.enabledExtensionCount = k;
    c.ppEnabledExtensionNames = ext;
    VkResult r = lxrt_mvk_vkCreateInstance(&c, alloc, out);
    free(ext);
    return r;
}

PFN_vkVoidFunction lxrt_inner_vkGetInstanceProcAddr(VkInstance inst, const char *name)
{
    // Every entry point the shim wraps (vk_rebase.c: guest pointers rebased,
    // then this file's or features.c's version, or MoltenVK's). Handing out
    // MoltenVK's own pointers would skip the rebasing.
    void *f = lxrt_rebase_proc(name);
    if (!f)
        f = (void *)lxrt_mvk_vkGetInstanceProcAddr(inst, name);
    return (PFN_vkVoidFunction)lxrt_tramp32(f);
}

// The loader-ICD entry point answers the same way.
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance inst, const char *name)
{
    void *f = lxrt_rebase_proc(name);
    if (!f)
        f = (void *)lxrt_mvk_vk_icdGetInstanceProcAddr(inst, name);
    return (PFN_vkVoidFunction)lxrt_tramp32(f);
}
