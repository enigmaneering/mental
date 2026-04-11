/*
 * Mental - WebGPU Backend (wgpu-native)
 *
 * Cross-platform GPU compute via WebGPU, backed by wgpu-native which
 * translates to Vulkan/Metal/D3D12 under the hood.
 *
 * wgpu-native is loaded dynamically (dlopen / LoadLibrary) from either:
 *   1. The bundled redistributable in external/wgpu/lib/
 *   2. A relative path ../external/wgpu/lib/
 *   3. The system search path as a final attempt
 *
 * This backend uses the wgpu-native C API surface directly through
 * resolved function pointers — it does NOT link against wgpu-native
 * at build time.
 */

#ifdef MENTAL_HAS_WEBGPU

#ifdef _WIN32
#  include <windows.h>
#  define WGPU_DLOPEN(path)    LoadLibraryA(path)
#  define WGPU_DLSYM(lib, sym) GetProcAddress((HMODULE)(lib), sym)
#  define WGPU_DLCLOSE(lib)    FreeLibrary((HMODULE)(lib))
#  define WGPU_LIB_NAME        "wgpu_native.dll"
#else
#  include <dlfcn.h>
#  define WGPU_DLOPEN(path)    dlopen(path, RTLD_LAZY)
#  define WGPU_DLSYM(lib, sym) dlsym(lib, sym)
#  define WGPU_DLCLOSE(lib)    dlclose(lib)
#  ifdef __APPLE__
#    define WGPU_LIB_NAME      "libwgpu_native.dylib"
#  else
#    define WGPU_LIB_NAME      "libwgpu_native.so"
#  endif
#endif

#ifdef __EMSCRIPTEN__
/* emdawnwebgpu port provides webgpu.h via --use-port flag */
#include <webgpu/webgpu.h>
#else
/* wgpu-native headers from redistributables */
#include "external/wgpu/include/webgpu/webgpu.h"
#include "external/wgpu/include/webgpu/wgpu.h"
#endif
#include "mental_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  wgpu function pointer types (subset we need)                      */
/* ------------------------------------------------------------------ */

typedef WGPUInstance     (*pfn_wgpuCreateInstance)(const WGPUInstanceDescriptor*);
typedef void             (*pfn_wgpuInstanceRelease)(WGPUInstance);
typedef void             (*pfn_wgpuInstanceProcessEvents)(WGPUInstance);
typedef WGPUFuture       (*pfn_wgpuInstanceRequestAdapter)(WGPUInstance, const WGPURequestAdapterOptions*, WGPURequestAdapterCallbackInfo);
#ifndef __EMSCRIPTEN__
/* wgpu-native extensions not available in Dawn/Emscripten */
typedef size_t           (*pfn_wgpuInstanceEnumerateAdapters)(WGPUInstance, const WGPUInstanceEnumerateAdapterOptions*, WGPUAdapter*);
#else
typedef size_t           (*pfn_wgpuInstanceEnumerateAdapters)(void*, const void*, void*);
#endif

typedef WGPUStatus       (*pfn_wgpuAdapterGetInfo)(WGPUAdapter, WGPUAdapterInfo*);
typedef void             (*pfn_wgpuAdapterInfoFreeMembers)(WGPUAdapterInfo);
typedef WGPUFuture       (*pfn_wgpuAdapterRequestDevice)(WGPUAdapter, const WGPUDeviceDescriptor*, WGPURequestDeviceCallbackInfo);
typedef void             (*pfn_wgpuAdapterRelease)(WGPUAdapter);

typedef void             (*pfn_wgpuDeviceDestroy)(WGPUDevice);
typedef void             (*pfn_wgpuDeviceRelease)(WGPUDevice);
typedef WGPUQueue        (*pfn_wgpuDeviceGetQueue)(WGPUDevice);
typedef WGPUBuffer       (*pfn_wgpuDeviceCreateBuffer)(WGPUDevice, const WGPUBufferDescriptor*);
typedef WGPUShaderModule (*pfn_wgpuDeviceCreateShaderModule)(WGPUDevice, const WGPUShaderModuleDescriptor*);
typedef WGPUComputePipeline (*pfn_wgpuDeviceCreateComputePipeline)(WGPUDevice, const WGPUComputePipelineDescriptor*);
typedef WGPUBindGroup    (*pfn_wgpuDeviceCreateBindGroup)(WGPUDevice, const WGPUBindGroupDescriptor*);
typedef WGPUBindGroupLayout (*pfn_wgpuDeviceCreateBindGroupLayout)(WGPUDevice, const WGPUBindGroupLayoutDescriptor*);
typedef WGPUCommandEncoder (*pfn_wgpuDeviceCreateCommandEncoder)(WGPUDevice, const WGPUCommandEncoderDescriptor*);
typedef WGPUPipelineLayout (*pfn_wgpuDeviceCreatePipelineLayout)(WGPUDevice, const WGPUPipelineLayoutDescriptor*);
#ifndef __EMSCRIPTEN__
typedef WGPUBool         (*pfn_wgpuDevicePoll)(WGPUDevice, WGPUBool, const WGPUSubmissionIndex*);
#else
/* Dawn doesn't have wgpuDevicePoll — use wgpuInstanceProcessEvents instead */
typedef void             (*pfn_wgpuDevicePoll)(void*, int, const void*);
#endif

typedef void             (*pfn_wgpuQueueSubmit)(WGPUQueue, size_t, const WGPUCommandBuffer*);
typedef void             (*pfn_wgpuQueueWriteBuffer)(WGPUQueue, WGPUBuffer, uint64_t, const void*, size_t);
typedef void             (*pfn_wgpuQueueRelease)(WGPUQueue);

typedef WGPUFuture       (*pfn_wgpuBufferMapAsync)(WGPUBuffer, WGPUMapMode, size_t, size_t, WGPUBufferMapCallbackInfo);
typedef const void*      (*pfn_wgpuBufferGetConstMappedRange)(WGPUBuffer, size_t, size_t);
typedef void             (*pfn_wgpuBufferUnmap)(WGPUBuffer);
typedef void             (*pfn_wgpuBufferDestroy)(WGPUBuffer);
typedef void             (*pfn_wgpuBufferRelease)(WGPUBuffer);

typedef WGPUComputePassEncoder (*pfn_wgpuCommandEncoderBeginComputePass)(WGPUCommandEncoder, const WGPUComputePassDescriptor*);
typedef void             (*pfn_wgpuCommandEncoderCopyBufferToBuffer)(WGPUCommandEncoder, WGPUBuffer, uint64_t, WGPUBuffer, uint64_t, uint64_t);
typedef WGPUCommandBuffer (*pfn_wgpuCommandEncoderFinish)(WGPUCommandEncoder, const WGPUCommandBufferDescriptor*);
typedef void             (*pfn_wgpuCommandEncoderRelease)(WGPUCommandEncoder);

