// A swapchain that does not make the game wait for the display.
//
// The layer a guest's swapchain presents to is shown by another process (a
// CAContext, runtime/remote_layer.m), and the window server takes one of its
// drawables per refresh whatever CAMetalLayer.displaySyncEnabled says:
// vk_x11_present reaches 165 frames a second on a 165 Hz display with FIFO
// and with IMMEDIATE alike, on both drivers (MEASURED, benchmarks/stage52).
// CAMetalLayer has no acquire that does not block, so a game whose frame
// takes a little longer than one refresh waits for the second one: Counter-
// Strike 2 stopped at 82.5 frames a second with 3.3 ms of every frame spent
// in vkAcquireNextImageKHR.
//
// For a swapchain created with IMMEDIATE or MAILBOX the game therefore gets
// images of the shim's own ("virtual", as shim/scaler.c's), and never waits
// (a FIFO one too, since stage 53: see "V-Sync" below):
//   - vkAcquireNextImageKHR hands out the next virtual image at once;
//   - a thread of the shim's acquires the driver's image (that is the call
//     that waits for the display) and says when it has one;
//   - vkQueuePresentKHR copies the game's image into the driver's and
//     presents it if one is ready, and otherwise drops the frame: what
//     MAILBOX means, and what IMMEDIATE can be without tearing.
// The driver's acquire and present never overlap: the thread starts the next
// acquire only when the game's thread has presented the image of the last.
// Everything on the queue is done by the game's own thread, in its own
// vkAcquireNextImageKHR and vkQueuePresentKHR, as the driver's would be.
//
// V-Sync: a FIFO swapchain of the game's goes the same way, with one
// difference: a frame that finds no driver image is never dropped, the game
// waits for the image, and so it is held to the display's rate when it is
// faster, and shows every frame, in step with the display, when it is
// slower -- without the driver's own FIFO, where a game whose GPU work is
// the limit waited at every acquire until the frame before had been shown
// and lost a third of its rate (Counter-Strike 2: 23.5 against 33 frames a
// second; MEASURED, benchmarks/stage53).
// LXRT_VK_MAILBOX=0 leaves every swapchain to the driver; =immediate only
// takes IMMEDIATE and MAILBOX ones (as before stage 53). A picture smaller
// than its window: shim/scaler.c makes its window-sized swapchain one of
// these, and its pass draws into the images here before each present.
// LXRT_VK_DEBUG=1 reports frames shown and dropped.
// LXRT_VK_PROBE=1 reads back one shown frame in every 120 and prints the
// mean colour of eight zones of it (4 x 2): what the game drew, without a
// capture of the screen ("the picture is white": is it the game's image?).
// LXRT_VK_PROBE=2 reads back every shown frame and prints only those that
// are black or white all over, and those that differ from the frame before
// by more than half the range in most zones ("the picture flickers").
#include <stdint.h>
#include <stddef.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

extern char *getenv(const char *);
extern int dprintf(int, const char *, ...);
extern void *calloc(size_t, size_t);
extern void free(void *);
extern void *malloc(size_t);
struct lx_timespec { long tv_sec, tv_nsec; };
extern int clock_gettime(int, struct lx_timespec *);
extern int nanosleep(const struct lx_timespec *, struct lx_timespec *);
static uint64_t now_ns(void)
{
    struct lx_timespec t = { 0, 0 };
    clock_gettime(1 /* CLOCK_MONOTONIC */, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

// The guest's threads: glibc 2.34 and later have them in libc. Weak, so that
// the shim still loads where they are not (the swapchain is then the
// driver's).
typedef unsigned long lx_thread;
typedef union { char bytes[64]; long align; } lx_mutex, lx_cond;   // all zeros: initialised
extern int pthread_create(lx_thread *, const void *, void *(*)(void *), void *) __attribute__((weak));
extern int pthread_join(lx_thread, void **) __attribute__((weak));
extern int pthread_mutex_lock(lx_mutex *) __attribute__((weak));
extern int pthread_mutex_unlock(lx_mutex *) __attribute__((weak));
extern int pthread_cond_wait(lx_cond *, lx_mutex *) __attribute__((weak));
extern int pthread_cond_signal(lx_cond *) __attribute__((weak));

VkResult lxrt_mvk_vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *,
                                       VkSwapchainKHR *);
void lxrt_mvk_vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR, uint32_t *, VkImage *);
VkResult lxrt_mvk_vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *);
VkResult lxrt_mvk_vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR *);
VkResult lxrt_mvk_vkQueueSubmit(VkQueue, uint32_t, const VkSubmitInfo *, VkFence);
void lxrt_mvk_vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice, VkPhysicalDeviceMemoryProperties *);
VkResult lxrt_mvk_vkCreateImage(VkDevice, const VkImageCreateInfo *, const VkAllocationCallbacks *, VkImage *);
void lxrt_mvk_vkDestroyImage(VkDevice, VkImage, const VkAllocationCallbacks *);
void lxrt_mvk_vkGetImageMemoryRequirements(VkDevice, VkImage, VkMemoryRequirements *);
VkResult lxrt_mvk_vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *, const VkAllocationCallbacks *,
                                   VkDeviceMemory *);
