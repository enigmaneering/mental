/*
 * Mental - Core Implementation
 */

#include "mental.h"
#include "mental_internal.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Global state */
mental_device* g_devices = NULL;
int g_device_count = 0;
pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

/* Thread-local error state */
static _Thread_local mental_error g_last_error = MENTAL_SUCCESS;
static _Thread_local char g_last_error_message[512] = {0};

/* Backend priority order by platform */
static mental_backend** get_backend_priority(int* count) {
    static mental_backend* backends[4];
    *count = 0;

#if defined(__APPLE__)
    /* macOS: Metal -> OpenCL */
    if (metal_backend) backends[(*count)++] = metal_backend;
#ifdef MENTAL_HAS_OPENCL
    if (opencl_backend) backends[(*count)++] = opencl_backend;
#endif
#elif defined(_WIN32)
    /* Windows: D3D12 -> OpenCL */
    if (d3d12_backend) backends[(*count)++] = d3d12_backend;
#ifdef MENTAL_HAS_OPENCL
    if (opencl_backend) backends[(*count)++] = opencl_backend;
#endif
#else
    /* Linux: Vulkan -> OpenCL */
    if (vulkan_backend) backends[(*count)++] = vulkan_backend;
#ifdef MENTAL_HAS_OPENCL
    if (opencl_backend) backends[(*count)++] = opencl_backend;
#endif
#endif

    return backends;
}

/* Initialize Mental - discover devices */
static void mental_initialize(void) {
    pthread_mutex_lock(&g_init_lock);

    if (g_initialized) {
        pthread_mutex_unlock(&g_init_lock);
        return;
    }

    /* Try backends in priority order */
    int backend_count;
    mental_backend** backends = get_backend_priority(&backend_count);

    mental_backend* selected_backend = NULL;
    for (int i = 0; i < backend_count; i++) {
        if (backends[i]->init() == 0) {
            int count = backends[i]->device_count();
            if (count > 0) {
                selected_backend = backends[i];
                g_device_count = count;
                break;
            }
            backends[i]->shutdown();
        }
    }

    if (!selected_backend) {
        mental_set_error(MENTAL_ERROR_NO_DEVICES, "No GPU devices found");
        pthread_mutex_unlock(&g_init_lock);
        return;
    }

    /* Create device handles */
    g_devices = calloc(g_device_count, sizeof(mental_device));
    for (int i = 0; i < g_device_count; i++) {
        g_devices[i] = malloc(sizeof(struct mental_device_t));
        g_devices[i]->index = i;
        g_devices[i]->api = selected_backend->api;
        g_devices[i]->backend = selected_backend;

        selected_backend->device_info(i, g_devices[i]->name, sizeof(g_devices[i]->name));
        g_devices[i]->backend_device = selected_backend->device_create(i);
    }

    g_initialized = 1;
    pthread_mutex_unlock(&g_init_lock);
}

/*
 * Device Management
 */

int mental_device_count(void) {
    if (!g_initialized) mental_initialize();
    return g_device_count;
}

mental_device mental_device_get(int index) {
    if (!g_initialized) mental_initialize();

    if (index < 0 || index >= g_device_count) {
        mental_set_error(MENTAL_ERROR_INVALID_DEVICE, "Invalid device index");
        return NULL;
    }

    return g_devices[index];
}

const char* mental_device_name(mental_device dev) {
    if (!dev) return "";
    return dev->name;
}

mental_api_type mental_device_api(mental_device dev) {
    if (!dev) return MENTAL_API_OPENCL;
    return dev->api;
}

const char* mental_device_api_name(mental_device dev) {
    if (!dev) return "Unknown";

    switch (dev->api) {
        case MENTAL_API_METAL: return "Metal";
        case MENTAL_API_D3D12: return "D3D12";
        case MENTAL_API_VULKAN: return "Vulkan";
        case MENTAL_API_OPENCL: return "OpenCL";
        default: return "Unknown";
    }
}

/*
 * Reference (GPU Memory Buffer)
 */

