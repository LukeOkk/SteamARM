// SteamARM native Wayland compositor. No libwayland, X server, or VM.
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#import <CoreGraphics/CoreGraphics.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>

static NSString *dumpDir;
static BOOL verbose;
static BOOL selftestInput;
static BOOL exitWhenEmpty;       // leave 5 s after the last window closed (an app session ends with its windows)
static BOOL hadWindow;
static void shutdownServer(int signalNumber);
static void windowsChanged(void);
static uint32_t serialNumber = 1;
static uint32_t serialNext(void) { return ++serialNumber; }
static uint32_t word(const uint8_t *p) { uint32_t v; memcpy(&v,p,4); return v; }
static int32_t sint(const uint8_t *p) { return (int32_t)word(p); }
static NSData *u32(uint32_t n) { return [NSData dataWithBytes:&n length:4]; }
static void append32(NSMutableData *d, uint32_t n) { [d appendBytes:&n length:4]; }
static void appendString(NSMutableData *d, NSString *s) {
    NSData *b = [s dataUsingEncoding:NSUTF8StringEncoding] ?: [NSData data];
    uint32_t n = (uint32_t)b.length + 1; append32(d,n); [d appendData:b];
    uint8_t zero[4] = {0}; [d appendBytes:zero length:1+((4-(n&3))&3)];
}
static NSString *readString(const uint8_t *p, size_t len, size_t *at) {
    if (*at+4>len) return nil;
    uint32_t n=word(p+*at); *at+=4;
    if (!n || n>len-*at || p[*at+n-1]) return nil;
    NSString *s=[[NSString alloc] initWithBytes:p+*at length:n-1 encoding:NSUTF8StringEncoding];
    *at+=(n+3)&~3u; return s;
}
static uint32_t at32(const uint8_t *p,size_t len,size_t at) { return at+4<=len?word(p+at):0; }
static NSString *safeFile(NSString *s) {
    NSMutableString *r=[NSMutableString string];
    for (NSUInteger i=0;i<s.length && i<90;i++) { unichar c=[s characterAtIndex:i]; [r appendFormat:@"%C",(unichar)((c=='/' || c=='\\' || c<32)?'_':c)]; }
    return r.length?r:@"window";
}
static void logLine(NSString *s) { fprintf(stderr,"%s\n",s.UTF8String); fflush(stderr); }

@class Client, Obj, WView;
@interface WWindow : NSWindow <NSWindowDelegate>
@property(nonatomic,weak) Obj *top;
@end
@interface WView : NSView
@property(nonatomic,weak) Obj *top;
@end
@interface Obj : NSObject
@property(nonatomic) uint32_t oid, version, bufferID, pendingBuffer, pendingFrame, xdgID, topID, parentID;
@property(nonatomic) uint32_t format, offset, stride, width, height, poolSize;
@property(nonatomic) int fd, scale, transform, inputX, inputY, inputW, inputH;
@property(nonatomic) int subX,subY,pendingX,pendingY,viewportW,viewportH;
@property(nonatomic) int sourceX,sourceY,sourceW,sourceH;
@property(nonatomic) int geoX,geoY,geoW,geoH,pendingGeoX,pendingGeoY,pendingGeoW,pendingGeoH;
@property(nonatomic) BOOL pendingGeo;
@property(nonatomic) double offX,offY;      // the window's origin in root-surface coordinates
@property(nonatomic) BOOL hasPendingAttach,hasInput, synchronous, pendingSubPos, configured, maximized, fullscreen;
@property(nonatomic,strong) NSString *kind,*title,*appID;
@property(nonatomic,strong) CALayer *layer;
@property(nonatomic,strong) NSImage *image;
@property(nonatomic,strong) WWindow *window;
@property(nonatomic,strong) NSMutableArray<NSNumber *> *frames;
@property(nonatomic,strong) NSMutableArray<NSNumber *> *pendingFrames;
@property(nonatomic,weak) Client *client;
@property(nonatomic,strong) NSMutableArray<Obj *> *children;
@property(nonatomic,strong) Obj *poolObject;
@end
@implementation Obj
- (instancetype)init { if ((self=[super init])) { _fd=-1; _scale=1; _frames=[NSMutableArray array]; _pendingFrames=[NSMutableArray array]; _children=[NSMutableArray array]; _viewportW=-1; _viewportH=-1; _sourceW=-1; _sourceH=-1; _synchronous=YES; } return self; }
- (void)dealloc { if (_fd>=0) close(_fd); }
@end

static NSArray<NSDictionary *> *globals(void) { return @[
    @{@"name":@1,@"iface":@"wl_compositor",@"version":@4},
    @{@"name":@2,@"iface":@"wl_subcompositor",@"version":@1},
    @{@"name":@3,@"iface":@"wl_shm",@"version":@1},
    @{@"name":@4,@"iface":@"wl_output",@"version":@3},
    @{@"name":@5,@"iface":@"wl_seat",@"version":@5},
    @{@"name":@6,@"iface":@"xdg_wm_base",@"version":@2},
    @{@"name":@7,@"iface":@"wp_viewporter",@"version":@1} ]; }

@interface Client : NSObject
@property(nonatomic) int fd;
@property(nonatomic) dispatch_source_t source;
@property(nonatomic,strong) NSMutableData *incoming;
@property(nonatomic,strong) NSMutableArray<NSNumber *> *fds;
@property(nonatomic,strong) NSMutableDictionary<NSNumber *,Obj *> *objects;
@property(nonatomic) uint32_t pointerID,keyboardID,pointerFocus,keyboardFocus,buttonMask;
@property(nonatomic) BOOL dead;
- (instancetype)initWithFD:(int)fd;
- (void)event:(uint32_t)id opcode:(uint16_t)op body:(NSData *)body fd:(int)passed;
- (void)error:(uint32_t)id text:(NSString *)text;
- (Obj *)object:(uint32_t)id;
- (Obj *)create:(uint32_t)id kind:(NSString *)kind version:(uint32_t)v;
- (void)remove:(uint32_t)id;
- (void)request:(uint32_t)id op:(uint16_t)op bytes:(const uint8_t *)p length:(size_t)n;
- (void)closeClient;
@end

static NSMutableArray<Client *> *clients;
static NSString *socketPath;
static int listenFD=-1;
static dispatch_source_t listenSource;
static NSTimer *frameTimer;
static const char *keymapPath;
static uint64_t copyBytes=0, copyNanos=0, copyFrames=0;

