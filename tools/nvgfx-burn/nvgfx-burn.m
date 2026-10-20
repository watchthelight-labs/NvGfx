// nvgfx-burn: a Metal compute load for watching GPU power, clocks and throttling.
// Each thread runs a long chain of fused multiply-adds on its float4 element of
// a 64 MB buffer; command buffers are kept in flight back to back for N seconds.
// Usage: nvgfx-burn [seconds] [--iters N]   (default 60 s, 4096 FMA steps per element)
// <wtl> fork: nvgfx | author: watchthelight </wtl>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <mach/mach_time.h>

static NSString *kSrc =
@"#include <metal_stdlib>\n"
"using namespace metal;\n"
"kernel void burn(device float4 *buf [[buffer(0)]], constant uint &iters [[buffer(1)]], uint id [[thread_position_in_grid]]) {\n"
"    float4 a = buf[id];\n"
"    float4 b = a * 0.5f + 0.25f;\n"
"    const float4 m = float4(0.9999f, 1.0001f, 0.99995f, 1.00005f);\n"
"    const float4 c = float4(1e-4f, -1e-4f, 2e-4f, -2e-4f);\n"
"    for (uint i = 0; i < iters; i++) {\n"
"        a = fma(a, m, c); b = fma(b, m, c);\n"
"        a = fma(a, m, c); b = fma(b, m, c);\n"
"        a = fma(a, m, c); b = fma(b, m, c);\n"
"        a = fma(a, m, c); b = fma(b, m, c);\n"
"    }\n"
"    buf[id] = a + b;\n"
"}\n";

static volatile sig_atomic_t gStop;
static void on_sig(int s) { (void)s; gStop = 1; }

static double now_s(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e9;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        double seconds = 60;
        uint32_t iters = 4096;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--iters") && i + 1 < argc) iters = (uint32_t)atoi(argv[++i]);
            else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
                fprintf(stderr, "usage: nvgfx-burn [seconds] [--iters N]\n"); return 0;
            } else seconds = atof(argv[i]);
        }
        if (seconds <= 0) seconds = 60;
        if (iters < 1) iters = 1;

        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "nvgfx-burn: no Metal device\n"); return 1; }
        printf("device: %s (registryID 0x%llx, lowPower %d)\n", dev.name.UTF8String, dev.registryID, dev.lowPower);

        NSError *err = nil;
        MTLCompileOptions *opt = [MTLCompileOptions new];
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSrc options:opt error:&err];
        if (!lib) { fprintf(stderr, "nvgfx-burn: compile failed: %s\n", err.localizedDescription.UTF8String); return 1; }
        id<MTLFunction> fn = [lib newFunctionWithName:@"burn"];
        id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
        if (!pso) { fprintf(stderr, "nvgfx-burn: pipeline failed: %s\n", err.localizedDescription.UTF8String); return 1; }

        const size_t bytes = 64u << 20;
        const NSUInteger n = bytes / sizeof(float) / 4;   // float4 elements
        id<MTLBuffer> buf = [dev newBufferWithLength:bytes options:MTLResourceStorageModePrivate];
        if (!buf) { fprintf(stderr, "nvgfx-burn: buffer alloc failed\n"); return 1; }
        {   // give the buffer defined contents
            id<MTLCommandQueue> q0 = [dev newCommandQueue];
            id<MTLCommandBuffer> cb = [q0 commandBuffer];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl fillBuffer:buf range:NSMakeRange(0, bytes) value:0x3c];
            [bl endEncoding]; [cb commit]; [cb waitUntilCompleted];
        }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        NSUInteger tg = pso.maxTotalThreadsPerThreadgroup; if (tg > 256) tg = 256;
        // 8 FMA ops per loop step per thread, each a float4 FMA = 4 lanes x 2 flops.
        const double flopsPerDispatch = (double)n * iters * 8.0 * 4.0 * 2.0;
        printf("buffer 64 MB (%lu float4), %u steps per element, %.1f GFLOP per dispatch, threadgroup %lu, %.0f s\n",
               (unsigned long)n, iters, flopsPerDispatch / 1e9, (unsigned long)tg, seconds);
        fflush(stdout);

        signal(SIGINT, on_sig); signal(SIGTERM, on_sig);
        dispatch_semaphore_t inflight = dispatch_semaphore_create(3);
        __block volatile long done = 0;
        __block volatile long errors = 0;
        const double t0 = now_s();
        double lastT = t0; long lastDone = 0, total = 0;
        while (!gStop) {
            double t = now_s();
            if (t - t0 >= seconds) break;
            dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
            id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
            [enc setComputePipelineState:pso];
            [enc setBuffer:buf offset:0 atIndex:0];
            [enc setBytes:&iters length:sizeof iters atIndex:1];
            [enc dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
            [enc endEncoding];
            [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
                if (c.status == MTLCommandBufferStatusError) __sync_fetch_and_add(&errors, 1);
                else __sync_fetch_and_add(&done, 1);
                dispatch_semaphore_signal(inflight);
            }];
            [cb commit];
            total++;
            t = now_s();
            if (t - lastT >= 1.0) {
                long d = done;
                printf("%6.1f s  %8.1f GFLOP/s  (%ld dispatches done, %ld errors)\n", t - t0,
                       (d - lastDone) * flopsPerDispatch / (t - lastT) / 1e9, d, (long)errors);
                fflush(stdout);
                lastT = t; lastDone = d;
            }
        }
        for (int i = 0; i < 3; i++) dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
        // libdispatch traps if a semaphore is freed below its initial value
        for (int i = 0; i < 3; i++) dispatch_semaphore_signal(inflight);
        double el = now_s() - t0;
        printf("done: %ld dispatches in %.1f s, average %.1f GFLOP/s, %ld errors\n", (long)done, el,
               done * flopsPerDispatch / el / 1e9, (long)errors);
        (void)total;
    }
    return 0;
}
