/*
 * Mental - Core Implementation
 */

#include "mental.h"
#include "mental_internal.h"
#include "transpile.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Version macros from compiled-in transpilation libraries.
 * These headers are in the include paths set by CMakeLists.txt:
 *   external/glslang/ and external/spirv-cross/ */
#include "build/include/glslang/build_info.h"
#include "spirv_cross_c.h"

#ifndef _WIN32
#include <unistd.h>
#define MENTAL_ACCESS access
#else
#include <io.h>
#define MENTAL_ACCESS _access
#ifndef F_OK
#define F_OK 0
#endif
#endif

/* Global state */
mental_device* g_devices = NULL;
int g_device_count = 0;
pthread_mutex_t g_init_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_initialized = 0;

/* Thread-local error state */
static _Thread_local mental_error g_last_error = MENTAL_SUCCESS;
static _Thread_local char g_last_error_message[512] = {0};

/* Backend priority order by platform.
 * Each backend is only in the list when its SDK was found at build time.
 * The runtime init loop (mental_initialize) tries them in order and picks
 * the first one that successfully initialises and reports devices. */
static mental_backend** get_backend_priority(int* count) {
    static mental_backend* backends[8];
    *count = 0;

#if defined(__APPLE__)
    /* macOS: Metal -> OpenCL */
#ifdef MENTAL_HAS_METAL
    if (metal_backend) backends[(*count)++] = metal_backend;
#endif
#elif defined(_WIN32)
    /* Windows: D3D12 -> Vulkan -> OpenCL -> OpenGL -> PoCL */
#ifdef MENTAL_HAS_D3D12
    if (d3d12_backend) backends[(*count)++] = d3d12_backend;
#endif
#ifdef MENTAL_HAS_VULKAN
    if (vulkan_backend) backends[(*count)++] = vulkan_backend;
#endif
#else
    /* Linux: Vulkan -> OpenCL */
#ifdef MENTAL_HAS_VULKAN
    if (vulkan_backend) backends[(*count)++] = vulkan_backend;
#endif
#endif

    /* Universal fallbacks: OpenCL -> OpenGL 4.3+ -> PoCL (CPU-only last resort) */
#ifdef MENTAL_HAS_OPENCL
    if (opencl_backend) backends[(*count)++] = opencl_backend;
#endif
#ifdef MENTAL_HAS_OPENGL
    if (opengl_backend) backends[(*count)++] = opengl_backend;
#endif
#ifdef MENTAL_HAS_POCL
    if (pocl_backend) backends[(*count)++] = pocl_backend;
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

    /* Register the selected backend as a library */
    mental_register_library(selected_backend->name, NULL, 1);

    /* Register compiled-in transpilation libraries with versions */
    {
        char glslang_ver[64];
        snprintf(glslang_ver, sizeof(glslang_ver), "%d.%d.%d",
                 GLSLANG_VERSION_MAJOR, GLSLANG_VERSION_MINOR, GLSLANG_VERSION_PATCH);
        mental_register_library("glslang", glslang_ver, 1);

        char spirv_cross_ver[64];
        snprintf(spirv_cross_ver, sizeof(spirv_cross_ver), "%d.%d.%d",
                 SPVC_C_API_VERSION_MAJOR, SPVC_C_API_VERSION_MINOR, SPVC_C_API_VERSION_PATCH);
        mental_register_library("spirv-cross", spirv_cross_ver, 1);
    }

    pthread_mutex_unlock(&g_init_lock);

    /* Auto-detect external tool paths (DXC, Naga) from common locations.
     * Only probes if not already configured via mental_set_tool_path. */
    static const char *dxc_paths[] = {
        "external/dxc/bin/dxc", "external/dxc/dxc",
        "external/dxc/bin/dxc.exe", "external/dxc/dxc.exe",
        "../external/dxc/bin/dxc", "../external/dxc/dxc",
        "../external/dxc/bin/dxc.exe", "../external/dxc/dxc.exe",
        "../../external/dxc/bin/dxc", "../../external/dxc/dxc",
        "../../external/dxc/bin/dxc.exe", "../../external/dxc/dxc.exe",
        NULL
    };
    static const char *naga_paths[] = {
        "external/naga/bin/naga", "external/naga/bin/naga.exe",
        "../external/naga/bin/naga", "../external/naga/bin/naga.exe",
        "../../external/naga/bin/naga", "../../external/naga/bin/naga.exe",
        NULL
    };
    if (!mental_get_tool_path(MENTAL_TOOL_DXC)) {
        for (int i = 0; dxc_paths[i]; i++) {
            if (MENTAL_ACCESS(dxc_paths[i], F_OK) == 0) {
                /* Resolve to absolute path — MSYS2 popen can't handle ../.. */
                char resolved[4096];
#ifdef _WIN32
                if (_fullpath(resolved, dxc_paths[i], sizeof(resolved)))
                    mental_set_tool_path(MENTAL_TOOL_DXC, resolved);
                else
#endif
                    mental_set_tool_path(MENTAL_TOOL_DXC, dxc_paths[i]);
                break;
            }
        }
    }
    if (!mental_get_tool_path(MENTAL_TOOL_NAGA)) {
        for (int i = 0; naga_paths[i]; i++) {
            if (MENTAL_ACCESS(naga_paths[i], F_OK) == 0) {
                char resolved[4096];
#ifdef _WIN32
                if (_fullpath(resolved, naga_paths[i], sizeof(resolved)))
                    mental_set_tool_path(MENTAL_TOOL_NAGA, resolved);
                else
#endif
                    mental_set_tool_path(MENTAL_TOOL_NAGA, naga_paths[i]);
                break;
            }
        }
    }

    /* External tools (dxc, naga) are registered lazily — the Go layer
     * may discover them later via ensureTools().  Don't register them
     * here with a potentially stale available=0. */
}

