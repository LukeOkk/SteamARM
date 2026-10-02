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
// images of the shim's own ("virtual", as shim/scaler.c's), and never waits:
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
// LXRT_VK_MAILBOX=0 leaves such swapchains to the driver. FIFO swapchains,
// and those the scaler takes (a picture smaller than its window), are not
// touched. LXRT_VK_DEBUG=1 reports frames shown and dropped.
#include <stdint.h>
#include <stddef.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

extern char *getenv(const char *);
extern int dprintf(int, const char *, ...);
extern void *calloc(size_t, size_t);
extern void free(void *);

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

VkQueue lxrt_scaler_a_queue(void);            // scaler.c: a queue the game got from this shim
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

int lxrt_mailbox_wanted(const VkSwapchainCreateInfoKHR *ci)
{
    const char *e = getenv("LXRT_VK_MAILBOX");
    if (e && *e == '0')
        return 0;
    if (!pthread_create || !pthread_join || !pthread_mutex_lock || !pthread_mutex_unlock || !pthread_cond_wait ||
        !pthread_cond_signal)
        return 0;
    return (ci->presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR || ci->presentMode == VK_PRESENT_MODE_MAILBOX_KHR) &&
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
        VkResult r = lxrt_mvk_vkAcquireNextImageKHR(m->dev, m->sc, UINT64_MAX, m->acq[slot], VK_NULL_HANDLE, &index);
        pthread_mutex_lock(&m->lock);
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
    m->ext = ci->imageExtent;
    VkSwapchainCreateInfoKHR c = *ci;
    c.imageUsage |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
        dprintf(2, "[shim] mailbox: %ux%u, %u images for the game over %u of the driver's\n", m->ext.width,
                m->ext.height, m->nvirt, m->nreal);
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
    VkQueue q = m->q ? m->q : lxrt_scaler_a_queue();
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
    *shown = 1;

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

// LXRT_VK_TIMING: frames shown and dropped by every mailbox since the last call.
void lxrt_mailbox_stats(unsigned *shown, unsigned *dropped)
{
    static unsigned last_shown[16], last_dropped[16];
    *shown = *dropped = 0;
    lock();
    for (int i = 0; i < 16; i++) {
        if (!g_mb[i]) {
            last_shown[i] = last_dropped[i] = 0;
            continue;
        }
        *shown += g_mb[i]->shown - last_shown[i];
        *dropped += g_mb[i]->dropped - last_dropped[i];
        last_shown[i] = g_mb[i]->shown;
        last_dropped[i] = g_mb[i]->dropped;
    }
    unlock();
}
