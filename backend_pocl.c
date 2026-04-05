/*
 * Mental - PoCL Backend (Portable Computing Language)
 *
 * Absolute last-resort fallback: CPU-only OpenCL compute via PoCL.
 * Only activates when every other backend (Metal/D3D12/Vulkan, system
 * OpenCL, OpenGL 4.3+) has failed — meaning the system has no GPU
 * drivers and no system OpenCL runtime installed at all.
 *
 * PoCL is loaded dynamically (dlopen / LoadLibrary) from either:
 *   1. A path set via mental_set_tool_path(MENTAL_TOOL_POCL, "/path/to/libpocl.so")
 *   2. The bundled redistributable in external/pocl/
 *   3. The system search path as a final attempt
 *
 * This backend uses the OpenCL API surface directly through resolved
 * function pointers — it does NOT depend on a system OpenCL ICD loader.
 */

#ifdef MENTAL_HAS_POCL

#ifdef _WIN32
#  include <windows.h>
#  define POCL_DLOPEN(path)    LoadLibraryA(path)
#  define POCL_DLSYM(lib, sym) GetProcAddress((HMODULE)(lib), sym)
#  define POCL_DLCLOSE(lib)    FreeLibrary((HMODULE)(lib))
#  define POCL_LIB_NAME        "pocl.dll"
#else
#  include <dlfcn.h>
#  define POCL_DLOPEN(path)    dlopen(path, RTLD_LAZY)
#  define POCL_DLSYM(lib, sym) dlsym(lib, sym)
#  define POCL_DLCLOSE(lib)    dlclose(lib)
#  ifdef __APPLE__
#    define POCL_LIB_NAME      "libpocl.dylib"
#  else
#    define POCL_LIB_NAME      "libpocl.so"
#  endif
#endif

/* Bring in the CL type definitions without linking the system loader.
 * We only need the types and constants — all functions are resolved
 * through our own function pointers below. */
#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#include "mental_internal.h"
#include "transpile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  OpenCL function pointer types (subset we need)                    */
/* ------------------------------------------------------------------ */

typedef cl_int      (CL_API_CALL *pfn_clGetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*);
typedef cl_int      (CL_API_CALL *pfn_clGetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id*, cl_uint*);
typedef cl_int      (CL_API_CALL *pfn_clGetDeviceInfo)(cl_device_id, cl_device_info, size_t, void*, size_t*);
typedef cl_context  (CL_API_CALL *pfn_clCreateContext)(const cl_context_properties*, cl_uint, const cl_device_id*, void (CL_CALLBACK*)(const char*, const void*, size_t, void*), void*, cl_int*);
typedef cl_int      (CL_API_CALL *pfn_clReleaseContext)(cl_context);
typedef cl_command_queue (CL_API_CALL *pfn_clCreateCommandQueue)(cl_context, cl_device_id, cl_command_queue_properties, cl_int*);
typedef cl_int      (CL_API_CALL *pfn_clReleaseCommandQueue)(cl_command_queue);
typedef cl_mem      (CL_API_CALL *pfn_clCreateBuffer)(cl_context, cl_mem_flags, size_t, void*, cl_int*);
typedef cl_int      (CL_API_CALL *pfn_clReleaseMemObject)(cl_mem);
typedef cl_int      (CL_API_CALL *pfn_clEnqueueWriteBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, const void*, cl_uint, const cl_event*, cl_event*);
typedef cl_int      (CL_API_CALL *pfn_clEnqueueReadBuffer)(cl_command_queue, cl_mem, cl_bool, size_t, size_t, void*, cl_uint, const cl_event*, cl_event*);
typedef cl_int      (CL_API_CALL *pfn_clEnqueueCopyBuffer)(cl_command_queue, cl_mem, cl_mem, size_t, size_t, size_t, cl_uint, const cl_event*, cl_event*);
typedef cl_int      (CL_API_CALL *pfn_clFinish)(cl_command_queue);
typedef cl_program  (CL_API_CALL *pfn_clCreateProgramWithSource)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
typedef cl_int      (CL_API_CALL *pfn_clBuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*, void (CL_CALLBACK*)(cl_program, void*), void*);
typedef cl_int      (CL_API_CALL *pfn_clGetProgramBuildInfo)(cl_program, cl_device_id, cl_program_build_info, size_t, void*, size_t*);
typedef cl_int      (CL_API_CALL *pfn_clGetProgramInfo)(cl_program, cl_program_info, size_t, void*, size_t*);
typedef cl_int      (CL_API_CALL *pfn_clReleaseProgram)(cl_program);
typedef cl_kernel   (CL_API_CALL *pfn_clCreateKernel)(cl_program, const char*, cl_int*);
typedef cl_int      (CL_API_CALL *pfn_clSetKernelArg)(cl_kernel, cl_uint, size_t, const void*);
typedef cl_int      (CL_API_CALL *pfn_clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*, const size_t*, const size_t*, cl_uint, const cl_event*, cl_event*);
typedef cl_int      (CL_API_CALL *pfn_clReleaseKernel)(cl_kernel);

