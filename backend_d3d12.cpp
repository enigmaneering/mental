/*
 * Mental - D3D12 Backend (Windows, runtime-loaded)
 *
 * d3d12.dll and dxgi.dll are loaded at runtime via LoadLibraryA / GetProcAddress.
 * Only three free functions need dlsym:
 *   - D3D12CreateDevice        (d3d12.dll)
 *   - D3D12SerializeRootSignature (d3d12.dll)
 *   - CreateDXGIFactory2       (dxgi.dll)
 * All other D3D12/DXGI calls go through COM vtable pointers on the returned
 * interface objects, so they don't need explicit symbol resolution.
 *
 * On non-Windows platforms the file exports d3d12_backend = NULL.
 */

#ifdef _WIN32

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include "mental_internal.h"
#include "transpile.h"
#include <vector>
#include <string>
#include <stdio.h>
#include <stdlib.h>
#include <windows.h>

using Microsoft::WRL::ComPtr;

/* ── Runtime-loaded function pointers ────────────────────────────── */

static HMODULE g_d3d12_dll = NULL;
static HMODULE g_dxgi_dll  = NULL;

/* d3d12.dll */
typedef HRESULT (WINAPI *PFN_D3D12CreateDevice)(
    IUnknown* pAdapter, D3D_FEATURE_LEVEL MinimumFeatureLevel,
    REFIID riid, void** ppDevice);
typedef HRESULT (WINAPI *PFN_D3D12SerializeRootSignature)(
    const D3D12_ROOT_SIGNATURE_DESC* pRootSignature,
    D3D_ROOT_SIGNATURE_VERSION Version, ID3DBlob** ppBlob, ID3DBlob** ppErrorBlob);

/* dxgi.dll */
typedef HRESULT (WINAPI *PFN_CreateDXGIFactory2)(
    UINT Flags, REFIID riid, void** ppFactory);

static PFN_D3D12CreateDevice          pfn_D3D12CreateDevice          = NULL;
static PFN_D3D12SerializeRootSignature pfn_D3D12SerializeRootSignature = NULL;
static PFN_CreateDXGIFactory2          pfn_CreateDXGIFactory2          = NULL;

static int d3d12_load_libraries(void) {
    g_d3d12_dll = LoadLibraryA("d3d12.dll");
    if (!g_d3d12_dll) return -1;

    g_dxgi_dll = LoadLibraryA("dxgi.dll");
    if (!g_dxgi_dll) {
        FreeLibrary(g_d3d12_dll);
        g_d3d12_dll = NULL;
        return -1;
    }

    pfn_D3D12CreateDevice = (PFN_D3D12CreateDevice)
        GetProcAddress(g_d3d12_dll, "D3D12CreateDevice");
    pfn_D3D12SerializeRootSignature = (PFN_D3D12SerializeRootSignature)
        GetProcAddress(g_d3d12_dll, "D3D12SerializeRootSignature");
    pfn_CreateDXGIFactory2 = (PFN_CreateDXGIFactory2)
        GetProcAddress(g_dxgi_dll, "CreateDXGIFactory2");

    if (!pfn_D3D12CreateDevice || !pfn_D3D12SerializeRootSignature || !pfn_CreateDXGIFactory2) {
        FreeLibrary(g_dxgi_dll);  g_dxgi_dll  = NULL;
        FreeLibrary(g_d3d12_dll); g_d3d12_dll = NULL;
        pfn_D3D12CreateDevice          = NULL;
        pfn_D3D12SerializeRootSignature = NULL;
        pfn_CreateDXGIFactory2          = NULL;
        return -1;
    }

    return 0;
}

static void d3d12_free_libraries(void) {
    if (g_dxgi_dll)  { FreeLibrary(g_dxgi_dll);  g_dxgi_dll  = NULL; }
    if (g_d3d12_dll) { FreeLibrary(g_d3d12_dll); g_d3d12_dll = NULL; }
    pfn_D3D12CreateDevice          = NULL;
    pfn_D3D12SerializeRootSignature = NULL;
    pfn_CreateDXGIFactory2          = NULL;
}

/* ── D3D12 type wrappers ────────────────────────────────────────── */

/* D3D12 device wrapper */
typedef struct {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command_list;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value;
    HANDLE fence_event;
} D3D12Device;

/* D3D12 buffer wrapper - three-buffer system for proper D3D12 buffer management */
typedef struct {
    ComPtr<ID3D12Resource> resource;        /* DEFAULT heap - GPU-accessible with UAV */
    ComPtr<ID3D12Resource> upload_buffer;   /* UPLOAD heap - CPU writes */
    ComPtr<ID3D12Resource> readback_buffer; /* READBACK heap - CPU reads */
    void* mapped_ptr;                       /* Points to mapped upload buffer */
    size_t size;
    D3D12Device* device;
} D3D12Buffer;

/* D3D12 kernel wrapper */
typedef struct {
    ComPtr<ID3D12PipelineState> pipeline;
    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12DescriptorHeap> descriptor_heap;
    ComPtr<ID3D12Device> device;
} D3D12Kernel;

/* D3D12 viewport wrapper */
typedef struct {
    ComPtr<IDXGISwapChain3> swapChain;  /* NULL in headless/single-buffer mode */
    ComPtr<ID3D12Resource> renderTarget; /* single buffer when no swapchain */
    D3D12Buffer* buffer;
    D3D12Device* device;
    UINT bufferIndex;
    int headless;  /* 1 = no swapchain, single render target */
} D3D12Viewport;

