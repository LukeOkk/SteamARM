// MetalFX spatial upscaling for the Vulkan shim's scaling pass
// (shim/scaler.c, LXRT_VK_SCALER=metalfx|auto).
//
// The shim exports the Metal objects behind its Vulkan ones
// (VK_EXT_metal_objects: the game's picture, the output texture, a timeline
// semaphore's MTLSharedEvent, and the queue where the driver has a classic
// one) and asks the runtime to put Apple's spatial scaler between two of its
// Vulkan submissions: a command buffer of its own that waits for the event's
// value `wait` (the first submission is done with the picture), scales, and
// signals `signal` (the second submission, which copies the output to the
// swapchain image, waits for it).
//
// The queue: MoltenVK's MTLCommandQueue when the shim got one. KosmicKrisp's
// queue is an MTL4CommandQueue and is not exported
// (patches/kosmickrisp-10-metal-objects.patch), so run->queue is 0 there and
// the command buffer goes on a classic queue of the runtime's own on the
// textures' device; the shared event orders it against the driver's queue
// either way.
//
// Manual reference counting, like the rest of the runtime: the scaler is
// created on first use from the two textures' sizes and formats, kept in
// run->scaler (+1), and released by lxrt_mfx_release. The runtime's own
// queue lives as long as the process.
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <os/lock.h>
#include <stdint.h>

struct lxrt_mfx_run {
    void *scaler;           // in/out: id<MTLFXSpatialScaler>, 0 the first time
    void *queue;            // id<MTLCommandQueue>, or 0: the runtime's own
    void *in, *out;         // id<MTLTexture>
    void *event;            // id<MTLSharedEvent>
    uint64_t wait, signal;
};

// One queue per device, made on first use (a process has one GPU here).
static os_unfair_lock g_queue_lock = OS_UNFAIR_LOCK_INIT;
static id<MTLDevice> g_queue_device;
static id<MTLCommandQueue> g_queue;

static id<MTLCommandQueue> own_queue(id<MTLDevice> dev)
{
    id<MTLCommandQueue> q = nil;
    os_unfair_lock_lock(&g_queue_lock);
    if (g_queue && g_queue_device != dev) {
        [g_queue release];
        [g_queue_device release];
        g_queue = nil;
        g_queue_device = nil;
    }
    if (!g_queue && dev) {
        g_queue = [dev newCommandQueue];
        if (g_queue) {
            g_queue.label = @"SteamARM MetalFX";
            g_queue_device = [dev retain];
        }
    }
    q = g_queue;
    os_unfair_lock_unlock(&g_queue_lock);
    return q;
}

long lxrt_mfx_encode(struct lxrt_mfx_run *r)
{
    if (!r || !r->in || !r->out || !r->event)
        return -22;
    if (@available(macOS 13.0, *)) {
        @autoreleasepool {
            id<MTLTexture> in = (__bridge id<MTLTexture>)r->in;
            id<MTLTexture> out = (__bridge id<MTLTexture>)r->out;
            id<MTLCommandQueue> q = r->queue ? (__bridge id<MTLCommandQueue>)r->queue : own_queue(in.device);
            if (!q)
                return -12;
            id<MTLSharedEvent> ev = (__bridge id<MTLSharedEvent>)r->event;
            id<MTLFXSpatialScaler> sc = (__bridge id<MTLFXSpatialScaler>)r->scaler;
            if (!sc) {
                if (![MTLFXSpatialScalerDescriptor supportsDevice:q.device])
                    return -95;
                MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
                d.inputWidth = in.width;
                d.inputHeight = in.height;
                d.outputWidth = out.width;
                d.outputHeight = out.height;
                d.colorTextureFormat = in.pixelFormat;
                d.outputTextureFormat = out.pixelFormat;
                d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
                sc = [d newSpatialScalerWithDevice:q.device];
                [d release];
                if (!sc)
                    return -95;
                // Textures the scaler cannot use would stop the process in
                // Metal's validation, not fail here: refuse instead, and the
                // shim blits.
                if ((sc.colorTextureUsage & ~in.usage) || (sc.outputTextureUsage & ~out.usage)) {
                    [sc release];
                    return -95;
                }
                r->scaler = (__bridge void *)sc;
            }
            id<MTLCommandBuffer> cb = [q commandBuffer];
            if (!cb)
                return -12;
            cb.label = @"SteamARM MetalFX";
            [cb encodeWaitForEvent:ev value:r->wait];
            sc.colorTexture = in;
            sc.outputTexture = out;
            sc.inputContentWidth = in.width;
            sc.inputContentHeight = in.height;
            [sc encodeToCommandBuffer:cb];
            [cb encodeSignalEvent:ev value:r->signal];
            [cb commit];
        }
        return 0;
    }
    return -95;
}

void lxrt_mfx_release(void *scaler)
{
    if (scaler)
        [(__bridge id)scaler release];
}
