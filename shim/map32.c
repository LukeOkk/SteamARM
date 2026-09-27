// vkMapMemory for 32-bit guests.
//
// A 32-bit guest lives at host = base + guest (FEX's guest base), so the
// host pointer MoltenVK returns for mapped memory lies outside anything the
// guest can address; FEX's 32-bit thunk would truncate it. Here the mapped
// host pages are aliased (shared, not copied: runtime syscall LXRT_NR_ALIAS)
// into a range FEX's guest allocator reserves (fex_lxrt_guest_reserve,
// patches/fex-lxrt-guest-reserve.patch), and the window address is returned;
// the thunk turns it into the guest offset. vkUnmapMemory gives the range
// back. 64-bit guests (and runs outside FEX) are passed straight through.
//
// Allocation sizes are recorded at vkAllocateMemory: VK_WHOLE_SIZE maps
// "the rest of the allocation", which only the allocation knows.
#include <stdint.h>
#include <stddef.h>
#include "lxrt_host.h"

typedef int32_t VkResult;
typedef void *VkDevice;
typedef uint64_t VkDeviceMemory;
typedef uint64_t VkDeviceSize;
typedef struct { int32_t sType; const void *pNext; VkDeviceSize allocationSize; uint32_t memoryTypeIndex; } VkMemoryAllocateInfo;

#define VK_SUCCESS 0
#define VK_ERROR_MEMORY_MAP_FAILED (-5)
#define VK_WHOLE_SIZE (~0ULL)
#define HOST_PAGE 0x4000ULL

// glibc / FEX, resolved at load time or looked up (the shim is -nostdlib).
extern void *dlsym(void *, const char *);
extern int dprintf(int, const char *, ...);

VkResult lxrt_mvk_vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const void *, VkDeviceMemory *);
void lxrt_mvk_vkFreeMemory(VkDevice, VkDeviceMemory, const void *);
VkResult lxrt_mvk_vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, uint32_t, void **);
void lxrt_mvk_vkUnmapMemory(VkDevice, VkDeviceMemory);

static uint64_t (*g_bits)(void);
static uint64_t (*g_reserve)(uint64_t);
static void (*g_release)(uint64_t, uint64_t);
static uint64_t g_base;
static int g_mode = -1;          // 1: 32-bit guest with a base; 0: pass through

static int mode32(void)
{
    if (g_mode < 0) {
        g_bits = (uint64_t (*)(void))dlsym(0, "fex_lxrt_guest_bits");
        g_reserve = (uint64_t (*)(uint64_t))dlsym(0, "fex_lxrt_guest_reserve");
        g_release = (void (*)(uint64_t, uint64_t))dlsym(0, "fex_lxrt_guest_release");
        long b = lxrt_syscall2(LXRT_NR_GUEST_BASE_GET, 0, 0);
        g_base = b > 0 ? (uint64_t)b : 0;
        g_mode = g_bits && g_reserve && g_release && g_base && g_bits() == 32;
    }
    return g_mode;
}

// memory -> size, and memory -> (guest range) while mapped.
#define SLOTS 8192
static struct { VkDeviceMemory mem; VkDeviceSize size; uint64_t gaddr, glen; } g_tab[SLOTS];
static volatile int g_lock;
static void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) ; }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

static int slot_of(VkDeviceMemory m, int create)
{
    unsigned h = (unsigned)((m >> 4) ^ (m >> 20)) % SLOTS;
    for (unsigned i = 0; i < SLOTS; i++) {
        unsigned k = (h + i) % SLOTS;
        if (g_tab[k].mem == m)
            return (int)k;
        if (!g_tab[k].mem) {
            if (!create)
                return -1;
            g_tab[k].mem = m;
            return (int)k;
        }
    }
    return -1;
}

VkResult lxrt_inner_vkAllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *ai, const void *alloc, VkDeviceMemory *out)
{
    VkResult r = lxrt_mvk_vkAllocateMemory(dev, ai, alloc, out);
    if (r == VK_SUCCESS && mode32() && out && *out) {
        lock();
        int k = slot_of(*out, 1);
        if (k >= 0) { g_tab[k].size = ai->allocationSize; g_tab[k].gaddr = g_tab[k].glen = 0; }
        unlock();
    }
    return r;
}

static void drop_alias(int k)
{
    if (g_tab[k].gaddr) {
        g_release(g_tab[k].gaddr, g_tab[k].glen);   // the allocator re-reserves: the alias goes
        g_tab[k].gaddr = g_tab[k].glen = 0;
    }
}

