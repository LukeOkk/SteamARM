// lxrt host bridge -- for Linux guest code compiled against this runtime.
//
// A guest ELF cannot dlopen a Mach-O dylib: its own ld.so has no idea what one
// is. These three calls let it ask the runtime instead, and what comes back is
// a real function pointer into the host's address space. The guest then calls
// it with an ordinary `blr`.
//
// That is the point: AAPCS64 is the same calling convention on both sides and
// there is only one address space, so after the lookup there is no marshalling,
// no ring buffer and no second process. Compare with what this replaced in
// the VM era: every Vulkan call serialised by a guest driver across a shared
// ring to a renderer process on the host.

#ifndef LXRT_HOST_H
#define LXRT_HOST_H

#include <stdint.h>

#define LXRT_NR_DLOPEN  0x4C580001L
#define LXRT_NR_DLSYM   0x4C580002L
#define LXRT_NR_DLERROR 0x4C580003L
#define LXRT_NR_WINDOW   0x4C580010L
#define LXRT_NR_DRAWABLE 0x4C580011L
#define LXRT_NR_RLAYER_CREATE  0x4C580012L
#define LXRT_NR_RLAYER_RESIZE  0x4C580013L
#define LXRT_NR_RLAYER_RELEASE 0x4C580014L
#define LXRT_NR_GUEST_BASE_GET 0x4C580031L   // () -> guest address base, 0 if none
#define LXRT_NR_ALIAS          0x4C580032L   // (src, len, dst) -> 0: shared alias of host pages

static inline long lxrt_syscall2(long nr, long a0, long a1)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    __asm__ __volatile__("svc #0"
                         : "+r"(x0)
                         : "r"(x1), "r"(x8)
                         : "memory", "cc");
    return x0;
}

// Returns an opaque host handle, or 0. `flags` is ignored; the runtime always
// resolves eagerly.
static inline void *lxrt_host_dlopen(const char *path, int flags)
{
    return (void *)lxrt_syscall2(LXRT_NR_DLOPEN, (long)(uintptr_t)path, flags);
}

// Returns a host function pointer that guest code can call directly.
static inline void *lxrt_host_dlsym(void *handle, const char *name)
{
    return (void *)lxrt_syscall2(LXRT_NR_DLSYM, (long)(uintptr_t)handle,
                                 (long)(uintptr_t)name);
}

static inline long lxrt_syscall3(long nr, long a0, long a1, long a2)
{
    register long x8 __asm__("x8") = nr;
    register long x0 __asm__("x0") = a0;
    register long x1 __asm__("x1") = a1;
    register long x2 __asm__("x2") = a2;
    __asm__ __volatile__("svc #0"
                         : "+r"(x0)
                         : "r"(x1), "r"(x2), "r"(x8)
                         : "memory", "cc");
    return x0;
}

// Creates the runtime's window and returns its CAMetalLayer. Pass the result
// as VkMetalSurfaceCreateInfoEXT::pLayer -- that is the whole presentation
// path, with no framebuffer copy anywhere in it.
static inline void *lxrt_host_window(int width, int height, const char *title)
{
    return (void *)lxrt_syscall3(LXRT_NR_WINDOW, width, height,
                                 (long)(uintptr_t)title);
}

// The layer's drawable size in pixels. A swapchain must match it, and on a
// Retina display it is not the size that was requested.
static inline void lxrt_host_drawable_size(uint32_t *w, uint32_t *h)
{
    (void)lxrt_syscall2(LXRT_NR_DRAWABLE, (long)(uintptr_t)w, (long)(uintptr_t)h);
}

static inline long lxrt_host_dlerror(char *buf, unsigned long len)
{
    return lxrt_syscall2(LXRT_NR_DLERROR, (long)(uintptr_t)buf, (long)len);
}

#endif