typedef void             (*pfn_wgpuComputePassEncoderSetPipeline)(WGPUComputePassEncoder, WGPUComputePipeline);
typedef void             (*pfn_wgpuComputePassEncoderSetBindGroup)(WGPUComputePassEncoder, uint32_t, WGPUBindGroup, size_t, const uint32_t*);
typedef void             (*pfn_wgpuComputePassEncoderDispatchWorkgroups)(WGPUComputePassEncoder, uint32_t, uint32_t, uint32_t);
typedef void             (*pfn_wgpuComputePassEncoderEnd)(WGPUComputePassEncoder);
typedef void             (*pfn_wgpuComputePassEncoderRelease)(WGPUComputePassEncoder);

typedef WGPUBindGroupLayout (*pfn_wgpuComputePipelineGetBindGroupLayout)(WGPUComputePipeline, uint32_t);
typedef void             (*pfn_wgpuComputePipelineRelease)(WGPUComputePipeline);

typedef void             (*pfn_wgpuShaderModuleRelease)(WGPUShaderModule);
typedef void             (*pfn_wgpuBindGroupRelease)(WGPUBindGroup);
typedef void             (*pfn_wgpuBindGroupLayoutRelease)(WGPUBindGroupLayout);
typedef void             (*pfn_wgpuCommandBufferRelease)(WGPUCommandBuffer);

/* ------------------------------------------------------------------ */
/*  Resolved function pointers                                        */
/* ------------------------------------------------------------------ */

static pfn_wgpuCreateInstance               p_wgpuCreateInstance;
static pfn_wgpuInstanceRelease              p_wgpuInstanceRelease;
static pfn_wgpuInstanceProcessEvents        p_wgpuInstanceProcessEvents;
static pfn_wgpuInstanceRequestAdapter       p_wgpuInstanceRequestAdapter;
static pfn_wgpuInstanceEnumerateAdapters    p_wgpuInstanceEnumerateAdapters;

static pfn_wgpuAdapterGetInfo               p_wgpuAdapterGetInfo;
static pfn_wgpuAdapterInfoFreeMembers       p_wgpuAdapterInfoFreeMembers;
static pfn_wgpuAdapterRequestDevice         p_wgpuAdapterRequestDevice;
static pfn_wgpuAdapterRelease               p_wgpuAdapterRelease;

static pfn_wgpuDeviceDestroy                p_wgpuDeviceDestroy;
static pfn_wgpuDeviceRelease                p_wgpuDeviceRelease;
static pfn_wgpuDeviceGetQueue               p_wgpuDeviceGetQueue;
static pfn_wgpuDeviceCreateBuffer           p_wgpuDeviceCreateBuffer;
static pfn_wgpuDeviceCreateShaderModule     p_wgpuDeviceCreateShaderModule;
static pfn_wgpuDeviceCreateComputePipeline  p_wgpuDeviceCreateComputePipeline;
static pfn_wgpuDeviceCreateBindGroup        p_wgpuDeviceCreateBindGroup;
static pfn_wgpuDeviceCreateBindGroupLayout  p_wgpuDeviceCreateBindGroupLayout;
static pfn_wgpuDeviceCreateCommandEncoder   p_wgpuDeviceCreateCommandEncoder;
static pfn_wgpuDeviceCreatePipelineLayout   p_wgpuDeviceCreatePipelineLayout;
static pfn_wgpuDevicePoll                   p_wgpuDevicePoll;

static pfn_wgpuQueueSubmit                  p_wgpuQueueSubmit;
static pfn_wgpuQueueWriteBuffer             p_wgpuQueueWriteBuffer;
static pfn_wgpuQueueRelease                 p_wgpuQueueRelease;

static pfn_wgpuBufferMapAsync               p_wgpuBufferMapAsync;
static pfn_wgpuBufferGetConstMappedRange    p_wgpuBufferGetConstMappedRange;
static pfn_wgpuBufferUnmap                  p_wgpuBufferUnmap;
static pfn_wgpuBufferDestroy                p_wgpuBufferDestroy;
static pfn_wgpuBufferRelease                p_wgpuBufferRelease;

static pfn_wgpuCommandEncoderBeginComputePass    p_wgpuCommandEncoderBeginComputePass;
static pfn_wgpuCommandEncoderCopyBufferToBuffer  p_wgpuCommandEncoderCopyBufferToBuffer;
static pfn_wgpuCommandEncoderFinish              p_wgpuCommandEncoderFinish;
static pfn_wgpuCommandEncoderRelease             p_wgpuCommandEncoderRelease;

static pfn_wgpuComputePassEncoderSetPipeline          p_wgpuComputePassEncoderSetPipeline;
static pfn_wgpuComputePassEncoderSetBindGroup         p_wgpuComputePassEncoderSetBindGroup;
static pfn_wgpuComputePassEncoderDispatchWorkgroups   p_wgpuComputePassEncoderDispatchWorkgroups;
static pfn_wgpuComputePassEncoderEnd                  p_wgpuComputePassEncoderEnd;
static pfn_wgpuComputePassEncoderRelease              p_wgpuComputePassEncoderRelease;

static pfn_wgpuComputePipelineGetBindGroupLayout p_wgpuComputePipelineGetBindGroupLayout;
static pfn_wgpuComputePipelineRelease            p_wgpuComputePipelineRelease;

static pfn_wgpuShaderModuleRelease          p_wgpuShaderModuleRelease;
static pfn_wgpuBindGroupRelease             p_wgpuBindGroupRelease;
static pfn_wgpuBindGroupLayoutRelease       p_wgpuBindGroupLayoutRelease;
static pfn_wgpuCommandBufferRelease         p_wgpuCommandBufferRelease;

/* ------------------------------------------------------------------ */
/*  Dynamic loader                                                    */
/* ------------------------------------------------------------------ */

static void* g_wgpu_lib = NULL;