/* Global D3D12 state */
static std::vector<ComPtr<IDXGIAdapter1>> g_adapters;

/* Forward declarations */
static void d3d12_buffer_destroy(void* buf);

static int d3d12_init(void) {
    /* Load d3d12.dll and dxgi.dll at runtime */
    if (d3d12_load_libraries() != 0) return -1;

    HRESULT hr;

    /* Create DXGI factory */
    ComPtr<IDXGIFactory6> factory;
    hr = pfn_CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        d3d12_free_libraries();
        return -1;
    }

    /* Enumerate adapters */
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                          IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        /* Skip software adapters */
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }

        /* Check if D3D12 is supported with Feature Level 12_0+.
         * FL 12_0 guarantees Shader Model 6.0 and compute pipeline support.
         * Virtual adapters (e.g. Parallels) often expose D3D12 at FL 11_x
         * which supports rendering but not cs_6_0 compute pipelines.
         * Requiring FL 12_0 lets the fallback chain drop to D3D11. */
        if (SUCCEEDED(pfn_D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                                             __uuidof(ID3D12Device), nullptr))) {
            g_adapters.push_back(adapter);
        }
    }

    if (g_adapters.empty()) {
        d3d12_free_libraries();
        return -1;
    }

    return 0;
}

static void d3d12_shutdown(void) {
    g_adapters.clear();
    d3d12_free_libraries();
}

static int d3d12_device_count(void) {
    return (int)g_adapters.size();
}

static int d3d12_device_info(int index, char* name, size_t name_len) {
    if (index < 0 || index >= (int)g_adapters.size()) return -1;

    DXGI_ADAPTER_DESC1 desc;
    g_adapters[index]->GetDesc1(&desc);

    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, (int)name_len, NULL, NULL);
    return 0;
}

static void* d3d12_device_create(int index) {
    if (index < 0 || index >= (int)g_adapters.size()) return NULL;

    D3D12Device* dev = new D3D12Device();

    /* Create device */
    HRESULT hr = pfn_D3D12CreateDevice(g_adapters[index].Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&dev->device));
    if (FAILED(hr)) {
        delete dev;
        return NULL;
    }

    /* Create command queue */
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    queue_desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    hr = dev->device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&dev->queue));
    if (FAILED(hr)) {
        delete dev;
        return NULL;
    }

    /* Create command allocator */
    hr = dev->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                               IID_PPV_ARGS(&dev->allocator));
    if (FAILED(hr)) {
        delete dev;
        return NULL;
    }

    /* Create command list */
    hr = dev->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                         dev->allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&dev->command_list));
    if (FAILED(hr)) {
        delete dev;
        return NULL;
    }
    dev->command_list->Close();  /* Start closed */

    /* Create fence for synchronization */
    dev->fence_value = 0;
    hr = dev->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&dev->fence));
    if (FAILED(hr)) {
        delete dev;
        return NULL;
    }

    dev->fence_event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!dev->fence_event) {
        delete dev;
        return NULL;
    }

    return dev;
}

static void d3d12_device_destroy(void* dev) {
    if (!dev) return;

    D3D12Device* d3d_dev = (D3D12Device*)dev;
    if (d3d_dev->fence_event) {
        CloseHandle(d3d_dev->fence_event);
    }
    delete d3d_dev;
}

static void* d3d12_buffer_alloc(void* dev, size_t bytes) {
    D3D12Device* d3d_dev = (D3D12Device*)dev;

    D3D12Buffer* buf = new D3D12Buffer();
    buf->size = bytes;
    buf->device = d3d_dev;
    buf->mapped_ptr = nullptr;

    /* Resource description (same for all buffers) */
    D3D12_RESOURCE_DESC resource_desc = {};
    resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resource_desc.Width = bytes;
    resource_desc.Height = 1;
    resource_desc.DepthOrArraySize = 1;
    resource_desc.MipLevels = 1;
    resource_desc.Format = DXGI_FORMAT_UNKNOWN;
    resource_desc.SampleDesc.Count = 1;
    resource_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    /* 1. Create DEFAULT heap buffer (GPU-accessible, UAV-enabled for compute) */
    D3D12_HEAP_PROPERTIES default_heap = {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    HRESULT hr = d3d_dev->device->CreateCommittedResource(
        &default_heap,
        D3D12_HEAP_FLAG_NONE,
        &resource_desc,
        D3D12_RESOURCE_STATE_COMMON,
        nullptr,
        IID_PPV_ARGS(&buf->resource));

    if (FAILED(hr)) {
        delete buf;
        return NULL;
    }

    /* 2. Create UPLOAD buffer (for CPU -> GPU transfers) */
    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    resource_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = d3d_dev->device->CreateCommittedResource(
        &upload_heap,
        D3D12_HEAP_FLAG_NONE,
        &resource_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&buf->upload_buffer));

    if (FAILED(hr)) {
        delete buf;
        return NULL;
    }

    /* 3. Create READBACK buffer (for GPU -> CPU transfers) */
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;

    hr = d3d_dev->device->CreateCommittedResource(
        &readback_heap,
        D3D12_HEAP_FLAG_NONE,
        &resource_desc,
        D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,
        IID_PPV_ARGS(&buf->readback_buffer));

    if (FAILED(hr)) {
        delete buf;
        return NULL;
    }

    /* Map the UPLOAD buffer for CPU writes (stays mapped) */
    hr = buf->upload_buffer->Map(0, nullptr, &buf->mapped_ptr);
    if (FAILED(hr)) {
        delete buf;
        return NULL;
    }

    return buf;
}

