/*
 * Mental - Metal Backend (macOS)
 *
 * Runtime-loaded via dlopen — zero link-time framework dependencies.
 * On macOS the Metal.framework and QuartzCore.framework are loaded at
 * init time.  All Objective-C classes and selectors work via the
 * runtime once the frameworks are resident; no individual function
 * pointer resolution is needed.
 *
 * On non-Apple platforms this file compiles to a single NULL export.
 */

#if defined(__APPLE__) && !defined(MENTAL_DISABLE_METAL)

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>
#import <QuartzCore/CAMetalLayer.h>
#if TARGET_OS_OSX
#import <AppKit/AppKit.h>
#else
#import <UIKit/UIKit.h>
#endif
#include "mental_internal.h"
#include <string.h>
#include <stdlib.h>
#include <dlfcn.h>

/* Framework handles (dlopen'd at init, dlclose'd at shutdown) */
static void* g_metal_lib = NULL;
static void* g_quartzcore_lib = NULL;

/* Metal device wrapper */
typedef struct {
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
} MetalDevice;

/* Metal buffer wrapper */
typedef struct {
    id<MTLBuffer> buffer;
} MetalBuffer;

/* Metal kernel wrapper */
typedef struct {
    id<MTLComputePipelineState> pipeline;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue; /* reused from MetalDevice */
} MetalKernel;

/* Metal viewport wrapper */
typedef struct {
    CAMetalLayer* metalLayer;
    MetalBuffer* buffer;
    id<MTLDevice> device;
    id<MTLCommandQueue> queue;
} MetalViewport;

/* Global Metal devices */
static NSArray<id<MTLDevice>>* g_metal_devices = nil;

static int metal_init(void) {
    @autoreleasepool {
        /* Load Metal.framework at runtime */
        g_metal_lib = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_LAZY);
        if (!g_metal_lib) return -1;

        /* Load QuartzCore.framework for CAMetalLayer */
        g_quartzcore_lib = dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_LAZY);
        if (!g_quartzcore_lib) {
            dlclose(g_metal_lib);
            g_metal_lib = NULL;
            return -1;
        }

        /* Frameworks are loaded — Objective-C classes are now registered
         * with the runtime.  Normal ObjC message dispatch works. */
        g_metal_devices = MTLCopyAllDevices();
        if (!g_metal_devices || g_metal_devices.count == 0) {
            dlclose(g_quartzcore_lib);
            g_quartzcore_lib = NULL;
            dlclose(g_metal_lib);
            g_metal_lib = NULL;
            return -1;
        }
        return 0;
    }
}

static void metal_shutdown(void) {
    @autoreleasepool {
        g_metal_devices = nil;

        if (g_quartzcore_lib) {
            dlclose(g_quartzcore_lib);
            g_quartzcore_lib = NULL;
        }
        if (g_metal_lib) {
            dlclose(g_metal_lib);
            g_metal_lib = NULL;
        }
    }
}

static int metal_device_count(void) {
    return (int)g_metal_devices.count;
}

static int metal_device_info(int index, char* name, size_t name_len) {
    @autoreleasepool {
        if (index < 0 || index >= g_metal_devices.count) return -1;

        id<MTLDevice> device = g_metal_devices[index];
        const char* device_name = [device.name UTF8String];
        strncpy(name, device_name, name_len - 1);
        name[name_len - 1] = '\0';
        return 0;
    }
}

static void* metal_device_create(int index) {
    @autoreleasepool {
        if (index < 0 || index >= g_metal_devices.count) return NULL;

        MetalDevice* dev = malloc(sizeof(MetalDevice));
        if (!dev) return NULL;

        dev->device = g_metal_devices[index];
        dev->queue = [dev->device newCommandQueue];

        return dev;
    }
}

static void metal_device_destroy(void* dev) {
    @autoreleasepool {
        if (!dev) return;

        MetalDevice* metal_dev = (MetalDevice*)dev;
        /* ARC handles cleanup */
        free(metal_dev);
    }
}

