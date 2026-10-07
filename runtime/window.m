// The window the guest presents into.
//
// A Vulkan swapchain needs a surface, and on Metal a surface is a CAMetalLayer.
// The runtime owns it: the guest asks for one through a private syscall and
// gets back a real CAMetalLayer pointer, which it hands straight to
// vkCreateMetalSurfaceEXT.
//
// That is the whole presentation path. There is no framebuffer transport left:
// in the VM, a displayed frame was composited in software by the guest, copied
// to a host backing store, copied to a display frame, and uploaded into an
// MTLTexture -- three full-frame copies. Here the swapchain images *are* the
// layer's drawables.
//
// AppKit insists on the main thread for window creation and on a running run
// loop for the window to behave, so main.c runs the guest on a second thread
// and leaves the main thread servicing the loop.

#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>

#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>
#include <stdio.h>

void lxrt_signal_rescue_stranded(void);   // signal.c

void lxrt_window_want_ui(void);
static NSWindow *g_window;
static CAMetalLayer *g_layer;

static void create_on_main(int w, int h, const char *title)
{
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];

    NSRect frame = NSMakeRect(0, 0, w, h);
    g_window = [[NSWindow alloc]
        initWithContentRect:frame
                  styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
                             NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable)
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [g_window setTitle:[NSString stringWithUTF8String:title ? title : "lxrt"]];
    [g_window center];

    NSView *view = [g_window contentView];
    [view setWantsLayer:YES];

    g_layer = [CAMetalLayer layer];
    g_layer.device = MTLCreateSystemDefaultDevice();
    g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    g_layer.framebufferOnly = YES;
    g_layer.frame = view.bounds;
    // The drawable follows the window, and the backing scale factor is what
    // turns a 1280x720 request into the pixel count the GPU actually fills.
    CGFloat scale = g_window.screen.backingScaleFactor ?: 1.0;
    g_layer.contentsScale = scale;
    g_layer.drawableSize = CGSizeMake(w * scale, h * scale);
    [view setLayer:g_layer];

    [g_window makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
}

// Returns a CAMetalLayer the guest can pass to vkCreateMetalSurfaceEXT.
// Callable from any thread; AppKit work is forced onto the main one.
void *lxrt_window_create(int w, int h, const char *title)
{
    if (g_layer)
        return (__bridge void *)g_layer;
    if (w <= 0 || h <= 0)
        return NULL;

    if ([NSThread isMainThread]) {
        create_on_main(w, h, title);
    } else {
        // Wake the main thread into AppKit first: until now it has been
        // parked outside it (see lxrt_window_pump), so the main queue does
        // not drain yet.
        lxrt_window_want_ui();
        dispatch_sync(dispatch_get_main_queue(), ^{
            create_on_main(w, h, title);
        });
    }
    return (__bridge void *)g_layer;
}

// The drawable size in pixels, which is what a swapchain has to match. Asking
// the layer rather than assuming the requested size is what makes a Retina
// display work instead of rendering a quarter of the window.
void lxrt_window_drawable_size(uint32_t *w, uint32_t *h)
{
    if (!g_layer) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    CGSize s = g_layer.drawableSize;
    if (w) *w = (uint32_t)s.width;
    if (h) *h = (uint32_t)s.height;
}

// Runs the main thread's run loop until the guest exits. Without this the
// window never draws its chrome and never responds.
// AppKit is started lazily. Until the guest asks for a window the main thread
// waits on a plain condition variable and never touches AppKit, CoreFoundation
// or libdispatch's main queue. Reason (measured): a fork() from the guest
// thread while the main thread sat in nextEventMatchingMask left the child
// spinning forever in xpc_atfork_child -> objc +initialize of a class the
// parent's main thread had been initializing at that instant (the i386 Steam
// client forks helpers constantly and never opens a Darwin window).
static pthread_mutex_t g_ui_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ui_cond = PTHREAD_COND_INITIALIZER;
static bool g_ui_wanted;

// The Metal HUD (MTL_HUD_ENABLED) installs its present hooks from a block it
// puts on the main queue (libMTLHud HUDInitInterposeCA: dispatch_after on the
// main queue), and this thread never ran that queue for a game, which shows
// its picture through a remote layer and no window: the HUD loaded and never
// drew (the user, 2026-10-04). With the HUD loaded (remote_layer.m asks),
// the wait below runs the main queue instead -- CFRunLoop in 20 ms slices,
// no AppKit (MEASURED in a window-less test: about 44 wake-ups a second, the
// HUD tracked every present). Without the HUD nothing changes.
static bool g_mainq_wanted;

// The Metal HUD also reaches for AppKit: +[NSApplication sharedApplication]
// for its keyboard shortcuts (HUDKeyboardHookInit) and +[NSScreen mainScreen]
// for the refresh rate and safe area (-[HUDMTLLayerTracking
// _updateInfrequentFields]). Either one checks the process in with Launch
// Services (HIServices _RegisterApplication), and the game became an
// application of its own, "BackgroundOnly" (lsappinfo). With Game Mode on,
// macOS ranks the game it recognises -- the X server, whose windows show the
// game -- above every other application, and that included the one drawing
// the frames: Counter-Strike 2 at 30-53 frames a second with the HUD against
// 127 without it, same Game Mode (MEASURED, benchmarks/stage61 section 12).
// A game's window is the X server's, so the HUD's shortcuts could never fire
// here anyway (no key reaches this process): the HUD is told there is no
// NSApp, and gets a stand-in screen made from CoreGraphics, which registers
// nothing. Only calls made from the HUD's own code are diverted, and none
// once this process has a window of its own (lxrt_window_create).
// LXRT_HUD_APPKIT=1: the HUD's calls go to AppKit as before.
@interface LXRTHudScreen : NSObject
@end

