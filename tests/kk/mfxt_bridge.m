// The MetalFX temporal encode of SteamARM's KosmicKrisp
// (patches/kosmickrisp-20-metalfx-temporal-entry.patch, bridge/mtl_metalfx.m)
// on the host, without Vulkan: the bridge's own source is compiled in, and
// its textures are made the way KosmicKrisp makes them (shared storage,
// untracked, in a placement heap, resident through a residency set on a
// Metal 4 queue). Each frame is one Metal 4 command buffer as KosmicKrisp
// records it: an encoder writes the inputs and ends with the producer
// barrier every KosmicKrisp encoder ends with, MetalFX is encoded, and the
// next encoder (the read back) starts with the queue barrier after_metalfx
// gives it.
//
// The picture: a pattern rendered at 960x540 with a known sub-pixel jitter
// (Halton 2,3) and, in the moving cases, a known motion per frame; the
// output (1920x1080) is compared with a supersampled reference over the
// last 8 of 64 frames (RMSE; the pattern is about 0.25..0.75). The game's
// conventions go through unchanged: the game's jitter is the negated sample
// offset, its motion vectors point from the current position to the
// previous one.
//
// Checks: the jitter and motion vector signs (the right ones clearly best),
// the output plausible, a dynamic resolution keeps its one scaler, the
// refusals (more than 3x, a downscale, missing depth, a format MetalFX does
// not take, sizes outside a texture, a view a texture does not allow) with
// their status, a destroyed context's scaler living on until its command
// buffer is gone, the tail encoder (mtl_metalfx_temporal_tail) for a read in
// a later command buffer, exposure / reactive / auto exposure, and motion
// vectors at the output's size (macOS 27).
//
// Usage: tests/kk/run_mfxt_bridge.sh (builds it; runs it plain and with
// KK_MFXT_SCRATCH=1). Prints "== mfxt_bridge: ok" or what failed.
#include "mtl_metalfx.m" /* the bridge itself, its statics included */

#import <Foundation/Foundation.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { IW = 960, IH = 540, OW = 1920, OH = 1080, FRAMES = 64, MEASURE = 8 };

static id<MTLDevice> dev;
static id<MTL4Compiler> comp;
static id<MTL4CommandQueue> q;
static id<MTL4CommandAllocator> alloc;
static id<MTLSharedEvent> ev;
static uint64_t ev_value;
static id<MTLHeap> heap;
static NSUInteger heap_off;
static id<MTLResidencySet> rs;
static int failures;

/* Released with the object it is attached to: tells when that is gone. */
static volatile int sentinel_gone;
@interface MfxtSentinel : NSObject
@end
@implementation MfxtSentinel
- (void)dealloc
{
    sentinel_gone = 1;
    [super dealloc];
}
@end
static char sentinel_key;

#define FAIL(...) do { printf("  FAIL  "); printf(__VA_ARGS__); printf("\n"); failures++; } while (0)
#define OK(...) do { printf("  ok    "); printf(__VA_ARGS__); printf("\n"); } while (0)

/* A texture as KosmicKrisp makes one. */
static id<MTLTexture>
ktex(MTLPixelFormat f, NSUInteger w, NSUInteger h, MTLTextureUsage usage)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    d.usage = usage;
    d.storageMode = MTLStorageModeShared;
    d.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    MTLSizeAndAlign sa = [dev heapTextureSizeAndAlignWithDescriptor:d];
    heap_off = (heap_off + sa.align - 1) / sa.align * sa.align;
    id<MTLTexture> t = [heap newTextureWithDescriptor:d offset:heap_off];
    heap_off += sa.size;
    if (!t) { printf("texture %lu %lux%lu: none\n== mfxt_bridge: FAIL\n", (unsigned long)f, (unsigned long)w, (unsigned long)h); exit(1); }
    return t;
}

static id<MTLBuffer>
kbuf(NSUInteger size)
{
    id<MTLBuffer> b = [dev newBufferWithLength:size options:MTLResourceStorageModeShared];
    [rs addAllocation:b];
    [rs commit];
    return b;
}

