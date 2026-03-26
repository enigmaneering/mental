/* Multi-language shader test: GLSL, HLSL, MSL, WGSL, ESSL */
#include "../mental.h"
#include "../transpile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#else
#include <direct.h>
#include <io.h>
#define getcwd _getcwd
#define access _access
#ifndef F_OK
#define F_OK 0
#endif
#endif

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

/* Simple compute shader that adds two arrays - GLSL 4.50 */
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

/* Same shader in ESSL 3.10 (OpenGL ES) */
const char* essl_shader =
    "#version 310 es\n"
    "layout(local_size_x = 256) in;\n"
    "layout(binding = 0) buffer Input0 { float data[]; } input0;\n"
    "layout(binding = 1) buffer Input1 { float data[]; } input1;\n"
    "layout(binding = 2) buffer Output { float data[]; } output_buf;\n"
    "void main() {\n"
    "    uint idx = gl_GlobalInvocationID.x;\n"
    "    output_buf.data[idx] = input0.data[idx] + input1.data[idx];\n"
    "}\n";

/* Same shader in HLSL */
const char* hlsl_shader =
    "RWStructuredBuffer<float> input0 : register(u0);\n"
    "RWStructuredBuffer<float> input1 : register(u1);\n"
    "RWStructuredBuffer<float> output : register(u2);\n"
    "\n"
    "[numthreads(256, 1, 1)]\n"
    "void main(uint3 id : SV_DispatchThreadID) {\n"
    "    output[id.x] = input0[id.x] + input1[id.x];\n"
    "}\n";

/* Same shader in MSL (Metal Shading Language) */
const char* msl_shader =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "\n"
    "kernel void main0(\n"
    "    device float* input0 [[buffer(0)]],\n"
    "    device float* input1 [[buffer(1)]],\n"
    "    device float* output [[buffer(2)]],\n"
    "    uint id [[thread_position_in_grid]])\n"
    "{\n"
    "    output[id] = input0[id] + input1[id];\n"
    "}\n";

/* Same shader in WGSL (WebGPU Shading Language) */
const char* wgsl_shader =
    "@group(0) @binding(0) var<storage, read> input0: array<f32>;\n"
    "@group(0) @binding(1) var<storage, read> input1: array<f32>;\n"
    "@group(0) @binding(2) var<storage, read_write> output: array<f32>;\n"
    "\n"
    "@compute @workgroup_size(256)\n"
    "fn main(@builtin(global_invocation_id) id: vec3<u32>) {\n"
    "    output[id.x] = input0[id.x] + input1[id.x];\n"
    "}\n";

static int test_shader(const char* name, const char* shader_source, mental_device dev, int required) {
    printf("  Testing %s shader...\n", name);

    /* Compile shader */
    mental_kernel kernel = mental_compile(dev, shader_source, strlen(shader_source));
    if (kernel == NULL) {
        mental_error err = mental_get_error();
        if (err != MENTAL_SUCCESS) {
            if (required) {
                printf("    FAIL: %s shader REQUIRED but failed: %s\n", name, mental_get_error_message());
                return 1; /* Hard failure for required shaders */
            } else {
                printf("    SKIP: %s not supported (%s)\n", name, mental_get_error_message());
                return 0; /* Skip for optional shaders */
            }
        }
        printf("    FAIL: Failed to compile %s shader\n", name);
        return 1;
    }

    /* Create test buffers */
    size_t count = 256;
    size_t size = count * sizeof(float);

    /* Build unique reference names from shader language name */
    char name_in0[64], name_in1[64], name_out[64];
    char lower[16];
    for (int i = 0; name[i] && i < 15; i++) {
        lower[i] = (name[i] >= 'A' && name[i] <= 'Z') ? name[i] + 32 : name[i];
        lower[i+1] = '\0';
    }
    snprintf(name_in0, sizeof(name_in0), "%s-in0", lower);
    snprintf(name_in1, sizeof(name_in1), "%s-in1", lower);
    snprintf(name_out, sizeof(name_out), "%s-out", lower);

    mental_reference input0 = mental_reference_create(name_in0, size);
    mental_reference_pin(input0, dev);
    mental_reference input1 = mental_reference_create(name_in1, size);
    mental_reference_pin(input1, dev);
    mental_reference output = mental_reference_create(name_out, size);
    mental_reference_pin(output, dev);

    if (!input0 || !input1 || !output) {
        printf("    FAIL: Failed to allocate buffers\n");
        if (input0) mental_reference_close(input0);
        if (input1) mental_reference_close(input1);
        if (output) mental_reference_close(output);
        mental_kernel_finalize(kernel);
        return 1;
    }

    /* Fill input buffers */
    float data0[256], data1[256];
    for (int i = 0; i < 256; i++) {
        data0[i] = (float)i;
        data1[i] = (float)i * 2.0f;
    }
    mental_reference_write(input0, data0, size);
    mental_reference_write(input1, data1, size);

    /* Dispatch kernel */
    mental_reference inputs[] = { input0, input1 };
    mental_dispatch(kernel, inputs, 2, output, count);

    if (mental_get_error() != MENTAL_SUCCESS) {
        printf("    FAIL: Dispatch failed: %s\n", mental_get_error_message());
        mental_reference_close(input0);
        mental_reference_close(input1);
        mental_reference_close(output);
        mental_kernel_finalize(kernel);
        return 1;
    }

    /* Read and verify results */
    float results[256];
    mental_reference_read(output, results, size);

    int errors = 0;
    for (int i = 0; i < 256 && errors < 5; i++) {
        float expected = data0[i] + data1[i];
        if (results[i] != expected) {
            printf("    ERROR at %d: got %f, expected %f\n", i, results[i], expected);
            errors++;
        }
    }

    /* Cleanup */
    mental_reference_close(input0);
    mental_reference_close(input1);
    mental_reference_close(output);
    mental_kernel_finalize(kernel);

    if (errors > 0) {
        printf("    FAIL: %s shader produced incorrect results (%d errors)\n", name, errors);
        return 1;
    }

    printf("    PASS: %s shader\n", name);
    return 0;
}

