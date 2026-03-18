//go:build linux || darwin || windows

#define CL_TARGET_OPENCL_VERSION 120
#define CL_USE_DEPRECATED_OPENCL_1_2_APIS
#ifdef __APPLE__
#include <OpenCL/cl.h>
#else
#include <CL/cl.h>
#endif
#include "opencl_loader.h"
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <vector>

// Helper to duplicate a string (caller must free)
static char* strdup_helper(const char* str) {
    if (str == nullptr) return nullptr;
    size_t len = strlen(str);
    char* dup = (char*)malloc(len + 1);
    if (dup) {
        memcpy(dup, str, len + 1);
    }
    return dup;
}

// Device context structure
struct OpenCLDevice {
    cl_platform_id platform;
    cl_device_id device;
    cl_context context;
    cl_command_queue queue;
};

// Buffer structure
struct OpenCLBuffer {
    cl_mem buffer;
    void* mappedPtr;
    size_t size;
    cl_command_queue queue;
};

// Kernel structure
struct OpenCLKernel {
    cl_program program;
    cl_kernel kernel;
    cl_context context;
};

extern "C" {

// Enumerate OpenCL devices
int opencl_enumerate_devices(void*** devices_out, char*** names_out, int** types_out) {
    // Get platforms
    cl_uint platformCount = 0;
    clGetPlatformIDs(0, nullptr, &platformCount);

    if (platformCount == 0) {
        return 0;
    }

    std::vector<cl_platform_id> platforms(platformCount);
    clGetPlatformIDs(platformCount, platforms.data(), nullptr);

    // Count total devices across all platforms
    cl_uint totalDevices = 0;
    for (cl_uint i = 0; i < platformCount; i++) {
        cl_uint deviceCount = 0;
        clGetDeviceIDs(platforms[i], CL_DEVICE_TYPE_ALL, 0, nullptr, &deviceCount);
        totalDevices += deviceCount;
    }

    if (totalDevices == 0) {
        return 0;
    }

    // Allocate output arrays
    void** device_array = (void**)malloc(totalDevices * sizeof(void*));
    char** name_array = (char**)malloc(totalDevices * sizeof(char*));
    int* type_array = (int*)malloc(totalDevices * sizeof(int));

    // Collect all devices
    cl_uint deviceIndex = 0;
    for (cl_uint p = 0; p < platformCount; p++) {
        cl_uint deviceCount = 0;
        clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_ALL, 0, nullptr, &deviceCount);

        if (deviceCount == 0) continue;

        std::vector<cl_device_id> devices(deviceCount);
        clGetDeviceIDs(platforms[p], CL_DEVICE_TYPE_ALL, deviceCount, devices.data(), nullptr);

        for (cl_uint d = 0; d < deviceCount; d++) {
            // Get device name
            char deviceName[256] = {0};
            clGetDeviceInfo(devices[d], CL_DEVICE_NAME, sizeof(deviceName), deviceName, nullptr);

            // Get device type
            cl_device_type deviceType;
            clGetDeviceInfo(devices[d], CL_DEVICE_TYPE, sizeof(deviceType), &deviceType, nullptr);

            // Map OpenCL device type to our Type enum
            // Other=0, Integrated=1, Discrete=2, Virtual=3, CPU=4
            int type = 0; // Other
            if (deviceType & CL_DEVICE_TYPE_GPU) {
                type = 2; // Discrete (we'll treat all GPUs as discrete for now)
            } else if (deviceType & CL_DEVICE_TYPE_CPU) {
                type = 4; // CPU
            } else if (deviceType & CL_DEVICE_TYPE_ACCELERATOR) {
                type = 0; // Other
            }

            // Store device info (we need to keep platform info too)
            // For simplicity, we'll encode platform:device as a pair
            // Allocate a pair structure
            cl_platform_id* pair = (cl_platform_id*)malloc(2 * sizeof(void*));
            pair[0] = platforms[p];
            ((cl_device_id*)pair)[1] = devices[d];

            device_array[deviceIndex] = (void*)pair;
            name_array[deviceIndex] = strdup_helper(deviceName);
            type_array[deviceIndex] = type;
            deviceIndex++;
        }
    }

    *devices_out = device_array;
    *names_out = name_array;
    *types_out = type_array;

    return (int)deviceIndex;
}