void lxrt_mvk_vkFreeMemory(VkDevice, VkDeviceMemory, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize);
VkResult lxrt_mvk_vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *, const VkAllocationCallbacks *,
                                      VkCommandPool *);
void lxrt_mvk_vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *, VkCommandBuffer *);
VkResult lxrt_mvk_vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo *);
VkResult lxrt_mvk_vkEndCommandBuffer(VkCommandBuffer);
void lxrt_mvk_vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags, VkDependencyFlags,
                                   uint32_t, const VkMemoryBarrier *, uint32_t, const VkBufferMemoryBarrier *, uint32_t,
                                   const VkImageMemoryBarrier *);
void lxrt_mvk_vkCmdCopyImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout, uint32_t,
                             const VkImageCopy *);
VkResult lxrt_mvk_vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo *, const VkAllocationCallbacks *,
                                    VkSemaphore *);
void lxrt_mvk_vkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkCreateFence(VkDevice, const VkFenceCreateInfo *, const VkAllocationCallbacks *, VkFence *);
void lxrt_mvk_vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks *);
VkResult lxrt_mvk_vkWaitForFences(VkDevice, uint32_t, const VkFence *, VkBool32, uint64_t);
VkResult lxrt_mvk_vkResetFences(VkDevice, uint32_t, const VkFence *);
VkResult lxrt_mvk_vkDeviceWaitIdle(VkDevice);
VkResult lxrt_mvk_vkCreateBuffer(VkDevice, const VkBufferCreateInfo *, const VkAllocationCallbacks *, VkBuffer *);
void lxrt_mvk_vkDestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks *);
void lxrt_mvk_vkGetBufferMemoryRequirements(VkDevice, VkBuffer, VkMemoryRequirements *);
VkResult lxrt_mvk_vkBindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory, VkDeviceSize);
VkResult lxrt_mvk_vkMapMemory(VkDevice, VkDeviceMemory, VkDeviceSize, VkDeviceSize, VkMemoryMapFlags, void **);
void lxrt_mvk_vkCmdCopyImageToBuffer(VkCommandBuffer, VkImage, VkImageLayout, VkBuffer, uint32_t,
                                     const VkBufferImageCopy *);

VkQueue lxrt_scaler_a_queue(VkDevice);        // scaler.c: a queue of that device the game got from this shim
void lxrt_mvk_vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *);
uint32_t lxrt_scaler_queue_family(VkQueue);

#define MAXV 8      // images the game sees
#define RING 4      // the copy passes in flight: a command buffer, two semaphores and a fence each

typedef struct mailbox {
    VkSwapchainKHR sc;      // the driver's: the handle the game has
    VkDevice dev;
    VkExtent2D ext;
    uint32_t nvirt, nreal, next;
    VkImage virt[MAXV], real[MAXV];
    VkDeviceMemory vmem[MAXV];
    // The copy passes, made at the first present (the queue is known then).
    VkQueue q;
    VkCommandPool pool;
    VkCommandBuffer cb[RING];
    VkSemaphore acq[RING], done[RING];
    VkFence fence[RING];
    int pending[RING];
    uint32_t slot;          // of the acquire in progress or ready
    // The thread that acquires the driver's images.
    lx_thread thread;
    lx_mutex lock;
    lx_cond wake;
    int go, ready, quit, started;
    uint32_t real_index;
    VkResult acquire_result, sticky;   // sticky: what the game is told at its next acquire
    unsigned shown, dropped;
    uint64_t last_shown_ns;
    // The thread's waits in the driver's acquire (LXRT_VK_TIMING): a layer
    // nobody shows gets no drawable for a second at a time.
    uint64_t acquire_ns, acquire_max_ns;
    unsigned acquires;
    int fifo;               // the game's mode: no frame is dropped
    // LXRT_VK_PROBE: a buffer a shown frame is read back into.
    VkPhysicalDevice pd;
    VkFormat format;
    VkBuffer probe;
    VkDeviceMemory probe_mem;
    const unsigned char *probe_map;
} mailbox;