static uint32_t keyCode(uint16_t k) {
    static const uint16_t map[128]={
      30,31,32,33,35,34,44,45,46,47,86,48,16,17,18,19,21,20,2,3,4,5,7,6,13,10,8,12,9,11,27,24,
      22,26,23,25,28,38,36,40,37,39,43,51,53,49,50,52,15,57,41,14,0,1,126,125,42,58,56,29,54,100,97,0,
      187,83,0,55,0,78,0,69,115,114,113,98,96,0,74,188,189,117,82,79,80,81,75,76,77,71,190,72,73,124,89,95,
      63,64,65,61,66,67,123,87,122,183,186,184,0,68,127,88,0,185,110,102,104,111,62,107,60,109,59,105,106,108,103};
    return k<128?map[k]:0;
}
static uint32_t modifiers(NSEventModifierFlags f) {
    return ((f&NSEventModifierFlagShift)?1:0)|((f&NSEventModifierFlagCapsLock)?2:0)|
           ((f&NSEventModifierFlagControl)?4:0)|((f&NSEventModifierFlagOption)?8:0)|
           ((f&NSEventModifierFlagCommand)?64:0);
}
static Obj *rootSurface(Obj *s) { while (s && s.parentID) s=[s.client object:s.parentID]; return s; }
static NSSize surfaceSize(Obj *s) {
    if (s.viewportW>0 && s.viewportH>0) return NSMakeSize(s.viewportW,s.viewportH);
    return NSMakeSize(s.width/MAX(1,s.scale),s.height/MAX(1,s.scale));
}
static void layoutSurface(Obj *s) {
    if (!s.layer) return;
    NSSize z=surfaceSize(s); s.layer.frame=CGRectMake(s.subX,s.subY,z.width,z.height);
    for (Obj *c in s.children) layoutSurface(c);
}
static void configureTop(Obj *top, int w, int h) {
    Client *c=top.client; if (!c || !top.xdgID || !top.topID) return;
    NSMutableData *b=[NSMutableData data]; append32(b,MAX(0,w)); append32(b,MAX(0,h));
    NSMutableData *states=[NSMutableData data]; if(top.maximized) append32(states,1); if(top.fullscreen) append32(states,2);
    append32(b,(uint32_t)states.length); [b appendData:states];
    [c event:top.topID opcode:0 body:b fd:-1];
    [c event:top.xdgID opcode:0 body:u32(serialNext()) fd:-1];
    top.configured=YES;
}

@implementation WWindow
- (BOOL)windowShouldClose:(id)sender {
    Obj *s=self.top; if(s) [s.client event:s.topID opcode:1 body:[NSData data] fd:-1];
    return NO;
}
- (void)windowDidResize:(NSNotification *)n {
    Obj *s=self.top; if(s && s.configured) { NSSize z=self.contentView.bounds.size; configureTop(s,(int)z.width,(int)z.height); }
}
- (void)windowDidBecomeKey:(NSNotification *)n {
    Obj *s=self.top; Client *c=s.client; if (!c || !c.keyboardID) return;
    c.keyboardFocus=s.oid;
    NSMutableData *b=[NSMutableData data]; append32(b,serialNext()); append32(b,s.oid); append32(b,0);
    [c event:c.keyboardID opcode:1 body:b fd:-1];
    b=[NSMutableData data]; append32(b,serialNext()); append32(b,modifiers(NSEvent.modifierFlags)); append32(b,0); append32(b,0); append32(b,0);
    [c event:c.keyboardID opcode:4 body:b fd:-1];
}
- (void)windowDidResignKey:(NSNotification *)n {
    Obj *s=self.top; Client *c=s.client; if(c.keyboardID && c.keyboardFocus) {
        NSMutableData *b=[NSMutableData data]; append32(b,serialNext()); append32(b,c.keyboardFocus);
        [c event:c.keyboardID opcode:2 body:b fd:-1]; c.keyboardFocus=0;
    }
}
@end

static Obj *hitSurface(Obj *s, NSPoint p) {
    for (Obj *child in s.children.reverseObjectEnumerator) {
        NSPoint q=NSMakePoint(p.x-child.subX,p.y-child.subY); NSSize z=surfaceSize(child);
        if(q.x>=0 && q.y>=0 && q.x<z.width && q.y<z.height) {
            Obj *hit=hitSurface(child,q); if(hit) return hit;
        }
    }
    NSSize z=surfaceSize(s);
    BOOL inside=p.x>=0 && p.y>=0 && p.x<z.width && p.y<z.height;
    if(s.hasInput) inside=p.x>=s.inputX && p.y>=s.inputY && p.x<s.inputX+s.inputW && p.y<s.inputY+s.inputH;
    return inside?s:nil;
}
static uint32_t eventTime(void) { return (uint32_t)([NSProcessInfo processInfo].systemUptime*1000.0); }
static void pointerFrame(Client *c) { Obj *p=[c object:c.pointerID]; if(p && p.version>=5) [c event:c.pointerID opcode:5 body:[NSData data] fd:-1]; }
static void pointerEvent(WView *v,NSEvent *e, int action) {
    Obj *top=v.top; Client *c=top.client; if(!c.pointerID) return;
    NSPoint p=[v convertPoint:e.locationInWindow fromView:nil]; p.y=v.bounds.size.height-p.y;
    p.x+=top.offX; p.y+=top.offY;       // window -> root surface (geometry or the app's layers)
    Obj *hit=hitSurface(top,p); if(!hit && action!=1) hit=top;
    if(c.pointerFocus && (!hit || hit.oid!=c.pointerFocus)) {
        NSMutableData *b=[NSMutableData data]; append32(b,serialNext()); append32(b,c.pointerFocus);
        [c event:c.pointerID opcode:1 body:b fd:-1]; c.pointerFocus=0;
    }
    if(!hit) { pointerFrame(c); return; }
    for(Obj *s=hit;s && s!=top;s=[c object:s.parentID]) { p.x-=s.subX; p.y-=s.subY; }
    if(c.pointerFocus!=hit.oid) {
        NSMutableData *b=[NSMutableData data]; append32(b,serialNext()); append32(b,hit.oid);
        append32(b,(int32_t)llround(p.x*256)); append32(b,(int32_t)llround(p.y*256));
        [c event:c.pointerID opcode:0 body:b fd:-1]; c.pointerFocus=hit.oid;
    }
    if(action==0) { NSMutableData *b=[NSMutableData data]; append32(b,eventTime()); append32(b,(int32_t)llround(p.x*256)); append32(b,(int32_t)llround(p.y*256)); [c event:c.pointerID opcode:2 body:b fd:-1]; }
    else if(action==2 || action==3) {
        uint32_t button=e.buttonNumber==0?0x110:(e.buttonNumber==1?0x111:0x112);
        NSMutableData *b=[NSMutableData data]; append32(b,serialNext()); append32(b,eventTime()); append32(b,button); append32(b,action==2?1:0);
        [c event:c.pointerID opcode:3 body:b fd:-1];
    } else if(action==4) {
        for(int axis=0;axis<2;axis++) { double d=axis==0?-e.scrollingDeltaY:-e.scrollingDeltaX; if(!d) continue;
            NSMutableData *b=[NSMutableData data]; append32(b,eventTime()); append32(b,axis); append32(b,(int32_t)llround(d*256)); [c event:c.pointerID opcode:4 body:b fd:-1]; }
    }
    pointerFrame(c);
}
static void keyboardEvent(WView *v,NSEvent *e, BOOL down) {
    Client *c=v.top.client; if(!c.keyboardID) return;
    uint32_t k=keyCode(e.keyCode); if(!k) return;
    NSMutableData *b=[NSMutableData data]; append32(b,serialNext()); append32(b,eventTime()); append32(b,k); append32(b,down?1:0);
    [c event:c.keyboardID opcode:3 body:b fd:-1];
    b=[NSMutableData data]; append32(b,serialNext()); append32(b,modifiers(e.modifierFlags)); append32(b,0); append32(b,0); append32(b,0);
    [c event:c.keyboardID opcode:4 body:b fd:-1];
}
@implementation WView
- (BOOL)isFlipped { return YES; }
- (BOOL)acceptsFirstResponder { return YES; }
- (void)mouseMoved:(NSEvent *)e { pointerEvent(self,e,0); }
- (void)mouseDragged:(NSEvent *)e { pointerEvent(self,e,0); }
- (void)rightMouseDragged:(NSEvent *)e { pointerEvent(self,e,0); }
- (void)otherMouseDragged:(NSEvent *)e { pointerEvent(self,e,0); }
- (void)mouseEntered:(NSEvent *)e { pointerEvent(self,e,0); }
- (void)mouseExited:(NSEvent *)e { pointerEvent(self,e,1); }
- (void)mouseDown:(NSEvent *)e { pointerEvent(self,e,2); }
- (void)mouseUp:(NSEvent *)e { pointerEvent(self,e,3); }
- (void)rightMouseDown:(NSEvent *)e { pointerEvent(self,e,2); }
- (void)rightMouseUp:(NSEvent *)e { pointerEvent(self,e,3); }
- (void)otherMouseDown:(NSEvent *)e { pointerEvent(self,e,2); }
- (void)otherMouseUp:(NSEvent *)e { pointerEvent(self,e,3); }
- (void)scrollWheel:(NSEvent *)e { pointerEvent(self,e,4); }
- (void)keyDown:(NSEvent *)e { keyboardEvent(self,e,YES); }
- (void)keyUp:(NSEvent *)e { keyboardEvent(self,e,NO); }
- (void)flagsChanged:(NSEvent *)e { keyboardEvent(self,e,(e.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask)!=0); }
@end