static void* metal_buffer_alloc(void* dev, size_t bytes) {
    @autoreleasepool {
        MetalDevice* metal_dev = (MetalDevice*)dev;

        id<MTLBuffer> buffer = [metal_dev->device newBufferWithLength:bytes
                                                              options:MTLResourceStorageModeShared];
        if (!buffer) return NULL;

        MetalBuffer* buf = malloc(sizeof(MetalBuffer));
        if (!buf) {
            return NULL;
        }

        buf->buffer = buffer;
        return buf;
    }
}

static void metal_buffer_write(void* buf, const void* data, size_t bytes) {
    @autoreleasepool {
        MetalBuffer* metal_buf = (MetalBuffer*)buf;
        memcpy(metal_buf->buffer.contents, data, bytes);
    }
}

static void metal_buffer_read(void* buf, void* data, size_t bytes) {
    @autoreleasepool {
        MetalBuffer* metal_buf = (MetalBuffer*)buf;
        memcpy(data, metal_buf->buffer.contents, bytes);
    }
}

static void* metal_buffer_resize(void* dev, void* old_buf, size_t old_size, size_t new_size) {
    @autoreleasepool {
        MetalDevice* metal_dev = (MetalDevice*)dev;
        MetalBuffer* old_metal_buf = (MetalBuffer*)old_buf;

        /* Allocate new buffer */
        id<MTLBuffer> new_buffer = [metal_dev->device newBufferWithLength:new_size
                                                                  options:MTLResourceStorageModeShared];
        if (!new_buffer) return NULL;

        /* Copy old data */
        size_t copy_size = old_size < new_size ? old_size : new_size;
        memcpy(new_buffer.contents, old_metal_buf->buffer.contents, copy_size);

        /* Update buffer (ARC handles old buffer cleanup) */
        old_metal_buf->buffer = new_buffer;

        return old_buf;
    }
}

static void* metal_buffer_clone(void* dev, void* src_buf, size_t size) {
    @autoreleasepool {
        MetalDevice* metal_dev = (MetalDevice*)dev;
        MetalBuffer* src_metal_buf = (MetalBuffer*)src_buf;

        /* Allocate new buffer */
        MetalBuffer* clone_buf = (MetalBuffer*)malloc(sizeof(MetalBuffer));
        if (!clone_buf) return NULL;

        id<MTLBuffer> new_buffer = [metal_dev->device newBufferWithLength:size
                                                                  options:MTLResourceStorageModeShared];
        if (!new_buffer) {
            free(clone_buf);
            return NULL;
        }

        /* Copy data from source buffer */
        memcpy(new_buffer.contents, src_metal_buf->buffer.contents, size);

        clone_buf->buffer = new_buffer;
        return clone_buf;
    }
}

static void metal_buffer_destroy(void* buf) {
    @autoreleasepool {
        if (!buf) return;

        MetalBuffer* metal_buf = (MetalBuffer*)buf;
        /* ARC handles buffer cleanup */
        free(metal_buf);
    }
}

