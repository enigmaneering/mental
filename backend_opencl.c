/*
 * Mental - OpenCL Backend (System ICD Loader)
 *
 * Uses the system-installed OpenCL ICD loader (e.g. ocl-icd on Linux,
 * the OpenCL.framework on macOS, OpenCL.dll on Windows).
 * The library is loaded dynamically via dlopen / LoadLibrary so that
 * the backend is always compilable — it simply fails gracefully at
 * runtime if no OpenCL runtime is present.
 */

#ifdef _WIN32
#  include <windows.h>
#  define CL_DLOPEN(path)    LoadLibraryA(path)
#  define CL_DLSYM(lib, sym) GetProcAddress((HMODULE)(lib), sym)
#  define CL_DLCLOSE(lib)    FreeLibrary((HMODULE)(lib))
#else
#  include <dlfcn.h>
#  define CL_DLOPEN(path)    dlopen(path, RTLD_LAZY)
#  define CL_DLSYM(lib, sym) dlsym(lib, sym)
#  define CL_DLCLOSE(lib)    dlclose(lib)
#endif

/* Bring in the CL type definitions without linking the system loader.
 * We only need the types and constants — all functions are resolved
 * through our own function pointers below.
 * If headers aren't available (e.g. WASM), export opencl_backend = NULL. */
#ifdef __APPLE__
#  if __has_include(<OpenCL/opencl.h>)
#    include <OpenCL/opencl.h>
#    define MENTAL_OPENCL_AVAILABLE 1
#  endif
#else
#  if __has_include(<CL/cl.h>)
#    include <CL/cl.h>
#    define MENTAL_OPENCL_AVAILABLE 1
#  endif
#endif

#ifndef MENTAL_OPENCL_AVAILABLE
#  define MENTAL_OPENCL_AVAILABLE 0
#endif

#include "mental_internal.h"
#include "transpile.h"

#if !MENTAL_OPENCL_AVAILABLE
#include <stddef.h>
mental_backend* opencl_backend = NULL;
#else
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
typedef cl_program  (CL_API_CALL *pfn_clCreateProgramWithIL)(cl_context, const void*, size_t, cl_int*);
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
static pfn_clCreateProgramWithIL     p_clCreateProgramWithIL;  /* may be NULL (OpenCL < 2.1) */
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

static void* g_opencl_lib = NULL;

static int load_opencl_symbols(void) {
#define LOAD(ptr, name) do { \
    *(void**)(&ptr) = (void*)CL_DLSYM(g_opencl_lib, #name); \
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

    /* clCreateProgramWithIL is optional (OpenCL 2.1+) — don't fail if absent */
    *(void**)(&p_clCreateProgramWithIL) = (void*)CL_DLSYM(g_opencl_lib, "clCreateProgramWithIL");

    return 0;
}

/* Try to load the system OpenCL ICD loader */
static int open_opencl_library(void) {
    const char* candidates[4];
    int n = 0;

#ifdef _WIN32
    candidates[n++] = "OpenCL.dll";
#elif defined(__APPLE__)
    candidates[n++] = "/System/Library/Frameworks/OpenCL.framework/OpenCL";
#else
    candidates[n++] = "libOpenCL.so.1";
    candidates[n++] = "libOpenCL.so";
#endif

    for (int i = 0; i < n; i++) {
        g_opencl_lib = CL_DLOPEN(candidates[i]);
        if (g_opencl_lib && load_opencl_symbols() == 0) return 0;
        if (g_opencl_lib) { CL_DLCLOSE(g_opencl_lib); g_opencl_lib = NULL; }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/*  Device state                                                      */
/* ------------------------------------------------------------------ */

/* OpenCL device wrapper */
typedef struct {
    cl_context context;
    cl_command_queue queue;
    cl_device_id device_id;
} OpenCLDevice;

/* OpenCL buffer wrapper */
typedef struct {
    cl_mem buffer;
    cl_command_queue queue;
} OpenCLBuffer;

/* OpenCL kernel wrapper */
typedef struct {
    cl_kernel kernel;
    cl_command_queue queue;
} OpenCLKernel;

/* Global OpenCL state */
static cl_platform_id g_platform = NULL;
static cl_device_id* g_devices = NULL;
static cl_uint g_device_count = 0;

/* ------------------------------------------------------------------ */
/*  Backend interface                                                 */
/* ------------------------------------------------------------------ */

static int opencl_init(void) {
    if (open_opencl_library() != 0) return -1;

    cl_int err;

    /* Get first platform */
    cl_uint platform_count;
    err = p_clGetPlatformIDs(1, &g_platform, &platform_count);
    if (err != CL_SUCCESS || platform_count == 0) {
        return -1;
    }

    /* Get GPU devices first; fall back to ALL (including CPU) if no GPU.
     * This covers systems where system OpenCL provides CPU compute.
     * PoCL (further down the chain) is only for targets with no system
     * OpenCL installed at all. */
    err = p_clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_GPU, 0, NULL, &g_device_count);
    if (err != CL_SUCCESS || g_device_count == 0) {
        err = p_clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_ALL, 0, NULL, &g_device_count);
        if (err != CL_SUCCESS || g_device_count == 0) {
            return -1;
        }
    }

    g_devices = malloc(sizeof(cl_device_id) * g_device_count);
    if (!g_devices) return -1;

    err = p_clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_GPU, g_device_count, g_devices, NULL);
    if (err != CL_SUCCESS) {
        err = p_clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_ALL, g_device_count, g_devices, NULL);
        if (err != CL_SUCCESS) {
            free(g_devices);
            g_devices = NULL;
            return -1;
        }
    }

    return 0;
}

