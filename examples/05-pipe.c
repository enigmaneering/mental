/*
 * Example 05: Pipe (Chained Kernel Dispatch)
 *
 * Demonstrates chaining three kernels in a single GPU submission:
 *   1. Multiply: B[i] = A[i] * 2
 *   2. Add:      C[i] = B[i] + 10
 *   3. Scale:    D[i] = C[i] * 3
 *
 * All dispatches run in one command buffer — data stays on GPU
 * between stages with no CPU round-trips.
 *
 * Build:
 *   cc -o pipe 05-pipe.c -I.. -L../build -lmental-static -lc++ \
 *      -framework Metal -framework Foundation -framework QuartzCore \
 *      -framework AppKit -framework OpenCL
 *
 * Run:
 *   ./pipe
 */

#include "mental.h"
#include <stdio.h>
#include <string.h>

#define N 4

/* Kernel 1: multiply each element by 2 */
static const char *multiply_src =
    "#version 450\n"
    "layout(local_size_x = 4) in;\n"
    "layout(std430, binding = 0) readonly buffer A { float a[]; };\n"
    "layout(std430, binding = 1) writeonly buffer B { float b[]; };\n"
    "void main() {\n"
    "    uint i = gl_GlobalInvocationID.x;\n"
    "    b[i] = a[i] * 2.0;\n"
    "}\n";

/* Kernel 2: add 10 to each element */
static const char *add_src =
    "#version 450\n"
    "layout(local_size_x = 4) in;\n"
    "layout(std430, binding = 0) readonly buffer A { float a[]; };\n"
    "layout(std430, binding = 1) writeonly buffer B { float b[]; };\n"
    "void main() {\n"
    "    uint i = gl_GlobalInvocationID.x;\n"
    "    b[i] = a[i] + 10.0;\n"
    "}\n";

/* Kernel 3: scale each element by 3 */
static const char *scale_src =
    "#version 450\n"
    "layout(local_size_x = 4) in;\n"
    "layout(std430, binding = 0) readonly buffer A { float a[]; };\n"
    "layout(std430, binding = 1) writeonly buffer B { float b[]; };\n"
    "void main() {\n"
    "    uint i = gl_GlobalInvocationID.x;\n"
    "    b[i] = a[i] * 3.0;\n"
    "}\n";

int main(void) {
    int count = mental_device_count();
    if (count == 0) {
        printf("No GPU devices found.\n");
        return 1;
    }

    mental_device dev = mental_device_get(0);
    printf("Device: %s (%s)\n\n", mental_device_name(dev), mental_device_api_name(dev));

    /* Create references for each stage */
    size_t size = N * sizeof(float);
    mental_reference ref_a = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference ref_b = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference ref_c = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference ref_d = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);

    /* Write input data */
    float a[N] = {1.0f, 2.0f, 3.0f, 4.0f};
    mental_reference_write(ref_a, a, size);
    printf("Input:        [%.0f, %.0f, %.0f, %.0f]\n", a[0], a[1], a[2], a[3]);

    /* Pin all references to GPU */
    mental_reference_pin(ref_a, dev);
    mental_reference_pin(ref_b, dev);
    mental_reference_pin(ref_c, dev);
    mental_reference_pin(ref_d, dev);

    /* Compile three kernels */
    mental_kernel k_multiply = mental_compile(dev, multiply_src, strlen(multiply_src));
    mental_kernel k_add      = mental_compile(dev, add_src, strlen(add_src));
    mental_kernel k_scale    = mental_compile(dev, scale_src, strlen(scale_src));

    if (!k_multiply || !k_add || !k_scale) {
        printf("Compilation failed: %s\n", mental_get_error_message());
        return 1;
    }
    printf("3 kernels compiled.\n");

    /* Build a pipe: multiply → add → scale */
    mental_pipe pipe = mental_pipe_create(dev);
    if (!pipe) {
        printf("Failed to create pipe: %s\n", mental_get_error_message());
        return 1;
    }

    mental_reference inputs_1[1] = {ref_a};
    mental_reference outputs_1[1] = {ref_b};
    mental_pipe_add(pipe, k_multiply, inputs_1, 1, outputs_1, 1, N);

    mental_reference inputs_2[1] = {ref_b};
    mental_reference outputs_2[1] = {ref_c};
    mental_pipe_add(pipe, k_add, inputs_2, 1, outputs_2, 1, N);

    mental_reference inputs_3[1] = {ref_c};
    mental_reference outputs_3[1] = {ref_d};
    mental_pipe_add(pipe, k_scale, inputs_3, 1, outputs_3, 1, N);

    printf("Pipe built: 3 dispatches queued.\n");

    /* Execute — one GPU submission */
    if (mental_pipe_execute(pipe) != 0) {
        printf("Pipe execution failed: %s\n", mental_get_error_message());
        return 1;
    }
    printf("Pipe executed.\n\n");

    /* Read back the final result — the only read from the GPU.
     * Intermediate buffers (ref_b, ref_c) were never read;
     * data stayed on-device between stages. */
    float d[N];
    mental_reference_read(ref_d, d, size);

    printf("Pipeline:     (A * 2 + 10) * 3\n");
    printf("Result:       [%.0f, %.0f, %.0f, %.0f]\n", d[0], d[1], d[2], d[3]);

    /* Verify: D[i] = (A[i] * 2 + 10) * 3 */
    float expected[N];
    int ok = 1;
    for (int i = 0; i < N; i++) {
        expected[i] = (a[i] * 2.0f + 10.0f) * 3.0f;
        if (d[i] != expected[i]) {
            printf("  mismatch at [%d]: got %.0f, expected %.0f\n", i, d[i], expected[i]);
            ok = 0;
        }
    }
    printf("Expected:     [%.0f, %.0f, %.0f, %.0f]\n",
           expected[0], expected[1], expected[2], expected[3]);
    printf("\n%s\n", ok ? "✓ Pipe correct!" : "✗ Mismatch!");

    /* Cleanup */
    mental_pipe_finalize(pipe);
    mental_kernel_finalize(k_multiply);
    mental_kernel_finalize(k_add);
    mental_kernel_finalize(k_scale);
    mental_reference_close(ref_a);
    mental_reference_close(ref_b);
    mental_reference_close(ref_c);
    mental_reference_close(ref_d);

    return ok ? 0 : 1;
}