static void
setup(void)
{
    dev = MTLCreateSystemDefaultDevice();
    NSError *e = nil;
    comp = [dev newCompilerWithDescriptor:[[MTL4CompilerDescriptor new] autorelease] error:&e];
    q = [dev newMTL4CommandQueue];
    alloc = [dev newCommandAllocator];
    ev = [dev newSharedEvent];
    MTLHeapDescriptor *hd = [[MTLHeapDescriptor new] autorelease];
    hd.type = MTLHeapTypePlacement;
    hd.storageMode = MTLStorageModeShared;
    hd.hazardTrackingMode = MTLHazardTrackingModeUntracked;
    hd.size = 1ull << 30;
    heap = [dev newHeapWithDescriptor:hd];
    rs = [dev newResidencySetWithDescriptor:[[MTLResidencySetDescriptor new] autorelease] error:&e];
    [rs addAllocation:heap];
    [rs commit];
    [q addResidencySet:rs];
}

static id<MTL4CommandBuffer>
begin_cb(void)
{
    id<MTL4CommandBuffer> cb = [dev newCommandBuffer];
    [cb beginCommandBufferWithAllocator:alloc];
    return cb;
}

/* Commits, waits, and releases the command buffer (as KosmicKrisp does when
 * the GPU has finished it). Returns false on a GPU error. */
static bool
commit_wait(id<MTL4CommandBuffer> cb)
{
    [cb endCommandBuffer];
    __block bool gpu_ok = true;
    MTL4CommitOptions *opt = [[MTL4CommitOptions new] autorelease];
    [opt addFeedbackHandler:^(id<MTL4CommitFeedback> fb) {
        if (fb.error) { fprintf(stderr, "feedback error: %s\n", fb.error.description.UTF8String); gpu_ok = false; }
    }];
    id<MTL4CommandBuffer> arr[1] = {cb};
    [q commit:arr count:1 options:opt];
    [q signalEvent:ev value:++ev_value];
    if (![ev waitUntilSignaledValue:ev_value timeoutMS:10000]) { printf("GPU timeout\n== mfxt_bridge: FAIL\n"); exit(1); }
    usleep(2000); /* the feedback handler */
    [alloc reset];
    [cb release];
    return gpu_ok;
}

/* Commits without waiting (the next command buffer goes right behind it,
 * as KosmicKrisp's commits do); commit_wait on the next one waits for both.
 * The command buffer is released by the caller. */
static void
commit_nowait(id<MTL4CommandBuffer> cb)
{
    [cb endCommandBuffer];
    id<MTL4CommandBuffer> arr[1] = {cb};
    [q commit:arr count:1];
}

/* ---- the scene */
static float dir[3][2];
static void
pattern_init(void)
{
    const float f[3] = {0.31f, 0.58f, 0.72f}, a[3] = {0.3f, 1.2f, 2.3f};
    for (int i = 0; i < 3; i++) { dir[i][0] = f[i] * cosf(a[i]); dir[i][1] = f[i] * sinf(a[i]); }
}
static float
pattern(float x, float y)
{
    float v = 0.5f;
    v += 0.12f * sinf(6.2831853f * (x * dir[0][0] + y * dir[0][1]));
    v += 0.12f * sinf(6.2831853f * (x * dir[1][0] + y * dir[1][1]));
    v += 0.10f * sinf(6.2831853f * (x * dir[2][0] + y * dir[2][1]));
    float rx = (x * 0.9397f - y * 0.3420f) / 2.7f, ry = (x * 0.3420f + y * 0.9397f) / 2.7f;
    float c = fmodf(floorf(rx) + floorf(ry), 2.0f);
    if (c < 0) c += 2;
    return v + (c - 0.5f) * 0.12f;
}
static float
halton(int i, int b)
{
    float f = 1, r = 0;
    while (i > 0) { f /= b; r += f * (i % b); i /= b; }
    return r;
}

struct scene {
    id<MTLTexture> color, depth, motion, output, exposure, reactive;
    id<MTLBuffer> color_stage, motion_stage, depth_stage, out_rb, small_stage;
    NSUInteger mw, mh; /* motion texture size */
};