@implementation LXRTHudScreen
- (NSInteger)maximumFramesPerSecond
{
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    double hz = m ? CGDisplayModeGetRefreshRate(m) : 0;
    if (m)
        CGDisplayModeRelease(m);
    return hz > 0 ? (NSInteger)(hz + 0.5) : 60;
}
- (NSTimeInterval)minimumRefreshInterval { return 1.0 / (double)[self maximumFramesPerSecond]; }
- (NSTimeInterval)maximumRefreshInterval { return 1.0 / (double)[self maximumFramesPerSecond]; }
- (NSTimeInterval)displayUpdateGranularity { return 0; }
- (NSEdgeInsets)safeAreaInsets { return NSEdgeInsetsMake(0, 0, 0, 0); }
- (NSRect)frame { return NSRectFromCGRect(CGDisplayBounds(CGMainDisplayID())); }
- (NSRect)visibleFrame { return [self frame]; }
- (CGFloat)backingScaleFactor { return 1; }
- (CGFloat)maximumExtendedDynamicRangeColorComponentValue { return 1; }
- (CGFloat)maximumPotentialExtendedDynamicRangeColorComponentValue { return 1; }
- (NSString *)localizedName { return @"Display"; }
- (NSDictionary *)deviceDescription { return @{ @"NSScreenNumber": @(CGMainDisplayID()) }; }
// Anything else NSScreen answers comes back zero rather than raising.
- (NSMethodSignature *)methodSignatureForSelector:(SEL)sel
{
    NSMethodSignature *m = [super methodSignatureForSelector:sel];
    return m ? m : [NSScreen instanceMethodSignatureForSelector:sel];
}
- (void)forwardInvocation:(NSInvocation *)inv
{
    NSUInteger n = inv.methodSignature.methodReturnLength;
    if (n) {
        void *zero = calloc(1, n);
        [inv setReturnValue:zero];
        free(zero);
    }
}
@end

static IMP g_shared_app_imp, g_main_screen_imp;

static bool called_from_hud(void *ret)
{
    if (g_ui_wanted)
        return false;
    Dl_info di;
    return dladdr(ret, &di) && di.dli_fname && strstr(di.dli_fname, "/libMTLHud.");
}

static id hud_shared_app(id self, SEL cmd)
{
    if (called_from_hud(__builtin_return_address(0)))
        return nil;
    return ((id (*)(id, SEL))g_shared_app_imp)(self, cmd);
}

static id hud_main_screen(id self, SEL cmd)
{
    if (called_from_hud(__builtin_return_address(0))) {
        static LXRTHudScreen *screen;
        static dispatch_once_t once;
        dispatch_once(&once, ^{ screen = [LXRTHudScreen new]; });
        return screen;
    }
    return ((id (*)(id, SEL))g_main_screen_imp)(self, cmd);
}

static void hud_keep_out_of_appkit(void)
{
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("LXRT_HUD_APPKIT");
        if (e && e[0] == '1')
            return;
        Method m = class_getClassMethod([NSApplication class], @selector(sharedApplication));
        if (m)
            g_shared_app_imp = method_setImplementation(m, (IMP)hud_shared_app);
        m = class_getClassMethod([NSScreen class], @selector(mainScreen));
        if (m)
            g_main_screen_imp = method_setImplementation(m, (IMP)hud_main_screen);
    });
}

void lxrt_window_want_main_queue(void)
{
    hud_keep_out_of_appkit();
    pthread_mutex_lock(&g_ui_lock);
    g_mainq_wanted = true;
    pthread_cond_broadcast(&g_ui_cond);
    pthread_mutex_unlock(&g_ui_lock);
}

void lxrt_window_want_ui(void)
{
    pthread_mutex_lock(&g_ui_lock);
    g_ui_wanted = true;
    pthread_cond_broadcast(&g_ui_cond);
    pthread_mutex_unlock(&g_ui_lock);
}

void lxrt_window_pump(volatile bool *guest_running)
{
    pthread_mutex_lock(&g_ui_lock);
    while (*guest_running && !g_ui_wanted) {
        if (g_mainq_wanted) {
            pthread_mutex_unlock(&g_ui_lock);
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
            lxrt_signal_rescue_stranded();
            pthread_mutex_lock(&g_ui_lock);
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 20 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&g_ui_cond, &g_ui_lock, &ts);
        // A process-directed signal no guest thread took when it was posted
        // is pending on this thread: hand it on (signal.c).
        pthread_mutex_unlock(&g_ui_lock);
        lxrt_signal_rescue_stranded();
        pthread_mutex_lock(&g_ui_lock);
    }
    pthread_mutex_unlock(&g_ui_lock);
    if (!*guest_running)
        return;
    @autoreleasepool {
        while (*guest_running) {
            @autoreleasepool {
                NSEvent *e;
                while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                               untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
                                                  inMode:NSDefaultRunLoopMode
                                                 dequeue:YES]))
                    [NSApp sendEvent:e];
                // dispatch_sync from the guest thread lands on the main queue,
                // which only drains while the main thread is in a run loop.
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.005, true);
                lxrt_signal_rescue_stranded();
            }
        }
    }
}
