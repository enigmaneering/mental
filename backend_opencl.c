/*
 * Mental - OpenCL Backend (Fallback for all platforms)
 */

#ifdef MENTAL_HAS_OPENCL

#ifdef __APPLE__
#include <OpenCL/opencl.h>
#else
#include <CL/cl.h>
#endif

#include "mental_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static int opencl_init(void) {
    cl_int err;

    /* Get first platform */
    cl_uint platform_count;
    err = clGetPlatformIDs(1, &g_platform, &platform_count);
    if (err != CL_SUCCESS || platform_count == 0) {
        return -1;
    }

    /* Get GPU devices first; fall back to ALL (including CPU) if no GPU.
     * This covers systems where system OpenCL provides CPU compute.
     * PoCL (further down the chain) is only for targets with no system
     * OpenCL installed at all. */
    err = clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_GPU, 0, NULL, &g_device_count);
    if (err != CL_SUCCESS || g_device_count == 0) {
        err = clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_ALL, 0, NULL, &g_device_count);
        if (err != CL_SUCCESS || g_device_count == 0) {
            return -1;
        }
    }

    g_devices = malloc(sizeof(cl_device_id) * g_device_count);
    if (!g_devices) return -1;

    err = clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_GPU, g_device_count, g_devices, NULL);
    if (err != CL_SUCCESS) {
        err = clGetDeviceIDs(g_platform, CL_DEVICE_TYPE_ALL, g_device_count, g_devices, NULL);
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
}

static int opencl_device_count(void) {
    return (int)g_device_count;
}