static void* metal_kernel_compile(void* dev, const char* source, size_t source_len,
                                   char* error, size_t error_len) {
    @autoreleasepool {
        MetalDevice* metal_dev = (MetalDevice*)dev;

        /* Create library from source */
        NSString* source_str = [[NSString alloc] initWithBytes:source
                                                         length:source_len
                                                       encoding:NSUTF8StringEncoding];
        NSError* ns_error = nil;
        id<MTLLibrary> library = [metal_dev->device newLibraryWithSource:source_str
                                                                  options:nil
                                                                    error:&ns_error];

        if (!library) {
            if (error && ns_error) {
                const char* error_str = [[ns_error localizedDescription] UTF8String];
                strncpy(error, error_str, error_len - 1);
                error[error_len - 1] = '\0';
            }
            return NULL;
        }

        /* Get compute function - try common names (SPIRV-Cross uses main0), then first function */
        id<MTLFunction> function = [library newFunctionWithName:@"main0"];
        if (!function) {
            function = [library newFunctionWithName:@"compute_main"];
        }
        if (!function) {
            /* Get the first function */
            NSArray* function_names = [library functionNames];
            if (function_names.count > 0) {
                function = [library newFunctionWithName:function_names[0]];
            }
        }

        if (!function) {
            if (error) {
                strncpy(error, "No compute function found in shader", error_len - 1);
                error[error_len - 1] = '\0';
            }
            return NULL;
        }

        /* Create pipeline state */
        id<MTLComputePipelineState> pipeline = [metal_dev->device
            newComputePipelineStateWithFunction:function
                                           error:&ns_error];

        if (!pipeline) {
            if (error && ns_error) {
                const char* error_str = [[ns_error localizedDescription] UTF8String];
                strncpy(error, error_str, error_len - 1);
                error[error_len - 1] = '\0';
            }
            return NULL;
        }

        MetalKernel* kernel = malloc(sizeof(MetalKernel));
        if (!kernel) {
            return NULL;
        }

        kernel->pipeline = pipeline;
        kernel->device = metal_dev->device;
        kernel->queue = metal_dev->queue;

        return kernel;
    }
}

static void metal_kernel_dispatch(void* kernel, void** inputs, int input_count,
                                   void** outputs, int output_count, int work_size) {
    @autoreleasepool {
        MetalKernel* metal_kernel = (MetalKernel*)kernel;

        /* Create command buffer from the reusable queue */
        id<MTLCommandBuffer> commandBuffer = [metal_kernel->queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];

        [encoder setComputePipelineState:metal_kernel->pipeline];

        /* Bind input buffers */
        for (int i = 0; i < input_count; i++) {
            if (inputs[i]) {
                MetalBuffer* input_buf = (MetalBuffer*)inputs[i];
                [encoder setBuffer:input_buf->buffer offset:0 atIndex:i];
            }
        }

        /* Bind output buffers */
        for (int i = 0; i < output_count; i++) {
            if (outputs[i]) {
                MetalBuffer* output_buf = (MetalBuffer*)outputs[i];
                [encoder setBuffer:output_buf->buffer offset:0 atIndex:input_count + i];
            }
        }

        /* Dispatch */
        MTLSize gridSize = MTLSizeMake(work_size, 1, 1);
        NSUInteger threadGroupSize = metal_kernel->pipeline.maxTotalThreadsPerThreadgroup;
        if (threadGroupSize > work_size) threadGroupSize = work_size;
        MTLSize threadgroupSize = MTLSizeMake(threadGroupSize, 1, 1);

        [encoder dispatchThreads:gridSize threadsPerThreadgroup:threadgroupSize];
        [encoder endEncoding];

        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
    }
}

static int metal_kernel_workgroup_size(void* kernel) {
    if (!kernel) return 256;
    MetalKernel* mk = (MetalKernel*)kernel;
    return (int)mk->pipeline.maxTotalThreadsPerThreadgroup;
}

static void metal_kernel_destroy(void* kernel) {
    @autoreleasepool {
        if (!kernel) return;

        MetalKernel* metal_kernel = (MetalKernel*)kernel;
        /* ARC handles cleanup */
        free(metal_kernel);
    }
}

