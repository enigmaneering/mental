/*
 * Example 02: GPU Compute
 *
 * Demonstrates the full GPU compute pipeline:
 *   1. Get a device
 *   2. Create references for input and output
 *   3. Pin them to the GPU
 *   4. Compile a GLSL compute shader
 *   5. Dispatch the kernel
 *   6. Read back the result
 *
 * The shader adds two float vectors: C[i] = A[i] + B[i]
 *
 * Build:
 *   cc -o gpu-compute 02-gpu-compute.c -I.. -L../build -lmental -lc++ \
 *      -framework Metal -framework Foundation -framework QuartzCore \
 *      -framework AppKit -framework OpenCL
 *
 * Run:
 *   ./gpu-compute
 */

#include "mental.h"
#include <stdio.h>
#include <string.h>

#define N 4

static const char *shader_source =
    "#version 450\n"
    "layout(local_size_x = 4) in;\n"
    "layout(std430, binding = 0) readonly buffer A { float a[]; };\n"
    "layout(std430, binding = 1) readonly buffer B { float b[]; };\n"
    "layout(std430, binding = 2) writeonly buffer C { float c[]; };\n"
    "void main() {\n"
    "    uint i = gl_GlobalInvocationID.x;\n"
    "    c[i] = a[i] + b[i];\n"
    "}\n";

int main(void) {
    /* Get a device */
    int count = mental_device_count();
    if (count == 0) {
        printf("No GPU devices found.\n");
        return 1;
    }

    mental_device dev = mental_device_get(0);
    printf("Device: %s (%s)\n\n", mental_device_name(dev), mental_device_api_name(dev));

    /* Create input references */
    size_t size = N * sizeof(float);

    mental_reference ref_a = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference ref_b = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference ref_c = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);

    /* Write input data */
    float a[N] = {1.0f, 2.0f, 3.0f, 4.0f};
    float b[N] = {10.0f, 20.0f, 30.0f, 40.0f};
    mental_reference_write(ref_a, a, size);
    mental_reference_write(ref_b, b, size);

    printf("Input A: [%.0f, %.0f, %.0f, %.0f]\n", a[0], a[1], a[2], a[3]);
    printf("Input B: [%.0f, %.0f, %.0f, %.0f]\n", b[0], b[1], b[2], b[3]);

    /* Pin to GPU */
    mental_reference_pin(ref_a, dev);
    mental_reference_pin(ref_b, dev);
    mental_reference_pin(ref_c, dev);

    /* Compile shader */
    mental_kernel kernel = mental_compile(dev, shader_source, strlen(shader_source));
    if (!kernel) {
        printf("Compilation failed: %s\n", mental_get_error_message());
        return 1;
    }
    printf("Kernel compiled.\n");

    /* Dispatch */
    mental_reference inputs[2] = {ref_a, ref_b};
    mental_reference outputs[1] = {ref_c};
    mental_dispatch(kernel, inputs, 2, outputs, 1, N);
    printf("Dispatched.\n");

    /* Read back result */
    float c[N];
    mental_reference_read(ref_c, c, size);
    printf("Output:  [%.0f, %.0f, %.0f, %.0f]\n", c[0], c[1], c[2], c[3]);

    /* Verify */
    int ok = 1;
    for (int i = 0; i < N; i++) {
        if (c[i] != a[i] + b[i]) ok = 0;
    }
    printf("\n%s\n", ok ? "✓ Correct!" : "✗ Mismatch!");

    /* Cleanup */
    mental_kernel_finalize(kernel);
    mental_reference_close(ref_a);
    mental_reference_close(ref_b);
    mental_reference_close(ref_c);

    return ok ? 0 : 1;
}