static mailbox *g_mb[16];
static volatile int g_lock;
static void lock(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) ; }
static void unlock(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

// The queue, while a mailbox exists. A game need not keep other threads off
// its queue during vkAcquireNextImageKHR -- DXVK acquires on one thread and
// submits on another -- and the acquire here signals the game's semaphore
// with a batch of its own. So that batch and the game's submits take turns.
// No mailbox: the game's submits go straight to the driver.
static volatile int g_active, g_queue_lock;
extern int sched_yield(void);
static void queue_lock(void)
{
    while (__atomic_exchange_n(&g_queue_lock, 1, __ATOMIC_ACQUIRE))
        sched_yield();
}
static void queue_unlock(void) { __atomic_store_n(&g_queue_lock, 0, __ATOMIC_RELEASE); }

VkResult lxrt_mvk_vkQueueSubmit2(VkQueue, uint32_t, const VkSubmitInfo2 *, VkFence);
VkResult lxrt_mvk_vkQueueSubmit2KHR(VkQueue, uint32_t, const VkSubmitInfo2 *, VkFence);

VkResult lxrt_inner_vkQueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f)
{
    if (!__atomic_load_n(&g_active, __ATOMIC_RELAXED))
        return lxrt_mvk_vkQueueSubmit(q, n, s, f);
    queue_lock();
    VkResult r = lxrt_mvk_vkQueueSubmit(q, n, s, f);
    queue_unlock();
    return r;
}

VkResult lxrt_inner_vkQueueSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f)
{
    if (!__atomic_load_n(&g_active, __ATOMIC_RELAXED))
        return lxrt_mvk_vkQueueSubmit2(q, n, s, f);
    queue_lock();
    VkResult r = lxrt_mvk_vkQueueSubmit2(q, n, s, f);
    queue_unlock();
    return r;
}

VkResult lxrt_inner_vkQueueSubmit2KHR(VkQueue q, uint32_t n, const VkSubmitInfo2 *s, VkFence f)
{
    if (!__atomic_load_n(&g_active, __ATOMIC_RELAXED))
        return lxrt_mvk_vkQueueSubmit2KHR(q, n, s, f);
    queue_lock();
    VkResult r = lxrt_mvk_vkQueueSubmit2KHR(q, n, s, f);
    queue_unlock();
    return r;
}

static int debug(void)
{
    const char *d = getenv("LXRT_VK_DEBUG");
    return d && *d == '1';
}

static int probing(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("LXRT_VK_PROBE");
        on = e && (*e == '1' || *e == '2') ? *e - '0' : 0;
    }
    return on;
}

// LXRT_VK_SNAPSHOT=<guest directory>: when <directory>/request exists, the
// next frame shown is read back and written as <directory>/frame.ppm at
// half size, and the request removed -- the game's own picture, for tests
// that must see a menu. The top right of the picture (13 % of its height)
// is left black: that is where a game shows the player's name and avatar.
extern int access(const char *, int);
extern int unlink(const char *);
extern int open(const char *, int, ...);
extern long write(int, const void *, size_t);
extern int close(int);
static const char *snap_dir(void)
{
    static const char *d = (const char *)1;
    if (d == (const char *)1) {
        d = getenv("LXRT_VK_SNAPSHOT");
        if (d && !*d) d = 0;
    }
    return d;
}

static int snap_requested(void)
{
    extern int snprintf(char *, size_t, const char *, ...);
    const char *d = snap_dir();
    if (!d) return 0;
    char p[512];
    snprintf(p, sizeof p, "%s/request", d);
    return access(p, 0) == 0;
}

static void snap_write(const mailbox *m)
{
    extern int snprintf(char *, size_t, const char *, ...);
    const char *d = snap_dir();
    char p[512];
    uint32_t w = m->ext.width / 2, h = m->ext.height / 2, top = h * 13 / 100;
    int bgr = m->format == VK_FORMAT_B8G8R8A8_UNORM || m->format == VK_FORMAT_B8G8R8A8_SRGB;
    snprintf(p, sizeof p, "%s/frame.ppm", d);
    int fd = open(p, 01 | 0100 | 01000, 0644);              // O_WRONLY|O_CREAT|O_TRUNC (Linux)
    if (fd >= 0) {
        char hdr[64];
        int n = snprintf(hdr, sizeof hdr, "P6\n%u %u\n255\n", w, h);
        write(fd, hdr, (size_t)n);
        unsigned char *row = malloc((size_t)w * 3);
        for (uint32_t y = 0; row && y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                const unsigned char *px = m->probe_map + 4 * ((size_t)(2 * y) * m->ext.width + 2 * x);
                unsigned char *o = row + 3 * x;
                if (y < top && x >= w / 2) { o[0] = o[1] = o[2] = 0; continue; }
                o[0] = bgr ? px[2] : px[0];
                o[1] = px[1];
                o[2] = bgr ? px[0] : px[2];
            }
            write(fd, row, (size_t)w * 3);
        }
        free(row);
        close(fd);
    }
    snprintf(p, sizeof p, "%s/request", d);
    unlink(p);
}

