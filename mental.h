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
 *
 * For globally unique IDs across a spark cluster, combine with process
 * identity (e.g., pid or stdlink fd) to form a composite key:
 *   (process_id, count_seq)
 *
 * Each sparked subprocess starts its own counter at 1 — goroutines
 * within a process share the same global counter.
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
 * Incrementing from empty treats it as 0 (empty + 1 = 1).
 * Decrementing below zero transitions to empty.
 * Empty is the only way to represent "no value" and can only be
 * reinstated by decrementing below zero.
 *
 * Useful for spark-relevant counting: tracking in-flight messages,
 * coordinating stages, reference counting shared resources, etc.
 *
 * All operations are thread-safe via atomic instructions.
 */

/* Opaque handle */
typedef struct mental_counter_t* mental_counter;

/* Create a new counter in the empty state.  Returns NULL on failure. */
mental_counter mental_counter_create(void);

/* Atomically add delta and return the new value.
 * If the counter is empty, it is treated as 0 and transitions out of
 * the empty state — even when delta is 0. */
uint64_t mental_counter_increment(mental_counter ctr, uint64_t delta);

/* Atomically subtract delta and return the new value.
 * If value < delta, the counter becomes empty and returns 0. */
uint64_t mental_counter_decrement(mental_counter ctr, uint64_t delta);

/* Returns 1 if the counter is in the empty state, 0 otherwise. */
int mental_counter_empty(mental_counter ctr);

/* Atomically reset and return the previous value.
 * If to_empty is nonzero, resets to the empty state; otherwise resets to 0.
 * If the counter was empty, returns 0. */
uint64_t mental_counter_reset(mental_counter ctr, int to_empty);

/* Destroy the counter and free its memory. */
void mental_counter_finalize(mental_counter ctr);

/*
 * Ref (UUID-scoped Shared Memory)
 *
 * Named shared memory regions scoped by a per-process UUID.
 * Each process generates a UUID on first use, and all refs it creates
 * live under that namespace: /mental-{uuid}/{name}.
 *
 * Owner (creator):
 *   mental_ref_create("Velocity", 4096) allocates 4096 bytes of shared
 *   memory at /mental-{my_uuid}/Velocity.  Returns a handle to read/write
 *   via mental_ref_data().
 *
 * Observer (sparked child or peer):
 *   mental_ref_open(peer_uuid, "Velocity") maps the owner's region.
 *   Returns NULL gracefully if the owner has exited and the region is gone.
 *
 * Cleanup:
 *   All owned refs are automatically unlinked when the process exits
 *   (via atexit).  Calling mental_ref_close() on an owned ref also
 *   unlinks it immediately.  Observers should close their handles but
 *   the underlying region persists until the owner exits or closes.
 *
 * The intended pattern is for refs to flow down the spark chain:
 * parent creates, children observe.  Sharing outside the spark chain
 * works but fails gracefully if the owner is gone.
 */

/* Opaque handle */
typedef struct mental_ref_t* mental_ref;

/* Disclosure — controls observer access to a ref.
 *
 * The disclosure lives in shared memory so the owner can change it
 * on-the-fly and every observer sees the change immediately.
 *
 *   RELATIONALLY_OPEN (default):
 *     Anyone can read and write.  No passphrase required.
 *
 *   RELATIONALLY_INCLUSIVE:
 *     Read-only access without passphrase.
 *     Write access requires the passphrase.
 *
 *   RELATIONALLY_EXCLUSIVE:
 *     All access (read or write) requires the passphrase.
 *     Without it, mental_ref_data() returns NULL.
 *
 * The owner always has full access regardless of disclosure mode.
 */
typedef enum {
    MENTAL_RELATIONALLY_OPEN      = 0,
    MENTAL_RELATIONALLY_INCLUSIVE  = 1,
    MENTAL_RELATIONALLY_EXCLUSIVE = 2
} mental_disclosure;

/* Get this process's UUID (32 hex chars, no dashes).  Read-only.
 * Generated once on first call, stable for process lifetime. */
const char* mental_uuid(void);

/* Create a named shared memory region of the given size.
 * The region is scoped to this process's UUID namespace.
 * Disclosure defaults to RELATIONALLY_OPEN with no passphrase.
 * Returns NULL on failure (e.g., name too long, allocation failed). */
mental_ref mental_ref_create(const char *name, size_t size);

/* Open an existing ref created by another process.
 * Returns NULL gracefully if the owner has exited or the ref doesn't exist. */
mental_ref mental_ref_open(const char *peer_uuid, const char *name);

/* Get a pointer to the mapped shared memory.
 * Pass NULL for passphrase when disclosure is open or inclusive (read).
 * Returns NULL if the disclosure denies access.
 * The owner always gets full access regardless of disclosure. */
void* mental_ref_data(mental_ref ref, const char *passphrase);

/* Get the size of the mapped region in bytes (user data only). */
size_t mental_ref_size(mental_ref ref);

/* Check if write access is permitted under the current disclosure.
 * Returns 1 if writable, 0 if read-only or denied.
 * The owner always returns 1. */
int mental_ref_writable(mental_ref ref, const char *passphrase);

/* Get the current disclosure mode. */
mental_disclosure mental_ref_get_disclosure(mental_ref ref);

/* Set the disclosure mode (owner only — no-op for observers). */
void mental_ref_set_disclosure(mental_ref ref, mental_disclosure mode);

/* Set the passphrase (owner only — no-op for observers).
 * Pass NULL to clear the passphrase. Max 63 chars. */
void mental_ref_set_passphrase(mental_ref ref, const char *passphrase);

/* Close the ref handle.  If this process owns the ref, the shared
 * memory is unlinked (destroyed).  Observer handles are simply unmapped. */
void mental_ref_close(mental_ref ref);

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
