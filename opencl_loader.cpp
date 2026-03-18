#include "opencl_loader.h"
#include <cstring>
#include <cstdio>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// We need OpenCL types for function pointer typedefs, but we can't include
// the full OpenCL headers because that would conflict with our function pointers.
// Instead, we'll forward declare the minimal types we need.

// Forward declarations for OpenCL types (minimal set needed for function pointers)
typedef struct _cl_platform_id* cl_platform_id;
typedef struct _cl_device_id* cl_device_id;
typedef struct _cl_context* cl_context;
typedef struct _cl_command_queue* cl_command_queue;
typedef struct _cl_mem* cl_mem;
typedef struct _cl_program* cl_program;
typedef struct _cl_kernel* cl_kernel;
typedef struct _cl_event* cl_event;

typedef unsigned int cl_uint;
typedef int cl_int;
typedef unsigned long long cl_ulong;
typedef unsigned long cl_bitfield;
typedef cl_bitfield cl_device_type;
typedef cl_bitfield cl_mem_flags;
typedef cl_bitfield cl_map_flags;
typedef cl_bitfield cl_command_queue_properties;
typedef cl_uint cl_bool;
typedef cl_uint cl_device_info;
typedef cl_uint cl_context_properties;
typedef cl_uint cl_program_build_info;

// OpenCL calling convention
#ifdef _WIN32
#define CL_CALLBACK __stdcall
#else
#define CL_CALLBACK
#endif

// Global state
static void* opencl_lib = nullptr;
static int opencl_available = 0;
static char error_message[512] = {0};

// OpenCL function pointers
typedef cl_int (*PFN_clGetPlatformIDs)(cl_uint, cl_platform_id*, cl_uint*);
typedef cl_int (*PFN_clGetDeviceIDs)(cl_platform_id, cl_device_type, cl_uint, cl_device_id*, cl_uint*);
typedef cl_int (*PFN_clGetDeviceInfo)(cl_device_id, cl_device_info, size_t, void*, size_t*);
typedef cl_context (*PFN_clCreateContext)(const cl_context_properties*, cl_uint, const cl_device_id*, void (CL_CALLBACK*)(const char*, const void*, size_t, void*), void*, cl_int*);
typedef cl_int (*PFN_clReleaseContext)(cl_context);
typedef cl_command_queue (*PFN_clCreateCommandQueue)(cl_context, cl_device_id, cl_command_queue_properties, cl_int*);
typedef cl_int (*PFN_clReleaseCommandQueue)(cl_command_queue);
typedef cl_mem (*PFN_clCreateBuffer)(cl_context, cl_mem_flags, size_t, void*, cl_int*);
typedef cl_int (*PFN_clReleaseMemObject)(cl_mem);
typedef void* (*PFN_clEnqueueMapBuffer)(cl_command_queue, cl_mem, cl_bool, cl_map_flags, size_t, size_t, cl_uint, const cl_event*, cl_event*, cl_int*);
typedef cl_int (*PFN_clEnqueueUnmapMemObject)(cl_command_queue, cl_mem, void*, cl_uint, const cl_event*, cl_event*);
typedef cl_program (*PFN_clCreateProgramWithSource)(cl_context, cl_uint, const char**, const size_t*, cl_int*);
typedef cl_int (*PFN_clBuildProgram)(cl_program, cl_uint, const cl_device_id*, const char*, void (CL_CALLBACK*)(cl_program, void*), void*);
typedef cl_int (*PFN_clGetProgramBuildInfo)(cl_program, cl_device_id, cl_program_build_info, size_t, void*, size_t*);
typedef cl_kernel (*PFN_clCreateKernel)(cl_program, const char*, cl_int*);
typedef cl_int (*PFN_clReleaseProgram)(cl_program);
typedef cl_int (*PFN_clReleaseKernel)(cl_kernel);
typedef cl_int (*PFN_clSetKernelArg)(cl_kernel, cl_uint, size_t, const void*);
typedef cl_int (*PFN_clEnqueueNDRangeKernel)(cl_command_queue, cl_kernel, cl_uint, const size_t*, const size_t*, const size_t*, cl_uint, const cl_event*, cl_event*);
typedef cl_int (*PFN_clFinish)(cl_command_queue);
typedef cl_int (*PFN_clFlush)(cl_command_queue);

