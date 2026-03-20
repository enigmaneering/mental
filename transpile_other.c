/*
 * Mental - HLSL and WGSL to SPIRV compilation
 *
 * Uses external compilers:
 * - HLSL: Microsoft DXC compiler
 * - WGSL: Naga from the wgpu project
 */

#include "transpile.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#define mkdtemp(template) _mktemp(template)
#endif

/* Helper to find DXC compiler */
static const char* find_dxc(void) {
    /* Platform-specific executable name */
#ifdef _WIN32
    #define DXC_EXE "dxc.exe"
#else
    #define DXC_EXE "dxc"
#endif

    /* Try relative paths (from build dir, build/test dir, and project root) */
    /* Use F_OK (0) instead of X_OK since Windows doesn't have execute bits */
    /* Windows redistributable has flat structure, Unix has bin/ subdirectory */
    if (access("../external/dxc/" DXC_EXE, 0) == 0) return "../external/dxc/" DXC_EXE;
    if (access("../../external/dxc/" DXC_EXE, 0) == 0) return "../../external/dxc/" DXC_EXE;
    if (access("external/dxc/" DXC_EXE, 0) == 0) return "external/dxc/" DXC_EXE;
    if (access("../external/dxc/bin/" DXC_EXE, 0) == 0) return "../external/dxc/bin/" DXC_EXE;
    if (access("../../external/dxc/bin/" DXC_EXE, 0) == 0) return "../../external/dxc/bin/" DXC_EXE;
    if (access("external/dxc/bin/" DXC_EXE, 0) == 0) return "external/dxc/bin/" DXC_EXE;

    /* Fall back to system PATH */
    return DXC_EXE;
#undef DXC_EXE
}

/* Helper to find Naga compiler */
static const char* find_naga(void) {
    /* Platform-specific executable name */
#ifdef _WIN32
    #define NAGA_EXE "naga.exe"
#else
    #define NAGA_EXE "naga"
#endif

    /* Try relative paths (from build dir, build/test dir, and project root) */
    /* Use F_OK (0) instead of X_OK since Windows doesn't have execute bits */
    if (access("../external/naga/bin/" NAGA_EXE, 0) == 0) return "../external/naga/bin/" NAGA_EXE;
    if (access("../../external/naga/bin/" NAGA_EXE, 0) == 0) return "../../external/naga/bin/" NAGA_EXE;
    if (access("external/naga/bin/" NAGA_EXE, 0) == 0) return "external/naga/bin/" NAGA_EXE;

    /* Fall back to system PATH */
    return NAGA_EXE;
#undef NAGA_EXE
}

/* Helper to read file into buffer */
static unsigned char* read_file(const char* path, size_t* out_len) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return NULL;
    }

    unsigned char* data = malloc(size);
    if (!data) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(data, 1, size, f);
    fclose(f);

    if (read != (size_t)size) {
        free(data);
        return NULL;
    }

    *out_len = size;
    return data;
}

unsigned char* mental_hlsl_to_spirv(const char* source, size_t source_len,
                                    size_t* out_len, char* error, size_t error_len) {
    const char* dxc = find_dxc();

    /* Create temporary directory */
    char tmpdir[] = "/tmp/mental_hlsl_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        if (error) strncpy(error, "Failed to create temporary directory", error_len - 1);
        return NULL;
    }

    /* Write HLSL source to temp file */
    char src_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/shader.hlsl", tmpdir);
    FILE* f = fopen(src_path, "wb");
    if (!f) {
        if (error) strncpy(error, "Failed to write HLSL source", error_len - 1);
        rmdir(tmpdir);
        return NULL;
    }
    fwrite(source, 1, source_len, f);
    fclose(f);

    /* Compile HLSL to SPIRV using DXC */
    char cmd[4096];
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/shader.spv", tmpdir);
    snprintf(cmd, sizeof(cmd),
             "%s -spirv -T cs_6_0 -E main -Fo %s %s 2>&1",
             dxc, out_path, src_path);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        if (error) strncpy(error, "Failed to execute DXC compiler (is DXC installed?)", error_len - 1);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    char output_buf[4096] = {0};
    fread(output_buf, 1, sizeof(output_buf) - 1, pipe);
    int status = pclose(pipe);

    if (status != 0) {
        if (error) snprintf(error, error_len, "DXC compilation failed: %s", output_buf);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    /* Read compiled SPIRV */
    unsigned char* spirv = read_file(out_path, out_len);

    /* Cleanup */
    remove(src_path);
    remove(out_path);
    rmdir(tmpdir);

    if (!spirv && error) {
        strncpy(error, "Failed to read SPIRV output", error_len - 1);
    }

    return spirv;
}

unsigned char* mental_wgsl_to_spirv(const char* source, size_t source_len,
                                    size_t* out_len, char* error, size_t error_len) {
    const char* naga = find_naga();

    /* Create temporary directory */
    char tmpdir[] = "/tmp/mental_wgsl_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        if (error) strncpy(error, "Failed to create temporary directory", error_len - 1);
        return NULL;
    }

    /* Write WGSL source to temp file */
    char src_path[1024];
    snprintf(src_path, sizeof(src_path), "%s/shader.wgsl", tmpdir);
    FILE* f = fopen(src_path, "wb");
    if (!f) {
        if (error) strncpy(error, "Failed to write WGSL source", error_len - 1);
        rmdir(tmpdir);
        return NULL;
    }
    fwrite(source, 1, source_len, f);
    fclose(f);

    /* Compile WGSL to SPIRV using Naga */
    char cmd[4096];
    char out_path[1024];
    snprintf(out_path, sizeof(out_path), "%s/shader.spv", tmpdir);
    snprintf(cmd, sizeof(cmd),
             "%s %s %s 2>&1",
             naga, src_path, out_path);

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        if (error) strncpy(error, "Failed to execute Naga compiler (is Naga installed?)", error_len - 1);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    char output_buf[4096] = {0};
    fread(output_buf, 1, sizeof(output_buf) - 1, pipe);
    int status = pclose(pipe);

    if (status != 0) {
        if (error) snprintf(error, error_len, "Naga compilation failed: %s", output_buf);
        remove(src_path);
        rmdir(tmpdir);
        return NULL;
    }

    /* Read compiled SPIRV */
    unsigned char* spirv = read_file(out_path, out_len);

    /* Cleanup */
    remove(src_path);
    remove(out_path);
    rmdir(tmpdir);

    if (!spirv && error) {
        strncpy(error, "Failed to read SPIRV output", error_len - 1);
    }

    return spirv;
}
