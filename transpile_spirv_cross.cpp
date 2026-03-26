/*
 * Mental - SPIRV transpilation (using SPIRV-Cross)
 */

#include "transpile.h"
#include <spirv_cross.hpp>
#include <spirv_glsl.hpp>
#include <spirv_hlsl.hpp>
#include <spirv_msl.hpp>
#include <cstring>
#include <vector>

extern "C" {

static char* copy_string(const std::string& str, size_t* out_len) {
    size_t len = str.length();
    char* result = (char*)malloc(len + 1);
    if (!result) return nullptr;

    memcpy(result, str.c_str(), len);
    result[len] = '\0';
    *out_len = len;

    return result;
}

char* mental_spirv_to_glsl(const unsigned char* spirv, size_t spirv_len,
                           size_t* out_len, char* error, size_t error_len) {
    try {
        const uint32_t* spirv_data = reinterpret_cast<const uint32_t*>(spirv);
        size_t spirv_word_count = spirv_len / sizeof(uint32_t);

        spirv_cross::CompilerGLSL glsl(spirv_data, spirv_word_count);

        /* Configure for Vulkan compute shaders */
        spirv_cross::CompilerGLSL::Options options;
        options.version = 450;
        options.es = false;
        options.vulkan_semantics = true;
        glsl.set_common_options(options);

        std::string result = glsl.compile();
        return copy_string(result, out_len);

    } catch (const std::exception& e) {
        if (error) {
            strncpy(error, e.what(), error_len - 1);
            error[error_len - 1] = '\0';
        }
        return nullptr;
    }
}

char* mental_spirv_to_hlsl(const unsigned char* spirv, size_t spirv_len,
                           size_t* out_len, char* error, size_t error_len) {
    try {
        const uint32_t* spirv_data = reinterpret_cast<const uint32_t*>(spirv);
        size_t spirv_word_count = spirv_len / sizeof(uint32_t);

        spirv_cross::CompilerHLSL hlsl(spirv_data, spirv_word_count);

        /* Configure for compute shaders */
        spirv_cross::CompilerHLSL::Options options;
        options.shader_model = 50; /* Shader Model 5.0 */
        hlsl.set_hlsl_options(options);

        std::string result = hlsl.compile();
        return copy_string(result, out_len);

    } catch (const std::exception& e) {
        if (error) {
            strncpy(error, e.what(), error_len - 1);
            error[error_len - 1] = '\0';
        }
        return nullptr;
    }
}

char* mental_spirv_to_msl(const unsigned char* spirv, size_t spirv_len,
                          size_t* out_len, char* error, size_t error_len) {
    try {
        const uint32_t* spirv_data = reinterpret_cast<const uint32_t*>(spirv);
        size_t spirv_word_count = spirv_len / sizeof(uint32_t);

        spirv_cross::CompilerMSL msl(spirv_data, spirv_word_count);

        /* Configure for Metal compute shaders */
        spirv_cross::CompilerMSL::Options options;
        options.platform = spirv_cross::CompilerMSL::Options::macOS;
        msl.set_msl_options(options);

        /* Set up resource bindings (buffer indices) */
        /* Preserve SPIRV binding numbers - don't reorder! */
        spirv_cross::ShaderResources resources = msl.get_shader_resources();

        /* Bind storage buffers - keep original binding numbers */
        for (auto& resource : resources.storage_buffers) {
            spirv_cross::MSLResourceBinding binding;
            binding.stage = spv::ExecutionModelGLCompute;
            binding.desc_set = msl.get_decoration(resource.id, spv::DecorationDescriptorSet);
            binding.binding = msl.get_decoration(resource.id, spv::DecorationBinding);
            /* Use the SPIRV binding number as the Metal buffer index */
            binding.msl_buffer = binding.binding;
            msl.add_msl_resource_binding(binding);
        }

        std::string result = msl.compile();
        return copy_string(result, out_len);

    } catch (const std::exception& e) {
        if (error) {
            strncpy(error, e.what(), error_len - 1);
            error[error_len - 1] = '\0';
        }
        return nullptr;
    }
}

/* mental_spirv_to_wgsl is implemented in transpile_other.c using Naga,
 * since spirv-cross does not have a WGSL backend. */

} /* extern "C" */
