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
 * Reference (Unified Shareable Data)
 *
 * A reference is a named, UUID-scoped shared memory region that can
 * optionally be pinned to a GPU device for compute operations.
 *
 *   Shared memory provides the cross-process data plane.
 *   GPU pinning provides the compute plane.
 *   Disclosure controls the access plane.
 *
 * Owner (creator):
 *   mental_reference_create("Velocity", 4096) allocates 4096 bytes of
 *   shared memory at /mental-{my_uuid}/Velocity.
 *
 * Observer (sparked child or peer):
 *   mental_reference_open(peer_uuid, "Velocity") maps the owner's region.
 *   Returns NULL gracefully if the owner has exited.
 *
 * GPU pinning:
 *   mental_reference_pin(ref, device) attaches a GPU buffer.
 *   mental_reference_write/read transfer data between host and GPU.
 *   Pinned references can participate in dispatch and viewport operations.
 *
 * Cleanup:
 *   All owned refs are automatically unlinked at process exit.
 *   Calling mental_reference_close() on an owned ref unlinks immediately.
 *
 * All operations are thread-safe (internally locked).
 */

/* Disclosure — controls observer access to a reference.
 *
 *   RELATIONALLY_OPEN (default):
 *     Anyone can read and write.  No credential required.
 *
 *   RELATIONALLY_INCLUSIVE:
 *     Read-only access without credential.
 *     Write access requires the credential.
 *
 *   RELATIONALLY_EXCLUSIVE:
 *     All access (read or write) requires the credential.
 *     Without it, mental_reference_data() returns NULL.
 *
 * The owner always has full access regardless of disclosure mode.
 */
typedef enum {
    MENTAL_RELATIONALLY_OPEN      = 0,
    MENTAL_RELATIONALLY_INCLUSIVE  = 1,
    MENTAL_RELATIONALLY_EXCLUSIVE = 2
} mental_disclosure;

/* Credential provider callback type.
 * Called under the disclosure mutex each time an access check occurs.
 * Write credential bytes into buf (capacity buf_size), set *out_len.
 *
 *   ctx      — opaque context pointer (passed through from registration)
 *   buf      — write credential bytes here
 *   buf_size — capacity (always 128 bytes)
 *   out_len  — set to number of bytes written */
typedef void (*mental_credential_fn)(void *ctx,
                                      void *buf, size_t buf_size,
                                      size_t *out_len);

/* Get this process's UUID (32 hex chars, no dashes).  Read-only.
 * Generated once on first call, stable for process lifetime. */
const char* mental_uuid(void);

/* Create a named shared memory region of the given size.
 * The region is scoped to this process's UUID namespace.
 * Disclosure defaults to RELATIONALLY_OPEN with no credential.
 * Returns NULL on failure (e.g., name too long, allocation failed). */
mental_reference mental_reference_create(const char *name, size_t size);

/* Open an existing reference created by another process.
 * Returns NULL gracefully if the owner has exited or doesn't exist. */
mental_reference mental_reference_open(const char *peer_uuid, const char *name);

/* Get a pointer to the mapped shared memory.
 * Pass NULL/0 for credential when disclosure is open or inclusive (read).
 * Returns NULL if the disclosure denies access.
 * The owner always gets full access regardless of disclosure. */
void* mental_reference_data(mental_reference ref,
                             const void *credential, size_t credential_len);

/* Get the size of the mapped region in bytes (user data only). */
size_t mental_reference_size(mental_reference ref);

/* Check if write access is permitted under the current disclosure.
 * Returns 1 if writable, 0 if read-only or denied.
 * The owner always returns 1. */
int mental_reference_writable(mental_reference ref,
                               const void *credential, size_t credential_len);

/* Get the current disclosure mode. */
mental_disclosure mental_reference_get_disclosure(mental_reference ref);

/* Set the disclosure mode (owner only — no-op for observers). */
void mental_reference_set_disclosure(mental_reference ref, mental_disclosure mode);

/* Set the credential bytes (owner only — no-op for observers).
 * The credential is opaque — mental stores and compares raw bytes.
 * Pass NULL/0 to clear.  Max 128 bytes.
 * Setting raw bytes clears any credential provider. */
void mental_reference_set_credential(mental_reference ref,
                                      const void *credential, size_t len);

/* Set a credential provider function (owner only).
 * Instead of caching credential bytes, the provider is evaluated
 * under the disclosure mutex each time an access check occurs.
 * This guarantees the comparison always uses a fresh credential —
 * no stale cache, no race window.
 * Pass NULL to clear the provider. */
void mental_reference_set_credential_provider(mental_reference ref,
                                               mental_credential_fn fn, void *ctx);

/* Returns 1 if this handle owns the reference (creator), 0 if observer. */
int mental_reference_is_owner(mental_reference ref);

/* Clone the reference into a new locally-owned region.
 * Snapshots the current observed value into a fresh reference under this
 * process's UUID namespace.  If the source is a cross-process observer
 * handle, this breaks the linkage — the result is an independent copy.
 *
 * If device is non-NULL, the clone is pinned to that GPU device.  This
 * enables cross-boundary clone-and-pin: observe a remote reference, then
 * clone it directly onto a local GPU — even if the source device differs.
 *
 * The credential is used to obtain read access on the source.
 * Returns NULL if the source is inaccessible or allocation fails. */
mental_reference mental_reference_clone(mental_reference ref,
                                         const char *new_name,
                                         mental_device device,
                                         const void *credential,
                                         size_t credential_len);