static int load_wgpu_symbols(void) {
#define LOAD(ptr, name) do { \
    *(void**)(&ptr) = (void*)WGPU_DLSYM(g_wgpu_lib, #name); \
    if (!ptr) return -1; \
} while (0)

    LOAD(p_wgpuCreateInstance,              wgpuCreateInstance);
    LOAD(p_wgpuInstanceRelease,             wgpuInstanceRelease);
    LOAD(p_wgpuInstanceProcessEvents,       wgpuInstanceProcessEvents);
    LOAD(p_wgpuInstanceRequestAdapter,      wgpuInstanceRequestAdapter);
    LOAD(p_wgpuInstanceEnumerateAdapters,   wgpuInstanceEnumerateAdapters);

    LOAD(p_wgpuAdapterGetInfo,              wgpuAdapterGetInfo);
    LOAD(p_wgpuAdapterInfoFreeMembers,      wgpuAdapterInfoFreeMembers);
    LOAD(p_wgpuAdapterRequestDevice,        wgpuAdapterRequestDevice);
    LOAD(p_wgpuAdapterRelease,              wgpuAdapterRelease);

    LOAD(p_wgpuDeviceDestroy,               wgpuDeviceDestroy);
    LOAD(p_wgpuDeviceRelease,               wgpuDeviceRelease);
    LOAD(p_wgpuDeviceGetQueue,              wgpuDeviceGetQueue);
    LOAD(p_wgpuDeviceCreateBuffer,          wgpuDeviceCreateBuffer);
    LOAD(p_wgpuDeviceCreateShaderModule,    wgpuDeviceCreateShaderModule);
    LOAD(p_wgpuDeviceCreateComputePipeline, wgpuDeviceCreateComputePipeline);
    LOAD(p_wgpuDeviceCreateBindGroup,       wgpuDeviceCreateBindGroup);
    LOAD(p_wgpuDeviceCreateBindGroupLayout, wgpuDeviceCreateBindGroupLayout);
    LOAD(p_wgpuDeviceCreateCommandEncoder,  wgpuDeviceCreateCommandEncoder);
    LOAD(p_wgpuDeviceCreatePipelineLayout,  wgpuDeviceCreatePipelineLayout);
    LOAD(p_wgpuDevicePoll,                  wgpuDevicePoll);

    LOAD(p_wgpuQueueSubmit,                 wgpuQueueSubmit);
    LOAD(p_wgpuQueueWriteBuffer,            wgpuQueueWriteBuffer);
    LOAD(p_wgpuQueueRelease,               wgpuQueueRelease);

    LOAD(p_wgpuBufferMapAsync,              wgpuBufferMapAsync);
    LOAD(p_wgpuBufferGetConstMappedRange,   wgpuBufferGetConstMappedRange);
    LOAD(p_wgpuBufferUnmap,                 wgpuBufferUnmap);
    LOAD(p_wgpuBufferDestroy,               wgpuBufferDestroy);
    LOAD(p_wgpuBufferRelease,               wgpuBufferRelease);

    LOAD(p_wgpuCommandEncoderBeginComputePass,   wgpuCommandEncoderBeginComputePass);
    LOAD(p_wgpuCommandEncoderCopyBufferToBuffer, wgpuCommandEncoderCopyBufferToBuffer);
    LOAD(p_wgpuCommandEncoderFinish,             wgpuCommandEncoderFinish);
    LOAD(p_wgpuCommandEncoderRelease,            wgpuCommandEncoderRelease);

    LOAD(p_wgpuComputePassEncoderSetPipeline,        wgpuComputePassEncoderSetPipeline);
    LOAD(p_wgpuComputePassEncoderSetBindGroup,       wgpuComputePassEncoderSetBindGroup);
    LOAD(p_wgpuComputePassEncoderDispatchWorkgroups, wgpuComputePassEncoderDispatchWorkgroups);
    LOAD(p_wgpuComputePassEncoderEnd,                wgpuComputePassEncoderEnd);
    LOAD(p_wgpuComputePassEncoderRelease,            wgpuComputePassEncoderRelease);

    LOAD(p_wgpuComputePipelineGetBindGroupLayout, wgpuComputePipelineGetBindGroupLayout);
    LOAD(p_wgpuComputePipelineRelease,            wgpuComputePipelineRelease);

    LOAD(p_wgpuShaderModuleRelease,         wgpuShaderModuleRelease);
    LOAD(p_wgpuBindGroupRelease,            wgpuBindGroupRelease);
    LOAD(p_wgpuBindGroupLayoutRelease,      wgpuBindGroupLayoutRelease);
    LOAD(p_wgpuCommandBufferRelease,        wgpuCommandBufferRelease);

#undef LOAD
    return 0;
}

/* Try to load wgpu-native from a series of candidate paths */
/* Shared search paths for wgpu-native library */
static const char* g_wgpu_search_paths[] = {
    "./external/wgpu/lib/" WGPU_LIB_NAME,
    "../external/wgpu/lib/" WGPU_LIB_NAME,
    "../../external/wgpu/lib/" WGPU_LIB_NAME,
    "../../../external/wgpu/lib/" WGPU_LIB_NAME,
    "../../../../external/wgpu/lib/" WGPU_LIB_NAME,
    WGPU_LIB_NAME,
};
#define WGPU_SEARCH_PATH_COUNT (sizeof(g_wgpu_search_paths) / sizeof(g_wgpu_search_paths[0]))

static int open_wgpu_library(void) {
#ifdef __EMSCRIPTEN__
    /* Emscripten's emdawnwebgpu port provides WebGPU symbols at link time —
     * no dlopen needed. Bind function pointers directly. */
    p_wgpuCreateInstance              = wgpuCreateInstance;
    p_wgpuInstanceRelease             = wgpuInstanceRelease;
    p_wgpuInstanceProcessEvents       = wgpuInstanceProcessEvents;
    p_wgpuInstanceRequestAdapter      = wgpuInstanceRequestAdapter;
    /* wgpuInstanceEnumerateAdapters is a wgpu-native extension, not available
     * in Dawn/emdawnwebgpu. The init function handles NULL gracefully by
     * falling back to wgpuInstanceRequestAdapter. */
    p_wgpuInstanceEnumerateAdapters   = NULL;
    p_wgpuAdapterGetInfo              = wgpuAdapterGetInfo;
    p_wgpuAdapterInfoFreeMembers      = wgpuAdapterInfoFreeMembers;
    p_wgpuAdapterRequestDevice        = wgpuAdapterRequestDevice;
    p_wgpuAdapterRelease              = wgpuAdapterRelease;
    p_wgpuDeviceDestroy               = wgpuDeviceDestroy;
    p_wgpuDeviceRelease               = wgpuDeviceRelease;
    p_wgpuDeviceGetQueue              = wgpuDeviceGetQueue;
    p_wgpuDeviceCreateBuffer          = wgpuDeviceCreateBuffer;
    p_wgpuDeviceCreateShaderModule    = wgpuDeviceCreateShaderModule;
    p_wgpuDeviceCreateComputePipeline = wgpuDeviceCreateComputePipeline;
    p_wgpuDeviceCreateBindGroup       = wgpuDeviceCreateBindGroup;
    p_wgpuDeviceCreateBindGroupLayout = wgpuDeviceCreateBindGroupLayout;
    p_wgpuDeviceCreateCommandEncoder  = wgpuDeviceCreateCommandEncoder;
    p_wgpuDeviceCreatePipelineLayout  = wgpuDeviceCreatePipelineLayout;
    p_wgpuDevicePoll                  = NULL; /* Dawn uses wgpuInstanceProcessEvents instead */
    p_wgpuQueueSubmit                 = wgpuQueueSubmit;
    p_wgpuQueueWriteBuffer            = wgpuQueueWriteBuffer;
    p_wgpuQueueRelease                = wgpuQueueRelease;
    p_wgpuBufferMapAsync              = wgpuBufferMapAsync;
    p_wgpuBufferGetConstMappedRange   = wgpuBufferGetConstMappedRange;
    p_wgpuBufferUnmap                 = wgpuBufferUnmap;
    p_wgpuBufferDestroy               = wgpuBufferDestroy;
    p_wgpuBufferRelease               = wgpuBufferRelease;
    p_wgpuCommandEncoderBeginComputePass    = wgpuCommandEncoderBeginComputePass;
    p_wgpuCommandEncoderCopyBufferToBuffer  = wgpuCommandEncoderCopyBufferToBuffer;
    p_wgpuCommandEncoderFinish              = wgpuCommandEncoderFinish;
    p_wgpuCommandEncoderRelease             = wgpuCommandEncoderRelease;
    p_wgpuComputePassEncoderSetPipeline         = wgpuComputePassEncoderSetPipeline;
    p_wgpuComputePassEncoderSetBindGroup        = wgpuComputePassEncoderSetBindGroup;
    p_wgpuComputePassEncoderDispatchWorkgroups  = wgpuComputePassEncoderDispatchWorkgroups;
    p_wgpuComputePassEncoderEnd                 = wgpuComputePassEncoderEnd;
    p_wgpuComputePassEncoderRelease             = wgpuComputePassEncoderRelease;
    p_wgpuComputePipelineGetBindGroupLayout = wgpuComputePipelineGetBindGroupLayout;
    p_wgpuComputePipelineRelease            = wgpuComputePipelineRelease;
    p_wgpuShaderModuleRelease          = wgpuShaderModuleRelease;
    p_wgpuBindGroupRelease             = wgpuBindGroupRelease;
    p_wgpuBindGroupLayoutRelease       = wgpuBindGroupLayoutRelease;
    p_wgpuCommandBufferRelease         = wgpuCommandBufferRelease;
    return 0;
#else
    for (size_t i = 0; i < WGPU_SEARCH_PATH_COUNT; i++) {
        g_wgpu_lib = WGPU_DLOPEN(g_wgpu_search_paths[i]);
        if (g_wgpu_lib && load_wgpu_symbols() == 0) return 0;
        if (g_wgpu_lib) { WGPU_DLCLOSE(g_wgpu_lib); g_wgpu_lib = NULL; }
    }
    return -1;
#endif
}