/* ------------------------------------------------------------------ */
/*  Resolved function pointers                                        */
/* ------------------------------------------------------------------ */

static pfn_clGetPlatformIDs          p_clGetPlatformIDs;
static pfn_clGetDeviceIDs            p_clGetDeviceIDs;
static pfn_clGetDeviceInfo           p_clGetDeviceInfo;
static pfn_clCreateContext           p_clCreateContext;
static pfn_clReleaseContext          p_clReleaseContext;
static pfn_clCreateCommandQueue      p_clCreateCommandQueue;
static pfn_clReleaseCommandQueue     p_clReleaseCommandQueue;
static pfn_clCreateBuffer            p_clCreateBuffer;
static pfn_clReleaseMemObject        p_clReleaseMemObject;
static pfn_clEnqueueWriteBuffer      p_clEnqueueWriteBuffer;
static pfn_clEnqueueReadBuffer       p_clEnqueueReadBuffer;
static pfn_clEnqueueCopyBuffer       p_clEnqueueCopyBuffer;
static pfn_clFinish                  p_clFinish;
static pfn_clCreateProgramWithSource p_clCreateProgramWithSource;
static pfn_clBuildProgram            p_clBuildProgram;
static pfn_clGetProgramBuildInfo     p_clGetProgramBuildInfo;
static pfn_clGetProgramInfo          p_clGetProgramInfo;
static pfn_clReleaseProgram          p_clReleaseProgram;
static pfn_clCreateKernel            p_clCreateKernel;
static pfn_clSetKernelArg            p_clSetKernelArg;
static pfn_clEnqueueNDRangeKernel    p_clEnqueueNDRangeKernel;
static pfn_clReleaseKernel           p_clReleaseKernel;

/* ------------------------------------------------------------------ */
/*  Dynamic loader                                                    */
/* ------------------------------------------------------------------ */

static void* g_pocl_lib = NULL;

static int load_pocl_symbols(void) {
#define LOAD(ptr, name) do { \
    *(void**)(&ptr) = (void*)POCL_DLSYM(g_pocl_lib, #name); \
    if (!ptr) return -1; \
} while (0)

    LOAD(p_clGetPlatformIDs,          clGetPlatformIDs);
    LOAD(p_clGetDeviceIDs,            clGetDeviceIDs);
    LOAD(p_clGetDeviceInfo,           clGetDeviceInfo);
    LOAD(p_clCreateContext,           clCreateContext);
    LOAD(p_clReleaseContext,          clReleaseContext);
    LOAD(p_clCreateCommandQueue,      clCreateCommandQueue);
    LOAD(p_clReleaseCommandQueue,     clReleaseCommandQueue);
    LOAD(p_clCreateBuffer,            clCreateBuffer);
    LOAD(p_clReleaseMemObject,        clReleaseMemObject);
    LOAD(p_clEnqueueWriteBuffer,      clEnqueueWriteBuffer);
    LOAD(p_clEnqueueReadBuffer,       clEnqueueReadBuffer);
    LOAD(p_clEnqueueCopyBuffer,       clEnqueueCopyBuffer);
    LOAD(p_clFinish,                  clFinish);
    LOAD(p_clCreateProgramWithSource, clCreateProgramWithSource);
    LOAD(p_clBuildProgram,            clBuildProgram);
    LOAD(p_clGetProgramBuildInfo,     clGetProgramBuildInfo);
    LOAD(p_clGetProgramInfo,          clGetProgramInfo);
    LOAD(p_clReleaseProgram,          clReleaseProgram);
    LOAD(p_clCreateKernel,            clCreateKernel);
    LOAD(p_clSetKernelArg,            clSetKernelArg);
    LOAD(p_clEnqueueNDRangeKernel,    clEnqueueNDRangeKernel);
    LOAD(p_clReleaseKernel,           clReleaseKernel);

#undef LOAD
    return 0;
}