static void opencl_shutdown(void) {
    if (g_devices) {
        free(g_devices);
        g_devices = NULL;
    }
    g_device_count = 0;
    if (g_opencl_lib) { CL_DLCLOSE(g_opencl_lib); g_opencl_lib = NULL; }
}

static int opencl_device_count(void) {
    return (int)g_device_count;
}

static int opencl_device_info(int index, char* name, size_t name_len) {
    if (index < 0 || index >= (int)g_device_count) return -1;

    cl_int err = p_clGetDeviceInfo(g_devices[index], CL_DEVICE_NAME,
                                  name_len, name, NULL);
    return (err == CL_SUCCESS) ? 0 : -1;
}

static void* opencl_device_create(int index) {
    if (index < 0 || index >= (int)g_device_count) return NULL;

    OpenCLDevice* dev = malloc(sizeof(OpenCLDevice));
    if (!dev) return NULL;

    cl_int err;
    dev->device_id = g_devices[index];

    /* Create context */
    dev->context = p_clCreateContext(NULL, 1, &dev->device_id, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        free(dev);
        return NULL;
    }

    /* Create command queue */
    dev->queue = p_clCreateCommandQueue(dev->context, dev->device_id, 0, &err);
    if (err != CL_SUCCESS) {
        p_clReleaseContext(dev->context);
        free(dev);
        return NULL;
    }

    return dev;
}

static void opencl_device_destroy(void* dev) {
    if (!dev) return;

    OpenCLDevice* cl_dev = (OpenCLDevice*)dev;
    p_clReleaseCommandQueue(cl_dev->queue);
    p_clReleaseContext(cl_dev->context);
    free(cl_dev);
}

static void* opencl_buffer_alloc(void* dev, size_t bytes) {
    OpenCLDevice* cl_dev = (OpenCLDevice*)dev;

    cl_int err;
    cl_mem buffer = p_clCreateBuffer(cl_dev->context,
                                    CL_MEM_READ_WRITE,
                                    bytes,
                                    NULL,
                                    &err);
    if (err != CL_SUCCESS) return NULL;

    OpenCLBuffer* buf = malloc(sizeof(OpenCLBuffer));
    if (!buf) {
        p_clReleaseMemObject(buffer);
        return NULL;
    }

    buf->buffer = buffer;
    buf->queue = cl_dev->queue;

    return buf;
}

static void opencl_buffer_write(void* buf, const void* data, size_t bytes) {
    OpenCLBuffer* cl_buf = (OpenCLBuffer*)buf;

    p_clEnqueueWriteBuffer(cl_buf->queue,
                         cl_buf->buffer,
                         CL_TRUE,
                         0,
                         bytes,
                         data,
                         0, NULL, NULL);
}

static void opencl_buffer_read(void* buf, void* data, size_t bytes) {
    OpenCLBuffer* cl_buf = (OpenCLBuffer*)buf;

    p_clEnqueueReadBuffer(cl_buf->queue,
                        cl_buf->buffer,
                        CL_TRUE,
                        0,
                        bytes,
                        data,
                        0, NULL, NULL);
}

