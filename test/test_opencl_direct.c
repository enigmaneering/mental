/* Direct OpenCL shader test - native OpenCL C, no transpilation */
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

/* Native OpenCL C kernel */
const char* opencl_shader =
    "__kernel void compute_main(\n"
    "    __global const float* input0,\n"
    "    __global const float* input1,\n"
    "    __global float* output)\n"
    "{\n"
    "    int idx = get_global_id(0);\n"
    "    output[idx] = input0[idx] + input1[idx];\n"
    "}\n";

int main(void) {
    printf("Testing direct OpenCL shader...\n");

    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to get device");
    ASSERT_NO_ERROR();

    const char* api_name = mental_device_api_name(dev);
    printf("  Device API: %s\n", api_name);

    if (strstr(api_name, "OpenCL") == NULL) {
        printf("SKIP: Not on OpenCL backend\n");
        return 0;
    }

    /* Compile shader */
    mental_kernel kernel = mental_compile(dev, opencl_shader, strlen(opencl_shader));
    if (kernel == NULL) {
        printf("FAIL: Compilation failed: %s\n", mental_get_error_message());
        return 1;
    }

    /* Create test buffers */
    size_t count = 256;
    size_t size = count * sizeof(float);

    mental_reference input0 = mental_alloc(dev, size);
    mental_reference input1 = mental_alloc(dev, size);
    mental_reference output = mental_alloc(dev, size);

    ASSERT(input0 != NULL, "Failed to allocate input0");
    ASSERT(input1 != NULL, "Failed to allocate input1");
    ASSERT(output != NULL, "Failed to allocate output");
    ASSERT_NO_ERROR();

    /* Fill input buffers */
    float data0[256], data1[256];
    for (int i = 0; i < 256; i++) {
        data0[i] = (float)i;
        data1[i] = (float)i * 2.0f;
    }
    mental_write(input0, data0, size);
    mental_write(input1, data1, size);
    ASSERT_NO_ERROR();

    /* Dispatch kernel */
    mental_reference inputs[] = { input0, input1 };
    mental_dispatch(kernel, inputs, 2, output, count);
    ASSERT_NO_ERROR();

    /* Read and verify results */
    float results[256];
    mental_read(output, results, size);
    ASSERT_NO_ERROR();

    int errors = 0;
    for (int i = 0; i < 256 && errors < 5; i++) {
        float expected = data0[i] + data1[i];
        if (results[i] != expected) {
            printf("  ERROR at %d: got %f, expected %f\n", i, results[i], expected);
            errors++;
        }
    }

    /* Cleanup */
    mental_finalize(input0);
    mental_finalize(input1);
    mental_finalize(output);
    mental_kernel_finalize(kernel);

    if (errors > 0) {
        printf("FAIL: OpenCL shader produced incorrect results (%d errors)\n", errors);
        return 1;
    }

    printf("PASS: Direct OpenCL shader executed correctly\n");
    return 0;
}