/* Helper to wait for GPU */
static void d3d12_wait_for_gpu(D3D12Device* dev) {
    dev->fence_value++;
    dev->queue->Signal(dev->fence.Get(), dev->fence_value);
    if (dev->fence->GetCompletedValue() < dev->fence_value) {
        dev->fence->SetEventOnCompletion(dev->fence_value, dev->fence_event);
        WaitForSingleObject(dev->fence_event, INFINITE);
    }
}

static void d3d12_buffer_write(void* buf, const void* data, size_t bytes) {
    D3D12Buffer* d3d_buf = (D3D12Buffer*)buf;
    D3D12Device* dev = d3d_buf->device;

    /* Write to mapped UPLOAD buffer */
    memcpy(d3d_buf->mapped_ptr, data, bytes);

    /* Flush: Copy UPLOAD -> DEFAULT with proper state transitions */
    dev->allocator->Reset();
    dev->command_list->Reset(dev->allocator.Get(), nullptr);

    /* Transition DEFAULT buffer to COPY_DEST */
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = d3d_buf->resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dev->command_list->ResourceBarrier(1, &barrier);

    /* Copy from UPLOAD to DEFAULT */
    dev->command_list->CopyResource(d3d_buf->resource.Get(), d3d_buf->upload_buffer.Get());

    /* Transition DEFAULT buffer to UNORDERED_ACCESS for compute */
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    dev->command_list->ResourceBarrier(1, &barrier);

    dev->command_list->Close();

    /* Execute and wait */
    ID3D12CommandList* cmd_lists[] = { dev->command_list.Get() };
    dev->queue->ExecuteCommandLists(1, cmd_lists);
    d3d12_wait_for_gpu(dev);
}

static void d3d12_buffer_read(void* buf, void* data, size_t bytes) {
    D3D12Buffer* d3d_buf = (D3D12Buffer*)buf;
    D3D12Device* dev = d3d_buf->device;

    /* Copy DEFAULT -> READBACK */
    dev->allocator->Reset();
    dev->command_list->Reset(dev->allocator.Get(), nullptr);

    /* Transition DEFAULT buffer to COPY_SOURCE */
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = d3d_buf->resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    dev->command_list->ResourceBarrier(1, &barrier);

    /* Copy from DEFAULT to READBACK */
    dev->command_list->CopyResource(d3d_buf->readback_buffer.Get(), d3d_buf->resource.Get());

    /* Transition DEFAULT buffer back to UNORDERED_ACCESS */
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    dev->command_list->ResourceBarrier(1, &barrier);

    dev->command_list->Close();

    /* Execute and wait */
    ID3D12CommandList* cmd_lists[] = { dev->command_list.Get() };
    dev->queue->ExecuteCommandLists(1, cmd_lists);
    d3d12_wait_for_gpu(dev);

    /* Map and read from READBACK buffer */
    void* read_ptr = nullptr;
    D3D12_RANGE read_range = {0, static_cast<SIZE_T>(bytes)};
    if (SUCCEEDED(d3d_buf->readback_buffer->Map(0, &read_range, &read_ptr))) {
        memcpy(data, read_ptr, bytes);
        d3d_buf->readback_buffer->Unmap(0, nullptr);
    }
}

static void* d3d12_buffer_resize(void* dev, void* old_buf, size_t old_size, size_t new_size) {
    D3D12Device* d3d_dev = (D3D12Device*)dev;
    D3D12Buffer* old_d3d_buf = (D3D12Buffer*)old_buf;

    /* Create new buffer */
    D3D12Buffer* new_buf = (D3D12Buffer*)d3d12_buffer_alloc(dev, new_size);
    if (!new_buf) return NULL;

    /* Copy old data */
    size_t copy_size = old_size < new_size ? old_size : new_size;
    memcpy(new_buf->mapped_ptr, old_d3d_buf->mapped_ptr, copy_size);

    /* Release old buffer */
    old_d3d_buf->resource->Unmap(0, nullptr);
    delete old_d3d_buf;

    /* Update old buffer pointer to point to new buffer */
    return new_buf;
}

static void* d3d12_buffer_clone(void* dev, void* src_buf, size_t size) {
    D3D12Device* d3d_dev = (D3D12Device*)dev;
    D3D12Buffer* src_d3d_buf = (D3D12Buffer*)src_buf;

    /* Allocate new buffer */
    D3D12Buffer* clone_buf = (D3D12Buffer*)d3d12_buffer_alloc(dev, size);
    if (!clone_buf) return NULL;

    /* Copy GPU-side data: DEFAULT -> READBACK -> temp -> UPLOAD -> DEFAULT */
    /* First, read from source DEFAULT buffer */
    char* temp_data = (char*)malloc(size);
    if (!temp_data) {
        d3d12_buffer_destroy(clone_buf);
        return NULL;
    }

    d3d12_buffer_read(src_buf, temp_data, size);

    /* Then write to clone (copies to DEFAULT buffer) */
    d3d12_buffer_write(clone_buf, temp_data, size);

    free(temp_data);
    return clone_buf;
}

