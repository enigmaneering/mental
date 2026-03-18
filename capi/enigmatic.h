#ifndef ENIGMATIC_H
#define ENIGMATIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Opaque Handle Types
// ============================================================================

typedef void* MentalDevice;
typedef void* MentalReference;
typedef void* MentalKernel;

// ============================================================================
// Enumerations
// ============================================================================

// Device types
typedef enum {
    MENTAL_DEVICE_OTHER = 0,
    MENTAL_DEVICE_INTEGRATED = 1,
    MENTAL_DEVICE_DISCRETE = 2,
    MENTAL_DEVICE_VIRTUAL = 3,
} MentalDeviceType;

// GPU API types
typedef enum {
    MENTAL_API_METAL = 0,
    MENTAL_API_VULKAN = 1,
    MENTAL_API_D3D12 = 2,
    MENTAL_API_OPENCL = 3,
} MentalAPI;

// Error codes
typedef enum {
    MENTAL_OK = 0,
    MENTAL_ERROR_INVALID_HANDLE = -1,
    MENTAL_ERROR_ALLOCATION_FAILED = -2,
    MENTAL_ERROR_COMPILATION_FAILED = -3,
    MENTAL_ERROR_DISPATCH_FAILED = -4,
    MENTAL_ERROR_INVALID_SIZE = -5,
    MENTAL_ERROR_DEVICE_NOT_FOUND = -6,
} MentalError;

// ============================================================================
// Structures
// ============================================================================

// Device information structure
typedef struct {
    int index;
    char* name;              // Caller must free with mental_free_device_info()
    MentalDeviceType device_type;
    MentalAPI api;
} MentalDeviceInfo;

// ============================================================================
// Initialization
// ============================================================================

// Initialize the mental library. Call this once at program startup.
// Returns: MENTAL_OK on success, error code otherwise
int mental_init(void);

// ============================================================================
// Device Management
// ============================================================================

// Get the number of available GPU devices.
// Returns: Number of devices (>= 0)
int mental_get_device_count(void);

// Get information about a specific device.
// Parameters:
//   - index: Device index (0 to mental_get_device_count()-1)
//   - info: Pointer to MentalDeviceInfo structure to fill
// Returns: MENTAL_OK on success, MENTAL_ERROR_DEVICE_NOT_FOUND if index invalid
//
// NOTE: The 'name' field in MentalDeviceInfo is dynamically allocated.
//       You MUST call mental_free_device_info() when done with the struct.
int mental_get_device_info(int index, MentalDeviceInfo* info);

// Free memory allocated in MentalDeviceInfo (specifically the name field).
// Parameters:
//   - info: Pointer to MentalDeviceInfo structure to clean up
void mental_free_device_info(MentalDeviceInfo* info);

// ============================================================================
// Reference (GPU Memory) Management
// ============================================================================

// Create a new GPU memory reference of the specified size.
// Parameters:
//   - size: Size in bytes to allocate
//   - device_index: Index of device to allocate on (0 for default)
// Returns: Handle to the reference, or NULL on failure
//
// NOTE: Caller MUST call mental_release_reference() when done to free GPU memory.
MentalReference mental_create_reference(size_t size, int device_index);

// Create a new GPU memory reference and initialize it with data.
// Parameters:
//   - data: Pointer to data to copy to GPU
//   - size: Size of data in bytes
//   - device_index: Index of device to allocate on (0 for default)
// Returns: Handle to the reference, or NULL on failure
//
// NOTE: Caller MUST call mental_release_reference() when done to free GPU memory.
MentalReference mental_create_reference_from_data(const uint8_t* data, size_t size, int device_index);

// Read data from GPU memory into a buffer.
// Parameters:
//   - handle: Reference handle
//   - buffer: Destination buffer (must be at least 'size' bytes)
//   - size: Number of bytes to read
// Returns: MENTAL_OK on success, error code otherwise
int mental_reference_read(MentalReference handle, uint8_t* buffer, size_t size);

