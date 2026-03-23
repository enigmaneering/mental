/*
 * Mental - Universal GPU Compute Library
 *
 * A minimal C library for unified GPU compute across all platforms.
 * Write shaders in any language (GLSL/HLSL/MSL/WGSL), run on any GPU.
 *
 * Automatic transpilation via SPIRV intermediate representation.
 * Automatic backend selection: Metal/D3D12/Vulkan/OpenCL with fallback chain.
 * Thread-safe: all operations internally lock/unlock.
 *
 * Copyright (c) 2025 Enigmaneering
 * Licensed under Apache 2.0
 */

#ifndef MENTAL_H
#define MENTAL_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Opaque handles */
typedef struct mental_device_t* mental_device;
typedef struct mental_reference_t* mental_reference;
typedef struct mental_kernel_t* mental_kernel;

/* Device API types */
typedef enum {
    MENTAL_API_METAL,
    MENTAL_API_D3D12,
    MENTAL_API_VULKAN,
    MENTAL_API_OPENCL
} mental_api_type;

/* Error codes */
typedef enum {
    MENTAL_SUCCESS = 0,
    MENTAL_ERROR_NO_DEVICES = -1,
    MENTAL_ERROR_INVALID_DEVICE = -2,
    MENTAL_ERROR_ALLOCATION_FAILED = -3,
    MENTAL_ERROR_COMPILATION_FAILED = -4,
    MENTAL_ERROR_INVALID_REFERENCE = -5,
    MENTAL_ERROR_INVALID_KERNEL = -6,
    MENTAL_ERROR_DISPATCH_FAILED = -7,
    MENTAL_ERROR_BACKEND_FAILED = -8
} mental_error;

/*
 * Device Management
 */

/* Get number of available GPU devices */
int mental_device_count(void);

/* Get device by index (0-based) */
mental_device mental_device_get(int index);

/* Get device name (e.g., "Apple M1", "NVIDIA RTX 4090") */
const char* mental_device_name(mental_device dev);

/* Get device API type (Metal, D3D12, Vulkan, OpenCL) */
mental_api_type mental_device_api(mental_device dev);

/* Get device API name as string */
const char* mental_device_api_name(mental_device dev);

/*
 * Reference (GPU Memory Buffer)
 *
 * All operations are thread-safe (internally locked).
 * write() automatically resizes if needed.
 */

/* Allocate GPU memory buffer of specified size in bytes */
mental_reference mental_alloc(mental_device dev, size_t bytes);

/* Write data to GPU buffer (auto-resizes if data is larger than current size) */
void mental_write(mental_reference ref, const void* data, size_t bytes);

/* Read data from GPU buffer into host memory */
void mental_read(mental_reference ref, void* data, size_t bytes);

/* Get current size of GPU buffer in bytes */
size_t mental_size(mental_reference ref);

/* Clone GPU buffer (creates new buffer with copy of data, thread-safe) */
mental_reference mental_clone(mental_reference ref);

/* Free GPU memory (must be called explicitly) */
void mental_finalize(mental_reference ref);

/*
 * Kernel (Compute Shader)
 *
 * Automatic language detection and transpilation.
 * Supports: GLSL, HLSL, MSL, WGSL source code.
 */

/* Compile shader source for device (auto-detects language and transpiles) */
mental_kernel mental_compile(mental_device dev, const char* source, size_t source_len);

/* Execute kernel with input/output buffers */
void mental_dispatch(mental_kernel kernel, mental_reference* inputs, int input_count,
                     mental_reference output, int work_size);

/* Free compiled kernel (must be called explicitly) */
void mental_kernel_finalize(mental_kernel kernel);

/*
 * Viewport (Surface Presentation)
 *
 * Attach a reference to an OS surface for zero-copy presentation.
 * User creates window/surface, Mental handles GPU->display connection.
 *
 * Supported surface types:
 *   - macOS/iOS: NSView*, UIView*, CALayer*, CAMetalLayer*
 *   - Windows: HWND
 *   - Linux: VkSurfaceKHR or native window handle
 *
 * Falls back to CPU roundtrip if native presentation unavailable (OpenCL).
 */

/* Opaque handle */
typedef struct mental_viewport_t* mental_viewport;

/* Attach reference to OS surface for presentation */
mental_viewport mental_viewport_attach(mental_reference ref, void* surface);

/* Present reference contents to attached surface */
void mental_viewport_present(mental_viewport viewport);

/* Detach and cleanup viewport */
void mental_viewport_detach(mental_viewport viewport);

/*
 * Error Handling
 */

/* Get last error code (thread-local) */
mental_error mental_get_error(void);

/* Get last error message (thread-local) */
const char* mental_get_error_message(void);

/*
 * Lifecycle Management
 *
 * Register callbacks for automatic cleanup at process exit.
 * Callbacks run in LIFO order (last registered = first called).
 */

/* Register a callback to run at process exit (LIFO order). */
void mental_atexit(void (*fn)(void));

#ifdef __cplusplus
}
#endif

#endif /* MENTAL_H */