void lxrt_inner_vkFreeMemory(VkDevice dev, VkDeviceMemory mem, const void *alloc)
{
    if (mode32() && mem) {
        lock();
        int k = slot_of(mem, 0);
        if (k >= 0) {
            drop_alias(k);
            // Tombstone-free delete: re-insert the following cluster.
            g_tab[k].mem = 0;
            for (unsigned i = ((unsigned)k + 1) % SLOTS; g_tab[i].mem; i = (i + 1) % SLOTS) {
                typeof(g_tab[0]) e = g_tab[i];
                g_tab[i].mem = 0;
                int n = slot_of(e.mem, 1);
                if (n >= 0) g_tab[n] = e;
            }
        }
        unlock();
    }
    lxrt_mvk_vkFreeMemory(dev, mem, alloc);
}

VkResult lxrt_inner_vkMapMemory(VkDevice dev, VkDeviceMemory mem, VkDeviceSize off, VkDeviceSize size,
                                uint32_t flags, void **pdata)
{
    void *host = 0;
    VkResult r = lxrt_mvk_vkMapMemory(dev, mem, off, size, flags, &host);
    if (r != VK_SUCCESS || !mode32()) {
        if (pdata) *pdata = host;
        return r;
    }
    lock();
    int k = slot_of(mem, 0);
    VkDeviceSize total = k >= 0 ? g_tab[k].size : 0;
    VkDeviceSize len = size == VK_WHOLE_SIZE ? (total > off ? total - off : 0) : size;
    uint64_t hs = (uint64_t)(uintptr_t)host & ~(HOST_PAGE - 1);
    uint64_t he = ((uint64_t)(uintptr_t)host + len + HOST_PAGE - 1) & ~(HOST_PAGE - 1);
    uint64_t g = (k >= 0 && len) ? g_reserve(he - hs) : 0;
    long ar = g ? lxrt_syscall3(LXRT_NR_ALIAS, (long)hs, (long)(he - hs), (long)(g_base + g)) : -1;
    if (ar != 0) {
        if (g) g_release(g, he - hs);
        unlock();
        dprintf(2, "[shim] vkMapMemory for a 32-bit guest: no window range for 0x%llx bytes\n",
                (unsigned long long)len);
        lxrt_mvk_vkUnmapMemory(dev, mem);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    g_tab[k].gaddr = g;
    g_tab[k].glen = he - hs;
    unlock();
    // The window address of the same byte; the 32-bit thunk subtracts the base.
    *pdata = (void *)(uintptr_t)(g_base + g + ((uint64_t)(uintptr_t)host - hs));
    return VK_SUCCESS;
}

void lxrt_inner_vkUnmapMemory(VkDevice dev, VkDeviceMemory mem)
{
    if (mode32()) {
        lock();
        int k = slot_of(mem, 0);
        if (k >= 0) drop_alias(k);
        unlock();
    }
    lxrt_mvk_vkUnmapMemory(dev, mem);
}

// Host function pointers for a 32-bit guest.
//
// FEX's 32-bit thunks hand the guest the host pointer vkGetInstanceProcAddr
// returns and call it back through the value the guest keeps -- 32 bits of
// it. Host code lives far above 4 GiB here, so every such pointer came back
// truncated (MEASURED: a call to 0xb425a7cc). Each function gets a
// trampoline (ldr x16, #8; br x16; .quad target) inside the guest window
// instead: its window address is a host pointer as well, the thunk turns it
// into the guest offset, and patches/fex-thunks-guestbase32.patch adds the
// base back before calling. Pages come from the guest allocator (RWX, which
// the runtime runs as a JIT region).
// The runtime's mmap directly: FEX exports its own mmap, which a -nostdlib
// library would bind to (MEASURED: the trampoline page never reached the
// runtime and stayed PROT_NONE).
static void *raw_mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    register long x8 __asm__("x8") = 222;
    register long x0 __asm__("x0") = (long)addr;
    register long x1 __asm__("x1") = (long)len;
    register long x2 __asm__("x2") = prot;
    register long x3 __asm__("x3") = flags;
    register long x4 __asm__("x4") = fd;
    register long x5 __asm__("x5") = off;
    __asm__ __volatile__("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x8)
                         : "memory", "cc");
    return (void *)x0;
}
#define TRAMP_PAGE 0x4000u
#define TRAMP_SIZE 16u
static struct { void *fn; void *tramp; } g_tr[4096];
static unsigned g_ntr;
static uint8_t *g_tr_page;
static unsigned g_tr_used;