// Global function pointers
PFN_clGetPlatformIDs clGetPlatformIDs = nullptr;
PFN_clGetDeviceIDs clGetDeviceIDs = nullptr;
PFN_clGetDeviceInfo clGetDeviceInfo = nullptr;
PFN_clCreateContext clCreateContext = nullptr;
PFN_clReleaseContext clReleaseContext = nullptr;
PFN_clCreateCommandQueue clCreateCommandQueue = nullptr;
PFN_clReleaseCommandQueue clReleaseCommandQueue = nullptr;
PFN_clCreateBuffer clCreateBuffer = nullptr;
PFN_clReleaseMemObject clReleaseMemObject = nullptr;
PFN_clEnqueueMapBuffer clEnqueueMapBuffer = nullptr;
PFN_clEnqueueUnmapMemObject clEnqueueUnmapMemObject = nullptr;
PFN_clCreateProgramWithSource clCreateProgramWithSource = nullptr;
PFN_clBuildProgram clBuildProgram = nullptr;
PFN_clGetProgramBuildInfo clGetProgramBuildInfo = nullptr;
PFN_clCreateKernel clCreateKernel = nullptr;
PFN_clReleaseProgram clReleaseProgram = nullptr;
PFN_clReleaseKernel clReleaseKernel = nullptr;
PFN_clSetKernelArg clSetKernelArg = nullptr;
PFN_clEnqueueNDRangeKernel clEnqueueNDRangeKernel = nullptr;
PFN_clFinish clFinish = nullptr;
PFN_clFlush clFlush = nullptr;

extern "C" {

int opencl_loader_init() {
    if (opencl_available) {
        return 1; // Already initialized
    }

    // Try to load OpenCL library
#ifdef _WIN32
    // Windows: Try to load OpenCL.dll
    const wchar_t* lib_names[] = {
        L"OpenCL.dll",
        nullptr
    };

    for (int i = 0; lib_names[i] != nullptr; i++) {
        opencl_lib = (void*)LoadLibraryW(lib_names[i]);
        if (opencl_lib != nullptr) {
            break;
        }
    }
#elif defined(__APPLE__)
    // macOS: OpenCL is a system framework
    const char* lib_names[] = {
        "/System/Library/Frameworks/OpenCL.framework/OpenCL",
        nullptr
    };

    for (int i = 0; lib_names[i] != nullptr; i++) {
        opencl_lib = dlopen(lib_names[i], RTLD_NOW | RTLD_LOCAL);
        if (opencl_lib != nullptr) {
            break;
        }
    }
#else
    // Linux: Try various .so naming schemes
    const char* lib_names[] = {
        "libOpenCL.so.1",
        "libOpenCL.so",
        nullptr
    };

    for (int i = 0; lib_names[i] != nullptr; i++) {
        opencl_lib = dlopen(lib_names[i], RTLD_NOW | RTLD_LOCAL);
        if (opencl_lib != nullptr) {
            break;
        }
    }
#endif

    if (opencl_lib == nullptr) {
        snprintf(error_message, sizeof(error_message),
                 "OpenCL not found - please install either OpenCL or PoCL for CPU-based compute");
        return 0;
    }

    // Load all required OpenCL functions
#ifdef _WIN32
    #define LOAD_CL_FUNC(name) \
        name = (PFN_##name)GetProcAddress((HMODULE)opencl_lib, #name); \
        if (name == nullptr) { \
            snprintf(error_message, sizeof(error_message), "Failed to load " #name); \
            FreeLibrary((HMODULE)opencl_lib); \
            opencl_lib = nullptr; \
            return 0; \
        }
#else
    #define LOAD_CL_FUNC(name) \
        name = (PFN_##name)dlsym(opencl_lib, #name); \
        if (name == nullptr) { \
            snprintf(error_message, sizeof(error_message), "Failed to load " #name); \
            dlclose(opencl_lib); \
            opencl_lib = nullptr; \
            return 0; \
        }
#endif

    LOAD_CL_FUNC(clGetPlatformIDs)
    LOAD_CL_FUNC(clGetDeviceIDs)
    LOAD_CL_FUNC(clGetDeviceInfo)
    LOAD_CL_FUNC(clCreateContext)
    LOAD_CL_FUNC(clReleaseContext)
    LOAD_CL_FUNC(clCreateCommandQueue)
    LOAD_CL_FUNC(clReleaseCommandQueue)
    LOAD_CL_FUNC(clCreateBuffer)
    LOAD_CL_FUNC(clReleaseMemObject)
    LOAD_CL_FUNC(clEnqueueMapBuffer)
    LOAD_CL_FUNC(clEnqueueUnmapMemObject)
    LOAD_CL_FUNC(clCreateProgramWithSource)
    LOAD_CL_FUNC(clBuildProgram)
    LOAD_CL_FUNC(clGetProgramBuildInfo)
    LOAD_CL_FUNC(clCreateKernel)
    LOAD_CL_FUNC(clReleaseProgram)
    LOAD_CL_FUNC(clReleaseKernel)
    LOAD_CL_FUNC(clSetKernelArg)
    LOAD_CL_FUNC(clEnqueueNDRangeKernel)
    LOAD_CL_FUNC(clFinish)
    LOAD_CL_FUNC(clFlush)

    #undef LOAD_CL_FUNC

    opencl_available = 1;
    return 1;
}

int opencl_loader_available() {
    return opencl_available;
}

const char* opencl_loader_error() {
    return error_message;
}

} // extern "C"
