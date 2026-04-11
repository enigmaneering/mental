/*
 * transpile_dxc_direct.cpp — Direct DXC integration for Emscripten/WASM
 *
 * On native platforms, mental invokes DXC as a subprocess via posix_spawn().
 * On WASM, posix_spawn doesn't exist, so we call DXC's dxc::main() directly
 * with synthetic argc/argv, using Emscripten's in-memory filesystem for I/O.
 *
 * This bypasses the COM API entirely (which crashes under WASM due to vtable
 * and global allocator issues) and instead uses the same code path as the
 * DXC command-line executable.
 *
 * This file is only compiled when EMSCRIPTEN is defined (see CMakeLists.txt).
 */

#ifdef __EMSCRIPTEN__
#ifdef MENTAL_HAS_DXC_DIRECT

#include <cstdlib>
#include <cstring>
#include <cstdio>

/* DXC's command-line entry point — declared in dxclib/dxc.h */
namespace dxc {
    int main(int argc, const char **argv);
}

extern "C" {

unsigned char* mental_hlsl_to_spirv_direct(const char* source, size_t source_len,
                                            size_t* out_len, char* error, size_t error_len) {
    *out_len = 0;

    /* Write HLSL source to Emscripten's in-memory filesystem */
    const char* src_path = "/tmp/mental_hlsl_input.hlsl";
    const char* out_path = "/tmp/mental_hlsl_output.spv";

    FILE* f = fopen(src_path, "wb");
    if (!f) {
        if (error) snprintf(error, error_len, "DXC: Failed to write HLSL to virtual filesystem");
        return nullptr;
    }
    fwrite(source, 1, source_len, f);
    fclose(f);

    /* Remove any stale output */
    remove(out_path);

    /* Invoke DXC's main() with command-line arguments:
     * dxc -spirv -T cs_6_0 -E main -Fo <output> <input> */
    const char* argv[] = {
        "dxc",
        "-spirv",
        "-T", "cs_6_0",
        "-E", "main",
        "-Fo", out_path,
        src_path
    };
    int argc = sizeof(argv) / sizeof(argv[0]);

    int rc = dxc::main(argc, argv);

    /* Clean up input */
    remove(src_path);

    if (rc != 0) {
        if (error) snprintf(error, error_len, "DXC: Compilation failed (exit code %d)", rc);
        remove(out_path);
        return nullptr;
    }

    /* Read compiled SPIRV from virtual filesystem */
    FILE* spv = fopen(out_path, "rb");
    if (!spv) {
        if (error) snprintf(error, error_len, "DXC: No SPIRV output produced");
        return nullptr;
    }

    fseek(spv, 0, SEEK_END);
    long spv_size = ftell(spv);
    fseek(spv, 0, SEEK_SET);

    if (spv_size <= 0) {
        if (error) snprintf(error, error_len, "DXC: SPIRV output is empty");
        fclose(spv);
        remove(out_path);
        return nullptr;
    }

    unsigned char* output = (unsigned char*)malloc(spv_size);
    if (!output) {
        if (error) snprintf(error, error_len, "DXC: Failed to allocate output buffer");
        fclose(spv);
        remove(out_path);
        return nullptr;
    }

    fread(output, 1, spv_size, spv);
    fclose(spv);
    remove(out_path);

    *out_len = (size_t)spv_size;
    return output;
}

}

#endif /* MENTAL_HAS_DXC_DIRECT */
#endif /* __EMSCRIPTEN__ */
