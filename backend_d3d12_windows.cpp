#include "backend_d3d12_windows.h"
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cstring>

using Microsoft::WRL::ComPtr;

// Device context that holds D3D12 resources
struct D3D12DeviceContext {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12CommandAllocator> commandAllocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValue;
    HANDLE fenceEvent;
};

// Buffer context
struct D3D12BufferContext {
    ComPtr<ID3D12Resource> resource;      // DEFAULT heap - GPU-accessible with UAV
    ComPtr<ID3D12Resource> uploadBuffer;  // UPLOAD heap - for CPU writes
    ComPtr<ID3D12Resource> readbackBuffer; // READBACK heap - for CPU reads
    void* mappedPtr;
    size_t size;
    D3D12DeviceContext* deviceCtx;  // Need device context for transfers
};

// Shader/Pipeline context
struct D3D12ShaderContext {
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap;
    int bufferCount;
};

// Helper to wait for GPU
static void WaitForGPU(D3D12DeviceContext* ctx) {
    ctx->fenceValue++;
    ctx->commandQueue->Signal(ctx->fence.Get(), ctx->fenceValue);
    if (ctx->fence->GetCompletedValue() < ctx->fenceValue) {
        ctx->fence->SetEventOnCompletion(ctx->fenceValue, ctx->fenceEvent);
        WaitForSingleObject(ctx->fenceEvent, INFINITE);
    }
}

// Device enumeration
void* d3d12_enumerate_devices(int* count) {
    *count = 0;

    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }

    std::vector<D3D12DeviceInfo> devices;
    ComPtr<IDXGIAdapter1> adapter;

    for (UINT i = 0; factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                         IID_PPV_ARGS(&adapter)) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        // Try to create a D3D12 device to verify support
        ComPtr<ID3D12Device> testDevice;
        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&testDevice)))) {
            D3D12DeviceInfo info = {};

            // Convert wide string to narrow string
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, info.name, sizeof(info.name), nullptr, nullptr);

            // Determine device type
            if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
                info.device_type = 3;  // Other (software)
            } else {
                // Check for dedicated memory to distinguish discrete vs integrated
                if (desc.DedicatedVideoMemory > 2ULL * 1024 * 1024 * 1024) {  // > 2GB = discrete
                    info.device_type = 0;  // Discrete
                } else {
                    info.device_type = 1;  // Integrated
                }
            }

            info.vendor_id = desc.VendorId;
            devices.push_back(info);
        }
    }

    *count = static_cast<int>(devices.size());
    if (devices.empty()) {
        return nullptr;
    }

    // Allocate and copy device info array
    D3D12DeviceInfo* result = new D3D12DeviceInfo[devices.size()];
    std::memcpy(result, devices.data(), devices.size() * sizeof(D3D12DeviceInfo));
    return result;
}

void d3d12_free_device_list(void* devices) {
    if (devices) {
        delete[] static_cast<D3D12DeviceInfo*>(devices);
    }
}

// Device management
void* d3d12_create_device(int device_index) {
    ComPtr<IDXGIFactory6> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)))) {
        return nullptr;
    }

    ComPtr<IDXGIAdapter1> adapter;
    if (FAILED(factory->EnumAdapterByGpuPreference(device_index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                   IID_PPV_ARGS(&adapter)))) {
        return nullptr;
    }

    D3D12DeviceContext* ctx = new D3D12DeviceContext();

    // Create device
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&ctx->device)))) {
        delete ctx;
        return nullptr;
    }

    // Create command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    if (FAILED(ctx->device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&ctx->commandQueue)))) {
        delete ctx;
        return nullptr;
    }

    // Create command allocator
    if (FAILED(ctx->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                                   IID_PPV_ARGS(&ctx->commandAllocator)))) {
        delete ctx;
        return nullptr;
    }

    // Create command list
    if (FAILED(ctx->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                              ctx->commandAllocator.Get(), nullptr,
                                              IID_PPV_ARGS(&ctx->commandList)))) {
        delete ctx;
        return nullptr;
    }
    ctx->commandList->Close();  // Start closed, will open before recording

    // Create fence for synchronization
    ctx->fenceValue = 0;
    if (FAILED(ctx->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ctx->fence)))) {
        delete ctx;
        return nullptr;
    }

    ctx->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!ctx->fenceEvent) {
        delete ctx;
        return nullptr;
    }

    return ctx;
}

void d3d12_destroy_device(void* device) {
    if (!device) return;

    D3D12DeviceContext* ctx = static_cast<D3D12DeviceContext*>(device);
    if (ctx->fenceEvent) {
        CloseHandle(ctx->fenceEvent);
    }
    delete ctx;
}