/* Try to load PoCL from a series of candidate paths */
static int open_pocl_library(void) {
    const char* candidates[4];
    int n = 0;

    /* 1. User-configured path via mental_set_tool_path(MENTAL_TOOL_POCL, ...) */
    const char* user_path = mental_get_tool_path(MENTAL_TOOL_POCL);
    if (user_path) candidates[n++] = user_path;

    /* 2. Bundled redistributable next to the library */
    /* (built-in path set by CMake at compile time) */
#ifdef MENTAL_POCL_LIB_PATH
    candidates[n++] = MENTAL_POCL_LIB_PATH;
#endif

    /* 3. System search path */
    candidates[n++] = POCL_LIB_NAME;

    for (int i = 0; i < n; i++) {
        g_pocl_lib = POCL_DLOPEN(candidates[i]);
        if (g_pocl_lib && load_pocl_symbols() == 0) return 0;
        if (g_pocl_lib) { POCL_DLCLOSE(g_pocl_lib); g_pocl_lib = NULL; }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/*  Device state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    cl_context context;
    cl_command_queue queue;
    cl_device_id device_id;
} PoclDevice;

typedef struct {
    cl_mem buffer;
    cl_command_queue queue;
} PoclBuffer;

typedef struct {
    cl_kernel kernel;
    cl_command_queue queue;
} PoclKernel;

static cl_platform_id g_platform = NULL;
static cl_device_id*  g_devices = NULL;
static cl_uint        g_device_count = 0;

/* ------------------------------------------------------------------ */
/*  Backend interface                                                 */
/* ------------------------------------------------------------------ */

static int pocl_init(void) {
    if (open_pocl_library() != 0) return -1;

    cl_int err;
    cl_uint platform_count;
    err = p_clGetPlatformIDs(1, &g_platform, &platform_count);
    if (err != CL_SUCCESS || platform_count == 0) return -1;

    /* PoCL provides CPU devices */
    err = p_clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_CPU, 0, NULL, &g_device_count);
    if (err != CL_SUCCESS || g_device_count == 0) {
        /* Try ALL as final fallback */
        err = p_clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_ALL, 0, NULL, &g_device_count);
        if (err != CL_SUCCESS || g_device_count == 0) return -1;
    }

    g_devices = malloc(sizeof(cl_device_id) * g_device_count);
    if (!g_devices) return -1;

    err = p_clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_CPU, g_device_count, g_devices, NULL);
    if (err != CL_SUCCESS) {
        err = p_clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_ALL, g_device_count, g_devices, NULL);
        if (err != CL_SUCCESS) {
            free(g_devices); g_devices = NULL;
            return -1;
        }
    }

    return 0;
}

static void pocl_shutdown(void) {
    if (g_devices) { free(g_devices); g_devices = NULL; }
    g_device_count = 0;
    if (g_pocl_lib) { POCL_DLCLOSE(g_pocl_lib); g_pocl_lib = NULL; }
}

static int pocl_device_count(void) { return (int)g_device_count; }

static int pocl_device_info(int index, char* name, size_t name_len) {
    if (index < 0 || index >= (int)g_device_count) return -1;
    cl_int err = p_clGetDeviceInfo(g_devices[index], CL_DEVICE_NAME, name_len, name, NULL);
    if (err == CL_SUCCESS) {
        /* Prefix with "PoCL:" so users know this is the software fallback */
        char tmp[256];
        snprintf(tmp, sizeof(tmp), "PoCL: %s", name);
        strncpy(name, tmp, name_len);
        name[name_len - 1] = '\0';
    }
    return (err == CL_SUCCESS) ? 0 : -1;
}

static void* pocl_device_create(int index) {
    if (index < 0 || index >= (int)g_device_count) return NULL;

    PoclDevice* dev = malloc(sizeof(PoclDevice));
    if (!dev) return NULL;

    cl_int err;
    dev->device_id = g_devices[index];

    dev->context = p_clCreateContext(NULL, 1, &dev->device_id, NULL, NULL, &err);
    if (err != CL_SUCCESS) { free(dev); return NULL; }

    dev->queue = p_clCreateCommandQueue(dev->context, dev->device_id, 0, &err);
    if (err != CL_SUCCESS) { p_clReleaseContext(dev->context); free(dev); return NULL; }

    return dev;
}

static void pocl_device_destroy(void* dev) {
    if (!dev) return;
    PoclDevice* d = (PoclDevice*)dev;
    p_clReleaseCommandQueue(d->queue);
    p_clReleaseContext(d->context);
    free(d);
}

/* ------------------------------------------------------------------ */
/*  Buffer operations                                                 */
/* ------------------------------------------------------------------ */

