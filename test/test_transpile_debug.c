/* Debug test to see what transpilation produces */
#include "../mental.h"
#include "../transpile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    printf("=== Transpilation Debug Test ===\n\n");

    /* Test GLSL to SPIRV */
    printf("1. Compiling GLSL to SPIRV...\n");
    size_t spirv_len = 0;
    char error[1024] = {0};
    unsigned char* spirv = mental_glsl_to_spirv(glsl_shader, strlen(glsl_shader),
                                                  &spirv_len, error, sizeof(error));

    if (!spirv) {
        printf("   FAILED: %s\n", error);
        return 1;
    }
    printf("   SUCCESS: Generated %zu bytes of SPIRV\n", spirv_len);

    /* Test SPIRV to MSL */
    printf("\n2. Transpiling SPIRV to MSL...\n");
    size_t msl_len = 0;
    char* msl = mental_spirv_to_msl(spirv, spirv_len, &msl_len, error, sizeof(error));

    if (!msl) {
        printf("   FAILED: %s\n", error);
        free(spirv);
        return 1;
    }
    printf("   SUCCESS: Generated %zu bytes of MSL\n", msl_len);

    printf("\n=== Generated MSL Code ===\n");
    printf("%s\n", msl);
    printf("=== End MSL Code ===\n");

    /* Try compiling with Mental */
    printf("\n3. Testing with Mental...\n");
    mental_device dev = mental_device_get(0);
    if (!dev) {
        printf("   FAILED: No device\n");
        free(spirv);
        free(msl);
        return 1;
    }

    mental_kernel kernel = mental_compile(dev, glsl_shader, strlen(glsl_shader));
    if (!kernel) {
        printf("   FAILED: %s\n", mental_get_error_message());
        free(spirv);
        free(msl);
        return 1;
    }
    printf("   SUCCESS: Kernel compiled\n");

    /* Test execution */
    size_t count = 4;  // Small test
    size_t size = count * sizeof(float);

    mental_reference input0 = mental_alloc(dev, size);
    mental_reference input1 = mental_alloc(dev, size);
    mental_reference output = mental_alloc(dev, size);

    float data0[] = {1.0f, 2.0f, 3.0f, 4.0f};
    float data1[] = {10.0f, 20.0f, 30.0f, 40.0f};

    mental_write(input0, data0, size);
    mental_write(input1, data1, size);

    mental_reference inputs[] = {input0, input1};
    mental_dispatch(kernel, inputs, 2, output, count);

    float results[4] = {0};
    mental_read(output, results, size);

    printf("\n=== Execution Results ===\n");
    for (int i = 0; i < 4; i++) {
        float expected = data0[i] + data1[i];
        printf("   [%d]: %.1f + %.1f = %.1f (expected %.1f) %s\n",
               i, data0[i], data1[i], results[i], expected,
               results[i] == expected ? "✓" : "✗");
    }

    mental_finalize(input0);
    mental_finalize(input1);
    mental_finalize(output);
    mental_kernel_finalize(kernel);
    free(spirv);
    free(msl);

    return 0;
}