// Buffer operations
void* d3d12_alloc_buffer(void* device, size_t size) {
    if (!device) return nullptr;

    D3D12DeviceContext* ctx = static_cast<D3D12DeviceContext*>(device);
    D3D12BufferContext* bufCtx = new D3D12BufferContext();
    bufCtx->size = size;
    bufCtx->deviceCtx = ctx;
    bufCtx->mappedPtr = nullptr;

    // Resource description for all buffers
    D3D12_RESOURCE_DESC resourceDesc = {};
    resourceDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    resourceDesc.Width = size;
    resourceDesc.Height = 1;
    resourceDesc.DepthOrArraySize = 1;
    resourceDesc.MipLevels = 1;
    resourceDesc.Format = DXGI_FORMAT_UNKNOWN;
    resourceDesc.SampleDesc.Count = 1;
    resourceDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    // 1. Create DEFAULT heap buffer (GPU-accessible, UAV-enabled for compute shaders)
    D3D12_HEAP_PROPERTIES defaultHeapProps = {};
    defaultHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    if (FAILED(ctx->device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE,
                                                    &resourceDesc, D3D12_RESOURCE_STATE_COMMON,
                                                    nullptr, IID_PPV_ARGS(&bufCtx->resource)))) {
        delete bufCtx;
        return nullptr;
    }

    // 2. Create UPLOAD buffer (for CPU -> GPU transfers)
    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    resourceDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    if (FAILED(ctx->device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE,
                                                    &resourceDesc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                    nullptr, IID_PPV_ARGS(&bufCtx->uploadBuffer)))) {
        delete bufCtx;
        return nullptr;
    }

    // 3. Create READBACK buffer (for GPU -> CPU transfers)
    D3D12_HEAP_PROPERTIES readbackHeapProps = {};
    readbackHeapProps.Type = D3D12_HEAP_TYPE_READBACK;

    if (FAILED(ctx->device->CreateCommittedResource(&readbackHeapProps, D3D12_HEAP_FLAG_NONE,
                                                    &resourceDesc, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&bufCtx->readbackBuffer)))) {
        delete bufCtx;
        return nullptr;
    }

    // Map the UPLOAD buffer for CPU writes (stays mapped)
    if (FAILED(bufCtx->uploadBuffer->Map(0, nullptr, &bufCtx->mappedPtr))) {
        delete bufCtx;
        return nullptr;
    }

    return bufCtx;
}

void d3d12_free_buffer(void* buffer) {
    if (!buffer) return;

    D3D12BufferContext* bufCtx = static_cast<D3D12BufferContext*>(buffer);
    if (bufCtx->mappedPtr) {
        bufCtx->uploadBuffer->Unmap(0, nullptr);
    }
    delete bufCtx;
}

void* d3d12_get_buffer_pointer(void* buffer) {
    if (!buffer) return nullptr;

    D3D12BufferContext* bufCtx = static_cast<D3D12BufferContext*>(buffer);
    return bufCtx->mappedPtr;
}

void d3d12_flush_buffer(void* buffer) {
    // Copy from UPLOAD buffer to DEFAULT buffer
    if (!buffer) return;

    D3D12BufferContext* bufCtx = static_cast<D3D12BufferContext*>(buffer);
    D3D12DeviceContext* ctx = bufCtx->deviceCtx;

    // Reset and record copy command
    ctx->commandAllocator->Reset();
    ctx->commandList->Reset(ctx->commandAllocator.Get(), nullptr);

    // Transition DEFAULT buffer to COPY_DEST
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = bufCtx->resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ctx->commandList->ResourceBarrier(1, &barrier);

    // Copy from UPLOAD to DEFAULT
    ctx->commandList->CopyResource(bufCtx->resource.Get(), bufCtx->uploadBuffer.Get());

    // Transition DEFAULT buffer to UNORDERED_ACCESS for compute
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ctx->commandList->ResourceBarrier(1, &barrier);

    ctx->commandList->Close();

    // Execute and wait
    ID3D12CommandList* cmdLists[] = { ctx->commandList.Get() };
    ctx->commandQueue->ExecuteCommandLists(1, cmdLists);
    WaitForGPU(ctx);
}

void* d3d12_read_buffer(void* buffer) {
    // Copy from DEFAULT buffer to READBACK buffer for CPU reading
    if (!buffer) return nullptr;

    D3D12BufferContext* bufCtx = static_cast<D3D12BufferContext*>(buffer);
    D3D12DeviceContext* ctx = bufCtx->deviceCtx;

    // Reset and record copy command
    ctx->commandAllocator->Reset();
    ctx->commandList->Reset(ctx->commandAllocator.Get(), nullptr);

    // Transition DEFAULT buffer to COPY_SOURCE
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = bufCtx->resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ctx->commandList->ResourceBarrier(1, &barrier);

    // Copy from DEFAULT to READBACK
    ctx->commandList->CopyResource(bufCtx->readbackBuffer.Get(), bufCtx->resource.Get());

    // Transition DEFAULT buffer back to UNORDERED_ACCESS
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ctx->commandList->ResourceBarrier(1, &barrier);

    ctx->commandList->Close();

    // Execute and wait
    ID3D12CommandList* cmdLists[] = { ctx->commandList.Get() };
    ctx->commandQueue->ExecuteCommandLists(1, cmdLists);
    WaitForGPU(ctx);

    // Map and return the readback buffer
    void* readPtr = nullptr;
    D3D12_RANGE readRange = {0, static_cast<SIZE_T>(bufCtx->size)};
    if (FAILED(bufCtx->readbackBuffer->Map(0, &readRange, &readPtr))) {
        return nullptr;
    }

    return readPtr;
}