static void* pocl_buffer_alloc(void* dev, size_t bytes) {
    PoclDevice* d = (PoclDevice*)dev;
    cl_int err;
    cl_mem buffer = p_clCreateBuffer(d->context, CL_MEM_READ_WRITE, bytes, NULL, &err);
    if (err != CL_SUCCESS) return NULL;

    PoclBuffer* buf = malloc(sizeof(PoclBuffer));
    if (!buf) { p_clReleaseMemObject(buffer); return NULL; }
    buf->buffer = buffer;
    buf->queue = d->queue;
    return buf;
}

static void pocl_buffer_write(void* buf, const void* data, size_t bytes) {
    PoclBuffer* b = (PoclBuffer*)buf;
    p_clEnqueueWriteBuffer(b->queue, b->buffer, CL_TRUE, 0, bytes, data, 0, NULL, NULL);
}

static void pocl_buffer_read(void* buf, void* data, size_t bytes) {
    PoclBuffer* b = (PoclBuffer*)buf;
    p_clEnqueueReadBuffer(b->queue, b->buffer, CL_TRUE, 0, bytes, data, 0, NULL, NULL);
}

static void* pocl_buffer_resize(void* dev, void* old_buf, size_t old_size, size_t new_size) {
    PoclDevice* d = (PoclDevice*)dev;
    PoclBuffer* old = (PoclBuffer*)old_buf;

    cl_int err;
    cl_mem new_buffer = p_clCreateBuffer(d->context, CL_MEM_READ_WRITE, new_size, NULL, &err);
    if (err != CL_SUCCESS) return NULL;

    size_t copy_size = old_size < new_size ? old_size : new_size;
    p_clEnqueueCopyBuffer(d->queue, old->buffer, new_buffer, 0, 0, copy_size, 0, NULL, NULL);
    p_clFinish(d->queue);

    p_clReleaseMemObject(old->buffer);
    old->buffer = new_buffer;
    return old_buf;
}

static void* pocl_buffer_clone(void* dev, void* src_buf, size_t size) {
    PoclDevice* d = (PoclDevice*)dev;
    PoclBuffer* src = (PoclBuffer*)src_buf;

    cl_int err;
    cl_mem new_buffer = p_clCreateBuffer(d->context, CL_MEM_READ_WRITE, size, NULL, &err);
    if (err != CL_SUCCESS) return NULL;

    p_clEnqueueCopyBuffer(d->queue, src->buffer, new_buffer, 0, 0, size, 0, NULL, NULL);
    p_clFinish(d->queue);

    PoclBuffer* clone = malloc(sizeof(PoclBuffer));
    if (!clone) { p_clReleaseMemObject(new_buffer); return NULL; }
    clone->buffer = new_buffer;
    clone->queue = d->queue;
    return clone;
}

static void pocl_buffer_destroy(void* buf) {
    if (!buf) return;
    PoclBuffer* b = (PoclBuffer*)buf;
    p_clReleaseMemObject(b->buffer);
    free(b);
}

/* ------------------------------------------------------------------ */
/*  Kernel operations                                                 */
/* ------------------------------------------------------------------ */

static void* pocl_kernel_compile(void* dev, const char* source, size_t source_len,
                                  char* error, size_t error_len) {
    PoclDevice* d = (PoclDevice*)dev;

    cl_int err;
    cl_program program = p_clCreateProgramWithSource(d->context, 1, &source, &source_len, &err);
    if (err != CL_SUCCESS) {
        if (error) snprintf(error, error_len, "PoCL: failed to create program");
        return NULL;
    }

    err = p_clBuildProgram(program, 1, &d->device_id, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        if (error) p_clGetProgramBuildInfo(program, d->device_id, CL_PROGRAM_BUILD_LOG, error_len, error, NULL);
        p_clReleaseProgram(program);
        return NULL;
    }

    cl_kernel kernel = p_clCreateKernel(program, "main", &err);
    if (err != CL_SUCCESS) {
        size_t names_size;
        p_clGetProgramInfo(program, CL_PROGRAM_KERNEL_NAMES, 0, NULL, &names_size);
        if (names_size > 0) {
            char* names = malloc(names_size);
            p_clGetProgramInfo(program, CL_PROGRAM_KERNEL_NAMES, names_size, names, NULL);
            char* semicolon = strchr(names, ';');
            if (semicolon) *semicolon = '\0';
            kernel = p_clCreateKernel(program, names, &err);
            free(names);
        }
        if (err != CL_SUCCESS) {
            if (error) snprintf(error, error_len, "PoCL: failed to create kernel");
            p_clReleaseProgram(program);
            return NULL;
        }
    }

    p_clReleaseProgram(program);

    PoclKernel* k = malloc(sizeof(PoclKernel));
    if (!k) { p_clReleaseKernel(kernel); return NULL; }
    k->kernel = kernel;
    k->queue = d->queue;
    return k;
}