// The read-back buffer, made at the first probe. 0 if there is none.
static int probe_buffer(mailbox *m)
{
    if (m->probe_map)
        return 1;
    if (m->probe)
        return 0;       // failed before
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = (VkDeviceSize)m->ext.width * m->ext.height * 4,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if (lxrt_mvk_vkCreateBuffer(m->dev, &bci, 0, &m->probe))
        return 0;
    VkMemoryRequirements mr;
    lxrt_mvk_vkGetBufferMemoryRequirements(m->dev, m->probe, &mr);
    VkPhysicalDeviceMemoryProperties mp;
    lxrt_mvk_vkGetPhysicalDeviceMemoryProperties(m->pd, &mp);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++)
        if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & want) == want)
            type = t;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, type };
    void *map = 0;
    if (type == UINT32_MAX || lxrt_mvk_vkAllocateMemory(m->dev, &mai, 0, &m->probe_mem) ||
        lxrt_mvk_vkBindBufferMemory(m->dev, m->probe, m->probe_mem, 0) ||
        lxrt_mvk_vkMapMemory(m->dev, m->probe_mem, 0, VK_WHOLE_SIZE, 0, &map))
        return 0;
    m->probe_map = map;
    return 1;
}

// The mean of each channel, as stored, in 4 x 2 zones of the frame read back.
static void probe_report(const mailbox *m)
{
    char line[256];
    int n = 0;
    static unsigned char last[8];
    unsigned char now[8];
    int dark = 0, bright = 0, jumped = 0;
    extern int snprintf(char *, size_t, const char *, ...);
    for (int zy = 0; zy < 2; zy++)
        for (int zx = 0; zx < 4; zx++) {
            unsigned long sum[3] = { 0, 0, 0 }, count = 0;
            uint32_t x0 = m->ext.width * zx / 4, x1 = m->ext.width * (zx + 1) / 4;
            uint32_t y0 = m->ext.height * zy / 2, y1 = m->ext.height * (zy + 1) / 2;
            for (uint32_t y = y0; y < y1; y += 8)
                for (uint32_t x = x0; x < x1; x += 8) {
                    const unsigned char *p = m->probe_map + 4 * ((size_t)y * m->ext.width + x);
                    sum[0] += p[0]; sum[1] += p[1]; sum[2] += p[2];
                    count++;
                }
            if (!count)
                count = 1;
            n += snprintf(line + n, sizeof line - n, " %lu,%lu,%lu", sum[0] / count, sum[1] / count, sum[2] / count);
            unsigned long mean = (sum[0] + sum[1] + sum[2]) / (3 * count);
            int z = zy * 4 + zx;
            now[z] = (unsigned char)mean;
            dark += mean < 4;
            bright += mean > 250;
            jumped += (mean > last[z] ? mean - last[z] : last[z] - mean) > 100;
        }
    for (int z = 0; z < 8; z++)
        last[z] = now[z];
    if (probing() == 2 && dark < 8 && bright < 8 && jumped < 5)
        return;
    dprintf(2, "[shim] probe: frame %u, format %d, zones%s\n", m->shown, (int)m->format, line);
}

int lxrt_mailbox_wanted(const VkSwapchainCreateInfoKHR *ci)
{
    const char *e = getenv("LXRT_VK_MAILBOX");
    if (e && *e == '0')
        return 0;
    if (!pthread_create || !pthread_join || !pthread_mutex_lock || !pthread_mutex_unlock || !pthread_cond_wait ||
        !pthread_cond_signal)
        return 0;
    int fifo = ci->presentMode == VK_PRESENT_MODE_FIFO_KHR || ci->presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    if (fifo && e && *e == 'i')
        return 0;
    return (fifo || ci->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR || ci->presentMode == VK_PRESENT_MODE_MAILBOX_KHR) &&
           ci->imageArrayLayers == 1 && ci->imageExtent.width && ci->imageExtent.height;
}

static mailbox *find(VkSwapchainKHR sc)
{
    mailbox *m = 0;
    lock();
    for (int i = 0; i < 16 && !m; i++)
        if (g_mb[i] && g_mb[i]->sc == sc)
            m = g_mb[i];
    unlock();
    return m;
}

int lxrt_mailbox_is(VkSwapchainKHR sc) { return find(sc) != 0; }