static struct scene
scene_new(MTLPixelFormat out_format, bool mv_display)
{
    struct scene s = {0};
    /* Usage as KosmicKrisp gives it: sampled + attachment (patch 19: shader
     * write only for storage images; FSR's output is one, a D3D12 UAV). */
    s.color = ktex(MTLPixelFormatRGBA16Float, IW, IH, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    s.depth = ktex(MTLPixelFormatDepth32Float, IW, IH, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    s.mw = mv_display ? OW : IW;
    s.mh = mv_display ? OH : IH;
    s.motion = ktex(MTLPixelFormatRG16Float, s.mw, s.mh, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
    s.output = ktex(out_format, OW, OH, MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget);
    s.color_stage = kbuf(IW * IH * 8);
    s.motion_stage = kbuf(s.mw * s.mh * 4);
    s.depth_stage = kbuf(IW * IH * 4);
    s.out_rb = kbuf(OW * OH * 8);
    s.small_stage = kbuf(4096);
    float *d = s.depth_stage.contents;
    for (int i = 0; i < IW * IH; i++) d[i] = 0.5f;
    return s;
}

struct run {
    float jitter_mul;      /* jitter given to MetalFX = jitter_mul * sample offset */
    int jitter_on;
    float vx, vy;          /* the content moves by +v input pixels a frame */
    float mv_mul;          /* the motion vectors written = mv_mul * v */
    bool mv_display, dynamic, exposure, reactive, auto_exposure, tail_only, no_tail;
    float hdr;             /* colour multiplied by this */
    uint64_t context;
    int frames;
    int encode_status;     /* last status */
    double mean;
};

/* One frame's inputs into their staging buffers, and their copies. */
static void
write_inputs(struct scene *s, struct run *r, int t, float sx, float sy, uint32_t rw, uint32_t rh)
{
    __fp16 *c = s->color_stage.contents;
    /* The picture of rw x rh in the top left: the same view of the scene
     * at a lower resolution (a dynamic resolution); scene units are pixels
     * of the full 960x540 render, the jitter is in render pixels. */
    float kx = (float)IW / rw, ky = (float)IH / rh;
    for (uint32_t y = 0; y < IH; y++)
        for (uint32_t x = 0; x < IW; x++) {
            float px = (x + 0.5f + sx) * kx - r->vx * t;
            float py = (y + 0.5f + sy) * ky - r->vy * t;
            float v = (x < rw && y < rh) ? pattern(px, py) * r->hdr : 0.0f;
            __fp16 *o = &c[(y * IW + x) * 4];
            o[0] = o[1] = o[2] = v;
            o[3] = 1;
        }
    __fp16 *m = s->motion_stage.contents;
    float k = r->mv_display ? (float)OW / rw : 1.0f;
    for (NSUInteger i = 0; i < s->mw * s->mh; i++) {
        m[i * 2] = r->mv_mul * r->vx * k;
        m[i * 2 + 1] = r->mv_mul * r->vy * k;
    }
}

static void
copy_in(id<MTL4ComputeCommandEncoder> ce, id<MTLBuffer> b, NSUInteger bpp, id<MTLTexture> t, NSUInteger w, NSUInteger h)
{
    [ce copyFromBuffer:b sourceOffset:0 sourceBytesPerRow:w * bpp sourceBytesPerImage:w * h * bpp
            sourceSize:MTLSizeMake(w, h, 1) toTexture:t destinationSlice:0 destinationLevel:0
     destinationOrigin:MTLOriginMake(0, 0, 0)];
}

static struct mtl_metalfx_temporal_args
args_for(struct scene *s, struct run *r, uint32_t rw, uint32_t rh)
{
    struct mtl_metalfx_temporal_args a = {0};
    a.context = r->context;
    a.color.texture = s->color;
    a.depth.texture = s->depth;
    a.motion.texture = s->motion;
    a.output.texture = s->output;
    if (r->exposure) a.exposure.texture = s->exposure;
    if (r->reactive) a.reactive.texture = s->reactive;
    a.render_w = rw;
    a.render_h = rh;
    a.out_w = OW;
    a.out_h = OH;
    a.mv_scale_x = a.mv_scale_y = 1.0f;
    a.pre_exposure = 0.0f; /* 0: 1 */
    a.depth_reversed = false;
    a.auto_exposure = r->auto_exposure;
    a.mv_display = r->mv_display;
    return a;
}

/* RMSE of the output (RGBA16F in out_rb) against the supersampled scene at
 * frame t, every 3rd pixel away from the borders. */
static double
rmse(struct scene *s, struct run *r, int t, double *mean)
{
    const __fp16 *o = s->out_rb.contents;
    double se = 0, sum = 0;
    long n = 0;
    float k = (float)IW / OW;
    for (int y = 16; y < OH - 16; y += 3)
        for (int x = 16; x < OW - 16; x += 3) {
            float ref = 0;
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                    ref += pattern((x + (i + 0.5f) / 4) * k - r->vx * t, (y + (j + 0.5f) / 4) * k - r->vy * t);
            ref = ref / 16 * r->hdr;
            double v = o[(y * OW + x) * 4];
            se += (v - ref) * (v - ref);
            sum += v;
            n++;
        }
    *mean = sum / n;
    return sqrt(se / n);
}

/* The bridge refuses a ninth scaler made within 10 s (a game making them
 * again and again); this test makes one per case on purpose. */
static void
limiter_reset(void)
{
    pthread_mutex_lock(&mfxt_lock);
    memset(mfxt_made_at, 0, sizeof(mfxt_made_at));
    pthread_mutex_unlock(&mfxt_lock);
}

static double
run_case(struct scene *s, struct run *r)
{
    limiter_reset();
    double err = 0, mean = 0;
    int nerr = 0;
    int frames = r->frames ? r->frames : FRAMES;
    for (int t = 0; t < frames; t++) {
        float sx = r->jitter_on ? halton(t % 32 + 1, 2) - 0.5f : 0;
        float sy = r->jitter_on ? halton(t % 32 + 1, 3) - 0.5f : 0;
        uint32_t rw = IW, rh = IH;
        if (r->dynamic && (t / 4) % 2) { rw = 864; rh = 486; } /* 2.22x, then 2x */
        write_inputs(s, r, t, sx, sy, rw, rh);
        bool measure = t >= frames - MEASURE && !r->dynamic;
        if (r->dynamic && t == frames - 1) measure = true;

        id<MTL4CommandBuffer> cb = begin_cb(), prev = nil;
        id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
        copy_in(ce, s->color_stage, 8, s->color, IW, IH);
        copy_in(ce, s->motion_stage, 4, s->motion, s->mw, s->mh);
        if (t == 0) copy_in(ce, s->depth_stage, 4, s->depth, IW, IH);
        /* The producer barrier every KosmicKrisp encoder ends with. */
        [ce barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
        [ce endEncoding];

        struct mtl_metalfx_temporal_args a = args_for(s, r, rw, rh);
        a.jitter_x = r->jitter_mul * sx;
        a.jitter_y = r->jitter_mul * sy;
        a.reset = t == 0;
        const char *why = NULL;
        enum mtl_metalfx_temporal_status st = mtl_metalfx_temporal_encode(dev, comp, cb, &a, &why);
        r->encode_status = st;
        if (st != MTL_MFXT_OK) {
            printf("    encode: status %d (%s)\n", st, why ? why : "");
            commit_wait(cb);
            return -1;
        }
        if (r->tail_only && measure) {
            /* Nothing of the driver after MetalFX in this command buffer: the
             * tail, and the read in the next one, with no barrier of its own
             * (as a later KosmicKrisp command buffer's first encoder). */
            if (!r->no_tail)
                mtl_metalfx_temporal_tail(dev, cb);
            commit_nowait(cb);
            prev = cb;
            cb = begin_cb();
            ce = [cb computeCommandEncoder];
        } else if (measure) {
            ce = [cb computeCommandEncoder];
            /* after_metalfx: the next encoder waits for all that came before. */
            [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
        }
        if (measure) {
            [ce copyFromTexture:s->output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                     sourceSize:MTLSizeMake(OW, OH, 1) toBuffer:s->out_rb destinationOffset:0
         destinationBytesPerRow:OW * 8 destinationBytesPerImage:OW * OH * 8];
            [ce endEncoding];
        }
        if (!commit_wait(cb)) return -2;
        [prev release];
        prev = nil;
        if (measure) {
            double m;
            /* A dynamic resolution's last frame was rendered at 864x486. */
            err += rmse(s, r, t, &m);
            mean += m;
            nerr++;
        }
    }
    r->mean = mean / nerr;
    return err / nerr;
}

static enum mtl_metalfx_temporal_status
refusal(struct mtl_metalfx_temporal_args a, const char **why)
{
    limiter_reset();
    id<MTL4CommandBuffer> cb = begin_cb();
    enum mtl_metalfx_temporal_status st = mtl_metalfx_temporal_encode(dev, comp, cb, &a, why);
    commit_wait(cb);
    return st;
}

int
main(void)
{
    @autoreleasepool {
        setup();
        pattern_init();
        bool scratch = mtl_metalfx_temporal_scratch();
        struct mtl_metalfx_temporal_caps caps;
        mtl_metalfx_temporal_get_caps(dev, &caps);
        printf("== mfxt_bridge: %s, temporal %d, scale %.2f..%.2f, mv_display %d, offsets %d, output %s\n",
               dev.name.UTF8String, caps.supported, caps.min_scale, caps.max_scale, caps.mv_display, caps.offsets,
               scratch ? "through a private copy" : "written directly (shared)");
        if (!caps.supported) { printf("== mfxt_bridge: skip (no Metal 4 MetalFX temporal scaler)\n"); return 0; }

        struct scene s = scene_new(MTLPixelFormatRGBA16Float, false);
        s.exposure = ktex(MTLPixelFormatR16Float, 1, 1, MTLTextureUsageShaderRead);
        s.reactive = ktex(MTLPixelFormatR8Unorm, IW, IH, MTLTextureUsageShaderRead);
        {   /* exposure 1, reactive 0 */
            id<MTL4CommandBuffer> cb = begin_cb();
            id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
            __fp16 one = 1.0f;
            memcpy(s.small_stage.contents, &one, 2);
            [ce copyFromBuffer:s.small_stage sourceOffset:0 sourceBytesPerRow:2 sourceBytesPerImage:2
                    sourceSize:MTLSizeMake(1, 1, 1) toTexture:s.exposure destinationSlice:0 destinationLevel:0
             destinationOrigin:MTLOriginMake(0, 0, 0)];
            memset(s.color_stage.contents, 0, IW * IH);
            copy_in(ce, s.color_stage, 1, s.reactive, IW, IH);
            [ce barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:MTL4VisibilityOptionDevice];
            [ce endEncoding];
            commit_wait(cb);
        }

        /* 1. Jitter: the game's value (minus the sample offset) against the
         * opposite sign and none, on a still picture. */
        struct run good = {.jitter_mul = -1, .jitter_on = 1, .hdr = 1, .context = 1};
        double e_good = run_case(&s, &good);
        struct run bad = good; bad.jitter_mul = 1; bad.context = 2;
        double e_bad = run_case(&s, &bad);
        struct run none = good; none.jitter_mul = 0; none.context = 3;
        double e_none = run_case(&s, &none);
        printf("    still, jitter as the game gives it %.5f, opposite sign %.5f, told none %.5f (mean %.3f)\n",
               e_good, e_bad, e_none, good.mean);
        if (e_good < 0 || e_good > 0.04 || e_good * 2 > e_bad || e_good * 2 > e_none || fabs(good.mean - 0.5) > 0.05)
            FAIL("jitter: the game's convention is not clearly the best, or the picture is wrong");
        else
            OK("jitter passed through unchanged: RMSE %.4f (opposite %.4f, none %.4f)", e_good, e_bad, e_none);

        /* 2. Motion vectors: previous minus current position, input pixels. */
        struct run mv = good; mv.vx = 1.37f; mv.vy = 0.61f; mv.mv_mul = -1; mv.context = 4;
        double e_mv = run_case(&s, &mv);
        struct run mvp = mv; mvp.mv_mul = 1; mvp.context = 5;
        double e_mvp = run_case(&s, &mvp);
        struct run mv0 = mv; mv0.mv_mul = 0; mv0.context = 6;
        double e_mv0 = run_case(&s, &mv0);
        printf("    moving (1.37, 0.61)/frame: motion vectors -v %.5f, +v %.5f, 0 %.5f\n", e_mv, e_mvp, e_mv0);
        if (e_mv < 0 || e_mv >= e_mvp || e_mv >= e_mv0 || e_mv > 0.15)
            FAIL("motion vectors: the game's convention is not the best");
        else
            OK("motion vectors passed through unchanged: RMSE %.4f (+v %.4f, 0 %.4f)", e_mv, e_mvp, e_mv0);

        /* 3. A dynamic resolution: one scaler for the context. */
        unsigned made0 = mfxt_made;
        struct run dyn = good; dyn.dynamic = true; dyn.context = 7; dyn.frames = 24;
        double e_dyn = run_case(&s, &dyn);
        unsigned made_dyn = mfxt_made - made0;
        if (e_dyn < 0 || e_dyn > 0.08 || made_dyn != 1)
            FAIL("dynamic resolution: RMSE %.4f, %u scalers made", e_dyn, made_dyn);
        else
            OK("dynamic resolution 960x540 / 864x486 -> 1920x1080: one scaler, RMSE %.4f", e_dyn);

        /* 4. Exposure, reactive mask; HDR with auto exposure. */
        struct run ex = good; ex.exposure = ex.reactive = true; ex.context = 8;
        double e_ex = run_case(&s, &ex);
        struct run hdr = good; hdr.hdr = 8; hdr.auto_exposure = true; hdr.context = 9;
        double e_hdr = run_case(&s, &hdr);
        printf("    exposure 1 + reactive 0: %.5f; HDR x8 auto exposure: %.5f (mean %.3f)\n", e_ex, e_hdr, hdr.mean);
        if (e_ex < 0 || e_ex > 0.04 || e_hdr < 0 || e_hdr > 8 * 0.04 || fabs(hdr.mean - 4.0) > 0.4)
            FAIL("exposure / reactive / auto exposure");
        else
            OK("exposure texture + reactive mask RMSE %.4f; HDR auto exposure RMSE %.4f, mean %.2f (x8)", e_ex, e_hdr, hdr.mean);

        /* 5. The tail: read in the next command buffer. */
        struct run tl = good; tl.tail_only = true; tl.context = 10;
        double e_tl = run_case(&s, &tl);
        if (e_tl < 0 || e_tl > 0.04)
            FAIL("tail: a later command buffer read %.4f", e_tl);
        else
            OK("tail: a later command buffer reads MetalFX's output, RMSE %.4f", e_tl);
        struct run nt = tl; nt.no_tail = true; nt.context = 15;
        double e_nt = run_case(&s, &nt);
        printf("    (informational) the same without the tail: RMSE %.4f\n", e_nt);

        /* 6. Motion vectors at the output's size (macOS 27). */
        if (caps.mv_display) {
            struct scene sd = scene_new(MTLPixelFormatRGBA16Float, true);
            struct run md = mv; md.mv_display = true; md.context = 11;
            double e_md = run_case(&sd, &md);
            if (e_md < 0 || e_md > e_mv * 1.3 + 0.005)
                FAIL("motion vectors at the output's size: RMSE %.4f (input size %.4f)", e_md, e_mv);
            else
                OK("motion vectors at the output's size: RMSE %.4f (input size %.4f)", e_md, e_mv);
        }

        /* 7. A destroyed context's scaler lives until its work has run. */
        {
            struct run d = good; d.context = 12;
            limiter_reset();
            id<MTL4CommandBuffer> cb = begin_cb();
            struct mtl_metalfx_temporal_args a = args_for(&s, &d, IW, IH);
            const char *why = NULL;
            enum mtl_metalfx_temporal_status st = mtl_metalfx_temporal_encode(dev, comp, cb, &a, &why);
            sentinel_gone = 0;
            for (unsigned i = 0; i < MFXT_ENTRIES; i++)
                if (mfxt_entries[i].used && mfxt_entries[i].context == 12) {
                    MfxtSentinel *m = [MfxtSentinel new];
                    objc_setAssociatedObject(mfxt_entries[i].scaler, &sentinel_key, m, OBJC_ASSOCIATION_RETAIN);
                    [m release];
                }
            mtl_metalfx_temporal_destroy(12);
            bool gone_in_cache = true;
            for (unsigned i = 0; i < MFXT_ENTRIES; i++)
                if (mfxt_entries[i].used && mfxt_entries[i].context == 12) gone_in_cache = false;
            usleep(20000);
            bool alive_before = !sentinel_gone;
            commit_wait(cb);
            bool gone_after = false;
            for (int i = 0; i < 200 && !gone_after; i++) {
                gone_after = sentinel_gone;
                if (!gone_after) usleep(5000);
            }
            if (st != MTL_MFXT_OK || !gone_in_cache || !alive_before || !gone_after)
                FAIL("destroy: status %d, dropped from the cache %d, kept by the command buffer %d, released after it %d",
                     st, gone_in_cache, alive_before, gone_after);
            else
                OK("destroy: dropped from the cache, kept by its command buffer, released when that is gone");
        }

        /* 8. An output without shader-write usage: through the private copy. */
        {
            struct scene sn = scene_new(MTLPixelFormatRGBA16Float, false);
            sn.output = ktex(MTLPixelFormatRGBA16Float, OW, OH, MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
            struct run nw = good; nw.context = 14;
            double e_nw = run_case(&sn, &nw);
            bool via_scratch = false;
            for (unsigned i = 0; i < MFXT_ENTRIES; i++)
                if (mfxt_entries[i].used && mfxt_entries[i].context == 14) via_scratch = mfxt_entries[i].scratch;
            if (e_nw < 0 || e_nw > 0.04 || !via_scratch)
                FAIL("output without shader-write usage: RMSE %.4f, private copy %d", e_nw, via_scratch);
            else
                OK("output without shader-write usage: through the private copy, RMSE %.4f", e_nw);
        }

        /* 9. Refusals. */
        {
            struct run rr = good; rr.context = 13;
            struct { const char *name; enum mtl_metalfx_temporal_status want; struct mtl_metalfx_temporal_args a; } c[8];
            unsigned n = 0;
            struct mtl_metalfx_temporal_args a = args_for(&s, &rr, IW, IH);
            c[n].name = "638x358 -> 1920x1080 (more than 3x)"; c[n].want = MTL_MFXT_UNSUPPORTED; c[n].a = a;
            c[n].a.render_w = 638; c[n].a.render_h = 358; n++;
            c[n].name = "downscale 960x540 -> 640x360"; c[n].want = MTL_MFXT_UNSUPPORTED; c[n].a = a;
            c[n].a.out_w = 640; c[n].a.out_h = 360; n++;
            c[n].name = "no depth"; c[n].want = MTL_MFXT_BAD_ARGS; c[n].a = a; c[n].a.depth.texture = NULL; n++;
            c[n].name = "render size larger than the colour texture"; c[n].want = MTL_MFXT_BAD_ARGS; c[n].a = a;
            c[n].a.render_w = IW + 64; n++;
            id<MTLTexture> uint_tex = ktex(MTLPixelFormatRGBA8Uint, IW, IH, MTLTextureUsageShaderRead);
            c[n].name = "colour RGBA8Uint"; c[n].want = MTL_MFXT_UNSUPPORTED; c[n].a = a; c[n].a.color.texture = uint_tex; n++;
            id<MTLTexture> u8 = ktex(MTLPixelFormatRGBA8Unorm, IW, IH, MTLTextureUsageShaderRead);
            c[n].name = "an sRGB view of a texture without pixel-format-view usage"; c[n].want = MTL_MFXT_UNSUPPORTED;
            c[n].a = a; c[n].a.color.texture = u8; c[n].a.color.format = MTLPixelFormatRGBA8Unorm_sRGB; n++;
            id<MTLTexture> u8v = ktex(MTLPixelFormatRGBA8Unorm, IW, IH, MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView);
            c[n].name = "an sRGB view of a mutable texture (taken)"; c[n].want = MTL_MFXT_OK;
            c[n].a = a; c[n].a.color.texture = u8v; c[n].a.color.format = MTLPixelFormatRGBA8Unorm_sRGB; n++;
            c[n].name = "exposure not at its texture's origin"; c[n].want = MTL_MFXT_UNSUPPORTED; c[n].a = a;
            c[n].a.exposure.texture = s.exposure; c[n].a.exposure.x = 1; n++;
            for (unsigned i = 0; i < n; i++) {
                const char *why = NULL;
                enum mtl_metalfx_temporal_status st = refusal(c[i].a, &why);
                if (st != c[i].want)
                    FAIL("%s: status %d, want %d (%s)", c[i].name, st, c[i].want, why ? why : "");
                else
                    OK("%s: status %d%s%s", c[i].name, st, why ? ", " : "", why ? why : "");
            }
        }

        /* GPU time of one upscale (informational): 300 frames, three in
         * flight (Metal 4 commit feedback times), median of the last 280. */
        {
            limiter_reset();
            struct run b = good; b.context = 16;
            enum { N = 300, SKIP = 20 };
            double ms_store[N] = {0};
            double *ms = ms_store; /* written by the feedback handlers */
            id<MTL4CommandAllocator> al[3];
            id<MTL4CommandBuffer> cbs[3] = {nil};
            for (int k = 0; k < 3; k++) al[k] = [dev newCommandAllocator];
            uint64_t base = ev_value;
            for (int i = 0; i < N; i++) {
                int k = i % 3;
                if (i >= 3) {
                    [ev waitUntilSignaledValue:base + i - 2 timeoutMS:10000];
                    [cbs[k] release];
                    [al[k] reset];
                }
                id<MTL4CommandBuffer> cb = [dev newCommandBuffer];
                [cb beginCommandBufferWithAllocator:al[k]];
                struct mtl_metalfx_temporal_args a = args_for(&s, &b, IW, IH);
                a.jitter_x = halton(i % 32 + 1, 2) - 0.5f;
                a.jitter_y = halton(i % 32 + 1, 3) - 0.5f;
                mtl_metalfx_temporal_encode(dev, comp, cb, &a, NULL);
                [cb endCommandBuffer];
                MTL4CommitOptions *opt = [[MTL4CommitOptions new] autorelease];
                int idx = i;
                [opt addFeedbackHandler:^(id<MTL4CommitFeedback> fb) { ms[idx] = (fb.GPUEndTime - fb.GPUStartTime) * 1000; }];
                id<MTL4CommandBuffer> arr[1] = {cb};
                [q commit:arr count:1 options:opt];
                [q signalEvent:ev value:base + i + 1];
                cbs[k] = cb;
            }
            ev_value = base + N;
            [ev waitUntilSignaledValue:ev_value timeoutMS:10000];
            usleep(50000);
            for (int k = 0; k < 3; k++) { [cbs[k] release]; [al[k] release]; }
            double sorted[N - SKIP];
            memcpy(sorted, ms + SKIP, sizeof(sorted));
            int m = N - SKIP;
            for (int i = 0; i < m; i++)
                for (int j = i + 1; j < m; j++)
                    if (sorted[j] < sorted[i]) { double t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
            printf("    (informational) GPU time per upscale 960x540 -> 1920x1080 RGBA16F%s, 3 in flight: median %.3f ms (p10 %.3f, p90 %.3f)\n",
                   scratch ? " + private copy" : "", sorted[m / 2], sorted[m / 10], sorted[m * 9 / 10]);
        }

        /* 10. Scalers made again and again: the ninth within 10 s refused. */
        {
            limiter_reset();
            struct run rr = good;
            enum mtl_metalfx_temporal_status st = MTL_MFXT_OK;
            int made = 0;
            for (int i = 0; i < 9; i++) {
                rr.context = 100 + i;
                struct mtl_metalfx_temporal_args a = args_for(&s, &rr, IW, IH);
                const char *why = NULL;
                st = mtl_metalfx_temporal_prepare(dev, comp, &a, &why);
                if (st == MTL_MFXT_OK) made++;
            }
            if (made != 8 || st != MTL_MFXT_NO_SCALER)
                FAIL("scalers made again and again: %d made, the ninth status %d", made, st);
            else
                OK("scalers made again and again: 8 made, the ninth within 10 s refused (status %d)", st);
            for (int i = 0; i < 9; i++) mtl_metalfx_temporal_destroy(100 + i);
        }
    }
    printf(failures ? "== mfxt_bridge: FAIL (%d)\n" : "== mfxt_bridge: ok\n", failures);
    return failures != 0;
}
