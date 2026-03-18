//go:build darwin

#import <Metal/Metal.h>
#import <Foundation/Foundation.h>

// Enumerate all Metal devices and return count
int metal_enumerate_devices(void ***devices_out) {
    @autoreleasepool {
        NSArray<id<MTLDevice>> *devices = MTLCopyAllDevices();
        if (devices == nil || devices.count == 0) {
            return 0;
        }

        int count = (int)devices.count;
        void **device_array = (void **)malloc(count * sizeof(void *));

        for (int i = 0; i < count; i++) {
            device_array[i] = (void *)CFBridgingRetain(devices[i]);
        }

        *devices_out = device_array;
        return count;
    }
}

// Get device name (caller must free the returned string)
const char* metal_device_name(void *device) {
    @autoreleasepool {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
        const char *name = [mtlDevice.name UTF8String];
        return strdup(name); // Caller must free
    }
}

// Check if device is low power (integrated)
int metal_device_is_low_power(void *device) {
    @autoreleasepool {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
        return mtlDevice.lowPower ? 1 : 0;
    }
}

// Check if device is headless
int metal_device_is_headless(void *device) {
    @autoreleasepool {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
        return mtlDevice.headless ? 1 : 0;
    }
}

// Release device reference
void metal_device_release(void *device) {
    if (device != NULL) {
        CFRelease(device);
    }
}

// Create command queue
void* metal_create_command_queue(void *device) {
    @autoreleasepool {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
        id<MTLCommandQueue> queue = [mtlDevice newCommandQueue];
        return (void *)CFBridgingRetain(queue);
    }
}

// Release command queue
void metal_release_command_queue(void *queue) {
    if (queue != NULL) {
        CFRelease(queue);
    }
}

// Allocate buffer
void* metal_alloc_buffer(void *device, size_t size) {
    @autoreleasepool {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
        id<MTLBuffer> buffer = [mtlDevice newBufferWithLength:size
                                                      options:MTLResourceStorageModeShared];
        return (void *)CFBridgingRetain(buffer);
    }
}

// Release buffer
void metal_release_buffer(void *buffer) {
    if (buffer != NULL) {
        CFRelease(buffer);
    }
}

// Get buffer contents pointer
void* metal_buffer_contents(void *buffer) {
    @autoreleasepool {
        id<MTLBuffer> mtlBuffer = (__bridge id<MTLBuffer>)buffer;
        return [mtlBuffer contents];
    }
}

// Get buffer length
size_t metal_buffer_length(void *buffer) {
    @autoreleasepool {
        id<MTLBuffer> mtlBuffer = (__bridge id<MTLBuffer>)buffer;
        return [mtlBuffer length];
    }
}

// Compile Metal shader from source
void* metal_compile_shader(void *device, const char *source, char **error_out) {
    @autoreleasepool {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
        NSString *sourceString = [NSString stringWithUTF8String:source];

        NSError *error = nil;
        id<MTLLibrary> library = [mtlDevice newLibraryWithSource:sourceString
                                                         options:nil
                                                           error:&error];

        if (error != nil) {
            if (error_out != NULL) {
                const char *errorMsg = [[error localizedDescription] UTF8String];
                *error_out = strdup(errorMsg);
            }
            return NULL;
        }

        // Get the first function (kernel) from the library
        NSArray<NSString *> *functionNames = [library functionNames];
        if (functionNames.count == 0) {
            if (error_out != NULL) {
                *error_out = strdup("No kernel functions found in shader");
            }
            return NULL;
        }

        NSString *functionName = functionNames[0];
        id<MTLFunction> function = [library newFunctionWithName:functionName];

        if (function == nil) {
            if (error_out != NULL) {
                *error_out = strdup("Failed to create function from library");
            }
            return NULL;
        }

        return (void *)CFBridgingRetain(function);
    }
}

// Release shader function
void metal_release_shader(void *function) {
    if (function != NULL) {
        CFRelease(function);
    }
}

// Create compute pipeline state
void* metal_create_pipeline_state(void *device, void *function, char **error_out) {
    @autoreleasepool {
        id<MTLDevice> mtlDevice = (__bridge id<MTLDevice>)device;
        id<MTLFunction> mtlFunction = (__bridge id<MTLFunction>)function;

        NSError *error = nil;
        id<MTLComputePipelineState> pipelineState =
            [mtlDevice newComputePipelineStateWithFunction:mtlFunction error:&error];

        if (error != nil) {
            if (error_out != NULL) {
                const char *errorMsg = [[error localizedDescription] UTF8String];
                *error_out = strdup(errorMsg);
            }
            return NULL;
        }

        return (void *)CFBridgingRetain(pipelineState);
    }
}

// Release pipeline state
void metal_release_pipeline_state(void *pipelineState) {
    if (pipelineState != NULL) {
        CFRelease(pipelineState);
    }
}

// Dispatch compute kernel
int metal_dispatch_compute(void *queue, void *pipelineState, void **buffers, int bufferCount, int workSize, char **error_out) {
    @autoreleasepool {
        id<MTLCommandQueue> commandQueue = (__bridge id<MTLCommandQueue>)queue;
        id<MTLComputePipelineState> pipeline = (__bridge id<MTLComputePipelineState>)pipelineState;

        id<MTLCommandBuffer> commandBuffer = [commandQueue commandBuffer];
        if (commandBuffer == nil) {
            if (error_out != NULL) {
                *error_out = strdup("Failed to create command buffer");
            }
            return 0;
        }

        id<MTLComputeCommandEncoder> encoder = [commandBuffer computeCommandEncoder];
        if (encoder == nil) {
            if (error_out != NULL) {
                *error_out = strdup("Failed to create compute encoder");
            }
            return 0;
        }

        [encoder setComputePipelineState:pipeline];

        // Set buffers
        for (int i = 0; i < bufferCount; i++) {
            id<MTLBuffer> buffer = (__bridge id<MTLBuffer>)buffers[i];
            [encoder setBuffer:buffer offset:0 atIndex:i];
        }

        // Calculate threadgroup sizes
        NSUInteger threadExecutionWidth = pipeline.threadExecutionWidth;
        NSUInteger maxThreadsPerThreadgroup = pipeline.maxTotalThreadsPerThreadgroup;

        MTLSize threadsPerThreadgroup = MTLSizeMake(
            MIN(threadExecutionWidth, maxThreadsPerThreadgroup),
            1,
            1
        );

        MTLSize gridSize = MTLSizeMake(workSize, 1, 1);

        [encoder dispatchThreads:gridSize
           threadsPerThreadgroup:threadsPerThreadgroup];

        [encoder endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];

        if (commandBuffer.error != nil) {
            if (error_out != NULL) {
                const char *errorMsg = [[commandBuffer.error localizedDescription] UTF8String];
                *error_out = strdup(errorMsg);
            }
            return 0;
        }

        return 1;
    }
}
