/*
 * transpile_naga_direct.c — Direct Naga FFI integration for Emscripten/WASM
 *
 * On native platforms, mental invokes Naga as a subprocess via posix_spawn().
 * On WASM, posix_spawn doesn't exist, so we link libnaga_ffi.a directly
 * and call the Naga C FFI to convert WGSL ↔ SPIRV in-process.
 *
 * This file is only compiled when EMSCRIPTEN is defined (see CMakeLists.txt).
 */

#ifdef __EMSCRIPTEN__
#ifdef MENTAL_HAS_NAGA_DIRECT

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* Naga C FFI — provided by libnaga_ffi.a */
extern int naga_wgsl_to_spirv(const uint8_t *wgsl_source, uint32_t wgsl_len,
                               uint8_t **spirv_out, uint32_t *spirv_len);
extern int naga_spirv_to_wgsl(const uint8_t *spirv_data, uint32_t spirv_len,
                               uint8_t **wgsl_out, uint32_t *wgsl_len);
extern void naga_free(uint8_t *ptr, uint32_t len);

unsigned char* mental_wgsl_to_spirv_direct(const char* source, size_t source_len,
                                            size_t* out_len, char* error, size_t error_len) {
    uint8_t *spirv_data = NULL;
    uint32_t spirv_len = 0;

    *out_len = 0;

    int rc = naga_wgsl_to_spirv((const uint8_t*)source, (uint32_t)source_len,
                                 &spirv_data, &spirv_len);
    if (rc != 0) {
        /* On failure, spirv_data contains the error message */
        if (error && spirv_data && spirv_len > 0) {
            size_t copy_len = spirv_len < error_len - 1 ? spirv_len : error_len - 1;
            memcpy(error, spirv_data, copy_len);
            error[copy_len] = '\0';
        } else if (error) {
            snprintf(error, error_len, "Naga: WGSL to SPIRV conversion failed");
        }
        if (spirv_data) naga_free(spirv_data, spirv_len);
        return NULL;
    }

    /* Copy to malloc'd buffer (caller expects to free with free(), not naga_free()) */
    unsigned char* output = (unsigned char*)malloc(spirv_len);
    if (!output) {
        if (error) snprintf(error, error_len, "Naga: Failed to allocate output buffer");
        naga_free(spirv_data, spirv_len);
        return NULL;
    }
    memcpy(output, spirv_data, spirv_len);
    *out_len = spirv_len;

    naga_free(spirv_data, spirv_len);
    return output;
}

char* mental_spirv_to_wgsl_direct(const unsigned char* spirv, size_t spirv_len,
                                   size_t* out_len, char* error, size_t error_len) {
    uint8_t *wgsl_data = NULL;
    uint32_t wgsl_len = 0;

    *out_len = 0;

    int rc = naga_spirv_to_wgsl(spirv, (uint32_t)spirv_len, &wgsl_data, &wgsl_len);
    if (rc != 0) {
        if (error && wgsl_data && wgsl_len > 0) {
            size_t copy_len = wgsl_len < error_len - 1 ? wgsl_len : error_len - 1;
            memcpy(error, wgsl_data, copy_len);
            error[copy_len] = '\0';
        } else if (error) {
            snprintf(error, error_len, "Naga: SPIRV to WGSL conversion failed");
        }
        if (wgsl_data) naga_free(wgsl_data, wgsl_len);
        return NULL;
    }

    /* Copy to malloc'd buffer — add null terminator for string use */
    char* output = (char*)malloc(wgsl_len + 1);
    if (!output) {
        if (error) snprintf(error, error_len, "Naga: Failed to allocate output buffer");
        naga_free(wgsl_data, wgsl_len);
        return NULL;
    }
    memcpy(output, wgsl_data, wgsl_len);
    output[wgsl_len] = '\0';
    *out_len = wgsl_len;

    naga_free(wgsl_data, wgsl_len);
    return output;
}

#endif /* MENTAL_HAS_NAGA_DIRECT */
#endif /* __EMSCRIPTEN__ */
