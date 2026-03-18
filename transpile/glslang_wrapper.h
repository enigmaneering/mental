#ifndef GLSLANG_WRAPPER_H
#define GLSLANG_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

// Initialize glslang - must be called once before any compilation
void glslang_initialize();

// Compile GLSL compute shader source to SPIRV binary
// Returns 0 on success, non-zero on error
// On success, spirvData and spirvSize contain the compiled binary (caller must free spirvData)
// On error, errorMsg contains the error message (caller must free errorMsg)
int compile_glsl_to_spirv(
    const char* source,
    char** spirvData,
    int* spirvSize,
    char** errorMsg
);

#ifdef __cplusplus
}
#endif

#endif // GLSLANG_WRAPPER_H