static void* opencl_buffer_resize(void* dev, void* old_buf, size_t old_size, size_t new_size) {
    OpenCLDevice* cl_dev = (OpenCLDevice*)dev;
    OpenCLBuffer* old_cl_buf = (OpenCLBuffer*)old_buf;

    /* Allocate new buffer */
    cl_int err;
    cl_mem new_buffer = p_clCreateBuffer(cl_dev->context,
                                        CL_MEM_READ_WRITE,
                                        new_size,
                                        NULL,
                                        &err);
    if (err != CL_SUCCESS) return NULL;

    /* Copy old data */
    size_t copy_size = old_size < new_size ? old_size : new_size;
    p_clEnqueueCopyBuffer(cl_dev->queue,
                        old_cl_buf->buffer,
                        new_buffer,
                        0, 0,
                        copy_size,
                        0, NULL, NULL);
    p_clFinish(cl_dev->queue);

    /* Release old buffer */
    p_clReleaseMemObject(old_cl_buf->buffer);
    old_cl_buf->buffer = new_buffer;

    return old_buf;
}

static void* opencl_buffer_clone(void* dev, void* src_buf, size_t size) {
    OpenCLDevice* cl_dev = (OpenCLDevice*)dev;
    OpenCLBuffer* src_cl_buf = (OpenCLBuffer*)src_buf;

    /* Allocate new buffer */
    cl_int err;
    cl_mem new_buffer = p_clCreateBuffer(cl_dev->context,
                                        CL_MEM_READ_WRITE,
                                        size,
                                        NULL,
                                        &err);
    if (err != CL_SUCCESS) return NULL;

    /* Copy data from source buffer */
    p_clEnqueueCopyBuffer(cl_dev->queue,
                        src_cl_buf->buffer,
                        new_buffer,
                        0, 0,
                        size,
                        0, NULL, NULL);
    p_clFinish(cl_dev->queue);

    /* Create new buffer wrapper */
    OpenCLBuffer* clone_buf = malloc(sizeof(OpenCLBuffer));
    if (!clone_buf) {
        p_clReleaseMemObject(new_buffer);
        return NULL;
    }

    clone_buf->buffer = new_buffer;
    clone_buf->queue = cl_dev->queue;

    return clone_buf;
}

static void opencl_buffer_destroy(void* buf) {
    if (!buf) return;

    OpenCLBuffer* cl_buf = (OpenCLBuffer*)buf;
    p_clReleaseMemObject(cl_buf->buffer);
    free(cl_buf);
}

