// MetalFX upscaling for the Vulkan shim's scaling pass (shim/scaler.c,
// LXRT_VK_SCALER=metalfx|auto: spatial; metalfx-temporal: temporal, below).
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
#include <stdio.h>

struct lxrt_mfx_run {
    void *scaler;           // in/out: id<MTLFXSpatialScaler> or a SteamARMMfxTemporal, 0 the first time
    void *queue;            // id<MTLCommandQueue>, or 0: the runtime's own
    void *in, *out;         // id<MTLTexture>
    void *event;            // id<MTLSharedEvent>
    uint64_t wait, signal;
    int mode;               // 0 spatial, 1 temporal
};

// MetalFX temporal, from outside the game (LXRT_VK_SCALER=metalfx-temporal).
//
// The temporal scaler wants, besides the picture, the frame's depth, its
// motion vectors and the sub-pixel jitter the game rendered it with. A game
// presenting through the shim gives none of them (Counter-Strike 2 has no
// temporal pass of its own to take them from), so: a depth texture cleared
// to 1, motion vectors of zero, no jitter -- and a reactive mask (macOS 15)
// worked out here every frame from the difference between this picture and
// the previous one: where a pixel changed, the scaler is told not to trust
// its history, so moving things leave no trails; where it did not, the
// history accumulates and steadies edges and fine detail (less shimmer than
// the spatial scaler). What it cannot do without the game's jitter is
// recover detail the low resolution never rendered, which is what a
// temporal upscaler built into a game does.
@interface SteamARMMfxTemporal : NSObject {
@public
    id<MTLFXTemporalScaler> scaler;
    id<MTLTexture> depth, motion, mask, prev;
    id<MTLComputePipelineState> reactive;
    BOOL fresh;
}
@end
@implementation SteamARMMfxTemporal
- (void)dealloc
{
    [scaler release];
    [depth release];
    [motion release];
    [mask release];
    [prev release];
    [reactive release];
    [super dealloc];
}
@end

static NSString *const kReactiveSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void steamarm_reactive(texture2d<float, access::read> cur [[texture(0)]],\n"
     "                              texture2d<float, access::read> prev [[texture(1)]],\n"
     "                              texture2d<float, access::write> mask [[texture(2)]],\n"
     "                              uint2 g [[thread_position_in_grid]]) {\n"
     "    if (g.x >= mask.get_width() || g.y >= mask.get_height()) return;\n"
     "    float3 d = abs(cur.read(g).rgb - prev.read(g).rgb);\n"
     "    mask.write(float4(smoothstep(0.015, 0.08, max(d.r, max(d.g, d.b)))), g);\n"
     "}\n";

static SteamARMMfxTemporal *temporal_new(id<MTLDevice> dev, id<MTLTexture> in, id<MTLTexture> out)
{
    if (![MTLFXTemporalScalerDescriptor supportsDevice:dev])
        return nil;
    MTLFXTemporalScalerDescriptor *d = [MTLFXTemporalScalerDescriptor new];
    d.inputWidth = in.width;
    d.inputHeight = in.height;
    d.outputWidth = out.width;
    d.outputHeight = out.height;
    d.colorTextureFormat = in.pixelFormat;
    d.depthTextureFormat = MTLPixelFormatDepth32Float;
    d.motionTextureFormat = MTLPixelFormatRG16Float;
    d.outputTextureFormat = out.pixelFormat;
    d.autoExposureEnabled = NO;
    BOOL masked = NO;
    if (@available(macOS 15.0, *)) {
        d.reactiveMaskTextureEnabled = YES;
        d.reactiveMaskTextureFormat = MTLPixelFormatR8Unorm;
        masked = YES;
    }
    id<MTLFXTemporalScaler> sc = [d newTemporalScalerWithDevice:dev];
    [d release];
    if (!sc)
        return nil;
    if ((sc.colorTextureUsage & ~in.usage) || (sc.outputTextureUsage & ~out.usage)) {
        [sc release];
        return nil;
    }
    SteamARMMfxTemporal *t = [SteamARMMfxTemporal new];
    t->scaler = sc;
    t->fresh = YES;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                                                   width:in.width height:in.height mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = sc.depthTextureUsage | MTLTextureUsageRenderTarget;
    t->depth = [dev newTextureWithDescriptor:td];
    td.pixelFormat = MTLPixelFormatRG16Float;
    td.usage = sc.motionTextureUsage | MTLTextureUsageRenderTarget;
    t->motion = [dev newTextureWithDescriptor:td];
    td.pixelFormat = in.pixelFormat;
    td.usage = MTLTextureUsageShaderRead;
    t->prev = [dev newTextureWithDescriptor:td];
    if (masked) {
        if (@available(macOS 15.0, *)) {
            td.pixelFormat = MTLPixelFormatR8Unorm;
            td.usage = sc.reactiveTextureUsage | MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead;
            t->mask = [dev newTextureWithDescriptor:td];
        }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kReactiveSource options:nil error:&err];
        id<MTLFunction> fn = [lib newFunctionWithName:@"steamarm_reactive"];
        if (fn)
            t->reactive = [dev newComputePipelineStateWithFunction:fn error:&err];
        [fn release];
        [lib release];
    }
    if (!t->depth || !t->motion || !t->prev || (masked && (!t->mask || !t->reactive))) {
        [t release];
        return nil;
    }
    return t;
}

