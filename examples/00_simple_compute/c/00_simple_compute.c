#include "../../../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

int main() {
    printf("=== Mental Simple Compute Example (C) ===\n\n");

    // Initialize Mental
    if (mental_init() != MENTAL_OK) {
        fprintf(stderr, "Failed to initialize Mental\n");
        return 1;
    }

    // List available GPU devices
    printf("Available GPU devices:\n");
    int device_count = mental_get_device_count();

    for (int i = 0; i < device_count; i++) {
        MentalDeviceInfo info;
        if (mental_get_device_info(i, &info) == MENTAL_OK) {
            const char* api_name = "Unknown";
            switch (info.api) {
                case MENTAL_API_METAL: api_name = "Metal"; break;
                case MENTAL_API_VULKAN: api_name = "Vulkan"; break;
                case MENTAL_API_D3D12: api_name = "D3D12"; break;
                case MENTAL_API_OPENCL: api_name = "OpenCL"; break;
            }

            const char* type_name = "Other";
            switch (info.device_type) {
                case MENTAL_DEVICE_OTHER: type_name = "Other"; break;
                case MENTAL_DEVICE_INTEGRATED: type_name = "Integrated"; break;
                case MENTAL_DEVICE_DISCRETE: type_name = "Discrete"; break;
                case MENTAL_DEVICE_VIRTUAL: type_name = "Virtual"; break;
            }

            printf("  [%d] %s (%s, %s)\n", i, info.name, api_name, type_name);
            mental_free_device_info(&info);
        }
    }
    printf("\n");

    // Use device 0
    MentalDeviceInfo default_device;
    mental_get_device_info(0, &default_device);
    printf("Using device: %s\n\n", default_device.name);
    mental_free_device_info(&default_device);

    // Allocate GPU buffers
    printf("Allocating GPU buffers...\n");
    const size_t buffer_size = 256;
    MentalReference input = mental_create_reference(buffer_size, 0);
    MentalReference output = mental_create_reference(buffer_size, 0);

    if (!input || !output) {
        fprintf(stderr, "Failed to allocate GPU buffers\n");
        return 1;
    }

    // Populate input with sequential values
    printf("Writing input data...\n");
    uint8_t* input_data = malloc(buffer_size);
    for (size_t i = 0; i < buffer_size; i++) {
        input_data[i] = (uint8_t)i;
    }
    mental_reference_write(input, input_data, buffer_size);
    free(input_data);

    // Compile a simple compute kernel that doubles values
    printf("Compiling kernel...\n");
    const char* kernel_source;

#ifdef __APPLE__
    // Use MSL (Metal Shading Language) on macOS
    kernel_source =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "\n"
        "kernel void compute_main(\n"
        "    device float* input [[buffer(0)]],\n"
        "    device float* output [[buffer(1)]],\n"
        "    uint id [[thread_position_in_grid]]\n"
        ") {\n"
        "    output[id] = input[id] * 2.0;\n"
        "}\n";
#else
    // Use HLSL on other platforms (automatically transpiled)
    kernel_source =
        "RWStructuredBuffer<float> input : register(u0);\n"
        "RWStructuredBuffer<float> output : register(u1);\n"
        "\n"
        "[numthreads(256, 1, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "    output[id.x] = input[id.x] * 2.0;\n"
        "}\n";
#endif

    MentalKernel kernel = mental_compile_kernel(kernel_source, 0);
    if (!kernel) {
        fprintf(stderr, "Failed to compile kernel\n");
        mental_release_reference(input);
        mental_release_reference(output);
        return 1;
    }

    // Execute kernel on GPU
    printf("Executing compute kernel...\n");
    MentalReference inputs[] = { input };
    int dispatch_result = mental_kernel_dispatch(kernel, inputs, 1, output, buffer_size);

    if (dispatch_result != MENTAL_OK) {
        fprintf(stderr, "Failed to dispatch kernel\n");
        mental_release_kernel(kernel);
        mental_release_reference(input);
        mental_release_reference(output);
        return 1;
    }

    // Read results
    printf("Reading results...\n");
    uint8_t* result = malloc(buffer_size);
    mental_reference_read(output, result, buffer_size);

    // Verify first 10 values
    printf("\nResults (first 10 values):\n");
    printf("  Input  → Output\n");
    for (int i = 0; i < 10; i++) {
        printf("  %3d    → %3d\n", i, result[i]);
    }

    // Cleanup
    free(result);
    mental_release_kernel(kernel);
    mental_release_reference(input);
    mental_release_reference(output);

    printf("\n=== Complete ===\n");
    return 0;
}