static void* opencl_kernel_compile(void* dev, const char* source, size_t source_len,
                                    char* error, size_t error_len) {
    OpenCLDevice* cl_dev = (OpenCLDevice*)dev;

    cl_int err;
    cl_program program = NULL;

    /* Check if source is SPIR-V binary (magic number 0x07230203). */
    int is_spirv = (source_len >= 4 &&
                    (unsigned char)source[0] == 0x03 &&
                    (unsigned char)source[1] == 0x02 &&
                    (unsigned char)source[2] == 0x23 &&
                    (unsigned char)source[3] == 0x07);

    /* Strategy:
     * 1. If source is already SPIR-V and we have clCreateProgramWithIL → use it
     * 2. Otherwise, transpile GLSL → SPIR-V → GLSL (spirv-cross) → OpenCL C
     *    and use clCreateProgramWithSource with the OpenCL C text.
     *    This path works on any OpenCL implementation, no SPIR-V support needed. */
    if (is_spirv && p_clCreateProgramWithIL) {
        program = p_clCreateProgramWithIL(cl_dev->context, source, source_len, &err);
    } else if (is_spirv) {
        if (error) snprintf(error, error_len, "SPIR-V input requires OpenCL 2.1+ (clCreateProgramWithIL not available)");
        return NULL;
    } else {
        /* Transpile: GLSL → SPIR-V → GLSL (spirv-cross) → OpenCL C */
        size_t spirv_len = 0;
        unsigned char* spirv = mental_glsl_to_spirv(source, source_len,
                                                     &spirv_len, error, error_len);
        if (!spirv) return NULL;

        size_t glsl_out_len = 0;
        char* glsl_out = mental_spirv_to_glsl(spirv, spirv_len,
                                               &glsl_out_len, error, error_len);
        free(spirv);
        if (!glsl_out) return NULL;

        size_t opencl_len = 0;
        char* opencl_src = mental_glsl_to_opencl_c(glsl_out, glsl_out_len,
                                                    &opencl_len, error, error_len);
        free(glsl_out);
        if (!opencl_src) return NULL;

        if (!opencl_src || opencl_len == 0) {
            if (error) snprintf(error, error_len, "OpenCL C transpilation produced empty output");
            return NULL;
        }

        /* Debug: print the generated OpenCL C so CI logs show what's being compiled */
        printf("=== Generated OpenCL C (%zu bytes) ===\n%s=== END ===\n", opencl_len, opencl_src);
        fflush(stdout);

        /* Pass NULL for lengths to let OpenCL auto-detect (null-terminated) */
        const char* src_ptr = opencl_src;
        program = p_clCreateProgramWithSource(cl_dev->context, 1,
                                               &src_ptr,
                                               NULL, &err);
        printf("clCreateProgramWithSource returned: %d\n", err);
        fflush(stdout);
        free(opencl_src);
    }

    if (!program || err != CL_SUCCESS) {
        if (error) snprintf(error, error_len, "Failed to create OpenCL program (err=%d). "
                           "Try running sanity-check for diagnostics.", err);
        return NULL;
    }

    /* Build program */
    err = p_clBuildProgram(program, 1, &cl_dev->device_id, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        if (error) {
            p_clGetProgramBuildInfo(program, cl_dev->device_id, CL_PROGRAM_BUILD_LOG,
                                  error_len, error, NULL);
        }
        p_clReleaseProgram(program);
        return NULL;
    }

    /* Create kernel (assume function named "main" or first kernel) */
    /* Try "mental_compute" first (our OpenCL C transpiler output),
     * then "main" (standard GLSL entry point for non-transpiled sources) */
    cl_kernel kernel = p_clCreateKernel(program, "mental_compute", &err);
    if (err != CL_SUCCESS) {
        kernel = p_clCreateKernel(program, "main", &err);
    }
    if (err != CL_SUCCESS) {
        /* Try to get first kernel name */
        size_t kernel_names_size;
        p_clGetProgramInfo(program, CL_PROGRAM_KERNEL_NAMES, 0, NULL, &kernel_names_size);
        if (kernel_names_size > 0) {
            char* kernel_names = malloc(kernel_names_size);
            p_clGetProgramInfo(program, CL_PROGRAM_KERNEL_NAMES, kernel_names_size,
                            kernel_names, NULL);
            /* Get first kernel name (up to first ';') */
            char* semicolon = strchr(kernel_names, ';');
            if (semicolon) *semicolon = '\0';
            kernel = p_clCreateKernel(program, kernel_names, &err);
            free(kernel_names);
        }

        if (err != CL_SUCCESS) {
            if (error) {
                snprintf(error, error_len, "Failed to create OpenCL kernel");
            }
            p_clReleaseProgram(program);
            return NULL;
        }
    }

    p_clReleaseProgram(program);

    OpenCLKernel* cl_kernel = malloc(sizeof(OpenCLKernel));
    if (!cl_kernel) {
        p_clReleaseKernel(kernel);
        return NULL;
    }

    cl_kernel->kernel = kernel;
    cl_kernel->queue = cl_dev->queue;

    return cl_kernel;
}

static int opencl_kernel_workgroup_size(void* kernel) {
    (void)kernel;
    /* Return 0 to indicate the OpenCL runtime should choose the
     * workgroup size (NULL local_work_size in clEnqueueNDRangeKernel). */
    return 0;
}

