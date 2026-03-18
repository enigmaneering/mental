#ifndef SPIRV_CROSS_WRAPPER_H
#define SPIRV_CROSS_WRAPPER_H

#ifdef __cplusplus
extern "C" {
#endif

// Target languages for SPIRV-Cross transpilation
typedef enum {
    TARGET_GLSL = 0,
    TARGET_HLSL = 1,
    TARGET_MSL = 2
} TargetLanguage;

// Transpile SPIRV binary to target language
// Returns 0 on success, non-zero on error
// On success, outputSource contains the transpiled source (caller must free)
// On error, errorMsg contains the error message (caller must free errorMsg)
int transpile_spirv(
    const char* spirvData,
    int spirvSize,
    TargetLanguage target,
    char** outputSource,
    char** errorMsg
);

#ifdef __cplusplus
}
#endif

#endif // SPIRV_CROSS_WRAPPER_H