static int pocl_kernel_workgroup_size(void* kernel) {
    (void)kernel;
    /* Return 0 to indicate the PoCL runtime should choose the
     * workgroup size (NULL local_work_size in clEnqueueNDRangeKernel). */
    return 0;
}

static void pocl_kernel_dispatch(void* kernel, void** inputs, int input_count,
                                  void* output, int work_size) {
    PoclKernel* k = (PoclKernel*)kernel;

    for (int i = 0; i < input_count; i++) {
        if (inputs[i]) {
            PoclBuffer* buf = (PoclBuffer*)inputs[i];
            p_clSetKernelArg(k->kernel, i, sizeof(cl_mem), &buf->buffer);
        }
    }

    PoclBuffer* out = (PoclBuffer*)output;
    p_clSetKernelArg(k->kernel, input_count, sizeof(cl_mem), &out->buffer);

    size_t global_work_size = work_size;
    p_clEnqueueNDRangeKernel(k->queue, k->kernel, 1, NULL, &global_work_size, NULL, 0, NULL, NULL);
    p_clFinish(k->queue);
}

static void pocl_kernel_destroy(void* kernel) {
    if (!kernel) return;
    PoclKernel* k = (PoclKernel*)kernel;
    p_clReleaseKernel(k->kernel);
    free(k);
}

/* ── Pipe ──────────────────────────────────────────────────────── */

typedef struct {
    cl_command_queue queue;
} PoclPipe;

static void* pocl_pipe_create(void* dev) {
    PoclDevice* d = (PoclDevice*)dev;

    PoclPipe* pipe = malloc(sizeof(PoclPipe));
    if (!pipe) return NULL;
    pipe->queue = d->queue;

    return pipe;
}

static int pocl_pipe_add(void* pipe_ptr, void* kernel, void** inputs,
                          int input_count, void* output, int work_size) {
    PoclPipe* pipe = (PoclPipe*)pipe_ptr;
    PoclKernel* k = (PoclKernel*)kernel;

    /* Set kernel arguments */
    for (int i = 0; i < input_count; i++) {
        if (inputs[i]) {
            PoclBuffer* buf = (PoclBuffer*)inputs[i];
            p_clSetKernelArg(k->kernel, i, sizeof(cl_mem), &buf->buffer);
        }
    }

    PoclBuffer* out = (PoclBuffer*)output;
    p_clSetKernelArg(k->kernel, input_count, sizeof(cl_mem), &out->buffer);

    /* Enqueue without waiting */
    size_t global_work_size = work_size;
    p_clEnqueueNDRangeKernel(pipe->queue, k->kernel, 1, NULL,
                              &global_work_size, NULL, 0, NULL, NULL);

    return 0;
}

static int pocl_pipe_execute(void* pipe_ptr) {
    PoclPipe* pipe = (PoclPipe*)pipe_ptr;
    p_clFinish(pipe->queue);
    return 0;
}

static void pocl_pipe_destroy(void* pipe_ptr) {
    if (pipe_ptr) free(pipe_ptr);
}

/* ------------------------------------------------------------------ */
/*  Backend descriptor                                                */
/* ------------------------------------------------------------------ */

static mental_backend g_pocl_backend = {
    .name = "PoCL",
    .api = MENTAL_API_POCL,
    .init = pocl_init,
    .shutdown = pocl_shutdown,
    .device_count = pocl_device_count,
    .device_info = pocl_device_info,
    .device_create = pocl_device_create,
    .device_destroy = pocl_device_destroy,
    .buffer_alloc = pocl_buffer_alloc,
    .buffer_write = pocl_buffer_write,
    .buffer_read = pocl_buffer_read,
    .buffer_resize = pocl_buffer_resize,
    .buffer_clone = pocl_buffer_clone,
    .buffer_destroy = pocl_buffer_destroy,
    .kernel_compile = pocl_kernel_compile,
    .kernel_workgroup_size = pocl_kernel_workgroup_size,
    .kernel_dispatch = pocl_kernel_dispatch,
    .kernel_destroy = pocl_kernel_destroy,
    .pipe_create = pocl_pipe_create,
    .pipe_add = pocl_pipe_add,
    .pipe_execute = pocl_pipe_execute,
    .pipe_destroy = pocl_pipe_destroy,
    .viewport_attach = NULL,
    .viewport_present = NULL,
    .viewport_detach = NULL
};

mental_backend* pocl_backend = &g_pocl_backend;

#else
/* PoCL not available at build time */
#include "mental_internal.h"
#include <stddef.h>
mental_backend* pocl_backend = NULL;
#endif /* MENTAL_HAS_POCL */