/* Probe-only: try to dlopen wgpu-native without loading symbols. */
static void* try_load_wgpu(void) {
    for (size_t i = 0; i < WGPU_SEARCH_PATH_COUNT; i++) {
        void *lib = WGPU_DLOPEN(g_wgpu_search_paths[i]);
        if (lib) return lib;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/*  Internal types                                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    WGPUAdapter adapter;
    WGPUDevice  device;
    WGPUQueue   queue;
} WebGPUDevice;

typedef struct {
    WGPUBuffer    buffer;
    size_t        size;
    WebGPUDevice *dev;
} WebGPUBuffer;

typedef struct {
    WGPUShaderModule    shader;
    WGPUComputePipeline pipeline;
    WGPUBindGroupLayout bind_layout;
    WebGPUDevice       *dev;
} WebGPUKernel;

/* ------------------------------------------------------------------ */
/*  Global state                                                      */
/* ------------------------------------------------------------------ */

static WGPUInstance   g_instance     = NULL;
static WGPUAdapter   *g_adapters     = NULL;
static size_t         g_adapter_count = 0;

/* ------------------------------------------------------------------ */
/*  Async callback helpers                                            */
/* ------------------------------------------------------------------ */

typedef struct {
    int          done;
    WGPUDevice   device;
    WGPURequestDeviceStatus status;
} DeviceCallbackData;

static void on_device_request(WGPURequestDeviceStatus status, WGPUDevice device,
                              WGPUStringView message, void* userdata1, void* userdata2) {
    (void)message; (void)userdata2;
    DeviceCallbackData *d = (DeviceCallbackData*)userdata1;
    d->device = device;
    d->status = status;
    d->done = 1;
}

typedef struct {
    int          done;
    WGPUAdapter  adapter;
    WGPURequestAdapterStatus status;
} AdapterCallbackData;

static void on_adapter_request(WGPURequestAdapterStatus status, WGPUAdapter adapter,
                               WGPUStringView message, void* userdata1, void* userdata2) {
    (void)message; (void)userdata2;
    AdapterCallbackData *d = (AdapterCallbackData*)userdata1;
    d->adapter = adapter;
    d->status = status;
    d->done = 1;
}

typedef struct {
    int               done;
    WGPUMapAsyncStatus status;
} MapCallbackData;

static void on_buffer_mapped(WGPUMapAsyncStatus status, WGPUStringView message,
                             void* userdata1, void* userdata2) {
    (void)message; (void)userdata2;
    MapCallbackData *d = (MapCallbackData*)userdata1;
    d->status = status;
    d->done = 1;
}

/* ------------------------------------------------------------------ */
/*  Helper: submit a command encoder and wait                         */
/* ------------------------------------------------------------------ */

static void submit_and_wait(WebGPUDevice *dev, WGPUCommandEncoder encoder) {
    WGPUCommandBuffer cmd = p_wgpuCommandEncoderFinish(encoder, NULL);
    p_wgpuQueueSubmit(dev->queue, 1, &cmd);
    p_wgpuCommandBufferRelease(cmd);
    /* Poll until all GPU work completes */
#ifdef __EMSCRIPTEN__
    /* Dawn/Emscripten uses instance-level event processing */
    p_wgpuInstanceProcessEvents(g_instance);
#else
    p_wgpuDevicePoll(dev->device, 1, NULL);
#endif
}

/* ------------------------------------------------------------------ */
/*  Backend interface                                                 */
/* ------------------------------------------------------------------ */

static int webgpu_init(void) {
    if (open_wgpu_library() != 0) return -1;

    /* Create wgpu instance */
    WGPUInstanceDescriptor inst_desc = {0};
    g_instance = p_wgpuCreateInstance(&inst_desc);
    if (!g_instance) return -1;

#ifndef __EMSCRIPTEN__
    if (p_wgpuInstanceEnumerateAdapters) {
        /* wgpu-native path: enumerate all adapters synchronously */
        WGPUInstanceEnumerateAdapterOptions enum_opts = {
            .nextInChain = NULL,
            .backends = WGPUInstanceBackend_All,
        };
        g_adapter_count = p_wgpuInstanceEnumerateAdapters(g_instance, &enum_opts, NULL);
        if (g_adapter_count == 0) {
            p_wgpuInstanceRelease(g_instance);
            g_instance = NULL;
            return -1;
        }
        g_adapters = malloc(sizeof(WGPUAdapter) * g_adapter_count);
        if (!g_adapters) {
            p_wgpuInstanceRelease(g_instance);
            g_instance = NULL;
            return -1;
        }
        p_wgpuInstanceEnumerateAdapters(g_instance, &enum_opts, g_adapters);
    } else
#endif
    {
        /* Dawn/Emscripten path: request a single adapter via the standard API */
        AdapterCallbackData cb = {0};
        WGPURequestAdapterCallbackInfo cb_info = {
            .callback = on_adapter_request,
            .userdata1 = &cb,
        };
        p_wgpuInstanceRequestAdapter(g_instance, NULL, cb_info);
        /* Poll until the adapter request completes */
        while (!cb.done) {
            p_wgpuInstanceProcessEvents(g_instance);
        }
        if (cb.status != WGPURequestAdapterStatus_Success || !cb.adapter) {
            p_wgpuInstanceRelease(g_instance);
            g_instance = NULL;
            return -1;
        }
        g_adapter_count = 1;
        g_adapters = malloc(sizeof(WGPUAdapter));
        if (!g_adapters) {
            p_wgpuAdapterRelease(cb.adapter);
            p_wgpuInstanceRelease(g_instance);
            g_instance = NULL;
            return -1;
        }
        g_adapters[0] = cb.adapter;
    }

    return 0;
}

static void webgpu_shutdown(void) {
    if (g_adapters) {
        for (size_t i = 0; i < g_adapter_count; i++) {
            p_wgpuAdapterRelease(g_adapters[i]);
        }
        free(g_adapters);
        g_adapters = NULL;
    }
    g_adapter_count = 0;

    if (g_instance) {
        p_wgpuInstanceRelease(g_instance);
        g_instance = NULL;
    }

    if (g_wgpu_lib) {
        WGPU_DLCLOSE(g_wgpu_lib);
        g_wgpu_lib = NULL;
    }
}

static int webgpu_device_count(void) {
    return (int)g_adapter_count;
}

static int webgpu_device_info(int index, char* name, size_t name_len) {
    if (index < 0 || index >= (int)g_adapter_count) return -1;

    WGPUAdapterInfo info = {0};
    WGPUStatus status = p_wgpuAdapterGetInfo(g_adapters[index], &info);
    if (status != WGPUStatus_Success) return -1;

    /* Combine vendor + device description into a readable name */
    const char *desc = info.description.data;
    size_t desc_len = info.description.length;
    if (desc && desc_len > 0 && desc_len != WGPU_STRLEN) {
        snprintf(name, name_len, "WebGPU: %.*s", (int)desc_len, desc);
    } else if (desc) {
        snprintf(name, name_len, "WebGPU: %s", desc);
    } else {
        const char *dev = info.device.data;
        if (dev && info.device.length > 0 && info.device.length != WGPU_STRLEN) {
            snprintf(name, name_len, "WebGPU: %.*s", (int)info.device.length, dev);
        } else if (dev) {
            snprintf(name, name_len, "WebGPU: %s", dev);
        } else {
            snprintf(name, name_len, "WebGPU: adapter %d", index);
        }
    }
    name[name_len - 1] = '\0';

    p_wgpuAdapterInfoFreeMembers(info);
    return 0;
}

static void* webgpu_device_create(int index) {
    if (index < 0 || index >= (int)g_adapter_count) return NULL;

    WebGPUDevice *dev = calloc(1, sizeof(WebGPUDevice));
    if (!dev) return NULL;

    dev->adapter = g_adapters[index];

    /* Request device with default limits */
    WGPUDeviceDescriptor dev_desc = {0};
    dev_desc.label = (WGPUStringView){ .data = "mental", .length = 6 };
    dev_desc.defaultQueue.label = (WGPUStringView){ .data = "mental_queue", .length = 12 };

    DeviceCallbackData cb = { .done = 0 };
    WGPURequestDeviceCallbackInfo cb_info = {
        .nextInChain = NULL,
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = on_device_request,
        .userdata1 = &cb,
        .userdata2 = NULL,
    };

    p_wgpuAdapterRequestDevice(dev->adapter, &dev_desc, cb_info);
    while (!cb.done) {
        p_wgpuInstanceProcessEvents(g_instance);
    }

    if (cb.status != WGPURequestDeviceStatus_Success || !cb.device) {
        free(dev);
        return NULL;
    }

    dev->device = cb.device;
    dev->queue = p_wgpuDeviceGetQueue(dev->device);

    return dev;
}

static void webgpu_device_destroy(void* device) {
    if (!device) return;
    WebGPUDevice *dev = (WebGPUDevice*)device;
    if (dev->queue) p_wgpuQueueRelease(dev->queue);
    if (dev->device) {
        p_wgpuDeviceDestroy(dev->device);
        p_wgpuDeviceRelease(dev->device);
    }
    free(dev);
}

/* ------------------------------------------------------------------ */
/*  Buffer operations                                                 */
/* ------------------------------------------------------------------ */

static void* webgpu_buffer_alloc(void* device, size_t bytes) {
    WebGPUDevice *dev = (WebGPUDevice*)device;

    WGPUBufferDescriptor desc = {
        .nextInChain = NULL,
        .label = { .data = "mental_buf", .length = 10 },
        .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst,
        .size = (uint64_t)bytes,
        .mappedAtCreation = 0,
    };

    WGPUBuffer buffer = p_wgpuDeviceCreateBuffer(dev->device, &desc);
    if (!buffer) return NULL;

    WebGPUBuffer *buf = malloc(sizeof(WebGPUBuffer));
    if (!buf) {
        p_wgpuBufferDestroy(buffer);
        p_wgpuBufferRelease(buffer);
        return NULL;
    }
    buf->buffer = buffer;
    buf->size = bytes;
    buf->dev = dev;
    return buf;
}

static void webgpu_buffer_write(void* buf_handle, const void* data, size_t bytes) {
    WebGPUBuffer *buf = (WebGPUBuffer*)buf_handle;
    p_wgpuQueueWriteBuffer(buf->dev->queue, buf->buffer, 0, data, bytes);
}

static void webgpu_buffer_read(void* buf_handle, void* data, size_t bytes) {
    WebGPUBuffer *buf = (WebGPUBuffer*)buf_handle;
    WebGPUDevice *dev = buf->dev;

    /* Create a staging buffer with MapRead + CopyDst usage */
    WGPUBufferDescriptor staging_desc = {
        .nextInChain = NULL,
        .label = { .data = "staging_read", .length = 12 },
        .usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst,
        .size = (uint64_t)bytes,
        .mappedAtCreation = 0,
    };
    WGPUBuffer staging = p_wgpuDeviceCreateBuffer(dev->device, &staging_desc);
    if (!staging) return;

    /* Copy source buffer to staging buffer via command encoder */
    WGPUCommandEncoder encoder = p_wgpuDeviceCreateCommandEncoder(dev->device, NULL);
    p_wgpuCommandEncoderCopyBufferToBuffer(encoder, buf->buffer, 0, staging, 0, (uint64_t)bytes);
    submit_and_wait(dev, encoder);
    p_wgpuCommandEncoderRelease(encoder);

    /* Map the staging buffer for reading */
    MapCallbackData map_cb = { .done = 0 };
    WGPUBufferMapCallbackInfo map_info = {
        .nextInChain = NULL,
        .mode = WGPUCallbackMode_AllowProcessEvents,
        .callback = on_buffer_mapped,
        .userdata1 = &map_cb,
        .userdata2 = NULL,
    };

    p_wgpuBufferMapAsync(staging, WGPUMapMode_Read, 0, bytes, map_info);
    while (!map_cb.done) {
        p_wgpuInstanceProcessEvents(g_instance);
    }

    if (map_cb.status == WGPUMapAsyncStatus_Success) {
        const void *mapped = p_wgpuBufferGetConstMappedRange(staging, 0, bytes);
        if (mapped) {
            memcpy(data, mapped, bytes);
        }
        p_wgpuBufferUnmap(staging);
    }

    p_wgpuBufferDestroy(staging);
    p_wgpuBufferRelease(staging);
}

static void* webgpu_buffer_resize(void* device, void* old_buf, size_t old_size, size_t new_size) {
    WebGPUDevice *dev = (WebGPUDevice*)device;
    WebGPUBuffer *old = (WebGPUBuffer*)old_buf;

    /* Allocate new buffer */
    WGPUBufferDescriptor desc = {
        .nextInChain = NULL,
        .label = { .data = "mental_buf", .length = 10 },
        .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst,
        .size = (uint64_t)new_size,
        .mappedAtCreation = 0,
    };
    WGPUBuffer new_buffer = p_wgpuDeviceCreateBuffer(dev->device, &desc);
    if (!new_buffer) return NULL;

    /* Copy old data into new buffer */
    size_t copy_size = old_size < new_size ? old_size : new_size;
    WGPUCommandEncoder encoder = p_wgpuDeviceCreateCommandEncoder(dev->device, NULL);
    p_wgpuCommandEncoderCopyBufferToBuffer(encoder, old->buffer, 0, new_buffer, 0, (uint64_t)copy_size);
    submit_and_wait(dev, encoder);
    p_wgpuCommandEncoderRelease(encoder);

    /* Destroy old buffer, reuse the wrapper */
    p_wgpuBufferDestroy(old->buffer);
    p_wgpuBufferRelease(old->buffer);
    old->buffer = new_buffer;
    old->size = new_size;

    return old_buf;
}

static void* webgpu_buffer_clone(void* device, void* src_buf, size_t size) {
    WebGPUDevice *dev = (WebGPUDevice*)device;
    WebGPUBuffer *src = (WebGPUBuffer*)src_buf;

    /* Allocate new buffer */
    WGPUBufferDescriptor desc = {
        .nextInChain = NULL,
        .label = { .data = "mental_buf", .length = 10 },
        .usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst,
        .size = (uint64_t)size,
        .mappedAtCreation = 0,
    };
    WGPUBuffer new_buffer = p_wgpuDeviceCreateBuffer(dev->device, &desc);
    if (!new_buffer) return NULL;

    /* Copy source to new buffer */
    WGPUCommandEncoder encoder = p_wgpuDeviceCreateCommandEncoder(dev->device, NULL);
    p_wgpuCommandEncoderCopyBufferToBuffer(encoder, src->buffer, 0, new_buffer, 0, (uint64_t)size);
    submit_and_wait(dev, encoder);
    p_wgpuCommandEncoderRelease(encoder);

    WebGPUBuffer *clone = malloc(sizeof(WebGPUBuffer));
    if (!clone) {
        p_wgpuBufferDestroy(new_buffer);
        p_wgpuBufferRelease(new_buffer);
        return NULL;
    }
    clone->buffer = new_buffer;
    clone->size = size;
    clone->dev = dev;

    return clone;
}

static void webgpu_buffer_destroy(void* buf_handle) {
    if (!buf_handle) return;
    WebGPUBuffer *buf = (WebGPUBuffer*)buf_handle;
    p_wgpuBufferDestroy(buf->buffer);
    p_wgpuBufferRelease(buf->buffer);
    free(buf);
}

/* ------------------------------------------------------------------ */
/*  Kernel operations                                                 */
/* ------------------------------------------------------------------ */

static void* webgpu_kernel_compile(void* device, const char* source, size_t source_len,
                                    char* error, size_t error_len) {
    WebGPUDevice *dev = (WebGPUDevice*)device;

    /* Create WGSL shader module */
    WGPUShaderSourceWGSL wgsl_desc = {
        .chain = {
            .next = NULL,
            .sType = WGPUSType_ShaderSourceWGSL,
        },
        .code = {
            .data = source,
            .length = source_len,
        },
    };

    WGPUShaderModuleDescriptor shader_desc = {
        .nextInChain = (const WGPUChainedStruct*)&wgsl_desc,
        .label = { .data = "mental_shader", .length = 13 },
    };

    WGPUShaderModule shader = p_wgpuDeviceCreateShaderModule(dev->device, &shader_desc);
    if (!shader) {
        if (error) snprintf(error, error_len, "WebGPU: failed to create shader module");
        return NULL;
    }

    /* Create compute pipeline with auto layout */
    WGPUComputePipelineDescriptor pipeline_desc = {
        .nextInChain = NULL,
        .label = { .data = "mental_pipeline", .length = 15 },
        .layout = NULL, /* auto layout */
        .compute = {
            .nextInChain = NULL,
            .module = shader,
            .entryPoint = { .data = "main", .length = 4 },
            .constantCount = 0,
            .constants = NULL,
        },
    };

    WGPUComputePipeline pipeline = p_wgpuDeviceCreateComputePipeline(dev->device, &pipeline_desc);
    if (!pipeline) {
        if (error) snprintf(error, error_len, "WebGPU: failed to create compute pipeline");
        p_wgpuShaderModuleRelease(shader);
        return NULL;
    }

    /* Get the auto-generated bind group layout for group 0 */
    WGPUBindGroupLayout bind_layout = p_wgpuComputePipelineGetBindGroupLayout(pipeline, 0);
    if (!bind_layout) {
        if (error) snprintf(error, error_len, "WebGPU: failed to get bind group layout");
        p_wgpuComputePipelineRelease(pipeline);
        p_wgpuShaderModuleRelease(shader);
        return NULL;
    }

    WebGPUKernel *k = malloc(sizeof(WebGPUKernel));
    if (!k) {
        p_wgpuBindGroupLayoutRelease(bind_layout);
        p_wgpuComputePipelineRelease(pipeline);
        p_wgpuShaderModuleRelease(shader);
        return NULL;
    }

    k->shader = shader;
    k->pipeline = pipeline;
    k->bind_layout = bind_layout;
    k->dev = dev;

    return k;
}

static int webgpu_kernel_workgroup_size(void* kernel) {
    (void)kernel;
    /* Default workgroup size for WebGPU compute shaders.  Matches the
     * @workgroup_size(64) declaration in transpiled WGSL. */
    return 64;
}

static void webgpu_kernel_dispatch(void* kernel, void** inputs, int input_count,
                                    void** outputs, int output_count, int work_size) {
    WebGPUKernel *k = (WebGPUKernel*)kernel;
    WebGPUDevice *dev = k->dev;

    /* Build bind group entries: inputs at bindings 0..N-1, outputs at bindings N..N+M-1 */
    int total_bindings = input_count + output_count;
    WGPUBindGroupEntry *entries = calloc(total_bindings, sizeof(WGPUBindGroupEntry));
    if (!entries) return;

    for (int i = 0; i < input_count; i++) {
        WebGPUBuffer *in_buf = (WebGPUBuffer*)inputs[i];
        entries[i] = (WGPUBindGroupEntry){
            .nextInChain = NULL,
            .binding = (uint32_t)i,
            .buffer = in_buf->buffer,
            .offset = 0,
            .size = (uint64_t)in_buf->size,
            .sampler = NULL,
            .textureView = NULL,
        };
    }

    for (int i = 0; i < output_count; i++) {
        WebGPUBuffer *out_buf = (WebGPUBuffer*)outputs[i];
        entries[input_count + i] = (WGPUBindGroupEntry){
            .nextInChain = NULL,
            .binding = (uint32_t)(input_count + i),
            .buffer = out_buf->buffer,
            .offset = 0,
            .size = (uint64_t)out_buf->size,
            .sampler = NULL,
            .textureView = NULL,
        };
    }

    WGPUBindGroupDescriptor bg_desc = {
        .nextInChain = NULL,
        .label = { .data = "mental_bg", .length = 9 },
        .layout = k->bind_layout,
        .entryCount = (size_t)total_bindings,
        .entries = entries,
    };

    WGPUBindGroup bind_group = p_wgpuDeviceCreateBindGroup(dev->device, &bg_desc);
    free(entries);
    if (!bind_group) return;

    /* Record and submit compute pass */
    WGPUCommandEncoder encoder = p_wgpuDeviceCreateCommandEncoder(dev->device, NULL);
    WGPUComputePassEncoder pass = p_wgpuCommandEncoderBeginComputePass(encoder, NULL);

    p_wgpuComputePassEncoderSetPipeline(pass, k->pipeline);
    p_wgpuComputePassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);

    /* Dispatch: work_size elements, using the backend's workgroup size */
    uint32_t wg_size = (uint32_t)webgpu_kernel_workgroup_size(kernel);
    uint32_t workgroup_count = ((uint32_t)work_size + wg_size - 1) / wg_size;
    p_wgpuComputePassEncoderDispatchWorkgroups(pass, workgroup_count, 1, 1);

    p_wgpuComputePassEncoderEnd(pass);
    p_wgpuComputePassEncoderRelease(pass);

    submit_and_wait(dev, encoder);
    p_wgpuCommandEncoderRelease(encoder);

    p_wgpuBindGroupRelease(bind_group);
}

static void webgpu_kernel_destroy(void* kernel) {
    if (!kernel) return;
    WebGPUKernel *k = (WebGPUKernel*)kernel;
    if (k->bind_layout) p_wgpuBindGroupLayoutRelease(k->bind_layout);
    if (k->pipeline) p_wgpuComputePipelineRelease(k->pipeline);
    if (k->shader) p_wgpuShaderModuleRelease(k->shader);
    free(k);
}

/* ── Pipe ──────────────────────────────────────────────────────── */

typedef struct {
    WebGPUDevice       *dev;
    WGPUCommandEncoder  encoder;
    int                 dispatch_count;
} WebGPUPipe;

static void* webgpu_pipe_create(void* device) {
    WebGPUDevice* dev = (WebGPUDevice*)device;

    WebGPUPipe* pipe = malloc(sizeof(WebGPUPipe));
    if (!pipe) return NULL;
    pipe->dev = dev;
    pipe->dispatch_count = 0;

    /* Create command encoder */
    pipe->encoder = p_wgpuDeviceCreateCommandEncoder(dev->device, NULL);
    if (!pipe->encoder) {
        free(pipe);
        return NULL;
    }

    return pipe;
}

static int webgpu_pipe_add(void* pipe_ptr, void* kernel, void** inputs,
                            int input_count, void** outputs, int output_count,
                            int work_size) {
    WebGPUPipe* pipe = (WebGPUPipe*)pipe_ptr;
    WebGPUKernel* k = (WebGPUKernel*)kernel;
    WebGPUDevice* dev = pipe->dev;

    /* Each dispatch gets its own compute pass to ensure storage buffer
     * writes from one dispatch are visible to the next.  WebGPU does
     * not guarantee visibility within a single pass. */
    WGPUComputePassEncoder pass = p_wgpuCommandEncoderBeginComputePass(pipe->encoder, NULL);
    if (!pass) return -1;

    /* Build bind group entries */
    int total_bindings = input_count + output_count;
    WGPUBindGroupEntry* entries = calloc(total_bindings, sizeof(WGPUBindGroupEntry));
    if (!entries) {
        p_wgpuComputePassEncoderEnd(pass);
        p_wgpuComputePassEncoderRelease(pass);
        return -1;
    }

    for (int i = 0; i < input_count; i++) {
        WebGPUBuffer* in_buf = (WebGPUBuffer*)inputs[i];
        entries[i] = (WGPUBindGroupEntry){
            .nextInChain = NULL,
            .binding = (uint32_t)i,
            .buffer = in_buf->buffer,
            .offset = 0,
            .size = (uint64_t)in_buf->size,
            .sampler = NULL,
            .textureView = NULL,
        };
    }

    for (int i = 0; i < output_count; i++) {
        WebGPUBuffer* out_buf = (WebGPUBuffer*)outputs[i];
        entries[input_count + i] = (WGPUBindGroupEntry){
            .nextInChain = NULL,
            .binding = (uint32_t)(input_count + i),
            .buffer = out_buf->buffer,
            .offset = 0,
            .size = (uint64_t)out_buf->size,
            .sampler = NULL,
            .textureView = NULL,
        };
    }

    WGPUBindGroupDescriptor bg_desc = {
        .nextInChain = NULL,
        .label = { .data = "pipe_bg", .length = 7 },
        .layout = k->bind_layout,
        .entryCount = (size_t)total_bindings,
        .entries = entries,
    };

    WGPUBindGroup bind_group = p_wgpuDeviceCreateBindGroup(dev->device, &bg_desc);
    free(entries);
    if (!bind_group) {
        p_wgpuComputePassEncoderEnd(pass);
        p_wgpuComputePassEncoderRelease(pass);
        return -1;
    }

    /* Set pipeline, bind group, and dispatch */
    p_wgpuComputePassEncoderSetPipeline(pass, k->pipeline);
    p_wgpuComputePassEncoderSetBindGroup(pass, 0, bind_group, 0, NULL);

    uint32_t wg_size = (uint32_t)webgpu_kernel_workgroup_size(kernel);
    uint32_t workgroup_count = ((uint32_t)work_size + wg_size - 1) / wg_size;
    p_wgpuComputePassEncoderDispatchWorkgroups(pass, workgroup_count, 1, 1);

    /* End this pass — the next add will create a fresh one, giving
     * the WebGPU runtime an implicit barrier between passes. */
    p_wgpuComputePassEncoderEnd(pass);
    p_wgpuComputePassEncoderRelease(pass);
    p_wgpuBindGroupRelease(bind_group);

    pipe->dispatch_count++;
    return 0;
}

static int webgpu_pipe_execute(void* pipe_ptr) {
    WebGPUPipe* pipe = (WebGPUPipe*)pipe_ptr;

    /* Finish encoding and submit */
    WGPUCommandBuffer cmd = p_wgpuCommandEncoderFinish(pipe->encoder, NULL);
    p_wgpuQueueSubmit(pipe->dev->queue, 1, &cmd);
    p_wgpuCommandBufferRelease(cmd);

    /* Poll until all GPU work completes */
#ifdef __EMSCRIPTEN__
    p_wgpuInstanceProcessEvents(g_instance);
#else
    p_wgpuDevicePoll(pipe->dev->device, 1, NULL);
#endif

    return 0;
}

static void webgpu_pipe_destroy(void* pipe_ptr) {
    if (!pipe_ptr) return;
    WebGPUPipe* pipe = (WebGPUPipe*)pipe_ptr;
    if (pipe->encoder) p_wgpuCommandEncoderRelease(pipe->encoder);
    free(pipe);
}

/* ------------------------------------------------------------------ */
/*  WASM Readback Viewport                                            */
/*  In WASM the library doesn't own the display surface — the host    */
/*  (browser/Node.js) does. The viewport renders into an internal     */
/*  RGBA framebuffer that the host reads via mental_viewport_read().  */
/* ------------------------------------------------------------------ */

#ifdef __EMSCRIPTEN__

typedef struct {
    WebGPUBuffer *buffer;       /* source GPU buffer */
    WebGPUDevice *dev;          /* device context */
    void         *framebuffer;  /* malloc'd pixel data */
    size_t        fb_size;      /* byte length */
} WasmViewport;

static void* wasm_viewport_attach(void* dev, void* buffer, void* surface,
                                   char* error, size_t error_len) {
    (void)surface;  /* WASM readback ignores the surface parameter */
    WebGPUBuffer *wbuf = (WebGPUBuffer*)buffer;

    WasmViewport *vp = calloc(1, sizeof(WasmViewport));
    if (!vp) {
        if (error) snprintf(error, error_len, "Failed to allocate WASM viewport");
        return NULL;
    }

    vp->buffer = wbuf;
    vp->dev = (WebGPUDevice*)dev;
    vp->fb_size = wbuf->size;
    vp->framebuffer = malloc(vp->fb_size);
    if (!vp->framebuffer) {
        if (error) snprintf(error, error_len, "Failed to allocate WASM framebuffer (%zu bytes)", vp->fb_size);
        free(vp);
        return NULL;
    }

    return vp;
}

static void wasm_viewport_present(void* viewport_ptr) {
    if (!viewport_ptr) return;
    WasmViewport *vp = (WasmViewport*)viewport_ptr;

    /* Copy GPU buffer contents into the internal framebuffer.
     * In WASM the "GPU buffer" is CPU-accessible, so this is essentially memcpy. */
    webgpu_buffer_read(vp->buffer, vp->framebuffer, vp->fb_size);
}

static const void* wasm_viewport_readback(void* viewport_ptr, size_t *out_size) {
    if (!viewport_ptr) return NULL;
    WasmViewport *vp = (WasmViewport*)viewport_ptr;
    if (out_size) *out_size = vp->fb_size;
    return vp->framebuffer;
}

static void wasm_viewport_detach(void* viewport_ptr) {
    if (!viewport_ptr) return;
    WasmViewport *vp = (WasmViewport*)viewport_ptr;
    free(vp->framebuffer);
    free(vp);
}

#endif /* __EMSCRIPTEN__ */

/* ------------------------------------------------------------------ */
/*  Backend descriptor                                                */
/* ------------------------------------------------------------------ */

static mental_backend g_webgpu_backend = {
    .name = "WebGPU",
    .api = MENTAL_API_WEBGPU,
    .init = webgpu_init,
    .shutdown = webgpu_shutdown,
    .device_count = webgpu_device_count,
    .device_info = webgpu_device_info,
    .device_create = webgpu_device_create,
    .device_destroy = webgpu_device_destroy,
    .buffer_alloc = webgpu_buffer_alloc,
    .buffer_write = webgpu_buffer_write,
    .buffer_read = webgpu_buffer_read,
    .buffer_resize = webgpu_buffer_resize,
    .buffer_clone = webgpu_buffer_clone,
    .buffer_destroy = webgpu_buffer_destroy,
    .kernel_compile = webgpu_kernel_compile,
    .kernel_workgroup_size = webgpu_kernel_workgroup_size,
    .kernel_dispatch = webgpu_kernel_dispatch,
    .kernel_destroy = webgpu_kernel_destroy,
    .pipe_create = webgpu_pipe_create,
    .pipe_add = webgpu_pipe_add,
    .pipe_execute = webgpu_pipe_execute,
    .pipe_destroy = webgpu_pipe_destroy,
#ifdef __EMSCRIPTEN__
    .viewport_attach  = wasm_viewport_attach,
    .viewport_present = wasm_viewport_present,
    .viewport_detach  = wasm_viewport_detach,
    .viewport_readback = wasm_viewport_readback,
#else
    .viewport_attach  = NULL,
    .viewport_present = NULL,
    .viewport_detach  = NULL,
    .viewport_readback = NULL,
#endif
};

mental_backend* webgpu_backend = &g_webgpu_backend;

/* Probe for wgpu-native at load time so the state system can report
 * availability even when WebGPU isn't the active backend. */
__attribute__((constructor))
static void webgpu_probe(void) {
    void *lib = try_load_wgpu();
    if (lib) {
        mental_register_library("wgpu", NULL, 1);
        WGPU_DLCLOSE(lib);
    } else {
        mental_register_library("wgpu", NULL, 0);
    }
}

#else
/* WebGPU not available at build time */
#include "mental_internal.h"
#include <stddef.h>
mental_backend* webgpu_backend = NULL;
#endif /* MENTAL_HAS_WEBGPU */