/* Close the reference handle.  If this process owns it, the shared
 * memory is unlinked (destroyed) and any GPU buffer is freed.
 * Observer handles are simply unmapped. */
void mental_reference_close(mental_reference ref);

/*
 * GPU Pinning
 *
 * Pin a reference to a GPU device, attaching a backend buffer for
 * compute operations (dispatch, viewport).  Data written via
 * mental_reference_write() goes to both GPU and shared memory.
 */

/* Pin the reference to a GPU device.
 * Allocates a backend buffer and optionally uploads the current
 * shared memory contents to the GPU.
 * Returns 0 on success, -1 on failure.
 * No-op if already pinned to the same device. */
int mental_reference_pin(mental_reference ref, mental_device device);

/* Write data to the reference.
 * If pinned: writes to both GPU buffer and shared memory.
 * If not pinned: writes to shared memory only.
 * Size is clamped to the reference's capacity. */
void mental_reference_write(mental_reference ref,
                             const void *data, size_t bytes);

/* Read data from the reference.
 * If pinned: reads from GPU buffer to host memory.
 * If not pinned: reads from shared memory.
 * Size is clamped to the reference's capacity. */
void mental_reference_read(mental_reference ref,
                            void *data, size_t bytes);

/* Returns 1 if the reference is pinned to a GPU device, 0 otherwise. */
int mental_reference_is_pinned(mental_reference ref);

/* Returns the GPU device the reference is pinned to, or NULL. */
mental_device mental_reference_device(mental_reference ref);

/*
 * Kernel (Compute Shader)
 *
 * Automatic language detection and transpilation.
 * Supports: GLSL, HLSL, MSL, WGSL source code.
 */

/* Compile shader source for device (auto-detects language and transpiles) */
mental_kernel mental_compile(mental_device dev, const char* source, size_t source_len);

/* Execute kernel with input/output references.
 * All references must be pinned to a GPU device.
 * Backend buffers are used directly for compute. */
void mental_dispatch(mental_kernel kernel, mental_reference* inputs, int input_count,
                     mental_reference output, int work_size);

/* Free compiled kernel (must be called explicitly) */
void mental_kernel_finalize(mental_kernel kernel);

/*
 * Viewport (Surface Presentation)
 *
 * Attach a reference to an OS surface for zero-copy presentation.
 * The reference must be pinned to a GPU device.
 */

/* Opaque handle */
typedef struct mental_viewport_t* mental_viewport;

/* Attach reference to OS surface for presentation.
 * The reference must be pinned. */
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
 * Standard Link (stdlink)
 *
 * A bidirectional file descriptor for structured record exchange,
 * sitting alongside stdin(0), stdout(1), stderr(2) as fd 3.
 *
 * Backed by socketpair(AF_UNIX, SOCK_STREAM) on Unix and a pipe pair
 * on Windows.  Created lazily on first call to mental_stdlink().
 *
 * Records are length-prefixed on the wire:
 *   [4 bytes: uint32 payload length, network byte order][payload]
 *
 * In the future, sparked child processes will inherit the peer end of
 * this channel as their own stdlink, forming a parent<->child neural bridge.
 */

/* Get the local stdlink file descriptor.  Returns -1 on failure.
 * The fd is created once and reused for the lifetime of the process. */
int mental_stdlink(void);

/* Get the peer (far) end of the stdlink channel.
 * When sparking, this fd is passed to the child process. */
int mental_stdlink_peer(void);

/* Send a length-prefixed record on the local stdlink fd.
 * Returns 0 on success, -1 on error. */
int mental_stdlink_send(const void *data, size_t len);

/* Receive the next length-prefixed record from the local stdlink fd.
 * Blocks until a complete record is available.
 * On success, writes up to buf_len bytes into buf, stores the full
 * record length in *out_len (may exceed buf_len — excess is discarded),
 * and returns 0.  Returns -1 on error. */
int mental_stdlink_recv(void *buf, size_t buf_len, size_t *out_len);

/*
 * Count (Global Monotonic Counter)
 *
 * A process-local, lock-free, strictly increasing 64-bit counter.
 * Each call to mental_count() returns the next value, starting at 1.
 *
 * Thread-safe via atomic fetch-and-add — no locks, no contention.
 */

/* Return the next monotonic value (1, 2, 3, …).  Never returns 0. */
uint64_t mental_count(void);

/*
 * Counter (Counting Primitive)
 *
 * A heap-allocated, lock-free, 64-bit atomic counter.
 * Unlike the global count, counters are independent instances that can
 * be created, incremented, decremented, reset, and finalized.
 *
 * Counters start in an "empty" state — distinct from zero.
 * All operations are thread-safe via atomic instructions.
 */

/* Opaque handle */
typedef struct mental_counter_t* mental_counter;

/* Create a new counter in the empty state.  Returns NULL on failure. */
mental_counter mental_counter_create(void);

/* Atomically add delta and return the new value. */
uint64_t mental_counter_increment(mental_counter ctr, uint64_t delta);

/* Atomically subtract delta and return the new value.
 * If value < delta, the counter becomes empty and returns 0. */
uint64_t mental_counter_decrement(mental_counter ctr, uint64_t delta);

/* Returns 1 if the counter is in the empty state, 0 otherwise. */
int mental_counter_empty(mental_counter ctr);

/* Atomically reset and return the previous value. */
uint64_t mental_counter_reset(mental_counter ctr, int to_empty);

/* Destroy the counter and free its memory. */
void mental_counter_finalize(mental_counter ctr);

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
