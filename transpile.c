/*
 * Mental - Shader Transpilation Implementation
 */

#include "transpile.h"
#include "mental_internal.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* Language detection patterns */
static int contains(const char* haystack, size_t len, const char* needle) {
    size_t needle_len = strlen(needle);
    if (needle_len > len) return 0;

    for (size_t i = 0; i <= len - needle_len; i++) {
        if (strncmp(haystack + i, needle, needle_len) == 0) {
            return 1;
        }
    }
    return 0;
}

mental_language mental_detect_language(const char* source, size_t source_len) {
    if (!source || source_len == 0) return MENTAL_LANG_UNKNOWN;

    /* Check for SPIRV binary (magic number: 0x07230203) */
    if (source_len >= 4) {
        unsigned char* bytes = (unsigned char*)source;
        if (bytes[0] == 0x03 && bytes[1] == 0x02 && bytes[2] == 0x23 && bytes[3] == 0x07) {
            return MENTAL_LANG_SPIRV;
        }
    }

    /* WGSL detection */
    if (contains(source, source_len, "@compute") ||
        contains(source, source_len, "@vertex") ||
        contains(source, source_len, "@fragment") ||
        contains(source, source_len, "fn ")) {
        return MENTAL_LANG_WGSL;
    }

    /* MSL detection */
    if (contains(source, source_len, "kernel void") ||
        contains(source, source_len, "[[kernel]]") ||
        contains(source, source_len, "device ") ||
        contains(source, source_len, "threadgroup ")) {
        return MENTAL_LANG_MSL;
    }

    /* HLSL detection */
    if (contains(source, source_len, "[numthreads(") ||
        contains(source, source_len, "RWStructuredBuffer") ||
        contains(source, source_len, "StructuredBuffer") ||
        contains(source, source_len, "cbuffer ")) {
        return MENTAL_LANG_HLSL;
    }

    /* GLSL detection (default for compute shaders) */
    if (contains(source, source_len, "#version") ||
        contains(source, source_len, "layout(") ||
        contains(source, source_len, "gl_") ||
        contains(source, source_len, "main()")) {
        return MENTAL_LANG_GLSL;
    }

    /* Default to GLSL for compute shaders */
    return MENTAL_LANG_GLSL;
}

mental_language mental_api_to_language(mental_api_type api) {
    switch (api) {
        case MENTAL_API_METAL: return MENTAL_LANG_MSL;
        case MENTAL_API_D3D12: return MENTAL_LANG_HLSL;
        case MENTAL_API_VULKAN: return MENTAL_LANG_GLSL;
        case MENTAL_API_OPENCL: return MENTAL_LANG_GLSL; /* OpenCL uses GLSL compute */
        case MENTAL_API_OPENGL: return MENTAL_LANG_GLSL; /* OpenGL 4.3+ uses GLSL compute */
        case MENTAL_API_POCL:   return MENTAL_LANG_GLSL; /* PoCL uses OpenCL C (GLSL-like) */
        default: return MENTAL_LANG_GLSL;
    }
}

/* Main transpilation function (called from mental.c) */
char* mental_transpile(const char* source, size_t source_len, mental_api_type target_api, size_t* out_len) {
    mental_language src_lang = mental_detect_language(source, source_len);
    mental_language target_lang = mental_api_to_language(target_api);

    /* No transpilation needed if already in target language */
    if (src_lang == target_lang) {
        char* result = malloc(source_len + 1);
        if (!result) return NULL;
        memcpy(result, source, source_len);
        result[source_len] = '\0';
        *out_len = source_len;
        return result;
    }

    /* Transpilation pipeline: Source -> SPIRV -> Target */
    unsigned char* spirv = NULL;
    size_t spirv_len = 0;
    char error[1024] = {0};

    /* Step 1: Compile source to SPIRV */
    switch (src_lang) {
        case MENTAL_LANG_GLSL:
            spirv = mental_glsl_to_spirv(source, source_len, &spirv_len, error, sizeof(error));
            break;
        case MENTAL_LANG_HLSL:
            spirv = mental_hlsl_to_spirv(source, source_len, &spirv_len, error, sizeof(error));
            break;
        case MENTAL_LANG_WGSL:
            spirv = mental_wgsl_to_spirv(source, source_len, &spirv_len, error, sizeof(error));
            break;
        case MENTAL_LANG_SPIRV:
            /* Already SPIRV, just copy */
            spirv = malloc(source_len);
            if (spirv) {
                memcpy(spirv, source, source_len);
                spirv_len = source_len;
            }
            break;
        case MENTAL_LANG_MSL:
            /* MSL can only be used directly on Metal backend, not transpiled from */
            fprintf(stderr, "ERROR: MSL shaders cannot be transpiled to other backends. Use on Metal only.\n");
            return NULL;
        default:
            fprintf(stderr, "ERROR: Unknown or unsupported source language\n");
            return NULL;
    }

    if (!spirv) {
        fprintf(stderr, "ERROR: Failed to compile source to SPIRV: %s\n", error[0] ? error : "Unknown error");
        return NULL;
    }

    /* Step 2: Transpile SPIRV to target language */
    char* result = NULL;
    switch (target_lang) {
        case MENTAL_LANG_GLSL:
            result = mental_spirv_to_glsl(spirv, spirv_len, out_len, error, sizeof(error));
            break;
        case MENTAL_LANG_HLSL:
            result = mental_spirv_to_hlsl(spirv, spirv_len, out_len, error, sizeof(error));
            break;
        case MENTAL_LANG_MSL:
            result = mental_spirv_to_msl(spirv, spirv_len, out_len, error, sizeof(error));
            break;
        case MENTAL_LANG_WGSL:
            result = mental_spirv_to_wgsl(spirv, spirv_len, out_len, error, sizeof(error));
            break;
        default:
            fprintf(stderr, "ERROR: Unknown or unsupported target language\n");
            free(spirv);
            return NULL;
    }

    if (!result) {
        fprintf(stderr, "ERROR: Failed to transpile SPIRV to target: %s\n", error[0] ? error : "Unknown error");
    }

    free(spirv);
    return result;
}

void mental_transpile_free(char* result) {
    if (result) free(result);
}
