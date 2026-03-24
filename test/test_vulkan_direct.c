/* Direct Vulkan shader test - native SPIRV binary, no transpilation */
#include "../mental.h"
#include "../transpile.h"
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

/* GLSL source to compile to SPIRV */
const char* glsl_shader =
    "#version 450\n"
    "layout(local_size_x = 256) in;\n"
    "layout(binding = 0) buffer Input0 { float data[]; } input0;\n"
    "layout(binding = 1) buffer Input1 { float data[]; } input1;\n"
    "layout(binding = 2) buffer Output { float data[]; } output_buf;\n"
    "void main() {\n"
    "    uint idx = gl_GlobalInvocationID.x;\n"
    "    output_buf.data[idx] = input0.data[idx] + input1.data[idx];\n"
    "}\n";

int main(void) {
    printf("Testing direct Vulkan shader (SPIRV binary)...\n");

    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to get device");
    ASSERT_NO_ERROR();

    const char* api_name = mental_device_api_name(dev);
    printf("  Device API: %s\n", api_name);

    if (strstr(api_name, "Vulkan") == NULL) {
        printf("SKIP: Not on Vulkan backend\n");
        return 0;
    }

    /* Compile GLSL to SPIRV binary */
    printf("  Compiling GLSL to SPIRV...\n");
    size_t spirv_len = 0;
    char error[1024] = {0};
    unsigned char* spirv = mental_glsl_to_spirv(glsl_shader, strlen(glsl_shader),
                                                 &spirv_len, error, sizeof(error));
    if (!spirv) {
        printf("FAIL: GLSL to SPIRV compilation failed: %s\n", error);
        return 1;
    }
    printf("  SPIRV binary size: %zu bytes\n", spirv_len);

    /* Compile SPIRV binary directly (Vulkan's native format) */
    mental_kernel kernel = mental_compile(dev, (const char*)spirv, spirv_len);
    free(spirv);

    if (kernel == NULL) {
        printf("FAIL: Kernel compilation failed: %s\n", mental_get_error_message());
        return 1;
    }

    /* Create test buffers */
    size_t count = 256;
    size_t size = count * sizeof(float);

    mental_reference input0 = mental_reference_create("vulkan-in0", size);
    mental_reference_pin(input0, dev);
    mental_reference input1 = mental_reference_create("vulkan-in1", size);
    mental_reference_pin(input1, dev);
    mental_reference output = mental_reference_create("vulkan-out", size);
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
        printf("FAIL: Vulkan shader produced incorrect results (%d errors)\n", errors);
        return 1;
    }

    printf("PASS: Direct Vulkan SPIRV shader executed correctly\n");
    return 0;
}
