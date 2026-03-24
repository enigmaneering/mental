/*
 * Mental - Internal Types and Backend Interface
 *
 * Internal header for backend implementations.
 * Not exposed in public API.
 */

#ifndef MENTAL_INTERNAL_H
#define MENTAL_INTERNAL_H

#include "mental.h"
#include <pthread.h>

/* Forward declarations */
typedef struct mental_backend_t mental_backend;

/* Device structure */
struct mental_device_t {
    int index;
    char name[256];
    mental_api_type api;
    mental_backend* backend;
    void* backend_device;  /* Backend-specific device handle */
};

/*
 * Unified Reference structure.
 *
 * Every reference has shared-memory backing (named, UUID-scoped,
 * cross-process visible).  Optionally, it can be pinned to a GPU
 * device, adding a backend buffer for compute operations.
 *
 * Shared memory provides the cross-process data plane.
 * GPU pinning provides the compute plane.
 * Disclosure controls the access plane.
 */
struct mental_reference_t {
    /* Shared memory (always present) */
    void  *addr;       /* mmap'd / MapViewOfFile address (includes header) */
    size_t total_size; /* total mapped size (header + user data) */
    size_t user_size;  /* user-visible data size */
    int    owner;      /* 1 = we created it, 0 = observer */
    char   path[320];  /* shm path for cleanup */

    /* GPU backing (optional — NULL if CPU-only) */
    mental_device device;
    void *backend_buffer;

    /* Credential provider (owner only) — evaluated under spinlock */
    mental_credential_fn credential_fn;
    void                *credential_ctx;

    /* Thread safety */
    pthread_mutex_t lock;
    int valid;

#ifdef _WIN32
    HANDLE hMap;
#else
    int    fd;
#endif
};

/* Kernel structure (compiled shader) */
struct mental_kernel_t {
    mental_device device;
    void* backend_kernel;  /* Backend-specific kernel handle */
    pthread_mutex_t lock;
    int valid;
};

/* Viewport structure (surface presentation) */
struct mental_viewport_t {
    mental_reference reference;  /* Reference to present */
    void* backend_viewport;      /* Backend-specific viewport handle */
    pthread_mutex_t lock;
    int valid;
};

/* Backend interface - each backend (Metal/D3D12/Vulkan/OpenCL) implements these */
struct mental_backend_t {
    const char* name;
    mental_api_type api;

    /* Backend initialization/cleanup */
    int (*init)(void);
    void (*shutdown)(void);

    /* Device enumeration */
    int (*device_count)(void);
    int (*device_info)(int index, char* name, size_t name_len);
    void* (*device_create)(int index);
    void (*device_destroy)(void* dev);

    /* Buffer operations */
    void* (*buffer_alloc)(void* dev, size_t bytes);
    void (*buffer_write)(void* buf, const void* data, size_t bytes);
    void (*buffer_read)(void* buf, void* data, size_t bytes);
    void* (*buffer_resize)(void* dev, void* old_buf, size_t old_size, size_t new_size);
    void* (*buffer_clone)(void* dev, void* src_buf, size_t size);
    void (*buffer_destroy)(void* buf);

    /* Kernel operations */
    void* (*kernel_compile)(void* dev, const char* source, size_t source_len, char* error, size_t error_len);
    void (*kernel_dispatch)(void* kernel, void** inputs, int input_count, void* output, int work_size);
    void (*kernel_destroy)(void* kernel);

    /* Viewport operations (optional - may be NULL for backends without native presentation) */
    void* (*viewport_attach)(void* dev, void* buffer, void* surface, char* error, size_t error_len);
    void (*viewport_present)(void* viewport);
    void (*viewport_detach)(void* viewport);
};

/* Backend registry */
extern mental_backend* metal_backend;
extern mental_backend* d3d12_backend;
extern mental_backend* vulkan_backend;
#ifdef MENTAL_HAS_OPENCL
extern mental_backend* opencl_backend;
#endif

/* Error handling (thread-local) */
void mental_set_error(mental_error code, const char* message);

/* Transpilation */
char* mental_transpile(const char* source, size_t source_len, mental_api_type target_api, size_t* out_len);
void mental_transpile_free(char* transpiled);

#endif /* MENTAL_INTERNAL_H */