// The thread: one acquire of the driver's image after each "go".
static void *acquirer(void *arg)
{
    mailbox *m = arg;
    for (;;) {
        pthread_mutex_lock(&m->lock);
        while (!m->go && !m->quit)
            pthread_cond_wait(&m->wake, &m->lock);
        int quit = m->quit;
        uint32_t slot = m->slot;
        m->go = 0;
        pthread_mutex_unlock(&m->lock);
        if (quit)
            return 0;
        uint32_t index = 0;
        // Waits for the display: up to a refresh, or the driver's own limit.
        uint64_t t0 = now_ns();
        VkResult r = lxrt_mvk_vkAcquireNextImageKHR(m->dev, m->sc, UINT64_MAX, m->acq[slot], VK_NULL_HANDLE, &index);
        uint64_t dt = now_ns() - t0;
        pthread_mutex_lock(&m->lock);
        m->acquire_ns += dt;
        m->acquires++;
        if (dt > m->acquire_max_ns)
            m->acquire_max_ns = dt;
        m->acquire_result = r;
        m->real_index = index;
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
            m->ready = 1;
        if (r != VK_SUCCESS)
            m->sticky = r;      // out of date, surface lost, suboptimal: the game hears of it
        int stop = r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR;
        pthread_mutex_unlock(&m->lock);
        if (stop)
            return 0;           // no image will come from this swapchain any more
    }
}

static void stop_thread(mailbox *m)
{
    if (!m->started)
        return;
    pthread_mutex_lock(&m->lock);
    m->quit = 1;
    pthread_cond_signal(&m->wake);
    pthread_mutex_unlock(&m->lock);
    pthread_join(m->thread, 0);
    m->started = 0;
}

static void destroy(mailbox *m)
{
    stop_thread(m);
    VkDevice d = m->dev;
    if (m->pool) {
        lxrt_mvk_vkDeviceWaitIdle(d);
        lxrt_mvk_vkDestroyCommandPool(d, m->pool, 0);
    }
    for (int k = 0; k < RING; k++) {
        if (m->acq[k]) lxrt_mvk_vkDestroySemaphore(d, m->acq[k], 0);
        if (m->done[k]) lxrt_mvk_vkDestroySemaphore(d, m->done[k], 0);
        if (m->fence[k]) lxrt_mvk_vkDestroyFence(d, m->fence[k], 0);
    }
    if (m->probe) lxrt_mvk_vkDestroyBuffer(d, m->probe, 0);
    if (m->probe_mem) lxrt_mvk_vkFreeMemory(d, m->probe_mem, 0);
    for (uint32_t i = 0; i < MAXV; i++) {
        if (m->virt[i]) lxrt_mvk_vkDestroyImage(d, m->virt[i], 0);
        if (m->vmem[i]) lxrt_mvk_vkFreeMemory(d, m->vmem[i], 0);
    }
    if (debug())
        dprintf(2, "[shim] mailbox: %u frames shown, %u dropped\n", m->shown, m->dropped);
    free(m);
}