// Shader compilation
void* d3d12_compile_shader(void* device, const void* dxil_bytecode, size_t bytecode_size,
                           int buffer_count, char** error_msg) {
    if (!device || !dxil_bytecode) {
        if (error_msg) *error_msg = _strdup("Invalid parameters");
        return nullptr;
    }

    D3D12DeviceContext* ctx = static_cast<D3D12DeviceContext*>(device);
    D3D12ShaderContext* shaderCtx = new D3D12ShaderContext();
    shaderCtx->bufferCount = buffer_count;

    // Create root signature
    std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
    std::vector<D3D12_ROOT_PARAMETER> rootParams;

    if (buffer_count > 0) {
        // UAV descriptor range for all buffers
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = buffer_count;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        ranges.push_back(range);

        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = ranges.data();
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams.push_back(param);
    }

    D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
    rootSigDesc.NumParameters = static_cast<UINT>(rootParams.size());
    rootSigDesc.pParameters = rootParams.empty() ? nullptr : rootParams.data();
    rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

    ComPtr<ID3DBlob> signature;
    ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                          &signature, &error))) {
        if (error_msg && error) {
            *error_msg = _strdup(static_cast<const char*>(error->GetBufferPointer()));
        }
        delete shaderCtx;
        return nullptr;
    }

    if (FAILED(ctx->device->CreateRootSignature(0, signature->GetBufferPointer(),
                                                signature->GetBufferSize(),
                                                IID_PPV_ARGS(&shaderCtx->rootSignature)))) {
        if (error_msg) *error_msg = _strdup("Failed to create root signature");
        delete shaderCtx;
        return nullptr;
    }

    // Create compute pipeline state
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = shaderCtx->rootSignature.Get();
    psoDesc.CS.pShaderBytecode = dxil_bytecode;
    psoDesc.CS.BytecodeLength = bytecode_size;

    if (FAILED(ctx->device->CreateComputePipelineState(&psoDesc,
                                                       IID_PPV_ARGS(&shaderCtx->pipelineState)))) {
        if (error_msg) *error_msg = _strdup("Failed to create pipeline state");
        delete shaderCtx;
        return nullptr;
    }

    // Create descriptor heap if we have buffers
    if (buffer_count > 0) {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = buffer_count;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

        if (FAILED(ctx->device->CreateDescriptorHeap(&heapDesc,
                                                     IID_PPV_ARGS(&shaderCtx->descriptorHeap)))) {
            if (error_msg) *error_msg = _strdup("Failed to create descriptor heap");
            delete shaderCtx;
            return nullptr;
        }
    }

    return shaderCtx;
}

void d3d12_free_shader(void* shader) {
    if (!shader) return;
    delete static_cast<D3D12ShaderContext*>(shader);
}

// Compute dispatch
void d3d12_dispatch_compute(void* device, void* shader, void** buffers, int buffer_count,
                           int x, int y, int z) {
    if (!device || !shader) return;

    D3D12DeviceContext* ctx = static_cast<D3D12DeviceContext*>(device);
    D3D12ShaderContext* shaderCtx = static_cast<D3D12ShaderContext*>(shader);

    // Reset command list
    ctx->commandAllocator->Reset();
    ctx->commandList->Reset(ctx->commandAllocator.Get(), shaderCtx->pipelineState.Get());

    // Set root signature
    ctx->commandList->SetComputeRootSignature(shaderCtx->rootSignature.Get());

    // Bind descriptor heap and buffers
    if (buffer_count > 0 && shaderCtx->descriptorHeap) {
        ID3D12DescriptorHeap* heaps[] = { shaderCtx->descriptorHeap.Get() };
        ctx->commandList->SetDescriptorHeaps(1, heaps);

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = shaderCtx->descriptorHeap->GetCPUDescriptorHandleForHeapStart();
        UINT descriptorSize = ctx->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Create UAV for each buffer
        for (int i = 0; i < buffer_count; i++) {
            D3D12BufferContext* bufCtx = static_cast<D3D12BufferContext*>(buffers[i]);

            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_R32_TYPELESS;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uavDesc.Buffer.FirstElement = 0;
            uavDesc.Buffer.NumElements = static_cast<UINT>(bufCtx->size / 4);
            uavDesc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;

            D3D12_CPU_DESCRIPTOR_HANDLE handle = cpuHandle;
            handle.ptr += i * descriptorSize;
            ctx->device->CreateUnorderedAccessView(bufCtx->resource.Get(), nullptr, &uavDesc, handle);
        }

        // Set descriptor table
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = shaderCtx->descriptorHeap->GetGPUDescriptorHandleForHeapStart();
        ctx->commandList->SetComputeRootDescriptorTable(0, gpuHandle);
    }

    // Dispatch
    ctx->commandList->Dispatch(x, y, z);

    // Close and execute
    ctx->commandList->Close();
    ID3D12CommandList* commandLists[] = { ctx->commandList.Get() };
    ctx->commandQueue->ExecuteCommandLists(1, commandLists);

    // Wait for completion (synchronous execution)
    WaitForGPU(ctx);
}