static void d3d12_buffer_destroy(void* buf) {
    if (!buf) return;

    D3D12Buffer* d3d_buf = (D3D12Buffer*)buf;
    if (d3d_buf->mapped_ptr) {
        d3d_buf->upload_buffer->Unmap(0, nullptr);
    }
    delete d3d_buf;
}

/* Helper to compile HLSL to DXIL using DXC command-line tool */
static unsigned char* compile_hlsl_to_dxil(const char* hlsl_source, size_t hlsl_len,
                                            size_t* out_dxil_len, char* error, size_t error_len) {
    const char* dxc = mental_get_tool_path(MENTAL_TOOL_DXC);
    if (!dxc) {
        if (error) snprintf(error, error_len, "DXC compiler not found (set via mental_set_tool_path)");
        return nullptr;
    }

    /* Create temporary directory.
     * Use forward slashes throughout — MinGW/MSYS2 popen goes through sh
     * which can mangle backslash-escaped paths. Native Windows APIs and
     * DXC both accept forward slashes. */
    char temp_path[MAX_PATH];
    char temp_dir[MAX_PATH];
    GetTempPathA(MAX_PATH, temp_path);
    /* Normalize backslashes to forward slashes */
    for (char *p = temp_path; *p; p++) { if (*p == '\\') *p = '/'; }
    snprintf(temp_dir, sizeof(temp_dir), "%smental_hlsl_%lu", temp_path, (unsigned long)GetTickCount64());
    if (!CreateDirectoryA(temp_dir, NULL)) {
        if (error) snprintf(error, error_len, "Failed to create temp directory");
        return nullptr;
    }

    /* Write HLSL to temp file */
    char src_path[MAX_PATH];
    snprintf(src_path, sizeof(src_path), "%s/shader.hlsl", temp_dir);
    FILE* f = fopen(src_path, "wb");
    if (!f) {
        if (error) snprintf(error, error_len, "Failed to write HLSL source");
        RemoveDirectoryA(temp_dir);
        return nullptr;
    }
    fwrite(hlsl_source, 1, hlsl_len, f);
    fclose(f);

    /* Compile HLSL to DXIL using DXC */
    char out_path[MAX_PATH];
    char cmd[4096];
    snprintf(out_path, sizeof(out_path), "%s/shader.dxil", temp_dir);
    /* Normalize DXC path too */
    char dxc_norm[MAX_PATH];
    strncpy(dxc_norm, dxc, sizeof(dxc_norm) - 1);
    dxc_norm[sizeof(dxc_norm) - 1] = '\0';
    for (char *p = dxc_norm; *p; p++) { if (*p == '\\') *p = '/'; }
    snprintf(cmd, sizeof(cmd), "%s -T cs_6_0 -E main -Fo %s %s 2>&1",
             dxc_norm, out_path, src_path);

    FILE* pipe = _popen(cmd, "r");
    if (!pipe) {
        if (error) snprintf(error, error_len, "Failed to execute DXC compiler");
        DeleteFileA(src_path);
        RemoveDirectoryA(temp_dir);
        return nullptr;
    }

    char output_buf[4096] = {0};
    fread(output_buf, 1, sizeof(output_buf) - 1, pipe);
    int status = _pclose(pipe);

    if (status != 0) {
        if (error) snprintf(error, error_len, "DXC compilation failed: %s", output_buf);
        DeleteFileA(src_path);
        RemoveDirectoryA(temp_dir);
        return nullptr;
    }

    /* Read compiled DXIL bytecode */
    FILE* dxil_file = fopen(out_path, "rb");
    if (!dxil_file) {
        if (error) snprintf(error, error_len, "Failed to read DXIL output");
        DeleteFileA(src_path);
        DeleteFileA(out_path);
        RemoveDirectoryA(temp_dir);
        return nullptr;
    }

    fseek(dxil_file, 0, SEEK_END);
    long dxil_size = ftell(dxil_file);
    fseek(dxil_file, 0, SEEK_SET);

    unsigned char* dxil = (unsigned char*)malloc(dxil_size);
    if (!dxil) {
        if (error) snprintf(error, error_len, "Failed to allocate memory for DXIL");
        fclose(dxil_file);
        DeleteFileA(src_path);
        DeleteFileA(out_path);
        RemoveDirectoryA(temp_dir);
        return nullptr;
    }

    fread(dxil, 1, dxil_size, dxil_file);
    fclose(dxil_file);

    /* Cleanup temp files */
    DeleteFileA(src_path);
    DeleteFileA(out_path);
    RemoveDirectoryA(temp_dir);

    *out_dxil_len = dxil_size;
    return dxil;
}