static __weak WWindow *newestWindow;      // the target of test commands (runCommands)
static void makeWindow(Obj *s) {
    if(s.window) return;
    NSRect r=NSMakeRect(120+clients.count*20,120+clients.count*20,MAX(1,s.width?:480),MAX(1,s.height?:320));
    WWindow *w=[[WWindow alloc] initWithContentRect:r styleMask:NSWindowStyleMaskTitled|NSWindowStyleMaskClosable|NSWindowStyleMaskResizable|NSWindowStyleMaskMiniaturizable backing:NSBackingStoreBuffered defer:NO];
    w.releasedWhenClosed=NO; w.top=s; w.delegate=w; w.title=s.title?:@"SteamARM Android";
    WView *v=[[WView alloc] initWithFrame:NSMakeRect(0,0,r.size.width,r.size.height)]; v.top=s; v.wantsLayer=YES;
    v.layer.geometryFlipped=YES; w.contentView=v; [w setAcceptsMouseMovedEvents:YES];
    NSTrackingArea *track=[[NSTrackingArea alloc] initWithRect:NSZeroRect options:NSTrackingMouseEnteredAndExited|NSTrackingMouseMoved|NSTrackingActiveAlways|NSTrackingInVisibleRect owner:v userInfo:nil];
    [v addTrackingArea:track];
    if(!s.layer) s.layer=[CALayer layer]; s.layer.geometryFlipped=YES; s.layer.frame=v.bounds; [v.layer addSublayer:s.layer];
    s.window=w; newestWindow=w; hadWindow=YES; [w makeKeyAndOrderFront:nil]; [NSApp activateIgnoringOtherApps:YES];
    for(Obj *output in s.client.objects.allValues) if([output.kind isEqual:@"wl_output"]) [s.client event:s.oid opcode:0 body:u32(output.oid) fd:-1];
    logLine([NSString stringWithFormat:@"window title=%@ size=%dx%d app_id=%@",w.title,(int)r.size.width,(int)r.size.height,s.appID?:@""]);
    if(selftestInput) dispatch_async(dispatch_get_main_queue(), ^{
        if(!s.client.pointerID || !s.client.keyboardID) return;
        [w windowDidBecomeKey:[NSNotification notificationWithName:NSWindowDidBecomeKeyNotification object:w]];
        NSPoint loc=NSMakePoint(MIN(20,r.size.width-1),MAX(1,r.size.height-20));
        NSEvent *move=[NSEvent mouseEventWithType:NSEventTypeMouseMoved location:loc modifierFlags:0 timestamp:0 windowNumber:w.windowNumber context:nil eventNumber:1 clickCount:0 pressure:0];
        NSEvent *down=[NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:loc modifierFlags:0 timestamp:0 windowNumber:w.windowNumber context:nil eventNumber:2 clickCount:1 pressure:1];
        NSEvent *up=[NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:loc modifierFlags:0 timestamp:0 windowNumber:w.windowNumber context:nil eventNumber:3 clickCount:1 pressure:0];
        NSEvent *key=[NSEvent keyEventWithType:NSEventTypeKeyDown location:loc modifierFlags:0 timestamp:0 windowNumber:w.windowNumber context:nil characters:@"a" charactersIgnoringModifiers:@"a" isARepeat:NO keyCode:0];
        pointerEvent(v,move,0); pointerEvent(v,down,2); pointerEvent(v,up,3); keyboardEvent(v,key,YES);
        logLine(@"selftest input: pointer enter/motion/button and keyboard key sent");
    });
}
static void dumpWindow(Obj *s) {
    // The window as composed (the root surface and its subsurfaces, each a
    // CALayer), rendered offscreen: no screen capture. At most once a second.
    if(!dumpDir.length || !s.window) return;
    static NSMutableDictionary<NSString *,NSNumber *> *last; if(!last) last=[NSMutableDictionary dictionary];
    NSString *key=[NSString stringWithFormat:@"%p",s]; double now=[NSProcessInfo processInfo].systemUptime;
    if(last[key] && now-last[key].doubleValue<1.0) {
        // Skipped: dump once more a second later, so the last frame of a
        // burst is on disk even if nothing is committed after it.
        static NSMutableSet<NSString *> *pending; if(!pending) pending=[NSMutableSet set];
        if(![pending containsObject:key]) {
            [pending addObject:key];
            __weak Obj *weak=s;
            dispatch_after(dispatch_time(DISPATCH_TIME_NOW,(int64_t)(1.1*NSEC_PER_SEC)),dispatch_get_main_queue(),^{
                [pending removeObject:key]; Obj *o=weak; if(o) dumpWindow(o); });
        }
        return;
    }
    last[key]=@(now);
    CALayer *root=s.window.contentView.layer; NSSize z=s.window.contentView.bounds.size;
    if(!root || z.width<1 || z.height<1) return;
    CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx=CGBitmapContextCreate(NULL,(size_t)z.width,(size_t)z.height,8,0,space,(CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGColorSpaceRelease(space); if(!ctx) return;
    CGContextTranslateCTM(ctx,0,z.height); CGContextScaleCTM(ctx,1,-1);
    [root renderInContext:ctx];
    CGImageRef img=CGBitmapContextCreateImage(ctx); CGContextRelease(ctx); if(!img) return;
    NSBitmapImageRep *rep=[[NSBitmapImageRep alloc] initWithCGImage:img]; CGImageRelease(img);
    NSData *png=[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}];
    NSString *path=[dumpDir stringByAppendingPathComponent:[safeFile(s.title?:@"window") stringByAppendingString:@".png"]];
    [png writeToFile:path atomically:YES];
}
static void applySurface(Obj *s) {
    Client *c=s.client;
    if(s.pendingSubPos) { s.subX=s.pendingX; s.subY=s.pendingY; s.pendingSubPos=NO; }
    if(s.hasPendingAttach) {
        s.hasPendingAttach=NO; s.bufferID=s.pendingBuffer;
        Obj *b=[c object:s.bufferID]; Obj *pool=b.poolObject;
        if(!s.bufferID) {
            // attach(NULL): the surface is unmapped. Waydroid's composer hides
            // a layer so -- the app's white starting window stayed over the
            // app until this cleared it (MEASURED).
            s.layer.contents=nil; s.image=nil; s.width=0; s.height=0;
            goto attached;
        }
        if(!b || !pool || pool.fd<0 || b.width==0 || b.height==0 || b.stride<b.width*4 ||
           (uint64_t)b.offset+(uint64_t)b.stride*b.height>pool.poolSize) {
            [c error:s.oid text:@"invalid shm buffer"]; return;
        }
        s.width=b.width; s.height=b.height;
        uint64_t start=clock_gettime_nsec_np(CLOCK_MONOTONIC);
        size_t n=(size_t)b.stride*b.height;
        void *mapping=mmap(NULL,pool.poolSize,PROT_READ,MAP_SHARED,pool.fd,0);
        if(mapping==MAP_FAILED) { [c error:s.oid text:@"mmap failed"]; return; }
        NSData *pixels=[[NSData alloc] initWithBytes:(uint8_t *)mapping+b.offset length:n];
        munmap(mapping,pool.poolSize);
        if(verbose) {
            uint64_t sum=0; const uint8_t *px=pixels.bytes; for(size_t i=0;i<n;i+=4093) sum=sum*31+px[i];
            logLine([NSString stringWithFormat:@"commit surface %u buffer %ux%u fmt %x at %d,%d viewport %dx%d src %d,%d %dx%d sum %llx",
                     s.oid,b.width,b.height,b.format,s.subX,s.subY,s.viewportW,s.viewportH,s.sourceX/256,s.sourceY/256,s.sourceW/256,s.sourceH/256,sum]);
        }
        CGColorSpaceRef space=CGColorSpaceCreateDeviceRGB();
        CGBitmapInfo info;
        switch(b.format) {
            case 0: info=kCGImageAlphaFirst|kCGBitmapByteOrder32Little; break;
            case 1: info=kCGImageAlphaNoneSkipFirst|kCGBitmapByteOrder32Little; break;
            case 0x34324241: info=kCGImageAlphaLast|kCGBitmapByteOrder32Big; break;
            default: info=kCGImageAlphaNoneSkipLast|kCGBitmapByteOrder32Big; break;
        }
        CGDataProviderRef provider=CGDataProviderCreateWithCFData((__bridge CFDataRef)pixels);
        CGImageRef cg=CGImageCreate(b.width,b.height,8,32,b.stride,space,info,provider,NULL,NO,kCGRenderingIntentDefault);
        CGDataProviderRelease(provider); CGColorSpaceRelease(space);
        if(!cg) { [c error:s.oid text:@"invalid bitmap format"]; return; }
        if(verbose && dumpDir.length) {     // each surface's own last buffer, at most every 2 s
            static NSMutableDictionary<NSNumber *,NSNumber *> *lastSurf; if(!lastSurf) lastSurf=[NSMutableDictionary dictionary];
            double now=[NSProcessInfo processInfo].systemUptime;
            if(!lastSurf[@(s.oid)] || now-lastSurf[@(s.oid)].doubleValue>2.0) {
                lastSurf[@(s.oid)]=@(now);
                NSBitmapImageRep *rep=[[NSBitmapImageRep alloc] initWithCGImage:cg];
                [[rep representationUsingType:NSBitmapImageFileTypePNG properties:@{}]
                    writeToFile:[dumpDir stringByAppendingPathComponent:[NSString stringWithFormat:@"surface-%u-fmt%x.png",s.oid,b.format]] atomically:YES];
            }
        }
        s.image=[[NSImage alloc] initWithCGImage:cg size:NSMakeSize(b.width,b.height)];
        if(!s.layer) { s.layer=[CALayer layer]; s.layer.geometryFlipped=YES; Obj *parent=[c object:s.parentID]; [parent.layer addSublayer:s.layer]; }
        s.layer.contents=(__bridge id)cg; s.layer.contentsGravity=kCAGravityResize;
        if(s.sourceW>0 && s.sourceH>0) s.layer.contentsRect=CGRectMake((double)s.sourceX/256/b.width,(double)s.sourceY/256/b.height,(double)s.sourceW/256/b.width,(double)s.sourceH/256/b.height);
        CGImageRelease(cg);
        [c event:b.oid opcode:0 body:[NSData data] fd:-1];
        copyNanos+=clock_gettime_nsec_np(CLOCK_MONOTONIC)-start; copyBytes+=n; copyFrames++;
        if(verbose && copyFrames%120==0) logLine([NSString stringWithFormat:@"copy frames=%llu bytes=%llu us_per_frame=%llu",copyFrames,copyBytes,copyNanos/copyFrames/1000]);
    }
attached:
    if(s.pendingFrames.count) { [s.frames addObjectsFromArray:s.pendingFrames]; [s.pendingFrames removeAllObjects]; }
    for(Obj *child in s.children) if(child.synchronous) applySurface(child);
    // Size and place on every commit, not only on a new buffer: Waydroid's
    // composer attaches a 1x1 buffer to its window once and then sizes the
    // window with wp_viewport (MEASURED: the window stayed 1x1).
    if(s.pendingGeo) { s.geoX=s.pendingGeoX; s.geoY=s.pendingGeoY; s.geoW=s.pendingGeoW; s.geoH=s.pendingGeoH; s.pendingGeo=NO; }
    layoutSurface(s);
    // A toplevel's Mac window appears once it has a real size: Waydroid's
    // composer keeps a 1x1 "Waydroid" toplevel it never shows content in
    // (MEASURED), which would be an empty window on the Mac.
    // So does a 1x1 backdrop with no mapped subsurface: Waydroid's
    // "Waydroid" toplevel in multi-window mode is only that, and on the Mac
    // it would be an invisible window over the whole screen taking clicks.
    BOOL bare=NO;
    if(s==rootSurface(s) && s.width<=1 && s.height<=1) {
        bare=YES; for(Obj *ch in s.children) if(ch.layer.contents) { bare=NO; break; }
    }
    if(s==rootSurface(s) && s.topID && !s.window && !bare) {
        NSSize z=(s.geoW>1 && s.geoH>1)?NSMakeSize(s.geoW,s.geoH):surfaceSize(s);
        if(z.width>1 && z.height>1 && s.layer) makeWindow(s);
    }
    if(s==rootSurface(s) && s.window && bare && s.window.isVisible) { [s.window orderOut:nil]; windowsChanged(); }
    if(s==rootSurface(s) && s.window && !bare && !s.window.isVisible) [s.window orderFront:nil];
    if(s==rootSurface(s) && s.window) {
        // The window geometry, when the client gives one, is the window:
        // Waydroid's multi-window composer draws each app's task at its
        // place on the whole screen and names that part with it.
        NSSize z=surfaceSize(s);
        s.offX=0; s.offY=0;
        // A 1x1 backdrop stretched over the screen with the app's layers as
        // subsurfaces (Waydroid's multi-window mode, whose window geometry
        // is the whole screen too, MEASURED): the window is the union of the
        // mapped subsurfaces. Otherwise the client's window geometry.
        CGRect u=CGRectNull;
        if(s.width<=1 && s.height<=1)
            for(Obj *ch in s.children) if(ch.layer.contents) {
                NSSize cz=surfaceSize(ch); u=CGRectUnion(u,CGRectMake(ch.subX,ch.subY,cz.width,cz.height));
            }
        if(!CGRectIsNull(u) && u.size.width>1 && u.size.height>1) {
            z=u.size; s.offX=u.origin.x; s.offY=u.origin.y;
        } else if(s.geoW>0 && s.geoH>0) {
            z=NSMakeSize(s.geoW,s.geoH); s.offX=s.geoX; s.offY=s.geoY;
        }
        if(s.offX || s.offY) { CGRect f=s.layer.frame; f.origin=CGPointMake(-s.offX,-s.offY); s.layer.frame=f; }
        if(verbose) {
            NSMutableString *kids=[NSMutableString string];
            for(Obj *ch in s.children) [kids appendFormat:@" %u:%dx%d@%d,%d%s(%lu)",ch.oid,(int)surfaceSize(ch).width,(int)surfaceSize(ch).height,
                                        ch.subX,ch.subY,ch.layer.contents?"":"-",(unsigned long)ch.children.count];
            logLine([NSString stringWithFormat:@"root %u buf %ux%u vp %dx%d window %dx%d off %.0f,%.0f kids%@",
                     s.oid,s.width,s.height,s.viewportW,s.viewportH,(int)z.width,(int)z.height,s.offX,s.offY,kids]);
        }
        if(z.width>1 && z.height>1 && !NSEqualSizes(s.window.contentView.bounds.size,z)) [s.window setContentSize:z];
        dumpWindow(s);
    } else {
        Obj *r=rootSurface(s); if(r.window) dumpWindow(r);     // a subsurface's new content
    }
}

@implementation Client
- (instancetype)initWithFD:(int)fd {
    if((self=[super init])) { _fd=fd; _incoming=[NSMutableData data]; _fds=[NSMutableArray array]; _objects=[NSMutableDictionary dictionary];
        Obj *d=[self create:1 kind:@"wl_display" version:1]; (void)d;
        __weak Client *weak=self;
        _source=dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,fd,0,dispatch_get_main_queue());
        dispatch_source_set_event_handler(_source,^{ [weak readAvailable]; });
        dispatch_resume(_source);
    } return self;
}
- (Obj *)object:(uint32_t)id { return _objects[@(id)]; }
- (Obj *)create:(uint32_t)id kind:(NSString *)kind version:(uint32_t)v {
    if(!id || [self object:id]) { [self error:id text:@"duplicate object id"]; return nil; }
    Obj *o=[Obj new]; o.oid=id; o.kind=kind; o.version=v; o.client=self; _objects[@(id)]=o; return o;
}
- (void)remove:(uint32_t)id { [_objects removeObjectForKey:@(id)]; [self event:1 opcode:1 body:u32(id) fd:-1]; }
- (void)event:(uint32_t)id opcode:(uint16_t)op body:(NSData *)body fd:(int)passed {
    if(_dead) return;
    uint32_t h[2]={id,(uint32_t)(((body.length+8)<<16)|op)};
    struct iovec vec[2]={{h,8},{(void *)body.bytes,body.length}};
    struct msghdr msg={0}; msg.msg_iov=vec; msg.msg_iovlen=body.length?2:1;
    union { struct cmsghdr align; uint8_t bytes[CMSG_SPACE(sizeof(int))]; } ancillary={0};
    if(passed>=0) { msg.msg_control=ancillary.bytes; msg.msg_controllen=sizeof(ancillary.bytes);
        struct cmsghdr *ch=CMSG_FIRSTHDR(&msg); ch->cmsg_level=SOL_SOCKET; ch->cmsg_type=SCM_RIGHTS; ch->cmsg_len=CMSG_LEN(sizeof(int)); memcpy(CMSG_DATA(ch),&passed,sizeof(int)); }
    ssize_t n=sendmsg(_fd,&msg,0);
    if(n<0 || (size_t)n!=body.length+8) [self closeClient];
}
- (void)error:(uint32_t)id text:(NSString *)message {
    NSMutableData *b=[NSMutableData data]; append32(b,id); append32(b,1); appendString(b,message);
    [self event:1 opcode:0 body:b fd:-1]; logLine([NSString stringWithFormat:@"protocol error object=%u %@",id,message]); [self closeClient];
}
- (void)closeClient {
    if(_dead) return; _dead=YES;
    for(NSNumber *f in _fds) close(f.intValue); [_fds removeAllObjects];
    for(Obj *o in _objects.allValues) if(o.window) { [o.window close]; o.window=nil; }
    dispatch_async(dispatch_get_main_queue(),^{ windowsChanged(); });
    [_objects removeAllObjects];
    if(_source) { dispatch_source_cancel(_source); _source=nil; }
    close(_fd); [clients removeObject:self];
}
- (void)readAvailable {
    if(_dead) return;
    for(;;) {
        uint8_t bytes[65536], control[CMSG_SPACE(64*sizeof(int))]; struct iovec vec={bytes,sizeof(bytes)};
        struct msghdr msg={0}; msg.msg_iov=&vec; msg.msg_iovlen=1; msg.msg_control=control; msg.msg_controllen=sizeof(control);
        ssize_t n=recvmsg(_fd,&msg,0);
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) break;
        if(n<=0) { [self closeClient]; return; }
        if(msg.msg_flags&MSG_CTRUNC) { [self error:1 text:@"too many passed fds"]; return; }
        for(struct cmsghdr *ch=CMSG_FIRSTHDR(&msg);ch;ch=CMSG_NXTHDR(&msg,ch)) if(ch->cmsg_level==SOL_SOCKET && ch->cmsg_type==SCM_RIGHTS)
            for(size_t j=0;j<(ch->cmsg_len-CMSG_LEN(0))/sizeof(int);j++) { int fd; memcpy(&fd,(int *)CMSG_DATA(ch)+j,sizeof(fd)); [_fds addObject:@(fd)]; }
        [_incoming appendBytes:bytes length:n];
        size_t consumed=0;
        while(_incoming.length-consumed>=8) {
            const uint8_t *p=(const uint8_t *)_incoming.bytes+consumed;
            uint32_t id=word(p), header=word(p+4), size=header>>16;
            if(size<8 || size>65535 || (size&3)) { [self error:id text:@"invalid message size"]; return; }
            if(_incoming.length-consumed<size) break;
            [self request:id op:header&65535 bytes:p+8 length:size-8];
            if(_dead) return; consumed+=size;
        }
        if(consumed) [_incoming replaceBytesInRange:NSMakeRange(0,consumed) withBytes:NULL length:0];
    }
}
- (void)request:(uint32_t)id op:(uint16_t)op bytes:(const uint8_t *)p length:(size_t)n {
    Obj *o=[self object:id];
    if(verbose) logLine([NSString stringWithFormat:@"-> %u %@ op %u (%zu bytes)",id,o?o.kind:@"?",op,n]);
    if(!o) { [self error:id text:@"unknown object"]; return; }
    NSString *k=o.kind;
    if([k isEqual:@"wl_display"]) {
        if(op==0 && n>=4) { uint32_t cb=word(p); [self create:cb kind:@"wl_callback" version:1]; [self event:cb opcode:0 body:u32(eventTime()) fd:-1]; [self remove:cb]; }
        else if(op==1 && n>=4) { uint32_t reg=word(p); [self create:reg kind:@"wl_registry" version:1]; for(NSDictionary *g in globals()) {
            NSMutableData *b=[NSMutableData data]; append32(b,[g[@"name"] unsignedIntValue]); appendString(b,g[@"iface"]); append32(b,[g[@"version"] unsignedIntValue]);
            [self event:reg opcode:0 body:b fd:-1]; } }
        else [self error:id text:@"invalid display request"];
    } else if([k isEqual:@"wl_registry"]) {
        if(op!=0 || n<16) { [self error:id text:@"invalid bind"]; return; }
        uint32_t name=word(p); size_t a=4; NSString *iface=readString(p,n,&a);
        uint32_t v=at32(p,n,a), newID=at32(p,n,a+4);
        NSDictionary *found=nil; for(NSDictionary *g in globals()) if([g[@"name"] unsignedIntValue]==name) found=g;
        if(!found || ![found[@"iface"] isEqual:iface] || !v || v>[found[@"version"] unsignedIntValue] || a+8>n) { [self error:id text:@"invalid global bind"]; return; }
        Obj *x=[self create:newID kind:iface version:v]; if(!x) return;
        if([iface isEqual:@"wl_shm"]) for(uint32_t fmt[] = {0,1,0x34324241,0x34324258},i=0;i<4;i++) [self event:newID opcode:0 body:u32(fmt[i]) fd:-1];
        if([iface isEqual:@"wl_output"]) [self outputEvents:x];
        if([iface isEqual:@"wl_seat"]) { [self event:newID opcode:0 body:u32(3) fd:-1]; NSMutableData *b=[NSMutableData data]; appendString(b,@"SteamARM seat"); [self event:newID opcode:1 body:b fd:-1]; }
    } else if([k isEqual:@"wl_compositor"]) {
        if(n<4 || op>1) { [self error:id text:@"invalid compositor request"]; return; }
        Obj *s=[self create:word(p) kind:op==0?@"wl_surface":@"wl_region" version:o.version];
        if(s && op==0) { s.layer=[CALayer layer]; s.layer.geometryFlipped=YES; }
    } else if([k isEqual:@"wl_region"]) {
        if(op==0) [self remove:id];
        else if((op==1 || op==2) && n>=16) { if(op==1) { o.inputX=sint(p); o.inputY=sint(p+4); o.inputW=sint(p+8); o.inputH=sint(p+12); } }
        else [self error:id text:@"invalid region request"];
    } else if([k isEqual:@"wl_surface"]) {
        switch(op) {
            case 0: [o.window close]; o.window=nil; [self remove:id]; windowsChanged(); break;
            case 1: if(n<12) goto malformed; o.pendingBuffer=word(p); o.hasPendingAttach=YES; break;
            case 2: case 9: if(n<16) goto malformed; break;
            case 3: if(n<4) goto malformed; [o.pendingFrames addObject:@(word(p))]; [self create:word(p) kind:@"wl_callback" version:1]; break;
            case 4: break;
            case 5: { if(n<4) goto malformed; Obj *r=[self object:word(p)]; o.hasInput=(r!=nil); if(r) { o.inputX=r.inputX; o.inputY=r.inputY; o.inputW=r.inputW; o.inputH=r.inputH; } break; }
            case 6: if(o.parentID && o.synchronous) break; applySurface(o); break;
            case 7: if(n<4) goto malformed; o.transform=sint(p); break;
            case 8: if(n<4) goto malformed; o.scale=MAX(1,sint(p)); break;
            default: goto malformed;
        }
    } else if([k isEqual:@"wl_shm"]) {
        if(op!=0 || n<8 || !_fds.count) goto malformed;
        Obj *pool=[self create:word(p) kind:@"wl_shm_pool" version:1]; if(!pool) return;
        pool.fd=_fds.firstObject.intValue; [_fds removeObjectAtIndex:0]; pool.poolSize=word(p+4);
        if(!pool.poolSize) { [self error:id text:@"empty shm pool"]; return; }
    } else if([k isEqual:@"wl_shm_pool"]) {
        if(op==0 && n>=24) { Obj *b=[self create:word(p) kind:@"wl_buffer" version:1]; if(!b) return;
            b.poolObject=o; b.offset=word(p+4); b.width=word(p+8); b.height=word(p+12); b.stride=word(p+16); b.format=word(p+20);
            if(b.format!=0 && b.format!=1 && b.format!=0x34324241 && b.format!=0x34324258) [self error:id text:@"unsupported shm format"];
        } else if(op==1) [self remove:id];
        else if(op==2 && n>=4) { uint32_t size=word(p); if(size<o.poolSize) [self error:id text:@"pool shrink"]; else o.poolSize=size; }
        else goto malformed;
    } else if([k isEqual:@"wl_buffer"]) { if(op==0) [self remove:id]; else goto malformed;
    } else if([k isEqual:@"wl_subcompositor"]) {
        if(op==0) [self remove:id]; else if(op==1 && n>=12) {
            Obj *sub=[self create:word(p) kind:@"wl_subsurface" version:1]; Obj *child=[self object:word(p+4)], *parent=[self object:word(p+8)];
            if(!sub || !child || !parent || ![child.kind isEqual:@"wl_surface"] || ![parent.kind isEqual:@"wl_surface"]) { [self error:id text:@"invalid subsurface"]; return; }
            sub.parentID=child.oid; child.parentID=parent.oid; [parent.children addObject:child]; [parent.layer addSublayer:child.layer];
        } else goto malformed;
    } else if([k isEqual:@"wl_subsurface"]) {
        Obj *child=[self object:o.parentID]; Obj *parent=[self object:child.parentID];
        if(op==0) { [parent.children removeObject:child]; child.parentID=0; [self remove:id]; }
        else if(op==1 && n>=8) { child.pendingX=sint(p); child.pendingY=sint(p+4); child.pendingSubPos=YES; }
        else if((op==2 || op==3) && n>=4) { Obj *sibling=[self object:word(p)]; if(sibling && [parent.children containsObject:sibling]) {
            [parent.children removeObject:child]; NSUInteger idx=[parent.children indexOfObject:sibling]; [parent.children insertObject:child atIndex:MIN(parent.children.count,idx+(op==2))];
            [child.layer removeFromSuperlayer]; [parent.layer insertSublayer:child.layer atIndex:(unsigned)[parent.children indexOfObject:child]];
        } }
        else if(op==4) child.synchronous=YES;
        else if(op==5) { child.synchronous=NO; applySurface(child); }
        else goto malformed;
    } else if([k isEqual:@"wl_output"]) { if(op==0 && o.version>=3) [self remove:id]; else goto malformed;
    } else if([k isEqual:@"wl_seat"]) {
        if((op==0 || op==1) && n>=4) { Obj *device=[self create:word(p) kind:op==0?@"wl_pointer":@"wl_keyboard" version:o.version]; if(!device) return;
            if(op==0) _pointerID=device.oid; else { _keyboardID=device.oid; [self sendKeymap]; } }
        else if(op==3 && o.version>=5) [self remove:id]; else goto malformed;
    } else if([k isEqual:@"wl_pointer"] || [k isEqual:@"wl_keyboard"]) {
        // wl_pointer: 0 set_cursor (the client's cursor surface; the Mac's
        // arrow stays), 1 release. wl_keyboard: 0 release.
        BOOL pointer=[k isEqual:@"wl_pointer"];
        if(pointer && op==0) { if(n<16) goto malformed; }
        else if((pointer && op==1) || (!pointer && op==0)) { if(pointer) _pointerID=0; else _keyboardID=0; [self remove:id]; }
        else goto malformed;
    } else if([k isEqual:@"xdg_wm_base"]) {
        if(op==0) [self remove:id];
        else if(op==2 && n>=8) { Obj *xdg=[self create:word(p) kind:@"xdg_surface" version:o.version], *s=[self object:word(p+4)];
            if(!xdg || !s || ![s.kind isEqual:@"wl_surface"]) { [self error:id text:@"invalid xdg surface"]; return; }
            xdg.parentID=s.oid; s.xdgID=xdg.oid;
        } else if(op==3 && n>=4) { /* pong */ } else goto malformed;
    } else if([k isEqual:@"xdg_surface"]) {
        Obj *s=[self object:o.parentID];
        if(op==0) [self remove:id];
        else if(op==1 && n>=4) { Obj *top=[self create:word(p) kind:@"xdg_toplevel" version:o.version]; if(!top) return;
            top.parentID=s.oid; s.topID=top.oid; configureTop(s,0,0);
        } else if(op==2 && n>=12) { Obj *pop=[self create:word(p) kind:@"xdg_popup" version:o.version]; pop.parentID=s.oid; s.topID=pop.oid; [self event:o.oid opcode:0 body:u32(serialNext()) fd:-1]; }
        else if(op==3 && n>=16) {     // set_window_geometry: applied on the surface's next commit
            s.pendingGeoX=sint(p); s.pendingGeoY=sint(p+4); s.pendingGeoW=sint(p+8); s.pendingGeoH=sint(p+12); s.pendingGeo=YES;
            if(verbose) logLine([NSString stringWithFormat:@"geometry surface %u: %d,%d %dx%d",s.oid,s.pendingGeoX,s.pendingGeoY,s.pendingGeoW,s.pendingGeoH]);
        }
        else if(op==4 && n>=4) { s.configured=YES; }
        else goto malformed;
    } else if([k isEqual:@"xdg_toplevel"]) {
        Obj *s=[self object:o.parentID];
        if(op==0) { [s.window close]; s.window=nil; s.topID=0; [self remove:id]; windowsChanged(); }
        else if(op==2 || op==3) { size_t a=0; NSString *value=readString(p,n,&a); if(!value) goto malformed;
            if(op==2) { s.title=value; s.window.title=value; } else s.appID=value;
        } else if(op==7 || op==8) { /* min/max size stored by client; AppKit resizing remains free */ }
        else if(op==9 || op==10 || op==11 || op==12) { s.maximized=(op==9)?YES:((op==10)?NO:s.maximized); s.fullscreen=(op==11)?YES:((op==12)?NO:s.fullscreen); configureTop(s,(int)s.window.contentView.bounds.size.width,(int)s.window.contentView.bounds.size.height); }
        else if(op==4 || op==5 || op==6 || op==13 || op==1) { /* menu/move/resize/minimise/parent hints */ }
        else goto malformed;
    } else if([k isEqual:@"xdg_popup"]) { if(op==0) [self remove:id]; else if(op==1 && n>=4) { /* grab */ } else goto malformed;
    } else if([k isEqual:@"wp_viewporter"]) {
        if(op==0) [self remove:id]; else if(op==1 && n>=8) { Obj *v=[self create:word(p) kind:@"wp_viewport" version:1]; Obj *s=[self object:word(p+4)]; if(!v || !s) { [self error:id text:@"invalid viewport"]; return; } v.parentID=s.oid; }
        else goto malformed;
    } else if([k isEqual:@"wp_viewport"]) {
        Obj *s=[self object:o.parentID]; if(op==0) [self remove:id];
        else if(op==1 && n>=16) { /* source crop in 24.8 fixed coordinates */
            s.sourceX=sint(p); s.sourceY=sint(p+4); s.sourceW=sint(p+8); s.sourceH=sint(p+12);
            if(s.width && s.height && s.sourceW>0 && s.sourceH>0) s.layer.contentsRect=CGRectMake((double)s.sourceX/256/s.width,(double)s.sourceY/256/s.height,(double)s.sourceW/256/s.width,(double)s.sourceH/256/s.height);
        } else if(op==2 && n>=8) { s.viewportW=sint(p); s.viewportH=sint(p+4); layoutSurface(s); }
        else goto malformed;
    } else if([k isEqual:@"wl_callback"]) { [self error:id text:@"callback has no requests"]; }
    else goto malformed;
    return;
malformed: [self error:id text:[NSString stringWithFormat:@"malformed %@ request %u",k,op]];
}
- (void)outputEvents:(Obj *)o {
    NSScreen *screen=NSScreen.mainScreen; NSRect r=screen?screen.frame:NSMakeRect(0,0,1440,900);
    NSMutableData *b=[NSMutableData data]; append32(b,0); append32(b,0); append32(b,(uint32_t)(r.size.width*0.2646)); append32(b,(uint32_t)(r.size.height*0.2646)); append32(b,0);
    appendString(b,@"SteamARM"); appendString(b,@"Main display"); append32(b,0); [self event:o.oid opcode:0 body:b fd:-1];
    b=[NSMutableData data]; append32(b,3); append32(b,(uint32_t)r.size.width); append32(b,(uint32_t)r.size.height); append32(b,60000); [self event:o.oid opcode:1 body:b fd:-1];
    if(o.version>=2) { [self event:o.oid opcode:3 body:u32(1) fd:-1]; [self event:o.oid opcode:2 body:[NSData data] fd:-1]; }
}
- (void)sendKeymap {
    NSString *source=[NSString stringWithContentsOfFile:[NSString stringWithUTF8String:keymapPath] encoding:NSUTF8StringEncoding error:nil];
    if(!source) { [self error:_keyboardID text:@"missing embedded keymap"]; return; }
    NSData *data=[source dataUsingEncoding:NSUTF8StringEncoding];
    NSString *tmp=[NSTemporaryDirectory() stringByAppendingPathComponent:[NSString stringWithFormat:@"wlmac-keymap-%u-XXXXXX",arc4random()]];
    char *path=strdup(tmp.fileSystemRepresentation); int fd=mkstemp(path); if(fd<0) { free(path); [self error:_keyboardID text:@"keymap tempfile failed"]; return; }
    unlink(path); free(path); write(fd,data.bytes,data.length); uint8_t zero=0; write(fd,&zero,1); lseek(fd,0,SEEK_SET);
    NSMutableData *b=[NSMutableData data]; append32(b,1); append32(b,(uint32_t)data.length+1);
    [self event:_keyboardID opcode:0 body:b fd:fd]; close(fd);
    if([self object:_keyboardID].version>=4) { b=[NSMutableData data]; append32(b,25); append32(b,600); [self event:_keyboardID opcode:5 body:b fd:-1]; }
}
@end

