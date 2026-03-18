
#include "glslang_wrapper.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#include <string>
#include <vector>
#include <cstring>

static bool glslangInitialized = false;

void glslang_initialize() {
    if (!glslangInitialized) {
        glslang::InitializeProcess();
        glslangInitialized = true;
    }
}

int compile_glsl_to_spirv(
    const char* source,
    char** spirvData,
    int* spirvSize,
    char** errorMsg
) {
    // Set up compute shader
    const EShLanguage stage = EShLangCompute;
    glslang::TShader shader(stage);
    shader.setStrings(&source, 1);

    // Set environment to Vulkan 1.0 with SPIRV 1.0
    shader.setEnvInput(glslang::EShSourceGlsl, stage, glslang::EShClientVulkan, 100);
    shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
    shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

    // Parse shader
    const int defaultVersion = 100;
    const TBuiltInResource* resources = GetDefaultResources();
    if (!shader.parse(resources, defaultVersion, false, EShMsgDefault)) {
        std::string err = shader.getInfoLog();
        *errorMsg = strdup(err.c_str());
        return 1;
    }

    // Link shader
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(EShMsgDefault)) {
        std::string err = program.getInfoLog();
        *errorMsg = strdup(err.c_str());
        return 2;
    }

    // Convert to SPIRV
    std::vector<unsigned int> spirv;
    glslang::GlslangToSpv(*program.getIntermediate(stage), spirv);

    // Copy SPIRV data to output buffer
    *spirvSize = spirv.size() * sizeof(unsigned int);
    *spirvData = (char*)malloc(*spirvSize);
    memcpy(*spirvData, spirv.data(), *spirvSize);

    return 0;
}