static void* d3d12_kernel_compile(void* dev, const char* source, size_t source_len,
                                   char* error, size_t error_len) {
    D3D12Device* d3d_dev = (D3D12Device*)dev;

    /* Compile HLSL to DXIL bytecode */
    size_t dxil_len = 0;
    unsigned char* dxil = compile_hlsl_to_dxil(source, source_len, &dxil_len, error, error_len);
    if (!dxil) {
        return nullptr;  /* Error already set by compile_hlsl_to_dxil */
    }

    D3D12Kernel* kernel = new D3D12Kernel();
    kernel->device = d3d_dev->device;

    /* Create root signature with both UAV and SRV descriptor tables.
     *
     * Transpiled HLSL may use:
     *   - RWStructuredBuffer (UAV, register(u#)) for read-write buffers
     *   - StructuredBuffer (SRV, register(t#)) for read-only buffers
     *
     * WGSL var<storage, read> maps to SRV via Naga->spirv-cross, while
     * GLSL/HLSL typically uses RWStructuredBuffer (UAV) for everything.
     * Supporting both ensures all source languages work. */
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};

    /* UAVs: RWStructuredBuffer at register(u0..u15) */
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[0].NumDescriptors = 16;
    ranges[0].BaseShaderRegister = 0;
    ranges[0].RegisterSpace = 0;
    ranges[0].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    /* SRVs: StructuredBuffer at register(t0..t15) */
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[1].NumDescriptors = 16;
    ranges[1].BaseShaderRegister = 0;
    ranges[1].RegisterSpace = 0;
    ranges[1].OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER param = {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable.NumDescriptorRanges = 2;
    param.DescriptorTable.pDescriptorRanges = ranges;
    param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC root_sig_desc = {};
    root_sig_desc.NumParameters = 1;
    root_sig_desc.pParameters = &param;
    root_sig_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> sig_error;
    HRESULT hr = pfn_D3D12SerializeRootSignature(&root_sig_desc, D3D_ROOT_SIGNATURE_VERSION_1,
                                                  &signature, &sig_error);
    if (FAILED(hr)) {
        if (error && sig_error) {
            snprintf(error, error_len, "Failed to serialize root signature: %s",
                     (char*)sig_error->GetBufferPointer());
        }
        delete kernel;
        return NULL;
    }

    hr = d3d_dev->device->CreateRootSignature(0, signature->GetBufferPointer(),
                                               signature->GetBufferSize(),
                                               IID_PPV_ARGS(&kernel->root_signature));
    if (FAILED(hr)) {
        if (error) {
            snprintf(error, error_len, "Failed to create root signature");
        }
        delete kernel;
        return NULL;
    }

    /* Create compute pipeline state */
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = kernel->root_signature.Get();
    pso_desc.CS.pShaderBytecode = dxil;
    pso_desc.CS.BytecodeLength = dxil_len;

    hr = d3d_dev->device->CreateComputePipelineState(&pso_desc,
                                                      IID_PPV_ARGS(&kernel->pipeline));
    if (FAILED(hr)) {
        if (error) {
            snprintf(error, error_len, "Failed to create compute pipeline state (HRESULT: 0x%08X, bytecode size: %zu bytes)",
                     (unsigned int)hr, dxil_len);
        }
        free(dxil);
        delete kernel;
        return NULL;
    }

    /* DXIL bytecode no longer needed after pipeline creation */
    free(dxil);

    /* Create descriptor heap for UAVs + SRVs (16 each) */
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.NumDescriptors = 32;  /* 0..15 = UAVs, 16..31 = SRVs */
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

    hr = d3d_dev->device->CreateDescriptorHeap(&heap_desc,
                                                IID_PPV_ARGS(&kernel->descriptor_heap));
    if (FAILED(hr)) {
        if (error) {
            snprintf(error, error_len, "Failed to create descriptor heap");
        }
        delete kernel;
        return NULL;
    }

    return kernel;
}

static int d3d12_kernel_workgroup_size(void* kernel) {
    (void)kernel;
    /* D3D12 default thread group size -- matches the [numthreads(64,1,1)]
     * declaration used in transpiled HLSL shaders. */
    return 64;
}

