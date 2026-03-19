/*
 * Mental - Simple Example
 *
 * This example demonstrates:
 * 1. Device enumeration
 * 2. Allocating GPU memory (Reference)
 * 3. Writing data to GPU
 * 4. Compiling a compute kernel (auto-transpilation)
 * 5. Dispatching computation
 * 6. Reading results back
 */

#include "mental.h"
#include <stdio.h>
#include <stdlib.h>

/* Simple GLSL compute shader that doubles input values */
const char* compute_shader =
    "#version 450\n"
    "layout(std430, binding = 0) buffer Input { float data[]; } input_buf;\n"
    "layout(std430, binding = 1) buffer Output { float data[]; } output_buf;\n"
    "void main() {\n"
    "    uint idx = gl_GlobalInvocationID.x;\n"
    "    output_buf.data[idx] = input_buf.data[idx] * 2.0;\n"
    "}\n";

int main(int argc, char** argv) {
    printf("Mental Example - Simple Compute\n");
    printf("================================\n\n");

    /* Initialize Mental */
    if (mental_init() != 0) {
        fprintf(stderr, "Failed to initialize Mental: %s\n", mental_error());
        return 1;
    }

    /* Enumerate devices */
    int device_count = mental_device_count();
    printf("Found %d compute device(s):\n", device_count);

    if (device_count == 0) {
        fprintf(stderr, "No compute devices found\n");
        mental_shutdown();
        return 1;
    }

    for (int i = 0; i < device_count; i++) {
        mental_device dev = mental_device_get(i);
        const char* name = mental_device_name(dev);
        mental_api_type api = mental_device_api(dev);

        const char* api_name = "Unknown";
        switch (api) {
            case MENTAL_API_METAL: api_name = "Metal"; break;
            case MENTAL_API_D3D12: api_name = "D3D12"; break;
            case MENTAL_API_VULKAN: api_name = "Vulkan"; break;
            case MENTAL_API_OPENCL: api_name = "OpenCL"; break;
        }

        printf("  [%d] %s (%s)\n", i, name, api_name);
    }

    /* Use first device */
    mental_device device = mental_device_get(0);
    printf("\nUsing device: %s\n\n", mental_device_name(device));

    /* Prepare input data */
    const int N = 1024;
    float* input_data = (float*)malloc(N * sizeof(float));
    float* output_data = (float*)malloc(N * sizeof(float));

    for (int i = 0; i < N; i++) {
        input_data[i] = (float)i;
    }

    /* Allocate GPU memory and write input data */
    printf("Allocating GPU memory and uploading data...\n");
    mental_reference input = mental_alloc(device, N * sizeof(float));
    mental_reference output = mental_alloc(device, N * sizeof(float));

    if (!input || !output) {
        fprintf(stderr, "Failed to allocate GPU memory: %s\n", mental_error());
        free(input_data);
        free(output_data);
        mental_shutdown();
        return 1;
    }

    mental_write(input, input_data, N * sizeof(float));
    printf("  Input size: %zu bytes\n", mental_size(input));
    printf("  Output size: %zu bytes\n", mental_size(output));

    /* Compile kernel (auto-transpiles GLSL to target backend language) */
    printf("\nCompiling kernel...\n");
    mental_kernel kernel = mental_compile(device, compute_shader, strlen(compute_shader));

    if (!kernel) {
        fprintf(stderr, "Failed to compile kernel: %s\n", mental_error());
        mental_finalize(input);
        mental_finalize(output);
        free(input_data);
        free(output_data);
        mental_shutdown();
        return 1;
    }
    printf("  Kernel compiled successfully!\n");

    /* Dispatch computation */
    printf("\nDispatching kernel (work size: %d)...\n", N);
    mental_reference inputs[] = { input };
    mental_dispatch(kernel, inputs, 1, output, N);
    printf("  Computation complete!\n");

    /* Read results back */
    printf("\nReading results...\n");
    mental_read(output, output_data, N * sizeof(float));

    /* Verify results */
    printf("\nVerifying results (first 10 values):\n");
    int errors = 0;
    for (int i = 0; i < 10; i++) {
        float expected = input_data[i] * 2.0f;
        printf("  [%d] %.1f * 2 = %.1f (expected %.1f) %s\n",
               i, input_data[i], output_data[i], expected,
               output_data[i] == expected ? "✓" : "✗");

        if (output_data[i] != expected) errors++;
    }

    /* Check all values */
    for (int i = 10; i < N; i++) {
        if (output_data[i] != input_data[i] * 2.0f) errors++;
    }

    if (errors == 0) {
        printf("\n✓ All %d values computed correctly!\n", N);
    } else {
        printf("\n✗ %d errors found!\n", errors);
    }

    /* Cleanup */
    mental_kernel_finalize(kernel);
    mental_finalize(input);
    mental_finalize(output);
    free(input_data);
    free(output_data);
    mental_shutdown();

    printf("\nDone!\n");
    return errors == 0 ? 0 : 1;
}