/* Viewport operations */
static void* metal_viewport_attach(void* dev, void* buffer, void* surface, char* error, size_t error_len) {
    @autoreleasepool {
        MetalDevice* metal_dev = (MetalDevice*)dev;
        MetalBuffer* metal_buf = (MetalBuffer*)buffer;
        id obj = (__bridge id)surface;

        CAMetalLayer* metalLayer = nil;

        /* Handle different surface types via introspection */
        if ([obj isKindOfClass:[CAMetalLayer class]]) {
            /* Already a CAMetalLayer - use directly */
            metalLayer = (CAMetalLayer*)obj;
        }
#if TARGET_OS_OSX
        else if ([obj isKindOfClass:[NSView class]]) {
            /* NSView - set up layer */
            NSView* view = (NSView*)obj;
            metalLayer = [CAMetalLayer layer];
            metalLayer.device = metal_dev->device;
            metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            metalLayer.framebufferOnly = NO;
            view.layer = metalLayer;
            view.wantsLayer = YES;
        }
#endif
#if TARGET_OS_IPHONE
        else if ([obj isKindOfClass:[UIView class]]) {
            /* UIView - set up layer */
            UIView* view = (UIView*)obj;
            metalLayer = [CAMetalLayer layer];
            metalLayer.device = metal_dev->device;
            metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            metalLayer.framebufferOnly = NO;
            view.layer = metalLayer;
        }
#endif
        else if ([obj isKindOfClass:[CALayer class]]) {
            /* CALayer - add as sublayer */
            CALayer* layer = (CALayer*)obj;
            metalLayer = [CAMetalLayer layer];
            metalLayer.device = metal_dev->device;
            metalLayer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            metalLayer.framebufferOnly = NO;
            metalLayer.frame = layer.bounds;
            [layer addSublayer:metalLayer];
        }
        else {
            if (error) {
                snprintf(error, error_len, "Unsupported surface type");
            }
            return NULL;
        }

        /* Create viewport */
        MetalViewport* viewport = malloc(sizeof(MetalViewport));
        if (!viewport) {
            if (error) {
                snprintf(error, error_len, "Failed to allocate viewport");
            }
            return NULL;
        }

        viewport->metalLayer = metalLayer;
        viewport->buffer = metal_buf;
        viewport->device = metal_dev->device;
        viewport->queue = metal_dev->queue;

        return viewport;
    }
}

static void metal_viewport_present(void* viewport_ptr) {
    @autoreleasepool {
        if (!viewport_ptr) return;

        MetalViewport* viewport = (MetalViewport*)viewport_ptr;

        /* Get next drawable */
        id<CAMetalDrawable> drawable = [viewport->metalLayer nextDrawable];
        if (!drawable) return;

        /* Copy buffer contents to drawable texture */
        id<MTLCommandBuffer> commandBuffer = [viewport->queue commandBuffer];
        id<MTLBlitCommandEncoder> blitEncoder = [commandBuffer blitCommandEncoder];

        /* Calculate dimensions */
        NSUInteger width = viewport->metalLayer.drawableSize.width;
        NSUInteger height = viewport->metalLayer.drawableSize.height;
        NSUInteger bytesPerRow = width * 4; /* BGRA8 = 4 bytes per pixel */

        /* Copy from buffer to texture */
        [blitEncoder copyFromBuffer:viewport->buffer->buffer
                       sourceOffset:0
                  sourceBytesPerRow:bytesPerRow
                sourceBytesPerImage:bytesPerRow * height
                         sourceSize:MTLSizeMake(width, height, 1)
                          toTexture:drawable.texture
                   destinationSlice:0
                   destinationLevel:0
                  destinationOrigin:MTLOriginMake(0, 0, 0)];

        [blitEncoder endEncoding];

        /* Present drawable */
        [commandBuffer presentDrawable:drawable];
        [commandBuffer commit];
    }
}

static void metal_viewport_detach(void* viewport_ptr) {
    @autoreleasepool {
        if (!viewport_ptr) return;

        MetalViewport* viewport = (MetalViewport*)viewport_ptr;
        /* ARC handles cleanup */
        free(viewport);
    }
}

/* Backend implementation */
/* -- Pipe --------------------------------------------------------- */

typedef struct {
    id<MTLCommandQueue> queue;
    id<MTLCommandBuffer> commandBuffer;
} MetalPipe;