static void d3d12_kernel_dispatch(void* kernel, void** inputs, int input_count,
                                   void** outputs, int output_count, int work_size) {
    if (!kernel) return;

    D3D12Kernel* d3d_kernel = (D3D12Kernel*)kernel;
    D3D12Buffer* first_output = (D3D12Buffer*)outputs[0];
    D3D12Device* d3d_dev = first_output->device;

    /* Reset command list */
    d3d_dev->allocator->Reset();
    d3d_dev->command_list->Reset(d3d_dev->allocator.Get(), d3d_kernel->pipeline.Get());

    /* Set root signature */
    d3d_dev->command_list->SetComputeRootSignature(d3d_kernel->root_signature.Get());

    /* Bind descriptor heap */
    ID3D12DescriptorHeap* heaps[] = { d3d_kernel->descriptor_heap.Get() };
    d3d_dev->command_list->SetDescriptorHeaps(1, heaps);

    /* Create UAV and SRV views for all buffers.
     *
     * Descriptor layout: [0..15] = UAVs (register(u#)), [16..31] = SRVs (register(t#))
     *
     * Transpiled HLSL from GLSL uses RWStructuredBuffer (UAV) for all buffers.
     * Transpiled HLSL from WGSL uses StructuredBuffer (SRV) for read-only inputs.
     * We bind every buffer to both slots so either style works. */
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = d3d_kernel->descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    UINT descriptor_size = d3d_dev->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (int i = 0; i < input_count; i++) {
        D3D12Buffer* input_buf = (D3D12Buffer*)inputs[i];
        UINT num_elements = (UINT)(input_buf->size / 4);

        /* UAV at slot i (register(u{i})) */
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
        uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_desc.Buffer.NumElements = num_elements;
        uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

        D3D12_CPU_DESCRIPTOR_HANDLE uav_handle = cpu_handle;
        uav_handle.ptr += i * descriptor_size;
        d3d_dev->device->CreateUnorderedAccessView(input_buf->resource.Get(), nullptr, &uav_desc, uav_handle);

        /* SRV at slot 16+i (register(t{i})) */
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Buffer.NumElements = num_elements;
        srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

        D3D12_CPU_DESCRIPTOR_HANDLE srv_handle = cpu_handle;
        srv_handle.ptr += (16 + i) * descriptor_size;
        d3d_dev->device->CreateShaderResourceView(input_buf->resource.Get(), &srv_desc, srv_handle);
    }

    /* UAV for output buffers at slots input_count..input_count+output_count-1 */
    for (int o = 0; o < output_count; o++) {
        D3D12Buffer* output_buf = (D3D12Buffer*)outputs[o];
        D3D12_UNORDERED_ACCESS_VIEW_DESC out_uav_desc = {};
        out_uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        out_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        out_uav_desc.Buffer.NumElements = (UINT)(output_buf->size / 4);
        out_uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

        D3D12_CPU_DESCRIPTOR_HANDLE output_handle = cpu_handle;
        output_handle.ptr += (input_count + o) * descriptor_size;
        d3d_dev->device->CreateUnorderedAccessView(output_buf->resource.Get(), nullptr, &out_uav_desc, output_handle);
    }

    /* Set descriptor table */
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = d3d_kernel->descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    d3d_dev->command_list->SetComputeRootDescriptorTable(0, gpu_handle);

    /* Dispatch compute shader
     * Calculate thread groups: 64 threads per group (common default) */
    UINT thread_group_size = (UINT)d3d12_kernel_workgroup_size(kernel);
    UINT num_groups = (work_size + thread_group_size - 1) / thread_group_size;
    d3d_dev->command_list->Dispatch(num_groups, 1, 1);

    /* Close and execute command list */
    d3d_dev->command_list->Close();
    ID3D12CommandList* cmd_lists[] = { d3d_dev->command_list.Get() };
    d3d_dev->queue->ExecuteCommandLists(1, cmd_lists);

    /* Wait for GPU to finish (synchronous execution) */
    d3d_dev->fence_value++;
    d3d_dev->queue->Signal(d3d_dev->fence.Get(), d3d_dev->fence_value);
    if (d3d_dev->fence->GetCompletedValue() < d3d_dev->fence_value) {
        d3d_dev->fence->SetEventOnCompletion(d3d_dev->fence_value, d3d_dev->fence_event);
        WaitForSingleObject(d3d_dev->fence_event, INFINITE);
    }
}

static void d3d12_kernel_destroy(void* kernel) {
    if (!kernel) return;
    delete (D3D12Kernel*)kernel;
}

/* ── Pipe ──────────────────────────────────────────────────────── */

typedef struct {
    D3D12Device* device;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> command_list;
} D3D12Pipe;

static void* d3d12_pipe_create(void* dev) {
    D3D12Device* d3d_dev = (D3D12Device*)dev;

    D3D12Pipe* pipe = new D3D12Pipe();
    pipe->device = d3d_dev;

    /* Create a dedicated command allocator for the pipe */
    HRESULT hr = d3d_dev->device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_COMPUTE,
        IID_PPV_ARGS(&pipe->allocator));
    if (FAILED(hr)) {
        delete pipe;
        return NULL;
    }

    /* Create a dedicated command list */
    hr = d3d_dev->device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
        pipe->allocator.Get(), nullptr,
        IID_PPV_ARGS(&pipe->command_list));
    if (FAILED(hr)) {
        delete pipe;
        return NULL;
    }

    return pipe;
}

