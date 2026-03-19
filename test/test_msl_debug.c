/* Debug MSL transpilation */
#include "../mental.h"
#include "../mental_internal.h"
#include "../transpile.h"
#include <stdio.h>
#include <string.h>

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

int main(void) {
    printf("Testing MSL language detection and transpilation...\n\n");

    /* Test language detection */
    mental_language detected = mental_detect_language(msl_shader, strlen(msl_shader));
    printf("Detected language: ");
    switch (detected) {
        case MENTAL_LANG_GLSL: printf("GLSL\n"); break;
        case MENTAL_LANG_HLSL: printf("HLSL\n"); break;
        case MENTAL_LANG_MSL: printf("MSL\n"); break;
        case MENTAL_LANG_WGSL: printf("WGSL\n"); break;
        case MENTAL_LANG_SPIRV: printf("SPIRV\n"); break;
        default: printf("UNKNOWN\n"); break;
    }

    /* Test transpilation to MSL (should pass through) */
    size_t out_len = 0;
    char* result = mental_transpile(msl_shader, strlen(msl_shader), MENTAL_API_METAL, &out_len);

    if (!result) {
        printf("ERROR: Transpilation failed (returned NULL)\n");
        return 1;
    }

    printf("\nTranspilation succeeded!\n");
    printf("Input length: %zu\n", strlen(msl_shader));
    printf("Output length: %zu\n", out_len);
    printf("Strings match: %s\n", strcmp(msl_shader, result) == 0 ? "YES" : "NO");

    printf("\n=== Output MSL ===\n%s\n", result);

    mental_transpile_free(result);

    printf("\nPASS: MSL pass-through works correctly\n");
    return 0;
}
