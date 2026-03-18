#ifndef BACKEND_D3D12_H
#define BACKEND_D3D12_H

#include <stddef.h>  // for size_t

#ifdef __cplusplus
extern "C" {
#endif

// Device enumeration
typedef struct {
    char name[256];
    int device_type;  // 0=Discrete, 1=Integrated, 2=Virtual, 3=Other
    int vendor_id;
} D3D12DeviceInfo;

void* d3d12_enumerate_devices(int* count);
void d3d12_free_device_list(void* devices);

// Device management
void* d3d12_create_device(int device_index);
void d3d12_destroy_device(void* device);

// Buffer operations
void* d3d12_alloc_buffer(void* device, size_t size);
void d3d12_free_buffer(void* buffer);
void* d3d12_get_buffer_pointer(void* buffer);  // Get mapped pointer (UPLOAD buffer for writes)
void d3d12_flush_buffer(void* buffer);  // Copy UPLOAD -> DEFAULT (CPU writes to GPU)
void* d3d12_read_buffer(void* buffer);  // Copy DEFAULT -> READBACK and return mapped pointer

// Shader compilation
void* d3d12_compile_shader(void* device, const void* dxil_bytecode, size_t bytecode_size,
                           int buffer_count, char** error_msg);
void d3d12_free_shader(void* shader);

// Compute dispatch
void d3d12_dispatch_compute(void* device, void* shader, void** buffers, int buffer_count,
                           int x, int y, int z);

#ifdef __cplusplus
}
#endif

#endif // BACKEND_D3D12_H
