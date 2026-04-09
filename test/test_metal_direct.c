/* Direct Metal shader test - no transpilation */
#include "../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASSERT(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        return 1; \
    } \
} while(0)

#define ASSERT_NO_ERROR() do { \
    if (mental_get_error() != MENTAL_SUCCESS) { \
        fprintf(stderr, "FAIL: %s\n", mental_get_error_message()); \
        return 1; \
    } \
} while(0)

/* Direct MSL shader with explicit "compute_main" name */
const char* msl_shader =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "kernel void compute_main(\n"
    "    device float* input0 [[buffer(0)]],\n"
    "    device float* input1 [[buffer(1)]],\n"
    "    device float* output [[buffer(2)]],\n"
    "    uint id [[thread_position_in_grid]])\n"
    "{\n"
    "    output[id] = input0[id] + input1[id];\n"
    "}\n";

int main(void) {
    printf("Testing direct Metal shader...\n");

    if (mental_device_count() == 0) {
        printf("SKIP: No GPU devices available\n");
        return 0;
    }
    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to get device");
    ASSERT_NO_ERROR();

    const char* api_name = mental_device_api_name(dev);
    printf("  Device API: %s\n", api_name);

    if (strstr(api_name, "Metal") == NULL) {
        printf("SKIP: Not on Metal backend\n");
        return 0;
    }

    /* Compile shader */
    mental_kernel kernel = mental_compile(dev, msl_shader, strlen(msl_shader));
    if (kernel == NULL) {
        printf("FAIL: Compilation failed: %s\n", mental_get_error_message());
        return 1;
    }
    printf("  Kernel compiled successfully\n");

    /* Create test buffers */
    size_t count = 256;
    size_t size = count * sizeof(float);

    mental_reference input0 = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_pin(input0, dev);
    mental_reference input1 = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_pin(input1, dev);
    mental_reference output = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_pin(output, dev);

    ASSERT(input0 && input1 && output, "Failed to allocate buffers");

    /* Fill input buffers */
    float data0[256], data1[256];
    for (int i = 0; i < 256; i++) {
        data0[i] = (float)i;
        data1[i] = (float)i * 2.0f;
    }
    mental_reference_write(input0, data0, size);
    mental_reference_write(input1, data1, size);
    printf("  Buffers filled\n");

    /* Dispatch kernel */
    mental_reference inputs[] = { input0, input1 };
    mental_dispatch(kernel, inputs, 2, (mental_reference[]){output}, 1, count);

    if (mental_get_error() != MENTAL_SUCCESS) {
        printf("FAIL: Dispatch failed: %s\n", mental_get_error_message());
        mental_reference_close(input0);
        mental_reference_close(input1);
        mental_reference_close(output);
        mental_kernel_finalize(kernel);
        return 1;
    }
    printf("  Kernel dispatched\n");

    /* Read and verify results */
    float results[256];
    mental_reference_read(output, results, size);

    int errors = 0;
    for (int i = 0; i < 256 && errors < 10; i++) {
        float expected = data0[i] + data1[i];
        if (results[i] != expected) {
            printf("  ERROR at %d: got %f, expected %f\n", i, results[i], expected);
            errors++;
        }
    }

    /* Cleanup */
    mental_reference_close(input0);
    mental_reference_close(input1);
    mental_reference_close(output);
    mental_kernel_finalize(kernel);

    if (errors > 0) {
        printf("FAIL: %d computation errors\n", errors);
        return 1;
    }

    printf("PASS: Direct Metal shader works\n");
    return 0;
}