static int d3d12_pipe_add(void* pipe_ptr, void* kernel, void** inputs,
                           int input_count, void** outputs, int output_count,
                           int work_size) {
    D3D12Pipe* pipe = (D3D12Pipe*)pipe_ptr;
    D3D12Kernel* d3d_kernel = (D3D12Kernel*)kernel;

    /* Set pipeline state and root signature */
    pipe->command_list->SetPipelineState(d3d_kernel->pipeline.Get());
    pipe->command_list->SetComputeRootSignature(d3d_kernel->root_signature.Get());

    /* Bind descriptor heap */
    ID3D12DescriptorHeap* heaps[] = { d3d_kernel->descriptor_heap.Get() };
    pipe->command_list->SetDescriptorHeaps(1, heaps);

    /* Create UAV and SRV views for all buffers */
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle = d3d_kernel->descriptor_heap->GetCPUDescriptorHandleForHeapStart();
    UINT descriptor_size = pipe->device->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    for (int i = 0; i < input_count; i++) {
        D3D12Buffer* input_buf = (D3D12Buffer*)inputs[i];
        UINT num_elements = (UINT)(input_buf->size / 4);

        /* UAV at slot i */
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
        uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav_desc.Buffer.NumElements = num_elements;
        uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

        D3D12_CPU_DESCRIPTOR_HANDLE uav_handle = cpu_handle;
        uav_handle.ptr += i * descriptor_size;
        pipe->device->device->CreateUnorderedAccessView(input_buf->resource.Get(), nullptr, &uav_desc, uav_handle);

        /* SRV at slot 16+i */
        D3D12_SHADER_RESOURCE_VIEW_DESC srv_desc = {};
        srv_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        srv_desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv_desc.Buffer.NumElements = num_elements;
        srv_desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;

        D3D12_CPU_DESCRIPTOR_HANDLE srv_handle = cpu_handle;
        srv_handle.ptr += (16 + i) * descriptor_size;
        pipe->device->device->CreateShaderResourceView(input_buf->resource.Get(), &srv_desc, srv_handle);
    }

    /* UAV for output buffers at slots input_count..input_count+output_count-1 */
    for (int o = 0; o < output_count; o++) {
        D3D12Buffer* output_buf = (D3D12Buffer*)outputs[o];
        D3D12_UNORDERED_ACCESS_VIEW_DESC out_uav_desc = {};
        out_uav_desc.Format = DXGI_FORMAT_R32_TYPELESS;
        out_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        out_uav_desc.Buffer.NumElements = (UINT)(output_buf->size / 4);
        out_uav_desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

        D3D12_CPU_DESCRIPTOR_HANDLE output_handle = cpu_handle;
        output_handle.ptr += (input_count + o) * descriptor_size;
        pipe->device->device->CreateUnorderedAccessView(output_buf->resource.Get(), nullptr, &out_uav_desc, output_handle);
    }

    /* Set descriptor table */
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle = d3d_kernel->descriptor_heap->GetGPUDescriptorHandleForHeapStart();
    pipe->command_list->SetComputeRootDescriptorTable(0, gpu_handle);

    /* UAV barrier before dispatch -- ensures writes from a previous
     * dispatch in this pipe are visible.  Required when one stage's
     * output feeds the next stage's input via the same UAV resource. */
    D3D12_RESOURCE_BARRIER uav_barrier = {};
    uav_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    uav_barrier.UAV.pResource = NULL; /* NULL = barrier on all UAVs */
    pipe->command_list->ResourceBarrier(1, &uav_barrier);

    /* Dispatch */
    UINT thread_group_size = (UINT)d3d12_kernel_workgroup_size(kernel);
    UINT num_groups = (work_size + thread_group_size - 1) / thread_group_size;
    pipe->command_list->Dispatch(num_groups, 1, 1);

    return 0;
}

static int d3d12_pipe_execute(void* pipe_ptr) {
    D3D12Pipe* pipe = (D3D12Pipe*)pipe_ptr;
    D3D12Device* dev = pipe->device;

    /* Close the command list */
    HRESULT hr = pipe->command_list->Close();
    if (FAILED(hr)) return -1;

    /* Execute */
    ID3D12CommandList* cmd_lists[] = { pipe->command_list.Get() };
    dev->queue->ExecuteCommandLists(1, cmd_lists);

    /* Wait for GPU */
    dev->fence_value++;
    dev->queue->Signal(dev->fence.Get(), dev->fence_value);
    if (dev->fence->GetCompletedValue() < dev->fence_value) {
        dev->fence->SetEventOnCompletion(dev->fence_value, dev->fence_event);
        WaitForSingleObject(dev->fence_event, INFINITE);
    }

    return 0;
}

static void d3d12_pipe_destroy(void* pipe_ptr) {
    if (!pipe_ptr) return;
    delete (D3D12Pipe*)pipe_ptr;
}

/* Viewport operations */
static void* d3d12_viewport_attach(void* dev, void* buffer, void* surface, char* error, size_t error_len) {
    D3D12Device* d3d_dev = (D3D12Device*)dev;
    D3D12Buffer* d3d_buf = (D3D12Buffer*)buffer;
    HWND hwnd = (HWND)surface;

    if (!hwnd || !IsWindow(hwnd)) {
        if (error) {
            snprintf(error, error_len, "Invalid HWND");
        }
        return NULL;
    }

    /* Create DXGI factory */
    ComPtr<IDXGIFactory4> factory;
    HRESULT hr = pfn_CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        if (error) {
            snprintf(error, error_len, "Failed to create DXGI factory");
        }
        return NULL;
    }

    /* Describe swap chain */
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.Width = 0;  /* Use window width */
    swapChainDesc.Height = 0; /* Use window height */
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.Stereo = FALSE;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;

    /* Try to create swap chain for double-buffered present */
    ComPtr<IDXGISwapChain1> swapChain1;
    hr = factory->CreateSwapChainForHwnd(
        d3d_dev->queue.Get(),
        hwnd,
        &swapChainDesc,
        nullptr,
        nullptr,
        &swapChain1
    );

    D3D12Viewport* viewport = new D3D12Viewport();
    viewport->buffer = d3d_buf;
    viewport->device = d3d_dev;
    viewport->bufferIndex = 0;
    viewport->headless = 0;

    if (SUCCEEDED(hr)) {
        /* Double-buffered path: swapchain available */
        ComPtr<IDXGISwapChain3> swapChain;
        hr = swapChain1.As(&swapChain);
        if (FAILED(hr)) {
            if (error) snprintf(error, error_len, "Failed to query IDXGISwapChain3");
            delete viewport;
            return NULL;
        }
        viewport->swapChain = swapChain;
    } else {
        /* Headless / software renderer: no swapchain, use a single
         * render target.  Present becomes a no-op but the viewport
         * lifecycle (attach/present/detach) still works for compute
         * workflows that read back results rather than display them. */
        RECT rect;
        GetClientRect(hwnd, &rect);
        UINT width  = rect.right  > 0 ? (UINT)rect.right  : 1;
        UINT height = rect.bottom > 0 ? (UINT)rect.bottom : 1;

        D3D12_RESOURCE_DESC rt_desc = {};
        rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rt_desc.Width = width;
        rt_desc.Height = height;
        rt_desc.DepthOrArraySize = 1;
        rt_desc.MipLevels = 1;
        rt_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        rt_desc.SampleDesc.Count = 1;
        rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_HEAP_PROPERTIES heap_props = {};
        heap_props.Type = D3D12_HEAP_TYPE_DEFAULT;

        hr = d3d_dev->device->CreateCommittedResource(
            &heap_props, D3D12_HEAP_FLAG_NONE, &rt_desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&viewport->renderTarget));

        if (FAILED(hr)) {
            if (error) snprintf(error, error_len, "Failed to create headless render target");
            delete viewport;
            return NULL;
        }

        viewport->headless = 1;
    }

    return viewport;
}