// The game's swapchain. VK_SUCCESS with *out set, or an error and nothing
// left behind (the caller then creates the swapchain the plain way).
VkResult lxrt_mailbox_create(VkDevice dev, VkPhysicalDevice pd, const VkSwapchainCreateInfoKHR *ci,
                             const VkAllocationCallbacks *alloc, VkSwapchainKHR *out)
{
    if (!pd)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    mailbox *m = calloc(1, sizeof *m);
    if (!m)
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    m->dev = dev;
    m->pd = pd;
    m->format = ci->imageFormat;
    m->ext = ci->imageExtent;
    m->fifo = ci->presentMode == VK_PRESENT_MODE_FIFO_KHR || ci->presentMode == VK_PRESENT_MODE_FIFO_RELAXED_KHR;
    VkSwapchainCreateInfoKHR c = *ci;
    c.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // The driver's own swapchain is FIFO: its presents follow the display.
    // The game does not wait for them (that is the thread's part), and an
    // IMMEDIATE layer that covers the display is flipped by the window
    // server outside its refreshes (LXRT_VK_MAILBOX_REAL=immediate: as the
    // game asked).
    const char *real = getenv("LXRT_VK_MAILBOX_REAL");
    if (!(real && real[0] == 'i')) {
        c.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        // Three of the driver's images: one on the display, one that the GPU
        // has not finished yet (a frame's worth of time when the GPU is the
        // limit) and one for the thread to acquire meanwhile. With two, a
        // third of the frames found none ready and were dropped (MEASURED:
        // 23 of 68 at 34 frames a second).
        if (c.minImageCount < 3)
            c.minImageCount = 3;
    }
    VkResult r = lxrt_mvk_vkCreateSwapchainKHR(dev, &c, alloc, &m->sc);
    if (r) {
        free(m);
        return r;
    }
    uint32_t n = MAXV;
    r = lxrt_mvk_vkGetSwapchainImagesKHR(dev, m->sc, &n, m->real);
    if (r != VK_SUCCESS)
        goto fail;
    m->nreal = n;
    // One more than the game asked for, at least three: the one it draws
    // into, the one last presented and still to be copied in queue order,
    // and one to spare.
    m->nvirt = ci->minImageCount + 1 < 3 ? 3 : ci->minImageCount + 1;
    if (m->nvirt > MAXV)
        m->nvirt = MAXV;

    VkPhysicalDeviceMemoryProperties mp;
    lxrt_mvk_vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = ci->imageFormat,
        .extent = { ci->imageExtent.width, ci->imageExtent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = ci->imageSharingMode,
        .queueFamilyIndexCount = ci->queueFamilyIndexCount,
        .pQueueFamilyIndices = ci->pQueueFamilyIndices,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    // The formats of a mutable-format swapchain go to its images as they are.
    VkImageFormatListCreateInfo fl;
    for (const VkBaseInStructure *p = ci->pNext; p; p = p->pNext)
        if (p->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            fl = *(const VkImageFormatListCreateInfo *)p;
            fl.pNext = 0;
            ii.pNext = &fl;
        }
    for (uint32_t i = 0; i < m->nvirt; i++) {
        if ((r = lxrt_mvk_vkCreateImage(dev, &ii, 0, &m->virt[i])))
            goto fail;
        VkMemoryRequirements mr;
        lxrt_mvk_vkGetImageMemoryRequirements(dev, m->virt[i], &mr);
        uint32_t type = UINT32_MAX;
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++)
            if ((mr.memoryTypeBits & (1u << t)) && (mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                type = t;
        for (uint32_t t = 0; t < mp.memoryTypeCount && type == UINT32_MAX; t++)
            if (mr.memoryTypeBits & (1u << t))
                type = t;
        VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, mr.size, type };
        if (type == UINT32_MAX || (r = lxrt_mvk_vkAllocateMemory(dev, &mai, 0, &m->vmem[i])) ||
            (r = lxrt_mvk_vkBindImageMemory(dev, m->virt[i], m->vmem[i], 0))) {
            r = r ? r : VK_ERROR_OUT_OF_DEVICE_MEMORY;
            goto fail;
        }
    }
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (int k = 0; k < RING; k++)
        if ((r = lxrt_mvk_vkCreateSemaphore(dev, &sci, 0, &m->acq[k])) ||
            (r = lxrt_mvk_vkCreateSemaphore(dev, &sci, 0, &m->done[k])) ||
            (r = lxrt_mvk_vkCreateFence(dev, &fci, 0, &m->fence[k])))
            goto fail;

    lock();
    int k = -1;
    for (int i = 0; i < 16 && k < 0; i++)
        if (!g_mb[i])
            k = i;
    if (k >= 0)
        g_mb[k] = m;
    unlock();
    if (k < 0) {
        r = VK_ERROR_TOO_MANY_OBJECTS;
        goto fail;
    }
    // The first driver image is acquired while the game draws its first frame.
    m->go = 1;
    if (pthread_create(&m->thread, 0, acquirer, m) != 0) {
        lock();
        g_mb[k] = 0;
        unlock();
        r = VK_ERROR_INITIALIZATION_FAILED;
        goto fail;
    }
    m->started = 1;
    __atomic_add_fetch(&g_active, 1, __ATOMIC_RELAXED);
    if (debug())
        dprintf(2, "[shim] mailbox: %ux%u, %u images for the game over %u of the driver's%s\n", m->ext.width,
                m->ext.height, m->nvirt, m->nreal, m->fifo ? ", FIFO (every frame shown)" : "");
    *out = m->sc;
    return VK_SUCCESS;
fail:
    lxrt_mvk_vkDestroySwapchainKHR(dev, m->sc, alloc);
    m->sc = VK_NULL_HANDLE;
    destroy(m);
    return r ? r : VK_ERROR_INITIALIZATION_FAILED;
}

// A swapchain about to be replaced (oldSwapchain) or destroyed: its thread
// must not be inside the driver's acquire while the driver retires it.
void lxrt_mailbox_retire(VkSwapchainKHR sc)
{
    mailbox *m = sc ? find(sc) : 0;
    if (m)
        stop_thread(m);
}

int lxrt_mailbox_destroy(VkDevice dev, VkSwapchainKHR sc, const VkAllocationCallbacks *alloc)
{
    mailbox *m = find(sc);
    if (!m)
        return 0;
    lock();
    for (int i = 0; i < 16; i++)
        if (g_mb[i] == m)
            g_mb[i] = 0;
    unlock();
    stop_thread(m);
    destroy(m);
    lxrt_mvk_vkDestroySwapchainKHR(dev, sc, alloc);
    __atomic_sub_fetch(&g_active, 1, __ATOMIC_RELAXED);
    return 1;
}

VkResult lxrt_mailbox_images(VkSwapchainKHR sc, uint32_t *count, VkImage *images)
{
    mailbox *m = find(sc);
    if (!images) {
        *count = m->nvirt;
        return VK_SUCCESS;
    }
    uint32_t k = *count < m->nvirt ? *count : m->nvirt;
    for (uint32_t i = 0; i < k; i++)
        images[i] = m->virt[i];
    *count = k;
    return k < m->nvirt ? VK_INCOMPLETE : VK_SUCCESS;
}

// The game's acquire: the next of its own images, at once. Its semaphore and
// fence are signalled by an empty batch on the queue it presents with.
VkResult lxrt_mailbox_acquire(VkSwapchainKHR sc, VkSemaphore sem, VkFence fence, uint32_t *index)
{
    mailbox *m = find(sc);
    pthread_mutex_lock(&m->lock);
    VkResult sticky = m->sticky;
    m->sticky = VK_SUCCESS;
    pthread_mutex_unlock(&m->lock);
    if (sticky != VK_SUCCESS && sticky != VK_SUBOPTIMAL_KHR)
        return sticky;                  // out of date: the game makes a new swapchain
    // Before the game's first present: a queue of the swapchain's own device
    // (never another device's: it may have been destroyed). The game always
    // gets one before it can draw; if it got it some other way, the first
    // queue of family 0, which every driver here has.
    VkQueue q = m->q ? m->q : lxrt_scaler_a_queue(m->dev);
    if (!q && (sem || fence))
        lxrt_mvk_vkGetDeviceQueue(m->dev, 0, 0, &q);
    if (sem || fence) {
        if (!q)
            return VK_ERROR_DEVICE_LOST;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = sem ? 1 : 0, .pSignalSemaphores = &sem };
        queue_lock();
        VkResult r = lxrt_mvk_vkQueueSubmit(q, 1, &si, fence);
        queue_unlock();
        if (r)
            return r;
    }
    *index = m->next;
    m->next = (m->next + 1) % m->nvirt;
    return sticky;
}