// The temporal pass on cb: the reactive mask from this picture and the last,
// the scaler, then this picture kept as the next frame's last.
static void temporal_encode(SteamARMMfxTemporal *t, id<MTLCommandBuffer> cb, id<MTLTexture> in, id<MTLTexture> out)
{
    if (t->fresh) {
        // Depth 1 everywhere, no motion: once, they never change.
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t->motion;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        rp.depthAttachment.texture = t->depth;
        rp.depthAttachment.loadAction = MTLLoadActionClear;
        rp.depthAttachment.clearDepth = 1.0;
        rp.depthAttachment.storeAction = MTLStoreActionStore;
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    }
    if (t->reactive) {
        id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
        [ce setComputePipelineState:t->reactive];
        [ce setTexture:in atIndex:0];
        [ce setTexture:t->prev atIndex:1];
        [ce setTexture:t->mask atIndex:2];
        [ce dispatchThreads:MTLSizeMake(in.width, in.height, 1) threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
        [ce endEncoding];
    }
    id<MTLFXTemporalScaler> sc = t->scaler;
    sc.colorTexture = in;
    sc.depthTexture = t->depth;
    sc.motionTexture = t->motion;
    sc.outputTexture = out;
    if (@available(macOS 15.0, *)) {
        if (t->mask)
            sc.reactiveMaskTexture = t->mask;
    }
    sc.inputContentWidth = in.width;
    sc.inputContentHeight = in.height;
    sc.jitterOffsetX = 0;
    sc.jitterOffsetY = 0;
    sc.motionVectorScaleX = 1;
    sc.motionVectorScaleY = 1;
    sc.depthReversed = NO;
    sc.reset = t->fresh;
    [sc encodeToCommandBuffer:cb];
    id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
    [be copyFromTexture:in toTexture:t->prev];
    [be endEncoding];
    t->fresh = NO;
}

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

// A pass that failed or ran long, on stderr (a GPU timeout here makes macOS
// ignore the process's later command buffers: the game goes black).
static void mfx_watch(id<MTLCommandBuffer> cb, uint64_t wait)
{
    [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
        double ms = (done.GPUEndTime - done.GPUStartTime) * 1000.0;
        if (done.status != MTLCommandBufferStatusCompleted || ms > 100.0)
            fprintf(stderr, "[lxrt] metalfx pass (wait %llu): status %ld error %ld, %.1f ms on the GPU\n",
                    (unsigned long long)wait, (long)done.status, (long)(done.error ? done.error.code : 0), ms);
    }];
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
            if (r->mode == 1) {
                SteamARMMfxTemporal *t = (__bridge SteamARMMfxTemporal *)r->scaler;
                if (!t) {
                    if (!(t = temporal_new(q.device, in, out)))
                        return -95;
                    r->scaler = (__bridge void *)t;
                }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                if (!cb)
                    return -12;
                cb.label = @"SteamARM MetalFX temporal";
                [cb encodeWaitForEvent:ev value:r->wait];
                temporal_encode(t, cb, in, out);
                [cb encodeSignalEvent:ev value:r->signal];
                mfx_watch(cb, r->wait);
                [cb commit];
                return 0;
            }
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
