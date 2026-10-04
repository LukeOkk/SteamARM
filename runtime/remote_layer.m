// A CAMetalLayer another process can display (native-window stage W2).
//
// A Linux game renders with Vulkan -> MoltenVK into a CAMetalLayer that lives
// in ITS process, while its window belongs to the X server (another macOS
// process). Core Animation can host a layer tree across processes: the layer
// is put into a CAContext, whose 32-bit id the window server knows; the X
// server shows it in the X window's NSView through a CALayerHost with that id
// (MEASURED 2026-09-27: a Metal-cleared layer from one process displayed in
// another's window, no copy -- the same mechanism Chromium uses between its
// GPU process and its browser windows). The id travels to the X server as an
// X property on the window (the Vulkan shim sets it).
//
// Private syscalls (dispatch.c):
//   0x4C580012 (w, h, uint32_t *ctx_id)  -> CAMetalLayer * or 0
//   0x4C580013 (layer, w_px, h_px)        -> 0: resize the drawable
//   0x4C580014 (layer)                    -> 0: release
// No NSApplication is involved: only a window-server connection. Changes are
// committed explicitly (CATransaction flush) because the calling thread has
// no run loop.
#import <Foundation/Foundation.h>
#import <QuartzCore/QuartzCore.h>
#import <Metal/Metal.h>
#include <stdint.h>

void lxrt_window_want_main_queue(void);   // window.m: the Metal HUD's hooks

@interface CAContext : NSObject
+ (instancetype)contextWithCGSConnection:(uint32_t)c options:(NSDictionary *)o;
@property(readonly) uint32_t contextId;
@property(retain) CALayer *layer;
@end
extern uint32_t CGSMainConnectionID(void);

// Keeps each context alive as long as its layer.
static NSMutableDictionary<NSValue *, CAContext *> *g_contexts;

void *lxrt_remote_layer_create(uint32_t w, uint32_t h, uint32_t *ctx_id)
{
    @autoreleasepool {
        if (!g_contexts)
            g_contexts = [NSMutableDictionary new];
        CAContext *ctx = [CAContext contextWithCGSConnection:CGSMainConnectionID()
                                                     options:@{@"kCAContextCIFilterBehavior": @"ignore"}];
        if (!ctx)
            return NULL;
        [CATransaction begin];
        CAMetalLayer *layer = [CAMetalLayer layer];
        layer.device = MTLCreateSystemDefaultDevice();
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.framebufferOnly = YES;
        layer.opaque = YES;
        // The X server's view scales by its screen; the layer's own frame is
        // in points of that view, the drawable in pixels.
        layer.frame = CGRectMake(0, 0, w ? w : 1, h ? h : 1);
        layer.drawableSize = CGSizeMake(w ? w : 1, h ? h : 1);
        ctx.layer = layer;
        [CATransaction commit];
        [CATransaction flush];
        // The Metal HUD loaded: its hooks come from the main queue
        // (runtime/window.m lxrt_window_want_main_queue).
        if (NSClassFromString(@"HUDMTLLayerTracking"))
            lxrt_window_want_main_queue();
        g_contexts[[NSValue valueWithPointer:(__bridge void *)layer]] = ctx;
        if (ctx_id)
            *ctx_id = ctx.contextId;
        return (__bridge void *)layer;
    }
}

int lxrt_remote_layer_resize(void *p, uint32_t w, uint32_t h)
{
    @autoreleasepool {
        CAMetalLayer *layer = (__bridge CAMetalLayer *)p;
        if (!g_contexts[[NSValue valueWithPointer:p]] || !w || !h)
            return -1;
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer.frame = CGRectMake(0, 0, w, h);
        layer.drawableSize = CGSizeMake(w, h);
        [CATransaction commit];
        [CATransaction flush];
        return 0;
    }
}

void lxrt_remote_layer_release(void *p)
{
    @autoreleasepool {
        [g_contexts removeObjectForKey:[NSValue valueWithPointer:p]];
    }
}