// Create OpenCL device context
void* opencl_create_device(void* device_pair_ptr, char** error_out) {
    if (device_pair_ptr == nullptr) {
        if (error_out) *error_out = strdup_helper("Invalid device pointer");
        return nullptr;
    }

    cl_platform_id* pair = (cl_platform_id*)device_pair_ptr;
    cl_platform_id platform = pair[0];
    cl_device_id device = ((cl_device_id*)pair)[1];

    // Create context
    cl_int err;
    cl_context context = clCreateContext(nullptr, 1, &device, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) {
        if (error_out) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Failed to create OpenCL context (error %d)", err);
            *error_out = strdup_helper(buf);
        }
        return nullptr;
    }

    // Create command queue
    cl_command_queue queue = clCreateCommandQueue(context, device, 0, &err);
    if (err != CL_SUCCESS) {
        clReleaseContext(context);
        if (error_out) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Failed to create command queue (error %d)", err);
            *error_out = strdup_helper(buf);
        }
        return nullptr;
    }

    // Create device context
    OpenCLDevice* ctx = new OpenCLDevice();
    ctx->platform = platform;
    ctx->device = device;
    ctx->context = context;
    ctx->queue = queue;

    return (void*)ctx;
}

// Release OpenCL device
void opencl_release_device(void* device_ctx) {
    if (device_ctx == nullptr) return;

    OpenCLDevice* ctx = (OpenCLDevice*)device_ctx;

    if (ctx->queue != nullptr) {
        clReleaseCommandQueue(ctx->queue);
    }

    if (ctx->context != nullptr) {
        clReleaseContext(ctx->context);
    }

    delete ctx;
}

// Allocate buffer
void* opencl_alloc_buffer(void* device_ctx, size_t size) {
    OpenCLDevice* ctx = (OpenCLDevice*)device_ctx;

    cl_int err;
    cl_mem buffer = clCreateBuffer(ctx->context,
                                    CL_MEM_READ_WRITE | CL_MEM_ALLOC_HOST_PTR,
                                    size, nullptr, &err);
    if (err != CL_SUCCESS || buffer == nullptr) {
        return nullptr;
    }

    // Map the buffer immediately for persistent access
    void* mappedPtr = clEnqueueMapBuffer(ctx->queue, buffer, CL_TRUE,
                                         CL_MAP_READ | CL_MAP_WRITE,
                                         0, size, 0, nullptr, nullptr, &err);
    if (err != CL_SUCCESS) {
        clReleaseMemObject(buffer);
        return nullptr;
    }

    // Create buffer structure
    OpenCLBuffer* clBuf = new OpenCLBuffer();
    clBuf->buffer = buffer;
    clBuf->mappedPtr = mappedPtr;
    clBuf->size = size;
    clBuf->queue = ctx->queue;

    return (void*)clBuf;
}

// Release buffer
void opencl_release_buffer(void* buffer_ptr) {
    if (buffer_ptr == nullptr) return;

    OpenCLBuffer* clBuf = (OpenCLBuffer*)buffer_ptr;

    if (clBuf->mappedPtr != nullptr) {
        clEnqueueUnmapMemObject(clBuf->queue, clBuf->buffer, clBuf->mappedPtr, 0, nullptr, nullptr);
    }

    if (clBuf->buffer != nullptr) {
        clReleaseMemObject(clBuf->buffer);
    }

    delete clBuf;
}

// Get buffer contents pointer
void* opencl_buffer_contents(void* buffer_ptr) {
    if (buffer_ptr == nullptr) return nullptr;
    OpenCLBuffer* clBuf = (OpenCLBuffer*)buffer_ptr;
    return clBuf->mappedPtr;
}

// Get buffer size
size_t opencl_buffer_size(void* buffer_ptr) {
    if (buffer_ptr == nullptr) return 0;
    OpenCLBuffer* clBuf = (OpenCLBuffer*)buffer_ptr;
    return clBuf->size;
}