/*
 * Library Registry
 *
 * Backends and tools register themselves here during initialization.
 * The registry is a simple dynamic array — no hardcoded library names.
 */

#define MAX_LIBRARIES 32
static mental_library_info g_libraries[MAX_LIBRARIES];
static int g_library_count = 0;
static pthread_mutex_t g_library_lock = PTHREAD_MUTEX_INITIALIZER;

void mental_register_library(const char *name, const char *version, int available) {
    pthread_mutex_lock(&g_library_lock);
    if (g_library_count < MAX_LIBRARIES) {
        mental_library_info *lib = &g_libraries[g_library_count++];
        lib->name = strdup(name);
        lib->version = version ? strdup(version) : NULL;
        lib->available = available;
    }
    pthread_mutex_unlock(&g_library_lock);
}

/*
 * State Snapshot
 */

mental_state* mental_state_get(void) {
    /* Block until backend discovery is complete. */
    if (!g_initialized) mental_initialize();

    mental_state *s = calloc(1, sizeof(mental_state));
    if (!s) return NULL;

    /* Active backend */
    if (g_device_count > 0 && g_devices && g_devices[0]) {
        s->active_backend = g_devices[0]->api;
        s->active_backend_name = mental_device_api_name(g_devices[0]);
    } else {
        s->active_backend = -1;
        s->active_backend_name = "none";
    }

    /* Devices — copy the handle array (not the structs) */
    s->device_count = g_device_count;
    if (g_device_count > 0) {
        s->devices = calloc(g_device_count, sizeof(mental_device));
        for (int i = 0; i < g_device_count; i++)
            s->devices[i] = g_devices[i];
    }

    /* Libraries — deep copy the registry + append tools checked at snapshot time */
    pthread_mutex_lock(&g_library_lock);
    int tool_count = 2; /* dxc, naga */
    int total = g_library_count + tool_count;
    s->library_count = total;
    s->libraries = calloc(total, sizeof(mental_library_info));

    for (int i = 0; i < g_library_count; i++) {
        s->libraries[i].name = strdup(g_libraries[i].name);
        s->libraries[i].version = g_libraries[i].version ? strdup(g_libraries[i].version) : NULL;
        s->libraries[i].available = g_libraries[i].available;
    }
    pthread_mutex_unlock(&g_library_lock);

    /* Tools are checked live — the Go layer may have configured them
     * after mental_initialize() ran. */
    int idx = g_library_count;
    s->libraries[idx].name = strdup("dxc");
    s->libraries[idx].version = NULL;
    s->libraries[idx].available = mental_get_tool_path(MENTAL_TOOL_DXC) != NULL;
    idx++;
    s->libraries[idx].name = strdup("naga");
    s->libraries[idx].version = NULL;
    s->libraries[idx].available = mental_get_tool_path(MENTAL_TOOL_NAGA) != NULL;

    return s;
}

void mental_state_free(mental_state *state) {
    if (!state) return;
    free(state->devices);
    for (int i = 0; i < state->library_count; i++) {
        free((void *)state->libraries[i].name);
        free((void *)state->libraries[i].version);
    }
    free(state->libraries);
    free(state);
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
        case MENTAL_API_OPENGL: return "OpenGL";
        case MENTAL_API_POCL: return "PoCL";
        default: return "Unknown";
    }
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

    /* All references must be pinned to a GPU device */
    if (!output->backend_buffer) {
        mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Output reference is not pinned to a GPU device");
        return;
    }

    for (int i = 0; i < input_count; i++) {
        if (inputs[i] && !inputs[i]->backend_buffer) {
            mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Input reference is not pinned to a GPU device");
            return;
        }
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

    if (!ref->backend_buffer || !ref->device) {
        mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Reference is not pinned to a GPU device");
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

static void (*g_atexit_fns[MENTAL_MAX_ATEXIT])(void);
static int g_atexit_count = 0;
static int g_shutdown_registered = 0;

static void mental_shutdown_handler(void) {
    /* Run atexit callbacks in LIFO order */
    for (int i = g_atexit_count - 1; i >= 0; i--) {
        if (g_atexit_fns[i]) {
            g_atexit_fns[i]();
        }
    }
}

void mental_atexit(void (*fn)(void)) {
    if (!g_shutdown_registered) {
        atexit(mental_shutdown_handler);
        g_shutdown_registered = 1;
    }
    if (g_atexit_count < MENTAL_MAX_ATEXIT && fn) {
        g_atexit_fns[g_atexit_count++] = fn;
    }
}
