/*
 * Mental - Internal Types and Backend Interface
 *
 * Internal header for backend implementations and the reference system.
 * Not exposed in public API.
 */

#ifndef MENTAL_INTERNAL_H
#define MENTAL_INTERNAL_H

#include "mental.h"
#include "mental_pthread.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations */
typedef struct mental_backend_t mental_backend;

/* Device structure */
struct mental_device_t {
    int index;
    char name[256];
    mental_api_type api;
    mental_backend* backend;
    void* backend_device;
};

/* Reference structure (process-local) */
struct mental_reference_t {
    void  *data;
    size_t size;

    mental_relationship mode;
    uint8_t *credential;
    size_t   credential_len;
    mental_credential_fn credential_fn;
    void                *credential_ctx;

    mental_device device;
    void *backend_buffer;

    pthread_mutex_t lock;
    int valid;
};

/* Disclosure handle */
struct mental_disclosure_t {
    mental_reference ref;
};

/* Kernel structure */
struct mental_kernel_t {
    mental_device device;
    void* backend_kernel;
    int workgroup_size;     /* set by backend at compile time */
    pthread_mutex_t lock;
    int valid;
};

/* Pipe structure (chained kernel dispatch) */
struct mental_pipe_t {
    mental_device device;
    void *backend_pipe;
    pthread_mutex_t lock;
    int valid;
    int dispatch_count;
    int executed;       /* 1 after execute — no more adds allowed */
};

/* Viewport structure */
struct mental_viewport_t {
    mental_reference reference;
    void* backend_viewport;
    pthread_mutex_t lock;
    int valid;
};

/* Backend interface */
struct mental_backend_t {
    const char* name;
    mental_api_type api;

    int (*init)(void);
    void (*shutdown)(void);

    int (*device_count)(void);
    int (*device_info)(int index, char* name, size_t name_len);
    void* (*device_create)(int index);
    void (*device_destroy)(void* dev);

    void* (*buffer_alloc)(void* dev, size_t bytes);
    void (*buffer_write)(void* buf, const void* data, size_t bytes);
    void (*buffer_read)(void* buf, void* data, size_t bytes);
    void* (*buffer_resize)(void* dev, void* old_buf, size_t old_size, size_t new_size);
    void* (*buffer_clone)(void* dev, void* src_buf, size_t size);
    void (*buffer_destroy)(void* buf);

    void* (*kernel_compile)(void* dev, const char* source, size_t source_len, char* error, size_t error_len);
    int (*kernel_workgroup_size)(void* kernel);
    void (*kernel_dispatch)(void* kernel, void** inputs, int input_count, void* output, int work_size);
    void (*kernel_destroy)(void* kernel);

    /* Pipe operations (chained dispatch) */
    void* (*pipe_create)(void* dev);
    int   (*pipe_add)(void* pipe, void* kernel, void** inputs,
                       int input_count, void* output, int work_size);
    int   (*pipe_execute)(void* pipe);
    void  (*pipe_destroy)(void* pipe);

    /* Viewport operations (optional) */
    void* (*viewport_attach)(void* dev, void* buffer, void* surface, char* error, size_t error_len);
    void (*viewport_present)(void* viewport);
    void (*viewport_detach)(void* viewport);
};

/* Backend registry — all backends are always declared.
 * Each backend file exports either a valid pointer (when the platform
 * supports it and dlopen succeeds) or NULL (graceful degradation). */
extern mental_backend* metal_backend;
extern mental_backend* d3d12_backend;
extern mental_backend* vulkan_backend;
extern mental_backend* opencl_backend;
extern mental_backend* opengl_backend;
extern mental_backend* d3d11_backend;
extern mental_backend* pocl_backend;
extern mental_backend* webgpu_backend;

/* Error handling */
void mental_set_error(mental_error code, const char* message);

/* Transpilation */
char* mental_transpile(const char* source, size_t source_len, mental_api_type target_api, size_t* out_len);
void mental_transpile_free(char* transpiled);

#ifdef __cplusplus
}
#endif

#endif /* MENTAL_INTERNAL_H */
