/* The mailbox's real predicate/wake code with a controlled drawable source.
 * Build with -ffunction-sections -Wl,--gc-sections to discard unused Vulkan
 * entry points. No display, GPU work, or game settings are involved. */
#define pthread_cond_clockwait mailbox_test_clockwait
#include "../../shim/mailbox.c"
#undef pthread_cond_clockwait

extern int pthread_cond_clockwait(lx_cond *, lx_mutex *, int, const struct lx_timespec *);
extern int printf(const char *, ...);
static int unsupported_clock;
static unsigned acquire_delay_us = 3000;
static VkResult acquire_return = VK_SUCCESS;

int mailbox_test_clockwait(lx_cond *c, lx_mutex *m, int clock, const struct lx_timespec *until)
{
    return unsupported_clock ? 22 /* EINVAL */ : pthread_cond_clockwait(c, m, clock, until);
}

VkResult lxrt_mvk_vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR s, uint64_t timeout,
                                       VkSemaphore sem, VkFence fence, uint32_t *index)
{
    const struct lx_timespec nap = { 0, (long)acquire_delay_us * 1000 };
    nanosleep(&nap, 0);
    *index = 7;
    return acquire_return;
}

static int drawable_case(VkResult result)
{
    mailbox m = { 0 };
    acquire_return = result;
    m.go = 1;
    if (pthread_create(&m.thread, 0, acquirer, &m))
        return 1;
    m.started = 1;
    uint64_t start = now_ns();
    pthread_mutex_lock(&m.lock);
    wait_ready_locked(&m, start + 50000000ull);
    uint64_t dt = now_ns() - start;
    int bad = dt >= 50000000ull || dt < 1000000ull ||
              (result == VK_SUCCESS ? !m.ready || m.real_index != 7 : m.ready || m.sticky != result);
    pthread_mutex_unlock(&m.lock);
    stop_thread(&m);
    pthread_cond_destroy(&m.ready_wake);
    pthread_cond_destroy(&m.wake);
    printf("%s wake: %.3f ms, %s\n", result == VK_SUCCESS ? "drawable" : "terminal error", dt / 1e6,
           bad ? "FAIL" : "ok");
    return bad;
}

static void *spurious_notifications(void *arg)
{
    mailbox *m = arg;
    const struct lx_timespec nap = { 0, 1000000 };
    for (int i = 0; i < 4; i++) {
        nanosleep(&nap, 0);
        pthread_mutex_lock(&m->lock);
        if (i == 3)
            m->ready = 1;
        pthread_cond_signal(&m->ready_wake);
        pthread_mutex_unlock(&m->lock);
    }
    return 0;
}

static void *stop_waiting_acquirer(void *arg)
{
    const struct lx_timespec nap = { 0, 3000000 };
    nanosleep(&nap, 0);
    stop_thread(arg);
    return 0;
}

static int stop_case(void)
{
    mailbox m = { 0 };
    lx_thread stopper;
    if (pthread_create(&m.thread, 0, acquirer, &m))
        return 1;
    m.started = 1;
    if (pthread_create(&stopper, 0, stop_waiting_acquirer, &m)) {
        stop_thread(&m);
        return 1;
    }
    uint64_t start = now_ns();
    pthread_mutex_lock(&m.lock);
    wait_ready_locked(&m, start + 50000000ull);
    uint64_t dt = now_ns() - start;
    int bad = !m.quit || m.ready || dt >= 50000000ull;
    pthread_mutex_unlock(&m.lock);
    pthread_join(stopper, 0);
    pthread_cond_destroy(&m.ready_wake);
    pthread_cond_destroy(&m.wake);
    printf("stop/cleanup wake: %.3f ms, %s\n", dt / 1e6, bad ? "FAIL" : "ok");
    return bad;
}

int main(int argc, char **argv)
{
    unsupported_clock = argc > 1;
    int bad = drawable_case(VK_SUCCESS) | drawable_case(VK_ERROR_OUT_OF_DATE_KHR) | stop_case();
    mailbox m = { 0 };
    uint64_t start = now_ns();
    pthread_mutex_lock(&m.lock);
    wait_ready_locked(&m, start + 3000000ull);
    uint64_t dt = now_ns() - start;
    pthread_mutex_unlock(&m.lock);
    int timeout_bad = m.ready || dt < 2000000ull || dt > 100000000ull;
    bad |= timeout_bad;
    printf("bounded timeout: %.3f ms, %s\n", dt / 1e6, timeout_bad ? "FAIL" : "ok");
    lx_thread t;
    if (pthread_create(&t, 0, spurious_notifications, &m))
        return 1;
    start = now_ns();
    pthread_mutex_lock(&m.lock);
    wait_ready_locked(&m, start + 50000000ull);
    dt = now_ns() - start;
    int spurious_bad = !m.ready || dt < 3000000ull || dt >= 50000000ull;
    pthread_mutex_unlock(&m.lock);
    pthread_join(t, 0);
    pthread_cond_destroy(&m.ready_wake);
    bad |= spurious_bad;
    printf("spurious notifications: %.3f ms, %s\n", dt / 1e6, spurious_bad ? "FAIL" : "ok");
    printf("== mailbox wait (%s): %s\n", unsupported_clock ? "unsupported clock fallback" : "clock wait", bad ? "FAIL" : "ok");
    return bad;
}