static void* metal_pipe_create(void* dev) {
    @autoreleasepool {
        MetalDevice* metal_dev = (MetalDevice*)dev;
        MetalPipe* pipe = malloc(sizeof(MetalPipe));
        if (!pipe) return NULL;
        pipe->queue = metal_dev->queue;
        pipe->commandBuffer = [metal_dev->queue commandBuffer];
        return pipe;
    }
}

static int metal_pipe_add(void* pipe_ptr, void* kernel, void** inputs,
                            int input_count, void** outputs, int output_count,
                            int work_size) {
    @autoreleasepool {
        MetalPipe* pipe = (MetalPipe*)pipe_ptr;
        MetalKernel* mk = (MetalKernel*)kernel;

        id<MTLComputeCommandEncoder> encoder = [pipe->commandBuffer computeCommandEncoder];
        [encoder setComputePipelineState:mk->pipeline];

        for (int i = 0; i < input_count; i++) {
            if (inputs[i]) {
                MetalBuffer* in_buf = (MetalBuffer*)inputs[i];
                [encoder setBuffer:in_buf->buffer offset:0 atIndex:i];
            }
        }
        for (int i = 0; i < output_count; i++) {
            if (outputs[i]) {
                MetalBuffer* out_buf = (MetalBuffer*)outputs[i];
                [encoder setBuffer:out_buf->buffer offset:0 atIndex:input_count + i];
            }
        }

        MTLSize gridSize = MTLSizeMake(work_size, 1, 1);
        NSUInteger threadGroupSize = mk->pipeline.maxTotalThreadsPerThreadgroup;
        if (threadGroupSize > (NSUInteger)work_size) threadGroupSize = work_size;
        MTLSize tgSize = MTLSizeMake(threadGroupSize, 1, 1);

        [encoder dispatchThreads:gridSize threadsPerThreadgroup:tgSize];
        [encoder endEncoding];

        return 0;
    }
}

static int metal_pipe_execute(void* pipe_ptr) {
    @autoreleasepool {
        MetalPipe* pipe = (MetalPipe*)pipe_ptr;
        [pipe->commandBuffer commit];
        [pipe->commandBuffer waitUntilCompleted];
        return 0;
    }
}

static void metal_pipe_destroy(void* pipe_ptr) {
    if (pipe_ptr) free(pipe_ptr);
}

static mental_backend g_metal_backend = {
    .name = "Metal",
    .api = MENTAL_API_METAL,
    .init = metal_init,
    .shutdown = metal_shutdown,
    .device_count = metal_device_count,
    .device_info = metal_device_info,
    .device_create = metal_device_create,
    .device_destroy = metal_device_destroy,
    .buffer_alloc = metal_buffer_alloc,
    .buffer_write = metal_buffer_write,
    .buffer_read = metal_buffer_read,
    .buffer_resize = metal_buffer_resize,
    .buffer_clone = metal_buffer_clone,
    .buffer_destroy = metal_buffer_destroy,
    .kernel_compile = metal_kernel_compile,
    .kernel_workgroup_size = metal_kernel_workgroup_size,
    .kernel_dispatch = metal_kernel_dispatch,
    .kernel_destroy = metal_kernel_destroy,
    .pipe_create = metal_pipe_create,
    .pipe_add = metal_pipe_add,
    .pipe_execute = metal_pipe_execute,
    .pipe_destroy = metal_pipe_destroy,
    .viewport_attach = metal_viewport_attach,
    .viewport_present = metal_viewport_present,
    .viewport_detach = metal_viewport_detach,
    .viewport_readback = NULL
};

mental_backend* metal_backend = &g_metal_backend;

#else
/* Not Apple or force-disabled — Metal is not available */
#include "mental_internal.h"
mental_backend* metal_backend = NULL;
#endif /* __APPLE__ && !MENTAL_DISABLE_METAL */
