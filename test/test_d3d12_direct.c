/* Direct D3D12 shader test - native HLSL, no transpilation */
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

/* Native HLSL compute shader */
const char* hlsl_shader =
    "RWStructuredBuffer<float> input0 : register(u0);\n"
    "RWStructuredBuffer<float> input1 : register(u1);\n"
    "RWStructuredBuffer<float> output : register(u2);\n"
    "\n"
    "[numthreads(256, 1, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    output[id.x] = input0[id.x] + input1[id.x];\n"
    "}\n";

int main(void) {
    printf("Testing direct D3D12 shader...\n");

    if (mental_device_count() == 0) {
        printf("SKIP: No GPU devices available\n");
        return 0;
    }
    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to get device");
    ASSERT_NO_ERROR();

    const char* api_name = mental_device_api_name(dev);
    printf("  Device API: %s\n", api_name);

    if (strstr(api_name, "D3D12") == NULL && strstr(api_name, "Direct3D") == NULL) {
        printf("SKIP: Not on D3D12 backend\n");
        return 0;
    }

    /* Compile shader */
    mental_kernel kernel = mental_compile(dev, hlsl_shader, strlen(hlsl_shader));
    if (kernel == NULL) {
        printf("FAIL: Compilation failed: %s\n", mental_get_error_message());
        return 1;
    }

    /* Create test buffers */
    size_t count = 256;
    size_t size = count * sizeof(float);

    mental_reference input0 = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_pin(input0, dev);
    mental_reference input1 = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_pin(input1, dev);
    mental_reference output = mental_reference_create(size, MENTAL_RELATIONALLY_OPEN, NULL, 0, NULL);
    mental_reference_pin(output, dev);

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
    mental_reference_write(input0, data0, size);
    mental_reference_write(input1, data1, size);
    ASSERT_NO_ERROR();

    /* Dispatch kernel */
    mental_reference inputs[] = { input0, input1 };
    mental_dispatch(kernel, inputs, 2, output, count);
    ASSERT_NO_ERROR();

    /* Read and verify results */
    float results[256];
    mental_reference_read(output, results, size);
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
    mental_reference_close(input0);
    mental_reference_close(input1);
    mental_reference_close(output);
    mental_kernel_finalize(kernel);

    if (errors > 0) {
        printf("FAIL: D3D12 shader produced incorrect results (%d errors)\n", errors);
        return 1;
    }

    printf("PASS: Direct D3D12 shader executed correctly\n");
    return 0;
}
