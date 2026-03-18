#include "../../../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif

// Helper to test a shader
int test_shader(const char* language, const char* source,
                MentalReference input, MentalReference output, size_t size) {
    printf("--- Testing %s Shader ---\n", language);

    // Compile shader
    MentalKernel kernel = mental_compile_kernel(source, 0);
    if (!kernel) {
        printf("  ❌ Failed to compile\n\n");
        return 0;
    }

    // Execute
    MentalReference inputs[] = { input };
    int result = mental_kernel_dispatch(kernel, inputs, 1, output, size);
    if (result != MENTAL_OK) {
        printf("  ❌ Failed to execute\n\n");
        mental_release_kernel(kernel);
        return 0;
    }

    // Verify results (check first 5 values - should match input since we're copying)
    uint8_t* result_data = malloc(size);
    mental_reference_read(output, result_data, size);

    int all_correct = 1;
    for (int i = 0; i < 5; i++) {
        uint8_t expected = (uint8_t)i;
        if (result_data[i] != expected) {
            all_correct = 0;
            printf("  ❌ Mismatch at index %d: expected %d, got %d\n",
                   i, expected, result_data[i]);
            break;
        }
    }

    if (all_correct) {
        printf("  ✅ %s shader compiled and executed successfully\n", language);
        printf("     Results: [%d, %d, %d, %d, %d, ...] (copied from input)\n",
               result_data[0], result_data[1], result_data[2],
               result_data[3], result_data[4]);
    } else {
        printf("  ❌ Results incorrect\n");
    }
    printf("\n");

    free(result_data);
    mental_release_kernel(kernel);
    return all_correct;
}

int main() {
    printf("=== Cross-Language Shader Compilation Example (C) ===\n\n");

    // Initialize Mental
    mental_init();

    // Show current device
    MentalDeviceInfo device_info;
    mental_get_device_info(0, &device_info);

    const char* api_name = "Unknown";
    switch (device_info.api) {
        case MENTAL_API_METAL: api_name = "Metal"; break;
        case MENTAL_API_VULKAN: api_name = "Vulkan"; break;
        case MENTAL_API_D3D12: api_name = "D3D12"; break;
        case MENTAL_API_OPENCL: api_name = "OpenCL"; break;
    }

    printf("Device: %s (%s)\n\n", device_info.name, api_name);
    mental_free_device_info(&device_info);

    // Allocate buffers
    const size_t size = 256;
    MentalReference input = mental_create_reference(size, 0);
    MentalReference output = mental_create_reference(size, 0);

    if (!input || !output) {
        fprintf(stderr, "Failed to allocate GPU buffers\n");
        return 1;
    }

    // Initialize input with sequential values
    uint8_t* input_data = malloc(size);
    for (size_t i = 0; i < size; i++) {
        input_data[i] = (uint8_t)i;
    }
    mental_reference_write(input, input_data, size);
    free(input_data);

    // Example 1: GLSL (cross-platform, transpiled to native format)
    const char* glsl_shader =
        "#version 450\n"
        "\n"
        "layout(local_size_x = 256) in;\n"
        "layout(std430, binding = 0) buffer InputBuffer { uint data[]; } inputBuf;\n"
        "layout(std430, binding = 1) buffer OutputBuffer { uint data[]; } outputBuf;\n"
        "\n"
        "void main() {\n"
        "    uint idx = gl_GlobalInvocationID.x;\n"
        "    outputBuf.data[idx] = inputBuf.data[idx];\n"
        "}\n";

    test_shader("GLSL", glsl_shader, input, output, size);

    // Example 2: HLSL (cross-platform, transpiled to native format)
    const char* hlsl_shader =
        "RWStructuredBuffer<uint> input : register(u0);\n"
        "RWStructuredBuffer<uint> output : register(u1);\n"
        "\n"
        "[numthreads(256, 1, 1)]\n"
        "void main(uint3 id : SV_DispatchThreadID) {\n"
        "    output[id.x] = input[id.x];\n"
        "}\n";

    test_shader("HLSL", hlsl_shader, input, output, size);

    // Example 3: WGSL (cross-platform, WebGPU Shading Language)
    const char* wgsl_shader =
        "@group(0) @binding(0) var<storage, read> input: array<u32>;\n"
        "@group(0) @binding(1) var<storage, read_write> output: array<u32>;\n"
        "\n"
        "@compute @workgroup_size(256)\n"
        "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
        "    output[id.x] = input[id.x];\n"
        "}\n";

    test_shader("WGSL", wgsl_shader, input, output, size);

    // Example 4: MSL (macOS only - cannot transpile FROM MSL)
#ifdef __APPLE__
    printf("--- Testing MSL Shader (macOS native) ---\n");
    const char* msl_shader =
        "#include <metal_stdlib>\n"
        "using namespace metal;\n"
        "\n"
        "kernel void compute_main(\n"
        "    device uchar* input [[buffer(0)]],\n"
        "    device uchar* output [[buffer(1)]],\n"
        "    uint id [[thread_position_in_grid]]\n"
        ") {\n"
        "    output[id] = input[id];\n"
        "}\n";

    test_shader("MSL", msl_shader, input, output, size);
#else
    printf("--- MSL Shader (skipped - macOS only) ---\n");
    printf("MSL is Apple's Metal Shading Language and only works on macOS\n\n");
#endif

    printf("=== Complete ===\n\n");
    printf("Key Takeaway:\n");
    printf("  - GLSL, HLSL, and WGSL work on all platforms via automatic transpilation\n");
    printf("  - MSL only works on macOS (Metal's native language)\n");
    printf("  - All shaders produce identical results despite different syntax\n");

    // Cleanup
    mental_release_reference(input);
    mental_release_reference(output);

    return 0;
}