void *lxrt_tramp32(void *fn)
{
    if (!fn || !mode32())
        return fn;
    lock();
    for (unsigned i = 0; i < g_ntr; i++)
        if (g_tr[i].fn == fn) {
            void *t = g_tr[i].tramp;
            unlock();
            return t;
        }
    if (g_ntr == sizeof g_tr / sizeof g_tr[0]) {
        unlock();
        return fn;
    }
    if (!g_tr_page || g_tr_used + TRAMP_SIZE > TRAMP_PAGE) {
        uint64_t g = g_reserve(TRAMP_PAGE);
        void *p = g ? raw_mmap((void *)(uintptr_t)(g_base + g), TRAMP_PAGE, 7 /* RWX */,
                           0x32 /* MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS */, -1, 0) : (void *)-1;
        if ((uint64_t)(uintptr_t)p != g_base + g) {
            unlock();
            dprintf(2, "[shim] no trampoline page in the 32-bit guest window\n");
            return fn;
        }
        g_tr_page = p;
        g_tr_used = 0;
    }
    uint32_t *t = (uint32_t *)(g_tr_page + g_tr_used);
    t[0] = 0x58000050u;                     // ldr x16, #8
    t[1] = 0xd61f0200u;                     // br  x16
    *(uint64_t *)&t[2] = (uint64_t)(uintptr_t)fn;
    __builtin___clear_cache((char *)t, (char *)t + TRAMP_SIZE);
    g_tr_used += TRAMP_SIZE;
    g_tr[g_ntr].fn = fn;
    g_tr[g_ntr].tramp = t;
    g_ntr++;
    unlock();
    return t;
}

// Dispatchable handles for a 32-bit guest.
//
// VkInstance, VkPhysicalDevice, VkDevice, VkQueue and VkCommandBuffer are
// pointers to MoltenVK objects, far above 4 GiB; FEX's 32-bit thunks keep
// them in 32 bits (MEASURED: vkEnumeratePhysicalDevices faulted at
// base + 0x38445b0, the truncated VkInstance). Each host page holding a
// handle is aliased (shared) into one range of the guest window, so the
// guest gets a real address whose bytes are the object's; when the guest
// hands it back, the thunk (patches/fex-thunks-guestbase32.patch) turns it
// into the original host pointer again, so MoltenVK and this shim only ever
// see their own handles. Slots are two host pages (an object may cross a
// page end) and are never given back: pages are keyed by host address, and
// MoltenVK recycles its objects.
#define HA_SLOTS 2048u
#define HA_SLOT (2 * HOST_PAGE)
struct lxrt_handle_alias {
    uint64_t lo, hi;                         // guest range [lo, hi), 0 until first use
    uint32_t (*h2g)(uint64_t);
    uint64_t (*g2h)(uint32_t);
};
static uint64_t g_ha_page[HA_SLOTS];         // slot -> host page (0: free)
static unsigned g_ha_used;

static uint32_t handle_h2g(uint64_t h);
static uint64_t handle_g2h(uint32_t g);
struct lxrt_handle_alias lxrt_handle_alias = { 0, 0, handle_h2g, handle_g2h };

static int ha_find(uint64_t page)
{
    unsigned h = (unsigned)(page >> 14) % HA_SLOTS;
    for (unsigned i = 0; i < HA_SLOTS; i++) {
        unsigned k = (h + i) % HA_SLOTS;
        if (g_ha_page[k] == page || !g_ha_page[k])
            return (int)k;
    }
    return -1;
}

static uint32_t handle_h2g(uint64_t h)
{
    if (!mode32())
        return (uint32_t)h;
    uint64_t page = h & ~(HOST_PAGE - 1);
    lock();
    if (!lxrt_handle_alias.lo) {
        uint64_t g = g_reserve(HA_SLOTS * HA_SLOT);
        if (!g) {
            unlock();
            dprintf(2, "[shim] no guest window range for Vulkan handles\n");
            return (uint32_t)h;
        }
        lxrt_handle_alias.hi = g + HA_SLOTS * HA_SLOT;
        __atomic_store_n(&lxrt_handle_alias.lo, g, __ATOMIC_RELEASE);
    }
    int k = ha_find(page);
    if (k >= 0 && !g_ha_page[k]) {
        uint64_t g = lxrt_handle_alias.lo + (uint64_t)k * HA_SLOT;
        if (g_ha_used + 1 >= HA_SLOTS ||
            lxrt_syscall3(LXRT_NR_ALIAS, (long)page, (long)HA_SLOT, (long)(g_base + g)) != 0)
            k = -1;
        else {
            g_ha_page[k] = page;
            g_ha_used++;
        }
    }
    unlock();
    if (k < 0) {
        dprintf(2, "[shim] Vulkan handle 0x%llx: no alias for the 32-bit guest\n", (unsigned long long)h);
        return (uint32_t)h;
    }
    return (uint32_t)(lxrt_handle_alias.lo + (uint64_t)k * HA_SLOT + (h - page));
}

static uint64_t handle_g2h(uint32_t g)
{
    uint64_t off = (uint64_t)g - lxrt_handle_alias.lo;
    uint64_t page = g_ha_page[off / HA_SLOT];
    return page ? page + off % HA_SLOT : g_base + g;
}