mental_reference mental_alloc(mental_device dev, size_t bytes) {
    if (!dev) {
        mental_set_error(MENTAL_ERROR_INVALID_DEVICE, "Invalid device");
        return NULL;
    }

    mental_reference ref = malloc(sizeof(struct mental_reference_t));
    if (!ref) {
        mental_set_error(MENTAL_ERROR_ALLOCATION_FAILED, "Failed to allocate reference");
        return NULL;
    }

    ref->device = dev;
    ref->size = bytes;
    ref->valid = 1;
    pthread_mutex_init(&ref->lock, NULL);

    ref->backend_buffer = dev->backend->buffer_alloc(dev->backend_device, bytes);
    if (!ref->backend_buffer) {
        pthread_mutex_destroy(&ref->lock);
        free(ref);
        mental_set_error(MENTAL_ERROR_ALLOCATION_FAILED, "Backend buffer allocation failed");
        return NULL;
    }

    return ref;
}

void mental_write(mental_reference ref, const void* data, size_t bytes) {
    if (!ref || !ref->valid) {
        mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Invalid reference");
        return;
    }

    pthread_mutex_lock(&ref->lock);

    /* Auto-resize if needed */
    if (bytes > ref->size) {
        void* new_buffer = ref->device->backend->buffer_resize(
            ref->device->backend_device,
            ref->backend_buffer,
            ref->size,
            bytes
        );

        if (!new_buffer) {
            pthread_mutex_unlock(&ref->lock);
            mental_set_error(MENTAL_ERROR_ALLOCATION_FAILED, "Failed to resize buffer");
            return;
        }

        ref->backend_buffer = new_buffer;
        ref->size = bytes;
    }

    /* Write data */
    ref->device->backend->buffer_write(ref->backend_buffer, data, bytes);

    pthread_mutex_unlock(&ref->lock);
}

void mental_read(mental_reference ref, void* data, size_t bytes) {
    if (!ref || !ref->valid) {
        mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Invalid reference");
        return;
    }

    pthread_mutex_lock(&ref->lock);

    size_t read_size = bytes < ref->size ? bytes : ref->size;
    ref->device->backend->buffer_read(ref->backend_buffer, data, read_size);

    pthread_mutex_unlock(&ref->lock);
}

size_t mental_size(mental_reference ref) {
    if (!ref || !ref->valid) return 0;

    pthread_mutex_lock(&ref->lock);
    size_t size = ref->size;
    pthread_mutex_unlock(&ref->lock);

    return size;
}

mental_reference mental_clone(mental_reference ref) {
    if (!ref || !ref->valid) {
        mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Invalid reference");
        return NULL;
    }

    /* Lock source buffer */
    pthread_mutex_lock(&ref->lock);

    /* Create new reference structure */
    mental_reference clone = (mental_reference)malloc(sizeof(struct mental_reference_t));
    if (!clone) {
        pthread_mutex_unlock(&ref->lock);
        mental_set_error(MENTAL_ERROR_ALLOCATION_FAILED, "Failed to allocate clone reference");
        return NULL;
    }

    clone->device = ref->device;
    clone->size = ref->size;
    clone->valid = 1;
    pthread_mutex_init(&clone->lock, NULL);

    /* Use backend's buffer_clone to copy data */
    clone->backend_buffer = ref->device->backend->buffer_clone(
        ref->device->backend_device,
        ref->backend_buffer,
        ref->size
    );

    if (!clone->backend_buffer) {
        pthread_mutex_unlock(&ref->lock);
        pthread_mutex_destroy(&clone->lock);
        free(clone);
        mental_set_error(MENTAL_ERROR_ALLOCATION_FAILED, "Failed to clone buffer");
        return NULL;
    }

    /* Unlock source buffer */
    pthread_mutex_unlock(&ref->lock);

    return clone;
}

void mental_finalize(mental_reference ref) {
    if (!ref) return;

    if (ref->valid) {
        pthread_mutex_lock(&ref->lock);
        ref->device->backend->buffer_destroy(ref->backend_buffer);
        ref->valid = 0;
        pthread_mutex_unlock(&ref->lock);
        pthread_mutex_destroy(&ref->lock);
    }

    free(ref);
}