/* Auto-detect DXC and Naga from common paths */
static void setup_tools(void) {
    if (!mental_get_tool_path(MENTAL_TOOL_DXC)) {
        const char *env = getenv("MENTAL_DXC_PATH");
        if (env) { mental_set_tool_path(MENTAL_TOOL_DXC, env); }
        else {
            const char *paths[] = {
                "../external/dxc/bin/dxc", "../../external/dxc/bin/dxc",
                "../external/dxc/bin/dxc.exe", "../../external/dxc/bin/dxc.exe",
                "../external/dxc/dxc", "../../external/dxc/dxc",
                "../external/dxc/dxc.exe", "../../external/dxc/dxc.exe",
                NULL};
            for (int i = 0; paths[i]; i++)
                if (access(paths[i], F_OK) == 0) { mental_set_tool_path(MENTAL_TOOL_DXC, paths[i]); break; }
        }
    }
    if (!mental_get_tool_path(MENTAL_TOOL_NAGA)) {
        const char *env = getenv("MENTAL_NAGA_PATH");
        if (env) { mental_set_tool_path(MENTAL_TOOL_NAGA, env); }
        else {
            const char *paths[] = {"../external/naga/bin/naga", "../../external/naga/bin/naga", NULL};
            for (int i = 0; paths[i]; i++)
                if (access(paths[i], F_OK) == 0) { mental_set_tool_path(MENTAL_TOOL_NAGA, paths[i]); break; }
        }
    }
}

int main(void) {
    printf("Testing multi-language shader support...\n");

    setup_tools();

    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd))) {
        printf("  Working directory: %s\n", cwd);
    }

    mental_device dev = mental_device_get(0);
    ASSERT(dev != NULL, "Failed to get device");
    ASSERT_NO_ERROR();

    const char* api_name = mental_device_api_name(dev);
    printf("  Device API: %s\n", api_name);

    int failures = 0;

    /* Test GLSL (REQUIRED - should work on all backends via SPIRV) */
    failures += test_shader("GLSL", glsl_shader, dev, 1);

    /* Test ESSL (REQUIRED - OpenGL ES Shading Language) */
    failures += test_shader("ESSL", essl_shader, dev, 1);

    /* Test HLSL (REQUIRED - must work through transpilation) */
    failures += test_shader("HLSL", hlsl_shader, dev, 1);

    /* Test MSL (REQUIRED on Metal, skip on other backends) */
    if (strstr(api_name, "Metal") != NULL) {
        failures += test_shader("MSL", msl_shader, dev, 1);
    } else {
        printf("  SKIP: MSL shader (not on Metal backend)\n");
    }

    /* Test WGSL (REQUIRED - WebGPU Shading Language) */
    failures += test_shader("WGSL", wgsl_shader, dev, 1);

    if (failures > 0) {
        printf("FAIL: %d shader(s) failed\n", failures);
        return 1;
    }

    printf("PASS: All supported shaders passed\n");
    return 0;
}