static int opencl_device_info(int index, char* name, size_t name_len) {
    if (index < 0 || index >= (int)g_device_count) return -1;

    cl_int err = clGetDeviceInfo(g_devices[index], CL_DEVICE_NAME,
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
    dev->context = clCreateContext(NULL, 1, &dev->device_id, NULL, NULL, &err);
    if (err != CL_SUCCESS) {
        free(dev);
        return NULL;
    }

    /* Create command queue */
    dev->queue = clCreateCommandQueue(dev->context, dev->device_id, 0, &err);
    if (err != CL_SUCCESS) {
        clReleaseContext(dev->context);
        free(dev);
        return NULL;
    }

    return dev;
}

static void opencl_device_destroy(void* dev) {
    if (!dev) return;

    OpenCLDevice* cl_dev = (OpenCLDevice*)dev;
    clReleaseCommandQueue(cl_dev->queue);
    clReleaseContext(cl_dev->context);
    free(cl_dev);
}

static void* opencl_buffer_alloc(void* dev, size_t bytes) {
    OpenCLDevice* cl_dev = (OpenCLDevice*)dev;

    cl_int err;
    cl_mem buffer = clCreateBuffer(cl_dev->context,
                                    CL_MEM_READ_WRITE,
                                    bytes,
                                    NULL,
                                    &err);
    if (err != CL_SUCCESS) return NULL;

    OpenCLBuffer* buf = malloc(sizeof(OpenCLBuffer));
    if (!buf) {
        clReleaseMemObject(buffer);
        return NULL;
    }

    buf->buffer = buffer;
    buf->queue = cl_dev->queue;

    return buf;
}

static void opencl_buffer_write(void* buf, const void* data, size_t bytes) {
    OpenCLBuffer* cl_buf = (OpenCLBuffer*)buf;

    clEnqueueWriteBuffer(cl_buf->queue,
                         cl_buf->buffer,
                         CL_TRUE,
                         0,
                         bytes,
                         data,
                         0, NULL, NULL);
}

static void opencl_buffer_read(void* buf, void* data, size_t bytes) {
    OpenCLBuffer* cl_buf = (OpenCLBuffer*)buf;

    clEnqueueReadBuffer(cl_buf->queue,
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
    cl_mem new_buffer = clCreateBuffer(cl_dev->context,
                                        CL_MEM_READ_WRITE,
                                        new_size,
                                        NULL,
                                        &err);
    if (err != CL_SUCCESS) return NULL;

    /* Copy old data */
    size_t copy_size = old_size < new_size ? old_size : new_size;
    clEnqueueCopyBuffer(cl_dev->queue,
                        old_cl_buf->buffer,
                        new_buffer,
                        0, 0,
                        copy_size,
                        0, NULL, NULL);
    clFinish(cl_dev->queue);

    /* Release old buffer */
    clReleaseMemObject(old_cl_buf->buffer);
    old_cl_buf->buffer = new_buffer;

    return old_buf;
}

static void* opencl_buffer_clone(void* dev, void* src_buf, size_t size) {
    OpenCLDevice* cl_dev = (OpenCLDevice*)dev;
    OpenCLBuffer* src_cl_buf = (OpenCLBuffer*)src_buf;

    /* Allocate new buffer */
    cl_int err;
    cl_mem new_buffer = clCreateBuffer(cl_dev->context,
                                        CL_MEM_READ_WRITE,
                                        size,
                                        NULL,
                                        &err);
    if (err != CL_SUCCESS) return NULL;

    /* Copy data from source buffer */
    clEnqueueCopyBuffer(cl_dev->queue,
                        src_cl_buf->buffer,
                        new_buffer,
                        0, 0,
                        size,
                        0, NULL, NULL);
    clFinish(cl_dev->queue);

    /* Create new buffer wrapper */
    OpenCLBuffer* clone_buf = malloc(sizeof(OpenCLBuffer));
    if (!clone_buf) {
        clReleaseMemObject(new_buffer);
        return NULL;
    }

    clone_buf->buffer = new_buffer;
    clone_buf->queue = cl_dev->queue;

    return clone_buf;
}

static void opencl_buffer_destroy(void* buf) {
    if (!buf) return;

    OpenCLBuffer* cl_buf = (OpenCLBuffer*)buf;
    clReleaseMemObject(cl_buf->buffer);
    free(cl_buf);
}

static void* opencl_kernel_compile(void* dev, const char* source, size_t source_len,
                                    char* error, size_t error_len) {
    OpenCLDevice* cl_dev = (OpenCLDevice*)dev;

    cl_int err;
    cl_program program = NULL;

    /* Check if source is SPIR-V binary (magic number 0x07230203).
     * If so, use clCreateProgramWithIL (OpenCL 2.1+).
     * Otherwise, fall back to clCreateProgramWithSource for text. */
    int is_spirv = (source_len >= 4 &&
                    (unsigned char)source[0] == 0x03 &&
                    (unsigned char)source[1] == 0x02 &&
                    (unsigned char)source[2] == 0x23 &&
                    (unsigned char)source[3] == 0x07);

    if (is_spirv) {
#ifdef CL_VERSION_2_1
        program = clCreateProgramWithIL(cl_dev->context, source, source_len, &err);
#else
        /* clCreateProgramWithIL not available — try loading it dynamically */
        typedef cl_program (CL_API_CALL *pfn_clCreateProgramWithIL)(
            cl_context, const void*, size_t, cl_int*);
        pfn_clCreateProgramWithIL fn = NULL;
#ifdef clGetExtensionFunctionAddressForPlatform
        fn = (pfn_clCreateProgramWithIL)clGetExtensionFunctionAddressForPlatform(
            NULL, "clCreateProgramWithIL");
#endif
        if (fn) {
            program = fn(cl_dev->context, source, source_len, &err);
        } else {
            if (error) snprintf(error, error_len, "SPIR-V input requires OpenCL 2.1+ (clCreateProgramWithIL not available)");
            return NULL;
        }
#endif
    } else {
        program = clCreateProgramWithSource(cl_dev->context, 1, &source, &source_len, &err);
    }

    if (!program || err != CL_SUCCESS) {
        if (error) snprintf(error, error_len, "Failed to create OpenCL program (err=%d)", err);
        return NULL;
    }

    /* Build program */
    err = clBuildProgram(program, 1, &cl_dev->device_id, NULL, NULL, NULL);
    if (err != CL_SUCCESS) {
        if (error) {
            clGetProgramBuildInfo(program, cl_dev->device_id, CL_PROGRAM_BUILD_LOG,
                                  error_len, error, NULL);
        }
        clReleaseProgram(program);
        return NULL;
    }

    /* Create kernel (assume function named "main" or first kernel) */
    cl_kernel kernel = clCreateKernel(program, "main", &err);
    if (err != CL_SUCCESS) {
        /* Try to get first kernel name */
        size_t kernel_names_size;
        clGetProgramInfo(program, CL_PROGRAM_KERNEL_NAMES, 0, NULL, &kernel_names_size);
        if (kernel_names_size > 0) {
            char* kernel_names = malloc(kernel_names_size);
            clGetProgramInfo(program, CL_PROGRAM_KERNEL_NAMES, kernel_names_size,
                            kernel_names, NULL);
            /* Get first kernel name (up to first ';') */
            char* semicolon = strchr(kernel_names, ';');
            if (semicolon) *semicolon = '\0';
            kernel = clCreateKernel(program, kernel_names, &err);
            free(kernel_names);
        }

        if (err != CL_SUCCESS) {
            if (error) {
                snprintf(error, error_len, "Failed to create OpenCL kernel");
            }
            clReleaseProgram(program);
            return NULL;
        }
    }

    clReleaseProgram(program);

    OpenCLKernel* cl_kernel = malloc(sizeof(OpenCLKernel));
    if (!cl_kernel) {
        clReleaseKernel(kernel);
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
                                    void* output, int work_size) {
    OpenCLKernel* cl_kernel = (OpenCLKernel*)kernel;

    /* Set kernel arguments */
    for (int i = 0; i < input_count; i++) {
        if (inputs[i]) {
            OpenCLBuffer* input_buf = (OpenCLBuffer*)inputs[i];
            clSetKernelArg(cl_kernel->kernel, i, sizeof(cl_mem), &input_buf->buffer);
        }
    }

    /* Set output argument */
    OpenCLBuffer* output_buf = (OpenCLBuffer*)output;
    clSetKernelArg(cl_kernel->kernel, input_count, sizeof(cl_mem), &output_buf->buffer);

    /* Execute */
    size_t global_work_size = work_size;
    clEnqueueNDRangeKernel(cl_kernel->queue,
                           cl_kernel->kernel,
                           1,
                           NULL,
                           &global_work_size,
                           NULL,
                           0, NULL, NULL);
    clFinish(cl_kernel->queue);
}

static void opencl_kernel_destroy(void* kernel) {
    if (!kernel) return;

    OpenCLKernel* cl_kernel = (OpenCLKernel*)kernel;
    clReleaseKernel(cl_kernel->kernel);
    free(cl_kernel);
}

/* ── Pipe ────────────────────────────────────���─────────────────── */

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
                            int input_count, void* output, int work_size) {
    OpenCLPipe* pipe = (OpenCLPipe*)pipe_ptr;
    OpenCLKernel* cl_kernel = (OpenCLKernel*)kernel;

    /* Set kernel arguments */
    for (int i = 0; i < input_count; i++) {
        if (inputs[i]) {
            OpenCLBuffer* input_buf = (OpenCLBuffer*)inputs[i];
            clSetKernelArg(cl_kernel->kernel, i, sizeof(cl_mem), &input_buf->buffer);
        }
    }

    OpenCLBuffer* output_buf = (OpenCLBuffer*)output;
    clSetKernelArg(cl_kernel->kernel, input_count, sizeof(cl_mem), &output_buf->buffer);

    /* Enqueue without waiting */
    size_t global_work_size = work_size;
    clEnqueueNDRangeKernel(pipe->queue,
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
    clFinish(pipe->queue);
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

#else
/* OpenCL not available */
#include "mental_internal.h"
#include <stddef.h>
mental_backend* opencl_backend = NULL;
#endif /* MENTAL_HAS_OPENCL */