/*
 * Kernel (Compute Shader)
 */

mental_kernel mental_compile(mental_device dev, const char* source, size_t source_len) {
    if (!dev) {
        mental_set_error(MENTAL_ERROR_INVALID_DEVICE, "Invalid device");
        return NULL;
    }

    /* Transpile source to backend's native language */
    size_t transpiled_len;
    char* transpiled = mental_transpile(source, source_len, dev->api, &transpiled_len);
    if (!transpiled) {
        mental_set_error(MENTAL_ERROR_COMPILATION_FAILED, "Transpilation failed");
        return NULL;
    }

    /* Compile kernel */
    mental_kernel kernel = malloc(sizeof(struct mental_kernel_t));
    if (!kernel) {
        mental_transpile_free(transpiled);
        mental_set_error(MENTAL_ERROR_ALLOCATION_FAILED, "Failed to allocate kernel");
        return NULL;
    }

    kernel->device = dev;
    kernel->valid = 1;
    pthread_mutex_init(&kernel->lock, NULL);

    char error[1024];
    kernel->backend_kernel = dev->backend->kernel_compile(
        dev->backend_device,
        transpiled,
        transpiled_len,
        error,
        sizeof(error)
    );

    mental_transpile_free(transpiled);

    if (!kernel->backend_kernel) {
        pthread_mutex_destroy(&kernel->lock);
        free(kernel);
        mental_set_error(MENTAL_ERROR_COMPILATION_FAILED, error);
        return NULL;
    }

    return kernel;
}

void mental_dispatch(mental_kernel kernel, mental_reference* inputs, int input_count,
                     mental_reference output, int work_size) {
    if (!kernel || !kernel->valid) {
        mental_set_error(MENTAL_ERROR_INVALID_KERNEL, "Invalid kernel");
        return;
    }

    if (!output || !output->valid) {
        mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Invalid output reference");
        return;
    }

    /* Lock kernel */
    pthread_mutex_lock(&kernel->lock);

    /* Lock all input references */
    for (int i = 0; i < input_count; i++) {
        if (inputs[i] && inputs[i]->valid) {
            pthread_mutex_lock(&inputs[i]->lock);
        }
    }

    /* Lock output reference */
    pthread_mutex_lock(&output->lock);

    /* Gather backend buffer handles */
    void** backend_inputs = NULL;
    if (input_count > 0) {
        backend_inputs = malloc(sizeof(void*) * input_count);
        for (int i = 0; i < input_count; i++) {
            backend_inputs[i] = inputs[i] ? inputs[i]->backend_buffer : NULL;
        }
    }

    /* Dispatch */
    kernel->device->backend->kernel_dispatch(
        kernel->backend_kernel,
        backend_inputs,
        input_count,
        output->backend_buffer,
        work_size
    );

    if (backend_inputs) free(backend_inputs);

    /* Unlock all */
    pthread_mutex_unlock(&output->lock);
    for (int i = input_count - 1; i >= 0; i--) {
        if (inputs[i] && inputs[i]->valid) {
            pthread_mutex_unlock(&inputs[i]->lock);
        }
    }
    pthread_mutex_unlock(&kernel->lock);
}

void mental_kernel_finalize(mental_kernel kernel) {
    if (!kernel) return;

    if (kernel->valid) {
        pthread_mutex_lock(&kernel->lock);
        kernel->device->backend->kernel_destroy(kernel->backend_kernel);
        kernel->valid = 0;
        pthread_mutex_unlock(&kernel->lock);
        pthread_mutex_destroy(&kernel->lock);
    }

    free(kernel);
}

/*
 * Viewport (Surface Presentation)
 */