static void opencl_kernel_dispatch(void* kernel, void** inputs, int input_count,
                                    void** outputs, int output_count, int work_size) {
    OpenCLKernel* cl_kernel = (OpenCLKernel*)kernel;

    /* Set kernel arguments */
    for (int i = 0; i < input_count; i++) {
        if (inputs[i]) {
            OpenCLBuffer* input_buf = (OpenCLBuffer*)inputs[i];
            p_clSetKernelArg(cl_kernel->kernel, i, sizeof(cl_mem), &input_buf->buffer);
        }
    }

    /* Set output arguments */
    for (int i = 0; i < output_count; i++) {
        OpenCLBuffer* output_buf = (OpenCLBuffer*)outputs[i];
        p_clSetKernelArg(cl_kernel->kernel, input_count + i, sizeof(cl_mem), &output_buf->buffer);
    }

    /* Execute */
    size_t global_work_size = work_size;
    p_clEnqueueNDRangeKernel(cl_kernel->queue,
                           cl_kernel->kernel,
                           1,
                           NULL,
                           &global_work_size,
                           NULL,
                           0, NULL, NULL);
    p_clFinish(cl_kernel->queue);
}

static void opencl_kernel_destroy(void* kernel) {
    if (!kernel) return;

    OpenCLKernel* cl_kernel = (OpenCLKernel*)kernel;
    p_clReleaseKernel(cl_kernel->kernel);
    free(cl_kernel);
}

/* ── Pipe ─────────────────────────────────────────────────────────── */

typedef struct {
    cl_command_queue queue;
} OpenCLPipe;

static void* opencl_pipe_create(void* dev) {
    OpenCLDevice* cl_dev = (OpenCLDevice*)dev;

    OpenCLPipe* pipe = malloc(sizeof(OpenCLPipe));
    if (!pipe) return NULL;
    pipe->queue = cl_dev->queue;

    return pipe;
}

static int opencl_pipe_add(void* pipe_ptr, void* kernel, void** inputs,
                            int input_count, void** outputs, int output_count,
                            int work_size) {
    OpenCLPipe* pipe = (OpenCLPipe*)pipe_ptr;
    OpenCLKernel* cl_kernel = (OpenCLKernel*)kernel;

    /* Set kernel arguments */
    for (int i = 0; i < input_count; i++) {
        if (inputs[i]) {
            OpenCLBuffer* input_buf = (OpenCLBuffer*)inputs[i];
            p_clSetKernelArg(cl_kernel->kernel, i, sizeof(cl_mem), &input_buf->buffer);
        }
    }

    /* Set output arguments */
    for (int i = 0; i < output_count; i++) {
        OpenCLBuffer* output_buf = (OpenCLBuffer*)outputs[i];
        p_clSetKernelArg(cl_kernel->kernel, input_count + i, sizeof(cl_mem), &output_buf->buffer);
    }

    /* Enqueue without waiting */
    size_t global_work_size = work_size;
    p_clEnqueueNDRangeKernel(pipe->queue,
                           cl_kernel->kernel,
                           1,
                           NULL,
                           &global_work_size,
                           NULL,
                           0, NULL, NULL);

    return 0;
}

static int opencl_pipe_execute(void* pipe_ptr) {
    OpenCLPipe* pipe = (OpenCLPipe*)pipe_ptr;
    p_clFinish(pipe->queue);
    return 0;
}

static void opencl_pipe_destroy(void* pipe_ptr) {
    if (pipe_ptr) free(pipe_ptr);
}

/* Backend implementation */
static mental_backend g_opencl_backend = {
    .name = "OpenCL",
    .api = MENTAL_API_OPENCL,
    .init = opencl_init,
    .shutdown = opencl_shutdown,
    .device_count = opencl_device_count,
    .device_info = opencl_device_info,
    .device_create = opencl_device_create,
    .device_destroy = opencl_device_destroy,
    .buffer_alloc = opencl_buffer_alloc,
    .buffer_write = opencl_buffer_write,
    .buffer_read = opencl_buffer_read,
    .buffer_resize = opencl_buffer_resize,
    .buffer_clone = opencl_buffer_clone,
    .buffer_destroy = opencl_buffer_destroy,
    .kernel_compile = opencl_kernel_compile,
    .kernel_workgroup_size = opencl_kernel_workgroup_size,
    .kernel_dispatch = opencl_kernel_dispatch,
    .kernel_destroy = opencl_kernel_destroy,
    .pipe_create = opencl_pipe_create,
    .pipe_add = opencl_pipe_add,
    .pipe_execute = opencl_pipe_execute,
    .pipe_destroy = opencl_pipe_destroy,
    .viewport_attach = NULL,
    .viewport_present = NULL,
    .viewport_detach = NULL
};

mental_backend* opencl_backend = &g_opencl_backend;

#endif /* MENTAL_OPENCL_AVAILABLE */
