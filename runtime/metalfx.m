// MetalFX spatial upscaling for the Vulkan shim's scaling pass
// (shim/scaler.c, LXRT_VK_SCALER=metalfx).
//
// The shim exports the Metal objects behind its Vulkan ones
// (VK_EXT_metal_objects: the queue, the game's picture, the output texture,
// a timeline semaphore's MTLSharedEvent) and asks the runtime to put Apple's
// spatial scaler between two of its Vulkan submissions: a command buffer of
// its own on MoltenVK's MTLCommandQueue that waits for the event's value
// `wait` (the first submission is done with the picture), scales, and
// signals `signal` (the second submission, which copies the output to the
// swapchain image, waits for it).
//
// Manual reference counting, like the rest of the runtime: the scaler is
// created on first use from the two textures' sizes and formats, kept in
// run->scaler (+1), and released by lxrt_mfx_release.
#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>
#include <stdint.h>

struct lxrt_mfx_run {
    void *scaler;           // in/out: id<MTLFXSpatialScaler>, 0 the first time
    void *queue;            // id<MTLCommandQueue>
    void *in, *out;         // id<MTLTexture>
    void *event;            // id<MTLSharedEvent>
    uint64_t wait, signal;
};

long lxrt_mfx_encode(struct lxrt_mfx_run *r)
{
    if (!r || !r->queue || !r->in || !r->out || !r->event)
        return -22;
    if (@available(macOS 13.0, *)) {
        @autoreleasepool {
            id<MTLCommandQueue> q = (__bridge id<MTLCommandQueue>)r->queue;
            id<MTLTexture> in = (__bridge id<MTLTexture>)r->in;
            id<MTLTexture> out = (__bridge id<MTLTexture>)r->out;
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
