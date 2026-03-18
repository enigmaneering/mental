
#include "spirv_cross_wrapper.h"
#include <spirv_cross.hpp>
#include <spirv_glsl.hpp>
#include <spirv_hlsl.hpp>
#include <spirv_msl.hpp>
#include <string>
#include <vector>
#include <cstring>

int transpile_spirv(
    const char* spirvData,
    int spirvSize,
    TargetLanguage target,
    char** outputSource,
    char** errorMsg
) {
    try {
        // Convert SPIRV data to vector of uint32_t
        const uint32_t* spirvWords = reinterpret_cast<const uint32_t*>(spirvData);
        size_t spirvWordCount = spirvSize / sizeof(uint32_t);
        std::vector<uint32_t> spirv(spirvWords, spirvWords + spirvWordCount);

        std::string result;

        switch (target) {
            case TARGET_GLSL: {
                spirv_cross::CompilerGLSL compiler(spirv);
                spirv_cross::CompilerGLSL::Options options;
                options.version = 450;
                options.es = false;
                compiler.set_common_options(options);
                result = compiler.compile();
                break;
            }
            case TARGET_HLSL: {
                spirv_cross::CompilerHLSL compiler(spirv);
                spirv_cross::CompilerHLSL::Options options;
                options.shader_model = 50;
                compiler.set_hlsl_options(options);
                result = compiler.compile();
                break;
            }
            case TARGET_MSL: {
                spirv_cross::CompilerMSL compiler(spirv);
                spirv_cross::CompilerMSL::Options options;
                options.platform = spirv_cross::CompilerMSL::Options::macOS;
                compiler.set_msl_options(options);

                // Ensure buffers use their SPIRV binding indices as MSL buffer indices
                // This preserves the original buffer order from GLSL/HLSL
                spirv_cross::ShaderResources resources = compiler.get_shader_resources();
                for (auto& resource : resources.storage_buffers) {
                    uint32_t binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
                    // Use MSL resource binding to map SPIRV binding to Metal buffer index
                    spirv_cross::MSLResourceBinding msl_binding;
                    msl_binding.stage = spv::ExecutionModelGLCompute;
                    msl_binding.desc_set = compiler.get_decoration(resource.id, spv::DecorationDescriptorSet);
                    msl_binding.binding = binding;
                    msl_binding.msl_buffer = binding;  // Map to same buffer index in Metal
                    compiler.add_msl_resource_binding(msl_binding);
                }

                result = compiler.compile();
                break;
            }
            default:
                *errorMsg = strdup("Unknown target language");
                return 1;
        }

        *outputSource = strdup(result.c_str());
        return 0;

    } catch (const std::exception& e) {
        *errorMsg = strdup(e.what());
        return 2;
    }
}
