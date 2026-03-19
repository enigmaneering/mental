/* Kernel operations test: compile and dispatch */
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

/* Simple GLSL compute shader that adds two arrays */
const char* add_shader_glsl =
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
    printf("Testing kernel operations...\n");

    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to create device");
    ASSERT_NO_ERROR();

    /* Compile kernel */
    mental_kernel kernel = mental_compile(dev, add_shader_glsl, strlen(add_shader_glsl));
    ASSERT(kernel != NULL, "Failed to compile kernel");
    ASSERT_NO_ERROR();

    /* Create input buffers */
    size_t count = 256;
    size_t size = count * sizeof(float);

    mental_reference input0 = mental_alloc(dev, size);
    mental_reference input1 = mental_alloc(dev, size);
    mental_reference output = mental_alloc(dev, size);
    ASSERT(input0 && input1 && output, "Failed to allocate buffers");
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

    /* Read results */
    float results[256];
    mental_read(output, results, size);
    ASSERT_NO_ERROR();

    /* Verify computation */
    int match = 1;
    for (int i = 0; i < 256; i++) {
        float expected = data0[i] + data1[i];
        if (results[i] != expected) {
            fprintf(stderr, "Mismatch at %d: got %f, expected %f\n",
                    i, results[i], expected);
            match = 0;
            break;
        }
    }
    ASSERT(match, "Kernel computation incorrect");

    /* Cleanup */
    mental_finalize(input0);
    mental_finalize(input1);
    mental_finalize(output);
    mental_kernel_finalize(kernel);
    
    ASSERT_NO_ERROR();

    printf("PASS: All kernel tests passed\n");
    return 0;
}