// Write data to GPU memory.
// Parameters:
//   - handle: Reference handle
//   - data: Source data to write
//   - size: Number of bytes to write
// Returns: MENTAL_OK on success, error code otherwise
int mental_reference_write(MentalReference handle, const uint8_t* data, size_t size);

// Get the size of a GPU memory reference.
// Parameters:
//   - handle: Reference handle
// Returns: Size in bytes, or 0 if handle is invalid
size_t mental_reference_size(MentalReference handle);

// Release a GPU memory reference.
// Parameters:
//   - handle: Reference handle to release
// Returns: MENTAL_OK on success, MENTAL_ERROR_INVALID_HANDLE if handle is invalid
//
// NOTE: After calling this, the handle is invalid and must not be used.
int mental_release_reference(MentalReference handle);

// ============================================================================
// Kernel (Compute Shader) Management
// ============================================================================

// Compile a compute kernel from source code.
// The source language is automatically detected (GLSL, HLSL, MSL, WGSL).
// Parameters:
//   - source: Null-terminated shader source code string
//   - device_index: Index of device to compile for (0 for default)
// Returns: Handle to the compiled kernel, or NULL on compilation failure
//
// NOTE: Caller MUST call mental_release_kernel() when done to free resources.
MentalKernel mental_compile_kernel(const char* source, int device_index);

// Execute a compute kernel.
// Parameters:
//   - kernel_handle: Compiled kernel handle
//   - input_handles: Array of input reference handles (can be NULL if input_count is 0)
//   - input_count: Number of input buffers
//   - output_handle: Output reference handle
//   - work_size: Number of parallel invocations (e.g., 1024)
// Returns: MENTAL_OK on success, error code otherwise
int mental_kernel_dispatch(
    MentalKernel kernel_handle,
    MentalReference* input_handles,
    int input_count,
    MentalReference output_handle,
    int work_size
);

// Release a compiled kernel.
// Parameters:
//   - handle: Kernel handle to release
// Returns: MENTAL_OK on success, MENTAL_ERROR_INVALID_HANDLE if handle is invalid
//
// NOTE: After calling this, the handle is invalid and must not be used.
int mental_release_kernel(MentalKernel handle);

// ============================================================================
// Usage Example (C)
// ============================================================================
/*
 * #include "enigmatic.h"
 * #include <stdio.h>
 *
 * int main() {
 *     // Initialize
 *     mental_init();
 *
 *     // Enumerate devices
 *     int device_count = mental_get_device_count();
 *     printf("Found %d GPU device(s)\n", device_count);
 *
 *     for (int i = 0; i < device_count; i++) {
 *         MentalDeviceInfo info;
 *         mental_get_device_info(i, &info);
 *         printf("  [%d] %s\n", info.index, info.name);
 *         mental_free_device_info(&info);
 *     }
 *
 *     // Create GPU memory
 *     size_t size = 1024;
 *     MentalReference ref = mental_create_reference(size, 0);
 *
 *     // Write data
 *     uint8_t data[1024] = {0};
 *     mental_reference_write(ref, data, size);
 *
 *     // Compile kernel
 *     const char* source =
 *         "#version 450\n"
 *         "layout(local_size_x = 256) in;\n"
 *         "layout(binding = 0) buffer Output { float data[]; };\n"
 *         "void main() { data[gl_GlobalInvocationID.x] = float(gl_GlobalInvocationID.x); }\n";
 *
 *     MentalKernel kernel = mental_compile_kernel(source, 0);
 *
 *     // Dispatch kernel
 *     mental_kernel_dispatch(kernel, NULL, 0, ref, 1024);
 *
 *     // Read results
 *     uint8_t result[1024];
 *     mental_reference_read(ref, result, size);
 *
 *     // Clean up
 *     mental_release_kernel(kernel);
 *     mental_release_reference(ref);
 *
 *     return 0;
 * }
 */

#ifdef __cplusplus
}
#endif

#endif // ENIGMATIC_H