static void image_barrier(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    lxrt_mvk_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, 0, 0,
                                  0, 1, &b);
}

// The game's present of its image i, waiting for `waits`. *shown: 1 if a
// driver image took it.
VkResult lxrt_mailbox_present(VkQueue q, VkSwapchainKHR sc, uint32_t i, const VkSemaphore *waits, uint32_t nwaits,
                              int *shown)
{
    mailbox *m = find(sc);
    VkPipelineStageFlags stages[17];
    for (int k = 0; k < 17; k++)
        stages[k] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    if (nwaits > 16)
        nwaits = 16;
    *shown = 0;
    if (i >= m->nvirt)
        return VK_ERROR_VALIDATION_FAILED_EXT;
    m->q = q;
    if (!m->pool) {
        VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0,
                                        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, lxrt_scaler_queue_family(q) };
        VkResult r = lxrt_mvk_vkCreateCommandPool(m->dev, &pci, 0, &m->pool);
        if (r)
            return r;
        VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, m->pool,
                                            VK_COMMAND_BUFFER_LEVEL_PRIMARY, RING };
        if ((r = lxrt_mvk_vkAllocateCommandBuffers(m->dev, &cai, m->cb)))
            return r;
    }

    pthread_mutex_lock(&m->lock);
    int ready = m->ready;
    uint32_t real = m->real_index, slot = m->slot;
    VkResult acquired = m->acquire_result;
    if (ready)
        m->ready = 0;
    pthread_mutex_unlock(&m->lock);

    // No driver image yet. A game faster than the display drops this frame
    // (the one before was shown a moment ago). A slow one -- the GPU is the
    // limit, and the driver's image comes back only when the frame before
    // has reached the display, a few milliseconds after the game's next
    // present -- waits for it instead: every one of its frames is shown,
    // and the wait is time its GPU is busy anyway (MEASURED: 34 frames a
    // second drawn, 22 shown without the wait).
    // With the game's own FIFO (V-Sync) the wait is for as long as it takes:
    // one driver image comes back per refresh, so this is what holds a fast
    // game to the display's rate. (Up to a second: the thread may have met
    // an error, which the game hears of at its next acquire.)
    if (!ready && (m->fifo || now_ns() - m->last_shown_ns > 13000000ull)) {
        const uint64_t deadline = now_ns() + (m->fifo ? 1000000000ull : 30000000ull);
        const struct lx_timespec nap = { 0, 250000 };
        while (!ready && now_ns() < deadline) {
            nanosleep(&nap, 0);
            pthread_mutex_lock(&m->lock);
            ready = m->ready;
            if (ready) {
                m->ready = 0;
                real = m->real_index;
                slot = m->slot;
                acquired = m->acquire_result;
            }
            pthread_mutex_unlock(&m->lock);
        }
    }

    if (!ready) {
        // No driver image yet: this frame is not shown. What it waited for
        // is still consumed, so that the game can signal it again.
        m->dropped++;
        if (!nwaits)
            return VK_SUCCESS;
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = nwaits,
                            .pWaitSemaphores = waits, .pWaitDstStageMask = stages };
        queue_lock();
        VkResult dr = lxrt_mvk_vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
        queue_unlock();
        return dr;
    }

    if (m->pending[slot]) {
        lxrt_mvk_vkWaitForFences(m->dev, 1, &m->fence[slot], VK_TRUE, UINT64_MAX);
        lxrt_mvk_vkResetFences(m->dev, 1, &m->fence[slot]);
        m->pending[slot] = 0;
    }
    VkCommandBuffer cb = m->cb[slot];
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0,
                                    VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VkResult r = lxrt_mvk_vkBeginCommandBuffer(cb, &bi);
    if (r)
        return r;
    image_barrier(cb, m->virt[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    image_barrier(cb, m->real[real], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy region = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           { 0, 0, 0 }, { m->ext.width, m->ext.height, 1 } };
    lxrt_mvk_vkCmdCopyImage(cb, m->virt[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m->real[real],
                            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    int snap = snap_requested();
    int probe = ((probing() && (probing() == 2 || m->shown % 120 == 119)) || snap) && probe_buffer(m);
    if (probe) {
        VkBufferImageCopy out = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                  .imageExtent = { m->ext.width, m->ext.height, 1 } };
        lxrt_mvk_vkCmdCopyImageToBuffer(cb, m->virt[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m->probe, 1, &out);
    }
    image_barrier(cb, m->real[real], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    image_barrier(cb, m->virt[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    if ((r = lxrt_mvk_vkEndCommandBuffer(cb)))
        return r;

    // After what the game's present waited for, and after the acquire.
    VkSemaphore all[17];
    for (uint32_t k = 0; k < nwaits; k++)
        all[k] = waits[k];
    all[nwaits] = m->acq[slot];
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = nwaits + 1,
        .pWaitSemaphores = all,
        .pWaitDstStageMask = stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &m->done[slot],
    };
    // The game's own thread, but another of its threads may be in an
    // acquire of ours (queue_lock).
    queue_lock();
    if ((r = lxrt_mvk_vkQueueSubmit(q, 1, &si, m->fence[slot]))) {
        queue_unlock();
        return r;
    }
    m->pending[slot] = 1;
    VkPresentInfoKHR pi = { .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1,
                            .pWaitSemaphores = &m->done[slot], .swapchainCount = 1, .pSwapchains = &m->sc,
                            .pImageIndices = &real };
    r = lxrt_mvk_vkQueuePresentKHR(q, &pi);
    queue_unlock();
    m->shown++;
    m->last_shown_ns = now_ns();
    *shown = 1;
    if (probe && lxrt_mvk_vkWaitForFences(m->dev, 1, &m->fence[slot], VK_TRUE, UINT64_MAX) == VK_SUCCESS) {
        if (snap)
            snap_write(m);
        if (probing())
            probe_report(m);
    }

    // The driver's present is done: the next acquire may start.
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        pthread_mutex_lock(&m->lock);
        m->slot = (slot + 1) % RING;
        m->go = 1;
        pthread_cond_signal(&m->wake);
        pthread_mutex_unlock(&m->lock);
    }
    if (r == VK_SUCCESS && acquired == VK_SUBOPTIMAL_KHR)
        r = VK_SUBOPTIMAL_KHR;
    return r;
}

// LXRT_VK_TIMING: frames shown and dropped by every mailbox since the last
// call, and the thread's waits in the driver's acquire (average and longest,
// microseconds).
void lxrt_mailbox_stats(unsigned *shown, unsigned *dropped, unsigned *acq_avg_us, unsigned *acq_max_us)
{
    static unsigned last_shown[16], last_dropped[16], last_acquires[16];
    static uint64_t last_acquire_ns[16];
    uint64_t ns = 0, max = 0;
    unsigned n = 0;
    *shown = *dropped = 0;
    lock();
    for (int i = 0; i < 16; i++) {
        mailbox *m = g_mb[i];
        if (!m) {
            last_shown[i] = last_dropped[i] = last_acquires[i] = 0;
            last_acquire_ns[i] = 0;
            continue;
        }
        *shown += m->shown - last_shown[i];
        *dropped += m->dropped - last_dropped[i];
        last_shown[i] = m->shown;
        last_dropped[i] = m->dropped;
        pthread_mutex_lock(&m->lock);
        n += m->acquires - last_acquires[i];
        ns += m->acquire_ns - last_acquire_ns[i];
        last_acquires[i] = m->acquires;
        last_acquire_ns[i] = m->acquire_ns;
        if (m->acquire_max_ns > max)
            max = m->acquire_max_ns;
        m->acquire_max_ns = 0;
        pthread_mutex_unlock(&m->lock);
    }
    unlock();
    *acq_avg_us = n ? (unsigned)(ns / n / 1000) : 0;
    *acq_max_us = (unsigned)(max / 1000);
}
