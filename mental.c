/*
 * Mental - Core Implementation
 */

#include "mental.h"
#include "mental_internal.h"
#include "transpile.h"
#include <stdint.h>
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

/* Library registry (backends, transpilers, tools) */
#define MAX_LIBRARIES 32
static mental_library_info g_libraries[MAX_LIBRARIES];
static int g_library_count = 0;
static pthread_mutex_t g_library_lock = PTHREAD_MUTEX_INITIALIZER;

/* Thread-local error state */
static _Thread_local mental_error g_last_error = MENTAL_SUCCESS;
static _Thread_local char g_last_error_message[512] = {0};

/* Backend priority order by platform.
 * Each backend is only in the list when its SDK was found at build time.
 * The runtime init loop (mental_initialize) tries them in order and picks
 * the first one that successfully initialises and reports devices. */
static mental_backend** get_backend_priority(int* count) {
    static mental_backend* backends[10];
    *count = 0;

    /* Platform-preferred backends first */
#if defined(__APPLE__)
    /* macOS: Metal */
    if (metal_backend) backends[(*count)++] = metal_backend;
#elif defined(_WIN32)
    /* Windows: D3D12 */
    if (d3d12_backend) backends[(*count)++] = d3d12_backend;
#endif

    /* Cross-platform GPU backends */
    if (vulkan_backend) backends[(*count)++] = vulkan_backend;
    if (webgpu_backend) backends[(*count)++] = webgpu_backend;

    /* Universal fallbacks */
    if (opengl_backend) backends[(*count)++] = opengl_backend;
    if (d3d11_backend)  backends[(*count)++] = d3d11_backend;
    if (opencl_backend) backends[(*count)++] = opencl_backend;
    if (pocl_backend)   backends[(*count)++] = pocl_backend;

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
                /* Register as selected */
                mental_register_library(backends[i]->name, "selected", 1);
            } else {
                /* Initialized but no devices */
                mental_register_library(backends[i]->name, "no devices", 0);
                backends[i]->shutdown();
            }
        } else {
            /* Failed to initialize */
            mental_register_library(backends[i]->name, "init failed", 0);
        }
        if (selected_backend) break;
    }

    /* Register remaining backends that weren't tried */
    for (int i = 0; i < backend_count; i++) {
        if (backends[i] == selected_backend) continue;
        /* Check if already registered (was tried above) */
        int already = 0;
        for (int j = 0; j < g_library_count; j++) {
            if (strcmp(g_libraries[j].name, backends[i]->name) == 0) {
                already = 1;
                break;
            }
        }
        if (!already) {
            mental_register_library(backends[i]->name, "skipped", 0);
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
#if defined(__EMSCRIPTEN__) && defined(MENTAL_HAS_DXC_DIRECT)
        /* WASM: DXC is linked directly as a library — no external binary needed.
         * Register a sentinel path so consumers know HLSL compilation is available. */
        mental_set_tool_path(MENTAL_TOOL_DXC, "(direct)");
#else
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
#endif
    }
    if (!mental_get_tool_path(MENTAL_TOOL_NAGA)) {
#if defined(__EMSCRIPTEN__) && defined(MENTAL_HAS_NAGA_DIRECT)
        /* WASM: Naga is linked directly as a library — no external binary needed. */
        mental_set_tool_path(MENTAL_TOOL_NAGA, "(direct)");
#else
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
#endif
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
    int lib_count = g_library_count;
    pthread_mutex_unlock(&g_library_lock);

    /* Tools are checked live — the Go layer may have configured them
     * after mental_initialize() ran. */
    int idx = lib_count;
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
    if (!dev) return MENTAL_API_NONE;
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
        case MENTAL_API_WEBGPU: return "WebGPU";
        case MENTAL_API_D3D11: return "D3D11";
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

    /* Query the backend for the compiled kernel's workgroup size */
    if (dev->backend->kernel_workgroup_size) {
        kernel->workgroup_size = dev->backend->kernel_workgroup_size(kernel->backend_kernel);
    }
    if (kernel->workgroup_size <= 0) {
        kernel->workgroup_size = 256; /* fallback */
    }

    return kernel;
}

int mental_dispatch(mental_kernel kernel,
                    mental_reference* inputs, int input_count,
                    mental_reference* outputs, int output_count,
                    int work_size) {
    if (!kernel || !kernel->valid) {
        mental_set_error(MENTAL_ERROR_INVALID_KERNEL, "Invalid kernel");
        return -1;
    }

    /* Validate all references are pinned */
    for (int i = 0; i < input_count; i++) {
        if (inputs[i] && !inputs[i]->backend_buffer) {
            mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Input reference is not pinned to a GPU device");
            return -1;
        }
    }
    for (int i = 0; i < output_count; i++) {
        if (!outputs[i] || !outputs[i]->valid || !outputs[i]->backend_buffer) {
            mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Output reference is not pinned to a GPU device");
            return -1;
        }
    }

    /* Collect all unique references (inputs + outputs) for ordered locking */
    mental_reference lock_order[input_count + output_count];
    int lock_count = 0;

    for (int i = 0; i < input_count; i++) {
        if (inputs[i] && inputs[i]->valid) {
            int dup = 0;
            for (int j = 0; j < lock_count; j++) {
                if (lock_order[j] == inputs[i]) { dup = 1; break; }
            }
            if (!dup) lock_order[lock_count++] = inputs[i];
        }
    }
    for (int i = 0; i < output_count; i++) {
        if (outputs[i]->valid) {
            int dup = 0;
            for (int j = 0; j < lock_count; j++) {
                if (lock_order[j] == outputs[i]) { dup = 1; break; }
            }
            if (!dup) lock_order[lock_count++] = outputs[i];
        }
    }

    /* Sort by pointer address to prevent deadlock */
    for (int i = 0; i < lock_count - 1; i++) {
        for (int j = 0; j < lock_count - 1 - i; j++) {
            if ((uintptr_t)lock_order[j] > (uintptr_t)lock_order[j + 1]) {
                mental_reference tmp = lock_order[j];
                lock_order[j] = lock_order[j + 1];
                lock_order[j + 1] = tmp;
            }
        }
    }

    /* Lock kernel, then references in sorted order */
    pthread_mutex_lock(&kernel->lock);
    for (int i = 0; i < lock_count; i++) {
        pthread_mutex_lock(&lock_order[i]->lock);
    }

    /* Gather backend buffer handles */
    void** backend_inputs = NULL;
    if (input_count > 0) {
        backend_inputs = malloc(sizeof(void*) * input_count);
        for (int i = 0; i < input_count; i++) {
            backend_inputs[i] = inputs[i] ? inputs[i]->backend_buffer : NULL;
        }
    }
    void** backend_outputs = malloc(sizeof(void*) * output_count);
    for (int i = 0; i < output_count; i++) {
        backend_outputs[i] = outputs[i]->backend_buffer;
    }

    /* Dispatch */
    kernel->device->backend->kernel_dispatch(
        kernel->backend_kernel,
        backend_inputs, input_count,
        backend_outputs, output_count,
        work_size
    );

    if (backend_inputs) free(backend_inputs);
    free(backend_outputs);

    /* Unlock in reverse order */
    for (int i = lock_count - 1; i >= 0; i--) {
        pthread_mutex_unlock(&lock_order[i]->lock);
    }
    pthread_mutex_unlock(&kernel->lock);

    return 0;
}

void mental_kernel_finalize(mental_kernel kernel) {
    if (!kernel) return;

    pthread_mutex_lock(&kernel->lock);
    if (kernel->valid) {
        kernel->device->backend->kernel_destroy(kernel->backend_kernel);
        kernel->valid = 0;
    }
    pthread_mutex_unlock(&kernel->lock);
    pthread_mutex_destroy(&kernel->lock);

    free(kernel);
}

/*
 * Pipe (Chained Kernel Dispatch)
 */

mental_pipe mental_pipe_create(mental_device device) {
    if (!device) {
        mental_set_error(MENTAL_ERROR_INVALID_DEVICE, "NULL device for pipe");
        return NULL;
    }

    if (!device->backend->pipe_create) {
        mental_set_error(MENTAL_ERROR_BACKEND_FAILED, "Backend does not support pipes");
        return NULL;
    }

    mental_pipe pipe = calloc(1, sizeof(struct mental_pipe_t));
    if (!pipe) {
        mental_set_error(MENTAL_ERROR_ALLOCATION_FAILED, "Failed to allocate pipe");
        return NULL;
    }

    pipe->device = device;
    pipe->backend_pipe = device->backend->pipe_create(device->backend_device);
    if (!pipe->backend_pipe) {
        free(pipe);
        mental_set_error(MENTAL_ERROR_BACKEND_FAILED, "Backend pipe creation failed");
        return NULL;
    }

    pipe->valid = 1;
    pipe->dispatch_count = 0;
    pipe->executed = 0;
    pthread_mutex_init(&pipe->lock, NULL);

    return pipe;
}

int mental_pipe_add(mental_pipe pipe, mental_kernel kernel,
                     mental_reference *inputs, int input_count,
                     mental_reference *outputs, int output_count,
                     int work_size) {
    if (!pipe || !pipe->valid || !pipe->backend_pipe) {
        mental_set_error(MENTAL_ERROR_BACKEND_FAILED, "Invalid pipe");
        return -1;
    }
    if (pipe->executed) {
        mental_set_error(MENTAL_ERROR_BACKEND_FAILED,
                         "Pipe already executed — cannot add more dispatches");
        return -1;
    }
    if (!kernel || !kernel->valid) {
        mental_set_error(MENTAL_ERROR_INVALID_KERNEL, "Invalid kernel for pipe");
        return -1;
    }
    if (kernel->device != pipe->device) {
        mental_set_error(MENTAL_ERROR_INVALID_KERNEL,
                         "Kernel device does not match pipe device");
        return -1;
    }

    for (int i = 0; i < input_count; i++) {
        if (inputs[i] && !inputs[i]->backend_buffer) {
            mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Input reference is not pinned");
            return -1;
        }
    }
    for (int i = 0; i < output_count; i++) {
        if (!outputs[i] || !outputs[i]->valid || !outputs[i]->backend_buffer) {
            mental_set_error(MENTAL_ERROR_INVALID_REFERENCE, "Output reference is not pinned");
            return -1;
        }
    }

    /* Collect all unique references for ordered locking */
    mental_reference lock_order[input_count + output_count];
    int lock_count = 0;

    for (int i = 0; i < input_count; i++) {
        if (inputs[i] && inputs[i]->valid) {
            int dup = 0;
            for (int j = 0; j < lock_count; j++) {
                if (lock_order[j] == inputs[i]) { dup = 1; break; }
            }
            if (!dup) lock_order[lock_count++] = inputs[i];
        }
    }
    for (int i = 0; i < output_count; i++) {
        if (outputs[i]->valid) {
            int dup = 0;
            for (int j = 0; j < lock_count; j++) {
                if (lock_order[j] == outputs[i]) { dup = 1; break; }
            }
            if (!dup) lock_order[lock_count++] = outputs[i];
        }
    }

    /* Sort by pointer address to prevent deadlock */
    for (int i = 0; i < lock_count - 1; i++) {
        for (int j = 0; j < lock_count - 1 - i; j++) {
            if ((uintptr_t)lock_order[j] > (uintptr_t)lock_order[j + 1]) {
                mental_reference tmp = lock_order[j];
                lock_order[j] = lock_order[j + 1];
                lock_order[j + 1] = tmp;
            }
        }
    }

    /* Lock kernel, then pipe, then references in sorted order */
    pthread_mutex_lock(&kernel->lock);
    pthread_mutex_lock(&pipe->lock);
    for (int i = 0; i < lock_count; i++) {
        pthread_mutex_lock(&lock_order[i]->lock);
    }

    /* Gather backend buffer handles under lock */
    void **backend_inputs = NULL;
    if (input_count > 0) {
        backend_inputs = malloc(sizeof(void*) * input_count);
        if (!backend_inputs) {
            for (int i = lock_count - 1; i >= 0; i--)
                pthread_mutex_unlock(&lock_order[i]->lock);
            pthread_mutex_unlock(&pipe->lock);
            pthread_mutex_unlock(&kernel->lock);
            return -1;
        }
        for (int i = 0; i < input_count; i++) {
            backend_inputs[i] = inputs[i] ? inputs[i]->backend_buffer : NULL;
        }
    }
    void **backend_outputs = malloc(sizeof(void*) * output_count);
    for (int i = 0; i < output_count; i++) {
        backend_outputs[i] = outputs[i]->backend_buffer;
    }

    int rc = pipe->device->backend->pipe_add(
        pipe->backend_pipe,
        kernel->backend_kernel,
        backend_inputs, input_count,
        backend_outputs, output_count,
        work_size
    );
    if (rc == 0) pipe->dispatch_count++;

    /* Unlock in reverse order */
    for (int i = lock_count - 1; i >= 0; i--) {
        pthread_mutex_unlock(&lock_order[i]->lock);
    }
    pthread_mutex_unlock(&pipe->lock);
    pthread_mutex_unlock(&kernel->lock);

    if (backend_inputs) free(backend_inputs);
    free(backend_outputs);
    return rc;
}

int mental_pipe_execute(mental_pipe pipe) {
    if (!pipe || !pipe->valid || !pipe->backend_pipe) {
        mental_set_error(MENTAL_ERROR_BACKEND_FAILED, "Invalid pipe");
        return -1;
    }
    if (pipe->dispatch_count == 0) {
        return 0; /* nothing to execute */
    }

    pthread_mutex_lock(&pipe->lock);
    int rc = pipe->device->backend->pipe_execute(pipe->backend_pipe);
    pipe->executed = 1;
    pthread_mutex_unlock(&pipe->lock);

    return rc;
}

void mental_pipe_finalize(mental_pipe pipe) {
    if (!pipe) return;

    pthread_mutex_lock(&pipe->lock);
    if (pipe->backend_pipe && pipe->device->backend->pipe_destroy) {
        pipe->device->backend->pipe_destroy(pipe->backend_pipe);
    }
    pipe->valid = 0;
    pthread_mutex_unlock(&pipe->lock);
    pthread_mutex_destroy(&pipe->lock);

    free(pipe);
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

    pthread_mutex_lock(&viewport->lock);
    if (viewport->valid) {
        viewport->reference->device->backend->viewport_detach(viewport->backend_viewport);
        viewport->valid = 0;
    }
    pthread_mutex_unlock(&viewport->lock);
    pthread_mutex_destroy(&viewport->lock);

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
static pthread_mutex_t g_atexit_lock = PTHREAD_MUTEX_INITIALIZER;

static void mental_shutdown_handler(void) {
    /* Run atexit callbacks in LIFO order */
    for (int i = g_atexit_count - 1; i >= 0; i--) {
        if (g_atexit_fns[i]) {
            g_atexit_fns[i]();
        }
    }
}

void mental_atexit(void (*fn)(void)) {
    pthread_mutex_lock(&g_atexit_lock);
    if (!g_shutdown_registered) {
        atexit(mental_shutdown_handler);
        g_shutdown_registered = 1;
    }
    if (g_atexit_count < MENTAL_MAX_ATEXIT && fn) {
        g_atexit_fns[g_atexit_count++] = fn;
    }
    pthread_mutex_unlock(&g_atexit_lock);
}

int mental_shutdown(void) {
    /* Run atexit callbacks in LIFO order */
    pthread_mutex_lock(&g_atexit_lock);
    for (int i = g_atexit_count - 1; i >= 0; i--) {
        if (g_atexit_fns[i]) {
            g_atexit_fns[i]();
        }
    }
    g_atexit_count = 0;
    pthread_mutex_unlock(&g_atexit_lock);

    /* Shut down the active backend and free device handles */
    pthread_mutex_lock(&g_init_lock);
    if (g_initialized && g_devices && g_device_count > 0) {
        mental_backend *backend = g_devices[0]->backend;

        for (int i = 0; i < g_device_count; i++) {
            if (g_devices[i]) {
                if (g_devices[i]->backend_device && backend->device_destroy) {
                    backend->device_destroy(g_devices[i]->backend_device);
                }
                free(g_devices[i]);
            }
        }
        free(g_devices);
        g_devices = NULL;
        g_device_count = 0;

        if (backend->shutdown) {
            backend->shutdown();
        }

        g_initialized = 0;
    }
    pthread_mutex_unlock(&g_init_lock);

    /* Clear the library registry to prevent stale entries on re-init */
    pthread_mutex_lock(&g_library_lock);
    for (int i = 0; i < g_library_count; i++) {
        if (g_libraries[i].name) free(g_libraries[i].name);
        if (g_libraries[i].version) free(g_libraries[i].version);
    }
    g_library_count = 0;
    pthread_mutex_unlock(&g_library_lock);

    return 0;
}
