// Swapchain policy test with synthetic eligibility; does not establish VRR.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../shim/present.c"

const char *lxrt_vk_driver = "moltenvk";
static int eligible, stretched, offers_fifo = 1, plain, mailbox_calls, scaler_calls;
static VkPresentModeKHR passed;
int lxrt_wsi_adaptive_sync(VkSurfaceKHR s) { return eligible; }
int lxrt_wsi_layer_extent(VkSurfaceKHR s, uint32_t *w, uint32_t *h) { return 0; }
VkResult lxrt_inner_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice p, VkSurfaceKHR s, void *out)
{
    if (!stretched) return VK_ERROR_SURFACE_LOST_KHR;
    VkSurfaceCapabilitiesKHR *c = out;
    *c = (VkSurfaceCapabilitiesKHR){.currentExtent = {1600, 1200}};
    return VK_SUCCESS;
}
VkResult lxrt_mvk_vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice p, VkSurfaceKHR s,
                                                          uint32_t *n, VkPresentModeKHR *m)
{
    m[0] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    *n = 1;
    if (offers_fifo) m[(*n)++] = VK_PRESENT_MODE_FIFO_KHR;
    return VK_SUCCESS;
}
VkResult lxrt_mvk_vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *c,
                                     const VkAllocationCallbacks *a, VkSwapchainKHR *s)
{ plain++; passed = c->presentMode; *s = (VkSwapchainKHR)2; return VK_SUCCESS; }
int lxrt_mailbox_wanted(const VkSwapchainCreateInfoKHR *c) { passed = c->presentMode; return 1; }
VkResult lxrt_mailbox_create(VkDevice d, VkPhysicalDevice p, const VkSwapchainCreateInfoKHR *c,
                            const VkAllocationCallbacks *a, VkSwapchainKHR *s)
{ mailbox_calls++; *s = (VkSwapchainKHR)3; return VK_SUCCESS; }
void lxrt_mailbox_retire(VkSwapchainKHR s) {}
void lxrt_scaler_note_plain(VkDevice d, VkSwapchainKHR s, VkExtent2D e, VkFormat f) {}
VkResult lxrt_scaler_create(VkDevice d, VkPhysicalDevice p, const VkSwapchainCreateInfoKHR *c, VkExtent2D e,
                           const VkAllocationCallbacks *a, VkSwapchainKHR *s)
{ scaler_calls++; *s = (VkSwapchainKHR)4; return VK_SUCCESS; }

static void check(int expect_plain, VkPresentModeKHR expect_mode, int expect_scaler)
{
    plain = mailbox_calls = scaler_calls = 0;
    VkSwapchainCreateInfoKHR c = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = (VkSurfaceKHR)1, .imageExtent = {800, 600}, .presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR};
    VkSwapchainKHR s;
    assert(lxrt_inner_vkCreateSwapchainKHR((VkDevice)1, &c, 0, &s) == VK_SUCCESS);
    assert(c.presentMode == VK_PRESENT_MODE_IMMEDIATE_KHR); // caller stays const
    assert(plain == expect_plain && scaler_calls == expect_scaler);
    assert(mailbox_calls == (!expect_plain && !expect_scaler));
    if (!expect_scaler) assert(passed == expect_mode);
}

int main(void)
{
    unsetenv("LXRT_VK_PRESENT_MODE"); unsetenv("LXRT_VK_ADAPTIVE_SYNC");
    lxrt_note_device((VkDevice)1, (VkPhysicalDevice)1);
    eligible = 1; check(0, VK_PRESENT_MODE_IMMEDIATE_KHR, 0);
    setenv("LXRT_VK_ADAPTIVE_SYNC", "1", 1);
    eligible = 0; check(0, VK_PRESENT_MODE_IMMEDIATE_KHR, 0);
    eligible = 1; check(1, VK_PRESENT_MODE_FIFO_KHR, 0);
    offers_fifo = 0; check(0, VK_PRESENT_MODE_IMMEDIATE_KHR, 0);
    offers_fifo = 1;
    setenv("LXRT_VK_PRESENT_MODE", "IMMEDIATE", 1); check(0, VK_PRESENT_MODE_IMMEDIATE_KHR, 0);
    unsetenv("LXRT_VK_PRESENT_MODE");
    stretched = 1; check(0, VK_PRESENT_MODE_IMMEDIATE_KHR, 1);
    puts("Adaptive Sync present policy: PASS (synthetic, hardware activation unverified)");
    return 0;
}
