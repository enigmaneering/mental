/*
 * transpile_dxc_direct.cpp — Direct DXC API integration for Emscripten/WASM
 *
 * On native platforms, mental invokes DXC as a subprocess via posix_spawn().
 * On WASM, posix_spawn doesn't exist, so we link libdxcompiler.a directly
 * and call the DXC COM API to compile HLSL to SPIRV in-process.
 *
 * This file is only compiled when EMSCRIPTEN is defined (see CMakeLists.txt).
 */

#ifdef __EMSCRIPTEN__
#ifdef MENTAL_HAS_DXC_DIRECT

#include <cstdlib>
#include <cstring>
#include <cstdio>

/* DXC's public API header — COM-style interfaces */
#include "dxc/dxcapi.h"

extern "C" {

/* Forward declaration — matches the signature in transpile_other.c */
unsigned char* mental_hlsl_to_spirv_direct(const char* source, size_t source_len,
                                            size_t* out_len, char* error, size_t error_len);

}

/* CLSID_DxcCompiler and CLSID_DxcUtils are defined in dxcapi.h */

/* Helper to convert narrow string to wide string (DXC API uses wchar_t) */
static wchar_t* to_wide(const char* str) {
    if (!str) return nullptr;
    size_t len = strlen(str) + 1;
    wchar_t* wide = (wchar_t*)malloc(len * sizeof(wchar_t));
    for (size_t i = 0; i < len; i++) {
        wide[i] = (wchar_t)str[i];
    }
    return wide;
}

extern "C"
unsigned char* mental_hlsl_to_spirv_direct(const char* source, size_t source_len,
                                            size_t* out_len, char* error, size_t error_len) {
    IDxcCompiler3* compiler = nullptr;
    IDxcUtils* utils = nullptr;
    IDxcResult* result = nullptr;
    unsigned char* output = nullptr;

    *out_len = 0;

    /* DXC's library initialization normally runs via __attribute__((constructor))
     * in DXCompiler.cpp's DllMain(). When linked as a static library under
     * Emscripten, this constructor may not fire — initialize manually.
     * Both functions are declared in dxc/Support/Global.h. */
    static bool dxc_initialized = false;
    if (!dxc_initialized) {
        extern HRESULT DxcInitThreadMalloc() throw();
        extern void DxcSetThreadMallocToDefault() throw();
        HRESULT initHr = DxcInitThreadMalloc();
        if (FAILED(initHr)) {
            if (error) snprintf(error, error_len, "DXC: DxcInitThreadMalloc failed (0x%08x)", (unsigned)initHr);
            return nullptr;
        }
        DxcSetThreadMallocToDefault();
        dxc_initialized = true;
    }

    /* Create compiler and utils instances */
    HRESULT hr = DxcCreateInstance(CLSID_DxcCompiler, __uuidof(IDxcCompiler3), (void**)&compiler);
    if (FAILED(hr)) {
        if (error) snprintf(error, error_len, "DXC: Failed to create compiler instance (0x%08x)", (unsigned)hr);
        goto cleanup;
    }

    hr = DxcCreateInstance(CLSID_DxcUtils, __uuidof(IDxcUtils), (void**)&utils);
    if (FAILED(hr)) {
        if (error) snprintf(error, error_len, "DXC: Failed to create utils instance (0x%08x)", (unsigned)hr);
        goto cleanup;
    }

    {
        /* Set up source buffer */
        DxcBuffer sourceBuffer;
        sourceBuffer.Ptr = source;
        sourceBuffer.Size = source_len;
        sourceBuffer.Encoding = DXC_CP_UTF8;

        /* Compiler arguments: output SPIRV, compute shader 6.0, entry point "main" */
        const wchar_t* args[] = {
            L"-spirv",
            L"-T", L"cs_6_0",
            L"-E", L"main",
        };

        /* Compile */
        hr = compiler->Compile(&sourceBuffer, args, sizeof(args) / sizeof(args[0]),
                               nullptr, __uuidof(IDxcResult), (void**)&result);
        if (FAILED(hr)) {
            if (error) snprintf(error, error_len, "DXC: Compile call failed (0x%08x)", (unsigned)hr);
            goto cleanup;
        }

        /* Check compilation status */
        HRESULT status;
        result->GetStatus(&status);

        if (FAILED(status)) {
            /* Get error message */
            IDxcBlobEncoding* errBlob = nullptr;
            result->GetErrorBuffer(&errBlob);
            if (errBlob && errBlob->GetBufferSize() > 0) {
                if (error) {
                    size_t copyLen = errBlob->GetBufferSize();
                    if (copyLen >= error_len) copyLen = error_len - 1;
                    memcpy(error, errBlob->GetBufferPointer(), copyLen);
                    error[copyLen] = '\0';
                }
                errBlob->Release();
            } else {
                if (error) snprintf(error, error_len, "DXC: HLSL compilation failed (0x%08x)", (unsigned)status);
            }
            goto cleanup;
        }

        /* Get compiled SPIRV binary */
        IDxcBlob* spirvBlob = nullptr;
        hr = result->GetOutput(DXC_OUT_OBJECT, __uuidof(IDxcBlob), (void**)&spirvBlob, nullptr);
        if (FAILED(hr) || !spirvBlob || spirvBlob->GetBufferSize() == 0) {
            if (error) snprintf(error, error_len, "DXC: No SPIRV output produced");
            if (spirvBlob) spirvBlob->Release();
            goto cleanup;
        }

        /* Copy output (caller owns the memory) */
        *out_len = spirvBlob->GetBufferSize();
        output = (unsigned char*)malloc(*out_len);
        if (output) {
            memcpy(output, spirvBlob->GetBufferPointer(), *out_len);
        } else {
            if (error) snprintf(error, error_len, "DXC: Failed to allocate output buffer");
            *out_len = 0;
        }

        spirvBlob->Release();
    }

cleanup:
    if (result) result->Release();
    if (utils) utils->Release();
    if (compiler) compiler->Release();
    return output;
}

#endif /* MENTAL_HAS_DXC_DIRECT */
#endif /* __EMSCRIPTEN__ */