static void d3d12_viewport_present(void* viewport_ptr) {
    if (!viewport_ptr) return;

    D3D12Viewport* viewport = (D3D12Viewport*)viewport_ptr;

    /* Headless: copy buffer to render target (no display present) */
    if (viewport->headless) {
        viewport->device->allocator->Reset();
        viewport->device->command_list->Reset(viewport->device->allocator.Get(), nullptr);
        viewport->device->command_list->CopyResource(viewport->renderTarget.Get(),
                                                      viewport->buffer->resource.Get());
        viewport->device->command_list->Close();
        ID3D12CommandList* cmd_lists[] = { viewport->device->command_list.Get() };
        viewport->device->queue->ExecuteCommandLists(1, cmd_lists);

        viewport->device->fence_value++;
        viewport->device->queue->Signal(viewport->device->fence.Get(), viewport->device->fence_value);
        if (viewport->device->fence->GetCompletedValue() < viewport->device->fence_value) {
            viewport->device->fence->SetEventOnCompletion(viewport->device->fence_value, viewport->device->fence_event);
            WaitForSingleObject(viewport->device->fence_event, INFINITE);
        }
        return;
    }

    /* Double-buffered path: swap chain present */
    viewport->bufferIndex = viewport->swapChain->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> backBuffer;
    HRESULT hr = viewport->swapChain->GetBuffer(viewport->bufferIndex, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return;

    viewport->device->allocator->Reset();
    viewport->device->command_list->Reset(viewport->device->allocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = backBuffer.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    viewport->device->command_list->ResourceBarrier(1, &barrier);

    viewport->device->command_list->CopyResource(backBuffer.Get(), viewport->buffer->resource.Get());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    viewport->device->command_list->ResourceBarrier(1, &barrier);

    viewport->device->command_list->Close();
    ID3D12CommandList* cmd_lists[] = { viewport->device->command_list.Get() };
    viewport->device->queue->ExecuteCommandLists(1, cmd_lists);

    viewport->swapChain->Present(1, 0);

    viewport->device->fence_value++;
    viewport->device->queue->Signal(viewport->device->fence.Get(), viewport->device->fence_value);
    if (viewport->device->fence->GetCompletedValue() < viewport->device->fence_value) {
        viewport->device->fence->SetEventOnCompletion(viewport->device->fence_value, viewport->device->fence_event);
        WaitForSingleObject(viewport->device->fence_event, INFINITE);
    }
}

static void d3d12_viewport_detach(void* viewport_ptr) {
    if (!viewport_ptr) return;
    delete (D3D12Viewport*)viewport_ptr;
}

/* Backend implementation */
static mental_backend g_d3d12_backend = {
    .name = "D3D12",
    .api = MENTAL_API_D3D12,
    .init = d3d12_init,
    .shutdown = d3d12_shutdown,
    .device_count = d3d12_device_count,
    .device_info = d3d12_device_info,
    .device_create = d3d12_device_create,
    .device_destroy = d3d12_device_destroy,
    .buffer_alloc = d3d12_buffer_alloc,
    .buffer_write = d3d12_buffer_write,
    .buffer_read = d3d12_buffer_read,
    .buffer_resize = d3d12_buffer_resize,
    .buffer_clone = d3d12_buffer_clone,
    .buffer_destroy = d3d12_buffer_destroy,
    .kernel_compile = d3d12_kernel_compile,
    .kernel_workgroup_size = d3d12_kernel_workgroup_size,
    .kernel_dispatch = d3d12_kernel_dispatch,
    .kernel_destroy = d3d12_kernel_destroy,
    .pipe_create = d3d12_pipe_create,
    .pipe_add = d3d12_pipe_add,
    .pipe_execute = d3d12_pipe_execute,
    .pipe_destroy = d3d12_pipe_destroy,
    .viewport_attach = d3d12_viewport_attach,
    .viewport_present = d3d12_viewport_present,
    .viewport_detach = d3d12_viewport_detach,
    .viewport_readback = NULL
};

mental_backend* d3d12_backend = &g_d3d12_backend;

#else
/* Non-Windows: D3D12 is not available */
#include "mental_internal.h"
mental_backend* d3d12_backend = NULL;
#endif /* _WIN32 */