mental_viewport mental_viewport_attach(mental_reference ref, void* surface) {
    if (!ref || !ref->valid) {
        mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Invalid reference");
        return NULL;
    }

    if (!surface) {
        mental_set_error(MENTAL_ERROR_INVALID_DEVICE, "Invalid surface");
        return NULL;
    }

    /* Check if backend supports viewports */
    if (!ref->device->backend->viewport_attach) {
        mental_set_error(MENTAL_ERROR_BACKEND_FAILED, "Backend does not support native viewport presentation");
        return NULL;
    }

    /* Create viewport structure */
    mental_viewport viewport = malloc(sizeof(struct mental_viewport_t));
    if (!viewport) {
        mental_set_error(MENTAL_ERROR_ALLOCATION_FAILED, "Failed to allocate viewport");
        return NULL;
    }

    viewport->reference = ref;
    viewport->valid = 1;
    pthread_mutex_init(&viewport->lock, NULL);

    /* Call backend to attach surface */
    char error[1024];
    pthread_mutex_lock(&ref->lock);
    viewport->backend_viewport = ref->device->backend->viewport_attach(
        ref->device->backend_device,
        ref->backend_buffer,
        surface,
        error,
        sizeof(error)
    );
    pthread_mutex_unlock(&ref->lock);

    if (!viewport->backend_viewport) {
        pthread_mutex_destroy(&viewport->lock);
        free(viewport);
        mental_set_error(MENTAL_ERROR_BACKEND_FAILED, error);
        return NULL;
    }

    return viewport;
}

void mental_viewport_present(mental_viewport viewport) {
    if (!viewport || !viewport->valid) {
        mental_set_error(MENTAL_ERROR_INVALID_DEVICE, "Invalid viewport");
        return;
    }

    pthread_mutex_lock(&viewport->lock);

    /* Lock reference during present */
    pthread_mutex_lock(&viewport->reference->lock);

    viewport->reference->device->backend->viewport_present(viewport->backend_viewport);

    pthread_mutex_unlock(&viewport->reference->lock);
    pthread_mutex_unlock(&viewport->lock);
}

void mental_viewport_detach(mental_viewport viewport) {
    if (!viewport) return;

    if (viewport->valid) {
        pthread_mutex_lock(&viewport->lock);
        viewport->reference->device->backend->viewport_detach(viewport->backend_viewport);
        viewport->valid = 0;
        pthread_mutex_unlock(&viewport->lock);
        pthread_mutex_destroy(&viewport->lock);
    }

    free(viewport);
}

/*
 * Error Handling
 */

void mental_set_error(mental_error code, const char* message) {
    g_last_error = code;
    strncpy(g_last_error_message, message, sizeof(g_last_error_message) - 1);
    g_last_error_message[sizeof(g_last_error_message) - 1] = '\0';
}

mental_error mental_get_error(void) {
    return g_last_error;
}

const char* mental_get_error_message(void) {
    return g_last_error_message;
}

/*
 * Lifecycle Management
 */

#define MENTAL_MAX_ATEXIT 32
#define MENTAL_MAX_TEMP_FILES 64

static void (*g_atexit_fns[MENTAL_MAX_ATEXIT])(void);
static int g_atexit_count = 0;

static char g_temp_files[MENTAL_MAX_TEMP_FILES][4096];
static int g_temp_file_count = 0;

static int g_shutdown_registered = 0;

static void mental_shutdown_handler(void) {
    /* Run atexit callbacks in LIFO order */
    for (int i = g_atexit_count - 1; i >= 0; i--) {
        if (g_atexit_fns[i]) {
            g_atexit_fns[i]();
        }
    }
    /* Remove registered temporary files */
    for (int i = 0; i < g_temp_file_count; i++) {
        remove(g_temp_files[i]);
    }
}

static void ensure_shutdown_registered(void) {
    if (!g_shutdown_registered) {
        atexit(mental_shutdown_handler);
        g_shutdown_registered = 1;
    }
}

void mental_atexit(void (*fn)(void)) {
    ensure_shutdown_registered();
    if (g_atexit_count < MENTAL_MAX_ATEXIT && fn) {
        g_atexit_fns[g_atexit_count++] = fn;
    }
}

void mental_register_temp_file(const char* path) {
    ensure_shutdown_registered();
    if (g_temp_file_count < MENTAL_MAX_TEMP_FILES && path) {
        strncpy(g_temp_files[g_temp_file_count], path, 4095);
        g_temp_files[g_temp_file_count][4095] = '\0';
        g_temp_file_count++;
    }
}