static void tick(void) {
    for(Client *c in [clients copy]) for(Obj *o in [c.objects.allValues copy]) if([o.kind isEqual:@"wl_surface"] && o.frames.count) {
        NSArray *frames=[o.frames copy]; [o.frames removeAllObjects];
        for(NSNumber *f in frames) { [c event:f.unsignedIntValue opcode:0 body:u32(eventTime()) fd:-1]; [c remove:f.unsignedIntValue]; }
    }
}
static void shutdownServer(int signalNumber) {
    (void)signalNumber; if(listenSource) { dispatch_source_cancel(listenSource); listenSource=nil; }
    for(Client *c in [clients copy]) [c closeClient];
    if(listenFD>=0) { close(listenFD); listenFD=-1; }
    if(socketPath) unlink(socketPath.fileSystemRepresentation);
    logLine([NSString stringWithFormat:@"exit frames=%llu copied_bytes=%llu copy_us_per_frame=%llu",copyFrames,copyBytes,copyFrames?copyNanos/copyFrames/1000:0]);
    [NSApp terminate:nil];
}
static int visibleWindows(void) {
    int n=0; for(Client *c in clients) for(Obj *o in c.objects.allValues) if(o.window && o.window.isVisible) n++;
    return n;
}
static void windowsChanged(void) {
    if(visibleWindows()) { hadWindow=YES; return; }
    if(!exitWhenEmpty || !hadWindow) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW,5*NSEC_PER_SEC),dispatch_get_main_queue(),^{
        if(!visibleWindows()) { logLine(@"no window left for 5 s: exiting (--exit-when-empty)"); shutdownServer(0); }
    });
}
static void handleSignal(int signo) { dispatch_async(dispatch_get_main_queue(),^{ shutdownServer(signo); }); }
// Test input (SIGUSR1): <socket>.cmd holds lines "click X Y" (points from the
// top left of the newest window's content) and "key MACKEYCODE"; each goes
// through the same path as a person's mouse and keyboard.
static void runCommands(void) {
    NSString *text=[NSString stringWithContentsOfFile:[socketPath stringByAppendingString:@".cmd"] encoding:NSUTF8StringEncoding error:nil];
    WWindow *w=newestWindow; WView *v=(WView *)w.contentView;
    if(!text || !w || ![v isKindOfClass:[WView class]]) { logLine(@"command: no window or no command file"); return; }
    [w windowDidBecomeKey:[NSNotification notificationWithName:NSWindowDidBecomeKeyNotification object:w]];
    for(NSString *line in [text componentsSeparatedByString:@"\n"]) {
        NSArray<NSString *> *a=[line componentsSeparatedByString:@" "];
        if(a.count==3 && [a[0] isEqual:@"click"]) {
            NSPoint loc=NSMakePoint(a[1].doubleValue, v.bounds.size.height-a[2].doubleValue);
            NSEvent *move=[NSEvent mouseEventWithType:NSEventTypeMouseMoved location:loc modifierFlags:0 timestamp:0 windowNumber:w.windowNumber context:nil eventNumber:1 clickCount:0 pressure:0];
            NSEvent *down=[NSEvent mouseEventWithType:NSEventTypeLeftMouseDown location:loc modifierFlags:0 timestamp:0 windowNumber:w.windowNumber context:nil eventNumber:2 clickCount:1 pressure:1];
            NSEvent *up=[NSEvent mouseEventWithType:NSEventTypeLeftMouseUp location:loc modifierFlags:0 timestamp:0 windowNumber:w.windowNumber context:nil eventNumber:3 clickCount:1 pressure:0];
            pointerEvent(v,move,0); pointerEvent(v,down,2); pointerEvent(v,up,3);
            logLine([NSString stringWithFormat:@"command: click %@,%@ in \"%@\"",a[1],a[2],w.title]);
        } else if(a.count==2 && [a[0] isEqual:@"key"]) {
            NSEvent *k=[NSEvent keyEventWithType:NSEventTypeKeyDown location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:w.windowNumber context:nil characters:@"" charactersIgnoringModifiers:@"" isARepeat:NO keyCode:(unsigned short)a[1].intValue];
            keyboardEvent(v,k,YES); keyboardEvent(v,k,NO);
            logLine([NSString stringWithFormat:@"command: key %@ in \"%@\"",a[1],w.title]);
        }
    }
}
static void handleCommand(int signo) { (void)signo; dispatch_async(dispatch_get_main_queue(),^{ runCommands(); }); }
static BOOL startSocket(void) {
    listenFD=socket(AF_UNIX,SOCK_STREAM,0); if(listenFD<0) return NO;
    struct sockaddr_un sa={0}; sa.sun_family=AF_UNIX;
    if(strlen(socketPath.fileSystemRepresentation)>=sizeof(sa.sun_path)) return NO;
    strlcpy(sa.sun_path,socketPath.fileSystemRepresentation,sizeof(sa.sun_path));
    struct stat st; if(lstat(sa.sun_path,&st)==0) { if(!S_ISSOCK(st.st_mode)) return NO; unlink(sa.sun_path); }
    if(bind(listenFD,(struct sockaddr *)&sa,sizeof(sa)) || listen(listenFD,64)) return NO;
    chmod(sa.sun_path,0600); fcntl(listenFD,F_SETFL,O_NONBLOCK);
    listenSource=dispatch_source_create(DISPATCH_SOURCE_TYPE_READ,listenFD,0,dispatch_get_main_queue());
    dispatch_source_set_event_handler(listenSource,^{ for(;;) { int fd=accept(listenFD,NULL,NULL); if(fd<0) break;
        fcntl(fd,F_SETFL,O_NONBLOCK); Client *c=[[Client alloc] initWithFD:fd]; [clients addObject:c]; if(verbose) logLine(@"client connected"); } });
    dispatch_resume(listenSource); return YES;
}
int main(int argc, const char **argv) {
    @autoreleasepool {
        for(int i=1;i<argc;i++) {
            if(!strcmp(argv[i],"--socket") && i+1<argc) socketPath=[NSString stringWithUTF8String:argv[++i]];
            else if(!strcmp(argv[i],"--dump-dir") && i+1<argc) dumpDir=[NSString stringWithUTF8String:argv[++i]];
            else if(!strcmp(argv[i],"--verbose")) verbose=YES;
            else if(!strcmp(argv[i],"--selftest-input")) selftestInput=YES;
            else if(!strcmp(argv[i],"--exit-when-empty")) exitWhenEmpty=YES;
            else { fprintf(stderr,"usage: steamarm-wlmac --socket ABSOLUTE_PATH [--dump-dir DIR] [--verbose] [--selftest-input] [--exit-when-empty]\n"); return 2; }
        }
        if(!socketPath.length || ![socketPath hasPrefix:@"/"]) { fprintf(stderr,"--socket requires absolute path\n"); return 2; }
        if(dumpDir.length) [[NSFileManager defaultManager] createDirectoryAtPath:dumpDir withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *exec=[NSString stringWithUTF8String:argv[0]];
        keymapPath=[[[exec stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"../tools/wlmac/us.xkb"] stringByStandardizingPath].fileSystemRepresentation;
        clients=[NSMutableArray array]; [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSMenu *bar=[NSMenu new], *appMenu=[NSMenu new]; [appMenu addItemWithTitle:@"Quit SteamARM Android" action:@selector(terminate:) keyEquivalent:@"q"];
        NSMenuItem *item=[NSMenuItem new]; item.title=@"SteamARM Android"; item.submenu=appMenu; [bar addItem:item]; NSApp.mainMenu=bar;
        signal(SIGPIPE,SIG_IGN); signal(SIGTERM,handleSignal); signal(SIGINT,handleSignal); signal(SIGUSR1,handleCommand);
        if(!startSocket()) { perror("wlmac socket"); return 1; }
        frameTimer=[NSTimer scheduledTimerWithTimeInterval:1.0/60.0 repeats:YES block:^(NSTimer *t){ (void)t; tick(); }];
        logLine([NSString stringWithFormat:@"listening %@",socketPath]); [NSApp run];
        [frameTimer invalidate]; return 0;
    }
}
