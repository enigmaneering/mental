/* Test full HLSL compilation pipeline */
#include "../mental.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    printf("Testing full HLSL compilation pipeline...\n");

    mental_device dev = mental_device_get(0);
    if (!dev) {
        printf("ERROR: Failed to get device: %s\n", mental_get_error_message());
        return 1;
    }

    printf("Device API: %s\n", mental_device_api_name(dev));

    /* Compile HLSL shader */
    printf("\nCompiling HLSL shader...\n");
    mental_kernel kernel = mental_compile(dev, hlsl_shader, strlen(hlsl_shader));

    if (!kernel) {
        printf("ERROR: Compilation failed: %s\n", mental_get_error_message());
        return 1;
    }

    printf("SUCCESS: Kernel compiled\n");

    /* Test execution */
    size_t count = 256;
    size_t size = count * sizeof(float);

    mental_reference input0 = mental_reference_create("hlsl-full-in0", size);
    mental_reference_pin(input0, dev);
    mental_reference input1 = mental_reference_create("hlsl-full-in1", size);
    mental_reference_pin(input1, dev);
    mental_reference output = mental_reference_create("hlsl-full-out", size);
    mental_reference_pin(output, dev);

    if (!input0 || !input1 || !output) {
        printf("ERROR: Buffer allocation failed\n");
        return 1;
    }

    float data0[256], data1[256];
    for (int i = 0; i < 256; i++) {
        data0[i] = (float)i;
        data1[i] = (float)i * 2.0f;
    }

    mental_reference_write(input0, data0, size);
    mental_reference_write(input1, data1, size);

    printf("Dispatching kernel...\n");
    mental_reference inputs[] = { input0, input1 };
    mental_dispatch(kernel, inputs, 2, output, count);

    if (mental_get_error() != MENTAL_SUCCESS) {
        printf("ERROR: Dispatch failed: %s\n", mental_get_error_message());
        return 1;
    }

    float results[256];
    mental_reference_read(output, results, size);

    /* Verify results */
    int errors = 0;
    for (int i = 0; i < 256 && errors < 5; i++) {
        float expected = data0[i] + data1[i];
        if (results[i] != expected) {
            printf("ERROR at %d: got %f, expected %f\n", i, results[i], expected);
            errors++;
        }
    }

    mental_reference_close(input0);
    mental_reference_close(input1);
    mental_reference_close(output);
    mental_kernel_finalize(kernel);

    if (errors > 0) {
        printf("FAIL: %d errors in computation\n", errors);
        return 1;
    }

    printf("\nPASS: HLSL shader executed correctly\n");
    return 0;
}
