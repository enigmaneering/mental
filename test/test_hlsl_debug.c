/* Debug HLSL transpilation */
#include "../mental.h"
#include "../mental_internal.h"
#include "../transpile.h"
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
    printf("Testing HLSL compilation to SPIRV...\n\n");

    char error[1024] = {0};
    size_t spirv_len = 0;

    unsigned char* spirv = mental_hlsl_to_spirv(hlsl_shader, strlen(hlsl_shader),
                                                 &spirv_len, error, sizeof(error));

    if (!spirv) {
        printf("ERROR: HLSL compilation failed\n");
        printf("Error message: %s\n", error);
        return 1;
    }

    printf("SUCCESS: HLSL compiled to SPIRV\n");
    printf("SPIRV size: %zu bytes\n", spirv_len);

    free(spirv);
    return 0;
}
