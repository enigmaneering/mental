/*
 * Mental - GLSL to SPIRV compilation (using glslang)
 */

#include "transpile.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <vector>
#include <cstring>

static bool g_glslang_initialized = false;

extern "C" {

static void ensure_glslang_init() {
    if (!g_glslang_initialized) {
        glslang::InitializeProcess();
        g_glslang_initialized = true;
    }
}

unsigned char* mental_glsl_to_spirv(const char* source, size_t source_len,
                                    size_t* out_len, char* error, size_t error_len) {
    ensure_glslang_init();

    /* Create shader */
    glslang::TShader shader(EShLangCompute);
    shader.setStrings(&source, 1);
    shader.setEnvInput(glslang::EShSourceGlsl, EShLangCompute, glslang::EShClientVulkan, 450);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    /* Parse */
    const TBuiltInResource* resources = GetDefaultResources();
    EShMessages messages = (EShMessages)(EShMsgSpvRules | EShMsgVulkanRules);

    if (!shader.parse(resources, 450, false, messages)) {
        if (error) {
            strncpy(error, shader.getInfoLog(), error_len - 1);
            error[error_len - 1] = '\0';
        }
        return nullptr;
    }

    /* Link */
    glslang::TProgram program;
    program.addShader(&shader);

    if (!program.link(messages)) {
        if (error) {
            strncpy(error, program.getInfoLog(), error_len - 1);
            error[error_len - 1] = '\0';
        }
        return nullptr;
    }

    /* Convert to SPIRV */
    std::vector<unsigned int> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(EShLangCompute), spirv);

    /* Copy to output buffer */
    size_t spirv_bytes = spirv.size() * sizeof(unsigned int);
    unsigned char* result = (unsigned char*)malloc(spirv_bytes);
    if (!result) {
        if (error) {
            strncpy(error, "Memory allocation failed", error_len - 1);
            error[error_len - 1] = '\0';
        }
        return nullptr;
    }

    memcpy(result, spirv.data(), spirv_bytes);
    *out_len = spirv_bytes;

    return result;
}

} /* extern "C" */
