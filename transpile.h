/*
 * Mental - Shader Transpilation
 *
 * Language detection and SPIRV-based transpilation.
 * Supports: GLSL, HLSL, MSL, WGSL
 */

#ifndef MENTAL_TRANSPILE_H
#define MENTAL_TRANSPILE_H

#include "mental.h"
#include <stddef.h>


#ifdef __cplusplus
extern "C" {
#endif

/* Shader language types */
typedef enum {
    MENTAL_LANG_UNKNOWN = 0,
    MENTAL_LANG_GLSL,
    MENTAL_LANG_HLSL,
    MENTAL_LANG_MSL,
    MENTAL_LANG_WGSL,
    MENTAL_LANG_SPIRV
} mental_language;

/* Detect shader language from source code */
mental_language mental_detect_language(const char* source, size_t source_len);

/* Compile GLSL to SPIRV */
unsigned char* mental_glsl_to_spirv(const char* source, size_t source_len, size_t* out_len, char* error, size_t error_len);

/* Compile HLSL to SPIRV */
unsigned char* mental_hlsl_to_spirv(const char* source, size_t source_len, size_t* out_len, char* error, size_t error_len);

/* Compile WGSL to SPIRV */
unsigned char* mental_wgsl_to_spirv(const char* source, size_t source_len, size_t* out_len, char* error, size_t error_len);

/* Transpile SPIRV to GLSL */
char* mental_spirv_to_glsl(const unsigned char* spirv, size_t spirv_len, size_t* out_len, char* error, size_t error_len);

/* Transpile SPIRV to HLSL */
char* mental_spirv_to_hlsl(const unsigned char* spirv, size_t spirv_len, size_t* out_len, char* error, size_t error_len);

/* Transpile SPIRV to MSL */
char* mental_spirv_to_msl(const unsigned char* spirv, size_t spirv_len, size_t* out_len, char* error, size_t error_len);

/* Transpile SPIRV to WGSL */
char* mental_spirv_to_wgsl(const unsigned char* spirv, size_t spirv_len, size_t* out_len, char* error, size_t error_len);

/* Transpile GLSL (from spirv-cross) to OpenCL C source code.
 * Used by the OpenCL/PoCL backends when clCreateProgramWithIL isn't
 * available or rejects Vulkan SPIR-V.
 * Pipeline: GLSL → SPIR-V → GLSL (spirv-cross) → OpenCL C (this). */
char* mental_glsl_to_opencl_c(const char* glsl_source, size_t glsl_len,
                               size_t* out_len, char* error, size_t error_len);

/* Free transpilation results */
void mental_transpile_free(char* result);

/* Map API type to target language */
mental_language mental_api_to_language(mental_api_type api);

/* External tool identifiers */
typedef enum {
    MENTAL_TOOL_DXC  = 0,
    MENTAL_TOOL_NAGA = 1,
    MENTAL_TOOL_POCL = 2
} mental_tool;

/* Configure the path to an external tool (DXC, Naga).
 * Must be an absolute or resolvable path to the executable.
 * Pass NULL to clear a previously set path. */
void mental_set_tool_path(mental_tool tool, const char* path);

/* Query the configured path for an external tool.
 * Returns NULL if no path has been explicitly set. */
const char* mental_get_tool_path(mental_tool tool);

#ifdef __cplusplus
}
#endif

#endif /* MENTAL_TRANSPILE_H */
