// Native Metal 4 constructor lifetime probe, no VM, Vulkan or game.
// Build with manual reference counting, as KosmicKrisp's bridge uses:
//   clang -O2 -fno-objc-arc -Wno-unguarded-availability-new \
//     -framework Foundation -framework Metal \
//     tests/kk/render_pass_lifetime.m -o /tmp/render_pass_lifetime
// Run 0 for patch 23's constructor, 1 for the old double initialization:
//   /tmp/render_pass_lifetime 0 500000
// The pools and +1 release mirror mtl_new_render_pass_descriptor/mtl_release.
// Numbers are allocation/RSS deltas, not game FPS or a whole-driver leak test.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <malloc/malloc.h>
#include <stdio.h>
#include <stdlib.h>

static id create_descriptor(int twice)
{
    @autoreleasepool {
        return twice ? [[MTL4RenderPassDescriptor new] init] : [MTL4RenderPassDescriptor new];
    }
}

static void release_descriptor(id descriptor)
{
    @autoreleasepool {
        [descriptor release];
    }
}

static int cycle(int twice)
{
    id descriptor = create_descriptor(twice);
    int ok;
    @autoreleasepool {
        MTL4RenderPassDescriptor *d = descriptor;
        d.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
        ok = d.colorAttachments[0].clearColor.green == 1;
    }
    release_descriptor(descriptor);
    return ok;
}

static void usage(unsigned long *heap, unsigned long *rss)
{
    malloc_statistics_t statistics = {0};
    malloc_zone_statistics(NULL, &statistics);
    *heap = statistics.size_in_use;
    mach_task_basic_info_data_t info = {0};
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count);
    *rss = info.resident_size;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int twice = argc > 1 ? atoi(argv[1]) : 0;
    int iterations = argc > 2 ? atoi(argv[2]) : 500000;
    if ((twice != 0 && twice != 1) || iterations < 1) return 2;
    for (int i = 0; i < 10000; i++) if (!cycle(twice)) return 3;
    unsigned long heap0, rss0, heap, rss;
    usage(&heap0, &rss0);
    printf("constructor=%s warmup=10000 heap=%lu rss=%lu\n", twice ? "new+init" : "new", heap0, rss0);
    for (int i = 0; i < iterations; i++) if (!cycle(twice)) return 3;
    usage(&heap, &rss);
    printf("iterations=%d heap_delta=%ld rss_delta=%ld\n", iterations, (long)(heap-heap0), (long)(rss-rss0));
    // Warm-up absorbs lazy Metal/Foundation allocations. The fixed constructor
    // must not retain memory in proportion to the number of descriptors made.
    if (!twice && heap > heap0 + 1024 * 1024) return 1;
    printf("== render_pass_lifetime: ok\n");
    return 0;
}