// Compile OpenCL kernel from source
void* opencl_compile_kernel(void* device_ctx, const char* source, char** error_out) {
    OpenCLDevice* ctx = (OpenCLDevice*)device_ctx;

    // Create program
    cl_int err;
    size_t sourceLen = strlen(source);
    cl_program program = clCreateProgramWithSource(ctx->context, 1, &source, &sourceLen, &err);
    if (err != CL_SUCCESS) {
        if (error_out) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Failed to create program (error %d)", err);
            *error_out = strdup_helper(buf);
        }
        return nullptr;
    }

    // Build program
    err = clBuildProgram(program, 1, &ctx->device, nullptr, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        // Get build log
        size_t logSize;
        clGetProgramBuildInfo(program, ctx->device, CL_PROGRAM_BUILD_LOG, 0, nullptr, &logSize);

        char* buildLog = (char*)malloc(logSize + 1);
        clGetProgramBuildInfo(program, ctx->device, CL_PROGRAM_BUILD_LOG, logSize, buildLog, nullptr);
        buildLog[logSize] = '\0';

        if (error_out) {
            *error_out = buildLog;
        } else {
            free(buildLog);
        }

        clReleaseProgram(program);
        return nullptr;
    }

    // Create kernel (assume entry point is "main" or "compute")
    cl_kernel kernel = clCreateKernel(program, "main", &err);
    if (err != CL_SUCCESS) {
        // Try "compute" as fallback
        kernel = clCreateKernel(program, "compute", &err);
        if (err != CL_SUCCESS) {
            if (error_out) {
                char buf[512];
                snprintf(buf, sizeof(buf),
                        "Failed to create kernel. No 'main' or 'compute' entry point found (error %d)", err);
                *error_out = strdup_helper(buf);
            }
            clReleaseProgram(program);
            return nullptr;
        }
    }

    // Create kernel structure
    OpenCLKernel* clKernel = new OpenCLKernel();
    clKernel->program = program;
    clKernel->kernel = kernel;
    clKernel->context = ctx->context;

    return (void*)clKernel;
}

// Release kernel
void opencl_release_kernel(void* kernel_ptr) {
    if (kernel_ptr == nullptr) return;

    OpenCLKernel* clKernel = (OpenCLKernel*)kernel_ptr;

    if (clKernel->kernel != nullptr) {
        clReleaseKernel(clKernel->kernel);
    }

    if (clKernel->program != nullptr) {
        clReleaseProgram(clKernel->program);
    }

    delete clKernel;
}

// Dispatch compute kernel
int opencl_dispatch_compute(void* device_ctx, void* kernel_ptr, void** buffers, int buffer_count, int work_size, char** error_out) {
    OpenCLDevice* ctx = (OpenCLDevice*)device_ctx;
    OpenCLKernel* clKernel = (OpenCLKernel*)kernel_ptr;

    // Set kernel arguments
    for (int i = 0; i < buffer_count; i++) {
        OpenCLBuffer* clBuf = (OpenCLBuffer*)buffers[i];
        cl_int err = clSetKernelArg(clKernel->kernel, i, sizeof(cl_mem), &clBuf->buffer);
        if (err != CL_SUCCESS) {
            if (error_out) {
                char buf[256];
                snprintf(buf, sizeof(buf), "Failed to set kernel arg %d (error %d)", i, err);
                *error_out = strdup_helper(buf);
            }
            return 0;
        }
    }

    // Execute kernel
    size_t globalWorkSize = work_size;
    cl_int err = clEnqueueNDRangeKernel(ctx->queue, clKernel->kernel, 1,
                                        nullptr, &globalWorkSize, nullptr,
                                        0, nullptr, nullptr);
    if (err != CL_SUCCESS) {
        if (error_out) {
            char buf[256];
            snprintf(buf, sizeof(buf), "Failed to execute kernel (error %d)", err);
            *error_out = strdup_helper(buf);
        }
        return 0;
    }

    // Wait for completion
    clFinish(ctx->queue);

    return 1;
}

} // extern "C"
