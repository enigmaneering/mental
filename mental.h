/*
 * Mental - Universal GPU Compute Library
 *
 * A minimal C library for unified GPU compute across all platforms.
 * Write shaders in any language (GLSL/HLSL/MSL/WGSL), run on any GPU.
 *
 * Automatic transpilation via SPIRV intermediate representation.
 * Automatic backend selection with platform-aware fallback chain:
 *   Darwin:  Metal → WebGPU → OpenCL → PoCL
 *   Windows: D3D12 → Vulkan → WebGPU → OpenCL → OpenGL → PoCL
 *   Linux:   Vulkan → WebGPU → OpenCL → OpenGL → PoCL
 *
 * Thread-safe: all operations internally lock/unlock.
 *
 * Higher-level orchestration (manifests, spark links, thoughts,
 * epiphanies) lives in the Go layer — not here.  This library
 * provides the GPU primitives that any language can consume.
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
typedef struct mental_disclosure_t* mental_disclosure;
typedef struct mental_kernel_t* mental_kernel;

/* Device API types */
typedef enum {
    MENTAL_API_NONE = -1,
    MENTAL_API_METAL,
    MENTAL_API_D3D12,
    MENTAL_API_VULKAN,
    MENTAL_API_OPENCL,
    MENTAL_API_OPENGL,
    MENTAL_API_POCL,
    MENTAL_API_WEBGPU,
    MENTAL_API_D3D11
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

/* Disclosure modes — control access to a reference.
 *
 *   OPEN:      Read and write access granted without credentials.
 *   INCLUSIVE:  Read access always granted (mental_reference_data succeeds).
 *              Write access requires credentials (mental_reference_writable
 *              checks).  Note: mental_reference_data returns a writable
 *              pointer — the read/write distinction is cooperative.
 *   EXCLUSIVE: All access requires matching credentials. */
typedef enum {
    MENTAL_RELATIONALLY_OPEN      = 0,
    MENTAL_RELATIONALLY_INCLUSIVE  = 1,
    MENTAL_RELATIONALLY_EXCLUSIVE = 2
} mental_relationship;

/* Credential provider callback */
typedef void (*mental_credential_fn)(void *ctx,
                                      void *buf, size_t buf_size,
                                      size_t *out_len);

/*
 * Device Management
 */

int mental_device_count(void);
mental_device mental_device_get(int index);
const char* mental_device_name(mental_device dev);
mental_api_type mental_device_api(mental_device dev);
const char* mental_device_api_name(mental_device dev);

/*
 * Reference (Process-Local Data with GPU Pinning)
 */

mental_reference mental_reference_create(size_t size,
                                          mental_relationship mode,
                                          const void *credential,
                                          size_t credential_len,
                                          mental_disclosure *out_disclosure);

/* Get a pointer to the process-local data buffer.
 * Pass NULL/0 for credential when disclosure is open or inclusive (read).
 * Returns NULL if the disclosure denies access.
 *
 * IMPORTANT: The returned pointer provides direct memory access.
 * For INCLUSIVE mode, this grants read access — but writes through the
 * pointer bypass mental_reference_writable() checks.  This is a
 * cooperative trust boundary: C callers are trusted not to write
 * through a read-only pointer.  Use mental_reference_write() for
 * enforced write access. */
void* mental_reference_data(mental_reference ref,
                             const void *credential, size_t credential_len);
size_t mental_reference_size(mental_reference ref);
int mental_reference_writable(mental_reference ref,
                               const void *credential, size_t credential_len);
mental_relationship mental_reference_get_disclosure(mental_reference ref);
mental_reference mental_reference_clone(mental_reference ref,
                                         mental_device device,
                                         const void *credential,
                                         size_t credential_len);
void mental_reference_close(mental_reference ref);

/*
 * Disclosure Handle
 */

void mental_disclosure_set_mode(mental_disclosure dh, mental_relationship mode);
void mental_disclosure_set_credential(mental_disclosure dh,
                                        const void *credential, size_t len);
void mental_disclosure_set_credential_provider(mental_disclosure dh,
                                                 mental_credential_fn fn, void *ctx);
/* Free the disclosure handle.  Does NOT revoke access or close the
 * reference — the reference keeps whatever mode and credential were
 * last set.  This freezes the disclosure rules permanently: no
 * further changes are possible without a disclosure handle. */
void mental_disclosure_close(mental_disclosure dh);

/* NOTE: Do not repin a reference while a viewport is actively
 * presenting from it.  The viewport may read stale buffer state
 * during the transition.  Detach the viewport first, repin, then
 * reattach. */

/*
 * GPU Pinning
 */

int mental_reference_pin(mental_reference ref, mental_device device);
/* Write data to the reference.  Returns bytes actually written
 * (clamped to the reference's capacity).  Writes to both local
 * memory and GPU buffer if pinned. */
size_t mental_reference_write(mental_reference ref,
                               const void *data, size_t bytes);

/* Read data from the reference.  Returns bytes actually read
 * (clamped to the reference's capacity).  Reads from GPU buffer
 * if pinned, otherwise from local memory. */
size_t mental_reference_read(mental_reference ref,
                              void *data, size_t bytes);
int mental_reference_is_pinned(mental_reference ref);
mental_device mental_reference_device(mental_reference ref);

/*
 * Kernel (Compute Shader)
 */

mental_kernel mental_compile(mental_device dev, const char* source, size_t source_len);
int mental_dispatch(mental_kernel kernel, mental_reference* inputs, int input_count,
                    mental_reference output, int work_size);
void mental_kernel_finalize(mental_kernel kernel);

/*
 * Pipe (Chained Kernel Dispatch)
 *
 * A pipe records multiple kernel dispatches into a single GPU command
 * buffer and submits them together.  Data stays on the GPU between
 * stages — no CPU round-trips.
 *
 * A pipe is one-shot: after mental_pipe_execute(), no more dispatches
 * can be added.  Create a new pipe for each batch.
 *
 * All kernels added to a pipe must be compiled for the same device
 * that the pipe was created on.
 *
 * Usage:
 *   mental_pipe pipe = mental_pipe_create(device);
 *   mental_pipe_add(pipe, kernel_a, inputs, 2, intermediate, N);
 *   mental_pipe_add(pipe, kernel_b, &intermediate, 1, output, N);
 *   mental_pipe_execute(pipe);   // one GPU submission for both
 *   mental_pipe_finalize(pipe);
 */

typedef struct mental_pipe_t* mental_pipe;

mental_pipe mental_pipe_create(mental_device device);
int mental_pipe_add(mental_pipe pipe, mental_kernel kernel,
                     mental_reference *inputs, int input_count,
                     mental_reference output, int work_size);
int mental_pipe_execute(mental_pipe pipe);
void mental_pipe_finalize(mental_pipe pipe);

/*
 * Viewport (Surface Presentation)
 */

typedef struct mental_viewport_t* mental_viewport;

mental_viewport mental_viewport_attach(mental_reference ref, void* surface);
void mental_viewport_present(mental_viewport viewport);
void mental_viewport_detach(mental_viewport viewport);

/*
 * Error Handling
 */

mental_error mental_get_error(void);
const char* mental_get_error_message(void);

/*
 * State (Runtime Introspection)
 */

typedef struct {
    const char *name;
    const char *version;
    int         available;
} mental_library_info;

typedef struct {
    mental_api_type       active_backend;
    const char           *active_backend_name;
    int                   device_count;
    mental_device        *devices;
    int                   library_count;
    mental_library_info  *libraries;
} mental_state;

mental_state* mental_state_get(void);
void mental_state_free(mental_state *state);
void mental_register_library(const char *name, const char *version, int available);

/*
 * Lifecycle
 */

void mental_atexit(void (*fn)(void));
int mental_shutdown(void);

/*
 * Sanity Check (Built-in Self-Test)
 *
 * Exercises device enumeration, buffer operations, shader compilation,
 * GPU dispatch, and result verification.  Prints results to stdout.
 *
 * Returns 0 if all checks pass, non-zero on failure.
 * If no GPU devices are found, GPU checks are skipped and the function
 * returns 0 (graceful degradation is correct behavior).
 */

int mental_sanity_check(void);

#ifdef __cplusplus
}
#endif

#endif /* MENTAL_H */
